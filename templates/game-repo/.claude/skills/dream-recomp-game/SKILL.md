---
name: dream-recomp-game
description: Project knowledge for a single-title Dreamcast recompilation built on the dream-recomp tool, which is vendored here as a submodule. Use this skill for ANY work in this repository: bringing the title further through its bring-up stages, relocations, discovery seeds and excludes, symbols, reading SH-4 disassembly, diagnosing a fault, deciding whether an address is a missing translation or a wrong one, the regression gate, release builds, per-title enhancements, and reporting defects upstream against the tool. Load it whenever the user says "continue", "what's next", "it crashes", "it stops at", "why is it slow", or mentions the title by name, a guest address, a seed, a relocation, an untranslated target, a write hash, the interpreter, Katana, KallistiOS, Flycast or recompilation, so the work follows the method rather than re-deriving it.
---

# <TITLE> recompilation

> Copy this directory to your game repository as `.claude/skills/dream-recomp-game/`, then replace
> `<TITLE>` and `<id>` throughout with your title and its directory name.

This repository is one game. The recompiler, the runtime and every instrument live in the
`dream-recomp` submodule and serve every title; what is here is this title's measured facts -- where
it copies code, which entries discovery cannot find, what its functions are called, and which of its
functions the emitter gets wrong.

**Read `dream-recomp/docs/per-game-bring-up.md` before doing anything else.** It is the method. This
skill is the part that has to be right every time, plus what this title has learned so far.

## Layout

```
game/<id>.toml        relocations, discovery settings, seeds, excludes, HLE bindings
game/symbols.tsv      address, name, source -- hand-editable, commit it
game/CMakeLists.txt   dream_add_game(<id> TITLE "<TITLE>")
game/slice.cmake      leaf functions for the differential harness
docs/progress.md      the ledger: which stage, what is blocked, what was decided and why
docs/findings/        one file per defect worth remembering
dream-recomp/         the tool, as a submodule pinned to a SHA
```

**Pin the submodule to a SHA, never a branch.** A recompilation's correctness is a property of one
specific translator: change it and every golden trace and write hash moves underneath you. Bump it
deliberately, re-run the gate, and commit the new SHA together with the result.

## Hard rules

- **Never commit anything from the disc.** No extracted files, no `1ST_READ.BIN`, no `.chd` or
  `.gdi`, no BIOS or flash, no VMU images, no save data. The config and the symbol table are the
  only things that belong in version control. Check `git status` before every commit; a `.gitignore`
  is not a substitute for looking.
- **Never paste Sega SDK source or header text anywhere**, including comments. Describe an interface
  in your own words, or cite the KallistiOS equivalent, which is BSD-licensed and safe to read.
- **Work in a development build.** It carries the interpreter that absorbs whatever the translator
  missed. A release build is the exam at the end, not the environment to work in.
- **Never ship a seed or an exclude without running the regression gate.** See below. This is the
  rule most likely to be broken by someone in a hurry, and the one that costs most.

## The gate, which comes before everything

Every guest write is hashed per frame, so an identical hash means the run did bit-for-bit the same
thing.

```
<id>_boot --config game/<id>.toml --no-audio --rtc-seed 1 --max-seconds 40 \
    --write-hash /tmp/after.hash --report /tmp/after.report
```

**`--rtc-seed` is mandatory.** Without it the console clock comes from the host date and two runs of
the same build diverge around frame 24. Strip timing lines from the report before diffing it.

Run **two scenarios** -- one untouched, one driven with `--press` into actual play. They exercise
different code and a change can be free in one and costly in the other.

The hash covers guest writes only. It says nothing about rendering: use
`--screenshot-presented --screenshot-at N` and the `textures N decoded, N failed` counter for that.

## The classification that must not be got wrong

A run reports an address as untranslated. **There are three cases, they look identical in the
report, and the wrong choice corrupts the game silently.**

1. **A hole.** In the image, in no translated function, begins cleanly after an `rts` and its delay
   slot. A real entry the program reaches through a register. **Seed it** in `extra`.
2. **Inside a translated function.** The runtime resolves this itself. **Leave it alone.** Seeding
   it gives it top rank, makes the enclosing function's branch to it foreign, and truncates that
   function, which then computes garbage -- measured elsewhere at 152 texture decode failures
   against none.
3. **Looks like 1, behaves like neither.** No emitted block covers it, but a function's declared
   *range* spans it, so `find_containing` succeeds and the resume still fails. Seeding it moves the
   code from the interpreter into translated code that may disagree. **The only signal is the write
   hash.** If it changes, this is a candidate emitter defect, not a missing seed: leave it
   interpreted and take it to the oracle.

Tell 1 from 2 with the function list, then confirm with the disassembler:

```
python3 -c "import json;d=json.load(open('build/game/gen/<id>.functions.json'));..."
dream-translate disasm --image game/extracted/fs/1ST_READ.BIN --base 0x0C010000 --start ADDR --count 12
```

Tell 3 from 1 only by seeding it and watching the hash. **So set the gate up first.**

## The four instruments

Every defect found bringing up the reference title was found with these.

| Question | Instrument |
| --- | --- |
| Where did the run start going wrong? | `DREAM_STOP_ON_UNMAPPED=1` -- stops at the first unmapped access, not the millionth symptom |
| Is the emitter at fault? | `--interpret` -- if the interpreter runs it correctly, the translation is wrong |
| Which function? | `DREAM_INTERP_FUNCS=lo:hi` -- hide a set, shrink until putting one back brings the fault |
| Did behaviour change? | `--write-hash` with `--rtc-seed` |

Two things that read as success and are not:

- **`--replay` prints `0 calls compared, 0 disagreements` on a build with no hooks in it.** The
  emitter only plants them under `--replay-hooks`. Build with
  `-DDREAM_TRANSLATE_EXTRA_FLAGS=--replay-hooks` before believing it.
- **A float bit pattern in an address register** -- something like `0x420187ac`, which is 32.4 as a
  float -- means a register that should hold a pointer holds a number. That is a wrong translation,
  not a missing one. Do not go looking for a seed.

## Stages

1 builds, 2 boots, 3 draws, 4 title screen, 5 plays, 6 runs in release, 7 is right. Stages 4 and 5
are the bulk, because each fault is a decision rather than a command. Record which stage this title
is at, and what is blocking it, in `docs/progress.md` -- update it in the same commit as the work,
never afterwards.

## When it is the tool's fault

Some of what you hit is not this title's to fix: an emitter defect, a discovery limitation, a
missing runtime feature. Do not work around it silently and do not fork the submodule.

Quarantine it so work can continue -- `exclude = [0x…]` leaves a function to the interpreter -- with
a comment recording the evidence, and raise it upstream with the address, the disassembly, the
write-hash difference and a reproduction. **A quarantine is not a repair:** the emitter still
mistranslates that construct, it will do so in every other title that uses it, and a release build
cannot run the function at all.

## Working a session

1. Read `docs/progress.md`. Say which stage the title is at and what is blocking it.
2. Reproduce the current blocker before changing anything, and capture a baseline hash.
3. Make one change. Run the gate. If the hash moved, understand why before continuing -- a changed
   hash is a fact, and whether it matters is a judgement that has to be made rather than assumed.
4. Update `docs/progress.md` and, if the finding is worth keeping, `docs/findings/`.

Commit messages: an imperative summary under 72 characters, then why. Record what was measured, not
what was intended. Never push to the default branch directly.
