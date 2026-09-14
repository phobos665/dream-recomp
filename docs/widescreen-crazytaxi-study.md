# True widescreen (Hor+) for Crazy Taxi: where the projection lives and what it costs (study)

Written 2026-09-14 from a reading pass over `docs/runtime-render.md`, `render/`, `runtime/boot/`,
`translator/src/config/`, a static analysis pass over the owner's `1ST_READ.BIN`
(`games/crazytaxi/extracted/fs/1ST_READ.BIN`, SHA-1 as pinned in `games/crazytaxi/crazytaxi.toml:13`),
the KallistiOS checkout at `kallistios/` (BSD, quotable), and Flycast's cheat and renderer sources.
**No source was changed and no code was written.** Nothing from the Katana SDK directory was opened;
every Dreamcast API claim below is either from KallistiOS, from Flycast, or from disassembly of the
owner's own disc.

Every guest address below is in the link-space the TOML declares (`link_address = 0x0C010000`,
`games/crazytaxi/crazytaxi.toml:19`). Flycast's cheat table uses the P1 alias `0x8C…`; the two name
the same RAM.

---

## 0. Verdict

**True Hor+ is feasible, and the lever is unusually clean — cleaner than in any emulator.** The
game's projection is not a matrix hidden in a library we cannot name. It is a **nine-word viewport
block in guest RAM** whose two halves are computed independently, one horizontal and one vertical.
Section 2 has the arithmetic, the disassembly and the addresses.

**Recommended approach, in one sentence:** widen the offscreen render target, and patch the guest's
own viewport block so that the horizontal *pixel* scale is held constant while the viewport *width*
grows — which is exactly Hor+ by construction, because the vertical half of the block is never
touched.

**What that costs:** the 3D part is small — about 3 focused engineer-days, most of it in building a
hook mechanism the repo has schema for but no implementation of. The 2D/HUD part is the whole rest
of the job and it is the part that decides whether this meets the brief, because the requirement
says "no HUD distortion" and Crazy Taxi's 2D is authored against a 640-wide screen. Headline:
**11 days central, 7 low, 21 high.**

**What is explicitly not recommended:** scaling vertex X in the renderer (a stretch), rendering two
side viewports (impossible — one TA stream per frame), and Flycast's anamorphic cheat as-is (true
Hor+ for the world, but it stretches the HUD by 4/3, which the brief forbids).

---

## 1. The spine: vertices arrive pre-projected, so the renderer cannot widen anything

This had to be settled first because it decides whether the work is a renderer change (cheap) or a
guest change (not).

**They arrive pre-projected. Confirmed four ways.**

### 1.1 Our own documentation says so

`docs/runtime-render.md:101-103`:

> - **The coordinates are already in screen space.** There is no model or view transform: x and y are
>   pixels and z is 1/w. The rasteriser is given w = 1 so it does not perspective-divide, and
>   perspective correction is done by hand the way the hardware does it, multiplying attributes by
>   1/w in the vertex shader and dividing in the fragment shader.

### 1.2 The decoder's own type says so

`render/include/dream/render/display_list.h:34-38`:

```cpp
// A vertex as the renderer wants it. Screen-space x and y in pixels, z is 1/w (larger is nearer),
// colours are ARGB8888 whatever the source encoding was.
struct Vertex {
    float x = 0, y = 0, z = 0;
```

### 1.3 The decoder does no arithmetic on the position

`render/src/display_list.cpp:288-290` is the whole of the position path:

```cpp
    v.x = as_float(w[1]);
    v.y = as_float(w[2]);
    v.z = as_float(w[3]);
```

Three raw reinterprets of the TA parameter words. The sprite path (`:358-368`) is the same, plus the
fourth corner solved on the plane the other three define. There is no matrix anywhere in
`render/src/display_list.cpp`.

### 1.4 The renderer's "projection" is an affine screen-to-NDC map and nothing else

`render/src/vk/renderer.cpp:371-378`:

```cpp
    PushConstants push{};
    push.scale[0] = 2.0f / geometry.width;
    push.scale[1] = 2.0f / geometry.height;
    push.offset[0] = -1.0f - 2.0f * geometry.left / geometry.width;
    push.offset[1] = -1.0f - 2.0f * geometry.top / geometry.height;
    (void)target;
```

consumed at `render/shaders/geometry.vert:30`:

```glsl
    gl_Position = vec4(in_pos.xy * push.scale + push.offset, 0.0, 1.0);
```

`FrameGeometry` is declared at `render/include/dream/render/vk/renderer.h:22-29` with
`float width = 640, height = 480;` and its own comment already concedes the point: *"The region of
the guest's screen to show."* A region. Not a frustum.

**So what does changing `geometry` actually do?** Exactly two things, neither of them Hor+:

| change | result |
|---|---|
| `geometry.width = 853.33`, `geometry.left = -106.67`, offscreen extent 853×480 | the guest's 640-wide picture sits 1:1 in the middle of an 853-wide frame with **empty margins**. Nothing new appears, because the game never drew anything out there. |
| `geometry.width = 640` unchanged, offscreen extent 853×480 | the viewport (`render/src/vk/offscreen.cpp:176-181`) stretches 640 guest units across 853 pixels: a **horizontal stretch**. Everything is 1.33× fatter. |

That is the whole space of renderer-side outcomes. **A renderer-side matrix change cannot widen the
field of view. It can only stretch or add blank margin.** The only way to fill those margins with
world is to change what the guest computes.

### 1.5 And the hardware confirms there is nowhere else to look

KallistiOS is unambiguous that the PowerVR2 has no transform stage at all
(`kallistios/kernel/arch/dreamcast/include/dc/pvr.h:16-25`):

> This file provides support for using the PVR 3D hardware in the Dreamcast.
> Note that this does not handle any sort of perspective transformations or
> anything of the like. This is just a very thin wrapper around the actual
> hardware support.

The TA's vertex is a bare `float x, y, z` with no `w`
(`kallistios/kernel/arch/dreamcast/include/dc/pvr.h:419-437`), and the divide happens on the SH-4's
FPU inside the transform macro itself
(`kallistios/kernel/arch/dreamcast/include/dc/matrix.h:145-160`):

```c
#define mat_trans_single(x, y, z) { \
        ...
        __asm__ __volatile__( \
                              "fldi1	fr3\n" \
                              "ftrv	xmtrx,fv0\n" \
                              "fldi1	fr2\n" \
                              "fdiv	fr3,fr2\n" \
                              "fmul	fr2,fr0\n" \
                              "fmul	fr2,fr1\n" \
```

One `ftrv xmtrx,fv0`, then `1/w`, then two multiplies. The result is `(x/w, y/w, 1/w)` in pixels.
A sweep of KOS's full PVR register list
(`kallistios/kernel/arch/dreamcast/include/dc/pvr/pvr_regs.h:76-140`) turns up no projection,
frustum, FOV, viewport-scale or aspect register; the nearest candidates are `PVR_PCLIP_X`/`_Y`
(a pixel scissor — can only crop) and `PVR_SCALER_CFG` (FSAA plus a *vertical* scale factor only).
A case-insensitive grep of the whole KOS tree for `widescreen|anamorphic|aspect.ratio|16:9` returns
zero hits outside vendored third-party image headers.

**Conclusion for the rest of this document: the field of view exists in exactly one place on a
Dreamcast — the matrix the SH-4 loads into `XMTRX`, and the scale/offset pair the game applies after
the divide. In Crazy Taxi both are reachable, and section 2 says where.**

---

## 2. Where Crazy Taxi sets up its projection

### 2.1 It is not a Katana entry point, so there is nothing to HLE

`games/crazytaxi/symbols.tsv` holds 366 FID/Ghidra names. Every rendering name in it is the
low-level kamui TA layer:

```
0x0c148d60	_kmSetBackGroundPlane	fid
0x0c15b0c0	_kmProcessVertexRenderState	fid
0x0c15b880	_kmSetVertexRenderState	fid
0x0c073b70	_kmSetFogTable	ghidra
0x0c153280	_kmiDMAtoTARequest	fid
```

There is **no `_kgl*` symbol of any kind**, no matrix function, no perspective function, no viewport
function. Grepping the file for `persp|proj|matrix|viewport|camera|frustum|aspect|view` returns three
false positives (`_vmsfs_isformat`, `_kmiDMAtoTARequest`). Crazy Taxi is an arcade port and brings its
own transform-and-lighting code; the FID databases have nothing to match it against.

Measured evidence that the T&L is in-game: the binary contains **150 `ftrv`, 27 `fipr`, 15 `fsrra`,
78 `frchg` and 263 `fschg`** instructions, essentially all of them in `0x0C076000`–`0x0C086000`.
`0x0C076180`–`0x0C0761D2` is a textbook point-transform helper trio (`fmov @r4+` ×3, `fldi1 fr7`,
`ftrv xmtrx,fv4`, store 4 / store 3).

**So option (b) from the brief — "HLE a Katana projection entry point" — has nothing to bind to. It
is dead on arrival, not expensive.**

### 2.2 The viewport block, found by constant-chasing

Scanning the image for the IEEE-754 words `0x43A00000` (320.0f), `0x43700000` (240.0f),
`0x44200000` (640.0f), `0x43F00000` (480.0f) and `0x3F400000` (0.75f), then resolving which
instructions reference each (SH-4 loads FP constants as `mova` + `fmov @r0,FRn`, or `mov.l` of a
pointer + `fmov @Rn`), the 640/480 pairs cluster in exactly one place that is read by the
rendering code rather than by UI layout code:

```
0c148618: 44200000  640      <- read at 0xc07815e, 0xc078394, 0xc078610, 0xc078814, 0xc078af4  (+ written)
0c14861c: 44200000  640      <- read at 0xc07815c, 0xc078392, 0xc07860e, 0xc078812, 0xc078afe  (+ written)
0c148620: 44200000  640      <- read at 0xc07819c, 0xc0783d2, 0xc078648, 0xc07883e, 0xc078b08  (+ written)
0c148624: 00000000  0
0c148628: 00000000  0
0c14862c: 44200000  640      <- read at 12 sites incl. 0xc078160, 0xc07d16c, 0xc080e5e
0c148630: 43f00000  480      <- read at 12 sites incl. 0xc078196, 0xc07d17a, 0xc080e60
0c148634: 00000000  0        <- read at 9 sites incl. 0xc080fea, 0xc08210a, 0xc0849be
0c148638: 00000000  0        <- read at 8 sites incl. 0xc080ffe, 0xc08211e, 0xc0849cc
```

These addresses are inside the loaded image, but the image lives in writable RAM from
`0x8C010000`, and the code below **writes** them. They are initialised data, not constants.

### 2.3 What computes them: `0x0C078AC0`–`0x0C078B26`

Disassembly (`dream-translate disasm --image games/crazytaxi/extracted/fs/1ST_READ.BIN
--base 0x0C010000 --start 0x0C078AC0 --count 60`), with the literal pool at `0x0C078B88`–`0x0C078BBC`
resolved into the register comments:

```
0c078ac0  d232  mov.l 0xc078b8c,r2      ; r2 = 0x0C2B0944
0c078ac2  d331  mov.l 0xc078b88,r3      ; r3 = 0x0C2B0940
0c078ac4  d132  mov.l 0xc078b90,r1      ; r1 = 0x0C2B0938
0c078ac6  f3fa  fmov fr15,@r3
0c078ac8  f2da  fmov fr13,@r2
0c078aca  d233  mov.l 0xc078b98,r2      ; r2 = 0x0C148638   (viewport Y origin)
0c078acc  d331  mov.l 0xc078b94,r3      ; r3 = 0x0C2B093C
0c078ace  d633  mov.l 0xc078b9c,r6      ; r6 = 0x0C148630   (viewport HEIGHT)
0c078ad0  d534  mov.l 0xc078ba4,r5      ; r5 = 0x0C14862C   (viewport WIDTH)
0c078ad2  d733  mov.l 0xc078ba0,r7      ; r7 = 0x0C148634   (viewport X origin)
0c078ad4  f1ea  fmov fr14,@r1
0c078ad6  f3ca  fmov fr12,@r3
0c078ad8  f26a  fmov fr6,@r2            ; [0x0C148638] = y origin
0c078ada  f65a  fmov fr5,@r6            ; [0x0C148630] = height
0c078adc  8545  mov.w @(10,r4),r0
0c078ade  2008  tst r0,r0
0c078ae0  8906  bt 0xc078af0
0c078ae2  f3fc  fmov fr15,fr3
0c078ae4  f342  fmul fr4,fr3
0c078ae6  f2ec  fmov fr14,fr2
0c078ae8  f242  fmul fr4,fr2
0c078aea  f73a  fmov fr3,@r7            ; [0x0C148634] = x origin * k
0c078aec  a002  bra 0xc078af4
0c078aee  f52a  fmov fr2,@r5            ; [0x0C14862C] = width * k
0c078af0  f7fa  fmov fr15,@r7           ; [0x0C148634] = x origin
0c078af2  f5ea  fmov fr14,@r5           ; [0x0C14862C] = width
0c078af4  d32d  mov.l 0xc078bac,r3      ; r3 = 0x0C148618   (normalised H scale)
0c078af6  f258  fmov @r5,fr2            ; fr2 = width
0c078af8  f338  fmov @r3,fr3            ; fr3 = normalised H scale
0c078afa  d42b  mov.l 0xc078ba8,r4      ; r4 = 0x0C2B0930   (H PIXEL scale)
0c078afc  f232  fmul fr3,fr2
0c078afe  d22d  mov.l 0xc078bb4,r2      ; r2 = 0x0C14861C   (normalised V scale)
0c078b00  d52b  mov.l 0xc078bb0,r5      ; r5 = 0x0C2B0934   (V PIXEL scale)
0c078b02  f42a  fmov fr2,@r4            ; [0x0C2B0930] = width  * [0x0C148618]
0c078b04  f268  fmov @r6,fr2            ; fr2 = height
0c078b06  f328  fmov @r2,fr3            ; fr3 = normalised V scale
0c078b08  d62b  mov.l 0xc078bb8,r6      ; r6 = 0x0C148620
0c078b0a  f232  fmul fr3,fr2
0c078b0c  f42c  fmov fr2,fr4
0c078b0e  f52a  fmov fr2,@r5            ; [0x0C2B0934] = height * [0x0C14861C]
0c078b10  f548  fmov @r4,fr5
0c078b12  f545  fcmp/gt fr4,fr5
0c078b14  8b04  bf 0xc078b20
0c078b16  a004  bra 0xc078b22
0c078b18  f65a  fmov fr5,@r6            ; [0x0C148620] = max(H, V) pixel scale
0c078b20  f64a  fmov fr4,@r6
0c078b22  d426  mov.l 0xc078bbc,r4      ; r4 = 0x0C2B08B0   <-- Flycast patches THIS
0c078b24  bf44  bsr 0xc0789b0
```

A second, independent site does the same thing while also *computing* the normalised scales, at
`0x0C078150`–`0x0C0781B0`. Its literal pool at `0x0C0783DC` resolves the same way, with
`0x0C0783DC = 0.5f`:

