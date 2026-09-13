# Where the project is, and what to pick up next

Written 2026-09-13. This is the rolling pick-up point: what the last session did, what is open, and
what the next session should start on. The permanent records are elsewhere and this file never
replaces them — `docs/progress.md` is the ledger of work packages, `docs/autonomous-log.md` records
every decision taken without the owner, and `docs/implementation-plan.md` is the plan this is
measured against.

## Where it stands

Crazy Taxi boots as native code, plays its music, loads the owner's memory-card save, and **plays in
a window**. It reaches its title screen and gets as far as the attract sequence, where it crashes.
That crash is the one open defect and it is an emitter bug, not a graphics one.

```
build/dream-dev/games/crazytaxi/crazytaxi_boot \
  --config games/crazytaxi/crazytaxi.toml --window --scale 2 \
  --vmu ~/Library/Application\ Support/Flycast/data/MK-51035_vmu_save_A1.bin
```

Use the development build. The release build stops at the first untranslated call target by design,
because discovery is not closed. Controls, flags and the rest are in `docs/dev-setup.md`.

## The open defect: a crash once gameplay starts

**What happens.** The owner reaches the title screen and menus, which are correct, and the crash
comes as gameplay begins. A vertex-transform path reads a pointer out of a list of variable-length
records, and gets a floating-point bit pattern instead. The launcher prints the guest call chain at
any bad access:

```
unmapped read32 of 0x4bb9394c at frame 1111
  r4  0c39394c   r14 4bb9394c
  guest call chain (innermost first):
    fn_0c084900  fn_0c07a2d0  fn_0c0434d0  fn_0c043ba2  fn_0c02c810
```

`r14` is `r4` plus `0x3F800000`, which is the bit pattern of the float 1.0. The walker reads a tag
word, tests its bottom bit, and advances either 32 bytes or 8 depending on the answer; one wrong
decision lands it mid-record, where floats read as offsets. At the fault the tag reads `0xffffffff`,
which is not a plausible tag, so the walk had already desynchronised before the instruction that
faulted. A second crash further in, with `DREAM_INTERP_RANGE=0x0c035f40:0x0c084910` as a workaround,
has the same signature one call deeper, so it is the same defect and not a new one.

**What is proven.** It is the emitter. Fully interpreted (`--interpret`) the same run reaches 1300
frames cleanly in 37 seconds; translated it dies at 1111. Both reproduce exactly with `--rtc-seed`.
UndefinedBehaviorSanitizer over the same thousand frames is clean, which rules out a large class of
emitter bug.

**What is NOT proven, and this corrects the previous note.** The bisection by interpreted range that
this file used to record did not isolate anything. `DREAM_INTERP_RANGE` and `DREAM_INTERP_FUNCS`
only redirect calls that go through `find_function`; the emitter writes direct C++ calls for known
targets, so hiding a function whose callers all name it changes nothing. Hiding `fn_0c084900` gives
a byte-identical run. The ranges that came out "clean" were sets containing an ancestor reached
indirectly, whose interpretation cascades. Treat every range in the old table as "a region worth
reading", never as "the bug is here". The reasoning is in `docs/runtime-devinterp.md`.

**Where to start.** Build the tool that is immune to this: record a function's entry state during a
real run, replay that function through the interpreter from the same state, and compare the exit
state and the writes it made. The first function whose two versions disagree is the defective one.
It does not care how the function was called, and unlike whole-run comparison it does not care when
interrupts land. The cheaper alternative is to emit calls through a lookup under a development flag,
which would make the existing bisection work as intended.

## What the last session did

Six commits, on branch `claude/sharp-wozniak-7le0pi`. Graphics step 6 landed, then four real defects
were found and fixed against the owner's own screenshots.

- **The game plays in a window.** Keyboard mapped onto the Maple controller, real-time pacing off
  the guest clock, `F12` screenshots, and `--scale` for internal resolution. About 1.6 to 2 times
  real time at `--scale 1` on an Apple M4, and about real time at `--scale 4`.
- **Writing rendered frames back into video memory was erasing the game's logo.** It lands on
  texture data Crazy Taxi keeps in the same memory. Now behind `--framebuffer-writeback`; the window
  is shown the rendered image for any buffer the renderer drew into, and decodes video memory for
  any other, so a title's own pixel writes still appear.
- **Four TSP instruction-word fields were at the wrong bit positions.** "Ignore the texture's alpha"
  and "use alpha" were each read one bit high, which drew transparent parts of logo tiles opaque.
- **A polygon header size ate a vertex from every intensity-mode-2 strip**, which is what the yellow
  wedges across the SEGA logo were.
- **The background plane was never drawn.** That is why the SEGA screen was black instead of white,
  hiding its black "presented by" text entirely, and why the title screen had no yellow.
- **Diagnostics**: the first-unmapped-access hook above, and capture by guest frame.

Render tests went from 26 to 40. The new ones pin hardware constants against the reference
(Flycast, ADR 1) field by field, so a wrong bit position fails a test instead of drawing something
plausible.

**The lesson worth keeping.** Three of those four defects drew a believable picture rather than
failing. One of them was mis-attributed in a commit that had to be corrected the next commit,
because the control used removed a *consequence* of the suspected cause rather than the cause
itself. A control has to remove the thing under suspicion.

## What is left, against the plan

| Package | State |
| --- | --- |
| Phase 0 (0.1 to 0.4) | done |
| Phase 1 (1.1 to 1.6) | done except 1.4, discovery and CFG closure |
| 2.1 Memory, 2.2 Scheduler and interrupts | done, small items deferred |
| 2.3 Holly and PVR2 | all six renderer steps done |
| 2.4 Maple, 2.5 AICA, 2.7 Dev interpreter | done |
| 2.6 BIOS syscall HLE and GD-ROM | in progress |
| 3.1 Project setup | todo |
| 3.2 Discovery closure to title screen | in progress, blocked by the crash above |
| 3.3 Katana specifics, 3.4 Gameplay closure | todo |
| 3.5 Release hardening, 3.6 Launcher and packaging | todo |

Known gaps outside the crash:

- **Renderer**: modifier volumes (shadows) are decoded but not drawn, transparency sorts per strip
  rather than per pixel, and there is no fog. Details and the reasoning in
  `docs/runtime-render.md`.
- **Audio** reaches a WAV file, not the speakers. SDL3 output is the missing piece, along with an
  ARM7 cross-check against the oracle and AICA internal DMA.
- **Packaging**: there is no build anyone can double-click. That is WP3.6 and it needs discovery
  closed first.
- **Noted, not built**: a hotkey that renders the VMU screen. Titles send it images continuously and
  the traffic is already counted.

## Owner items

`docs/owner-tasks.md` is the live list and nothing on it was closed this session. The ones that
matter soonest: delete the four zero-byte CHDs and the stray `flycast/` folder in the checkout, and
an Apple Developer account for signing a macOS release build, which WP3.6 will need.
