#!/usr/bin/env python3
"""Print the programs.cmake entry for a flat C unit, using the translator's discovery (seeded with the
unit's symbols) so shared-tail entries libgcc branches into are included, named from the symbols.

  gen_specs.py --translate build/dream/translator/dream-translate --name c1_idioms
               --bin tests/sh4/cprogs/c1_idioms.bin --syms tests/sh4/cprogs/c1_idioms.syms
"""
from __future__ import annotations

import argparse
import json
import subprocess
import tempfile

BASE = 0x8C010000


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--translate", required=True)
    ap.add_argument("--name", required=True)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--syms", required=True, help="`sh-elf-nm -n` output")
    a = ap.parse_args()
    names = {}
    for line in open(a.syms):
        parts = line.split()
        if len(parts) == 3 and parts[1] in ("T", "t") and parts[2] not in ("__bss_start", "_edata", "_end"):
            names[int(parts[0], 16)] = parts[2].lstrip("_")
    with tempfile.NamedTemporaryFile(suffix=".json") as out:
        cmd = [a.translate, "discover", "--image", a.bin, "--base", hex(BASE), "--no-sweep", "--no-pointers",
               "--out", out.name]
        for addr in names:
            cmd += ["--entry", hex(addr)]
        subprocess.run(cmd, check=True, capture_output=True)
        fns = json.load(open(out.name))
    fns = fns["functions"] if isinstance(fns, dict) else fns
    specs = []
    for f in sorted(fns, key=lambda f: int(f["entry"], 0)):
        entry = int(f["entry"], 0)
        name = names.get(entry) or f"{names.get(max((n for n in names if n < entry), default=entry), 'fn')}_{entry & 0xFFFF:04x}"
        specs.append(f"{f['entry']}:{f['end']}:{name}")
    print(f'  "{a.name}|../cprogs/{a.name}.bin|{BASE:#010x}|{" ".join(specs)}"')


if __name__ == "__main__":
    main()
