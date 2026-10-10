#pragma once

// -----------------------------------------------------------------------------
// The bodies a bullet meets in a person.
//
// A weapon's shot is a line with no extent (Weapon.CalcWeaponFire: Trace(HitLocation, HitNormal,
// EndTrace, StartTrace, true, vect(0,0,0), HitInfo, TRACEFLAG_Bullet)), and neither of a pawn's
// cylinders blocks one (TdPawn's CollisionCylinder and ActorCollisionCylinder: BlockZeroExtent
// False). It can meet a pawn only through his third-person mesh (TdPawnMesh3p: CollideActors,
// BlockZeroExtent), whose line check is its physics asset's:
//
//   USkeletalMeshComponent::LineCheck   a mesh with no PerPolyCollisionBones (every enemy's) hands the
//                                       line to its PhysicsAsset
//   UPhysicsAsset::LineCheck (MirrorsEdge.exe 0x00bd90f0)
//                                       every RB_BodySetup whose bone the skeleton has, its shapes
//                                       placed by that bone's matrix as it is now; the nearest hit
//                                       wins: the body's index is the hit's Item, its bone the hit's
//                                       BoneName, its material the hit's PhysMaterial
//   FKBoxElem::LineCheck   (0x00dcfb80) a box about the element's origin; X, Y, Z are whole edges
//   FKSphylElem::LineCheck (0x00dd1310) a capsule along the element's Z: Length is the cylinder's
//                                       alone, with a half sphere of Radius on each end
//
// Every bot's asset is CH_TKY_Cop_SWAT.Male3p_Physics (the TdPawnMesh3p of each bot pawn class names
// it; the template only chooses the mesh): 16 bodies, 7 boxes and 10 capsules, PM_Character_Body
// but for the Neck's (a capsule round the head) and the two hands', which are PM_Character_Head.
//
// Here: the shapes as they stand in the world for one bot at one moment (anim/anim_system.hpp poses
// them with the bones the drawn mesh is skinned by), and the line through them.
// -----------------------------------------------------------------------------

#include "../math/types.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace me {

enum class EBodyShape : uint8_t { Box = 0, Capsule = 1 };

// One KBoxElem or KSphylElem of a body, placed in the world.
struct BodyShape {
    EBodyShape kind = EBodyShape::Box;
    uint8_t body = 0;  // the RB_BodySetup it belongs to, by its place in the asset's list (FCheckResult.Item)
    Vec3 centre{0.0f, 0.0f, 0.0f};
    Vec3 axis[3] = {Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, 0.0f, 1.0f)};  // unit; a capsule runs along axis[2]
    Vec3 half{0.0f, 0.0f, 0.0f};  // a box's half edges; a capsule's radius in x and half of its Length in z
};

// An RB_BodySetup: the bone that carries it and its material.
struct EnemyBody {
    std::string bone;  // BoneName (FCheckResult.BoneName)
    ECharacterSurface surface = ECharacterSurface::Body;  // PhysMaterial (math/types.hpp)
};

// One bot's bodies at one moment.
struct EnemyBodySet {
    bool valid = false;  // false: he has none (no character assets): the caller keeps to his cylinder
    Vec3 bound_centre{0.0f, 0.0f, 0.0f};  // a sphere round every shape
    float bound_radius = 0.0f;
    std::vector<EnemyBody> bodies;  // the asset's, in its order
    std::vector<BodyShape> shapes;  // body by body, a body's boxes before its capsules
};

struct EnemyBodyHit {
    float distance = 0.0f;         // from the line's start
    Vec3 location{0.0f, 0.0f, 0.0f};
    Vec3 normal{0.0f, 0.0f, 1.0f};  // out of the shape, where the line goes in
    int32_t shape = -1;            // EnemyBodySet::shapes
    uint8_t body = 0;              // EnemyBodySet::bodies
    ECharacterSurface surface = ECharacterSurface::Body;
    std::string_view bone;         // the body's bone: the set's own string, good while the set is
};

// Whether a bullet can meet this bot at all. A pawn that has died is still what the weapon's trace
// meets: the script has a branch for the bullet that finds him so (TdBotPawn.TakeDamage does
// nothing to one already dead; TdBotPawn.PreventWeaponImpactEffect answers true in the state Dying,
// so it leaves no effect and no sound). That his mesh goes on colliding is inferred from those
// branches, not read in the executable. Here a dead bot is drawn on in the pose he died in, by the
// code that poses a living one. A story character's `alive` says only whether she is on stage.
inline bool enemy_stops_bullets(const EnemyBot& bot) {
    return bot.alive || !bot.is_story_npc;
}

// Whether the line from `from` along the unit vector `dir`, `max_dist` long, comes within `radius`
// of `centre`.
inline bool line_near_point(const Vec3& from, const Vec3& dir, float max_dist, const Vec3& centre, float radius) {
    const Vec3 to_centre = centre - from;
    const float along = std::clamp(to_centre.dot(dir), 0.0f, std::max(max_dist, 0.0f));
    return (to_centre - dir * along).length_sq() <= radius * radius;
}

