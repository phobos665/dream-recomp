"""The automatable part of the baseline disc verification checklist (docs/baseline-game.md).

Steps 1-7 are produced here from the image alone. Steps 8-9 (syscall trace, timing probe) need a
running emulator and are listed as manual follow-ups in the report.
"""

from __future__ import annotations

import hashlib
import json
import os
from dataclasses import asdict, dataclass, field
from typing import Optional

from . import codescan
from .image import DiscImage, HD_AREA_LBA
from .ipbin import IpBin, parse_ipbin, read_ipbin
from .iso9660 import Entry, Iso9660
from .scramble import descramble


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


@dataclass
class FileInfo:
    path: str
    size: int
    sha256: str
    code_score: Optional[dict] = None
    looks_like_code: bool = False


@dataclass
class Report:
    image: str
    image_kind: str
    tracks: list[dict]
    ipbin: dict
    ipbin_sha256: str
    wince: bool
    wince_reason: list[str]
    files: list[FileInfo]
    code_files: list[str]
    boot_file: str
    boot_size: int
    boot_sha256_scrambled: str
    boot_sha256_descrambled: str
    boot_descrambled_looks_like_code: bool
    boot_scrambled_looks_like_code: bool
    boot_descrambled_pointer_ratio: float
    boot_scrambled_pointer_ratio: float
    boot_was_scrambled: bool
    banners: dict
    referenced_files: list[str]
    streaming_files: list[dict]
    streaming_bytes: int
    total_bytes: int
    notes: list[str] = field(default_factory=list)


def inspect_image(image: DiscImage, scan_all: bool = True) -> Report:
    notes: list[str] = []
    tracks = [dict(number=t.number, lba=t.lba, sectors=t.sectors, sector_size=t.sector_size,
                   type="data" if t.is_data else "audio") for t in image.tracks]
    if not any(t.is_data and t.lba >= HD_AREA_LBA for t in image.tracks):
        notes.append("No data track at LBA 45000 or above: this is not a GD-ROM layout "
                     "(CDI-style self-boot or homebrew disc).")

    ip_raw = read_ipbin(image)
    ip = parse_ipbin(ip_raw)
    if not ip.looks_valid:
        notes.append("IP.BIN hardware ID is not 'SEGA SEGAKATANA'; boot header may be damaged.")
    if not ip.crc_ok:
        notes.append(f"IP.BIN CRC mismatch: stored {ip.crc_stored:04X}, "
                     f"computed {ip.crc_computed:04X}.")

    fs = Iso9660(image)
    entries = fs.files()
    names = [e.path.lstrip("/") for e in entries]

    wince_reason = []
    if ip.is_wince:
        wince_reason.append("IP.BIN peripherals bit 0 (Windows CE) is set")
    for n in names:
        if n.rsplit("/", 1)[-1].upper() in codescan.WINCE_FILES:
            wince_reason.append(f"{n} present on disc")

    files: list[FileInfo] = []
    code_files: list[str] = []
    total = 0
    streaming = []
    stream_bytes = 0
    boot_name = ip.boot_filename or "1ST_READ.BIN"
    boot_entry: Optional[Entry] = None
    for e in entries:
        data = fs.read(e)
        total += e.size
        info = FileInfo(e.path.lstrip("/"), e.size, sha256(data))
        if e.name.upper() == boot_name.upper():
            boot_entry = e
        ext = os.path.splitext(e.name)[1].upper()
        if ext in codescan.STREAMING_EXTS:
            streaming.append(dict(path=info.path, size=e.size))
            stream_bytes += e.size
        if scan_all or e.name.upper().endswith(".BIN"):
            sc = codescan.score_code(data)
            info.code_score = asdict(sc)
            info.looks_like_code = sc.looks_like_code
            if sc.looks_like_code:
                code_files.append(info.path)
        files.append(info)

    if boot_entry is None:
        raise FileNotFoundError(f"boot file {boot_name} not found in filesystem")
    boot_stored = fs.read(boot_entry)
    boot_desc = descramble(boot_stored)
    sc_stored = codescan.score_code(boot_stored)
    sc_desc = codescan.score_code(boot_desc)
    # Scrambling preserves per-word statistics, so "looks like code" is true either way; the
    # literal-pool pointer ratio is what tells the two apart. GD-ROM dumps store the boot binary
    # plain; CD-R style images (CDI) store it scrambled.
    was_scrambled = sc_desc.pointer_ratio > sc_stored.pointer_ratio
    if was_scrambled:
        boot_plain, sc_plain, sc_scr = boot_desc, sc_desc, sc_stored
    else:
        boot_plain, sc_plain, sc_scr = boot_stored, sc_stored, sc_desc
    if not sc_plain.looks_like_code:
        notes.append("Boot binary does not score as SH-4 code at all; check the dump.")
    elif was_scrambled:
        notes.append("Boot binary is stored scrambled (CD-R style image); descrambled for analysis.")
    if sc_plain.looks_like_code and not sc_plain.looks_ordered:
        notes.append(f"Descrambled boot binary has a low literal-pool pointer ratio "
                     f"({sc_plain.pointer_ratio:.2f}); the descrambler output deserves a manual look.")
    banners = codescan.find_banners(boot_plain)
    referenced = codescan.referenced_filenames(boot_plain, [n for n in names
                                                             if n.upper() != boot_name.upper()])
    # A referenced file that also scores as code is the overlay case the checklist warns about.
    code_files = sorted(set(code_files))

    return Report(
        image=image.path,
        image_kind=type(image).__name__,
        tracks=tracks,
        ipbin={k: v for k, v in asdict(ip).items()} | dict(
            peripheral_names=ip.peripheral_names, regions=ip.regions, crc_ok=ip.crc_ok),
        ipbin_sha256=sha256(ip_raw),
        wince=bool(wince_reason),
        wince_reason=wince_reason,
        files=files,
        code_files=code_files,
        boot_file=boot_entry.path.lstrip("/"),
        boot_size=boot_entry.size,
        boot_sha256_scrambled=sha256(boot_stored),
        boot_sha256_descrambled=sha256(boot_plain),
        boot_descrambled_looks_like_code=sc_plain.looks_like_code,
        boot_scrambled_looks_like_code=sc_scr.looks_like_code,
        boot_descrambled_pointer_ratio=round(sc_plain.pointer_ratio, 3),
        boot_scrambled_pointer_ratio=round(sc_scr.pointer_ratio, 3),
        boot_was_scrambled=was_scrambled,
        banners=banners,
        referenced_files=referenced,
        streaming_files=streaming,
        streaming_bytes=stream_bytes,
        total_bytes=total,
        notes=notes,
    )


