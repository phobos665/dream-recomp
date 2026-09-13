// The display framebuffer (WP2.3 step 6). Framebuffers are built here in each pixel format and
// decoded back, so a failure names the format rather than pointing at a whole frame.
#include <cstdint>
#include <cstring>
#include <vector>

#include "dream/render/framebuffer.h"

#include "doctest.h"

namespace {

using namespace dream::render;
using u32 = std::uint32_t;

// A PVR register block, as the launcher dumps it: 0x2000 bytes from 0x005F8000.
struct Regs {
    std::vector<u32> words = std::vector<u32>(0x2000 / 4, 0);
    u32& at(std::size_t offset) { return words[offset / 4]; }
    // fb_enable, depth, concat, and the pixel clock divider that selects 480 lines.
    void control(unsigned depth, unsigned concat = 0, bool full_height = true) {
        at(0x044) = 1u | (depth << 2) | (concat << 4) | (full_height ? (1u << 23) : 0u);
    }
    void size(unsigned width_words, unsigned height, unsigned modulus_words) {
        at(0x05C) = ((width_words / 2u) - 1u) | ((height - 1u) << 10) | (modulus_words << 20);
    }
    void read_address(u32 a) { at(0x050) = a; }
    void write_address(u32 a) { at(0x060) = a; }
    // FB_W_CTRL's pixel format and FB_W_LINESTRIDE's row length in eight-byte units.
    void write_control(unsigned packmode) { at(0x048) = packmode; }
    void write_stride(unsigned eight_byte_units) { at(0x04C) = eight_byte_units; }
    // FB_X_CLIP and FB_Y_CLIP: the region a render is allowed to write.
    void write_clip(unsigned x_max, unsigned y_max) {
        at(0x068) = x_max << 16;
        at(0x06C) = y_max << 16;
    }
};

void put16(std::vector<std::uint8_t>& v, std::size_t at, std::uint16_t value) {
    v[at] = static_cast<std::uint8_t>(value);
    v[at + 1] = static_cast<std::uint8_t>(value >> 8);
}

}  // namespace

TEST_CASE("framebuffer: the registers give size, format and stride") {
    Regs r;
    r.control(1);         // RGB565
    r.size(640, 480, 1);  // 640 words wide, 480 lines, no extra stride
    r.read_address(0x200000);
    FramebufferInfo info;
    REQUIRE(describe_framebuffer(r.words.data(), info));
    CHECK(info.enabled);
    CHECK(info.format == FramebufferFormat::Rgb565);
    CHECK(info.width == 640);
    CHECK(info.height == 480);
    CHECK(info.modulus == 0);
    CHECK(info.address == 0x200000);

    // Width and stride are counted in 16-bit words whatever the format, so a 32-bit framebuffer
    // covers half as many pixels in the same number of words.
    r.control(3);  // ARGB8888
    REQUIRE(describe_framebuffer(r.words.data(), info));
    CHECK(info.width == 320);
    // And a 24-bit one two thirds as many.
    r.control(2);  // RGB888
    REQUIRE(describe_framebuffer(r.words.data(), info));
    CHECK(info.width == 426);

    // At the lower pixel clock the hardware scans 240 lines and the display doubles them.
    r.control(1, 0, /*full_height=*/false);
    REQUIRE(describe_framebuffer(r.words.data(), info));
    CHECK(info.height == 240);
}

