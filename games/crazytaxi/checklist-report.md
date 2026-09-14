# Disc verification report: CRAZY TAXI

Image: `crazytaxi.chd` read via ChdImage.

## Tracks

| # | LBA | Sectors | Sector size | Type |
|---|---|---|---|---|
| 1 | 0 | 450 | 2352 | data |
| 2 | 450 | 676 | 2352 | audio |
| 3 | 45000 | 504150 | 2352 | data |

## IP.BIN

- Title: CRAZY TAXI
- Product: MK-51035 V1.004, released 19991219
- Company: SEGA ENTERPRISES; maker: SEGA ENTERPRISES
- Regions: USA
- Boot file: 1ST_READ.BIN
- Device: EDB5 GD-ROM1/1 (CRC ok)
- Peripherals 0x0799A10: VGA box, Puru Puru pack, Memory card (VMU), Start, A, B, D-pad, X button, Y button, Analog R trigger, Analog L trigger, Analog horizontal, Analog vertical
- IP.BIN SHA-256: `69cbc1db3a5da8f2ac9feb846449758b6f0f8debca8701d906639a2584fae734`

## Checklist

1. **Windows CE:** pass, no WinCE markers
2. **Code files on disc:** 1ST_READ.BIN (expected: only the boot binary)
3. **Boot binary:** 1ST_READ.BIN, 1.4 MB (1468208 bytes)
   - as stored on disc, SHA-256 `67daaabc743ad6da0515cc25363eaf5e6624fba9353cda4dabfe2ee5c610c044`
   - plain (analysable) form, SHA-256 `67daaabc743ad6da0515cc25363eaf5e6624fba9353cda4dabfe2ee5c610c044` (same bytes: stored plain, as GD-ROMs do)
   - scores as SH-4 code: yes; literal-pool pointer ratio 0.87 in plain form vs 0.40 in the other (stored plain)
4. **SDK banners in boot binary:** ADX x39, AICA x1, Copyright x1, KAMUI x2, KATANA x1, SEGA ENTERPRISES x5, Shinobi x2, Ver. x5, Version x1. FID match rate is measured in Ghidra, not here.
5. **Disc files referenced by name from the boot binary:** AICADRV.BIN, BINC1.AFS, BINC2.AFS, BINC3.AFS, COLDC1.BIN, COLDC2.BIN, COLDC3.BIN, LANDDC1.AFS, LANDDC2.AFS, MOTDC.BIN, OBJDC1.BIN, OBJDC2.BIN, OBJDC3.BIN, POLDC0.BIN, POLDC1.BIN, POLDC2.BIN, POLDC3.BIN, RECADVAC.BIN, RECADVDC.BIN, RECENDAC.BIN, RECENDDC.BIN, SNDDC0.BIN, SNDDC1.BIN, SNDDC2.BIN, SNDDC3.BIN, SNDDC5.BIN, SNDDC6.BIN, SNDDC7.BIN, SNDDC8.BIN, SONG01.AFS, SPLDC1.BIN, SPLDC2.BIN, SPLDC3.BIN, SPRADV.BIN, SPRBOX.BIN, SPRCENG.BIN, SPRCJAP.BIN, SPRCOMM.BIN, SPRGAME.BIN, SPRGENG.BIN, SPRGJAP.BIN, SPRMENG.BIN, SPRMENU.BIN, SPRMJAP.BIN, TEXDC0.BIN, TEXDC1.BIN, TEXDC2.BIN, TEXDC3.BIN, VOICE01.AFS. Any of these that also appears in step 2 is a runtime-loaded code overlay.
6. **Streaming media:** 7 files, 74.0 MB of 100.4 MB total filesystem data
7. **Peripherals:** VGA box, Puru Puru pack, Memory card (VMU), Start, A, B, D-pad, X button, Y button, Analog R trigger, Analog L trigger, Analog horizontal, Analog vertical
8. **BIOS syscall trace:** manual, run in Flycast with the GDB server (see checklist).
9. **Timing probe:** manual, confirm VBlank lock in Flycast (see checklist).

## Files

