# 144 fps by frame generation, and anti-aliasing, for Crazy Taxi: what is achievable and what it costs (study)

Written 2026-09-14 from a reading pass over `docs/runtime-render.md`, `docs/runtime-interrupts.md`,
`docs/decisions/README.md`, `runtime/src/pvr/`, `runtime/src/sched/`, `runtime/boot/boot_main.cpp`,
`render/src/`, `render/shaders/`, `audio/`, the vendored Flycast tree at `build/flycast-src/`, and a
static analysis pass over the owner's `1ST_READ.BIN` (`games/crazytaxi/extracted/fs/1ST_READ.BIN`,
SHA-1 as pinned in `games/crazytaxi/crazytaxi.toml`). **No source was changed and no code was
written.** Nothing from the Katana SDK directory was opened; every Dreamcast API claim below is from
KallistiOS, from Flycast, or from disassembly of the owner's own disc.

Companion to `docs/widescreen-crazytaxi-study.md`. That study settled that vertices arrive
pre-projected from the guest, and located the viewport block and the 2D choke point. Both facts are
load-bearing here and are cited rather than re-derived; section 11.2 notes the shared code.

Throughout, **measured** means I read it out of this repository's code, this repository's own
recorded measurements, or the owner's binary. **Inferred** means I reasoned about it. The two are
labelled separately wherever the difference matters.

---

## 0. Verdicts

### A. 144 fps by renderer-side frame generation — feasible in principle, but this is a research
### project wearing an engineering project's clothes

The guest stays at 59.83 Hz; the renderer manufactures the frames in between. The brief is right
that this is the only acceptable shape (section 1.4 shows that running the guest faster would make
Crazy Taxi play at 2.4× speed and would also wreck the audio). The question is what the renderer has
to work with, and the answer is **less than any modern frame-generation technique assumes**:

- **Nothing in the pipeline correlates geometry between frame N and N+1.** The Tile Accelerator
  stream is a flat list of screen-space triangle strips with no object identity, no stable index, and
  no submission-order guarantee, and the renderer does not even *retain* the previous frame's
  decoded geometry — it calls `decoder.reset()` at the top of every render
  (`runtime/boot/boot_main.cpp:159`). Section 2 establishes this concretely. **It decides everything
  below.**
- **There are no motion vectors and none can be derived from the render path alone.** Section 4.1.
- **But the recompiler can recover camera motion from the guest**, because `Ctx::xf[16]` *is* the
  SH-4's XMTRX (`runtime/include/dream/runtime/sh4/ctx.h:37`,
  `runtime/include/dream/runtime/sh4/ops.h:350-362`), and Crazy Taxi transforms every vertex through
  it (`ftrv xmtrx,fv4` at `0x0C080F1C`; 150 `ftrv` instructions in the image). That is the one thing
  available here that is genuinely unavailable to a black-box emulator, and it is why option (b) is
  the recommendation.

**Recommended approach, in one sentence:** depth-aware **camera reprojection**, driven by a
per-frame snapshot of the guest's own XMTRX and viewport block taken through the `[hooks]` mechanism
the widescreen study already specifies, run as **forward extrapolation** rather than interpolation
so that no input latency is added, with the 2D/HUD layer excluded from the warp and re-composited
unwarped.

**What that costs:** **35 focused engineer-days central, range 17–75.** The spread is not
estimation slack; it is the honest difference between "camera-only reprojection of a mostly-static
city, shipped as an experimental toggle" and "traffic, pedestrians and your own taxi's hood do not
smear". Section 8 breaks it down and section 8.5 names a 13-day minimum honest slice.

**Explicitly not recommended:** geometry interpolation (needs the correspondence that does not
exist, section 4.1); image-space optical flow (no game data, worst artefacts, smears the HUD,
section 4.3); and interpolation-with-a-held-back-frame in any form, because this is a driving game
and 16.7 ms of added latency is a worse regression than the aliasing anyone would have fixed
(section 5).

### B. Anti-aliasing — do not build MSAA; the repo already has supersampling it is throwing away

**The single most useful finding in part B is a defect, not a design.** `--scale` genuinely
supersamples the geometry — `--scale 4` renders 2560×1920 (`runtime/boot/boot_main.cpp:129`) — and
then the presenter resolves it to the window with a **nearest-neighbour** sampler
(`render/src/vk/present.cpp:74-76`, with `smooth = false` at
`render/include/dream/render/vk/present.h:53` and never assigned anywhere in the tree) and no mip
chain (`render/src/vk/present.cpp:229`). At `--scale 4` into the default 960×720 window
(`boot_main.cpp:130`) roughly one rendered sample in seven reaches the screen and the rest is
discarded. **SSAA is paid for and then discarded at the last step.**

**Recommended approach, in one sentence:** fix the resolve (a proper box downsample in
`render/shaders/present.frag`, plus the linear sampler), then add **SMAA 1x** — or FXAA as the
cheap first cut — as a second pass in the same place, with the punch-through-heavy 2D layer in mind.

**What that costs:** **5 focused engineer-days central, range 3–10.**

**Explicitly not recommended: MSAA.** It conflicts structurally with the per-pixel
order-independent transparency this project has already accepted (ADR 9,
`docs/decisions/README.md:21`), it fights the fragment shader's `gl_FragDepth` write
(`render/shaders/geometry.frag:68`), it does nothing for the punch-through list that most of Crazy
Taxi's foliage and signage lives in, and **Flycast — the renderer this project is porting — has no
multisampling anywhere in its Vulkan backend**, which I checked: a `grep -rn` for `SAMPLE_COUNT` and `MSAA` over
`build/flycast-src/core/rend/` returns only the words "Rasterization and multisample states" in
comments. Section 9.1 has the argument.

**TAA/TSAA is not assessable as "expensive" — it is unavailable.** It needs motion vectors, and
section 4.1 shows that motion vectors cannot exist here without first solving the same
correspondence problem that sinks frame-generation option (a). If part A's recommended work lands,
TAA becomes *possible*; until then it is not a choice on the table.

---

## 1. The timing spine, and why the guest must stay at 59.83 Hz

### 1.1 One virtual clock, in SH-4 cycles

`runtime/include/dream/runtime/sched/scheduler.h:14`:

```cpp
constexpr std::uint64_t kSh4Clock = 200'000'000;
```

`docs/runtime-interrupts.md:5-8`: *"`sched::Scheduler` is the virtual clock in SH-4 cycles (200 MHz)
with a small event list … Nothing in the runtime reads a wall clock, so a run is a deterministic
function of the guest instruction stream."* Guest time is a function of instructions retired.

### 1.2 Where 59.83 Hz comes from, exactly

`runtime/src/pvr/spg.cpp:29-38`:

```cpp
void Spg::recalc() {
    const std::uint64_t pixel_clock = vclk_full_ ? kPixelClock : kPixelClock / 2;
    const std::uint64_t hcount = (spg_load & 0x3FFu) + 1;  // SPG_LOAD: hcount 9:0, vcount 25:16
    line_cycles_ = sched::kSh4Clock * hcount / pixel_clock;
```

with `kPixelClock = 27'000'000` (`runtime/include/dream/runtime/pvr/spg.h:23`) and the NTSC reset
value `spg_load = 0x01060359` (`runtime/src/pvr/spg.cpp:17`):

| quantity | value | source |
|---|---|---|
| hcount + 1 | 0x359 + 1 = **858** | `spg.cpp:31` |
| vcount + 1 (`lines()`) | 0x106 + 1 = **263** | `spg.h:31` |
| pixel clock | 13.5 MHz (`vclk_div` unset) | `spg.cpp:30` |
| `line_cycles_` | 200e6 × 858 / 13.5e6 = **12,711** | `spg.cpp:32` |
| `frame_cycles()` | 12,711 × 263 = **3,342,993** | `spg.h:33` |
| frame rate | 200e6 / 3,342,993 = **59.827 Hz** | derived |

`Spg::on_line` (`runtime/src/pvr/spg.cpp:40-73`) runs one scheduler event per scanline, raises
`VBlankIn`/`VBlankOut` on the lines `SPG_VBLANK_INT` names, fires the `on_vblank_out` hook
(`spg.cpp:50-51`) and increments `frames_` at scanline 0 (`spg.cpp:69-71`).

**What would break if it were raised.** Writing a smaller `spg_load` (or setting `vclk_div`) makes
the SPG fire vblank more often *on the guest clock*. Everything the guest paces off vblank then runs
faster in guest-seconds terms, and everything paced off the SH-4 cycle count — TMU
(`runtime/src/sh4/tmu.cpp`), AICA sample generation, GD-ROM and Maple timings — does not. That is
not "144 fps", it is a desynchronised console. This is why the brief's option (i) is off the table
independently of section 1.4.

### 1.3 The host pacer, and `--unthrottled`

`runtime/boot/boot_main.cpp:1571-1583`:

```cpp
            if (unthrottled)
                return;
            const auto guest =
                std::chrono::duration<double>(static_cast<double>(sys.ctx.cycles) / 200e6);
            const auto target =
                started + paused_for +
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(guest);
            const auto now = std::chrono::steady_clock::now();
            if (target > now && target - now < std::chrono::seconds(1))
                std::this_thread::sleep_for(target - now);
```

