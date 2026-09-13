#!/usr/bin/env python3
"""Compare two context dumps (oracle vs recompiled). Exit 0 when they agree on every field that the
harness considers architectural: r0-r15, T, GBR, MACH/MACL, FPUL, FPSCR, FR/XF, and the memory hash.
SR's non-T bits and PR/VBR are reported but not compared (the harness sets them differently)."""
from __future__ import annotations

import json
import sys

COMPARE = ["r", "t", "gbr", "mach", "macl", "fpul", "fpscr", "fr", "xf", "mem"]


def diff(a: dict, b: dict) -> list[str]:
    diffs: list[str] = []
    for k in COMPARE:
        if k not in a or k not in b:
            continue
        if a[k] != b[k]:
            if isinstance(a[k], list):
                for i, (x, y) in enumerate(zip(a[k], b[k])):
                    if x != y:
                        diffs.append(f"{k}[{i}]: oracle {x} recomp {y}")
            else:
                diffs.append(f"{k}: oracle {a[k]} recomp {b[k]}")
    return diffs


def main() -> int:
    a = json.load(open(sys.argv[1]))
    b = json.load(open(sys.argv[2]))
    diffs = diff(a, b)
    if diffs:
        print("MISMATCH")
        for d in diffs[:40]:
            print("  " + d)
        return 1
    print(f"MATCH ({a.get('steps', '?')} oracle steps)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
