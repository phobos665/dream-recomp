#!/usr/bin/env bash
# Run a command inside the toolchain container with the repository mounted at /work.
#   tools/docker/run.sh                       # interactive shell
#   tools/docker/run.sh make -C tests/kos/hello
set -euo pipefail
IMAGE=${DREAM_TOOLCHAIN_IMAGE:-dream-recomp/toolchain:local}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  echo "building $IMAGE" >&2
  docker build -t "$IMAGE" "$ROOT/tools/docker"
fi
if [ $# -eq 0 ]; then
  exec docker run --rm -it -v "$ROOT:/work" -w /work "$IMAGE" bash
fi
exec docker run --rm -v "$ROOT:/work" -w /work "$IMAGE" bash -c "$*"
