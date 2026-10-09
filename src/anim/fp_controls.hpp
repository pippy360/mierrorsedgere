#pragma once

// -----------------------------------------------------------------------------
// The skeletal controls of Faith's first-person tree: AT_C1P's SkelControlLists,
// which move bones after the animations have posed them. All of them are native;
// the rules here are read out of retail's executable.
//
//   SpineX      WeaponYaw, WeaponPitch, WeaponRoll   TdSkelControlLazySpring
//   EyeJoint    CameraRoll                           TdSkelControlLazySpring
//   SpineXLeft / SpineXRight
//               the aim (TdSkelControlAim1p), then two TdSkelControlRandom
//   RightHand   RightHandRecoil (TdSkelControlRecoil), RightHandPitch / Yaw / Roll
//               (lazy springs), RightHandWorldIKController, the wall's (TdSkelControlAgainstWall)
//   LeftHand    LeftHandLocalIKController, LeftHandWorldIKController, the wall's
//   LeftFoot / RightFoot
//               TdSkelControlFootPlacement
//
// docs/FIRST_PERSON_ANIMATION_RE.md, "The skeletal controls".
// -----------------------------------------------------------------------------

#include "anim_system.hpp"
#include "fp_anim.hpp"

#include <functional>
#include <vector>

namespace me::fp {

struct Pose;

// SkelControlBase.ControlStrength: SetSkelControlStrength and the blend TickSkelControl runs.
struct ControlStrength {
    float value = 0.0f;
    float target = 0.0f;
    float to_go = 0.0f;
    void set(float strength, float blend_time);
    void tick(float dt);
};

// Where the mesh component is in the world, for the controls that work there.
struct MeshPlace {
    Vec3 origin{0.0f, 0.0f, 0.0f};
    Vec3 forward{1.0f, 0.0f, 0.0f};  // the pawn's facing; up is +Z
    Vec3 right{0.0f, 1.0f, 0.0f};
    // A line check through the level (TRACE_World, no extent): true with where it meets something.
    std::function<bool(const Vec3& from, const Vec3& to, Vec3& hit, Vec3& normal)> trace;
};

class SkelControls {
public:
    // TdPawn.MoveAimMode.
    enum AimMode : uint8_t { kAimLeft, kAimRight, kAimTwoHanded, kAimNoHands, kAimDefault };
    // TdMove.AimMode of the move that runs a movement state.
    static AimMode move_aim_mode(EMovement move);

    struct Input {
        float dt = 0.0f;
        EMovement movement = EMovement::MOVE_Walking;
        uint8_t walking_state = 0;       // TdPawn.CurrentWalkingState
        int weapon_state = 0;            // TdPawn.WeaponAnimState
        bool armed = false;              // MyWeapon != None
        bool heavy_weapon = false;       // GetWeaponType() == EWT_Heavy
        float view_pitch_deg = 0.0f;     // Controller.Rotation
        float view_yaw_deg = 0.0f;
        float pawn_yaw_deg = 0.0f;       // Rotation.Yaw
        Vec3 velocity{0.0f, 0.0f, 0.0f};
        float walking_node_weight = 0.0f;  // NodeTotalWeight of the node named WalkingState
        float wall_weight[2] = {0.0f, 0.0f};  // ... AgainstWallLeft, AgainstWallRight
        int against_wall = 0;            // TdPawn.AgainstWallState
        Vec3 wall_hand[2];               // TdPawn.AgainstWallLeftHand, AgainstWallRightHand (world)
        // LeftHandWorldIKController / RightHandWorldIKController as the moves set them.
        bool hand_ik[2] = {false, false};
        Vec3 hand_ik_target[2];          // EffectorLocation (world)
        float hand_ik_blend[2] = {0.0f, 0.0f};  // the blend time of the last change
        // TdPlayerPawn.EnableFootPlacement / DisableFootPlacement.
        bool foot_placement = false;
        float foot_blend = 0.2f;
        bool floor_sloped = false;       // Floor.Z != 1
        float smooth_offset = 0.0f;      // TdPawn.SmoothOffset
        bool fired = false;              // TdWeapon.PlayFiringAnimation this frame
    };

