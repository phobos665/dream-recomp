// PowerVR2 texture decoding (WP2.3 step 4). Textures are built here by hand in each hardware
// layout and decoded back, so a failure names the format rather than pointing at a whole frame.
#include <cstdint>
#include <cstring>
#include <vector>

#include "dream/render/texture.h"

#include "doctest.h"

namespace {

using namespace dream::render;
using u32 = std::uint32_t;

std::vector<std::uint8_t> vram(std::size_t bytes = 64 * 1024) {
    return std::vector<std::uint8_t>(bytes, 0);
}

void put16(std::vector<std::uint8_t>& v, std::size_t offset, std::uint16_t value) {
    v[offset] = static_cast<std::uint8_t>(value);
    v[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

// Builds a texture control word. `address` is a byte offset and must be 8-byte aligned.
u32 make_tcw(u32 address, unsigned format, bool twiddled = true, bool vq = false,
             bool mipmapped = false, unsigned palette_select = 0) {
    return ((address >> 3) & 0x1FFFFFu) | (palette_select << 21) |
           (static_cast<u32>(!twiddled) << 26) | (static_cast<u32>(format) << 27) |
           (static_cast<u32>(vq) << 30) | (static_cast<u32>(mipmapped) << 31);
}

// TSP word carrying only the size: width = 8 << u, height = 8 << v.
u32 make_tsp(unsigned u_shift, unsigned v_shift) {
    return (u_shift << 3) | v_shift;
}

}  // namespace

TEST_CASE("texture: the twiddled order interleaves y and x, y first") {
    // On a square texture this is the usual Morton order.
    CHECK(twiddle_index(0, 0, 8, 8) == 0);
    CHECK(twiddle_index(0, 1, 8, 8) == 1);
    CHECK(twiddle_index(1, 0, 8, 8) == 2);
    CHECK(twiddle_index(1, 1, 8, 8) == 3);
    CHECK(twiddle_index(0, 2, 8, 8) == 4);
    CHECK(twiddle_index(2, 0, 8, 8) == 8);
    // Every texel of a square texture maps to a distinct index covering the whole range.
    std::vector<bool> seen(64, false);
    for (u32 y = 0; y < 8; ++y)
        for (u32 x = 0; x < 8; ++x) {
            const u32 i = twiddle_index(x, y, 8, 8);
            REQUIRE(i < 64);
            CHECK_FALSE(seen[i]);
            seen[i] = true;
        }
    // A rectangular texture: the narrow dimension runs out first and the wide one continues.
    std::vector<bool> seen2(32, false);
    for (u32 y = 0; y < 4; ++y)
        for (u32 x = 0; x < 8; ++x) {
            const u32 i = twiddle_index(x, y, 8, 4);
            REQUIRE(i < 32);
            CHECK_FALSE(seen2[i]);
            seen2[i] = true;
        }
}

TEST_CASE("texture: the word pair describes size, format and layout") {
    TextureInfo info;
    REQUIRE(describe_texture(make_tcw(0x8000, 1), make_tsp(2, 1), 0, info));
    CHECK(info.width == 32);
    CHECK(info.height == 16);
    CHECK(info.address == 0x8000);
    CHECK(info.format == PixelFormat::Rgb565);
    CHECK(info.twiddled);
    CHECK_FALSE(info.vq);
    CHECK(info.size_bytes() == 32 * 16 * 2);

    // Scan order is available to a plain 16-bit texture.
    REQUIRE(describe_texture(make_tcw(0, 0, /*twiddled=*/false), make_tsp(0, 0), 0, info));
    CHECK_FALSE(info.twiddled);
    // But an indexed texture is always twiddled, whatever the bit says.
    REQUIRE(describe_texture(make_tcw(0, 6, /*twiddled=*/false), make_tsp(0, 0), 0, info));
    CHECK(info.twiddled);
    CHECK(info.indexed());
    // As is a compressed one.
    REQUIRE(
        describe_texture(make_tcw(0, 0, /*twiddled=*/false, /*vq=*/true), make_tsp(0, 0), 0, info));
    CHECK(info.twiddled);

    // A palette selector picks a window of palette memory: 16 entries for 4-bit, 256 for 8-bit.
    REQUIRE(describe_texture(make_tcw(0, 5, true, false, false, /*palette_select=*/3),
                             make_tsp(0, 0), 0, info));
    CHECK(info.palette_base == 48);
    // An 8-bit texture has only four palette windows, so only the selector's top two bits count.
    REQUIRE(describe_texture(make_tcw(0, 6, true, false, false, /*palette_select=*/8),
                             make_tsp(0, 0), 0, info));
    CHECK(info.palette_base == 0);
    REQUIRE(describe_texture(make_tcw(0, 6, true, false, false, /*palette_select=*/32),
                             make_tsp(0, 0), 0, info));
    CHECK(info.palette_base == 512);

    // The reserved format is refused rather than guessed at.
    CHECK_FALSE(describe_texture(make_tcw(0, 7), make_tsp(0, 0), 0, info));
}

TEST_CASE("texture: the three 16-bit formats expand to full-range RGBA") {
    auto v = vram();
    TextureInfo info;
    // 1555: opaque white, then transparent black.
    REQUIRE(describe_texture(make_tcw(0, 0, /*twiddled=*/false), make_tsp(0, 0), 0, info));
    put16(v, 0, 0xFFFFu);
    put16(v, 2, 0x0000u);
    std::vector<u32> pixels;
    std::uint32_t palette[1024] = {};
    REQUIRE(decode_texture(info, v.data(), v.size(), palette, pixels));
    CHECK(pixels[0] == 0xFFFFFFFFu);  // opaque white
    CHECK((pixels[1] >> 24) == 0u);   // transparent

    // 565: full red has no alpha channel, so alpha is opaque.
    REQUIRE(describe_texture(make_tcw(0, 1, false), make_tsp(0, 0), 0, info));
    put16(v, 0, 0xF800u);
    REQUIRE(decode_texture(info, v.data(), v.size(), palette, pixels));
    CHECK(pixels[0] == 0xFF0000FFu);  // RGBA: r=255, a=255

    // 4444: each nibble scales by 17 so 0xF becomes 255.
    REQUIRE(describe_texture(make_tcw(0, 2, false), make_tsp(0, 0), 0, info));
    put16(v, 0, 0x8F00u);  // a=8, r=15, g=0, b=0
    REQUIRE(decode_texture(info, v.data(), v.size(), palette, pixels));
    CHECK((pixels[0] & 0xFFu) == 255u);          // red
    CHECK(((pixels[0] >> 24) & 0xFFu) == 136u);  // alpha 8 * 17
}

TEST_CASE("texture: a twiddled texture reads back in scan order") {
    auto v = vram();
    TextureInfo info;
    REQUIRE(describe_texture(make_tcw(0, 1, /*twiddled=*/true), make_tsp(1, 1), 0, info));
    REQUIRE(info.width == 16);
    REQUIRE(info.height == 16);
    // Store a value derived from the texel's position, at its twiddled offset.
    for (u32 y = 0; y < 16; ++y)
        for (u32 x = 0; x < 16; ++x)
            put16(v, twiddle_index(x, y, 16, 16) * 2, static_cast<std::uint16_t>((y << 8) | x));
    std::vector<u32> pixels;
    std::uint32_t palette[1024] = {};
    REQUIRE(decode_texture(info, v.data(), v.size(), palette, pixels));
    // Decoding must put each texel back where it belongs; check via the 565 green channel, which
    // carries the low bits of x.
    for (u32 y = 0; y < 16; ++y)
        for (u32 x = 0; x < 16; ++x) {
            const std::uint16_t expected = static_cast<std::uint16_t>((y << 8) | x);
            const u32 want_r = ((expected >> 11) & 0x1Fu);
            const u32 got_r = (pixels[y * 16 + x] & 0xFFu) >> 3;
            CHECK(got_r == want_r);
        }
}

TEST_CASE("texture: indexed formats look up palette memory") {
    auto v = vram();
    std::uint32_t palette_ram[1024] = {};
    // Palette memory in 8888: entry n is a distinct colour.
    for (unsigned i = 0; i < 1024; ++i) palette_ram[i] = 0xFF000000u | (i * 7u);
    std::uint32_t palette[1024];
    unpack_palette(palette_ram, PaletteFormat::Argb8888, palette);

    TextureInfo info;
    // 8-bit indexed, palette window 2 (entries 512..767): selectors 32 to 47 pick it.
    REQUIRE(describe_texture(make_tcw(0, 6, true, false, false, 32), make_tsp(0, 0), 0, info));
    REQUIRE(info.palette_base == 512);
    for (u32 y = 0; y < 8; ++y)
        for (u32 x = 0; x < 8; ++x)
            v[twiddle_index(x, y, 8, 8)] = static_cast<std::uint8_t>(y * 8 + x);
    std::vector<u32> pixels;
    REQUIRE(decode_texture(info, v.data(), v.size(), palette, pixels));
    for (u32 i = 0; i < 64; ++i) CHECK(pixels[i] == palette[512 + i]);

    // 4-bit indexed: two texels per byte, low nibble first.
    std::fill(v.begin(), v.end(), 0);
    REQUIRE(describe_texture(make_tcw(0, 5, true, false, false, 1), make_tsp(0, 0), 0, info));
    REQUIRE(info.palette_base == 16);
    for (u32 y = 0; y < 8; ++y)
        for (u32 x = 0; x < 8; ++x) {
            const u32 index = twiddle_index(x, y, 8, 8);
            const std::uint8_t value = static_cast<std::uint8_t>((y + x) & 0xFu);
            if (index & 1u)
                v[index / 2] = static_cast<std::uint8_t>((v[index / 2] & 0x0Fu) | (value << 4));
            else
                v[index / 2] = static_cast<std::uint8_t>((v[index / 2] & 0xF0u) | value);
        }
    REQUIRE(decode_texture(info, v.data(), v.size(), palette, pixels));
    for (u32 y = 0; y < 8; ++y)
        for (u32 x = 0; x < 8; ++x) CHECK(pixels[y * 8 + x] == palette[16 + ((y + x) & 0xFu)]);
}

TEST_CASE("texture: vector quantisation expands a codebook of 2x2 blocks") {
    auto v = vram();
    TextureInfo info;
    REQUIRE(describe_texture(make_tcw(0, 1, true, /*vq=*/true), make_tsp(1, 1), 0, info));
    REQUIRE(info.width == 16);
    // Codebook entry 5 holds four distinguishable 565 texels.
    const std::uint16_t block[4] = {0xF800u, 0x07E0u, 0x001Fu, 0xFFFFu};  // red, green, blue, white
    for (unsigned t = 0; t < 4; ++t) put16(v, 5 * 8 + t * 2, block[t]);
    // Every 2x2 block of the texture uses entry 5.
    const u32 indices = 256 * 8;
    for (u32 by = 0; by < 8; ++by)
        for (u32 bx = 0; bx < 8; ++bx) v[indices + twiddle_index(bx, by, 8, 8)] = 5;

    std::vector<u32> pixels;
    std::uint32_t palette[1024] = {};
    REQUIRE(decode_texture(info, v.data(), v.size(), palette, pixels));
    // Within a block the four texels are themselves twiddled: (0,0) (0,1) (1,0) (1,1).
    CHECK(pixels[0 * 16 + 0] == 0xFF0000FFu);  // red
    CHECK(pixels[1 * 16 + 0] == 0xFF00FF00u);  // green
    CHECK(pixels[0 * 16 + 1] == 0xFFFF0000u);  // blue
    CHECK(pixels[1 * 16 + 1] == 0xFFFFFFFFu);  // white
    // And the pattern repeats across the whole texture.
    CHECK(pixels[4 * 16 + 6] == 0xFF0000FFu);
}

TEST_CASE("texture: YUV pairs share their chroma") {
    auto v = vram();
    TextureInfo info;
    REQUIRE(describe_texture(make_tcw(0, 3, /*twiddled=*/false), make_tsp(0, 0), 0, info));
    // Neutral chroma and two different luminances: the result is grey, darker then lighter.
    for (u32 i = 0; i < 8 * 8 / 2; ++i) {
        v[i * 4 + 0] = 128;  // u
        v[i * 4 + 1] = 64;   // y0
        v[i * 4 + 2] = 128;  // v
        v[i * 4 + 3] = 192;  // y1
    }
    std::vector<u32> pixels;
    std::uint32_t palette[1024] = {};
    REQUIRE(decode_texture(info, v.data(), v.size(), palette, pixels));
    CHECK((pixels[0] & 0xFFu) == 64u);
    CHECK(((pixels[0] >> 8) & 0xFFu) == 64u);
    CHECK((pixels[1] & 0xFFu) == 192u);
    CHECK((pixels[0] >> 24) == 255u);
}

TEST_CASE("texture: a texture that would read past video memory is refused") {
    std::vector<std::uint8_t> tiny(64, 0);
    TextureInfo info;
    REQUIRE(describe_texture(make_tcw(0, 1, false), make_tsp(3, 3), 0, info));  // 64x64
    std::vector<u32> pixels;
    std::uint32_t palette[1024] = {};
    CHECK_FALSE(decode_texture(info, tiny.data(), tiny.size(), palette, pixels));
}

TEST_CASE("texture: the size a texture occupies bounds the cache's invalidation range") {
    // Used to decide whether a frame written back into video memory has overwritten a cached
    // texture. Guessing low there leaves a stale texture on screen, so a mipmapped texture counts
    // its whole chain and every bound rounds up.
    TextureInfo info;
    info.width = 64;
    info.height = 64;
    info.format = PixelFormat::Rgb565;
    info.mipmapped = false;
    CHECK(info.size_bytes() == 64u * 64u * 2u);
    info.mipmapped = true;
    CHECK(info.size_bytes() > 64u * 64u * 2u);
    CHECK(info.size_bytes() <= 64u * 64u * 2u * 3u / 2u);

    info.mipmapped = false;
    info.format = PixelFormat::Palette4;
    CHECK(info.size_bytes() == 64u * 64u / 2u);
    info.format = PixelFormat::Palette8;
    CHECK(info.size_bytes() == 64u * 64u);

    info.vq = true;
    info.format = PixelFormat::Rgb565;
    CHECK(info.size_bytes() == 256u * 8u + 64u * 64u / 4u);  // codebook plus one index per block
}
