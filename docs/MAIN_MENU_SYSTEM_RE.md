# Mirror's Edge front end: the "Press Any Key" screen and the main menu

What the retail PC game does between boot and the first menu choice, read out of the shipped packages and scripts. Every value below comes from the retail data unless it says "measured" (taken from a retail frame) or "not established".

Tools used: `tools/ue3_tree.py` (property trees), `tools/uscript_bytecode.py` (script), `tools/retail/` (running the retail game).

---

## 1. Boot flow

1. `DefaultEngine.ini` `[URL] Map=TdMainMenu` / `LocalMap=TdMainMenu` loads `Maps/Menu/TdMainMenu.me1` with `TdGame.TdMenuGameInfo`.
2. The level's Kismet (`SeqEvent_LevelLoaded`) opens the UI scene `tdstart` (class `TdUIScene_Start`), the "Press Any Key" screen.
3. Any key released after `TimeTillStartButton` (4 s) hides the start scene's `SafeRegionPanel`, runs the profile and save-file checks, then `StartGame()` opens the scene `TdMainMenu` (class `TdUIScene_MainMenu`).
4. Leaving the start screen idle for `TimeTillAttractMovie` (90 s) plays `Movies/Attract_Movie.bik`; a key stops it.

Both scenes live in `UI/TdUI_FrontEnd.upk` and are cooked into `TdMainMenu.me1` as forced exports. They are authored at 1280x720 (`CurrentViewportSize`).

`[TdGame.TdUIScene_Start]` in `DefaultUI.ini`: `MovieName="Attract_Movie"`, `TimeTillAttractMovie=90`, `TimeTillStartButton=4`, `bGoToLoadGame=false`. `[TdGame.TdUIScene] SceneAnimDuration=0.25`.

---

## 2. The start scene (`tdstart`, `TdUIScene_Start`)

Widget tree, positions in pixels at 1280x720:

| Widget | Class | Left, Top, Right, Bottom | Notes |
|---|---|---|---|
| `SafeRegionPanel` | `UISafeRegionPanel` | 96, 54, 1184, 666 | 7.5% inset each side, `bForce16x9AspectRatio` |
| `ContentPanel` | `UIPanel` | 96, 54, 1184, 666 | docked to all four faces of the safe region; background image is empty |
| `TitleImage` | `UIImage` | 96, 54, 1184, 666 | `TdUIResources.Scene.StartTitleImage`, `ADJUST_Justified`, centred both ways |
| `PressStartLabel` | `UILabel` | 96, 115.2, 1184, 666 | top docked to the panel top with 10% of the panel height as padding; `bHidden` until the timer |
| `CopyrightLabel` | `UILabel` | 96, 631.0, 1184, 666 | top docked to the panel *bottom* with -5.7156% padding |

Both labels use the skin style `TdLabelTextCommonTextBold`: font `UI_Fonts_Final.Helvetica_Small_Bold`, style colour (0, 0.0037, 0.0278), but each label overrides it with `DrawColor = (1, 0, 0, 1)` and centres the text on both axes.

Script (`TdUIScene_Start`):

* `SceneActivated`: `Opacity = 0`; the label text becomes `Localize("TdStart", IsConsole() ? "PressStartText" : "PressAnyKeyText", "TdGameUI")`, which is **"Press Any Key"** on PC; the label is hidden.
* `HandleInputKey`: reacts to `IE_Released` only. If the attract movie is playing, the key stops it. Otherwise, once `TimeElapsedInScene >= TimeTillStartButton`, it hides and disables `SafeRegionPanel` and calls `CheckProfile()` → `InitSavefileSystem()` → `StartGame()` → `OpenScene(TdMainMenu)`.
* The copyright line is `TdStart.CopyrightText`: "© 2009 EA Digital Illusions CE AB. All rights reserved."
* The tick that advances `TimeElapsedInScene`, fades `Opacity` in, shows the label and starts the attract movie is native (`MirrorsEdge.exe`), so its timing is measured in section 8.

