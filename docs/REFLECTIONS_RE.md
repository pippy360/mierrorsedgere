# Mirror's Edge (2008) — Reflections Architecture Reverse Engineering

This document details the complete reflection architecture of retail **Mirror's Edge** (Unreal Engine 3 v536 / DICE Licensee 43 `TdGame`), reverse-engineered directly from:
- **`Binaries/MirrorsEdge.exe`** (`UnSceneCapture.cpp`, `SceneCaptureRendering.cpp`, `FHLSLMaterialTranslator`)
- **`TdGame/CookedPC/Engine.u` & `TdGame.u`** (`USceneCaptureComponent`, `ASceneCaptureReflectActor`, `ATdReflectionVolume`, `UMaterialExpression*`)
- **`Engine/Shaders/*.usf`** (`MaterialTemplate.usf`, `BasePassPixelShader.usf`, `SphericalHarmonicLightPixelShader.usf`, `TdInterpolatePixelShader.usf`)
- **`TdGame/CookedPC/Maps/**/*.me1` & Content `.upk` Packages** (`TdMainMenu.me1`, `Edge_Pt1.me1`, `Edge_Pt2.me1`, `Factory_Arena.me1`, `B_Vista.upk`, `P_Generic.upk`, `SP09_Skyscraper.upk`, etc.)

---

## 1. Three-Tier Reflection Hierarchy

DICE combined three distinct reflection mechanisms in *Mirror's Edge* to achieve high-contrast glass, water, tile, and metallic reflections at 60 FPS on 7th-generation consoles and Direct3D 9 PC hardware:

| Tier | Mechanism | Primary Classes & Resources | Primary Use Cases |
| :--- | :--- | :--- | :--- |
| **Tier 1** | **Real-Time Planar Mirror Reflections** | `ASceneCaptureReflectActor`, `USceneCaptureReflectComponent`, `ATdReflectionVolume`, `UTextureRenderTarget2D`, `UMaterialExpressionScreenPosition(bScreenAlign=true)` | Main menu harbor water (`TdMainMenu.me1`), Prologue skyscraper glass curtain wall (`Edge_Pt2.me1`), Chapter 6 arena glass monitors (`Factory_Arena.me1`), elevator mirrors (`P_Generic.upk`). |
| **Tier 2** | **Static & Pre-Captured Cubemap Reflections** | `UTextureCube`, `UTextureRenderTargetCube`, `USceneCaptureCubeMapComponent`, `UMaterialExpressionReflectionVector`, `UMaterialExpressionTransform`, `UMaterialExpressionFresnel` | Building glass facades (`B_C_*`, `B_B_*`, `SP09_Skyscraper`), interior marble/linoleum floors, ducts, chrome pipes, water bodies (`B_Vista.Water`), elevator doors (`P_Generic`), sunglasses/visors (`CH_*`). |
| **Tier 3** | **Analytical Directional Lightmap & SH Specular** | `BasePassPixelShader.usf` (`LightMapBasis`), `SphericalHarmonicLightPixelShader.usf` (`WorldReflectionVector`) | All Phong-lit static meshes and skeletal meshes using `SpecularColor` and `SpecularPower` against Beast-baked 3-axis directional lightmaps. |

---

## 2. Tier 1: Real-Time Planar Reflections (`UnSceneCapture.cpp` & `SceneCaptureRendering.cpp`)

### 2.1 Class Layouts (`Engine.u` & `TdGame.u`)

#### `ASceneCaptureReflectActor` (`Engine.SceneCaptureReflectActor : Engine.SceneCaptureActor : Engine.Actor`)
- `SceneCapture` (`USceneCaptureReflectComponent*`): The capture component that owns the mirror plane and render target.
- `StaticMesh` (`UStaticMeshComponent*`): Editor visualization / proxy mesh (`EngineMeshes.CamCube`).
- `ReflectMaterialInst` (`UMaterialInstanceConstant*`): Transient material instance whose `"ScreenTex"` texture parameter is bound to `SceneCapture->TextureTarget`.
- `ReflectionVolume` (`ATdReflectionVolume*`, offset `+0x1CC` in `ASceneCaptureReflectActor`): **DICE custom property** linking the actor to a convex brush volume that gates when the planar reflection render target is updated.

