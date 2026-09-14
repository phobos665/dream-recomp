# Bringing up a new title

How to take a Dreamcast disc you own and get it running, from `dcdisc new-game` to a release build.
Written for someone who has not worked on this repository before.

The tool is general; a title is not. Everything in `translator/` and `runtime/` serves every game,
and everything in `games/<id>/` is one game's measured facts: where it copies code, which entries
discovery cannot find, what its functions are called. Bringing up a title is producing that second
half, and this document is the order to do it in and the instrument to reach for at each step.

Expect this to take days, not hours, and expect most of it to be reading disassembly. The tooling
tells you *where* something went wrong quickly; deciding *what to do about it* is the work.

## Before you start

You need the disc. Nothing in this repository ships game data and nothing you produce should commit
any: `games/*/disc/`, `games/*/extracted/` and `*.chd` are all ignored, and the per-game config you
write is the only thing that belongs in version control.

```
python3 -m dcdisc doctor          # checks this machine can build and run
cmake -S . -B build && cmake --build build --parallel
```

`build/` is a development build. **Work in one.** It carries an SH-4 interpreter that runs anything
the translator missed, which is what lets a half-configured title run at all; a release build stops
at the first thing it cannot translate. You will make a release build at the end, as the exit exam.

## Step 1: make the project

```
python3 -m dcdisc new-game "games/Your Game (USA).chd"
```

This extracts the disc, reads IP.BIN, and writes `games/<id>/<id>.toml` and a `CMakeLists.txt`. It
records what can be read off the disc -- the title, the region, the product code, the load and link
addresses -- and nothing else. What it does not write is everything in the rest of this document.

Reconfigure so CMake notices the new directory, then build the launcher:

```
cmake -S . -B build && cmake --build build --target <id>_boot --parallel
```

## Step 2: the first run, which will not get far

```
./build/games/<id>/<id>_boot --config games/<id>/<id>.toml --no-audio --max-seconds 30
```

A title with a stub config typically stops within a handful of frames. Two titles in this repository
sit at frame 0 and frame 16 for exactly this reason. The line that matters is the last one:

```
fault: untranslated call target 0x8c126a20 from pc 0x0c168da0
```

**Read the address.** It tells you which problem you have, and they need opposite treatments:

| Address looks like | What it means | Go to |
| --- | --- | --- |
| `0x8C…` or `0x8c00…` -- RAM, not the disc image | the title copied code somewhere and ran it | Step 3 |
| `0x0C…` -- inside the image | discovery did not reach it | Step 4 |
| `cannot resume at …` | a non-local return, see Step 6 | Step 6 |

## Step 3: relocations, the first real barrier

A Dreamcast title does not just run from where the BIOS loaded it. A Katana program has a loader
that copies a startup stub somewhere and jumps to it, and the runtime installs interrupt handlers
into the vector area at `0x8C00F400` and `0x8C00FA00`. None of that code is at the address the
translator saw it at, so the translator has to be told: take these bytes from here, and translate
them as if they lived there.

```toml
[[relocations]]
source = 0x0C010100     # where the bytes are in the image
size   = 0x3F00
dest   = 0x8C004000     # where the program runs them
entries = [0x8C004000, 0x8C004070]
```

An address the vector area holds at different times over a run -- and it will, because the startup
stub installs one handler and the runtime later replaces it -- needs `overlay = true` and
`no_fold = true` as well. The runtime then keeps every translation of that address and picks
whichever matches the bytes currently in RAM, by comparing a signature. This is why
`find_function` reads guest memory, and why anything caching it has to respect that.

**You do not have to find these by hand.** Run with `--suggest-config` and the launcher writes the
TOML it earned -- the call targets discovery never reached, and the regions the program copied and
ran elsewhere:

```
./build/games/<id>/<id>_boot --config games/<id>/<id>.toml --no-audio \
    --max-seconds 30 --suggest-config /tmp/suggested.toml
```

Read what it produces before pasting it. It is evidence, not a decision -- Step 4 is the judgement
that has to be applied to it.

