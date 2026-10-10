#pragma once

// -----------------------------------------------------------------------------
// What the game makes while it runs: a bullet's impact effect and its bullet hole, and the
// emitters of the script's actor factories (docs/RENDERING_RE.md, "Particles", "Decals";
// assets/level_impacts.hpp reads what they are made of, renderer/particles.hpp runs and
// draws them).
//
// TdWeapon.RegisterPendingImpact, for what a bullet stopped in: a bot gets nothing when another
// bot shot him or when he is dying (TdBotPawn.PreventWeaponImpactEffect); otherwise
// TdWeapon.PlayImpactEffects, which for anything but the player is the effect and the decal,
// and for everything the sound:
//
//   SpawnImpactEffects   the effect of the surface's physical material for this ammunition,
//                        else of its parents, else of the default material; made at the hit,
//                        pointed along the ray mirrored in the surface and lifted from it:
//                          R = D - 2 N (D . N);  R += (1 - R . N) 0.3 N
//                        A bot's surface is a body of his physics asset: PM_Character_Body.
//   SpawnImpactDecal     AngleOfImpact = acos(-N . D). The material's impact list under its
//                        CriticalAngle, its ricochet list over it, one of the list by chance;
//                        else its parents'; else the default 16 x 16 decal.
//                        RandScaling = 1 + FRand() 0.5. DecalRotation 360: any angle; -360:
//                        turned to the way the bullet went along the surface, and stretched
//                        by 1 + AngleOfImpact / 90 * DecalStretchingMultiplier (1.5).
//                        DecalManager.SpawnDecal(material, hit, rotator(-N), Width RandScaling,
//                        Height RandScaling Stretching, Thickness 10, .., HitComponent, ..): a box
//                        5 units to each side of the surface, 30 seconds, a hundred at a time,
//                        on the component that was hit: a hole in a door or a lift goes with it.
//   SpawnImpactSounds    the material's TdPhysicalMaterialImpactSounds.LightAmmo (every weapon
//                        plays that one), else its parents', else the default material's, at the hit.
//
// A spawned decal's frame follows from its Orientation and DecalRotation as a placed decal's
// does (checked on the 4225 placed ones that save an Orientation): with X, Y, Z the
// orientation's axes, HitNormal = -X, HitTangent = -Y cos - Z sin, HitBinormal = Z cos - Y sin.
//
// Not as the game: the surface is found by a short line check through the bullet's end
// against the meshes' own triangles and the BSP (the BSP's surfaces count as the default
// material); whether a bot's shot is relevant is only its distance from the player; a bot is
// his pawn's cylinder, all of it the body's material, and takes no decal.
// -----------------------------------------------------------------------------

#include "../math/types.hpp"
#include "../physics/collision_world.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>

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

// Where a part of a lift or a door is now, against where the level has it (the place its
// collision triangles are in): a lift's part is moved by its offset, a door turned about its hinge.
struct MoverPose {
    uint8_t kind = 0;  // 0 the level itself, 1 a lift's part, 2 a door
    int32_t mover = -1;
    int32_t part = -1;
    Vec3 offset{0.0f, 0.0f, 0.0f};
    Vec3 hinge{0.0f, 0.0f, 0.0f};
    float angle = 0.0f;

    [[nodiscard]] Vec3 turned(const Vec3& d, float by) const {
        const float c = std::cos(by), sn = std::sin(by);
        return Vec3(c * d.x - sn * d.y, sn * d.x + c * d.y, d.z);
    }
    [[nodiscard]] Vec3 dir_to_world(const Vec3& d) const { return kind == 2 ? turned(d, angle) : d; }
    [[nodiscard]] Vec3 dir_to_rest(const Vec3& d) const { return kind == 2 ? turned(d, -angle) : d; }
    [[nodiscard]] Vec3 to_world(const Vec3& p) const { return kind == 1 ? p + offset : kind == 2 ? hinge + turned(p - hinge, angle) : p; }
    [[nodiscard]] Vec3 to_rest(const Vec3& p) const { return kind == 1 ? p - offset : kind == 2 ? hinge + turned(p - hinge, -angle) : p; }
};

