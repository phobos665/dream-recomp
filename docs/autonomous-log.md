# Autonomous decision log

Decisions taken without the owner in the loop, newest first within each day. Each entry says what
was decided, why, and how to reverse it. Anything marked **review** wants a yes/no from the owner.

## 2026-09-11

- **PVR started with the headless half.** WP2.3's renderer waits on the owner's Vulkan SDK (open
  owner task) and the MoltenVK spike; the register block, the TA parser and the render/list-complete
  interrupts are what the game loop needs to advance and are testable now with synthetic parameter
  streams. The parser mirrors Flycast's state machine (header and vertex sizes from the control
  word) rather than the vertex-type table in the PowerVR documentation, because Flycast's rules
  are what thousands of titles have been run through. The captured stream is kept per render so the
  renderer can be developed against real Crazy Taxi frames once the boot gets that far.
- **Render completion is a fixed cost plus per-word cost (200k cycles + 4/word).** A guess until
  Crazy Taxi renders; exposed as two fields on `pvr::Core` so the launcher can sweep them.

- **Dev interpreter built from the project's own decoder and `ops.h`, not a Flycast port (ADR 2
  revised, references synced).** Both pieces are already bit-exact against Flycast (decoder oracle,
  differential harness), each opcode is the run-time form of the emitter's lowering so the two
  cannot drift, and the correctness test is direct: the golden replay runs all 78 cases translated,
  interpreted and hybrid and demands identity (it passed first time, 170,011 assertions). A port
  would have meant an adapter between two context layouts and two memory models with no
  comparable test. Reversal: swap `runtime/src/devinterp/interpreter.cpp` for a ported core behind
  the same `run()` entry point.
- **Decoder split into `dream::sh4dec`** so the runtime links only that under the dev flag, never the
  emitter, discovery or TOML; the translator links it publicly. Two CMake lines.
- **Release builds now throw `UntranslatedCall`; `crazytaxi.boot` registered only in the dev
  configuration.** The WP2.2 log said "logged and skipped" was acceptable only until WP2.7; ADR 2
  requires the release build to abort with the fault log. The other stream's boot test therefore
  cannot pass in the release build until discovery is closed (WP3.2), and its registration in
  `games/crazytaxi/CMakeLists.txt` says so; that is the one edit to a file outside my lane, and it
  is a gate, not a behaviour change. **Review:** that gate comes off when the boot no longer needs
  the interpreter.
- **RTE outside a runtime-delivered exception is a jump to SPC.** Found by the first boot: Katana's
  startup restores a full context and enters the program through a RAM stub that ends in `rte`,
  reached by `jmp`, not by an exception. Treating every RTE as "unwind to the delivering frame"
  sent control back into the stub, which idled for 900 frames with interrupts masked. `System` now
  answers `Hooks::in_exception_frame()` from its nesting depth and the interpreter jumps to SPC
  otherwise, never taking an RTE as a return through PR (with SPC and PR both zero that would have
  hidden the real fault). The emitted `rte(c, m); return;` has the same gap; recorded as a WP1.4
  follow-up rather than edited now, because `emit.cpp` is being changed by the other stream.
- **First boot finding handed on, not chased.** With the interpreter the boot gets past the stub
  and faults executing address 0, because the saved context the stub restores is all zeros
  (`DREAM_DEVINTERP_TRACE` showed it in six lines). That is bring-up work on the boot path the
  launcher commits are already investigating; chasing it here would duplicate them. Noted in the
  WP2.7 ledger row for WP3.2.
- **Shared working tree, two streams.** My edits to `translator/CMakeLists.txt` and
  `games/crazytaxi/CMakeLists.txt` were absorbed into the other stream's commits b188d35 and
  e4c4d8d (they were on disk when it staged those files); the end state is right, the attribution
  is not. For this commit I staged only my own hunks (two shared CMake files were synthesised from
  HEAD plus my block, leaving the Maple lines uncommitted for their author). Also learned the hard
  way: hand-assembled test code must save and restore PR around a `jsr` exactly as compilers do,
  or the interpreter faithfully executes an infinite loop.

