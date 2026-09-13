# Maple bus (WP2.4, headless part)

`runtime/maple/` is the controller bus without any host input yet: the DMA engine the game programs
at 0x005F6C00, the frame protocol, and a standard controller whose state a host layer (SDL3, WP2.4's
second half) or a test sets directly. Protocol details follow Flycast's `hw/maple` (GPL-2.0, ADR 1).

## Transfer

The game builds a descriptor list in RAM and points SB_MDSTAR at it. Each entry is a header word
(bit 31 last entry, bits 17:16 port, bits 10:8 pattern, bits 7:0 frame words minus one), the receive
address, and for pattern START the frame itself: `command | recipient << 8 | sender << 16 | words << 24`
followed by the argument words. A main device on port p has address `p << 6 | 0x20`; the console is
`p << 6`. SB_MDST = 1 (software trigger) or the SPG's VBlank-out line with SB_MDTSEL = 1 (hardware
trigger, what Katana's pad library uses) starts the walk. Replies are written to the receive
addresses when the transfer time has elapsed on the virtual clock (2 Mb/s out, 740 kb/s back, Flycast's
measured rates), SB_MDST clears and Holly's Maple-DMA interrupt (bit 12) is raised. A port with
nothing attached receives 0xFFFFFFFF, the bus time-out marker. SB_MMSEL = 0 byte-swaps frames.

## Devices

`maple::Device` handles one frame and fills a `Payload`; `maple::Controller` answers Device Request
and All Status Request with the fixed status block (function 0x01000000, capabilities 0xFE060F00,
"Dreamcast Controller", the SEGA licence string, 43/50 mA), Get Condition with the button word
(active low, D-pad 2/C/D/Z forced released) and six axes (right and left trigger, stick x/y, two
unused at 0x80), and Reset/Kill with a plain reply. Sub-devices (VMU in the expansion sockets),
storage, LCD and rumble are not modelled yet; Get Condition on a wrong function code and unknown
commands return the standard error replies.

## Wiring

The launcher maps the bus, attaches a controller on port A and routes `Spg::on_vblank_out` to
`Bus::vblank()`. `dream_runtime_tests` drives the same paths through the SB registers: a device
request, a condition read with buttons and axes set, an empty port, an error reply, and the
VBlank-triggered list running once per frame.

## Not done

- Host input (SDL3 gamepad), VMU (block storage, LCD, clock), Puru Puru, sub-device addressing,
  lightgun. The bus and controller are the parts the boot needs first.

## Memory cards (2026-09-12)

A visual memory unit is 128 KB of flash in 256 blocks of 512 bytes, read and written a block at a
time. `maple::MemoryCard` implements the storage function against a plain 128 KB image in the
layout every Dreamcast tool uses, so a save made in an emulator works directly; writes go back to
the file. Attach it with `crazytaxi_boot --vmu FILE`. **Never commit an image**: it is the owner's
data.

Three things had to be right before a title would talk to one:

- **A memory card lives in a controller's expansion slot, not on a port of its own.** The recipient
  byte of a frame names the device: bit 5 is the controller and bits 0 to 4 its five slots, so a
  card in the first slot is addressed as 0x01. The bus routes on that byte now.
- **The controller's reply says which slots are occupied.** A title discovers a card from the
  sender byte of the controller's own reply rather than by probing every address, so the bus ORs
  the occupied-slot bits into it.
- **The function selector in a frame is in the frame's own byte order, but the block and phase word
  after it is big-endian.** Swapping both reads the selector as 2 rather than the storage function,
  and every request is refused with no other symptom: the card simply looks absent.

Titles also draw on the card's little screen; Crazy Taxi sends it an image every few frames. The
screen is not modelled, but those writes are accepted so the title does not decide the device is
broken. **Idea for later (owner, 2026-09-12): a hotkey that renders the VMU screen**, which would
make that traffic visible and is a small, self-contained feature once there is a window to draw it
in.

With the owner's Crazy Taxi save (Flycast writes one per title as
`<product-id>_vmu_save_A1.bin`, and Crazy Taxi's product code MK-51035 is in its own IP.BIN) the
title reads the file and says so on screen: "File loaded from the VMU".
