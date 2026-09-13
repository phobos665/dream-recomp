# Implementation plan

**Status:** Proposed (phases and scope for sign-off)
**Baseline title:** Crazy Taxi (ADR 15, accepted; owner's choice)
**Licence:** GPL-2.0 (ADR 1, accepted)

This plan turns the feasibility report's four phases into work packages with deliverables, exit
criteria, effort estimates, dependencies and the tools each needs. Estimates are in **focused
engineer-days** (a full day of uninterrupted work by someone comfortable in C++ and with the SH-4
manual open). Calendar conversions are at the end.

## 1. Phase map

```
Phase 0  Foundations            ─┐
                                 ├─► Phase 1  Translator core ──┐
                                 │                              ├─► Phase 3  Crazy Taxi      ──► Phase 4  Generalise
                                 └─► Phase 2  Runtime ──────────┘
```

Phases 1 and 2 are independent and can run in parallel or interleaved. This is the main consequence
of the dev-time interpreter fallback (ADR 2): the runtime can be brought up as a Flycast-core-based
emulator front end and tested against real discs before the translator emits a single function, and
the translator can be validated against a bare RAM-only harness before the runtime exists.

| Phase | Goal | Exit criterion | Effort (days) |
|---|---|---|---|
| 0 | Foundations | CI green on all three platforms; Crazy Taxi dump passes the disc checklist | 12 |
| 1 | Translator core | Bit-exact on the SH-4 instruction suite and 20+ KOS programs, incl. two retail binaries, identical on both host ISAs | 45 |
| 2 | Runtime | Three KOS homebrew games playable start to finish with audio, under the dev interpreter | 64 |
| 3 | Crazy Taxi | Playable start to finish, VMU saves, zero untranslated calls in the release build | 47 |
| 4 | Generalise and enhance | Out of initial scope; outlined for direction only | — |
| | **Total to a playable commercial title** | | **168** |

## 2. Phase 0: Foundations (12 days)

Goal: a repository someone else could clone and build, a toolchain container, disc tooling, and the
baseline disc verified.

| WP | Work | Deliverable | Days |
|---|---|---|---|
| 0.1 | Repository skeleton: CMake superbuild for `translator/`, `runtime/`, `tools/`, `tests/`; clang-format and clang-tidy config; GitHub Actions for macOS ARM64 (Apple clang), Windows x64 (MSVC) and Linux x64 (clang); `-ffp-contract=off` and warning flags set centrally; CONTRIBUTING with licence and no-game-data rules | Empty targets that build and run a smoke test on all three platforms | 3 |
| 0.2 | Toolchain container: multi-arch (linux/arm64 for Apple Silicon Docker Desktop, linux/amd64 for CI) Dockerfile with `sh-elf-gcc` and KallistiOS, Flycast built from source (for the GDB server and, later, as the interpreter library), Ghidra with SH-4 and the Dreamcast loader, Python 3 with the FID-generation script | `tools/docker/` image built in CI for both architectures, documented in `docs/dev-setup.md` | 3 |
| 0.3 | Disc tooling in Python: GDI reader and writer, CHD reader (libchdr via ctypes, chdman fallback), ISO9660 reader, IP.BIN decoder (peripheral flags, boot filename, WinCE flag), `1ST_READ.BIN` descrambler with the literal-pool verification heuristic, SH-4 code-file scanner, SDK banner-string scanner, manifest with SHA-256 per file, checklist report; synthetic-disc test suite | `tools/dcdisc/` CLI: `info`, `ls`, `extract`, `chd2gdi`, `inspect`, `manifest`, `descramble`, `scan` | 4 |
| 0.4 | Run the disc verification checklist (`baseline-game.md`) against the owner's Crazy Taxi dump with `dcdisc inspect`; build the matching Katana FID database; record function-match rate; write the result into `games/crazytaxi/checklist-report.md` | Checklist report; ADR 15 confirmed or the alternates (ChuChu Rocket!, Ikaruga) re-evaluated | 2 |

**Owner-side prerequisite for WP0.4:** a GDI dump of the disc. See §7 for dumping options.

**Phase 0 exit:** CI green on macOS ARM64, Windows x64 and Linux x64; `dcdisc inspect` on the Crazy Taxi dump reports the code-file count, a Katana SDK
version, and an FID match rate; the report is committed.

## 3. Phase 1: Translator core (45 days)

Goal: an SH-4 to C++ translator whose output is bit-exact against the Flycast interpreter on real
compiled code, with function discovery good enough to seed Phase 3.

| WP | Work | Deliverable | Days |
|---|---|---|---|
| 1.1 | SH-4 decoder: all integer, system, and FPU opcodes (SH7750 set incl. `FIPR`, `FTRV`, `FSRRA`, `FSCA`, `PREF`, `OCBI/OCBP/OCBWB`, `MOVCA.L`, `LDTLB`); operand extraction; disassembler text output. Validated by decoding every 16-bit value and diffing against Flycast's disassembler | `translator/decoder/` with 100% opcode-space coverage test | 4 |
| 1.2 | Integer emitter: `sh4_ctx` struct; per-instruction C++ emission for ALU, shifts, `MAC`, `DIV0/DIV1`, `DMULS/U`, T-bit, GBR-relative, PC-relative constant loads (folded to immediates), branch forms with delay slots, `TRAPA` and `RTE` as runtime calls | `translator/emit/` producing compilable C++ for straight-line and branching code | 8 |
| 1.3 | FPU emitter and FPSCR pass: single/double/pair semantics keyed on PR/SZ/FR; forward data-flow over the CFG seeded from `LDS FPSCR`, `FSCHG`, `FPCHG`, `FRCHG`; mode-specialised clones where inference fails; runtime-branch fallback with warning; rounding-mode propagation to host | FPSCR analysis report per function; clone selection at call sites | 8 |
| 1.4 | Discovery and CFG: recursive descent from seeds (entry, IP.BIN, `LDC VBR` tables, syscall installers, constant-pool words in code range); images may be linked at the 0x0C alias and contain regions executed from a relocated copy, so the image model is (file range, link address, list of relocated copies); basic-block splitting; `BRAF`/`JMP @R0` switch-table recovery; idle-loop detection tagging (consumed by the runtime in Phase 2); function table generation (address to pointer, sorted, with hash) | `translator/analysis/`; `functions.json` output; unreached-address report format shared with the runtime fault log | 10 |
| 1.5 | Differential test harness: ELF loader for KOS-built test programs; bare RAM-only host harness with a hooked output syscall; Flycast interpreter built as a library and driven in lockstep, comparing full `sh4_ctx` at every function return; per-opcode instruction test suite (own, generated from the decoder tables); FPSCR fuzzing; the delay-slot regression suite (Initial D v2461 case first); `fenv` and `simd` abstraction layers (ADR 16) with cross-implementation tests; golden traces committed and compared on both host ISAs | `tests/sh4/`; CI job running the suite on macOS ARM64 and Linux x64 with identical golden traces | 11 |
| 1.6 | Symbol and config input: TOML per-game config (disc hash, load addresses, entry, extra function addresses, HLE overrides); importer for Ghidra FID export (address, name, SDK version); SHC and CodeWarrior idiom pass using two retail binaries (Crazy Taxi and one non-Sega title the owner holds) as inputs with no execution, decode and CFG only | `translator/config/`; documented TOML schema; idiom notes in `docs/compiler-idioms.md` | 4 |

**Phase 1 exit:** the instruction suite and at least 20 KOS programs produce identical `sh4_ctx`
traces to the Flycast interpreter; the two retail binaries decode with no unknown opcodes and
discovery covers at least 85% of their code bytes (the rest is expected to be data or reached only
at runtime).

**Key risk in this phase:** FPSCR inference on real compiler output (WP1.3). Budget assumes GCC and
SHC keep PR/SZ stable per function, which the report asserts; if CodeWarrior output toggles modes
mid-function, WP1.3 grows by roughly 5 days.

## 4. Phase 2: Runtime (64 days)

Goal: a Dreamcast hardware runtime that can run KOS homebrew via the dev interpreter, ready to have
translated code dropped in. Every WP here is a port and adaptation of Flycast code under GPL-2.0
unless noted.

| WP | Work | Deliverable | Days |
|---|---|---|---|
| 2.1 | Memory: 16 MB RAM, 8 MB VRAM with 32-bit and 64-bit views, 2 MB sound RAM, OC-RAM at 0x7C000000, store queues with `PREF` flush via QACR, MMIO dispatch table; mask-and-index fast path; the emitted-code access helpers | `runtime/mem/` with unit tests for mirrors, SQ flush, and view aliasing | 4 |
| 2.2 | Scheduler and interrupts: virtual clock; event queue for TMU (3 channels), SPG/VBlank, TA list-complete, render-done, DMA-done, Maple, GD-ROM; `deliver_irqs` implementing the bank swap, SR.BL/RB/MD, `VBR + 0x600` dispatch, `RTE` unwind; idle-loop fast-forward using Phase 1 tags | `runtime/sched/`; interrupt latency test with a KOS TMU program | 8 |
| 2.3 | Holly and PVR2: two-day MoltenVK spike first (confirm the OIT technique runs on macOS before porting); SPG registers and timing; TA parameter parser (all list types, sprites, modifier volumes); Vulkan per-pixel OIT renderer ported from Flycast; texture decode (twiddled, VQ, palette, YUV) with invalidation on CPU write; render-to-texture; framebuffer direct write and presentation; internal resolution scaling; SDL3 window | `runtime/pvr/`; renders Flycast's PVR test scenes and KOS `pvr` examples identically on all three platforms | 22 |
| 2.4 | Maple: bus frame protocol and DMA; controller (SDL3 gamepad), VMU as a file-backed 128 KB block device plus LCD rendering to an overlay; Puru Puru rumble; four ports | `runtime/maple/`; KOS `vmu` and `controller` examples pass | 6 |
| 2.5 | AICA: ARM7DI interpreter, 64 channels, DSP, timers, RTC; mix to SDL3 audio at 44.1 kHz; ARM7 interpreter cross-checked against Flycast's | `runtime/aica/`; KOS sound examples audible and in tune | 10 |
| 2.6 | BIOS syscall HLE and GD-ROM: vectors at 0x8C0000B0–BC (GD-ROM command queue, sysinfo, flashrom, font, misc); GDI and CHD (libchdr) readers with sector-level access; **streaming reads with a read-completion timing model** (Crazy Taxi streams ADX music and speech during play, so the command queue must report progress plausibly, not instantly); flashrom as a file; region and language settings | `runtime/hle/`; boots a Katana disc to its first TA submission; a KOS ADX/streaming example plays without underruns | 10 |
| 2.7 | Dev interpreter integration: Flycast SH-4 interpreter behind `DREAM_DEV_INTERPRETER`; function-table miss handler that logs and falls back; release configuration that aborts with a fault log instead; CI builds both | `runtime/devinterp/`; both configurations in CI | 4 |

**Phase 2 exit:** three KOS homebrew games with source (proposed: a Doom port, the Wipeout Rewrite
Dreamcast port, and one 2D title using framebuffer writes) run start to finish with audio and
controller input under the dev interpreter, on both platforms, at full speed.

**Key risk in this phase:** WP2.3 is a third of the phase and depends on how cleanly Flycast's Vulkan
OIT renderer separates from its emulator plumbing. Budget assumes a port, not a rewrite; if the
coupling is worse than expected, the fallback is Flycast's per-triangle-sorted renderer first (simpler,
visually inferior) and OIT later.

