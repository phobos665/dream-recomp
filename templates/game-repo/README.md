# Game repository template

The starting shape for a repository that recompiles one Dreamcast title with `dream-recomp`.

```
your-game-recomp/
  dream-recomp/                 submodule, pinned to a SHA
  game/
    <id>.toml                   relocations, seeds, excludes, symbols
    symbols.tsv
    CMakeLists.txt              dream_add_game(<id> TITLE "…")
    slice.cmake
  docs/
    progress.md                 which stage, what is blocked, what was decided
    findings/                   one file per defect worth remembering
  .claude/skills/dream-recomp-game/SKILL.md
  .gitignore
```

## Setting it up

```
git init your-game-recomp && cd your-game-recomp
git submodule add <dream-recomp url> dream-recomp
cp -r dream-recomp/templates/game-repo/.claude .
cp dream-recomp/templates/game-repo/gitignore-fragment .gitignore
```

Then replace `<TITLE>` and `<id>` through the skill, and follow
`dream-recomp/docs/per-game-bring-up.md` from Step 1.

## Building

```
cmake -S . -B build -DDREAM_DEV_INTERPRETER=ON
cmake --build build --target <id>_boot --parallel
./build/game/<id>_boot --config game/<id>.toml --no-audio --rtc-seed 1 --max-seconds 20
```

`add_subdirectory(dream-recomp)` then `add_subdirectory(game)` is all the top-level `CMakeLists.txt`
needs. The tool resolves its own sources through `DREAM_ROOT` rather than `CMAKE_SOURCE_DIR`, so it
does not mind being a subproject, and it skips its own reference titles when it is not the top-level
project -- otherwise their targets would collide with yours the moment you named a game the same
thing.

Verified on 2026-09-14 by building Crazy Taxi from a repository holding nothing but a config, a
symbol table and a one-line `game/CMakeLists.txt`: 1209 frames, no untranslated targets, no unmapped
accesses.
