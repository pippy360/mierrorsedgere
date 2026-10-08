#include "fp_director.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace me::fp {

namespace {

// Default__TdMove_Jump.
constexpr float kJumpBlendInTime = 0.1f;
constexpr float kJumpBlendOutTime = 0.2f;
constexpr float kLongJumpNormalThreshold = 500.0f;
// Default__TdMove_Landing.
constexpr float kSoftLandingHeight = 300.0f;
constexpr float kHardLandingHeight = 530.0f;

// Moves she is off the ground in: leaving one for the ground is a landing.
bool is_airborne(EMovement m) {
    switch (m) {
        case EMovement::MOVE_Falling:
        case EMovement::MOVE_Jump:
        case EMovement::MOVE_WallRunJump:
        case EMovement::MOVE_GrabJump:
        case EMovement::MOVE_DodgeJump:
        case EMovement::MOVE_WallRunDodgeJump:
        case EMovement::MOVE_WallClimbDodgeJump:
        case EMovement::MOVE_WallClimb180TurnJump:
        case EMovement::MOVE_180TurnInAir:
        case EMovement::MOVE_Coil:
        case EMovement::MOVE_MeleeAir:
        case EMovement::MOVE_SwingJump:
        case EMovement::MOVE_AirBarge:
        case EMovement::MOVE_FallingUncontrolled:
            return true;
        default:
            return false;
    }
}

float forward_speed(const PawnFrame& f) {
    const float yaw = f.yaw_deg * DEG2RAD;
    return std::cos(yaw) * f.velocity.x + std::sin(yaw) * f.velocity.y;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// How a move plays an animation it names: the arguments of its PlayMoveAnim / PlayCustomAnim call.
struct MoveAnim {
    const char* name;  // lower case
    Slot slot;
    float rate, blend_in, blend_out;
    bool looping;
};

// Every animation a player move plays whose choice depends on what the move found in the level
// (which vault, which ledge, which rung), with the arguments its script gives. The controller
// names the animation (PawnFrame::move_anim); how it is played is here.
const MoveAnim kMoveAnims[] = {
    // TdMove_SpeedVault / TdMove_VaultOver: VaultTypes[].AnimName, PlayMoveAnim(FullBody, AnimToPlay, 1.0, 0.15, 0.2).
    {"autostepuprightleg", Slot::FullBody, 1.0f, 0.15f, 0.2f, false},
    {"stepuprightleg88", Slot::FullBody, 1.0f, 0.15f, 0.2f, false},
    {"vaultonto", Slot::FullBody, 1.0f, 0.15f, 0.2f, false},
    {"vaultover", Slot::FullBody, 1.0f, 0.15f, 0.2f, false},
    {"vaultoverhigh", Slot::FullBody, 1.0f, 0.15f, 0.2f, false},
    {"vaultontohigh", Slot::FullBody, 1.0f, 0.15f, 0.2f, false},
    // TdMove_WallRun.ReachedWall, when the camera takes the hit; TdMove_WallrunJump.StartMove, pushing off hard.
    {"wallrunimpactright", Slot::Camera, 1.0f, 0.15f, 0.15f, false},
    {"wallrunimpactleft", Slot::Camera, 1.0f, 0.15f, 0.15f, false},
    {"wallrunjumpleft", Slot::FullBodyDir, 1.0f, 0.2f, 0.2f, false},
    {"wallrunjumpright", Slot::FullBodyDir, 1.0f, 0.2f, 0.2f, false},
    // TdMove_StepUp.ReachedPreciseLocation.
    {"stepupleftleg48", Slot::FullBody, 1.0f, 0.15f, 0.1f, false},
    {"stepuprightleg48", Slot::FullBody, 1.0f, 0.15f, 0.1f, false},
    {"stepupleftleg88", Slot::FullBody, 1.2f, 0.15f, 0.1f, false},
    {"stepup144", Slot::FullBody, 1.0f, 0.15f, 0.1f, false},
    // TdMove_GrabPullUp.StartMove.
    {"hangfoldedheaveup", Slot::FullBody, 1.0f, 0.1f, 0.2f, false},
    {"hangfreeheaveup", Slot::FullBody, 1.0f, 0.1f, 0.2f, false},
    {"hangfreeheaveuptocrouch", Slot::FullBody, 1.0f, 0.1f, 0.2f, false},
    {"hangheaveup", Slot::FullBody, 1.0f, 0.1f, 0.2f, false},
    {"hangheaveuptocrouch", Slot::FullBody, 1.0f, 0.1f, 0.2f, false},
    {"hangheaveover", Slot::FullBody, 1.0f, 0.2f, 0.2f, false},
    {"hangfreeheaveover", Slot::FullBody, 1.0f, 0.2f, 0.2f, false},
    // TdMove_IntoGrab.ReachedPreciseLocation.
    {"hanghardstartvertical", Slot::FullBody, 1.0f, 0.2f, 0.2f, false},
    {"hangfreehardstart", Slot::FullBody, 1.0f, 0.1f, 0.2f, false},
    {"hanghardstart3", Slot::FullBody, 1.0f, 0.1f, 0.2f, false},
    {"hanghardstart2", Slot::FullBody, 1.0f, 0.1f, 0.2f, false},
    {"hanghardstart", Slot::FullBody, 1.0f, 0.1f, 0.2f, false},
    // TdMove_Grab.
    {"hangfoldedstart", Slot::FullBody, 1.0f, 0.2f, 0.0f, false},
    {"hangend", Slot::FullBody, 1.0f, 0.1f, 0.2f, false},
    {"hangfreeend", Slot::FullBody, 1.0f, 0.1f, 0.2f, false},
    {"hangfreefoldedendhangfree", Slot::FullBody, 1.0f, 0.0f, 0.2f, false},
    {"hangfoldedendhang", Slot::FullBody, 1.0f, 0.0f, 0.2f, false},
    // TdMove_GrabTransfer (the same turns TdMove_Grab plays by its own rules while hanging).
    {"hangturnrightstart", Slot::FullBody, 1.0f, 0.2f, 0.1f, false},
    {"hangturnleftstart", Slot::FullBody, 1.0f, 0.2f, 0.1f, false},
    {"hangfreetransferup", Slot::FullBody, 1.0f, 0.1f, 0.1f, false},
    {"hangtransferup", Slot::FullBody, 1.0f, 0.1f, 0.1f, false},
    {"hangturnjump", Slot::FullBody, 1.0f, 0.2f, 0.2f, false},
    // TdMove_Landing.
    {"fallinglandhard", Slot::FullBody, 1.0f, 0.05f, 0.2f, false},
    {"fallinglandhard2", Slot::FullBody, 1.0f, 0.05f, 0.2f, false},
    {"fallinglandsoftlanding", Slot::FullBody, 1.0f, 0.1f, 0.1f, false},
    {"jumpturnlanding", Slot::FullBody, 1.0f, 0.1f, 0.1f, false},
    // TdMove_Swing.
    {"swinghardstart", Slot::FullBody, 1.0f, 0.15f, 0.2f, false},
    {"swinghardstartwide", Slot::FullBody, 1.0f, 0.15f, 0.2f, false},
    {"swing180", Slot::FullBody, 1.0f, 0.2f, 0.3f, false},
    {"swingjumpoff", Slot::FullBody, 1.0f, 0.2f, 0.2f, false},
    {"swingend", Slot::FullBody, 1.0f, 0.2f, 0.2f, false},
    {"swingoff", Slot::FullBody, 1.0f, 0.2f, 0.2f, false},
    // TdMove_IntoClimb.PlayStartAnimation and TdMove_Climb.
    {"pipeclimbhangstartleft", Slot::FullBody, 1.0f, 0.15f, 0.25f, false},
    {"ladderclimbhangstartleft", Slot::FullBody, 1.0f, 0.15f, 0.25f, false},
    {"pipeclimbhangstartright", Slot::FullBody, 1.0f, 0.15f, 0.25f, false},
    {"ladderclimbhangstartright", Slot::FullBody, 1.0f, 0.15f, 0.25f, false},
    {"pipeclimbhangstarthard", Slot::FullBody, 1.0f, 0.1f, 0.25f, false},
    {"ladderclimbhangstarthard", Slot::FullBody, 1.0f, 0.1f, 0.25f, false},
    {"pipeclimbhangstart", Slot::FullBody, 1.0f, 0.1f, 0.25f, false},
    {"ladderclimbhangstart", Slot::FullBody, 1.0f, 0.1f, 0.25f, false},
    {"pipeclimbstart", Slot::FullBody, 1.0f, 0.15f, 0.25f, false},
    {"ladderentertop", Slot::FullBody, 1.0f, 0.25f, 0.1f, false},
    {"pipeexitbottom", Slot::FullBody, 1.0f, 0.1f, 0.4f, false},
    {"pipeexittoprighthand", Slot::FullBody, 1.0f, 0.1f, 0.1f, false},
    {"pipeexittoplefthand", Slot::FullBody, 1.0f, 0.1f, 0.1f, false},
    {"ladderexittoprighthand", Slot::FullBody, 1.0f, 0.1f, 0.1f, false},
    {"ladderexittoplefthand", Slot::FullBody, 1.0f, 0.1f, 0.1f, false},
    // TdMove_ZipLine.
    {"ziplinestart", Slot::FullBody, 1.0f, 0.2f, 0.4f, false},
    {"ziplinehitwall", Slot::FullBody, 1.0f, 0.1f, 0.2f, false},
    {"ziplineintohitwall", Slot::FullBody, 1.0f, 0.3f, 0.2f, true},
    // TdMove_Barge and TdMove_AirBarge.
    {"bargeinleft", Slot::UpperBody, 1.0f, 0.2f, 0.0f, false},
    {"meleekickobject", Slot::FullBody, 1.0f, 0.1f, 0.1f, false},
    {"bargeoutleft", Slot::UpperBody, 1.0f, 0.0f, 0.2f, false},
    {"airbargeidle", Slot::FullBody, 1.0f, 0.15f, 0.15f, false},
    {"meleeinair", Slot::FullBody, 1.0f, 0.15f, 0.15f, false},
    {"airbargeimpact", Slot::FullBody, 1.0f, 0.0f, 0.1f, false},
    {"airbargeland", Slot::FullBody, 1.0f, 0.1f, 0.1f, false},
    // TdMove_Balance, TdMove_LedgeWalk, TdMove_LayOnGround.
    {"walkbalancefalloffleft", Slot::FullBody, 1.0f, 0.3f, 0.3f, false},
    {"walkbalancefalloffright", Slot::FullBody, 1.0f, 0.3f, 0.3f, false},
    {"ledgeinto", Slot::FullBody, 1.0f, 0.2f, 0.3f, false},
    {"jumpturnlandingstand", Slot::FullBody, 1.0f, 0.2f, 0.2f, false},
    {"evaderoll", Slot::FullBody, 1.0f, 0.2f, 0.2f, false},
    // TdMove_Melee and its kin (the upper-body swings run at 1.5).
    {"meleestartleft", Slot::UpperBody, 1.5f, 0.1f, -1.0f, false},
    {"meleestartright", Slot::UpperBody, 1.5f, 0.1f, -1.0f, false},
    {"meleemissedleft", Slot::UpperBody, 1.5f, 0.1f, 0.3f, false},
    {"meleemissedright", Slot::UpperBody, 1.5f, 0.1f, 0.3f, false},
    {"meleehitleft", Slot::UpperBody, 1.5f, 0.2f, 0.1f, false},
    {"meleehitright", Slot::UpperBody, 1.5f, 0.2f, 0.1f, false},
    {"meleeinairstill", Slot::FullBody, 1.0f, 0.1f, 0.2f, false},
    {"meleefromabove", Slot::FullBody, 1.0f, 0.1f, 0.1f, false},
    {"meleeinairhit", Slot::FullBody, 1.0f, 0.1f, 0.2f, false},
    {"meleeslide", Slot::FullBody, 1.0f, 0.1f, 0.1f, false},
    {"meleeslidehard", Slot::FullBody, 1.0f, 0.1f, 0.1f, false},
    {"meleewallrunleft", Slot::FullBody, 1.0f, 0.1f, 0.2f, false},
    {"meleewallrunright", Slot::FullBody, 1.0f, 0.1f, 0.2f, false},
    {"meleevaultover", Slot::FullBody, 1.0f, 0.1f, 0.1f, false},
    {"snatchfail", Slot::FullBody, 1.0f, 0.1f, 0.4f, false},
};

const MoveAnim* find_move_anim(const std::string& name) {
    const std::string key = lower(name);
    for (const MoveAnim& a : kMoveAnims) {
        if (key == a.name) return &a;
    }
    return nullptr;
}

}  // namespace

bool Director::init(const std::string& game_root, AnimTree::SequenceLookup lookup, std::string& error) {
    tree_.set_sequence_lookup(std::move(lookup));
    if (!tree_.load(game_root, error)) return false;
    reset();
    return true;
}

void Director::reset() {
    tree_.reset();
    pawn_ = PawnAnimState{};
    started_ = false;
    pending_animation_state_ = EMovement::MOVE_None;
    animation_state_timer_ = -1.0f;
    last_velocity_ = Vec3(0.0f, 0.0f, 0.0f);
    last_move_anim_.clear();
    close_to_ground_ = false;
    time_in_move_ = 0.0f;
    was_accelerating_ = false;
    airborne_ = false;
    fall_top_ = 0.0f;
    was_armed_ = false;
    climb_left_hand_ = false;
    climb_step_time_ = -1.0f;
    grab_turn_ = 0;
    grab_timer_ = -1.0f;
    grab_free_turn_ = false;
    shimmy_ = false;
    melee_phase_ = 0;
    root_offset_ = root_target_ = Vec3(0.0f, 0.0f, 0.0f);
    root_blend_ = 0.0f;
    root_timer_ = -1.0f;
    swing_strength_ = swing_target_ = 0.0f;
    swing_blend_ = 0.0f;
    swan_forward_ = swan_down_ = 0.0f;
    hips_offset_ = Vec3(0.0f, 0.0f, 0.0f);
    slide_ended_ = 1.0f;
}

// The weapon in hand: TdPawn.SetArmed / PlayWeaponDeploy, PlayFireAnimation, UpdateWeaponAnimState.
// Taken up or fired it is at the ready (the weapon arm on the ready stances); a light one is let
// down again (relaxed: the arm goes with the run, in the armed sets' versions of it) once 5 s have
// passed and she has moved 1000 uu.
void Director::tick_weapon(const PawnFrame& frame) {
    if (!frame.armed) {
        was_armed_ = false;
        pawn_.weapon_state = 0;
        pawn_.armed_right = pawn_.armed_left = 0.0f;
        return;
    }
    const bool light = !frame.heavy_weapon;
    auto make_ready = [&]() {
        pawn_.weapon_state = 2;
        ready_for_ = 0.0f;
        amount_til_unarmed_ = light ? 1000.0f : 0.0f;
    };
    if (!was_armed_) {
        was_armed_ = true;
        tree_.play_custom_anim(Slot::CannedUpperBody, "unholster", 1.0f, 0.0f, 0.2f, false, true);
        make_ready();
    }
    if (frame.fired) {
        // PlayCustomAnim(CNT_Weapon, 'standfire', 1.0, 0.1, 0.0). (The script gives no blend out and
        // marks the node as a firing animation for native code to clear; here it goes back to the
        // ready stance over 0.2 s as it ends.)
        tree_.stop_custom_anim(Slot::Weapon, 0.0f);
        tree_.play_custom_anim(Slot::Weapon, "standfire", 1.0f, 0.1f, 0.2f, false, true);
        make_ready();
    }
    ready_for_ += frame.dt;
    amount_til_unarmed_ -= frame.velocity.length() * frame.dt;
    if (pawn_.weapon_state == 2 && amount_til_unarmed_ <= 0.0f && ready_for_ > (light ? 5.0f : 1.0f) && light) pawn_.weapon_state = 1;
    pawn_.armed_right = 1.0f;
    pawn_.armed_left = light ? 0.0f : 1.0f;
}

// TdSwanNeck (script): looking down past the move's SwanNeckEnableAtPitch the camera cranes forward
// and down off the eye, so she sees her feet and not her chest. With P the pitch below level in
// Unreal units, thr = int(Start * 182.044) and t = (P - thr) / (17385 - thr):
//     forward = SwanNeckForward * t * cos(t * pi / 4),  down = SwanNeckDown * t * sin(t * pi / 4)
// and the neck follows that, a fraction dt / 0.07 of the way each tick. TdMove.StartMove sets the
// move's constants (15, 35, 30 unless it has its own), StopMove puts the defaults back.
// TdPlayerPawn.SetHipsOffset moves the hips and legs (not the eye): running, back by four times
// the neck's reach up to 20 (TdMove_Walking.UpdateViewRotation); crouched, with the turn of the
// view off the legs (TdMove_Crouch.UpdateViewRotation).
void Director::tick_swan_neck(const PawnFrame& frame) {
    float start = 15.0f, forward = 35.0f, down = 30.0f;
    switch (frame.movement) {
        case EMovement::MOVE_Grabbing:
            start = 0.0f;
            forward = 70.0f;
            break;
        case EMovement::MOVE_Climb:
            forward = 40.0f;
            break;
        case EMovement::MOVE_LayOnGround:
        case EMovement::MOVE_180TurnInAir:
            start = forward = down = 0.0f;
            break;
        case EMovement::MOVE_SkillRoll:
            if (time_in_move_ >= 0.2f) start = forward = down = 0.0f;  // the DisableSwanneck timer
            break;
        default:
            break;
    }
    const float pitch_units = -frame.view_pitch_deg * (65536.0f / 360.0f);
    const float threshold = std::floor(start * 182.044f);
    float want_forward = 0.0f, want_down = 0.0f;
    if (pitch_units > threshold && (forward != 0.0f || down != 0.0f)) {
        const float t = (pitch_units - threshold) / (17385.0f - threshold);
        want_forward = forward * t * std::cos(t * PI * 0.25f);
        want_down = down * t * std::sin(t * PI * 0.25f);
    }
    const float step = std::min(1.0f, frame.dt / 0.07f);
    swan_forward_ += (want_forward - swan_forward_) * step;
    swan_down_ += (want_down - swan_down_) * step;

    slide_ended_ += frame.dt;
    Vec3 hips(0.0f, 0.0f, 0.0f);
    if (frame.movement == EMovement::MOVE_Walking && pawn_.walking_state > kWasWalk) {
        hips.x = -std::min(swan_forward_, 5.0f) * 4.0f;
    } else if (frame.movement == EMovement::MOVE_Crouch && slide_ended_ > 0.3f) {
        float turn = frame.view_yaw_deg - tree_.leg_yaw();
        while (turn > 180.0f) turn -= 360.0f;
        while (turn < -180.0f) turn += 360.0f;
        hips.x = 25.0f * std::fabs(turn) / 90.0f;
        hips.y = 30.0f * turn / 90.0f;
    }
    // SetHipsOffset(Offset, 0.3).
    const float blend = std::min(1.0f, frame.dt / 0.1f);
    hips_offset_ += (hips - hips_offset_) * blend;
}

void Director::set_root_offset(const Vec3& offset, float blend_time) {
    root_target_ = offset;
    root_blend_ = blend_time;
    if (blend_time <= 0.0f) root_offset_ = offset;
}

void Director::apply_mesh_transform(ViewFrame& view) const {
    // The root offset is in the root bone's space, so it turns with the body.
    Vec3 eye = view.eye_pawn + root_offset_;
    const float angle = swing_angle_ * swing_strength_;
    if (angle != 0.0f) {
        // SetPawnRotation: the body turns by the swing's angle about a point 94 above the mesh's
        // origin (the capsule's centre), feet forward when she is ahead of the bar.
        constexpr float kPivot = 94.0f;
        const float c = std::cos(angle), sn = std::sin(angle);
        const float x = eye.x, z = eye.z - kPivot;
        eye.x = x * c - z * sn;
        eye.z = kPivot + x * sn + z * c;
        view.anim_pitch += angle * RAD2DEG;
    }
    view.eye_pawn = eye;
}

// TdMove_Climb.HandleClimbAction / Climb / OnTimer: one step at a time, hand over hand. A pipe is
// climbed two steps to the animation (the "fast" ones), a ladder one; down is the same animation
// backwards with the other hand's. A step is 32 uu, and the move flies it at 64 (pipe) or 96
// (ladder) uu/s a step, so each animation lasts as long as its step.
void Director::tick_climb(const PawnFrame& frame) {
    if (climb_step_time_ >= 0.0f) {
        climb_step_time_ += frame.dt;
        if (!climb_hand_switched_ && climb_step_time_ >= 0.1f) {
            climb_hand_switched_ = true;
            climb_left_hand_ = !climb_left_hand_;
        }
        if (climb_step_time_ >= climb_step_length_) climb_step_time_ = -1.0f;
    }
    // Going down is the slide (bClimbDownFast: the stick held right down, which a key always is);
    // the tree's Climb node shows it. Stepping down, the up animations backwards, is not played.
    pawn_.climb_sliding = frame.velocity.z < -1.0f;
    if (climb_step_time_ < 0.0f && !climb_exiting_ && frame.velocity.z > 20.0f) {
        // ClimbAnims[bClimbLeftHand ? right : left].
        const bool right_hand = climb_left_hand_;
        const char* name = frame.climbing_pipe ? (right_hand ? "PipeClimbUpFastRightHand" : "PipeClimbUpFastLeftHand")
                                               : (right_hand ? "LadderClimbUpRightHand" : "LadderClimbUpLeftHand");
        play(Slot::FullBody, name, 1.0f, 0.1f, 0.075f);
        climb_step_time_ = 0.0f;
        climb_step_length_ = frame.climbing_pipe ? 0.5f : 1.0f / 3.0f;
        climb_hand_switched_ = false;
    }
    pawn_.climb_hand = climb_left_hand_ ? 1 : 0;
}

// TdMove_Grab: the shimmy (StartShimmy / AbortShimmy) and looking back over a shoulder
// (UpdateViewRotation, OnTimer, OnCustomAnimEnd).
void Director::tick_grab(const PawnFrame& frame) {
    const float yaw = frame.yaw_deg * DEG2RAD;
    const float side = -std::sin(yaw) * frame.velocity.x + std::cos(yaw) * frame.velocity.y;
    std::string playing;
    const bool custom = tree_.custom_anim_playing(Slot::FullBody, &playing);

    // One strafe animation a step, started again from its first frame while she keeps going.
    if (shimmy_ && !custom) shimmy_ = false;
    if (!shimmy_ && std::fabs(side) > 5.0f && grab_turn_ == 0) {
        const bool left = side < 0.0f;
        play(Slot::FullBody, frame.hanging_free ? (left ? "HangFreeStrafeLeft" : "HangFreeStrafe") : (left ? "HangStrafeLeft" : "HangStrafeRight"),
             1.0f, 0.2f, 0.0f);
        shimmy_ = true;
    } else if (shimmy_ && std::fabs(side) <= 5.0f) {
        // Let go of the stick: it stops only early in the step or near its end.
        const float at = tree_.custom_anim_time(Slot::FullBody) / 1.0667f;
        if ((at > 0.1f && at < 0.25f) || at > 0.8f) {
            tree_.stop_custom_anim(Slot::FullBody, 0.4f);
            shimmy_ = false;
        }
    }

    float turn = frame.view_yaw_deg - frame.yaw_deg;
    while (turn > 180.0f) turn -= 360.0f;
    while (turn < -180.0f) turn += 360.0f;
    const bool forward = turn > -90.0f && turn < 90.0f;
    const bool right = turn >= 0.0f;
    if (grab_timer_ >= 0.0f) {
        grab_timer_ -= frame.dt;
        if (grab_timer_ < 0.0f) {
            if (grab_turn_ == 2) grab_turn_ = 0;
            else if (grab_turn_ == 1) grab_turn_ = 3;
        }
    }
    if (!frame.hanging_free) {
        if (grab_turn_ == 0 && !forward) {
            play(Slot::FullBody, right ? "HangTurnRightStart" : "HangTurnLeftStart", 1.0f, 0.2f, 0.2f);
            grab_turn_ = 1;
            grab_timer_ = 0.2f;
            grab_turned_right_ = right;
        } else if (grab_turn_ == 3 && forward) {
            play(Slot::FullBody, right ? "HangTurnRightEnd" : "HangTurnLeftEnd", 1.0f, 0.2f, 0.2f);
            grab_turn_ = 2;
            grab_timer_ = 0.6f;
        } else if (grab_turn_ == 1 && forward) {
            tree_.stop_custom_anim(Slot::FullBody, 0.2f);
            grab_turn_ = 0;
        }
    } else {
        grab_turn_ = 0;
        if (grab_free_turn_ && !custom) grab_free_turn_ = false;
        // 750 short of the look constraint, which is a quarter turn either way.
        if (!grab_free_turn_ && std::fabs(turn) >= 85.88f) {
            play(Slot::FullBody, right ? "HangFreeTurnRight" : "HangFreeTurnLeft", 1.0f, 0.2f, 0.2f);
            grab_free_turn_ = true;
        }
    }
    // OnCustomAnimEnd: the start's end makes it the idle, the end's end clears it.
    if (!tree_.custom_anim_playing(Slot::FullBody)) {
        if (grab_turn_ == 1) grab_turn_ = 3;
        else if (grab_turn_ == 2) grab_turn_ = 0;
    }
    pawn_.grab_turn_type = grab_turn_;
    pawn_.grab_turn_deg = turn;
}

// TdMove_Melee.TriggerMove, OnCustomAnimEnd, TriggerHit / TriggerMiss (and TdMove_MeleeCrouch):
// the wind-up, then the blow that lands or misses when the wind-up ends.
void Director::tick_melee(const PawnFrame& frame) {
    if (melee_phase_ != 1 || tree_.custom_anim_playing(Slot::UpperBody)) return;
    melee_phase_ = 2;
    const bool left = melee_variant_ == 1;
    if (melee_variant_ == 3) {
        play(Slot::UpperBody, "MeleeCrouchHit", 1.0f, 0.1f, 0.2f);
    } else if (melee_variant_ == 2) {
        play(Slot::UpperBody, "MeleeHitShove", 1.0f, 0.0f, 0.1f);
    } else if (frame.melee_hit) {
        play(Slot::UpperBody, left ? "MeleeHitLeft" : "MeleeHitRight", 1.5f, 0.2f, 0.1f);
    } else {
        play(Slot::UpperBody, left ? "MeleeMissedLeft" : "MeleeMissedRight", 1.5f, 0.08f, 0.1f);
    }
}

void Director::set_animation_state(EMovement state, float delay) {
    pending_animation_state_ = state;
    if (delay > 0.0f) {
        animation_state_timer_ = delay;
    } else {
        pawn_.animation_movement = state;
        animation_state_timer_ = -1.0f;
    }
}

// TdPawn.UpdateWalkingState is native. The pawn's Tick runs before its physics, so the state
// follows the velocity of the frame before. The thresholds are measured on the recordings (the
// recorder logs CurrentWalkingState): Sneak from the first movement, Walk from 50, Jog from 260,
// Run from 400, Sprint from 630, the same speeding up and slowing down.
void Director::update_walking_state(const PawnFrame& frame) {
    (void)frame;
    const float speed = std::sqrt(last_velocity_.x * last_velocity_.x + last_velocity_.y * last_velocity_.y);
    uint8_t state = kWasIdle;
    if (speed >= 630.0f) state = kWasSprint;
    else if (speed >= 400.0f) state = kWasRun;
    else if (speed >= 260.0f) state = kWasJog;
    else if (speed >= 50.0f) state = kWasWalk;
    else if (speed >= 1.0f) state = kWasSneak;
    pawn_.walking_state = state;
}

// An animation the move names, played the way its script plays it.
bool Director::play_named(const std::string& name) {
    const MoveAnim* a = find_move_anim(name);
    if (!a) return false;
    std::string playing;
    if (tree_.custom_anim_playing(a->slot, &playing) && lower(playing) == a->name && tree_.custom_anim_time(a->slot) < 0.05f) return true;
    tree_.play_custom_anim(a->slot, name, a->rate, a->blend_in, a->blend_out, a->looping, true);
    // TdMove_Climb.ExitAtTop: over the top, and the walking tree comes in under it.
    if (std::strstr(a->name, "exittop")) {
        climb_exiting_ = true;
        climb_step_time_ = -1.0f;
        set_animation_state(EMovement::MOVE_Walking, 0.5f);
    }
    // TdMove_IntoGrab.ReachedPreciseLocation: the hardest catch also knocks the camera.
    if (std::strcmp(a->name, "hanghardstart3") == 0) tree_.play_custom_anim(Slot::Camera, "gethitfront", 1.0f, 0.05f, 0.2f, false, true);
    return true;
}

// TdMove_Landing.LandNormal: the run takes the landing in its stride.
void Director::land_normal(float amount) {
    tree_.set_landed(amount);
    tree_.activate_custom_blend("LandingRun", amount, 0.6f * amount, 0.15f, 0.4f);
}

void Director::stop_move(EMovement move, EMovement pending, const PawnFrame& frame) {
    (void)frame;
    switch (move) {
        case EMovement::MOVE_Jump:
            // TdMove_Jump.StopMove: the jump animation carries on into a fall or a vault.
            if (pending != EMovement::MOVE_Falling && pending != EMovement::MOVE_VaultOver) tree_.stop_custom_anim(Slot::FullBodyDir, 0.2f);
            break;
        case EMovement::MOVE_Falling:
            if (pawn_.old_movement != EMovement::MOVE_DodgeJump) {
                tree_.stop_custom_anim(Slot::FullBody, 0.2f);
                tree_.stop_custom_anim(Slot::FullBodyDir, 0.2f);
            }
            break;
        case EMovement::MOVE_WallRunJump:
            // TdMove_WallrunJump.StopMove: like the jump, it carries on into a fall or a vault.
            if (pending != EMovement::MOVE_Falling && pending != EMovement::MOVE_VaultOver) tree_.stop_custom_anim(Slot::FullBodyDir, 0.2f);
            break;
        case EMovement::MOVE_Vertigo:
            tree_.stop_custom_anim(Slot::FullBodyDir, 0.4f);
            break;
        case EMovement::MOVE_Grabbing:
        case EMovement::MOVE_SoftLanding:
        case EMovement::MOVE_180Turn:
            tree_.stop_custom_anim(Slot::FullBody, 0.2f);
            break;
        case EMovement::MOVE_GrabJump:
            set_animation_state(EMovement::MOVE_None);
            tree_.stop_custom_anim(Slot::FullBody, 0.2f);
            break;
        case EMovement::MOVE_SpringBoarding:
            tree_.stop_custom_anim(Slot::FullBody, 0.1f);
            break;
        case EMovement::MOVE_WallClimbing:
            tree_.stop_custom_anim(Slot::FullBody, 0.25f);
            break;
        case EMovement::MOVE_Slide:
            // TdMove_Slide.StopMove: out of the slide, and into the crouch if that is what follows.
            tree_.stop_custom_anim(Slot::FullBody, 0.2f);
            if (pending == EMovement::MOVE_Crouch) play(Slot::FullBody, "CrouchSlideToCrouch", 1.0f, 0.1f, 0.2f);
            break;
        case EMovement::MOVE_Crouch:
            play(Slot::Camera, "CrouchIntoStand", 1.0f, 0.2f, 0.2f);
            break;
        case EMovement::MOVE_Coil:
            play(Slot::FullBody, "JumpCoilEnd", 1.0f, 0.25f, 0.35f);
            set_animation_state(EMovement::MOVE_None);
            break;
        case EMovement::MOVE_ZipLine:
            // TdMove_ZipLine.StopMove: letting go swings her off the line.
            tree_.stop_custom_anim(Slot::FullBody, 0.3f);
            if (pending == EMovement::MOVE_Falling || pending == EMovement::MOVE_Jump) play(Slot::FullBody, "SwingJumpOff", 1.0f, 0.2f, 0.2f);
            break;
        case EMovement::MOVE_MeleeSlide:
            tree_.stop_custom_anim(Slot::FullBody, 0.15f);
            set_animation_state(EMovement::MOVE_None);
            break;
        case EMovement::MOVE_IntoZipLine:
        case EMovement::MOVE_AirBarge:
            set_animation_state(EMovement::MOVE_None);
            break;
        case EMovement::MOVE_IntoClimb:
            set_animation_state(EMovement::MOVE_None, 0.2f);
            break;
        case EMovement::MOVE_Barge:
            tree_.stop_custom_anim(Slot::UpperBody, 0.2f);
            break;
        case EMovement::MOVE_Melee:
            // TdMove_MeleeBase.StopMove, then TdMove_Melee's (TdMove_MeleeCrouch lets its blow go quicker).
            tree_.stop_custom_anim(Slot::Canned, 0.15f);
            tree_.stop_custom_anim(Slot::Weapon, 0.15f);
            tree_.stop_custom_anim(Slot::UpperBody, melee_variant_ == 3 ? 0.2f : 0.3f);
            tree_.stop_custom_anim(Slot::FullBody, 0.15f);
            melee_phase_ = 0;
            break;
        case EMovement::MOVE_MeleeCrouch:
            tree_.stop_custom_anim(Slot::FullBody, 0.15f);
            tree_.stop_custom_anim(Slot::UpperBody, 0.2f);
            set_animation_state(EMovement::MOVE_None);
            break;
        case EMovement::MOVE_MeleeAir:
        case EMovement::MOVE_MeleeWallrun:
            tree_.stop_custom_anim(Slot::FullBody, 0.15f);
            break;
        default:
            break;
    }
}

void Director::start_move(EMovement move, EMovement old, const PawnFrame& frame) {
    switch (move) {
        case EMovement::MOVE_Jump: {
            // TdMove_Jump.StartJump.
            const float forward = forward_speed(frame);
            if (forward < 5.0f) play(Slot::FullBodyDir, "JumpStill", 1.0f, 0.15f, 0.15f);
            else if (forward < kLongJumpNormalThreshold || !frame.long_jump_over_gap) play(Slot::FullBodyDir, "JumpSlow", 1.0f, kJumpBlendInTime, kJumpBlendOutTime);
            else play(Slot::FullBodyDir, "JumpFast", 1.0f, kJumpBlendInTime, kJumpBlendOutTime);
            break;
        }
        case EMovement::MOVE_Falling:
            // TdMove_Falling.StartMove: walking off an edge.
            if (old == EMovement::MOVE_Walking) {
                if (forward_speed(frame) < 0.0f) play(Slot::FullBodyDir, "JumpStill", 1.0f, 0.3f, 0.2f);
                else play(Slot::FullBodyDir, "JumpAir", 1.0f, 0.3f, 0.2f);
            } else if (old == EMovement::MOVE_DodgeJump) {
                set_animation_state(EMovement::MOVE_DodgeJump);
            }
            break;
        case EMovement::MOVE_SpeedVaulting:
        case EMovement::MOVE_VaultOver:
            // TdMove_SpeedVault.StartMove: the vault type's animation.
            tree_.stop_custom_anim(Slot::FullBodyDir, 0.2f);
            if (frame.move_anim.empty()) play(Slot::FullBody, "VaultOver", 1.0f, 0.15f, 0.2f);
            break;
        case EMovement::MOVE_AutoStepUp:
            play(Slot::FullBody, "autostepuprightleg", 0.8f, 0.15f, 0.25f);
            break;
        case EMovement::MOVE_StepUp:
            play(Slot::FullBody, "vaultonto", 1.0f, 0.15f, 0.25f);
            break;
        case EMovement::MOVE_SpringBoarding:
            // TdMove_SpringBoard.StartMove; the step itself plays when she reaches the foot plant ("@reached").
            land_normal(0.85f);
            break;
        case EMovement::MOVE_WallRunningRight:
        case EMovement::MOVE_WallRunningLeft:
            // TdMove_WallRun.StartMove: WallrunStartUpperBodyAnimPlayRate, with the wall beside her head.
            play(Slot::FullBody, move == EMovement::MOVE_WallRunningRight ? "wallrunrightstart" : "wallrunleftstart", 0.6f, 0.2f, 0.2f);
            break;
        case EMovement::MOVE_WallRunJump:
            // TdMove_WallrunJump.StartMove: WallrunJumpLeft / Right when she pushes off hard (the
            // move names it), a plain jump otherwise.
            tree_.stop_custom_anim(Slot::FullBody, 0.2f);
            if (frame.move_anim.empty()) play(Slot::FullBodyDir, "JumpSlow", 1.0f, 0.2f, 0.2f);
            set_animation_state(EMovement::MOVE_Jump);
            break;
        case EMovement::MOVE_WallClimbing:
            tree_.play_custom_anim(Slot::FullBody, "WallRunVertical", 0.2f, 0.2f, 0.0f, true, true);
            break;
        case EMovement::MOVE_WallClimbDodgeJump:
            play(Slot::FullBody, "JumpSlow", 1.0f, kJumpBlendInTime, kJumpBlendOutTime);
            break;
        case EMovement::MOVE_WallClimb180TurnJump:
            play(Slot::FullBody, "wallrunvertical180turn", 1.0f, 0.2f, 0.1f);
            break;
        case EMovement::MOVE_WallRunDodgeJump:
        case EMovement::MOVE_DodgeJump:
            play(Slot::FullBody, frame.move_left ? "dodgejumpleft" : "dodgejumpright", 1.0f, move == EMovement::MOVE_DodgeJump ? 0.1f : 0.2f, 0.2f);
            break;
        case EMovement::MOVE_GrabPullUp:
            // TdMove_GrabPullUp.StartMove: up onto the ledge (or over it), then the walking or crouching tree.
            if (frame.move_anim.empty()) play(Slot::FullBody, frame.hanging_free ? "HangFreeHeaveUp" : "HangHeaveUp", 1.0f, 0.1f, 0.2f);
            set_animation_state(EMovement::MOVE_Grabbing);
            set_animation_state(lower(frame.move_anim).find("tocrouch") != std::string::npos ? EMovement::MOVE_Crouch : EMovement::MOVE_None, 0.2f);
            break;
        case EMovement::MOVE_GrabJump:
            play(Slot::FullBody, "HangTurnJump", 1.0f, 0.2f, 0.2f);
            set_animation_state(EMovement::MOVE_Grabbing);
            break;
        case EMovement::MOVE_GrabTransfer:
            tree_.stop_custom_anim(Slot::FullBody, 0.1f);
            set_animation_state(old);
            break;
        case EMovement::MOVE_Landing:
            // TdMove_Landing.LandHard.
            if (frame.move_anim.empty()) play(Slot::FullBody, "FallingLandHard", 1.0f, 0.05f, 0.2f);
            break;
        case EMovement::MOVE_SkillRoll:
            // PlayMoveAnim(CNT_FullBody, 'fallinglandroll', 1.0, 0.2, 0.2, bRootMotion).
            tree_.play_custom_anim(Slot::FullBody, "fallinglandroll", 1.0f, 0.2f, 0.2f, false, true, true);
            set_animation_state(EMovement::MOVE_Walking, 0.2f);
            break;
        case EMovement::MOVE_SoftLanding:
            set_animation_state(EMovement::MOVE_180TurnInAir);
            tree_.play_custom_anim(Slot::FullBody, "fallinglandintosoftlanding", 1.0f, 0.6f, 0.2f, true, true);
            break;
        case EMovement::MOVE_Swing:
            // SetRootOffset(vect(0, -50, -32), AnimBlendTime, BCS_BoneSpace): up 50 and back 32, so the
            // hands are on the bar; EnableSwingControl.
            set_root_offset(Vec3(-32.0f, 0.0f, 50.0f), 0.15f);
            swing_target_ = 1.0f;
            swing_blend_ = 0.15f;
            tree_.stop_custom_anim(Slot::FullBody, 0.15f);
            tree_.stop_custom_anim(Slot::FullBodyDir, 0.15f);
            if (frame.move_anim.empty()) play(Slot::FullBody, "SwingHardStart", 1.0f, 0.15f, 0.2f);
            break;
        case EMovement::MOVE_SwingJump:
            set_animation_state(EMovement::MOVE_Swing);
            set_animation_state(EMovement::MOVE_None, 0.3f);
            break;
        case EMovement::MOVE_IntoZipLine:
            if (frame.move_anim.empty()) play(Slot::FullBody, "ZiplineStart", 1.0f, 0.2f, 0.4f);
            set_animation_state(EMovement::MOVE_IntoZipLine, 0.2f);
            break;
        case EMovement::MOVE_Slide:
            // TdMove_Slide.StartMove.
            tree_.stop_custom_anim(Slot::UpperBody, 0.1f);
            set_animation_state(EMovement::MOVE_Walking);
            set_animation_state(EMovement::MOVE_None, 0.4f);
            play(Slot::FullBody, "CrouchSlide", 1.0f, 0.4f, 0.4f);
            break;
        case EMovement::MOVE_RumpSlide:
            set_root_offset(Vec3(0.0f, 0.0f, 20.0f), 0.3f);  // TdMove_RumpSlide.RootOffset
            set_animation_state(EMovement::MOVE_None);
            play(Slot::FullBody, "crouchslideintoend45", 1.0f, 0.15f, 0.2f);
            break;
        case EMovement::MOVE_Crouch:
            tree_.stop_custom_anim(Slot::FullBodyDir, 0.25f);
            tree_.stop_custom_anim(Slot::LowerBody, 0.25f);
            // Out of a walk the mesh is lifted 15 for a moment while the capsule drops.
            if (old == EMovement::MOVE_Walking) {
                set_root_offset(Vec3(0.0f, 0.0f, 15.0f), 0.1f);
                root_timer_ = 0.15f;
            }
            break;
        case EMovement::MOVE_180Turn: {
            // TdMove_180Turn.StartMove: the running turn, or the standing one.
            const float speed = std::sqrt(frame.velocity.x * frame.velocity.x + frame.velocity.y * frame.velocity.y);
            play(Slot::FullBody, speed > 100.0f ? "RunTurn180" : "StandTurn180Right", 1.0f, 0.2f, 0.2f);
            break;
        }
        case EMovement::MOVE_180TurnInAir:
            play(Slot::FullBody, "JumpTurnFly", 1.0f, 0.1f, 0.1f);
            break;
        case EMovement::MOVE_Coil:
            set_animation_state(EMovement::MOVE_Crouch);
            play(Slot::FullBody, "JumpCoil", 1.0f, 0.15f, 0.15f);
            break;
        case EMovement::MOVE_AirBarge:
            set_animation_state(EMovement::MOVE_Crouch);
            if (frame.move_anim.empty()) play(Slot::FullBody, "MeleeInAir", 1.0f, 0.15f, 0.15f);
            break;
        case EMovement::MOVE_Barge:
            if (frame.move_anim.empty()) play(Slot::UpperBody, "BargeInLeft", 1.0f, 0.2f, 0.0f);
            break;
        case EMovement::MOVE_Vertigo:
            play(Slot::FullBodyDir, "edgedetection", 1.0f, 0.28f, 0.28f);
            break;
        case EMovement::MOVE_LedgeWalk:
            play(Slot::FullBody, "LedgeInto", 1.0f, 0.2f, 0.3f);
            break;
        case EMovement::MOVE_Melee:
            // TdMove_Melee.TriggerMove: punches alternate hands, the third of a combo is a shove.
            tree_.stop_custom_anim(Slot::Camera, 0.05f);
            tree_.stop_custom_anim(Slot::Weapon, 0.1f);
            set_animation_state(EMovement::MOVE_Walking);
            melee_variant_ = frame.melee_variant;
            melee_phase_ = 0;
            if (melee_variant_ == 3) {
                // The port's crouched blow: TdMove_MeleeCrouch.
                set_animation_state(EMovement::MOVE_Crouch);
                tree_.play_custom_anim(Slot::UpperBody, "MeleeCrouchStart", 1.0f, 0.1f, -1.0f, false, true);
                melee_phase_ = 1;
            } else if (melee_variant_ == 2) {
                tree_.play_custom_anim(Slot::UpperBody, "MeleeStartShove", 1.0f, 0.1f, -1.0f, false, true);
                melee_phase_ = 1;
            } else if (melee_variant_ >= 0) {
                tree_.play_custom_anim(Slot::UpperBody, melee_variant_ == 1 ? "MeleeStartLeft" : "MeleeStartRight", 1.5f, 0.1f, -1.0f, false, true);
                melee_phase_ = 1;
            }
            break;
        case EMovement::MOVE_MeleeAir: {
            // TdMove_MeleeAir: the flying kick, or the kick on the spot when she is hardly moving.
            const float speed = std::sqrt(frame.velocity.x * frame.velocity.x + frame.velocity.y * frame.velocity.y);
            if (frame.move_anim.empty()) play(Slot::FullBody, speed > 200.0f ? "MeleeInAir" : "MeleeInAirStill", 1.0f, 0.1f, 0.2f);
            break;
        }
        case EMovement::MOVE_MeleeSlide:
            if (frame.move_anim.empty()) play(Slot::FullBody, "MeleeSlide", 1.0f, 0.1f, 0.1f);
            set_animation_state(EMovement::MOVE_Slide);
            break;
        case EMovement::MOVE_MeleeWallrun:
            if (frame.move_anim.empty()) play(Slot::FullBody, old == EMovement::MOVE_WallRunningLeft ? "MeleeWallRunLeft" : "MeleeWallRunRight", 1.0f, 0.1f, 0.2f);
            break;
        case EMovement::MOVE_MeleeCrouch:
            set_animation_state(EMovement::MOVE_Crouch);
            if (frame.move_anim.empty()) {
                tree_.play_custom_anim(Slot::UpperBody, "MeleeCrouchStart", 1.0f, 0.1f, -1.0f, false, true);
                melee_variant_ = 3;
                melee_phase_ = 1;
            }
            break;
        case EMovement::MOVE_IntoClimb:
            // TdMove_IntoClimb.PlayStartAnimation ends with it, whichever animation it picked.
            set_animation_state(EMovement::MOVE_Climb, 0.15f);
            break;
        case EMovement::MOVE_Climb:
            climb_left_hand_ = false;
            climb_step_time_ = -1.0f;
            climb_exiting_ = false;
            break;
        case EMovement::MOVE_Grabbing:
            // TdMove_Grab.StartMove: RootOffset.X += RelativeExtent + 1 with the legs on the wall (the
            // recordings have the eye 1 forward in 794 of 872 hanging frames), over 0.3 s.
            if (!frame.hanging_free) set_root_offset(Vec3(1.0f, 0.0f, 0.0f), 0.3f);
            grab_turn_ = 0;
            grab_timer_ = -1.0f;
            grab_free_turn_ = false;
            shimmy_ = false;
            break;
        case EMovement::MOVE_LayOnGround:
            // TdMove_Landing.LandBackwards.
            play(Slot::FullBody, "JumpTurnLanding", 1.0f, 0.1f, 0.1f);
            break;
        default:
            break;
    }
}

void Director::tick(const PawnFrame& frame) {
    if (!started_) {
        started_ = true;
        pawn_.movement = frame.movement;
        pawn_.old_movement = EMovement::MOVE_None;
    } else if (frame.movement != pawn_.movement) {
        const EMovement old = pawn_.movement;
        // TdMove.StopMove: SetRootOffset(vect(0, 0, 0), 0.3); the swing lets go quicker.
        if (old == EMovement::MOVE_Swing) {
            set_root_offset(Vec3(0.0f, 0.0f, 0.0f), 0.1f);
            swing_target_ = 0.0f;
            swing_blend_ = 0.25f;
        } else {
            set_root_offset(Vec3(0.0f, 0.0f, 0.0f), 0.3f);
        }
        root_timer_ = -1.0f;
        if (old == EMovement::MOVE_Slide) slide_ended_ = 0.0f;
        stop_move(old, frame.movement, frame);
        // TdMove.StopMove: ClearAnimationMovementState.
        pawn_.animation_movement = EMovement::MOVE_None;
        pending_animation_state_ = EMovement::MOVE_None;
        pawn_.old_movement = old;
        pawn_.movement = frame.movement;
        // TdMove_Landing.StartMove: an ordinary landing is LandNormal and straight back to walking.
        if (is_airborne(old) && (frame.movement == EMovement::MOVE_Walking || frame.movement == EMovement::MOVE_Crouch)) {
            float amount = std::max(0.0f, (fall_top_ - frame.position.z - kSoftLandingHeight) / (kHardLandingHeight - kSoftLandingHeight));
            amount = std::clamp(amount, 0.2f, 1.0f);
            if (old == EMovement::MOVE_Coil) amount = 0.7f;
            land_normal(amount);
        }
        close_to_ground_ = false;
        time_in_move_ = 0.0f;
        last_move_anim_.clear();
        start_move(frame.movement, old, frame);
    }
    time_in_move_ += frame.dt;

    if (root_timer_ >= 0.0f) {
        root_timer_ -= frame.dt;
        if (root_timer_ < 0.0f) set_root_offset(Vec3(0.0f, 0.0f, 0.0f), 0.1f);
    }
    if (root_blend_ > frame.dt) {
        root_offset_ += (root_target_ - root_offset_) * (frame.dt / root_blend_);
        root_blend_ -= frame.dt;
    } else {
        root_offset_ = root_target_;
        root_blend_ = 0.0f;
    }
    if (swing_blend_ > frame.dt) {
        swing_strength_ += (swing_target_ - swing_strength_) * (frame.dt / swing_blend_);
        swing_blend_ -= frame.dt;
    } else {
        swing_strength_ = swing_target_;
        swing_blend_ = 0.0f;
    }
    if (frame.movement == EMovement::MOVE_Swing) swing_angle_ = frame.swing_angle;

    // TdPawn.EnterFallingHeight.
    const bool airborne = is_airborne(frame.movement);
    if (airborne) fall_top_ = airborne_ ? std::max(fall_top_, frame.position.z) : frame.position.z;
    else if (!airborne_) fall_top_ = frame.position.z;
    airborne_ = airborne;

    // The animation the move's own checks chose, on the frame it chooses it.
    if (frame.move_anim == "@reached") {
        // TdMove_SpringBoard.ReachedPreciseLocation: off the other leg than the one in front.
        if (frame.movement == EMovement::MOVE_SpringBoarding) {
            play(Slot::FullBody, tree_.left_leg_forward() ? "SpringBoardRightLeg" : "SpringBoardLeftLeg", 1.0f, 0.15f, 0.25f);
        }
    } else if (!frame.move_anim.empty()) {
        // Hanging, the turns are TdMove_Grab's own (tick_grab), not something the move names.
        const bool own = frame.movement == EMovement::MOVE_Grabbing && lower(frame.move_anim).compare(0, 8, "hangturn") == 0;
        if (!own) play_named(frame.move_anim);
    }

    // Letting go of the stick while walking: the legs take the stopping step (native; measured).
    // Which leg: walktostandpassright with the walk cycle between 0.21 and 0.71 of its length.
    if (frame.movement == EMovement::MOVE_Walking && was_accelerating_ && !frame.accelerating) {
        const float at = tree_.walk_cycle();
        const bool right = at > 0.21f && at < 0.71f;
        play(Slot::LowerBody, right ? "walktostandpassright" : "walktostandpassleft", 1.0f, 0.1f, 0.15f);
    }
    was_accelerating_ = frame.accelerating;

    if (frame.movement == EMovement::MOVE_Climb) tick_climb(frame);
    if (frame.movement == EMovement::MOVE_Grabbing) tick_grab(frame);
    else pawn_.grab_turn_type = 0;
    if (frame.movement == EMovement::MOVE_Melee || frame.movement == EMovement::MOVE_MeleeCrouch) tick_melee(frame);

    // TdMove_Falling.CloseToGround (called from native code): the jump animation lets go before the
    // feet arrive. Measured: it fires while falling faster than 400 with the ground less than 0.4 s
    // away at the speed she is falling.
    if (frame.movement == EMovement::MOVE_Falling && !close_to_ground_ && frame.ground_distance >= 0.0f && frame.velocity.z < -400.0f &&
        frame.ground_distance <= -frame.velocity.z * 0.4f) {
        close_to_ground_ = true;
        tree_.stop_custom_anim(Slot::FullBodyDir, 0.3f);
    }

    if (animation_state_timer_ > 0.0f) {
        animation_state_timer_ -= frame.dt;
        if (animation_state_timer_ <= 0.0f) pawn_.animation_movement = pending_animation_state_;
    }
    pawn_.velocity = frame.velocity;
    pawn_.yaw_deg = frame.yaw_deg;
    pawn_.view_yaw_deg = frame.view_yaw_deg;
    pawn_.view_pitch_deg = frame.view_pitch_deg;
    pawn_.heavy_weapon = frame.heavy_weapon;
    // The swing's angle stays where it was when she lets go (the poses fade under the jump off).
    if (frame.movement == EMovement::MOVE_Swing) pawn_.swing_angle = frame.swing_angle;
    pawn_.balance_lean = frame.balance_lean;
    pawn_.hanging_free = frame.hanging_free;
    pawn_.climbing_pipe = frame.climbing_pipe;
    update_walking_state(frame);
    tick_weapon(frame);
    tick_swan_neck(frame);
    tree_.tick(pawn_, frame.dt);
    last_velocity_ = frame.velocity;
}

}  // namespace me::fp
