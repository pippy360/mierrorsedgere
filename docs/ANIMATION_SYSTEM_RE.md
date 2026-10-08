# Mirror's Edge (UE3 v536) — Animation & Skeletal Skinning System Reverse Engineering

## 1. Executive Summary

Mirror's Edge (DICE, 2009; Unreal Engine 3 `PackageVersion=536`, `LicenseeVersion=43`, `EngineVersion=3716`) drives both its first-person full-body parkour awareness (`CH_TKY_Crim_Fixer_1P.upk`) and its third-person AI enemies (`CH_TKY_Cop_SWAT.upk`, `CH_TKY_Cop_Patrol.upk`) through a custom skeletal animation subsystem built on top of UE3's `UAnimTree`, `USkeletalMesh`, `TdAnimSet`, and `UAnimSequence`.

All binary structures documented below were reverse-engineered directly from the retail game packages in `/Users/tomnom/mirrorsedge/TdGame/CookedPC/` and implemented natively in C++20/Metal in [`src/anim/anim_system.hpp`](../src/anim/anim_system.hpp) and [`src/anim/anim_system.cpp`](../src/anim/anim_system.cpp).

---

## 2. Animation Class Hierarchy in `TdGame.u` & `DefaultAnimation.ini`

Inspection of `TdGame.u` reveals **79 custom animation classes** split across two subtrees:

### 2.1 Custom `TdAnimNode*` Blend Tree Nodes (68 Classes)
| Class Name | Superclass | Purpose |
|---|---|---|
| `TdAnimNodeSequence` | `AnimNodeSequence` | Custom animation sequence player with root-motion extraction, normalized time syncing, and weapon-pose scaling |
| `TdAnimNodeBlendList` | `AnimNodeBlendList` | Base state-driven multi-child blend node |
| `TdAnimNodeMovementState` | `TdAnimNodeBlendList` | Master parkour state selector (38 states: Walking, Sprint, WallRun, WallClimb, SpringBoard, SpeedVault, ZipLine, CrouchSlide, SkillRoll, Snatch, etc.) |
| `TdAnimNodeWalkingState` | `TdAnimNodeBlendList` | Locomotion velocity blender (`Stand` $\leftrightarrow$ `walkfwd` $\leftrightarrow$ `runfwd` $\leftrightarrow$ `SprintFwd`) |
| `TdAnimNodeInAir` | `TdAnimNodeBlendList` | Airborne state selector (`JumpStart`, `InAir`, `IntoLand`, `Landing`) |
| `TdAnimNodeDirBone` | `TdAnimNodeBlendBase` | Directional bone offset node for upper/lower body aim & lean |
| `TdAnimNodeAimOffset` | `AnimNodeAimOffset` | 1P and 3P weapon aiming additive blend space |
| `TdAnimNodeBlendByArmed` | `TdAnimNodeBlendList` | Switches between Unarmed, Light Weapon (1H Pistol), and Heavy Weapon (2H Rifle/Shotgun) subtrees |
| `TdAnimNodeBlendByFire` | `TdAnimNodeBlendList` | Blends between Relaxed, Ready, Firing, and Reloading states |
| `TdAnimNodeGrabbing` | `TdAnimNodeBlendList` | Ledge hang, shimmy, heave-up, and 45° turn-from-grab blending |
| `TdAnimNodeLanding` | `TdAnimNodeBlendList` | Soft landing, hard landing, and skill roll transitions |
| `TdAnimNodeWallJump` | `TdAnimNodeBlendList` | Wallrun-to-jump blend controller |

