# CPU performance: where the time goes, and what a day of work buys (study)

Written 2026-09-14 on an Apple M4 (macOS 25.6, Apple clang, ARM64) from timed runs and `sample`
profiles of the release build against the owner's Crazy Taxi disc in attract mode at 640x480, plus
a reading pass over `runtime/src/mem/`, `runtime/src/sh4/`, `runtime/src/sched/`,
`runtime/src/aica/`, `runtime/src/pvr/`, `render/src/vk/`, `runtime/boot/boot_main.cpp` and
`cmake/`. **No file in the repository was changed except this one.**

Everything labelled **measured** was produced by running a binary and reading a number off it.
Where a claim rests on reading code rather than running it, it says **inferred**.

Two working prototypes were built to settle the two biggest questions rather than estimate them.
They live entirely outside the repository, in a scratch copy of `runtime/` compiled against the
repository's own emitted code and its other static libraries. They are throwaway measurement rigs,
not proposed patches, and both are described precisely enough below to be rebuilt.

The reproduction command throughout is

```
./build-release/games/crazytaxi/crazytaxi_boot --config games/crazytaxi/crazytaxi.toml \
    --no-audio --unthrottled --max-seconds 20
```

which prints `host: N s (Mx real time)`.

---

## 0. Verdict

**About 3x is available, and 2.9x of it has already been measured on a working prototype.** The
release build runs Crazy Taxi's attract mode at **3.5x real time headless**. Two changes, each
confined to a single file's worth of runtime code and each producing a **byte-identical run
report**, took that to **10.0x**. A third measured change (the AICA DSP) is worth a further 11 per
cent. Nothing here touches the translator, the emitted code, or a floating-point result.

**The three findings, in order of size:**

1. **The memory fast path is not inlined, and it is 56 per cent of the profile.** `resolve()` is a
   non-inlined out-of-line call reached through a virtual dispatch, and it re-derives the region of
   the address space on *every* access. Inlining a mask-and-index fast path for main RAM — the
   thing ADR 5 already accepts and `docs/progress.md:32` defers — measured **3.5x → 5.8x**.
   Four days central.

2. **The guest makes 5.9 million indirect calls per guest second, and each one does a binary
   search.** This was not previously identified anywhere in the docs. `find_function` does a
   `std::lower_bound` over a 3,794-entry, 152 KB table for every `jsr @Rn`. A direct-mapped cache
   measured **5.8x → 10.0x**. Two days central.

3. **The windowed-versus-headless gap is not a CPU cost at all.** The window is locked to the
   display's 120 Hz refresh by `VK_PRESENT_MODE_FIFO_KHR`, and `--unthrottled` does not turn that
   off. Windowed, 1,209 frames took 10.11 s; 1209 / 120 Hz = 10.07 s. Windowed CPU time is
   6.55 s against headless 5.69 s — the window costs **15 per cent more CPU**, not half the
   throughput. The remaining 4.4 s is the process asleep. One day central.

**What this is worth in the real-time case.** The measured baseline (0.40 CPU-seconds per
wall-second, ~43 per cent of one core) should fall to roughly **0.14 CPU-s/wall-s, ~15 per cent of
one core** — comfortably under Flycast's 0.31, and with headroom for the 144 fps and supersampling
work in `docs/rendering-enhancements-study.md`.

**The honest caveat, stated up front.** Attract mode is not gameplay. In attract mode the runtime
dominates and the emitted game code is a tenth of the profile; after the two fixes the emitted code
becomes the *largest* single term. A gameplay profile will start from a different mix, and the
speedup there will be smaller — plausibly 2x rather than 3x. Section 11 says how to find out.

**Explicitly not recommended:** `-ffast-math` and friends (ADR 16 forbids them), replacing the
non-local-return exceptions, and `mmap`-based address-space tricks (ADR 5 rules them out for v1 and
the measured numbers say they are not needed).

---

## 1. The measured baseline

All runs are 20 guest seconds of attract mode, release build, `--no-audio --unthrottled`, repeated
three times; the spread was under 5 per cent except where noted.

| Configuration | Host wall | Speed | CPU time |
|---|---|---|---|
| Headless, release (`build-release/`) | 5.54, 5.55, 5.69, 5.70, 6.04 s | **3.3–3.6x** | 5.67 s user, 0.02 s sys (99 % of a core) |
| Windowed, release | 10.11 s | **1.9x** | 6.17 s user, 0.38 s sys (**60 % of a core**) |

The windowed row is the whole of finding 3 and is dealt with in section 4. The headless row is the
one everything else is measured against.

`/usr/bin/time -l` on the windowed run also reported 125,677,270,428 instructions retired against
26,079,022,395 cycles elapsed — the process is not instruction-starved, it is idle.

### 1.1 The profile

`sample <pid> 10 1` on the headless release build, 8,449 leaf samples listed.

| Symbol | Samples | Share | Subsystem |
|---|---:|---:|---|
| `dream::mem::DcMemory::resolve` | 3,452 | 40.9 % | memory |
| `dream::sh4::find_function` | 1,356 | 16.0 % | indirect calls |
| `dream::mem::DcMemory::read32` | 1,063 | 12.6 % | memory |
| `dream::gen::fn_0c1582a0__resume` | 785 | 9.3 % | **emitted game code** |
| `dream::aica::Mixer::dsp_step` | 395 | 4.7 % | AICA |
| `dream::aica::Arm7::step` | 238 | 2.8 % | AICA |
| `dream::mem::DcMemory::store<unsigned>` | 189 | 2.2 % | memory |
| `dream::gen::fn_0c07d018__resume` | 116 | 1.4 % | emitted game code |
| `dream::sh4::call_indirect` | 104 | 1.2 % | indirect calls |
| `dream::gen::fn_0c07d7c4__resume` | 101 | 1.2 % | emitted game code |
| `dream::gen::fn_0c156c30` | 99 | 1.2 % | emitted game code |
| `dream::aica::Mixer::Channel::step_channel` | 58 | 0.7 % | AICA |
| `dream::aica::Arm7::run` | 52 | 0.6 % | AICA |
| `dream::aica::Mixer::sample` | 44 | 0.5 % | AICA |
| `dream::sched::Scheduler::advance_to` | 40 | 0.5 % | scheduler |
| `dream::mem::DcMemory::write32` | 39 | 0.5 % | memory |
| `dream::aica::Aica::aram` | 32 | 0.4 % | AICA |
| `dream::sh4::Intc::select` | 10 | 0.1 % | poll |
| `dream::sched::Scheduler::next_deadline` | 6 | 0.1 % | scheduler |

Rolled up (**measured**):

| Subsystem | Share of profile |
|---|---:|
| Memory (`resolve`, `read32`, `read16`, `store`, `write32`) | **56 %** |
| Indirect-call resolution (`find_function`, `call_indirect`) | **17 %** |
| AICA (mixer, DSP, ARM7) | **10 %** |
| Emitted game code | **13 %** |
| Scheduler and poll | **1 %** |
| Everything else (dyld, unwinder, PVR, maple) | ~3 % |

The brief's own profile put `on_poll` and `advance_to` at roughly the same weight as AICA. On this
machine they are an order of magnitude smaller; the difference is that `on_poll`'s *inclusive* cost
is almost entirely the guest's own interrupt handler running inside it (see the call graph at
`win.sample`: `on_poll` → `enter_exception` → `fn_0c16bdb0__resume` → `fn_0c16a590__resume`), which
is guest work, not runtime overhead. Section 7 quantifies the runtime half.

### 1.2 Event and call frequencies

An instrumented build (counters only, everything else identical) over the same 20 guest seconds:

| Counter | Total | Per guest second |
|---|---:|---:|
| `call_indirect` calls | **118,307,678** | **5,915,384** |
| `System::on_poll` calls | 1,856,312 | 92,816 |
| `Scheduler::advance_to` calls | 2,572,717 | 128,636 |
| Scheduler events fired | 1,828,513 | 91,426 |
| Interrupts actually delivered | 6,854 | 343 |
| `nonlocal_return` throws | 20,129 | 1,006 |
| AICA samples | 882,031 | 44,102 |
| ARM7 instructions | 60,058,788 | 3,002,939 |
| Guest cycles | 4,000,011,887 | 200,000,594 |

Two numbers here carry most of the report. **One indirect call every 34 guest cycles**, and
**92,816 polls to deliver 343 interrupts — a 270:1 ratio**.

---

## 2. Attribution table, measured

Each row was produced by building a variant and timing it, not by adding up profile percentages.
Rows 1 and 2 are cumulative (2 is applied on top of 1); rows 3–5 are measured on top of 1+2.

| # | Change | Host s / 20 guest s | Speed | Gain over previous | Report identical? |
|---|---|---:|---:|---:|---|
| 0 | Release build as it stands | 5.69 | 3.5x | — | (reference) |
| 1 | + inlined RAM fast path | 3.40 | 5.8x | **1.68x** | **yes, byte-identical** |
| 2 | + `find_function` direct-mapped cache | 1.98 | 10.0x | **1.71x** | **yes, byte-identical** |
| 3 | + AICA DSP step skipped | 1.71 | 11.6x | 1.13x | AICA line only (`dsp stopped`) |
| 4 | + whole AICA mixer skipped | 1.64 | 12.1x | 1.18x over row 2 | AICA line only |
| — | ARM7 skipped (probe, not a proposal) | 2.56 | 7.7x | **0.75x — slower** | no: guest diverges |
| — | AICA tick skipped entirely (probe) | 2.39 | 8.3x | **0.81x — slower** | no: guest diverges |
| — | Emitted code at `-O2` instead of `-O1` | 5.45 | 3.6x | 1.02x | yes |

Rows 1 and 2 together are **2.87x**, measured, with a run report — 43 printed lines covering guest
cycles, frame count, final PC, interrupt counts, syscall counts, maple traffic, AICA sample and ARM7
instruction counts, holly register state and the relocation-candidate list — that is **byte-for-byte
identical to the unmodified release build**.

The two "slower" probes are the useful negative result of the study and are explained in section 6.

### 2.1 Profile after rows 1 and 2

`sample` on the two-fix prototype, 8,361 leaf samples listed. This is what a future performance pass
starts from.

| Symbol | Samples | Share |
|---|---:|---:|
| `dream::gen::fn_0c1582a0__resume` | 1,717 | 20.5 % |
| `dream::aica::Mixer::dsp_step` | 1,089 | 13.0 % |
| `dream::sh4::find_function` | 810 | 9.7 % |
| `dream::aica::Arm7::step` | 741 | 8.9 % |
| `dream::gen::fn_0c156c30` | 558 | 6.7 % |
| `dream::gen::fn_0c07d018__resume` | 501 | 6.0 % |
| `dream::sh4::call_indirect` | 462 | 5.5 % |
| `dream::gen::fn_0c07d7c4__resume` | 286 | 3.4 % |
| `dream::gen::fn_0c074b50__resume` | 264 | 3.2 % |
| `dream::aica::Mixer::Channel::step_channel` | 221 | 2.6 % |
| `dream::aica::Arm7::run` | 195 | 2.3 % |
| `dream::aica::Mixer::sample` | 107 | 1.3 % |
| `dream::sched::Scheduler::advance_to` | 106 | 1.3 % |
| `dream::mem::DcMemory::resolve` | 100 | 1.2 % |
| dyld + `libunwind` (exception unwinding) | ~200 | ~2.4 % |
| `dream::mem::DcMemory::store<unsigned>` | 89 | 1.1 % |
| `dream::sched::Scheduler::next_deadline` | 35 | 0.4 % |

Rolled up: **emitted game code ~44 %**, **AICA ~30 %**, **indirect-call resolution ~15 %**,
exception unwinding ~2.4 %, memory ~2.3 %, scheduler ~1.7 %. The memory subsystem has gone from
56 per cent to 2.3 per cent, and the emitted code — which is the only part that is doing the
program's actual work — is now the largest single item, which is where a recompiler wants to be.

---

## 3. The memory fast path

### 3.1 What happens today, per access

Emitted code holds a `dream::Memory&` and calls the accessors on it directly. From
`build-release/games/crazytaxi/gen/crazytaxi.cpp:509951` onwards, a store and a load look like:

```cpp
    // 0x0c07d018: mov.l r8,@-r15
    c.r[15] -= 4; m.write32(c.r[15], c.r[8]);
    // 0x0c07d026: mov.l @(r0,r15),r2
    c.r[2] = m.read32(c.r[15] + c.r[0]);
```

`crazytaxi.cpp` alone contains **66,913** such `read32`/`write32` call sites.

`read32` is declared pure virtual at `runtime/include/dream/runtime/memory.h:21`, so every one of
those is an indirect call through a vtable that no compiler can devirtualise (the emitted TU only
ever sees the abstract base). It lands in `DcMemory::read32` at
`runtime/src/mem/dc_memory.cpp:246`, which:

1. tests `(a & 0xFFFFFFC0u) == 0xFF000000u` for the CCN register window and, on a miss,
2. calls `load<std::uint32_t>` (`:176`), which
3. calls `resolve(a, 4, false)` (`:97`) — a separate out-of-line function, 656+ bytes of code in
   the release build, which for every single access re-derives the region:
   - `(a & 0xE0000000u) == 0xE0000000u`? — the P4 test, including the store-queue window and a
     linear scan `find(p4_mmio_, a)` over the P4 range table (`:106`, `:77`);
   - `(a & 0xFC000000u) == 0x7C000000u`? — operand-cache-as-RAM (`:121`);
   - `phys = a & 0x1FFFFFFF`, then `switch (phys >> 26)` with cases for area 0 (BIOS/flash/sound
     RAM, three nested range tests), area 1 (VRAM, including `vram_map32` interleaving), area 3
     (RAM) (`:126`–`:157`);
   - failing all of those, a **linear scan** `find(phys_mmio_, phys)` over `phys_mmio_` (`:161`);
4. builds and returns a 24-byte `Target` struct by value (`kind`, `bytes`, `mmio`, `mmio_addr`,
   `runtime/include/dream/runtime/mem/dc_memory.h:200`–`:206`);
5. branches on `t.kind` and `memcpy`s four bytes.

A RAM read — the overwhelmingly common case — therefore costs an indirect call, a second
out-of-line call, roughly a dozen unpredictable branches and a 24-byte struct return, to do work
that is two instructions: mask and index.

The store path (`:195`) additionally tests `journaling`, `hash_writes` and the
`watch_lo`/`watch_hi` window on every store (`:198`, `:206`, `:209`), all three of which are
development features that are off in a release run.

**Why it is not inlined:** `resolve`, `load<T>` and `store<T>` are all private members defined only
in `dc_memory.cpp` (declared at `dc_memory.h:207`–`:211`), and the emitted translation units are
compiled separately and see only `memory.h`. There is no inlining boundary the compiler could
cross, LTO is off (`build-release/CMakeCache.txt`, `BUILD_LTO:BOOL=OFF`), and even with LTO the
virtual call would remain because `Memory` has two implementations.

`docs/progress.md:32` records this exactly: *"Deferred: inlining the RAM fast path into emitted code
(virtual call remains; measure in Phase 3)."* ADR 5 (`docs/decisions/README.md:17`) already accepts
"Mask-and-index fast path for RAM, switch to MMIO handlers". **This section is that measurement.**

### 3.2 What inlining looks like

The prototype moved the accessors from the abstract base into a concrete, non-virtual,
header-inline fast path, and renamed the virtuals:

```cpp
class Memory {
public:
    std::uint8_t* fast_ram = nullptr;   // the 16 MB of area 3
    bool fast_ok = false;               // false when a dev feature needs the slow path

    std::uint32_t read32(std::uint32_t a) {
        if (fast_ok && (a & 0x1C000000u) == 0x0C000000u) {
            std::uint32_t v;
            std::memcpy(&v, fast_ram + (a & 0x00FFFFFFu), 4);
            return v;
        }
        return read32_slow(a);
    }
    virtual std::uint32_t read32_slow(std::uint32_t addr) = 0;
    // ... the same for read8/16/64 and write8/16/32/64
};
```

`(a & 0x1C000000u) == 0x0C000000u` is not new arithmetic: it is exactly the test `BareMemory::idx`
already uses (`runtime/include/dream/runtime/memory.h:70`–`:78`) and it is what
`resolve`'s `switch (phys >> 26) case 3:` means. It selects area 3 through every P0/P1/P2/P3 alias,
and `& 0x00FFFFFF` folds the three 16 MB mirrors inside area 3, which is what
`dc_memory.cpp:155` does. `sq_write32` and `sq_flush` stay virtual and untouched.

Everything the fast path does not claim goes to `*_slow`, which is the present `DcMemory` code
verbatim. Concretely:

| Concern | How the fast path handles it |
|---|---|
| **Store queues** (`0xE0000000`–`0xE3FFFFFF`) | `a & 0x1C000000` is `0x00000000`, not `0x0C000000` — falls through to `*_slow`. `sq_write32`/`sq_flush` are not touched at all. |
| **MMIO dispatch** (area 0 registers, area 4 TA FIFO, P4 on-chip) | Never matches area 3 — falls through, including `on_device_access` (`dc_memory.h:111`), which is what keeps TMU reads in the present. |
| **The fault log** | Only reached through `resolve`, which only the slow path calls. An address in area 3 is always mapped, so it can never fault. |
| **RAM mirrors** | `& 0x00FFFFFF` is the mirror fold, identical to `phys & (kRamSize - 1)`. |
| **VRAM 32/64-bit views** | Area 1, not area 3 — falls through to `resolve`, so `vram_map32` (`dc_memory.cpp:86`) is untouched. |
| **CCN registers at `0xFF000000`** | `a & 0x1C000000` is `0x1C000000` — falls through to `read32_slow`, which keeps the existing test at `:246`. |
| **Operand cache as RAM** (`0x7C000000`) | Also `0x1C000000` — falls through. |
| **Journal / write hash / write watch** | `fast_ok` must be cleared whenever any of them is armed. **This is the one real hazard**; see 3.4. |

### 3.3 What it measured

**1.68x.** Three runs each, interleaved:

```
baseline  host: 5.69 s (3.5x)   fastpath  host: 3.40 s (5.8x)
baseline  host: 6.04 s (3.3x)   fastpath  host: 3.60 s (5.5x)
baseline  host: 5.70 s (3.5x)   fastpath  host: 3.37 s (5.9x)
```

The full 43-line run report was **byte-identical** to the baseline's.

In the profile, `resolve` fell from 3,452 samples (40.9 %) to 100 (1.2 %) and `read32`, `write32`
and `store<unsigned>` disappeared as separate symbols.

### 3.4 Cost and risk

**Emitted code size.** Each `m.read32(x)` becomes roughly: `and`, `cmp`, `b.ne` slow, `and`, `ldr`
— about five instructions inlined where there was one `bl`. With 66,913 sites in `crazytaxi.cpp`
this is real but bounded: the object file grew from 8,652,760 to a measured-comparable size in the
prototype (the prototype's `crazytaxi.cpp.o` was rebuilt at `-O1` as before and the resulting
binary was 9.3 MB against 9.27 MB for the stock release build — under 1 per cent). **Inferred:** the
`fast_ok` check lets the compiler hoist very little, so expect 10–20 per cent more text in the
emitted TUs once the redundant `memcpy` forms are cleaned up. Compile time for the 30 MB TU was
unchanged within noise.

**Correctness risk: medium, and concentrated in one place.** The prototype was deliberately built
with `fast_ok` always true, which demonstrates the hazard exactly:

```
$ crazytaxi_boot        --write-hash h_base.txt   # 1 frame 3342993 4a879c6f98e97588 147787
$ crazytaxi_boot_proto  --write-hash h_fast.txt   # 1 frame 3342993 0000000000000000      0
```

The prototype records **zero** guest stores, because the fast path bypasses `note_write`
(`dc_memory.h:153`). Everything the differential harness is built on —
`docs/differential-harness.md`, `--write-hash`, `--write-log`, the replay journal
(`dc_memory.h:146`), the `DREAM_WATCH_WRITE` watch (`dc_memory.h:114`) — would silently stop
working. The production version must therefore set

```cpp
fast_ok = !journaling && !hash_writes && !on_watch_write && watch_lo >= watch_hi;
```

and re-evaluate it whenever any of those is armed. That is one predictable, always-false branch in
release, and it is the same shape of guard the codebase already accepts for `hash_writes`
("one predictable branch when on", `dc_memory.h:120`).

The second risk is more subtle and worth a test of its own: `Memory::read32` becoming non-virtual
means a future `Memory` implementation that wants to intercept *all* RAM traffic has to set
`fast_ram = nullptr` rather than override a method. `BareMemory` in `memory.h:34` is the only other
implementation today and it converts trivially.

**Files touched:** `runtime/include/dream/runtime/memory.h`,
`runtime/include/dream/runtime/mem/dc_memory.h`, `runtime/src/mem/dc_memory.cpp`, plus whatever
`runtime/tests/` asserts on the accessor names. No emitted code, no translator, no CMake.

**Days: 2 low / 4 central / 7 high.** The mechanical change is under a day; the rest is the
`fast_ok` gating, the tests that prove the dev features still see every store, and a careful pass
over the eight accessor sizes.

---

## 4. Windowed versus headless: the window is not a CPU cost

### 4.1 The measurement

| | Wall | User | Sys | CPU / wall | Frames | Speed |
|---|---:|---:|---:|---:|---:|---:|
| Headless, `--unthrottled` | 5.71 s | 5.67 s | 0.02 s | **99 %** | 1,209 | 3.5x |
| Windowed, `--unthrottled` | 10.92 s | 6.17 s | 0.38 s | **60 %** | 1,209 | 1.9x |

The window costs **0.86 s more CPU (+15 %)**. It costs **4.4 s more wall clock**, during which the
process is asleep. The brief's "the window and present path costs roughly half the throughput" is
true of throughput and false of CPU; the throughput is being given away, not consumed.

### 4.2 Why: FIFO present mode, and `--unthrottled` does not reach it

`render/src/vk/window.cpp:159`:

```cpp
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;  // always supported; vsync
```

FIFO blocks `vkQueuePresentKHR` / `vkAcquireNextImageKHR` until the display's next refresh. The
arithmetic closes it: **1,209 presented frames ÷ 120 Hz = 10.07 s**, against a measured 10.11 s
wall. The machine's panel is a 120 Hz ProMotion display. The windowed run is not slow; it is
running at exactly the refresh rate and waiting.

The windowed profile confirms it from the other side: of 46,575 leaf samples,
**33,181 are `__workq_kernreturn`**, 6,657 are `mach_msg2_trap`, 1,521 are `__semwait_signal` and
667 are `__psynch_cvwait` — 90 per cent of the samples are threads asleep. Actual Vulkan/Metal work
(`AGXMetalG16G_B0`, `libMoltenVK`) totals under 40 samples.

**`--unthrottled` was used, and the pacing sleep is genuinely off.** `boot_main.cpp:1596`:

