# Where the memory goes: a measured breakdown of the release build's footprint (study)

Written 2026-09-14 from a measurement pass over the existing `build-release/` binary (Apple M4,
macOS 26.6.2, Crazy Taxi attract mode from the owner's gitignored `crazytaxi.chd`) plus a reading
pass over `runtime/src/mem/`, `render/src/vk/`, `runtime/src/aica/` and `runtime/boot/boot_main.cpp`.
**No source was changed and no code was written.** Every measurement below can be reproduced with
the commands in the appendix.

Numbers are labelled **[M]** where they were measured on this machine and **[I]** where they were
inferred from reading the code or from arithmetic over measured quantities. Where the two disagree,
section 7 says so.

---

## 0. Verdict

**The 163 MB headline is mostly not our heap, and the single largest item in it is a texture-cache
allocation strategy, not texture data.**

Three things follow from the measurements, and each one is a surprise relative to what a reading
pass alone would predict:

1. **The guest's own memory is 28.1 MB and it is genuinely irreducible** — 16 MB RAM, 8 MB VRAM,
   2 MB sound RAM, 2 MB boot ROM, 128 KB flash, 8 KB on-chip. All six are allocated zero-filled at
   construction, so all 28.1 MB is resident from the first instruction. Section 1.

2. **Headless, the whole process is 39.1 MB of real private memory** [M] — not 75 MB, and certainly
   not 163 MB. `/usr/bin/time -l`'s "maximum resident set size" of 75.6 MB counts ~37 MB of
   *shared* system dylib text that every process on the machine shares. The honest headless number
   is the 39.1 MB physical footprint, of which 29.6 MB is the guest (with malloc rounding) and
   9.5 MB is everything else. Section 6.

3. **With a window, 116 MB of the footprint is GPU-side, and 92 MB of that is 46 allocations of
   exactly 2 MB each — one per cached texture.** [M] This is the finding, and the amplification is
   not subtle: decoding every texture the title uses out of its own display lists gives **34
   distinct textures, the largest of them 64 KB, totalling 0.82 MB of RGBA8888.** [M] Under
   1 MB of pixels is occupying 92 MB of memory. It is not that the texture cache never evicts (it
   doesn't, but with 48 entries that hardly matters); it is that **every cached texture gets its own
   `vkAllocateMemory`, and on this host each one costs 2 MB of physical memory whatever the
   texture's real size.** Sections 2.3 to 2.5.

**Irreducible floor: about 61 MB with a window at 640x480, about 33 MB headless** — 28.1 MB guest,
~4.2 MB binary text and data, ~2.5 MB of render targets, and ~26 MB of
Vulkan/MoltenVK/SDL/Metal/swapchain state that comes with having a window on this platform at all.
Section 7.

**Addressable: about 115 MB of the 176.6 MB footprint measured at scale 1** [M], dominated by one
change. Suballocating texture images from a small number of pooled device allocations should recover
**~90 MB for about 4 focused engineer-days**. Section 8 ranks the rest.

**What is explicitly not recommended:** shrinking any guest region (section 1.4), adding an LRU to
the texture cache *as the primary fix* (section 2.3 — it addresses a bound that is not currently
being hit), or chasing the binary's 9 MB of text (section 3).

---

## 1. The guest's own memory: 28.1 MB, all of it touched, none of it reducible

### 1.1 The six allocations

`runtime/include/dream/runtime/mem/dc_memory.h:93-98` declares the sizes and
`runtime/src/mem/dc_memory.cpp:61-67` is the whole of the allocation:

```cpp
DcMemory::DcMemory()
    : ram_(new std::uint8_t[kRamSize]()),
      vram_(new std::uint8_t[kVramSize]()),
      aram_(new std::uint8_t[kAramSize]()),
      bios_(new std::uint8_t[kBiosSize]()),
      flash_(new std::uint8_t[kFlashSize]()),
      ocram_(new std::uint8_t[kOcramSize]()) {}
```

| Region | Constant | Declared size | Malloc'd [M] | Why it exists |
|---|---|---|---|---|
| Main RAM | `kRamSize = 16u << 20` | 16,777,216 | 16,793,600 | `dc_memory.h:93`; area 3 and its three mirrors, `dc_memory.cpp:154-157` |
| VRAM | `kVramSize = 8u << 20` | 8,388,608 | 8,404,992 | `dc_memory.h:94`; area 1, **both views**, `dc_memory.cpp:147-151` |
| Sound RAM | `kAramSize = 2u << 20` | 2,097,152 | 2,113,536 | `dc_memory.h:95`; area 0 at +0x00800000, `dc_memory.cpp:135-139` |
| Boot ROM | `kBiosSize = 2u << 20` | 2,097,152 | 2,113,536 | `dc_memory.h:96`; area 0 at +0, `dc_memory.cpp:130-134` |
| Flash | `kFlashSize = 128u << 10` | 131,072 | 147,456 | `dc_memory.h:97`; area 0 at +0x00200000 |
| On-chip RAM | `kOcramSize = 8u << 10` | 8,192 | 10,240 | `dc_memory.h:98` |
| **Total** | | **29,499,392 (28.13 MB)** | **29,583,360 (28.21 MB)** | |

The "Malloc'd" column is measured, not computed: `malloc_history -allBySize` attributes each of the
six allocations to `dream::mem::DcMemory::DcMemory()` by name. The excess over the declared size is
libmalloc's large-allocation rounding.

### 1.2 VRAM is one allocation with two views, not two allocations

This was worth settling because "8 MB VRAM in two views" reads like it could be 16 MB.

It is one 8 MB buffer. `dc_memory.cpp:147-151` is the whole of area 1:

```cpp
        case 1: {  // area 1: VRAM, 64-bit view at +0x0000000, 32-bit view at +0x1000000, mirrors
            const std::uint32_t off = phys & 0x007FFFFFu;
            t.kind = Target::kBytes;
            t.bytes = vram_.get() + ((phys & 0x01000000u) ? vram_map32(off) : off);
            return t;
        }
```

The 32-bit view is an *address remap* into the same bytes — `vram_map32`
(`dc_memory.cpp:86-94`) interleaves the two 4 MB banks word by word, matching the hardware's 64-bit
bus. One buffer, two addressings. Confirmed by measurement: there is exactly one 8 MB allocation in
the process, not two. [M]

### 1.3 All 28.1 MB is resident from startup

Every one of the six uses `new std::uint8_t[N]()` — the trailing `()` is
value-initialisation, which zero-fills. That writes every page, so every page is dirty and resident
before the guest executes an instruction. vmmap confirms it: the three `MALLOC_LARGE` regions show
`RESIDENT == DIRTY == VSIZE` for the 16 MB and 8 MB buffers. [M]

This is correct behaviour and not a defect — the guest may read any of it, and a lazily-faulted
region would only move the cost, not remove it. But it means the floor is paid up front and is
insensitive to how much of the console the title actually uses.

### 1.4 Is any of it unused by this title? Yes — and it should stay

Two regions are near-dead for Crazy Taxi:

- **Boot ROM, 2 MB.** The runtime HLEs the BIOS (`runtime/src/hle/bios.cpp`); no real boot ROM is
  loaded. The region exists so that a title reading the BIOS's syscall vectors or font data finds
  memory rather than the fault log.
- **Flash, 128 KB.** Backed by `runtime/src/hle/flash.cpp`; a title reads a few dozen bytes of it
  (region, language, the ISP block).

Between them that is 2.13 MB, or 7.6% of the guest floor and 1.5% of the windowed footprint. It is
not worth touching: the saving is small, the risk is a title that reads an address the runtime
stops mapping and falls into the fault log, and the regions are exactly the kind of thing that
turns a one-title runtime into a general one. **Treat the full 28.1 MB as the floor.**

---

## 2. The renderer: 116 MB of GPU-side allocation at 640x480, and where it goes

### 2.1 The offscreen target

`render/src/vk/offscreen.cpp:21-33` creates two attachments at `width x height`:

- colour, `VK_FORMAT_R8G8B8A8_UNORM` (`offscreen.cpp:21`, `:35`) — 4 bytes per pixel;
- depth, from `pick_depth_format` (`offscreen.cpp:27`), which prefers `VK_FORMAT_D32_SFLOAT`
  (`render/src/vk/resources.cpp:162-171`) — 4 bytes per pixel on this host.

Both are `VK_SAMPLE_COUNT_1_BIT` (`offscreen.cpp:36`, `:44`) — there is no MSAA to pay for.

`--scale` multiplies both dimensions. `runtime/boot/boot_main.cpp:621` fixes the guest resolution at
`kGuestWidth = 640, kGuestHeight = 480`, and `:129` computes the target:

```cpp
        const std::uint32_t w = kGuestWidth * scale, h = kGuestHeight * scale;
```

There is a third full-size buffer: the readback, `offscreen.cpp:119`, `width * height * 4` bytes,
host-visible, allocated once and kept.

| `--scale` | Resolution | Colour | Depth (D32) | Readback | Subtotal [I] |
|---|---|---|---|---|---|
| 1 | 640 x 480 | 1.23 MB | 1.23 MB | 1.23 MB | 3.69 MB |
| 2 | 1280 x 960 | 4.92 MB | 4.92 MB | 4.92 MB | 14.75 MB |
| 4 | 2560 x 1920 | 19.66 MB | 19.66 MB | 19.66 MB | 58.98 MB |

Measured, these appear as four `owned unmapped (graphics)` regions: 4 x 1296 KB at scale 1, and
4 x 19.0 MB at scale 4. [M] The fourth is the presenter's image, section 2.2.

### 2.2 The presenter and the swapchain

`render/src/vk/present.cpp:223-287` keeps one `VK_FORMAT_R8G8B8A8_UNORM` image sized to the frame it
is given (`ensure_image`, resized only when the size changes) plus a host-visible staging buffer of
`width * height * 4` (`present.cpp:298-303`). Both follow `--scale`, because
`boot_main.cpp:245-250` deliberately hands the window the full rendered resolution:

```cpp
        // --scale visible: the window gets the full rendered resolution.
            shown.width = offscreen.width();
            shown.height = offscreen.height();
```

The swapchain asks for `caps.minImageCount + 1` (`render/src/vk/window.cpp:142`) — three images on
this host — and `kFramesInFlight = 2` (`window.cpp:14`) command buffers and fence/semaphore sets.
Swapchain images are not our allocations; they show up as `IOAccelerator (graphics)` shared memory,
measured at 7.6 MB at scale 1 and 42.6 MB at scale 4. [M]

### 2.3 The texture cache: no eviction — but that is not the problem

**There is no eviction of any kind.** `render/include/dream/render/vk/texture_cache.h` has no
budget, no LRU, no high-water mark and no age. Entries leave the map only through:

- `invalidate()` — drops everything, called when the palette actually changes
  (`texture_cache.cpp:77-87`);
- `invalidate_range(begin, end)` — drops entries whose pixels a render overwrote
  (`texture_cache.cpp:89-113`);
- `destroy()` — shutdown.

The only ceiling is the descriptor pool: `kMaxTextures = 2048` (`render/src/vk/texture_cache.cpp:12`,
used at `:34` and `:37`). Past that, `vkAllocateDescriptorSets` fails and `get()` sets
`error_ = "the descriptor pool is exhausted"` (`texture_cache.cpp:249-253`) — and, note, still
inserts the entry, so the memory is kept and the texture is never drawn again.

Each surviving entry holds, per `texture_cache.h:130-138`:

- a `VkImage` + its own `VkDeviceMemory` — `R8G8B8A8_UNORM`, one mip level
  (`texture_cache.cpp:268-283`), so **4 bytes per texel regardless of the guest's format**;
- a `HostBuffer staging` — a host-visible, permanently mapped second copy of the same pixels
  (`texture_cache.cpp:263-266`), kept alive for the entry's whole life even though it is only read
  once, during upload.

**Upper bound [I]:** 2048 entries x a 1024x1024 texture x 4 bytes x 2 copies = 16 GB. That number is
theoretical — nothing evicts, so a long session in a texture-heavy title is bounded only by that.
For Crazy Taxi's attract mode it is not the live issue: the run decodes **48 textures**, measured
from the run's own report — `textures 48 decoded, 0 failed, 0 overwritten by a render, 1 palette
changes` — over 1287 frames, and the guest fed 2,391,904 bytes of texture data in total. [M]

So: unbounded, yes; and it should be bounded before the runtime meets a second title. But it is not
where the megabytes are in *this* title.

### 2.4 Where the megabytes actually are: 46 allocations of exactly 2 MB

This is the finding that reading the code does not predict.

At `--scale 1`, vmmap reports 100.6 MB of `owned unmapped (graphics)` across 98 regions. The size
histogram is: [M]

```
  46 x 2048K      <- 94,208 KB = 92.0 MB
   4 x 1296K      <- the offscreen colour, depth, readback and presenter image
   2 x 2912K      <- presentation-sized surfaces
  17 x 16K, 12 x 32K, 8 x 80K, 6 x 48K, 1 x 512K, 1 x 272K, 1 x 160K
```

**46 allocations of exactly 2048 KB, for a run that decoded 48 textures.** The correlation is the
evidence, and it was confirmed by re-running at `--scale 4`, where the render targets grew from
4 x 1296 KB to 4 x 19.0 MB while **the 46 x 2048 KB block was unchanged**: [M]

```
  46 x 2048K      <- identical
   4 x 19.0M      <- the render targets, now 2560x1920
```

Texture count is the same at both scales (48), and texture allocations are the same at both scales.
Render-target size is the only thing that moved. The texture images are therefore the 46.

**The mechanism [I]:** `TextureCache::upload` calls `vkAllocateMemory` once per texture
(`texture_cache.cpp:286-296`) with `VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT`. On this host that is
MoltenVK, and each device-local `VkDeviceMemory` becomes its own Metal backing store; the observed
quantum is 2 MB. I did not confirm the exact Metal rule, and there are two reasons to be careful
about it: the 16 KB / 32 KB / 80 KB regions in the same histogram prove there is no universal 2 MB
minimum, and the offscreen attachments go through the *same* `vkCreateImage` +
`vkGetImageMemoryRequirements` + `vkAllocateMemory(DEVICE_LOCAL)` sequence
(`render/src/vk/resources.cpp:108-126`) yet get size-proportional allocations. The difference is
presumably usage — `SAMPLED|TRANSFER_DST` versus `COLOR_ATTACHMENT`/`DEPTH_STENCIL_ATTACHMENT`.
Section 9 says how to settle it. **It does not affect the size of the prize**, which section 2.5
establishes independently of the mechanism.

### 2.5 How big the textures actually are: 0.82 MB, total

The 46 identical allocations are on their own strong evidence of a quantum, because real textures
are *not* all the same size. To prove it, I decoded the texture size out of the title's own display
lists rather than trusting any inference.

`--dump-ta` writes a render's TA parameter stream. The size lives in the TSP instruction word:
`render/src/texture.cpp:41-46` and `:158-159`,

```cpp
    out.width = 8u << tsp_tex_u(tsp);
    out.height = 8u << tsp_tex_v(tsp);
```

so a three-bit field each way, maximum `8 << 7` = 1024. Parsing 64 captured frames for textured
polygon and sprite headers and taking distinct (size, TCW address) pairs gives **34 distinct
textures**: [M]

| Size | Distinct addresses | RGBA8888 each |
|---|---|---|
| 256x64 | 1 | 64 KB |
| 128x128 | 6 | 64 KB |
| 128x64, 256x32, 512x16 | 2, 1, 3 | 32 KB |
| 128x32, 64x64, 256x16 | 1, 9, 1 | 16 KB |
| 64x32, 128x16 | 1, 1 | 8 KB |
| 32x32 | 1 | 4 KB |
| 16x16, 8x32 | 1, 6 | 1 KB |
| **Total** | **34** | **863,232 bytes (0.82 MB)** |

**The largest texture in Crazy Taxi's attract mode is 64 KB decoded, and all of them together are
0.82 MB.** They occupy 92 MB. That is a **112x amplification**, and it is pure allocator overhead —
not one byte of pixel data is duplicated by it.

Thirteen distinct sizes spanning 1 KB to 64 KB, and forty-six allocations of identically 2048 KB.
Whatever the Metal mechanism turns out to be, the size of the waste is now measured, not inferred.

---

## 3. The translated code: ~9 MB mapped, ~0.7 MB of it dirty

`build-release/games/crazytaxi/crazytaxi_boot` is 9,267,344 bytes on disk. `size -m` gives the
segments:

| Segment | Size | Notes |
|---|---|---|
| `__TEXT` | 8,224,768 | of which `__text` 7,455,532 and `__const` 678,184 |
| `__DATA_CONST` | 180,224 | `__got` 2,176, `__const` 166,752 |
| `__DATA` | 540,672 | `__data` 80, `__bss` 524,473 (zerofill) |
| `__LINKEDIT` | 851,968 | |

vmmap on the live process shows what is actually resident: [M]

```
__TEXT       102654000-102e2c000  [ 8032K  3728K     0K     0K] r-x/r-x SM=COW   crazytaxi_boot
__DATA_CONST 102e2c000-102e58000  [  176K   176K     0K     0K] r--/rw- SM=COW   crazytaxi_boot
__LINKEDIT   102edc000-102fac000  [  832K    16K     0K     0K] r--/r-- SM=COW   crazytaxi_boot
__DATA       102e58000-102e5c000  [   16K    16K    16K     0K] rw-/rw- SM=COW   crazytaxi_boot
__DATA       102e5c000-102edc000  [  512K   512K   512K     0K] rw-/rw- SM=PRV   crazytaxi_boot
```

So: **3.7 MB of the 8 MB of translated text is ever touched** in a 30-second attract run, and it is
clean, file-backed, evictable under pressure. Only 528 KB is dirty — the `__bss` and `__data`. The
emitted C++ in `build-release/games/crazytaxi/gen/` is 31 MB of source across 24 files; none of that
is in the process.

The binary is therefore worth about **0.5 MB of dirty memory and 3.7 MB of shared clean pages**. It
is not a target. Its contribution to the RSS number is real but it is the cheapest kind of memory
there is.

One small duplicate: `dream::sh4::register_functions` (`runtime/src/sh4/abi_bare.cpp:34-48`) runs at
static-init time and copies the emitted `FunctionEntry` table into a sorted `std::vector`, measured
at 180,224 bytes of heap. [M] The source table is already in `__DATA_CONST`. 176 KB — noted for
completeness, not worth acting on.

---

## 4. The runtime subsystems: 9.5 MB non-guest heap, and two items in it are dev-flavoured

The full headless heap, attributed by `malloc_history -allBySize` on a live process — this is a
complete accounting of 38.87 MB across 1,672 live nodes: [M]

| Bytes | Attributed to | Verdict |
|---|---|---|
| 16,793,600 | `DcMemory::DcMemory()` | guest RAM |
| 8,404,992 | `DcMemory::DcMemory()` | guest VRAM |
| **6,602,752** | **`open_disc` -> `chd_open_core_file_callbacks` -> `decompress_v5_map`** | **the CHD hunk map — section 4.1** |
| 2,113,536 | `DcMemory::DcMemory()` | guest sound RAM |
| 2,113,536 | `DcMemory::DcMemory()` | guest boot ROM |
| **2,113,536** | **`main` -> `vector<char>` from `istreambuf_iterator`** | **a full copy of `1ST_READ.BIN` — section 4.2** |
| 180,224 | `dream::sh4::register_functions` | the function table copy, section 3 |
| 147,456 | `DcMemory::DcMemory()` | guest flash |
| 98,304 x2 | `cdzs_codec_init` -> `ZSTD_createDStream` | CHD decompression contexts |
| 81,920 | `pvr::Ta::feed` vector growth | the TA parameter stream, section 4.4 |
| 49,152 | `main` | |
| 16,384 | `aica::Mixer::Mixer` | section 4.3 |
| 10,240 | `DcMemory::DcMemory()` | guest on-chip RAM |
| 8,544 (178 nodes) | `arm_pcs` hash inserts from `aica.on_sample` | section 4.5 |
| ~0.3 MB | libobjc/libxpc/CoreFoundation static init | unavoidable on macOS |

Guest total 29.58 MB; non-guest total 9.29 MB. Nothing else in the process exceeds 100 KB.

### 4.1 The CHD hunk map: 6.3 MB, the largest non-guest allocation

`runtime/src/gdrom/disc.cpp:330` calls `chd_open`, and libchdr's `decompress_v5_map` builds the
full hunk map for the disc in memory: **6,602,752 bytes**, held for the whole run. [M]

The disc data itself is *not* buffered — `disc.cpp:213` sizes `hunk_buf_` to one hunk, and
`frame_ptr` (`disc.cpp:323-332`) decompresses one hunk at a time into it with a single-entry cache
(`cached_hunk_`). That part is well behaved. It is the map — one entry per hunk for a 1 GB GD-ROM —
that is large.

This is 17% of the headless footprint and third-largest allocation in the process. It is third-party
code (`third_party/libchdr`, BSD-3, linked at `runtime/CMakeLists.txt:29-31`), so the fix is either a
libchdr option, a lazily-paged map, or not using CHD for shipping.

### 4.2 A full copy of the game binary, kept for a diagnostic

`runtime/boot/boot_main.cpp:1322`:

```cpp
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
```

`1ST_READ.BIN` is 1,468,208 bytes; the vector's doubling growth lands it at 2,113,536 bytes of
heap. [M] It is memcpy'd into guest RAM at `:1330-1331` — and then **kept alive for the entire
run**, because the end-of-run report compares RAM against it: `boot_main.cpp:807-815` reports "image
bytes changed in RAM", and `suggest_relocations` (`:971` onward, with its 16-byte-window index at
`:978`) scans RAM for runs that reproduce image bytes in order to print `[[relocations]]` blocks.

That is a translator-development feature. It is on unconditionally in the release build — the
relocation suggestions appear in the release run's own output, quoted in the appendix. 2.1 MB.

### 4.3 AICA: 16 KB of state, and the tables are in `__TEXT`

The mixer's lookup tables are `static constexpr` arrays in `runtime/src/aica/mixer_tables.inc` —
`kVolumeLut[16]`, `kTlLut[1024]` and the rest, 239 lines generated by `tools/aica/gen_tables.py`.
Being `constexpr`, they land in the binary's `__const`, not the heap: clean, shared, file-backed.

Live AICA state is small and all inline:

- `std::array<std::uint8_t, 0x8000> regs_` — 32 KB, `runtime/include/dream/runtime/aica/aica.h:62`;
- the DSP's `TEMP[128]`, `MEMS[32]`, `MIXS[16]` `std::int32_t` arrays —
  `runtime/include/dream/runtime/aica/mixer.h:58-60`, 704 bytes;
- `timer_step_`/`timer_left_`, `aica.h:85`.

The only AICA heap allocation measured is a single 16,384-byte block from `Mixer::Mixer`. [M]
**Sound RAM is already counted in section 1 and there is no second copy of it.** ARM7 state is
register-file-sized; there are no per-channel sample buffers.

Confirmed by measurement at the process level: headless with audio is 75,628,544 bytes RSS versus
75,612,160 without — a 16 KB difference. [M] Audio costs essentially nothing in memory.

### 4.4 The scheduler, the fault log, `last_stream`, and replay

- **Fault log.** Bounded by construction: `FaultLog::kMaxUnique = 4096`
  (`runtime/include/dream/runtime/mem/dc_memory.h:79`), with a `dropped_` counter past it
  (`:88`). Ceiling ~4096 x 24 bytes = 98 KB. [I] In this run the log is empty — the release run
  reports `unmapped memory accesses: 0`. [M]
- **`last_stream`.** `boot_main.cpp:636` declares `std::vector<std::uint32_t> last_stream` and
  `:158` copies the whole display list into it every frame — `last_stream = stream;  // kept so F11
  can write the list that drew what is on screen`. The stream measured 81,920 bytes at its high
  water mark (the same allocation is visible growing under `pvr::Ta::feed`). [M] **80 KB, and a
  ~30 KB memcpy per frame.** The memory is negligible; the per-frame copy for a key nobody pressed
  is the part worth noting.
- **Replay / golden traces.** `runtime/src/devinterp/replay.cpp` is compiled only under
  `DREAM_DEV_INTERPRETER` (`runtime/CMakeLists.txt:36-39`). Not in this binary — see section 5.
- **Scheduler.** A small vector of named callbacks; below the 100 KB measurement floor.

### 4.5 `arm_pcs`: an unbounded profiling histogram in the release audio path

`runtime/boot/boot_main.cpp:1616`:

```cpp
    std::unordered_map<std::uint32_t, std::uint64_t> arm_pcs;
```

and `:1640-1641`, inside the 44,100 Hz sample callback:

```cpp
    aica.on_sample = [&](std::int16_t l, std::int16_t r) {
        ++arm_pcs[aica.arm.next_pc()];
```

An ARM7 program-counter histogram, incremented **once per audio sample**, unconditionally, in
release. It is bounded in practice by the number of distinct PCs in the title's sound driver
(AICADRV.BIN is 57,216 bytes), and it measured 178 nodes / 8,544 bytes at six seconds. [M] So the
memory is trivial. What it costs is a hash lookup and insert 44,100 times a second in release, for
a profile that is printed once and read by nobody in a shipping run. It belongs behind a flag.

---

## 5. What is dev-only, and what survived into release

`build-release/CMakeCache.txt` confirms `DREAM_DEV_INTERPRETER:BOOL=OFF` and
`CMAKE_CXX_FLAGS_RELEASE:STRING=-O3 -DNDEBUG`. [M]

**What the flag actually removes** — `runtime/CMakeLists.txt:35-40`:

```cmake
# WP2.7 / ADR 2: the interpreter fallback exists only in the development configuration. It reuses
# the translator's decoder; the release runtime never links it.
if(DREAM_DEV_INTERPRETER)
  target_sources(dream_runtime PRIVATE src/devinterp/interpreter.cpp src/devinterp/replay.cpp)
  target_link_libraries(dream_runtime PRIVATE dream::sh4dec)
endif()
```

So it removes the interpreter, the replay machinery, **and the link against `dream::sh4dec`** —
which is where the SH-4 decode tables live. **Verified by symbol scan:** the release binary contains
no `dream::devinterp` symbols at all; the only two matches for "interp" are the knobs that set the
range, and they are local stubs: [M]

```
0000000100694634 t __ZN5dream3sh419set_interpret_rangeEjj
000000010069464c t __ZN5dream3sh423set_interpret_functionsERKNSt3__16vectorINS1_4pairIjjEENS1_9allocatorIS4_EEEE
```

**The flag does its job.** No decode tables, no trace buffers, no replay in the release binary.

**But dev-flavoured things survive by other routes**, because they were never behind the flag:

| Item | Where | Cost |
|---|---|---|
| The `1ST_READ.BIN` copy kept for the relocation report | `boot_main.cpp:1322`, used at `:807`, `:971` | 2.1 MB [M] |
| `arm_pcs` PC histogram in the audio callback | `boot_main.cpp:1616`, `:1641` | 8.5 KB + 44.1 kHz of hash inserts [M] |
| `last_stream` display-list copy every frame | `boot_main.cpp:158`, `:636` | 80 KB + ~30 KB/frame memcpy [M] |
| Write-hash / journal / watch machinery | `dc_memory.h:116-152` | Flags default off; `std::vector<WriteRecord> journal` is an empty vector (24 bytes) |
| `symbols.tsv` | `games/crazytaxi/symbols.tsv`, 11,766 bytes | **Translator-side only** — `load_symbols` is called from `translator/src/main.cpp:206,347,479`, never from the runtime. Not in the binary. |

The write-hash and journal machinery in `DcMemory` is the well-behaved case: the fields are there
but nothing is allocated when they are off, and `note_write` is one predictable branch, exactly as
its comment at `dc_memory.h:120-123` claims.

---

## 6. The measured breakdown

### 6.1 The matrix

All runs: `--config games/crazytaxi/crazytaxi.toml`, 30 guest-seconds of attract mode unless
noted, Apple M4, release build. "RSS" is `/usr/bin/time -l`'s maximum resident set size; "Footprint"
is its peak memory footprint (`phys_footprint`). [M]

| Run | RSS | Footprint | Delta vs. previous row |
|---|---|---|---|
| headless, `--no-audio`, 3 s | 74.2 MB | 63.8 MB | |
| headless, `--no-audio`, 10 s | 75.6 MB | 64.5 MB | +1.4 MB (warm-up, then flat) |
| headless, `--no-audio`, 30 s | 75.6 MB | 64.5 MB | **+0.0 MB** |
| headless, `--no-audio`, 60 s | 75.6 MB | 64.5 MB | **+0.0 MB** |
| headless, audio, 30 s | 75.6 MB | 64.5 MB | +0.02 MB for audio |
| `--window --scale 1 --no-audio` | 141.5 MB | 192.8 MB | **+65.9 MB for the window** |
| `--window --scale 1` | 146.1 MB | 198.8 MB | +4.6 MB for audio with a window (SDL device) |
| `--window --scale 2` | 165.4 MB | 205.6 MB | +19.3 MB |
| `--window --scale 4` | 181.8 MB | 283.2 MB | +16.4 MB RSS / +77.6 MB footprint |

Two things this table says that the brief's single number cannot:

- **Duration does not matter.** 3 s to 60 s headless moves the footprint by 0.7 MB and then stops
  dead. Nothing in the runtime grows without bound on a timescale that matters. [M]
- **Audio is free.** 16 KB headless, ~4.6 MB with a window (that is SDL3's playback device and its
  CoreAudio buffers, not our mixer). [M]

The brief's baseline of 163 MB sits between my scale-1 (146 MB) and scale-2 (165 MB) figures. I
could not reproduce 163 MB exactly at scale 1; run-to-run variation, window size, and how much of the
attract loop is reached all move it by several MB. The *shape* of the answer does not depend on
which of 146 or 163 is right.

### 6.2 RSS is the wrong metric, and by how much

vmmap on a live headless process, 6 seconds in: [M]

```
Physical footprint:         39.1M
Writable regions: Total=127.9M written=38.2M(30%) resident=38.2M(30%)
```

Against a `/usr/bin/time` RSS of 75.6 MB for the same workload. The 36 MB gap is resident pages of
`__TEXT`, `__LINKEDIT`, `__OBJC_RO` and `__DATA_CONST` belonging to **system dylibs, shared
copy-on-write with every other process on the machine**: vmmap counts 260 MB of `__TEXT` resident
across 539 regions, almost all of it libSystem, CoreFoundation and friends.

**Headless, dream-recomp's own memory is 39.1 MB, not 75.6 MB.** [M]

With a window the relationship inverts, because macOS charges GPU allocations to `phys_footprint`
but not to RSS. At scale 4: RSS 181.8 MB, footprint 283.2 MB. **Footprint is the metric to report**
— it is the one that includes the texture images, which are the whole story.

### 6.3 The windowed breakdown, scale 1

vmmap summary on a live `--window --scale 1` process, 25 seconds in. Footprint 176.6 MB. [M]

| Region | Dirty | What it is |
|---|---|---|
| **`owned unmapped (graphics)`** | **100.6 MB** | Metal allocations. **92 MB of it is 46 x 2048 KB — the texture images (section 2.4).** The rest is the render targets. |
| `MALLOC_LARGE` | 30.3 MB | Guest RAM + VRAM + the CHD map (sections 1, 4.1) |
| `MALLOC_SMALL` | 18.4 MB | Everything else on our heap plus MoltenVK/SDL/Metal host-side state |
| `IOSurface` | 8.2 MB | Window surfaces |
| `IOAccelerator (graphics)` | 7.6 MB | Swapchain images |
| `__DATA` / `__DATA_CONST` / `__DATA_DIRTY` / `__AUTH*` (all libs) | ~5.1 MB | Framework static data |
| `owned unmapped` (non-graphics) | 1.4 MB | |
| MALLOC metadata, stacks, QuartzCore, CoreAnimation, dispatch, misc | ~3 MB | |

Our own malloc zone totals **49.4 MB dirty** windowed against **38.9 MB headless** — so the Vulkan
loader, MoltenVK and SDL3 add about **10.5 MB of host-side state**. [M]

### 6.4 The reconciliation

Windowed, `--scale 1`, footprint 176.6 MB measured. Reading predicted:

| Component | Predicted [I] | Measured [M] | Agreement |
|---|---|---|---|
| Guest memory | 28.1 MB | 29.6 MB | yes (malloc rounding) |
| Offscreen colour + depth + readback + presenter image | 4.9 MB | 5.2 MB (4 x 1296 KB) | yes |
| Texture images + staging, 48 entries | ~1-2 MB | **92 MB** | **no — 100x over** |
| Binary, dirty | 0.5 MB | 0.5 MB | yes |
| CHD map | not predicted | 6.3 MB | **missed by reading** |
| Game image copy | not predicted | 2.1 MB | **missed by reading** |
| Vulkan/MoltenVK/SDL host state | not predicted | 10.5 MB | missed by reading |
| Swapchain + IOSurface | ~7 MB | 15.8 MB | roughly |

**The two disagreements are the two findings.** Reading `texture_cache.cpp` predicts a couple of
megabytes for 48 textures — and the title's own display lists confirm 0.82 MB is the true figure
(section 2.5) — yet it costs 92 MB, because the code makes no statement about allocator granularity
and the granularity is the whole cost. And reading `disc.cpp` predicts a *streaming* disc reader,
which it is — while missing that libchdr's index for a GD-ROM is 6.3 MB on its own.

---

## 7. What the irreducible floor actually is

| Component | MB | Reducible? |
|---|---|---|
| Guest RAM, VRAM, sound RAM, boot ROM, flash, on-chip | 28.1 | No — section 1.4 |
| Binary dirty data (`__bss`, `__data`) | 0.5 | No |
| Binary text actually executed (clean, shared, evictable) | 3.7 | No, and cheap |
| Offscreen colour + depth at 640x480 | 2.5 | No (the renderer must have a target) |
| Vulkan loader + MoltenVK + SDL3 + Metal driver host state | ~10.5 | Not by us |
| Swapchain + IOSurface | ~15.8 | Marginally (image count) |
| Texture images, at their true size (section 2.5) | 0.8 | No |
| **Floor with a window at 640x480** | **~62** | |
| **Floor headless** | **~33** | |

Against a measured 176.6 MB windowed footprint (146.1 MB RSS), that leaves roughly **115 MB
addressable**, of which 92 MB is one problem.

A caveat I want to state plainly: ~26 MB of that "floor" (the driver host state, the swapchain, the
IOSurfaces) is the price of having a Vulkan window on macOS via MoltenVK. On a native-Vulkan host it
would be different, probably smaller. I measured one platform.

---

## 8. Reduction opportunities, ranked

Dev estimates are in **focused engineer-days**, the convention of `docs/implementation-plan.md:8-10`
— a full day of uninterrupted work by someone comfortable in C++ and the codebase. Low / central /
high.

### 8.1 Pool the texture cache's device allocations — **~90 MB**, 3 / 4 / 7 days

**The change.** Stop calling `vkAllocateMemory` once per texture. Suballocate texture images from a
small number of large device-local allocations — either by hand (a simple linear/buddy allocator over
16 MB blocks) or by adopting VulkanMemoryAllocator, which exists precisely for this and is what
every production Vulkan renderer does.

**Files.** `render/src/vk/texture_cache.cpp:286-300` (the allocation), `render/src/vk/resources.cpp`
(where a `DeviceAllocator` would live alongside `HostBuffer`),
`render/include/dream/render/vk/resources.h`, `render/include/dream/render/vk/texture_cache.h`
(`Entry::memory` becomes an allocation handle), `free_entry` at `texture_cache.cpp:65-75`.

**Estimated saving.** 92 MB measured, minus what the textures genuinely need, which section 2.5
measures at **0.82 MB**. Allow one or two 16 MB pooled blocks' worth of slack and rounding and the
result is **~90 MB central, 85 low, 91 high.** This is 51% of the windowed footprint at scale 1 and
it is the single largest item in the process by a factor of three. [I, from a measured base]

**Risk.** Moderate and well understood. The failure mode of a hand-rolled suballocator is
image-aliasing corruption, which is visible immediately and testable. VMA is a third-party
dependency and an ADR question (it is MIT, so licence-compatible with the repo's posture), but it is
also 4 days of risk removed. The interaction with `invalidate_range` (`texture_cache.cpp:89-113`)
matters: freeing a suballocation must not disturb neighbours, and the existing
`vkDeviceWaitIdle`-before-free discipline at `:105` must be preserved.

**How to verify.** Re-run the appendix's vmmap recipe at `--scale 1` and `--scale 4`. Success is the
`owned unmapped (graphics)` histogram losing its 46 x 2048 KB block and gaining a handful of large
blocks whose total is under 15 MB, with the 4 render-target regions unchanged. Compare a screenshot
(`--screenshot-at`) before and after for pixel identity.

### 8.2 Free the texture staging buffer after upload — **~0.8 MB**, 0.5 / 1 / 2 days

**The change.** `Entry::staging` (`texture_cache.h:137`) is a host-visible, permanently mapped copy
of every texture's pixels, written once at `texture_cache.cpp:263-265` and read once by the
`vkCmdCopyBufferToImage` at `:189-191`. Nothing reads it again. Destroy it once the upload's
command buffer has completed — or, better, share one growable staging buffer across all uploads,
which is what `HostBuffer::ensure`'s grow-and-keep design (`resources.cpp:25-30`) already wants to
be used for.

**Files.** `render/include/dream/render/vk/texture_cache.h:137`,
`render/src/vk/texture_cache.cpp:259-266` and `:212-260` (`get`, where the upload is sequenced).

**Estimated saving.** One duplicate of the 0.82 MB of texture pixels (section 2.5), so **~0.8 MB.**
[I, from a measured base]

Host-visible buffers demonstrably do *not* carry the images' 2 MB granularity, which is worth
stating because it was my first hypothesis and it is wrong: if the 48 staging buffers cost 2 MB
each, the graphics total at scale 1 would be ~190 MB rather than the 100.6 MB measured. The
arithmetic rules it out. So this is a small, tidy change and not a second big win.

**Risk.** Low, but there is a real hazard: the upload command buffer is recorded, not executed, when
`get()` returns. Freeing the staging buffer before the GPU has consumed it corrupts the texture.
This needs the existing fence discipline (`offscreen.cpp:113-118`) extended to uploads, or a
deferred-free list drained a frame later.

**How to verify.** Same vmmap recipe; the count of small graphics regions should fall. Plus a
validation-layer run (`--validation`) to prove no use-after-free of the buffer.

### 8.3 Drop the CHD hunk map, or the CHD dependency, at run time — **~6.3 MB**, 1 / 3 / 6 days

**The change.** libchdr's `decompress_v5_map` builds the entire hunk map eagerly
(`third_party/libchdr`, reached from `runtime/src/gdrom/disc.cpp:330`), costing 6,602,752 bytes [M].
Options, cheapest first: (a) check whether a newer libchdr has a lazy-map option; (b) page the map
in blocks, keeping a fixed-size window; (c) support a plain GDI/track-file disc source for shipping
and keep CHD as the archival format — `open_disc` (`disc.cpp:343` onward) already dispatches on
extension, so a second backend is a natural fit.

**Files.** `runtime/src/gdrom/disc.cpp:320-345`, `third_party/libchdr` (a patch we would carry),
`runtime/CMakeLists.txt:28-31`.

**Estimated saving.** 6.3 MB measured. 16% of the headless footprint.

**Risk.** Low for (c), higher for (b) — a patched third-party decoder is a maintenance tail, and the
repo's posture on `third_party` (BSD-3, vendored) makes carrying a patch awkward. (a) is a
half-day of investigation that might make the rest moot.

**How to verify.** `malloc_history -allBySize` should no longer show a ~6.6 MB `decompress_v5_map`
entry; headless footprint should fall from 39.1 MB to ~32.8 MB.

### 8.4 Release the game-image copy after boot — **~2.1 MB**, 0.5 / 1 / 2 days

**The change.** `boot_main.cpp:1322`'s `std::vector<char> bytes` is kept only so the end-of-run
report (`:807-815`) and `suggest_relocations` (`:971`) can diff RAM against the image. Put both
behind the same flag that turns the report on, and free the vector after the memcpy at `:1330` when
the flag is off.

**Files.** `runtime/boot/boot_main.cpp:1322`, `:1330`, `:689` (the report's signature takes
`const std::vector<char>& image`), `:807-815`, `:875-930`, `:971-1010`.

**Estimated saving.** 2.1 MB measured.

**Risk.** Very low. The only risk is removing a diagnostic somebody relies on by default — which is
why the recommendation is a flag, not a deletion. Note the relocation suggestions currently print on
every release run; making them opt-in is arguably the point.

**How to verify.** `malloc_history` loses the 2,113,536-byte `istreambuf_iterator` entry; the
release run's stdout no longer ends with `[[relocations]]` blocks unless asked.

### 8.5 Bound the texture cache — **0 MB today, unbounded tomorrow**, 1 / 2 / 4 days

**The change.** Add a byte budget and an LRU to `TextureCache`. Track bytes in `entries_`, evict
least-recently-used past a configurable ceiling (128 MB is generous), and handle the
descriptor-pool-exhausted path at `texture_cache.cpp:249-253` by evicting rather than by inserting a
permanently broken entry.

**Files.** `render/include/dream/render/vk/texture_cache.h:127-146`,
`render/src/vk/texture_cache.cpp:212-270`.

**Estimated saving.** **Zero for Crazy Taxi** — 48 entries, never evicted, never near the 2048
ceiling. This is insurance for the second title, not a saving for the first. It is on the list
because "unbounded texture cache" was the brief's prior hypothesis and the honest answer is: the
cache *is* unbounded, and that is a latent defect, and it is not where the 163 MB is.

**Risk.** Low, with one real hazard: evicting a texture that the frame in flight still references.
Needs the same deferred-free discipline as 8.2.

**How to verify.** A synthetic test that feeds more than 2048 distinct TCW/TSP pairs and asserts the
cache's byte total stays under budget and `error()` stays empty.

### 8.6 Make `arm_pcs` and `last_stream` opt-in — **~0.1 MB**, 0.5 / 0.5 / 1 day

**The change.** Guard `boot_main.cpp:1641`'s `++arm_pcs[...]` on a flag (it already only matters for
the profile printed at `:1915`-ish), and make `:158`'s `last_stream = stream` conditional on the
capture feature being armed.

**Files.** `runtime/boot/boot_main.cpp:158`, `:636`, `:1616`, `:1641`.

**Estimated saving.** ~90 KB. **This one is not about memory** — it is about removing a 44.1 kHz
hash insert and a ~30 KB per-frame memcpy from the release hot path. Listed here because it is the
same class of finding.

**Risk.** Negligible. F11's capture must still work when armed.

### 8.7 Reduce the swapchain image count — **~5 MB at scale 1**, 0.5 / 1 / 2 days

**The change.** `render/src/vk/window.cpp:142` asks for `caps.minImageCount + 1` (three here). Two
is legal and enough for a title that is not CPU-bound at present.

**Files.** `render/src/vk/window.cpp:142-169`.

**Estimated saving.** One swapchain image, ~2.9 MB at scale 1 and ~19 MB at scale 4. [I]

**Risk.** Real and not worth it at scale 1. Dropping to two images can cost frame pacing on a
mailbox/FIFO presentation path, and the repo has just spent commits on presentation quality
(`6f4ea6f Present: average the supersamples instead of throwing them away`). **Recommended only at
scale 4, if at all**, and only after measuring frame times.

### 8.8 Summary

| # | Opportunity | Saving | Days (L/C/H) | Confidence |
|---|---|---|---|---|
| 8.1 | Pool texture device allocations | **~90 MB** | 3 / **4** / 7 | High — 92 MB measured, 0.82 MB of real pixels measured |
| 8.2 | Free texture staging after upload | ~0.8 MB | 0.5 / **1** / 2 | High — bounded by §2.5 |
| 8.3 | Drop the CHD hunk map | **6.3 MB** | 1 / **3** / 6 | High — measured |
| 8.4 | Release the game-image copy | **2.1 MB** | 0.5 / **1** / 2 | High — measured |
| 8.5 | Bound the texture cache | 0 now | 1 / **2** / 4 | High — it is insurance |
| 8.6 | `arm_pcs` / `last_stream` opt-in | ~0.1 MB | 0.5 / **0.5** / 1 | High |
| 8.7 | Fewer swapchain images | ~2.9 MB | 0.5 / **1** / 2 | Not recommended at scale 1 |
| | **8.1 + 8.3 + 8.4 together** | **~98 MB** | **8 days central** | |

Doing 8.1, 8.3 and 8.4 takes the windowed scale-1 footprint from a measured 176.6 MB to about
**79 MB** [I], and the headless footprint from 39.1 MB to about **30.7 MB** — within 2.6 MB of the
guest's own 28.1 MB floor. 8.1 alone is worth more than everything else on the list combined by a
factor of ten, and it should be done first and possibly alone.

---

## 9. What I could not determine, and how to find out

1. **Why a device-local image allocation costs exactly 2 MB on this host.** That it *is* a quantum
   rather than the textures' real size is now settled (section 2.5: the largest texture is 64 KB and
   the whole set is 0.82 MB, against 46 identical 2048 KB allocations). What is *not* settled is the
   Metal-side rule, and the puzzle is sharp: the offscreen attachments take the identical
   `vkCreateImage` / `vkGetImageMemoryRequirements` / `vkAllocateMemory(DEVICE_LOCAL)` path at
   `render/src/vk/resources.cpp:108-126` and get size-proportional allocations instead. *How to find
   out:* build a 30-line Vulkan program that allocates N images of varying size and usage flags and
   watches `phys_footprint`; or read MoltenVK's `MVKDeviceMemory` / `MVKImage` allocation path. Half
   a day. **It does not change 8.1's estimate** — 92 MB in, 0.82 MB of pixels, both measured — but
   it decides whether the fix is "pool the allocations" or "stop asking for a dedicated allocation",
   and the second is cheaper.

2. **Whether the textures grow in gameplay.** Section 2.5 covers attract mode: 34 textures, none
   above 64 KB. The city almost certainly uses more and larger ones, and the per-texture 2 MB cost
   means the footprint grows in 2 MB steps regardless. *How to find out:* the same TA-dump parse
   from the appendix, run over a captured gameplay session — which needs item 4's replay.

3. **Whether the footprint behaves the same on a native-Vulkan host.** Everything here is macOS +
   MoltenVK + Metal. The 46 x 2 MB effect may be entirely a MoltenVK artefact, in which case the
   Linux/Windows footprint is already close to the floor and 8.1's urgency drops there (though not
   on macOS, which is the platform this build targets). *How to find out:* run the
   vmmap-equivalent (`/proc/self/smaps_rollup`, or Valgrind massif) on the Linux build. One day, and
   it needs a Linux build to exist.

4. **How the footprint behaves in gameplay rather than attract mode.** Attract mode decoded 48
   textures. A drive through the city will decode far more, and that is where the *unbounded* cache
   (8.5) stops being theoretical and the texture-allocation multiplier (8.1) compounds. *How to
   find out:* a scripted input replay through one fare, with the same vmmap recipe at 30 s
   intervals. Two days, mostly building the replay.

5. **Whether `--scale 4`'s 283 MB is acceptable at all.** At scale 4 the render targets alone are
   4 x 19 MB plus 2 x 18.8 MB of swapchain, and that is before 8.1's 92 MB. The readback buffer in
   particular (`offscreen.cpp:119`) exists to hand the presenter a CPU copy; with the presenter's
   own staging buffer that is three full-resolution copies of every frame. *How to find out:* read
   `present.cpp:292-340` against `offscreen.cpp:185-192` and establish whether a GPU-to-GPU blit
   could replace the CPU round trip. One day of reading; the change itself is larger and belongs
   with the rendering-enhancements work in `docs/rendering-enhancements-study.md`.

6. **Exact `MALLOC_SMALL` attribution windowed.** 18.4 MB dirty, against 6.7 MB headless.
   `malloc_history` on the windowed process would name the 11.7 MB difference; I attributed it to
   MoltenVK/SDL by subtraction, not by name. *How to find out:* run the appendix's
   `MallocStackLogging` recipe with `--window`. An hour.

7. **Whether 163 MB or 146 MB is the right baseline.** I could not reproduce 163 MB at scale 1
   (I measured 146.1 MB RSS / 198.8 MB footprint) and it sits between my scale-1 and scale-2
   figures. *How to find out:* pin the window size and the number of attract-loop iterations, and
   report footprint rather than RSS from here on. Trivial, and worth doing before anyone measures a
   saving against the wrong base.

---

## Appendix: reproducing every measurement

```sh
cd /Users/Daniel.Joyce/funrepos/dream-recomp
B=./build-release/games/crazytaxi/crazytaxi_boot
C=games/crazytaxi/crazytaxi.toml

# --- The matrix (section 6.1). Headless runs are faster than real time; windowed are not.
/usr/bin/time -l $B --config $C --max-seconds 30 --no-audio
/usr/bin/time -l $B --config $C --max-seconds 30
/usr/bin/time -l $B --config $C --max-seconds 60 --no-audio
/usr/bin/time -l $B --config $C --max-seconds 30 --window --scale 1
/usr/bin/time -l $B --config $C --max-seconds 30 --window --scale 2
/usr/bin/time -l $B --config $C --max-seconds 30 --window --scale 4

# --- Live region attribution (sections 6.2, 6.3, 2.4).
$B --config $C --max-seconds 600 --window --scale 1 & PID=$!
sleep 25
vmmap -summary $PID
vmmap $PID > /tmp/vm.txt
kill $PID
# The finding, in one line:
grep "owned unmapped (graphics)" /tmp/vm.txt | sed 's/.*\[ *//' | awk '{print $1}' | sort | uniq -c | sort -rn
#   46 2048K     <- one per cached texture
#    4 1296K     <- the render targets at 640x480 (19.0M at --scale 4)

# --- Heap attribution by call site (sections 1.1, 4).
MallocStackLogging=1 $B --config $C --max-seconds 600 --no-audio & PID=$!
sleep 8
malloc_history $PID -allBySize | head -40
kill $PID
# Names DcMemory::DcMemory (x6), decompress_v5_map (6,602,752), the istreambuf_iterator
# vector<char> (2,113,536), register_functions (180,224), Mixer::Mixer (16,384).

# --- The binary (section 3).
size -m $B
otool -l $B | grep -A5 segname | grep -E 'segname|vmsize|filesize'

# --- Dev-only verification (section 5).
grep -i "DEV_INTERPRETER\|CMAKE_BUILD_TYPE" build-release/CMakeCache.txt
nm $B | grep -i interp          # only the two set_interpret_* stubs
nm $B | grep -ci devinterp      # 0

# --- The texture count, straight from the run's own report (section 2.3).
$B --config $C --max-seconds 30 --window --scale 1 2>&1 | grep '^window:'
# window: 1287 frames drawn ...; textures 48 decoded, 0 failed, 0 overwritten by a render,
#         1 palette changes

# --- How big the textures really are (section 2.5). Decoded from the title's own display
#     lists, so it depends on no assumption about the renderer or the driver.
$B --config $C --max-seconds 25 --no-audio --dump-ta /tmp/multi   # writes /tmp/multi.000 .. .063
python3 - <<'EOF'
import struct, glob, collections
seen = collections.Counter()
for f in sorted(glob.glob('/tmp/multi.*')):
    d = open(f, 'rb').read()
    w = struct.unpack('<%dI' % (len(d) // 4), d[:len(d) // 4 * 4])
    for i in range(0, len(w) - 7, 8):          # the TA stream is 32-byte chunks
        pcw = w[i]
        if ((pcw >> 29) & 7) in (4, 5) and ((pcw >> 3) & 1):   # polygon/sprite, textured
            tsp, tcw = w[i + 2], w[i + 3]
            # render/src/texture.cpp:41-46 and :158-159
            seen[(8 << ((tsp >> 3) & 7), 8 << (tsp & 7), tcw & 0x1FFFFF)] += 1
tot = sum(u * v * 4 for (u, v, a) in seen)
print('distinct textures:', len(seen))
print('total RGBA8888: %d bytes (%.2f MB)' % (tot, tot / 1048576))
print('largest single:  %d bytes' % max(u * v * 4 for (u, v, a) in seen))
EOF
# distinct textures: 34
# total RGBA8888: 863232 bytes (0.82 MB)
# largest single:  65536 bytes
```
