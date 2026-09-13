# Contributing

Thanks for looking. Before anything else, three rules that keep this project legal and reproducible.

1. **No game data, BIOS, flash, VMU images, SDK files or FID databases in the repository, ever.**
   Not in tests, not in fixtures, not in comments. Users supply their own dumps; tools read them from
   gitignored locations and skip cleanly when they are absent. The synthetic disc in
   `tools/dcdisc/tests/fixtures` is generated, contains no game content, and is the model for any
   fixture you add.
2. **No Sega SDK source or header text.** Describe interfaces in your own words or cite the
   KallistiOS equivalent (BSD-licensed).
3. **Everything is GPL-2.0** (see `LICENSE`). Porting from Flycast is expected and welcome; say
   where a piece came from in a comment. Third-party code under `third_party/` keeps its own licence
   (libchdr BSD-3, doctest MIT).

## Working in the repo

- Read `docs/progress.md` first, then `docs/implementation-plan.md` for the work package you are
  picking up. Decisions in `docs/decisions/README.md` are followed unless you open a new ADR.
- Build: `cmake -S . -B build && cmake --build build --parallel && ctest --test-dir build`.
  The build needs the `third_party/libchdr` submodule (`git submodule update --init --recursive`).
- Python tooling: `pip install -e "tools/dcdisc[test]"`, tests with `pytest` in `tools/dcdisc`.
- Formatting is `clang-format` (config at the root); CI checks it.
- Every change to the translator's emitter comes with a differential test.
- Commit messages: imperative summary under 72 characters, a body that says why, and a `WP x.y`
  reference when the change advances a work package.
- Release builds never contain an SH-4 interpreter. `DREAM_DEV_INTERPRETER` is a development aid and
  CI builds both configurations so the fallback never becomes load-bearing.
