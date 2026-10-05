# Mirror's Edge: Binary & Shaders Reverse Engineering Specification

## Part 1: PE Binary Architecture (`Binaries/MirrorsEdge.exe`)

### 1.1 Executable Overview
- **File**: `/Users/tomnom/mirrorsedge/Binaries/MirrorsEdge.exe`
- **File Size**: 31,946,072 bytes (30.46 MB)
- **Format**: Windows PE32 (Portable Executable 32-bit)
- **Target Machine**: `0x014c` (`IMAGE_FILE_MACHINE_I386` / x86 32-bit Intel 80386+)
- **Linker Timestamp**: `1231421425` -> `2009-01-08 13:30:25 UTC`
- **Original Export Module Name**: `TdGame-ShippingPC.exe`
- **Image Base**: `0x00400000`
- **Entry Point RVA**: `0x01f1f2ed` (in `.bind` section, pointing to SecuROM initial unpacking thunk; original engine entry point unpacked into `.text` at `0x00001000`+)
- **Image Size**: `0x01f73000` (32.98 MB in memory)
- **Section Alignment**: `0x1000` (4096 bytes)
- **File Alignment**: `0x1000` (4096 bytes)
- **Subsystem**: `2` (`IMAGE_SUBSYSTEM_WINDOWS_GUI`)
- **Characteristics**: `0x0103` (`IMAGE_FILE_RELOCS_STRIPPED=0`, `IMAGE_FILE_EXECUTABLE_IMAGE=1`, `IMAGE_FILE_32BIT_MACHINE=1`)

---

### 1.2 PE Section Table
The binary contains 6 sections with distinct characteristics:

| Section | Virtual RVA | Virtual Size | Raw Offset | Raw Size | Entropy | Characteristics | Purpose |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **`.text`** | `0x00001000` | `0x01689b5a` (22.53 MB) | `0x00001000` | `0x0168a000` (22.54 MB) | 6.56 | `0x60000020` (Execute/Read, Code) | UE3 engine core, DICE game logic, physics integration, script VM |
| **`.rdata`** | `0x0168b000` | `0x004c7810` (4.78 MB) | `0x0168b000` | `0x004c8000` (4.78 MB) | 5.38 | `0x40000040` (Read-only, Init Data) | Import Address Table (IAT), string tables, RTTI type descriptors, vtables |
| **`.data`** | `0x01b53000` | `0x00141c44` (1.25 MB) | `0x01b53000` | `0x00045000` (276 KB) | 5.62 | `0xc0000040` (Read/Write, Init Data) | Global engine singletons (`GEngine`, `GWorld`, `GNatives`), static variables |
| **`.rsrc`** | `0x01c95000` | `0x00095540` (597 KB) | `0x01b98000` | `0x00096000` (600 KB) | 2.19 | `0x40000040` (Read-only, Init Data) | Application icons (`ME_Icon.ico`), cursors, version resources |
| **`.reloc`** | `0x01d2b000` | `0x001f3228` (1.95 MB) | `0x01c2e000` | `0x001f4000` (1.95 MB) | 6.65 | `0x42000040` (Discardable/Read, Init Data) | Base relocation table for ASLR / DLL rebasing |
| **`.bind`** | `0x01f1f000` | `0x00054000` (336 KB) | `0x01e22000` | `0x00054000` (336 KB) | 7.99 | `0x60000040` (Execute/Read, Init Data) | SecuROM wrapper envelope, stub decryption, activation export |

---

### 1.3 Import Table & Dynamic Libraries (38 Imported DLLs)
The shipping binary imports 3,141 external functions across 38 libraries:

