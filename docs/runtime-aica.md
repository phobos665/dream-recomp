# AICA: the sound processor (WP2.5)

The Dreamcast's sound hardware is a separate computer: an ARM7DI at 22.58 MHz with 2 MB of its own
RAM, the AICA mixer (64 channels, a DSP, three timers) and a mailbox to the SH-4. Titles upload a
sound driver (Sega's Manatee in Crazy Taxi) into sound RAM, release the ARM from reset and talk to
the driver through shared memory; the driver programs the mixer. ADR 10 chose to run this as it is:
an ARM7 interpreter plus AICA emulation, both derived from Flycast (GPL-2.0, ADR 1).

**Status 2026-09-12: Crazy Taxi plays music.** The ARM7 runs Manatee, the title uploads and keys
channels, and the mixer produces continuous stereo audio (recorded with the launcher's `--wav`;
about 60 dB below full scale, peaks near full scale, stereo, low-frequency-dominant, which is what
its intro music should look like). SDL3 output is not wired yet: the mixer hands samples to
`Aica::on_sample` and the launcher writes a WAV.

The silence before this was not an AICA or BIOS bug at all: the title was sitting on a screen
waiting for the Start button, which a headless run never pressed. `crazytaxi_boot --press
start@300,start@600,...` holds a button for eight frames from the given frame; with that the boot
loads three times as much from the disc, keys three channels and plays. Diagnosing this cost most
of a day of BIOS reverse-engineering, so: **when a headless title looks stalled but still renders
every frame and reads the disc, suspect input before emulation.**

## Files

- `runtime/aica/arm7.{h,cpp}`: the ARM7 core. The decode/execute table `arm7_ops.inc` is Flycast's
  copy of the VisualBoyAdvance interpreter, included verbatim; the class around it supplies the
  register file (Flycast's layout, banked copies included), mode switches, SWI/undefined/FIQ entry,
  the memory bus and the e68k interrupt latch. No Thumb (the ARM7DI has none).
- `runtime/aica/aica.{h,cpp}`: the 32 KB register block seen by the SH-4 at 0x00700000 and by the
  ARM at 0x00800000, the three timers, both interrupt controllers, the ARM reset register and the
  44.1 kHz sample tick.
- `runtime/aica/rtc.h`: the real-time clock at 0x00710000 (WP2.2).

## Model

- **Clocking.** One scheduler event every 4535 SH-4 cycles (44.1 kHz). Each tick runs the ARM7 for
  512 of its cycles (the table's own per-instruction estimates), steps the timers, raises
  SAMPLE_DONE on both controllers and re-evaluates them. Deterministic on the virtual clock.
- **Interrupts to the ARM.** `SCIEB & SCIPD`; the lowest pending bit picks the level from
  SCILV0-2 (bits above 7 share 7). The result feeds the e68k latch: FIQ is asserted until the
  driver writes INTREQ M bit 0 (0x2D04), which drops the latch and immediately re-latches a still
  pending source. The FIQ vector is 0x1C; LR is the interrupted instruction + 4.
- **Interrupts to the SH-4.** `MCIEB & MCIPD` drives Holly's external AICA line (level: raised
  while pending, cleared when the driver or the SH-4 clears the last bit through MCIRE).
- **Timers.** Bits 0-7 count once every 2^md samples (bits 8-10) and interrupt on wrapping to 0;
  the count is readable. A write that changes md restarts the prescaler.
- **ARMRST (0x2C00).** Bit 0 holds the core in reset (the state after power-on). Releasing it
  resets the core (SVC mode, FIQ masked, pc 0) and starts it; VREG lives in the high byte.
- **Registers** are 16 bits on 32-bit slots; a 32-bit read returns the low half. Channel (0x0000
  to 0x1FFF) and DSP (0x3000+) registers are stored and counted for the second half. The
  register-to-wave-memory DMA at 0x288C is counted, not performed.
- **SWI/undefined.** LR is the next instruction. Flycast's interpreter glue adds 4 more; its
  recompiler path (the one normally used) does not, and the table's convention is the
  recompiler's, so that is what this port follows.

## Verification

`runtime/tests/test_arm7.cpp`: hand-assembled ARM programs in sound RAM (data processing with
flags, loads/stores including bytes, a counted loop, STM/LDM through r13, SWI through the vector
with `movs pc, lr`, FIQ from the SH-4-side SCPU source at a SCILV level with INTREQ acknowledge),
timer wrap and prescale, MCIEB gating of the SH-4 line, ARMRST semantics and the scheduled tick.
The plan's cross-check of the ARM7 against Flycast's needs an oracle entry point for the ARM core;
not done yet. On Crazy Taxi the driver runs 29 million instructions in the first 600 frames,
services 19,000 FIQs from 13,000 timer interrupts, and the title proceeds past its handshake into
rendering (347 renders in 600 frames).

## Sound generation (`runtime/aica/mixer.{h,cpp}`)

A close port of Flycast's `sgc_if.cpp` and `dsp_interp.cpp`: 64 channels with PCM16/PCM8/ADPCM
(both the looping and the 4-sample-aligned stream variants) and noise sources, the loop rules
(including LSA > LEA), amplitude and filter envelopes with the key-rate scaling, the amplitude and
pitch LFOs, the resonant low-pass filter, TL/DISDL/DIPAN/IMXL attenuation through the log table,
the 128-step effects DSP with its ring buffer in sound RAM and the EFSDL/EFPAN effect returns, and
the master volume and 18-bit DAC options. The monitor registers (MSLC-selected EG/SGC/LP and CA,
with Flycast's LEA-3 clamp) are refreshed on read. Differences from Flycast: the lookup tables are
generated once by `tools/aica/gen_tables.py` into `mixer_tables.inc` so every host mixes
bit-identically (ADR 16); no CDDA, MIDI or VMU-beep inputs; the register images are bit-field
structs on the AICA register bytes.

Tests (`runtime/tests/test_mixer.cpp`): a keyed-on looping PCM channel plays and releases to
silence, panning, the noise source, DSP start on a non-empty programme, and determinism.

## G2 bus DMA (`runtime/holly/g2dma.{h,cpp}`)

The four channels at 0x005F7800 (AICA, EXT1, EXT2, DEV) with STAG/STAR/LEN/DIR/TSEL/EN/ST/SUSP,
immediate copy and a timed end-of-DMA interrupt on the 25 MHz 16-bit bus, as in Flycast's
`Write_SB_ADST`. Crazy Taxi does not use it in the first 30 seconds (it uploads with CPU stores).

## Playback (`audio/`, WP2.5)

`dream::audio::Sink` opens the default device as 16-bit stereo at 44.1 kHz and takes the samples the
mixer hands to `Aica::on_sample`. `crazytaxi_boot --audio` turns it on, `--window` implies it, and
`--no-audio` turns it off for a run that is only being measured.

The whole difficulty is that the AICA produces a sample per tick of the *guest's* virtual clock
while the device consumes on the *host's* real clock, and those are not the same clock. The sink is
deliberately a queue and not a resampler: the launcher already paces the guest against the wall
clock when there is a window, so the rates agree to within a frame's jitter and the queue only has
to absorb that. Samples are sent in blocks of 512 frames, and a block is dropped rather than queued
when more than `high_water` samples are already waiting, which is about a fifth of a second. A queue
that only grows is audible for the rest of the run; one dropped block is audible once.

The counters say which situation a run was in. Windowed and paced, Crazy Taxi plays 286,584 samples
with **none dropped** and about 5,000 queued, at 1.0x real time. The same run headless and
unthrottled runs at 5x real time and drops four samples in five, which is the sink reporting
honestly that the guest outran the device rather than letting the delay grow.

Playback is optional in exactly the way the renderer is: without SDL3 the library is not built,
`--audio` says so, and `--wav FILE` still records what the mixer produced.

**A library must only quit the subsystems it started.** `Window::destroy` called `SDL_Quit()`, which
tears down every subsystem in the process. With sound playing, the sink's stream then belonged to a
dead subsystem and the run segfaulted on exit, after a clean report. It now quits only video.

## Not done

The register-to-wave-memory internal DMA, MIDI, hardware-triggered G2 DMA (TSEL), and the ARM7
oracle cross-check. Resampling: if a title needs the guest to run at a rate the device cannot
follow, the dropped count will say so and that is the point to write one.
