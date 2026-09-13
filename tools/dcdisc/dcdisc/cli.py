"""dcdisc command line.

    dcdisc backends                       show which CHD backend would be used
    dcdisc info IMAGE                     tracks and IP.BIN summary
    dcdisc ls IMAGE                       list the filesystem
    dcdisc extract IMAGE OUTDIR           IP.BIN, filesystem, descrambled boot binary
    dcdisc chd2gdi IMAGE.chd OUTDIR       convert CHD to GDI (uses libchdr or chdman)
    dcdisc inspect IMAGE [-o REPORT.md]   run the automatable checklist steps
    dcdisc manifest IMAGE                 JSON with per-file SHA-256
    dcdisc descramble IN OUT / scramble IN OUT
    dcdisc scan FILE                      SH-4 code heuristics and SDK banners for one file
    dcdisc shortlist IMAGE [IMAGE...]     one comparison row per disc, for choosing a baseline title
"""

from __future__ import annotations

import argparse
import json
import os
import sys

from . import __version__, codescan
from .image import open_image
from .ipbin import parse_ipbin, read_ipbin
from .iso9660 import Iso9660
from .scramble import descramble, ensure_plain, scramble


def _open(args):
    return open_image(args.image, prefer_chdman=getattr(args, "chdman", False))


def cmd_backends(args) -> int:
    from .chd import backend_status
    for k, v in backend_status().items():
        print(f"{k}: {v or 'not found'}")
    return 0


def cmd_info(args) -> int:
    with _open(args) as img:
        print(f"{args.image} ({type(img).__name__})")
        print(f"{'#':>2} {'LBA':>7} {'sectors':>8} {'size':>5} type")
        for t in img.tracks:
            print(f"{t.number:>2} {t.lba:>7} {t.sectors:>8} {t.sector_size:>5} "
                  f"{'data' if t.is_data else 'audio'}")
        ip = parse_ipbin(read_ipbin(img))
        print()
        print(f"Title:      {ip.title}")
        print(f"Product:    {ip.product_number} {ip.product_version} ({ip.release_date})")
        print(f"Company:    {ip.company}")
        print(f"Regions:    {', '.join(ip.regions) or ip.area_symbols}")
        print(f"Boot file:  {ip.boot_filename}")
        print(f"Device:     {ip.device_info}  CRC {'ok' if ip.crc_ok else 'MISMATCH'}")
        print(f"Periph:     0x{ip.peripherals:07X} {', '.join(ip.peripheral_names)}")
        print(f"WinCE:      {'YES' if ip.is_wince else 'no'}")
    return 0


def cmd_ls(args) -> int:
    with _open(args) as img:
        fs = Iso9660(img)
        print(f"volume '{fs.volume_id}', {'absolute' if fs.offset == 0 else 'track-relative'} LBAs")
        for e in fs.walk():
            kind = "d" if e.is_dir else "-"
            print(f"{kind} {e.size:>10} {e.lba:>7} {e.path}")
    return 0


