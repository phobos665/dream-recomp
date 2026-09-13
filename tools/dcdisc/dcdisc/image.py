"""Container-independent view of a Dreamcast disc.

A GD-ROM has a low-density CD area (tracks 1-2, LBA 0 to about 23000) and a high-density area
starting at LBA 45000 (track 3 onwards) that holds the game's ISO9660 filesystem. Both containers
expose the same ``DiscImage`` so the filesystem, IP.BIN and inspection code never care which one
they are reading.
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from typing import Iterator, Optional

HD_AREA_LBA = 45000
SECTOR_USER = 2048

# GDI track types
GDI_TYPE_AUDIO = 0
GDI_TYPE_DATA = 4


@dataclass
class Track:
    number: int
    lba: int             # first sector, absolute disc LBA
    sectors: int         # sector count
    sector_size: int     # bytes per stored sector: 2352 (raw/audio) or 2048 (cooked data)
    is_data: bool

    @property
    def end_lba(self) -> int:
        return self.lba + self.sectors

    def contains(self, lba: int) -> bool:
        return self.lba <= lba < self.end_lba

    @property
    def gdi_type(self) -> int:
        return GDI_TYPE_DATA if self.is_data else GDI_TYPE_AUDIO


class DiscImage:
    """Abstract disc. Subclasses implement ``read_raw_sector``."""

    path: str
    tracks: list[Track]

    def close(self) -> None:  # pragma: no cover - trivial
        pass

    def __enter__(self) -> "DiscImage":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    # -- track lookup -------------------------------------------------------------------------

    def track_for_lba(self, lba: int) -> Track:
        for t in self.tracks:
            if t.contains(lba):
                return t
        raise ValueError(f"LBA {lba} is not inside any track")

    @property
    def hd_track(self) -> Track:
        """The first data track of the high-density area, where the game filesystem lives."""
        for t in self.tracks:
            if t.is_data and t.lba >= HD_AREA_LBA:
                return t
        # A plain CD image (CDI-style self-boot or a KOS test disc) has its filesystem in the
        # first data track instead; allow that so the tooling is usable on homebrew discs.
        for t in self.tracks:
            if t.is_data:
                return t
        raise ValueError("image has no data track")

    # -- sector access ------------------------------------------------------------------------

    def read_raw_sector(self, lba: int) -> bytes:
        """Return the stored bytes of one sector (2352 for raw tracks, 2048 for cooked)."""
        raise NotImplementedError

    def read_sector(self, lba: int) -> bytes:
        """Return the 2048 user-data bytes of a data sector, whatever the stored layout."""
        raw = self.read_raw_sector(lba)
        if len(raw) == SECTOR_USER:
            return raw
        if len(raw) == 2352:
            # sync(12) header(4) user(2048) edc/ecc; mode 2 form 1 would put user data at 24,
            # but Dreamcast data tracks are mode 1.
            return raw[16:16 + SECTOR_USER]
        raise ValueError(f"unexpected stored sector size {len(raw)}")

    def read_sectors(self, lba: int, count: int) -> bytes:
        return b"".join(self.read_sector(lba + i) for i in range(count))

    def iter_track_raw(self, track: Track) -> Iterator[bytes]:
        for i in range(track.sectors):
            yield self.read_raw_sector(track.lba + i)


def open_image(path: str, prefer_chdman: bool = False) -> DiscImage:
    """Open a .gdi or .chd by extension."""
    ext = os.path.splitext(path)[1].lower()
    if ext == ".gdi":
        from .gdi import GdiImage
        return GdiImage(path)
    if ext == ".chd":
        from .chd import open_chd
        return open_chd(path, prefer_chdman=prefer_chdman)
    raise ValueError(f"unsupported image type {ext!r}; expected .gdi or .chd")