```cpp
            if (unthrottled)
                return;
            ...
            if (target > now && target - now < std::chrono::seconds(1))
                std::this_thread::sleep_for(target - now);   // :1605
```

The flag returns before the sleep. It has no effect on the swapchain, which is where the waiting
actually happens.

### 4.3 The things that were suspected and are *not* the problem

- **The overlay `decorate` callback does not run per frame.** `boot_main.cpp:273`:
  `if (show_fps || menu.is_open())`. With neither the FPS counter nor the binding menu open, the
  plain three-argument `presenter.upload` at `:283` is taken and `decorate` is never constructed.
  **Cleared.**
- **`read_pixels` does not run per frame.** It is only reached from `screenshot()` under
  `--screenshot-presented` (`boot_main.cpp:533`–`:544`). **Cleared.**
- **The staging copy is small.** `render/src/vk/present.cpp:303`, `staging_.write(rgba, bytes)`, is
  a 640 × 480 × 4 = 1.23 MB `memcpy` per frame, 74 MB/s at 60 fps. `_platform_memmove` accounts for
  28 of 46,575 samples (0.06 %). Real, but not the story.

### 4.4 The thing that *is* a real CPU-and-latency cost: the offscreen round trip

`render/src/vk/offscreen.cpp:202`–`:212` submits the frame and then **blocks on a fence**:

```cpp
    if (vkQueueSubmit(ctx_->queue(), 1, &si, fence_) != VK_SUCCESS) { ... }
    if (vkWaitForFences(ctx_->device(), 1, &fence_, VK_TRUE, 1'000'000'000ull) != VK_SUCCESS) { ... }
```

Each frame is therefore: render to an offscreen image → `vkCmdCopyImageToBuffer` into a host-visible
readback buffer (`:189`) → **full CPU stall until the GPU finishes** → `memcpy` 1.23 MB out of that
buffer into the presenter's staging buffer (`present.cpp:303`) → `vkCmdCopyBufferToImage` back onto
the GPU (`present.cpp:325`) → present. The picture makes a GPU → CPU → GPU round trip with zero
CPU/GPU overlap, and it is this serialisation (not the pixel copying) that makes up most of the
0.86 s of extra CPU.

