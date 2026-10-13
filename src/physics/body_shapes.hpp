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

// A mesh a bot carries whose own triangles a bullet is tested against: the riot shield
// (TdInv_Shield's RiotShield component, WP_RiotShield.SK_WP_RiotShield). Its mesh names a
// PerPolyCollisionBones (Shield_Root) and has no physics asset, so USkeletalMeshComponent::LineCheck
// takes the per-triangle branch for a bullet's trace (TRACEFLAG_Bullet) and no other trace meets it.
// The tree the package stores for that bone holds all 1086 triangles on all 1174 vertices. Here
// they are, in the bone's space, with their box.
struct EnemyShieldMesh {
    std::vector<Vec3> vertices;
    std::vector<uint32_t> indices;  // three to a triangle
    Vec3 lo{0.0f, 0.0f, 0.0f}, hi{0.0f, 0.0f, 0.0f};
};

// The per-triangle hit is drawn back along the line (USkeletalMeshComponent::LineCheck,
// MirrorsEdge.exe 0xd53a52..0xd53b91): Time - Min(4 / Dist, Max(0.1, 0.1 / Dist)), Dist the
// line's whole length, which is 4 uu for any line over 40 long (every weapon's), and reports the
// bone named there.
inline constexpr float kShieldPullBack = 4.0f;
inline constexpr std::string_view kShieldBone = "Shield_Root";

// The shield as one bot carries it at one moment: its bone's frame in the world.
struct EnemyShield {
    bool valid = false;
    Vec3 origin{0.0f, 0.0f, 0.0f};
    Vec3 axis[3] = {Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, 0.0f, 1.0f)};  // unit, square to each other
    const EnemyShieldMesh* mesh = nullptr;  // the animation system's, shared by every bot
};

// One bot's bodies at one moment.
struct EnemyBodySet {
    bool valid = false;  // false: he has none (no character assets): the caller keeps to his cylinder
    Vec3 bound_centre{0.0f, 0.0f, 0.0f};  // a sphere round every shape, and round the shield's box
    float bound_radius = 0.0f;
    std::vector<EnemyBody> bodies;  // the asset's, in its order
    std::vector<BodyShape> shapes;  // body by body, a body's boxes before its capsules
    EnemyShield shield;             // the riot shield, when he carries one
};

