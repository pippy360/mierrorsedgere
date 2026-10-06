# Mirror's Edge (UE3 / DICE Beast) — Ambient Lighting & Global Illumination Reverse Engineering

This document details how ambient lighting, sky hemisphere lighting, baked Beast global illumination (GI), dynamic spherical harmonic light environments, screen-space ambient occlusion (SSAO), height fog, and directional sun haze are architected and executed in the retail PC release of *Mirror's Edge* (Unreal Engine 3 build 536 / Licensee 43 `TdGame`, `MirrorsEdge.exe`).

---

## 1. Architectural Overview: 5-Layer Ambient & GI Pipeline

In retail *Mirror's Edge*, ambient and indirect illumination are not a single flat constant; they are composed from five cooperating subsystems:

1. **Offline DICE / Illuminate Labs Beast Global Illumination (`LightMap2D` / `LightMap1D`)**:
   - Precomputed radiosity and photon-mapped indirect bounce lighting baked into 3-axis directional lightmap textures (`LightMapTexture2D` in `*_p.me1` and streaming sublevels) or vertex lightmaps (`LightMap1D`).
   - Configured per level via DICE-custom `WorldInfo` properties (`SkyColor`, `IBLFileName`, `IBLIntensity`), per-light Baker overrides (`BakerColor`, `BakerBrightness`, `bUseBakerColorAndBrightness`, `PhotonIntensity`, `Photons`, `SoftShadowAngle`), and custom window/portal area lights (`TdAreaLight`).
2. **Base-Pass Hemisphere SkyLight Transfer (`USkyLightComponent` / `FSkyLightSceneProxy`)**:
   - Analytical upper/lower hemisphere sky lighting evaluated per pixel in `BasePassPixelShader.usf` (`GetMaterialHemisphereLightTransferFull`) using a quadratic cosine-lobe curve over the world-space vertical normal component $N_z$.
   - Static meshes with baked lightmaps (`bHasLightEverBeenBuiltIntoLightMap = true` on the `SkyLightComponent` and `Interaction->IsLightMapped() == true`) receive their sky light directly from the Beast lightmap (`UpperSkyLightColor = LowerSkyLightColor = 0` in `FSkyLightSceneProxy::AttachPrimitive`), whereas unbaked/dynamic primitives receive real-time `UpperSkyColor` and `LowerSkyColor`.
3. **Dynamic Light Environments & Order-3 Spherical Harmonics (`UDynamicLightEnvironmentComponent`)**:
   - Used by all dynamic characters, weapons, pickups, and movers (`TdPawn`, `TdBotPawn`, `TdPickup`, `InterpActor`, `KActor`).
   - CPU-side `FDynamicLightEnvironmentState` integrates all affecting `SkyLightComponent`s, `DirectionalLight`s, `PointLight`s, `AmbientGlow`, `AmbientShadowColor`, and DICE's custom Beast bounce approximation (`BouncedLightingIntensity`) into a 3-band (9-coefficient, `MAX_SH_ORDER = 3`) RGB Spherical Harmonic irradiance representation (`WorldIncidentLighting[7]`), evaluated on the GPU via `SphericalHarmonicLightPixelShader.usf`.
4. **Shadow-Tinting via Modulated Shadows (`ModShadowColor`)**:
   - Retail `DirectionalLightComponent` and `SkyLightComponent` actors specify saturated azure/cobalt `ModShadowColor` values (e.g., `(0.494, 0.659, 0.875)` in `Tutorial_lgts.me1`, `(0.530, 0.745, 0.932)` in `Edge_Ext_Lgts.me1`, `(0.205, 0.426, 0.767)` in `Cranes_Ext_Lgts.me1`) so dynamic shadows cast by characters and objects match the cool sky-lit Beast radiosity shadows rather than turning grey/black.
5. **Atmospheric & Screen-Space Post-Processing (`AmbientOcclusionShader.usf`, `HeightFogPixelShader.usf`, `TdDirHazePixelShader.usf`)**:
   - View-space 8-tap randomized SSAO with bilateral depth filtering and temporal history reprojection (`AmbientOcclusionShader.usf`).
   - Exponential line-integral height fog (`HeightFogCommon.usf`) and DICE's custom directional sun haze post-process (`TdDirHazePixelShader.usf`).

