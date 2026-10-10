#pragma once

// -----------------------------------------------------------------------------
// CollisionWorld: triangle-soup collision for real Mirror's Edge level geometry.
//
// Replaces the old per-actor AABB "colliders" (and the procedural append_box test
// course) with the geometry UE3 itself collides against:
//   * UStaticMesh RB_BodySetup.AggGeom simple collision (KConvexElem hulls, KBoxElem,
//     KSphereElem, KSphylElem) when UseSimpleBoxCollision / UseSimpleLineCollision apply,
//   * otherwise the per-poly kDOP tree triangles (FkDOPCollisionTriangle over the LOD0
//     PositionVertexBuffer),
//   * BlockingVolume BrushComponent.BrushAggGeom convex hulls.
//
// Queries mirror UE3's two check flavours:
//   * sweep_box()  - non-zero-extent check: an axis-aligned box (the pawn cylinder
//                    approximated by its extent, exactly like UE3 MoveActor/SingleLineCheck
//                    with Extent) swept along a segment, separating-axis test per triangle.
//   * line_check() - zero-extent check (traces, bullets, movement probes).
// -----------------------------------------------------------------------------

#include "../math/types.hpp"

#include <cstdint>
#include <vector>

namespace me {

// UE3 PrimitiveComponent blocking channels stored per triangle.
enum CollisionChannel : uint8_t {
    COLL_BlockNonZeroExtent = 1u << 0,  // blocks swept extent checks (player / AI movement)
    COLL_BlockZeroExtent = 1u << 1,     // blocks zero-extent line checks (traces, bullets)
    COLL_BlockAll = COLL_BlockNonZeroExtent | COLL_BlockZeroExtent,
    // What a light's line of sight is tested against (TRACE_ShadowCast): a mesh's own triangles,
    // never its simplified hull, whatever its UseSimple*Collision flags say. A triangle that has
    // only this bit takes no part in movement or in traces.
    COLL_ShadowCast = 1u << 2,
};

struct CollisionHit {
    bool hit = false;
    bool start_penetrating = false;  // query started overlapping the hit triangle (time == 0)
    float time = 1.0f;               // fraction of the query delta at first contact
    Vec3 normal{0.0f, 0.0f, 1.0f};   // contact normal, facing the query (against the motion)
    Vec3 location{0.0f, 0.0f, 0.0f}; // box centre / line point at the time of contact
    int32_t actor = -1;              // LevelScene::actors index of the owning actor (-1 = none)
};

class CollisionWorld {
public:
    struct Triangle {
        Vec3 a, b, c;
        Vec3 normal;           // unit geometric normal (a, b, c winding)
        int32_t actor = -1;
        uint8_t channels = COLL_BlockAll;
    };

    void clear();
    void reserve(size_t triangle_count);
    // Adds a world-space triangle; degenerate (zero-area) triangles are dropped.
    void add_triangle(const Vec3& a, const Vec3& b, const Vec3& c, int32_t actor, uint8_t channels);
    // Builds the bounding volume hierarchy. Must be called before querying.
    void build();

    [[nodiscard]] bool empty() const { return tris_.empty(); }
    [[nodiscard]] size_t triangle_count() const { return tris_.size(); }
    [[nodiscard]] const std::vector<Triangle>& triangles() const { return tris_; }
    [[nodiscard]] AABB bounds() const { return nodes_.empty() ? AABB() : nodes_.front().box; }

    // Sweeps an axis-aligned box (centre `start`, half-size `extent`) along `delta`.
    // Only triangles with any of `channels` set are considered.
    [[nodiscard]] CollisionHit sweep_box(const Vec3& start, const Vec3& delta, const Vec3& extent,
                                         uint8_t channels = COLL_BlockNonZeroExtent) const;

    // Zero-extent segment check from `start` to `end`.
    [[nodiscard]] CollisionHit line_check(const Vec3& start, const Vec3& end,
                                          uint8_t channels = COLL_BlockZeroExtent) const;

    // Static overlap test of an axis-aligned box against the triangles.
    [[nodiscard]] bool overlap_box(const Vec3& center, const Vec3& extent,
                                   uint8_t channels = COLL_BlockNonZeroExtent) const;

private:
    struct Node {
        AABB box;
        uint32_t first = 0;  // leaf: first triangle; internal: index of the right child
        uint32_t count = 0;  // leaf: triangle count (> 0); internal: 0 (left child = this + 1)
    };

    std::vector<Triangle> tris_;
    std::vector<Node> nodes_;
};

}  // namespace me
