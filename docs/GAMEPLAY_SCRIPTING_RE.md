# Gameplay scripting: cutscene triggers, level transitions, on-screen text

What the retail game does between the parkour: which Kismet runs a chapter, how a cutscene is
started and skipped, how a chapter ends and the next begins, and what text it puts on the screen.
Read out of the cooked maps (`TdGame/CookedPC/Maps/SP00..SP09`), the decompiled `TdGame.u` and
`Engine.u`, the config and localization files, and one retail recording. Then how the port runs
the same graphs, and where it still differs.

[KISMET_SEQUENCE_GRAPHS_RE.md](KISMET_SEQUENCE_GRAPHS_RE.md) is the census of the nodes;
[LEVEL_INTROS.md](LEVEL_INTROS.md) the chapter openings' Matinees. This page is the runtime.

## 1. What runs a chapter

Every map, persistent (`*_p.me1`) and streamed (`*_Spt`, `*_Slc`, `*_CS`, `*_Aud`, `*_Mus`), has a
`Main_Sequence`. The engine ticks each loaded level's sequence in turn (`USequence::UpdateOp`):
ops are taken off the end of the active list, and what an op activates is pushed, so a chain runs
depth first, in link order, within one frame. An event that fires (`USequenceEvent::CheckActivate`)
is put at the *front*, so it runs after the ops already ticking. A link can carry a delay on
either end (`ActivateDelay` on the output and on the input) and can be `bDisabled`. Latent ops
(`SeqAct_Delay`, `SeqAct_Interp`, `SeqAct_TdPlaySound`, `SeqAct_TdFadeEffect`,
`SeqAct_TdTutorialMessage`) stay on the list until done.

Levels talk to each other through `SeqAct_ActivateRemoteEvent` / `SeqEvent_RemoteEvent` by name
(`IntroCS`, `Finish_Level`, `Office_CS_Finished`): the persistent map holds the checkpoints and
the streaming, the `_Spt` levels the encounters, the `_CS` levels the cutscenes.

### Where the level starts

`TdSPStoryGame.TriggerEventsOnLevelReload` (bytecode: iterates `SeqEvent_LevelLoaded`, then
`WorldInfo.GeneratedEvents` of class `SeqEvt_TdCheckpointLoaded` and `SeqEvt_TdCheckpointActivated`):
on a load, every loaded level's `SeqEvent_LevelLoaded` fires, then the active checkpoint's
`SeqEvt_TdCheckpointLoaded` and `SeqEvt_TdCheckpointActivated`. The pawn spawns at the checkpoint
(`FindPlayerStart` through `CheckpointManager`). Dying reloads the script levels
(`ResetLevel` with `bReloadScriptLevels`) and fires the same events again, so a chapter's Kismet
state starts over from the checkpoint.

