// The TSP instruction word: the per-polygon shading and sampling state.
//
// One place for these because getting a bit position wrong here does not fail, it draws. Four of
// them were wrong for a while and the symptom was solid red rectangles over Crazy Taxi's logo: the
// field that says "ignore the texture's alpha" was read one bit high, so transparent parts of a
// logo tile came out opaque. Nothing in a build catches that, so the layout is pinned by a test
// (`render/tests/test_tsp.cpp`) against the reference, Flycast's `union TSP` in
// `core/hw/pvr/ta_structs.h` (GPL-2.0, ADR 1), which is bit-for-bit the hardware's.
//
//   0-2   TexV          v size          13-14  FilterMode    point, bilinear, trilinear
//   3-5   TexU          u size          15     ClampV
//   6-7   ShadInstr     how the texture combines with the colour
//   8-11  MipMapD       mipmap bias     16     ClampU
//   12    SupSample                     17     FlipV
//   19    IgnoreTexA    treat the texture as opaque
//   20    UseAlpha      use the colour's alpha; clear means the surface is opaque
//   21    ColorClamp                    18     FlipU
//   22-23 FogCtrl       24 DstSelect    25 SrcSelect
//   26-28 DstInstr      29-31 SrcInstr  the blend factors
#pragma once

#include <cstdint>

namespace dream::render {

constexpr std::uint32_t tsp_v_size(std::uint32_t w) {
    return w & 7u;
}
constexpr std::uint32_t tsp_u_size(std::uint32_t w) {
    return (w >> 3) & 7u;
}
constexpr std::uint32_t tsp_shading_instruction(std::uint32_t w) {
    return (w >> 6) & 3u;
}
constexpr std::uint32_t tsp_mipmap_bias(std::uint32_t w) {
    return (w >> 8) & 15u;
}
constexpr std::uint32_t tsp_super_sample(std::uint32_t w) {
    return (w >> 12) & 1u;
}
constexpr std::uint32_t tsp_filter_mode(std::uint32_t w) {
    return (w >> 13) & 3u;
}
constexpr std::uint32_t tsp_clamp_v(std::uint32_t w) {
    return (w >> 15) & 1u;
}
constexpr std::uint32_t tsp_clamp_u(std::uint32_t w) {
    return (w >> 16) & 1u;
}
constexpr std::uint32_t tsp_flip_v(std::uint32_t w) {
    return (w >> 17) & 1u;
}
constexpr std::uint32_t tsp_flip_u(std::uint32_t w) {
    return (w >> 18) & 1u;
}
constexpr std::uint32_t tsp_ignore_texture_alpha(std::uint32_t w) {
    return (w >> 19) & 1u;
}
constexpr std::uint32_t tsp_use_alpha(std::uint32_t w) {
    return (w >> 20) & 1u;
}
constexpr std::uint32_t tsp_colour_clamp(std::uint32_t w) {
    return (w >> 21) & 1u;
}
constexpr std::uint32_t tsp_fog_control(std::uint32_t w) {
    return (w >> 22) & 3u;
}
constexpr std::uint32_t tsp_dst_instr(std::uint32_t w) {
    return (w >> 26) & 7u;
}
constexpr std::uint32_t tsp_src_instr(std::uint32_t w) {
    return (w >> 29) & 7u;
}

// Everything a sampler is built from: filtering, clamping and mirroring, bits 13 to 18. Used to
// key the sampler cache, so it has to cover every field a sampler reads and nothing else.
constexpr std::uint32_t tsp_sampler_key(std::uint32_t w) {
    return (w >> 13) & 0x3Fu;
}

}  // namespace dream::render
