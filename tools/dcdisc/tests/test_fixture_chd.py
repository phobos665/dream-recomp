"""Reads the committed synthetic CHD (made by chdman 0.264 from the conftest GDI) so the libchdr
path is exercised in environments without chdman. Timestamps inside the ISO differ between runs,
so this checks structure and content rather than byte equality with a freshly built GDI."""
import os

import pytest

from dcdisc.chd import ChdImage, load_libchdr, parse_tracks, read_metadata
from dcdisc.inspect import inspect_image
from dcdisc.iso9660 import Iso9660
from dcdisc.scramble import descramble
from dcdisc import codescan

FIXTURE = os.path.join(os.path.dirname(__file__), "fixtures", "synthetic.chd")


def test_fixture_metadata():
    tracks, is_gdrom = parse_tracks(read_metadata(FIXTURE))
    assert is_gdrom
    assert [(t.number, t.lba, t.sectors) for t in tracks] == [(1, 0, 300), (2, 300, 60), (3, 45000, 79)]


@pytest.mark.skipif(load_libchdr() is None, reason="libchdr not available")
def test_fixture_contents():
    with ChdImage(FIXTURE) as img:
        fs = Iso9660(img)
        assert fs.read_path("README.TXT") == b"synthetic test disc for dcdisc\n"
        boot = descramble(fs.read_path("1ST_READ.BIN"))
        assert codescan.score_code(boot).looks_ordered
        r = inspect_image(img)
        assert r.boot_was_scrambled and not r.wince
        assert r.ipbin["title"] == "SYNTHETIC TEST DISC"
