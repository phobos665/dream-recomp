# Future enhancements

What a recompilation can offer beyond running the game: rebindable input, runtime options, and
modifications. This is a design study, not a plan. Nothing here is scheduled, and none of it should
displace closing the emitter defect that WP3.2 is blocked on, because every feature below is worth
less on a build that stops partway into a race.

Written 2026-09-13 from a discovery pass over the repository and over the Flycast checkout in
`build/flycast-src`, which is the ADR 1 reference implementation and already contains working
versions of two of the three things studied here. Effort figures are engineer-days for the work when
it is picked up.

## Summary

| Area | Size | Kind of work |
| --- | --- | --- |
| Input binding, keyboard and gamepad | **done 2026-09-14** | Engineering |
| Widescreen, true Hor+ | 11 days (7-21) | Engineering, with a defect to fix first |
| Fix the supersample resolve | **1 day** | A defect, and the best visual return on this list |
| Anti-aliasing after that | 5 days (3-10) | Engineering |
| Frame generation to 120/144 | 35 days (17-75) | Half engineering, half research |
| Delta-time conversion of the title | 1 day to size it, then unknown | Research |
| VMU screen on the display | 2.5 days (2-3.5) | Engineering |
| Runtime options: counters, frame pacing, internal resolution | 6 days | Engineering, and two defect fixes |
| Texture replacement | 6 days | Engineering, with a reference to port |
| Audio replacement | 4 days to find out, 5 to 10 to do | Conditional |
| Model replacement | Open-ended | Research |
| Adding characters or locations | Open-ended | A research programme |

Input binding landed on 2026-09-14 and section 1 below is kept as the design record rather than as
a plan. Widescreen and the VMU screen have studies of their own
(`widescreen-crazytaxi-study.md`, and the wire format in `runtime-maple.md`); the sections here are
summaries, and the studies are where the addresses and the evidence live.

**Do input binding first.** It is the only one that removes a blocker rather than adding a feature.
Crazy Taxi is a driving game with an analogue trigger and an analogue stick; on a keyboard the
accelerator is fully down or fully up, so the baseline title cannot honestly be evaluated as a game.
WP3.4's deliverable is an acceptance matrix of both maps at every time setting and every Crazy Box
stage, which is fifty-odd rows of play-testing that only the owner can do. Every row is faster and
more accurate with a pad. It also pays for the other two areas, because the overlay it needs is the
same overlay the options panel and any mod-authoring tool need.

## 1. Input binding

### What already exists

The seam is already drawn. `render/include/dream/render/vk/window.h` defines `enum class Control`
with twelve entries and says why: SDL's key codes stop at the window, the launcher maps `Control`
onto the Maple controller, and the runtime never learns what a keyboard is. `Live::read_controls`
in the launcher is the only consumer, and `--press` already arbitrates against live input, with a
scripted press winning while it is held.

What is missing is everything else: no gamepad support at all, no representation of "a physical
input" that can be stored or compared, no user configuration file anywhere in the project, and no
way to draw text over the frame.

### Where bindings should live

Two files, split on a principle rather than convenience.

**The game's TOML gets an `[input]` profile**: which Dreamcast controls this title actually uses, and
what they mean. Crazy Taxi needs an analogue accelerator on the right trigger, a brake on the left
and a steering axis; a title with digital controls needs none of that. That is per-title, it is
reviewable, and it belongs with the other per-game settings.

**A per-user file gets the actual bindings**, keyed by game and by gamepad identifier, under the
platform's configuration directory. This is the part that must not be committed: the game TOML is a
tracked file, so putting a player's keys in it would mean every rebind dirties the repository.

With nothing in the user file, the defaults are today's keyboard table plus a gamepad mapping
derived from SDL's own button names, so a first run with a pad plugged in works without visiting any
UI.

