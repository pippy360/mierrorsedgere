# Rendering: what retail draws and how the port draws it

How Mirror's Edge lights and finishes a frame, worked out from the game's own files, and what the port
does with that. It covers the baked lighting, the fog, the post-process chain (haze, bloom, exposure, tone
mapping, the material effects, the screen fade, the motion blur), the light and the shadows of what is not
level geometry, lens flares, decals and particles. Materials themselves are in
[`MATERIAL_SYSTEM.md`](MATERIAL_SYSTEM.md).

The work was measured against retail pictures taken from the same camera (section 15). Over the ten level
intros the difference between the two games' pictures fell from 68.5 to 30.0 on the scale described there,
and what is left is mostly things other than lighting and tone: the first-person body where retail's is out
of view, and objects the port does not draw.

## 1. Where the knowledge comes from

| Source | What it gives |
|---|---|
| `Engine/Shaders/*.usf` in the retail install (113 files, shipped as source) | The exact arithmetic of every pass: `BasePassPixelShader.usf`, `HeightFogCommon.usf`, `TdDirHazePixelShader.usf`, `DOFAndBloomGatherPixelShader.usf`, `DOFAndBloomBlendPixelShader.usf`, `FilterPixelShader.usf`, `TdToneMapExposurePixelShader.usf`, `TdToneMappingPixelShader.usf`, `TdMotionBlurShader.usf`, `PointLightPixelShader.usf`, `SphericalHarmonicLightPixelShader.usf`, `ModShadowProjectionPixelShader.usf`, `LensFlareVertexFactory.usf` |
| `TdGame/CookedPC/Effects/FX_PostProcess.upk` | The chain: which effects run, in what order, with what values |
| `Engine.u`, `TdGame.u` | The defaults of `PostProcessSettings` and of the light environments, and the scripts that drive the fade and the material effects (`TdHUD`, `TdHudEffectManager`, `SeqAct_TdFadeEffect`) |
| The level packages | Light maps, lights, `WorldInfo.DefaultPostProcessSettings`, `PostProcessVolume`s, `HeightFog` actors, each mover's light environment, lens-flare sources and their templates, decals with their clipped triangles |
| `MirrorsEdge.exe` | What the shaders are handed: the fog constants, the exposure's metering, the motion blur's amount, a light environment's lights and its shadow, where a lens flare's quads go. Addresses are given where a fact was read there |
| Retail recordings | A back buffer a second through every level intro (`tools/retail/intro_capture.py --frames`), to check all of the above |

The user's `TdEngine.ini [SystemSettings]` on the machine the recordings were made on: `DirectionalLightmaps=True`,
`Bloom=True`, `QualityBloom=False`, `DepthOfField=True`, `TdSunHaze=True`, `TdTonemapping=True`,
`TdBicubicFiltering=True`, `TdMotionBlur=True`, `MotionBlur=False`, `AmbientOcclusion=False`, `LensFlares=True`,
`FogVolumes=False`.

## 2. A retail frame

1. **Base pass.** Every opaque surface writes linear, unbounded scene colour into a 16-bit float buffer:
   its light map through the material's diffuse and specular, plus what it emits. There is no forward sun,
   no ambient term and no shadow map for level geometry: the light is all in the light maps. What is not
   level geometry takes the lights its light environment makes for it (section 9).
2. **The dynamic objects' shadows**, multiplied into scene colour (section 10).
3. **Height fog**, a full-screen pass that reads the depth buffer.
4. **Decals** (section 12), **translucency** with the particles (section 13) and the lens flares (section 11)
   in it, then the first-person body in its own depth range. Translucent surfaces take the fog in their own
   shaders.
5. **The post-process chain** of `FX_PostProcess.FX_PostProcess`, in this order (effects marked off are
   `bShowInGame=False` until script turns them on):

   | # | Effect | Notes |
   |---|---|---|
   | 1 | `TdDirectionalHazePostProcess` | world settings |
   | 2 | `DOFAndBloomEffect` | its own values, not the world's: `BloomScale` 0.15, blur kernel 16, both blur clamps 0 (no depth of field) |
   | 3 | `DeathEffect`, `Slideshow`, `MotionBlurEffect`, `ReactionTimeEffect` | off |
   | 4 | `TdToneMappingPostProcess` | world settings: exposure, shadows / midtones / highlights, desaturation, curves |
   | 5 | `HealthEffect`, `MeleeEffect` on; taser, fall, explosion, flashbang, falling, laser, scope off | material effects, on the tone-mapped picture; section 8 |
   | 6 | `FadeInEffect` | off until `TdHUD` fades; section 7 |
   | 7 | `SaturationFilter` | off |
   | 8 | `TdCalibrationPostProcess` | draws only its calibration squares |
   | 9 | `TdMotionBlurPostProcess` | amount 0.6, from 400 uu/s; section 8 |

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

