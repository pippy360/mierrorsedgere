# The Android Port

The game as an Android app (`android/`, package `com.pippy360.mierrorsedgere`): the engine built
with the NDK into `libmain.so`, drawn with **OpenGL ES 3.0** by the same backend that draws on
Linux, inside SDL2's Java activity. Everything but the window, the GL dialect and the controls is
the source the desktop builds compile: the asset loaders, the parkour controller, the level
script, audio, the front end and `src/main.cpp`. arm64-v8a, minSdk 26 (Android 8), targetSdk 35.

How to build, install, push the assets and run it is in [`android/README.md`](../android/README.md).
This document is what the port is made of, what was measured, and what is still missing.

## What is where

| File | Role |
|---|---|
| `android/` | The Gradle project: `app/build.gradle` (AGP 8.8, `externalNativeBuild` over the repository's `CMakeLists.txt`), `AndroidManifest.xml` (landscape, immersive, `glEsVersion 0x30000`), `MirrorsEdgeActivity.java` (`SDLActivity` with the command line from an intent extra or `me_args.txt`), `fetch_deps.sh` (SDL2 2.32, OpenAL Soft 1.24, libogg 1.3.6, libvorbis 1.3.7 into the ignored `android/third_party/`), `run_emulator_smoke.sh`. |
| `CMakeLists.txt`, `elseif(ANDROID)` | `libmain.so` from `src/main.cpp`, the game objects and the OpenGL renderer sources with `ME_RENDERER_OPENGL=1 ME_GLES=1 ME_NO_FFMPEG=1`; SDL2 shared (the activity loads it), OpenAL Soft (OpenSL ES backend), ogg and vorbis static; the NDK's zlib. The headless tools are not built. |
| `src/renderer/opengl_renderer.cpp` | The GL backend with its ES path (`gl::is_es()` at run time, `ME_GLES` or env `ME_GLES=1` to ask for an ES context): ES 3.2 → 3.1 → 3.0 context, ES spellings of the API, CPU-decoded DXT, the limits it checks. |
| `src/renderer/msl_to_glsl.*` | `MslToGlsl::set_dialect(GlslDialect::ES, 300)`: `#version 300 es` with precision defaults, the ES strictness fixes. The desktop output is unchanged. |
| `src/renderer/bc_decode.hpp` | BC1 / BC2 / BC3 block decoder (DXT1 / 3 / 5 → RGBA8), for GPUs without S3TC. |
| `src/renderer/gl/gl_api.*` | The loader: only functions in both GL 4.1 core and ES 3.0 are required; `glDrawBuffer`, `glClearDepth`, `glDepthRange`, `glGetQueryObjectui64v` are optional; `OES` is tried as a suffix. `gl::is_es()`, `gl::version_major()`. |
| `src/renderer/mod_shadow.hpp` | The modulated-shadow atlas laid out over the shadow map the renderer actually allocated (`map_size`), not necessarily `kSunShadowMapSize`. |
| `src/platform/platform.cpp` | `platform_name()` "Android"; the game root under the app's external files directory; temp and cache directories in the app's storage. |
| `src/platform/android_log.*` | stdout and stderr piped to logcat (tag `mirrorsedge`): every `std::cout` line of the engine shows in `adb logcat -s mirrorsedge`. |
| `src/platform/touch_controls.*`, `touch_overlay.hpp` | The on-screen controls, and what the HUD draws for them (`overlay_ui.inl`'s `draw_touch_overlay`, shared by all backends; nothing on the desktop). |
| `src/cutscene/cutscene_player.cpp` | `ME_NO_FFMPEG`: no Bink decoding; `play_bink_movie()` says so and returns false, and the game goes on as it does when a `.bik` is missing. |
| `src/tools/glsl_main.cpp` | `me_glsl --es [300\|310\|320]`: the ES dialect, validated offline with the NDK's `glslc`. |

## OpenGL ES 3.0 from the OpenGL 4.1 backend

The backend was written for OpenGL 4.1 core because that is what Linux and macOS share
([`docs/LINUX_PORT.md`](LINUX_PORT.md)). ES 3.0 is what the Android emulator on this Mac exposes
(`ANDROID_EMU_gles_max_version_3_0`, over Apple's OpenGL 4.1), and what every phone since 2013
has, so it is the baseline; a 3.1 or 3.2 context is taken when offered and changes nothing.
What ES wants differently, and what the port does:

| Difference | Handling |
|---|---|
| GLSL dialect | `#version 300 es` with `precision highp` for `float`, `int` and every sampler type used (`sampler2D`, `sampler2DArray`, `sampler2DShadow`, `sampler2DArrayShadow`, `samplerCube`, `samplerCubeShadow`) in both stages. GLSL ES has no implicit `int` → `float` or `int` → `uint` conversion: an integer literal passed to a helper's `float` / `uint` parameter is spelled `0.0` / `0u` (the one place it bit: `sun_shadow_pcf(maps, 0, ...)`). Everything else the translator emits is in the 3.00 subset already (no `textureGather`, bitfield operations, images, SSBOs or `layout(binding)`). |
| API spellings | `glDrawBuffers(1, &x)` for `glDrawBuffer`; `glClearDepthf`, `glDepthRangef`; the four `GL_TEXTURE_SWIZZLE_R/G/B/A` parameters instead of `GL_TEXTURE_SWIZZLE_RGBA`; no `GL_TEXTURE_CUBE_MAP_SEAMLESS` (ES filters seamlessly by itself). |
| Texture formats | No `GL_BGRA` uploads: BGRA8 textures are swapped to RGBA on the CPU. No `GL_SRGB8` from a red channel: an sRGB L8 texture is expanded to RGB bytes. `GL_RGBA16` is not core: the exposure metering's targets are `RGBA16F` unless `GL_EXT_texture_norm16` is there; the 1 x 1 exposure is `R32F` with `GL_EXT_color_buffer_float`, else `R16F`. |
| DXT | Phones have no S3TC. Without `GL_EXT_texture_compression_s3tc` (or with `ME_DXT_CPU=1`) every DXT1 / 3 / 5 texture is decoded to RGBA8 (`sRGB8_ALPHA8`) on the CPU, every mip, and uploaded uncompressed: four to eight times the GPU memory. Against the GPU's own decode the pictures differ by 0.3–0.4 / 255 on average (the 1/3–2/3 interpolants round differently). `ME_TEX_DROP_MIPS=<n>` leaves out the n largest levels of the material and light-map textures, to fit a smaller device. |
| HDR targets | `RGBA16F` colour attachments need `GL_EXT_color_buffer_float` or `_half_float`; without either the renderer refuses to start with a message. Every device with ES 3.0 and a 2015-or-later GPU has one of them. |
| Depth textures | ES 3.0 makes a depth texture sampled with linear filtering and no comparison *incomplete* (it reads 0). The scene depth, its copy and the shadow array are therefore bound through a nearest sampler when read without comparison; the shadow lookups keep their comparison sampler. |
| The shadow array | Some ES drivers (the emulator's translator among them) apply `GL_MAX_3D_TEXTURE_SIZE` to 2D array textures and refuse a 4096 x 4096 x 3 depth array. On ES the array is as large as the device allows up to `kSunShadowMapSize` (`ME_SHADOW_SIZE=<n>` overrides), the viewports and the modulated-shadow atlas follow the size that was allocated (`layout_mod_shadows(.., map_size)`), and the shaders measure the texel from the texture itself. At 2048 the near cascade's texel is 3.5 uu instead of 1.76, the far one's 24 instead of 12, and four modulated shadows fit the atlas instead of eight. The desktop backends keep 4096. |
| Limits | ES 3.0 guarantees 16 textures a fragment stage, 32 combined units, 15 varying vectors, 24 uniform buffer bindings, 16 KB a uniform block. The renderer prints what the device has (`[OpenGLRenderer] Limits:`), assigns units within them (MSL `texture(n)` to unit n when free, extras from what is left) and refuses to start, naming the limit, when one is short. The material shaders need at most 11 fragment samplers and 11 varying vectors. |
| Timer queries | Only with `GL_EXT_disjoint_timer_query`; otherwise `ME_RENDER_PROF=1` reports wall-clock time alone. |
| Debug output | `GL_KHR_debug` where the driver has it (the emulator has not); `glGetError` is polled after each pass under `ME_GL_DEBUG=1` otherwise. |

`me_glsl --es` runs the translation on the desktop and validates every shader with the NDK's
`glslc` (`--target-env=opengl`). glslc wants `#version 310 es` at least, so the 300 sources are
checked as 310: a superset for what the translator emits, not a bit-exact 300 check; the
emulator's own compiler is the final arbiter and the renderer prints the shader's name and log
when one fails.

## The app

- **Entry.** `SDLActivity` loads `libSDL2.so` and `libmain.so` and calls `SDL_main`; `main.cpp`
  does not define `SDL_MAIN_HANDLED` on Android so `main` is that symbol. The first thing it does is
  pipe stdout and stderr to logcat and `chdir` to the external files directory
  (`/sdcard/Android/data/com.pippy360.mierrorsedgere/files`), so `screenshots/` and every
  relative path land where `adb pull` reaches them.
- **Command line.** `MirrorsEdgeActivity.getArguments()` splits the intent extra `args`, else
  `<files>/me_args.txt`, else nothing (the main menu). All desktop options work; `--exit-screenshot
  <png>` (new, every platform) saves the frame `--max-frames` ends on.
- **Assets.** `default_game_root()` is `<files>/mirrorsedge`, then `/sdcard/mirrorsedge`;
  `MEDGE_ME_INSTALL` and `--game-root` override. Nothing is bundled: the retail `TdGame/` is pushed
  with `adb` (`android/README.md`), 7 GB for everything, 3 GB for the training area and the
  Prologue without the movies.
- **Window.** Fullscreen, landscape only, immersive; the drawable is the display (2400 x 1080 on
  the emulator), `SDL_WINDOW_OPENGL` with the ES attributes the renderer sets. The window surface
  is blitted from the off-screen RGBA8 target as on Linux, so screenshots and `--exit-screenshot`
  are the same path.
- **Controls.** Touch, through SDL's finger events (`src/platform/touch_controls.*`), active when
  the device has a touch screen (or `ME_TOUCH=1` on the desktop, to see the overlay): the left 40 %
  of the screen is a move stick that centres where the finger lands; the right 60 % is the look
  area (drag = yaw / pitch, scaled by the options' sensitivity); JUMP, CROUCH, ACTION (use / disarm),
  ATTACK (melee / fire), 180 and RT buttons on the right, MENU top-left. Each finger is tracked by
  id, so a button can be held while the stick moves and the view turns. A JUMP tap during a
  cutscene or a hint card is a Space press (skip / accept), MENU is Escape, and Android's Back
  button is Escape too. The menus and the front end take SDL's synthesized mouse events from the
  first finger as clicks, exactly as a mouse; in play those synthesized events are ignored so a tap
  does not punch. Bluetooth gamepads work through SDL's controller API as on the desktop.
- **Background / foreground.** SDL blocks the game thread while the activity is paused and brings
  the surface back on resume (`SDL_APP_*` events clear the finger state). The GL context is kept by
  the emulator and by every modern device; a device that drops it would need the level's GPU
  objects rebuilt, which is not done.
- **Cutscenes.** No FFmpeg: the chapter movies (`.bik`) are reported unavailable and the game goes
  to the level's in-engine intro, as it does on the desktop when a movie file is missing. The
  in-engine Matinee intros and cutscenes, subtitles and the voice lines play.
- **Audio.** OpenAL Soft on OpenSL ES, Vorbis decoded as on the desktop.

## What was run (2026-10-11)

On the Android emulator (AVD `Medium_Phone`, API 37 arm64 image, 8 GB RAM, `-gpu auto`: the
"Android Emulator OpenGL ES Translator" over Apple's OpenGL 4.1 on an M5 Pro, OpenGL ES 3.0,
GLSL ES 3.00, `GL_EXT_color_buffer_float`, no S3TC, no `GL_KHR_debug`, no timer query), from
`./gradlew assembleDebug` (AGP 8.8.1, Gradle 8.11.1, NDK 27.0.12077973, Homebrew CMake 4.4):

- **The APK**: 6.4 MB, `lib/arm64-v8a/`: `libSDL2.so` 1.9 MB, `libmain.so` 5.2 MB (OpenAL Soft,
  ogg, vorbis inside), `libc++_shared.so` 1.3 MB.
- **Chapter 0** (`--chapter 0 --max-frames 400 --exit-screenshot screenshots/android_smoke.png`):
  the level loads (2403 static meshes, 582,078 collision triangles, 378 materials, 536 textures,
  99 light maps), **271/271 material shaders compile** on the emulator's GLSL ES compiler, the 536
  textures are CPU-decoded and uploaded in 10.5 s, the training area's opening pan plays with its
  voice line, 400 frames are rendered and the screenshot is written: the picture is the same scene
  as `screenshots/tutorial_1_rooftop_start.png`'s level drawn by the macOS build — light maps,
  shadows, sky, haze, the billboard, the text. The run ends with `[Game] Shutdown cleanly`.
- **Memory on the device** during play: 2.1 GB native heap, 2.4 GB PSS (the GPU's memory is the
  host's on the emulator). See below.
- **Frame rate** on the emulator: about 13–15 fps at 2400 x 1080 through the translator; not a
  measure of a phone.
- **A Pixel 9** (Android 17, Mali-G715, OpenGL ES 3.2, 12 GB, 2424 x 1080): the same APK installs,
  the level loads, **271/271 material shaders compile on Mali's compiler** and the 536 textures are
  resident in 2.3–2.7 s (against 10.5 s on the emulator); the training area plays with the HUD, the
  subtitles and the touch overlay, the stick answering a finger. 5.4 GB PSS, of which 3.5 GB is
  graphics memory (the uncompressed textures and render targets, counted against the process on a
  phone). Android 15+'s 16 KB page-size check passes: every shared library is linked with
  `-z max-page-size=16384` (SDL2's own link flags had left `libSDL2.so` at 4 KB) and the APK stores
  them uncompressed and 16 KB-aligned.
- **Shaders offline**: `me_glsl --es` validates the 14 built-in programs and all 5,573 material
  shaders of the eleven maps as GLSL ES (as 310, see above), with no shader over the ES limits.
- **The desktop builds are unchanged**: `mirrorsedge_macos --verify-all` and `mirrorsedge_opengl
  --verify-all` pass every stage from this tree; the GLSL 4.10 the translator emits is byte for
  byte what it emitted before (`me_glsl --dump-all`, 292 files of chapter 0 and the 20 built-ins
  diffed).

Not run: a play-through of a level on the phone (it was started and looked at, not played
through); the touch layout tuned by hand; an Adreno device; a Bluetooth gamepad; going to the
background and back; a release build.

## Known gaps

- **Memory.** The engine keeps about 4 GB resident on macOS for chapter 0, 2.1–2.4 GB on the
  emulator (where the GPU's copies are the host's) and 5.4 GB PSS on a Pixel 9 (3.5 GB of it
  graphics). A phone with 8 GB or more runs it; one with 4–6 GB may see it killed. The buckets, measured on macOS: the 81 character and weapon DXT1 textures
  decoded to RGBA8 with mip chains at start-up (~0.8 GB on the GPU and as much again in driver
  copies); every stock audio bank decoded to PCM at start (560 MB, half of it the same clips
  stored twice under their short and full names, `AudioEngine::store_clip`); the CPU copies of
  the level's textures (156 MB), light maps (58 MB) and vertices (169 MB) kept after upload. On
  Android the CPU DXT decode adds the uncompressed level textures on top. `ME_TEX_DROP_MIPS=1`
  quarters the texture memory at a visible cost. These are in `TODO.md`.
- **No Bink movies**: the chapter cutscenes are skipped. FFmpeg's Bink decoder would have to be
  cross-compiled with the NDK (`--enable-decoder=bink,binkaudio_dct,binkaudio_rdft
  --enable-demuxer=bink`) and linked in place of the `ME_NO_FFMPEG` stub; nothing in the player
  needs to change for it.
- **Shadow map resolution**: 2048 on a device that caps array textures at 2048 (the emulator;
  Adreno and Mali report 2048 for `GL_MAX_3D_TEXTURE_SIZE` too). The far cascade's offsets in the
  shader are still those of a 12 uu texel.
- **Material residency is serial** and on the game thread, as on Linux: 10 s for the training
  area on the emulator, longer on a phone; the first frame of a level waits for it.
- **The touch layout is a first draft**: fixed sizes in fractions of the screen, no options
  screen for them, no haptics, no gyroscope look.
- **No app icon beyond the default**, no release signing, no Play-store packaging (the assets
  cannot be redistributed anyway).
- As on Linux: the stand-in sky, sun and character shading, the approximated effects.
