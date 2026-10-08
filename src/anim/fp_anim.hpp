#pragma once

// -----------------------------------------------------------------------------
// Faith's first-person animation tree, as retail runs it: AT_C1P.AT_C1P loaded out of
// Characters/AT_C1P.upk and ticked from the pawn's state. The tree decides which
// sequences play and how much each weighs; the moves play their one-shot "custom
// animations" on its slots (TdPawn.PlayCustomAnim).
//
// docs/FIRST_PERSON_ANIMATION_RE.md describes the tree and where each rule comes from.
// -----------------------------------------------------------------------------

#include "../math/types.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace me {

struct AnimSequenceAsset;
struct AnimSetAsset;

namespace fp {

// TdPawn.CustomNodeType: the slot a custom animation plays on.
enum class Slot : uint8_t { Canned, CannedUpperBody, FullBody, FullBodyDir, UpperBody, LowerBody, Camera, Weapon, Face, Count };

// TdPawn.WalkingState.
enum WalkingState : uint8_t { kWasIdle, kWasSneak, kWasWalk, kWasJog, kWasRun, kWasSprint, kWasNone };

// What the tree's nodes read off the pawn each frame.
struct PawnAnimState {
    EMovement movement = EMovement::MOVE_Walking;           // MovementState
    EMovement old_movement = EMovement::MOVE_None;          // OldMovementState
    EMovement animation_movement = EMovement::MOVE_None;    // AnimationMovementState: what SetAnimationMovementState forces
    uint8_t walking_state = kWasIdle;                       // CurrentWalkingState
    Vec3 velocity{0.0f, 0.0f, 0.0f};
    // The velocity of the frame before: the pawn's Tick runs before its physics, and what it works
    // out there (the walking state, the speed the play rates scale by) is a frame behind.
    Vec3 last_velocity{0.0f, 0.0f, 0.0f};
    float yaw_deg = 0.0f;         // the pawn's Rotation.Yaw
    float view_yaw_deg = 0.0f;    // the controller's view
    float view_pitch_deg = 0.0f;
    bool heavy_weapon = false;    // GetWeaponType() == EWT_Heavy
    // How far the weapon arms are laid over the body (ArmedLeft / ArmedRight's Child2Weight): 0 unarmed.
    float armed_left = 0.0f;
    float armed_right = 0.0f;
    int weapon_state = 0;         // TdPawn.WeaponAnimState: 0 unarmed, 1 relaxed, 2 ready, 3 reload, 4 throwing, 5 heavy armed
    float swing_angle = 0.0f;     // radians from hanging straight down, positive ahead of the bar
    float balance_lean = 0.0f;    // -1 .. 1 off the beam
    bool hanging_free = false;
    bool climbing_pipe = false;
    int climb_hand = 0;           // 0 left hand up, 1 right
    bool climb_sliding = false;
    int grab_turn_type = 0;       // TdPawn.CurrentGrabTurnType: 0 none, 1 start, 2 end, 3 idle
    float look_deg = 0.0f;        // the view's yaw off the body's, positive to the right
    float grab_turn_deg = 0.0f;   // the view's yaw off the body's while hanging
};

// One node of the tree: its fixed properties and its state this frame.
struct TreeNode {
    enum class Kind : uint8_t {
        Passthrough,   // one child at full weight (AnimTree, AnimNodeSynch, and the pose-offset nodes, which only move bones)
        Sequence,      // TdAnimNodeSequence
        Slot,          // TdAnimNodeSlot: Source, then two channels
        PerBone,       // AnimNodeBlendPerBone: Source always, Target by Child2Weight on its bones
        List,          // a TdAnimNodeBlendList of some class: one active child, blended to over a time
        Directional,   // TdAnimNodeBlendDirectional
        Fixed,         // children at the weights the package saved (AnimNodeCrossfader)
    };
    Kind kind = Kind::Passthrough;
    std::string cls;
    std::string name;  // NodeName
    std::vector<int> children;
    std::vector<std::string> child_names;

