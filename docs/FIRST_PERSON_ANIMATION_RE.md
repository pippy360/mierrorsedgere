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

**The stopping step.** Letting go of the stick while walking plays `walktostandpassleft` or `walktostandpassright` on `Custom_LowerBody`, blending in over 0.1 s and out over the last 0.15 s. It starts on the frame the braking starts, at any speed, so it is the input that triggers it and not the speed. Which one: `right` when the leading walk sequence is between 0.21 and 0.71 of its length: 95 of 101 starts on retail's own cycle.

**Direction.** `TdAnimNodeBlendDirectional` puts the weight on the sideways child in proportion to the angle between the velocity and the facing (45 degrees is half), within the forward three or the backward three; it moves between the two threes over `ForwardInterpTime` 0.4 s and sideways over `DirInterpTime` 0.1 s. The sideways children are the "stiff" runs: `1pAim` (a `TdAnimNodeDirBone`, an aim offset on `Hips` and `Spine`) turns the hips the way she is going, up to the quarter turn its left and right poses hold, and the spine back against them.

**Landing.** Every landing goes through `TdMove_Landing.StartMove`. An ordinary one is `LandNormal(GetLandingAmount())` and straight back to walking: `Amount = clamp((fall - 300) / 230, 0.2, 1)`, 0.7 out of a coil, 0.85 for a springboard. It sets `LandNode.Landed` (the dip of the eye and `SpineX`, section 7) and activates the `LandingRun` custom blend: `FallingLandMedium` over the run at `Amount` for `0.6 x Amount` seconds, 0.15 s in and 0.4 s out.

**Close to the ground.** `TdMove_Falling.CloseToGround` (called from native code) lets the jump animation go with `StopCustomAnim(FullBody_Dir, 0.3)`. It fits falling faster than 400 with the ground less than 0.4 s away at that speed.

**The forced state.** `SetAnimationMovementState(State, Delay)` makes the state nodes read `State` instead of the pawn's movement state, at once or after `Delay`; `TdMove.StopMove` clears it, so it never outlives the move that set it.

**Hanging (`TdAnimNodeGrabbing`).** With `d` the view's yaw off the body's and `T` the move's `CurrentGrabTurnType` (0 none, 1 the turn's start, 3 turned, 2 the turn's end): turned past a quarter turn with `T` 2 or 3, the idle of that side shows at once, shared with its "extreme" by how far past, `(|d| - 90) / 90`; with `T` 0 or 1 the node goes back to `Hang` (`HangFree` hanging free: `SetActiveMove` from the script) over 0.2 s; in between it stays as it is. It starts on the hang whenever it regains weight. All 777 frames in which the node has weight fit to 0.005.

**Climbing (`TdAnimNodeClimb`).** The child is the slide while she slides (`bClimbDownFast`), else the still pose of the hand `bClimbLeftHand` says, of the ladder's or the pipe's set; a change blends over 0.2 s. All 580 frames fit to 0.009. The look-back idles never show in the recordings (the furthest she looks round on a ladder is 77 degrees).

**The swing (`TdAnimNodeSwing`).** By the move's `SwingAngle` alone, with nothing smoothing it: with `a = clamp(angle / 90 degrees, -1, 1)`, Front `max(a, 0)`, back `max(-a, 0)`, Middle `1 - |a|`. The crossfader under Middle stays at its saved 0.2556 and the camera-bone mask at 0.8013. Three swings, 2,143 frames, every leaf to 0.005. The angle is positive ahead of the bar; the pawn's centre is on a circle of radius 120 (`SwingPendulumLength`) about it, to 0.001.

**Standing turns (`TdAnimNodeTurn`).** The legs keep their own yaw. Moving, it comes round to the way she is going (the other way going backward). Standing, it stays while the view turns, never more than 90 degrees behind it; once the view is more than 65 degrees (`ExtendedRegionLimit`) off the legs the quarter-turn step of that side plays, bringing the legs round 90 degrees over its 0.8 s whatever the view does meanwhile, blending in and out over 0.2 s. The 45 degree steps are never chosen in 76 turns. Taking the first turn of a standing stretch as given, the rule makes 27 of the 30 that follow, on the right side and within three frames. How fast the legs come round while she moves is a rough fit (2.4 degrees a second for each uu/s). The standing legs' aim offset (`bAimSourceIsLegRotation`) twists the hips by how far the legs are off the body, whole at 18 degrees (its `HorizontalRange` 0.2 of a quarter turn).

