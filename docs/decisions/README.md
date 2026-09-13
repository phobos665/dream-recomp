# Decisions to make before Phase 0

Each decision below is an architecture decision record (ADR). Status is one of **Proposed** (my
recommendation, awaiting sign-off), **Accepted**, or **Superseded**. All sixteen were accepted by
2026-09-12; a change now needs a superseding ADR. The feasibility report
(`../feasibility-report.md`) identified licensing as "the strategic decision that most shapes the
project"; this document lists it alongside the other choices that block the first commit of code.

Recommendations are ordered by how much they constrain everything after them.

| # | Decision | Recommendation | Status |
|---|---|---|---|
| 1 | Runtime licence and reference implementation | GPL-2.0 for the whole repository, Flycast as primary reference, Deecy (MIT) as secondary | **Accepted** 2026-09-10 |
| 2 | Development-time interpreter fallback | Yes, dev-only, behind a build flag, never in release builds | **Accepted** 2026-09-12 |
| 3 | Implementation language and build | C++20 for translator and runtime, CMake, clang/MSVC; Python for scripts | **Accepted** 2026-09-12 |
| 4 | Emitted code shape | One C++ function per guest function, `sh4_ctx` struct, registers in locals | **Accepted** 2026-09-12 |
| 5 | Memory model | Mask-and-index fast path for RAM, switch to MMIO handlers; no mmap tricks in v1 | **Accepted** 2026-09-12 |
| 6 | FPSCR mode handling | Data-flow inference, then mode-specialised clones, then runtime branch | **Accepted** 2026-09-12 |
| 7 | Interrupt and timing model | Cooperative checks at function entry and loop back-edges, virtual clock, idle-loop skip | **Accepted** 2026-09-12 |
| 8 | Code discovery and symbol source | Recursive descent plus constant-pool harvesting, Ghidra FID databases for Katana R09–R11, log-and-iterate closure | **Accepted** 2026-09-12 |
| 9 | Graphics back-end | SDL3 windowing, Vulkan renderer ported from Flycast's per-pixel OIT path | **Accepted** 2026-09-12 |
| 10 | Audio | ARM7 interpreter plus AICA channel/DSP emulation (Flycast-derived) | **Accepted** 2026-09-12 |
| 11 | Disc access | GDI and CHD both first-class from Phase 0 (libchdr, chdman fallback); syscall-level GD-ROM HLE with register-level fallback later | **Accepted** 2026-09-12 (revised 2026-09-10) |
| 12 | CPU correctness oracle | Flycast interpreter in CI, real hardware via dcload for ground truth | **Accepted** 2026-09-12 |
| 13 | Host platforms for v1 | macOS ARM64 (primary development machine), Windows x64, Linux x64 | **Accepted** 2026-09-12 (revised 2026-09-10) |
| 14 | Repository layout and per-game config | Monorepo; TOML per-game config | **Accepted** 2026-09-12 |
| 15 | Baseline commercial title | Crazy Taxi (owner's choice, confirmed by measurement; Charge 'N Blast and Tech Romancer as alternates), see `../baseline-game.md` | **Accepted** 2026-09-10; checklist steps 1-7 passed 2026-09-11 |
| 16 | Cross-ISA portability and floating-point determinism | ISA-neutral emitted code, `-ffp-contract=off`, fenv abstraction, golden traces must match across x86-64 and ARM64 | **Accepted** 2026-09-12 |

---

## ADR 1: Runtime licence and reference implementation

**Context.** The feasibility report frames this as a binary choice: build on Flycast (GPL-2.0, fast)
or clean-room from Sega/Renesas documentation and KallistiOS (slow, permissive). Research on 10 Sept
2026 found a third input the report did not weigh: Deecy, a Zig Dreamcast emulator, is MIT-licensed
(with MAME-derived data under BSD-3-Clause), implements PVR2, AICA/ARM7, Maple and GDI/CDI/CHD
loading, and runs commercial titles including Soul Calibur and Grandia II. It is far less mature than
Flycast and lacks Flycast's fifteen years of per-game edge cases.

**Options.**

