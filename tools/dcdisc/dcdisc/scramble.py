"""1ST_READ.BIN scrambling.

The BIOS scrambles the boot binary only when booting from a CD-R (the MIL-CD path): the binary on
such a disc has its 32-byte slices permuted and the BIOS descrambles it while loading. **A GD-ROM
stores 1ST_READ.BIN in plain form**, so dumps of retail GD-ROMs (GDI, CHD) need no descrambling;
self-boot CD-R images (CDI) do. Verified 2026-09-11 on Crazy Taxi and Tony Hawk's Pro Skater 2 CHDs:
the raw boot binary is ordered code and descrambling it destroys it.

The permutation is driven by a 16-bit LCG seeded with the file size, applied over progressively
smaller windows (2 MB down to 32 bytes), then any final sub-slice remainder is copied verbatim. This
is the algorithm from Marcus Comstedt's ``scramble.c``, reimplemented; a scrambled file and its plain
form are the same length. Descramble reads the scrambled input sequentially and scatters slices to
their permuted positions; scramble is the exact inverse.

Use ``codescan.score_code(data).pointer_ratio`` to decide which form a binary is in: ordered code
has a high literal-pool pointer ratio, a scrambled copy a low one. ``is_scrambled`` below does that.
"""

from __future__ import annotations

MAXCHUNK = 2048 * 1024
SLICE = 32


class _Rng:
    def __init__(self, seed: int):
        self.seed = seed & 0xFFFF

    def next(self) -> int:
        self.seed = (self.seed * 2109 + 2) & 0xFFFF
        return self.seed


def _permutation(rng: _Rng, slices: int) -> list[int]:
    """Order in which sequential input slices land in the output window."""
    idx = list(range(slices))
    order = []
    for i in range(slices - 1, -1, -1):
        x = (rng.next() * i) >> 16
        idx[i], idx[x] = idx[x], idx[i]
        order.append(idx[i])
    return order


def descramble(data: bytes) -> bytes:
    size = len(data)
    out = bytearray(size)
    rng = _Rng(size)
    in_pos = 0
    out_pos = 0
    remaining = size
    chunk = MAXCHUNK
    while chunk >= SLICE:
        while remaining >= chunk:
            for target in _permutation(rng, chunk // SLICE):
                dst = out_pos + target * SLICE
                out[dst:dst + SLICE] = data[in_pos:in_pos + SLICE]
                in_pos += SLICE
            out_pos += chunk
            remaining -= chunk
        chunk >>= 1
    if remaining:
        out[out_pos:out_pos + remaining] = data[in_pos:in_pos + remaining]
    return bytes(out)


def scramble(data: bytes) -> bytes:
    size = len(data)
    out = bytearray(size)
    rng = _Rng(size)
    in_pos = 0
    out_pos = 0
    remaining = size
    chunk = MAXCHUNK
    while chunk >= SLICE:
        while remaining >= chunk:
            for target in _permutation(rng, chunk // SLICE):
                src = in_pos + target * SLICE
                out[out_pos:out_pos + SLICE] = data[src:src + SLICE]
                out_pos += SLICE
            in_pos += chunk
            remaining -= chunk
        chunk >>= 1
    if remaining:
        out[out_pos:out_pos + remaining] = data[in_pos:in_pos + remaining]
    return bytes(out)


def is_scrambled(data: bytes) -> bool:
    """True if ``data`` reads as ordered SH-4 code only after descrambling."""
    from .codescan import score_code
    return score_code(descramble(data)).pointer_ratio > score_code(data).pointer_ratio


def ensure_plain(data: bytes) -> tuple[bytes, bool]:
    """Return (plain binary, was_scrambled)."""
    if is_scrambled(data):
        return descramble(data), True
    return data, False
