# dream-recomp

Turns a Sega Dreamcast game into a native program for your own machine.

Not an emulator. An emulator reads the game's original instructions and acts them out, one at a
time, every time you play. This reads them **once**, ahead of time, and writes out the equivalent
C++ — which your compiler then optimises like any other source code. The result is a normal
executable for macOS, Windows or Linux, with no SH-4 processor left anywhere in it.

The same approach as [N64Recomp](https://github.com/Mr-Wiseguy/N64Recomp) and
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp), aimed at the Dreamcast's SH-4 and the
Katana SDK most Dreamcast games were built with.

**You supply the game.** Nothing here contains any Sega code or data, and none is ever committed.
You need your own disc, dumped yourself, of a game you own.

## What actually works today

Being straight about this, because the gap between "it translates" and "it plays" is the whole
project:

| Title | Translates | Builds | Runs |
|---|---|---|---|
| Crazy Taxi | yes | yes | **plays**: boots, music, memory-card save, title screen, menus, attract sequence. Crashes entering a race. |
| Tech Romancer | yes | yes | boots, draws its options screen |
| Metropolis Street Racer | yes | yes | boots and runs, but slowly: its disc carries five code files and only the loader is configured, so almost everything falls back to the interpreter |
| Charge 'N Blast | yes | yes | boots, reads the disc, renders |

So: one title is genuinely playable up to a point. If you bring your own game, expect it to
translate, build, and then stop somewhere. The tooling for finding out *where* and *why* is the
part that is actually mature, and it is described below.

The last two rows are the ones to read twice. Getting either to *build and boot* took one command
and no code; getting one to run *well* is the work.

Roughly 79–90% of each binary's code is found automatically (`docs/emitter-design.md`). The rest is
what you go and find.

## What you need

- CMake 3.24+, a C++20 compiler (Apple clang, clang 16+, or MSVC 2022), Python 3.9+, git
- A disc image you dumped yourself: `.chd` or `.gdi`
- For a window and sound: Vulkan (MoltenVK on macOS), SDL3, and `glslc`. All optional — without
  them everything still builds and runs headless.

## Build

```sh
git clone --recurse-submodules <this repo>
cd dream-recomp
cmake -S . -B build -DDREAM_DEV_INTERPRETER=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
pip install -e tools/dcdisc        # the disc tool
```

Check the machine first if you like:

```sh
dcdisc doctor
```

It lists what is present and what is missing, and says which of it is actually required.

**`DREAM_DEV_INTERPRETER=ON` is the one option that matters.** It keeps a fallback interpreter in
the build for any code the translator did not find. Turn it on: without it the program stops dead at
the first gap, which is by design, and no title is yet complete enough to go without it.

The renderer is on by default (`DREAM_RENDERER`), and quietly skips itself if Vulkan, SDL3 or
`glslc` are missing — CMake prints what it found. Everything still builds and runs headless without
them; only `--window` needs them.

## Recompile your own game

### The short version

```sh
tools/new-game.sh ~/discs/mygame.chd
```

It asks for the few things it cannot work out — the title, a short id, where to put things — and
does the rest: reads the disc, writes the config and the build file, configures, builds, and offers
a first run. Point it at a *folder* of images instead and it lists them to pick from.

It prints every command before running it, because the point is to save the typing rather than to
hide what happened. The same three steps by hand:

```sh
dcdisc new-game mygame.chd                       # extract, write the config and the build file
cmake --build build --target mygame_boot -j8
build/games/mygame/mygame_boot --config games/mygame/mygame.toml --window
```

That is genuinely it for a title that boots. `new-game` reads the name, region, product number and
boot filename out of the disc's own header, so it gets right the things that are easy to assume
wrongly — Charge 'N Blast's boot file is `1ST_READ.US`, not the `1ST_READ.BIN` everything else
hard-codes. The long version below is the same thing done by hand, and is worth reading once so the
files those commands write are not a mystery.

### The long version

Six steps. Crazy Taxi is the worked example; substitute your own title throughout.

### 1. Check the disc

```sh
dcdisc inspect mygame.chd -o report.md
```

Read the report before going further. Two things decide whether to continue:

- **Windows CE titles are out of scope.** The tool says so and exits non-zero. Roughly a tenth of
  the library is Windows CE; those games do not use the Katana SDK and nothing here applies.
- **How many files look like code.** One is the easy case. Several means the game loads modules at
  run time, and you will have to describe each one.

### 2. Extract it

```sh
dcdisc extract mygame.chd games/mygame/extracted
```

This writes the filesystem under `extracted/fs/` and unscrambles the boot executable if needed.
Everything under `games/*/extracted/` is gitignored and must stay that way.

### 3. Describe it

Create `games/mygame/mygame.toml`. The minimum is genuinely this small:

```toml
[game]
id = "mygame"
title = "My Game"

[binary]
path = "extracted/fs/1ST_READ.BIN"
load_address = 0x8C010000     # where the console's BIOS loads the file
link_address = 0x0C010000     # the same RAM through a different address; what the code assumes
entry = 0x8C010000            # the first instruction
```

Those addresses are the same for nearly every Dreamcast game, so start by copying them. Full
reference, including games that copy code around at run time: [docs/game-config.md](docs/game-config.md).

### 4. Add it to the build

Copy `games/techromancer/CMakeLists.txt` into `games/mygame/`, replace `techromancer` with
`mygame` throughout, and add one line to the top-level `CMakeLists.txt`:

```cmake
add_subdirectory(games/mygame)
```

It is a no-op for anyone without your extracted files, which is why the repository can carry game
configs without carrying games.

