# Mirror's Edge: Configuration, Pawn Movement & Game Mechanics Specification

This document provides the exhaustive, reverse-engineered technical and numerical specification of all configuration files, parkour movement states, combat mechanics, weapons, input bindings, campaign chapter flows, and time trial stretches from `/Users/tomnom/mirrorsedge/TdGame/Config/`.

## Section 1: Overview of Configuration & Localization Architecture
The Mirror's Edge runtime configuration is governed by **23 primary configuration files** in `TdGame/Config/` and **46 localization files** per language in `TdGame/Localization/INT/`.

| Configuration File | Purpose & Primary Subsystems |
| :--- | :--- |
| `DefaultPawnMovement.ini` | Core parkour kinematics, 55 movement state parameter blocks, collision checks, speeds |
| `DefaultGame.ini` | Campaign map progression (Prologue - Ch 9), checkpoints, 23 time trial stretch definitions |
| `DefaultInput.ini` | Input bindings for keyboard/mouse and gamepad, axis sensitivities, parkour action buttons |
| `DefaultWeapons.ini` | Ballistics, clip sizes, firing rates, recoil, damage, spread, and mobility penalties |
| `DefaultAI.ini` | Enemy sight ranges, reaction delays, combat distances, and evasion behaviors |
| `DefaultAIMeleeAttacks.ini` | AI melee attack moves, block states, damage windows, and pursuit finishing attacks |
| `DefaultAITemplates.ini` | Archetype definitions (PatrolCop, SWAT, Support, Gunner, Riot, Pursuit, Celeste) |
| `DefaultEngine.ini` | Graphics settings, render targets, texture streaming, lighting cache, font definitions |
| `DefaultHudEffects.ini` | Damage vignettes, bullet whiz-by, reaction time post-processing effects |
| `DefaultCamera.ini` | First-person camera offsets, FOV settings, head bobbing, tilt modifiers |
| `DefaultPathfindingCosts.ini` | AI parkour navigation costs for vault, slide, wallrun, and jump-into-grab nodes |
| `DefaultScoring.ini` | Time trial scoring multipliers, clean run bonuses, speed rating thresholds |
| `DefaultLOI.ini` | Runner Vision (Line of Intuition) fade-in and fade-out speeds |
| `DefaultAchievements.ini` | 44 achievement definitions, triggers, and criteria |
| `DefaultAnimation.ini` | Animation set bindings and blend spaces |
| `DefaultCompat.ini` | Hardware compatibility profiles and fallback render paths |
| `DefaultUI.ini` / `DefaultEditor*.ini` | UI viewport layouts and editor tooling parameters |

---

## Section 2: Comprehensive Pawn Movement Specification (`DefaultPawnMovement.ini`)

Mirror's Edge replaces traditional UE3 character movement with a discrete state machine consisting of 55 specialized movement classes derived from `UTdMove`. Each state governs distinct physics overrides, camera animations, and transition rules.

### 2.1 Fundamental Locomotion

#### `[TdGame.TdMove]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `AiAimOneShotPenalties` | `{'Easy': 0.0, 'Medium': 0.0, 'Hard': 0.0}` | Enemy weapon dispersion modifier during move |
| `AiAimPenalties` | `{'Easy': 1.0, 'Medium': 1.0, 'Hard': 1.0}` | Enemy weapon dispersion modifier during move |
| `FireAnimSeqName` | `standfire` | Configuration parameter |
| `FrictionModifier` | `1.0` | Kinematic drag / slowdown factor |
| `RedoMoveTime` | `0.0` | Time in seconds |
| `ReloadAnimSeqName` | `reload` | Configuration parameter |
| `SpeedModifier` | `1.0` | Speed threshold in Unreal Units/sec |
| `StickyAimedModifier` | `0.0` | Configuration parameter |
| `StickyAngle` | `800` | Angle in degrees / Unreal rotation units |
| `bStickyAim` | `True` | Configuration parameter |
| `bTriggersCompliment` | `False` | Configuration parameter |

#### `[TdGame.TdMove_Walking]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `FrictionModifier` | `1.0` | Kinematic drag / slowdown factor |
| `TriggerIdleAnimMaxTime` | `40.0` | Time in seconds |
| `TriggerIdleAnimMinTime` | `30.0` | Time in seconds |
| `UnarmedIdleAnims` | `[{'AnimName': 'standidle1', 'NodeType': 'CNT_Canned', 'bResetCameraLook': True}, {'AnimName': 'standidle2', 'NodeType': 'CNT_Canned', 'bResetCameraLook': True}, {'AnimName': 'standidle3', 'NodeType': 'CNT_Canned', 'bResetCameraLook': True}]` | Configuration parameter |

#### `[TdGame.TdMove_Crouch]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `IntoCrouchSitTime` | `1.0` | Time in seconds |
| `SpeedModifier` | `0.2` | Speed threshold in Unreal Units/sec |

#### `[TdGame.TdMove_180Turn]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `FrictionModifier` | `0.3` | Kinematic drag / slowdown factor |
| `RedoMoveTime` | `0.5` | Time in seconds |
| `TurnAnimBlendInTime` | `0.2` | Time in seconds |
| `TurnAnimBlendOutTime` | `0.2` | Time in seconds |
| `TurnTime` | `0.25` | Time in seconds |

#### `[TdGame.TdMove_Falling]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `EnterToFallingZSpeed` | `- 200` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `MaximumSpeedForRollLanding` | `-5000` | Speed threshold in Unreal Units/sec |
| `RollLandingTimeThreshold` | `2.0` | Time in seconds |
| `bCheckForGrab` | `True` | Configuration parameter |
| `bCheckForVaultOver` | `True` | Configuration parameter |

#### `[TdGame.TdPhysicsMove]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `ContextMoveDistanceMultiplier` | `1.8` | Configuration parameter |
| `HandPlantCheckDistance` | `200` | Configuration parameter |
| `HandPlantCheckHeight` | `112` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `HandPlantExtentCheckHeight` | `80` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `HandPlantExtentCheckWidth` | `10` | Configuration parameter |
| `bCheckForEdgeInVelDir` | `False` | Configuration parameter |
| `bCheckForGrab` | `False` | Configuration parameter |
| `bCheckForWallClimb` | `False` | Configuration parameter |

### 2.2 Jumping, Vaulting & Springboard

#### `[TdGame.TdMove_Jump]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `AiAimPenalties` | `{'Easy': 0.7, 'Medium': 0.8, 'Hard': 1.0}` | Enemy weapon dispersion modifier during move |
| `BaseJumpZ` | `630.0` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `BaseJumpZHeavy` | `430.0` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `JumpAddXY` | `100` | Configuration parameter |
| `JumpBlendInTime` | `0.1` | Time in seconds |
| `JumpBlendOutTime` | `0.2` | Time in seconds |
| `JumpStillBlendOutTime` | `0.2` | Time in seconds |
| `LongJumpFastThreshold` | `700` | Configuration parameter |
| `LongJumpNormalThreshold` | `500` | Configuration parameter |
| `LongJumpSlowThreshold` | `400` | Configuration parameter |
| `bCheckForGrab` | `True` | Configuration parameter |
| `bCheckForVaultOver` | `True` | Configuration parameter |
| `bCheckForWallClimb` | `True` | Configuration parameter |

#### `[TdGame.TdMove_BotJump]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `bCheckForGrab` | `True` | Configuration parameter |
| `bCheckForVaultOver` | `True` | Configuration parameter |

#### `[TdGame.TdMove_DodgeJump]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `AiAimOneShotPenalties` | `{'Easy': 75.0, 'Medium': 75.0, 'Hard': 75.0}` | Enemy weapon dispersion modifier during move |
| `BaseJumpZ` | `300.0` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `DodgeJumpInertiaConservation` | `0.3` | Configuration parameter |
| `JumpAddXY` | `600.0` | Configuration parameter |
| `JumpBlendInTime` | `0.1` | Time in seconds |
| `JumpBlendOutTime` | `0.2` | Time in seconds |
| `LookAtAIRadiusThreshold` | `90` | Configuration parameter |
| `LookAtAITraceDistance` | `250` | Configuration parameter |
| `LookAtAIVelThreshold` | `200` | Configuration parameter |
| `RedoMoveTime` | `0.3` | Time in seconds |
| `StrafeThreshold` | `0.99` | Configuration parameter |
| `bCheckForGrab` | `False` | Configuration parameter |
| `bCheckForVaultOver` | `False` | Configuration parameter |
| `bTriggersCompliment` | `False` | Configuration parameter |

