// G2 bus DMA: RAM to sound RAM and back, enable handling, the end-of-DMA interrupt.
#include <cstdint>

#include "dream/runtime/holly/g2dma.h"
#include "dream/runtime/system.h"

#include "doctest.h"

namespace {
struct Rig {
    dream::System sys;
    dream::holly::G2Dma g2{sys.sched, sys.holly, sys.memory};
    using G = dream::holly::G2Dma;
    void w(std::uint32_t off, std::uint32_t v) { g2.write(G::kBase + off, v, 4); }
};
}  // namespace

TEST_CASE("g2dma: AICA channel copies a block into sound RAM and interrupts") {
    Rig r;
    for (std::uint32_t i = 0; i < 64; ++i)
        r.sys.memory.write32(0x8C100000u + 4 * i, 0xA0000000u + i);
    r.w(Rig::G::kStar, 0x0C100000u);
    r.w(Rig::G::kStag, 0x00800400u);
    r.w(Rig::G::kLen, 256u | 0x80000000u);  // clear EN when done
    r.w(Rig::G::kDir, 0);
    r.w(Rig::G::kEn, 1);
    r.w(Rig::G::kSt, 1);
    // 256 bytes take 1024 cycles: still in progress until the scheduler gets there.
    CHECK(r.g2.reg(0, Rig::G::kSt) == 1);
    CHECK(r.sys.memory.read32(0x00800400u) == 0xA0000000u);
    CHECK(r.sys.memory.read32(0x008004FCu) == 0xA000003Fu);
    CHECK_FALSE(r.sys.holly.raised(dream::holly::Irq::AicaDma));
    r.sys.sched.advance(2000);
    CHECK(r.g2.reg(0, Rig::G::kSt) == 0);
    CHECK(r.g2.reg(0, Rig::G::kEn) == 0);
    CHECK(r.g2.reg(0, Rig::G::kStar) == 0x0C100100u);
    CHECK(r.g2.reg(0, Rig::G::kLen) == 0);
    CHECK(r.sys.holly.raised(dream::holly::Irq::AicaDma));
    CHECK(r.g2.transfers == 1);
    CHECK(r.g2.to_aica_ram == 256);
}

TEST_CASE(
    "g2dma: reverse direction, short transfers complete at once, disabled channel ignores start") {
    Rig r;
    r.sys.memory.write32(0x00801000u, 0x12345678u);
    r.w(Rig::G::kStar, 0x0C200000u);
    r.w(Rig::G::kStag, 0x00801000u);
    r.w(Rig::G::kLen, 32);
    r.w(Rig::G::kDir, 1);
    r.w(Rig::G::kEn, 1);
    r.w(Rig::G::kSt, 1);
    CHECK(r.sys.memory.read32(0x8C200000u) == 0x12345678u);
    CHECK(r.g2.reg(0, Rig::G::kSt) == 0);  // 32 bytes: 128 cycles, finished immediately
    CHECK(r.g2.reg(0, Rig::G::kEn) == 1);  // LEN bit 31 clear keeps the channel enabled
    r.w(Rig::G::kEn, 0);
    r.w(Rig::G::kLen, 32);
    r.w(Rig::G::kSt, 1);
    CHECK(r.g2.transfers == 1);
    // An invalid source is refused with the illegal-address interrupt.
    r.w(Rig::G::kEn, 1);
    r.w(Rig::G::kStar, 0xA0000000u);  // BIOS ROM is not a DMA source
    r.w(Rig::G::kLen, 32);
    r.w(Rig::G::kSt, 1);
    CHECK(r.g2.refused == 1);
    CHECK(r.g2.reg(0, Rig::G::kEn) == 0);
}