One wrinkle: `GameConfig` lives in the translator library, which ADR 1 asks to keep free of runtime
dependencies so it could be relicensed. Player-facing settings do not belong in its data model. A
separate runtime configuration loader should read the same file and own `[input]`, `[video]` and
`[mods]`, leaving `GameConfig` to describe the binary. That decision deserves its own ADR, because
it fixes the file layout every later option lands in.

### How the UI gets drawn

The renderer is bespoke Vulkan with no text, no widgets and no font. The options are Dear ImGui, a
hand-rolled overlay, or an external launcher window.

**Dear ImGui**, vendored at a pinned tag and built only inside the optional Vulkan library. It is
what the reference implementation uses, so porting its binding behaviour is reading rather than
inventing. It is licensed compatibly. A binding screen specifically needs modal "press an input"
capture, a device list that changes under hot-plug, and scrolling, which are the three things a
hand-rolled overlay is worst at. And the same integration pays for the options panel and any future
debug panel, so it is amortised over three features.

The honest counter-argument: if the owner wants only a frame-rate counter and never a menu, ImGui is
heavy. That is why the counter is sequenced behind this decision rather than in front of it.

### Analogue input, which is where this stops being trivial

The Maple axes are single bytes centred on 0x80. Four things need to exist that do not:

- **Triggers** scale from SDL's range directly. The keyboard stays all-or-nothing, which for a
  driving game means full throttle or none. A ramp to full over about 150 ms while held is a cheap
  and genuinely noticeable improvement, and should be an option rather than a silent behaviour.
- **Sticks** need an explicit centre. A stick that rests at 0x7F instead of 0x80 makes the car drift
  forever and reads as a physics bug, so the scaling deserves a unit test.
- **A dead zone**, per axis, configurable, or a worn stick steers on its own.
- **Conversion both ways.** The existing d-pad to stick synthesis must stay for keyboard players, and
  a stick to d-pad synthesis is needed for menus in titles that only read the d-pad.

**Hot-plug policy worth deciding explicitly:** unplugging the pad should release every button rather
than freeze the last state, or a pad yanked mid-race leaves the accelerator held.

### Keeping the scripted-input path working

`--press` is what the headless tests and the audio bring-up depend on, so it must keep working with
no window and no binding file. It writes the controller state directly and stays upstream of the
binding layer. While refactoring, `--press` should learn to express axes, so a scripted regression
run can drive a car; that costs an hour and unblocks automated gameplay tests.

### Risks

The Vulkan overlay integration is the only real technical risk and it is modest; the reference ships
the same thing on macOS. Beyond that: scope creep into a general settings GUI, a rebind that makes
the game unplayable with no way back (needs reset-to-defaults and a configuration file that is
regenerated rather than fatal when it fails to parse), and a key held while the overlay is open
reaching the game, which needs an explicit input-focus state.

## 2. Runtime options

### Two defects found while studying this

These are worth fixing regardless of whether any options UI is built.

**The pacer carries its deficit forever.** The comment says it never speeds up to catch up, because a
frame that took too long is gone and pretending otherwise makes the audio stutter. But the target is
absolute, so after a host stall the guest runs flat out until the debt is repaid, which is exactly
the behaviour the comment disowns. The symptom is bursts of dropped audio after every hitch. The fix
is to keep the absolute target, which is the right way to avoid drift, but clamp the deficit to
about one frame.

**Vsync is a second, invisible pacer.** The swapchain hard-codes the always-available present mode,
which blocks until the display's next refresh inside the guest's vertical-blank callback. Against a
60 Hz display this is benign. On a 50 Hz display, or a compositor throttling a background window,
the guest is held *below* real time, the audio device drains, and nothing reports it, because the
sink counts dropped blocks but not starvation. Present mode should be an option, and the sink needs
a starvation counter so the two failure directions are distinguishable.

