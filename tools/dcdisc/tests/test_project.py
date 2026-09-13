"""`dcdisc new-game` and `dcdisc doctor`, on the synthetic disc.

new-game writes the two files a title needs and nothing else. The things worth pinning are the ones
that would be silently wrong rather than loudly broken: the boot filename comes from IP.BIN rather
than being assumed, and the generated config is the shape the C++ loader accepts.
"""

import os

from dcdisc.cli import main
from dcdisc.project import _slug


def test_slug_from_a_title():
    assert _slug("CHARGEN BLAST") == "chargenblast"
    assert _slug("MSR - Metropolis Street Racer") == "msrmetropolisstreetracer"
    assert _slug("!!!") == "game"  # never an empty directory name


def test_new_game_writes_config_and_build_file(gdi_path, tmp_path):
    games = tmp_path / "games"
    rc = main(["new-game", gdi_path, "synth", "--dir", str(games), "--no-extract"])
    assert rc == 0
    toml = (games / "synth" / "synth.toml").read_text()
    # Read off IP.BIN, not assumed. The synthetic disc's boot file happens to be the usual name;
    # a title whose IP.BIN says otherwise (Charge 'N Blast says 1ST_READ.US) must follow it.
    assert 'path = "extracted/fs/1ST_READ.BIN"' in toml
    assert 'id = "synth"' in toml
    assert "load_address = 0x8C010000" in toml
    assert "link_address = 0x0C010000" in toml
    assert "entry = 0x8C010000" in toml
    cmake = (games / "synth" / "CMakeLists.txt").read_text()
    assert "dream_add_game(synth" in cmake
    # --no-extract writes the files and nothing else.
    assert not (games / "synth" / "extracted").exists()


def test_new_game_refuses_to_overwrite(gdi_path, tmp_path):
    games = tmp_path / "games"
    assert main(["new-game", gdi_path, "synth", "--dir", str(games), "--no-extract"]) == 0
    # A second run must not quietly discard a config someone has since edited by hand.
    assert main(["new-game", gdi_path, "synth", "--dir", str(games), "--no-extract"]) == 2
    assert main(["new-game", gdi_path, "synth", "--dir", str(games), "--no-extract",
                 "--force"]) == 0


def test_new_game_rejects_a_bad_id(gdi_path, tmp_path):
    assert main(["new-game", gdi_path, "../escape", "--dir", str(tmp_path), "--no-extract"]) == 2


def test_doctor_runs_and_reports(capsys):
    # It reports rather than asserting an environment, so the check is that it produces the table
    # and an exit code, on whatever machine happens to be running it.
    rc = main(["doctor"])
    out = capsys.readouterr().out
    assert "Required" in out and "Optional" in out
    assert "CMake 3.24+" in out
    assert rc in (0, 1)


def test_new_game_title_override_and_brief(gdi_path, tmp_path, capsys):
    games = tmp_path / "games"
    rc = main(["new-game", gdi_path, "synth", "--dir", str(games), "--no-extract",
               "--title", "My Own Name", "--brief"])
    assert rc == 0
    toml = (games / "synth" / "synth.toml").read_text()
    assert 'title = "My Own Name"' in toml
    assert 'dream_add_game(synth TITLE "My Own Name")' in (
        games / "synth" / "CMakeLists.txt").read_text()
    # --brief is for a caller that is about to run those steps itself.
    assert "next:" not in capsys.readouterr().out
