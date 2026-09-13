import hashlib
import os

import pytest

from dcdisc import open_image
from dcdisc.chd import (ChdImage, ChdmanImage, find_chdman, load_libchdr, parse_tracks,
                        read_header, read_metadata)
from dcdisc.iso9660 import Iso9660
from conftest import FIXTURE_CHD


def _needs_fresh_chd(chd_path):
    # The committed fixture was built from an ISO with different timestamps than the GDI the
    # session just generated, so byte-equality tests only make sense on a chdman-made CHD.
    if chd_path == FIXTURE_CHD:
        pytest.skip("byte-equality tests need chdman to build a CHD from this session's GDI")


def _track_hashes(img):
    return {t.number: hashlib.sha256(b"".join(img.iter_track_raw(t))).hexdigest()
            for t in img.tracks}


def test_header_and_metadata_pure_python(chd_path):
    hdr = read_header(chd_path)
    assert hdr.version == 5
    assert hdr.unit_bytes == 2448
    assert hdr.hunk_bytes % 2448 == 0
    tracks, is_gdrom = parse_tracks(read_metadata(chd_path))
    assert is_gdrom
    assert [t.number for t in tracks] == [1, 2, 3]
    assert tracks[2].lba == 45000, "PAD field must reconstruct the LBA 45000 jump"
    assert tracks[1].sectors == 60 and tracks[1].pad == 44640
    assert tracks[2].chd_frame == 300 + 44700
    assert tracks[2].type == "MODE1_RAW"
    assert tracks[0].type == "AUDIO"


@pytest.mark.skipif(load_libchdr() is None, reason="libchdr not available")
def test_libchdr_matches_gdi(chd_path, gdi_path):
    _needs_fresh_chd(chd_path)
    with ChdImage(chd_path) as chd, open_image(gdi_path) as gdi:
        assert [(t.lba, t.sectors, t.sector_size) for t in chd.tracks] == \
               [(t.lba, t.sectors, t.sector_size) for t in gdi.tracks]
        assert _track_hashes(chd) == _track_hashes(gdi)
        assert Iso9660(chd).read_path("README.TXT") == Iso9660(gdi).read_path("README.TXT")


@pytest.mark.skipif(find_chdman() is None, reason="chdman not available")
def test_chdman_backend_matches_gdi(chd_path, gdi_path):
    with ChdmanImage(chd_path) as chd, open_image(gdi_path) as gdi:
        assert _track_hashes(chd) == _track_hashes(gdi)


@pytest.mark.skipif(load_libchdr() is None, reason="libchdr not available")
def test_chd2gdi_roundtrip(chd_path, gdi_path, tmp_path):
    _needs_fresh_chd(chd_path)
    from dcdisc.gdi import write_gdi
    with open_image(chd_path) as chd:
        out = write_gdi(chd, str(tmp_path), "rt")
    with open_image(out) as back, open_image(gdi_path) as gdi:
        assert _track_hashes(back) == _track_hashes(gdi)