**This is a sleep and nothing else.** It slows a fast host down; it cannot make anything faster.
`--unthrottled` (`boot_main.cpp:1214-1215`, help at `:1049`) removes the sleep — which is the brief's
option (iii), and it is *already implemented*. It does not produce 144 fps of anything; it produces a
guest running as fast as the host allows, i.e. Crazy Taxi at 1.6–2× speed with chopped audio. Noted
and dismissed.

Two further properties of this block matter later (`docs/future-enhancements.md:128-140`,
found while studying the options UI, not by me): the deficit is uncapped, so after a host stall the
guest sprints; and **the swapchain hard-codes `VK_PRESENT_MODE_FIFO_KHR`**
(`render/src/vk/window.cpp:159`), so vsync is a second, invisible pacer running *inside* the guest's
vblank callback. `docs/future-enhancements.md:142-143` states the rule this creates: *"there must be
exactly one authority for real time."* Part A changes who that authority is, which is why section 7
treats it as a first-class piece of work rather than a detail.

### 1.4 Crazy Taxi is vblank-locked — briefly, as the justification for not touching the guest

The owner has ruled out speeding the guest up, so this section exists only to record why that ruling
is correct. Confidence: **high**, on four independent pieces of evidence, three of them measured.

1. **The project already selected the title on this property.** `docs/baseline-game.md:26`, criterion
   C5: *"Fixed 60 Hz, VBlank-locked, no raster tricks"*, and `:56`: *"Crazy Taxi is a Katana-SDK,
   single-player, VBlank-locked arcade port"*, scoring 2 on C5 (`:46`).
2. **Measured from a real run: the game's task scheduler is resumed from inside the vblank handler.**
   `docs/runtime-interrupts.md:31-34` records, dated 2026-09-12: *"Katana's scheduler runs inside the
   VBlank handler. When the handler's `rte` restores an SPC/r15 pair that is not the interrupted
   frame's…"* — the non-local-return machinery exists **because Crazy Taxi does this**, and
   `System::task_switch_rtes` counts the occurrences. A title whose tasks are resumed by the vblank
   interrupt steps its logic once per vblank by construction.
3. **The game registers a vertical-sync callback.** `games/crazytaxi/symbols.tsv:113` names
   `_kmSetVSyncCallback` at `0x0C15689A`; scanning the image for that word as a literal-pool entry
   finds it at `0x0C073E24`, whose user is the import trampoline at `0x0C073D90`
   (`mov.l @pool,r3; jmp @r3; mov #0,r5` — a tail call passing a null second argument). The game's
   own code, not the library's, links against it.
4. **Renders and guest frames already differ.** `docs/future-enhancements.md:160`: *"On Crazy Taxi
   the first two differ by about half during the boot"* — i.e. the game submits a Tile Accelerator
   render roughly every second vblank at times. Its *display* cadence is vblank-derived either way.

**So: running the guest at 144 Hz means running Crazy Taxi at 144/59.83 = 2.41× speed.** Said
plainly, because the brief asks for it plainly: the taxi would drive 2.4× faster, the fare timer
would run out 2.4× sooner, and the music would play 2.4× faster — except that it would not, because
`Aica::kSh4CyclesPerSample = 200'000'000 / 44100` (`runtime/include/dream/runtime/aica/aica.h:25`)
would then generate 106 kHz of samples into a 44.1 kHz device, and
`audio/src/sdl_sink.cpp:66-73` would discard most of them:

```cpp
    if (queued_samples() > high_water) {
        dropped += kBlockFrames;
        block_.clear();
        return;
    }
```

**Measured headroom, for completeness.** `docs/runtime-render.md:369-373`: *"Crazy Taxi runs at
about 1.6 to 2 times real time at `--scale 1` on an Apple M4, and at about real time at
`--scale 4`."* **Inferred:** 1.6–2.0× is below 2.41×, so even the rejected option would not have
reached 144 Hz on the reference machine. And critically for part A, **at `--scale 4` there is no
spare host time at all** — frame generation has to fit in whatever `--scale` is not using.

---

## 2. What the renderer has frame to frame — the question that decides everything

### 2.1 The data structures carry no identity

`render/include/dream/render/display_list.h:36-41`:

```cpp
struct Vertex {
    float x = 0, y = 0, z = 0;
    float u = 0, v = 0;
    std::uint32_t base = 0;    // base colour
    std::uint32_t offset = 0;  // offset (specular) colour, 0 when the polygon has none
};
```

`display_list.h:46-56`:

```cpp
struct Polygon {
    std::uint32_t first = 0;  // index into Frame::vertices
    std::uint32_t count = 0;  // vertices in the strip (>= 3), a triangle strip
    std::uint32_t pcw = 0;    // parameter control word
    std::uint32_t isp = 0;    // ISP/TSP instruction word: depth, culling
    std::uint32_t tsp = 0;    // TSP instruction word: blending, filtering, fog
    std::uint32_t tcw = 0;    // texture control word: format and address
    std::uint32_t tsp1 = 0xFFFFFFFFu, tcw1 = 0xFFFFFFFFu;
    std::uint32_t tile_clip = 0;  // user tile clip mode, from the parameter control word
};
```

and `Frame` (`display_list.h:66-77`) is five `std::vector<Polygon>` plus one vertex array. **There is
no object handle, no draw-call id, no guest address, and no sequence number anywhere.** A `Polygon`
is "some triangles, drawn this way".

### 2.2 The previous frame is not even kept

`runtime/boot/boot_main.cpp:157-167`:

```cpp
    void render(const std::vector<std::uint32_t>& stream) {
        last_stream = stream;  // kept so F11 can write the list that drew what is on screen
        decoder.reset();
        decoder.feed_stream(stream.data(), stream.size());
```

`decoder.reset()` clears `Frame` in place. `last_stream` is the **raw parameter words**, retained for
one reason only — the comment says it, and `boot_main.cpp:319` confirms it — so that F11 / `--capture-at`
can write out the list. Nothing reads it for rendering. Likewise `pvr::Ta::stream`
(`runtime/include/dream/runtime/pvr/core.h:49`, *"raw parameter words since the last list init"*) is
cleared on every list init (`runtime/src/pvr/core.cpp:51`).

**Measured: the renderer holds exactly one frame of geometry at a time, and discards it before
decoding the next.** Retaining two is trivial (a second `DisplayList`); *correlating* them is not.

### 2.3 What partial signals do exist, and why none of them is correspondence

Honest inventory of everything that could in principle link a polygon in frame N to one in N+1:

| signal | where | is it correspondence? |
|---|---|---|
| **list type** (opaque / PT / translucent) | `display_list.h:24-31` | No — five buckets for hundreds of strips. |
| **TCW** (texture address + format) | `Polygon::tcw` | **Weak**. Groups strips sharing a texture. Crazy Taxi's title screen is *"240 quads sharing one 16×16 texture"* (`display_list.h:88`), so a texture key can name 240 indistinguishable candidates. |
| **TSP / ISP** state words | `Polygon::tsp`, `:isp` | No — render state, shared by everything drawn the same way. |
| **submission order within a list** | `Frame::lists[i]` order | **Unreliable, and I could not measure how unreliable.** The order is whatever the guest's scene traversal produced. In an open-world driving game that traversal is culled and LOD-switched per frame, so strips appear and disappear and the indices of everything after them shift. |
| **strip boundaries** | `Polygon::first/count` | No — a boundary is a topology fact, not an identity. |
| **vertex positions** | `Vertex::x/y/z` | This is what you are trying to *predict*; using it as the key is circular. |
| **draw order after sorting** | `render/src/vk/renderer.cpp:415` | Actively hostile: the translucent list is `std::stable_sort`ed by depth every frame, so its draw order changes whenever anything moves. |

**Conclusion, stated plainly as the brief asks: nothing in the renderer correlates geometry between
frames.** Any technique that needs per-object or per-triangle correspondence must obtain it from the
guest, not from the display list. That is section 4.1's finding and it removes options (a) and TAA
from the table as renderer-only work.

### 2.4 The one thing the guest does have, and why the recompiler can reach it

`runtime/include/dream/runtime/sh4/ctx.h:36-37`:

```cpp
    float fr[16];          // front FP bank (FR0..15 when FPSCR.FR == 0)
    float xf[16];          // back FP bank
```

and `runtime/include/dream/runtime/sh4/ops.h:350-351`:

```cpp
// FTRV XMTRX,FVn: FV[n] = XMTRX * FV[n], XMTRX being the back bank as a 4x4 column-major matrix.
```

**`Ctx::xf` is XMTRX.** It is a plain host array in the recompiled CPU state, readable at any
instant with no emulator instrumentation at all.

Crazy Taxi transforms its vertices through it. `docs/widescreen-crazytaxi-study.md:316-321` located
the main vertex pipeline at `0x0C080E20` and its *"software-pipelined `ftrv xmtrx,fv4` loop at
`0x0C080F1C`"*; I confirmed that instruction directly:

```
0c080f1c  f5fd  ftrv xmtrx,fv4
```

and counted **150 `ftrv` and 78 `frchg` instructions** across the image (a scan of the binary for the
`0xF1FD`/mask-`0xF3FF` and `0xFBFD` encodings). The study names two further per-vertex pipelines at
`0x0C081F72` and `0x0C08492E` (`widescreen-crazytaxi-study.md:322-323`).

**What this buys, and the caveat.** XMTRX at the moment `0x0C080E20` is entered is the *full*
model-view-projection for whatever object is about to be transformed — not a single camera matrix.
That is the caveat, and it is also the opportunity:

- **For camera motion it is sufficient anyway**, because the static city is submitted with a model
  transform that is the identity (or a fixed placement), so the frame-to-frame delta of XMTRX over
  the world geometry *is* the camera delta. **Inferred, not measured** — section 8.6 says how to
  check it in half a day.
- **For per-object motion it is better than sufficient**, because the hook also sees the caller's
  argument registers. `0x0C080E20` takes its source vertex data through `r4`/`r5`
  (`fmov @r5+,fr10` at `0c080e6a`, `mov.l @r4+,r8` at `0c080eb0`) — **a guest pointer, which is a
  stable identity across frames in a way nothing in the TA stream is.**

This is the answer to "what can a recompilation do that a black-box emulator cannot". An emulator
can read XMTRX too, but it cannot cheaply know *which guest function* is being entered, nor hook it
without per-instruction instrumentation. Here it is an entry hook on a named function.

**The complication, already documented:** Crazy Taxi feeds the TA by bulk DMA, not inline stores.
`games/crazytaxi/symbols.tsv:97` names `_kmiDMAtoTARequest` at `0x0C153280`, and
`docs/widescreen-crazytaxi-study.md:645-655` spells out the consequence: *"By the time parameters
reach `pvr::Core::fifo_word`, the function that produced them has long returned, so 'which guest PC
wrote this parameter' is not directly available."* The fix the study proposes — record which byte
ranges of the vertex buffer each hooked producer wrote, then split the DMA into spans — is exactly
the mechanism part A needs, and exactly the mechanism the widescreen HUD work needs. **It is a
shared prerequisite.** Section 6 and section 11.2.

---

## 3. What the render path can and cannot hand a reprojector

Establishing this before assessing the four families, because two of them die on it.

### 3.1 The offscreen target: one subpass, one colour image, depth thrown away

`render/src/vk/offscreen.cpp:21-32` creates the two attachments:

```cpp
    if (!colour_.create(ctx, VK_FORMAT_R8G8B8A8_UNORM, extent_,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT)) { ... }
    const VkFormat depth_format = pick_depth_format(ctx.physical_device());
    if (!depth_.create(ctx, depth_format, extent_, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                       VK_IMAGE_ASPECT_DEPTH_BIT)) { ... }
```

**The depth image has no `TRANSFER_SRC` and no `SAMPLED` usage**, and its `storeOp` is
`VK_ATTACHMENT_STORE_OP_DONT_CARE` (`offscreen.cpp:46`). So **the depth buffer exists but is
discarded and is unreadable today.** Making it readable is two flag changes plus a layout transition
— genuinely small — but it is not free today and must appear in the plan.

The format is `VK_FORMAT_D32_SFLOAT` where available, chosen deliberately
(`render/src/vk/resources.cpp:162-173`): *"32-bit float depth first: the PowerVR's depth is 1/w over
a wide range, and the fragment shader writes a logarithm of it, so precision matters more than memory
here."* That is good news for reprojection — 32-bit depth is what a warp wants.

### 3.2 Depth is a logarithm of 1/w, and it is invertible

`render/shaders/geometry.frag:67-68`:

```glsl
    const float w = 100000.0 * inv_w;
    gl_FragDepth = log2(1.0 + max(w, -0.999999)) / 34.0;
```

So `d = log2(1 + 100000/w_eye) / 34`, and therefore `w_eye = 100000 / (2^(34d) − 1)`. **Measured
from the shader, invertible in closed form, one line in a reprojection shader.** Combined with the
viewport block's pixel scales (`[0x0C2B0930]`, `[0x0C2B0934]`;
`docs/widescreen-crazytaxi-study.md:301-302`) and origins, a screen pixel plus its depth reconstructs
a view-space point exactly. **This is the technical fact that makes option (b) real rather than
hand-waved.**

One caveat, measured: `docs/runtime-render.md:105-107` notes writing `gl_FragDepth` *"costs
early-Z"*, and the sorted translucent pass writes no depth at all
(`render/src/vk/renderer.cpp:423-425`). **So the depth buffer describes the opaque and punch-through
geometry only.** Translucent surfaces — Crazy Taxi's logo, its HUD panels, glass, particle effects —
have no depth and cannot be reprojected by a depth warp. They have to be handled as a separate layer
(section 6) or left alone.

### 3.3 Presentation goes through the host, once per guest vblank

The full path for one frame today:

1. `pvr::Core::start_render` fires `on_render(ta.stream)` (`runtime/src/pvr/core.cpp:175-181`).
2. `Live::render` decodes and calls `Offscreen::render`, which records, submits, and **waits**
   (`render/src/vk/offscreen.cpp:197-211`), then copies the colour image into a host-mapped buffer
   (`offscreen.cpp:187-191`). `offscreen.h:13-14` is explicit: *"The readback is synchronous: record,
   submit, wait."*
3. At vblank-out, `Live::present` (`boot_main.cpp:234`) picks the source, and
   `Presenter::upload` copies those **host** pixels back into a staging buffer and thence to a
   device image (`render/src/vk/present.cpp:286-300`), which a fullscreen triangle then samples
   (`render/shaders/present.vert`, `present.frag`).
4. `Window::end_frame` presents, `vkQueuePresentKHR` at `render/src/vk/window.cpp:559`, FIFO.

**Measured consequences for part A:**

- **There is exactly one present per guest vblank**, because `present()` is only ever called from the
  `on_vblank_out` chain (`boot_main.cpp:1548-1551`, and from the menu pump at `:1557-1565`).
  Presenting at 144 Hz requires a presentation loop that is not the guest's vblank callback.
- **The frame makes a GPU→host→GPU round trip every frame.** A reprojector wants the colour and
  depth *on the GPU*; today the colour comes back to the CPU and is re-uploaded. Reprojection needs a
  GPU-resident path added alongside the existing readback (which must stay: the guest can read its
  own framebuffer, `offscreen.h:6-9`).
- **One queue, single-threaded.** `Offscreen::render` and `Window::end_frame` both submit to
  `ctx.queue()`. A presenter running on its own thread needs external synchronisation or a second
  queue.

---

## 4. The four families, assessed for this pipeline

### 4.1 (a) Geometry interpolation — rejected

**What it needs:** for each polygon in frame N+1, the same polygon in frame N, so its vertices can be
lerped.

**Why it fails here:** section 2.3. There is no identity, and the only plausible key (TCW + strip
index) collides hundreds of times in a single Crazy Taxi frame by this repository's own measurement
(`display_list.h:88`, 240 quads on one texture). A wrong match does not degrade gracefully: it lerps
one building's corner towards another building's corner, producing a triangle that sweeps across the
screen. **The failure mode is not "slightly soft", it is "geometry explodes".**

Could correspondence be *manufactured* from the guest? Yes — this is the `0x0C080E20` hook plus DMA
span tracking from section 2.4. But then you are no longer doing renderer-side frame generation; you
are reconstructing a scene graph out of a recompiled arcade game, per title, by hand. **Inferred
cost: 25 / 50 / 100+ days**, and the high end is not bounded by anything I can point at. Rejected.

**Note for the record:** the same rejection applies to **TAA/TSAA**. A temporal accumulator needs
per-pixel motion vectors, which need exactly this correspondence. Neither the display list nor the
depth buffer can produce them. *If* the option (b) work lands, a **camera-only** motion vector field
becomes available and a TAA variant becomes discussable — but it would ghost on every moving object
for the same reason section 4.2 does, which in a game about traffic is most of the screen that
matters.

### 4.2 (b) Depth-aware camera reprojection — **recommended**

**What it needs, and whether it exists:**

| requirement | status |
|---|---|
| a depth buffer at the rendered resolution | **exists**, must be made readable (3.1) — small |
| depth invertible to view space | **yes**, closed form (3.2) |
| the projection parameters | **yes**, guest RAM: `[0x0C2B0930]`, `[0x0C2B0934]`, `[0x0C14862C]`, `[0x0C148630]`, `[0x0C148634]`, `[0x0C148638]` (`widescreen-crazytaxi-study.md:294-303`) |
| camera motion between frames | **recoverable**, `Ctx::xf` via a hook (2.4) — **inferred**, needs the half-day check in 8.6 |
| a hook mechanism to read them | **specified but not built** — `docs/widescreen-crazytaxi-study.md:797` step 2, 1 / 2 / 3.5 days, **shared with widescreen** |
| a GPU-resident colour+depth history | **does not exist** (3.3) — must be added |
| the 2D layer separable | **not yet** — shared with widescreen (section 6) |

**What it would look like when it fails.** Three named artefacts, in the order they will show up:

- **Disocclusion holes.** Warping the camera forward uncovers surfaces that were behind something in
  frame N. Nothing in the source frame has their colour. In a driving game the disoccluded band is
  along every vertical edge in the direction of travel: the trailing edge of every building, lamp post
  and vehicle. Mitigations are all approximations — nearest-valid-neighbour fill, dilate-from-the-far-side,
  or re-projecting *both* frame N and the previous one and preferring whichever has data. **This is
  where the estimate spread lives.**
- **Moving objects smear.** A camera-only warp treats the world as rigid. Other taxis, traffic,
  pedestrians and (worst, because it is large and close) **the player's own car body** move relative to
  the camera. They get warped by the camera delta and land in the wrong place, which reads as a
  rubber-band wobble at 144 Hz. Fixing it properly needs per-object motion, i.e. the identity problem
  again — but at *per-object* granularity a guest hook can supply it (2.4), which is why this is a
  cost rather than a wall.