- **Maple done bus-first, host input later.** The boot needs the pad library's VBlank-triggered
  transfers answered, not a gamepad; a controller with a settable state covers the launcher and the
  tests, and SDL3 (the plan's host layer) can be brought in when a window exists (WP2.3). Frame
  layout and device responses are Flycast's.
- **Maple lives in the launcher, not in `System`, for now.** Same reason as the register stubs:
  `system.h/.cpp` carry the other session's uncommitted interpreter work. Folding the bus into
  `System` is a five-line change once that lands.

- **A second session is working in this checkout (WP2.7, the dev interpreter).** Commits `99e0c52`
  and `3f4babc` and the uncommitted `runtime/devinterp/` work are not from this session. Rule
  adopted here: commit only files this session changed, by explicit path, never `git add -A`;
  do not edit `runtime/src/devinterp/*`; keep additions to shared files (`system.h/.cpp`) out of
  the way. The launcher's on-chip register stubs and serial capture were moved out of `System`
  into the launcher for that reason.
- **Crazy Taxi boots through its loader and startup stub headless, then blocks on generated code.**
  The launcher (`games/crazytaxi/crazytaxi_boot`) found, in order: the loader enters the stub at
  +0 and +0x70; the stub's position-independent calls (`mova`+literal+`or r14`) needed block-local
  constant propagation; the stub patches its jump-to-program slot at run time, which literal
  folding had read as 0 (now a pre-pass keeps stored-to pool words unfolded); and finally the
  stub **writes a fresh interrupt trampoline into 0x8C00FA00 instruction by instruction** (not a
  copy of image bytes) and jumps to it. Static translation cannot reach code that only exists at
  run time; this is precisely the case for WP2.7's interpreter fallback, so the boot work stops
  here until that lands. The launcher's report now lists run-time copies of image code as ready-
  made `[[relocations]]` entries (the stub itself: 0x0C010100 -> 0x8C004000) and dumps memory on
  request, so the next step is a re-run, not a new investigation.
- **Relocation units can opt out of literal folding (`no_fold`)** for templates the program
  patches after copying; added while chasing the trampoline, kept because Katana's other copied
  vector blocks will need it once they are found.

- **Syscalls are native functions in the guest function table, not translated BIOS code.** The
  vectors point at 0x8C001000+ where `Bios` registers C++ handlers, so the game's own wrapper code
  calls them like any function and the runtime needs no BIOS image (ADR 1's HLE route). Flycast
  patches an opcode there for its interpreter; we need only the table entry.
- **GD-ROM timing copies Flycast's rate model** (five sectors per million cycles above 10 KB, two
  cycles per byte below) rather than a seek model. It is what thousands of titles have been run
  with, it gives ADX streaming plausible progress, and `EXEC_SERVER` advances the clock so polling
  games see it. A seek-distance model can be added if Crazy Taxi's streaming stutters.
- **libchdr linked statically into the runtime.** The Python tooling uses the shared build; the
  runtime's executables would otherwise need the DLL next to them on Windows CI.
- **Flash formatted in memory with Flycast's default factory string and English.** Nothing is
  persisted yet; the game's settings survive a session only. **Review:** the region string is what
  Flycast uses for a blank flash; a real console's factory partition differs and games could read
  it for region locks.
- **CD-audio commands complete as no-ops.** Crazy Taxi's music is ADX from data sectors; a title
  that plays CDDA tracks would be silent, not broken.

- **clang-format pinned to 20.1.7 via the PyPI wheel, in CI and locally.** CI installed the distro
  `clang-format` (Ubuntu 24.04 ships v18) while the runtime headers had been formatted with a newer
  version, so the check went red on code that was locally clean — a version-drift false failure, not
  a real defect. The PyPI `clang-format` wheel is the same binary on every OS, so pinning it (CI:
  `pipx install clang-format==20.1.7`; local: same, noted in `docs/dev-setup.md`) makes the two
  agree. No source files changed: v20.1.7 already considers every checked file clean. Reversal:
  revert the CI step and the dev-setup note. **Review:** bump the pin deliberately when a newer
  clang-format is wanted, reformatting the tree in the same commit.
- **Interrupt handlers run as nested guest calls from the poll site.** Recompiled code cannot be
  pre-empted between C++ statements, so `deliver_irq` performs the SH-4 entry sequence and calls
  the VBR+0x600 function; its RTE restores SR and returns up the C++ stack to the poll. This is the
  cooperative model ADR 7 assumed, made concrete. Limits written down: SPC is approximate, and a
  handler that never returns (longjmp-style task switch) would need a different mechanism; Katana's
  trampoline returns, and the runtime fault log will show if Crazy Taxi does otherwise.
- **The poll flag became a clock deadline** (`Ctx::next_event`, compared with `cycles`). A flag the
  scheduler had no way to set from outside emitted code could never make time advance; the deadline
  also gives timers and VBlank their cadence for free. The emitter changed one line; goldens and
  harness are unaffected because the bare hook disarms it.
- **Untranslated call targets are logged and skipped in the full runtime.** Throwing (the harness
  behaviour) would end the run at the first miss; skipping keeps the game going so the log lists
  every miss at once. It is wrong by construction, which WP2.7's dev interpreter fixes. **Review:**
  acceptable only until then.
- **TMU reload follows the SH7750 manual, one tick apart from Flycast.** The reference reloads
  TCOR-1 on the underflow tick; the manual says TCOR. Unobservable to games; recorded so a future
  lockstep comparison is not mistaken for a bug.
- **SPG uses one event per scanline** (15,700 events a second of guest time) rather than Flycast's
  next-interesting-line optimisation. Simpler, and negligible against the emitted code's cost;
  optimise if profiling ever shows it.

- **Unmapped memory accesses log and read zero instead of aborting.** A title's first run will
  touch devices that do not exist yet (WP2.2–2.6); stopping at the first one would hide everything
  behind it. The fault log is bounded (4,096 unique sites) and printed at exit, mirroring the
  translator's unreached-address report. The bare harness memory keeps throwing, since there a
  stray access is a test bug.
- **VRAM 32-bit view interleave taken from Flycast's `pvr_map32`.** No oracle path covers VRAM, so
  the hardware-proven mapping is copied (GPL-2.0, ADR 1) and pinned by a unit test rather than
  re-derived from the PowerVR documentation.
- **On-chip RAM modelled from the SH7750 manual (bit 13 selects the half).** Flycast does not
  emulate `CCR.ORA`, so there is no reference to compare with; recorded as unverified. Reversal:
  drop the 0x7C000000 case and let it fault.
- **Emitted code keeps calling virtual `Memory` methods.** The plan's "mask-and-index fast path"
  exists inside `DcMemory`, but inlining it into 260k emitted call sites is a code-size and
  compile-time trade to measure on the real game, not to guess now.

- **`jsr`/`bsrf` with a literal target are emitted as direct calls.** The idiom pass showed the
  emitter sent every `jsr` through the run-time function table even when discovery had already
  resolved the target from the same block. A direct call is faster, lets the C++ compiler see the
  callee, and makes the FPSCR summaries apply. Unresolvable sites keep the table lookup. Verified
  by the harness (78/78) and the golden replay; Crazy Taxi compiles unchanged in 13.7 s.
- **Idiom statistics were re-based on walked instructions after a wrong first cut.** Counting
  every word inside function ranges read literal-pool data as instructions (1,172 `bsrf`, 4,716
  GBR accesses); the true numbers are 119 and 172. `tools/idioms.py` now reads the emitted unit's
  instruction comments. The published document uses only the corrected numbers; the mistake is
  recorded here so nobody trusts the earlier figures if they surface in a transcript.
- **THPS2 identified as GCC output from code shape alone** (register push order, frame-pointer
  prologue, `nop` slot ratio); no compiler strings survive in the binary. Recorded as evidence,
  not certainty, in `docs/compiler-idioms.md`.

- **toml++ v3.4.0 vendored as a single header (486 KB, MIT).** ADR 14 fixed TOML for per-game
  configs and the plan lists toml++; the alternative, hand-parsing a subset, would have been a
  second ad-hoc parser next to the JSON one in `discover.cpp`. Header-only, exceptions off, exposed
  as `dream::toml`, linked privately into the translator only. Reversal: delete
  `third_party/tomlplusplus` and `translator/config/game_config.cpp`.
- **FID conflict names are dropped by default.** Ghidra's `FID_conflict:` prefix means several
  library functions matched the same body; picking one silently would mislabel code. The importer
  keeps them only with `--keep-conflicts` (tagged `fid-conflict`). 29 of Crazy Taxi's 394 named
  entries are conflicts.
- **Relocated regions are separate units, not patched into the main image.** The Katana startup
  stub runs at 0x8C004000 but lives in the file at 0x0C010100; the emitter works on one Image, so
  the copy is a second Image view at its destination with its own discovery. Costs one extra unit
  per relocation; keeps the emitter unaware of relocation. Phase 3 wires both units into the runtime.
- **Tony Hawk's Pro Skater 2 (USA) chosen as the second binary for the compiler-idiom pass.** The
  plan asks for one non-Sega title the owner holds; THPS2 is a Western port (Treyarch) and the
  largest of the owner's discs' executables (3.5 MB), so it is the most different from Crazy Taxi's
  SHC/Shinobi build. Extracted into the session scratchpad only; nothing from the disc is committed.

- **Goldens are committed as text, one file per case (796 KB).** The alternative, regenerating
  them in CI from a cached oracle core, needs a 5-minute Flycast build per platform and a Mac
  runner to match the capture host. Text files diff in review, replay in 0.3 s, and are regenerated
  by one script when a case or the fill generator changes. **Review:** trim the memory range in
  the hash of read-only cases if the directory grows past a few MB.
- **WP1.5 declared done with its deviations written down** (flat binaries rather than an ELF
  loader; end-of-case comparison rather than lockstep at each return) and the unbuilt parts
  carried as extensions in `progress.md`, rather than kept "in progress" indefinitely. The
  harness has already found and fixed five real defects, which is the deliverable.

- **Branches that leave a function create functions; overlapping extents are accepted.** The
  harness's GCC unit hit `untranslated call target` inside libgcc's `__sdivsi3_i4i`, which
  `bf`-branches into the middle of `__udivsi3_i4i`. Alternatives were (a) splitting the host
  function at every such target (tried first: it fragmented Crazy Taxi into tiny functions and
  lost five switch tables) or (b) letting the target be a second entry whose emitted copy
  overlaps its host. (b) costs 5% duplicated code and keeps every host intact; the emitter needs
  no change because it already tail-calls out-of-range branch targets. Blocks that the extent
  trim leaves outside a function are enqueued the same way, which took the untranslated direct
  branch targets in Crazy Taxi from 65 to 0. **Review:** the 176 fault
  stubs at undefined words (116 before) are branch targets inside data reached from sweep or
  pointer false positives; harmless unless executed, and the runtime fault log will show them.
- **C test programs are compiled with the KOS GCC, freestanding, and their function ranges come
  from the translator's own discovery** (`tests/sh4/cprogs/gen_specs.py`), not from `nm`, so the
  harness sees exactly the function set the real pipeline would produce, shared tails included.
  The `.syms` file is committed so the specs can be regenerated without the container.

- **Fill pattern is made of in-region pointers.** A byte-noise fill sent struct-walking game
  functions off into unmapped space (the runner faults there, Flycast reads through). Making every
  aligned word an aligned pointer into the region keeps them inside without knowing the struct
  layouts. The remaining state-dependent functions (globals via zero RAM, data-driven loops, one
  Holly register) were pruned by hand and the picker's caveats recorded in its docstring rather
  than adding heuristics for them.
- **Cross-ISA check is now real.** Windows x86-64 CI passes p7's Flycast-captured values under
  MSVC `/fp:strict`; no host-specific code path exists in the FPU helpers. ADR 16 item 4 holds
  for everything the harness covers so far.

- **FSCA table lives in static storage, not built on the stack.** Windows CI crashed `p5_fpu` with
  a stack overflow: the first version built the 512 KB table as a local `std::array` and the
  default Windows thread stack is 1 MB. Linux (8 MB) and macOS hid it. Filled once via a
  function-local static flag. General rule recorded in `emitter-design.md`: no large locals in
  runtime helpers, since guest code may run on threads with small stacks.

- **Game functions enter the harness through an optional CMake include, never through committed
  bytes.** `games/crazytaxi/slice.cmake` references the owner's extracted `1ST_READ.BIN` by path
  and is a no-op when it is absent, so CI and fresh clones are unaffected while the Mac runs the
  slice. The twelve functions were picked by a script over the emitted code (pure leaf FPU code);
  the selection is data, easy to extend.
- **Oracle restores the host FP environment.** Flycast's `setHostRoundingMode` is sticky and
  caches the last mode; the first slice run showed 0.1f parsed as 0x3DCCCCCC because Python's
  `struct.pack` ran in the round-toward-zero the previous oracle call left behind. `dream_oracle_run`
  now saves/restores `fenv_t` and calls `restoreHostRoundingMode()` on entry so each run applies
  the case's FPSCR regardless of history. Without this the p7 run had only been correct because
  p5 happened to run first.

- **`-frounding-math` (and MSVC `/fp:strict`) on every dream target and all emitted code.** The
  harness's first divergence: Clang folded `1.0f/3.0f` at compile time under round-to-nearest,
  but the guest runs round-toward-zero (FPSCR 0x00040001) and Flycast honours it. The flag makes
  the compiler treat the rounding mode as dynamic. Cost: some lost constant folding; the
  whole-binary Crazy Taxi compile (22.5 MB of C++, -O1) took 12.8 s against 12 s before. ADR 16 revised, references copy synced.
- **FSCA table copied from Flycast (393 KB of GPL-2.0 coefficient data).** Hardware FSCA is a
  table; Flycast's coefficients were captured from a real SH-4 and the oracle uses them, so
  `sin`/`cos` can never match bit for bit (sin 90° came out 0x3F7FFFFF). The data is GPL like the
  rest of the repo (ADR 1) and credited in the file header. Reversal: delete
  `runtime/src/sh4/fsca_coefs.inc` and `fsca.cpp`, restore the live computation.
- **FIPR/FTRV evaluated in double, Flycast's way; `runtime/simd.h` from ADR 16 dropped.** Exact
  products in double make the result independent of contraction and of the host ISA, which is a
  simpler guarantee than three tested SIMD paths. Reversal: reinstate the SIMD plan if profiling
  shows FTRV on the hot path in Phase 3.
- **NaN payload behaviour left as a documented gap.** Flycast itself differs between its ARM64 and
  x86-64 hosts here (FPCR.DN on ARM only), so there is no single oracle answer; noted in ADR 16.
  **Review:** whether to canonicalise NaNs to 0x7FBFFFFF at a small per-op cost.
- **p7's expected values are Flycast's captured state, not hand-computed.** That makes
  `dream_emit_tests` on Linux x64 and Windows the cross-ISA determinism check (ADR 16 item 4)
  without needing the oracle core in CI.

- **Oracle built as Flycast's libretro core plus a local patch, not a fork or submodule.** The
  macOS gtest binary turned out to be the Cocoa app (its `main` is the shell's), so it cannot host
  a scripted interpreter. The libretro target is a headless shared library; adding one source file
  and one export-list line gives `dream_oracle_run`. The patch is two files against a pinned
  commit (0abac34) with a build script, so anyone can rebuild it in ~5 minutes; vendoring Flycast
  properly is a Phase 2 question (ADR 1). Reversal: delete `tools/flycast/oracle/`.
- **Differential test is opt-in (`DREAM_FLYCAST_CORE`).** The core takes minutes to build and is
  15 MB, so it is not built in CI yet; the test registers with CTest only when the path is given
  and runs on the owner's Mac. **Review:** a cached-core CI job is worth adding once Phase 2 makes
  Flycast a build dependency anyway.
- **Runner selects programs by name, not address.** Every test program is linked at 0x8C010000, so
  the address-keyed function table returned p1's entry for all of them (the first run showed p6
  computing n²). A generated `kPrograms` table in the umbrella header fixes it; indirect calls
  inside a program still resolve through the shared table, which is fine because no program calls
  its own entry indirectly. Noted in `runner.cpp`.
- **Comparison excludes SR outside T, PR and VBR.** They are set by the harness on each side and
  Flycast reports SR with its MD/RB/BL bits, which the recompiled context does not model as
  architectural state. Everything the guest program can observe is compared.

- **Remaining FPSCR runtime branches accepted as correct.** After switch recovery, the 218 SZ
  tests left in Crazy Taxi sit in the interrupt/exception handlers (functions ending in `stc
  vbr,r0; jmp @r0`), which save and restore the FP register file in whatever mode the interrupted
  code was using. The mode is genuinely dynamic there, so a runtime test is the right lowering and
  clones would not remove it. Decision: no further analysis work on this; per-function clones stay
  deferred.

- **"Calls preserve FPSCR.PR/SZ" assumption falsified by Crazy Taxi.** Traced with a new edge-trace
  debug switch (`DREAM_EMIT_DEBUG=1`) and per-block mode comments in the emitted code: helper
  0x0C08538C contains one `fschg` and returns in the other mode; its callers depend on it.
  Decision: keep the correct runtime-branch fallback, record the design change (callee summaries)
  and implement it next rather than special-casing this game. No owner action.

- **Linear sweep kept as an opt-in discovery source, not trusted.** On Crazy Taxi only 27% of
  sweep-found entries appear in Ghidra's set, against 98% for call-derived and 79% for pointer-
  derived ones. It stays enabled by default in `discover` because a false-positive function costs
  only compile time and an unreachable body, while a missed function costs a runtime fault; the
  translator's fault log is the arbiter. **Review:** flip the default off if compile time matters.
- **Emitter emits slot-targeted labels and entry labels.** Two real-code shapes the test programs
  never produced: a branch into another branch's delay slot (23 sites in Crazy Taxi) and a branch
  to the function's own entry (`bra .` halt padding and loops). Both found by compiling the whole
  translated game with clang, which is now the emitter's smoke test on real code.

- **FPSCR clones (ADR 6 option b) deferred; runtime branch (option c) implemented as the fallback.**
  On the test programs the data-flow pass resolves every mode statically, so there is no evidence
  yet that clones are worth their complexity. The emitter counts and reports every runtime branch
  it emits; when Crazy Taxi is translated that count decides whether clones are built. Reversal:
  implement clone generation in `emit.cpp` where `lower_fp` currently emits the branch.
- **FMAC lowered as a fused multiply-add (`std::fmaf`).** The SH-4 manual describes FMAC as a
  single-rounding operation; Flycast's interpreter is the tie-breaker in WP1.5 and the choice lives
  in one helper (`ops.h: fmac`) so it can flip without touching the emitter.
- **Calls are assumed to preserve FPSCR.PR/SZ** in the mode analysis (Katana library convention).
  A callee that changes the mode and does not restore it would be mis-modelled; the differential
  harness will catch that on real binaries. Recorded in `docs/emitter-design.md`.

- **Phase 0 declared complete on the strength of the dynamic trace.** Steps 8 and 9 were closed
  with the source-built Flycast: syscall surface enumerated, interrupt model confirmed (VBR at
  0x8C00F400). No owner action was needed. The Flycast build lives only in the scratchpad; when
  WP1.5 needs Flycast as a library it will come in as a submodule with the three local fixes
  (Objective-C enable, zlib path, MoltenVK copy guard) recorded here as the build recipe.
- **Tracer command table corrected against Flycast's reios and KallistiOS `cdrom.h`.** My initial
  GD-ROM command numbering was wrong above code 27; the log showed REQ_MODE/SET_MODE/GET_VERS
  where my table said STOP/GETSCD/GETSES. Fixed before recording results.

- **Emitter register model: context struct, no host-register caching (v1).** Recorded in
  `docs/emitter-design.md`. Halves emitter complexity; the host compiler recovers most of the
  benefit once a function is one large straight-line C++ body. Revisit with WP1.5 numbers.
- **Literal-pool folding is on by default in the emitter, but only for words inside the image.**
  Correctness risk: a game that patches its own tables. Mitigation planned in WP1.4 (fold only
  addresses the analysis proves read-only); until then `--no-fold` exists. **Review:** acceptable
  for test programs; for Crazy Taxi the analysis gate must land first.
- **Test programs are hand-assembled SH-4, committed as .s plus .bin.** Two of my first drafts were
  wrong (wrong index register; a division sequence that mixed the manual's two forms), caught by
  the assembler and by the tests themselves, then verified against a Python model of DIV1 before
  the final version. The `div1` helper matched the manual throughout.
- **Flycast scratch clone patched locally** to skip the MoltenVK copy step when Vulkan is off; a
  one-line `if(USE_VULKAN)` guard. Worth sending upstream; not a repo change here.

- **Decoder test mirrors an objdump bug rather than the ISA for four branch forms.** binutils
  2.43.1's SH disassembler prints BT/BF/BT.S/BF.S targets with the 8-bit displacement taken as
  unsigned (verified: `bf loop` assembled to 0xFC is printed as a forward target). Decision: the
  decoder implements the correct signed displacement, a dedicated test asserts it, and the
  65,536-word oracle comparison substitutes objdump's rendering only for those four opcodes. The
  oracle stays as generated so it is reproducible. Reversal: regenerate the oracle with a fixed
  binutils and delete the special case.
- **Flycast side build pinned to the Makefile generator with an explicit zlib path.** Flycast sets
  `ZLIB_LIBRARY` to the bare flag `-lz` for Xcode's benefit, which both Ninja and Make treat as a
  missing file; passing the SDK's `libz.tbd` path fixes configure. Not a repo change.

- **SYSROF converter promoted from "maybe" to "needed".** The FID pass over the GNU and Metrowerks
  SDK variants named only CRI ADX and a few Shinobi routines (9.5% of code); the Sega libraries in
  Crazy Taxi are SHC-built and exist only in Hitachi's SYSROF library format. Decision: write a
  Python SYSROF-to-ELF converter in `tools/ghidra/` as the next step of WP0.4 step 4 rather than
  installing Wine; estimated two days, and it is reusable for every SHC-built title. **Review:** the
  alternative is `brew install --cask wine-stable` plus running the SDK's `libsplit.exe` and
  `elfcnv.exe`; faster to try but a heavier dependency on the owner's machine.
- **Report table corrected.** The "3,989 functions" figure previously attributed to the aggressive
  finder alone included a partial seeding pass; the clean figure is 2,391. Corrected in the report.

- **GD-ROM register question closed on static evidence.** The five register references are boot-time
  G1 bus configuration and constant pokes, not command traffic (dumped with
  `tools/ghidra/DumpFunctionsAt.java`). Decision: step 8 is answered for Crazy Taxi; syscall-level
  GD-ROM HLE suffices. No dynamic confirmation needed for this point.
- **Phase 1 started before Phase 0 formally closes.** WP0.4's remaining items (FID match rate,
  dynamic trace) are waiting on background jobs, and Phase 1 has no owner dependencies, so WP1.1
  (SH-4 decoder) begins now. The decoder oracle is binutils `sh-elf-objdump` from the toolchain
  container run over all 65,536 instruction words (`tests/sh4/oracle/`), committed so the decoder
  test runs on every platform without the container. Reversal: none needed; the plan allowed
  Phases 1 and 2 to start independently of the checklist.
- **CMake 3.31 installed into the session scratchpad via pip** (not on the system) to work around a
  CMake 4.0.2 Objective-C scoping failure when configuring Flycast. Decision: keep Homebrew's CMake
  4 for the repo (builds fine); use the scratchpad CMake 3 only for the Flycast side build.
  **Review:** if Flycast becomes a submodule in WP1.5, its CMake needs either this workaround or a
  fix upstream; noted for then.

- **Flycast source build for the dynamic trace.** The macOS Flycast 2.7 release lacks the GDB
  server and full logging (`ENABLE_GDB_SERVER`, `ENABLE_LOG` default OFF), so checklist steps 8 and
  9 cannot be traced against it. Decision: build Flycast from a shallow clone in the session
  scratchpad with both options on, Vulkan/Breakpad/Lua/Discord off (not needed for tracing). Not
  added to the repository yet; when Flycast becomes the CPU oracle in WP1.5 it will come in as a
  `third_party/flycast` submodule (GPL-2.0, consistent with ADR 1). Reversal: delete the scratchpad.
- **Static evidence accepted for steps 8 and 9 pending the dynamic trace.** Syscall vector call
  sites and the direct-MMIO register profile were extracted from the binary and recorded in
  `games/crazytaxi/checklist-report.md`. Decision: treat step 8 as substantially answered (all
  visible disc access is via the GD-ROM syscall vector; five GD-ROM register references under
  investigation) and step 9 as "VBlank via SPG_STATUS polling and Holly IRQs, rate lock to be
  confirmed dynamically". Reversal: none needed; the dynamic trace will refine, not replace.
- **FID database built from GNU and Metrowerks variants only, Hitachi SYSROF deferred.** Ghidra
  cannot import Hitachi's object format and macOS cannot run the SDK's `libsplit.exe`/`elfcnv.exe`
  without Wine. Decision: build the database from the 7,172 ELF objects available now and measure
  the match rate on Crazy Taxi; only if it is low (which would suggest the game was built with SHC)
  invest in a SYSROF-to-ELF converter (preferred, pure Python) or Wine. **Review:** if the
  converter is needed it is roughly two days of work; Wine is a `brew install --cask wine-stable`
  but adds a large, fragile dependency to the owner's machine.
- **`gdbtrace.py` written without a live server to test against.** Decision: write it now against
  the GDB Remote Serial Protocol spec so it is ready the moment the source build finishes; it is
  marked untested in its header until then.
- **Flycast config edits reverted.** To test the GDB server I toggled `Debug.GDBEnabled` and
  `Debug.GDBWaitForConnection` in the owner's `emu.cfg`; both were restored from the backup once
  the build was found to lack the feature. No lasting change to the owner's Flycast settings.
- **Flycast launched on the owner's Mac.** Twice, briefly, to test the GDB port, and killed
  afterwards. It boots the game with Flycast's HLE BIOS (no BIOS image present), which is itself a
  useful data point recorded in the report.
- **SDK R10.1 sourced via sparse checkout.** Owner approved the pull; decision on *how*: a sparse
  `git clone` of only `Lib/`, `Include/`, `Utl/` (275 MB) rather than the full mirror, kept under
  the ignored `sdk/` folder and symlinked as `sdk/katana-r10.1`. Reversal: `rm -rf sdk/`.
- **Container based on the KallistiOS project's image instead of building the toolchain.** The
  official `kallistios/dc-kos-toolchain:14.2-stable` is multi-arch, maintained, and ships a
  prebuilt KOS. Decision: use it and drop the "build sh-elf-gcc from source" plan; WP0.2 shrank
  from 3 days to 1. Reversal: a from-source Dockerfile can replace the `FROM` line later.
- **Flycast-as-library moved from WP0.2 to WP1.5.** It is only needed once the differential
  harness exists; carrying it in the base container would slow every CI job. Plan updated.
- **macOS CI job gated to manual dispatch and tags.** The repository is private and macOS runner
  minutes cost 10x. The Mac is the primary development machine and is verified locally on every
  change. **Review:** flip to unconditional if the repo goes public.
- **libchdr submodule pinned at upstream master (8e7b8bd, v0.3.0-113).** Latest commit at the
  time of adding; the Debian snapshot used for testing on Linux was 9108f34 (Sept 2023). Both read
  the fixture and the real CHDs identically. Reversal: `git -C third_party/libchdr checkout <sha>`.
- **Tools installed on the owner's machine:** `clang-format` (Homebrew), needed so local formatting
  matches the CI check; `unshieldv3` built in the scratchpad only (not installed) to unpack the
  SDK installer. No other host software added.
- **Diagnostic scripts kept in the repo.** `tools/ghidra/DebugPointers.java` and
  `DumpFunctionsAt.java` are small and were useful more than once; kept rather than deleted.

## 2026-09-12 (Crazy Taxi boot to the sound-driver wait)

- **Non-local returns in emitted code** (ADR 4 revised, `docs/emitter-design.md`). Every function
  gained a resume entry and an `rts` PR check; `sh4::run_guest` is the new top loop the launcher
  and the harness use. Code size grows by one `switch` per function; a normal return costs one
  compare. Alternative considered and rejected: host fibers per guest task (portable stack
  switching is a platform-specific pile, and resuming at a saved PR still needed labels).
  **Review:** the design; the cost is measurable if a title switches tasks thousands of times per
  frame (Crazy Taxi: a few).
- **RTE task switches** (ADR 7 revised): `System::on_rte` throws to SPC when the handler resumes a
  different context; poll points store their pc. Reversal: the two lines in `on_rte`.
- **BIOS syscalls charge guest cycles** (100; GD-ROM 300) and the status syscalls plus every
  device register access advance the scheduler to the guest clock (`DcMemory::on_device_access`).
  Without both, Crazy Taxi's 120-try status loop and `syTmrGetCount` busy-wait gave up before time
  moved. **Review:** the two constants are guesses at the real BIOS's cost; a dcload measurement on
  hardware would pin them.
- **Discovery origin ranking**: pointer/sweep entries no longer trim called functions; delay-slot
  addresses are rejected as seeds. Function count 3758 -> 3884 (overlapping copies), FP runtime
  branches 144 -> 155.
- **p10_longjmp is linked at 0x8c030000**, unlike the other programs at 0x8c010000: its resume
  goes through the function table by address, which must not collide with other programs linked
  into the same test binary. `run_guest(Ctx&, Memory&, GuestFn)` exists for the same reason.
- **Kept in the repo:** `tools/trace/diff_returns.py` and the launcher's `--interpret`,
  `DREAM_TRACE_RETURNS`, `DREAM_TRACE_NONLOCAL`, `DREAM_INTERP_RANGE` and image-integrity report;
  `DREAM_TRANSLATE_EXTRA_FLAGS` CMake cache variable for the game unit (dev trees only).
- **Flycast oracle rebuilt** into `build/flycast-oracle/` (the scratchpad copy was gone); the build
  trees are configured with `-DDREAM_FLYCAST_CORE=` pointing there. The p9 golden had been
  committed stale (`a8138f9` was made before its suites were rerun); regenerated with the oracle.
- **Instrumentation left in `System`:** `task_switch_rtes`/`first_switches` counters and the
  `frame_pcs`/`frame_sps` stacks that detect a switched RTE are now load-bearing, not diagnostics.
- **WP2.5 started with the control half** (ARM7 + AICA registers/timers/interrupts) because the
  Crazy Taxi boot was parked on the sound-driver handshake; sound generation follows. The ARM7
  decode table is Flycast's VisualBoyAdvance-derived `arm-new.h` copied verbatim into
  `runtime/src/aica/arm7_ops.inc` (GPL-2.0, attribution kept) with the glue rewritten as a class;
  warnings inside the table are suppressed with pragmas rather than edited. **Review:** the SWI/
  undefined return address deviates from Flycast's interpreter glue on purpose (see
  `docs/runtime-aica.md`, "SWI/undefined"); the ARM7 oracle cross-check the plan asks for is
  still to do.
