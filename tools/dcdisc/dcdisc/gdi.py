"""GDI reader and writer.

A .gdi is a text file: first line is the track count, then one line per track::

    <track> <lba> <type> <sector size> <filename> <offset>

type 4 is data, 0 is audio. Sector size is 2352 for raw dumps (the norm) or 2048 for cooked data
tracks. Filenames may be quoted when they contain spaces. Track files are raw concatenated sectors.
"""

from __future__ import annotations

import os
import shlex
from typing import BinaryIO, Optional

from .image import DiscImage, Track, GDI_TYPE_DATA


class GdiImage(DiscImage):
    def __init__(self, path: str):
        self.path = path
        self.dir = os.path.dirname(os.path.abspath(path))
        self.tracks: list[Track] = []
        self.files: dict[int, str] = {}
        self.offsets: dict[int, int] = {}
        self._handles: dict[int, BinaryIO] = {}
        self._parse()

    def _parse(self) -> None:
        with open(self.path, "r", encoding="utf-8", errors="replace") as f:
            lines = [ln.strip() for ln in f if ln.strip()]
        if not lines:
            raise ValueError("empty GDI file")
        try:
            count = int(lines[0])
        except ValueError as e:
            raise ValueError("first line of a GDI must be the track count") from e
        for ln in lines[1:]:
            parts = shlex.split(ln)
            if len(parts) < 5:
                raise ValueError(f"malformed GDI line: {ln!r}")
            number, lba, ttype, ssize = (int(parts[0]), int(parts[1]), int(parts[2]), int(parts[3]))
            fname = parts[4]
            offset = int(parts[5]) if len(parts) > 5 else 0
            fpath = os.path.join(self.dir, fname)
            if not os.path.exists(fpath):
                raise FileNotFoundError(f"GDI references missing track file {fname}")
            size = os.path.getsize(fpath) - offset
            if size % ssize:
                raise ValueError(f"{fname}: size {size} is not a multiple of sector size {ssize}")
            self.tracks.append(Track(number, lba, size // ssize, ssize, ttype == GDI_TYPE_DATA))
            self.files[number] = fpath
            self.offsets[number] = offset
        if len(self.tracks) != count:
            raise ValueError(f"GDI declares {count} tracks but lists {len(self.tracks)}")
        self.tracks.sort(key=lambda t: t.number)

    def _handle(self, track: Track) -> BinaryIO:
        h = self._handles.get(track.number)
        if h is None:
            h = open(self.files[track.number], "rb")
            self._handles[track.number] = h
        return h

    def read_raw_sector(self, lba: int) -> bytes:
        t = self.track_for_lba(lba)
        h = self._handle(t)
        h.seek(self.offsets[t.number] + (lba - t.lba) * t.sector_size)
        data = h.read(t.sector_size)
        if len(data) != t.sector_size:
            raise IOError(f"short read at LBA {lba}")
        return data

    def close(self) -> None:
        for h in self._handles.values():
            h.close()
        self._handles.clear()


def track_filename(track: Track) -> str:
    """Conventional GDI track filename: .bin for data, .raw for audio."""
    return f"track{track.number:02d}.{'bin' if track.is_data else 'raw'}"


def write_gdi(image: DiscImage, out_dir: str, basename: Optional[str] = None,
              progress=None) -> str:
    """Write ``image`` out as a GDI set in ``out_dir``. Returns the .gdi path.

    Works for any DiscImage, which is how CHD to GDI conversion is done.
    """
    os.makedirs(out_dir, exist_ok=True)
    basename = basename or os.path.splitext(os.path.basename(image.path))[0]
    gdi_path = os.path.join(out_dir, basename + ".gdi")
    lines = [str(len(image.tracks))]
    for t in image.tracks:
        fname = track_filename(t)
        with open(os.path.join(out_dir, fname), "wb") as f:
            for i, sector in enumerate(image.iter_track_raw(t)):
                f.write(sector)
                if progress and i % 4096 == 0:
                    progress(t, i)
            if progress:
                progress(t, t.sectors)
        lines.append(f"{t.number} {t.lba} {t.gdi_type} {t.sector_size} {fname} 0")
    with open(gdi_path, "w", encoding="ascii", newline="\r\n") as f:
        f.write("\n".join(lines) + "\n")
    return gdi_path
