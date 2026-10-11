# Native Vulkan 1.0 Renderer Backend (`VulkanRenderer`)

`me::VulkanRenderer` (`src/renderer/vulkan_renderer.*`, `src/renderer/vk/vk_backend.*`, and `GlslDialect::Vulkan` in `src/renderer/msl_to_glsl.*`) draws the complete *Mirror's Edge* rendering pipeline natively on **Vulkan 1.0+** across **Android (`arm64-v8a`, default mobile renderer)**, **macOS (`arm64` via MoltenVK -> `mirrorsedge_vulkan` / `./play_vulkan.sh`)**, **Linux (`VK_KHR_surface` + SDL2)**, and **Windows**.

---

## Architecture Summary

| Component | Path | Responsibility |
|---|---|---|
| **MSL -> Vulkan GLSL 450 Translator** | `src/renderer/msl_to_glsl.{hpp,cpp}` | Translates all 14 built-in MSL shader programs and all 5,573 generated UE3 material shaders across all 11 campaign chapters into Vulkan `#version 450` GLSL (`layout(location = N)` varyings, explicit `set = 0` descriptor bindings `0..15` VS UBO, `16..31` FS UBO, `32..79` combined samplers, `uint(gl_VertexIndex)` for `[[vertex_id]]`, and Vulkan clip-space `y = -y` with native `[0, w]` depth preserved). |
| **Offline SPIR-V Validator** | `src/tools/glsl_main.cpp` | `./build/me_glsl --vulkan [--dump-all]` translates and compiles every built-in and level material shader via `glslc --target-env=vulkan1.0`. Verified **14/14 built-in shaders** and **5,573/5,573 material shaders** with **0 errors and 0 warnings**. |
| **Vulkan 1.0 Hardware Dispatcher (`vk_backend`)** | `src/renderer/vk/vk_backend.{hpp,cpp}` | Runtime loader (`VK_NO_PROTOTYPES` via `SDL_Vulkan_GetVkGetInstanceProcAddr` / `dlopen`), `VkInstance` + `VK_KHR_portability_enumeration`, `VkSurfaceKHR`, `VkSwapchainKHR` with per-swapchain-image render-done semaphores, persistent on-disk + multi-threaded worker-pool SPIR-V cache (`vk_spirv_cache_v1.bin` via `libshaderc` on desktop and NDK's bundled `glslang` + `SPIRV` C interface on Android), automatic subresource layout barrier tracking, overflow-safe transient ring/staging buffers, and graphics pipeline + render-pass + framebuffer caches. |
| **Vulkan Renderer Frontend** | `src/renderer/vulkan_renderer.{hpp,cpp}` | Complete feature-identical implementation of every render pass in `MetalRenderer` / `OpenGLRenderer` / `D3D11Renderer`: opaque & alpha-masked UE3 materials, Beast HL2 directional lightmaps, 2D array cascaded sun shadow maps (`VK_FORMAT_D32_SFLOAT`), dynamic actor light environments & modulated shadows, translucent/additive/modulate pass reading scene color/depth copies, first-person `CH_Faith_1P` skeletal mesh & weapons, GPU particle systems, decals, lens flares, auto-exposure metering, bloom, `TdDirHaze`, `TdToneMapping` color curves, `TdMotionBlur`, Bink cutscene video playback, touch controls overlay, and 2D vector HUD. |
| **Silent / Headless Testing (`--mute` / `ME_MUTE=1`)** | `src/main.cpp` | Full CLI (`--mute`), environment variable (`ME_MUTE=1`), and automatic muting whenever `--max-frames` or `--exit-screenshot` is active (`SDL_AUDIODRIVER=dummy`, headless OpenAL initialization, and `-no-audio` in `android/run_emulator_smoke.sh`). |

---

## Building & Running

### Desktop (`mirrorsedge_vulkan`)
```bash
# Quick launch (macOS / Linux):
./play_vulkan.sh

# Silent / muted automated smoke test:
./play_vulkan.sh --mute --chapter 0 --max-frames 60 --exit-screenshot /tmp/vk_smoke.png

# Full 25-stage deterministic oracle verification suite (100% muted):
ME_MUTE=1 ./build/mirrorsedge_vulkan --verify-all
```

### Android (`com.pippy360.mierrorsedgere`)
`ME_VULKAN` defaults to `ON` (`-PmeVulkan=true` in Gradle), building `VulkanRenderer` + NDK's bundled `glslang`/`SPIRV` directly into 16 KiB page-aligned `libmain.so` (`LOAD align 0x4000`):
```bash
cd android
./fetch_deps.sh
./gradlew assembleDebug

# Silent emulator / connected-device smoke test (no audio):
ME_FRAMES=40 ./run_emulator_smoke.sh /tmp/android_vk_smoke
```
To build the fallback OpenGL ES 3.0 backend on Android instead of Vulkan, pass `-PmeVulkan=false` (`./gradlew assembleDebug -PmeVulkan=false`).