| Path | Size | SHA-256 | Code? |
|---|---|---|---|
| 0GDTEX.PVR | 131088 | `9eb44e32f43b9b46...` |  |
| 1ST_READ.BIN | 1468208 | `67daaabc743ad6da...` | yes |
| AICADRV.BIN | 57216 | `6d659f97eedcd9e0...` |  |
| BINC1.AFS | 12777472 | `151dcb89964930df...` |  |
| BINC2.AFS | 12046336 | `ee9b5b5bc45778c3...` |  |
| BINC3.AFS | 3598336 | `c432ba423c1456da...` |  |
| COLDC1.BIN | 3205672 | `25ad5b10c030aeae...` |  |
| COLDC2.BIN | 3268952 | `b9d368ed3b1e62a0...` |  |
| COLDC3.BIN | 1821220 | `662621f73f214661...` |  |
| LANDDC1.AFS | 1835008 | `ef72ab555d8cf120...` |  |
| LANDDC2.AFS | 1835008 | `62c8447aec1ac246...` |  |
| MOTDC.BIN | 2854020 | `02506f700cad9546...` |  |
| OBJDC1.BIN | 65536 | `8e6f2be689804d57...` |  |
| OBJDC2.BIN | 65536 | `b5a925a89c767844...` |  |
| OBJDC3.BIN | 65536 | `df91fcd18418502f...` |  |
| POLDC0.BIN | 2358960 | `8fb32a156287fcef...` |  |
| POLDC1.BIN | 369816 | `0e16f42bd00704d4...` |  |
| POLDC2.BIN | 151640 | `b2eaef65d794b594...` |  |
| POLDC3.BIN | 22152 | `4dbced1c218a9f3a...` |  |
| RECADVAC.BIN | 108000 | `bd7d30fbf5895756...` |  |
| RECADVDC.BIN | 108000 | `47501b409c915693...` |  |
| RECENDAC.BIN | 108000 | `b596d817f72a3629...` |  |
| RECENDDC.BIN | 108000 | `80d66cefaf41bf86...` |  |
| SNDDC0.BIN | 795860 | `7ce47ac6a2185b81...` |  |
| SNDDC1.BIN | 477668 | `2ca1929bffd8fa10...` |  |
| SNDDC2.BIN | 184972 | `23df892ad1e3d2ad...` |  |
| SNDDC3.BIN | 37608 | `968cdc6f5c762557...` |  |
| SNDDC5.BIN | 70148 | `1607a21b38aa3355...` |  |
| SNDDC6.BIN | 208716 | `d3bfa57c83ccc4ee...` |  |
| SNDDC7.BIN | 145576 | `23f5eea2295cf972...` |  |
| SNDDC8.BIN | 181724 | `d38d5cb85f7c8868...` |  |
| SONG01.AFS | 31383552 | `2a7845c9a62deac4...` |  |
| SPLDC1.BIN | 323756 | `e9e957117597bfca...` |  |
| SPLDC2.BIN | 379044 | `83a4b1c4fd872b88...` |  |
| SPLDC3.BIN | 198244 | `0ae94d0e649ad66b...` |  |
| SPRADV.BIN | 220672 | `f3317698153eb829...` |  |
| SPRBOX.BIN | 77184 | `ea1ba161afe16976...` |  |
| SPRCENG.BIN | 98304 | `97d27ab4c1449d5f...` |  |
| SPRCJAP.BIN | 90112 | `7b551b456befe321...` |  |
| SPRCOMM.BIN | 586688 | `c514511fcae206d9...` |  |
| SPRGAME.BIN | 1277056 | `a932357b54ab3e2a...` |  |
| SPRGENG.BIN | 122880 | `7401ac7f2d580c5c...` |  |
| SPRGJAP.BIN | 95232 | `90b9f41ef895a54d...` |  |
| SPRMENG.BIN | 426496 | `e312c1a9711057aa...` |  |
| SPRMENU.BIN | 145664 | `e23d3624272ba225...` |  |
| SPRMJAP.BIN | 545792 | `32673cf2b7881874...` |  |
| TEXDC0.BIN | 1534368 | `a268126dfeaf9815...` |  |
| TEXDC1.BIN | 1158016 | `20036c2a8b32993e...` |  |
| TEXDC2.BIN | 1096512 | `3c1c8374b38da8e8...` |  |
| TEXDC3.BIN | 800416 | `2b44c60a468a7d5d...` |  |
| VOICE01.AFS | 14137344 | `1994d6c56f9eac49...` |  |

