#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build-wasm"
PORT="${PORT:-8080}"

echo "[WASM] Configuring Emscripten CMake build in ${BUILD_DIR} ..."
emcmake cmake -B "${BUILD_DIR}" -S "${ROOT_DIR}" -DCMAKE_BUILD_TYPE=Release

echo "[WASM] Building mirrorsedge_wasm.html + .js + .wasm ..."
cmake --build "${BUILD_DIR}" -j

echo "[WASM] Starting asset streaming web server on http://127.0.0.1:${PORT}/ ..."
exec python3 "${SCRIPT_DIR}/serve.py" --build-dir "${BUILD_DIR}" --port "${PORT}" "$@"
