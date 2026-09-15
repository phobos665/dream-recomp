#include <cstring>
#include <string>

#include "dream/runtime/gdrom/disc.h"
#include "dream/runtime/hle/bios.h"
#include "dream/runtime/sh4/abi.h"
#include "dream/runtime/sh4/ops.h"

#include "doctest.h"

using namespace dream;
using hle::Bios;

namespace {
const char* kFixture = DREAM_DCDISC_FIXTURES "/synthetic.chd";
}

TEST_CASE("flash: the factory partition records region, language and broadcast in both copies") {
    // Pinned against Flycast's fixUpDCFlash() (core/hw/flashrom/nvmem.cpp), which writes
    // '0' + the setting at 0x1a002/3/4 and again at 0x1a0a2/3/4. This existed as a defect for a
    // while: format() wrote "00000Dreamcast  " unconditionally, claiming a Japanese console set to
    // Japanese, for USA discs. The offsets are the console's, not ours, so they are asserted
    // literally rather than through a helper that could drift with them.
    System sys;
    hle::Flash flash(sys.memory.flash());
    const std::uint8_t* f = sys.memory.flash();

    flash.format(hle::Language::English, hle::Region::Usa, hle::Broadcast::Ntsc);
    for (std::uint32_t base : {0x1A000u, 0x1A0A0u}) {
        CHECK(std::memcmp(f + base, "00110Dreamcast  ", 16) == 0);
    }

    flash.format(hle::Language::Japanese, hle::Region::Japan, hle::Broadcast::Ntsc);
    for (std::uint32_t base : {0x1A000u, 0x1A0A0u}) {
        CHECK(std::memcmp(f + base, "00000Dreamcast  ", 16) == 0);
    }

    flash.format(hle::Language::German, hle::Region::Europe, hle::Broadcast::Pal);
    for (std::uint32_t base : {0x1A000u, 0x1A0A0u}) {
        CHECK(std::memcmp(f + base, "00221Dreamcast  ", 16) == 0);
    }
}

TEST_CASE("flash: an unwritten partition reads as erased, not as zeros") {
    // Flash erases to all-ones, and the usual way a title asks whether a block was ever written is
    // to test it for 0xFF. format() used to zero the Reserved partition, so that test failed and
    // the title concluded the partition held real data.
    System sys;
    hle::Flash flash(sys.memory.flash());
    flash.format(hle::Language::English, hle::Region::Usa, hle::Broadcast::Ntsc);
    std::uint32_t off = 0, size = 0;
    REQUIRE(hle::Flash::partition(hle::Flash::Reserved, off, size));
    const std::uint8_t* f = sys.memory.flash();
    for (std::uint32_t i = 0; i < size; ++i) REQUIRE(f[off + i] == 0xFF);
}

TEST_CASE("flash: region names map as dcdisc writes them, and anything else falls back to USA") {
    // dcdisc writes the IP.BIN area symbol in full (tools/dcdisc/dcdisc/ipbin.py).
    CHECK(hle::region_from_name("Japan") == hle::Region::Japan);
    CHECK(hle::region_from_name("USA") == hle::Region::Usa);
    CHECK(hle::region_from_name("Europe") == hle::Region::Europe);
    CHECK(hle::region_from_name("") == hle::Region::Usa);
    CHECK(hle::region_from_name("Brazil") == hle::Region::Usa);
    CHECK(hle::broadcast_for(hle::Region::Europe) == hle::Broadcast::Pal);
    CHECK(hle::broadcast_for(hle::Region::Usa) == hle::Broadcast::Ntsc);
}

