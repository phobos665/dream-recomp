// The background plane (WP2.3). Built in video memory the way a title builds it, then read back,
// because the two things that go wrong here are the address mapping and the vertex layout, and
// both produce a plausible-looking nothing rather than an error.
#include <cstdint>
#include <cstring>
#include <vector>

#include "dream/render/background.h"
#include "dream/runtime/mem/dc_memory.h"

#include "doctest.h"

namespace {

using namespace dream::render;
using u32 = std::uint32_t;

u32 f2u(float f) {
    u32 w;
    std::memcpy(&w, &f, 4);
    return w;
}

// A PVR register block and a video memory image, with the parameter buffer written through the
// 32-bit view exactly as the guest writes it.
struct Scene {
    std::vector<u32> regs = std::vector<u32>(0x2000 / 4, 0);
    std::vector<std::uint8_t> vram = std::vector<std::uint8_t>(dream::mem::DcMemory::kVramSize, 0);

    void put(u32 addr, u32 value) {
        const u32 mapped = dream::mem::DcMemory::vram_map32(addr);
        std::memcpy(vram.data() + mapped, &value, 4);
    }
    // ISP_BACKGND_T: where the plane is, how many extra words each vertex has, which vertex starts.
    void tag(u32 address_words, u32 skip, u32 offset = 0, u32 shadow = 0) {
        regs[0x08C / 4] = offset | (address_words << 3) | (skip << 24) | (shadow << 27);
    }
    void depth(float d) { regs[0x088 / 4] = f2u(d); }
    void param_base(u32 v) { regs[0x020 / 4] = v; }
};

}  // namespace

TEST_CASE("background: an untextured plane covers the screen in its own colour") {
    Scene s;
    s.param_base(0);
    s.tag(0x1c40, 1);
    s.depth(1e-5f);
    const u32 base = 0x1c40 * 4;
    s.put(base + 0, 0x80800000u);  // ISP: gouraud, no texture, depth mode 4
    s.put(base + 4, 0x20880440u);  // TSP
    s.put(base + 8, 0);            // TCW
    // Three vertices of four words each: x, y, z, packed colour.
    const u32 white = 0xFFFFFFFFu;
    const float xs[3] = {0.0f, 640.0f, 0.0f}, ys[3] = {0.0f, 0.0f, 480.0f};
    for (unsigned i = 0; i < 3; ++i) {
        const u32 v = base + 12 + i * 16;
        s.put(v + 0, f2u(xs[i]));
        s.put(v + 4, f2u(ys[i]));
        s.put(v + 8, f2u(1e-5f));
        s.put(v + 12, white);
    }

    Frame f;
    REQUIRE(add_background(f, s.regs.data(), s.vram.data(), s.vram.size()));
    REQUIRE(f.vertices.size() == 4);
    REQUIRE(f.lists[0].size() == 1);
    const Polygon& p = f.lists[0][0];
    CHECK(p.count == 4);
    for (const Vertex& v : f.vertices) CHECK(v.base == white);
    // The hardware ignores the positions it was given for an untextured plane and covers the
    // screen with room on both sides, so a stretched or offset display never shows its edge.
    CHECK(f.vertices[0].x <= 0.0f);
    CHECK(f.vertices[1].x >= 640.0f);
    CHECK(f.vertices[2].y == 480.0f);
    CHECK(f.vertices[3].x == f.vertices[1].x);
    // Always pass the depth test and never cull: it is behind everything and has no winding.
    CHECK(((p.isp >> 29) & 7u) == 7u);
    CHECK(((p.isp >> 27) & 3u) == 0u);
    // No parameter control word exists, so the flags come from the ISP word.
    CHECK(((p.pcw >> 3) & 1u) == 0u);  // untextured
    CHECK(((p.pcw >> 1) & 1u) == 1u);  // gouraud
}

TEST_CASE("background: reading the parameter buffer flat finds nothing") {
    // The buffer is written through the 32-bit view, which interleaves the two banks. Reading the
    // register's address straight out of the flat image lands on the wrong bytes, and those are
    // zero, which looks exactly like "this title has no background plane".
    Scene s;
    s.tag(0x1c40, 1);
    const u32 base = 0x1c40 * 4;
    s.put(base, 0x80800000u);
    CHECK(dream::mem::DcMemory::vram_map32(base) != base);
    u32 flat = 0;
    std::memcpy(&flat, s.vram.data() + base, 4);
    CHECK(flat == 0);
}

TEST_CASE("background: an empty parameter buffer is not a plane") {
    Scene s;
    s.tag(0x1c40, 1);
    Frame f;
    CHECK_FALSE(add_background(f, s.regs.data(), s.vram.data(), s.vram.size()));
    CHECK(f.vertices.empty());
}

TEST_CASE("background: PARAM_BASE selects the buffer by its top bits only") {
    // Crazy Taxi alternates two parameter buffers, 0 and 0x400000, and only the top four bits of
    // PARAM_BASE are part of the address.
    Scene s;
    s.param_base(0x400000u | 0x1234u);
    s.tag(0x1c40, 1);
    s.depth(1e-5f);
    const u32 base = 0x400000u + 0x1c40 * 4;
    s.put(base + 0, 0x80800000u);
    s.put(base + 4, 0x20880440u);
    s.put(base + 8, 0);
    for (unsigned i = 0; i < 3; ++i) {
        const u32 v = base + 12 + i * 16;
        s.put(v + 0, f2u(0.0f));
        s.put(v + 4, f2u(0.0f));
        s.put(v + 8, f2u(1e-5f));
        s.put(v + 12, 0xFF00FF00u);
    }
    Frame f;
    REQUIRE(add_background(f, s.regs.data(), s.vram.data(), s.vram.size()));
    CHECK(f.vertices[0].base == 0xFF00FF00u);
}