The general rule, which belongs in the renderer notes: **there must be exactly one authority for real
time.** Today it is the guest clock, with vsync as an uncontrolled second authority and the audio
device as a third that copes by discarding. The reference implementation instead makes the audio
device the authority and is glitch-free by construction, but that couples the run rate to a device
that may not exist. Keeping the guest clock as the authority and making vsync subordinate to it is
the smaller change.

### What a frame-rate counter should measure

Three rates are in play and they are genuinely different numbers here, so conflating them would hide
the faults the counter exists to show.

| Rate | Source | What it tells you |
| --- | --- | --- |
| Guest frames per second | the video timing generator | whether the game is running at the right speed |
| Renders per second | Tile Accelerator renders | the game's own internal frame rate |
| Presents per second | host swapchain | the display pipeline's health |

On Crazy Taxi the first two differ by about half during the boot, so a single "FPS" number would be
meaningless. Show all three, plus speed as a percentage of real time, plus the audio queue depth,
which is the early warning that the others are about to go wrong. Measure over about half a second;
an instantaneous rate is noise.

### What can change mid-run

More than expected. Pipelines are built with dynamic viewport and scissor state and the pipeline key
carries no size, so changing internal resolution needs only the offscreen images and framebuffer
rebuilt while the render pass stays alive. That sidesteps the question of pipelines referencing a
destroyed render pass rather than reasoning about it. The cost is a device wait and a few megabytes
reallocated, which is a hitch of a frame or two for a deliberate user action.

Only the validation layers and the audio device are genuinely restart-only.

### A caution about naming

"Uncapped frame rate" is not available on this architecture and the renderer notes already say why:
a title's simulation is tied to its own timing, so removing the cap gives the game in fast-forward,
not more frames. Anything labelled "uncap" in a menu will be read as the sixty-to-unlimited uplift
people know from other recompilations, and will disappoint. Call it speed. If a cap below the guest
rate is ever wanted, implement it by skipping presents and never by slowing the guest clock, which
would slow the sound hardware with it.

Determinism must survive all of this: the write-hash comparison and the fixed clock seed depend on a
reproducible run, so no option may touch the virtual clock.

## 2a. Widescreen, and why it is the showcase enhancement

Full evidence, addresses and disassembly in `widescreen-crazytaxi-study.md`. This is the summary.

**What is wanted is Hor+**: a wider view showing more of the city at the sides, with the vertical
field of view, every model proportion and every texture ratio untouched. Not a stretched 4:3 image,
not a cropped one. The distinction is the whole feature: a stretch is half a day and looks wrong to
anyone who has seen the game, and it is what an emulator's renderer-side widescreen gives you.

**It cannot be done in our renderer**, and establishing that is what makes the rest tractable.
Vertices arrive already projected, because the SH-4 does its own transform and lighting and hands
the Tile Accelerator screen-space coordinates with 1/w. A matrix on our side can stretch the picture
or pad it with blank margin; it cannot widen a field of view that was already applied. The
projection has to change where it is computed, which is inside the title.

**Where it is computed**: a viewport block at `0x0C148618`..`0x0C148638`, written by `0x0C078AC0`
and `0x0C078150`, holding a normalised horizontal scale, a normalised vertical scale, a width of
640.0, a height of 480.0 and two origins, with the pixel scales cached at `0x0C2B0930` and
`0x0C2B0934`. The two axes come from separate expressions, which is the lever: set the width to
853.33 and scale the horizontal normalised term by 0.75, and the horizontal pixel scale stays
exactly 320.0 while the frustum widens. The vertical half comes out bit-identical.

**The part an emulator cannot match**: the frustum clip test at `0x0C080FE8` reads the same block,
so the clip planes widen with the view rather than staying at 4:3. That is what stops geometry
popping in at the new edges, and it is the argument for doing this in a recompilation at all.

