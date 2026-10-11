# Mirror's Edge Level Loading, Multi-Level Streaming & Interactive Elevator Architecture

This document records the reverse-engineered Unreal Engine 3 (`TdGame`) level loading, checkpoint streaming, and interactive elevator transition architecture extracted directly from the retail PC packages (`/Users/tomnom/mirrorsedge/TdGame/CookedPC/Maps/`) and implemented natively in `mirrorsedge_macos`.

---

## 1. Master Persistent Maps (`*_p.me1`) & Sub-Level Decomposition

Every campaign chapter in Mirror's Edge is anchored by a lightweight persistent master map (`*_p.me1`) that contains no heavy rooftop geometry itself, but instead orchestrates dozens of streaming sub-packages:

| Chapter | Persistent Master Package | Sub-Package Naming Convention | Total `LevelStreamingKismet` Entries | `TdCheckpoint` Count |
| :--- | :--- | :--- | :---: | :---: |
| **Tutorial** | `SP00/Tutorial_p.me1` | `Tutorial_Art`, `Tutorial_Spt`, `Tutorial_lgts` | 7 | 160 |
| **Prologue: The Edge** | `SP01/Edge_p.me1` | `Edge_Pt1`, `Edge_Pt1-Pt2_Slc`, `Edge_Pt2` | 23 | 9 |
| **Ch 1: Flight** | `SP01/Escape_p.me1` | `Escape_Intro`, `Escape_Intro-Off_Slc`, `Escape_Off`, `Escape_R1`, `Escape_St1`, `Escape_Plaza` | 53 | 12 |
| **Ch 2: Jacknife** | `SP02/Jacknife_p.me1` | `Stormdrain_All_p`, `Stormdrain_StdP-*_Slc`, `Stormdrain_Gate`, `Stormdrain_boss` | 48 | 17 |
| **Ch 3: Heat** | `SP03/Heat_p.me1` | `Heat_Off`, `Heat_Off-R1_Slc`, `Heat_R1`, `Heat_R1-R2_Slc`, `Heat_R2`, `Heat_R3`, `Heat_Cr` | 44 | 15 |
| **Ch 4: Ropeburn** | `SP04/Ropeburn_p.me1` | `Ropeburn_Off`, `Ropeburn_Dis`, `Ropeburn_Subway`, `Ropeburn_ Mall` | 64 | 21 |
| **Ch 5: New Eden** | `SP05/New_Eden_p.me1` | `New_Eden_R1`, `New_Eden_R1-R2_Slc`, `New_Eden_R2`, `New_Eden_Mall` | 36 | 11 |
| **Ch 6: Pirandello Kruger** | `SP06/Factory_p.me1` | `Factory_R1`, `Factory_Prso`, `Factory_Arena`, `Factory_Train` | 49 | 16 |
| **Ch 7: The Boat** | `SP07/Boat_p.me1` | `Boat_Deck`, `Boat_Car`, `Boat_Ind`, `Boat_Chase` | 42 | 14 |
| **Ch 8: Kate** | `SP08/Convoy_p.me1` | `Convoy_Snipe`, `Convoy_Atr`, `Convoy_Chase` | 38 | 12 |
| **Ch 9: The Shard** | `SP09/Scraper_p.me1` | `Scraper_Deck`, `Scraper_Lobby`, `Scraper_Vent`, `Scraper_Svr`, `Scraper_Svr-Roof_Slc`, `Scraper_Roof`, `Scraper_Heli` | 55 | 18 |

Each zone is split into specialized sub-package suffixes:
- `<Zone>.me1` — Structural collision and whitebox BSP/static meshes
- `<Zone>_Art.me1` — Detail static meshes, props, signage, HVAC, and glass
- `<Zone>_Bac.me1` / `<Zone>_LW.me1` — Distant city backdrop buildings and low-LOD skyline
- `<Zone>_Lgts.me1` — Precomputed directional/skylight actors and lightmaps
- `<Zone>_Spt.me1` — Kismet scripts, AI squads, Matinee (`SeqAct_Interp`) timelines, and interactive elevator actors
- `<ZoneA>-<ZoneB>_Slc.me1` & `<ZoneA>-<ZoneB>_Spt.me1` — **Elevator / Airlock Transition Slices** bridging two major zones