- **Translucent surfaces do not move at all**, because they wrote no depth (3.2). Sparks, glass,
  headlight glows and the HUD would sit still while the world slides under them.

**Latency:** see section 5. Run as extrapolation (option d), it adds none.

**Why it is the right answer despite all that:** it is the only option whose missing input is
*obtainable*, and obtaining it is work this repository is already committed to doing for widescreen.
Every other option's missing input is missing permanently.

### 4.3 (c) Image-space interpolation / optical flow — rejected

**What it needs:** two presented frames and a dense flow field between them, computed with no game
data.

**Why it fails here:**

- **It is a second, harder research problem.** A usable flow estimator is either a classical
  pyramidal solver (slow on CPU, and the frames are already on the CPU in this pipeline — see 3.3 —
  which is the one thing in its favour) or a small neural network, which this project has no
  infrastructure for and which would drag a runtime dependency into a GPL-2.0 tree.
- **It has no way to protect the HUD.** Section 6 — the fare counter and the timer are exactly the
  high-contrast small text that optical flow gets wrong, and unlike (b) there is no signal that says
  "these pixels are 2D".
- **60→144 is not a 2× interpolation.** 144/59.83 = 2.407, so the generated frames sit at irregular
  phases and a flow-based interpolator must be evaluated at arbitrary t, compounding error.
- **Inferred cost: 15 / 30 / 60 days**, for the worst artefacts of the four and the least
  controllable failure mode. Rejected.

### 4.4 (d) Extrapolation from the latest frame — **recommended, as the mode of (b)**

This is not a fourth pipeline; it is a choice of *sign* on the same reprojection, and it is the right
choice.

- **Interpolation** between N and N+1 requires holding N+1 until N+1 has been drawn — one full guest
  frame, 16.7 ms, of added latency (section 5).
- **Extrapolation** warps the newest frame forward by the *predicted* camera delta and presents
  immediately. No latency is added. The cost is that the prediction can be wrong.

**What it looks like when it fails:** the world over-shoots and then snaps back when the real frame
arrives — a judder at the guest's own 60 Hz, which is the frequency the eye is least forgiving of.
This is mitigated by (i) predicting from the *measured* previous delta rather than assuming
constant velocity, (ii) damping the extrapolation towards zero as the extrapolation distance grows,
and (iii) capping the warp magnitude. All three are cheap and all three trade artefact for
"the generated frames do less". **Inferred:** in a driving game the camera is smooth and
low-acceleration for the great majority of frames, which is the case extrapolation handles best;
the bad case is a collision or a hard handbrake turn, which is also the case where the player is
least able to see the artefact.

**Cost delta over (b)-as-interpolation:** effectively zero. It is a different matrix and a different
present schedule.

### 4.5 Summary table

| option | needs | exists here? | latency added | worst artefact | cost (days, low/central/high) |
|---|---|---|---|---|---|
| (a) geometry interpolation | per-polygon correspondence | **no**, and not obtainable cheaply | 16.7 ms | geometry sweeps across the screen | 25 / 50 / 100+ |
| (b) depth reprojection, interpolating | depth + camera delta | depth yes (3.1), camera via hook (2.4) | 16.7 ms | disocclusion holes; moving objects smear | ~ as (d) + 1 |
| (c) image-space flow | nothing from the game | n/a | 16.7 ms | HUD smears, unbounded | 15 / 30 / 60 |
| **(d) depth reprojection, extrapolating** | **depth + camera delta** | **as (b)** | **none** | **overshoot judder at 60 Hz** | **17 / 35 / 75** |

---

## 5. Latency, treated as a first-class concern

This is a driving game with analogue steering, so latency is not a tiebreaker, it is a requirement.

**The budget today.** Input is sampled inside the same vblank callback that presents
(`boot_main.cpp:1567-1569`, `live->read_controls(pad_ptr->state)`), so the guest sees a control
change at most one guest frame — 16.7 ms — after the host read it, plus the swapchain's FIFO queue
depth, plus the display. Call it 30–50 ms end to end, which is ordinary.

**What interpolation would cost.** To interpolate between frames N and N+1 you must have both, so
frame N cannot be shown until N+1 is finished. That is **+16.7 ms on everything the player does**, a
~40% increase on the guest-side budget. Against that, the benefit is smoother *apparent* motion of a
picture the player is reacting to 16.7 ms late. **For a driving game that is a bad trade and I would
not ship it**, and I would say so even if the artefacts were perfect — which they are not.

**What extrapolation costs.** Nothing, by construction: the newest real frame is presented as soon as
it exists, and the manufactured frames are inserted *after* it while waiting for the next. It can
even *reduce* perceived latency slightly, because a generated frame carries a fresher camera
prediction than the last real frame did — but that benefit should not be claimed until it is
measured, and section 8.6 says how.

**Recommendation, taking latency seriously:** **extrapolation only.** Do not build an interpolation
mode "for comparison"; build the extrapolator, and if someone wants the comparison, they can negate
the time parameter, which is a one-line debug flag rather than a supported mode.

**One latency trap to avoid.** Section 1.3 notes vsync is already a hidden pacer inside the guest's
vblank callback. Presenting at 144 Hz from a separate loop while the guest still sleeps against its
own clock means **two** loops with opinions about time. Section 7 keeps the guest clock as the single
authority and makes the presenter subordinate; doing it the other way round reintroduces the
`docs/future-enhancements.md:142-143` problem at triple the rate.

---

## 6. The 2D layer: a shared prerequisite, not a part-A detail

**The HUD must not be warped.** Crazy Taxi's fare counter, timer, destination arrow and menu text are
authored in screen space against a 640-wide screen. A camera reprojection that includes them would
slide the fare counter around the screen every generated frame — a far more objectionable artefact
than the aliasing part B fixes, and one that is visible on a still photograph.

**The separation problem is already characterised, by the widescreen study, for the same reason.**
`docs/widescreen-crazytaxi-study.md:626-661` ("The 2D/HUD layer — the single biggest risk") names:

- **`fn_0x0C07D018` as the sprite choke point** — *"100+ call sites, all in `0x0C02F000`–`0x0C05F000`
  (the UI and menu code where every literal `320.0f`/`240.0f` in the binary lives)"*
  (`widescreen-crazytaxi-study.md:327-329`). Confidence recorded there as **medium-high**
  (`:373`), with the caveat that a 3D billboard could use the same function.
- **Two ways to attribute geometry to it**: a cheap classifier first — *"2D quads are submitted as
  PVR sprites with a constant `1/w`"* and a captured frame was *"473 of 524 polygons are sprites"*
  with *"depth 0.01 .. 10.0"* (`:657-661`) — and, if that fails, entry/exit hooks on
  `fn_0x0C07D018` recording which byte ranges of the vertex buffer the 2D path wrote, so the
  `_kmiDMAtoTARequest` DMA can be split into 2D and 3D spans (`:645-655`).

