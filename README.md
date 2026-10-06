# Mirror's Edge Native macOS Reimplementation (`mierrorsedgere`)

A high-performance, clean-room native macOS port and engine reimplementation of *Mirror's Edge* (PC Unreal Engine 3 CookedPC) designed from first principles for Apple Silicon (arm64, M-series) and macOS Metal 3.0.

Built under the `universal-modder` methodology (Pattern 4: *Reimplement, then Fuse*), `mierrorsedgere` directly loads and runs retail PC assets (UPK/ME1 packages, Ogg Vorbis audio banks, INI physics configs, and INT localization files) from the user's local game installation without redistributing or modifying proprietary game binaries.

---

## Architecture Overview

The engine fuses five specialized native subsystems into a single executable (`mirrorsedge_macos`):

1. **Asset & Package Loader (`src/assets/upk_loader.*`, `src/assets/ini_config.*`)**:
   - High-throughput binary Unreal Package (`.upk` / `.me1`) parser for UE3 package version 536 / licensee version 43.
   - Built-in LZO1X-1 decompression engine resolving linear memory address spaces for multi-chunk packages (`Entry.upk`, `SP00/Tutorial_p.me1` through `SP09/Scraper_p.me1`).
   - UE3 INI and INT configuration and localization parser reading physics constants from `DefaultPawnMovement.ini`, campaign progression from `DefaultGame.ini`, and dialogue from `Subtitles.int`.
   - Automatic sub-level slice discovery (`*_Art`, `*_Spt`, `*_Lgts`, `*_Slc`) assembling contiguous 3D rooftop geometry and swept collision hulls.
   - **Material system** (`src/assets/material_system.*`, `package_manager.*`, `texture_loader.*`, `ue3_props.*`): resolves every static-mesh element's real UE3 material, including component overrides, `MaterialInstanceConstant` chains, parameters and static switches. It loads the referenced DXT/RGBA textures and cubemaps from any CookedPC package and translates each UE3 material expression graph into Metal Shading Language. The level sun comes from the level's baked `DirectionalLight`. See [`docs/MATERIAL_SYSTEM.md`](docs/MATERIAL_SYSTEM.md).

2. **Discrete Kinematic Parkour Controller (`src/physics/parkour_controller.*`)**:
   - 120 Hz fixed substepping physics engine implementing 100% authentic *Mirror's Edge* movement mechanics:
     - **Momentum Locomotion**: Seamless acceleration curve from jog (260 u/s) to full sprint (630 u/s, ~24 km/h) with dynamic FOV scaling (100° to 108°).
     - **Wallrun & Wall Climb**: Incident angle filtering (0°–57° wallrun, <33° climb) with 15° camera Dutch tilt and vertical boost impulses.
     - **Vault & Springboard**: Swept obstacle detection triggering Speed Vaults and high-impulse Springboards (`JumpZ = 950 u/s`).
     - **Ledge Grab & Zipline**: Ray-cast ledge pull-ups and line-segment zipline cable traversal.
     - **Crouch Slide & Coil**: Sprint-initiated low-friction slides under ventilation ducts, mid-air leg coils (+60 unit clearance), and momentum-preserving Skill Rolls.
     - **Combat & Disarm**: Interactive weapon snatch (`MOVE_Snatch`) against patrol cops/SWAT bots, firearm ballistics, and Reaction Time slow-motion (0.25x time dilation with cool blue tint).

3. **Apple Metal 3.0 Graphics Engine (`src/renderer/metal_renderer.*`)**:
   - Native Apple Metal shader pipelines written in Metal Shading Language (MSL):
     - `Generated UE3 materials`: one pipeline per translated material shader, drawn per mesh section with back-face culling (two-sided materials excepted). Opaque/Masked sections go in the base pass. Translucent/Additive/Modulate sections go in a second pass that reads copies of scene colour and depth.
     - `TdDirHaze`: Directional atmospheric sun haze, sky dome gradient, sharp corona, and horizon glare.
     - `BasePass + Beast Radiosity`: High-key white architectural aesthetic, dual-hemisphere ambient bounce (cyan sky / warm ground), and contact ambient occlusion.
     - `Runner Vision (LOI)`: Dynamic breathing scarlet red (`#E61414`) pulse on parkour targets, springboard ramps, and conduit pipes.
     - `CH_Faith_1P`: Articulated procedural first-person viewmodel (scarlet red runner glove, forearm runner eye tattoo, split-toe tabi shoes, and dynamic weapon handling).
     - `TdToneMapping & TdMotionBlur`: DICE photographic S-curve contrast scaling, radial speed blur, and low-health vignette.
     - `2D Vector HUD & Bitmap Font`: Minimalist built-in ASCII typography overlay, dynamic center reticle, momentum speedometer, health/reaction gauges, subtitle prompts, and interactive Chapter Select modal.
   - Dual-Mode: Seamless switching between interactive windowed mode (SDL2 + `CAMetalLayer` with Retina high-DPI support) and zero-copy shared memory headless mode for automated verification.

4. **Dynamic Audio Engine (`src/audio/audio_engine.*`)**:
   - Native OpenAL spatial audio subsystem with Xiph.Org libVorbis streaming.
   - Loads stock audio packages (`A_Bodyfalls.upk`, `A_Weapons.upk`, etc.) directly from `TdGame/CookedPC/Audio/`.
   - Procedural sound fallbacks ensuring 100% offline playback reliability.
   - Realistic parkour foley (cadenced footsteps, jump grunts, wallrun friction, vault impacts, slide sweeps, zipline whines, and disarm clicks).

