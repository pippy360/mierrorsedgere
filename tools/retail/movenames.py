"""EMovement, decoded from TdGame.u - GENERATED.

Regenerate with:  python -m tools.retail.movestates --emovement

The value of ATdPawn::MovementState indexes this list. It is the game's
own name for what Faith is doing, and the reason the telemetry ring
can report a move instead of leaving the harness to infer one from a
z-curve.
"""

MOVES = [
    "MOVE_None",                         # 0
    "MOVE_Walking",                      # 1
    "MOVE_Falling",                      # 2
    "MOVE_Grabbing",                     # 3
    "MOVE_WallRunningRight",             # 4
    "MOVE_WallRunningLeft",              # 5
    "MOVE_WallClimbing",                 # 6
    "MOVE_SpringBoarding",               # 7
    "MOVE_SpeedVaulting",                # 8
    "MOVE_VaultOver",                    # 9
    "MOVE_GrabPullUp",                   # 10
    "MOVE_Jump",                         # 11
    "MOVE_WallRunJump",                  # 12
    "MOVE_GrabJump",                     # 13
    "MOVE_IntoGrab",                     # 14
    "MOVE_Crouch",                       # 15
    "MOVE_Slide",                        # 16
    "MOVE_Melee",                        # 17
    "MOVE_Snatch",                       # 18
    "MOVE_Barge",                        # 19
    "MOVE_Landing",                      # 20
    "MOVE_Climb",                        # 21
    "MOVE_IntoClimb",                    # 22
    "MOVE_WallKick",                     # 23
    "MOVE_180Turn",                      # 24
    "MOVE_180TurnInAir",                 # 25
    "MOVE_LayOnGround",                  # 26
    "MOVE_IntoZipLine",                  # 27
    "MOVE_ZipLine",                      # 28
    "MOVE_Balance",                      # 29
    "MOVE_LedgeWalk",                    # 30
    "MOVE_GrabTransfer",                 # 31
    "MOVE_MeleeAir",                     # 32
    "MOVE_DodgeJump",                    # 33
    "MOVE_WallRunDodgeJump",             # 34
    "MOVE_Stumble",                      # 35
    "MOVE_Snatched",                     # 36
    "MOVE_StepUp",                       # 37
    "MOVE_RumpSlide",                    # 38
    "MOVE_Interact",                     # 39
    "MOVE_WallRun",                      # 40
    "MOVE_BotStop",                      # 41
    "MOVE_BotStartWalking",              # 42
    "MOVE_BotStartRunning",              # 43
    "MOVE_BotTurnRunning",               # 44
    "MOVE_BotTurnStanding",              # 45
    "MOVE_ExitCover",                    # 46
    "MOVE_Vertigo",                      # 47
    "MOVE_MeleeSlide",                   # 48
    "MOVE_WallClimbDodgeJump",           # 49
    "MOVE_WallClimb180TurnJump",         # 50
    "MOVE_WallClimbDodgeJumpLeft",       # 51
    "MOVE_WallClimbDodgeJumpRight",      # 52
    "MOVE_MeleeVault",                   # 53
    "MOVE_BotMeleeSecondSwing",          # 54
    "MOVE_StumbleHard",                  # 55
    "MOVE_BotRoll",                      # 56
    "MOVE_BotFlip",                      # 57
    "MOVE_Backflip_OBSOLETE",            # 58
    "MOVE_BackflipToRun_OBSOLETE",       # 59
    "MOVE_Swing",                        # 60
    "MOVE_Coil",                         # 61
    "MOVE_MeleeWallrun",                 # 62
    "MOVE_MeleeCrouch",                  # 63
    "MOVE_BotJumpShort",                 # 64
    "MOVE_BotJumpMedium",                # 65
    "MOVE_BotJumpLong",                  # 66
    "MOVE_JumpIntoGrab",                 # 67
    "MOVE_StandGrabHeaveBot",            # 68
    "MOVE_BotMeleeDodge",                # 69
    "MOVE_FinishAttack",                 # 70
    "MOVE_MeleeBarge",                   # 71
    "MOVE_FallingUncontrolled",          # 72
    "MOVE_SwingJump",                    # 73
    "MOVE_AnimationPlayback",            # 74
    "MOVE_EnterCover",                   # 75
    "MOVE_Cover",                        # 76
    "MOVE_StumbleFalling",               # 77
    "MOVE_SoftLanding",                  # 78
    "MOVE_HeadButtedByCeleste",          # 79
    "MOVE_MeleeOriginalCeleste_OBSOLETE", # 80
    "MOVE_AutoStepUp",                   # 81
    "MOVE_MeleeAirAbove",                # 82
    "MOVE_MeleeCounterAttack_OBSOLETE",  # 83
    "MOVE_Block",                        # 84
    "MOVE_AirBarge",                     # 85
    "MOVE_RB_Bullrush_OBSOLETE",         # 86
    "MOVE_RB_Bullrush_End_OBSOLETE",     # 87
    "MOVE_RB_HitWall_OBSOLETE",          # 88
    "MOVE_RB_HitFence_OBSOLETE",         # 89
    "MOVE_RB_Ledge_OBSOLETE",            # 90
    "MOVE_SkillRoll",                    # 91
    "MOVE_BotGetDistance",               # 92
    "MOVE_Cutscene",                     # 93
    "MOVE_MAX",                          # 94
]


def name(value):
    """A move enum value as its name, or "?<n>"."""
    if value is None:
        return None
    return MOVES[value] if 0 <= value < len(MOVES) else "?%d" % value
