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

## One thing to check first

`dream_add_game` resolves `boot_main.cpp` through `CMAKE_SOURCE_DIR`, which is the top-level project
-- your repository, not the submodule. Until that is parameterised upstream, a top-level
`add_subdirectory(dream-recomp)` will not find it. Check whether the submodule you pinned still has
that assumption before building, and raise it upstream if so; it is a small change and it is the
only thing standing between this layout and working out of the box.
