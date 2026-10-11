#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
cmake -B "$ROOT/build" -S "$ROOT" -DCMAKE_BUILD_TYPE=Release
cmake --build "$ROOT/build" --target mirrorsedge_vulkan -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 8)"
exec "$ROOT/build/mirrorsedge_vulkan" "$@"