### 2.2 Custom `TdSkelControl*` Bone Controllers (9 Classes)
| Class Name | Superclass | Purpose |
|---|---|---|
| `TdSkelControlSpring` | `SkelControlSingleBone` | Procedural angular velocity spring lag on 1P forearms/wrists (`TimeBetweenSpringUpdates = 0.04s`, `SpringPitchInterpVel = 10.0`, `SpringYawInterpVel = 5.0`) |
| `TdSkelControlAim1p` | `SkelControlSingleBone` | First-person arm/weapon pitch & yaw alignment (`UnarmedAimingMomentumThreshold = 100`, `BlendIn = 0.5s`, `BlendOut = 0.2s`) |
| `TdSkelControlLimb` | `SkelControlLimb` | Two-bone analytic IK solver for `LeftHand_GameIK`, ledge contacts, and wallrun hand placement |
| `TdSkelControlFootPlacement` | `SkelControlFootPlacement` | Ground normal alignment and pelvis drop for stairs/ramps |
| `TdSkelControlLookAt` | `SkelControlLookAt` | Head/eye tracking (`HeadRotationYawLimit = 90°`, `BodyRotationYawLimit = 10°`, `AiLookAtInterpolationSpeed = 12.5`) |
| `TdSkelControlRecoil` | `SkelControlBase` | Procedural weapon recoil impulse solver |

### 2.3 Exact Blend Timings & Velocity Thresholds (`DefaultAnimation.ini`)
- **`[TdGame.TdAnimNodeCharacterMovement]`**:
  - `SneakVel = 1.0`, `WalkVel = 200.0`, `RunVel = 380.0`, `RunAccToRunVel = 550.0`, `StartBlendingInSprint = 690.0`, `FullSprint = 700.0`
  - `IntoIdleBlendTime = 0.2s`, `IntoWalkBlendTime = 0.1s`, `IntoRunBlendTime = 0.5s`, `IntoSprintBlendTime = 0.1s`, `StopBlendTime = 0.2s`
- **`[TdGame.TdAnimNodeInAir]`**:
  - `StartOfJumpBlendTime = 0.1s`, `InAirBlendTime = 1.0s`, `IntoLandBlendTime = 1.0s`, `FallVelThreshold = 100.0`, `FallToDeathThreshold = 1800.0`, `StartOfLongJumpMomentumThreshold = 380.0`
- **`[TdGame.TdAnimNodeMovementState]`**:
  - `GoesIntoWallRunBlendTime = 0.6s`, `GoesIntoJumpBlendTime = 0.1s`, `GoesIntoInAirBlendTimeFromWallJump = 0.5s`
- **`[TdGame.TdAnimNodeLanding]`**:
  - `GoesFromLandingToWalkingBlendTime = 0.1s`, `GoesIntoLandingBlendTime = 0.0s`

---

## 3. Cooked Animation & Skeletal Mesh Asset Catalog

| Package (`.upk`) | Primary Exports | Details |
|---|---|---|
| `Characters/CH_TKY_Crim_Fixer_1P.upk` | `SK_UpperBody`, `SK_LowerBody`, `AT_C1P`, `Female_1p_C`, `Faith_Glove_C` | Faith 1P upper body (`74` bones, `4,037` verts, `6,876` tris), lower body (`74` bones, `3,120` verts, `5,378` tris), 235-node 1P `AnimTree` |
| `Characters/CH_Faith_Cinematic.upk` | `CH_Faith_Cinematic`, `Faith_Cine_Lower_C`, `Faith_Cine_Upper_C` | Faith full-body mesh (`70` bones, `8,096` verts) & 2048x2048 `PF_DXT1` diffuse textures |
| `Characters/CH_TKY_Cop_SWAT.upk` | `CH_TKY_Cop_SWAT`, `T_TKY_Cop_SWAT_D`, `T_TKY_Cop_SWAT_S` | KrugerSec / CPF Tactical SWAT Officer (`88` bones, `7,776` verts, `12,413` tris) & 2048x2048 `PF_DXT1` diffuse/specular textures |
| `Weapons/WP_Colt1911.upk` | `SK_Colt1911`, `T_Colt1911_D`, `T_Colt1911_S` | Colt 1911 handgun (`8` bones, `2,728` verts, `2,256` tris) & 512x512 `PF_DXT1` textures |
| `Animations/AS_C1P_Unarmed.upk` | `AS_C1P_Unarmed` (`TdAnimSet`) | `281` `UAnimSequence` exports across `74` bone tracks (all parkour moves) |
| `Animations/AS_C1P_OneHanded_Common.upk` | `AS_C1P_OneHanded_Common` (`TdAnimSet`) | `144` `UAnimSequence` exports (armed parkour, `SnatchFwd` disarms, melee) |
| `Animations/AS_C1P_OneHanded_Colt1911.upk` | `AS_C1P_OneHanded_Colt1911` (`TdAnimSet`) | `5` `UAnimSequence` exports (`standfire`, `reload`, `melee`, weapon sub-tracks) |
| `Animations/AS_AI_PatrolCop_OneHanded.upk` | `AT_Cop`, `AS_AI_PatrolCop_OneHanded` | 176-node AI `AnimTree` + `119` `UAnimSequence` exports (`97` full-body `70`-track sequences + weapon sub-sequences) |

