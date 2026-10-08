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

void PoseEvaluator::init(const SkeletalMeshAsset& mesh, const AnimSetAsset& set, const AnimTree& tree) {
    mesh_ = &mesh;
    const size_t bones = mesh.bones.size();
    track_of_.assign(bones, -1);
    for (size_t b = 0; b < bones; ++b) {
        const std::string key = mesh.bones[b].name_lower.empty() ? lower(mesh.bones[b].name) : mesh.bones[b].name_lower;
        auto it = set.bone_to_track.find(key);
        if (it != set.bone_to_track.end()) track_of_[b] = it->second;
        if (key == "eyejoint") eye_ = static_cast<int>(b);
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
    for (size_t b = 0; b < out.pos.size(); ++b) {
        const int track = track_of_[b];
        if (track < 0 || static_cast<size_t>(track) >= seq->tracks.size()) continue;
        const AnimTrack& tr = seq->tracks[static_cast<size_t>(track)];
        if (!tr.positions.empty()) {
            out.pos[b] = key_at(tr.positions, u, n.looping, [](const Vec3& a, const Vec3& c, float t) { return a + (c - a) * t; });
        }
        if (!tr.rotations.empty()) {
            out.rot[b] = key_at(tr.rotations, u, n.looping, [](const Quat4& a, const Quat4& c, float t) { return Quat4::slerp(a, c, t); });
        }
    }
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
            aim_offset(n.aim[k], n.aim_x, n.aim_y, q, t);
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
}

void PoseEvaluator::evaluate(const AnimTree& tree, Pose& out) const {
    if (!mesh_) return;
    int root = -1;
    for (size_t i = 0; i < tree.nodes().size(); ++i) {
        if (tree.nodes()[i].cls == "AnimTree") root = static_cast<int>(i);
    }
    atoms(tree, root, out, 0);
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
                              float yaw_offset_deg) const {
    ViewFrame v;
    const size_t eye = static_cast<size_t>(eye_);
    v.eye = comp_pos[eye];
    // SwanNeck1p.GetSwanNeckPos (native): looking down past TdMove.SwanNeckEnableAtPitch (15 degrees)
    // the camera cranes forward and down off the eye, along the way the view faces, so that looking
    // at her feet she sees them and not her own chest. Measured on retail standing still at five
    // pitches (docs/FIRST_PERSON_ANIMATION_RE.md): with a = 90 degrees x (pitch - 15) / 75,
    // forward 20.1 sin a + 4.1 (1 - cos a) and down 18.6 (1 - cos a), which is 24.2 and 18.6
    // looking straight down.
    if (view_pitch_deg < -15.0f) {
        const float a = std::min((-view_pitch_deg - 15.0f) / 75.0f, 1.0f) * (PI * 0.5f);
        const float forward = 20.1f * std::sin(a) + 4.1f * (1.0f - std::cos(a));
        const float down = 18.6f * (1.0f - std::cos(a));
        const float yaw = yaw_offset_deg * DEG2RAD;
        v.eye += (fwd_ * std::cos(yaw) + right_ * std::sin(yaw)) * forward - up_ * down;
    }
    v.eye_pawn = Vec3(v.eye.dot(fwd_), v.eye.dot(right_), v.eye.dot(up_));

    // Where the animation has the eye looking, against the pawn: its axes in the pawn's.
    auto to_pawn = [&](const Vec3& c) { return Vec3(c.dot(fwd_), c.dot(right_), c.dot(up_)); };
    const Vec3 ax = to_pawn(comp_rot[eye].rotate(Vec3(0.0f, 0.0f, 1.0f)));
    const Vec3 ay = to_pawn(comp_rot[eye].rotate(Vec3(-1.0f, 0.0f, 0.0f)));
    const Vec3 az = to_pawn(comp_rot[eye].rotate(Vec3(0.0f, -1.0f, 0.0f)));
    // FMatrix::Rotator.
    v.anim_pitch = std::atan2(ax.z, std::sqrt(ax.x * ax.x + ax.y * ax.y)) * RAD2DEG;
    v.anim_yaw = std::atan2(ax.y, ax.x) * RAD2DEG;
    const Vec3 flat_right = Rotator::from_degrees(v.anim_pitch, v.anim_yaw, 0.0f).right();
    v.anim_roll = std::atan2(az.dot(flat_right), ay.dot(flat_right)) * RAD2DEG;

    // CalcCamera adds the animation's turn to the view rotation, angle by angle.
    const Rotator rot = Rotator::from_degrees(view_pitch_deg + v.anim_pitch, yaw_offset_deg + v.anim_yaw, v.anim_roll);
    auto to_comp = [&](const Vec3& p) { return fwd_ * p.x + right_ * p.y + up_ * p.z; };
    v.forward = to_comp(rot.forward());
    v.left = to_comp(rot.right()) * -1.0f;
    v.up = to_comp(rot.up());
    return v;
}

}  // namespace me::fp