```
0c078156  c7a1  mova 0xc0783dc,r0       ; the constant 0.5
0c07815a  f403  fdiv fr0,fr4            ; fr4 = fr14 / fr0   (a cotangent, from the jsr @r14 above)
0c078174  f233  fdiv fr3,fr2            ; fr2 = [sp+0] / [sp+8]
0c07817e  f012  fmul fr1,fr0            ;   * [sp+20]
0c078180  f052  fmul fr5,fr0            ;   * 0.5
0c078182  f40a  fmov fr0,@r4            ; [0x0C148618] = (a/b) * c * 0.5
0c078186  f342  fmul fr4,fr3
0c078188  f352  fmul fr5,fr3
0c07818a  f63a  fmov fr3,@r6            ; [0x0C14861C] = a * cot * 0.5
0c07818c  f348  fmov @r4,fr3
0c07818e  f238  fmov @r3,fr2            ; r3 = 0x0C14862C (width)
0c078192  f232  fmul fr3,fr2
0c078194  f52a  fmov fr2,@r5            ; [0x0C2B0930] = [0x0C148618] * width
0c078198  f368  fmov @r6,fr3
0c07819a  f228  fmov @r2,fr2            ; r2 = 0x0C148630 (height)
0c07819e  f232  fmul fr3,fr2
0c0781a2  f42a  fmov fr2,@r4            ; [0x0C2B0934] = [0x0C14861C] * height
0c0781a6  f545  fcmp/gt fr4,fr5
0c0781ae  f65a  fmov fr5,@r6            ; [0x0C148620] = max of the two
```

### 2.4 The model, stated plainly

```
[0x0C148618]  normalised horizontal scale   = 0.5 · cot(fovy/2) / aspect
[0x0C14861C]  normalised vertical   scale   = 0.5 · cot(fovy/2)
[0x0C14862C]  viewport width  in pixels     = 640.0
[0x0C148630]  viewport height in pixels     = 480.0
[0x0C148634]  viewport X origin             = 0.0
[0x0C148638]  viewport Y origin             = 0.0

[0x0C2B0930]  horizontal PIXEL scale        = [0x0C14862C] · [0x0C148618]
[0x0C2B0934]  vertical   PIXEL scale        = [0x0C148630] · [0x0C14861C]
[0x0C148620]  max of the two                (guard band / LOD reference)
```

With a 4:3 aspect and the game's own field of view, `[0x0C148618] = 0.5` gives
`[0x0C2B0930] = 320.0` — which is exactly the value Flycast's community cheat patches to `240.0`
(section 5.2). **The horizontal and vertical halves are computed by separate expressions from
separate inputs. That is what makes Hor+ available: you can move one without moving the other.**

### 2.5 The consumers: three vertex pipelines and one sprite path

`[0x0C2B0930]` and `[0x0C2B0934]` are read by ten and eight sites respectively. Three of them are
hand-scheduled per-vertex pipelines:

- `0x0C080E20` — the main one. Loads `[0x0C2B0930]`/`[0x0C2B0934]` at `0x0C080E52`/`0x0C080E60`
  and `[0x0C14862C]`/`[0x0C148630]` at `0x0C080E5C`/`0x0C080E5E` into stack slots, then runs a
  software-pipelined `ftrv xmtrx,fv4` loop at `0x0C080F1C` with `pref` prefetch and `fschg` pair
  moves — i.e. it is writing TA parameters directly. The block at `0x0C080FE8`–`0x0C081030` is a
  chain of `fsub`/`fcmp/gt` against `[0x0C148634]` and `[0x0C148638]`: **this is the frustum clip
  test, and it reads the same viewport block.**
- `0x0C081F72`/`0x0C081F80` — a second pipeline.
- `0x0C08492E`/`0x0C08493C` — a third.

And the 2D/sprite path is one function with a very large fan-in:

- **`0x0C07D018`** — 100+ call sites, all in `0x0C02F000`–`0x0C05F000` (the UI and menu code where
  every literal `320.0f`/`240.0f` in the binary lives). It builds a four-corner quad via
  `fn_0x0C07D7C4` (`0x0C07D166`), then at `0x0C07D16A`–`0x0C07D184`:

```
0c07d16a  d20c  mov.l 0xc07d19c,r2   ; r2 = 0x0C2B4B74  (1 / app screen width)
0c07d16c  d30c  mov.l 0xc07d1a0,r3   ; r3 = 0x0C14862C  (viewport width, 640.0)
0c07d16e  f338  fmov @r3,fr3
0c07d170  f228  fmov @r2,fr2
0c07d172  f232  fmul fr3,fr2
0c07d176  ff27  fmov fr2,@(r0,r15)   ; [sp+8] = 640 / app_w
0c07d178  d10a  mov.l 0xc07d1a4,r1   ; r1 = 0x0C2B4B78  (1 / app screen height)
0c07d17a  d30b  mov.l 0xc07d1a8,r3   ; r3 = 0x0C148630  (viewport height, 480.0)
0c07d184  ff07  fmov fr0,@(r0,r15)   ; [sp+4] = 480 / app_h
```

  followed by a four-iteration loop at `0x0C07D1AC`–`0x0C07D1D8`:

```
0c07d1b8  f3f6  fmov @(r0,r15),fr3   ; the 640/app_w factor
0c07d1ba  f238  fmov @r3,fr2         ; xs[i]  (sp+88 + i*4)
0c07d1bc  f232  fmul fr3,fr2
0c07d1be  f32a  fmov fr2,@r3         ; xs[i] *= 640/app_w
0c07d1cc  f3f6  fmov @(r0,r15),fr3   ; the 480/app_h factor
0c07d1d2  f32a  fmov fr2,@r3         ; ys[i] *= 480/app_h
```

  The "app screen" dimensions are registered separately: `fn_0x0C07C69C` calls `fn_0x0C07CFB6` with
  `fr4 = 640.0` (`0x0C07C714`) and `fr5 = 480.0` (`0x0C07C710`), and `fn_0x0C07CFB6` stores
  `1/w`, `1/h`, `w`, `h` into `0x0C2B4B74`, `0x0C2B4B78`, `0x0C2B4B7C`, `0x0C2B4B80`.

  **The load-bearing consequence: the sprite path multiplies X by `[0x0C14862C]/app_w`. Raise
  `[0x0C14862C]` to 853.33 and every 2D quad in the game gets 1.333× wider.** That is exactly the
  HUD distortion the brief forbids, and it is the reason the 2D work in section 6 exists.

### 2.6 Confidence

| claim | confidence | why |
|---|---|---|
| Vertices reach our renderer pre-projected | **certain** | code, docs, KOS, and the decoder does no arithmetic |
| `0x0C14862C`/`0x0C148630` are the viewport pixel width/height | **high** | written as a pair beside the origins, multiplied into the pixel scales, and read by every drawing path |
| `0x0C148618`/`0x0C14861C` are the normalised H/V scales | **high** | `0.5 ×` a cotangent and an aspect quotient, then multiplied by width/height |
| `0x0C2B0930`/`0x0C2B0934` are the H/V pixel scales | **high** | product of the two above; read by all three vertex pipelines |
| `[0x0C2B0930]` is 320.0 at run time in 4:3 | **high (inferred)** | arithmetic identity `640 × 0.5`, corroborated by Flycast's cheat writing 320→240 nearby |
| `0x0C2B08B0` is the projection/camera object those globals belong to | **medium** | it is passed to `fn_0x0C0789B0` immediately after the block is recomputed, and `0x0C2B0930` sits at `+0x80` from it. I did not disassemble its writer. |
| `0x0C148634`/`0x0C148638` are the viewport origins | **medium** | zero, adjacent to width/height, consumed by the clip test — plausible but not proven to be the screen centre |
| `fn_0x0C07D018` is the game's 2D/sprite choke point | **medium-high** | 100+ callers all in UI address space, builds 4 corners, applies UV flip flags — but a 3D billboard could use the same function |
| Which RAM word the *screen centre* (+320 / +W/2) is added from | **not determined** | see section 9 |

