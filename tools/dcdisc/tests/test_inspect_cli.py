import json

from dcdisc import open_image
from dcdisc.cli import main
from dcdisc.inspect import inspect_image, report_markdown


def test_inspect_report(gdi_path):
    with open_image(gdi_path) as img:
        r = inspect_image(img)
    assert not r.wince
    assert r.boot_file == "1ST_READ.BIN"
    assert r.boot_descrambled_looks_like_code
    assert r.boot_descrambled_pointer_ratio > 0.8 > r.boot_scrambled_pointer_ratio
    assert r.boot_was_scrambled
    assert "DATA/LEVEL01.BIN" in r.code_files
    assert "DATA/LEVEL01.BIN" in r.referenced_files      # overlay case detected
    assert "SOUND/BGM01.ADX" in r.referenced_files
    assert r.streaming_files and r.streaming_files[0]["path"] == "SOUND/BGM01.ADX"
    assert "Ninja" in r.banners
    md = report_markdown(r)
    assert "Windows CE:** pass" in md
    assert "45000" in md


def test_cli_smoke(gdi_path, tmp_path, capsys):
    assert main(["info", gdi_path]) == 0
    assert "SYNTHETIC TEST DISC" in capsys.readouterr().out
    assert main(["ls", gdi_path]) == 0
    assert "/1ST_READ.BIN" in capsys.readouterr().out
    out = tmp_path / "x"
    assert main(["extract", gdi_path, str(out)]) == 0
    capsys.readouterr()
    assert (out / "IP.BIN").stat().st_size == 0x8000
    assert (out / "fs" / "DATA" / "LEVEL01.BIN").exists()
    assert (out / "1ST_READ.plain.bin").exists()
    assert main(["manifest", gdi_path]) == 0
    m = json.loads(capsys.readouterr().out)
    assert m["files"]["1ST_READ.BIN"]["boot_stored_scrambled"] is True
    rep = tmp_path / "r.md"
    assert main(["inspect", gdi_path, "-o", str(rep)]) == 0
    assert rep.read_text().startswith("# Disc verification report")
    assert main(["backends"]) == 0
    capsys.readouterr()
    assert main(["shortlist", gdi_path, gdi_path]) == 0
    out = capsys.readouterr().out
    assert out.count("SYNTHETIC TEST DISC") == 2 and "Ninja,KAMUI" in out
