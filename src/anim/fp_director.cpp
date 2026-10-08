#include "fp_director.hpp"

#include <algorithm>
#include <cmath>

namespace me::fp {

namespace {

// Default__TdMove_Jump.
constexpr float kJumpBlendInTime = 0.1f;
constexpr float kJumpBlendOutTime = 0.2f;
constexpr float kLongJumpNormalThreshold = 500.0f;

float forward_speed(const PawnFrame& f) {
    const float yaw = f.yaw_deg * DEG2RAD;
    return std::cos(yaw) * f.velocity.x + std::sin(yaw) * f.velocity.y;
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

// TdPawn.UpdateWalkingState is native. These thresholds are fitted to the recordings
// (docs/FIRST_PERSON_ANIMATION_RE.md, the walking state).
void Director::update_walking_state(const PawnFrame& frame) {
    const float speed = std::sqrt(frame.velocity.x * frame.velocity.x + frame.velocity.y * frame.velocity.y);
    uint8_t state = kWasIdle;
    if (speed >= 650.0f) state = kWasSprint;
    else if (speed >= 350.0f) state = kWasRun;
    else if (speed >= 200.0f) state = kWasJog;
    else if (speed >= 50.0f) state = kWasWalk;
    else if (speed >= 1.0f) state = kWasSneak;
    pawn_.walking_state = state;
}

void Director::stop_move(EMovement move, EMovement pending, const PawnFrame& frame) {
    (void)frame;
    switch (move) {
        case EMovement::MOVE_Jump:
            // TdMove_Jump.StopMove: the jump animation carries on into a fall or a pull-up.
            if (pending != EMovement::MOVE_Falling && pending != EMovement::MOVE_VaultOver) tree_.stop_custom_anim(Slot::FullBodyDir, 0.2f);
            break;
        case EMovement::MOVE_Falling:
            // TdMove_Falling.StopMove.
            if (pawn_.old_movement != EMovement::MOVE_DodgeJump) {
                tree_.stop_custom_anim(Slot::FullBody, 0.2f);
                tree_.stop_custom_anim(Slot::FullBodyDir, 0.2f);
            }
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
        pawn_.old_movement = old;
        pawn_.movement = frame.movement;
        start_move(frame.movement, old, frame);
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
}

}  // namespace me::fp
