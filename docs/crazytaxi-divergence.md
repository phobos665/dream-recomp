# Crazy Taxi: why translated and interpreted runs disagree

Closed 2026-09-21. The answer is not an emitter defect. Two of the three differences were
artefacts of how the comparison was run, and the third is a structural limit of using the
development interpreter as an oracle.

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

### 3. Write 2,824,621: a call into IP.BIN

The first genuine difference. Writes `0` to `2,824,620` agree exactly, and `pr` — which is
maintained on both paths — first differs at the same index, so control flow split in the
instructions just before it, with no store in between.

```
translated : 2824621 0c00f3dc 4 ...0c02a2de             pr 0c02a2de
interpreted: 2824621 0c00f3dc 4 ...1fffff0f  pc 8c00fa26  pr 0c02a2ce
```

Guest memory at `0x8C00FA26` holds `0x2F16`, `mov.l r1,@-r15`, which is exactly the store logged,
so the interpreter is executing real code there. **That address is inside `0x8C008000`–`0x8C010000`,
the 32 KB of IP.BIN the HLE loads at boot** — before any instrument is armed, which is why no
write into it appears in the log and why an earlier pass over the log wrongly concluded the
region was untouched.

Two further facts place it:

- The guest never rewrote the BIOS syscall vectors at `0x8C0000B0`–`0x8C0000C4`, so this is not a
  vector the game redirected.
- The translated run reports **0 untranslated call targets**. It never calls into IP.BIN at all.

So the interpreter follows a call into IP.BIN's own code, and the translated build, whose BIOS
calls are intercepted by the Katana HLE, does not.

## What this means

**We do not translate IP.BIN.** The translator's image is `1ST_READ.BIN`; the 32 KB loaded at
`0x8C008000` is never lifted. A title that calls into it has code the translated build reaches by
a different route — the HLE — and the two routes do different work and write different things.

So `--interpret` is **not a whole-run oracle** for a title that calls IP.BIN. Past the first such
call the two paths are running different programs, and comparing their write hashes measures that,
not the emitter. This is a limit of the method, not a defect to fix.

What remains a valid signal:

- The write hash comparing **two runs of the same path** — the regression gate. Unaffected.
- `--replay`, which compares each translated function against the interpreter *for that function
  only*: 153,450,465 calls, 0 disagreements. That never leaves the function, so it never crosses
  into IP.BIN.
- Translated against interpreted **up to the first IP.BIN call**, which for Crazy Taxi is
  2,824,621 writes — most of the boot.

## Method note

Use `--mask-segment` on every translated-against-interpreted comparison. Then, when hashes differ,
dump the window with `--write-log FILE --write-log-range FROM:COUNT` and compare on index,
address, size and value; `pc` is exact for the interpreter but stale in translated code, and `pr`
is meaningful in both, which makes `pr` the better column for finding where control flow split.
See [write-coverage.md](write-coverage.md).
