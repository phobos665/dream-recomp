#!/usr/bin/env python3
"""Compiler-idiom statistics over a translated function set (WP1.6, docs/compiler-idioms.md).

  idioms.py --emitted build/ct/crazytaxi.cpp [--top 25]

Reads the instruction comments (`// 0xADDR: mnemonic operands`) of a unit emitted by dream-translate,
so only instructions the translator actually walked count (literal-pool words inside function ranges
do not), and reports the shapes the translator must handle: opcode mix, prologue/epilogue patterns,
call and return idioms, delay-slot usage, literal-pool and GBR addressing, FPU mode changes,
computed jumps and function sizes."""
from __future__ import annotations

import argparse
import collections
import json
import re


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--emitted", required=True)
    ap.add_argument("--top", type=int, default=20)
    a = ap.parse_args()

    # One function per `// name: guest 0xA..0xB` header; instructions as `// 0xADDR: text`
    # (delay slots carry a "(delay slot)" suffix, dropped here).
    fns = []
    cur = None
    head = re.compile(r"^// (\S+): guest (0x[0-9a-f]+)\.\.(0x[0-9a-f]+)$")
    insn = re.compile(r"^\s*// (0x[0-9a-f]{8}): (.*?)(?: \(delay slot\))?$")
    for line in open(a.emitted):
        m = head.match(line)
        if m:
            cur = {"entry": m.group(2), "end": m.group(3), "ins": {}}
            fns.append(cur)
            continue
        m = insn.match(line)
        if m and cur is not None and not m.group(2).startswith("block "):
            cur["ins"][int(m.group(1), 16)] = m.group(2)

    mn = collections.Counter()
    prologues = collections.Counter()
    epilogues = collections.Counter()
    calls = collections.Counter()
    slots = collections.Counter()
    jumps = collections.Counter()
    sizes = []
    lit = collections.Counter()
    fpu = collections.Counter()
    misc = collections.Counter()
    total = 0
    branch_ops = {"bra", "bsr", "bt.s", "bf.s", "jmp", "jsr", "rts", "braf", "bsrf", "rte"}
    for f in fns:
        e, end = int(f["entry"], 0), int(f["end"], 0)
        sizes.append(end - e)
        body = sorted(f["ins"].items())
        mnems = [t.split()[0] for _, t in body]
        total += len(body)
        mn.update(mnems)
        prologues[" ; ".join(t for _, t in body[:3])] += 1
        # last control transfer with its slot
        for i in range(len(body) - 1, -1, -1):
            if mnems[i] in ("rts", "jmp", "rte", "bra"):
                epilogues[" ; ".join(t for _, t in body[max(0, i - 2): i + 2])] += 1
                break
        prev = None
        for i, (pc, t) in enumerate(body):
            m = mnems[i]
            if m in branch_ops:
                slot = mnems[i + 1] if i + 1 < len(body) else "?"
                slots["nop" if slot == "nop" else "useful"] += 1
            if m == "bsr":
                calls["bsr (direct)"] += 1
            elif m == "jsr":
                calls["jsr @rN after mov.l @(disp,pc)" if prev and prev.startswith("mov.l 0x") else "jsr @rN (other)"] += 1
            elif m == "bsrf":
                calls["bsrf"] += 1
            if m in ("jmp", "braf"):
                jumps[m + (" (table via mova)" if any(x.startswith("mova") for _, x in body[max(0, i - 6): i]) else "")] += 1
            if m in ("mov.l", "mov.w") and re.match(r"mov\.[lw] 0x", t):
                lit[m + " @(disp,pc)"] += 1
            if "@(" in t and ",gbr)" in t:
                lit["gbr-relative"] += 1
            if m == "mova":
                lit["mova"] += 1
            if m in ("fschg", "frchg"):
                fpu[m] += 1
            if m == "lds" and t.endswith("fpscr"):
                fpu["lds rN,fpscr"] += 1
            if m == "lds.l" and t.endswith("fpscr"):
                fpu["lds.l @rN+,fpscr"] += 1
            if m.startswith("f") and m not in ("fschg", "frchg"):
                fpu["fpu instructions"] += 1
            if m in ("trapa", "rte", "sleep", "pref", "ocbwb", "ocbp", "ocbi", "movca.l", "ldtlb", "clrmac", "div0s", "div0u", "div1", "mac.l", "mac.w", "dmuls.l", "dmulu.l", "sett", "clrt", "tas.b"):
                misc[m] += 1
            prev = t

    def show(title, counter, top=a.top, total_n=None):
        print(f"\n## {title}")
        for k, n in counter.most_common(top):
            pct = f" ({100.0 * n / total_n:.1f}%)" if total_n else ""
            print(f"{n:8d}{pct}  {k}")

    sizes.sort()
    print(f"functions {len(fns)}, walked instructions {total}, unit {a.emitted}")
    print(f"function size bytes: median {sizes[len(sizes)//2]}, p90 {sizes[int(len(sizes)*0.9)]}, max {sizes[-1]}")
    show("opcode mix", mn, total_n=total)
    show("prologues (first three instructions)", prologues, 12)
    show("epilogues (around the return)", epilogues, 12)
    show("call idioms", calls)
    show("delay slots after control transfers", slots)
    show("computed jumps", jumps)
    show("literal pools and GBR", lit)
    show("FPU", fpu)
    show("notable instructions", misc)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
