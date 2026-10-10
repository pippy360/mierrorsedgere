# Mirror's Edge Audio System Reverse Engineering Specification

## Overview

Mirror's Edge (Unreal Engine 3 build v536 / Licensee 43, `MirrorsEdge.exe`) uses a heavily customized audio architecture built on top of Epic's UE3 OpenAL audio subsystem (`ALAudio.ALAudioDevice`) combined with 11 custom DICE `TdSoundNode*` signal-processing nodes, a 29-group hierarchical bus mixer with 11 state-driven ducking/LPF profiles (`SoundGroupEffects`), a 36-slot × 9-material surface foley matrix (`TdPhysicalMaterialFootSteps`), stamina/exertion-driven 1st-person breathing & vocalization cues (`TdAnimNotify_CharacterSound`), dedicated level-streaming spatial audio sublevels (`*_Aud.me1`), and Solar Fields' multi-layer interactive music system (`A_M_*.upk`).

---

## 1. Native Audio Backend (`ALAudio.ALAudioDevice`)

### 1.1 Binary & DLL Architecture (`/Users/tomnom/mirrorsedge/Binaries/`)
- **Hardware / Software OpenAL Driver**:
  - `OpenAL32.dll` (115,432 bytes) — OpenAL router DLL.
  - `wrap_oal.dll` (442,368 bytes) — Creative Labs OpenAL 1.1 mixer / EFX filter backend.
- **Ogg Vorbis Decoder Stack**:
  - `ogg.dll` (19,536 bytes), `vorbis.dll` (318,544 bytes), `vorbisfile.dll` (23,632 bytes), `vorbisenc.dll` (986,192 bytes), `libresample.dll` (68,688 bytes).
- **INI Configuration (`TdGame/Config/DefaultEngine.ini`)**:
  ```ini
  [ALAudio.ALAudioDevice]
  MinOggVorbisDuration=10
  DeviceName=Generic Hardware
  ```
- **Cooked Audio Format (`SoundNodeWave` in `.upk` / `.me1`)**:
  - Every `SoundNodeWave` export stores standard UE3 `FPropertyTag` metadata (`CompressionQuality = 41`, `Duration`, `NumChannels = 1` or `2`, `SampleRate = 44100`, `SampleDataSize`) followed by bulk-data headers and raw embedded **Ogg Vorbis (`OggS`)** audio streams.
  - Spatial coordinates use standard UE3 scale (`1 UU = 0.01 meters`, i.e. `AL_POSITION = Location * 0.01f`).
- **Distance**: the device asks OpenAL for no attenuation with distance (`alDistanceModel(AL_NONE)`); a source's gain is worked out by the cue's sound nodes on every update (section 3.2). The positions only pan.

---

## 2. Hierarchical SoundGroup & MixGroup Architecture

### 2.1 `SoundGroups` Tree (`DefaultEngine.ini` lines 133–163)

All `SoundCue` exports belong to a named `SoundGroup` in a 4-tier tree rooted at `Master`:

```text
Master (Volume=1.0)
├── Music (Volume=1.0, bIsMusic=True)
│   ├── HudMusic          (Volume=0.8, bIsUISound=True, bIsMusic=True)
│   ├── CutSceneMusic     (Volume=1.0, bAlwaysPlay=True, bIsMusic=True)
│   ├── InGameMusic       (Volume=1.2, bIsUISound=True, bAlwaysPlay=True, bIsMusic=True)
│   └── TimeTrialMusic    (Volume=1.0, bIsUISound=True, bAlwaysPlay=True, bIsMusic=True)
├── SFX (Volume=1.0)
│   ├── AmbientPropSounds (Volume=1.0)
│   │   ├── StereoAmbient       (Volume=0.9)
│   │   ├── IndoorAmbient       (Volume=1.0)
│   │   ├── IndoorAmbientProps  (Volume=1.2, MaxUpdateDistance=2000.0)
│   │   ├── OutdoorAmbient      (Volume=1.0)
│   │   └── OutdoorAmbientProps (Volume=1.3, MaxUpdateDistance=3000.0)
│   ├── HudSFX            (Volume=1.0, bIsUISound=True)
│   ├── Dead              (Volume=1.0)
│   ├── InGameSFX         (Volume=1.0)
│   ├── BigLooped         (Volume=1.0, bAlwaysPlay=True)
│   ├── Weapons1p         (Volume=1.0)
│   ├── Weapons3p         (Volume=0.75)
│   ├── FaithBreath       (Volume=1.0)
│   ├── FaithVocal        (Volume=1.0)
│   ├── Helicopter        (Volume=1.0, bAlwaysPlay=True)
│   └── Convoy            (Volume=1.0, bAlwaysPlay=True, bUberAlwaysPlay=True)
└── VO (Volume=1.0)
    ├── VOMain (Volume=0.9)
    │   ├── DialogueRadio (Volume=1.0, VoiceCenterChannelVolume=1, bNoReverb=True, bAlwaysPlay=True, bUberAlwaysPlay=True)
    │   ├── DialogueFaith (Volume=1.0, VoiceCenterChannelVolume=1, bAlwaysPlay=True, bUberAlwaysPlay=True)
    │   ├── DialogueOther (Volume=1.0, VoiceCenterChannelVolume=1, bAlwaysPlay=True, bUberAlwaysPlay=True)
    │   └── DialogueHUD   (Volume=1.0, VoiceCenterChannelVolume=1, bAlwaysPlay=True, bIsUISound=True)
    └── DialogueAI        (Volume=1.4)
```

### 2.2 DICE `MixGroups` (`DefaultEngine.ini` lines 164–175)

DICE added a secondary orthogonal modulation layer (`TdSoundNodeMixGroup`) controlled by `MixGroups` with explicit slow-motion (`SloMo`) time-dilation participation flags:

| MixGroup Name | Base Volume | Base Pitch | `SloMo` Pitch/Rate Affected |
|---|---|---|---|
| `Default` | `1.0` | `1.0` | `True` |
| `WeaponSounds` | `1.0` | `1.0` | `True` |
| `CharacterSounds` | `1.0` | `1.0` | `True` |
| `AmbientSounds` | `1.0` | `1.0` | `True` |
| `Reverb` | `1.0` | `1.0` | `True` |
| `AIVO` | `1.0` | `1.0` | `True` |
| `AmbientPropSounds` | `0.9` | `1.0` | `True` |
| `SlowMotionSounds` | `1.0` | `1.0` | `False` |
| `Outdoor` | `1.0` | `1.0` | `False` |
| `Indoor` | `1.0` | `1.0` | `False` |
| `Helicopter` | `1.0` | `1.0` | `False` |
| `Breathing` | `1.0` | `1.0` | `True` |

### 2.3 The 11 Global `SoundGroupEffects` Modes (`DefaultEngine.ini` lines 177–209)

DICE switches global mix snapshots dynamically via 11 indexed `SoundGroupEffects` presets:

| Mode ID | Name / Trigger | Key Volume & LPF Adjustments |
|---|---|---|
| `0` | **Normal** | All `VolumeAdjuster = 1.0`, `LowPassAdjuster = 1.0` |
| `1` | **Ingame CutScenes** | `CutSceneMusic=1.0`, `DialogueRadio/Faith/Other=1.0`, all gameplay `SFX/InGameMusic/Ambient=0.001` |
| `2` | **Ingame VO** | Ducks `InGameMusic=0.6`, `InGameSFX=0.7`, `OutdoorAmbient=0.5`, `Weapons=0.3`, `FaithBreath=0.1` while Merc/Faith speak |
| `3` | **Reaction Time** | **Slow-Motion Focus**: `FaithBreath=1.5` (amplified heartbeat/breathing!), `FaithVocal=0.0`, `InGameMusic=0.7`, `InGameSFX=0.4 (LPF=0.7)`, `AmbientPropSounds=0.3 (LPF=0.2)`, `Weapons3p=0.3 (LPF=0.1)` |
| `4` | **Pause** | Mutes `SFX=0.0`, `VO=0.0`; attenuates `HudMusic=0.3`, `InGameMusic=0.3`, `BigLooped=0.3` |
| `5` | **All Turned Off** | `HudMusic=1.0`, `BigLooped=0.3`, all other buses `0.0` |
| `6` | **Falling to Death** | Mutes all dialogue (`VO=0.0`, `DialogueRadio/Faith/Other=0.0`) while wind rush plays at full gain |
| `7` | **Custom Cutscene Track** | `Master=1.0`, `BigLooped=0.3 (LPF=0.1)`, `OutdoorAmbientProps=0.6 (LPF=0.6)` |
| `8` | **Death by Fall** | `Dead=1.0` (bone impact crunch + tinnitus), `BigLooped=0.3`, all other buses `0.0` |
| `9` | **Death Generic** | `Dead=1.0 (LPF=0.1)`, heavy low-pass muffling (`Volume=0.001, LPF=0.001`) across all world & music buses |
| `10` | **End Credit** | `Music=1.0`, `HudMusic=1.0`, `CutSceneMusic=1.0`, all gameplay SFX/VO `0.0` |