TEST_CASE("flash: formatted layout has partitions, a valid header and the system settings block") {
    System sys;
    hle::Flash flash(sys.memory.flash());
    flash.format(hle::Language::English);
    std::uint32_t off = 0, size = 0;
    REQUIRE(hle::Flash::partition(hle::Flash::User, off, size));
    CHECK(off == 0x1C000u);
    CHECK(size == 0x4000u);
    CHECK_EQ(std::memcmp(sys.memory.flash() + off, "KATANA_FLASH____", 16), 0);
    CHECK(sys.memory.flash()[off + 16] == 2);
    CHECK_EQ(std::memcmp(sys.memory.flash() + 0x1A005, "Dreamcast  ", 11), 0);
    std::uint8_t blk[60];
    REQUIRE(flash.read_block(hle::Flash::User, hle::Flash::kSyscfgBlock, blk));
    CHECK(blk[5] == 1);  // English
    CHECK(blk[7] == 1);  // autostart
    // The block carries a valid CRC where KallistiOS and the BIOS expect it.
    const std::uint8_t* phys = sys.memory.flash() + off + 64;  // first user block
    CHECK(static_cast<unsigned>(phys[0] | (phys[1] << 8)) == 5u);
    CHECK(static_cast<unsigned>(phys[62] | (phys[63] << 8)) == hle::Flash::crc(phys));
    // Rewriting a logical block keeps a single copy; a new id allocates the next physical block.
    blk[5] = 3;
    REQUIRE(flash.write_block(hle::Flash::User, hle::Flash::kSyscfgBlock, blk));
    std::uint8_t back[60];
    REQUIRE(flash.read_block(hle::Flash::User, hle::Flash::kSyscfgBlock, back));
    CHECK(back[5] == 3);
    REQUIRE(flash.write_block(hle::Flash::User, 0x80, blk));
    CHECK(static_cast<unsigned>(sys.memory.flash()[off + 128] |
                                (sys.memory.flash()[off + 129] << 8)) == 0x80u);
    CHECK(!flash.read_block(hle::Flash::User, 0x81, back));
    CHECK(!flash.read_block(hle::Flash::Factory, 5, back));  // not block-allocated
}

TEST_CASE("bios: vectors, SYSINFO, font and flash syscalls") {
    System sys;
    Bios bios(sys);
    bios.install();
    auto& m = sys.memory;
    auto& c = sys.ctx;
    CHECK(m.read32(Bios::kVecSystem) == Bios::kHookSystem);
    CHECK(m.read32(Bios::kVecGd) == Bios::kHookGd);
    CHECK(sh4::find_function(Bios::kHookGd) != nullptr);
    // SYSINFO_INIT then SYSINFO_ID through the vector, as a game's wrapper would call them.
    c.r[7] = 0;
    sh4::call_indirect(c, m, m.read32(Bios::kVecSystem));
    CHECK(c.r[0] == 0);
    c.r[7] = 3;
    sh4::call_indirect(c, m, m.read32(Bios::kVecSystem));
    CHECK(c.r[0] == Bios::kSysInfoBlock);
    CHECK(m.read8(Bios::kSysInfoBlock + 8) ==
          '0');  // system property bytes from the factory string
    // Font
    c.r[1] = 0;
    sh4::call_indirect(c, m, m.read32(Bios::kVecFont));
    CHECK(c.r[0] == 0xA0100020u);
    // FLASHROM_INFO partition 2, then READ the header magic through the syscall.
    c.r[7] = 0;
    c.r[4] = 2;
    c.r[5] = 0x8C100000;
    sh4::call_indirect(c, m, m.read32(Bios::kVecFlash));
    CHECK(c.r[0] == 0);
    CHECK(m.read32(0x8C100000) == 0x1C000u);
    CHECK(m.read32(0x8C100004) == 0x4000u);
    c.r[7] = 1;
    c.r[4] = 0x1C000;
    c.r[5] = 0x8C100100;
    c.r[6] = 16;
    sh4::call_indirect(c, m, m.read32(Bios::kVecFlash));
    CHECK(m.read8(0x8C100100) == 'K');
    CHECK(m.read8(0x8C10010F) == '_');
    c.r[7] = 0;
    c.r[4] = 7;  // no such partition
    sh4::call_indirect(c, m, m.read32(Bios::kVecFlash));
    CHECK(c.r[0] == 0xFFFFFFFFu);
    CHECK(bios.counts["sysinfo"] == 2);
    CHECK(bios.counts["flashrom"] == 3);
}

