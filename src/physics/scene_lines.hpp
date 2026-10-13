#pragma once

// -----------------------------------------------------------------------------
// A line through what the level holds where it is now: LevelScene::collision, and the actors the
// level script's Matinees move (LevelScene::movers), whose triangles are kept apart, where the
// level placed them, and are met through each one's `delta` (game/script_movers.hpp). For the line
// checks outside the player's controller: a lens flare's coverage, a light environment's shadowing,
// the loader's floor snaps.
// -----------------------------------------------------------------------------

#include "../math/types.hpp"
#include "collision_world.hpp"

#include <algorithm>

namespace me {

struct SceneLines {
    const LevelScene* scene = nullptr;

    [[nodiscard]] CollisionHit line_check(const Vec3& start, const Vec3& end, uint8_t channels = COLL_BlockZeroExtent) const {
        CollisionHit best;
        if (!scene) return best;
        if (scene->collision) best = scene->collision->line_check(start, end, channels);
        const Vec3 lo(std::min(start.x, end.x), std::min(start.y, end.y), std::min(start.z, end.z));
        const Vec3 hi(std::max(start.x, end.x), std::max(start.y, end.y), std::max(start.z, end.z));
        for (const ScriptMover& m : scene->movers) {
            if (!m.collision || m.blocking_suspended) continue;
            const AABB& b = m.world_box;
            if (b.min_pt.x > hi.x || b.max_pt.x < lo.x || b.min_pt.y > hi.y || b.max_pt.y < lo.y || b.min_pt.z > hi.z || b.max_pt.z < lo.z) continue;
            CollisionHit h = m.moved ? m.collision->line_check(m.delta_inv.transform_point(start), m.delta_inv.transform_point(end), channels)
                                     : m.collision->line_check(start, end, channels);
            if (!h.hit || (best.hit && h.time >= best.time)) continue;
            if (m.moved) {
                h.location = m.delta.transform_point(h.location);
                h.normal = m.delta.transform_vector(h.normal);
            }
            best = h;
        }
        return best;
    }
};

}  // namespace me
