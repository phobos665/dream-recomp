// See texture.h.
#include "dream/render/texture.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace dream::render {

namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using s32 = std::int32_t;

// Texture control word (Flycast's TCW union).
constexpr u32 tcw_address(u32 w) {
    return (w & 0x1FFFFFu) << 3;
}  // in 8-byte units
constexpr u32 tcw_stride_sel(u32 w) {
    return (w >> 25) & 1u;
}
constexpr u32 tcw_scan_order(u32 w) {
    return (w >> 26) & 1u;
}
constexpr u32 tcw_pixel_format(u32 w) {
    return (w >> 27) & 7u;
}
constexpr u32 tcw_vq(u32 w) {
    return (w >> 30) & 1u;
}
constexpr u32 tcw_mipmapped(u32 w) {
    return (w >> 31) & 1u;
}
constexpr u32 tcw_palette_select(u32 w) {
    return (w >> 21) & 0x3Fu;
}

// TSP instruction word: the texture's size.
constexpr u32 tsp_tex_v(u32 w) {
    return (w >> 0) & 7u;
}
constexpr u32 tsp_tex_u(u32 w) {
    return (w >> 3) & 7u;
}

u32 pack_rgba(u32 r, u32 g, u32 b, u32 a) {
    return r | (g << 8) | (b << 16) | (a << 24);
}

// The 16-bit formats. Each expands the high bits downwards so that all-ones maps to 255.
u32 unpack_1555(u16 p) {
    const u32 a = (p >> 15) & 1u;
    const u32 r = (p >> 10) & 0x1Fu, g = (p >> 5) & 0x1Fu, b = p & 0x1Fu;
    return pack_rgba((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), a ? 255u : 0u);
}
u32 unpack_565(u16 p) {
    const u32 r = (p >> 11) & 0x1Fu, g = (p >> 5) & 0x3Fu, b = p & 0x1Fu;
    return pack_rgba((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255u);
}
u32 unpack_4444(u16 p) {
    const u32 a = (p >> 12) & 0xFu, r = (p >> 8) & 0xFu, g = (p >> 4) & 0xFu, b = p & 0xFu;
    return pack_rgba(r * 17u, g * 17u, b * 17u, a * 17u);
}
u32 unpack_8888(u32 p) {
    return pack_rgba((p >> 16) & 0xFFu, (p >> 8) & 0xFFu, p & 0xFFu, (p >> 24) & 0xFFu);
}

// YUV 4:2:2: two texels share a pair of chroma samples, packed as u0 y0 v0 y1 across four bytes.
// The coefficients are Flycast's, which came from the hardware's own conversion.
u32 yuv_to_rgba(s32 y, s32 u, s32 v) {
    u -= 128;
    v -= 128;
    const s32 r = y + v * 11 / 8;
    const s32 g = y - (u * 11 + v * 22) / 32;
    const s32 b = y + u * 110 / 64;
    return pack_rgba(static_cast<u32>(std::clamp(r, 0, 255)),
                     static_cast<u32>(std::clamp(g, 0, 255)),
                     static_cast<u32>(std::clamp(b, 0, 255)), 255u);
}

u16 read16(const u8* p) {
    u16 v;
    std::memcpy(&v, p, 2);
    return v;
}

}  // namespace

// The twiddled order interleaves the bits of y and x, y first, taking bits only while each
// dimension still has some left. For a square texture this is the familiar Morton order; for a
// rectangular one the longer dimension's remaining bits follow.
std::uint32_t twiddle_index(std::uint32_t x, std::uint32_t y, std::uint32_t width,
                            std::uint32_t height) {
    u32 rv = 0, shift = 0;
    u32 xs = width >> 1, ys = height >> 1;
    while (xs != 0 || ys != 0) {
        if (ys != 0) {
            rv |= (y & 1u) << shift;
            ys >>= 1;
            y >>= 1;
            ++shift;
        }
        if (xs != 0) {
            rv |= (x & 1u) << shift;
            xs >>= 1;
            x >>= 1;
            ++shift;
        }
    }
    return rv;
}

std::uint32_t TextureInfo::size_bytes() const noexcept {
    u32 texels = width * height;
    // A mipmapped texture is followed by its whole chain, each level a quarter of the last, which
    // converges on a third more texels. Rounded up: this bounds an invalidation range, and
    // guessing low there means a stale texture, which is the worse way to be wrong.
    if (mipmapped)
        texels += texels / 3u + 1u;
    if (vq)
        return 256u * 8u + texels / 4u;  // codebook plus one index per 2x2 block
    switch (format) {
        case PixelFormat::Palette4:
            return texels / 2u;
        case PixelFormat::Palette8:
            return texels;
        case PixelFormat::Argb1555:
        case PixelFormat::Rgb565:
        case PixelFormat::Argb4444:
        case PixelFormat::Yuv422:
        case PixelFormat::BumpMap:
            return texels * 2u;
        default:
            return 0;
    }
}