- **Sound generation ported before the title makes a sound.** Once the driver ran, the mixer,
  DSP and G2 DMA were ported so that the first command from the title would be audible; the
  title turned out to be blocked in its ADX loader by a GD-ROM HLE difference (see
  `docs/runtime-hle.md`, Open). The mixer's lookup tables are generated once by
  `tools/aica/gen_tables.py` and committed so all hosts mix bit-identically; Flycast computes them
  with libm at start-up. `GET_DRV_STAT` now reports BUSY during commands (was PLAY); reporting
  CONTINUE for a finished DMA read was tried and reverted. The owner's direction (2026-09-12):
  audio only has to work; graphics is where the value is.
- **The silent title was waiting for the Start button, not blocked on emulation.** A day went into
  reverse-engineering Katana's GD file system and CRI's ADX loader on the theory that the BIOS HLE
  was at fault. The tell was there all along: the title rendered every frame and read the disc
  steadily, which is not what a stalled program does. `--press start@FRAME` in the launcher (hold
  a button for eight frames) got it past the screen and the music started. **Rule for the next
  bring-up: a headless title that renders and loads but does nothing new is waiting for input.**
  The BIOS work was not wasted (GET_DRV_STAT now matches Flycast, and both HLE traces can be
  diffed via `tools/flycast/oracle/flycast-gdrom-trace.patch`), but it was the wrong suspect.
