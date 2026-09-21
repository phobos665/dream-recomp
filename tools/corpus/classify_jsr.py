#!/usr/bin/env python3
"""Why can't we resolve these indirect calls?

`discover` reports the sites it gave up on. This asks what defines the register at each one, which
is the difference between a heuristic that would help and one that would not:

  pc_literal  the pointer came from a literal pool -- statically knowable, we simply missed it
  mem_load    read out of a struct or a vtable -- genuinely dynamic, no static pass can resolve it
  reg_copy    copied from another register we did not trace back far enough
  argument    the callee was passed in, so the caller decides -- resolvable only per call site
  none        no definition within the search window

Run after tools/corpus/scan.py has cached the extracted binaries.
"""
from __future__ import annotations

import json
import pathlib
import subprocess
import sys
import tempfile
from collections import Counter

REPO = pathlib.Path(__file__).resolve().parents[2]
TRANSLATE = REPO / "build" / "translator" / "dream-translate"
CACHE = pathlib.Path(tempfile.gettempdir()) / "dream-corpus-cache"
WINDOW = 200  # see the note in docs/corpus-scan-findings.md: the result is window-sensitive,
              # because this walks back linearly with no idea where basic blocks begin


def w16(img: bytes, off: int) -> int:
    return img[off] | (img[off + 1] << 8) if 0 <= off + 1 < len(img) else -1


def defines(word: int, reg: int) -> str | None:
    """How `word` defines `reg`, or None if it does not."""
    if word < 0:
        return None
    n = (word >> 8) & 0xF
    if (word & 0xF000) == 0xD000 and n == reg:          # mov.l @(disp,PC),Rn
        return "pc_literal"
    if (word & 0xF000) == 0x5000 and n == reg:          # mov.l @(disp,Rm),Rn
        return "mem_load"
    if (word & 0xF00F) == 0x6002 and n == reg:          # mov.l @Rm,Rn
        return "mem_load"
    if (word & 0xF00F) == 0x6006 and n == reg:          # mov.l @Rm+,Rn
        return "mem_load"
    if (word & 0xF00F) == 0x6003 and n == reg:          # mov Rm,Rn
        return "reg_copy"
    if (word & 0xF000) == 0xE000 and n == reg:          # mov #imm,Rn
        return "immediate"
    if (word & 0xF00F) == 0x300C and n == reg:          # add Rm,Rn
        return "arith"
    if (word & 0xF000) == 0x7000 and n == reg:          # add #imm,Rn
        return "arith"
    return None


def classify(img: bytes, base: int, addr: int, fn_entry: int) -> str:
    off = addr - base
    word = w16(img, off)
    reg = (word >> 8) & 0xF
    # Walk back, stopping at the function entry: a definition before it belongs to someone else.
    for back in range(2, WINDOW * 2 + 2, 2):
        a = addr - back
        if a < fn_entry:
            break
        kind = defines(w16(img, a - base), reg)
        if kind:
            return kind
    return "argument" if reg in (4, 5, 6, 7) else "none"


def main() -> int:
    if not CACHE.exists():
        print("run tools/corpus/scan.py first to cache the binaries", file=sys.stderr)
        return 2
    grand: Counter[str] = Counter()
    print(f"{'title':26}{'JSR':>7}{'pc_lit':>8}{'mem':>7}{'copy':>7}{'arg':>6}{'other':>7}")
    print("-" * 68)
    for d in sorted(CACHE.iterdir()):
        cands = list(d.rglob("1ST_READ.BIN")) + list(d.rglob("1st_read.bin"))
        if not cands:
            continue
        proc = subprocess.run(
            [str(TRANSLATE), "discover", "--image", str(cands[0]), "--base", "0x0C010000",
             "--entry", "0x0C010000"], capture_output=True, text=True, timeout=900)
        if proc.returncode:
            continue
        j = json.loads(proc.stdout)
        base = int(j["image_base"], 16)
        img = cands[0].read_bytes()
        c: Counter[str] = Counter()
        for u in j["unresolved_indirect"]:
            if u["op"] != "JSR":
                continue
            c[classify(img, base, int(u["address"], 16), int(u["in_function"], 16))] += 1
        grand.update(c)
        tot = sum(c.values())
        other = tot - c["pc_literal"] - c["mem_load"] - c["reg_copy"] - c["argument"]
        print(f"{d.name[:25]:26}{tot:>7}{c['pc_literal']:>8}{c['mem_load']:>7}"
              f"{c['reg_copy']:>7}{c['argument']:>6}{other:>7}")
    print("-" * 68)
    tot = sum(grand.values())
    for k, v in grand.most_common():
        print(f"  {k:12} {v:>7}  {100 * v // max(tot, 1):>3}%")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