## 5. Phase 3: Crazy Taxi (47 days)

Goal: the first commercial title, end to end, translated with zero fallback.

| WP | Work | Deliverable | Days |
|---|---|---|---|
| 3.1 | Project setup: `games/crazytaxi/` TOML with disc hash, load address 0x8C010000, link address 0x0C010000 and the startup stub relocated to 0x8C004000 (measured 2026-09-11); FID symbol import; first translate and build; boot under dev interpreter with fault logging | Game builds; boot reaches the Sega logo | 3 |
| 3.2 | Discovery closure to title screen: iterate on unreached-address logs; add constant-pool and switch-table cases the analysis missed; fix emitter bugs surfaced by the game's compiler; budget assumes a larger binary than a puzzle title | Title screen and menus with zero fallback hits | 10 |
| 3.3 | Katana specifics: Manatee driver upload and mailbox traffic through AICA; ADX streaming path (GD-ROM reads feeding the sound driver) without dropouts; VMU save format and the Katana buffer library's Maple traffic; syscall usage audit against WP2.6 | Music, speech and SFX correct; save and load round-trip verified against a real VMU dump | 6 |
| 3.4 | Gameplay closure: Arcade and Original maps at every time setting and under Arcade rules, all Crazy Box stages, rankings and settings persisting across restart; performance profile of the open city; fix timing races in streaming and in fare pickup/drop-off | Acceptance matrix (mode × map × time setting × result) with every row green | 15 |
| 3.5 | Release hardening: zero untranslated calls in the no-interpreter build over the full acceptance matrix; regression ledger with hash-locked builds; crash-free soak (attract mode for an hour) | Release configuration passes the matrix | 6 |
| 3.6 | Launcher and packaging: CLI that takes a GDI path, verifies the disc hash, extracts and unscrambles into a cache, and runs; internal resolution option; controller mapping; macOS app bundle with signing and notarization; README for end users | Tagged v0.1.0 build for macOS ARM64, Windows and Linux, no game data included | 7 |

