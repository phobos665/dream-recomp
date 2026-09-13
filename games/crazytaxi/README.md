# Crazy Taxi (baseline title)

Per-game project for the Phase 3 baseline (ADR 15, owner's decision 2026-09-10). Nothing from the disc
is committed; see `.gitignore`.

## Local layout (gitignored)

```
games/crazytaxi/disc/       your dump: crazytaxi.chd (80 MB), or a .gdi with its track files
games/crazytaxi/extracted/  output of `dcdisc extract` (IP.BIN, fs/, descrambled boot binary)
games/crazytaxi/cache/      discovery reports and build products
```

## Committed here (eventually)

- `crazytaxi.toml`: the per-game config (`docs/game-config.md`): executable hash, load 0x8C010000 /
  link 0x0C010000, entry, the startup-stub relocation to 0x8C004000, symbols file, HLE/hook tables
- `symbols.tsv`: 365 function names from the Ghidra FID pass (`dream-translate symbols`)
- `slice.cmake`, `oracle-cases.json`: functions from the game run through the differential harness
  when the extracted image is present (`docs/differential-harness.md`)
- `functions.json`: discovered function list with FID names (addresses and names only, no code)
- `checklist-report.md`: result of the disc verification checklist (WP0.4)
- `acceptance.md`: the mode x map x time-setting x result matrix (WP3.4)

## Running the checklist (WP0.4)

```
pip install -e "tools/dcdisc[test]"
dcdisc backends                                  # confirm a CHD backend (libchdr or chdman)
dcdisc inspect games/crazytaxi/disc/crazytaxi.chd -o games/crazytaxi/checklist-report.md
dcdisc extract games/crazytaxi/disc/crazytaxi.chd games/crazytaxi/extracted
```

Then read the report against the nine steps in `docs/baseline-game.md`; steps 8 and 9 are manual in
Flycast. Things to look for on this title specifically: the count and total size of `.ADX`/`.AFS`
files (streamed music and speech), whether any file other than the boot binary scores as SH-4 code,
and which Katana banners the boot binary carries.
