"""Builds a small synthetic GD-ROM as a GDI so every reader can be tested without game data.

Layout mirrors a retail disc: an audio track and a tiny data track in the low-density area, then a
data track at LBA 45000 holding an ISO9660 filesystem with IP.BIN, a fake 1ST_READ.BIN that contains
recognisable SH-4 function shapes, some data files, and a streaming .ADX. The boot binary is stored
*scrambled* here to exercise the CD-R style path; real GD-ROM dumps store it plain (see scramble.py).
"""

from __future__ import annotations

import io
import os
import struct

import pytest

pycdlib = pytest.importorskip("pycdlib")

from dcdisc import codescan  # noqa: E402
from dcdisc.ipbin import crc16  # noqa: E402
from dcdisc.scramble import scramble  # noqa: E402

HD_LBA = 45000


def fake_sh4_binary(size: int = 24 * 1024, seed: int = 1) -> bytes:
    """Deterministic bytes shaped like compiler output: prologue, body with PC-relative loads
    from a literal pool placed after RTS/NOP, epilogue, and a few strings."""
    import random
    rnd = random.Random(seed)
    out = bytearray()
    strings = [b"SEGA LIBRARY Ninja Ver.2.30\0", b"KAMUI v1.05\0", b"Manatee Sound Driver\0",
               b"Copyright (C) 1999 Example\0", b"DATA/LEVEL01.BIN\0", b"SOUND/BGM01.ADX\0"]
    while len(out) < size - 1024:
        base = len(out)
        func = bytearray(struct.pack("<HH", codescan.OP_STS_L_PR, codescan.OP_PUSH_R14))
        pcrel_slots = []
        for _ in range(rnd.randint(8, 60)):
            if rnd.random() < 0.15:
                pcrel_slots.append(len(func))
                func += b"\0\0"  # placeholder for MOV.L @(disp,PC),Rn
            else:
                op = rnd.choice([0x6003 | (rnd.randrange(16) << 8), 0x7001 | (rnd.randrange(16) << 8),
                                 0x3000 | rnd.randrange(0x1000), 0xE000 | rnd.randrange(0x1000),
                                 0x2000 | rnd.randrange(0x1000), 0x4000 | rnd.randrange(0x1000)])
                func += struct.pack("<H", op)
        func += struct.pack("<HHHH", codescan.OP_POP_R14, codescan.OP_LDS_L_PR,
                            codescan.OP_RTS, codescan.OP_NOP)
        while (base + len(func)) % 4:
            func += b"\0\0"
        pool = base + len(func)
        for k, slot in enumerate(pcrel_slots):
            pc = base + slot
            disp = (pool + 4 * k - ((pc & ~3) + 4)) // 4
            assert 0 <= disp < 256
            struct.pack_into("<H", func, slot, 0xD000 | (rnd.randrange(16) << 8) | disp)
        for _ in pcrel_slots:
            func += struct.pack("<I", 0x8C010000 + rnd.randrange(0, size, 4))
        out += func
        if rnd.random() < 0.15:
            s = rnd.choice(strings)
            out += s + b"\0" * ((-len(s)) % 4)
    out += bytes(size - len(out))
    return bytes(out)


def make_ipbin(title: str = "SYNTHETIC TEST DISC", boot: str = "1ST_READ.BIN",
               peripherals: int = 0x0799A10, wince: bool = False) -> bytes:
    if wince:
        peripherals |= 1
    ip = bytearray(0x8000)

    def put(off, s, width):
        ip[off:off + width] = s.encode("ascii").ljust(width, b" ")[:width]

    put(0x00, "SEGA SEGAKATANA", 16)
    put(0x10, "SEGA ENTERPRISES", 16)
    put(0x30, "JUE", 8)
    put(0x38, f"{peripherals:07X}", 8)
    put(0x40, "T-99999N", 10)
    put(0x4A, "V1.000", 6)
    put(0x50, "20260910", 16)
    put(0x60, boot, 16)
    put(0x70, "DREAM-RECOMP", 16)
    put(0x80, title, 128)
    crc = crc16(bytes(ip[0x40:0x50]))
    put(0x20, f"{crc:04X} GD-ROM1/1", 16)
    ip[0x100:0x104] = struct.pack("<HH", codescan.OP_RTS, codescan.OP_NOP)
    return bytes(ip)


def build_iso(files: dict[str, bytes]) -> bytes:
    iso = pycdlib.PyCdlib()
    iso.new(interchange_level=1, vol_ident="DREAMTEST")
    dirs = set()
    for path in files:
        parts = path.strip("/").split("/")
        for i in range(1, len(parts)):
            d = "/" + "/".join(parts[:i])
            if d not in dirs:
                iso.add_directory(d.upper())
                dirs.add(d)
    for path, data in files.items():
        name = path.strip("/").upper()
        if "." not in name.rsplit("/", 1)[-1]:
            name += "."
        iso.add_fp(io.BytesIO(data), len(data), "/" + name + ";1")
    buf = io.BytesIO()
    iso.write_fp(buf)
    iso.close()
    return buf.getvalue()