**What the estimate is actually spent on.** The 3D half is small. Three days central, six high, go
on the 2D layer: `fn_0x0C07D018` is the sprite choke point with over a hundred callers and it
multiplies every quad's X by the viewport width, so widening *actively stretches the HUD* unless 2D
is separated from 3D first. The Tile Accelerator is fed by bulk DMA, so "which code submitted this
quad" is a real question; there is a half-day measurement -- can a cheap depth, list-type or sprite
classifier separate them -- that would remove most of those days if it works. The other driver is
that the TOML's `[hooks]` and `[hle]` sections are schema that nothing implements yet, so the hook
mechanism is part of this job.

**Most of the cost is not Crazy Taxi.** Only the addresses are: they go in
`games/crazytaxi/crazytaxi.toml` and they are tied to the dump `sha1_1st_read` already records. The
hook mechanism (`translator/`, `runtime/`) and the wider render target and aspect handling
(`render/`, `runtime/boot/boot_main.cpp`) are shared, so the second title's widescreen is a
fraction of the first's. There are no hand-written per-game C++ files and this does not introduce
any; emitted code stays generated.

**A 3.5-day slice exists and should not be mistaken for the feature.** It ships the anamorphic
version: world correct, HUD stretched by 4/3. Useful as a stepping stone to prove the projection
patch, not as something to show anyone.

**Corroboration, and one loose end.** Flycast carries a per-game widescreen cheat for Crazy Taxi USA
that writes 240.0f over a 320.0f, exactly the 0.75 this approach needs. Its address sits 0x80 away
from the cached horizontal scale found here by static analysis. Close enough to be encouraging, not
close enough to call a match -- resolve it early, because it is either strong confirmation or a sign
that one of the two addresses is wrong.

## 2b. The VMU screen

Titles draw on the memory card's little screen; Crazy Taxi sends it an image every few frames, 917
of them in a 150-second run. The frame is one Block Write of 192 bytes, 48x32 pixels at one bit
each, and the layout is settled against KallistiOS rather than guessed -- see `runtime-maple.md`.
Today those writes are counted and discarded.

Draw it as an overlay in the game window rather than opening a second one. `Presenter::upload()`
already takes a decorate callback with two clients (the frame-rate counter and the binding screen),
`fill_rect` is exactly the primitive a monochrome grid needs, and `implementation-plan.md` already
scopes WP2.4 as LCD rendering to an overlay. A genuine second window is roughly double, because
`vk::Window` owns its own Vulkan instance and device, `poll()` does not filter by window id -- so
closing the VMU window would end the run -- and `destroy()` tears down the SDL subsystems, a file
that has already caused one crash on exit.

One thing stays open: whether bit zero is the physical top-left or the bottom-right. KallistiOS
ships `vmu_draw_lcd` and `vmu_draw_lcd_rotated` side by side because a card seated in a controller
is upside down relative to one held alone, so both orientations are real. One look at a frame from a
title drawing text settles it.

## 2c. Frame rate: what 144 fps can and cannot mean

Full working in `rendering-enhancements-study.md`. This section is the shape of the problem and the
one measurement that decides it.

**Crazy Taxi is VBlank-locked, and this is now measured rather than assumed.** The VBlank interrupt
handler at `0x0C169F40` increments a counter at `0x0C2E7E90`; the library's WaitVsync at
`0x0C156CFC` snapshots that counter and spins with `cmp/eq` until it changes. Every one of the
twelve sites that reads the counter compares it for equality. **Nothing anywhere subtracts it.**
There is no elapsed-time value in the binary: the only rate constant that reaches a calculation is a
hardcoded 1/60, and the title's own frame-rate option is an integer VBlank divisor clamped to 0, 1
or 2 -- 60, 30 or 20 Hz -- set from game code at `0x0C07399C`. The game counts ticks; it never reads
a clock.

The hardware timer is a red herring worth recording so nobody chases it twice. TMU0 is fully
programmed at `0x0C0742C0` and a complete microsecond elapsed-time API sits at `0x0C074550` to
`0x0C0745B0`, with a real delta computed at `0x0C048ADC`. **None of it has a reachable caller.** It
is development leftovers, started and never read.