```
[Graphics & Direct3D]
  ├── d3d9.dll (Direct3DCreate9)
  ├── d3dx9_35.dll (10 functions: D3DXCheckVersion, D3DXCreateMesh, D3DXSimplifyMesh)
  └── d3dx10_35.dll (4 functions: D3DX10CheckVersion, D3DX10CompileFromMemory, D3DXMatrixInverse)

[Physics & Simulation]
  ├── PhysXExtensions.dll (NxCreateExtension)
  └── [Delay-loaded / dynamic]: PhysXCore.dll (2.8.1), NxCharacter.dll, NxCooking.dll

[Audio & Media]
  ├── binkw32.dll (19 functions: _BinkOpenDirectSound@4, _BinkNextFrame@4, _BinkCopyToBuffer@28)
  ├── vorbis.dll (16 functions: vorbis_info_clear, vorbis_dsp_clear, vorbis_block_clear)
  ├── vorbisfile.dll (7 functions: ov_clear, ov_pcm_total, ov_info, ov_read)
  ├── vorbisenc.dll (vorbis_encode_init_vbr)
  ├── ogg.dll (6 functions: ogg_stream_clear, ogg_page_eos, ogg_stream_pageout)
  ├── libresample.dll (3 functions: resample_open, resample_process, resample_close)
  └── [Delay-loaded / dynamic]: OpenAL32.dll, wrap_oal.dll

[wxWidgets 2.8 Framework (UnrealEd Tools embedded in binary)]
  ├── wxmsw28u_core_vc_custom.dll (1,957 functions: wxWindow, wxFrame, wxControl, wxButton)
  ├── wxmsw28u_vc_custom.dll (236 functions: wxString, wxConfig, wxConvAuto)
  ├── wxmsw28u_adv_vc_custom.dll (211 functions: wxWizard, wxCalendar, wxTaskBarIcon)
  ├── wxmsw28u_html_vc_custom.dll (63 functions: wxHtmlWindow, wxHtmlContainerCell)
  ├── wxmsw28u_aui_vc_custom.dll (33 functions: wxAuiManager, wxAuiPaneInfo)
  └── wxmsw28u_xrc_vc_custom.dll (7 functions: wxXmlResource)

[Illumination & Baking]
  └── ILPointCloudLib.dll (4 functions: ILCreateCloud, ILGetNearestPointValue, ILIsValidCloud)

[Input & System]
  ├── DINPUT8.dll (DirectInput8Create)
  ├── XINPUT1_3.dll (XInputGetState, XInputSetState)
  ├── WINMM.dll (14 functions: waveOutOpen, timeGetTime)
  ├── IMM32.dll (6 functions: ImmCreateContext, ImmGetContext)
  ├── POWRPROF.dll (CallNtPowerInformation)
  ├── WS2_32.dll / WSOCK32.dll (Winsock networking)
  ├── MSVCR80.dll / MSVCP80.dll (Visual C++ 2005 SP1 CRT & STL)
  └── KERNEL32 / USER32 / GDI32 / ADVAPI32 / SHELL32 / OLE32 / CRYPT32 / dbghelp.dll
```

---

### 1.4 Native UnrealScript-to-C++ Execution Thunks (`int[AU]Td*exec*`)
In Unreal Engine 3, native script functions declared as `native` or `exec` in UnrealScript `.uc` classes have their C++ implementations hooked via native thunk functions conforming to the signature:
```cpp
void ClassName::execFunctionName(FFrame& Stack, RESULT_DECL);
// Mapped in GRegisterNative or symbol tables as int<ClassName>exec<FunctionName>
```

Our reverse-engineering extraction identified **300+ native execution thunks** in `MirrorsEdge.exe`. Below is the complete catalog of primary gameplay, pawn, movement, and camera systems:

#### 1.4.1 Player Pawn & Controller (`ATdPawn`, `ATdPlayerPawn`, `ATdPlayerController`)
- **`ATdPawn`**:
  - `CanDropWeapon`: Checks if the active weapon can be dropped/holstered.
  - `GetSprintAcceleration`, `GetWalkAcceleration`: Dynamic acceleration curves based on momentum.
  - `GetAverageSpeed`: Calculates sliding window velocity magnitude.
  - `GetMobilityMultiplier`: Speed modifier based on equipped weapon weight class.
  - `GetAimMode`, `GetWeaponType`: Stance and equipped weapon classification.
  - `InitMoveObjects`: Allocates and initializes `UTdMove` state objects for all 55 moves.
  - `IsInMove`: Fast integer move ID query.
  - `RegenerateHealth`: Regenerative health state machine tick.
  - `UpdateVelocityVariables`: Updates forward, lateral, and vertical speed components.
  - `UpdateLegToWorldMatrix`, `SyncLegMovement`: First-person visible body leg alignment.
  - `PlayCustomAnim`, `StopCustomAnim`, `ReplicateCustomAnim`: First-person hand/leg animation triggers.