---

## 2. Binary Disassembly & Engine Internals (`MirrorsEdge.exe`)

### 2.1 `ULightComponent` & `USkyLightComponent` Scene Proxy Creation

In `MirrorsEdge.exe`, `USkyLightComponent` (`vtable @ 0x01C92588`, static class registration at `0x00ED49E0`) overrides `CreateSceneProxy()` at **`0x00ED4580`**, which allocates and constructs `FSkyLightSceneProxy` (`vtable @ 0x01C923B8` / `0x01C923AC`) at **`0x00ED4100`**:

- **Base `FLightSceneInfo::FLightSceneInfo` (`0x0103F0D0`)**:
  - Reads `InComponent->Brightness` (`float` at `this + 0x100`) and `InComponent->LightColor` (`FColor` B,G,R,A bytes at `this + 0x104`).
  - Converts `LightColor` from 8-bit sRGB to linear floating-point via `FLinearColor::FLinearColor(const FColor&)` (`0x0115E240`, which indexes the 256-entry `GSRGBToLinear` LUT `(c / 255.0)^2.2`).
  - Multiplies by `Brightness` and stores the resulting linear `FLinearColor Color` at `FLightSceneInfo + 0xD0..0xDC`:
    $$\text{UpperColor}_{\text{linear}} = \text{sRGBToLinear}(\text{LightColor}) \times \text{Brightness}$$
- **`FSkyLightSceneProxy::FSkyLightSceneProxy` (`0x00ED4100`)**:
  - Calls `FLightSceneInfo::FLightSceneInfo(this, InComponent)` (`0x0103F0D0`).
  - Reads `InComponent->LowerBrightness` (`float` at `this + 0x184`) and `InComponent->LowerColor` (`FColor` at `this + 0x188`).
  - Calls `FLinearColor::FLinearColor(const FColor&)` (`0x0115E240`) on `LowerColor`, multiplies by `LowerBrightness`, and stores `FLinearColor LowerColor` at `FSkyLightSceneProxy + 0x208..0x214`:
    $$\text{LowerColor}_{\text{linear}} = \text{sRGBToLinear}(\text{LowerColor}) \times \text{LowerBrightness}$$

### 2.2 `FSkyLightSceneProxy::AttachPrimitive` & `DetachPrimitive`

When a `SkyLight` interacts with a scene primitive, `FSkyLightSceneProxy::AttachPrimitive` (**`0x00ED43A0`**) and `DetachPrimitive` (**`0x00ED4460`**) accumulate the sky light's upper and lower hemisphere colors onto `FPrimitiveSceneInfo`:

```cpp
// Reconstructed from MirrorsEdge.exe @ 0x00ED43A0
void FSkyLightSceneProxy::AttachPrimitive(const FLightPrimitiveInteraction* Interaction) {
    // Check bit 1 of Interaction flags (offset +0x14): Interaction->IsLightMapped()
    if (!Interaction->IsLightMapped()) {
        FPrimitiveSceneInfo* PrimitiveSceneInfo = Interaction->GetPrimitiveSceneInfo(); // offset +0x10
        // Accumulate UpperColor (this + 0xD0) into PrimitiveSceneInfo->UpperSkyLightColor (+0x74)
        PrimitiveSceneInfo->UpperSkyLightColor += this->Color;
        // Accumulate LowerColor (this + 0x208) into PrimitiveSceneInfo->LowerSkyLightColor (+0x84)
        PrimitiveSceneInfo->LowerSkyLightColor += this->LowerColor;
        // Mark static meshes dirty for base pass draw list regeneration (0x0101CE90)
        PrimitiveSceneInfo->UpdateStaticMeshes();
    }
}
```

### 2.3 Base-Pass Shader Binding & Parameter Setup

In `TBasePassPixelShader` (`Bind` @ **`0x01055470`**, `SetParameters` @ **`0x00CDC870`**, `SetSkyColor` @ **`0x00CDC7D0`**) and `TBasePassDrawingPolicy::SetMeshRenderState` (**`0x01072C58`** / **`0x01073813`**):

