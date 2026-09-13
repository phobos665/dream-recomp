// PowerVR2 texture decoding (WP2.3 step 4). Device-independent: video memory and a palette in,
// 32-bit RGBA out, so it is unit-tested on every CI runner with no GPU.
//
// A texture is described by two hardware words. The texture control word says where it is and how
// its pixels are encoded; the TSP instruction word says how big it is and how it is sampled. Four
// things vary independently and all four combine:
//
//   * the pixel format: 16-bit ARGB1555, RGB565 or ARGB4444, YUV422, a bump map, or a 4- or 8-bit
//     index into palette memory;
//   * the layout: twiddled (a Morton order that keeps nearby texels near in memory) or a plain
//     scan order, which is only available to non-palette, non-compressed textures;
//   * vector quantisation, which stores a 256-entry codebook of 2x2 blocks and then one index
//     byte per block;
//   * mipmaps, which put the smaller levels before the base level.
//
// Reference: Flycast's core/rend/texconv.cpp and TexCache.cpp (GPL-2.0, ADR 1).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dream::render {

enum class PixelFormat : unsigned {
    Argb1555 = 0,
    Rgb565 = 1,
    Argb4444 = 2,
    Yuv422 = 3,
    BumpMap = 4,
    Palette4 = 5,
    Palette8 = 6,
    Reserved = 7
};

// How palette memory itself is encoded (the PAL_RAM_CTRL register).
enum class PaletteFormat : unsigned { Argb1555 = 0, Rgb565 = 1, Argb4444 = 2, Argb8888 = 3 };

// Everything the decoder needs, worked out from the two hardware words.
struct TextureInfo {
    std::uint32_t address = 0;  // byte offset into the 64-bit view of video memory
    std::uint32_t width = 8, height = 8;
    PixelFormat format = PixelFormat::Argb1555;
    bool twiddled = true;
    bool vq = false;
    bool mipmapped = false;
    bool stride = false;
    std::uint32_t palette_base = 0;  // first palette entry for an indexed texture

    bool indexed() const noexcept {
        return format == PixelFormat::Palette4 || format == PixelFormat::Palette8;
    }
    // Bytes the texture occupies, for the cache's invalidation range. Zero when unknown.
    std::uint32_t size_bytes() const noexcept;
    std::string describe() const;
};

// `stride_width` is the width in texels from the TEXT_CONTROL register, used only by stride
// textures. Returns false when the words describe something the decoder cannot handle.
bool describe_texture(std::uint32_t tcw, std::uint32_t tsp, std::uint32_t stride_width,
                      TextureInfo& out);

// Decodes into `out` as RGBA8888, `width * height` texels, row 0 at the top. `palette` is the
// 1024-entry palette memory already unpacked to RGBA8888 (see unpack_palette). Returns false when
// the texture would read outside video memory or the format is not supported.
bool decode_texture(const TextureInfo& info, const std::uint8_t* vram, std::size_t vram_size,
                    const std::uint32_t* palette, std::vector<std::uint32_t>& out);

// Unpacks the 1024 words of palette memory into RGBA8888.
void unpack_palette(const std::uint32_t* palette_ram, PaletteFormat format, std::uint32_t* out);

// The twiddled (Morton) offset of a texel, in texels. Exposed for the tests.
std::uint32_t twiddle_index(std::uint32_t x, std::uint32_t y, std::uint32_t width,
                            std::uint32_t height);

}  // namespace dream::render