TEST_CASE("framebuffer: 16-bit formats decode, with the concat field filling the low bits") {
    std::vector<std::uint8_t> vram(64 * 1024, 0);
    Regs r;
    r.control(1, /*concat=*/7);  // RGB565, low bits filled with ones
    r.size(4, 2, 1);
    r.read_address(0);
    FramebufferInfo info;
    REQUIRE(describe_framebuffer(r.words.data(), info));
    REQUIRE(info.width == 4);
    REQUIRE(info.height == 2);

    put16(vram, 0, 0xF800u);  // full red
    put16(vram, 2, 0x07E0u);  // full green
    put16(vram, 4, 0x001Fu);  // full blue
    put16(vram, 6, 0x0000u);  // black
    std::vector<u32> pixels;
    REQUIRE(decode_framebuffer(info, vram.data(), vram.size(), pixels));
    REQUIRE(pixels.size() == 8);
    CHECK((pixels[0] & 0xFFu) == 255u);          // red channel saturates
    CHECK(((pixels[1] >> 8) & 0xFFu) == 255u);   // green
    CHECK(((pixels[2] >> 16) & 0xFFu) == 255u);  // blue
    CHECK((pixels[0] >> 24) == 255u);            // always opaque
    // Black is not quite black once the concat field fills in: that is the hardware's behaviour.
    CHECK((pixels[3] & 0xFFu) == 7u);

    // RGB555 ignores the top bit; keep the same concat so the channel still saturates.
    r.control(0, /*concat=*/7);
    REQUIRE(describe_framebuffer(r.words.data(), info));
    put16(vram, 0, 0x7C00u);  // full red in 555
    REQUIRE(decode_framebuffer(info, vram.data(), vram.size(), pixels));
    CHECK((pixels[0] & 0xFFu) == 255u);
}

TEST_CASE("framebuffer: a row stride wider than the picture is skipped") {
    std::vector<std::uint8_t> vram(64 * 1024, 0);
    Regs r;
    r.control(1);
    r.size(4, 2, 3);  // two pixels of padding after each row
    r.read_address(0);
    FramebufferInfo info;
    REQUIRE(describe_framebuffer(r.words.data(), info));
    REQUIRE(info.width == 4);
    REQUIRE(info.modulus == 4);

    // Second row starts eight pixels in: four of picture plus four of padding.
    put16(vram, 0, 0xF800u);            // first pixel of row 0: red
    put16(vram, (4 + 4) * 2, 0x001Fu);  // first pixel of row 1: blue
    std::vector<u32> pixels;
    REQUIRE(decode_framebuffer(info, vram.data(), vram.size(), pixels));
    CHECK((pixels[0] & 0xFFu) == 248u);          // red, without concat
    CHECK(((pixels[4] >> 16) & 0xFFu) == 248u);  // blue, at the start of the second row
}

TEST_CASE("framebuffer: 24- and 32-bit formats decode") {
    std::vector<std::uint8_t> vram(64 * 1024, 0);
    Regs r;
    r.size(6, 1, 1);
    r.read_address(0);
    // RGB888: three bytes per pixel, blue first in memory.
    r.control(2);
    FramebufferInfo info;
    REQUIRE(describe_framebuffer(r.words.data(), info));
    vram[0] = 0x10;  // b
    vram[1] = 0x20;  // g
    vram[2] = 0x30;  // r
    std::vector<u32> pixels;
    REQUIRE(decode_framebuffer(info, vram.data(), vram.size(), pixels));
    CHECK((pixels[0] & 0xFFu) == 0x30u);
    CHECK(((pixels[0] >> 8) & 0xFFu) == 0x20u);
    CHECK(((pixels[0] >> 16) & 0xFFu) == 0x10u);

    // ARGB8888: four bytes, the same order plus an ignored alpha byte.
    r.control(3);
    REQUIRE(describe_framebuffer(r.words.data(), info));
    REQUIRE(decode_framebuffer(info, vram.data(), vram.size(), pixels));
    CHECK((pixels[0] & 0xFFu) == 0x30u);
    CHECK((pixels[0] >> 24) == 255u);
}

TEST_CASE("framebuffer: one that would read past video memory is refused") {
    std::vector<std::uint8_t> tiny(1024, 0);
    Regs r;
    r.control(1);
    r.size(640, 480, 1);
    r.read_address(0);
    FramebufferInfo info;
    REQUIRE(describe_framebuffer(r.words.data(), info));
    std::vector<u32> pixels;
    CHECK_FALSE(decode_framebuffer(info, tiny.data(), tiny.size(), pixels));
}

