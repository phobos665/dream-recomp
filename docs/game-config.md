# Per-game configuration (`games/<id>/<id>.toml`)

One TOML file per title drives `dream-translate game` (ADR 14). Everything in it is measured from
the owner's disc and reviewable; nothing from the disc itself is committed. `games/crazytaxi/crazytaxi.toml`
is the reference instance.

```toml
[game]
id = "crazytaxi"              # [A-Za-z0-9_-]+; names the output unit and the games/ directory
title = "Crazy Taxi"
region = "USA"                # optional
product = "MK-51035"          # optional, IP.BIN product number

[disc]
sha1_1st_read = "8b5b..."     # optional: SHA-1 of the plain executable the config was measured on
chd_sha1 = "..."              # optional

[binary]
path = "extracted/fs/1ST_READ.BIN"   # relative to this file
load_address = 0x8C010000            # where the BIOS loads it
link_address = 0x0C010000            # what the literal pools assume; defaults to load_address
entry = 0x8C010000                   # first instruction, in load-address spelling

[[relocations]]                      # zero or more regions the program copies and runs elsewhere
source = 0x0C010100                  # link-space address of the first copied byte
size = 0x3F00
dest = 0x8C004000                    # where the copy executes
entry = 0x8C004000                   # optional seed inside the copy (or `entries = [...]`)
no_fold = false                      # true: the copy is a template the program patches; read every literal at run time
overlay = false                      # true: other code occupies `dest` at other times (see Overlays)

[functions]
extra = [0x0C08538C]                 # additional discovery seeds (load or link spelling)
exclude = [0x0C08F4EA]               # entries never emitted
symbols = "symbols.tsv"              # names file, see below
pointers = true                      # follow literal-pool pointers (default true)
sweep = true                         # linear sweep of gaps (default true)

[hle]                                # guest address -> runtime handler (Phase 2/3)
"0x0C080684" = "libc.bfslu"

[hooks]                              # guest address -> hook name (Phase 3)
"0x0C010000" = "boot"
```

Rules the loader enforces: `game.id`, `game.title`, `binary.path`, `binary.load_address` and
`binary.entry` are required; `load_address` and `link_address` must alias the same RAM
(`addr & 0x1FFFFFFF` equal); addresses are even; `hle`/`hooks` keys are addresses written as strings.
Errors name the field.

## What `dream-translate game` does

```sh
dream-translate game --config games/crazytaxi/crazytaxi.toml --out-dir build/ct
```

1. Loads the binary at `link_address`.
2. Runs discovery seeded with `entry` and `functions.extra` (translated into link space), with the
   pointer and sweep passes as configured; drops `functions.exclude`.
3. Names functions from `functions.symbols` (see below), then emits `<id>.cpp`/`.h`, a
   `<id>.functions.json` with names and origins, and `<id>.report.txt` with counts and every
   emitter note.
4. For each `[[relocations]]` entry, views the copied bytes at `dest` and repeats 2 and 3 into
   `<id>_reloc_<dest>.*`. Crazy Taxi's startup stub (31 functions, 2,205 instructions) translates
   this way with nothing left unlowered.

## Overlays

A RAM address can hold different code over a program's life. Crazy Taxi's interrupt entry at
VBR+0x600 (0x8C00FA00) first receives a small pair of stubs from the startup code that hand the
first task to the main program, then Katana's runtime installs its own, taken on every interrupt.
Describe each as its own `[[relocations]]` entry with `overlay = true` (they may share `dest`).
The emitter then records up to 32 instruction words of each function in its table entry, and the
runtime's `find_function` only dispatches to a translation while guest memory at that address
still holds those words; when none match, the address is untranslated (dev interpreter, or the
release fault). Signatures cover the function's own extent, never its literal pool, so patched
`no_fold` copies still match. Later entries for a repeated `dest` are emitted as
`<id>_reloc_<dest>_2`, `_3`, ... with function names suffixed to keep them distinct; the game's
CMakeLists derives the same unit names from the TOML.

## Symbols file (`symbols.tsv`)

Tab-separated `address<TAB>name<TAB>source`, `#` comments, hand-editable. Produce it from a Ghidra
export (`tools/ghidra/ExportFunctions.java` after the FID pass):

```sh
dream-translate symbols --ghidra games/crazytaxi/cache/functions_ghidra_fid.json \
    --out games/crazytaxi/symbols.tsv          # merges into an existing file
```

Ghidra's default `FUN_*`/`thunk_*` names are skipped. `FID_conflict:` names (several candidates)
are skipped unless `--keep-conflicts`, since a wrong name is worse than none. When applied, names
become C++ identifiers (leading underscores dropped, other characters to `_`, keywords and
digit-first names prefixed `f_`); two functions with the same name get the address appended to the
second (`kmSetFogTable`, `kmSetFogTable_0c075b50`). Aliases match: a symbol at 0x8C010000 names the
function emitted at 0x0C010000. Crazy Taxi: 365 symbols, 355 functions named.
