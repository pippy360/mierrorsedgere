# The Windows Port

`mirrorsedge_windows.exe` is the same game as `mirrorsedge_macos`, drawn with Direct3D 11. Everything
except the renderer backend is the same source: the asset loaders, the parkour controller, audio,
cutscenes, the front end and `src/main.cpp`.

## Build and run

1. Install [MSYS2](https://www.msys2.org) and, in its shell, the UCRT64 packages:

   ```bash
   pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,SDL2,openal,libvorbis,ffmpeg,zlib}
   ```

2. From the repository, in `cmd` or PowerShell:

   ```bat
   play_windows.bat
   ```

   It configures `build-win/`, builds `mirrorsedge_windows` and starts it. Arguments go to the game
   (`play_windows.bat --chapter 1`). Set `MSYS2_ROOT` if MSYS2 is not in `C:\msys64`.

By hand, with `C:\msys64\ucrt64\bin` first on `PATH`:

```bash
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-win --target mirrorsedge_windows
build-win/mirrorsedge_windows.exe --verify-all
```

The build copies the DLLs the game loads (SDL2, OpenAL Soft, Vorbis, FFmpeg and their dependencies,
about 100 files and 150 MB with MSYS2's FFmpeg) next to the executable, so it starts from Explorer
too. `-DME_BUNDLE_RUNTIME_DLLS=OFF` skips that; the game then needs the UCRT64 `bin` directory on
`PATH`.

The game finds the retail install by itself: `MEDGE_ME_INSTALL` if set, else Steam's
`steamapps/common/mirrors edge` in any Steam library, else the EA / Origin / GOG default folders.
`--game-root <dir>` overrides it. The command line and the controls are the ones in the README.

`--verify-all` writes its screenshots to `screenshots/` under the current directory. Those files
are tracked and were rendered by the Metal renderer, so run the oracle from another directory
unless you mean to replace them.

## What is where

| File | Role |
|---|---|
| `src/renderer/d3d11_renderer.{hpp,cpp}` | The Direct3D 11 backend, `me::D3D11Renderer`. Same public interface as `MetalRenderer` apart from `init_with_window(HWND)`. |
| `src/renderer/renderer.hpp` | `me::Renderer`: the platform's backend. `main.cpp` uses only this name. |
| `src/renderer/msl_to_hlsl.{hpp,cpp}` | Translates the renderer's Metal Shading Language to HLSL. |
| `src/renderer/builtin_shaders_msl.hpp` | The built-in shaders (MSL), used by both backends. |
| `src/renderer/overlay_ui.inl`, `hud_font.hpp` | The HUD, cutscene overlay and chapter-select draw lists, included by both backends. |
| `src/renderer/render_common.hpp` | Texture layout helpers and the clip test, used by both backends. |
| `src/platform/platform.{hpp,cpp}` | Default game root, temp and cache directories, platform name. |
| `cmake/bundle_runtime_dlls.cmake` | Copies the runtime DLLs next to the executable. |

## One copy of every shader

The shaders are written once, in MSL: the built-in passes, the sun shadow lookup
(`sun_shadow.hpp`) and the UE3 material shaders that `material_system.cpp` generates. The Metal
renderer compiles that text as it is. The Direct3D renderer passes the same text through
`MslToHlsl` and compiles the result with `D3DCompile` (`d3dcompiler_47.dll`, part of Windows). A
shader edited for Metal therefore changes on Windows too, and nothing in `material_system.cpp`
knows about HLSL.

This works because Metal and Direct3D agree on what matters outside the syntax: clip space (y up,
z from 0 to 1), framebuffer and texture origins, column-vector matrices stored by column, and how a
`float3` followed by a `float` packs into a uniform block.

The translator works on tokens and handles the subset of MSL those sources use. What it rewrites:

| MSL | HLSL |
|---|---|
| `vertex` / `fragment` functions, `[[stage_in]]` | entry points; struct members get `SV_Position`, `ATTRn`, `TEXCOORDn` |
| `constant T& u [[buffer(n)]]` | `cbuffer` at `bn` holding `T u` |
| `constant float4* U [[buffer(n)]]` | `cbuffer` at `bn` holding `float4 U[count]` |
| `vertices[vertex_id]` from `[[buffer(0)]]` in a vertex stage | the input assembler: the struct's members become `VTX0..n` |
| `texture2d` / `depth2d` / `depth2d_array` / `texturecube` arguments | `Texture2D` / `Texture2DArray` / `TextureCube` globals at `tn` |
| `sampler s [[sampler(n)]]`, `constexpr sampler` | `SamplerState` at `sn`; constexpr samplers get slots 14, 13, ... and are created by the renderer |
| `t.sample(s, uv)`, `sample(..., level(x))`, `sample_compare`, `get_width()` | `Sample`, `SampleLevel`, `SampleCmpLevelZero`, `GetDimensions` |
| `matrix * x` | `mul(matrix, x)` |
| `float3(x)` with one argument | `((float3)(x))` |
| `mix`, `fract`, `dfdx`, `dfdy`, `discard_fragment()` | `lerp`, `frac`, `ddx`, `ddy`, `discard` |
| `constant` globals, `const T&` / `T&` parameters, `template <typename T>` helpers | `static const`, by value / `inout`, one overload each for `float`..`float4` |
| identifiers that are HLSL keywords (`in`, `out`, `sample`, `line`, ...) | the same name with `_` appended |

Anything else passes through unchanged. If MSL written later uses a construct outside this list,
the HLSL compiler rejects it and the renderer prints the error and the shader's name at start-up
or when the level loads (`[D3D11Renderer] shader error: ...`); a failed material falls back to the
legacy world shader, as on macOS. Matrix products are recognised by name: a `*` whose left side is
a struct member or local declared `float4x4` / `float3x3` / `float2x2`. A product written any
other way is left as `*`, which HLSL refuses rather than computing something else.

Compiled bytecode is cached in `%LOCALAPPDATA%\mierrorsedgere\shader_cache`, keyed by a hash of the
HLSL. The first load of a chapter compiles its 213 to 537 material shaders in about four seconds;
later loads take a tenth of a second. Deleting the directory is safe.

## Where the Direct3D backend differs from the Metal one

- **Off-screen first.** Every frame is rendered to an RGBA8 texture that is then copied to the
  window's back buffer. `P` / `F12` screenshots therefore work in a window.
- **Vertices come from the input assembler**, not from a buffer indexed by `vertex_id`, so a mesh
  section is drawn with `Draw(count, first)`.
- **Enemies are drawn indexed.** Each posed officer uploads its unique vertices, and the animation
  system's static triangle lists are index buffers. The Metal backend expands every triangle list
  on the CPU; here that made the frame take 20 to 45 ms in the chapters with 45 to 71 enemies.
- **Enemies that cannot be seen are not posed.** A sphere of 1024 UU around an enemy's origin is
  tested against the view and the near shadow cascade before it is skinned. Posed vertices stay
  within 224 UU of the origin in all ten chapters.
- **The level is not drawn under the front end.** While a front end frame covers the window the
  shadow, scene and post passes are skipped; the level's materials are still made resident.
- **D3D9 texture formats that Direct3D 11 samples differently are widened on upload:** L8 to BGRA8,
  V8U8 to RGBA8_SNORM (Metal uses channel swizzles).
- **PNG screenshots are opaque.** The Metal backend's PNG writer treats the frame's alpha as
  premultiplied, which brightens translucent HUD panels in its files; on screen the two match.
- **GPU choice.** On a machine with two GPUs the fast one is used. `ME_GPU=integrated` picks the
  other.

## Environment variables

| Variable | Effect |
|---|---|
| `MEDGE_ME_INSTALL` | The retail install (as for the tools in `tools/retail`). |
| `ME_GPU=integrated` | Use the power-saving GPU. |
| `ME_RENDER_PROF=1` | Every 120 frames, print where a frame's time went, by phase, and the draw count. |
| `ME_NO_SHADER_CACHE=1` | Compile every shader again. |
| `ME_D3D_DEBUG=1` | Create the device with the Direct3D debug layer (the optional Windows feature "Graphics Tools") and print its messages. |
| `ME_CULL=off\|cw\|ccw` | Material back-face culling, as on macOS. |

## What was run (2026-10-08)

On a laptop with an NVIDIA RTX A5000 and Intel UHD Graphics, Windows 11, retail game from Steam:

- **`--verify-all`: all stages pass**, on the NVIDIA GPU and on the Intel one. Material shaders
  compiled: 213/213, 537/537, 289/289 and 6/6.
- **Every chapter's material shaders translate and compile:** 4,281 fragment shaders across the ten
  chapters and `Escape_p`, none failing.
- **Every chapter loads and runs in a window** (`--chapter 0..9`, 600 frames each, clean exit) at
  the display's 60 Hz, 1600x900. CPU time per frame is 3 to 8 ms.
- **Against the Metal screenshots in `screenshots/`:** the shots whose camera does not depend on
  the simulation (`tutorial_1..6`, `oracle_6`, `oracle_9`) differ by 1.0 to 1.7 of 255 on average.
  The differences are the translucent HUD panels (see above), the pose of the hands and texture
  filtering. The other shots were taken at simulation states that have since changed, so they do
  not compare.
- **The front end in a window:** "Press Any Key", the main menu, moving between columns, opening
  the chapter-select overlay and resizing the window.

Not run: the Direct3D debug layer (not installed on that machine), a gamepad, the fullscreen
toggle, a play-through of any level, AMD GPUs, Windows 10.

## Known gaps

- **The front end runs at about 21 frames a second** on that laptop. It is drawn by the CPU
  reference renderer (`src/ui/frontend/soft_city.cpp`), which takes about 34 ms for the city and the
  rest for the 2D layer. Building with AVX2 saves about 15%. In game the frame rate is the
  display's.
- The macOS build is checked from Windows by compiling it on a `macos-15` GitHub runner. That
  proves it builds, not that it runs: the runner has no retail assets.
