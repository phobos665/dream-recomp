// Fog (WP2.3). The registers are small and the curve is easy to get subtly wrong in a way that
// looks like haze either way, so both are checked here as numbers rather than judged as a picture.
#include <cmath>
#include <cstdint>
#include <vector>

#include "dream/render/fog.h"

#include "doctest.h"

namespace {
using namespace dream::render;
using u32 = std::uint32_t;

struct Regs {
    std::vector<u32> words = std::vector<u32>(0x2000 / 4, 0);
    u32& at(std::size_t byte_offset) { return words[byte_offset / 4]; }
    void density(std::uint8_t mantissa, std::int8_t exponent) {
        at(0x0B8) = static_cast<u32>(static_cast<std::uint8_t>(exponent)) | (u32{mantissa} << 8);
    }
};
}  // namespace

TEST_CASE("fog: the density register's exponent is signed") {
    // Mantissa over a signed exponent. Reading the exponent as unsigned turns a fog that thins with
    // distance into one that barely moves, which looks like a haze either way and so survives a
    // glance at a screenshot.
    Regs r;
    FogSettings f;

    r.density(128, 0);  // 128/128 * 2^0
    describe_fog(r.words.data(), f);
    CHECK(f.density == doctest::Approx(1.0f));

    r.density(128, 1);
    describe_fog(r.words.data(), f);
    CHECK(f.density == doctest::Approx(2.0f));

    r.density(128, -1);  // the case that breaks if the exponent is read unsigned
    describe_fog(r.words.data(), f);
    CHECK(f.density == doctest::Approx(0.5f));

    r.density(64, -2);
    describe_fog(r.words.data(), f);
    CHECK(f.density == doctest::Approx(0.125f));

    // Crazy Taxi's own value, so a change that breaks this title fails here.
    r.words[0x0B8 / 4] = 0x0000808Fu;
    describe_fog(r.words.data(), f);
    CHECK(f.density == doctest::Approx(std::pow(2.0f, -113.0f)).epsilon(0.01));
}

TEST_CASE("fog: colour registers are RGB in the low three bytes") {
    Regs r;
    r.at(0x0B0) = 0x00123456u;  // FOG_COL_RAM
    r.at(0x0B4) = 0x00ABCDEFu;  // FOG_COL_VERT
    FogSettings f;
    describe_fog(r.words.data(), f);
    // Stored as RGBA8888 with an opaque alpha, which is what the renderer wants.
    CHECK((f.table_colour & 0xFFu) == 0x12u);          // red
    CHECK(((f.table_colour >> 8) & 0xFFu) == 0x34u);   // green
    CHECK(((f.table_colour >> 16) & 0xFFu) == 0x56u);  // blue
    CHECK(((f.table_colour >> 24) & 0xFFu) == 0xFFu);
    CHECK((f.vertex_colour & 0xFFu) == 0xABu);
    CHECK(((f.vertex_colour >> 16) & 0xFFu) == 0xEFu);
}

TEST_CASE("fog: the table is 128 entries of a factor and a delta") {
    Regs r;
    for (std::size_t i = 0; i < 128; ++i) r.words[0x200 / 4 + i] = (u32(i) << 8) | (127u - u32(i));
    FogSettings f;
    describe_fog(r.words.data(), f);
    CHECK(f.factor[0] == 0);
    CHECK(f.delta[0] == 127);
    CHECK(f.factor[127] == 127);
    CHECK(f.delta[127] == 0);
}

TEST_CASE("fog: the table index is logarithmic in depth, sixteen entries per doubling") {
    // This is the part that makes the curve right. A linear index would spend the same number of
    // entries on the far half of the world as on the near, and the near half is the half anyone
    // looks at.
    const float density = 1.0f;
    const float a = fog_table_index(density, 1.0f);
    const float b = fog_table_index(density, 2.0f);
    const float c = fog_table_index(density, 4.0f);
    CHECK(b - a == doctest::Approx(16.0f));  // one doubling of depth, sixteen entries
    CHECK(c - b == doctest::Approx(16.0f));  // and again, wherever it starts

    // Below the table's range everything lands on the first entry, above it on the last.
    CHECK(fog_table_index(density, 0.001f) == doctest::Approx(0.0f));
    CHECK(fog_table_index(density, 1e9f) <= 128.0f);
    CHECK(fog_table_index(density, 1e9f) >= 112.0f);

    // Density scales depth before the lookup, so doubling it is one doubling of depth.
    CHECK(fog_table_index(2.0f, 1.0f) - fog_table_index(1.0f, 1.0f) == doctest::Approx(16.0f));
}