### 5. Build it

```sh
cmake --build build --target mygame_boot --parallel
```

The translation happens during this build. Watch for the line that says how much of the binary was
decoded:

```text
coverage: 490480 of 621888 bytes decoded (78.9%); 118480 bytes are neither
instruction nor literal pool, in 727 runs of 16 bytes or more
```

That is not a score to optimise. It tells you how much code exists that the program does not yet
contain, and the runs it lists are where to look when something goes wrong later.

### 6. Run it

```sh
build/games/mygame/mygame_boot --config games/mygame/mygame.toml --window
```

Arrow keys are the d-pad, `Z` `X` `A` `S` are A, B, X, Y, return is start, `Q` and `W` are the
triggers, `F10` toggles the frame-rate counter, escape quits.

Useful flags: `--scale 2` draws at higher internal resolution, `--vmu FILE.bin` supplies a memory
card, `--fps` starts with the frame-rate counter showing, `--wav OUT.wav` records the audio,
`--rtc-seed 1000000` fixes the console clock so two runs do the same thing. `--help` lists them all.

Without `--window` it runs headless and prints a report — which is often the faster way to find out
what happened.

## When it doesn't work

It will not work first time. The workflow for that is the part worth learning.

**It stops at an address.** The most common outcome: the game jumps somewhere the translator never
found. Rather than reading the address off the screen, let the run write the fix:

```sh
build/games/mygame/mygame_boot --config games/mygame/mygame.toml --suggest-config next.toml
```

`next.toml` holds the `[functions] extra` seeds for addresses discovery never reached and the
`[[relocations]]` entries for code the program copied and ran elsewhere, skipping anything your
config already has. Paste it in, rebuild, run again. Each seed usually finds more than the one
function.

This is not a marginal convenience. Crazy Taxi's release build — the configuration with no
interpreter in it — used to fault on frame 0. Ten entries found this way, over three rounds, and it
now runs with zero untranslated call targets.

**It draws something wrong.** Press `F11`. You get five files — the picture, the display list that
drew it, video memory and the graphics registers from that exact instant, and a note on where the
run had got to. That bundle replays without the game:

```sh
build/render/dream_render_view --vram capture-000.vram capture-000.ta
```

This matters more than it sounds. A rendering bug you can replay offline is one you can bisect;
a screenshot is one you can only argue about.

**It behaves differently from the real thing.** The project's reference is Flycast's interpreter,
run as an oracle and compared instruction by instruction against the translated code. See
[docs/differential-harness.md](docs/differential-harness.md). This is how the translator's
correctness is argued rather than asserted, and every emitter change is required to come with one.

**Something drifts partway through a long run.** `--write-hash FILE` writes a rolling hash of every
memory write, one line per frame. Two runs that agree line for line did the same thing; the first
line that differs is the frame where they parted.

## How it works, briefly

| Layer | Approach | What that means |
|---|---|---|
| CPU | **Compiled** | Each guest function becomes a C++ function. Registers are local variables. No interpreter in a release build. |
| Memory | Emulated | 16 MB RAM, 8 MB video memory in both of the hardware's two views, store queues. Address translation is a mask and an index. |
| Graphics | Emulated (low level) | The real PowerVR2 display lists are decoded and drawn through Vulkan. The game talks to the hardware exactly as it did. |
| Audio | Emulated (low level) | The game's own sound driver runs on an emulated ARM7, feeding an emulated mixer. |
| BIOS and disc | **Replaced** (high level) | Console BIOS calls and the GD-ROM filesystem are answered by native code, not emulated firmware. No BIOS image needed. |

So it is a hybrid: high-level where the console's own software would otherwise have to be emulated
(and legally supplied), low-level everywhere the game touches hardware directly — because that is
where games rely on exact behaviour.

Deeper: [docs/emitter-design.md](docs/emitter-design.md) for how SH-4 becomes C++,
[docs/runtime-render.md](docs/runtime-render.md), [docs/runtime-aica.md](docs/runtime-aica.md),
[docs/runtime-memory.md](docs/runtime-memory.md) for the hardware side.

## Rules

- **Never commit game data, BIOS images, flash, memory-card saves, or SDK files.** The `.gitignore`
  covers the usual cases; check `git status` anyway.
- **Never paste Sega SDK source or headers into this repository**, including into comments.
  Describe the interface in your own words, or point at the equivalent in
  [KallistiOS](https://github.com/KallistiOS/KallistiOS), which is BSD-licensed.
- Windows CE titles are out of scope.
- Emitted code must behave identically on x86-64 and ARM64; see ADR 16.

## Where to read more

| Document | What it covers |
|---|---|
| [docs/dev-setup.md](docs/dev-setup.md) | Full setup, every launcher flag, the toolchain container |
| [docs/game-config.md](docs/game-config.md) | The TOML format, relocations, overlays, symbol files |
| [docs/emitter-design.md](docs/emitter-design.md) | How SH-4 becomes C++: delay slots, FPU modes, interrupts, coverage |
| [docs/differential-harness.md](docs/differential-harness.md) | Testing against Flycast as an oracle |
| [docs/decisions/](docs/decisions/) | Architecture decision records, with the reasoning |
| [docs/progress.md](docs/progress.md) | Where every work package actually stands |
| [docs/implementation-plan.md](docs/implementation-plan.md) | The four phases and what remains |
| [tools/dcdisc/README.md](tools/dcdisc/README.md) | Disc tooling in detail |

## Licence

GPL-2.0, see [LICENSE](LICENSE). Flycast is GPL-2.0 and parts of it are ported here deliberately;
that decision and its consequences are ADR 1.
