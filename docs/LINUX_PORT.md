# The Linux Port

`mirrorsedge_linux` is the same game as `mirrorsedge_macos`, drawn with OpenGL 4.1 core. Everything
except the renderer backend is the same source: the asset loaders, the parkour controller, audio,
cutscenes, the front end and `src/main.cpp`.

## Build and run

1. Install the toolchain and the libraries the game loads. On Debian / Ubuntu:

   ```bash
   sudo apt install g++ cmake ninja-build pkg-config libsdl2-dev libopenal-dev libvorbis-dev \
                    libavformat-dev libavcodec-dev libswscale-dev libswresample-dev libavutil-dev zlib1g-dev
   ```

   On Fedora: `dnf install gcc-c++ cmake ninja-build pkgconf SDL2-devel openal-soft-devel libvorbis-devel
   ffmpeg-free-devel zlib-devel`. On Arch: `pacman -S base-devel cmake ninja sdl2 openal libvorbis ffmpeg zlib`.

2. From the repository:

   ```bash
   ./play_linux.sh
   ```

   It configures `build/`, builds `mirrorsedge_linux` and starts it. Arguments go to the game
   (`./play_linux.sh --chapter 1`).

By hand:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target mirrorsedge_linux
build/mirrorsedge_linux --verify-all
```

The game finds the retail install by itself: `MEDGE_ME_INSTALL` if set, else `steamapps/common/mirrors edge`
in any Steam library (the native, Flatpak and Snap clients and the extra libraries in
`libraryfolders.vdf`; the game installed through Proton lands there), else `~/mirrorsedge`,
`~/Games/mirrorsedge` or `~/Games/Mirror's Edge`. `--game-root <dir>` overrides it. The command line
and the controls are the ones in the README.

`--verify-all` renders without a window. It still needs a GL context: on a desktop SDL opens a hidden
window; without a display (a server, CI) run it under `xvfb-run`, or with SDL's off-screen driver on
Mesa (`SDL_VIDEODRIVER=offscreen`), which the renderer falls back to by itself when the default driver
cannot start. Its screenshots go to `screenshots/` under the current directory; those files are
tracked and were rendered by the Metal renderer, so run the oracle from another directory unless you
mean to replace them.

### Checking the backend on macOS

The OpenGL backend is also built on macOS, as `build/mirrorsedge_opengl` (CMake option `ME_OPENGL`,
on by default), on Apple's OpenGL 4.1 implementation. That is how it is developed and compared with
the Metal renderer on the machine that has the retail assets: the two accept the same command line,
so `--verify-all`, `--shots` and the interactive window can be run on both and their screenshots put
next to each other.

## What is where

| File | Role |
|---|---|
| `src/renderer/opengl_renderer.{hpp,cpp}` | The OpenGL backend, `me::OpenGLRenderer`. Same public interface as `MetalRenderer` apart from `init_with_window(SDL_Window*)` and `prepare_window_attributes()`. |
| `src/renderer/gl/` | `glcorearb.h` / `KHR/khrplatform.h` from the Khronos registry, and `gl_api.{hpp,cpp}`: the GL entry points the renderer uses, resolved through `SDL_GL_GetProcAddress`. Neither libGL nor OpenGL.framework is linked. |
| `src/renderer/renderer.hpp` | `me::Renderer`: the platform's backend. `main.cpp` uses only this name; `ME_RENDERER_OPENGL` selects this one. |
| `src/renderer/msl_to_glsl.{hpp,cpp}` | Translates the renderer's Metal Shading Language to GLSL 4.10. |
| `src/renderer/builtin_shaders_msl.hpp` | The built-in shaders (MSL), used by all backends. |
| `src/renderer/overlay_ui.inl`, `hud_font.hpp` | The HUD, cutscene overlay and chapter-select draw lists, included by all backends. |
| `src/renderer/render_common.hpp` | Texture layout helpers and the clip test, used by all backends. |
| `src/tools/glsl_main.cpp` | `me_glsl`: translates every built-in and material shader to GLSL and compiles it on the GPU. |
| `src/platform/platform.{hpp,cpp}` | Default game root, temp and cache directories, platform name. |
| `play_linux.sh` | Configure, build, run. |

