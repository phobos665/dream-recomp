#include <vector>

#include "dream/runtime/pvr/core.h"
#include "dream/runtime/system.h"

#include "doctest.h"

using namespace dream;

namespace {
struct Rig {
    System sys;
    pvr::Core core{sys.sched, sys.holly, sys.spg, sys.memory};
    Rig() {
        sys.memory.map_mmio(pvr::Core::kRegBase, pvr::Core::kRegEnd, &core);
        sys.memory.map_mmio(pvr::Core::kFifoBase, pvr::Core::kFifoEnd, &core);
    }
    // Control words. obj_ctrl bits: UV16 0, Gouraud 1, Offset 2, Texture 3, Col_Type 5:4, Volume 6.
    static std::uint32_t pcw(unsigned para, unsigned list, unsigned obj_ctrl = 0,
                             bool end_of_strip = false) {
        return (para << 29) | (end_of_strip ? 1u << 28 : 0) | (list << 24) | obj_ctrl;
    }
    void chunk(std::uint32_t w0) {
        std::uint32_t words[8] = {w0, 1, 2, 3, 4, 5, 6, 7};
        sys.memory.sq_write32(0xE0000000u, words[0]);
        for (unsigned i = 1; i < 8; ++i) sys.memory.sq_write32(0xE0000000u + 4 * i, words[i]);
        sys.memory.set_qacr(0, 4u << 2);  // area 4: TA FIFO
        sys.memory.sq_flush(0xE0000000u);
    }
    void list_init() { sys.memory.write32(0xA05F8144u, 0x80000000u); }
};
}  // namespace

TEST_CASE("pvr: registers reset to the hardware ids and TA_LIST_INIT seeds the pointers") {
    Rig r;
    auto& m = r.sys.memory;
    CHECK(m.read32(0xA05F8000u) == 0x17FD11DBu);
    CHECK(m.read32(0xA05F8004u) == 0x11u);
    m.write32(0xA05F8128u, 0x00100000u);  // TA_ISP_BASE
    m.write32(0xA05F8164u, 0x00200000u);  // TA_NEXT_OPB_INIT
    r.list_init();
    CHECK(m.read32(0xA05F8138u) == 0x00100000u);  // TA_ITP_CURRENT
    CHECK(m.read32(0xA05F8134u) == 0x00200000u);  // TA_NEXT_OPB
    CHECK(r.core.list_inits == 1);
    // SPG registers keep working through the core's range.
    CHECK((m.read32(0xA05F810Cu) & 0x3FFu) == r.sys.spg.scanline());
    m.write32(0xA05F8044u, 1u << 23);  // FB_R_CTRL vclk_div: 27 MHz pixel clock
    CHECK(r.sys.spg.line_cycles() == 12711u / 2);
}

TEST_CASE("pvr: an opaque list of 32-byte polygons ends with the opaque-list interrupt") {
    Rig r;
    r.list_init();
    r.chunk(Rig::pcw(pvr::kPolygonOrModVol, pvr::kOpaque,
                     0x02));  // gouraud, packed colour: 32-byte header
    for (int i = 0; i < 3; ++i) r.chunk(Rig::pcw(pvr::kVertex, 0, 0, i == 2));
    CHECK(!r.sys.holly.raised(holly::Irq::OpaqueDone));
    r.chunk(Rig::pcw(pvr::kEndOfList, pvr::kOpaque));
    CHECK(r.sys.holly.raised(holly::Irq::OpaqueDone));
    const auto& op = r.core.ta.lists[pvr::kOpaque];
    CHECK(op.polygons == 1);
    CHECK(op.vertices == 3);
    CHECK(op.ends == 1);
    CHECK(r.core.ta.chunks == 5);
    CHECK(r.core.ta.stream.size() == 5 * 8);
    CHECK(r.core.ta.invalid == 0);
    CHECK(r.sys.memory.read32(0xA05F8138u) == 5 * 32);  // TA_ITP_CURRENT advanced
}

