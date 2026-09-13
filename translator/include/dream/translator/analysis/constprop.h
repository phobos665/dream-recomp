// Block-local constant propagation (WP1.4/WP3.1): the value a register holds at `pc`, when it was
// built inside the block from immediates, literal-pool loads, `mova` and simple arithmetic. Used
// for call targets (`jsr @rN` after `mov.l @(disp,pc)`, or the position-independent SHC idiom
// `mov.l off,rN ; mova base,r0 ; add r0,rN ; or r14,rN ; jsr @rN`) and for FPSCR loads.
#pragma once

#include <cstdint>
#include <optional>

#include "dream/translator/emit.h"

namespace dream::translator {

// Walks [block_start, pc) forward. With `alias_or`, an OR with an unknown register keeps the known
// operand: the only such OR in call sequences sets P1/P2 alias bits, which the alias-normalising
// callers discard anyway. Returns nothing when the register is not a compile-time constant.
std::optional<std::uint32_t> constant_at(const Image& img, std::uint32_t block_start,
                                         std::uint32_t pc, unsigned reg, bool alias_or);

}  // namespace dream::translator
