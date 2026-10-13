#pragma once

// -----------------------------------------------------------------------------
// A Matinee's movement track, evaluated as the game evaluates it (docs/GAMEPLAY_SCRIPTING_RE.md
// section 9; the functions named are MirrorsEdge.exe's):
//
//   UInterpTrackMove::GetKeyTransformAtTime  0x00e41080   the keys at a time: a position and a rotation
//   ...its curve evaluation                  0x00e40ae0   PosTrack (0x00e40db0, the same code, EulerTrack)
//   FRotator::MakeFromEuler                  0x0116f040   degrees to rotation units, truncated
//   UInterpTrackMove::GetMoveRefFrame        0x00e3ca00   what the keys are relative to
//   UInterpTrackMove::GetLocationAtTime      0x00e41340   the two composed: where the actor goes
//   UInterpTrackInstMove::CalcInitialTransform 0x00e51ad0 InitialTM, when the Matinee is initialised
//
// The executable's matrices are row-vector (World = Rel * Initial * Base); the port's are
// column-vector, so the same products are written here the other way round:
//
//   World(t) = Base * Initial * Rel(t)                 IMF_RelativeToInitial
//   World(t) = Base * Rel(t)                           IMF_World
//   Initial  = inverse(Base) * Actor * inverse(Rel(T0))
//
// T0 is the Matinee's Position at the moment it is initialised (0 when the transforms are reset for
// a rewind), Actor the actor's transform at that moment and Base that of the actor it is attached
// to (the identity for one attached to nothing). A relative track therefore goes on from wherever
// its actor stands, with the key transform of that moment divided out: "placed pose plus key" only
// for a Matinee first played from Position 0 with an identity first key.
// -----------------------------------------------------------------------------

#include "../math/types.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace me {

// FInterpCurve<FVector>::Eval as 0x00e40ae0 has it. The tangents are per unit of time: they are
// multiplied by the segment's length (the curve's InterpMethod is never set in the cooked data).
inline Vec3 eval_move_curve(const std::vector<MoveCurveKey>& keys, float t) {
    const size_t n = keys.size();
    if (n == 0) return Vec3(0.0f, 0.0f, 0.0f);
    if (n < 2 || keys.front().time >= t) return keys.front().value;
    if (t >= keys.back().time) return keys.back().value;
    size_t i = 1;
    while (i < n && !(keys[i].time > t)) ++i;  // the first key after the time
    const MoveCurveKey& a = keys[i - 1];
    const MoveCurveKey& b = keys[i];
    const float diff = b.time - a.time;
    if (diff <= 0.0f || a.mode == 2) return a.value;  // CIM_Constant: held until the next key's own time
    const float alpha = (t - a.time) / diff;
    if (a.mode == 0) return a.value + (b.value - a.value) * alpha;  // CIM_Linear
    const float a2 = alpha * alpha;
    const float a3 = a2 * alpha;
    return a.value * (2.0f * a3 - 3.0f * a2 + 1.0f) + a.leave * (diff * (a3 - 2.0f * a2 + alpha)) + b.arrive * (diff * (a3 - a2)) +
           b.value * (-2.0f * a3 + 3.0f * a2);
}

// FRotator::MakeFromEuler: (roll, pitch, yaw) in degrees to rotation units, each truncated.
inline Rotator rotator_from_euler(const Vec3& euler) {
    constexpr float kUnitsPerDegree = 182.04444885253906f;  // the float at 0x01b0c864: 65536 / 360
    return Rotator(std::trunc(euler.y * kUnitsPerDegree), std::trunc(euler.z * kUnitsPerDegree), std::trunc(euler.x * kUnitsPerDegree));
}

// FRotationTranslationMatrix: the axes of the rotation in the columns, the origin in the last.
inline Mat4 rotation_translation_matrix(const Rotator& rotation, const Vec3& origin) {
    // Whole turns taken out first (a cooked yaw can be several turns round), for the float trigonometry.
    const Rotator r(std::remainder(rotation.pitch, 65536.0f), std::remainder(rotation.yaw, 65536.0f), std::remainder(rotation.roll, 65536.0f));
    const Vec3 x = r.forward(), y = r.right(), z = r.up();
    Mat4 m = Mat4::identity();
    m.m[0] = x.x;
    m.m[1] = x.y;
    m.m[2] = x.z;
    m.m[4] = y.x;
    m.m[5] = y.y;
    m.m[6] = y.z;
    m.m[8] = z.x;
    m.m[9] = z.y;
    m.m[10] = z.z;
    m.m[12] = origin.x;
    m.m[13] = origin.y;
    m.m[14] = origin.z;
    return m;
}

// A rotation as a quaternion, for the tracks that turn by the shortest way between two keys.
struct MoveQuat {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;

    // FQuat(FMatrix): from the rotation part of a matrix.
    static MoveQuat from_matrix(const Mat4& m) {
        const auto at = [&](int row, int col) { return m.m[col * 4 + row]; };
        MoveQuat q;
        const float trace = at(0, 0) + at(1, 1) + at(2, 2);
        if (trace > 0.0f) {
            const float s = std::sqrt(trace + 1.0f) * 2.0f;
            q.w = 0.25f * s;
            q.x = (at(2, 1) - at(1, 2)) / s;
            q.y = (at(0, 2) - at(2, 0)) / s;
            q.z = (at(1, 0) - at(0, 1)) / s;
        } else if (at(0, 0) > at(1, 1) && at(0, 0) > at(2, 2)) {
            const float s = std::sqrt(1.0f + at(0, 0) - at(1, 1) - at(2, 2)) * 2.0f;
            q.w = (at(2, 1) - at(1, 2)) / s;
            q.x = 0.25f * s;
            q.y = (at(0, 1) + at(1, 0)) / s;
            q.z = (at(0, 2) + at(2, 0)) / s;
        } else if (at(1, 1) > at(2, 2)) {
            const float s = std::sqrt(1.0f + at(1, 1) - at(0, 0) - at(2, 2)) * 2.0f;
            q.w = (at(0, 2) - at(2, 0)) / s;
            q.x = (at(0, 1) + at(1, 0)) / s;
            q.y = 0.25f * s;
            q.z = (at(1, 2) + at(2, 1)) / s;
        } else {
            const float s = std::sqrt(1.0f + at(2, 2) - at(0, 0) - at(1, 1)) * 2.0f;
            q.w = (at(1, 0) - at(0, 1)) / s;
            q.x = (at(0, 2) + at(2, 0)) / s;
            q.y = (at(1, 2) + at(2, 1)) / s;
            q.z = 0.25f * s;
        }
        return q;
    }

