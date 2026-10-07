# Mirror's Edge Material System — Reverse Engineering & Implementation

This document covers how `mirrorsedge_macos` turns the materials cooked into Mirror's Edge's
UE3 packages (engine version 536, licensee 43) into Metal pipelines. The goal is that every
static mesh in a level renders with its real material: the right textures, parameters,
static switches, blend mode and two-sidedness.

| Level | Static meshes placed | Missing meshes | Materials resolved | Textures loaded | Generated shaders compiled |
|---|---|---|---|---|---|
| SP00 `Tutorial_p` | 2403 | 0 | 273 / 273 | 469 / 469 (143 MB) | 205 / 205 |
| SP01 `Edge_p` | 4276 | 0 | 407 / 407 | 646 / 647 (215 MB) | 267 / 267 |
| SP02 `Stormdrain_p` | 12752 | 0 | 502 / 502 | 799 / 799 (242 MB) | 353 / 353 |
| SP03 `Cranes_p` | 9980 | 0 | 533 / 533 | 791 / 792 (236 MB) | 340 / 340 |
| SP04 `Subway_p` | 7459 | 0 | 459 / 459 | 721 / 721 (211 MB) | 335 / 335 |
| SP05 `Mall_p` | 7654 | 0 | 581 / 581 | 854 / 854 (267 MB) | 372 / 372 |
| SP06 `Factory_p` | 8817 | 0 | 565 / 565 | 815 / 819 (229 MB) | 397 / 397 |
| SP07 `Boat_p` | 8494 | 0 | 382 / 382 | 608 / 608 (170 MB) | 300 / 300 |
| SP08 `Convoy_p` | 6616 | 0 | 498 / 498 | 765 / 766 (234 MB) | 323 / 323 |
| SP09 `Scraper_p` | 10738 | 0 | 448 / 448 | 727 / 728 (212 MB) | 348 / 348 |

The textures that don't load use runtime-only classes with no cooked pixels:
- `TextureMovie`: Bink video on LCD screens, for example `M_LCDScreens.ZB.TM_ZB_Widescreen_01`.
- `TextureRenderTarget2D`: planar reflections, for example `M_SP01.T_EdgeReflection_01_R`.

Their materials fall back to the default texture. Building the material library takes
0.05–0.2 s per level, and the GPU upload plus MSL compile takes 0.75–2.5 s.

---

## 1. Pipeline overview

```mermaid
flowchart LR
    A["Level packages<br/>*_p, *_Art, *_Bac, *_Slc ..."] --> B["StaticMesh decode<br/>(per-element LOD0 triangles)"]
    A --> C["StaticMeshComponent<br/>Materials[] overrides"]
    B --> D["generate_rooftop_level_geometry<br/>material-binned MeshSections"]
    C --> D
    D --> E["material_paths (canonical object paths)"]
    E --> F["PackageManager<br/>(CookedPC index, lazy loads)"]
    F --> G["MaterialBuilder<br/>MIC chain, static switches,<br/>expression graph to MSL"]
    F --> H["TextureLoader<br/>Texture2D / TextureCube mips"]
    G --> I["SceneMaterialLibrary"]
    H --> I
    I --> J["MetalRenderer<br/>per-shader pipelines,<br/>per-section draws,<br/>translucent pass"]
    K["*_Lgts packages<br/>DirectionalLight"] --> L["LevelScene.sun_direction / sun_color"]
    L --> J
```

| Stage | Source |
|---|---|
| Generic tagged-property tree parser and object paths | [`ue3_props.cpp`](../src/assets/ue3_props.cpp) |
| Package index, cache, raw reads, export lookup | [`package_manager.cpp`](../src/assets/package_manager.cpp) |
| Texture2D / TextureCube decoding | [`texture_loader.cpp`](../src/assets/texture_loader.cpp) |
| Material resolution and the UE3 expression graph to MSL translator | [`material_system.cpp`](../src/assets/material_system.cpp) |
| GPU-facing contract (formats, bindings, library) | [`scene_materials.hpp`](../src/assets/scene_materials.hpp) |
| Mesh elements, component overrides, section binning, level sun | [`upk_loader.cpp`](../src/assets/upk_loader.cpp) |
| Pipelines, culling, scene copies, translucent pass | [`metal_renderer.mm`](../src/renderer/metal_renderer.mm) |

