# Mirror's Edge Native macOS Reimplementation (`mierrorsedgere`)

A high-performance, clean-room native macOS port and engine reimplementation of *Mirror's Edge* (PC Unreal Engine 3 CookedPC) designed from first principles for Apple Silicon (arm64, M-series) and macOS Metal 3.0.

The same game also builds and runs on Windows, drawn with Direct3D 11: see [`docs/WINDOWS_PORT.md`](docs/WINDOWS_PORT.md).

Built under the `universal-modder` methodology (Pattern 4: *Reimplement, then Fuse*), `mierrorsedgere` directly loads and runs retail PC assets (UPK/ME1 packages, Ogg Vorbis audio banks, INI physics configs, and INT localization files) from the user's local game installation without redistributing or modifying proprietary game binaries.

---

## Architecture Overview

The engine fuses five specialized native subsystems into a single executable (`mirrorsedge_macos`):

1. **Asset & Package Loader (`src/assets/upk_loader.*`, `src/assets/ini_config.*`)**:
   - High-throughput binary Unreal Package (`.upk` / `.me1`) parser for UE3 package version 536 / licensee version 43.
   - Built-in LZO1X-1 decompression engine resolving linear memory address spaces for multi-chunk packages (`Entry.upk`, `SP00/Tutorial_p.me1` through `SP09/Scraper_p.me1`).
   - UE3 INI and INT configuration and localization parser reading physics constants from `DefaultPawnMovement.ini`, campaign progression from `DefaultGame.ini`, and dialogue from `Subtitles.int`.
   - Automatic sub-level slice discovery (`*_Art`, `*_Spt`, `*_Lgts`, `*_Slc`) assembling contiguous 3D rooftop geometry.
   - Real UE3 level collision (`src/physics/collision_world.*`): StaticMesh `BodySetup` convex hulls and kDOP triangle trees (honouring `UseSimpleBoxCollision` / `UseSimpleLineCollision` / `bCollideComplex`), `BlockingVolume` brushes and BSP, with the `BlockNonZeroExtent` / `BlockZeroExtent` channels. Elevator cabs and doors are the real moving `InterpActor`s with their own collision.
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
     - `CH_Faith_1P`: the real skinned first-person mesh (`USkeletalMesh` + `TdAnimSet`, `src/anim/anim_system.*`), and the real skinned KrugerSec / CPF enemies and weapons.
     - `TdToneMapping & TdMotionBlur`: DICE photographic S-curve contrast scaling, radial speed blur, and low-health vignette.
     - `2D Vector HUD & Bitmap Font`: Minimalist built-in ASCII typography overlay, dynamic center reticle, momentum speedometer, health/reaction gauges, subtitle prompts, and interactive Chapter Select modal.
   - Dual-Mode: Seamless switching between interactive windowed mode (SDL2 + `CAMetalLayer` with Retina high-DPI support) and zero-copy shared memory headless mode for automated verification.
   - On Windows, `src/renderer/d3d11_renderer.*` draws the same passes with Direct3D 11. It has no shaders of its own: the MSL above and the generated material shaders are translated to HLSL at start-up (`src/renderer/msl_to_hlsl.*`).

4. **Dynamic Audio Engine (`src/audio/audio_engine.*`)**:
   - Native OpenAL spatial audio subsystem with Xiph.Org libVorbis streaming.
   - Loads stock audio packages (`A_Bodyfalls.upk`, `A_Weapons.upk`, etc.) directly from `TdGame/CookedPC/Audio/`.
   - Procedural sound fallbacks ensuring 100% offline playback reliability.
   - Realistic parkour foley (cadenced footsteps, jump grunts, wallrun friction, vault impacts, slide sweeps, zipline whines, and disarm clicks).

5. **Engine Integration & Oracle Harness (`src/main.cpp`)**:
   - Complete C++ application combining interactive gameplay with automated headless verification testing, the same source on macOS and Windows.

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