This closes an item `baseline-game.md` still lists as unverified, and answers it the other way from
the guess: the lock is the **VBlank interrupt**, not SPG_STATUS polling. It also corrects a runtime
assumption -- `runtime-interrupts.md` defers idle-loop fast-forward pending a measurement on this
title, and the measurement says the game spins on a **RAM word**, not on MMIO, so a detector that
only watches device registers will never fire here.

**So three different features hide under "144 fps", and two are not features.**

| Reading | Verdict |
| --- | --- |
| Run the guest faster | The simulation steps per VBlank, so the game plays at 2.4x speed. Available today as `--unthrottled`. Not a feature, a bug. |
| Present the same image more often | No benefit; the guest still produces sixty distinct frames. |
| **Generate intermediate frames, guest untouched at 60 Hz** | The only real one. |

**Target 120 before 144.** Sixty into 144 is 2.4x, which does not divide: you would alternate between
generating one and two frames, and irregular cadence reads as judder. Sixty into 120 is exactly 2x,
one generated frame per real frame, perfectly regular. A steady 120 will look better than a lumpy
144, and panels that do 144 do 120.

**The approach that fits is depth-aware reprojection**, and it is another case where a recompilation
can do what a black-box emulator cannot. Frame generation normally has to infer motion from pixels.
We have the depth buffer, and `widescreen-crazytaxi-study.md` already located the guest's camera
block at `0x0C148618`, so the actual camera motion can be read out of guest RAM. In a driving game
the camera is most of the motion on screen. Geometry interpolation is the weaker sibling: the Tile
Accelerator stream is a flat polygon list with no object identity, so there is nothing to match
across frames.

**The decision rests on latency, not on rendering.** Interpolating between frames N and N+1 means
holding N+1 back to display it late: about 16.7 ms of added input lag. For a game about threading
traffic at speed, 144 fps that feels less responsive than 60 is a downgrade, and players notice lag
sooner than smoothness. Extrapolating forward from the newest frame costs no latency but has to
invent detail where moving objects uncover what was behind them, which shows as shimmer at edges.

**Scope**: the reprojection, the extra presents and the pacing are generic (`render/`,
`runtime/boot/boot_main.cpp`). Per game it is one address block -- where the camera lives -- in the
TOML. Keeping the HUD from ghosting needs the same 2D/3D separation widescreen needs, so that work
is shared between the two features and should be sequenced once, not twice.

### What the study concluded

**Frame generation: 35 days central, 17 to 75.** Of that, 13/21/31 is real engineering and
7/16/45 is research -- disocclusion fill and moving objects are the open problems, and they are the
reason the high end is so wide. Minimum honest slice is 13 days.

Two findings sharpen the approach. **Geometry interpolation and TAA are unavailable rather than
merely expensive**: `Polygon` and `Vertex` carry no identity (`render/include/dream/render/display_list.h:36-56`)
and the previous frame is discarded outright (`runtime/boot/boot_main.cpp:159`), so there is nothing
to correspond across frames and no way to produce motion vectors. But the camera *is* recoverable:
`Ctx::xf[16]` is XMTRX (`runtime/include/dream/runtime/sh4/ctx.h:37`) and the title transforms every
vertex through it -- `ftrv xmtrx,fv4` at `0x0C080F1C`, 150 of them in the image -- so a guest hook
gets us the camera matrix per frame.

So: **depth-aware camera reprojection, run as forward extrapolation rather than interpolation**,
with the 2D layer excluded and re-composited unwarped. Extrapolation because interpolation means
holding a frame back, and 16.7 ms of added lag in a driving game is a worse trade than the edge
shimmer extrapolation costs. One prerequisite: depth is currently thrown away
(`render/src/vk/offscreen.cpp:28-32,46` -- no SAMPLED or TRANSFER_SRC usage, `storeOp DONT_CARE`),
so it has to be kept before anything can warp by it. The log-depth encoding inverts in closed form.

