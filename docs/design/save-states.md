# Save states

The reference for the state format and for what a resumable state has to contain. Code comments in
`runtime/include/dream/runtime/state/state.h`, `runtime/src/state/save_state.cpp` and the launcher
point here.

**Status: stage 1.** `--save-state FILE --save-state-at N` writes a state and the run carries on;
`--load-state FILE` starts from one instead of booting. Measured on the baseline title: at seven
capture points spread across a twenty-second run, every resumed run reached the same frame with the
same final PC, zero unmapped accesses and zero untranslated call targets, and four of the seven
reproduced the uninterrupted run's per-frame write hash exactly for the entire remainder (six do
with `--load-state-no-aica`, but a different six). See the
title repository's `docs/design/save-states.md` for the measurements in full.

## Why this is not an emulator's save state

Translated guest functions call translated guest functions as ordinary C++ calls, so the guest's
call chain *is* the host call chain. At an arbitrary instant a large part of the guest's execution
state is in host stack frames, and nothing can serialise those. Two rules follow.

**Capture only where `Ctx.pc` is exact.** Emitted code sets `c.pc` once per basic block, immediately
before the interrupt poll (`emitter-design.md`). Everywhere else it is the last block start passed.

**Resume through the non-local-return path.** `sh4::resume_at` already re-enters translated code at a
block start inside the function containing a PC, because cooperative task switching needs exactly
that (`runtime-interrupts.md`). `sh4::resume_guest` is `run_guest`'s loop entered through that path
rather than through a call; the guest's own stack and PR then rebuild the host chain one frame at a
time as each resumed function returns. `sh4::resumable(pc, m)` answers whether a PC can be re-entered
without falling back to the development interpreter.

`resume_guest` passes a sentinel stop-PR rather than the captured `c.pr`. A resumed state has no
outermost frame, and arming the loop with a live guest address ends the run the next time the
program calls from that site.

## Where the launcher captures

From the per-scanline watchdog, a scheduler event, which runs from inside `deliver_irq`. Guards:

- **after the event re-arms itself.** `Scheduler::advance_to` disarms an event before calling it. A
  state captured before the re-arm records that event as disarmed, and the resumed run loses it.
- **not while `System::advancing_for_device`** — the clock also advances on the way into an MMIO
  access, where the guest is mid-block.
- **not while `System::nesting > 0`** — the capture would be of an interrupt handler.

## Format

```
header   magic "DRMSTATE", format version, header size, section count, table offset,
         game id, 1ST_READ.BIN SHA-1, entry, function count, translation fingerprint,
         guest cycles, frame, flags               (fixed 156 bytes)
table    per section: name[16], version, flags, offset, size   (40 bytes each)
payload  sections, in table order
```

The container version (`state::kFormatVersion`) changes only for the header or table layout. Each
section carries its own version, so adding a section does not invalidate states without it; a reader
skips a section flagged `kOptional` and must refuse one whose version it does not know.

**Provenance.** Guest addresses in `Ctx` and in guest RAM mean nothing except against the function
table they were captured with, so a state records the game id, the disc binary's SHA-1, and
`sh4::function_table_fingerprint()` — a hash over every registered translation's address range. Any
change to discovery, relocations or the exclude list changes it, and the state is refused with a
sentence rather than executing garbage. Header flags also record whether the writing build had the
development interpreter and whether the capture PC was verified resumable; a release build refuses a
state it cannot re-enter, having no interpreter to fall back to.

## What a state contains

Bulk memory: `ram` 16 MB, `vram` 8 MB, `aram` 2 MB, `flash` 128 KB. `sh4.ctx`, written field by
field — never blit `Ctx`, it is the one struct emitted code indexes by name. `sh4.mem` for the CCN
block, QACR and the store-queue buffers.

Devices, each through `MmioHandler::save_state` / `load_state`: `sched` (the clock and every event's
deadline, keyed by name rather than id), `sh4.intc`, `sh4.tmu`, `sh4.dmac`, `holly.intc`, `holly.sb`,
`holly.g2`, `pvr.spg`, `pvr.core` (register block, TA parser and its parameter stream, FIFO, YUV),
`maple`, `aica`, `rtc`, `bios.gd` (the GD-ROM HLE including a read in flight).

Not carried, deliberately: the AICA **mixer's** per-channel and DSP state; the TA parser's position
within one 32-byte parameter; the renderer's host-side texture cache.

## Adding a device to the state

1. Declare `void save_state(state::Writer&) override;` and `load_state(state::Reader&) override;` in
   its header and implement both in `runtime/src/state/save_state.cpp`, next to the others — what a
   state contains should be readable top to bottom in one file.
2. Write fields explicitly. Never blit a struct: a state outlives the build that wrote it.
3. Bump that section's version where the launcher calls `Writer::begin`.
4. **Do not restore a scheduler event id, and do not re-arm an event the scheduler section already
   restored.** Re-arming replaces the capture's remaining delay with a fresh full one, which lands
   the event later than it did. Re-arm only when `Scheduler::armed()` says nothing was restored.
5. Measure. Save at several points, reload, and compare the per-frame write hash against an
   uninterrupted run (`differential-harness.md`). A device left out shows as a divergence some
   frames later, not as a fault.

**A partial restore of a subsystem can be worse than none.** Restoring the AICA register block and
ARM7 without the mixer makes the mixer play channels from default internal state and the DSP write
into sound RAM at the wrong ring-buffer offset, on top of the ARM7's own code — 1,994 undefined ARM
instructions and spurious interrupts to the SH-4. Restoring none of the AICA is merely silent.
`--load-state-no-aica` exists to bisect exactly this, and is a diagnostic, not a mode to run in.

## Flags

```
--save-state FILE        write a resumable state and carry on running
--save-state-at N        take it at the first safe point at or after guest frame N
--save-state-stop        end the run once the state is written
--load-state FILE        start from a state instead of booting the title
--load-state-no-aica     restore everything but the sound hardware (divergence bisection)
```

States are gitignored (`*.state`, `*.dcstate`): a state is an image of guest RAM, video memory and
sound RAM, so it is disc-derived data by definition.
