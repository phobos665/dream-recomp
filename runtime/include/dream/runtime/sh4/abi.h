// Functions emitted code calls into the runtime (docs/emitter-design.md). The full runtime and
// the bare harness each provide an implementation; emitted translation units only see this header.
#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "dream/runtime/memory.h"
#include "dream/runtime/sh4/ctx.h"

namespace dream::sh4 {

using GuestFn = void (*)(Ctx&, ::dream::Memory&);
// Resume entry: enters the function at a block start or call-return address inside it.
using ResumeFn = void (*)(Ctx&, ::dream::Memory&, std::uint32_t pc);

struct FunctionEntry {
    std::uint32_t address;
    GuestFn fn;
    // Overlay units (docs/game-config.md): the instruction words the function was translated
    // from. The entry only resolves while guest memory at `address` still holds them, so several
    // translations of code the program copies to the same RAM address over time can coexist.
    const std::uint16_t* signature = nullptr;
    std::uint32_t signature_words = 0;
    std::uint32_t end = 0;      // one past the last instruction (0: no range, e.g. HLE hooks)
    ResumeFn resume = nullptr;  // non-local returns re-enter the function here
};

// A guest `rts` whose PR is not the address the function was called from: a setjmp/longjmp or a
// cooperative task switch (Katana's context restore). The host frames between the throw and
// run_guest belong to the abandoned guest context and are dropped; run_guest continues at `pc`.
struct NonLocalReturn {
    std::uint32_t pc;
};

// Registers a translation unit's function table; called from a static initialiser the emitter
// generates. Tables from several units are merged.
void register_functions(const FunctionEntry* entries, std::size_t count) noexcept;

// Looks the guest address up in the merged table. Returns nullptr when untranslated. Entries with
// a signature are only returned when `m` is given and the words match memory at `address`.
GuestFn find_function(std::uint32_t address, ::dream::Memory* m = nullptr) noexcept;
// Development aid: translations whose entry lies in [lo, hi) are hidden from find_function, so
// calls to them take the miss handler (the dev interpreter). Used to bisect a translated-code bug
// by address range; an empty range (the default) hides nothing.
void set_interpret_range(std::uint32_t lo, std::uint32_t hi) noexcept;
// Hides these function entries as well, whether or not they fall in the range above. A range says
// "somewhere between here and there"; isolating a bug to one function needs an arbitrary subset,
// so that it can be shrunk until putting any member back brings the fault return.
void set_interpret_functions(const std::vector<std::pair<std::uint32_t, std::uint32_t>>& fns);

// Control transfers whose target is only known at run time.
void call_indirect(Ctx& c, ::dream::Memory& m, std::uint32_t target);

// Non-local returns (docs/emitter-design.md, "Non-local returns"). Emitted code calls
// nonlocal_return from an `rts` whose target is not its entry PR; it throws NonLocalReturn.
// resume_at enters the translated function containing `pc` at that address (its entry when `pc`
// is one); with no such translation it hands the address to the hooks' miss handler.
// resume_miss is what a resume entry calls for an address it has no label for.
// run_guest runs the program from `entry` until control returns to the PR it was started with,
// re-entering translated code through resume_at after every non-local return.
[[noreturn]] void nonlocal_return(Ctx& c, ::dream::Memory& m, std::uint32_t pc);
void resume_at(Ctx& c, ::dream::Memory& m, std::uint32_t pc);
void resume_miss(Ctx& c, ::dream::Memory& m, std::uint32_t pc);
void run_guest(Ctx& c, ::dream::Memory& m, std::uint32_t entry);
// Same, starting from a function pointer (the test harness links many programs at one base).
void run_guest(Ctx& c, ::dream::Memory& m, GuestFn entry);

// Exceptions and interrupts. Emitted code calls deliver_irq when `c.cycles >= c.next_event`.
void deliver_irq(Ctx& c, ::dream::Memory& m);
void trapa(Ctx& c, ::dream::Memory& m, std::uint32_t imm);
// Returns true when the RTE closes a delivered exception and resumes the interrupted context (the
// emitted `return` then unwinds to the delivery point) and false when it is a jump: SR and PC are
// restored from SSR/SPC and emitted code continues through nonlocal_return(c.pc) (Katana's
// startup enters the first task this way). An RTE that closes the exception but resumes a
// different context (a task switch inside the handler) does not return: the runtime throws
// NonLocalReturn to SPC from inside.
bool rte(Ctx& c, ::dream::Memory& m);

// The full runtime plugs its scheduler and exception model in here (runtime/system.h). Without
// hooks the bare harness behaviour applies: no interrupts, TRAPA/RTE and untranslated targets
// throw.
class Hooks {
public:
    virtual ~Hooks() = default;
    virtual void on_poll(Ctx& c, ::dream::Memory& m) = 0;
    virtual void on_trapa(Ctx& c, ::dream::Memory& m, std::uint32_t imm) = 0;
    virtual void on_rte(Ctx& c, ::dream::Memory& m) = 0;
    virtual void on_untranslated(Ctx& c, ::dream::Memory& m, std::uint32_t target) = 0;
    // A non-local return landed at `pc`, an address inside translated code with no resume label,
    // or inside untranslated code. Unlike on_untranslated there is no return address to stop at:
    // the runtime interprets from `pc` until the next non-local return or the run ends.
    virtual void on_resume(Ctx& c, ::dream::Memory& m, std::uint32_t pc) {
        on_untranslated(c, m, pc);
    }
    // True while an exception delivered by the runtime is on the C++ stack, so an RTE unwinds to
    // it. Otherwise RTE is a plain transfer to SPC with SR from SSR (Katana's startup enters the
    // program that way); the interpreter continues there instead of returning.
    virtual bool in_exception_frame() const noexcept { return true; }
};
void set_hooks(Hooks* hooks) noexcept;
Hooks* hooks() noexcept;

// An instruction the emitter does not lower yet (FPU before WP1.3, SH-4A only, illegal).
[[noreturn]] void unimplemented(Ctx& c, std::uint32_t raw, std::uint32_t pc);

// Optional trace hook, emitted under DREAM_TRACE at every function return; the harness uses it
// to compare against the interpreter.
void trace_return(Ctx& c, ::dream::Memory& m, std::uint32_t fn_address);

// Brackets one guest function call so it can be compared against the interpreter
// (devinterp/replay.h). Emitted around the plain entry point under --replay-hooks, never around the
// resume entry, because a resume is the middle of a call and not the start of one. Does nothing
// when no comparison harness is installed, which is always in a release build.
struct ReplayScope {
    // `assumed_mode` carries what the emitter compiled this function for: bit 0 set when PR was
    // known and bit 1 its value, bit 2 set when SZ was known and bit 3 its value. A bit the
    // emitter left unknown already gets a runtime branch and is not worth checking.
    ReplayScope(std::uint32_t fn_address, std::uint32_t assumed_mode) noexcept;
    ~ReplayScope();
    ReplayScope(const ReplayScope&) = delete;
    ReplayScope& operator=(const ReplayScope&) = delete;

private:
    // Both are unused in a release build, where the body compiles away with the dev interpreter.
    [[maybe_unused]] std::uint32_t fn_;
    [[maybe_unused]] std::uint32_t assumed_;
    [[maybe_unused]] int uncaught_;
};

}  // namespace dream::sh4
