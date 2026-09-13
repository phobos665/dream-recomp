#!/usr/bin/env python3
"""Diff two guest return traces (DREAM_TRACE_RETURNS) to the first real divergence.

    DREAM_TRACE_RETURNS=/tmp/t.log crazytaxi_boot --config ... --max-frames 120
    DREAM_TRACE_RETURNS=/tmp/i.log crazytaxi_boot --config ... --max-frames 120 --interpret
    python3 tools/trace/diff_returns.py /tmp/t.log /tmp/i.log

Each line is "rts_pc r0..r15 pr sr" in hex. RAM mirror bits are masked (translated code folds
addresses to the 0x0C alias, the interpreter keeps the guest's 0xAC values), runs of identical
lines are collapsed (polling loops), and a small window is used to resynchronise after a timing
shift (an interrupt landing an iteration earlier in one run) so only a divergence that does not
realign is reported. See docs/runtime-devinterp.md.
"""
import argparse
import sys

COLS = ['rtspc'] + ['r%d' % i for i in range(16)] + ['pr', 'sr']


def norm(line):
    out = []
    for tok in line.split():
        v = int(tok, 16)
        if (v & 0x1C000000) == 0x0C000000:
            v &= 0x1FFFFFFF
        out.append('%08x' % v)
    return ' '.join(out)


def load(path):
    seq, last = [], None
    with open(path) as f:
        for line in f:
            n = norm(line)
            if n != last:
                seq.append(n)
                last = n
    return seq


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('translated')
    ap.add_argument('interpreted')
    ap.add_argument('--window', type=int, default=300, help='resync window in collapsed lines')
    ap.add_argument('--context', type=int, default=4)
    args = ap.parse_args()
    a, b = load(args.translated), load(args.interpreted)
    print('collapsed returns: %s %d, %s %d' % (args.translated, len(a), args.interpreted, len(b)))
    i = j = shifts = 0
    W = args.window
    while i < len(a) and j < len(b):
        if a[i] == b[j]:
            i += 1
            j += 1
            continue
        found = None
        for di in range(W):
            for dj in range(W):
                if (di or dj) and i + di + 1 < len(a) and j + dj + 1 < len(b) \
                        and a[i + di] == b[j + dj] and a[i + di + 1] == b[j + dj + 1]:
                    found = (di, dj)
                    break
            if found:
                break
        if found:
            shifts += 1
            i += found[0]
            j += found[1]
            continue
        print('divergence at line #%d of the first trace, #%d of the second (after %d timing shifts)' % (i, j, shifts))
        print('    ' + ' '.join('%-8s' % c for c in COLS))
        for k in range(max(0, i - args.context), i):
            print('A ', a[k])
        print('A*', a[i])
        for k in range(i + 1, min(len(a), i + args.context + 1)):
            print('A+', a[k])
        print()
        for k in range(max(0, j - args.context), j):
            print('B ', b[k])
        print('B*', b[j])
        for k in range(j + 1, min(len(b), j + args.context + 1)):
            print('B+', b[k])
        ta, tb = a[i].split(), b[j].split()
        print('differing columns:', [COLS[k] for k in range(len(ta)) if k < len(tb) and ta[k] != tb[k]])
        return 1
    print('aligned to the end of the shorter trace (%d timing shifts); first trace %d/%d, second %d/%d'
          % (shifts, i, len(a), j, len(b)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
