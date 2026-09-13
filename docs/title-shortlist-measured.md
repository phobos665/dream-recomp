# Baseline title shortlist: measured

Produced 2026-09-11 with `dcdisc shortlist` (WP0.3) from the owner's CHD dumps on macOS ARM64 with
libchdr 0.3. Every column is read from the disc image; nothing here is a guess. See
`baseline-game.md` for the criteria (C1 to C11) and the decision.

| Title | Product | Boot binary MB | Files scoring as SH-4 code | Streamed media MB | Filesystem MB | WinCE | Katana banners in boot binary |
|---|---|---|---|---|---|---|---|
| **Crazy Taxi** | MK-51035 | **1.40** | **1** | 74 | **100** | no | Kamui, Shinobi (+ CRI ADX) |
| Charge 'N Blast | T15127N | 2.42 | 1 | **33** | 78 | no | Kamui, Shinobi |
| Tech Romancer | T1208N | 2.52 | 1 | 202 | 966 | no | Ninja, Kamui, Shinobi |
| Dino Crisis | T1217N | 1.66 | 1 | 545 | 647 | no | Ninja, Kamui, Shinobi |
| Tony Hawk's Pro Skater 2 | T13006N | 3.35 | 1 | 696 | 803 | no | Kamui, Shinobi |
| Metropolis Street Racer (Rev A) | MK-51012 | 0.48 | 5 | 490 | 758 | no | Kamui, Shinobi |
| AeroWings | T40201N | 3.98 | 7 | 741 | 857 | no | Kamui, Shinobi |
| Street Fighter III: 3rd Strike | T1213N | 13.67 | 5 | 222 | 330 | no | Ninja, Kamui, Shinobi |

All eight store the boot binary plain (GD-ROM dumps always do; only CD-R style images are scrambled).
None flags Windows CE. MSR alone sets IP.BIN peripheral bit 0x2000000, which the reference
documentation calls "gun"; the game has no light-gun mode, so that bit likely covers the racing
wheel or is a mastering quirk. Treat IP.BIN peripheral flags as advisory.

## How to read it

- **Boot binary MB** is the size of `1ST_READ.BIN`, the thing the translator has to lift. Smaller means
  a shorter discovery loop. Street Fighter III's 13.7 MB binary carries its sprite data inside the
  executable, which is why it dwarfs the others.
- **Files scoring as SH-4 code** counts files the heuristic identifies as SH-4 machine code. One is the
  ideal: the whole game is in the boot binary. More than one means runtime-loaded code overlays
  (MSR, AeroWings, Street Fighter III), which the translator must handle as separate images with
  their own load addresses. `AICADRV.BIN`-style ARM7 sound drivers are correctly not counted.
- **Streamed media MB** totals `.ADX`, `.AFS`, `.SFD` and `.STR` files: music, speech and FMV read from
  the disc during play. This is the GD-ROM streaming load the runtime must sustain (WP2.6).
- **Filesystem MB** is the sum of all files. Large values with modest streaming (Tech Romancer,
  AeroWings) are mostly dummy padding files that place the real data on the fast outer part of the
  disc; they cost nothing at runtime.
- **Katana banners** are library names found as strings in the boot binary. Ninja present means the
  stock Sega 3D library, which the Katana Function ID databases name well. Crazy Taxi and the
  Bizarre, Treyarch and Sammy titles use Kamui directly with their own engines above it.

## Reading against the criteria

- **Crazy Taxi** has the smallest boot binary of any single-code-file title, the smallest real
  filesystem, moderate streaming, and standard peripherals. It is the best measured candidate. Its
  engine is Hitmaker's own on top of Kamui, so FID coverage will be limited to Shinobi and Kamui.
- **Charge 'N Blast** is the closest rival: one code file, the least streaming of all eight, a tiny
  disc. Its boot binary is 1 MB larger than Crazy Taxi's. First alternate.
- **Tech Romancer** is the best fighter: one code file, moderate size, and, contrary to the earlier
  assumption that Capcom used its own libraries, it carries Ninja. A well-behaved second alternate.
- **Dino Crisis** also carries Ninja and has a small binary, but half a gigabyte of FMV and a long
  survival-horror campaign make "playable start to finish" slow to verify.
- **Tony Hawk's Pro Skater 2**, **MSR**, **AeroWings** and **Street Fighter III** are ruled out as a
  first title by binary size, overlays, or both. All remain good Phase 4 candidates, and MSR and
  AeroWings will be the titles that force overlay support.

## Decision

Crazy Taxi stays (ADR 15). Alternates, in order: Charge 'N Blast, Tech Romancer. Both are owned.