#### `USceneCaptureReflectComponent` (`Engine.SceneCaptureReflectComponent : Engine.SceneCaptureComponent`)
Default properties from `Engine.u` (`Default__SceneCaptureReflectComponent`):
- `TextureTarget` (`UTextureRenderTarget2D*`, offset `+0x80`): Target 2D render target (`PF_A8R8G8B8`).
- `ScaleFOV` (`float`, offset `+0x84`, default `1.0`): Multiplier applied to the main camera's FOV matrix when rendering the reflection pass.
- `NearPlane` (`float`, default `20.0`), `FarPlane` (`float`, default `500.0`).
- `bEnableClipPlane` (`bool`, default `true`): Enables hardware oblique near-plane clipping along the mirror plane.
- `ViewMode` (`ESceneCaptureViewMode`, default `SceneCapView_LitNoShadows`): Renders the reflected scene with baked Beast lightmaps and unshadowed dynamic lights.
- `FrameRate` (`float`, default `1000.0`): Captures every frame when visible and inside `ReflectionVolume`.
- `bSkipUpdateIfTextureUsersOccluded` (`bool`, default `true` in DICE levels): Skips rendering the capture pass if all primitives sampling `TextureTarget` were hardware-occluded on the previous frame.
- `bSkipRenderingDepthPrepass` (`bool`, default `true` in DICE levels): Skips the Z prepass during the reflection capture pass.
- `bUseMainScenePostProcessSettings` (`bool`, default `true`): Applies the main view's tonemapping/color grading to the captured reflection.
- `MaxUpdateDist` / `MaxStreamingUpdateDist` (`float`): Distance thresholds beyond which capture updates and texture streaming are suspended.
- **Default Archetype Rotation**: `Rotator(Pitch=16384, Yaw=0, Roll=0)` (`+90°` pitch), so an unrotated `SceneCaptureReflectActor` has its local `+X` forward axis pointing straight up along world `+Z` (`MirrorNormal = (0, 0, 1)`).

#### `ATdReflectionVolume` (`TdGame.TdReflectionVolume : Engine.Volume : Engine.Brush`)
- Custom DICE volume class (`TdGame.u`) with property `bool bEnabled` (default `true`).
- Referenced by `ASceneCaptureReflectActor::ReflectionVolume` (`+0x1CC`).

---

### 2.2 Disassembly of `MirrorsEdge.exe` Planar Reflection Pipeline

#### A. Mirror Plane Construction — `USceneCaptureReflectComponent::UpdateTransform` (`VA 0x00f99390`..`0x00f9958a`)
When a `SceneCaptureReflectComponent` is attached or moved:
1. Reads `OwnerActor` (`[esi + 0x38]`).
2. Reads `OwnerActor->Rotation` at `[eax + 0xF4]` (`FRotator(Pitch, Yaw, Roll)`) and `OwnerActor->Location` at `[eax + 0xE8]` (`FVector(X, Y, Z)`).
3. Calls `FRotator::Vector()` (`call 0x004250e0`) to compute the unit forward direction `MirrorNormal` from `OwnerActor->Rotation`:
   $$\mathbf{n} = \begin{pmatrix} \cos(\text{Pitch})\cos(\text{Yaw}) \\ \cos(\text{Pitch})\sin(\text{Yaw}) \\ \sin(\text{Pitch}) \end{pmatrix}$$
4. Normalizes $\mathbf{n}$ (`0x00f99402`..`0x00f9944d`) and constructs the world-space `FPlane(Location, MirrorNormal)` (`0x00f9944f`..`0x00f9949b`):
   $$\text{Plane} = (\mathbf{n}_x,\; \mathbf{n}_y,\; \mathbf{n}_z,\; W), \quad W = \mathbf{p}_{\text{actor}} \cdot \mathbf{n}$$
5. Enqueues the render-thread scene proxy update via `FSceneCaptureReflectSceneProxy` (`call 0x00f97c70`).

#### B. Automatic Material Parameter Binding — `ASceneCaptureReflectActor::SyncComponents` (`VA 0x00f9d2b0`..`0x00f9d312`)
- Resolves `FName(L"ScreenTex")` (`push 0x01ca2a2c = L"ScreenTex"`) and calls `UMaterialInstanceConstant::SetTextureParameterValue("ScreenTex", SceneCapture->TextureTarget)`.
- Note: In cooked retail levels, DICE artists also referenced the `UTextureRenderTarget2D` asset directly inside `UMaterialExpressionTextureSample` nodes wired to `UMaterialExpressionScreenPosition(bScreenAlign=true)`.