## One copy of every shader

The shaders are written once, in MSL: the built-in passes, the sun shadow lookup
(`sun_shadow.hpp`) and the UE3 material shaders that `material_system.cpp` generates. The Metal
renderer compiles that text as it is, the Direct3D one translates it to HLSL, and this one passes
the same text through `MslToGlsl` and hands the result to the driver's GLSL compiler. A shader
edited for Metal therefore changes on Linux too, and nothing in `material_system.cpp` knows about
GLSL.

OpenGL 4.1 is the target because it is what every Linux driver (Mesa included) and Apple's OpenGL
offer. 4.1 has no `layout(binding = n)`: the translator reports the uniform block and sampler
names it emitted together with the MSL slots they stand for, and the renderer binds them by name
after linking the program.

Unlike Direct3D, OpenGL does not agree with Metal on everything outside the syntax. What the
emitted GLSL and the renderer do about it:

| Difference | Handling |
|---|---|
| Clip space z: Metal [0, 1], OpenGL [-1, 1] | Every vertex entry point ends with `gl_Position.z = 2.0 * gl_Position.z - gl_Position.w`. Depth tests, clears and the shadow maps' stored depths are then the same numbers as on Metal. |
| Framebuffer rows: Metal top-down, OpenGL bottom-up | The vertex entry point also flips `gl_Position.y`. Row 0 of an OpenGL render target then holds what row 0 (the top) of the Metal one holds, so render targets sampled by later passes, uploaded textures (row 0 = top), `gl_FragCoord.y`, `dFdy` and screenshot readback all match Metal with no further flips. The flip mirrors triangle winding, so the renderer declares front faces the opposite way round from the Metal backend, and the finished frame is blitted to the window with the y axis inverted. |
| Separate textures and samplers | GLSL 4.1 only has combined samplers. Each (texture, sampler) pair a shader samples becomes one `sampler*` uniform named `<texture>__<sampler>` (`t0__s0`); a texture sampled through a `constexpr sampler` is `<texture>__<static sampler>` where the static sampler is `me_ss_<function>_<name>` (`shadow_map__me_ss_sun_shadow_pcf_cmp`), one only measured with `get_width()` is `<texture>__default` (`sampler_slot` -1). The translator follows the texture through helper functions: a helper's `sampler` parameters are dropped and its texture parameters become combined samplers, so `mat_lm_bicubic(lm_a, lm_smp, uv)` is called as `mat_lm_bicubic(lm_a__scene_smp, uv)`. `sample_compare` makes the uniform a `*Shadow` sampler and the lookup `texture(s, vec4(uv, slice, ref))`; `depth2d` lookups are `textureLod(s, uv, 0.0).r`. The renderer binds the texture and a sampler object (`glBindSampler`) to one unit and points the uniform at it. |
| Vertex and fragment buffer slots are separate spaces in MSL | Uniform blocks are named by stage and slot (`VSB1`, `FSB0`); the renderer gives the two stages separate binding point ranges. A block whose members the shader never reads may be optimised away by the driver, so the renderer must accept `GL_INVALID_INDEX` for a reported block. |
| `constant T& u [[buffer(n)]]` | `layout(std140) uniform <stage>B<n> { T u; };`. std140 lays out the renderer's uniform structs (float4x4, packed_float3 + float pairs, float4 arrays) as MSL does, so the CPU-side structs are shared with the other backends unchanged. `constant float4* U [[buffer(n)]]` becomes `vec4 U[<pointer_array_size>]` in the block. |
| Vertices from `vertices[vertex_id]` | Attributes: member i of the vertex struct is `layout(location = i) in <type> in_<member>` (`uint` members stay `uint`, for `glVertexAttribIPointer`), and the renderer's vertex array object matches, as the Direct3D input layouts do. `[[stage_in]]` structs with `[[attribute(n)]]` use location n. |
| Varyings | The vertex stage's return struct members are `out <type> v_<member>` (`[[flat]]` and integer members `flat`), its `[[position]]` member `gl_Position`; the fragment stage's `[[stage_in]]` struct reads them back, `[[position]]` from `gl_FragCoord`. A `float4` fragment result is `layout(location = 0) out vec4 frag_out0`. |
| Integer literals | C++ converts `uint_value & 0xFF`'s literal; GLSL's bitwise operators want one signedness and Apple's compiler enforces it. A literal next to an operand whose declared type is unsigned (parameters, locals, struct members) gets a `u` suffix. |
| `fmod`, `saturate`, `select` | GLSL's `mod` floors where Metal's `fmod` truncates; a prelude at the top of every shader defines `me_fmod`, `saturate` and `select` with Metal's meaning. |
| Names | `float4` -> `vec4`, `float4x4` -> `mat4`, `half` -> `float`, `int2` -> `ivec2`, `dfdx` -> `dFdx`, `rsqrt` -> `inversesqrt`, `atan2` -> `atan`, `as_type<uint>` -> `floatBitsToUint`, `discard_fragment()` -> `discard`, `constant` -> `const`, `const T&` -> by value, `T&` -> `inout`, `template <typename T>` -> one overload per float..float4, `{...}` array initialisers -> `T[N](...)`, `metal::`, `inline`, `static` and `#include` dropped, identifiers that are GLSL keywords or built-in functions (`in`, `out`, `sample`, `filter`, `input`, `texture`, ...) get `_` appended. Matrix products need no rewriting: GLSL's `*` is Metal's. Only the helper functions an entry point can reach are emitted, so a vertex shader never sees fragment-only built-ins. |