1. **Parameter Offsets in `TBasePassPixelShader`**:
   - `this + 0x58`: `FMaterialPixelShaderParameters` (`Bind` @ `0x00F7C200`, `Set` @ `0x00F7B7F0`, `SetMesh` @ `0x00F7BCB0`)
   - `this + 0xB8`: `FShaderParameter AmbientColorAndSkyFactorParameter` (`"AmbientColorAndSkyFactor"` @ `0x01CBBE04`)
   - `this + 0xBE`: `FShaderParameter UpperSkyColorParameter` (`"UpperSkyColor"` @ `0x01CBBDE8`)
   - `this + 0xC4`: `FShaderParameter LowerSkyColorParameter` (`"LowerSkyColor"` @ `0x01CBBDCC`)
2. **`TBasePassPixelShader::SetParameters` (`0x00CDC870`)**:
   - Checks `(View->Family->ShowFlags & SHOW_Lighting)` (`0x1000` at `View->Family + 0x14`):
     - If `SHOW_Lighting` is set: sets `AmbientColorAndSkyFactor = FLinearColor(0.0f, 0.0f, 0.0f, 1.0f)` (`AmbientColor = (0,0,0)`, `SkyFactor = 1.0f`).
     - If `SHOW_Lighting` is cleared (unlit mode): sets `AmbientColorAndSkyFactor = FLinearColor(1.0f, 1.0f, 1.0f, 0.0f)` (`AmbientColor = (1,1,1)`, `SkyFactor = 0.0f`).
3. **`TBasePassDrawingPolicy::SetMeshRenderState` (`0x01072C58`)**:
   - If `bEnableSkyLight` (`this + 0x20` / `0x28`) is true:
     - Reads `PrimitiveSceneInfo->UpperSkyLightColor` (`+0x74`) and `PrimitiveSceneInfo->LowerSkyLightColor` (`+0x84`) (or `(0,0,0,0)` if `PrimitiveSceneInfo == NULL`).
     - Calls `TBasePassPixelShader::SetSkyColor` (`0x00CDC7D0`) to upload `UpperSkyColor` and `LowerSkyColor` via `RHISetPixelShaderParameter`.

---

## 3. Retail HLSL Shader Formulas (`Engine/Shaders/*.usf`)

### 3.1 Hemisphere SkyLight Transfer (`MaterialTemplate.usf` & `BasePassVertexShader.usf`)

In `BasePassVertexShader.usf` (line 167), the world up vector `(0, 0, 1)` is transformed into tangent space per vertex:
```hlsl
#if ENABLE_SKY_LIGHT
    Output.SkyVector = VertexFactoryWorldToTangentSpace(Input, TangentBasis, float3(0, 0, 1));
#endif
```

In `MaterialTemplate.usf` (lines 240–270), `GetMaterialHemisphereLightTransferFull` computes the upper and lower hemisphere transfer weights using a **quadratic cosine-lobe** curve:
```hlsl
half3 GetMaterialHemisphereLightTransferFull(FMaterialPixelShaderParameters Parameters, half3 SkyVector, half3 UpperColor, half3 LowerColor)
{
    half3 TwoSidedLighting = 0;
    half3 TwoSidedLightingMask = 0;
    TwoSidedLightingMask = GetMaterialTwoSidedLightingMask(Parameters);
    TwoSidedLighting = TwoSidedLightingMask *
        GetMaterialSubsurfaceLightingColor(Parameters) * GetMaterialDiffuseColorNormalized(Parameters);

    half NormalContribution = dot(SkyVector, Parameters.TangentNormal);
    half2 ContributionWeightsSqrt = half2(0.5, 0.5) + half2(0.5, -0.5) * NormalContribution;
    half2 ContributionWeights = ContributionWeightsSqrt * ContributionWeightsSqrt;

#if MATERIAL_LIGHTINGMODEL_NONDIRECTIONAL
    half3 UpperLighting = GetMaterialDiffuseColor(Parameters);
    half3 LowerLighting = GetMaterialDiffuseColor(Parameters);
#else
    half3 UpperLighting = GetMaterialDiffuseColor(Parameters) * ContributionWeights[0];
    half3 LowerLighting = GetMaterialDiffuseColor(Parameters) * ContributionWeights[1];
#endif

    return lerp(UpperLighting, TwoSidedLighting, TwoSidedLightingMask) * UpperColor +
           lerp(LowerLighting, TwoSidedLighting, TwoSidedLightingMask) * LowerColor;
}
```
Specifically, for a surface with world-space unit normal $\mathbf{N} = (N_x, N_y, N_z)$:
$$w_{\text{upper}}(N_z) = \left(\frac{1 + N_z}{2}\right)^2, \qquad w_{\text{lower}}(N_z) = \left(\frac{1 - N_z}{2}\right)^2$$
- At $N_z = +1$ (horizontal floor facing straight up): $w_{\text{upper}} = 1.0$, $w_{\text{lower}} = 0.0$.
- At $N_z = 0$ (vertical wall): $w_{\text{upper}} = 0.25$, $w_{\text{lower}} = 0.25$ (sum $= 0.5$).
- At $N_z = -1$ (horizontal ceiling facing straight down): $w_{\text{upper}} = 0.0$, $w_{\text{lower}} = 1.0$.

