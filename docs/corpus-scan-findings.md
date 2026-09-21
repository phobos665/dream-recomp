# What a corpus scan found

Run 2026-09-21 over the owner's ten discs with `tools/corpus/scan.py`, before porting any heuristic
from `DreamcastRecompiled`. The point was to find out which gaps we actually have rather than which
patterns another project happens to have written, and it changed the answer.

## The corpus

Eight of ten discs yielded a boot binary. Coverage is what discovery claims of the image, after
merging overlapping function ranges.

| Title | functions | claimed | holes >=32B | hole KB |
| --- | ---: | ---: | ---: | ---: |
| Street Fighter III 3rd Strike | 14893 | 12.2% | 1789 | 12266 |
| AeroWings | 5329 | 16.6% | 809 | 3394 |
| Tony Hawk's Pro Skater 2 | 6983 | 34.5% | 1993 | 2206 |
| Crazy Taxi | 3884 | 42.4% | 597 | 819 |
| Metropolis Street Racer | 2652 | 52.3% | 334 | 228 |
| techromancer | 11478 | 65.0% | 1442 | 889 |
| Dino Crisis | 8655 | 74.1% | 1222 | 423 |
| Rayman 2 | 5021 | 80.6% | 1156 | 172 |

Coverage alone says little: Street Fighter III is a 2D fighter and most of that unclaimed 12 MB
will be sprite data, not missed code. The number worth acting on is the next one.

## The finding: BRAF has no fallback

`discover.cpp:167-186` gives `JMP` and `BRAF` the same first chance -- `recover_switch`, which
handles long jump tables and the `and/shll2/braf` code-table idiom. When that fails, **`JMP` falls
back to constant propagation and `BRAF` falls back to nothing at all.** A `BRAF` whose target
constant propagation could establish is simply dropped.

Measured across the corpus, counting `BRAF` sites inside discovered code that never became a
switch site:

| Title | BRAF | unresolved | |
| --- | ---: | ---: | --- |
| AeroWings | 106 | 89 | 83% |
| Dino Crisis | 69 | 46 | 66% |
| Tony Hawk's Pro Skater 2 | 269 | 159 | 59% |
| Street Fighter III | 66 | 31 | 46% |
| Metropolis Street Racer | 3 | 1 | 33% |
| Crazy Taxi | 46 | 12 | 26% |
| techromancer | 50 | 11 | 22% |
| Rayman 2 | 33 | 2 | 6% |
| **Total** | **642** | **351** | **55%** |

Adding the same constant-propagation fallback `JMP` already has is a handful of lines, and it is
the single clearest gap the corpus shows. It should be measured after, not assumed: some of those
351 are genuine switches our table recovery does not recognise, and those need the table work
rather than a constant.

The spread is informative on its own. Rayman 2 resolves 94% of its computed jumps and AeroWings
17%, which is the difference between toolchains rather than between our efforts -- so a title's
compiler predicts how much discovery work it will need.

## Two discs have no 1ST_READ.BIN

**Charge 'N Blast** and **Record of Lodoss War** boot something else, and `dcdisc` assumes
`1ST_READ.BIN`. Those titles cannot reach step 1 of `per-game-bring-up.md` today. Lodoss War is one
of the four titles `DreamcastRecompiled` supports and their notes mention secondary-executable
work, so this is a known shape rather than a broken disc. Worth handling before anyone picks a
title and finds the tool cannot open it.

## What the scan cannot tell us, and the fix

**Resolution cannot be measured from outside.** `discover` reports the functions it found and the
switch sites it recovered; it does not report the indirect sites it gave up on. Everything above is
inferred by decoding the image and subtracting, which is why this document went through three wrong
numbers before this one:

- scanning the whole image counted data that looks like opcodes -- 11,632 false indirect sites on
  Crazy Taxi against a few hundred real;
- overlapping function ranges double-counted instructions, and separately put Rayman 2's coverage
  at 101.5%;
- `BSRF` was measured against `switch_sites`, which it can never appear in, producing a confident
  and meaningless "2048 of 2048 unresolved".

`BSRF` resolution is therefore still unknown. It is handled at `discover.cpp:188` through constant
propagation, and there is no way from the outside to count how often that succeeds.

**So the first improvement is instrumentation, not a heuristic.** `discover` should emit an
`unresolved_indirect` list: every `JMP`/`JSR`/`BRAF`/`BSRF` site inside discovered code whose target
it could not establish, with the opcode and the containing function. That turns this scan from
inference into measurement, makes every later heuristic gradable, and is perhaps a day's work.

## Revised order

1. **Report unresolved indirect sites from `discover`** -- 1 day. Everything else is guesswork
   without it.
2. **Re-run the scan** and rank the real gaps.
3. **The `BRAF` constant-propagation fallback** -- small, and the clearest gap visible today.
4. Then the ported heuristics from `dreamcastrecompiled-review.md`, in whatever order step 2 says,
   rather than the order that review guessed.
