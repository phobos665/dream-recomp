// See fog.h.
#include "dream/render/fog.h"

#include <algorithm>
#include <cmath>

namespace dream::render {

namespace {

constexpr std::size_t kFogColRam = 0x0B0 / 4;
constexpr std::size_t kFogColVert = 0x0B4 / 4;
constexpr std::size_t kFogDensity = 0x0B8 / 4;
constexpr std::size_t kFogClampMax = 0x0BC / 4;
constexpr std::size_t kFogClampMin = 0x0C0 / 4;
constexpr std::size_t kFogTable = 0x200 / 4;

// A PVR colour register is 0x00RRGGBB; the renderer wants RGBA8888 with the alpha opaque.
std::uint32_t colour_of(std::uint32_t reg) {
    const std::uint32_t r = (reg >> 16) & 0xFFu, g = (reg >> 8) & 0xFFu, b = reg & 0xFFu;
    return r | (g << 8) | (b << 16) | (0xFFu << 24);
}

}  // namespace

void describe_fog(const std::uint32_t* pvr_regs, FogSettings& out) {
    out = FogSettings{};
    if (!pvr_regs)
        return;

    // FOG_DENSITY: mantissa in the high byte over a *signed* exponent in the low one. Reading the
    // exponent as unsigned turns a fog that thins with distance into one that does not move.
    const std::uint32_t density = pvr_regs[kFogDensity];
    const auto exponent = static_cast<std::int8_t>(density & 0xFFu);
    const auto mantissa = static_cast<std::uint8_t>((density >> 8) & 0xFFu);
    out.density =
        static_cast<float>(mantissa) / 128.0f * std::pow(2.0f, static_cast<float>(exponent));

    out.table_colour = colour_of(pvr_regs[kFogColRam]);
    out.vertex_colour = colour_of(pvr_regs[kFogColVert]);
    out.clamp_min = pvr_regs[kFogClampMin];
    out.clamp_max = pvr_regs[kFogClampMax];
    out.clamping = out.clamp_min != 0 || out.clamp_max != 0xFFFFFFFFu;

    // The table is 128 words, each holding one entry's blend factor and the delta to the next.
    for (std::size_t i = 0; i < 128; ++i) {
        const std::uint32_t w = pvr_regs[kFogTable + i];
        out.factor[i] = static_cast<std::uint8_t>((w >> 8) & 0xFFu);
        out.delta[i] = static_cast<std::uint8_t>(w & 0xFFu);
    }
}

float fog_table_index(float density, float inv_w) {
    // The hardware indexes the table by the exponent and mantissa of the scaled depth, which makes
    // the 128 entries logarithmic in depth rather than linear: sixteen entries per doubling, so
    // near geometry gets the resolution and far geometry does not waste it. Clamped to the range
    // the eight exponents cover.
    const float z = std::clamp(density * inv_w, 1.0f, 255.9999f);
    const float exponent = std::floor(std::log2(z));
    const float m = z * 16.0f / std::pow(2.0f, exponent) - 16.0f;
    return std::floor(m) + exponent * 16.0f;
}

}  // namespace dream::render
