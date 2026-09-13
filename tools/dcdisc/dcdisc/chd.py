"""CHD (MAME "Compressed Hunks of Data") reader for CD and GD-ROM images.

Two backends:

* **libchdr** (preferred): the BSD-licensed C library every Dreamcast emulator uses. Loaded through
  ctypes from, in order: ``$DCDISC_LIBCHDR``, a build of the ``third_party/libchdr`` submodule under
  the repository's build directories, then the system library path (``apt install libchdr-dev``,
  ``brew install libchdr``). Only hunk decompression is delegated to it; the header and the track
  metadata are parsed here in Python so ``dcdisc info`` works without any native library.
* **chdman**: MAME's own tool, found via ``$DCDISC_CHDMAN`` or ``PATH``. Used when libchdr is not
  available: ``chdman extractcd`` writes a temporary GDI which is then read normally. Slower and
  needs scratch disk space equal to the uncompressed disc, but always correct.

CHD CD layout, for anyone maintaining this: every sector is stored as a 2448-byte *frame* (2352 bytes
of sector data followed by 96 bytes of subchannel). A hunk holds ``hunkbytes / 2448`` frames. Tracks
are stored back to back, each padded to a multiple of 4 frames. The padding frames are not part of
the disc; the ``PAD`` field of the GD-ROM metadata records the LBA gap that follows a track (for a
GDI this is how the jump to LBA 45000 before track 3 survives the round trip).
"""

from __future__ import annotations

import ctypes
import ctypes.util
import glob
import os
import platform
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from typing import Optional

from .image import DiscImage, Track

CHD_MAGIC = b"MComprHD"
FRAME_SIZE = 2448
MAX_SECTOR_DATA = 2352
TRACK_PADDING = 4

TAG_CHTR = b"CHTR"  # CDROM_TRACK_METADATA_TAG      "TRACK:%d TYPE:%s SUBTYPE:%s FRAMES:%d"
TAG_CHT2 = b"CHT2"  # CDROM_TRACK_METADATA2_TAG     "... PREGAP:%d PGTYPE:%s PGSUB:%s POSTGAP:%d"
TAG_CHGT = b"CHGT"  # GDROM_OLD_METADATA_TAG
TAG_CHGD = b"CHGD"  # GDROM_TRACK_METADATA_TAG      "... FRAMES:%d PAD:%d PREGAP:%d ..."

DATASIZE = {
    "MODE1": 2048, "MODE1_RAW": 2352, "MODE1/2048": 2048, "MODE1/2352": 2352,
    "MODE2": 2336, "MODE2_FORM1": 2048, "MODE2_FORM2": 2324, "MODE2_FORM_MIX": 2336,
    "MODE2_RAW": 2352, "MODE2/2336": 2336, "MODE2/2352": 2352,
    "AUDIO": 2352,
}


class ChdError(ValueError):
    """Raised for unreadable CHDs and missing backends; a ValueError so the CLI reports it cleanly."""


# --------------------------------------------------------------------------------------------
# Header and metadata (pure Python)
# --------------------------------------------------------------------------------------------

@dataclass
class ChdHeader:
    version: int
    compressors: list[str]
    logical_bytes: int
    map_offset: int
    meta_offset: int
    hunk_bytes: int
    unit_bytes: int
    raw_sha1: bytes
    sha1: bytes
    parent_sha1: bytes

    @property
    def total_hunks(self) -> int:
        return (self.logical_bytes + self.hunk_bytes - 1) // self.hunk_bytes

    @property
    def has_parent(self) -> bool:
        return any(self.parent_sha1)