**Where a restart puts her.** `TdSPStoryGame.RestartPlayer` spawns the pawn at the start spot
(`SpawnDefaultPawnFor(NewPlayer, StartSpot)`: `StartSpot.Location`), and
`TdCheckpoint.HandlePawnTeleport` is `Pawn.SetLocation(Location)`, `SetPhysics(PHYS_Falling)`. An
actor's `Location` is its cylinder's centre: her capsule's centre goes to the checkpoint's, and she
falls from there. A `TdCheckpoint`'s own cylinder is 96 high (`CollisionHeight=96` on all 184 of
the campaign) and hers is 90, so over a checkpoint that stands on its floor her feet start 6 over
that floor; most do, some are placed up to a few dozen units higher, and a few stand in a lift's
cab or over a drop that is meant (the Prologue's `Cops` is the top of the shaft she comes down).
The port (`ParkourController::restart_at`) puts her on the floor under the spot when there is one
within 60 of where her feet would be, and otherwise lets her fall from the spot. Until 2026-10-11
it took the checkpoint's place plus 35 for her feet, 125 too high: her head stood in any ceiling
lower than 311 over the floor. A restart of the level (`restart_level_at`: a death, the R key, a
chapter begun at a checkpoint) also puts the lifts back, which are the script levels': each where
the level has it, but one whose cab at the far end of its run is round the spot (Heat's
`Pursuit_chase`, The Shard's `Elevator_shaft`), which waits there. `--verify-respawn` restarts at
every checkpoint of every chapter; the oracle's stage 25 at the Training Area's and the
Prologue's.

A `TdCheckpoint` carries `CheckpointName`, `DefaultCheckpoint` and `StreamingLevels`, the
sublevels resident while it is active. All 183 `SeqEvt_TdCheckpointLoaded` events of the campaign
are in the persistent maps, next to their checkpoints.

### Checkpoints are set by Kismet, not by walking up to them

`SeqAct_TdCheckpoint` (`teleportPawnToCheckpoint`, `skipSaveToDisk`) sets the active checkpoint.
Its `In` is wired from trigger volumes (`SeqEvent_TdTouch` on a `TdTriggerVolume` or `Trigger`),
from remote events (a cutscene's end, a lift arriving) and from `SeqEvt_TdCheckpointLoaded`
itself (the start checkpoint re-set on load). The checkpoint actor's own position is where the
player respawns, not what is touched: in Flight, `TdCheckpoint_9` (4281, 7487, 12768) is set by
`Office_Checkpoint_Event`, fired at the end of the office cutscene. Setting one fires its
`SeqEvt_TdCheckpointActivated` events (`TdCheckpoint.OnTdCheckpoint`): The Shard's intro hangs on
the activation of `TdCheckpoint_2`, not on its load.

## 2. Cutscenes

A player cutscene is a `SeqAct_Interp` with `SeqVar_TdLocalPawn` and a placed
`SkeletalMeshActor(MAT)` in one group (the intro is one; 36 across the campaign). The event keys
of its Matinee fire the action's outputs of the same name; what hangs on them is the rest of the
scene: the teleport onto the stand-in (`SeqAct_Teleport` + `SeqAct_AttachToActor` on `go` /
`AttachPlayer` / `tele`), voice lines (`SeqAct_TdPlaySound`), fades, slow motion
(`SeqAct_TdSetTdTimeDilation`), the credits lettering (`SeqAct_ToggleHidden` on `InterpActor`s),
and at the end a checkpoint and `SeqAct_TdEnablePlayerInput`.

How one is started, from the data:

| Cutscene | Started by | Chain |
|---|---|---|
| Edge intro `sp01_intro` | load at `Edge_Start` | `SeqEvt_TdCheckpointLoaded_15` -> `TdDisablePlayerInput` -> remote `IntroCS` -> `Edge_Pt1_CS` Interp |
| Edge bag hand-off `sp01_baghandoff_faith` | `Trigger_3` (-21700, 4558, 7404) r 87 | `SeqEvent_Touch_5` -> `TdDisablePlayerInput` -> `TdIntoCutscene` -> Interp (17.8 s) |
| Edge helicopter `sp01_helicopter_hang` | `Trigger_16` r 174, or `Trigger_8` r 1781 -> 0.1 s -> the jumpable Matinee | gate -> Interp (30 s), `FadeToWhite` -> 2 s -> remote `Finish_Level` |
| Flight intro `sp01_intro_b` | load at `Start` | `SeqEvt_TdCheckpointLoaded_17` -> remote `IntroCS` -> `Escape_Intro_Spt` Interp |
| Flight office `cs3_r1_faith` (130 s) | `TdTriggerVolume_11` (5888, 5696, 12416), 16 x 256 x 256 | `SeqEvent_Touch_19` -> `TdIntoCutscene` -> `TdEnablePlayerInput` -> `TdDisablePlayerInput` -> Interp |
| Flight outro `sp01b_outro_part1/2` | `TdTriggerVolume_3` (1184, 80192, -352), 2560 x 256 x 1024 | `SeqEvent_TdTouch_13` -> 0.2 s -> `TdDisablePlayerInput_3` -> Interp at PlayRate 0.8 |

`SeqAct_TdIntoCutscene` (latent, `Target` the pawn, `Destination` an actor) walks the pawn onto
its mark and fires `Finished`. `SeqAct_TdDisablePlayerInput` (`bDisablePlayerMoveInput`,
`bDisablePlayerLookInput`, `bSetCinematicMode`, `bDisableSkipCutscenes`) holds the player before
the Matinee starts and through lifts; `SeqAct_TdEnablePlayerInput` releases, often behind a
0.5 to 1.5 s delay. Skipping: `SpaceBar` is bound to `GBA_Jump | SkipCutscene`
(`DefaultInput.ini`); the Matinee stops only if `bIsSkippable` (true on every intro, false on The
Shard's helicopter scenes) and no `bDisableSkipCutscenes` is in force. The skip sets the Matinee to
its end (`USeqAct_Interp::SetPosition(InterpLength)`, the keys in between not fired) and the op fires
`Completed`: the Edge intro only teleports the pawn to `TdCheckpoint_0` on a skip, through a gate its
`Land` key (60.3 s) closes 0.5 s later, and that can only be `Completed`'s doing. `Aborted` is the `Stop`
input's; in the 13 Matinees that wire it, its links are a subset of `Completed`'s (the sounds to stop,
input to enable).

Where the pawn is during a cutscene matters for the triggers: the animation's root moves it
(its first-person mesh plays the sequence with root motion), so the triggers on its path fire as
it passes, and the Flight outro's `LevelFinish` key, Edge's landing `Trigger_6`, the supers'
triggers all count on it.

## 3. Level transitions

`SeqAct_TdLevelCompleted` (`NextLevelName`, `NextCheckpointName`) ends a chapter. The ten
instances and their targets are in [KISMET_SEQUENCE_GRAPHS_RE.md](KISMET_SEQUENCE_GRAPHS_RE.md),
section 2.1. What leads to it is the chapter's last scene:

- Edge: the helicopter Matinee's `FadeToWhite` key -> 2 s -> remote `Finish_Level` ->
  `SeqAct_TdFadeEffect` (out to white over 5 s, 3 s in) -> `Completed` -> 2 s -> switch ->
  `SeqAct_TdLevelCompleted` (`Escape_p` at `Start`). Its other input, `Final_VO_Finished`, comes
  2 s after Faith's last line (`A_VO_SP01_1_17_2_Faith_Cue`) has finished playing: a
  `SeqAct_TdPlaySound`'s `Finished` output, so the line's length is part of the timing.
- Flight: the outro Matinee's `LevelFinish` key (16.55 s at 0.8x) -> remote `Escape_Finished` ->
  `SeqAct_TdLevelCompleted` (`Stormdrain_p` at `Canals`).

The next map loads behind its movie: `DefaultEngine.ini [LoadMovies]` maps `escape_p = scene_02`,
`stormdrain_p = scene_04`, `cranes_p = scene_06`, `subway_p = scene_07`, `mall_p = scene_09`,
`factory_p = scene_10`, `boat_p = scene_11`, `convoy_p = scene_12`, `scraper_p = scene_13`,
`tutorial_P = scene_01`; Edge has none. The Shard's `NextLevelName` is `tdmainmenu`.

## 4. Text on the screen

| Action | Where the text is | Shown |
|---|---|---|
| `SeqAct_TdSupersMessage` (`SupersMessage`, `Duration` 6) | `TdGameUI.int [TdSupersMessage]`: `SP01A=Financial District 1.58pm` .. `SP09=The Shard 9.55pm` | the district and time of day as the chapter opens; from a `Trigger` the pawn reaches (Edge `Trigger_8`, Flight `Trigger_6`) |
| `SeqAct_TdTutorialMessage` (`TutorialMessage`, `Duration`, `bReplaceCurrentMessage`) | `[TdTutorialMessages]`: `SpeedVault=<StringAliasBindings:GBA_Jump> when close to low obstacles performs a speed vault...` | `TdUIScene_TutorialHUDMessage`; 56 in the training area, with `Duration` 1e8 (until replaced) or 0.01 (clear) |
| `SeqAct_TdTriggerSubtitle` (`Subtitles[].Text`, `Duration` 5) | inline or `TdLookAt.int [TdLookAtData]`: `LookAt-Escape_Intro_Spt_0="WARNING. HIGH VOLTAGE!"` | the writing on a sign when it is looked at: a `Trigger_LOS` (`TriggerDistance`, `ScreenCenterDistance`) |
| `SeqAct_TdTriggerSplashHint` (`HintNumber`) | `[TdSplashHints] SplashHint1..7`, `TitleLabelText=HINT`, `GamePausedText` | a card with a picture, the game paused under it |
| voice | `Subtitles.INT`, `A_VO_*.INT` | the subtitle of the line playing |
| skip prompt | `[TdPopUps] PopUp4=to skip` | while a skippable Matinee plays |

`<StringAliasBindings:GBA_Jump>` is replaced by the key bound to the alias in `DefaultInput.ini`
(SpaceBar, LeftShift for `GBA_Crouch`, E for `GBA_Use`, right mouse for `GBA_SwitchWeapon`, R for
`GBA_ReactionTime`, LeftAlt for `GBA_LookAt`, Q for `GBA_LookBehind`, TAB for `GBA_InGameMenu`).
`%SixAxis_...%` is the PS3's and shows nothing on PC.

Retail's HUD shows nothing for a checkpoint, a lift, a weapon pickup or a death.

## 5. How the port runs it

| Where | What |
|---|---|
| `src/game/level_script.hpp/.cpp` | `ScriptGraph::load` reads every loaded package's `Main_Sequence` (ops, links with delays and disabled flags, variables, the trigger actors with their cylinder or brush hulls, the Matinees' event and sound keys). `LevelScript` runs it with the engine's ordering above: the events against the player each frame (touch by cylinder or convex hull, line of sight, use), remote events across packages, delays, gates, switches, latent sounds and fades, the Td actions through `ScriptHost` callbacks. `LocalizedStrings` resolves `<Strings:...>` and the key aliases. |
| `src/assets/level_intro.cpp` | `extract_player_cutscenes()`: every pawn Matinee baked like the intro (the intro itself copied), several animations on one timeline (`LevelIntroSequence::segments`). |
| `src/cutscene/cutscene_player.cpp` | `play_level_cutscene(index, play_rate)`: plays one at the Matinee's `PlayRate`; the first-person mesh follows the segment. |
| `src/main.cpp` | `begin_level_play()` after the loading movie: the script starts as `TriggerEventsOnLevelReload` does. The host: checkpoints to the controller (`set_checkpoint`), input locks onto the `InputFrame`, cutscenes to the player, text to the HUD (`PlayerTelemetry::tutorial_text`, `supers_text`, `sign_text`, `splash_hint_*`, `skip_prompt`), `SeqAct_TdLevelCompleted` to a load of the next map at its checkpoint behind its `[LoadMovies]` movie. A death reloads the script at the checkpoint. `R` is load-last-checkpoint, refused while `SeqAct_DisableLoadFromLastCheckpoint` holds. |
| `src/renderer/overlay_ui.inl` | `draw_script_text()`: the supers lower left, the tutorial card top centre, the sign's text, the hint card, the skip prompt. The port's own notes (checkpoint, lift, weapon) no longer use the subtitle slot when a script runs the level. |
| `src/physics/parkour_controller.cpp` | proximity checkpoints and the respawn's drifting baseline only when no script owns the level (`LevelScene::script_checkpoints`). |

With the script running a cutscene, the sounds behind its event keys and on its sound tracks
come from the script (the same actions retail runs); the baked list contributes the animation's
own notifies only (`IntroSoundEvent::from_notify`).

What was wrong before, found on the way: the loading movie was chosen by substring of the map
path, which matched the folder name first (`SP02/Stormdrain_p` played `scene_02`, Flight's;
every chapter from Jacknife to The Shard played the movie of the one before). Checkpoints were
reached by walking within 240 uu of the checkpoint actor, and the respawn point followed the pawn
downwards. `SeqAct_TdLevelCompleted` was read and never acted on. The HUD showed port-only text
(`Checkpoint: ...`, `Elevator In Transit`, a key legend) and none of retail's.

## 6. Checked

- Edge, loaded at `Edge_Start`: the chain `SeqEvt_TdCheckpointLoaded_15 -> TdDisablePlayerInput ->
  IntroCS -> Edge_Pt1_CS SeqAct_Interp_8` starts the intro in the first tick, with the 4 s fade in
  from white and `SeqAct_TdCheckpoint_8`; the baked camera is the one
  [LEVEL_INTROS.md](LEVEL_INTROS.md) measured (the intro sequence is the same bake).
- The retail recording `recordings/20261002_102817_edge_pt1.jsonl.gz` has `A_VO_SP01_1_2_1_Merc`
  at 65.34 s = Matinee start 2.74 s + the `Merc_Intro` key at 62.60 s, `A_VO_SP01_1_3_2_Merc` with
  the pawn at (1286, -2290, 6722), inside `TdTriggerVolume_3` (1152, -2240, 6656) which sets the
  `Canals`-side checkpoint and fires `vo_1_3_2`, and `A_VO_SP01_1_5_1_Merc` with the pawn at
  (-8213, -2392, 5725), 5 s after `Trigger_2` (-7486, -2208, 5796) r 174: the triggers and delays
  the port now runs.

- Flight, loaded at `Office` (oracle stage 21, `--verify-script`; section 8): a bullet at a display
  case's glass fires `SeqEvent_TakeDamage_5`, which hides the pane, turns its collision off, shows the
  twin and makes the cracking emitter; a second bullet shatters the twin. 100 of barge damage on an office
  glass wall runs both of its events and the `SeqAct_CauseDamage` that breaks the twin with it. The player
  run at a glass wall with the melee key down goes into `MOVE_Barge` on the pane and comes out 457 uu past
  it. The remote event `R1_Streamed` reaches a `SeqAct_Toggle` that switches an emitter.

## 7. Not done

- The training area: its Kismet hangs on the movement-challenge system
  (`SeqAct_TdStartMovementChallenge`, `SeqEvt_TdMovementChallenge*`, `SeqEvt_TdTutorialEvent`), which
  the port does not run, so `Tutorial_p` keeps the port's staged tutorial and its own text. (The
  runtime reads a `TdTutorialCheckpoint` as completed when the pawn reaches it, for when it is.)
- AI factories, AI directives, lights, material parameters, physics props, stats and music
  actions finish at once through their `Out` / `Finished` / `Spawned` outputs; a factory's `All Dead`
  never fires, so a checkpoint or door that waits for a squad to die does not come (Jacknife's pillar
  room, the Mall's and the Boat's lifts). The port's bots are not the script's yet.
- `SeqAct_TdDisarmRopeburn` takes `Succeeded`: the Ropeburn counter is not played.
- A Matinee's sound track plays only in a player cutscene; the others would need positioning.
- `SeqAct_TdIntoCutscene` glides the pawn onto its mark over 0.4 s; retail's blend is not measured.
- The splash hint's picture (`TdUIResources_InGame_Hints.splashNN`) is not drawn; the card pauses
  the game as retail's does.
- The layout of retail's text (fonts, positions) is not measured against frames; the positions here
  are the port's.
- A pawn attached to a stand-in that does not move (Edge's is at the origin) is read as moving with
  its animation's root, which is what the retail recordings of the other chapters show.
- `SeqEvent_LOS` is read as a cone test against the trigger's location (`ScreenCenterDistance` in
  pixels of a 1280-wide picture); the engine's own test was not traced.

## 8. Damage events and breakable glass

The story maps hold 1,734 `SeqEvent_TakeDamage` events bound to an actor (1,651 on `InterpActor`s) and no
bound `SeqEvent_Death`. They are how glass breaks, how a door is barged, and how the Shard's servers are
shot out.

**The event** (`Actor.TakeDamage` -> `SeqEvent_TakeDamage.HandleDamage`, `Engine.u`): the damage has to be at
least `MinDamageAmount`, of a class in `DamageTypes` when that list is not empty and of none in
`IgnoreDamageTypes`, and from a player when `bPlayerOnly`. It is added to `CurrentDamage`; at
`DamageThreshold` (100 by class, 1 on 1,622 of the events) the event fires if it may (`MaxTriggerCount`,
`ReTriggerDelay`), writes `Damage Taken`, and takes the threshold off again.

**A pane of glass** is two `InterpActor`s in one place, the pane and its broken twin (a mesh named
`.._Broken`), the twin hidden. Each has an event with threshold 1:

```
the pane   SeqEvent_TakeDamage -> SeqAct_ActorFactory (ActorFactoryEmitter: the cracking effect)
                               -> SeqAct_TdPlaySound
                               -> SeqAct_ToggleHidden (Hide, the pane) -> SeqAct_ChangeCollision (none)
                                  -> SeqAct_ToggleHidden (UnHide, the twin)
the twin   SeqEvent_TakeDamage -> SeqAct_ActorFactory (the breaking effect), SeqAct_TdPlaySound,
                                  SeqAct_ToggleHidden, SeqAct_Destroy
```

A first hit cracks it, a second shatters it. Most panes carry a second event for `TdDmgType_Barge` alone,
which runs the same chain and also sends `SeqAct_CauseDamage` (100, `TdDmgType_Bullet`) to the twin: a
barge goes straight through.

**Who deals damage.** A weapon's instant hit (`Victim.TakeDamage` with the weapon's damage). And 100 of
`TdDmgType_Barge` from `TdMove_Barge` (`BargeHitNotify`, or the shoulder's first contact), `TdMove_AirBarge`
and `TdMove_MeleeAir` (`Bump` / `HitWall`), `TdMove_MeleeCrouch` and `TdMove_MeleeSlide`, each on what its
trace found with `bInteractable` (`TdMove_Barge.CalcBargeDamage`: a zero-extent trace from the pawn's
location along its facing, `max(BargeMinTraceDistance, BargeSpeed * BargeTraceTime)` long when it runs
forward).

**In the port** (`src/game/level_script.*`, `src/game/script_effects.hpp`, `src/game/impact_effects.hpp`,
`src/physics/parkour_controller.cpp`): the events are read with their class defaults and run by the rule
above (`LevelScript::damage_actor`). A bullet tracer tells the script of the level actor it struck; the
barge, the airborne and crouched blow and the slide kick find an interactable actor with a damage event
the way they find a door, and deal their 100. `SeqAct_ToggleHidden`, `SeqAct_Destroy` and
`SeqAct_ChangeCollision` act on the mesh actors: an actor the script can hide is built into a mesh buffer of
its own at load, hidden or not (`LevelActor::script_switched`, `MeshBuffer::actor`), which the three
renderers leave out while it is hidden, shadows included; its collision goes with `SeqAct_ChangeCollision`
and `SeqAct_Destroy`. Some twins wait with no collision and are given it as they are shown (the office
glass walls'): an actor whose collision the script changes has its triangles in the collision world from
the start, switched off, and each triangle keeps its role (the hull's or the mesh's own) for when they are
switched on. A hidden actor that does collide (a display case's twin, waiting) is passed through by a
bullet's damage and its mark. `SeqAct_CauseDamage` reaches any actor's events, not only the player. When a
checkpoint is reloaded the panes are whole again (`restore_script_actors`).

Not as the game: the damage classes are told apart as bullet, barge and blow only (a class filter naming
another subclass is not met); a punch or kick on the ground deals none; the glass has no physics (no
shards beyond the particle effects); a door the port opens itself (`LevelScene::barge_doors`) does not run
its sequence.