---

## 3. SoundCue & DICE Custom `TdSoundNode*` Classes (`TdGame.u`)

Decompilation of `TdGame.u` reveals 11 custom C++ `SoundNode` subclasses implemented by DICE inside `MirrorsEdge.exe`:

1. **`TdSoundNodeADSR` (`extends SoundNode`)**:
   - Full `Attack`, `Decay`, `Sustain`, `Release` envelope generator (`RawDistributionFloat`) modulating volume and/or pitch (`bModulateVolume`, `bModulatePitch`).
   - Supports 4 curve modes (`enum SoundInterpolationMethod`):
     - `INTERPOLATION_Linear` ($f(t) = t$)
     - `INTERPOLATION_Smooth` ($f(t) = 3t^2 - 2t^3$)
     - `INTERPOLATION_Square` ($f(t) = t^2$)
     - `INTERPOLATION_Fast` ($f(t) = \sqrt{t}$)
2. **`TdSoundNodeAttack` (`extends SoundNode`)**:
   - Distance-coupled attack/decay shaper (`AttackAtMinRadius`, `AttackAtMaxRadius`, `DecayAtMinRadius`, `DecayAtMaxRadius`) softening transient attacks on distant footsteps/gunfire.
3. **`TdSoundNodeAttenuation` (`extends SoundNodeAttenuation`)**:
   - Adds `bDelay` + `SpeedOfSound` (`RawDistributionFloat`, 33100 to 33101 uu/s by default) on top of UE3 `MinRadius` / `MaxRadius` / `LPFMinRadius` / `LPFMaxRadius`. It holds the sound back by its distance over the speed of sound and attenuates twice (section 3.2). Six nodes, in the three gun slap-back cues.
4. **`TdSoundNodeDelayToDistance` (`extends SoundNode`)**:
   - Computes realistic acoustic propagation delay from emitter to listener:
     $$\Delta t_{\text{delay}} = \frac{\min(\|\mathbf{x}_{\text{src}} - \mathbf{x}_{\text{listener}}\|, \text{MaxDistance})}{\text{SpeedOfSound}}$$
     with `MaxDistance = 100000.0` UU (`1000 m`) and `SpeedOfSound` 33100 to 33101 UU/s by default. It never changes the volume, and no cooked cue uses it.
5. **`TdSoundNodeDuckTrigger` (`extends SoundNode`)**:
   - Dynamically ducks specified `MixGroups` to `DuckLevel` (`0.3`) for `DuckDuration` (`3.0s`) when triggered (used on explosions, heavy gunshots, and sniper fire).
6. **`TdSoundNodeInsideOutside` (`extends SoundNode`)**:
   - Crossfades child branches `[0]` (Indoor) and `[1]` (Outdoor) based on whether the listener is inside an indoor `TdReverbVolume`.
7. **`TdSoundNodeMixGroup` (`extends SoundNode`)**:
   - Binds a `SoundCue` sub-branch to one or more DICE `MixGroups` (`bModulateVolume`, `bModulatePitch`, `bModulateLowPass`).
8. **`TdSoundNodeRelativePosition` (`extends SoundNode`)**:
   - Offsets child sound position relative to the actor or camera (`bRelativeToCamera`, `RelativePos`).
9. **`TdSoundNodeRelease` (`extends SoundNode`)**:
   - Distance-scaled release tail (`ReleaseToDistanceFactor = 100.0`, `bReleaseToDistance`).
10. **`TdSoundNodeSlowMotion` (`extends SoundNode`)**:
    - Explicitly scales pitch and volume during Reaction Time (`Slomo`) using `VolumeInterpolationMethod` and `PitchInterpolationMethod`.
