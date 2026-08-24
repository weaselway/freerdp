#!/usr/bin/env bash

set -e

# This script lives inside the FreeRDP checkout, so everything is anchored to
# its own location rather than $PWD -- it can be run from anywhere.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

SOURCE=$SCRIPT_DIR
BUILD_DIR="${SOURCE}/build/freerdp"

# No CMAKE_TOOLCHAIN_FILE. The cross environment comes from the nix devshell in
# flake.nix, which puts the x86_64-w64-mingw32 wrappers on PATH and points $CC
# and $CXX at them -- CMake picks those up on its own, so all that is left to
# state is the target system. A toolchain file here would fight that by naming
# compilers and a sysroot that only exist on a Fedora-style layout.
#
# What the toolchain file *does* carry that we still need is CMAKE_FIND_ROOT_PATH.
# Without it, find_package() searches the build host: CMake derives prefixes from
# $PATH, so /usr/bin puts the whole of /usr on the search path. Under nix that is
# harmless -- there is no host /usr/include -- but on a distro image it means the
# Linux copy of a dependency wins. jansson is the one FreeRDP asks for that Arch
# has natively and not for mingw, and finding it at /usr/include + /usr/lib puts
# "-isystem /usr/include" on every compile, so glibc's headers shadow the mingw
# ones and winpr dies in a pile of "redefinition of 'struct timeval'" and
# conflicting ssize_t/time_t/fd_set/select declarations.
#
# So point find_* at the target sysroot instead, when there is one to point at.
# Distro mingw packaging (Arch, Fedora) puts it at /usr/<triple>; nix has no such
# directory and needs the search left alone, which is exactly the condition below.
# The SDL prefixes join the root path because find_package still has to see them.
#
# Configure once behind an "okay" marker and build incrementally after that,
# same as build.sh and build-mesa.sh. CLEAN=1 forces a reconfigure, which is
# what you want after changing anything below or switching environments -- a
# cache holding paths from a different toolchain is the one way this goes
# quietly wrong.
if [ "${CLEAN:-}" = "1" ]; then
    rm -rf "$BUILD_DIR"
fi
mkdir -p "$BUILD_DIR"

# Official SDL mingw development tarballs. These are prebuilt and shared only
# (libSDL3.dll.a import libs, no static archives), so WITH_SDL_LINK_SHARED stays ON
# and the DLLs get copied next to the client below.
SDL3_VERSION=3.4.14
SDL3_TTF_VERSION=3.2.2

# Fetched dependencies, not sources, so they live under build/ with everything
# else that is generated -- and are covered by its .gitignore entry.
SDL3_DIR=${SOURCE}/build/SDL3-$SDL3_VERSION
SDL3_TTF_DIR=${SOURCE}/build/SDL3_ttf-$SDL3_TTF_VERSION

SDL3_PREFIX=$SDL3_DIR/x86_64-w64-mingw32
SDL3_TTF_PREFIX=$SDL3_TTF_DIR/x86_64-w64-mingw32

# Fetch and unpack the tarballs if they are not already here. Each unpacks to
# exactly the directory name built above, so the directory existing is the test
# for "already have it" -- there is nothing else to check against, as upstream
# publishes no checksum alongside the release asset.
fetch_sdl() {
    local dir=$1 url=$2 tarball

    if [ -d "$dir" ]; then
        return 0
    fi

    tarball="${SOURCE}/build/$(basename "$url")"
    echo "build-freerdp.sh: fetching $(basename "$tarball")"
    mkdir -p "${SOURCE}/build"
    curl -fsSL --retry 3 -o "$tarball" "$url"

    # Unpack into the parent, not into $dir: the tarball carries its own
    # top-level directory, so extracting into one would nest it a level deeper.
    tar xf "$tarball" -C "$(dirname "$dir")"
    rm -f "$tarball"

    if [ ! -d "$dir" ]; then
        echo "build-freerdp.sh: $(basename "$tarball") did not contain $(basename "$dir")" >&2
        return 1
    fi
}

fetch_sdl "$SDL3_DIR" \
    "https://github.com/libsdl-org/SDL/releases/download/release-$SDL3_VERSION/SDL3-devel-$SDL3_VERSION-mingw.tar.gz"

fetch_sdl "$SDL3_TTF_DIR" \
    "https://github.com/libsdl-org/SDL_ttf/releases/download/release-$SDL3_TTF_VERSION/SDL3_ttf-devel-$SDL3_TTF_VERSION-mingw.tar.gz"

source mingw-env x86_64-w64-mingw32

