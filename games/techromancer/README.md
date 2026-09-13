# Tech Romancer: a second title

Not a port target. This is a measurement of how much of the toolchain is general and how much was
fitted, without anyone noticing, to Crazy Taxi.

The rule for this directory: **nothing here may be Crazy Taxi-shaped.** The config below contains
only facts read off the disc by `dcdisc inspect`. There are no symbols, no relocations, no hooks and
no per-title code anywhere in the runtime. What breaks with only that much is the result.

## What generalised with no changes

The disc tooling read the image, parsed the filesystem and passed the checklist: one code file, a
2.5 MB boot binary stored plain, no Windows CE, and the same middleware as Crazy Taxi (Kamui,
Katana, Manatee, Ninja, Shinobi and the same audio codec). The translator then took a twenty-line
config and produced a build.

| | Crazy Taxi | Tech Romancer |
|---|---|---|
| Boot binary | 1.4 MB | 2.5 MB |
| Functions discovered | 3,884 | 11,478 |
| Symbols available | 361 | none |
| Instructions lifted | 263,563 | 822,775 |
| Not lowered | 187 | 870 |
| Decode coverage | 78.9% | 86.2% |

Nothing that failed to lower is a missing opcode: they are zero words sitting in data.

It boots. 1,800 frames without a fault, reading the disc and rendering, and with `--press` to get
past the screens that wait for a button it reaches its options screen, which draws correctly:
gradients, transparency, text and the logo.

## What did not generalise, and was fixed

The build **hard-required a symbol table**. A title with no Ghidra database could not build at all,
even though the translator is content to name functions by address. The dependency is now
conditional, in both games' build files. This is exactly the kind of assumption a second title
exists to find: invisible while only one game had ever been built.

## The number that was wrong, and what it is really

The first version of this note reported decode coverage of 64.2% for Crazy Taxi and 63.9% here, and
concluded from how close they were that the missing third was one systematic defect in every title.

**That was an artefact of a broken metric.** It summed each function's range independently, but
function ranges overlap: a caller is given a range covering callees it falls into, and those callees
are separately emitted functions. Code compiled under one name was counted as missing from another.
Checking a specific "gap" here found the bytes emitted perfectly well as `fn_0c0100aa`,
`fn_0c010112` and `fn_0c0101f0`, all of which discovery had already found as call targets.

Measured over the union of intervals, and with a third title added to check:

| Binary | Coverage | Genuinely missing |
|---|---|---|
| Crazy Taxi | 78.9% | 118,480 bytes |
| Tech Romancer | 86.2% | 317,050 bytes |
| Metropolis Street Racer, loader | 90.2% | 14,080 bytes |
| Metropolis Street Racer, game module | 88.8% | 45,638 bytes |
| Metropolis Street Racer, data module | 85.8% | 718,572 bytes |

No coincidence, because the coincidence was the bug. Five binaries from three publishers spread
between 79% and 90%, and Crazy Taxi is the worst of them rather than typical.

So discovery is doing considerably better than the first note claimed, and the function-boundary
work is not "the same defect in every title". Function ranges really do overlap, and that is worth
fixing because it makes the sweep reject genuine functions inside a higher-ranked range. But it is
not the structural hole it was reported to be.

## Reproducing

```
python3 -m dcdisc inspect games/techromancer.chd -o /tmp/report.md
python3 -m dcdisc extract games/techromancer.chd games/techromancer/extracted
cmake --build build/dream-dev --target techromancer_boot -j8
build/dream-dev/games/techromancer/techromancer_boot \
  --config games/techromancer/techromancer.toml --window \
  --press start@300,start@600,start@900,a@1200,start@1500
```

The disc image and everything extracted from it are gitignored and never committed.
