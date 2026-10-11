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
  - A source's position and velocity go to OpenAL as the wave's `Location` and `Velocity` times 0.0127 (`FALSoundSource::Update`, 0x010D72AE), the listener's position likewise with its velocity left at 0 (`UALAudioDevice::Update`, 0x010D6F30). The port uses 0.01 and sets no velocity: with no distance model the scale changes nothing but Doppler, which the port does not have.
- **Distance**: the device asks OpenAL for no attenuation with distance (`alDistanceModel(AL_NONE)`); a source's gain is worked out by the cue's sound nodes on every update (section 3.2). The positions only pan.
- **Sources**: `MaxChannels=32` and `UseEffectsProcessing=True` (`Engine/Config/BaseEngine.ini`, `[ALAudio.ALAudioDevice]`). The device makes 32 OpenAL sources for everything that sounds (64 at most; `UALAudioDevice::Init`, 0x010D9760) and, when its effects come up, an EFX low-pass filter for each (section 3.2). Which waves get the sources is section 3.3.

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

DICE added a secondary orthogonal modulation layer (`TdSoundNodeMixGroup`) controlled by `MixGroups` with explicit slow-motion (`SloMo`) time-dilation participation flags. A `TdSoundNodeMixGroup` in a cue's graph multiplies the volume, the pitch and the high-frequency gain of what is below it by those of each mix group it names (`UTdSoundNodeMixGroup::ParseNodes`, 0x012292A0; `Default__TdSoundNodeMixGroup` has all three modulations on and names `Default`). Nearly every level emitter's cue has one at or next to its root: the props' cues (`A_Prop_*`) name `AmbientPropSounds`, the ambiences `AmbientSounds`. The port applies the ini's Volume and Pitch below wherever it walks a cue's graph (`src/audio/sound_groups.hpp`); what the device's mix group object adds while the game runs (`TdSoundNodeDuckTrigger`'s ducking, slow motion) was not read.

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
| `4` | **Pause** | Mutes `SFX=0.0`, `VO=0.0`; attenuates `HudMusic=0.3`, `InGameMusic=0.3`, `BigLooped=0.3`. No script of `TdGame.u` sets this mode: a paused game is section 3.3's |
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

A cue whose graph has a `SoundNodeMixer` plays every wave the graph reaches, each with what the nodes above it do to it (`AudioEngine::collect_cue_voices`): a mixer passes on all its inputs, each scaled by its `InputVolume`; `SoundNodeRandom` picks one input by weight; `SoundNodeDelay` holds its branch back by a time drawn from its range; `SoundNodeModulator` scales volume and pitch by values drawn from its ranges; a `TdSoundNodeMixGroup` scales them by its mix groups' (section 2.2). At most 8 layers play; the delayed ones wait in a queue that the audio update starts. A cue with no mixer plays one wave, as before. In the `--trace` play log a layered cue is one entry, lasting as long as its longest layer. That is a sound asked for once. A level's emitter is different: its cue plays for as long as the level does, and its graph is parsed on every update as retail parses it (section 3.3).

| cue | graph |
| --- | --- |
| `Doors.Door_Barge` (`A_Props_Interactive`) | mixer: the impact, and a random bash after a delay |
| `Doors.Door_Hit` (`A_Props_Interactive`) | mixer of two waves imported from `A_CXP_Plaza.Door_RAW` |
| the footstep cues (`A_Material_Footstep`: 148 mixers in its 133 cues) | mostly mixers too, so footsteps now play their layers |

**Imported waves.** A cue can play waves that live in another package (imports in its package's import table). UE3 loads the imported package along with the cue. The port loads only the imported waves that are not loaded yet (`AudioEngine::load_imported_waves`), from `CookedPC/Audio/<package>.upk` or, for localized packages, `CookedPC/Audio/int/<package>.upk`.

### 3.2 How loud a sound is at a distance (`SoundNodeAttenuation`)

Read out of `MirrorsEdge.exe` and counted over every cooked package (2,406 files, 19,363 cue copies, 2,030 different cues). The listings and the scans are research scratch in the main checkout's ignored `build/re/impacts/research/attenuation/` (`notes.md`, `verify.md`). The script functions named below are `Engine.u`'s bytecode as `tools/uscript_bytecode.py` decodes it.

**OpenAL does none of it.** The audio device's init calls `alDistanceModel(AL_NONE)` (0x010D97EC), and no reference distance, maximum distance or roll-off is ever set on a source. On every audio update the cue's graph is parsed again for each playing sound: `CurrentVolume` is put back to 1 and `CurrentUseSpatialization` to 0 (0x00B64770), each node multiplies what it has to say into them (`ParseNodes`), and the source plays at the result.

**`USoundNodeAttenuation::ParseNodes`** (0x00B7DD60):

- The node does nothing for a component whose `bAllowSpatialization` is clear (the test at 0x00B7DD68): no fall-off, no low pass, and `CurrentUseSpatialization` put to 0. The flag is on by default (`Default__AudioComponent`, Engine.u). `PlayerController.ClientHearSound` clears it on the component it makes for a sound whose actor is the view target or the controller itself, and `PlayerController.Kismet_ClientPlaySound` when it is passed `bSuppressSpatialization`: a sound that reaches the player that way from her own pawn has no fall-off and is not placed.
- `MinRadius`, `MaxRadius`, `LPFMinRadius` and `LPFMaxRadius` are drawn from their distributions the first time the node is parsed for a play and kept for that play. A cooked radius is a table holding one pair of values and the draw is between them (the table's sampler, 0x011699D0, takes a step of the engine's random generator; that the result is uniform between the two was not traced). Most pairs are one value twice; 62 `MinRadius` and 101 `MaxRadius` of the 1,828 nodes are ranges (the vehicle packs: 18000 to 27000), and three tables store the higher value first (`MaxRadius` 5000, 800 and 5000, 3000; `MinRadius` 1500, 400).
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
- With `bAttenuateWithLowPassFilter` (0x00B7DEDA), whatever the model and whether or not the node has `bAttenuate`: at or beyond `LPFMaxRadius` `CurrentHighFrequencyGain` is set to 0, between the two LPF radii it is multiplied by `1 - (D - LPFMin) / (LPFMax - LPFMin)`, at or inside `LPFMinRadius` it is left alone. 1,689 nodes have it on. What the device does with the value is under **The low pass** below.
- A sound out of range is not culled here: the node goes on being parsed and gives volume 0 while the listener is out there. The wave is then not handed to the device at all (section 3.3): it holds no source and does not move on. (The port's one-shots keep their source and play on unheard, so such a sound is heard again, further on, if the listener comes back while it lasts.)

**The nodes belong to a branch, not to the cue.** 1,588 cues have one attenuation node, 338 none, 104 several. In those the nodes sit on the inputs of a mixer, each layer with its own radii (the Plaza helicopter: five layers, 2500 / 6000 up to 5000 / 20000). In 17 cues two nodes are on one branch and their factors multiply (the guns' `Fire3P`: LogReverse 1 / 18000 at the root, LogReverse 10 / 8000 over one of its two layers). Seven cues that have a node keep a layer with none above it, which plays at its own level and is not placed (the soft hand step on metal, `Swing.Swing`, `Subway_Ride`). 305 nodes attenuate without spatialising (the 300 radio lines: quieter with distance, never panned) and 74 do neither (Faith's own voice).

**`UTdSoundNodeAttenuation::ParseNodes`** (0x0122AE80), DICE's subclass: its first parse takes the distance D0. With `bDelay` everything below it is held back for `D0 / SpeedOfSound` seconds (not at all when D0 is out of range). With `bAttenuate` it multiplies the volume by the linear factor of D0 on every parse, whatever its model, and then calls its parent's `ParseNodes`, which applies the model to the distance as it is now: a slap-back echo is attenuated twice.

**`USoundNodeAmbient::ParseNodes`** (0x00B7E6B0), the one node of an `AmbientSoundSimple` actor's cue: the same radii, the linear factor only (there is no switch on `DistanceModel`), then its `VolumeModulation` and `PitchModulation`, then its `Wave`. Two actors in the game have one (police sirens in `Escape_Plaza_Aud` and `Cranes_Roof_Aud`, 400 / 5000).

**The low pass.** (The listings are research scratch in the main checkout's ignored `build/re/open2/audio2/`: `notes.md`, `verify.md`.) The value that reaches the device is the product of four things, put together by `UAudioComponent::UpdateWaveInstances` (0x00B6F570) and `USoundNodeWave::ParseNodes` (0x00B77ADD):

1. the node graph's `CurrentHighFrequencyGain`: the ramps above, the same two branches in `USoundNodeAmbient` and `USoundNodeAmbientNonLoop`, and under a `TdSoundNodeMixGroup` its mix groups' low pass;
2. the sound group's `LowPass`, DICE's addition to `SoundGroupProperties` (1 for every group in the ini; multiplied down the group tree, and by the current sound mode's `LowPassAdjuster`: in Reaction Time 0.7 on `InGameSFX`, 0.1 on `BigLooped` and `VOMain`);
3. the component's adjust-low-pass level, which script moves with `AdjustLowPassFiltering` (an occluded sound's goes to `OcclusionFilterDuckLevel`, 0.3);
4. the component's `LowPassMultiplier` (1 by default).

`FALSoundSource::Update` (0x010D71B0) holds the product between 0.01 and 1.0, and `FALAudioEffectsManager::UpdateEffect` (0x010DBCB0) writes it, at the end of every update of the source, as `AL_LOWPASS_GAINHF` of the source's own EFX low-pass filter (its `AL_LOWPASS_GAIN` 1.0) and sets that filter as the source's `AL_DIRECT_FILTER` and on its reverb send. So past `LPFMaxRadius` a sound that is still inside its `MaxRadius` is dull, never without high frequencies; a `TdSoundNodeAttenuation` with the low pass on is at that floor at every distance (its parent's `ParseNodes` finds the first distance and the delay where it looks for its LPF radii); and the nodes do not filter a component whose `bAllowSpatialization` is clear.

The filters are there only if the device's effects came up (0x010DBE50): `UseEffectsProcessing` (set in the ini), the `ALC_EXT_EFX` extension and every one of its entry points, at least one auxiliary effect slot and one auxiliary send, and a filter for each of the 32 sources. The game ships Creative's `wrap_oal.dll` (2.2.0.5, which names `ALC_EXT_EFX`) behind the OpenAL router and opens the device `Generic Hardware`. Whether that gives slots, sends and filters on a retail machine was not determined (it needs a run of the game or of that library), so **it is not known whether retail's players hear the low pass**.

**Whether a sound starts at all.**

- `USoundCue::MaxAudibleDistance` is worked out once per cue, on first use (`CalculateMaxAudibleDistance`, 0x00B76C80): the largest `MaxRadius` over all the cue's nodes, one draw for a ranged one, with no look at `bAttenuate`; 524288 for a cue with no radius. No cue is cooked with a value of its own.
- `UAudioDevice::CreateComponent` (0x00B6B550), given a location, makes no component when `USoundCue::IsAudibleSimple` (0x00B76F10) says no: the cue lasts at most 1.0 s (495 of the attenuated cues) and no listener is strictly nearer than its `MaxAudibleDistance`. A longer cue is always started. (What is compared with 1.0 is the float the cue's virtual at vtable offset 0x114 returns. That this is the cue's duration, stock Unreal Engine 3's `GetCueDuration`, is by the slot's place: its body was not read.)
- `APlayerController::HearSound` (0x00EF4480), the only caller of `USoundCue::IsAudible` (0x00B76D90), drops a sound that is not the controller's own when the view target's `Location` is farther than the cue's `MaxAudibleDistance`, whatever its length, and hands the rest to `ClientHearSound`. That `Actor.PlaySound`, a native, is what calls it for each player is stock Unreal Engine 3: the call itself was not read (the function sits in three vtables and nothing calls it directly).
- `PlayerController.ClientHearSound` makes the component: for a sound with no actor, one at `SourceLocation`; for the view target's or the controller's own, one with no place and `bAllowSpatialization` cleared; for any other actor's, one on the actor (at `SourceLocation` when that is given and is not the actor's own location). It halves the component's `VolumeMultiplier` when `HearSound` found the sound occluded, and plays it.

**The port** does the same arithmetic in the same place:

- *Loader* (`UPKPackage::extract_sound_cues_and_ambients`): every `SoundNodeAttenuation`, `TdSoundNodeAttenuation` and `SoundNodeAmbient` becomes a node of the cue's graph with its `SoundAttenuation` (`src/math/types.hpp`): model, the radius pairs as stored, `dBAttenuationAtMax`, the three flags, `bDelay` and `SpeedOfSound`, over the class defaults (400 / 5000, low pass 1500 / 5000, -60 dB, all flags on).
- *Curve* (`src/audio/attenuation.hpp`): `attenuation_gain` is the table above with the executable's constants and `low_pass_gain` the low pass's ramp; `VoiceAttenuation` is the list of nodes above one wave, with the product of their factors (`gain`) and of their low passes (`high_frequency_gain`).
- *Playing* (`src/audio/audio_engine.cpp`): OpenAL's distance model is off. `play_sound_3d` is started by the frame's `update()`, which knows where the listener is: the radii of the cue's nodes are drawn once for the play, every wave gets the nodes on its own way down the graph, is placed in the world if one of them spatialises it (source-relative at the listener otherwise), and keeps a record on its pool source; every `update()` sets each such source to its volume times the product of its nodes' factors for the listener's distance (`update_voice_gains`). The game loop gives every `update()` the same listener, the pawn's location plus her eye height, in play and while a menu has the frame (`update_audio` in `src/main.cpp`): a sound the level's script asks for as play begins from a menu is started by that frame's update. The 2D entry points do not look at the nodes, as retail's nodes do nothing for a component `ClientHearSound` made for the view target's own sound (`bAllowSpatialization` cleared, above).
- *The low pass*: every sound at a place carries the product of its nodes' low passes, held between 0.01 and 1 as the source holds it (`PositionalVoice::high_frequency_gain`), taken again at every update like the gain. Where the device's OpenAL has EFX it is applied as retail applies it: one low-pass filter for each source that plays sounds at a place (the pool's and the emitters'), `AL_LOWPASS_GAIN` 1, `AL_LOWPASS_GAINHF` the value, the filter set on the source again at every update (`AudioEngine::set_source_low_pass`); a 2D sound on a pool source has no filter. The entry points are asked of the library at run time (`alcIsExtensionPresent("ALC_EXT_EFX")`, then `alGetProcAddress`) and never linked. OpenAL Soft, which the Windows and Linux builds use, has the extension; Apple's OpenAL, which the macOS build uses, is not expected to: there the value is worked out and nothing is applied. (On this Windows machine OpenAL Soft reports the extension, gives a filter for each of the 64 sources that play sounds at a place and takes the calls with no error: `--audio-device-check`. The macOS build was not run.) Of the four factors only the first is in: the port has no group low pass, no occlusion and no component multiplier, and no reverb send. The port's choice is to filter wherever the device can, as retail's code does; whether retail's own OpenAL lets it is the open point above.
- *Emitters*: a level's `AmbientSound` is played by parsing its cue's graph on every update: section 3.3.
- *Starting*: `play_sound_3d` refuses a cue of at most a second whose `MaxAudibleDistance` the listener is not strictly within, as `CreateComponent` does. The bullet impacts, which retail starts with `PlaySound`, have `HearSound`'s test in `src/game/impact_effects.hpp` (the player farther than the cue's `MaxRadius`, any length).
- *Checks*: `--verify-sound` (oracle stages 23 and 26) holds the curve and the low pass against worked values and the engine's gains and high-frequency gains against them, with no audio device, and walks the factory's emitters for a source at volume 0 or a layer off its node's curve; `--dump-sound-cues <file>` writes every cue's nodes as the loader reads them and `tools/retail/attenuation_check.py` compares that with a scan of the cooked data; `ME_AUDIO_DEBUG=1` prints the nodes, the distance, the gain and the high frequencies of every sound at a place when it starts and as it changes; `--audio-device-check` opens the audio device with no window and the master volume at nothing and says whether it gave the filters.

**Not done.** Nothing here was listened to: the gains and the filter values are numbers read back with no audio device, and the one run on a device (`--audio-device-check`, silent) says only that OpenAL Soft takes the filter calls without an error, not what they sound like. Occlusion (`bCheckSoundOcclusion`, the halved volume in `ClientHearSound`, the component's `OcclusionVolumeDuckLevel` and `OcclusionFilterDuckLevel`, both 0.3) is not done, nor the sound groups' and sound modes' low pass. The listener is the pawn's location plus her eye height; which point retail's listener is was not traced. `TdSoundNodeRelativePosition` (126 cues) moves the source before the attenuation measures, and the port passes it through. A `TdSoundNodeAttenuation`'s first distance is taken when the cue starts, not after the delays of the nodes above it. A one-shot from the pool keeps its source while it is out of range, and is not ranked against anything (section 3.3).

### 3.3 The device's update: which waves get a source, rounds, a paused game

Read out of `MirrorsEdge.exe`, `Engine.u` and `TdGame.u` and counted over the cooked `*_Aud` sublevels; the listings are research scratch in the main checkout's ignored `build/re/open2/audio2/` (`notes.md`, and `verify.md`, a second reading that corrected it in places).

**One wave, one voice.** `UAudioComponent::UpdateWaveInstances` (0x00B6F570) parses a playing cue's graph on every update. `USoundNodeWave::ParseNodes` (0x00B77880) keeps one wave instance for each wave under each parent node and fills it with what the nodes above it on its own branch have left in the component at that moment: volume, pitch, high-frequency gain, place, whether it is spatialised, whether it loops. A `USoundNodeMixer` (0x00B767A0) parses every input from the state it was reached with, scaled by the input's `InputVolume`, and puts the state back after each: the layers of a mixer sound together, each with the attenuation of its own branch. An indoor air vent is two voices at one place, its hiss on Logarithmic 2 / 1500 and its hum on Logarithmic 150 / 3000.

**Which waves get a source** (`UAudioDevice::Update`, 0x00B70440):

- A wave whose volume comes to 0.0001 or less is not handed to the device (0x00B77B78), unless its sound group has `bUberAlwaysPlay`: out of range means no source. The cue goes on being parsed.
- A cue whose sound group has a `MaxUpdateDistance` is not parsed at all on an update that finds the nearest listener farther off (0x00B6FBB5): `IndoorAmbientProps` 2000 uu, `OutdoorAmbientProps` 3000. Of an indoor vent whose hum would carry to 3000 nothing is left past 2000.
- The waves handed in are sorted by `PlayPriority` (0x00B77A8D, 0x00B68590): the volume the graph gave the wave (the wave's own `Volume`, the mixer's inputs, the modulators, the mix groups, the distance), without the cue's, the component's and the sound group's multipliers and fades, plus 1.0 for a sound group with `bAlwaysPlay` (the dialogue groups but `DialogueAI`, `BigLooped`, `Helicopter`, `Convoy`, the cutscenes', the level's and the time trials' music). The 32 highest are the active ones (0x00B7037D). Loudness decides, not distance, and equal ones fall as the sort leaves them.
- `StopSources` (0x00B6B340) stops a source whose wave is not active any more. `StartSources` (0x00B6EFB0) gives an active wave that has no source a free one, which plays it from its start (`FALSoundSource::Init`, 0x010DB0E0), and only updates one that has a source: a change of rank among the active waves restarts nothing.
- A wave that is left out is marked finished on the spot, so it never plays, unless it loops for ever or its component has `bShouldRemainActiveIfDropped`. A level emitter's component has it (`Default__AmbientSound`'s `AudioComponent0`, `Engine.u`), so an emitter that loses the ranking is in the running again on every update and starts over when it wins a source. A wave without a source does not move on.
- A component whose cue lasts less than 10000 s is stopped once it has played for 2.5 times the cue's length (0x00B702E6).

**Loops and rounds.** `USoundNodeLooping::ParseNodes` (0x00B7E130) makes itself the hook the waves below report their end to and asks for seamless looping; every emitter's looping node loops for ever (`Default__SoundNodeLooping` has `bLoopIndefinitely`, and none of their nodes stores its own). `USoundNodeMixer`, `USoundNodeRandom` (0x00B7EFB1) and `USoundNodeDelay` (0x00B7F296) clear the seamless flag for what is below them, so **a wave loops in its source only when the last of those four kinds of node above it is a looping node**. The source then queues the wave's buffer twice and queues it again each time one has played, telling the hook once a pass (`FALSoundSource::IsFinished`, 0x010D8DF0). Any other wave plays once. When it ends, `USoundNodeLooping::NotifyWaveInstanceFinished` (0x00B7FE50) counts it, and when every wave the round's parse reached has ended it marks every node below itself to be drawn again and un-finishes its waves: the next parse plays the next round. A node above the looping node is never drawn again. A `USoundNodeDelay` (0x00B7F200) draws its wait at its first parse and keeps the nodes below it from being parsed, so no wave and no source, until it is over on the game's clock (`WorldInfo.TimeSeconds`). So:

| cue (emitters) | graph, root first | what plays |
|---|---|---|
| `A_Prop_AirVent_Indoor` and the other layered props (767) | mix group, mixer of two branches, each: attenuation, modulator, random, a looping node over each wave | each layer draws one of its waves and its modulation once and loops it without a gap |
| `VehiclePack_01` to `_03` (291) | mix group, attenuation, looping, delay 7 to 15 s, modulator, random over 38 passes | a round is a wait, then one pass once; the next round draws all three again |
| `WindHard` (63) | mix group, attenuation, mixer of a looping node over the bed and three more, each over a random node (one of them after a delay) | the bed loops; each of the other layers plays one gust a round, one after another |
| `DullHammering`, `Horns&Boats` (8) | mix group, attenuation, delay, looping, modulator, random | one wait when the level starts, then one sound after another |
| `WallWater_Discreet_StormDrain`, `A_Prop_SecurityCamera` (24) | a looping node over a mixer of delayed layers | the layers play once each; the round starts again when the last of them has ended |
| `BirdsChirp` (17) | a looping node over a random node whose inputs are delays over random nodes, one of them with an empty input | a round that draws the empty input reaches no wave: nothing in the graph is playing or waiting, and the component stops for good (0x00B6FCF1) |

That a random node, a modulator and an attenuation node draw again when a loop above them asks is stock Unreal Engine 3 for the first two (their functions were not read beyond the store above) and read for the third (its draw is behind the payload's first word, 0x00B7DE0B, which the looping node sets).

**A paused game.** The engine's tick gives the device `bGameTicking = !World->IsPaused()` (0x007C7E47), and the world is paused while `WorldInfo.Pauser` is set (also for the frames of a blocking level load or of a pending map change: 0x00B53A30). `TdUIScene_Pause.PostInitialize` calls `SetPause(true)`; a UI scene with `bPauseGameWhileActive` does it through `GameUISceneClient.ConditionalPause` (a tutorial message whose Kismet action has `bPauseGame`; the hint card's scene is labelled "game paused"). The main menu is not a paused game: it is a map of its own, and `TdHUD.PauseGame` returns at once there.

- On the first update of a game that is not ticking `UAudioDevice::HandlePause` (0x00B652B0) pauses every source whose component is not of a UI sound group (`alSourcePause`, the only one in the executable), and on the first update of one that ticks again it plays them all, which takes each up where it stopped. The UI sound groups are `HudSFX`, `DialogueHUD`, `HudMusic`, `InGameMusic` and `TimeTrialMusic`. So the level's music and the menu's clicks play on; the emitters, the one-shots, her breath and the wind, the weapons, the cutscenes' music and all other dialogue are paused and resumed. Nothing is stopped.
- While it lasts every cue is still parsed, with a `DeltaTime` of 0 for the ones that are not UI sounds (0x00B70316: their fades stand still), and `StartSources` neither starts nor updates a wave that is not a UI sound (0x00B6EFE4): a sound asked for meanwhile is still being handed in when the game ticks again, and starts then. A delay stands still with the game's clock. A paused source is not updated (0x010D71B6).
- `TdPlayerController.SetPause` (`TdGame.u`) calls `SetAudioGroupVolume('InGameMusic', 0.25)` when it has paused the game and `SetAudioGroupVolume('InGameMusic', 1.0)` when it has let it go; nothing else in it touches sound. `UAudioDevice::SetGroupVolume` (0x00B6DD00) puts the number in the place of the group's own `Volume`, so the ini's 1.2 is gone with the first call, and the new value reaches the playing music at once unless a fade between two sound modes is under way. `TdPlayerController.ClientRestart` calls `SetPause(false)` every time the controller is given its pawn (`GameInfo.RestartPlayer` and `TdSPStoryGame.RestartPlayer` call `Possess`, `PlayerController.Possess` calls `Restart`, which calls `ClientRestart`): in play the group is at 1.0 from the first spawn, and a pause turns the music down to a quarter of that.
- No script of `TdGame.u` sets the sound mode 4, "Pause"; a native caller was not looked for. The fade between two sound modes runs on real time (0x00B65130), so one begun before a pause finishes under it.

**The port** (`src/audio/audio_engine.cpp`, `src/audio/sound_groups.hpp`):

- *Emitters* (`update_ambient_emitters`): each emitter keeps a payload for every node of its cue and a record for every wave its parses have reached, and its graph is parsed on every update by the rules above (`parse_emitter_node`; `emitter_wave_finished` is the looping node's count). The radii of its attenuation nodes are drawn at their first parse; a wave's gain and high-frequency gain are its own nodes' at the listener's distance; a wave is handed in when its volume is above 0.0001, not at all past its sound group's `MaxUpdateDistance`, and ranked by its volume in the graph with the distance in it (1.0 more for `BigLooped`'s 39 emitters). A wave that keeps its rank among the active ones keeps its source; one that gets a source starts from its beginning; one that has none stays where it is. An emitter whose parse finds nothing playing or waiting stops for good.
- *Sources*: retail ranks every wave for its 32 sources. The port's other sounds (the one-shots of its pool, a voice line, the run wind, the music) are not ranked and have sources of their own; the emitters' waves share what those are using leaves of 32 at that update (`count_other_voices`), on up to 32 sources of their own (fewer if the device refuses them). Equal ranks go to the earlier emitter.
- *Pause* (`set_game_paused`; `game_is_paused` in `src/main.cpp`): under the pause menu, under the port's own overlay menu when it was opened from play, and under a hint card, every pool source that is not playing a sound of a UI sound group, the emitters' sources, the run wind and the voice line are paused and played on afterwards; no delay, timer or emitter moves on and no gain follows the listener meanwhile; a sound asked for meanwhile waits and starts with the first update after it, unless its cue's sound group is a UI one (the menus' `A_HUD` cues are `HudSFX`); the music plays on with its mix as it was, at the `InGameMusic` group's volume, 0.25 then 1.0 (the stems' fixed level stands for 1.0; the engine has the ini's 1.2 only until the first call). The front end is not a pause (`set_menu_music`): the menu's theme plays and the level's sounds are stopped, not paused.
- *Checks*: oracle stage 26 (with stage 23 under `--verify-sound`): a minute of updates under a pause with nothing started, ended, moved on or re-gained, the echo's delay held, the menu's click through and the other requests after it, the music group's three volumes; a vent's two layers, each on its curve, and none past 2000 uu; on The Boat, where about 80 waves ask for the 32 sources at the busiest place, the ones that have one never ranked under one without; no voice that keeps its source restarted as the listener moves and the ranks change; a vehicle pack's rounds, a wind's bed and gusts, a delay above its looping node. `--emitter-walk` counts the emitter voices heard above every emitter of the eleven maps (6,600 places: 121,759 voices where the four nearest emitters gave 17,956, 71 places with none where there were 1,447, more waves asking than there are sources at 1,443). `--audio-device-check` runs the engine on the audio device with no window and the master volume at nothing and asks OpenAL itself for its sources' states across a pause: the emitters', an impact's and the run wind's paused, the music's four playing on, the menu's click starting under the pause, all of them playing again after it.

**Not done.** Nothing was listened to. The one-shots are not ranked, keep a source while out of range and are never stopped at 2.5 times their cue's length; retail has one pool for everything. An emitter's level is its component's `VolumeMultiplier` times a fixed 0.55: its cue's `VolumeMultiplier`, its sound group's `Volume` and its wave's (0.75 on every emitter's wave) are not applied. A random node does not yet avoid repeating an input until all are used (`bRandomizeWithoutReplacement`, on by default). `SoundNodeOscillator` (the transformers, 13 emitters) and `TdSoundNodeVelocity` (the pillars' and iron bars' whoosh, 43) pass through. The mix groups' run-time values are not applied. Only the `*_Aud` sublevels' emitters exist in the port. A tutorial message with `bPauseGame` does not pause the port's game. A paused game for the frames of a level load or a map change is not done.

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

**How far an emitter carries.** Every one of the 1,650 emitters in the `*_Aud` files has a cue with an attenuation node: 881 one, 767 two (a mixer of two layers, each with its own radii). The largest `MaxRadius` of an emitter's cue is 3000 uu at the median, 1000 or less for a tenth of them (fans, vents, lights) and 27000 for the vehicle packs (Logarithmic, 1800 to a `MaxRadius` drawn between 18000 and 27000). The training area's 38 are 24 `WindHard` (Linear 10 / 2000), 9 vehicle packs and 5 `GapWind` (Linear 10 / 1000). Every wave of an emitter's cue is a voice at the gain of its own nodes' curve, and the voices of all the emitters are ranked by their volume for the sources there are (`AudioEngine::update_ambient_emitters`, section 3.3); the port had first given every emitter 200 / 3000 through OpenAL's inverse model, then one wave each to the four nearest. The two `AmbientSoundSimple` actors' cue is a single `SoundNodeAmbient` holding the wave itself; the loader had found no wave in it, so they were silent.

**How an emitter repeats** is its cue's own graph (section 3.3 has the rule and a table of the shapes). A wave straight under a `SoundNodeLooping` loops without a gap (the vents' layers, a wind's bed). The vehicle packs have a `SoundNodeDelay` and a `SoundNodeRandom` between the two: `VehiclePack_02` is `Looping` → `Delay` (7 to 15 s) → `Modulator` → a `SoundNodeRandom` over six groups weighted 7 / 3 / 8 / 8 / 5 / 7 (brakes, buses, cars, horns, motorcycles, trucks; 38 waves). Every round draws the delay again, waits it out, plays one pass once with its own volume and pitch, and the next round starts when that ends: a vehicle every 10 to 20 s, a horn about one time in five. The port parses the emitter's graph as retail does (`AudioEngine::parse_emitter_node`); it had looped the cue's first wave end to end, then played rounds only for a cue whose first wave had a delay.

**Behind the front end the emitters are silent; under a pause they are paused.** Retail's front end is a map of its own, `TdMainMenu`, and it has no `AmbientSound` at all: its Kismet cross-fades the menu music and its UI scenes play `A_HUD` cues (`UIAction_PlaySound`), nothing else. The port loads a level behind its front end and does not play that level's sounds while the front end is up. The pause menu is another thing, a paused game: its sounds are paused where they are and go on from there (section 3.3).
