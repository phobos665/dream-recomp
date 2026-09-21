# Review: GeneralAtrox/flycast_Redux

Read 2026-09-21. **Adopt it — as an external, pinned reference implementation that emits SQLite
traces we diff against, not as code we vendor.** It is real, and thicker than its description
suggests.

## What it is

A genuine in-tree fork of `flyinghead/flycast`, 24 commits ahead and 44 behind upstream, 244 files
changed. Most of the volume is new code under `core/research/` (~60 files) plus a vendored SQLite
amalgamation; upstream files carry deliberately small diffs and the repository states a
"keep upstream diffs minimal" rule that appears to hold. **Everything is off by default**, behind
`research.*` config options.

- Emulator side: observation buses per subsystem, a SQLite recorder, an asio TCP JSON-lines control
  server, SH-4 PC checkpoints.
- Host side: Python tooling, and a `.mcp.json` registering an MCP server of about thirty tools.
- Roughly 37 googletest suites under `tests/src/research/`, including a dynarec-versus-interpreter
  differential test.

Single author, no stars, last pushed 2026-09-15. High quality, and unreviewed one-person work.

**Licence: GPL-2.0**, same as upstream and as us, so nothing changes about our position. Worth
noting the cheaper path though: *using* it as an external oracle that writes SQLite files raises no
derivative-work question at all, where lifting code from it would.

## What is actually valuable

Not the MCP layer, which is a thin wrapper over a control socket and some canned SQL — we could use
the socket and the database directly and skip MCP entirely.

The value is the **attribution model** underneath. `sh4_events` carries 44 columns: event type
(begin, end, abort, read, **write**, exception, call, return), tick, pc, next_pc, opcode, memory
address, width and value, call kind, target and return pc, and a full integer register snapshot
with `pr`, `gbr`, `vbr`, `mach`, `macl`, `sr`, `fpul`, `fpscr`. Every hardware bus — Maple, PVR
TA/draw/present, GD-ROM, AICA, CD-DA — carries `init_pc`/`init_pr`/`init_opcode`, joining each
hardware effect back to the SH-4 instruction that caused it.

That is precisely the thing our own instruments cannot do: **show a register and a memory location
at the same instant, attributed to a PC.**

## What it would answer

**The Crazy Taxi divergence** (our interpreter and our translated code disagree from frame 1, same
cycle count and same write count, different content). Record writes only over a bounded address
range with input replay and a pinned RTC, then diff the ordered write stream against ours. That
gives the first divergent write *with the writing PC*. Two conditions: we must emit a comparable
ordered write log, and the runs must be input-identical. Diff on write sequence, not absolute
ticks — their tick numbering will not match ours.

**The Rayman 2 contradiction** (`docs/progress.md` in that project: the register trace says PR came
from `0x8CFFFFD4` holding `0x00100000`, our write-watch says that word last held `0x00000008`).
`writes_timeline` on that address gives every write in order with writer PC, value and width, in
the same ordinal-ordered stream as the return events. The disagreement resolves by inspection
rather than inference. A checkpoint on `pc=0x8C010D22` gated on that RAM word gives a coherent
register-and-memory snapshot at the exact instruction.

**The fn_0c07b760 emitter defect**: partial only. There are no per-instruction float register
columns, so isolating it means stepping checkpoint by checkpoint rather than diffing a trace.
Usable, tedious.

## Cost and risk

**Five to eight engineer-days to a first-divergence answer**, of which roughly half is work we want
regardless (emitting an ordered write log from both our paths, and a differ).

- Build on macOS ARM64 via CMake: 1–2 days, and this is the main unknown. The signs are good — the
  research sources are gated only on `NOT LIBRETRO`, SQLite is a portable amalgamation, and the
  ARM64 dynarec carries the full marker lowering rather than being x64-only — but the repository
  says Windows is its primary platform, its CI has no macOS job, and nobody has built this
  configuration on Darwin. **Not verified: the review read build files and did not build it.**
- Pin a deterministic run (input replay, RTC seed, threaded rendering off): 1 day.
- Our own ordered write log plus differ: 2–3 days.

Risks, in the order they are likely to bite:

1. **Trace size.** The SH-4 bus costs roughly a tenfold slowdown, and per-instruction capture is
   order half a gigabyte to a gigabyte *per frame*. Writes-only with PC and address filters is the
   only viable mode.
2. **Determinism.** A diff means nothing unless both runs take the same input path. Validate by
   diffing two of their runs against each other before trusting any comparison with ours.
3. **Flycast is not hardware.** Where it and we disagree, it is not automatically right.
4. **Fork maintenance.** Pin a SHA, treat it as an external tool, and keep it out of our build.

## Recommendation

Pin a SHA and use it out-of-tree. Do not vendor it: that reopens a GPL question we have no need to
reopen for something we only need to *run*. Build it once, prove determinism, and point it at the
Rayman 2 contradiction first — that is the smallest question with the clearest answer, and it will
tell us whether the tool earns the rest of the investment.