#### `[TdGame.TdMove_SpringBoard]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `AiAimOneShotPenalties` | `{'Easy': 75.0, 'Medium': 75.0, 'Hard': 75.0}` | Enemy weapon dispersion modifier during move |
| `AiAimPenalties` | `{'Easy': 0.3, 'Medium': 0.4, 'Hard': 0.6}` | Enemy weapon dispersion modifier during move |
| `CheckDistanceTime` | `1.0` | Time in seconds |
| `IntermediateFootPlantDistance` | `112` | Configuration parameter |
| `IntermediateFootPlantHeight` | `64` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `SpringBoardJumpXYAdd` | `-100` | Configuration parameter |
| `SpringBoardJumpXYMin` | `400` | Configuration parameter |
| `SpringBoardJumpZ` | `950` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `SpringBoardMaxHeight` | `148` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `SpringBoardMinHeight` | `80` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `bCheckForGrab` | `True` | Configuration parameter |
| `bCheckForVaultOver` | `True` | Configuration parameter |
| `bCheckForWallClimb` | `True` | Configuration parameter |
| `bTriggersCompliment` | `True` | Configuration parameter |

#### `[TdGame.TdMove_WallKick]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `RedoMoveTime` | `1.0` | Time in seconds |
| `WallKickCheckHeight` | `90` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `WallKickExtentHeight` | `10` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `WallKickExtentWidth` | `20` | Configuration parameter |
| `WallKickMaxDistance` | `60` | Configuration parameter |
| `WallKickVelocity2D` | `300` | Speed threshold in Unreal Units/sec |
| `WallKickVelocityZ` | `580` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `bCheckForGrab` | `True` | Configuration parameter |
| `bCheckForVaultOver` | `True` | Configuration parameter |
| `bTriggersCompliment` | `True` | Configuration parameter |

#### `[TdGame.TdMove_SpeedVault]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `AiAimOneShotPenalties` | `{'Easy': 50.0, 'Medium': 50.0, 'Hard': 50.0}` | Enemy weapon dispersion modifier during move |
| `MaxTimeToLedge` | `0.4` | Time in seconds |
| `VaultClearObjectHeight` | `35` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `VaultTypes` | `[{'AnimName': 'autostepuprightleg', 'bVaultOnto': True, 'MinHeight': 0, 'MaxHeight': 48, 'MinSpeedZ': -600, 'MaxSpeedZ': 0, 'MaxDistanceTime': 0.2, 'ClampSpeedMin': 100, 'ClampSpeedMax': 300, 'VaultTimeUp': 0.0, 'VaultTimeOver': 0.3, 'VaultTimeDown': 0.2, 'OverObstacleTime': 0.2, 'HandplantOffset': {'X': 0.0, 'Y': 0.0, 'Z': 5.0}, 'HandIKOffset': {'X': 0.0, 'Y': 0.0, 'Z': 0.0}, 'LedgeOffset': {'X': 0.0, 'Y': 0.0, 'Z': 90.0}, 'bLeftHandIK': False, 'bRightHandIK': False}, {'AnimName': 'stepuprightleg88', 'bVaultOnto': True, 'MinHeight': 48, 'MaxHeight': 148, 'MinSpeedZ': 0, 'MaxSpeedZ': 700, 'MaxMomentum': 200.0, 'MaxDistanceTime': 0.4, 'ClampSpeedMin': 200, 'ClampSpeedMax': 700, 'VaultTimeOver': 0.4, 'VaultTimeDown': 0.25, 'OverObstacleTime': 0.2, 'LedgeOffset': {'X': 0.0, 'Y': -20.0, 'Z': 60.0}}, {'AnimName': 'vaultOnto', 'bVaultOnto': True, 'MinHeight': 64, 'MaxHeight': 148, 'MinSpeedZ': 0, 'MaxSpeedZ': 10000, 'MaxDistanceTime': 0.4, 'ClampSpeedMin': 400, 'ClampSpeedMax': 720, 'SpeedAddition': 80.0, 'VaultTimeUp': 0.0, 'VaultTimeOver': 0.35, 'VaultTimeDown': 0.3, 'OverObstacleTime': 0.2, 'HandplantOffset': {'X': 0.0, 'Y': 0.0, 'Z': 5.0}, 'HandIKOffset': {'X': 0.0, 'Y': -7.0, 'Z': 0.0}, 'LedgeOffset': {'X': 0.0, 'Y': 0.0, 'Z': 25.0}, 'bLeftHandIK': True, 'bRightHandIK': False, 'bIsStringable': True}, {'AnimName': 'vaultOver', 'bVaultOnto': False, 'MinHeight': 64, 'MaxHeight': 148, 'MinSpeedZ': 0, 'MaxSpeedZ': 10000, 'MaxDistanceTime': 0.4, 'ClampSpeedMin': 400, 'ClampSpeedMax': 720, 'SpeedAddition': 80.0, 'VaultTimeUp': 0.0, 'VaultTimeOver': 0.35, 'VaultTimeDown': 0.3, 'OverObstacleTime': 0.2, 'HandplantOffset': {'X': 0.0, 'Y': 0.0, 'Z': 5.0}, 'HandIKOffset': {'X': 0.0, 'Y': -7.0, 'Z': 0.0}, 'LedgeOffset': {'X': 0.0, 'Y': 0.0, 'Z': 25.0}, 'bLeftHandIK': True, 'bRightHandIK': False, 'bIsStringable': True}, {'AnimName': 'VaultOverHigh', 'bVaultOnto': False, 'MinHeight': 145, 'MaxHeight': 192, 'MinSpeedZ': 50, 'MaxSpeedZ': 10000, 'MaxDistanceTime': 0.4, 'ClampSpeedMin': 200, 'ClampSpeedMax': 400, 'VaultTimeUp': 0.28, 'VaultTimeOver': 0.3, 'VaultTimeDown': 0.45, 'OverObstacleTime': 0.35, 'HandplantOffset': {'X': 0.0, 'Y': -40.0, 'Z': -69.0}, 'HandIKOffset': {'X': 0.0, 'Y': -7.0, 'Z': 0.0}, 'LedgeOffset': {'X': 0.0, 'Y': 0.0, 'Z': 5.0}, 'bLeftHandIK': False, 'bRightHandIK': False, 'bResetCamera': True}, {'AnimName': 'VaultOntoHigh', 'bVaultOnto': True, 'MinHeight': 145, 'MaxHeight': 192, 'MinSpeedZ': 50, 'MaxSpeedZ': 10000, 'MaxDistanceTime': 0.4, 'ClampSpeedMin': 200, 'ClampSpeedMax': 400, 'VaultTimeUp': 0.27, 'VaultTimeOver': 0.3, 'VaultTimeDown': 0.6, 'OverObstacleTime': 0.35, 'HandplantOffset': {'X': 0.0, 'Y': -65.0, 'Z': -15.0}, 'HandIKOffset': {'X': 0.0, 'Y': -7.0, 'Z': 0.0}, 'LedgeOffset': {'X': 0.0, 'Y': 0.0, 'Z': 35.0}, 'bLeftHandIK': True, 'bRightHandIK': True, 'bResetCamera': True}]` | Configuration parameter |
| `bTriggersCompliment` | `False` | Configuration parameter |

#### `[TdGame.TdMove_StepUp]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `StepUpDistanceLimit` | `60` | Configuration parameter |
| `StepUpHighMaxHeight` | `148` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `StepUpHighMinHeight` | `112` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `StepUpLowMinHeight` | `33` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `StepUpMediumMinHeight` | `68` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `StepUpOptimalHighHeight` | `144` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `StepUpOptimalLowHeight` | `48` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `StepUpOptimalMediumHeight` | `88` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `StepUpSpeedLimit` | `300` | Speed threshold in Unreal Units/sec |

#### `[TdGame.TdMove_AutoStepUp]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `StepUpDistanceLimit` | `60` | Configuration parameter |
| `StepUpHighMaxHeight` | `48` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `StepUpHighMinHeight` | `8` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `StepUpLowMinHeight` | `33` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `StepUpMediumMinHeight` | `68` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `StepUpOptimalHighHeight` | `144` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `StepUpOptimalLowHeight` | `48` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `StepUpOptimalMediumHeight` | `88` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `StepUpSpeedLimit` | `300` | Speed threshold in Unreal Units/sec |

#### `[TdGame.TdMove_VaultOver]`
*Inherits defaults with no parameter overrides.*

### 2.3 Wall Interactions (Wallrun & Wallclimb)

