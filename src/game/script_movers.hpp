#pragma once

// -----------------------------------------------------------------------------
// The actors a Matinee of the level's script moves (LevelScene::movers), and how they are moved:
// shared by the game loop, the oracle and the headless pictures, so that what is tested is what is
// played (docs/GAMEPLAY_SCRIPTING_RE.md section 9).
//
// In the game a movement track does nothing when its Matinee is updated (UInterpTrackMove's
// UpdateTrack is the empty base function). The actor moves itself, once a tick, while its Physics
// is PHYS_Interpolating (an InterpActor's default): AActor::physInterpolating (0x00ef9870) looks
// through the actor's LatentActions for a SeqAct_Interp with a group for it, takes that group's
// first movement track and calls AActor::MoveWithInterpMoveTrack (0x00f04130), which asks the track
// where the actor is at the Matinee's current Position and goes there with UWorld::MoveActor: a
// move that is placed, not swept (the level never stops a mover), and that, for an actor that
// collides, first asks UWorld::CheckEncroachment (0x00c12f50) about whoever is in the way.
// With no Matinee on it the actor stays where the last one left it; nothing puts it back.
//
// So here: the level script (game/level_script.cpp) owns the Positions and tells this file of them
// through the host's interp_* callbacks; update_script_movers, once a frame after the script's
// update, is every mover's physics tick.
// -----------------------------------------------------------------------------

#include "../math/types.hpp"
#include "../physics/collision_world.hpp"
#include "../physics/parkour_controller.hpp"
#include "level_script.hpp"
#include "matinee_move.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace me {

// The transform of what a mover is attached to (UInterpTrackMove's GetBaseMatrix, 0x00e3c6d0):
// FRotationTranslationMatrix(Base.Rotation, Base.Location), the identity for an actor with no Base.
// The Base is another mover, where that is now, or an actor that never moves.
inline Mat4 script_mover_base(const LevelScene& scene, const ScriptMover& m) {
    return m.base >= 0 && static_cast<size_t>(m.base) < scene.movers.size() ? scene.movers[static_cast<size_t>(m.base)].actor_tm : m.base_static;
}

// Puts a mover at `actor_tm`: what it is drawn with and collided in follow, and its place in its
// base's frame, unless it is its base that carried it there (`carried`: that place is kept).
inline void set_script_mover_transform(LevelScene& scene, ScriptMover& m, const Mat4& actor_tm, bool carried = false) {
    m.actor_tm = actor_tm;
    if (!carried) m.rel_tm = script_mover_base(scene, m).rigid_inverse() * actor_tm;
    m.delta = actor_tm * m.placed_inv;
    m.delta_inv = m.delta.rigid_inverse();
    float off = 0.0f;
    const Mat4 unit = Mat4::identity();
    for (int k = 0; k < 16; ++k) off = std::max(off, std::abs(m.delta.m[k] - unit.m[k]) * (k >= 12 ? 1.0f : 1000.0f));
    m.moved = off > 1.0e-3f;
    m.pure_yaw = std::abs(m.delta.m[10] - 1.0f) < 1.0e-5f;
    m.world_box = m.local_box;
    if (m.moved && m.collision) {
        m.world_box = AABB(Vec3(1e30f, 1e30f, 1e30f), Vec3(-1e30f, -1e30f, -1e30f));
        for (int k = 0; k < 8; ++k) {
            m.world_box.expand(m.delta.transform_point(Vec3((k & 1) ? m.local_box.max_pt.x : m.local_box.min_pt.x, (k & 2) ? m.local_box.max_pt.y : m.local_box.min_pt.y,
                                                             (k & 4) ? m.local_box.max_pt.z : m.local_box.min_pt.z)));
        }
    }
}

// The level is loaded again (a restart reloads the script levels: TdSPStoryGame.ResetLevel sets
// WorldInfo.bReloadScriptLevels): every mover is where the level places it, and no Matinee is on it.
inline void reset_script_movers(LevelScene& scene) {
    for (ScriptMover& m : scene.movers) {
        m.latent.clear();
        m.velocity = Vec3(0.0f, 0.0f, 0.0f);
        m.blocking_suspended = false;
    }
    for (ScriptMover& m : scene.movers) set_script_mover_transform(scene, m, m.placed);  // bases come before what is attached to them
    for (MatineeMove& mm : scene.matinee_moves) {
        mm.initial = Mat4::identity();
        mm.position = 0.0f;
    }
}