## 2d. Anti-aliasing, and a defect worth fixing first

**Do not build MSAA.** It conflicts structurally with the per-pixel OIT direction the project has
accepted -- Flycast's OIT path is a three-subpass input-attachment A-buffer with no multisampling
anywhere in its Vulkan backend -- and between the `gl_FragDepth` write and the `discard`-based
punch-through list there is little left for it to do.

**There is a one-day fix that is probably the largest visual improvement available anywhere on this
list.** `--scale` already supersamples: at scale 4 the geometry is drawn at 2560x1920. The presenter
then throws that away with a **nearest-neighbour** resolve. `smooth` is declared false at
`render/include/dream/render/vk/present.h:53`, selects `VK_FILTER_NEAREST` over `LINEAR` at
`render/src/vk/present.cpp:74`, and **is never assigned anywhere in the tree** -- verified by grep,
2026-09-14. There is no mip chain either. So every user running `--scale 2` or `--scale 4` today is
paying the full rendering cost of supersampling and receiving a point-sampled image for it.

Fix the resolve first -- a box downsample in `present.frag` -- and measure before deciding whether
anything further is wanted. Proper SSAA may simply be enough. If it is not, SMAA 1x in the same
fullscreen pass, with FXAA as the cheap first cut. Five days central for the lot, three to ten, of
which the first day is the resolve.

### The other route: give the game a delta time

Worth recording because it is the recompilation thesis taken to its conclusion, and because it is
the thing no emulator could ever attempt. We hold C++ for every function in the title. In principle
the fixed step could be replaced with a real elapsed time, after which the game runs correctly at
any rate with no interpolation, no added latency and no artefacts at all. It is the *right* answer
if it is reachable.

What makes it hard is that there is no timestep to change. The step is **implicit**: the code says
`position += velocity` and the velocity is already per-frame, so the 1/60 exists only in the
tuning of the constants. Four specific obstacles, all of which want measuring before anyone commits
to a number:

- **No types.** Emitted code is register arithmetic on floats. Nothing marks a value as a velocity,
  an acceleration or a counter, so integrations cannot be found mechanically; they have to be
  identified by hand.
- **It is everywhere.** Vehicle physics, pedestrian and traffic behaviour, animation, the fare
  timer, particles, camera smoothing, spawn logic. Hundreds of sites, each implicitly once-per-frame.
- **Not all of it is float.** Integer counters and fixed-point state do not scale by a fraction
  without either breaking or accumulating error.
- **The tuning is 60 Hz tuning.** Jump distances, collision resolution order and the Crazy Box
  challenges were balanced against a whole-frame step. Changing the step changes the game, subtly
  and everywhere, and the acceptance matrix in WP3.4 is what would have to catch it.

There is a middle route that keeps most of the prize. Leave the simulation at 60 Hz untouched and
interpolate **game state** rather than pixels: capture object transforms on frame N and N+1 and
re-render real geometry at an in-between position. That gives true motion rather than a warped
image -- no disocclusion holes, no ghosting -- and we know the projection well enough to resubmit
geometry, because the widescreen work had to find it. It still costs the latency of holding a frame,
and it needs the transform storage located, but it degrades far more gracefully than reprojection.

**Estimate: deliberately not given here.** The interpolation study will carry numbers for
reprojection. Delta-time conversion is research rather than engineering until someone has measured
how many integration sites there actually are -- a bounded first question, and the right thing to
spend a day on before anybody estimates the rest. This is the one item on this list where "it works
but does not look good enough to ship" is a realistic outcome, and an optimistic number would be the
least useful thing to write down.

## 3. Modifications

Three of the four requests here are not the same kind of work, so they are separated rather than
listed together.

### Texture replacement: tractable

