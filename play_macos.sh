#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "=== Mirror's Edge Native macOS Engine ==="

mkdir -p build
if [ ! -f "build/Makefile" ]; then
    echo "[CMake] Configuring build..."
    cmake -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
fi

echo "[CMake] Building mirrorsedge_macos..."
cmake --build build -j$(sysctl -n hw.ncpu || echo 4)

echo "[Game] Launching..."
exec ./build/mirrorsedge_macos "$@"
