# dcdisc: Dreamcast disc image tooling (WP0.3)

Reads GDI and CHD images through one interface and does everything the disc verification checklist
needs above the container: ISO9660 extraction, IP.BIN decoding, 1ST_READ.BIN descrambling, SH-4
code detection, SDK banner search, and the inspection report.

```
pip install -e tools/dcdisc            # gives the `dcdisc` command
pip install -e "tools/dcdisc[test]"    # plus pytest and pycdlib for the synthetic-disc tests
```

## Commands

| Command | What it does |
|---|---|
| `dcdisc backends` | Which CHD backend is available (libchdr path, chdman path) |
| `dcdisc info IMAGE` | Track table and IP.BIN summary |
| `dcdisc ls IMAGE` | List the ISO9660 filesystem |
| `dcdisc extract IMAGE OUTDIR` | Write `IP.BIN`, the filesystem under `fs/`, and the boot binary in plain form (descrambled only if the image stores it scrambled) |
| `dcdisc chd2gdi IMAGE.chd OUTDIR` | Convert a CHD to a GDI set (`track01.raw`, `track02.bin`, ...) |
| `dcdisc inspect IMAGE [-o report.md] [--json]` | Steps 1 to 7 of the checklist in `docs/baseline-game.md`; exit code 1 if Windows CE markers are found |
| `dcdisc manifest IMAGE` | JSON with size and SHA-256 of every file, plus the descrambled boot hash |
| `dcdisc descramble IN OUT`, `dcdisc scramble IN OUT` | 1ST_READ.BIN slice permutation, both directions |
| `dcdisc scan FILE [--strings N]` | Code heuristics and SDK banners for any single file |
| `dcdisc new-game IMAGE [ID]` | Set a title up: extract it, and write `games/<id>/<id>.toml` and its `CMakeLists.txt` from the disc's own header. Refuses Windows CE titles, and refuses to overwrite an existing config without `--force` |
| `dcdisc doctor` | Whether this machine can build and run the project: CMake, compiler, submodule, CHD backend, and the optional Vulkan/SDL3/glslc/clang-format |
| `dcdisc shortlist IMAGE [IMAGE...]` | One row per disc: boot size, code-file count, streaming volume, WinCE, Katana banners. For choosing a baseline title from a pile of dumps |

`IMAGE` is a `.gdi` or a `.chd`. Add `--chdman` to force the chdman backend for a CHD.

## CHD backends

Two independent paths, because CHDs need a decompressor and you may have one tool but not the other:

1. **libchdr** (preferred). The BSD-3 library used by every Dreamcast emulator. Found via, in order,
   `$DCDISC_LIBCHDR`, a build of the `third_party/libchdr` submodule under `build*/`, then the system
   (`apt install libchdr-dev` on Debian/Ubuntu; on macOS build the submodule or use Homebrew if a
   formula is available). Only hunk decompression is delegated to it; header and track metadata are
   parsed in Python, so `dcdisc info` on a CHD works with no native library at all.
2. **chdman** (fallback). MAME's tool, via `$DCDISC_CHDMAN` or `PATH` (`apt install mame-tools`,
   `brew install mame`). `dcdisc` runs `chdman extractcd` into a temporary directory and reads the
   result as a GDI. Always correct, slower, needs scratch space equal to the uncompressed disc.

To add the submodule (needs network access to GitHub, so it is done from a normal checkout):

```
git submodule add https://github.com/rtissera/libchdr third_party/libchdr
```

Building it is part of WP0.1's CMake work; until then `apt install libchdr-dev` or `DCDISC_LIBCHDR`
pointing at any libchdr shared library is enough.

## What was verified

The test suite builds a synthetic GD-ROM as a GDI (audio track, small data track, data track at LBA
45000 with an ISO9660 filesystem containing IP.BIN, a scrambled fake SH-4 boot binary with literal
pools, a code overlay, a texture and an ADX), converts it to CHD with chdman 0.264, and checks:

- libchdr and chdman backends both return byte-identical tracks to the source GDI, including the
  audio track (chdman stores CD audio big-endian; `dcdisc` swaps it back);
- CHD to GDI conversion round-trips;
- chdman's GD-ROM convention: the gap before LBA 45000 is stored as real zero frames at the end of
  the preceding track, with `FRAMES` counting them and `PAD` saying how many (so track sectors are
  `FRAMES - PAD`);
- descramble/scramble are exact inverses at many sizes;
- the literal-pool pointer ratio separates scrambled from descrambled code (per-instruction
  statistics cannot, because scrambling permutes whole 32-byte slices).

Verified on real discs (2026-09-11, Crazy Taxi and Tony Hawk's Pro Skater 2 CHDs, libchdr 0.3 built
from the submodule on macOS ARM64): tracks and filesystem read correctly, IP.BIN fields and CRC
decode, and **the boot binary on a GD-ROM is stored plain**. Scrambling is a CD-R (MIL-CD) boot
mechanism, so only self-boot CDI images need descrambling; `dcdisc` detects which form it has via the
pointer ratio and never descrambles a plain binary.

`tests/fixtures/synthetic.chd` (150 KB, no game data) is committed so the libchdr path is tested
where chdman is absent.

## Not yet verified on a real disc

The descrambler itself (the permutation constants) has only been exercised on synthetic data,
because no scrambled retail image has been run through it yet. The first CDI-style image will be the
test: `dcdisc inspect` reports the pointer ratio in both forms and a correct descramble shows a high
ratio in the plain form.
