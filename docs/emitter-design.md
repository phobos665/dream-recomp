# Emitter design (WP1.2 to WP1.4)

How SH-4 machine code becomes C++. This is the contract between the translator (which writes the
C++), the runtime (which supplies the context, memory and helpers it calls) and the differential
harness (which checks the result against Flycast's interpreter). It refines ADR 4, ADR 6 and ADR 7.

## Units of emission

One C++ function per discovered guest function:

```cpp
void fn_8c010f40(dream::sh4::Ctx& c, dream::Memory& m);
```

Inside, each basic block is a label; guest branches within the function become `goto`. Calls
(`JSR`, `BSR`, `BSRF`) become direct C++ calls when the target is a known function, otherwise
`dream_call_indirect(c, m, target)`, which looks the address up in the sorted function table. `RTS`
is `return` when PR still holds the address the function was entered with; otherwise it is a
non-local return (below). A tail jump (`JMP`/`BRA` to another function's entry) becomes a call
followed by `return`, so the host stack mirrors the guest's PR discipline and recursion depth stays
bounded. A block that ends in a conditional branch still falls through on the not-taken path, and a
fall-through off the end of the function range that is not another function's entry goes through
`call_indirect` (the miss handler interprets it in dev builds; the untranslated-target log records
it).

## Register model

Guest registers live in the context struct (`runtime/include/dream/runtime/sh4/ctx.h`), and the
emitter accesses them as `c.r[n]`. No register caching in host locals in v1: it doubles the
emitter's complexity for a gain the host compiler largely recovers on its own once the function is
inlined into a few hundred lines of straight-line C++. Revisit after WP1.5 numbers exist.

The T bit is kept in `c.t` (0/1) and folded back into `c.sr` only where SR is observed (`STC SR`,
`RTE`, `TRAPA`, interrupt delivery). Every emitted comparison writes `c.t` directly.

## Instruction lowering

Each decoded instruction maps to a fixed C++ snippet with the register numbers substituted.
Representative forms:

| SH-4 | Emitted |
|---|---|
| `add r1,r2` | `c.r[2] += c.r[1];` |
| `addc r1,r2` | `{ uint32_t a = c.r[2], s = a + c.r[1] + c.t; c.t = (s < a) \|\| (c.t && s == a); c.r[2] = s; }` |
| `mov.l @(4,r14),r3` | `c.r[3] = m.read32(c.r[14] + 4);` |
| `mov.l r0,@-r15` | `c.r[15] -= 4; m.write32(c.r[15], c.r[0]);` |
| `mov.l 0x8c010f80,r1` (PC-relative literal) | `c.r[1] = 0x0c020000u;` when the pool word is in the image and not written at runtime; otherwise `m.read32(0x8c010f80)` |
| `cmp/hs r1,r2` | `c.t = c.r[2] >= c.r[1];` |
| `shad r1,r2` | helper `sh4_shad(c.r[2], c.r[1])` (sign-dependent direction, 32-bit special case) |
| `div1 r1,r2` | helper `sh4_div1(c, m_reg, n_reg)` operating on Q, M, T in `c.sr` |
| `mac.l @r1+,@r2+` | helper, 64-bit accumulate with S-bit saturation |
| `bt/s label` | `{ bool t = c.t; <delay slot>; if (t) goto L_xxxx; }` |
| `jsr @r1` | `{ uint32_t tgt = c.r[1]; <delay slot>; c.pr = pc + 4; dream_call(c, m, tgt); }` |
| `trapa #n` | `dream_trapa(c, m, n);` (runtime raises the exception through VBR) |

Literal pools: a `MOV.L @(disp,PC)` whose target lies inside the image is folded to a constant
**only** if the analysis marks that address read-only (never a store target and not inside a
writable data section). Otherwise it is a memory read. Getting this wrong silently breaks games that
patch their own tables, so the default is the read and the fold is an optimisation with a proof.

## Computed jumps

`braf Rn` and `jmp @Rn` are first tried as switch dispatch (`analysis/switch.cpp`): walk back from the
jump to the indexed load that produced the register (`mov.{b,w,l} @(r0,Rm),Rn`, possibly followed
by an `add` for table-relative entries), find the table base as a constant in either r0 or Rm
(`mova`, `mov.l @(disp,pc)`, or a copy of one), then read entries while they resolve to even
addresses inside the function. `braf` entries are relative to the instruction after the slot;
`jmp` entries are absolute unless an `add` made them table-relative. A third shape has no table: `and
#mask,r0; shll2 r0; braf r0` jumps into a run of `bra target; nop` stubs following the jump, so the
targets are `pc+4+k*4` for `k` up to the mask. A recovered dispatch is emitted
as `switch (target) { case addr: goto L_addr; ... default: call_indirect(...); return; }`, so an
index outside the recovered table still behaves correctly. Unrecognised computed jumps remain tail
calls through the function table.

## Delay slots

The slot instruction is emitted before the branch takes effect but after the branch condition and
target have been captured into locals, which is what the hardware does. The two hazards the Initial D
project hit are tested explicitly: a slot that modifies the register the branch reads (`jsr @r1; mov
r2,r1`) and a slot after `rts` that must still execute before the return.

## Memory access

`m.read8/16/32`, `m.write8/16/32`, `m.read64/write64` for FMOV pairs, all taking a guest virtual
address. The runtime masks to 29 bits and dispatches on the area bits (ADR 5). Store queues: the
emitter recognises stores to 0xE0000000 to 0xE3FFFFFF and `PREF @rn` in that range and routes them to
`m.sq_write32` / `m.sq_flush(addr)`.

## FPU (WP1.3)

Emitted according to the FPSCR analysis: for each FP instruction the analysis provides PR, SZ and FR
as known constants or "unknown". Known constants pick the single/double/pair snippet directly
(`c.fr[n]`, or a `double` reassembled from `c.fr[n]`/`c.fr[n+1]` in the SH-4's odd/even order).
The analysis (implemented in `emit.cpp`, `analyse_fp_modes`) is a forward data-flow over the
function's blocks: the entry mode comes from `FunctionSpec::fpscr_entry`, `FSCHG` toggles SZ, `LDS
Rn,FPSCR` yields a known mode when Rn was loaded from a literal pool word or an immediate in the same
block and unknown otherwise, `LDS.L @Rm+,FPSCR` is unknown, and modes merge at joins (known and
equal stays known). Calls were assumed to preserve PR and SZ; **Crazy Taxi disproves that**: a Kamui-style helper at
0x0C08538C toggles SZ with a single `fschg` and returns, and its callers rely on it, so the
assumption produced 312 spurious runtime SZ tests in one region. The fix is a callee summary per
function (net effect on PR/SZ from entry to return: identity, toggle, set-to-constant, or unknown),
computed bottom-up over the call graph with recursion resolved to unknown, and applied at `bsr`/`jsr`
sites whose target is known. **Implemented:** modes are a five-valued lattice per bit (0, 1, entry,
entry-toggled, unknown); `emit_unit` iterates summaries to a fixpoint (widening to unknown after
repeated change); `lds.l @r15+,fpscr` in a function that also saves FPSCR to the stack restores the
entry mode; a `braf` switch is not an exit. Blocks reachable only through computed jumps keep the
unknown mode and hence the runtime test, until switch tables are recovered.
Where a mode is still unknown at an FP instruction the emitter writes both lowerings behind a
runtime test of `c.fpscr` and counts it (`EmitResult::fp_runtime_branches`); per-function clones
(ADR 6 option b) are deferred until that count is non-zero on a real binary. FR needs no analysis:
`FRCHG` and `LDS FPSCR` swap the two 16-word banks so `c.fr` is always the front bank. `write_fpscr`
also programs the host rounding and denormal mode through the fenv layer (ADR 16).

## Interrupt checks (ADR 7)

`if (c.irq_pending) dream_deliver_irq(c, m);` at function entry and at every back-edge `goto`. The
virtual clock `c.cycles` is advanced by a per-block estimate (instruction count for v1).

## What the harness sees

The harness loads a KOS-built ELF, sets up `Ctx` identically for the emitted code and for the
interpreter, runs both until the program's exit hook, and compares the full `Ctx` plus a hash of RAM
at every function return (via a hook the emitter inserts under `DREAM_TRACE`). Divergence reports
the guest PC, both register files and the last few instructions.

## Runtime helper rules learned from the harness

- No large locals in `ops.h` helpers or their `.cpp` backends (the FSCA table once lived on the
  stack and overflowed Windows's 1 MB default). Tables go in static storage, filled on first use.
- Approximate instructions (`fsca`, `fsrra`, `fipr`, `ftrv`) follow Flycast's evaluation exactly,
  not the mathematically best answer; the differential harness is the arbiter
  (`docs/differential-harness.md`).
- Every inexact operation must be a run-time operation: the compiler is told the rounding mode is
  dynamic (`-frounding-math`, `/fp:strict`) so it cannot fold under round-to-nearest.

## Calls (updated 2026-09-11)

`bsr` is always a direct call. `jsr @rN` and `bsrf rN` become a direct call when the register holds a
constant from the same block (`mov.l @(disp,pc)` or `mov #imm`; for `bsrf` the target is the constant
plus the site address plus 4) and that constant is the entry of a function in the unit; otherwise
the register value goes through `call_indirect`, which resolves through the sorted function table
with RAM-alias matching. `EmitResult::direct_calls` counts the resolved sites; the report from
`dream-translate game` prints it next to the table-lookup count so a title's indirect-call surface is
visible before it runs.

## Non-local returns (2026-09-12)

Katana's task switcher (Crazy Taxi 0x0c088560 / 0x0c0884ec, a setjmp/longjmp pair) saves every
register including r15 and PR into a task block and later reloads them and executes `rts`: the
guest continues in a different task, at the return address of *that* task's earlier save call. A
C++ `return` would instead unwind into the caller's host frame with the other task's registers
(that is how the first boot corrupted r10 and jumped through a task struct). The same happens to
an interrupt handler whose `rte` resumes a different context, and to Katana's startup `rte` that
enters the first task.

The contract, all in `runtime/include/dream/runtime/sh4/abi.h`:

- Every emitted function body is the **resume entry** `static void NAME__resume(Ctx&, Memory&,
  uint32_t resume_pc)`; `NAME(c, m)` calls it with 0. The body starts with
  `entry_pr = c.pr` and, when `resume_pc` is set, a `switch` that jumps to the label for that
  address. Every block start and every call-return address has a label, so control can re-enter a
  function at any point where it could have been suspended. Registers live in `c`, so no host
  state is lost. The function table entry carries `end` and the resume pointer.
- `rts` reads PR before its delay slot; if `rts_target != entry_pr` and the body was entered
  normally, it calls `nonlocal_return(c, m, rts_target)`, which throws `NonLocalReturn`. A resumed
  body returns plainly: after a non-local return the host stack no longer mirrors the guest's, so
  `run_guest` continues at PR after every return anyway.
- `run_guest(c, m, entry)` is the top loop: it calls the entry, catches `NonLocalReturn`, and
  re-enters through `resume_at(pc)`, which finds the function containing `pc` (entry or interior
  label) or hands the address to `Hooks::on_resume` (the dev interpreter, with no return address
  to stop at). It ends when control returns to the PR it started with.
- Poll points set `c.pc` to their exact address before `deliver_irq`, so an SPC saved there is
  resumable. An `rte` whose SPC/r15 differ from the interrupted frame's is a task switch: the
  runtime throws `NonLocalReturn(SPC)` from inside `rte()`, dropping the handler's and the
  suspended task's host frames. An `rte` with no delivery frame is a jump and goes the same way.

Cost: one compare per return, a `switch` that is skipped on normal entry, and a C++ exception per
task switch (a few per frame in Crazy Taxi). Tests: `p10_longjmp` (save/restore pair, one call
deep with a different stack), `p9_rte` (RTE as a jump), all three replay modes agree with Flycast.

## Interrupt poll (updated 2026-09-11)

Function entries and loop back-edges emit `if (c.cycles >= c.next_event) deliver_irq(c, m);`. The
runtime sets `next_event` to the next scheduler deadline (or to `cycles` while a pending interrupt is
masked); the bare harness sets it to UINT64_MAX on the first poll. `write_sr` zeroes it when BL or
IMASK change. See `docs/runtime-interrupts.md` for what `deliver_irq` does.

## The dev interpreter mirrors the lowering

`runtime/src/devinterp/interpreter.cpp` (WP2.7, ADR 2) is the run-time form of `lower()`,
`lower_fp_mode()` and `emit_delayed()`: one `case` per opcode, the same `ops.h` helpers, the same
delay-slot ordering (branch operands and PR captured before the slot; the slot's own address for its
PC-relative forms), the same illegal-slot fault. When a lowering here changes, change the matching
case there in the same commit; the three-mode golden replay (`tests/sh4/test_golden.cpp`) fails if
the two disagree on any captured case.

## A branch may target another branch's delay slot (2026-09-13)

The SH-4 allows this, and compilers emit it:

```
0c081204  bf  0xc081208     the target is the delay slot below
0c081206  bra 0xc081328
0c081208  tst #1,r0         the bra's delay slot
0c08120a  bf  0xc081210     reachable only by entering through the slot
0c08120c  bra 0xc081476
```

Entering through the slot runs that one instruction and then carries on at the instruction *after*
the branch pair. Discovery marks a delay slot as seen when it decodes the branch that owns it, so
the work item for a branch *to* that slot found the address already seen and was dropped, and
decoding never continued past the pair. The instructions after it were never emitted at all, and the
emitted fall-through landed in whichever block happened to be written next, which was the other
branch's target.

Nothing fails when this happens. The unit compiles, the tests pass, and the program takes a wrong
branch at run time. In Crazy Taxi's `fn_0c081200`, a display-list builder, **141 of its 312
instructions were missing**, and the wrong control flow is what eventually walks a pointer into
floating-point data and faults.

Discovery now distinguishes an address it actually began decoding at from one merely marked seen as
a delay slot, and continues at slot + 2 for the second kind. The emitter keeps a second, defensive
check that makes the instruction after such a pair a block of its own if discovery somehow did not.

## Decode coverage: a check that needs no run (2026-09-13)

Every byte of a function's range is one of three things: an instruction discovery decoded, a literal
pool entry some instruction reads as a constant, or code the emitted program is missing. The first
two are accounted for; the third is silent, because the unit still compiles and the tests still
pass, and the guest simply branches somewhere that was never translated.

`dream-translate` reports the split after every unit and lists the runs of sixteen bytes or more
that are neither. It reports and never fails, because a literal pool that nothing reads looks
exactly like missing code from here.

**The figure has to be taken over the union of intervals, not summed per function.** Function ranges
overlap: discovery gives a caller a range that covers callees it falls into, and those callees are
separately discovered and separately emitted functions. Summing per-function figures therefore
double-counts, and reports as missing the bytes that were compiled under another name. The first
version of this check did exactly that, and the numbers it produced were wrong by a wide margin:

| Binary | Per-function sum | Union of intervals |
|---|---|---|
| Crazy Taxi | 64.2% | **78.9%** |
| Tech Romancer | 63.9% | **86.2%** |
| Metropolis Street Racer, loader | &mdash; | **90.2%** |
| Metropolis Street Racer, game module | &mdash; | **88.8%** |
| Metropolis Street Racer, data module | &mdash; | **85.8%** |

The wrong metric also produced a striking coincidence, 64.2% against 63.9%, which was written up as
evidence that the shortfall was one systematic defect present in every title. It was not. Measured
properly, five binaries from three publishers spread between 79% and 90%, and Crazy Taxi is the
worst of them rather than the representative case.

**The rule worth keeping:** when a measurement produces a striking result, the first hypothesis is
that the measurement is wrong. That was the fourth instrument in one day to give a confident,
self-consistent, false answer, and the only one whose output had already been acted on.

What the corrected numbers do say is that discovery finds roughly four fifths to nine tenths of the
code, consistently, across unrelated titles and with no symbol table for three of them. The residue
still matters, because a gap that really is code fails the way Crazy Taxi's delay-slot case did, by
silently taking a wrong branch. But it is a tail to characterise, not a structural hole, and the
next step is to find out what is actually in those gaps before changing discovery.
