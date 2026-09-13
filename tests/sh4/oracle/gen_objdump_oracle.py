#!/usr/bin/env python3
"""Generate the SH-4 decoder oracle: every 16-bit instruction word as disassembled by binutils.

Writes all 65536 words as a little-endian binary, runs `sh-elf-objdump -D -b binary -m sh4` on it
(inside the toolchain container, where the KallistiOS binutils live) and stores one line per word:

    <opcode hex>\\t<objdump text or ".word 0x....">

The result is the ground truth WP1.1's decoder is tested against (tests/sh4/test_decoder.cpp).
Run from the repository root:

    tools/docker/run.sh python3 tests/sh4/oracle/gen_objdump_oracle.py

The generated file is committed (about 1.5 MB) so the decoder test runs without the container.
"""
from __future__ import annotations

import os
import re
import struct
import subprocess
import sys
import tempfile

OBJDUMP = os.environ.get("SH_OBJDUMP", "/opt/toolchains/dc/sh-elf/bin/sh-elf-objdump")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sh4_objdump.txt")


def main() -> int:
    if not os.path.exists(OBJDUMP):
        sys.exit(f"{OBJDUMP} not found; run inside tools/docker (tools/docker/run.sh ...)")
    with tempfile.TemporaryDirectory() as td:
        binpath = os.path.join(td, "all.bin")
        with open(binpath, "wb") as f:
            for w in range(0x10000):
                f.write(struct.pack("<H", w))
                f.write(struct.pack("<H", 0x0009))  # NOP after each word so delay-slot forms decode cleanly
        res = subprocess.run([OBJDUMP, "-D", "-b", "binary", "-m", "sh4", "--endian=little",
                              "--adjust-vma=0", binpath], capture_output=True, text=True, check=True)
    line_re = re.compile(r"^\s*([0-9a-f]+):\s+([0-9a-f]{2} [0-9a-f]{2}|[0-9a-f]{4})\s+(.*)$")
    rows: dict[int, str] = {}
    for line in res.stdout.splitlines():
        m = line_re.match(line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        if addr % 4:
            continue  # the padding NOPs
        text = m.group(3).strip()
        text = re.sub(r"\s+", " ", text)
        text = re.sub(r"\s*!.*$", "", text)       # strip objdump comments
        rows[addr // 4] = text
    with open(OUT, "w") as f:
        for w in range(0x10000):
            f.write(f"{w:04x}\t{rows.get(w, '')}\n")
    print(f"wrote {OUT}: {len(rows)} decoded of 65536")
    return 0


if __name__ == "__main__":
    sys.exit(main())