## Step 4: discovery, and the one rule that matters most

Turn discovery on in the config:

```toml
[functions]
pointers = true
sweep = true
extra = []
```

Then deal with what a run still reports as untranslated. **Every address falls into one of two
kinds, they look identical in a run report, and treating one as the other silently corrupts the
game.**

**A hole.** The address is in the image, but no translated function contains it. It is a genuine
entry point the program reaches through a register, so no static pass can see it. Seed it:
`extra = [0x0C081476]`.

**A mid-function address.** The address is *inside* a function that is already translated. The
runtime resolves this itself and you must leave it alone. Seeding it makes it a discovery seed,
which gives it top rank, makes the enclosing function's branch to it foreign, and **truncates that
function** -- which then computes garbage. Measured on Crazy Taxi: 152 texture decode failures
against none, and 138 frames drawn against 330.

To tell them apart, look at the function boundaries the translator produced:

```python
import json
d = json.load(open("build/games/<id>/gen/<id>.functions.json"))
fns = d if isinstance(d, list) else d["functions"]
rows = sorted((int(f["entry"], 16) if isinstance(f["entry"], str) else f["entry"],
               int(f["end"], 16) if isinstance(f["end"], str) else f["end"]) for f in fns)
t = 0x0C081476
print([r for r in rows if r[0] <= t < r[1]] or "NOT in any function -- a hole, seed it")
```

Then confirm with the disassembler, because the boundary alone can mislead:

```
./build/translator/dream-translate disasm --image games/<id>/extracted/fs/1ST_READ.BIN \
    --base 0x0C010000 --start 0x0C081470 --count 12
```

A hole begins cleanly after an `rts` and its delay slot. A mid-function address lands in the middle
of a computation.

## Step 5: symbols, which are optional and worth it

`symbols = "symbols.tsv"` names the functions in reports and disassembly. Names come from a Ghidra
Function ID pass over a Katana SDK (`tools/ghidra/`); generate them locally and never commit the
FID databases. The file itself -- address, name, source -- is yours to commit and to hand-edit as
you learn what things are.

Debugging without them is possible and unpleasant. `_kmSetFogTable + 0x6a80` is not a real
attribution, it is the nearest preceding symbol in a sparse table, and it will mislead you.

## Step 6: when it runs but goes wrong

A title that boots and then misbehaves -- garbage geometry, a jump to a nonsense address, an
unmapped read -- has a bad translation somewhere. Four instruments, in this order.

**Stop at the first fault rather than the thousandth symptom.** The first unmapped access is where
the run started going wrong; everything after it is the guest following garbage.

```
DREAM_STOP_ON_UNMAPPED=1 ./build/games/<id>/<id>_boot --config … --max-seconds 150
```

You get the faulting pc, the return address, and every register. **A float bit pattern in an address
register** -- something like `0x420187ac`, which is 32.4 as a float -- means a register that should
hold a pointer holds a number, and that is a wrong translation rather than a missing one.

**Ask whether it is the translation at all.** If the interpreter runs the same scenario correctly,
the defect is in emitted code:

```
./build/games/<id>/<id>_boot --config … --interpret --max-seconds 150
```

**Bisect to one function.** `DREAM_INTERP_FUNCS` hides specific functions so the interpreter runs
them. Start with the neighbourhood the fault named, then shrink the set until putting any one member
back brings the fault:

```
DREAM_INTERP_FUNCS=0x0c07b760:0x0c07c478 ./build/games/<id>/<id>_boot --config …
```

**Then decide.** A function whose translation is wrong can be quarantined in the config, which
leaves it to the interpreter:

```toml
exclude = [0x0C07B760]
```

Record *why* in a comment, with the evidence. **A quarantine is not a repair.** The emitter still
mistranslates whatever construct that function uses, it will mistranslate it in every other title
that uses it, and a release build cannot run the function at all. Raise it against the translator
with the differential harness (`docs/differential-harness.md`) and take the line out when it is
fixed.

## Step 7: the regression gate

