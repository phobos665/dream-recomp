#include <vector>

#include "dream/runtime/mem/dc_memory.h"

#include "doctest.h"

using dream::mem::DcMemory;
using dream::mem::MmioHandler;

namespace {
struct Recorder final : MmioHandler {
    struct Access {
        std::uint32_t addr, value;
        unsigned size;
        bool write;
    };
    std::vector<Access> log;
    std::vector<std::uint32_t> burst;
    std::uint32_t burst_addr = 0;
    std::uint32_t read(std::uint32_t addr, unsigned size) override {
        log.push_back({addr, 0, size, false});
        return 0xA5000000u | (addr & 0xFFFFu);
    }
    void write(std::uint32_t addr, std::uint32_t value, unsigned size) override {
        log.push_back({addr, value, size, true});
    }
    void write_burst(std::uint32_t addr, const std::uint32_t* words) override {
        burst_addr = addr;
        burst.assign(words, words + 8);
    }
};
}  // namespace

TEST_CASE("memory: main RAM is reachable through every alias and mirror") {
    DcMemory m;
    m.write32(0x8C001000u, 0x11223344u);
    CHECK(m.read32(0x0C001000u) == 0x11223344u);  // P0 physical
    CHECK(m.read32(0xAC001000u) == 0x11223344u);  // P2 uncached
    CHECK(m.read32(0xCC001000u) == 0x11223344u);  // P3
    CHECK(m.read32(0x0D001000u) == 0x11223344u);  // 16 MB mirror inside the 64 MB area
    CHECK(m.read8(0x8C001000u) == 0x44u);         // little-endian
    CHECK(m.read16(0x8C001002u) == 0x1122u);
    m.write64(0x8C002000u, 0x0102030405060708ull);
    CHECK(m.read32(0x8C002000u) == 0x05060708u);
    CHECK(m.read32(0x8C002004u) == 0x01020304u);
    CHECK(m.read64(0x8C002000u) == 0x0102030405060708ull);
    CHECK(m.ram()[0x1000] == 0x44u);
    CHECK(m.faults().total() == 0);
}

TEST_CASE("memory: the two VRAM views interleave banks word by word") {
    DcMemory m;
    // Fill the 64-bit view linearly: word k at byte 4k.
    for (std::uint32_t k = 0; k < 8; ++k) m.write32(0xA4000000u + 4 * k, 0x1000u + k);
    for (std::uint32_t k = 0; k < 8; ++k)
        m.write32(0xA4400000u + 4 * k, 0x2000u + k);  // second half
    // 32-bit view, bank 0 (first 4 MB): word j reads 64-bit slot 2j.
    CHECK(m.read32(0xA5000000u) == 0x1000u);
    CHECK(m.read32(0xA5000004u) == 0x1002u);
    CHECK(m.read32(0xA5000008u) == 0x1004u);
    // 32-bit view, bank 1 (offset 0x400000): word j reads 64-bit slot 2j + 1.
    CHECK(m.read32(0xA5400000u) == 0x1001u);
    CHECK(m.read32(0xA5400004u) == 0x1003u);
    // Mirrors: 0x06000000 is the 64-bit view again, 0x07000000 the 32-bit one; P1 aliases too.
    CHECK(m.read32(0x86000004u) == 0x1001u);
    CHECK(m.read32(0x87400000u) == 0x1001u);
    CHECK(DcMemory::vram_map32(0) == 0);
    CHECK(DcMemory::vram_map32(4) == 8);
    CHECK(DcMemory::vram_map32(0x400000) == 4);
    CHECK(DcMemory::vram_map32(0x400004) == 12);
    CHECK(DcMemory::vram_map32(0x3FFFFC) == 0x7FFFF8);
    CHECK(DcMemory::vram_map32(0x7FFFFC) == 0x7FFFFC);
    CHECK(m.faults().total() == 0);
}