TEST_CASE("bios: a GD-ROM DMA read completes on the virtual clock and lands in RAM") {
    std::string err;
    auto disc = gdrom::open_disc(kFixture, err);
    REQUIRE_MESSAGE(disc, err);
    System sys;
    Bios bios(sys);
    bios.attach_disc(disc.get());
    bios.install();
    auto& m = sys.memory;
    auto& c = sys.ctx;
    auto gd = [&](std::uint32_t fn, std::uint32_t r4 = 0, std::uint32_t r5 = 0) {
        c.r[6] = 0;
        c.r[7] = fn;
        c.r[4] = r4;
        c.r[5] = r5;
        sh4::call_indirect(c, m, m.read32(Bios::kVecGd));
        return c.r[0];
    };
    CHECK(gd(3) == 0);  // INIT_SYSTEM
    // Drive status: paused, GD-ROM inserted.
    gd(4, 0x8C100000);
    CHECK(m.read32(0x8C100000) == 1u);
    CHECK(m.read32(0x8C100004) == 0x80u);
    // DMAREAD 4 sectors from FAD 45150 (LBA 45000) to 0x8C200000.
    m.write32(0x8C100010, 45150);
    m.write32(0x8C100014, 4);
    m.write32(0x8C100018, 0x8C200000);
    m.write32(0x8C10001C, 0);
    const std::uint32_t id = gd(0, 0x11, 0x8C100010);
    CHECK(id == 1);
    CHECK(gd(0, 0x11, 0x8C100010) == 0);  // queue busy: no id
    // Not complete until the clock has moved: EXEC_SERVER starts the read, GET_CMD_STAT says busy.
    gd(2);
    CHECK(gd(1, id, 0x8C100020) == hle::GDC_BUSY);
    CHECK(m.read32(0x8C200000) == 0u);
    c.cycles += 200'000;  // 8 KB at the small-transfer rate: 2 cycles a byte
    gd(2);
    CHECK(gd(1, id, 0x8C100020) == hle::GDC_COMPLETE);
    CHECK(m.read32(0x8C100028) == 4 * 2048);  // bytes transferred
    CHECK_EQ(std::memcmp(m.ram() + 0x200000, "SEGA SEGAKATANA ", 16), 0);
    CHECK(sys.holly.raised(holly::Irq::GdromDma));
    CHECK(gd(1, id, 0x8C100020) == hle::GDC_OK);  // reported once; queue idle again
    CHECK(bios.sectors_read == 4);
    // REQ_MODE / SET_MODE / GET_VERSION round trip.
    m.write32(0x8C100010, 0x8C100040);
    const std::uint32_t id2 = gd(0, 0x1E, 0x8C100010);
    gd(2);
    CHECK(gd(1, id2, 0) == hle::GDC_COMPLETE);
    CHECK(m.read32(0x8C100044) == 0xE10u);  // standby default
    m.write32(0x8C100010, 0);               // speed
    m.write32(0x8C100014, 0x1234);          // standby
    m.write32(0x8C100018, 0x19);
    m.write32(0x8C10001C, 8);
    const std::uint32_t id3 = gd(0, 0x1F, 0x8C100010);
    gd(2);
    CHECK(gd(1, id3, 0) == hle::GDC_COMPLETE);
    m.write32(0x8C100010, 0x8C100060);
    m.write32(0x8C100014, 0);
    const std::uint32_t id4 = gd(0, 0x28, 0x8C100010);
    gd(2);
    CHECK(gd(1, id4, 0) == hle::GDC_COMPLETE);
    CHECK_EQ(std::memcmp(m.ram() + 0x100060, "GDC Version 1.1", 15), 0);
    // GETTOC2 for the high-density area.
    m.write32(0x8C100010, 1);
    m.write32(0x8C100014, 0x8C100200);
    const std::uint32_t id5 = gd(0, 0x13, 0x8C100010);
    gd(2);
    CHECK(gd(1, id5, 0) == hle::GDC_COMPLETE);
    CHECK(m.read32(0x8C100200 + 2 * 4) == (0x41u << 24 | 45150u));
    // Unknown command reports an error once.
    const std::uint32_t id6 = gd(0, 0x77, 0);
    gd(2);
    CHECK(gd(1, id6, 0x8C100020) == static_cast<std::uint32_t>(hle::GDC_ERR));
    CHECK(m.read32(0x8C100020) == 5u);
    // Misc through the same vector.
    c.r[6] = 0xFFFFFFFFu;
    c.r[7] = 0;
    sh4::call_indirect(c, m, m.read32(Bios::kVecGd));
    CHECK(c.r[0] == 0);
}

TEST_CASE("bios: boot state mirrors the real BIOS hand-over and loads IP.BIN") {
    std::string err;
    auto disc = gdrom::open_disc(kFixture, err);
    REQUIRE(disc);
    System sys;
    Bios bios(sys);
    bios.attach_disc(disc.get());
    bios.setup_boot(0x8C010000);
    auto& c = sys.ctx;
    CHECK(c.pc == 0x8C010000u);
    CHECK(c.r[15] == 0x8D000000u);
    CHECK(c.gbr == 0x8C000000u);
    CHECK(c.vbr == 0x8C000000u);
    CHECK(sh4::read_sr(c) == 0x400000F1u);
    CHECK(c.fpscr == 0x00040001u);
    CHECK(sys.memory.read32(Bios::kVecGd) == Bios::kHookGd);
    CHECK_EQ(std::memcmp(sys.memory.ram() + 0x8000, "SEGA SEGAKATANA ", 16), 0);
    CHECK(sys.memory.read8(Bios::kSysInfoBlock + 8) == '0');
}