std::string TextureInfo::describe() const {
    static const char* kFormats[8] = {"1555", "565",  "4444", "yuv422",
                                      "bump", "pal4", "pal8", "reserved"};
    char buf[160];
    std::snprintf(buf, sizeof buf, "%ux%u %s%s%s%s at 0x%06x", width, height,
                  kFormats[static_cast<unsigned>(format) & 7u], twiddled ? " twiddled" : " linear",
                  vq ? " vq" : "", mipmapped ? " mipmapped" : "", address);
    return buf;
}

bool describe_texture(std::uint32_t tcw, std::uint32_t tsp, std::uint32_t stride_width,
                      TextureInfo& out) {
    out = TextureInfo{};
    out.address = tcw_address(tcw);
    out.format = static_cast<PixelFormat>(tcw_pixel_format(tcw));
    out.vq = tcw_vq(tcw) != 0;
    out.mipmapped = tcw_mipmapped(tcw) != 0;
    out.stride = tcw_stride_sel(tcw) != 0;
    out.width = 8u << tsp_tex_u(tsp);
    out.height = 8u << tsp_tex_v(tsp);
    // Only non-palette, non-compressed textures may be in scan order; the bit is ignored
    // otherwise, and a stride texture is always in scan order.
    const bool can_be_linear = !out.vq && !out.indexed();
    out.twiddled = !(can_be_linear && (tcw_scan_order(tcw) != 0 || out.stride));
    if (out.stride && stride_width != 0)
        out.width = stride_width;

    if (out.format == PixelFormat::Palette4)
        out.palette_base = tcw_palette_select(tcw) << 4;
    else if (out.format == PixelFormat::Palette8)
        // Only the top two bits of the selector matter for an 8-bit texture: palette memory holds
        // four windows of 256 entries, against sixty-four windows of 16 for a 4-bit one.
        out.palette_base = (tcw_palette_select(tcw) >> 4) << 8;

    if (out.format == PixelFormat::Reserved)
        return false;
    if (out.width == 0 || out.height == 0 || out.width > 1024 || out.height > 1024)
        return false;
    // A mipmapped texture must be square and a power of two; the hardware has no other kind.
    if (out.mipmapped && out.width != out.height)
        return false;
    return true;
}

void unpack_palette(const std::uint32_t* palette_ram, PaletteFormat format, std::uint32_t* out) {
    for (unsigned i = 0; i < 1024; ++i) {
        const u32 w = palette_ram[i];
        switch (format) {
            case PaletteFormat::Argb1555:
                out[i] = unpack_1555(static_cast<u16>(w));
                break;
            case PaletteFormat::Rgb565:
                out[i] = unpack_565(static_cast<u16>(w));
                break;
            case PaletteFormat::Argb4444:
                out[i] = unpack_4444(static_cast<u16>(w));
                break;
            case PaletteFormat::Argb8888:
                out[i] = unpack_8888(w);
                break;
        }
    }
}

namespace {

// Where the base level of a mipmapped texture starts. The hardware stores the chain smallest
// first, each level a quarter of the next, with a small fixed offset the size of one texel.
u32 mipmap_base_offset(const TextureInfo& info) {
    if (!info.mipmapped)
        return 0;
    u32 offset = 0;
    for (u32 size = 1; size < info.width; size <<= 1) {
        u32 texels = size * size;
        if (info.vq)
            texels /= 4;  // one index byte per 2x2 block
        else if (info.format == PixelFormat::Palette4)
            texels /= 2;
        else if (info.format != PixelFormat::Palette8)
            texels *= 2;
        offset += texels;
    }
    // The chain begins after a single texel's worth of padding.
    return offset + (info.vq ? 1u : (info.format == PixelFormat::Palette8 ? 1u : 2u));
}

u32 unpack_16(PixelFormat format, u16 p) {
    switch (format) {
        case PixelFormat::Argb1555:
            return unpack_1555(p);
        case PixelFormat::Rgb565:
            return unpack_565(p);
        case PixelFormat::Argb4444:
            return unpack_4444(p);
        // A bump map stores a normal as two angles. Until lighting uses it, showing it as a flat
        // mid-grey is honest: it is not colour data and interpreting it as colour looks wrong.
        case PixelFormat::BumpMap:
            return pack_rgba(128, 128, 255, 255);
        default:
            return 0;
    }
}

}  // namespace