The texture cache decodes once, keyed on the two hardware words, and uploads plain RGBA, so
substituting a replacement is mechanically "use this image instead". The decode is already separated
from the upload.

**The correction that matters: the cache key is not a usable identity for a replacement pack.** It
contains the texture's video memory address, and a title that streams a city reloads the same art at
different addresses. A pack keyed that way would work on the title screen and fall apart in the
city. The identity must be a **content hash** over the raw video memory bytes, with the compression
codebook hashed separately, exactly as the reference does. Raw rather than decoded, because it is
cheaper, because it matches the reference so packs are interoperable in principle, and because
decoded output would change if a decoder bug were ever fixed, silently invalidating every pack.
Once a hash scheme ships, packs depend on it forever, so it should be versioned in the pack manifest
from the first release.

A **dump mode** comes first and is worth more than the replace path, because without it nobody can
author a pack. Both want a PNG writer, which is a small header-only dependency the reference already
uses.

Limitations to document rather than discover: a replacement may be a higher resolution but its
aspect ratio must match or the art distorts, and an indexed texture whose palette the game animates
cannot be replaced by a flat image without losing the animation.

The characteristic failure is a pack that looks right on the title screen and wrong in the city, so
the acceptance test has to be in-game rather than on a capture. Memory is a real ceiling: a
high-resolution pack is gigabytes, the cache has a fixed descriptor budget and no eviction policy,
and the cache currently drops everything on a palette change, which with a large pack means
re-uploading the world. An asynchronous preload path, which the reference has, is a real part of the
effort rather than a refinement.

### Audio replacement: conditional

Crazy Taxi streams CRI ADX from a large archive on the disc. The runtime's disc layer is
sector-level, with no file-level interface. Four seams exist, in increasing order of ambition:

1. **Sector redirect.** Wrap the disc reader so a range of sectors comes from a host file instead.
   The game's own decoder then plays your audio and nothing else changes. About 200 lines,
   title-agnostic, testable, and it needs no understanding of the sound driver. The constraint is
   real: the replacement must be the same size or smaller and padded, in a format the decoder
   accepts.
2. **Sound memory interception**, which works for short effects and is hopeless for a stream.
3. **Sound-driver mailbox emulation**, which ADR 10 already records as a possible later enhancement
   layer for music replacement. It would make track substitution trivial, and it is also, in that
   ADR's own words, undocumented and driver-version specific. The traffic can now be observed, so
   the research is possible, but it is research.
4. **Final-mix substitution**, which is crude and gets the timing wrong.

**Recommendation: the sector redirect first, with the mailbox recorded as the eventual right answer.**
Before committing to it, one day of investigation settles whether the audio is decoded on the main
processor or in the sound driver, because that decides how tight the format constraint is.

The risk worth stating: a wrong-sized replacement desynchronises the archive index and the game reads
garbage, which is silent and sounds like corruption. Streaming timing is also already named in the
plan as a race-prone area, and changing the size of what is streamed perturbs it.

### Model replacement: research

Geometry never exists as a file. It arrives as a stream of parameters through the Tile Accelerator,
and by the time the renderer sees a character the model has already been transformed to screen
space: the coordinates are pixels and depth is a reciprocal. The library did the transform on the
main processor and sent the results.

Three consequences follow, and together they make this a different category of problem from
textures. There is no stable identity for "the taxi's body", because the parameter stream differs
every frame. Replacing it means hooking the submission function before the transform, recovering the
model from guest memory in the game's own format, and substituting. And skinning and animation live
in the game's code, so a replacement must match its skeleton.

The project does have the hooking mechanism, and the symbol database names hundreds of library
functions, so the hook is supported. What is missing is knowing which function, what its arguments
mean, and what the model format is. **That is weeks to months per title, and the first weeks produce
nothing visible.**

