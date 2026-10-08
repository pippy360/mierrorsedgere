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

    // The tree's pose this frame.
    void evaluate(const AnimTree& tree, Pose& out) const;
    // Every bone in the mesh component's space.
    void component_space(const Pose& pose, std::vector<Vec3>& pos, std::vector<Quat4>& rot) const;

    // TdPlayerPawn.CalcCamera: the camera sits at the EyeJoint and looks where the controller
    // looks, turned further by what the animation does to the eye. `view_pitch_deg` is the
    // controller's pitch, `yaw_offset_deg` how far its yaw is from the pawn's.
    [[nodiscard]] ViewFrame view(const std::vector<Vec3>& comp_pos, const std::vector<Quat4>& comp_rot, float view_pitch_deg,
                                 float yaw_offset_deg) const;

    [[nodiscard]] int eye_bone() const { return eye_; }

private:
    void atoms(const AnimTree& tree, int index, Pose& out, size_t depth) const;
    void sample(const TreeNode& n, Pose& out) const;
    void reference(Pose& out) const;
    void apply_aim(const TreeNode& n, size_t node_index, Pose& out) const;

    const SkeletalMeshAsset* mesh_ = nullptr;
    std::vector<int> track_of_;               // the AnimSet track of each bone, -1 for none
    std::vector<std::vector<int>> aim_bone_;  // per tree node: the bone of each aim component
    int eye_ = 0;
    // The pawn's axes in the mesh component's space, read off the reference pose's eye.
    Vec3 fwd_{1.0f, 0.0f, 0.0f}, right_{0.0f, 1.0f, 0.0f}, up_{0.0f, 0.0f, 1.0f};
    mutable std::vector<Pose> scratch_;
};

}  // namespace me::fp
