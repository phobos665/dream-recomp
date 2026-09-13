#include "dream/runtime/holly/sysblock.h"
#include "dream/runtime/pvr/core.h"
#include "dream/runtime/sh4/dmac.h"
#include "dream/runtime/system.h"

#include "doctest.h"

using namespace dream;

namespace {
struct Rig {
    System sys;
    pvr::Core core{sys.sched, sys.holly, sys.spg, sys.memory};
    sh4::Dmac dmac{sys.memory};
    holly::SysBlock sb{dmac, sys.holly, sys.memory, core};
    Rig() {
        sys.memory.map_mmio(pvr::Core::kRegBase, pvr::Core::kRegEnd, &core);
        sys.memory.map_mmio(pvr::Core::kFifoBase, pvr::Core::kFifoEnd, &core);
        sys.memory.map_p4(sh4::Dmac::kBase, sh4::Dmac::kEnd, &dmac);
        sys.memory.map_mmio(holly::SysBlock::kBase, holly::SysBlock::kEnd, &sb);
    }
    // Katana's DMA init: channel 2 armed for DDT, controller enabled, then the SB registers.
    void arm_ch2(std::uint32_t src, std::uint32_t dst, std::uint32_t len) {
        auto& m = sys.memory;
        m.write32(0xFFA00020u, src);          // SAR2
        m.write32(0xFFA00028u, len / 32);     // DMATCR2
        m.write32(0xFFA0002Cu, 0x00001201u);  // CHCR2: DE, external request, 32-byte units
        m.write32(0xFFA00040u, 0x00008201u);  // DMAOR: DDT | DME
        m.write32(0xA05F6800u, dst);          // SB_C2DSTAT
        m.write32(0xA05F6804u, len);          // SB_C2DLEN
    }
    void start_ch2() { sys.memory.write32(0xA05F6808u, 1); }
};
}  // namespace

TEST_CASE("dmac: registers read back as written, DMAOR flags and CHCR.TE clear only by writing 0") {
    Rig r;
    auto& m = r.sys.memory;
    m.write32(0xFFA00040u, 0x8201u);
    CHECK(m.read32(0xFFA00040u) == 0x8201u);  // the Katana init loop polls for exactly this
    m.write32(0xFFA00020u, 0x0C100000u);
    m.write32(0xFFA0002Cu, 0x1201u);
    CHECK(m.read32(0xFFA00020u) == 0x0C100000u);
    CHECK(m.read32(0xFFA0002Cu) == 0x1201u);
    r.dmac.complete(2);
    CHECK((m.read32(0xFFA0002Cu) & sh4::Dmac::kTe) != 0);
    m.write32(0xFFA0002Cu, 0x1203u);  // writing TE=1 leaves it set
    CHECK((m.read32(0xFFA0002Cu) & sh4::Dmac::kTe) != 0);
    m.write32(0xFFA0002Cu, 0x1201u);  // writing 0 clears it
    CHECK((m.read32(0xFFA0002Cu) & sh4::Dmac::kTe) == 0);
    r.dmac.address_error();
    CHECK((m.read32(0xFFA00040u) & sh4::Dmac::kAe) != 0);
    m.write32(0xFFA00040u, 0x8205u);  // AE written 1: stays
    CHECK((m.read32(0xFFA00040u) & sh4::Dmac::kAe) != 0);
    m.write32(0xFFA00040u, 0x8201u);  // AE written 0: cleared
    CHECK(m.read32(0xFFA00040u) == 0x8201u);
    CHECK(r.sb.reg(holly::SysBlock::kSbRev) == 0x0Bu);
    CHECK(m.read32(0xA05F689Cu) == 0x0Bu);
    CHECK(m.read32(0xA05F6880u) == 8u);  // TA FIFO free slots
}

