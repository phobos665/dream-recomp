// The on-screen counter's glyphs, checked as pixels. A font is exactly the kind of thing that
// looks fine in a screenshot while being a row out, and exactly the kind of thing nobody re-checks
// once it has been seen working, so it is pinned here instead.
#include <vector>

#include "dream/render/overlay.h"

#include "doctest.h"

using namespace dream::render;
using u32 = std::uint32_t;

namespace {

constexpr unsigned kW = 64, kH = 32;
constexpr u32 kBlack = 0xFF000000u, kWhite = 0xFFFFFFFFu;

std::vector<u32> canvas() {
    return std::vector<u32>(kW * kH, kBlack);
}
bool lit(const std::vector<u32>& c, unsigned x, unsigned y) {
    return c[y * kW + x] == kWhite;
}

}  // namespace

TEST_CASE("a glyph lands where the font says it does") {
    auto c = canvas();
    // No background panel, scale 1: one pixel per font pixel, so the buffer is the font.
    draw_text(c.data(), kW, kW, kH, 0, 0, "I", TextStyle{kWhite, 0, 1});
    // 'I' is ".###." over five rows of "..#.." and a closing ".###.".
    CHECK(lit(c, 1, 0));
    CHECK(lit(c, 2, 0));
    CHECK(lit(c, 3, 0));
    CHECK_FALSE(lit(c, 0, 0));
    CHECK_FALSE(lit(c, 4, 0));
    for (unsigned row = 1; row <= 5; ++row) {
        CHECK(lit(c, 2, row));
        CHECK_FALSE(lit(c, 1, row));
    }
    CHECK(lit(c, 1, 6));
    CHECK(lit(c, 3, 6));
    // Nothing is drawn in the cell's gap column or row.
    for (unsigned row = 0; row < kGlyphHeight; ++row) CHECK_FALSE(lit(c, 5, row));
    for (unsigned col = 0; col < kCellWidth; ++col) CHECK_FALSE(lit(c, col, 7));
}

TEST_CASE("scaling is whole pixels, and advances by whole cells") {
    auto c = canvas();
    draw_text(c.data(), kW, kW, kH, 0, 0, "I", TextStyle{kWhite, 0, 2});
    // Every font pixel becomes a 2x2 block: the top bar now covers x 2..7 on rows 0 and 1.
    for (unsigned x = 2; x < 8; ++x) {
        CHECK(lit(c, x, 0));
        CHECK(lit(c, x, 1));
    }
    CHECK_FALSE(lit(c, 1, 0));
    CHECK_FALSE(lit(c, 8, 0));
    CHECK(text_width("I", 2) == kCellWidth * 2);
    CHECK(text_width("II", 2) == 2 * kCellWidth * 2);
    CHECK(text_height(2) == kCellHeight * 2);
}

TEST_CASE("width does not depend on which characters are in the string") {
    // A counter that reflows as its digits change is worse than no counter, so an unknown
    // character costs a blank cell rather than nothing.
    CHECK(text_width("60.0 FPS", 2) == text_width("59.8 FPS", 2));
    CHECK(text_width("~~~", 2) == text_width("ABC", 2));
    auto c = canvas();
    draw_text(c.data(), kW, kW, kH, 0, 0, "~I", TextStyle{kWhite, 0, 1});
    CHECK(lit(c, kCellWidth + 2, 1));  // the I is in the second cell, not the first
    CHECK_FALSE(lit(c, 2, 1));
}

TEST_CASE("lower case is folded, not dropped") {
    auto upper = canvas(), lower = canvas();
    draw_text(upper.data(), kW, kW, kH, 0, 0, "FPS", TextStyle{kWhite, 0, 1});
    draw_text(lower.data(), kW, kW, kH, 0, 0, "fps", TextStyle{kWhite, 0, 1});
    CHECK(upper == lower);
}

TEST_CASE("drawing off every edge clips instead of corrupting memory") {
    // Under a sanitiser this is the whole test: a counter positioned from a window size can end up
    // anywhere when the window is small or being resized.
    auto c = canvas();
    const auto before = c;
    draw_text(c.data(), kW, kW, kH, -1000, -1000, "CLIPPED");
    draw_text(c.data(), kW, kW, kH, 1000, 1000, "CLIPPED");
    CHECK(c == before);
    // Straddling an edge draws the part that is inside and nothing else.
    draw_text(c.data(), kW, kW, kH, -3, 0, "I", TextStyle{kWhite, 0, 1});
    CHECK(lit(c, 0, 0));
    CHECK_FALSE(lit(c, 0, 7));
}

TEST_CASE("the panel is translucent and the glyphs are not") {
    auto c = canvas();
    for (auto& p : c) p = 0xFFFFFFFFu;  // white picture underneath
    draw_text(c.data(), kW, kW, kH, 4, 4, "8", TextStyle{kBlack, 0x80000000u, 1});
    // A panel pixel is halfway between the white underneath and black. The panel starts one
    // pixel left of the text, so x=3 is panel and x=2 is untouched picture.
    CHECK(c[4 * kW + 2] == 0xFFFFFFFFu);
    const u32 panel = c[4 * kW + 3];
    CHECK(((panel >> 0) & 0xFFu) == 127);  // black over white at 128/255
    // ...and the other direction rounds the other way, which is the point of rounding at all.
    auto dark = canvas();
    draw_text(dark.data(), kW, kW, kH, 4, 4, "8", TextStyle{kBlack, 0x80FFFFFFu, 1});
    CHECK(((dark[4 * kW + 3] >> 0) & 0xFFu) == 128);  // white over black at 128/255
    CHECK(((panel >> 24) & 0xFFu) == 255);
    // A glyph pixel is fully the foreground colour.
    CHECK(c[4 * kW + 5] == kBlack);
}

TEST_CASE("a zero scale draws nothing rather than dividing by it") {
    auto c = canvas();
    const auto before = c;
    draw_text(c.data(), kW, kW, kH, 0, 0, "X", TextStyle{kWhite, kBlack, 0});
    CHECK(c == before);
    CHECK(text_width("X", 0) == 0);
}
