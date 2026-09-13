// Switch-table recovery (WP1.4). SH-4 compilers dispatch `switch` statements through a table of
// code addresses or offsets indexed by the case value, ending in `braf Rn` (offsets relative to the
// instruction after the slot) or `jmp @Rn` (absolute addresses, or table-relative when an `add`
// follows the load). Recognising the idiom turns a computed jump into a known set of local targets.
#pragma once

#include <cstdint>
#include <vector>

#include "dream/translator/emit.h"
#include "dream/translator/sh4/decoder.h"

namespace dream::translator {

struct SwitchTable {
    std::uint32_t table = 0;             // address of the first entry
    std::uint8_t entry_size = 4;         // 1, 2 or 4 bytes
    bool relative_to_pc = false;         // braf: target = jump_pc + 4 + entry
    bool relative_to_table = false;      // jmp with add: target = table + entry
    std::vector<std::uint32_t> targets;  // resolved code addresses, in table order
};

// Try to recover the table behind a `braf`/`jmp @Rn` at `jump_pc` whose block starts at
// `block_start`. Targets are accepted while they fall inside [range_lo, range_hi) and are even;
// the scan stops at the first that does not, or after max_entries.
bool recover_switch(const Image& image, std::uint32_t block_start, std::uint32_t jump_pc,
                    const sh4::Instr& jump, std::uint32_t range_lo, std::uint32_t range_hi,
                    SwitchTable& out, unsigned max_entries = 256);

}  // namespace dream::translator