TEST_CASE("framebuffer: the render target is reported, not judged") {
    Regs r;
    r.read_address(0x200000);
    r.write_address(0x200000);
    CHECK(describe_render_target(r.words.data()).same_as_display);
    // A different address is usually just the other half of a double buffer, so this says where
    // the frame is going and leaves the interpretation to the caller.
    r.write_address(0x400000);
    const RenderTarget rt = describe_render_target(r.words.data());
    CHECK_FALSE(rt.same_as_display);
    CHECK(rt.address == 0x400000);
}

TEST_CASE("framebuffer: a rendered frame encodes back into the guest's format") {
    std::vector<std::uint8_t> vram(64 * 1024, 0);
    Regs r;
    r.control(1);  // RGB565
    r.size(4, 2, 1);
    r.read_address(0x100);
    FramebufferInfo info;
    REQUIRE(describe_framebuffer(r.words.data(), info));

    // Red, green, blue, white across the first row.
    const std::vector<u32> rendered{0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u, 0xFFFFFFFFu,
                                    0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u};
    REQUIRE(encode_framebuffer(info, rendered.data(), 4, 2, vram.data(), vram.size()));

    // Reading it back gives the same picture, to the precision the format allows.
    std::vector<u32> pixels;
    REQUIRE(decode_framebuffer(info, vram.data(), vram.size(), pixels));
    REQUIRE(pixels.size() == 8);
    CHECK((pixels[0] & 0xFFu) == 248u);          // red, 5 bits of it
    CHECK(((pixels[1] >> 8) & 0xFFu) == 252u);   // green, 6 bits
    CHECK(((pixels[2] >> 16) & 0xFFu) == 248u);  // blue
    CHECK(pixels[3] == 0xFFF8FCF8u);             // white
    CHECK((pixels[4] & 0x00FFFFFFu) == 0u);      // the second row stayed black

    // A rendered image larger than the guest's framebuffer is scaled down, which is what an
    // increased internal resolution will need.
    std::vector<u32> big(8 * 4, 0xFF0000FFu);
    REQUIRE(encode_framebuffer(info, big.data(), 8, 4, vram.data(), vram.size()));
    REQUIRE(decode_framebuffer(info, vram.data(), vram.size(), pixels));
    CHECK((pixels[0] & 0xFFu) == 248u);
    CHECK((pixels[7] & 0xFFu) == 248u);
}

TEST_CASE("framebuffer: the write side has its own registers and its own format numbering") {
    Regs r;
    r.write_address(0x600000);

    // FB_W_CTRL numbers seven formats where FB_R_CTRL numbers four, and in a different order.
    FramebufferInfo info;
    r.write_control(1);  // RGB565
    REQUIRE(describe_write_framebuffer(r.words.data(), 640, 480, info));
    CHECK(info.format == FramebufferFormat::Rgb565);
    CHECK(info.address == 0x600000);
    CHECK(info.width == 640);
    CHECK(info.height == 480);
    CHECK(info.modulus == 0);  // no stride programmed: rows are packed

    r.write_control(3);  // ARGB1555: sixteen bits, the alpha unused once it is on screen
    REQUIRE(describe_write_framebuffer(r.words.data(), 640, 480, info));
    CHECK(info.format == FramebufferFormat::Rgb555);

    r.write_control(2);  // ARGB4444, which FB_R_CTRL's two-bit field cannot name at all
    REQUIRE(describe_write_framebuffer(r.words.data(), 640, 480, info));
    CHECK(info.format == FramebufferFormat::Argb4444);

    r.write_control(5);  // KRGB0888 and ARGB8888 are the same thirty-two bits here
    REQUIRE(describe_write_framebuffer(r.words.data(), 640, 480, info));
    CHECK(info.format == FramebufferFormat::Argb8888);

    r.write_control(7);  // reserved: the hardware does not say what it writes, so neither do we
    CHECK_FALSE(describe_write_framebuffer(r.words.data(), 640, 480, info));

    // A row longer than the picture leaves a gap the encoder has to skip.
    r.write_control(1);
    r.write_stride(176);  // 176 * 8 = 1408 bytes = 704 pixels at sixteen bits
    REQUIRE(describe_write_framebuffer(r.words.data(), 640, 480, info));
    CHECK(info.modulus == 64);

    CHECK_FALSE(describe_write_framebuffer(r.words.data(), 0, 480, info));
}