Two things are worth doing instead, in this order. A **model dumper**, converting a captured frame's
geometry to a standard mesh format, is about 2 days; the meshes are posed and projected and so
useless for replacement, but they make every later investigation cheaper. And **per-model material
override** is achievable on top of the texture work with no reverse engineering at all, since
polygons can be identified by which texture they use; it covers a surprising share of what people
actually want from a visual mod.

### Adding characters or locations: a research programme

This needs the model and animation formats, the level and collision format, the archive layout well
enough to insert rather than replace, the table the game indexes characters through, the spawn data
for a location, and the streaming schedule that decides what is resident. Each is a
reverse-engineering project and they compose: a new character with no animation data crashes, and a
new location the streaming code does not know about never loads.

The recompilation does give one genuine advantage over an emulator: the game's code is C++ that can
be hooked at named functions, so once the formats are understood, inserting is easier than it would
be under emulation. But understanding the formats is the whole job, and the recompilation does not
help with it. **This should not be estimated until texture replacement and a model dumper exist and
have shown what the data looks like.**

## How a modification pack is identified, stored and shipped

One rule covering all of the above, because getting it wrong is a licensing problem rather than a
bug.

A pack is a directory or archive with a manifest naming the pack, its author, its licence, the game
and disc it targets, and the hash-scheme version. Assets inside are named by content hash.

**Packs live in gitignored directories and are never committed.** A texture pack derived from a
retail game is a derivative of the publisher's art however much was repainted, and the project's
hard rule against committing game data covers it absolutely. The ignore entries should be added at
the same time as the loader, not afterwards.

The game's TOML gets a `[mods]` section listing pack search paths and load order, which is per-title,
reviewable, and contains no game data. The repository ships loaders, dumpers and the manifest
schema. It never ships assets.

On licensing: under GPL-2.0 anything linking the runtime is covered, but **data read by it is not**,
so third-party packs can carry whatever licence their authors choose. Worth writing down once so it
is not re-litigated per pack.

## Sequence

0. ~~**Input binding**~~ -- done 2026-09-14, along with the overlay the rest of this list needs.
1. **Runtime options**, because two of its findings are defects, and because the performance profile
   of the open city is already a deliverable in the plan with no instrument to measure it.
2. **Widescreen**, because it is the showcase: it is the one enhancement that is *better* in a
   recompilation than in any emulator rather than merely equal, since the clip planes widen with the
   view and the geometry does not pop in. Most of its cost is the hook mechanism and the wider
   render path, both of which every later title and several items below reuse.
3. **Texture replacement**, because it is the next most visible difference from an emulator, and
   there is a reference to port.
4. **The VMU screen**, cheap and self-contained, and it shows a piece of hardware no PC port has.
5. **Audio replacement** after its one day of investigation.
6. **Model work**, labelled as research in the plan, after a dumper has shown what the data is.

All of it after the emitter defect. A showcase built on a build that stops partway into a race is a
showcase nobody gets to the end of.

## Picking a showcase set

For demonstrating what a recompilation gives you that an emulator does not, the argument is
strongest where the answer is not "the same thing, faster":

- **Widescreen** -- an emulator widens the picture; we widen the frustum, so the culling follows and
  nothing pops in at the edges. This is the clearest example of the whole thesis.
- **Rebindable input with real analogue travel** -- done. The trigger ramp exists because a keyboard
  accelerator that is only ever fully down or fully up is most of why a driving game needs a pad.
- **Texture replacement** -- reaching into the title's own data rather than filtering the output.
- **The VMU screen** -- hardware no PC port of anything has ever shown.
- **Frame rate and anti-aliasing** -- see `rendering-enhancements-study.md`. Be careful what is
  claimed here: raising the frame rate of a title whose physics step per vertical blank makes the
  game run fast rather than smooth, and that distinction is the difference between a showcase and an
  embarrassment.

What to leave out of a showcase: anything that is merely a higher-resolution version of what Flycast
already does well. `--scale` is genuinely useful and genuinely unremarkable.
