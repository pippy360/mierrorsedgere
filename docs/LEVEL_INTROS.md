# Level intros

The seconds at the start of a chapter in which Faith moves into place by herself, before the player gets
control: the run across the roofs that opens the game, the drop into the storm drain, the door kicked open
onto the harbour. This is not the chapter's movie (the `.bik`); it follows it, in the engine, in first person.

This page records what the retail game does there, how the port reproduces it, and how closely, measured
against recordings of retail on 2026-10-08.

## 1. What an intro is in the cooked data

A `SeqAct_Interp` in one of the chapter's sublevels, started by loading the chapter's first checkpoint
(a `SeqEvent_RemoteEvent`). One of its groups, usually named `Faith 1p`, is linked to two things at once:
`SeqVar_TdLocalPawn` and a hidden placed `SkeletalMeshActorMAT` (a plain `SkeletalMeshActor` in Boat), the
stand-in the level designers animated against.

| Chapter | Map | Sublevel | Animation | Animation | Matinee | First checkpoint |
|---|---|---|---|---|---|---|
| Prologue | `edge_p` | `Edge_Pt1_CS` | `sp01_intro` | 60.13 s, from 3.0 s | 63.13 s | `Edge_Start` |
| Flight | `escape_p` | `Escape_Intro_Spt` | `sp01_intro_b` | 16.17 s | 17.00 s | `Start` |
| Jacknife | `Stormdrain_p` | `Stormdrain_All_SPT` | `sp02_intro` | 7.57 s | 7.57 s | `Canals` |
| Heat | `cranes_p` | `Cranes_Puzz_Spt` | `sp03_intro` | 17.10 s | 17.10 s | `SP03_Start` |
| Ropeburn | `Subway_p` | `Subway_MoPu_Spt` | `sp04_intro` | 22.70 s | 22.70 s | `Start_Point_subway` |
| New Eden | `mall_p` | `Mall_HW_Spt` | `sp05_intro` | 16.67 s | 16.67 s | `LevelStart` |
| Pirandello Kruger | `factory_p` | `Factory_Roof_Spt` | `sp06_intro` | 10.00 s | 10.00 s | `Start_point` |
| The Boat | `boat_p` | `Boat_Cont_Spt` | `sp07_intro` | 18.00 s | 18.05 s | `Start` |
| Kate | `convoy_p` | `Convoy_Roof_Spt` | `sp08_intro` | 3.27 s | 3.27 s | `Kates_convoy` |
| The Shard | `Scraper_p` | `Scraper_Out_Spt` | `sp09_intro` | 12.70 s | 12.70 s | `Scraper_Start` |

The training level (`Tutorial_p`) has none: the only body its opening Matinee animates is Celeste's.

The group's tracks:

- **`InterpTrackAnimControl`**, slot `Custom_Canned`: one full-body first-person `AnimSequence`. The camera is
  the `CameraJoint` of the first-person skeleton (`CH_TKY_Crim_Fixer_1P.SK_UpperBody`), animated like any bone.
- **`InterpTrackEvent`** keys, each firing the Kismet output of the same name: the teleport onto the stand-in,
  voice lines (`SeqAct_TdPlaySound`), door hits, the fade.
- **`InterpTrackMove`**, in Boat only: slides the pawn 104 uu sideways over the first seven seconds.

The animation's own `Notifies` carry the foley: `AnimNotify_Footstep` (`FootDown` is the footstep cue's number,
its sign the foot), `TdAnimNotify_CharacterSound` (`ECSClothing_Run` plays `Cloth.Run`) and `AnimNotify_Sound`
(a cue by reference).

## 2. What retail does with it

Found by laying the port over retail and reading the differences, so each item below is something the port
first got wrong.

