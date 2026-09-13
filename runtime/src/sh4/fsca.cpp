// FSCA lookup table, built the way Flycast builds sin_table so the differential harness agrees bit
// for bit: sin over the first half-turn comes from the hardware-captured coefficients, the second
// half is its negation, and cos(i) = sin(i + quarter turn).
#include <cstdint>
#include <cstring>

#include "dream/runtime/sh4/ops.h"

namespace dream::sh4::detail {
namespace {

constexpr std::uint32_t kCoefs[0x8000] = {
#include "fsca_coefs.inc"
};

// Static storage, filled once on first use. Not a local: 512 KB on the stack overflowed the 1 MB
// default thread stack on Windows (found by CI when p5 called fsca).
SinCos g_table[0x10000];

void fill() noexcept {
    for (std::uint32_t i = 0; i < 0x10000; ++i) {
        float s;
        std::memcpy(&s, &kCoefs[i & 0x7FFF], sizeof s);
        g_table[i].s = i < 0x8000 ? s : -s;
    }
    for (std::uint32_t i = 0; i < 0x10000; ++i) g_table[i].c = g_table[(i + 0x4000) & 0xFFFF].s;
}

}  // namespace

const SinCos* fsca_table() noexcept {
    static const bool built = [] {
        fill();
        return true;
    }();
    (void)built;
    return g_table;
}

}  // namespace dream::sh4::detail