## Addendum 2026-09-11: binary layout (measured)

`1ST_READ.BIN` is **linked at 0x0C010000**, the physical (P0) alias of the 0x8C010000 load address:
11,850 literal-pool values fall inside `[0x0C010000, 0x0C176730)` and 43% of them land on a function
prologue or PC-relative load when the file is mapped there with no offset (5% would be chance). Zero
literals fall in the 0x8C01xxxx range. The image is **not compressed**; the low-entropy words at
file offset 0x4000 are data tables.

Boot sequence, decoded from the literal pools:

1. The first 0x100 bytes are a loader running at 0x8C010000. It copies `[0x0C010100, 0x0C014000)`
   (file offsets 0x100 to 0x4000, 16,128 bytes) to **0x8C004000** through the uncached alias and
   jumps there.
2. The copy is the Katana startup stub. It sets FPSCR to 0x00040001, initialises the system
   variables at 0x8C00F400 to 0x8C00FA3C, and enters the program proper (references to 0x0C010000
   and 0x0C020000).

Consequences: Ghidra must import the file at base 0x0C010000 with a second initialised block at
0x8C004000 (`games/crazytaxi/ghidra/CrazyTaxiLayout.java` does this); the translator's per-game
config needs `load_address = 0x8C010000`, `link_address = 0x0C010000`, and a relocated region for the
stub. The other seven measured discs are linked at 0x8C010000 with a plain NOP-slide prologue, except
AeroWings, which also uses the 0x0C alias.

Ghidra 12.1.3 headless discovery on this binary (the binary contains 3,997 `RTS` instructions, so ~3,989 functions is close to complete; the remaining 74% of the image is data):

| Configuration | Functions | Code bytes | Coverage |
|---|---|---|---|
| Base 0x8C010000, recursive descent from entry | 283 | 28,496 | 1.9% |
| Base 0x8C010000, plus aggressive instruction finder | 1,127 | 178,684 | 12.2% |
| Base 0x0C010000, stub block, aggressive finder | 2,391 | 379,164 | 25.5% |
| Same, plus iterative literal-pool seeding | 4,180 | 388,882 | 26.2% |

(An earlier version of this table gave 3,989 for the third row; that run had a partial seeding pass
in it. The corrected figure without seeding is 2,391.)

Seeding detail: round 1 found 3,013 pointer targets in undefined bytes and created 1,589 functions,
round 2 found 1,412 more targets and could disassemble none. Coverage rose from 379,164 to 388,882
bytes (+2.6%) while the function count rose by 1,789, so seeding mostly splits and labels code the
finder had already reached, plus a tail of false positives from data pointers. Conclusion for the translator (ADR 8): the correct link address and an instruction
finder do the heavy lifting on this binary; literal-pool seeding is a small supplement that must be
gated on a plausibility test (target starts like a function and reaches an `RTS`), not taken raw.
The 908 P-code warnings about undisassembled delay slots came from the aggressive finder and are
cosmetic here.

## Addendum 2026-09-11: steps 8 and 9, static evidence

