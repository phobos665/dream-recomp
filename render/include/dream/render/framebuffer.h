// The display framebuffer (WP2.3 step 6): the pixels the video hardware actually scans out.
//
// Everything drawn ends up here, whichever path put it there. The Tile Accelerator writes a
// rendered frame into it; a title can also write pixels into it directly, which is how video
// playback, some 2D screens and a few effects work, and those pixels never pass through a display
// list at all. Reading it is therefore the only way to see what a player would see.
//
// Four pixel formats, a row stride independent of the width, and a "concat" field that fills the
// low bits when a 16-bit format is widened. Reference: Flycast's ReadFramebuffer in
// core/rend/TexCache.cpp (GPL-2.0, ADR 1).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dream::render {

enum class FramebufferFormat : unsigned {
    Rgb555 = 0,    // 16-bit, one bit unused
    Rgb565 = 1,    // 16-bit
    Rgb888 = 2,    // 24-bit, three bytes per pixel
    Argb8888 = 3,  // 32-bit
    // Only the write side produces this one: FB_W_CTRL can ask for ARGB4444, which FB_R_CTRL's
    // two-bit field cannot name. Values 0 to 3 keep FB_R_CTRL's own encoding, so this sits past it.
    Argb4444 = 4
};

struct FramebufferInfo {
    std::uint32_t address = 0;  // byte offset into video memory
    std::uint32_t width = 640, height = 480;
    std::uint32_t modulus = 0;  // extra pixels between the end of a row and the start of the next
    FramebufferFormat format = FramebufferFormat::Rgb565;
    std::uint32_t concat = 0;  // low bits used to fill out a widened 16-bit channel
    bool enabled = false;
    std::string describe() const;
};

// Works the above out from the PVR register block, given as 0x2000 bytes of 32-bit registers
// starting at 0x005F8000 (which is what the launcher's --dump-vram writes beside the image).
// Returns false when the registers do not describe a displayable framebuffer.
bool describe_framebuffer(const std::uint32_t* pvr_regs, FramebufferInfo& out);

// The same for the buffer a render is written into, from FB_W_CTRL and FB_W_LINESTRIDE. The write
// side is described by different registers with a different format encoding. The size comes from
// FB_X_CLIP and FB_Y_CLIP, the region the hardware is allowed to write; the `width` and `height`
// arguments are the fallback for a title that leaves the clip registers at zero. Returns false for
// the reserved pixel format.
bool describe_write_framebuffer(const std::uint32_t* pvr_regs, std::uint32_t width,
                                std::uint32_t height, FramebufferInfo& out);

// Decodes into `out` as RGBA8888, `width * height` pixels, row 0 at the top. Returns false when
// the framebuffer would read outside video memory.
bool decode_framebuffer(const FramebufferInfo& info, const std::uint8_t* vram,
                        std::size_t vram_size, std::vector<std::uint32_t>& out);

// Where the hardware was told to write the next rendered frame.
//
// A different address from the one being displayed does *not* by itself mean a render to texture:
// almost every title double-buffers, so the write address is normally the buffer that is not on
// screen. Telling the two apart needs to know whether the written region is later sampled as a
// texture, which only the texture cache can answer. This reports the address and leaves the
// judgement to the caller.
struct RenderTarget {
    std::uint32_t address = 0;
    bool same_as_display = false;  // rendering straight into the buffer being scanned out
};
RenderTarget describe_render_target(const std::uint32_t* pvr_regs);

// Converts a rendered frame back into the guest's own framebuffer format and writes it into video
// memory. This is what makes the two drawing paths compose: a title that renders geometry and then
// writes pixels directly into the same buffer expects to see both.
bool encode_framebuffer(const FramebufferInfo& info, const std::uint32_t* rgba,
                        std::uint32_t src_width, std::uint32_t src_height, std::uint8_t* vram,
                        std::size_t vram_size);

}  // namespace dream::render