And in `BasePassPixelShader.usf` (lines 195–207):
```hlsl
#if !MATERIAL_LIGHTINGMODEL_UNLIT
    #if ENABLE_SKY_LIGHT
        Color += GetMaterialHemisphereLightTransferFull(
            MaterialParameters,
            normalize(In.SkyVector),
            UpperSkyColor,
            LowerSkyColor
            ) * AmbientColorAndSkyFactor.a;
    #endif

    #if !SIMPLE_LIGHTING
        Color += GetMaterialDiffuseColor(MaterialParameters) * AmbientColorAndSkyFactor.rgb;
    #endif
#endif
```

### 3.2 Baked 3-Axis Directional Lightmaps (`BasePassPixelShader.usf`)

When `TEXTURE_LIGHTMAP` or `VERTEX_LIGHTMAP` is active, *Mirror's Edge* uses the 3-axis orthonormal Half-Life 2 / UE3 tangent-space lightmap basis (`NUM_LIGHTMAP_COEFFICIENTS = 3`):
$$\mathbf{B}_0 = \left(0,\; \frac{\sqrt{6}}{3},\; \frac{1}{\sqrt{3}}\right), \quad
\mathbf{B}_1 = \left(-\frac{1}{\sqrt{2}},\; -\frac{1}{\sqrt{6}},\; \frac{1}{\sqrt{3}}\right), \quad
\mathbf{B}_2 = \left(+\frac{1}{\sqrt{2}},\; -\frac{1}{\sqrt{6}},\; \frac{1}{\sqrt{3}}\right)$$
Notice that $\mathbf{B}_0 + \mathbf{B}_1 + \mathbf{B}_2 = (0, 0, \sqrt{3})$ and $\mathbf{B}_i \cdot \mathbf{B}_j = \delta_{ij}$.
In `BasePassPixelShader.usf`:
- Each of the 3 directional lightmap textures (`LightMapTextures[0..2]`) is sampled (using either standard bilinear `tex2D` or NVIDIA 4-tap B-spline bicubic filtering `tex2DBicubic` generated by `0x00CE8780`–`0x00CE88FC` in `MirrorsEdge.exe`) and scaled by `LightMapScale[0..2].rgb` (`FTextureLightMapPolicy::SetMesh` @ `0x00CDB5E0`).
- Diffuse transfer for each basis direction is:
  $$d_i = \text{pow}\!\left(\text{saturate}(\mathbf{N}_{\text{tangent}} \cdot \mathbf{B}_i),\; P_{\text{diffuse}}\right)$$
  modulated by `GetMaterialDiffuseColorNormalized` $= \text{DiffuseColor} \times \frac{1 + P_{\text{diffuse}}}{2}$.

### 3.3 Dynamic Light Environments & Order-3 Spherical Harmonics (`SphericalHarmonicLightPixelShader.usf`)

Dynamic actors (`TdPawn`, `TdBotPawn`, `TdPickup`, `InterpActor`, `KActor`) use `UDynamicLightEnvironmentComponent` (`0x00FF1EB8`).
In `MirrorsEdge.exe`, `FSHVector` (`MAX_SH_ORDER = 3`, `MAX_SH_BASIS = 9`) defines three fundamental analytical SH projections at **`0x0117FE70`–`0x0117FFFA`**:
1. **`FSHVector::UpperSkyFunction()` (`0x0117FF00`)**:
   - $Y_0^0 = \frac{1}{\sqrt{\pi}} \approx 0.564190$, $Y_1^0 (Z) = +\frac{1}{2}\sqrt{\frac{3}{\pi}} \approx +0.488603$, all other basis coefficients $= 0$.