- **Standalone Flycast was not built in the end.** Two macOS build failures (a zlib link path and
  an Objective-C precompiled-header clash) burned time for a comparison that turned out to be
  unnecessary; the libretro core we already build for the CPU oracle carries the trace patch if a
  BIOS comparison is ever needed. **Review:** whether to keep a standalone Flycast build recipe.
- **The display-list decoder is cross-checked against the runtime's own TA parser.** Two parsers
  written for different purposes (one for timing and statistics, one for geometry) agreeing on a
  real captured frame is a stronger claim than either alone, and it caught nothing only because the
  synthetic tests had already caught the strip-lifetime bug. Kept as a permanent test.
- **`--dump-ta` captures one render's parameter stream** for that test; the capture lives in the
  gitignored `games/*/extracted/` and the test skips without it, like the golden traces.
- **MSVC found dead code the other compilers accepted**: `switch` on a template constant leaves
  every other arm unreachable, which `/W4 /WX` rejects. Rewritten as `if constexpr`, which is what
  it should have been. Second time this week that a non-clang compiler caught something real; the
  new Linux renderer CI job exists for the same reason.
- **The first rendered frame looked blank, and the diagnosis was the tooling, not the renderer.**
  `dream_render_view --fit --screenshot` showed the captured Crazy Taxi frame spans x -640..1280,
  three screens wide, with 26 of 524 polygons on the 640x480 screen; the rest is UI the game keeps
  off-screen. Without the fit-and-screenshot path this would have been hours of guessing at depth
  modes. Kept as a permanent tool. **Review:** whether the off-screen geometry means the game
  expects a different framebuffer origin; textures (step 4) will make that obvious either way.