TEST_CASE("pvr: 64-byte headers and vertices, sprites and modifier volumes are sized correctly") {
    Rig r;
    r.list_init();
    // Translucent, textured, intensity colour with offset: 64-byte header; vertices 32 bytes.
    const std::uint32_t hdr64 =
        Rig::pcw(pvr::kPolygonOrModVol, pvr::kTranslucent, 0x08 | 0x04 | (2u << 4));
    CHECK(pvr::Ta::header_is_64(hdr64));
    CHECK(!pvr::Ta::vertex_is_64(hdr64));
    r.chunk(hdr64);
    r.chunk(0xDEADBEEFu);  // second half of the header: not a control word
    r.chunk(Rig::pcw(pvr::kVertex, 0, 0, true));
    r.chunk(Rig::pcw(pvr::kEndOfList, pvr::kTranslucent));
    CHECK(r.sys.holly.raised(holly::Irq::TransDone));
    CHECK(r.core.ta.lists[pvr::kTranslucent].polygons == 1);
    CHECK(r.core.ta.lists[pvr::kTranslucent].vertices == 1);
    CHECK(r.core.ta.invalid == 0);
    // Punch-through, textured floating colour: 32-byte header, 64-byte vertices.
    const std::uint32_t hdr_v64 =
        Rig::pcw(pvr::kPolygonOrModVol, pvr::kPunchThrough, 0x08 | (1u << 4));
    CHECK(!pvr::Ta::header_is_64(hdr_v64));
    CHECK(pvr::Ta::vertex_is_64(hdr_v64));
    r.chunk(hdr_v64);
    for (int v = 0; v < 2; ++v) {
        r.chunk(Rig::pcw(pvr::kVertex, 0, 0, v == 1));
        r.chunk(0x12345678u);  // second half
    }
    r.chunk(Rig::pcw(pvr::kEndOfList, pvr::kPunchThrough));
    CHECK(r.sys.holly.raised(holly::Irq::PunchThruDone));
    CHECK(r.core.ta.lists[pvr::kPunchThrough].vertices == 2);
    CHECK(r.core.ta.invalid == 0);
    // Sprites: 32-byte header, 64-byte vertices.
    r.chunk(Rig::pcw(pvr::kSprite, pvr::kOpaque, 0x08));
    r.chunk(Rig::pcw(pvr::kVertex, 0, 0, true));
    r.chunk(0);
    r.chunk(Rig::pcw(pvr::kEndOfList, pvr::kOpaque));
    CHECK(r.core.ta.lists[pvr::kOpaque].sprites == 1);
    CHECK(r.core.ta.lists[pvr::kOpaque].vertices == 1);
    // Modifier volume: 32-byte header, 64-byte volumes.
    r.chunk(Rig::pcw(pvr::kPolygonOrModVol, pvr::kOpaqueModVol));
    r.chunk(Rig::pcw(pvr::kVertex, 0, 0, true));
    r.chunk(0);
    r.chunk(Rig::pcw(pvr::kEndOfList, pvr::kOpaqueModVol));
    CHECK(r.sys.holly.raised(holly::Irq::OpaqueModDone));
    CHECK(r.core.ta.lists[pvr::kOpaqueModVol].modvols == 1);
    CHECK(r.core.ta.lists[pvr::kOpaqueModVol].vertices == 1);
    CHECK(r.core.ta.invalid == 0);
    // A list init clears the capture.
    r.list_init();
    CHECK(r.core.ta.stream.empty());
    CHECK(r.core.ta.lists[pvr::kOpaque].polygons == 0);
}

TEST_CASE("pvr: STARTRENDER completes on the clock; texture and frame-buffer paths are tracked") {
    Rig r;
    auto& m = r.sys.memory;
    r.list_init();
    r.chunk(Rig::pcw(pvr::kPolygonOrModVol, pvr::kOpaque));
    r.chunk(Rig::pcw(pvr::kVertex, 0, 0, true));
    r.chunk(Rig::pcw(pvr::kEndOfList, pvr::kOpaque));
    bool first = false;
    r.core.on_first_ta_data = [&] { first = true; };  // already past the first chunk: stays false
    m.write32(0xA05F8014u, 0xFFFFFFFFu);              // STARTRENDER
    CHECK(r.core.renders == 1);
    CHECK(!r.sys.holly.raised(holly::Irq::RenderDone));
    r.sys.sched.advance(r.core.render_base_cycles + 4 * 24 + 1);
    CHECK(r.core.renders_done == 1);
    CHECK(r.sys.holly.raised(holly::Irq::RenderDone));
    CHECK(r.sys.holly.raised(holly::Irq::RenderDoneIsp));
    CHECK(r.sys.holly.raised(holly::Irq::RenderDoneVideo));
    CHECK(!first);
    m.write32(0xA05F8050u, 0x00200000u);  // FB_R_SOF1: present
    CHECK(r.core.frame_swaps == 1);
    CHECK(m.read32(0xA05F8050u) == 0x00200000u);
    m.write32(0x11000100u, 0xCAFEBABEu);  // texture path -> VRAM 64-bit view
    CHECK(m.read32(0xA4000100u) == 0xCAFEBABEu);
    CHECK(r.core.texture_words == 1);
    m.write32(0x10800000u, 1);
    CHECK(r.core.yuv_words == 1);
    // Word-at-a-time FIFO writes assemble chunks too (the chunk counter is cumulative).
    r.list_init();
    const std::uint64_t before = r.core.ta.chunks;
    for (unsigned i = 0; i < 8; ++i)
        m.write32(0x10000000u + 4 * i, i == 0 ? Rig::pcw(pvr::kEndOfList, pvr::kOpaque) : 0);
    CHECK(r.core.ta.chunks == before + 1);
}
