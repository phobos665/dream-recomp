# Differential harness (WP1.5)

The translator is checked against Flycast's SH-4 interpreter, not against hand-written expected
values. The same program, initial registers and memory run on both sides; the final context must be
identical bit for bit. This is the oracle ADR 5 chose (see `decisions/README.md`) and the tie-breaker
for every floating-point choice in `runtime/include/dream/runtime/sh4/ops.h`.

## Pieces

| Piece | Where | What it does |
|---|---|---|
| Oracle entry point | `tools/flycast/oracle/dream_oracle.cpp` | Added to Flycast's libretro core as a local patch. `dream_oracle_run(image, base, entry, stop_pc, regs[16], fpscr, max_steps, dump, hash_addr, hash_len)` loads a raw image into guest RAM, sets the registers, single-steps the interpreter until `pc == stop_pc` (the harness sets `pr` to a sentinel, so the top-level `rts` stops the run) and writes the context as JSON |
| Core build recipe | `tools/flycast/oracle/build_oracle.sh`, `flycast-oracle.patch` | Clones the pinned Flycast commit, applies the two-file patch (adds the source to the libretro target, exports the symbol on macOS), builds `flycast_libretro.{dylib,so}` |
| Oracle driver | `tools/oracle/oracle.py` | ctypes wrapper; runs one case and writes the dump |
| Recompiled runner | `tests/sh4/runner.cpp` → `dream_sh4_runner` | Links every generated test program; selects the entry by program name, runs it on `BareMemory`, writes the same JSON shape |
| Comparison | `tools/oracle/compare.py`, `differential.py`, `cases.json` | Field-by-field diff; the batch driver runs every case and returns the mismatch count |

The dump shape is `{steps, pc, stopped, r[16], sr, t, gbr, vbr, pr, mach, macl, fpul, fpscr, fr[16],
xf[16], mem_hash?}` with hex strings for registers and FNV-1a over `hash_addr..+hash_len` when asked.
`compare.py` checks `r`, `t`, `gbr`, `mach`, `macl`, `fpul`, `fpscr`, `fr`, `xf` and `mem_hash`;
`sr` outside T, `pr` and `vbr` are harness-owned and only reported.

Both sides start from the harness state used by `tests/sh4/test_programs.cpp`: `sr = 0x700000F0`,
`fpscr = 0x00040001` (Katana default), `r15 = 0x8C00F400`, everything else zero, and the image loaded
at its link base. The stop address is `0x8C00FFF0`.

## Running it

```sh
tools/flycast/oracle/build_oracle.sh                      # once; ~5 min, prints the core path
export DREAM_FLYCAST_CORE=build/flycast-oracle/flycast_libretro.dylib
cmake -S . -B build/dream -DDREAM_FLYCAST_CORE=$DREAM_FLYCAST_CORE
cmake --build build/dream && ctest --test-dir build/dream -R differential --output-on-failure
# or directly:
python3 tools/oracle/differential.py --runner build/dream/tests/sh4/dream_sh4_runner --keep /tmp/diff
```

The CTest entry `translator.differential` is registered only when `DREAM_FLYCAST_CORE` is set, so CI
and plain checkouts are unaffected. Keep the dumps (`--keep DIR`) when investigating a mismatch; each
case leaves `NAME.oracle.json` and `NAME.recomp.json`.

Adding a case: append to `tools/oracle/cases.json` (`image`, `base`, `entry`, optional `regs`,
`fpscr`, `hash`, `program`). Adding a program: see `tests/sh4/README.md`; the runner picks it up from
`programs.cmake` automatically.

## GCC-compiled C unit

`tests/sh4/cprogs/c1_idioms.c` is built freestanding in the toolchain container (`make` there
produces the flat `.bin` and an `nm` listing) and translated like the hand-written programs. Its
function ranges come from `gen_specs.py`, which runs the translator's discovery seeded with the
symbols, so libgcc's shared division tails are included as branch-derived functions. It covers the
shapes a compiler produces that hand-written programs do not: `__sdivsi3_i4i`/`__udivsi3_i4i`, a
jump-table `switch`, 64-bit multiply and subtract, struct copies, insertion sort, the float ABI,
recursion and byte loops. 32 cases in `tools/oracle/cases.json` exercise them; the first run found
the shared-tail branch gap in discovery (see `progress.md`, WP1.4).

## Crazy Taxi slice

`games/crazytaxi/slice.cmake` adds functions from the game itself as programs when the owner-supplied
`1ST_READ.BIN` is present (never in CI). `tools/oracle/pick_slice.py` chooses them from the emitted
code: leaf functions (no calls, traps, switches, GBR/SR/VBR use or folded absolute addresses), either
pure (`--pure`) or memory-writing (`--writes`). `games/crazytaxi/oracle-cases.json` runs each with a
spread of float arguments in fr4–fr11 and pointer arguments in r4–r9:

```sh
python3 tools/oracle/differential.py --runner build/dream/tests/sh4/dream_sh4_runner \
    --cases tools/oracle/cases.json --cases games/crazytaxi/oracle-cases.json
```

Memory-writing functions get a `fill` region (`ADDR:LEN:SEED`) that both sides populate with the same
generator before the run, and a `hash` over it afterwards. Every aligned word of the fill is itself an
aligned pointer into the region, so struct-walking code stays inside it. Functions that still depend on
state the harness cannot seed (globals read through pointers in zeroed RAM, loops on data, MMIO) show up
as runner faults, step-limit hits or timeouts and are pruned by hand; 8 of 39 picked functions went
that way. The slice is 31 functions: 12 pure FPU routines and 19 that write memory (vector/matrix code
with `frchg`, `fipr`, `ftrv`, `fschg` pair moves, `fsqrt`, `fcmp`; integer struct and table code).

## Golden replay on every host

`tools/oracle/write_golden.py` turns a run's oracle dumps into one text file per case under
`tests/sh4/golden/` (inputs plus the interpreter's final state, 796 KB for 78 cases), and
`tests/sh4/test_golden.cpp` in `dream_emit_tests` replays them all: same image, fill, registers and
FPSCR, then a bit-for-bit comparison of every register, T, FPSCR, FR/XF and the hashed memory words.
Goldens whose image is absent (the Crazy Taxi slice on CI) are skipped and counted. This is ADR 16
item 4: the states were captured on ARM64 and CI checks them on x86-64 Linux and Windows.

Regenerate after adding cases or changing the fill generator:

```sh
python3 tools/oracle/differential.py --runner build/dream/tests/sh4/dream_sh4_runner \
    --cases tools/oracle/cases.json --cases games/crazytaxi/oracle-cases.json --keep /tmp/dumps
python3 tools/oracle/write_golden.py --dumps /tmp/dumps --out tests/sh4/golden \
    --cases tools/oracle/cases.json --cases games/crazytaxi/oracle-cases.json
```

Only cases that agreed with the oracle are worth committing; the writer records whatever the oracle
produced, so run the differential first and fix mismatches before regenerating.

## Status

2026-09-11: all 78 cases (p1–p7, the C unit and the 31-function Crazy Taxi slice) agree with Flycast at commit 0abac34 (integer ALU, DIV1 sequence,
addressing modes, calls, single/double FPU with `fschg` and `lds fpscr`, both switch-table idioms,
and the FPU edge cases of `p7_fpuedge`).

Findings so far, each fixed the same day:

| Symptom | Cause | Fix |
|---|---|---|
| `1/3` stored as 0x3EAAAAAB, oracle 0x3EAAAAAA | Clang folded the division at compile time under round-to-nearest; the guest runs round-toward-zero | `-frounding-math` / `/fp:strict` on all dream targets (ADR 16 revision) |
| `fsca` sin 90° = 0x3F7FFFFF, cos 90° = tiny positive; oracle 1.0 and -0.0 | Live `sin`/`cos` under the guest's rounding mode; hardware (and Flycast) use a table | Flycast's hardware-captured table, `runtime/src/sh4/fsca.cpp` |
| `ftrv` components off by 1–3 ulp | Single-precision accumulation with a rounding per step | Double accumulation, rounded once (Flycast's evaluation), also for `fipr` |

Confirmed agreements worth knowing: FMAC is fused on both sides (2^-46 survives), FTRC saturates to
0x7FFFFFFF/0x80000000 and maps NaN to 0x80000000, FLOAT honours round-toward-zero (INT_MAX becomes
0x4EFFFFFF), denormal inputs are flushed under DN=1, FSRRA is `1/sqrtf`.

Harness bug found by the slice: Flycast programs the host rounding mode from FPSCR and leaves it
there, so Python parsed the next case's float literals in round-toward-zero (0.1 became 0x3DCCCCCC).
The oracle now saves and restores the host FP environment and re-applies the guest mode on entry.

Cross-ISA: `dream_emit_tests` carries p7's Flycast-captured state and passes on Windows x86-64 (MSVC,
`/fp:strict`) and Linux x86-64 as well as on the ARM64 Mac that captured it. That is ADR 16 item 4 in
practice, without the oracle core in CI.

Known gap: NaN payloads (see ADR 16 revision).

## Why the libretro core

Flycast's gtest binary on macOS is the Cocoa application (`main` comes from the app shell), so it
cannot host the oracle. The libretro build is already a shared library with a headless core, needs no
window system, and links in ~5 minutes; a 90-line entry point on top of it is the smallest change that
gives a scriptable interpreter. The patch is kept as a diff against a pinned commit rather than a
fork or submodule: it touches two files, and the runtime's own Flycast-derived components (ADR 1) will
decide how Flycast is vendored when Phase 2 starts.

## Comparing whole runs (WP3.2 bring-up)

The pieces above compare one function against the oracle with hand-chosen inputs. Bring-up needs the
other end of the scale: a title runs for minutes and goes wrong somewhere. Two things make that
comparable at all.

**Runs have to repeat.** `--rtc-seed N` fixes the console clock, which is otherwise seeded from the
host clock. Without it two runs of the same build diverge and nothing below means anything.

**Every guest store is hashed.** `--write-hash FILE` writes one line per frame with a rolling hash
of every store and the number of stores so far, which is cheap enough to leave on. Two runs that
agree line for line did the same thing. `--write-log FILE --write-log-range FROM:COUNT` then dumps
the individual stores in a window with the call site, so the first line that differs names the
store.

`--mask-segment` ignores the top three bits of a 32-bit stored value. A stored pointer carries the
segment the code was running in, and the same function reached through P1 and through P2 pushes a
different return address on real hardware; a statically translated build cannot reproduce that and
it is not a bug, since the runtime masks the segment off every access. Without the mask, Crazy Taxi
shows 79 such differences in its first two frames and they drown everything else.

**What this cannot do, and why.** Comparing a translated run against `--interpret` breaks down once
interrupts start. The two engines deliver interrupts at different instructions by design: the
translated build checks at function entries and loop back-edges (ADR 4), the interpreter can take
one anywhere. Crazy Taxi's two runs agree for 23 frames and then part at a store made from the
interrupt vector at VBR+0x600 in one run and from ordinary code in the other. That is correct
behaviour on both sides.

So whole-run write comparison answers "is this build deterministic" and "do these two builds of the
*same* engine agree", not "is the emitter right". For that, compare a function against the
interpreter with identical inputs, which is what the pieces at the top of this file do, and what a
record-and-replay pass over a real run would do at scale.

## Comparing one function against the interpreter (WP3.2)

`runtime/devinterp/replay.{h,cpp}` asks the narrow question the two comparisons above cannot. At a
guest function's entry the context is saved and stores start being journalled. At its exit the
journal is undone, putting memory back exactly as it was, the interpreter runs the same function
from the same context, and the two exits are compared: registers, floating-point registers, and the
sequence of stores. Then the interpreter's stores are undone, the translated ones put back, and the
run carries on as if nothing had happened.

It does not care how the function was called, which is what defeats hiding a function
(`docs/runtime-devinterp.md`), and it does not care when interrupts land, which is what defeats
comparing whole runs.

Translate with `--replay-hooks` and run with `--replay`, or `--replay-only addr,addr` for named
functions. On Crazy Taxi it compares **170 million calls over 1,200 frames at 0.8x real time**.

Four kinds of call cannot be compared and are counted rather than guessed at:

| Skipped | Why |
| --- | --- |
| a device write | it cannot be undone and must not be repeated |
| an interrupt during the call | the handler's stores are in the journal and the replay delivers none |
| a non-local return | the replay would have to be unwound the same way |
| too long | a function that does not return within the journal's bound, such as the entry point |

The last one needs care. The first function a run enters never returns, so a comparison waiting for
it would journal the whole run and nothing else would ever be compared. A comparison that grows past
the bound is abandoned, and the function is remembered so it is not tried again; after a handful of
attempts the entry point and the main loop are set aside and the short, deep functions that matter
get their turn.

Four bugs in the harness itself, worth knowing because each produced confident nonsense:

- **Re-entrancy.** The replay calls translated functions natively, and their hooks started
  comparisons inside the comparison and threw away the journal being compared. Every result was a
  false "the interpreter stored nothing".
- **Interrupts.** Without the skip above, every call the translated run was interrupted in reported
  a disagreement at the interrupt vector's own store.
- **Unwinding.** A non-local return thrown inside the replay escaped into a real run whose stack it
  was not thrown for, and aborted the process. It is caught and the comparison voided.
- **Side effects that are not stores.** The skip for device writes only saw stores going through
  `store()`. A store-queue flush reaches the hardware through `write_burst`, and a device read can
  change state as much as a write can, so neither tripped it. The consequence was that every
  function which hands the Tile Accelerator a display list was replayed, the hardware got the list
  twice, and the emitter was blamed for the difference: one such function accounted for 3,575 of the
  reported disagreements and none of them were real. Bursts and device reads now mark the journal
  too.

The general rule, and the one to apply to the next skip: **the comparison is only valid over effects
the harness can put back.** Anything that leaves the emulated machine, in either direction, has to
be declared rather than discovered.