    // Fixed properties.
    std::string seq_name;            // AnimSeqName
    float rate = 1.0f;
    bool looping = false;
    bool playing = false;
    bool scale_rate_by_speed = false;  // ScalePlayRateBySpeed with SPRT_GroundSpeedSize
    float base_speed = 0.0f, rate_min = 0.0f, rate_max = 0.0f;
    bool synchronize = false;        // has a SynchGroupName
    float synch_offset = 0.0f;       // SynchPosOffset
    bool reset_on_relevant = false;  // bResetOnBecomeRelevant
    float start_position = 0.0f;     // NormalizedStartPosition
    std::vector<int> state_mapping;  // TdAnimNodeState.StateMapping: an enum value per child after the first
    std::vector<float> blend_in;     // TdAnimNodeBlendList.BlendWeight: seconds to blend to each child
    std::vector<float> blend_out;    // BlendOutWeight
    bool use_old_state = false;      // TdAnimNodeMovementState.bUseOldState
    float child2_weight = 0.0f;      // AnimNodeBlend.Child2Weight
    std::vector<float> bone_weight;  // AnimNodeBlendPerBone.Child2PerBoneWeight: how much of the target each bone takes
    // AnimNodeAimOffset.Profiles[0].AimComponents: what the node adds to a bone, in the mesh's space,
    // for each of the nine directions LU LC LD CU CC CD RU RC RD.
    struct AimBone {
        std::string bone;
        float rot[9][4];
        float trans[9][3];
    };
    std::vector<AimBone> aim;
    float aim_range_neg = 1.0f, aim_range_pos = 1.0f;  // the profile's HorizontalRange: the Aim.X at which its left and right poses are whole
    bool aim_from_legs = false;                         // TdAnimNodeAimOffset.bAimSourceIsLegRotation

    // State.
    std::vector<float> weight;   // per child
    std::vector<float> target;
    float blend_to_go = 0.0f;
    int active = 0;
    int last_state = -1;         // TdAnimNodeMovementState: the state it last blended to (GetBlendValue's previous state)
    float total = 0.0f;          // NodeTotalWeight
    float incoming = 0.0f;       // what the parents ticked so far this frame have passed down
    bool relevant = false;
    float time = 0.0f;           // CurrentTime
    float pending_blend_out = -1.0f;  // a slot: blend back to the source when this much of the animation is left
    float hold = -1.0f;               // TdAnimNodeCustomBlend.Duration: how long it stays before blending back
    float hold_blend_out = 0.0f;      // TdAnimNodeCustomBlend.BlendOutTime
    float forward_blend = 1.0f;       // TdAnimNodeBlendDirectional.ForwardBlend: 1 going forward, 0 going backward
    float side_blend = 0.0f;          // 0 straight ahead (or back), 0.9 straight sideways
    float dir_side = 0.0f, dir_forward = 1.0f;  // TdAnimNodeBlendDirectional.Direction: |side| + |forward| = 1
    bool going_forward = true;        // bGoingForward
    float aim_x = 0.0f, aim_y = 0.0f; // AnimNodeAimOffset.Aim
    bool root_motion = false;         // a slot's channel: the animation's root movement goes to the pawn
    bool unlisted = false;            // a channel of the Camera or Canned slot: not in the retail recorder's list
    const AnimSequenceAsset* seq = nullptr;
    const AnimSetAsset* seq_set = nullptr;  // the AnimSet `seq` is from (its tracks are in that set's bone order)
};

class AnimTree {
public:
    // Looks a sequence up by name in the mesh's AnimSets.
    // The mesh's AnimSets are searched last to first, so a weapon's sequences shadow the unarmed
    // ones of the same name; the set a sequence came from is handed back too.
    using SequenceLookup = std::function<const AnimSequenceAsset*(const std::string&, const AnimSetAsset**)>;