11. **`TdSoundNodeVelocity` (`extends SoundNode`)**:
    - Modulates pitch and gain as a function of speed (`SPEEDTYPE_Source`, `SPEEDTYPE_Listener`, `SPEEDTYPE_Relative`, `SPEEDTYPE_Custom`):
      - `MinSpeed` → `MaxSpeed` mapped through `INTERPOLATION_Square` ($u^2$)
      - `PitchAtMinSpeed = 1.0` → `PitchAtMaxSpeed = 1.2`
      - Drives Faith's signature 1st-person wind rush (`A_Character_Effects.upk:RunWind` → `CharacterRunWind` + `HighSpeedClothing`).

### 3.1 How the port plays a cue's graph

A cue whose graph has a `SoundNodeMixer` plays every wave the graph reaches, each with what the nodes above it do to it (`AudioEngine::collect_cue_voices`): a mixer passes on all its inputs, each scaled by its `InputVolume`; `SoundNodeRandom` picks one input by weight; `SoundNodeDelay` holds its branch back by a time drawn from its range; `SoundNodeModulator` scales volume and pitch by values drawn from its ranges. At most 8 layers play; the delayed ones wait in a queue that the audio update starts. A cue with no mixer plays one wave, as before. In the `--trace` play log a layered cue is one entry, lasting as long as its longest layer.

| cue | graph |
| --- | --- |
| `Doors.Door_Barge` (`A_Props_Interactive`) | mixer: the impact, and a random bash after a delay |
| `Doors.Door_Hit` (`A_Props_Interactive`) | mixer of two waves imported from `A_CXP_Plaza.Door_RAW` |
| the footstep cues (`A_Material_Footstep`: 148 mixers in its 133 cues) | mostly mixers too, so footsteps now play their layers |

