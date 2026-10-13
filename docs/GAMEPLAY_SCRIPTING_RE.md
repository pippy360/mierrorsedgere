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
`Pursuit_chase`, The Shard's `Elevator_shaft`), which waits there. A teleport or a cutscene's
hand-over moves her and leaves the active checkpoint what it was (`relocate`): a death goes back to
the checkpoint, not to where she was last put.

The Training Area has no such checkpoints. There nobody dies: `TdSPTutorialGame.PreventDeath`
restarts the movement challenge, and `ResetPlayer` sets the pawn's `Location` to a `TdTutorialStart`'s,
the challenge's start nearest the last gate passed (`FindMCStartSpot`), or nearest
`LastValidPlayerLocation`, which a timer stores while she walks. The port keeps a list of the
nineteen stages' starts (`LevelScene::checkpoints`, her feet there) and restarts her at the last one
she walked onto. Until 2026-10-11 that restart took the stage's place at the height of the lowest
floor she had stood on since, which is what the void test keeps; a death after a drop put her
inside whatever stands under the stage.

`--verify-respawn` restarts at every checkpoint of every chapter and every stage of the Training
Area; the oracle's stage 25 does the Training Area's and the Prologue's, and a death in each.

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

## 9. World Matinees: the actors they move

The story maps' scripts hold 1,191 `SeqAct_Interp`s whose Matinee has a movement track (`InterpTrackMove`)
bound to a placed actor in the packages the port loads. Before this section the port played those tracks
only for the lifts and the barge doors, which have state machines of their own; every other `InterpActor`
stayed where the level placed it. Research: the main checkout's ignored `build/re/open2/movers/`
(`notes.md`, with a second reader's verdicts in `verify.md`, which overrides it).

**Who moves the actor.** A movement track does nothing when its Matinee is updated (`UInterpTrackMove`'s
`UpdateTrack` is the empty base function). The actor moves itself, once a tick, while its `Physics` is
`PHYS_Interpolating` (an `InterpActor`'s default): `AActor::physInterpolating` (0x00ef9870) looks through
the actor's `LatentActions` for a `SeqAct_Interp` with a group for it (`FindInterpMoveTrack`, 0x00e51960),
takes that group's first movement track and calls `AActor::MoveWithInterpMoveTrack` (0x00f04130). That asks
the track where the actor is at the Matinee's current `Position` and goes there with `UWorld::MoveActor`
(0x00c12050): a move that is placed, not swept. For an actor that collides it first asks
`UWorld::CheckEncroachment` (0x00c12f50) about whoever is in the way. With no Matinee on it an actor stays
where the last one left it; nothing puts it back until the level is loaded again (a restart reloads the
script levels: `TdSPStoryGame.ResetLevel` sets `WorldInfo.bReloadScriptLevels`).

**Where the track puts it** (`UInterpTrackMove::GetLocationAtTime`, 0x00e41340). The keys at a time
(`GetKeyTransformAtTime`, 0x00e41080) are a position from `PosTrack` and a rotation from `EulerTrack`,
both `FInterpCurveVector`s evaluated as 0x00e40ae0 does (linear, constant, or a cubic whose tangents are per
unit of time; the cooked curves never set `InterpMethod`). The Euler angles go through
`FRotator::MakeFromEuler` (0x0116f040: degrees to rotation units, truncated); a track with
`bUseQuatInterpolation` turns by the shortest way between two keys instead. In the port's column-vector
matrices:

```
World(t) = Base * Initial * Rel(t)                 IMF_RelativeToInitial
World(t) = Base * Rel(t)                           IMF_World (the default when MoveFrame is not saved)
Initial  = inverse(Base) * Actor * inverse(Rel(T0))   UInterpTrackInstMove::CalcInitialTransform, 0x00e51ad0
```

`T0` is the Matinee's `Position` when it is initialised (0 when a rewind resets the transforms), `Actor`
where the actor stands at that moment and `Base` the transform of what it is attached to
(`GetBaseMatrix`, 0x00e3c6d0; the identity for nothing). A relative track therefore goes on from wherever
its actor stands, with the key transform of that moment divided out: it is "placed pose plus key" only for
a Matinee first played from 0 with an identity first key (New Eden's lift hull has a first key far from
zero).

**Playback** (`USeqAct_Interp`):
- `Play` (0x00e51d70): with `bForceStartPos` a Matinee that is not playing jumps to `ForceStartPosition`;
  with `bRewindOnPlay` it goes back to its start (when not playing, or playing with
  `bRewindIfAlreadyPlaying`), its movement tracks first taking where their actors stand as the new start
  if `bNoResetOnRewind`. With neither it goes on from its `Position`: one that ran to its end ends again
  at once. `Reverse` plays it backwards, `Stop` clears `bIsPlaying` and `bPaused` and keeps the
  `Position`, `Pause` holds it, `Change Dir` turns it round.
- `StepInterp` (0x00e51dd0): the `Position` moves by `dt * PlayRate`; the event keys passed fire their
  outputs (`(from, to]` forwards, `[to, from)` backwards); past the end (or the start, backwards) a looping
  Matinee goes round and any other stops there. `PlayRate` and `ForceStartPosition` come from the property
  or a linked float variable; a `SeqVar_RandomFloat` is drawn anew between its `Min` and `Max` each time it
  is read (stock Unreal Engine 3, not read in this executable; `SeqAct_Delay` reads it the same way).
- `SetPosition(InterpLength, bJump)` (0x00e4f960), a cutscene's skip: the event keys in between do not
  fire, the actors are put at the end at once, and the op fires `Completed`.
- `DeActivated` (0x00e44f80): output 1 (`Aborted`, the end of a reverse play) when the `Position` is at the
  start, output 0 (`Completed`) when it is at the end, neither for a Matinee stopped part way. An op that
  never built its instances (a `Stop` or `Pause` on an idle Matinee) fires nothing. The op leaves its
  actors' `LatentActions`; they stay where the last update left them.

**Attached actors** go with their base (`UWorld::MoveActor` moves an actor's `Attached`); a track on an
attached actor is relative to its base's transform.

**The player and a mover.** She is based on what she stands on (`Pawn.Base`) and goes with it, turned with
it when it turns (her view is not turned: whether the game turns it was not read). A mover that comes at her
asks `CheckEncroachment`: first she is pushed (`bPushedByEncroachers`, `Actor`'s default, which the pawn
keeps) by the mover's own move, plus, when it turned, 1.5 times the way her place is carried by the turn,
slid along the level; a pawn still in it is tried once more, a cylinder's radius away from its surface and a
tenth of that up. Then `InterpActor.EncroachingOn` decides: only a mover that moves towards her (or one that
only turns) says anything; a falling pawn on a rising mover is set on top of it (`bLandingPawn`) and the
mover goes on; otherwise a mover with `bStopOnEncroach` stops where it was. The `SeqEvent_Mover` events this
tells are not run.

**How the port does it.**

| Where | What |
|---|---|
| `src/game/matinee_move.hpp` | The track evaluation above: the curve, `MakeFromEuler`, the quaternion path, `CalcInitialTransform`, `GetLocationAtTime`. |
| `src/game/level_script.cpp` | The move tracks are read with their Matinee; `Play`, `Reverse`, `Stop`, `Pause`, `Change Dir`, looping, rewind, forced start and the outputs by the rules above; the host's `interp_*` callbacks tell the movers. |
| `src/game/script_movers.hpp` | `update_script_movers`: every mover's physics tick, once a frame after the script's update, bases before what they carry; `bind_scene_movers`, `reset_script_movers`. |
| `src/assets/upk_loader.cpp` | `assign_script_movers`: `LevelScene::movers` (the placed `InterpActor`s a script's movement track is bound to, and what is attached to them) and `LevelScene::matinee_moves`; `build_script_mover_geometry`: each mover's triangles in a collision world of its own, where the level placed it; `release_script_doors`. |
| `src/physics/parkour_controller.cpp` | Every query visits the movers (taken there by the inverse of the mover's `delta`, brought back by it); `carried_by_mover`, `encroached_by_mover`. |
| renderers (Direct3D 11, OpenGL, Metal, Vulkan), `light_environment.hpp`, `impact_effects.hpp` | A mover's mesh is drawn with its `delta`; its light environment, its decals and the bullet holes on it go with it. |

The port's own choices: a lift's parts keep the port's lift machine (`LevelScene::elevators`), which the
script's Matinees do not move (`settle_lifts_for_restart` stays for them). A door she barges (one the
script listens on for damage), one a player's cutscene swings, and a door-like actor no Matinee touches
stay `LevelScene::barge_doors`; a door only Matinees move (Flight's office door, opened by a remote event)
becomes one of the script's movers. No actor is moved by both. Only `InterpActor`s and what is attached to
them are moved: skeletal mesh actors, emitters, lights and triggers that a track names stay where they are.
The same `InterpActor` cooked into two packages (a `_Slc` and a `_Spt`, as the lifts are) is one mover.

**Checked** (oracle stage 29, `--verify-movers`, which then counts the Matinees by chapter;
`ME_MOVER_DEBUG=1` lists them):
- *Evaluation:* Flight's office door turns from yaw 270 to 140.63 and its place moves 0.002; New Eden's lift
  hull is off by 0.000 at 0 s and 1279.999 up at 10 s; Ropeburn's first train ends at y -34095.973;
  Pirandello Kruger's construction lift (world-framed, four keys) ends 0.003 from its start and is 1648.001
  away at its furthest; a turn from 0 to 270 is at 135 half way by its angles, -45 by quaternion.
- *Playback:* the end reached after 16 ticks of 0.01 s and `Completed` the tick after; played again with no
  rewind, it ends at once; reversed to 0 and `Aborted` there; `Stop` at 0.05 fires nothing; a `Stop` on an
  idle Matinee is silent; paused at 0.07 and resumed; a loop went round twice in 50 s and still plays; a
  forced start at 3.0 with rate 2.0, played again by its own `Completed`, rewinds; a linked `PlayRate`
  drew 1.048 from 1..2.
- *The door:* shut, it stands on the line and a walk at it gets 35.7 short; open (-129.37 degrees about its
  hinge, 0.002 off), a walk gets 841.9 past; shutting on her pushes her 81.4 and the door stays at 0 degrees
  (`bStopOnEncroach`).
- *Riding:* Heat's window-cleaner gondola goes 1352.003 uu and she 1352.003 with it, standing on it all the
  way; reversed it is back at its start. Jacknife's girder starts 4886.6 up (`ForceStartPosition`) and rises
  880.3 in 5 s with her on it.
- *Census:* 835 of the 1,191 Matinees move the script's movers, 193 only what the port's lifts and doors
  move, 163 only actors that are not moved (skeletal mesh actors, a few emitters, triggers and static mesh
  actors); of the 463 with something that blocks, 278 are moved and 185 are the lifts' and doors'. 1,204
  movers, 389 with collision.
- *Restarts* (`--verify-respawn` with the level's script run as a restart runs it): 202 places, 190 stand,
  7 in a lift's cab, 2 fall and live, 3 known, 0 failed. Jacknife's `construction` (on the girder its events
  raise) and Ropeburn's `Train_Ride_End` (on the train) left the known list; The Shard's `Scraper_Start`
  (140 under the plaza's floor) is the one place still known for that reason.

**Not done:** the event and sound keys at Time 0 still do not fire (the key test is `(from, to]` from a
`Position` that starts at 0: `TODO.md`); a world Matinee's sound track and its other tracks (float,
vector, colour, visibility, anim control) do nothing; `SeqEvent_Mover` is not run; a skeletal mesh actor a
track names is not moved; a based pawn's view does not turn with its base.
