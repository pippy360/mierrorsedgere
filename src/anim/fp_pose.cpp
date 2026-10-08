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
        if (key == "spinexright") spine_right_ = static_cast<int>(b);
        if (key == "spinexleft") spine_left_ = static_cast<int>(b);
        if (key == "rightshoulder") shoulder_right_ = static_cast<int>(b);
        if (key == "hips") hips_ = static_cast<int>(b);
        if (key == "leftarm") arm_[0] = static_cast<int>(b);
        if (key == "leftforearm") forearm_[0] = static_cast<int>(b);
        if (key == "lefthand") hand_[0] = static_cast<int>(b);
        if (key == "rightarm") arm_[1] = static_cast<int>(b);
        if (key == "rightforearm") forearm_[1] = static_cast<int>(b);
        if (key == "righthand") hand_[1] = static_cast<int>(b);
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

// The arm from `bone` out is turned about that bone by `degrees` of pitch, in the mesh's space.
// TdSkelControlAim1p (native): the armed arm keeps its place in the view. In retail's frames a
// pistol and a rifle are at the same spot on the screen at every pitch from 75 degrees up to 75
// down, with the camera craned out over the feet by the swan neck or not. So the arm, from its
// spine bone out, is carried rigidly with the camera: turned by the view's pitch about the eye,
// and moved by what the swan neck moves the camera.
void PoseEvaluator::turn_arm(int bone, float degrees, const Vec3& shift, Pose& out) const {
    if (bone < 0 || (degrees == 0.0f && shift.length_sq() == 0.0f)) return;
    auto place = [&](int b, Vec3& pos, Quat4& rot) {
        // The bone's place and turn in the mesh's space, by its chain up to the root.
        std::vector<int> chain;
        for (int k = b; k >= 0; k = (k == 0 ? -1 : mesh_->bones[static_cast<size_t>(k)].parent_index)) chain.push_back(k);
        pos = Vec3(0.0f, 0.0f, 0.0f);
        rot = Quat4();
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            const size_t k = static_cast<size_t>(*it);
            pos = pos + rot.rotate(out.pos[k]);
            rot = Quat4::multiply(rot, out.rot[k]).normalized();
        }
    };
    const int up = mesh_->bones[static_cast<size_t>(bone)].parent_index;
    Vec3 parent_pos, eye_pos;
    Quat4 parent, eye_rot;
    place(up, parent_pos, parent);
    place(eye_, eye_pos, eye_rot);
    // About the pawn's side axis, forward towards up.
    const Vec3 axis = fwd_.cross(up_).normalized();
    const float half = degrees * DEG2RAD * 0.5f;
    const Quat4 turn(axis.x * std::sin(half), axis.y * std::sin(half), axis.z * std::sin(half), std::cos(half));
    const size_t i = static_cast<size_t>(bone);
    const Vec3 at = parent_pos + parent.rotate(out.pos[i]);
    const Vec3 moved = eye_pos + turn.rotate(at - eye_pos) + shift;
    out.pos[i] = parent.conjugate().rotate(moved - parent_pos);
    out.rot[i] = Quat4::multiply(parent.conjugate(), Quat4::multiply(turn, Quat4::multiply(parent, out.rot[i]))).normalized();
}

