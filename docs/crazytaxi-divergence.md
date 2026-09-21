# Crazy Taxi: why translated and interpreted runs disagree

Two of the three differences are artefacts of how the comparison was run. The third is open: the
two paths select different overlay bodies at `0x8C00FA20`, and which of them is correct is not
established.

**Revised 2026-09-21.** A first version of this closed the whole question by attributing the third
difference to IP.BIN code we never translate. That was wrong — see below.

## The three differences, in the order they were peeled off

### 1. Frame 1: segment bits in stored pointers

Without `--mask-segment`, frame 1 shows different hashes at an identical cycle count and an
identical write count. With it, frames 0 to 3 are byte-identical.

A stored pointer carries the P1/P2 segment the code was running in, and a statically translated
build cannot reproduce that; the runtime masks the segment off every access anyway. This is the
known false-difference class `--mask-segment` exists for, and the comparison had simply been run
without it.

It is visible in the hash itself once you know to look. With `h = (h ^ value) * prime`, a
difference confined to the top bits of a stored *value* leaves the low bits of the hash equal:

```
frame 1 translated  4a879c6f 98e97588
frame 1 interpreted 830a5ed0 98e97588
```

Identical low halves. Two hashes that differ only in their top half are this, not a divergence.

### 2. Frame 4: the sampling boundary

With `--mask-segment` the first difference moves to frame 4, where the write counts differ by one
(`928650` against `928651`) at an identical cycle count. Dumping writes `824000` to `933999` from
both paths gives **zero** differences in address, size and value.

The write stream is identical; the frame boundary lands one write later. The hash is sampled per
frame, not per write index, so the two runs are being compared at different points in the same
sequence. The tell is a write count differing by one or two with no differing write anywhere
around it.

### 3. Write 2,824,621: the paths pick different overlay bodies

The first genuine difference. Writes `0` to `2,824,620` agree exactly, and `pr` — which is
maintained on both paths — first differs at the same index, so control flow split in the
instructions just before it, with no store in between.

```
translated : 2824621 0c00f3dc 4 ...0c02a2de             pr 0c02a2de
interpreted: 2824621 0c00f3dc 4 ...1fffff0f  pc 8c00fa26  pr 0c02a2ce
```

Guest memory at `0x8C00FA26` holds `0x2F16`, `mov.l r1,@-r15`, which is exactly the store logged,
so the interpreter is executing real code there.

**That code is a declared overlay.** `games/crazytaxi/crazytaxi.toml` carries:

```toml
[[relocations]]
source = 0x0C16BBB8
size = 0x58
dest = 0x8C00FA00
entries = [0x8C00FA00, 0x8C00FA20]
overlay = true
```

`0x8C00FA26` is six bytes into the second entry. The write log shows the body being copied in at
write index 721,033 from `pc 0x0C15EB94`, and the bytes match the memory dump once
`--mask-segment`'s top-three-bit masking is undone (`0x0F164F26` logged, `0x2F164F26` in memory).

So at the divergence the interpreter is executing an overlay variant and the translated build is
not: its `pr` is `0x0C02A2DE` against the interpreter's `0x0C02A2CE`, and it reports **0
untranslated call targets**, so it has code for that address and dispatched somewhere else.

## What is and is not established

Established: the two paths disagree about which overlay body to run, at a call whose target is
disambiguated at run time by a signature read from guest memory.

**Not established: which one is right.** Overlay dispatch is emitter and runtime machinery, so
this is not evidence that the emitter is clean — an earlier revision of this document said it was,
on the mistaken grounds that `0x8C00FA26` was IP.BIN code we never translate. That was wrong: the
address is inside the IP.BIN load window numerically, but the bytes there are game code copied
over it at run time, and we do translate them.

It remains true that we never translate IP.BIN itself, and that is worth knowing for a title that
calls into it. It is not what happens here.

The two artefacts in section 1 and 2 above are unaffected: they were measured, not inferred.

## Next on this

Which overlay variant each path selects at `0x8C00FA20`, and why they differ. The signature read
at call time is the thing to instrument.

## Method note

Use `--mask-segment` on every translated-against-interpreted comparison. Then, when hashes differ,
dump the window with `--write-log FILE --write-log-range FROM:COUNT` and compare on index,
address, size and value; `pc` is exact for the interpreter but stale in translated code, and `pr`
is meaningful in both, which makes `pr` the better column for finding where control flow split.
See [write-coverage.md](write-coverage.md).
