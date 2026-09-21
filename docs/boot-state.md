# The boot stack pointer

`Bios::setup_boot` hands a title the register state a real console would have left, and starts
`r15` at `0x8D000000`, the top of the 16 MB. That value matches Flycast's `reios_setup_state`, and
it is wrong for at least one title.

## What Rayman 2 showed

Rayman 2 dies on frame 19 returning to `0x00100000`. The cause is not the emitter. The game's
allocator hands out a 2 MB block based at `0x8CDFFFA0`, ending at `0x8CFFFFA0`, and keeps the
0x60 bytes above that for an eleven-word region descriptor at `0x8CFFFFC0`. One field of that
descriptor is a 1 MB size, `0x00100000`, written by `mov.l r2,@(16,r14)` at `0x8C05A8F6` — a
literal from the pool at `0x8C05A980`, not a stray value.

`0x8CFFFFD0` is both that size field and, in our runs, the saved `PR` of a function still running
on the initial stack. Rayman 2's entry point at `0x8C010000` never sets `r15`, so it inherits
whatever the boot state gives it, and `0x8D000000` puts the first frames exactly where the game
reserves its descriptor.

Crazy Taxi is unaffected because it moves its own stack to about `0x8C00F400` early on.

## What the real bootstrap does

Rayman 2's IP.BIN, disassembled at its entry:

```
8c008300  mov.l 0x8c008320,r0    ; r0 = 0xFF000000
8c008302  mov.l 0x8c008324,r1    ; r1 = 0x0000092B
8c008304  mov.l r1,@(28,r0)      ; CCR at 0xFF00001C: cache as RAM
8c008306  mov.l 0x8c00831c,r15   ; r15 = 0x7E001000
```

So the first thing a real boot does is enable the operand cache as RAM and put the stack in it at
`0x7E001000` — not in main RAM at all. It is the only literal stack load in the whole 32 KB.

That is not directly usable as our boot value: we model the region (`0x7C000000`-`0x7FFFFFFF`,
`ocram_`), and booting Rayman 2 there still fails, because the function at `0x8C010D1A` subtracts
a 16 KB frame and the operand cache is 8 KB. IP.BIN's second stage must move the stack into main
RAM before `1ST_READ.BIN` runs; three `mov r0,r15` sites at `0x8C00B802`, `0x8C00E046` and
`0x8C00E0A0` are the candidates, and none has been read yet.

## Measured

| Boot `r15` | Rayman 2 | Crazy Taxi |
| --- | --- | --- |
| `0x8D000000` (current) | fault at frame 19, `pc 0x00100000` | 60 frames, baseline |
| `0x7E001000` (IP.BIN's) | fault at frame 19, `pc 0xFFFFFFFF` | boots |
| `0x8CDF0000` | fault at frame 1502 | not run |
| `0x8C00F400` | **1797 frames, no fault** | 60 frames, **write hash byte-identical** |

Crazy Taxi is byte-identical under `0x8C00F400` because it never uses the boot stack. That is one
title's worth of evidence that the change is inert, not a corpus result: no other title in the
repository has a config that runs.

## Open

`0x8C00F400` is a value that works, not a value we have justified. Before it becomes the default,
one of these should land:

1. **Read IP.BIN's second stage** and find the `r15` it leaves for `1ST_READ.BIN`. That is the
   authoritative answer and the disassembly is sitting there.
2. **Run IP.BIN** rather than emulating its end state. We already load all 32 KB to `0x8C008000`
   and then jump past it to the game.
3. If neither is practical, make it a config key and default it to today's `0x8D000000`, which at
   least keeps the choice visible per title rather than silently wrong.

Whichever it is, this is a runtime-level fix. Nothing about it belongs in a game's TOML except as
a last resort.
