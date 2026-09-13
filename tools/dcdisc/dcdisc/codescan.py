"""Heuristics for finding SH-4 code and SDK banners in files from a disc.

Used by the disc verification checklist to answer "which files contain code?" and "which Katana
libraries does this binary carry?". These are heuristics tuned on compiler output patterns, not
proofs; the translator's real discovery pass is the authority.
"""

from __future__ import annotations

import re
import struct
from dataclasses import dataclass

# 16-bit SH-4 opcodes that compilers emit at nearly every function boundary
OP_RTS = 0x000B
OP_NOP = 0x0009
OP_STS_L_PR = 0x4F22      # STS.L PR,@-R15
OP_LDS_L_PR = 0x4F26      # LDS.L @R15+,PR
OP_PUSH_R14 = 0x2FE6      # MOV.L R14,@-R15
OP_MOV_R15_R14 = 0x6EF3   # MOV R15,R14
OP_POP_R14 = 0x6EF6       # MOV.L @R15+,R14

SDK_BANNERS = [
    b"SEGA LIBRARY", b"SEGA ENTERPRISES", b"KATANA", b"Katana",
    b"Ninja", b"NINJA", b"njdrp", b"nj_", b"KAMUI", b"Kamui", b"kamui",
    b"Shinobi", b"SHINOBI", b"Manatee", b"MANATEE", b"AICA", b"DPCM", b"ADX",
    b"Windows CE", b"WINCE", b"COREDLL", b"Copyright", b"Ver.", b"Version",
    b"SHC", b"Hitachi", b"CodeWarrior", b"Metrowerks", b"GCC", b"GNU",
]

WINCE_FILES = {"0WINCEOS.BIN", "WINCEOS.BIN"}
STREAMING_EXTS = {".ADX", ".AFS", ".SFD", ".STR", ".AIX", ".MPG", ".SAN"}


@dataclass
class CodeScore:
    size: int
    words: int
    rts: int
    rts_nop: int
    prologue: int
    epilogue: int
    pc_rel_loads: int
    pc_rel_plausible: int = 0

    @property
    def per_kb(self) -> float:
        return (self.prologue + self.epilogue) / max(1, self.size / 1024)

    @property
    def pointer_ratio(self) -> float:
        """Fraction of ``MOV.L @(disp,PC)`` loads whose literal looks like a Dreamcast address
        or small constant. Scrambling permutes 32-byte slices, which leaves per-instruction
        statistics intact but breaks most literal-pool references, so this ratio is what
        separates a correctly descrambled binary (typically > 0.5) from a scrambled one
        (typically < 0.2). Per-word heuristics cannot make that distinction."""
        return self.pc_rel_plausible / self.pc_rel_loads if self.pc_rel_loads else 0.0

    @property
    def looks_ordered(self) -> bool:
        return self.pc_rel_loads >= 16 and self.pointer_ratio >= 0.35

    @property
    def looks_like_code(self) -> bool:
        # Real SH-4 binaries show a stack save/restore pair every few hundred bytes and RTS
        # followed by NOP (or a real delay-slot instruction) at a similar density. Data and
        # textures very rarely produce both signals together.
        return (self.size >= 4096 and self.per_kb >= 1.0 and self.rts >= self.size / 4096
                and self.prologue > 0 and self.epilogue > 0)


def plausible_literal(v: int) -> bool:
    return (0x8C000000 <= v < 0x8D000000 or   # main RAM, cached
            0xAC000000 <= v < 0xAD000000 or   # main RAM, uncached
            0x0C000000 <= v < 0x0D000000 or   # main RAM, physical
            0xA0000000 <= v < 0xA6000000 or   # ROM, flash, Holly, sound RAM, VRAM (uncached)
            0x005F0000 <= v < 0x00800000 or   # Holly and AICA registers, physical
            0xE0000000 <= v < 0xE4000000 or   # store queues
            0xFF000000 <= v <= 0xFFFFFFFF or  # CPU control registers, also -1 and small negatives
            v < 0x00010000)                   # small constants


def score_code(data: bytes) -> CodeScore:
    n = len(data) // 2
    words = struct.unpack_from(f"<{n}H", data, 0) if n else ()
    rts = rts_nop = prologue = epilogue = pcrel = plausible = 0
    prev = None
    for i, w in enumerate(words):
        if w == OP_RTS:
            rts += 1
        if prev == OP_RTS and w == OP_NOP:
            rts_nop += 1
        if w in (OP_STS_L_PR, OP_PUSH_R14):
            prologue += 1
        if w in (OP_LDS_L_PR, OP_POP_R14):
            epilogue += 1
        if (w & 0xF000) == 0xD000:   # MOV.L @(disp,PC),Rn ; target = (PC & ~3) + 4 + disp*4
            pcrel += 1
            target = ((2 * i) & ~3) + 4 + (w & 0xFF) * 4
            if target + 4 <= len(data):
                v = struct.unpack_from("<I", data, target)[0]
                if plausible_literal(v):
                    plausible += 1
        prev = w
    return CodeScore(len(data), n, rts, rts_nop, prologue, epilogue, pcrel, plausible)


def find_banners(data: bytes) -> dict[str, int]:
    out = {}
    for b in SDK_BANNERS:
        c = data.count(b)
        if c:
            out[b.decode("ascii")] = c
    return out


_STR_RE = re.compile(rb"[\x20-\x7e]{8,}")


def strings(data: bytes, min_len: int = 8) -> list[str]:
    pat = _STR_RE if min_len == 8 else re.compile(rb"[\x20-\x7e]{%d,}" % min_len)
    return [m.group().decode("ascii") for m in pat.finditer(data)]


def referenced_filenames(data: bytes, names: list[str]) -> list[str]:
    """Which of the disc's filenames appear as strings inside ``data`` (case-insensitive)."""
    up = data.upper()
    hits = []
    for n in names:
        base = n.rsplit("/", 1)[-1].upper().encode("ascii", "ignore")
        if len(base) >= 5 and base in up:
            hits.append(n)
    return sorted(set(hits))