- **`ATdPlayerPawn`**:
  - `CheckAgainstWall`: Collision sweep to detect adjacent walls for wall-run and wall-climb initiation.
  - `CheckValidFloor`: Steepness and traction validation for walkable surfaces.
  - `UpdateAgainstWall`: First-person posture tuck when facing close obstacles.
  - `Update1pArms`: First-person weapon and hand positioning solver.
  - `MAT_BeginAnimControl`, `MAT_FinishAnimControl`, `MAT_SetAnimPosition`, `MAT_SetAnimWeights`: Matinee cutscene control.
- **`ATdPlayerController`**:
  - `GetMeleeTarget`, `GetAATarget`: Auto-aim and melee targeting acquisition.
  - `GetInvViewProjection`: Inverse view-projection matrix calculation for screen-to-world raycasts.
  - `SetContrast`, `SetGamma`, `SetNearClippingPlane`: Camera post-processing parameter updates.
  - `SetControllerTiltActive`, `SetUseTiltForwardAndBack`: Sixaxis / motion tilt inputs.
  - `LocalEnemyActors`, `MaintainEnemyList`: Tactical awareness manager.
  - `CheckCutsceneSkippable`, `SkipCutscene`: Bink video and in-engine cutscene skipping.

#### 1.4.2 Movement System Core (`UTdMove` & Specialized Move Classes)
- **`UTdMove` (Base Class)**:
  - `MovementTrace`: Multi-point collision sweep for parkour geometry discovery.
  - `MovementTraceForBlocking`, `MovementTraceForBlockingEx`, `MovementTraceForBlockingBetweenActors`: Hull collision tests.
  - `FindLedge`, `FindLedgeEx`, `FindLedgeInFrontOfPlayer`: Ledge detector measuring top edge height, normal, and grab clearance.
  - `CalculateRelativeExtent`: Capsule bounds adjustment during crouch/slide/coil.
  - `TestCanUnCrouch`: Clearance raycast above player before exiting low-profile moves.
  - `GetMovementExclusionVolume`: Checks if current volume disables specific moves.
- **`UTdMove_WallRun`**:
  - `FindWallSide`: Sweeps lateral rays (left/right $\pm 90^\circ$) to find planar wall surface.
  - `FindWallForward`: Sweeps forward ray ($\pm 57^\circ$) to find upcoming wall termination or corners.
- **`UTdMove_WallClimb`**:
  - `DetectPossibleHandPlant`: Validates horizontal ledge lip above player ($Z \le 280\text{ cm}$).
  - `CheckDoubleJump`: Validates upward wall-kick / tic-tac chaining.
- **`UTdMove_Swing`**:
  - `CheckForTargetVolume`: Detects horizontal pipes / bars.
  - `GetPawnLocation`, `GetPawnAngle`: Pendulum physics solver.
  - `CanShimmy`, `UpdateShimmy`: Lateral hand-over-hand bar traversal.
- **`UTdMove_Grab` & `UTdMove_GrabTransfer`**:
  - `CheckWallLegPlacement`: Pushes feet against vertical walls below hang position.
  - `IsHangingFree`: True if feet have no contact surface behind them (dangling over abyss).
  - `CheckContextMove`: Context-sensitive transition from grab to pipe/ladder/ledge.
- **`UTdMove_Slide`**:
  - `FloorDeclineTooSteep`: Slope angle check to prevent sliding down vertical cliffs.
- **`UTdMove_Jump` & `UTdMove_Landing`**:
  - `IsOkToJump`: Checks cooldown, stance, and footing.
  - `IsLandingOnSoftObject`: Checks if landing target is mattress, trash container, or air bag (cancels fall damage).

#### 1.4.3 Runner Vision & Environment Interactivity
- **`UTdLOIAddOnObject`**:
  - `ActivateLOIGroups`: Activates runner vision highlight for targeted geometry groups.
  - `RegisterLOIGroups`: Indexes interactive objects (pipes, springboards, red doors, ziplines).