**Phase 3 exit:** the report's Phase 2 criterion, verbatim: playable start to finish, VMU saves
working, zero untranslated calls.

## 6. Phase 4: Generalise and enhance (outline only)

Not estimated. Ordered by expected value:

1. Sonic Adventure as the showpiece, then ChuChu Rocket! as a small second title to measure how much
   of the Crazy Taxi work transfers across Sega studios.
2. Widescreen via projection-matrix hooks, uncapped frame rate where game logic permits, texture
   replacement. These are the features that make a recomp preferable to an emulator.
3. Selective HLE of hot Ninja and Kamui routines identified by FID, once profiling justifies it.
4. Register-level GD-ROM, if a later title bypasses the BIOS syscalls.
5. Linux and Windows on ARM64.
6. Mod API and per-game project template so third parties can add titles without touching the core.

## 7. Tools and materials

### Hardware

| Item | Needed for | Notes |
|---|---|---|
| Sega Dreamcast console | Dumping the disc; optional ground-truth oracle | Any region; a GD-ROM drive in working order |
| Crazy Taxi retail disc | Baseline | Owned, dumped as an 80 MB CHD |
| SD card adapter plus DreamShell, **or** Broadband Adapter with httpd-ack, **or** serial coder's cable with dcload-serial | Dumping the GD-ROM to GDI | DreamShell's GD Ripper to SD is the cheapest route; BBA is fastest but expensive; serial works but takes hours per disc |
| VMU | Save format verification in WP3.3 | Dump a real save to compare against the runtime's file |
| Second title the owner holds (non-Sega developer) | Compiler-idiom coverage in WP1.6 | Used for decode and CFG only |
| BBA or serial cable plus dcload | Optional: real-hardware oracle in WP1.5 | Skip if unavailable; Flycast's interpreter is the CI oracle regardless |