    // SlerpQuat (0x0116fc20): along the shorter of the two ways round.
    static MoveQuat slerp(const MoveQuat& a, const MoveQuat& b, float alpha) {
        const float raw = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
        const float cosom = std::abs(raw);
        float s0 = 1.0f - alpha, s1 = alpha;
        if (cosom < 0.9999f) {
            const float omega = std::acos(cosom);
            const float inv_sin = 1.0f / std::sin(omega);
            s0 = std::sin((1.0f - alpha) * omega) * inv_sin;
            s1 = std::sin(alpha * omega) * inv_sin;
        }
        if (raw < 0.0f) s1 = -s1;
        MoveQuat q;
        q.x = s0 * a.x + s1 * b.x;
        q.y = s0 * a.y + s1 * b.y;
        q.z = s0 * a.z + s1 * b.z;
        q.w = s0 * a.w + s1 * b.w;
        const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        if (len > 1.0e-8f) {
            q.x /= len;
            q.y /= len;
            q.z /= len;
            q.w /= len;
        }
        return q;
    }

    [[nodiscard]] Mat4 to_matrix(const Vec3& origin) const {
        Mat4 m = Mat4::identity();
        const float xx = x * x, yy = y * y, zz = z * z, xy = x * y, xz = x * z, yz = y * z, wx = w * x, wy = w * y, wz = w * z;
        m.m[0] = 1.0f - 2.0f * (yy + zz);
        m.m[1] = 2.0f * (xy + wz);
        m.m[2] = 2.0f * (xz - wy);
        m.m[4] = 2.0f * (xy - wz);
        m.m[5] = 1.0f - 2.0f * (xx + zz);
        m.m[6] = 2.0f * (yz + wx);
        m.m[8] = 2.0f * (xz + wy);
        m.m[9] = 2.0f * (yz - wx);
        m.m[10] = 1.0f - 2.0f * (xx + yy);
        m.m[12] = origin.x;
        m.m[13] = origin.y;
        m.m[14] = origin.z;
        return m;
    }
};

// UInterpTrackMove::GetKeyTransformAtTime, as FRotationTranslationMatrix(RelRot, RelPos): the keys'
// own transform at a time, before any reference frame. With bUseQuatInterpolation the rotation is
// the slerp of the two keys round the time (each key's Euler angles made a quaternion), and that of
// the first or last key outside them.
inline Mat4 move_key_transform(const MoveTrack& track, float t) {
    const Vec3 pos = eval_move_curve(track.pos, t);
    const std::vector<MoveCurveKey>& keys = track.euler;
    if (!track.quat || keys.size() < 2 || t <= keys.front().time || t >= keys.back().time) {
        Vec3 euler = eval_move_curve(keys, t);
        if (track.quat && !keys.empty()) euler = t >= keys.back().time && keys.size() >= 2 ? keys.back().value : keys.front().value;
        return rotation_translation_matrix(rotator_from_euler(euler), pos);
    }
    size_t i = 1;
    while (i + 1 < keys.size() && !(t < keys[i].time)) ++i;
    const float span = keys[i].time - keys[i - 1].time;
    const float alpha = span > 0.0f ? std::clamp((t - keys[i - 1].time) / span, 0.0f, 1.0f) : 1.0f;
    const MoveQuat a = MoveQuat::from_matrix(rotation_translation_matrix(rotator_from_euler(keys[i - 1].value), Vec3(0.0f, 0.0f, 0.0f)));
    const MoveQuat b = MoveQuat::from_matrix(rotation_translation_matrix(rotator_from_euler(keys[i].value), Vec3(0.0f, 0.0f, 0.0f)));
    return MoveQuat::slerp(a, b, alpha).to_matrix(pos);
}

// UInterpTrackInstMove::CalcInitialTransform: `actor_in_base` is the actor's transform in the frame
// of what it is attached to (its own transform when attached to nothing), `t0` the Matinee's
// Position at that moment, or 0 when the transforms are zeroed from here (bZeroFromHere).
inline Mat4 move_initial_transform(const MoveTrack& track, const Mat4& actor_in_base, float t0) {
    return actor_in_base * move_key_transform(track, t0).rigid_inverse();
}

// UInterpTrackMove::GetLocationAtTime: where the track puts its actor at a time. False, and `out`
// untouched, for a track with no rotation keys: the game then leaves the actor where it is.
// (The whole turns the game adds back to the actor's Rotation mean nothing to a matrix.)
inline bool move_track_transform(const MoveTrack& track, const Mat4& initial, const Mat4& base, float t, Mat4& out) {
    if (track.euler.empty()) return false;
    const Mat4 rel = move_key_transform(track, t);
    out = track.relative ? base * initial * rel : base * rel;
    return true;
}

}  // namespace me
