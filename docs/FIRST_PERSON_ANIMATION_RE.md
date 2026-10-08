# Mirror's Edge first-person animation: what plays when, and where the hands are

How retail chooses Faith's first-person animations, how the camera relates to her body, and how the port reproduces both. It follows [`ANIMATION_SYSTEM_RE.md`](ANIMATION_SYSTEM_RE.md), which covers the file formats (skeletal meshes, animation sequences, skinning).

Every statement comes from the retail packages and scripts unless it says "measured" (taken from a retail recording or frame) or "not established".

Tools: `tools/ue3_tree.py` (property trees), `tools/uscript_bytecode.py` (script), `tools/retail/` (the retail recorder), the recordings in `recordings/`.

---

## 1. The pieces

| Piece | Where | What |
|---|---|---|
| `TdPlayerPawn.Mesh1p` | `TdGame.u`, `Default__TdPlayerPawn.TdPawnMesh1p` | `TdSkeletalMeshComponent`: `SK_UpperBody`, `AnimTreeTemplate = AT_C1P.AT_C1P`, `AnimSets = [AS_C1P_Unarmed, 4 empty]` (the weapon fills the rest), `FOV = 90`, `SDPG_Foreground`, owner-only |
| `TdPlayerPawn.Mesh1pLowerBody` | `…TdPawnMesh1pLowerBody` | `SK_LowerBody`, `ParentAnimComponent = Mesh1p` (it has no tree: it takes the upper body's pose), `SDPG_Intermediate` |
| the tree | `Characters/AT_C1P.upk` | one `AnimTree`, 203 nodes: 117 sequence players, 11 custom-animation slots, the rest blends and state switches. Also the skeletal controls and morph nodes |
| the sequences | `Animations/AS_C1P_Unarmed.upk` and the weapon sets | named `AnimSequence`s; a node names the one it plays |
| what the moves ask for | `TdGame.u`, `TdMove_*` | each move's script calls `PlayMoveAnim` / `StopCustomAnim` / `SetAnimationMovementState` |

So an animation reaches the screen one of two ways:

* a **state node** of the tree picks a child from the pawn's state (movement state, walking state, weapon, grab slope …) and the child's sequence loops, or
* a move's script plays a **custom animation** by name on one of the tree's slots, over whatever the state nodes are doing.

## 2. The camera

`TdPlayerPawn.CalcCamera`, in full:

```
out_Location = Mesh1p.GetBoneLocation('EyeJoint')           // world space
out_Rotation = GetViewRotation()                            // the controller's view
GetCameraAnimation(CameraAnimLocation, CameraAnimRotation)
out_Rotation.Pitch += -CameraAnimRotation.Roll
out_Rotation.Yaw   +=  CameraAnimRotation.Pitch
out_Rotation.Roll  += -CameraAnimRotation.Yaw
if (SwanNeck1p != None) out_Location += SwanNeck1p.GetSwanNeckPos(yaw-only view rotation)
if (the move wants it and this is not a cinematic) Moves[MovementState].CheckForCameraCollision(out_Location, out_Rotation)
```

The camera is not a point above the capsule. It is the `EyeJoint` bone of the animated first-person skeleton, wherever the animation and the skeletal controls have put it, turned to the controller's view. The hands are where they are in the picture because the same pose holds both the eye and the hands.

The first-person mesh is drawn with its own field of view (`TdSkeletalMeshComponent.FOV = 90`), in the foreground depth group, whatever the world camera's field of view is.

## 3. The tree, top down

```
AnimTree
 WeaponPoseOffset                      TdAnimNodeWeaponPoseOffset
  IkEffectorController                 TdAnimNodeIKEffectorController
   IgnoreRootTransformation            per-bone blend from Hips: Source = a "notifier dummy" branch (footstep and breathing notifies by speed)
    Target: Custom_Canned              slot (cutscenes, intros)
     per-bone blend from SpineX        Target: Custom_CannedUpperBody (slot)
      Camera_Split                     per-bone blend, CameraJoint only. Target: Custom_Camera (slot)
       LandNode                        TdAnimNodeLandOffset: the dip after a landing
        ArmedRight / ArmedLeft         per-bone blends that lay the weapon arm(s) over the body
         AgainstWallCam                TdAnimNodeDirBone
          UpperBodySplit               per-bone blend from SpineX and EyeJoint. Target: Custom_UpperBody (slot)
           Custom_FullBody             slot
            MasterSync                 AnimNodeSynch, group "Walk"
             TdAnimNodeMovementState   Walking / Jump / Falling / Crouch / Vertigo go through "1pAim" (TdAnimNodeDirBone) and the
                                       Custom_FullBody_Dir and Custom_LowerBody slots; everything else goes straight to:
              TdAnimNodeMovementState  one child per movement state (below)
```

The second movement-state node's children, by `StateMapping` (an `EMovement` value each):

| Child | State | Plays |
|---|---|---|
| Default | anything not listed | `WalkingState`: Idle → `Stand` with the stand-turn sequences; Sneak / Walk / Jog+Run → a `TdAnimNodeBlendDirectional` each (`sneakfwd`/`sneakbwd`; `walkfwd`, `walkfwdstiff`, `walkbwd`, `walkbwdstiff`; `runfwd`, `runfwdstiff`, `runbwd`, `runbwdstiff`); Sprint → `SprintFwd` |
| Grabbing | 3 | `Hang` / `HangFree` by slope (`hang45left/right`, `hangfree45left/right`), and the hang-turn idles |
| WallClimbing, Falling, Jump, GrabJump, IntoGrab | 6, 2, 11, 13, 14 | `JumpLand` (a pose; the move's custom animation is what is seen) |
| WallRunningLeft / Right | 5, 4 | `WallrunLeft` / `WallrunRight`, rate 1.4 |
| Slide | 16 | `CrouchSlideEnd` |
| Balance | 29 | `walkbalancefwd` with its lean and lose-balance variants |
| 180TurnInAir | 25 | `jumpturnflyend` |
| LayOnGround | 26 | `jumpturnlandingidle` |
| Climb | 21 | ladder and pipe: the two "still" poses, the slide-down variants, the look-left/right idles |
| ZipLine | 28 | `ZipLine` |
| Crouch | 15 | `crouchfwd`/`crouchbwd` (`…ready` with a heavy weapon), idle `crouchstill` with the crouch-turn sequences |
| rumpslide | 38 | `crouchslideend45` |
| LedgeWalk | 30 | `ledgeright` / `ledgeleft` |
| Vertigo | 47 | `edgedetectionidle` |
| Swing | 60 | `swingposefronttop`, `swingposebackstraight`, `swingposebacktop` by swing phase |
| GrabTransfer | 31 | `hangtransferupidle` |
| fallinguncontrolled | 72 | `fallinguncontrolled` (`fallinguncontrolledbwd` after a 180 turn in the air) |
| SoftLanding | 78 | `fallinglandintosoftlanding` |

The slots are `CustomNodeType`: `CNT_Canned` 0, `CNT_CannedUpperBody` 1, `CNT_FullBody` 2, `CNT_FullBody_Dir` 3, `CNT_UpperBody` 4, `CNT_LowerBody` 5, `CNT_Camera` 6, `CNT_Weapon` 7, `CNT_Face` 8. `TdMove.PlayMoveAnim(Type, AnimName, Rate, BlendInTime, BlendOutTime, bRootMotion, bRootRotation)` is `TdPawn.PlayCustomAnim` with `bLooping = false, bOverride = true`.

## 4. What retail was recorded doing

From telemetry version 6 the recorder's hook walks `Mesh1p`'s tree every frame and logs the three heaviest sequence players: `anim1p = [[name, time in seconds, weight], …]`. Nine of the recordings in `recordings/` have it. Standing still:

```
MOVE_Walking  [["Stand", 0.0161, 1.0], ["StandTurn90Right", 0.0161, 0.0806]]
```

and the first frames of a running jump:

```
MOVE_Jump     [["runfwd", 0.3162, 0.7457], ["JumpSlow", 0.0168, 0.1682], ["JumpLand", 0.0168, 0.07]]
```

`JumpSlow` is the custom animation `TdMove_Jump.StartJump` played; `JumpLand` is the Jump child of the state node coming in under it; `runfwd` is the walking state going out. These records are what the port's choice of animation is checked against.
