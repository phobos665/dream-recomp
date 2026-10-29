#include "dream/render/image.h"

#include <algorithm>

namespace dream::render {
namespace {
using u32 = std::uint32_t;
using u64 = std::uint64_t;

u32 channel(u32 c, unsigned shift) {
    return (c >> shift) & 0xFFu;
}

// Source over destination with the source's alpha; the destination stays opaque, as a frame is.
u32 blend(u32 dst, u32 src) {
    const u32 a = channel(src, 24);
    if (a == 0)
        return dst;
    if (a == 255)
        return src | 0xFF000000u;
    const u32 ia = 255 - a;
    auto mix = [&](unsigned shift) {
        return ((channel(src, shift) * a + channel(dst, shift) * ia + 127) / 255) << shift;
    };
    return mix(0) | mix(8) | mix(16) | 0xFF000000u;
}

void plot(u32* rgba, unsigned pitch, unsigned width, unsigned height, int x, int y, u32 colour) {
    if (x < 0 || y < 0 || x >= static_cast<int>(width) || y >= static_cast<int>(height))
        return;
    u32& d = rgba[static_cast<std::size_t>(y) * pitch + static_cast<unsigned>(x)];
    d = blend(d, colour);
}

}  // namespace

Image scale_to_fit(const Image& src, unsigned max_w, unsigned max_h) {
    Image out;
    if (src.empty() || max_w == 0 || max_h == 0)
        return out;
    // Whichever side is tighter decides, so the picture keeps its shape.
    const u64 by_w = static_cast<u64>(max_w) * src.height,
              by_h = static_cast<u64>(max_h) * src.width;
    if (by_w <= by_h) {
        out.width = max_w;
        out.height = static_cast<unsigned>(std::max<u64>(1, by_w / src.width));
    } else {
        out.height = max_h;
        out.width = static_cast<unsigned>(std::max<u64>(1, by_h / src.height));
    }
    out.rgba.assign(static_cast<std::size_t>(out.width) * out.height, 0);
    for (unsigned y = 0; y < out.height; ++y) {
        const unsigned y0 = static_cast<unsigned>(static_cast<u64>(y) * src.height / out.height);
        const unsigned y1 = std::max(
            y0 + 1, static_cast<unsigned>(static_cast<u64>(y + 1) * src.height / out.height));
        for (unsigned x = 0; x < out.width; ++x) {
            const unsigned x0 = static_cast<unsigned>(static_cast<u64>(x) * src.width / out.width);
            const unsigned x1 = std::max(
                x0 + 1, static_cast<unsigned>(static_cast<u64>(x + 1) * src.width / out.width));
            // Premultiplied sums: a transparent neighbour must not pull a colour towards black.
            u64 r = 0, g = 0, b = 0, a = 0, n = 0;
            for (unsigned sy = y0; sy < y1 && sy < src.height; ++sy)
                for (unsigned sx = x0; sx < x1 && sx < src.width; ++sx) {
                    const u32 p = src.rgba[static_cast<std::size_t>(sy) * src.width + sx];
                    const u32 pa = channel(p, 24);
                    r += channel(p, 0) * pa;
                    g += channel(p, 8) * pa;
                    b += channel(p, 16) * pa;
                    a += pa;
                    ++n;
                }
            u32& o = out.rgba[static_cast<std::size_t>(y) * out.width + x];
            if (a == 0 || n == 0)
                continue;
            o = static_cast<u32>(r / a) | static_cast<u32>(g / a) << 8 |
                static_cast<u32>(b / a) << 16 | static_cast<u32>(a / n) << 24;
        }
    }
    return out;
}

void blit(u32* rgba, unsigned pitch, unsigned width, unsigned height, int x, int y,
          const Image& img) {
    if (!rgba || img.empty())
        return;
    for (unsigned iy = 0; iy < img.height; ++iy) {
        const int dy = y + static_cast<int>(iy);
        if (dy < 0 || dy >= static_cast<int>(height))
            continue;
        for (unsigned ix = 0; ix < img.width; ++ix) {
            const int dx = x + static_cast<int>(ix);
            if (dx < 0 || dx >= static_cast<int>(width))
                continue;
            u32& d = rgba[static_cast<std::size_t>(dy) * pitch + static_cast<unsigned>(dx)];
            d = blend(d, img.rgba[static_cast<std::size_t>(iy) * img.width + ix]);
        }
    }
}

void blend_rect(u32* rgba, unsigned pitch, unsigned width, unsigned height, int x, int y, int w,
                int h, u32 colour) {
    if (!rgba)
        return;
    const int x0 = std::max(0, x), y0 = std::max(0, y);
    const int x1 = std::min(static_cast<int>(width), x + w);
    const int y1 = std::min(static_cast<int>(height), y + h);
    for (int py = y0; py < y1; ++py)
        for (int px = x0; px < x1; ++px) {
            u32& d = rgba[static_cast<std::size_t>(py) * pitch + static_cast<unsigned>(px)];
            d = blend(d, colour);
        }
}

void fill_disc(u32* rgba, unsigned pitch, unsigned width, unsigned height, int cx, int cy, int r,
               u32 colour) {
    if (!rgba || r <= 0)
        return;
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx)
            if (dx * dx + dy * dy <= r * r)
                plot(rgba, pitch, width, height, cx + dx, cy + dy, colour);
}

void draw_ring(u32* rgba, unsigned pitch, unsigned width, unsigned height, int cx, int cy, int r,
               int thickness, u32 colour) {
    if (!rgba || r <= 0)
        return;
    const int inner = std::max(0, r - thickness);
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx) {
            const int d2 = dx * dx + dy * dy;
            if (d2 <= r * r && d2 > inner * inner)
                plot(rgba, pitch, width, height, cx + dx, cy + dy, colour);
        }
}

}  // namespace dream::render
