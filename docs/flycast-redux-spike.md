# Spike: can we run flycast_Redux as an oracle on macOS ARM64?

Run 2026-09-21 against `GeneralAtrox/flycast_Redux` at `e4345f32`, on Darwin 27.0.0 / Apple M4.
The question was the one [flycast-redux-review.md](flycast-redux-review.md) left open: that review
read build files and did not build anything.

**Answer: it builds. Whether it runs is unresolved, and an earlier version of this document got
that badly wrong.**

> **Corrected 2026-09-22.** This document previously concluded that no Flycast boots a game on
> this machine, and recommended against adopting the fork on that basis. That conclusion does not
> survive: Flycast runs here. The owner launches it normally and plays titles in it, including the
> one this project is bringing up.
>
> What the measurements below actually support is narrower: **no Flycast boots a game when
> launched from a terminal by this agent** — not the fork, not stock upstream, not the official
> release binary. Running the same official binary the way a person does works. The difference
> has not been identified; a sandboxed launch fails differently from an unsandboxed one, so the
> sandbox is part of it but not all of it.
>
> Read every "does not run" below as "did not run when launched this way". The build result is
> unaffected and stands.

## What was measured

### It builds

`cmake` configured first try with the same three Darwin workarounds the libretro oracle already
carries (`enable_language(OBJC)`, the SDK's `libz.tbd` stub, single-arch). One build failure, in
vendored breakpad: `dump_syms` compiles `NXGetLocalArchInfo` under `-Werror`, and that is
deprecated from macOS 13. `-DUSE_BREAKPAD=OFF` clears it. Second build: exit 0, arm64 binary,
zero errors.

The recipe is [tools/flycast/redux/build_redux.sh](../tools/flycast/redux/build_redux.sh).

This contradicts the review's risk ordering. The build was not the problem; it cost one flag.

Note the configuration is not the one we already use. The research recorder is gated on
`NOT LIBRETRO`, so it cannot ride in the libretro core that
[tools/flycast/oracle](../tools/flycast/oracle) builds. This is the full SDL application.

### It does not boot a game

Every attempt stops during `Sh4Recompiler::Init`, before any research code runs, with one of two
upstream assertions, varying run to run:

```
Verify Failed : &mem_b[0] == ((u8*)getContext()->sq_buffer + sizeof(Sh4Context) + 0x0C000000)
  in Init -> core/hw/sh4/dyna/driver.cpp : 401
Verify Failed : (void*)bm_GetCode(block->addr) == (void*)ngen_FailedToFindBlock
  in bm_AddBlock -> core/hw/sh4/dyna/blockmanager.cpp : 222
```

Both require the guest RAM mapping to sit immediately after the SH-4 context. The log shows it
does not: `BASE 0x136e20000 RAM(16 MB) 0x7d0cc00000` — `ram_base` and `&mem_b[0]` are in different
regions entirely, and both move every run. Which of the two assertions fires depends on whether
`virtmemEnabled()` is true that run, which is why the failure alternates.

Setting `config:Dynarec.Enabled=no` does not help: `Sh4Recompiler::Init` runs either way.

### It is not the fork, and not our build recipe

Three independent confirmations, in increasing strength:

1. **Static.** Redux does not touch `core/hw/mem/` or `blockmanager.cpp` at all, and does not touch
   `Sh4Context`. Its `driver.cpp` additions are profiling hooks that return early when the
   collector is off. Both failing assertions are byte-identical to the merge base
   (`flyinghead/flycast` `4126f146`).
2. **Stock from source.** Upstream at that merge base, built with the same recipe, fails with the
   same two alternating assertions.
3. **The official release.** `/Applications/Flycast.app`, an adhoc-signed universal binary from
   Flycast's own CI (its log path is `/Users/runner/work/flycast/flycast/...`), fails with the
   same assertion at `driver.cpp:349` — the same line at its own version.

So this is Flycast against this OS and this machine, not something a different fork or a better
build recipe would avoid.

## What this changes

**Adopting flycast_Redux is blocked on an upstream problem, and so is every alternative that boots
Flycast.** The fallback of extending our own Flycast patch to log writes would hit the same wall
the moment it had to boot a game rather than run a function slice.

The cost is no longer "build it" (done, cheap). It is "make Flycast start on this machine", which
is unscoped: it needs the virtual-memory reservation root-caused against a current macOS, and it
is not work the research fork's author has any reason to do.

**Our existing oracle is not affected.** Checked, not assumed: the libretro core was rebuilt from
[tools/flycast/oracle/build_oracle.sh](../tools/flycast/oracle/build_oracle.sh) and the
differential harness run against it — 52/52 cases agree. It survives because it never boots a
game: it reserves the address space, loads an image, and steps the interpreter, so
`Sh4Recompiler::Init` is never called. The emitter gate is intact.

**A separate consequence, outside this spike's question:** any measurement in this repository that
compares against Flycast *running a game* on this machine should be re-checked, since no Flycast
boots here today. Whether it ever did on this OS version is not established.

## What the tool would have given us, for when this unblocks

The interface is right, and better than the review credited. Confirmed by reading the source and
[docs/workbench/Workbench.md](https://github.com/GeneralAtrox/flycast_Redux) in the fork:

- `WorkbenchSh4Types=memory-write` with `WorkbenchSh4MemStart/End` — writes only, over a bounded
  address range. That is exactly the Crazy Taxi instrument, as a launch option.
- `sh4_events` rows carry `ordinal`, so a diff can key on write sequence rather than tick, which
  is what comparing against a different emulator requires.
- `Sh4PcCheckpoint` with `Sh4PcCheckpointU32Address/Value` — pause at an instruction, gated on a
  RAM word, then read registers and memory coherently. That is the Rayman 2 instrument.
- `MapleRecord`/`MapleReplay` plus `DreamcastRtcSeed` pin determinism, and replay *verifies*
  itself: the first divergence stops the run and names the field. That answers risk 2 of the
  review (prove determinism before trusting a comparison) without us building anything.
- Unbudgeted bonus: `indirect_call_targets` and a Ghidra facts export, both of which bear on our
  7,464 unresolved indirect sites.

None of this was exercised. The emulator never reached a game.

## Reproducing

```
tools/flycast/redux/build_redux.sh
build/flycast-redux/Flycast.app/Contents/MacOS/Flycast \
    -config config:rend.ThreadedRendering=no games/<title>.chd
```
