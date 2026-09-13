# Owner tasks

Things only the owner can do, in the order the plan needs them. Everything else is engineering work
that can be picked up from `progress.md`. Tick items off here as they happen.

## Needed for Phase 0 (now)

- [ ] **Delete the four zero-byte CHDs** in `games/` (the copies without "(USA)" in the name, apart
      from `crazytaxi.chd`, which is good). They are failed copies. All of `games/*.chd` is gitignored.
- [x] **Katana SDK supplied:** `Dreamcast Katana SDK 1.0B2/` (InstallShield 3 installer, ignored by
      git) was unpacked with `unshieldv3` into `sdk/katana-1.0B2/` (also ignored). It is the
      September 1998 beta ("Dreamcast SDK Version 0.70a"): `ninja.lib`, `shinobi.lib`, the SHC
      runtime, no Kamui. Useful as a first FID source but too early to name Crazy Taxi's late-1999
      libraries well.
- [x] **Katana SDK R10.1 pulled** (owner approved 2026-09-11): `sdk/kochise-dreamcast-docs/` sparse
      checkout of `Lib/` (Hitachi `.lib`, GNU `.a`, Metrowerks ELF variants of every library, 23 MB),
      `Include/` and `Utl/` (SDK tools incl. `shc.exe`, `libsplit.exe`, `elfcnv.exe`, 250 MB), built
      2000-05-18; symlinked as `sdk/katana-r10.1/`. Crazy Taxi's binary names Shinobi 1.62zr
      (Aug 1999) and Kamui 1.11.0.1 (Jul 1999), so R9 would be a closer match if it is ever
      wanted; the other public copies were:
      - archive.org item `dc_sdks`: "Sega Katana Development Software - Release 9.zip" (636 MB),
        "Release 10.1.zip" (957 MB), "Release 11b.zip" (532 MB); also CodeWarrior for Dreamcast 4.0.
      - archive.org item `dreamcast-sdks`: `SEGA Katana Dreamcast SDK R10.1.iso` (450 MB),
        `R11b.iso` (539 MB).
      - GitHub `Kochise/dreamcast-docs`, path `SDK/EXES/INSTALL KATANA SDK/INPUT/R10.1_000518`: an
        already-installed R10.1 tree with `Include/`, `Lib/` (kamui2.lib, ninja.lib, audio64.lib,
        cri_adxs.lib, and GNU/Metrowerks variants) and `Utl/`. Smallest and quickest option.
      Unpack under `sdk/<release>/` (ignored). The FID database builder needs the SDK's own
      `Utl/Dev/Hitachi/libsplit.exe` and `elfcnv.exe` to turn Hitachi `.lib` archives into ELF
      objects Ghidra can import; on this Mac that means Wine or a Windows machine, or we write a
      SYSROF-to-ELF converter (a small, well-defined job).
- [x] **Ghidra installed** (Homebrew formula 12.1.3, JDK 21 already present). Headless runs work:
      see `tools/ghidra/`.
- [x] **Flycast v2.7 in `/Applications`**, launched; owner confirms Crazy Taxi boots and plays
      fully (2026-09-11). The release build lacks the GDB server, so steps 8 and 9 were traced with a
      source build kept in the session scratchpad (`docs/autonomous-log.md`); nothing more needed
      from you here. Flycast also wrote a per-game VMU file (`MK-51035_vmu_save_A1.bin`), a useful
      emulator-side reference for WP3.3 alongside the real hardware dump.
- [ ] **Delete the stray `flycast/` folder** in the repo checkout (ignored, but 42 MB of clutter).
- [x] **Docker Desktop installed** (29.7, runs linux/arm64 and linux/amd64 containers). 2026-09-11.
- [x] **Repository is private** (confirmed 2026-09-11). CI therefore runs Linux (both interpreter
      configurations) and Windows on every push; the macOS job runs only on manual dispatch and on
      tags because private-repo macOS minutes cost 10x. Flip `build-macos` to unconditional if the
      repo goes public.
- [x] **GitHub CLI logged in.** 2026-09-11.
- [x] **ADR 1 reaffirmed** 2026-09-11 (GPL-2.0 runtime on Flycast, "option 1").
- [x] **ADRs 2 to 9 and 11 to 16 accepted** 2026-09-12.
- [x] **ADR 10 (audio) accepted as recommended** 2026-09-12 (LLE: ARM7 + AICA). All ADRs are now accepted.
- [ ] **State your working cadence** (hours per week). The plan's calendar conversions in
      `implementation-plan.md` §8 pick the row for you; nothing else depends on it.

## Needed for Phase 1

- [ ] Nothing new. The second retail binary for compiler-idiom coverage (WP1.6) can be Tech Romancer
      (Capcom) or Tony Hawk's Pro Skater 2 (Treyarch), both already dumped.

## Needed for Phase 2

- [ ] **Install the Vulkan SDK** from LunarG (the macOS package bundles MoltenVK; requires accepting
      their licence, so it cannot be scripted). Needed before WP2.3.
- [ ] Optional: a real Dreamcast with a serial coder's cable or Broadband Adapter and dcload, for the
      hardware oracle in WP1.5 and WP2.x. Flycast's interpreter is the CI oracle regardless.

## Needed for Phase 3

- [ ] **Dump a Crazy Taxi VMU save** from real hardware (a VMU file via DreamShell, a VMU backup tool,
      or a Dreamcast-to-PC link) into `games/crazytaxi/disc/vmu/` (gitignored). Used in WP3.3 to
      verify the runtime's save format against the real thing.
- [ ] **Play-test the acceptance matrix** (WP3.4): both maps at every time setting, all Crazy Box
      stages, saves persisting across restart. Engineering produces the matrix; someone has to drive
      the taxi.
- [x] ~~For a macOS release build (WP3.6): an Apple Developer account for signing and
      notarization.~~ **Closed by the owner on 2026-09-13: we are not doing a signed build.**
      Consequence for WP3.6: macOS builds are unsigned, so Gatekeeper quarantines anything
      downloaded. Whoever runs it has to open it from the right-click menu once, or clear the
      quarantine attribute, and the packaging notes have to say so. Nothing else in the plan
      depends on this.
      Optional; unsigned builds work locally with a right-click Open.

## Already done

- [x] Licence (GPL-2.0) and baseline title (Crazy Taxi) accepted. 2026-09-10.
- [x] Crazy Taxi CHD in `games/`. Checklist steps 1 to 7 passed. 2026-09-11.
- [x] `third_party/libchdr` submodule added and built. 2026-09-11.
- [x] Eight candidate discs measured; alternates set to Charge 'N Blast, Tech Romancer. 2026-09-11.
