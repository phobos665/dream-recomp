# Baseline commercial title (Phase 3 of the implementation plan)

**Status:** Accepted 2026-09-10, **owner's decision: Crazy Taxi** (Hitmaker, 2000). Disc verification
checklist pending on the owner's CHD dump.
**History:** the analysis below originally recommended ChuChu Rocket!; the owner chose Crazy Taxi the
same day because they hold a legal copy and its disc is small (85 MB as CHD). On 2026-09-11 the
owner's eight candidate dumps were measured with `dcdisc shortlist` (see `title-shortlist-measured.md`)
and Crazy Taxi came out best on every measured axis but streaming. **Alternates, in order: Charge 'N
Blast, then Tech Romancer**, both owned; they replace the earlier ChuChu Rocket! and Ikaruga, which
the owner does not hold. The original scoring is kept as written so the trade-off is on record.

## What the baseline is for

Phase 2's exit criterion is "one commercial title playable start to finish with VMU saves working,
zero untranslated calls". The baseline exists to prove the pipeline, not to impress. Every criterion
below follows from that.

## Selection criteria

| # | Criterion | Why it matters |
|---|---|---|
| C1 | Katana SDK, not Windows CE | WinCE is out of scope (report §6) |
| C2 | Single `1ST_READ.BIN`, no runtime-loaded `.BIN` overlays, no compressed or generated code | Removes the hardest code-discovery case from the first attempt |
| C3 | Small to medium binary (roughly 1–3 MB) | Bounds the discovery closure loop |
| C4 | Standard Sega libraries: Ninja or Kamui for the PVR, Manatee for sound, Shinobi for system | Existing FID databases identify these; what we learn generalises to Sega first-party titles |
| C5 | Fixed 60 Hz, VBlank-locked, no raster tricks | Matches the coarse cooperative timing model (ADR 7) |
| C6 | Minimal GD-ROM streaming (no FMV-heavy or ADX-streamed gameplay) | Syscall HLE with file-level reads is enough; register-level GD-ROM comes later (ADR 11) |
| C7 | VMU save through the standard Katana buffer library | The exit criterion requires saves |
| C8 | No hard dependency on modem, light gun, fishing rod, microphone | Those are stubs in v1 |
| C9 | Deterministic, fast to verify: attract mode to gameplay in under a minute | Every rebuild is checked by hand at first |
| C10 | Reasonably popular, cheap, English release | Motivates the work, easy to source a disc, easy to find testers |
| C11 | Existing reverse-engineering knowledge (widescreen hacks, decomp, symbols) | Reduces Phase 3 hook work |

## Shortlist

Scores are 0 (fails), 1 (partial or unverified), 2 (meets). Entries marked "verify" are beliefs about
the binary that the disc checklist below must confirm.

| Title | Developer / year | C1 | C2 | C3 | C4 | C5 | C6 | C7 | C8 | C9 | C10 | C11 | Total | Notes |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **ChuChu Rocket!** | Sonic Team, 2000 | 2 | 2 (verify) | 2 | 2 (verify: Ninja) | 2 | 2 | 2 | 1 | 2 | 2 | 1 | **20** | Online mode via modem is a menu option, not a dependency. Small board, simple 3D, 4-player. Very cheap disc. |
| Ikaruga | Treasure, 2002 | 2 | 1 (verify) | 2 | 1 (verify) | 2 | 2 | 2 | 2 | 2 | 1 | 1 | 18 | Best pure-determinism candidate; NAOMI port; Japan-only and expensive. Treasure may use custom rendering paths rather than Ninja. |
| Power Stone | Capcom, 1999 | 2 | 1 (verify) | 1 | 1 (verify) | 2 | 2 | 2 | 2 | 2 | 1 | 1 | 17 | NAOMI port; Capcom likely uses its own engine on top of Shinobi/Manatee. Disc now pricey. |
| Soulcalibur | Namco, 1999 | 2 | 1 (verify) | 1 | 0 (verify) | 2 | 2 | 2 | 2 | 2 | 2 | 1 | 17 | Iconic showpiece and Deecy runs it, but Namco's own 3D engine means FID coverage will be thin. Better as a Phase 3 target. |
| Marvel vs. Capcom 2 | Capcom, 2000 | 2 | 1 | 1 | 1 | 2 | 1 | 2 | 2 | 2 | 1 | 1 | 16 | CPS2-derived codebase, heavy sprite streaming from disc, expensive disc. |
| Crazy Taxi | Hitmaker, 2000 | 2 | 1 (verify) | 0 | 1 | 2 | 0 | 2 | 2 | 1 | 2 | 2 | 15 | Large open city, streamed licensed music (ADX). Good Phase 3 title, bad first title. |
| Sonic Adventure | Sonic Team, 1998/99 | 2 | 1 | 0 | 2 | 1 | 1 | 2 | 2 | 1 | 2 | 2 | 16 | The canonical Ninja title and the natural showpiece. Large, many subsystems, early SDK (R6-era). Not first. |
| Tokyo Bus Guide | Fortyfive, 1999 | 2 | 1 | 1 | 1 | 2 | 1 | 2 | 2 | 0 | 0 | 2 | 14 | Only public Dreamcast decomp, so full function boundaries are available. Japan-only, slow-paced, poor motivator. Use its test framework, not the game. |
| NFL 2K (JP) | Visual Concepts, 1999 | 2 | 1 | 0 | 0 | 2 | 1 | 2 | 2 | 0 | 0 | 2 | 12 | Retail disc shipped with the linker `.MAP` file, so every function is named. Huge binary, custom engine. Worth translating once as a **symbol-rich test input** for the translator, not as the baseline. |

