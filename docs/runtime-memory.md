# Runtime memory model (WP2.1)

`runtime/include/dream/runtime/mem/dc_memory.h` implements the `dream::Memory` interface the emitted
code calls with guest virtual addresses. It owns the byte-addressable regions and dispatches
everything else to devices registered by the later Phase 2 work packages.

## Map

Virtual address `a`; physical `a & 0x1FFFFFFF`; area = physical bits 28:26.

| Guest range | Backing | Notes |
|---|---|---|
| Area 3, 0x0C000000–0x0FFFFFFF (P0/P1/P2/P3 aliases) | 16 MB `ram()` | index `phys & 0xFFFFFF`; three mirrors |
| Area 1, 0x04000000–0x07FFFFFF | 8 MB `vram()` | bit 24 clear: 64-bit view, linear; bit 24 set: 32-bit view, `vram_map32` interleave (bank 0 words in even 64-bit slots, bank 1 in odd); 0x06/0x07 are mirrors |
| Area 0, 0x00000000–0x001FFFFF | 2 MB `bios()` | zero unless a BIOS image is loaded; syscalls are HLE (WP2.6) |
| Area 0, 0x00200000–0x003FFFFF | 128 KB `flash()` | mirrored; file-backed in WP2.6 |
| Area 0, 0x00800000–0x00FFFFFF | 2 MB `aram()` | sound RAM, mirrored every 2 MB |
| Area 0 registers (Holly 0x005F6800+, AICA 0x00700000+, RTC 0x00710000, G2 0x01000000+) | `map_mmio` table | unmapped → fault log |
| Area 4, 0x10000000–0x13FFFFFF | `map_mmio` table | TA FIFO (WP2.3) |
| Areas 2, 5, 6, 7 | none | fault log, reads 0 |
| 0x7C000000–0x7FFFFFFF | 8 KB `ocram` | operand cache as RAM: two 4 KB halves selected by bit 13, mirrored (SH7750 manual) |
| 0xE0000000–0xE3FFFFFF | two 32-byte store queues | bit 5 selects SQ0/SQ1; readable back |
| 0xFF000000+ | CCN QACR0/1 (0xFF000038/3C) and CCR handled here; other modules via `map_p4` | TMU, INTC, DMAC, SCIF arrive with WP2.2/2.6 |

## Store queues

`sq_write32` fills the selected queue; `sq_flush(a)` (emitted for `PREF @Rn` when Rn is in the SQ
area) writes the 32 bytes to physical `(QACRn.AREA << 26) | (a & 0x03FFFFE0)`, where bit 5 of `a`
both selects the queue and stays in the destination. A destination in RAM or VRAM is a 32-byte copy;
a destination in a device (the TA FIFO) calls `MmioHandler::write_burst`, so the polygon path sees
whole 32-byte submissions.

## Devices and faults

`MmioHandler` has `read(addr, size)`, `write(addr, value, size)` and `write_burst`. Sizes are 1, 2 or 4;
64-bit accesses are split. Addresses handed over are physical for `map_mmio` ranges and virtual for
`map_p4` ranges. Lookup is a linear scan of a short table; the RAM and VRAM paths never consult it.

Anything unmapped is recorded in `FaultLog` as (address, size, direction) with a count, capped at 4,096
unique entries, and the access reads as zero. The runtime prints the log at exit; with the translator's
unreached-address report it is how a new title's missing pieces are found without the game aborting.

`DcMemory::on_device_access` (2026-09-12) is called before every device read or write; the runtime
installs a callback that advances the scheduler to the guest's cycle count (re-entrancy guarded), so
timers and drive status observe the present rather than the last interrupt poll. Katana's
`syTmrGetCount` busy-waits on TMU0 between polls and gave up before the clock moved without it.

## Derivation and deviations

- The VRAM interleave is Flycast's `pvr_map32` (GPL-2.0, ADR 1), which matches the hardware's 64-bit
  bus behaviour; the harness cannot check it (Flycast's oracle core has no VRAM test path), so the unit
  test pins the mapping.
- On-chip RAM follows the SH7750 manual; Flycast does not emulate it, so this is untested against a
  reference. Katana titles rarely enable `CCR.ORA`.
- Not yet done from the plan's wording: inline "mask-and-index" access helpers in the emitted code.
  Emitted code calls the virtual `Memory` methods; `DcMemory` takes the RAM path first, but the
  virtual call remains. Phase 3 profiling decides whether the emitter should inline the RAM check.
