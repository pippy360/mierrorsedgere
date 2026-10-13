# Mirror's Edge Kismet Scripting, Helicopter AI & Gunfire Architecture

This document records the reverse-engineered Unreal Engine 3 (`TdGame.u` + `*_Spt.me1`) Kismet scripting, helicopter vehicle physics (`TdVehicle_Helicopter`), autonomous flight/attack controller (`TdAI_HeliController` / `TdAI_BossHeliController`), door-gunner AI (`TdAI_Gunner` / `TdAI_HeliSniper`), and scripted gunfire setpiece (`SeqAct_TdDummyWeaponFire`) systems extracted from retail PC packages and implemented in `mierrorsedgere`.

---

## 1. Decompiled UnrealScript Class Hierarchy (`TdGame.u`)

Inspection and bytecode disassembly of `TdGame.u` reveals that every helicopter encounter in Mirror's Edge is orchestrated by nine specialized UnrealScript classes working alongside UE3 Kismet (`SequenceEvent` / `SequenceAction`):

| Class | Superclass | Role in Gameplay & Scripting |
| :--- | :--- | :--- |
| **`TdVehicle_Helicopter`** | `Engine.SVehicle` | Physical vehicle actor (`SWAT_Blackhawk.SK_SWAT_Blackhawk_01`), rotor dust emitter (`StartDustEffect()`), multi-seat gunner slots (`Seats`, `AddGunner()`), and flight force/banking constraints (`MaxVelocity = 2000`, `MaxAcceleration = 1000`, `StayUprightPitchResistAngle = 6.0`, `StayUprightRollResistAngle = 6.0`). |
| **`TdAI_HeliController`** | `Engine.AIController` | Autonomous helicopter flight & tactical positioning controller. Evaluates candidate `TdAttackPathNode`s using weighted priority scoring (`GetPrio()` / `FindBestAttackPoint()`) and orients the fuselage (`EHeliAttackSide`) so the door gunner has a clear broadside shot at Faith. |
| **`TdAI_BossHeliController`** | `TdAI_HeliController` | Specialized controller for Chapter 9 (`Scraper_Heli_Spt`) that invalidates attack nodes after Faith closes within `PlayerNearDistance = 3000.0` for `PlayerNearTimeToInvalidate = 10.0s`. |
| **`TdAI_Gunner`** | `TdAIController` | Door-gunner AI controller (`AITemplate_Gunner`, `PawnClass = TdBotPawn_SupportHelicopterGunner`) seated inside `TdVehicle_Helicopter.Seats` and armed with `TdWeapon_Machinegun_FNMinimi` (`CosHalfAttackAngle = 0.68`, `SuppressionTime = 3.0s`). |
| **`TdAI_HeliSniper`** | `TdAIController` | Sniper AI controller (`AITemplate_HeliSniper`, `PawnClass = TdBotPawn_JKSniper`, `SK_TKY_Crim_Jacknife`) used for Jacknife's helicopter pursuit in Chapter 2 & Chapter 9. |
| **`TdAttackPathNode`** | `TdConfinedVolumePathNode` | 3D aerial tactical waypoint storing `AttackVolumeRadius` (`3000.0`), `AttackVolumeHeight` (`2000.0`), `AttackVolumeAngle` (`45.0`), precomputed ground visibility links (`CanSeeMePoints`), and `Exposure`. |
| **`SeqAct_TdHelicopterFactory`** | `SeqAct_ActorFactoryEx` | Kismet latent action spawning a `TdVehicle_Helicopter` at a `Spawn Point` (`TdConfinedVolumePathNode`), boarding AI pawns wired to its `Crew` variable link, and outputting the vehicle reference to `Spawned 1`. |
| **`SeqAct_SetHeliTarget`** | `SequenceAction` | Kismet action binding `Target` (helicopter `SeqVar_Object`) to `AimTarget` (`SeqVar_TdLocalPawn` = Faith) and selecting `SideOfHelicopter` (`ESide_Right`, `ESide_Left`, `ESide_UseLeftWhenHovering`, `ESide_UseRightWhenHovering`, `ESide_Both`, `ESide_None`). |
| **`SeqAct_SetHeliSpeed`** | `SequenceAction` | Kismet action adjusting flight speed (`EHSpeed_Fastest_Default`, `EHSpeed_Fast`, `EHSpeed_Slow`, `EHSpeed_Slower`, `EHSpeed_Slowest`). |
| **`SeqAct_TdDummyWeaponFire`** | `SeqAct_Latent` | Scripted gunfire emitter (`Origin`, `Target`, `WeaponClass`, `ShotsToFire`, `MaxSpread` in UE3 `FRotator` units) used to blast glass office windows and walls along Faith's corridor sprint paths without requiring a full AI line-of-sight trace. |

