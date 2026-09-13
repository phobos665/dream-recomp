#!/usr/bin/env python3
"""Run an SH-4 program through Flycast's interpreter (the oracle) and dump the final context.

Needs the flycast libretro core built with dream-recomp's `dream_oracle.cpp` patch (see
docs/oracle.md). The library path comes from --lib or $DREAM_FLYCAST_CORE.

  oracle.py --image p1_sum.bin --base 0x8c010000 --entry 0x8c010000 --reg r4=5 --out oracle.json
"""
from __future__ import annotations

import argparse
import ctypes
import json
import os
import sys

STOP_PC = 0x8C00FFF0          # sentinel return address; never executed
DEFAULT_SP = 0x8C00F400
DEFAULT_FPSCR = 0x00040001


def parse_regs(items: list[str]) -> list[int]:
    regs = [0] * 16
    regs[15] = DEFAULT_SP
    for it in items or []:
        name, _, val = it.partition("=")
        n = int(name.strip().lower().lstrip("r"))
        regs[n] = int(val, 0) & 0xFFFFFFFF
    return regs


def parse_fregs(items: list[str]) -> list[int]:
    """frN=bits (hex bit pattern) or frN=1.5f (a float literal)."""
    import struct
    fr = [0] * 16
    for it in items or []:
        name, _, val = it.partition("=")
        n = int(name.strip().lower().lstrip("fr"))
        val = val.strip()
        if val.endswith("f"):
            fr[n] = struct.unpack("<I", struct.pack("<f", float(val[:-1])))[0]
        else:
            fr[n] = int(val, 0) & 0xFFFFFFFF
    return fr


def run(lib_path: str, image: str, base: int, entry: int, regs: list[int], fpscr: int, out: str,
        max_steps: int = 50_000_000, hash_addr: int = 0, hash_len: int = 0,
        fregs: list[int] | None = None, fill: tuple[int, int, int] = (0, 0, 0)) -> int:
    lib = ctypes.CDLL(lib_path)
    fn = lib.dream_oracle_run
    fn.restype = ctypes.c_int
    fn.argtypes = [ctypes.c_char_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32,
                   ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_uint32), ctypes.c_uint32,
                   ctypes.c_uint64, ctypes.c_char_p, ctypes.c_uint32, ctypes.c_uint32,
                   ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32]
    arr = (ctypes.c_uint32 * 16)(*regs)
    farr = (ctypes.c_uint32 * 16)(*(fregs or [0] * 16))
    return fn(image.encode(), base, entry, STOP_PC, arr, farr, fpscr, max_steps, out.encode(), hash_addr,
              hash_len, fill[0], fill[1], fill[2])


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--lib", default=os.environ.get("DREAM_FLYCAST_CORE"))
    ap.add_argument("--image", required=True)
    ap.add_argument("--base", type=lambda x: int(x, 0), required=True)
    ap.add_argument("--entry", type=lambda x: int(x, 0), required=True)
    ap.add_argument("--reg", action="append", default=[], help="rN=value (repeatable)")
    ap.add_argument("--freg", action="append", default=[], help="frN=bits or frN=1.5f (repeatable)")
    ap.add_argument("--fpscr", type=lambda x: int(x, 0), default=DEFAULT_FPSCR)
    ap.add_argument("--hash", help="ADDR:LEN memory range to hash into the dump")
    ap.add_argument("--fill", help="ADDR:LEN:SEED deterministic data pattern written before the run")
    ap.add_argument("--max-steps", type=int, default=50_000_000)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    if not a.lib:
        sys.exit("no oracle library: pass --lib or set DREAM_FLYCAST_CORE")
    ha, hl = (int(x, 0) for x in a.hash.split(":")) if a.hash else (0, 0)
    fill = tuple(int(x, 0) for x in a.fill.split(":")) if a.fill else (0, 0, 0)
    rc = run(a.lib, a.image, a.base, a.entry, parse_regs(a.reg), a.fpscr, a.out, a.max_steps, ha, hl,
             parse_fregs(a.freg), fill)
    if rc != 0:
        print(f"oracle returned {rc} (1 = did not reach the stop address, 3 = cannot open image)", file=sys.stderr)
    with open(a.out) as f:
        d = json.load(f)
    print(f"steps {d['steps']}, r0={d['r'][0]}, stopped={d['stopped']}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
