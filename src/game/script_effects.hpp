#pragma once

// -----------------------------------------------------------------------------
// What the level's script does to the scene's actors and effects, and what the scene
// tells the script: shared by the game loop and the oracle, so that what is tested
// is what is played (docs/GAMEPLAY_SCRIPTING_RE.md, "Damage events and breakable glass").
//
// A pane of glass in the story maps is two InterpActors in one place: the pane, and its
// broken twin, hidden. Each has a SeqEvent_TakeDamage (DamageThreshold 1):
//
//   the pane     -> SeqAct_ActorFactory (the cracking emitter), a sound,
//                   SeqAct_ToggleHidden (hide the pane) -> SeqAct_ChangeCollision (none)
//                   -> SeqAct_ToggleHidden (show the twin)
//   the twin     -> SeqAct_ActorFactory (the breaking emitter), a sound, SeqAct_Destroy
//
// so a first hit cracks it and a second shatters it. A second event on the pane, for
// TdDmgType_Barge only, also sends SeqAct_CauseDamage (100, TdDmgType_Bullet) to the twin:
// a barge goes straight through. Damage comes from Actor.TakeDamage: a bullet's
// (the weapon's damage), and 100 of TdDmgType_Barge from a barge, an air barge, a jump
// kick's bump, a crouched blow or a slide kick on an actor with bInteractable.
// -----------------------------------------------------------------------------

#include "../assets/ue3_props.hpp"
#include "../math/types.hpp"
#include "../physics/collision_world.hpp"
#include "impact_effects.hpp"
#include "level_script.hpp"

#include <cmath>
#include <string>