// The pose of a mover this frame, and its collision triangles. False when there is no such mover.
inline bool mover_pose(const LevelScene& scene, uint8_t kind, int32_t mover, int32_t part, MoverPose& out, const CollisionWorld** triangles = nullptr) {
    out = MoverPose{};
    out.kind = kind;
    out.mover = mover;
    out.part = part;
    if (kind == 0) {
        if (triangles) *triangles = scene.collision.get();
        return scene.collision != nullptr;
    }
    if (mover < 0 || part < 0) return false;
    if (kind == 1) {
        if (static_cast<size_t>(mover) >= scene.elevators.size()) return false;
        const ElevatorInstance& lift = scene.elevators[static_cast<size_t>(mover)];
        if (static_cast<size_t>(part) >= lift.parts.size()) return false;
        out.offset = lift.parts[static_cast<size_t>(part)].offset;
        if (triangles) *triangles = lift.parts[static_cast<size_t>(part)].collision.get();
        return true;
    }
    if (kind == 2) {
        if (static_cast<size_t>(mover) >= scene.barge_doors.size()) return false;
        const BargeDoorInstance& door = scene.barge_doors[static_cast<size_t>(mover)];
        if (static_cast<size_t>(part) >= door.parts.size()) return false;
        out.hinge = door.hinge_pos;
        out.angle = door.open_angle_rad;
        if (triangles) *triangles = door.parts[static_cast<size_t>(part)].collision.get();
        return true;
    }
    return false;
}

// What a line from `from` to `to` meets first: the level's own triangles, or those of a lift's
// part or a door where it is now. What is hidden (a pane's broken twin, waiting for the level's
// script) is passed through.
struct ImpactSurface {
    bool hit = false;
    float time = 2.0f;
    Vec3 location{0.0f, 0.0f, 0.0f};
    Vec3 normal{0.0f, 0.0f, 1.0f};
    int32_t actor = -1;
    uint16_t element = 0;
    MoverPose pose;
    const CollisionWorld* triangles = nullptr;  // in the pose's rest place
};

