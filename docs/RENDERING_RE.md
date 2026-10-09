# Rendering: what retail draws and how the port draws it

How Mirror's Edge lights and finishes a frame, worked out from the game's own files, and what the port
does with that. It covers the baked lighting, the fog, and the post-process chain (haze, bloom, exposure,
tone mapping, the screen fade). Materials themselves are in [`MATERIAL_SYSTEM.md`](MATERIAL_SYSTEM.md).

The work was measured against retail pictures taken from the same camera (section 9). Over the ten level
intros the difference between the two games' pictures fell from 68.5 to 31.9 on the scale described there,
and what is left is mostly things other than lighting and tone: the first-person body, motion blur, objects
the port does not draw.

## 1. Where the knowledge comes from

| Source | What it gives |
|---|---|
| `Engine/Shaders/*.usf` in the retail install (113 files, shipped as source) | The exact arithmetic of every pass: `BasePassPixelShader.usf`, `HeightFogCommon.usf`, `TdDirHazePixelShader.usf`, `DOFAndBloomGatherPixelShader.usf`, `DOFAndBloomBlendPixelShader.usf`, `FilterPixelShader.usf`, `TdToneMapExposurePixelShader.usf`, `TdToneMappingPixelShader.usf`, `TdMotionBlurShader.usf` |
| `TdGame/CookedPC/Effects/FX_PostProcess.upk` | The chain: which effects run, in what order, with what values |
| `Engine.u`, `TdGame.u` | The defaults of `PostProcessSettings`, and the scripts that drive the fade (`TdHUD`, `SeqAct_TdFadeEffect`) |
| The level packages | Light maps, lights, `WorldInfo.DefaultPostProcessSettings`, `PostProcessVolume`s, `HeightFog` actors |
| `MirrorsEdge.exe` | What the shaders are handed: the fog constants, the exposure's metering. Addresses are given where a fact was read there |
| Retail recordings | A back buffer a second through every level intro (`tools/retail/intro_capture.py --frames`), to check all of the above |

The user's `TdEngine.ini [SystemSettings]` on the machine the recordings were made on: `DirectionalLightmaps=True`,
`Bloom=True`, `QualityBloom=False`, `DepthOfField=True`, `TdSunHaze=True`, `TdTonemapping=True`,
`TdBicubicFiltering=True`, `TdMotionBlur=True`, `MotionBlur=False`, `AmbientOcclusion=False`, `LensFlares=True`,
`FogVolumes=False`.

## 2. A retail frame

1. **Base pass.** Every opaque surface writes linear, unbounded scene colour into a 16-bit float buffer:
   its light map through the material's diffuse and specular, plus what it emits. There is no forward sun,
   no ambient term and no shadow map for level geometry: the light is all in the light maps.
2. **Height fog**, a full-screen pass that reads the depth buffer.
3. **Translucency**, then the first-person body in its own depth range.
4. **The post-process chain** of `FX_PostProcess.FX_PostProcess`, in this order (effects marked off are
   `bShowInGame=False` until script turns them on):

   | # | Effect | Notes |
   |---|---|---|
   | 1 | `TdDirectionalHazePostProcess` | world settings |
   | 2 | `DOFAndBloomEffect` | its own values, not the world's: `BloomScale` 0.15, blur kernel 16, both blur clamps 0 (no depth of field) |
   | 3 | `DeathEffect`, `Slideshow`, `MotionBlurEffect`, `ReactionTimeEffect` | off |
   | 4 | `TdToneMappingPostProcess` | world settings: exposure, shadows / midtones / highlights, desaturation, curves |
   | 5 | `HealthEffect`, `MeleeEffect` on; taser, fall, explosion, flashbang, falling, laser, scope off | material effects, on the tone-mapped picture |
   | 6 | `FadeInEffect` | off until `TdHUD` fades; section 7 |
   | 7 | `SaturationFilter` | off |
   | 8 | `TdCalibrationPostProcess` | draws only its calibration squares |
   | 9 | `TdMotionBlurPostProcess` | amount 0.6, from 400 uu/s |

   There is no ambient occlusion effect in the chain, though the shader ships.