---

## 4. UE3 v536 Binary Format Specifications

### 4.1 Critical `FPropertyTag` Quirk in UE3 PackageVersion 536
In later UE3 versions (`v584+`), `ByteProperty` tags include an 8-byte `FName EnumName` in the tag header before the property value. In **Mirror's Edge (`PackageVersion=536`, `LicenseeVersion=43`)**, `ByteProperty` has **no `EnumName` in the tag header**—its payload (`Size=1` for raw `uint8` or `Size=8` for `FName` enum values like `ACF_Fixed48NoW`) immediately follows `ArrayIndex`. Failing to account for this shifts the stream by 8 bytes on any object containing a `ByteProperty` (such as `UAnimSequence::RotationCompressionFormat` and `UTexture2D::Format`), truncating subsequent properties (`CompressedTrackOffsets`, `CompressedByteStream`).

### 4.2 `USkeletalMesh` Post-Property Binary Layout
Immediately following the terminating `None` `FPropertyTag`:

1. **Header (`32` bytes)**:
   - `int32 Version` (`1`)
   - `FBoxSphereBounds Bounds` (`FVector Origin` 12B, `FVector BoxExtent` 12B, `float SphereRadius` 4B)
2. **Materials (`TArray<UMaterialInterface*>`)**:
   - `int32 Count`, followed by `Count * int32` object references
3. **Mesh Origin & Rotation Origin (`24` bytes)**:
   - `FVector Origin` (`3 * float`, typically `(0.0, 94.0, 0.0)`)
   - `FRotator RotOrigin` (`3 * int32`: `Pitch=0, Yaw=-16384, Roll=16384`)
4. **Reference Skeleton (`TArray<FMeshBone>`)**:
   - `int32 BoneCount`, followed by `BoneCount * 52` bytes:
     ```cpp
     struct FMeshBoneDisk {
         int32_t  name_index;   // +0x00: FName index
         int32_t  name_number;  // +0x04: FName instance number
         uint32_t bone_flags;   // +0x08: Bone flags
         float    quat[4];      // +0x0C: FQuat (X, Y, Z, W) local bind rotation
         float    pos[3];       // +0x1C: FVector (X, Y, Z) local bind translation
         int32_t  num_children; // +0x28: Child count
         int32_t  parent_index; // +0x2C: Parent bone index (0 for root)
         int32_t  bone_color;   // +0x30: Editor color (B, G, R, A)
     }; // 52 bytes
     ```
