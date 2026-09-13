// The TSP instruction word's field layout (WP2.3). A wrong bit position here does not fail a
// build, it draws: reading "ignore the texture's alpha" one bit high turned the transparent parts
// of Crazy Taxi's logo tiles opaque and put solid red rectangles on the title screen. Each field is
// pinned by setting it alone, so a failure names the field.
#include <cstdint>
#include <initializer_list>

#include "dream/render/tsp.h"

#include "doctest.h"

namespace {

using namespace dream::render;
using u32 = std::uint32_t;

// One field, at one bit position, with every other bit set, so a reader that is off by one or that
// forgets to mask picks up a neighbour and the test says so.
constexpr u32 only(unsigned lsb, u32 value) {
    return value << lsb;
}
constexpr u32 all_but(unsigned lsb, unsigned bits) {
    return ~(((1u << bits) - 1u) << lsb);
}

}  // namespace

TEST_CASE("tsp: every field sits where the hardware puts it") {
    // Positions from Flycast's union TSP (core/hw/pvr/ta_structs.h), which is the hardware's own
    // order: TexV, TexU, ShadInstr, MipMapD, SupSample, FilterMode, ClampV, ClampU, FlipV, FlipU,
    // IgnoreTexA, UseAlpha, ColorClamp, FogCtrl, DstSelect, SrcSelect, DstInstr, SrcInstr.
    struct Field {
        const char* name;
        unsigned lsb, bits;
        u32 (*read)(u32);
    };
    static const Field kFields[] = {
        {"TexV", 0, 3, tsp_v_size},
        {"TexU", 3, 3, tsp_u_size},
        {"ShadInstr", 6, 2, tsp_shading_instruction},
        {"MipMapD", 8, 4, tsp_mipmap_bias},
        {"SupSample", 12, 1, tsp_super_sample},
        {"FilterMode", 13, 2, tsp_filter_mode},
        {"ClampV", 15, 1, tsp_clamp_v},
        {"ClampU", 16, 1, tsp_clamp_u},
        {"FlipV", 17, 1, tsp_flip_v},
        {"FlipU", 18, 1, tsp_flip_u},
        {"IgnoreTexA", 19, 1, tsp_ignore_texture_alpha},
        {"UseAlpha", 20, 1, tsp_use_alpha},
        {"ColorClamp", 21, 1, tsp_colour_clamp},
        {"FogCtrl", 22, 2, tsp_fog_control},
        {"DstInstr", 26, 3, tsp_dst_instr},
        {"SrcInstr", 29, 3, tsp_src_instr},
    };
    for (const Field& f : kFields) {
        CAPTURE(f.name);
        const u32 max = (1u << f.bits) - 1u;
        // The field alone, everything else clear.
        CHECK(f.read(only(f.lsb, max)) == max);
        CHECK(f.read(0) == 0);
        // The field clear, everything else set: anything but zero means it is reading a neighbour.
        CHECK(f.read(all_but(f.lsb, f.bits)) == 0);
    }
}

TEST_CASE("tsp: a real word from Crazy Taxi's logo decodes as the title expects") {
    // Captured from the polygon that draws a logo tile on the "no VMU" screen. Read one bit high,
    // IgnoreTexA came out set and the tile was drawn as a solid red rectangle.
    constexpr u32 kLogoTile = 0x949024e4u;
    CHECK(tsp_ignore_texture_alpha(kLogoTile) == 0);  // the texture's alpha is real
    CHECK(tsp_use_alpha(kLogoTile) == 1);             // and the surface blends
    CHECK(tsp_src_instr(kLogoTile) == 4);             // source alpha
    CHECK(tsp_dst_instr(kLogoTile) == 5);             // one minus source alpha
    CHECK(tsp_shading_instruction(kLogoTile) == 3);
}

TEST_CASE("tsp: the sampler key covers filtering, clamping and mirroring and nothing else") {
    // The sampler cache is keyed on this, so a field a sampler reads that falls outside it would
    // hand back a sampler built for different state.
    for (unsigned bit : {13u, 14u, 15u, 16u, 17u, 18u}) CHECK(tsp_sampler_key(1u << bit) != 0);
    for (unsigned bit : {0u, 6u, 12u, 19u, 20u, 26u, 31u}) CHECK(tsp_sampler_key(1u << bit) == 0);
}