**Not listed by the recorder:** what plays on the `Custom_Camera` and `Custom_Canned` slots (`gethitfront`, the wall run impacts, `CrouchIntoStand`, the level intros). Their names appear in no recording; the port plays them and leaves them out of what it compares.

## 6. What each move plays

`PlayMoveAnim(Slot, Name, Rate, BlendIn, BlendOut)` is `TdPawn.PlayCustomAnim` without looping. Blend times in seconds; a blend-out of 0 or less holds the last frame until something else takes the slot. `TdPawn.OnAnimEnd` calls the current move's `OnCustomAnimEnd` only for an animation that move started.

Where a move picks between animations by what it found in the level, the controller says which (`PlayerTelemetry::move_anim`, section 8) and the director plays it the way this table says.

| move | calls |
| --- | --- |
| Jump | `StartJump`: forward speed under 5 `JumpStill` (0.15 / 0.15); under `LongJumpNormalThreshold` 500, or with ground within 200 below the point 1.1 x speed ahead, `JumpSlow` (0.1 / 0.2); else `JumpFast`. All on `FullBody_Dir`. Stop: `StopCustomAnim(FullBody_Dir, 0.2)` unless a fall or a vault follows |
| Falling | off an edge from walking: `JumpAir` (0.3 / 0.2), `JumpStill` going backward. After a dodge jump: state 33. Stop: `FullBody` and `FullBody_Dir` out over 0.2 |
| SpeedVault / VaultOver | `StopCustomAnim(FullBody_Dir, 0.2)`; `VaultTypes[].AnimName` on `FullBody` (0.15 / 0.2): `autostepuprightleg` (a ledge up to 48), `stepuprightleg88` (48 to 148 with momentum under 200), `VaultOnto`, `VaultOver` (64 to 148), `VaultOverHigh`, `VaultOntoHigh` (145 to 192) |
| AutoStepUp / StepUp | `autostepuprightleg` at 0.8 (0.15 / 0.25); `vaultonto` (0.15 / 0.25), then `stepup{left,right}leg48`, `stepupleftleg88` at 1.2, `stepup144` (0.15 / 0.1) |
| SpringBoard | start: `LandNormal` at 0.85. At the foot plant (`ReachedPreciseLocation`): `SpringBoardRightLeg` with the left leg forward, else `SpringBoardLeftLeg` (0.15 / 0.25). Stop: `FullBody` out over 0.1 |
| WallRun | `wallrunrightstart` / `wallrunleftstart` at `WallrunStartUpperBodyAnimPlayRate` 0.6 (1.0 with no wall beside the head) (0.2 / 0.2); at the wall, when the camera takes the hit, `wallrunimpactright` / `left` on `Camera` (0.15 / 0.15) |
| WallrunJump | `FullBody` out over 0.2; pushing off hard (`PushSpeed > 0.6`) `WallrunJumpLeft` / `Right`, else `JumpSlow`, on `FullBody_Dir` (0.2 / 0.2); state 11. Stop as Jump |
| WallClimb | `WallRunVertical` looping at 0.2 (0.2 in); stop over 0.25. 180: `wallrunvertical180turn` (0.2 / 0.1) then `WallrunJumpLeft` (0.1 / 0.2) |
| DodgeJump | `dodgejumpleft` / `right` (0.1 / 0.2; 0.2 / 0.2 off a wall run) |
| IntoGrab | plays nothing until the hands are on the ledge (`ReachedPreciseLocation`, just before `SetMove(Grabbing)`), then by where she came from and `IntoGrabSpeed`, her vertical speed when the move started: out of a wall climb's double jump `hanghardstartvertical` (0.2 / 0.2); hanging free `HangFreeHardStart` (0.1 / 0.2); under -1000 `HangHardStart3` with `gethitfront` on `Camera` (0.05 / 0.2); under -600 `HangHardStart2`; else `HangHardStart` (0.1 / 0.2). A folded hang (`CheckForFoldedHang`) plays `HangFoldedStart` (0.2 / 0) with state 3 instead |
| Grab | shimmy (`StartShimmy`): one `HangStrafeLeft` / `Right` (`HangFreeStrafeLeft` / `HangFreeStrafe`) a step (0.2 / 0), started again from its first frame while the stick is held; let go, it stops over 0.4 only between 0.1 and 0.25 of the step or past 0.8. Looking back (`UpdateViewRotation`): past a quarter turn `HangTurnRightStart` / `LeftStart` (0.2 / 0.2), the turn type 1 and after 0.2 s 3; back inside it `HangTurnRightEnd` / `LeftEnd` (0.2 / 0.2), type 2 until it ends. Hanging free: `HangFreeTurnRight` / `Left` once past 85.9 degrees. Dropping: `HangEnd` / `HangFreeEnd` (0.1 / 0.2) with state 2 after 0.2 |
| GrabPullUp | `HangHeaveUp`, `HangFreeHeaveUp`, `HangFoldedHeaveUp`, `...ToCrouch` (0.1 / 0.2) or `Hang(Free)HeaveOver` (0.2 / 0.2); state 3, then 0 (or 15 for the crouching ones) after 0.2 |
| GrabJump / GrabTransfer | `HangTurnJump` (0.2 / 0.2), state 3; transfer: `HangTurn{Right,Left}Start` (0.2 / 0.1), `hang(free)transferup` (0.1 / 0.1), state 31 |
| Landing | hard: `FallingLandHard` / `FallingLandHard2` (0.05 / 0.2); on something soft `FallingLandSoftLanding` (0.1 / 0.1); backward `JumpTurnLanding` (0.1 / 0.1) |
| SkillRoll / SoftLanding | `fallinglandroll` with root motion (0.2 / 0.2), state 1 after 0.2; state 25 and `fallinglandintosoftlanding` looping (0.6 in) |
| Swing / SwingJump | `SwingHardStart(Wide)` (0.15 / 0.2), `Swing180` (0.2 / 0.3), `SwingJumpOff`, `SwingEnd` (0.2 / 0.2); state 60, cleared after 0.3, `SwingOff` |
| IntoClimb | `PlayStartAnimation`: from a wall run or a transfer `{Pipe,Ladder}ClimbHangStart{Left,Right}` (0.15 / 0.25); falling faster than 800 `...HangStartHard`, than 200 `...HangStart` (0.1 / 0.25); else, on a pipe, `PipeClimbStart` (0.15 / 0.25); always state 21 after 0.15. From the top: `LadderEnterTop` (0.25 / 0.1) |
| Climb | a step at a time (`HandleClimbAction`, `Climb`): `{Pipe,Ladder}ClimbUp{Left,Right}Hand` on `FullBody` (0.1 / 0.075), the left hand's first, `bClimbLeftHand` turning over 0.1 s into each step; a pipe with more than one rung left uses `PipeClimbUpFast...`, two rungs to the animation. The move flies each 32 uu rung at 96 uu/s (ladder) or 64 (pipe), so a step lasts as long as its animation and the next one starts when it arrives. Down is the slide (the tree's node); stepping down, the same animations at -1, is in the script and in no recording. Over the top: `...ExitTop{Right,Left}Hand` with root motion (0.1 / 0.1), state 1 after 0.5. Letting go: `PipeExitBottom` (0.1 / 0.4) |
| IntoZipLine / ZipLine | `ZiplineStart` (0.2 / 0.4), state 27 after 0.2; letting go `SwingJumpOff` (0.2 / 0.2); `ziplinehitwall` (0.1 / 0.2) |
| Slide / RumpSlide | `UpperBody` out over 0.1, state 1 then cleared after 0.4, `CrouchSlide` (0.4 / 0.4); stop over 0.2 and `CrouchSlideToCrouch` (0.1 / 0.2) into a crouch; `crouchslideintoend45` (0.15 / 0.2) |
| Crouch | start: `FullBody_Dir` and `LowerBody` out over 0.25; stop: `CrouchIntoStand` on `Camera` (0.2 / 0.2) |
| Coil | state 15, `JumpCoil` (0.15 / 0.15); stop `JumpCoilEnd` (0.25 / 0.35) |
| 180Turn | `RunTurn180` or `StandTurn180Right` (0.2 / 0.2); in the air `JumpTurnFly` (0.1 / 0.1) |
| Barge / AirBarge | `BargeInLeft` on `UpperBody` (0.2 / 0) or `MeleeKickObject` (0.1 / 0.1), `BargeOutLeft` (0 / 0.2); state 15 and `MeleeInAir` / `AirBargeIdle` (0.15 / 0.15) |
| Melee | `TriggerMove`: `Camera` out over 0.05, `Weapon` over 0.1, state 1, then on `UpperBody` `MeleeStartLeft` / `Right` at 1.5 (0.1, held); hands alternate, and the third blow of a combo is `MeleeStartShove`. When the wind-up ends (0.11 s) the blow lands or misses (`TestHit`: a target within 170 and 37 degrees): `MeleeHitLeft` / `Right` at 1.5 (0.2 / 0.1) or `MeleeMissedLeft` / `Right` at 1.5 (0.08 / 0.1); the shove is `MeleeHitShove` (0 / 0.1) either way. Stop: `UpperBody` and `FullBody` out over 0.3 |
| MeleeCrouch | state 15; `MeleeCrouchStart` (0.1, held), then `MeleeCrouchHit` (0.1 / 0.2), hit or miss |
| MeleeAir | faster than 200: `MeleeInAir` (0.1 / 0.2), on a hit `MeleeInAirHit` (0.1 / 0.2); else `MeleeInAirStill`; on a target below, `MeleeFromAbove` (0.1 / 0.1) |
| MeleeSlide / MeleeWallrun | `MeleeSlide` (0.1 / 0.1) with state 16; `MeleeWallRunLeft` / `Right` by the wall she was on (0.1 / 0.2) |
| Vertigo / LedgeWalk / Balance | `edgedetection` on `FullBody_Dir` (0.28 / 0.28), out over 0.4; `LedgeInto` (0.2 / 0.3); `walkbalancefalloff{left,right}` (0.3 / 0.3) |