---

## 3. The main menu scene (`TdMainMenu`, `TdUIScene_MainMenu`)

Four columns, `ETdMainMenuPanel`: `TDMMP_STORY`, `TDMMP_TIMETRIAL` (caption "RACE"), `TDMMP_OPTIONS`, `TDMMP_EXTRAS`.

### 3.1 Layout (pixels at 1280x720)

| Widget | Left, Top, Right, Bottom | Notes |
|---|---|---|
| `SafeRegionPanel` | 96, 54, 1184, 666 | |
| `PanelButtonsPanel` | 96, 482.4, 1184, 584 | top = safe bottom - 30% of safe height; height 16.6% |
| `StoryCaptionButton` / `BigStoryCaptionButton` | 96, 482.4, 368, 583.8 | each caption is a quarter of the panel: 272 px |
| `TimeTrialCaptionButton` / `Big…` | 368, 482.4, 640, 583.8 | |
| `OptionsCaptionButton` / `Big…` | 640, 482.4, 912, 583.8 | |
| `ExtrasCaptionButton` / `Big…` | 912, 482.4, 1184, 583.8 | |
| `StoryPanel`, `TimeTrialPanel`, `OptionsPanel`, `ExtrasPanel` | caption left/right, 54, 482.4 | one per column, above its caption |
| sub-buttons | panel left/right, 53.55 px tall | stacked upward from the panel bottom; each is 12.5% of the panel height |
| `DescriptionLabel` | 748.8, 54, 1184, 176.4 | left padding 60% of the safe width, bottom padding -80% |
| `ButtonBar` | 96, 635.4, 1184, 666 | bottom 5% of the safe region; buttons right-aligned, 50 px apart |
| `StoryPanelBGImage` … `ExtrasPanelBGImage` | panel left - 27.2, 0, panel right + 27.2, 720 | on `ScreenRegionPanel` (full screen); 10% of the panel width wider on each side |

Sub-buttons, bottom row first (`Top` values 428.85, 375.3, 321.75, 268.2, 214.65):

| Column | Buttons, bottom to top | Hidden on PC when |
|---|---|---|
| STORY | `NewGameButton` "NEW GAME", `LoadLevelButton` "PLAY CHAPTER", `LoadGameButton` "CONTINUE GAME" | CONTINUE without a save (`CanContinueGame`); PLAY CHAPTER until level 0 is unlocked |
| RACE | `LeaderboardsButton` "LEADERBOARDS", `TimeTrialOnlineButton` "TIME TRIAL", `LevelRaceButton` "SPEED RUN" | SPEED RUN until every level is unlocked |
| OPTIONS | `GameSettingsButton` "GAME SETTINGS", `ControlsButton` "CONTROLS", `AudioButton` "AUDIO", `VideoButton` "VIDEO", `GamepadButton` "GAMEPAD SETUP" | GAMEPAD SETUP unless a controller is connected |
| EXTRAS | `CreditsButton` "CREDITS", `UnlocksButton` "UNLOCKABLES", `XBoxAchievementsButton`, `DownloadsButton` | ACHIEVEMENTS and DOWNLOADABLE CONTENT always (not a console) |

`TrainingButton` in the STORY panel has an empty caption and no handler. A hidden button leaves a gap in neither direction: the rows above it keep their docked positions, so e.g. OPTIONS without a controller shows four rows starting at 268.2.

Up/Down wrap inside a column through `ForcedNavigationTarget`s.

### 3.2 Text styles (skin `UI_Skins.UI_Skins_TDUISkins2`)