**Answer to the brief's question: yes, the 2D/3D separation is a shared prerequisite.** Widescreen
needs it to centre the HUD without distorting it; frame generation needs it to exclude the HUD from
the warp. It should be built **once**, as a labelled classification on `Frame` (a per-`Polygon` "this
is 2D" bit), and both features consume it. Building it twice would be the most avoidable waste in
either project.

**One difference worth noting.** Frame generation's requirement is weaker than widescreen's.
Widescreen needs the HUD attributed *correctly* or it is visibly mispositioned in a screenshot;
frame generation needs it attributed *conservatively* — anything uncertain can be left unwarped,
which costs a little judder on a misclassified 3D element and costs nothing at all on a
misclassified 2D one. **So the cheap depth/list/sprite classifier may well be sufficient for part A
even if it is not sufficient for widescreen.** That is worth measuring before committing to the
expensive path (section 8.6).

---

## 7. Presentation and pacing: what has to change

Today (3.3): one present per guest vblank, from the guest's own callback, on the guest's thread, to
a FIFO swapchain, with the guest clock as the pacing authority.

**What 144 Hz presentation requires.**

1. **A presentation loop that is not `on_vblank_out`.** Two shapes:
   - *Same thread, nested pump.* Replace the pacer's `std::this_thread::sleep_for`
     (`boot_main.cpp:1583`) with a loop that, while waiting for the guest's next frame deadline,
     presents generated frames. **Cheapest, and it keeps the single-queue single-thread invariant
     intact** — nothing needs new synchronisation. The weakness is that it only generates frames
     during the *idle* part of a guest frame, so at `--scale 4` (where `docs/runtime-render.md:372`
     records there is no idle time) it degrades to doing nothing. **Inferred: this is the right first
     implementation**, because it is honest about where the host time comes from.
   - *Separate presentation thread.* Decouples properly, but requires external synchronisation on
     `ctx.queue()` (3.3) or a second queue, a double-buffered GPU-resident history, and careful
     handling of `Window::poll` (SDL event pumping is main-thread-only on macOS). Considerably more
     work for a benefit that only appears when the guest is *not* the bottleneck.
2. **Present mode.** `VK_PRESENT_MODE_FIFO_KHR` (`window.cpp:159`) blocks to the display refresh. At
   144 Hz on a 144 Hz display FIFO is what you want; on a 60 Hz display it silently caps the whole
   feature, and generating frames a monitor cannot show is pure waste. **The feature must query the
   display refresh and cap the generation factor to it**, and should offer `MAILBOX` where available.
   `docs/future-enhancements.md:135-140` already flags present mode as needing to be an option.
3. **Keep the guest clock as the one authority for real time.** The generated frames must *not*
   influence `sys.ctx.cycles` or the pacer's target. The pacer's job stays "sleep until the guest's
   next frame is due"; the only change is that the sleep is spent presenting instead of idling.
4. **The FPS counter must learn a fourth number.** `docs/future-enhancements.md:154-158` already
   specifies three (guest frames, TA renders, presents); generated-versus-real presents is the fourth,
   and without it nobody can tell the feature is working.
5. **A GPU-resident colour+depth history**, alongside — not replacing — the existing host readback
   (3.3), because the guest can still read its own framebuffer.

---

## 8. Part A: plan, estimate, verification, unknowns

### 8.1 Recommended approach

**Depth-aware camera reprojection, forward-extrapolating, 2D layer excluded, off by default.**

Concretely: keep the guest at 59.83 Hz. Each time the guest finishes a render, snapshot (i) the
offscreen colour and depth as GPU-resident images, (ii) the guest's XMTRX and viewport block through
a hook. Between guest frames, present additional frames produced by warping the newest colour+depth
by an extrapolated camera delta, with 2D-classified geometry composited unwarped on top. Cap the
generation factor to the display's refresh rate.

### 8.2 Rejected alternatives, with reasons

| rejected | reason |
|---|---|
| Geometry interpolation (a) | Needs per-polygon correspondence that does not exist (2.3) and whose manufacture is a per-title scene-graph reconstruction (4.1). |
| Image-space optical flow (c) | Second research problem, no way to protect the HUD, irregular 2.407× phase (4.3). |
| Any interpolating mode | +16.7 ms input latency in a driving game (5). |
| Raising the SPG rate (brief's option i) | 2.41× game speed, desynchronised from TMU and AICA (1.2, 1.4). Owner has ruled it out; the evidence supports the ruling. |
| `--unthrottled` (brief's option iii) | Already implemented (`boot_main.cpp:1214`). Removes the cap; does not produce frames. |
| TAA/TSAA | Needs the same absent motion vectors (4.1). Not a cost question — an availability one. |

### 8.3 Step-by-step plan and estimate

Effort in **focused engineer-days**, the convention from `docs/implementation-plan.md:8-10`.

| # | Step | Files touched | Low | **Central** | High | What drives the spread |
|---|---|---|---|---|---|---|
| A1 | **Implement `[hooks]`** — wire `GameConfig::hooks` into the emitter as entry/exit calls; a `dream::hooks` registry in the runtime. **Shared verbatim with `docs/widescreen-crazytaxi-study.md:797` step 2; count it once across both projects.** | `translator/src/emit/emit.cpp`, `translator/src/main.cpp`, `translator/include/dream/translator/emit.h`, new `runtime/src/hooks/`, `docs/game-config.md` | 1 | **2** | 3.5 | The registry, naming, tests, and the "hook on a function that is also an overlay" case. |
| A2 | **Camera capture.** Entry hook on `fn_0x0C080E20` (and `0x0C081F72`, `0x0C08492E`) snapshotting `Ctx::xf[16]` plus the six viewport words; expose the per-render camera to the renderer. Confirm the static-world delta *is* the camera delta (8.6). | `games/crazytaxi/crazytaxi.toml` `[hooks]`, new hook source, `runtime/boot/boot_main.cpp` | 1 | **2** | 4 | Whether one pipeline covers the world geometry or all three are needed; whether the model part of XMTRX is identity for the city. |
| A3 | **Make depth readable and keep a GPU-resident history.** `storeOp = STORE`, add `SAMPLED`/`TRANSFER_SRC` usage, a layout transition, and a second colour+depth pair so frame N survives while N+1 is drawn. | `render/src/vk/offscreen.cpp:21-50,183-191`, `offscreen.h:59`, `render/src/vk/resources.cpp` | 1 | **2** | 3 | Small and known. High end is MoltenVK depth-sampling quirks. |
| A4 | **The reprojection pass itself.** Unproject `log2(1+100000/w)/34` to view space, transform by the extrapolated camera delta, reproject, hole-fill. New shader + pipeline in the present path. | new `render/shaders/reproject.{vert,frag}`, `render/src/vk/present.cpp`, `render/include/dream/render/vk/present.h` | 4 | **8** | 20 | **The dominant uncertainty.** A naive backward warp is 2 days and looks bad. Acceptable disocclusion handling is where the 20 comes from. |
| A5 | **2D/HUD exclusion.** A per-`Polygon` "2D" bit on `Frame`, populated by the cheap sprite/depth/list classifier first, by `fn_0x0C07D018` span tracking if that fails. **Shared with `widescreen-crazytaxi-study.md:801` step 6**; part A's requirement is the weaker one (section 6). | `render/src/display_list.cpp`, `render/include/dream/render/display_list.h`, `render/src/vk/renderer.cpp`, hook source | 2 | **3** | 6 | Whether the cheap classifier suffices. Measure before building (8.6). |
| A6 | **Moving objects.** Decide and implement: accept the smear, mask them out of the warp, or supply per-object motion from the `0x0C080E20` hook's guest pointer (2.4). | hook source, `render/src/vk/renderer.cpp`, reprojection shader | 3 | **8** | 25 | **The second dominant uncertainty and the honest reason this is research.** "Accept it" is 3 days and visibly wrong on traffic. Per-object motion is a scene-graph reconstruction. |
| A7 | **Presentation and pacing.** Nested-pump presentation loop inside the pacer; display-refresh query and generation cap; present-mode option; the fourth counter. | `runtime/boot/boot_main.cpp:1571-1583,486-505`, `render/src/vk/window.cpp:159,515-560` | 2 | **4** | 8 | Low if the nested pump is enough; high if a presentation thread turns out to be required (7.1). |
| A8 | **Guards, tests, docs, CI on three platforms.** Off by default; forbidden in differential-harness and golden-trace runs; `--screenshot-at` semantics (section 12.3); update `docs/runtime-render.md:393`. | `runtime/boot/boot_main.cpp`, `render/tests/`, `docs/runtime-render.md`, `docs/differential-harness.md` | 1.5 | **3** | 5 | Mostly known work; the spread is the three-platform matrix. |
| | **Total** | | **15.5** | **32** | **74.5** | |

**Rounded for quoting: 35 days central, range 17–75**, allowing a little for integration the table
does not name. **If the `[hooks]` mechanism (A1) is built for widescreen first, subtract 2 days.**

### 8.4 Be honest about what kind of job this is

Steps A1–A3 and A7–A8 are engineering: bounded, well-understood, estimable within a factor of two.
That is 13 / 21 / 31 days and it produces **infrastructure** — a hook mechanism, a readable depth
buffer, a GPU-resident history, a decoupled presenter — all of which are useful for other things
(TAA later, render-to-texture, the options UI).

Steps A4 and A6 are **research**: 7 / 16 / 45 days, with the high end genuinely open. Disocclusion
fill and moving-object handling are the two problems the whole commercial frame-generation field
exists to solve, and it solves them with motion vectors this project does not have. **Do not promise
a quality bar on A4 and A6 in advance.** Promise the infrastructure, ship the camera-only warp behind
a flag, and let the screenshots decide whether A6 is worth 25 days.

### 8.5 The minimum honest slice — 13 days

Steps A1, A2, A3, A4-partial (backward warp with nearest-valid hole fill), A5 with the cheap
classifier, A7 nested pump, A8-partial. Ships as `--frame-generation=N` (off by default), documented
as *experimental, camera-only, moving objects will smear*. Everything in it is reused by the full
version, and A1/A5 are shared with widescreen, so a substantial part of the cost is not attributable
to this feature at all.

### 8.6 How to verify — concretely

**Before building anything (half a day, and it decides A2 and A5):**

- **Is the static-world XMTRX delta the camera delta?** With a throwaway probe printing `Ctx::xf[16]`
  at entry to `fn_0x0C080E20` for every call in two consecutive frames of a stationary-camera scene,
  the matrices for the city geometry must be **bit-identical between the two frames**. If they are,
  the model part is identity and A2 is easy. If they differ, A2 needs the delta computed per call
  site and the estimate moves to its high end.
- **Does the cheap 2D classifier work?** Decode a captured in-game frame (`--capture-at N`, then
  `dream_render_view`) and print, per polygon, its list, its `1/w` range, and whether it is a sprite.
  This is the same half-day measurement `widescreen-crazytaxi-study.md:890-893` calls for and it
  should be done once for both.

**Proving the generated frames are real frames and not duplicates:**

- Present-counter ratio must be 2.407 ± 0.01 against `sys.spg.frames()` over a 600-frame run with a
  144 Hz display, and the generated:real split must be 1.407:1. A feature that silently falls back to
  presenting duplicates shows here as 1.000 and nowhere else.
- **Frame-hash the presented images.** Every presented frame's pixel hash must be distinct from its
  neighbours during camera motion. Duplicates are the failure this catches.

**Proving the warp is correct (the decisive test):**

- **Zero-delta identity.** With the camera stationary, a generated frame must be **byte-identical**
  to the real frame it was warped from. Any difference is a bug in the unprojection or the sampler,
  and this test localises it before any artefact argument starts.
- **Round-trip.** Warp frame N forward by the *measured* delta to N+1 and compare against the real
  frame N+1, as a mean absolute error over the non-disoccluded pixels. Track that number across
  changes; it is the single quality metric for A4. Report the disoccluded fraction separately — it is
  the number A4's hole-filling is judged on.

**Proving the HUD does not move:**

- Crop the HUD bounding box from a real frame and from each generated frame that follows it. The
  crops must be **byte-identical**. Same test as `widescreen-crazytaxi-study.md:883-886`, same
  tolerance argument (none needed).

**Proving latency did not regress:**

- Scripted `--press` at a known guest frame; measure guest frames from the press to the first pixel
  change. Must be unchanged with the feature on. If it is not, something is holding a frame back and
  the extrapolation has silently become interpolation.

**Proving nothing else regressed:**

- `ctest --test-dir build` on all three platforms with the feature off, unchanged.
- One differential-harness run with the feature off, confirming the hooks cost nothing when unused.
- Audio: `dropped` and `starved` counts unchanged (section 12.1).

### 8.7 What I could not determine, and how to find out

| unknown | why it matters | how to settle it |
|---|---|---|
| **Is Crazy Taxi's submission order stable frame to frame?** | If it were surprisingly stable, option (a) would become discussable. I have no way to measure it statically. | Capture two consecutive in-game TA streams (`--capture-at N`, `N+1`), decode both, and compute the longest common subsequence of (TCW, strip length) pairs. High stability would be a genuine surprise and would be worth knowing. |
| **Whether XMTRX over the city is camera-only.** | Decides A2 (2 days vs 4) and whether per-object motion in A6 is cheap or not. | The bit-identity probe in 8.6. |
| **Whether the cheap 2D classifier suffices.** | Decides A5 (3 days vs 6) and is shared with widescreen. | The half-day frame decode in 8.6. |
| **What fraction of screen pixels disocclude per generated frame in city driving.** | It *is* A4's quality ceiling. | The round-trip measurement in 8.6, on a recorded 600-frame drive. This is the first number I would want. |
| **Whether an in-city frame can be captured at all today.** | `docs/runtime-render.md:232-233` records the city needs a crash fixed first. If still true, every measurement above is blocked on it, and the whole of part A is blocked rather than slow. | Try `--capture-at` on a scripted drive. Same blocker `widescreen-crazytaxi-study.md:796` names. |
| **Real host cost of the warp at `--scale 4`.** | `docs/runtime-render.md:372` says `--scale 4` already runs at about real time — there may be no host budget at all. | Time the reprojection pass alone on the reference M4 once A4 exists; until then, assume `--scale 4` and frame generation are mutually exclusive. |

---

## 9. Part B: the render path as it bears on anti-aliasing

### 9.1 Single-sampled everywhere, and the OIT interaction

`render/src/vk/offscreen.cpp:34-50` builds a two-attachment, one-subpass render pass with
`attachments[0].samples = VK_SAMPLE_COUNT_1_BIT` (`:36`) and the same for depth (`:44`). Every
geometry pipeline is single-sampled (`render/src/vk/renderer.cpp:250-252`), as are the presenter
(`present.cpp:130-132`), the window's pass (`window.cpp:186,194`), the attachment helper
(`resources.cpp:106`) and the texture cache (`texture_cache.cpp:154`). **There is no multisampling
anywhere in the project.**

**Now the OIT interaction, which the brief correctly calls the most important technical finding in
part B. There are two things to get right and they point the same way.**

**First, the repo's state is not what ADR 9 implies, and the study must say so.** ADR 9 is
**Accepted** (`docs/decisions/README.md:21`): *"Vulkan renderer ported from Flycast's per-pixel OIT
path"*, and `:249-252` confirms the macOS risk is resolved — MoltenVK on an M4 reports
`fragmentStoresAndAtomics` and `VK_EXT_fragment_shader_interlock`. But what is **in the tree** is the
per-strip fallback. `render/src/vk/renderer.cpp:390-393`:

```cpp
    // Translucent polygons are sorted back to front per strip. That is the fallback the hardware
    // does not need: a real PowerVR sorts per pixel, so two translucent surfaces that intersect
    // come out right and a per-strip sort cannot. Per-pixel sorting is a later refinement
```

implemented as a `std::stable_sort` at `renderer.cpp:415`. The capability flags are detected
(`render/src/vk/context.cpp:141-150`) and reported, and **nothing consumes them yet.** So per-pixel
OIT is a *pending* part of the render path.

**Second — and this is the decisive part — MSAA does not survive that pending work.** Flycast's OIT
path, the one this project is committed to porting, is a **three-subpass render pass with input
attachments and a per-pixel A-buffer** (`build/flycast-src/core/rend/vulkan/oit/oit_renderpass.cpp:56-77`:
*"Depth and modvol pass"*, then two more, with `vk::SubpassExternal` fragment-shader dependencies at
`:78` and `:103`). And **it has no multisampling at all** — a grep for `SAMPLE_COUNT`, `MSAA` and
`multisample` over `build/flycast-src/core/rend/` returns nothing but the words *"Rasterization and
multisample states"* in nine comments (`oit_pipeline.cpp:35,211,295,375,487`,
`pipeline.cpp:52,174,291`, `quad.cpp:74`). Every one of those pipelines takes the default one sample.

The reason is structural, not an oversight:

- **A per-pixel linked list becomes a per-*sample* linked list.** At 4× MSAA the A-buffer's storage
  and its per-fragment atomic traffic multiply by four, and the resolve must sort each sample's list
  independently. That is not "MSAA costs 30%"; it is a different memory budget.
- **Input attachments must match the render pass's sample count**, and reading a multisampled input
  attachment requires per-sample shading in the consuming subpass — which is full supersampling of
  the OIT resolve, at which point you have paid for SSAA and got MSAA's edge quality.
- **`gl_FragDepth` is written by this project's own geometry shader** (`geometry.frag:68`), which
  `docs/runtime-render.md:105-107` already notes *"costs early-Z"*. With MSAA it also forces the
  depth value to be shader-computed per fragment and broadcast to all covered samples, so the usual
  "one shader invocation, four depth samples" economy that makes MSAA cheap is exactly the economy
  this shader gives up.
- **Punch-through gets nothing.** The PT list is `discard`-based (`geometry.frag:62-63`) against
  `PT_ALPHA_REF`, and MSAA does not anti-alias a `discard` edge without alpha-to-coverage — which
  changes the look of every cut-out texture in the game and is a correctness change, not a quality
  one. Crazy Taxi's foliage, fences, signage and the "473 of 524 polygons are sprites" frame
  (`docs/runtime-render.md:93-96`) live disproportionately in exactly this list.

**Conclusion: MSAA would be built once now, be wrong for punch-through, and be thrown away when OIT
lands.** That is the finding.

### 9.2 `--scale` is real SSAA — and the resolve throws it away

`runtime/boot/boot_main.cpp:128-137`:

```cpp
    bool start(unsigned scale, bool validation, const std::string& title) {
        const std::uint32_t w = kGuestWidth * scale, h = kGuestHeight * scale;
        if (!window.create(title.c_str(), 960, 720, validation)) { ... }
        if (!offscreen.create(window.context(), w, h)) { ... }
```

`kGuestWidth = 640, kGuestHeight = 480` (`:603`), `--scale` 1..4 (`:1264-1266`, help at `:1039`). The
rendered image really is 2560×1920 at `--scale 4`, and `Live::present` shows it directly rather than
the guest's framebuffer (`boot_main.cpp:246-251`):

```cpp
        from_renderer = described && fb.enabled && rendered_into(fb.address);
        if (from_renderer) {
            shown = fb;
            shown.width = offscreen.width();
            shown.height = offscreen.height();
```

`docs/runtime-render.md:331-336` states the intent: *"because the window is shown the rendered image
rather than a copy squeezed back through the guest's pixel format, the extra resolution actually
reaches the screen."*

**It does not reach the screen.** `render/include/dream/render/vk/present.h:51-53`:

```cpp
    // Nearest-neighbour rather than linear filtering: the guest's pixels shown as pixels. An
    // upscaling mode will want the choice, so it is a field rather than a constant.
    bool smooth = false;
```

and `render/src/vk/present.cpp:74-76`:

```cpp
    sci.magFilter = smooth ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sci.minFilter = sci.magFilter;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
```

**`smooth` is never assigned anywhere in the tree** — grep over `render/` and `runtime/` finds the
declaration and this use and nothing else. The presenter's image has `mipLevels = 1`
(`present.cpp:229`), so there is no mip chain either, and `render/shaders/present.frag:10` is a
single `texture()` fetch.

The default window is 960×720 (`boot_main.cpp:130`). So at `--scale 4`, a 2560×1920 image is minified
to 960×720 **by point sampling**: about one rendered sample in seven survives. At `--scale 2` it is
about one in 1.8. Even `VK_FILTER_LINEAR` would only be a 2×2 tap, which is the correct box filter at
exactly 2:1 and under-samples beyond it.

**This is measured from the code, not judged from a picture, and it reframes part B entirely.** The
default is *defensible* at `--scale 1` — showing the guest's 640×480 pixels as pixels is a deliberate
choice and the comment says so. It is simply wrong above `--scale 1`, which is the only case where
anti-aliasing is being discussed.

**Inferred:** fixing the resolve gives, at `--scale 4` into a 960×720 window, a true 2.67×2.67 box
average per output pixel — **better edge quality than 4× MSAA**, on every list including
punch-through, with no OIT interaction of any kind, for roughly one day of work.

---

## 10. Part B: each option, and the recommendation

### 10.1 MSAA — rejected

| | |
|---|---|
| **What it needs** | Multisampled colour and depth images (`offscreen.cpp:21-32`), `samples` on both attachment descriptions plus a **resolve attachment** (`offscreen.cpp:34-50`), `rasterizationSamples` on every pipeline (`renderer.cpp:252`), and the readback copy re-pointed at the resolve target (`offscreen.cpp:187-191`). |
| **Memory** | 4× samples on colour and depth. At `--scale 1`: 640×480×4 B×4 ≈ 4.9 MB colour + 4.9 MB depth (D32) — trivial. At `--scale 4`: **≈ 78 MB + 78 MB**, plus the resolve target. Not fatal, but it is a real allocation on a laptop GPU that is also holding the texture cache. |
| **Cost** | The render-pass surgery is genuinely small. The expensive parts are: `gl_FragDepth` defeating MSAA's economy (9.1), the punch-through list getting nothing without alpha-to-coverage, and **the whole thing being rebuilt when OIT lands**. |
| **Interaction with OIT** | Hostile and structural (9.1). Flycast, the reference, has none. |
| **Estimate** | 4 / 8 / 20 days — the high end being "and then we did per-sample OIT". |
| **Verdict** | **Rejected.** Strictly worse than fixing the resolve (9.2), which is 1 day and beats it on quality. |

### 10.2 TAA / TSAA — unavailable, not merely expensive

**The brief asks whether the required inputs exist at all. They do not.**

A temporal accumulator needs, per pixel, where that pixel's surface was in the previous frame. That
is a motion vector. Producing one requires knowing which object a pixel belongs to and where that
object was — i.e. exactly the per-object correspondence section 2.3 shows is absent from the display
list, and section 4.1 shows cannot be recovered from the renderer alone.

**Saying it plainly: motion vectors cannot exist here while vertices arrive pre-projected from the
guest with no object identity.** `docs/runtime-render.md:101-103` and
`docs/widescreen-crazytaxi-study.md:41-73` establish the pre-projection; `display_list.h:36-56`
establishes the absence of identity.

Two secondary reasons, so the rejection does not rest on one point:

- **Jitter would fight the guest.** TAA jitters the projection sub-pixel per frame. Here the
  projection is the guest's — jittering it means patching `[0x0C2B0930]`/`[0x0C148634]` every frame
  (`widescreen-crazytaxi-study.md:294-303`), which changes what the guest computes, which is the one
  thing `widescreen-crazytaxi-study.md:780-783` warns must never leak into a differential-harness or
  golden-trace run.
- **It would ghost the HUD** unless the 2D separation of section 6 lands first.

**If part A's A2 and A5 land, a camera-only TAA becomes discussable** — camera motion vectors plus a
2D mask. It would still ghost every moving vehicle. **Estimate, conditional on part A: 5 / 10 / 20
days on top.** Not recommended now.

### 10.3 FXAA / SMAA post-process — **recommended**

| | |
|---|---|
| **What it needs** | A fullscreen pass over a finished colour image. **The pipeline already has exactly that**: `Presenter::draw` renders one fullscreen triangle sampling one texture (`present.vert`, `present.frag`, pipeline at `present.cpp:105-167`). FXAA is a replacement `present.frag` plus a push constant for the inverse resolution; SMAA 1x is three passes and two small precomputed lookup textures, which the texture upload path already knows how to do. |
| **Cost** | FXAA: under 0.5 ms at 960×720 on the reference M4 (**inferred**, it is a fixed ~12-tap kernel). SMAA 1x: roughly 2–3× that, still small. No new attachments at the offscreen size, no render-pass change, **no interaction with OIT whatsoever** because it runs after everything. |
| **What it costs in sharpness** | This is the real objection and it lands on the 2D layer. Crazy Taxi's HUD is small, high-contrast, axis-aligned text and icons — the exact input FXAA blurs worst, because its edge detector fires on every glyph stem. |
| **Why that is manageable here** | Three reasons. (1) **FXAA's blur is proportional to how much the image was already resampled** — applied *after* a correct box downsample from `--scale 2`+ there is far less aliasing left to chew on, so a lower edge threshold does less damage. (2) **SMAA 1x is markedly better than FXAA on text** because its pattern classification rejects the isolated-stem case FXAA smears; it is the right target even if FXAA ships first. (3) **If the 2D classification from section 6 / A5 exists, the HUD can be excluded outright** — write a stencil or an alpha-channel tag for 2D-classified geometry and skip AA there. That is the clean answer and it is, again, shared work. |
| **Estimate** | See 10.5. |
| **Verdict** | **Recommended**, behind the resolve fix. |

### 10.4 SSAA — already present, and the recommendation is to finish it

Not one of the three the brief lists, but it is the honest first answer (9.2). `--scale` already
supersamples; only the resolve is wrong. **This is the highest quality-per-day item in either half of
this study.**

### 10.5 Plan and estimate for part B

| # | Step | Files touched | Low | **Central** | High | What drives the spread |
|---|---|---|---|---|---|---|
| B1 | **Fix the resolve.** Set `Presenter::smooth` from a policy (nearest at `--scale 1`, box downsample above), and implement a proper N×N box filter in `present.frag` driven by a source/destination ratio push constant. | `render/src/vk/present.cpp:74-76,286-330`, `render/include/dream/render/vk/present.h:53`, `render/shaders/present.frag`, `render/shaders/present.vert:5-8`, `runtime/boot/boot_main.cpp:128-137` | 0.5 | **1** | 2 | Trivial in itself. The spread is deciding the policy and not regressing the deliberate nearest-neighbour look at `--scale 1`, plus the overlay interaction (B4). |
| B2 | **FXAA in the present pass**, behind `--antialias=fxaa`. | `render/shaders/` (new `fxaa.frag`), `render/src/vk/present.cpp`, `render/cmake/wrap_spirv.cmake`, `runtime/boot/boot_main.cpp` (flag) | 0.5 | **1** | 2 | A known shader. High end is a second pipeline object and the flag plumbing. |
| B3 | **SMAA 1x**, behind `--antialias=smaa`. Three passes, two lookup textures, an intermediate target. | as B2 plus `render/src/vk/resources.cpp`, new intermediate attachments | 1.5 | **2.5** | 5 | Three passes and two baked textures in a project with no precedent for either. Worth it for the text. |
| B4 | **Exclude the 2D layer from AA**, using the same classification as section 6 / A5. Also: the FPS counter and binding menu are composited into the CPU staging buffer *before* upload (`boot_main.cpp:273-281`), so they would be anti-aliased too unless moved after the AA pass. | `render/src/vk/present.cpp:286-300`, `runtime/boot/boot_main.cpp:268-283`, `render/src/vk/renderer.cpp` | 0.5 | **1** | 3 | Low if A5 exists. Standalone, the "overlay composited pre-upload" reshuffle is the real work. |
| B5 | **Tests, docs, CI.** A reference-image test per mode at `--scale 1` and `--scale 4`; update `docs/runtime-render.md`. | `render/tests/`, `docs/runtime-render.md` | 0.5 | **1** | 2 | |
| | **Total** | | **3.5** | **6.5** | **14** | |

**Rounded for quoting: 5 days central, range 3–10** if B3 (SMAA) is deferred and FXAA ships first;
**7 central** with SMAA included. The headline recommendation — **B1 alone, one day, for the largest
single quality gain** — should not be lost in the table.

### 10.6 How to verify — concretely

- **B1's decisive test is arithmetic, not aesthetic.** Render a synthetic frame of known content at
  `--scale 4`, read it back (`Offscreen::pixels()`), box-average it in the test on the CPU, and assert
  the presented image matches within 1 LSB. Today that test fails by construction; that failure *is*
  the bug report.
- **Edge-energy metric.** On a fixed captured frame, compute the mean absolute Laplacian over the
  image for each mode. AA must lower it on geometry edges; if it lowers it uniformly, it is blurring
  the whole picture and B4 is needed.
- **HUD byte-identity.** Crop the HUD bounding box with AA off and on. With B4 correct they are
  **byte-identical**. Without B4, the diff quantifies exactly how much sharpness the HUD lost, which
  is the number to argue about rather than opinions about screenshots.
- **Text legibility, as a measurement.** The copyright line on the title screen is already a known
  reference (`docs/runtime-render.md:157`: *"the copyright line is legible"*). Screenshot it in
  each mode at `--scale 1` and `--scale 4` and compare. It is the hardest case in the game and the
  repo already treats it as a bellwether.
- **Cost.** Time the present pass alone per mode. FXAA over ~0.5 ms at 960×720 means something is
  wrong.

### 10.7 What I could not determine

| unknown | how to find out |
|---|---|
| **Whether the nearest-neighbour resolve was deliberate above `--scale 1`.** The comment (`present.h:51-53`) justifies it for the guest's own pixels and says *"an upscaling mode will want the choice"* — which reads as "not yet wired up" rather than "chosen". I have not found a decision record either way. | Ask the owner; check whether any `--scale 4` screenshot in the repo's history shows the aliasing this predicts. |
| **How much aliasing is actually left after a correct box resolve at `--scale 4`.** It may be little enough that B2/B3 are not worth shipping at all. | Do B1, then look at the edge-energy metric before deciding on B2. **This is the right order and it may save 4 days.** |
| **Whether MoltenVK's `VK_FILTER_LINEAR` minification behaves as expected past 2:1.** Metal's sampler minification without a mip chain is the same under-sampling; the box filter in the shader sidesteps it entirely, which is why B1 specifies a shader filter rather than just flipping `smooth`. | The arithmetic test above, run on macOS. |

---

## 11. Interactions

### 11.1 A and B with each other

- **They compete for the same host budget.** `docs/runtime-render.md:372` records `--scale 4` already
  runs at about real time on the reference M4. Frame generation needs spare host time (section 7)
  and `--scale 4` has none. **Inferred: `--scale 4` and frame generation are mutually exclusive on
  the reference machine**, and the launcher should say so rather than letting a user discover it as
  stutter.
- **They share the present pass.** B1–B3 and A4 all add work to `Presenter`. Order matters: AA must
  run on the *real* frame before reprojection, or each generated frame gets its own independently
  anti-aliased edges and the result shimmers. **The correct order is: render → box resolve (B1) →
  AA (B2/B3) → store as the reprojection history (A3) → warp (A4) → composite unwarped 2D → present.**
  Establishing that order once, in the design, is cheap; discovering it after both are built is not.
- **B helps A.** A reprojection warp resamples the source image; a source that is already box-resolved
  and anti-aliased warps to a better result than a point-sampled one.
- **A helps B.** If A5 lands, B4 is nearly free.

### 11.2 With `docs/widescreen-crazytaxi-study.md`

Read; **not redone**. Shared code, explicitly:

| shared piece | widescreen | this study |
|---|---|---|
| **The `[hooks]` mechanism** (`translator/src/emit/emit.cpp`, `runtime/src/hooks/`) | step 2, `:797`, 1 / 2 / 3.5 days | step A1, same numbers. **Count once.** |
| **2D/3D classification** (`fn_0x0C07D018`, the sprite/depth classifier, DMA span tracking) | step 6, `:801`, the widest estimate in that study | steps A5 and B4. Part A's requirement is weaker (section 6). **Build once, to widescreen's stricter bar.** |
| **The viewport block** `0x0C148618..0x0C148638` and the pixel scales `0x0C2B0930/34` | patched, to widen | **read only**, to unproject. No conflict; they compose. |
| **`FrameGeometry` and the offscreen extent** (`boot_main.cpp:128-137`, `renderer.cpp:371-378`) | widened | must follow whatever width widescreen sets. A4's unprojection must read `FrameGeometry`, not assume 640×480. |
| **The differential-harness guard** (`widescreen-crazytaxi-study.md:780-783`) | required | required, for the same reason: A2's hooks must be inert, and A4 must never run, in any oracle run. |

**Sequencing recommendation:** do widescreen's step 2 (`[hooks]`) and step 6 (2D classification)
first, whichever feature pays for them. They unblock three features, and `widescreen-crazytaxi-study.md:818-820`
already says the same thing: *"If step 1 is blocked on the in-city crash, do step 2 anyway — it is
independently useful and is the thing the plan will want for texture replacement and the frame-rate
work too."*

---

## 12. Risks

### 12.1 Audio — the one that bites first

**The AICA is paced against the guest clock, not the host.**
`runtime/include/dream/runtime/aica/aica.h:25`:

```cpp
    static constexpr std::uint64_t kSh4CyclesPerSample = 200'000'000 / 44100;  // 4535
```

scheduled on the virtual clock (`aica.h:51-53`), firing `on_sample`
(`runtime/src/aica/aica.cpp:240-241`), which the launcher forwards to the SDL sink
(`runtime/boot/boot_main.cpp:1615-1619`). `audio/include/dream/audio/sink.h:1-12` is explicit about
the hazard: *"run the guest faster than real time and samples pile up, slower and the device runs
dry."* Overflow is handled by discarding whole 512-frame blocks
(`audio/src/sdl_sink.cpp:66-73`); **starvation is not counted at all**, which
`docs/future-enhancements.md:138-140` already flags.

**Part A as recommended does not change the guest clock, so it does not change sample production.**
That is the main reason to keep the guest at 59.83 Hz beyond the gameplay argument. But two
second-order risks remain and both are real:

- **The pacer's sleep becomes a work loop.** Today the guest thread sleeps
  (`boot_main.cpp:1583`); with a nested present pump it renders instead. If the pump overruns its
  budget, the guest falls behind, sample *production* slows, and the device starves — the failure
  direction nobody is counting. **Mitigation: the pump must check the guest's next deadline before
  each generated frame and yield, and the sink needs the starvation counter first.**
- **Vsync on a 60 Hz display.** Presenting 144 generated frames into a FIFO swapchain on a 60 Hz
  panel blocks 2.4× per guest frame and holds the guest *below* real time — the same trap
  `docs/future-enhancements.md:135-140` describes, made worse. **Mitigation: query the refresh rate
  and cap the generation factor (step A7); never generate more frames than the display can show.**

Part B carries none of this: it adds GPU work to a pass that is already on the critical path but does
not change who paces whom.

### 12.2 Golden traces and `dream_emit_tests`

**Lower risk than the brief fears, and worth saying so.** The golden traces are **SH-4 state**, not
frame output: `docs/differential-harness.md:80-82` — *"`tools/oracle/write_golden.py` turns a run's
oracle dumps into one text file per case under `tests/sh4/golden/` (inputs plus the interpreter's
final state, 796 KB for 78 cases), and `tests/sh4/test_golden.cpp` in `dream_emit_tests` replays them
all: same image, fill, registers and …"*. Nothing in either feature touches emitted-code semantics.

**The real exposure is the hooks.** A2's entry hooks run *inside* the guest's instruction stream. If
they are compiled in unconditionally, or if they perturb `Ctx` or `cycles`, the oracle stops meaning
anything. This is the identical hazard `widescreen-crazytaxi-study.md:780-783` records for the
widescreen patch, and it deserves the identical treatment: **off by default, a hard guard, and a
paragraph in `docs/differential-harness.md`. A one-line guard, but forgetting it would be
expensive.**

The per-frame memory hash (`boot_main.cpp:1700-1707`) is keyed on `sys.spg.frames()` and hashes guest
writes, so it is unaffected by presentation rate — **provided** the hooks write nothing to guest
memory. They must not.

### 12.3 Screenshots, `--capture`, and the counters

`--screenshot-at N` and `--capture-at N` trigger on the **presented** frame counter
(`boot_main.cpp:317-319`, incremented at `:308`; help at `:1081`). With frame generation on, `N` no
longer means what every existing script and every line of documentation assumes: the Nth presented
frame becomes the Nth *generated-or-real* frame, and worse, **a screenshot could land on a
manufactured frame**, producing a reference image with disocclusion artefacts in it.

**Mitigation, and it is not optional: re-key both on real frames, or refuse to screenshot a generated
frame.** Given `docs/runtime-render.md:375-377` records `--screenshot-at` as the mechanism by which a
past regression was verified, silently changing its meaning would break the project's own
verification story. Part B has a milder version of the same issue: a screenshot after B2/B3 is
anti-aliased, so any committed reference image needs regenerating or the mode pinning off.

### 12.4 `--framebuffer-writeback`

Off by default and for a good reason (`docs/runtime-render.md:313-322`: on Crazy Taxi it erases the
game's logo). Both features interact with it:

- **Part A:** a generated frame must **never** be written back into video memory. It is not a frame
  the guest rendered, and `encode_framebuffer` (`boot_main.cpp:215-218`) would hand the guest
  pixels its own logic never produced. The write-back path is driven from `Live::render`
  (`boot_main.cpp:182`), which only runs on a real render, so **the default structure is already
  correct** — but if the presenter ever gains write-back responsibility this becomes a live bug.
- **Part B:** the AA pass runs in the presenter, after `write_back()`, so `--framebuffer-writeback`
  writes the un-anti-aliased image. That is right — the guest should see its own pixels — but it
  means a render-to-texture would sample a non-AA'd frame while the screen shows an AA'd one. Worth a
  sentence in the documentation rather than a fix.

### 12.5 Determinism

`docs/runtime-interrupts.md:5-8`: *"Nothing in the runtime reads a wall clock, so a run is a
deterministic function of the guest instruction stream."* Frame generation introduces a component
whose output depends on host timing — how many frames got generated between two guest frames. **That
must stay strictly downstream of the guest**: it may read guest state, it must never write it, and
its count must never feed back into `Ctx::cycles` or the pacer's target. If that invariant holds,
determinism is preserved and the golden traces are safe. **It is the single invariant the whole of
part A rests on, and it should be written down in `docs/runtime-render.md` before any code is
written.**

---

## 13. Summary of estimates

| | low | **central** | high |
|---|---|---|---|
| **A. Frame generation** (depth reprojection, extrapolating, 2D excluded) | 17 | **35** | 75 |
| — of which is engineering (A1–A3, A7–A8) | 13 | 21 | 31 |
| — of which is research (A4, A6) | 7 | 16 | 45 |
| — minimum honest slice (8.5) | | **13** | |
| — less `[hooks]` if widescreen builds it first | −1 | **−2** | −3.5 |
| **B. Anti-aliasing** (resolve fix + FXAA) | 3 | **5** | 10 |
| — with SMAA 1x instead of FXAA | 4.5 | **7** | 14 |
| — **the single best line item: B1, the resolve fix, alone** | 0.5 | **1** | 2 |
