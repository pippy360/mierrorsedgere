#include "fp_controls.hpp"

#include "fp_pose.hpp"

#include <algorithm>
#include <cmath>

namespace me::fp {

namespace {

constexpr float kZero = 0.00001f;  // ZERO_ANIMWEIGHT_THRESH
constexpr float kUnitsPerDegree = 65536.0f / 360.0f;
constexpr float kRadiansPerUnit = 6.28318530718f / 65536.0f;

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

int units(float degrees) { return static_cast<int>(std::lround(degrees * kUnitsPerDegree)); }

// FRotator::NormalizeAxis: an angle in -32768 .. 32767.
int norm_axis(int a) {
    a &= 0xFFFF;
    return a > 0x7FFF ? a - 0x10000 : a;
}

Quat4 about(float x, float y, float z, float angle) {
    const float s = std::sin(angle * 0.5f);
    return Quat4(x * s, y * s, z * s, std::cos(angle * 0.5f));
}

// FRotationMatrix(Pitch, Yaw, Roll) as a turn. Its rows are where it takes the axes: X to
// (cp cy, cp sy, sp), so the yaw is a turn about +Z, the pitch one about -Y and the roll one about
// -X, the roll first.
Quat4 rotator_quat(int pitch, int yaw, int roll) {
    return Quat4::multiply(about(0.0f, 0.0f, 1.0f, static_cast<float>(yaw) * kRadiansPerUnit),
                           Quat4::multiply(about(0.0f, 1.0f, 0.0f, -static_cast<float>(pitch) * kRadiansPerUnit),
                                           about(1.0f, 0.0f, 0.0f, -static_cast<float>(roll) * kRadiansPerUnit)))
        .normalized();
}

// FMatrix::Rotator of a turn, in radians.
void rotator_of(const Quat4& q, float& pitch, float& yaw, float& roll) {
    const Vec3 x = q.rotate(Vec3(1.0f, 0.0f, 0.0f)), y = q.rotate(Vec3(0.0f, 1.0f, 0.0f)), z = q.rotate(Vec3(0.0f, 0.0f, 1.0f));
    pitch = std::atan2(x.z, std::sqrt(x.x * x.x + x.y * x.y));
    yaw = std::atan2(x.y, x.x);
    const Vec3 flat_y(-std::sin(yaw), std::cos(yaw), 0.0f);
    roll = std::atan2(z.dot(flat_y), y.dot(flat_y));
}

// The turn that takes the axes to `ax`, `ay`, `az`.
Quat4 quat_of_axes(const Vec3& ax, const Vec3& ay, const Vec3& az) {
    const float m00 = ax.x, m10 = ax.y, m20 = ax.z, m01 = ay.x, m11 = ay.y, m21 = ay.z, m02 = az.x, m12 = az.y, m22 = az.z;
    const float trace = m00 + m11 + m22;
    Quat4 q;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q = Quat4((m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s);
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q = Quat4(0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s);
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q = Quat4((m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s);
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q = Quat4((m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s);
    }
    return q.normalized();
}

Vec3 safe_normal(const Vec3& v) {
    const float len2 = v.length_sq();
    if (len2 < 1e-8f) return Vec3(0.0f, 0.0f, 0.0f);
    return v * (1.0f / std::sqrt(len2));
}

// FInterpTo / VInterpTo.
float interp_to(float current, float target, float dt, float speed) {
    if (speed <= 0.0f) return target;
    const float dist = target - current;
    if (dist * dist < 1e-8f) return target;
    return current + dist * std::clamp(dt * speed, 0.0f, 1.0f);
}

Vec3 vinterp_to(const Vec3& current, const Vec3& target, float dt, float speed) {
    if (speed <= 0.0f) return target;
    const Vec3 dist = target - current;
    if (dist.length_sq() < 1e-4f) return target;
    return current + dist * std::clamp(dt * speed, 0.0f, 1.0f);
}

Quat4 nlerp(const Quat4& a, Quat4 b, float t) {
    if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0.0f) b = b.negated();
    return Quat4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t).normalized();
}

// BuildMatrixFromVectors: the bone's `bone_axis` along `dir` and its `joint_axis` along `normal`
// (EAxis: 1 X, 2 Y, 4 Z), the third axis by the right-hand formula.
Quat4 limb_turn(int bone_axis, const Vec3& dir, int joint_axis, const Vec3& normal) {
    Vec3 axis[3];
    const int a = bone_axis == 1 ? 0 : bone_axis == 2 ? 1 : 2;
    const int b = joint_axis == 1 ? 0 : joint_axis == 2 ? 1 : 2;
    if (a == b) return Quat4();
    axis[a] = dir;
    axis[b] = normal;
    const int c = 3 - a - b;
    // Z = X x Y, X = Y x Z, Y = Z x X.
    axis[c] = axis[(c + 1) % 3].cross(axis[(c + 2) % 3]);
    return quat_of_axes(axis[0], axis[1], axis[2]);
}

}  // namespace

// SetSkelControlStrength: a change of target, or a shorter blend than the one running, starts
// the blend; no blend time sets it at once.
void ControlStrength::set(float strength, float blend_time) {
    strength = std::clamp(strength, 0.0f, 1.0f);
    blend_time = std::max(blend_time, 0.0f);
    if (target == strength && to_go <= blend_time) return;
    target = strength;
    to_go = blend_time;
    if (blend_time <= 0.0f) {
        value = strength;
        to_go = 0.0f;
    }
}

// USkelControlBase::TickSkelControl: straight to the target over what is left of the blend.
void ControlStrength::tick(float dt) {
    if (to_go == 0.0f && value == target) return;
    if (to_go > dt && to_go != 0.0f) {
        value += (target - value) / to_go * dt;
        to_go -= dt;
    } else {
        value = target;
        to_go = 0.0f;
    }
}

SkelControls::AimMode SkelControls::move_aim_mode(EMovement move) {
    switch (move) {
        case EMovement::MOVE_Snatch:
        case EMovement::MOVE_Balance:
        case EMovement::MOVE_Barge:
        case EMovement::MOVE_Climb:
        case EMovement::MOVE_Grabbing:
        case EMovement::MOVE_GrabPullUp:
        case EMovement::MOVE_GrabTransfer:
        case EMovement::MOVE_IntoClimb:
        case EMovement::MOVE_IntoGrab:
        case EMovement::MOVE_IntoZipLine:
        case EMovement::MOVE_ZipLine:
        case EMovement::MOVE_Swing:
        case EMovement::MOVE_WallClimbing:
        case EMovement::MOVE_MeleeAir:      // TdMove_MeleeBase's
        case EMovement::MOVE_MeleeSlide:
        case EMovement::MOVE_MeleeWallrun:
            return kAimNoHands;
        case EMovement::MOVE_LayOnGround:
        case EMovement::MOVE_SkillRoll:
        case EMovement::MOVE_WallClimb180TurnJump:
        case EMovement::MOVE_WallClimbDodgeJump:
            return kAimRight;
        case EMovement::MOVE_Melee:
        case EMovement::MOVE_MeleeCrouch:
        case EMovement::MOVE_Vertigo:
            return kAimTwoHanded;
        default:
            return kAimDefault;
    }
}

// TdPawn.GetAimMode (native). `for_aim` is its argument: the aim controls pass true, the lazy
// springs false.
SkelControls::AimMode SkelControls::aim_mode(bool for_aim) const {
    const AimMode move = move_aim_mode(in_.movement);
    if (move != kAimDefault) return move;
    const bool heavy = in_.armed && in_.heavy_weapon;
    constexpr int kRelaxed = 1, kReady = 2, kReload = 3, kThrowing = 4;
    if (for_aim) {
        if (in_.weapon_state == kReload) return kAimTwoHanded;
        if (in_.walking_state > kWasWalk) return kAimTwoHanded;  // jogging and faster, armed or not
        if (heavy) return kAimTwoHanded;
        if (in_.weapon_state == kReady || in_.weapon_state == kThrowing) return kAimRight;
        return kAimDefault;
    }
    AimMode mode = kAimDefault;
    if (in_.against_wall == 1 || heavy || in_.weapon_state == kReload || in_.movement == EMovement::MOVE_Vertigo) mode = kAimTwoHanded;
    else if (in_.weapon_state == kReady || in_.weapon_state == kRelaxed) mode = kAimRight;
    if (in_.against_wall == 2) return kAimTwoHanded;
    if (in_.against_wall == 3) return mode == kAimTwoHanded ? kAimTwoHanded : kAimRight;
    return mode;
}

void SkelControls::init(const SkeletalMeshAsset& mesh, const Vec3& fwd, const Vec3& right, const Vec3& up) {
    mesh_ = &mesh;
    fwd_ = fwd;
    right_ = right;
    up_ = up;
    auto find = [&](const char* name) {
        for (size_t b = 0; b < mesh.bones.size(); ++b) {
            if (lower(mesh.bones[b].name) == name) return static_cast<int>(b);
        }
        return -1;
    };
    spine_ = find("spinex");
    spine_side_[0] = find("spinexleft");
    spine_side_[1] = find("spinexright");
    hand_[0] = find("lefthand");
    hand_[1] = find("righthand");
    eye_ = find("eyejoint");
    ik_bone_ = find("lefthand_gameik");
    foot_[0].bone = find("leftfoot");
    foot_[1].bone = find("rightfoot");
    reset();
}

void SkelControls::reset() {
    // The controls as AT_C1P saves them. SpringAxis / RandomAxis: 0 yaw, 1 pitch, 2 roll.
    auto spring = [&](int i, int bone, int affected, int source, int limit, float interpolate, float multiplier, bool by_velocity) {
        Spring s;
        s.bone = bone;
        s.affected = affected;
        s.source = source;
        s.min_angle = -limit;
        s.max_angle = limit;
        s.interpolate = interpolate;
        s.multiplier = multiplier;
        s.by_velocity = by_velocity;
        springs_[i] = s;
    };
    spring(0, spine_, 1, 0, 1000, 0.2f, -0.1f, false);      // WeaponYaw
    spring(1, spine_, 2, 1, 1000, 0.25f, 0.08f, false);     // WeaponPitch
    spring(2, spine_, 0, 0, 1000, 0.15f, 0.05f, false);     // WeaponRoll
    spring(3, hand_[1], 1, 1, 1000, 0.15f, -0.25f, false);  // RightHandPitch
    spring(4, hand_[1], 0, 0, 1000, 0.07f, 0.2f, false);    // RightHandYaw
    spring(5, hand_[1], 2, 0, 1000, 0.1f, 0.2f, false);     // RightHandRoll
    spring(6, eye_, 0, 0, 8192, 0.2f, -0.15f, true);        // CameraRoll
    aim_[0] = aim_[1] = ControlStrength();
    sway_[0] = Sway();
    sway_[0].axis = 2;
    sway_[0].min_angle = -500;
    sway_[0].max_angle = 1000;
    sway_[0].interpolate = 0.1f;
    sway_[0].until = 0.1404f;
    sway_[0].current = 347;
    sway_[0].target = 287;
    sway_[1] = Sway();
    sway_[1].axis = 0;
    sway_[1].min_angle = -2000;
    sway_[1].max_angle = 1000;
    sway_[1].interpolate = 0.1f;
    sway_[1].until = 0.1404f;
    sway_[1].current = -691;
    sway_[1].target = -777;
    sway_strength_ = 0.0f;
    recoil_ = shoulder_ = local_ik_ = ControlStrength();
    recoil_x_ = -2.435613f;
    recoil_delay_ = 0.0f;
    world_ik_[0] = world_ik_[1] = ControlStrength();
    wall_effector_[0] = wall_target_[0] = Vec3(5.0f, -154.75f, 25.057079f);
    wall_effector_[1] = wall_target_[1] = Vec3(-27.059465f, -147.75f, 10.0f);
    for (Foot& f : foot_) {
        f.strength = ControlStrength();
        f.reset = true;
        f.interpolated = 0.0f;
    }
    foot_[0].invert_bone = true;
    foot_[0].joint_target = Vec3(0.0f, 0.0f, -8.98833f);
    foot_[1].invert_bone = false;
    foot_[1].joint_target = Vec3(0.0f, 0.0f, 0.0f);
    in_ = Input();
    seed_ = 1;
    started_ = false;
}

// The C runtime's rand(), which TdSkelControlRandom draws from.
int SkelControls::rand15() {
    seed_ = seed_ * 214013u + 2531011u;
    return static_cast<int>((seed_ >> 16) & 0x7FFF);
}

void SkelControls::tick(const Input& in) {
    in_ = in;
    const float dt = in.dt;
    const float speed = std::sqrt(in.velocity.x * in.velocity.x + in.velocity.y * in.velocity.y);
    const bool heavy = in.armed && in.heavy_weapon;
    const int source[3] = {norm_axis(units(in.view_yaw_deg)), norm_axis(units(in.view_pitch_deg)), 0};

    // TdSkelControlLazySpring::TickSkelControl. The control keeps an angle that follows one of the
    // view's (a fraction dt / InterpolateTime of the way a frame, a unit at least), and turns its
    // bone by how far that is behind, times SpringMultiplier. The ones that are not scaled by
    // speed (the weapon's and the hand's) are off in a move whose aim mode is MAM_NoHands.
    const bool hands = aim_mode(false) != kAimNoHands;
    for (Spring& s : springs_) {
        const bool on = s.by_velocity || hands;
        if (on) {
            if (s.strength.value <= 0.0f) {
                s.lazy = source[s.source];
                s.strength.set(1.0f, 0.1f);
            }
        } else if (s.strength.value >= 1.0f) {
            s.strength.set(0.0f, 0.1f);
        }
        if (!started_) s.lazy = source[s.source];
        if (s.strength.value > kZero) {
            const int delta = norm_axis(source[s.source] - s.lazy);
            const float magnitude = static_cast<float>(std::abs(delta));
            if (delta != 0) {
                const float step = std::min(std::max(dt / std::max(s.interpolate, 0.03f) * magnitude, 1.0f), magnitude);
                s.lazy += static_cast<int>(delta < 0 ? -step : step);
            }
            int lag = norm_axis(source[s.source] - s.lazy);
            if (s.by_velocity) lag = static_cast<int>(static_cast<float>(lag) * std::clamp(std::max(100.0f, speed) / 400.0f, 0.0f, 1.0f));
            const float multiplier = heavy ? s.multiplier * 0.5f : s.multiplier;  // HeavyWeaponModifier
            s.angle = std::clamp(static_cast<int>(static_cast<float>(lag) * multiplier), s.min_angle, s.max_angle);
        }
        s.strength.tick(dt);
    }
    started_ = true;

    // TdSkelControlAim1p::TickSkelControl: the right arm's with MAM_Right or MAM_TwoHanded, the
    // left's with MAM_Left or MAM_TwoHanded, over half a second (a tenth in a melee).
    {
        const AimMode mode = aim_mode(true);
        const float want[2] = {mode == kAimLeft || mode == kAimTwoHanded ? 1.0f : 0.0f, mode == kAimRight || mode == kAimTwoHanded ? 1.0f : 0.0f};
        const float blend = in.movement == EMovement::MOVE_Melee || in.movement == EMovement::MOVE_MeleeCrouch ? 0.1f : 0.5f;
        for (int side = 0; side < 2; ++side) {
            if (aim_[side].value != want[side]) aim_[side].set(want[side], blend);
            aim_[side].tick(dt);
        }
    }

    // TdSkelControlRandom::TickSkelControl: a new angle every Frequency seconds, scaled by speed,
    // none with a heavy weapon or a weapon at the ready; the bone goes 3 dt of the way to it a frame.
    for (Sway& w : sway_) {
        if (w.until <= 0.0f) {
            w.target = w.min_angle + rand15() % (w.max_angle - w.min_angle + 1);
            w.target = static_cast<int>(static_cast<float>(w.target) * std::clamp(speed / 400.0f, 0.0f, 1.0f));
            if (heavy || in.weapon_state == 2) w.target = 0;
            w.until = w.frequency;
        } else {
            w.until -= dt;
        }
        const float a = std::min(w.interpolate * dt * 30.0f, 1.0f);
        w.current = norm_axis(w.current + static_cast<int>(static_cast<float>(norm_axis(w.target - w.current)) * a));
    }
    sway_strength_ = std::min(in.walking_node_weight, 1.0f);

    // TdPawn.UpdateWeaponSkelControls: the recoil and the shoulder's offset with a light weapon at the ready.
    {
        const bool ready_light = in.armed && !in.heavy_weapon && in.weapon_state == 2;
        recoil_.set(ready_light ? 1.0f : 0.0f, 0.2f);
        shoulder_.set(ready_light ? 1.0f : 0.0f, 0.2f);
        // TdSkelControlRecoil.AddImpulse(RecoilAmount, RecoilRecoverTime, MinRecoil, MaxRecoil).
        if (in.fired) {
            recoil_x_ = std::clamp(recoil_x_ + 0.8f, 0.0f, 6.0f);
            recoil_delay_ = 0.1f;
        }
        recoil_.tick(dt);
        shoulder_.tick(dt);
        if (recoil_delay_ > 0.0f) recoil_delay_ -= dt;
        else if (recoil_x_ > 0.0f) recoil_x_ -= std::max(15.0f * recoil_x_ * dt, 0.05f);
    }

    // TdAnimNodeIKEffectorController::TickAnim: the left hand on the weapon's IK bone whenever she
    // holds a weapon that is not a light one and is not throwing it (SetSkelControlActive, 0.05 s).
    {
        const float want = in.armed && in.heavy_weapon && in.weapon_state != 4 ? 1.0f : 0.0f;
        local_ik_.target = want;
        local_ik_.to_go = 0.05f * std::fabs(want - local_ik_.value);
        local_ik_.tick(dt);
    }

    for (int side = 0; side < 2; ++side) {
        world_ik_[side].set(in.hand_ik[side] ? 1.0f : 0.0f, in.hand_ik_blend[side]);
        if (in.hand_ik[side]) world_ik_target_[side] = in.hand_ik_target[side];
        world_ik_[side].tick(dt);
    }
    for (Foot& f : foot_) {
        if (in.foot_placement && f.strength.value <= 0.0f && f.strength.target <= 0.0f) f.reset = true;
        f.strength.set(in.foot_placement ? 1.0f : 0.0f, in.foot_blend);
        f.strength.tick(dt);
    }
}

Vec3 SkelControls::to_comp(const MeshPlace& place, const Vec3& world) const {
    const Vec3 d = world - place.origin;
    return fwd_ * d.dot(place.forward) + right_ * d.dot(place.right) + up_ * d.z;
}

Vec3 SkelControls::to_world(const MeshPlace& place, const Vec3& comp) const {
    return place.origin + place.forward * comp.dot(fwd_) + place.right * comp.dot(right_) + Vec3(0.0f, 0.0f, comp.dot(up_));
}

void SkelControls::refresh(const Pose& pose) {
    const size_t bones = pose.pos.size();
    cpos_.resize(bones);
    crot_.resize(bones);
    for (size_t b = 0; b < bones; ++b) {
        const int parent = mesh_->bones[b].parent_index;
        if (b == 0 || parent < 0 || static_cast<size_t>(parent) >= b) {
            cpos_[b] = pose.pos[b];
            crot_[b] = pose.rot[b];
        } else {
            cpos_[b] = cpos_[static_cast<size_t>(parent)] + crot_[static_cast<size_t>(parent)].rotate(pose.pos[b]);
            crot_[b] = Quat4::multiply(crot_[static_cast<size_t>(parent)], pose.rot[b]).normalized();
        }
    }
}

// ApplyControllersForBoneIndex: the bone's new place in the component's space, put back into its
// parent's and blended with what it had by the control's strength.
void SkelControls::blend_in(Pose& pose, int bone, const Vec3& pos, const Quat4& rot, float strength) {
    const size_t b = static_cast<size_t>(bone);
    const int parent = mesh_->bones[b].parent_index;
    const bool root = b == 0 || parent < 0 || static_cast<size_t>(parent) >= b;
    const Vec3 ppos = root ? Vec3(0.0f, 0.0f, 0.0f) : cpos_[static_cast<size_t>(parent)];
    const Quat4 prot = root ? Quat4() : crot_[static_cast<size_t>(parent)];
    const Quat4 local_rot = Quat4::multiply(prot.conjugate(), rot).normalized();
    const Vec3 local_pos = prot.conjugate().rotate(pos - ppos);
    if (strength >= 1.0f - kZero) {
        pose.rot[b] = local_rot;
        pose.pos[b] = local_pos;
    } else {
        pose.rot[b] = nlerp(pose.rot[b], local_rot, strength);
        pose.pos[b] = pose.pos[b] + (local_pos - pose.pos[b]) * strength;
    }
    refresh(pose);
}

// SkelControlSingleBone with a rotation in the bone's own space.
void SkelControls::turn_local(Pose& pose, int bone, const Quat4& turn, float strength) {
    if (bone < 0 || strength <= kZero) return;
    const size_t b = static_cast<size_t>(bone);
    blend_in(pose, bone, cpos_[b], Quat4::multiply(crot_[b], turn).normalized(), strength);
}

// USkelControlLimb::CalculateNewBoneTransforms: `bone` (a hand, a foot) is put at `desired` and
// its two parents turned to reach, the joint in the plane of the limb's root, `desired` and
// `joint_target`. `end_rot`: bTakeRotationFromEffectorSpace. `keep_upper_roll`:
// TdSkelControlLimb.bDisableRotationAdjustment.
bool SkelControls::limb(Pose& pose, int bone, const Vec3& desired, const Vec3& joint_target, int bone_axis, int joint_axis, bool invert_bone,
                        const Quat4* end_rot, bool keep_upper_roll, float strength) {
    if (bone < 0 || strength <= kZero) return false;
    const int lower_bone = mesh_->bones[static_cast<size_t>(bone)].parent_index;
    if (lower_bone <= 0) return false;
    const int upper_bone = mesh_->bones[static_cast<size_t>(lower_bone)].parent_index;
    if (upper_bone <= 0) return false;
    const size_t e = static_cast<size_t>(bone), l = static_cast<size_t>(lower_bone), u = static_cast<size_t>(upper_bone);
    constexpr float kSmall = 1e-4f;  // KINDA_SMALL_NUMBER

    const Vec3 root = cpos_[u];
    const Vec3 delta = desired - root;
    float length = delta.length();
    Vec3 dir;
    if (length < kSmall) {
        length = kSmall;
        dir = Vec3(1.0f, 0.0f, 0.0f);
    } else {
        dir = delta * (1.0f / length);
    }
    const Vec3 joint_delta = joint_target - root;
    Vec3 normal, bend;
    if (joint_delta.length() < kSmall) {
        bend = Vec3(0.0f, 1.0f, 0.0f);
        normal = Vec3(0.0f, 0.0f, 1.0f);
    } else {
        normal = dir.cross(joint_delta);
        if (normal.length() < kSmall) {
            // FindBestAxisVectors.
            const Vec3 pick = std::fabs(dir.z) > std::fabs(dir.x) && std::fabs(dir.z) > std::fabs(dir.y) ? Vec3(1.0f, 0.0f, 0.0f) : Vec3(0.0f, 0.0f, 1.0f);
            normal = safe_normal(pick - dir * pick.dot(dir));
            bend = normal.cross(dir);
        } else {
            normal = normal.normalized();
            bend = safe_normal(joint_delta - dir * joint_delta.dot(dir));
        }
    }
    // The limb's lengths are the reference skeleton's.
    const float lower_len = mesh_->bones[e].bind_pos.length();
    const float upper_len = mesh_->bones[l].bind_pos.length();
    Vec3 out_end = desired;
    Vec3 out_joint = cpos_[l];
    if (length > upper_len + lower_len) {
        out_end = root + dir * (upper_len + lower_len);
        out_joint = root + dir * upper_len;
    } else {
        const float cosine = (upper_len * upper_len + length * length - lower_len * lower_len) / (2.0f * upper_len * length);
        if (cosine > 1.0f || cosine < -1.0f) {
            // Too close to reach: the limb doubles back on itself.
            if (upper_len > lower_len) {
                out_joint = root + dir * upper_len;
                out_end = out_joint - dir * lower_len;
            } else {
                out_joint = root - dir * upper_len;
                out_end = out_joint + dir * lower_len;
            }
        } else {
            const float angle = std::acos(cosine);
            const float off_line = upper_len * std::sin(angle);
            float along = std::sqrt(std::max(0.0f, upper_len * upper_len - off_line * off_line));
            if (cosine < 0.0f) along = -along;
            out_joint = root + dir * along + bend * off_line;
        }
    }
    const float sign = invert_bone ? -1.0f : 1.0f;
    const Vec3 upper_dir = safe_normal(out_joint - root) * sign;
    const Vec3 lower_dir = safe_normal(out_end - out_joint) * sign;
    Quat4 upper_rot = crot_[u], lower_rot = crot_[l];
    if (upper_dir != normal && upper_dir.dot(normal) < 0.1f) upper_rot = limb_turn(bone_axis, upper_dir, joint_axis, normal);
    if (lower_dir != normal && lower_dir.dot(normal) < 0.1f) lower_rot = limb_turn(bone_axis, lower_dir, joint_axis, normal);
    if (keep_upper_roll) {
        // The upper bone keeps the roll it had.
        float pitch = 0.0f, yaw = 0.0f, roll = 0.0f, was_pitch = 0.0f, was_yaw = 0.0f, was_roll = 0.0f;
        rotator_of(upper_rot, pitch, yaw, roll);
        rotator_of(crot_[u], was_pitch, was_yaw, was_roll);
        upper_rot = rotator_quat(static_cast<int>(pitch / kRadiansPerUnit), static_cast<int>(yaw / kRadiansPerUnit),
                                 static_cast<int>(was_roll / kRadiansPerUnit));
    }
    const Quat4 hand_rot = end_rot ? *end_rot : crot_[e];
    blend_in(pose, upper_bone, root, upper_rot, strength);
    blend_in(pose, lower_bone, out_joint, lower_rot, strength);
    blend_in(pose, bone, out_end, hand_rot, strength);
    return true;
}

// TdSkelControlFootPlacement::CalculateNewBoneTransforms. A line from the hip down the leg, to
// 25 uu past the foot (and 10 towards the toes): where it meets a floor, the foot goes there along
// the leg, up by as much as 50 uu or down by as much as 4, to stand FootOffset (11) off it. Only
// on a floor that is not level, or while the mesh is still catching up with a step (SmoothOffset).
void SkelControls::apply_foot(Pose& pose, Foot& foot, const MeshPlace& place) {
    const float strength = foot.strength.value;
    if (foot.bone < 0 || strength <= kZero) return;
    const int lower_bone = mesh_->bones[static_cast<size_t>(foot.bone)].parent_index;
    if (lower_bone <= 0) return;
    const int upper_bone = mesh_->bones[static_cast<size_t>(lower_bone)].parent_index;
    if (upper_bone <= 0) return;
    constexpr float kFootOffset = 11.0f, kMaxUp = 50.0f, kMaxDown = 4.0f;
    const size_t e = static_cast<size_t>(foot.bone);
    const Vec3 hip = to_world(place, cpos_[static_cast<size_t>(upper_bone)]);
    const Vec3 at = to_world(place, cpos_[e]);
    const Vec3 leg = safe_normal(at - hip);
    const Vec3 toes = to_world(place, cpos_[e] + crot_[e].rotate(Vec3(0.0f, 1.0f, 0.0f)) * (foot.invert_bone ? -10.0f : 10.0f)) - at;
    const Vec3 end = at + Vec3(toes.x, toes.y, 0.0f) + leg * (kFootOffset + kMaxDown + 10.0f);
    Vec3 hit_at, hit_normal;
    bool hit = place.trace && place.trace(hip, end, hit_at, hit_normal);
    bool adjust = true;
    if (!hit || hit_normal.z < 0.5f) {
        adjust = false;
        hit = false;
    }
    Vec3 desired = at + leg * kMaxDown;
    if (hit) desired = at + leg * std::clamp((hit_at - at).dot(leg) - kFootOffset, -kMaxUp, kMaxDown);
    if (foot.reset) foot.interpolated = desired.z;
    foot.interpolated = interp_to(foot.interpolated, desired.z, in_.dt, 20.0f);
    if (std::fabs(desired.z - foot.interpolated) > 0.05f) adjust = true;
    if (in_.smooth_offset == 0.0f && !in_.floor_sloped) adjust = false;
    foot.reset = false;
    if (!adjust) return;
    const size_t l = static_cast<size_t>(lower_bone);
    limb(pose, foot.bone, to_comp(place, desired), cpos_[l] + crot_[l].rotate(foot.joint_target), 2, 1, foot.invert_bone, nullptr, false, strength);
}

void SkelControls::apply(Pose& pose, const MeshPlace& place, const Vec3& swan) {
    if (!mesh_ || pose.pos.size() != mesh_->bones.size()) return;
    refresh(pose);
    auto axis_turn = [](int axis, int angle) {
        return axis == 0 ? rotator_quat(0, angle, 0) : axis == 1 ? rotator_quat(angle, 0, 0) : rotator_quat(0, 0, angle);
    };
    auto spring = [&](int i) {
        const Spring& s = springs_[i];
        turn_local(pose, s.bone, axis_turn(s.affected, s.angle), s.strength.value);
    };
    // SpineX: the whole upper body lags a turn of the view.
    spring(0);
    spring(1);
    spring(2);

    // The right arm first, the left after: the skeleton's LeftHand_GameIK, which the left hand's
    // first control reads, hangs off the right hand (the tree's PrioritizedSkelBranches).
    for (int side = 1; side >= 0; --side) {
        const int base = spine_side_[side];
        // TdSkelControlAim1p::UpdateTransformation: the arm turned, in the component's space and
        // about its own root (which is at the eye), by the view's pitch and by how far the view's
        // yaw is off the pawn's, and moved by what the swan neck moves the camera.
        if (base >= 0 && aim_[side].value > kZero) {
            const size_t b = static_cast<size_t>(base);
            const int look = norm_axis(units(in_.view_yaw_deg) - units(in_.pawn_yaw_deg));
            const Quat4 turn = rotator_quat(look, 0, -units(in_.view_pitch_deg));
            blend_in(pose, base, cpos_[b] + swan, Quat4::multiply(turn, crot_[b]).normalized(), aim_[side].value);
        }
        for (const Sway& w : sway_) turn_local(pose, base, axis_turn(w.axis, w.current), sway_strength_);

        const int hand = hand_[side];
        if (hand < 0) continue;
        const int forearm = mesh_->bones[static_cast<size_t>(hand)].parent_index;
        if (forearm <= 0) continue;
        const size_t h = static_cast<size_t>(hand), f = static_cast<size_t>(forearm);
        const bool invert = side == 1;
        if (side == 1) {
            // TdSkelControlRecoil: the hand along its own X, the elbow towards 10 uu down the forearm's Z.
            if (recoil_.value > kZero) {
                limb(pose, hand, cpos_[h] + crot_[h].rotate(Vec3(recoil_x_, 0.0f, 0.0f)), cpos_[f] + crot_[f].rotate(Vec3(0.0f, 0.0f, 10.0f)), 1, 2, true,
                     nullptr, true, recoil_.value);
            }
            spring(3);
            spring(4);
            spring(5);
        } else if (local_ik_.value > kZero && ik_bone_ >= 0) {
            // LeftHandLocalIKController: on the weapon's IK bone, turned as it is.
            const Quat4 turn = crot_[static_cast<size_t>(ik_bone_)];
            limb(pose, hand, cpos_[static_cast<size_t>(ik_bone_)], cpos_[f], 1, 2, false, &turn, false, local_ik_.value);
        }
        // The moves' hand placement (TdMove_Grab.EnableGrabIK, TdMove_SpeedVault.EnableVaultIK).
        if (world_ik_[side].value > kZero) {
            limb(pose, hand, to_comp(place, world_ik_target_[side]), cpos_[f], 1, 2, invert, nullptr, false, world_ik_[side].value);
        }
        // TdSkelControlAgainstWall::TickSkelControl: the hand's place on the wall (20 under where
        // the pawn's trace met it), held inside a box in the component's space, and closed on at 6 a second.
        {
            const bool on = in_.against_wall == 1 || in_.against_wall == (side == 0 ? 2 : 3);
            const Vec3 lo = side == 0 ? Vec3(5.0f, -170.0f, 10.0f) : Vec3(-35.0f, -170.0f, 10.0f);
            const Vec3 hi = side == 0 ? Vec3(35.0f, -100.0f, 30.0f) : Vec3(-5.0f, -100.0f, 30.0f);
            if (on) {
                const Vec3 t = to_comp(place, in_.wall_hand[side] + Vec3(0.0f, 0.0f, -20.0f));
                wall_target_[side] = Vec3(std::clamp(t.x, lo.x, hi.x), std::clamp(t.y, lo.y, hi.y), std::clamp(t.z, lo.z, hi.z));
            }
            wall_effector_[side] = vinterp_to(wall_effector_[side], wall_target_[side], in_.dt, 6.0f);
            const float weight = std::min(in_.wall_weight[side], 1.0f);
            if (weight > kZero) limb(pose, hand, wall_effector_[side], cpos_[f], 1, 2, invert, nullptr, false, weight);
        }
    }

    // EyeJoint: the camera rolls into a turn made on the move.
    spring(6);

    apply_foot(pose, foot_[0], place);
    apply_foot(pose, foot_[1], place);
    in_.dt = 0.0f;  // the controls that step as they are applied step once a tick
}

}  // namespace me::fp