---

## 2. Why `LevelStreamingVolume` Is Disabled & How Streaming Actually Works

Inspection of all `LevelStreamingVolume` exports across every `.me1` package in `/Users/tomnom/mirrorsedge/TdGame/CookedPC/Maps/` revealed a critical architectural fact:

> **100% of `LevelStreamingVolume` objects in retail Mirror's Edge have `bDisabled = True`.**

Instead of volume overlap streaming, DICE implemented deterministic two-tier level streaming:

1. **`TdCheckpoint.StreamingLevels` (Checkpoint-Driven Streaming)**:
   - Every `TdCheckpoint` actor in the persistent level or `*_Spt.me1` stores:
     - `CheckpointName` (`FString`)
     - `CheckpointWeight` (`IntProperty`, monotonically increasing along the chapter route)
     - `DefaultCheckpoint` (`BoolProperty`, marks the initial chapter spawn checkpoint)
     - `StreamingLevels` (`ArrayProperty` of object references to `LevelStreamingKismet` exports in the persistent level)
   - Each `LevelStreamingKismet` export holds `PackageName` (`NameProperty`), naming the `.me1` sub-package to keep resident when that checkpoint is active.

2. **`SeqAct_MultiLevelStreaming` (Mid-Elevator Kismet Streaming)**:
   - Inside persistent maps and `*_Slc.me1` / `*_Spt.me1` elevator slices, Kismet `SeqAct_MultiLevelStreaming` nodes hold an `ArrayProperty` `Levels` of `LevelStreamingKismet` references.
   - While Faith is sealed inside a moving elevator cab (`*_Slc.me1`), Kismet unloads the previous zone's sub-packages (`StreamOut`) and loads the destination zone's sub-packages (`StreamIn`) before the upper/lower elevator doors slide open.

---

## 3. Interactive Elevator Architecture (`InterpActor` + `SeqAct_Interp` + `InterpTrackMove`)

Across all campaign chapters, elevators are constructed from four synchronized Unreal Engine 3 objects inside `*_Spt.me1` and `*_Slc.me1` packages:

1. **Elevator Cab (`InterpActor`)**:
   - Uses `StaticMeshComponent` -> `StaticMesh` (or actor `ReplicatedMesh`) resolved across multi-level archetype chains to `P_Elevator.S_Elevator_01` or `P_SP09_Scraper_Gameplay.S_SP09_ElevatorWithTop_01`.
   - **Local Pivot / Bounds Offset**: `S_Elevator_01` has local bounds `Origin = (-120.0, -132.5, 131.0)` and `Extent = (120.0, 132.5, 131.0)`. Its pivot is at one bottom corner rather than the center of the cab floor. Rotating `(Origin.x, Origin.y, 0)` by the cab actor's `Rotation.Yaw` yields `cab_local_offset`, aligning the 240x265x262 interior volume with the paired doors and control panel.
2. **Sliding Doors (`InterpActor` -> `P_Elevator.S_ElevatorDoor_01`)**:
   - Paired left/right door leaves at both the entry landing (`door_open_Start`) and destination landing (`door_open_End`), each sliding 76 Unreal units laterally when opening/closing.
3. **Interactive Call / Floor Button (`InterpActor` -> `P_Elevator.S_ElevatorButton_Single`)**:
   - Highlighted in Runner Vision red; triggered by proximity or pressing Use (`E`).
