#!/usr/bin/env python3
"""Did that change alter what the guest does?

Every guest write is hashed per frame, so two runs with the same hash did bit-for-bit the same
thing. That is the only signal that reliably catches a translation change: frame counts, texture
counters, function boundaries and the unit tests all called a behaviour change clean during the
0x0C081E56 investigation, and only the hash disagreed.

    python3 tools/regress/gate.py capture before
    # ... change something, rebuild ...
    python3 tools/regress/gate.py capture after
    python3 tools/regress/gate.py compare before after

Two scenarios are run: `attract`, which any build reaches, and `gameplay`, driven with scripted
presses into actual play. They exercise very different code and a change can be free in one and
costly in the other.

Captures live in .regress/ at the repository root, which is gitignored. They are keyed to a disc
and a build, so they mean nothing on another machine.
"""
from __future__ import annotations

import argparse
import hashlib
import pathlib
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parents[2]
STORE = REPO / ".regress"

# Pinned so two captures are comparable. --rtc-seed is not optional: without it the console clock
# comes from the host date and two runs of the same build diverge at about frame 24.
COMMON = ["--no-audio", "--rtc-seed", "1", "--max-seconds", "40"]
SCENARIOS = {
    "attract": [],
    "gameplay": ["--press", "start@300,start@600,a@900,start@1200,start@1800,a@2400,start@3000"],
}
# Timings differ between runs by design and say nothing about behaviour. Dropped whole lines rather
# than substituted: an alternation like `host:|.*real time` matches the shorter branch first and
# leaves the rest of the line behind, which then differs between captures and fails the gate for no
# reason.
def strip_timings(report: str) -> str:
    drop = ("real time", "per second", "elapsed")
    return "\n".join(ln for ln in report.splitlines()
                      if not ln.startswith("host:") and not any(d in ln for d in drop))


def digest(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()[:16]


def capture(label: str, build: str, config: str, game: str) -> int:
    exe = REPO / build / "games" / game / f"{game}_boot"
    if not exe.exists():
        print(f"no launcher at {exe}", file=sys.stderr)
        return 2
    out = STORE / label
    out.mkdir(parents=True, exist_ok=True)
    for name, extra in SCENARIOS.items():
        h, rep = out / f"{name}.hash", out / f"{name}.report"
        proc = subprocess.run(
            [str(exe), "--config", str(REPO / config), *COMMON, *extra,
             "--write-hash", str(h), "--report", str(rep)],
            capture_output=True, text=True, timeout=3600, cwd=str(REPO))
        if not h.exists() or h.stat().st_size == 0:
            tail = (proc.stderr or proc.stdout or "").strip().split("\n")[-1:]
            print(f"  {label}/{name}: NO OUTPUT -- {' '.join(tail)}", file=sys.stderr)
            return 2
        counters = strip_timings(rep.read_text()) if rep.exists() else ""
        (out / f"{name}.counters").write_text(counters)
        frames = sum(1 for _ in h.open())
        print(f"  {label:14} {name:9} frames={frames:<6} writehash={digest(h)} "
              f"counters={hashlib.sha256(counters.encode()).hexdigest()[:16]}")
    return 0


def compare(a: str, b: str) -> int:
    bad = False
    for name in SCENARIOS:
        pa, pb = STORE / a / f"{name}.hash", STORE / b / f"{name}.hash"
        # Loudly, not silently. Comparing two absent files used to report a match, which is the
        # worst possible failure mode for a gate: it says "no regression" when it checked nothing.
        missing = [p for p in (pa, pb) if not p.exists()]
        for p in missing:
            print(f"  {name:9} MISSING {p.relative_to(REPO)} -- capture it before comparing",
                  file=sys.stderr)
        if missing:
            # Per scenario, not overall: one missing capture must not hide the other scenario's
            # result the way a shared flag did.
            bad = True
            continue
        ha, hb = digest(pa), digest(pb)
        ca = STORE / a / f"{name}.counters"
        cb = STORE / b / f"{name}.counters"
        same_c = ca.exists() and cb.exists() and ca.read_text() == cb.read_text()
        if ha == hb and same_c:
            print(f"  {name:9} identical")
        else:
            bad = True
            print(f"  {name:9} CHANGED  writehash {ha} -> {hb}"
                  f"{'' if same_c else '   counters differ too'}")
    if bad:
        print("\n  A changed hash is a fact; whether it matters is a judgement. Understand it "
              "before continuing.", file=sys.stderr)
    return 1 if bad else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("capture", help="run both scenarios and store the hashes")
    c.add_argument("label")
    c.add_argument("--build", default="build")
    c.add_argument("--config", default="games/crazytaxi/crazytaxi.toml")
    c.add_argument("--game", default="crazytaxi")
    d = sub.add_parser("compare", help="compare two captures")
    d.add_argument("before")
    d.add_argument("after")
    a = ap.parse_args()
    if a.cmd == "capture":
        return capture(a.label, a.build, a.config, a.game)
    return compare(a.before, a.after)


if __name__ == "__main__":
    raise SystemExit(main())