1. GPL-2.0 runtime, port Flycast's PVR/AICA/GD-ROM cores directly. Fastest to first playable title.
   Any distributed game executable is GPL; mod and launcher ecosystems must be GPL-compatible.
2. MIT runtime, port Deecy's cores and fill gaps from documentation and KallistiOS. Permissive, but
   Deecy is experimental and the gap-filling is real work.
3. Full clean-room. Slowest by a wide margin, no clear benefit over option 2 now that an MIT
   reference exists.

**Recommendation.** Option 1 for the runtime, with two hedges:

- The **translator** (SH-4 decoder, CFG builder, C++ emitter, FID tooling) never links against the
  runtime. Licence it MIT so it can be reused by anyone, including a future permissive runtime.
- Keep a **module boundary** between "hardware cores" (TA parser, renderer, AICA, GD-ROM) and the
  rest of the runtime (memory, scheduler, HLE, Maple), so that swapping the GPL cores for MIT ones
  later is a bounded task rather than a rewrite.

**Rationale.** The project's risk is not licensing, it is never reaching a playable commercial title.
Flycast is the only reference that has already solved every PVR2 corner case a retail game will hit.
The N64 ecosystem picked permissive licences because RT64 existed; the Dreamcast equivalent (Deecy)
is not yet at that level. If the project succeeds, a permissive runtime can follow, and the MIT
translator makes that possible.

**Decision (2026-09-10).** Accepted as GPL-2.0 for the **whole repository** (`LICENSE` at the root).
The MIT carve-out for `translator/` and `tools/` proposed above is deferred: keep the translator free
of runtime dependencies so relicensing it later remains a one-line change, but do not split licences
now. No Sega SDK code or headers may be committed.

---

## ADR 2: Development-time interpreter fallback

**Context.** The Initial D project explicitly rejects any SH-4 interpreter or JIT fallback and
converges on zero untranslated calls through iterative fault logging. That is the right shipping
posture, but during development it means the game cannot boot until discovery is nearly complete.

**Recommendation.** Include Flycast's SH-4 interpreter as a **dev-only** fallback behind
`DREAM_DEV_INTERPRETER`. When an indirect jump lands on an untranslated address, the interpreter
executes until control returns to a known function, and the address is logged for the next translate
pass. Release builds compile the fallback out; the function table lookup then aborts with a fault log
exactly as Initial D does.

**Rationale.** Cuts the discovery loop from "rebuild, run, crash, read log" to "run, read log" and
lets the runtime (PVR, AICA, Maple) be developed against a booting game before the translator is
finished. The licensing cost is nil given ADR 1.

**Consequence.** CI must build and test the no-interpreter configuration so the fallback never becomes
load-bearing.

**Revision (2026-09-11, WP2.7).** The fallback is built from the project's own verified pieces
rather than a port of Flycast's interpreter: the SH-4 decoder (checked against binutils on every
opcode word) and the `ops.h` helpers (checked bit for bit against Flycast by the differential
harness), with each opcode's semantics being the run-time form of the statement the emitter emits
(`docs/runtime-devinterp.md`). Reasons: it removes an adapter between two context layouts and two
memory models; translated and interpreted code share one definition of every instruction, so they
cannot drift apart; and the correctness argument is direct, since the golden replay runs every
captured Flycast state through both paths and requires identity. The rest of the record stands:
dev-only behind `DREAM_DEV_INTERPRETER`, compiled out of release builds, which throw
`dream::UntranslatedCall` with the fault log populated; CI builds and tests both configurations.

---

## ADR 3: Implementation language and build

**Recommendation.** C++20 for both translator and runtime, CMake, clang on Linux and MSVC or
clang-cl on Windows. Python 3 for scripts (disc extraction, FID database generation, test
orchestration). Emitted code is C++20 so memory-access helpers can be templates.

**Rationale.** N64Recomp, XenonRecomp and the Initial D runtime are all C++; contributors from that
community expect it. Flycast code being ported is C++. Rust for the translator was considered and
rejected only to avoid a second toolchain; revisit if the translator becomes a standalone product.

---

## ADR 4: Emitted code shape

