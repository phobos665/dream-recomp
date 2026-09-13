---
name: dream-recomp
description: Project knowledge for the dream-recomp repository, a static recompiler that lifts Sega Dreamcast (SH-4, Katana SDK) games to native C++ with Crazy Taxi as the baseline title. Use this skill for ANY work in this repository or any discussion of the project, including SH-4 decoding or translation, the runtime (PVR2, AICA, Maple, GD-ROM, BIOS syscall HLE), disc tooling and GDI dumps, the phased implementation plan and its work packages (WP0.1 through WP3.6), the architecture decisions (licence, interpreter fallback, FPSCR, interrupts), progress tracking, or choosing what to do next. Load it even when the user only says "let's continue", "what's next", "start on phase 0", or mentions Crazy Taxi, ChuChu, Dreamcast, Katana, KallistiOS, Flycast, or recompilation, so the work stays consistent with the accepted decisions and plan.
---

# dream-recomp

Static recompilation of Sega Dreamcast titles: SH-4 machine code is lifted ahead of time to C++, compiled
natively, and linked against a runtime that emulates the hardware the game talks to (PowerVR2, AICA,
Maple, GD-ROM) and HLEs the BIOS. The model is N64Recomp / XenonRecomp; the closest prior art is the
Initial D Arcade Stage 3 NAOMI 2 recompilation, which uses the same SH-4 CPU.

This skill exists so every session starts from the same accepted decisions and the same plan instead
of re-deriving them. Read it, then read `docs/progress.md` to see where the work actually is.

## Orientation: read these first, in this order

1. `docs/progress.md`: the ledger of which work packages are done, in progress, or blocked. This is
   the single source of truth for "where are we". Always check it before proposing what to do next.
   `docs/owner-tasks.md` lists what only the owner can do (SDK copies, installs, sign-offs,
   play-testing); when a WP is blocked on one of those, say so and pick the next unblocked WP.
   `docs/autonomous-log.md` records every decision taken while the owner was away: add an entry
   (what, why, how to reverse, and **review** if it needs their yes) whenever you decide something
   they did not ask for, including installing tools or changing plan estimates.
2. `references/implementation-plan.md` (also `docs/implementation-plan.md`): the four phases and 23
   work packages with deliverables, exit criteria and effort estimates. Work is picked from here.
3. `references/decisions.md` (also `docs/decisions/README.md`): sixteen architecture decision records.
   Two are accepted (GPL-2.0; Crazy Taxi baseline), the rest are proposed recommendations that the
   work should follow unless the owner overturns them.
4. `references/baseline-game.md`: the title criteria, the history of the choice, and the nine-step
   disc verification checklist that is WP0.4. `references/title-shortlist-measured.md` is the
   measured table for the owner's eight discs; `games/crazytaxi/checklist-report.md` is the
   Crazy Taxi report (steps 1 to 7 passed).
5. `references/hardware-cheatsheet.md`: memory map, syscall vectors, FPSCR bits, PVR and Maple
   essentials. Consult it when writing runtime or translator code; verify against KallistiOS headers
   before hard-coding a value.
6. `references/feasibility-report.md`: the original analysis. Background only; the plan supersedes
   its phase numbering (the report's Phase 0–3 became the plan's Phase 1–4 once Foundations was
   split out).

The `docs/` copies are canonical for humans browsing the repo; the `references/` copies are what
this skill loads. When either the plan, the decisions, or the baseline document changes, update both
copies in the same commit so they never drift.

## Accepted decisions (do not re-litigate)

- **Licence: GPL-2.0 for the whole repository.** Porting Flycast code is therefore allowed and
  expected. Keep `translator/` free of runtime dependencies so it could be relicensed later.
- **Baseline title: Crazy Taxi.** The owner's decision, confirmed by measurement on 2026-09-11
  (`references/title-shortlist-measured.md`): smallest boot binary (1.4 MB) of any single-code-file
  title among the eight discs the owner holds. Alternates, in order: Charge 'N Blast, Tech Romancer,
  both owned. Do not reopen this unless checklist steps 8 or 9 fail. Consequence already in the plan: ADX streaming from the GD-ROM is
  Phase 2 runtime scope. Sonic Adventure is the intended Phase 4 showpiece. Windows CE titles are out
  of scope entirely.

## Proposed decisions the work follows by default