#### `[TdGame.TdMove_WallRun]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `AiAimPenalties` | `{'Easy': 0.4, 'Medium': 0.5, 'Hard': 0.7}` | Enemy weapon dispersion modifier during move |
| `LookAlongWallInterpolationTime` | `0.2` | Time in seconds |
| `MaximumVelocityIntoWall` | `700` | Speed threshold in Unreal Units/sec |
| `MinimumVelocityIntoWall` | `600` | Speed threshold in Unreal Units/sec |
| `PlayCameraHitWallEffect` | `True` | Configuration parameter |
| `RedoMoveTime` | `0.15` | Time in seconds |
| `StickyAimedModifier` | `1.0` | Configuration parameter |
| `StickyAngle` | `4000` | Angle in degrees / Unreal rotation units |
| `TimeToDo90Turn` | `0.25` | Time in seconds |
| `WallRunningDelayPawnRotationTime` | `0.01` | Time in seconds |
| `WallRunningDistanceForIntoWall` | `180` | Configuration parameter |
| `WallRunningForwardCheckDistance` | `50` | Configuration parameter |
| `WallRunningForwardMaxStartAngle` | `57` | Angle in degrees / Unreal rotation units |
| `WallRunningForwardMinStartAngle` | `0` | Angle in degrees / Unreal rotation units |
| `WallRunningHorisontalAcceleration` | `820` | Configuration parameter |
| `WallRunningHorisontalAlignSpeed` | `700` | Speed threshold in Unreal Units/sec |
| `WallRunningHorisontalDeceleration` | `500` | Kinematic drag / slowdown factor |
| `WallRunningHorisontalFriction` | `0.05` | Kinematic drag / slowdown factor |
| `WallRunningHorisontalInitialZHeight` | `170` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `WallRunningIntoWallrunBlendInTime` | `0.2` | Time in seconds |
| `WallRunningIntoWallrunBlendOutTime` | `0.2` | Time in seconds |
| `WallRunningMinSpeed` | `200` | Speed threshold in Unreal Units/sec |
| `WallRunningMinWallHeight` | `192` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `WallRunningMoveToIntoPositionDegreeThreshold` | `70` | Configuration parameter |
| `WallRunningRotatePawnAlongWallTime` | `0.4` | Time in seconds |
| `WallRunningStrafeCheckDistance` | `50` | Configuration parameter |
| `WallRunningStrafeStartAngle` | `60` | Angle in degrees / Unreal rotation units |
| `WallRunningVelocityStartLimit` | `300` | Speed threshold in Unreal Units/sec |
| `WallRunningVelocityStopLimit` | `-500` | Speed threshold in Unreal Units/sec |
| `WallrunStartUpperBodyAnimPlayRate` | `0.6` | Configuration parameter |
| `bTriggersCompliment` | `False` | Configuration parameter |

#### `[TdGame.TdMove_WallClimb]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `AddOnSpeed2DHeight` | `60` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `AddOnSpeed2DMaxLimit` | `650` | Speed threshold in Unreal Units/sec |
| `AddOnSpeedZHeight` | `130` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `AddOnSpeedZMaxLimit` | `320` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `AiAimPenalties` | `{'Easy': 0.4, 'Medium': 0.5, 'Hard': 0.7}` | Enemy weapon dispersion modifier during move |
| `FrictionModifier` | `0.3` | Kinematic drag / slowdown factor |
| `HandPlantCheckHeight` | `112` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `MaxIntoWallClimbVelocityToDoubleJump` | `100` | Speed threshold in Unreal Units/sec |
| `MinLegdeZNormal` | `0.707` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `MinUpwardsVelocityToDoubleJump` | `100` | Speed threshold in Unreal Units/sec |
| `MinWallHeight` | `180` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `WallClimbingGravity` | `800` | Configuration parameter |
| `WallClimbingMaxDistance2D` | `120` | Configuration parameter |
| `WallClimbingVelocityStartLimit` | `0` | Speed threshold in Unreal Units/sec |
| `WallClimbingVerticalFriction` | `6` | Kinematic drag / slowdown factor |
| `WallClimbingVerticalStartAngle` | `33 //55` | Angle in degrees / Unreal rotation units |
| `bCheckForEdgeInVelDir` | `True` | Configuration parameter |
| `bCheckForGrab` | `True` | Configuration parameter |
| `bCheckForVaultOver` | `True` | Configuration parameter |
| `bTriggersCompliment` | `False` | Configuration parameter |

#### `[TdGame.TdMove_WallrunJump]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `AiAimOneShotPenalties` | `{'Easy': 75.0, 'Medium': 75.0, 'Hard': 75.0}` | Enemy weapon dispersion modifier during move |
| `WallRunningJumpOffZHeightForward` | `100` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `WallRunningJumpOffZHeightMaxAddTurned` | `60` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `WallRunningPushAwaySpeedNoob` | `120` | Speed threshold in Unreal Units/sec |
| `WallRunningPushAwaySpeedProAdd` | `400` | Speed threshold in Unreal Units/sec |
| `WallRunningPushForwardSpeedMin` | `0.1` | Speed threshold in Unreal Units/sec |
| `bCheckForGrab` | `True` | Configuration parameter |
| `bCheckForVaultOver` | `True` | Configuration parameter |
| `bCheckForWallClimb` | `[True, True]` | Configuration parameter |
| `bTriggersCompliment` | `False` | Configuration parameter |

#### `[TdGame.TdMove_WallrunDodgeJump]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `AiAimOneShotPenalties` | `{'Easy': 75.0, 'Medium': 75.0, 'Hard': 75.0}` | Enemy weapon dispersion modifier during move |
| `BaseJumpZ` | `300.0` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `DodgeJumpInertiaConservation` | `0.3` | Configuration parameter |
| `JumpAddXY` | `600.0` | Configuration parameter |
| `JumpBlendInTime` | `0.0` | Time in seconds |
| `JumpBlendOutTime` | `0.0` | Time in seconds |
| `bCheckForGrab` | `False` | Configuration parameter |
| `bCheckForVaultOver` | `False` | Configuration parameter |
| `bTriggersCompliment` | `True` | Configuration parameter |

#### `[TdGame.TdMove_WallClimbDodgeJump]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `AiAimOneShotPenalties` | `{'Easy': 75.0, 'Medium': 75.0, 'Hard': 75.0}` | Enemy weapon dispersion modifier during move |
| `BaseJumpZ` | `700.0` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `DodgeJumpInertiaConservation` | `1.0` | Configuration parameter |
| `JumpAddXY` | `150.0` | Configuration parameter |
| `JumpBlendInTime` | `0.2` | Time in seconds |
| `JumpBlendOutTime` | `0.2` | Time in seconds |
| `bCheckForGrab` | `True` | Configuration parameter |
| `bCheckForVaultOver` | `True` | Configuration parameter |
| `bCheckForWallClimb` | `True` | Configuration parameter |
| `bTriggersCompliment` | `True` | Configuration parameter |

#### `[TdGame.TdMove_WallClimb180TurnJump]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `AiAimOneShotPenalties` | `{'Easy': 100.0, 'Medium': 100.0, 'Hard': 100.0}` | Enemy weapon dispersion modifier during move |
| `AiAimPenalties` | `{'Easy': 0.3, 'Medium': 0.4, 'Hard': 0.6}` | Enemy weapon dispersion modifier during move |
| `JumpOffZHeight` | `250` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `JumpPushAwaySpeed` | `400` | Speed threshold in Unreal Units/sec |
| `JumpTimeWindow` | `0.6` | Time in seconds |
| `RedoMoveTime` | `1.0` | Time in seconds |
| `bCheckForGrab` | `True` | Configuration parameter |
| `bCheckForVaultOver` | `True` | Configuration parameter |
| `bCheckForWallClimb` | `True` | Configuration parameter |
| `bTriggersCompliment` | `True` | Configuration parameter |

### 2.4 Ledge & Pipe Grabbing

#### `[TdGame.TdMove_IntoGrab]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `GrabDesiredLedgeOffset` | `{'X': 30.0, 'Y': 0.0, 'Z': 92.8}` | Configuration parameter |
| `GrabMinGrabableZNormal` | `0.707` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `HangFoldedDownwardSpeedLimit` | `-300` | Speed threshold in Unreal Units/sec |
| `HangFoldedIntoGrabSpeed2DThreshold` | `50` | Speed threshold in Unreal Units/sec |
| `HangFoldedIntoGrabZSpeedThreshold` | `150` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `HangFoldedLowerDeltaDistance` | `70` | Configuration parameter |
| `HangFoldedMaxDistance` | `60` | Configuration parameter |
| `HangFoldedUpperDeltaDistance` | `35` | Configuration parameter |
| `HangHardImpactMinZSpeed` | `-1000` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `HangImpactMinZSpeed` | `-600` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `IntoGrabAlignSpeed` | `300` | Speed threshold in Unreal Units/sec |
| `IntoGrabBelowEdgeZVelocityThreshold` | `800` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `IntoGrabMaxAngle` | `70` | Angle in degrees / Unreal rotation units |
| `IntoGrabMaxDistance` | `200` | Configuration parameter |
| `IntoGrabMinInitialAlignSpeed` | `-3000` | Speed threshold in Unreal Units/sec |
| `IntoGrabZVelocityThreshold` | `150` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `MinGrabLedgeAdjustDistance` | `32` | Configuration parameter |
| `bCheckForVaultOver` | `True` | Configuration parameter |