- **GCC caught two more narrowing bit-field stores** in the renderer's pipeline key, the same
  pattern as the mixer. The Docker GCC check in `docs/dev-setup.md` now runs against the Vulkan
  sources too, which is how these were found before CI.
- **Crazy Taxi renders its title screen** (2026-09-12). The path from a recompiled SH-4 instruction
  to a picture is now complete end to end: translated code, the runtime, the display-list decoder,
  the texture decoder and the Vulkan renderer. The screen it draws is the game asking for a VMU,
  which is an owner task (`docs/owner-tasks.md`); with a VMU dump the title would proceed past it.
- **Texture invalidation is coarse on purpose**: the whole cache is dropped when palette memory
  changes, rather than tracking which texels a write touched. **Review:** revisit if a title shows
  a measurable cost; the failure mode of getting fine-grained invalidation wrong (textures that
  never update) is much worse than decoding one again.
- **The owner's Flycast install already had the Crazy Taxi save** (`MK-51035_vmu_save_A1.bin`,
  matching the product code in the game's own IP.BIN), so the VMU owner-task was already satisfied
  and the dump from real hardware is no longer needed to get past the title's save prompt. Reading
  it needed the Maple bus to route to expansion slots and to advertise them, plus one byte-order
  fix.
- **A capture tool that mixes two moments in time lies.** The garbled strip on the first title
  screen was not a texture bug: video memory was dumped at the end of the run while the display
  list came from a render much earlier, and the texture it referred to had been overwritten.
  `--dump-ta` now writes video memory beside the first capture, from the same instant, and the
  artifact is gone. **Rule: a capture must be of one moment, or it will be debugged as a bug.**