`FadeAmount` moves in a straight line over the time, in steps of at most 0.066 s a frame, and what the
material is given is eased: `FadeInAmount = 3 F^2 - 2 F^3` (`TdHudEffectManager`).

## 8. The material effects and the motion blur

### 8.1 Material effects

The chain's effects 3 and 5 to 7 (section 2) are materials of `FX_PostProcess.upk` drawn over the whole
picture, each reading the picture so far as its scene colour. `TdHudEffectManager` (`TdGame.u`) switches them
on and feeds their parameters:

| Effect | Shown | Parameters |
|---|---|---|
| `HealthEffect` | health under 100 | `Health` = health / 100 (0 while dead) |
| `ReactionTimeEffect` (before tone mapping) | while time is slowed | `FXFScreen_ReactionTime` = `saturate(1 - TimeDilation)` (0.75 at full); dilation goes in over 0.8 s and out over 1.6 s. `FXFScreen_ReactionTimeCharged` pulses 0.5 in / 0.5 hold / 0.5 out when the energy reaches 100 |
| `MeleeEffect` | a blow lands | `FXFScreen_MeleeDamage` = damage / 100 through an envelope 0.06 / 0.06 / 0.4 s; `FXFScreen_MeleeHitDirection` in turns |
| `FallDamageEffect` | a hard landing | `FXFScreen_FallDamage`, envelope 0.03 / 0.12 / 0.75 s (a hard landing counts 15) |
| `UncontrolledFallingEffect` | `Velocity.Z < -2000` | `FXFScreen_UncontrolledFalling` from 0 at 0.5 a second up to 0.99; off at once |
| `DeathEffect` (before tone mapping) | dead | `DeathAmount` 0.25, then 0.25 a second more |
| `FadeInEffect` | section 7 | |

An envelope eases in from the value it has to `min(1, value + strength)`, holds, and eases out (smoothstep
both ways). A bullet hit drives no material effect.

### 8.2 `TdMotionBlurPostProcess`