    bool load(const std::string& game_root, std::string& error);
    void set_sequence_lookup(SequenceLookup lookup) { lookup_ = std::move(lookup); }
    // The AnimSets changed (a weapon taken or dropped): every node looks its sequence up again.
    void invalidate_sequences();
    // TdAnimNodeWeaponPoseOffset.Profiles: the bones each weapon's pose profile holds, by profile name.
    [[nodiscard]] const std::vector<std::pair<std::string, std::vector<int>>>& weapon_pose_profiles() const { return weapon_pose_profiles_; }
    // TdPawn.LegRotation: where the legs point (degrees).
    [[nodiscard]] float leg_yaw() const { return leg_yaw_; }
    // How much of the ready stance the weapon state node shows (its Default child's weight).
    [[nodiscard]] float weapon_ready() const { return weapon_ready_; }
    [[nodiscard]] bool loaded() const { return root_ >= 0; }

    // Everything back to how the package saved it.
    void reset();
    void tick(const PawnAnimState& pawn, float dt);

    // TdPawn.PlayCustomAnim / StopCustomAnim.
    void play_custom_anim(Slot slot, const std::string& name, float rate, float blend_in, float blend_out, bool looping, bool override_playing,
                          bool root_motion = false);
    void stop_custom_anim(Slot slot, float blend_out);
    [[nodiscard]] bool custom_anim_playing(Slot slot, std::string* name = nullptr) const;
    // How far into its animation the slot's active channel is (0 with none).
    [[nodiscard]] float custom_anim_time(Slot slot) const;

    // TdAnimNodeCustomBlend.Activate on the node of that name: its second child at `amount` for
    // `duration`, blended in and back out.
    void activate_custom_blend(const std::string& node_name, float amount, float duration, float blend_in, float blend_out);

    // TdAnimNodeLandOffset.Landed: the dip of a landing, by how hard it was (0..1).
    void set_landed(float amount);

    // Where the walk cycle is: the "Walk" synch group's master, as CurrentTime / SequenceLength (-1 with none).
    [[nodiscard]] float walk_cycle() const;

    // Every sequence player with weight, heaviest first: what the retail recorder logs as anim1p.
    struct Leaf {
        std::string name;
        float time = 0.0f;
        float weight = 0.0f;
    };
    void leaves(std::vector<Leaf>& out, size_t limit = 3) const;

    // MasterSync.Groups[0].MasterNode.CurrentTime > half its length (TdPawn.IsLeftLegForward).
    [[nodiscard]] bool left_leg_forward() const;

    [[nodiscard]] const std::vector<TreeNode>& nodes() const { return nodes_; }

private:
    void set_active(TreeNode& n, int child, float blend_time);
    void update_list(TreeNode& n, const PawnAnimState& pawn, bool became_relevant);
    void update_directional(TreeNode& n, const PawnAnimState& pawn, float dt, bool became_relevant);
    void advance(TreeNode& n, const PawnAnimState& pawn, float dt);
    void tick_walk_group(const PawnAnimState& pawn, float dt);
    int slot_node(Slot slot) const;
    void resolve(TreeNode& n) const;

    std::vector<TreeNode> nodes_;
    std::vector<int> order_;  // parents before children
    int root_ = -1;
    int slots_[static_cast<size_t>(Slot::Count)];
    SequenceLookup lookup_;
    std::vector<std::pair<std::string, std::vector<int>>> weapon_pose_profiles_;
    float weapon_ready_ = 0.0f;
    // The "Walk" synch group: its members, and the one leading it this frame.
    std::vector<int> walk_group_;
    std::vector<char> in_walk_group_;
    int walk_master_ = -1;
    int walk_synch_ = -1;  // the AnimNodeSynch that owns it ("MasterSync")
    float land_amount_ = 0.0f;
    float land_time_ = -1.0f;
    // TdAnimNodeTurn (native; measured): where the legs point, and the standing turn that brings
    // them round when the view has left them more than 65 degrees behind.
    float leg_yaw_ = 0.0f;
    bool leg_yaw_set_ = false;
    bool turning_ = false;
    float turn_time_ = 0.0f;
    float turn_side_ = 0.0f;
    void tick_turn(TreeNode& n, const PawnAnimState& pawn, float dt, bool became_relevant);
};

}  // namespace fp
}  // namespace me