| Text | Style | Font | Colour | Drop shadow |
|---|---|---|---|---|
| sub-buttons | `TdMainMenu_Sub-Button` | `Helvetica_Medium_Italic` | white | `TdMainMenu_Sub-Button_DropShadow`: (0, 0.078, 0.227, 0.5), `VerticalPctOffset` 0.1 |
| small captions (unselected columns) | `TdLabelText_MenuButton_Normal` | `Helvetica_Medium_Italic` | white | `TdLabelText_MenuButton_DropShadow`: alpha 0.7, vertical 0.1; centred |
| big caption (selected column) | `TdLabelTextTitleThickWhite` | `Helvetica_Headline_Light_Italic` | white | `TdLabelTextTitleThickDropShadow`: alpha 0.5, vertical 0.055, horizontal 0.035; centred |
| description | `TdLabelText_CommonText` | `Helvetica_Small_Normal` | (0, 0.0037, 0.0278) | `TdLabelText_Common_Text_DropShadow_Light`: alpha 0.34; right-aligned, wrapped |

Colours are linear. Fonts are `MultiFont`s in `UI/UI_Fonts_Final.upk` (section 5).

### 3.3 Behaviour (`TdUIScene_MainMenu`)

* First tick: `SetActivePanel(0)` (STORY) unless a scene is being restored.
* `SetActivePanel(i)`: sets `bIsAnimatingPanel`, hides all four panels and shows all four small captions, tells the panel renderer, fires the level event `panel<i+1>` (Kismet moves the camera), shows `DescriptionLabel` only for columns 0 and 1, zeroes `FadeTimer` and plays the UI sound `TabChangeRight`.
* `PanelAnimFinished(i)`: for the active column, shows its panel (focus goes to its first enabled child), swaps the small caption for the big one and clears `bIsAnimatingPanel`.
* `HandleInputKey`: `Left`/`Right` pressed → `SwitchTab(∓1)`, which wraps modulo 4. `Escape` released → `OnQuitGame()`. Clicks are ignored while `bIsAnimatingPanel`.
* `HandleButtonClicked` fires the level event `<ButtonName>_Clicked`, opens the matching scene and plays `Accept`.
* `Tick`: `DescriptionLabel.Opacity = clamp((FadeTimer - TimeToFadeStart) / FadeTime, 0, 1)` with `TimeToFadeStart = 2`, `FadeTime = 1`. `FadeTimer` restarts whenever a button gains focus or the column changes, so the description fades in two seconds after the selection settles.
* Button bar on PC: "Friends" and "Quit", plus "Accept" when a controller is connected.

### 3.4 The red columns are a material (`TdMenuPostProcesWrapper`)

The four `*PanelBGImage` widgets are not textures. `TdMenuPostProcesWrapper.Initialize` gives each one its own `MaterialInstanceConstant` of `UI_Menus.M_MainMenuStick_01` (translucent, unlit) and drives it with parameters:

| Parameter | Set to |
|---|---|
| `StickWidth` | `CosineInterp(MinWidth, 1, AnimPosition / PanelAnimDuration)`; `MinWidth` = `UnfocusedPanelBGWidth` = 0.02, `PanelAnimDuration` = 0.3 s. `AnimPosition` runs up while the column is active and back down when it is not. |
| `SelectTop`, `SelectBottom` | the focused button's top and bottom as a fraction of the screen height (the white selection bar) |
| `SelectOpacity` | 1 once the active column's animation has finished, else 0 |
| `MovementOffset` | `PanelIndex / 4` |
| `MovementAmount` | 0 for the active column, 1 for the others |
| `LeftSideOffset`, `RightSideOffset` | `FRand()` each, once per boot |
| `SelectColor` | `SelectionColor` (class default) |

The material, with `uv` running 0..1 across the widget and `t` the unpaused time in seconds:

```
move   = (T_StickMovementTimeline_01(MovementOffset*5 + t*0.015).g - 0.5) * 5 * MovementAmount
right  = T_StickMaskRight_01( 40u - (StickWidth*0.9*20 + 20 + move),
                              0.15v + 0.15*(RightSideOffset*5 + t) )
left   = T_StickMaskLeft_01 ( 40u - (20*(1 - StickWidth*0.9) - 1 + move),
                              0.15v - 0.15*(LeftSideOffset*5 + t) )
shadow = T_StickMaskRightShadow_01(right's coordinates - (0.5, 0))

inField  = saturate(saturate(SelectTop*512 - v*512) + saturate(v*512 - SelectBottom*512)
                    + (1 - SelectOpacity))
Emissive = lerp(ShadowColor(0.1, 0.2, 0.4), lerp(SelectColor, StickColor(0.91575, 0, 0), inField), right.g)
Opacity  = saturate(right.rgb + shadow.rgb) * left.rgb
```

So a column is centred on its widget and `0.9 * StickWidth` of the widget wide: 293.8 px when open, 5.9 px as a "stick". The two edge masks scroll vertically in opposite directions, which is why the edges are never quite straight and never still, and an unselected stick drifts up to ±20 px sideways along a timeline texture. A blue-grey shadow falls to the right.

---

## 4. The level (`Maps/Menu/TdMainMenu.me1`)

| Actor | What |
|---|---|
| `StaticMeshActor_14/15/16/17/0` | `UI_City.S_City_01` … `S_City_05`, the city, at (0, -960, 0) |
| `StaticMeshActor_4`, `_5`, `_2` | `S_CityBase_01`, `S_CityBaseMountains_01`, `S_CityBaseWater_01` at (0, -960, 0) |
| `StaticMeshActor_7` | a second `S_CityBaseWater_01` at (0, -1616, 1) with `M_CityWaves_01`, no shadow |
| `StaticMeshActor_1` | `S_Skydome_Menu`, scale 33.9562, yaw -8192 |
| `SceneCaptureReflectActor_0` | planar reflection at (0.15, -956, 0), scale 0.2, into `UI_City.T_CityReflection_01_R` (the water) |
| `DirectionalLight_0` | rotation (-2750, 24554, 35134), baker colour (255, 245, 225), brightness 2; lighting is baked into 32 lightmaps |
| `CameraActor_0` … `_11` | twelve viewpoints (section 6) |
| `InterpActor_0` … `_11` | twelve hidden `UI_City.CC_Target` meshes, scale 5.23: the cameras' look-at targets |

Materials in `UI_City`: `M_CityBuildings_01`, `M_CityBase_01`, `M_CityReflection_01`, `M_CityWaves_01`, `M_Skydome_Menu`, and one instance per chapter `MI_SP00_01` … `MI_SP09_01` (the `Selected` parameter lights a district on the chapter-select screen, not here).

`WorldInfo.DefaultPostProcessSettings`: `Bloom_Scale` 0.15, `DOF_BlurKernelSize` 50, `DOF_MaxNearBlurAmount` 0.2, `DOF_FocusType` `FOCUS_Position`, `DOF_FocusInnerRadius` 22500, `Scene_ExposureManual` 0.83, and a 16-segment per-channel tone curve (`Curves.Ms` / `Bs`).

---

## 5. Fonts (`UI/UI_Fonts_Final.upk`)

