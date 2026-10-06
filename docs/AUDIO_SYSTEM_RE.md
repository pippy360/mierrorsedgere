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
   - Adds `bDelay` + `SpeedOfSound` (`RawDistributionFloat`) on top of UE3 `MinRadius` / `MaxRadius` / `LPFMinRadius` / `LPFMaxRadius`.
4. **`TdSoundNodeDelayToDistance` (`extends SoundNode`)**:
   - Computes realistic acoustic propagation delay from emitter to listener:
     $$\Delta t_{\text{delay}} = \frac{\min(\|\mathbf{x}_{\text{src}} - \mathbf{x}_{\text{listener}}\|, \text{MaxDistance})}{\text{SpeedOfSound}}$$
     with `MaxDistance = 100000.0` UU (`1000 m`, `SpeedOfSound = 34300.0` UU/s).
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
- `SoundNodeAttenuation` (`MinRadius`, `MaxRadius`, `LPFMinRadius`, `LPFMaxRadius` via `DistributionFloatUniform` subobjects).