---

## 2. How Helicopters Spawn, Aim, and Shoot: Two Distinct Patterns

Across all 10 campaign chapters in `/Users/tomnom/mirrorsedge/TdGame/CookedPC/Maps/`, DICE combines two complementary mechanisms:

### Pattern A: Autonomous Helicopter + Door Gunner Crew (`Escape_Plaza_Spt`, `Escape_R1_Spt`, `Cranes_Plaza_Spt`, `Mall_HW_Spt`)

Tracing the Kismet sequence `Main_Sequence.AI_Spawners` (`SequenceFrame_4: "Helicopter Plaza"`) in `SP01/Escape_Plaza_Spt.me1` and `SequenceFrame_21: "Helicopter"` in `SP01/Escape_R1_Spt.me1` reveals the exact execution pipeline when a helicopter shows up:

1. **Entry Trigger (`SeqEvent_TdTouch` / `SeqEvent_RemoteEvent`)**:
   - In `Escape_Plaza_Spt`: touching `Trigger_5` (`[9294] SeqEvent_TdTouch_25`) OR eliminating the plaza SWAT squad (`[9238] SeqEvent_RemoteEvent_4 'SWAT_PlazaPushers_Dead'`, `ActivateDelay = 5.0s`) fires a one-shot `SeqAct_Switch_6` (`[9111]`).
   - In `Escape_R1_Spt`: touching `Trigger_4` (`[10991]`) or firing remote event `Heli_Spawn` (`[10855]`) triggers `SeqAct_Switch_7` (`[10553]`).
2. **Door Gunner Crew Spawn (`SeqAct_TdActorFactory`)**:
   - Before the helicopter spawns, `SeqAct_TdActorFactory` (`TdActorFactoryAI` with `AITemplate_Gunner`) spawns 1 or 2 `TdBotPawn_SupportHelicopterGunner` pawns armed with `TdWeapon_Machinegun_FNMinimi` (`M249 SAW`), writing their actor references into `SeqVar_Object` (`'Heli Crew 1'` / `'Heli Crew 2'`).
3. **Helicopter Spawn & Crew Boarding (`SeqAct_TdHelicopterFactory`)**:
   - `SeqAct_TdHelicopterFactory` spawns `TdVehicle_Helicopter` (`SWAT_Blackhawk.SK_SWAT_Blackhawk_01`) at `TdConfinedVolumePathNode`, reads the `Crew` variable link to seat the gunners via `AddGunner()`, and writes the spawned chopper into `SeqVar_Object` (`'Helicopter'`).
4. **Arrival Windup / Hold-Fire Grace Period (`SeqAct_AIHoldFire` + `SeqAct_Delay`)**:
   - In `Escape_R1_Spt`, immediately upon spawning the two gunners (`[10569]`), Kismet fires `SeqAct_AIHoldFire` (`[10318]`, `[10319]`) so the helicopter roars into view (`SoundCue'A_Vehicle_Helicopter.Helicopter.Helicopter'`) and banks into position for **`Duration = 6.0` seconds** (`[10465] SeqAct_Delay_25`) before releasing `Hold Fire` and opening fire on Faith!