TEST_CASE("sysblock: channel-2 DMA feeds the TA FIFO in 32-byte chunks and completes") {
    Rig r;
    auto& m = r.sys.memory;
    // Two chunks: a polygon header for the opaque list and a vertex; the parser counts both.
    const std::uint32_t src = 0x0C100000u;
    for (unsigned i = 0; i < 16; ++i)
        m.write32(src + 4 * i, i == 0 ? (4u << 29) : i == 8 ? (7u << 29) : 0x3F800000u);
    r.arm_ch2(src, 0x10000000u, 64);
    CHECK(r.core.ta.chunks == 0);
    r.start_ch2();
    CHECK(r.core.ta.chunks == 2);
    CHECK(r.sb.ch2_transfers == 1);
    CHECK(r.sb.ch2_ta_bytes == 64);
    CHECK(m.read32(0xA05F6808u) == 0);                     // SB_C2DST clears
    CHECK(m.read32(0xA05F6804u) == 0);                     // SB_C2DLEN clears
    CHECK((m.read32(0xFFA0002Cu) & sh4::Dmac::kTe) != 0);  // channel done
    CHECK(m.read32(0xFFA00028u) == 0);
    CHECK(r.sys.holly.raised(holly::Irq::Ch2Dma));
}

TEST_CASE(
    "sysblock: channel-2 DMA to the texture area lands in VRAM and advances the destination") {
    Rig r;
    auto& m = r.sys.memory;
    const std::uint32_t src = 0x0C200000u;
    for (unsigned i = 0; i < 16; ++i) m.write32(src + 4 * i, 0xA0000000u + i);
    m.write32(0xA05F6884u, 0);  // LMMODE0: 64-bit path
    r.arm_ch2(src, 0x11000000u | 0x00100000u, 64);
    r.start_ch2();
    CHECK(r.sb.ch2_texture_bytes == 64);
    CHECK(m.read32(0xA4100000u) == 0xA0000000u);  // 64-bit VRAM view
    CHECK(m.read32(0xA410003Cu) == 0xA000000Fu);
    CHECK(m.read32(0xA05F6800u) == 0x11100040u);  // SB_C2DSTAT advanced past the data
    CHECK(r.core.ta.chunks == 0);
    CHECK(r.sys.holly.raised(holly::Irq::Ch2Dma));
    // 32-bit path through LMMODE0 = 1 interleaves into the other view.
    r.sys.holly.reset();
    m.write32(0xA05F6884u, 1);
    r.arm_ch2(src, 0x11000000u | 0x00200000u, 32);
    r.start_ch2();
    CHECK(m.read32(0xA5200000u) == 0xA0000000u);
    CHECK(m.read32(0xA520001Cu) == 0xA0000007u);
}

TEST_CASE("sysblock: channel-2 DMA refuses a source outside RAM and a controller not in DDT mode") {
    Rig r;
    auto& m = r.sys.memory;
    r.arm_ch2(0x0C100000u, 0x10000000u, 32);
    m.write32(0xFFA00040u, 0x0001u);  // DDT off
    r.start_ch2();
    CHECK(r.sb.ch2_errors == 1);
    CHECK(r.core.ta.chunks == 0);
    CHECK_FALSE(r.sys.holly.raised(holly::Irq::Ch2Dma));
    r.arm_ch2(0x04000000u, 0x10000000u, 32);  // VRAM is not a legal source
    r.start_ch2();
    CHECK(r.sb.ch2_errors == 2);
    CHECK((m.read32(0xFFA00040u) & sh4::Dmac::kAe) != 0);
    CHECK(r.sys.holly.raised(holly::Irq::Ch2Dma));
    CHECK(r.core.ta.chunks == 0);
}

TEST_CASE("dmac: auto-request manual DMA copies memory and sets TE") {
    Rig r;
    auto& m = r.sys.memory;
    for (unsigned i = 0; i < 8; ++i) m.write32(0x0C300000u + 4 * i, 0x100 + i);
    m.write32(0xFFA00040u, 0x0001u);      // DME
    m.write32(0xFFA00000u, 0x0C300000u);  // SAR0
    m.write32(0xFFA00004u, 0x0C300100u);  // DAR0
    m.write32(0xFFA00008u, 8);            // 8 longwords
    // CHCR0: DM=inc (bit 14), SM=inc (bit 12), RS=4 auto request (bits 11:8), TS=longword (bits 6:4
    // = 3), DE
    m.write32(0xFFA0000Cu, (1u << 14) | (1u << 12) | (4u << 8) | (3u << 4) | 1u);
    CHECK(r.dmac.manual_transfers == 1);
    CHECK(m.read32(0x0C300100u) == 0x100u);
    CHECK(m.read32(0x0C30011Cu) == 0x107u);
    CHECK((m.read32(0xFFA0000Cu) & sh4::Dmac::kTe) != 0);
    CHECK(m.read32(0xFFA00008u) == 0);
}
