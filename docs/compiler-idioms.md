# Compiler idioms in Dreamcast executables (WP1.6)

Two retail binaries, decoded and analysed only (no execution), to see what code shapes the
translator must handle beyond the Katana/SHC world of the baseline title. Numbers come from
`tools/idioms.py` over the instructions the translator actually walked in each emitted unit (a
first attempt that counted every word inside function ranges was polluted by literal pools:
0x0003 words read as `bsrf r0`, and so on; walked instructions are the honest measure).

| | Crazy Taxi (USA) | Tony Hawk's Pro Skater 2 (USA) |
|---|---|---|
| Executable | 1,468,208 bytes | 3,510,532 bytes |
| Link base | 0x0C010000 (P0 alias; loader copies a stub to 0x8C004000) | 0x8C010000, plain NOP slide |
| Libraries seen in strings | Shinobi 1.62zr, NLOBJPUT/NLSPRITE, NAOMI&YUKA sound driver | none identifying the compiler |
| Functions found | 3,508 (301 branch-derived, 10 from `bsrf`) | 6,285 (137 branch-derived) |
| Median / p90 / max function size | 74 / 488 / 29,886 bytes | 72 / 400 / 20,634 bytes |
| Switch tables recovered | 42 of 42 discovered | 127 emitted, 113 counted by discovery |
| Whole-binary emit | 263,138 instructions, 176 fault stubs, 225 FP runtime branches, 28 unknown FPSCR summaries, 0 untranslated direct branches | 537,427 instructions, 30 fault stubs, 1,616 FP runtime branches, 852 unknown FPSCR summaries, 0 untranslated direct branches |
| Calls resolved to direct C++ calls | 8,342 `jsr`/`bsrf` + 2,357 `bsr`; 3,648 stay table lookups | 15,733 `jsr` + 1,616 `bsr`; 3,980 stay table lookups |

## Which compiler

Crazy Taxi is Hitachi SHC with the Sega libraries: callee-saved registers are pushed r14, r13, r12
(and `fmov fr15,@-r15` for a float callee-save), `pr` is saved after them, and `bsrf` is used for
119 position-independent calls, which GCC never emits. Tony Hawk's Pro Skater 2 has GCC's
fingerprints: r8, r9, r10 pushed in ascending order (701 prologues), the `mov.l r14,@-r15 ;
mov r15,r14` frame-pointer prologue with the matching `mov r14,r15 ; mov.l @r15+,r14 ; rts`
epilogue, `lds.l @r15+,macl` register restores, and 68% of delay slots left as `nop` against 25%
for SHC. Whether it is the KOS GCC or a CodeWarrior build cannot be told from strings; the shapes
below are what matter.

## Idioms and what the translator does with them

**Prologue and epilogue.** Both compilers keep the frame in r15 with optional r14 as frame pointer;
returns are `rts` with the last register pop in the slot (SHC) or a `nop` slot (GCC). Nothing here
needs special handling; the emitter treats stack traffic as ordinary memory accesses.

**Calls.** SHC: 11,197 `jsr @rN` (3,789 with the literal load immediately before, the rest earlier
in the block or through a register argument), 2,357 `bsr`, 119 `bsrf`. GCC: 18,908 `jsr @rN`
(3,228 right after the literal), 1,616 `bsr`, no `bsrf`. Discovery has always resolved `jsr` targets
through the literal pool within the block, but the emitter used to send every `jsr` through the
function table at run time. This pass changed both: `bsrf` targets are seeded and resolved the same
way (target = literal + pc + 4), and a `jsr`/`bsrf` whose register holds a constant from the same
block is emitted as a direct C++ call when the target is a function of the unit. Table lookups
dropped from 11,989 to 3,648 sites in Crazy Taxi and from 19,713 to 3,980 in THPS2; the remainder
are true indirect calls (function pointers, vtables, callbacks) and the Phase 3 fault log will show
which of them reach untranslated code.

**Delay slots.** SHC fills 75% of slots with useful work (21,039 of 28,078), including the register
pops after `rts` and the argument moves after `jsr`; GCC leaves 68% as `nop` (26,993 of 39,549). The emitter's delay-slot capture
(`emit_delayed`) is exercised far harder by SHC code; the test programs mirror the SHC shapes.

**Computed jumps.** SHC: 615 `jmp @rN` (mostly tail calls through literals), 30 `braf` tables via
`mova`, 16 `jmp` tables. GCC: 521 `jmp`, 172 `braf` tables via `mova` (GCC's preferred switch
lowering), 6 `jmp` tables. Both table idioms are recovered (`analysis/switch.cpp`); GCC's higher
count is why the C unit in the harness includes a dense `switch`.

**Literal pools and GBR.** SHC: 24,132 `mov.l @(disp,pc)`, 6,321 `mov.w`, 2,772 `mova`, 172
GBR-relative accesses (system variables addressed from GBR, set up by the Katana startup stub).
GCC: 34,914 / 20,903 / 631 / none. The runtime must initialise GBR as the stub does before any SHC
function runs; the translator folds literal reads when the pool word is inside the image.

**FPU.** SHC: 29,306 FPU instructions (11% of all), 267 `fschg` (pair moves for matrix/vertex
code), 51 `lds rN,fpscr` and 4 `lds.l @rN+,fpscr` (mode switches around double-precision maths),
43 `frchg`. GCC: 26,342 FPU instructions (5%), 126 `frchg` (XMTRX loads for `ftrv`), 47 `fschg`,
58 `lds rN,fpscr`, 7 `lds.l`. GCC's mode changes are less often traceable to a literal in the same block, which is
why THPS2 emits 1,616 FP runtime branches (0.3% of instructions) against Crazy Taxi's 225: the
first real data point for ADR 6's clone option, and still small.

**Multiply-accumulate and division.** SHC uses `mac.l`/`mac.w` sparingly (25/9); GCC prefers
`dmuls.l`/`dmulu.l` (279/134). Both use the `div1` step sequence (149 / 177); libgcc's shared
division tails are what produced the branch-derived-entry work in WP1.4.

**System instructions.** `trapa` 18 in SHC code, none walked in GCC code (Katana uses traps for a
few kernel services; the runtime gets a trap hook), `rte` 6 (interrupt handlers), `pref` 329 / 89
(store-queue submissions to the TA, hence the SQ model in WP2.1), `ocbi`/`ocbp`/`ocbwb` cache
control (25 / 28), `movca.l` 33 in GCC code (cache-line allocate: a plain store for us), `tas.b`
29 / 28 (spinlocks; atomic in a single-threaded runtime), `clrt`/`sett` for carry set-up, `sleep`
absent from both.

## Changes this pass produced

- `bsrf` targets are recovered when the register was loaded from a literal in the same block
  (target = literal + address of the `bsrf` + 4): discovery seeds them (10 new functions in Crazy
  Taxi) and the emitter calls them directly.
- `jsr`/`bsrf` with a constant target that names a function of the unit are emitted as direct
  calls instead of function-table lookups (8,342 sites in Crazy Taxi, 15,733 in THPS2).
- `tools/idioms.py` stays in the repo for the next title; run it on any emitted unit.

## What to expect from other compilers

CodeWarrior (Metrowerks) output on SH-4 resembles GCC's in slot usage and register order but uses
`bsr` more and has its own runtime library (`__ptmf_*`, `__init_data`); a title with those symbols
is the natural third sample when the owner has one. Windows CE titles are out of scope (ADR 15).