### Windows
Install [MSYS2](https://www.msys2.org) and, in its shell, the UCRT64 packages:
```bash
pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,SDL2,openal,libvorbis,ffmpeg,zlib}
```
Then run the launcher from `cmd` or PowerShell:
```bat
play_windows.bat
```
It builds `build-win/mirrorsedge_windows.exe` and starts it; arguments are passed on (`play_windows.bat --chapter 1`). The retail install is found through Steam, or set `MEDGE_ME_INSTALL` or pass `--game-root`. Details, what was verified and the known gaps are in [`docs/WINDOWS_PORT.md`](docs/WINDOWS_PORT.md).

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

The engine features a built-in verification suite that validates assets and drives the parkour controller through 8 deterministic stages in headless Metal mode. Stages 1–7 run against the real collision of `SP00/Tutorial_p.me1` at the tutorial's own training spots; stage 8 rides the real lift in `SP01/Escape_p.me1`. No procedural test geometry is involved:

```bash
./build/mirrorsedge_macos --verify-all
```

### Verification Stages & Assertions:
1. **Config & Localization Audit**: Verifies `DefaultPawnMovement.ini`, `Subtitles.int`, and `DefaultGame.ini`.
2. **Audio & Map Package Audit**: Validates `A_Bodyfalls.upk`, `Entry.upk`, `SP00/Tutorial_p.me1`, and `SP01/Edge_p.me1`.
3. **Stage 1 (Sprint Acceleration)**: Verifies momentum timer accumulation, top sprint speed (>400 u/s), and dynamic FOV widening (>100°).
4. **Stage 2 (Speed Vault & Springboard)**: Tests hurdle clearance (`MOVE_SpeedVaulting`) and super-jump impulse (`MOVE_SpringBoarding`, `JumpZ = 950 u/s`).
5. **Stage 3 (Wallrun & Camera Tilt)**: Wallruns along the stage-6 billboard across the rooftop gap with the 15° camera Dutch roll (`MOVE_WallRunningLeft`) and lands on the far roof.
6. **Stage 4 (Wallclimb)**: Starts a wall climb up the stage-10 facade and checks the vertical ascent (`MOVE_WallClimbing`).
7. **Stage 5 (Zipline & Crouch Slide)**: Verifies cable attachment, gravitational descent, rooftop landing, and low-clearance crouch slide (`MOVE_ZipLine`, `MOVE_Slide`).
8. **Stage 6 (Mid-Air Coil & Skill Roll)**: Verifies mid-air leg retraction (`MOVE_Coil`, +60 unit boost) and buffered landing momentum retention (`MOVE_SkillRoll`).
9. **Stage 7 (Combat Disarm & Reaction Time)**: Validates enemy disarm QTE (`MOVE_Snatch`), weapon equip (`Colt1911`), and 0.25x reaction time slow-motion dilation.
10. **Stage 8 (Elevator & Level Streaming)**: Rides the real `Escape_p` main lift (`S_Elevator_01` cab and door `InterpActor`s, `PosTrack` Z 10608 → 12288), checks the mid-shaft sublevel streaming and walks out at the top.
11. **Stage 9 (Retail Level & UI Overlay)**: Renders `SP01/Edge_p.me1` with the first-person Faith viewmodel and the interactive Chapter Select menu.
12. **Stage 10 (Tutorial Screenshots)**: Renders six `SP00/Tutorial_p.me1` training-area screenshots.
13. **Stage 11 (Cutscenes)**: Decodes a Bink (`.bik`) movie frame and its audio, and checks the synced subtitle.

Telemetry is exported to `/tmp/me_oracle_telemetry.json` (`%TEMP%` on Windows) and the PNG verification screenshots (`oracle_*.png`, `tutorial_*.png`) are exported to `screenshots/`. The process exits with status 1 if any verified stage (parkour stages 1–8 or the cutscene stage 11) fails.

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
  --game-root <dir>        Set retail game assets directory (default: $MEDGE_ME_INSTALL, else the Steam
                           install on Windows, else /Users/tomnom/mirrorsedge)
  --help, -h               Show help message
```

---

## Replaying Retail Runs

`tools/retail/` records a person playing the retail game on Windows: a `d3d9.dll` proxy reads the pawn's position, velocity, move and view every frame, plus every key. `me_replay` then replays that run, frame for frame, through the `ParkourController` against the same level's collision, and reports where the two paths part. `me_replay` is a headless CMake target that builds on macOS and Windows. See [`tools/retail/README.md`](tools/retail/README.md).

```bash
python -m tools.retail.record_session --name myrun --map escape_p     # Windows, retail installed
cmake --build build --target me_replay
python -m tools.retail.replay --trace build/retail/trials/<stamp>_myrun.jsonl
```

---

## The Front End, Headless

`src/ui/frontend/` is the "Press Any Key" screen, the main menu and the screens its buttons open, reverse engineered from the retail scenes, scripts, materials and menu level ([`docs/MAIN_MENU_SYSTEM_RE.md`](docs/MAIN_MENU_SYSTEM_RE.md), [`docs/SUB_MENUS_RE.md`](docs/SUB_MENUS_RE.md)): a state machine that produces a camera and a 2D draw list, and a CPU reference renderer for it. The app boots into it. `me_menu` runs the same code from a script and writes PNGs, on macOS or Windows, so its frames can be put next to retail's:

```bash
cmake --build build --target me_menu
./build/me_menu --out shots --script "wait 6; shot start.png; key any; wait 46; shot story.png; key right; wait 6; shot race.png"
python -m tools.retail.side_by_side retail.png shots/story.png side_by_side.png
```

![Press Any Key, retail and port](screenshots/menu/press_any_key.png)
![Main menu, retail and port](screenshots/menu/main_menu_story.png)

---

## License & Compliance

Clean-room reimplementation written from first principles for macOS Apple Silicon. Contains no proprietary game binaries, decompiled bytecodes, or extracted copyrighted assets. All game assets remain the property of Electronic Arts and DICE and are read directly from the user's legal game installation.
