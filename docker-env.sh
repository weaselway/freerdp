#!/usr/bin/env bash

set -e

# Runs build-freerdp.sh inside the Arch + mingw-w64 image described by the
# Dockerfile next to this script, for machines without a cross toolchain of
# their own. On a host that has one, build-freerdp.sh runs directly -- it
# sources mingw-env itself and is not otherwise container-specific.
#
#   ./docker-env.sh                 build FreeRDP
#   ./docker-env.sh bash            a shell in the toolchain image
#   ./docker-env.sh cmake --build … anything else against the same toolchain
#
# REBUILD=1 rebuilds the image; CLEAN=1 is forwarded to build-freerdp.sh.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

IMAGE=${IMAGE:-wsl/mingw}

if ! command -v docker >/dev/null; then
    echo "docker-env.sh: docker not found" >&2
    exit 1
fi

# The Dockerfile pulls everything from pacman and the AUR and COPYs nothing in,
# so it is fed on stdin with no build context at all. Handing docker $SCRIPT_DIR
# as the context instead would upload the whole FreeRDP checkout -- build/ and
# its SDL tarballs included -- on every run, for a Dockerfile that never reads
# a byte of it.
if [ "${REBUILD:-}" = "1" ] || ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "docker-env.sh: building $IMAGE (this takes a while the first time)"
    docker build -t "$IMAGE" - <"$SCRIPT_DIR/Dockerfile"
fi

if [ $# -eq 0 ]; then
    set -- ./build-freerdp.sh
fi

TTY_ARGS=()
if [ -t 0 ] && [ -t 1 ]; then
    TTY_ARGS=(-it)
fi

# Forward the knobs build-freerdp.sh reads. Without this, "CLEAN=1 ./docker-env.sh"
# would set the variable in the calling shell only and the container would
# quietly reuse the previous CMakeCache.txt.
ENV_ARGS=()
for var in CLEAN MINGW_SYSROOT SDL3_VERSION SDL3_TTF_VERSION; do
    if [ -n "${!var:-}" ]; then
        ENV_ARGS+=(-e "$var=${!var}")
    fi
done

# Mount the repo at its own path so paths resolve identically inside and out:
# the CMake cache, compile_commands.json and the debug info all record absolute
# paths, and matching them means an editor or debugger on the host can follow
# them without a mapping. --user keeps the build artifacts owned by the caller
# rather than by root; the image's own "builder" account is not needed for
# anything, as nothing is installed at run time.
#
# The image entrypoint is "env bash", so what follows is that shell's argv.
# mingw-env is sourced here rather than left to build-freerdp.sh so that the
# ad-hoc forms above -- a bare cmake, a shell -- get the same CC/CXX and flags;
# build-freerdp.sh sourcing it a second time is harmless.
exec docker run --rm "${TTY_ARGS[@]}" "${ENV_ARGS[@]}" \
    --user="$(id -u):$(id -g)" \
    -e HOME=/tmp \
    -v "$SCRIPT_DIR:$SCRIPT_DIR" \
    -w "$SCRIPT_DIR" \
    "$IMAGE" \
    -c '
        source mingw-env x86_64-w64-mingw32
        exec "$@"
    ' docker-env "$@"
