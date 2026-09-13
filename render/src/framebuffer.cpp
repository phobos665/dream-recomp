// See framebuffer.h.
#include "dream/render/framebuffer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace dream::render {

namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;

// PVR register offsets from 0x005F8000, as 32-bit words.
constexpr std::size_t kFbRCtrl = 0x044 / 4;
constexpr std::size_t kFbWCtrl = 0x048 / 4;
constexpr std::size_t kFbRSof1 = 0x050 / 4;
constexpr std::size_t kFbRSize = 0x05C / 4;
constexpr std::size_t kFbWSof1 = 0x060 / 4;
// FB_W_LINESTRIDE sits between FB_W_CTRL and FB_R_SOF1, not after the write addresses. Reading it
// at 0x06C lands on FB_Y_CLIP, whose low bits are the clip's first line and so are normally zero:
// the wrong register gives a plausible answer, which is how it survives a glance.
constexpr std::size_t kFbWLineStride = 0x04C / 4;
constexpr std::size_t kFbXClip = 0x068 / 4;
constexpr std::size_t kFbYClip = 0x06C / 4;

constexpr u32 ctrl_enable(u32 w) {
    return w & 1u;
}
constexpr u32 ctrl_depth(u32 w) {
    return (w >> 2) & 3u;
}
constexpr u32 ctrl_concat(u32 w) {
    return (w >> 4) & 7u;
}
constexpr u32 ctrl_vclk_div(u32 w) {
    return (w >> 23) & 1u;
}

constexpr u32 size_x(u32 w) {
    return w & 0x3FFu;
}
constexpr u32 size_y(u32 w) {
    return (w >> 10) & 0x3FFu;
}
constexpr u32 size_modulus(u32 w) {
    return (w >> 20) & 0x3FFu;
}

u32 pack_rgba(u32 r, u32 g, u32 b, u32 a) {
    return r | (g << 8) | (b << 16) | (a << 24);
}

u16 read16(const u8* p) {
    u16 v;
    std::memcpy(&v, p, 2);
    return v;
}

unsigned bytes_per_pixel(FramebufferFormat f) {
    switch (f) {
        case FramebufferFormat::Rgb555:
        case FramebufferFormat::Rgb565:
        case FramebufferFormat::Argb4444:
            return 2;
        case FramebufferFormat::Rgb888:
            return 3;
        default:
            return 4;
    }
}

}  // namespace

std::string FramebufferInfo::describe() const {
    static const char* kNames[5] = {"RGB555", "RGB565", "RGB888", "ARGB8888", "ARGB4444"};
    const unsigned f = static_cast<unsigned>(format);
    char buf[160];
    std::snprintf(buf, sizeof buf, "%ux%u %s at 0x%06x, stride +%u%s", width, height,
                  kNames[f < 5 ? f : 0], address, modulus, enabled ? "" : " (display off)");
    return buf;
}

bool describe_framebuffer(const std::uint32_t* pvr_regs, FramebufferInfo& out) {
    out = FramebufferInfo{};
    const u32 ctrl = pvr_regs[kFbRCtrl];
    const u32 size = pvr_regs[kFbRSize];
    out.enabled = ctrl_enable(ctrl) != 0;
    out.format = static_cast<FramebufferFormat>(ctrl_depth(ctrl));
    out.concat = ctrl_concat(ctrl);
    out.address = pvr_regs[kFbRSof1] & 0x00FFFFFFu;

    // The width and the row stride are both counted in 16-bit words, whatever the pixel format,
    // so a 24- or 32-bit format covers fewer pixels in the same number of words.
    u32 width_words = (size_x(size) + 1u) * 2u;
    u32 modulus_words = size_modulus(size) == 0 ? 0u : (size_modulus(size) - 1u) * 2u;
    switch (out.format) {
        case FramebufferFormat::Rgb555:
        case FramebufferFormat::Rgb565:
        case FramebufferFormat::Argb4444:  // FB_R_CTRL's two-bit field cannot name this one
            out.width = width_words;
            out.modulus = modulus_words;
            break;
        case FramebufferFormat::Rgb888:
            out.width = width_words * 2u / 3u;
            out.modulus = modulus_words * 2u / 3u;
            break;
        case FramebufferFormat::Argb8888:
            out.width = width_words / 2u;
            out.modulus = modulus_words / 2u;
            break;
    }
    out.height = size_y(size) + 1u;
    // At the lower pixel clock the hardware scans 240 lines and the display doubles them.
    if (ctrl_vclk_div(ctrl) == 0)
        out.height = std::min(out.height, 240u);

    return out.width != 0 && out.height != 0 && out.width <= 1024 && out.height <= 1024;
}