Anything else passes through unchanged. If MSL written later uses a construct outside this list,
the GLSL compiler rejects it and the renderer prints the error and the shader's name at start-up
or when the level loads (`[OpenGLRenderer] shader error: ...`); a failed material falls back to the
legacy world shader, as on macOS.

`me_glsl` runs the whole pipeline without the game: it translates the built-in shaders and every
material shader of every chapter and compiles them on the GPU, printing what failed and why.

```bash
cmake --build build --target me_glsl
build/me_glsl                                 # the built-in shaders and every chapter's materials
build/me_glsl --chapter 3 --dump /tmp/glsl    # one chapter, writing the failed shaders' GLSL to a directory
build/me_glsl --builtin-only --dump-all /tmp/glsl   # just the built-ins, all of them written out
```

It exits with 1 when anything failed, so it can gate a merge.

## Where the OpenGL backend differs from the Metal one

- **Off-screen first.** Every frame is rendered to an RGBA8 framebuffer that is then blitted to the
  window (`glBlitFramebuffer`, y inverted). `P` / `F12` screenshots therefore work in a window, and
  `glReadPixels` on that framebuffer already returns the rows top-down.
- **Vertices come from vertex arrays**, not from a buffer indexed by `vertex_id`, so a mesh section is
  drawn with `glDrawArrays(first, count)`. OpenGL 4.1 ties attribute pointers to a buffer, so every
  vertex buffer carries its own vertex array object, set up the first time it is drawn with a layout.
- **Enemies are drawn indexed**, posed into mapped vertex buffers (`glMapBufferRange` on the context's
  thread, the posing on worker threads), as on Direct3D; enemies that cannot be seen are not posed.
- **Textures and samplers are bound per program.** The passes put textures and sampler objects into
  the MSL slots the shaders name, as the other backends do. Which GL texture unit a program reads a
  slot through is settled when it is linked (one unit per combined sampler, MSL `texture(n)` on unit
  n, a texture read through two samplers gets a spare unit from 32 up), and the renderer applies the
  current program's table before each draw, skipping what is already bound. The MSL's `constexpr`
  samplers become sampler objects fixed to their units at link time, so nothing is bound for them per
  pass. Uniform blocks go to binding points by stage: vertex `buffer(n)` -> n, fragment `buffer(n)` ->
  16 + n.