#### `[TdGame.TdMove_IntoGrabBot]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `IntoGrabMaxAngle` | `90` | Angle in degrees / Unreal rotation units |

#### `[TdGame.TdMove_Grab]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `GrabDesiredLedgeOffset` | `{'X': 30.0, 'Y': 0.0, 'Z': 92.8}` | Configuration parameter |
| `GrabMaxAngle` | `40` | Angle in degrees / Unreal rotation units |
| `HangFreeZDistanceCheck` | `128` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `RedoMoveTime` | `0.15` | Time in seconds |
| `StartTurningAngle` | `16384` | Angle in degrees / Unreal rotation units |

#### `[TdGame.TdMove_GrabPullUp]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `GrabAllowedPullUpAngle` | `45` | Angle in degrees / Unreal rotation units |
| `bCheckForGrab` | `False` | Configuration parameter |

#### `[TdGame.TdMove_GrabJump]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `GrabAllowedJumpAngle` | `45` | Angle in degrees / Unreal rotation units |
| `GrabJumpOffZHeight` | `160` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `GrabJumpPushAwayMaxSpeed` | `400` | Speed threshold in Unreal Units/sec |
| `GrabJumpPushAwayMinSpeed` | `200` | Speed threshold in Unreal Units/sec |
| `bCheckForGrab` | `True` | Configuration parameter |
| `bCheckForVaultOver` | `True` | Configuration parameter |

#### `[TdGame.TdMove_GrabTransfer]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `AiAimOneShotPenalties` | `{'Easy': 75.0, 'Medium': 75.0, 'Hard': 75.0}` | Enemy weapon dispersion modifier during move |
| `Allowed2DTransferDistance` | `260` | Configuration parameter |
| `AllowedZTransferDistance` | `140` | Vertical distance in Unreal Units (1 uu = 1 cm) |

#### `[TdGame.TdMove_IntoClimb]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `RedoMoveTime` | `0.5` | Time in seconds |

#### `[TdGame.TdMove_Climb]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `AllowJumpAngle` | `8192` | Angle in degrees / Unreal rotation units |
| `ClimbBlendInTime` | `0.05` | Time in seconds |
| `ClimbDownBlendInTime` | `0.5` | Time in seconds |
| `ClimbDownFastVelocity` | `200` | Speed threshold in Unreal Units/sec |
| `ClimbFastUpPipeAnimRate` | `2.0` | Configuration parameter |
| `IdleBlendInTime` | `0.05` | Time in seconds |
| `RedoMoveTime` | `0.5` | Time in seconds |
| `StartTurningAngle` | `16384` | Angle in degrees / Unreal rotation units |

### 2.5 Sliding, Rolling & Landing

#### `[TdGame.TdMove_Slide]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `AiAimPenalties` | `{'Easy': 0.7, 'Medium': 0.8, 'Hard': 0.9}` | Enemy weapon dispersion modifier during move |
| `FrictionModifier` | `0.1` | Kinematic drag / slowdown factor |
| `MaxFloorInclineZ` | `0.5` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `SlideAbortSpeed` | `250` | Speed threshold in Unreal Units/sec |
| `SlideAbortTime` | `2.0` | Time in seconds |
| `SlideSoundAndEffectPollInterval` | `0.2` | Configuration parameter |
| `SlideSoundFadeInTime` | `0.1` | Time in seconds |
| `SlideSoundFadeOutTime` | `0.5` | Time in seconds |

#### `[TdGame.TdMove_SlideBot]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `FrictionModifier` | `0.035` | Kinematic drag / slowdown factor |
| `SlideAbortSpeed` | `100` | Speed threshold in Unreal Units/sec |
| `SlideAbortTime` | `1.0` | Time in seconds |

#### `[TdGame.TdMove_RumpSlide]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `AIAimOneShotPenalties` | `{'Easy': 200.0, 'Medium': 200.0, 'Hard': 200.0}` | Configuration parameter |
| `AiAimPenalties` | `{'Easy': 0.01, 'Medium': 0.04, 'Hard': 0.1}` | Enemy weapon dispersion modifier during move |
| `GravityModifier` | `0.5` | Configuration parameter |
| `InitialSpeedLoss` | `0.75` | Speed threshold in Unreal Units/sec |
| `MaxSlideSpeed` | `1000.0` | Speed threshold in Unreal Units/sec |
| `MinSlideFloorZ` | `0.9` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `RedoMoveTime` | `1.0` | Time in seconds |
| `SideControl` | `350.0` | Configuration parameter |

#### `[TdGame.TdMove_Landing]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `HardLandingDamage` | `15` | Configuration parameter |
| `HardLandingHeight` | `530.0` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `LandingSpeedReduction` | `65.0` | Speed threshold in Unreal Units/sec |
| `SkillRollLandingHeight` | `200.0` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `SoftLandingHeight` | `300.0` | Vertical distance in Unreal Units (1 uu = 1 cm) |

#### `[TdGame.TdMove_LayOnGround]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `FrictionModifier` | `0.15` | Kinematic drag / slowdown factor |

#### `[TdGame.TdMove_Coil]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `CoilMinTriggerSpeed` | `100.0` | Speed threshold in Unreal Units/sec |
| `CoilTime` | `0.5` | Time in seconds |
| `HeightBoostDuration` | `0.25` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `TotalHeightBoost` | `60.0` | Vertical distance in Unreal Units (1 uu = 1 cm) |

### 2.6 Acrobatic Traversals (Zipline, Bar Swing, Balance)

#### `[TdGame.TdMove_ZipLine]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `AIAimOneShotPenalties` | `{'Easy': 200.0, 'Medium': 200.0, 'Hard': 200.0}` | Configuration parameter |
| `AiAimPenalties` | `{'Easy': 0.01, 'Medium': 0.1, 'Hard': 0.3}` | Enemy weapon dispersion modifier during move |
| `MinZipAcceleration` | `400` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `MinZipVelocity` | `300` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `ZipFadeInTime` | `0.1` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `ZipFadeOutTime` | `0.5` | Vertical distance in Unreal Units (1 uu = 1 cm) |

#### `[TdGame.TdMove_IntoZipLine]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `IntoZiplineBlendInTime` | `0.3` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `IntoZiplineBlendOutTime` | `0.2` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `RedoMoveTime` | `0.5` | Time in seconds |
| `ZVelocityFallLimit` | `-600` | Vertical distance in Unreal Units (1 uu = 1 cm) |

#### `[TdGame.TdMove_Swing]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `AIAimOneShotPenalties` | `{'Easy': 200.0, 'Medium': 200.0, 'Hard': 200.0}` | Configuration parameter |
| `AiAimPenalties` | `{'Easy': 0.1, 'Medium': 0.2, 'Hard': 0.4}` | Enemy weapon dispersion modifier during move |
| `ExitVelocityModifier` | `600.0` | Speed threshold in Unreal Units/sec |
| `SwingAngleTimingOffset` | `1.0` | Angle in degrees / Unreal rotation units |
| `SwingExitGravityModifier` | `0.75` | Configuration parameter |
| `SwingExitGravityModifierTime` | `0.7` | Time in seconds |
| `SwingPendulumLength` | `120.0` | Configuration parameter |
| `bCheckForGrab` | `True` | Configuration parameter |
| `bCheckForVaultOver` | `True` | Configuration parameter |
| `bCheckForWallClimb` | `True` | Configuration parameter |
| `bTriggersCompliment` | `False` | Configuration parameter |

#### `[TdGame.TdMove_SwingJump]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `AIAimOneShotPenalties` | `{'Easy': 200.0, 'Medium': 200.0, 'Hard': 200.0}` | Configuration parameter |
| `AiAimPenalties` | `{'Easy': 0.2, 'Medium': 0.3, 'Hard': 0.5}` | Enemy weapon dispersion modifier during move |
| `GravityModifier` | `0.73` | Configuration parameter |
| `GravityModifierTimer` | `0.75` | Time in seconds |
| `TargetVolumeOffset` | `{'X': -120.0, 'Y': 0.0, 'Z': -20}` | Configuration parameter |
| `bCheckForGrab` | `True` | Configuration parameter |
| `bCheckForVaultOver` | `True` | Configuration parameter |
| `bCheckForWallClimb` | `True` | Configuration parameter |
| `bTriggersCompliment` | `True` | Configuration parameter |