## 7. The view

`TdPlayerPawn.CalcCamera` puts the camera at the `EyeJoint` bone of the first-person mesh and turns the controller's view by what the animation does to the camera bone. Both are checked against the camera retail recorded:

- **Position.** The eye of the port's pose, against the capsule centre retail logged, is retail's camera with one constant: the mesh's origin is 94 below the capsule centre (4 below the feet; 65 when crouched, with the shorter capsule). Standing, the difference is that constant with no spread at all; through the moves the median stays within 3 uu of it. Standing, the camera is 64 to 65 above the capsule centre, about 155 above the feet. It is **not** `BaseEyeHeight` 76 above the centre.
- **Rotation.** `GetCameraAnimation` (native) is `FMatrix::Rotator` of the `CameraJoint` bone, the eye's child, in the mesh's space. The mesh's axes are +X left, -Y up, +Z forward, so the rotator's roll is the view's pitch, its pitch the view's yaw and its yaw the view's roll, which is the swizzle `CalcCamera` does. Read this way the pitch runs on past straight down, which is how the skill roll turns the view right over. Retail's camera minus the controller's view is that turn to within a degree at the median, in every move recorded. The wall run's tilt (17.8 degrees) is all animation: the controller's view has no roll.
- **The landing dip.** `LandNode` (`TdAnimNodeLandOffset`) is an `AnimNodeAimOffset` whose one pose, `jumplandpose`, turns `SpineX` and the `EyeJoint` and moves the eye 8.25 in the mesh's space. Against retail's camera through four landings its strength goes to `Landed` in 0.1 s and back in 0.3 (the node's `LandOut` says 0.4): the view dips 3 degrees and the port's follows it to a tenth of a degree.
- **The swan neck.** Looking down past `SwanNeckEnableAtPitch` (15 degrees) `SwanNeck1p.GetSwanNeckPos` cranes the camera forward and down off the eye, so that she sees her feet and not her chest. Measured standing still at five pitches: with `a = 90 degrees x (pitch - 15) / 75`, forward `20.1 sin a + 4.1 (1 - cos a)` and down `18.6 (1 - cos a)`: 24.2 and 18.6 looking straight down (`TdMove`'s defaults are `SwanNeckForward` 35 and `SwanNeckDown` 30; how the node gets from those to what it does is not established).
- **Field of view.** `Mesh1p.FOV` is 90, horizontal, whatever the world's is.
- **The root.** `IgnoreRootTransformation` takes the root bone from the notify carriers, not from what is playing, so no animation moves the mesh off the pawn; root motion (`UseRootMotion`) moves the pawn instead.

With those, the body is where retail has it. Below is retail looking straight down, standing; the marks are where the port's pose puts the `LeftToeBase`, `LeftFoot`, `RightToeBase` and `RightFoot` bones through that camera (`build/re/fpanim/handmark.py`). The wrists come out just under the bottom edge, which is where retail's are: only the fingers show.

![Retail looking straight down with the port's foot bones marked](../screenshots/fp/retail_look_down_bones.png)

Nothing in the tree turns the unarmed arms with the view. Standing, they hang at her sides and are out of sight until she looks nearly straight down; running, the wrists reach about 42 degrees under the horizon at the top of the swing, so with a 90 degree view at 16:9 (29 degrees from the centre to the bottom edge) the hands show when she looks a little down and not when she looks at the horizon. A retail frame of a run at 446 uu/s looking 35 degrees down has no arm in it, and neither does the port's pose for that frame.

Retail's camera sits further back than the eye in a few moves, by a steady amount the tree does not account for: 10 uu in a wall run, 9 on a springboard, 4 to 5 crouched or sliding. Where that comes from is not established.

**Where the moves put the mesh.** Two things move the whole mesh against the pawn without being in the pose, so they move the camera and leave the body's place in the view alone:

- `TdPawn.SetRootOffset`: the swing lifts the mesh 50 and sets it back 32 (`vect(0, -50, -32)` in the root bone's space, over 0.15 s, off over 0.1), the rump slide lifts it 20, a crouch out of a walk lifts it 15 for 0.15 s while the capsule drops; `TdMove.StopMove` takes any of them off over 0.3 s.
- `TdMove_Swing.SetPawnRotation`: the swing control turns the body by the swing's angle about a point 94 above the mesh's origin, which is the capsule's centre.

With both, the port's eye through 2,118 frames of swinging is retail's camera to 3 uu and 3 degrees (10th to 90th percentile), where without them it was 131 uu and 69 degrees out.

## 8. The port

| file | what |
| --- | --- |
| `src/anim/fp_anim.*` | `fp::AnimTree`: `AT_C1P` loaded from the package and ticked: node weights and blend times, the state nodes (movement, walking, hanging, climbing, swing, standing turn, balance, direction), the slots (`play_custom_anim` / `stop_custom_anim`), the synch group, the custom blend, the aim offsets' strengths |
| `src/anim/fp_director.*` | `fp::Director`: the walking state, each move's calls from section 6, the forced animation state and its timer, the landing, the stopping step, the climb's steps, the shimmy and the look back while hanging, the blows, the root offset and the swing's turn |
| `src/anim/fp_pose.*` | `fp::PoseEvaluator`: the bones the tree's weights make (list and slot blends, per-bone masks from `Child2PerBoneWeight`, aim offsets over their range), and `view()`: the camera of section 7 |
| `src/anim/anim_system.cpp` | `tick_first_person` runs the tree once per simulated time from the controller's telemetry. `player_camera` puts the world camera on the tree's eye in play. `evaluate_faith_1p` draws the tree's pose from that eye: the whole body unarmed, the legs under the hand-posed arms with a weapon |
| `src/physics/parkour_controller.cpp` | says what its moves chose (below) |
| `src/tools/anim_main.cpp` | `me_anim`: the tree over a recorded retail run, headless; `--eye` writes the camera, `--bones` where bones are in it |
| `tools/retail/anim_check.py` | lays what `me_anim` plays next to retail's `anim1p` records and scores it |

What the controller tells the animation, in `PlayerTelemetry`:

| field | from |
| --- | --- |
| `move_anim`, `move_anim_serial` | the vault type by the ledge's height, landing on top or beyond, and pace; the ledge catch by where she came from and her fall speed; the heave that ends standing or crouched; the wall run jump that pushes off hard; `@reached` when the springboard's foot is on the step |
| `ground_distance` | a trace down while falling faster than 400 |
| `jump_over_gap` | `StartJump`'s trace 1.1 x the speed ahead and 200 down |
| `move_left`, `hanging_free`, `climbing_pipe` | the dodge's side; no wall for the legs under a ledge; `TdLadderVolume.LadderType` |
| `swing_angle`, `balance_lean`, `body_yaw_deg`, `move_input` | the swing, the beam, `TdPawn.Rotation.Yaw`, the stick |

`python -m tools.retail.anim_check` over the nine recordings:

| | frames | same lead sequence | weight in common |
| --- | --- | --- | --- |
| all | 80,238 | 95.8% | 95.4% |
| Walking | 63,877 | 96.1% | 95.8% |
| Swing | 2,226 | 99.9% | 99.9% |
| Falling | 2,104 | 82.6% | 84.2% |
| Grabbing | 2,037 | 90.5% | 89.1% |
| VaultOver | 1,891 | 96.2% | 95.7% |
| Jump | 1,583 | 96.3% | 96.4% |
| Climb | 1,333 | 97.6% | 97.2% |
| WallRunning left / right | 810 | 95.6% / 98.0% | 95.3% / 95.5% |
| GrabPullUp | 471 | 96.6% | 96.7% |
| Balance | 427 | 97.2% | 67.5% |
| SpringBoarding | 416 | 100% | 99.9% |
| ZipLine, Landing, Slide, WallRunJump, SwingJump | 1,010 | 100% | 99.4% to 99.9% |
| IntoGrab | 113 | 68.1% | 76.2% |

A recording has the pawn but not the level or the stick, so the check gives the director what the controller gives it in the game, taken from the recording itself: the height above the ground from where the fall ends; the swing's angle from where she is on the arc; pipe or ladder, the vault, catch and heave variants and the long jump's gap from the names of what retail went on to play; the stick's release from the stopping step it plays. The hints never say when or how an animation plays, except the release. `--no-hints` runs without them. The climb's steps, the shimmy, the turns and the hang's look back are not hinted: they come out of the pawn's movement and view by the rules above.

The camera, by `build/re/fpanim/eyecheck.py` on a recording (`20260925_160622`), retail minus port, median and (10th to 90th percentile spread); 94 is the mesh's origin under the capsule's centre:

| move | frames | forward | up | pitch | roll |
| --- | --- | --- | --- | --- | --- |
| Walking | 2,138 | -1.8 (15.3) | -94.0 (7.5) | 0.0 (0.5) | 0.0 (1.5) |
| Climb | 243 | 0.0 (1.6) | -94.2 (2.2) | 0.0 (0.6) | 0.0 (0.3) |
| Grabbing | 146 | 1.0 (0.8) | -94.0 (0.8) | 0.1 (7.2) | 0.0 (1.4) |
| VaultOver | 108 | -2.6 (4.9) | -94.3 (11.6) | -0.3 (1.9) | 0.0 (0.8) |
| GrabPullUp | 94 | 0.0 (1.6) | -94.5 (3.6) | 0.1 (3.2) | 0.0 (1.1) |
| WallRunning left | 87 | -10.1 (10.7) | -94.5 (8.4) | 0.0 (2.3) | 0.0 (1.5) |
| Swing (`20260926_164012`) | 2,118 | 0.0 (2.9) | -94.0 (2.6) | 0.0 (3.1) | 0.0 (0.0) |

## 9. Not done, and not established

- **Armed arms.** With a weapon the legs, the body's place and the camera are the tree's, but the arms and the gun are still the hand-picked weapon poses held to the view (`evaluate_faith_1p`). Retail lays the weapon branch over the arm bones (`ArmedRight`, `ArmedLeft`) and turns them with the view by `TdSkelControlAim1p`, which is native; there is no retail recording with a weapon in hand to measure it against.
- **The skeletal controls** other than the root offset and the swing control are not run: the lazy springs, limb IK (the hands on a sloped ledge), foot placement.
- **Nodes without a recording:** the ladder's look-back idles, the ledge slope children (`hang45...`), hanging free beyond its start, the crouched turn, `AgainstWallState`, the balance's lose-balance poses. The balance's lean is driven by the controller's own lean, which no recording checks (the low "weight in common" for Balance above).
- **The camera's place in a few moves.** Retail's sits further back than the eye by a steady amount the tree does not account for: 10 uu in a wall run, 9 on a springboard, 4 to 5 crouched or sliding.
- **Falls that end on a ledge.** `CloseToGround` lets the jump animation go before a ledge catch too; a recording does not say how far the ledge was, so the check holds the jump animation too long there (the IntoGrab and Falling rows). In the game the controller's trace decides it.
- **The walk cycle's phase** is ahead of retail's by more than a twentieth of a cycle at a third of the stops, which picks the other leg for the stopping step in 23 of 101 (the rule itself misses 6 on retail's own cycle).
- **A pipe's last rung** is climbed with the fast animation in the game; retail uses the plain one when a single rung is left.
- **In the game, not checked on screen:** the mouse cannot be driven in the windowed tests, so everything that depends on the view's pitch or on looking round (the body looking down, the hang's look back, the standing turn) is checked through `me_anim` against the recordings and against retail's frames, not by eye in the running port.
- The tracked `screenshots/oracle_*.png` are Metal renders from before this work and still show the old arms and camera.