struct EnemyBodyHit {
    float distance = 0.0f;         // from the line's start
    Vec3 location{0.0f, 0.0f, 0.0f};
    Vec3 normal{0.0f, 0.0f, 1.0f};  // out of the shape, where the line goes in
    int32_t shape = -1;            // EnemyBodySet::shapes (-1 on the shield)
    uint8_t body = 0;              // EnemyBodySet::bodies
    ECharacterSurface surface = ECharacterSurface::Body;
    std::string_view bone;         // the body's bone: the set's own string, good while the set is
    bool shield = false;           // on the shield (bone Shield_Root), not on a body
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

// The nearest of the shield's triangles the line from `from` along the unit vector `dir` crosses
// within `max_dist`, from either side (that the kDOP test takes both faces is inferred: the stock
// line check, not read in the executable): how far along the line, and the triangle's normal turned
// to face back along it. Not drawn back.
inline bool trace_enemy_shield(const EnemyShield& shield, const Vec3& from, const Vec3& dir, float max_dist, float& out_distance, Vec3& out_normal) {
    if (!shield.valid || !shield.mesh || shield.mesh->indices.size() < 3 || max_dist < 0.0f) return false;
    const EnemyShieldMesh& m = *shield.mesh;
    // The line in the bone's frame, from the bone's place, so that the arithmetic is among numbers
    // of the shield's own size.
    const Vec3 rel = from - shield.origin;
    const float os[3] = {rel.dot(shield.axis[0]), rel.dot(shield.axis[1]), rel.dot(shield.axis[2])};
    const float ds[3] = {dir.dot(shield.axis[0]), dir.dot(shield.axis[1]), dir.dot(shield.axis[2])};
    // Its box first (a little grown), as the tree's root node is.
    constexpr float kGrow = 0.1f;
    const float lo[3] = {m.lo.x - kGrow, m.lo.y - kGrow, m.lo.z - kGrow}, hi[3] = {m.hi.x + kGrow, m.hi.y + kGrow, m.hi.z + kGrow};
    float enter = 0.0f, leave = max_dist;
    for (int i = 0; i < 3; ++i) {
        if (std::abs(ds[i]) < 1.0e-9f) {
            if (os[i] < lo[i] || os[i] > hi[i]) return false;
            continue;
        }
        float a = (lo[i] - os[i]) / ds[i], b = (hi[i] - os[i]) / ds[i];
        if (a > b) std::swap(a, b);
        enter = std::max(enter, a);
        leave = std::min(leave, b);
        if (enter > leave) return false;
    }
    const Vec3 o(os[0], os[1], os[2]), d(ds[0], ds[1], ds[2]);
    float best = max_dist;
    bool found = false;
    Vec3 n(0.0f, 0.0f, 0.0f);
    for (size_t k = 0; k + 2 < m.indices.size(); k += 3) {
        const Vec3& v0 = m.vertices[m.indices[k]];
        const Vec3 e1 = m.vertices[m.indices[k + 1]] - v0, e2 = m.vertices[m.indices[k + 2]] - v0;
        const Vec3 p = d.cross(e2);
        const float det = e1.dot(p);
        if (std::abs(det) < 1.0e-12f) continue;  // the line runs in the triangle's plane
        const float inv = 1.0f / det;
        const Vec3 s = o - v0;
        const float u = s.dot(p) * inv;
        if (u < 0.0f || u > 1.0f) continue;
        const Vec3 q = s.cross(e1);
        const float v = d.dot(q) * inv;
        if (v < 0.0f || u + v > 1.0f) continue;
        const float t = e2.dot(q) * inv;
        if (t < 0.0f || t > best || (found && t >= best)) continue;
        best = t;
        n = e1.cross(e2);
        found = true;
    }
    if (!found) return false;
    Vec3 normal = shield.axis[0] * n.x + shield.axis[1] * n.y + shield.axis[2] * n.z;
    const float len = normal.length();
    normal = len > 1.0e-12f ? normal * (1.0f / len) : dir * -1.0f;
    if (normal.dot(dir) > 0.0f) normal = normal * -1.0f;
    out_distance = best;
    out_normal = normal;
    return true;
}

// UPhysicsAsset::LineCheck: the nearest of a bot's shapes the line from `from` along the unit vector
// `dir` goes into within `max_dist`; and his shield's per-triangle check beside it, as another
// component of the same actor: the nearer of the two wins, the shield's hit drawn back first.
inline bool trace_enemy_bodies(const EnemyBodySet& set, const Vec3& from, const Vec3& dir, float max_dist, EnemyBodyHit& out) {
    const float reach = set.shield.valid ? max_dist + kShieldPullBack : max_dist;
    if (!set.valid || !line_near_point(from, dir, reach, set.bound_centre, set.bound_radius)) return false;
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
    // A triangle up to 4 uu past the end still counts: drawn back, it is within it.
    float on_shield = 0.0f;
    Vec3 shield_normal;
    if (trace_enemy_shield(set.shield, from, dir, best + kShieldPullBack, on_shield, shield_normal)) {
        on_shield = std::max(on_shield - kShieldPullBack, 0.0f);
        if (on_shield <= max_dist && (hit < 0 || on_shield < best)) {
            out.distance = on_shield;
            out.location = from + dir * on_shield;
            out.normal = shield_normal;
            out.shape = -1;
            out.body = 0;
            out.surface = ECharacterSurface::Body;
            out.bone = kShieldBone;
            out.shield = true;
            return true;
        }
    }
    if (hit < 0) return false;
    out.shield = false;
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
