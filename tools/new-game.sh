#!/usr/bin/env bash
# Set a Dreamcast title up and build it, asking for what it cannot work out.
#
# This is the README's "Recompile your own game" as one command. Everything it does can be done by
# hand and is worth understanding, so it prints each command before running it: the point is to
# save the typing, not to hide what happened.
#
#   tools/new-game.sh                       # ask for everything
#   tools/new-game.sh ~/discs/mygame.chd    # ask for the rest
#   tools/new-game.sh ~/discs               # pick from the images in a folder
#
# Deliberately bash 3.2 compatible: that is what macOS ships, and a script that only runs on the
# author's machine is worse than no script.

set -euo pipefail

# --- presentation -------------------------------------------------------------------------------

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
  B=$(printf '\033[1m'); DIM=$(printf '\033[2m'); R=$(printf '\033[0m')
  RED=$(printf '\033[31m'); YEL=$(printf '\033[33m')
else
  B=""; DIM=""; R=""; RED=""; YEL=""
fi

say()  { printf '%s\n' "$*"; }
head2() { printf '\n%s%s%s\n' "$B" "$*" "$R"; }
warn() { printf '%s%s%s\n' "$YEL" "$*" "$R" >&2; }
die()  { printf '%s%s%s\n' "$RED" "$*" "$R" >&2; exit 1; }

# Every command that changes something is shown before it runs, so the script teaches the manual
# steps rather than replacing understanding of them. Quoted as a shell would need it: disc
# filenames have spaces, brackets and apostrophes in them, and a line that cannot be pasted back
# teaches the wrong command.
run() {
  _q=""
  for _a in "$@"; do
    case "$_a" in
      "" | *[!A-Za-z0-9_@%+=:,./-]*)
        _q="$_q '$(printf '%s' "$_a" | sed "s/'/'\\\\''/g")'" ;;
      *) _q="$_q $_a" ;;
    esac
  done
  printf '%s$%s%s\n' "$DIM" "$_q" "$R"
  "$@"
}

# Reads one answer, offering a default. Repeats until non-empty, since every question here has a
# usable default and a blank answer means "take it".
ask() {
  _prompt=$1; _default=$2; _answer=""
  while [ -z "$_answer" ]; do
    if [ -n "$_default" ]; then
      printf '%s [%s]: ' "$_prompt" "$_default" >&2
    else
      printf '%s: ' "$_prompt" >&2
    fi
    if ! IFS= read -r _answer; then
      printf '\n' >&2
      die "cancelled"
    fi
    [ -z "$_answer" ] && _answer=$_default
  done
  printf '%s' "$_answer"
}

confirm() {
  printf '%s [y/N]: ' "$1" >&2
  IFS= read -r _yn || { printf '\n' >&2; return 1; }
  case "$_yn" in [yY] | [yY][eE][sS]) return 0 ;; *) return 1 ;; esac
}

# --- where we are -------------------------------------------------------------------------------

script_dir=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$script_dir/.." && pwd)
[ -f "$root/CMakeLists.txt" ] || die "cannot find the repository root from $script_dir"

# The path came from the user's shell, so it is relative to the directory they typed it in.
# Everything below runs from $root, so resolve it while it still means what they meant.
arg_image=${1:-}
case $arg_image in
  '' | /*) ;;
  *) arg_image=$PWD/$arg_image ;;
esac

cd "$root"

say "${B}dream-recomp: set a title up${R}"
say "Recompiles a Dreamcast game you own into a native program. You supply the disc."

# --- the tooling --------------------------------------------------------------------------------

PY=${PYTHON:-python3}
command -v "$PY" >/dev/null 2>&1 || die "python3 is required (set PYTHON to choose another)"

if ! "$PY" -c "import dcdisc" >/dev/null 2>&1; then
  head2 "The disc tool is not installed"
  say "It reads the disc and writes the configuration."
  if confirm "Install it now (pip install -e tools/dcdisc)?"; then
    run "$PY" -m pip install -e tools/dcdisc
  else
    die "cannot continue without dcdisc"
  fi
fi
dcdisc() { "$PY" -m dcdisc "$@"; }

# --- the disc -----------------------------------------------------------------------------------

image=$arg_image

# A folder rather than a file: list what is in it and pick by number. Most people keep their dumps
# together, and typing one of those filenames exactly is its own small ordeal.
if [ -n "$image" ] && [ -d "$image" ]; then
  head2 "Images in $image"
  i=0; choices=""
  # A while-read loop rather than an array: bash 3.2 has no readarray, and filenames contain
  # spaces, apostrophes and brackets, so nothing may go through word splitting.
  while IFS= read -r f; do
    i=$((i + 1))
    printf '  %2d) %s\n' "$i" "$(basename "$f")"
    choices="$choices$f
"
  done <<EOF
$(find "$image" -maxdepth 1 \( -name '*.chd' -o -name '*.gdi' \) | sort)
EOF
  [ "$i" -gt 0 ] || die "no .chd or .gdi images in $image"
  n=$(ask "Which one" "1")
  case "$n" in
    '' | *[!0-9]*) die "not a number: $n" ;;
  esac
  [ "$n" -ge 1 ] && [ "$n" -le "$i" ] || die "no image number $n"
  image=$(printf '%s' "$choices" | sed -n "${n}p")
fi

while [ -z "${image:-}" ] || [ ! -f "$image" ]; do
  [ -n "${image:-}" ] && warn "no such file: $image"
  image=$(ask "Path to your disc image (.chd or .gdi), or a folder of them" "")
  if [ -d "$image" ]; then
    warn "that is a folder; re-run as: tools/new-game.sh '$image'"
    image=""
  fi
done

head2 "Reading $image"
info=$(dcdisc info "$image") || die "dcdisc could not read that image"
field() { printf '%s\n' "$info" | sed -n "s/^$1: *//p" | head -1; }
det_title=$(field "Title")
det_wince=$(field "WinCE")
printf '%s\n' "$info" | sed -n '/^Title:/,$p'