2. **`FSHVector::LowerSkyFunction()` (`0x0117FE70`)**:
   - $Y_0^0 = \frac{1}{\sqrt{\pi}} \approx 0.564190$, $Y_1^0 (Z) = -\frac{1}{2}\sqrt{\frac{3}{\pi}} \approx -0.488603$, all other basis coefficients $= 0$.
3. **`FSHVector::AmbientFunction()` (`0x0117FF90`)**:
   - $Y_0^0 = \frac{1}{2\sqrt{\pi}} \approx 0.282095$, all other basis coefficients $= 0$.

At runtime (`0x00FF1EF0` / `0x00FF2310`), `FDynamicLightEnvironmentState` accumulates:
- `AmbientGlow` projected via `AmbientFunction()`
- `AmbientShadowColor` projected along `AmbientShadowSourceDirection`
- Every affecting `USkyLightComponent`'s `UpperColor * UpperSkyFunction() + LowerColor * LowerSkyFunction()`
- DICE's custom **`BouncedLightingIntensity`** (default `0.1`, `0.2` on `Default__TdPawn` and `Default__TdPickup`), which injects simulated Beast indirect bounce from the primary light sources into the SH environment!

On the GPU, `SphericalHarmonicLightPixelShader.usf` reconstructs `FSHVectorRGB WorldIncidentLighting` from 7 `float4` uniform registers (`WorldIncidentLighting[7]`, bound at `0x00FE5F13`), evaluates `ClampCosPowerSH(WorldNormal, DiffusePower)` via two cubemap lookups (`SHBasisCubeTextures[0..1]`), and multiplies by `GetMaterialDiffuseColorNormalized`.

### 3.4 Screen-Space Ambient Occlusion (`AmbientOcclusionShader.usf`)

When enabled in `PostProcessSettings`, `AmbientOcclusionShader.usf` (`Bind` @ `0x00FCFDC0`):
- Reflects 8 unit-sphere sample offsets (`AO_SAMPLE_DIRECTIONS`) across a random normal from `RandomNormalTexture` within world-space radius `AORadius`.
- Compares sample depth against `SceneDepth` with `OcclusionBias = -0.3`, `OcclusionScale = 20.0`, `OcclusionPower = 4.0`, and `MinOcclusion = 0.05`.
- Applies an edge-preserving depth bilateral blur (`FilterPixelMain`) and temporal exponential moving average (`HistoryUpdatePixelMain`, `HistoryConvergenceTime = 0.05s`).
- Modulates scene lighting by `lerp(OcclusionColor.rgb, 1.0, Occlusion)` (`OcclusionColor` @ `0x01CAD0E0`, default `(0, 0, 0, 1)`).

---

## 4. DICE Custom Beast GI Properties (`Engine.u` & `TdGame.u`)

DICE extended UE3's `WorldInfo`, `Light`, `LightComponent`, `DirectionalLightComponent`, `PointLightComponent`, and `DynamicLightEnvironmentComponent` with custom properties for Illuminate Labs **Beast** offline GI baking:

| Class | Property | Type | Default (`Engine.u` / `TdGame.u`) | Purpose |
|---|---|---|---|---|
| `WorldInfo` | `SkyColor` | `Vector` | `(0.3, 0.7, 1.0)` | Beast sky dome radiosity color during GI baking |
| `WorldInfo` | `IBLFileName` | `Str` | `"sky.hdr"` (in `TdMainMenu`, `Cranes_p`) | HDR environment map used by Beast Image-Based Lighting |
| `WorldInfo` | `IBLIntensity` | `Float` | `1.0` (`2.0` in `TdMainMenu`, `Cranes_p`) | Multiplier on Beast HDR image-based sky lighting |
| `WorldInfo` | `CubeMapOverride` | `TextureCube` | `M_GenericCubemaps.SP00.CM_SP00` | Level-wide default reflection cubemap override |
| `Light` | `BakerColor` | `Color` | `(0, 0, 0, 0)` | Override color sent to Beast lightbaker instead of `LightColor` |
| `Light` | `BakerBrightness` | `Float` | `0.0` | Override brightness sent to Beast lightbaker instead of `Brightness` |
| `Light` | `bUseBakerColorAndBrightness` | `Bool` | `false` | When `true`, Beast bakes using `BakerColor * BakerBrightness` while real-time specular/dynamic lighting uses `LightColor * Brightness` |
| `Light` | `SampleFactor` | `Float` | `1.0` | Beast lightmap supersampling factor for this light |
| `DirectionalLightComponent` | `SoftShadowAngle` | `Float` | `0.0` (`1.0`–`2.5` in maps) | Sun angular diameter (degrees) for Beast soft area shadows |
| `DirectionalLightComponent` | `Photons` | `Int` | `100000` | Number of GI photons emitted into Beast photon map |
| `DirectionalLightComponent` | `PhotonIntensity` | `Float` | `1.0` (`1.4` in `Edge`, `Cranes`, `Factory`) | Indirect bounce multiplier for Beast photons (boosted `1.4x` to make white/colored walls bleed vibrantly!) |
| `LightComponent` | `ModShadowColor` | `LinearColor` | `(0, 0, 0, 1)` | Tints dynamic modulated shadows with sky-blue radiosity color |
| `TdAreaLight` (`TdGame.u`) | `WindowLightAngle` | `Float` | `0.2` | Rectangular portal/window area light (`BoxComponent` + `PointLightComponent`) for indoor GI |
| `DynamicLightEnvironmentComponent` | `BouncedLightingIntensity` | `Float` | `0.1` (`0.2` on `TdPawn`, `TdPickup`) | Real-time approximation of Beast colour bleeding onto dynamic actors |
| `DynamicLightEnvironmentComponent` | `AmbientShadowColor` | `LinearColor` | `(0.15, 0.15, 0.15, 1.0)` | Directional ambient shadow floor in `DynamicLightEnvironmentComponent` |

---

## 5. Extracted Retail Ambient, SkyLight, Shadow & Fog Values Across All Chapters

Below are the exact retail values extracted from `/Users/tomnom/mirrorsedge/TdGame/CookedPC/Maps/` across the main menu and all 10 campaign chapters (`SP00`–`SP09`):