#### C. Culling, `TdReflectionVolume` Containment, Mirror Matrix & Oblique Clip Plane — `FSceneCaptureReflectSceneProxy::CreateSceneCaptureRenderer` (`VA 0x00f9bc80`..`0x00f9c355`)
Before rendering a planar reflection pass for the current main camera view (`FSceneView* MainView` in `edi`):
1. **Back-of-Plane Culling (`VA 0x00f9bd85`..`0x00f9bdd8`)**:
   - Evaluates the signed plane distance from `MainView->ViewOrigin` (`[edi + 0x1D0..0x1D8]`) to `MirrorPlane`:
     $$d = \mathbf{n} \cdot \mathbf{v}_{\text{eye}} - W$$
   - If the camera is behind the mirror plane, the capture pass is skipped immediately.
2. **DICE Custom `TdReflectionVolume` Camera Containment Check (`VA 0x00f9bdde`..`0x00f9bed9`)**:
   - Reads `OwnerActor->ReflectionVolume` (`mov ecx, [eax + 0x1cc]`).
   - If non-null, queries the volume brush component's world-space bounding box (`BoxSphereBounds` at `[eax + 0x20..0x38]`) and tests `FBox::IsInside(MainView->ViewOrigin)` (`0x00f9be6d`..`0x00f9bed2`), plus performs a point-in-brush check via `UPrimitiveComponent::PointCheck`.
   - **If the player camera is outside the `TdReflectionVolume`, the reflection capture is skipped (`jmp 0x00f9c329`).** This is how DICE prevented expensive 1280x720 planar reflection passes in `SP01/Edge_Pt2` from running when Faith is in another part of the streamed district.
3. **Mirror View Matrix (`FMirrorMatrix`, `VA 0x00f960b0`, called at `VA 0x00f9bee6`)**:
   - Constructs the Householder reflection matrix across `MirrorPlane` $(\mathbf{n}, W)$:
     $$\mathbf{M}_{\text{mirror}} = \begin{pmatrix} 1 - 2n_x^2 & -2n_x n_y & -2n_x n_z & 0 \\ -2n_y n_x & 1 - 2n_y^2 & -2n_y n_z & 0 \\ -2n_z n_x & -2n_z n_y & 1 - 2n_z^2 & 0 \\ 2W n_x & 2W n_y & 2W n_z & 1 \end{pmatrix}$$
   - Multiplies `MirrorMatrix * MainView->ViewMatrix` (`call 0x00426080`) and scales the projection FOV by `1.0 / ScaleFOV`.
4. **Oblique Near-Plane Clipping (`FClipProjectionMatrix`, `VA 0x00f97830`, called at `VA 0x00f9c0be`)**:
   - Transforms the world mirror plane into the reflected view space (`Plane.TransformBy(ViewMatrix)`) and replaces the third column (depth row/column) of the projection matrix so the near clip plane coincides with the mirror plane, preventing any geometry behind the glass/water surface from leaking into the reflection render target.

---

### 2.3 All Retail Level Instances of `SceneCaptureReflectActor` & `TdReflectionVolume`

| Map Package | Actor Export | Location `(X, Y, Z)` | Rotation `(Pitch, Yaw, Roll)` | Mirror Plane $(\mathbf{n}, W)$ | `TextureTarget` (`UTextureRenderTarget2D`) | `ReflectionVolume` |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **`Maps/Menu/TdMainMenu.me1`** | `SceneCaptureReflectActor_0` (`#668`) | `(0.154, -956.0, 0.0)` | `(16384, 0, 0)` *(default +90° pitch)* | $\mathbf{n} = (0, 0, 1)$, $W = 0.0$ *(horizontal harbor water plane at $Z = 0$)* | `UI_City.T_CityReflection_01_R` (`1280x720`, `PF_A8R8G8B8`) | *None* |
| **`Maps/SP01/Edge_Pt1.me1`** | — | `(32286.99, -6619.68, 16056.14)` | `(0, 0, 0)` | — | — | `TdReflectionVolume_0` (`#8736`, `Model #4707`) |
| **`Maps/SP01/Edge_Pt2.me1`** | `SceneCaptureReflectActor_0` (`#7527`) | `(-27764.0, 3056.0, 6896.0)` | `(0, 32768, 32768)` *(180° yaw, 180° roll)* | $\mathbf{n} = (-1, 0, 0)$, $W = 27764.0$ *(vertical skyscraper glass facade at $X = -27764$)* | `M_SP01.T_EdgeReflection_01_R` (`1280x720`, `PF_A8R8G8B8`, `MaxUpdateDist = 20000`) | `TdReflectionVolume_1` (`#10285` at `(-27552, 3472, 7584)`) |
| **`Maps/SP06/Factory_Arena.me1`** | `SceneCaptureReflectActor_3` (`#8092`) | `(-1930.13, 7469.37, -1262.31)` | `(-213, -24611, -16381)` | $\mathbf{n} = (-0.247, -0.969, -0.020)$ | `M_Reflections.SP06.T_TrainingFacilityMonitorReflection_01_R` (`512x256`, `FarCullingDistance = 2000`) | *None* |

