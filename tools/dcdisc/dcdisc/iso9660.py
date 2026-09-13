"""Minimal ISO9660 reader for the game filesystem in a GD-ROM's high-density area.

Retail discs are mastered with the filesystem's internal LBAs already absolute (the equivalent of
``mkisofs -C 0,45000``), so a directory extent of 45123 means disc LBA 45123. Homebrew and test
images are often plain ISOs whose LBAs start at 0 relative to the track. Both are handled: if the
root directory extent recorded in the volume descriptor lies below the track start, LBAs are treated
as track-relative.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Iterator, Optional

from .image import DiscImage, SECTOR_USER, Track


@dataclass
class Entry:
    name: str
    path: str
    lba: int          # filesystem LBA (before offset translation)
    size: int
    is_dir: bool
    children: list["Entry"] = field(default_factory=list)


class Iso9660:
    def __init__(self, image: DiscImage, track: Optional[Track] = None):
        self.image = image
        self.track = track or image.hd_track
        self.offset = 0
        self._parse_pvd()

    # -- low level --------------------------------------------------------------------------

    def _read_fs_sector(self, fs_lba: int) -> bytes:
        return self.image.read_sector(fs_lba + self.offset)

    def _parse_pvd(self) -> None:
        pvd = self.image.read_sector(self.track.lba + 16)
        if pvd[0] != 1 or pvd[1:6] != b"CD001":
            raise ValueError("no ISO9660 primary volume descriptor at track start + 16")
        self.volume_id = pvd[40:72].decode("ascii", "replace").rstrip()
        self.volume_sectors = int.from_bytes(pvd[80:84], "little")
        self.logical_block_size = int.from_bytes(pvd[128:130], "little")
        root = pvd[156:190]
        root_lba = int.from_bytes(root[2:6], "little")
        root_size = int.from_bytes(root[10:14], "little")
        # absolute (retail) vs relative (plain ISO) addressing
        self.offset = 0 if root_lba >= self.track.lba else self.track.lba
        self.root = Entry("", "/", root_lba, root_size, True)

    # -- directory walking -------------------------------------------------------------------

    def _read_dir(self, entry: Entry) -> list[Entry]:
        sectors = (entry.size + SECTOR_USER - 1) // SECTOR_USER
        data = b"".join(self._read_fs_sector(entry.lba + i) for i in range(sectors))
        out: list[Entry] = []
        pos = 0
        while pos < entry.size:
            length = data[pos]
            if length == 0:
                # records never cross a sector boundary; skip to the next one
                pos = (pos // SECTOR_USER + 1) * SECTOR_USER
                continue
            rec = data[pos:pos + length]
            lba = int.from_bytes(rec[2:6], "little")
            size = int.from_bytes(rec[10:14], "little")
            flags = rec[25]
            name_len = rec[32]
            raw_name = rec[33:33 + name_len]
            pos += length
            if raw_name in (b"\x00", b"\x01"):
                continue  # . and ..
            name = raw_name.decode("ascii", "replace")
            if ";" in name:
                name = name.split(";", 1)[0]
            if name.endswith("."):
                name = name[:-1]
            is_dir = bool(flags & 0x02)
            child = Entry(name, entry.path.rstrip("/") + "/" + name, lba, size, is_dir)
            out.append(child)
        return out

    def walk(self) -> Iterator[Entry]:
        """Depth-first over all entries, populating ``children`` on directories."""
        stack = [self.root]
        seen = set()
        while stack:
            d = stack.pop()
            if d.lba in seen:
                continue
            seen.add(d.lba)
            d.children = self._read_dir(d)
            for c in d.children:
                yield c
                if c.is_dir:
                    stack.append(c)

    def files(self) -> list[Entry]:
        return [e for e in self.walk() if not e.is_dir]

    def find(self, path: str) -> Optional[Entry]:
        want = "/" + path.strip("/").upper()
        for e in self.walk():
            if e.path.upper() == want:
                return e
        return None

    # -- file data -------------------------------------------------------------------------

    def read(self, entry: Entry) -> bytes:
        if entry.is_dir:
            raise IsADirectoryError(entry.path)
        sectors = (entry.size + SECTOR_USER - 1) // SECTOR_USER
        data = b"".join(self._read_fs_sector(entry.lba + i) for i in range(sectors))
        return data[:entry.size]

    def read_path(self, path: str) -> bytes:
        e = self.find(path)
        if e is None:
            raise FileNotFoundError(path)
        return self.read(e)
