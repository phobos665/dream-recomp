// The background plane (WP2.3).
//
// Everything a frame draws sits on top of one polygon that fills the screen behind it. It is not
// in any display list: the game writes it into the parameter buffer itself and points ISP_BACKGND_T
// at it, so a renderer that only draws display lists shows its own clear colour instead. On Crazy
// Taxi that lost the SEGA screen its white and the title screens their yellow.
//
// Two details make it awkward. The parameter buffer is read through the 32-bit view of video
// memory, which interleaves the two banks, so the address in the register is not an offset into the
// flat buffer. And the vertex layout is not fixed: ISP_BACKGND_T carries a "skip" giving the extra
// words per vertex, and which of them are texture coordinates and which are colours comes from the
// ISP word, not from a parameter control word, because the plane has none.
//
// Reference: Flycast's FillBGP in core/hw/pvr/ta_vtx.cpp (GPL-2.0, ADR 1).
#pragma once

#include <cstddef>
#include <cstdint>

#include "dream/render/display_list.h"

namespace dream::render {

// Appends the plane's four vertices to `frame` and puts the polygon at the front of the opaque
// list, so it is drawn before everything else. False when the registers do not describe one.
bool add_background(Frame& frame, const std::uint32_t* pvr_regs, const std::uint8_t* vram,
                    std::size_t vram_size);

}  // namespace dream::render