## 3. Baked lighting

Mirror's Edge was lit offline with Beast. Every light of a level has `bHasLightEverBeenBuiltIntoLightMap`,
no component has a static shadow map, and the lights left dynamic (sky lights, the cinematic key lights) are
on channels the level geometry is not.

### 3.1 Static meshes

After a `StaticMeshComponent`'s tagged properties comes its `LODData`:

```
int32 count
per LOD:  int32 n, n object refs   ShadowMaps
          int32 n, n object refs   ShadowVertexBuffers
          int32 type               0 none, 1 FLightMap1D, 2 FLightMap2D
          the light map
```

**`FLightMap2D`**: `int32 n, n GUIDs` (the lights in it; always empty in the cooked game), then four times
`{object ref texture, float3 scale}` (three directional coefficients, then the simple light map), then
`float2 CoordinateScale`, `float2 CoordinateBias`. The textures are `LightMapTexture2D` exports of the same
package: DXT1, sRGB, atlases shared by many components. A texel is `texture.rgb * scale`.

**`FLightMap1D`**: `GUIDs`, `object ref Owner`, bulk data (`flags, count, bytes, offset`, then `count` samples
of three `FColor`s, B G R A), three `float3` scales, the simple samples. A sample is `pow(c, 2.2) * scale`, one
per mesh vertex in the order of the mesh's own vertex buffer.

The mesh's light-map UV set is `StaticMesh.LightMapCoordinateIndex`; the place in the atlas is
`uv * CoordinateScale + CoordinateBias`.

### 3.2 BSP

A level's BSP is a `Model` drawn by `ModelComponent`s. After a component's tagged properties:

```
object ref Model, int32 ZoneIndex
int32 n, n FModelElement
uint16 ComponentIndex
int32 n, n uint16 Nodes

FModelElement:  light map (type, then as above)
                object ref Component, object ref Material
                int32 n, n uint16 Nodes
                int32 n, n {GUID, object ref}   ShadowMaps
                int32 n, n GUID                 IrrelevantLights
```

Every element has a light map of its own. The model's cooked vertex buffer (36 bytes a vertex: position,
two packed tangents, `TexCoord`, `ShadowTexCoord`) carries the light-map coordinate as `ShadowTexCoord`,
0..1 across the element's rectangle, placed in the atlas by the element's scale and bias. An element baked no
light map (a few surfaces a level, mostly `DefaultMaterial`) takes no light.

### 3.3 What the shader does with it

`BasePassPixelShader.usf`, with `DirectionalLightmaps`: the three coefficients are the light arriving along
the three directions of the Half-Life 2 basis in tangent space,

```
(0, 0.8165, 0.5774)   (-0.7071, -0.4082, 0.5774)   (0.7071, -0.4082, 0.5774)
```

and the material's diffuse and specular are weighted by its normal and reflection vector against each. With
`TdBicubicFiltering` the textures are read through a bicubic B-spline (four bilinear taps).

### 3.4 In the port

`src/assets/level_lightmaps.*` reads all three layouts. A vertex carries `lm_u, lm_v` and three scales packed
as RGB9E5 (`Vertex` is 80 bytes); world geometry is binned by material and light-map texture set, and the
three textures of a section's set are bound at texture slots 24 to 26. Vertex light maps travel in the same
three packed values, with `lm_u = -1`. Level geometry nothing was baked for (the sky dome, cards of the far
skyline) also has `lm_u = -1` with samples of nothing: it shows what it emits. `lm_u = -2` is geometry that
is not the level's (movers, characters, the first-person body), which still takes the stand-in described in
section 10.

