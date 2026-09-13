#!/usr/bin/env bash
# Build Flycast's libretro core with the dream-recomp oracle entry point (docs/differential-harness.md).
#
#   tools/flycast/oracle/build_oracle.sh [SRC_DIR] [BUILD_DIR]
#
# Clones flyinghead/flycast at the pinned commit into SRC_DIR (default: build/flycast-src), applies
# flycast-oracle.patch, adds dream_oracle.cpp, configures the core as a shared library and builds it.
# Prints the core path at the end; export it as DREAM_FLYCAST_CORE (or pass -DDREAM_FLYCAST_CORE=...
# to the dream-recomp CMake configure) to enable the differential test.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
SRC="${1:-$REPO/build/flycast-src}"
BUILD="${2:-$REPO/build/flycast-oracle}"
FLYCAST_URL="https://github.com/flyinghead/flycast"
FLYCAST_COMMIT="0abac3465dc9547dca5f30f3352fee10b67e34b2"   # 2026-09-10; bump deliberately

if [ ! -d "$SRC/.git" ]; then
  git clone --filter=blob:none --no-checkout "$FLYCAST_URL" "$SRC"
fi
git -C "$SRC" fetch --depth 1 origin "$FLYCAST_COMMIT"
git -C "$SRC" checkout --force --detach "$FLYCAST_COMMIT"
git -C "$SRC" submodule update --init --recursive --depth 1

# Local patch: add the oracle source to the libretro target and export its symbol on macOS.
git -C "$SRC" apply --check "$HERE/flycast-oracle.patch" && git -C "$SRC" apply "$HERE/flycast-oracle.patch"
cp "$HERE/dream_oracle.cpp" "$SRC/shell/libretro/dream_oracle.cpp"

CMAKE_ARGS=(-DLIBRETRO=ON -DCMAKE_BUILD_TYPE=Release -DUSE_VULKAN=OFF -DENABLE_LOG=OFF)
case "$(uname -s)" in
  Darwin)
    # CMake 4 + the vendored SDL need Objective-C enabled before project(); the SDK's libz is a
    # .tbd stub, so name it explicitly; build only the host architecture.
    printf 'enable_language(OBJC)\nenable_language(OBJCXX)\n' > "$BUILD.enable_objc.cmake"
    CMAKE_ARGS+=(-DCMAKE_PROJECT_INCLUDE="$BUILD.enable_objc.cmake"
                 -DFLYCAST_PRESEED_DARWIN_XCODE_CHECKS=OFF
                 -DZLIB_LIBRARY="$(xcrun --show-sdk-path)/usr/lib/libz.tbd"
                 -DCMAKE_OSX_ARCHITECTURES="$(uname -m)")
    CORE="$BUILD/flycast_libretro.dylib"
    ;;
  *)
    CORE="$BUILD/flycast_libretro.so"
    ;;
esac

cmake -S "$SRC" -B "$BUILD" "${CMAKE_ARGS[@]}"
cmake --build "$BUILD" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
test -f "$CORE"
echo "oracle core: $CORE"
echo "export DREAM_FLYCAST_CORE=$CORE"