// Where the line from `from` along the unit vector `dir` goes into one shape, if it does within
// `max_dist`: the distance, and the surface's normal there. A line that starts inside the shape
// meets it at once, facing back along the line.
inline bool trace_body_shape(const BodyShape& s, const Vec3& from, const Vec3& dir, float max_dist, float& out_distance, Vec3& out_normal) {
    // The line in the shape's own frame, as both of the engine's element checks take it.
    const Vec3 rel = from - s.centre;
    const Vec3 o(rel.dot(s.axis[0]), rel.dot(s.axis[1]), rel.dot(s.axis[2]));
    const Vec3 d(dir.dot(s.axis[0]), dir.dot(s.axis[1]), dir.dot(s.axis[2]));
    // Distances are taken from the line's point nearest the shape's centre, so that the arithmetic
    // is among numbers of the shape's own size however far off the shot was fired.
    const float mid = -o.dot(d);
    const Vec3 c = o + d * mid;
    float enter = 0.0f;
    Vec3 n(0.0f, 0.0f, 0.0f);

    if (s.kind == EBodyShape::Box) {
        const float cs[3] = {c.x, c.y, c.z}, ds[3] = {d.x, d.y, d.z}, hs[3] = {s.half.x, s.half.y, s.half.z};
        const float os[3] = {o.x, o.y, o.z};
        if (std::abs(os[0]) <= hs[0] && std::abs(os[1]) <= hs[1] && std::abs(os[2]) <= hs[2]) {
            out_distance = 0.0f;
            out_normal = dir * -1.0f;
            return max_dist >= 0.0f;
        }
        // The three pairs of faces: in through the last one entered, if that is before the first one left.
        enter = -std::numeric_limits<float>::max();
        float leave = std::numeric_limits<float>::max();
        int side = -1;
        float sign = 0.0f;
        for (int i = 0; i < 3; ++i) {
            if (std::abs(ds[i]) < 1.0e-8f) {
                if (std::abs(cs[i]) > hs[i]) return false;
                continue;
            }
            float near_face = (-hs[i] - cs[i]) / ds[i], far_face = (hs[i] - cs[i]) / ds[i];
            float face_sign = -1.0f;
            if (near_face > far_face) {
                std::swap(near_face, far_face);
                face_sign = 1.0f;
            }
            if (near_face > enter) {
                enter = near_face;
                side = i;
                sign = face_sign;
            }
            leave = std::min(leave, far_face);
            if (enter > leave) return false;
        }
        if (side < 0 || leave + mid < 0.0f) return false;
        n = Vec3(side == 0 ? sign : 0.0f, side == 1 ? sign : 0.0f, side == 2 ? sign : 0.0f);
    } else {
        const float r = s.half.x, h = s.half.z;
        // Inside when within the radius of the stretch of the axis between the two end spheres' centres.
        const auto from_axis = [h](const Vec3& p) { return Vec3(p.x, p.y, p.z - std::clamp(p.z, -h, h)); };
        if (from_axis(o).length_sq() <= r * r) {
            out_distance = 0.0f;
            out_normal = dir * -1.0f;
            return max_dist >= 0.0f;
        }
        bool found = false;
        enter = std::numeric_limits<float>::max();
        const auto consider = [&](float at) {
            if (at + mid < 0.0f || at >= enter) return;
            enter = at;
            found = true;
        };
        // The cylinder's side, between the ends...
        const float a = d.x * d.x + d.y * d.y;
        if (a > 1.0e-12f) {
            const float b = c.x * d.x + c.y * d.y;
            const float disc = b * b - a * (c.x * c.x + c.y * c.y - r * r);
            if (disc >= 0.0f) {
                const float at = (-b - std::sqrt(disc)) / a;
                if (std::abs(c.z + d.z * at) <= h) consider(at);
            }
        }
        // ...and the sphere on each end (a line that comes in through the flat of an end has met its sphere before).
        for (const float end : {-h, h}) {
            const Vec3 oc(c.x, c.y, c.z - end);
            const float b = oc.dot(d);
            const float disc = b * b - (oc.length_sq() - r * r);
            if (disc >= 0.0f) consider(-b - std::sqrt(disc));
        }
        if (!found) return false;
        n = from_axis(c + d * enter);
        const float len = n.length();
        n = len > 1.0e-6f ? n * (1.0f / len) : d * -1.0f;
    }

    const float distance = mid + enter;
    if (distance > max_dist) return false;
    out_distance = std::max(distance, 0.0f);
    out_normal = s.axis[0] * n.x + s.axis[1] * n.y + s.axis[2] * n.z;
    return true;
}

// UPhysicsAsset::LineCheck: the nearest of a bot's shapes the line from `from` along the unit vector
// `dir` goes into within `max_dist`.
inline bool trace_enemy_bodies(const EnemyBodySet& set, const Vec3& from, const Vec3& dir, float max_dist, EnemyBodyHit& out) {
    if (!set.valid || !line_near_point(from, dir, max_dist, set.bound_centre, set.bound_radius)) return false;
    float best = max_dist;
    int32_t hit = -1;
    Vec3 normal(0.0f, 0.0f, 0.0f);
    for (size_t i = 0; i < set.shapes.size(); ++i) {
        float distance = 0.0f;
        Vec3 n;
        if (!trace_body_shape(set.shapes[i], from, dir, best, distance, n)) continue;
        if (hit >= 0 && distance >= best) continue;  // the first of two met at the same place keeps it
        best = distance;
        hit = static_cast<int32_t>(i);
        normal = n;
    }
    if (hit < 0) return false;
    const BodyShape& s = set.shapes[static_cast<size_t>(hit)];
    out.distance = best;
    out.location = from + dir * best;
    out.normal = normal;
    out.shape = hit;
    out.body = s.body;
    if (s.body < set.bodies.size()) {
        out.surface = set.bodies[s.body].surface;
        out.bone = set.bodies[s.body].bone;
    } else {
        out.surface = ECharacterSurface::Body;
        out.bone = std::string_view();
    }
    return true;
}

// Fills `out` with a bot's bodies as he is drawn at `sim_time` (AnimSystem::pose_enemy_bodies); false,
// and `out.valid` false, when it cannot.
using EnemyBodyPoser = std::function<bool(const EnemyBot& bot, float sim_time, bool reaction_disarm, EnemyBodySet& out)>;

}  // namespace me