Windows CE titles (Half-Life, Sega Rally 2, Resident Evil 2/3, Tomb Raider, Armada, Hidden &
Dangerous) are excluded by C1 and not scored.

## Crazy Taxi: what the choice changes

Crazy Taxi is a Katana-SDK, single-player, VBlank-locked arcade port with VMU saves and no special
peripherals, so it satisfies C1, C5, C7, C8 and C10 outright. Two rows scored low and each has a
concrete consequence in the plan rather than being a blocker:

- **C6, streaming.** The licensed soundtrack (The Offspring, Bad Religion) and speech are ADX audio
  streamed from the disc during play. GD-ROM syscall HLE therefore has to serve sustained sector reads
  with plausible timing from the first commercial title, not as a Phase 4 addition. WP2.6 grows by two
  days and gains "streaming reads with a read-completion model" as a deliverable. Register-level GD-ROM
  is still deferred unless the checklist's syscall trace shows the game bypassing the BIOS.
- **C3, binary size.** A 3D open-city NAOMI port is a larger boot binary than ChuChu Rocket!, so the
  discovery closure loop in WP3.2 is budgeted at two more days. The checklist's step 3 measures the
  real size.

Other points that matter for the acceptance matrix in WP3.4: two maps (Arcade and Original), Arcade
and Crazy Box modes, time-limited runs (3, 5, 10 minutes, Arcade rules), Crazy Box mini-game
completion, ranking and settings saved to VMU. "Playable start to finish" means: every Crazy Box stage
completable, both maps completable at every time setting, high scores persist across a restart.

### Measured on the owner's dump (2026-09-11, `games/crazytaxi/checklist-report.md`)

| Check | Result |
|---|---|
| C1 Windows CE | Pass: no WinCE flag, no WinCE files |
| C2 Code files | **One**: `1ST_READ.BIN`. The ~50 `.BIN`/`.AFS` files it references by name are data (collision, polygons, textures, sprites, sound banks) and `AICADRV.BIN` is the ARM7 sound driver, not SH-4 code |
| C3 Boot binary | 1.4 MB (1,468,208 bytes), stored plain as GD-ROMs do. Linked at 0x0C010000 (physical alias of the load address) with a 16 KB startup stub relocated to 0x8C004000 at boot; not compressed. Ghidra finds ~3,989 functions, about 26% of the image, the rest being data |
| C4 Libraries | Banners: KAMUI, Shinobi, KATANA, AICA, and 39 hits for ADX (CRI's streaming library). **No Ninja**: Hitmaker's own 3D engine sits directly on Kamui. Manatee is not named in the binary; the sound driver is the separate `AICADRV.BIN`. FID match rate still to be measured in Ghidra |
| C6 Streaming | 7 AFS files, 74 MB of the 100 MB filesystem (music `SONG01.AFS`, speech `VOICE01.AFS`, `BINC*.AFS`, `LANDDC*.AFS`). Confirms WP2.6's streaming scope |
| C7 / C8 Peripherals | Standard controller, VMU, Puru Puru, VGA box. Nothing exotic |
| IP.BIN | MK-51035 V1.004, USA, 1999-12-19, CRC valid |

Consequence for the signature work: expect FID to name the Shinobi (system) and Kamui (PVR) layers
and little else; the renderer above Kamui is Hitmaker's. That is fine for a recompiler (everything is
translated regardless) but means the C4 hope of broad library coverage is only partly met.