---

## 3. How we could intervene: the four options, judged

### (a) Patch the guest's viewport block — **recommended**

Set, per frame or on a hook:

```
[0x0C14862C] = W                       (853.333 for 16:9 at 480 lines)
[0x0C148618] = old_value × 640 / W     (× 0.75 for 16:9)
```

so that `[0x0C2B0930] = W × [0x0C148618]` comes out **unchanged at 320.0**, while the viewport is
wider. `[0x0C148630]`, `[0x0C14861C]` and `[0x0C2B0934]` are never touched.

- **Hor+ or stretch?** True Hor+, by construction. The horizontal pixels-per-tangent is literally the
  same number before and after; only the width of the region changes. Vertical is bit-identical.
- **Does the clip test follow?** Almost certainly yes, and this is the big prize. The clip chain at
  `0x0C080FE8` reads the same viewport block, so widening the viewport widens the clip planes in the
  same instruction stream. That is what kills the pop-in that Flycast's renderer-side widescreen
  suffers from. **Needs measurement, not assumption** — section 8.3.
- **Cost:** two 32-bit stores. The cost is entirely in *where you put them* — see section 4.
- **Downside:** it moves `[0x0C14862C]`, which the 2D path also reads (2.5). That has to be handled,
  and it is the expensive part of the job (section 6.2).

### (a′) Patch only the horizontal scale, Flycast-style — **rejected, but it is the fallback**

`[0x0C2B0930] ×= 0.75` (or `[0x0C148618] ×= 0.75`) with the viewport left at 640×480, then stretch
the 640-wide frame to 16:9 on present. This is exactly what Flycast's cheat does (5.2).

- **Hor+ or stretch?** True Hor+ for the **world**. The 3D is squashed into 640 columns and the
  display stretch undoes it exactly.
- **Why rejected:** the HUD is squashed too and the display stretch does not know it should not
  un-squash it. Net effect: HUD 1.333× wider than it should be. The brief says no HUD distortion.
- **Why it is still the fallback:** it is a two-line change, it is proven on real hardware and in
  Flycast, and it gets the world right. If the 2D work in section 6 overruns, shipping (a′) behind a
  flag is better than shipping nothing.

### (b) HLE a Katana projection entry point — **rejected, nothing to bind to**

There is no `_kgl*` symbol and no matrix symbol in `games/crazytaxi/symbols.tsv` (2.1). The
projection is in the game's own code. And the `[hle]` table is not implemented anyway (section 4.1).
Cost to make this work: everything in option (a), plus a naming exercise that buys nothing.

### (c) Scale vertex X in the renderer after the fact — **rejected, it is a stretch by definition**

Multiplying the decoded `Vertex::x` about the screen centre by `k` in `render/src/display_list.cpp`
or in `geometry.vert` makes every object `k` times wider in the same frame. It reveals nothing,
because the guest never emitted the geometry that would live at the new edges, and it clipped away
what little it did emit out there. It also widens the HUD. This is the "stretch" the brief rules out,
just implemented at a different layer.

There is one *legitimate* renderer-side X translate — moving the 2D layer to the centre of a widened
frame — and that is part of the recommendation (6.2), not this option.

### (d) Render two side viewports — **rejected, physically impossible here**

The guest produces **one** TA parameter stream per render (`pvr::Core::on_render`,
`runtime/include/dream/runtime/pvr/core.h:95-97`, delivering `const std::vector<uint32_t>& stream`).
There is no camera to re-aim and re-submit, because the geometry is already flattened to pixels by
the time we see it. Drawing the same stream twice at different offsets duplicates the 4:3 image; it
does not extend it. To get genuinely different side viewports you would have to make the guest render
three times per frame, which triples its frame cost, breaks its own double-buffering assumptions
(`docs/runtime-render.md:283-288`) and still needs the projection patch to aim each pass. It is
strictly worse than (a) in every dimension.

---

## 4. What hook mechanisms exist today (short answer: none)

### 4.1 `[hooks]` and `[hle]` are schema without implementation

Both tables are declared (`translator/include/dream/translator/config/game_config.h:40-41`):

```cpp
    std::map<std::uint32_t, std::string> hle;    // guest address -> runtime handler name
    std::map<std::uint32_t, std::string> hooks;  // guest address -> hook name
```

parsed and validated (`translator/src/config/game_config.cpp:163-164`), documented
(`docs/game-config.md:42-48`), and tested (`translator/tests/test_config.cpp:41,70`). And then
**nothing reads them.** `grep -rn "cfg.hooks\|cfg.hle" translator/src` returns nothing; the emitter
(`translator/src/emit/emit.cpp`) and the `game` driver (`translator/src/main.cpp:467-520`) never
mention either field. Crazy Taxi's own tables are empty
(`games/crazytaxi/crazytaxi.toml:100-105`).

So "use the existing hook mechanism" is not available. It has to be built. The good news is that the
emitter already demonstrates the exact capability under a different name.

### 4.2 The proof that it is cheap: `--replay-hooks`

`translator/src/emit/emit.cpp:298-300` already plants a call at the entry and the exit of a guest
function when `EmitOptions::replay_hooks` is set
(`translator/include/dream/translator/emit.h:49`), and the runtime side is specified at
`runtime/include/dream/runtime/devinterp/replay.h:45-47`:

> Called at the entry and the exit of a guest function by hooks the emitter plants under
> `--replay-hooks`.

Wiring `cfg.hooks` to emit `dream_hook_<name>_entry(c, m)` / `..._exit(c, m)` around the named
function is the same code path with a different predicate and a different callee name. That is the
single most reusable piece of this whole study, and it is why option (a) is cheap.

### 4.3 The patch target is live memory, not a folded literal — this matters

`docs/emitter-design.md:56-59`:

> Literal pools: a `MOV.L @(disp,PC)` whose target lies inside the image is folded to a constant
> **only** if the analysis marks that address read-only (never a store target and not inside a
> writable data section). Otherwise it is a memory read.

Every read of the viewport block in section 2 is an ordinary load through a pointer
(`fmov @r3,fr3` with `r3` from a literal, or `mov.l @r0,r1`), **not** a PC-relative literal load —
and `0x0C14862C`, `0x0C148618` and friends are written by `0x0C078AC0` anyway, so the analysis will
see them as store targets and will not fold them even where the shape would allow it. **A runtime
store to `0x0C14862C` is therefore observed by the guest.** No translator change is needed to make
the patch land; the translator change in 4.2 is only needed to control *when* it lands.

### 4.4 Where the patch has to land, and why "every VBlank" is not good enough here

Flycast re-applies its cheat every VBlank (`CheatManager::apply()`, `core/cheats.cpp:367-378`) and
gets away with it because it patches one value the game rarely rewrites. Ours is different: we are
patching **an input** (`[0x0C14862C]`) and **a derived value** (`[0x0C148618]`) that
`fn_0x0C078AC0` and `fn_0x0C078150` recompute together. Land the two stores between those two
recomputations and the frame projects with a mismatched pair — a one-frame horizontal jolt.

The correct place is an **exit hook on the function that recomputes the block**, so the pair is
always consistent. That is precisely what 4.2 buys.

---

## 5. Prior art

### 5.1 Flycast's "Widescreen" option is opportunistic un-cropping, not a stretch and not Hor+

`core/rend/transform_matrix.cpp`, `TransformMatrix::CalcMatrices()`:

```cpp
    if (config::Widescreen && !config::Rotate90 && !config::EmulateFramebuffer)
    {
        widescreenShift = 1.f - dcViewport.x / dcViewport.y * renderViewport.y / renderViewport.x;
        ...
        dcViewport.x *= 4.f / 3.f;
    }
    glm::mat4 trans = glm::translate(glm::vec3(-1 + widescreenShift, -flipY, 0));
    float x_coef = 2.0f / dcViewport.x;
```

