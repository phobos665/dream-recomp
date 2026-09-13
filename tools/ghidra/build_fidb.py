#!/usr/bin/env python3
"""Build a Ghidra Function ID database from a Katana SDK's library files (ADR 8, checklist step 4).

Adapted for macOS/Linux from AltoRetrato's build_dc_fidb.py (Dreamcast RE diaries). Stages the
SDK's ELF-format library objects into the folder layout Ghidra's CreateMultipleLibraries.java
expects, imports and analyses them headlessly, then creates and populates a .fidb.

Handled today:
  Lib/Gnu/*.a         GNU ar archives of sh-elf ELF objects (GCC-built library variants)
  Lib/Mwerks/*.elf.lib, *.obj.elf   ar archives / loose ELF objects (CodeWarrior-built variants)
Not handled yet:
  Lib/*.lib, *.obj    Hitachi SYSROF format (SHC-built variants). Needs the SDK's libsplit.exe and
                      elfcnv.exe under Wine, or a SYSROF-to-ELF converter. If a game was built with
                      SHC, its library code matches only these, so this gap matters.

Usage:
  build_fidb.py SDK_ROOT OUT_DIR [--name katana-r10.1] [--ghidra /path/to/ghidra/libexec]

The .fidb and the staging folder are SDK derivatives: keep them under sdk/ (gitignored).
"""

from __future__ import annotations

import argparse
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

LANGUAGE = "SuperH4:LE:32:default"
ELF_MAGIC = b"\x7fELF"
AR_MAGIC = b"!<arch>\n"


def is_elf(p: Path) -> bool:
    try:
        with open(p, "rb") as f:
            return f.read(4) == ELF_MAGIC
    except OSError:
        return False


def is_ar(p: Path) -> bool:
    try:
        with open(p, "rb") as f:
            return f.read(8) == AR_MAGIC
    except OSError:
        return False


def ar_extract(archive: Path, dest: Path) -> int:
    """Extract a GNU/SysV ar archive (with the '//' long-name table). macOS `ar` cannot."""
    data = archive.read_bytes()
    assert data[:8] == AR_MAGIC, archive
    pos = 8
    longnames = b""
    n = 0
    seen: dict[str, int] = {}
    while pos + 60 <= len(data):
        hdr = data[pos:pos + 60]
        name = hdr[0:16].decode("latin1").rstrip()
        size = int(hdr[48:58].decode("ascii").strip() or "0")
        body = data[pos + 60:pos + 60 + size]
        pos += 60 + size + (size & 1)
        if name in ("/", "/SYM64/"):        # symbol table
            continue
        if name == "//":                    # long-name table
            longnames = body
            continue
        if name.startswith("/") and name[1:].isdigit():
            off = int(name[1:])
            end = longnames.index(b"/\n", off) if b"/\n" in longnames[off:] else longnames.index(b"\n", off)
            name = longnames[off:end].decode("latin1")
        elif name.startswith("#1/"):        # BSD long name: name follows header
            ln = int(name[3:])
            name = body[:ln].decode("latin1").rstrip("\0")
            body = body[ln:]
        name = name.rstrip("/") or f"member{n}"
        name = name.replace("/", "_")
        if name in seen:
            seen[name] += 1
            stem, dot, ext = name.rpartition(".")
            name = f"{stem or ext}_{seen[name]}.{ext}" if dot else f"{name}_{seen[name]}"
        else:
            seen[name] = 0
        if body[:4] != ELF_MAGIC:
            continue
        out = dest / (name if name.endswith(".o") else name + ".o")
        out.write_bytes(body)
        n += 1
    return n


def stage(sdk_root: Path, work: Path, sdk_name: str) -> tuple[dict[str, int], list[str]]:
    """Copy/extract ELF objects into work/<compiler>/<library>/<version>/<sdk_name>/."""
    lib = sdk_root / "Lib"
    counts: dict[str, int] = {}
    skipped: list[str] = []
    jobs: list[tuple[str, Path]] = []
    for p in sorted((lib / "Gnu").glob("*")) if (lib / "Gnu").is_dir() else []:
        if p.is_file():
            jobs.append(("gnu", p))
    for p in sorted((lib / "Mwerks").glob("*")) if (lib / "Mwerks").is_dir() else []:
        if p.is_file():
            jobs.append(("mwerks", p))
    for p in sorted(lib.glob("*")):
        if p.is_file() and p.suffix.lower() in (".lib", ".obj") and not is_elf(p) and not is_ar(p):
            skipped.append(p.name)  # Hitachi SYSROF
    for compiler, p in jobs:
        library = p.name.split(".")[0]
        variant = "debug" if library.endswith("_d") else "release"
        dest = work / compiler / library / variant / sdk_name
        dest.mkdir(parents=True, exist_ok=True)
        if is_ar(p):
            counts[compiler] = counts.get(compiler, 0) + ar_extract(p, dest)
        elif is_elf(p):
            shutil.copy(p, dest / (p.stem + ".o"))
            counts[compiler] = counts.get(compiler, 0) + 1
    return counts, skipped