5. **Tactical Positioning (`TdAI_HeliController.FindBestAttackPoint`) & Evasion Rules**:
   - `SeqAct_SetHeliTarget` sets `AimTarget = SeqVar_TdLocalPawn_0` and `SideOfHelicopter = ESide_UseLeftWhenHovering`.
   - `TdAI_HeliController` evaluates candidate `TdAttackPathNode`s using:
     $$\text{Score} = 1.0 \cdot d(\text{Node}, \text{Heli}) + 1.5 \cdot d(\text{Node}, \text{Player}) + 1000.0 \cdot \text{AngleTerm} + 2000.0 \cdot \text{RecentVisitPenalty}$$
   - As Faith sprints through rooftop volumes (`TdTriggerVolume_20`, `TdTriggerVolume_6`, `TdTriggerVolume_7`), Kismet dynamically flips `SideOfHelicopter` between `ESide_Both`, `ESide_UseLeftWhenHovering`, and `ESide_UseRightWhenHovering`.
   - **`SequenceFrame_30 ("Reduce Gunner Accuracy During Slide")`**: the gunner is a bot like any other, with his own `TdAimBot` and `AITemplate_Gunner`'s values (his aim offset starts at 200 uu at every range and a hit counts at 80 or under; it shrinks by 30 / 25 / 15 uu/s near / medium / far at Medium, x 0.55 while she runs across his view, and grows while she runs away). Her moves act on it through their own `AiAimPenalty` (a slide 0.7 / 0.8 / 0.9 of the rate by difficulty, a wall run 0.4 / 0.5 / 0.7); the skill roll and sprinting change nothing by themselves. Kismet's `SeqAct_TdAIPerfectAim` overrides the rate with its `AccuracyImprovementRate`, or, when that is 0, puts the perfect aim bot in his place ([`TDGAME_CLASSES_AND_MOVES_RE.md`](TDGAME_CLASSES_AND_MOVES_RE.md), "Combat between the player and the bots"). The levels' 53 such actions carry: `Edge_Pt2_Spt` 0 and 0.7; `Escape_Off_Spt` 0, seven of 10 and one that writes none (the class's default, 1); `Escape_Plaza_Spt` 0, 0.2, 0, 0.7; `Escape_R1_Spt` 0.4; `Escape_St1_Spt` twelve of 0 and one of 0.5; `Cranes_Off_Spt` six of 100; `Subway_Plat_Spt` two of 0; `Subway_Stat_Spt` 0; `Mall_HW_Spt` five of 0; `Mall_Mall_Spt` 100000, 10000, 10, -10, 50, 100, 0 and one that writes none; `Scraper_Heli_Spt` 1000; `Scraper_Lobby_Spt` 10.
   - **30-Second Anti-Camping Timer (`SeqAct_TdAIPerfectAim`)**: In `Escape_Plaza_Spt`, touching `Trigger_6` (`[9292]`) starts `SeqAct_Delay_0` (`Duration = 30.0s`) wired to `SeqAct_TdAIPerfectAim_2` (`[9129]`). If Faith lingers outside for 30 seconds instead of escaping into the building, the gunner switches to lethal pinpoint accuracy.
6. **Disengage & Cleanup (`TdTriggerVolume_3` / `SeqAct_AIMoveToActor` / `SeqAct_Destroy`)**:
   - Entering the interior exit trigger (`TdTriggerVolume_3` in `Escape_Plaza_Spt`, `Trigger_6` in `Escape_R1_Spt`) fires `SeqAct_AIHoldFire`, fades out `A_Vehicle_Helicopter` (`FadeOutTime = 15s`), flies the chopper along a retreat path (`SeqAct_AIMoveToActor`), and destroys the vehicle and crew (`SeqAct_Destroy`).

---

### Pattern B: Scripted Window-Shattering Barrages (`Escape_Off-R1_Spt`, `Cranes_Off_Spt`, `Boat_Chase_Spt`, `Scraper_Shaft_Spt`)

