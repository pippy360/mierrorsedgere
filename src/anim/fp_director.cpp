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
    {"hangstrafeleft", Slot::FullBody, 1.0f, 0.2f, 0.2f, false},
    {"hangstraferight", Slot::FullBody, 1.0f, 0.2f, 0.2f, false},
    {"hangfreestrafeleft", Slot::FullBody, 1.0f, 0.2f, 0.2f, false},
    {"hangfreestrafe", Slot::FullBody, 1.0f, 0.2f, 0.2f, false},
    {"hangfoldedstart", Slot::FullBody, 1.0f, 0.2f, 0.0f, false},
    {"hangend", Slot::FullBody, 1.0f, 0.1f, 0.2f, false},
    {"hangfreeend", Slot::FullBody, 1.0f, 0.1f, 0.2f, false},
    {"hangturnrightstart", Slot::FullBody, 1.0f, 0.2f, 0.2f, false},
    {"hangturnleftstart", Slot::FullBody, 1.0f, 0.2f, 0.2f, false},
    {"hangturnrightend", Slot::FullBody, 1.0f, 0.2f, 0.2f, false},
    {"hangturnleftend", Slot::FullBody, 1.0f, 0.2f, 0.2f, false},
    {"hangfreeturnright", Slot::FullBody, 1.0f, 0.2f, 0.2f, false},
    {"hangfreeturnleft", Slot::FullBody, 1.0f, 0.2f, 0.2f, false},
    {"hangfreefoldedendhangfree", Slot::FullBody, 1.0f, 0.0f, 0.2f, false},
    {"hangfoldedendhang", Slot::FullBody, 1.0f, 0.0f, 0.2f, false},
    // TdMove_GrabTransfer.
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
        case EMovement::MOVE_IntoGrab:
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
        case EMovement::MOVE_IntoZipLine:
        case EMovement::MOVE_AirBarge:
        case EMovement::MOVE_MeleeSlide:
            set_animation_state(EMovement::MOVE_None);
            break;
        case EMovement::MOVE_IntoClimb:
            set_animation_state(EMovement::MOVE_None, 0.2f);
            break;
        case EMovement::MOVE_Barge:
            tree_.stop_custom_anim(Slot::UpperBody, 0.2f);
            break;
        case EMovement::MOVE_Melee:
            tree_.stop_custom_anim(Slot::UpperBody, 0.3f);
            tree_.stop_custom_anim(Slot::FullBody, 0.3f);
            break;
        case EMovement::MOVE_MeleeCrouch:
            tree_.stop_custom_anim(Slot::UpperBody, 0.2f);
            set_animation_state(EMovement::MOVE_None);
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
        case EMovement::MOVE_IntoGrab:
            if (frame.move_anim.empty()) play(Slot::FullBody, frame.hanging_free ? "HangFreeHardStart" : "HangHardStart", 1.0f, 0.1f, 0.2f);
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
            set_animation_state(EMovement::MOVE_None);
            play(Slot::FullBody, "crouchslideintoend45", 1.0f, 0.15f, 0.2f);
            break;
        case EMovement::MOVE_Crouch:
            tree_.stop_custom_anim(Slot::FullBodyDir, 0.25f);
            tree_.stop_custom_anim(Slot::LowerBody, 0.25f);
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
        case EMovement::MOVE_MeleeAir:
            if (frame.move_anim.empty()) play(Slot::FullBody, "MeleeInAir", 1.0f, 0.1f, 0.2f);
            break;
        case EMovement::MOVE_MeleeSlide:
            if (frame.move_anim.empty()) play(Slot::FullBody, "MeleeSlide", 1.0f, 0.1f, 0.1f);
            set_animation_state(EMovement::MOVE_Slide);
            break;
        case EMovement::MOVE_MeleeCrouch:
            set_animation_state(EMovement::MOVE_Crouch);
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
        play_named(frame.move_anim);
    }

    // Letting go of the stick while walking: the legs take the stopping step (native; measured).
    // Which leg: walktostandpassright with the walk cycle between 0.21 and 0.71 of its length.
    if (frame.movement == EMovement::MOVE_Walking && was_accelerating_ && !frame.accelerating) {
        const float at = tree_.walk_cycle();
        const bool right = at > 0.21f && at < 0.71f;
        play(Slot::LowerBody, right ? "walktostandpassright" : "walktostandpassleft", 1.0f, 0.1f, 0.15f);
    }
    was_accelerating_ = frame.accelerating;

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
    update_walking_state(frame);
    tree_.tick(pawn_, frame.dt);
    last_velocity_ = frame.velocity;
}

}  // namespace me::fp
