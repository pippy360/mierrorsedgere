#pragma once

// -----------------------------------------------------------------------------
// What the pawn and its moves ask of the first-person animation tree, frame by
// frame: the walking state, and each TdMove_*'s PlayMoveAnim / StopCustomAnim /
// SetAnimationMovementState calls, made where the move's script makes them.
//
// docs/FIRST_PERSON_ANIMATION_RE.md lists the calls per move.
// -----------------------------------------------------------------------------

#include "fp_anim.hpp"

#include <string>

namespace me::fp {

// One frame of the pawn, as the scripts see it.
struct PawnFrame {
    float dt = 0.0f;
    EMovement movement = EMovement::MOVE_Walking;
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 velocity{0.0f, 0.0f, 0.0f};
    float yaw_deg = 0.0f;         // the pawn's Rotation.Yaw
    float view_yaw_deg = 0.0f;
    float view_pitch_deg = 0.0f;
    bool heavy_weapon = false;
    // TdMove_Jump.StartJump: nothing to land on 1.1 x the forward speed ahead and up to 200 below.
    bool long_jump_over_gap = false;
};

class Director {
public:
    bool init(const std::string& game_root, AnimTree::SequenceLookup lookup, std::string& error);
    void reset();
    // A change of `movement` from the last frame is the move change: StopMove on the old move, StartMove on the new.
    void tick(const PawnFrame& frame);

    [[nodiscard]] AnimTree& tree() { return tree_; }
    [[nodiscard]] const AnimTree& tree() const { return tree_; }
    [[nodiscard]] const PawnAnimState& pawn() const { return pawn_; }

private:
    void stop_move(EMovement move, EMovement pending, const PawnFrame& frame);
    void start_move(EMovement move, EMovement old, const PawnFrame& frame);
    void update_walking_state(const PawnFrame& frame);
    // TdMove.PlayMoveAnim.
    void play(Slot slot, const char* name, float rate, float blend_in, float blend_out) {
        tree_.play_custom_anim(slot, name, rate, blend_in, blend_out, false, true);
    }
    // TdPawn.SetAnimationMovementState / ClearAnimationMovementState, with their timers.
    void set_animation_state(EMovement state, float delay = -1.0f);

    AnimTree tree_;
    PawnAnimState pawn_;
    bool started_ = false;
    EMovement pending_animation_state_ = EMovement::MOVE_None;
    float animation_state_timer_ = -1.0f;
};

}  // namespace me::fp
