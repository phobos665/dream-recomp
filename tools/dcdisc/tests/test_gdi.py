import hashlib
import os

from dcdisc import open_image
from dcdisc.gdi import GdiImage, write_gdi
from dcdisc.iso9660 import Iso9660
from dcdisc.ipbin import parse_ipbin, read_ipbin
from dcdisc.scramble import descramble
from dcdisc import codescan


def test_tracks(gdi_path):
    with open_image(gdi_path) as img:
        assert isinstance(img, GdiImage)
        assert [t.number for t in img.tracks] == [1, 2, 3]
        assert img.tracks[2].lba == 45000
        assert img.hd_track.number == 3
        assert not img.tracks[0].is_data and img.tracks[2].is_data


def test_ipbin_and_filesystem(gdi_path, synthetic_files):
    files, boot_plain = synthetic_files
    with open_image(gdi_path) as img:
        ip = parse_ipbin(read_ipbin(img))
        assert ip.title == "SYNTHETIC TEST DISC"
        fs = Iso9660(img)
        assert fs.offset == 45000  # pycdlib writes track-relative LBAs
        names = sorted(e.path for e in fs.files())
        assert names == sorted("/" + p for p in files)
        for p, data in files.items():
            assert fs.read_path(p) == data, p
        assert descramble(fs.read_path("1ST_READ.BIN")) == boot_plain


def test_cooked_data_track(cooked_gdi_path, synthetic_files):
    files, _ = synthetic_files
    with open_image(cooked_gdi_path) as img:
        assert img.hd_track.sector_size == 2048
        fs = Iso9660(img)
        assert fs.read_path("README.TXT") == files["README.TXT"]


def test_codescan_on_synthetic_boot(synthetic_files):
    files, boot_plain = synthetic_files
    plain = codescan.score_code(boot_plain)
    scr = codescan.score_code(files["1ST_READ.BIN"])
    assert plain.looks_like_code
    # Scrambling permutes 32-byte slices, so per-word statistics survive it...
    assert scr.looks_like_code
    # ...but literal-pool references do not, which is the check that matters.
    assert plain.looks_ordered and plain.pointer_ratio > 0.8
    assert not scr.looks_ordered and scr.pointer_ratio < 0.3
    assert not codescan.score_code(files["DATA/TEXTURE.PVR"]).looks_like_code
    assert codescan.score_code(files["DATA/LEVEL01.BIN"]).looks_like_code
    banners = codescan.find_banners(boot_plain)
    assert "Ninja" in banners and "KAMUI" in banners


def test_write_gdi_roundtrip(gdi_path, tmp_path):
    with open_image(gdi_path) as img:
        out = write_gdi(img, str(tmp_path / "out"), "copy")
    with open_image(gdi_path) as a, open_image(out) as b:
        assert [(t.lba, t.sectors, t.sector_size, t.is_data) for t in a.tracks] == \
               [(t.lba, t.sectors, t.sector_size, t.is_data) for t in b.tracks]
        for t in a.tracks:
            ha = hashlib.sha256(b"".join(a.iter_track_raw(t))).hexdigest()
            hb = hashlib.sha256(b"".join(b.iter_track_raw(t))).hexdigest()
            assert ha == hb, t.number
