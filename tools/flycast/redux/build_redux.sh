#!/usr/bin/env bash
# Build GeneralAtrox/flycast_Redux as an out-of-tree oracle (docs/flycast-redux-spike.md).
#
#   tools/flycast/redux/build_redux.sh [SRC_DIR] [BUILD_DIR]
#
# Unlike tools/flycast/oracle/build_oracle.sh, which builds the libretro core for the per-function
# differential test, this builds the full SDL application: the research recorder is gated on
# NOT LIBRETRO, so the core build cannot carry it.
#
# It BUILDS on macOS ARM64 and does not yet RUN: see the spike doc for the upstream virtual-memory
# failure at boot. Nothing in dream-recomp links against this; it is a tool we drive, not a
# dependency.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
SRC="${1:-$REPO/build/flycast-redux-src}"
BUILD="${2:-$REPO/build/flycast-redux}"
REDUX_URL="https://github.com/GeneralAtrox/flycast_Redux"
REDUX_COMMIT="e4345f3255de47d33cb633bed89e39ede3d70295"   # 2026-09-21; bump deliberately

if [ ! -d "$SRC/.git" ]; then
  git clone --filter=blob:none "$REDUX_URL" "$SRC"
fi
git -C "$SRC" fetch origin "$REDUX_COMMIT"
git -C "$SRC" checkout --force --detach "$REDUX_COMMIT"
git -C "$SRC" submodule update --init --recursive --depth 1

# USE_BREAKPAD=OFF: the vendored breakpad builds dump_syms with -Werror, and NXGetLocalArchInfo is
# deprecated from macOS 13. That is upstream Flycast, not the fork, and we do not want crash
# reporting in a tool we run locally.
CMAKE_ARGS=(-DCMAKE_BUILD_TYPE=Release -DUSE_BREAKPAD=OFF)
case "$(uname -s)" in
  Darwin)
    # Same three Darwin workarounds the libretro oracle needs: CMake 4 plus the vendored SDL want
    # Objective-C enabled before project(), the SDK's libz is a .tbd stub, and we build one arch.
    printf 'enable_language(OBJC)\nenable_language(OBJCXX)\n' > "$BUILD.enable_objc.cmake"
    CMAKE_ARGS+=(-DCMAKE_PROJECT_INCLUDE="$BUILD.enable_objc.cmake"
                 -DFLYCAST_PRESEED_DARWIN_XCODE_CHECKS=OFF
                 -DZLIB_LIBRARY="$(xcrun --show-sdk-path)/usr/lib/libz.tbd"
                 -DCMAKE_OSX_ARCHITECTURES="$(uname -m)")
    : "${VULKAN_SDK:=$HOME/VulkanSDK/1.4.357.1/macOS}"
    export VULKAN_SDK
    APP="$BUILD/Flycast.app/Contents/MacOS/Flycast"
    ;;
  *)
    APP="$BUILD/flycast"
    ;;
esac

cmake -S "$SRC" -B "$BUILD" "${CMAKE_ARGS[@]}"
cmake --build "$BUILD" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
test -x "$APP"
echo "redux app: $APP"
