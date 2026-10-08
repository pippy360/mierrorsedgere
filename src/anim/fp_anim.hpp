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
    float yaw_deg = 0.0f;         // the pawn's Rotation.Yaw
    float view_yaw_deg = 0.0f;    // the controller's view
    float view_pitch_deg = 0.0f;
    bool heavy_weapon = false;    // GetWeaponType() == EWT_Heavy
    // How far the weapon arms are laid over the body (ArmedLeft / ArmedRight's Child2Weight): 0 unarmed.
    float armed_left = 0.0f;
    float armed_right = 0.0f;
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

    // State.
    std::vector<float> weight;   // per child
    std::vector<float> target;
    float blend_to_go = 0.0f;
    int active = 0;
    float total = 0.0f;          // NodeTotalWeight
    float incoming = 0.0f;       // what the parents ticked so far this frame have passed down
    bool relevant = false;
    float time = 0.0f;           // CurrentTime
    float pending_blend_out = -1.0f;  // a slot: blend back to the source when this much of the animation is left
    float forward_blend = 1.0f;       // TdAnimNodeBlendDirectional.ForwardBlend: 1 going forward, 0 going backward
    float side_blend = 0.0f;          // 0 straight ahead (or back), 1 straight sideways
    const AnimSequenceAsset* seq = nullptr;
};

class AnimTree {
public:
    // Looks a sequence up by name in the mesh's AnimSets.
    using SequenceLookup = std::function<const AnimSequenceAsset*(const std::string&)>;

    bool load(const std::string& game_root, std::string& error);
    void set_sequence_lookup(SequenceLookup lookup) { lookup_ = std::move(lookup); }
    [[nodiscard]] bool loaded() const { return root_ >= 0; }

    // Everything back to how the package saved it.
    void reset();
    void tick(const PawnAnimState& pawn, float dt);

    // TdPawn.PlayCustomAnim / StopCustomAnim.
    void play_custom_anim(Slot slot, const std::string& name, float rate, float blend_in, float blend_out, bool looping, bool override_playing);
    void stop_custom_anim(Slot slot, float blend_out);
    [[nodiscard]] bool custom_anim_playing(Slot slot, std::string* name = nullptr) const;

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
    int slot_node(Slot slot) const;
    const AnimSequenceAsset* find_sequence(const std::string& name) const;

    std::vector<TreeNode> nodes_;
    std::vector<int> order_;  // parents before children
    int root_ = -1;
    int slots_[static_cast<size_t>(Slot::Count)];
    SequenceLookup lookup_;
    // The "Walk" synch group: its members, and the one leading it this frame.
    std::vector<int> walk_group_;
    int walk_master_ = -1;
};

}  // namespace fp
}  // namespace me