For 640×480 into 16:9: `widescreenShift = 1 − 0.75 = 0.25`, `dcViewport.x = 853.33`. Guest `x = 0`
maps to NDC −0.75 and `x = 640` to +0.75 — the nominal 4:3 image occupies the middle 75%, and the
outer bands are filled by whatever the game happened to transform outside 0..640 and would normally
have been scissored away (`getBaseScissor()` widens the scissor to the whole framebuffer in this
mode). Aspect is preserved exactly. It is Hor+ *in effect* but not *in the game's mind*: the game
still culls, LODs and lays out its HUD for 4:3, hence the well-known pop-in and clipped backdrops.

**This is structurally the same thing as our option (c)+(widened geometry), and it is why our
`FrameGeometry` widening alone gives blank margins rather than free scenery:** Flycast sees the
un-scissored overdraw because the guest submitted it; whether Crazy Taxi submits any is a
measurement (8.3), and community reports say it submits very little, which is why the title is on
the "use the cheat, not the hack" list.

Two adjacent Flycast knobs, for the record: `ExtraDepthScale` scales only `z` (a depth-precision
workaround, nothing to do with FOV), and `ScreenStretching` (`core/cfg/option.h:454`,
*"in percent. 150 means stretch from 4/3 to 6/3"*) is a pure geometric stretch.

### 5.2 Flycast's per-game widescreen cheat patches guest RAM — and Crazy Taxi has an entry

`core/cheats.h:26-33` declares `WidescreenCheat { const char *game_id; const char *area_or_version;
u32 addresses[16]; u32 values[16]; ... }`; `core/cheats.cpp` matches on the IP.BIN product ID and
area symbols, and re-applies every VBlank with `WriteMem32_nommu(0x8C000000 + addr, value)`.

`core/cheats.cpp:75-76`:

```cpp
{ "MK-51035",   " U      ", { 0x2B08B0 }, { 0x43700000 } },		// Crazy Taxi (USA)
{ "MK-51035",   "  E     ", { 0x2B3410 }, { 0x43700000 } },		// Crazy Taxi (PAL)
```

`0x43700000` is `240.0f`. The owner's dump is product `MK-51035`, area `" U      "` — an exact match
for the USA row. The same code works on real hardware as a CodeBreaker line `022B08B0 43700000`;
Flycast's table is an import of hardware codes (one entry even carries the note
*"Only works on real Dreamcast"*).

The universal rule across the table is **multiply the horizontal projection term by 0.75**, because
`0.75 = (4/3) / (16/9)`:

| original | value | patched | value |
|---|---|---|---|
| `0x43A00000` | 320.0f | `0x43700000` | 240.0f  ← Crazy Taxi |
| `0x44200000` | 640.0f | `0x43F00000` | 480.0f |
| `0x3F800000` | 1.0f | `0x3F400000` | 0.75f |
| `0x3FAAAAAB` | 4/3 | `0x3FE38E39` | 16/9 |

**And `0x8C2B08B0` is 0x80 bytes below `0x8C2B0930`, the horizontal pixel scale this study found
independently by disassembly, whose 4:3 value is `640 × 0.5 = 320.0f`.** The two lines of evidence
converge on the same quantity in the same object. That is the strongest single result in this
document: the community's twenty-year-old hardware cheat and our static analysis are pointing at the
same float.

`0x0C2B08B0` is read at six sites (`0xC078B22`, `0xC078C44`, `0xC078CB4`, `0xC078D12`, `0xC078DC4`,
`0xC078E70`) and is passed as `r4` to `fn_0x0C0789B0` immediately after the viewport block is
recomputed — consistent with it being the base of the camera/viewport object, with the cached pixel
scales at `+0x80`/`+0x84`.

### 5.3 The community consensus matches the brief's distinction

The "true anamorphic widescreen" projects for Dreamcast instruct: **widescreen cheats ON, widescreen
hack OFF**, on exactly the grounds this study reaches independently — the hack reveals un-culled
geometry and produces pop-in, the cheat changes the game's own projection and does not. Crazy Taxi
appears on their working list. What none of them solve is the HUD, because a RAM poke cannot
translate a 2D layer; that is the part a static recompiler can do and an emulator cannot.

### 5.4 KallistiOS's own perspective helper, for calibration

`kallistios/kernel/arch/dreamcast/math/matrix3d.c:89-125`:

```c
/* Screen view matrix (used to transform to screen space) */
static matrix_t sv_mat = {
    { YCENTER,    0.0f,   0.0f,  0.0f },
    {    0.0f, YCENTER,   0.0f,  0.0f },
    {    0.0f,    0.0f,   1.0f,  0.0f },
    { XCENTER, YCENTER,   0.0f,  1.0f }
};
...
void mat_perspective(float xcenter, float ycenter, float cot_fovy_2,
                     float znear, float zfar) {
    sv_mat[0][0] = sv_mat[1][1] = sv_mat[3][1] = ycenter;
    sv_mat[3][0] = xcenter;
    mat_apply(&sv_mat);
    fr_mat[0][0] = fr_mat[1][1] = cot_fovy_2;
```

Called as `mat_perspective(320, 240, cot_fovy_2, ...)` this gives
`x_screen = 320 + 240·cot(fovy/2)·(x/−z)`. Note that KOS scales **both** axes by the half-*height*
and uses `xcenter` only as a translation — so KOS cannot express Hor+ through this helper at all;
you would have to `mat_scale(0.75f, 1, 1)` afterwards.

**Crazy Taxi is better structured than KOS for our purpose.** It keeps `[0x0C148618]` and
`[0x0C14861C]` as genuinely independent horizontal and vertical scales (2.3), which is why the
one-sided change in option (a) is available at all.

---

## 6. What breaks

This is the heart of "no artefacting". Ordered by how likely each is to be the thing that sinks the
change.

### 6.1 The 2D/HUD layer — the single biggest risk

**The mechanism is proven, not suspected** (2.5): `fn_0x0C07D018` multiplies every quad's X by
`[0x0C14862C] / app_width`. Raise `[0x0C14862C]` to 853.33 and every 2D element is 1.333× wider —
precisely the distortion the brief forbids. It also means the HUD cannot simply be "left alone":
option (a) actively breaks it unless something is done.

Three ways out, in increasing order of cost and correctness:

1. **Give the 2D path its own width word.** The sprite path reads `[0x0C14862C]` through exactly one
   literal-pool entry, at `0x0C07D1A0`. Point that entry at a spare word that stays 640.0 and the 2D
   layer is untouched by the widening. Then the HUD renders into columns 0..640 of an 853-wide
   frame — correct proportions, **pinned to the left**. Still needs a centring translate.
2. **Translate the 2D layer at the renderer.** Add `(W − 640)/2 = 106.67` to the X of every vertex
   the 2D path produced. This needs the renderer to know which geometry is 2D.
3. **Anchor per element** (fare top-left to the real left edge, timer top-centre, arrow top-right to
   the real right edge). This is what a hand-tuned widescreen patch does and it looks best, but it is
   per-element reverse engineering and is out of scope for a first cut.

**The hard sub-problem in (2) is attribution, and there is a complication:** the symbol table
contains `_kmiDMAtoTARequest` (`games/crazytaxi/symbols.tsv:97`, `0x0C153280`), so Crazy Taxi feeds
the TA by **bulk DMA from a prepared vertex buffer**, not only by inline stores. By the time
parameters reach `pvr::Core::fifo_word`, the function that produced them has long returned, so
"which guest PC wrote this parameter" is not directly available.