- **Idea recorded for later (owner):** a hotkey that renders the VMU screen. Titles send it images
  continuously (Crazy Taxi, 229 writes in a short run) and the traffic is already accepted and
  counted; drawing it is small once there is a window.
- **Per-strip transparency sorting, not per-pixel** (2026-09-12). The hardware sorts per pixel and
  the development machine can too (fragment stores and atomics are present), but a per-strip sort
  is a fraction of the work and is right for everything except intersecting translucent surfaces.
  **Review:** promote to per-pixel when a title shows the difference; the capability check is
  already in `vk::Context`.
- **Modifier volumes are decoded but deliberately not drawn.** No captured Crazy Taxi frame
  contains one, so shadow rendering would be written blind and look finished while being wrong.
  Listed in "Not done" until there is a frame to verify against.
- **"Too dark" was a flag, not a colour-space bug.** With blending enabled the whole title logo
  darkened. The cause was the TSP word's use-alpha bit: clear means the surface is opaque and the
  vertex alpha is data for something else. Worth remembering because the symptom (a correct
  picture, uniformly too dark) points naturally at gamma and not at a per-polygon flag.
- **A different render-target address is double buffering, not render-to-texture** (2026-09-12).
  The first version of `describe_render_target` said "a texture" whenever the write address
  differed from the displayed one, and reported Crazy Taxi's ordinary double buffering as a render
  to texture. Corrected to report the address and leave the judgement to the caller, because the
  real test needs to know whether the written region is later sampled, which only the texture cache
  can answer. **A confident wrong answer in a diagnostic is worse than no answer.**
