#pragma once

// -----------------------------------------------------------------------------
// The pose Faith's first-person animation tree makes, and the view retail takes
// of it: AnimTree says which sequences play and how much each weighs (fp_anim),
// this blends their bones the way the tree's nodes do, and places the camera
// where TdPlayerPawn.CalcCamera places it, at the EyeJoint.
//
// docs/FIRST_PERSON_ANIMATION_RE.md, "The view".
// -----------------------------------------------------------------------------

#include "anim_system.hpp"
#include "fp_anim.hpp"

#include <unordered_map>
#include <vector>

namespace me::fp {

// A pose: every bone of the mesh, in its parent bone's space.
struct Pose {
    std::vector<Vec3> pos;
    std::vector<Quat4> rot;
};

// The first-person camera against the mesh, in the mesh component's space.
struct ViewFrame {
    Vec3 eye{0.0f, 0.0f, 0.0f};
    Vec3 forward{1.0f, 0.0f, 0.0f};
    Vec3 left{0.0f, -1.0f, 0.0f};
    Vec3 up{0.0f, 0.0f, 1.0f};
    // The eye against the pawn (X forward, Y right, Z up, from the mesh's origin) and what the
    // animation turns the camera by (TdPawn.GetCameraAnimation), in degrees.
    Vec3 eye_pawn{0.0f, 0.0f, 0.0f};
    float anim_pitch = 0.0f, anim_yaw = 0.0f, anim_roll = 0.0f;
};

class PoseEvaluator {
public:
    void init(const SkeletalMeshAsset& mesh, const AnimSetAsset& set, const AnimTree& tree);
    [[nodiscard]] bool ready() const { return mesh_ != nullptr; }

    // The weapon in hand (TdPawn.UpdateWeaponPoseProfile): the bones of its pose profile take what
    // its own `weaponpose` differs by from the common set's, so one set of armed animations grips
    // every weapon. Null sets clear it.
    void set_weapon_pose(const AnimSetAsset* weapon_set, const AnimSetAsset* common_set, const std::vector<int>& bones);
    // TdAnimNodeWeaponPoseOffset.bDisable: a disarm turns the grip offsets off while it plays.
    void set_grip_enabled(bool on) { grip_ = on; }

    // TdSkelControlAim1p on SpineXRight / SpineXLeft: how far each arm is turned with the view's pitch.
    struct Aim {
        float pitch_deg = 0.0f;
        float right = 0.0f;
        float left = 0.0f;
        // Where the swan neck has the camera off the eye (forward, down): the armed arm goes with it.
        float swan_forward = 0.0f;
        float swan_down = 0.0f;
        // OneHandedRightShoulderOffset: the weapon's own nudge of the right shoulder, in the bone's
        // space, at the ready (TdWeapon.OneHandedRightShoulderTranslationOffset).
        Vec3 shoulder{0.0f, 0.0f, 0.0f};
        // HipsControl (TdPlayerPawn.SetHipsOffset): the hips and legs moved against the pawn
        // (forward, right, up), out from under the camera when she looks down running.
        Vec3 hips{0.0f, 0.0f, 0.0f};
    };

    // The tree's pose this frame.
    void evaluate(const AnimTree& tree, Pose& out, const Aim& aim) const;
    void evaluate(const AnimTree& tree, Pose& out) const { evaluate(tree, out, Aim{}); }
    // Every bone in the mesh component's space.
    void component_space(const Pose& pose, std::vector<Vec3>& pos, std::vector<Quat4>& rot) const;

    // TdPlayerPawn.CalcCamera: the camera sits at the EyeJoint and looks where the controller
    // looks, turned further by what the animation does to the eye. `view_pitch_deg` is the
    // controller's pitch, `yaw_offset_deg` how far its yaw is from the pawn's; `swan_forward` and
    // `swan_down` are the swan neck's reach (TdSwanNeck, which the director follows).
    [[nodiscard]] ViewFrame view(const std::vector<Vec3>& comp_pos, const std::vector<Quat4>& comp_rot, float view_pitch_deg,
                                 float yaw_offset_deg, float swan_forward, float swan_down) const;

    [[nodiscard]] int eye_bone() const { return eye_; }

private:
    void atoms(const AnimTree& tree, int index, Pose& out, size_t depth) const;
    void sample(const TreeNode& n, Pose& out) const;
    void reference(Pose& out) const;
    void apply_aim(const TreeNode& n, size_t node_index, Pose& out) const;

    const std::vector<int>& tracks(const AnimSetAsset* set) const;
    void turn_arm(int bone, float degrees, const Vec3& shift, Pose& out) const;

    const SkeletalMeshAsset* mesh_ = nullptr;
    const AnimSetAsset* base_set_ = nullptr;
    // The AnimSet track of each bone (-1 for none), per set: the armed sets order their tracks differently.
    mutable std::unordered_map<const AnimSetAsset*, std::vector<int>> tracks_;
    std::vector<int> pose_bones_;             // the weapon pose profile's bones, with what each is turned and moved by
    std::vector<Quat4> pose_rot_;
    std::vector<Vec3> pose_pos_;
    bool grip_ = true;
    int spine_right_ = -1, spine_left_ = -1, shoulder_right_ = -1, hips_ = -1;
    std::vector<std::vector<int>> aim_bone_;  // per tree node: the bone of each aim component
    int eye_ = 0;
    int camera_ = 0;  // CameraJoint, the eye's child: what the Camera slot's animations turn
    // The pawn's axes in the mesh component's space, read off the reference pose's eye.
    Vec3 fwd_{1.0f, 0.0f, 0.0f}, right_{0.0f, 1.0f, 0.0f}, up_{0.0f, 0.0f, 1.0f};
    mutable std::vector<Pose> scratch_;
};

}  // namespace me::fp
