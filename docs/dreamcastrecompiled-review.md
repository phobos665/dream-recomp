# Review: Duw0ng/DreamcastRecompiled

Read on 2026-09-21 at commit `ba7df34`, to see what is worth taking. Short answer: **the discovery
heuristics and the CDI reader, and nothing else.** Both are genuinely ahead of us in their areas,
and the rest of the project is behind ours in ways that make adopting it a downgrade.

## What it is

An MIT-licensed Dreamcast recompiler, public repository created 2026-09-20, nine commits, one star.
It consolidates a longer private `0.0.x` line -- the code refers to versions up to `0.0.210` -- so
it is much more mature than the commit count suggests.

Its self-reported compatibility is four titles: ChuChu Rocket! green throughout, and Crazy Taxi 2,
Daytona USA 2001 and Record of Lodoss War green to the menus with gameplay marked partial. There is
no differential harness or reproducible gate behind those marks, so they mean "it looked right when
run", which is a weaker claim than the one this project makes about Crazy Taxi. It is still four
titles further than one, and that is worth respecting.

**Licence: MIT.** Compatible with our GPL-2.0 in the direction we need -- we may take their code
into this repository provided their copyright notice travels with it. They are scrupulous in
`THIRD_PARTY_NOTICES.md` about Flycast being GPLv2 and not copied into their tree, which is a good
sign about the provenance of the rest.

## Why we should not adopt the architecture

| | DreamcastRecompiled | Here |
| --- | --- | --- |
| Source | 33k lines, **22k of them in one file** | ~25k across libraries |
| Runtime | **emitted as string literals** from `src/codegen/cpp_emitter.cpp` | `dream::runtime`, `dream::render`, `dream::audio` |
| Renderer | D3D11, Windows only | Vulkan on macOS, Linux, Windows |
| Correctness | 23 test files, no oracle | 139 unit tests, golden traces, Flycast oracle, per-frame write hash |
| Disc input | CDI | GDI and CHD |

The runtime is the decisive difference. `cpp_emitter.cpp` contains 3,386 references to PVR, 467 to
Maple and 145 to D3D11 -- not as a library the emitted code links against, but as text the emitter
writes into its output. That runtime therefore cannot be unit-tested, cannot be reviewed as code,
and cannot be reused between titles except by regenerating it. Ours is a library with tests because
that is what lets a change to the PVR be proven not to break Maple.

Their compatibility lead comes from breadth of *effort*, not from a better structure, and the
structure is why they have 3,536 lines of tests where we have a differential oracle.

## Worth taking: three discovery heuristics

`src/common/function_analysis.cpp` (2,153 lines) is the best part of the project and it lands
exactly on our weakest one. Our `translator/src/analysis/switch.cpp` already resolves long jump
tables, the `and/shll2/braf` code-table idiom and mova-established constants. These three it does
not.

### 1. GCC's signed byte jump table -- 2 days

```
cmp/hi #N,index ; bt default ; mova table,r0
mov.b @(r0,index),index ; braf index ; <signed byte table>
```

Each table byte is an offset from BRAF's architectural PC+4. Their implementation handles three
things worth having: it does not require the instructions to be adjacent, it treats a following
`EXTU.B` as deliberately selecting *unsigned* offsets rather than signed, and it recovers the table
length from the `CMP/HI` bound and the `MOV #N` feeding it. We have no `mov.b` handling in
`switch.cpp` at all.

Crazy Taxi is SHC-compiled so this is not its idiom, but it is GCC's, which means every KallistiOS
homebrew title and anything built with the GNU toolchain.

### 2. Branch-selected indirect call targets -- 2 days

```
mov.l callback_a,r3
bf/s .join
 <delay slot>
mov.l callback_b,r3
.join:
jsr @r3
```

Straight-line constant propagation resolves `callback_b` and cannot represent `callback_a`, so one
of the two callees is never discovered. Our `constprop.cpp` has exactly this limitation. Their fix
records the alternate as a dynamic branch reference with both targets, which is the right shape.

### 3. Katana SDK veneers -- 1 to 2 days, measure first

`mov.l @(disp,PC),Rn ; jmp @Rn ; <delay slot>`, plus a two-target selector variant and a method
veneer they attribute to ChuChu Rocket. Our constant propagation may already cover the simple form;
the selector variant it will not. **Measure what we already resolve before writing any of this** --
the point of the exercise is the addresses we currently miss, not the patterns they happen to name.

### Why this matters here specifically

Their comment on the BRAF case is the same defect class this project hit last week: *"returning to
the host on an in-function BRAF silently skips epilogues and corrupts callee-saved state."* That is
our `resume_at` problem described from the other side, and it is independent confirmation that
in-function entry points are where a Dreamcast recompiler bleeds.

**How to take them.** Port deliberately, one at a time, with attribution in the file header and
their MIT notice preserved. Each is independently testable: add the pattern, translate Crazy Taxi,
and compare the write hash. A heuristic that finds real code changes nothing about behaviour; one
that misfires corrupts a function and the hash says so immediately. Do not take the file wholesale.

## Worth taking: a CDI reader -- 3 days

`src/disc/main.cpp` has a compact, readable CDI parser: track marks, sector-size identifiers, track
modes, and the metadata header CDI keeps at the *end* of the file. `dcdisc` has `gdi.py` and
`chd.py` and no CDI at all, so this is purely additive -- and CDI is the format most Dreamcast
images circulate in, which matters for anyone bringing up a title who does not have a CHD.

It is C++ and `dcdisc` is Python, so this is a reimplementation against their code as the
specification rather than a port. Budget accordingly, and write it against the synthetic-image
tests `dcdisc` already uses so it needs no disc to test.

Note the interaction with something already known here: `dcdisc` detects scrambled boot binaries by
the literal-pool pointer ratio, and the note in `baseline-game.md` is that CDI-style images are the
scrambled ones. A CDI reader makes that path reachable rather than theoretical.

## Worth learning, not taking

- **Breadth beats depth for finding discovery gaps.** They have four titles part-working and we have
  one nearly working, and their heuristics are better precisely because four binaries showed them
  four sets of idioms. `dc_corpus_scan` exists to run analysis across a corpus. We should have an
  equivalent long before we have four ports -- running discovery over several binaries and counting
  unresolved targets per title costs little and would have found all three heuristics above.
- **Their compatibility table is honest about partial state**, with a per-subsystem breakdown rather
  than one verdict per game. `docs/progress.md` would read better in that shape.

## Not worth taking

The emitter architecture, the D3D11 backend, the embedded runtime, the Windows batch-file workflow,
and `build-v011/` -- 508 committed build artefacts including object files, which is 55 MB of the
repository's 7.9 MB checkout size and presumably an oversight.

## Total

**7 to 9 engineer-days** for all four items, none of which touches floating-point behaviour, so ADR
16's cross-ISA golden traces are unaffected. Sequence: measure what we already resolve, then the
byte table, then branch-selected calls, then CDI, then veneers if they are still missing anything.