The recompiler's advantage is still real, it is just one level of indirection away: an entry/exit
hook on `fn_0x0C07D018` can record *which byte ranges of the vertex buffer* the 2D path wrote, and
the DMA can then be split into 2D and 3D spans. That is a genuinely available design and it is
unavailable to an emulator — but it is also the reason this step carries the widest estimate in
section 7.

A cheaper classifier may exist and should be measured first: 2D quads are submitted as PVR
**sprites** with a constant `1/w`, and `docs/runtime-render.md:93-96` records that a captured Crazy
Taxi frame was "473 of 524 polygons are sprites" with "depth 0.01 .. 10.0". If the in-game HUD sits
at a single distinguishable depth in the punch-through or translucent list, a depth/list classifier
is a few lines rather than a few days. **Measure before building** (8.4).

### 6.2 Frustum culling and near-plane clipping — probably fine, must be proved

The clip chain at `0x0C080FE8`–`0x0C081030` reads `[0x0C148634]`/`[0x0C148638]` and compares
transformed components — i.e. it is in the same code, reading the same viewport block, as the
transform. If the clip planes are derived from `[0x0C148618]`, widening the viewport widens them
automatically and there is no pop-in. That is the difference between this approach and Flycast's
renderer-side hack, and it is the main reason to prefer it.

But there is almost certainly a **second, coarser cull** at the object level (bounding sphere against
the frustum) somewhere in `0x0C07A000`–`0x0C07C000` that may hold its own cached plane normals.
`[0x0C148620] = max(sx, sy)` smells exactly like a cached guard-band radius. Under option (a) `sx` is
unchanged so `[0x0C148620]` is unchanged — which is *correct* if it is a pixel guard band and
*wrong* if it is an angular one. This is the most likely source of "buildings pop in at the new
edges" and it is the thing to look for first in testing (8.3).

### 6.3 The background plane — a hard ±256 px limit and a hard 480 bottom

`render/src/background.cpp:136-147`, for the untextured case:

```cpp
    if (!textured) {
        v[0].x = -256.0f;
        v[1].x = 896.0f;
        v[2].y = 480.0f;
```

and the textured case applies the same `±256.0f` at `:154`, `:156`, `:158`. Coverage is
x ∈ [−256, 896], y ∈ [0, 480]. The plane is in no display list at all
(`docs/runtime-render.md:265-268`) — the renderer synthesises it — so the guest patch does nothing
for it.

16:9 at 480 lines needs x ∈ [−106.67, 746.67], which **fits** inside ±256. 21:9 (1120 px) needs
x ∈ [−240, 880], which fits only just. Anything wider does not. The `480.0f` is an absolute barrier
to any vertical change, which is fine because Hor+ makes none.

`render/tests/test_background.cpp:74-76` pins this:

```cpp
    CHECK(f.vertices[0].x <= 0.0f);
    CHECK(f.vertices[1].x >= 640.0f);
    CHECK(f.vertices[2].y == 480.0f);
```

The first two tolerate widening; the third is an exact equality and would break if the plane were
ever stretched vertically. It should not need to change for Hor+.

### 6.4 Sprites and punch-through quads

A sprite is decoded as three corners plus the x and y of the fourth, with the fourth's depth and UVs
solved on the plane (`render/src/display_list.cpp:358-397`). Nothing there is resolution-dependent,
so widened 3D sprites are correct for free. 2D sprites are 6.1's problem, not a separate one.

One real hazard exists and is already documented: `polygon_tiles()`
(`render/include/dream/render/display_list.h:78-90`) notes that above the guest's own resolution the
outermost half-texel leaves the texture and repeat addressing produces a seam on every quad boundary.
Widescreen is not a resolution increase, so the guest-pixel grid is unchanged and this does not
newly apply — but it *does* apply the moment someone combines `--scale 2` with widescreen, which is
the obvious thing a user will do. The existing clamp logic should cover it; worth one test.

### 6.5 Anything that assumes x ∈ 0..640

Measured: 46 occurrences of `320.0f` and 26 of `240.0f` in the image, almost all in
`0x0C02B000`–`0x0C05E000` (UI code), e.g. `0x0C02BC62`–`0x0C02BC78` computing
`x = 320 − width × 0.5` before a draw call — a horizontal centring idiom. These are the callers of
`fn_0x0C07D018` and they are covered by 6.1: they author against a 640-wide screen and that stays
true under solution 6.1(1).

The sprite path also relies on the separate "app screen size" registration
(`0x0C2B4B74`–`0x0C2B4B80`, set to 640×480 by `fn_0x0C07C69C`). Leave that alone.

### 6.6 `--framebuffer-writeback`

Off by default and for good reason (`docs/runtime-render.md:317-322`). If someone turns it on,
`describe_write_framebuffer` takes the size from `FB_X_CLIP`/`FB_Y_CLIP`
(`render/src/framebuffer.cpp:180-185`), which the guest sets to 639/479 — so the wide frame is
**squashed back to 640 columns** by `encode_framebuffer`'s independent-axis nearest-neighbour
resample (`render/src/framebuffer.cpp:239-243`). The result is a 4:3-shaped buffer containing a
horizontally-crushed wide image. It is not a crash, but it is wrong, and the two features are
mutually exclusive until someone decides whether to crop or letterbox. **Recommendation: make
`--widescreen` and `--framebuffer-writeback` refuse to run together, with a message, rather than
silently producing a squashed frame.**

Note also `describe_framebuffer`'s sanity clamp at `render/src/framebuffer.cpp:120`
(`width <= 1024 && height <= 1024`). It is not in the way at 853 but it is in the way at 4K internal
resolutions combined with writeback.

### 6.7 Screenshots and `--capture`

Both already follow the frame rather than a constant. `screenshot_to()`
(`runtime/boot/boot_main.cpp:528-543`) writes its PPM header from `shown.width`/`shown.height`,
which `present()` sets to `offscreen.width()`/`offscreen.height()` whenever the renderer drew the
displayed buffer (`runtime/boot/boot_main.cpp:246-251`). `capture()` (`:550-586`) writes the PPM plus
the raw `.ta`, `.vram` and register sidecar, none of which are size-pinned. **Nothing to change**, and
this is what makes the verification in section 8 cheap: `--screenshot-at N` already produces a
correctly-sized wide PPM.

The one cosmetic wart: the FPS counter's scale is `2u * (w / kGuestWidth)`
(`runtime/boot/boot_main.cpp:514`), which divides by 640 and would grow the counter on a wider frame.
The input menu divides by 320 (`render/src/input_menu.cpp:116`) and is fine.

### 6.8 Golden traces and tests

**There is no golden-image or frame-hash harness in the repo.** The ~100 `tests/sh4/golden/*.golden`
files are SH-4 register and memory traces, not frames, and are untouched by anything here as long as
widescreen is off by default in the translated build. `--capture` writes artefacts for offline
inspection (`runtime/boot/boot_main.cpp:545-549`); there is no replay comparison.

Tests that would need attention:

- `render/tests/test_background.cpp:74-76` — see 6.3. Should still pass.
- `render/tests/test_framebuffer.cpp:260-262` — pins that `FB_X_CLIP` wins over the caller's size.
  Correct and should stay; it is the thing that makes 6.6 true.
- No test covers `encode_framebuffer`'s scaling branch (`framebuffer.cpp:241,243`) — src size always
  equals dst size in the suite. If 6.6 is resolved rather than refused, that branch needs one.

**The real risk to the differential story is different and worth stating plainly:** every hard rule
in this project says the translation is validated bit-exactly against the interpreter. A widescreen
patch deliberately makes the guest compute different numbers. It therefore **must** be off by default
and must never be enabled in any run that feeds the differential harness or a golden trace, or the
oracle stops meaning anything. That is a one-line guard and a paragraph in `docs/differential-harness.md`,
not an engineering problem — but forgetting it would be expensive.

---