Light maps are loaded at full size (a level's first checkpoint has 30 to 180 sets, 50 to 290 MB).

## 4. Scene colour and depth

Materials write linear light with no upper bound, as retail's do; everything that shapes the picture comes
after, on the whole frame. The far plane is at 10,000,000 uu: sky domes are meshes about 840,000 uu out
(a mesh of radius 128 at `DrawScale` 6577) and were being clipped by the old one.

## 5. Height fog

`HeightFogCommon.usf` takes four layers. For each: the part of the view ray inside the layer's slab, less
`StartDistance`, gives `exp2(FogDistanceScale * distance)`, and past `ExtinctionDistance` the layer is opaque.
The layers are then composed in order: `fog = fog * scattering + (scattering - 1) * FogInScattering`.

What fills those constants is native (`0x0104d0e0`, UE3's `InitFogConstants`). The scene keeps its fogs
sorted highest first and uses the first four. For the fog at place `i` of that list:

```
FogMaxHeight     = Height[i]
FogMinHeight     = Height[i + 1], or -262144 for the last
FogDistanceScale = log2(1 - Density)
FogInScattering  = LightColor * LightBrightness / ln(0.5)        (LightColor through pow 2.2)
FogStartDistance = max(0, StartDistance)
```

So a fog fills the slab from its plane down to the next fog's plane, whichever side the eye is on. The order
the layers are handed over is back to front: the fogs under the eye from the lowest up, then the fogs over it
from the highest down.

This matters wherever a level has fogs for its depths: Stormdrain has four, two of them black, for the drain.
Taking a fog as "the half space on the eye's side of its plane" (as this port first did) pulled the drain's
fog over the sky.

A `HeightFog` actor's fog plane is its `Location.z`; `HeightFogComponent` defaults are `Density` 0.00005,
`LightBrightness` 1, white, no start distance, `ExtinctionDistance` 100,000,000.

## 6. The post-process chain

### 6.1 Haze (`TdDirHazePixelShader.usf`)

A glow towards `HazeSunLocation` that grows with distance, added to the scene:

```
depth = clamp(pow(min(65535, distance) / HazeDistanceDivider, HazeDistanceCurve), 0, 500)
sun   = pow(clamp((dot(toSun, viewRay) + HazeAngleStart) / (1 + HazeAngleStart), 0, HazeAngleClampHigh), HazeAngleCurve)
scene += HazeMultiplier * clamp(sun * HazeColor * depth, HazeTotalClampLow, distance > FarDistance ? FarHigh : CloseHigh)
```

### 6.2 Bloom (`DOFAndBloomGather`, `Filter`, `DOFAndBloomBlend`)

Into a buffer a quarter of the view each way: a pixel blooms if any channel is above 1 and it is nearer than
60,000 uu; the gathered value is `scene * BloomScale / 4`. Two passes of a Gaussian of variance equal to its
radius, `kernel * (view width / 1280) / 4` texels, taken two texels at a time with bilinear taps
(`Compute1DGaussianFilterKernel`). The blend back: `(scene * focus + 4 * blur.rgb) / max(focus + 4 * blur.a, 0.001)`,
where with no depth of field `focus` is 1 and `blur.a` 0.

### 6.3 Tone mapping (`TdToneMappingPixelShader.usf`)

```
c = pow(saturate(scene * exposure) / Scene_HighLights - Scene_Shadows, Scene_MidTones)
c = c * (1 - Scene_Desaturation) + dot(c, (0.3, 0.59, 0.11)) * Scene_Desaturation
c = pow(saturate(c), 1 / 2.2)
c = c * Ms[segment] + Bs[segment]        segment = trunc(c * 15), per channel
```

`Curves` (16 slopes `Ms` and 16 intercepts `Bs` per channel) is the grade the level's artists drew; every
`WorldInfo` and every volume has one. `exposure` is the square of the adapted value times
`Scene_ExposureManual` (next).

### 6.4 Exposure (`TdToneMapExposurePixelShader.usf`, and the executable)

The shader, once a frame, into a 1 x 1 buffer that holds `exposure^2 * Manual / 64`:

```
luminance = dot(SceneDownsampledTexture(0.5, 0.5), (0.3, 0.59, 0.11))        (0.25 if red is 0 or not a number)
target    = clamp(sqrt(0.25 / luminance), Scene_ExposureLow, Scene_ExposureHigh)
a         = |target - last|
next      = last + clamp((target - last) * a, -MaxDeltaDown * a * a, MaxDeltaUp * a * a)
```

so the pace falls with the distance left to go: the last tenth of a stop takes seconds.

What the shader does not say, read from the executable:

- **What it meters** (`0x012d65bf`, the downsample at `0x012cffa0`). The scene colour as the tone mapper is
  given it (haze and bloom in), shrunk through `DownSampleBuffer512`, `128`, `32`, `8`, `2` to a 1 x 1
  `ToneMapingExposureBuffer`: each step averages n x n taps a source texel apart, n the reduction rounded, at
  most 4. All of these buffers are `PF_A16B16G16R16` (pixel format 20, `0x00ceb3e3` on): 16-bit fixed point.
  **Nothing in the picture counts for more than 1.** A sky at 6 weighs as 1, which is why the game exposes
  brighter than a true average would: metering the unclamped scene left the port about a third of a stop dark.
  Retail spreads the six steps over six frames; the port runs them every frame.
- **The speeds** (`0x012d3b1e`): `MaxDeltaUp = dt * min(Scene_ExposureSpeedUp, 2.5)`,
  `MaxDeltaDown = dt * min(Scene_ExposureSpeedDown, 3)`, `dt` in real seconds.
- **The start** (`0x012d6993`): both exposure buffers are first filled with 0.25, an exposure of 4, which the
  clamp turns into `Scene_ExposureHigh`. `TdHUD.PostBeginPlay` asks for that reset
  (`WorldInfo.SetSceneExposureReset`), so every level opens at its brightest and comes down.

### 6.5 The settings: world and volumes

`WorldInfo.DefaultPostProcessSettings` is in force unless the view is inside a `PostProcessVolume`, in which
case the enabled volume of the highest `Priority` that holds the view point wins. The story maps have 168
volumes; all carry curves, most change the exposure clamps.

A volume's `Settings` start from the defaults of the `PostProcessSettings` struct, not from the world's:

```
Scene_ExposureLow 0.85   Scene_ExposureHigh 1.65   Scene_ExposureSpeedUp 3.5   Scene_ExposureSpeedDown 4.5
Scene_ExposureManual 1   Scene_InterpolationDuration 1   HazeEnabled False
```

So the volume around the room Pirandello Kruger opens in, which sets nothing but its curves, also turns the
haze off and opens the exposure from the level's 0.6..0.7 to 0.85..1.65. That is the difference between that
room being dim and being the bright place it is in the game.

When the volume in force changes, the settings go over to the new ones in a straight line over the new ones'
`Scene_InterpolationDuration` (`ULocalPlayer::UpdatePostProcessSettings`); switches change at once.

Whether a view point is inside a volume is tested against the convex pieces of its brush
(`BrushComponent.BrushAggGeom.ConvexElems`). Six volumes ship switched off (`bEnabled=False`), for Kismet to
toggle; the port leaves them off. The per-platform modifiers (`DefaultPostProcessSettingsModifierXbox360`,
`...PS3`) do not apply on PC: the one PC modifier in the game (Flight) is the identity.

## 7. The screen fade

`TdHUD` keeps `FadeAmount` between 0 (all `FadeColor`) and 1 (the picture) and hands it to the chain's
`FadeInEffect`, a material effect that runs after the tone mapping:

```
away   = 1 - FadeInAmount                 weight = pow(away, 1.5)
diff   = FadeColor - picture
near   = saturate((1 - saturate(0.577 * dot(diff, diff))) * 2.5 * away)
result = lerp(lerp(picture, FadeColor, near), picture + diff * weight, weight)
```

What is already near the colour goes first. What moves `FadeAmount`:

- `TdHUD.PlayerOwnerRestart`: white, `FadeAmount` forced to 0, back in over 1 s. Every start and restart.
- `SeqAct_TdFadeEffect` (Kismet): `FadeIn` or `FadeOut` over `FadeTime` (0.5 s unless set) towards a colour
  whose channels are its `FadeColor`'s divided by 255 as integers, so 1 at 255 and 0 otherwise. The action
  is latent: its `Completed` output fires when the fade has run.

Around a level intro the levels use it the same way. What starts the intro's Matinee also fades the picture
in (4 s in the Prologue and Flight, 0.5 s elsewhere), and an event key near the Matinee's end fades out to
white in 0.2 s, its `Completed` fading back in over 0.2 s: the flash that covers the hand-over to the player.

The step of `FadeAmount` itself is native; the port takes it as a straight line over the time.

## 8. In the port

| File | What |
|---|---|
| `src/assets/level_lightmaps.*` | Light maps: components, BSP elements, the texture sets |
| `src/assets/level_postprocess.*` | `WorldInfo` settings, `PostProcessVolume`s, `HeightFog` actors |
| `src/assets/level_intro.cpp` | The fades a level intro asks for (`LevelIntroSequence::fades`) |
| `src/cutscene/screen_fade.hpp` | `TdHUD`'s fade state |
| `src/renderer/post_process.hpp` | The constants of every pass, shared by both renderers: fog layers, haze, bloom taps, metering, exposure, tone mapping; the settings in force at the view and their blend |
| `src/renderer/builtin_shaders_msl.hpp`, section 4 | The passes: `fog_fragment`, `haze_fragment`, `bloom_gather_fragment`, `filter_fragment`, `meter_scene_fragment`, `meter_fragment`, `exposure_fragment`, `tonemap_fragment` |
| `src/assets/material_system.cpp` | The light-map lookup and transfer in the material prelude |
| `src/renderer/d3d11_renderer.cpp`, `metal_renderer.mm` | The targets and the pass sequence |

The passes of a frame: scene (opaque) → fog → translucency and the first-person body → haze → bloom gather →
blur across → blur down → metering (512, 128, 32, 8, 2, 1) → exposure step → blend, tone mapping and fade into
the back buffer → HUD. Targets: scene and hazed scene RGBA16F, two quarter-size RGBA16F, six RGBA16 fixed-point
metering buffers, two 1 x 1 R32F exposure buffers.

Shaders are written once, in Metal Shading Language, and translated for Direct3D
([`WINDOWS_PORT.md`](WINDOWS_PORT.md)).

The exposure in time. The game loop asks for the start at the high clamp when a level opens
(`PlayerTelemetry::exposure_reset`, as `TdHUD` asks retail), and from then on the exposure follows the view
with the frame's time step. A picture that is not part of a played sequence settles at once instead: the same
moment drawn again, a level's first picture when nobody opened it, a gap of half a second or more, or a view
300 uu from the last one. That is what keeps the oracle's screenshots, drawn now and then from wherever a
stage put the player, at the exposure the place has.

Options for looking at things:

| | |
|---|---|
| `--intro-shots <map> <t,t,..> <dir>` | Plays the level's intro headless from its start and saves the picture at those Matinee times |
| `--dump-shaders <dir>` | Writes the Metal sources as the renderer compiles them, with a material of no graph that calls every entry of the material prelude: `xcrun -sdk macosx metal -std=macos-metal2.4 -c` checks them on a machine with no game data |
| `ME_EXPOSURE=<v>` | Holds the exposure (the factor the scene is multiplied by) |
| `ME_NO_HUD=1` | No debug HUD |
| `ME_SHOW_UNBAKED=1` | Level geometry with no baked lighting in magenta |

## 9. Measured against retail

The level intros are matched cameras: the port's intro camera is retail's to within a unit
([`LEVEL_INTROS.md`](LEVEL_INTROS.md)), and the recordings hold a retail back buffer for every second of each.
`tools/retail/render_check.py shots` asks the port for the same Matinee times, played through from the start
so that exposure and fades are where the game has them; `compare` pairs the pictures.

The measure is the root mean square difference of the two pictures on a 32 x 18 grid of cell means, in
display values (0..255). The grid forgives what a still cannot match (a hand's exact pose, grain) and keeps
exposure, colour and where light falls.

| Chapter | Pairs | Difference | Mean luminance, retail / port |
|---|---|---|---|
| Prologue (`edge_p`) | 36 | 43.8 | 145 / 148 |
| Flight (`escape_p`) | 10 | 34.1 | 126 / 128 |
| Jacknife (`stormdrain_p`) | 7 | 37.2 | 138 / 138 |
| Heat (`cranes_p`) | 10 | 19.7 | 144 / 147 |
| Ropeburn (`subway_p`) | 13 | 20.6 | 176 / 177 |
| New Eden (`mall_p`) | 10 | 28.6 | 138 / 141 |
| Pirandello Kruger (`factory_p`) | 6 | 31.1 | 188 / 188 |
| The Boat (`boat_p`) | 10 | 27.3 | 83 / 90 |
| Kate (`convoy_p`) | 1 | 64.3 | 214 / 199 |
| The Shard (`scraper_p`) | 7 | 12.7 | 35 / 37 |
| **All ten** | 110 | **31.9** (was **68.5**) | 139 / 139 |

The 68.5 is the renderer as it was: a forward sun with shadow cascades and a hemisphere standing in for the
light maps, a filmic curve and screen-space ambient occlusion standing in for the chain. Its pictures had a
mean luminance near 120 whatever the level and a saturation of 35 against retail's 57; the port's is 57 now.
Along the way: light maps and the chain 50.8, BSP light maps 43.0, the fog's slabs and clamped metering 36.9,
volumes 33.5, fades 31.9.

What the remaining difference is made of, from the pictures: the first-person body (drawn where retail's is
not, or posed a frame apart), retail's motion blur in fast stretches, the sun's lens flare (Pirandello Kruger,
the Prologue), objects the port does not draw (banners, screens, runner-vision red), and the single Kate
frame, which falls inside its opening fade.

## 10. What is still a stand-in, or missing

- **Light for what is not level geometry.** Movers, characters, the first-person body, dropped weapons. Retail
  lights these through `DynamicLightEnvironmentComponent` (a directional light and spherical harmonics made from
  the lights around the object); the port still gives them the old stand-in, a sun with shadow cascades plus a
  hemisphere. A door in shade comes out dark where retail's is light.
- **Level geometry with no light map.** Drawn with what it emits only. The cooked light maps list no lights
  (their GUID arrays are empty), so which lights retail lets fall on such a mesh dynamically is not known from
  the data; none was seen in the pictures checked.
- **Lens flares.** `LensFlareSource` actors (the sun in Pirandello Kruger, lamps) are not drawn. The vertex
  factory ships (`LensFlareVertexFactory.usf`); the element layout is native.
- **`TdMotionBlurPostProcess`.** The shader is a radial blur outside the middle of the screen
  (`pow(distance, 0.1) - 0.95`, clamped to 0.07, times `MotionPacked.r`, eight taps); how the player's speed
  becomes `MotionPacked` is native and not read yet.
- **Material effects** of the chain (health, melee, reaction time and the rest) are still the old
  approximations in `tonemap_fragment`.
- **Fog on translucency.** Translucent surfaces are drawn after the fog pass and take none; retail fogs them
  in their vertex shader.
- **Modulated shadows** (the blue-tinted shadows dynamic objects cast), decals and particles.
- **Fades outside the intros.** Only a start or restart and the intro's own Kismet fades are followed.
- **Volumes switched by Kismet** stay as they ship.
- **The sky pass.** The procedural sky is still drawn first; every level's own sky dome now covers it.
- **Metal.** The Metal renderer has the same passes, written without a Mac to run them on: the app and both
  shader sources compile there (`--dump-shaders`), and that is all that was checked.

## 11. Scratch

The scripts behind this page are outside git, in the main checkout's ignored `build/re/`: `lm_scan.py`
(component light maps), `bsp_scan.py` and `bsp_verts.py` (BSP elements and their coordinates), `light_scan.py`
and `unbaked_scan.py` (lights, meshes with no light map), `ppv_scan.py` (volumes), `fade_scan.py` (every
`SeqAct_TdFadeEffect` and what fires it), and for the executable `exestr.py` (where a string is used),
`execonst.py` (where a float constant is read) and `exegrep.py` (search a disassembled range), on top of
`build/re/fpanim/exe.py`.