// A hand moved by `shift` (in the mesh's space) with the arm following: the upper arm and the
// forearm keep their lengths, the elbow stays in the plane it is in, and the hand keeps its turn.
void PoseEvaluator::reach_hand(int side, const Vec3& shift, Pose& out) const {
    const int upper = arm_[side], lower = forearm_[side], hand = hand_[side];
    if (upper < 0 || lower < 0 || hand < 0 || shift.length_sq() < 1e-6f) return;
    if (mesh_->bones[static_cast<size_t>(lower)].parent_index != upper || mesh_->bones[static_cast<size_t>(hand)].parent_index != lower) return;
    std::vector<Vec3> pos;
    std::vector<Quat4> rot;
    component_space(out, pos, rot);
    const size_t u = static_cast<size_t>(upper), l = static_cast<size_t>(lower), h = static_cast<size_t>(hand);
    const Vec3 a = pos[u], b = pos[l], c = pos[h];
    const float l1 = (b - a).length(), l2 = (c - b).length();
    if (l1 < 1e-3f || l2 < 1e-3f) return;
    const Vec3 to = c + shift - a;
    const float d = std::clamp(to.length(), std::fabs(l1 - l2) + 0.1f, l1 + l2 - 0.1f);
    const Vec3 dir = to.normalized();
    Vec3 pole = (b - a) - dir * (b - a).dot(dir);
    if (pole.length_sq() < 1e-6f) pole = up_ * -1.0f;
    pole = pole.normalized();
    const float along = (l1 * l1 - l2 * l2 + d * d) / (2.0f * d);
    const float out_of_line = std::sqrt(std::max(0.0f, l1 * l1 - along * along));
    const Vec3 elbow = a + dir * along + pole * out_of_line;
    const Vec3 target = a + dir * d;
    auto between = [](const Vec3& from, const Vec3& onto) {
        const Vec3 f = from.normalized(), t = onto.normalized();
        const Vec3 axis = f.cross(t);
        return Quat4(axis.x, axis.y, axis.z, 1.0f + f.dot(t)).normalized();
    };
    const Quat4 q1 = between(b - a, elbow - a);
    const Quat4 upper_rot = Quat4::multiply(q1, rot[u]).normalized();
    const Quat4 q2 = between(q1.rotate(c - b), target - elbow);
    const Quat4 lower_rot = Quat4::multiply(q2, Quat4::multiply(q1, rot[l])).normalized();
    const int up_parent = mesh_->bones[u].parent_index;
    const Quat4 parent = up_parent >= 0 ? rot[static_cast<size_t>(up_parent)] : Quat4();
    out.rot[u] = Quat4::multiply(parent.conjugate(), upper_rot).normalized();
    out.rot[l] = Quat4::multiply(upper_rot.conjugate(), lower_rot).normalized();
    out.rot[h] = Quat4::multiply(lower_rot.conjugate(), rot[h]).normalized();
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

void PoseEvaluator::evaluate(const AnimTree& tree, Pose& out, const Aim& aim) const {
    if (!mesh_) return;
    int root = -1;
    for (size_t i = 0; i < tree.nodes().size(); ++i) {
        if (tree.nodes()[i].cls == "AnimTree") root = static_cast<int>(i);
    }
    atoms(tree, root, out, 0);
    // The aim controls: the weapon arm (both, with a two-handed weapon) follows the view's pitch.
    if (shoulder_right_ >= 0 && aim.right > 0.0f) {
        const size_t b = static_cast<size_t>(shoulder_right_);
        out.pos[b] += out.rot[b].rotate(aim.shoulder) * aim.right;
    }
    if (hips_ >= 0 && (aim.hips.x != 0.0f || aim.hips.y != 0.0f || aim.hips.z != 0.0f)) {
        // The hips hang off the root, which no animation turns.
        const size_t b = static_cast<size_t>(hips_);
        out.pos[b] += out.rot[0].conjugate().rotate(fwd_ * aim.hips.x + right_ * aim.hips.y + up_ * aim.hips.z);
    }
    const Vec3 swan = fwd_ * aim.swan_forward - up_ * aim.swan_down;
    turn_arm(spine_right_, aim.pitch_deg * aim.right, swan * aim.right, out);
    turn_arm(spine_left_, aim.pitch_deg * aim.left, swan * aim.left, out);
    // Against a wall the hands are set on it (TdPawn.AgainstWallLeftHand / RightHand and the limb
    // controls, native; the skeleton has an IK bone for the left hand only, LeftHand_GameIK). In
    // retail's frames at a flat wall the left fingers are where they would be with the wrist 15 uu
    // out from her middle and 17 under the eye, on the wall: 4 further in and 6 lower than the
    // animation alone has it, which also has both hands 3 uu inside the wall. The right hand is
    // where the animation has it. At a fence she stood further from, the left hand reaches out to
    // it. So: the left wrist goes to the point of the wall in front of its shoulder (where the
    // controller found it), a hand's thickness short of it; the right is only brought out to the
    // wall's surface; the arms bend or reach to suit and the hands keep their turn.
    if ((aim.wall_left > 0.0f && aim.wall_ahead_left >= 0.0f) || (aim.wall_right > 0.0f && aim.wall_ahead_right >= 0.0f)) {
        constexpr float kShoulderOut = 15.0f, kPalm = 2.0f, kWristBelowTrace = 2.2f, kMeshOriginBelowFeet = 4.0f;
        std::vector<Vec3> pos;
        std::vector<Quat4> rot;
        component_space(out, pos, rot);
        const Vec3 wrist[2] = {hand_[0] >= 0 ? pos[static_cast<size_t>(hand_[0])] : Vec3(0.0f, 0.0f, 0.0f),
                               hand_[1] >= 0 ? pos[static_cast<size_t>(hand_[1])] : Vec3(0.0f, 0.0f, 0.0f)};
        for (int side = 0; side < 2; ++side) {
            const float weight = side == 0 ? aim.wall_left : aim.wall_right;
            const float ahead = side == 0 ? aim.wall_ahead_left : aim.wall_ahead_right;
            if (weight <= 0.0f || ahead < 0.0f || hand_[side] < 0) continue;
            const Vec3 point = fwd_ * (ahead - kPalm) + right_ * -kShoulderOut + up_ * (aim.wall_height - kWristBelowTrace + kMeshOriginBelowFeet);
            const Vec3 target = side == 0 ? point : wrist[side] + fwd_ * ((ahead - kPalm) - wrist[side].dot(fwd_));
            Vec3 shift = (target - wrist[side]) * weight;
            const float far = shift.length();
            if (far > 20.0f) shift = shift * (20.0f / far);
            reach_hand(side, shift, out);
        }
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

    // TdPawn.GetCameraAnimation (native): the camera bone's rotation in the mesh's space, as
    // FMatrix::Rotator reads it. The mesh's axes are +X left, -Y up, +Z forward, so the rotator's
    // roll is the view's pitch, its pitch the view's yaw and its yaw the view's roll, which is the
    // swizzle CalcCamera does: Pitch += -Roll, Yaw += Pitch, Roll += -Yaw. Read this way the pitch
    // keeps going past straight down, which is how a roll turns the view right over.
    {
        const Quat4& q = comp_rot[static_cast<size_t>(camera_)];
        const Vec3 x_axis = q.rotate(Vec3(1.0f, 0.0f, 0.0f));
        const Vec3 y_axis = q.rotate(Vec3(0.0f, 1.0f, 0.0f));
        const Vec3 z_axis = q.rotate(Vec3(0.0f, 0.0f, 1.0f));
        const float pitch = std::atan2(x_axis.z, std::sqrt(x_axis.x * x_axis.x + x_axis.y * x_axis.y));
        const float yaw = std::atan2(x_axis.y, x_axis.x);
        const Vec3 flat_y(-std::sin(yaw), std::cos(yaw), 0.0f);
        const float roll = std::atan2(z_axis.dot(flat_y), y_axis.dot(flat_y));
        v.anim_pitch = -roll * RAD2DEG;
        v.anim_yaw = pitch * RAD2DEG;
        v.anim_roll = -yaw * RAD2DEG;
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