#### `[TdGame.TdMove_Balance]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `AiAimOneShotPenalties` | `{'Easy': 50.0, 'Medium': 50.0, 'Hard': 50.0}` | Enemy weapon dispersion modifier during move |
| `CameraInfluence` | `0.3` | Configuration parameter |
| `ControlInfluence` | `1.5` | Configuration parameter |
| `GravityInfluence` | `0.3` | Configuration parameter |
| `RedoMoveTime` | `0.5` | Time in seconds |
| `SpeedInfluence` | `2.5` | Speed threshold in Unreal Units/sec |
| `SpeedModifier` | `0.34` | Speed threshold in Unreal Units/sec |
| `TimeToCounter` | `0.8` | Time in seconds |

#### `[TdGame.TdMove_LedgeWalk]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `SpeedModifier` | `0.1` | Speed threshold in Unreal Units/sec |

#### `[TdGame.TdMove_Vertigo]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `RedoMoveTime` | `3.0` | Time in seconds |
| `ZoomFOV` | `84.0` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `ZoomOutTime` | `1.2` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `ZoomRate` | `30.0` | Vertical distance in Unreal Units (1 uu = 1 cm) |

#### `[TdGame.TdMove_Interact]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `DistanceToAButton` | `48.0` | Configuration parameter |
| `DistanceToAValve` | `40.0` | Configuration parameter |

### 2.7 Combat, Melee & Disarm

#### `[TdGame.TdMove_MeleeBase]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `MaxMeleeAngle` | `0.5` | Angle in degrees / Unreal rotation units |
| `MaxMeleeDistance` | `180.0` | Configuration parameter |
| `MeleeDamage` | `50.0` | Configuration parameter |

#### `[TdGame.TdMove_Melee]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `BlendInMissed` | `0.08` | Configuration parameter |
| `BlendOutMissed` | `0.1` | Configuration parameter |
| `MeleeAssistCone` | `0.8` | Configuration parameter |
| `MeleeAssistRotationTime` | `0.2` | Time in seconds |
| `MeleeDamage` | `33.5` | Configuration parameter |

#### `[TdGame.TdMove_MeleeAir]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `HeavyAttackHoldTime` | `1.4` | Time in seconds |
| `MeleeAirAboveMaxSeparation` | `500.0` | Configuration parameter |
| `MeleeAirAboveMinAngle` | `0.8` | Angle in degrees / Unreal rotation units |
| `MeleeAirAboveMinSeparation` | `150.0` | Configuration parameter |
| `MeleeDamage` | `100.0` | Configuration parameter |
| `RedoMoveTime` | `0.3` | Time in seconds |

#### `[TdGame.TdMove_MeleeAirAbove]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `MeleeDamage` | `300.0` | Configuration parameter |

#### `[TdGame.TdMove_MeleeSlide]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `FrictionModifier` | `0.1` | Kinematic drag / slowdown factor |
| `MeleeDamage` | `60.0` | Configuration parameter |
| `SoccerKickDamage` | `60.0` | Configuration parameter |

#### `[TdGame.TdMove_MeleeWallrun]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `MeleeDamage` | `80.0` | Configuration parameter |

#### `[TdGame.TdMove_MeleeCrouch]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `MeleeDamage` | `33.5.0f` | Configuration parameter |
| `SpeedModifier` | `0.2` | Speed threshold in Unreal Units/sec |

#### `[TdGame.TdMove_MeleeBarge]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `BargeAdditionalUpForce` | `100.0` | Configuration parameter |
| `BargeForceMultiplier` | `2.4` | Configuration parameter |
| `BargeMaxDistance` | `500.0` | Configuration parameter |
| `MeleeDamage` | `50.0` | Configuration parameter |

#### `[TdGame.TdMove_Barge]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `BargeAddOnSpeed` | `200` | Speed threshold in Unreal Units/sec |
| `BargeKickThresholdSpeed` | `250` | Speed threshold in Unreal Units/sec |
| `BargeMaxSpeed` | `500` | Speed threshold in Unreal Units/sec |
| `BargeMinTraceDistance` | `90` | Configuration parameter |
| `BargeTraceTime` | `0.5` | Time in seconds |
| `RedoMoveTime` | `0.1` | Time in seconds |

#### `[TdGame.TdMove_AirBarge]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `BargeMinTraceDistance` | `300` | Configuration parameter |
| `HeightBoostDuration` | `0.25` | Vertical distance in Unreal Units (1 uu = 1 cm) |
| `TotalHeightBoost` | `60.0` | Vertical distance in Unreal Units (1 uu = 1 cm) |

#### `[TdGame.TdMove_Disarm]`
| Parameter | Exact Value | Mechanical Description |
| :--- | :--- | :--- |
| `AIAimOneShotPenalties` | `{'Easy': 200.0, 'Medium': 200.0, 'Hard': 200.0}` | Configuration parameter |
| `AiAimPenalties` | `{'Easy': 0.01, 'Medium': 0.08, 'Hard': 0.3}` | Enemy weapon dispersion modifier during move |
| `RedoMoveTime` | `0.2` | Time in seconds |

---

## Section 3: Weapon Arsenal Specification (`DefaultWeapons.ini`)

| Weapon Class | Model / Name | Mag Size | Damage | Fire Mode | Recoil / Kick | Mobility Multiplier |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| `TdWeapon_Pistol_Glock18c` | Glock 18C | 19 | 18 | Auto / Semi | Light | 1.00 |
| `TdWeapon_Pistol_Colt1911` | Colt 1911 | 7 | 45 | Semi-Auto | Medium | 1.00 |
| `TdWeapon_Pistol_BerettaM93R` | Beretta 93R | 15 | 22 | 3-Round Burst | Medium | 1.00 |
| `TdWeapon_Pistol_TaserContent` | Stun Taser | 1 | 100 (Non-lethal) | Single | Zero | 1.00 |
| `TdWeapon_SMG_SteyrTMP` | Steyr TMP | 30 | 20 | Full Auto | Low | 0.90 |
| `TdWeapon_AssaultRifle_MP5K` | HK MP5K | 30 | 24 | Full Auto | Moderate | 0.90 |
| `TdWeapon_AssaultRifle_FNSCARL` | FN SCAR-L | 30 | 35 | Full Auto | High | 0.80 |
| `TdWeapon_AssaultRifle_HKG36` | HK G36C | 30 | 34 | Full Auto | High | 0.80 |
| `TdWeapon_Shotgun_Remington870` | Remington 870 | 6 | 120 (Pellet) | Pump Action | Heavy | 0.75 |
| `TdWeapon_Shotgun_Neostead` | Neostead 2000 | 12 | 130 (Pellet) | Pump Action | Heavy | 0.75 |
| `TdWeapon_Machinegun_FNMinimi` | FN Minimi (M249) | 100 | 40 | Full Auto | Very Heavy | 0.60 |
| `TdWeapon_Sniper_BarretM95` | Barrett M95 | 5 | 250 (1-Hit Kill) | Bolt Action | Extreme | 0.50 |

---

## Section 4: Parkour & Combat Input Action Mappings (`DefaultInput.ini`)

Mirror's Edge uses a contextual "Up/Down" action philosophy rather than traditional FPS keys:

| Action Name | Default Key | Gamepad (Xbox/PS3) | Contextual Function |
| :--- | :--- | :--- | :--- |
| **UpAction** | `SpaceBar` | `Left Trigger (LT/L2)` | Jump, Wallrun initiate, Wallclimb, Grab ledge, Vault, Coil (in air) |
| **DownAction** | `LeftShift` / `C` | `Left Bumper (LB/L1)` | Crouch, Slide (while running), Skill Roll (before landing) |
| **TurnAround (180)** | `Q` | `Right Bumper (RB/R1)` | Instant 180° turn (on ground, during wallclimb, or in mid-air) |
| **Interact / Disarm** | `E` | `Y / Triangle` | Open doors, push buttons, disarm flashing red enemy weapon |
| **Attack / Strike** | `LeftMouseButton` | `Right Trigger (RT/R2)` | Melee punch, drop-kick (in air), slide kick (sliding), fire gun |
| **ReactionTime (SlowMo)** | `R` / `MiddleMouseButton` | `X / Square` | Slows time down to 0.25x speed while stamina bar discharges |
| **LookAtTarget (Route Hint)**| `LeftAlt` | `B / Circle` | Swivels camera toward next Runner Vision objective or escape route |
| **DropWeapon** | `F` | `D-Pad Down` | Discards currently carried heavy firearm to regain 100% sprint mobility |

---

## Section 5: Campaign Story Map Progression (`DefaultGame.ini`)

