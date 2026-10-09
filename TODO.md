# Open Issues & Retail Parity Tracker (`TODO.md`)

This file tracks open bugs, missing mechanics, and known retail parity gaps across the engine.

- **Adding issues:** Add a bullet under **User-Added Issues** below (or under any subsystem section). Plain one-line notes are fine.
- **Resolving issues (Agents):** When an item is fixed and verified inside your Git worktree, **remove it from `TODO.md`** in the same commit and record the fix + oracle evidence in `MODLOG.md`. Keep only open issues in this file (no `[x]` history).

---

## User-Added Issues

<!-- Add new bug reports or feature requests right below this comment: -->

---

## Gameplay Scripting, Cutscenes & HUD
*(Refs: `docs/GAMEPLAY_SCRIPTING_RE.md` §7, `docs/LEVEL_INTROS.md` §5, `src/game/level_script.*`, `src/assets/level_intro.*`, `src/cutscene/cutscene_player.*`, `src/renderer/overlay_ui.inl`)*

- **Training Area (`SP00/Tutorial_p`) Kismet challenge nodes:** Implement `SeqAct_TdStartMovementChallenge`, `SeqEvt_TdMovementChallenge*`, and `SeqEvt_TdTutorialEvent` so `Tutorial_p` runs retail Kismet challenges and prompts instead of the port's staged tutorial fallback.
- **AI factory squads & `All Dead` Kismet pin (`SeqAct_TdActorFactory`):** Spawn scripted bot squads from Kismet factories and fire the `All Dead` output link (`EveryoneDied()`) so combat-gated elevators, doors, and checkpoints unlock properly (e.g., Jacknife pillar room, New Eden mall lift, The Boat lift, Shard lobby).
- **Boss fight & finale Kismet mechanics:**
  - `SeqAct_TdActivateRopeburnDisarm` / `SeqAct_TdDisarmRopeburn` QTE in Chapter 4 (`Subway_p`, currently auto-succeeds via `Succeeded` link).
  - `SeqAct_TdTriggerBoss` / `SeqEvt_TdCelesteBossFight` scripting in Chapter 7 (`Boat_p`).
  - Shard server room 4-cluster destruction (`SeqEvent_TakeDamage` + `SeqAct_Switch` counters) in Chapter 9 (`Scraper_p`).
- **Positioned 3D `InterpTrackSound` playback on world Matinees:** Currently only `InterpTrackSound` tracks inside player cutscenes emit audio; non-player world/actor Matinee sound tracks need spatialized playback.
- **`SeqAct_TdIntoCutscene` blend curve parity:** Measure and replicate retail's exact pawn-to-cutscene-mark blend curve/animation (`src/main.cpp` currently glides linearly over 0.4 s).
- **Splash hint texture rendering (`SeqAct_TdTriggerSplashHint`):** Render the hint image `TdUIResources_InGame_Hints.splashNN` inside the paused hint card (`src/renderer/overlay_ui.inl`).
- **On-screen text font & layout parity:** Measure exact retail pixel coordinates, font metrics, and fade timings for `[TdSupersMessage]`, `[TdTutorialMessages]`, `TdLookAt.int` subtitles, and skip prompts (`src/renderer/overlay_ui.inl`, `src/renderer/hud_font.hpp`).
- **Prologue (`Edge_p`) 3D opening title credits & chapter title overlay:** Animate and render the `InterpActor` 3D credit lettering driven by the opening Matinee in `Edge_p`, as well as chapter title / time-of-day HUD cards.
- **Kismet `SeqEvent_LOS` exact visibility check:** Verify retail's native `SeqEvent_LOS` raycast/screen-center check against the port's cone approximation (`src/game/level_script.cpp`).

---

## Movement, Parkour Physics & First-Person Animation
*(Refs: `MODLOG.md` §10.4 & §13.4, `docs/ANIMATION_SYSTEM_RE.md` §6.3, `docs/FIRST_PERSON_ANIMATION_RE.md`, `src/physics/parkour_controller.*`, `src/anim/anim_system.*`, `src/anim/fp_pose.*`)*