1. **The root rides on the stand-in.** The animation's root is at the placed actor's Location, and the pawn's
   own Location is 94.0 uu above the root throughout, whether there is floor under the actor or not
   (Jacknife's is in mid-air: the intro starts mid-jump).
2. **Boat is the exception.** Its stand-in is placed 96 uu above the deck, and there the pawn's Location itself
   is at the actor and the root 94 uu below. Boat is also the one intro whose pawn group has a movement track.
   That is the rule the port applies; it is an observation from the recordings, not a mechanism read out of
   the engine.
3. **The pawn follows the full-body root.** Edge and Boat ship two sequences of the same name: the first-person
   one (82 tracks) and a full-body one (89). The pawn's own mesh plays the full-body one and its root is what
   moves the pawn; the first-person body, camera included, rides on the pawn. In Edge the two roots part by up
   to 29 uu and 14 degrees for a few seconds (21.2 to 23.8 s and 33.9 to 40.1 s into the Matinee), and retail's
   pawn and camera follow the full-body one.
4. **The root's rotation is stored the other way round.** Every bone's rotation but the root's is stored with W
   negated, so in the convention the other tracks are read in, the root's is the inverse. A pose taken relative
   to a bone never notices; a pose placed in the world comes out mirrored.
5. **Kismet runs as wired, including what is switched off.** The Mall's Matinee has three door-sound events and
   plays one: the `Play` inputs of the other two actions are `bDisabled`. A link can also delay at either end
   (`ActivateDelay` on the output and on the input: Boat's second door slam is 0.3 s after the squeak).
6. **Doors have Matinees of their own.** Boat's `Kick` event and Subway's `OpenDoor` event activate a remote
   event that plays a 0.6 s Matinee turning the door's `InterpActor`, waits 3 s and plays another that closes
   it, with a sound track (`InterpTrackSound`) and sound actions along the way. The Mall's door is a group of
   the intro's Matinee itself.
7. **`AnimNotify_Sound` cues start twice.** Every one of them is doubled in retail's sound log, in the same
   frame, through all ten intros; footstep and clothing notifies are not. A cue with a random node therefore
   plays two of its variants together.
8. **Footstep 34 is the roll.** `FootDown` runs 1 Sneak, 2 Walk, 3 Run, 5 SprintRelease, 10 LandHard in the
   intros, and 34, for which retail plays `A_Character_Female_01.Body.Roll`.
9. **The Matinee's end stops what it started.** In Edge the `Completed` output is wired to the `Stop` input of
   the two long audio tracks (`A_SP01A_Intro.Intro.Intro_Part_01` and `_02`), so skipping the intro silences
   them.
10. **Nothing frames it.** No letterbox and no title bar: retail shows the skip prompt, top left, and for the
    first seconds the chapter's name and time of day.

## 3. How the port plays it

| Where | What |
|---|---|
| `src/assets/level_intro.cpp` | `extract_level_intro()` finds the Matinee, picks the sequences, bakes the camera per animation frame in world space, and collects every sound with its time, the cues the end stops, and the door swings. Fills `LevelScene::level_intro`. |
| `src/anim/anim_system.cpp` | `bake_canned_camera()`: the first-person pose through the sequence, carried on the pawn's root. |
| `src/cutscene/cutscene_player.cpp` | Plays it for the Matinee's length: interpolates the baked camera, hands the sounds whose time has come to the game, poses the doors. |
| `src/audio/audio_engine.cpp` | `play_cue()`, `play_footstep_number()`, `load_cue_bank()` (a cue's own package, and the packages its waves are imported from), `stop_cue()`. |
| `src/main.cpp` | Loads the intro's cue packages with the level, plays the sounds, stops them and settles the doors when the intro ends or is skipped. `--trace <file>` writes the camera and every sound per frame. |

A level without an intro in its data plays none after its movie. The camera fly-in the port used to play at
every chapter start is kept only for the EXTRAS menu and the cutscene keys, where a scene without an intro
still has something to show.

## 4. The measurement

```
python -m tools.retail.intro_capture --frames 1.0                  # retail: boots each chapter, records its intro
python -m tools.retail.intro_check play build-win/mirrorsedge_windows.exe   # the port, same chapters, with --trace
python -m tools.retail.intro_check compare
```

`intro_check.py` documents how the two are lined up. What matters for reading the numbers:

- Retail's first rendered frame is followed by a hitch of 0.24 to 0.55 s in which its Matinee runs on, so the
  port's clock is shifted by one fitted offset per chapter. The figures start 0.6 s in and stop 0.5 s before
  the end.
- **The hook's camera position is 10 uu ahead of the eye.** It solves the position out of the view-projection's
  first three columns, and the third is the depth column, which carries the near plane. Every camera position
  in a retail trace has this offset along the view direction; the comparison takes it out. (It is also why the
  recordings appeared to put the standing camera 18.5 uu ahead of the pawn. It is 8.5.)
- For about six seconds at the start of a chapter the hook reports yaw 0, pitch 90: it reads its angles off
  another pass's matrix while the chapter's title is on screen. Angles are compared only where they can be
  read, which is nowhere in Kate's 3.3 s.
- The hook logs no sound in the first second or so of a chapter, and reports no animation for the pawn while
  a `Custom_Canned` one plays.
- In the Ropeburn recording the hook's pawn is not the player's (it never moves). The camera is unaffected.

Port at `agent/level-intros`, Windows build, 2026-10-08:

| Map | Eye, median | Eye, 95 % | View direction, median / 95 % / worst | Angles readable | Retail's cues paired |
|---|---|---|---|---|---|
| `edge_p` | 0.2 uu | 4.4 uu | 0.03° / 1.54° / 9.3° | 86 % | 2 of 2 |
| `escape_p` | 0.3 uu | 5.8 uu | 0.06° / 1.23° / 4.3° | 62 % | 24 of 25 |
| `stormdrain_p` | 0.0 uu | 5.9 uu | 0.14° / 0.37° / 0.4° | 9 % | 12 of 13 |
| `cranes_p` | 0.0 uu | 3.1 uu | 0.02° / 0.10° / 0.9° | 62 % | 30 of 30 |
| `subway_p` | 0.0 uu | 1.1 uu | 0.03° / 0.05° / 0.2° | 78 % | 25 of 26 |
| `mall_p` | 0.1 uu | 1.0 uu | 0.02° / 0.37° / 0.8° | 62 % | 31 of 31 |
| `factory_p` | 0.1 uu | 3.3 uu | 0.01° / 0.03° / 0.1° | 39 % | 24 of 25 |
| `boat_p` | 0.0 uu | 0.7 uu | 0.01° / 0.03° / 0.4° | 82 % | 34 of 35 |
| `convoy_p` | 0.5 uu | 8.6 uu | not readable | 0 % | 5 of 5 |
| `scraper_p` | 0.0 uu | 0.5 uu | 0.01° / 0.05° / 0.1° | 48 % | 20 of 20 |

Paired cues start a median of 1 to 6 ms apart, 49 ms at worst. The 95 % eye figures are mostly timing: at
1000 uu/s and more, one of retail's stalled frames is tens of units. The port runs in real time, so they move
by a unit or two from one run to the next; the medians do not.

Before this work the same comparison gave: the view pitched and turned the wrong way in every chapter (pitch
correlation with retail -0.98 to -1.0), no intro found for Ropeburn or The Boat (a made-up 3.2 s fly-in played,
500 to 1300 uu from retail's path), Edge's animation started 3 s early, and none of retail's sounds: footsteps
came from a cadence timer and no voice line played.

## 5. What still differs

- **The hand-over pops 12 uu.** When the intro ends the port's camera jumps 8.6 uu back and 8.2 uu up. This is
  not the intro's doing: the animation ends in the standing pose with the eye 158.7 uu above the feet and 8.6
  ahead of them, which is where retail's camera stays, and the port's gameplay camera stands at feet + 166 on
  the pawn's axis (`kEyeHeightStand`). Left alone here because it is the gameplay camera, used by everything.
- **The turn after the hand-over.** In five chapters (Flight, Jacknife, Ropeburn, Pirandello Kruger, The Boat)
  retail's pawn plays `StandTurn90Left` or `Right` as control returns, heard as one `Cloth.Walk` at the
  Matinee's end and two `FootStepSneak` in the half second after. The port has no turn in place. Which chapters
  turn does not follow from the angle between the stand-in and the final view, so it is not imitated.
- **One second of Edge.** From 50.8 to 51.8 s, during a fast move looking steeply down, retail's view tilts
  about half as far as the animation's camera bone (roll -16° against -34°, 8° in view direction). Not
  explained. There is a 0.3 s stretch of 3° just before it.
- **One footstep.** The Shard's last `FootDown` notify (a sneak step at 6.3 s) sounds in the port and not in
  retail.
- **Surfaces.** The port plays every intro footstep on concrete. Retail's log names the step, not the surface.
- **Edge's title lettering.** The credits that stand in the world during the opening are `InterpActor`s moved
  and shown by the same Matinee. The port does not draw them, nor the chapter name and time of day.
- **Not measured:** retail's angles in the first six seconds of each chapter, anything about Kate's view
  direction, loudness and placement of sounds, and the training level's start in retail (it cannot be booted
  at a checkpoint the way the others are).