- **Decoding the framebuffer from a capture gives noise, and that is expected**: nothing writes a
  rendered frame into video memory during a boot, because the launcher is headless and owns no
  renderer. The conversion is unit-tested in both directions instead, and connecting it is the
  integration step after step 6. *Resolved the same day: `--window` now writes each frame back, and
  decoding a dump taken from a windowed run gives the game's own screen.*
- **The frame goes the long way round, on purpose** (2026-09-12) — **reversed the same day.** The
  first version wrote every frame back into video memory in the guest's own pixel format and showed
  whatever `FB_R_SOF1` pointed at, so that a title's own framebuffer writes would compose and a
  render to texture would be real rather than simulated. On Crazy Taxi the write lands on top of
  texture data the title keeps in the same memory and erases its logo. Write-back is now behind
  `--framebuffer-writeback`, and the window is shown the rendered image whenever the guest is
  displaying a buffer the renderer drew into. The guest still nominates which buffer; only the
  source of the pixels changed. Flycast defaults the same way for the same reason.
- **"One the renderer has drawn into", not "the one it drew last"** (2026-09-12). The first attempt
  compared the displayed address with the most recent render target and matched on zero frames out
  of 295: Crazy Taxi holds the display on one buffer for four or five frames while rendering into
  the other. Keeping the set of buffers the renderer has ever targeted matches all 295, and a
  buffer never targeted is exactly the one whose pixels the title wrote itself.