def read_header(path: str) -> ChdHeader:
    with open(path, "rb") as f:
        raw = f.read(124)
    if raw[:8] != CHD_MAGIC:
        raise ChdError(f"{path}: not a CHD file")
    length, version = struct.unpack(">II", raw[8:16])
    if version != 5:
        raise ChdError(f"{path}: CHD version {version} is not supported (only v5); "
                       "re-create it with a current chdman")
    comps = [raw[16 + 4 * i:20 + 4 * i] for i in range(4)]
    compressors = [c.decode("ascii", "replace") for c in comps if c != b"\0\0\0\0"]
    logical, mapoff, metaoff = struct.unpack(">QQQ", raw[32:56])
    hunkbytes, unitbytes = struct.unpack(">II", raw[56:64])
    return ChdHeader(version, compressors, logical, mapoff, metaoff, hunkbytes, unitbytes,
                     raw[64:84], raw[84:104], raw[104:124])


def read_metadata(path: str) -> list[tuple[bytes, bytes]]:
    """Return [(tag, data), ...] walking the metadata chain. Data has its trailing NUL stripped."""
    hdr = read_header(path)
    out = []
    with open(path, "rb") as f:
        offset = hdr.meta_offset
        while offset:
            f.seek(offset)
            entry = f.read(16)
            if len(entry) < 16:
                break
            tag = entry[0:4]
            length = int.from_bytes(entry[4:8], "big") & 0x00FFFFFF
            nxt = int.from_bytes(entry[8:16], "big")
            data = f.read(length)
            out.append((tag, data.rstrip(b"\0")))
            offset = nxt
    return out


@dataclass
class ChdTrack:
    number: int
    type: str
    subtype: str
    frames: int
    pad: int
    pregap: int
    pgtype: str
    pgsub: str
    postgap: int
    # derived
    chd_frame: int = 0   # first frame of this track inside the CHD
    lba: int = 0         # first disc LBA

    @property
    def datasize(self) -> int:
        try:
            return DATASIZE[self.type]
        except KeyError:
            raise ChdError(f"unknown CHD track type {self.type!r}")

    @property
    def is_data(self) -> bool:
        return self.type != "AUDIO"

    @property
    def sectors(self) -> int:
        """Genuine disc sectors in this track (FRAMES minus the stored PAD gap)."""
        return self.frames - self.pad


_META_RE = re.compile(rb"(\w+):(\S+)")


def parse_tracks(meta: list[tuple[bytes, bytes]]) -> tuple[list[ChdTrack], bool]:
    """Turn metadata entries into tracks with CHD frame offsets and disc LBAs.

    Returns (tracks, is_gdrom).
    """
    tracks: list[ChdTrack] = []
    is_gdrom = False
    for tag, data in meta:
        if tag not in (TAG_CHTR, TAG_CHT2, TAG_CHGT, TAG_CHGD):
            continue
        kv = {k.decode(): v.decode() for k, v in _META_RE.findall(data)}
        t = ChdTrack(
            number=int(kv["TRACK"]), type=kv["TYPE"], subtype=kv.get("SUBTYPE", "NONE"),
            frames=int(kv["FRAMES"]), pad=int(kv.get("PAD", 0)), pregap=int(kv.get("PREGAP", 0)),
            pgtype=kv.get("PGTYPE", "MODE1"), pgsub=kv.get("PGSUB", "NONE"),
            postgap=int(kv.get("POSTGAP", 0)),
        )
        if tag in (TAG_CHGT, TAG_CHGD):
            is_gdrom = True
        tracks.append(t)
    if not tracks:
        raise ChdError("CHD has no CD track metadata; is this a hard-disk CHD?")
    tracks.sort(key=lambda t: t.number)

    chd_frame = 0
    lba = 0
    for t in tracks:
        # A pregap whose type starts with 'V' has its frames stored in the CHD; otherwise the
        # pregap is implied and only shifts the logical LBA. GDI-derived images have neither.
        stored_pregap = t.pregap if t.pgtype.startswith("V") else 0
        if not t.pgtype.startswith("V"):
            lba += t.pregap
        t.chd_frame = chd_frame + stored_pregap
        t.lba = lba
        # chdman (verified with 0.264) stores a GDI's inter-track gap as real zero frames at the
        # end of the preceding track: FRAMES counts them and PAD says how many they are. So the
        # track's genuine sectors are FRAMES - PAD, the next track starts FRAMES later, and the
        # CHD layout advances by FRAMES rounded up to the 4-frame padding.
        padded = (t.frames + TRACK_PADDING - 1) // TRACK_PADDING * TRACK_PADDING
        chd_frame += padded
        lba += t.frames + t.postgap
    return tracks, is_gdrom


