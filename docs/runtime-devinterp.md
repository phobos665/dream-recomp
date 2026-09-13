# Development interpreter (WP2.7)

The function-table miss handler for development builds (ADR 2). When emitted code transfers control
to a guest address that has no translation, `System::on_untranslated` records the address in the
fault log and, when `DREAM_DEV_INTERPRETER` is on, interprets the guest code from memory until
control comes back to the caller. Release builds compile the interpreter out: the same miss throws
`dream::UntranslatedCall` and the launcher prints the fault log, which is the Initial D posture the
ADR asks for.

The point is the discovery loop. A title boots on the first run instead of the run in which
discovery is complete; every address the interpreter had to execute is in the log for the next
translate pass; and the runtime (PVR, AICA, Maple) can be built against a running game meanwhile.

## What it is built from

Not a port of Flycast's interpreter, which ADR 2 originally named. It is the emitter's semantics
executed at run time:

- `translator/src/sh4/decoder.cpp` decodes each word (split into its own library, `dream::sh4dec`,
  so the runtime does not link the emitter, discovery or TOML). Verified against binutils on all
  65,536 words.
- `runtime/include/dream/runtime/sh4/ops.h` provides every non-trivial operation (DIV1, MAC, FPSCR
  writes with bank swaps, FSCA table, FIPR/FTRV, FTRC saturation). Verified bit for bit against
  Flycast's interpreter by the differential harness.
- Each `case` in `runtime/src/devinterp/interpreter.cpp` is the run-time form of the statement
  `translator/src/emit/emit.cpp` emits for that opcode, including delay-slot ordering (branch
  operands and PR captured before the slot executes; the slot uses its own address for PC-relative
  forms) and the treatment of an illegal slot instruction as a fault.

Reusing the verified pieces gives a stronger correctness argument than adapting a second
implementation: the golden replay test runs every captured Flycast state through the translated
function **and** through the interpreter and requires identical results (see below). It also means
the runtime has one context layout and one memory model, with no adapter layer.

## Control-flow contract

`devinterp::run(c, m, return_to, options)` starts at `c.pc` and returns when:

| Event | Result |
|---|---|
| Any transfer whose target equals `return_to` (an `rts` to the caller's PR, or a `jmp` there) | `Stop::Returned` |
| An `rte` while an exception delivered by the runtime is on the C++ stack (`Hooks::in_exception_frame()`) | `Stop::Rte`; the delivering frame resumes |
| `Options::max_instructions` reached | `Stop::StepLimit` |

An `rte` **outside** a delivery frame is a jump: SR comes from SSR, control continues at SPC (natively
if SPC is translated). Katana's startup uses exactly this — a full context restore ending in a RAM
stub that reloads r0–r2 and executes `rte` to enter the program — so the first real boot exercised it.
Such an RTE is never taken as a return through PR, even when SPC happens to equal the return address
(both are zero in an uninitialised context, and "returning" there would hide the fault of executing
address 0). The emitted code's `rte(c, m); return;` has the same distinction to make; it is an
emitter follow-up recorded in the progress ledger.

`bsr`/`jsr`/`bsrf` whose target **has** a translation call it natively and resume at the return
address; `jmp`/`braf` into a translated function are tail calls, after which interpretation resumes
at PR (and stops if that is `return_to`). Targets without a translation are simply interpreted, and
their own `rts` comes back through PR, so nesting needs no explicit stack. `Options::call_translated
= false` interprets everything, which is what the pure-mode golden replay uses.

Interrupt polling mirrors emitted code: at entry, on back-edges (a taken branch to `target <= pc`),
after every call and return. `SLEEP`, which the emitter does not lower, advances the clock to the
next scheduled event and polls, so a sleeping guest keeps making progress. `TRAPA` goes through the
ABI hook like emitted code. `c.pc` always holds the current instruction, so faults and traps report
the exact address.

## Seeing what it did

Bring-up tools added while chasing the first Crazy Taxi task switch (2026-09-12); all dev-only:

- `crazytaxi_boot --interpret` runs the whole title through the interpreter (exception handlers
  too, via `System::interpret_all`), calling only the BIOS HLE hooks natively. If the interpreted
  boot gets further than the translated one, the bug is in emitted code.
- `DREAM_TRACE_RETURNS=path` writes one line per guest return (rts pc, r0..r15, PR, SR) from the
  interpreter and from translated code built with `--trace` (configure the game tree with
  `-DDREAM_TRANSLATE_EXTRA_FLAGS=--trace`). `tools/trace/diff_returns.py t.log i.log` masks RAM
  mirror bits, collapses polling loops, resynchronises across timing shifts and prints the first
  return whose state differs. It found a function cut off in a delay slot and a fall-through the
  emitter dropped, each in minutes.
- `DREAM_TRACE_NONLOCAL=1` logs every non-local return, resume and interrupt entry/exit with PR
  and r15 (`docs/emitter-design.md`, "Non-local returns").
- `DREAM_INTERP_RANGE=lo:hi` hides the translations in that range from the function table so the
  interpreter runs them, for bisecting by address. Only calls that go through the table are
  affected (register-indirect calls, resumes); direct `bsr`/`jsr`-to-literal calls are compiled
  in, so a culprit reached only directly needs the return-trace diff instead.
- `DREAM_WATCH_WRITE=addr[:len]` logs every guest store into a range with the last call site, PR,
  r15 and frame; `DREAM_TRACE_GDROM=1` logs the GD-ROM syscalls; `DREAM_ARM7_WATCH=pc[,n]`
  prints the ARM7 registers when the sound driver is about to execute `pc`; `--dump-aram FILE`
  writes sound RAM at the stop (disassemble it with the container's `arm-eabi-objdump -D -b
  binary -marm`); the report includes an ARM7 pc histogram and the AICA/Holly interrupt state.
- The launcher report lists image bytes rewritten in RAM (Katana patches a few pool words; anything
  large is a stray write) and, for every non-local return into untranslated code, the target.

`DREAM_DEVINTERP_TRACE=N` prints the first N interpreted instructions to stderr (address, word,
disassembly, r0/r15/PR/SR/SPC/SSR). On the first Crazy Taxi boot this showed, in six lines, that the
untranslated target was a run-time copy of the context-restore stub and that the context it restored
was all zeros, which is the next bring-up question, not an interpreter one.

Interpreter options that exist for these tools: `Options::native_lo/native_hi` (always call the
BIOS hooks natively even with `call_translated` off), `Options::catch_nonlocal` (the harness lets a
non-local return thrown by natively called code continue interpreting at its target; the runtime
leaves it off so `run_guest` handles it), and `Hooks::on_resume` (resume at an address inside
untranslated code: interpret from there with no return address to stop at).

## Tests

- `runtime/tests/test_devinterp.cpp`: an untranslated callee (hand-assembled in RAM, with a
  delay-slot instruction) returning to a translated caller; interpreted code calling a translated
  function natively and resuming; an untranslated interrupt handler running in register bank 1 and
  unwinding through `rte`; the step limit. In the release configuration the same file checks that
  the miss is recorded and throws.
- `tests/sh4/test_golden.cpp`: every golden (p1–p7, the GCC C unit, the Crazy Taxi slice) replays in
  three modes, translated, interpreted, and hybrid (calls into translated code made natively), and
  all three must equal Flycast's captured state.
- `crazytaxi.boot` (`games/crazytaxi/CMakeLists.txt`, needs the owner's image) boots the real game;
  it is registered only in the development configuration because the release build stops at the
  first untranslated target by design.

CI builds and tests both configurations on Linux (ADR 2's consequence: the fallback must never
become load-bearing).

## Known limits

- Cycle counts are one per instruction, as in emitted code; no per-opcode timing.
- A handler that never returns (a longjmp-style task switch) is not modelled, the same limit as the
  cooperative delivery model in WP2.2.
- `LDTLB` and the SH-4A-only forms fault, as they do in emitted code.

## Isolating a bug to a function (2026-09-13)

`DREAM_INTERP_RANGE=lo:hi` hides the translations in an address range so the interpreter runs them
instead. Bisecting that range against a fault finds a neighbourhood, and no more: a range can only
say "somewhere between here and there", and when two distant functions are both needed the range
between them is large and mostly innocent. Crazy Taxi's frame-1111 fault bisects to
`0x0c035f40:0x0c084910`, which is 322 KB and 1,639 functions.

`DREAM_INTERP_FUNCS=lo:hi,lo:hi,...` hides an arbitrary set of whole functions, which is what
shrinking needs: delta debugging asks whether a subset still avoids the fault, and keeps dropping
members until putting any one back brings it back. `tools/` has no driver for this yet; the one used
on 2026-09-13 is small enough to rewrite, and the mechanism is the part worth keeping.

## What hiding a function cannot do, and why (2026-09-13)

**Neither switch can redirect a call the emitter resolved statically.** The emitter writes a direct
C++ call for a known target:

```cpp
c.pc = 0x0c08518eu; fn_0c085320(c, m);
```

That call is compiled in. `set_interpret_range` and `set_interpret_functions` are consulted by
`find_function` and by the resume path, so they only affect **indirect** calls and code entered
through the miss handler. Hiding a function that every caller reaches by name changes nothing at
all: hiding `fn_0c084900`, which has four direct callers, produced a run byte-identical to the
baseline, with the same fault at the same frame and the same registers, and the same count of
untranslated targets.

The consequence is that **bisecting a fault by interpreted range does not isolate the faulty
function.** What it actually finds is a set whose interpretation cascades widely enough through the
interpreter's own dispatch to avoid the fault, because once an ancestor is interpreted its callees
are reached through `find_function` and can then be hidden too. That is why hiding five functions
from one call chain avoided Crazy Taxi's frame-1111 fault while hiding any one of them did nothing:
the set included the ancestor that is entered indirectly.

Nothing above is wrong as a way of finding a *neighbourhood* to read, and the range switch remains
useful for that. It is wrong as a way of naming the function whose translation is at fault, and the
bisection results recorded before this was understood should be read as "a region worth reading",
not "the bug is here".

**Two ways out, when this is picked up again.**

- Emit calls through a lookup under a development flag, so `fn_x(c, m)` becomes something like
  `call_guest(0x...)`, and every call becomes redirectable. This makes the existing technique work
  as intended, at the cost of a slower development build, and it is the smaller change.
- Compare one function against the interpreter with identical inputs: record its entry state during
  a real run, replay the same function through the interpreter from that state, and compare the exit
  state and the writes it made. That is immune to how the function was called, and it is immune to
  the interrupt-timing problem that stops whole-run comparison working
  (`docs/differential-harness.md`). It is the larger change and the better tool.

Two details matter, and getting either wrong makes the set behave unlike the range:

- **An entry is a whole function, not its first address.** The range hides every address inside a
  function, including the resume points a non-local return lands on. A set keyed on entry addresses
  alone leaves those translated, and the run then behaves differently from the equivalent range: the
  1,639 functions above crash when hidden by entry and are clean when hidden by extent.
- **Validate the verdict before trusting a search.** The first bisections run against this fault
  were wrong twice: once because the binary was rebuilt underneath running background probes, and
  once because the probe grepped for a message the launcher does not print, so every run was scored
  clean. Both produced confident, monotonic, wrong answers. Check that the predicate says CRASH on a
  known-bad run and CLEAN on a known-good one before starting.

## Naming the guest function that faulted

The guest program counter in the context is only refreshed where the emitted code checks for an
interrupt, so it names the last such point, not the instruction that faulted. Reading it as the
faulting address sends you to the wrong basic block, which is what happened while chasing Crazy
Taxi's frame-1111 fault.

The host stack does name it. Every guest function is emitted as a real C++ function called
`fn_<address>`, so walking the host stack at the fault and keeping those frames gives the guest call
chain exactly, innermost first. It costs nothing while a run is going well, needs no tracking, and
the launcher prints it with the registers when `DREAM_STOP_ON_UNMAPPED=1` catches an access:

```
unmapped read32 of 0x4bb9394c at frame 1111
  r4  0c39394c   r14 4bb9394c
  guest call chain (innermost first):
    fn_0c084900
    fn_0c07a2d0
    fn_0c0434d0
    fn_0c043ba2
    fn_0c02c810
```

That example is worth keeping for what the registers say as well: `r14` is `r4` plus `0x3F800000`,
and that constant is the bit pattern of the float 1.0. A float had been added to a pointer, which
points at the integer and floating-point sides of one instruction being confused, and is a far
narrower question than "somewhere in 322 KB".
