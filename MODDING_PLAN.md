# Mirror's Edge Modding & Clean-Room macOS Runtime Plan

## Overview & Fingerprinting
- **Install**: `/Users/tomnom/mirrorsedge` (Steam AppID: `17410`, manifest install script `17410_install.vdf`), retail version `1.01` (Steam shipping PC build date: `2009-01-08 13:30:25 UTC`).
- **Engine**: Unreal Engine 3 (custom EA DICE 2008/2009 branch, build ~5137/5354 equivalent).
  - Code: Native x86 32-bit (`IMAGE_FILE_MACHINE_I386` = `0x014c`). Subsystem: Windows GUI (`2`).
  - Sections (6): `.text` (23.6 MB, code), `.rdata` (5.0 MB, read-only data & RTTI), `.data` (282 KB, writable globals), `.rsrc` (614 KB, Windows resources/icons), `.reloc` (2.0 MB, base relocations), `.bind` (344 KB, SecuROM PE envelope).
  - Export: `TdGame-ShippingPC.exe` exporting `SecuROM` (single entry).
- **Anti-cheat / Online**:
  - Anti-cheat: None found.
  - DRM: Original binary had a SecuROM runtime binding envelope (`.bind` section, high entropy 7.99) and CD-key check routines (`intUTdEADMPatcherWrapperexecGetCDKey`), but offline singleplayer and local play require no active online services.
  - Online servers: EA online time trial / leaderboard servers and messenger backend (`DefaultPresence.ini`, `Tp.TpSystem`) are officially offline.
- **Saves, Config, & Logs**:
  - Windows paths:
    - Saves / Profiles: `%USERPROFILE%\Documents\EA Games\Mirror's Edge\TdGame\Save\` (`TdProfile`, `*.dat`, `*.pro`, recorded ghosts)
    - Config: `%USERPROFILE%\Documents\EA Games\Mirror's Edge\TdGame\Config\` (`TdEngine.ini`, `TdGame.ini`, `TdInput.ini`, `TdUI.ini`)
    - Stock Config Defaults: `/Users/tomnom/mirrorsedge/TdGame/Config/` (23 `.ini` files)
    - Logs: `%USERPROFILE%\Documents\EA Games\Mirror's Edge\TdGame\Logs\Launch.log`
  - macOS Clean-Room Runtime (`mierrorsedgere`) paths:
    - Saves: `~/Library/Application Support/mierrorsedgere/Save/`
    - Config: `~/Library/Application Support/mierrorsedgere/Config/`
    - Logs: `~/Library/Logs/mierrorsedgere/launch.log`

---

## Binary & Library Dependencies (Binaries/ Analysis)
The retail PC release uses 38 dynamic link libraries. Key runtime dependencies identified:
1. **Physics Engine (`PhysXCore.dll` & `NxCharacter.dll`)**:
   - AGEIA / NVIDIA PhysX SDK 2.8.1 (2.8.1.18).
   - `NxCharacter.dll`: PhysX Character Controller (`NxControllerManager`, capsule collision, sweep tests).
   - `NxCooking.dll`: Mesh cooking for rigid body and cloth physics.
   - `PhysXExtensions.dll` / `PhysXDevice.dll`: Hardware PhysX acceleration support.
2. **Video Playback (`binkw32.dll`)**:
   - RAD Game Tools Bink Video 1.8x (`_BinkOpenDirectSound@4`, `_BinkNextFrame@4`, `_BinkCopyToBuffer@28`).
   - Powers all animated cutscenes in `TdGame/Movies/*.bik` (intro, animated story cutscenes CS01-CS16).
3. **Audio Subsystem (`OpenAL32.dll`, `wrap_oal.dll`, `vorbis*.dll`, `ogg.dll`)**:
   - OpenAL 1.1 3D spatialized sound rendering.
   - Xiph.Org Ogg Vorbis audio decoding (`vorbis.dll`, `vorbisfile.dll`, `vorbisenc.dll`, `ogg.dll`) for music and dialogue streams.
   - `libresample.dll`: Audio sample rate conversion (e.g. 22kHz/44.1kHz to 48kHz).
4. **Editor & Tooling Framework (`wxmsw28u*.dll`)**:
   - wxWidgets 2.8.x Unicode for UnrealEd toolbars, dialogs, and browser windows baked into the shipping binary (`wxmsw28u_core_vc_custom.dll`, `wxmsw28u_adv_vc_custom.dll`, `wxmsw28u_aui_vc_custom.dll`, `wxmsw28u_html_vc_custom.dll`, etc.).
5. **Illumination & Point Cloud (`ILPointCloudLib.dll`)**:
   - DICE proprietary point cloud illumination library for global illumination / radiosity normal mapping baking (Beast integration).
6. **Graphics (`d3d9.dll`, `d3dx9_35.dll`, `d3dx10_35.dll`)**:
   - Direct3D 9.0c with custom HLSL/USF post-processing passes.

---

