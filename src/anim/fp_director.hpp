#pragma once

// -----------------------------------------------------------------------------
// What the pawn and its moves ask of the first-person animation tree, frame by
// frame: the walking state, and each TdMove_*'s PlayMoveAnim / StopCustomAnim /
// SetAnimationMovementState calls, made where the move's script makes them.
//
// docs/FIRST_PERSON_ANIMATION_RE.md lists the calls per move.
// -----------------------------------------------------------------------------

#include "fp_anim.hpp"
#include "fp_pose.hpp"

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
    bool armed = false;          // a weapon in hand
    bool fired = false;          // it fired this frame
    // TdMove_Jump.StartJump: nothing to land on 1.1 x the forward speed ahead and up to 200 below.
    bool long_jump_over_gap = false;
    // How far the feet are above what is under them; negative when not known.
    float ground_distance = -1.0f;
    // The animation the move's own checks of the level settled on this frame (which vault, which
    // ledge grab, which heave), by name; empty when the move plays the one it always plays.
    std::string move_anim;
    // "@reached" in move_anim: the move got to the place it was steering for (TdMove.ReachedPreciseLocation).
    bool move_left = false;      // a sideways move going left (dodge jumps)
    bool accelerating = true;    // the player is pushing a direction (Acceleration is not zero)
    bool hanging_free = false;   // hanging with nothing for the feet (TdMove_Grab.bIsHangingFree)
    bool ledge_sloped = false;   // the ledge's top is not level (TdMove_Grab.bSlopedLedge)
    float ledge_slope_deg = 0.0f;  // how steeply it runs up to her right
    float swing_angle = 0.0f;    // radians from hanging straight down, positive ahead of the bar
    float balance_lean = 0.0f;   // -1 .. 1 off the beam
    int balance_danger = 0;      // losing her balance to the left (-1) or the right (1)
    bool climbing_pipe = false;  // on a pipe, not a ladder
    float climb_top = -1.0f;     // uu up to the ladder's last step, down to its first (-1 not known)
    float climb_bottom = -1.0f;
    // TdMove_Melee: which blow (0 right, 1 left, 2 the shove that ends a combo, 3 crouched) and
    // whether it lands. -1: the move names its animations itself (move_anim).
    int melee_variant = -1;
    bool melee_hit = false;
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

    // Where the moves put the whole mesh against the pawn, which the pose does not hold: the root
    // offset (TdPawn.SetRootOffset) and the swing's turn about the bar (TdMove_Swing.SetPawnRotation).
    // Moves the eye and turns the camera of `view` accordingly; the body's place against its own
    // eye is not changed by either.
    void apply_mesh_transform(ViewFrame& view) const;
    // The swan neck's reach this frame (TdSwanNeck.UpdateSwanNeck), and the hips' offset (SetHipsOffset).
    [[nodiscard]] float swan_forward() const { return swan_forward_; }
    [[nodiscard]] float swan_down() const { return swan_down_; }
    [[nodiscard]] Vec3 hips_offset() const { return hips_offset_; }

private:
    void stop_move(EMovement move, EMovement pending, const PawnFrame& frame);
    void start_move(EMovement move, EMovement old, const PawnFrame& frame);
    void update_walking_state(const PawnFrame& frame);
    bool play_named(const std::string& name);
    void land_normal(float amount);
    void tick_climb(const PawnFrame& frame);
    void tick_grab(const PawnFrame& frame);
    void tick_melee(const PawnFrame& frame);
    void set_root_offset(const Vec3& offset, float blend_time);
    void tick_weapon(const PawnFrame& frame);
    void tick_swan_neck(const PawnFrame& frame);
    // TdMove.PlayMoveAnim.
    void play(Slot slot, const char* name, float rate, float blend_in, float blend_out) {
        tree_.play_custom_anim(slot, name, rate, blend_in, blend_out, false, true);
    }
    // TdPawn.SetAnimationMovementState / ClearAnimationMovementState, with their timers.
    void set_animation_state(EMovement state, float delay = -1.0f);

    AnimTree tree_;
    PawnAnimState pawn_;
    bool started_ = false;
    Vec3 last_velocity_{0.0f, 0.0f, 0.0f};  // the velocity the pawn's Tick sees: last frame's physics
    EMovement pending_animation_state_ = EMovement::MOVE_None;
    float animation_state_timer_ = -1.0f;
    std::string last_move_anim_;
    bool close_to_ground_ = false;   // TdMove_Falling.CloseToGround has fired in this fall
    float time_in_move_ = 0.0f;
    bool was_accelerating_ = false;
    float fall_top_ = 0.0f;          // TdPawn.EnterFallingHeight: the highest she has been in this fall
    bool airborne_ = false;
    // TdMove_Climb: bClimbLeftHand, and the step being climbed.
    bool climb_left_hand_ = false;
    float climb_step_time_ = -1.0f;
    float climb_step_length_ = 0.0f;
    bool climb_hand_switched_ = false;
    bool climb_exiting_ = false;     // TdMove_Climb.ExitAtTop is playing
    float climb_last_vz_ = 0.0f;
    float climb_step_z_ = 0.0f;      // where the step being climbed started
    bool sloped_ledge_ = false;      // the ledge being caught or hung from is sloped
    float climb_step_size_ = 32.0f;  // and how far it goes: one rung, or two on a pipe
    // TdMove_Grab: CurrentGrabTurnType (0 none, 1 start, 2 end, 3 idle) with its timer, the free
    // hang's turn, and the shimmy step.
    int grab_turn_ = 0;
    float grab_timer_ = -1.0f;
    bool grab_turned_right_ = false;
    bool grab_free_turn_ = false;
    bool shimmy_ = false;
    // TdPawn.SetRootOffset, as forward, right, up; and the swing control's strength.
    Vec3 root_offset_{0.0f, 0.0f, 0.0f};
    Vec3 root_target_{0.0f, 0.0f, 0.0f};
    float root_blend_ = 0.0f;
    float root_timer_ = -1.0f;       // TdMove_Crouch: when its lift is taken off again
    float swing_strength_ = 0.0f;
    float swing_target_ = 0.0f;
    float swing_blend_ = 0.0f;
    float swing_angle_ = 0.0f;
    float swan_forward_ = 0.0f, swan_down_ = 0.0f;
    Vec3 hips_offset_{0.0f, 0.0f, 0.0f};
    float slide_ended_ = 1.0f;       // seconds since a slide ended (the crouch's hips wait 0.3 s)
    // TdPawn.WeaponAnimState's bookkeeping (UpdateWeaponAnimState): BecameReadyTime, AmountTilUnarmed.
    bool was_armed_ = false;
    float ready_for_ = 0.0f;
    float amount_til_unarmed_ = 0.0f;
    // TdMove_Melee: 1 while the wind-up plays, 2 after the blow.
    int melee_phase_ = 0;
    int melee_variant_ = -1;
};

}  // namespace me::fp