**Imported waves.** A cue can play waves that live in another package (imports in its package's import table). UE3 loads the imported package along with the cue. The port loads only the imported waves that are not loaded yet (`AudioEngine::load_imported_waves`), from `CookedPC/Audio/<package>.upk` or, for localized packages, `CookedPC/Audio/int/<package>.upk`.

### 3.2 How loud a sound is at a distance (`SoundNodeAttenuation`)

Read out of `MirrorsEdge.exe` and counted over every cooked package (2,406 files, 19,363 cue copies, 2,030 different cues). The listings and the scans are research scratch in the main checkout's ignored `build/re/impacts/research/attenuation/` (`notes.md`, `verify.md`).

**OpenAL does none of it.** The audio device's init calls `alDistanceModel(AL_NONE)` (0x010D97EC), and no reference distance, maximum distance or roll-off is ever set on a source. On every audio update the cue's graph is parsed again for each playing sound: `CurrentVolume` is put back to 1 and `CurrentUseSpatialization` to 0 (0x00B64770), each node multiplies what it has to say into them (`ParseNodes`), and the source plays at the result.

**`USoundNodeAttenuation::ParseNodes`** (0x00B7DD60):

- `MinRadius`, `MaxRadius`, `LPFMinRadius` and `LPFMaxRadius` are drawn from their distributions the first time the node is parsed for a play and kept for that play. A cooked radius is a table holding one pair of values and the draw is between them. Most pairs are one value twice; 62 `MinRadius` and 101 `MaxRadius` of the 1,828 nodes are ranges (the vehicle packs: 18000 to 27000), and three tables store the higher value first (`MaxRadius` 5000, 800 and 5000, 3000; `MinRadius` 1500, 400).
- The distance D is from the listener to the sound's current location, in unreal units, and is taken on every parse.
- With `bAttenuate`: at or beyond MaxRadius the volume is set to 0; at or inside MinRadius it is left alone; between them `DistanceModel` gives the factor:

| `DistanceModel` | factor | nodes |
|---|---|---|
| 0 `ATTENUATION_Linear` | `1 - (D - Min) / (Max - Min)` | 1,399 |
| 1 `ATTENUATION_Logarithmic` | `min(1, -K * ln(D / Max))`, `K = -1 / ln(Min / Max)`, 0.25 when Min is 0 | 117 |
| 2 `ATTENUATION_Inverse` | `min(1, 0.02 / (D / Max) * K)`, `K = Max / Min`, 1 when Min is 0 | 91 |
| 3 `ATTENUATION_LogReverse` | `max(0, 1 - K * ln(1 / (1 - D / Max)))`, K as for Logarithmic | 32 |
| 4 `ATTENUATION_NaturalSound` | `10 ^ ((D - Min) / (Max - Min) * dBAttenuationAtMax * 0.05)` | 189 |

  Linear and Logarithmic run from 1 at MinRadius to 0 at MaxRadius. Inverse is still `0.02 * Max / Min` just inside MaxRadius and is then cut; NaturalSound is at `dBAttenuationAtMax` there (-60 dB on 1,657 nodes) and is then cut; LogReverse steps down as the listener leaves MinRadius (to 0.967 for 400 / 5000) and reaches 0 before MaxRadius. The radii are tested in that order, so a node whose radii are equal (the bodies' impact cue, 300 / 300) is full volume inside and nothing outside.
- `bSpatialize` only ORs into `CurrentUseSpatialization`. The device places a wave in the world when that is set and makes any other one source-relative, at the listener (0x010DB137): a wave is positioned only if a node above it says so.
- With `bAttenuateWithLowPassFilter`, `CurrentHighFrequencyGain` falls linearly from 1 at `LPFMinRadius` to 0 at `LPFMaxRadius`, whatever the model. 1,689 nodes have it on. Whether the PC device does anything with that value was not traced.
- A sound out of range is not culled here: the node goes on being parsed and gives volume 0 while the listener is out there. (Whether the device keeps a source for a wave at volume 0 was not read; the port keeps it, so the sound is heard again if the listener comes back while it lasts.)

**The nodes belong to a branch, not to the cue.** 1,588 cues have one attenuation node, 338 none, 104 several. In those the nodes sit on the inputs of a mixer, each layer with its own radii (the Plaza helicopter: five layers, 2500 / 6000 up to 5000 / 20000). In 17 cues two nodes are on one branch and their factors multiply (the guns' `Fire3P`: LogReverse 1 / 18000 at the root, LogReverse 10 / 8000 over one of its two layers). Seven cues that have a node keep a layer with none above it, which plays at its own level and is not placed (the soft hand step on metal, `Swing.Swing`, `Subway_Ride`). 305 nodes attenuate without spatialising (the 300 radio lines: quieter with distance, never panned) and 74 do neither (Faith's own voice).

**`UTdSoundNodeAttenuation::ParseNodes`** (0x0122AE80), DICE's subclass: its first parse takes the distance D0. With `bDelay` everything below it is held back for `D0 / SpeedOfSound` seconds (not at all when D0 is out of range). With `bAttenuate` it multiplies the volume by the linear factor of D0 on every parse, whatever its model, and then calls its parent's `ParseNodes`, which applies the model to the distance as it is now: a slap-back echo is attenuated twice.

**`USoundNodeAmbient::ParseNodes`** (0x00B7E6B0), the one node of an `AmbientSoundSimple` actor's cue: the same radii, the linear factor only (there is no switch on `DistanceModel`), then its `VolumeModulation` and `PitchModulation`, then its `Wave`. Two actors in the game have one (police sirens in `Escape_Plaza_Aud` and `Cranes_Roof_Aud`, 400 / 5000).

**Whether a sound starts at all.**

- `USoundCue::MaxAudibleDistance` is worked out once per cue, on first use (`CalculateMaxAudibleDistance`, 0x00B76C80): the largest `MaxRadius` over all the cue's nodes, one draw for a ranged one, with no look at `bAttenuate`; 524288 for a cue with no radius. No cue is cooked with a value of its own.
- `UAudioDevice::CreateComponent` (0x00B6B550), given a location, makes no component when `USoundCue::IsAudibleSimple` (0x00B76F10) says no: the cue lasts at most 1.0 s (495 of the attenuated cues) and no listener is strictly nearer than its `MaxAudibleDistance`. A longer cue is always started.
- `Actor.PlaySound` reaches each player through `APlayerController::HearSound` (0x00EF4480), which drops the sound when the view target's `Location` is farther than the cue's `MaxAudibleDistance`, whatever its length.

**The port** does the same arithmetic in the same place:

- *Loader* (`UPKPackage::extract_sound_cues_and_ambients`): every `SoundNodeAttenuation`, `TdSoundNodeAttenuation` and `SoundNodeAmbient` becomes a node of the cue's graph with its `SoundAttenuation` (`src/math/types.hpp`): model, the radius pairs as stored, `dBAttenuationAtMax`, the three flags, `bDelay` and `SpeedOfSound`, over the class defaults (400 / 5000, low pass 1500 / 5000, -60 dB, all flags on).
- *Curve* (`src/audio/attenuation.hpp`): `attenuation_gain` is the table above with the executable's constants; `VoiceAttenuation` is the list of nodes above one wave and their product.
- *Playing* (`src/audio/audio_engine.cpp`): OpenAL's distance model is off. `play_sound_3d` is started by the frame's `update()`, which knows where the listener is: the radii of the cue's nodes are drawn once for the play, every wave gets the nodes on its own way down the graph, is placed in the world if one of them spatialises it (source-relative at the listener otherwise), and keeps a record on its pool source; every `update()` sets each such source to its volume times the product of its nodes' factors for the listener's distance (`update_voice_gains`). The 2D entry points do not look at the nodes, as retail does not for a sound played on the view target (`bAllowSpatialization` off).
- *Emitters*: a level's `AmbientSound` plays its wave at the same product; its radii are drawn once, the first time the emitter is looked at, and it is a candidate for one of the four emitter sources only while the listener is inside the largest of them.
- *Starting*: `play_sound_3d` refuses a cue of at most a second whose `MaxAudibleDistance` the listener is not strictly within, as `CreateComponent` does. The bullet impacts, which retail starts with `PlaySound`, have `HearSound`'s test in `src/game/impact_effects.hpp` (the player farther than the cue's `MaxRadius`, any length).
- *Checks*: `--verify-sound` (oracle stage 23) holds the curve against worked values and the engine's gains against the curve, with no audio device; `--dump-sound-cues <file>` writes every cue's nodes as the loader reads them and `tools/retail/attenuation_check.py` compares that with a scan of the cooked data; `ME_AUDIO_DEBUG=1` prints the nodes, the distance and the gain of every sound at a place when it starts and as it changes.

**Not done.** The low pass is read and kept but not applied. Occlusion (`bCheckSoundOcclusion`, the component's `OcclusionVolumeDuckLevel` 0.3) is not done. The listener is the pawn's location plus her eye height; which point retail's listener is was not traced. `TdSoundNodeRelativePosition` (126 cues) moves the source before the attenuation measures, and the port passes it through. A `TdSoundNodeAttenuation`'s first distance is taken when the cue starts, not after the delays of the nodes above it. The emitter pool still plays one wave per emitter, so a layered emitter cue has one layer's curve. The sound groups' `MaxUpdateDistance` (2000 for `IndoorAmbientProps`, 3000 for `OutdoorAmbientProps`) is not used; what retail does with it is not known.

---

## 4. Surface-Aware Footsteps & Handsteps (`TdPhysicalMaterialFootSteps`)

Every physical surface in Mirror's Edge maps to one of **9 material groups** in `A_Material_Footstep.upk` (133 SoundCues, 425 Ogg Vorbis waves) and `A_Material_Handstep.upk` (37 SoundCues, 116 Ogg Vorbis waves):

- **Surface Groups**:
  1. `Concrete` (default rooftop & street surface)
  2. `Metal` (catwalks, vents, trim)
  3. `MetalGantry` (suspended scaffolding & catwalks)
  4. `Metal_Airduct` (HVAC ducts — hollow resonant booming steps!)
  5. `Metal_Ladder` (ladder rungs & pipes)
  6. `Wood` (planks, interior floors, scaffolding boards)
  7. `Glass` ( skylights, glass walkways)
  8. `Water` (stormdrain puddles & shallow channels)
  9. `Cardboard` (rooftop prop piles)

- **Numbered Action Slots on `TdPhysicalMaterialFootSteps`**:
  - `_01_Female_FootStepSneak` / `Crouch`
  - `_02_Female_FootStepWalk`
  - `_03_Female_FootStepRun`
  - `_04_Female_FootStepSprint`
  - `_05_Female_FootStepSprintRelease`
  - `_06_Female_FootStepWallRun`
  - `_07_Female_FootStepWallrunRelease`
  - `_08_Female_FootStepLandSoft`
  - `_09_Female_FootStepLandMedium`
  - `_10_Female_FootStepLandHard`
  - `_11_Female_FootStepSlide` / `Attack`
  - `_21_Female_HandStepSoft` .. `_25_Female_HandStepFastRelease` (vaults, ledge grabs, wallrun palm touches, pipe climbs)
  - `_31_Female_BodyAttack` .. `_36_Female_BodySlide` (skill rolls, body landings, slides)

---

## 5. Faith Character Breathing, Vocalizations & Combat Audio

Decompiled from `TdAnimNotify_CharacterSound` (`TdGame.u`) and `A_Character_Female_01.upk` (40 SoundCues, 257 Ogg Vorbis waves):

- **Stamina-Coupled Breathing System (`SoundGroup = FaithBreath`)**:
  - Alternates inhale (`_In`) and exhale (`_Out`) cues across 3 exertion tiers:
    - Tier 1 (Walk / Jog): `Breath_Soft.Breath_Soft_Long_In` / `Out`, `Breath_Soft_Short_In` / `Out`
    - Tier 2 (Run / Parkour chain): `Breath_Medium.Breath_Medium_Long_In` / `Out`, `Breath_Medium_Short_In` / `Out`
    - Tier 3 (Full Sprint / Chase / Reaction Time): `Breath_Hard.Breath_Hard_Long_In` / `Out`, `Breath_Hard_Short_In` / `Out`
- **Exertion & Impact Vocalizations (`SoundGroup = FaithVocal`)**:
  - `A_Character_Oral.upk`: `Strain_Medium_Cue` (8 variations), `Strain_Hard_Cue` (8 variations)
  - `A_Character_Female_01.upk`: `Oral_Impact.Soft`, `Oral_Impact.Medium`, `Oral_Impact.Hard`, `Oral_Snatch.Snatch`, `Oral_Death.Death`
- **Clothing & Parkour Foley (`A_Character_Effects.upk` & `A_Character_Female_01.upk`)**:
  - `RunWind` (`CharacterRunWind` + `HighSpeedClothing` velocity-modulated loop)
  - `Vault` (`Foley_Vault_01..03` + `Vault_01..04`)
  - `Body.Roll` + `Body.RollCloth` (skill roll impact + cloth rustle)
  - `Body.BodySlide` (sustained slide friction loop)
- **Disarm, Melee & Weapons (`A_Character_Disarm.upk`, `A_Character_Melee.upk`, `A_WP_*.upk`)**:
  - Every firearm (`A_WP_Pistol_BerettaM93R.upk`, `A_WP_Pistol_Colt1911.upk`, `A_WP_Assault_G36.upk`, `A_WP_SMG_MP5K.upk`, `A_WP_Shotgun_Remington870.upk`, `A_WP_Sniper_BarretM95.upk`) defines 5 coordinated cues:
    1. `Fire1P` (close dry transient stereo layer)
    2. `Fire3P` (NPC 3rd-person mono spatial shot)
    3. `Reverb1P` (indoor/exterior reflection tail)
    4. `Reverb3P` (distant urban canyon tail)
    5. `Slapback` (rooftop building echo slapback)

---

## 6. Solar Fields Interactive Music System (`A_M_*.upk`)

Every campaign chapter (`A_M_SP01A.upk` through `A_M_SP09.upk`), Main Menu (`A_M_Menu.upk`), Ambient (`A_M_Ambient.upk`), and Time Trial (`A_M_TimeTrial.upk`) ships as a dedicated UPK containing synchronized Ogg Vorbis stems driven by Kismet (`SeqAct_SetMusicTrack`, `SeqAct_CrossFadeMusicTracks`):

| Package | Key SoundCues & Stems | Musical Role |
|---|---|---|
| `A_M_Menu.upk` | `Menu.Menu` (`RAW.A_M_Menu`) | Iconic floating Solar Fields Main Menu / Pause synth pad |
| `A_M_SP01A.upk` | `Music.ambience_01`, `Music.ambience_011`, `Music.chase_01` | Prologue / Flight rooftop exploration + helicopter chase |
| `A_M_SP01B.upk` | `Music.ambience_01`, `Music.chase_01..013`, `Music.combat_01`, `Music.stinger_01..03` | Plaza interior/exterior chase & SWAT combat layers |
| `A_M_SP02.upk` | `Music.ambience_01..015`, `Music.Puzzle_01..02`, `Music.chase_01..014`, `Music.combat_01..011` | Jacknife / Stormdrains 4-stem adaptive score |
| `A_M_SP03.upk` | `Music.ME_THEME_Ambience`, `Music.ambience_01..012`, `Music.Puzzle_01`, `Music.chase_01..013`, `Music.combat_01..012` | Heat / Cranes main theme & pursuit |
| `A_M_SP04.upk`..`SP09.upk` | Full 4-layer stems (`ambience_*`, `Puzzle_*`, `chase_*`, `combat_*` + `stinger_*` + `The_End`) | Ropeburn, New Eden, Pirandello, Boat, Kate, The Shard |
| `A_M_TimeTrial.upk` | `Cues.Music` (`RAW.A_TT_Music`) | Dedicated Time Trial pulse track |

---

## 7. Streaming Spatial Audio Sublevels (`*_Aud.me1`)

Mirror's Edge separates level audio into dedicated streaming `.me1` sublevels:
- `Maps/Menu/TdMainMenu_Audio0.me1` .. `TdMainMenu_Audio20.me1`
- `Maps/SP00/Tutorial_Aud.me1` (304 exports: 38 `AmbientSound` + `AudioComponent` emitters, 88 embedded `SoundNodeWave` Ogg Vorbis streams, 2 `TdReverbVolume` zones)
- `Maps/SP01/Edge_Pt1_Aud.me1`, `Edge_Pt1-Pt2_Aud.me1`, etc.

Each `AmbientSound` actor specifies:
- `Location` (`FVector` in UU)
- `AudioComponent` → `SoundCue` (`VehiclePack_02`, `VehiclePack_03`, `WindHard`, `AirConditioner`, `Transformer`, `City_Calm`, `ID_Corridor_01`, etc.)
- How far it carries is not the actor's: it is the `SoundNodeAttenuation` nodes of its cue (section 3.2).

**How far an emitter carries.** Every one of the 1,650 emitters in the `*_Aud` files has a cue with an attenuation node: 881 one, 767 two (a mixer of two layers, each with its own radii). The largest `MaxRadius` of an emitter's cue is 3000 uu at the median, 1000 or less for a tenth of them (fans, vents, lights) and 27000 for the vehicle packs (Logarithmic, 1800 to a `MaxRadius` drawn between 18000 and 27000). The training area's 38 are 24 `WindHard` (Linear 10 / 2000), 9 vehicle packs and 5 `GapWind` (Linear 10 / 1000). The port's emitter pool gives a source to the four nearest emitters whose reach the listener is inside, each at its curve's gain (`AudioEngine::update_ambient_emitters`); it had given every emitter 200 / 3000 through OpenAL's inverse model. The two `AmbientSoundSimple` actors' cue is a single `SoundNodeAmbient` holding the wave itself; the loader had found no wave in it, so they were silent.

**How an emitter repeats** is its cue's own graph. A wave under a `SoundNodeLooping` plays end to end (`WindHard`, the air conditioners). The vehicle packs have a `SoundNodeDelay` between the two: `VehiclePack_02` is `Looping` → `Delay` (7 to 15 s) → `Modulator` → a `SoundNodeRandom` over six groups weighted 7 / 3 / 8 / 8 / 5 / 7 (brakes, buses, cars, horns, motorcycles, trucks; 38 waves). Every round draws the delay again, waits it out, plays one pass once with its own volume and pitch, and the next round starts when that ends: a vehicle every 10 to 20 s, a horn about one time in five. The port's emitter pool does the same (`AudioEngine::next_ambient_voice`); it had looped the cue's first wave end to end.

**With a menu up the emitters are silent.** Retail's front end is a map of its own, `TdMainMenu`, and it has no `AmbientSound` at all: its Kismet cross-fades the menu music and its UI scenes play `A_HUD` cues (`UIAction_PlaySound`), nothing else. The port loads a level behind its front end and listened to it from the world's origin, which in the Training Area is next to a vehicle pack; it no longer plays the level's emitters while the front end or the pause menu is up.