The macOS Flycast 2.7 release is built without `ENABLE_GDB_SERVER` and `ENABLE_LOG`, so its GDB
settings are inert and its HLE BIOS does not log syscalls. A source build with both enabled is the
dynamic route (`tools/flycast/gdbtrace.py` drives it). Meanwhile the binary itself answers most of
step 8 and part of step 9. Flycast boots and runs the game **without a BIOS image** ("Did not load
BIOS, using reios"), so every syscall the game needs is one Flycast's HLE BIOS implements: a good
sign for WP2.6.

**BIOS syscall vectors loaded by code** (literal-pool scan; `MOV.L @(disp,PC)` of the vector
address, i.e. the call wrappers):

| Vector | Purpose | Code sites | Where |
|---|---|---|---|
| 0x8C0000BC | GD-ROM / misc | 13 | 0x0C1653EC to 0x0C1654xx, one wrapper cluster (Shinobi `gd` library) |
| 0x8C0000B8 | Flash ROM | 4 | 0x0C16E5A2 to 0x0C16E5DE |
| 0x8C0000B0 | System info | 3 | 0x0C165556 to 0x0C16557E |
| 0x8C0000E0 | System (reboot to menu) | 3 | 0x0C14F2B4 to 0x0C14F2CC |
| 0x8C0000B4 | ROM font | 0 | not used |

**Hardware registers referenced directly** (distinct literal-pool addresses; displacement
addressing from a base register is not counted, so these are lower bounds):

| Block | Distinct / refs | Notable | Meaning for the runtime |
|---|---|---|---|
| PVR / TA registers 0xA05F8xxx | 25 / 39 | 0x5F8000 ID, 0x5F8008 reset, 0x5F8040/44 border and FB config, 0x5F8068/6C, 0x5F810C **SPG_STATUS**, 0x5F80E8 | Kamui programs the PVR directly; full PVR MMIO needed. SPG_STATUS polling is the VBlank wait (step 9) |
| TA FIFO 0x10000000, YUV 0x10800000 | 8 / 40 | | Store-queue vertex submission, YUV converter used |
| Holly interrupt 0xA05F69xx | 7 / 24 | 0x5F6900/04/08 status, 0x5F6920/24/30/38 masks | Game-side interrupt dispatch on top of VBR handlers |
| Holly system 0xA05F68xx | 6 / 17 | 0x5F6800/04/08 (channel-2 DMA to TA), 0x5F688C, 0x5F689C | Ch2 DMA must be modelled |
| G2 / AICA DMA 0xA05F78xx | 3 / 3 | 0x5F7800, 0x5F7890, 0x5F78BC | Sound driver and sample upload by G2 DMA |
| AICA 0x702800, 0x702C00; RTC 0x710000/04/08 | 6 / 9 | | Direct AICA control and real-time clock reads |
| GD-ROM ATA block 0xA05F70xx | 4 / 5 | 0x5F705C, 0x5F707C, 0x5F7080, 0x5F70C0 | **To check in Ghidra**: status/ID polling would be benign, command issue would mean register-level GD-ROM is needed for this title |
| G1 bus 0xA05F74xx | 3 / 3 | 0x5F7490, 0x5F7494, 0x5F74A0 | Bus timing/protection, not GD DMA (0x5F7404/08/14 absent) |
| Maple 0xA05F6C00 | 1 / 1 | | Maple is driven by the Shinobi pad library through a base register; expect full Maple DMA use |
| Flash ROM 0x00200000 | 6 / 24 | 0x200000, 0x200005, 0x2000A0 | Direct flash reads alongside the flash syscall |
| SH-4 control | many | 0xFFD80004 (TMU0), 0xFF000038 (CCR), 0xFF00001C/38/3C | TMU used for timing; cache control at startup |

**GD-ROM register references resolved** (Ghidra dump of the five sites): the three in the startup stub
(0x0C010F56 to 0x0C010F8E, running at 0x8C0040xx) are the Katana boot-time G1 bus setup: they write
the timing constant 0x0511 to 0xA05F7080 and 0xA05F70C0, program 0xA05F7490/0x7494 and set
0xA05F74A0 to 5, and OR bit 3 into 0xA05F707C, once, before the game starts. The two in the main
program (0x0C07A436, 0x0C07A720) are tiny wrappers that store the constant 3 to 0xA05F705C, again
configuration rather than a command. **No GD-ROM command or data-register traffic exists outside the
BIOS syscall path**, so syscall-level GD-ROM HLE (ADR 11) is sufficient for this title; the runtime
only has to accept these configuration writes.

Step 9 conclusion so far: the game polls SPG_STATUS and services VBlank through the Holly interrupt
registers, the standard Kamui frame flip. Confirming the frame rate lock needs the dynamic trace or
the translator's own run. Step 8 conclusion so far: all disc access visible at the call level goes
through the GD-ROM syscall vector; the five GD-ROM register references are the one open question.

## Addendum 2026-09-11: step 4, Function ID match rate

A Ghidra FID database was built from the Katana SDK R10.1 libraries available as ELF objects: the
GNU (GCC) and Metrowerks (CodeWarrior) variants, 7,172 objects in total (`tools/ghidra/build_fidb.py`).
The Hitachi SHC variants (`Lib/*.lib`, SYSROF format) could not be imported yet.

Applied to the Crazy Taxi binary (base 0x0C010000, stub mapped, aggressive finder, no seeding):

| | Count |
|---|---|
| Functions found | 2,391 |
| Named by FID | 372 (15.6%), 28 of them multi-match conflicts |
| Code bytes named | 35,836 of 379,164 (9.5%) |

What matched: essentially all of CRI's ADX library (`_ADXB_*`, `_ADXF_*`, `_ADXPD_*`, `_ADXAMP_*`,
`_ADXCRS_*`), a few Shinobi system and Maple routines (`_syCacheSetForm`, `_mpdrv_*`, `_kd*`,
`_kbCfgIni`). What did not: Kamui, Ninja and the bulk of Shinobi. CRI shipped one build of ADX for
every toolchain, which is why it matches regardless; the Sega libraries exist in three compiler
variants and the game evidently links the **SHC-built** ones. Conclusion: the FID pass is working, the
missing coverage is a library-format problem, and a SYSROF-to-ELF converter (or running the SDK's
`libsplit.exe`/`elfcnv.exe` under Wine) is required before the Sega library functions can be named.