    // `fwd`, `right`, `up`: the pawn's axes in the mesh component's space.
    void init(const SkeletalMeshAsset& mesh, const Vec3& fwd, const Vec3& right, const Vec3& up);
    void reset();
    // TickSkelControl of every control.
    void tick(const Input& in);
    // The controls laid over the pose, bone by bone (USkeletalMeshComponent::ApplyControllersForBoneIndex).
    // `swan` is what the swan neck moves the camera by, in the component's space.
    void apply(Pose& pose, const MeshPlace& place, const Vec3& swan);

    // TdPawn.GetAimMode(false / true).
    [[nodiscard]] AimMode aim_mode(bool for_aim) const;
    [[nodiscard]] float aim_strength(int side) const { return aim_[side].value; }
    [[nodiscard]] float shoulder_strength() const { return shoulder_.value; }
    [[nodiscard]] float foot_strength() const { return foot_[0].strength.value; }
    [[nodiscard]] float hand_ik_strength(int side) const { return world_ik_[side].value; }
    [[nodiscard]] float camera_roll_units() const { return static_cast<float>(springs_[6].angle); }

private:
    // TdSkelControlLazySpring.
    struct Spring {
        int bone = -1;
        int affected = 0;  // SpringAxis: 0 yaw, 1 pitch, 2 roll
        int source = 0;
        int min_angle = -8192, max_angle = 8192;
        float interpolate = 0.1f;
        float multiplier = 1.0f;
        bool by_velocity = true;
        int lazy = 0;      // LazyRotation
        int angle = 0;     // what it sets BoneRotation's axis to
        ControlStrength strength;
    };
    // TdSkelControlRandom.
    struct Sway {
        int axis = 0;      // RandomAxis: 0 yaw, 1 pitch, 2 roll
        int min_angle = -1000, max_angle = 1000;
        float frequency = 0.5f;
        float interpolate = 0.5f;
        float until = 0.0f;  // TimeToUpdate
        int current = 0, target = 0;
    };
    struct Foot {
        int bone = -1;
        bool invert_bone = false;
        Vec3 joint_target{0.0f, 0.0f, 0.0f};
        ControlStrength strength;
        bool reset = true;       // bResetOnActivated
        float interpolated = 0.0f;
    };

    void refresh(const Pose& pose);
    void blend_in(Pose& pose, int bone, const Vec3& pos, const Quat4& rot, float strength);
    void turn_local(Pose& pose, int bone, const Quat4& turn, float strength);
    bool limb(Pose& pose, int bone, const Vec3& desired, const Vec3& joint_target, int bone_axis, int joint_axis, bool invert_bone,
              const Quat4* end_rot, bool keep_upper_roll, float strength);
    void apply_foot(Pose& pose, Foot& foot, const MeshPlace& place);
    [[nodiscard]] Vec3 to_comp(const MeshPlace& place, const Vec3& world) const;
    [[nodiscard]] Vec3 to_world(const MeshPlace& place, const Vec3& comp) const;
    int rand15();

    const SkeletalMeshAsset* mesh_ = nullptr;
    Vec3 fwd_{0.0f, 0.0f, 1.0f}, right_{-1.0f, 0.0f, 0.0f}, up_{0.0f, -1.0f, 0.0f};
    int spine_ = -1, spine_side_[2] = {-1, -1}, hand_[2] = {-1, -1}, eye_ = -1, ik_bone_ = -1;
    std::vector<Vec3> cpos_;
    std::vector<Quat4> crot_;

    Input in_;
    Spring springs_[7];        // WeaponYaw, WeaponPitch, WeaponRoll, RightHandPitch, RightHandYaw, RightHandRoll, CameraRoll
    ControlStrength aim_[2];   // LeftAimController, RightAimController
    Sway sway_[2];
    float sway_strength_ = 0.0f;
    ControlStrength recoil_;
    float recoil_x_ = 0.0f;    // RightHandRecoil's EffectorLocation.X
    float recoil_delay_ = 0.0f;
    ControlStrength shoulder_; // OneHandedRightShoulderOffset
    ControlStrength local_ik_; // LeftHandLocalIKController
    ControlStrength world_ik_[2];
    Vec3 world_ik_target_[2];
    Vec3 wall_effector_[2];    // TdSkelControlAgainstWall.EffectorLocation, in the component's space
    Vec3 wall_target_[2];
    Foot foot_[2];
    uint32_t seed_ = 1;
    bool started_ = false;
};

}  // namespace me::fp
