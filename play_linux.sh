#!/usr/bin/env bash
# Configure, build and run the Linux build (docs/LINUX_PORT.md). Arguments go to the game.
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "=== Mirror's Edge Native Linux Engine ==="

GENERATOR_ARGS=()
if command -v ninja >/dev/null 2>&1; then
    GENERATOR_ARGS=(-G Ninja)
fi

if [ ! -f "build/CMakeCache.txt" ]; then
    echo "[CMake] Configuring build..."
    cmake -S . -B build "${GENERATOR_ARGS[@]}" -DCMAKE_BUILD_TYPE=Release
fi

echo "[CMake] Building mirrorsedge_linux..."
cmake --build build --target mirrorsedge_linux -j"$(nproc 2>/dev/null || echo 4)"

echo "[Game] Launching..."
exec ./build/mirrorsedge_linux "$@"