### Software

| Tool | Phase | Purpose |
|---|---|---|
| CMake 3.28+, Ninja, Apple clang (macOS), clang 18+ (Linux), MSVC 2022 or clang-cl (Windows) | all | Build |
| Docker Desktop (Apple Silicon) or Docker Engine | 0–2 | Runs the linux/arm64 or linux/amd64 toolchain container |
| SDL3 | 2 | Window, input, audio |
| Vulkan SDK 1.3 (LunarG; the macOS SDK bundles MoltenVK) | 2 | Renderer |
| Catch2 or doctest, toml++, nlohmann/json | 1, 2 | Tests, config, discovery reports |
| Python 3.11+ | 0, 1 | Disc tools, FID generation, test orchestration |
| KallistiOS with `sh-elf-gcc` toolchain (Docker) | 0, 1, 2 | Building test programs and homebrew corpus |
| Flycast source (GPL-2.0) | 0–3 | GDB server for RE; interpreter library as oracle and dev fallback; PVR, AICA, GD-ROM source to port |
| Ghidra 11.x with SH-4 processor module, `sega-dreamcast/ghidra-loader`, `iamsh4/dc-re-ghidra`, AltoRetrato's `build_dc_fidb.py` | 0, 3 | Disassembly, SDK header import, Function ID databases |
| Katana SDK releases R9–R11 (owner-sourced, never committed) | 0, 3 | FID database generation, header reference |
| Disc tooling: own `dcdisc` (WP0.3) reading GDI and CHD; libchdr (submodule or `libchdr-dev`) and/or MAME `chdman` for CHD; GD-ROM Explorer as a cross-check on Windows | 0 | Extraction and inspection |
| ImHex | 0, 3 | Binary inspection |
| `mkdcdisc`, `makeip` | 1, 2 | Building test discs from KOS programs |
| Renesas SH7750 Hardware Manual and SH-4 Software Manual; Sega Dreamcast hardware specification; KallistiOS source | all | Reference |
| libchdr (BSD-3, `third_party/libchdr` submodule) | 0, 2 | CHD decoding in the tooling and the runtime |