- **`ATdWeapon`**:
  - `LOINotify`: Signals Runner Vision to highlight dropped or disarmable enemy weapons in vivid red.

#### 1.4.4 Artificial Intelligence & Pathfinding
- **`ATdAIController`** (54 methods):
  - `TdMoveTo`: Custom parkour-aware path follower.
  - `CheckFireCondition`, `FindGoodFiringPosition`, `FindSuppressionSpot`, `WeAreSuppressed`.
  - `CanSeePlayerFromPoint`, `WillShotHitPlayer`: Line-of-sight and ballistic trajectory solvers.
  - `OkToEvade`, `ThrowGrenade`, `SelectAdvancePoint`, `UpdateAggressionLevel`.
- **`ATdAIManager`**:
  - `SetLastSeenLocation`, `FindPredictedLastSeenLocation`: Player pursuit prediction.
  - `FindBestTaserSpot`: Close-quarters taser unit positioning.

---

## Part 2: Custom Shaders & Rendering Pipeline Specification

Mirror's Edge runs a heavily customized post-processing and lighting pipeline developed by DICE Stockholm on top of UE3, defining the game's clean, high-saturation, white-and-red visual identity.

### 2.1 Complete Inventory of Custom `.usf` Shaders

| Shader File | Stage | Purpose |
| :--- | :--- | :--- |
| `TdToneMappingPixelShader.usf` | Pixel | High-contrast tone mapping, mid-tone power curves, saturation masking, color curve LUTs |
| `TdToneMappingVertexShader.usf` | Vertex | Full-screen post-process quad coordinate generation |
| `TdToneMapExposurePixelShader.usf`| Pixel | Dynamic eye-adaptation / auto-exposure with non-linear rise/fall speed clamping |
| `TdDirHazePixelShader.usf` | Pixel | Atmospheric directional sun haze scattering with depth attenuation |
| `TdDirHazeVertexShader.usf` | Vertex | World vector & view forward vector generation for sun angle calculation |
| `TdMotionBlurShader.usf` | Pixel | 8-tap directional/radial speed blur based on pawn velocity and look direction |
| `TdCalibrationShader.usf` | Pixel | Video calibration screen test pattern (0.5% and 95% black/white validation swatches) |
| `TdClearPixelShader.usf` | Pixel | Fast scene clear pass to constant neutral gray `(0.5, 0.5, 0.5, 1.0)` |
| `TdInterpolatePixelShader.usf` | Pixel | Two-texture temporal blend pass `lerp(TexA, TexB, t)` |
| `TdUIBlurGatherPixelShader.usf` | Pixel | Downsampled bloom/UI blur gather filter with HDR bright-pass accumulation |
| `TdUIBlurEffectPixelShader.usf` | Pixel | Zoomed menu blur overlay with quadratic fade |
| `TdUIApproximateAlphaPixelShader.usf`| Pixel | DICE fake-alpha reconstruction for UI elements rendered without dedicated alpha channels |
| `TdUICompositingPixelShader.usf` | Pixel | Alpha-blended composite of final UI render target over 3D scene |
| `BasePassPixelShader.usf` | Pixel | Modified with Henrik's custom sRGB lightmap curve and NVIDIA bicubic filtering |
| `DirectionalLightPixelShader.usf` | Pixel | Static shadow-masked directional sun pass with light transfer accumulation |

---

### 2.2 Mathematical Formulations of the Custom Pipeline

#### 2.2.1 Dynamic Auto-Exposure (`TdToneMapExposurePixelShader.usf`)
To simulate the blinding transition from dark stairwells to sunlit rooftops:
1. Sample downsampled average scene color $C_{\text{avg}}$:
   $$\text{Luminosity} = 0.30 \cdot R_{\text{avg}} + 0.59 \cdot G_{\text{avg}} + 0.11 \cdot B_{\text{avg}}$$
2. Compute raw target exposure $E_{\text{target}}$:
   $$E_{\text{target}} = \text{clamp}\left(\sqrt{\frac{0.25}{\text{clamp}(\text{Luminosity}, 10^{-7}, 5000.0)}}, \text{LowClamp}, \text{HighClamp}\right)$$
