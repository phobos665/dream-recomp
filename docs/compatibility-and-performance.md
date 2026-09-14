# Compatibility and performance

The two things standing between a recompilation that boots a title and one anybody would choose to
use. They are separate problems with separate owners, and this plan keeps them apart deliberately:
compatibility is about how much of a game runs at all, performance is about what it costs to run.

Opened 2026-09-14, after the first honest measurement against Flycast. The numbers below are the
baseline everything here is measured against; when they move, they move here.

## The baseline, measured

Crazy Taxi, attract mode, 640x480, real-time with audio, Apple M4, 2026-09-14. Ours is the release
build (`cmake -S . -B build-release -DDREAM_DEV_INTERPRETER=OFF`), which reached a usable state for
the first time on the same day -- the title's config had said "this title needs a development build"
until the discovery fixes in 45b4315 and 329c04e.

| | Ours (release) | Flycast |
| --- | --- | --- |
| CPU, steady state | ~43% of a core | ~37%, climbing through the run |
| CPU-seconds per wall-second | 0.40 | 0.31 |
| Peak resident memory | **163 MB** | **727 MB** |
| Unthrottled, headless | 3.5x real time | -- |
| Unthrottled, windowed | 1.9x real time | -- |

Flycast's CPU average is flattered by about ten seconds of boot and menu; steady-state the two are
closer than 0.40 against 0.31 suggests. Both figures are one attract-mode workload on one machine
and should be re-taken on the other two host platforms before anyone quotes them.

### What the profile says, which is not what anyone expected

A `sample` of the release build, heaviest symbols:

```
145  dream::mem::DcMemory::resolve
126  dream::mem::DcMemory::read32
 75  dream::mem::DcMemory::store<unsigned>
 45  dream::System::on_poll
 44  dream::sched::Scheduler::advance_to
 44  dream::aica::Aica::sample_tick
 25  dream::aica::Mixer::dsp_step
 21  dream::aica::Arm7::run
 17  dream::gen::fn_0c07d018__resume      <- the recompiled game code
```

**The recompiled code is a rounding error.** Nearly all of the time is the memory subsystem, the
poll and scheduler path, and the sound chip. Three consequences follow, and they reframe the whole
performance question:

- The single largest cost is `DcMemory::resolve`, which `progress.md` has carried as a deferred item
  under WP2.1 since 2026-09-11: "inlining the RAM fast path into emitted code (virtual call remains;
  measure in Phase 3)". The measurement is now done and it says this is the thing.
- AICA is roughly a quarter of the profile **and it runs with `--no-audio`**.
- The window costs about half the throughput on its own: 3.5x headless against 1.9x windowed, both
  unthrottled, so pacing is not the explanation.

We are not losing to Flycast on the quality of the recompilation. We are losing on infrastructure
around it that was always known to need work.

## Compatibility

### Where it stands

| Workload | Untranslated call targets | Release build |
| --- | --- | --- |
| Attract mode | 0 | runs |
| Gameplay | ~22,048 | faults on the first one |
| Memory-card path | calls `fn_0c07b760` | faults: excluded by `7b1015e` |

A development build absorbs all of these through the interpreter, which is why the title has been
playable there and nowhere else. A release build has nothing to fall back on, so every one of them
is a hard stop.

### The question this plan has to answer first

**Is the 22,048 a tool problem or a game-by-game problem?** The two have completely different costs
and completely different shapes, and today's work produced one data point for each:

- **Tool.** `0x0c081476` was a hole discovery could not reach, and the fix was a generic one: the
  runtime now resolves a jump into the middle of an already-translated function itself
  (`329c04e`), which removed 42,575 untranslated targets from attract mode and took it to zero.
- **Game.** The four `0x0C161xxx` seeds are addresses in one title's TOML, reached by non-local
  returns rather than calls, and no generic mechanism covers them because `resume_at` can only
  re-enter a function at one of its own block starts.

So the honest answer is probably "both", and the useful form of the question is **what proportion**.
That is a bounded measurement rather than an argument: take the 22,048, group them by cause, and
count. Until that is done, nobody can say whether closing WP3.2 is a fortnight or a research
programme, and nobody should be asked to guess.

### Sequence

1. **Classify the 22,048.** Group by the reason each is unresolved: a hole discovery never reached,
   a mid-function target the runtime should resolve, a non-local return to a non-block-start, a
   run-time-generated or copied region, or something else. Count each. This is the gate for
   everything below it and should be measured before anything is planned.
2. **Fix what is generic** in the translator and the runtime, which is where the leverage is: one
   fix that removes a whole class costs the same as one game-specific seed and is worth far more.
3. **Seed what is genuinely per-title**, with the discipline today's work established -- a real
   entry in a hole gets a seed, a mid-function address never does, because seeding one truncates the
   function containing it and silently corrupts what it computes.
4. **Repair the emitter defect behind `exclude = [0x0C07B760]`.** It is a quarantine, not a fix: the
   function is left to the interpreter because our translation of it computes addresses from
   floating-point values. A release build cannot use a memory card until it is repaired, and
   whatever construct is mistranslated will be mistranslated for every other title that uses it.

## Performance

Two investigations opened 2026-09-14, both measuring rather than arguing:

- `memory-footprint-study.md` -- what the 163 MB is made of, how much is irreducible, and what could
  come off it.
- `cpu-performance-study.md` -- the memory fast path, the windowed-versus-headless gap, AICA, and
  the poll path.

Their findings and estimates land here when they are done. The candidates going in, in the order the
profile ranks them:

1. **Inline the memory fast path into emitted code.** The largest single item, ADR-accepted in
   principle (mask-and-index, no mmap mirroring in v1), deferred since Phase 2 pending exactly the
   measurement that has now been taken.
2. **Find out what the window costs.** Halving throughput by opening a window is not a rendering
   cost -- `--scale` barely moves the figure, so it is not fill rate -- and something in the present
   or swapchain path is paying for it.
3. **Make AICA cheaper, or conditional.** A quarter of the profile, and it runs when nothing is
   listening.
4. **The poll and scheduler path**, comparable in cost to AICA, and governed by how often emitted
   code checks for interrupts.

### What is off limits

ADR 16 requires `-ffp-contract=off` and golden traces that match bit-for-bit across x86-64 and
ARM64. Any optimisation that changes floating-point results is not a trade to weigh, it is out of
scope, and the golden replay is what proves it.

## Not in this plan

Enhancements -- widescreen, frame generation, anti-aliasing, the VMU screen -- live in
`future-enhancements.md`. They are worth less on a build that stops partway into a race, which is
the compatibility half of this document, and they are worth less again on a build that costs more
than Flycast to run, which is the performance half.