#### How Planar Reflection Materials Sample `TextureRenderTarget2D`:
1. **`UI_City.M_CityReflection_01` (`TdMainMenu.me1`, applied to `S_CityBaseWater_01`)**:
   - Samples `UI_City.T_CityReflection_01_R` at `ScreenPosition(ScreenAlign=true).rg`:
     $$\text{EmissiveColor} = \text{lerp}\Big(\text{T\_CityReflection\_01\_R}(\text{ScreenUV}) \times 1.4 - 0.05,\; \text{T\_Skydome\_Menu}(\text{UV}_0) \times 2.0 - 0.3,\; 1.0 - \text{T\_CityFade\_01\_A}(\text{UV}_0).g\Big)$$
2. **`B_C_06.M_C_06_02_Reflection` (`Edge_Pt2.me1`, applied to the glass curtain wall tower)**:
   - Samples `M_SP01.T_EdgeReflection_01_R` at `ScreenPosition(ScreenAlign=true).rg` and modulates by the building's specular mask `T_C_06_02_S`:
     $$\text{DiffuseColor} = \text{T\_EdgeReflection\_01\_R}(\text{ScreenUV}) \times \big(2.0 \times \text{T\_C\_06\_02\_S}(\text{UV}_0)\big) + 1.2 \times \text{T\_C\_06\_02\_D}(\text{UV}_0)$$
3. **`M_SP01.M_EdgeReflection_01` (`Edge_Pt2.me1`, 9-tap screen-space blurred planar reflection)**:
   - Samples `M_SP01.T_EdgeReflection_01_R` 9 times at `ScreenPosition(ScreenAlign=true).rg + (dx, dy)` with `dx, dy` in `{-0.001, 0.0, +0.001}`, averages with `BlurPower = 0.083`, and blends via `Fresnel(Exponent=4.0)`.
4. **`Props/P_Generic.Elevators.M_ElevatorReflection_01` (`P_Generic.upk`)**:
   - Samples `T_ElevatorReflection_01_R` (`1024x1024`) twice — once at `ScreenPosition(ScreenAlign=true).rg` and once at `BumpOffset(Height=0.2, HeightRatio=0.05, ReferencePlane=0.5, Coordinate=ScreenPosition.rg)` — and averages them (`* 0.5`) to simulate a double-glazed glass reflection inside elevator cabs.

---

## 3. Tier 2: Cubemap Reflections (`TextureCube`, `TextureRenderTargetCube`, & HLSL Translator)

### 3.1 `MaterialTemplate.usf` & `FHLSLMaterialTranslator` (`MirrorsEdge.exe` @ `VA 0x0185e3e0`..`0x0185f0d0`)

In `Engine/Shaders/MaterialTemplate.usf` (`CalcMaterialParameters`, lines 435–447):
```hlsl
Parameters.TangentNormal = NormalizePerPixelNormal(GetMaterialNormal(Parameters));
Parameters.TangentCameraVector = NormalizePerPixelNormal(CameraVector);
Parameters.TangentReflectionVector = -Parameters.TangentCameraVector + Parameters.TangentNormal * dot(Parameters.TangentNormal, Parameters.TangentCameraVector) * 2.0;
```
In `MirrorsEdge.exe`, `FHLSLMaterialTranslator` compiles reflection-related material expressions as follows:
- **`UMaterialExpressionReflectionVector::Compile` (`VA 0x0185e3e0`)**:
  - Emits `"Parameters.TangentReflectionVector"` (3-component tangent-space reflection vector).
- **`UMaterialExpressionTransform::Compile` (`VA 0x0185edc0`..`0x0185f0d0`)**:
  - Reads `TransformSourceType` (`TRANSFORMSOURCE_Tangent = 0`, `TRANSFORMSOURCE_Local = 1`, `TRANSFORMSOURCE_World = 2`, `TRANSFORMSOURCE_View = 3`) and `TransformType` (`TRANSFORM_World = 0`, `TRANSFORM_View = 1`, `TRANSFORM_Local = 2`, `TRANSFORM_Tangent = 3`).
  - For default `TRANSFORMSOURCE_Tangent -> TRANSFORM_World`: emits `MulMatrix(LocalToWorldMatrix, mul(Parameters.TangentBasisInverse, Input))`.
  - For `TRANSFORMSOURCE_Tangent -> TRANSFORM_View`: emits `MulMatrix(WorldToViewMatrix, MulMatrix(LocalToWorldMatrix, mul(Parameters.TangentBasisInverse, Input)))`.
