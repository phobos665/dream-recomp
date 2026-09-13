# Dreamcast hardware cheat sheet for the recompiler

Working reference for addresses and conventions that come up constantly. Values are from the SH7750
manuals, Marcus Comstedt's Dreamcast documentation, and KallistiOS headers. Confirm any value against
KallistiOS `kernel/arch/dreamcast/include/dc/*.h` before hard-coding it; that source is the
project's register-level ground truth.

## SH-4 address space (MMU off, as in every Katana title)

| Area | Virtual (P1, cached) | Virtual (P2, uncached) | Physical | Size | Contents |
|---|---|---|---|---|---|
| Boot ROM | 0x80000000 | 0xA0000000 | 0x00000000 | 2 MB | BIOS (not needed; syscalls are HLE'd) |
| Flash | 0x80200000 | 0xA0200000 | 0x00200000 | 128 KB | Settings, region, language |
| Holly registers | 0x805F0000 | 0xA05F0000 | 0x005F0000 | 64 KB | System, Maple (0x5F6C00), GD-ROM (0x5F7000), G1/G2 DMA, PVR (0x5F8000) |
| Sound RAM | 0x80800000 | 0xA0800000 | 0x00800000 | 2 MB | AICA program and samples |
| AICA registers | 0x80700000 | 0xA0700000 | 0x00700000 | | Channels, DSP, ARM7 control, RTC at 0x710000 |
| VRAM 64-bit view | 0x84000000 | 0xA4000000 | 0x04000000 | 8 MB | Textures, object lists, framebuffer |
| VRAM 32-bit view | 0x85000000 | 0xA5000000 | 0x05000000 | 8 MB | Same memory, different interleave |
| Main RAM | 0x8C000000 | 0xAC000000 | 0x0C000000 | 16 MB | Mirrored every 16 MB across the 64 MB area |
| TA FIFO | | 0x10000000 | | | Polygon path 0x10000000, YUV converter 0x10800000, texture DMA 0x11000000 |
| Store queues | 0xE0000000–0xE3FFFFFF | | | 2 × 32 B | `PREF` flushes to the address formed with QACR0/1 |
| OC RAM | 0x7C000000 | | | 8 KB | Operand cache as RAM (CCR.ORA) |
| CPU control registers | 0xFF000000–0xFFFFFFFF | | | | CCN 0xFF000000, UBC 0xFF200000, BSC 0xFF800000, DMAC 0xFFA00000, INTC 0xFFD00000, TMU 0xFFD80000, SCIF 0xFFE80000 |

Physical address = virtual & 0x1FFFFFFF. Main RAM fast path: `(addr & 0x1C000000) == 0x0C000000`,
index `addr & 0x00FFFFFF`.

## Boot layout in main RAM

| Address | Contents |
|---|---|
| 0x8C000000 | BIOS work area, syscall vector table below |
| 0x8C0000B0 | Syscall vector: SYSINFO |
| 0x8C0000B4 | Syscall vector: ROMFONT |
| 0x8C0000B8 | Syscall vector: FLASHROM |
| 0x8C0000BC | Syscall vector: GD-ROM and misc |
| 0x8C0000E0 | Syscall vector: system (reboot to menu) |
| 0x8C004000–0x8C008000 | Persistent area across disc swaps (multi-disc titles) |
| 0x8C008000 | IP.BIN load address (bootstrap 1 at +0x300, bootstrap 2 at +0x3800) |
| 0x8C010000 | `1ST_READ.BIN` load address and entry point |
| 0x8C00F400 | Initial stack pointer set by the BIOS (games reset it) |

## Exceptions and interrupts

| Vector offset from VBR | Event |
|---|---|
| 0x100 | General exceptions (TRAPA, illegal instruction, address error, FPU) |
| 0x400 | TLB miss (never fires with MMU off) |
| 0x600 | External interrupts (IRL, TMU, DMAC, Holly via IRL levels) |

Holly routes its many sources through three interrupt status registers (normal 0x5F6900, external
0x5F6904, error 0x5F6908) with per-IRL-level mask registers at 0x5F6910–0x5F6938. Games typically
enable VBlank-in, TA list end (opaque, translucent, punch-through), render done, and Maple DMA done.

On interrupt entry the CPU sets SR.BL=1, SR.RB=1, SR.MD=1, saves PC to SPC and SR to SSR, and jumps
to VBR+0x600. R0–R7 switch to BANK1. `RTE` restores from SPC/SSR. The runtime's `deliver_irqs`
emulates exactly this.

## FPSCR bits that change instruction meaning

| Bit | Meaning | Set by |
|---|---|---|
| PR (bit 19) | 0 single, 1 double precision for arithmetic | `LDS Rn,FPSCR`, `FPCHG` |
| SZ (bit 20) | 0 32-bit, 1 64-bit pair `FMOV` | `LDS Rn,FPSCR`, `FSCHG` |
| FR (bit 21) | Bank select FR0–15 vs XF0–15 | `LDS Rn,FPSCR`, `FRCHG` |
| DN (bit 18) | Denormals flushed to zero | `LDS` only |
| RM (bits 0–1) | Rounding: 00 nearest, 01 toward zero | `LDS` only |

Katana default FPSCR is 0x00040001 (DN=1, RM=toward zero). KOS default is the same. Note the
non-IEEE default rounding: host MXCSR/FPCR must follow it.

## PVR2 essentials

- TA parameter format: 32-byte global parameters (control word, ISP/TSP, TCW) then 32- or 64-byte
  vertices; parameter type in bits 31–29 of the first word (0 end of list, 1 user clip, 4 polygon or
  modifier volume, 5 sprite, 7 vertex).
- List types: opaque, opaque modifier, translucent, translucent modifier, punch-through.
- Render trigger: write to `STARTRENDER` (0x5F8014); frame flip via `FB_R_SOF1/2` (0x5F8050/54).
- Texture formats: ARGB1555, RGB565, ARGB4444, YUV422, bumpmap, palette 4/8-bit; twiddled, VQ,
  stride, mipmapped.
- VBlank timing registers: SPG_HBLANK_INT 0x5F80C8, SPG_VBLANK_INT 0x5F80CC, SPG_STATUS 0x5F810C.

## GD-ROM disc layout

- Single-density area: LBA 0–~23000, standard CD (session 1: audio track, session 2: tiny data track
  with warning image).
- High-density area: starts at LBA 45000; track 3 onward. The game's ISO9660 filesystem is here.
- GDI file: text list of tracks with LBA, type, sector size, filename, offset.
- `1ST_READ.BIN` is stored plain on a GD-ROM. The BIOS scrambles/descrambles it only on the CD-R
  (MIL-CD) boot path, so self-boot CDI images carry a scrambled copy; the permutation is Marcus
  Comstedt's `scramble.c`, reimplemented in `tools/dcdisc/dcdisc/scramble.py`.

## Maple

- Four ports, each with a main device and up to five sub-devices (VMU, rumble, mic).
- Frame: 32-bit header (command, destination, source, length in words) plus payload; DMA transfer
  list in main RAM pointed to by MAPLE_DMAADDR (0x5F6C04), triggered by MAPLE_STATE (0x5F6C18).
- Commands the runtime needs first: DEVICE_REQUEST (1), GET_CONDITION (9), BLOCK_READ (11),
  BLOCK_WRITE (12), SET_CONDITION (14), GET_LAST_ERROR (15). VMU is 128 KB in 512-byte blocks,
  LCD is 48×32 1-bit.