5. **`int32 SkeletalDepth`** & **`int32 LODModelsCount`**
6. **`FStaticLODModel` (`LODModels[0]`)**:
   - **`Sections` (`TArray<FSkelMeshSection>`)**: `int32 SectionCount`, followed by `SectionCount * 10` bytes (`uint16 MaterialIndex`, `uint16 ChunkIndex`, `int32 BaseIndex`, `uint16 NumTriangles`)
   - **`IndexBuffer` (`FMultiSizeIndexContainer`)**: `int32 DataTypeSize` (`2` or `4`), `int32 IndexCount`, followed by `IndexCount * DataTypeSize` bytes
   - **`ShadowIndices` (`TArray<uint16>`)**: `int32 Count` + `Count * 2` bytes
   - **`UsedBones` (`TArray<uint16>`)**: `int32 Count` + `Count * 2` bytes
   - **`ShadowTriangleDoubleSided` (`TArray<uint8>`)**: `int32 Count` + `Count` bytes
   - **`Chunks` (`TArray<FSkelMeshChunk>`)**: `int32 ChunkCount`, where each chunk contains:
     - `uint32 BaseVertexIndex`
     - `RigidVertices` (`int32 Count` + `Count * 49` bytes):
       - `+0x00`: `FVector Position` (`12B`)
       - `+0x0C`: `FPackedNormal TangentX, TangentY, TangentZ` (`12B`: `3 * 4B` biased `uint8` `(b - 127.5) / 127.5`)
       - `+0x18`: `FVector2D UVs[3]` (`24B`: `3 * 8B` float UV pairs)
       - `+0x30`: `uint8 BoneIndex` (`1B` index into chunk's `BoneMap`)
     - `SoftVertices` (`int32 Count` + `Count * 56` bytes):
       - `+0x00`: `FVector Position` (`12B`)
       - `+0x0C`: `FPackedNormal TangentX, TangentY, TangentZ` (`12B`)
       - `+0x18`: `FVector2D UVs[3]` (`24B`)
       - `+0x30`: `uint8 InfluenceBones[4]` (`4B` indices into chunk's `BoneMap`)
       - `+0x34`: `uint8 InfluenceWeights[4]` (`4B` weights summing to `255`)
     - `BoneMap` (`int32 Count` + `Count * uint16` mapping chunk-local bone indices to `RefSkeleton` bone indices)
     - `int32 NumRigidVertices`, `int32 NumSoftVertices`, `int32 MaxBoneInfluences` (`12B`)

### 4.3 `UAnimSequence` Compressed Track Binary Layout
Each `UAnimSequence` export stores `CompressedTrackOffsets` (`TArray<int32>`, `4 * NumTracks` elements) in its properties and `CompressedByteStream` (`int32 ByteCount` + raw bytes) immediately after the terminating `None` property tag:

- For track $t \in [0, \text{NumTracks}-1]$:
  - `trans_offset = CompressedTrackOffsets[4*t + 0]`
  - `trans_keys   = CompressedTrackOffsets[4*t + 1]`
  - `rot_offset   = CompressedTrackOffsets[4*t + 2]`
  - `rot_keys     = CompressedTrackOffsets[4*t + 3]`
- **Translation Track (`ACF_None`)**:
  - `trans_keys * 12` bytes at `stream + trans_offset` (`FVector` `float x, y, z` per key).
- **Rotation Track (`ACF_Fixed48NoW` & `ACF_Float96NoW`)**:
  - If `rot_keys == 1`: stored as an unquantized `12`-byte `FQuatFloat96NoW` (`float rx, ry, rz`) at `stream + rot_offset`.
  - If `rot_keys > 1`: starts with a `24`-byte `Mins[3] + Ranges[3]` bounding header at `stream + rot_offset`, followed at `stream + rot_offset + 24` by:
    - **`ACF_Fixed48NoW`**: `rot_keys * 6` bytes (`uint16 qx, qy, qz`), dequantized directly over $[-1, +1]$ via:
      $$r_x = \frac{q_x - 32767}{32767}, \quad r_y = \frac{q_y - 32767}{32767}, \quad r_z = \frac{q_z - 32767}{32767}, \quad r_w = \sqrt{\max(0, 1 - r_x^2 - r_y^2 - r_z^2)}$$
    - **`ACF_Float96NoW`**: `rot_keys * 12` bytes (`float rx, ry, rz`).
  - **Quaternion Handedness Alignment**: `UAnimSequence` canonicalizes $w \ge 0$ in UE3's left-handed quaternion convention, whereas `RefSkeleton` stores raw quaternions with $w \le 0$. Negating $r_w$ (`Quat4(rx, ry, rz, -rw).normalized()`) aligns `UAnimSequence` rotations with `RefSkeleton` under standard right-handed quaternion multiplication (`Quat4::multiply(parent, local)`).

---

## 5. Linear Blend Skinning (LBS) & Coordinate Spaces

For each bone $b$, the component-space bind pose $(p_b^{\text{ref}}, q_b^{\text{ref}})$ and animated pose $(p_b^{\text{anim}}, q_b^{\text{anim}})$ are evaluated via forward kinematics from the root bone (`Bone[0]`). The skinning delta transform mapping component-space bind vertices into animated component space is:
$$\Delta q_b = q_b^{\text{anim}} \otimes (q_b^{\text{ref}})^*, \qquad \Delta p_b = p_b^{\text{anim}} - \Delta q_b \cdot p_b^{\text{ref}}$$
Each vertex $v$ with up to 4 bone influences $(b_k, w_k)$ ($\sum w_k = 1$) is skinned via:
$$p_v^{\text{skinned}} = \sum_{k=0}^{3} w_k \left( \Delta q_{b_k} \cdot p_v^{\text{bind}} + \Delta p_{b_k} \right), \qquad n_v^{\text{skinned}} = \text{normalize}\left(\sum_{k=0}^{3} w_k (\Delta q_{b_k} \cdot n_v^{\text{bind}})\right)$$

In Mirror's Edge's Maya/MotionBuilder component space (`RotOrigin = (0, -16384, 16384)`):
- $+X_{\text{raw}}$ = Character Left ($-X_{\text{raw}}$ = Character Right)
- $-Y_{\text{raw}}$ = Character Up ($0$ at feet, $-108.7$ at `Hips`, $-166.2$ at `EyeJoint`)
- $+Z_{\text{raw}}$ = Character Forward

---

## 6. First-Person Camera Animation (`TdPlayerPawn.CalcCamera`)

Retail puts the first-person camera on the animated 1P mesh, not at a fixed eye height:

- **Location**: `Mesh1p`'s `EyeJoint` (bone `72` of `SK_UpperBody`).
- **Rotation**: `GetViewRotation()` (the controller's view) plus the native `GetCameraAnimation()`. That is the `CameraJoint` (bone `73`, identity bind pose) rotation relative to the mesh root, decomposed with `FMatrix::Rotator()` and swizzled from the raw mesh axes into view axes:
  ```
  View.Pitch += -CamRot.Roll;   View.Yaw += CamRot.Pitch;   View.Roll += -CamRot.Yaw;
  ```
  The sum is per component, so an animation can carry the view past ±90° of pitch, all the way round.
- The camera animation comes from the blended mesh pose. It therefore fades in and out with the slot animation that plays it (`PlayMoveAnim(..., BlendIn, BlendOut)`): weight $w = \text{clamp}\left(\min\left(\frac{\tau}{\text{BlendIn}}, \frac{\text{len} - \tau}{\text{BlendOut}}\right), 0, 1\right)$.

### 6.1 `fallinglandroll` (`TdMove_SkillRoll`)

`TdMove_SkillRoll.StartMove` does the following:
- `UseRootMotion(true)`
- `PlayMoveAnim(CNT_FullBody, 'fallinglandroll', 1.0, 0.2, 0.2, bRootMotion)`
- `SetIgnoreMoveInput(-1)` and `SetIgnoreLookInput(-1)`
- `ResetCameraLook(0.2)`
- `Velocity = Acceleration = 0`

`OnCustomAnimEnd` then calls `SetMove(MOVE_Walking)`.

- **Timing**: `1.2667 s`, `38` keys spaced `length / 37` apart (UE3's non-looping `TimeToIndex`).
- **Root bone**:
  - Pure forward translation (raw $+Z$): `21.3 → 334.9`, so `313.5 uu` in all.
  - A single identity rotation key.
  - Root speed starts near 500 uu/s and peaks near 800 uu/s 0.1 s after touchdown. It falls to about 20 uu/s while inverted (0.45 s), holds about 310 uu/s coming up, and exits at 233 uu/s.
- **Camera**: `CameraJoint` has no rotation relative to `EyeJoint` in this sequence. The EyeJoint itself turns one full forward somersault, as below. Yaw stays within ±5° and roll within ±3.5°.

| t (s) | 0 | 0.19 | 0.44 | 0.57 | ~0.8 | 0.95 | 1.267 |
|---|---|---|---|---|---|---|---|
| camera pitch (animation) | −51.3° | −89° | −192.7° | −276° | −360° | −372.8° | −361.6° |
| EyeJoint height above feet | 145 | | 13.3 | | | | 152.9 |

**Retail recording check**: `recordings/20260920_145610_escape_overlay_session.jsonl.gz` captures a skill roll on SP01 at 126 s. Its unwrapped view pitch is:

| t (s) | 0 | 0.199 | 0.42 | 0.514 | 0.652 | 0.8 | 0.953 | 1.25 |
|---|---|---|---|---|---|---|---|---|
| retail camera pitch | −16.5° | −87.7° | −170.4° | −250.5° | −332.6° | −359.7° | −372.8° | −360° |

The model above reproduces it to within about 4°: the view pitch reset to 0 over 0.2 s, plus $w$ times the animation, with 0.2 s blends.

### 6.2 Port

- [`AnimSystem::camera_animation`](../src/anim/anim_system.cpp) returns `CameraAnimation { weight, eye, pitch/yaw/roll }`. It works in these steps:
  1. Sample the sequence at `PlayerTelemetry::combat_anim_time` (the move's clock, restarted on the landing frame).
  2. Run FK.
  3. Measure the EyeJoint from the root bone, since root motion moves the root's translation into the pawn.
  4. Map raw (X left, −Y up, Z forward) to (forward, right, up).
  5. Slerp the CameraJoint rotation from identity by $w$ and apply the swizzle above.

  So far only `MOVE_SkillRoll` plays a camera animation.
- [`AnimSystem::player_camera`](../src/anim/anim_system.cpp) gives the camera position and rotation:
  - Position: a lerp from the standing eyes to the animated EyeJoint, by $w$.
  - Rotation: the view plus the camera animation.

  Both backends' `render_frame` (`MetalRenderer`, `D3D11Renderer`) draw from it, and their `player_camera()` exposes it to the oracle. Oracle Stage 6 checks the somersault: pitch minimum ≤ −355°, ending level at −360°, eyes below 40 uu.
- The viewmodel (`evaluate_faith_1p`) is already built in the EyeJoint's frame, so the arms and legs stay with the turning camera.

### 6.3 Other `AS_C1P_Unarmed` sequences with camera motion (not applied yet)

Peak swizzled camera rotation, measured with the same FK:

| Sequence | Camera motion |
|---|---|
| `fallinglandsoftlanding` | pitch down to −68.6° |
| `fallinglandhard` | pitch down to −53.5° |
| `HangHeaveUp` / `HangFreeHeaveUp` | pitch down to −47.7° / −52.6° |
| `HangFree` | pitch held at +13.2° |
| `LadderEnterTop` | pitch −72.9°..+26°, yaw up to 53° |
| `springboardfail` | pitch down to −58.4° |
| `HangFreeTurnLeft` / `Right` | yaw ∓80° with large pitch and roll |
