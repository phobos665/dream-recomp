#!/usr/bin/env python3
"""Run discovery across several titles and report where it falls short.

One title tells you whether discovery works. Several tell you which idioms it does not know, which
is a different and more useful question: a heuristic worth writing is one that several binaries
need, and a hole that appears in one title is usually a hole in the others too.

    python3 tools/corpus/scan.py games/*.chd
    python3 tools/corpus/scan.py --json out.json games/*.chd

Needs `dream-translate` built and `dcdisc` importable. Extracted binaries are cached under the
scratch directory; nothing from a disc is written into the repository.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys
from collections import Counter

REPO = pathlib.Path(__file__).resolve().parents[2]
TRANSLATE = REPO / "build" / "translator" / "dream-translate"

# SH-4 instructions that transfer control through a register. Discovery can only follow one when
# something resolves the register, so each unresolved site is a place the emitted code may have no
# entry for -- which at run time is an untranslated target or a failed resume.
def decode_indirect_sites(image: bytes, base: int,
                          spans: list[tuple[int, int]]) -> dict[str, list[int]]:
    """Every jmp/jsr through a register, and every braf/bsrf, inside discovered code.

    Restricted to the ranges discovery claimed. Scanning the whole image instead counts every data
    word that happens to look like an opcode -- on Crazy Taxi that is eleven thousand false
    positives against a few hundred real ones, which makes the ratio meaningless.
    """
    sites: dict[str, list[int]] = {"jmp_reg": [], "jsr_reg": [], "braf": [], "bsrf": []}
    for lo_addr, hi_addr in spans:
        for addr in range(lo_addr, hi_addr - 1, 2):
            off = addr - base
            if off < 0 or off + 1 >= len(image):
                continue
            w = image[off] | (image[off + 1] << 8)
            # Masks from translator/src/sh4/decoder.cpp. BRAF and BSRF are high-nibble 0, not 4 --
            # getting that wrong counts unrelated 0x4nXX instructions and makes every computed jump
            # look unresolved.
            form = w & 0xF0FF
            if form == 0x402B:
                sites["jmp_reg"].append(addr)
            elif form == 0x400B:
                sites["jsr_reg"].append(addr)
            elif form == 0x0023:
                sites["braf"].append(addr)
            elif form == 0x0003:
                sites["bsrf"].append(addr)
    return sites


def gaps(functions: list[dict], base: int, size: int) -> list[tuple[int, int]]:
    """Runs of the image no function claims. Candidate discovery holes."""
    spans = sorted((int(f["entry"], 16), int(f["end"], 16)) for f in functions)
    merged: list[list[int]] = []
    for s, e in spans:
        if merged and s <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], e)
        else:
            merged.append([s, e])
    out, cursor = [], base
    for s, e in merged:
        if s > cursor:
            out.append((cursor, s))
        cursor = max(cursor, e)
    if cursor < base + size:
        out.append((cursor, base + size))
    return out


def scan_one(name: str, binary: pathlib.Path, base: int, entry: int) -> dict:
    proc = subprocess.run(
        [str(TRANSLATE), "discover", "--image", str(binary), "--base", hex(base),
         "--entry", hex(entry)],
        capture_output=True, text=True, timeout=600)
    if proc.returncode != 0:
        return {"title": name, "error": (proc.stderr or "discover failed").strip().split("\n")[-1]}
    d = json.loads(proc.stdout)
    image = binary.read_bytes()
    # image_base is emitted as a hex string, everything else as an integer.
    img_base = int(d["image_base"], 16) if isinstance(d["image_base"], str) else d["image_base"]
    size = d["image_size"]
    fns = d["functions"]
    holes = gaps(fns, img_base, size)
    # Merge before summing. Discovery keeps overlapping ranges, so adding each function's length
    # double-counts the shared parts and can exceed the image -- Rayman 2 reported 101.5% coverage
    # before this.
    _m: list[list[int]] = []
    for _s, _e in sorted((int(f["entry"], 16), int(f["end"], 16)) for f in fns):
        if _m and _s <= _m[-1][1]:
            _m[-1][1] = max(_m[-1][1], _e)
        else:
            _m.append([_s, _e])
    claimed = sum(b - a for a, b in _m)
    # Only gaps big enough to be a function are interesting; a handful of bytes is alignment or a
    # literal pool tail.
    real_holes = [(s, e) for s, e in holes if e - s >= 32]
    ind = decode_indirect_sites(image, img_base, [(a, b) for a, b in _m])
    # Two different populations, and conflating them produces a meaningless ratio. braf/bsrf are
    # computed jumps, which switch analysis is supposed to resolve; jmp/jsr through a register are
    # function-pointer calls, which constant propagation resolves separately and which are simply
    # common in Katana code. Only the first has a denominator worth quoting.
    computed = len(ind["braf"]) + len(ind["bsrf"])
    calls = len(ind["jmp_reg"]) + len(ind["jsr_reg"])
    resolved = len(d.get("switch_sites", []))
    return {
        "title": name,
        "image_size": size,
        "functions": len(fns),
        "by_origin": dict(Counter(f["origin"] for f in fns)),
        "claimed_pct": round(100.0 * claimed / size, 1) if size else 0.0,
        "holes_over_32b": len(real_holes),
        "hole_bytes": sum(e - s for s, e in real_holes),
        "computed_jumps": computed,
        "switch_sites_resolved": resolved,
        "computed_unresolved": max(0, computed - resolved),
        "indirect_calls": calls,
        "indirect_breakdown": {k: len(v) for k, v in ind.items()},
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("images", nargs="+", help=".chd or .gdi discs, or extracted 1ST_READ.BIN files")
    ap.add_argument("--base", type=lambda s: int(s, 0), default=0x0C010000)
    ap.add_argument("--json", help="also write the rows here")
    args = ap.parse_args()

    if not TRANSLATE.exists():
        print(f"dream-translate not built at {TRANSLATE}", file=sys.stderr)
        return 2

    sys.path.insert(0, str(REPO / "tools" / "dcdisc"))
    rows = []
    for spec in args.images:
        # Absolute: the extract below runs with cwd set to the dcdisc package directory,
        # so a relative disc path given on our command line would not resolve there.
        p = pathlib.Path(spec).resolve()
        name = p.stem
        try:
            if p.suffix.lower() in (".chd", ".gdi"):
                import tempfile
                # Through the CLI rather than by importing internals: `dcdisc extract` is the
                # documented interface and will not move under us.
                cache = pathlib.Path(tempfile.gettempdir()) / "dream-corpus-cache" / name
                out = cache
                cached = list(out.rglob("1ST_READ.BIN")) + list(out.rglob("1st_read.bin"))
                if cached:
                    rows.append(scan_one(name, cached[0], args.base, args.base))
                    continue
                out.mkdir(parents=True, exist_ok=True)
                ex = subprocess.run([sys.executable, "-m", "dcdisc", "extract", str(p), str(out)],
                                    capture_output=True, text=True, timeout=1800,
                                    cwd=str(REPO / "tools" / "dcdisc"))
                if ex.returncode != 0:
                    rows.append({"title": name,
                                 "error": (ex.stderr or "extract failed").strip().split("\n")[-1]})
                    continue
                cands = list(out.rglob("1ST_READ.BIN")) + list(out.rglob("1st_read.bin"))
                if not cands:
                    rows.append({"title": name, "error": "no 1ST_READ.BIN on the disc"})
                    continue
                binary = cands[0]
            else:
                binary = p
            rows.append(scan_one(name, binary, args.base, args.base))
        except Exception as exc:  # a corpus scan should report a bad disc, not stop on it
            rows.append({"title": name, "error": f"{type(exc).__name__}: {exc}"})

    hdr = (f"{'title':26}{'fns':>6}{'claimed':>9}{'holes':>7}{'holeKB':>8}"
           f"{'braf/bsrf':>11}{'resolved':>10}{'ptr calls':>11}")
    print(hdr)
    print("-" * len(hdr))
    for r in rows:
        if "error" in r:
            print(f"  {r['title'][:26]:26}  -- {r['error'][:60]}")
            continue
        print(f"{r['title'][:25]:26}{r['functions']:>6}{r['claimed_pct']:>8.1f}%"
              f"{r['holes_over_32b']:>7}{r['hole_bytes']//1024:>8}"
              f"{r['computed_jumps']:>11}{r['switch_sites_resolved']:>10}{r['indirect_calls']:>11}")
    if args.json:
        pathlib.Path(args.json).write_text(json.dumps(rows, indent=2))
        print(f"\nwrote {args.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