- **The level is not drawn under the front end.** While a front end frame covers the window the
  shadow, scene and post passes are skipped; the level's materials are still made resident.
- **Texture formats.** L8 and V8U8 use texture swizzles as Metal does (`GL_TEXTURE_SWIZZLE_*`): L8 is
  `GL_R8`, or `GL_SRGB8` filled through the red channel when sRGB (core OpenGL has no sRGB R8), V8U8
  `GL_RG8_SNORM`. BC1/2/3 need `GL_EXT_texture_compression_s3tc`, which Mesa and every vendor driver
  expose; without it the level's textures fall back to the material defaults. BC textures whose top
  level is not a multiple of 4 are accepted, as on Metal.
- **Texture upload and shader compilation are serial.** The Direct3D backend uploads a level's textures
  and compiles its material shaders on worker threads; a GL context belongs to one thread, so only
  the MSL -> GLSL translation runs in parallel and the GL calls follow on the main thread. A level's
  materials take 4 to 11 s to become resident on this Mac (see below).
- **The shadow pass's slope-scaled bias is capped** only where the driver has
  `GL_ARB/EXT_polygon_offset_clamp` (core in 4.6; Mesa has it, Apple does not). Without it the bias
  on near-grazing surfaces is not limited to `kSunShadowSlopeBiasCap`.
- **The shadow pass has a stand-in fragment stage.** The MSL has none (depth is all it writes); the
  renderer links `shadow_vertex` with an empty `main()`.
- **PNG screenshots are opaque.**
- **No shader cache of its own.** Mesa and the vendor drivers cache compiled programs themselves.

## Environment variables

| Variable | Effect |
|---|---|
| `MEDGE_ME_INSTALL` | The retail install (as for the tools in `tools/retail`). |
| `ME_RENDER_PROF=1` | Every 120 frames, print where a frame's time went, by phase, and the draw count. |
| `ME_GL_DEBUG=1` | Create a debug context and print the driver's messages (`GL_KHR_debug`, where the driver has it; Apple's does not, and there `glGetError` is polled after every pass instead). |
| `ME_CULL=off\|cw\|ccw` | Material back-face culling, as on macOS. |
| `SDL_VIDEODRIVER=offscreen` | Render without a display (Mesa). |

## What was run (2026-10-09)

On the macOS development machine (Apple M5 Pro, Apple's OpenGL 4.1 Metal - 90.5 driver, GLSL 4.10,
retail game in `~/mirrorsedge`), with `mirrorsedge_opengl`. Linux itself was not run: no Linux
machine was at hand, so the Linux build (`g++`, Mesa or a vendor driver, X11 / Wayland through SDL)
is untested. The code avoids compiler-specific constructs and Apple headers, and was reviewed with
GCC in mind, no more.

- **`--verify-all`: all stages pass**, run from a scratch directory so the tracked Metal screenshots
  stay as they are. Material shaders compiled: 213/213 (`Tutorial_p`), 537/537 (`Escape_p`), 289/289
  (`Edge_p`), 6/6 (`TdMainMenu`); every texture of every library uploaded. The same run under
  `ME_GL_DEBUG=1` reports no GL error after any pass.
- **`me_glsl`** (OpenGL 4.1 Metal - 90.5, Apple M5 Pro): built-in shaders 14/14 programs;
  material shaders Tutorial_p 213/213, Edge_p 289/289, Stormdrain_p 397/397, Cranes_p 372/372,
  Subway_p 451/451, Mall_p 465/465, Factory_p 440/440, Boat_p 330/330, Convoy_p 362/362,
  Scraper_p 425/425, Escape_p 537/537 — every shader the game generates compiles and links.