3. Adapt exposure over time with asymmetric rise/fall clamping:
   $$\Delta E = |E_{\text{target}} - E_{\text{last}}|$$
   $$E_{\text{adapted}} = E_{\text{last}} + \text{clamp}\left((E_{\text{target}} - E_{\text{last}}) \cdot \Delta E, -\text{MaxDeltaDown} \cdot (\Delta E)^2, \text{MaxDeltaUp} \cdot (\Delta E)^2\right)$$
4. Final exposure output written to a $1 \times 1$ texture:
   $$\text{OutExposure} = \frac{E_{\text{adapted}}^2 \cdot \text{ManualScale}}{64}$$

---

#### 2.2.2 Tone Mapping & Color Grading (`TdToneMappingPixelShader.usf`)
DICE replaced Epic's standard tone mapper with a high-key photographic transfer curve:
1. Recover current exposure:
   $$E = \text{tex2D}(T_{\text{exposure}}, (0.5, 0.5)) \cdot 64$$
2. Power and contrast curve:
   $$C_1 = \text{pow}\left(\text{saturate}(C_{\text{scene}} \cdot E) \cdot \frac{1}{\text{SceneHighLights}} - \text{SceneShadows}, \text{SceneMidTones}\right)$$
3. Luminance desaturation and overlay tint:
   $$\text{ScaledLuminance} = C_1 \cdot (\text{LuminanceWeights} \cdot \text{SceneDesaturation})$$
   $$C_2 = \text{GammaOverlayColor} + C_1 \cdot (1 - \text{SceneDesaturation}) + \text{ScaledLuminance}$$
4. Gamma color scale:
   $$C_3 = \text{pow}(\text{saturate}(C_2 \cdot \text{GammaColorScale}), \text{GammaInverse})$$
5. 16-Segment Piece-wise Curve Adjustment (PC / Console LUT):
   - On PC, uses 2D LUTs `ColorCurvesKTexture` and `ColorCurvesMTexture` with coordinate scale $\frac{15}{16}$:
     $$R_{\text{final}} = C_{3,r} \cdot K_r(C_{3,r}) + K_g(C_{3,r})$$
     $$G_{\text{final}} = C_{3,g} \cdot K_b(C_{3,g}) + K_a(C_{3,g})$$
     $$B_{\text{final}} = C_{3,b} \cdot M_r(C_{3,b}) + M_g(C_{3,b})$$
   This guarantees that white buildings remain stark and pure ($> 0.95$) while shadows retain crisp contrast.

---

#### 2.2.3 Directional Atmospheric Sun Haze (`TdDirHazePixelShader.usf`)
Creates the blinding city sun glare and atmospheric depth perspective:
1. Compute view direction and depth correction:
   $$V = \frac{\mathbf{WorldVector}}{\|\mathbf{WorldVector}\|}, \quad Z_{\text{corrected}} = \text{min}\left(65535, \text{SceneDepth} \cdot \frac{\|\mathbf{WorldVector}\|}{\|\mathbf{Forward}\|}\right)$$
2. Depth attenuation:
   $$\text{DepthFactor} = \text{clamp}\left(\left(\frac{Z_{\text{corrected}}}{\text{HazePacked}.a}\right)^{\text{HazePacked}.b}, 0, 500\right)$$
3. Solar alignment:
   $$\text{SunView} = \text{pow}\left(\text{clamp}\left(\frac{\mathbf{SunVector} \cdot V + \text{HazePacked}.g}{1 + \text{HazePacked}.g}, 0, \text{HazePacked2}.r\right), \text{HazePacked}.r\right)$$
4. Final additive composition:
   $$C_{\text{final}} = C_{\text{scene}} + \text{Multiplier} \cdot \text{clamp}(\text{SunView} \cdot \text{HazeColor} \cdot \text{DepthFactor}, \text{TotalClampLow}, \text{HazeClamp})$$

---