bool decode_framebuffer(const FramebufferInfo& info, const std::uint8_t* vram,
                        std::size_t vram_size, std::vector<std::uint32_t>& out) {
    const unsigned bpp = bytes_per_pixel(info.format);
    const std::size_t needed = static_cast<std::size_t>(info.address) +
                               static_cast<std::size_t>(info.height) *
                                   (static_cast<std::size_t>(info.width) + info.modulus) * bpp;
    if (needed > vram_size)
        return false;

    out.assign(static_cast<std::size_t>(info.width) * info.height, 0);
    // A 16-bit channel widened to eight bits gets its low bits from the concat field rather than
    // from the channel itself, which is what the hardware does.
    const u32 concat = info.concat;
    std::size_t at = info.address;
    std::size_t dst = 0;
    for (u32 y = 0; y < info.height; ++y) {
        for (u32 x = 0; x < info.width; ++x, at += bpp) {
            switch (info.format) {
                case FramebufferFormat::Rgb555: {
                    const u16 p = read16(vram + at);
                    out[dst++] = pack_rgba((((p >> 10) & 0x1Fu) << 3) | concat,
                                           (((p >> 5) & 0x1Fu) << 3) | concat,
                                           ((p & 0x1Fu) << 3) | concat, 255u);
                    break;
                }
                case FramebufferFormat::Rgb565: {
                    const u16 p = read16(vram + at);
                    out[dst++] = pack_rgba((((p >> 11) & 0x1Fu) << 3) | concat,
                                           (((p >> 5) & 0x3Fu) << 2) | (concat >> 1),
                                           ((p & 0x1Fu) << 3) | concat, 255u);
                    break;
                }
                case FramebufferFormat::Argb4444: {
                    const u16 p = read16(vram + at);
                    const u32 r = (p >> 8) & 0xFu, g = (p >> 4) & 0xFu, b = p & 0xFu;
                    out[dst++] = pack_rgba(r * 17u, g * 17u, b * 17u, 255u);
                    break;
                }
                case FramebufferFormat::Rgb888:
                    out[dst++] = pack_rgba(vram[at + 2], vram[at + 1], vram[at], 255u);
                    break;
                case FramebufferFormat::Argb8888:
                    out[dst++] = pack_rgba(vram[at + 2], vram[at + 1], vram[at], 255u);
                    break;
            }
        }
        at += static_cast<std::size_t>(info.modulus) * bpp;
    }
    return true;
}

