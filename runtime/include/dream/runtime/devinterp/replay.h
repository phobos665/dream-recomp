// Comparing one guest function against the interpreter (WP1.5, WP3.2).
//
// The differential harness compares a hand-written program against the oracle with chosen inputs.
// Bring-up needs the same question asked of a real run: which of the thousands of functions a title
// executes does the emitter get wrong? Two earlier attempts answer something else.
//
//   * Comparing whole runs breaks down once interrupts start, because the two engines deliver them
//     at different instructions by design (docs/differential-harness.md).
//   * Hiding a function so the interpreter runs it instead cannot redirect a call the emitter
//     resolved statically, which is most of them (docs/runtime-devinterp.md).
//
// This asks the narrow question instead. At a function's entry the context is saved and stores
// start being journalled. At its exit the journal is undone, putting memory back as it was, the
// interpreter runs the same function from the same context, and the two exits are compared:
// registers, and the sequence of stores. Then the interpreter's stores are undone and the
// translated ones put back, so the run carries on exactly as it would have.
//
// It does not care how the function was called, which is what defeated hiding, and it does not care
// when interrupts land, which is what defeated whole-run comparison.
//
// Some calls cannot be compared and are counted rather than guessed at: one that writes to a device
// register (the write cannot be undone and must not be repeated), one that does not return
// normally, and one that takes an interrupt while it runs.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "dream/runtime/mem/dc_memory.h"
#include "dream/runtime/sh4/ctx.h"

namespace dream::devinterp {

struct Divergence {
    std::uint32_t function = 0;
    std::string what;  // which register or store differed, and both values
};

class Replay {
public:
    Replay(sh4::Ctx& ctx, mem::DcMemory& memory) : ctx_(ctx), memory_(memory) {}

    // Called at the entry and the exit of a guest function by hooks the emitter plants under
    // --replay-hooks. Nested calls are ignored: only the outermost function under comparison is
    // replayed, because replaying it replays everything it called.
    void enter(std::uint32_t function, std::uint32_t assumed_mode = 0);
    void exit(std::uint32_t function);

    // Points at the run's delivered-interrupt count. A call that took an interrupt while it ran
    // cannot be compared: the handler's work is in the translated run's stores and the replay does
    // not deliver one, so the two would differ for a reason that is not a bug.
    const std::uint64_t* interrupts = nullptr;

    // Compare only these functions; empty means every function. Keeps the cost bearable on a run
    // that executes millions of calls.
    std::vector<std::uint32_t> only;
    // Self-check: replay the function through the *translated* code again instead of the
    // interpreter. Everything else is identical, so a disagreement in this mode is a bug in the
    // harness and not in the emitter, and any finding it reports is worth nothing until this
    // reports nothing.
    bool self_check = false;
    // Stop the run at the first disagreement rather than counting them.
    bool stop_on_divergence = true;

    std::uint64_t compared = 0, skipped_device = 0, skipped_interrupt = 0, skipped_nonlocal = 0,
                  skipped_too_long = 0;
    // Calls that arrived in a floating-point mode other than the one the function was compiled for.
    // This needs no replay at all: the emitter's assumption travels with the hook and is checked
    // against FPSCR on the way in, so one run names every function the inference got wrong.
    std::uint64_t mode_mismatches = 0;
    std::vector<std::uint32_t> mode_mismatch_functions;
    // Capturing a real call's inputs, instead of comparing it (WP3.2).
    //
    // Comparing a function needs it to return inside the journal's bound, and the functions that
    // most need comparing are exactly the ones that do not: a 3,352-byte function that walks a
    // scene graph is abandoned as "too long" on every call, and the differential harness cannot
    // synthesise inputs for it either, because it dereferences pointers into live heap.
    //
    // So capture the call instead of replaying it. At the entry, write the whole of guest RAM and
    // the register state as a differential-harness case; the oracle and the recompiled runner both
    // load an image at a base, so a RAM image at 0x0c000000 puts each of them in exactly the state
    // the real run was in. No journal is held and nothing is undone, so the length of the function
    // stops mattering.
    std::vector<std::uint32_t> capture;
    std::string capture_prefix;
    unsigned capture_limit = 1;
    unsigned captured = 0;

    // A comparison that has journalled more than this is abandoned: it is almost certainly a
    // function that does not return for a long time, and holding memory hostage for it would stop
    // anything else being compared.
    static constexpr std::size_t kJournalLimit = 20000;
    const std::vector<Divergence>& divergences() const noexcept { return divergences_; }

private:
    void enter_impl(std::uint32_t function);
    // Writes guest RAM and the entry context as a case the differential harness can run.
    void capture_entry(std::uint32_t function);
    bool wanted(std::uint32_t function) const;
    // Compares the interpreted exit against the translated one, appending to divergences_.
    void compare(std::uint32_t function, const sh4::Ctx& translated,
                 const std::vector<mem::DcMemory::WriteRecord>& translated_writes);

    // One captured call: the RAM image written for it and the context it was entered with.
    struct Capture {
        std::uint32_t function;
        std::string ram_path;
        sh4::Ctx ctx;
    };
    std::vector<Capture> captured_cases_;

    sh4::Ctx& ctx_;
    mem::DcMemory& memory_;
    unsigned depth_ = 0;
    bool replaying_ = false;
    std::unordered_set<std::uint32_t> too_long_;
    std::uint32_t active_ = 0;
    sh4::Ctx entry_{};
    std::uint64_t entry_interrupts_ = 0;
    std::vector<Divergence> divergences_;
};

// The run's replay harness, when one is installed. Null in a release build and whenever the
// launcher was not asked for it.
Replay* replay() noexcept;
void set_replay(Replay* r) noexcept;

}  // namespace dream::devinterp