- **Against the Metal screenshots in `screenshots/`** (mean absolute difference over the RGB
  channels, in units of 1/255; the fraction of pixels off by more than 8 in brackets):

  | Image | MAD | Image | MAD |
  |---|---|---|---|
  | `tutorial_1_rooftop_start` | 0.90 (1.4 %) | `oracle_1_sprint_rooftop` | 0.80 (0.7 %) |
  | `tutorial_2_slide_airduct_gap` | 0.64 (1.1 %) | `oracle_2_springboard_vault` | 0.34 (0.1 %) |
  | `tutorial_3_wallrun_speedvault` | 0.63 (1.4 %) | `oracle_3_wallrun_tilt` | 0.56 (1.1 %) |
  | `tutorial_4_balance_wallclimb` | 0.28 (0.3 %) | `oracle_4_zipline_slide` | 1.17 (2.6 %) |
  | `tutorial_5_zipline_skillroll` | 0.84 (1.3 %) | `oracle_5_combat_disarm_reaction` | 0.20 (0.2 %) |
  | `tutorial_6_springboard_combat` | 0.39 (0.2 %) | `oracle_6_skill_roll_upside_down` | 0.33 (0.2 %) |
  | `oracle_6_sp01_edge_level` | 0.38 (0.0 %) | `oracle_7_chapter_select_menu` | 0.11 (0.0 %) |
  | `oracle_8_elevator_level_streaming` | 0.22 (0.0 %) | `oracle_9_cutscene_bink_player` | 0.37 (0.0 %) |

  Nothing is flipped or has its channels swapped (the same measure against the vertically flipped
  or BGR picture is 45 to 120). What differs is at triangle edges and in texture filtering: the two
  drivers rasterise and sample a little differently. No pass is missing in any picture.
- **The interactive window** (`--chapter 0 --max-frames 600`, 1280x720 at Retina scale): the level
  loads, plays and exits cleanly. `ME_RENDER_PROF=1` reports 3.5 to 4.5 ms of CPU time for the scene
  pass and about 1 ms for the translucent one per frame, 1,434 draws; the first frame of a level
  spends 4 s making the tutorial's 213 material programs and 486 textures resident (10.5 s for
  `Escape_p`'s 537 and 1,174).
- **`mirrorsedge_macos --verify-all`** still passes from the same tree (it shares `main.cpp` and the
  CMake files with this backend).

Not run: Linux (any distribution, Mesa, NVIDIA or AMD drivers, Wayland, `SDL_VIDEODRIVER=offscreen`),
a debug context with `GL_KHR_debug`, `glPolygonOffsetClamp`, window resizing and the fullscreen
toggle, a gamepad, a play-through of any level. The `P` key in the window was not pressed (the
runs were unattended and this machine does not let a script send keystrokes); what it calls is the
same `glReadPixels` of the off-screen framebuffer that produced the oracle pictures above.

## Known gaps

- **Linux untested.** Everything above was measured on Apple's OpenGL implementation. Mesa and the
  vendor drivers differ from it in what they optimise away, how strictly they validate, and their
  GLSL front ends; the first Linux run may turn up compile errors or binding mistakes that Apple's
  driver let pass. `me_glsl` is the first thing to run there.
- **Material residency is slow.** Textures are uploaded and programs linked one after another on the
  main thread, with the GLSL compiler of the driver doing the work (4 to 11 s per level here). The
  other backends do this on worker threads. Nothing is cached between runs beyond what the driver
  caches itself.
- **The shadow bias cap** needs `GL_ARB/EXT_polygon_offset_clamp`; without it (Apple) steep casters
  get a larger bias than on Metal. It did not show in the screenshots above.
- **Sampler count.** OpenGL 4.1 guarantees only 16 sampler uniforms per fragment stage. A material
  shader uses one per texture plus the light maps, the shadow map and the scene copies; a material
  with 14 textures that also reads the scene would exceed the limit, fail to link, and fall back to
  the legacy world shader with a message. No shipped material does.
- **An L8 texture flagged sRGB costs three bytes a texel** (`GL_SRGB8` through the red channel),
  since core OpenGL has no sRGB single-channel format.
- **No `SDL_VIDEODRIVER=offscreen` run.** The fallback to it when the default video driver cannot start
  is in place but was never exercised; it needs an SDL built with EGL support for the offscreen
  driver, which not every distribution's package has.
- As on macOS: the stand-in sky, sun and character shading, the approximated reaction-time and
  low-health effects.