TEST_CASE("memory: sound RAM, flash, boot ROM and on-chip RAM") {
    DcMemory m;
    m.write16(0xA0800100u, 0xBEEFu);
    CHECK(m.read16(0x00800100u) == 0xBEEFu);
    CHECK(m.read16(0x00A00100u) == 0xBEEFu);  // 2 MB mirror within the 8 MB window
    CHECK(m.aram()[0x100] == 0xEFu);
    m.flash()[0x1A000] = 0x42;
    CHECK(m.read8(0xA021A000u) == 0x42u);
    CHECK(m.read8(0xA023A000u) == 0x42u);  // 128 KB mirror
    m.bios()[0x100] = 0x99;
    CHECK(m.read8(0xA0000100u) == 0x99u);
    // OC RAM: two 4 KB halves selected by bit 13, mirrored over the 64 MB window.
    m.write32(0x7C000000u, 1);
    m.write32(0x7C002000u, 2);
    CHECK(m.read32(0x7C000000u) == 1u);
    CHECK(m.read32(0x7C002000u) == 2u);
    CHECK(m.read32(0x7C004000u) == 1u);
    CHECK(m.read32(0x7D000000u) == 1u);
    CHECK(m.faults().total() == 0);
}

TEST_CASE("memory: store queues flush to the area QACR selects") {
    DcMemory m;
    m.write32(0xFF000038u, 3u << 2);  // QACR0: area 3 (RAM)
    m.write32(0xFF00003Cu, 4u << 2);  // QACR1: area 4 (TA FIFO)
    CHECK(m.read32(0xFF000038u) == (3u << 2));
    for (std::uint32_t i = 0; i < 8; ++i) m.sq_write32(0xE0000000u + 4 * i, 0x100u + i);  // SQ0
    for (std::uint32_t i = 0; i < 8; ++i) m.sq_write32(0xE0000020u + 4 * i, 0x200u + i);  // SQ1
    // The SQ contents are readable back through the P4 window.
    CHECK(m.read32(0xE0000004u) == 0x101u);
    CHECK(m.read32(0xE0000024u) == 0x201u);
    m.sq_flush(0xE0100040u);  // SQ0 -> 0x0C100040
    for (std::uint32_t i = 0; i < 8; ++i) CHECK(m.read32(0x8C100040u + 4 * i) == 0x100u + i);
    Recorder ta;
    m.map_mmio(0x10000000u, 0x10800000u, &ta);
    // Bit 5 both selects SQ1 and stays part of the destination address (SH7750 manual 4.6).
    m.sq_flush(0xE0000020u | 0x00000100u);  // SQ1 -> 0x10000120 (burst to the handler)
    CHECK(ta.burst_addr == 0x10000120u);
    REQUIRE(ta.burst.size() == 8);
    CHECK(ta.burst[0] == 0x200u);
    CHECK(ta.burst[7] == 0x207u);
    CHECK(m.faults().total() == 0);
}

TEST_CASE("memory: MMIO dispatch hands physical addresses and sizes to the device") {
    DcMemory m;
    Recorder holly, tmu;
    m.map_mmio(0x005F6800u, 0x005FA000u, &holly);
    m.map_p4(0xFFD80000u, 0xFFD80030u, &tmu);
    CHECK(m.read32(0xA05F8004u) == 0xA5008004u);    // PVR ID register, via P2
    m.write8(0x805F6C08u, 0x7F);                    // Maple, via P1, byte access
    m.write16(0xA05F7018u, 0x1234);                 // GD-ROM, halfword
    m.write64(0xA05F8100u, 0x0000000200000001ull);  // split into two words
    REQUIRE(holly.log.size() == 5);
    CHECK(holly.log[0].addr == 0x005F8004u);
    CHECK(holly.log[0].size == 4);
    CHECK(holly.log[1].addr == 0x005F6C08u);
    CHECK(holly.log[1].size == 1);
    CHECK(holly.log[1].value == 0x7Fu);
    CHECK(holly.log[2].size == 2);
    CHECK(holly.log[3].addr == 0x005F8100u);
    CHECK(holly.log[3].value == 1u);
    CHECK(holly.log[4].addr == 0x005F8104u);
    CHECK(holly.log[4].value == 2u);
    m.write32(0xFFD80008u, 0xFFFFFFFFu);  // TMU TCOR0
    REQUIRE(tmu.log.size() == 1);
    CHECK(tmu.log[0].addr == 0xFFD80008u);
    CHECK(m.faults().total() == 0);
}

