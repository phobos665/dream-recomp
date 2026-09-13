"""Setting a title up, and checking the machine can build it.

Two commands that are about the repository rather than about discs, but which live here because
this is the CLI people already install. `new-game` turns steps 2 to 4 of the README into one
command; `doctor` answers "is my machine ready" without making you find out by failing.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys

# Nearly every Katana title is loaded and linked at these addresses. They are offered as the
# starting point rather than as fact: a title that disagrees shows it immediately by going nowhere,
# and the config is the place to correct it.
DEFAULT_LOAD = 0x8C010000
DEFAULT_LINK = 0x0C010000

TOML_TEMPLATE = """\
# {title}{product_comment}
# Written by `dcdisc new-game`. Everything here is read off the disc or is the standard Katana
# layout; nothing from the disc itself is ever committed.

[game]
id = "{id}"
title = "{title}"
{region_line}{product_line}
[disc]
image = "{image_rel}"

[binary]
path = "extracted/fs/{boot}"
load_address = 0x{load:08X}    # where the console's BIOS loads the file
link_address = 0x{link:08X}    # the same RAM through a different address; what the code assumes
entry = 0x{load:08X}           # the first instruction

# Addresses discovery does not reach, and regions the program copies and runs elsewhere, both go
# here. Run the launcher with --suggest-config FILE and it writes them for you.
# [functions]
# extra = []
"""

CMAKE_TEMPLATE = """\
# {title}. Everything is in cmake/DreamAddGame.cmake; this file exists so that adding a game is
# adding a directory. Add BOOT_TEST once the title boots far enough to be worth asserting on.
dream_add_game({id} TITLE "{title}")
"""


def _slug(text: str) -> str:
    s = re.sub(r"[^a-z0-9]+", "", text.lower())
    return s or "game"


def cmd_new_game(args) -> int:
    """Extract a disc and write the config and build file for it."""
    from .image import open_image
    from .ipbin import parse_ipbin, read_ipbin

    games_dir = os.path.abspath(args.dir)
    game_id = args.id or None

    with open_image(args.image, prefer_chdman=getattr(args, "chdman", False)) as img:
        ip = parse_ipbin(read_ipbin(img))
        # Windows CE titles do not use the Katana SDK and nothing in this toolchain applies to
        # them. IP.BIN says so in its peripheral word, so this costs one sector rather than the
        # gigabyte it would take to find out by extracting.
        if ip.is_wince:
            print("error: this is a Windows CE title, which is out of scope for dream-recomp",
                  file=sys.stderr)
            return 2
        title = (getattr(args, "title", None) or ip.title).strip()
        if game_id is None:
            game_id = _slug(title)
        if not re.fullmatch(r"[A-Za-z0-9_-]+", game_id):
            print(f"error: id {game_id!r} must be letters, digits, underscore or hyphen",
                  file=sys.stderr)
            return 2
        out_dir = os.path.join(games_dir, game_id)
        toml_path = os.path.join(out_dir, f"{game_id}.toml")
        cmake_path = os.path.join(out_dir, "CMakeLists.txt")
        if os.path.exists(toml_path) and not args.force:
            print(f"error: {toml_path} exists (use --force to overwrite)", file=sys.stderr)
            return 2
        os.makedirs(out_dir, exist_ok=True)

        region = ip.regions[0] if ip.regions else ""
        # The image is referenced where it is, rather than copied into the tree: disc dumps are
        # large and are never committed, so the config points at wherever the owner keeps it.
        image_rel = os.path.relpath(os.path.abspath(args.image), out_dir)
        text = TOML_TEMPLATE.format(
            id=game_id,
            title=title or game_id,
            product_comment=f", {ip.product_number}" if ip.product_number.strip() else "",
            region_line=f'region = "{region}"\n' if region else "",
            product_line=f'product = "{ip.product_number.strip()}"\n' if ip.product_number.strip() else "",
            image_rel=image_rel.replace(os.sep, "/"),
            boot=ip.boot_filename.strip() or "1ST_READ.BIN",
            load=DEFAULT_LOAD,
            link=DEFAULT_LINK,
        )

    print(f"{title or game_id}  ({ip.product_number.strip()}, {region or 'region unknown'})")
    extracted = os.path.join(out_dir, "extracted")
    if args.no_extract:
        print(f"skipping extraction; put the filesystem under {extracted}/fs/")
    else:
        from .cli import cmd_extract

        class _A:  # cmd_extract reads these off its argument object
            image = args.image
            chdman = getattr(args, "chdman", False)
            outdir = extracted
            no_descramble = False

        rc = cmd_extract(_A())
        if rc != 0:
            return rc

    with open(toml_path, "w") as f:
        f.write(text)
    with open(cmake_path, "w") as f:
        f.write(CMAKE_TEMPLATE.format(id=game_id, title=title or game_id))
    print(f"wrote {os.path.relpath(toml_path)}")
    print(f"wrote {os.path.relpath(cmake_path)}")
    # --brief: the caller is about to do these steps itself and saying them twice, once with a
    # BUILDDIR placeholder, reads as though something went wrong.
    if getattr(args, "brief", False):
        return 0
    print()
    print("next:")
    print(f"  cmake --build BUILDDIR --target {game_id}_boot --parallel")
    print(f"  BUILDDIR/games/{game_id}/{game_id}_boot --config {os.path.relpath(toml_path)} --window")
    print()
    print("It will stop somewhere. Run it with --suggest-config FILE and paste what that writes")
    print("into the config, then build again.")
    return 0


# --- doctor ----------------------------------------------------------------------------------

CLANG_FORMAT_PINNED = "20.1.7"


def _run(cmd) -> str | None:
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=20)
    except (OSError, subprocess.SubprocessError):
        return None
    return (r.stdout + r.stderr).strip() or ""


def _version(text: str | None) -> str:
    if not text:
        return ""
    m = re.search(r"(\d+\.\d+(?:\.\d+)?)", text)
    return m.group(1) if m else ""


def cmd_doctor(args) -> int:
    """Check the machine can build and run what this repository builds.

    Reports rather than fails, except where something is genuinely required: most of this is
    optional, and a build without a renderer is a perfectly good headless build.
    """
    rows: list[tuple[str, bool, str]] = []      # (what, ok, detail)
    optional: list[tuple[str, bool, str]] = []

    py = f"{sys.version_info.major}.{sys.version_info.minor}"
    rows.append(("Python 3.9+", sys.version_info >= (3, 9), py))

    cmake = _version(_run(["cmake", "--version"]))
    ok_cmake = bool(cmake) and tuple(int(x) for x in cmake.split(".")[:2]) >= (3, 24)
    rows.append(("CMake 3.24+", ok_cmake, cmake or "not found"))

    cxx = _run(["c++", "--version"])
    rows.append(("C++ compiler", bool(cxx), (cxx or "not found").splitlines()[0] if cxx else "not found"))

    rows.append(("git", bool(_run(["git", "--version"])), _version(_run(["git", "--version"])) or "not found"))

    # The submodule is required: the build stops with a message about it otherwise.
    root = _repo_root()
    sub = os.path.join(root, "third_party", "libchdr", "CMakeLists.txt") if root else None
    rows.append(("libchdr submodule", bool(sub and os.path.exists(sub)),
                 "present" if sub and os.path.exists(sub)
                 else "run: git submodule update --init --recursive"))

    # A CHD needs one of the two backends; a GDI needs neither.
    from .chd import backend_status
    status = backend_status()
    lib, chdman = status.get("libchdr"), status.get("chdman")
    rows.append(("CHD support (libchdr or chdman)", bool(lib or chdman),
                 f"libchdr {lib}" if lib else (f"chdman {chdman}" if chdman
                                               else "neither; .gdi images still work")))

    # Optional: only --window and sound need these, and CMake says so at configure time too.
    glslc = shutil.which("glslc")
    optional.append(("glslc (shader compiler)", bool(glslc), glslc or "not found"))
    sdl = _run(["pkg-config", "--modversion", "sdl3"])
    optional.append(("SDL3", bool(sdl and sdl[0].isdigit()), sdl if sdl and sdl[0].isdigit() else "not found"))
    vulkan = _vulkan()
    optional.append(("Vulkan", bool(vulkan), vulkan or "not found"))

    # The pinned clang-format is worth calling out: a different major version reformats files that
    # nobody touched and fails CI on them, which is a confusing first experience.
    cf = _clang_format()
    optional.append((f"clang-format {CLANG_FORMAT_PINNED}", cf == CLANG_FORMAT_PINNED,
                     cf or "not found"))

    failed = _print("Required", rows)
    _print("Optional (a headless build needs none of these)", optional)

    if failed:
        print(f"\n{failed} required check(s) failed; see the detail column.")
        return 1
    print("\nReady. Configure with:")
    print("  cmake -S . -B build -DDREAM_DEV_INTERPRETER=ON")
    return 0


def _print(heading: str, rows) -> int:
    print(f"\n{heading}")
    width = max((len(r[0]) for r in rows), default=0)
    bad = 0
    for what, ok, detail in rows:
        mark = "ok  " if ok else "MISS"
        if not ok:
            bad += 1
        print(f"  [{mark}] {what.ljust(width)}  {detail}")
    return bad


def _repo_root() -> str | None:
    out = _run(["git", "rev-parse", "--show-toplevel"])
    return out if out and os.path.isdir(out) else None


def _vulkan() -> str:
    if sys.platform == "darwin":
        # MoltenVK, usually from the Vulkan SDK or Homebrew. The loader is what matters.
        for p in ("/usr/local/lib/libvulkan.dylib", "/opt/homebrew/lib/libvulkan.dylib",
                  os.path.expanduser("~/VulkanSDK")):
            if os.path.exists(p):
                return p
    v = _version(_run(["vulkaninfo", "--summary"]))
    if v:
        return v
    for p in ("/usr/lib/x86_64-linux-gnu/libvulkan.so.1", "/usr/lib/libvulkan.so.1"):
        if os.path.exists(p):
            return p
    return ""


def _clang_format() -> str:
    # The pinned one is normally installed by pipx, which puts it outside the default PATH used
    # by a shell that has not sourced a profile, so look there as well as on PATH.
    for cand in (os.path.expanduser("~/.local/bin/clang-format"), shutil.which("clang-format")):
        if cand and os.path.exists(cand):
            v = _version(_run([cand, "--version"]))
            if v == CLANG_FORMAT_PINNED:
                return v
            found = v
            break
    else:
        return ""
    return found