TEST_CASE("framebuffer: the clip registers size the written frame") {
    // FB_W_LINESTRIDE is at 0x04C. Read at 0x06C it lands on FB_Y_CLIP, whose low bits are the
    // clip's first line and so are usually zero, which makes the wrong register look right.
    Regs r;
    r.write_control(1);
    r.write_address(0x600000);
    r.write_clip(639, 479);

    FramebufferInfo info;
    REQUIRE(describe_write_framebuffer(r.words.data(), 320, 240, info));
    CHECK(info.width == 640);  // the clip wins over the size the caller offered
    CHECK(info.height == 480);
    CHECK(info.modulus == 0);  // stride 0 at 0x04C, and FB_Y_CLIP must not be mistaken for it

    r.write_stride(160);  // 160 * 8 = 1280 bytes = 640 pixels: exactly the width, so no gap
    REQUIRE(describe_write_framebuffer(r.words.data(), 320, 240, info));
    CHECK(info.modulus == 0);

    // Clip registers left alone: fall back to the size the caller knows.
    Regs plain;
    plain.write_control(1);
    REQUIRE(describe_write_framebuffer(plain.words.data(), 320, 240, info));
    CHECK(info.width == 320);
    CHECK(info.height == 240);
}

TEST_CASE("framebuffer: ARGB4444 survives the round trip") {
    std::vector<std::uint8_t> vram(4096, 0);
    Regs r;
    r.write_control(2);
    r.write_address(0x40);
    FramebufferInfo info;
    REQUIRE(describe_write_framebuffer(r.words.data(), 2, 2, info));

    const std::vector<u32> rendered{0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u, 0xFFFFFFFFu};
    REQUIRE(encode_framebuffer(info, rendered.data(), 2, 2, vram.data(), vram.size()));
    std::vector<u32> pixels;
    REQUIRE(decode_framebuffer(info, vram.data(), vram.size(), pixels));
    REQUIRE(pixels.size() == 4);
    CHECK((pixels[0] & 0xFFu) == 255u);          // four bits of red, widened back to full
    CHECK(((pixels[1] >> 8) & 0xFFu) == 255u);   // green
    CHECK(((pixels[2] >> 16) & 0xFFu) == 255u);  // blue
    CHECK(pixels[3] == 0xFFFFFFFFu);             // white
}

TEST_CASE("framebuffer: a written frame lands where the render target says") {
    // The whole point of the write side: the renderer's output goes to FB_W_SOF1, and the buffer
    // being displayed is left alone until the guest swaps them.
    std::vector<std::uint8_t> vram(64 * 1024, 0);
    Regs r;
    r.control(1);  // the display reads RGB565 from 0x100
    r.size(4, 2, 1);
    r.read_address(0x100);
    r.write_control(1);  // the renderer writes RGB565 to 0x2000
    r.write_address(0x2000);

    FramebufferInfo display, target;
    REQUIRE(describe_framebuffer(r.words.data(), display));
    REQUIRE(describe_write_framebuffer(r.words.data(), display.width, display.height, target));
    const std::vector<u32> rendered(8, 0xFFFFFFFFu);
    REQUIRE(encode_framebuffer(target, rendered.data(), 4, 2, vram.data(), vram.size()));

    std::vector<u32> pixels;
    REQUIRE(decode_framebuffer(display, vram.data(), vram.size(), pixels));
    CHECK((pixels[0] & 0x00FFFFFFu) == 0u);  // still black: nothing was written there

    // Point the display at the buffer that was just written and the frame appears.
    r.read_address(0x2000);
    REQUIRE(describe_framebuffer(r.words.data(), display));
    REQUIRE(decode_framebuffer(display, vram.data(), vram.size(), pixels));
    CHECK(pixels[0] == 0xFFF8FCF8u);
}