namespace me {

// The level actor a script actor is: the same package (stems, any case) and name.
inline bool is_script_actor(const LevelActor& a, const ScriptActor& sa) {
    return a.unique_name == sa.name && to_lower(a.source_package) == to_lower(sa.package);
}

// What an actor collides with, in its flags and in the collision world (SeqAct_ChangeCollision).
inline void set_actor_collision(LevelScene& scene, size_t index, bool collidable, bool blocks_traces) {
    LevelActor& a = scene.actors[index];
    a.is_collidable = collidable;
    a.blocks_traces = blocks_traces;
    if (!scene.collision) return;
    uint8_t channels = 0;
    if (a.is_collidable) channels |= COLL_BlockNonZeroExtent;
    if (a.blocks_traces) channels |= COLL_BlockZeroExtent;
    scene.collision->set_actor_channels(static_cast<int32_t>(index), channels);
}

// A checkpoint is reloaded: what the script hid, showed, destroyed or gave collision is as the level
// had it (the panes are whole again), and what the game made on the way is gone.
inline void restore_script_actors(LevelScene& scene) {
    for (size_t i = 0; i < scene.actors.size(); ++i) {
        LevelActor& a = scene.actors[i];
        if (!a.script_switched && !a.script_collision) continue;
        a.is_hidden = a.initial_hidden;
        if (a.is_collidable != a.initial_collidable || a.blocks_traces != a.initial_blocks_traces) {
            set_actor_collision(scene, i, a.initial_collidable, a.initial_blocks_traces);
        }
    }
    scene.dynamic_decals.clear();
    scene.spawned_effects.clear();
    scene.actor_damage.clear();
}

// Gives `host` the callbacks that act on `scene`, which has to outlive the host.
inline void bind_scene_effects(ScriptHost& host, LevelScene& scene) {
    LevelScene* const s = &scene;
    // The level's script switches an emitter or a lens flare on or off, or hides it.
    host.toggle_effect = [s](const ScriptActor& sa, int action) {
        const std::string package = to_lower(sa.package);
        for (ParticleSystemPlacement& p : s->particle_systems) {
            if (p.export_index != sa.export_index || p.package != package) continue;
            const bool on = action == 0 || (action == 2 && !p.active);
            if (on) ++p.activations;  // a system that has run out starts again
            p.active = on;
        }
        for (LensFlareSourceInfo& f : s->lens_flares) {
            if (f.export_index != sa.export_index || f.package != package) continue;
            f.active = action == 0 || (action == 2 && !f.active);
        }
    };
    host.spawn_effect = [s](const std::string& package, int32_t factory_export, const ScriptActor& at) {
        const std::string stem = to_lower(package);
        for (const EffectFactory& f : s->effect_factories) {
            if (f.export_index != factory_export || f.package != stem) continue;
            const float yaw = at.yaw_deg * 0.01745329252f, pitch = at.pitch_deg * 0.01745329252f;
            spawn_effect(*s, f.template_index, at.location,
                         Vec3(std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch)), true);
        }
    };
    host.hide_effect = [s](const ScriptActor& sa, bool hidden) {
        const std::string package = to_lower(sa.package);
        for (ParticleSystemPlacement& p : s->particle_systems) {
            if (p.export_index == sa.export_index && p.package == package) p.active = !hidden && p.active;
        }
        for (LensFlareSourceInfo& f : s->lens_flares) {
            if (f.export_index == sa.export_index && f.package == package) f.hidden = hidden;
        }
    };
    // SeqAct_ChangeCollision, and a SeqAct_Toggle on a volume.
    const auto set_collision = [s](size_t index, bool collide_actors, bool block_actors) {
        LevelActor& a = s->actors[index];
        if (!collide_actors) {
            a.is_electric_volume = false;
            a.is_barbed_wire_volume = false;
            a.is_movement_exclusion_volume = false;
        }
        set_actor_collision(*s, index, collide_actors && block_actors, collide_actors && block_actors);
    };
    host.change_collision = [s, set_collision](const ScriptActor& sa, bool collide_actors, bool block_actors) {
        for (size_t i = 0; i < s->actors.size(); ++i) {
            if (is_script_actor(s->actors[i], sa)) set_collision(i, collide_actors, block_actors);
        }
    };
    // SeqAct_ToggleHidden / SeqAct_Destroy on a mesh: 0 hide, 1 show, 2 the other way, 3 destroyed.
    host.hide_actor = [s, set_collision](const ScriptActor& sa, int action) {
        for (size_t i = 0; i < s->actors.size(); ++i) {
            LevelActor& a = s->actors[i];
            if (!is_script_actor(a, sa)) continue;
            const bool hide = action == 0 || action == 3 || (action == 2 && !a.is_hidden);
            a.is_hidden = hide;
            if (action == 3) set_collision(i, false, false);
            if (!hide) continue;
            // The bullet holes on it go with it.
            AABB box = a.world_bounds;
            box.min_pt = box.min_pt - Vec3(6.0f, 6.0f, 6.0f);
            box.max_pt = box.max_pt + Vec3(6.0f, 6.0f, 6.0f);
            for (auto it = s->dynamic_decals.begin(); it != s->dynamic_decals.end();) {
                const Vec3 p = it->vertices.empty() ? Vec3(1.0e30f, 0.0f, 0.0f) : it->vertices.front().position;
                const bool on_it = p.x >= box.min_pt.x && p.x <= box.max_pt.x && p.y >= box.min_pt.y && p.y <= box.max_pt.y &&
                                   p.z >= box.min_pt.z && p.z <= box.max_pt.z;
                it = on_it ? s->dynamic_decals.erase(it) : it + 1;
            }
        }
    };
}

// Tells the script of the damage the scene's actors took since the last call (the bullets', the
// barges'). A barge door is the port's own (LevelScene::barge_doors): its sequence is not run.
inline void deliver_actor_damage(LevelScript& script, LevelScene& scene) {
    if (scene.actor_damage.empty()) return;
    std::vector<ActorDamage> list;
    list.swap(scene.actor_damage);
    if (!script.valid()) return;
    static const char* const kTypes[3] = {"tddmgtype_bullet", "tddmgtype_barge", "tddmgtype_melee"};
    for (const ActorDamage& d : list) {
        if (d.actor < 0 || static_cast<size_t>(d.actor) >= scene.actors.size()) continue;
        const LevelActor& a = scene.actors[static_cast<size_t>(d.actor)];
        if (a.barge_door >= 0) continue;
        script.damage_actor(to_lower(a.source_package), a.unique_name, d.amount, d.by_player, kTypes[d.type < 3 ? d.type : 0]);
    }
}

}  // namespace me
