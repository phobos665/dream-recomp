// Pictures for the settings screen, and the shapes drawn over them.
//
// Device-independent like overlay.h: pixels are RGBA with red in the low byte, as the frame the
// overlay draws into holds them, and nothing here knows about files or SDL. The launcher decodes a
// PNG through SDL and hands the pixels over; this layer only scales and composites them.
#pragma once

#include <cstdint>
#include <vector>

namespace dream::render {

struct Image {
    unsigned width = 0, height = 0;
    std::vector<std::uint32_t> rgba;  // width * height, rows top to bottom
    bool empty() const noexcept { return width == 0 || height == 0 || rgba.empty(); }
};

// The largest copy that fits in max_w x max_h with the aspect kept. Shrinking averages every source
// pixel an output pixel covers, weighted by alpha, so a photo cut out of its background keeps
// clean edges instead of a white fringe; growing repeats pixels.
Image scale_to_fit(const Image& src, unsigned max_w, unsigned max_h);

// Composites `img` with its top-left corner at (x, y), honouring its alpha, clipped to the buffer.
void blit(std::uint32_t* rgba, unsigned pitch, unsigned width, unsigned height, int x, int y,
          const Image& img);

// Like fill_rect, but blended with the colour's own alpha rather than overwriting.
void blend_rect(std::uint32_t* rgba, unsigned pitch, unsigned width, unsigned height, int x, int y,
                int w, int h, std::uint32_t colour);

// A filled disc and a ring, blended. For marking a control's place on a picture.
void fill_disc(std::uint32_t* rgba, unsigned pitch, unsigned width, unsigned height, int cx, int cy,
               int r, std::uint32_t colour);
void draw_ring(std::uint32_t* rgba, unsigned pitch, unsigned width, unsigned height, int cx, int cy,
               int r, int thickness, std::uint32_t colour);

}  // namespace dream::render