inline const MoveTrack* matinee_move_track(const LevelScene& scene, const MatineeMove& mm) {
    if (!scene.script || mm.node < 0 || static_cast<size_t>(mm.node) >= scene.script->nodes.size()) return nullptr;
    const int matinee = scene.script->nodes[static_cast<size_t>(mm.node)].matinee;
    if (matinee < 0 || static_cast<size_t>(matinee) >= scene.script->matinees.size()) return nullptr;
    const auto& moves = scene.script->matinees[static_cast<size_t>(matinee)].moves;
    return mm.move >= 0 && static_cast<size_t>(mm.move) < moves.size() ? &moves[static_cast<size_t>(mm.move)].track : nullptr;
}

// Where a mover's Matinee has it now: the first of its LatentActions that has a track for it
// (FindInterpMoveTrack, 0x00e51960). False when none moves it (no Matinee on it, or a track with no
// rotation keys, which the game leaves alone).
inline bool script_mover_target(const LevelScene& scene, const ScriptMover& m, Mat4& out) {
    if (m.latent.empty()) return false;
    const MatineeMove& mm = scene.matinee_moves[static_cast<size_t>(m.latent.front())];
    const MoveTrack* track = matinee_move_track(scene, mm);
    return track && move_track_transform(*track, mm.initial, script_mover_base(scene, m), mm.position, out);
}

// UWorld::MoveActor (0x00c12050) for a mover, with the pawn it may meet. An actor that collides
// (bCollideActors) and is PHYS_Interpolating is an encroacher: it is put at its new place and
// CheckEncroachment decides whether it may stay. What stands on it is not asked: it goes with it.
// Returns false when the move was refused (the mover is back where it was).
inline bool move_script_mover(LevelScene& scene, size_t index, const Mat4& target, ParkourController* pawn, bool carried = false) {
    ScriptMover& m = scene.movers[index];
    const Mat4 from = m.actor_tm;
    const Mat4 step = target * from.rigid_inverse();  // how the mover's own points go this move
    const LevelActor& actor = scene.actors[static_cast<size_t>(m.actor_index)];
    set_script_mover_transform(scene, m, target, carried);
    if (!pawn || !m.collision || !actor.is_collidable) return true;
    if (pawn->base_actor() == m.actor_index) {
        pawn->carried_by_mover(scene, step);
        return true;
    }
    if (pawn->encroached_by_mover(scene, index, step)) {
        set_script_mover_transform(scene, m, from);
        return false;
    }
    return true;
}

// Every mover's physics tick (AActor::physInterpolating), bases before what is attached to them.
// `pawn` is the player the movers carry, push and are stopped by; null where there is none.
inline void update_script_movers(LevelScene& scene, float dt, ParkourController* pawn = nullptr) {
    std::vector<char> went(scene.movers.size(), 0);  // which have moved this tick
    for (size_t i = 0; i < scene.movers.size(); ++i) {
        ScriptMover& m = scene.movers[i];
        Mat4 target;
        const bool tracked = script_mover_target(scene, m, target);
        if (!tracked) {
            // An actor attached to one that moved goes with it (its RelativeLocation and RelativeRotation
            // kept); any other with no Matinee on it stands (physInterpolating: Velocity zero).
            m.velocity = Vec3(0.0f, 0.0f, 0.0f);
            if (m.base < 0 || !went[static_cast<size_t>(m.base)]) continue;
            went[i] = 1;
            move_script_mover(scene, i, script_mover_base(scene, m) * m.rel_tm, pawn, true);
            continue;
        }
        float change = 0.0f;
        for (int k = 0; k < 16; ++k) change = std::max(change, std::abs(target.m[k] - m.actor_tm.m[k]) * (k >= 12 ? 1.0f : 1000.0f));
        const Vec3 old_origin(m.actor_tm.m[12], m.actor_tm.m[13], m.actor_tm.m[14]);
        // MoveWithInterpMoveTrack: Velocity is the way to go over the time before the move is tried (it is
        // what InterpActor.EncroachingOn reads), and the way gone over the time after it.
        if (dt > 1.0e-4f) m.velocity = (Vec3(target.m[12], target.m[13], target.m[14]) - old_origin) * (1.0f / dt);
        if (change <= 1.0e-4f) continue;
        went[i] = move_script_mover(scene, i, target, pawn) ? 1 : 0;
        if (dt > 1.0e-4f) m.velocity = (Vec3(m.actor_tm.m[12], m.actor_tm.m[13], m.actor_tm.m[14]) - old_origin) * (1.0f / dt);
    }
}

