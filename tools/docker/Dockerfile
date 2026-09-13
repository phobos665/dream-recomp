# dream-recomp toolchain container (WP0.2).
#
# Based on the KallistiOS project's own multi-arch image, which ships sh-elf-gcc 14.2 (SH-4),
# arm-eabi-gcc (AICA ARM7), a prebuilt KallistiOS with kos-ports, CMake, Ninja, Python 3 and git on
# Alpine. Runs natively on Apple Silicon (linux/arm64) and on CI (linux/amd64). We add only the
# Python test dependencies for tools/dcdisc; the repository itself is bind-mounted at /work.
#
# Flycast built as a library (the CPU oracle for WP1.5) is added in Phase 1 as a second stage.
ARG KOS_TAG=14.2-stable
FROM kallistios/dc-kos-toolchain:${KOS_TAG}

RUN apk add --no-cache py3-pip bash && \
    pip3 install --break-system-packages --no-cache-dir pytest pycdlib

# The base image's entrypoint sources KOS's environ.sh.master (KOS_BASE, kos-cc, ...) and then
# execs the given command, so it is kept as-is: run commands as `docker run IMAGE bash -c "..."`.
WORKDIR /work