- **Render size separated from window size from the start.** The renderer draws into its own image
  at the guest's resolution times `--scale`. Doing this now rather than when upscaling is wanted
  cost nothing and means the enhancement is one argument instead of a redesign. Showing the
  rendered image rather than a copy squeezed back through the guest's pixel format is also what
  makes the extra resolution reach the screen at all.
- **The launcher's renderer is optional at link time.** `crazytaxi_boot` links `dream::render_vk`
  only when that target exists, behind `DREAM_WITH_RENDERER`, and `--window` reports that the build
  has no renderer rather than doing nothing. CI keeps building and testing the headless
  configuration untouched.
- **A control that removes a consequence is not a control** (2026-09-12). Writing frames back drew
  two red rectangles on Crazy Taxi's "no VMU" screen where the logo and a number belong. Running
  the same frames with the texture cache held across the write
  (`DREAM_NO_WRITEBACK_INVALIDATE=1`) gave a pixel-identical picture, and that was written up as
  "the fault predates this step". It did not. Holding the cache stops the renderer *noticing* the
  write; the write still happens, so both runs decoded textures that had already been overwritten.
  The control that settles it removes the write itself, which `dream_render_view` does on the same
  capture, and there the logo is complete. **The wrong conclusion was committed and had to be
  corrected in the next commit, which is the cost of a control that only removes a symptom.**
- **Reading the register map twice would have been cheaper** (2026-09-12). `FB_W_LINESTRIDE` was
  read at 0x06C, which is `FB_Y_CLIP`. Its low bits are the clip's first line and so are normally
  zero, so the wrong register produced a plausible stride of "packed rows" and survived review. It
  was found only while chasing the write-back bug. The write size now comes from `FB_X_CLIP` and
  `FB_Y_CLIP`, which is what the hardware clips against, with the caller's size as a fallback.
- **Palette memory is compared, not reloaded** (2026-09-12). `set_memory` invalidates the whole
  texture cache, so calling it every render would decode every texture every frame. `set_palette`
  re-unpacks palette memory and invalidates only when the contents actually changed: one change in
  a 700-frame Crazy Taxi run.

- **Four TSP fields were at the wrong bit positions, and the test came second** (2026-09-12). The
  red rectangles on Crazy Taxi's title and "no VMU" screens were not the write-back after all, or
  not only: "ignore the texture's alpha" and "use alpha" were each read one bit high, so a logo
  tile's transparent parts were drawn opaque, and the u and v mirror flags and the filter mode were
  four bits low. Found by reading Flycast's `union TSP` beside our extractors after the write-back
  fix left the rectangles in place. The fields now live in `render/tsp.h` with
  `render/tests/test_tsp.cpp` pinning each one by setting it alone with every other bit set. **A
  wrong bit position in a hardware word does not fail a build, it draws**, and it draws something
  plausible enough to be blamed on gamma, on blending, or on the last thing that changed.
- **Yellow wedges across the SEGA logo were a polygon header size, not a colour** (2026-09-13).
  The screen is a hundred and five small strips sampling one 8x8 all-white texture, so every colour
  comes from the face colour in the header. Twenty of those strips use intensity mode 2, whose
  header is always 32 bytes; we read them as 64, ate the first vertex of each strip, and drew the
  rest from the wrong corners. The give-away was that the wedges had diagonal edges, which is what
  a quad split into two triangles looks like when one triangle's corners are wrong. Found by
  reading Flycast's `poly_header_type_size` beside ours, not by staring at the picture.
- **Capture by guest frame, not by render index** (2026-09-13). `--dump-ta-frame N` names a render,
  but the render index for a given moment is not stable between runs, because the real-time clock
  is seeded from the host clock. Half an hour went into capturing the wrong screen before that was
  noticed. `--dump-ta-at-frame N` captures the first render at or after a guest frame, and the
  screenshot message now prints both numbers so a picture can be matched to a capture.
- **The background plane is not in the display list, and its absence looks like a black screen**
  (2026-09-13). The owner reported black where the SEGA screen should be white and a missing yellow
  behind the title. Both were the same gap: the plane every frame sits on is written by the title
  into the parameter buffer and pointed at by `ISP_BACKGND_T`, so nothing in the parameter stream
  mentions it. Reading it also needs the 32-bit view of video memory; read flat, the address lands
  on zeros, which is indistinguishable from a title that has no plane. That false negative is now a
  test of its own.