- **`UMaterialExpressionFresnel::Compile`**:
  - Emits `pow(1.0 - max(0.0, dot(Normal, Parameters.TangentCameraVector)), Exponent)`.

### 3.2 Two Cubemap Coordinate Idioms in DICE Material Graphs
Inspection of all `UMaterial` expression graphs in `CookedPC` reveals two distinct cubemap coordinate patterns:
1. **World-Space Cubemap Reflection (`ReflectionVector -> Transform(TRANSFORM_World) -> TextureSample(TextureCube)`)**:
   - Used on hero skyscrapers, vista water, chrome pipes, and phones:
     - `SP09_Skyscraper.M_Tower_01_facade_01` (`ReflectionVector -> Transform(TRANSFORM_World) -> TextureSample(CubeMap_04)`)
     - `B_Vista.Water.M_VistaWater_SP01B` (`ReflectionVector -> Transform(TRANSFORM_World) -> TextureSample(CM_WaterCube_SP01_Edge)`)
     - `Interiors/i_Maintenance.M_MaintenancePipeSmall_01_Chrome`
     - `Props/P_Sky.Skydome.M_Fakebuilding01`
     - `CellPhone.M_CellPhone_01`
2. **Direct Tangent-Space Cubemap Reflection (`ReflectionVector -> TextureSample(TextureCube)`)**:
   - Used on hundreds of architectural doors, windows, floors, and horizontal water meshes (`P_Generic.Doors.M_Doors_Generic_01`, `P_SP09.BSPMaterials.M_Glass02`, `B_Vista.Water.M_VistaWater`):
   - Wires `ReflectionVector` (`Parameters.TangentReflectionVector`) **directly** into `TextureSample(TextureCube).Coordinates` without a `Transform` node. On horizontal surfaces (floors, vista water), tangent space coincides with world space; on vertical doors/windows, it saves 3 `dp3` matrix-multiply instructions per pixel while still producing view-dependent normal-mapped reflections.

### 3.3 DICE Cubemap Extensions (`bTdSpecialCubeMapLayout` & `TdInterpolatePixelShader.usf`)
- `USceneCaptureCubeMapComponent` (`Engine.u`) adds a custom DICE boolean `bTdSpecialCubeMapLayout` (`UnSceneCapture.cpp` @ `VA 0x00f99d00`).
- `Engine/Shaders/TdInterpolatePixelShader.usf` (`FTdInterpolatePixelShader` in `MirrorsEdge.exe` @ `VA 0x00f96c90`) blends between two captured environment textures (`TextureA`, `TextureB`) using `InterpolateWeight`:
  ```hlsl
  void Main(in float2 TextureCoordinate : TEXCOORD0, out float4 OutColor : COLOR0)
  {
      float4 ColorA = tex2D(TextureA, TextureCoordinate.xy);
      float4 ColorB = tex2D(TextureB, TextureCoordinate.xy);
      float t = saturate(InterpolateWeight);
      OutColor = ColorA * t + ColorB * (1.f - t);
  }
  ```

---

## 4. Tier 3: Directional Lightmap Specular & Spherical Harmonic Reflections

In `Engine/Shaders/BasePassPixelShader.usf` (`GetMaterialPointLightTransfer`, lines 77–108), every Phong-lit surface evaluates specular reflection directly against the 3 Half-Life 2 directional lightmap basis vectors (`LightMapBasis[0..2]`):
```hlsl
static const half3 LightMapBasis[3] = {
    half3( 0.0f,               sqrt(6.0f) / 3.0f, 1.0f / sqrt(3.0f)),
    half3(-1.0f / sqrt(2.0f), -1.0f / sqrt(6.0f), 1.0f / sqrt(3.0f)),
    half3(+1.0f / sqrt(2.0f), -1.0f / sqrt(6.0f), 1.0f / sqrt(3.0f))
};

half3 LightMapNormal = saturate(mul(Parameters.TangentNormal, LightMapBasis));
half3 LightMapReflection = saturate(mul(Parameters.TangentReflectionVector, LightMapBasis));
half3 SpecularTransfer = pow(LightMapReflection, Parameters.SpecularPower + 1.0f) * (1.0f - TwoSidedLightingMask);
```
And for dynamic characters/objects lit by Spherical Harmonics (`Engine/Shaders/SphericalHarmonicLightPixelShader.usf`), `WorldReflectionVector = TransformTangentVectorToWorld(Parameters, Parameters.TangentReflectionVector)` is used to evaluate specular SH irradiance.
