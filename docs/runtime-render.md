# The renderer (WP2.3)

The Dreamcast draws through the PowerVR2's Tile Accelerator: the game writes a stream of
parameters (polygon headers, vertices, sprites, modifier volumes) into a FIFO, groups them into
five display lists, and then asks the hardware to render a frame. `runtime/pvr/` already models
that stream, the register block and the timing (`docs/runtime-pvr.md`); this document covers
turning the captured stream into a picture.

## Shape of the code

Two libraries, split so that most of the work is testable without a GPU:

- **`dream::render`** (`render/src/display_list.cpp`) is device-independent: display-list decoding
  and texture decoding. No Vulkan, no window. Built and unit-tested on every CI runner.
- **`dream::render_vk`** (`render/src/vk/`) is the Vulkan backend and the SDL3 window. Built only
  when `DREAM_RENDERER=ON` and the dependencies are found, so a machine without a graphics stack
  still builds everything else.

The launcher links the second only when it exists, behind `DREAM_WITH_RENDERER`. Without it the
launcher builds and runs headless exactly as before, which is what CI builds, and `--window` says
the build has no renderer rather than silently doing nothing.

`dream_render_probe` opens a window, draws a triangle and prints the device's capabilities. It is
not a test (it needs a display); run it by hand to check a new machine.

## Why Vulkan, and what that costs on each platform

ADR 9 chose Vulkan with the renderer ported from Flycast. One backend covers all three hosts, and
shaders compile once to SPIR-V at build time (`glslc`, wrapped into headers by
`render/cmake/wrap_spirv.cmake`) so a release build carries no shader source.

