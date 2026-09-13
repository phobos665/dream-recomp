#include "dream/render/overlay.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace dream::render {
namespace {

using u32 = std::uint32_t;

// The glyphs are written out as pictures rather than as hex, because a font typed as numbers is a
// font nobody can check. Seven rows of five, '#' set and '.' clear, in ASCII order from space.
// Everything above 'Z' folds to upper case or draws blank.
struct Glyph {
    char ch;
    const char* rows[kGlyphHeight];
};

constexpr Glyph kFont[] = {
    {' ', {".....", ".....", ".....", ".....", ".....", ".....", "....."}},
    {'!', {"..#..", "..#..", "..#..", "..#..", "..#..", ".....", "..#.."}},
    {'%', {"##..#", "##..#", "...#.", "..#..", ".#...", "#..##", "#..##"}},
    {'(', {"..#..", ".#...", "#....", "#....", "#....", ".#...", "..#.."}},
    {')', {"..#..", "...#.", "....#", "....#", "....#", "...#.", "..#.."}},
    {'+', {".....", "..#..", "..#..", "#####", "..#..", "..#..", "....."}},
    {',', {".....", ".....", ".....", ".....", ".##..", ".##..", ".#..."}},
    {'-', {".....", ".....", ".....", "#####", ".....", ".....", "....."}},
    {'.', {".....", ".....", ".....", ".....", ".....", ".##..", ".##.."}},
    {'/', {"....#", "...#.", "...#.", "..#..", ".#...", ".#...", "#...."}},
    {'0', {".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."}},
    {'1', {"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."}},
    {'2', {".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"}},
    {'3', {"#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###."}},
    {'4', {"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."}},
    {'5', {"#####", "#....", "####.", "....#", "....#", "#...#", ".###."}},
    {'6', {"..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."}},
    {'7', {"#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."}},
    {'8', {".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."}},
    {'9', {".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.."}},
    {':', {".....", ".##..", ".##..", ".....", ".##..", ".##..", "....."}},
    {'<', {"...#.", "..#..", ".#...", "#....", ".#...", "..#..", "...#."}},
    {'=', {".....", ".....", "#####", ".....", "#####", ".....", "....."}},
    {'>', {".#...", "..#..", "...#.", "....#", "...#.", "..#..", ".#..."}},
    {'A', {".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
    {'B', {"####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."}},
    {'C', {".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."}},
    {'D', {"###..", "#..#.", "#...#", "#...#", "#...#", "#..#.", "###.."}},
    {'E', {"#####", "#....", "#....", "####.", "#....", "#....", "#####"}},
    {'F', {"#####", "#....", "#....", "####.", "#....", "#....", "#...."}},
    {'G', {".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".####"}},
    {'H', {"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
    {'I', {".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."}},
    {'J', {"....#", "....#", "....#", "....#", "#...#", "#...#", ".###."}},
    {'K', {"#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"}},
    {'L', {"#....", "#....", "#....", "#....", "#....", "#....", "#####"}},
    {'M', {"#...#", "##.##", "#.#.#", "#...#", "#...#", "#...#", "#...#"}},
    {'N', {"#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#"}},
    {'O', {".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
    {'P', {"####.", "#...#", "#...#", "####.", "#....", "#....", "#...."}},
    {'Q', {".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"}},
    {'R', {"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"}},
    {'S', {".####", "#....", "#....", ".###.", "....#", "....#", "####."}},
    {'T', {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."}},
    {'U', {"#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
    {'V', {"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."}},
    {'W', {"#...#", "#...#", "#...#", "#...#", "#.#.#", "##.##", "#...#"}},
    {'X', {"#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"}},
    {'Y', {"#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."}},
    {'Z', {"#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"}},
};

const Glyph* find_glyph(char c) {
    const char up = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (const Glyph& g : kFont)
        if (g.ch == up)
            return &g;
    return nullptr;
}

// src over dst, both RGBA with red in the low byte. The background is drawn at less than full
// alpha so the picture underneath still shows; the glyphs themselves are opaque.
u32 blend(u32 dst, u32 src) {
    const u32 a = (src >> 24) & 0xFFu;
    if (a == 0)
        return dst;
    if (a == 0xFF)
        return src;
    u32 out = 0xFF000000u;
    for (unsigned i = 0; i < 3; ++i) {
        const u32 s = (src >> (8 * i)) & 0xFFu, d = (dst >> (8 * i)) & 0xFFu;
        // Rounded rather than truncated, so a 50% grey over black is 128 and not 127: the tests
        // compare exact values and a half-pixel bias is the kind of thing that drifts unnoticed.
        const u32 v = (s * a + d * (255u - a) + 127u) / 255u;
        out |= v << (8 * i);
    }
    return out;
}

}  // namespace

unsigned text_width(std::string_view text, unsigned scale) {
    if (scale == 0)
        return 0;
    return static_cast<unsigned>(text.size()) * kCellWidth * scale;
}

unsigned text_height(unsigned scale) {
    return kCellHeight * scale;
}

void fill_rect(u32* rgba, unsigned pitch, unsigned width, unsigned height, int x, int y, int w,
               int h, u32 colour) {
    if (!rgba || w <= 0 || h <= 0)
        return;
    const int x0 = std::max(0, x), y0 = std::max(0, y);
    const int x1 = std::min(static_cast<int>(width), x + w);
    const int y1 = std::min(static_cast<int>(height), y + h);
    for (int py = y0; py < y1; ++py)
        for (int px = x0; px < x1; ++px) {
            u32& d = rgba[static_cast<std::size_t>(py) * pitch + static_cast<std::size_t>(px)];
            d = blend(d, colour);
        }
}

void draw_text(u32* rgba, unsigned pitch, unsigned width, unsigned height, int x, int y,
               std::string_view text, const TextStyle& style) {
    if (!rgba || style.scale == 0 || text.empty())
        return;
    const int s = static_cast<int>(style.scale);
    if (style.bg) {
        // A one-pixel margin all round, so the text is never flush against its own panel edge.
        fill_rect(rgba, pitch, width, height, x - s, y - s,
                  static_cast<int>(text_width(text, style.scale)) + s,
                  static_cast<int>(text_height(style.scale)) + s, style.bg);
    }
    int pen = x;
    for (const char c : text) {
        const Glyph* g = find_glyph(c);
        if (g) {
            for (unsigned row = 0; row < kGlyphHeight; ++row)
                for (unsigned col = 0; col < kGlyphWidth; ++col) {
                    if (g->rows[row][col] != '#')
                        continue;
                    fill_rect(rgba, pitch, width, height, pen + static_cast<int>(col) * s,
                              y + static_cast<int>(row) * s, s, s, style.fg);
                }
        }
        pen += static_cast<int>(kCellWidth) * s;
    }
}

}  // namespace dream::render