#### 2.2.4 Radial Velocity Motion Blur (`TdMotionBlurShader.usf`)
Active during high-speed parkour runs, slides, and free-falls:
1. Compute screen-center vector:
   $$\mathbf{D} = 2 \cdot \left(-0.5 + (1 - \text{ScreenPos}_y, \text{ScreenPos}_x)\right), \quad r = \|\mathbf{D}\|$$
2. Non-linear radial expansion:
   $$\Delta r = \text{clamp}\left(2^{0.1 \cdot \log_2(r)} - 0.95, 0, 0.07\right) \cdot \text{MotionPacked}.r$$
3. 8-tap weighted blur accumulation along offset vector $\mathbf{v}_{\text{step}} = \frac{\mathbf{D}}{r} \cdot \frac{\Delta r}{8}$:
   $$C_{\text{blur}} = \frac{1}{\sum_{k=0}^7 w_k} \sum_{k=0}^7 w_k \cdot \text{tex2D}\left(T_{\text{scene}}, \mathbf{uv} + k \cdot \mathbf{v}_{\text{step}}\right)$$
   Where $w_k = 1.0 - \frac{k}{8}$, normalized by $\text{INV\_TOTAL\_WEIGHT} = \frac{1}{36} \approx 0.02778$.

---

#### 2.2.5 UI Fake-Alpha Reconstruction (`TdUIApproximateAlphaPixelShader.usf`)
Because UE3 rendered UI into color buffers without dedicated destination alpha channels, DICE Stockholm invented an analytical algorithm to reconstruct exact alpha transparency:
1. Examine green vs. blue channel levels relative to background clear color:
   $$\alpha_{\text{est}} = \begin{cases} 1.0 - G & \text{if } G = B \text{ (pure clear color)} \\ 1.0 - B & \text{if } G > B \text{ (UI green contribution)} \\ 1.0 - G & \text{if } B > G \text{ (UI blue contribution)} \end{cases}$$
2. Clamp against red channel:
   $$\alpha = \max(\alpha_{\text{est}}, R)$$
3. Color restoration (un-premultiply) and desaturation:
   $$R' = \frac{R}{\alpha}, \quad G' = 1 + \frac{G - 1}{\alpha}, \quad B' = 1 + \frac{B - 1}{\alpha}$$
   $$C_{\text{final}} = \text{lerp}\left(\frac{R' + G' + B'}{3}, (R', G', B'), \alpha^2\right)$$

---

#### 2.2.6 Runner Vision (`LOI`) Architecture
- **Object Tagging**: Objects marked as parkour conduits (pipes, ramps, vault boxes, escape doors) are bound to `UTdLOIRenderingComponent` and `UTdLOIGroupManager`.
- **Material Parameter**: A scalar parameter `LOI_Strength` ($[0.0, 1.0]$) is exposed to all Runner Vision materials.
- **Dynamic Response**:
  - `DefaultLOI.ini`: `FadeInSpeed = 1.0f`, `FadeOutSpeed = 4.0f`.
  - When Faith approaches or focuses toward the object, `LOI_Strength` ramps up at $1.0\text{ s}^{-1}$.
  - The material blends its base diffuse color toward pure saturated scarlet red `RGB(0.95, 0.02, 0.02)` with a slight specular glint.
  - When looking away or passing the obstacle, it rapidly fades out at $4.0\text{ s}^{-1}$ ($0.25\text{ s}$ total fade duration).

---

## Part 3: Apple Metal Translation Guidelines (`mierrorsedgere`)
For the clean-room macOS engine, all 14 `.usf` HLSL shaders translate directly into Metal Shading Language (MSL 2.4/3.0):
1. **Half Precision**: Use MSL `half3` and `half4` for HDR scene textures to match mobile/Apple Silicon tile memory performance.
2. **Compute vs. Pixel Shaders**: The 8-tap motion blur and UI fake-alpha reconstruction can be implemented as Metal compute kernels (`kernel void`) operating in-place with SIMD tile groups for 240+ FPS performance on Apple Silicon.
3. **Color Curve Textures**: Pack `ColorCurvesKTexture` and `ColorCurvesMTexture` into standard `MTLTextureType2D` with `MTLPixelFormatRGBA16Float` or `R8Unorm` samplers.