- **Linux and Windows** have native Vulkan drivers; nothing special.
- **macOS has no native Vulkan.** The loader talks to MoltenVK, which translates to Metal. Two
  consequences the code handles in `vk::Context::create`: MoltenVK is a *portability* driver, so a
  loader from 1.3.216 on will not even enumerate it unless the instance passes
  `VK_KHR_portability_enumeration`, and the device must enable `VK_KHR_portability_subset`.
  Geometry shaders are unavailable on MoltenVK; the renderer does not use them (checked against
  Flycast's Vulkan backend, which only mentions the stage in a shader-compiler enum).
- **MoltenVK is Apache-2.0 and this project is GPL-2.0**, which is awkward for static linking. The
  build links dynamically against the Vulkan loader, and a macOS bundle ships MoltenVK beside it,
  which is what Flycast does under the same licence. Recorded here so the packaging work (WP3.6)
  does not rediscover it.

## Capabilities are a property of the GPU, not the platform

The per-pixel order-independent transparency path needs `fragmentStoresAndAtomics`. A weak or old
device may lack it, and then the per-strip sorting fallback is used instead. `vk::Context` scores
devices so the one that can do per-pixel sorting wins, and reports what it found in
`DeviceCapabilities`. Measured on the development machine (Apple M4, MoltenVK on Vulkan 1.4.357):

    fragment stores and atomics: yes      per-pixel transparency available
    fragment shader interlock:   yes
    independent blend:           yes
    anisotropic filtering:       yes
    max texture size:            16384

## Display-list decoding (`render/display_list.{h,cpp}`)

The hardware receives 32-byte parameter chunks. Each is a polygon header (which opens a strip and
sets the render state), a sprite header, a modifier-volume header, a vertex, or an end-of-list
marker. Some headers and vertices are 64 bytes, and which ones depends on bits of the parameter
control word, so the parser is a small state machine. Vertex layout varies further with the colour
type, whether the polygon is textured, whether texture coordinates are 16-bit and whether two
volumes are in use.

What the decoder produces is a `Frame`: one vertex array, five lists of `Polygon` (a contiguous
triangle strip plus the raw ISP/TSP/TCW words), and modifier-volume triangles. The hardware words
are kept raw and decoded by the backend, so this layer never has to model blending or depth and
cannot get them subtly wrong.

Points worth knowing, each of which cost a test to establish:

- **A polygon header stays in force across strips.** One header can be followed by several strips,
  each ended by the end-of-strip flag on its last vertex. Treating the flag as closing the header
  silently drops most of a frame's geometry.
- **Colour arrives four ways.** Packed ARGB in the vertex; four floats in the vertex's second half;
  or an intensity that scales a face colour carried by the header (colour type 3 reuses the
  *previous* header's face colour). All become ARGB8888 here.
- **A sprite is a quad given as three corners plus the x and y of the fourth.** The fourth corner's
  depth and texture coordinates are solved on the plane the other three define, as in Flycast's
  `CaclulateSpritePlane`. One sprite becomes a four-vertex strip.
- **A modifier-volume vertex is a whole triangle**, nine floats spanning both halves of a 64-byte
  parameter, not a strip vertex.

Tested two ways (`render/tests/test_display_list.cpp`). Hand-built streams cover each parameter
form, so a failure points at the decoder rather than at a capture. And a captured Crazy Taxi frame
(`crazytaxi_boot --dump-ta FILE`, owner-supplied data so the test skips on CI) is decoded and
cross-checked against the *runtime's* independent parameter parser, which was written for timing
and counts parameters rather than geometry. The two agreeing on a real frame is worth more than
either alone: a misunderstanding of the 32/64-byte rules would show up as a mismatch. On the
captured frame:

    524 polygons (240 opaque, 284 translucent, all textured), 473 of them sprites
    2096 vertices, 0 modifier triangles, depth 0.01 .. 10.0
    screen extent x -640..1280, y 0..448

## Drawing a frame (`render/vk/renderer.{h,cpp}`)

Opaque geometry, untextured, with the hardware's depth and culling. Three things are worth knowing:

- **The coordinates are already in screen space.** There is no model or view transform: x and y are
  pixels and z is 1/w. The rasteriser is given w = 1 so it does not perspective-divide, and
  perspective correction is done by hand the way the hardware does it, multiplying attributes by
  1/w in the vertex shader and dividing in the fragment shader.
- **Depth is written from the fragment shader**, as `log2(1 + 100000/w) / 34`. The hardware's depth
  is 1/w over a very wide range and Vulkan's depth range is not, so the logarithm spreads it over
  the available precision. This costs early-Z, which is the trade Flycast makes too. The depth
  buffer is cleared to 0 and the comparisons run "greater is nearer".
- **Pipeline state comes from the polygon's own ISP word**: depth comparison (eight modes), depth
  write, and culling. Pipelines are cached on exactly those bits, so a frame of the captured Crazy
  Taxi screen needs one pipeline for its 240 opaque polygons.

`dream_render_view FILE` draws a captured display list in a window without running the game;
`--all` also draws the translucent list with the opaque pipeline, `--fit` scales to the frame's
bounding box, and `--screenshot OUT.ppm` reads the framebuffer back. Those three together are how a
frame gets diagnosed: the first captured Crazy Taxi screen looked blank at 640x480 until `--fit`
showed that the frame spans x -640..1280, three screens wide, with only 26 of 524 polygons on
screen. The screen itself holds one large UI panel, which draws as a white rectangle until textures
exist.

## Textures (`render/texture.{h,cpp}`, `render/vk/texture_cache.{h,cpp}`)

A texture is described by two hardware words, and four things vary independently: the pixel format
(three 16-bit encodings, YUV 4:2:2, a bump map, or a 4- or 8-bit palette index), the layout
(twiddled or scan order), vector quantisation, and mipmapping. The decoding is
device-independent and unit-tested with textures built by hand in each layout, so a failure names
the format.

Worth knowing:

- **Twiddling interleaves the bits of y and x, y first**, taking bits only while each dimension
  still has some. On a square texture that is the familiar Morton order; on a rectangular one the
  longer dimension's remaining bits follow. The tests check that every texel maps to a distinct
  index covering the whole range, for both shapes.
- **A compressed texture's codebook sits at the texture's address and the block indices follow it**,
  2 KB later. Reading the indices from the same place as the codebook decodes an entire texture to
  the same colour, which is exactly what the first attempt did.
- **Within a codebook entry the four texels are themselves twiddled**: (0,0), (0,1), (1,0), (1,1).
- **Only plain 16-bit textures may be in scan order.** An indexed or compressed texture is twiddled
  whatever the bit says.

The cache keys on the two words, decodes and uploads once, and hands back a descriptor set.
Invalidation is deliberately coarse: everything is dropped when palette memory changes. Tracking
which texels a write touched is a later optimisation, and getting it wrong shows as textures that
never update, which is a far worse failure than decoding one again. A polygon whose texture cannot
be decoded draws untextured rather than not at all, because a flat shape in the right place says
more during bring-up than a hole does.

How a texture and the vertex colour combine is the TSP word's shading instruction: decal replaces
the colour, modulate multiplies it, and the two "alpha" variants also use the vertex colour's
alpha. The fragment shader takes it as a push constant rather than specialising the pipeline,
which costs a branch and saves a pipeline per mode.

**This is where Crazy Taxi became recognisable.** `dream_render_view ta.000 --vram vram.bin --all`
draws the title screen: the logo with its flames and the game's "No VMU found" notice, from 34
decoded textures with none failing. With the TSP word's fields at the right bit positions (below)
the live window draws it exactly: the flames keep their shape, and the copyright line is legible.

## Transparency (WP2.3 step 5)

The hardware draws the lists in a fixed order and titles rely on it: opaque geometry, then
punch-through, then translucent blended over the result.

- **Punch-through is opaque geometry with holes in it.** The hardware keeps or discards a pixel
  outright against the PT_ALPHA_REF register rather than blending, so the list writes depth and
  needs no sorting. The fragment shader discards below the threshold.
- **Blending comes from the TSP word**: three bits of source factor and three of destination, eight
  each. Index 2 and 3 mean "the other colour", which is the destination colour for a source factor
  and the source colour for a destination factor; the rest are the same on both sides. These are
  part of the pipeline key, so a frame needs one pipeline per combination it actually uses (the
  Crazy Taxi title screen: two).
- **A translucent surface tests depth but does not write it**, or it would hide surfaces behind it
  that still have to be blended in.
- **"Use alpha" clear means the surface is opaque** however its colours are encoded: the vertex
  alpha is data for something else and must not reach the blender. Honouring it instead blends a
  logo into the background — a picture correct in every respect except that it is too dark, easy to
  mistake for a gamma or colour-space problem.

**Polygon header sizes are the other place a wrong constant draws rather than fails.** A header is
64 bytes only for intensity mode 1, and then only when it has both a texture and an offset colour
to carry; mode 2 reuses the previous polygon's face colour and is always 32 bytes however it is
textured. Reading a mode 2 header as 64 bytes swallows the first vertex of its strip and every
triangle after it is built from the wrong corners: on Crazy Taxi's SEGA logo, which mixes both
modes in one screen, twenty strips out of a hundred and five came out as yellow wedges across the
letters. The 32-byte case also carries its face colour, in words 4 to 7, the words a header without
one leaves unused; substituting white there washed the polygon out. Sizes follow
`TaTypeLut::poly_header_type_size` in Flycast's `core/hw/pvr/ta.cpp`.

**The TSP word's field positions are pinned by a test** (`render/tests/test_tsp.cpp`) against
Flycast's `union TSP`, and `render/tsp.h` is the one place they are written down. Four of them were
a bit or two out for a while: "ignore the texture's alpha" and "use alpha" were each read one bit
high, and the u and v mirror flags four bits low. The visible result was solid red rectangles over
Crazy Taxi's logo, because a tile's transparent parts were drawn opaque, and the filter mode was
read from the wrong two bits so every texture sampled the same way. **A wrong bit position in this
word does not fail a build, it draws**, and it draws something plausible enough to be blamed on
something else; the test sets each field alone with every other bit set, so a reader that is off by
one picks up a neighbour and the failure names the field.

Translucent strips are sorted back to front by their nearest vertex. **This is the fallback, not
what the hardware does.** A real PowerVR sorts per pixel, so two translucent surfaces that
intersect come out right and a per-strip sort cannot. Per-pixel sorting needs fragment stores and
atomics, which the development machine has (`dream_render_probe` reports them), and is the quality
refinement to make once a title shows the difference; a per-strip sort is what most Dreamcast
rendering used for years and is right for everything but intersecting transparency.

**Modifier volumes are decoded but not drawn.** They need a stencil pass, and no captured Crazy
Taxi frame so far contains one, so the code would be unverifiable. It is listed in "Not done"
rather than written blind.

## Fog (`render/fog.{h,cpp}`)

The hardware fogs a pixel by its depth, and it does it with a 128-entry table rather than a formula,
so a title can shape the curve however it likes. Two modes are in real use and they are not
variations of one idea. **Table fog** looks the depth up in that table and blends towards
`FOG_COL_RAM`. **Per-vertex fog** ignores the table entirely and uses the alpha of the polygon's
offset colour, interpolated across the triangle, against `FOG_COL_VERT`. Which one a polygon uses is
two bits of its TSP word, and a frame mixes them freely.

Two details are worth stating because both fail quietly.

`FOG_DENSITY` is a mantissa over a **signed** exponent. Reading the exponent as unsigned turns a fog
that thins with distance into one that barely moves, and the result looks like haze either way, so
it survives a glance at a screenshot. A test pins it, including Crazy Taxi's own value.

The table index is **logarithmic** in depth: the hardware builds it from the exponent and mantissa
of a scaled depth, giving sixteen entries per doubling. A linear index would spend as many entries
on the far half of the world as on the near, and the near half is the half anyone looks at. That is
also pinned as arithmetic rather than judged as a picture.

The data layer is done and tested; the shader that consumes it is not written yet, because there is
no captured frame with fog actually enabled to check it against. Crazy Taxi's menus set the density
to 2^-113, which is off. That needs a frame from the city, which currently needs the crash below
fixed first.

## The background plane (`render/background.{h,cpp}`)

Every frame sits on one polygon that fills the screen behind it, and it is in no display list: the
title writes it into the parameter buffer itself and points `ISP_BACKGND_T` at it. A renderer that
draws only display lists therefore shows its own clear colour instead. That is what cost Crazy
Taxi's SEGA screen its white background, which also hid the black "presented by" above the logo,
and the title screen its yellow.

Two details make it awkward, and both fail quietly rather than loudly.

- **The parameter buffer is read through the 32-bit view of video memory**, which interleaves the
  two banks, so the address in the register is not an offset into the flat image. Reading it flat
  lands on bytes that happen to be zero, which looks exactly like "this title has no background
  plane". A test asserts that the flat read finds nothing, so the mapping cannot be dropped by
  accident.
- **The vertex layout is not fixed.** `ISP_BACKGND_T` carries a "skip" giving the extra words per
  vertex, and which of those are texture coordinates and which are colours comes from the ISP word,
  because the plane has no parameter control word at all. The renderer's usual flags are
  reconstructed from that word.

The depth comes from `ISP_BACKGND_D` rather than from the vertices, and an untextured plane ignores
the positions it was given and covers the screen with room to spare, which is what the hardware
does. The polygon is given depth mode "always" and no culling, since it is behind everything and
has no winding of its own. Reference: Flycast's `FillBGP` in `core/hw/pvr/ta_vtx.cpp`.

## The display framebuffer (`render/framebuffer.{h,cpp}`)

The framebuffer is the pixels the video hardware scans out, and everything ends up there whichever
path drew it. The Tile Accelerator writes a rendered frame into it, and a title can also write
pixels into it directly, which is how video playback, some 2D screens and several effects work.
Those pixels never pass through a display list at all, so a renderer that only draws display lists
shows an incomplete picture.

Four pixel formats, and one detail that catches people: **the width and the row stride are both
counted in 16-bit words whatever the format**, so a 32-bit framebuffer covers half as many pixels
in the same number of words and a 24-bit one two thirds as many. A second: when a 16-bit channel is
widened to eight bits, the low bits come from the register's "concat" field rather than from the
channel, so black is not quite black on hardware that sets it.

Conversion runs both ways. `decode_framebuffer` reads the guest's framebuffer into RGBA; and
`encode_framebuffer` writes a rendered frame back into it, scaling if the rendered image is a
different size, which is what an increased internal resolution will need. The second is what makes
the two drawing paths compose: a title that renders geometry and then writes pixels into the same
buffer expects to see both.

**What the render target register does not tell you.** A write address different from the
displayed one does *not* mean a render to texture: almost every title double-buffers, so the write
address is normally just the buffer that is not on screen. Crazy Taxi's title screen displays from
0x200000 and renders to 0x600000, and that is ordinary double buffering. Telling a genuine
render-to-texture apart needs to know whether the written region is later sampled as a texture,
which only the texture cache can answer. `describe_render_target` therefore reports the address and
leaves the judgement to its caller; claiming otherwise would have been a confident wrong answer.

**The write side has its own registers.** `FB_W_CTRL` names seven pixel formats where `FB_R_CTRL`
names four, and numbers them differently: ARGB4444 exists only on the write side, and the two
32-bit formats and the two 1555-shaped ones collapse together once the frame is on screen.
`FB_W_LINESTRIDE` gives the row length in eight-byte units. Nothing in those registers gives the
*size* of the frame, because the hardware learns that from the region array, so
`describe_write_framebuffer` takes the size from its caller, which passes the display
framebuffer's.

## A live run (WP2.3 step 6)

`crazytaxi_boot --window` opens a window and plays. The guest still nominates what is shown: at
vertical blank the address in `FB_R_SOF1` decides. What differs is where those pixels come from.

| The guest is displaying | What is shown |
| --- | --- |
| a buffer the renderer has drawn into | the rendered image, at whatever resolution `--scale` asked for |
| any other buffer | that buffer decoded out of video memory, which is how a title's own pixel writes appear |

"One the renderer has drawn into" rather than "the one it drew last", because a title alternates
between two buffers and does not swap the displayed one on every render. Crazy Taxi holds the
display on 0x200000 for stretches of four or five frames while rendering into 0x600000, so asking
whether the displayed buffer is *the* most recent target answers no almost every time. Asking
whether it is one of ours answers yes, and a buffer the renderer has never touched is exactly the
one whose pixels the title wrote itself.

**Writing rendered frames back into video memory is off by default, and that was a correction.**
The first version of this step wrote every frame back to `FB_W_SOF1` in the guest's own pixel
format, on the reasoning that going the long way round is what makes a title's own framebuffer
writes compose and what a render to texture needs. It is exact, and on Crazy Taxi it erases the
game's logo: the title has texture data inside the two regions the write covers, and the only
tiles that survived were the two whose addresses fall just past the end of each buffer. That is
what the two red rectangles on the "no VMU" screen were. `--framebuffer-writeback` turns it back on
for when a render to texture needs it. Flycast reaches the same default from the other direction:
its framebuffer emulation is off unless a title needs it.

**How that was mis-diagnosed, and the lesson.** Running the same frames with the texture cache held
across the write (`DREAM_NO_WRITEBACK_INVALIDATE=1`) gave a pixel-identical picture, and that was
read as "the fault predates this step". It does not: holding the cache stops the renderer noticing
the write, but the write still happens, so both runs decoded textures that had already been
overwritten. The control that settles it is a render with no write-back at all, which
`dream_render_view` does on the same capture, and there the logo is complete. **A control has to
remove the suspected cause, not just one of its consequences.**

**Rendering happens offscreen, at the guest's resolution times `--scale`.** Separating the render
size from the window size is what makes an increased internal resolution one argument rather than a
redesign: `--scale 4` draws at 2560x1920, and because the window is shown the rendered image rather
than a copy squeezed back through the guest's pixel format, the extra resolution actually reaches
the screen. The readback is synchronous, because the guest is blocked on the render anyway, which
is what the hardware makes it do.

**Sizing the written frame.** `FB_X_CLIP` and `FB_Y_CLIP` give the region the hardware is allowed
to write, so they size it better than the display registers do. Note that `FB_W_LINESTRIDE` is at
offset 0x04C, before `FB_R_SOF1`, and not after the write addresses: reading it at 0x06C lands on
`FB_Y_CLIP`, whose low bits are the clip's first line and so are normally zero, which makes the
wrong register give a plausible answer.

**Invalidating what a write-back overwrote.** When `--framebuffer-writeback` is on, the texture
cache drops the textures whose pixels lie in the written region
(`TextureCache::invalidate_range`), because a render to texture needs the frame it just drew to
replace what was decoded from those bytes before. The count it reports is also the measurement that
exposed the problem above: about six textures a frame were being dropped at addresses inside the
buffer being written.

**Input.** The window maps a keyboard and any connected pad onto the Maple controller through the
player's own bindings (`render/input.h`), both live at once. The defaults are the layout the
launcher always had: arrow keys for the d-pad, `Z` `X` `A` `S` for A, B, X and Y, return for start,
`Q` and `W` for the analogue triggers. The d-pad also drives the analogue stick, because a title
that steers with the stick and ignores the d-pad is otherwise unplayable from a keyboard; that is a
real binding now rather than something hidden in the launcher, so it can be changed. A pad's stick
and triggers reach the guest as values rather than as flags, past a dead zone, and a digital trigger
ramps to full over about 150 ms so a keyboard accelerator is not only ever fully down or fully up.

The launcher's own keys are separate and deliberately not bindable (`Control` in `vk/window.h`):
`F1` or a pad's select button opens the binding screen, `F12` screenshots, `F11` captures, `F10`
toggles the counter, escape quits -- or backs out, while the screen is up. Keeping them apart is
what stops a layout somebody has broken from also being a layout they cannot escape. The screen
itself (`render/input_menu.h`) is drawn over the frame and stops the guest while it is open, so a
control cannot be captured and played at the same time; the paused time is given back to the pacing
below, or the run would sprint to catch up on closing. `--press` still works alongside all of it,
and a scripted press wins while it is held.

**Pacing.** The guest clock is the reference: the vertical-blank handler sleeps while the host is
ahead of it and never speeds anything up to catch up, because a frame that took too long is gone
and pretending otherwise makes the audio stutter. `--unthrottled` removes the sleep. Crazy Taxi
runs at about 1.6 to 2 times real time at `--scale 1` on an Apple M4, and at about real time at
`--scale 4`.

**Checking a windowed run without a keyboard.** `--screenshot-at N` writes the Nth presented
frame to a PPM, which is how the fix above was verified and how a regression in it would be
caught.

## Plan

1. **Window and first triangle.** Done: SDL3 window, Vulkan device through MoltenVK, the probe.
2. **Vertex assembly.** Done: see above.
3. **Opaque geometry.** Done: see below.
4. **Textures.** Done: see below.
5. **Transparency.** Done: see below. Modifier volumes (shadows) are decoded but not yet drawn.
6. **Framebuffer paths.** Done: conversion both ways, and the live run above. `--window` draws the
   game into a window and presents the buffer the guest nominates, from the renderer when it drew
   that buffer and from video memory otherwise. Writing frames back into video memory is behind
   `--framebuffer-writeback` until the written region can be worked out precisely enough to be
   safe.

Enhancements (internal resolution, widescreen, texture replacement) follow step 4 and are per-game
settings in the title's TOML, since what is safe differs by title. A framerate uplift is *not* a
renderer change: a title's simulation is tied to its own timing, so it is separate per-game work.