| Level | `WorldInfo.SkyColor` | Primary `SkyLightComponent` Upper (`LightColor` $\times$ `Brightness`) | Primary `SkyLightComponent` Lower (`LowerColor` $\times$ `LowerBrightness`) | Sun `BakerColor` $\times$ `BakerBrightness` (`PhotonIntensity`) | Sun `ModShadowColor` (Linear RGB) | `HeightFog` (`LightColor` $\times$ `LightBrightness`, `Density`, `StartDist`) | `HazeColor` (`HazeMultiplier`) |
|---|---|---|---|---|---|---|---|
| **Main Menu** (`TdMainMenu.me1`) | `(0.30, 0.70, 1.00)` (`IBL="sky.hdr"`, `2.0`) | — | — | `(1.00, 0.90, 0.69) * 3.0` | `(0.00, 0.00, 0.00)` | — | `(0.20, 0.20, 0.10)` (`0.0`) |
| **Tutorial** (`Tutorial_p` + `Tutorial_lgts`) | `(0.30, 0.52, 0.65)` | `SkyLight_0`: `(0.643, 0.702, 0.851) * 0.30` (Cinematic: `* 3.0`) | `(0.737, 0.627, 0.447) * 0.25` (Cinematic: `* 2.5`) | `(0.996, 0.898, 0.753) * 3.0` (`Photons=1.0`) | **`(0.494, 0.659, 0.875)`** | `(0.624, 0.831, 0.980) * 0.9`, `d=5e-6`, `start=2000` | `(1.00, 0.96, 0.85)` (`0.5`) |
| **Prologue — The Edge** (`Edge_p` + `Edge_Ext_Lgts`) | `(0.30, 0.52, 0.65)` | `SkyLight_3`: `(0.620, 0.749, 0.855) * 0.70` (`bBuiltIntoLightMap=true`) | `(0.980, 0.957, 0.922) * 0.25` | `(0.996, 0.898, 0.753) * 2.2` (`Photons=1.4`) | **`(0.530, 0.745, 0.932)`** | `(0.624, 0.831, 0.980) * 0.9`, `d=5e-6`, `start=2000` | `(1.00, 0.96, 0.85)` (`0.5`) |
| **Ch 1 — Flight** (`Escape_p` + `Escape_*_Lgts`) | `(0.34, 0.42, 0.77)` | `SkyLight_1`: `(0.325, 0.357, 0.259) * 1.0` / `St1`: `(0.298, 0.306, 0.365)` | `(1.000, 1.000, 1.000) * 0.0` | `(1.000, 0.847, 0.631) * 3.0` (`Rt`: `2.2`) | `(0.494, 0.659, 0.875)` | `(0.878, 0.729, 0.412) * 0.5`, `d=3e-6`, `start=15000` | `(1.00, 0.75, 0.40)` (`0.8`) |
| **Ch 2 — Jacknife** (`Stormdrain_p` + `Stormdrain_Ext_Lgts`) | `(0.30, 0.70, 1.00)` | `SkyLight_0`: `(0.376, 0.451, 0.576) * 1.0` (`StdP`: `(0.149, 0.188, 0.129) * 0.5`) | `(1.000, 1.000, 1.000) * 0.0` | `(1.000, 0.941, 0.863) * 1.15` (`Rt`: `3.45`) | `(0.421, 0.619, 0.730)` | `(0.608, 0.678, 0.776) * 0.75`, `d=5e-6`, `start=10000` | `(1.00, 0.95, 0.70)` (`0.7`) |
| **Ch 3 — Heat** (`Cranes_p` + `Cranes_Ext_Lgts`) | `(0.30, 0.70, 1.00)` (`IBL="sky.hdr"`, `2.0`) | `SkyLight_0`: `(0.651, 0.792, 1.000) * 0.25` (`SkyLight_1`: `(0.749, 0.851, 1.000) * 0.50`) | `(0.910, 0.925, 0.957) * 0.30` | `(1.000, 0.961, 0.882) * 2.5` (`Rt`: `2.25`, `Photons=1.4`) | **`(0.205, 0.426, 0.767)`** | `(0.224, 0.667, 0.992) * 5.0`, `d=2e-6`, `start=20000` | `(0.75, 0.65, 0.20)` (`1.5`) |
| **Ch 4 — Ropeburn** (`Subway_p` + `Subway_Ext_Lgts`) | `(0.30, 0.52, 0.65)` | `SkyLight_0`: `(1.000, 0.988, 0.910) * 0.80` (`Baker`: `(0.957, 0.988, 1.0) * 1.2`) | `(0.694, 0.780, 0.878) * 0.0` (`Plat`: `(0.824, 0.969, 0.969) * 0.5`) | `(1.000, 0.902, 0.776) * 2.2` (`Rt`: `3.0`) | **`(0.421, 0.619, 0.730)`** (`Stat`: `(0.259, 0.339, 0.599)`) | `(1.000, 0.922, 0.800) * 2.0`, `d=5e-6`, `start=25000` | `(0.30, 0.20, 0.10)` (`0.05`) |
| **Ch 5 — New Eden** (`Mall_p` + `Mall_*_Lgts`) | `(0.17, 0.38, 0.95)` | `SkyLight_0`: `(0.490, 0.502, 0.678) * 2.00` (`bBuiltIntoLightMap=true`) | `(1.000, 1.000, 1.000) * 0.0` | `(1.000, 0.773, 0.545) * 2.0` | `(0.259, 0.339, 0.599)` | `(1.000, 1.000, 1.000) * 0.1`, `d=1e-6`, `start=15000` | `(0.90, 0.60, 0.40)` (`0.1`) |
| **Ch 6 — Pirandello Kruger** (`Factory_p` + `Factory_Ext_Lgts`) | `(0.30, 0.52, 0.65)` (`IBLIntensity=0`) | `SkyLight_0`: `(1.000, 1.000, 1.000) * 0.30` (`Lbay`: `(1.0, 0.929, 0.851) * 0.5`) | `(1.000, 1.000, 1.000) * 0.0` | `(1.000, 0.902, 0.776) * 2.2` (`Rt`: `2.5`, `Photons=1.4`) | `(0.421, 0.619, 0.730)` | `(1.000, 0.945, 0.882) * 0.5`, `d=2e-5`, `start=12000` | `(0.30, 0.20, 0.10)` (`0.2`) |
| **Ch 7 — The Boat** (`Boat_p` + `Boat_Ext_Lgts`) | `(0.083, 0.20, 0.40)` | `SkyLight_0`: `(1.000, 1.000, 1.000) * 0.20` (`bBuiltIntoLightMap=true`) | `(1.000, 1.000, 1.000) * 0.0` | `(0.729, 0.875, 0.745) * 0.025` (Night!) | `(0.083, 0.200, 0.400)` | `d=0` (disabled) | `(0.80, 1.00, 1.00)` (`1.0`) |
| **Ch 8 — Kate** (`Convoy_p` + `Convoy_Ext_Lgts`) | `(0.30, 0.70, 1.00)` | `SkyLight_1`: `(1.000, 0.973, 0.882) * 1.00` (`SkyLight_0`: `(0.718, 0.737, 0.765) * 0.90`) | `(0.216, 0.192, 0.141) * 0.50` | `(1.000, 0.961, 0.882) * 2.5` (`Rt`: `2.0`) | **`(0.205, 0.298, 0.851)`** | `(0.541, 0.851, 1.000) * 1.0`, `d=2e-5`, `start=50000` | `(4.00, 4.00, 4.00)` (`0.1`) |
| **Ch 9 — The Shard** (`Scraper_p` + `Scraper_*_Lgts`) | `(0.00, 0.10, 0.20)` | `SkyLight_0` (`Heli`): `(0.337, 0.502, 0.922) * 0.25` (`Shaft`: `(0.063, 0.282, 0.553) * 0.5`) | `(0.745, 0.588, 0.471) * 0.10` (`Lobby`: `(0.424, 0.584, 0.851) * 1.0`) | `(0.741, 0.788, 0.824) * 1.3` (`Lobby`) | `(0.063, 0.282, 0.553)` | `(0.235, 0.596, 1.000) * 0.0` | `(2.00, 1.00, 0.50)` (`0.1` in `Heli`) |