bool decode_texture(const TextureInfo& info, const std::uint8_t* vram, std::size_t vram_size,
                    const std::uint32_t* palette, std::vector<std::uint32_t>& out) {
    const u32 w = info.width, h = info.height;
    out.assign(static_cast<std::size_t>(w) * h, 0);
    if (info.format == PixelFormat::Reserved)
        return false;

    const u32 base = info.address + mipmap_base_offset(info);
    const auto in_range = [&](u32 offset, u32 bytes) {
        return static_cast<std::size_t>(offset) + bytes <= vram_size;
    };

    if (info.vq) {
        // The 256-entry codebook sits at the texture's address and the block indices follow it,
        // so the index data starts 2 KB in (plus the mipmap chain when there is one).
        const u32 codebook = info.address;
        const u32 indices = info.address + 256u * 8u + mipmap_base_offset(info);
        if (!in_range(codebook, 256 * 8))
            return false;
        const u32 blocks_x = w / 2, blocks_y = h / 2;
        for (u32 by = 0; by < blocks_y; ++by) {
            for (u32 bx = 0; bx < blocks_x; ++bx) {
                const u32 index_offset =
                    indices + (info.twiddled ? twiddle_index(bx, by, blocks_x, blocks_y)
                                             : by * blocks_x + bx);
                if (!in_range(index_offset, 1))
                    return false;
                const u8* entry =
                    vram + codebook + static_cast<std::size_t>(vram[index_offset]) * 8;
                // Within a codebook entry the four texels are themselves in twiddled order:
                // (0,0), (0,1), (1,0), (1,1).
                for (u32 t = 0; t < 4; ++t) {
                    const u32 tx = t >> 1, ty = t & 1u;
                    const u32 x = bx * 2 + tx, y = by * 2 + ty;
                    out[static_cast<std::size_t>(y) * w + x] =
                        unpack_16(info.format, read16(entry + t * 2));
                }
            }
        }
        return true;
    }

    switch (info.format) {
        case PixelFormat::Palette4: {
            if (!in_range(base, w * h / 2))
                return false;
            for (u32 y = 0; y < h; ++y)
                for (u32 x = 0; x < w; ++x) {
                    const u32 index = info.twiddled ? twiddle_index(x, y, w, h) : y * w + x;
                    const u8 byte = vram[base + index / 2];
                    const u32 nibble = (index & 1u) ? (byte >> 4) : (byte & 0xFu);
                    out[static_cast<std::size_t>(y) * w + x] = palette[info.palette_base + nibble];
                }
            return true;
        }
        case PixelFormat::Palette8: {
            if (!in_range(base, w * h))
                return false;
            for (u32 y = 0; y < h; ++y)
                for (u32 x = 0; x < w; ++x) {
                    const u32 index = info.twiddled ? twiddle_index(x, y, w, h) : y * w + x;
                    out[static_cast<std::size_t>(y) * w + x] =
                        palette[info.palette_base + vram[base + index]];
                }
            return true;
        }
        case PixelFormat::Yuv422: {
            if (!in_range(base, w * h * 2))
                return false;
            // Two texels per four bytes. Twiddled YUV is stored as pairs, so the pair index is
            // twiddled over half the width.
            for (u32 y = 0; y < h; ++y)
                for (u32 x = 0; x < w; x += 2) {
                    const u32 pair =
                        info.twiddled ? twiddle_index(x / 2, y, w / 2, h) : (y * w + x) / 2;
                    const u8* p = vram + base + static_cast<std::size_t>(pair) * 4;
                    const s32 u = p[0], y0 = p[1], v = p[2], y1 = p[3];
                    out[static_cast<std::size_t>(y) * w + x] = yuv_to_rgba(y0, u, v);
                    if (x + 1 < w)
                        out[static_cast<std::size_t>(y) * w + x + 1] = yuv_to_rgba(y1, u, v);
                }
            return true;
        }
        case PixelFormat::Argb1555:
        case PixelFormat::Rgb565:
        case PixelFormat::Argb4444:
        case PixelFormat::BumpMap: {
            if (!in_range(base, w * h * 2))
                return false;
            for (u32 y = 0; y < h; ++y)
                for (u32 x = 0; x < w; ++x) {
                    const u32 index = info.twiddled ? twiddle_index(x, y, w, h) : y * w + x;
                    out[static_cast<std::size_t>(y) * w + x] = unpack_16(
                        info.format, read16(vram + base + static_cast<std::size_t>(index) * 2));
                }
            return true;
        }
        default:
            return false;
    }
}

}  // namespace dream::render
