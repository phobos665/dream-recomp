// Text drawn over the frame: the on-screen frame-rate counter, and anything else a development
// build wants to say without a console.
//
// Device-independent on purpose, like the rest of this layer. Stamping pixels into an RGBA buffer
// needs no Vulkan, so the glyphs are unit-tested on every CI runner instead of being judged by
// squinting at a screenshot. It also means the counter appears in screenshots and frame captures,
// which is what you want in a bug report: the picture says how fast it was going.
//
// The font is 5x7 in a 6x8 cell, upper case and digits only, which is what a counter needs and
// what the period's own hardware would have had.
#pragma once

#include <cstdint>
#include <string_view>

namespace dream::render {

constexpr unsigned kGlyphWidth = 5, kGlyphHeight = 7;
constexpr unsigned kCellWidth = 6, kCellHeight = 8;  // one pixel of gap, right and below

struct TextStyle {
    std::uint32_t fg = 0xFFFFFFFFu;  // RGBA, red in the low byte, as the framebuffer holds it
    std::uint32_t bg = 0xC0000000u;  // drawn behind the text so it reads over any picture
    unsigned scale = 2;              // whole-pixel scaling; the counter is unreadable at 1 on a
                                     // 1280x960 window and silly at 4 on a 640x480 one
};

// Pixels a string occupies, including the cell gap after the last glyph.
unsigned text_width(std::string_view text, unsigned scale);
unsigned text_height(unsigned scale);

// Fills a rectangle, clipped to the buffer. `pitch` is pixels per row.
void fill_rect(std::uint32_t* rgba, unsigned pitch, unsigned width, unsigned height, int x, int y,
               int w, int h, std::uint32_t colour);

// Draws `text` with its top-left corner at (x, y), clipped to the buffer. Characters the font does
// not carry are drawn as a blank cell rather than dropped, so a string's width never depends on
// what is in it. Lower case is folded to upper.
void draw_text(std::uint32_t* rgba, unsigned pitch, unsigned width, unsigned height, int x, int y,
               std::string_view text, const TextStyle& style = {});

}  // namespace dream::render
