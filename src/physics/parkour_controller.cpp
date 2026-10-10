#include "parkour_controller.hpp"
#include "../game/impact_effects.hpp"
#include <cmath>
#include <algorithm>
#include <iostream>
#include <iterator>

namespace me {

namespace {
// TdPawn collision cylinder: Radius=30, CollisionHeight=90. CollisionHeight is a UE3 half-height,
// so Faith is 180 tall; crouching, sliding and coiling shrink the cylinder to 122
// (TdPawn.ShrinkCollision). UE3 PHYS_Walking / PHYS_Falling sweep the axis-aligned box of the
// cylinder ("sweep out the axis aligned bounding box of just the CollisionComponent").
constexpr float kPawnRadius = 30.0f;
constexpr float kPawnHeight = 180.0f;

// SequenceLength of the first-person disarm animations: the weapon's own set where it has them
// (AS_C1P_TwoHanded_*), else AS_C1P_OneHanded_Common's.
float snatch_length(const std::string& weapon, const std::string& anim) {
    struct Row { const char* weapon; float fwd, back; };
    static const Row kOwn[] = {
        {"FNMinimi", 3.8667f, 4.6667f}, {"FNSCARL", 2.3333f, 1.5f}, {"G36C", 2.3333f, 1.5f}, {"M95", 2.1f, 1.5f},
        {"MP5K", 2.5333f, 1.5f}, {"Neostead", 2.4333f, 1.5f}, {"Remington", 1.5f, 1.9333f},
    };
    const bool back = anim == "SnatchBack";
    for (const Row& r : kOwn) {
        if (weapon.find(r.weapon) != std::string::npos) return back ? r.back : r.fwd;
    }
    if (back) return 1.9667f;
    if (anim == "SnatchFwd2") return 2.1f;
    if (anim == "SnatchFwd3") return 2.0333f;
    return 2.5333f;
}
constexpr float kCrouchHeight = 122.0f;
// Camera heights above the feet: BaseEyeHeight 76 above the cylinder centre (90) when standing;
// the crouch / slide cameras ride the lowered first-person skeleton.
constexpr float kEyeHeightStand = 166.0f;
constexpr float kEyeHeightCrouch = 108.0f;
constexpr float kEyeHeightSlide = 70.0f;
// UE3 APawn walking constants.
constexpr float kMaxStepHeight = 35.0f;   // Pawn.MaxStepHeight
constexpr float kMaxFloorDist = 2.4f;     // MAXFLOORDIST
constexpr float kWalkableFloorZ = 0.71f;  // TdPawn.WalkableFloorZ
// Floor probes start slightly above the feet so a pawn that ends a move marginally inside a floor
// (float rounding at large world coordinates) still finds it.
constexpr float kFloorProbeLift = 10.0f;
// Distance kept from a blocking surface after a swept move (world units).
constexpr float kContactSkin = 0.1f;
// [TdGame.TdMove_SpeedVault] VaultTypes (DefaultPawnMovement.ini): obstacles up to 48 are stepped,
// 48..148 are vaulted onto / over from the ground, 145..192 need the VaultOverHigh moves started
// from a jump.
constexpr float kVaultMaxHeight = 148.0f;
constexpr float kVaultHighMinHeight = 145.0f;
constexpr float kVaultHighMaxHeight = 192.0f;
// Hands: a ledge is caught (TdMove_IntoGrab) when its top is between the chest and a little above
// the head; the hang then puts the top GrabHangDepth (182.8) above the feet.
constexpr float kGrabMinRise = 100.0f;
constexpr float kGrabMaxRise = 215.0f;
// Ledges this close below the hands are mantled straight over instead of hung from.
constexpr float kMantleMaxRise = 125.0f;
// Camera roll while wallrunning (TdMove_WallRun camera modifier).
constexpr float kWallrunCameraRoll = 15.0f;
// TdLadderVolume (MirrorsEdge.exe GetLastStep 0x12aa0e0, GetLadderLocation 0x12ab3b0): the cooked
// PawnLadderLocations run StepHeight (32) apart up to End.Z - 96 (every Tutorial_p volume). A ladder
// is climbed to its top location, a pipe only to the fourth from the top (LadderSteps.Num() - 4),
// and a pipe's pawn location is ZOffsetPipe (-5) lower. They are cylinder centres, so the feet are
// CollisionHeight (90) under them.
constexpr float kLadderTopStepBelowEnd = 96.0f;
constexpr float kLadderStepHeight = 32.0f;
constexpr float kPipeZOffset = -5.0f;
// The feet on the last step the move climbs to (TdMove_Climb.HandleClimbAction stops there).
float climb_last_step_feet_z(const Vec3& base, const Vec3& top, bool pipe) {
    const float top_step = top.z - kLadderTopStepBelowEnd;
    const float centre = pipe ? top_step - 3.0f * kLadderStepHeight + kPipeZOffset : top_step;
    return std::max(base.z, centre - 0.5f * kPawnHeight);
}
// TdMove_IntoClimb.CanDoMove: a pipe is not taken by a pawn whose centre is above the pipe's top
// location (GetLadderLocation(Num - 1)) - her hands would be over the end of it.
float pipe_highest_catch_feet_z(const Vec3& top) {
    return top.z - kLadderTopStepBelowEnd + kPipeZOffset - 0.5f * kPawnHeight;
}
// TdMove_Barge defaults (Default__TdMove_Barge): BargeAddOnSpeed, BargeMaxSpeed,
// BargeKickThresholdSpeed, BargeMinTraceDistance, BargeTraceTime, BargeAnimTime.
constexpr float kBargeAddOnSpeed = 200.0f;
constexpr float kBargeMaxSpeed = 500.0f;
constexpr float kBargeKickThresholdSpeed = 250.0f;
constexpr float kBargeMinTraceDistance = 90.0f;
constexpr float kBargeTraceTime = 0.5f;
constexpr float kBargeAnimTime = 0.3f;
// The move's custom animations (AS_C1P_Unarmed): BargeInLeft runs at the door, BargeOutLeft plays
// from the impact, MeleeKickObject is the kick (its BargeHitNotify at 0.3272 s opens the door).
// StartBargin keeps move input off for 0.6 s of the kick.
constexpr float kBargeInLeftLength = 0.6f;
constexpr float kBargeOutLeftLength = 1.066667f;
constexpr float kMeleeKickObjectLength = 0.866667f;
constexpr float kBargeKickHitTime = 0.3272f;
constexpr float kBargeKickIgnoreInput = 0.6f;
// TdMove_MeleeSlide (AS_C1P_Unarmed.MeleeSlide): 20 frames at 30 fps (0.6667 s), OnFindBargeTargetTimer at 0.33 s.
constexpr float kMeleeSlideLength = 0.666667f;
constexpr float kMeleeSlideBargeTime = 0.33f;

// Sound notifies on the move's animations. FootDown notifies resolve through the floor's
// TdPhysicalMaterialFootSteps: the walk / run slots of the default material (PM_Concrete), and the
// attack slot (11), which PM_Concrete points at A_Material_Footstep.Wood._11_Female_FootStepAttack.
// The oral / cloth notifies play the player's CharacterSoundCues (A_Character_Female_01).
struct AnimSoundNotify {
    float time;
    const char* cue;
};
constexpr AnimSoundNotify kBargeInLeftNotifies[] = {
    {0.2854f, "Concrete._03_Female_FootStepRun"},
    {0.3695f, "Oral_Impact.Hard"},
    {0.6f, "Concrete._03_Female_FootStepRun"},
};
constexpr AnimSoundNotify kBargeOutLeftNotifies[] = {
    {0.05096f, "Wood._11_Female_FootStepAttack"},
    {0.083f, "Oral_Impact.Medium"},
    {0.1519f, "Concrete._03_Female_FootStepRun"},
    {0.4896f, "Concrete._03_Female_FootStepRun"},
    {0.8298f, "Concrete._03_Female_FootStepRun"},
};
constexpr AnimSoundNotify kMeleeKickObjectNotifies[] = {
    {0.0269f, "Cloth.Run"},
    {0.3104f, "Oral_Strain.Hard"},
    {0.3118f, "Foot_Swoosh"},
    {0.3624f, "Wood._11_Female_FootStepAttack"},
    {0.6071f, "Cloth.Run"},
    {0.7845f, "Concrete._02_Female_FootStepWalk"},
};
constexpr AnimSoundNotify kMeleeSlideNotifies[] = {
    {0.0000f, "Cloth.Run"},
    {0.2513f, "Oral_Strain.Medium"},
    {0.2584f, "Foot_Swoosh"},
    {0.2693f, "Cloth.Run"},
};

// The barge doors' Kismet (SP00 SPT_OnewayDoor_Seq): SeqEvent_TakeDamage turns the doorway slab's
// collision off, plays Door_Barge on the leaf and starts the open matinee; when that completes a
// 3 s Delay starts the close matinee, after which the slab collides again. Both matinees are 0.6 s
// EulerTrack yaw curves (degrees, IMF_RelativeToInitial) with user tangents: the leaf slams open to
// 103.755 deg in 0.25 s, bounces back 5.6 deg and settles; the close track reuses the open track's
// tangents, so the leaf swings ~12.6 deg past shut and bounces twice before it settles. Sound
// tracks: Door_Hit at 0 when opening; hatch.Squek at 0 and Door_Hit at 0.25 when closing.
struct MatineeKey {
    float time;
    float value;
    float arrive;
    float leave;
};
constexpr MatineeKey kDoorOpenTrack[] = {
    {0.0f, 0.0f, 0.0f, -294.565f},
    {0.25f, -103.755f, -798.479f, 32.281f},
    {0.4f, -98.1299f, 11.0229f, 11.0229f},
    {0.6f, -103.755f, 0.0f, 0.0f},
};
constexpr MatineeKey kDoorCloseTrack[] = {
    {0.0f, 0.0f, 0.0f, -294.565f},
    {0.25f, 103.75f, -798.479f, -203.167f},
    {0.4f, 103.75f, 126.24f, 126.24f},
    {0.6f, 103.75f, 0.0f, 0.0f},
};
constexpr float kDoorMatineeLength = 0.6f;
constexpr float kDoorOpenDelay = 3.0f;
constexpr float kDoorCloseHitTime = 0.25f;

// FInterpCurve::Eval with CIM_CurveUser keys: cubic Hermite between keys, tangents x key spacing.
template <size_t N>
float eval_matinee_track(const MatineeKey (&keys)[N], float t) {
    if (t <= keys[0].time) return keys[0].value;
    for (size_t i = 0; i + 1 < N; ++i) {
        if (t < keys[i + 1].time) {
            const float diff = keys[i + 1].time - keys[i].time;
            const float a = (t - keys[i].time) / diff;
            const float a2 = a * a;
            const float a3 = a2 * a;
            return (2.0f * a3 - 3.0f * a2 + 1.0f) * keys[i].value +
                   (a3 - 2.0f * a2 + a) * keys[i].leave * diff +
                   (a3 - a2) * keys[i + 1].arrive * diff +
                   (-2.0f * a3 + 3.0f * a2) * keys[i + 1].value;
        }
    }
    return keys[N - 1].value;
}

// TdPawn.SpeedCurve_LightWeapon: seconds of sprinting -> speed (uu/s), CIM_Linear keys.
const std::vector<std::pair<float, float>> kSpeedCurveLightWeapon = {
    {0.0f, 0.0f}, {0.4f, 400.0f}, {1.0f, 520.0f}, {3.5f, 650.0f}, {7.0f, 720.0f}};

// TdMove_SkillRoll.StartMove zeroes Velocity, turns on root motion and plays AS_C1P_Unarmed
// fallinglandroll (1.2667 s, 38 keys) with bRootMotion; only OnCustomAnimEnd hands back to
// MOVE_Walking. The animation's root bone never rotates and only moves forward (raw +Z): these are
// its 38 keys, 313.5 uu in all. A non-looping sequence spaces its keys Length / (NumKeys - 1) apart.
// The 2026-09-20 14:56 escape recording's roll moves at exactly these keys' rate from its first
// frame (496, 571, 779, 534, 258 ... 234 uu/s), lasts 1.25 s and is in MOVE_Walking at 1.267 s.
constexpr float kSkillRollLength = 1.266667f;
constexpr float kSkillRollRootForward[] = {
    21.3379f,  38.3493f,  57.8220f,  84.1520f,  111.4325f, 129.7298f, 138.6140f, 144.7754f,
    148.7529f, 151.6988f, 153.8096f, 155.2819f, 156.3122f, 157.0971f, 157.8332f, 158.7171f,
    159.9452f, 161.7142f, 164.2207f, 167.6612f, 172.5321f, 179.0229f, 186.8704f, 195.8112f,
    205.5820f, 215.9794f, 226.6942f, 237.3042f, 247.7936f, 258.3770f, 268.9775f, 279.5186f,
    289.9233f, 300.1150f, 309.7264f, 318.5809f, 326.8875f, 334.8549f};

// fallinglandroll's root bone forward offset `t` seconds into the roll (keys interpolated
// linearly, held at either end).
float skill_roll_root_forward(float t) {
    constexpr int n = static_cast<int>(std::size(kSkillRollRootForward));
    const float f = std::clamp(t / kSkillRollLength, 0.0f, 1.0f) * static_cast<float>(n - 1);
    const int i0 = std::min(static_cast<int>(f), n - 2);
    const float a = f - static_cast<float>(i0);
    return kSkillRollRootForward[i0] + (kSkillRollRootForward[i0 + 1] - kSkillRollRootForward[i0]) * a;
}

// TdMove_GrabPullUp (PawnPhysics PHYS_Flying, bDisableCollision) plays the heave with root motion:
// PlayMoveAnim(CNT_FullBody, <heave>, 1.0, 0.1, 0.2, bRootMotion). The pawn rises and goes over
// the lip as the animation's Root bone does, from the first frame: in the five HangHeaveUp
// pull-ups recorded in retail (2026-09-20 to 09-26) the capsule is on these keys to half a unit
// (+70.2 up at 0.409 s, +188.5 at 1.193 s, +67.8 forward at the end), and in the free hang of
// 2026-09-29 21:48 on HangFreeHeaveUp's until collision comes back. AT_C1P's
// IgnoreRootTransformation keeps the first-person mesh, and its EyeJoint, on the pawn, so the
// camera rides these keys. They are AS_C1P_Unarmed's Root track (ACF_None; the bone never turns),
// forward and up of the pawn from the first key, Length / (NumKeys - 1) apart.
// HangHeaveUp: 46 keys over 1.533333 s.
constexpr float kHangHeaveUpRoot[][2] = {
    {0.000f, 0.000f}, {1.098f, 5.927f}, {2.142f, 11.820f}, {3.139f, 17.685f}, {4.096f, 23.528f},
    {5.019f, 29.355f}, {5.917f, 35.172f}, {6.794f, 40.985f}, {7.660f, 46.800f}, {8.519f, 52.623f},
    {9.380f, 58.460f}, {10.249f, 64.317f}, {11.133f, 70.200f}, {12.039f, 76.115f}, {12.975f, 82.068f},
    {13.946f, 88.064f}, {14.959f, 94.111f}, {16.023f, 100.213f}, {17.143f, 106.377f}, {18.326f, 112.608f},
    {19.580f, 118.914f}, {20.908f, 125.423f}, {22.307f, 132.179f}, {23.775f, 139.058f}, {25.308f, 145.935f},
    {26.904f, 152.688f}, {28.560f, 159.192f}, {30.274f, 165.323f}, {32.043f, 170.957f}, {33.863f, 175.971f},
    {35.733f, 180.239f}, {37.650f, 183.640f}, {39.610f, 186.050f}, {41.611f, 187.544f}, {43.651f, 188.290f},
    {45.727f, 188.457f}, {47.835f, 188.216f}, {49.974f, 187.733f}, {52.140f, 187.180f}, {54.331f, 186.724f},
    {56.544f, 186.534f}, {58.776f, 186.534f}, {61.025f, 186.534f}, {63.288f, 186.534f}, {65.562f, 186.534f},
    {67.844f, 186.534f},
};
// HangFreeHeaveUp: 60 keys over 2.000000 s.
constexpr float kHangFreeHeaveUpRoot[][2] = {
    {0.000f, 0.000f}, {-0.043f, 15.059f}, {-0.100f, 31.693f}, {-0.160f, 48.722f}, {-0.213f, 64.962f},
    {-0.249f, 79.233f}, {-0.257f, 90.351f}, {-0.227f, 97.135f}, {-0.255f, 100.803f}, {-0.423f, 103.440f},
    {-0.702f, 105.166f}, {-1.059f, 106.103f}, {-1.463f, 106.372f}, {-1.885f, 106.092f}, {-2.292f, 105.385f},
    {-2.653f, 104.371f}, {-2.938f, 103.171f}, {-3.115f, 101.906f}, {-3.153f, 100.696f}, {-3.021f, 99.662f},
    {-2.688f, 98.925f}, {-2.124f, 98.605f}, {-1.296f, 98.823f}, {-0.175f, 99.701f}, {1.272f, 101.357f},
    {2.963f, 104.261f}, {4.787f, 108.577f}, {6.739f, 113.966f}, {8.808f, 120.093f}, {10.986f, 126.620f},
    {13.264f, 133.210f}, {15.635f, 139.527f}, {18.089f, 145.234f}, {20.619f, 149.994f}, {23.214f, 153.470f},
    {25.868f, 156.140f}, {28.571f, 158.705f}, {31.314f, 161.162f}, {34.090f, 163.508f}, {36.890f, 165.738f},
    {39.704f, 167.849f}, {42.525f, 169.838f}, {45.345f, 171.702f}, {48.153f, 173.436f}, {50.943f, 175.038f},
    {53.705f, 176.504f}, {56.430f, 177.830f}, {59.111f, 179.014f}, {61.739f, 180.050f}, {64.305f, 180.937f},
    {66.800f, 181.671f}, {69.217f, 182.247f}, {71.546f, 182.663f}, {73.779f, 182.915f}, {75.907f, 183.000f},
    {77.347f, 183.173f}, {77.750f, 183.628f}, {77.462f, 184.272f}, {76.828f, 185.010f}, {76.195f, 185.748f},
};
// HangHeaveUpToCrouch: 45 keys over 1.500000 s.
constexpr float kHangHeaveUpToCrouchRoot[][2] = {
    {0.000f, 0.000f}, {1.312f, 8.421f}, {2.635f, 17.687f}, {3.966f, 27.588f}, {5.302f, 37.912f},
    {6.641f, 48.446f}, {7.981f, 58.981f}, {9.317f, 69.305f}, {10.648f, 79.205f}, {11.971f, 88.472f},
    {13.283f, 96.893f}, {14.581f, 104.257f}, {15.862f, 110.352f}, {17.125f, 114.968f}, {18.365f, 117.892f},
    {19.580f, 118.914f}, {20.675f, 118.952f}, {21.601f, 119.053f}, {22.431f, 119.196f}, {23.237f, 119.360f},
    {24.090f, 119.523f}, {25.062f, 119.666f}, {26.226f, 119.767f}, {27.652f, 119.805f}, {29.392f, 121.414f},
    {31.404f, 125.683f}, {33.625f, 131.780f}, {35.990f, 138.871f}, {38.435f, 146.121f}, {40.895f, 152.698f},
    {43.305f, 157.768f}, {45.785f, 161.709f}, {48.450f, 165.434f}, {51.227f, 168.904f}, {54.047f, 172.084f},
    {56.837f, 174.937f}, {59.525f, 177.425f}, {62.042f, 179.512f}, {64.315f, 181.162f}, {66.273f, 182.337f},
    {67.844f, 183.000f}, {68.712f, 183.593f}, {68.820f, 184.436f}, {68.495f, 185.309f}, {68.061f, 185.993f},
};
// hangfreeheaveuptocrouch: 60 keys over 2.000000 s.
constexpr float kHangFreeHeaveUpToCrouchRoot[][2] = {
    {0.000f, 0.000f}, {-0.043f, 15.059f}, {-0.100f, 31.693f}, {-0.160f, 48.722f}, {-0.213f, 64.962f},
    {-0.249f, 79.233f}, {-0.257f, 90.351f}, {-0.227f, 97.135f}, {-0.255f, 100.803f}, {-0.423f, 103.440f},
    {-0.702f, 105.166f}, {-1.059f, 106.103f}, {-1.463f, 106.372f}, {-1.885f, 106.092f}, {-2.292f, 105.385f},
    {-2.653f, 104.371f}, {-2.938f, 103.171f}, {-3.115f, 101.906f}, {-3.153f, 100.696f}, {-3.021f, 99.662f},
    {-2.688f, 98.925f}, {-2.124f, 98.605f}, {-1.296f, 98.823f}, {-0.175f, 99.701f}, {1.272f, 101.357f},
    {2.963f, 104.261f}, {4.787f, 108.577f}, {6.739f, 113.966f}, {8.808f, 120.093f}, {10.986f, 126.620f},
    {13.264f, 133.210f}, {15.635f, 139.527f}, {18.089f, 145.234f}, {20.619f, 149.994f}, {23.214f, 153.470f},
    {25.868f, 156.140f}, {28.571f, 158.705f}, {31.314f, 161.162f}, {34.090f, 163.508f}, {36.890f, 165.738f},
    {39.704f, 167.849f}, {42.525f, 169.838f}, {45.345f, 171.702f}, {48.153f, 173.436f}, {50.943f, 175.038f},
    {53.705f, 176.504f}, {56.430f, 177.830f}, {59.111f, 179.014f}, {61.739f, 180.050f}, {64.305f, 180.937f},
    {66.800f, 181.671f}, {69.217f, 182.247f}, {71.546f, 182.663f}, {73.779f, 182.915f}, {75.907f, 183.000f},
    {77.347f, 183.204f}, {77.750f, 183.741f}, {77.462f, 184.500f}, {76.828f, 185.370f}, {76.195f, 186.241f},
};

struct HeaveRootMotion {
    const char* anim;        // the sequence, as the move names it to the animation (move_anim)
    float length;            // s: the move ends with it (OnCustomAnimEnd)
    const float (*keys)[2];  // Root: forward, up
    int count;
    bool crouch;             // GPUT_IntoCrouch: no room to stand on top
};
// Indexed by ParkourController::m_pullup_heave: bit 0 hanging free, bit 1 crouched on top.
constexpr HeaveRootMotion kHeaveRootMotion[] = {
    {"HangHeaveUp", 1.533333f, kHangHeaveUpRoot, static_cast<int>(std::size(kHangHeaveUpRoot)), false},
    {"HangFreeHeaveUp", 2.0f, kHangFreeHeaveUpRoot, static_cast<int>(std::size(kHangFreeHeaveUpRoot)), false},
    {"HangHeaveUpToCrouch", 1.5f, kHangHeaveUpToCrouchRoot, static_cast<int>(std::size(kHangHeaveUpToCrouchRoot)), true},
    {"hangfreeheaveuptocrouch", 2.0f, kHangFreeHeaveUpToCrouchRoot, static_cast<int>(std::size(kHangFreeHeaveUpToCrouchRoot)), true},
};

// Where the heave's Root bone is `t` seconds in, forward and up of the pawn (keys interpolated
// linearly, held at either end).
void heave_root_motion(const HeaveRootMotion& h, float t, float& forward, float& up) {
    const float f = std::clamp(t / h.length, 0.0f, 1.0f) * static_cast<float>(h.count - 1);
    const int i0 = std::min(static_cast<int>(f), h.count - 2);
    const float a = f - static_cast<float>(i0);
    forward = h.keys[i0][0] + (h.keys[i0 + 1][0] - h.keys[i0][0]) * a;
    up = h.keys[i0][1] + (h.keys[i0 + 1][1] - h.keys[i0][1]) * a;
}

// TdMove_GrabPullUp.StartMove's timers for these heaves (TdGame.u bytecode): ReleaseCamera after
// 0.8 s (the body follows the view again: retail's turns from 0.817 s in 2026-09-26 10:21) and
// EnableCollision after 1.4 s (bCollideWorld: the step-ups recorded at exactly 1.40 s).
constexpr float kPullUpReleaseCamera = 0.8f;
constexpr float kPullUpEnableCollision = 1.4f;

// Fraction of a move of length `move_len` that stops kContactSkin short of the contact.
float safe_fraction(float fraction, float move_len) {
    if (move_len <= 1e-6f) return 0.0f;
    return std::clamp(fraction - kContactSkin / move_len, 0.0f, 1.0f);
}

// ATdPawn builds AccelCurve_LightWeapon from the speed curve on load (called with 10 steps):
// sample the speed curve at keys x 10 + 1 even times to get time-at-speed, then at 11 speeds from 0
// to the top one take the speed gained over the next 0.5 / 10 s. Sprinting straight then follows
// the designer's speed curve exactly.
LinearCurve accel_curve_from(const std::vector<std::pair<float, float>>& speed, int steps) {
    LinearCurve s{speed};
    const float t_last = speed.back().first;
    const float v_last = speed.back().second;
    const int samples = static_cast<int>(speed.size()) * 10;
    LinearCurve inverse;
    for (int k = 0; k <= samples; ++k) {
        const float t = static_cast<float>(k) * t_last / static_cast<float>(samples);
        inverse.points.emplace_back(s.eval(t), t);
    }
    const float dt = 0.5f / static_cast<float>(steps);
    LinearCurve out;
    for (int i = 0; i <= steps; ++i) {
        const float v = static_cast<float>(i) * v_last / static_cast<float>(steps);
        const float t = inverse.eval(v);
        out.points.emplace_back(v, (s.eval(t + dt) - s.eval(t)) / dt);
    }
    return out;
}

// The game rounds requested accelerations to 0.1 uu/s^2 (floor(x * 10 + 0.5) / 10).
Vec3 round_accel(const Vec3& a) {
    auto r = [](float x) { return std::floor(x * 10.0f + 0.5f) / 10.0f; };
    return Vec3(r(a.x), r(a.y), r(a.z));
}

Vec3 horiz(const Vec3& v) { return Vec3(v.x, v.y, 0.0f); }

float wrap_deg(float a) {
    while (a < -180.0f) a += 360.0f;
    while (a >= 180.0f) a -= 360.0f;
    return a;
}

float yaw_of(const Vec3& v) { return std::atan2(v.y, v.x) * RAD2DEG; }

float sign_of(float v) { return v < 0.0f ? -1.0f : 1.0f; }

bool is_air_move(EMovement m) {
    switch (m) {
        case EMovement::MOVE_Jump:
        case EMovement::MOVE_Falling:
        case EMovement::MOVE_Coil:
        case EMovement::MOVE_WallRunJump:
        case EMovement::MOVE_GrabJump:
        case EMovement::MOVE_WallClimb180TurnJump:
        case EMovement::MOVE_DodgeJump:
        case EMovement::MOVE_180TurnInAir:
        case EMovement::MOVE_MeleeAir:
        case EMovement::MOVE_MeleeWallrun:
            return true;
        default:
            return false;
    }
}

// Moves the coil may start from (TdMove_Coil.CanDoMove): jumps, not plain falls or dodges.
bool coil_allowed_from(EMovement m) {
    switch (m) {
        case EMovement::MOVE_Jump:
        case EMovement::MOVE_WallRunJump:
        case EMovement::MOVE_GrabJump:
        case EMovement::MOVE_WallClimb180TurnJump:
        case EMovement::MOVE_SpringBoarding:
            return true;
        default:
            return false;
    }
}

// TdMove_ZipLine / TdMove_IntoZipLine / TdZiplineVolume (TdGame.u defaults) and the native move
// (MirrorsEdge.exe 0x1209400: TdMove_ZipLine's per-tick physics, after which the pawn runs
// PHYS_Flying). HangOffset (0, 0, -90) holds the pawn's centre 90 below the cable, so the bottom of
// its 180-tall cylinder - the feet - rides 180 below it.
constexpr float kZipHangHeight = 180.0f;
constexpr float kZipMinVelocity = 300.0f;      // TdMove_ZipLine.MinZipVelocity
constexpr float kZipMinAcceleration = 400.0f;  // TdMove_ZipLine.MinZipAcceleration
// PHYS_Flying's friction on the ride. calcVelocity applies half the PhysicsVolume's FluidFriction
// (0.3); 0.163 reproduces the retail Edge_p ride (recordings/20261002_102817_edge_pt1.jsonl.gz,
// t 111.47-115.03: 446 -> 1783 uu/s, the simulated path within 31 uu of the recorded one).
constexpr float kZipFlyingFriction = 0.163f;
// The native move's trace ahead of the hands, by ZipLineStatus: 600 (Moving: PrepareForForwardImpact
// at a hit), 20 (CloseToEnd: PlayForwardImpact), 2 (Impact: the pawn is held still). It sweeps the
// pawn's extent with half its height, centred 40 below the cable; CurrentLookAtPoint is its far end.
constexpr float kZipTraceDrop = 40.0f;
constexpr float kZipTraceHalfHeight = 45.0f;
constexpr float kZipTraceReach[3] = {600.0f, 20.0f, 2.0f};
constexpr float kZipImpactTime = 0.8f;         // PlayForwardImpact: SetTimer / SetIgnoreLookInput(0.8)
constexpr float kZipLandingStrip = 500.0f;     // TdZiplineVolume.LandingStrip
constexpr float kZipRedoTime = 0.5f;           // TdMove_IntoZipLine.RedoMoveTime
constexpr float kZipSameLineRedoTime = 3.0f;   // TdMove_IntoZipLine.SameZipLineRedoMoveTime
constexpr float kZipFallLimitZ = -600.0f;      // TdMove_IntoZipLine.ZVelocityFallLimit
// Retail catches the cable anywhere inside the TdZiplineVolume brush wrapped around it; the port
// catches it when the hands come this close.
constexpr float kZipGrabReach = 135.0f;

// TdZiplineVolume.FindClosestPointOnDSpline: the point of the cable polyline closest to `p`, and its
// parameter (segment index + fraction along that segment, 0 .. N-1).
Vec3 closest_on_cable(const std::vector<Vec3>& pts, const Vec3& p, float& param) {
    param = 0.0f;
    if (pts.empty()) return p;
    Vec3 best_point = pts.front();
    float best = (p - best_point).length_sq();
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        const Vec3 d = pts[i + 1] - pts[i];
        const float l2 = d.length_sq();
        const float s = l2 > 1e-6f ? std::clamp((p - pts[i]).dot(d) / l2, 0.0f, 1.0f) : 0.0f;
        const Vec3 c = pts[i] + d * s;
        const float dist = (p - c).length_sq();
        if (dist < best) {
            best = dist;
            best_point = c;
            param = static_cast<float>(i) + s;
        }
    }
    return best_point;
}

// TdZiplineVolume.GetSlopeOnSpline: the direction of the cable at `param` (its segment's).
Vec3 cable_slope(const std::vector<Vec3>& pts, float param) {
    if (pts.size() < 2) return Vec3(1.0f, 0.0f, 0.0f);
    const size_t i = std::min(pts.size() - 2, static_cast<size_t>(std::max(0.0f, param)));
    const Vec3 d = (pts[i + 1] - pts[i]).normalized();
    return d.length_sq() > 0.5f ? d : (pts.back() - pts.front()).normalized();
}

// -----------------------------------------------------------------------------
// Per-move camera rules: the TdMove_* class defaults in TdGame.u.
//   bConstrainLook + MinLookConstraint / MaxLookConstraint: how far the view may pitch / yaw away
//     from the body (TdPawn.Rotation), in rotation units (65536 = 360 deg);
//   bDisableFaceRotation: the body stops turning with the view;
//   DisableLookTime: TdMove.StartMove -> SetIgnoreLookInput (0 none, > 0 seconds, -1 until the
//     move ends).
// The port's MOVE_SoftLanding / MOVE_StepUp / MOVE_AutoStepUp are walking substates, not retail's
// TdMove_SoftLanding / TdMove_StepUp moves, so they keep Walking's free look. Vaults lock look
// only for their VaultTimeUp (high vaults), set when the vault starts.
// -----------------------------------------------------------------------------
struct MoveCamera {
    bool constrain = false;
    float pitch_min = 0.0f;
    float pitch_max = 0.0f;
    float yaw_min = 0.0f;
    float yaw_max = 0.0f;
    bool disable_face_rotation = false;
    float disable_look_time = 0.0f;
};

MoveCamera move_camera(EMovement m) {
    auto lim = [](float pmin, float pmax, float ymin, float ymax, bool dfr, float dlt = 0.0f) {
        return MoveCamera{true, pmin, pmax, ymin, ymax, dfr, dlt};
    };
    auto free_look = [](bool dfr, float dlt) {
        MoveCamera r;
        r.disable_face_rotation = dfr;
        r.disable_look_time = dlt;
        return r;
    };
    switch (m) {
        case EMovement::MOVE_Grabbing:         return lim(-3200, 16000, -32768, 32768, true, 0.8f);
        case EMovement::MOVE_WallRunningRight:
        case EMovement::MOVE_WallRunningLeft:  return lim(-13000, 13000, 0, 0, true);  // yaw: world window
        case EMovement::MOVE_SpeedVaulting:
        case EMovement::MOVE_VaultOver:        return lim(-3000, 6000, -8000, 8000, true);
        case EMovement::MOVE_GrabPullUp:       return lim(0, 16384, -10000, 10000, true, 0.2f);
        case EMovement::MOVE_GrabJump:         return free_look(true, 0.0f);
        case EMovement::MOVE_Crouch:           return lim(-14000, 14000, -32768, 32768, false);
        case EMovement::MOVE_Slide:            return lim(-10000, 10000, -10000, 10000, true);
        case EMovement::MOVE_Melee:            return lim(-10000, 10000, -32768, 32768, false);
        case EMovement::MOVE_Snatch:           return free_look(false, -1.0f);  // TdMove_Disarm
        case EMovement::MOVE_Barge:            return lim(-14000, 16384, -5000, 5000, true);
        case EMovement::MOVE_Climb:            return lim(-5000, 10000, -32000, 32000, true);
        case EMovement::MOVE_180Turn:          return lim(-10000, 10000, -16384, 16384, false);
        case EMovement::MOVE_180TurnInAir:     return lim(0, 32768, -5000, 5000, true);
        case EMovement::MOVE_LayOnGround:      return lim(-2000, 32768, -5000, 5000, true);
        case EMovement::MOVE_ZipLine:          return lim(-3200, 32768, -7000, 7000, true);
        case EMovement::MOVE_Balance:          return lim(-13000, 25000, -6000, 6000, true);
        case EMovement::MOVE_LedgeWalk:        return lim(-14000, 16384, -10000, 10000, true);
        case EMovement::MOVE_GrabTransfer:     return free_look(true, -1.0f);
        case EMovement::MOVE_RumpSlide:        return lim(-5000, 5000, -5000, 5000, true);
        case EMovement::MOVE_MeleeSlide:       return free_look(false, -1.0f);
        case EMovement::MOVE_Swing:            return free_look(false, -1.0f);
        case EMovement::MOVE_Coil:             return lim(-5000, 30000, -32768, 32768, false);
        case EMovement::MOVE_MeleeWallrun:     return lim(-3000, 16000, -8000, 8000, false);
        case EMovement::MOVE_SkillRoll:        return lim(-2000, 32768, -5000, 5000, true);
        case EMovement::MOVE_SoftLanding:      return lim(-16384, 16384, -5000, 5000, true);
        default:                               return MoveCamera{};
    }
}

// TdMove.ConstrainAxis (rotation units, truncating where the script converts to int). The frame's
// look delta `d` may not carry `angle` (the view relative to the body) outside [lo, hi]; it slows
// over the last 40% of either side; and a view that starts outside (the move just began, or the
// body turned under it) is pulled back with Speed = dt / 0.2, a 0.2 s time constant.
float constrain_axis(float angle, float lo, float hi, float speed, float d) {
    if (angle < lo) return std::max(std::trunc((lo - angle) * speed), d);
    if (angle > hi) return std::min(std::trunc((hi - angle) * speed), d);
    float t = angle + d;
    if (t < lo) d -= t - lo;
    else if (t > hi) d -= t - hi;
    t = angle + d;
    if (t < lo * 0.6f && lo != 0.0f) d = std::max(d, std::trunc(d * std::abs((lo - angle) / (lo * 0.4f))));
    else if (t > hi * 0.6f && hi != 0.0f) d = std::min(d, std::trunc(d * std::abs((hi - angle) / (hi * 0.4f))));
    return d;
}

constexpr float kUUPerDeg = 65536.0f / 360.0f;
}  // namespace

float LinearCurve::eval(float x) const {
    const auto& p = points;
    if (p.empty()) return 0.0f;
    if (p.size() == 1 || x <= p.front().first) return p.front().second;
    if (x >= p.back().first) return p.back().second;
    for (size_t i = 0; i + 1 < p.size(); ++i) {
        if (x < p[i + 1].first) {
            const float d = p[i + 1].first - p[i].first;
            if (d <= 0.0f) return p[i].second;
            return p[i].second + (p[i + 1].second - p[i].second) * (x - p[i].first) / d;
        }
    }
    return p.back().second;
}

ParkourController::ParkourController(const MovementConfig& config)
    : m_config(config), m_accel_curve(accel_curve_from(kSpeedCurveLightWeapon, 10)) {
    reset(Vec3(0.0f, 0.0f, 100.0f), 0.0f);
}

void ParkourController::reset(const Vec3& spawn_pos, float spawn_yaw) {
    m_telemetry.tick = 0;
    m_telemetry.sim_time = 0.0f;
    m_telemetry.position = spawn_pos;
    m_telemetry.velocity = Vec3(0.0f, 0.0f, 0.0f);
    m_telemetry.speed_2d = 0.0f;
    m_telemetry.speed_3d = 0.0f;
    m_telemetry.yaw_deg = spawn_yaw;
    m_telemetry.pitch_deg = 0.0f;
    m_telemetry.camera_roll_deg = 0.0f;
    m_telemetry.fov_deg = 90.0f;
    m_telemetry.eye_height = kEyeHeightStand;
    m_telemetry.health = 100.0f;
    m_telemetry.reaction_energy = 100.0f;
    m_telemetry.reaction_active = false;
    m_telemetry.grounded = true;
    m_telemetry.move_state = EMovement::MOVE_Walking;
    m_telemetry.wall_normal = Vec3(0.0f, 0.0f, 0.0f);
    m_telemetry.active_checkpoint = 0;
    m_telemetry.active_checkpoint_name = "";
    m_telemetry.bags_collected = 0;
    m_telemetry.in_elevator = false;
    m_telemetry.active_elevator_idx = -1;
    m_telemetry.elevator_progress = 0.0f;
    m_telemetry.weapon = WeaponState{};
    m_telemetry.combat_anim_time = 0.0f;
    m_telemetry.combat_anim_duration = 0.45f;
    m_telemetry.melee_variant = 0;
    m_telemetry.snatch_from_back = false;
    m_cam_mesh_offset = Vec3(0.0f, 0.0f, 0.0f);
    m_telemetry.camera_mesh_offset = m_cam_mesh_offset;
    m_mesh_smooth_z = 0.0f;
    m_smooth_was_walking = false;
    m_smooth_was_heave = false;
    m_cam_constrain_look = false;
    m_snatch_align = false;
    m_against_wall = 0;
    m_against_wall_off = 0.0f;
    m_telemetry.against_wall = 0;
    m_telemetry.melee_hit_confirmed = false;
    m_telemetry.disarm_prompt_visible = false;
    m_telemetry.hit_marker_timer = 0.0f;
    m_telemetry.damage_flash_timer = 0.0f;
    m_telemetry.health_desat = 0.0f;
    m_telemetry.hit_blur = 0.0f;
    m_telemetry.hit_focus_distance = 1600.0f;
    m_telemetry.melee_damage_strength = 0.0f;
    m_telemetry.melee_hit_dir = 0.0f;
    m_telemetry.fall_damage_strength = 0.0f;
    for (int i = 0; i < 4; ++i) {
        m_telemetry.bullet_hit_angles[i] = 0.0f;
        m_telemetry.bullet_hit_timers[i] = 0.0f;
        m_telemetry.bullet_hit_seeds[i] = 0;
    }
    m_telemetry.camera_anim.clear();
    m_telemetry.falling_to_death = false;
    m_telemetry.fall_death_impact = false;
    m_telemetry.death_anim_progress = 0.0f;
    m_telemetry.active_subtitle = "";

    m_sprint_energy = 0.0f;
    m_accel_time = 0.0f;
    m_stop_timer = 0.0f;
    m_frame_turn_uu = 0.0f;
    m_frame_dt = 1.0f / 60.0f;
    m_frame_accel = Vec3(0.0f, 0.0f, 0.0f);
    m_frame_accel_valid = false;

    m_prev_jump = false;
    m_prev_crouch = false;
    m_prev_turn_180 = false;
    m_crouch_pressed = false;
    m_jump_buffer = 0.0f;
    m_roll_trigger_time = -100.0f;

    m_state_timer = 0.0f;
    m_wallrun_cooldown = 0.0f;
    m_wallrun_begin_speed = 0.0f;
    m_slide_timer = 0.0f;
    m_slide_yaw = spawn_yaw;
    m_coil_timer = 0.0f;
    m_turn_timer = 0.0f;
    m_turn_total = 0.0f;
    m_turn_target_yaw = spawn_yaw;
    m_landing_timer = 0.0f;
    m_roll_dir = Rotator::from_degrees(0.0f, spawn_yaw, 0.0f).forward();
    m_damage_cooldown = 0.0f;
    m_hit_spazz_cooldown = 0.0f;
    m_bullet_hit_counter = 0;
    m_env_health_desat.reset();
    m_env_blur.reset();
    m_env_melee.reset();
    m_env_fall.reset();
    m_hazard_shock_cooldown = 0.0f;
    m_air_fall_start_z = spawn_pos.z;
    m_fall_peak_z = spawn_pos.z;
    m_melee_cooldown = 0.0f;
    m_melee_combo_index = 0;
    m_melee_combo_reset_timer = 0.0f;
    m_weapon_cycle_index = -1;
    m_jump_consumed = false;
    m_barge_kick = false;
    m_barge_door = -1;
    m_barge_anim = 0;
    m_barge_anim_pos = 0.0f;
    m_barge_anim_elapsed = 0.0f;
    m_barge_anim_rate = 1.0f;
    m_barge_speed = 0.0f;
    m_barge_precise = false;
    m_barge_dealt_damage = false;
    m_ignore_move_input = 0.0f;
    m_walk_blocked = false;
    m_walk_block_actor = -1;
    m_telemetry.barge_anim = 0;
    m_telemetry.barge_anim_pos = 0.0f;
    m_telemetry.barge_anim_weight = 0.0f;
    m_telemetry.sound_events.clear();

    m_last_jump_location = spawn_pos;
    m_pre_jump_momentum = 0.0f;
    m_takeoff_move = EMovement::MOVE_Falling;
    m_turned_in_air = false;
    m_consecutive_wallruns = 0;
    m_wall_turned = false;
    m_illegal_wall_timer = 0.0f;

    m_wall_tangent = Vec3(0.0f, 0.0f, 0.0f);
    m_last_wallrun_normal = Vec3(0.0f, 0.0f, 0.0f);
    m_into_wallclimb_speed = 0.0f;
    m_wallclimb_reached = false;
    m_zip_points.clear();
    m_zip_param = 0.0f;
    m_zip_status = 0;
    m_zip_impact_timer = 0.0f;
    m_zip_body_yaw = spawn_yaw;
    m_zip_look_at = Vec3(0.0f, 0.0f, 0.0f);
    m_zip_look_assist = false;
    m_zip_last_actor = -1;
    m_zip_last_stop_time = -100.0f;
    m_zip_exit_z = 1e30f;
    m_zipline_cooldown = 0.0f;
    m_climb_base = Vec3(0.0f, 0.0f, 0.0f);
    m_climb_top = Vec3(0.0f, 0.0f, 0.0f);
    m_climb_normal = Vec3(0.0f, 0.0f, 0.0f);
    m_climb_cooldown = 0.0f;
    m_climb_can_exit_top = true;
    m_climb_is_pipe = false;
    m_climb_settle_speed = 0.0f;
    m_balance_start = Vec3(0.0f, 0.0f, 0.0f);
    m_balance_end = Vec3(0.0f, 0.0f, 0.0f);
    m_balance_lean = 0.0f;
    m_balance_cooldown = 0.0f;
    m_ledge_walk_start = Vec3(0.0f, 0.0f, 0.0f);
    m_ledge_walk_end = Vec3(0.0f, 0.0f, 0.0f);
    m_ledge_walk_normal = Vec3(0.0f, 0.0f, 0.0f);
    m_ledge_walk_cooldown = 0.0f;
    m_swing_anchor = Vec3(0.0f, 0.0f, 0.0f);
    m_swing_bar_start = Vec3(0.0f, 0.0f, 0.0f);
    m_swing_bar_end = Vec3(0.0f, 0.0f, 0.0f);
    m_swing_dir = Vec3(1.0f, 0.0f, 0.0f);
    m_swing_angle = 0.0f;
    m_swing_angular_vel = 0.0f;
    m_swing_cooldown = 0.0f;
    m_ledge_z = 0.0f;
    m_hang_time = 0.0f;
    m_base_actor = -1;

    m_pawn_yaw = spawn_yaw;
    m_cam_move = EMovement::MOVE_Walking;
    m_cam_move_time = 0.0f;
    m_face_rotation_disabled = false;
    m_face_rotation_time_left = 0.0f;
    m_ignore_look_time = 0.0f;
    m_reset_look_time = -1.0f;
    m_look_at_active = false;
    m_wallrun_yaw_min = 0.0f;
    m_wallrun_yaw_max = 0.0f;
    m_vault_look_lock = 0.0f;
    m_vault_down = false;

    m_last_checkpoint_pos = spawn_pos;
    m_last_checkpoint_yaw = spawn_yaw;
    m_respawn_pos = spawn_pos;
    m_respawn_yaw = spawn_yaw;
    m_death_timer = 0.0f;
    m_death_total_duration = 1.35f;
}

// SeqAct_TdCheckpoint, through the level script: the checkpoint the player respawns at and is
// counted as having reached. Replaces the proximity test of update_checkpoints_and_volumes.
void ParkourController::set_checkpoint(const Vec3& feet, float yaw_deg, int index, const std::string& name) {
    m_last_checkpoint_pos = feet;
    m_last_checkpoint_yaw = yaw_deg;
    m_respawn_pos = feet;
    m_respawn_yaw = yaw_deg;
    m_telemetry.active_checkpoint = index;
    m_telemetry.active_checkpoint_name = name;
}

void ParkourController::anchor(const Vec3& feet, const Vec3& velocity, float yaw, float pitch,
                               bool grounded, const AnchorState& state) {
    reset(feet, yaw);
    m_telemetry.velocity = velocity;
    m_telemetry.speed_2d = velocity.length_xy();
    m_telemetry.speed_3d = velocity.length();
    m_telemetry.pitch_deg = std::clamp(pitch, -85.0f, 85.0f);
    m_sprint_energy = state.sprint_energy;
    m_accel_time = state.accel_time;
    m_stop_timer = state.stop_left;
    m_prev_jump = state.held_jump;
    m_prev_crouch = state.held_crouch;
    if (!grounded) {
        leave_ground(state.jump ? EMovement::MOVE_Jump : EMovement::MOVE_Falling);
        if (state.jump == 1) m_telemetry.move_state = EMovement::MOVE_Falling;
    }
    if (state.jump) m_pre_jump_momentum = state.pre_jump_momentum;
}

void ParkourController::equip_weapon(const std::string& weapon_name) {
    struct WeaponSpec {
        const char* id;
        const char* display;
        EWeaponFireMode fire_mode;
        int max_ammo;
        float fire_interval;
        float damage_near;
        float damage_far;
        float falloff_start;
        float falloff_end;
        float max_range;
        int pellet_count;
        float spread_rad;
        float recoil_pitch;
        bool is_heavy;
        bool is_two_handed;
        float mobility_scale;
    };

    // Reverse-engineered from TdGame/Config/DefaultWeapons.ini & TdSharedContent.u
    static const WeaponSpec kWeaponSpecs[] = {
        {"Colt1911",     "M1911 .45 Pistol",        EWeaponFireMode::SemiAuto,   8,   0.22f, 45.0f, 25.0f, 1200.0f, 3000.0f, 10000.0f, 1,  0.018f, 1.8f, false, false, 0.92f},
        {"Glock18",      "G18C Machine Pistol",     EWeaponFireMode::FullAuto,   19,  0.08f, 22.0f, 10.0f,  800.0f, 2200.0f,  8000.0f, 1,  0.045f, 1.0f, false, false, 0.92f},
        {"BerettaM93R",  "93R Burst Pistol",        EWeaponFireMode::Burst3,     20,  0.085f,30.0f, 15.0f, 1000.0f, 2500.0f,  9000.0f, 1,  0.028f, 1.1f, false, false, 0.92f},
        {"SteyrTMP",     "Steyr TMP Tactical SMG",  EWeaponFireMode::FullAuto,   30,  0.066f,20.0f,  9.0f,  800.0f, 2200.0f,  8000.0f, 1,  0.048f, 0.85f,false, false, 0.90f},
        {"MP5K",         "HK MP5K Submachine Gun",  EWeaponFireMode::FullAuto,   30,  0.075f,26.0f, 13.0f, 1000.0f, 2800.0f,  9000.0f, 1,  0.035f, 1.0f, false, true,  0.86f},
        {"G36C",         "HK G36C Assault Rifle",   EWeaponFireMode::FullAuto,   30,  0.08f, 36.0f, 18.0f, 1800.0f, 4500.0f, 12000.0f, 1,  0.024f, 1.3f, true,  true,  0.72f},
        {"FNSCARL",      "FN SCAR-L Carbine",       EWeaponFireMode::FullAuto,   20,  0.096f,42.0f, 24.0f, 2000.0f, 5000.0f, 13000.0f, 1,  0.020f, 1.55f,true,  true,  0.70f},
        {"Remington870", "Remington 870 Shotgun",   EWeaponFireMode::PumpAction, 7,   0.82f, 18.0f,  4.0f,  400.0f, 1500.0f,  3000.0f, 10, 0.092f, 4.0f, true,  true,  0.72f},
        {"Neostead",     "Neostead 2000 Shotgun",   EWeaponFireMode::PumpAction, 12,  0.62f, 16.0f,  4.0f,  450.0f, 1600.0f,  3200.0f, 10, 0.082f, 3.4f, true,  true,  0.72f},
        {"FNMinimi",     "M249 SAW Squad LMG",      EWeaponFireMode::FullAuto,   100, 0.075f,38.0f, 20.0f, 2000.0f, 5000.0f, 13000.0f, 1,  0.046f, 1.4f, true,  true,  0.62f},
        {"M95",          "Barrett M95 .50 BMG",     EWeaponFireMode::BoltAction, 5,   1.25f, 160.0f,120.0f,4000.0f, 9000.0f, 15000.0f, 1,  0.004f, 6.0f, true,  true,  0.58f}
    };

    const WeaponSpec* matched = &kWeaponSpecs[0];
    int matched_idx = 0;
    for (int i = 0; i < static_cast<int>(sizeof(kWeaponSpecs) / sizeof(kWeaponSpecs[0])); ++i) {
        if (weapon_name.find(kWeaponSpecs[i].id) != std::string::npos) {
            matched = &kWeaponSpecs[i];
            matched_idx = i;
            break;
        }
    }
    // Additional substring aliases
    if (weapon_name.find("Glock") != std::string::npos) { matched = &kWeaponSpecs[1]; matched_idx = 1; }
    else if (weapon_name.find("Beretta") != std::string::npos || weapon_name.find("93R") != std::string::npos) { matched = &kWeaponSpecs[2]; matched_idx = 2; }
    else if (weapon_name.find("TMP") != std::string::npos || weapon_name.find("Steyr") != std::string::npos) { matched = &kWeaponSpecs[3]; matched_idx = 3; }
    else if (weapon_name.find("SCAR") != std::string::npos) { matched = &kWeaponSpecs[6]; matched_idx = 6; }
    else if (weapon_name.find("Remington") != std::string::npos || weapon_name.find("870") != std::string::npos) { matched = &kWeaponSpecs[7]; matched_idx = 7; }
    else if (weapon_name.find("Minimi") != std::string::npos || weapon_name.find("M249") != std::string::npos) { matched = &kWeaponSpecs[9]; matched_idx = 9; }
    else if (weapon_name.find("Barret") != std::string::npos || weapon_name.find("M95") != std::string::npos) { matched = &kWeaponSpecs[10]; matched_idx = 10; }

    m_weapon_cycle_index = matched_idx;

    WeaponState ws{};
    ws.equipped = true;
    ws.name = matched->id;
    ws.display_name = matched->display;
    ws.fire_mode = matched->fire_mode;
    ws.ammo = matched->max_ammo;
    ws.max_ammo = matched->max_ammo;
    ws.cooldown = 0.15f;
    ws.fire_interval = matched->fire_interval;
    ws.range = matched->max_range;
    ws.falloff_start = matched->falloff_start;
    ws.falloff_end = matched->falloff_end;
    ws.damage = matched->damage_near;
    ws.damage_far = matched->damage_far;
    ws.pellet_count = matched->pellet_count;
    ws.spread_rad = matched->spread_rad;
    ws.recoil_pitch_deg = matched->recoil_pitch;
    ws.is_heavy = matched->is_heavy;
    ws.is_two_handed = matched->is_two_handed;
    ws.mobility_scale = matched->mobility_scale;
    if (matched->fire_mode == EWeaponFireMode::BoltAction) {
        ws.fire_anim_duration = 1.45f;
    } else if (matched->fire_mode == EWeaponFireMode::PumpAction) {
        ws.fire_anim_duration = (std::string(matched->id) == "Remington870") ? 1.05f : 0.88f;
    } else if (matched->fire_mode == EWeaponFireMode::SemiAuto) {
        ws.fire_anim_duration = 0.52f;
    } else {
        ws.fire_anim_duration = 0.48f;
    }
    ws.equip_timer = matched->is_heavy ? 0.65f : 0.45f; // trigger 1P unholster animation
    m_telemetry.weapon = ws;
}

void ParkourController::step(const InputFrame& input, float dt, LevelScene& scene) {
    m_telemetry.sound_events.clear();
    if (dt <= 0.0f) return;
    m_telemetry.move_input = std::fabs(input.forward) > 0.01f || std::fabs(input.strafe) > 0.01f;
    if (m_telemetry.grounded) m_telemetry.ground_distance = -1.0f;

    // Input edges. TdPlayerInput: Jump and Crouch are press actions (holding a key never
    // retriggers a move); a jump press is buffered for JumpTapTime.
    const bool jump_press = input.jump && !m_prev_jump;
    m_crouch_pressed = input.crouch && !m_prev_crouch;
    m_prev_jump = input.jump;
    m_prev_crouch = input.crouch;
    m_jump_consumed = input.jump && !jump_press;
    if (jump_press) m_jump_buffer = m_config.jump_tap_time;
    // TdPawn.Tick: a crouch press arms the skill roll (RollTriggerTime) unless a press armed it
    // within the last 0.6 s (so holding / spamming crouch does not keep it armed).
    if (m_crouch_pressed && m_telemetry.sim_time - m_roll_trigger_time >= m_config.roll_trigger_rearm) {
        m_roll_trigger_time = m_telemetry.sim_time;
    }
    // PlayerInput.aTurn this frame, in rotation units (65536 per turn): sprint turn damping.
    m_frame_turn_uu = std::abs(input.look_yaw_delta) * (65536.0f / 360.0f);

    // 1. Reaction Time Slow-Motion
    float effective_dt = dt;
    update_reaction_time(input, dt, effective_dt);
    m_frame_dt = effective_dt;
    m_frame_accel_valid = false;  // PlayerMove runs again this frame

    // 2. Camera, Rotation & Look-At Hint
    update_camera_and_inputs(input, effective_dt, scene);

    // 3. Substepped Kinematic Physics (fixed ~120Hz substepping for precision)
    constexpr float MAX_SUBSTEP = 1.0f / 120.0f;
    float remaining_time = effective_dt;

    while (remaining_time > 1e-6f) {
        float step_dt = std::min(remaining_time, MAX_SUBSTEP);
        remaining_time -= step_dt;

        m_state_timer += step_dt;
        m_telemetry.combat_anim_time = m_state_timer;
        m_wallrun_cooldown = std::max(0.0f, m_wallrun_cooldown - step_dt);
        m_zipline_cooldown = std::max(0.0f, m_zipline_cooldown - step_dt);
        m_climb_cooldown = std::max(0.0f, m_climb_cooldown - step_dt);
        m_balance_cooldown = std::max(0.0f, m_balance_cooldown - step_dt);
        m_ledge_walk_cooldown = std::max(0.0f, m_ledge_walk_cooldown - step_dt);
        m_swing_cooldown = std::max(0.0f, m_swing_cooldown - step_dt);
        m_ignore_move_input = std::max(0.0f, m_ignore_move_input - step_dt);
        m_hazard_shock_cooldown = std::max(0.0f, m_hazard_shock_cooldown - step_dt);
        if (m_illegal_wall_timer > 0.0f) {
            m_illegal_wall_timer = std::max(0.0f, m_illegal_wall_timer - step_dt);
            if (m_illegal_wall_timer <= 0.0f) m_last_wallrun_normal = Vec3(0.0f, 0.0f, 0.0f);
        }

        // Step hinged bargeable doors (TdMove_Barge + InterpActor hinge rotation)
        update_barge_doors(input, step_dt, scene);

        // Step interactive elevators (InterpActor + InterpTrackMove) and carry player with cab_delta
        update_elevators(input, step_dt, scene);

        // State Machine Dispatch
        switch (m_telemetry.move_state) {
            case EMovement::MOVE_Walking:
            case EMovement::MOVE_Crouch:
            case EMovement::MOVE_StepUp:
            case EMovement::MOVE_AutoStepUp:
            case EMovement::MOVE_180Turn:
                update_ground_locomotion(input, step_dt, scene);
                if (m_telemetry.move_state == EMovement::MOVE_Slide && input.melee) {
                    update_slide(input, 0.0f, scene);
                }
                break;

            case EMovement::MOVE_SoftLanding:
                if (m_telemetry.grounded) {
                    update_landing_moves(input, step_dt, scene);
                } else {
                    update_air_locomotion(input, step_dt, scene);
                }
                break;

            case EMovement::MOVE_Jump:
            case EMovement::MOVE_Falling:
            case EMovement::MOVE_Coil:
            case EMovement::MOVE_WallRunJump:
            case EMovement::MOVE_GrabJump:
            case EMovement::MOVE_WallClimb180TurnJump:
            case EMovement::MOVE_DodgeJump:
            case EMovement::MOVE_180TurnInAir:
                update_air_locomotion(input, step_dt, scene);
                break;

            case EMovement::MOVE_MeleeAir:
            case EMovement::MOVE_MeleeWallrun: {
                EMovement combat_air_state = m_telemetry.move_state;
                float saved_timer = m_state_timer;
                update_air_locomotion(input, step_dt, scene);
                if (m_telemetry.move_state == combat_air_state ||
                    m_telemetry.move_state == EMovement::MOVE_Falling ||
                    m_telemetry.move_state == EMovement::MOVE_Jump) {
                    if (saved_timer < m_telemetry.combat_anim_duration && !m_telemetry.grounded) {
                        m_telemetry.move_state = combat_air_state;
                        m_state_timer = saved_timer;
                        m_telemetry.combat_anim_time = saved_timer;
                    } else if (!m_telemetry.grounded) {
                        m_telemetry.move_state = EMovement::MOVE_Falling;
                        m_state_timer = 0.0f;
                    }
                }
                break;
            }

            case EMovement::MOVE_Melee: {
                EMovement combat_gnd_state = m_telemetry.move_state;
                float saved_timer = m_state_timer;
                update_ground_locomotion(input, step_dt, scene);
                if (m_telemetry.move_state == combat_gnd_state ||
                    m_telemetry.move_state == EMovement::MOVE_Walking ||
                    m_telemetry.move_state == EMovement::MOVE_Crouch ||
                    m_telemetry.move_state == EMovement::MOVE_AutoStepUp) {
                    if (saved_timer < m_telemetry.combat_anim_duration && m_telemetry.grounded) {
                        m_telemetry.move_state = combat_gnd_state;
                        m_state_timer = saved_timer;
                        m_telemetry.combat_anim_time = saved_timer;
                    } else if (m_telemetry.grounded) {
                        const bool crouch = input.crouch || !has_room(kPawnHeight, scene);
                        m_telemetry.move_state = crouch ? EMovement::MOVE_Crouch : EMovement::MOVE_Walking;
                        m_state_timer = 0.0f;
                        set_stance(crouch ? kEyeHeightCrouch : kEyeHeightStand);
                    }
                }
                break;
            }

            case EMovement::MOVE_Barge:
                update_barge(input, step_dt, scene);
                break;

            case EMovement::MOVE_Snatch: {
                // TdMove_Disarm: she has no velocity of her own; AlignPawn flies her to DisarmOffset
                // from the enemy (SetPreciseLocation), where the two canned animations meet.
                if (m_snatch_fail) {
                    m_telemetry.velocity.x *= std::max(0.0f, 1.0f - 4.0f * step_dt);
                    m_telemetry.velocity.y *= std::max(0.0f, 1.0f - 4.0f * step_dt);
                } else {
                    m_telemetry.velocity = Vec3(0.0f, 0.0f, 0.0f);
                }
                if (m_snatch_align) {
                    Vec3 to = m_snatch_target - m_telemetry.position;
                    to.z = 0.0f;
                    const float dist = to.length();
                    const float step = m_snatch_speed * step_dt;
                    if (dist <= step) {
                        if (dist > 1e-3f) move_swept(to, kPawnHeight, kMaxStepHeight, scene);
                        m_snatch_align = false;
                    } else if (move_swept(to * (step / dist), kPawnHeight, kMaxStepHeight, scene).hit) {
                        m_snatch_align = false;  // something in the way (retail moves the enemy instead)
                    }
                }
                if (m_state_timer >= m_snatch_attach) m_telemetry.snatch_weapon_attached = true;
                if (m_state_timer >= m_telemetry.combat_anim_duration) {
                    m_telemetry.move_state = m_telemetry.grounded ? EMovement::MOVE_Walking : EMovement::MOVE_Falling;
                }
                break;
            }

            case EMovement::MOVE_WallRunningLeft:
            case EMovement::MOVE_WallRunningRight:
                update_wallrun(input, step_dt, scene);
                break;

            case EMovement::MOVE_WallClimbing:
                update_wallclimb(input, step_dt, scene);
                break;

            case EMovement::MOVE_Slide:
            case EMovement::MOVE_MeleeSlide:
                update_slide(input, step_dt, scene);
                break;

            case EMovement::MOVE_Grabbing:
            case EMovement::MOVE_GrabPullUp:
            case EMovement::MOVE_IntoGrab:
                update_ledge_grab(input, step_dt, scene);
                break;

            case EMovement::MOVE_GrabTransfer:
                update_grab_transfer(step_dt);
                break;

            case EMovement::MOVE_ZipLine:
                update_zipline(input, step_dt, scene);
                break;

            case EMovement::MOVE_Swing:
                update_swing_bar(input, step_dt, scene);
                break;

            case EMovement::MOVE_Climb:
                update_climb(input, step_dt, scene);
                break;

            case EMovement::MOVE_Balance:
                update_balance(input, step_dt, scene);
                break;

            case EMovement::MOVE_LedgeWalk:
                update_ledge_walk(input, step_dt, scene);
                break;

            case EMovement::MOVE_SkillRoll:
            case EMovement::MOVE_Landing:
            case EMovement::MOVE_LayOnGround:
                update_landing_moves(input, step_dt, scene);
                break;

            case EMovement::MOVE_SpeedVaulting:
            case EMovement::MOVE_VaultOver:
            case EMovement::MOVE_SpringBoarding:
                update_vault(input, step_dt, scene);
                break;

            default:
                m_telemetry.move_state = m_telemetry.grounded ? EMovement::MOVE_Walking : EMovement::MOVE_Falling;
                break;
        }

        // UE3 floor check: a walking pawn follows floors up to MaxStepHeight + MAXFLOORDIST below its
        // feet (stairs, kerbs, seams between meshes); an airborne pawn lands only on floors it touches.
        const EMovement state = m_telemetry.move_state;
        const bool low_profile = (state == EMovement::MOVE_Crouch || state == EMovement::MOVE_Slide ||
                                  state == EMovement::MOVE_MeleeSlide || state == EMovement::MOVE_SkillRoll ||
                                  state == EMovement::MOVE_Coil);
        const bool attached = (state == EMovement::MOVE_Grabbing || state == EMovement::MOVE_GrabPullUp ||
                               state == EMovement::MOVE_IntoGrab || state == EMovement::MOVE_GrabTransfer ||
                               state == EMovement::MOVE_ZipLine ||
                               state == EMovement::MOVE_Swing || state == EMovement::MOVE_Climb ||
                               state == EMovement::MOVE_WallClimbing || state == EMovement::MOVE_SpeedVaulting ||
                               state == EMovement::MOVE_VaultOver || state == EMovement::MOVE_SpringBoarding);
        const bool wallrunning = (state == EMovement::MOVE_WallRunningLeft || state == EMovement::MOVE_WallRunningRight);
        const bool balancing = (state == EMovement::MOVE_Balance || state == EMovement::MOVE_LedgeWalk);
        FloorHit floor;
        const float probe_depth = m_telemetry.grounded ? (kMaxStepHeight + kMaxFloorDist) : kMaxFloorDist;
        const bool has_floor = !attached &&
                               check_ground(scene, probe_depth, low_profile ? kCrouchHeight : kPawnHeight, floor);

        m_telemetry.floor_sloped = has_floor && floor.normal.z < 0.9999f;
        if (has_floor && m_telemetry.velocity.z <= 50.0f) {
            if (!m_telemetry.grounded) {
                land(floor, scene);
            }
            m_telemetry.position.z = floor.z;
            m_telemetry.grounded = true;
            m_base_actor = floor.actor_index;
            if (m_telemetry.velocity.z < 0.0f) {
                m_telemetry.velocity.z = 0.0f;
            }
        } else if (balancing) {
            m_telemetry.grounded = true;
            if (m_telemetry.velocity.z < 0.0f) {
                m_telemetry.velocity.z = 0.0f;
            }
        } else if (!attached && !wallrunning) {
            m_base_actor = -1;
            if (m_telemetry.grounded) {
                // Walked / slid / rolled off an edge (TdMove_Falling.StartMove).
                const EMovement from = m_telemetry.move_state;
                const Vec3 fwd = facing_forward();
                if (from == EMovement::MOVE_Slide || from == EMovement::MOVE_MeleeSlide) {
                    // TdMove_Slide.StopMove halves the velocity.
                    m_telemetry.velocity.x *= 0.5f;
                    m_telemetry.velocity.y *= 0.5f;
                } else if (from == EMovement::MOVE_Walking && m_telemetry.velocity.dot(fwd) < 0.0f) {
                    // TdMove_Falling.StartMove: backing off a drop with room for her below the
                    // edge (CanStand two half heights under her centre), the horizontal velocity
                    // is zeroed so the pawn drops straight down.
                    if (has_room_at(m_telemetry.position - Vec3(0.0f, 0.0f, kPawnHeight), kPawnHeight, scene)) {
                        m_telemetry.velocity.x = 0.0f;
                        m_telemetry.velocity.y = 0.0f;
                    }
                }
                if (!is_air_move(from)) {
                    leave_ground(EMovement::MOVE_Falling);
                    set_stance(kEyeHeightStand);
                } else {
                    m_telemetry.grounded = false;
                }
            }
            if (m_telemetry.position.z > m_fall_peak_z) {
                m_fall_peak_z = m_telemetry.position.z;
            }
        } else {
            m_base_actor = -1;
        }

        check_hazard_volumes(step_dt, scene);
        m_jump_buffer = std::max(0.0f, m_jump_buffer - step_dt);
    }

    // The barge's custom animation only plays while the move does (walking off a ledge ends it).
    if (m_telemetry.move_state != EMovement::MOVE_Barge) {
        m_barge_anim = 0;
        m_telemetry.barge_anim = 0;
        m_telemetry.barge_anim_weight = 0.0f;
    }

    // 4. Combat, Weapons & Disarms
    update_combat_and_weapons(input, effective_dt, scene);

    // 5. AI Bots Simulation
    update_ai_bots(effective_dt, scene);

    // 6. Health Regeneration
    update_health_and_regen(effective_dt);

    // 7. Checkpoints, Kill Volumes & Collectibles
    update_checkpoints_and_volumes(scene);

    // 8. Final Telemetry Update
    m_prev_turn_180 = input.turn_180;
    m_telemetry.tick++;
    m_telemetry.sim_time += effective_dt;
    m_telemetry.swing_angle = m_swing_angle;
    m_telemetry.body_yaw_deg = m_pawn_yaw;
    update_against_wall(effective_dt, scene);
    {
        // The first-person mesh, and the eye in it, do not take a change of the floor's height at
        // once (TdPawn.SmoothOffset / TargetMeshTranslationZ, native). Retail over a 32 uu step,
        // either way, walking or running: the eye is left 20 uu behind, no more, and closes on the
        // pawn by 0.835 a sixtieth of a second until 6.7 uu is left, then at 60 uu/s. On a flight
        // of stairs, a ramp to the pawn: 1.6 low climbing at 96 uu/s of rise, 14 at 249, 21 at 283
        // and 293; 15 to 16 high coming down at 280 to 446; nothing at 79. So: what the floor
        // moves faster than 85 uu/s is held back, up to 20 uu, and let out as over the step.
        const bool walking = m_telemetry.grounded && m_base_actor < 0 &&
                             (m_telemetry.move_state == EMovement::MOVE_Walking || m_telemetry.move_state == EMovement::MOVE_Crouch);
        // A heave ends a few units over the floor (17 after the free hang's collision lift) and the
        // walk drops her onto it: held back the same way, whatever she lands on (retail's camera
        // eased that drop, 14.6 uu for its hovering capsule, over 0.2 s: 2026-09-29 21:48).
        const bool heave_landed = m_smooth_was_heave && m_telemetry.grounded &&
                                  (m_telemetry.move_state == EMovement::MOVE_Walking || m_telemetry.move_state == EMovement::MOVE_Crouch);
        if (((walking && m_smooth_was_walking) || heave_landed) && effective_dt > 0.0f) {
            const float dz = m_telemetry.position.z - m_smooth_last_z;
            const float held = std::max(0.0f, std::abs(dz) - 85.0f * effective_dt);
            m_mesh_smooth_z = std::clamp(m_mesh_smooth_z - (dz < 0.0f ? -held : held), -20.0f, 20.0f);
        }
        if (std::abs(m_mesh_smooth_z) > 6.7f) {
            m_mesh_smooth_z *= std::pow(0.835f, effective_dt * 60.0f);
        } else {
            const float step = 60.0f * effective_dt;
            m_mesh_smooth_z = std::abs(m_mesh_smooth_z) <= step ? 0.0f : m_mesh_smooth_z - (m_mesh_smooth_z < 0.0f ? -step : step);
        }
        m_smooth_was_walking = walking;
        m_smooth_was_heave = m_telemetry.move_state == EMovement::MOVE_GrabPullUp;
        m_smooth_last_z = m_telemetry.position.z;
        m_telemetry.camera_mesh_offset.z = m_mesh_smooth_z;
    }
    // Retail's frames have the pistol in her hand between 0.08 and 0.16 s after the move ends, not
    // on its last frame.
    if (m_telemetry.move_state == EMovement::MOVE_Snatch) {
        m_snatch_ended = 0.0f;
    } else {
        m_snatch_ended += effective_dt;
        if (m_snatch_ended >= 0.1f) m_telemetry.snatch_weapon_attached = true;
    }
    for (auto& bot : scene.enemies) {
        if (bot.disarm_weapon.empty()) continue;
        bot.disarm_weapon_time -= effective_dt;
        if (bot.disarm_weapon_time <= 0.0f || m_telemetry.move_state != EMovement::MOVE_Snatch) bot.disarm_weapon.clear();
    }
    m_telemetry.balance_lean = std::clamp(m_balance_lean + m_balance_sway, -1.0f, 1.0f);
    if (m_telemetry.move_state != EMovement::MOVE_Balance) {
        m_telemetry.balance_danger = 0;
        m_balance_danger_time = 0.0f;
        m_balance_fall_time = -1.0f;
        m_balance_time = 0.0f;
        m_balance_sway = 0.0f;
    }
    m_telemetry.speed_2d = m_telemetry.velocity.length_xy();
    m_telemetry.speed_3d = m_telemetry.velocity.length();

    // Dynamic FOV scaling: 90° horizontal at base (BaseEngine.ini FOVAngle=90), up to 98° at full sprint
    float speed_ratio = std::clamp((m_telemetry.speed_2d - m_config.run_speed) / (m_config.sprint_speed - m_config.run_speed), 0.0f, 1.0f);
    m_telemetry.fov_deg = 90.0f + 8.0f * speed_ratio;
}

// -----------------------------------------------------------------------------
// Reaction Time Slow-Motion Subsystem
// -----------------------------------------------------------------------------
void ParkourController::update_reaction_time(const InputFrame& input, float dt, float& effective_dt) {
    if (input.reaction_time && m_telemetry.reaction_energy > 5.0f) {
        m_telemetry.reaction_active = true;
    } else if (m_telemetry.reaction_energy <= 0.0f || !input.reaction_time) {
        m_telemetry.reaction_active = false;
    }

    if (m_telemetry.reaction_active) {
        effective_dt = dt * m_config.reaction_time_dilation;
        m_telemetry.reaction_energy = std::max(0.0f, m_telemetry.reaction_energy - m_config.reaction_time_drain * dt);
    } else {
        // Slowly recharge energy when running and chaining moves
        if (m_telemetry.speed_2d > m_config.jog_speed) {
            m_telemetry.reaction_energy = std::min(100.0f, m_telemetry.reaction_energy + 4.0f * dt);
        }
    }
}

// -----------------------------------------------------------------------------
// Camera, Orientation & Look-At Objective Hint Subsystem
// -----------------------------------------------------------------------------
void ParkourController::update_camera_and_inputs(const InputFrame& input, float dt, const LevelScene& scene) {
    // Q: 180° turn (TdMove_180Turn on the ground from Walking only; TdMove_180TurnInAir in the air
    // when moving the way you face). Both are root-rotation moves: the view swings TurnTime.
    if (input.turn_180 && !m_prev_turn_180 && m_turn_timer <= 0.0f) {
        const EMovement st = m_telemetry.move_state;
        bool start = false;
        if (st == EMovement::MOVE_Walking || st == EMovement::MOVE_AutoStepUp || st == EMovement::MOVE_SoftLanding) {
            m_telemetry.move_state = EMovement::MOVE_180Turn;
            m_state_timer = 0.0f;
            start = true;
        } else if (st == EMovement::MOVE_Jump || st == EMovement::MOVE_Falling || st == EMovement::MOVE_Coil ||
                   st == EMovement::MOVE_WallRunJump || st == EMovement::MOVE_GrabJump ||
                   st == EMovement::MOVE_WallClimb180TurnJump) {
            const Vec3 h = horiz(m_telemetry.velocity);
            if (h.length() > 1.0f && facing_forward().dot(h.normalized()) >= 0.2f) {
                m_telemetry.move_state = EMovement::MOVE_180TurnInAir;
                m_turned_in_air = true;
                m_coil_timer = 0.0f;
                start = true;
            }
        }
        if (start) {
            m_turn_timer = m_config.turn_180_time;
            m_turn_total = m_config.turn_180_time;
            m_turn_target_yaw = m_telemetry.yaw_deg + 180.0f;
        }
    }

    // TdMove camera rules: StopMove / StartMove of the move the pawn changed to since the last frame,
    // then TdMove.UpdateViewRotation's look lock, look-at, constraint and recentring. A recorded
    // view (the replay harness) already went through retail's, so it is applied as it is.
    if (m_telemetry.move_state != m_cam_move) camera_move_changed(m_cam_move, m_telemetry.move_state);
    m_cam_move_time += dt;
    float yaw_delta = input.look_yaw_delta;
    float pitch_delta = input.look_pitch_delta;
    if (!input.view_recorded) camera_view_rotation(yaw_delta, pitch_delta, dt);

    if (m_turn_timer > 0.0f) {
        const float step = (wrap_deg(m_turn_target_yaw - m_telemetry.yaw_deg) /
                            std::max(m_turn_timer, 1e-4f)) * std::min(dt, m_turn_timer);
        m_telemetry.yaw_deg += step;
        // The 180 turns are root rotations: the body turns and the view turns with it. The wallrun
        // turn to the wall only swings the view (bDisableControllerFacingPawnYawRotation).
        const bool wallrun = (m_telemetry.move_state == EMovement::MOVE_WallRunningLeft ||
                              m_telemetry.move_state == EMovement::MOVE_WallRunningRight);
        if (!wallrun) m_pawn_yaw += step;
        m_turn_timer -= dt;
        if (m_turn_timer <= 0.0f) {
            m_turn_timer = 0.0f;
            if (!wallrun) m_pawn_yaw += wrap_deg(m_turn_target_yaw - m_telemetry.yaw_deg);
            m_telemetry.yaw_deg = m_turn_target_yaw;
        }
        m_telemetry.pitch_deg = std::clamp(m_telemetry.pitch_deg + pitch_delta, -85.0f, 85.0f);
    } else {
        m_telemetry.yaw_deg += yaw_delta;
        m_telemetry.pitch_deg = std::clamp(m_telemetry.pitch_deg + pitch_delta, -85.0f, 85.0f);
    }

    // Keep yaw normalized in [0, 360)
    while (m_telemetry.yaw_deg < 0.0f) m_telemetry.yaw_deg += 360.0f;
    while (m_telemetry.yaw_deg >= 360.0f) m_telemetry.yaw_deg -= 360.0f;

    // TdPawn.FaceRotation: the body follows the final view unless the move holds it.
    camera_face_rotation(dt);

    // Look-At Route Hint: Smoothly swivels view towards next checkpoint / runner objective
    if (input.look_at) {
        Vec3 target_pos = m_telemetry.position + Rotator::from_degrees(0.0f, m_telemetry.yaw_deg, 0.0f).forward() * 1000.0f;

        if (m_telemetry.active_checkpoint < static_cast<int>(scene.checkpoints.size())) {
            target_pos = scene.checkpoints[m_telemetry.active_checkpoint];
        } else {
            // Find nearest runner vision actor
            float closest_d = 1e9f;
            for (const auto& act : scene.actors) {
                if (act.is_runner_vision || act.is_checkpoint) {
                    float d = m_telemetry.position.distance(act.location);
                    if (d < closest_d) {
                        closest_d = d;
                        target_pos = act.location;
                    }
                }
            }
        }

        Vec3 diff = target_pos - (m_telemetry.position + Vec3(0, 0, m_telemetry.eye_height));
        float dist_xy = diff.length_xy();
        if (dist_xy > 10.0f) {
            float desired_yaw = std::atan2(diff.y, diff.x) * RAD2DEG;
            float desired_pitch = std::atan2(diff.z, dist_xy) * RAD2DEG;

            // Smooth interpolation
            float yaw_diff = desired_yaw - m_telemetry.yaw_deg;
            while (yaw_diff < -180.0f) yaw_diff += 360.0f;
            while (yaw_diff > 180.0f) yaw_diff -= 360.0f;

            m_telemetry.yaw_deg += yaw_diff * std::min(1.0f, 8.0f * dt);
            m_telemetry.pitch_deg += (desired_pitch - m_telemetry.pitch_deg) * std::min(1.0f, 8.0f * dt);
        }
    }

    // Camera roll interpolation: smoothly relax roll unless in wallrun or lethal fall/death
    if (m_telemetry.move_state != EMovement::MOVE_WallRunningLeft &&
        m_telemetry.move_state != EMovement::MOVE_WallRunningRight &&
        !m_telemetry.falling_to_death &&
        !m_telemetry.fall_death_impact) {
        m_telemetry.camera_roll_deg += (0.0f - m_telemetry.camera_roll_deg) * std::min(1.0f, 10.0f * dt);
    }
}

// -----------------------------------------------------------------------------
// TdMove camera rules (TdPlayerController.UpdateRotation -> TdMove.UpdateViewRotation ->
// TdPawn.FaceRotation). During many moves the view may only turn so far from the body (the table
// in move_camera); some moves take the look away for a while (DisableLookTime /
// SetIgnoreLookInput), swing the view back to the body (ResetCameraLook) or at a target
// (SetLookAtTarget*). The body (m_pawn_yaw) turns with the view unless the move holds it.
// -----------------------------------------------------------------------------
void ParkourController::camera_ignore_look(float seconds) {
    // TdPawn.SetIgnoreLookInput: -1 = until StopIgnoreLookInput (the move ends); a timed lock only
    // starts when look input is not locked already.
    if (seconds < 0.0f) m_ignore_look_time = -1.0f;
    else if (seconds > 0.0f && m_ignore_look_time == 0.0f) m_ignore_look_time = seconds;
}

void ParkourController::camera_reset_look(float seconds) {
    m_reset_look_time = std::max(0.0f, seconds);  // CancelResetCameraLookTime = Now + seconds
}

// TdPlayerPawn.UpdateAgainstWall / CheckAgainstWall are native. From a retail recording made for
// it, walking into a flat wall and away again, standing and crouched, with and without a pistol:
//  - at the wall both hands go up on it (state 1) with the body up to 28.6 degrees off square to
//    it; from 29.6 to 59.9 only the hand of the near shoulder (2 the left, 3 the right); from 63.9
//    neither. Walked in at 10 and 20 degrees: both; at 30 and 60: the near one;
//  - standing, it is on up to 11 uu short of touching the wall and off from 12;
//  - running at it (430 uu/s) it comes on 65 uu out square on, 61 at 20 degrees, 49 at 30, 25 at
//    60, and creeping up to it crouched only as she arrives: the reach grows with her speed;
//  - walking away the arms start down about 0.15 s after she starts (StopAgainstWall checks again
//    on a 0.15 s timer);
//  - things that stop below her shoulders (a pipe stub, a vent) do not count.
// Fitted: a trace from each shoulder (15 uu either side) along her facing, 43.3 uu from her centre
// line and 0.121 s of her speed further, that has to meet a wall no more than 62 degrees off square.
// Only in the moves with bEnableAgainstWall (Walking, Crouch, LedgeWalk).
bool ParkourController::leg_line_check(const Vec3& from, const Vec3& to, const LevelScene& scene, Vec3& hit, Vec3& normal) const {
    const TraceHit t = trace_ray(from, to, scene, COLL_BlockZeroExtent);
    if (!t.hit) return false;
    hit = t.point;
    normal = t.normal;
    return true;
}

void ParkourController::update_against_wall(float dt, const LevelScene& scene) {
    const EMovement m = m_telemetry.move_state;
    int state = 0;
    if ((m == EMovement::MOVE_Walking || m == EMovement::MOVE_Crouch || m == EMovement::MOVE_LedgeWalk) && m_telemetry.grounded) {
        // TdPawn's native check, as the executable has it: a box of 2 x 2 x 5 (half extents) swept
        // from 14 uu to either side of her centre, 68 above it (26 crouched), the way she faces,
        // by 40 uu or 0.28 of her speed that way if that is more. A hand has a wall where the
        // box meets one that faces her within 60 degrees. The hand's place is where the box stopped,
        // moved out to its side by up to 15 uu as she looks down.
        const Rotator body = Rotator::from_degrees(0.0f, m_pawn_yaw, 0.0f);
        const Vec3 fwd = body.forward();
        const Vec3 right = body.right();
        const bool crouched = m == EMovement::MOVE_Crouch;
        const Vec3 centre = m_telemetry.position + Vec3(0.0f, 0.0f, 0.5f * (crouched ? kCrouchHeight : kPawnHeight));
        const float reach = std::max(40.0f, m_telemetry.velocity.dot(fwd) * 0.7f * 0.4f);
        const float look_down = std::min(0.0f, std::sin(m_telemetry.pitch_deg * DEG2RAD));
        const Vec3 extent(2.0f, 2.0f, 5.0f);
        bool hand[2] = {false, false};
        Vec3 normal(0.0f, 0.0f, 0.0f);
        for (int k = 1; k >= 0; --k) {  // the right hand's first
            const float side = k == 0 ? -1.0f : 1.0f;
            const Vec3 start = centre + right * (14.0f * side) + Vec3(0.0f, 0.0f, crouched ? 26.0f : 68.0f);
            const TraceHit hit = sweep_box(start, extent, fwd * reach, scene);
            const bool is_barge_door = hit.hit && hit.actor_index >= 0 &&
                                       static_cast<size_t>(hit.actor_index) < scene.actors.size() &&
                                       scene.actors[static_cast<size_t>(hit.actor_index)].barge_door >= 0;
            if (hit.hit && hit.normal.dot(fwd) < -0.5f && !(is_barge_door && m_telemetry.velocity.dot(fwd) > 180.0f)) {
                hand[k] = true;
                m_telemetry.against_wall_hand[k] = hit.point - right * (side * look_down * 15.0f);
                normal = normal + horiz(hit.normal);
            }
        }
        const bool heavy = m_telemetry.weapon.equipped && m_telemetry.weapon.is_heavy;
        state = hand[0] && hand[1] ? 1 : hand[0] ? (heavy ? 1 : 2) : hand[1] ? (heavy ? 1 : 3) : 0;
        if (state != 0 && normal.length_sq() > 1e-4f) m_against_wall_yaw = yaw_of(-normal);
    }
    if (m == EMovement::MOVE_Barge || m == EMovement::MOVE_AirBarge ||
        m == EMovement::MOVE_Melee || m == EMovement::MOVE_MeleeSlide) {
        m_against_wall = 0;
        m_against_wall_off = 0.15f;
    } else if (state != 0) {
        // UpdateAgainstWall: the state holds for 0.15 s after the check last found a wall.
        m_against_wall = state;
        m_against_wall_off = 0.0f;
    } else if (m_against_wall != 0) {
        m_against_wall_off += dt;
        if (m_against_wall_off >= 0.15f) m_against_wall = 0;
    }
    m_telemetry.against_wall = m_against_wall;
}

// TdMove.CheckForCameraCollision and the moves' own (Walking, Crouch, Slide, GrabPullUp, SpeedVault):
// a 2 uu box swept from 5 behind the camera to a little ahead of it, and where it meets something
// TdPawn.OffsetMeshXY moves the first-person mesh, and so the eye, back by what is missing. The
// script adds the miss each frame to a mesh that is already offset, which settles with the box's
// end just clear; here the eye comes in without the offset, so one sweep gives that place.
void ParkourController::update_camera_collision(const Vec3& eye, float dt, const LevelScene& scene) {
    const EMovement m = m_telemetry.move_state;
    const Vec3 extent(2.0f, 2.0f, 2.0f);
    // How far short of `reach` ahead of the camera the box stops, in the plane (0 with nothing there).
    auto miss = [&](const Vec3& dir, float reach, float lift, Vec3* back) {
        const Vec3 start = eye - dir * 5.0f;
        const Vec3 end = eye + dir * reach + Vec3(0.0f, 0.0f, lift);
        const TraceHit hit = sweep_box(start, extent, end - start, scene);
        if (!hit.hit) return 0.0f;
        const Vec3 at = start + (end - start) * hit.fraction;  // HitLocation: the box's centre
        if (back) *back = Vec3(at.x - end.x, at.y - end.y, 0.0f);
        return (end - at).length_xy();
    };
    const Vec3 facing = Rotator::from_degrees(0.0f, m_pawn_yaw, 0.0f).forward();
    Vec3 want(0.0f, 0.0f, 0.0f);
    bool base = false;
    switch (m) {
        case EMovement::MOVE_Walking:
        case EMovement::MOVE_Jump:
        case EMovement::MOVE_Melee:
        case EMovement::MOVE_180Turn:
        case EMovement::MOVE_Vertigo:
        case EMovement::MOVE_WallRunningLeft:
        case EMovement::MOVE_WallRunningRight:
            base = true;  // bUseCameraCollision with TdMove's own check
            break;
        case EMovement::MOVE_Crouch:
        case EMovement::MOVE_Slide:
            // The first 0.2 s of both: 15 ahead and 5 up, and a unit more than the miss.
            if (m_state_timer < 0.2f) {
                const float d = miss(facing, 15.0f, 5.0f, nullptr);
                if (d > 0.0f) want = facing * -(d + 1.0f);
            } else if (m == EMovement::MOVE_Crouch) {
                base = true;
            }
            break;
        case EMovement::MOVE_GrabPullUp: {
            // Along the view, 20 ahead; the offset is HitLocation - TraceEnd in the world.
            const Vec3 view = Rotator::from_degrees(m_telemetry.pitch_deg, m_telemetry.yaw_deg, 0.0f).forward();
            miss(view, 20.0f, 0.0f, &want);
            break;
        }
        case EMovement::MOVE_SpeedVaulting:
        case EMovement::MOVE_VaultOver: {
            // CameraCollisionDirection = MoveNormal cross (0, 0, -1): to the side she vaults on.
            const Vec3 n = horiz(m_telemetry.wall_normal);
            if (n.length_sq() > 0.25f) miss(n.normalized().cross(Vec3(0.0f, 0.0f, -1.0f)), 15.0f, 0.0f, &want);
            break;
        }
        default:
            break;
    }
    if (base) want = facing * -miss(facing, 11.0f, 0.0f, nullptr);

    // OffsetMeshXY is native. Retail jumping into a wall looking 75 degrees down, the swan neck
    // carrying the eye to within a unit of it: the eye is 12.6 uu off the wall 0.1 s after she
    // meets it (11.7 uu back, where the sweep's 11 and the box's 2 put it) and stays there through
    // the jump with the view not pushed at all; once she is falling, a move without the check, it
    // goes forward again at 97 uu/s. So: back at 170 uu/s, forward at 100.
    constexpr float kCameraMeshBack = 170.0f, kCameraMeshReturn = 100.0f;  // uu/s
    {
        const Vec3 d = want - m_cam_mesh_offset;
        const float far = d.length_xy();
        const float step = (want.length_xy() >= m_cam_mesh_offset.length_xy() ? kCameraMeshBack : kCameraMeshReturn) * dt;
        m_cam_mesh_offset = far <= step ? want : m_cam_mesh_offset + d * (step / far);
    }
    m_telemetry.camera_mesh_offset.x = m_cam_mesh_offset.x;
    m_telemetry.camera_mesh_offset.y = m_cam_mesh_offset.y;

    // TdMove_Walking.CheckForCameraCollision goes on (AgainstWallState 0): the same box along the
    // view, 15 ahead. Met, the view cannot go further down than it is, and met inside the last
    // fifth of the sweep, or as the limit comes on, it is brought up by how soon it was met.
    const bool was = m_cam_constrain_look;
    m_cam_constrain_look = false;
    if (m == EMovement::MOVE_Walking && m_against_wall == 0) {
        const Vec3 camera = eye + m_cam_mesh_offset;
        const Vec3 view = Rotator::from_degrees(m_telemetry.pitch_deg, m_telemetry.yaw_deg, 0.0f).forward();
        const Vec3 start = camera - view * 5.0f;
        const TraceHit hit = sweep_box(start, extent, view * 20.0f, scene);
        if (hit.hit) {
            const float pitch = m_telemetry.pitch_deg * kUUPerDeg;
            m_cam_min_pitch = (!was || hit.fraction < 0.8f) ? std::trunc(pitch * hit.fraction) : pitch;
            m_cam_constrain_look = true;
        }
    }
}

void ParkourController::camera_look_at(float yaw, float pitch, float interp_time, float duration) {
    m_look_at_active = true;
    m_look_at_is_location = false;
    m_look_at_yaw = yaw;
    m_look_at_pitch = pitch;
    m_look_at_interp = std::max(interp_time, 1e-4f);
    m_look_at_duration = duration;
}

void ParkourController::camera_look_at_location(const Vec3& target, float interp_time, float duration) {
    camera_look_at(0.0f, 0.0f, interp_time, duration);
    m_look_at_is_location = true;
    m_look_at_location = target;
}

bool ParkourController::camera_body_yaw(float& yaw) const {
    // Moves that turn the body themselves while it does not follow the view (bDisableFaceRotation
    // with SetRotation / SetPreciseRotation / the slide's own steering in retail).
    switch (m_telemetry.move_state) {
        case EMovement::MOVE_Slide:
            yaw = m_slide_yaw;
            return true;
        case EMovement::MOVE_WallRunningRight:
        case EMovement::MOVE_WallRunningLeft:
            yaw = yaw_of(m_wall_tangent);  // TdMove_WallRun.FacePawnAlongWall
            return true;
        case EMovement::MOVE_Grabbing:
        case EMovement::MOVE_GrabPullUp:
        case EMovement::MOVE_IntoGrab:
        case EMovement::MOVE_GrabTransfer:
            if (horiz(m_telemetry.wall_normal).length() < 0.5f) return false;
            yaw = yaw_of(-m_telemetry.wall_normal);  // TdMove_IntoGrab: SetPreciseRotation(-MoveNormal)
            return true;
        case EMovement::MOVE_Climb:
            if (horiz(m_climb_normal).length() < 0.5f) return false;
            yaw = yaw_of(-m_climb_normal);
            return true;
        case EMovement::MOVE_LedgeWalk:
            if (horiz(m_ledge_walk_normal).length() < 0.5f) return false;
            yaw = yaw_of(m_ledge_walk_normal);  // back to the wall
            return true;
        case EMovement::MOVE_ZipLine:
            yaw = m_zip_body_yaw;  // TdMove_IntoZipLine.ReachedPreciseLocation: SetRotation(slope)
            return true;
        case EMovement::MOVE_Balance: {
            // The beam direction the walk uses (the end the view faces).
            const Vec3 ab = horiz(m_balance_end - m_balance_start);
            if (ab.length() < 1.0f) return false;
            const Vec3 u = ab.normalized();
            yaw = yaw_of(facing_forward().dot(u) >= 0.0f ? u : -u);
            return true;
        }
        default:
            return false;
    }
}

void ParkourController::camera_move_changed(EMovement from, EMovement to) {
    // TdMove.StopMove of the move left: look input comes back, its look-at and recentring stop, and
    // some moves ease the body back under the view (FaceRotationTimeLeft).
    m_ignore_look_time = 0.0f;
    m_look_at_active = false;
    m_reset_look_time = -1.0f;
    switch (from) {
        case EMovement::MOVE_Grabbing:         m_face_rotation_time_left = 0.5f; break;
        case EMovement::MOVE_WallRunningRight:
        case EMovement::MOVE_WallRunningLeft:  if (!m_wall_turned) m_face_rotation_time_left = 0.3f; break;
        case EMovement::MOVE_Slide:            m_face_rotation_time_left = 0.4f; break;
        case EMovement::MOVE_Climb:            m_face_rotation_time_left = 0.2f; break;
        case EMovement::MOVE_Balance:          m_face_rotation_time_left = 0.4f; break;
        default: break;
    }

    // TdMove.StartMove of the new one.
    const MoveCamera mc = move_camera(to);
    m_cam_move = to;
    m_cam_move_time = 0.0f;
    m_face_rotation_disabled = mc.disable_face_rotation;
    m_vault_down = false;
    // The body at the start of the move: where the move places it, or - for moves the body follows
    // the view in - the view, which the port's move code may just have snapped to the move's facing
    // (retail rotates the pawn and the controller follows).
    float held = 0.0f;
    if (m_face_rotation_disabled && camera_body_yaw(held)) m_pawn_yaw = held;
    else if (!m_face_rotation_disabled && m_face_rotation_time_left <= 0.0f) m_pawn_yaw = wrap_deg(m_telemetry.yaw_deg);
    if (mc.disable_look_time != 0.0f) camera_ignore_look(mc.disable_look_time);
    const bool heavy = m_telemetry.weapon.equipped && m_telemetry.weapon.is_heavy;  // GetWeaponType() == 1
    switch (to) {
        case EMovement::MOVE_Falling:
            if (from == EMovement::MOVE_WallClimbing) camera_reset_look(0.5f);
            break;
        case EMovement::MOVE_Grabbing:
            camera_reset_look(0.15f);  // TdMove_IntoGrab.ReachedPreciseLocation
            break;
        case EMovement::MOVE_GrabPullUp:
            camera_reset_look(0.2f);   // ResetCameraLook(DisableLookTime)
            break;
        case EMovement::MOVE_WallRunningRight:
        case EMovement::MOVE_WallRunningLeft: {
            // bUseAbsoluteYawConstraint: a world window from the wall normal to the run direction.
            const float n = yaw_of(m_telemetry.wall_normal);
            const bool right = (to == EMovement::MOVE_WallRunningRight);
            m_wallrun_yaw_min = right ? n : n - 90.0f;
            m_wallrun_yaw_max = right ? n + 90.0f : n;
            break;
        }
        case EMovement::MOVE_SpeedVaulting:
        case EMovement::MOVE_VaultOver:
            // High vaults (VaultTimeUp > 0, bResetCamera): no look until the hand plant, and the view
            // swings back to the body over 0.2 s.
            if (m_vault_look_lock > 0.0f) {
                camera_ignore_look(m_vault_look_lock);
                camera_reset_look(0.2f);
            }
            break;
        case EMovement::MOVE_Climb:
            camera_reset_look(0.3f);
            break;
        case EMovement::MOVE_Snatch:
            camera_reset_look(0.2f);  // TdMove_Disarm.StartMove
            break;
        case EMovement::MOVE_Barge:
            camera_reset_look(m_barge_kick ? 0.2f : 0.3f);  // TdMove_Barge.StartBargin
            break;
        case EMovement::MOVE_Landing:
            // TdMove_Landing.LandHard: no look until the landing animation is done.
            camera_ignore_look(-1.0f);
            camera_reset_look(heavy ? 0.1f : 0.3f);
            break;
        case EMovement::MOVE_LayOnGround:
            camera_reset_look(0.3f);  // TdMove_Landing.LandBackwards
            break;
        case EMovement::MOVE_SkillRoll:
            camera_ignore_look(-1.0f);
            camera_reset_look(0.2f);
            break;
        case EMovement::MOVE_MeleeWallrun:
            camera_reset_look(0.1f);
            break;
        case EMovement::MOVE_WallClimb180TurnJump:
            camera_reset_look(0.2f);
            break;
        case EMovement::MOVE_Swing:
            camera_reset_look(0.15f);  // AnimBlendTime
            break;
        case EMovement::MOVE_180TurnInAir:
            // Look back at the take-off (LastJumpLocation + 90 above the pawn's centre) for up to 2 s.
            camera_look_at_location(m_last_jump_location + Vec3(0.0f, 0.0f, 90.0f + 90.0f), 0.3f, 2.0f);
            break;
        default:
            break;
    }
}

void ParkourController::camera_view_rotation(float& yaw_d, float& pitch_d, float dt) {
    const EMovement m = m_cam_move;

    // TdPlayerInput.PlayerInput: aTurn and aLookUp are scaled by the pawn's GetMobilityMultiplier
    // (native). With a weapon in hand retail's view turns half as far for the same mouse travel,
    // pistol or rifle, at the ready or not (360 degrees' worth of counts gave 180.5).
    if (m_telemetry.weapon.equipped) {
        yaw_d *= 0.5f;
        pitch_d *= 0.5f;
    }

    // Camera changes inside a move.
    if ((m == EMovement::MOVE_VaultOver || m == EMovement::MOVE_SpeedVaulting) && !m_vault_down &&
        m_telemetry.move_state == m && m_state_timer >= m_path_t1) {
        // TdMove_SpeedVault VaultState 3 (the drop): the body turns back under the view over 0.4 s.
        m_vault_down = true;
        m_face_rotation_disabled = false;
        m_face_rotation_time_left = 0.4f;
    }
    if (m == EMovement::MOVE_GrabPullUp && m_face_rotation_disabled && m_cam_move_time >= kPullUpReleaseCamera) {
        m_face_rotation_disabled = false;  // TdMove_GrabPullUp.ReleaseCamera (TimeToReleaseCamera)
        m_face_rotation_time_left = 0.25f;
    }
    if (m == EMovement::MOVE_GrabJump && m_face_rotation_disabled && m_cam_move_time >= 0.1f) {
        m_face_rotation_disabled = false;  // TdMove_GrabJump.OnTimer
        m_face_rotation_time_left = 0.3f;
    }

    // TdPlayerInput.PlayerInput: no look input while it is ignored.
    if (m_ignore_look_time != 0.0f) {
        yaw_d = 0.0f;
        pitch_d = 0.0f;
        if (m_ignore_look_time > 0.0f) m_ignore_look_time = std::max(0.0f, m_ignore_look_time - dt);
    }
    const bool turned = (yaw_d != 0.0f);  // DeltaRot.Yaw as the moves see it

    // TdPlayerController.UpdateRotation: the view turns at 40% while it pitches 63 deg or more away
    // from the body (RotSpeedMod).
    if (std::abs(m_telemetry.pitch_deg) * kUUPerDeg / 16384.0f + 0.3f >= 1.0f) yaw_d *= 0.4f;

    // TdMove_WallClimb.LookAtLedge: up the wall, the view is drawn to a point 100 above the eyes,
    // one radius into the wall (squared up to the wall when within 22.5 deg of it).
    if (m == EMovement::MOVE_WallClimbing && horiz(m_telemetry.wall_normal).length() > 0.5f) {
        const Vec3 n = horiz(m_telemetry.wall_normal).normalized();
        const float wall_yaw = yaw_of(-n);
        const float yaw = (std::abs(wrap_deg(wall_yaw - m_pawn_yaw)) > 22.5f) ? m_pawn_yaw : wall_yaw;
        camera_look_at(yaw, std::atan2(100.0f, kPawnRadius) * RAD2DEG, 0.2f, -1.0f);
    }

    // TdMove.UpdateViewRotation: the look-at target pulls the view itself...
    if (m_look_at_active) {
        if (m_look_at_duration < 0.0f || m_look_at_duration >= m_cam_move_time) {
            float ty = m_look_at_yaw, tp = m_look_at_pitch;
            if (m_look_at_is_location) {
                const Vec3 d = m_look_at_location - (m_telemetry.position + Vec3(0.0f, 0.0f, m_telemetry.eye_height));
                ty = d.length_xy() > 1.0f ? yaw_of(d) : m_telemetry.yaw_deg;
                tp = std::atan2(d.z, std::max(d.length_xy(), 1e-3f)) * RAD2DEG;
            }
            const float f = std::min(1.0f, dt / m_look_at_interp);
            m_telemetry.yaw_deg += wrap_deg(ty - m_telemetry.yaw_deg) * f;
            m_telemetry.pitch_deg = std::clamp(m_telemetry.pitch_deg + (tp - m_telemetry.pitch_deg) * f, -85.0f, 85.0f);
        } else {
            m_look_at_active = false;
        }
    }

    // ... the move's look constraint limits the frame's turn, relative to the body ...
    MoveCamera mc = move_camera(m);
    const bool at_wall = m_against_wall != 0 && (m == EMovement::MOVE_Walking || m == EMovement::MOVE_Crouch);
    if (at_wall) {
        // Against a wall retail's view stops 26 to 27.5 degrees down, standing or crouched, with
        // one hand on it or two (5000 in Unreal units, by the look of it; how far up it goes was
        // not tried), and is brought up to that with a time constant of 0.2 s. It turns freely.
        // Away from the wall the same view went to 80 down.
        mc.constrain = true;
        mc.pitch_min = -5000.0f;
        mc.pitch_max = 32768.0f;
        mc.yaw_min = -65536.0f;
        mc.yaw_max = 65536.0f;
    } else if (m == EMovement::MOVE_Walking && m_cam_constrain_look) {
        // TdMove_Walking.CheckForCameraCollision: with something right in front of the camera she
        // cannot look further down (bConstrainLook; nothing limits the yaw or looking up).
        mc.constrain = true;
        mc.pitch_min = m_cam_min_pitch;
        mc.pitch_max = 32768.0f;
        mc.yaw_min = -65536.0f;
        mc.yaw_max = 65536.0f;
    }
    if (mc.constrain) {
        const float speed = dt / 0.2f;
        float lo = mc.yaw_min, hi = mc.yaw_max;
        if (m == EMovement::MOVE_WallRunningRight || m == EMovement::MOVE_WallRunningLeft) {
            lo = wrap_deg(m_wallrun_yaw_min - m_pawn_yaw) * kUUPerDeg;
            hi = wrap_deg(m_wallrun_yaw_max - m_pawn_yaw) * kUUPerDeg;
        }
        const float rel_yaw = wrap_deg(m_telemetry.yaw_deg - m_pawn_yaw) * kUUPerDeg;
        yaw_d = constrain_axis(rel_yaw, lo, hi, speed, yaw_d * kUUPerDeg) / kUUPerDeg;
        pitch_d = constrain_axis(m_telemetry.pitch_deg * kUUPerDeg, mc.pitch_min, mc.pitch_max, speed,
                                 pitch_d * kUUPerDeg) / kUUPerDeg;
    }
    // TdMove_180TurnInAir.UpdateViewRotation: any yaw turn drops the look back at the take-off.
    if (m == EMovement::MOVE_180TurnInAir && yaw_d != 0.0f) m_look_at_active = false;
    // TdMove_ZipLine.UpdateViewRotation: until the player turns the view (bZipLineLookAssist), it is
    // drawn along the ride to CurrentLookAtPoint, the far end of the impact trace ahead.
    if (m == EMovement::MOVE_ZipLine && m_telemetry.move_state == EMovement::MOVE_ZipLine) {
        if (turned) {
            m_zip_look_assist = false;
            m_look_at_active = false;
        } else if (m_zip_look_assist) {
            camera_look_at_location(m_zip_look_at, 0.2f, -1.0f);
        }
    }

    // ... and ResetCameraLook swings it back to the body, level, linearly over the time left.
    if (m_reset_look_time >= 0.0f) {
        m_reset_look_time -= dt;
        if (m_reset_look_time > 0.0f) {
            const float steps = std::max(1.0f, m_reset_look_time / dt);
            m_telemetry.yaw_deg += wrap_deg(m_pawn_yaw - m_telemetry.yaw_deg) / steps;
            m_telemetry.pitch_deg -= m_telemetry.pitch_deg / steps;
        } else {
            m_telemetry.pitch_deg = 0.0f;
            m_reset_look_time = -1.0f;
        }
    }
}

void ParkourController::camera_face_rotation(float dt) {
    // TdPawn.FaceRotation: the body takes the view's yaw, eases to it over FaceRotationTimeLeft
    // after some moves, and stays put (or where the move turns it) while the move disables it.
    float held = 0.0f;
    if (m_face_rotation_disabled) {
        if (camera_body_yaw(held)) m_pawn_yaw = held;
    } else if (m_face_rotation_time_left > 0.0f) {
        m_pawn_yaw += wrap_deg(m_telemetry.yaw_deg - m_pawn_yaw) * std::min(1.0f, dt / m_face_rotation_time_left);
        m_face_rotation_time_left -= dt;
    } else {
        m_pawn_yaw = m_telemetry.yaw_deg;
    }
    m_pawn_yaw = wrap_deg(m_pawn_yaw);
}

// -----------------------------------------------------------------------------
// Swept Collision & Tracing Subsystem (real UE3 level collision)
// -----------------------------------------------------------------------------
// The pawn box is swept against LevelScene::collision (StaticMesh BodySetup hulls / kDOP triangles,
// BlockingVolume brushes, BSP) and against every moving elevator part, whose collision is stored at
// the part's initial pose and is therefore queried shifted by -offset.
ParkourController::TraceHit ParkourController::sweep_capsule(const Capsule& capsule, const Vec3& delta, const LevelScene& scene) const {
    TraceHit best;
    const float half_z = std::max(1.0f, 0.5f * (capsule.height - capsule.bottom_offset));
    const Vec3 extent(capsule.radius, capsule.radius, half_z);
    const Vec3 centre = capsule.base + Vec3(0.0f, 0.0f, capsule.bottom_offset + half_z);

    auto consider = [&](const CollisionHit& h, const Vec3& shift) {
        if (!h.hit) return;
        // Earliest contact wins; ties prefer the most floor-like normal (as CollisionWorld does).
        if (best.hit && (h.time > best.fraction || (h.time == best.fraction && h.normal.z <= best.normal.z))) return;
        best.hit = true;
        best.start_penetrating = h.start_penetrating;
        best.fraction = h.time;
        best.normal = h.normal;
        best.point = h.location + shift;
        best.actor_index = h.actor;
        best.actor = (h.actor >= 0 && static_cast<size_t>(h.actor) < scene.actors.size()) ? &scene.actors[h.actor] : nullptr;
    };

    if (scene.collision) {
        consider(scene.collision->sweep_box(centre, delta, extent, COLL_BlockNonZeroExtent), Vec3(0.0f, 0.0f, 0.0f));
    }
    for (const auto& elev : scene.elevators) {
        for (const auto& part : elev.parts) {
            if (!part.collision) continue;
            consider(part.collision->sweep_box(centre - part.offset, delta, extent, COLL_BlockNonZeroExtent), part.offset);
        }
    }
    for (const auto& door : scene.barge_doors) {
        const float ang = door.open_angle_rad;
        const bool is_rotated = (std::abs(ang) > 1e-5f);
        const float ca = std::cos(-ang);
        const float sa = std::sin(-ang);
        const float ca_fwd = ca;
        const float sa_fwd = -sa;
        for (const auto& part : door.parts) {
            if (!part.collision) continue;
            if (door.state != DoorState::Closed && part.is_blocker_only) continue;
            if (!is_rotated) {
                consider(part.collision->sweep_box(centre, delta, extent, COLL_BlockNonZeroExtent), Vec3(0.0f, 0.0f, 0.0f));
            } else {
                const Vec3 rel_c = centre - door.hinge_pos;
                const Vec3 loc_c = door.hinge_pos + Vec3(ca * rel_c.x - sa * rel_c.y, sa * rel_c.x + ca * rel_c.y, rel_c.z);
                const Vec3 loc_d(ca * delta.x - sa * delta.y, sa * delta.x + ca * delta.y, delta.z);
                CollisionHit h = part.collision->sweep_box(loc_c, loc_d, extent, COLL_BlockNonZeroExtent);
                if (h.hit) {
                    const Vec3 rel_p = h.location - door.hinge_pos;
                    h.location = door.hinge_pos + Vec3(ca_fwd * rel_p.x - sa_fwd * rel_p.y,
                                                       sa_fwd * rel_p.x + ca_fwd * rel_p.y, rel_p.z);
                    h.normal = Vec3(ca_fwd * h.normal.x - sa_fwd * h.normal.y,
                                    sa_fwd * h.normal.x + ca_fwd * h.normal.y, h.normal.z);
                    consider(h, Vec3(0.0f, 0.0f, 0.0f));
                }
            }
        }
    }
    return best;
}

ParkourController::TraceHit ParkourController::trace_ray(const Vec3& start, const Vec3& end, const LevelScene& scene,
                                                         uint8_t channels) const {
    TraceHit best;
    if ((end - start).length_sq() < 1e-8f) return best;

    auto consider = [&](const CollisionHit& h, const Vec3& shift) {
        if (!h.hit || (best.hit && h.time >= best.fraction)) return;
        best.hit = true;
        best.start_penetrating = h.start_penetrating;
        best.fraction = h.time;
        best.normal = h.normal;
        best.point = h.location + shift;
        best.actor_index = h.actor;
        best.actor = (h.actor >= 0 && static_cast<size_t>(h.actor) < scene.actors.size()) ? &scene.actors[h.actor] : nullptr;
    };

    if (scene.collision) consider(scene.collision->line_check(start, end, channels), Vec3(0.0f, 0.0f, 0.0f));
    for (const auto& elev : scene.elevators) {
        for (const auto& part : elev.parts) {
            if (!part.collision) continue;
            consider(part.collision->line_check(start - part.offset, end - part.offset, channels), part.offset);
        }
    }
    for (const auto& door : scene.barge_doors) {
        const float ang = door.open_angle_rad;
        const bool is_rotated = (std::abs(ang) > 1e-5f);
        const float ca = std::cos(-ang);
        const float sa = std::sin(-ang);
        const float ca_fwd = ca;
        const float sa_fwd = -sa;
        for (const auto& part : door.parts) {
            if (!part.collision) continue;
            if (door.state != DoorState::Closed && part.is_blocker_only) continue;
            if (!is_rotated) {
                consider(part.collision->line_check(start, end, channels), Vec3(0.0f, 0.0f, 0.0f));
            } else {
                const Vec3 rs = start - door.hinge_pos;
                const Vec3 re = end - door.hinge_pos;
                const Vec3 loc_s = door.hinge_pos + Vec3(ca * rs.x - sa * rs.y, sa * rs.x + ca * rs.y, rs.z);
                const Vec3 loc_e = door.hinge_pos + Vec3(ca * re.x - sa * re.y, sa * re.x + ca * re.y, re.z);
                CollisionHit h = part.collision->line_check(loc_s, loc_e, channels);
                if (h.hit) {
                    const Vec3 rp = h.location - door.hinge_pos;
                    h.location = door.hinge_pos + Vec3(ca_fwd * rp.x - sa_fwd * rp.y,
                                                       sa_fwd * rp.x + ca_fwd * rp.y, rp.z);
                    h.normal = Vec3(ca_fwd * h.normal.x - sa_fwd * h.normal.y,
                                    sa_fwd * h.normal.x + ca_fwd * h.normal.y, h.normal.z);
                    consider(h, Vec3(0.0f, 0.0f, 0.0f));
                }
            }
        }
    }
    return best;
}

bool ParkourController::check_ground(const LevelScene& scene, float probe_depth, float height, FloorHit& out) const {
    Capsule cap;
    cap.base = m_telemetry.position + Vec3(0.0f, 0.0f, kFloorProbeLift);
    cap.radius = kPawnRadius;
    cap.height = height;
    const Vec3 delta(0.0f, 0.0f, -(kFloorProbeLift + probe_depth));
    const TraceHit hit = sweep_capsule(cap, delta, scene);
    if (!hit.hit || hit.normal.z < kWalkableFloorZ) return false;
    out.z = cap.base.z + delta.z * hit.fraction;
    out.normal = hit.normal;
    out.actor_index = hit.actor_index;
    return true;
}

bool ParkourController::has_room(float height, const LevelScene& scene) const {
    return has_room_at(m_telemetry.position, height, scene);
}

bool ParkourController::has_room_at(const Vec3& feet, float height, const LevelScene& scene) const {
    // Lifted off the floor it rests on and shrunk slightly so resting / sliding contacts do not count.
    constexpr float kLift = 1.0f;
    const float half_z = 0.5f * (height - kLift);
    const Vec3 extent(kPawnRadius - 1.0f, kPawnRadius - 1.0f, half_z);
    const Vec3 centre = feet + Vec3(0.0f, 0.0f, kLift + half_z);
    return box_free(centre, extent, scene);
}

bool ParkourController::box_free(const Vec3& centre, const Vec3& extent, const LevelScene& scene) const {
    if (scene.collision && scene.collision->overlap_box(centre, extent, COLL_BlockNonZeroExtent)) return false;
    for (const auto& elev : scene.elevators) {
        for (const auto& part : elev.parts) {
            if (part.collision && part.collision->overlap_box(centre - part.offset, extent, COLL_BlockNonZeroExtent)) {
                return false;
            }
        }
    }
    for (const auto& door : scene.barge_doors) {
        if (door.state != DoorState::Closed) continue;
        for (const auto& part : door.parts) {
            if (part.collision && part.collision->overlap_box(centre, extent, COLL_BlockNonZeroExtent)) {
                return false;
            }
        }
    }
    return true;
}

ParkourController::TraceHit ParkourController::sweep_box(const Vec3& centre, const Vec3& extent, const Vec3& delta,
                                                         const LevelScene& scene) const {
    TraceHit best;
    auto consider = [&](const CollisionHit& h, const Vec3& shift) {
        if (!h.hit) return;
        if (best.hit && (h.time > best.fraction || (h.time == best.fraction && h.normal.z <= best.normal.z))) return;
        best.hit = true;
        best.start_penetrating = h.start_penetrating;
        best.fraction = h.time;
        best.normal = h.normal;
        best.point = h.location + shift;
        best.actor_index = h.actor;
        best.actor = (h.actor >= 0 && static_cast<size_t>(h.actor) < scene.actors.size()) ? &scene.actors[h.actor] : nullptr;
    };
    if (scene.collision) consider(scene.collision->sweep_box(centre, delta, extent, COLL_BlockNonZeroExtent), Vec3(0.0f, 0.0f, 0.0f));
    for (const auto& elev : scene.elevators) {
        for (const auto& part : elev.parts) {
            if (!part.collision) continue;
            consider(part.collision->sweep_box(centre - part.offset, delta, extent, COLL_BlockNonZeroExtent), part.offset);
        }
    }
    for (const auto& door : scene.barge_doors) {
        if (door.state != DoorState::Closed) continue;
        for (const auto& part : door.parts) {
            if (!part.collision) continue;
            consider(part.collision->sweep_box(centre, delta, extent, COLL_BlockNonZeroExtent), Vec3(0.0f, 0.0f, 0.0f));
        }
    }
    return best;
}


float ParkourController::headroom(float max_rise, const LevelScene& scene) const {
    if (max_rise <= 0.0f) return 0.0f;
    Capsule cap;
    cap.base = m_telemetry.position;
    cap.radius = kPawnRadius;
    cap.height = kPawnHeight;
    const TraceHit hit = sweep_capsule(cap, Vec3(0.0f, 0.0f, max_rise), scene);
    if (!hit.hit) return max_rise;
    return std::max(0.0f, max_rise * hit.fraction - kContactSkin);
}

// The game's wall checks trace with the pawn's extent at one height: a thin slab of the footprint
// swept along `dir` for `reach` beyond the pawn centre. A face is reported with its outward
// (horizontal) normal and its distance from the pawn centre.
ParkourController::WallFace ParkourController::probe_wall(const Vec3& dir_in, float reach, float height,
                                                          const LevelScene& scene) const {
    WallFace out;
    Vec3 dir = horiz(dir_in);
    if (dir.length_sq() < 1e-6f || reach <= 0.0f) return out;
    dir = dir.normalized();

    Capsule cap;
    cap.base = m_telemetry.position;
    cap.radius = kPawnRadius;
    cap.height = height + 1.0f;
    cap.bottom_offset = height - 1.0f;
    const TraceHit hit = sweep_capsule(cap, dir * reach, scene);
    if (!hit.hit || std::abs(hit.normal.z) > 0.3f) return out;
    Vec3 n = horiz(hit.normal);
    if (n.length_sq() < 1e-6f) return out;
    n = n.normalized();
    if (n.dot(dir) > -0.05f) return out;  // a face we are moving along / away from, not into

    const Vec3 centre = m_telemetry.position + Vec3(0.0f, 0.0f, height);
    const float support = kPawnRadius * (std::abs(n.x) + std::abs(n.y));
    out.found = true;
    out.normal = n;
    out.point = hit.point - n * support;  // a point on the face plane
    out.distance = std::max(0.0f, (out.point - centre).dot(-n));
    out.actor_index = hit.actor_index;
    return out;
}

// A ledge ahead: the wall face whose walkable top lies between min_rise and max_rise above the
// feet (TdPawn.FindLedge: a forward trace for the wall, a downward trace just beyond it for the top).
ParkourController::Ledge ParkourController::find_ledge(const Vec3& dir_in, float reach, float min_rise, float max_rise,
                                                       const LevelScene& scene) const {
    Ledge out;
    Vec3 dir = horiz(dir_in);
    if (dir.length_sq() < 1e-6f || max_rise <= min_rise) return out;
    dir = dir.normalized();

    const Vec3& feet = m_telemetry.position;
    // TdPhysicsMove.HandPlantExtentCheckWidth / Height: the hands need a 10 x 10 x 80 clear box
    // standing on the ledge top, so a thin rail with a panel right behind it is not a ledge.
    constexpr float kHandPlantWidth = 10.0f;
    constexpr float kHandPlantHeight = 80.0f;
    // Thin probe box matching faith-runner column_top: catches 0..5 uu thin chain-link fences and
    // vertical collision sheets whose triangles have zero horizontal cap area.
    constexpr Vec3 kTopProbeExtent(2.5f, 2.5f, 0.5f);
    const float z_start = feet.z + max_rise + 10.0f;
    const float z_end = feet.z + std::max(0.0f, min_rise - 2.0f);

    constexpr int kSamples = 6;
    for (int i = 0; i < kSamples; ++i) {
        const float h = min_rise + (max_rise - min_rise) * (static_cast<float>(i) + 0.5f) / kSamples;
        const WallFace wall = probe_wall(dir, reach, h, scene);
        if (!wall.found) continue;

        const Vec3 into = -wall.normal;
        for (float extra : {2.0f, 8.0f, 20.0f, 40.0f}) {
            Vec3 column = wall.point + into * extra;
            float top_z = 0.0f;
            bool got_top = false;
            int32_t top_actor = -1;
            Vec3 top_normal(0.0f, 0.0f, 1.0f);
            const TraceHit top = trace_ray(Vec3(column.x, column.y, z_start),
                                           Vec3(column.x, column.y, z_end), scene);
            if (top.hit && top.normal.z >= kWalkableFloorZ) {
                top_z = top.point.z;
                top_normal = top.normal;
                top_actor = top.actor_index;
                got_top = true;
            } else {
                const TraceHit box_top = sweep_box(Vec3(column.x, column.y, z_start),
                                                   kTopProbeExtent,
                                                   Vec3(0.0f, 0.0f, z_end - z_start), scene);
                if (box_top.hit && !box_top.start_penetrating && box_top.normal.z >= kWalkableFloorZ) {
                    top_z = box_top.point.z - kTopProbeExtent.z;
                    top_actor = box_top.actor_index;
                    got_top = true;
                }
            }
            if (!got_top) continue;
            const float rise = top_z - feet.z;
            if (rise < min_rise || rise > max_rise) continue;
            const Vec3 top_pt(column.x, column.y, top_z);
            if (is_hand_move_excluded(top_pt, top_actor, scene) ||
                is_hand_move_excluded(wall.point, -1, scene)) {
                continue;
            }
            const Vec3 hands(column.x, column.y, top_z + 1.0f + 0.5f * kHandPlantHeight);
            if (!box_free(hands, Vec3(kHandPlantWidth, kHandPlantWidth, 0.5f * kHandPlantHeight), scene)) continue;
            out.found = true;
            out.normal = wall.normal;
            out.top_z = top_z;
            out.wall_distance = wall.distance;
            out.top_point = top_pt;
            out.top_normal = top_normal;
            out.actor_index = top_actor;
            return out;
        }
    }
    return out;
}

// TdMove_GrabTransfer (Allowed2DTransferDistance 260, AllowedZTransferDistance 140) and its
// CheckReachableVaultOver. When the pawn hangs from a lip with no room on top, the thing in the way
// can be a rail standing on the lip. Then a jump moves the hands up onto the rail's top and vaults
// over it. Retail does this at a railed slab on the escape roof, recorded twice with identical
// paths (20260920_185901 t=144.19 and 20260926_204019 t=116.45). The slab's top is at 10624, and
// an 8 uu rail stands 16 back from the slab's face, rising 96 above it.
ParkourController::RailTransfer ParkourController::find_rail_transfer(const Vec3& wall_normal, float ledge_z,
                                                                      const LevelScene& scene) const {
    RailTransfer out;
    Vec3 into(-wall_normal.x, -wall_normal.y, 0.0f);
    if (into.length_sq() < 1e-6f) return out;
    into = into.normalized();
    const Vec3& feet = m_telemetry.position;
    constexpr float kAllowedZTransferDistance = 140.0f;
    constexpr float kReach = 120.0f;        // pawn centre -> rail face
    constexpr float kRailMaxWidth = 64.0f;  // anything wider is a top to stand on, not a rail

    // The rail's near face, just above the lip.
    float face_d = -1.0f;
    for (float h : {8.0f, 32.0f, 64.0f}) {
        const Vec3 s(feet.x, feet.y, ledge_z + h);
        const TraceHit w = trace_ray(s, s + into * kReach, scene);
        if (!w.hit || w.start_penetrating || std::abs(w.normal.z) > 0.3f) continue;
        const Vec3 n = horiz(w.normal);
        if (n.length_sq() < 1e-6f || n.normalized().dot(into) > -0.7f) continue;
        const float d = (w.point - s).dot(into);
        if (face_d < 0.0f || d < face_d) face_d = d;
    }
    if (face_d < 0.0f) return out;

    // Its top: within AllowedZTransferDistance of the lip, at least a step above it, and with room
    // for the hands (the same 10 x 10 x 80 box as find_ledge).
    const Vec3 column = feet + into * (face_d + 2.0f);
    constexpr Vec3 kTopProbeExtent(2.5f, 2.5f, 0.5f);
    const float z_start = ledge_z + kAllowedZTransferDistance + 10.0f;
    const float z_end = ledge_z + 1.0f;
    float rail_top = 0.0f;
    int32_t top_actor = -1;
    bool got_top = false;
    const TraceHit top = trace_ray(Vec3(column.x, column.y, z_start), Vec3(column.x, column.y, z_end), scene);
    if (top.hit && !top.start_penetrating && top.normal.z >= kWalkableFloorZ) {
        rail_top = top.point.z;
        top_actor = top.actor_index;
        got_top = true;
    } else {
        const TraceHit box_top = sweep_box(Vec3(column.x, column.y, z_start), kTopProbeExtent,
                                           Vec3(0.0f, 0.0f, z_end - z_start), scene);
        if (box_top.hit && !box_top.start_penetrating && box_top.normal.z >= kWalkableFloorZ) {
            rail_top = box_top.point.z - kTopProbeExtent.z;
            top_actor = box_top.actor_index;
            got_top = true;
        }
    }
    if (!got_top) return out;
    const float rise = rail_top - ledge_z;
    if (rise < kMaxStepHeight || rise > kAllowedZTransferDistance) return out;
    if (is_hand_move_excluded(Vec3(column.x, column.y, rail_top), top_actor, scene)) return out;
    if (!box_free(Vec3(column.x, column.y, rail_top + 41.0f), Vec3(10.0f, 10.0f, 40.0f), scene)) return out;

    // Thin enough to vault: the top has to end within kRailMaxWidth. The far face is then found
    // by tracing back from beyond it.
    float far_d = -1.0f;
    for (float d = face_d + 4.0f; d <= face_d + kRailMaxWidth + 0.5f; d += 4.0f) {
        const Vec3 p = feet + into * d;
        if (!trace_ray(Vec3(p.x, p.y, rail_top + 8.0f), Vec3(p.x, p.y, rail_top - 24.0f), scene).hit) {
            far_d = d;
            break;
        }
    }
    if (far_d < 0.0f) return out;
    {
        Vec3 s = feet + into * far_d;
        s.z = rail_top - 4.0f;
        const TraceHit back = trace_ray(s, s - into * (far_d - face_d), scene);
        if (back.hit && !back.start_penetrating) far_d = (back.point - feet).dot(into);
    }

    // Retail's path, measured against the rail. The transfer ends hanging under the rail's top,
    // with the centre 32 out from its face. The vault rises to an apex with the centre 2 short of
    // the face and the feet 64 under the top. It then carries over to 38 past the far face (the
    // body clear of it), with the feet 95 under the top, or on the floor there if that is higher.
    const Vec3 hang_xy = feet + into * (face_d - kPawnRadius - 2.0f);
    const Vec3 apex_xy = feet + into * (face_d - 2.0f);
    const Vec3 end_xy = feet + into * (far_d + kPawnRadius + 8.0f);
    float end_z = rail_top - 95.0f;
    bool on_floor = false;
    const TraceHit floor = trace_ray(Vec3(end_xy.x, end_xy.y, rail_top),
                                     Vec3(end_xy.x, end_xy.y, rail_top - 240.0f), scene);
    if (floor.hit && floor.normal.z >= kWalkableFloorZ) {
        end_z = std::max(end_z, floor.point.z);
        on_floor = floor.point.z >= end_z - (kMaxStepHeight + kMaxFloorDist);
    }
    if (!has_room_at(Vec3(end_xy.x, end_xy.y, end_z + 1.0f), kPawnHeight, scene)) return out;

    out.found = true;
    out.hang = Vec3(hang_xy.x, hang_xy.y, rail_top - m_config.grab_hang_depth);
    out.apex = Vec3(apex_xy.x, apex_xy.y, rail_top - 64.0f);
    out.end = Vec3(end_xy.x, end_xy.y, end_z);
    out.end_on_floor = on_floor;
    return out;
}

// -----------------------------------------------------------------------------
// Swept Movement (UE3 MoveActor / SlideAlongSurface / APawn::stepUp)
// -----------------------------------------------------------------------------
ParkourController::TraceHit ParkourController::move_swept(const Vec3& delta, float height, float bottom_offset,
                                                          const LevelScene& scene) {
    Capsule cap;
    cap.base = m_telemetry.position;
    cap.radius = kPawnRadius;
    cap.height = height;
    cap.bottom_offset = bottom_offset;
    const TraceHit hit = sweep_capsule(cap, delta, scene);
    m_telemetry.position += delta * (hit.hit ? safe_fraction(hit.fraction, delta.length()) : 1.0f);
    return hit;
}

ParkourController::TraceHit ParkourController::move_and_slide(const Vec3& delta, float height, float bottom_offset,
                                                              const LevelScene& scene) {
    const TraceHit first = move_swept(delta, height, bottom_offset, scene);
    if (!first.hit) return first;
    // The blocked remainder continues along the surface, nudged off it so the next sweep does not
    // re-detect the resting contact.
    Vec3 remaining = delta * (1.0f - first.fraction);
    remaining -= first.normal * remaining.dot(first.normal);
    if (remaining.length_sq() < 1e-6f) return first;
    const TraceHit second = move_swept(remaining + first.normal * 0.01f, height, bottom_offset, scene);
    if (second.hit) {
        // Crease between two surfaces: continue along their intersection line.
        Vec3 crease = first.normal.cross(second.normal);
        if (crease.length_sq() > 1e-6f) {
            crease = crease.normalized();
            const Vec3 rest = crease * (remaining * (1.0f - second.fraction)).dot(crease);
            if (rest.length_sq() > 1e-6f) move_swept(rest, height, bottom_offset, scene);
        }
    }
    return first;
}

// UE3 APawn::physWalking horizontal move: walkable ramps are followed, kerbs / stairs / seams between
// meshes are stepped up (stepUp), walls are slid along.
void ParkourController::walk_move(const Vec3& delta, float height, const LevelScene& scene) {
    Vec3 remaining = delta;
    Vec3 prev_wall_normal(0.0f, 0.0f, 0.0f);
    bool has_prev_wall = false;
    m_walk_blocked = false;
    m_walk_block_actor = -1;
    for (int iter = 0; iter < 3 && remaining.length_sq() > 1e-6f; ++iter) {
        const TraceHit hit = move_swept(remaining, height, 0.0f, scene);
        if (!hit.hit) return;
        remaining *= (1.0f - hit.fraction);
        if (hit.normal.z >= kWalkableFloorZ) {
            // Walkable ramp: continue parallel to the surface.
            remaining -= hit.normal * remaining.dot(hit.normal);
            remaining += hit.normal * 0.01f;
            continue;
        }
        const float z_before = m_telemetry.position.z;
        if (step_up(remaining, height, scene)) {
            if (m_telemetry.position.z - z_before > 8.0f && m_telemetry.move_state == EMovement::MOVE_Walking) {
                m_telemetry.move_state = EMovement::MOVE_AutoStepUp;
                m_state_timer = 0.0f;
            }
            return;
        }
        // UE3 processHitWall / Bump: the move was blocked by a wall.
        if (!m_walk_blocked) {
            m_walk_blocked = true;
            m_walk_block_actor = hit.actor_index;
        }
        // Wall: slide along it and drop the velocity component into it.
        Vec3 n(hit.normal.x, hit.normal.y, 0.0f);
        if (n.length_sq() < 1e-6f) return;
        n = n.normalized();
        remaining -= n * remaining.dot(n);
        remaining.z = 0.0f;
        // UE3 TwoWallAdjust: if sliding along the second wall pushes back into the first blocking wall
        // (acute / right-angle inside corner), halt horizontal motion instead of pushing into wall 0.
        if (has_prev_wall && remaining.dot(prev_wall_normal) < -1e-4f) {
            m_telemetry.velocity.x = 0.0f;
            m_telemetry.velocity.y = 0.0f;
            return;
        }
        prev_wall_normal = n;
        has_prev_wall = true;
        const float vn = m_telemetry.velocity.x * n.x + m_telemetry.velocity.y * n.y;
        if (vn < 0.0f) {
            m_telemetry.velocity.x -= n.x * vn;
            m_telemetry.velocity.y -= n.y * vn;
        }
        // Nothing left to slide (running straight into the wall): stay in contact.
        if (remaining.length_sq() < 1e-4f) return;
        remaining += n * 0.01f;
    }
}

// UE3 APawn::stepUp: move up MaxStepHeight, retry the blocked move at that height, step back down.
bool ParkourController::step_up(const Vec3& delta, float height, const LevelScene& scene) {
    const float len = delta.length();
    if (len < 1e-3f) return false;
    const Vec3 start = m_telemetry.position;
    move_swept(Vec3(0.0f, 0.0f, kMaxStepHeight), height, 0.0f, scene);
    const float rise = m_telemetry.position.z - start.z;
    if (rise < 1.0f) {
        m_telemetry.position = start;
        return false;
    }
    const Vec3 raised = m_telemetry.position;
    const TraceHit fwd = move_swept(delta, height, 0.0f, scene);
    const float advanced = (m_telemetry.position - raised).length();
    if (fwd.hit && advanced < std::min(1.0f, 0.5f * len)) {
        // Still blocked straight away at the raised height: a wall, not a step.
        m_telemetry.position = start;
        return false;
    }
    const TraceHit down = move_swept(Vec3(0.0f, 0.0f, -rise), height, 0.0f, scene);
    if (down.hit && down.normal.z < kWalkableFloorZ) {
        m_telemetry.position = start;
        return false;
    }
    return true;
}

// Free ballistic flight: gravity plus a swept move with sliding.
void ParkourController::integrate_ballistic(float dt, const LevelScene& scene) {
    m_telemetry.velocity.z -= m_config.gravity * dt;
    const TraceHit hit = move_and_slide(m_telemetry.velocity * dt, kPawnHeight, 0.0f, scene);
    if (hit.hit) {
        const float vn = m_telemetry.velocity.dot(hit.normal);
        if (vn < 0.0f) m_telemetry.velocity -= hit.normal * vn;
    }
}

bool ParkourController::find_ledge_top(const Vec3& wall_normal, float max_rise, const LevelScene& scene, float& ledge_z) const {
    Vec3 into(-wall_normal.x, -wall_normal.y, 0.0f);
    if (into.length_sq() < 1e-6f) return false;
    into = into.normalized();
    const Vec3& p = m_telemetry.position;
    // Distance to the wall face, probed low on the wall (its top may already be below the chest).
    float wall_dist = -1.0f;
    for (float h : {90.0f, 50.0f, 20.0f}) {
        const Vec3 s = p + Vec3(0.0f, 0.0f, h);
        const TraceHit w = trace_ray(s, s + into * 120.0f, scene);
        if (w.hit && std::abs(w.normal.z) < 0.5f) {
            wall_dist = 120.0f * w.fraction;
            break;
        }
    }
    if (wall_dist < 0.0f) return false;
    // Walkable top just beyond the wall face (including thin railings/fences via box sweep).
    constexpr Vec3 kTopProbeExtent(2.5f, 2.5f, 0.5f);
    for (float extra : {2.0f, 8.0f, 20.0f, 36.0f}) {
        const Vec3 column = p + into * (wall_dist + extra);
        float top_z = 0.0f;
        int32_t top_actor = -1;
        bool got_top = false;
        const TraceHit h = trace_ray(column + Vec3(0.0f, 0.0f, max_rise + 10.0f), column + Vec3(0.0f, 0.0f, 5.0f), scene);
        if (h.hit && h.normal.z >= kWalkableFloorZ && h.point.z <= p.z + max_rise) {
            top_z = h.point.z;
            top_actor = h.actor_index;
            got_top = true;
        } else {
            const float z_start = p.z + max_rise + 10.0f;
            const float z_end = p.z + 5.0f;
            const TraceHit bh = sweep_box(Vec3(column.x, column.y, z_start), kTopProbeExtent,
                                          Vec3(0.0f, 0.0f, z_end - z_start), scene);
            if (bh.hit && !bh.start_penetrating && bh.normal.z >= kWalkableFloorZ) {
                const float tz = bh.point.z - kTopProbeExtent.z;
                if (tz <= p.z + max_rise) {
                    top_z = tz;
                    top_actor = bh.actor_index;
                    got_top = true;
                }
            }
        }
        if (!got_top) continue;
        if (is_hand_move_excluded(Vec3(column.x, column.y, top_z), top_actor, scene)) continue;
        if (!has_room_at(Vec3(column.x, column.y, top_z + 0.5f), kPawnHeight, scene)) continue;
        ledge_z = top_z;
        return true;
    }
    return false;
}

bool ParkourController::climb_onto_ledge(const Vec3& wall_normal, float ledge_z, const LevelScene& scene) {
    Vec3 into(-wall_normal.x, -wall_normal.y, 0.0f);
    if (into.length_sq() < 1e-6f) return false;
    into = into.normalized();
    const Vec3 start = m_telemetry.position;
    // Distance to the wall face just below the ledge lip.
    float wall_dist = 40.0f;
    {
        const Vec3 s(start.x, start.y, std::max(start.z + 5.0f, ledge_z - 8.0f));
        const TraceHit w = trace_ray(s, s + into * 120.0f, scene);
        if (w.hit) wall_dist = 120.0f * w.fraction;
    }
    // Rise until the feet clear the ledge, then move over it.
    const float rise = ledge_z + 2.0f - start.z;
    if (rise > 0.0f) {
        move_swept(Vec3(0.0f, 0.0f, rise), kPawnHeight, 0.0f, scene);
        if (m_telemetry.position.z < ledge_z - 0.5f) {
            m_telemetry.position = start;  // no headroom above the ledge
            return false;
        }
    }
    move_swept(into * (wall_dist + kPawnRadius + 6.0f), kPawnHeight, 0.0f, scene);
    return true;
}

// -----------------------------------------------------------------------------
// TdPawn ground model (native ATdPawn::GetSprintAcceleration / GetWalkAcceleration /
// CalcVelocity and TdPlayerController.PlayerWalking.PlayerMove)
// -----------------------------------------------------------------------------
Vec3 ParkourController::facing_forward() const {
    return Rotator::from_degrees(0.0f, m_telemetry.yaw_deg, 0.0f).forward();
}

Vec3 ParkourController::facing_right() const {
    return Rotator::from_degrees(0.0f, m_telemetry.yaw_deg, 0.0f).right();
}

Vec3 ParkourController::input_direction(const InputFrame& input) const {
    const Vec3 d = facing_forward() * input.forward + facing_right() * input.strafe;
    if (d.length_sq() < 1e-8f) return Vec3(0.0f, 0.0f, 0.0f);
    return d.normalized();
}

// Speed that rises `height` under the pawn's gravity. The scripts write this as
// Gravity * 2 * Sqrt(Height / Gravity) with Gravity = |GetGravityZ()| = 800.
float ParkourController::speed_for_height(float height) const {
    return std::sqrt(2.0f * m_config.gravity * std::max(height, 0.0f));
}

// ATdPawn::GetSprintAcceleration: the acceleration curve along the input direction, steering onto
// it, friction compensation (so sprinting straight follows the speed curve exactly) and a damping
// term for how fast the view turns.
Vec3 ParkourController::sprint_acceleration(const Vec3& dir, const Vec3& vel, float dt, bool falling, float turn_uu) {
    const MovementConfig& c = m_config;
    const Vec3 h = horiz(vel);
    const float speed = h.length();
    if (dir.length_sq() < 1e-8f) {
        m_sprint_energy = 0.0f;
        return Vec3(0.0f, 0.0f, 0.0f);
    }
    m_sprint_energy = std::max(0.0f, speed - c.speed_max_base_velocity);
    Vec3 accel = dir * m_accel_curve.eval(speed);
    if (!falling) {
        // Steer the velocity onto the input direction, keeping its speed.
        const Vec3 diff = dir * speed - h;
        const float d = diff.length();
        if (d > 1e-4f) {
            const float steer = std::min(c.sprint_accel_factor * d, dt > 0.0f ? d / dt : 0.0f);
            accel += diff * (steer / d);
        }
        accel += h * (c.ground_friction * 0.1f);
    }
    accel -= h * (c.turn_decel_factor * turn_uu / 32768.0f);
    return round_accel(accel);
}

// ATdPawn::GetWalkAcceleration: aim for SpeedMinBaseVelocity along the input plus
// (SpeedMaxBaseVelocity - SpeedMinBaseVelocity + sprint energy) scaled by the stick, per axis, and
// close the gap at the walk (forward) and strafe rates. Sprint energy drains unless you keep
// pushing the way you face.
Vec3 ParkourController::walk_acceleration(const Vec3& dir, const InputFrame& input, const Vec3& vel, bool falling) {
    const MovementConfig& c = m_config;
    if (dir.length_sq() < 1e-8f) {
        m_sprint_energy = 0.0f;
        return Vec3(0.0f, 0.0f, 0.0f);
    }
    const Vec3 fwd = facing_forward();
    const Vec3 right = facing_right();
    if (m_sprint_energy > 0.0f) {
        const float along = std::clamp(dir.dot(fwd), 0.0f, 0.9f);
        const float drain = (c.ground_speed - c.speed_max_base_velocity) *
                            (1.0f - std::pow(along, c.energy_decel_exponent)) / c.energy_decel_time;
        m_sprint_energy = std::max(0.0f, m_sprint_energy - drain * m_frame_dt);
    }
    const float top = c.speed_max_base_velocity - c.speed_min_base_velocity + m_sprint_energy;
    const float want_f = dir.dot(fwd) * c.speed_min_base_velocity + top * input.forward;
    const float want_s = dir.dot(right) * c.speed_min_base_velocity + top * input.strafe;
    const float have_f = vel.dot(fwd);
    const float have_s = vel.dot(right);
    Vec3 accel = fwd * ((want_f - have_f) * c.walk_accel_factor) + right * ((want_s - have_s) * c.strafe_accel_factor);
    if (!falling) {
        accel += horiz(vel) * (c.ground_friction * 0.1f);
    }
    return round_accel(accel);
}

// TdPlayerController.PlayerWalking.PlayerMove: sprint acceleration when pushing forward hard
// (aForward > InputMaxSprintHeightLimit and the stick past InputMaxSprintRaduisLimit) and moving
// that way, walk acceleration otherwise. Letting go within 0.15 s of starting stops you dead for
// 0.25 s at 35 uu/s (the tap-stop). There is no sprint button in Mirror's Edge.
//
// PlayerMove runs once per frame, before the pawn's physics: it hands the pawn one Acceleration
// (ProcessMove) built from the frame's DeltaTime, aTurn and the velocity the frame starts with,
// and physWalking / physFalling then sub-iterate the whole frame with it. So the first is
// evaluated on the first call of a frame and held for the rest of that frame's sub-steps.
// Re-evaluating it every 1/120 s sub-step made a start from rest take walk acceleration
// (7 x 400 = 2800 uu/s^2, sprint needs Velocity . wish > 0) for only the first 8 ms: the edge_pt1
// and escape recordings leave rest at 2800 x the frame's dt (46.7 uu/s at 60 fps) where the
// port reached 31.6, and 309 uu/s on a 0.11 s hitch frame where it reached 114. It also drained
// the sprint energy (walk_acceleration, per frame dt) once per sub-step.
Vec3 ParkourController::controller_acceleration(const InputFrame& input, bool falling) {
    if (m_frame_accel_valid) return m_frame_accel;
    m_frame_accel_valid = true;
    const MovementConfig& c = m_config;
    const float dt = m_frame_dt;
    Vec3& vel = m_telemetry.velocity;
    if (!falling && m_stop_timer > 0.0f) {
        m_stop_timer -= dt;
        const Vec3 d = horiz(vel);
        const Vec3 stopped = (d.length_sq() > 1e-6f) ? d.normalized() * 35.0f : Vec3(0.0f, 0.0f, 0.0f);
        vel.x = stopped.x;
        vel.y = stopped.y;
        m_frame_accel = Vec3(0.0f, 0.0f, 0.0f);
        return m_frame_accel;
    }
    // PlayerMove clamps the stick to the unit circle (InputSize > 1: aForward and aStrafe divided by
    // it), so two keys give 0.707 per axis, not 1 - walk_acceleration's per-axis targets are then
    // 400 x 0.707. In the 2026-09-26 20:40 run, adding D to a 348 uu/s backpedal turns retail's
    // velocity onto the diagonal at near-constant speed, (-347.8, 0) -> (-340.0, 48.9) -> (-333.4,
    // 87.4) in the facing frame: 7 x and 10 x the gap to (-282.8, 282.8) per second. The raw keys
    // aimed the port at (-397.1, 397.1) and sped it up to 486 uu/s: (-353.3, 67.0) -> (-358.3, 121.0).
    InputFrame stick = input;
    const float size = std::sqrt(stick.forward * stick.forward + stick.strafe * stick.strafe);
    if (size > 1.0f) {
        stick.forward /= size;
        stick.strafe /= size;
    }
    const Vec3 dir = input_direction(stick);
    if (dir.length_sq() > 0.0f) {
        if (!falling) m_accel_time += dt;
        const bool sprint = stick.forward > c.sprint_input_threshold && std::min(size, 1.0f) > c.sprint_input_threshold &&
                            horiz(vel).dot(dir) > 0.0f;
        m_frame_accel = sprint ? sprint_acceleration(dir, vel, dt, falling, m_frame_turn_uu)
                               : walk_acceleration(dir, stick, vel, falling);
        return m_frame_accel;
    }
    m_frame_accel = walk_acceleration(dir, stick, vel, falling);
    if (!falling) {
        if (m_accel_time > 0.0f && m_accel_time < 0.15f) {
            m_stop_timer = 0.25f;
            const Vec3 d = horiz(vel);
            const Vec3 stopped = (d.length_sq() > 1e-6f) ? d.normalized() * 35.0f : Vec3(0.0f, 0.0f, 0.0f);
            vel.x = stopped.x;
            vel.y = stopped.y;
        }
        m_accel_time = 0.0f;
    }
    return m_frame_accel;
}

// ATdPawn::CalcVelocity for walking: `speed_mod` is the move's SpeedModifier (TdMove_Crouch 0.2),
// `friction` is GroundFriction x the move's FrictionModifier. Accelerating takes velocity x
// friction x 0.1 off per second; braking runs in steps of at most 0.03 s at 2 x friction x
// BrakingFrictionStrength, averaged over the steps, and stops dead once reversed or slow.
void ParkourController::calc_velocity(const Vec3& accel_in, float dt, float speed_mod, float friction) {
    const MovementConfig& c = m_config;
    const float max_accel = c.accel_rate * speed_mod;
    const float max_speed = c.ground_speed * speed_mod;
    Vec3 accel = horiz(accel_in);
    if (accel.length_sq() > max_accel * max_accel) accel = accel.normalized() * max_accel;

    Vec3 v = horiz(m_telemetry.velocity);
    if (accel.length_sq() > 0.0f) {
        v -= v * (dt * friction * 0.1f);
    } else {
        const Vec3 before = v;
        float left = dt;
        Vec3 avg(0.0f, 0.0f, 0.0f);
        while (left > 0.0f) {
            const float step = std::min(left, 0.03f);
            left -= step;
            v -= v * (2.0f * step * friction * c.braking_friction_strength);
            if (v.dot(before) > 0.0f && dt > 0.0f) {
                avg += v * (step / dt);
            }
        }
        v = avg;
        if (v.dot(before) < 0.0f || v.length_sq() < 10.0f * 10.0f) {
            v = Vec3(0.0f, 0.0f, 0.0f);
        }
    }
    v += accel * dt;
    if (v.length_sq() > max_speed * max_speed) v = v.normalized() * max_speed;
    m_telemetry.velocity.x = v.x;
    m_telemetry.velocity.y = v.y;
}

void ParkourController::set_stance(float eye_height) {
    m_telemetry.eye_height = eye_height;
}

// TdPawn.CanSkillRoll: a crouch press within the last RollTriggerWindow (0.2 s).
bool ParkourController::can_skill_roll() const {
    const float age = m_telemetry.sim_time - m_roll_trigger_time;
    return age >= 0.0f && age < m_config.roll_trigger_window;
}

// The pawn leaves the floor into `air_move`: fall tracking starts here (TdMove_Falling.StartMove
// records EnterFallingHeight, the peak the landing height is measured from).
void ParkourController::leave_ground(EMovement air_move) {
    m_telemetry.grounded = false;
    m_base_actor = -1;
    m_air_fall_start_z = m_telemetry.position.z;
    m_fall_peak_z = m_telemetry.position.z;
    m_takeoff_move = air_move;
    m_telemetry.move_state = air_move;
    m_state_timer = 0.0f;
    m_coil_timer = 0.0f;
}

// TdMove_Jump.StartMove: PreJumpMomentum, JumpAddXY along the facing when already moving that
// way, BaseJumpZ (BaseJumpZHeavy with a heavy weapon), LastJumpLocation. Then the ledge assist: a
// ledge less than LedgeAssistHeight above the feet that the arc would fall short of gets the jump
// aimed onto it (the game flies the pawn there; here the take-off speed is raised to reach it).
void ParkourController::start_jump(const LevelScene& scene) {
    const MovementConfig& c = m_config;
    const Vec3 fwd = facing_forward();
    Vec3 vel = m_telemetry.velocity;
    m_pre_jump_momentum = vel.length_xy();
    if (fwd.dot(vel) > 10.0f) {
        vel += fwd * c.jump_add_xy;
    }
    vel.z = m_telemetry.weapon.is_heavy ? c.base_jump_z_heavy : c.base_jump_z;

    const Vec3 h = horiz(vel);
    const float speed = h.length();
    if (speed > 50.0f) {
        const Vec3 dir = h.normalized();
        const Ledge ledge = find_ledge(dir, std::min(300.0f, speed * 0.6f), kMaxStepHeight, c.ledge_assist_height, scene);
        if (ledge.found && dir.dot(-ledge.normal) > 0.7071f) {
            const float rise = ledge.top_z - m_telemetry.position.z;
            float dist = std::max(0.0f, ledge.wall_distance - kPawnRadius - 8.0f);
            const float t = dist / std::max(speed, 1.0f);
            const float z_at = vel.z * t - 0.5f * c.gravity * t * t;
            if (z_at < rise && has_room_at(ledge.top_point, kPawnHeight, scene)) {
                vel.z = std::max(vel.z, speed_for_height(rise + 4.0f));
            }
        }
    }

    m_telemetry.velocity = vel;
    m_last_jump_location = m_telemetry.position;
    set_stance(kEyeHeightStand);
    // TdMove_Jump.StartJump: a long jump (JumpFast) is one with nothing to land on within 200 below
    // the point 1.1 x her speed ahead.
    {
        const Vec3 ahead = m_telemetry.position + fwd * (1.1f * m_pre_jump_momentum) + Vec3(0.0f, 0.0f, 10.0f);
        m_telemetry.jump_over_gap = !trace_ray(ahead, ahead - Vec3(0.0f, 0.0f, 210.0f), scene).hit;
    }
    leave_ground(EMovement::MOVE_Jump);
}

// TdMove_DodgeJump: strafing hard (MoveActionHint left / right at bMoveActionMax) when jumping
// hops sideways: +-600 sideways plus 0.3 of the old velocity, 300 up. No air control, no grabs.
bool ParkourController::try_initiate_dodge_jump(const InputFrame& input) {
    if (std::abs(input.strafe) <= 0.96f) return false;
    const MovementConfig& c = m_config;
    const Vec3 side = facing_right() * sign_of(input.strafe);
    Vec3 vel = side * c.dodge_jump_side_speed + m_telemetry.velocity * c.dodge_jump_inertia;
    vel.z = c.dodge_jump_z;
    m_pre_jump_momentum = m_telemetry.velocity.length_xy();
    m_telemetry.velocity = vel;
    m_last_jump_location = m_telemetry.position;
    set_stance(kEyeHeightStand);
    m_telemetry.move_left = input.strafe < 0.0f;
    leave_ground(EMovement::MOVE_DodgeJump);
    return true;
}

// TdMove_Landing.StartMove: FallingHeight = EnterFallingHeight - Z decides LandHard (>= 530),
// SkillRoll (>= 200 with a fresh crouch press, not backwards), SoftLanding (>= 300, animation
// only) or a plain landing. Landing a plain jump caps the speed at PreJumpMomentum -
// LandingSpeedReduction (SubtractLandingSpeed). Out of a 180 in the air you land on your back.
bool ParkourController::is_soft_landing_surface(const FloorHit& floor, const LevelScene& scene) const {
    if (floor.normal.z <= 0.9f) return false;
    if (floor.actor_index >= 0 && floor.actor_index < static_cast<int32_t>(scene.actors.size())) {
        if (scene.actors[floor.actor_index].is_soft_landing) return true;
    }
    const Vec3 foot(m_telemetry.position.x, m_telemetry.position.y, floor.z);
    for (const auto& act : scene.actors) {
        if (!act.is_soft_landing) continue;
        if (foot.x >= act.world_bounds.min_pt.x - 50.0f && foot.x <= act.world_bounds.max_pt.x + 50.0f &&
            foot.y >= act.world_bounds.min_pt.y - 50.0f && foot.y <= act.world_bounds.max_pt.y + 50.0f &&
            foot.z >= act.world_bounds.min_pt.z - 50.0f && foot.z <= act.world_bounds.max_pt.z + 60.0f) {
            return true;
        }
    }
    return false;
}

bool ParkourController::has_soft_landing_below(const LevelScene& scene) const {
    const Vec3 pos = m_telemetry.position;
    const Vec3 vel = m_telemetry.velocity;
    const float gravity = std::max(100.0f, m_config.gravity);

    // Native UTdPhysicsMove::Tick (0x1206df0) + 0x11f9970: 2.0s ballistic parabola trace onto a
    // surface with bEnableSoftLanding == True more than 2 * CylinderHeight (180 uu) below the pawn.
    for (const auto& act : scene.actors) {
        if (!act.is_soft_landing) continue;
        const float top_z = act.world_bounds.max_pt.z;
        const float dz = pos.z - top_z;
        if (dz < -60.0f || dz > 3800.0f) continue;

        const float dz_pos = std::max(0.0f, dz);
        const float disc = std::max(0.0f, vel.z * vel.z + 2.0f * gravity * dz_pos);
        const float t_fall = std::clamp((vel.z + std::sqrt(disc)) / gravity, 0.0f, 2.0f);
        const Vec3 pred(pos.x + vel.x * t_fall, pos.y + vel.y * t_fall, top_z);

        const float pad = 120.0f;
        if (pred.x >= act.world_bounds.min_pt.x - pad && pred.x <= act.world_bounds.max_pt.x + pad &&
            pred.y >= act.world_bounds.min_pt.y - pad && pred.y <= act.world_bounds.max_pt.y + pad) {
            return true;
        }
    }
    return false;
}

bool ParkourController::is_hand_move_excluded(const Vec3& pt, int32_t actor_index, const LevelScene& scene) const {
    if (actor_index >= 0 && static_cast<size_t>(actor_index) < scene.actors.size()) {
        if (scene.actors[static_cast<size_t>(actor_index)].exclude_hand_moves) return true;
    }
    for (const auto& act : scene.actors) {
        if (!act.exclude_hand_moves) continue;
        if (!act.is_movement_exclusion_volume && !act.is_electric_volume && !act.is_barbed_wire_volume) continue;
        if (act.world_bounds.min_pt.x > act.world_bounds.max_pt.x) continue;
        const float pad_xy = (act.is_electric_volume || act.is_barbed_wire_volume) ? 28.0f : 8.0f;
        const float pad_z  = (act.is_electric_volume || act.is_barbed_wire_volume) ? 40.0f : 12.0f;
        if (pt.x >= act.world_bounds.min_pt.x - pad_xy && pt.x <= act.world_bounds.max_pt.x + pad_xy &&
            pt.y >= act.world_bounds.min_pt.y - pad_xy && pt.y <= act.world_bounds.max_pt.y + pad_xy &&
            pt.z >= act.world_bounds.min_pt.z - pad_z  && pt.z <= act.world_bounds.max_pt.z + pad_z) {
            return true;
        }
    }
    return false;
}

bool ParkourController::is_foot_move_excluded(const Vec3& pt, int32_t actor_index, const LevelScene& scene) const {
    if (actor_index >= 0 && static_cast<size_t>(actor_index) < scene.actors.size()) {
        if (scene.actors[static_cast<size_t>(actor_index)].exclude_foot_moves) return true;
    }
    for (const auto& act : scene.actors) {
        if (!act.exclude_foot_moves) continue;
        if (!act.is_movement_exclusion_volume && !act.is_electric_volume && !act.is_barbed_wire_volume) continue;
        if (act.world_bounds.min_pt.x > act.world_bounds.max_pt.x) continue;
        const float pad_xy = (act.is_electric_volume || act.is_barbed_wire_volume) ? 28.0f : 8.0f;
        const float pad_z  = (act.is_electric_volume || act.is_barbed_wire_volume) ? 40.0f : 12.0f;
        if (pt.x >= act.world_bounds.min_pt.x - pad_xy && pt.x <= act.world_bounds.max_pt.x + pad_xy &&
            pt.y >= act.world_bounds.min_pt.y - pad_xy && pt.y <= act.world_bounds.max_pt.y + pad_xy &&
            pt.z >= act.world_bounds.min_pt.z - pad_z  && pt.z <= act.world_bounds.max_pt.z + pad_z) {
            return true;
        }
    }
    return false;
}

// TdPawn.UpdateSpecialDamage (TdDmgType_ElectricShock: DamageImpulse = 300, DamageZDirection = 0.2)
// and TdBarbedWireVolume.Touch: touching an active electric fence PhysicsVolume or barbed-wire volume
// aborts any parkour move into MOVE_Falling, shocks Faith, and knocks her away from the fence.
void ParkourController::check_hazard_volumes(float /*dt*/, const LevelScene& scene) {
    if (m_telemetry.intro_active || m_telemetry.falling_to_death || m_telemetry.health <= 0.0f) return;
    if (m_hazard_shock_cooldown > 0.0f) return;

    const EMovement st = m_telemetry.move_state;
    const bool low = (st == EMovement::MOVE_Crouch || st == EMovement::MOVE_Slide ||
                      st == EMovement::MOVE_MeleeSlide || st == EMovement::MOVE_SkillRoll ||
                      st == EMovement::MOVE_Coil);
    const float h = low ? kCrouchHeight : kPawnHeight;
    const AABB pawn_box(
        m_telemetry.position - Vec3(kPawnRadius + 6.0f, kPawnRadius + 6.0f, 0.0f),
        m_telemetry.position + Vec3(kPawnRadius + 6.0f, kPawnRadius + 6.0f, h));

    for (const auto& vol : scene.actors) {
        if (!vol.is_electric_volume && !vol.is_barbed_wire_volume) continue;
        if (vol.world_bounds.min_pt.x > vol.world_bounds.max_pt.x) continue;
        if (!pawn_box.intersects(vol.world_bounds)) continue;

        m_hazard_shock_cooldown = 0.65f;
        const float dmg = std::max(15.0f, vol.damage_per_sec);
        m_telemetry.health = std::max(0.0f, m_telemetry.health - dmg);
        m_telemetry.damage_flash_timer = 0.35f;
        m_damage_cooldown = m_config.health_regen_delay;

        const Vec3 vol_center = (vol.world_bounds.min_pt + vol.world_bounds.max_pt) * 0.5f;
        const Vec3 vol_ext = (vol.world_bounds.max_pt - vol.world_bounds.min_pt) * 0.5f;
        Vec3 push_dir;
        if (vol_ext.x < vol_ext.y * 0.5f) {
            push_dir = Vec3((m_telemetry.position.x >= vol_center.x) ? 1.0f : -1.0f, 0.0f, 0.0f);
        } else if (vol_ext.y < vol_ext.x * 0.5f) {
            push_dir = Vec3(0.0f, (m_telemetry.position.y >= vol_center.y) ? 1.0f : -1.0f, 0.0f);
        } else {
            push_dir = horiz(m_telemetry.position - vol_center);
            if (push_dir.length_sq() < 1e-4f) push_dir = -facing_forward();
            else push_dir = push_dir.normalized();
        }

        constexpr float kShockImpulse = 300.0f;
        m_telemetry.velocity = push_dir * kShockImpulse + Vec3(0.0f, 0.0f, 160.0f);
        m_sprint_energy = 0.0f;
        m_ignore_move_input = std::max(m_ignore_move_input, 0.35f);
        m_telemetry.camera_roll_deg = 0.0f;
        leave_ground(EMovement::MOVE_Falling);
        set_stance(kEyeHeightStand);
        move_swept(push_dir * 8.0f, kPawnHeight, 0.0f, scene);
        break;
    }
}

void ParkourController::update_fall_height_volumes(const LevelScene& scene) {
    const AABB pawn_box(
        m_telemetry.position - Vec3(kPawnRadius, kPawnRadius, 0.0f),
        m_telemetry.position + Vec3(kPawnRadius, kPawnRadius, kPawnHeight));
    for (const auto& act : scene.actors) {
        if (!act.is_fall_height_volume) continue;
        if (act.world_bounds.intersects(pawn_box)) {
            m_fall_peak_z = act.fall_height_target_z;
            if (m_fall_peak_z - m_telemetry.position.z < m_config.uncontrolled_fall) {
                m_telemetry.falling_to_death = false;
            }
        }
    }
}

void ParkourController::land(const FloorHit& floor, const LevelScene& scene) {
    const MovementConfig& c = m_config;
    const float fall = std::max(0.0f, m_fall_peak_z - floor.z);
    const bool soft_surface = is_soft_landing_surface(floor, scene);
    const EMovement air_move = m_telemetry.move_state;
    const Vec3 fwd = facing_forward();
    Vec3 h = horiz(m_telemetry.velocity);
    float speed = h.length();

    m_consecutive_wallruns = 0;
    m_wall_turned = false;
    m_coil_timer = 0.0f;
    m_last_wallrun_normal = Vec3(0.0f, 0.0f, 0.0f);
    m_illegal_wall_timer = 0.0f;
    m_telemetry.velocity.z = 0.0f;
    m_telemetry.grounded = true;
    m_air_fall_start_z = floor.z;
    m_fall_peak_z = floor.z;

    // TdPlayerPawn.TakeFallingDamage / TdMove_Landing.LandOnSoftObject:
    // Landing on a soft object cancels all fall damage and lethal fall death regardless of drop
    // height and plays FallingLandSoftLanding in MOVE_Landing (with ResetCameraLook(0.3)).
    if (soft_surface && fall >= c.hard_landing_min_fall) {
        m_telemetry.falling_to_death = false;
        m_telemetry.fall_death_impact = false;
        m_telemetry.move_state = EMovement::MOVE_Landing;
        m_landing_timer = 1.50f;
        m_state_timer = 0.0f;
        m_telemetry.velocity = Vec3(0.0f, 0.0f, 0.0f);
        m_sprint_energy = 0.0f;
        m_telemetry.camera_roll_deg = 0.0f;
        set_move_anim("FallingLandSoftLanding");
        set_stance(kEyeHeightStand);
        return;
    }

    if (fall >= c.uncontrolled_fall) {
        // TdPawn.UncontrolledFall: lethal fall impact -> play FallingLandDie animation + impact SFX -> fade to black -> respawn.
        m_telemetry.health = 0.0f;
        m_telemetry.falling_to_death = true;
        m_telemetry.fall_death_impact = true;
        m_telemetry.move_state = EMovement::MOVE_Landing;
        m_landing_timer = c.hard_landing_time;
        m_state_timer = 0.0f;
        m_death_total_duration = 1.35f;
        m_death_timer = m_death_total_duration;
        m_telemetry.death_anim_progress = 0.0f;
        m_telemetry.damage_flash_timer = 0.55f;
        m_telemetry.velocity = Vec3(0.0f, 0.0f, 0.0f);
        m_sprint_energy = 0.0f;
        set_stance(28.0f);
        m_telemetry.camera_roll_deg = 38.0f;
        return;
    }

    if (m_turned_in_air || air_move == EMovement::MOVE_180TurnInAir) {
        // LandBackwards: dead stop, on your back (TdMove_LayOnGround).
        m_turned_in_air = false;
        m_turn_timer = 0.0f;
        m_telemetry.velocity = Vec3(0.0f, 0.0f, 0.0f);
        m_telemetry.move_state = EMovement::MOVE_LayOnGround;
        m_landing_timer = c.lay_on_ground_time;
        m_sprint_energy = 0.0f;
        set_stance(kEyeHeightSlide);
        return;
    }

    const bool backwards = speed > 1.0f && h.dot(fwd) < 0.0f;
    if (fall >= c.skill_roll_min_fall && can_skill_roll() && !backwards && air_move != EMovement::MOVE_MeleeAir) {
        // TdMove_SkillRoll.StartMove: Velocity = Acceleration = 0 and root motion from here on. The
        // landing speed is dropped; the pawn rolls fallinglandroll's 313.5 uu along the way the body
        // faces, and the move lasts the whole 1.27 s animation (update_landing_moves).
        m_telemetry.velocity = Vec3(0.0f, 0.0f, 0.0f);
        m_telemetry.move_state = EMovement::MOVE_SkillRoll;
        m_landing_timer = kSkillRollLength;
        m_state_timer = 0.0f;
        m_telemetry.combat_anim_time = 0.0f;  // fallinglandroll's clock (viewmodel and camera animation)
        m_roll_trigger_time = -100.0f;
        m_roll_dir = Rotator::from_degrees(0.0f, m_pawn_yaw, 0.0f).forward();
        m_sprint_energy = 0.0f;
        // The camera rides the animation's EyeJoint (AnimSystem::camera_animation), blending down from
        // and back up to the standing eyes.
        set_stance(kEyeHeightStand);
        return;
    }

    if (fall >= c.hard_landing_min_fall) {
        // TdMove_Landing.LandHard: dead stop, no input until the animation ends, damage.
        m_telemetry.move_state = EMovement::MOVE_Landing;
        m_landing_timer = c.hard_landing_time;
        m_state_timer = 0.0f;
        apply_damage(15.0f, 2, Vec3(0.0f, 0.0f, 0.0f));
        m_telemetry.velocity = Vec3(0.0f, 0.0f, 0.0f);
        m_sprint_energy = 0.0f;
        set_stance(kEyeHeightCrouch);
        return;
    }

    // SubtractLandingSpeed: a plain jump (or the coil / air kick out of one) lands at most
    // PreJumpMomentum - LandingSpeedReduction.
    if (m_takeoff_move == EMovement::MOVE_Jump &&
        (air_move == EMovement::MOVE_Jump || air_move == EMovement::MOVE_Falling || air_move == EMovement::MOVE_Coil ||
         air_move == EMovement::MOVE_MeleeAir)) {
        const float cap = std::max(0.0f, m_pre_jump_momentum - c.landing_speed_reduction);
        if (speed > cap) {
            h = (speed > 1e-4f) ? h * (cap / speed) : Vec3(0.0f, 0.0f, 0.0f);
            speed = cap;
            m_telemetry.velocity.x = h.x;
            m_telemetry.velocity.y = h.y;
        }
    }
    m_sprint_energy = std::max(0.0f, speed - c.speed_max_base_velocity);

    // TdMove_Landing.StartMove: ordinary medium landings on normal ground transition directly to
    // MOVE_Walking so Director::tick() triggers LandNormal (FallingLandMedium + TdAnimNodeLandOffset).
    m_telemetry.move_state = EMovement::MOVE_Walking;
    set_stance(kEyeHeightStand);
}

// -----------------------------------------------------------------------------
// Ground Locomotion: TdMove_Walking / TdMove_Crouch / TdMove_180Turn + the ground jump chain
// -----------------------------------------------------------------------------
void ParkourController::update_ground_locomotion(const InputFrame& input, float dt, const LevelScene& scene) {
    if (try_initiate_zipline(scene)) return;
    if (try_initiate_climb(input, scene)) return;
    if (try_initiate_balance(scene)) return;
    if (try_initiate_ledge_walk(scene)) return;

    const MovementConfig& c = m_config;
    EMovement& st = m_telemetry.move_state;
    const bool combat = (st == EMovement::MOVE_Melee || st == EMovement::MOVE_Barge);
    const bool turning = (st == EMovement::MOVE_180Turn);
    bool crouched = (st == EMovement::MOVE_Crouch);

    if (turning && m_turn_timer <= 0.0f) {
        st = EMovement::MOVE_Walking;
    }

    // Jump (TdPlayerMoveManager, ground: DodgeJump -> SpringBoard -> SpeedVault -> Jump). A crouched
    // pawn stands up first; under a low ceiling the press is dropped.
    if (jump_pressed() && !combat && !turning) {
        consume_jump();
        if (!crouched || has_room(kPawnHeight, scene)) {
            if (crouched) {
                crouched = false;
                st = EMovement::MOVE_Walking;
                set_stance(kEyeHeightStand);
            }
            if (try_initiate_dodge_jump(input)) return;
            if (try_initiate_springboard(input, scene)) return;
            if (try_initiate_vault(input, scene)) return;
            start_jump(scene);
            return;
        }
    }

    // Crouch press (TdPlayerMoveManager: Slide when TdMove_Slide.CanDoMove, else Crouch).
    if ((m_crouch_pressed || (crouched && input.melee && m_melee_cooldown <= 0.0f)) && !combat && !turning) {
        const bool was_crouch_press = m_crouch_pressed && !crouched;
        if (was_crouch_press) m_crouch_pressed = false;
        const Vec3 fwd = facing_forward();
        const float along = m_telemetry.velocity.dot(fwd);
        FloorHit floor;
        bool steep = false;
        if (check_ground(scene, kMaxFloorDist + 1.0f, kPawnHeight, floor)) {
            const Vec3 h = horiz(m_telemetry.velocity);
            if (h.length_sq() > 1.0f) {
                // Incline along the velocity: uphill slopes steeper than 0.5 refuse the slide.
                const Vec3 d = h.normalized();
                steep = (-(floor.normal.dot(d)) / std::max(floor.normal.z, 0.1f)) > 0.5f;
            }
        }
        bool door_slide_kick = false;
        if (input.melee && m_melee_cooldown <= 0.0f && (along >= 40.0f || input.forward > 0.1f) && !steep) {
            Vec3 hit_pt;
            const Vec3 h_vel = horiz(m_telemetry.velocity);
            const Vec3 move_dir = (h_vel.length_sq() > 1.0f) ? h_vel.normalized() : fwd;
            if (find_barge_door(scene, move_dir, 220.0f, hit_pt) >= 0 ||
                find_barge_door(scene, fwd, 220.0f, hit_pt) >= 0) {
                door_slide_kick = true;
                if (along < c.slide_min_speed) {
                    m_telemetry.velocity.x = move_dir.x * c.slide_min_speed;
                    m_telemetry.velocity.y = move_dir.y * c.slide_min_speed;
                }
            }
        }
        if ((was_crouch_press && along >= c.slide_min_speed && !steep) || door_slide_kick) {
            st = EMovement::MOVE_Slide;
            m_slide_timer = 0.0f;
            m_state_timer = 0.0f;
            m_slide_yaw = yaw_of(horiz(m_telemetry.velocity));
            m_barge_speed = m_telemetry.velocity.length_xy();
            m_barge_dealt_damage = false;
            m_melee_cooldown = 0.0f;
            set_stance(kEyeHeightSlide);
            return;
        }
    }

    // Stance: crouch while held (or while there is no room to stand up).
    if (!combat && !turning) {
        if (input.crouch || (crouched && !has_room(kPawnHeight, scene))) {
            st = EMovement::MOVE_Crouch;
            crouched = true;
            set_stance(kEyeHeightCrouch);
        } else {
            crouched = false;
            if (st == EMovement::MOVE_SoftLanding) {
                m_landing_timer -= dt;
                if (m_landing_timer <= 0.0f) st = EMovement::MOVE_Walking;
            } else if (st == EMovement::MOVE_AutoStepUp || st == EMovement::MOVE_StepUp) {
                if (m_state_timer >= 0.34f) st = EMovement::MOVE_Walking;
            } else {
                st = EMovement::MOVE_Walking;
            }
            set_stance(kEyeHeightStand);
        }
    }

    // What the controller asks for, and how the pawn turns it into velocity.
    float speed_mod = crouched ? c.crouch_speed_modifier : 1.0f;
    if (m_telemetry.weapon.equipped) speed_mod *= m_telemetry.weapon.mobility_scale;
    float friction = c.ground_friction;
    Vec3 accel(0.0f, 0.0f, 0.0f);
    if (turning) {
        friction *= c.turn_180_friction;  // TdMove_180Turn.FrictionModifier: coast through the turn
    } else {
        accel = controller_acceleration(input, false);
    }
    calc_velocity(accel, dt, speed_mod, friction);

    // UE3 physWalking: swept horizontal move with stepUp (MaxStepHeight 35).
    const float height = crouched ? kCrouchHeight : kPawnHeight;
    walk_move(horiz(m_telemetry.velocity) * dt, height, scene);
}

// -----------------------------------------------------------------------------
// Air Locomotion: TdMove_Jump / Falling / Coil / DodgeJump and the context moves out of them
// -----------------------------------------------------------------------------
void ParkourController::update_air_locomotion(const InputFrame& input, float dt, const LevelScene& scene) {
    const MovementConfig& c = m_config;
    EMovement& st = m_telemetry.move_state;
    if (m_telemetry.velocity.z > 0.0f && m_telemetry.position.z > m_fall_peak_z) {
        m_fall_peak_z = m_telemetry.position.z;
    }
    // TdMove_Falling.CloseToGround lets the jump animation go a little before she arrives. Native
    // code calls it; measured, it is her own box swept 0.4 s along the velocity meeting something:
    // the ground she lands on (ten landings, the drop left 0.38 to 0.42 of the fall speed), and as
    // well a wall she is about to fly into (0.39 s before she hit it). At any falling speed.
    m_telemetry.ground_distance = -1.0f;
    if (m_telemetry.velocity.z < 0.0f) {
        const Vec3 delta = m_telemetry.velocity * 0.4f;
        const float half_z = 0.5f * (kPawnHeight - 1.0f);
        const Vec3 extent(kPawnRadius - 1.0f, kPawnRadius - 1.0f, half_z);
        const TraceHit ahead = sweep_box(m_telemetry.position + Vec3(0.0f, 0.0f, 1.0f + half_z), extent, delta, scene);
        if (ahead.hit && !ahead.start_penetrating) m_telemetry.ground_distance = ahead.fraction * delta.length();
    }
    update_fall_height_volumes(scene);

    const float fall_dist = std::max(0.0f, m_fall_peak_z - m_telemetry.position.z);
    const bool soft_below = has_soft_landing_below(scene);
    if (soft_below && m_telemetry.falling_to_death) {
        m_telemetry.falling_to_death = false;
        m_telemetry.camera_roll_deg *= std::max(0.0f, 1.0f - 8.0f * dt);
    }
    const bool check_soft = (st == EMovement::MOVE_Falling || st == EMovement::MOVE_180TurnInAir);
    if (check_soft && soft_below && fall_dist >= c.soft_landing_min_fall &&
        m_telemetry.ground_distance > 180.0f && m_telemetry.velocity.z < -400.0f) {
        st = EMovement::MOVE_SoftLanding;
    } else if (st != EMovement::MOVE_SoftLanding && !soft_below &&
               fall_dist >= c.uncontrolled_fall && m_telemetry.velocity.z < -200.0f) {
        if (!m_telemetry.falling_to_death) {
            m_telemetry.falling_to_death = true;
            st = EMovement::MOVE_Falling;
        }
        // TdMove_Falling (FallingUncontrolled): wind buffeting roll & disoriented pitch while plummeting
        float wind_roll = std::sin(m_telemetry.sim_time * 9.5f) * 14.0f + 8.0f;
        m_telemetry.camera_roll_deg += (wind_roll - m_telemetry.camera_roll_deg) * std::min(1.0f, 6.0f * dt);
    }

    if (st == EMovement::MOVE_180TurnInAir && m_turn_timer <= 0.0f) {
        st = EMovement::MOVE_Falling;  // the turn is done; the landing still knows (m_turned_in_air)
    }
    const bool dodge = (st == EMovement::MOVE_DodgeJump);
    const bool turning = (st == EMovement::MOVE_180TurnInAir);
    const bool kicking = (st == EMovement::MOVE_MeleeAir || st == EMovement::MOVE_MeleeWallrun);

    // TdMove_Coil.CanDoMove: a crouch press out of a jump (never a plain fall or a dodge), moving
    // the way you face, without a heavy weapon. The legs lift TotalHeightBoost over
    // HeightBoostDuration and stay tucked until the landing.
    if (!m_telemetry.falling_to_death && m_crouch_pressed && !m_telemetry.weapon.is_heavy && coil_allowed_from(st) &&
        facing_forward().dot(m_telemetry.velocity) >= 100.0f) {
        m_crouch_pressed = false;
        st = EMovement::MOVE_Coil;
        m_coil_timer = 0.0f;
    }
    if (st == EMovement::MOVE_Coil) m_coil_timer += dt;

    // Context moves (TdPlayerMoveManager auto moves while airborne — disabled once in uncontrolled lethal fall).
    // Vault takes priority over wallclimb and ledge grab when holding forward (matching faith-runner controller.rs).
    if (!m_telemetry.falling_to_death && !dodge && !turning && !kicking) {
        if (try_initiate_zipline(scene)) return;
        if (try_initiate_swing_bar(input, scene)) return;
        if (try_initiate_climb(input, scene)) return;
        if (try_initiate_vault(input, scene)) return;
        if (try_initiate_wallclimb(input, scene)) return;
        if (try_initiate_ledge_grab(input, scene)) return;
        if (try_initiate_wallrun(input, scene)) return;
        if (try_initiate_ledge_walk(scene)) return;
    }
    if (dodge && m_telemetry.velocity.z < -190.0f) {
        st = EMovement::MOVE_Falling;
    }

    // Gravity
    m_telemetry.velocity.z -= c.gravity * dt;

    // Air control: the controller keeps asking for its ground acceleration; physFalling clamps it
    // to AccelRate x AirControl (6144 x 0.025 = 153.6 uu/s^2). Dodges, turns, and uncontrolled falls have none.
    // The pawn then gains that acceleration twice per tick - the same doubling as the gravity above
    // (1600 = 2 x the world's 800). Every retail recording shows it: holding W in the air from 50
    // to 520 uu/s gains 306-307 uu/s^2 (Jump, Falling, SpringBoarding, GrabJump, WallClimb180TurnJump
    // alike, and strafing gains 307 sideways); above that the speed curve takes over at twice its
    // value - median 102 at 550-600, 92 at 600-650, 43-53 at 650-700, 14-16 at 700-750, where the
    // curve gives ~52, ~47, ~28, ~6. With one application the port's springboard arc gained 50 uu/s^2
    // where retail's gained 99 and landed ~60 uu/s slower.
    if (!m_telemetry.falling_to_death && !dodge && !turning) {
        Vec3 a = horiz(controller_acceleration(input, true));
        const float max_a = c.accel_rate * c.air_control;
        if (a.length_sq() > max_a * max_a) a = a.normalized() * max_a;
        m_telemetry.velocity.x += 2.0f * a.x * dt;
        m_telemetry.velocity.y += 2.0f * a.y * dt;
    }

    // Swept displacement with slide-along-surface; the coil lifts the bottom of the box.
    float bottom = 0.0f;
    if (st == EMovement::MOVE_Coil) {
        bottom = c.coil_height_boost * std::clamp(m_coil_timer / std::max(c.coil_duration, 1e-3f), 0.0f, 1.0f);
    }
    const TraceHit hit = move_and_slide(m_telemetry.velocity * dt, kPawnHeight, bottom, scene);
    if (hit.hit) {
        const float vn = m_telemetry.velocity.dot(hit.normal);
        if (vn < 0.0f) m_telemetry.velocity -= hit.normal * vn;
    }
}

// -----------------------------------------------------------------------------
// Wallrun Subsystem (TdMove_WallRun / TdMove_WallrunJump, native UTdMove_WallRun update and
// ATdPawn::physWallRunning)
// -----------------------------------------------------------------------------
bool ParkourController::try_initiate_wallrun(const InputFrame& input, const LevelScene& scene) {
    const MovementConfig& c = m_config;
    if (m_telemetry.weapon.is_heavy) return false;
    if (m_wallrun_cooldown > 0.0f) return false;

    const Vec3 h = horiz(m_telemetry.velocity);
    const float speed = h.length();
    if (speed < c.wallrun_min_speed) return false;
    const Vec3 fwd = facing_forward();
    const Vec3 right = facing_right();
    if (h.dot(fwd) < 0.0f) return false;  // CanDoMove: moving the way you face

    const float top = c.wallrun_min_wall_height - 2.0f;
    auto same_wall = [&](const Vec3& n) {
        return m_last_wallrun_normal.length_sq() > 0.5f && n.dot(m_last_wallrun_normal) > 0.9f;
    };
    // Which way along the wall you would run: the tangent on the side you face.
    auto along_of = [&](const Vec3& n) {
        Vec3 t(-n.y, n.x, 0.0f);
        if (t.dot(fwd) < 0.0f) t = -t;
        return t;
    };

    WallFace hit;
    // MoveActionHint left / right only counts within 30 units of LastJumpLocation (FindWallSide).
    const bool side_hint = (m_telemetry.position - m_last_jump_location).length() < 30.0f && std::abs(input.strafe) > 0.3f;
    if (side_hint) {
        const Vec3 dir = right * sign_of(input.strafe);
        const WallFace hi = probe_wall(dir, c.wallrun_check_distance, top, scene);
        if (hi.found && std::abs(hi.normal.dot(dir)) >= std::cos(c.wallrun_side_angle_deg * DEG2RAD) &&
            !same_wall(hi.normal) && m_telemetry.velocity.dot(along_of(hi.normal)) >= 0.0f) {
            // The same wall again just above step height (a real wall, not a rail).
            const WallFace lo = probe_wall(dir, c.wallrun_check_distance, kMaxStepHeight + 2.0f, scene);
            if (lo.found && lo.normal.dot(hi.normal) > 0.999f) hit = hi;
        }
    }
    if (!hit.found) {
        // FindWallForward: the reach grows from WallRunningForwardCheckDistance at SpeedMaxBase to
        // ContextMoveDistanceMultiplier times it at GroundSpeed.
        const float frac = std::clamp((speed - c.speed_max_base_velocity) /
                                      std::max(c.ground_speed - c.speed_max_base_velocity, 1.0f), 0.0f, 1.0f);
        const float reach = ((c.wallrun_check_distance_mult - 1.0f) * frac + 1.0f) * c.wallrun_check_distance;
        const WallFace mid = probe_wall(fwd, reach, kPawnHeight * 0.5f, scene);
        if (mid.found) {
            // Facing at most WallRunningForwardMaxStartAngle into the wall (MinStartAngle is 0).
            const float d = -mid.normal.dot(fwd);
            if (d <= std::cos((90.0f - c.wallrun_max_angle_deg) * DEG2RAD) && !same_wall(mid.normal)) {
                // Tall enough: the wall again up at MinWallHeight, 50 lower the squarer you face it.
                const WallFace tall = probe_wall(fwd, reach, top - (1.0f - d) * 50.0f, scene);
                if (tall.found && tall.normal.dot(mid.normal) > 0.9f &&
                    m_telemetry.velocity.dot(along_of(mid.normal)) >= 0.0f) {
                    hit = mid;
                }
            }
        }
    }
    if (!hit.found) return false;
    if (h.dot(hit.normal) >= 0.0f) return false;  // must be moving into the wall
    if (is_foot_move_excluded(hit.point, hit.actor_index, scene)) return false;

    const Vec3 n = hit.normal;
    const Vec3 along = along_of(n);
    // Falling: no wallrun if there is ground just under where the run would take you (half a
    // second of travel ahead, 45 units down).
    if (m_telemetry.velocity.z < 0.0f) {
        const Vec3 start = m_telemetry.position + n * kPawnRadius + Vec3(0.0f, 0.0f, 2.0f);
        const Vec3 end = start + along * (speed * 0.5f) - Vec3(0.0f, 0.0f, 45.0f);
        if (trace_ray(start, end, scene).hit) return false;
    }

    // --- TdMove_WallRun.StartMove ---
    const bool from_wallrun_jump = (m_telemetry.move_state == EMovement::MOVE_WallRunJump);
    m_consecutive_wallruns = from_wallrun_jump ? m_consecutive_wallruns + 1 : 0;
    const float chain = 1.0f + static_cast<float>(m_consecutive_wallruns);

    float begin_speed = speed;
    if (m_telemetry.velocity.z > 0.0f) begin_speed = std::max(begin_speed, 300.0f);
    m_wallrun_begin_speed = begin_speed;

    // WallRunHeight: the InitialZHeight budget (cut by the ceiling) less what the jump already rose.
    float height = std::min(c.wallrun_initial_z, headroom(c.wallrun_initial_z, scene));
    height -= (m_telemetry.position.z - m_last_jump_location.z);
    height = std::max(0.0f, height);
    if (m_telemetry.velocity.z > 0.0f && !from_wallrun_jump) {
        m_telemetry.velocity.z = std::sqrt(2.0f * height * c.wallrun_accel * chain);
    }
    // ReachedWall: the horizontal velocity runs along the wall at BeginSpeed.
    m_telemetry.velocity.x = along.x * begin_speed;
    m_telemetry.velocity.y = along.y * begin_speed;

    const bool is_right = (n.dot(right) < 0.0f);
    m_telemetry.move_state = is_right ? EMovement::MOVE_WallRunningRight : EMovement::MOVE_WallRunningLeft;
    m_telemetry.wall_normal = n;
    m_telemetry.grounded = false;
    m_base_actor = -1;
    m_last_wallrun_normal = n;
    m_illegal_wall_timer = 0.0f;
    m_wall_tangent = along;
    m_wall_turned = false;
    m_state_timer = 0.0f;
    m_coil_timer = 0.0f;
    m_telemetry.camera_roll_deg = is_right ? -kWallrunCameraRoll : kWallrunCameraRoll;
    set_stance(kEyeHeightStand);

    // Keep the box's AABB clear of the (possibly rotated) wall so modular seams do not snag the sweeps.
    const float aabb_support = kPawnRadius * (std::abs(n.x) + std::abs(n.y)) + 4.0f;
    const float cur_wall_dist = (m_telemetry.position - hit.point).dot(n);
    if (cur_wall_dist < aabb_support) {
        move_swept(n * (aabb_support - cur_wall_dist), kPawnHeight, 0.0f, scene);
    }
    return true;
}

void ParkourController::update_wallrun(const InputFrame& input, float dt, const LevelScene& scene) {
    if (try_initiate_climb(input, scene)) {
        m_telemetry.camera_roll_deg = 0.0f;
        return;
    }

    const MovementConfig& c = m_config;
    const Vec3 n = m_telemetry.wall_normal;
    const Vec3 along = m_wall_tangent;
    const Vec3 h = horiz(m_telemetry.velocity);
    float speed = h.dot(along);
    const float chain = 1.0f + static_cast<float>(m_consecutive_wallruns);

    auto end_wallrun = [&](float cooldown) {
        m_wallrun_cooldown = cooldown;
        m_telemetry.move_state = EMovement::MOVE_Falling;
        m_takeoff_move = EMovement::MOVE_Falling;
        m_telemetry.camera_roll_deg = 0.0f;
        m_illegal_wall_timer = 2.0f;
    };

    // Q: look square at the wall (bTurned90FromWall): the view swings to the wall normal.
    if (input.turn_180 && !m_prev_turn_180 && !m_wall_turned) {
        m_wall_turned = true;
        m_turn_timer = 0.15f;
        m_turn_total = 0.15f;
        m_turn_target_yaw = yaw_of(n);
    }

    // Jump off (TdMove_WallrunJump.StartMove) after the first 0.1 s on the wall. Strafing hard away
    // from the wall is the WallrunDodge instead.
    if (jump_pressed() && m_state_timer > 0.1f) {
        consume_jump();
        const Vec3 fwd = facing_forward();
        const Vec3 strafe_dir = facing_right() * sign_of(input.strafe);
        if (std::abs(input.strafe) >= 0.8f && strafe_dir.dot(n) > 0.0f) {
            Vec3 vel = strafe_dir * 300.0f + m_telemetry.velocity * 0.3f;
            vel.z = 600.0f;
            m_telemetry.velocity = vel;
            m_telemetry.camera_roll_deg = 0.0f;
            m_last_jump_location = m_telemetry.position;
            m_last_wallrun_normal = n;
            m_illegal_wall_timer = 2.0f;
            m_wallrun_cooldown = 0.15f;
            // Away from the wall: left off a wall on the right.
            m_telemetry.move_left = m_telemetry.move_state == EMovement::MOVE_WallRunningRight;
            leave_ground(EMovement::MOVE_DodgeJump);
            return;
        }
        // `push`: how far you look away from the wall. It pushes you out harder and higher and
        // trades away along-wall speed; after Q it is the full push-off.
        const float push = m_wall_turned ? 1.0f : std::max(0.0f, fwd.dot(n));
        const float height = headroom(c.wallrun_jump_height + c.wallrun_jump_height_look_add * push, scene);
        const float up = speed_for_height(height) / chain;
        const float out = c.wallrun_jump_out + c.wallrun_jump_out_look_add * push;
        const float keep = c.wallrun_jump_forward_min + (1.0f - c.wallrun_jump_forward_min) * (1.0f - push);
        const Vec3 along_v = h - n * h.dot(n);
        m_telemetry.velocity = n * out + along_v * keep + Vec3(0.0f, 0.0f, up);
        m_telemetry.camera_roll_deg = 0.0f;
        m_last_jump_location = m_telemetry.position;
        m_last_wallrun_normal = n;
        m_illegal_wall_timer = 2.0f;
        m_wallrun_cooldown = 0.15f;
        // TdMove_WallrunJump.StartMove: pushing off hard (PushSpeed > 0.6) is the wall run jump
        // proper, anything less a plain jump.
        if (push > 0.6f) {
            set_move_anim(m_telemetry.move_state == EMovement::MOVE_WallRunningLeft ? "WallrunJumpLeft" : "WallrunJumpRight");
        }
        leave_ground(EMovement::MOVE_WallRunJump);
        return;
    }

    // Wallrun Kick (TdMove_MeleeWallrun)
    if (input.melee && m_melee_cooldown <= 0.0f) {
        m_wallrun_cooldown = 0.20f;
        m_telemetry.move_state = EMovement::MOVE_MeleeWallrun;
        m_takeoff_move = EMovement::MOVE_MeleeWallrun;
        m_state_timer = 0.0f;
        m_telemetry.combat_anim_time = 0.0f;
        m_telemetry.combat_anim_duration = 0.55f;
        m_melee_cooldown = 0.55f;
        m_telemetry.velocity = n * 260.0f + along * 380.0f + Vec3(0, 0, 140.0f);
        m_telemetry.camera_roll_deg = 0.0f;
        m_illegal_wall_timer = 2.0f;
        return;
    }

    // Still on the wall? The pawn's extent touches the wall anywhere between the knees and the head
    // (the reach tolerates modular wall seams).
    TraceHit wall_check;
    bool still_wall = false;
    for (float h : {kPawnHeight * 0.5f, 40.0f, kPawnHeight - 20.0f}) {
        const Vec3 probe_start = m_telemetry.position + Vec3(0.0f, 0.0f, h);
        const TraceHit t = trace_ray(probe_start, probe_start - n * (kPawnRadius + 65.0f), scene);
        if (t.hit && horiz(t.normal).normalized().dot(n) > 0.7f) {
            wall_check = t;
            still_wall = true;
            break;
        }
    }
    const bool falling_too_fast = m_telemetry.velocity.z < -c.wallrun_stop_fall_speed;
    if (!still_wall || falling_too_fast || m_crouch_pressed || input.forward < -0.3f) {
        if (m_crouch_pressed) m_crouch_pressed = false;
        end_wallrun(0.15f);
        return;
    }

    // UTdMove_WallRun's per-frame update and ATdPawn::physWallRunning: the controller asks for no
    // acceleration, so holding forward does not speed you up. The velocity is kept along the wall
    // with a slight drag (WallRunningHorisontalFriction); vertically you are pulled down at
    // WallRunningHorisontalAcceleration while rising and WallRunningHorisontalDeceleration while
    // falling, both x (1 + ConsequtiveWallruns).
    speed -= speed * c.wallrun_friction * dt;
    if (speed < 0.1f) {
        end_wallrun(0.15f);
        return;
    }
    const float pull = (m_telemetry.velocity.z > 0.0f ? c.wallrun_accel : c.wallrun_decel) * chain;
    m_telemetry.velocity = along * speed - n * 0.5f + Vec3(0.0f, 0.0f, m_telemetry.velocity.z - pull * dt);

    // Keep the box's AABB slightly clear of angled wall faces so modular static mesh seams never snag.
    const float aabb_support = kPawnRadius * (std::abs(n.x) + std::abs(n.y)) + 4.0f;
    const float cur_wall_dist = (m_telemetry.position - wall_check.point).dot(n);
    if (cur_wall_dist < aabb_support) {
        move_swept(n * (aabb_support - cur_wall_dist), kPawnHeight, 0.0f, scene);
    }

    // Swept movement along the wall. If a modular wall seam, window frame or thin pilaster
    // (<= 28u along +normal) blocks the tangent sweep, step outward along +normal just like
    // APawn::stepUp does for floor kerbs.
    const Vec3 full_delta = m_telemetry.velocity * dt;
    const TraceHit first = move_swept(full_delta, kPawnHeight, 0.0f, scene);
    if (first.hit) {
        bool stepped_over_seam = false;
        const Vec3 remaining = full_delta * (1.0f - first.fraction);
        if (first.normal.z < kWalkableFloorZ && remaining.length_sq() > 1e-6f) {
            constexpr float kMaxWallSeamStep = 28.0f;
            const Vec3 pos_at_contact = m_telemetry.position;
            move_swept(n * kMaxWallSeamStep, kPawnHeight, 0.0f, scene);
            const float step_out = (m_telemetry.position - pos_at_contact).dot(n);
            if (step_out > 0.5f) {
                const Vec3 pos_stepped = m_telemetry.position;
                const TraceHit retry = move_swept(remaining, kPawnHeight, 0.0f, scene);
                const float advanced = (m_telemetry.position - pos_stepped).dot(along);
                const float expected = remaining.dot(along);
                if (!retry.hit || (expected > 1e-3f && advanced > 0.5f * expected)) {
                    // Settle back toward the new wall panel plane while keeping clean standoff
                    move_swept(-n * std::max(0.0f, step_out - 2.0f), kPawnHeight, 0.0f, scene);
                    stepped_over_seam = true;
                } else {
                    m_telemetry.position = pos_at_contact;
                }
            } else {
                m_telemetry.position = pos_at_contact;
            }
        }

        if (!stepped_over_seam) {
            // Slide the remaining movement along the blocking surface; a wall across the run or a
            // floor ends the wallrun.
            Vec3 slide_rem = full_delta * (1.0f - first.fraction);
            slide_rem -= first.normal * slide_rem.dot(first.normal);
            if (slide_rem.length_sq() > 1e-6f) {
                move_swept(slide_rem + first.normal * 0.01f, kPawnHeight, 0.0f, scene);
            }
            const float vn = m_telemetry.velocity.dot(first.normal);
            if (vn < 0.0f) m_telemetry.velocity -= first.normal * vn;
            if (first.normal.dot(along) < -0.5f || first.normal.z >= kWalkableFloorZ) {
                end_wallrun(0.20f);
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Wallclimb & 180° Turn Jump Subsystem (TdMove_WallClimb / TdMove_WallClimb180TurnJump)
// -----------------------------------------------------------------------------
bool ParkourController::try_initiate_wallclimb(const InputFrame& input, const LevelScene& scene) {
    const MovementConfig& c = m_config;
    if (m_telemetry.weapon.is_heavy || m_climb_cooldown > 0.0f) return false;
    // Native CanDoMove: rising (VelocityStartLimit 0), MoveActionHint forward, facing the wall
    // within WallClimbingVerticalStartAngle, the wall within WallClimbingMaxDistance2D and at
    // least MinWallHeight tall.
    if (m_telemetry.velocity.z <= 0.0f) return false;
    if (input.forward <= 0.8f) return false;
    const Vec3 fwd = facing_forward();
    const WallFace wall = probe_wall(fwd, c.wallclimb_max_distance, kPawnHeight * 0.5f, scene);
    if (!wall.found || wall.distance > c.wallclimb_max_distance) return false;
    if (is_foot_move_excluded(wall.point, wall.actor_index, scene)) return false;
    if (fwd.dot(-wall.normal) < std::cos(c.wallclimb_max_angle_deg * DEG2RAD)) return false;
    if (m_last_wallrun_normal.length_sq() > 0.5f && wall.normal.dot(m_last_wallrun_normal) > 0.9f) return false;
    const WallFace tall = probe_wall(fwd, c.wallclimb_max_distance, c.wallclimb_min_wall_height - 2.0f, scene);
    if (!tall.found || tall.normal.dot(wall.normal) < 0.9f) return false;

    // --- TdMove_WallClimb.StartMove ---
    const Vec3 into = -wall.normal;
    m_into_wallclimb_speed = m_telemetry.velocity.length_xy() - 100.0f;
    if (m_into_wallclimb_speed < 100.0f) {
        // Too slow to carry into the wall: the pawn is sucked in at 400.
        m_telemetry.velocity.x = into.x * c.wallclimb_suck_in_speed;
        m_telemetry.velocity.y = into.y * c.wallclimb_suck_in_speed;
    }
    m_telemetry.move_state = EMovement::MOVE_WallClimbing;
    m_telemetry.wall_normal = wall.normal;
    m_telemetry.grounded = false;
    m_base_actor = -1;
    m_wallclimb_reached = false;
    m_wall_turned = false;
    m_state_timer = 0.0f;
    m_coil_timer = 0.0f;
    set_stance(kEyeHeightStand);
    return true;
}

void ParkourController::update_wallclimb(const InputFrame& input, float dt, const LevelScene& scene) {
    if (try_initiate_climb(input, scene)) return;

    const MovementConfig& c = m_config;
    const Vec3 n = m_telemetry.wall_normal;
    const Vec3 into = -n;
    if (m_telemetry.position.z > m_fall_peak_z) m_fall_peak_z = m_telemetry.position.z;

    auto fall_off = [&]() {
        m_telemetry.move_state = EMovement::MOVE_Falling;
        m_takeoff_move = EMovement::MOVE_Falling;
        m_last_wallrun_normal = n;
        m_illegal_wall_timer = 0.5f;
    };

    if (m_wall_turned) {
        // TdMove_WallClimb180TurnJump: turned to face away, JumpTimeWindow to push off (a ceiling
        // cuts the height); otherwise the pawn slides down the wall and drops.
        if (jump_pressed()) {
            consume_jump();
            Vec3 vel = n * c.wallclimb_turn_jump_out;
            vel.z = speed_for_height(headroom(c.wallclimb_turn_jump_height, scene));
            m_telemetry.velocity = vel;
            m_last_jump_location = m_telemetry.position;
            m_last_wallrun_normal = n;
            m_illegal_wall_timer = 2.0f;
            leave_ground(EMovement::MOVE_WallClimb180TurnJump);
            return;
        }
        if (m_state_timer > c.wallclimb_turn_jump_window) {
            fall_off();
            return;
        }
        m_telemetry.velocity = into * 0.3f + Vec3(0.0f, 0.0f, std::min(m_telemetry.velocity.z, 50.0f) - c.gravity * 0.25f * dt);
        move_and_slide(m_telemetry.velocity * dt, kPawnHeight, 0.0f, scene);
        return;
    }

    // Q: 180° turn on the wall (root rotation) then the jump window.
    if (input.turn_180 && !m_prev_turn_180 && m_wallclimb_reached) {
        m_wall_turned = true;
        m_turn_timer = c.turn_180_time;
        m_turn_total = c.turn_180_time;
        m_turn_target_yaw = m_telemetry.yaw_deg + 180.0f;
        m_state_timer = 0.0f;
        return;
    }

    // WallClimbDodge: a jump press while strafing hard hops sideways along the wall face.
    if (jump_pressed() && std::abs(input.strafe) >= 0.8f && m_wallclimb_reached) {
        consume_jump();
        Vec3 along(-n.y, n.x, 0.0f);
        if (along.dot(facing_right() * sign_of(input.strafe)) < 0.0f) along = -along;
        Vec3 vel = along * c.wallclimb_dodge_side_speed + n * 30.0f;
        vel.z = c.wallclimb_dodge_z;
        m_telemetry.velocity = vel;
        m_last_jump_location = m_telemetry.position;
        m_last_wallrun_normal = n;
        m_illegal_wall_timer = 2.0f;
        leave_ground(EMovement::MOVE_DodgeJump);
        return;
    }

    // Reaching the wall (ReachedWall): the climb speed is set from how fast you ran in and how
    // fast you were rising: height = AddOnSpeed2DHeight x f(IntoWallClimbSpeed) +
    // AddOnSpeedZHeight x f(Velocity.Z), Velocity.Z = Sqrt(4 x height x WallClimbingGravity).
    if (!m_wallclimb_reached) {
        const WallFace face = probe_wall(into, c.wallclimb_max_distance, kPawnHeight * 0.5f, scene);
        if (!face.found) {
            fall_off();
            return;
        }
        if (face.distance <= kPawnRadius + 4.0f) {
            const float f_xy = std::clamp((m_into_wallclimb_speed - c.speed_max_base_velocity) /
                                          std::max(c.wallclimb_add_xy_max_speed - c.speed_max_base_velocity, 1.0f), 0.0f, 1.0f);
            const float f_z = std::clamp(m_telemetry.velocity.z / std::max(c.wallclimb_boost_z, 1.0f), 0.0f, 1.0f);
            const float height = c.wallclimb_add_xy_height * f_xy + c.wallclimb_add_z_height * f_z;
            m_telemetry.velocity = Vec3(0.0f, 0.0f, std::sqrt(std::max(4.0f * height * c.wallclimb_gravity, 0.01f)));
            m_wallclimb_reached = true;
        } else {
            // Still flying in: ordinary falling physics until the body touches the wall.
            m_telemetry.velocity.z -= c.gravity * dt;
            move_and_slide(m_telemetry.velocity * dt, kPawnHeight, 0.0f, scene);
            if (m_telemetry.velocity.z <= 0.0f) fall_off();
            return;
        }
    }

    // The ledge at the top: vault over when holding forward on a thin fence/ledge, or catch/mantle.
    if (try_initiate_vault(input, scene)) return;
    if (try_initiate_ledge_grab(input, scene)) return;

    // Still a wall ahead? Otherwise the top has been passed with nothing to grab.
    const WallFace still = probe_wall(into, kPawnRadius + 20.0f, kPawnHeight * 0.5f, scene);
    if (!still.found) {
        fall_off();
        return;
    }

    // Climb: the pawn's acceleration is straight down at the climb gravity (x2, like every
    // vertical move); no drag on the climb itself.
    m_telemetry.velocity.z -= 2.0f * c.wallclimb_gravity * dt;
    if (m_telemetry.velocity.z <= 0.0f) {
        fall_off();
        return;
    }
    const TraceHit climb_hit = move_swept(Vec3(0.0f, 0.0f, m_telemetry.velocity.z * dt), kPawnHeight, 0.0f, scene);
    if (climb_hit.hit) {
        // An overhang stops the ascent.
        m_telemetry.velocity.z = 0.0f;
        fall_off();
    }
}

// -----------------------------------------------------------------------------
// Slide Subsystem (TdMove_Slide / TdMove_MeleeSlide)
// -----------------------------------------------------------------------------
void ParkourController::update_slide(const InputFrame& input, float dt, LevelScene& scene) {
    const MovementConfig& c = m_config;
    m_slide_timer += dt;
    EMovement& st = m_telemetry.move_state;
    m_barge_speed = std::max(m_barge_speed, m_telemetry.velocity.length_xy());

    // Slide Melee Kick (`MeleeSlide`: sweeps enemies off their feet with `HitMeleeSlide`
    // and barges closed doors via TdMove_MeleeSlide.FindBargeTarget / BargeObject)
    if (input.melee && st != EMovement::MOVE_MeleeSlide && m_melee_cooldown <= 0.0f) {
        st = EMovement::MOVE_MeleeSlide;
        m_state_timer = 0.0f;
        m_telemetry.combat_anim_time = 0.0f;
        m_telemetry.combat_anim_duration = kMeleeSlideLength;
        m_melee_cooldown = kMeleeSlideLength;
        m_barge_dealt_damage = false;
        m_barge_speed = std::max(m_barge_speed, std::max(m_telemetry.velocity.length_xy(), c.slide_abort_speed + 100.0f));
        for (const AnimSoundNotify& notify : kMeleeSlideNotifies) {
            if (notify.time <= 0.0f) {
                emit_sound(notify.cue, m_telemetry.position, true);
            }
        }

        for (auto& bot : scene.enemies) {
            if (bot.alive && m_telemetry.position.distance(bot.position) < 200.0f) {
                bot.health -= 65.0f;
                bot.stunned = true;
                bot.disarm_window = true;
                bot.attack_timer = 0.0f;
                bot.anim_timer = 0.0f;
                bot.active_anim_seq = "HitMeleeSlide";
                m_telemetry.melee_hit_confirmed = true;
                m_telemetry.hit_marker_timer = 0.25f;
                if (bot.health <= 0.0f) {
                    bot.alive = false;
                    bot.anim_state = EEnemyAnimState::KnockedOut;
                    m_telemetry.active_subtitle = "Slide Kick Knockout!";
                } else {
                    bot.anim_state = EEnemyAnimState::HitStagger;
                    m_telemetry.active_subtitle = "Slide Kick Hit! Enemy Staggered (Press Right-Click / E to Disarm)";
                }
            }
        }
        if (dt <= 0.0f) return;
    }

    if (st == EMovement::MOVE_MeleeSlide && dt > 0.0f) {
        const float prev_pos = std::max(0.0f, m_state_timer - dt);
        const float cur_pos = m_state_timer;
        for (const AnimSoundNotify& notify : kMeleeSlideNotifies) {
            if (notify.time > prev_pos && notify.time <= cur_pos) {
                emit_sound(notify.cue, m_telemetry.position, true);
            }
        }
    }

    // Move input is ignored for the whole slide (DisableMovementTime -1). The body turns toward
    // the view at SlideLookTurn x the angle per second, A / D (MoveActionHint left / right) turn it
    // 2000 rotation units per second, and the velocity is pointed along the body.
    const float hint_side = (input.strafe > 0.3f) ? 1.0f : (input.strafe < -0.3f ? -1.0f : 0.0f);
    const bool hint_down = hint_side == 0.0f && input.forward < -0.8f;
    const float d = wrap_deg(m_telemetry.yaw_deg - m_slide_yaw);
    m_slide_yaw += dt * c.slide_look_turn * d - hint_side * c.slide_strafe_turn_deg * dt;
    const Vec3 body = Rotator::from_degrees(0.0f, m_slide_yaw, 0.0f).forward();
    float speed = m_telemetry.velocity.length_xy();
    m_telemetry.velocity.x = body.x * speed;
    m_telemetry.velocity.y = body.y * speed;

    // CalcVelocity brakes it with GroundFriction x FrictionModifier.
    calc_velocity(Vec3(0.0f, 0.0f, 0.0f), dt, 1.0f, c.ground_friction * c.slide_friction);
    speed = m_telemetry.velocity.length_xy();
    m_barge_speed = std::max(m_barge_speed, speed);

    auto find_slide_barge_door = [&](float trace_dist) -> int {
        Vec3 hit_point;
        int door = find_barge_door(scene, body, trace_dist, hit_point);
        if (door < 0) {
            const Vec3 facing = Rotator::from_degrees(0.0f, m_pawn_yaw, 0.0f).forward();
            door = find_barge_door(scene, facing, trace_dist, hit_point);
        }
        if (door < 0) {
            door = find_barge_door(scene, facing_forward(), trace_dist, hit_point);
        }
        return door;
    };

    auto barge_slide_door = [&](int door) {
        m_barge_dealt_damage = true;
        emit_sound("Wood._11_Female_FootStepAttack", m_telemetry.position, true);
        open_barge_door(door, body, true, scene);
        const float retain = std::max(speed, std::max(m_barge_speed * 0.80f, c.slide_abort_speed + 50.0f));
        m_telemetry.velocity.x = body.x * retain;
        m_telemetry.velocity.y = body.y * retain;
        speed = retain;
    };

    if (st == EMovement::MOVE_MeleeSlide) {
        // TdMove_MeleeSlide.OnFindBargeTargetTimer (0.33s) -> FindBargeTarget -> BargeObject.
        // Also fire if the pawn already slid up against a closed door.
        if (!m_barge_dealt_damage && (m_state_timer >= kMeleeSlideBargeTime || speed < c.slide_abort_speed)) {
            const int door = find_slide_barge_door(125.0f);
            if (door >= 0) {
                barge_slide_door(door);
            }
        }
        // TdMove_MeleeSlide ends via OnCustomAnimEnd -> SetMove(MOVE_Crouch) (or MOVE_Walking if uncrouched).
        if (m_state_timer >= kMeleeSlideLength) {
            m_telemetry.velocity.x *= 0.5f;
            m_telemetry.velocity.y *= 0.5f;
            const bool stand = !input.crouch && has_room(kPawnHeight, scene);
            st = stand ? EMovement::MOVE_Walking : EMovement::MOVE_Crouch;
            set_stance(stand ? kEyeHeightStand : kEyeHeightCrouch);
            m_sprint_energy = std::max(0.0f, m_telemetry.velocity.length_xy() - c.speed_max_base_velocity);
            m_accel_time = 1.0f;
            return;
        }
    } else {
        // It aborts into a crouch below SlideAbortSpeed, after SlideAbortTime, or when you pull back;
        // letting go of crouch ends it into a walk (a crouch with no room), but not inside the first
        // 0.5 s (the request waits for StartMove's timer). Jump does nothing in a slide.
        // If sliding into a closed barge door inside the first 0.65 s, allow a brief window for a melee
        // kick input before aborting on low speed.
        const bool blocked_by_closed_door = (speed < c.slide_abort_speed && m_slide_timer < 0.65f &&
                                             find_slide_barge_door(100.0f) >= 0);
        const bool abort = hint_down || (speed < c.slide_abort_speed && !blocked_by_closed_door) ||
                           m_slide_timer >= c.slide_max_duration;
        const bool uncrouch = !input.crouch && m_slide_timer >= c.slide_min_duration;
        if (abort || uncrouch) {
            // StopMove halves the velocity, however the slide ends.
            m_telemetry.velocity.x *= 0.5f;
            m_telemetry.velocity.y *= 0.5f;
            const bool stand = !input.crouch && has_room(kPawnHeight, scene);
            st = stand ? EMovement::MOVE_Walking : EMovement::MOVE_Crouch;
            set_stance(stand ? kEyeHeightStand : kEyeHeightCrouch);
            m_sprint_energy = std::max(0.0f, m_telemetry.velocity.length_xy() - c.speed_max_base_velocity);
            m_accel_time = 1.0f;
            return;
        }
    }

    // Low (crouch-height) swept move: passes under ducts, steps over seams, glances off walls.
    const Vec3 pos_before_walk = m_telemetry.position;
    walk_move(horiz(m_telemetry.velocity) * dt, kCrouchHeight, scene);

    // If a slide kick swept into a closed barge door during walk_move, barge the door open immediately
    // on contact and carry the remaining slide step through the doorway.
    if (st == EMovement::MOVE_MeleeSlide && !m_barge_dealt_damage && m_walk_blocked) {
        const int door = find_slide_barge_door(125.0f);
        if (door >= 0) {
            barge_slide_door(door);
            const float moved_xy = horiz(m_telemetry.position - pos_before_walk).length();
            const float rem_dt = std::max(0.0f, dt - moved_xy / std::max(speed, 1.0f));
            if (rem_dt > 1e-5f) {
                walk_move(horiz(m_telemetry.velocity) * rem_dt, kCrouchHeight, scene);
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Ledge Grab, Shimmy & Pull-Up Subsystem (TdMove_IntoGrab / Grab / GrabPullUp / GrabJump)
// -----------------------------------------------------------------------------
bool ParkourController::try_initiate_ledge_grab(const InputFrame& input, const LevelScene& scene) {
    (void)input;
    const MovementConfig& c = m_config;
    const EMovement st = m_telemetry.move_state;
    if (st == EMovement::MOVE_Grabbing || st == EMovement::MOVE_GrabPullUp || st == EMovement::MOVE_IntoGrab ||
        st == EMovement::MOVE_DodgeJump) {
        return false;
    }

    const Vec3 fwd = facing_forward();
    const Ledge ledge = find_ledge(fwd, 45.0f, kGrabMinRise, kGrabMaxRise, scene);
    if (!ledge.found) return false;
    const Vec3 into = -ledge.normal;
    // GrabMaxAngle: facing the wall; and not moving away from it.
    if (fwd.dot(into) < std::cos(c.grab_max_angle_deg * DEG2RAD)) return false;
    if (horiz(m_telemetry.velocity).dot(ledge.normal) > 50.0f) return false;
    // No re-grabbing the wall just jumped off or same wall immediately after pipe jump-off.
    if (m_last_wallrun_normal.length_sq() > 0.5f && ledge.normal.dot(m_last_wallrun_normal) > 0.9f &&
        m_illegal_wall_timer > 0.0f && st != EMovement::MOVE_WallClimbing) {
        return false;
    }
    if (m_climb_cooldown > 0.12f && ledge.normal.dot(m_climb_normal) > 0.9f) {
        return false;
    }
    // Room for the body on top (standing, or crouched under a low ceiling). With none, a rail
    // standing on the lip is still a grab: retail hangs there and a jump takes it up onto the rail
    // and over (TdMove_IntoGrab.bCheckForVaultOver, TdMove_GrabTransfer).
    const Vec3 on_top = ledge.top_point + into * (kPawnRadius + 2.0f) + Vec3(0.0f, 0.0f, 0.5f);
    const bool stand_room = has_room_at(on_top, kPawnHeight, scene);
    const bool room = stand_room || has_room_at(on_top, kCrouchHeight, scene);
    if (!room && !find_rail_transfer(ledge.normal, ledge.top_z, scene).found) return false;

    const float rise = ledge.top_z - m_telemetry.position.z;
    if (room && rise <= kMantleMaxRise && m_telemetry.velocity.z > -50.0f) {
        // Already up to the hands: mantle straight over (the top of a wallclimb / a high jump).
        if (climb_onto_ledge(ledge.normal, ledge.top_z, scene)) {
            const float speed = std::max(200.0f, std::min(m_telemetry.velocity.length_xy(), c.speed_max_base_velocity));
            m_telemetry.velocity = into * speed;
            m_telemetry.grounded = true;
            m_telemetry.move_state = stand_room ? EMovement::MOVE_Walking : EMovement::MOVE_Crouch;
            m_state_timer = 0.0f;
            m_sprint_energy = 0.0f;
            m_last_wallrun_normal = Vec3(0.0f, 0.0f, 0.0f);
            set_stance(stand_room ? kEyeHeightStand : kEyeHeightCrouch);
            return true;
        }
        return false;
    }

    // Hang: hands on the lip, the top GrabHangDepth above the feet, the body against the wall.
    // The body has to fit there. A lip set back behind a wall below it would put the body inside
    // that wall: retail falling past the railed slab catches the slab's own lip, not the rail.
    // The test box is inscribed in the pawn's circle, so it clears a wall face at any angle.
    const float target_z = ledge.top_z - c.grab_hang_depth;
    const float gap = ledge.wall_distance - kPawnRadius - 1.0f;
    // TdPawn.MoveLedgeLocation: the lip in front of her.
    m_telemetry.ledge_known = true;
    m_telemetry.ledge_point = Vec3(m_telemetry.position.x, m_telemetry.position.y, ledge.top_z) + into * ledge.wall_distance;
    m_telemetry.ledge_wall_normal = ledge.normal;
    m_telemetry.ledge_top_normal = ledge.top_normal;
    {
        const float r = kPawnRadius * 0.7f;
        const Vec3 hang = m_telemetry.position + into * std::max(gap, 0.0f);
        if (!box_free(Vec3(hang.x, hang.y, target_z + 0.5f * kPawnHeight), Vec3(r, r, 0.5f * kPawnHeight - 2.0f), scene)) {
            return false;
        }
    }
    move_swept(Vec3(0.0f, 0.0f, target_z - m_telemetry.position.z), kPawnHeight, 0.0f, scene);
    if (gap > 0.0f) move_swept(into * gap, kPawnHeight, 0.0f, scene);

    // TdMove_Grab.bIsHangingFree: nothing in front of the legs to put the feet against.
    m_telemetry.hanging_free = !probe_wall(ledge.normal * -1.0f, kPawnRadius + 45.0f, 60.0f, scene).found;
    // TdMove_IntoGrab / TdMove_Grab.bSlopedLedge: MoveLedgeNormal.Z < 0.999. The hang's pose leans
    // with the ledge by how steeply it runs along her shoulders.
    {
        const Vec3 right(ledge.normal.y, -ledge.normal.x, 0.0f);  // her right, facing the wall
        m_telemetry.ledge_sloped = ledge.top_normal.z < 0.999f;
        m_telemetry.ledge_slope_deg = m_telemetry.ledge_sloped
            ? std::atan2(-ledge.top_normal.dot(right), ledge.top_normal.z) * RAD2DEG : 0.0f;
    }
    // TdMove_IntoGrab.ReachedPreciseLocation: how she catches the ledge, by where she came from and
    // how fast she was falling (HangImpactMinZSpeed -600, HangHardImpactMinZSpeed -1000).
    set_move_anim(m_telemetry.move_state == EMovement::MOVE_WallClimbing ? "hanghardstartvertical"
                  : m_telemetry.hanging_free ? "HangFreeHardStart"
                  : m_telemetry.velocity.z < -1000.0f ? "HangHardStart3"
                  : m_telemetry.velocity.z < -600.0f ? "HangHardStart2"
                  : "HangHardStart");
    m_telemetry.move_state = EMovement::MOVE_Grabbing;
    m_telemetry.velocity = Vec3(0.0f, 0.0f, 0.0f);
    m_telemetry.wall_normal = ledge.normal;
    m_telemetry.grounded = false;
    m_telemetry.camera_roll_deg = 0.0f;
    m_ledge_z = ledge.top_z;
    m_grab_rail = !room;
    m_hang_time = 0.0f;
    m_state_timer = 0.0f;
    m_base_actor = -1;
    m_coil_timer = 0.0f;
    m_consecutive_wallruns = 0;
    m_wall_turned = false;
    set_stance(kEyeHeightStand);
    return true;
}

void ParkourController::start_pull_up(const LevelScene& scene) {
    Vec3 into = horiz(-m_telemetry.wall_normal);
    into = into.length_sq() > 1e-6f ? into.normalized() : horiz(facing_forward()).normalized();
    // TdMove_GrabPullUp.StartMove: the heave that ends standing, or crouched where there is no room
    // to stand on top (GrabPullUpType); the free hang's own when nothing is in front of the legs.
    const Vec3 top_at = m_telemetry.position + into * (2.0f * kPawnRadius + 5.0f);
    const bool stand_up = has_room_at(Vec3(top_at.x, top_at.y, m_ledge_z + 0.5f), kPawnHeight, scene);
    m_pullup_heave = (m_telemetry.hanging_free ? 1 : 0) | (stand_up ? 0 : 2);
    const HeaveRootMotion& heave = kHeaveRootMotion[m_pullup_heave];
    m_telemetry.move_state = EMovement::MOVE_GrabPullUp;
    m_state_timer = 0.0f;
    m_pullup_start = m_telemetry.position;
    m_pullup_dir = into;
    m_pullup_lift = 0.0f;
    m_telemetry.combat_anim_duration = heave.length;
    set_move_anim(heave.anim);
}

void ParkourController::update_ledge_grab(const InputFrame& input, float dt, const LevelScene& scene) {
    const MovementConfig& c = m_config;
    EMovement& st = m_telemetry.move_state;
    const Vec3 n = m_telemetry.wall_normal;
    const Vec3 into = -n;
    const Vec3 fwd = facing_forward();
    m_hang_time += dt;
    m_telemetry.velocity = Vec3(0.0f, 0.0f, 0.0f);

    if (st == EMovement::MOVE_GrabPullUp) {
        // TdMove_GrabPullUp: the heave's root motion carries her (kHeaveRootMotion), PHYS_Flying with
        // collision off, along the way she faced the wall when it began (retail's capsule keeps to
        // that line while the body turns after ReleaseCamera). The camera on the EyeJoint rides it.
        const HeaveRootMotion& heave = kHeaveRootMotion[m_pullup_heave];
        float forward = 0.0f, up = 0.0f;
        heave_root_motion(heave, m_state_timer, forward, up);
        Vec3 next = m_pullup_start + m_pullup_dir * forward + Vec3(0.0f, 0.0f, up + m_pullup_lift);
        // EnableCollision: from then on PHYS_Flying steps a pawn the root motion has put into the
        // level up out of it (at most MaxStepHeight), and the lift stays for the rest of the heave.
        // Retail: 14 uu at 1.40 s in the free hang of 2026-09-29 21:48 (HangFreeHeaveUp's Root is
        // still 13 below the lip then), 25 onto a raised floor behind the lip in 2026-09-20 14:56.
        if (m_state_timer >= kPullUpEnableCollision && !has_room_at(next, kPawnHeight, scene) &&
            has_room_at(next + Vec3(0.0f, 0.0f, kMaxStepHeight), kPawnHeight, scene)) {
            float lo = 0.0f, hi = kMaxStepHeight;
            for (int i = 0; i < 10; ++i) {
                const float mid = 0.5f * (lo + hi);
                (has_room_at(next + Vec3(0.0f, 0.0f, mid), kPawnHeight, scene) ? hi : lo) = mid;
            }
            // has_room_at stands its box a unit off the feet, so at `hi` the feet are a unit under
            // the top they clear; the step-up leaves her hovering a floor distance over it (feet 1.4
            // above the lip; retail's lifts of 14.2 to 14.5 uu put them 1.5 above).
            const float lift = hi + kMaxFloorDist;
            m_pullup_lift += lift;
            next.z += lift;
        }
        if (dt > 1e-5f) {
            m_telemetry.velocity = (next - m_telemetry.position) * (1.0f / dt);
        }
        m_telemetry.position = next;
        set_stance(kEyeHeightStand);
        if (m_state_timer >= heave.length) {
            // OnCustomAnimEnd: walking (crouched after a ...ToCrouch heave) from where the heave left
            // her, a few units over the floor the walk then puts her feet on.
            const bool stand = !heave.crouch && has_room(kPawnHeight, scene);
            st = stand ? EMovement::MOVE_Walking : EMovement::MOVE_Crouch;
            m_telemetry.grounded = true;
            m_telemetry.velocity = m_pullup_dir * 100.0f;
            set_stance(stand ? kEyeHeightStand : kEyeHeightCrouch);
            m_sprint_energy = 0.0f;
        }
        return;
    }

    // TdMove_Grab: facing within 45° of the wall a jump (or pushing forward) pulls up; looking
    // away from it a jump is a GrabJump; crouch (or pulling back) lets go; A / D shimmy along.
    // Under a rail there is nothing to pull up onto. A jump transfers the hands up onto the rail
    // and vaults over it, and pushing forward does nothing. Retail held W through a 3.3 s hang
    // there (20260920_185901), and the GrabTransfer started in the frame of the space press.
    const bool facing_wall = fwd.dot(into) >= std::cos(45.0f * DEG2RAD);
    if (jump_pressed()) {
        consume_jump();
        if (facing_wall) {
            if (m_grab_rail) {
                // TdMove_GrabTransfer goes where the view points: within 60 degrees of the move up
                // to the rail. Retail ignored a jump looking 12 deg up (69 deg off that line) and
                // transferred looking 26 and 60 deg up (55 and 21 deg off). Its sideways pipe
                // transfer (edge_pt1 t=201.93) started 22 deg off.
                const RailTransfer rail = find_rail_transfer(n, m_ledge_z, scene);
                const Vec3 view = Rotator::from_degrees(m_telemetry.pitch_deg, m_telemetry.yaw_deg, 0.0f).forward();
                if (rail.found && view.dot((rail.hang - m_telemetry.position).normalized()) >= 0.5f) {
                    start_grab_transfer(rail);
                }
                return;
            }
            start_pull_up(scene);
            return;
        }
        const float push = std::clamp(fwd.dot(n), 0.0f, 1.0f);
        const Vec3 dir = horiz(fwd).normalized();
        Vec3 vel = dir * (c.grab_jump_push_min + (c.grab_jump_push_max - c.grab_jump_push_min) * push);
        vel.z = speed_for_height(c.grab_jump_height);
        m_telemetry.velocity = vel;
        m_last_jump_location = m_telemetry.position;
        m_last_wallrun_normal = n;
        m_illegal_wall_timer = 2.0f;
        leave_ground(EMovement::MOVE_GrabJump);
        return;
    }
    if (input.forward > 0.8f && facing_wall && m_hang_time > 0.1f && !m_grab_rail) {
        start_pull_up(scene);
        return;
    }
    if (m_crouch_pressed || (input.forward < -0.8f && m_hang_time > 0.2f)) {
        m_crouch_pressed = false;
        m_telemetry.velocity = n * 50.0f;
        m_last_wallrun_normal = n;
        m_illegal_wall_timer = 0.5f;
        leave_ground(EMovement::MOVE_Falling);
        return;
    }
    // TdMove_Grab.HandleMoveAction: there is no shimmying along a sloped ledge.
    if (std::abs(input.strafe) > 0.3f && m_hang_time > c.grab_shimmy_delay && !m_telemetry.ledge_sloped) {
        Vec3 along(-n.y, n.x, 0.0f);
        if (along.dot(facing_right() * sign_of(input.strafe)) < 0.0f) along = -along;
        // The ledge has to continue that way: test multiple depths beyond the wall face + thin box sweep
        // just like find_ledge() so thin railings and recessed lips shimmy smoothly too.
        bool continues = false;
        constexpr Vec3 kShimmyProbeExtent(2.5f, 2.5f, 0.5f);
        for (float extra : {2.0f, 8.0f, 18.0f, 34.0f}) {
            const Vec3 column = m_telemetry.position + along * (kPawnRadius + 8.0f) + into * (kPawnRadius + extra);
            const TraceHit top = trace_ray(Vec3(column.x, column.y, m_ledge_z + 12.0f),
                                           Vec3(column.x, column.y, m_ledge_z - 12.0f), scene);
            if (top.hit && top.normal.z >= kWalkableFloorZ && std::abs(top.point.z - m_ledge_z) < 6.0f) {
                continues = true;
                break;
            }
            const TraceHit btop = sweep_box(Vec3(column.x, column.y, m_ledge_z + 12.0f), kShimmyProbeExtent,
                                            Vec3(0.0f, 0.0f, -24.0f), scene);
            if (btop.hit && !btop.start_penetrating && btop.normal.z >= kWalkableFloorZ &&
                std::abs((btop.point.z - kShimmyProbeExtent.z) - m_ledge_z) < 6.0f) {
                continues = true;
                break;
            }
        }
        if (continues) {
            const Vec3 before_pos = m_telemetry.position;
            move_swept(along * (c.grab_shimmy_speed * dt), kPawnHeight, 0.0f, scene);
            if ((m_telemetry.position - before_pos).length_sq() > 1e-8f) {
                m_telemetry.velocity = along * c.grab_shimmy_speed;
            }
            // A rail need not run the whole length of the lip: re-test what the grab tested, at the
            // same spot on top (the body hangs kPawnRadius + 1 off the face; the grab tested
            // kPawnRadius + 2 beyond a column 2 past it).
            const Vec3 ahead = m_telemetry.position + into * (2.0f * kPawnRadius + 5.0f);
            const Vec3 on_top(ahead.x, ahead.y, m_ledge_z + 0.5f);
            const bool room = has_room_at(on_top, kPawnHeight, scene) || has_room_at(on_top, kCrouchHeight, scene);
            m_grab_rail = !room && find_rail_transfer(n, m_ledge_z, scene).found;
        }
    }
}

// TdMove_GrabTransfer is PHYS_Flying with collision off. It moves in a straight line from the
// hang to the hang under the rail's top at TransferSpeed (retail: a constant (-30.8, 0, 197.1),
// 199.5 uu/s, for 0.487 s), then goes straight into VaultOver. That uses the script's "vaultOver"
// VaultType timings (VaultTimeOver 0.35, VaultTimeDown 0.3). Retail's path, the same to 0.1 uu
// in both recordings:
//   - 0.35 s up to the apex: constant horizontal speed, with the vertical speed falling linearly
//     from 671 to 0.
//   - 0.31 s over the rail and down beyond it: a constant 160 uu/s horizontally, the drop
//     speeding up.
//   - A walk out at 160 uu/s.
void ParkourController::start_grab_transfer(const RailTransfer& rail) {
    constexpr float kTransferSpeed = 200.0f;
    m_transfer_from = m_telemetry.position;
    m_transfer_time = std::max(0.05f, (rail.hang - m_transfer_from).length() / kTransferSpeed);
    m_path_p0 = rail.hang;
    m_path_p1 = rail.apex;
    m_path_p2 = rail.end;
    m_path_t1 = 0.35f;
    m_path_t2 = 0.30f;
    m_path_exit_velocity = horiz(rail.end - rail.hang).normalized() * 160.0f;
    m_path_end_move = rail.end_on_floor ? EMovement::MOVE_Walking : EMovement::MOVE_Falling;
    m_path_hang_vault = true;
    m_vault_look_lock = 0.0f;  // the vault over the rail starts in its over phase
    m_telemetry.move_state = EMovement::MOVE_GrabTransfer;
    m_telemetry.velocity = Vec3(0.0f, 0.0f, 0.0f);
    m_state_timer = 0.0f;
    m_telemetry.combat_anim_duration = m_transfer_time;
}

void ParkourController::update_grab_transfer(float dt) {
    const float t = m_state_timer;
    if (t < m_transfer_time) {
        const Vec3 target = m_transfer_from + (m_path_p0 - m_transfer_from) * (t / m_transfer_time);
        m_telemetry.velocity = (target - m_telemetry.position) / std::max(dt, 1e-5f);
        m_telemetry.position = target;
        return;
    }
    // Under the rail's top: straight into the vault over it, with no hang in between.
    m_telemetry.position = m_path_p0;
    m_telemetry.velocity = Vec3(0.0f, 0.0f, 0.0f);
    m_telemetry.move_state = EMovement::MOVE_VaultOver;
    m_takeoff_move = EMovement::MOVE_VaultOver;
    m_state_timer = 0.0f;
    m_telemetry.combat_anim_duration = m_path_t1 + m_path_t2;
    m_fall_peak_z = m_path_p1.z;
    m_air_fall_start_z = m_fall_peak_z;
    m_coil_timer = 0.0f;
}

// -----------------------------------------------------------------------------
// Speed Vault & Springboard Subsystems (TdMove_SpeedVault / TdMove_SpringBoard): timed root
// motion over a pre-computed path, collision off (the game plays the pawn along the animation).
// -----------------------------------------------------------------------------
bool ParkourController::try_initiate_springboard(const InputFrame& input, const LevelScene& scene) {
    const MovementConfig& c = m_config;
    if (m_telemetry.weapon.is_heavy) return false;
    if (input.forward <= 0.8f) return false;
    const Vec3 fwd = facing_forward();
    const Vec3 h = horiz(m_telemetry.velocity);
    const float speed = h.length();
    if (speed < 200.0f) return false;
    const Vec3 dir = h.normalized();
    if (dir.dot(fwd) < 0.8f) return false;

    // A step StepHeight +- tolerance above the feet, at least 20 away, reached within
    // CheckDistanceTime at the current speed.
    const float reach = std::min(speed * c.springboard_check_time, 400.0f);
    const Ledge step = find_ledge(dir, reach, c.springboard_step_height - c.springboard_step_tolerance,
                                  c.springboard_step_height + c.springboard_step_tolerance, scene);
    if (!step.found) return false;
    if (is_foot_move_excluded(step.top_point, step.actor_index, scene)) return false;
    if (step.wall_distance < kPawnRadius + 20.0f) return false;
    if (dir.dot(-step.normal) < 0.7f) return false;

    // A second obstacle ObstacleDistance +- 20 beyond the step's face, 80..148 above the feet.
    const Vec3 step_face = step.top_point;
    const float obstacle_z_min = m_telemetry.position.z + c.springboard_obstacle_min;
    const float obstacle_z_max = m_telemetry.position.z + c.springboard_obstacle_max;
    bool found_obstacle = false;
    Vec3 obstacle_top(0.0f, 0.0f, 0.0f);
    for (float d = c.springboard_obstacle_distance - 20.0f; d <= c.springboard_obstacle_distance + 40.0f && !found_obstacle; d += 10.0f) {
        const Vec3 column = step_face + dir * d;
        const TraceHit top = trace_ray(Vec3(column.x, column.y, obstacle_z_max + 10.0f),
                                       Vec3(column.x, column.y, step.top_z + 10.0f), scene);
        if (!top.hit || top.normal.z < kWalkableFloorZ) continue;
        if (top.point.z < obstacle_z_min || top.point.z > obstacle_z_max) continue;
        const Vec3 cand_top(column.x, column.y, top.point.z);
        if (is_hand_move_excluded(cand_top, top.actor_index, scene) ||
            is_foot_move_excluded(cand_top, top.actor_index, scene)) {
            continue;
        }
        if (!has_room_at(Vec3(column.x, column.y, top.point.z + 0.5f), kCrouchHeight, scene)) continue;
        found_obstacle = true;
        obstacle_top = cand_top;
    }
    if (!found_obstacle) return false;

    // Run in to the step (1.2 x the speed, at least 200), plant on it, plant on the obstacle,
    // launch at SpringBoardJumpZ with facing x max(400, speed - 100).
    const float run_speed = 1.2f * std::max(200.0f, speed);
    m_path_p0 = m_telemetry.position;
    m_path_p1 = Vec3(step_face.x, step_face.y, step.top_z) + dir * 12.0f;
    m_path_p2 = obstacle_top + Vec3(0.0f, 0.0f, 1.0f);
    m_path_t1 = std::max(0.08f, (m_path_p1 - m_path_p0).length() / run_speed);
    m_path_t2 = std::max(0.12f, (m_path_p2 - m_path_p1).length() / run_speed);
    Vec3 exit = fwd * std::max(400.0f, speed - 100.0f);
    exit.z = c.springboard_jump_z;
    m_path_exit_velocity = exit;
    m_path_end_move = EMovement::MOVE_Jump;
    m_path_hang_vault = false;

    m_pre_jump_momentum = speed;
    m_telemetry.move_state = EMovement::MOVE_SpringBoarding;
    m_path_planted = false;
    m_takeoff_move = EMovement::MOVE_SpringBoarding;
    m_state_timer = 0.0f;
    m_telemetry.combat_anim_duration = m_path_t1 + m_path_t2;
    m_telemetry.grounded = false;
    m_base_actor = -1;
    m_fall_peak_z = m_telemetry.position.z;
    set_stance(kEyeHeightStand);
    return true;
}

bool ParkourController::try_initiate_vault(const InputFrame& input, const LevelScene& scene) {
    const MovementConfig& c = m_config;
    const bool grounded = m_telemetry.grounded;
    const EMovement st = m_telemetry.move_state;
    const Vec3 fwd = facing_forward();
    const Vec3 h = horiz(m_telemetry.velocity);
    const float speed = h.length();

    // On ground, vault requires forward intent (> 0.8); in mid-air, allow vaulting whenever holding
    // forward (> 0.25) OR pressing/holding Jump while carrying horizontal momentum toward the obstacle.
    if (grounded) {
        if (input.forward <= 0.8f) return false;
    } else {
        if (input.forward <= 0.25f && !jump_pressed() && !input.jump && h.dot(fwd) < 120.0f) return false;
    }

    // TdMove_SpeedVault / TdMove_VaultOver (DefaultPawnMovement.ini VaultTypes[0..5]):
    // VaultTypes[0] (autostepuprightleg, 0..48 uu) allows MinSpeedZ = -600.0; VaultTypes[1..3] allow
    // MinSpeedZ = -300.0 (e.g. jumping from 2..4 m away and reaching the railing just past jump apex!).
    if (!grounded) {
        if (st == EMovement::MOVE_DodgeJump || st == EMovement::MOVE_180TurnInAir ||
            st == EMovement::MOVE_MeleeAir || st == EMovement::MOVE_MeleeWallrun) {
            return false;
        }
        if (m_telemetry.velocity.z < -450.0f) return false;
    }

    // TimeToHandPlant: the ledge must be reachable within 0.4 s at the current speed (300 at least).
    const float reach = std::max(speed, 300.0f) * c.vault_max_handplant_time - kPawnRadius;
    const float min_rise = grounded ? kMaxStepHeight : 12.0f;
    const float max_rise = kVaultHighMaxHeight;
    const Ledge ledge = find_ledge(fwd, reach, min_rise, max_rise, scene);
    if (!ledge.found) return false;
    if (fwd.dot(ledge.normal) > -0.5f) return false;  // view facing the obstacle (not glancing along a wallrun wall)
    if (m_last_wallrun_normal.length_sq() > 0.5f && ledge.normal.dot(m_last_wallrun_normal) > 0.9f &&
        m_illegal_wall_timer > 0.0f && st != EMovement::MOVE_WallClimbing) {
        return false;
    }
    const float handplant = ledge.top_z - m_telemetry.position.z;
    if (handplant < min_rise || handplant > max_rise) return false;
    if (!grounded && handplant >= 48.0f && m_telemetry.velocity.z < -300.0f) return false;
    if (!grounded && handplant >= kVaultHighMinHeight && m_telemetry.velocity.z < 50.0f) return false;

    const Vec3 dir = horiz(fwd).normalized();
    const Vec3 into = -ledge.normal;
    // When handplant < 48 in mid-air (feet already jumped up near the railing top), verify the obstacle
    // actually rises >= 42 uu above the floor in front of it so flat ground / low curbs never trigger a vault.
    if (!grounded && handplant < 48.0f) {
        const Vec3 front_probe = ledge.top_point - into * 24.0f;
        const TraceHit gnd = trace_ray(Vec3(front_probe.x, front_probe.y, ledge.top_z + 4.0f),
                                       Vec3(front_probe.x, front_probe.y, ledge.top_z - 160.0f), scene);
        if (gnd.hit && (ledge.top_z - gnd.point.z) < 42.0f) return false;
    }

    const float end_dist = std::max(48.0f, speed * 0.3f);
    const float top_z = ledge.top_z;
    const float probe_extra = std::max(0.0f, (ledge.top_point - m_telemetry.position).dot(into) - ledge.wall_distance);
    Vec3 face = ledge.top_point - into * probe_extra;  // back on the front face line
    face.z = top_z;
    m_telemetry.ledge_known = true;
    m_telemetry.ledge_point = face;
    m_telemetry.ledge_wall_normal = ledge.normal;
    m_telemetry.ledge_top_normal = ledge.top_normal;

    // TdMove_SpeedVault.FindValidOntoEndLocation: the body (a 1.4 x radius box, full height) has to
    // pass VaultClearObjectHeight above the ledge for at least 32 (low ledge) / 64 units along the
    // move before anything blocks it; a wall or panel right behind the ledge means no vault.
    {
        constexpr float kVaultClearObjectHeight = 35.0f;
        const Vec3 body_extent(kPawnRadius * 1.4f, kPawnRadius * 1.4f, kPawnHeight * 0.5f);
        Vec3 start = face;
        start.z = top_z + kVaultClearObjectHeight + body_extent.z;
        const TraceHit block = sweep_box(start, body_extent, into * end_dist, scene);
        if (block.hit) {
            float width = block.start_penetrating ? 0.0f : (block.point - start).dot(into) + body_extent.x;
            if (width <= body_extent.x + 1.0f) {
                const Vec3 small_extent(10.0f, 10.0f, 10.0f);
                Vec3 small_start = start;
                small_start.z = top_z + kVaultClearObjectHeight + small_extent.z;
                const TraceHit small = sweep_box(small_start, small_extent, into * end_dist, scene);
                if (!small.hit) width = end_dist;
                else width = small.start_penetrating ? 0.0f : (small.point - small_start).dot(into) + small_extent.x;
            }
            if (width < (handplant <= 48.0f ? 32.0f : 64.0f)) return false;
        }
    }

    // Onto or over: the far edge within MaxLedgeWidth = clamp(speed x 0.2, 60, 180) and the floor
    // beyond (down to 240 below the top) lower than the top - 64 means over.
    const float max_width = std::clamp(speed * 0.2f, 60.0f, 180.0f);
    bool over = false;
    float far_d = max_width;
    float floor_beyond = top_z - 240.0f;
    bool floor_found = false;
    for (float d = 10.0f; d <= max_width; d += 10.0f) {
        const Vec3 column = face + into * d;
        const TraceHit t = trace_ray(Vec3(column.x, column.y, top_z + 8.0f), Vec3(column.x, column.y, top_z - 240.0f), scene);
        if (!t.hit || t.point.z < top_z - 64.0f) {
            over = true;
            far_d = d;
            floor_found = t.hit;
            floor_beyond = t.hit ? t.point.z : top_z - 240.0f;
            break;
        }
    }

    const float exit_speed = std::clamp(speed + c.vault_speed_add, c.vault_min_speed, c.ground_speed);
    m_path_p0 = m_telemetry.position;
    if (over) {
        m_path_p1 = face + into * (far_d * 0.5f) + Vec3(0.0f, 0.0f, c.vault_ledge_offset_z);
        const Vec3 end = face + into * (far_d + std::max(end_dist, kPawnRadius + 24.0f));
        const TraceHit land_trace = trace_ray(Vec3(end.x, end.y, top_z + 32.0f),
                                              Vec3(end.x, end.y, top_z - 240.0f), scene);
        if (land_trace.hit && land_trace.normal.z >= kWalkableFloorZ) {
            floor_found = true;
            floor_beyond = land_trace.point.z;
        }
        const float target_z = floor_found ? floor_beyond : (top_z - 20.0f);
        if (!has_room_at(Vec3(end.x, end.y, target_z + 1.0f), kCrouchHeight, scene)) {
            return false;
        }
        m_path_p2 = Vec3(end.x, end.y, target_z);
        m_path_end_move = floor_found ? EMovement::MOVE_Landing : EMovement::MOVE_Falling;
    } else {
        // Onto: room for the body on top.
        if (!has_room_at(ledge.top_point + into * (kPawnRadius + 4.0f) + Vec3(0.0f, 0.0f, 0.5f), kCrouchHeight, scene)) {
            return false;
        }
        m_path_p1 = face + into * (kPawnRadius + 6.0f) + Vec3(0.0f, 0.0f, c.vault_ledge_offset_z);
        const Vec3 end = face + into * (kPawnRadius + 6.0f + end_dist * 0.5f);
        m_path_p2 = Vec3(end.x, end.y, top_z);
        m_path_end_move = EMovement::MOVE_Walking;
    }
    if (is_hand_move_excluded(m_path_p1, -1, scene) ||
        is_hand_move_excluded(m_path_p1 + Vec3(0.0f, 0.0f, 40.0f), -1, scene)) {
        return false;
    }
    if (!grounded) {
        consume_jump();
    }
    const bool high_vault = (handplant >= kVaultHighMinHeight);
    // VaultOverHigh / VaultOntoHigh have an up phase (VaultTimeUp 0.28 / 0.27 s) with look input
    // ignored until the hand plant; the other vault types start in the over phase.
    m_vault_look_lock = high_vault ? (over ? 0.28f : 0.27f) : 0.0f;
    m_path_t1 = high_vault ? 0.48f : c.vault_time_over;
    m_path_t2 = high_vault ? 0.40f : c.vault_time_down;
    const Vec3 last_leg = horiz(m_path_p2 - m_path_p1);
    const float leg_speed = std::max(exit_speed * 0.5f, last_leg.length() / m_path_t2);
    m_path_exit_velocity = dir * std::min(leg_speed, c.ground_speed);
    if (over) m_path_exit_velocity = dir * exit_speed;
    m_path_hang_vault = false;

    m_pre_jump_momentum = speed;
    // Retail records every vault - onto or over, walking pace or full sprint - as MOVE_VaultOver
    // (3,454 samples in 61 runs across the recordings); MOVE_SpeedVaulting never appears.
    m_telemetry.move_state = EMovement::MOVE_VaultOver;
    // TdMove_SpeedVault.VaultTypes[].AnimName: by the ledge's height, whether she lands on top, and
    // (for the low step up) how little momentum she has.
    set_move_anim(handplant < 48.0f ? "autostepuprightleg"
                  : high_vault ? (over ? "VaultOverHigh" : "VaultOntoHigh")
                  : over ? "VaultOver"
                  : (speed <= 200.0f && handplant < 64.0f ? "stepuprightleg88" : "VaultOnto"));
    m_takeoff_move = EMovement::MOVE_VaultOver;
    m_state_timer = 0.0f;
    m_telemetry.combat_anim_duration = m_path_t1 + m_path_t2;
    m_telemetry.grounded = false;
    m_base_actor = -1;
    m_fall_peak_z = top_z + c.vault_ledge_offset_z;
    m_air_fall_start_z = m_fall_peak_z;
    m_coil_timer = 0.0f;
    set_stance(kEyeHeightStand);
    return true;
}

void ParkourController::update_vault(const InputFrame& input, float dt, const LevelScene& scene) {
    (void)input;
    const float t = m_state_timer;
    Vec3 target;
    if (t < m_path_t1) {
        const float s = t / std::max(m_path_t1, 1e-4f);
        target = m_path_p0 + (m_path_p1 - m_path_p0) * s;
        // Out of a GrabTransfer the rise eases out: retail's vertical speed falls linearly to 0.
        if (m_path_hang_vault) target.z = m_path_p0.z + (m_path_p1.z - m_path_p0.z) * s * (2.0f - s);
    } else if (t < m_path_t1 + m_path_t2) {
        // TdMove_SpringBoard.ReachedPreciseLocation: the foot is on the step.
        if (m_telemetry.move_state == EMovement::MOVE_SpringBoarding && !m_path_planted) {
            m_path_planted = true;
            set_move_anim("@reached");
        }
        const float s = (t - m_path_t1) / std::max(m_path_t2, 1e-4f);
        target = m_path_p1 + (m_path_p2 - m_path_p1) * s;
        // ... and the drop beyond the rail eases in from the apex.
        if (m_path_hang_vault) target.z = m_path_p1.z + (m_path_p2.z - m_path_p1.z) * s * s;
    } else {
        // End of the animation: the pawn carries on with the move's exit velocity.
        m_telemetry.position = m_path_p2;
        m_telemetry.velocity = m_path_exit_velocity;
        m_path_hang_vault = false;
        const EMovement end = m_path_end_move;
        if (end == EMovement::MOVE_Jump) {
            // Springboard launch.
            m_last_jump_location = m_telemetry.position;
            leave_ground(EMovement::MOVE_Jump);
            m_takeoff_move = EMovement::MOVE_SpringBoarding;
        } else if (end == EMovement::MOVE_Walking) {
            m_telemetry.move_state = EMovement::MOVE_Walking;
            m_telemetry.grounded = true;
            m_sprint_energy = std::max(0.0f, m_telemetry.velocity.length_xy() - m_config.speed_max_base_velocity);
        } else {
            // Over: a normal landing on the floor beyond (or a fall if there was none).
            m_telemetry.grounded = false;
            m_telemetry.move_state = EMovement::MOVE_Falling;
            m_takeoff_move = EMovement::MOVE_VaultOver;
            m_telemetry.velocity.z = -50.0f;
        }
        m_state_timer = 0.0f;
        return;
    }
    m_telemetry.velocity = (target - m_telemetry.position) / std::max(dt, 1e-5f);
    m_telemetry.position = target;
    if (m_telemetry.position.z > m_fall_peak_z) m_fall_peak_z = m_telemetry.position.z;
    (void)scene;
}

// -----------------------------------------------------------------------------
// Zipline, Swing Bar & Balance Beam Subsystems
// -----------------------------------------------------------------------------
bool ParkourController::try_initiate_zipline(const LevelScene& scene) {
    if (m_zipline_cooldown > 0.0f || m_telemetry.weapon.is_heavy) return false;
    const Vec3 centre = m_telemetry.position + Vec3(0.0f, 0.0f, 0.5f * kPawnHeight);  // retail's Location
    const Vec3 hand = m_telemetry.position + Vec3(0.0f, 0.0f, kZipHangHeight);
    const Vec3 body_fwd = Rotator::from_degrees(0.0f, m_pawn_yaw, 0.0f).forward();
    for (size_t ai = 0; ai < scene.actors.size(); ++ai) {
        const LevelActor& act = scene.actors[ai];
        if (!act.is_zipline || act.end_point.length_sq() <= 1.0f) continue;
        std::vector<Vec3> pts = act.spline_points;
        if (pts.size() < 2) pts = {act.location, act.end_point};
        if ((pts.back() - pts.front()).length() < 50.0f) continue;

        // TdZiplineVolume.PawnUpdate: the pawn is inside the volume wrapped around the cable.
        float param = 0.0f;
        if (closest_on_cable(pts, hand, param).distance(hand) >= kZipGrabReach) continue;
        // TdMove_IntoZipLine.CanDoMove: the body faces the way the cable is ridden, the pawn is clear
        // of the landing strip at its bottom end, and it is not back on the cable it let go of within
        // SameZipLineRedoMoveTime. (Retail grabs only from the air; the port also takes a cable within
        // reach from the ground.)
        Vec3 move_dir = act.move_direction;
        if (move_dir.length_sq() < 0.25f) move_dir = horiz(pts.back() - pts.front()).normalized();
        if (move_dir.dot(body_fwd) <= 0.0f) continue;
        if (horiz(centre - pts.back()).length() < kZipLandingStrip) continue;
        const int32_t id = static_cast<int32_t>(ai);
        if (id == m_zip_last_actor && m_telemetry.sim_time - m_zip_last_stop_time < kZipSameLineRedoTime) continue;

        // TdMove_IntoZipLine.StartMove: the hands go to the cable 100 ahead (along MoveDirection) of
        // the point nearest the pawn, its centre HangOffset below them. Retail glides there at
        // IntoClimbSpeed (about 0.3 s); the port puts the pawn there at once.
        float enter_param = 0.0f;
        const Vec3 nearest = closest_on_cable(pts, centre, enter_param);
        const Vec3 grab = closest_on_cable(pts, nearest + move_dir * 100.0f, enter_param);
        // The body has to fit there (retail only grabs from the air, with the cable clear below it).
        if (!has_room_at(grab - Vec3(0.0f, 0.0f, kZipHangHeight), kPawnHeight, scene)) continue;
        // ReachedPreciseLocation: the body turns down the cable, and the ride starts with the part of
        // the horizontal velocity along it (none out of a fall faster than ZVelocityFallLimit).
        const Vec3 slope = cable_slope(pts, enter_param);
        Vec3 v2d = horiz(m_telemetry.velocity);
        if (m_telemetry.velocity.z < kZipFallLimitZ) v2d = Vec3(0.0f, 0.0f, 0.0f);
        const float v2d_len = v2d.length();
        m_telemetry.velocity = v2d_len > 1e-3f ? slope * (v2d_len * std::max(slope.dot(v2d / v2d_len), 0.0f))
                                               : Vec3(0.0f, 0.0f, 0.0f);
        m_telemetry.position = grab - Vec3(0.0f, 0.0f, kZipHangHeight);
        m_telemetry.move_state = EMovement::MOVE_ZipLine;
        m_telemetry.grounded = false;
        m_base_actor = -1;
        m_coil_timer = 0.0f;
        m_state_timer = 0.0f;
        m_zip_points = std::move(pts);
        m_zip_param = enter_param;
        m_zip_status = 0;
        m_zip_impact_timer = 0.0f;
        m_zip_body_yaw = yaw_of(slope);
        m_zip_last_actor = id;     // LastZipLineVolumeName
        m_zip_look_assist = true;  // TdMove_ZipLine.StartMove: bZipLineLookAssist
        m_zip_look_at = grab - Vec3(0.0f, 0.0f, kZipTraceDrop) + slope * kZipTraceReach[0];
        m_zip_exit_z = 1e30f;
        set_stance(kEyeHeightStand);
        return true;
    }
    return false;
}

void ParkourController::stop_zipline(EMovement next) {
    m_zip_status = 0;                             // ZLS_Moving
    m_zip_impact_timer = 0.0f;
    m_zip_look_assist = false;
    m_zip_last_stop_time = m_telemetry.sim_time;  // LastStopMoveTime
    m_zip_exit_z = m_telemetry.position.z;
    m_zipline_cooldown = kZipRedoTime;
    leave_ground(next);                           // SetPhysics(PHYS_Falling) resets EnterFallingHeight
    m_fall_peak_z -= 80.0f;                       // EnterFallingHeight -= 80.0
}

// TdMove_ZipLine's native tick (MirrorsEdge.exe 0x1209400): each physics step the velocity is steered
// along the cable polyline (SplineLocations), at least MinZipVelocity; gravity pulls along the cable's
// pitch and an acceleration of at least MinZipAcceleration drives the pawn down it. Then the pawn
// flies (PHYS_Flying) with that velocity. A box trace ahead of the hands stops the ride at the end
// wall: ZLS_CloseToEnd at 600, the impact (held still for 0.8 s, then a drop) at 20.
void ParkourController::update_zipline(const InputFrame& input, float dt, const LevelScene& scene) {
    // TdPlayerMoveManager.HandleMoveAction: a crouch press lets go, keeping the ride's velocity (the
    // port also lets go while crouch is held). Retail ignores jump on the cable; the port hops off.
    if (jump_pressed()) {
        consume_jump();
        m_pre_jump_momentum = m_telemetry.velocity.length_xy();
        m_telemetry.velocity.z += 250.0f;
        m_last_jump_location = m_telemetry.position;
        stop_zipline(EMovement::MOVE_Jump);
        return;
    }
    if (m_crouch_pressed || (input.crouch && m_state_timer > 0.12f)) {
        m_crouch_pressed = false;
        stop_zipline(EMovement::MOVE_Falling);
        return;
    }
    // PlayForwardImpact's SetTimer(0.8): OnTimer drops the pawn off the cable.
    if (m_zip_status == 2) {
        m_zip_impact_timer -= dt;
        if (m_zip_impact_timer <= 0.0f) {
            stop_zipline(EMovement::MOVE_Falling);
            return;
        }
    }
    const std::vector<Vec3>& pts = m_zip_points;
    const int n = static_cast<int>(pts.size());
    if (n < 2) {
        stop_zipline(EMovement::MOVE_Falling);
        return;
    }

    // Gravity, scaled by how steeply the pawn is moving.
    Vec3 vel = m_telemetry.velocity;
    const Vec3 vel_dir = vel.normalized();
    vel.z -= std::abs(vel_dir.z) * m_config.script_gravity * dt;

    // This step runs from the hands back onto the cable and along it, towards the first spline point
    // at least a step away; with none left the pawn flies off the end of the cable.
    const Vec3 hand = m_telemetry.position + Vec3(0.0f, 0.0f, kZipHangHeight);
    closest_on_cable(pts, hand, m_zip_param);  // CurrentParamOnCurve
    const float step = std::max(vel.length(), kZipMinVelocity) * dt;
    int i = static_cast<int>(m_zip_param) + 1;
    while (i < n && (pts[i] - hand).length() < step) ++i;
    if (i >= n) {
        m_telemetry.velocity = vel;
        stop_zipline(EMovement::MOVE_Falling);
        return;
    }
    const Vec3 seg = pts[i] - pts[i - 1];
    const Vec3 to_next = pts[i] - hand;
    const float seg_len2 = seg.length_sq();
    const Vec3 to_line = seg_len2 > 1e-6f ? to_next - seg * (to_next.dot(seg) / seg_len2) : to_next;
    const float along = std::sqrt(std::max(0.0f, step * step - to_line.length_sq()));
    vel = (to_line + seg.normalized() * along) / dt;
    if (vel.length() < kZipMinVelocity) {
        vel = (vel.length_sq() > 1e-6f ? vel.normalized() : vel_dir) * kZipMinVelocity;
    }
    const Vec3 dir = vel.normalized();
    Vec3 accel = dir * std::max(kZipMinAcceleration, std::abs(m_config.script_gravity * dir.z));

    // The impact trace ahead of the hands; its far end is where the view is drawn (CurrentLookAtPoint).
    // A box that starts out touching the anchor near the top of the cable does not count as a wall.
    const Vec3 trace_start = hand - Vec3(0.0f, 0.0f, kZipTraceDrop);
    const TraceHit ahead = sweep_box(trace_start, Vec3(kPawnRadius, kPawnRadius, kZipTraceHalfHeight),
                                     dir * kZipTraceReach[m_zip_status], scene);
    if (ahead.hit && (!ahead.start_penetrating || m_zip_status > 0)) {
        if (m_zip_status == 0) {
            m_zip_status = 1;  // PrepareForForwardImpact (ziplineintohitwall)
        } else if (m_zip_status == 1) {
            m_zip_status = 2;  // PlayForwardImpact (ziplinehitwall): no look input for 0.8 s
            m_zip_impact_timer = kZipImpactTime;
            camera_ignore_look(kZipImpactTime);
        } else {
            vel = Vec3(0.0f, 0.0f, 0.0f);  // against the wall: held still
            accel = Vec3(0.0f, 0.0f, 0.0f);
        }
    }
    m_zip_look_at = trace_start + dir * kZipTraceReach[0];

    // PHYS_Flying: friction, the acceleration, a swept move sliding along what it hits, and the
    // velocity taken from how far the pawn actually moved.
    vel = vel * (1.0f - kZipFlyingFriction * dt) + accel * dt;
    const Vec3 before = m_telemetry.position;
    const TraceHit moved = move_and_slide(vel * dt, kPawnHeight, 0.0f, scene);
    m_telemetry.velocity = (m_telemetry.position - before) / dt;
    // TdMove_ZipLine.HitWall: running into a floor or a slope knocks the pawn off the cable
    // (TdMove_Stumble, a fall here).
    if (moved.hit && moved.normal.z > 0.1f) stop_zipline(EMovement::MOVE_Falling);
}

bool ParkourController::try_initiate_swing_bar(const InputFrame& input, const LevelScene& scene) {
    (void)input;
    if (m_swing_cooldown > 0.0f || m_telemetry.weapon.is_heavy) return false;
    if (m_telemetry.move_state == EMovement::MOVE_Swing) return false;

    constexpr float kSwingPendulumLength = 120.0f;
    const Vec3 hand_pos = m_telemetry.position + Vec3(0.0f, 0.0f, 155.0f);
    const Vec3 fwd = facing_forward();
    const Vec3 h_vel = horiz(m_telemetry.velocity);

    for (const auto& act : scene.actors) {
        if (!act.is_swing_bar || act.end_point.length_sq() < 1.0f) continue;
        const Vec3 a = act.location;
        const Vec3 b = act.end_point;
        const Vec3 ab = b - a;
        const float bar_len = ab.length();
        if (bar_len < 40.0f) continue;

        const Vec3 bar_axis = ab / bar_len;
        const float s = (hand_pos - a).dot(bar_axis);
        if (s < -25.0f || s > bar_len + 25.0f) continue;

        const float clamped_s = std::clamp(s, 15.0f, std::max(15.0f, bar_len - 15.0f));
        const Vec3 anchor = a + bar_axis * clamped_s;
        if (hand_pos.distance(anchor) > 115.0f) continue;

        // Horizontal swing plane normal perpendicular to the bar
        Vec3 swing_dir(-bar_axis.y, bar_axis.x, 0.0f);
        if (swing_dir.length_sq() < 1e-6f) swing_dir = Vec3(1.0f, 0.0f, 0.0f);
        swing_dir = swing_dir.normalized();
        if (h_vel.length_sq() > 25.0f) {
            if (swing_dir.dot(h_vel) < 0.0f) swing_dir = -swing_dir;
        } else if (swing_dir.dot(fwd) < 0.0f) {
            swing_dir = -swing_dir;
        }

        // Initial pendulum angle measured from straight down (-Z) toward +swing_dir
        const Vec3 rel = (m_telemetry.position + Vec3(0.0f, 0.0f, 90.0f)) - anchor;
        m_swing_anchor = anchor;
        m_swing_bar_start = a;
        m_swing_bar_end = b;
        m_swing_dir = swing_dir;
        m_swing_angle = std::clamp(std::atan2(rel.dot(swing_dir), std::max(25.0f, -rel.z)), -1.10f, 1.10f);
        const float incoming_speed = std::max(h_vel.dot(swing_dir), 320.0f);
        m_swing_angular_vel = std::clamp(incoming_speed / kSwingPendulumLength, 2.8f, 4.25f);

        m_telemetry.position = m_swing_anchor
                             + m_swing_dir * (kSwingPendulumLength * std::sin(m_swing_angle))
                             - Vec3(0.0f, 0.0f, kSwingPendulumLength * std::cos(m_swing_angle) + 90.0f);  // the capsule centre is on the arc (retail: radius 120 to 0.001), the feet 90 under it
        m_telemetry.velocity = m_swing_dir * (m_swing_angular_vel * kSwingPendulumLength * std::cos(m_swing_angle));
        m_telemetry.move_state = EMovement::MOVE_Swing;
        m_telemetry.grounded = false;
        m_base_actor = -1;
        m_state_timer = 0.0f;
        m_coil_timer = 0.0f;
        m_fall_peak_z = m_telemetry.position.z;
        m_air_fall_start_z = m_telemetry.position.z;
        if (fwd.dot(swing_dir) < 0.5f) {
            m_telemetry.yaw_deg = yaw_of(swing_dir);
        }
        set_stance(kEyeHeightStand);
        return true;
    }
    return false;
}

void ParkourController::update_swing_bar(const InputFrame& input, float dt, const LevelScene& scene) {
    (void)scene;
    constexpr float kSwingPendulumLength = 120.0f;
    constexpr float kExitVelocityModifier = 600.0f;
    constexpr float kMaxSwingAngularVel = 4.25f;
    constexpr float kMaxSwingAngle = 1.38f;

    // 180° flip on bar (TdMove_Swing 180 turn)
    if (input.turn_180 && !m_prev_turn_180 && m_turn_timer <= 0.0f) {
        m_swing_dir = -m_swing_dir;
        m_swing_angle = -m_swing_angle;
        m_swing_angular_vel = -m_swing_angular_vel;
        m_turn_timer = m_config.turn_180_time;
        m_turn_total = m_config.turn_180_time;
        m_turn_target_yaw = m_telemetry.yaw_deg + 180.0f;
    }

    // Jump off bar (TdMove_Swing.JumpOff: ExitVelocityModifier = 600.f)
    if (jump_pressed()) {
        consume_jump();
        m_swing_cooldown = 0.55f;
        const Vec3 fwd = facing_forward();
        const Vec3 launch_dir = (fwd.dot(m_swing_dir) >= 0.35f) ? horiz(fwd).normalized() : m_swing_dir;
        const float up_speed = 420.0f + 160.0f * std::max(0.0f, std::sin(m_swing_angle));
        m_telemetry.velocity = launch_dir * kExitVelocityModifier + Vec3(0.0f, 0.0f, up_speed);
        m_last_jump_location = m_telemetry.position;
        leave_ground(EMovement::MOVE_Jump);
        return;
    }

    // Let go / drop with Crouch or Shift (TdMove_Swing.LetGo)
    if (m_crouch_pressed) {
        m_crouch_pressed = false;
        m_swing_cooldown = 0.55f;
        const float tang_speed = std::clamp(m_swing_angular_vel * kSwingPendulumLength, -420.0f, 420.0f);
        m_telemetry.velocity = m_swing_dir * tang_speed;
        leave_ground(EMovement::MOVE_Falling);
        return;
    }

    // Lateral shimmy along the bar (TdMove_Swing.UpdateShimmy)
    const Vec3 ab = m_swing_bar_end - m_swing_bar_start;
    const float bar_len = ab.length();
    if (bar_len > 30.0f && std::abs(input.strafe) > 0.3f) {
        const Vec3 bar_axis = ab / bar_len;
        const float move_sign = sign_of(bar_axis.dot(facing_right() * input.strafe));
        float s = (m_swing_anchor - m_swing_bar_start).dot(bar_axis) + move_sign * 90.0f * dt;
        s = std::clamp(s, 15.0f, std::max(15.0f, bar_len - 15.0f));
        m_swing_anchor = m_swing_bar_start + bar_axis * s;
    }

    // Pendulum angular dynamics + W/S swing pumping
    const float gravity_torque = -(m_config.gravity / kSwingPendulumLength) * std::sin(m_swing_angle);
    float pump_accel = 0.0f;
    if (std::abs(input.forward) > 0.2f) {
        const float pump_dir = (std::abs(m_swing_angular_vel) > 0.25f)
            ? sign_of(m_swing_angular_vel) * sign_of(input.forward)
            : sign_of(input.forward);
        pump_accel = pump_dir * 4.5f;
    } else {
        m_swing_angular_vel *= std::max(0.0f, 1.0f - 0.15f * dt);
    }

    m_swing_angular_vel = std::clamp(m_swing_angular_vel + (gravity_torque + pump_accel) * dt,
                                     -kMaxSwingAngularVel, kMaxSwingAngularVel);
    m_swing_angle += m_swing_angular_vel * dt;
    if (m_swing_angle > kMaxSwingAngle) {
        m_swing_angle = kMaxSwingAngle;
        if (m_swing_angular_vel > 0.0f) m_swing_angular_vel = -m_swing_angular_vel * 0.85f;
    } else if (m_swing_angle < -kMaxSwingAngle) {
        m_swing_angle = -kMaxSwingAngle;
        if (m_swing_angular_vel < 0.0f) m_swing_angular_vel = -m_swing_angular_vel * 0.85f;
    }

    const Vec3 prev_pos = m_telemetry.position;
    const Vec3 target_pos = m_swing_anchor
                          + m_swing_dir * (kSwingPendulumLength * std::sin(m_swing_angle))
                          - Vec3(0.0f, 0.0f, kSwingPendulumLength * std::cos(m_swing_angle) + 90.0f);
    m_telemetry.position = target_pos;
    m_telemetry.velocity = (target_pos - prev_pos) / std::max(dt, 1e-4f);
    m_fall_peak_z = m_telemetry.position.z;
    m_air_fall_start_z = m_telemetry.position.z;
}

bool ParkourController::try_initiate_climb(const InputFrame& input, const LevelScene& scene) {
    if (m_telemetry.weapon.is_heavy || input.forward < -0.3f) return false;
    if (m_telemetry.grounded && input.forward <= 0.3f) return false;

    const float max_horiz = m_telemetry.grounded ? 95.0f : 135.0f;
    for (const auto& act : scene.actors) {
        if (!act.is_ladder || act.end_point.length_sq() < 1.0f) continue;
        if (!std::isfinite(act.location.x) || !std::isfinite(act.location.y) || !std::isfinite(act.location.z) ||
            !std::isfinite(act.end_point.x) || !std::isfinite(act.end_point.y) || !std::isfinite(act.end_point.z)) {
            continue;
        }
        const Vec3 base = (act.location.z <= act.end_point.z) ? act.location : act.end_point;
        const Vec3 top  = (act.location.z <= act.end_point.z) ? act.end_point : act.location;
        if (!(top.z - base.z >= 60.0f)) continue;

        // Cooldown applies only to re-grabbing the exact same pipe/ladder after letting go,
        // so jumping laterally between adjacent drainpipes catches the next pipe immediately.
        if (m_climb_cooldown > 0.0f && horiz(m_climb_base - base).length() < 80.0f) continue;

        if (!(m_telemetry.position.z >= base.z - 95.0f && m_telemetry.position.z <= top.z - 20.0f)) continue;
        if (act.is_pipe && m_telemetry.position.z > pipe_highest_catch_feet_z(top)) continue;
        const float h_dist = horiz(m_telemetry.position - base).length();
        if (!(h_dist <= max_horiz)) continue;

        // Native UE3 TdLadderVolume: serialized WallNormal points outward toward the climber;
        // fallback to actor Rotation (faces into the wall/ladder) or scene wall probes.
        Vec3 wall_out(0.0f, 0.0f, 0.0f);
        if (act.wall_normal.length_sq() > 0.25f) {
            wall_out = horiz(act.wall_normal).normalized();
        }
        if (wall_out.length_sq() < 0.25f &&
            (std::abs(act.rotation.yaw) > 1.0f || std::abs(act.rotation.pitch) > 1.0f)) {
            wall_out = horiz(-act.rotation.forward()).normalized();
        }
        if (wall_out.length_sq() < 0.25f) {
            const float mid_z = 0.5f * (base.z + top.z);
            const Vec3 probe_origin(base.x, base.y, mid_z);
            float best_frac = 1.0f;
            for (const Vec3& dir : {Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f),
                                    Vec3(1.0f, 0.0f, 0.0f), Vec3(-1.0f, 0.0f, 0.0f)}) {
                const TraceHit wh = trace_ray(probe_origin - dir * 24.0f, probe_origin + dir * 64.0f, scene);
                if (wh.hit && std::abs(wh.normal.z) < 0.4f && wh.fraction < best_frac) {
                    best_frac = wh.fraction;
                    wall_out = horiz(wh.normal).normalized();
                }
            }
        }
        if (wall_out.length_sq() < 0.25f) {
            wall_out = horiz(m_telemetry.position - base).normalized();
        }
        if (wall_out.length_sq() < 0.25f) wall_out = Vec3(0.0f, -1.0f, 0.0f);

        // Must approach from the front/side of the wall and not face away from the pipe
        if ((m_telemetry.position - base).dot(wall_out) < -25.0f) continue;
        if (facing_forward().dot(-wall_out) < -0.30f) continue;

        const Vec3 climb_xy = Vec3(base.x, base.y, 0.0f) + wall_out * 64.0f;
        // TdMove_IntoClimb.StartMove takes the closest step clamped to GetLastStep() and moves the
        // pawn onto it (SetPreciseLocation at FMax(VSize2D(Delta), 30) / 0.15 uu/s). With no way off
        // at the top, a pawn caught above the last step is left there for update_climb to lower.
        const float entry_z = act.can_exit_at_top
            ? std::clamp(m_telemetry.position.z, base.z, std::max(base.z, top.z - 35.0f))
            : std::max(m_telemetry.position.z, base.z);
        m_climb_settle_speed = std::max(horiz(m_telemetry.position - climb_xy).length(), 30.0f) / 0.15f;

        m_telemetry.position = Vec3(climb_xy.x, climb_xy.y, entry_z);
        m_telemetry.velocity = Vec3(0.0f, 0.0f, 0.0f);
        m_telemetry.move_state = EMovement::MOVE_Climb;
        m_telemetry.grounded = false;
        m_base_actor = -1;
        m_climb_base = base;
        m_climb_top = top;
        m_climb_normal = wall_out;
        m_climb_can_exit_top = act.can_exit_at_top;
        m_climb_is_pipe = act.is_pipe;
        m_telemetry.climbing_pipe = act.is_pipe;
        m_telemetry.wall_normal = wall_out;
        m_state_timer = 0.0f;
        m_coil_timer = 0.0f;
        m_fall_peak_z = m_telemetry.position.z;
        m_telemetry.yaw_deg = yaw_of(-wall_out);
        set_stance(kEyeHeightStand);
        return true;
    }
    return false;
}

void ParkourController::update_climb(const InputFrame& input, float dt, const LevelScene& scene) {
    if (m_telemetry.position.z > m_fall_peak_z) {
        m_fall_peak_z = m_telemetry.position.z;
    }

    const Vec3 into = -m_climb_normal;
    const Vec3 climb_xy = Vec3(m_climb_base.x, m_climb_base.y, 0.0f) + m_climb_normal * 64.0f;
    // With no way off at the top (bCanExitAtTop false), TdMove_Climb.HandleClimbAction stops on
    // GetLastStep() and never calls ExitAtTop: on a pipe that keeps her hands under its end.
    const float max_climb_z = m_climb_can_exit_top
        ? std::max(m_climb_base.z, m_climb_top.z - 65.0f)
        : climb_last_step_feet_z(m_climb_base, m_climb_top, m_climb_is_pipe);
    // How far the ladder goes on, for the animation: a pipe's last rung is climbed differently.
    m_telemetry.climb_top = std::max(0.0f, max_climb_z - m_telemetry.position.z);
    m_telemetry.climb_bottom = std::max(0.0f, m_telemetry.position.z - m_climb_base.z);
    m_telemetry.position.x = climb_xy.x;
    m_telemetry.position.y = climb_xy.y;

    // Caught above the last step (a jump onto a capped pipe): IntoClimb's precise move lowers her
    // onto it, and nothing else is taken until she is there (ClimbState 2 / bUsePreciseLocation).
    if (!m_climb_can_exit_top && m_telemetry.position.z > max_climb_z) {
        m_telemetry.position.z = std::max(max_climb_z, m_telemetry.position.z - m_climb_settle_speed * dt);
        m_telemetry.velocity = Vec3(0.0f, 0.0f, 0.0f);
        return;
    }

    // Jump off pipe/ladder (TdMove_Climb.HandleMoveAction(MA_Jump)):
    // Aiming away from the wall or holding A/D jumps toward adjacent pipes/ledges (e.g. Tutorial Stage 11 pipes);
    // facing straight into the pipe with neutral strafe drops/pushes cleanly off.
    if (jump_pressed()) {
        consume_jump();
        m_climb_cooldown = 0.65f;
        m_last_wallrun_normal = m_climb_normal;
        m_illegal_wall_timer = 0.65f;
        const Vec3 fwd = facing_forward();
        const bool aiming_away = (fwd.dot(into) < 0.78f);
        const bool strafing = (std::abs(input.strafe) > 0.25f);
        if (aiming_away || strafing || input.forward < -0.25f) {
            Vec3 launch_dir = aiming_away
                ? fwd
                : (facing_right() * sign_of(input.strafe) + m_climb_normal * 0.28f).normalized();
            if (launch_dir.dot(m_climb_normal) < 0.28f) {
                launch_dir = (launch_dir - m_climb_normal * launch_dir.dot(m_climb_normal) + m_climb_normal * 0.28f).normalized();
            }
            if (launch_dir.length_sq() < 0.1f) launch_dir = m_climb_normal;
            m_telemetry.velocity = launch_dir * 420.0f + Vec3(0.0f, 0.0f, 440.0f);
            m_last_jump_location = m_telemetry.position;
            leave_ground(EMovement::MOVE_Jump);
        } else {
            m_telemetry.velocity = m_climb_normal * 180.0f + Vec3(0.0f, 0.0f, 120.0f);
            leave_ground(EMovement::MOVE_Falling);
        }
        return;
    }

    if (input.forward > 0.2f) {
        constexpr float kClimbUpSpeed = 190.0f;
        if (!m_climb_can_exit_top) {
            // Up to the last step and no further (e.g. the Tutorial Stage 11 drainpipes, which end
            // under the roof's wire fence).
            m_telemetry.position.z = std::min(max_climb_z, m_telemetry.position.z + kClimbUpSpeed * dt);
            m_telemetry.velocity = (m_telemetry.position.z < max_climb_z)
                ? Vec3(0.0f, 0.0f, kClimbUpSpeed)
                : Vec3(0.0f, 0.0f, 0.0f);
            return;
        }
        m_telemetry.velocity = Vec3(0.0f, 0.0f, kClimbUpSpeed);
        m_telemetry.position.z += kClimbUpSpeed * dt;

        // Reaching top of pipe/ladder (TdMove_Climb.ExitAtTop):
        // In UE3 TdLadderVolume, GetLastStep().Z = End.Z - 96.0 (side rails extend ~96 UU above the platform),
        // so the top catwalk/roof surface can lie down to m_climb_top.z - 160.0f.
        const float exit_check_z = m_climb_top.z - 65.0f;
        if (m_telemetry.position.z >= exit_check_z) {
            for (float dist_in : {36.0f, 52.0f, 68.0f, 84.0f, 104.0f, 128.0f, 152.0f}) {
                const Vec3 probe_xy = Vec3(m_climb_base.x, m_climb_base.y, 0.0f) + into * dist_in;
                TraceHit roof = trace_ray(
                    Vec3(probe_xy.x, probe_xy.y, m_climb_top.z + 32.0f),
                    Vec3(probe_xy.x, probe_xy.y, m_climb_top.z - 160.0f),
                    scene
                );
                if (!roof.hit || roof.normal.z < kWalkableFloorZ) {
                    roof = trace_ray(
                        Vec3(probe_xy.x, probe_xy.y, m_climb_top.z + 150.0f),
                        Vec3(probe_xy.x, probe_xy.y, m_climb_top.z - 160.0f),
                        scene
                    );
                }
                if (roof.hit && roof.normal.z >= kWalkableFloorZ) {
                    const Vec3 stand_pos(probe_xy.x, probe_xy.y, roof.point.z + 2.0f);
                    const TraceHit path_block = trace_ray(
                        Vec3(m_climb_base.x, m_climb_base.y, stand_pos.z + 120.0f) + into * 16.0f,
                        Vec3(stand_pos.x, stand_pos.y, stand_pos.z + 120.0f),
                        scene
                    );
                    if (path_block.hit) continue;
                    if (has_room_at(stand_pos, kPawnHeight, scene) ||
                        has_room_at(stand_pos + Vec3(0.0f, 0.0f, 12.0f), kEyeHeightCrouch, scene)) {
                        m_telemetry.position = stand_pos;
                        m_telemetry.velocity = into * 300.0f;
                        m_telemetry.move_state = EMovement::MOVE_Walking;
                        m_telemetry.grounded = true;
                        m_climb_cooldown = 0.45f;
                        m_fall_peak_z = stand_pos.z;
                        return;
                    }
                }
            }
            if (m_telemetry.position.z >= m_climb_top.z - 25.0f) {
                const Vec3 exit_pos = Vec3(m_climb_base.x, m_climb_base.y, m_climb_top.z - 64.0f) + into * 64.0f;
                const TraceHit path_block = trace_ray(
                    Vec3(m_climb_base.x, m_climb_base.y, exit_pos.z + 120.0f) + into * 16.0f,
                    Vec3(exit_pos.x, exit_pos.y, exit_pos.z + 120.0f),
                    scene
                );
                if (!path_block.hit &&
                    (has_room_at(exit_pos, kPawnHeight, scene) ||
                     has_room_at(exit_pos + Vec3(0.0f, 0.0f, 12.0f), kEyeHeightCrouch, scene))) {
                    m_telemetry.position = exit_pos;
                    m_telemetry.velocity = into * 300.0f;
                    m_telemetry.move_state = EMovement::MOVE_Walking;
                    m_telemetry.grounded = true;
                    m_climb_cooldown = 0.45f;
                    m_fall_peak_z = exit_pos.z;
                    return;
                }
                m_telemetry.position.z = m_climb_top.z - 25.0f;
                m_telemetry.velocity = Vec3(0.0f, 0.0f, 0.0f);
            }
        }
    } else if (input.forward < -0.2f) {
        constexpr float kClimbDownSpeed = 240.0f;
        m_telemetry.velocity = Vec3(0.0f, 0.0f, -kClimbDownSpeed);
        m_telemetry.position.z -= kClimbDownSpeed * dt;

        FloorHit floor;
        const bool hit_bottom_floor = check_ground(scene, 18.0f, kPawnHeight, floor);
        if (m_telemetry.position.z <= m_climb_base.z || hit_bottom_floor) {
            m_climb_cooldown = 0.45f;
            m_telemetry.velocity = Vec3(0.0f, 0.0f, 0.0f);
            if (hit_bottom_floor) {
                m_telemetry.position.z = floor.z;
                m_telemetry.move_state = EMovement::MOVE_Walking;
                m_telemetry.grounded = true;
            } else {
                leave_ground(EMovement::MOVE_Falling);
            }
        }
    } else {
        m_telemetry.velocity = Vec3(0.0f, 0.0f, 0.0f);
    }
}

bool ParkourController::try_initiate_balance(const LevelScene& scene) {
    if (m_balance_cooldown > 0.0f || m_telemetry.weapon.is_heavy) return false;

    for (const auto& act : scene.actors) {
        if (!act.is_balance_beam || act.end_point.length_sq() < 1.0f) continue;
        const Vec3 a = act.location;
        const Vec3 b = act.end_point;
        const Vec3 ab = b - a;
        const float len = ab.length_xy();
        if (len < 40.0f) continue;

        const Vec3 u = horiz(ab) / len;
        const float s = horiz(m_telemetry.position - a).dot(u);
        if (s <= 18.0f || s >= len - 18.0f) continue;

        const Vec3 cp = a + ab * (s / len);
        if (horiz(m_telemetry.position - cp).length() > 42.0f) continue;
        if (std::abs(m_telemetry.position.z - cp.z) > 48.0f) continue;

        // TdMove_Balance.StartMove: snap XY onto beam centerline & project horizontal velocity along beam
        m_telemetry.position.x = cp.x;
        m_telemetry.position.y = cp.y;
        const float max_bal_spd = m_config.run_speed * 0.48f;
        const float along_spd = std::clamp(horiz(m_telemetry.velocity).dot(u), -max_bal_spd, max_bal_spd);
        m_telemetry.velocity = u * along_spd;
        m_sprint_energy = 0.0f;

        const Vec3 fwd = facing_forward();
        const Vec3 beam_fwd = (fwd.dot(u) >= 0.0f) ? u : -u;
        const Vec3 beam_right(beam_fwd.y, -beam_fwd.x, 0.0f);
        m_balance_lean = std::clamp(0.2f * fwd.dot(beam_right), -0.6f, 0.6f);
        m_balance_start = a;
        m_balance_end = b;
        m_telemetry.move_state = EMovement::MOVE_Balance;
        m_telemetry.grounded = true;
        set_stance(kEyeHeightStand);
        return true;
    }
    return false;
}

void ParkourController::update_balance(const InputFrame& input, float dt, const LevelScene& scene) {
    if (jump_pressed()) {
        consume_jump();
        m_balance_cooldown = 0.40f;
        m_telemetry.camera_roll_deg = 0.0f;
        start_jump(scene);
        return;
    }

    const Vec3 ab = m_balance_end - m_balance_start;
    const float len = ab.length_xy();
    if (len < 40.0f) {
        m_telemetry.camera_roll_deg = 0.0f;
        m_telemetry.move_state = EMovement::MOVE_Walking;
        return;
    }

    const Vec3 u = horiz(ab) / len;
    const Vec3 fwd = facing_forward();
    const Vec3 beam_fwd = (fwd.dot(u) >= 0.0f) ? u : -u;

    // PlayerBalanceWalk: along the beam at TdMove_Balance.SpeedModifier (0.34) of the top speed:
    // retail walks a beam at 245 uu/s.
    const float target_speed = input.forward * (720.0f * 0.34f);
    float cur_along = horiz(m_telemetry.velocity).dot(beam_fwd);
    const float accel_step = 900.0f * dt;
    if (std::abs(target_speed - cur_along) <= accel_step) {
        cur_along = target_speed;
    } else {
        cur_along += sign_of(target_speed - cur_along) * accel_step;
    }
    m_telemetry.velocity = beam_fwd * cur_along;

    // The balance itself is native. From a retail recording made for it (the lean is what the
    // tree's BalanceDir node shows, the lose-balance state its Danger children):
    //  - a key held leans her that way at about 2.4 a second (2.0 to 2.7), from either side;
    //  - stepping on with the view off the beam's line, by 0.7 degrees or by 8, she is over in 3.2
    //    to 3.6 s, mostly to the side away from the turn; dead in line she reaches the far end of
    //    a 1,740 uu beam every time. Here: a steady push while the view is off the line, growing
    //    on itself (GravityInfluence 0.3), sized to that time;
    //  - past about 0.65 with no key against it she is losing her balance: the key against it ends
    //    that at once, and TimeToCounter (0.8 s) of it has her off the beam on that side. Holding a
    //    key from the middle of the beam, that is 1.09 s from the key to the fall.
    // Retail's lean also wanders by itself as she walks, 0.2 to 0.7 either way over a beam's
    // length and back, without that ever being the lose-balance state (it showed 0.72 and she
    // walked on). What makes it is native; here a slow sway of that size is laid over what the
    // tree is shown, and plays no part in whether she falls.
    const float beam_yaw = yaw_of(beam_fwd);
    const float yaw_diff = wrap_deg(m_telemetry.yaw_deg - beam_yaw);
    const float key = std::abs(input.strafe) > 0.3f ? sign_of(input.strafe) : 0.0f;
    const float skew = std::abs(yaw_diff) > 0.5f ? -sign_of(yaw_diff) * 0.175f : 0.0f;
    const float drive = key * 2.4f + 0.3f * m_balance_lean + skew;
    m_balance_lean = std::clamp(m_balance_lean + drive * dt, -1.0f, 1.0f);
    const float side = sign_of(m_balance_lean);
    const bool losing = std::abs(m_balance_lean) >= 0.65f && key * side >= 0.0f;
    m_balance_danger_time = losing ? m_balance_danger_time + dt : 0.0f;
    m_telemetry.balance_danger = losing ? static_cast<int>(side) : 0;
    m_telemetry.camera_roll_deg = m_balance_lean * 9.0f;
    m_balance_time += dt;
    {
        const float pace = std::clamp(std::abs(cur_along) / 245.0f, 0.0f, 1.0f);
        const float phase = 0.37f * (m_balance_start.x + m_balance_start.y);  // not the same on every beam
        const float sway = 0.30f * std::sin(m_balance_time * 1.03f + phase) + 0.15f * std::sin(m_balance_time * 2.33f + 2.0f * phase);
        m_balance_sway += (sway * pace * std::min(1.0f, m_balance_time) - m_balance_sway) * std::min(1.0f, dt / 0.5f);
    }
    if (m_balance_fall_time < 0.0f && m_balance_danger_time >= 0.8f) {
        // TdMove_Balance.Falloff: the fall off animation of that side starts while she is still
        // on the beam, stopped; she is falling 0.27 to 0.30 s later.
        set_move_anim(side < 0.0f ? "walkbalancefalloffleft" : "walkbalancefalloffright");
        m_balance_fall_time = 0.0f;
        m_balance_fall_side = side;
    }
    if (m_balance_fall_time >= 0.0f) {
        m_balance_fall_time += dt;
        m_telemetry.velocity = Vec3(0.0f, 0.0f, 0.0f);
        m_telemetry.balance_danger = static_cast<int>(m_balance_fall_side);
        if (m_balance_fall_time >= 0.28f) {
            const Vec3 beam_right(-beam_fwd.y, beam_fwd.x, 0.0f);  // her right, walking the beam
            m_telemetry.velocity = beam_right * (m_balance_fall_side * 200.0f);
            m_telemetry.position = m_telemetry.position + beam_right * (m_balance_fall_side * (kPawnRadius + 6.0f));
            m_telemetry.camera_roll_deg = 0.0f;
            m_telemetry.balance_danger = 0;
            m_balance_danger_time = 0.0f;
            m_balance_fall_time = -1.0f;
            m_balance_lean = 0.0f;
            m_balance_cooldown = 0.6f;
            leave_ground(EMovement::MOVE_Falling);
        }
        return;
    }

    const Vec3 next_pos = m_telemetry.position + m_telemetry.velocity * dt;
    const float s = horiz(next_pos - m_balance_start).dot(u);
    if (s <= 10.0f || s >= len - 10.0f) {
        m_telemetry.position.x = next_pos.x;
        m_telemetry.position.y = next_pos.y;
        m_telemetry.camera_roll_deg = 0.0f;
        m_balance_cooldown = 0.35f;
        m_telemetry.move_state = EMovement::MOVE_Walking;
        return;
    }

    const Vec3 on = m_balance_start + ab * (s / len);
    m_telemetry.position.x = on.x;
    m_telemetry.position.y = on.y;
    m_telemetry.position.z = on.z;
    m_fall_peak_z = m_telemetry.position.z;
}

bool ParkourController::try_initiate_ledge_walk(const LevelScene& scene) {
    if (m_ledge_walk_cooldown > 0.0f || m_telemetry.weapon.is_heavy) return false;
    if (m_telemetry.move_state == EMovement::MOVE_LedgeWalk) return false;

    for (const auto& act : scene.actors) {
        if (!act.is_ledge || act.end_point.length_sq() < 1.0f) continue;
        const Vec3 a = act.location;
        const Vec3 b = act.end_point;
        const Vec3 ab = b - a;
        const float len = ab.length_xy();
        if (len < 40.0f) continue;

        const Vec3 u = horiz(ab) / len;
        const float s = horiz(m_telemetry.position - a).dot(u);
        if (s <= 8.0f || s >= len - 8.0f) continue;

        const Vec3 cp = a + ab * (s / len);
        if (horiz(m_telemetry.position - cp).length() > 52.0f) continue;
        if (std::abs(m_telemetry.position.z - cp.z) > 56.0f) continue;

        Vec3 wall_out = horiz(act.wall_normal);
        if (wall_out.length_sq() < 1e-4f) {
            wall_out = Vec3(-u.y, u.x, 0.0f);
            const TraceHit wh = trace_ray(cp + Vec3(0.0f, 0.0f, 80.0f),
                                          cp + Vec3(0.0f, 0.0f, 80.0f) - wall_out * 64.0f, scene);
            if (!wh.hit) wall_out = -wall_out;
        }
        wall_out = wall_out.normalized();

        m_ledge_walk_start = a;
        m_ledge_walk_end = b;
        m_ledge_walk_normal = wall_out;
        m_telemetry.wall_normal = wall_out;
        m_telemetry.position.x = cp.x;
        m_telemetry.position.y = cp.y;
        m_telemetry.position.z = cp.z;
        m_telemetry.velocity = Vec3(0.0f, 0.0f, 0.0f);
        m_sprint_energy = 0.0f;
        m_telemetry.move_state = EMovement::MOVE_LedgeWalk;
        m_telemetry.grounded = true;
        m_state_timer = 0.0f;
        if (facing_forward().dot(wall_out) < 0.0f) {
            m_telemetry.yaw_deg = yaw_of(wall_out);
        }
        set_stance(kEyeHeightStand);
        return true;
    }
    return false;
}

void ParkourController::update_ledge_walk(const InputFrame& input, float dt, const LevelScene& scene) {
    const Vec3 ab = m_ledge_walk_end - m_ledge_walk_start;
    const float len = ab.length_xy();
    if (len < 40.0f) {
        m_telemetry.move_state = EMovement::MOVE_Walking;
        return;
    }

    const Vec3 u = horiz(ab) / len;
    const Vec3 wall_out = (m_ledge_walk_normal.length_sq() > 1e-4f) ? m_ledge_walk_normal : Vec3(1.0f, 0.0f, 0.0f);

    // Constrain camera yaw within ±75° of outward wall normal (TdMove_LedgeWalk.bConstrainLook = True)
    const float base_yaw = yaw_of(wall_out);
    const float yaw_diff = wrap_deg(m_telemetry.yaw_deg - base_yaw);
    if (std::abs(yaw_diff) > 75.0f) {
        m_telemetry.yaw_deg = base_yaw + sign_of(yaw_diff) * 75.0f;
        while (m_telemetry.yaw_deg < 0.0f) m_telemetry.yaw_deg += 360.0f;
        while (m_telemetry.yaw_deg >= 360.0f) m_telemetry.yaw_deg -= 360.0f;
    }

    // Jump off narrow ledge
    if (jump_pressed()) {
        consume_jump();
        m_ledge_walk_cooldown = 0.45f;
        Vec3 out_dir = facing_forward();
        if (out_dir.dot(wall_out) < 0.35f) {
            out_dir = (horiz(out_dir) + wall_out * 0.6f).normalized();
        }
        m_telemetry.velocity = out_dir * 320.0f;
        start_jump(scene);
        return;
    }

    // Step/drop off narrow ledge with crouch
    if (m_crouch_pressed) {
        m_crouch_pressed = false;
        m_ledge_walk_cooldown = 0.45f;
        m_telemetry.velocity = wall_out * 90.0f;
        leave_ground(EMovement::MOVE_Falling);
        return;
    }

    // Sidestep shimmy along the narrow ledge (driven by A/D strafe or W/S along the spline)
    constexpr float kLedgeWalkMaxSpeed = 135.0f;
    const float wish_along = std::clamp(
        input.strafe * facing_right().dot(u) + input.forward * facing_forward().dot(u),
        -1.0f, 1.0f);
    const float target_speed = wish_along * kLedgeWalkMaxSpeed;
    float cur_along = horiz(m_telemetry.velocity).dot(u);
    const float accel_step = 900.0f * dt;
    if (std::abs(target_speed - cur_along) <= accel_step) {
        cur_along = target_speed;
    } else {
        cur_along += sign_of(target_speed - cur_along) * accel_step;
    }
    m_telemetry.velocity = u * cur_along;

    const Vec3 next_pos = m_telemetry.position + m_telemetry.velocity * dt;
    const float s = horiz(next_pos - m_ledge_walk_start).dot(u);
    if (s <= 6.0f || s >= len - 6.0f) {
        m_telemetry.position.x = next_pos.x;
        m_telemetry.position.y = next_pos.y;
        m_ledge_walk_cooldown = 0.35f;
        m_telemetry.move_state = EMovement::MOVE_Walking;
        m_telemetry.grounded = true;
        return;
    }

    const Vec3 on = m_ledge_walk_start + ab * (s / len);
    m_telemetry.position.x = on.x;
    m_telemetry.position.y = on.y;
    m_telemetry.position.z = on.z;
    m_fall_peak_z = m_telemetry.position.z;
}

// -----------------------------------------------------------------------------
// Landing moves: TdMove_SkillRoll, TdMove_Landing (LandHard), TdMove_LayOnGround
// -----------------------------------------------------------------------------
void ParkourController::update_landing_moves(const InputFrame& input, float dt, const LevelScene& scene) {
    const MovementConfig& c = m_config;
    EMovement& st = m_telemetry.move_state;
    m_landing_timer -= dt;

    if (st == EMovement::MOVE_SkillRoll) {
        // Root motion: the pawn moves as fallinglandroll's root bone does (SetIgnoreMoveInput(-1), so
        // no steering), at crouch height. Float rounding can leave a sliver of the animation for one
        // more step; it is finished now instead.
        const bool ends = m_landing_timer <= 1e-4f;
        const float t1 = ends ? kSkillRollLength : kSkillRollLength - m_landing_timer;
        const float t0 = std::clamp(kSkillRollLength - (m_landing_timer + dt), 0.0f, t1);
        const float z1 = skill_roll_root_forward(t1);
        // The velocity is the root motion's rate over the last step's worth of animation, so the
        // final (partial) step leaves at the animation's exit speed; walk_move takes off the part a
        // wall blocks.
        const float rate = (z1 - skill_roll_root_forward(std::max(0.0f, t1 - dt))) / dt;
        m_telemetry.velocity.x = m_roll_dir.x * rate;
        m_telemetry.velocity.y = m_roll_dir.y * rate;
        walk_move(m_roll_dir * (z1 - skill_roll_root_forward(t0)), kCrouchHeight, scene);
        if (ends) {
            // OnCustomAnimEnd: SetMove(MOVE_Walking) - crouching if crouch is held or there is no
            // room to stand.
            const bool stand = !input.crouch && has_room(kPawnHeight, scene);
            st = stand ? EMovement::MOVE_Walking : EMovement::MOVE_Crouch;
            set_stance(stand ? kEyeHeightStand : kEyeHeightCrouch);
            m_sprint_energy = std::max(0.0f, m_telemetry.velocity.length_xy() - c.speed_max_base_velocity);
            m_accel_time = 1.0f;
        }
        return;
    }

    // Lethal fall impact (FallingLandDie): keep collapsed posture and roll until blackout respawn
    if (m_telemetry.fall_death_impact) {
        m_telemetry.velocity = Vec3(0.0f, 0.0f, 0.0f);
        set_stance(28.0f);
        m_telemetry.camera_roll_deg = 38.0f + 6.0f * m_telemetry.death_anim_progress;
        return;
    }

    // LandHard / LayOnGround: dead stop, input ignored until the animation ends (a jump press gets
    // you up off the ground early once the fall has played out).
    m_telemetry.velocity.x = 0.0f;
    m_telemetry.velocity.y = 0.0f;
    const float total = (st == EMovement::MOVE_LayOnGround) ? c.lay_on_ground_time : c.hard_landing_time;
    const float progress = 1.0f - std::clamp(m_landing_timer / std::max(total, 1e-3f), 0.0f, 1.0f);
    const float low = (st == EMovement::MOVE_LayOnGround) ? kEyeHeightSlide : kEyeHeightCrouch;
    set_stance(low + (kEyeHeightStand - low) * std::clamp((progress - 0.6f) / 0.4f, 0.0f, 1.0f));
    const bool get_up = (st == EMovement::MOVE_LayOnGround && jump_pressed() && progress > 0.5f);
    if (m_landing_timer <= 0.0f || get_up) {
        if (get_up) consume_jump();
        st = EMovement::MOVE_Walking;
        set_stance(kEyeHeightStand);
        m_sprint_energy = 0.0f;
        m_accel_time = 0.0f;
    }
}

// -----------------------------------------------------------------------------
// Combat, Firearms, Ballistics & Disarm Subsystem
// (Reverse-engineered from TdGame.u TdMove_Melee*, TdMove_Disarm, TdWeapon & DefaultWeapons.ini)
// -----------------------------------------------------------------------------
void ParkourController::update_combat_and_weapons(const InputFrame& input, float dt, LevelScene& scene) {
    WeaponState& ws = m_telemetry.weapon;
    ws.fired_this_tick = false;

    if (ws.cooldown > 0.0f) ws.cooldown = std::max(0.0f, ws.cooldown - dt);
    if (ws.fire_anim_timer > 0.0f) ws.fire_anim_timer = std::max(0.0f, ws.fire_anim_timer - dt);
    if (ws.equip_timer > 0.0f) ws.equip_timer = std::max(0.0f, ws.equip_timer - dt);
    if (ws.muzzle_flash_timer > 0.0f) ws.muzzle_flash_timer = std::max(0.0f, ws.muzzle_flash_timer - dt);
    if (m_melee_cooldown > 0.0f) m_melee_cooldown = std::max(0.0f, m_melee_cooldown - dt);
    if (m_melee_combo_reset_timer > 0.0f) {
        m_melee_combo_reset_timer -= dt;
        if (m_melee_combo_reset_timer <= 0.0f) m_melee_combo_index = 0;
    }
    if (m_telemetry.hit_marker_timer > 0.0f) m_telemetry.hit_marker_timer = std::max(0.0f, m_telemetry.hit_marker_timer - dt);
    if (m_telemetry.damage_flash_timer > 0.0f) m_telemetry.damage_flash_timer = std::max(0.0f, m_telemetry.damage_flash_timer - dt);

    // Finish weapon throwaway drop animation
    if (ws.drop_timer > 0.0f) {
        ws.drop_timer -= dt;
        if (ws.drop_timer <= 0.0f) {
            ws.drop_timer = 0.0f;
            ws.equipped = false;
            ws.name = "None";
            ws.display_name = "Unarmed";
            ws.is_heavy = false;
            ws.is_two_handed = false;
        }
    }

    // What the bullets leave where they land (game/impact_effects.hpp)
    update_impact_effects(scene, dt, m_telemetry.position);

    // Update 3D bullet tracers in the level scene
    for (auto it = scene.active_tracers.begin(); it != scene.active_tracers.end();) {
        it->timer -= dt;
        if (it->timer <= 0.0f) {
            it = scene.active_tracers.erase(it);
        } else {
            ++it;
        }
    }

    // Update 3D dropped weapons physics on the ground
    for (auto& dw : scene.dropped_weapons) {
        if (!dw.grounded) {
            dw.velocity.z -= m_config.gravity * dt;
            dw.position += dw.velocity * dt;
            dw.yaw_deg += 180.0f * dt;
            TraceHit ghit = trace_ray(dw.position + Vec3(0, 0, 25.0f), dw.position - Vec3(0, 0, 35.0f), scene);
            if (ghit.hit && dw.position.z <= ghit.point.z + 4.0f) {
                dw.position.z = ghit.point.z + 3.0f;
                dw.velocity = Vec3(0, 0, 0);
                dw.grounded = true;
            } else if (dw.position.z < m_telemetry.position.z - 600.0f) {
                dw.grounded = true;
            }
        }
    }

    // 0A. Cycle through all 11 retail Mirror's Edge weapons (`T` / `Y` / MouseWheel)
    static const char* kAllWeapons[11] = {
        "Colt1911", "Glock18", "BerettaM93R", "SteyrTMP", "MP5K",
        "G36C", "FNSCARL", "Remington870", "Neostead", "FNMinimi", "M95"
    };
    if (input.cycle_weapon_dir != 0) {
        int next_idx = m_weapon_cycle_index + input.cycle_weapon_dir;
        if (next_idx < 0) next_idx = 10;
        if (next_idx >= 11) next_idx = 0;
        equip_weapon(kAllWeapons[next_idx]);
        m_telemetry.active_subtitle = "Equipped: " + m_telemetry.weapon.display_name +
                                      " (" + std::to_string(m_telemetry.weapon.ammo) + " RDS)";
    }

    // 0B. Spawn KrugerSec Combat Squad ahead of Faith (`H` key) for live combat testing
    if (input.spawn_combat_squad) {
        Vec3 fwd = Rotator::from_degrees(0.0f, m_telemetry.yaw_deg, 0.0f).forward();
        Vec3 right = Rotator::from_degrees(0.0f, m_telemetry.yaw_deg, 0.0f).right();
        static const char* kSquadWeapons[4] = {"G36C", "Remington870", "MP5K", "Colt1911"};
        static const char* kSquadArchetypes[4] = {"Assault_SWAT", "Support_Shotgun", "PatrolCop_SMG", "PatrolCop"};
        for (int i = 0; i < 3; ++i) {
            EnemyBot guard{};
            guard.archetype = kSquadArchetypes[i % 4];
            guard.weapon_name = kSquadWeapons[(i + static_cast<int>(m_telemetry.tick)) % 4];
            float lateral = (i - 1) * 140.0f;
            Vec3 spawn_pt = m_telemetry.position + fwd * (420.0f + i * 90.0f) + right * lateral;
            TraceHit f_hit = trace_ray(spawn_pt + Vec3(0, 0, 120.0f), spawn_pt - Vec3(0, 0, 220.0f), scene);
            if (f_hit.hit) spawn_pt.z = f_hit.point.z;
            guard.position = spawn_pt;
            guard.home_position = spawn_pt;
            guard.yaw_deg = m_telemetry.yaw_deg + 180.0f;
            guard.health = 100.0f;
            guard.max_health = 100.0f;
            guard.alive = true;
            guard.disarm_window = (i == 0);
            guard.anim_state = EEnemyAnimState::AimFire;
            scene.enemies.push_back(guard);
        }
        m_telemetry.active_subtitle = "KrugerSec Tactical Squad Deployed Ahead!";
    }

    // 0C. Manual Weapon Drop / Throwaway (`G` / `Backspace` or Right-Click when no disarm target is in range)
    auto drop_current_weapon = [&](const std::string& reason) {
        if (!ws.equipped || ws.drop_timer > 0.0f) return;
        DroppedWeapon dw{};
        dw.weapon_name = ws.name;
        dw.ammo = ws.ammo;
        Vec3 fwd = Rotator::from_degrees(0.0f, m_telemetry.yaw_deg, 0.0f).forward();
        Vec3 right = Rotator::from_degrees(0.0f, m_telemetry.yaw_deg, 0.0f).right();
        dw.position = m_telemetry.position + Vec3(0, 0, 55.0f) + fwd * 32.0f + right * 14.0f;
        dw.velocity = fwd * 220.0f + right * 45.0f + Vec3(0, 0, 90.0f);
        dw.yaw_deg = m_telemetry.yaw_deg + 35.0f;
        dw.grounded = false;
        scene.dropped_weapons.push_back(dw);

        ws.drop_timer = 0.35f; // plays 1P `throwaway` animation before clearing `ws.equipped`
        ws.is_heavy = false;
        ws.mobility_scale = 1.0f;
        m_telemetry.active_subtitle = reason;
    };

    if (input.drop_weapon && ws.equipped) {
        drop_current_weapon("Dropped " + ws.display_name);
    }

    // 0D. Pick up a DroppedWeapon from the ground when pressing Use (`E`) or Disarm while unarmed
    if (!ws.equipped && (input.use || input.disarm)) {
        for (auto it = scene.dropped_weapons.begin(); it != scene.dropped_weapons.end(); ++it) {
            if (it->ammo > 0 && m_telemetry.position.distance(it->position) < 135.0f) {
                std::string picked_name = it->weapon_name;
                int picked_ammo = it->ammo;
                scene.dropped_weapons.erase(it);
                equip_weapon(picked_name);
                ws.ammo = picked_ammo;
                m_telemetry.active_subtitle = "Picked up " + ws.display_name + " (" + std::to_string(ws.ammo) + " RDS)";
                break;
            }
        }
    }

    // Check if any nearby enemy is currently in a Disarmable state (or can be stealth-snatched from behind)
    m_telemetry.disarm_prompt_visible = false;
    EnemyBot* disarm_candidate = nullptr;
    bool candidate_from_back = false;
    bool disarm_out_of_time = false;  // an armed enemy in reach, but not open to it
    for (auto& bot : scene.enemies) {
        if (!bot.alive || bot.is_story_npc || bot.weapon_name == "None" || bot.weapon_name.empty()) continue;
        float dist = m_telemetry.position.distance(bot.position);
        if (dist < 210.0f) {
            disarm_out_of_time = true;
            Vec3 bot_fwd(std::cos(bot.yaw_deg * DEG2RAD), std::sin(bot.yaw_deg * DEG2RAD), 0.0f);
            Vec3 bot_to_player = (m_telemetry.position - bot.position).normalized_xy();
            bool behind_enemy = (bot_fwd.dot(bot_to_player) < -0.25f);
            if (bot.disarm_window || bot.stunned || behind_enemy) {
                m_telemetry.disarm_prompt_visible = true;
                disarm_candidate = &bot;
                candidate_from_back = behind_enemy;
                break;
            }
        }
    }

    // 1. Weapon Disarm QTE (`input.disarm` -> `TdMove_Disarm` / `MOVE_Snatch`: `SnatchFwd` or `SnatchBack`)
    if (input.disarm) {
        if (disarm_candidate != nullptr) {
            EnemyBot& bot = *disarm_candidate;
            m_telemetry.move_state = EMovement::MOVE_Snatch;
            m_snatch_fail = false;
            m_state_timer = 0.0f;
            m_telemetry.combat_anim_time = 0.0f;
            m_telemetry.snatch_from_back = candidate_from_back;
            m_telemetry.hit_marker_timer = 0.35f;

            // TdMove_Disarm.StartMove: she faces the enemy with a level view (TargetRotation,
            // ResetCameraLook(0.2)) and is flown to DisarmOffset (ChooseDisarmType: 125.899) short
            // of them at 400 uu/s, or her own speed if that is more, when they stand level.
            Vec3 to_bot = (bot.position - m_telemetry.position).normalized_xy();
            m_snatch_align = false;
            if (to_bot.length_sq() > 1e-4f) {
                m_telemetry.yaw_deg = std::atan2(to_bot.y, to_bot.x) * RAD2DEG;
                m_pawn_yaw = m_telemetry.yaw_deg;
                m_snatch_target = bot.position - to_bot * 125.899f;
                m_snatch_speed = std::max(400.0f, m_telemetry.velocity.length_xy());
                // (Standing more than 3 uu apart in height retail flies her up to him or slides him
                // to her instead, HandleHeightDifference; here she is brought in along the floor
                // either way, unless a step's height or more is between them.)
                m_snatch_align = std::abs(bot.position.z - m_telemetry.position.z) <= 35.0f;
            }

            std::string snatched_wep = bot.weapon_name;
            bot.disarm_window = false;
            bot.stunned = true;
            bot.attack_timer = 0.0f;
            bot.anim_timer = 0.0f;
            bot.active_anim_seq = candidate_from_back ? "SnatchBack" : "SnatchFwd";

            bool is_trainer = (bot.archetype.find("TutorialTrainer") != std::string::npos ||
                               bot.archetype.find("Celeste") != std::string::npos);
            if (is_trainer) {
                // Celeste sparring partner gets disarmed and recovers after the drill
                bot.anim_state = EEnemyAnimState::BeingDisarmed;
                bot.health = std::max(25.0f, bot.health - 25.0f);
            } else {
                // Standard KrugerSec guard is knocked out cold by Faith's disarm takedown!
                bot.health = 0.0f;
                bot.alive = false;
                bot.weapon_name = "None";
                bot.anim_state = EEnemyAnimState::KnockedOut;
            }

            equip_weapon(snatched_wep);
            // TdMove_Disarm.ChooseDisarmType: from behind SnatchBack; from the front SnatchFwd, and
            // for a patrol cop's light weapon one of three (of two for the TMP; Rand in the game,
            // in turn here). The move lasts as long as the animation (OnCustomAnimEnd).
            const char* snatch = candidate_from_back ? "SnatchBack" : "SnatchFwd";
            if (!candidate_from_back && !ws.is_two_handed && bot.archetype.find("PatrolCop") != std::string::npos) {
                static const char* const kFront[] = {"SnatchFwd", "SnatchFwd2", "SnatchFwd3"};
                const bool tmp = snatched_wep.find("TMP") != std::string::npos || snatched_wep.find("Steyr") != std::string::npos;
                snatch = tmp ? kFront[1 + m_disarm_count % 2] : kFront[m_disarm_count % 3];
            }
            ++m_disarm_count;
            m_telemetry.combat_anim_duration = snatch_length(snatched_wep, snatch);
            set_move_anim(snatch);
            // PlayDisarmStart / StopMove: AttachWeaponToHand comes as the move ends, but 0.8 s in for
            // the Remington and 1.4 s for the Neostead. Until then it is still in his hands.
            m_snatch_attach = snatched_wep.find("Remington") != std::string::npos ? 0.8f
                              : snatched_wep.find("Neostead") != std::string::npos ? 1.4f
                              : m_telemetry.combat_anim_duration;
            m_telemetry.snatch_weapon_attached = false;
            bot.disarm_weapon = snatched_wep;
            bot.disarm_weapon_time = m_snatch_attach;
            m_telemetry.active_subtitle = std::string(candidate_from_back ? "Stealth Disarm (" : "Weapon Disarmed (") +
                                          ws.display_name + ")!";
        } else if (disarm_out_of_time && !ws.equipped && m_telemetry.grounded && m_telemetry.move_state == EMovement::MOVE_Walking) {
            // TdMove_Disarm.StartMiss: SnatchFail on the full body with its own root motion
            // (0.1 in, 0.4 out). Retail: 0.77 s in the move, 590 uu/s down to 200 in its first frame.
            m_telemetry.move_state = EMovement::MOVE_Snatch;
            m_snatch_fail = true;
            m_snatch_align = false;
            m_state_timer = 0.0f;
            m_telemetry.combat_anim_time = 0.0f;
            m_telemetry.combat_anim_duration = 0.77f;
            m_telemetry.snatch_from_back = false;
            m_telemetry.snatch_weapon_attached = false;
            m_snatch_attach = m_telemetry.combat_anim_duration;
            m_telemetry.velocity.x *= 0.34f;
            m_telemetry.velocity.y *= 0.34f;
            set_move_anim("SnatchFail");
        } else if (ws.equipped && ws.drop_timer <= 0.0f && !input.use) {
            // In Mirror's Edge, pressing the Disarm/Secondary button while holding a gun with no enemy in range tosses the gun
            drop_current_weapon("Tossed " + ws.display_name);
        }
    }

    // 2. Firearm Shooting & Ballistics (`input.fire`)
    if (!input.fire) {
        ws.trigger_released = true;
    }

    // TdWeapon.ShouldRefire / the fire states: no shot with both hands on a wall, nor with a light
    // weapon while the one hand is (AgainstWallState 1, or 2 with WeaponType 2).
    const bool wall_stops_fire = m_against_wall == 1 || (m_against_wall == 2 && !ws.is_heavy);
    auto fire_single_shot = [&]() {
        if (ws.ammo <= 0 || wall_stops_fire) return;
        ws.ammo--;
        ws.fired_this_tick = true;
        if (ws.fire_mode == EWeaponFireMode::BoltAction) {
            ws.fire_anim_duration = 1.45f;
        } else if (ws.fire_mode == EWeaponFireMode::PumpAction) {
            ws.fire_anim_duration = (ws.name == "Remington870") ? 1.05f : 0.88f;
        } else if (ws.fire_mode == EWeaponFireMode::SemiAuto) {
            ws.fire_anim_duration = 0.52f;
        } else {
            ws.fire_anim_duration = 0.48f;
        }
        ws.fire_anim_timer = ws.fire_anim_duration;
        ws.muzzle_flash_timer = 0.065f;

        Rotator view_rot = Rotator::from_degrees(m_telemetry.pitch_deg, m_telemetry.yaw_deg, 0.0f);
        Vec3 fwd = view_rot.forward();
        Vec3 right = view_rot.right();
        Vec3 up = view_rot.up();
        Vec3 eye = m_telemetry.position + Vec3(0, 0, m_telemetry.eye_height);
        Vec3 muzzle_world = eye + fwd * 36.0f + right * 11.0f - up * 9.0f;

        int pellets = std::max(1, ws.pellet_count);
        bool any_hit = false;
        bool any_kill = false;

        for (int p = 0; p < pellets; ++p) {
            // Deterministic golden-ratio spiral cone spread for multi-pellet shotguns and automatic fire
            float spread = ws.spread_rad * (m_telemetry.reaction_active ? 0.45f : 1.0f);
            float angle = static_cast<float>(p) * 2.3999632f + static_cast<float>(m_telemetry.tick) * 0.71f;
            float radius = (pellets > 1)
                ? spread * std::sqrt((static_cast<float>(p) + 0.5f) / static_cast<float>(pellets))
                : spread * 0.35f * std::sin(static_cast<float>(m_telemetry.tick) * 1.7f);
            Vec3 ray_dir = (fwd + right * ( std::cos(angle) * radius ) + up * ( std::sin(angle) * radius )).normalized();
            Vec3 ray_end = eye + ray_dir * ws.range;

            TraceHit wall_hit = trace_ray(eye, ray_end, scene, COLL_BlockZeroExtent);
            float max_dist = wall_hit.hit ? eye.distance(wall_hit.point) : ws.range;
            Vec3 tracer_end = wall_hit.hit ? wall_hit.point : (eye + ray_dir * std::min(ws.range, 2500.0f));

            // Ray-Capsule intersection against living enemies (Headshot & Torso hitboxes)
            EnemyBot* hit_bot = nullptr;
            float best_bot_dist = max_dist;
            bool is_headshot = false;

            for (auto& bot : scene.enemies) {
                if (!bot.alive) continue;
                Vec3 bot_center = bot.position + Vec3(0, 0, 52.0f);
                Vec3 to_bot = bot_center - eye;
                float proj = to_bot.dot(ray_dir);
                if (proj > 0.0f && proj < best_bot_dist) {
                    Vec3 closest = eye + ray_dir * proj;
                    float dist_xy = closest.distance_xy(bot.position);
                    float rel_z = closest.z - bot.position.z;
                    if (dist_xy < 48.0f && rel_z >= -10.0f && rel_z <= 105.0f) {
                        best_bot_dist = proj;
                        hit_bot = &bot;
                        is_headshot = (rel_z >= 72.0f);
                        tracer_end = closest;
                    }
                }
            }

            if (hit_bot != nullptr) {
                any_hit = true;
                // Distance damage falloff from DefaultWeapons.ini
                float t_falloff = 0.0f;
                if (best_bot_dist > ws.falloff_start && ws.falloff_end > ws.falloff_start) {
                    t_falloff = std::clamp((best_bot_dist - ws.falloff_start) / (ws.falloff_end - ws.falloff_start), 0.0f, 1.0f);
                }
                float dmg = ws.damage + (ws.damage_far - ws.damage) * t_falloff;
                if (is_headshot) dmg *= 2.0f;

                hit_bot->health -= dmg;
                hit_bot->stunned = true;
                hit_bot->attack_timer = 0.0f;
                hit_bot->anim_timer = 0.0f;
                hit_bot->active_anim_seq = (p % 2 == 0) ? "HitMeleeRight" : "HitMeleeLeft";

                if (hit_bot->health <= 0.0f) {
                    bool is_trainer = (hit_bot->archetype.find("TutorialTrainer") != std::string::npos ||
                                       hit_bot->archetype.find("Celeste") != std::string::npos);
                    if (is_trainer) {
                        hit_bot->health = 100.0f;
                        hit_bot->anim_state = EEnemyAnimState::HitStagger;
                    } else {
                        hit_bot->alive = false;
                        hit_bot->anim_state = EEnemyAnimState::KnockedOut;
                        any_kill = true;
                        // Drop the enemy's weapon onto the ground if they had one
                        if (!hit_bot->weapon_name.empty() && hit_bot->weapon_name != "None") {
                            DroppedWeapon dw{};
                            dw.weapon_name = hit_bot->weapon_name;
                            dw.ammo = 15;
                            dw.position = hit_bot->position + Vec3(0, 0, 45.0f);
                            dw.velocity = ray_dir * 90.0f + Vec3(0, 0, 80.0f);
                            dw.yaw_deg = hit_bot->yaw_deg + 45.0f;
                            dw.grounded = false;
                            scene.dropped_weapons.push_back(dw);
                            hit_bot->weapon_name = "None";
                        }
                    }
                } else {
                    hit_bot->anim_state = EEnemyAnimState::HitStagger;
                }
            }

            // Spawn 3D BulletTracer in world
            BulletTracer tr{};
            tr.start_pos = muzzle_world;
            tr.end_pos = tracer_end;
            tr.timer = 0.09f;
            tr.max_time = 0.09f;
            tr.hit_enemy = (hit_bot != nullptr);
            tr.from_player = true;
            tr.ammo = pellets > 1 ? 3 : (ws.is_heavy ? 1 : 0);
            scene.active_tracers.push_back(tr);
        }

        if (any_hit) {
            m_telemetry.hit_marker_timer = 0.22f;
            if (any_kill) {
                m_telemetry.active_subtitle = "Target Neutralized (" + ws.display_name + ")";
            }
        }

        // Apply authentic camera recoil kick (TdSkelControlRecoil + view pitch kick)
        m_telemetry.pitch_deg = std::clamp(m_telemetry.pitch_deg + ws.recoil_pitch_deg * 0.45f, -85.0f, 85.0f);
        float yaw_jitter = ((m_telemetry.tick % 2 == 0) ? 1.0f : -1.0f) * ws.recoil_pitch_deg * 0.12f;
        m_telemetry.yaw_deg += yaw_jitter;
    };

    // Continue active 3-round burst for Beretta M93R
    if (ws.equipped && ws.drop_timer <= 0.0f && ws.burst_remaining > 0 && ws.cooldown <= 0.0f) {
        if (ws.ammo > 0) {
            ws.burst_remaining--;
            fire_single_shot();
            ws.cooldown = (ws.burst_remaining > 0) ? ws.fire_interval : 0.24f;
        } else {
            ws.burst_remaining = 0;
        }
    } else if (input.fire && ws.equipped && ws.drop_timer <= 0.0f && ws.cooldown <= 0.0f) {
        bool can_pull = (ws.fire_mode == EWeaponFireMode::FullAuto) || ws.trigger_released;
        if (can_pull) {
            ws.trigger_released = false;
            ws.equip_timer = 0.0f;
            if (ws.ammo > 0) {
                if (ws.fire_mode == EWeaponFireMode::Burst3) {
                    ws.burst_remaining = std::min(2, ws.ammo - 1);
                }
                fire_single_shot();
                ws.cooldown = ws.fire_interval;
            } else {
                // Empty magazine: play `standfireempty` click and toss empty weapon (`throwaway`)
                drop_current_weapon(ws.display_name + " Empty - Tossed");
            }
        }
    }

    // 3. Wallrun Kick Hit Detection (while airborne in MOVE_MeleeWallrun)
    if (m_telemetry.move_state == EMovement::MOVE_MeleeWallrun && m_state_timer < 0.35f) {
        for (auto& bot : scene.enemies) {
            if (bot.is_story_npc) continue;
            if (bot.alive && m_telemetry.position.distance(bot.position) < 195.0f) {
                bot.health -= 90.0f;
                bot.stunned = true;
                bot.disarm_window = true;
                bot.attack_timer = 0.0f;
                bot.anim_timer = 0.0f;
                bot.active_anim_seq = "HitMeleeWallrunRight";
                m_telemetry.melee_hit_confirmed = true;
                m_telemetry.hit_marker_timer = 0.28f;
                if (bot.health <= 0.0f) {
                    bot.alive = false;
                    bot.anim_state = EEnemyAnimState::KnockedOut;
                    m_telemetry.active_subtitle = "Wallrun Kick Knockout!";
                } else {
                    bot.anim_state = EEnemyAnimState::HitStagger;
                }
            }
        }
    }

    // 4. Context-Sensitive Unarmed Melee Strikes (`input.melee`: Combo Punch/Kick, Crouch Uppercut, Jump Kick, Barge)
    if (input.melee && m_melee_cooldown <= 0.0f && (!ws.equipped || ws.drop_timer > 0.0f) &&
        !(m_against_wall != 0 && m_telemetry.move_state == EMovement::MOVE_Walking) &&  // TdMove_Melee.CanDoMove
        m_telemetry.move_state != EMovement::MOVE_Slide &&
        m_telemetry.move_state != EMovement::MOVE_MeleeSlide &&
        m_telemetry.move_state != EMovement::MOVE_MeleeWallrun &&
        m_telemetry.move_state != EMovement::MOVE_Barge) {

        Vec3 fwd = Rotator::from_degrees(0.0f, m_telemetry.yaw_deg, 0.0f).forward();
        m_telemetry.melee_hit_confirmed = false;

        // Airborne / Crouched melee near a closed door opens the door (`TdMove_AirBarge` / `TdMove_MeleeCrouch`).
        {
            Vec3 door_hit;
            const int door_ahead = find_barge_door(scene, fwd, 185.0f, door_hit);
            if (door_ahead >= 0) {
                open_barge_door(door_ahead, fwd, true, scene);
            }
        }

        if (!m_telemetry.grounded) {
            // Airborne Flying Jump Kick (`TdMove_MeleeAir`: `JumpKickStart` -> `JumpKickEnd`)
            m_telemetry.move_state = EMovement::MOVE_MeleeAir;
            m_state_timer = 0.0f;
            m_telemetry.combat_anim_time = 0.0f;
            m_telemetry.combat_anim_duration = 0.62f;
            m_melee_cooldown = 0.62f;

            // Lunge forward in mid-air toward target
            m_telemetry.velocity += fwd * 140.0f + Vec3(0, 0, 60.0f);
        } else if (m_telemetry.move_state == EMovement::MOVE_Crouch || input.crouch) {
            // Crouch Uppercut (`MeleeCrouchHitUppercut`)
            m_telemetry.move_state = EMovement::MOVE_Melee;
            m_telemetry.melee_variant = 3;
            m_state_timer = 0.0f;
            m_telemetry.combat_anim_time = 0.0f;
            m_telemetry.combat_anim_duration = 0.48f;
            m_melee_cooldown = 0.48f;
            m_telemetry.velocity += fwd * 160.0f;
        } else {
            // Standing 3-Hit Combo (`MeleeStartRight` -> `MeleeHitLeft` -> `MeleeStartKick`)
            m_telemetry.move_state = EMovement::MOVE_Melee;
            m_telemetry.melee_variant = m_melee_combo_index % 3;
            m_melee_combo_index = (m_melee_combo_index + 1) % 3;
            m_melee_combo_reset_timer = 1.35f;

            float dur = (m_telemetry.melee_variant == 2) ? 0.56f : 0.42f;
            m_state_timer = 0.0f;
            m_telemetry.combat_anim_time = 0.0f;
            m_telemetry.combat_anim_duration = dur;
            m_melee_cooldown = dur * 0.90f;
            // No lunge: retail's MOVE_Melee keeps the walking velocity. At all nine melee starts in
            // the retail recordings the speed carries straight on (697 -> 698 -> 698 uu/s at a
            // sprint, 530 -> 532 -> 534, and 0 -> 0 -> 0 from rest); the +140 / +220 uu/s push the
            // port added here put it 40-50 uu ahead within half a second.
        }

        // Melee Hit Detection & Target Magnetism (from DefaultAIMeleeAttacks.ini)
        for (auto& bot : scene.enemies) {
            if (!bot.alive || bot.is_story_npc) continue;
            float reach = (m_telemetry.move_state == EMovement::MOVE_MeleeAir) ? 225.0f : 195.0f;
            float dist = m_telemetry.position.distance(bot.position);
            if (dist < reach) {
                Vec3 dir_to_bot = (bot.position - m_telemetry.position).normalized_xy();
                float dot = fwd.dot(dir_to_bot);
                if (dot > 0.35f || dist < 95.0f) {
                    m_telemetry.melee_hit_confirmed = true;
                    m_telemetry.hit_marker_timer = 0.25f;

                    float dmg = 34.0f;
                    const char* hit_seq = "HitMeleeRight";
                    if (m_telemetry.move_state == EMovement::MOVE_MeleeAir) {
                        dmg = 100.0f;
                        hit_seq = "HitMeleeInAir_High";
                        // Bounce off enemy chest slightly after landing a flying jump kick
                        m_telemetry.velocity = -dir_to_bot * 160.0f + Vec3(0, 0, 210.0f);
                    } else if (m_telemetry.melee_variant == 3) {
                        dmg = 50.0f;
                        hit_seq = "HitMeleeCrouchSweep";
                    } else if (m_telemetry.melee_variant == 2) {
                        dmg = 55.0f;
                        hit_seq = "HitMeleeSoccerKick";
                    } else if (m_telemetry.melee_variant == 1) {
                        dmg = 38.0f;
                        hit_seq = "HitMeleeLeft";
                    }

                    bot.health -= dmg;
                    bot.stunned = true;
                    bot.disarm_window = true; // Staggering an enemy opens their red disarm window!
                    bot.attack_timer = 0.0f;
                    bot.anim_timer = 0.0f;
                    bot.active_anim_seq = hit_seq;

                    bool is_trainer = (bot.archetype.find("TutorialTrainer") != std::string::npos ||
                                       bot.archetype.find("Celeste") != std::string::npos);
                    if (bot.health <= 0.0f) {
                        if (is_trainer) {
                            bot.health = 100.0f;
                            bot.anim_state = EEnemyAnimState::HitStagger;
                        } else {
                            bot.alive = false;
                            bot.anim_state = EEnemyAnimState::KnockedOut;
                            if (!bot.weapon_name.empty() && bot.weapon_name != "None") {
                                DroppedWeapon dw{};
                                dw.weapon_name = bot.weapon_name;
                                dw.ammo = 15;
                                dw.position = bot.position + Vec3(0, 0, 45.0f);
                                dw.velocity = dir_to_bot * 110.0f + Vec3(0, 0, 90.0f);
                                dw.yaw_deg = bot.yaw_deg + 30.0f;
                                dw.grounded = false;
                                scene.dropped_weapons.push_back(dw);
                                bot.weapon_name = "None";
                            }
                            m_telemetry.active_subtitle = (m_telemetry.move_state == EMovement::MOVE_MeleeAir)
                                ? "Flying Jump Kick Knockout!"
                                : "Melee Combo Knockout!";
                        }
                    } else {
                        bot.anim_state = EEnemyAnimState::HitStagger;
                        if (m_telemetry.active_subtitle.empty()) {
                            m_telemetry.active_subtitle = "Enemy Staggered - Weapon Red (Press Right-Click / E to Disarm)";
                        }
                    }
                }
            }
        }
    }
}

// -----------------------------------------------------------------------------
// AI Bots Pursuit, Ranged Fire, Melee Windup & Disarm Window Simulation
// -----------------------------------------------------------------------------
void ParkourController::update_ai_bots(float dt, LevelScene& scene) {
    for (auto& bot : scene.enemies) {
        if (bot.is_story_npc) continue;
        bot.anim_timer += dt;
        if (bot.muzzle_flash_timer > 0.0f) {
            bot.muzzle_flash_timer = std::max(0.0f, bot.muzzle_flash_timer - dt);
        }

        if (!bot.alive) {
            bot.anim_state = EEnemyAnimState::KnockedOut;
            bot.disarm_window = false;
            continue;
        }

        if (bot.home_position.length_sq() < 1e-3f) {
            bot.home_position = bot.position;
        }

        bool is_trainer = (bot.archetype.find("TutorialTrainer") != std::string::npos ||
                           bot.archetype.find("Celeste") != std::string::npos ||
                           bot.weapon_name.find("TutorialTrainer") != std::string::npos);

        // Handle stunned / staggered / disarmed recovery
        if (bot.stunned) {
            bot.attack_timer += dt;
            float stun_dur = is_trainer ? 2.2f : 1.6f;
            if (bot.attack_timer >= stun_dur) {
                bot.stunned = false;
                bot.attack_timer = 0.0f;
                bot.anim_timer = 0.0f;
                bot.anim_state = is_trainer ? EEnemyAnimState::MeleeWindup : EEnemyAnimState::AimFire;
                if (is_trainer && (bot.weapon_name.empty() || bot.weapon_name == "None")) {
                    bot.weapon_name = "Colt1911";
                }
            }
            continue;
        }

        float dist = bot.position.distance(m_telemetry.position);
        Vec3 dir_to_player = (m_telemetry.position - bot.position).normalized_xy();
        if (dir_to_player.length_sq() > 1e-4f) {
            bot.yaw_deg = std::atan2(dir_to_player.y, dir_to_player.x) * RAD2DEG;
        }

        bot.attack_timer += dt;

        // Tutorial sparring trainer (Celeste) stays at her training post with disarm window ready
        if (is_trainer) {
            bot.disarm_window = true;
            if (bot.weapon_name.empty() || bot.weapon_name == "None") {
                bot.weapon_name = "Colt1911";
            }
            bot.anim_state = (dist < 260.0f) ? EEnemyAnimState::MeleeWindup : EEnemyAnimState::Idle;
            continue;
        }

        if (dist > 1800.0f) {
            // Patrol / Guard Idle outside engagement radius
            bot.disarm_window = false;
            bot.anim_state = EEnemyAnimState::Idle;
        } else if (dist > 220.0f) {
            // Ranged engagement or closing distance
            bot.disarm_window = false;

            if (dist > 750.0f && bot.position.distance_xy(bot.home_position) < 450.0f) {
                // Advance toward Faith (`RunFwd`)
                bot.anim_state = EEnemyAnimState::Chase;
                Vec3 step_move = dir_to_player * (220.0f * dt);
                TraceHit wall_chk = trace_ray(bot.position + Vec3(0, 0, 50.0f),
                                              bot.position + Vec3(0, 0, 50.0f) + dir_to_player * 55.0f, scene);
                if (!wall_chk.hit) {
                    bot.position += step_move;
                }
            } else {
                bot.anim_state = EEnemyAnimState::AimFire;
            }

            // Fire weapon bursts at Faith
            float fire_cadence = (bot.weapon_name.find("Remington") != std::string::npos ||
                                  bot.weapon_name.find("Neostead") != std::string::npos) ? 1.45f : 0.95f;
            if (bot.attack_timer >= fire_cadence) {
                bot.attack_timer = 0.0f;
                bot.muzzle_flash_timer = 0.08f;

                Vec3 bot_muzzle = bot.position + Vec3(0, 0, 62.0f) + dir_to_player * 35.0f;
                Vec3 target_pt = m_telemetry.position + Vec3(0, 0, m_telemetry.eye_height * 0.75f);

                // Check line of sight so enemies don't shoot through solid walls
                TraceHit los = trace_ray(bot_muzzle, target_pt, scene, COLL_BlockZeroExtent);
                Vec3 tracer_end = los.hit ? los.point : target_pt;

                BulletTracer tr{};
                tr.start_pos = bot_muzzle;
                tr.end_pos = tracer_end;
                tr.timer = 0.085f;
                tr.max_time = 0.085f;
                tr.hit_enemy = false;
                tr.from_player = false;
                {
                    const auto has = [&](const char* part) { return bot.weapon_name.find(part) != std::string::npos; };
                    tr.ammo = (has("Remington") || has("Neostead")) ? 3
                              : (has("G36") || has("SCAR") || has("Minimi") || has("M95") || has("Barret")) ? 1 : 0;
                }
                scene.active_tracers.push_back(tr);

                // Emit 3D gunshot report from AI weapon muzzle
                emit_sound("BerettaM93R_Fire", bot_muzzle, false);

                // Deal damage if line-of-sight is clear and Faith isn't actively evading or in a cutscene
                bool evading = (m_telemetry.intro_active ||
                                m_telemetry.move_state == EMovement::MOVE_Slide ||
                                m_telemetry.move_state == EMovement::MOVE_MeleeSlide ||
                                m_telemetry.move_state == EMovement::MOVE_SkillRoll ||
                                m_telemetry.move_state == EMovement::MOVE_WallRunningLeft ||
                                m_telemetry.move_state == EMovement::MOVE_WallRunningRight ||
                                m_telemetry.move_state == EMovement::MOVE_Snatch ||
                                m_telemetry.speed_2d > 540.0f);
                if (!los.hit && !evading) {
                    apply_damage(10.0f, 0, bot_muzzle - target_pt);
                } else if (!los.hit) {
                    emit_sound("BulletBy.9mm_BulletBy", target_pt, false);
                }
            }
        } else {
            // Close-quarters melee range (< 220 units): wind up rifle/pistol butt strike and open Red Disarm Window!
            if (bot.attack_timer < 0.45f) {
                bot.disarm_window = false;
                bot.anim_state = EEnemyAnimState::AimFire;
            } else if (bot.attack_timer <= 1.45f) {
                if (!bot.disarm_window) {
                    bot.anim_timer = 0.0f;
                }
                bot.disarm_window = true;
                bot.anim_state = EEnemyAnimState::MeleeWindup;
            } else {
                bot.disarm_window = false;
                bot.attack_timer = 0.0f;
                bot.anim_timer = 0.0f;
                bot.anim_state = EEnemyAnimState::MeleeStrike;
                if (m_telemetry.move_state != EMovement::MOVE_Snatch && !m_telemetry.intro_active) {
                    apply_damage(22.0f, 1, bot.position - m_telemetry.position);
                }
            }
        }
    }

    // -------------------------------------------------------------------------
    // TdAI_HeliController + TdVehicle_Helicopter + SeqAct_TdDummyWeaponFire
    // -------------------------------------------------------------------------
    for (auto& heli : scene.helicopters) {
        heli.just_spawned = false;
        heli.just_fired = false;
        heli.muzzle_flash_timer = std::max(0.0f, heli.muzzle_flash_timer - dt);

        if (heli.state == EHeliState::Destroyed) continue;

        Vec3 trig_delta = m_telemetry.position - heli.trigger_pos;
        float dist_trig_xy = std::sqrt(trig_delta.x * trig_delta.x + trig_delta.y * trig_delta.y);
        float dist_trig_z = std::abs(trig_delta.z);
        if (heli.state == EHeliState::Dormant) {
            if (!m_telemetry.intro_active &&
                dist_trig_xy <= heli.trigger_radius && dist_trig_z <= 520.0f) {
                heli.state = (heli.hold_fire_delay > 0.05f) ? EHeliState::Arriving : EHeliState::Engaging;
                heli.active_timer = 0.0f;
                heli.just_spawned = true;
            } else {
                continue;
            }
        }

        heli.active_timer += dt;
        heli.main_rotor_rad = std::fmod(heli.main_rotor_rad + 18.0f * dt, 6.2831853f);
        heli.tail_rotor_rad = std::fmod(heli.tail_rotor_rad + 42.0f * dt, 6.2831853f);

        if (heli.state == EHeliState::Arriving && heli.active_timer >= heli.hold_fire_delay) {
            heli.state = EHeliState::Engaging;
        }
        if (heli.active_timer >= heli.perfect_aim_delay) {
            heli.perfect_aim_active = true;
        }

        // TdAI_HeliController.FindBestAttackPoint() priority scoring across TdAttackPathNodes:
        // DistanceNodeHeliWeight = 1.0, DistanceNodePlayerWeight = 1.5, LastVisitWeight = 2000.0
        Vec3 goal_pos = m_telemetry.position + Vec3(-1400.0f, 900.0f, 950.0f);
        if (!scene.heli_attack_nodes.empty()) {
            float best_score = 1e18f;
            int best_idx = heli.current_node_idx;
            for (size_t ni = 0; ni < scene.heli_attack_nodes.size(); ++ni) {
                const auto& node = scene.heli_attack_nodes[ni];
                float d_player = (node.location - m_telemetry.position).length();
                if (d_player < 550.0f || d_player > 6800.0f) continue;
                float d_heli = (node.location - heli.position).length();
                float visit_age = std::clamp((m_telemetry.sim_time - node.last_visit_time) / 30.0f, 0.0f, 1.0f);
                float visit_penalty = (1.0f - visit_age) * 2000.0f;
                float score = 1.0f * d_heli + 1.5f * d_player + visit_penalty;
                if (score < best_score) {
                    best_score = score;
                    best_idx = static_cast<int>(ni);
                }
            }
            if (best_idx >= 0 && static_cast<size_t>(best_idx) < scene.heli_attack_nodes.size()) {
                heli.current_node_idx = best_idx;
                goal_pos = scene.heli_attack_nodes[static_cast<size_t>(best_idx)].location;
                if ((heli.position - goal_pos).length() < 320.0f) {
                    scene.heli_attack_nodes[static_cast<size_t>(best_idx)].last_visit_time = m_telemetry.sim_time;
                }
            }
        }
        // Subtle hover oscillation (TdVehicle_Helicopter.HoveringNoiceDirection)
        goal_pos.z += std::sin(m_telemetry.sim_time * 1.4f) * 35.0f;

        Vec3 to_goal = goal_pos - heli.position;
        float dist_goal = to_goal.length();
        float max_spd = (heli.speed_setting == EHeliSpeed::Slow) ? 1000.0f : 1850.0f;
        Vec3 desired_vel = (dist_goal > 20.0f)
            ? to_goal * (std::min(max_spd, dist_goal * 1.6f) / dist_goal)
            : Vec3(0.0f, 0.0f, 0.0f);
        Vec3 dv = desired_vel - heli.velocity;
        float max_dv = 1000.0f * dt;
        if (dv.length() > max_dv && dv.length() > 1e-4f) {
            dv = dv.normalized() * max_dv;
        }
        heli.velocity += dv;
        heli.position += heli.velocity * dt;

        // Broadside orientation (EHeliAttackSide): present Left (+90 yaw) or Right (-90 yaw) door gunner to Faith
        Vec3 to_player = m_telemetry.position - heli.position;
        float bearing_deg = std::atan2(to_player.y, to_player.x) * RAD2DEG;
        float side_offset = (heli.side_preference == EHeliAttackSide::Right ||
                             heli.side_preference == EHeliAttackSide::UseRightWhenHovering) ? -90.0f : 90.0f;
        float target_yaw = bearing_deg + side_offset;
        float yaw_diff = std::fmod(target_yaw - heli.yaw_deg + 540.0f, 360.0f) - 180.0f;
        heli.yaw_deg += std::clamp(yaw_diff, -75.0f * dt, 75.0f * dt);

        // Banking pitch & roll (StayUprightPitchResistAngle = 6.0, StayUprightRollResistAngle = 6.0)
        float cy = std::cos(heli.yaw_deg * DEG2RAD);
        float sy = std::sin(heli.yaw_deg * DEG2RAD);
        float fwd_spd = heli.velocity.x * cy + heli.velocity.y * sy;
        float side_spd = -heli.velocity.x * sy + heli.velocity.y * cy;
        heli.pitch_deg = std::clamp(-fwd_spd * 0.008f, -12.0f, 12.0f);
        heli.roll_deg = std::clamp(side_spd * 0.010f, -15.0f, 15.0f);

        // Door gunner FNMinimi bursts when Engaging, in broadside door arc, and outside cutscenes
        Vec3 heli_fwd(cy, sy, 0.0f);
        Vec3 dir_player_2d = Vec3(to_player.x, to_player.y, 0.0f).normalized();
        float nose_dot = std::abs(heli_fwd.dot(dir_player_2d));
        float burst_cycle = std::fmod(heli.active_timer, 2.20f);
        bool in_burst_window = (burst_cycle < 0.85f);
        if (!m_telemetry.intro_active &&
            heli.state == EHeliState::Engaging &&
            in_burst_window &&
            nose_dot < 0.82f &&
            to_player.length() < 6500.0f) {
            heli.burst_timer += dt;
            if (heli.burst_timer >= 0.14f) {
                heli.burst_timer = 0.0f;
                heli.muzzle_flash_timer = 0.07f;
                heli.just_fired = true;

                Vec3 right(-sy, cy, 0.0f);
                float side_sign = (side_offset < 0.0f) ? 1.0f : -1.0f;
                Vec3 gun_muzzle = heli.position + right * (side_sign * 165.0f) - Vec3(0.0f, 0.0f, 45.0f);
                float spread_scale = heli.perfect_aim_active ? 24.0f : 115.0f;
                float phase = m_telemetry.sim_time * 13.7f;
                Vec3 aim_pt = m_telemetry.position + Vec3(std::sin(phase) * spread_scale,
                                                          std::cos(phase * 1.3f) * spread_scale,
                                                          m_telemetry.eye_height * 0.65f);
                TraceHit los = trace_ray(gun_muzzle, aim_pt, scene, COLL_BlockZeroExtent);
                BulletTracer tr{};
                tr.start_pos = gun_muzzle;
                tr.end_pos = los.hit ? los.point : aim_pt;
                tr.timer = 0.09f;
                tr.max_time = 0.09f;
                tr.hit_enemy = false;
                tr.from_player = false;
                tr.ammo = 2;  // the helicopter's gun
                scene.active_tracers.push_back(tr);

                // SequenceFrame_30 [Reduce Gunner Accuracy During Slide] + high-speed parkour evasion
                bool evading = !heli.perfect_aim_active &&
                               (m_telemetry.move_state == EMovement::MOVE_Slide ||
                                m_telemetry.move_state == EMovement::MOVE_SkillRoll ||
                                m_telemetry.move_state == EMovement::MOVE_WallRunningLeft ||
                                m_telemetry.move_state == EMovement::MOVE_WallRunningRight ||
                                m_telemetry.speed_2d > 480.0f);
                if (!los.hit && !evading) {
                    apply_damage(4.5f, 0, gun_muzzle - aim_pt);
                } else if (!los.hit) {
                    emit_sound("BulletBy.9mm_BulletBy", aim_pt, false);
                }
            }
        }
    }

    // Scripted window/corridor gunfire setpieces (SeqAct_TdDummyWeaponFire, e.g. Escape_Off-R1_Spt)
    for (auto& bar : scene.dummy_fire_barrages) {
        if (!bar.activated) {
            if ((m_telemetry.position - bar.trigger_pos).length() <= bar.trigger_radius) {
                bar.activated = true;
                bar.timer = -bar.delay_sec;
            } else {
                continue;
            }
        }
        if (bar.shots_fired >= bar.shots_to_fire) continue;
        bar.timer += dt;
        if (bar.timer >= 0.09f) {
            bar.timer = 0.0f;
            float angle = bar.spread_deg * DEG2RAD * std::sin(static_cast<float>(bar.shots_fired) * 1.9f);
            Vec3 base_dir = (bar.target - bar.origin);
            float dist = base_dir.length();
            Vec3 end_pt = bar.target + Vec3(std::sin(angle) * dist * 0.12f,
                                            std::cos(angle * 1.4f) * dist * 0.12f,
                                            std::sin(angle * 0.7f) * 65.0f);
            TraceHit hit = trace_ray(bar.origin, end_pt, scene, COLL_BlockZeroExtent);
            BulletTracer tr{};
            tr.start_pos = bar.origin;
            tr.end_pos = hit.hit ? hit.point : end_pt;
            tr.timer = 0.09f;
            tr.max_time = 0.09f;
            tr.hit_enemy = false;
            tr.from_player = false;
            scene.active_tracers.push_back(tr);
            emit_sound("BerettaM93R_Fire", bar.origin, false);
            emit_sound("BulletBy.9mm_BulletBy", tr.end_pos, false);
            bar.shots_fired++;
        }
    }
}

// -----------------------------------------------------------------------------
// UTdHudEffectManager + ATdPlayerPawn::TakeDamage / PlayHitCameraShake
// (Reverse-engineered from MirrorsEdge.exe 0x01264410, 0x01261330, 0x01261e00,
//  0x01262550, 0x01262930, 0x012b0d60 & DefaultHudEffects.ini)
// -----------------------------------------------------------------------------
void ParkourController::HudEffectEnvelope::trigger(float target_peak, float in_s, float hold_s, float out_s) {
    fade_in = std::max(0.001f, in_s);
    hold = std::max(0.0f, hold_s);
    fade_out = std::max(0.001f, out_s);
    start_val = current_val;
    peak_val = std::max(current_val, target_peak);
    phase = 1;
    timer = 0.0f;
}

void ParkourController::HudEffectEnvelope::reset() {
    phase = 0;
    timer = 0.0f;
    start_val = 0.0f;
    peak_val = 0.0f;
    current_val = 0.0f;
}

float ParkourController::HudEffectEnvelope::step(float dt) {
    if (phase == 0) {
        current_val = 0.0f;
        return 0.0f;
    }
    timer += dt;
    if (phase == 1) {
        if (timer < fade_in) {
            float a = timer / fade_in;
            current_val = start_val + (peak_val - start_val) * a;
            return current_val;
        }
        timer -= fade_in;
        current_val = peak_val;
        phase = 2;
    }
    if (phase == 2) {
        if (timer < hold) {
            current_val = peak_val;
            return current_val;
        }
        timer -= hold;
        phase = 3;
    }
    if (phase == 3) {
        if (timer < fade_out) {
            float a = timer / fade_out;
            current_val = peak_val * (1.0f - a);
            return current_val;
        }
        reset();
    }
    return current_val;
}

void ParkourController::apply_damage(float amount, int dmt, const Vec3& hit_dir_world) {
    if (m_telemetry.intro_active || amount <= 0.0f) return;

    if (dmt == 2) {
        // Non-lethal hard landing fall damage clamps at 1 HP
        m_telemetry.health = std::max(1.0f, m_telemetry.health - amount);
    } else {
        m_telemetry.health = std::max(0.0f, m_telemetry.health - amount);
    }
    m_damage_cooldown = m_config.health_regen_delay;

    // UTdHudEffectManager::GetHitAngleNorm (0x01262930):
    // Incoming direction in [0, 1) around camera yaw (0.0 = front, 0.25 = right, 0.5 = back, 0.75 = left)
    float hit_angle_norm = 0.0f;
    Vec3 d = horiz(hit_dir_world);
    if (d.length_sq() > 1e-6f) {
        d = d.normalized();
        float fwd_dot = d.dot(facing_forward());
        float right_dot = d.dot(facing_right());
        float angle_rad = std::atan2(right_dot, fwd_dot);
        hit_angle_norm = angle_rad / (2.0f * 3.14159265f);
        if (hit_angle_norm < 0.0f) hit_angle_norm += 1.0f;
    }

    // 1. Directional 1P Camera Hit Shake (ATdPlayerPawn::PlayHitCameraShake, 0x012b0d60)
    // Plays AS_F_1P_Unarmed gethitfront/gethitback/gethitleft/gethitright on CustomCameraNode (Slot::Camera)
    if ((dmt == 0 || dmt == 1) && m_hit_spazz_cooldown <= 0.0f) {
        m_hit_spazz_cooldown = 0.20f;  // DefaultHudEffects.ini SpazzThrottle = 0.2s
        if (hit_angle_norm <= 0.125f || hit_angle_norm >= 0.875f) {
            set_camera_anim("gethitfront");
        } else if (hit_angle_norm < 0.375f) {
            set_camera_anim("gethitright");
        } else if (hit_angle_norm < 0.625f) {
            set_camera_anim("gethitback");
        } else {
            set_camera_anim("gethitleft");
        }
    }

    // 2. Health Desaturation (ActivateSaturationEffect 0x012624e0 + PPHealthSaturationSettings)
    // FadeInDuration=0.06, Duration=0.5, FadeOutDuration=0.5
    float missing_health = std::clamp((100.0f - m_telemetry.health) * 0.01f, 0.0f, 1.0f);
    float desat_impulse = std::clamp(m_env_health_desat.current_val + std::max(0.32f, amount * 0.015f), 0.0f, 1.0f);
    m_env_health_desat.trigger(std::max(desat_impulse, missing_health), 0.06f, 0.50f, 0.50f);
    m_telemetry.health_desat = std::max(m_telemetry.health_desat, m_env_health_desat.peak_val);

    // 3. Hit Blur + Type-Specific Post-Process & Screen-Space Particle Effects (DisplayHit 0x01264410)
    if (dmt == 0) {
        // TdHudEffect_Bullet:
        // Blur=(FadeInDuration=0.03, Duration=0.25, FadeOutDuration=0.15)
        // Particles=(FadeInDuration=0.05, Duration=0.2, FadeOutDuration=0.0) -> PS_FX_FullScreenFX_BulletHit_01
        m_env_blur.trigger(1.0f, 0.03f, 0.25f, 0.15f);
        m_telemetry.damage_flash_timer = std::max(m_telemetry.damage_flash_timer, 0.25f);

        int slot = static_cast<int>(m_bullet_hit_counter % 4u);
        ++m_bullet_hit_counter;
        m_telemetry.bullet_hit_angles[slot] = hit_angle_norm;
        m_telemetry.bullet_hit_timers[slot] = 0.25f;
        m_telemetry.bullet_hit_seeds[slot] = m_bullet_hit_counter;

        emit_sound("Faith.9mm_Faith_Impact", m_telemetry.position, true);
        emit_sound("Oral_Impact.Hard", m_telemetry.position, true);
    } else if (dmt == 1) {
        // TdHudEffect_Melee:
        // Blur=(FadeInDuration=0.05, Duration=0.15, FadeOutDuration=0.2)
        // PP=(FadeInDuration=0.05, Duration=0.15, FadeOutDuration=0.4) -> M_FX_FullScreenFX_MeleeDamage_01
        m_env_blur.trigger(1.0f, 0.05f, 0.15f, 0.20f);
        m_env_melee.trigger(1.0f, 0.05f, 0.15f, 0.40f);
        ++m_telemetry.melee_hit_count;
        m_telemetry.melee_hit_damage = amount;
        m_telemetry.melee_hit_turns = hit_angle_norm;
        m_telemetry.melee_hit_dir = hit_angle_norm;
        m_telemetry.melee_damage_strength = std::max(m_telemetry.melee_damage_strength, 0.35f);
        m_telemetry.damage_flash_timer = std::max(m_telemetry.damage_flash_timer, 0.35f);

        emit_sound("Punch_Hit", m_telemetry.position, true);
        emit_sound("Oral_Impact.Hard", m_telemetry.position, true);
    } else if (dmt == 2) {
        // TdHudEffect_FallDamage:
        // Blur=(FadeInDuration=0.05, Duration=0.1, FadeOutDuration=0.75)
        // PP=(FadeInDuration=0.05, Duration=0.1, FadeOutDuration=0.75) -> M_FX_FullScreenFX_Falldamage_01
        m_env_blur.trigger(1.0f, 0.05f, 0.10f, 0.75f);
        m_env_fall.trigger(1.0f, 0.05f, 0.10f, 0.75f);
        ++m_telemetry.fall_hit_count;
        m_telemetry.fall_hit_damage = amount;
        m_telemetry.fall_damage_strength = std::max(m_telemetry.fall_damage_strength, 0.35f);
        m_telemetry.damage_flash_timer = std::max(m_telemetry.damage_flash_timer, 0.40f);

        emit_sound("Oral_Impact.Hard", m_telemetry.position, true);
    }
    m_telemetry.hit_blur = std::max(m_telemetry.hit_blur, 0.35f);
    m_telemetry.hit_focus_distance = -500.0f;
}

// -----------------------------------------------------------------------------
// Health Regeneration & HUD Damage Envelopes Subsystem
// -----------------------------------------------------------------------------
void ParkourController::update_health_and_regen(float dt) {
    if (m_telemetry.intro_active) {
        m_telemetry.health = 100.0f;
        m_telemetry.damage_flash_timer = 0.0f;
        m_telemetry.health_desat = 0.0f;
        m_telemetry.hit_blur = 0.0f;
        m_telemetry.hit_focus_distance = 1600.0f;
        m_telemetry.melee_damage_strength = 0.0f;
        m_telemetry.fall_damage_strength = 0.0f;
        for (int i = 0; i < 4; ++i) m_telemetry.bullet_hit_timers[i] = 0.0f;
        return;
    }
    if (m_hit_spazz_cooldown > 0.0f) {
        m_hit_spazz_cooldown = std::max(0.0f, m_hit_spazz_cooldown - dt);
    }
    if (m_damage_cooldown > 0.0f) {
        m_damage_cooldown -= dt;
    } else if (m_telemetry.health < 100.0f && m_telemetry.health > 0.0f) {
        m_telemetry.health = std::min(100.0f, m_telemetry.health + m_config.health_regen_rate * dt);
    }

    // Advance UTdHudEffectManager post-process and particle envelopes
    float env_desat = m_env_health_desat.step(dt);
    float low_health_desat = (m_telemetry.health > 0.0f)
        ? std::clamp((100.0f - m_telemetry.health) / 85.0f, 0.0f, 1.0f)
        : 1.0f;
    m_telemetry.health_desat = std::max(env_desat, low_health_desat);

    float blur_env = m_env_blur.step(dt);
    // When health is critically low (< 40 HP), maintain subtle peripheral blur
    float low_health_blur = (m_telemetry.health > 0.0f && m_telemetry.health < 40.0f)
        ? ((40.0f - m_telemetry.health) / 40.0f) * 0.45f
        : 0.0f;
    float total_blur_env = std::max(blur_env, low_health_blur);
    m_telemetry.hit_blur = total_blur_env * 0.95f;
    m_telemetry.hit_focus_distance = 1600.0f + (-500.0f - 1600.0f) * total_blur_env;

    m_telemetry.melee_damage_strength = m_env_melee.step(dt);
    m_telemetry.fall_damage_strength = m_env_fall.step(dt);

    for (int i = 0; i < 4; ++i) {
        if (m_telemetry.bullet_hit_timers[i] > 0.0f) {
            m_telemetry.bullet_hit_timers[i] = std::max(0.0f, m_telemetry.bullet_hit_timers[i] - dt);
        }
    }
}

// -----------------------------------------------------------------------------
// Checkpoints, Kill Volumes & Collectibles Subsystem
// -----------------------------------------------------------------------------
void ParkourController::update_checkpoints_and_volumes(LevelScene& scene) {
    if (m_telemetry.intro_active) {
        m_last_checkpoint_pos.z = std::min(m_last_checkpoint_pos.z, m_telemetry.position.z);
        return;
    }
    // When grounded safely on a lower rooftop/cushion after a zipline or elevator drop, lower the
    // checkpoint vertical void baseline so multi-story descents never trigger a false abyss kill.
    if (m_telemetry.grounded && m_telemetry.health > 0.0f && m_telemetry.position.z < m_last_checkpoint_pos.z) {
        m_last_checkpoint_pos.z = m_telemetry.position.z;
    }
    // A zipline carries the pawn far below its checkpoint: no void kill while riding one, and after
    // letting go the drop is measured from where it let go until it lands.
    if (m_telemetry.grounded) m_zip_exit_z = 1e30f;
    const float void_ref_z = std::min(m_last_checkpoint_pos.z, m_zip_exit_z);
    const bool void_fall = !m_telemetry.grounded && m_telemetry.move_state != EMovement::MOVE_ZipLine &&
                           (m_telemetry.position.z < void_ref_z - 2200.0f) &&
                           !has_soft_landing_below(scene);

    // 1. Non-floor Death (WorldInfo.KillZ, void drop below checkpoint, or combat health <= 0):
    // Note: fall_death_impact is ONLY set on physical floor impact in land(); void/combat deaths
    // do NOT play concrete body-splat (Death_Impact).
    if (m_telemetry.position.z < scene.kill_z ||
        void_fall ||
        m_telemetry.health <= 0.0f) {
        if (!m_telemetry.falling_to_death) {
            m_telemetry.falling_to_death = true;
            m_telemetry.health = 0.0f;
            m_death_total_duration = m_telemetry.grounded ? 1.35f : 0.90f;
            m_death_timer = m_death_total_duration;
            m_telemetry.death_anim_progress = 0.0f;
            m_telemetry.damage_flash_timer = 0.45f;
        } else {
            m_death_timer -= m_frame_dt;
            m_telemetry.death_anim_progress = std::clamp(
                1.0f - (m_death_timer / std::max(0.10f, m_death_total_duration)), 0.0f, 1.0f);
            if (m_death_timer <= 0.0f) {
                // The level script's checkpoint when it has one; the drifting void baseline otherwise.
                const Vec3 respawn = scene.script_checkpoints ? m_respawn_pos : m_last_checkpoint_pos;
                const float respawn_yaw = scene.script_checkpoints ? m_respawn_yaw : m_last_checkpoint_yaw;
                const int keep_index = m_telemetry.active_checkpoint;
                const std::string keep_name = m_telemetry.active_checkpoint_name;
                reset(respawn, respawn_yaw);
                m_telemetry.active_checkpoint = keep_index;
                m_telemetry.active_checkpoint_name = keep_name;
                m_telemetry.respawned = true;
                if (!scene.script_checkpoints) m_telemetry.active_subtitle = "Respawned at Checkpoint";
            }
        }
        return;
    }

    // 2. Checkpoints & TdCheckpoint.StreamingLevels. With a level script the checkpoints are its
    // SeqAct_TdCheckpoint actions (set_checkpoint); the proximity test is for levels without one.
    for (size_t i = 0; i < scene.checkpoints.size() && !scene.script_checkpoints; ++i) {
        if (m_telemetry.position.distance(scene.checkpoints[i]) < 240.0f) {
            if (static_cast<int>(i) > m_telemetry.active_checkpoint) {
                m_telemetry.active_checkpoint = static_cast<int>(i);
                m_last_checkpoint_pos = scene.checkpoints[i];
                m_last_checkpoint_yaw = m_telemetry.yaw_deg;
                if (i < scene.checkpoint_infos.size()) {
                    const auto& cp = scene.checkpoint_infos[i];
                    m_telemetry.active_checkpoint_name = cp.checkpoint_name;
                    if (!cp.streaming_levels.empty()) {
                        scene.loaded_sublevel_packages = cp.streaming_levels;
                    }
                    m_telemetry.active_subtitle = "Checkpoint: " + cp.checkpoint_name;
                }
                if (i < scene.subtitles.size() && !scene.subtitles[i].empty()) {
                    m_telemetry.active_subtitle = scene.subtitles[i];
                } else if (i >= scene.checkpoint_infos.size()) {
                    m_telemetry.active_subtitle = "Checkpoint Reached";
                }
            }
        }
    }
    m_telemetry.streamed_sublevel_count = static_cast<int>(scene.loaded_sublevel_packages.size());

    // 3. Courier Bags (`is_bag`)
    for (auto& act : scene.actors) {
        if (act.is_bag && m_telemetry.position.distance(act.location) < 100.0f) {
            act.is_bag = false; // collected
            m_telemetry.bags_collected++;
            m_telemetry.active_subtitle = "Runner Bag Collected! (" + std::to_string(m_telemetry.bags_collected) + ")";
        }
    }
}

// -----------------------------------------------------------------------------
// Interactive Elevator & Mid-Shaft Multi-Level Streaming Subsystem
// (Reverse-engineered from *_Slc.me1 / *_Spt.me1 InterpActor + InterpTrackMove)
// -----------------------------------------------------------------------------
// Drives the real elevator InterpActors: the cab and the actors attached to it follow the cab
// PosTrack, the sliding doors follow their door matinees. A pawn standing on a moving part
// (Pawn.Base) is carried with it, exactly like UE3 based movement.
void ParkourController::update_elevators(const InputFrame& input, float dt, LevelScene& scene) {
    m_telemetry.in_elevator = false;
    m_telemetry.active_elevator_idx = -1;

    for (size_t i = 0; i < scene.elevators.size(); ++i) {
        ElevatorInstance& elev = scene.elevators[i];
        elev.prev_pos = elev.current_pos;

        // Cab interior (S_Elevator_01 mesh bounds) at the cab's current position.
        const Vec3 fc_prev = elev.prev_pos + elev.cab_local_offset;
        const Vec3 he = elev.cab_half_extents;
        const float h_cab = he.z * 2.0f;
        const Vec3& p = m_telemetry.position;
        const bool player_inside = (std::abs(p.x - fc_prev.x) <= he.x - 8.0f) &&
                                   (std::abs(p.y - fc_prev.y) <= he.y - 8.0f) &&
                                   (p.z >= fc_prev.z - 30.0f) &&
                                   (p.z <= fc_prev.z + h_cab + 10.0f);

        const bool button_pressed = input.use && (player_inside || p.distance(elev.button_pos) < 180.0f);

        switch (elev.state) {
            case ElevatorState::IdleStart: {
                elev.door_open_Start = 1.0f;
                elev.door_open_End = 0.0f;
                // Trigger when Faith walks inside the cab and passes its center or presses E on the button
                const bool deep_inside = player_inside &&
                                         (std::abs(p.x - fc_prev.x) <= he.x * 0.65f) &&
                                         (std::abs(p.y - fc_prev.y) <= he.y * 0.65f);
                if ((elev.auto_trigger_on_enter && deep_inside) || button_pressed) {
                    elev.state = ElevatorState::DoorsClosing;
                    elev.timer = 0.0f;
                    m_telemetry.active_subtitle = "Elevator Activated - Doors Closing";
                }
                break;
            }

            case ElevatorState::DoorsClosing: {
                elev.timer += dt;
                const float t01 = std::clamp(elev.timer / std::max(0.1f, elev.door_duration), 0.0f, 1.0f);
                elev.door_open_Start = 1.0f - t01;
                elev.door_open_End = 0.0f;
                if (elev.timer >= elev.door_duration) {
                    elev.door_open_Start = 0.0f;
                    elev.state = ElevatorState::Moving;
                    elev.timer = 0.0f;
                    m_telemetry.active_subtitle = "Elevator In Transit - Streaming Sublevels";
                }
                break;
            }

            case ElevatorState::Moving: {
                elev.timer += dt;
                const float dur = std::max(0.1f, elev.ride_duration);

                // Evaluate UE3 InterpTrackMove PosTrack curve (Hermite smoothstep between keyframes)
                if (elev.keyframes.size() >= 2) {
                    if (elev.timer <= elev.keyframes.front().time) {
                        elev.current_pos = elev.keyframes.front().pos;
                    } else if (elev.timer >= elev.keyframes.back().time) {
                        elev.current_pos = elev.keyframes.back().pos;
                    } else {
                        for (size_t k = 0; k + 1 < elev.keyframes.size(); ++k) {
                            const float t0 = elev.keyframes[k].time;
                            const float t1 = elev.keyframes[k + 1].time;
                            if (elev.timer >= t0 && elev.timer <= t1) {
                                const float seg_u = (t1 > t0 + 1e-5f) ? (elev.timer - t0) / (t1 - t0) : 1.0f;
                                const float s = seg_u * seg_u * (3.0f - 2.0f * seg_u);
                                elev.current_pos = elev.keyframes[k].pos + (elev.keyframes[k + 1].pos - elev.keyframes[k].pos) * s;
                                break;
                            }
                        }
                    }
                } else {
                    const float u = std::clamp(elev.timer / dur, 0.0f, 1.0f);
                    const float s = u * u * (3.0f - 2.0f * u);
                    elev.current_pos = elev.start_pos + (elev.end_pos - elev.start_pos) * s;
                }

                const float progress = std::clamp(elev.timer / dur, 0.0f, 1.0f);

                // Mid-shaft Kismet SeqAct_MultiLevelStreaming / TdCheckpoint.StreamingLevels transition
                if (!elev.streaming_triggered && progress >= 0.5f) {
                    elev.streaming_triggered = true;
                    for (const auto& out_pkg : elev.stream_out_packages) {
                        scene.loaded_sublevel_packages.erase(
                            std::remove(scene.loaded_sublevel_packages.begin(),
                                        scene.loaded_sublevel_packages.end(), out_pkg),
                            scene.loaded_sublevel_packages.end());
                    }
                    for (const auto& in_pkg : elev.stream_in_packages) {
                        if (std::find(scene.loaded_sublevel_packages.begin(),
                                      scene.loaded_sublevel_packages.end(), in_pkg) == scene.loaded_sublevel_packages.end()) {
                            scene.loaded_sublevel_packages.push_back(in_pkg);
                        }
                    }
                    if (elev.target_checkpoint_idx >= 0 &&
                        static_cast<size_t>(elev.target_checkpoint_idx) < scene.checkpoint_infos.size()) {
                        const auto& dst_cp = scene.checkpoint_infos[static_cast<size_t>(elev.target_checkpoint_idx)];
                        if (!dst_cp.streaming_levels.empty()) {
                            scene.loaded_sublevel_packages = dst_cp.streaming_levels;
                        }
                        m_telemetry.active_checkpoint = std::max(m_telemetry.active_checkpoint, elev.target_checkpoint_idx);
                        m_telemetry.active_checkpoint_name = dst_cp.checkpoint_name;
                    }
                    m_last_checkpoint_pos = elev.end_pos + elev.cab_local_offset + Vec3(0.0f, 0.0f, 35.0f);
                    m_telemetry.active_subtitle = "Streamed Sublevels: " +
                        (elev.stream_in_packages.empty() ? elev.name : elev.stream_in_packages.front());
                }

                if (elev.timer >= dur) {
                    elev.current_pos = elev.end_pos;
                    elev.state = ElevatorState::DoorsOpening;
                    elev.timer = 0.0f;
                }
                break;
            }

            case ElevatorState::DoorsOpening: {
                elev.timer += dt;
                const float t01 = std::clamp(elev.timer / std::max(0.1f, elev.door_duration), 0.0f, 1.0f);
                elev.door_open_End = t01;
                if (elev.timer >= elev.door_duration) {
                    elev.door_open_End = 1.0f;
                    elev.state = ElevatorState::IdleEnd;
                    elev.timer = 0.0f;
                    m_telemetry.active_subtitle = "Elevator Arrived - Upper Zone Loaded";
                }
                break;
            }

            case ElevatorState::IdleEnd: {
                elev.door_open_End = 1.0f;
                break;
            }
        }

        // Pose the real InterpActors: the cab (and everything based on it) is displaced along the cab
        // PosTrack; sliding door leaves move along their door-matinee open offsets.
        const Vec3 cab_offset = elev.current_pos - elev.start_pos;
        float cab_doors_open = 0.0f;
        if (elev.state == ElevatorState::IdleStart || elev.state == ElevatorState::DoorsClosing) {
            cab_doors_open = elev.door_open_Start;
        } else if (elev.state == ElevatorState::DoorsOpening || elev.state == ElevatorState::IdleEnd) {
            cab_doors_open = elev.door_open_End;
        }
        for (auto& part : elev.parts) {
            part.prev_offset = part.offset;
            switch (part.role) {
                case ElevatorPartRole::Cab:
                case ElevatorPartRole::CabAttached:
                    part.offset = cab_offset;
                    break;
                case ElevatorPartRole::CabDoor:
                    part.offset = cab_offset + part.door_open_offset * cab_doors_open;
                    break;
                case ElevatorPartRole::StartDoor:
                    part.offset = part.door_open_offset * elev.door_open_Start;
                    break;
                case ElevatorPartRole::EndDoor:
                    part.offset = part.door_open_offset * elev.door_open_End;
                    break;
            }
        }

        // UE3 based movement: a pawn standing on a moving InterpActor rides with it.
        if (m_telemetry.grounded && m_base_actor >= 0) {
            for (const auto& part : elev.parts) {
                if (part.actor_index == m_base_actor) {
                    m_telemetry.position += part.offset - part.prev_offset;
                    break;
                }
            }
        }

        if (player_inside) {
            // Prevent false fall-damage accumulation during downward elevator rides
            m_fall_peak_z = m_telemetry.position.z;
            m_air_fall_start_z = m_telemetry.position.z;

            m_telemetry.in_elevator = true;
            m_telemetry.active_elevator_idx = static_cast<int>(i);
            if (elev.state == ElevatorState::Moving) {
                m_telemetry.elevator_progress = std::clamp(elev.timer / std::max(0.1f, elev.ride_duration), 0.0f, 1.0f);
            } else if (elev.state == ElevatorState::DoorsOpening || elev.state == ElevatorState::IdleEnd) {
                m_telemetry.elevator_progress = 1.0f;
            } else {
                m_telemetry.elevator_progress = 0.0f;
            }
        }
    }

    m_telemetry.streamed_sublevel_count = static_cast<int>(scene.loaded_sublevel_packages.size());
}

// -----------------------------------------------------------------------------
// TdMove_Barge (MOVE_Barge) and the barge doors' Kismet
// -----------------------------------------------------------------------------
void ParkourController::emit_sound(const char* cue, const Vec3& location, bool at_pawn) {
    SimSoundEvent ev;
    ev.cue = cue;
    ev.location = location;
    ev.at_pawn = at_pawn;
    m_telemetry.sound_events.push_back(std::move(ev));
}

int ParkourController::find_barge_door(const LevelScene& scene, const Vec3& dir, float dist, Vec3& hit_point) const {
    // TdMove_Barge.CalcBargeDamage: PawnOwner.Trace(.., bTraceActors, zero extent, TRACEFLAG_Bullet)
    // from the pawn's Location (the cylinder's centre). The first thing hit has to be interactable;
    // a wall or the door frame in the way means no barge. Check both standing and crouched cylinder centres.
    const float heights[2] = {0.5f * kPawnHeight, 0.5f * kCrouchHeight};
    for (float hz : heights) {
        const Vec3 start = m_telemetry.position + Vec3(0.0f, 0.0f, hz);
        const TraceHit hit = trace_ray(start, start + dir * dist, scene, COLL_BlockZeroExtent);
        if (!hit.hit || hit.actor_index < 0 || static_cast<size_t>(hit.actor_index) >= scene.actors.size()) continue;
        const int door = scene.actors[static_cast<size_t>(hit.actor_index)].barge_door;
        if (door < 0 || static_cast<size_t>(door) >= scene.barge_doors.size()) continue;
        if (scene.barge_doors[static_cast<size_t>(door)].state != DoorState::Closed) continue;
        hit_point = hit.point;
        return door;
    }
    return -1;
}

bool ParkourController::try_initiate_barge(const LevelScene& scene) {
    const EMovement st = m_telemetry.move_state;
    const bool walking = m_telemetry.grounded &&
                         (st == EMovement::MOVE_Walking || st == EMovement::MOVE_StepUp ||
                          st == EMovement::MOVE_AutoStepUp || st == EMovement::MOVE_SoftLanding);
    if (!walking || m_melee_cooldown > 0.0f) return false;

    // TdMove_Barge.CanDoMove: no running barge with a heavy weapon; the trace reaches as far as
    // BargeTraceTime at BargeSpeed when moving the way the pawn faces, else BargeMinTraceDistance.
    const Vec3 vel(m_telemetry.velocity.x, m_telemetry.velocity.y, 0.0f);
    const float speed = vel.length();
    const bool heavy = m_telemetry.weapon.equipped && m_telemetry.weapon.is_heavy;  // GetWeaponType() == 1
    if (heavy && speed > kBargeKickThresholdSpeed) return false;
    const float barge_speed = std::min(kBargeMaxSpeed, speed + kBargeAddOnSpeed);
    const Vec3 facing = Rotator::from_degrees(0.0f, m_pawn_yaw, 0.0f).forward();  // vector(PawnOwner.Rotation)
    const Vec3 cam_fwd = facing_forward();
    const Vec3 vel_dir = (speed > 1e-3f) ? vel * (1.0f / speed) : Vec3(0.0f, 0.0f, 0.0f);
    const bool forward = (facing.dot(vel_dir) > 0.707f || cam_fwd.dot(vel_dir) > 0.707f);
    const float trace_dist =
        forward ? std::max(kBargeMinTraceDistance, barge_speed * kBargeTraceTime) : kBargeMinTraceDistance;
    Vec3 hit_point;
    int door = find_barge_door(scene, facing, trace_dist, hit_point);
    if (door < 0) door = find_barge_door(scene, cam_fwd, trace_dist, hit_point);
    if (door < 0) return false;

    // TdMove_Barge.StartMove / StartBargin.
    m_telemetry.move_state = EMovement::MOVE_Barge;
    m_state_timer = 0.0f;
    m_barge_door = door;
    m_barge_dealt_damage = false;
    m_barge_anim_pos = 0.0f;
    m_barge_anim_elapsed = 0.0f;
    m_melee_cooldown = std::max(m_melee_cooldown, 0.45f);  // the press barged: no punch this frame
    if (speed > kBargeKickThresholdSpeed && forward) {
        // Shoulder first: Velocity = Normal(Velocity) * BargeSpeed, then SetPreciseLocation(Location +
        // BargeTraceTime * Velocity, 1, BargeSpeed) runs the pawn straight at the door. BargeInLeft
        // is sped up / slowed down so its impact (BargeAnimTime in) meets the door.
        m_barge_kick = false;
        m_barge_speed = barge_speed;
        m_barge_dir = vel_dir;
        m_telemetry.velocity.x = vel_dir.x * barge_speed;
        m_telemetry.velocity.y = vel_dir.y * barge_speed;
        m_barge_target = m_telemetry.position + vel_dir * (barge_speed * kBargeTraceTime);
        m_barge_precise = true;
        const float time_to_door = horiz(hit_point - m_telemetry.position).length() / barge_speed;
        m_barge_anim_rate = std::clamp(kBargeAnimTime / std::max(time_to_door, 1e-3f), 0.7f, 1.3f);
        m_barge_anim = 1;
        set_move_anim("BargeInLeft", m_barge_anim_rate);  // PlayMoveAnim(CNT_UpperBody, .., AnimPlayRate, 0.2, 0.0)
    } else {
        // The kick (MeleeKickObject): move input is ignored for 0.6 s and the animation's
        // BargeHitNotify opens the door.
        m_barge_kick = true;
        m_barge_speed = 0.0f;
        m_barge_dir = facing;
        m_barge_precise = false;
        m_barge_anim_rate = 1.0f;
        m_barge_anim = 3;
        m_ignore_move_input = kBargeKickIgnoreInput;
        set_move_anim("MeleeKickObject");  // PlayMoveAnim(CNT_FullBody, .., 1.0, 0.1, 0.1)
    }
    m_telemetry.barge_anim = m_barge_anim;
    m_telemetry.barge_anim_pos = 0.0f;
    m_telemetry.barge_anim_weight = 0.0f;
    m_telemetry.combat_anim_time = 0.0f;
    m_telemetry.combat_anim_duration = (m_barge_anim == 3) ? kMeleeKickObjectLength : kBargeInLeftLength;
    return true;
}

void ParkourController::update_barge(const InputFrame& input, float dt, LevelScene& scene) {
    const float prev_pos = m_barge_anim_pos;
    m_barge_anim_pos += dt * m_barge_anim_rate;
    m_barge_anim_elapsed += dt;
    const Vec3 start = m_telemetry.position;

    if (m_barge_precise) {
        // TdMove precise location: straight at the target at BargeSpeed whatever the input.
        const Vec3 to = horiz(m_barge_target - start);
        const float d = to.length();
        if (d <= 1.0f) {
            m_barge_precise = false;
        } else {
            const float v = std::min(m_barge_speed, d / dt);
            m_telemetry.velocity.x = to.x / d * v;
            m_telemetry.velocity.y = to.y / d * v;
        }
    }
    if (!m_barge_precise) {
        // The rest of the move walks on the controller's input (none during the kick's first 0.6 s).
        float speed_mod = 1.0f;
        if (m_telemetry.weapon.equipped) speed_mod *= m_telemetry.weapon.mobility_scale;
        const Vec3 accel =
            (m_ignore_move_input > 0.0f) ? Vec3(0.0f, 0.0f, 0.0f) : controller_acceleration(input, false);
        calc_velocity(accel, dt, speed_mod, m_config.ground_friction);
    }
    walk_move(horiz(m_telemetry.velocity) * dt, kPawnHeight, scene);

    // Sound notifies of the playing animation crossed this tick.
    const AnimSoundNotify* notifies = nullptr;
    size_t notify_count = 0;
    float length = kBargeInLeftLength;
    switch (m_barge_anim) {
        case 1:
            notifies = kBargeInLeftNotifies;
            notify_count = std::size(kBargeInLeftNotifies);
            length = kBargeInLeftLength;
            break;
        case 2:
            notifies = kBargeOutLeftNotifies;
            notify_count = std::size(kBargeOutLeftNotifies);
            length = kBargeOutLeftLength;
            break;
        default:
            notifies = kMeleeKickObjectNotifies;
            notify_count = std::size(kMeleeKickObjectNotifies);
            length = kMeleeKickObjectLength;
            break;
    }
    for (size_t i = 0; i < notify_count; ++i) {
        if (notifies[i].time > prev_pos && notifies[i].time <= m_barge_anim_pos) {
            emit_sound(notifies[i].cue, m_telemetry.position, true);
        }
    }

    if (m_barge_anim == 1 && (m_walk_blocked || m_barge_dealt_damage)) {
        // HitWall / Bump -> TryGiveBargeDamage (shoulder barge, once): open the door immediately
        // on contact so Faith powers cleanly through the leaf, and transition from BargeInLeft to
        // BargeOutLeft once at least 0.12 s of the shoulder wind-up has played.
        if (!m_barge_dealt_damage) {
            m_barge_dealt_damage = true;
            m_barge_precise = false;
            const Vec3 moved = horiz(m_telemetry.position - start);
            const float retain = std::max(moved.length() / dt, m_barge_speed * 0.72f);
            m_telemetry.velocity.x = m_barge_dir.x * retain;
            m_telemetry.velocity.y = m_barge_dir.y * retain;
            open_barge_door(m_barge_door, m_barge_dir, true, scene);
        }
        if (m_barge_anim_pos >= 0.12f) {
            m_barge_anim = 2;
            m_barge_anim_pos = 0.0f;
            m_barge_anim_elapsed = 0.0f;
            m_barge_anim_rate = 1.0f;
            set_move_anim("BargeOutLeft");  // PlayMoveAnim(CNT_UpperBody, .., 1.0, 0.0, 0.2)
            length = kBargeOutLeftLength;
        }
    } else if (m_barge_anim == 3 && !m_barge_dealt_damage && prev_pos < kBargeKickHitTime &&
               m_barge_anim_pos >= kBargeKickHitTime) {
        // The kick's BargeHitNotify: HitObject on the door.
        m_barge_dealt_damage = true;
        open_barge_door(m_barge_door, m_barge_dir, true, scene);
    }

    // OnCustomAnimEnd: StopCustomAnim, SetMove(MOVE_Walking).
    if (m_barge_anim_pos >= length) {
        m_telemetry.move_state = EMovement::MOVE_Walking;
        m_state_timer = 0.0f;
        m_barge_anim = 0;
        m_barge_precise = false;
        m_telemetry.barge_anim = 0;
        m_telemetry.barge_anim_pos = 0.0f;
        m_telemetry.barge_anim_weight = 0.0f;
        return;
    }

    // The custom animation slot's weight (PlayMoveAnim blend times): BargeInLeft blends in over
    // 0.2 s; BargeOutLeft starts at full weight and blends out over its last 0.2 s; MeleeKickObject
    // blends in and out over 0.1 s.
    float weight = 1.0f;
    if (m_barge_anim == 1) {
        weight = std::min(1.0f, m_barge_anim_elapsed / 0.2f);
    } else if (m_barge_anim == 2) {
        weight = std::clamp((length - m_barge_anim_pos) / 0.2f, 0.0f, 1.0f);
    } else {
        weight = std::clamp(std::min(m_barge_anim_elapsed / 0.1f, (length - m_barge_anim_pos) / 0.1f), 0.0f, 1.0f);
    }
    m_telemetry.barge_anim = m_barge_anim;
    m_telemetry.barge_anim_pos = m_barge_anim_pos;
    m_telemetry.barge_anim_weight = weight;
    m_telemetry.combat_anim_time = m_barge_anim_pos;
    m_telemetry.combat_anim_duration = length;
}

void ParkourController::open_barge_door(int door_index, const Vec3& push_dir, bool barged, LevelScene& scene) {
    if (door_index < 0 || static_cast<size_t>(door_index) >= scene.barge_doors.size()) return;
    BargeDoorInstance& door = scene.barge_doors[static_cast<size_t>(door_index)];
    // SeqEvent_TakeDamage's ReTriggerDelay (4.4 s) outlasts the whole open / wait / close cycle.
    if (door.state != DoorState::Closed) return;
    // The leaf swings away from the player: a positive yaw moves its middle along tangent_pos.
    Vec3 arm(door.center_pos.x - door.hinge_pos.x, door.center_pos.y - door.hinge_pos.y, 0.0f);
    if (arm.length_sq() < 1e-3f) arm = Vec3(1.0f, 0.0f, 0.0f);
    const Vec3 tangent_pos(-arm.y, arm.x, 0.0f);
    door.swing_sign = (push_dir.dot(tangent_pos) >= 0.0f) ? 1.0f : -1.0f;
    door.state = DoorState::Opening;
    door.anim_time = 0.0f;
    door.hold_timer = 0.0f;
    door.barged = barged;
    // SeqEvent_TakeDamage -> TdPlaySound Door_Barge on the leaf; the open matinee's sound track plays
    // Door_Hit at its start. Both play at the leaf actor's location (its hinge).
    if (barged) emit_sound("Doors.Door_Barge", door.hinge_pos, false);
    emit_sound("Doors.Door_Hit", door.hinge_pos, false);
}

bool ParkourController::door_encroaches_pawn(const BargeDoorInstance& door, float angle_rad, bool with_blockers) const {
    // The parts' collision is stored at the closed pose, so the pawn is turned back about the hinge
    // instead (a vertical cylinder keeps its box under a yaw), shrunk 1 uu so resting against the
    // leaf does not count.
    const Vec3 extent(kPawnRadius - 1.0f, kPawnRadius - 1.0f, 0.5f * kPawnHeight - 1.0f);
    const Vec3 centre = m_telemetry.position + Vec3(0.0f, 0.0f, 0.5f * kPawnHeight);
    const float c = std::cos(-angle_rad);
    const float s = std::sin(-angle_rad);
    const Vec3 r = centre - door.hinge_pos;
    const Vec3 local = door.hinge_pos + Vec3(c * r.x - s * r.y, s * r.x + c * r.y, r.z);
    for (const auto& part : door.parts) {
        if (!part.collision || (part.is_blocker_only && !with_blockers)) continue;
        if (part.collision->overlap_box(local, extent, COLL_BlockNonZeroExtent)) return true;
    }
    return false;
}

void ParkourController::update_barge_doors(const InputFrame& input, float dt, LevelScene& scene) {
    if (scene.barge_doors.empty()) return;

    // Melee at a closed door in reach barges it (TdMove_Barge) when standing; crouched / sliding melee
    // is handled by TdMove_MeleeSlide / TdMove_MeleeCrouch.
    if (input.melee && !input.crouch && !m_crouch_pressed && m_telemetry.move_state != EMovement::MOVE_Barge) {
        try_initiate_barge(scene);
    }

    // Use next to a closed door swings it open the same way, without the barge.
    if (input.use && m_telemetry.move_state != EMovement::MOVE_Barge) {
        const Vec3 fwd = facing_forward();
        const Vec3 vel_2d(m_telemetry.velocity.x, m_telemetry.velocity.y, 0.0f);
        const Vec3 push_dir = (vel_2d.length_sq() > 80.0f * 80.0f) ? vel_2d.normalized() : fwd;
        for (size_t i = 0; i < scene.barge_doors.size(); ++i) {
            const BargeDoorInstance& door = scene.barge_doors[i];
            if (door.state != DoorState::Closed) continue;
            const float pz = m_telemetry.position.z;
            if (pz + kPawnHeight < door.closed_bounds.min_pt.z - 20.0f || pz > door.closed_bounds.max_pt.z + 20.0f) continue;
            const float cx = std::clamp(m_telemetry.position.x, door.closed_bounds.min_pt.x, door.closed_bounds.max_pt.x);
            const float cy = std::clamp(m_telemetry.position.y, door.closed_bounds.min_pt.y, door.closed_bounds.max_pt.y);
            const float dx = cx - m_telemetry.position.x;
            const float dy = cy - m_telemetry.position.y;
            Vec3 to_door(door.center_pos.x - m_telemetry.position.x, door.center_pos.y - m_telemetry.position.y, 0.0f);
            to_door = (to_door.length_sq() > 1e-4f) ? to_door.normalized() : fwd;
            if (std::sqrt(dx * dx + dy * dy) <= 165.0f && fwd.dot(to_door) >= 0.0f) {
                open_barge_door(static_cast<int>(i), push_dir, false, scene);
            }
        }
    }

    const float open_deg = -eval_matinee_track(kDoorOpenTrack, kDoorMatineeLength);
    for (BargeDoorInstance& door : scene.barge_doors) {
        switch (door.state) {
            case DoorState::Closed:
                break;
            case DoorState::Opening:
                // The open matinee (relative to the closed pose).
                door.anim_time = std::min(door.anim_time + dt, kDoorMatineeLength);
                door.open_angle_rad = door.swing_sign * -eval_matinee_track(kDoorOpenTrack, door.anim_time) * DEG2RAD;
                if (door.anim_time >= kDoorMatineeLength) {
                    door.state = DoorState::Open;
                    door.hold_timer = kDoorOpenDelay;
                }
                break;
            case DoorState::Open:
                // Delay, then the close matinee (its sound track starts with hatch.Squek).
                door.hold_timer -= dt;
                if (door.hold_timer <= 0.0f) {
                    door.state = DoorState::Closing;
                    door.anim_time = 0.0f;
                    emit_sound("hatch.Squek", door.hinge_pos, false);
                }
                break;
            case DoorState::Closing: {
                // The close matinee (relative to the open pose). InterpActor bStopOnEncroach: the leaf
                // holds still rather than swing into the player.
                const float t = std::min(door.anim_time + dt, kDoorMatineeLength);
                const float angle = door.swing_sign * (open_deg - eval_matinee_track(kDoorCloseTrack, t)) * DEG2RAD;
                if (door_encroaches_pawn(door, angle, false)) break;
                if (door.anim_time < kDoorCloseHitTime && t >= kDoorCloseHitTime) {
                    emit_sound("Doors.Door_Hit", door.hinge_pos, false);
                }
                door.anim_time = t;
                door.open_angle_rad = angle;
                // ChangeCollision: the doorway slab collides again once the pawn is out of it.
                if (t >= kDoorMatineeLength && !door_encroaches_pawn(door, 0.0f, true)) {
                    door.state = DoorState::Closed;
                    door.open_angle_rad = 0.0f;
                    door.anim_time = 0.0f;
                    door.barged = false;
                }
                break;
            }
        }

        // Update world-space hinge rotation matrix: M = T(hinge) * Rz(open_angle_rad) * T(-hinge)
        if (std::abs(door.open_angle_rad) > 1e-5f) {
            const float c = std::cos(door.open_angle_rad);
            const float s = std::sin(door.open_angle_rad);
            const float hx = door.hinge_pos.x;
            const float hy = door.hinge_pos.y;
            Mat4 m = Mat4::identity();
            m.m[0]  = c;
            m.m[1]  = s;
            m.m[4]  = -s;
            m.m[5]  = c;
            m.m[12] = hx - c * hx + s * hy;
            m.m[13] = hy - s * hx - c * hy;
            door.model_matrix = m;
        } else {
            door.model_matrix = Mat4::identity();
        }
    }
}

} // namespace me

