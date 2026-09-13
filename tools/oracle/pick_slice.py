#!/usr/bin/env python3
"""Pick leaf functions from an emitted game unit for the differential harness (docs/differential-harness.md).

  pick_slice.py --emitted games/crazytaxi/cache/emit/crazytaxi.cpp --functions games/crazytaxi/cache/functions_dream.json
                [--writes | --pure] [--fpu-min N] [--max-lines N] [--top N] [--exclude ENTRY]...

Prints candidates as `entry fpu writes reads lines`. A leaf here is a function whose emitted body has no
calls (direct or indirect), no trap/unimplemented word, no switch, no GBR/SR/VBR use and no folded
absolute memory address (a global the harness cannot seed on both sides). --pure keeps only functions
without memory writes; --writes only those with writes (run them with a fill region and a hash).
Functions that read globals through pointers in zeroed RAM or loop on data still have to be pruned by
hand after a first run; the harness reports them as runner faults or step-limit hits."""
from __future__ import annotations

import argparse
import json
import re
import sys


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--emitted", required=True)
    ap.add_argument("--functions", required=True)
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--pure", action="store_true", help="no memory writes")
    g.add_argument("--writes", action="store_true", help="at least one memory write")
    ap.add_argument("--fpu-min", type=int, default=0, help="minimum fr[] references (0 = integer code allowed)")
    ap.add_argument("--fpu-only", action="store_true", help="require fpu-min and sort by FPU use")
    ap.add_argument("--max-lines", type=int, default=220)
    ap.add_argument("--min-lines", type=int, default=8)
    ap.add_argument("--top", type=int, default=20)
    ap.add_argument("--exclude", action="append", default=[], type=lambda x: int(x, 0))
    a = ap.parse_args()

    src = open(a.emitted).read()
    fns = json.load(open(a.functions))
    fns = fns["functions"] if isinstance(fns, dict) else fns
    by = {int(f["entry"], 0): f for f in fns}
    out = []
    for dm in re.finditer(r"^void fn_([0-9a-f]+)\(Ctx& c, Memory& m\) \{\n", src, re.M):
        entry = int(dm.group(1), 16)
        if entry in a.exclude or entry not in by:
            continue
        body = src[dm.end():src.find("\n}\n", dm.end())]
        if ("call_indirect(" in body or re.search(r"fn_[0-9a-f]+\(c, m\)", body) or "unimplemented(" in body
                or "trapa(" in body or "switch (" in body or "c.gbr" in body or "write_sr" in body or "c.vbr" in body):
            continue
        if re.search(r"m\.(read|write)\d+\(0x[0-9a-fA-F]+u?[,)]", body):
            continue
        lines, writes, reads, fpu = body.count("\n"), body.count("m.write"), body.count("m.read"), body.count("c.fr[")
        if not (a.min_lines <= lines <= a.max_lines) or fpu < a.fpu_min:
            continue
        if a.pure and writes or a.writes and not writes:
            continue
        out.append((fpu, writes, reads, lines, entry))
    out.sort(key=(lambda t: (-t[0], t[3])) if a.fpu_only else (lambda t: (-(t[1] + t[2]), t[3])))
    for fpu, w, r, l, e in out[: a.top]:
        f = by[e]
        print(f"{f['entry']}:{f['end']}  fpu={fpu:3d} writes={w:3d} reads={r:3d} lines={l}")
    print(f"{len(out)} candidates", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