def mode1_raw_sector(user: bytes, lba: int) -> bytes:
    """Sync + header (MSF of lba+150) + user data + zeroed EDC/ECC."""
    assert len(user) == 2048
    m = lba + 150
    mm, ss, ff = m // (60 * 75), (m // 75) % 60, m % 75
    bcd = lambda v: ((v // 10) << 4) | (v % 10)
    header = bytes([bcd(mm), bcd(ss), bcd(ff), 1])
    return b"\x00" + b"\xff" * 10 + b"\x00" + header + user + bytes(2352 - 16 - 2048)


def audio_pattern(sectors: int) -> bytes:
    # a slow ramp that is obviously little-endian 16-bit and compresses well under FLAC
    out = bytearray()
    for i in range(sectors * 2352 // 2):
        out += struct.pack("<h", ((i * 7) % 2000) - 1000)
    return bytes(out)


@pytest.fixture(scope="session")
def synthetic_files():
    boot_plain = fake_sh4_binary()
    files = {
        "IP.BIN": make_ipbin(),
        "1ST_READ.BIN": scramble(boot_plain),
        "DATA/LEVEL01.BIN": fake_sh4_binary(8192, seed=7),      # an "overlay" that is code
        "DATA/TEXTURE.PVR": bytes(range(256)) * 40,              # data, not code
        "SOUND/BGM01.ADX": b"\x80\x00" + bytes(30000),
        "README.TXT": b"synthetic test disc for dcdisc\n",
    }
    return files, boot_plain


@pytest.fixture(scope="session")
def gdi_dir(tmp_path_factory, synthetic_files):
    files, _ = synthetic_files
    d = tmp_path_factory.mktemp("gdi")
    iso = build_iso(files)
    assert len(iso) % 2048 == 0
    # IP.BIN must occupy the first 16 sectors of the track: pycdlib's system area (sectors 0-15)
    # is exactly that space, so splice it in.
    iso = files["IP.BIN"] + iso[0x8000:]

    t1_sectors, t2_sectors = 300, 60
    with open(d / "track01.raw", "wb") as f:
        f.write(audio_pattern(t1_sectors))
    with open(d / "track02.bin", "wb") as f:
        for i in range(t2_sectors):
            f.write(mode1_raw_sector(bytes([i & 0xFF]) * 2048, t1_sectors + i))
    with open(d / "track03.bin", "wb") as f:
        for i in range(len(iso) // 2048):
            f.write(mode1_raw_sector(iso[i * 2048:(i + 1) * 2048], HD_LBA + i))
    gdi = d / "synthetic.gdi"
    gdi.write_text("3\n"
                   f"1 0 0 2352 track01.raw 0\n"
                   f"2 {t1_sectors} 4 2352 track02.bin 0\n"
                   f"3 {HD_LBA} 4 2352 track03.bin 0\n")
    return d


@pytest.fixture(scope="session")
def gdi_path(gdi_dir):
    return str(gdi_dir / "synthetic.gdi")


@pytest.fixture(scope="session")
def cooked_gdi_path(tmp_path_factory, synthetic_files):
    """Same disc but with a 2048-byte-sector data track, as some dumps are."""
    files, _ = synthetic_files
    d = tmp_path_factory.mktemp("gdi2048")
    iso = files["IP.BIN"] + build_iso(files)[0x8000:]
    (d / "track01.raw").write_bytes(audio_pattern(10))
    (d / "track03.bin").write_bytes(iso)
    gdi = d / "cooked.gdi"
    gdi.write_text(f"2\n1 0 0 2352 track01.raw 0\n3 {HD_LBA} 4 2048 track03.bin 0\n")
    return str(gdi)


FIXTURE_CHD = os.path.join(os.path.dirname(__file__), "fixtures", "synthetic.chd")


@pytest.fixture(scope="session")
def chd_path(tmp_path_factory, gdi_path):
    """A CHD of the synthetic GDI: made with chdman when available, else the committed fixture."""
    from dcdisc.chd import find_chdman
    chdman = find_chdman()
    if chdman:
        import subprocess
        out = tmp_path_factory.mktemp("chd") / "synthetic.chd"
        subprocess.run([chdman, "createcd", "-i", gdi_path, "-o", str(out), "-f"],
                       check=True, capture_output=True)
        return str(out)
    if os.path.exists(FIXTURE_CHD):
        return FIXTURE_CHD
    pytest.skip("no chdman and no committed CHD fixture")