- **Remaining `CalcCamera` first-person camera animations:** Only `fallinglandroll` (skill roll) currently drives `camera_animation()`. Enable and verify `EyeJoint` + swizzled `CameraJoint` motion across hard landings, heave-ups, ladder entries, and hang-free turns (`docs/ANIMATION_SYSTEM_RE.md` §6.3).
- **Skill roll lower-body viewmodel offset (`is_lower`):** During `fallinglandroll`, Faith's legs still inherit the fixed standing lower-body offset in `evaluate_faith_1p` (54 uu back, 16° tilt), displacing them from the roll's authored pose (`src/anim/anim_system.cpp`).
- **Walk viewmodel skin clipping at 200–260 uu/s:** Large skin-colored polygons from the upper-body viewmodel clip into the screen edges at slow walk/jog speeds (`200–260 uu/s`, including right after exiting a skill roll).
- **Post-roll / diagonal walk acceleration gap (`seg08`):** Walking back-right after a skill roll caps around `350–390 uu/s` in the port whereas retail accelerates to `~610 uu/s` (`MODLOG.md` §13.4).
- **Gameplay standing eye offset (`12 uu` cutscene hand-over pop):** Retail's standing `1P` pose places the camera at `+158.7 uu` Z and `+8.6 uu` forward from feet, whereas gameplay `kEyeHeightStand` sits at `+166.0 uu` Z / `0.0 uu` forward, causing a 12 uu pop when level intros and cutscenes hand control back (`docs/LEVEL_INTROS.md` §5).
- **Post-handover turn-in-place (`StandTurn90Left` / `StandTurn90Right`):** Replicate retail's 90° turn-in-place animation and cloth/sneak step notifies when control returns in Flight, Jacknife, Ropeburn, Pirandello Kruger, and The Boat (`docs/LEVEL_INTROS.md` §5).
- **Zipline entry glide & impact animations:** Port `TdMove_IntoZipLine`'s ~0.3 s entry glide, `ziplinestart` / `ziplinehitwall` animations, and end-of-cable impact camera motion (`MODLOG.md` §10.4).

---

## Rendering, Lighting & Post-Processing
*(Refs: `docs/RENDERING_RE.md` §10, `MODLOG.md` §9.4 & §14.3, `src/renderer/*`, `src/assets/level_lightmaps.*`, `src/assets/level_postprocess.*`)*

- **Dynamic light environments (`DynamicLightEnvironmentComponent`):** Replace the legacy forward sun + hemisphere stand-in on movers (doors, lifts), NPCs, dropped weapons, and Faith's first-person mesh with retail's spherical harmonic + dominant directional light environment (`docs/AMBIENT_LIGHTING_RE.md` §3.3, `docs/RENDERING_RE.md` §10).
- **Modulated dynamic shadows, decals, and particle emitters:** Render UE3 modulated character/mover shadows (`ModShadowColor`), placed/dynamic `DecalComponent`s, and `ParticleSystemComponent` emitters (steam, sparks, glass shards, helicopter searchlight dust).
- **Sun & lamp lens flares (`LensFlareSource`):** Implement `LensFlareVertexFactory` rendering for sun and light lens flares (e.g., Pirandello Kruger and Prologue rooftops).
- **`TdMotionBlurPostProcess` pass:** Read native speed-to-`MotionPacked` parameter mapping from `MirrorsEdge.exe` and add the radial 8-tap peripheral motion blur pass to the post-process chain (`docs/RENDERING_RE.md` §10).
- **Post-process `MaterialEffect` shaders:** Replace the hand-tuned health, melee impact, and reaction-time tint approximations in `tonemap_fragment` with retail's compiled post-process material expressions.
- **Height fog on translucent surfaces:** Apply height fog / haze to translucent geometry (glass, water, alpha-blended props), which currently draws after the fog pass without fogging.
- **Honour `CastShadow=False` and Kismet-toggled lights/volumes:** Skip shadow-map rendering for static mesh components with `CastShadow=False` (`MODLOG.md` §9.4) and support Kismet actions that toggle lights, material scalar/vector parameters, and post-process volumes at runtime.

---

## Audio, Dialogue & Interactive Music
*(Refs: `docs/AUDIO_SYSTEM_RE.md`, `src/audio/audio_engine.*`)*

- **Surface-aware footsteps during level intros & cutscenes:** Trace floor physical materials (`TdPhysicalMaterialFootSteps`) beneath the animated root during cutscenes instead of defaulting to concrete (`docs/LEVEL_INTROS.md` §5).
- **Room acoustics, occlusion & `SoundGroupEffects` filter presets:** Implement UE3/DICE inside/outside room low-pass filtering, obstruction attenuation, and the 11 global `SoundGroupEffects` presets from `DefaultEngine.ini` (`docs/AUDIO_SYSTEM_RE.md` §2.3).
- **Localized elevator announcements (`SeqCond_TdCaseLanguage`):** Select the active locale link on `SeqCond_TdCaseLanguage` instead of firing all output pins simultaneously (`src/game/level_script.cpp`).

---

## Menus, Time Trials & Platform Backends
*(Refs: `docs/MAIN_MENU_SYSTEM_RE.md`, `docs/SUB_MENUS_RE.md`, `docs/WINDOWS_PORT.md`, `docs/LINUX_PORT.md`)*

- **Time Trial (`TT_*`) & Speed Run mode:** Implement race start/finish triggers (`SeqEvt_TTRace*`), checkpoint split timers, star rating targets from `TdTimeTrial` data, and ghost recording/playback.
- **Native GPU rendering for front-end menu background (`D3D11Renderer`):** Replace the slow (~21 fps) CPU reference background path on Windows front-end menus with hardware-accelerated rendering (`MODLOG.md` §11.3).
- **Linux (`gl_renderer`) native hardware testing & parallel shader upload:** Validate `me_glsl` / `mirrorsedge_linux` on native Linux Mesa/NVIDIA/AMD drivers, move material program linking off the main thread, and exercise `SDL_VIDEODRIVER=offscreen` (`docs/LINUX_PORT.md`).