Before changing anything else, get a gate that tells you whether you broke the emulation. Every
guest write is hashed, per frame, so an identical hash means the run did bit-for-bit the same thing.

```
./build/games/<id>/<id>_boot --config … --no-audio --rtc-seed 1 --max-seconds 40 \
    --write-hash /tmp/before.hash --report /tmp/before.report
```

**`--rtc-seed` is not optional.** Without it the console clock comes from the host date, two runs of
the same build diverge at about frame 24, and nothing is comparable. Strip the timing lines out of
the report before diffing it, since those differ run to run by design.

Run two scenarios -- one untouched, one driven with `--press` into actual play -- because they
exercise very different code. On Crazy Taxi, attract mode is dominated by the memory subsystem and
gameplay by the title's own translated functions; a change can be free in one and costly in the
other.

Note what the hash does *not* cover: it is guest writes, so it says nothing about rendering. For
that, `--screenshot-presented --screenshot-at N` captures what actually reached the screen, and the
`textures N decoded, N failed` counter in the run report is a cheap proxy for a truncation
regression.

## Step 8: the release build, which is the exit exam

```
cmake -S . -B build-release -DDREAM_DEV_INTERPRETER=OFF
cmake --build build-release --target <id>_boot --parallel
```

There is no interpreter here, so everything the development build was absorbing becomes a hard stop.
That is the point: it is the only honest measure of how complete the configuration is. A release
build that runs is a title that is genuinely translated rather than one being carried.

Expect it to fault at first. Take each fault back through Step 4 or Step 6. And compare the write
hash against the development build -- they should be identical, and if they are not, one of them is
wrong.

## The instruments, in one place

| You want to know | Use |
| --- | --- |
| what the disc holds | `dcdisc info`, `ls`, `inspect` |
| what the config should say | `--suggest-config FILE` |
| where a run started going wrong | `DREAM_STOP_ON_UNMAPPED=1` |
| whether it is the emitter | `--interpret` |
| which function is at fault | `DREAM_INTERP_FUNCS=lo:hi` |
| whether a change altered behaviour | `--write-hash` with `--rtc-seed` |
| what actually reached the screen | `--screenshot-presented` |
| what an address decodes to | `dream-translate disasm` |
| where functions begin and end | `build/games/<id>/gen/<id>.functions.json` |
| whether translated and interpreted agree | `--replay` -- **see the warning below** |

**`--replay` reports success on a build with no hooks in it.** The emitter only plants them when the
translator runs with `--replay-hooks`, which is not the default. On an ordinary build it prints
`0 calls compared, 0 disagreements`, which reads as a pass and is not one. Build the tree with
`-DDREAM_TRANSLATE_EXTRA_FLAGS=--replay-hooks` before believing it.

## A worked example

Crazy Taxi, 2026-09-14, from a stub config to a working release build:

1. The config had been lost, leaving a `dcdisc new-game` stub. Restoring five relocations, 366
   symbols and the discovery settings took attract mode from 42,575 untranslated call targets to 0.
2. Gameplay then faulted at frame 6093 with `unmapped read16 of 0x420187ac` -- a float used as a
   pointer. `--interpret` completed the same run cleanly, so the defect was in emitted code.
   `DREAM_INTERP_FUNCS` bisected it to one function, `0x0c07b760..0x0c07c478`, which was quarantined
   with a comment and an open item against the emitter.
3. `0x0c081476` was reported as untranslated. The function list showed it in no function, and the
   disassembly showed a clean block start after an `rts` -- a hole. Seeded, and the fault went.
4. Four addresses at `0x0C161xxx` were reached by non-local returns rather than calls, which
   `resume_at` cannot enter unless they are block starts. Seeded, since they translate correctly.
5. The release build then ran attract mode, with a write hash identical to the development build.
   Gameplay still faults at `0x0c081e56` -- a non-local return to a non-block-start, which is a tool
   limitation rather than anything this title can fix in its config.

Point 5 is the honest ending. Some of what you hit will not be yours to fix, and the useful thing to
do with it is report it against the tool with the address, the disassembly and the reproduction.