## Community Modding Landscape
- **Loaders & Mod Tools**:
  - `ME-Mod-Loader` / `Mirror's Edge Mod Loader` (GitHub / NexusMods): Native DLL injection hook using `d3d9.dll` or `dinput8.dll` proxying to load custom `.u` packages, replace UPK packages, and enable console commands (`~` key / `ToggleDebugCamera`, `Slomo`, `Ghost`).
  - `UPK Tool` / `UnrealPak` / `UModel` (Gildor's UE Viewer): Decompresses and extracts Mirror's Edge cooked UPK packages (`.upk`), extracting static meshes, textures, sound cues, and skeletal animations.
  - `ME-Custom-Map-Loader`: Replaces map entries in `DefaultGame.ini` (`[SP01a UIDataProvider_TdMaps]`) to boot user-made time trial and speedrun test chambers.
- **Example Hello World Mod**:
  - Modifying `DefaultGame.ini` or injecting custom console commands (`Bindings=(Name="F1",Command="god | slomo 0.5")`).
  - Replacing material parameters in `DefaultLOI.ini` (`FadeInSpeed`, `FadeOutSpeed`) to alter Runner Vision reactivity.

---

## Chosen Route: Clean-Room macOS Playable Runtime (`mierrorsedgere`)
- **Objective**: Develop a modern, 64-bit native macOS (Apple Silicon ARM64 & Intel x86_64) clean-room game engine runtime capable of directly mounting and playing Mirror's Edge retail assets (`~/mirrorsedge`).
- **Why Reimplementation over Emulation/Wine**:
  - 32-bit x86 executables cannot run natively on macOS Catalina and later (macOS 10.15+ dropped 32-bit support entirely).
  - Clean-room reimplementation provides native Metal graphics pipeline support, high DPI Retina rendering, Apple Silicon NEON vector acceleration, modern game controller support, zero Wine/Rosetta translation overhead, and direct moddability.
- **Architecture**:
  1. **Core Runtime**: Native modern C++20 / Rust executable.
  2. **Package & Asset Deserializer**: Clean-room reader for UE3 CookedPC packages (`.upk` in `TdGame/CookedPC/`), reading header tables, name tables, export tables, import tables, `FStaticMesh`, `USkeletalMesh`, `UTexture2D` (DXT1/DXT5 decompression to Metal textures), and sound node waves.
  3. **Graphics Engine**: Metal 3 backend translating UE3 shaders:
     - Radiosity Normal Mapping (Beast baked lighting coefficients).
     - Custom DICE post-processing passes ported to Metal Shading Language: `TdDirHaze` (atmospheric sun scattering), `TdMotionBlur` (radial speed blur), `TdToneMapping` (vivid exposure & color curves), and `TdUIBlur` (fake alpha UI blending).
     - Runner Vision (`LOI`) material system applying high-contrast red highlights on interaction targets.
  4. **Physics & Parkour Movement Engine**:
     - Custom kinematic pawn controller implementing the 55 movement states discovered in `DefaultPawnMovement.ini` (`TdMove_Jump`, `TdMove_WallRun`, `TdMove_WallClimb`, `TdMove_SpeedVault`, `TdMove_Slide`, `TdMove_Coil`, `TdMove_ZipLine`, `TdMove_Balance`, etc.).
     - PhysX 4/5 or Jolt Physics collision query backend replacing deprecated 32-bit PhysX 2.8.1.
  5. **Audio Engine**: OpenAL Soft or CoreAudio engine playing Vorbis-decoded streams and positional sound cues.
  6. **Cutscene Player**: FFmpeg / libbinkdec decoding `TdGame/Movies/*.bik` videos to Metal textures.

---

## Lab Setup & Safety Checklist
- **Asset Integrity**:
  - Game assets in `/Users/tomnom/mirrorsedge` are strictly READ-ONLY.
  - NEVER commit or copy proprietary binaries (`.exe`, `.dll`) or proprietary game assets (`.upk`, `.bik`, `.me1`) into `git/mierrorsedgere`.
  - `.gitignore` configured to reject all binaries and asset extensions.
- **Config & Movement Verification Oracle**:
  - Reusable parser `tools/ini_and_loc_parser.py` parses all 23 INI configs and 46 INT files directly from the game install into an in-memory JSON specification oracle.
- **Windowed & Developer Mode**:
  - Clean-room runtime initializes in resizable windowed mode by default (`DefaultEngine.ini: StartupFullscreen=False`), supporting Hot-Reloading of shaders and gameplay scripts.

---

## Unknowns & Validation Sequence
1. [x] **PE Binary Header & RTTI Inspection**: Extracted PE sections, imports, 356 RTTI classes, and 300+ native `int[AU]Td*exec*` UnrealScript execution thunks.
2. [x] **Custom DICE Shaders**: Reverse-engineered exact mathematical equations for Tone Mapping, Exposure, Directional Haze, Radial Motion Blur, and UI Fake-Alpha compositing.
3. [x] **Movement & Game Specs**: Mapped all 55 pawn movement states, weapons, and 10 singleplayer campaign chapters + 23 time trial stretches.
4. [ ] **Cooked UPK File Format & Compression**: Validate LZO / zlib chunk decompression for `TdGame/CookedPC/*.upk` packages.
5. [ ] **Skeletal Animation Graph**: Map UE3 `UTdAnimNodeSequence` and `UTdAnimNodeState` blending nodes to pawn movement states.