Dev-only Flycast interpreter fallback behind `DREAM_DEV_INTERPRETER`, compiled out of release builds.
C++20, CMake, clang on Linux and MSVC on Windows, Python 3 for tooling. One C++ function per guest
function with an `sh4_ctx` struct and registers in locals. Mask-and-index memory fast path, no mmap
mirroring in v1. FPSCR: data-flow inference, then mode-specialised clones, then a runtime branch.
Cooperative interrupts checked at function entry and loop back-edges on a virtual clock. Ghidra
Function ID databases generated from Katana SDK R9–R11 as the symbol source (generated locally, never
committed). SDL3 plus a Vulkan per-pixel OIT renderer ported from Flycast. ARM7 interpreter for AICA.
GDI and CHD both first-class inputs (the owner's library is mostly CHD); CHD via libchdr from the
`third_party/libchdr` submodule or a system package, chdman as the tooling fallback. Three host platforms: macOS ARM64 (the owner's development machine), Windows
x64, Linux x64, so the emitted code is ISA-neutral, built with `-ffp-contract=off`, and touches host
FP control only through the `fenv` layer; golden traces must match across both ISAs (ADR 16).
Monorepo with TOML per-game configs. Full rationale for each is in `references/decisions.md`; if a task seems to require deviating, say so
explicitly and record it there as a new or superseded ADR rather than deviating silently.

## Repository layout

```
translator/   SH-4 decoder, analysis, C++ emitter
runtime/      memory, scheduler, HLE, hardware cores (mem/ sched/ pvr/ maple/ aica/ hle/ devinterp/)
tools/        dcdisc (disc extraction and inspection), docker/ toolchain image, FID export, test drivers
games/<id>/   per-title TOML, function list, checklist and acceptance reports; disc data is gitignored (crazytaxi/ exists)
tests/        SH-4 instruction suite, KOS test programs, golden traces
docs/         canonical planning documents and the progress ledger
.claude/skills/dream-recomp/   this skill
```

The build is a CMake superbuild (WP0.1): `cmake -S . -B build && cmake --build build --parallel &&
ctest --test-dir build` builds libchdr (submodule), `dream::runtime` (currently the ADR 16 fenv layer
with doctest tests), the `dream-translate` stub, and runs the dcdisc pytest suite through CTest. All
project targets go through `dream_target_defaults()` in `cmake/DreamTargetDefaults.cmake`, which is
where the ADR 16 flags live; never add a target without it. CI (`.github/workflows/ci.yml`) covers
macOS ARM64, Linux x64 with and without `DREAM_DEV_INTERPRETER`, Windows x64, and clang-format.

The translator so far: `translator/src/sh4/decoder.cpp` (WP1.1, tested against
`tests/sh4/oracle/`) and `translator/src/emit/emit.cpp` (WP1.2), driven by `dream-translate emit
--image BIN --base ADDR --function ENTRY:END[:name] --out X.cpp --header X.h`. Emitted code targets
`runtime/include/dream/runtime/sh4/{ctx,ops,abi}.h` and `memory.h`; `docs/emitter-design.md` is the
contract. `tests/sh4/programs/` holds hand-written SH-4 programs (assembled in the container, see
`tests/sh4/README.md`) that CMake translates at build time and runs in `dream_emit_tests`; add a
program there whenever an instruction form or control-flow shape needs pinning down.

`tools/ghidra/` holds headless Ghidra scripts (`MarkEntry`, `SeedFromPointers`, `ExportFunctions`)
for function discovery experiments on real binaries; run them with `analyzeHeadless` from the
Homebrew Ghidra (`$(brew --prefix ghidra)/libexec/support/analyzeHeadless`). Owner-supplied material
lives in ignored folders: `games/*.chd`, `games/<id>/extracted/`, `sdk/` (unpacked Katana SDKs).

`tools/dcdisc/` exists (WP0.3): a Python package that reads GDI and CHD images (libchdr via ctypes,
chdman fallback), extracts the ISO9660 filesystem and IP.BIN, descrambles 1ST_READ.BIN, scores files
for SH-4 code, and writes the disc checklist report. Run its tests with
`cd tools/dcdisc && python3 -m pytest`; they build a synthetic GD-ROM so no game data is needed, and
`tests/fixtures/synthetic.chd` is a committed chdman-made CHD of it. Read `tools/dcdisc/README.md`
before touching disc code. The other code directories are created by WP0.1.

## Hard rules

These protect the project legally and keep it reproducible, which is why they are not negotiable:

- **Never commit game data, BIOS, flash, VMU images, SDK files or FID databases.** The `.gitignore`
  covers the usual extensions but is not a substitute for looking at `git status` before committing.
  Disc dumps live in `games/<id>/disc/`, extraction output in `games/<id>/extracted/`, both ignored.
  If a test needs real game bytes, it reads them from the ignored directory and skips cleanly when
  they are absent.
- **Never paste Sega SDK source or header text into the repository**, including in comments. Describe
  the interface in your own words or reference the KallistiOS equivalent, which is BSD-licensed.
- **Do not add an SH-4 interpreter to the release configuration.** The dev interpreter is a
  development aid; CI must keep building and testing the configuration without it.
- **Every emitter change gets a differential test.** The translator's correctness argument is
  "bit-exact against the Flycast interpreter on real compiled code"; a change without a test
  weakens that argument for every later title.

## How to work a session

1. Read `docs/progress.md`. Identify the lowest-numbered WP that is `todo` or `in progress` and whose
   dependencies are `done`. Phases 1 and 2 are independent, so if one is blocked, the other is fair
   game.
2. Re-read that WP's row in the plan: deliverable, days, and the phase's exit criterion. Scope the
   session to the deliverable; the estimates assume no gold-plating.
3. Before writing code, look at the reference implementation the plan names for that phase
   (Flycast, N64Recomp, XenonRecomp, KallistiOS, Initial D). Porting is expected; reinventing is
   budget overrun.
4. Build and run the fast checks before every push: the CMake build on the current platform, the
   test target, and clang-format. CI runs all three platforms and both interpreter configurations,
   and compares golden traces across x86-64 and ARM64. A test that passes on the Mac but not in
   Linux CI is usually an FMA-contraction or denormal-handling difference; see ADR 16.
5. When a WP's deliverable exists and its checks pass, update `docs/progress.md` to `done (date)` in
   the same commit, with the commit hash or PR link in the notes column.
6. When something in the plan turns out wrong (an estimate, a dependency, a missing WP), fix the plan
   in both `docs/` and `references/` and say what changed in the commit message. The plan is a
   living document; drift between reality and the plan is the thing to avoid.

## Git conventions

- Develop on the branch the session was given. Never push to `main` directly.
- Commit messages: imperative summary line under 72 characters, a body explaining why, and a
  `WP x.y` reference when the commit advances a work package.
- Do not open a pull request unless asked.

## Immediate next steps as of 2026-09-10

- **WP0.4 is in progress.** The dump is at `games/crazytaxi.chd` (gitignored). Steps 1 to 7 of the
  checklist were produced with `dcdisc inspect` into `games/crazytaxi/checklist-report.md`. GD-ROM dumps store
  1ST_READ.BIN plain (verified on Crazy Taxi); only CDI-style images are scrambled, and `dcdisc`
  detects which via the literal-pool pointer ratio. Pay attention to the streaming-media count (ADX music and speech are expected) and to any second file
  that scores as code. Steps 1 to 7 are done and passed; what remains is the Ghidra FID match rate and the two Flycast
  steps (8, 9).
- **Owner step:** `git submodule add https://github.com/rtissera/libchdr third_party/libchdr` from a
  normal checkout (the remote session could not reach GitHub for third-party repos).
- **WP0.1 is done locally; watch its first CI run.** **WP0.2** (toolchain container) is next and
  needs Docker Desktop on the owner's machine. WP0.4's remaining steps need Flycast (8, 9) and a
  later Katana SDK for a useful FID match rate (4).

## Owner-supplied materials (never committed)

| Material | Location | Used by |
|---|---|---|
| Crazy Taxi CHD (or GDI) dump | `games/crazytaxi/disc/` | WP0.4, Phase 3 |
| Katana SDK R9–R11 | outside the repo, path in a local env var `KATANA_SDK_DIR` | FID database generation |
| VMU save dump from real hardware | `games/crazytaxi/disc/vmu/` | WP3.3 |
| Second retail disc from a non-Sega developer | `games/<id>/disc/` | WP1.6 compiler idioms |

## Effort reference

168 focused engineer-days central estimate to a playable Crazy Taxi (range 101–235): Phase 0 12,
Phase 1 45, Phase 2 64, Phase 3 47. Roughly 8 months full time solo or 2 years at evenings and
weekends. Use these when the owner asks how long something will take, and update them in the plan
when reality disagrees.