def write_properties(work: Path, fidb: Path, compiler: str) -> None:
    (work / "duplicates.txt").touch()
    (work / "common_symbols.txt").touch()
    (work / "CreateMultipleLibraries.properties").write_text(
        f"Duplicate Results File OK = {work / 'duplicates.txt'}\n"
        "Do Duplication Detection Do you want to detect duplicates = true\n"
        f"Choose destination FidDB Please choose the destination FidDB for population = {fidb.name}\n"
        f"Select root folder containing all libraries (at a depth of 3): = /{compiler}\n"
        f"Common symbols file (optional): OK = {work / 'common_symbols.txt'}\n"
        f"Enter LanguageID To Process Language ID: = {LANGUAGE}\n")
    (work / "CreateEmptyFidDatabase.properties").write_text(
        f"Create new FidDb file Create = {fidb}\n")
    # Attachment of a user FidDb does not persist between headless sessions, so every populate
    # pass after the first re-attaches the file created by the first.
    (work / "AttachFidDatabase.properties").write_text(
        f"Attach existing FidDb Attach = {fidb}\n")


def find_ghidra(explicit: str | None) -> Path:
    if explicit:
        return Path(explicit)
    env = os.environ.get("GHIDRA_HOME")
    if env:
        return Path(env)
    try:
        prefix = subprocess.check_output(["brew", "--prefix", "ghidra"], text=True).strip()
        return Path(prefix) / "libexec"
    except Exception:
        sys.exit("Ghidra not found: pass --ghidra or set GHIDRA_HOME")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("sdk_root")
    ap.add_argument("out_dir")
    ap.add_argument("--name", default=None, help="fidb basename (default: SDK folder name)")
    ap.add_argument("--ghidra", default=None, help="Ghidra install dir containing support/analyzeHeadless")
    ap.add_argument("--stage-only", action="store_true")
    ap.add_argument("--populate-only", nargs="*", metavar="FAMILY",
                    help="skip staging/import; (re)run the populate pass for these families against the existing project and fidb")
    args = ap.parse_args()

    sdk_root = Path(args.sdk_root).resolve()
    out = Path(args.out_dir).resolve()
    name = args.name or sdk_root.name
    work = out / f"fid_work_{name}"
    fidb = out / f"{name}.fidb"
    if args.populate_only is not None:
        ghidra = find_ghidra(args.ghidra)
        analyze = ghidra / "support" / "analyzeHeadless"
        for c in args.populate_only:
            write_properties(work, fidb, c)
            cmd = [str(analyze), "ghidraproj", name, "-noanalysis", "-propertiesPath", str(work),
                   "-preScript", "AttachFidDatabase.java" if fidb.exists() else "CreateEmptyFidDatabase.java",
                   "-preScript", "CreateMultipleLibraries.java", "-log", f"generation_{c}.log"]
            print(f"[run] populate ({c})")
            subprocess.check_call(cmd, cwd=work)
        print(f"[done] {fidb} ({fidb.stat().st_size // 1024} KB)")
        return 0

    if work.exists():
        shutil.rmtree(work)
    (work / "ghidraproj").mkdir(parents=True)
    if fidb.exists():
        fidb.unlink()

    counts, skipped = stage(sdk_root, work, name)
    print(f"[stage] objects per compiler family: {counts}")
    if skipped:
        print(f"[stage] skipped {len(skipped)} Hitachi SYSROF files (need libsplit/elfcnv or a converter): "
              + ", ".join(skipped[:8]) + (" ..." if len(skipped) > 8 else ""))
    if not counts:
        sys.exit("no ELF objects staged")
    if args.stage_only:
        return 0

    ghidra = find_ghidra(args.ghidra)
    analyze = ghidra / "support" / "analyzeHeadless"
    if not analyze.exists():
        sys.exit(f"analyzeHeadless not found at {analyze}")

    compilers = list(counts)
    cmd1 = [str(analyze), "ghidraproj", name]
    for c in compilers:
        cmd1 += ["-import", str(work / c)]
    cmd1 += ["-recursive", "-processor", LANGUAGE,
             "-preScript", "FunctionIDHeadlessPrescript.java",
             "-postScript", "FunctionIDHeadlessPostscript.java",
             "-scriptlog", "script.log", "-log", "analyze.log"]
    print("[run] import + analysis:", " ".join(cmd1[:4]), "...")
    subprocess.check_call(cmd1, cwd=work)

    # One populate pass per compiler family; the first pass also creates and attaches the database.
    for i, c in enumerate(compilers):
        write_properties(work, fidb, c)
        cmd2 = [str(analyze), "ghidraproj", name, "-noanalysis", "-propertiesPath", str(work)]
        cmd2 += ["-preScript", "CreateEmptyFidDatabase.java" if i == 0 else "AttachFidDatabase.java"]
        cmd2 += ["-preScript", "CreateMultipleLibraries.java",
                 "-log", f"generation_{c}.log"]
        print(f"[run] populate ({c})")
        subprocess.check_call(cmd2, cwd=work)

    print(f"[done] {fidb} ({fidb.stat().st_size // 1024} KB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