inline ImpactSurface find_impact_surface(const LevelScene& scene, const Vec3& from, const Vec3& to) {
    ImpactSurface best;
    Vec3 dir = to - from;
    const float whole = dir.length();
    if (whole < 1.0e-4f) return best;
    dir = dir * (1.0f / whole);
    const auto consider = [&](uint8_t kind, int32_t mover, int32_t part) {
        MoverPose pose;
        const CollisionWorld* world = nullptr;
        if (!mover_pose(scene, kind, mover, part, pose, &world) || !world) return;
        Vec3 a = pose.to_rest(from);
        const Vec3 b = pose.to_rest(to);
        const Vec3 along = pose.dir_to_rest(dir);
        CollisionHit h;
        for (int tries = 0; tries < 4; ++tries) {
            h = world->line_check(a, b, COLL_ShadowCast);
            if (!h.hit || h.actor < 0 || static_cast<size_t>(h.actor) >= scene.actors.size() ||
                !scene.actors[static_cast<size_t>(h.actor)].is_hidden) {
                break;
            }
            a = h.location + along * 0.25f;
            h = CollisionHit{};
            if ((b - a).dot(along) <= 0.0f) break;
        }
        if (!h.hit) return;
        const float time = (pose.to_world(h.location) - from).dot(dir) / whole;  // along the whole line
        if (time >= best.time) return;
        best.hit = true;
        best.time = time;
        best.location = pose.to_world(h.location);
        best.normal = pose.dir_to_world(h.normal);
        best.actor = h.actor;
        best.element = h.element;
        best.pose = pose;
        best.triangles = world;
    };
    consider(0, -1, -1);
    for (size_t e = 0; e < scene.elevators.size(); ++e) {
        for (size_t k = 0; k < scene.elevators[e].parts.size(); ++k) consider(1, static_cast<int32_t>(e), static_cast<int32_t>(k));
    }
    for (size_t d = 0; d < scene.barge_doors.size(); ++d) {
        const BargeDoorInstance& door = scene.barge_doors[d];
        for (size_t k = 0; k < door.parts.size(); ++k) {
            if (door.parts[k].is_blocker_only) continue;  // the doorway's hidden slab
            consider(2, static_cast<int32_t>(d), static_cast<int32_t>(k));
        }
    }
    return best;
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

// A decal on the surface `on`, at `location`: a box `width` x `height`, `kImpactDecalThickness`
// deep, facing `normal`, turned by `rotation_deg`. Clipped against the triangles of what was hit
// (the level's, or a mover's in the place the level has it: the decal is kept there and goes with
// the mover). False when nothing took it.
inline bool spawn_decal(LevelScene& scene, const ImpactDecalInfo& info, const ImpactSurface& on, const Vec3& location, const Vec3& normal,
                        float width, float height, float rotation_deg) {
    if (info.material < 0 || !on.triangles || !(width > 0.0f) || !(height > 0.0f)) return false;
    Vec3 x, y, z;
    direction_axes(normal * -1.0f, x, y, z);
    const float angle = rotation_deg * 0.01745329252f;
    const float cs = std::cos(angle), sn = std::sin(angle);
    // The decal's frame, in the place the triangles are in.
    const Vec3 origin = on.pose.to_rest(location);
    const Vec3 n = on.pose.dir_to_rest(x * -1.0f);
    const Vec3 t = on.pose.dir_to_rest(y * -cs - z * sn);
    const Vec3 b = on.pose.dir_to_rest(z * cs - y * sn);
    const float half_depth = kImpactDecalThickness * 0.5f;  // FarPlane = Thickness / 2, NearPlane = -FarPlane

    DynamicDecal decal;
    decal.material = info.material;
    decal.life = kDynamicDecalLife;
    decal.mover_kind = on.pose.kind;
    decal.mover = on.pose.mover;
    decal.part = on.pose.part;
    const auto emit = [&](const Vec3& p, const Vec3& face) {
        Vertex v;
        v.position = p;
        v.normal = face;
        v.tangent = t;
        v.u = v.u2 = 0.5f - (p - origin).dot(t) / width;
        v.v = v.v2 = 0.5f - (p - origin).dot(b) / height;
        decal.vertices.push_back(v);
    };
    if (info.no_clip) {
        // bNoClip: the whole quad, on the surface's plane.
        const Vec3 c[4] = {origin + t * (width * 0.5f) + b * (height * 0.5f), origin - t * (width * 0.5f) + b * (height * 0.5f),
                           origin - t * (width * 0.5f) - b * (height * 0.5f), origin + t * (width * 0.5f) - b * (height * 0.5f)};
        for (int k : {0, 1, 2, 0, 2, 3}) emit(c[k], n);
    } else {
        // The box's six planes, as (direction, limit): inside when dot(P - O, direction) <= limit.
        const std::pair<Vec3, float> planes[6] = {
            {t, width * 0.5f}, {t * -1.0f, width * 0.5f}, {b, height * 0.5f}, {b * -1.0f, height * 0.5f}, {n * -1.0f, half_depth}, {n, half_depth},
        };
        const float reach = 0.5f * std::sqrt(width * width + height * height) + half_depth;
        std::vector<uint32_t> found;
        on.triangles->query_box(origin, Vec3(reach, reach, reach), COLL_ShadowCast, found);
        const auto& triangles = on.triangles->triangles();
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
                    const float da = (a - origin).dot(dir) - limit;
                    const float dc = (c - origin).dot(dir) - limit;
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

// A bullet hole's triangles where what it lies on is now: the level's as they are, a mover's
// carried with it. False when its mover is no longer there.
inline bool dynamic_decal_vertices(const LevelScene& scene, const DynamicDecal& decal, std::vector<Vertex>& out) {
    out = decal.vertices;
    if (decal.mover_kind == 0) return true;
    MoverPose pose;
    if (!mover_pose(scene, decal.mover_kind, decal.mover, decal.part, pose)) return false;
    for (Vertex& v : out) {
        v.position = pose.to_world(v.position);
        v.normal = pose.dir_to_world(v.normal);
        v.tangent = pose.dir_to_world(v.tangent);
    }
    return true;
}

// The impact effect, the bullet hole and the impact sound of every bullet that has not left its
// mark yet, and the passing of `dt` seconds for the marks already there. `player` is where the
// player stands; the sounds are added to `sounds`, to be played at their place.
inline void update_impact_effects(LevelScene& scene, float dt, const Vec3& player, std::vector<SimSoundEvent>* sounds = nullptr) {
    static std::minstd_rand chance(20081111u);
    const auto frand = [&]() { return static_cast<float>(chance() % 32768u) / 32768.0f; };
    static const bool debug = std::getenv("ME_IMPACT_DEBUG") != nullptr;

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
    // GetImpactEffect / GetImpactSound up the material's parents, then the default material's.
    const auto effect_of = [&](int32_t surface, int ammo) {
        int32_t effect = -1;
        int guard = 0;
        for (int32_t p = surface; valid(p) && effect < 0 && guard < 16; p = physical[static_cast<size_t>(p)].parent, ++guard) {
            effect = physical[static_cast<size_t>(p)].effects[ammo];
        }
        if (effect < 0 && valid(scene.default_physical)) effect = physical[static_cast<size_t>(scene.default_physical)].effects[ammo];
        return effect;
    };
    const auto sound_of = [&](int32_t surface) -> const PhysicalMaterialInfo* {
        int guard = 0;
        for (int32_t p = surface; valid(p) && guard < 16; p = physical[static_cast<size_t>(p)].parent, ++guard) {
            if (!physical[static_cast<size_t>(p)].impact_sound.empty()) return &physical[static_cast<size_t>(p)];
        }
        if (valid(scene.default_physical) && !physical[static_cast<size_t>(scene.default_physical)].impact_sound.empty()) {
            return &physical[static_cast<size_t>(scene.default_physical)];
        }
        return nullptr;
    };
    // SpawnImpactEffects: the way the effect points, the ray mirrored in the surface and lifted from it.
    const auto reflected = [](const Vec3& ray, const Vec3& n) {
        const Vec3 r = ray - n * (2.0f * ray.dot(n));
        return r + n * ((1.0f - r.dot(n)) * 0.3f);
    };
    // SpawnImpactSounds: PlaySound(ImpactSound, .., HitLocation). Past the cue's MaxRadius it is not heard.
    const auto play = [&](int32_t surface, const Vec3& at) {
        const PhysicalMaterialInfo* m = sound_of(surface);
        if (!m) return std::string("none");
        if ((at - player).length() > m->impact_sound_radius) return m->impact_sound + " (too far to hear)";
        if (sounds) {
            SimSoundEvent ev;
            ev.cue = m->impact_sound;
            ev.location = at;
            ev.at_pawn = false;
            sounds->push_back(std::move(ev));
        }
        return m->impact_sound;
    };

    for (BulletTracer& tracer : scene.active_tracers) {
        if (tracer.impact_done) continue;
        tracer.impact_done = true;
        if (!scene.collision) continue;
        Vec3 ray = tracer.end_pos - tracer.start_pos;
        const float length = ray.length();
        if (length < 1.0f) continue;
        ray = ray * (1.0f / length);
        const int ammo = std::min<int>(tracer.ammo, 3);

        // A person: the player's shot leaves the body's effect and sound on a bot, a bot's shot
        // only the sound on the player, and nothing else leaves anything.
        if (tracer.pawn_hit != 0 || tracer.hit_enemy) {
            if (physical.empty() || (tracer.pawn_hit != 1 && tracer.pawn_hit != 2)) continue;
            const int32_t body = scene.character_physical;
            int32_t effect = -1;
            if (tracer.pawn_hit == 1 && (tracer.end_pos - player).length() <= kImpactEffectDistance) {
                effect = effect_of(body, ammo);
                const Vec3 n = tracer.pawn_normal.length_sq() > 1.0e-6f ? tracer.pawn_normal : ray * -1.0f;
                if (effect >= 0) spawn_effect(scene, effect, tracer.end_pos, reflected(ray, n), false);
            }
            const std::string heard = play(body, tracer.end_pos);
            if (debug) {
                std::cout << "[Impact] on " << (tracer.pawn_hit == 1 ? "a bot" : "the player") << " at (" << tracer.end_pos.x << ", " << tracer.end_pos.y
                          << ", " << tracer.end_pos.z << "): effect "
                          << (effect >= 0 ? scene.particle_templates[static_cast<size_t>(effect)].path : std::string("none")) << ", sound " << heard
                          << std::endl;
            }
            continue;
        }

        // The first thing on the bullet's last stretch that is there to be hit: what the level's
        // script has hidden (a pane's broken twin, waiting) is passed through.
        const Vec3 stretch_from = tracer.end_pos - ray * std::min(16.0f, length);
        const Vec3 stretch_to = tracer.end_pos + ray * 16.0f;
        const auto first_shown = [&](uint8_t channels) {
            Vec3 from = stretch_from;
            CollisionHit found;
            for (int tries = 0; tries < 4; ++tries) {
                found = scene.collision->line_check(from, stretch_to, channels);
                if (!found.hit || found.actor < 0 || static_cast<size_t>(found.actor) >= scene.actors.size() ||
                    !scene.actors[static_cast<size_t>(found.actor)].is_hidden) {
                    break;
                }
                from = found.location + ray * 0.25f;
                found = CollisionHit{};
                if ((stretch_to - from).dot(ray) <= 0.0f) break;
            }
            return found;
        };
        // Actor.TakeDamage: the level actor it struck, for the script's damage events.
        const CollisionHit struck = first_shown(COLL_BlockZeroExtent | COLL_ShadowCast);
        if (struck.hit && struck.actor >= 0 && static_cast<size_t>(struck.actor) < scene.actors.size() &&
            scene.actors[static_cast<size_t>(struck.actor)].script_damage) {
            scene.actor_damage.push_back(ActorDamage{struck.actor, tracer.damage, tracer.from_player, 0});
        }
        if (physical.empty()) continue;
        // The surface the bullet stopped at: a mesh's own triangles there (its hull has no
        // materials), the level's or a mover's.
        const ImpactSurface hit = find_impact_surface(scene, stretch_from, stretch_to);
        if (!hit.hit) continue;
        const Vec3 n = hit.normal;
        bool shown = true, takes_decals = true;
        int32_t surface = -1;
        if (hit.actor >= 0 && static_cast<size_t>(hit.actor) < scene.actors.size()) {
            const LevelActor& a = scene.actors[static_cast<size_t>(hit.actor)];
            shown = !a.is_hidden;
            takes_decals = a.accepts_decals && a.accepts_decals_in_game;
            if (!a.element_physical.empty()) surface = a.element_physical[std::min<size_t>(hit.element, a.element_physical.size() - 1)];
        }

        // SpawnImpactSounds
        const std::string heard = play(surface, hit.location);
        if (!tracer.from_player && (hit.location - player).length() > kImpactEffectDistance) continue;

        // SpawnImpactEffects
        const int32_t effect = effect_of(surface, ammo);
        if (effect >= 0) spawn_effect(scene, effect, hit.location, reflected(ray, n), false);

        // SpawnImpactDecal
        if (!shown) continue;
        const int type = ammo == 0 ? 0 : ammo == 3 ? 2 : 1;
        const float impact_angle = std::acos(std::clamp(-n.dot(ray), -1.0f, 1.0f)) * 57.29578f;
        const ImpactDecalInfo* decal = nullptr;
        int guard = 0;
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
        const bool made = spawn_decal(scene, *decal, hit, hit.location, n, decal->width * scaling, decal->height * scaling * stretching, rotation);
        if (debug) {
            static const char* const kOn[3] = {"", " (a lift's part)", " (a door)"};
            std::cout << "[Impact] at (" << hit.location.x << ", " << hit.location.y << ", " << hit.location.z << ") actor " << hit.actor
                      << kOn[std::min<int>(hit.pose.kind, 2)] << " element " << hit.element << " on "
                      << (valid(surface) ? physical[static_cast<size_t>(surface)].path : std::string("no physical material"))
                      << ": effect " << (effect >= 0 ? scene.particle_templates[static_cast<size_t>(effect)].path : std::string("none"))
                      << ", sound " << heard << ", angle " << impact_angle << ", decal material "
                      << decal->material << " " << decal->width * scaling << " x " << decal->height * scaling * stretching << " turned " << rotation
                      << (made ? ", made of " : takes_decals ? ", not made" : ", not made: what it hit takes no decals")
                      << (made ? std::to_string(scene.dynamic_decals.back().vertices.size() / 3) + " triangles" : std::string()) << std::endl;
        }
    }
}

}  // namespace me
