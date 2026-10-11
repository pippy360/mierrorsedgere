# Mirror's Edge WebAssembly (`WASM` + `WebGL 2`) Port

This directory and build target bring the clean-room *Mirror's Edge* (`mierrorsedgere`) engine natively into modern browsers via **WebAssembly**, **WebGL 2.0** (`GLSL ES 3.00`), **SDL2**, and **OpenAL**.

---

## 1. Quick Start (One Command)

Ensure Emscripten (`emcmake` / `em++` via Homebrew `brew install emscripten` or `emsdk`) is installed and `/Users/tomnom/mirrorsedge` contains the retail game data (or pass `--root /path/to/mirrorsedge`):

```bash
./wasm/play_wasm.sh
```

Then open **`http://127.0.0.1:8080`** in Chrome, Edge, Firefox, or Safari.

Optional URL query parameters:
- `?chapter=0` (`0..10`) — boot straight into a specific single-player campaign chapter (skips frontend menu straight to rooftop gameplay).
- `?map=Maps/SP01/Edge_p.me1` — load a specific map package.
- `?mute=1` — disable audio output (useful for automated runs).
- `?max_frames=120` — run a fixed number of rendered frames and halt cleanly.

---

## 2. Architecture Overview

### 2.1 Zero-Bundle On-Demand Asset Streaming (`MEMFS` + `Asyncify`)
Retail *Mirror's Edge* is ~7.2 GB across 400+ Unreal packages (`.upk`, `.me1`, `.u`, `.ini`, `.int`, `.bik`). Rather than packing multi-gigabyte Emscripten `.data` bundles:
1. **`wasm/serve.py` (`/api/tree` + `/assets/*`)** scans the game root once at startup and serves a compact JSON manifest (`/api/tree`) alongside multi-threaded byte-range streaming under `/assets/`.
2. **`wasm/shell.html`** mounts 100% of retail directory entries into Emscripten `MEMFS` as lightweight stub nodes in `< 20 ms` so synchronous C++ directory iteration (`std::filesystem::directory_iterator`, `find_maps()`, localization scanning) works identically to desktop with zero network round-trips.
3. Startup config, localization, script bytecode (`.u`), frontend packages, and core player/NPC `.upk` packages stream across **16 parallel `fetch` workers** during `preRun` before `main()` starts.
4. Any campaign chapter (`ensure_asset_dir`) or external texture/audio package (`ensure_asset_file`) is fetched into `MEMFS` automatically via `EM_ASYNC_JS` + `-sASYNCIFY=1` right before C++ opens it, updating an live **STREAMING ASSETS** HUD indicator in the top-right corner.
5. **Offline Local Folder Mode**: If launched from static hosting without `/api/tree`, `wasm/shell.html` presents a City-of-Glass landing card with `<input webkitdirectory>` so players can select their local `MirrorsEdge` installation directory directly from disk with zero server dependencies.

### 2.2 WebGL 2.0 (`GLSL ES 3.00`) Renderer Adaptations
- Uses `OpenGLRenderer` (`src/renderer/opengl_renderer.cpp` + `src/renderer/msl_to_glsl.cpp`) targeting **WebGL 2.0 / GLSL ES 3.00**.
- Enables WebGL 2 extensions automatically (`EXT_color_buffer_float`, `EXT_color_buffer_half_float`, `OES_texture_float_linear`, `WEBGL_compressed_texture_s3tc`, `WEBGL_compressed_texture_s3tc_srgb`, `EXT_texture_filter_anisotropic`, `EXT_texture_norm16`).
- Because WebGL 2 omits `GL_TEXTURE_SWIZZLE_R/G/B/A` (WebGL 2 Specification §6.13), single-channel `TexFormat::G8` (`RGBA8` / `SRGB8_ALPHA8`) and two-channel normal map `TexFormat::V8U8` (`GL_RGBA8_SNORM`) textures expand on upload under `__EMSCRIPTEN__` so all 5,573 translated UE3 material shaders sample identical `.rgb` / `.xy` channels with zero `GL_INVALID_ENUM` errors.

### 2.3 Browser Event Loop & Single-Thread Safety
- `src/main.cpp` drives its frame tick through `emscripten_set_main_loop_arg(..., 0, 1)` on `__EMSCRIPTEN__`, yielding cleanly to browser `requestAnimationFrame` every frame.
- `material_system.cpp`, `level_lightmaps.cpp`, and `soft_sample.hpp` guard worker `std::thread` creation when `!defined(__EMSCRIPTEN_PTHREADS__)` so single-threaded WASM builds never throw `std::system_error` and keep package/lightmap bulk reads on the Asyncify-capable main WASM stack.

---

## 3. Manual Build Commands

```bash
emcmake cmake -B build-wasm -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build-wasm -j
python3 wasm/serve.py --build-dir build-wasm --root /Users/tomnom/mirrorsedge --port 8080
```
