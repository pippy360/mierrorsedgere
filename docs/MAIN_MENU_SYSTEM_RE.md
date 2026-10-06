# Mirror's Edge Main Menu & Frontend UI System — Reverse Engineering Report

## 1. Overview & Architecture

In the shipping build of **Mirror's Edge** (DICE / Electronic Arts, Unreal Engine 3 build `5369`), the Main Menu is a hybrid **2D Unreal Engine 3 `UIScene` hierarchy** layered on top of a **live 3D City of Glass background map (`Maps/Menu/TdMainMenu.me1`)**.

When the game boots (`DefaultEngine.ini` -> `LocalMap=TdMainMenu.me1`), Unreal Engine 3 loads `Maps/Menu/TdMainMenu.me1`, whose Kismet sequence opens the `TdMainMenu` UI scene from `UI/TdUI_FrontEnd.upk` and interpolates a 3D `CameraActor` across the City of Glass skyline while highlighting the selected campaign chapter's skyscraper cluster in signature **Runner Vision Scarlet Red (`#E61414`)**.

---

## 2. Reverse-Engineered Game Assets

### 2.1 UI Packages (`TdGame/CookedPC/UI/`)
| Package File | Exports | Role in Frontend UI |
|---|---|---|
| `UI/TdUI_FrontEnd.upk` | 1,735 exports | Contains all frontend `TdUIScene` definitions: `TdMainMenu`, `TdLoadLevel`, `TdLoadCheckpoint`, `TdNewGame`, `TdStart`, `TdRaceMainMenu`, `TdOptionsMain`, `TdExtrasMain`, and `TdUnlocks`. |
| `UI/TdUIResources.upk` | 298 exports | Core UI textures (`Texture2D`), materials, and fonts: `StartTitleImage` (256x64 DXT5 Mirror's Edge logo & star), `Icon_Time` (64x64 DXT5 stopwatch alpha-mask), `Icon_Bag` (64x64 DXT5 runner bag alpha-mask), `Icon_Star` (64x64 DXT5), `GUI_Frame`, `TextBackground`. |
| `UI/TdUIResources_FrontEnd.upk` | 91 exports | Frontend menu stickman/silhouette banners (`MenuStickman_Story`, `MenuStickman_Play`, `MenuStickman_Race`, `MenuStickman_Lead`, `MenuStickman_Opt`, `MenuStickman_Extra`, `MenuStickman_Womans`) and `UI_Background`. |
| `UI/TdUIResources_CheckpointImages.upk` | 92 exports | Skewed parallelogram chapter & checkpoint preview textures (`Texture2D`, 512x256 `PF_DXT5`): `Level0_CP1`..`Level0_CP21`, `Level1b_CP1`..`Level9_CP5`, `Tutorial_CP1`..`Tutorial_CP4`. |

### 2.2 3D Main Menu Map (`TdGame/CookedPC/Maps/Menu/TdMainMenu.me1`)
- **Static Mesh Actors (`StaticMeshActor`)**:
  - `UI_City.S_City_Menu_01`: The entire 3D City of Glass skyline mesh (`16` material sections), featuring distinct material instance slots for each campaign chapter's building cluster:
    - `MI_SP00_01` (Prologue — The Edge)
    - `MI_SP01b_01` (Chapter 1 — Flight)
    - `MI_SP02_01` (Chapter 2 — Jacknife)
    - `MI_SP03_01` (Chapter 3 — Heat)
    - `MI_SP04_01` (Chapter 4 — Ropeburn)
    - `MI_SP05_01` (Chapter 5 — New Eden)
    - `MI_SP06_01` (Chapter 6 — Pirandello Kruger)
    - `MI_SP07_01` (Chapter 7 — The Boat)
    - `MI_SP08_01` (Chapter 8 — Kate)
    - `MI_SP09_01` (Chapter 9 — The Shard)
    - Shared environment materials: `M_CityBuildings_01`, `M_CityGround_01`, `M_CityStreets_01`, `M_CityMountains_01`, `M_Water_01`, `M_Sky_01`.
- **Camera Actors (`CameraActor_2` .. `CameraActor_14`)**:
  - Placed above the 3D City of Glass skyline and driven by `SeqAct_Interp` Matinee tracks when navigating between chapters.
- **Kismet Sequence Events (`SeqEvent_TdUIScene_*`)**:
  - `LoadLevel_Prologue`, `LoadLevel_Escape`, `LoadLevel_Stormdrain`, `LoadLevel_Cranes`, `LoadLevel_Subway`, `LoadLevel_Mall`, `LoadLevel_Factory`, `LoadLevel_Boat`, `LoadLevel_Convoy`, `LoadLevel_Scraper`.
  - Each Kismet event sets a `SeqAct_SetMatInstScalarParam` (`ParamName = "Selected"`, `ScalarValue = 1.0`) on the active chapter's `MI_SP0*_01` `MaterialInstanceActor` while resetting the previously selected chapter's `ScalarValue = 0.0`, illuminating the chosen chapter's skyscraper cluster in Runner Vision Red.

### 2.3 Configuration & Localization Files
- **`TdGame/Config/DefaultUI.ini`**:
  - Defines `[TdGame.TdUIScene_MainMenu]` (`MenuStickman_Story`, `MenuStickman_Race`, `MenuStickman_Opt`, `MenuStickman_Extra`, `MenuStickman_Womans`) and `UIDataStore_TdGameData` (`TdMapInfoDataProvider`, `TdCheckpointDataProvider`).
  - Maps all 11 map packages (`Tutorial_p`, `Edge_p`, `Escape_p`, `Stormdrain_p`, `Cranes_p`, `Subway_p`, `Mall_p`, `Factory_p`, `Boat_p`, `Convoy_p`, `Scraper_p`) to their localized titles, checkpoint IDs, and qualifying speedrun times (`06:00:00`, `11:00:00`, etc.).
- **`TdGame/Localization/INT/TdGame.int` & `TdGameUI.int`**:
  - Provides the English localized strings for all 10 campaign chapter titles (`PROLOGUE - THE EDGE` through `CHAPTER 9 - THE SHARD`), district descriptions (`West Arlington 5.21am`, `New Eden Boilermakers 6.53am`, etc.), and per-chapter checkpoint titles (`Approaching skyscraper`, `Pope's Office`, `Helicopter Chase`, `Crossing the Avenue`, `Centurian Plaza`).

---

## 3. Native macOS Engine Implementation (`src/ui/main_menu.hpp`, `src/ui/main_menu.cpp`, `src/renderer/metal_renderer.mm`)

1. **Direct UPK `Texture2D` Decompression (`PF_DXT1` & `PF_DXT5`)**:
   - `MainMenuSystem::load_upk_texture()` parses the UE3 `Texture2D` export header (`Format`, `SizeX`, `SizeY`, and the `FTexture2DMipMap` bulk data array with LZO `0x9E2A83C1` chunk decompression) directly from `UI/TdUIResources.upk` and `UI/TdUIResources_CheckpointImages.upk` and decodes `PF_DXT1` / `PF_DXT5` blocks into RGBA8 GPU textures.
   - Supports alpha-mask UI icons (`Icon_Time`, `Icon_Bag`, `StartTitleImage`) where the glyph shape is stored in the DXT5 alpha channel (`s.a`) and tinted via `ui_tex_fragment` (`in.color.w < 0.0`).
2. **Live 3D `TdMainMenu.me1` City of Glass Background**:
   - Loads `Maps/Menu/TdMainMenu.me1` into a dedicated `LoadedLevel` (`menu_city_level_`) with `MaterialLibrary` (`menu_city_mat_lib_`).
   - Strips the opaque `M_Sky_01` hemisphere section so the engine's procedural `TdDirHaze.usf` sky dome (`sky_pipeline`) renders behind the skyline.
   - Computes the 3D centroid of each chapter's `MI_SP0*_01` skyscraper cluster so selecting any chapter frames its highlighted Runner Vision Red district in the center 3D viewport between the Left Chapter Selector and Right Chapter Inspector panels.
3. **Interactive & Headless Verification (`--verify-all`)**:
   - Supports keyboard navigation (`UP`/`DOWN` to cycle chapters and interpolate the 3D city camera + Runner Vision district highlight, `LEFT`/`RIGHT` to cycle checkpoints, `TAB`/`ESC` to toggle menu).
   - Verified automatically in Oracle Stage 8 (`screenshots/oracle_7_chapter_select_menu.png`).