### Reference implementations to read before starting each phase

- Phase 1: N64Recomp (function-per-function emission, delay slots), XenonRecomp (context struct,
  memory helpers), Initial D `translator/` (SH-4 specifics), Flycast `sh4/` interpreter and
  disassembler, Tokyo Bus Guide decomp test framework.
- Phase 2: Flycast `hw/pvr/`, `hw/aica/`, `hw/arm7/`, `hw/maple/`, `hw/gdrom/`, `reios/` (its HLE
  BIOS); Deecy's equivalents as a second opinion; KallistiOS drivers for the register-level truth.

## 8. Effort summary and calendar

| Phase | Days | Range (±40%) |
|---|---|---|
| 0 | 12 | 7–17 |
| 1 | 45 | 27–63 |
| 2 | 64 | 38–90 |
| 3 | 47 | 28–66 |
| **Total** | **168** | **101–235** |

Calendar at different cadences, using the central estimate:

| Cadence | Days per week | Calendar to Phase 3 exit |
|---|---|---|
| One engineer full time | 5 | ~34 weeks (8 months) |
| Two engineers full time, Phases 1 and 2 in parallel | 10 | ~20 weeks (Phase 2 is the critical path) |
| One person, evenings and weekends | ~1.5 | ~112 weeks (2 years) |
| One person, one focused day per week | 1 | ~3 years |

Assumptions behind the numbers:

- Porting Flycast code counts as porting, not rewriting. Phase 2 roughly doubles if a clean-room
  rewrite is attempted.
- Coding-assistant tooling compresses table-driven and boilerplate work (decoder, emitter cases,
  MMIO register maps) but not debugging time, which dominates Phases 1.5, 2.3 and 3.4. The
  estimates already assume that compression on the boilerplate.
- The owner supplies the disc dump, Katana SDK copy, and VMU dump; none of that is on the critical
  path beyond WP0.4.
- Two host ISAs from day one (ADR 13, ADR 16) add about six days in total, spread over WP0.1, WP0.2,
  WP1.5, WP2.3 and WP3.6. Retrofitting cross-ISA determinism later would cost far more.
- Crazy Taxi instead of ChuChu Rocket! adds four days: ADX streaming in WP2.6 and a larger discovery
  loop in WP3.2.

## 9. Milestones worth announcing

1. **M1, end of Phase 0:** the disc checklist result. This is the first evidence about Crazy Taxi
   specifically.
2. **M2, Phase 1 exit:** bit-exact translator on KOS programs. Interesting to the wider recomp
   community; a good moment to open the repository.
3. **M3, Phase 2 exit:** KOS homebrew running under the runtime. First video-worthy result.
4. **M4, first Crazy Taxi title screen with zero fallback (WP3.2).**
5. **M5, v0.1.0:** the report's Phase 2 criterion met.

## 10. What is deliberately not in this plan

Windows CE titles, modem and BBA networking, light gun, fishing rod, microphone, bit-exact `FTRV`
precision, Linux and Windows on ARM64, any per-game enhancement beyond internal resolution. Each is
listed in Phase 4 or the report's deferred list and none is needed for the baseline.
