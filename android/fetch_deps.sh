#!/usr/bin/env bash
# Downloads and unpacks the third-party sources the Android build compiles from source into
# android/third_party/ (gitignored). Idempotent: a directory that is already unpacked is kept.
#
#   SDL2         https://github.com/libsdl-org/SDL        (the window, GLES context, touch, Java glue)
#   OpenAL Soft  https://github.com/kcat/openal-soft      (the audio engine's OpenAL, OpenSL backend)
#   libogg       https://xiph.org                          (Vorbis streams: music and voice)
#   libvorbis    https://xiph.org
#
# Override a version with SDL2_VERSION=..., OPENAL_VERSION=..., OGG_VERSION=..., VORBIS_VERSION=...
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
DEPS="${HERE}/third_party"
mkdir -p "${DEPS}"

SDL2_VERSION="${SDL2_VERSION:-2.32.10}"
OPENAL_VERSION="${OPENAL_VERSION:-1.24.3}"
OGG_VERSION="${OGG_VERSION:-1.3.6}"
VORBIS_VERSION="${VORBIS_VERSION:-1.3.7}"

# fetch <dest dir name> <url> <top-level dir inside the tarball>
fetch() {
    local name="$1" url="$2" inner="$3"
    local dest="${DEPS}/${name}"
    if [ -f "${dest}/.fetched" ]; then
        echo "[fetch_deps] ${name}: already present (${dest})"
        return 0
    fi
    rm -rf "${dest}" "${dest}.tmp"
    mkdir -p "${dest}.tmp"
    local tarball="${DEPS}/${name}.tar.gz"
    echo "[fetch_deps] ${name}: downloading ${url}"
    curl -fL --retry 3 --retry-delay 2 -o "${tarball}" "${url}"
    tar -xzf "${tarball}" -C "${dest}.tmp"
    if [ -d "${dest}.tmp/${inner}" ]; then
        mv "${dest}.tmp/${inner}" "${dest}"
        rm -rf "${dest}.tmp"
    else
        # A tarball without the expected top-level directory: take the single directory it has.
        local only
        only="$(find "${dest}.tmp" -mindepth 1 -maxdepth 1 -type d | head -n 1)"
        if [ -z "${only}" ]; then
            echo "[fetch_deps] ${name}: nothing unpacked from ${tarball}" >&2
            exit 1
        fi
        mv "${only}" "${dest}"
        rm -rf "${dest}.tmp"
    fi
    rm -f "${tarball}"
    echo "${url}" > "${dest}/.fetched"
    echo "[fetch_deps] ${name}: unpacked into ${dest}"
}

fetch SDL2 \
    "https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/SDL2-${SDL2_VERSION}.tar.gz" \
    "SDL2-${SDL2_VERSION}"
fetch openal-soft \
    "https://github.com/kcat/openal-soft/archive/refs/tags/${OPENAL_VERSION}.tar.gz" \
    "openal-soft-${OPENAL_VERSION}"
fetch libogg \
    "https://downloads.xiph.org/releases/ogg/libogg-${OGG_VERSION}.tar.gz" \
    "libogg-${OGG_VERSION}"
fetch libvorbis \
    "https://downloads.xiph.org/releases/vorbis/libvorbis-${VORBIS_VERSION}.tar.gz" \
    "libvorbis-${VORBIS_VERSION}"

# (`#include <SDL2/SDL.h>` is served by a link CMakeLists.txt makes in the build tree.)

# SDL's Java side (org.libsdl.app.SDLActivity and friends) is compiled into the app from here.
if [ ! -d "${DEPS}/SDL2/android-project/app/src/main/java/org/libsdl/app" ]; then
    echo "[fetch_deps] SDL2 tree has no android-project/app/src/main/java/org/libsdl/app" >&2
    exit 1
fi

echo "[fetch_deps] done: $(ls "${DEPS}")"