The front end uses four `MultiFont`s: `Helvetica_Small_Bold` (start screen), `Helvetica_Small_Normal` (description), `Helvetica_Medium_Italic` (sub-buttons, small captions) and `Helvetica_Headline_Light_Italic` (the selected column's caption).

A `MultiFont` is three fonts in one, for 480, 720 and 1080 lines (`ResolutionTestTable`). The engine uses the tier whose height is nearest the viewport's and scales glyphs by `viewport height / tier height`, so at 720p the middle tier is drawn 1:1.

* `Characters`: 3 x 256 `FontCharacter`s of 21 bytes: `StartU`, `StartV`, `USize`, `VSize` (int32), `TextureIndex` (byte), `VerticalOffset` (int32). Tier *n* is entries `256n .. 256n+255`, indexed by Latin-1 code.
* `Textures`: the atlas pages, 256 wide, `PF_DXT5`; the glyph is in the alpha channel.
* `Kerning` (property, 1 on the `Small` fonts, else 0): pixels added after every glyph.
* Native tail: `int32 CharRemap count` (0), `int32 N`, then `N` kerning pairs `{uint16 first, uint16 second, float amount}`, then an `int32` array of 4 offsets splitting those pairs between the three tiers (e.g. 0, 209, 524, 933).

Middle-tier (720p) line heights: `Small_Normal` 24, `Small_Bold` 25, `Medium_Italic` 26, `Headline_Light_Italic` 55. A capital is 15 px tall in the first three and 32 px in the headline font.

`UIComp_TdDropShadowString` draws its string twice, the shadow first, offset by `HorizontalPctOffset` and `VerticalPctOffset` (class defaults 0.06 each) in the shadow style's colour.

## 6. The camera (Kismet `Main_Sequence` and its Matinees)

One camera, `CameraActor_0`, is used for the start screen and all four columns. Every Matinee has a `Camera` group and a `Target` group (bound to the hidden `InterpActor_0`); the camera's move track has `RotMode = IMR_LookAtGroup`, `LookAtGroupName = Target`, so its rotation is always "look at the target" and its `EulerTrack` is unused. Positions are world space. An `InterpTrackFloatProp` drives `FOVAngle`.

Curve segments follow UE3's `FInterpCurve::Eval`: the *earlier* key's `InterpMode` picks the segment type, `CIM_Linear` is a lerp, and the curve modes are a cubic Hermite over the earlier key's `LeaveTangent` and the later key's `ArriveTangent`, both multiplied by the segment's duration.

### 6.1 Start screen: the opening shot (`InterpData_17`, 60 s, looping)

Started once by `SeqEvent_SequenceActivated_2`, together with a 2 s `SeqAct_TdFadeEffect` from white.

| | t = 0 | t = 60 |
|---|---|---|
| Camera | (-224.4, -2375, 47.41), leave tangent (1.073, 36.23, 0), `CurveUser` | (-193.7, -1066, 47.41), tangents 0 |
| Target | (-144, 80, 848), leave (1.366, 52.42, -0.2675) | (-112, 1408, 832), arrive (0, 0, -0.2589) |
| FOV | 90 | |

A low, slow push across the water toward the city, looking up at the skyline. An event key `StartFade` at t = 58 fades to white over 2 s (`SeqAct_TdFadeEffect`, `FadeOut`), then back in over 2 s, hiding the loop point.

### 6.2 The four columns

`panel<N>` first stops every other column's Matinees. If the `Sub_Menu` flag is clear it plays the column's 0.35 s intro and, on `Completed`, its 60 s loop; if set (coming back from a sub-menu) it goes straight to the loop. The very first `panel1` (flag `First_time`) also stops the opening shot.

Intro Matinees (0.35 s). Camera and target each move between two keys; FOV is a lerp:

| Column | Camera from → to | Target from → to | FOV |
|---|---|---|---|
| STORY (`InterpData_21`) | (-164, -1878, 47.41) → (404.7, -229.3, 112.9) | (-144, 80, 192) → (-992, -800, 288) | 110 → 70 |
| RACE (`InterpData_36`) | (185.8, -976, 112.9) → (-843.2, -1525, 176.9) | (-800.4, -694.8, 207.4) → (335.6, -806.8, 175.4) | 110 → 90 |
| OPTIONS (`InterpData_32`) | (-887.2, -546.9, 180.3) → (1.872, -1337, 104.5) | fixed (511.6, -918.8, 111.4) | 110 → 80 |
| EXTRAS (`InterpData_33`) | (-237.8, -213.3, 137.8) → (1143, -21.51, 163.9) | (447.6, -454.8, 111.4) → (-784.4, -166.8, 175.4) | 90 → 60 |

Loop Matinees (60 s, `bLooping`):

| Column | Camera keys | Target keys | FOV |
|---|---|---|---|
| STORY (`InterpData_18`) | 0: (404.7, -229.3, 112.9); 35.5: (246.1, -1360, 112.9); 59.5: (94.9, -1771, 112.9); 60: back to the first | 0: (-992, -800, 288); 59.5: (-1072, -1088, 288); 60: back | 70 |
| RACE (`InterpData_23`) | 0: (-843.2, -1525, 176.9); 59.5: (-1031, -612.1, 252) | 0: (335.6, -806.8, 175.4); 59.5: (271.6, -422.8, 175.4) | 90 |
| OPTIONS (`InterpData_27`) | 0: (1.872, -1337, 104.5); 60: (65.91, -677.2, 104.5) | 0: (511.6, -918.8, 111.4); 60: (575.6, -470.8, 111.4) | 80 → 90 |
| EXTRAS (`InterpData_57`) | 0: (1143, -21.51, 163.9); 60: (1038, -616.9, 147.9) | fixed (-784.4, -166.8, 175.4) | 60 |

The full keys with tangents and modes are what `src/ui/frontend/` carries; `build/re/interp.py`-style dumps reproduce them.

Clicking a sub-button sets `Sub_Menu` and plays a 0.5 s "sub menu camera" Matinee; those belong to the sub-menu screens and are not covered here.

---

## 7. Sound

* **Music.** `SeqEvent_LevelLoaded` fires the remote event `DefaultMenuMusic`, which streams in the sublevel `Maps/Menu/TdMainMenu_Audio0.me1`. That sublevel's Kismet puts one track in the level's `MusicBank` and fires `PlayMenuMusic`, which cross-fades to track type `ambientMusic`:
  * cue `A_M_Menu.Menu.Menu`: `SoundGroup = HudMusic`, `VolumeMultiplier = 0.6`, a `SoundNodeLooping` over one wave;
  * wave `A_M_Menu.RAW.A_M_Menu`: Ogg Vorbis, stereo, 44.1 kHz, 110.782 s (also in `Audio/A_M_Menu.upk`);
  * `FadeInTime = 2`, `FadeOutTime = 2`, `bAutoPlay`.
  It starts on the start screen and plays through the menu. `TdMainMenu_Audio1` … `_Audio20` hold the unlockable tracks, one per sublevel.
* **UI sounds** (skin `SoundCues`, all in `Audio/A_HUD.upk`): `NavigateUp/Down/Left/Right`, `ListUp/Down`, `Slider*` → `A_HUD.Menu.D-Pad`; `TabChangeLeft/Right` → `A_HUD.Menu.Tab_Change`; `Accept` → `A_HUD.Menu.A_Pos`; `Cancel` → `A_HUD.Menu.B_Neg`; `Default` → `A_HUD.Menu.Accept`.
  Changing column plays `TabChangeRight` whichever way it goes; moving between buttons plays `NavigateUp`/`NavigateDown`; choosing one plays `Accept`.

---

## 8. Measured from retail frames

Taken from the retail game's back buffer at 1280x720 with `tools/retail`'s `d3d9` hook, which does not touch the game's input.

* The open STORY column is solid from x = 82 to 382 and its edges wander by a pixel or two over a minute; the three sticks are 5 to 9 px wide and drift about 10 px either way. Both agree with section 3.4.
* Column red is (233, 0, 0). `StickColor` is 0.91575, and 0.91575 x 255 = 233.5: the UI's material colours reach the screen without a gamma step.
* The shadow to the right of a column is `ShadowColor` at about 44% over the background, 4 px wide, then fades out over 3 more.
* The selection bar is pure white from the column's left edge to its right edge, exactly the focused button's top and bottom (321.75 to 375.3 for CONTINUE GAME).

Not established: the start scene's native tick (the fade-in rate and whether "Press Any Key" pulses), because that needs a restart of the game on its start screen.