# See the note at the top: confine find_* to the target sysroot on layouts that
# have one, and leave it alone where there is none. MINGW_SYSROOT overrides the
# guess -- docker-env.sh already forwards it.
CROSS_FIND_ARGS=()
MINGW_SYSROOT=${MINGW_SYSROOT:-/usr/x86_64-w64-mingw32}
if [ -d "$MINGW_SYSROOT" ]; then
    CROSS_FIND_ARGS=(
        -DCMAKE_FIND_ROOT_PATH="$MINGW_SYSROOT;$SDL3_PREFIX;$SDL3_TTF_PREFIX"
        # Host tools -- ninja, git, the resource compiler -- are build-platform
        # binaries and have to keep coming from the host, so only libraries,
        # headers and packages are confined to the sysroot.
        -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER
        -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY
        -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY
        -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY
    )
fi

# ...with one thing the setting above cannot cover. FreeRDP's own CMakeLists
# relaxes CMAKE_FIND_ROOT_PATH_MODE_PROGRAM to BOTH so it can locate ccache and
# git, then pins it back to ONLY rather than to whatever it was before. C is
# already enabled by then -- the top-level project() does it -- so its binutils
# were resolved while the search was still unrestricted. client/SDL enables CXX
# from a nested project() much further down, by which point the search for the
# LTO-aware x86_64-w64-mingw32-gcc-ar is confined to the target sysroot. Those
# wrappers are build-platform binaries and live in /usr/bin, so the search comes
# up empty and every C++ static library tries to run a program named
# CMAKE_CXX_COMPILER_AR-NOTFOUND. Naming them here leaves find_program nothing
# to do. (Only reachable with LTO on, which CMAKE_INTERPROCEDURAL_OPTIMIZATION
# turns on by default whenever the compiler supports it.)
CXX_BINUTILS_ARGS=()
for pair in AR:gcc-ar RANLIB:gcc-ranlib; do
    tool_path=$(command -v "x86_64-w64-mingw32-${pair#*:}" || true)
    if [ -n "$tool_path" ]; then
        CXX_BINUTILS_ARGS+=("-DCMAKE_CXX_COMPILER_${pair%%:*}=$tool_path")
    fi
done

if [ ! -f "$BUILD_DIR/okay" ]; then
cmake -GNinja \
    -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
    "${CROSS_FIND_ARGS[@]}" \
    "${CXX_BINUTILS_ARGS[@]}" \
    -DCMAKE_PREFIX_PATH="$SDL3_PREFIX;$SDL3_TTF_PREFIX" \
    -DCMAKE_BUILD_TYPE=Release \
    -DWITH_KRB5=OFF \
    -DWITH_TIMEZONE_ICU=OFF \
    -DWITH_WINPR_TOOL=OFF \
    -DWITH_CLIENT_WINDOWS=OFF \
    -DWITH_CLIENT_SDL=ON \
    -DWITH_CLIENT_SDL3=ON \
    -DWITH_CLIENT_SDL2=OFF \
    -DWITH_SDL_LINK_SHARED=ON \
    -DWITH_FFMPEG=OFF \
    -DWITH_SWSCALE=OFF \
    -DWITH_SERVER=OFF \
    -DWITH_MANPAGES=OFF \
    -DCMAKE_C_FLAGS="-D__STDC_NO_THREADS__ -Wno-error=incompatible-pointer-types -Wno-deprecated-declarations" \
    -DCMAKE_CXX_FLAGS="-D__STDC_NO_THREADS__" \
    -DWITH_SMARTCARD_PCSC=OFF \
    -DWITH_SMARTCARD_EMULATE=OFF \
    -DCHANNEL_SMARTCARD=OFF \
    -DWITH_PKCS11=OFF \
    -DUSE_UNWIND=OFF \
    -DCHANNEL_URBDRC=OFF \
    -DOPENSSL_USE_STATIC_LIBS=ON \
    -DZLIB_USE_STATIC_LIBS=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_WINDOWS_VERSION=Win10 \
    -DCMAKE_EXE_LINKER_FLAGS="-static" \
    -B "$BUILD_DIR" "$SOURCE"

    touch "$BUILD_DIR/okay"
fi

cmake --build "$BUILD_DIR"

cp -v "${BUILD_DIR}/client/SDL/SDL3/sdl-freerdp.exe" "${SOURCE}/build/"
cp -v "$SDL3_PREFIX/bin/SDL3.dll" "$SDL3_TTF_PREFIX/bin/SDL3_ttf.dll" "${SOURCE}/build/"