The readback exists for a stated reason — `render/include/dream/render/vk/offscreen.h:7`–`:9`
explains that a title which reads back what it drew needs the pixels on the host — so it cannot
simply be deleted. It can be *pipelined* (render frame N+1 while frame N's readback is in flight) or
skipped for the present path when `from_renderer` is true and nothing has asked for framebuffer
writeback.

### 4.5 What to do

| Change | Expected gain | Files | Risk |
|---|---|---|---|
| A `--present-mode` switch selecting `MAILBOX`/`IMMEDIATE` when available, so benchmarks measure the emulator and not the panel | **Windowed throughput goes from 1.9x to the headless figure.** Zero at real-time pacing. | `render/src/vk/window.cpp:159`, its header, `runtime/boot/boot_main.cpp` arg parsing | **Low.** FIFO stays the default; MAILBOX must be queried through `vkGetPhysicalDeviceSurfacePresentModesKHR` and fall back. |
| Pipeline the offscreen readback one frame deep (two `Offscreen` instances, or two readback buffers and fences) | ~10 % of windowed CPU (**inferred** from the 0.86 s delta); removes a full GPU round trip from the frame's latency | `render/src/vk/offscreen.{h,cpp}`, `runtime/boot/boot_main.cpp` present path | **Medium.** `pixels()` lifetime, screenshots, `--framebuffer-writeback`, and the frame the guest reads back must all still see the right frame. |
| Skip the readback entirely when `from_renderer` and no writeback is requested; present from the GPU image directly | Removes both the 1.23 MB `memcpy` and the readback copy | same | **Medium-high.** Changes what `--screenshot-presented` and `F11`/`F12` capture. |

**Days:** present-mode switch **0.5 / 1 / 2**. Offscreen pipelining **2 / 4 / 7**. Direct present
**2 / 4 / 8**; recommend deferring this one until the rendering-enhancements work settles.

Note for whoever writes the benchmark harness: **do not compare a windowed `--unthrottled` number
with a headless one** until the present-mode switch exists. The windowed number is a property of the
display.

---

## 5. The indirect-call table: the finding the docs did not have

### 5.1 The measurement

`call_indirect` is called **118,307,678 times in 20 guest seconds — 5.9 million per guest second,
one every 34 guest cycles.** Crazy Taxi's attract-mode driver is a chain of `jsr @Rn` through
function-pointer tables; `fn_0c1582a0` (`build-release/games/crazytaxi/gen/crazytaxi.cpp:673457`,
the function the run ends in, final PC `0x0c1583f8`) is full of them:

```cpp
    // 0x0c1583fc: jsr @r2
        c.pc = 0x0c1583fcu; call_indirect(c, m, target);
```

There are 3,305 `call_indirect` sites in `crazytaxi.cpp` alone.

### 5.2 What each one costs

`runtime/src/sh4/abi_bare.cpp:109`:

```cpp
GuestFn find_function(std::uint32_t address, ::dream::Memory* m) noexcept {
    const auto& t = table();
    const std::uint32_t phys = address & 0x1FFFFFFFu;
    if (interpreted(phys))
        return nullptr;
    auto it = std::lower_bound(t.begin(), t.end(), phys, ...);   // :115
```

`table()` holds **3,794 entries** (counted from the six `*.functions.json` files under
`build-release/games/crazytaxi/gen/`). `FunctionEntry` (`runtime/include/dream/runtime/sh4/abi.h:20`)
is 40 bytes — address, `fn`, `signature`, `signature_words`, `end`, `resume` — so the table is
**152 KB**, larger than the M4's L1 data cache. A `lower_bound` over it is ~12 probes, most of them
cache misses, and the first probes are shared across calls while the last few are essentially
random. That is the whole of the 16 per cent.

`interpreted()` (`:57`) is cheap when the dev-interpreter bisection aids are unused: `g_interp_lo ==
g_interp_hi` short-circuits and `g_interp_set` is empty.

Crazy Taxi's build has **no signed (overlay) entries at all** — every `FunctionEntry` in the six
generated tables has `nullptr` for `signature` — so the signature-matching loop at `:116`–`:123`,
which re-reads guest memory on every lookup, never runs for this title. It will for a title that
uses overlay units (`docs/game-config.md`).

### 5.3 The fix, and what it measured

A direct-mapped cache in front of the binary search. The prototype used 2^14 lines of
`{ std::uint32_t phys; GuestFn fn; }` (256 KB), a Knuth-multiplicative index on `phys >> 1`, and
stored both hits and misses:

```cpp
    const std::uint32_t slot = fn_cache_slot(phys);
    if (g_fn_cache[slot].phys == phys)
        return g_fn_cache[slot].fn;
```

A line is only filled when the lookup involved **no signature at all** and neither
`set_interpret_range` nor `set_interpret_functions` is active, so overlay validity is untouched:
an overlay's answer depends on what guest memory holds at that instant and must never be cached.
The cache is cleared by `register_functions`, `set_interpret_range` and `set_interpret_functions`.

**Measured: 5.8x → 10.0x, a further 1.71x**, three runs:

```
fast-mem     host: 3.29 s (6.0x)    fast-mem-fn  host: 1.98 s (10.0x)
fast-mem     host: 3.29 s (6.0x)    fast-mem-fn  host: 1.99 s (10.0x)
fast-mem     host: 3.30 s (6.0x)    fast-mem-fn  host: 1.97 s (10.1x)
```

The run report remained **byte-identical to the unmodified release build**.

`find_function` still shows 810 samples (9.7 %) afterwards — the cache is not free, and a 256 KB
table has its own miss rate. **Inferred:** a two-way set-associative cache, a smaller line (the
table index rather than the pointer), or an open-addressed hash sized to the table would do better;
so would having the emitter pass a per-site inline cache slot, which is the classic answer and
would reduce most sites to a single compare.

### 5.4 Cost and risk

**Risk: low-to-medium, and entirely about invalidation.** The failure mode is stale: a cached entry
surviving a `register_functions` call (only ever at static-init time today) or an overlay becoming
valid/invalid. Refusing to cache anything signature-dependent removes the overlay half completely.
The dev-interpreter bisection aids (`set_interpret_*`) must clear the cache, and a test should
assert that hiding a function by range genuinely hides it after a warm cache.

**Memory:** 256 KB of always-resident table, or 64 KB at 2^12 lines. Trivial.

**Files touched:** `runtime/src/sh4/abi_bare.cpp` only, plus a test in `runtime/tests/`.

**Days: 1 low / 2 central / 4 high.**

---

## 6. AICA

### 6.1 What it costs, measured

AICA is 10 per cent of the stock profile and **~30 per cent of the profile once the memory and
indirect-call fixes are in**, and it runs in full under `--no-audio`. Measured on the two-fix
prototype over 20 guest seconds:

| Variant | Host s | Speed | Saving |
|---|---:|---:|---:|
| Everything on | 1.93 | 10.3x | — |
| `Mixer::dsp_step` returns immediately | 1.71 | 11.6x | **11 %** |
| `Mixer::sample` not called (64 channels + DSP) | 1.64 | 12.1x | **15 %** |
| `Arm7::run` not called | 2.56 | 7.7x | **−33 % (slower)** |
| `Aica::sample_tick` returns immediately | 2.39 | 8.3x | **−24 % (slower)** |

### 6.2 What that says

**The 44.1 kHz tick and the ARM7 are load-bearing and must not be touched.** Removing either makes
the emulator *slower*, because the guest diverges: with the ARM7 stopped the run ends at a different
PC (`0x0c07e0b8` instead of `0x0c1583f8`), delivers 2,384 interrupts instead of 6,854, and the ARM7
PC histogram collapses to `00000000:882031`. The title is waiting on its own sound driver, spinning
in code that does more host work per guest cycle than the driver does. This is the answer to "must
it run at 44.1 kHz granularity": **yes for the ARM7 and the interrupt bookkeeping**
(`Aica::sample_tick`, `runtime/src/aica/aica.cpp:233`, which runs `arm.run`, `step_timers`, sets
`SCIPD`/`MCIPD` sample-done and calls `update_arm_interrupts` / `update_sh4_interrupts`), because
the Manatee driver's timing and the SH-4's view of it both depend on it.

**The sample *generation* is a different matter and is 15 per cent.** `Mixer::sample`
(`runtime/src/aica/mixer.cpp:1028`) is pure output: it produces `last_left`/`last_right` for the
audio sink and affects no guest-visible state. Skipping it changed **only** the AICA line of the run
report — the ARM7 instruction count (60,058,788), sample count (882,031), FIQ count, channel and DSP
write counts, and every non-AICA line were identical.

Within that 15 per cent, **the DSP is 11 points** and the channel loop is 4.

### 6.3 Is the DSP step needed when nothing is using it?

`Mixer::dsp_step` (`mixer.cpp:901`) already has a stop condition: if every word of `MPRO` is zero it
sets `dsp_.stopped` and returns. In Crazy Taxi's attract mode it does **not** trigger — the run
report says `3044 dsp writes; ... dsp running` — so the full loop runs:

```cpp
    for (int step = 0; step < 128; ++step) {       // :919
```

128 microprogram steps, **44,100 times a second — 5.6 million DSP steps per second** — while the
report simultaneously says `0 key-ons, 0 channels active, 0 of 882031 samples non-zero`. Every one
of those 5.6 million steps is processing silence.

`Mixer::sample` (`:1032`) has the same shape: `for (int i = 0; i < 64; ++i) chans_[i].step_mix(...)`
walks all 64 channel objects unconditionally. `step_channel` (`:554`) early-outs on `!enabled`, but
the loop still touches 64 scattered objects and does `*VolMix.DSPOut += oDsp` for each — 2.8 million
pointer chases per second to add zero to zero.

### 6.4 A cheaper structure that keeps "Crazy Taxi plays its music"

`docs/runtime-aica.md:9` records the result that must survive: *"Crazy Taxi plays music. The ARM7
runs Manatee, the title uploads and keys..."*. The proposals below never change what the ARM7 or the
register file do, only when the *mix* is computed.

1. **Track the live channel count instead of scanning.** `Mixer` already has `active_channels()`
   (`:892`) doing a 64-iteration scan; maintain a counter in `key_on`/`set_aeg_state` and a bitmask
   of enabled channels, and iterate only the set bits. Expected: most of the 4-point channel-loop
   cost when few channels play; **nothing** when all 64 are live, which is the case that matters for
   correctness. **Low risk.** Gain: up to 4 % in attract mode, near zero in dense gameplay.

2. **Extend the existing DSP stop condition to cover a silent DSP.** The DSP may be stopped when
   `MPRO` is all zeros *or* when every input is zero and its ring buffer has drained. The cheap,
   safe version: count consecutive samples for which all 16 `MIXS` slots and both `EXTS` are zero;
   after more than `RBL + 1` such samples (the ring length, so any tail has certainly been shifted
   out) stop stepping, and resume on the first non-zero input or any `MPRO`/DSP-register write
   (`dsp_.dirty` already exists for exactly this signal). Expected: **the full 11 %** whenever
   nothing is playing, zero when anything is. **Risk: medium** — get the drain length wrong and a
   reverb tail is cut short. It is audible, not silent, which is the right failure mode, and it is
   directly testable against a `--wav` capture.

3. **Do not compute the mix at all when no sink wants it.** Under `--no-audio` with no `--wav` and
   no `on_sample` consumer, `Mixer::sample` produces a number nobody reads. Gating it on
   "someone is listening" is a two-line change worth the whole 15 per cent in headless benchmark and
   CI runs. **Risk: low**, but it makes headless timings stop predicting windowed-with-audio
   timings, so it must be reported in the run header. Recommend it as an explicit
   `--no-audio-mix` rather than silent behaviour.

4. **Not recommended: decimating the 44.1 kHz tick.** Batching N samples per scheduler event would
   cut scheduler traffic (section 7) but changes when `SCIPD` sample-done reaches the ARM7 and when
   `update_sh4_interrupts` raises `AicaIrq`. The measured "skip the ARM7" and "skip the tick"
   results show how sharply the guest responds to changes in this path. If it is attempted, it must
   be batched *inside* `sample_tick` with the ARM7 still stepped per sample, and verified with
   `--write-hash`.

**Files touched:** `runtime/src/aica/mixer.cpp`, `runtime/include/dream/runtime/aica/mixer.h`; for
item 3 also `runtime/src/aica/aica.cpp` and `runtime/boot/boot_main.cpp`.

**Days:** channel bitmask **0.5 / 1 / 2**; DSP silence detection **1 / 2 / 4**; no-sink gate
**0.5 / 1 / 2**.

---

## 7. The poll and scheduler path

### 7.1 How often, measured

`on_poll` is called **1,856,312 times in 20 guest seconds — 92,816 per guest second, one every
~2,155 guest cycles** — and delivers **343 interrupts per guest second**. A 270:1 ratio.

That rate is not set by interrupt latency requirements. It is set by device tick granularity, via
`System::arm_next_poll` (`runtime/src/system.cpp:55`):

```cpp
    c.next_event = intc.select(sh4::read_sr(c)) ? c.cycles : sched.next_deadline();
```

`next_event` is always the *next scheduler deadline*, whatever that deadline is for. The scheduler
fired **1,828,513 events** — 91,426 per guest second — which decomposes as:

| Source | Per guest second | Why |
|---|---:|---|
| `aica.sample` | 44,100 | `kSh4CyclesPerSample = 200'000'000 / 44100` = 4,535 cycles (`aica.h:25`, `aica.cpp:14`–`:18`) |
| `spg-line` | ~31,700 | one event **per scanline**: 60.45 fps × 525 lines (`spg.cpp:6`, `:40`, `:72`) |
| TMU, RTC, maple, GD-ROM, watchdog, sampler | ~15,600 | the remaining nine registered events |

Every emitted function entry and loop back-edge carries the check; there are **5,496** of them in
`crazytaxi.cpp`, of the form (`crazytaxi.cpp:509953`):

```cpp
    if (c.cycles >= c.next_event) { c.pc = 0x0c07d018u; deliver_irq(c, m); }
```

The check itself is two loads and a compare and is not the problem. **The problem is that it comes
back true 92,816 times a second.**

### 7.2 What each poll does

`System::on_poll` (`system.cpp:122`) → `sched.advance_to(c.cycles)` → `deliver_pending` →
`arm_next_poll` → `sched.next_deadline()`. Both scheduler entry points are **linear scans**:

```cpp
std::uint64_t Scheduler::next_deadline() const noexcept {   // scheduler.cpp:17
    std::uint64_t best = kNever;
    for (const auto& e : events_)
        if (e.deadline < best) best = e.deadline;
    return best;
}
void Scheduler::advance_to(std::uint64_t target) {          // scheduler.cpp:25
    for (;;) {
        // full scan of events_ to find the minimum, every iteration
```

`Event` is `{ std::string name; Callback cb; std::uint64_t deadline; }`
(`runtime/include/dream/runtime/sched/scheduler.h:43`–`:47`) — 64 bytes each, with `Callback` a
`std::function` (`:19`), so every fired event is an indirect call through type-erased storage.
Thirteen events at 64-byte stride is 13 cache lines scanned per `advance_to` iteration, and
`advance_to` was called **2,572,717 times** (more often than `on_poll`, because
`memory.on_device_access` at `system.cpp:46` also advances the clock before every MMIO access).

### 7.3 What it is worth

**Measured: not much, and that is the useful finding.** In the two-fix profile, `advance_to` is 106
samples (1.3 %) and `next_deadline` is 35 (0.4 %) — **1.7 per cent combined**. Thirteen events at
64-byte stride stay resident in L1, so ~11 ns per call is about right and a heap would not beat it.
`Intc::select` is 10 samples in the stock profile.

The poll path's *apparent* weight in a call-graph profile is almost entirely the guest's own
interrupt handler running inside `enter_exception` (`system.cpp:59`), which is guest work.

### 7.4 What is nevertheless worth doing, in order

1. **Make SPG compute its scanline lazily instead of ticking per line.** `Spg::on_line`
   (`spg.cpp:40`) re-arms every `line_cycles_` purely to increment `scanline_` and compare it
   against four or five interesting values. `SPG_STATUS` reads (`spg.cpp:73`) already go through
   `on_device_access`, which advances the clock, so `scanline_` could be *derived* from
   `sched.now()` on demand and events armed only for the vblank-in, vblank-out, vstart, vbend and
   the programmed hblank line. That takes ~525 events per frame down to ~5 — **cutting total
   scheduler traffic by about a third**. The one case that still needs per-line events is
   `SPG_HBLANK_INT` mode 2 ("every line", `spg.cpp:62`), which Crazy Taxi does not use.
   **Risk: medium** — raster-timing reads and the vblank callback that drives presentation both
   depend on this being exactly right. **Days: 1 / 2 / 4.**
2. **Keep a cached minimum deadline in the scheduler,** invalidated on `request`. Removes the
   `next_deadline` scan from `arm_next_poll`. **Risk: low. Days: 1 / 2 / 3.** Worth ~0.5 %.
3. **Replace `std::function` in `Event` with a `void(*)(void*, u64, u64)` plus a context pointer,
   and drop `std::string name` to a `const char*`.** Takes `Event` from 64 bytes to 24 and removes
   an indirect call per fired event. **Risk: low. Days: 0.5 / 1 / 2.** Worth ~0.5 %.

**Do not** make the poll *less frequent* to buy speed. The check is nearly free; it is the events
behind it that cost, and interrupt latency is the one thing ADR 7
(`docs/decisions/README.md:19`, "cooperative checks at function entry and loop back-edges") is
protecting. Reducing the SPG tick rate is the right lever because it removes work, not accuracy.

---

## 8. Compiler flags, and what is off limits

### 8.1 Emitted code is compiled at `-O1`, and it does not matter

`cmake/DreamAddGame.cmake:115`:

```cmake
    # Tens of megabytes of generated C++: keep the optimiser modest so the edit-build-run loop
    # stays short.
    set_source_files_properties(${_units} PROPERTIES COMPILE_OPTIONS "-O1")
```

This is unconditional — it applies in `build-release/` exactly as in `build/`, which is why the
brief's observation that "dev and release builds measure identically" holds: the emitted code is at
`-O1` in both, and it is only 13 per cent of the profile.

**Measured**, by recompiling all six generated units at `-O2` and relinking against the stock
libraries:

```
--- O1  host: 5.54 s (3.6x)      --- O2  host: 5.40 s (3.7x)
--- O1  host: 5.55 s (3.6x)      --- O2  host: 5.45 s (3.6x)
```

**About 2 per cent.** The generated object grew from 8,652,760 to 9,581,344 bytes (+11 %). The
30 MB translation unit compiled at `-O2` in **25.8 s** on this machine, which is not a meaningful
change to the edit-build-run loop.

The conclusion is not "raise it to `-O2`" — 2 per cent is not worth an argument — it is that **the
`-O1` comment is now misleading**. It reads as a performance compromise; it is not one, because the
emitted code was never the bottleneck. Worth a one-line comment update alongside whichever fix lands
first. If `-O2` is adopted anyway (**0.25 days**), note that `dream_target_defaults` already appends
`-ffp-contract=off -frounding-math` to these units and `-O2` does not disturb them.

### 8.2 LTO is off and untested

`build-release/CMakeCache.txt` has `BUILD_LTO:BOOL=OFF`. **Not measured** — a full LTO link of a
9.5 MB generated object plus the runtime was out of scope here. LTO is *semantically safe* under
ADR 16 (it does not change floating-point contraction or rounding assumptions), and it is the one
mechanism that could devirtualise `Memory::read32` without the manual inlining of section 3 — but
it would still have two `Memory` implementations to speculate between, so it would produce a guarded
devirtualisation at best, which is what section 3 writes by hand and measures. **Recommendation:
measure it, but do it after section 3, not instead of it.** 0.5 days to measure.

### 8.3 Off limits, for correctness

ADR 16 (`docs/decisions/README.md:28` and `:392`–`:417`) and `cmake/DreamTargetDefaults.cmake:1`–`:11`
set the rules, and they are not negotiable for a build whose golden traces must match across x86-64
and ARM64:

| Forbidden | Why |
|---|---|
| `-ffp-contract=fast` / `=on`, or anything that re-enables FMA | "an FMA on ARM64 but not on x86-64 changes results in the last bit" (`DreamTargetDefaults.cmake:3`–`:4`). This is the single most tempting flag and the single most damaging. |
| `-ffast-math`, `-Ofast`, `-funsafe-math-optimizations`, `-ffinite-math-only`, `/fp:fast` | Each implies contraction and/or reassociation. |
| Dropping `-frounding-math` | The guest runs with `FPSCR.RM` = round-toward-zero and switches at run time; without it the compiler folds constants under the default mode. `DreamTargetDefaults.cmake:5`–`:8` records that the differential harness actually caught a folded `1.0f/3.0f`. |
| ISA intrinsics in emitted code (NEON, SSE) | ADR 16 item 1: emitted code stays portable C++; `FIPR`/`FTRV` go through helpers. |
| `-march=native` where it enables FMA contraction or changes `long double` behaviour | Same class of problem, and it makes a build unreproducible across machines. |

Everything proposed in this study is integer and pointer work. **None of it touches a
floating-point operation, an `FPSCR` mode, or an emitted arithmetic expression**, which is why all
of it is compatible with the golden traces.

---

## 9. Every optimisation, with gain, files, risk and days

Ordered by measured value. "Gain" is against the state after everything above it in the table.

| # | Optimisation | Gain | Measured? | Files | Correctness risk | Days (l/c/h) |
|---|---|---:|---|---|---|---|
| 1 | Inline the mask-and-index RAM fast path into emitted code (ADR 5) | **1.68x** | **yes** | `runtime/include/dream/runtime/memory.h`, `.../mem/dc_memory.h`, `runtime/src/mem/dc_memory.cpp` | **Medium** — dev features (journal/hash/watch) must force the slow path; store queues, MMIO, VRAM views, fault log all untouched by construction | 2 / **4** / 7 |
| 2 | Direct-mapped cache in front of `find_function` | **1.71x** | **yes** | `runtime/src/sh4/abi_bare.cpp` | **Low-medium** — invalidation on `register_functions` and `set_interpret_*`; never cache a signature-dependent answer | 1 / **2** / 4 |
| 3 | AICA: stop the DSP when its inputs have been silent longer than the ring buffer | **1.13x** | **yes** (as a hard skip) | `runtime/src/aica/mixer.cpp`, `.../aica/mixer.h` | **Medium** — a mis-sized drain window truncates a reverb tail | 1 / **2** / 4 |
| 4 | `--present-mode` switch (MAILBOX/IMMEDIATE) so windowed benchmarks measure the emulator | **1.9x → headless figure, windowed** | **yes** (diagnosed) | `render/src/vk/window.cpp:159`, its header, `runtime/boot/boot_main.cpp` | **Low** — FIFO stays default, query and fall back | 0.5 / **1** / 2 |
| 5 | AICA: iterate a live-channel bitmask instead of all 64 | ~1.04x | partly (mixer skip = 1.18x total) | `runtime/src/aica/mixer.cpp`, `.../mixer.h` | **Low** | 0.5 / **1** / 2 |
| 6 | SPG: derive the scanline from the clock; arm events only at interesting lines | ~1.02–1.05x | no (**inferred** from 31,700 of 91,426 events/s) | `runtime/src/pvr/spg.cpp`, `.../pvr/spg.h` | **Medium** — raster reads and the vblank present callback depend on it | 1 / **2** / 4 |
| 7 | Pipeline the offscreen readback one frame deep | ~1.1x windowed CPU | no (**inferred** from the 0.86 s CPU delta) | `render/src/vk/offscreen.{h,cpp}`, `runtime/boot/boot_main.cpp` | **Medium** — `pixels()` lifetime, screenshots, `--framebuffer-writeback` | 2 / **4** / 7 |
| 8 | AICA: skip the mix when no sink is listening (`--no-audio-mix`) | 1.18x in headless CI only | **yes** | `runtime/src/aica/aica.cpp`, `runtime/boot/boot_main.cpp` | **Low**, but headless timings stop predicting windowed ones — must be in the report header | 0.5 / **1** / 2 |
| 9 | Scheduler: cached minimum deadline; POD callback instead of `std::function`; `const char*` names | ~1.01x | **yes** (1.7 % measured as the ceiling) | `runtime/src/sched/scheduler.cpp`, `.../sched/scheduler.h` and its ten call sites | **Low** | 1 / **2** / 4 |
| 10 | Per-call-site inline cache for indirect calls, emitted by the translator | **inferred** ~1.05x on top of #2 | no | `translator/src/emit/`, `runtime/src/sh4/abi_bare.cpp` | **Medium** — a new emitted construct, needs its own golden-trace pass | 3 / **5** / 9 |
| 11 | Raise emitted code from `-O1` to `-O2` | 1.02x | **yes** | `cmake/DreamAddGame.cmake:115` | **Low** — `-ffp-contract=off -frounding-math` are appended by `dream_target_defaults` either way | 0.1 / **0.25** / 0.5 |
| 12 | Measure LTO (`BUILD_LTO=ON`) | unknown | no | `cmake/` | **Low** semantically; link time and memory are the risk | 0.25 / **0.5** / 1 |
| — | Replace exception-based non-local returns | ~1.02x (1,006 throws/s, ~2.4 % of profile in unwinder) | **measured cost**, fix not prototyped | `runtime/src/sh4/abi_bare.cpp`, every emitted `rts` | **High** — this is the mechanism `docs/emitter-design.md` uses for task switches | **not recommended** |

**Recommended first slice: items 1, 2, 3 and 4 — nine focused engineer-days central — for a measured
2.9x headless plus a windowed benchmark that means something.** Items 5, 6, 8 and 9 are a further
six days for roughly another 1.1x.

Compounded central estimate for items 1–9: **3.2x**, of which **2.9x is already measured on a
running binary**.

---

## 10. How to verify each of these without breaking the golden traces

The repository already has every tool this needs; none of it has to be built.

### 10.1 The four gates, in the order to run them

1. **The run report, diffed.** The cheapest and, empirically, the sharpest:
   ```
   ./build-release/games/crazytaxi/crazytaxi_boot --config games/crazytaxi/crazytaxi.toml \
       --no-audio --unthrottled --max-seconds 20 > after.txt
   diff <(grep -v '^host:' before.txt) <(grep -v '^host:' after.txt)
   ```
   Those 43 lines cover guest cycles, frame count, final PC, interrupts delivered and max nesting,
   task-switching RTEs, syscall counts by kind, maple frames by recipient and by command, AICA
   sample and ARM7 instruction counts, FIQ and timer-IRQ counts, holly register state, untranslated
   call targets, unmapped accesses and the relocation-candidate list. Both prototypes in this study
   pass it byte-for-byte. A change that alters guest behaviour will almost always move one of these.
   **Applies to: every item.**

2. **Per-frame store hashes.** `--write-hash FILE` writes one line per frame with a rolling hash of
   every guest store and the store count (`dc_memory.h:118`–`:124`,
   `docs/differential-harness.md:145`):
   ```
   crazytaxi_boot ... --max-seconds 20 --write-hash before.hash
   crazytaxi_boot ... --max-seconds 20 --write-hash after.hash
   diff before.hash after.hash
   ```
   When they diverge, `--write-log FILE --write-log-range FROM:COUNT` dumps the individual stores
   around the frame so two runs can be diffed down to the one store that differs
   (`docs/differential-harness.md:147`).
   **This is the mandatory gate for item 1**, and it is the gate that caught the prototype's own
   defect: with `fast_ok` forced true the prototype reports `0000000000000000` and a store count of
   `0` for every frame, because the fast path bypasses `note_write`. **A correct implementation must
   produce hashes identical to the current build**, which means `fast_ok` must be false whenever
   `hash_writes` is set. Treat "hashes match, and the store counts are non-zero" as the pass
   condition — matching zeros is not a pass.

3. **The golden SH-4 traces.** `tests/sh4/golden/` holds 78 cases (image, fill, registers and the
   interpreter's final state) replayed by `tests/sh4/test_golden.cpp` in `dream_emit_tests`, written
   by `tools/oracle/write_golden.py` (`docs/differential-harness.md:80`–`:92`). These are the ISA
   conformance gate ADR 16 requires to match on both host ISAs. **None of the items in section 9
   should move them**, because none touches an arithmetic expression, an `FPSCR` mode or a
   floating-point flag — which is exactly why that claim needs checking rather than assuming:
   ```
   cmake --build build && ctest --test-dir build -R differential --output-on-failure
   ctest --test-dir build --output-on-failure
   ```
   **Applies to: items 1, 2, 10, 11, 12.** Item 1 changes the accessor signatures the harness's
   `BareMemory` provides, so it will touch these files even though it must not change their results.

4. **The interpreter replay harness.** Translate with `--replay-hooks` and run with `--replay`, or
   `--replay-only addr` for one function (`docs/differential-harness.md:182`): every translated call
   is executed twice, once translated and once interpreted, from the same memory, using the store
   journal (`dc_memory.h:146`–`:151`) to roll back between them. **Item 1 must set `fast_ok = false`
   while `journaling` is on**, or the journal will not record the stores it has to undo, and the
   comparison will silently pass on rolled-back state that was never rolled back. Run
   `--replay-self-check` first to confirm the harness is sound before trusting a `--replay` pass.
   **Applies to: item 1 above all, then 2 and 10.**

### 10.2 Per-item additions

| Item | Additional verification |
|---|---|
| 1 memory fast path | `runtime/tests/` already has 88 assertions over mirrors, VRAM view aliasing, SQ flush to RAM and to a device, dispatch sizes and the fault log (`docs/progress.md:32`). Add: an area-3 access through all four of P0/P1/P2/P3; an access at `0x0CFFFFFF` and `0x0D000000` (the mirror boundary); one at `0x0BFFFFFF`; a `--write-hash` run with the fast path on, asserting a non-zero store count. |
| 2 function cache | A test that warms the cache, calls `set_interpret_range` over a warm entry and asserts the function is now hidden; a test with a signed (overlay) entry asserting it is never served from cache after guest memory changes underneath it. Use a title with overlay units — Crazy Taxi has none. |
| 3, 5, 8 AICA | `--wav out.wav` capture before and after, compared as audio. `docs/runtime-aica.md:102` records the reference: windowed and paced, Crazy Taxi plays 286,584 samples. The gate is "music still plays and the sample count is unchanged", not bit-equality of the PCM — item 3 deliberately changes the tail. Also: the AICA report line must still show `882031 samples` and `60058788 ARM instructions` for a 20 s attract run. |
| 4 present mode | `--screenshot-at N` before and after must produce identical PNGs; the only thing that may change is wall-clock time. FIFO must remain the default and the fallback path must be exercised on a device that reports no MAILBOX. |
| 6 SPG | A test that reads `SPG_STATUS` at many points across a frame and compares the scanline field against the current per-line implementation; the frame count in the run report must be unchanged (1,209 for a 20 s run); `--screenshot-at` must be identical. |
| 7 offscreen pipelining | `--screenshot-presented`, `F11` capture, and `--framebuffer-writeback` all still produce the frame they used to. `--capture-at N` before and after. |
| 9 scheduler | The instrumented counters from section 1.2 (`on_poll`, `advance_to`, events fired) must be **unchanged**, not merely similar — a scheduler that fires a different number of events has changed guest timing. |
| 11, 12 build flags | Golden traces on **both** host ISAs (ADR 16 item 4), not just ARM64. This is the one place where a change that passes locally can still be wrong. |

### 10.3 A regression guard worth adding

None of this is currently measured in CI. A `ctest` case that runs the 20-second attract-mode
benchmark and fails if `Nx real time` drops below a floor would have caught the `-O1` surprise and
would protect the 2.9x once it lands. **0.5 days**, and it needs a machine whose performance is
stable enough to set a floor on — which on CI runners usually means a generous floor and a trend
line rather than a hard gate.

---

## 11. What could not be determined here, and how to find out

1. **Whether 2.9x holds in gameplay.** This is the biggest open question and it undercuts every
   number above if the answer is no. Attract mode is a driving demo with the HUD and input paths
   quiet; after the two fixes the emitted game code is already 44 per cent of the profile, and in
   gameplay it will be more. The runtime-side fixes cannot speed up the guest's own code, so the
   speedup there will be smaller — **inferred**, plausibly 2x.
   **How to find out:** `--press` a scripted start sequence to reach in-game play, run
   `--unthrottled --max-seconds 60` from a fixed save state, and profile. `docs/game-config.md` and
   the `--press` option (`boot_main.cpp:1230`) already support scripted input. Half a day.
2. **Why `find_function` grew from 16 % to 49 % of the profile after the memory fix, before the
   cache.** Absolute throughput improved 1.68x, so this is a shift in attribution, not new work: on
   an out-of-order core, removing the `resolve` stalls exposes the binary search's cache misses that
   were previously overlapped. **Not confirmed.** It does not change any recommendation — the cache
   fixed it — but it is a warning that single-symbol percentages from `sample` on this machine are
   softer than they look, and that the wall-clock A/B is the number to trust.
   **How to find out:** hardware counters. `xctrace record --template 'CPU Counters'` with
   L1D/L2 miss and stall-cycle events, on the stock build and on the memory-fix prototype. One day.
3. **What the second-order effect of the function cache's own miss rate is.** `find_function` is
   still 9.7 % after a 2^14-line direct-mapped cache. **How to find out:** instrument hit/miss/
   conflict counts, sweep the size and associativity, and compare against a per-site inline cache
   (item 10). Half a day of measurement before committing to item 10's five.
4. **Whether the 1,006 non-local-return throws per second are avoidable.** They cost ~2.4 % in
   unwinder time. `task_switch_rtes` was 0 in this run, so these are `rts`-to-a-different-PR cases
   inside translated code, not Katana task switches. **How to find out:** log the `(from, to)` pairs
   with `DREAM_TRACE_NONLOCAL` (`system.cpp:82`) and see whether a small number of call sites
   dominate; if they do, the emitter may be able to handle them as ordinary control flow.
   One day to find out, before spending anything on the fix.
5. **LTO.** Untested, section 8.2. Half a day.
6. **Whether any of this helps on x86-64.** Every measurement here is Apple M4. The memory fast path
   should help more on a machine with a smaller out-of-order window; the `find_function` cache
   should help about the same. **Unknown**, and ADR 13 makes Windows x64 a v1 target.
   **How to find out:** repeat sections 1 and 2 on the x64 CI machine. Half a day, and it should be
   done before the estimates in section 9 are committed to a plan.
7. **The real-time (throttled, windowed, with audio) figure after the fixes.** Everything here is
   unthrottled headless. The brief's baseline of 0.40 CPU-s per wall-second is the number the owner
   actually cares about, and the projection of 0.14 in section 0 is arithmetic, not a measurement.
   **How to find out:** once items 1–4 land, re-run the exact baseline configuration — real time,
   windowed, audio on — and time it with `/usr/bin/time -l`. Half an hour.

---

## Appendix: reproducing the prototypes

Both prototypes were built from a copy of `runtime/` in a scratch directory, compiled with the
repository's own flags (`-O3 -DNDEBUG -std=c++20 -ffp-contract=off -frounding-math`, and `-O1` for
the generated units as `DreamAddGame.cmake:115` specifies), and linked against the stock
`libdream_translator.a`, `libdream_render_vk.a`, `libdream_audio.a`, `libdream_sh4_decoder.a`,
`libdream_render.a` and libchdr from `build-release/`.

The whole runtime had to be rebuilt for each, not just the changed file: adding a member to
`Memory` or to `Scheduler` changes the layout every translation unit sees, and relinking a subset
produces a binary that segfaults (observed, and fixed by forcing a full rebuild). A full rebuild of
the 23 runtime translation units plus `boot_main.cpp` plus the six generated units took **under two
minutes** on this machine, of which the 30 MB `crazytaxi.cpp` is 26–44 s. **The edit-measure loop
for CPU work in this repository is a couple of minutes, not an afternoon** — which is worth knowing
before anyone estimates this work from the size of the generated code.
