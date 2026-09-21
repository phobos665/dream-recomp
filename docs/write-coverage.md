# What the write hash and the write watch can see

`--write-hash` and `DREAM_WATCH_WRITE` are the two instruments a bring-up leans on hardest, and
until 2026-09-21 both were wired into one write path out of four. This records what they cover,
because a gap here does not look like a gap: it looks like two runs agreeing.

## The hooks

Everything that writes guest RAM must report through `DcMemory::note_ram_write`, which feeds both
the rolling hash and the watch. `tracing_writes()` says whether either is armed, for paths that
want to skip per-word work when neither is.

| Path | Covered | Note |
| --- | --- | --- |
| `DcMemory::store<T>` | yes | The ordinary 8/16/32-bit store. Always was. |
| `DcMemory::write64` | yes, since 2026-09-21 | Reported as two 4-byte writes, matching the split the device path already used. It also had no journal entries, so `undo_journal` could not put it back; that is fixed too. Only tests call it today. |
| `DcMemory::sq_flush` into RAM | yes, since 2026-09-21 | Reported as eight 4-byte writes. A burst into a device is a device write and is not reported. |
| `Bios::write_sector` into RAM | yes, since 2026-09-21 | It has a memcpy fast path and a `write32` slow path; with an instrument armed it now takes the slow one, so a sector landing in RAM is not invisible while the same sector landing elsewhere is visible. |
| G2 DMA, SH-4 DMAC | yes | Both already went through `memory_.write32`. |
| Device writes (`kMmio`) | **no** | By construction: the hash is over memory state. A divergence that lives only in device traffic will not show up here. |
| `Bios::setup_boot`'s 64 KB `memset` | **no** | Runs before any instrument is armed, and is identical in every run. |

## Reading a hash difference

Two things produce a difference that is not a defect.

**Segment bits.** A stored pointer carries the P1/P2 segment the code was running in, and a
statically translated build cannot reproduce that. `--mask-segment` removes the class. **Always
compare translated against interpreted with `--mask-segment`**; without it Crazy Taxi appears to
diverge at frame 1, and does not.

The signature is worth knowing: with a hash of the form `h = (h ^ value) * prime`, a difference
confined to the high bits of a stored *value* leaves the low bits of the hash equal. Two hashes
that differ only in their top half are almost certainly this and not a real divergence.

**The sampling boundary.** The hash is sampled per frame, so it compares two runs at a frame
boundary rather than at a write index. The interpreter and translated code can attribute cycles
slightly differently, which puts the boundary between two different writes of an otherwise
identical stream. The tell is a write count that differs by one or two with no differing write in
the surrounding window.

Neither is visible in the hash alone. A hash difference is a reason to dump the window, not a
conclusion.

## Finding the first divergent write

`--write-log FILE --write-log-range FROM:COUNT` dumps `index addr size value pc pr` for a window
of the write sequence, from either path. Compare on the first four fields only: `pc` is the
interpreter's exact PC but in translated code `sys.ctx.pc` is whatever the last write to it left,
so the two never match and the column is noise on that side. `pr` is meaningful in both.

Binary search on `FROM` until the window straddles the first difference, or dump one large window
and stream-compare. The first differing line is the answer, and the interpreter's `pc` on that
line says where it was.

## `trace_return`'s `r15` is post-delay-slot

`DREAM_TRACE_RETURNS` logs from the interpreter's `Flow::Ret` case, which runs after the delay
slot. The branch's operands — target, condition, `PR` for calls — are read *before* the slot, but
the registers in the logged line are read *after* it. An `rts` whose delay slot pops a register
therefore shows an `r15` one slot further on than the instruction listing suggests.

This cost real time on Rayman 2: reading `pr` from `r15 - 8` instead of `r15 - 12` put it on the
wrong stack word and produced an apparent contradiction between the return trace and the write
watch that stood for several commits. Both were correct.

When recovering stack slots from a return trace, check **every** register the epilogue restores,
not one. An off-by-one slot still matches the register next to it, so a single agreeing value
confirms nothing; the full set only lines up at the right offset.