## Addendum 2026-09-11: steps 8 and 9, dynamic trace (Flycast source build)

Flycast built from source with `ENABLE_GDB_SERVER` and `ENABLE_LOG` (see `docs/autonomous-log.md`),
running the CHD on its HLE BIOS; `tools/flycast/gdbtrace.py` attached over the GDB remote protocol.

**Step 8, syscalls in a 60 s window covering boot, attract and menu** (breakpoints on the five vector
targets, 256 hits) and the HLE BIOS's own log for the whole run:

| Syscall | Calls | Detail |
|---|---|---|
| Flash ROM read | 201 | Partition scan of offsets 0x1D280 to 0x1FFC0 in 0x40 steps, repeated; plus FLASHROM_INFO for partition 2 (settings) |
| GD-ROM MAINLOOP / EXEC_SERVER | 21 (937 in the full log) | Polled every frame while a read is outstanding |
| GD-ROM CHECK_DRIVE / GET_DRV_STAT | 15 (678) | Drive status polling |
| GD-ROM CHECK_COMMAND / GET_CMD_STAT | 12 | Completion polling |
| GD-ROM SEND_COMMAND | 6 (240) | 235 x DMAREAD (0x11), and once each at boot: INIT (0x18), REQ_MODE (0x1E), SET_MODE (0x1F, speed 0, standby 0xE10, read flags 0x19, retries 8), GET_VERS (0x28) |
| SYSINFO | 1 (4) | SYSINFO_ID (console ID) |
| ROM font | 0 | never |
| SYSTEM (reboot) | 0 | never |

Every disc read is a BIOS DMAREAD; no PIO reads, no streaming variants, no direct register traffic.
This is the complete syscall surface WP2.6 must implement for this title. Hardware traffic seen by the
emulator alongside: store-queue writes with QACR set to 0x10 (TA FIFO) and 0xAC (RAM), channel-2 DMA
(DMAC_CHCR2/DAR2/DMATCR2) for TA submission, Maple DMA at two descriptor addresses, and SPG_VBLANK
register reads once per frame.

**Step 9, timing model.** PC sampling (101 samples over 15 s in attract mode): 18% at 0x8C00FA00,
the rest spread over game code (function epilogues and small accessors, i.e. a normal frame loop).
0x8C00FA00 was dumped from the live machine: it is the **interrupt entry trampoline at VBR+0x600**
(saves registers, reads CCN_INTEVT at 0xFF000028, dispatches to a handler table), so VBR = 0x8C00F400
with the general-exception vector at 0x8C00F500 and TLB at 0x8C00F800. The game is interrupt-driven
(VBlank, TA list completion, DMA and Maple through Holly), reads SPG_VBLANK each frame, and does not
spin on SPG_STATUS in the sampled window. This is the cooperative model ADR 7 assumes: deliver
interrupts at function entries and back-edges, with the VBlank on the virtual clock.

**Checklist status: all nine steps have evidence. WP0.4 complete; Crazy Taxi confirmed as baseline.**