# --------------------------------------------------------------------------------------------
# libchdr backend
# --------------------------------------------------------------------------------------------

def _candidate_libraries(repo_root: Optional[str]) -> list[str]:
    names = {
        "Darwin": ["libchdr.dylib", "libchdr.0.dylib"],
        "Windows": ["chdr.dll", "libchdr.dll", "chdr-static.dll"],
    }.get(platform.system(), ["libchdr.so.0", "libchdr.so"])
    cands: list[str] = []
    env = os.environ.get("DCDISC_LIBCHDR")
    if env:
        cands.append(env)
    if repo_root:
        for pattern in ("build*/**/", "out/**/"):
            for n in names:
                cands += glob.glob(os.path.join(repo_root, pattern, n), recursive=True)
    for n in names:
        cands.append(n)
    found = ctypes.util.find_library("chdr")
    if found:
        cands.append(found)
    return cands


def find_repo_root(start: Optional[str] = None) -> Optional[str]:
    d = os.path.abspath(start or os.path.dirname(__file__))
    while True:
        if os.path.isdir(os.path.join(d, ".git")) or os.path.exists(os.path.join(d, ".gitmodules")):
            return d
        parent = os.path.dirname(d)
        if parent == d:
            return None
        d = parent


_lib_cache: Optional[ctypes.CDLL] = None
_lib_missing = False


def load_libchdr() -> Optional[ctypes.CDLL]:
    global _lib_cache, _lib_missing
    if _lib_cache is not None or _lib_missing:
        return _lib_cache
    for cand in _candidate_libraries(find_repo_root()):
        try:
            lib = ctypes.CDLL(cand)
        except OSError:
            continue
        lib.chd_open.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_void_p,
                                 ctypes.POINTER(ctypes.c_void_p)]
        lib.chd_open.restype = ctypes.c_int
        lib.chd_read.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p]
        lib.chd_read.restype = ctypes.c_int
        lib.chd_close.argtypes = [ctypes.c_void_p]
        lib.chd_close.restype = None
        lib.chd_error_string.argtypes = [ctypes.c_int]
        lib.chd_error_string.restype = ctypes.c_char_p
        lib._dcdisc_path = cand  # type: ignore[attr-defined]
        _lib_cache = lib
        return lib
    _lib_missing = True
    return None


CHD_OPEN_READ = 1


