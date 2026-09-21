# The boot stack pointer

`Bios::setup_boot` hands a title the register state a real console would have left. Until
2026-09-21 it started `r15` at `0x8D000000`, the top of the 16 MB. It now starts at `0x8C00F400`.

## The register capture was of the wrong instant

The values in `setup_boot` come from Flycast's `reios_setup_state`, which carries them in a comment
headed "Post Boot registers from actual bios boot". The last line of that capture is
`pc 0xac008300` — the entry of the disc's bootstrap, not the game. `r4` in the same capture holds
that address too, passed as the argument it is.

So the capture is the moment the BIOS enters the bootstrap. We applied it and then jumped straight
to the game, skipping the bootstrap entirely. Every register was right for a moment that never
occurs in our boot, `r15` included.

## What the bootstrap actually leaves

Read out of a disc's own IP.BIN. Its entry does two things in its first four instructions: it
writes a cache-control value that turns the operand cache into addressable RAM, and it points the
stack there, at `0x7E001000`. That is the only stack address in the first stage.

That value is not what a game inherits. We do model the operand-cache window
(`0x7C000000`-`0x7FFFFFFF`), and booting Rayman 2 with the stack there still fails: the function
at `0x8C010D1A` reserves a 16 KB frame and the operand cache is 8 KB, so the stack wraps.

The second stage settles it. Just before it hands over it loads the stack twice from literals,
once through the uncached window and once through the cached one, and both hold **`0x8C00F400`**:

| Bootstrap address | Literal | Value |
| --- | --- | --- |
| `0x8C00E046` | `0x8C00E04C` | `0xAC00F400` |
| `0x8C00E0A0` | `0x8C00E0BC` | `0x8C00F400` |

Confirmed independently against a retail boot ROM (`KABUTO Ver.1.01d`): scanning every literal the
ROM loads into `r15` finds `0x8D000000` once, in the reset path, and the same `0x7E001000` the
bootstrap uses. `0x8C00F400` appears nowhere in the ROM, which is consistent with the bootstrap
rather than the BIOS being what sets a game's stack.

Neither the ROM nor any disc content is in this repository, and the `/bios/` directory is ignored
as a directory so that a dump saved under any name cannot be committed. Nothing above is copied
from either: these are addresses and values, described.

## Measured

| Boot `r15` | Rayman 2 | Crazy Taxi |
| --- | --- | --- |
| `0x8D000000` (was) | fault at frame 19, `pc 0x00100000` | 60 frames |
| `0x7E001000` | fault at frame 19, `pc 0xFFFFFFFF` | boots |
| `0x8CDF0000` | fault at frame 1502 | not run |
| `0x8C00F400` (now) | **898 frames in 15 s, no fault** | 60 frames, **write hash identical** |

The Crazy Taxi row is an A/B on one line of source: two builds, two runs, byte-identical write
hashes over 60 frames. An earlier attempt to measure this through an environment-variable override
reported the same answer for the wrong reason and cannot be relied on; the A/B replaced it.

## Why Rayman 2 cared and Crazy Taxi did not

Rayman 2's allocator hands out a 2 MB block based at `0x8CDFFFA0`, ending at `0x8CFFFFA0`, and
keeps the `0x60` bytes above that for an eleven-word memory-region descriptor at `0x8CFFFFC0`. One
field is a 1 MB size, `0x00100000`. Its entry point never sets `r15`, so with the stack at
`0x8D000000` its first frames sat inside those reserved bytes, and the size field landed on a
saved return address.

Four of four titles examined — Rayman 2, MSR and Tony Hawk's Pro Skater 2 share a byte-identical
entry stub, and Crazy Taxi's differs — inherit the boot stack rather than setting their own at
entry. Crazy Taxi moves its stack to `0x8C00F400` itself shortly afterwards, which is why it never
noticed.

## Still open

`sgr` is still `0x8D000000`, from the same capture and so from the same wrong instant, as are
`r0`-`r7`, `gbr`, `vbr` and `pr`. Only `r15` has been corrected, because only `r15` had a
demonstrated failure behind it. The rest are worth revisiting the same way — against the
bootstrap — rather than assumed correct.
