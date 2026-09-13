#!/usr/bin/env python3
"""Differential harness: run every case through the Flycast oracle and the recompiled runner and
compare the final contexts (WP1.5).

  differential.py --runner build/dream/tests/sh4/dream_sh4_runner [--lib flycast_libretro.dylib]
                  [--programs tests/sh4/programs] [--cases tools/oracle/cases.json] [--keep DIR]

The case file lists {name, image, base, entry, regs{rN: value}, fregs{frN: bits|1.5f}, fpscr?,
hash?: "ADDR:LEN", fill?: "ADDR:LEN:SEED", program?} (program defaults to the image's stem, the name in
tests/sh4/programs/programs.cmake; an absolute image path is used as is, and ${CT_IMAGE} names the
owner-supplied Crazy Taxi 1ST_READ.BIN for games/crazytaxi/oracle-cases.json).
Exit status is the number of mismatching cases."""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import compare  # noqa: E402
import oracle  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--runner", required=True, help="dream_sh4_runner executable")
    ap.add_argument("--lib", default=os.environ.get("DREAM_FLYCAST_CORE"))
    ap.add_argument("--programs", default=str(HERE.parent.parent / "tests" / "sh4" / "programs"))
    ap.add_argument("--cases", action="append", help="case file(s); default tools/oracle/cases.json")
    ap.add_argument("--keep", help="directory to keep the JSON dumps in (default: temporary)")
    a = ap.parse_args()
    if not a.lib:
        sys.exit("no oracle library: pass --lib or set DREAM_FLYCAST_CORE")
    cases = []
    for path in a.cases or [str(HERE / "cases.json")]:
        cases += json.load(open(path))
    outdir = Path(a.keep) if a.keep else Path(tempfile.mkdtemp(prefix="dream-diff-"))
    outdir.mkdir(parents=True, exist_ok=True)
    failures = 0
    for case in cases:
        name = case["name"]
        image = case["image"].replace("${CT_IMAGE}", str(HERE.parent.parent / "games/crazytaxi/extracted/fs/1ST_READ.BIN"))
        image = image if os.path.isabs(image) else str(Path(a.programs) / image)
        base, entry = int(case["base"], 0), int(case["entry"], 0)
        fpscr = int(case.get("fpscr", "0x00040001"), 0)
        regs = oracle.parse_regs([f"{k}={v}" for k, v in case.get("regs", {}).items()])
        fregs = oracle.parse_fregs([f"{k}={v}" for k, v in case.get("fregs", {}).items()])
        ha, hl = (int(x, 0) for x in case["hash"].split(":")) if "hash" in case else (0, 0)
        fill = tuple(int(x, 0) for x in case["fill"].split(":")) if "fill" in case else (0, 0, 0)
        o_path, r_path = outdir / f"{name}.oracle.json", outdir / f"{name}.recomp.json"
        rc = oracle.run(a.lib, image, base, entry, regs, fpscr, str(o_path), 50_000_000, ha, hl, fregs, fill)
        if rc != 0:
            why = {1: "did not reach the stop address", 2: "could not reserve the address space",
                   3: f"could not open image {image}", 4: "could not write the dump"}.get(rc, f"rc {rc}")
            print(f"{name}: oracle {why}")
            failures += 1
            continue
        cmd = [a.runner, "--program", case.get("program", case["image"].rsplit(".", 1)[0]), "--image", image, "--base", hex(base), "--entry", hex(entry), "--fpscr", hex(fpscr),
               "--out", str(r_path)]
        for k, v in case.get("regs", {}).items():
            cmd += ["--reg", f"{k}={v}"]
        for k, v in case.get("fregs", {}).items():
            cmd += ["--freg", f"{k}={v}"]
        if hl:
            cmd += ["--hash", f"{ha:#x}:{hl}"]
        if fill[1]:
            cmd += ["--fill", case["fill"]]
        try:
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        except subprocess.TimeoutExpired:
            print(f"{name}: runner timed out (120 s)")
            failures += 1
            continue
        if p.returncode != 0:
            print(f"{name}: runner failed ({p.returncode}): {p.stderr.strip()}")
            failures += 1
            continue
        oj, rj = json.load(open(o_path)), json.load(open(r_path))
        diffs = compare.diff(oj, rj)
        if diffs:
            failures += 1
            print(f"{name}: MISMATCH ({oj['steps']} oracle steps)")
            for d in diffs[:20]:
                print("    " + d)
        else:
            print(f"{name}: match ({oj['steps']} oracle steps)")
    print(f"{len(cases) - failures}/{len(cases)} cases agree; dumps in {outdir}")
    return failures


if __name__ == "__main__":
    sys.exit(main())