TEST_CASE("memory: unhandled accesses are logged, read as zero and never abort") {
    DcMemory m;
    CHECK(m.read32(0xA0400000u) == 0u);  // area 0 hole
    CHECK(m.read32(0xA0400000u) == 0u);
    m.write32(0x88000000u, 1);           // area 2
    m.write8(0xA05F7000u, 1);            // GD-ROM with no device mapped yet
    CHECK(m.read32(0xFFD80000u) == 0u);  // TMU with no device mapped yet
    CHECK(m.faults().total() == 5);
    const auto recs = m.faults().records();
    REQUIRE(recs.size() == 4);
    CHECK(recs[0].addr == 0x88000000u);
    CHECK(recs[0].write);
    CHECK(recs[1].addr == 0xA0400000u);
    CHECK(recs[1].count == 2);
    CHECK(m.faults().format().find("0xa0400000 r4 x2") != std::string::npos);
    m.faults().clear();
    CHECK(m.faults().total() == 0);
}

TEST_CASE("memory: the write journal puts memory back exactly as it was") {
    // The replay harness runs a guest function twice from the same memory, so undoing its stores
    // has to be exact, including a location written more than once and a location whose new value
    // happens to equal its old one.
    DcMemory m;
    const std::uint32_t ram = 0x8C010000u;
    m.write32(ram, 0x11111111u);
    m.write32(ram + 4, 0x22222222u);
    m.write16(ram + 8, 0x3333u);

    m.journaling = true;
    m.write32(ram, 0xAAAAAAAAu);
    m.write32(ram, 0xBBBBBBBBu);      // the same location twice
    m.write32(ram + 4, 0x22222222u);  // written with the value it already held
    m.write8(ram + 8, 0x44u);
    m.journaling = false;
    CHECK(m.journal.size() == 4);
    CHECK(m.read32(ram) == 0xBBBBBBBBu);

    const auto written = m.journal;  // kept, the way the harness keeps the translated run's stores
    m.undo_journal();
    CHECK(m.read32(ram) == 0x11111111u);  // back to the first value, not the second
    CHECK(m.read32(ram + 4) == 0x22222222u);
    CHECK(m.read16(ram + 8) == 0x3333u);
    CHECK(m.journal.empty());

    // And putting them back reproduces the run exactly.
    m.journal = written;
    m.redo_journal();
    CHECK(m.read32(ram) == 0xBBBBBBBBu);
    CHECK(m.read8(ram + 8) == 0x44u);
}

TEST_CASE("memory: a device write marks the journal as one that cannot be undone") {
    // A store to a device register has effects outside memory: it cannot be put back and must not
    // be repeated, so the harness has to abandon the comparison rather than guess.
    struct Counter final : MmioHandler {
        std::uint32_t writes = 0;
        std::uint32_t read(std::uint32_t, unsigned) override { return 0; }
        void write(std::uint32_t, std::uint32_t, unsigned) override { ++writes; }
    } dev;
    DcMemory m;
    m.map_mmio(0x005F6C00u, 0x005F6D00u, &dev);

    m.journaling = true;
    m.write32(0x8C010000u, 1);
    CHECK_FALSE(m.journal_saw_device);
    m.write32(0xA05F6C00u, 1);
    CHECK(m.journal_saw_device);
    m.journaling = false;
    CHECK(dev.writes == 1);
    // The device write is not in the journal, so undoing cannot silently half-revert it.
    CHECK(m.journal.size() == 1);
}
