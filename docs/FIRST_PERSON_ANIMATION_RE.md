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

## 5. Rules measured on the recordings

What the scripts do not say (the nodes are native) was measured on the nine recordings, 80,238 frames.

**Walking state.** `TdPawn.UpdateWalkingState` follows the 2D speed of the frame before (the pawn ticks before its physics), with the same thresholds speeding up and slowing down. The recorder logs `CurrentWalkingState`, so these are read directly:

| state | from |
| --- | --- |
| Sneak | the first movement |
| Walk | 50 |
| Jog | 260 |
| Run | 400 |
| Sprint | 630 |

**The walk cycle.** Every walking, running, crouching, balancing and wall running sequence is in the synch group `Walk` of `MasterSync`. The heaviest member leads at its own rate (its `Rate` x the sequence's `RateScale` x speed / `BaseSpeed`, clamped to `RateMin`..`RateMax`), and every other member sits at the same place in its own cycle, `SynchPosOffset` apart (0.32 for the forward sequences, 0.78 for the backward ones). A member that becomes relevant starts at its `SynchPosOffset` (`TdAnimNodeSequence.OnBecomeRelevant`). `TdPawn.IsLeftLegForward` is the leader being past half its length.

**The stopping step.** Letting go of the stick while walking plays `walktostandpassleft` or `walktostandpassright` on `Custom_LowerBody`, blending in over 0.1 s and out over the last 0.15 s. It starts on the frame the braking starts, at any speed, so it is the input that triggers it and not the speed. Which one: `right` when the leading walk sequence is between 0.21 and 0.71 of its length, in 104 clean starts.

**Direction.** `TdAnimNodeBlendDirectional` puts the weight on the sideways child in proportion to the angle between the velocity and the facing (45 degrees is half), within the forward three or the backward three; it moves between the two threes over `ForwardInterpTime` 0.4 s and sideways over `DirInterpTime` 0.1 s.

**Landing.** Every landing goes through `TdMove_Landing.StartMove`. An ordinary one is `LandNormal(GetLandingAmount())` and straight back to walking: `Amount = clamp((fall - 300) / 230, 0.2, 1)`, 0.7 out of a coil, 0.85 for a springboard. It sets `LandNode.Landed` (the dip of the eye and `SpineX`, section 7) and activates the `LandingRun` custom blend: `FallingLandMedium` over the run at `Amount` for `0.6 x Amount` seconds, 0.15 s in and 0.4 s out.

**Close to the ground.** `TdMove_Falling.CloseToGround` (called from native code) lets the jump animation go with `StopCustomAnim(FullBody_Dir, 0.3)`. It fits falling faster than 400 with the ground less than 0.4 s away at that speed.

**The forced state.** `SetAnimationMovementState(State, Delay)` makes the state nodes read `State` instead of the pawn's movement state, at once or after `Delay`; `TdMove.StopMove` clears it, so it never outlives the move that set it.

## 6. What each move plays

`PlayMoveAnim(Slot, Name, Rate, BlendIn, BlendOut)` is `TdPawn.PlayCustomAnim` without looping. Blend times in seconds. Where a move picks between animations by what it found in the level, the director is told which by name (`PawnFrame::move_anim`) and plays it the way this table says.

| move | calls |
| --- | --- |
| Jump | `StartJump`: forward speed under 5 `JumpStill` (0.15 / 0.15); under `LongJumpNormalThreshold` 500, or with ground within 200 below the point 1.1 x speed ahead, `JumpSlow` (0.1 / 0.2); else `JumpFast`. All on `FullBody_Dir`. Stop: `StopCustomAnim(FullBody_Dir, 0.2)` unless a fall or a vault follows |
| Falling | off an edge from walking: `JumpAir` (0.3 / 0.2), `JumpStill` going backward. After a dodge jump: state 33. Stop: `FullBody` and `FullBody_Dir` out over 0.2 |
| SpeedVault / VaultOver | `StopCustomAnim(FullBody_Dir, 0.2)`; `VaultTypes[].AnimName` on `FullBody` (0.15 / 0.2): `autostepuprightleg`, `stepuprightleg88`, `VaultOnto`, `VaultOver`, `VaultOverHigh`, `VaultOntoHigh` |
| AutoStepUp / StepUp | `autostepuprightleg` at 0.8 (0.15 / 0.25); `vaultonto` (0.15 / 0.25), then `stepup{left,right}leg48`, `stepupleftleg88` at 1.2, `stepup144` (0.15 / 0.1) |
| SpringBoard | start: `LandNormal` at 0.85. At the foot plant: `SpringBoardRightLeg` with the left leg forward, else `SpringBoardLeftLeg` (0.15 / 0.25). Stop: `FullBody` out over 0.1 |
| WallRun | `wallrunrightstart` / `wallrunleftstart` at `WallrunStartUpperBodyAnimPlayRate` 0.6 (1.0 with no wall beside the head) (0.2 / 0.2); at the wall, when the camera takes the hit, `wallrunimpactright` / `left` on `Camera` (0.15 / 0.15) |
| WallrunJump | `FullBody` out over 0.2; pushing off hard (`PushSpeed > 0.6`) `WallrunJumpLeft` / `Right`, else `JumpSlow`, on `FullBody_Dir` (0.2 / 0.2); state 11. Stop as Jump |
| WallClimb | `WallRunVertical` looping at 0.2 (0.2 in); stop over 0.25. 180: `wallrunvertical180turn` (0.2 / 0.1) then `WallrunJumpLeft` (0.1 / 0.2) |
| DodgeJump | `dodgejumpleft` / `right` (0.1 / 0.2; 0.2 / 0.2 off a wall run) |
| IntoGrab | at the ledge: `hanghardstartvertical`, `HangFreeHardStart`, `HangHardStart3`, `HangHardStart2` or `HangHardStart` (0.1 to 0.2 / 0.2). Stop: `FullBody` out over 0.2 |
| Grab | `HangStrafeLeft` / `Right`, `HangFreeStrafe(Left)`, `HangTurn{Left,Right}{Start,End}`, `HangFreeTurn{Left,Right}` (0.2 / 0.2); `HangFoldedStart` (0.2 / 0); dropping `HangEnd` / `HangFreeEnd` (0.1 / 0.2) with state 2 after 0.2 |
| GrabPullUp | `HangHeaveUp`, `HangFreeHeaveUp`, `HangFoldedHeaveUp`, `...ToCrouch` (0.1 / 0.2) or `Hang(Free)HeaveOver` (0.2 / 0.2); state 3, then 0 (or 15 for the crouching ones) after 0.2 |
| GrabJump / GrabTransfer | `HangTurnJump` (0.2 / 0.2), state 3; transfer: `hang(free)transferup` (0.1 / 0.1), state 31 |
| Landing | hard: `FallingLandHard` / `FallingLandHard2` (0.05 / 0.2); on something soft `FallingLandSoftLanding` (0.1 / 0.1); backward `JumpTurnLanding` (0.1 / 0.1) |
| SkillRoll / SoftLanding | `fallinglandroll` (0.2 / 0.2), state 1 after 0.2; state 25 and `fallinglandintosoftlanding` looping (0.6 in) |
| Swing / SwingJump | `SwingHardStart(Wide)` (0.15 / 0.2), `Swing180` (0.2 / 0.3), `SwingJumpOff`, `SwingEnd` (0.2 / 0.2); state 60, cleared after 0.3, `SwingOff` |
| IntoClimb / Climb | `{Pipe,Ladder}ClimbHangStart{Left,Right,Hard}` (0.1 to 0.15 / 0.25), state 21 after 0.15; from the top `LadderEnterTop` (0.25 / 0.1); the exits (0.1 / 0.1), `PipeExitBottom` (0.1 / 0.4) |
| IntoZipLine / ZipLine | `ZiplineStart` (0.2 / 0.4), state 27 after 0.2; letting go `SwingJumpOff` (0.2 / 0.2); `ziplinehitwall` (0.1 / 0.2) |
| Slide / RumpSlide | `UpperBody` out over 0.1, state 1 then cleared after 0.4, `CrouchSlide` (0.4 / 0.4); stop over 0.2 and `CrouchSlideToCrouch` (0.1 / 0.2) into a crouch; `crouchslideintoend45` (0.15 / 0.2) |
| Crouch | start: `FullBody_Dir` and `LowerBody` out over 0.25; stop: `CrouchIntoStand` on `Camera` (0.2 / 0.2) |
| Coil | state 15, `JumpCoil` (0.15 / 0.15); stop `JumpCoilEnd` (0.25 / 0.35) |
| 180Turn | `RunTurn180` or `StandTurn180Right` (0.2 / 0.2); in the air `JumpTurnFly` (0.1 / 0.1) |
| Barge / AirBarge | `BargeInLeft` on `UpperBody` (0.2 / 0) or `MeleeKickObject` (0.1 / 0.1), `BargeOutLeft` (0 / 0.2); state 15 and `MeleeInAir` / `AirBargeIdle` (0.15 / 0.15) |
| Vertigo / LedgeWalk / Balance | `edgedetection` on `FullBody_Dir` (0.28 / 0.28), out over 0.4; `LedgeInto` (0.2 / 0.3); `walkbalancefalloff{left,right}` (0.3 / 0.3) |

## 7. The view

`TdPlayerPawn.CalcCamera` puts the camera at the `EyeJoint` bone of the first-person mesh and turns the controller's view by what the animation does to that bone. Both are checked against the camera retail recorded:

- **Position.** The eye of the port's pose, against the capsule centre retail logged, is retail's camera with one constant: the mesh's origin is 94 below the capsule centre (4 below the feet; 65 when crouched, with the shorter capsule). Standing, the difference is that constant with no spread at all; through the moves the median stays within 3 uu of it. Standing, the camera is 64 to 65 above the capsule centre. It is **not** `BaseEyeHeight` 76 above it, which is where the port's world camera still is (see section 9).
- **Rotation.** The eye bone looks along its +Z, with -Y up and +X to the left. Its turn from the reference pose, as pitch, yaw and roll against the pawn, is what `GetCameraAnimation` hands back (`CalcCamera` adds it to the view angle by angle). Retail's camera minus the controller's view is that turn to within a degree at the median, in every move recorded.
- **The landing dip.** `LandNode` (`TdAnimNodeLandOffset`) is an `AnimNodeAimOffset` whose one pose, `jumplandpose`, turns `SpineX` and the `EyeJoint` and moves the eye 8.25 in the mesh's space; its strength goes to `Landed` over `LandInto` 0.1 s and back over `LandOut` 0.4 s.
- **The swan neck.** Looking down past `SwanNeckEnableAtPitch` (15 degrees) `SwanNeck1p.GetSwanNeckPos` cranes the camera forward and down off the eye, so that she sees her feet and not her chest. Measured standing still at five pitches: with `a = 90 degrees x (pitch - 15) / 75`, forward `20.1 sin a + 4.1 (1 - cos a)` and down `18.6 (1 - cos a)`: 24.2 and 18.6 looking straight down (`TdMove`'s defaults are `SwanNeckForward` 35 and `SwanNeckDown` 30; how the node gets from those to what it does is not established).
- **Field of view.** `Mesh1p.FOV` is 90, horizontal, whatever the world's is.

With those, the body is where retail has it. Below is retail looking straight down, standing; the marks are where the port's pose puts the `LeftToeBase`, `LeftFoot`, `RightToeBase` and `RightFoot` bones through that camera (`build/re/handmark.py`). The wrists come out just under the bottom edge, which is where retail's are: only the fingers show.

![Retail looking straight down with the port's foot bones marked](../screenshots/fp/retail_look_down_bones.png)

Nothing in the tree turns the unarmed arms with the view. Standing, they hang at her sides and are out of sight until she looks nearly straight down; running, the wrists reach about 42 degrees under the horizon at the top of the swing, so with a 90 degree view at 16:9 (29 degrees from the centre to the bottom edge) the hands show when she looks a little down and not when she looks at the horizon. A retail frame of a run at 446 uu/s looking 35 degrees down has no arm in it, and neither does the port's pose for that frame.

## 8. The port

| file | what |
| --- | --- |
| `src/anim/fp_anim.*` | `fp::AnimTree`: `AT_C1P` loaded from the package and ticked: node weights and blend times, the state nodes, the slots (`play_custom_anim` / `stop_custom_anim`), the synch group, the directional blend, the custom blend, the landing offset |
| `src/anim/fp_director.*` | `fp::Director`: the walking state, each move's calls from section 6, the forced animation state and its timer, the landing, the stopping step |
| `src/anim/fp_pose.*` | `fp::PoseEvaluator`: the bones the tree's weights make (list and slot blends, per-bone masks from `Child2PerBoneWeight`, aim offsets), and `view()`: the camera of section 7 |
| `src/anim/anim_system.cpp` | `evaluate_faith_1p` draws the tree's pose from that camera when she is unarmed and in play. The hand-placed offsets are gone from that path; the whole upper and lower body is drawn, as retail draws it |
| `src/tools/anim_main.cpp` | `me_anim`: the tree over a recorded retail run, headless |
| `tools/retail/anim_check.py` | lays what `me_anim` plays next to retail's `anim1p` records and scores it |

`python -m tools.retail.anim_check` over the nine recordings:

| | frames | same lead sequence | weight in common |
| --- | --- | --- | --- |
| all | 80,238 | 90.1% | 90.7% |
| Walking | 63,877 | 93.6% | 93.9% |
| Jump | 1,583 | 94.0% | 96.2% |
| VaultOver | 1,891 | 96.2% | 95.7% |
| GrabPullUp | 471 | 96.6% | 96.7% |
| SpringBoarding | 416 | 100% | 99.9% |
| WallRunning left / right | 810 | 95.4% / 97.4% | 95.2% / 95.1% |
| WallRunJump | 175 | 100% | 99.7% |
| ZipLine, Landing | 502 | 100% | 99.7% / 99.9% |
| Slide | 195 | 96.4% | 98.0% |
| Falling | 2,104 | 76.6% | 82.8% |
| Grabbing | 2,037 | 78.4% | 76.3% |
| Swing | 2,226 | 36.5% | 46.0% |
| Climb | 1,333 | 22.0% | 21.8% |
| IntoGrab | 113 | 22.1% | 32.9% |

A recording has the pawn but not the level, so the check gives the director what the controller gives it in the game: the height above the ground (from where the fall ends), whether the stick is pushed (from the braking) and, for the moves of section 6 that pick an animation by the level, which one retail picked. The hints never say when or how an animation plays. `--no-hints` runs without them.

## 9. Not done, and not established

- **The world camera.** The port's renderers still put the world camera `eye_height` 166 above the feet. Retail's is at the `EyeJoint`: about 155 above the feet standing, bobbing with the run (5 up and down, 5 side to side at a sprint), dipping on a landing, craning forward looking down. Only the first-person body is drawn from the right place so far; moving the world camera there touches the cutscene hand-over and every per-move camera rule in the controller, and was left alone.
- **State nodes that stay on their first child:** `TdAnimNodeSwing` (which pose of the swing), `TdAnimNodeClimb` (ladder or pipe, which hand, the climbing sequences), `TdAnimNodeGrabbing` (the looking-over-the-shoulder idles, the shimmy), `TdAnimNodeTurn` (the standing turn steps), `TdAnimNodeBalanceWalk`, `TdAnimNodeLedgeWalk`, `TdAnimNodeDirSwitch`, `TdAnimNodeAgainstWallState`. They are native and each needs its rule measured; these are the low rows of the table.
- **`1pAim`** (the hips turning with the direction of movement) and `TdAnimNodeAimOffset` on the standing legs are loaded but left at their centre. The skeletal controls (`TdSkelControlAim1p`, the lazy springs, limb IK, foot placement) are not run. `TdSkelControlAim1p` is what turns the arms with the view with a weapon in hand; armed and melee poses are still picked by the older code in `evaluate_faith_1p`.
- **In the game** the controller does not yet say which vault, ledge catch or heave it chose, how far the ground is, or which side a dodge goes, so the director plays each move's plain animation (`VaultOver`, `HangHardStart`, `HangHeaveUp`, `FallingLandHard`), and the jump animation holds until the landing.
- **The walk cycle's phase** drifts from retail's by up to a sixth of a cycle after some stops, which sometimes picks the other leg for the stopping step. When the walking state changes, retail's weights between the old and new children move a little differently from the port's.
- The tracked `screenshots/oracle_*.png` are Metal renders from before this change and still show the old arms.
