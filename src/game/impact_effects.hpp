#pragma once

// -----------------------------------------------------------------------------
// What the game makes while it runs: a bullet's impact effect and its bullet hole, and the
// emitters of the script's actor factories (docs/RENDERING_RE.md, "Particles", "Decals";
// assets/level_impacts.hpp reads what they are made of, renderer/particles.hpp runs and
// draws them).
//
// TdWeapon.PlayImpactEffects, for a shot that did not land on a player:
//
//   SpawnImpactEffects   the effect of the surface's physical material for this ammunition,
//                        else of its parents, else of the default material; made at the hit,
//                        pointed along the ray mirrored in the surface and lifted from it:
//                          R = D - 2 N (D . N);  R += (1 - R . N) 0.3 N
//   SpawnImpactDecal     AngleOfImpact = acos(-N . D). The material's impact list under its
//                        CriticalAngle, its ricochet list over it, one of the list by chance;
//                        else its parents'; else the default 16 x 16 decal.
//                        RandScaling = 1 + FRand() 0.5. DecalRotation 360: any angle; -360:
//                        turned to the way the bullet went along the surface, and stretched
//                        by 1 + AngleOfImpact / 90 * DecalStretchingMultiplier (1.5).
//                        DecalManager.SpawnDecal(material, hit, rotator(-N), Width RandScaling,
//                        Height RandScaling Stretching, Thickness 10, ..): a box 5 units to
//                        each side of the surface, 30 seconds, a hundred at a time.
//
// A spawned decal's frame follows from its Orientation and DecalRotation as a placed decal's
// does (checked on the 4225 placed ones that save an Orientation): with X, Y, Z the
// orientation's axes, HitNormal = -X, HitTangent = -Y cos - Z sin, HitBinormal = Z cos - Y sin.
//
// Not as the game: the surface is found by a short line check through the bullet's end
// against the meshes' own triangles and the BSP (the BSP's surfaces count as the default
// material); whether a bot's shot is relevant is only its distance from the player; a pawn
// that is hit gets nothing here.
// -----------------------------------------------------------------------------

#include "../math/types.hpp"
#include "../physics/collision_world.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <random>