### Key Observations from the Retail Data

1. **Why *Mirror's Edge* Shadows Are Cool Azure Blue**:
   - In every daytime outdoor chapter (`Tutorial`, `Edge`, `Escape`, `Cranes`, `Convoy`), the directional sun light is warm golden-white (`LightColor = (1.0, 0.90–0.96, 0.75–0.88)`, `Brightness = 2.0–3.45`), while `ModShadowColor` is explicitly tuned to a saturated **sky-azure blue** (`(0.494, 0.659, 0.875)` in `Tutorial`, `(0.530, 0.745, 0.932)` in `Edge`, `(0.205, 0.426, 0.767)` in `Cranes`) and `WorldInfo.SkyColor` is `(0.30, 0.52–0.70, 0.65–1.00)`.
2. **Upper vs. Lower Hemisphere Warm/Cool Contrast**:
   - On `SkyLightComponent`, the upper hemisphere (`LightColor`) is cool sky blue (`(0.643, 0.702, 0.851)` in `Tutorial`, `(0.620, 0.749, 0.855)` in `Edge`), while the lower hemisphere (`LowerColor`) is warm sun-bounced concrete/terracotta (`(0.737, 0.627, 0.447)` in `Tutorial`, `(0.980, 0.957, 0.922)` in `Edge`).
   - Because `GetMaterialHemisphereLightTransferFull` uses quadratic weights $w_{\text{upper}} = (\frac{1+N_z}{2})^2$ and $w_{\text{lower}} = (\frac{1-N_z}{2})^2$, upward-facing roofs receive crisp cool sky light, downward-facing overhangs/soffits receive warm ground bounce, and vertical walls ($N_z = 0$) receive $0.25\,\text{Upper} + 0.25\,\text{Lower}$ plus Beast indirect bounce!
3. **`bUseBakerColorAndBrightness` & `PhotonIntensity = 1.4`**:
   - DICE frequently decoupled the Beast bake sun (`BakerColor`, `BakerBrightness`) from the real-time sun (`LightColor`, `Brightness`) and boosted `PhotonIntensity = 1.4` so indirect colour bleeding off red/orange/blue/white architectural surfaces is stronger than physically energy-conserving Lambertian bounce.