if [ "$det_wince" != "no" ] && [ -n "$det_wince" ]; then
  die "This is a Windows CE title. Those do not use the Katana SDK and nothing here applies to them."
fi
[ -n "$det_title" ] || die "that image has no readable IP.BIN header"

# --- the questions ------------------------------------------------------------------------------

# The id names a directory, a CMake target and the generated C++ unit, so it has to be plain.
default_id=$(printf '%s' "$det_title" | tr '[:upper:]' '[:lower:]' | tr -cd 'a-z0-9')
[ -n "$default_id" ] || default_id="game"

head2 "A few questions"
say "${DIM}Press return to take the value in brackets.${R}"
title=$(ask "Title, as people should see it" "$det_title")
while :; do
  id=$(ask "Short id (directory, build target, C++ unit)" "$default_id")
  case "$id" in
    *[!A-Za-z0-9_-]*) warn "letters, digits, underscore and hyphen only" ;;
    *) break ;;
  esac
done
games_dir=$(ask "Where titles live" "games")
build_dir=$(ask "Build directory" "build")

dest="$games_dir/$id"
if [ -f "$dest/$id.toml" ]; then
  warn "$dest/$id.toml already exists."
  confirm "Overwrite it?" || die "nothing changed"
  force="--force"
else
  force=""
fi

head2 "About to do this"
say "  disc        $image"
say "  title       $title"
say "  id          $id"
say "  config      $dest/$id.toml"
say "  extract to  $dest/extracted   ${DIM}(never committed)${R}"
say "  build in    $build_dir"
say ""
say "${DIM}Extraction copies the whole filesystem out of the disc, so this needs room and a minute.${R}"
confirm "Go ahead?" || die "nothing changed"

# --- do it --------------------------------------------------------------------------------------

head2 "1/3  Extracting and writing the configuration"
# shellcheck disable=SC2086  # $force is one optional flag, deliberately unquoted
run dcdisc new-game "$image" "$id" --dir "$games_dir" --title "$title" --brief $force

head2 "2/3  Configuring the build"
if [ -f "$build_dir/CMakeCache.txt" ]; then
  say "${DIM}$build_dir already configured; re-running to pick up the new title.${R}"
fi
# The development interpreter is what lets the program carry on past code the translator has not
# found yet. Without it a new title stops at the first gap, which for a first run is every time.
run cmake -S . -B "$build_dir" -DDREAM_DEV_INTERPRETER=ON

head2 "3/3  Translating and building"
say "${DIM}The translation happens now. Tens of megabytes of generated C++: this is the slow part.${R}"
jobs=$( (command -v nproc >/dev/null 2>&1 && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 4)
if ! run cmake --build "$build_dir" --target "${id}_boot" --parallel "$jobs"; then
  die "The build failed. The output above says why; a compile error in the generated C++ is a
translator bug worth reporting, not something you did wrong."
fi

launcher="$build_dir/$games_dir/$id/${id}_boot"
[ -x "$launcher" ] || die "built, but $launcher is missing"

head2 "Built: $launcher"

# --- the first run ------------------------------------------------------------------------------

say "A first run almost never gets far. What matters is that it tells you where it stopped."
if confirm "Run it now, headless, for 400 frames?"; then
  suggested="$dest/suggested.toml"
  run "$launcher" --config "$dest/$id.toml" --max-frames 400 --rtc-seed 1000000 \
      --suggest-config "$suggested" || true
  if [ -s "$suggested" ]; then
    head2 "It wrote $suggested"
    say "That is the configuration this run earned: addresses the translator never reached, and"
    say "regions the program copies and runs elsewhere. Paste it into $dest/$id.toml, build again,"
    say "and run again. Each round usually finds more than the one address."
  fi
fi

head2 "From here"
say "  Play it:        $launcher --config $dest/$id.toml --window"
say "  Close a gap:    ...same, plus --suggest-config next.toml"
say "  Every flag:     $launcher --help"
say ""
say "In the window: arrows are the d-pad, Z X A S are A B X Y, return is start,"
say "F10 toggles the frame-rate counter, F11 captures a frame, F12 screenshots, escape quits."
say ""
say "${DIM}Nothing from your disc is ever committed: $dest/extracted is ignored by git.${R}"