def _mb(n: int) -> str:
    return f"{n / (1024 * 1024):.1f} MB"


def report_markdown(r: Report) -> str:
    ip = r.ipbin
    lines = [
        f"# Disc verification report: {ip.get('title') or os.path.basename(r.image)}",
        "",
        f"Image: `{os.path.basename(r.image)}` read via {r.image_kind}.",
        "",
        "## Tracks",
        "",
        "| # | LBA | Sectors | Sector size | Type |",
        "|---|---|---|---|---|",
    ]
    for t in r.tracks:
        lines.append(f"| {t['number']} | {t['lba']} | {t['sectors']} | {t['sector_size']} | {t['type']} |")
    lines += [
        "",
        "## IP.BIN",
        "",
        f"- Title: {ip['title']}",
        f"- Product: {ip['product_number']} {ip['product_version']}, released {ip['release_date']}",
        f"- Company: {ip['company']}; maker: {ip['maker_id']}",
        f"- Regions: {', '.join(ip['regions']) or ip['area_symbols']}",
        f"- Boot file: {ip['boot_filename']}",
        f"- Device: {ip['device_info']} (CRC {'ok' if ip['crc_ok'] else 'MISMATCH'})",
        f"- Peripherals 0x{int(ip['peripherals_raw'] or '0', 16):07X}: "
        f"{', '.join(ip['peripheral_names']) or 'none flagged'}",
        f"- IP.BIN SHA-256: `{r.ipbin_sha256}`",
        "",
        "## Checklist",
        "",
        f"1. **Windows CE:** {'FAIL, ' + '; '.join(r.wince_reason) if r.wince else 'pass, no WinCE markers'}",
        f"2. **Code files on disc:** {', '.join(r.code_files) if r.code_files else 'none detected'}"
        + (" (expected: only the boot binary)" if len(r.code_files) <= 1 else
           "  -> more than one code file; check for overlays"),
        f"3. **Boot binary:** {r.boot_file}, {_mb(r.boot_size)} ({r.boot_size} bytes)",
        f"   - as stored on disc, SHA-256 `{r.boot_sha256_scrambled}`",
        f"   - plain (analysable) form, SHA-256 `{r.boot_sha256_descrambled}`"
        + ("" if r.boot_was_scrambled else " (same bytes: stored plain, as GD-ROMs do)"),
        f"   - scores as SH-4 code: {'yes' if r.boot_descrambled_looks_like_code else 'NO'}; "
        f"literal-pool pointer ratio {r.boot_descrambled_pointer_ratio:.2f} in plain form vs "
        f"{r.boot_scrambled_pointer_ratio:.2f} in the other "
        f"({'stored scrambled, CD-R style' if r.boot_was_scrambled else 'stored plain'})",
        f"4. **SDK banners in boot binary:** "
        + (", ".join(f"{k} x{v}" for k, v in sorted(r.banners.items())) or "none found")
        + ". FID match rate is measured in Ghidra, not here.",
        f"5. **Disc files referenced by name from the boot binary:** "
        + (", ".join(r.referenced_files) if r.referenced_files else "none")
        + ". Any of these that also appears in step 2 is a runtime-loaded code overlay.",
        f"6. **Streaming media:** {len(r.streaming_files)} files, {_mb(r.streaming_bytes)} of "
        f"{_mb(r.total_bytes)} total filesystem data",
        f"7. **Peripherals:** {', '.join(ip['peripheral_names']) or 'none flagged'}",
        "8. **BIOS syscall trace:** manual, run in Flycast with the GDB server (see checklist).",
        "9. **Timing probe:** manual, confirm VBlank lock in Flycast (see checklist).",
        "",
    ]
    if r.notes:
        lines += ["## Notes", ""] + [f"- {n}" for n in r.notes] + [""]
    lines += ["## Files", "", "| Path | Size | SHA-256 | Code? |", "|---|---|---|---|"]
    for f in r.files:
        lines.append(f"| {f.path} | {f.size} | `{f.sha256[:16]}...` | {'yes' if f.looks_like_code else ''} |")
    return "\n".join(lines) + "\n"


def report_json(r: Report) -> str:
    d = asdict(r)
    return json.dumps(d, indent=2)
