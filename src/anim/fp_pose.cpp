#include "fp_pose.hpp"

#include <algorithm>
#include <cmath>

namespace me::fp {

namespace {

constexpr float kZeroWeight = 0.00001f;

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

float dot4(const Quat4& a, const Quat4& b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

// A key of a track at `u` of the way through the sequence. A looping sequence runs on from its
// last key into its first (UE3: NumFrames intervals); one that plays once ends on its last key.
template <typename T, typename Lerp>
T key_at(const std::vector<T>& keys, float u, bool looping, Lerp lerp) {
    const size_t count = keys.size();
    if (count == 1) return keys[0];
    if (looping) {
        const float f = u * static_cast<float>(count);
        const size_t i0 = static_cast<size_t>(f) % count;
        return lerp(keys[i0], keys[(i0 + 1) % count], f - std::floor(f));
    }
    const float f = u * static_cast<float>(count - 1);
    const size_t i0 = std::min(static_cast<size_t>(f), count - 1);
    return lerp(keys[i0], keys[std::min(i0 + 1, count - 1)], f - static_cast<float>(i0));
}

Quat4 nlerp(const Quat4& a, Quat4 b, float t) {
    if (dot4(a, b) < 0.0f) b = b.negated();
    return Quat4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t).normalized();
}

// AnimNodeAimOffset.GetBoneAimQuaternion / GetBoneAimTranslation: between the four poses around the aim.
void aim_offset(const TreeNode::AimBone& a, float x, float y, Quat4& rot, Vec3& trans) {
    const int side = x >= 0.0f ? 6 : 0;   // R* or L*
    const int level = y >= 0.0f ? 0 : 2;  // *U or *D
    const float ax = std::min(std::fabs(x), 1.0f), ay = std::min(std::fabs(y), 1.0f);
    auto q = [&](int d) { return Quat4(a.rot[d][0], a.rot[d][1], a.rot[d][2], a.rot[d][3]); };
    auto t = [&](int d) { return Vec3(a.trans[d][0], a.trans[d][1], a.trans[d][2]); };
    const Quat4 centre = nlerp(q(4), q(side + 1), ax);
    const Quat4 edge = nlerp(q(3 + level), q(side + level), ax);
    rot = nlerp(centre, edge, ay);
    const Vec3 tc = t(4) + (t(side + 1) - t(4)) * ax;
    const Vec3 te = t(3 + level) + (t(side + level) - t(3 + level)) * ax;
    trans = tc + (te - tc) * ay;
}

}  // namespace

const std::vector<int>& PoseEvaluator::tracks(const AnimSetAsset* set) const {
    auto it = tracks_.find(set);
    if (it != tracks_.end()) return it->second;
    std::vector<int>& out = tracks_[set];
    out.assign(mesh_->bones.size(), -1);
    for (size_t b = 0; b < mesh_->bones.size(); ++b) {
        const std::string key = mesh_->bones[b].name_lower.empty() ? lower(mesh_->bones[b].name) : mesh_->bones[b].name_lower;
        auto track = set->bone_to_track.find(key);
        if (track != set->bone_to_track.end()) out[b] = track->second;
    }
    return out;
}

void PoseEvaluator::init(const SkeletalMeshAsset& mesh, const AnimSetAsset& set, const AnimTree& tree) {
    mesh_ = &mesh;
    base_set_ = &set;
    tracks_.clear();
    const size_t bones = mesh.bones.size();
    for (size_t b = 0; b < bones; ++b) {
        const std::string key = lower(mesh.bones[b].name);
        if (key == "eyejoint") eye_ = camera_ = static_cast<int>(b);
        if (key == "rightshoulder") shoulder_right_ = static_cast<int>(b);
        if (key == "hips") hips_ = static_cast<int>(b);
    }
    for (size_t b = 0; b < bones; ++b) {
        if (lower(mesh.bones[b].name) == "camerajoint") camera_ = static_cast<int>(b);
    }
    aim_bone_.assign(tree.nodes().size(), {});
    for (size_t i = 0; i < tree.nodes().size(); ++i) {
        for (const TreeNode::AimBone& a : tree.nodes()[i].aim) {
            auto it = mesh.bone_name_to_index.find(a.bone);
            int bone = it != mesh.bone_name_to_index.end() ? it->second : -1;
            if (bone < 0) {
                const std::string want = lower(a.bone);
                for (size_t b = 0; b < bones; ++b) {
                    if (lower(mesh.bones[b].name) == want) bone = static_cast<int>(b);
                }
            }
            aim_bone_[i].push_back(bone);
        }
    }
    // The eye bone looks along its +Z with -Y up and +X to the left; in the reference pose that is
    // the pawn looking straight ahead.
    const Quat4 ref = mesh.bones[static_cast<size_t>(eye_)].comp_ref_quat;
    fwd_ = ref.rotate(Vec3(0.0f, 0.0f, 1.0f));
    up_ = ref.rotate(Vec3(0.0f, -1.0f, 0.0f));
    right_ = ref.rotate(Vec3(-1.0f, 0.0f, 0.0f));
}

void PoseEvaluator::set_weapon_pose(const AnimSetAsset* weapon_set, const AnimSetAsset* common_set, const std::vector<int>& bones) {
    pose_bones_.clear();
    pose_rot_.clear();
    pose_pos_.clear();
    if (!mesh_ || !weapon_set || !common_set) return;
    const AnimSequenceAsset* own = weapon_set->find_sequence("weaponpose");
    const AnimSequenceAsset* common = common_set->find_sequence("weaponpose");
    if (!own || !common) return;
    const std::vector<int>& own_tracks = tracks(weapon_set);
    const std::vector<int>& common_tracks = tracks(common_set);
    for (int bone : bones) {
        if (bone < 0 || static_cast<size_t>(bone) >= mesh_->bones.size()) continue;
        const int to = own_tracks[static_cast<size_t>(bone)], tc = common_tracks[static_cast<size_t>(bone)];
        if (to < 0 || tc < 0 || static_cast<size_t>(to) >= own->tracks.size() || static_cast<size_t>(tc) >= common->tracks.size()) continue;
        const AnimTrack& a = own->tracks[static_cast<size_t>(to)];
        const AnimTrack& c = common->tracks[static_cast<size_t>(tc)];
        if (a.rotations.empty() || c.rotations.empty() || a.positions.empty() || c.positions.empty()) continue;
        // own = offset o common, in the parent bone's space.
        const Quat4 rot = Quat4::multiply(a.rotations[0], c.rotations[0].conjugate()).normalized();
        pose_bones_.push_back(bone);
        pose_rot_.push_back(rot);
        pose_pos_.push_back(a.positions[0] - rot.rotate(c.positions[0]));
    }
}

void PoseEvaluator::reference(Pose& out) const {
    const size_t bones = mesh_->bones.size();
    out.pos.resize(bones);
    out.rot.resize(bones);
    for (size_t b = 0; b < bones; ++b) {
        out.pos[b] = mesh_->bones[b].bind_pos;
        out.rot[b] = mesh_->bones[b].bind_quat;
    }
}

void PoseEvaluator::sample(const TreeNode& n, Pose& out) const {
    reference(out);
    const AnimSequenceAsset* seq = n.seq;
    if (!seq || seq->length <= 0.0f) return;
    const float u = std::clamp(n.time / seq->length, 0.0f, 1.0f);
    const std::vector<int>& track_of = tracks(n.seq_set ? n.seq_set : base_set_);
    for (size_t b = 0; b < out.pos.size(); ++b) {
        const int track = track_of[b];
        if (track < 0 || static_cast<size_t>(track) >= seq->tracks.size()) continue;
        const AnimTrack& tr = seq->tracks[static_cast<size_t>(track)];
        if (!tr.positions.empty()) {
            out.pos[b] = key_at(tr.positions, u, n.looping, [](const Vec3& a, const Vec3& c, float t) { return a + (c - a) * t; });
        }
        if (!tr.rotations.empty()) {
            out.rot[b] = key_at(tr.rotations, u, n.looping, [](const Quat4& a, const Quat4& c, float t) { return Quat4::slerp(a, c, t); });
        }
    }
    // Root motion: what the animation moves the root bone by moves the pawn instead (TdPawn.UseRootMotion).
    if (n.root_motion) out.pos[0] = mesh_->bones[0].bind_pos;
}

// AnimNodeAimOffset.GetBoneAtoms: each bone of the profile is turned and moved in the mesh's
// space, parents before children, and put back into its parent's space.
void PoseEvaluator::apply_aim(const TreeNode& n, size_t node_index, Pose& out) const {
    if (n.aim.empty() || (n.aim_x == 0.0f && n.aim_y == 0.0f)) return;
    const std::vector<int>& which = aim_bone_[node_index];
    const size_t bones = out.pos.size();
    std::vector<Vec3> cpos(bones);
    std::vector<Quat4> crot(bones);
    for (size_t b = 0; b < bones; ++b) {
        const int parent = mesh_->bones[b].parent_index;
        const bool root = b == 0 || parent < 0 || static_cast<size_t>(parent) >= b;
        const Vec3 ppos = root ? Vec3(0.0f, 0.0f, 0.0f) : cpos[static_cast<size_t>(parent)];
        const Quat4 prot = root ? Quat4() : crot[static_cast<size_t>(parent)];
        cpos[b] = ppos + prot.rotate(out.pos[b]);
        crot[b] = Quat4::multiply(prot, out.rot[b]).normalized();
        for (size_t k = 0; k < which.size(); ++k) {
            if (which[k] != static_cast<int>(b)) continue;
            Quat4 q;
            Vec3 t;
            // AnimNodeAimOffset.GetBoneAtoms: the aim over the profile's range, held to the poses it has.
            const float x = n.aim_x < 0.0f ? n.aim_x / n.aim_range_neg : n.aim_x / n.aim_range_pos;
            aim_offset(n.aim[k], std::clamp(x, -1.0f, 1.0f), n.aim_y, q, t);
            crot[b] = Quat4::multiply(q, crot[b]).normalized();
            cpos[b] += t;
            out.rot[b] = Quat4::multiply(prot.conjugate(), crot[b]).normalized();
            out.pos[b] = prot.conjugate().rotate(cpos[b] - ppos);
        }
    }
}

void PoseEvaluator::atoms(const AnimTree& tree, int index, Pose& out, size_t depth) const {
    if (index < 0) {
        reference(out);
        return;
    }
    const TreeNode& n = tree.nodes()[static_cast<size_t>(index)];
    if (n.kind == TreeNode::Kind::Sequence) {
        sample(n, out);
        return;
    }
    if (scratch_.size() <= depth) scratch_.resize(depth + 1);
    if (n.kind == TreeNode::Kind::PerBone) {
        // The source whole, and the target laid over the bones of its mask.
        atoms(tree, n.children.empty() ? -1 : n.children[0], out, depth + 1);
        const float weight = n.weight.size() > 1 ? n.weight[1] : 0.0f;
        if (weight > kZeroWeight && n.children.size() > 1 && n.children[1] >= 0) {
            Pose& target = scratch_[depth];
            atoms(tree, n.children[1], target, depth + 1);
            for (size_t b = 0; b < out.pos.size(); ++b) {
                const float w = weight * (b < n.bone_weight.size() ? n.bone_weight[b] : 1.0f);
                if (w <= kZeroWeight) continue;
                out.pos[b] = out.pos[b] + (target.pos[b] - out.pos[b]) * w;
                out.rot[b] = nlerp(out.rot[b], target.rot[b], w);
            }
        }
        return;
    }

    // Every other node blends the children that weigh something.
    float sum = 0.0f;
    for (size_t c = 0; c < n.children.size(); ++c) {
        if (c < n.weight.size() && n.weight[c] > kZeroWeight) sum += n.weight[c];
    }
    if (sum <= kZeroWeight) {
        reference(out);
    } else {
        bool first = true;
        for (size_t c = 0; c < n.children.size(); ++c) {
            if (c >= n.weight.size() || n.weight[c] <= kZeroWeight) continue;
            const float w = n.weight[c] / sum;
            if (first) {
                atoms(tree, n.children[c], out, depth + 1);
                first = false;
                if (w >= 1.0f - kZeroWeight) break;
                for (size_t b = 0; b < out.pos.size(); ++b) {
                    out.pos[b] = out.pos[b] * w;
                    out.rot[b] = Quat4(out.rot[b].x * w, out.rot[b].y * w, out.rot[b].z * w, out.rot[b].w * w);
                }
                continue;
            }
            Pose& other = scratch_[depth];
            atoms(tree, n.children[c], other, depth + 1);
            for (size_t b = 0; b < out.pos.size(); ++b) {
                out.pos[b] += other.pos[b] * w;
                const float s = dot4(out.rot[b], other.rot[b]) < 0.0f ? -w : w;
                out.rot[b] = Quat4(out.rot[b].x + other.rot[b].x * s, out.rot[b].y + other.rot[b].y * s, out.rot[b].z + other.rot[b].z * s,
                                   out.rot[b].w + other.rot[b].w * s);
            }
        }
        for (Quat4& q : out.rot) q = q.normalized();
    }
    apply_aim(n, static_cast<size_t>(index), out);
    // TdAnimNodeWeaponPoseOffset: the grip of the weapon in hand.
    if (grip_ && !pose_bones_.empty() && n.cls == "TdAnimNodeWeaponPoseOffset") {
        for (size_t k = 0; k < pose_bones_.size(); ++k) {
            const size_t b = static_cast<size_t>(pose_bones_[k]);
            out.rot[b] = Quat4::multiply(pose_rot_[k], out.rot[b]).normalized();
            out.pos[b] = pose_rot_[k].rotate(out.pos[b]) + pose_pos_[k];
        }
    }
}

void PoseEvaluator::evaluate(const AnimTree& tree, Pose& out, const Aim& aim, SkelControls* controls, const MeshPlace* place) const {
    if (!mesh_) return;
    int root = -1;
    for (size_t i = 0; i < tree.nodes().size(); ++i) {
        if (tree.nodes()[i].cls == "AnimTree") root = static_cast<int>(i);
    }
    atoms(tree, root, out, 0);
    // OneHandedRightShoulderOffset, on with a light weapon at the ready (TdPawn.UpdateWeaponSkelControls).
    const float shoulder = controls ? controls->shoulder_strength() : 0.0f;
    if (shoulder_right_ >= 0 && shoulder > 0.0f) {
        const size_t b = static_cast<size_t>(shoulder_right_);
        out.pos[b] += out.rot[b].rotate(aim.shoulder) * shoulder;
    }
    if (hips_ >= 0 && (aim.hips.x != 0.0f || aim.hips.y != 0.0f || aim.hips.z != 0.0f)) {
        // The hips hang off the root, which no animation turns.
        const size_t b = static_cast<size_t>(hips_);
        out.pos[b] += out.rot[0].conjugate().rotate(fwd_ * aim.hips.x + right_ * aim.hips.y + up_ * aim.hips.z);
    }
    // The rest of the tree's skeletal controls: the aim, the lazy springs, the hands and feet put
    // on the level.
    if (controls) {
        const float look = aim.look_yaw_deg * DEG2RAD;
        const Vec3 swan = (fwd_ * std::cos(look) + right_ * std::sin(look)) * aim.swan_forward - up_ * aim.swan_down;
        controls->apply(out, place ? *place : MeshPlace{}, swan);
    }
}

void PoseEvaluator::component_space(const Pose& pose, std::vector<Vec3>& pos, std::vector<Quat4>& rot) const {
    const size_t bones = pose.pos.size();
    pos.resize(bones);
    rot.resize(bones);
    for (size_t b = 0; b < bones; ++b) {
        const int parent = mesh_->bones[b].parent_index;
        if (b == 0 || parent < 0 || static_cast<size_t>(parent) >= b) {
            pos[b] = pose.pos[b];
            rot[b] = pose.rot[b];
        } else {
            pos[b] = pos[static_cast<size_t>(parent)] + rot[static_cast<size_t>(parent)].rotate(pose.pos[b]);
            rot[b] = Quat4::multiply(rot[static_cast<size_t>(parent)], pose.rot[b]).normalized();
        }
    }
}

ViewFrame PoseEvaluator::view(const std::vector<Vec3>& comp_pos, const std::vector<Quat4>& comp_rot, float view_pitch_deg,
                              float yaw_offset_deg, float swan_forward, float swan_down) const {
    ViewFrame v;
    const size_t eye = static_cast<size_t>(eye_);
    v.eye = comp_pos[eye];
    // CalcCamera: out_Location += SwanNeck1p.GetSwanNeckPos(the view's yaw): forward along the
    // way the view faces and straight down.
    {
        const float yaw = yaw_offset_deg * DEG2RAD;
        v.eye += (fwd_ * std::cos(yaw) + right_ * std::sin(yaw)) * swan_forward - up_ * swan_down;
    }
    v.eye_pawn = Vec3(v.eye.dot(fwd_), v.eye.dot(right_), v.eye.dot(up_));

    // TdPawn.GetCameraAnimation (native) and CalcCamera's Pitch += -Roll, Yaw += Pitch, Roll += -Yaw:
    // the camera bone's orientation as a view's. At rest the bone's axes are the mesh's (+X left,
    // -Y up, +Z forward), so the camera looks along the bone's Z with the bone's -X to its right
    // and -Y up; that frame, in the pawn's axes, read as a rotator (FMatrix::Rotator) is the pitch,
    // yaw and roll the animation adds. Through a disarm, where the camera pitches, turns and
    // rolls at once (36, 21 and 13 degrees), this is retail's camera to a degree; reading the
    // mesh-space rotator and swapping its angles, which is the same thing for a turn about one
    // axis, was 5 to 8 degrees out there. (Past straight down the pitch comes back and the yaw and
    // roll go half a turn: the same orientation a somersault's pitch would run on to.)
    {
        const Quat4& q = comp_rot[static_cast<size_t>(camera_)];
        const Vec3 x_axis = q.rotate(Vec3(1.0f, 0.0f, 0.0f));
        const Vec3 y_axis = q.rotate(Vec3(0.0f, 1.0f, 0.0f));
        const Vec3 z_axis = q.rotate(Vec3(0.0f, 0.0f, 1.0f));
        auto in_pawn = [&](const Vec3& m) { return Vec3(m.dot(fwd_), m.dot(right_), m.dot(up_)); };
        const Vec3 f = in_pawn(z_axis), r = in_pawn(x_axis) * -1.0f, u = in_pawn(y_axis) * -1.0f;
        const float pitch = std::atan2(f.z, std::sqrt(f.x * f.x + f.y * f.y));
        const float yaw = std::atan2(f.y, f.x);
        const Vec3 flat_y(-std::sin(yaw), std::cos(yaw), 0.0f);
        const float roll = std::atan2(u.dot(flat_y), r.dot(flat_y));
        v.anim_pitch = pitch * RAD2DEG;
        v.anim_yaw = yaw * RAD2DEG;
        v.anim_roll = roll * RAD2DEG;
    }

    // CalcCamera adds the animation's turn to the view rotation, angle by angle.
    const Rotator rot = Rotator::from_degrees(view_pitch_deg + v.anim_pitch, yaw_offset_deg + v.anim_yaw, v.anim_roll);
    auto to_comp = [&](const Vec3& p) { return fwd_ * p.x + right_ * p.y + up_ * p.z; };
    v.forward = to_comp(rot.forward());
    v.left = to_comp(rot.right()) * -1.0f;
    v.up = to_comp(rot.up());
    return v;
}

}  // namespace me::fp