// The script's side: the host callbacks a SeqAct_Interp's playback calls (ScriptHost::interp_*).
inline void bind_scene_movers(ScriptHost& host, LevelScene& scene) {
    LevelScene* const s = &scene;
    // InitInterp -> UInterpTrackInstMove::CalcInitialTransform(false): from where the actor is and
    // the Position the Matinee has at this moment.
    host.interp_init = [s](int node, float position) {
        for (MatineeMove& mm : s->matinee_moves) {
            if (mm.node != node) continue;
            mm.position = position;
            const MoveTrack* track = matinee_move_track(*s, mm);
            if (track) mm.initial = move_initial_transform(*track, s->movers[static_cast<size_t>(mm.mover)].rel_tm, position);
        }
    };
    // ResetMovementInitialTransforms (0x00e51ca0) -> CalcInitialTransform(true): from where the actor is, at time 0.
    host.interp_reset_initial = [s](int node) {
        for (MatineeMove& mm : s->matinee_moves) {
            if (mm.node != node) continue;
            const MoveTrack* track = matinee_move_track(*s, mm);
            if (track) mm.initial = move_initial_transform(*track, s->movers[static_cast<size_t>(mm.mover)].rel_tm, 0.0f);
        }
    };
    host.interp_position = [s](int node, float position) {
        for (MatineeMove& mm : s->matinee_moves) {
            if (mm.node == node) mm.position = position;
        }
    };
    // Activated: the op goes on each linked actor's LatentActions. (The game then moves each at once,
    // before the op's first step; here that is this frame's update_script_movers, at the Position
    // the step has left: the same place by the frame's end.)
    host.interp_started = [s](int node) {
        for (size_t k = 0; k < s->matinee_moves.size(); ++k) {
            if (s->matinee_moves[k].node != node) continue;
            std::vector<int32_t>& latent = s->movers[static_cast<size_t>(s->matinee_moves[k].mover)].latent;
            if (std::find(latent.begin(), latent.end(), static_cast<int32_t>(k)) == latent.end()) latent.push_back(static_cast<int32_t>(k));
        }
    };
    host.interp_finished = [s](int node) {
        for (size_t k = 0; k < s->matinee_moves.size(); ++k) {
            if (s->matinee_moves[k].node != node) continue;
            std::vector<int32_t>& latent = s->movers[static_cast<size_t>(s->matinee_moves[k].mover)].latent;
            latent.erase(std::remove(latent.begin(), latent.end(), static_cast<int32_t>(k)), latent.end());
        }
    };
    // SetPosition (0x00e4f960): the Position is set and every linked actor that is PHYS_Interpolating
    // is moved there at once (physInterpolating with no time passing). Used by a cutscene's skip,
    // where nobody stands in the way: the actors are placed.
    host.interp_jump = [s](int node, float position) {
        for (size_t k = 0; k < s->matinee_moves.size(); ++k) {
            MatineeMove& mm = s->matinee_moves[k];
            if (mm.node != node) continue;
            mm.position = position;
            ScriptMover& m = s->movers[static_cast<size_t>(mm.mover)];
            if (m.latent.empty() || m.latent.front() != static_cast<int32_t>(k)) continue;
            Mat4 target;
            if (script_mover_target(*s, m, target)) set_script_mover_transform(*s, m, target);
        }
    };
}

}  // namespace me
