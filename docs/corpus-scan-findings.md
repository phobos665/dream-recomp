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

## Measured, once `discover` reported it

The instrumentation landed and every inference above about *which* sites are unresolved turned out
to be wrong. Measured across the corpus, by the translator rather than by decoding from outside:

| Title | unresolved | JSR | JMP | BRAF | BSRF |
| --- | ---: | ---: | ---: | ---: | ---: |
| Street Fighter III | 3744 | 3513 | 226 | 1 | 4 |
| Dino Crisis | 3688 | 3015 | 665 | 1 | 7 |
| techromancer | 2585 | 2452 | 128 | 0 | 5 |
| Tony Hawk's Pro Skater 2 | 2511 | 2265 | 90 | **156** | 0 |
| Rayman 2 | 2356 | 2263 | 91 | 0 | 2 |
| Crazy Taxi | 2228 | 2145 | 76 | 2 | 5 |
| AeroWings | 1445 | 1355 | 87 | 0 | 3 |
| Metropolis Street Racer | 790 | 715 | 75 | 0 | 0 |
| **Total** | **19347** | **17723** | **1438** | **160** | **26** |

**`JSR` through a register constant propagation cannot trace is 91% of every unresolved site**, and
`no_constant` is the reason for all but a handful. Everything else is a rounding error beside it.

Three specific corrections to the section above, all in the same direction -- inference from outside
the translator was confidently wrong:

- **`BRAF` is 160 sites, not 351**, and 156 of those are Tony Hawk's alone. Adding the constant
  fallback `JMP` has would fix **two sites** in Crazy Taxi. It was proposed here as "the single
  clearest gap the corpus shows" and it is worth almost nothing.
- **`BSRF` is 26 sites, not 2048.** Constant propagation handles it comfortably. The earlier figure
  came from measuring against `switch_sites`, which `BSRF` can never appear in.
- The per-title `BRAF` resolution spread, and the conclusion drawn from it about toolchains
  predicting discovery effort, was measuring noise.

## What actually defines those registers

`tools/corpus/classify_jsr.py` walks back from each unresolved `JSR` to whatever last wrote the
register it calls through. The answer depends sharply on how far back it looks, and that dependence
is the finding:

| search window | none | mem_load | pc_literal |
| ---: | ---: | ---: | ---: |
| 24 instructions | 62% | 25% | 9% |
| 64 | 42% | 26% | 26% |
| 200 | 17% | 28% | **46%** |

`mem_load` barely moves. Those are pointers read out of a struct or a vtable, roughly 28% of the
total and genuinely dynamic -- no static pass resolves them, and they are already handled at run
time by `call_indirect`. Nothing to win there.

What grows is `pc_literal`: a pointer loaded from a literal pool, which is **statically knowable**.
At a 200-instruction window that is 8,211 sites, 46% of every unresolved indirect call in the
corpus.

## The cause: constant propagation stops at the basic block

`discover.cpp:88-96` says it plainly -- *"the constant `reg` holds at `pc` when built inside the
block"* -- and `block_start` is reset at the top of every block. A function that loads a callback
into a register in one block and calls it in a later one is invisible to it, however obvious the
constant is.

That is the whole explanation for the window sweep above. The classifier walks back linearly and
does not know where blocks begin, so the further it looks the more definitions it finds that
discovery, confined to one block, cannot.

**So the largest discovery gap is our own analysis being block-local, not a missing pattern.**
Extending constant propagation to follow the control-flow graph -- at minimum to dominating blocks
-- addresses a class an order of magnitude larger than anything in
`dreamcastrecompiled-review.md`.

**Treat 46% as an upper bound.** This classifier has no CFG: a definition it finds 150 instructions
back may sit on a path that never reaches the call, and a proper dominator-based propagation would
reject it. The recoverable share is somewhere between the 9% discovery manages today and that 46%,
and the way to find out is to build it and re-run this scan -- which is now a measurement rather
than an argument.

## Revised order

1. ~~Report unresolved indirect sites~~ -- done, and it rewrote everything below it.
2. **Carry constants across basic blocks** -- the single biggest item by a wide margin. Up to 8,211
   sites, 46% of all unresolved indirect calls, and it is our own analysis to extend rather than a
   pattern to port. Measure the real recovery by re-running this scan afterwards.
   `dreamcastrecompiled-review.md`'s branch-selected-literal heuristic is a special case of the
   same problem -- two literals reaching one call through different predecessors -- and is worth
   doing only if CFG-aware propagation does not already subsume it, which it probably does.
3. **`JMP`**, 1,438 sites, 7%. The SDK veneer patterns are the relevant heuristic.
4. **`BRAF`**, 160 sites. Almost entirely Tony Hawk's, so worth doing when that title matters and
   not before. The byte jump table is the relevant heuristic, and it is the one this project would
   have ported first on the strength of the review alone.

The order the review guessed was exactly backwards. That is the argument for instrumenting before
porting, and it cost a day.