def cmd_extract(args) -> int:
    with _open(args) as img:
        os.makedirs(args.outdir, exist_ok=True)
        ip_raw = read_ipbin(img)
        with open(os.path.join(args.outdir, "IP.BIN"), "wb") as f:
            f.write(ip_raw)
        ip = parse_ipbin(ip_raw)
        fs = Iso9660(img)
        fs_dir = os.path.join(args.outdir, "fs")
        n = 0
        boot = None
        for e in fs.walk():
            dest = os.path.join(fs_dir, e.path.lstrip("/"))
            if e.is_dir:
                os.makedirs(dest, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            data = fs.read(e)
            with open(dest, "wb") as f:
                f.write(data)
            n += 1
            if e.name.upper() == ip.boot_filename.upper():
                boot = data
        print(f"wrote IP.BIN and {n} files to {args.outdir}")
        if boot is not None and not args.no_descramble:
            plain, was_scrambled = ensure_plain(boot)
            out = os.path.join(args.outdir, os.path.splitext(ip.boot_filename)[0] + ".plain.bin")
            with open(out, "wb") as f:
                f.write(plain)
            state = ("was scrambled (CD-R style), descrambled" if was_scrambled
                     else "stored plain (GD-ROM), copied as-is")
            print(f"wrote boot binary to {out}: {state}")
        elif boot is None:
            print(f"warning: boot file {ip.boot_filename} not found", file=sys.stderr)
    return 0


def cmd_chd2gdi(args) -> int:
    from .gdi import write_gdi
    with _open(args) as img:
        def progress(t, i):
            end = "\n" if i >= t.sectors else ""
            print(f"\rtrack {t.number:02d}: {i}/{t.sectors} sectors", end=end, flush=True)
        path = write_gdi(img, args.outdir, args.name, progress)
        print(f"wrote {path}")
    return 0


def cmd_inspect(args) -> int:
    from .inspect import inspect_image, report_json, report_markdown
    with _open(args) as img:
        r = inspect_image(img, scan_all=not args.bin_only)
    text = report_json(r) if args.json else report_markdown(r)
    if args.output:
        with open(args.output, "w", encoding="utf-8") as f:
            f.write(text)
        print(f"wrote {args.output}")
    else:
        sys.stdout.write(text)
    return 1 if r.wince else 0


def cmd_manifest(args) -> int:
    from .inspect import sha256
    with _open(args) as img:
        fs = Iso9660(img)
        ip_raw = read_ipbin(img)
        ip = parse_ipbin(ip_raw)
        out = {"image": os.path.basename(args.image), "title": ip.title,
               "product": ip.product_number, "version": ip.product_version,
               "ipbin_sha256": sha256(ip_raw), "files": {}}
        for e in fs.files():
            data = fs.read(e)
            entry = {"size": e.size, "sha256": sha256(data)}
            if e.name.upper() == ip.boot_filename.upper():
                plain, was_scrambled = ensure_plain(data)
                entry["boot_stored_scrambled"] = was_scrambled
                entry["sha256_plain"] = sha256(plain)
            out["files"][e.path.lstrip("/")] = entry
    json.dump(out, sys.stdout, indent=2)
    print()
    return 0


def cmd_scramble(args, fn) -> int:
    with open(args.input, "rb") as f:
        data = f.read()
    with open(args.output, "wb") as f:
        f.write(fn(data))
    return 0


def cmd_scan(args) -> int:
    with open(args.file, "rb") as f:
        data = f.read()
    sc = codescan.score_code(data)
    print(f"{args.file}: {sc.size} bytes")
    print(f"  RTS {sc.rts}, RTS+NOP {sc.rts_nop}, prologue {sc.prologue}, epilogue {sc.epilogue}, "
          f"PC-relative loads {sc.pc_rel_loads}, boundary ops/KB {sc.per_kb:.2f}")
    print(f"  literal-pool pointer ratio {sc.pointer_ratio:.2f} ({sc.pc_rel_plausible}/{sc.pc_rel_loads})")
    print(f"  looks like SH-4 code: {'yes' if sc.looks_like_code else 'no'}; "
          f"ordered (not scrambled): {'yes' if sc.looks_ordered else 'no'}")
    banners = codescan.find_banners(data)
    if banners:
        print("  banners: " + ", ".join(f"{k} x{v}" for k, v in sorted(banners.items())))
    if args.strings:
        for s in codescan.strings(data, args.strings):
            print("  " + s)
    return 0


def cmd_shortlist(args) -> int:
    """Compare several discs on the checklist criteria that can be measured from the image."""
    from .inspect import inspect_image
    rows = []
    for path in args.images:
        try:
            with open_image(path, prefer_chdman=args.chdman) as img:
                r = inspect_image(img, scan_all=not args.fast)
        except Exception as e:  # keep going; one bad dump should not hide the others
            rows.append((os.path.basename(path), f"ERROR: {e}"))
            continue
        ip = r.ipbin
        banners = [k for k in ("Ninja", "KAMUI", "Shinobi", "Manatee", "SEGA LIBRARY", "Windows CE")
                   if k in r.banners]
        rows.append((os.path.basename(path), dict(
            title=ip["title"][:28], product=ip["product_number"].strip(),
            boot_mb=r.boot_size / 1048576, code_files=len(r.code_files),
            stream_mb=r.streaming_bytes / 1048576, total_mb=r.total_bytes / 1048576,
            wince="YES" if r.wince else "no",
            boot="scrmbl" if r.boot_was_scrambled else "plain",
            banners=",".join(banners) or "-",
            periph=",".join(n.split(" ")[0] for n in ip["peripheral_names"]
                            if n in ("Windows CE", "Light gun", "Keyboard", "Mouse", "Microphone")) or "-",
        )))
    hdr = f"{'image':<24} {'title':<28} {'product':<10} {'boot MB':>7} {'code':>4} {'stream MB':>9} {'total MB':>8} {'WinCE':<5} {'boot':<6} {'banners':<32} extra periph"
    print(hdr)
    print("-" * len(hdr))
    for name, d in rows:
        if isinstance(d, str):
            print(f"{name:<24} {d}")
            continue
        print(f"{name:<24} {d['title']:<28} {d['product']:<10} {d['boot_mb']:>7.2f} {d['code_files']:>4} "
              f"{d['stream_mb']:>9.1f} {d['total_mb']:>8.1f} {d['wince']:<5} {d['boot']:<6} {d['banners']:<32} {d['periph']}")
    print()
    print("boot MB: descrambled boot binary size (smaller is easier).  code: files scoring as SH-4 code "
          "(1 is ideal; more means overlays).  stream MB: .ADX/.AFS/.SFD/.STR media (GD-ROM streaming load).  "
          "boot: how the boot binary is stored (plain on GD-ROM dumps; scrmbl on CD-R style images, descrambled automatically).  "
          "banners: Katana library names found in the boot binary (more = stock Sega libs = better FID coverage).")
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="dcdisc", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--version", action="version", version=__version__)
    sub = p.add_subparsers(dest="cmd", required=True)

    def img(sp):
        sp.add_argument("image", help=".gdi or .chd")
        sp.add_argument("--chdman", action="store_true",
                        help="for .chd: use chdman extractcd even if libchdr is available")

    sub.add_parser("backends").set_defaults(fn=cmd_backends)
    s = sub.add_parser("info"); img(s); s.set_defaults(fn=cmd_info)
    s = sub.add_parser("ls"); img(s); s.set_defaults(fn=cmd_ls)
    s = sub.add_parser("extract"); img(s); s.add_argument("outdir")
    s.add_argument("--no-descramble", action="store_true"); s.set_defaults(fn=cmd_extract)
    s = sub.add_parser("chd2gdi"); img(s); s.add_argument("outdir")
    s.add_argument("--name", help="basename for the .gdi (default: image name)")
    s.set_defaults(fn=cmd_chd2gdi)
    s = sub.add_parser("inspect"); img(s); s.add_argument("-o", "--output")
    s.add_argument("--json", action="store_true")
    s.add_argument("--bin-only", action="store_true", help="only code-scan .BIN files (faster)")
    s.set_defaults(fn=cmd_inspect)
    s = sub.add_parser("manifest"); img(s); s.set_defaults(fn=cmd_manifest)
    s = sub.add_parser("descramble"); s.add_argument("input"); s.add_argument("output")
    s.set_defaults(fn=lambda a: cmd_scramble(a, descramble))
    s = sub.add_parser("scramble"); s.add_argument("input"); s.add_argument("output")
    s.set_defaults(fn=lambda a: cmd_scramble(a, scramble))
    s = sub.add_parser("shortlist"); s.add_argument("images", nargs="+")
    s.add_argument("--chdman", action="store_true")
    s.add_argument("--fast", action="store_true", help="only code-scan .BIN files")
    s.set_defaults(fn=cmd_shortlist)
    s = sub.add_parser("new-game", help="extract a disc and write its config and build file")
    img(s); s.add_argument("id", nargs="?", help="directory and unit name (default: from IP.BIN)")
    s.add_argument("--dir", default="games", help="where games live (default: games)")
    s.add_argument("--title", help="human-readable name (default: the one in IP.BIN)")
    s.add_argument("--no-extract", action="store_true", help="write the files, skip extraction")
    s.add_argument("--force", action="store_true", help="overwrite an existing config")
    s.add_argument("--brief", action="store_true", help="skip the what-to-do-next epilogue")
    s.set_defaults(fn=lambda a: __import__("dcdisc.project", fromlist=["x"]).cmd_new_game(a))
    s = sub.add_parser("doctor", help="check this machine can build and run the project")
    s.set_defaults(fn=lambda a: __import__("dcdisc.project", fromlist=["x"]).cmd_doctor(a))
    s = sub.add_parser("scan"); s.add_argument("file")
    s.add_argument("--strings", type=int, metavar="MINLEN", help="also dump printable strings")
    s.set_defaults(fn=cmd_scan)
    return p


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.fn(args)
    except (OSError, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