| Chapter | Level ID | Map Filename (`.umap`) | Level Event | Major Checkpoints |
| :--- | :--- | :--- | :--- | :--- |
| **Tutorial** | `TrainingArea` | `Tutorial_p` | `LoadLevel_Tutorial` | Tutorial intro, movement challenges |
| **Prologue: The Edge** | `SP01a` | `edge_p` | `LoadLevel_Edge` | Edge_Start, After_Intro, Rooftop_Action, SWAT_Response |
| **Chapter 1: Flight** | `SP01b` | `escape_p` | `LoadLevel_Escape` | Start, Office, Chase, Avenue, Plaza |
| **Chapter 2: Jacknife** | `SP02` | `Stormdrain_p` | `LoadLevel_Stormdrains` | Canals, Container, Gate1, Chute2, Construction, ChaseJK, JKfight |
| **Chapter 3: Heat** | `SP03` | `cranes_p` | `LoadLevel_Cranes` | SP03_Start, SP03_Office_01, SP03_Rooftop_01, SP03_Plaza_01 |
| **Chapter 4: Ropeburn** | `SP04` | `Subway_p` | `LoadLevel_Subway` | Start_Point_subway, Subway_bossfight, Renovation_Fight, Station, Tunnels, Train_Ride |
| **Chapter 5: New Eden** | `SP05` | `mall_p` | `LoadLevel_Mall` | LevelStart, CombatRooftops, Stretch, Mall |
| **Chapter 6: Pirandello Kruger** | `SP06` | `factory_p` | `LoadLevel_Factory` | Start_point, Loading_bay, Conveyor_puzzle, Training_area, Pursuit_chase |
| **Chapter 7: The Boat** | `SP07` | `boat_p` | `LoadLevel_Boat` | Start, CarDeck, Ventilation, TopBoat, Celeste_chase, Celeste |
| **Chapter 8: Kate** | `SP08` | `convoy_p` | `LoadLevel_Convoy` | Kates_convoy, Stretch2, Atrium, Sniper, Atrium_soft_cp |
| **Chapter 9: The Shard** | `SP09` | `Scraper_p` | `LoadLevel_Scraper` | Scraper_Start, Carpark, Lobby, Elevator_shaft, Rooftops, Server_room |

---

## Section 6: Time Trial Stretch Catalog & Threshold Times (`DefaultGame.ini`)

| Stretch Index | Map Filename | Stretch ID | Qualify Time | 1-Star (★) | 2-Star (★★) | 3-Star (★★★) |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| Stretch 00 | `tt_TutorialA01_p` | `ETTS_TutorialA01` | 120s | 120s | 75s | 65s |
| Stretch 01 | `tt_TutorialA02_p` | `ETTS_TutorialA02` | 130s | 130s | 85s | 75s |
| Stretch 02 | `tt_TutorialA03_p` | `ETTS_TutorialA03` | 140s | 140s | 110s | 95s |
| Stretch 03 | `tt_EdgeA01_p` | `ETTS_EdgeA01` | 70s | 70s | 55s | 47s |
| Stretch 04 | `tt_EscapeB01_p` | `ETTS_EscapeB01` | 70s | 70s | 55s | 47s |
| Stretch 05 | `tt_EscapeA01_p` | `ETTS_EscapeA01` | 80s | 80s | 67s | 61s |
| Stretch 06 | `tt_StormdrainA02_p` | `ETTS_StormdrainA02` | 105s | 105s | 85s | 78s |
| Stretch 07 | `tt_StormdrainB01_p` | `ETTS_StormdrainB01` | 120s | 120s | 90s | 82s |
| Stretch 08 | `tt_StormdrainB02_p` | `ETTS_StormdrainB02` | 80s | 80s | 63s | 57s |
| Stretch 09 | `tt_StormdrainB03_p` | `ETTS_StormdrainB03` | 70s | 70s | 59s | 53s |
| Stretch 10 | `tt_CranesA01_p` | `ETTS_CranesA01` | 75s | 75s | 62s | 56s |
| Stretch 11 | `tt_CranesC01_p` | `ETTS_CranesC01` | 85s | 85s | 70s | 62s |
| Stretch 12 | `tt_CranesB02_p` | `ETTS_CranesB02` | 105s | 105s | 85s | 77s |
| Stretch 13 | `tt_CranesB01_p` | `ETTS_CranesB01` | 80s | 80s | 69s | 62s |
| Stretch 14 | `tt_MallA01_p` | `ETTS_MallA01` | 80s | 80s | 61s | 55s |
| Stretch 15 | `tt_FactoryA01_p` | `ETTS_FactoryA01` | 95s | 95s | 80s | 70s |
| Stretch 16 | `tt_CranesD01_p` | `ETTS_CranesD01` | 65s | 65s | 32s | 27s |
| Stretch 17 | `tt_ConvoyB01_p` | `ETTS_ConvoyB01` | 80s | 80s | 64s | 58s |
| Stretch 18 | `tt_ConvoyB02_p` | `ETTS_ConvoyB02` | 80s | 80s | 64s | 58s |
| Stretch 19 | `tt_ConvoyA01_p` | `ETTS_ConvoyA01` | 120s | 120s | 75s | 55s |
| Stretch 20 | `tt_ConvoyA02_p` | `ETTS_ConvoyA02` | 85s | 85s | 67s | 60s |
| Stretch 21 | `tt_ScraperA01_p` | `ETTS_SkyscraperA01` | 120s | 120s | 75s | 68s |
| Stretch 22 | `tt_ScraperB01_p` | `ETTS_SkyscraperB01` | 75s | 75s | 67s | 62s |

---

## Section 7: AI Melee Attacks & Combat Behavior (`DefaultAIMeleeAttacks.ini`)

Enemy archetypes feature distinct melee animations and damage frames:

### `[TdGame.TdMove_BotBlock]`
| Parameter | Value |
| :--- | :--- |
| `GenericAttackProperties` | `{'HitAngle': 75, 'HitRange': 260.0, 'HitDetectionStartTime': -1.0, 'Damage': 50.0, 'AttackSpeed': 1.0, 'RotationSpeed': 360.0, 'RotationLimitAngle': 180, 'PredictionWeight': 0, 'PredictionTime': -0.1}` |

### `[TdGame.TdMove_BotMelee]`
| Parameter | Value |
| :--- | :--- |
| `GenericAttackProperties` | `{'HitAngle': 75, 'HitRange': 150.0, 'HitDetectionStartTime': -1.0, 'Damage': 20.0, 'AttackSpeed': 1.0, 'RotationSpeed': 360.0, 'RotationLimitAngle': 180, 'PredictionWeight': 0, 'PredictionTime': -0.1}` |

### `[TdGame.TdMove_BotMeleeSecondSwing_Assault]`
| Parameter | Value |
| :--- | :--- |
| `GenericAttackProperties` | `{'HitAngle': 75, 'HitRange': 180.0, 'HitDetectionStartTime': -1.0, 'Damage': 50.0, 'AttackSpeed': 1.0, 'RotationSpeed': 360.0, 'RotationLimitAngle': 180, 'PredictionWeight': 0, 'PredictionTime': -0.1}` |

### `[TdGame.TdMove_BotMeleeSecondSwing_CopRemington]`
| Parameter | Value |
| :--- | :--- |
| `GenericAttackProperties` | `{'HitAngle': 75, 'HitRange': 110.0, 'HitDetectionStartTime': -1.0, 'Damage': 50.0, 'AttackSpeed': 1.0, 'RotationSpeed': 360.0, 'RotationLimitAngle': 180, 'PredictionWeight': 0, 'PredictionTime': -0.1}` |

### `[TdGame.TdMove_BotMeleeSecondSwing_Sniper]`
| Parameter | Value |
| :--- | :--- |
| `GenericAttackProperties` | `{'HitAngle': 75, 'HitRange': 150.0, 'HitDetectionStartTime': -1.0, 'Damage': 40.0, 'AttackSpeed': 1.0, 'RotationSpeed': 360.0, 'RotationLimitAngle': 180, 'PredictionWeight': 0, 'PredictionTime': -0.1}` |

### `[TdGame.TdMove_BotMeleeSecondSwing_Support]`
| Parameter | Value |
| :--- | :--- |
| `GenericAttackProperties` | `{'HitAngle': 75, 'HitRange': 150.0, 'HitDetectionStartTime': -1.0, 'Damage': 60.0, 'AttackSpeed': 1.0, 'RotationSpeed': 360.0, 'RotationLimitAngle': 180, 'PredictionWeight': 0, 'PredictionTime': -0.1}` |