The last effect of the chain is a radial blur of the finished picture that grows towards the screen's edges
(`TdMotionBlurShader.usf`): eight taps along the line from the screen's centre, of reach
`clamp(pow(distance, 0.1) - 0.95, 0, 0.07) * MotionPacked.r`. Only `MotionPacked.r` matters, and the
executable makes it so (the proxy's render function, `0x012d42a0`):

```
V      = (eye - last eye) / dt, each component clamped to +-720
t      = clamp((|V| - 400) / 320, 0, 1)
smooth = 0.2 t + 0.8 smooth                    a frame, at about 62 frames a second
r      = 0.6 * dot(V / |V|, view direction) * smooth
```

The blur's centre is always the middle of the view, and the shader adds its step (`direction.x`) to both
texture coordinates, a slip the port keeps: the smear runs diagonally.

## 9. Light for what is not level geometry

Movers, characters, the first-person body, held and dropped weapons have no light map. Retail lights them
through a `DynamicLightEnvironmentComponent` (`FDynamicLightEnvironmentState`, constructor `0x00ff3120`;
`UpdateStaticEnvironment` `0x00ff4b60`; `CreateRepresentativeLight` `0x00ff3e50`).

**Gathering.** At the centre of its owner's bounds the environment sums the level's lights into nine
spherical-harmonic coefficients a colour:

- Only lights on the world's *static* list count: enabled, the owner not movable, not `bForceDynamicLight`;
  a `SkyLightToggleable` never (`ULightComponent::HasStaticLighting`, `0x00ed9210`).
- A light counts when it shares a lighting channel with the owner. The light's own `Dynamic` bit is ignored
  and its `CompositeDynamic` bit counts as `Dynamic` (`DoesLightAffectOwner`, `0x00ff1490`), so an owner on
  `Dynamic` takes the lights that were authored for the "composite" of dynamic objects.
- One line check a light, from the light to the centre of the bounds (`IsLightVisible`, `0x00ff11e0`); a
  directional light's from 100,000 uu out. A light that does not cast static shadows, and a sky light, is
  not checked. The check is a shadow-cast trace: against a mesh's own triangles, not its collision hull.
- A light adds `SHBasis(direction to it) * colour there`: `pow(LightColor, 2.2) * Brightness`, for a point
  light times `pow(max(1 - (d / Radius)^2, 0), FalloffExponent)`, for a spot light times the square of its
  cone ramp. A sky light adds its upper and lower colours through the two sky functions.
- DICE's bounce: `E += BouncedLightingIntensity * mirror(E * (1 - D) + luminance(E) * D)`, `D =
  BouncedLightingDesaturation` (-1 by default), the directional bands mirrored. `AmbientGlow * 4 * 0.282095`
  is added to the constant band.
- It is gathered again at most every 0.3 s and only when the owner has moved; the lights in use move to the
  new ones in a straight line meanwhile.

**The lights it becomes** (`CreateEnvironmentLightList`, `0x00ff4690`). The brightest direction
`(-L[3], -L[1], L[2])` of the coefficients' luminance is taken out as a **point light** standing
`LightDistance` bounds-radii from the centre, of radius `(LightDistance + ShadowDistance + 2)` radii and
falloff exponent 2, its colour the environment's light in that direction divided by the falloff at the centre
and put through eight bits a channel. What is left lights the object as **spherical harmonics**, diffuse only
(`SphericalHarmonicLightPixelShader.usf`: band factors `2 pi / (1 + p)`, `2 pi / (2 + p)`,
`p 2 pi / (3 + 4 p + p^2)` for `DiffusePower` p). With `bSynthesizeSHLight` off the remainder becomes a sky
light's two colours instead.

**Who has one.**

| Owner | Environment | `LightDistance` / `ShadowDistance` / bounce |
|---|---|---|
| `TdPawn`, `TdBotPawn`: the third-person body, the first-person legs | `MyLightEnvironment` | 8 / 1 / 0.2 |
| `TdPlayerPawn`: the first-person arms and the held weapon | `MyLightEnvironment1P` | 8 / 2.5 / 0.1 |
| `InterpActor`, `KActor`, `SkeletalMeshActor` | `MyLightEnvironment`, **off by class default**; a level switches it on per actor | 1.5 / 1 / 0.1 |

An actor whose environment is off is lit straight by the lights that share a channel with its mesh (the sky
through the base pass's hemisphere term, every other light as a pass of its own), with no line check; a mesh
with `bAcceptsLights=False` shows only what it emits. In Jacknife's 60 packages 119 `InterpActor`s have the
environment on and 100 off.

**In the port** (`src/assets/level_lights.*`, `src/renderer/light_environment.hpp`,
`scene_shading_msl.hpp`): the level's static-list lights are kept in the scene; every dynamic object drawn
(lift parts, doors, the `InterpActor`s and `KActor`s the port leaves in place, enemies, the first-person
body) has an environment that is gathered and split as above, and its point light and harmonics are handed
to the shaders with the object's draw. Differences: the whole first-person mesh takes the arms' environment;
an actor with its environment off is lit by the sky colours and the one brightest light, not by every light;
pawn bounds are a fixed sphere. `ME_LIGHT_ENV_DEBUG=1` prints what the first-person environment gathers,
light by light, and what hides a light.

## 10. Dynamic shadows

Every dynamic shadow of the game is the shadow of a light environment, and none is a shadow of a level
light. An environment with `bCastShadows` also sums a **shadow environment**: the same lights, but only those
with `bCastCompositeShadow` (the sun and the sky by class, lamps where the level says), no bounce, plus
`AmbientShadowColor` (0.15) from `AmbientShadowSourceDirection` (up; for the player
`Normal(vect(0,0,10) - vector(Rotation))`, so the faint shadow she has in shade falls a little in front of
her). From it (`CreateRepresentativeLight(S, 0, 1)`):

```
Dir            = brightest direction of S          none: no shadow
Dominant       = S's light from Dir                Rest = what is left, as an ambient level
ModShadowColor = min(1, Rest / (Rest + Dominant))  a channel
shadow light   = an invisible point light at centre + Dir * radius * LightDistance,
                 of radius (LightDistance + ShadowDistance + 2) * radius
```

A warm sun over a blue sky leaves a blue remainder: the shadows are blue because the sky is. Per frame and
caster (`CreateProjectedShadow` `0x0106e8c0`, the point light's frustum `0x00f2b670`, `CalcTransforms`
`0x010687e0`):

- The frustum runs from the light through the caster's bounding sphere; depth is linear across the sphere.
- `Res = clamp(trunc(2 * the sphere's radius on screen), 32, 1014)`; `FadeAlpha = ((Res - 32) / 992)^0.2`,
  so a caster 16 pixels in radius has no shadow. Depth bias `0.02 * 512 / Res` of the sphere's depth.
- The projection (`ModShadowProjectionPixelShader.usf`) multiplies scene colour, after the opaque scene and
  before height fog and translucency:

```
ShadowAtt = sqrt(saturate(1 - (distance to the shadow light / its radius)^2))
scene    *= lerp(lerp(1, lerp(1, ModShadowColor, FadeAlpha), ShadowAtt), 1, PCF^2)
```

  PCF is sixteen hardware taps of which eight are distinct (the offset table is read with a fixed column, a
  stock-engine slip that retail has), turned by 45 degrees, a texel apart. The shadow dies out a few
  bounds-radii behind the caster: retail's dynamic shadows are light and short by construction.

The player's shadow is cast by the third-person body, which is in the scene with DICE's
`bOwnerNoSeeWithShadow`: not drawn in her own view, still a shadow subject.

**In the port** (`src/renderer/mod_shadow.hpp`, `mod_shadow_fragment`): each caster's depth map is a cell of
the third slice of the shadow map array, and one pass over the picture applies up to eight shadows, largest
first. Casters: enemies, the dynamic objects whose environment is on and casts, and the player. The port has
no third-person body, so **the player's shadow is that of the first-person body**: no head. Not done: the
shadows inside the first-person depth groups (the arms' on the legs), `ModulateBetter` while hanging, and
pre-shadows. `ME_NO_DYNAMIC_SHADOWS=1` draws without them.

## 11. Lens flares

339 `LensFlareSource` actors in the ten chapters use 25 `LensFlare` templates (the sun, police lights, work
lamps, train lamps, warning lights). A flare is its template's quads ("reflections") strung along the line
from the source's place on the screen, `S`, through the screen's centre
(`FLensFlareSceneProxy::DrawDynamicElements` `0x010a0d80`, `RenderReflections` `0x010a0230`,
`GetElementValues` `0x0109efc0`):

- The quad of ray distance `r` sits at `E = S * (1 - 2 r)` in normalized device coordinates: 0 at the source,
  0.5 at the centre, 1 mirrored. Its width in pixels is
  `(view width / 2) * Proj[0][0] * Size.x * Scaling * AxisScaling.x * DistMap_Scale.x`, whatever the distance;
  a positive `Rotation` (radians) turns it clockwise. It is drawn at clip depth 0.1, just past the near plane.
- Everything that varies is a raw distribution, stored as a lookup table `[min, max, entries...]` read at
  `(x - StartTime) * TimeScale`, linear between entries (`FRawDistribution::GetEntry`, `0x00d72100`). The
  input is the quad's distance from the centre or, with `bUseSourceDistance`, `|S| * |r|`; the `DistMap_*`
  tables are read at the distance to the source in world units.
- Every material is additive and is given the quad's colour (linear, unclamped, up to 150) as its vertex
  colour and three numbers. The shipped vertex factory **swaps the two distances** the native code fills in:
  the node `LensFlareRadialDistance` gets `|S| * |r|`, and `LensFlareSourceDistance` gets `|E|`.
- `LensFlareOcclusion` is `ScreenPercentageMap(coverage)`: the part of the view's pixels that the source's
  bounding box (bounds * 1.1 + 1.1) covers, as last frame's occlusion query counted them. No pixel: nothing
  of the flare is drawn.
- `LensFlareIntensity` is the cone's strength (`CheckViewStatus`, `0x0109f450`): 1 for a template without a
  cone; with the cones the game uses, 1 in front of the lamp and not drawn behind it.
- The "source" element is never drawn (no template gives it a material); the reflections are drawn in
  ascending `RayDistance`, in the world's translucency, under the first-person body.

**In the port** (`src/assets/level_lensflares.*`, `src/renderer/lens_flare.hpp`): templates and sources are
read from the level packages; the quads are placed and sized as above and drawn with their own translated
materials. One thing is done another way: the coverage is the box's outline on the screen, measured, times
the part of a grid of sight lines to it that the level's meshes leave open (25 lines for a sun, 9 for a
lamp), not an occlusion query; a mesh with no collision does not hide a flare. Not done: the 70 sources a
level's Kismet switches on (`bAutoActivate=False`) stay off, and a source on a moving base (police cars,
trains) stays where the level placed it. `ME_LENS_FLARE_DEBUG=1` says what becomes of every source;
`ME_NO_LENS_FLARES=1` draws without them.

## 12. Decals

4421 decals are placed in the ten chapters (dirt, stains, painted arrows and stripes, drains, posters); 3919
carry triangles. A placed `DecalComponent` stores, after its properties, the triangles it was clipped to on
each receiver when the level was built (`UDecalComponent::Serialize`, `0x00fc6dc0`):

```
int32 NumStaticReceivers
per receiver:  int32 Component (the receiving component)
               int32 52, int32 NumVertices, the vertices
               int32 2,  int32 NumIndices,  uint16 indices (a triangle list)
               int32 NumTriangles
               int32 light-map type, then that light map (an FLightMap1D on ten receivers)
vertex (52):   float3 Position, packed TangentX, packed TangentZ (w: the basis' sign),
               float2 UV, float2 LightMapCoordinate, 2 x float2 (texture coordinates 1 and 2)
```

- Nothing is projected at draw time: the stored texture coordinates are used as they are, with the ordinary
  vertex factory. No shader clips or attenuates a decal.
- Positions are in the receiver's own space on a static mesh, and in the decal's frame on BSP:
  `world = HitLocation - x HitNormal + y HitTangent + z HitBinormal`.
- 95 in 100 are unlit: modulate (3425 uses: the colour multiplies what the receiver wrote, so the decal takes
  the receiver's baked light for nothing), translucent (755), additive (36). The 182 lit ones (drains,
  leaves, posters) take the receiving mesh's light-map textures at the decal vertex's stored coordinates.
- Each decal has `DepthBias` (-0.00006 of the depth range by default); a receiver's decals are drawn in
  ascending `SortOrder`.

**In the port** (`src/assets/level_decals.*`, `MeshBuffer::is_decal`): all placed decals of the loaded
packages are read (the Jacknife start: 621 decals, 30,291 triangles, none unread) and emitted as buffers of
their own, one a `SortOrder`. They are drawn with a depth bias (500 steps of the depth buffer at the
triangle's depth, slope 1), unculled, casting no shadow, and before the other translucent surfaces. Not done:
the 440 placed decals that store no receiver (retail may compute them when the level loads), a decal's own
`FLightMap1D`, per-decal bias, and the decals of bullet holes and footsteps.

## 13. Particles

1783 `Emitter` actors are placed in the ten chapters. 134 of them exist only with hardware PhysX
(`bPhysXMutatable`, `Group` "PhysXOnly": `Actor.PreBeginPlay` shuts them down when the `PhysXEnhanced` setting
is off); of the other 1649, 1299 run from the start and 1242 of those are never touched by Kismet. They are
what a player sees standing still: rooftop vent smoke (about 470 placements), far smoke columns (about 300),
the warning lights of The Boat's and The Shard's skyscrapers (about 200), flying paper (88), water drips (63),
heat haze, birds. There is no beam, trail or ribbon emitter in any story map.

**A template** (`ParticleSystem`, cooked into the level packages that place it) is a list of emitters. An
emitter's first LOD level has a required module (material, `ScreenAlignment`, loops, the sub-image grid), a
spawn module (rate and bursts) and a list of modules; a module with `bEnabled=False` is not there. Every value
is a raw distribution baked into a lookup table (section 11), saved as a difference from the module's class
default object in `Engine.u`: a table, or just its operation or chunk size, that the level does not save is
the class's. The operation says how an entry is read (`FRawDistribution::GetValue1`, `0x011699d0`): 1 a plain
value; 2 uniformly between the entry's minimum and maximum, by the engine's `appSRand()`
(`seed = seed * 196314165 + 907633515`, one sequence for everything); 3 one of the two. A module that acts
when a particle is spawned reads at the emitter's time, one that acts every frame at the particle's relative
time (0 at birth, 1 at death).

**A frame of an emitter** (the engine's emitter tick): the emitter's clock moves on, looping at
`EmitterDuration`; particles past the end of their life go; new ones are spawned at the spawn module's rate
(and its bursts), each run through the spawn modules; every particle's velocity, size, rotation rate and colour
go back to their base values and its relative time moves on by `dt / lifetime`; the per-frame modules run;
position and rotation are integrated. A lifetime of 0 never ends (`ParticleModuleLifetime::Spawn`,
`0x00d70340`): the steady flares are that. A system with `WarmupTime` (15 s on all vent smoke) has already run
that long when it is first seen.

| Module | What it does |
|---|---|
| `Lifetime`, `Size`, `Velocity` (+ radial), `Rotation`, `RotationRate`, `Color`, `Location`, `Acceleration`, `LocationPrimitiveSphere`, `LocationPrimitiveCylinder` | set at spawn (rotations in turns) |
| `ColorOverLife` | sets colour and alpha from curves over the particle's life |
| `SizeMultiplyLife`, `RotationRateMultiplyLife`, `VelocityOverLifetime` | scale by a curve over the life |
| `AccelerationOverLifetime` | adds to the velocity |
| `SubUV` | the sub-image index over the life |

**A sprite** is a quad around the particle (`ParticleSpriteVertexFactory.usf`, and the code that sets its
constants, `0x0109c930`). With `R` and `U` the world directions of screen right and up and `a` the rotation:

```
PSA_Square, PSA_Rectangle   Right = cos(a) R - sin(a) U     Up = -sin(a) R - cos(a) U
PSA_Velocity                Right = normalize(cross(to the camera, travel))     Up = -travel
corner (cx, cy)             P + Size.x (cx - 0.5) Right + Size.y (cy - 0.5) Up
```

The quad is parallel to the view plane, and a square's height is its width. A sub-image emitter
(`SubImages_Horizontal` x `SubImages_Vertical`) gives the material two texture coordinate sets, the current
image and the next, and their blend; the material node `ParticleSubUV` is the blend of the two samples. Vent
smoke is a 4 x 4 flipbook cross-fading from image 8 to 13 over a puff's life. The particle's colour (linear,
unclamped) is the material's vertex colour. Every placed template is unlit.

**In the port** (`src/assets/level_particles.*`, `src/renderer/particles.hpp`): the templates and placements
are read as above; the systems within 20,000 uu of the view are run each frame and their sprites drawn with
their own translated materials in the translucency pass, a batch an emitter, far systems first. The sprites
are ordinary scene vertices: the colour rides where a mesh vertex has its light map's first coefficient, the
alpha and the sub-image blend beside it.

What runs is the sprite emitters and the sixteen module classes of the table. Left out, each emitter whole:
**mesh emitters** (`ParticleModuleTypeDataMesh`: the far smoke columns and the flying paper, 35% of the
placements have one), `Orbit` and `LocationEmitter` (bird flocks, bat swarms), `OrientationAxisLock`,
`ColorScaleOverLife`, attractors, collision, and PhysX. In The Shard's opening area that is 959 emitters run
and 251 left out; in Heat's, 96 and 38. Also not done: the systems a level's Kismet switches on (410
placements wait), the ones spawned at run time (bullet impacts, breaking glass, footsteps), LOD levels past the
first (the port draws the first out to its range, where retail thins the far ones), and sorting against other
translucent surfaces. The emitter tick's order is stock Unreal Engine 3's of that year, not read out of the
executable. `ME_NO_PARTICLES=1` draws without them; `ME_PARTICLE_DEBUG=1` lists what was left out and why.

## 14. In the port

| File | What |
|---|---|
| `src/assets/level_lightmaps.*` | Light maps: components, BSP elements, the texture sets |
| `src/assets/level_postprocess.*` | `WorldInfo` settings, `PostProcessVolume`s, `HeightFog` actors, the chain's material effects |
| `src/assets/level_lights.*` | The lights a light environment gathers; an actor's environment settings |
| `src/assets/level_lensflares.*`, `level_decals.*`, `level_particles.*` | Lens-flare templates and sources; decals; particle templates and placements |
| `src/assets/level_intro.cpp` | The fades a level intro asks for (`LevelIntroSequence::fades`) |
| `src/cutscene/screen_fade.hpp`, `src/game/screen_effects.hpp` | `TdHUD`'s fade state; `TdHudEffectManager`'s effects |
| `src/renderer/post_process.hpp` | The constants of every pass, shared by the renderers: fog layers, haze, bloom taps, metering, exposure, tone mapping, motion blur; the scene block (fog, light environment, lens-flare quad) |
| `src/renderer/light_environment.hpp`, `mod_shadow.hpp`, `lens_flare.hpp`, `particles.hpp` | Sections 9, 10, 11 and 13 |
| `src/renderer/scene_shading_msl.hpp` | The scene block and the light-environment arithmetic every shader shares |
| `src/renderer/builtin_shaders_msl.hpp`, section 4 | The passes: `fog_fragment`, `mod_shadow_fragment`, `haze_fragment`, `bloom_gather_fragment`, `filter_fragment`, `meter_scene_fragment`, `meter_fragment`, `exposure_fragment`, `tonemap_fragment`, `finish_fragment` |
| `src/assets/material_system.cpp` | The light-map lookup and transfer, the environment's light, fog on translucency, the lens-flare inputs, in the material prelude |
| `src/renderer/d3d11_renderer.cpp`, `opengl_renderer.cpp`, `metal_renderer.mm` | The targets and the pass sequence |

The passes of a frame: sun shadow cascades and the dynamic shadows' depth maps → scene (opaque) → dynamic
shadows → fog → decals, translucency, particles, lens flares, then the first-person body → haze → bloom gather → blur
across → blur down → metering (512, 128, 32, 8, 2, 1) → exposure step → the material effects that come before
tone mapping → blend and tone mapping → the material effects after it → fade and motion blur into the back
buffer → HUD. Targets: scene and hazed scene RGBA16F, two quarter-size RGBA16F, six RGBA16 fixed-point
metering buffers, two 1 x 1 R32F exposure buffers, two display-range pictures for the effects.

Shaders are written once, in Metal Shading Language, and translated for Direct3D
([`WINDOWS_PORT.md`](WINDOWS_PORT.md)) and OpenGL ([`LINUX_PORT.md`](LINUX_PORT.md)). The OpenGL renderer
also builds on Windows (`mirrorsedge_opengl.exe`), which is how it was checked: its pictures match Direct3D's
to a mean difference of 0.4 of 255 or less.

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
| `ME_SCREEN_EFFECT="Name:Param=v,.."` | Switches a material effect of the chain on by hand, e.g. `HealthEffect:Health=0.3` |
| `ME_SHOT_LOOK="pitch,yaw"` | With `--intro-shots`: turns the view by hand (degrees), to look at something the intro does not |
| `ME_LIGHT_ENV_DEBUG=1`, `ME_LENS_FLARE_DEBUG=1` | Sections 9 and 11 |
| `ME_NO_LENS_FLARES=1`, `ME_NO_DYNAMIC_SHADOWS=1`, `ME_NO_PARTICLES=1` | A picture without them |
| `ME_PARTICLE_DEBUG=1` | Lists the particle emitters left out, and why |

## 15. Measured against retail

The level intros are matched cameras: the port's intro camera is retail's to within a unit
([`LEVEL_INTROS.md`](LEVEL_INTROS.md)), and the recordings hold a retail back buffer for every second of each.
`tools/retail/render_check.py shots` asks the port for the same Matinee times, played through from the start
so that exposure and fades are where the game has them; `compare` pairs the pictures.

The measure is the root mean square difference of the two pictures on a 32 x 18 grid of cell means, in
display values (0..255). The grid forgives what a still cannot match (a hand's exact pose, grain) and keeps
exposure, colour and where light falls.

| Chapter | Pairs | Difference | Mean luminance, retail / port |
|---|---|---|---|
| Prologue (`edge_p`) | 36 | 43.2 | 145 / 147 |
| Flight (`escape_p`) | 10 | 33.5 | 126 / 128 |
| Jacknife (`stormdrain_p`) | 7 | 37.4 | 138 / 137 |
| Heat (`cranes_p`) | 10 | 19.1 | 144 / 146 |
| Ropeburn (`subway_p`) | 13 | 16.4 | 176 / 181 |
| New Eden (`mall_p`) | 10 | 30.0 | 138 / 139 |
| Pirandello Kruger (`factory_p`) | 6 | 30.7 | 188 / 188 |
| The Boat (`boat_p`) | 10 | 16.6 | 83 / 89 |
| Kate (`convoy_p`) | 1 | 60.3 | 214 / 205 |
| The Shard (`scraper_p`) | 7 | 13.1 | 35 / 38 |
| **All ten** | 110 | **30.0** (was **68.5**; 31.9 before sections 8 to 13) | 139 / 140 |

The 68.5 is the renderer as it was: a forward sun with shadow cascades and a hemisphere standing in for the
light maps, a filmic curve and screen-space ambient occlusion standing in for the chain. Its pictures had a
mean luminance near 120 whatever the level and a saturation of 35 against retail's 57; the port's is 57 now.
Along the way: light maps and the chain 50.8, BSP light maps 43.0, the fog's slabs and clamped metering 36.9,
volumes 33.5, fades 31.9.

What the remaining difference is made of, from the pictures: the first-person body (drawn where retail's is
out of view, or posed a frame apart), objects the port does not draw (banners, screens, mesh particles), and the
single Kate frame, which falls inside its opening fade. The intros hold little of what sections 9 to 12 add
(in the Prologue's and New Eden's matched frames the sun is off screen, and the player's shadow shows in
few), so those were checked picture by picture (`MODLOG.md` section 23). The Boat and Ropeburn moved most (27.3 and 20.6 before); other
work on the intros reached the port in between, so not all of that is this work's.

## 16. What is still a stand-in, or missing

- **Particles:** mesh emitters (the far smoke columns, the flying paper), `Orbit` and `LocationEmitter` (birds,
  bats), the systems Kismet switches on or spawns, LOD levels past the first (section 13).
- **Level geometry with no light map.** Drawn with what it emits only. The cooked light maps list no lights
  (their GUID arrays are empty), so which lights retail lets fall on such a mesh dynamically is not known from
  the data; none was seen in the pictures checked.
- **Light environments:** the differences listed in section 9; and `SkeletalMeshActor`s, dropped weapons and
  pickups are not given one.
- **Dynamic shadows:** the player's is cast by the first-person body (section 10).
- **Lens flares:** Kismet-switched sources, sources on moving bases, coverage from sight lines (section 11).
- **Decals:** the ones with no stored receiver, bullet holes and footsteps (section 12).
- **Material effects** not driven: taser, explosion, flashbang, laser, scope, the slideshow.
- **Volumes and lights switched by Kismet** stay as they ship.
- **The sky pass.** The procedural sky is still drawn first; every level's own sky dome now covers it.
- **Metal.** The Metal renderer has the same passes, written without a Mac to run them on: the app and both
  shader sources compile there (`--dump-shaders`), and that is all that was checked.

## 17. Scratch

The scripts behind this page are outside git, in the main checkout's ignored `build/re/`: `lm_scan.py`
(component light maps), `bsp_scan.py` and `bsp_verts.py` (BSP elements and their coordinates), `light_scan.py`
and `unbaked_scan.py` (lights, meshes with no light map), `ppv_scan.py` (volumes), `fade_scan.py` (every
`SeqAct_TdFadeEffect` and what fires it), and for the executable `exestr.py` (where a string is used),
`execonst.py` (where a float constant is read) and `exegrep.py` (search a disassembled range), on top of
`build/re/fpanim/exe.py`. The notes each of sections 8 to 13 was written from, with the disassembly and the
data surveys behind them, are in `build/re/notes/` (`posteffects.md`, `motionblur.md`, `lightenv.md`,
`modshadow.md`, `lensflare.md`, `decals.md`, `particles.md`), with their
scripts in `build/re/<topic>/`.