## Why ChuChu Rocket! was the original recommendation

- It is the smallest Sonic Team title, and Sonic Team is the heaviest user of the stock Ninja/Kamui/
  Manatee stack. Function identification on it should approach the ceiling of what the Katana FID
  databases can do, and everything learned about Ninja call patterns transfers directly to Sonic
  Adventure in Phase 3.
- Gameplay is fully deterministic and frame-locked; a puzzle either solves or it does not, which
  makes "playable start to finish" objectively checkable (clear every puzzle in Puzzle mode).
- VMU is exercised twice: puzzle progress and user-created puzzles.
- Four-controller Maple traffic and the VMU LCD give the Maple layer more coverage than a
  one-player fighter would.
- The modem mode is the one stub interaction, and it is reachable only from a menu, so it can be
  made to fail gracefully rather than blocking boot.
- The disc is common and cheap, so contributors can join without a rare import.

## Why not a fighter or shmup first

The report suggested "a fighter or shmup is ideal" as shorthand for bounded scope and determinism.
ChuChu Rocket! satisfies the bounded-scope intent better than any of the fighters on the list, all of
which are ports of arcade code with publisher-specific engines (Namco, Capcom) that the SDK FID
databases will not recognise. Ikaruga is the strongest shmup candidate and stays as the alternate.

## Disc verification checklist (Phase 0 work package WP0.4; the decision is Accepted conditionally on it)

Run against a personal dump of the chosen disc. Every item is a scriptable check that belongs in
`tools/` eventually.

1. **WinCE check.** No `0WINCEOS.BIN` in the filesystem. IP.BIN "WinCE" flag clear.
2. **Binary inventory.** List every file on the disc that is SH-4 code (scan for the standard
   prologue idioms and for `RTS`/`NOP` density, not just the `.BIN` extension). Expected: `IP.BIN`
   and `1ST_READ.BIN` only. Any other hit means C2 is "partial" and the translator must handle
   multiple images from day one.
3. **Boot binary and size.** Record `1ST_READ.BIN` size and SHA-256 (GD-ROM dumps store it plain; only CD-R style images need descrambling, which `dcdisc` detects). Expect 1–3 MB.
4. **SDK version and libraries.** String-scan the unscrambled binary for the SDK banner strings
   (`SEGA LIBRARY`, `Ninja`, `KAMUI`, `Shinobi`, `Manatee`/`AICA`) and record which Katana release
   the version strings imply. Build the matching Ghidra FID database and record the percentage of
   discovered functions it names. Below 30% means the title is not using the stock libraries and C4
   should be re-scored.
5. **Overlay and code-loading check.** Search the binary for references to other filenames on the
   disc and for calls to the GD-ROM read syscall followed by a jump into the read buffer. Any hit
   means runtime-loaded code.
6. **Streaming check.** Count `.ADX`, `.AFS`, `.SFD` files and total size. Large FMV or streamed
   audio raises C6 risk.
7. **Peripheral requirements.** Decode the IP.BIN peripheral bitfield; confirm nothing beyond
   standard controller, VMU, and rumble is flagged mandatory.
8. **Boot in Flycast with the GDB server** and log which BIOS syscall vectors are called in the first
   60 seconds. Confirms whether syscall-level GD-ROM HLE (ADR 11) is sufficient for this title.
9. **Timing probe.** In Flycast, confirm the game frame-locks on VBlank (SPG status polling or VBlank
   IRQ) and record whether any TMU channel is used for gameplay timing.

If steps 8 or 9 fail on Crazy Taxi (steps 1 to 7 passed on 2026-09-11), re-run the checklist on
Charge 'N Blast (first alternate) and then Tech Romancer before choosing anything else.

## Phase 4 sequencing (for context only)

1. Sonic Adventure: the canonical Ninja title and the largest audience.
2. ChuChu Rocket!: small, cheap, measures how much of the Crazy Taxi work transfers to a Sonic Team
   title.
3. Soulcalibur or Power Stone: proves the toolchain on a non-Sega engine.