### `[TdGame.TdMove_BotPursuitFinishingAttack]`
| Parameter | Value |
| :--- | :--- |
| `FinishingAttackProperties` | `{'HitDetectionStartTime': -1.0, 'Damage': 700.0, 'AttackSpeed': 1.0, 'RotationSpeed': 0.0}` |

### `[TdGame.TdMove_Melee_BossCeleste]`
| Parameter | Value |
| :--- | :--- |
| `JumpKickAttackPropertiesE` | `{'HitAngle': 40, 'HitRange': 170.0, 'HitDetectionStartTime': -1.0, 'Damage': 25.0, 'MissedAttackPenelty': 2.0, 'AttackSpeed': 0.6, 'RotationSpeed': 0.0, 'RotationLimitAngle': 0, 'PredictionWeight': 1.2, 'PredictionTime': 0.3, 'bIsInterruptableByDodge': False, 'InvulnerableDamageTypes[0]': 'Dummy'}` |
| `JumpKickAttackPropertiesH` | `{'HitAngle': 40, 'HitRange': 170.0, 'HitDetectionStartTime': -1.0, 'Damage': 35.0, 'MissedAttackPenelty': 1.0, 'AttackSpeed': 0.7, 'RotationSpeed': 0.0, 'RotationLimitAngle': 0, 'PredictionWeight': 1.0, 'PredictionTime': 0.3, 'bIsInterruptableByDodge': False, 'InvulnerableDamageTypes[0]': 'AllMelee'}` |
| `JumpKickAttackPropertiesN` | `{'HitAngle': 40, 'HitRange': 170.0, 'HitDetectionStartTime': -1.0, 'Damage': 25.0, 'MissedAttackPenelty': 2.0, 'AttackSpeed': 0.6, 'RotationSpeed': 0.0, 'RotationLimitAngle': 0, 'PredictionWeight': 1.2, 'PredictionTime': 0.3, 'bIsInterruptableByDodge': False, 'InvulnerableDamageTypes[0]': 'Dummy'}` |
| `RunAttackPropertiesE` | `{'HitAngle': 90, 'HitRange': 120.0, 'HitDetectionStartTime': 0, 'Damage': 20.0, 'MissedAttackPenelty': 2.0, 'AttackSpeed': 1.0, 'RotationSpeed': 0.0, 'RotationLimitAngle': 0, 'PredictionWeight': 1.2, 'PredictionTime': 0.1, 'bIsInterruptableByDodge': False, 'InvulnerableDamageTypes[0]': 'Dummy'}` |
| `RunAttackPropertiesH` | `{'HitAngle': 90, 'HitRange': 120.0, 'HitDetectionStartTime': 0, 'Damage': 30.0, 'MissedAttackPenelty': 1.0, 'AttackSpeed': 1.1, 'RotationSpeed': 0.0, 'RotationLimitAngle': 0, 'PredictionWeight': 1.0, 'PredictionTime': 0.1, 'bIsInterruptableByDodge': False, 'InvulnerableDamageTypes[0]': 'AllMelee'}` |
| `RunAttackPropertiesN` | `{'HitAngle': 90, 'HitRange': 120.0, 'HitDetectionStartTime': 0, 'Damage': 20.0, 'MissedAttackPenelty': 2.0, 'AttackSpeed': 1.0, 'RotationSpeed': 0.0, 'RotationLimitAngle': 0, 'PredictionWeight': 1.2, 'PredictionTime': 0.1, 'bIsInterruptableByDodge': False, 'InvulnerableDamageTypes[0]': 'Dummy'}` |
| `ShoveAttackPropertiesE` | `{'HitAngle': 65, 'HitRange': 140.0, 'HitDetectionStartTime': -1.0, 'Damage': 15.0, 'MissedAttackPenelty': 0, 'AttackSpeed': 0.95, 'RotationSpeed': 360.0, 'RotationLimitAngle': 360, 'PredictionWeight': 0, 'PredictionTime': -1, 'bIsInterruptableByDodge': False}` |
| `ShoveAttackPropertiesH` | `{'HitAngle': 65, 'HitRange': 140.0, 'HitDetectionStartTime': -1.0, 'Damage': 15.0, 'MissedAttackPenelty': 0.35, 'AttackSpeed': 1.1, 'RotationSpeed': 360.0, 'RotationLimitAngle': 360, 'PredictionWeight': 0, 'PredictionTime': -1, 'bIsInterruptableByDodge': True, 'InvulnerableDamageTypes[0]': 'AllMelee'}` |
| `ShoveAttackPropertiesN` | `{'HitAngle': 65, 'HitRange': 140.0, 'HitDetectionStartTime': -1.0, 'Damage': 15.0, 'MissedAttackPenelty': 0.0, 'AttackSpeed': 1.0, 'RotationSpeed': 360.0, 'RotationLimitAngle': 360, 'PredictionWeight': 0, 'PredictionTime': -1, 'bIsInterruptableByDodge': False, 'InvulnerableDamageTypes[0]': 'AllMelee'}` |
| `StandAttackPropertiesE` | `{'HitAngle': 65, 'HitRange': 100.0, 'HitDetectionStartTime': -1.0, 'Damage': 15.0, 'MissedAttackPenelty': 1.0, 'AttackSpeed': 0.7, 'RotationSpeed': 0.0, 'RotationLimitAngle': 180, 'PredictionWeight': 0.8, 'PredictionTime': 0.15, 'bIsInterruptableByDodge': False, 'InvulnerableDamageTypes[0]': 'Dummy'}` |
| `StandAttackPropertiesH` | `{'HitAngle': 65, 'HitRange': 100.0, 'HitDetectionStartTime': -1.0, 'Damage': 25.0, 'MissedAttackPenelty': 0.8, 'AttackSpeed': 0.8, 'RotationSpeed': 0.0, 'RotationLimitAngle': 180, 'PredictionWeight': 0.8, 'PredictionTime': 0.15, 'bIsInterruptableByDodge': True, 'InvulnerableDamageTypes[0]': 'MeleePunsh'}` |
| `StandAttackPropertiesN` | `{'HitAngle': 65, 'HitRange': 100.0, 'HitDetectionStartTime': -1.0, 'Damage': 15.0, 'MissedAttackPenelty': 1.0, 'AttackSpeed': 0.7, 'RotationSpeed': 0.0, 'RotationLimitAngle': 180, 'PredictionWeight': 0.8, 'PredictionTime': 0.15, 'bIsInterruptableByDodge': False, 'InvulnerableDamageTypes[0]': 'Dummy'}` |

### `[TdGame.TdMove_Melee_Riot]`
| Parameter | Value |
| :--- | :--- |
| `ShieldPushProperties` | `{'HitAngle': 180, 'HitRange': 100.0, 'HitDetectionStartTime': 0.25, 'Damage': 50.0, 'AttackSpeed': 1.0, 'RotationSpeed': 0.0, 'RotationLimitAngle': 180, 'PredictionWeight': 0, 'PredictionTime': -0.1}` |