4. **Matinee Trajectory (`SeqAct_Interp` -> `InterpGroup` -> `InterpTrackMove`)**:
   - `SeqAct_Interp` connects via `VariableLinks` (`SeqVar_Object.ObjValue`) to the target `InterpActor` cab (e.g., `mainlift`, `gate_elevator`, `Elevator_Cab`).
   - Its `InterpData` -> `InterpGroup` -> `InterpTrackMove` stores `PosTrack` (`InterpCurveVector` keyframes of `InVal` time in seconds and `OutVal` 3D vector coordinates) and `MoveFrame` (`IMF_World` or `IMF_RelativeToInitial`). The enum is in that order and the class's defaults leave it at 0, so a track that saves no `MoveFrame` is in the world's frame: three lifts are (`Factory_Arena_Spt:Area_elevator`, `Scraper_Lobby-Shaft_Spt:ElevatorShaft` and `ElevatorHatch`), and read as relative they ran off to twice their coordinates.
   - Verified real `.me1` examples extracted by `UPKPackage::extract_elevators()`:
     - `Escape_Intro-Off_Spt:mainlift` (`S_Elevator_01`): `Z = 10608 -> 12288`, `Duration = 5.0s`
     - `Escape_Off-R1_Slc:InterpActor_2` (`S_Elevator_01`): `Z = 12288 -> 5500`, `Duration = 35.0s`
     - `Escape_R1-St1_Spt:InterpActor_9` (`S_Elevator_01`): `Z = 4236 -> 1268`, `Duration = 20.0s`
     - `Escape_Plaza_Spt:InterpActor_2` (`S_Elevator_01`): `Z = 1320 -> 5096`, `Duration = 28.18s`
     - `Stormdrain_All_p:gate_elevator` (`S_Stock_Elevator_01`): `Z = 3240 -> 960`, `Duration = 12.0s`
     - `Scraper_Svr-Roof_Slc:InterpActor_0` (`S_SP09_ElevatorWithTop_01`): `Z = 21666 -> 27774`, `Duration = 30.0s`

---

## 4. Native Engine Implementation Summary

- **[`src/assets/upk_loader.hpp`](../src/assets/upk_loader.hpp) & [`src/assets/upk_loader.cpp`](../src/assets/upk_loader.cpp)**:
  - `UPKPackage::extract_level_streaming_and_checkpoints()` parses all `LevelStreamingKismet`, `TdCheckpoint`, and `SeqAct_MultiLevelStreaming` exports.
  - `UPKPackage::extract_elevators()` traces `SeqAct_Interp` -> `VariableLinks` -> `SeqVar_Object` -> `InterpActor` and `InterpGroup` -> `InterpTrackMove` (`PosTrack` keyframes + `MoveFrame`), computing `cab_local_offset` and `cab_half_extents` from the resolved `UStaticMesh` bounds.
  - `load_level_scene()` loads `*_Slc.me1` and `*_Spt.me1` packages alongside geometry/art packages, extracts all checkpoints and elevators, and links each elevator to its nearest start/end `TdCheckpoint` streaming lists.
  - `assign_elevator_parts()` claims the real moving `InterpActor`s of each elevator as `ElevatorPart`s: the cab (`Cab`), actors based on it (`CabDoor` when a door matinee drives them, otherwise `CabAttached`), and the landing door leaves inside the cab footprint at the start/destination floor (`StartDoor` / `EndDoor`, each with its matinee `door_open_offset`). Duplicate lifts cooked into both `*_Slc` and `*_Spt` are merged.
  - `build_elevator_part_geometry()` gives every part its own render buffer (`MeshBuffer::elevator` / `elevator_part`, vertices at the initial pose) and its own `CollisionWorld`, and poses it for the initial `IdleStart` state (start-floor doors open). Moving parts are excluded from the static level geometry and collision.
  - `stream_level_to_checkpoint()` switches the active `TdCheckpoint` and updates `scene.loaded_sublevel_packages`.
- **[`src/physics/parkour_controller.cpp`](../src/physics/parkour_controller.cpp)**:
  - `sweep_capsule()` and `trace_ray()` also test every elevator part's real collision, shifted by the part's current `offset`.
  - `ParkourController::update_elevators()` drives `IdleStart -> DoorsClosing -> Moving -> DoorsOpening -> IdleEnd` at 120Hz substep resolution. It poses each part (cab along the `PosTrack`, door leaves along their open offsets), carries Faith with the part she stands on (UE3 `Pawn.Base`), triggers mid-shaft sublevel streaming (`50%` progress), and opens the destination doors on arrival.
- **[`src/renderer/metal_renderer.mm`](../src/renderer/metal_renderer.mm)**:
  - Draws (and shadows) each part's real mesh (`S_Elevator_01`, `S_ElevatorDoor_01`, ...) with a translation model matrix of its current `offset`, plus HUD telemetry (`STREAMED SUBLEVELS` and `ELEVATOR TRANSIT / STREAMING` progress bar).