bool describe_write_framebuffer(const std::uint32_t* pvr_regs, std::uint32_t width,
                                std::uint32_t height, FramebufferInfo& out) {
    out = FramebufferInfo{};
    // FB_X_CLIP and FB_Y_CLIP are the region the hardware is allowed to write, so they size the
    // written frame better than the display does. The caller's size is the fallback for a title
    // that leaves them at zero.
    const u32 x_max = (pvr_regs[kFbXClip] >> 16) & 0x7FFu;
    const u32 y_max = (pvr_regs[kFbYClip] >> 16) & 0x3FFu;
    if (x_max != 0 && y_max != 0) {
        width = x_max + 1u;
        height = y_max + 1u;
    }
    if (width == 0 || height == 0)
        return false;
    // FB_W_CTRL names seven pixel formats where FB_R_CTRL names four, and numbers them
    // differently: two of them carry alpha in sixteen bits and two more are thirty-two bits with
    // and without it. Rendering does not use the alpha once the frame is on screen, so the two
    // 32-bit formats and the two 1555-shaped ones collapse together.
    static const FramebufferFormat kPack[7] = {
        FramebufferFormat::Rgb555,    // 0: KRGB1555
        FramebufferFormat::Rgb565,    // 1: RGB565
        FramebufferFormat::Argb4444,  // 2: ARGB4444
        FramebufferFormat::Rgb555,    // 3: ARGB1555
        FramebufferFormat::Rgb888,    // 4: RGB888
        FramebufferFormat::Argb8888,  // 5: KRGB0888
        FramebufferFormat::Argb8888,  // 6: ARGB8888
    };
    const u32 pack = pvr_regs[kFbWCtrl] & 7u;
    if (pack > 6)
        return false;  // 7 is reserved; the hardware does not define what it writes
    out.format = kPack[pack];
    out.address = pvr_regs[kFbWSof1] & 0x00FFFFFFu;
    out.width = width;
    out.height = height;
    out.enabled = true;

    // FB_W_LINESTRIDE is the distance from one row to the next in eight-byte units. Zero means
    // the rows are packed, which is what a title that renders straight to the display buffer
    // leaves it at.
    const u32 stride_bytes = (pvr_regs[kFbWLineStride] & 0x1FFu) * 8u;
    const unsigned bpp = bytes_per_pixel(out.format);
    const u32 row_pixels = stride_bytes / bpp;
    out.modulus = row_pixels > width ? row_pixels - width : 0u;
    return true;
}

RenderTarget describe_render_target(const std::uint32_t* pvr_regs) {
    RenderTarget rt;
    rt.address = pvr_regs[kFbWSof1] & 0x00FFFFFFu;
    rt.same_as_display = rt.address == (pvr_regs[kFbRSof1] & 0x00FFFFFFu);
    return rt;
}

bool encode_framebuffer(const FramebufferInfo& info, const std::uint32_t* rgba,
                        std::uint32_t src_width, std::uint32_t src_height, std::uint8_t* vram,
                        std::size_t vram_size) {
    const unsigned bpp = bytes_per_pixel(info.format);
    const std::size_t needed = static_cast<std::size_t>(info.address) +
                               static_cast<std::size_t>(info.height) *
                                   (static_cast<std::size_t>(info.width) + info.modulus) * bpp;
    if (needed > vram_size || src_width == 0 || src_height == 0)
        return false;

    std::size_t at = info.address;
    for (u32 y = 0; y < info.height; ++y) {
        // Nearest-neighbour when the rendered image is a different size from the guest's
        // framebuffer, which is what an increased internal resolution will produce.
        const u32 sy = src_height == info.height ? y : y * src_height / info.height;
        for (u32 x = 0; x < info.width; ++x, at += bpp) {
            const u32 sx = src_width == info.width ? x : x * src_width / info.width;
            const u32 p = rgba[static_cast<std::size_t>(sy) * src_width + sx];
            const u32 r = p & 0xFFu, g = (p >> 8) & 0xFFu, b = (p >> 16) & 0xFFu;
            switch (info.format) {
                case FramebufferFormat::Rgb555: {
                    const u16 v = static_cast<u16>(((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
                    std::memcpy(vram + at, &v, 2);
                    break;
                }
                case FramebufferFormat::Rgb565: {
                    const u16 v = static_cast<u16>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
                    std::memcpy(vram + at, &v, 2);
                    break;
                }
                case FramebufferFormat::Argb4444: {
                    const u16 v =
                        static_cast<u16>(0xF000u | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4));
                    std::memcpy(vram + at, &v, 2);
                    break;
                }
                case FramebufferFormat::Rgb888:
                    vram[at] = static_cast<u8>(b);
                    vram[at + 1] = static_cast<u8>(g);
                    vram[at + 2] = static_cast<u8>(r);
                    break;
                case FramebufferFormat::Argb8888:
                    vram[at] = static_cast<u8>(b);
                    vram[at + 1] = static_cast<u8>(g);
                    vram[at + 2] = static_cast<u8>(r);
                    vram[at + 3] = 0xFF;
                    break;
            }
        }
        at += static_cast<std::size_t>(info.modulus) * bpp;
    }
    return true;
}

}  // namespace dream::render