## 7. Implementation plan and estimate

Effort in **focused engineer-days**, the convention from `docs/implementation-plan.md:8-10`. This is
Phase 4 work; the plan already names it (`docs/implementation-plan.md:122`, *"Widescreen via
projection-matrix hooks"*) without an estimate, so these numbers are new and should be folded back
into that line.

| # | Step | Files touched | Low | **Central** | High | What drives the spread |
|---|---|---|---|---|---|---|
| 1 | **Confirm the model on a live run.** Log `[0x0C148618]`, `[0x0C14861C]`, `[0x0C14862C]`, `[0x0C148630]`, `[0x0C2B0930]`, `[0x0C2B0934]`, `[0x0C2B08B0]` every frame from an in-city run; check `[0x0C2B0930] == 320.0` and `== [0x0C2B08B0]`. | `runtime/boot/boot_main.cpp` (a temporary `--probe-addr` or a throwaway patch) | 0.25 | **0.5** | 1 | Getting an in-city frame at all: `docs/runtime-render.md:243-246` records that the city needs a crash fixed first. If that is still true this step is blocked, not slow. |
| 2 | **Implement `[hooks]`**: wire `GameConfig::hooks` into the emitter as entry/exit calls, mirroring `--replay-hooks`. Plus a `dream::hooks` registry in the runtime. | `translator/src/emit/emit.cpp`, `translator/src/main.cpp`, `translator/include/dream/translator/emit.h`, `runtime/include/dream/runtime/sh4/abi.h`, new `runtime/src/hooks/`, `docs/game-config.md`, `docs/emitter-design.md` | 1 | **2** | 3.5 | This is the reusable piece. The mechanism is half-built (4.2); the cost is the registry, the naming, the tests, and the "hook on a function that is also an overlay" case. |
| 3 | **The 3D widening itself**: an exit hook on `fn_0x0C078AC0` and `fn_0x0C078150` that sets `[0x0C14862C] = W` and `[0x0C148618] ×= 640/W`; a `--widescreen[=RATIO]` flag; widen the offscreen and `FrameGeometry` to match. | `games/crazytaxi/crazytaxi.toml` (`[hooks]`), new hook source, `runtime/boot/boot_main.cpp:128-137,603,610` | 0.5 | **1** | 2 | Small if step 1 confirmed the model. High end is "the block is recomputed from a third site I did not find". |
| 4 | **Prove it is Hor+ and not a stretch** (section 8.1–8.2). | tests / scripts only | 0.5 | **1** | 1.5 | Mostly measurement time. |
| 5 | **Culling and clipping**: find the object-level cull, decide whether `[0x0C148620]` needs widening, fix pop-in at the new edges. | disassembly + the same hook file | 1 | **2** | 5 | The whole spread is "does the coarse cull follow the viewport block or hold its own cached planes". Could be zero work; could be a second reverse-engineering job. |
| 6 | **2D/HUD separation and centring** — the 640-wide HUD box centred in the wide frame without distortion. Redirect the sprite path's width literal (6.1 solution 1), then classify and translate the 2D geometry (6.1 solution 2). | `games/crazytaxi/crazytaxi.toml`, hook source, `render/src/display_list.cpp` or `render/src/vk/renderer.cpp`, `render/include/dream/render/display_list.h` | 2 | **3** | 6 | **The dominant uncertainty in the whole job.** If a depth/list/sprite classifier works on a captured in-game frame, 2 days. If attribution has to go through `_kmiDMAtoTARequest` buffer-span tracking, 6. Per-element anchoring is out of scope and would be more. |
| 7 | **Background plane, writeback guard, counter scale, and the differential-harness guard.** | `render/src/background.cpp`, `runtime/boot/boot_main.cpp:202,514`, `docs/differential-harness.md` | 0.5 | **0.75** | 1.5 | Small and known. The ±256 slack (6.3) may need widening to a computed value. |
| 8 | **Tests, docs, clang-format, CI on three platforms.** New tests for the hook mechanism, the background plane at a wide geometry, and `encode_framebuffer`'s scaling branch. Update `docs/runtime-render.md` and `docs/implementation-plan.md:122`. | `render/tests/`, `translator/tests/`, `docs/` | 0.75 | **1** | 1.5 | |
| | **Total** | | **6.5** | **11.25** | **22** | |

Rounded for quoting: **11 days central, range 7–21.**

Two useful sub-slices:

- **Minimum honest slice, 3.5 days** (steps 1, 2, 3, 7-partial, plus the fallback 3′): ship option
  (a′) — Flycast's proven `×0.75` — behind `--widescreen=anamorphic`, with the world correct and the
  HUD stretched, documented as a known limitation. Everything built here is reused by the full
  version, and step 2 is the expensive half of it.
- **The part that is genuinely new work versus Flycast, 5 days** (steps 5 and 6): no-pop-in culling
  and an undistorted HUD. This is the part that justifies doing it in a recompiler at all, and it is
  where the risk lives.

**Sequencing note:** step 2 blocks 3, 5 and 6, and step 1 blocks everything. If step 1 is blocked on
the in-city crash, do step 2 anyway — it is independently useful and is the thing the plan will want
for texture replacement and the frame-rate work too.

---

## 8. Verification: proving it is widescreen and proving it is clean

The brief asks for proof of two different things. They need different measurements.

### 8.1 Proving the field of view actually widened (not a stretch)

**The decisive measurement is on the TA parameter stream, before any renderer is involved.** Capture
the same scene twice from a deterministic run — `--rtc-seed` plus a scripted `--press` sequence, as
in the appendix of `docs/vmu-creation-study.md` — once in 4:3 and once in 16:9, with
`--capture-at N` at the same frame number. Then, from the two `.ta` files decoded through
`dream::render::DisplayList`:

| measurement | 4:3 | 16:9 Hor+ | 16:9 stretch (the failure) |
|---|---|---|---|
| vertex count in the frame | *n* | **> *n*** | *n* (identical) |
| min/max vertex **y** | 0..480 | **0..480, unchanged** | 0..480 |
| min/max vertex **x** | ~0..640 | ~0..853 | ~0..853 |
| a **specific** vertex's y, matched by its texture and strip index | y | **identical bit-for-bit** | identical |
| the same vertex's x, measured from the centre: `(x − W/2)` | d | **identical** | 1.333 × d |

The last row is the whole test and it is a one-line assertion. **Hor+ keeps `x − W/2` invariant and
adds vertices; a stretch scales `x − W/2` by 1.333 and adds none.** The vertex count going up is the
independent confirmation that new world entered the frustum rather than old world being spread out.

A second, cheaper check for a human: `dream_render_view capture.ta --fit` prints the frame's screen
extent (`docs/runtime-render.md:90-96` shows the format). In Hor+ the extent grows *and* the polygon
count grows; in a stretch only the extent grows.

### 8.2 Proving the vertical field of view is unchanged

Assert `[0x0C14861C]`, `[0x0C148630]` and `[0x0C2B0934]` are bit-identical between the two runs — a
direct memory read, no interpretation needed. Then confirm it reached the pixels: crop both PPMs to
the central 640 columns and compare. **For true Hor+ with the horizontal pixel scale held constant,
the central 640 columns of the wide frame should be very nearly pixel-identical to the 4:3 frame**
— not exactly, because the extra geometry can occlude and because translucency sorting is per-strip
(`docs/runtime-render.md:198-205`), but a difference image should be empty except at the edges and
behind newly-visible transparent surfaces. A difference image that is bright *everywhere* means a
stretch or a vertical change, and it localises the fault immediately.

### 8.3 Proving no pop-in at the new edges (the culling test)

This is the artefact most likely to appear and the one hardest to catch in a screenshot, because it
is temporal.

- **Static:** for each of five fixed camera positions in the city, compare the wide capture against a
  wide capture taken with the coarse object cull disabled (patch its predicate to always-pass).
  Identical polygon counts means nothing is being culled that should be drawn. This is the
  measurement that decides step 5's estimate.
- **Temporal:** run a fixed 600-frame scripted drive with `--screenshot-at` on every 10th frame,
  then for each pair of consecutive shots compute the number of pixels in the two 107-px edge bands
  that changed from background colour to geometry. A smooth pan gives a low, steady number; pop-in
  gives spikes. Plotting that series is more informative than watching for it, and it can go in CI as
  a threshold.
- **Near-plane:** drive up against a wall and past a lamp post at the frame edge. If the near-plane
  clipper is still working in the old 4:3 frustum, geometry will visibly shear off along a vertical
  line at x = 107 and x = 746. That line is the signature and it is unmistakable once looked for.

### 8.4 Proving the HUD is neither stretched nor drifting

- **Measure the HUD, do not eyeball it.** Capture the same HUD state (same fare, same timer) in 4:3
  and 16:9. Crop the HUD bounding box from each PPM. The two crops must be **byte-identical**. A
  1.333× stretch changes every pixel; a 1-pixel drift changes the edges. Byte equality catches both
  and needs no tolerance argument.
- **Position:** assert the HUD box's centre is at `W/2` in the wide frame and at 320 in the 4:3 one.
- Do this at four points — title screen, menu, in-game HUD, results screen — because they may go
  through different draw paths, and the title screen is the one the repo can already capture today.
- **Before building the classifier, measure whether one is needed:** decode a captured in-game frame
  and print, per polygon, its list, its `1/w` range and whether it is a sprite. If the HUD separates
  cleanly on those, step 6 is a filter rather than a tracing job. This is half a day and it decides
  the widest estimate in the table.

### 8.5 Proving nothing else regressed

- `ctest --test-dir build` on all three platforms, with widescreen off, must be unchanged — this is
  the guard from 6.8 doing its job.
- One differential-harness run with widescreen off, to confirm the hook mechanism added no cost or
  divergence when unused.
- A 9000-frame headless run with widescreen on, asserting zero unmapped accesses — the same bar
  `games/crazytaxi/crazytaxi.toml:88-95` uses for the memory-card path.

---

## 9. What I could not determine, and exactly how to find out

1. **Where the screen centre (`+320`, or `+W/2`) is added.** I located the scales and the origins but
   not the instruction that biases X by the half-width. It may be folded into `XMTRX`, or it may come
   from `[0x0C14862C] × 0.5` inside the vertex pipeline's stack slots at `0x0C080E5C`–`0x0C080E64`.
   **This matters**: if the centre is baked into `XMTRX` from a *separate* copy of the width, option
   (a) moves the frustum without moving the centre and the whole image slides left.
   *How to find out:* single-step `fn_0x0C080E20` in the dev interpreter (ADR 2) with a known vertex
   and diff the register file against a hand-computed projection. One hour with the interpreter;
   much longer by reading. Alternatively, do step 1 of the plan and simply try it — a slid image is
   an obvious and instantly diagnosable failure.

2. **The live value of `[0x0C148618]`.** I inferred 0.5 from the arithmetic and from Flycast's
   320.0f. It is computed at run time from function arguments I did not trace.
   *How to find out:* step 1 of the plan.

3. **What `0x0C2B08B0 + 0x00` actually is** — the exact word Flycast patches. I did not find its
   writer. It is 0x80 below the horizontal pixel scale and is the argument to `fn_0x0C0789B0`.
   *How to find out:* set a watchpoint on it in the dev interpreter and report the writing PC. If it
   turns out to be a third cached copy of the horizontal scale, it needs patching too or the two
   copies will disagree.

4. **Whether the coarse object cull follows the viewport block.** Section 6.2. This is the single
   biggest unknown by estimate impact (step 5's spread is 1 to 5 days).
   *How to find out:* 8.3's static test, which needs no reverse engineering at all — just a way to
   disable the cull, which itself needs the cull found. Failing that: apply the widening, drive
   around, and look. Pop-in is not subtle.

5. **Whether `fn_0x0C07D018` is the *only* 2D path.** It has 100+ callers and covers the sprite quad
   case, but the SEGA logo and title screen also draw through paths I did not trace, and menus may
   use a different one.
   *How to find out:* hook it, count TA parameters produced inside it versus in total, on a frame
   whose HUD content is known. If the ratio accounts for the HUD, it is the only path.

6. **Whether the 2D layer is separable at the TA without PC attribution.** Section 8.4's last bullet.
   Half a day of measurement that could remove four days of work.

7. **The correct target ratio.** 16:9 at 480 lines is 853.33 px, which is not an integer. Rounding to
   854 introduces a 0.08% horizontal scale error — invisible, but it means the offscreen extent and
   `FrameGeometry::width` must be kept consistent to avoid a slow drift between the guest's idea of
   the viewport and ours. *How to find out:* decide deliberately — either make the guest's width the
   integer 854 and accept a 16:8.99 frustum, or keep 853.333 in the guest and let only the pixel
   extent round. The second is correct; the first is what someone will do by accident.

8. **Whether any of this generalises.** Every address here is Crazy Taxi's. The *shape* — a viewport
   block with independent H and V scales, computed once and cached as pixel scales — is likely to
   recur in other Sega arcade ports, and Flycast's cheat table shows 40-odd titles with a single
   patchable float. Generality is a bonus, not the goal, but the `[hooks]` mechanism from step 2 is
   title-agnostic and is the part worth designing for reuse.

---

## Appendix: reproducing the static analysis

Everything in section 2 comes from the owner's gitignored dump plus the checked-in translator. No
game bytes are reproduced here.

```sh
# Literal-pool constant scan: find the (640, 480) viewport pair and its readers.
python3 - <<'EOF'
import struct
from collections import defaultdict
d = open('games/crazytaxi/extracted/fs/1ST_READ.BIN','rb').read()
base = 0x0C010000
# SH-4 loads FP constants as mova/mov.l of a pool address, then fmov through the register.
refs = defaultdict(list)
for i in range(0, len(d)-1, 2):
    w = struct.unpack_from('<H', d, i)[0]; pc = base + i
    if (w >> 12) == 0xD:  refs[(pc & ~3) + 4 + (w & 0xFF)*4].append(pc)   # mov.l @(disp,PC)
    elif (w >> 8) == 0xC7: refs[(pc & ~3) + 4 + (w & 0xFF)*4].append(pc)  # mova @(disp,PC),r0
# Words that name the viewport block, and the code that loads them.
for i in range(0, len(d)-3, 4):
    v = struct.unpack_from('<I', d, i)[0]
    if 0x0C148618 <= v <= 0x0C148638 or 0x0C2B0930 <= v <= 0x0C2B0934 or v == 0x0C2B08B0:
        for c in refs.get(base + i, []):
            print("%08x <- %08x" % (v, c))
EOF

# The two sites that compute the block:
./build/translator/dream-translate disasm --image games/crazytaxi/extracted/fs/1ST_READ.BIN \
    --base 0x0C010000 --start 0x0C078AC0 --count 60
./build/translator/dream-translate disasm --image games/crazytaxi/extracted/fs/1ST_READ.BIN \
    --base 0x0C010000 --start 0x0C078150 --count 70

# The main vertex pipeline and its clip chain:
./build/translator/dream-translate disasm --image games/crazytaxi/extracted/fs/1ST_READ.BIN \
    --base 0x0C010000 --start 0x0C080E20 --count 120

# The 2D/sprite choke point and its 640/480 normalisation:
./build/translator/dream-translate disasm --image games/crazytaxi/extracted/fs/1ST_READ.BIN \
    --base 0x0C010000 --start 0x0C07D16A --count 60
```
