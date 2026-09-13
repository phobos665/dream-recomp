# Technical Feasibility Report: Static Recompilation of Sega Dreamcast Games

**Date:** 10 September 2026
**Scope:** Can Dreamcast titles be translated ahead-of-time from SH-4 machine code into native C/C++ and shipped as standalone PC executables (in the manner of N64Recomp / XenonRecomp), and what would it take?

---

## 1. Executive summary

**Verdict: feasible, and arguably the most tractable sixth-generation target for static recompilation.** Roughly 90% of the retail library (everything built on Sega's Katana SDK with the MMU disabled) is a good fit. The ~30 Windows CE titles are a separate, much harder problem and should be scoped out of a first-generation toolchain.

The case rests on five points:

1. **The CPU side is already proven on sibling hardware.** A public, BIOS-free, no-JIT static AOT recompilation of *Initial D Arcade Stage 3* (NAOMI 2, same SH-4 CPU and PowerVR2 GPU family) boots, runs full races at 60/120 Hz, saves to card, and plays audio through a native ARM7/AICA path. Its author reports zero untranslated SH-4 calls on complete race runs as of v2460–v2462 (September 2026). Dreamcast is a strict simplification of NAOMI 2 (one CLX2 instead of two, no ELAN geometry coprocessor, no JVS I/O board).
2. **The ISA is unusually friendly to lifting.** SH-4 uses fixed-width 16-bit instructions, is little-endian (no byte-swapping against x86/ARM hosts), has only 16 GPRs plus a 32-register single-precision FPU, no vector unit beyond a 4×4 matrix op, and no coprocessor microcode. Every other recomp target to date (MIPS N64, PowerPC GameCube/Wii/360, x86 Xbox) was harder on at least one of these axes.
3. **The GPU problem is orthogonal to recompilation and already solved.** The PowerVR2 Tile Accelerator is fed a well-documented display-list stream over the SH-4 store queues. Flycast/redream/Deecy/DEmul already parse that stream and render it with modern APIs (per-pixel OIT, modifier volumes, VQ textures, render-to-texture). A recompiler changes nothing about this pipeline; it only replaces the CPU that produces the stream.
4. **Documentation is excellent.** The Hitachi SH7750 hardware/software manuals are public from Renesas; Sega's Dreamcast hardware specification and Katana SDK reference material circulate widely; KallistiOS is a complete open-source implementation of every subsystem (PVR, AICA, Maple, GD-ROM, DMA, timers, flash); and Flycast (GPLv2) is a 15-year-lineage reference for every edge case.
5. **A large open-source test corpus exists.** Hundreds of KallistiOS homebrew titles compile with `sh-elf-gcc` and ship with source, giving you source + binary pairs for differential validation of the SH-4 → C translator before you touch a single commercial disc.

The principal risks are (a) function/code discovery in stripped, fixed-address binaries with runtime-loaded overlays, (b) correctly handling the SH-4 FPU's mode bits (FPSCR.PR/SZ/FR), (c) delivering interrupts to recompiled code cooperatively without a scheduler-aware kernel, and (d) GPL licensing if the runtime is built on Flycast's hardware cores. None of these is novel; each has an established solution in the N64/360 recomp community or the Initial D project.

---

## 2. Background: where the recomp ecosystem stands (Sept 2026)

The Recompendium catalog currently tracks **92 static recompilation projects across 10 platforms** (N64, SNES, GameCube, Wii, PS1, PS2, Xbox 360, GBA, DS, and Dreamcast-family). The pattern that emerged from Zelda64Recomp (2024) and was industrialised by XenonRecomp/ReXGlue (2025–26) is:

| Layer | Role | Examples |
|---|---|---|
| **Translator** | Disassemble → lift each instruction to C/C++ with an explicit guest CPU context struct | N64Recomp, XenonRecomp, psxrecomp, the Initial D `translator/` |
| **Runtime** | Guest memory, MMIO, interrupts, HLE of OS/BIOS/library calls, GPU/APU back-end | RT64 (N64), ReXGlue (360), snesrecomp, ps3recomp |
| **Game project** | Per-title config: function list, HLE overrides, hooks for widescreen/60fps/mods | Zelda64Recomp, UnleashedRecomp, skate3recomp |

The **only SH-4 entry** is the Initial D Arcade Stage 3 project (GDS-0033, NAOMI 2), classified "Experimental" with a public early demo. Its README describes exactly the architecture a Dreamcast recomp would use: general SH-4 instruction decoding and C++ generation in `translator/`, a native runtime with "Flycast-derived hardware knowledge", a Vulkan renderer for submitted PVR work, native ARM7/AICA audio, and no SH-4 interpreter or JIT fallback. There is no equivalent public Dreamcast console project yet; the field is open.

Separately, sp00nznet's *recompclass* course lists Dreamcast among its "Semester 2" hard targets alongside N64, GameCube, PS2 and Saturn, and Ned Heller reports private recomp work across 12 architectures including Dreamcast, so tooling knowledge exists in the community even if no repo is public.

---

## 3. Hardware analysis from a recompiler's point of view

### 3.1 CPU: Hitachi SH-4 (SH7750/SH7091 at 200 MHz)

| Property | Value | Recomp impact |
|---|---|---|
| Instruction width | Fixed 16-bit | **Very favourable.** Linear-sweep and recursive-descent disassembly are trivial; no variable-length ambiguity (contrast x86 Xbox). |
| Endianness | Little-endian (Dreamcast config) | **Very favourable.** Guest memory maps 1:1 onto host memory; no `bswap` on every load/store as on N64/GC/Wii/360. |
| GPRs | R0–R15, with R0–R7 banked (BANK0/BANK1 by SR.RB) | Bank switch only in privileged interrupt paths. Games run in privileged mode with MMU off; treat as a 16-register context struct plus an 8-register shadow bank swapped by the interrupt shim. |
| Delay slots | BRA, BSR, BRAF, BSRF, JMP, JSR, RTS, BT/S, BF/S | Identical problem to MIPS; lift the slot instruction before the branch effect. The Initial D v2461 checkpoint ("67 non-NOP instructions placed after an early C++ return instead of in the RTS delay slot") shows this is the classic silent bug to test for. |
| PC-relative loads | `MOV.L @(disp,PC),Rn` constant pools; `MOVA` | PC is statically known, so constants and **function pointers in constant pools are recoverable at translate time** — a major aid to code discovery. |
| Control-flow | `JSR @Rn`, `JMP @Rn`, `BRAF/BSRF Rn` | Indirect targets need a function table (address → native fn pointer), as in N64Recomp. Switch tables use `BRAF` with PC-relative tables — pattern-matchable. |
| Flags | Single T bit; MACH/MACL; GBR base register | Simple to model as struct fields. |
| Multiply/divide | `DMULS/U`, `MAC.W/L`, `DIV0S/U` + `DIV1` step division | Straightforward integer C; `DIV1` sequences are usually compiler-emitted idioms and can be pattern-collapsed for speed. |
| Store queues | SQ0/SQ1 at 0xE000_0000–0xE3FF_FFFF, flushed by `PREF` to the address in QACR0/1 | **Must be emulated in the runtime.** This is how virtually every game feeds the PVR TA. Cheap: two 32-byte buffers, `PREF` copies to target. |
| On-chip RAM | 8 KB operand-cache-as-RAM at 0x7C00_0000 (OC index mode) | Trivial fixed region. |
| MMU | Present; **disabled in Katana-SDK titles** (P1/P2 direct-mapped, address = mask) | For non-WinCE games address translation is a constant AND-mask. For WinCE games the TLB is live on every access — see §6. |
| Exceptions | `TRAPA`, illegal instruction, address error | Rarely relied upon by retail games except `TRAPA` for a few BIOS-style syscalls; handle by HLE. |

**The FPU is the one genuinely awkward part.** Its semantics are mode-dependent:

- `FPSCR.PR` selects single vs. double precision for `FADD/FSUB/FMUL/FDIV/FCMP/FSQRT/FLOAT/FTRC`.
- `FPSCR.SZ` selects 32-bit vs. 64-bit pair moves for `FMOV`.
- `FPSCR.FR` swaps the front (FR0–15) and back (XF0–15) register banks (`FRCHG`).
- `FPSCR.RM` sets rounding mode; `FPSCR.DN` controls denormal flushing.

A static translator cannot always know PR/SZ at a given instruction without data-flow analysis, because compilers set them with `LDS Rn,FPSCR` / `FSCHG` / `FPCHG`. Dynarecs (Flycast) solve this by specialising each block on the FPSCR state at entry and recompiling on mismatch. The static equivalents are:

1. **Per-function mode inference.** In practice, Katana/GCC/SHC code keeps PR=0, SZ=0 for the vast majority of functions and toggles them in small, self-contained double-precision or 64-bit-copy routines. A forward data-flow pass over the CFG resolves >99% statically.
2. **Mode-specialised clones** of any function whose mode cannot be proven, selected at call time (cheap, bounded blow-up).
3. **Runtime branch on the mode bit** as a last resort.

The SIMD-ish instructions `FIPR` (4-component dot product) and `FTRV` (4×4 matrix × vector) map cleanly to SSE/NEON. Hardware `FIPR/FTRV` use reduced internal precision and `FSRRA`/`FSCA` are approximations; matching them bit-exactly is only needed for games whose gameplay depends on it (e.g. replay determinism). Flycast's implementations are a ready reference. Host MXCSR/FPCR must be set to match FPSCR.RM/DN.

### 3.2 GPU: PowerVR2 CLX2 ("Holly") at 100 MHz, 8 MB VRAM

The CLX2 is a tile-based deferred renderer. The game does not draw; it **submits parameterised display lists** — global polygon/sprite headers followed by vertex parameters — into the Tile Accelerator FIFO (0x1000_0000) via store-queue bursts or DMA. The TA bins primitives into per-tile object lists in VRAM; the ISP/TSP later rasterises them when the game writes `STARTRENDER`, with perfect per-pixel translucency sorting, punch-through, modifier (shadow) volumes, fog table, palette/VQ/twiddled textures, and render-to-texture.

For a recompiler this is **excellent news**: the hardware/software boundary is a byte stream with fixed formats, exactly what emulators have HLE'd for two decades. The runtime needs:

- A TA parameter parser (Flycast's `ta_vtx.cpp` lineage; well understood, deterministic).
- A renderer for the resulting per-list geometry with order-independent transparency (Flycast offers per-triangle and per-pixel OIT on OpenGL/Vulkan/D3D11; redream and Deecy have their own).
- VRAM as a 8 MB byte array with the 32-bit (0x0400_0000) and 64-bit (0x0500_0000) access views, texture cache invalidation on CPU writes, YUV converter, and framebuffer-direct-write handling for 2D titles.
- SPG/VBlank timing, since nearly every game frame-locks on the VBlank interrupt or `SPG_STATUS`.

Enhancement hooks (internal resolution, widescreen via projection-matrix patching, texture replacement) work identically to how N64/360 recomps do them, and because the CPU is now native C you can also patch game logic for uncapped frame rates — something an emulator cannot do generically.

### 3.3 Audio: AICA (Yamaha) with ARM7DI core, 2 MB sound RAM

Games upload a sound driver (usually Sega's "Manatee" driver from the Katana SDK, occasionally a bespoke one) into sound RAM and talk to it through a mailbox. Three viable strategies, in order of pragmatism:

1. **Interpret the ARM7** (as every emulator does; it runs at ~22.6 MHz and is a rounding error on a modern host) plus emulate the 64 AICA channels and DSP. This is what the Initial D project appears to do ("native ARM7/AICA audio"). Reuses Flycast/Deecy code or a clean-room ARM7TDMI core.
2. **Statically recompile the ARM7 driver blob too.** Possible per title since the blob is fixed, but low value.
3. **HLE the Manatee driver** by recognising its mailbox protocol. Attractive later for modding (music replacement at the track level), risky first.

Recommendation: option 1 for correctness, with option 3 as an optional per-game layer.

### 3.4 Storage, I/O, and everything else

| Subsystem | How games use it | Runtime approach |
|---|---|---|
| **GD-ROM** | Mostly via BIOS syscall vectors at 0x8C00_00B0–BC (`gdGdcReqCmd` etc.); some titles poke the GD-ROM registers directly (0x005F_7000) | HLE the syscall table (this is redream's "HLE BIOS" and it covers ~80%+ of the library); implement register-level GD-ROM for the rest. Read from GDI/CHD/CDI images or an extracted filesystem. |
| **BIOS/IP.BIN** | IP.BIN bootstrap loads `1ST_READ.BIN` to 0x8C01_0000; games call ROM font and flash syscalls; some read the BIOS date/region | No BIOS required if syscalls are HLE'd (Initial D and redream both demonstrate BIOS-free operation). |
| **Maple bus** | Controllers, VMU (save + LCD), keyboard, mouse, light gun, fishing rod, microphone, Puru Puru rumble | Emulate the Maple frame protocol and DMA; map to SDL/XInput. VMU saves become files on disk. |
| **Timers/RTC/DMAC/SCIF** | TMU channels for game timing; DMAC ch2 for PVR/sort DMA; RTC for save timestamps | Simple, well documented in the SH7750 manual and KOS. |
| **Modem / BBA** | Online play in a few dozen titles | Stub for v1; Flycast's DCNet shows it can be done later. |

---

## 4. The recompilation pipeline for a Dreamcast title

```
GDI/CHD ──► extract IP.BIN + 1ST_READ.BIN (+ any additional .BIN overlays)
        ──► unscramble 1ST_READ.BIN (retail discs scramble the main binary)
        ──► code discovery ──► SH-4 → C/C++ lifting ──► native compile
                                        │
                                        ▼
                             link against runtime library
                             (memory, MMIO, PVR TA renderer, AICA,
                              Maple, GD-ROM HLE, BIOS syscalls, scheduler)
                                        │
                                        ▼
                             game executable + user-supplied disc image
```

### 4.1 Code discovery (the hard part)

Dreamcast binaries are stripped, statically linked, position-fixed images with no relocation or symbol tables. This is the same situation N64Recomp faced before decomp symbol maps existed. Recommended approach:

- **Seeds:** entry point (0x8C01_0000), IP.BIN bootstrap, BIOS syscall installers, interrupt vector installs (`LDC Rn,VBR` followed by handler tables), and every 4-byte constant-pool value that points into the code range and is followed by a `JSR`/`JMP`.
- **Recursive descent** from seeds with delay-slot-aware decoding, plus **switch-table recovery** for `BRAF`/`JMP @R0` idioms.
- **Katana SDK library signatures.** The Ninja (3D), Kamui (PVR), Shinobi (system), Manatee (sound) libraries appear in hundreds of titles as identical or near-identical machine code. A FLIRT-style signature database gives you function boundaries and names for a large fraction of any game, and enables **selective HLE** of hot library routines (matrix stack, `njDrawPolygon`, `kmxxx` TA submission) — the same trick UnleashedRecomp used for Xbox 360 runtime libraries.
- **Iterative closure.** Run, log any indirect jump to an untranslated address, add it, rebuild. The Initial D checkpoints ("audited every retained runtime fault log … added four genuine missing SH-4 targets") describe precisely this loop converging.
- **Overlays and runtime-loaded code.** Some titles load extra `.BIN` files or decompress code into RAM. Scan the whole disc for SH-4 code; translate every discovered image into the same function table keyed by load address. Games that generate code at runtime (rare; some WinCE titles, some compressed-executable packers) are the one case that requires a fallback interpreter or exclusion.

### 4.2 Translation model

Follow the established recomp pattern: one C function per guest function, a `struct sh4_ctx` holding R0–R15 (+bank), SR/T, GBR, VBR, MACH/MACL, PR, FPUL, FPSCR and both FP banks; memory access through inlineable helpers with a **fast path for the 16 MB main RAM** (mask, add base) and a slow path for VRAM/MMIO. Both Flycast and XenonRecomp reserve a large host virtual region with the guest mirrors mapped so that most loads/stores compile to a single instruction; the same trick applies here with the added benefit that no byte-swap is needed.

Let the host compiler do the optimisation: emit straightforward C for each instruction, keep guest registers in locals within a function (spilling to the ctx struct at calls and interrupt-check points), and rely on LLVM/MSVC to fold the T-bit and `DIV1` sequences. Initial D's runtime sustains 120 Hz presentation with ~0.2–1.2 host cores including the Vulkan renderer, which suggests the SH-4 portion is essentially free on modern hardware.

### 4.3 Interrupts and scheduling

Dreamcast games are interrupt-driven (VBlank, TA list-complete, render-done, DMA-done, Maple, GD-ROM, TMU) but almost none run a preemptive OS; the Katana SDK installs handlers that set flags the main loop polls. The proven cooperative strategy:

- Insert an `if (ctx->irq_pending) deliver_irqs(ctx);` check at function entry and at loop back-edges (cheap, predictable branch).
- Raise the runtime's pending flags from a virtual clock advanced by the memory-mapped register accesses and by instruction-count estimates per basic block.
- Detect **idle loops** (a basic block that only reads an MMIO register/RAM flag and branches to itself) and have the runtime fast-forward the virtual clock to the next event, as every emulator does.
- Deliver an interrupt by saving the ctx, switching R0–R7 to BANK1, setting SR.BL/RB/MD, and calling the recompiled handler at `VBR + 0x600`; unwind on `RTE`.

Timing is coarse compared with a cycle-counting emulator, but retail games almost universally synchronise on VBlank rather than cycle counts, and the NAOMI 2 project demonstrates 60 Hz-cadence gameplay, physics, timers and audio staying locked under this model.

---

## 5. Validation strategy using the open-source corpus

This is where the Dreamcast's homebrew scene becomes a real asset rather than a talking point:

1. **Differential CPU testing.** Build a suite of KallistiOS programs (and the existing SH-4 conformance tests used by emulator authors) with `sh-elf-gcc`, run the recompiled output against Flycast's interpreter core and against the original ELF's expected output. Because you have source, every mismatch localises to a function immediately. The Tokyo Bus Guide decompilation team has already built a "custom SH4 object simulator and testing framework" for function-level equivalence; that is directly reusable.
2. **Compiler diversity.** Retail games were built with Hitachi SHC, GNU, and Metrowerks CodeWarrior. Homebrew is GCC-only, so also translate a handful of retail binaries early to catch SHC/CodeWarrior idioms (different prologues, `DIV1` unrolls, FPSCR handling).
3. **Full-system tests on open-source games.** KOS ports with source (e.g. the many Doom/Quake-engine ports, the 2025–26 *Wipeout Rewrite* Dreamcast port, DCA3) exercise PVR, AICA, Maple and GD-ROM through KOS's drivers rather than Katana's, giving a second, independent code path through every hardware block.
4. **Regression ledgers.** Adopt the Initial D project's practice of hash-locked, per-build acceptance matrices (route × condition × result) — tedious but it is what turned "boots" into "playable".

---

## 6. Risk register

| Risk | Severity | Likelihood | Mitigation |
|---|---|---|---|
| Function discovery misses code reached only via data-driven pointers | High | Certain (per title) | Constant-pool pointer harvesting; SDK signatures; log-and-iterate closure; optional interpreter fallback during development only |
| FPSCR mode mis-inference silently corrupts math | High | Medium | Data-flow inference + specialised clones; differential tests against interpreter with FPSCR fuzzing |
| Delay-slot / `RTS` mis-translation | High | Medium | Dedicated unit tests for every branch form; the v2461 Initial D bug is the template |
| Interrupt timing too coarse for a specific game (raster effects, tight VBlank races) | Medium | Low–Medium | Per-title tuning of check points; fall back to finer-grained clock advance in hot loops |
| Runtime-generated or self-modifying code | High | Low (Katana titles) | Exclude or hybrid-interpret; document per title |
| **Windows CE titles** (Half-Life, Sega Rally 2, RE2/3, Tomb Raider, Armada, Hidden & Dangerous, etc.) | Very high | Certain for those titles | **Out of scope for v1.** WinCE runs with the MMU on; every load/store goes through the TLB, and the kernel does page-fault-driven loading. Flycast needed a separate, slower "WinCE" build to get these working at all. A recomp would either need TLB emulation on every access (destroying the performance case) or a Win32-API-level HLE of CE — a different project. |
| `FIPR/FTRV/FSRRA/FSCA` precision differences | Low | Medium | Reuse Flycast's reference implementations; accept non-bit-exactness except where replays desync |
| Licensing | Medium | Certain if reusing Flycast | Flycast is GPLv2; a runtime linking its PVR/AICA cores must be GPL. Alternatives: clean-room from the Sega/Renesas docs + KOS (BSD-style), or Deecy (Zig, open source — check license). N64Recomp's RT64 is MIT, which is why that ecosystem could pick permissive licences. |
| Legal exposure | Medium | Ongoing | Ship code only, never assets or BIOS; require user-dumped discs; follow the N64/360 community's "no game data in repo" norms exactly as Initial D's LEGAL.md does |

---

## 7. Comparison with other recomp targets

| | N64 (N64Recomp) | Xbox 360 (XenonRecomp) | GameCube/Wii | **Dreamcast** |
|---|---|---|---|---|
| ISA | MIPS III, 32-bit fixed, big-endian | PowerPC 64 + VMX128, big-endian | PowerPC 32 + paired singles, big-endian | **SH-4, 16-bit fixed, little-endian** |
| FP complexity | Simple | Very high (VMX128) | Medium (paired singles) | Medium (mode bits), simple vectors |
| Coprocessor problem | RSP microcode (needs HLE per ucode) | None (GPU is D3D-ish) | None (GX is a fixed pipeline) | **None** — TA byte stream, fully HLE'd already |
| Symbol availability | Good (decomps) | Some (XEX imports) | Good (decomps) | Poor — signatures needed |
| OS/kernel | libultra threads | Xbox kernel (large HLE surface) | Minimal SDK | **Minimal SDK; small BIOS syscall surface** |
| Existing AOT proof | Yes (many) | Yes (many) | Partial | **Yes, on NAOMI 2 (arcade sibling)** |

Net: Dreamcast sits on the easier side of every row except symbol availability.

---

## 8. Recommended plan

**Phase 0 — Translator core (4–8 weeks, 1–2 engineers)**
SH-4 decoder (reuse Flycast's or robercano's opcode tables), C emitter, ctx struct, delay-slot handling, FPSCR data-flow pass. Differential test harness against Flycast's interpreter on KOS test programs. Exit criterion: bit-exact results on the full SH-4 instruction test suite and 20+ homebrew programs.

**Phase 1 — Minimal runtime (6–10 weeks)**
Memory map with fast RAM path, store queues, TMU/SPG/interrupt delivery, Maple (controller only), TA parser + a basic OIT renderer, AICA via ARM7 interpreter, GD-ROM syscall HLE. Exit criterion: three KOS homebrew games playable start to finish with audio.

**Phase 2 — First commercial title (8–12 weeks)**
Pick a Katana-SDK, non-WinCE, single-binary title with modest code size (a fighter or shmup is ideal). Build the SDK signature database from it. Iterate discovery to zero untranslated calls. Exit criterion: playable start to finish, VMU saves working.

**Phase 3 — Generalise and enhance**
Config-driven per-game projects (TOML like N64Recomp), launcher with disc hash check, internal resolution/widescreen/uncapped-fps hooks, mod API. Decide licensing model (GPL runtime on Flycast cores vs. clean-room permissive) before this phase, not after.

**Explicitly deferred:** WinCE titles, modem/BBA networking, light-gun/fishing-rod/microphone peripherals beyond stubs, bit-exact `FTRV` precision.

---

## 9. Conclusion

Static recompilation of Dreamcast games is technically feasible today. The machine is thoroughly documented, its CPU is one of the friendliest instruction sets ever put in a console for lifting to C, its GPU is driven by a byte-stream protocol that mature open-source emulators already render, and a working SH-4 ahead-of-time recompilation already exists for the arcade sibling hardware. The remaining work is engineering, not research: robust code discovery in stripped binaries, careful FPU mode handling, a cooperative interrupt model, and a hardware runtime that can be assembled largely from existing open-source components. The strategic decision that most shapes the project is licensing — whether to build the runtime on Flycast (fast, GPLv2) or clean-room it from the Sega/Renesas documentation and KallistiOS (slower, permissive) — and it should be made before the first line of runtime code is written.

---

## Sources

- Initial D Arcade Stage 3 — Native SH-4 Recompilation (NAOMI 2): https://github.com/distilledorion-sketch/Initial-D-Arcade-Stage-3-Native-SH-4-Recompilation-WIP-
- Recompendium — catalogue of static recompilation projects: https://nio03.github.io/unricopie/en/
- sp00nznet, *Static Recompilation: From Theory to Practice* (recompclass): https://github.com/sp00nznet/recompclass
- Flycast (SH-4 dynarec, PVR2, AICA, GD-ROM reference implementation, GPLv2): https://github.com/flyinghead/flycast
- Flycast WinCE core release notes (why full-MMU titles are hard): https://www.libretro.com/index.php/flycast-wince-libretro-experimental-core-released/
- flycast-wasm technical write-up (SHIL IR, SH-4 → WASM JIT): https://github.com/nasomers/flycast-wasm
- redream (HLE BIOS, closed source): https://redream.io / https://gitlab.com/inolen/redream
- Tokyo Bus Guide decompilation (SH-4 function-level test framework): https://github.com/lhsazevedo/tokyo-bus-guide-decomp
- robercano/dreamcast (SH7750 opcode tables, disassembler): https://github.com/robercano/dreamcast
- Emulation General Wiki — Dreamcast emulators and NAOMI variants: https://emulation.gametechwiki.com/index.php/Sega_Dreamcast_emulators
- RetroReversing — Dreamcast reverse-engineering resources: https://www.retroreversing.com/dreamcast
- Sega Retro — list of Windows CE titles: https://segaretro.org/Windows_CE
- KallistiOS (open-source Dreamcast SDK): https://github.com/KallistiOS/KallistiOS
- Renesas SH7750 Series Hardware Manual / SH-4 Software Manual (public)