namespace me {

inline constexpr float kImpactEffectDistance = 3000.0f;  // TdWeapon.SpawnImpactEffects: MaxImpactEffectDistance
inline constexpr size_t kMaxDynamicDecals = 100;         // DecalManager.MaxActiveDecals
inline constexpr float kDynamicDecalLife = 30.0f;        // DecalManager.DecalLifeSpan
inline constexpr float kDecalStretching = 1.5f;          // TdWeapon.DecalStretchingMultiplier
inline constexpr float kImpactDecalThickness = 10.0f;    // TdWeapon.SpawnImpactDecal
inline constexpr float kImpactEffectLife = 20.0f;        // how long a bullet's effect is kept here
inline constexpr size_t kMaxSpawnedEffects = 64;

// The axes of rotator(direction): FRotationMatrix of a rotation with no roll.
inline void direction_axes(const Vec3& direction, Vec3& x, Vec3& y, Vec3& z) {
    const float len = direction.length();
    x = len > 1.0e-6f ? direction * (1.0f / len) : Vec3(1.0f, 0.0f, 0.0f);
    const float flat = std::sqrt(x.x * x.x + x.y * x.y);
    const float cy = flat > 1.0e-6f ? x.x / flat : 1.0f, sy = flat > 1.0e-6f ? x.y / flat : 0.0f;
    y = Vec3(-sy, cy, 0.0f);
    z = Vec3(-x.z * cy, -x.z * sy, flat);
}

// A particle system at a place, with the X axis along `direction`. Returns its id, 0 when there is no such template.
inline uint32_t spawn_effect(LevelScene& scene, int32_t template_index, const Vec3& location, const Vec3& direction, bool forever) {
    if (template_index < 0 || static_cast<size_t>(template_index) >= scene.particle_templates.size()) return 0;
    SpawnedEffect e;
    e.id = scene.next_effect_id++;
    e.at.template_index = template_index;
    e.at.location = location;
    direction_axes(direction, e.at.axis_x, e.at.axis_y, e.at.axis_z);
    e.at.active = true;
    e.life = kImpactEffectLife;
    e.forever = forever;
    // The oldest of its kind makes room.
    size_t same = 0;
    for (const SpawnedEffect& other : scene.spawned_effects) same += other.forever == forever ? 1 : 0;
    for (size_t i = 0; same >= kMaxSpawnedEffects && i < scene.spawned_effects.size(); ++i) {
        if (scene.spawned_effects[i].forever != forever) continue;
        scene.spawned_effects.erase(scene.spawned_effects.begin() + static_cast<std::ptrdiff_t>(i));
        --same;
        --i;
    }
    const uint32_t id = e.id;
    scene.spawned_effects.push_back(std::move(e));
    return id;
}

// A decal on what lies at `location`: a box `width` x `height`, `kImpactDecalThickness` deep, facing
// `normal`, turned by `rotation_deg`. False when nothing took it.
inline bool spawn_decal(LevelScene& scene, const ImpactDecalInfo& info, const Vec3& location, const Vec3& normal, float width,
                        float height, float rotation_deg) {
    if (info.material < 0 || !scene.collision || !(width > 0.0f) || !(height > 0.0f)) return false;
    Vec3 x, y, z;
    direction_axes(normal * -1.0f, x, y, z);
    const float angle = rotation_deg * 0.01745329252f;
    const float cs = std::cos(angle), sn = std::sin(angle);
    const Vec3 n = x * -1.0f;
    const Vec3 t = y * -cs - z * sn;
    const Vec3 b = z * cs - y * sn;
    const float half_depth = kImpactDecalThickness * 0.5f;  // FarPlane = Thickness / 2, NearPlane = -FarPlane

    DynamicDecal decal;
    decal.material = info.material;
    decal.life = kDynamicDecalLife;
    const auto emit = [&](const Vec3& p, const Vec3& face) {
        Vertex v;
        v.position = p;
        v.normal = face;
        v.tangent = t;
        v.u = v.u2 = 0.5f - (p - location).dot(t) / width;
        v.v = v.v2 = 0.5f - (p - location).dot(b) / height;
        decal.vertices.push_back(v);
    };
    if (info.no_clip) {
        // bNoClip: the whole quad, on the surface's plane.
        const Vec3 c[4] = {location + t * (width * 0.5f) + b * (height * 0.5f), location - t * (width * 0.5f) + b * (height * 0.5f),
                           location - t * (width * 0.5f) - b * (height * 0.5f), location + t * (width * 0.5f) - b * (height * 0.5f)};
        for (int k : {0, 1, 2, 0, 2, 3}) emit(c[k], n);
    } else {
        // The box's six planes, as (direction, limit): inside when dot(P - O, direction) <= limit.
        const std::pair<Vec3, float> planes[6] = {
            {t, width * 0.5f}, {t * -1.0f, width * 0.5f}, {b, height * 0.5f}, {b * -1.0f, height * 0.5f}, {n * -1.0f, half_depth}, {n, half_depth},
        };
        const float reach = 0.5f * std::sqrt(width * width + height * height) + half_depth;
        std::vector<uint32_t> found;
        scene.collision->query_box(location, Vec3(reach, reach, reach), COLL_ShadowCast, found);
        const auto& triangles = scene.collision->triangles();
        std::vector<Vec3> poly, next;
        for (uint32_t index : found) {
            const CollisionWorld::Triangle& tri = triangles[index];
            // The side the bullet came to. What is edge-on to the decal takes none (BackfaceAngle 0.001).
            const Vec3 face = tri.normal.dot(n) < 0.0f ? tri.normal * -1.0f : tri.normal;
            if (face.dot(n) <= 0.001f) continue;
            if (tri.actor >= 0 && static_cast<size_t>(tri.actor) < scene.actors.size()) {
                const LevelActor& a = scene.actors[static_cast<size_t>(tri.actor)];
                if (a.is_hidden || !a.accepts_decals || !a.accepts_decals_in_game) continue;
            }
            poly.assign({tri.a, tri.b, tri.c});
            for (const auto& [dir, limit] : planes) {
                next.clear();
                for (size_t k = 0; k < poly.size(); ++k) {
                    const Vec3& a = poly[k];
                    const Vec3& c = poly[(k + 1) % poly.size()];
                    const float da = (a - location).dot(dir) - limit;
                    const float dc = (c - location).dot(dir) - limit;
                    if (da <= 0.0f) next.push_back(a);
                    if ((da < 0.0f && dc > 0.0f) || (da > 0.0f && dc < 0.0f)) next.push_back(a + (c - a) * (da / (da - dc)));
                }
                poly.swap(next);
                if (poly.size() < 3) break;
            }
            for (size_t k = 1; k + 1 < poly.size() && poly.size() >= 3; ++k) {
                emit(poly[0], face);
                emit(poly[k], face);
                emit(poly[k + 1], face);
            }
        }
    }
    if (decal.vertices.empty()) return false;
    if (scene.dynamic_decals.size() >= kMaxDynamicDecals) scene.dynamic_decals.erase(scene.dynamic_decals.begin());
    scene.dynamic_decals.push_back(std::move(decal));
    return true;
}

// The impact effect and the bullet hole of every bullet that has not left its mark yet, and the
// passing of `dt` seconds for the marks already there. `player` is where the player stands.
inline void update_impact_effects(LevelScene& scene, float dt, const Vec3& player) {
    static std::minstd_rand chance(20081111u);
    const auto frand = [&]() { return static_cast<float>(chance() % 32768u) / 32768.0f; };

    for (auto it = scene.spawned_effects.begin(); it != scene.spawned_effects.end();) {
        if (!it->forever) it->life -= dt;
        it = (!it->forever && it->life <= 0.0f) ? scene.spawned_effects.erase(it) : it + 1;
    }
    for (auto it = scene.dynamic_decals.begin(); it != scene.dynamic_decals.end();) {
        it->life -= dt;
        it = it->life <= 0.0f ? scene.dynamic_decals.erase(it) : it + 1;
    }

    const std::vector<PhysicalMaterialInfo>& physical = scene.physical_materials;
    const auto valid = [&](int32_t p) { return p >= 0 && static_cast<size_t>(p) < physical.size(); };
    for (BulletTracer& tracer : scene.active_tracers) {
        if (tracer.impact_done) continue;
        tracer.impact_done = true;
        if (tracer.hit_enemy || !scene.collision || physical.empty()) continue;
        Vec3 ray = tracer.end_pos - tracer.start_pos;
        const float length = ray.length();
        if (length < 1.0f) continue;
        ray = ray * (1.0f / length);
        // The surface the bullet stopped at: a mesh's own triangles there (its hull has no materials).
        const CollisionHit hit = scene.collision->line_check(tracer.end_pos - ray * std::min(16.0f, length), tracer.end_pos + ray * 16.0f, COLL_ShadowCast);
        if (!hit.hit) continue;
        if (!tracer.from_player && (hit.location - player).length() > kImpactEffectDistance) continue;
        const Vec3 n = hit.normal;
        bool shown = true;
        int32_t surface = -1;
        if (hit.actor >= 0 && static_cast<size_t>(hit.actor) < scene.actors.size()) {
            const LevelActor& a = scene.actors[static_cast<size_t>(hit.actor)];
            shown = !a.is_hidden;
            if (!a.element_physical.empty()) surface = a.element_physical[std::min<size_t>(hit.element, a.element_physical.size() - 1)];
        }
        const int ammo = std::min<int>(tracer.ammo, 3);

        // SpawnImpactEffects
        int32_t effect = -1;
        int guard = 0;
        for (int32_t p = surface; valid(p) && effect < 0 && guard < 16; p = physical[static_cast<size_t>(p)].parent, ++guard) {
            effect = physical[static_cast<size_t>(p)].effects[ammo];
        }
        if (effect < 0 && valid(scene.default_physical)) effect = physical[static_cast<size_t>(scene.default_physical)].effects[ammo];
        if (effect >= 0) {
            Vec3 reflection = ray - n * (2.0f * ray.dot(n));
            reflection = reflection + n * ((1.0f - reflection.dot(n)) * 0.3f);
            spawn_effect(scene, effect, hit.location, reflection, false);
        }

        // SpawnImpactDecal
        if (!shown) continue;
        const int type = ammo == 0 ? 0 : ammo == 3 ? 2 : 1;
        const float impact_angle = std::acos(std::clamp(-n.dot(ray), -1.0f, 1.0f)) * 57.29578f;
        const ImpactDecalInfo* decal = nullptr;
        guard = 0;
        for (int32_t p = surface; valid(p) && !decal && guard < 16; p = physical[static_cast<size_t>(p)].parent, ++guard) {
            const PhysicalMaterialInfo& m = physical[static_cast<size_t>(p)];
            if (!m.has_decals) continue;
            const std::vector<ImpactDecalInfo>& list = impact_angle < m.critical_angle ? m.impact[type] : m.ricochet[type];
            if (!list.empty()) decal = &list[chance() % list.size()];
        }
        if (!decal) decal = &scene.default_impact_decal;
        const float scaling = 1.0f + frand() * 0.5f;
        float stretching = 1.0f;
        Vec3 binormal = ray.cross(n);
        binormal = binormal.length_sq() > 1.0e-12f ? binormal.normalized() : Vec3(0.0f, 0.0f, 0.0f);
        const Vec3 tangent = n.cross(binormal);
        float rotation = decal->rotation;
        if (decal->rotation == 360.0f) {
            rotation = frand() * 360.0f;
        } else if (decal->rotation == -360.0f) {
            Vec3 upward(0.0f, 0.0f, 1.0f);
            if (std::abs(n.z) < 0.9999f) {
                upward = (upward - n * n.z).normalized();
            } else {
                upward = n.z > 0.0f ? Vec3(-1.0f, 0.0f, 0.0f) : Vec3(1.0f, 0.0f, 0.0f);
            }
            stretching += impact_angle / 90.0f * kDecalStretching;
            rotation = std::acos(std::clamp(upward.dot(tangent), -1.0f, 1.0f)) * 57.29578f;
            if (binormal.dot(upward) > 0.0f) rotation = 360.0f - rotation;
        }
        const bool made = spawn_decal(scene, *decal, hit.location, n, decal->width * scaling, decal->height * scaling * stretching, rotation);
        static const bool debug = std::getenv("ME_IMPACT_DEBUG") != nullptr;
        if (debug) {
            std::cout << "[Impact] at (" << hit.location.x << ", " << hit.location.y << ", " << hit.location.z << ") actor " << hit.actor
                      << " element " << hit.element << " on " << (valid(surface) ? physical[static_cast<size_t>(surface)].path : std::string("no physical material"))
                      << ": effect " << (effect >= 0 ? scene.particle_templates[static_cast<size_t>(effect)].path : std::string("none"))
                      << ", angle " << impact_angle << ", decal material " << decal->material << " " << decal->width * scaling << " x "
                      << decal->height * scaling * stretching << " turned " << rotation << (made ? ", made of " : ", not made")
                      << (made ? std::to_string(scene.dynamic_decals.back().vertices.size() / 3) + " triangles" : std::string()) << std::endl;
        }
    }
}

}  // namespace me
