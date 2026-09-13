# BIOS syscall HLE, GD-ROM and flash (WP2.6)

Katana titles reach the BIOS only through the syscall vectors at 0x8C0000B0..E0 (Crazy Taxi's
measured surface is in `games/crazytaxi/checklist-report.md`). `hle::Bios` writes those vectors to
point at native functions registered in the guest function table, so a `jsr` through a vector lands
in C++ with the guest registers in hand and returns like any translated function. Semantics follow
Flycast's reios and the KallistiOS headers (GPL-2.0 derivation per ADR 1).

| Vector | Selector | Implemented |
|---|---|---|
| 0x8C0000B0 SYSINFO | r7 | INIT (writes the 24-byte id block at 0x8C000068 from flash), ICON, ID |
| 0x8C0000B4 ROMFONT | r1 | ADDRESS (an empty area in the boot ROM space), LOCK, UNLOCK |
| 0x8C0000B8 FLASHROM | r7 | INFO (partition table), READ, WRITE (bits clear only), DELETE (erase partition) |
| 0x8C0000BC / C0 GD-ROM | r6 = 0, r7 | REQ_CMD, GET_CMD_STAT, EXEC_SERVER, INIT_SYSTEM, GET_DRV_STAT, G1_DMA_END, READ_ABORT, RESET, CHANGE_DATA_TYPE, SET_PIO_CALLBACK; the streaming transfer functions return GDC_ERR and are counted |
| 0x8C0000BC misc | r6 = -1, r7 | MISC_INIT, MISC_SETVECTOR (accepted, no effect) |
| 0x8C0000E0 system | r4 | normal init, check disc, exit to menu (no-op) |

## GD-ROM command queue

One command at a time. `REQ_CMD` copies the four parameter words and returns a request id (0 while
busy); `EXEC_SERVER` executes it; `GET_CMD_STAT` returns BUSY, COMPLETE or ERR with the four result
words, and the queue is idle again once a completion has been reported. Commands: INIT, DMAREAD and
PIOREAD (sector reads), GETTOC2, REQ_SES, REQ_MODE, SET_MODE, GET_VERSION, GETSCD; the CD-audio
commands complete without doing anything (Crazy Taxi streams ADX from data sectors); anything else
is an illegal request.

Reads complete on the virtual clock with Flycast's rate model: five sectors per million cycles for
transfers above 10 KB (about 1.8 MB/s) and two cycles per byte below. `EXEC_SERVER` advances the
scheduler to the caller's clock first, so a game that polls every frame sees the read progress it
would see on hardware, and a completed read raises the Holly GD-DMA interrupt. Parameters are FADs
(LBA + 150); data lands through the RAM fast path when the destination is main RAM.

Timing (2026-09-12): every syscall charges guest cycles (100; GD-ROM 300) because the real BIOS
entry runs a few hundred instructions before answering and games calibrate their polling against
that. Crazy Taxi retries a status check 120 times; at zero cost the loop expired before a 4096-cycle
sector read could land and the title re-issued the read forever. The status syscalls
(`GET_CMD_STAT`, `GET_DRV_STAT`) also run the scheduler up to the guest clock first, and every
device register access does the same through `DcMemory::on_device_access`, so a read that
completed before "now" is complete when asked whether or not an interrupt poll has run.

`GET_DRV_STAT` (2026-09-12) reports PAUSE (1) whenever a disc is present: BUSY describes the drive
mechanism rather than a queued syscall command, and PLAY is CD audio, which is not modelled. This
matches Flycast's HLE BIOS. Reporting BUSY during a read, and suppressing the G1 DMA-end
interrupt, were both tried against Crazy Taxi and change nothing; the interrupt is raised because
the real BIOS raises it and a title can hook it through `FN_G1_DMA_END`.

`DREAM_TRACE_GDROM=1` logs every GD-ROM syscall with its arguments, result words, drive status and
queue state. `tools/flycast/oracle/flycast-gdrom-trace.patch` adds the same log, in the same
format, to Flycast's HLE BIOS (`core/reios/gdrom_hle.cpp`) so the two can be diffed request by
request; apply it to the oracle checkout when a title's disc behaviour needs comparing.

## Disc images (`gdrom::Disc`)

GDI track sets and CHD files (through libchdr, static) at sector granularity, with the layout rules
`tools/dcdisc` established on real dumps: CHD frames of 2,448 bytes, tracks padded to four frames,
GD-ROM inter-track gaps stored as PAD frames at the end of the preceding track, audio byte-swapped.
`read_user` returns the 2,048 user bytes for 2048/2336/2352-byte layouts. `toc(area)` produces the
102-word Katana TOC (entries `(ctrl << 4 | 1) << 24 | FAD`, first/last/lead-out) per session. The
synthetic CHD in `tools/dcdisc/tests/fixtures` pins layout, sector reads and the TOC in
`dream_runtime_tests`.

## Flash

`hle::Flash` formats the 128 KB image the way the system libraries expect: factory strings at
0x1A000/0x1A0A0, zeroed reserved partition, and the block-allocated partitions (user 0x1C000/16K,
game 0x10000/32K, unknown 0x00000/64K) with the `KATANA_FLASH____` header, 64-byte user blocks with
a CRC-16, and free bitmaps at the end of each partition. A system-configuration block (id 5) with the
chosen language is written at format time. The syscalls read and program the same bytes; the
contents are not yet saved to a file between runs.

## Boot hand-over

`Bios::setup_boot(addr)` reproduces the register and memory state Flycast measured after a real
BIOS boot (r15 = 0x8D000000, GBR = VBR = 0x8C000000, SR 0x400000F1, FPSCR 0x00040001, the odd
scratch registers included), writes the vectors and the SYSINFO block, and loads IP.BIN from the
game area's first 16 sectors to 0x8C008000. The executable itself is placed by the launcher.

## Launcher

`runtime/boot/boot_main.cpp` is compiled with each game's emitted units (see
`games/crazytaxi/CMakeLists.txt`, active only when the owner's executable is present):

```sh
build/dream/games/crazytaxi/crazytaxi_boot --config games/crazytaxi/crazytaxi.toml \
    --stop-on-ta --max-frames 600 [--sample N --sample-file F] [--dump ADDR:LEN] [--report FILE]
```

It mounts `[disc] image`, installs the BIOS, loads the executable at `load_address`, and runs the
translated entry until the first TA FIFO write (`--stop-on-ta`), a frame or time limit, or a fault.
The report lists syscall counts, interrupts, GD-ROM sectors, unmapped accesses, untranslated call
targets with the image bytes found at them, and every run of RAM below the load address that
reproduces image code, printed as `[[relocations]]` entries ready for the TOML. Status for Crazy
Taxi is in `progress.md` (WP2.6).

## Not done

- Flash persistence to a file, GDI fixture test (the Python tests cover GDI; the C++ reader is only
  exercised by code review until a GDI fixture is generated), streaming read variants, CD audio.