### `[TdGame.TdMove_PursuitMelee]`
| Parameter | Value |
| :--- | :--- |
| `JumpKickAttackPropertiesE` | `{'HitAngle': 40, 'HitRange': 170.0, 'AttackHeightAdjustment': 0, 'HitDetectionStartTime': -1.0, 'Damage': 50.0, 'MissedAttackPenelty': 2, 'AttackSpeed': 0.95, 'RotationSpeed': 0.0, 'RotationLimitAngle': 120, 'PredictionWeight': 1, 'PredictionTime': 0.3, 'bIsInterruptableByDodge': False, 'InvulnerableDamageTypes[0]': 'AllMelee'}` |
| `JumpKickAttackPropertiesH` | `{'HitAngle': 40, 'HitRange': 170.0, 'AttackHeightAdjustment': 0, 'HitDetectionStartTime': -1.0, 'Damage': 50.0, 'MissedAttackPenelty': 0.55, 'AttackSpeed': 1.2, 'RotationSpeed': 0.0, 'RotationLimitAngle': 120, 'PredictionWeight': 0.8, 'PredictionTime': 0.3, 'bIsInterruptableByDodge': False, 'InvulnerableDamageTypes[0]': 'AllMelee'}` |
| `JumpKickAttackPropertiesN` | `{'HitAngle': 40, 'HitRange': 170.0, 'AttackHeightAdjustment': 0, 'HitDetectionStartTime': -1.0, 'Damage': 50.0, 'MissedAttackPenelty': 1.5, 'AttackSpeed': 1.0, 'RotationSpeed': 0.0, 'RotationLimitAngle': 0, 'PredictionWeight': 1, 'PredictionTime': 0.3, 'bIsInterruptableByDodge': False, 'InvulnerableDamageTypes[0]': 'AllMelee'}` |
| `RunAttackPropertiesE` | `{'HitAngle': 90, 'HitRange': 120.0, 'AttackHeightAdjustment': 0, 'HitDetectionStartTime': 0, 'Damage': 35.0, 'MissedAttackPenelty': 2.2, 'AttackSpeed': 0.5, 'RotationSpeed': 0.0, 'RotationLimitAngle': 80, 'PredictionWeight': 1, 'PredictionTime': 0.3, 'bIsInterruptableByDodge': False}` |
| `RunAttackPropertiesH` | `{'HitAngle': 90, 'HitRange': 120.0, 'AttackHeightAdjustment': 0, 'HitDetectionStartTime': 0, 'Damage': 35.0, 'MissedAttackPenelty': 0.65, 'AttackSpeed': 0.6, 'RotationSpeed': 0.0, 'RotationLimitAngle': 80, 'PredictionWeight': 0.8, 'PredictionTime': 0.3, 'bIsInterruptableByDodge': False, 'InvulnerableDamageTypes[0]': 'AllMelee'}` |
| `RunAttackPropertiesN` | `{'HitAngle': 90, 'HitRange': 120.0, 'AttackHeightAdjustment': 0, 'HitDetectionStartTime': 0, 'Damage': 35.0, 'MissedAttackPenelty': 1.7, 'AttackSpeed': 0.55, 'RotationSpeed': 0.0, 'RotationLimitAngle': 0, 'PredictionWeight': 1, 'PredictionTime': 0.3, 'bIsInterruptableByDodge': False, 'InvulnerableDamageTypes[0]': 'AllMelee'}` |
| `ShoveAttackPropertiesE` | `{'HitAngle': 65, 'HitRange': 140.0, 'AttackHeightAdjustment': 0, 'HitDetectionStartTime': -1.0, 'Damage': 15.0, 'MissedAttackPenelty': 0, 'AttackSpeed': 0.95, 'RotationSpeed': 360.0, 'RotationLimitAngle': 360, 'PredictionWeight': 0, 'PredictionTime': -1, 'bIsInterruptableByDodge': False}` |
| `ShoveAttackPropertiesH` | `{'HitAngle': 65, 'HitRange': 140.0, 'AttackHeightAdjustment': 0, 'HitDetectionStartTime': -1.0, 'Damage': 15.0, 'MissedAttackPenelty': 0.35, 'AttackSpeed': 1.1, 'RotationSpeed': 360.0, 'RotationLimitAngle': 360, 'PredictionWeight': 0, 'PredictionTime': -1, 'bIsInterruptableByDodge': True, 'InvulnerableDamageTypes[0]': 'AllMelee'}` |
| `ShoveAttackPropertiesN` | `{'HitAngle': 65, 'HitRange': 140.0, 'AttackHeightAdjustment': 0, 'HitDetectionStartTime': -1.0, 'Damage': 15.0, 'MissedAttackPenelty': 0.0, 'AttackSpeed': 1.0, 'RotationSpeed': 360.0, 'RotationLimitAngle': 360, 'PredictionWeight': 0, 'PredictionTime': -1, 'bIsInterruptableByDodge': False, 'InvulnerableDamageTypes[0]': 'AllMelee'}` |
| `SlideAttackPropertiesE` | `{'HitAngle': 120, 'HitRange': 200.0, 'AttackHeightAdjustment': -70, 'HitDetectionStartTime': 0.5, 'Damage': 35.0, 'MissedAttackPenelty': 1.5, 'AttackSpeed': 0.95, 'RotationSpeed': 0.0, 'RotationLimitAngle': 80, 'PredictionWeight': 1, 'PredictionTime': 0.3, 'bIsInterruptableByDodge': False, 'InvulnerableDamageTypes[0]': 'AllMelee'}` |
| `SlideAttackPropertiesH` | `{'HitAngle': 120, 'HitRange': 200.0, 'AttackHeightAdjustment': -70, 'HitDetectionStartTime': 0.5, 'Damage': 35.0, 'MissedAttackPenelty': 0.65, 'AttackSpeed': 1.2, 'RotationSpeed': 0.0, 'RotationLimitAngle': 80, 'PredictionWeight': 0.8, 'PredictionTime': 0.3, 'bIsInterruptableByDodge': False, 'InvulnerableDamageTypes[0]': 'AllMelee'}` |
| `SlideAttackPropertiesN` | `{'HitAngle': 120, 'HitRange': 200.0, 'AttackHeightAdjustment': -70, 'HitDetectionStartTime': 0.5, 'Damage': 35.0, 'MissedAttackPenelty': 1.0, 'AttackSpeed': 1.0, 'RotationSpeed': 0.0, 'RotationLimitAngle': 0, 'PredictionWeight': 1, 'PredictionTime': 0.3, 'bIsInterruptableByDodge': False, 'InvulnerableDamageTypes[0]': 'AllMelee'}` |
| `StandAttackPropertiesE` | `{'HitAngle': 100, 'HitRange': 120.0, 'AttackHeightAdjustment': 0, 'HitDetectionStartTime': -1.0, 'Damage': 35.0, 'MissedAttackPenelty': 0, 'AttackSpeed': 0.95, 'RotationSpeed': 0.0, 'RotationLimitAngle': 180, 'PredictionWeight': 1, 'PredictionTime': 0.3, 'bIsInterruptableByDodge': False}` |
| `StandAttackPropertiesH` | `{'HitAngle': 100, 'HitRange': 120.0, 'AttackHeightAdjustment': 0, 'HitDetectionStartTime': -1.0, 'Damage': 35.0, 'MissedAttackPenelty': 0.35, 'AttackSpeed': 1.2, 'RotationSpeed': 0.0, 'RotationLimitAngle': 180, 'PredictionWeight': 0.8, 'PredictionTime': 0.3, 'bIsInterruptableByDodge': True, 'InvulnerableDamageTypes[0]': 'MeleePunsh'}` |
| `StandAttackPropertiesN` | `{'HitAngle': 100, 'HitRange': 120.0, 'AttackHeightAdjustment': 0, 'HitDetectionStartTime': -1.0, 'Damage': 35.0, 'MissedAttackPenelty': 0.0, 'AttackSpeed': 1.0, 'RotationSpeed': 0.0, 'RotationLimitAngle': 180, 'PredictionWeight': 0.8, 'PredictionTime': 0.3, 'bIsInterruptableByDodge': False, 'InvulnerableDamageTypes[0]': 'MeleePunsh'}` |

### `[TdGame.TdMove_melee_PatrolCop]`
| Parameter | Value |
| :--- | :--- |
| `GenericAttackProperties` | `{'HitAngle': 75, 'HitRange': 110.0, 'HitDetectionStartTime': -1.0, 'Damage': 40.0, 'AttackSpeed': 1.0, 'RotationSpeed': 360.0, 'RotationLimitAngle': 180, 'PredictionWeight': 0, 'PredictionTime': -0.1}` |

### `[TdGame.TdMove_melee_PatrolCop_Remington]`
| Parameter | Value |
| :--- | :--- |
| `GenericAttackProperties` | `{'HitAngle': 75, 'HitRange': 110.0, 'HitDetectionStartTime': -1.0, 'Damage': 50.0, 'AttackSpeed': 1.0, 'RotationSpeed': 360.0, 'RotationLimitAngle': 180, 'PredictionWeight': 0, 'PredictionTime': -0.1}` |

### `[TdGame.TdMove_melee_SupportCop]`
| Parameter | Value |
| :--- | :--- |
| `GenericAttackProperties` | `{'HitAngle': 75, 'HitRange': 150.0, 'HitDetectionStartTime': -1.0, 'Damage': 60.0, 'AttackSpeed': 1.0, 'RotationSpeed': 360.0, 'RotationLimitAngle': 180, 'PredictionWeight': 0, 'PredictionTime': -0.1}` |

### `[TdGame.TdMove_melee_assault]`
| Parameter | Value |
| :--- | :--- |
| `GenericAttackProperties` | `{'HitAngle': 75, 'HitRange': 80.0, 'HitDetectionStartTime': -1.0, 'Damage': 50.0, 'AttackSpeed': 1.0, 'RotationSpeed': 360.0, 'RotationLimitAngle': 180, 'PredictionWeight': 0, 'PredictionTime': -0.1}` |

### `[TdGame.TdMove_meleedummy]`
| Parameter | Value |
| :--- | :--- |
| `GenericAttackProperties` | `{'HitAngle': 140, 'HitRange': 120.0, 'HitDetectionStartTime': -1.0, 'Damage': 50.0, 'AttackSpeed': 1.0, 'RotationSpeed': 0.0, 'RotationLimitAngle': 180, 'PredictionWeight': 0, 'PredictionTime': -0.1}` |

