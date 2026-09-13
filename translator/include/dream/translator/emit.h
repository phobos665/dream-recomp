// SH-4 to C++ emitter (WP1.2). See docs/emitter-design.md for the contract with the runtime.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace dream::translator {

// A loaded guest image: raw bytes at a link address.
struct Image {
    std::vector<std::uint8_t> bytes;
    std::uint32_t base = 0;

    bool contains(std::uint32_t addr, std::uint32_t len = 1) const noexcept {
        return addr >= base && addr - base + len <= bytes.size();
    }
    std::uint16_t read16(std::uint32_t addr) const noexcept {
        const std::size_t o = addr - base;
        return static_cast<std::uint16_t>(bytes[o] | (bytes[o + 1] << 8));
    }
    std::uint32_t read32(std::uint32_t addr) const noexcept {
        const std::size_t o = addr - base;
        return static_cast<std::uint32_t>(bytes[o] | (bytes[o + 1] << 8) | (bytes[o + 2] << 16) |
                                          (static_cast<std::uint32_t>(bytes[o + 3]) << 24));
    }
};

// A function to translate: the half-open address range of its code.
struct FunctionSpec {
    std::uint32_t entry = 0;
    std::uint32_t end = 0;  // exclusive
    std::string name;       // C++ identifier; defaults to fn_<entry hex>
    // FPSCR assumed live at entry (ADR 6). Katana's default is 0x00040001 (PR=0, SZ=0). Set the
    // PR/SZ bits to what callers establish; 0xFFFFFFFF means "unknown", forcing runtime branches.
    std::uint32_t fpscr_entry = 0x00040001;
    // Set by emit_unit's entry-mode inference when callers reach this function in different modes:
    // the bit is then tested at run time instead of assumed.
    bool entry_pr_unknown = false, entry_sz_unknown = false;
};

struct EmitOptions {
    bool fold_literals = true;  // MOV.L @(disp,PC) inside the image becomes a constant
    bool trace = false;         // call trace_return() before every return
    // Bracket each function's plain entry with a ReplayScope, so a development run can compare it
    // against the interpreter with the same inputs (runtime/devinterp/replay.h). Development only:
    // it costs a constructor and destructor per call even when no harness is installed.
    bool replay_hooks = false;
    bool irq_checks = true;  // interrupt polls at entry and back-edges (ADR 7)
    bool overlay = false;    // table entries carry the code words they were translated from
};

// A run of bytes inside a function that discovery never decoded. Often a literal pool, sometimes
// code the emitted program is missing (docs/emitter-design.md).
struct CoverageGap {
    std::uint32_t function = 0, from = 0, to = 0;
};

struct EmitResult {
    std::string source;                 // one translation unit
    std::string header;                 // declarations of the emitted functions
    std::vector<std::string> warnings;  // per-instruction notes (unlowered ops etc.)
    std::size_t instructions = 0;
    std::size_t unlowered = 0;
    std::size_t fp_runtime_branches = 0;
    std::size_t unknown_summaries = 0;
    std::size_t entries_inferred = 0;    // functions whose entry PR/SZ differs from the default
    std::size_t entries_unknown = 0;     // functions reached in more than one mode (runtime tests)
    std::size_t patched_words = 0;       // pool words written by unit code, read at run time
    std::size_t direct_calls = 0;        // jsr/bsrf sites resolved to a direct call
    std::size_t switches_recovered = 0;  // computed jumps lowered through a recovered table
    // Blocks made for the instruction after a branch whose delay slot another branch targets. That
    // path is reachable only by entering through the slot, so without the block its code is never
    // emitted at all.
    std::size_t slot_fallthrough_blocks = 0;
    // How much of the functions' address ranges discovery actually decoded, and where it did not.
    std::uint64_t bytes_in_range = 0, bytes_decoded = 0, bytes_undecoded = 0;
    std::vector<CoverageGap> gaps;
    // Intervals, so the union can be taken across functions whose ranges overlap.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> function_ranges, decoded_runs,
        literal_words;
};

std::string default_name(std::uint32_t entry);

// Translate the given functions of one image into a C++ translation unit plus a header.
EmitResult emit_unit(const Image& image, const std::vector<FunctionSpec>& functions,
                     const EmitOptions& options);

}  // namespace dream::translator