5. **Engine Integration & Oracle Harness (`src/main.mm`)**:
   - Complete Objective-C++ application combining interactive gameplay with automated headless verification testing.

---

## Build & Installation

### Prerequisites (macOS Apple Silicon)
- **macOS:** macOS 14+ (tested on Darwin 25.6.0 arm64, Apple M5 Pro)
- **Compiler:** Apple Clang / Xcode Command Line Tools (`clang++ -std=c++20 -fobjc-arc`)
- **Build System:** CMake 3.20+
- **Homebrew Packages:**
  ```bash
  brew install sdl2 libvorbis libogg
  ```

### Quick Launch
Simply execute the included launcher script:
```bash
./play_macos.sh
```
This automatically configures the CMake build directory, compiles with all CPU cores, and launches the game in interactive 1280x720 windowed mode.

### Manual CMake Build
```bash
mkdir -p build
cmake -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(sysctl -n hw.ncpu)
```

---

## Controls Reference

| Input | Action |
|---|---|
| **W, A, S, D** / Arrow Keys | Move (Automatic momentum acceleration from jog to full sprint) |
| **Mouse Motion** (or **I / K / J / L**) | Look Pitch & Yaw |
| **Space** | Jump / Wallrun / Wallclimb / Vault / Springboard / Ledge Pull-Up |
| **C** / **Left Ctrl** / **Left Shift** | Crouch / Low-Friction Slide / Mid-Air Coil / Skill Roll |
| **Q** | 180° Instant Turn |
| **Left Mouse** / **F** | Melee Punch/Kick or Fire Weapon |
| **Right Mouse** / **E** | Disarm Enemy Weapon (`MOVE_Snatch`) |
| **X** | Toggle Reaction Time Slow-Motion (0.25x Dilation) |
| **V** / **Left Alt** | Runner Vision Look-At Target |
| **Tab** / **M** | Toggle Chapter Select Menu (Releases/Captures Mouse Cursor) |
| **0 .. 9** | Directly load Campaign Chapter (`0` = Prologue Tutorial through `9` = The Shard) |
| **R** | Reset Faith to Checkpoint / Spawn |
| **P** / **F12** | Save High-Resolution PNG Screenshot to `screenshots/` |
| **Escape** | Close Menu / Quit Application |

*Gamepad / Controller support:* Fully supports Xbox, PlayStation, and MFi gamepads via SDL2 GameController API.

---

## Headless Deterministic Oracle Verification

The engine features a built-in verification suite that validates assets and executes an automated, deterministic 8-stage parkour gauntlet in headless Metal mode:

```bash
./build/mirrorsedge_macos --verify-all
```

### Verification Stages & Assertions:
1. **Config & Localization Audit**: Verifies `DefaultPawnMovement.ini`, `Subtitles.int`, and `DefaultGame.ini`.
2. **Audio & Map Package Audit**: Validates `A_Bodyfalls.upk`, `Entry.upk`, `SP00/Tutorial_p.me1`, and `SP01/Edge_p.me1`.
3. **Stage 1 (Sprint Acceleration)**: Verifies momentum timer accumulation, top sprint speed (>400 u/s), and dynamic FOV widening (>100°).
4. **Stage 2 (Speed Vault & Springboard)**: Tests hurdle clearance (`MOVE_SpeedVaulting`) and super-jump impulse (`MOVE_SpringBoarding`, `JumpZ = 950 u/s`).
5. **Stage 3 (Wallrun & Camera Tilt)**: Validates wall engagement angle, forward acceleration, and 15° camera Dutch roll (`MOVE_WallRunningRight`).
6. **Stage 4 (Wallclimb & Ledge Grab)**: Tests vertical wall ascent and ledge pull-up onto elevated structures (`MOVE_WallClimbing`).
7. **Stage 5 (Zipline & Crouch Slide)**: Verifies cable attachment, gravitational descent, rooftop landing, and low-clearance crouch slide (`MOVE_ZipLine`, `MOVE_Slide`).
8. **Stage 6 (Mid-Air Coil & Skill Roll)**: Verifies mid-air leg retraction (`MOVE_Coil`, +60 unit boost) and buffered landing momentum retention (`MOVE_SkillRoll`).
9. **Stage 7 (Combat Disarm & Reaction Time)**: Validates enemy disarm QTE (`MOVE_Snatch`), weapon equip (`Colt1911`), and 0.25x reaction time slow-motion dilation.
10. **Stage 8 (Retail Level & UI Overlay)**: Renders `SP01/Edge_p.me1` map package geometry with first-person Faith viewmodel and interactive Chapter Select menu.

Telemetry is exported to `/tmp/me_oracle_telemetry.json` and 7 high-resolution PNG verification screenshots are exported to `screenshots/`.

---

## Command-Line Options

```
Usage:
  mirrorsedge_macos [options]

Options:
  --verify-all             Run deterministic headless oracle verification suite
  --headless-oracle <file> Run script-based headless oracle
  --test-replay <trace>    Replay physics trace headless
  --chapter <0..9>         Start at specified campaign chapter (0: SP00, 1: SP01, etc.)
  --level <path>           Load custom level package (.me1 or .upk)
  --max-frames <N>         Exit cleanly after rendering N frames (useful for smoke tests)
  --game-root <dir>        Set retail game assets directory (default: /Users/tomnom/mirrorsedge)
  --help, -h               Show help message
```

---

## License & Compliance

Clean-room reimplementation written from first principles for macOS Apple Silicon. Contains no proprietary game binaries, decompiled bytecodes, or extracted copyrighted assets. All game assets remain the property of Electronic Arts and DICE and are read directly from the user's legal game installation.