class ChdImage(DiscImage):
    """A CHD read through libchdr."""

    def __init__(self, path: str, lib: Optional[ctypes.CDLL] = None):
        self.path = path
        self.header = read_header(path)
        if self.header.has_parent:
            raise ChdError("CHDs with a parent are not supported; merge with "
                           "`chdman copy` first")
        self.chd_tracks, self.is_gdrom = parse_tracks(read_metadata(path))
        if self.header.unit_bytes != FRAME_SIZE:
            raise ChdError(f"unit size {self.header.unit_bytes} is not a CD frame; not a CD CHD")
        self.frames_per_hunk = self.header.hunk_bytes // FRAME_SIZE
        self.tracks = [
            Track(t.number, t.lba, t.sectors, t.datasize, t.is_data) for t in self.chd_tracks
        ]
        self._lib = lib or load_libchdr()
        if self._lib is None:
            raise ChdError("libchdr not found")
        self._chd = ctypes.c_void_p()
        err = self._lib.chd_open(path.encode(), CHD_OPEN_READ, None, ctypes.byref(self._chd))
        if err:
            raise ChdError(f"chd_open failed: {self._lib.chd_error_string(err).decode()}")
        self._hunk_buf = ctypes.create_string_buffer(self.header.hunk_bytes)
        self._hunk_cache: dict[int, bytes] = {}

    def _hunk(self, n: int) -> bytes:
        h = self._hunk_cache.get(n)
        if h is None:
            err = self._lib.chd_read(self._chd, n, self._hunk_buf)
            if err:
                raise ChdError(f"chd_read hunk {n}: {self._lib.chd_error_string(err).decode()}")
            h = self._hunk_buf.raw
            if len(self._hunk_cache) > 64:
                self._hunk_cache.clear()
            self._hunk_cache[n] = h
        return h

    def read_frame(self, frame: int) -> bytes:
        hunk = self._hunk(frame // self.frames_per_hunk)
        off = (frame % self.frames_per_hunk) * FRAME_SIZE
        return hunk[off:off + FRAME_SIZE]

    def read_raw_sector(self, lba: int) -> bytes:
        for t in self.chd_tracks:
            if t.lba <= lba < t.lba + t.sectors:
                frame = self.read_frame(t.chd_frame + (lba - t.lba))
                data = frame[:t.datasize]
                if not t.is_data:
                    # chdman stores CD audio big-endian; discs and GDI dumps are little-endian.
                    data = _byteswap16(data)
                return data
        raise ValueError(f"LBA {lba} is not inside any track")

    def close(self) -> None:
        if getattr(self, "_chd", None):
            self._lib.chd_close(self._chd)
            self._chd = None


def _byteswap16(b: bytes) -> bytes:
    a = bytearray(b)
    a[0::2], a[1::2] = b[1::2], b[0::2]
    return bytes(a)


# --------------------------------------------------------------------------------------------
# chdman backend
# --------------------------------------------------------------------------------------------

def find_chdman() -> Optional[str]:
    env = os.environ.get("DCDISC_CHDMAN")
    if env and os.path.exists(env):
        return env
    return shutil.which("chdman")


class ChdmanImage(DiscImage):
    """A CHD converted to a temporary GDI by chdman and read from there."""

    def __init__(self, path: str, chdman: Optional[str] = None, keep_dir: Optional[str] = None):
        from .gdi import GdiImage
        self.path = path
        chdman = chdman or find_chdman()
        if not chdman:
            raise ChdError("chdman not found")
        self._tmp = keep_dir or tempfile.mkdtemp(prefix="dcdisc-chd-")
        self._owns_tmp = keep_dir is None
        gdi = os.path.join(self._tmp, os.path.splitext(os.path.basename(path))[0] + ".gdi")
        cmd = [chdman, "extractcd", "-i", path, "-o", gdi, "-f"]
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0:
            raise ChdError(f"chdman extractcd failed:\n{res.stdout}\n{res.stderr}")
        self._gdi = GdiImage(gdi)
        self.tracks = self._gdi.tracks

    def read_raw_sector(self, lba: int) -> bytes:
        return self._gdi.read_raw_sector(lba)

    def close(self) -> None:
        self._gdi.close()
        if self._owns_tmp:
            shutil.rmtree(self._tmp, ignore_errors=True)


def open_chd(path: str, prefer_chdman: bool = False) -> DiscImage:
    lib = None if prefer_chdman else load_libchdr()
    if lib is not None:
        return ChdImage(path, lib)
    if find_chdman():
        return ChdmanImage(path)
    raise ChdError(
        "cannot read CHD: neither libchdr nor chdman is available.\n"
        "  - build the third_party/libchdr submodule (or `apt install libchdr-dev`, "
        "`brew install libchdr`) and/or set DCDISC_LIBCHDR to the shared library, or\n"
        "  - install MAME's chdman (`apt install mame-tools`, `brew install mame`) or set "
        "DCDISC_CHDMAN to it."
    )


def backend_status() -> dict:
    lib = load_libchdr()
    return {
        "libchdr": getattr(lib, "_dcdisc_path", None) if lib else None,
        "chdman": find_chdman(),
        "python": sys.version.split()[0],
    }
