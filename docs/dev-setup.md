# Development setup

## Host (macOS ARM64, Linux x64, Windows x64)

- CMake 3.24+, a C++20 compiler (Apple clang, clang 16+, MSVC 2022), Python 3.9+, git.
- `git submodule update --init --recursive` (libchdr).
- `cmake -S . -B build && cmake --build build --parallel && ctest --test-dir build`.
- `pip install -e "tools/dcdisc[test]"` for the disc tooling and its tests.
- `clang-format` **pinned to 20.1.7** to match the CI check exactly: `pipx install clang-format==20.1.7`
  (the PyPI wheel is the same binary on every OS). A distro or Homebrew clang-format of a different
  major version reflows some lines differently and will fail the check even on unchanged code.

## Playing the game

The renderer is on by default (`DREAM_RENDERER`), but builds only where Vulkan (MoltenVK on macOS),
SDL3 and `glslc` are present; CMake prints whether it found them, and the rest of the project builds
either way. Then:

```
build/dream-dev/games/crazytaxi/crazytaxi_boot \
  --config games/crazytaxi/crazytaxi.toml --window --vmu YOUR_VMU.bin
```

Use the development build for now: the release build stops at the first untranslated call target by
design, and discovery is not closed (WP3.2). Arrow keys are the d-pad, `Z` `X` `A` `S` are A, B, X
and Y, return is start, `Q` and `W` are the analogue triggers, `F10` toggles the frame-rate counter,
`F12` writes a screenshot and escape quits. `--scale 2` or `--scale 4` draws at a higher internal
resolution; `--unthrottled` removes the real-time pacing; `--wav OUT.wav` records the audio; `--fps`
starts with the counter already showing. `--help` lists every flag. Never commit a VMU image: it is
owner data.

The counter reads `59.9 FPS  0.99X`: frames presented per second of wall clock, and guest time per
second of wall clock, where 1.00x is the console's own pace. Both are averaged over half a second,
because an instantaneous figure on a 60 Hz display flickers between two integers and is unreadable.
It is drawn into the presented frame and deliberately not into what `F11` and `F12` write, so a
screenshot is the game's own pixels and stays comparable with another.

### Reporting something that looks wrong

`F11` writes everything needed to reproduce the frame on screen: the picture, the display list that
drew it, video memory and the PVR registers from the same instant, and a note saying where the run
had got to. Five files, `capture-000.{ppm,ta,vram,vram.regs,txt}`, written to the working directory.

That bundle replays offline, so the frame can be looked at again without the game:

```
build/dream-dev/render/dream_render_view --vram capture-000.vram capture-000.ta
```

`F12` writes just the picture, for when the picture is the whole story. `--screenshot-at N` and
`--capture-at N` do the same at the Nth presented frame, for a run with nobody at the keyboard.

A report is most useful as: what you did, what you expected, what happened, and the capture bundle.
A screenshot alone shows the symptom; the bundle lets the same frame be drawn again and taken
apart.

Sound plays through the default device whenever there is a window. `--no-audio` turns it off, and
`--audio` turns it on for a headless run, though a headless run is usually faster than real time and
the sink will report dropping most of what it was given. `--rtc-seed N` fixes the console clock so
two runs of the same build do the same thing, which everything in `docs/differential-harness.md`
depends on.

## Toolchain container (Dreamcast side)

Everything that targets the console runs in `tools/docker`, built on the KallistiOS project's own
image (`kallistios/dc-kos-toolchain`, GCC 14.2, multi-arch):

```
tools/docker/run.sh                       # interactive shell, repo at /work
tools/docker/run.sh make -C tests/kos/hello   # build a KOS test program -> hello.elf, hello.bin
```

Inside: `sh-elf-gcc`, `arm-eabi-gcc`, `kos-cc`, `$KOS_BASE` (prebuilt KallistiOS), `kos-ports`,
CMake, Ninja, Python 3 with pytest and pycdlib. The image is about 250 MB; nothing is compiled from
source at build time.

`tests/kos/` holds the KallistiOS test corpus for the translator's differential harness (WP1.5);
each program is a directory with a `Makefile` following `tests/kos/hello`.

## Reverse-engineering tools (optional, owner machine)

- Ghidra 12 (`brew install ghidra`); headless scripts in `tools/ghidra/`, per-game layout scripts in
  `games/<id>/ghidra/`. Run with `$(brew --prefix ghidra)/libexec/support/analyzeHeadless`.
- Flycast in `/Applications` for the checklist's syscall trace and timing probe.
- Flycast oracle core for the differential test: `tools/flycast/oracle/build_oracle.sh`, then
  configure with `-DDREAM_FLYCAST_CORE=<core>` (`docs/differential-harness.md`).
- Owner-supplied material in ignored folders: `games/*.chd`, `games/<id>/extracted/`, `sdk/`.

## Warnings the Mac cannot reproduce

The three compilers disagree about what is worth warning on, and CI is the only place two of them
run. Two patterns have now broken CI after a clean local build:

- **`-Wconversion` on a bit-field store.** GCC treats a store from a wider value into a bit-field
  as narrowing; clang does not. Mask explicitly: `f.EG = value & 0x1FFF`.
- **Unreachable code after `if constexpr`.** `if constexpr (c) return;` does *not* discard what
  follows; for the instantiations where `c` holds, the rest is compiled but unreachable, which
  MSVC rejects under `/W4 /WX`. **No clang warning catches this, including `-Weverything`**
  (checked 2026-09-12), so write `if constexpr (c) { ... } else { ... }` and never an early
  `return` in a compile-time branch with code after it.

Before pushing C++ that other compilers will see, the cheap local check is GCC through Docker:

```
docker run --rm -v "$PWD:/w" -w /w gcc:14 g++ -std=c++20 -Wall -Wextra -Wpedantic -Wshadow \
  -Wconversion -Wno-sign-conversion -Werror -fsyntax-only -Iruntime/include <file.cpp>
```

There is no equivalent for MSVC on macOS; its warnings are found in CI.