In tight indoor corridors bordered by floor-to-ceiling glass (most famously Chapter 1 `Escape_Off-R1_Spt.me1`, which contains **29 `SeqAct_TdDummyWeaponFire` nodes**), DICE does not rely on live AI line-of-sight rays through unbroken glass:
- Each `SeqAct_TdDummyWeaponFire` links an exterior `Origin` actor (at the hovering helicopter's position) to an interior `Target` marker inside the glass corridor (`ShotsToFire = 18`, `WeaponClass = TdWeapon_AssaultRifle_FNSCARL`, `MaxSpread = Rotator(4551, 4551, 4551)` ≈ `±25°`, `ActivateDelay = 0.3s`).
- As Faith crosses sequential corridor triggers, Kismet fires each `SeqAct_TdDummyWeaponFire` ahead of her sprint path, shattering the glass panels in sequence and filling the hallway with tracers and glass shards (`PS_FX_Impact_Glass_Heli_01`).

---

## 3. Campaign Helicopter Encounter Inventory

| Chapter | Script Package (`.me1`) | Helicopter Spawners (`SeqAct_TdHelicopterFactory`) | `TdAttackPathNode` Count | `SeqAct_TdDummyWeaponFire` Nodes | Notes |
| :--- | :--- | :---: | :---: | :---: | :--- |
| **Prologue: The Edge** | `SP01/Edge_Pt2_Spt.me1` | 1 | 1 (`Edge_Pt1_Spt`) | 0 | Pursuit helicopter at finale rooftop jump (`SK_VH_Helicopter_01`). |
| **Ch 1: Flight** | `SP01/Escape_Off-R1_Spt.me1` | 0 (Matinee) | 0 | **29** | Iconic office window barrage (`18` rounds per window panel). |
| **Ch 1: Flight** | `SP01/Escape_R1_Spt.me1` | **3** | **9** | **35** | Rooftop chase with 6.0s hold-fire windup, slide evasion & fly-away. |
| **Ch 1: Flight** | `SP01/Escape_Plaza_Spt.me1` | **1** | **5** | 0 | Plaza ambush (`SWAT_Blackhawk`) with 30s anti-camping `PerfectAim`. |
| **Ch 2: Jacknife** | `SP02/Stormdrain_Std_Spt.me1` & `Stormdrain_All_SPT.me1` | **3** | **16** | **2** | Canal & gate pursuit choppers (`A_VO_AI_Chopper_Halt_4_Cue`). |
| **Ch 3: Heat** | `SP03/Cranes_Off_Spt.me1`, `Cranes_Plaza_Spt.me1`, `Cranes_Roof_Spt.me1` | **6** | **31** | **15** | Multi-chopper crane & rooftop pursuit sequences. |
| **Ch 5: New Eden** | `SP05/Mall_HW_Spt.me1` | **1** | **7** | **1** | Highway rooftop pursuit prior to mall entry. |
| **Ch 6: Pirandello Kruger** | `SP06/Factory_Pursu_Spt.me1` | 0 | 0 | **3** | Scripted pursuit gunfire volleys. |
| **Ch 7: The Boat** | `SP07/Boat_Chase_Spt.me1` | 0 | 0 | **6** | Car-deck chase gunfire barrages. |
| **Ch 9: The Shard** | `SP09/Scraper_Heli_Spt.me1` & `Scraper_Shaft_Spt.me1` | **1** (`BossHeli`) | **4** | **11** | Server hall / rooftop helipad finale & Jacknife helicopter showdown. |

---

## 4. Native C++ Implementation in `mierrorsedgere`

- **[`src/math/types.hpp`](../src/math/types.hpp)**:
  - Defines `EHeliAttackSide`, `EHeliSpeed`, `EHeliState`, `HeliAttackNode`, `DummyFireBarrage`, and `HelicopterInstance` on `LevelScene`.
- **[`src/assets/upk_loader.cpp`](../src/assets/upk_loader.cpp)**:
  - `UPKPackage::extract_helicopter_encounters()` extracts all cooked `TdAttackPathNode`, `SeqAct_TdHelicopterFactory` (resolving `Spawn Point`, `Crew` count, upstream `SeqEvent_TdTouch` triggers, `SeqAct_SetHeliTarget` broadside setting, and `SeqAct_Delay` hold-fire duration), and `SeqAct_TdDummyWeaponFire` barrages during `load_level_scene()`.
- **[`src/anim/anim_system.cpp`](../src/anim/anim_system.cpp)**:
  - Loads `Vehicles/SWAT_Blackhawk.upk` (`SK_SWAT_Blackhawk_01` + `T_blackhawk_outside_D`) and skins each active `HelicopterInstance` in `evaluate_combat_world_fx()` with live main rotor (`VH_Extra1`/`VH_Extra4` at 18 rad/s) and tail rotor (`VH_Extra2` at 42 rad/s) spin, fuselage banking (`pitch_deg`, `roll_deg`, `yaw_deg`), and door-gunner muzzle flash bursts.
- **[`src/physics/parkour_controller.cpp`](../src/physics/parkour_controller.cpp)**:
  - `ParkourController::update_ai_bots()` simulates the `TdAI_HeliController` state machine (`Dormant -> Arriving -> Engaging -> Retreating`), `TdAttackPathNode` priority scoring, broadside hover orientation (`EHeliAttackSide`), and `SeqAct_TdDummyWeaponFire` window barrages. The door gunner fires as a bot does (`src/physics/bot_fire.hpp`: `AITemplate_Gunner`'s aim bot, the FN Minimi's bursts of 15 to 30 shots, `HelicopterInstance::gunner`), from the gun's place in the door, while the helicopter engages, she is in the broadside arc and within 6500 uu (those three conditions are the port's own); after the delay that stands for the level's `SeqAct_TdAIPerfectAim` he has the perfect aim bot. The actions themselves are not run from the level's script (`TODO.md`).
  - The player's bullet stops on the one box of the helicopter's placeholder physics asset (`ParkourController::trace_helicopters`, `helicopter_box`): 487.4 x 202.9 x 137.5 about (0, 0, 434) over its `Location`, a slab round the rotor mast; the default material's impact effect and sound, no hole, and nothing is taken from the helicopter (`TdVehicle_Helicopter` has 300 health and no `TakeDamage` of its own; what its death would be was not found). `ME_SHOW_BODIES=1` draws the box; `ME_SHOT_HELI` hangs a helicopter in front of an `--intro-shots` view.