**Recommendation.** One C++ function per discovered guest function, signature
`void fn_8c0xxxxx(sh4_ctx&)`. Guest registers live in locals inside the function and are written
back to the context struct only at calls, returns, and interrupt-check points. Indirect calls go
through a sorted address-to-pointer table with a hot-path hash. Delay-slot instructions are emitted
before the branch effect; the Initial D v2461 bug (slot instruction placed after an early `return`)
gets a dedicated regression test before any game code is translated.

`DIV1` step sequences and T-bit chains are emitted as plain C and left to the host compiler; pattern
collapsing is an optimisation for later, gated on profiling.

**Revised 2026-09-12 (non-local returns).** Guest registers live in the context struct (the
emitter's v1 choice, `docs/emitter-design.md`), which is what makes this possible: every emitted
function body is also a resume entry that can be re-entered at any block start or call-return
address, `rts` to an address other than the entry PR throws a non-local return, and a runtime loop
(`sh4::run_guest`) re-enters the target. Needed because Katana's cooperative task switcher returns
through a restored PR into another task; a plain `return` corrupted the caller. The host stack
mirrors the guest's only until the first switch; afterwards every return dispatches through PR.

---

## ADR 5: Memory model

**Recommendation.** Physical 29-bit address after masking the P0–P3 area bits. Fast path:
`if ((addr & 0x1C000000) == 0x0C000000) return ram[addr & 0xFFFFFF]` for the 16 MB RAM mirror,
then a switch on the area bits for VRAM (32-bit and 64-bit views), sound RAM, store queues, and MMIO.
No large reserved virtual region with mirrored mappings in v1.

**Rationale.** Little-endian guest means no byte swap, so the mask-and-index path is already one
compare and one load. The mmap trick that Flycast and XenonRecomp use is an optimisation with
platform-specific setup cost; do it only if profiling shows memory access dominating, which Initial D's
numbers suggest it will not.

---

## ADR 6: FPSCR mode handling

**Recommendation.** As the report proposes, in order: (a) forward data-flow pass over the CFG to
infer PR/SZ/FR at every FP instruction, seeded from the entry state and every `LDS Rn,FPSCR`,
`FSCHG`, `FPCHG`, `FRCHG`; (b) for functions where inference fails, emit up to four clones
specialised on PR and SZ and select at the call site; (c) runtime branch on the mode bit as a last
resort, with a translator warning so it is visible. Host MXCSR/FPCR rounding is set from FPSCR.RM at
every mode change. `FIPR`/`FTRV`/`FSRRA`/`FSCA` use Flycast's reference implementations, not
bit-exact hardware precision, unless a title's replay determinism demands it.

**Test requirement.** Differential test against the interpreter with FPSCR fuzzing before Phase 0
exits.

---

## 
**Correction (2026-09-13).** The inference seeded a function with no caller in the image at the
reset mode. That is a guess, not a deduction, and it is wrong for every interrupt handler: an
interrupt arrives in whatever mode the code it interrupted was using, which is not a property of the
handler and cannot be read off the image. A wrong guess is not a small error, because the mode
decides how many bytes an `fmov` moves and how far it advances its pointer, so the handler and
everything it reaches transform the wrong bytes and walk at the wrong rate.

Such functions are now seeded unknown, which propagates and falls back to (c), a runtime branch on
the mode bit. Measured on Crazy Taxi: runtime branches go from 155 to 25,525, and the 8,139 calls
per run that arrived in a mode the emitter had not compiled for become zero. The cost is a drop from
4.5x to 3.8x real time headless, about 15 per cent, which is the price of being right and is
recoverable later through (b), the mode-specialised clones this ADR already calls for and which
remain deferred.

The check that found it is worth keeping: the emitter now records the mode it compiled each function
for, the development build carries it to run time, and one run names every function whose assumption
the guest violates. That is a direct test of this ADR's premise rather than an inference from a
crash.

ADR 7: Interrupt and timing model

**Recommendation.** Cooperative delivery: `if (ctx.irq_pending) deliver(ctx)` at function entry and
at loop back-edges. A virtual clock advances by per-block instruction estimates and by MMIO accesses;
TMU, SPG/VBlank, TA list-complete, render-done, DMA-done, Maple and GD-ROM events are scheduled on
it. Idle loops (a block that only polls a register or RAM flag and branches to itself) are detected at
translate time and emitted as "advance clock to next event". Delivery saves the context, swaps R0–R7
to BANK1, sets SR.BL/RB/MD, and calls the recompiled handler at `VBR + 0x600`; `RTE` unwinds.

**Known limitation.** Raster-timed effects and tight VBlank races may need per-title tuning of check
density. Accept this; the baseline title (ADR 15) is chosen so it does not arise.

**Revised 2026-09-12.** Three additions from the Crazy Taxi boot: an `RTE` that resumes a context
other than the interrupted one (Katana's scheduler runs inside the VBlank handler) is a non-local
return to SPC rather than an unwind, with poll points recording their exact pc so SPC is
resumable; every device register access advances the virtual clock to the guest's cycle count
(timers and drive status must observe the present, not the last poll); BIOS syscalls charge guest
cycles (100, GD-ROM 300) because titles calibrate polling loops against the real BIOS's cost.

---

## ADR 8: Code discovery and symbol source

**Context.** The report rated symbol availability "Poor, signatures needed" and proposed building a
FLIRT-style database from the first commercial title. Research found this already exists in a usable
form: AltoRetrato's Dreamcast reverse-engineering diaries ship `build_dc_fidb.py`, which generates
Ghidra Function ID databases from Katana SDK releases R09, R10 and R11, and the `iamsh4/dc-re-ghidra`
scripts import SDK headers (types and signatures) into Ghidra. Ghidra has had native SH-4 support
since 9.1 and there is a dedicated Dreamcast loader (`sega-dreamcast/ghidra-loader`).

**Recommendation.**

- Seeds: entry point 0x8C010000, IP.BIN bootstrap, `LDC Rn,VBR` handler tables, BIOS syscall
  installers, and every constant-pool word that lands in the code range.
- Recursive descent with delay-slot awareness; `BRAF`/`JMP @R0` switch-table recovery.
- **Use the existing Ghidra FID databases** for Katana library identification instead of building a
  signature system from scratch. Export the matched function list (address, name, SDK version) as
  the translator's symbol input. The translator itself stays Ghidra-independent; Ghidra is a
  development tool, not a build dependency.
- Log-and-iterate closure with the dev interpreter (ADR 2) reporting unreached addresses.
- Every SDK-identified function is a candidate for selective HLE later (Ninja matrix stack, Kamui
  TA submission, Manatee mailbox), but v1 translates them like any other code.

**Consequence.** The FID databases are built by each developer from their own SDK copy; the generated
`.fidb` files are SDK derivatives and are not committed.

**Evidence (Crazy Taxi, 2026-09-11, Ghidra 12.1.3).** Recursive descent from the entry alone found
283 functions (1.9% of the image); the same at the wrong base address is what a naive translator
would do. At the true link address (0x0C010000, with the startup stub mapped at its 0x8C004000 copy)
plus an aggressive linear-sweep instruction finder: 3,989 functions, 26.1%, essentially all the code
(the binary has 3,997 `RTS`). Raw literal-pool seeding on top: +191 net functions, +849 bytes, and
about 1,400 false positives from data pointers. So the discovery pass must (a) get the image model
right first (link address, relocated copies), (b) include a linear-sweep finder validated by the
prologue/`RTS` structure, and (c) treat pointer seeds as candidates that must pass a plausibility
test, never as facts. Log-and-iterate closure at runtime remains the backstop.

---

## ADR 9: Graphics back-end

**Recommendation.** SDL3 for window, input and audio output. Vulkan renderer, ported from Flycast's

**Confirmed 2026-09-12.** The macOS risk is resolved: MoltenVK on an Apple M4 reports
`fragmentStoresAndAtomics` and `VK_EXT_fragment_shader_interlock`, so the per-pixel transparency
path runs there, not just the per-strip fallback. Three portability points are now in the code and
in `docs/runtime-render.md`: the instance must opt in to portability drivers or the loader will not
enumerate MoltenVK at all; the device must enable `VK_KHR_portability_subset`; and MoltenVK's
Apache-2.0 licence against this project's GPL-2.0 means linking dynamically to the Vulkan loader
and shipping MoltenVK beside the binary, as Flycast does. Feature support is a property of the GPU
rather than the operating system, so the per-strip fallback is still required.
per-pixel order-independent-transparency Vulkan path, fed by a TA parameter parser derived from
Flycast's `ta_vtx.cpp` lineage. VRAM is an 8 MB array with both access views, texture cache
invalidation on CPU write, YUV converter, and framebuffer direct-write for 2D titles. Internal
resolution scaling from day one; widescreen and uncapped-fps hooks are per-game work in Phase 3.

**Rejected.** OpenGL first (simpler) was rejected because the OIT path needs GL 4.3+ anyway and the
Initial D project has shown Vulkan is sufficient on a small team. Metal/macOS is deferred (ADR 13).

---

## ADR 10: Audio

**Recommendation.** ARM7DI interpreter plus AICA 64-channel and DSP emulation, both Flycast-derived.
Manatee-driver HLE is an optional Phase 3 per-game layer for music replacement, never a correctness
dependency.

**Decision (2026-09-12).** Accepted as recommended (option 1 below). The owner asked whether audio
had a better or more reliable approach than ARM7 emulation, citing how XenonRecomp/ReXGlue titles
handle sound; those titles play audio through the Xbox 360 OS API, which the runtime implements on
the host, whereas the Dreamcast has no OS audio layer and the seam would have to be the sound
driver's undocumented mailbox. The alternatives, kept for the record:

1. **LLE, as recommended.** Emulate the ARM7 and AICA; the Manatee driver runs unmodified. Matches
   every title with no per-game work; the ARM7 core costs about 2% of a host core.
2. **Statically recompile the sound driver too.** Manatee is an ARM7 binary uploaded into sound RAM
   at boot; it could be lifted to C++ the same way the SH-4 code is. Needs an ARM7 decoder and
   emitter (a second translator) for a driver that is identical across most Katana titles.
3. **HLE the driver's mailbox protocol.** The Xbox 360 route: recompiled titles there call XAudio2
   and the XMA decoder through the OS API, so the runtime implements the API and never runs the
   sound DSP. Dreamcast has no OS audio API; the equivalent seam is Manatee's command mailbox in
   sound RAM, which is undocumented and driver-version specific. Music replacement at track level
   becomes trivial; correctness across titles becomes per-driver work.

WP2.5 (10 days) implements option 1. Option 3 remains a possible Phase 4 enhancement layer for
music replacement once the mailbox traffic can be observed through the working LLE path.

---

## ADR 11: Disc access

**Context.** The original recommendation deferred CHD to Phase 4 because GDI parsing is trivial and
CHD needs a decompressor. The owner's library is mostly CHD, so that ordering would have put a
conversion step in front of every disc from day one.

**Recommendation (revised).** GDI and CHD are both first-class inputs from Phase 0, behind one
`DiscImage` interface in `tools/dcdisc` (Python) and later in the runtime (C++):

- CHD decoding uses **libchdr** (BSD-3-Clause), added as the `third_party/libchdr` submodule and
  built by CMake, or found as a system library. Only hunk decompression is delegated to it; the
  header and track metadata are parsed by our own code.
- **chdman** (MAME) is the fallback backend for the Python tooling when libchdr is not available,
  by extracting to a temporary GDI. The runtime will not depend on chdman.
- GDI remains the canonical dump format for hashes in the Redump sense, but the launcher verifies a
  disc by hashing extracted contents (IP.BIN, the boot binary, the filesystem manifest), so a GDI and
  a CHD of the same disc verify identically.
- GD-ROM syscall HLE (vectors 0x8C0000B0-BC) covers the ~80% of titles that use it; register-level
  GD-ROM emulation (0x005F7000) is added when the first title that needs it is attempted.
- 1ST_READ.BIN descrambling lives in the extraction tool, applied only when the literal-pool
  pointer heuristic says the stored binary is scrambled. GD-ROM dumps store it plain (verified on
  Crazy Taxi and THPS2, 2026-09-11); scrambling is a CD-R boot mechanism, so it matters only for
  CDI-style images.

**Verified 2026-09-10** with chdman 0.264 and libchdr 0.2 (Debian snapshot of commit 9108f34): both
backends reproduce a GDI byte for byte; chdman stores a GDI's pre-45000 gap as zero frames inside
the preceding track (FRAMES includes PAD), and stores CD audio big-endian.

## ADR 12: CPU correctness oracle

**Recommendation.** Two oracles. In CI: Flycast's interpreter core built as a library, run in lockstep
against recompiled KallistiOS test programs, comparing full `sh4_ctx` after every function return.
For ground truth: the same test programs run on a retail Dreamcast via dcload-ip or dcload-serial,
with results shipped back over the link and committed as golden files. The Tokyo Bus Guide decomp's
SH-4 object simulator and test framework is evaluated for direct reuse before writing a new harness.

**Phase 0 exit criterion (unchanged from the report):** bit-exact results on a full SH-4 instruction
suite and 20+ homebrew programs across GCC-built (KOS) and at least two retail binaries (SHC and
CodeWarrior idioms).

---

## ADR 13: Host platforms for v1

**Context.** The original recommendation was Windows x64 and Linux x64. The owner's development
machine is Apple Silicon, which makes macOS ARM64 a first-class platform whether or not it is a
distribution target, and means the project spans two host ISAs from the first commit.

**Recommendation (revised).** Three v1 platforms, all in CI: macOS ARM64 (GitHub Actions
`macos-latest` runners are Apple Silicon), Windows x64 (MSVC), Linux x64 (clang). Steam Deck is
covered by Linux x64. Linux ARM64 and Windows ARM64 are not targeted but nothing prevents them.

**What macOS costs.**

- Vulkan runs through MoltenVK, which the LunarG Vulkan SDK for macOS bundles. Flycast ships a macOS
  build with its Vulkan renderer over MoltenVK, so the path is proven, but MoltenVK lacks some
  features (no geometry shaders, limited fragment interlock). WP2.3 gains a two-day spike to confirm
  the OIT renderer's technique works under MoltenVK before the port starts. OpenGL is not an option:
  macOS caps at GL 4.1 and the OIT path needs 4.3.
- Distribution needs an app bundle, code signing and notarization; that is Phase 3.6 work, not a
  design constraint.
- The toolchain container runs as linux/arm64 under Docker Desktop; `sh-elf-gcc`, KallistiOS, Flycast's
  interpreter and Ghidra all build or run there.

**Consequence.** ADR 16 exists because of this decision.

## ADR 14: Repository layout and per-game config

**Recommendation.** Monorepo:

```
translator/   SH-4 decoder, analysis, C++ emitter (MIT)
runtime/      memory, scheduler, HLE, hardware cores (GPL-2.0)
tools/        disc extraction, unscramble, FID export, test orchestration (MIT)
games/<id>/   per-title TOML config, function list, hooks, no game data (GPL-2.0)
tests/        SH-4 instruction suite, KOS programs, golden files
docs/
```

Per-game configuration in TOML, following N64Recomp's precedent: disc hash, binary load addresses,
entry point, manually added function addresses, HLE overrides, hook points.

---

## ADR 15: Baseline commercial title

See `../baseline-game.md`. **Accepted 2026-09-10: Crazy Taxi** (Hitmaker, 2000) is the baseline, by
the owner's decision; they hold a legal copy as an 85 MB CHD. The analysis had recommended ChuChu
Rocket!. On 2026-09-11 `dcdisc shortlist` measured the owner's eight candidate dumps
(`../title-shortlist-measured.md`): Crazy Taxi has the smallest boot binary (1.4 MB) of any
single-code-file title and the smallest filesystem, confirming the choice. Alternates are now
**Charge 'N Blast** then **Tech Romancer**, both owned. Consequences: ADX streaming from the
GD-ROM is in scope for the Phase 2 runtime (WP2.6 +2 days) and the discovery loop is budgeted longer
(WP3.2 +2 days). Checklist steps 1 to 7 passed on the dump (`games/crazytaxi/checklist-report.md`); steps 8 and 9
(Flycast syscall trace and timing probe) and the Ghidra FID match rate remain.

---

## ADR 16: Cross-ISA portability and floating-point determinism

**Context.** With x86-64 and ARM64 hosts both in v1, the emitted C++ and the runtime must produce
identical guest-visible results on both. Integer code is not a concern. Floating-point is: ARM64
compilers contract `a*b+c` into fused multiply-add by default because the instruction is always
available, x86-64 builds without `-mfma` do not, and the results differ in the last bit. Denormal
flushing and rounding control also use different mechanisms (MXCSR FTZ/DAZ versus FPCR FZ; both
support `fesetround`). The SH-4's own `FMAC` must be emitted the same way on both hosts.

**Recommendation.**

1. **No ISA intrinsics in emitted code.** The emitter produces portable C++. `FIPR` and `FTRV` go
   through a small `runtime/simd.h` with NEON, SSE and scalar implementations that are tested
   against each other for bit-identical output.
2. **`-ffp-contract=off`** (and `/fp:precise` with contraction disabled on MSVC) on every translation
   unit of emitted code and on the FPU helper library, so the compiler never introduces or removes an
   FMA. `FMAC` is emitted as an explicit call whose implementation matches Flycast's interpreter, and
   the choice (fused `std::fma` or separate multiply and add) is recorded there once, not in the
   emitter.
3. **A `runtime/fenv` layer** that maps FPSCR.RM and FPSCR.DN to the host: `fesetround` for rounding,
   MXCSR FTZ|DAZ or FPCR FZ for denormals. Nothing else in the runtime touches host FP control
   registers.
4. **Golden traces are ISA-independent.** The Phase 1 differential harness records `sh4_ctx` traces
   from the Flycast interpreter; CI compares the recompiled output against them on macOS ARM64 and on
   Linux x64. A mismatch between hosts is a build failure, not a tolerance.
5. Host-specific code (fenv, simd, thread naming, path handling) lives behind those small headers.
   Everything else compiles unchanged on both ISAs.

**Rationale.** A recompiled game that behaves differently on a Mac and a PC is a support burden
forever and makes bug reports impossible to triage. The cost of these rules is small if adopted on
day one and large if retrofitted.

**Revision (2026-09-11, from the WP1.5 differential harness).**

- Item 2 gains **`-frounding-math`** (GCC/Clang) and **`/fp:strict`** (MSVC, replacing
  `/fp:precise`). The first divergence the harness found was not an FMA but constant folding:
  Clang evaluated a `1.0f / 3.0f` that the emitted code built from `fldi1`/`fadd`/`fdiv` at compile
  time under round-to-nearest, while the guest runs with FPSCR.RM = round-toward-zero
  (Katana's default 0x00040001) and Flycast's interpreter honours that. With the flag the compiler
  assumes a dynamic rounding mode and leaves inexact operations to run time.
- Item 1's `runtime/simd.h` is dropped for now. `FIPR` and `FTRV` are plain scalar code that
  promotes each product to double, accumulates left to right and rounds to single once, which is
  how Flycast evaluates them; products of two singles are exact in double, so the result is the
  same with or without contraction on any host and no per-ISA implementation is needed.
- `FSCA` is a table lookup with the coefficients Flycast captured from hardware
  (`runtime/src/sh4/fsca_coefs.inc`, GPL-2.0), not `sin`/`cos`: evaluating live under the guest's
  rounding mode gave 0x3F7FFFFF for sin(90°) where hardware gives exactly 1.0.
- Item 4 is in place: 78 Flycast-captured states (`tests/sh4/golden/`) are replayed by
  `dream_emit_tests` on macOS ARM64, Linux x86-64 and Windows x86-64 in CI.
- Known gap: NaN payloads. Flycast sets ARM64 FPCR.DN (default NaN) and nothing on x86-64, and the
  two ISAs' default NaNs differ in sign; neither side canonicalises to the SH-4's 0x7FBFFFFF. Left
  open until a game is shown to depend on NaN bit patterns.
