# Scheduler and interrupts (WP2.2)

## Time

`sched::Scheduler` is the virtual clock in SH-4 cycles (200 MHz) with a small event list. Devices
register an event once and re-arm it with a relative delay; `advance_to(t)` runs due events in
deadline order with `now()` set to each deadline. Nothing in the runtime reads a wall clock, so a run
is a deterministic function of the guest instruction stream.

Emitted code advances `Ctx::cycles` by its per-block estimates and polls
`if (c.cycles >= c.next_event) deliver_irq(c, m)` at every function entry and loop back-edge. The
poll costs one compare in the common case. `System::on_poll` runs the scheduler up to the guest clock
(devices raise their interrupts from their event callbacks), delivers whatever the CPU state lets
through, and sets `next_event` to the next deadline, or to `cycles` when a pending interrupt is
still masked. Writes to SR that change BL or IMASK set `next_event = 0` so the next poll re-evaluates.

## Interrupt delivery (cooperative, ADR 7)

Hardware interrupts are asynchronous; the recompiled program cannot be interrupted between two C++
statements, so delivery happens at the poll sites and the handler runs as a **nested guest call**:

1. `SSR = SR`, `SPC = PC`, `SGR = R15`; `SR.BL = MD = RB = 1` (bank swap through `write_sr`);
   `INTEVT` (CCN 0xFF000028) receives the code; the handler at `VBR + 0x600` is called through the
   function table.
2. The handler runs to its `RTE`, which restores SR from SSR (swapping banks back) and sets `PC =
   SPC`; the emitted `return` after `rte()` unwinds to the poll site, which is where the hardware
   would have resumed.
3. Nested interrupts work the same way if the handler lowers IMASK or clears BL and polls; depth is
   bounded by the priority levels. `System::max_nesting` records it.

TRAPA takes the same path with `TRA`, `EXPEVT = 0x160` and `VBR + 0x100`. An untranslated call
target is recorded in `System::untranslated` and skipped; WP2.7 puts the dev interpreter there.

The SPC value is the PC the emitted code last recorded (call sites), not the exact interrupted
instruction; handlers that only save and restore it are unaffected, and Katana's trampoline does
exactly that.

Task switches (2026-09-12): Katana's scheduler runs inside the VBlank handler. When the handler's
`rte` restores an SPC/r15 pair that is not the interrupted frame's, `System::on_rte` restores SR and
PC and throws `sh4::NonLocalReturn(SPC)` instead of unwinding: the delivery frame's bookkeeping
unwinds through `enter_exception`, the suspended task's host frames are dropped, and
`sh4::run_guest` re-enters the resumed task at SPC. Emitted poll points store their exact pc before
`deliver_irq` so that SPC is a label the function can be re-entered at. The report counts these as
"task-switching RTEs". Details: `docs/emitter-design.md`, "Non-local returns".

## SH-4 INTC (`sh4::Intc`, 0xFFD00000)

Sources present on a Dreamcast with their INTEVT codes: IRL9/11/13 (0x320/0x360/0x3A0, fixed
priorities 6/4/2), TMU0–2 (0x400/0x420/0x440, IPRA), DMAC (0x640–0x6C0, IPRC), SCIF (0x700–0x760,
IPRC). Selection: highest priority pending source with priority > SR.IMASK while SR.BL is clear; a
priority of 0 never delivers. ICR/IPRA/IPRB/IPRC are 16-bit registers.

## TMU (`sh4::Tmu`, 0xFFD80000)

Flycast's model (GPL-2.0, ADR 1): the counter is `base - (now >> shift)` while running, so reads are
exact and only the underflow needs an event. TPSC 0–4 divide the peripheral clock (CPU/4) by 4, 16,
64, 256, 1024. On underflow UNF is set, the interrupt is pending when UNIE is set, and TCNT reloads
from TCOR on that tick (the SH7750 manual's wording; Flycast counts one tick further, a difference no
game can observe). Writing 0 to UNF clears it.

## Holly interrupt controller (`holly::Intc`, 0x005F6900)

SB_ISTNRM/ISTEXT/ISTERR collect the sources; the level-6 masks route to IRL9, level-4 to IRL11,
level-2 to IRL13. ISTNRM reads with bits 30/31 summarising the external and error registers; ISTNRM
and ISTERR are write-1-to-clear; external sources are cleared by their device. Source numbering is
Flycast's `HollyInterruptID` (Katana's `SB_ISTNRM` bit assignments).

## SPG (`pvr::Spg`, PVR + 0xC8..0xE0, SPG_STATUS at + 0x10C)

One scheduler event per scanline, `line_cycles = 200 MHz × (hcount+1) / pixel clock`, pixel clock
13.5 MHz unless FB_R_CTRL selects 27 MHz (WP2.3 will drive `set_vclk_div`). Defaults are the Katana
NTSC values: 858 × 263 → 12,711 cycles per line, 59.83 Hz. VBlank-in and VBlank-out fire on the
lines SPG_VBLANK_INT names, HBlank per SPG_HBLANK_INT's mode, and SPG_STATUS reports the scanline,
field and vsync (SPG_VBLANK vstart/vbend). Register field layouts follow Flycast's `pvr_regs.h`
(`SPG_LOAD`: hcount 9:0, vcount 25:16).

## Not done from the plan's wording

- Idle-loop fast-forward: needs the WP1.4 idle-loop tags, which do not exist yet. When a game spins
  on SPG_STATUS the runtime will run the loop at full speed; measure on Crazy Taxi before building.
- The interrupt-latency test with a KOS TMU program: needs a program loader and SCIF output
  (WP2.6/2.7). The unit test drives the same path with C++ stand-ins registered at VBR+0x600.