---

## 2. Which material does a mesh section use?

### 2.1 Static mesh elements
LOD0 of each `StaticMesh` stores an array of `FStaticMeshElement` records. Each record is
40 bytes plus 8 bytes per fragment:

```
Material (object ref), EnableCollision, OldEnableCollision, bEnableShadowCasting,
FirstIndex, NumTriangles, MinVertexIndex, MaxVertexIndex, MaterialIndex,
TArray<{int,int}> Fragments
```

The decoder expands each element's index range into a separate triangle range
(`StaticMeshElement{material, first_vertex, vertex_count}`).

Each vertex comes from the `StaticMeshVertexBuffer` and carries:
- TangentX and TangentZ as packed normals. TangentZ.w holds the binormal sign: 0xFF = +1, 0x00 = −1.
- An `FColor`.
- N UV sets, stored as half2 or as float2 when `bUseFullPrecisionUVs` is set.

The engine `Vertex` is 60 bytes. It includes `tangent_sign`, which lets the generated shaders
rebuild the full tangent basis for normal maps.

### 2.2 Per-element material (UE3 `UStaticMeshComponent::GetMaterial`)
1. `StaticMeshComponent.Materials[e]` if that entry is non-null (a per-placement override).
2. Otherwise `Element.Material`.
3. Otherwise `EngineMaterials.DefaultMaterial` (an empty path maps to it).

`generate_rooftop_level_geometry` gives every distinct material path an index into the
library. It then emits one `MeshSection{first_vertex, vertex_count, material}` per material
per batch, so each draw is a single non-indexed `drawPrimitives` call per section.

### 2.3 Mirrored placements
When the placement scale has a negative determinant, the emitter swaps vertices 1 and 2 of
every triangle. This mirrors UE3's `ReverseCulling` for mirrored primitives, so back-face
culling stays correct.

### 2.4 Which UV sets a section's vertices carry
A mesh has up to four UV sets and a material's `TextureCoordinate` nodes name the one they read
(`CoordinateIndex`). The engine `Vertex` carries two (`u,v` and `u2,v2`, `P.uv0` and `P.uv1` in the
generated shaders), so the two are chosen per section from what its material reads:

- The translator records the indices a graph reads (`CompiledMaterial::texcoord_mask`; a texture
  sample, `Panner`, `Rotator` or `BumpOffset` with no coordinate input reads index 0).
  `material_uv_slots` gives index 0 the first slot and index 1 the second, and an index above 1
  takes whichever of the two the graph leaves free. `M_Crane_Top` reads 1 (normal map) and 2
  (diffuse, mask), so its vertices carry mesh sets 2 and 1 and its shader samples the diffuse with
  `P.uv0`. A graph that reads three or more sets gets a warning; no material in the Tutorial or
  Escape levels does.
- The mesh emitter asks a `MaterialUVResolver` (the translator run without loading textures, cached
  per material path) for those slots and fills the two sets with `StaticMeshAsset::uv`.
- An index the mesh does not have reads the mesh's last set, as `FLocalVertexFactory::InitRHI`
  binds it. A mesh with one UV set therefore gives index 1 the same coordinates as index 0.
- BSP surfaces give every index their one `TexCoord`. Their `ShadowTexCoord` is the light-map
  coordinate only.

Before this, every index above 1 read mesh set 1. On the Tutorial crane (`VH_Stationary.S_Crane_01`,
three UV sets) the normal map reads set 1 and the diffuse and its colour mask read set 2, so the
diffuse was drawn with the wrong set and repeated along the arm:

![The crane's diffuse texture mapped with UV set 1 and with UV set 2](../screenshots/tutorial_crane_uv_sets.png)

The picture is an offline flat render of `T_Crane_Top_D` over the `M_Crane_Top_ColourA` sections as
the level loader emits them, not a frame from the app.

---

## 3. Finding objects across packages: canonical paths

Cooked level packages contain copies of objects from content packages. Getting their names
right is what makes cross-package resolution work:

- Inside a standalone content package (for example `M_GenericCubemaps.upk` or
  `EngineMaterials.upk`), the package's own exports have *file-relative* paths with no package
  prefix.
- In cooked level packages, foreign objects are rooted at top-level `Package` exports flagged
  `EF_ForcedExport` (`export_flags & 0x1`). For example, `Tutorial_p` has 87 of its 166
  top-level exports forced, while `M_GenericCubemaps` and `EngineMaterials` have none.

`object_canonical_path()` (in [`ue3_props.cpp`](../src/assets/ue3_props.cpp)) prefixes
package-owned objects with the file's package name. That gives the globally unique UE3 path
used by imports in other packages. `object_outermost_name()` uses the canonical path, and
also names the package that holds separate-file texture bulk data.

`PackageManager::find_export` indexes every export under both its canonical and its relative
path. The package manager also:
- indexes every file under `CookedPC` by lower-case stem;
- lazily loads and caches imports;
- keeps the already-loaded level packages registered under their names.

This fixed the missing `EngineMaterials.DefaultMaterial`, the `M_GenericCubemaps` cubemaps and
the `FX_TextureGeneric` textures.

---

## 4. Material resolution

### 4.1 Classes and properties
`Material` (and `DecalMaterial`) exports carry their inputs as `ExpressionInput` structs:

- `DiffuseColor`, `DiffusePower`, `SpecularColor`, `SpecularPower`
- `Normal`, `EmissiveColor`, `Opacity`, `OpacityMask`, `Distortion`
- `CustomLighting`, `TwoSidedLightingMask`

Each `ExpressionInput` is `Expression` (an object ref) plus `Mask`, `MaskR`, `MaskG`,
`MaskB` and `MaskA`. The mask picks components the same way `FExpressionInput::Compile` does.

They also carry `BlendMode` (Opaque, Masked, Translucent, Additive, Modulate),
`LightingModel` (Phong, NonDirectional, Unlit, Custom), `TwoSided`, `bIsMasked`,
`OpacityMaskClipValue` (default 0.3333) and `Expressions[]`.

### 4.2 MaterialInstanceConstant chains
A `MaterialInstanceConstant` has a `Parent` (another MIC or a Material) plus
`ScalarParameterValues`, `VectorParameterValues` and `TextureParameterValues`. These are arrays
of tagged structs with `ParameterName`, `ParameterValue` and `ExpressionGUID`.

The native tail holds two `{FMaterial; FStaticParameterSet}` blocks. A `FStaticParameterSet` is
`BaseMaterialId` followed by the static switches (32 bytes each:
`{FName, Value, bOverride, GUID}`) and the static component masks (44 bytes each:
`{FName, R, G, B, A, bOverride, GUID}`).

Resolution rules, matching UE3 behaviour:
- **Scalar, vector and texture parameters:** the first MIC in the chain (leaf → root) that
  defines the parameter wins. Otherwise the expression's default value is used.
- **Static switches and masks:** the first entry with `bOverride` while walking leaf → root
  wins. Otherwise the leaf-most stored value, otherwise the expression default. Static switches
  are folded at translation time, so each permutation becomes its own shader.

### 4.3 Expression translation
`MaterialBuilder` walks the expression graph from each material input and emits MSL with
UE3's type rules: `MCT_Float` scalars broadcast, otherwise component counts must match, and
masks are applied per input. Supported expression classes:

> Abs, Add, AppendVector, BumpOffset, CameraVector, Ceil, Clamp, ComponentMask, Constant,
> Constant2Vector, Constant3Vector, Constant4Vector, ConstantBiasScale, ConstantClamp, Cosine,
> CrossProduct, DepthBiasedAlpha, DepthBiasedBlend, Desaturation, DestColor, DestDepth, Divide,
> DotProduct, FlipBookSample, Floor, Frac, Fresnel, If, LensFlareIntensity, LensFlareOcclusion,
> LightVector, LinearInterpolate, Max, MeshEmitterVertexColor, MeshSubUV, Min, Multiply,
> Normalize, OneMinus, Panner, ParticleSubUV, PixelDepth, Power, ReflectionVector, Rotator,
> ScalarParameter, SceneDepth, SceneTexture, ScreenPosition, Sine, SquareRoot,
> StaticComponentMaskParameter, StaticSwitchParameter, Subtract, TextureCoordinate,
> TextureSample*, TextureSampleParameter2D/Normal/Cube, Time, Transform, VectorParameter,
> VertexColor

Shaders are de-duplicated by generated code. Material instances that share a shader differ
only in their uniform buffer (`float4` slots, at most 240) and their texture bindings (at most
14 slots). An unconnected input takes the UE3 default:
- Diffuse and Specular: `pow(128/255, 2.2)`
- SpecularPower: 15
- DiffusePower: 1

A missing texture falls back to FlatNormal, Black or White, chosen from the texture name and
whether it's a normal map.

---

## 5. Textures

`Texture2D` exports have tagged properties (`Format`, `SizeX`/`SizeY`, `AddressX`/`AddressY`,
`SRGB`, `CompressionSettings`, ...) followed by a native tail:

```
FByteBulkData SourceArt (16-byte header, empty when cooked)
int32 NumMips
per mip: { FByteBulkData header (flags, element count, size on disk, offset in file)
           [inline payload unless flags & 0x01]
           int32 SizeX; int32 SizeY }
```

Bulk-data flags:

| Flag | Meaning |
|---|---|
| `0x01` | Stored in a separate file. The payload is at an absolute file offset in the texture's canonical outermost package (for example `B_BD_Commercial.upk`), wrapped in UE3 compressed chunks (tag `0x9E2A83C1`). |
| `0x02` | ZLIB compressed |
| `0x10` | LZO compressed |
| `0x20` | Unused |

Supported formats: `PF_DXT1`, `PF_DXT3`, `PF_DXT5`, `PF_A8R8G8B8`, `PF_G8` and `PF_V8U8`.

`TextureCube` resolves `FacePosX`..`FaceNegZ` to six `Texture2D` faces. Mips above
`ME_MAX_TEXTURE_SIZE` (default 1024) are skipped at load time, and loading runs on a thread
pool.

### 5.1 LZO1X decompressor fix
The original LZO1X decoder sometimes reported success while writing wrong or all-zero output.
For example, `T_BD_08_03_NA` came out as all zeros and failed with "bad mip count". The
decoder in [`upk_loader.cpp`](../src/assets/upk_loader.cpp) is now a faithful port of the
reference "safe" `lzo1x_d.ch` decoder:

- first-byte > 17 literal run;
- 3-byte M1 match after a literal run, at distance `1 + 0x800 + (t >> 2) + (next << 2)`;
- 2-byte M1 match after trailing literals;
- M2, M3 and M4, including the end-of-stream marker;
- input, output and look-behind bounds checks.

It succeeds only if exactly `expected_len` bytes are produced. `load_from_file` counts failed
blocks and prints `[UPKPackage] <path>: N/M compressed blocks failed` when any fail; there are
0 failures across all 100 SP00 and SP01 packages. The fix also recovered geometry: SP00
colliders went from 2178 to 2405, and SP01 regained its skyline and red radio tower.

---

## 6. Generated MSL contract

Every material shares a prelude: `FrameUniforms`, the `mat_vertex` vertex function, and the
lighting and output helpers. The vertex function reads the 60-byte vertex by `vertex_id`
from `buffer(0)` and gets `FrameUniforms` from `buffer(1)`.

Each fragment entry point `mat_ps_N` binds:

| Binding | Contents |
|---|---|
| `buffer(0)` | `FrameUniforms` |
| `buffer(1)` | material uniforms (`float4[]`) |
| `texture(k)` / `sampler(k)` | 2D textures, then cube textures at `num_tex2d + j` |
| `texture(28)` | copy of the opaque scene colour (`SceneTexture`, `DestColor`) |
| `texture(29)` | copy of the opaque scene depth (`SceneDepth`, `DestDepth`, `DepthBiasedAlpha`) |
| `sampler(15)` | clamp sampler for the scene copies |

Set `ME_MATERIAL_DUMP=<file>` to write all generated MSL. There is no offline Metal compiler
in the toolchain, so a compile error only shows up at runtime, as
`[MetalRenderer] Material shaders: X/Y compiled`.

---

## 7. Lighting

The model follows `BasePassPixelShader.usf`, using its HL2 light-map basis and its
diffuse and specular transfer functions. Mirror's Edge lighting is baked by Beast into
`LightMapTexture2D`s, which are not decoded yet. Instead, a **virtual light-map** spreads an
unshadowed sun over the three HL2 basis directions. A flat normal with DiffusePower 1 receives
exactly Lambert N·L. On top of that come a sky/ground hemisphere term and a small ambient
term. Distance haze is added in the output helpers.

**The level sun** is read from the data (see `scan_level_suns` in
[`upk_loader.cpp`](../src/assets/upk_loader.cpp)):
- Sources: the master package, the loaded sub-levels, and the lighting packages. Any package
  whose stem contains `_lgt` counts: `*_Lgts`, `*_lgts`, `*_LGTs`, `*_Lgts_Pt1`, and SP07's
  odd `Boat_Chase_Lgt`. The lighting packages are opened for lights only.
- Every non-component class containing `DirectionalLight` is a candidate. Scoring:
  1. built into the light-map (`bHasLightEverBeenBuiltIntoLightMap`) first;
  2. then affects the Static lighting channel;
  3. then brightness.
- Cinematic-only and PhysX-only lights clear the Static channel, so they lose.
- Direction: `-FRotationMatrix(Rotation).GetAxis(0)`, where 65536 rotation units = 360°.
- Colour: `pow(LightColor, 2.2) * Brightness`. When `bUseBakerColorAndBrightness` is set, the
  Beast values `BakerColor` and `BakerBrightness` are used instead, since those are what the
  light-maps were baked with.

| Level | Source | Direction to sun | Linear colour |
|---|---|---|---|
| SP00 | `Tutorial_lgts.DirectionalLight_0` | (0.335, −0.500, 0.799) | (2.50, 2.29, 1.90) |
| SP01 | `Edge_Ext_Lgts.DirectionalLight_1` | (−0.575, 0.507, 0.643) | (2.50, 2.29, 1.90) |
| SP02 | `Stormdrain_Ext_Lgts.DirectionalLight_1` | (0.613, 0.526, 0.589) | (1.15, 1.01, 0.83) |
| SP03 | `Cranes_Ext_Lgts.DirectionalLight_1` | (0.258, 0.561, 0.786) | (2.50, 2.29, 1.90) |
| SP04 | `Subway_Ext_Lgts.DirectionalLight_1` | (−0.769, −0.486, 0.414) | (2.20, 1.75, 1.26) |
| SP05 | `Mall_Ext_Lgts.DirectionalLight_0` | (0.526, −0.508, 0.682) | (2.50, 2.39, 1.99) |
| SP06 | `Factory_Ext_Lgts.DirectionalLight_1` | (−0.677, −0.305, 0.669) | (2.20, 1.75, 1.26) |
| SP07 | `Boat_Ext_Lgts.DirectionalLight_0` | (−0.174, 0.874, 0.454) | (0.012, 0.019, 0.013) |
| SP08 | `Convoy_p.DirectionalLight_1` | (0.469, 0.377, 0.799) | (2.50, 2.29, 1.90) |
| SP09 | `Scraper_Lobby_Lgts.DirectionalLight_0` | (0.301, 0.615, 0.729) | (0.67, 0.77, 0.85) |

SP07's sun really is close to black: runtime `Brightness` 0.0, `BakerBrightness` 0.025. That
level was baked from sky and local lights, so here only the hemisphere term lights it.

The sky shader uses the same direction for its sun disc. Without a light, the old defaults
apply: direction (−0.4, 0.6, 0.7) and colour (2.0, 1.96, 1.9).

**TwoSidedLightingMask clamp.** UE3 compiles the input as `TwoSidedLightingMask *
TwoSidedLightingColor` and never clamps it.
- `BasePassPixelShader.usf` is linear in the mask M: `DiffuseTransfer = pow(...) * (1 - M) + M`
  and `SpecularTransfer = pow(...) * (1 - M)`.
- The hemisphere function in `MaterialTemplate.usf` is quadratic in M:
  `lerp(Lighting, M * Diffuse, M)`.

Values above 1 only make sense against real Beast light-map magnitudes, so the virtual
light-map uses `saturate(M)`. Every generated shader in the ten campaign maps was surveyed
(`ME_MATERIAL_DUMP` plus the runtime parameter values). These are the only non-zero masks:

| Mask source | Materials | M | Clamped |
|---|---|---|---|
| Constant | `G_Vegetation.M_BushA_Leaves_White_01` | 0.25 | no |
| Texture | `M_TreeA_BD_White_01` (1 − tex.g), `M_BasketContainerPlastic_01` (0.3·(1 − tex.r)), SP06 `M_PirandelloFlag_01` (0.5·(1 − n.y)) | 0..1 | no |
| Parameter | `M_Antenna_13` (SP01, SP09) / `MI_Antenna_13_Red` (SP01) | 1 / 2 | red MIC |
| Parameter | `M_Awning_01` colour MICs (SP01–SP06) / `*trans` MICs (SP01–SP05); SP03 `PX_MI_Awning_01_Transparent_Orange` / `..._Orange_Emissive` | 0 / 3 | `*trans`, PX emissive |
| Constant | SP07 `B_Vista.SP07.M_VistaWater_SP07` | 12 | yes |

- Unclamped, M = 12 turns SP07's vista-water hemisphere term into `144·Diffuse − 11·Lighting`.
  That is 133–144× its clamped value, whatever the sky colours, and the sea renders pure white.
- The awning `*trans` instances and the red antenna drop from M = 3 and M = 2 to full
  two-sided wrap (M = 1). They still look backlit next to their opaque siblings, but are dimmer
  than UE3's extrapolated transfer would make them. Revisit this once Beast light-maps are
  decoded (§11).

The scene colour buffer is display-referred: material outputs are encoded with `pow(1/2.2)`,
and `SceneTexture` reads decode with `pow(2.2)`.

---

## 8. Renderer integration

- **Pipelines:** one `MTLRenderPipelineState` per generated shader and blend mode.
  - Opaque and Masked write depth. Masked uses `discard` when `mask − clip < 0`.
  - Translucent uses alpha blending, Additive uses `One, One`, and Modulate uses `DestColor, Zero`.
- **Opaque pass:** draws every non-translucent section with its pipeline, uniforms and textures.
  Sections without a material fall back to the legacy procedural `world_pipeline`.
- **Translucent pass:** only runs when the frame has translucent sections. If any material
  reads the scene, the HDR colour and depth are first blitted into `scene_color_copy` and
  `scene_depth_copy`. Translucent sections then draw with depth testing on and depth writes off.
  They are not sorted.
- **Back-face culling:** front faces are **counter-clockwise** in this renderer's clip space.
  The view and projection matrices flip UE3's left-handed D3D convention, where front faces are
  clockwise.
  - Measured against culling off, CCW changed 0.3–1% of pixels, removing hidden back faces.
    CW changed 16–58%, which is wrong.
  - Two-sided materials (`TwoSided`) are drawn with culling off.

---

## 9. Environment variables

| Variable | Effect |
|---|---|
| `ME_NO_MATERIALS=1` | Skip the material system and use only the legacy procedural shading. |
| `ME_MATERIAL_VERBOSE=1` | Print per-material diagnostics (warnings, fallbacks, unsupported nodes) and the top missing meshes. |
| `ME_MAX_TEXTURE_SIZE=<n>` | Largest mip loaded (default 1024; minimum 16). |
| `ME_MATERIAL_DUMP=<file>` | Write the generated MSL (prelude plus all fragment shaders). The file is overwritten on every level load. |
| `ME_CULL=off\|ccw\|cw` | Override back-face culling, for debugging. |

---

## 10. Verification

```bash
cmake --build build -j
ME_MATERIAL_DUMP=/tmp/me_mats.metal ./build/mirrorsedge_macos --verify-all
```

Expected log lines:

```
[Level] Sun from Tutorial_lgts.DirectionalLight_0: direction (0.335133, -0.4999, 0.798615), linear colour (2.5, 2.28938, 1.89825), ModShadowColor (0.493616, 0.659224, 0.875138)
[Level] 2404 static meshes placed, 1 hidden (collision only), 18 missing (5 unique), 287 material sections; collision: 204865 triangles (73 BlockingVolumes)
[Materials] 274 materials (274 resolved, 0 fallback, 3 with warnings), 206 shaders, 469 textures (469 loaded, 0 failed, 143 MB) ...
[Level] Sun from Edge_Ext_Lgts.DirectionalLight_1: direction (-0.574586, 0.506797, 0.642657), linear colour (2.5, 2.28938, 1.89825), ModShadowColor (0.529523, 0.74453, 0.932277)
[Level] 4363 static meshes placed, 21 hidden (collision only), 63 missing (12 unique), 427 material sections; collision: 180380 triangles (109 BlockingVolumes)
[Materials] 414 materials (414 resolved, 0 fallback, 2 with warnings), 273 shaders, 665 textures (665 loaded, 0 failed, 219 MB) ...
[MetalRenderer] Material shaders: 206/206 compiled
[MetalRenderer] Material shaders: 273/273 compiled
```

The "missing" actors use level skeletal meshes (`SK_Flag_02`, `SK_Pigeon`, `SK_Celeste`,
`CH_TKY_Cop_SWAT`, the `PX_SK_*` cloth, ...) or the editor-only `MatineeCam_SM`, which is not cooked.
They are not drawn yet, and no procedural stand-in geometry is generated for them.

The log lines above were recorded before §2.4: the three Tutorial warnings were the crane materials
(`VH_Stationary.S_Crane_01.*`) sampling TexCoord index 2, and that warning no longer exists. The
portable loader (`me_replay`) now reports 0 materials with warnings for the Tutorial and Escape
levels; the counts on a Mac have not been re-recorded.

---

## 11. Known limitations and next steps

1. **Beast light-maps.** Decode `LightMapTexture2D` and the `FLightMap2D`/`FLightMap1D` data in
   `StaticMeshComponent.LODData`. That needs light-map UVs and per-component scale vectors,
   and would replace the virtual light-map with the real baked GI and shadows. Tutorial_p alone
   has 76 light-map textures (35 MB). With real light-map magnitudes, the TwoSidedLightingMask
   clamp (§7) can be dropped.
2. **Decals.** Implement `DecalComponent` static receivers, the pre-baked decal geometry
   (550 components in Tutorial_p).
3. **BSP.** Render the `ModelComponent`s (Tutorial_p has 151 Models).
4. **Skeletal meshes** placed in levels.
5. **More than two UV sets in one material.** A vertex carries two (§2.4). A material that reads
   three gets a warning and its third index shares a slot.
6. **Translucency sorting.** Translucent sections are drawn unsorted.
7. **`TextureRenderTarget2D`.** Planar reflections are not rendered, so these targets fall back
   to their default texture.
8. **`TextureMovie`.** Bink playback is not implemented, so the `M_LCDScreens.*` screens
   (SP03, SP06, SP08, SP09) fall back to their default texture.
