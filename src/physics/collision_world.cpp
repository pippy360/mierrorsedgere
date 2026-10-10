#include "collision_world.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <numeric>

namespace me {

namespace {

constexpr uint32_t kLeafSize = 4;
constexpr float kParallelEpsilon = 1e-9f;
// A start-penetrating box that overlaps a triangle by no more than this along some separating axis is
// only touching it there. World-space vertices are good to about one float ulp of their coordinates
// (0.008 uu at |x| = 65536), so this leaves a wide margin while staying under the controller's 0.1 uu
// contact skin.
constexpr float kTouchDepth = 0.05f;

inline Vec3 vmin(const Vec3& a, const Vec3& b) {
    return Vec3(std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z));
}
inline Vec3 vmax(const Vec3& a, const Vec3& b) {
    return Vec3(std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z));
}
inline float axis_of(const Vec3& v, int axis) { return axis == 0 ? v.x : (axis == 1 ? v.y : v.z); }

inline AABB triangle_box(const CollisionWorld::Triangle& t) {
    return AABB(vmin(t.a, vmin(t.b, t.c)), vmax(t.a, vmax(t.b, t.c)));
}

inline bool slab_axis(float o, float inv, bool par, float bmin, float bmax, float& t0, float& t1) {
    if (par) return o >= bmin && o <= bmax;
    float ta = (bmin - o) * inv;
    float tb = (bmax - o) * inv;
    if (ta > tb) std::swap(ta, tb);
    t0 = std::max(t0, ta);
    t1 = std::min(t1, tb);
    return t0 <= t1;
}

// Segment (origin + t * dir, t in [0, t_max]) vs AABB slab test. Returns the entry time.
inline bool segment_hits_box(const Vec3& origin, const Vec3& inv_dir, const bool parallel[3], const AABB& box,
                             float t_max, float& t_entry) {
    float t0 = 0.0f;
    float t1 = t_max;
    if (!slab_axis(origin.x, inv_dir.x, parallel[0], box.min_pt.x, box.max_pt.x, t0, t1)) return false;
    if (!slab_axis(origin.y, inv_dir.y, parallel[1], box.min_pt.y, box.max_pt.y, t0, t1)) return false;
    if (!slab_axis(origin.z, inv_dir.z, parallel[2], box.min_pt.z, box.max_pt.z, t0, t1)) return false;
    t_entry = t0;
    return true;
}

// Separating-axis swept box vs triangle (the UE3 FindSeparatingAxis formulation used by
// the kDOP box checks). Vertices are relative to the box centre at t = 0.
struct SweepResult {
    bool hit = false;
    bool start_penetrating = false;
    float time = 0.0f;
    Vec3 normal;
};

SweepResult sweep_box_triangle(const Vec3& v0, const Vec3& v1, const Vec3& v2, const Vec3& tri_normal,
                               const Vec3& delta, const Vec3& extent, float max_time = 1.0f) {
    SweepResult r;
    float t_enter = -FLT_MAX;
    float t_exit = FLT_MAX;
    Vec3 n_enter = tri_normal;

    // Returns false when the axis separates the box and triangle for the whole sweep.
    auto test_axis = [&](Vec3 axis, bool prefer) -> bool {
        const float len2 = axis.length_sq();
        if (len2 < 1e-10f) return true;  // degenerate cross product: not a separating candidate
        axis = axis / std::sqrt(len2);
        const float p0 = axis.dot(v0);
        const float p1 = axis.dot(v1);
        const float p2 = axis.dot(v2);
        const float tri_min = std::min(p0, std::min(p1, p2));
        const float tri_max = std::max(p0, std::max(p1, p2));
        const float r_box = extent.x * std::abs(axis.x) + extent.y * std::abs(axis.y) + extent.z * std::abs(axis.z);
        const float lo = tri_min - r_box;
        const float hi = tri_max + r_box;
        const float s = axis.dot(delta);
        if (std::abs(s) < kParallelEpsilon) {
            return lo <= 0.0f && 0.0f <= hi;  // stationary along this axis
        }
        float ta = lo / s;
        float tb = hi / s;
        if (ta > tb) std::swap(ta, tb);
        // Contact normal faces against the motion along this axis.
        const Vec3 n = (s > 0.0f) ? -axis : axis;
        // The triangle face normal wins ties so contacts report the true surface normal.
        if (prefer ? (ta >= t_enter) : (ta > t_enter + 1e-6f)) {
            t_enter = ta;
            n_enter = n;
        }
        t_exit = std::min(t_exit, tb);
        return t_enter <= t_exit && t_exit >= 0.0f && t_enter <= max_time;
    };

    const Vec3 e0 = v1 - v0;
    const Vec3 e1 = v2 - v1;
    const Vec3 e2 = v0 - v2;
    const Vec3 ax[3] = {Vec3(1, 0, 0), Vec3(0, 1, 0), Vec3(0, 0, 1)};

    if (!test_axis(tri_normal, true)) return r;
    for (const Vec3& a : ax) {
        if (!test_axis(a, false)) return r;
    }
    for (const Vec3& a : ax) {
        if (!test_axis(a.cross(e0), false)) return r;
        if (!test_axis(a.cross(e1), false)) return r;
        if (!test_axis(a.cross(e2), false)) return r;
    }

    if (t_enter > t_exit || t_exit < 0.0f || t_enter > 1.0f) return r;

    if (t_enter < 0.0f) {
        // Already overlapping at the start: only block motion that pushes further into the
        // triangle (resting contacts, e.g. standing on a floor while walking, must not stick).
        // The contact normal is the minimum-translation direction out of the triangle. A resting
        // contact on the face itself keeps the face normal (so coplanar seams never catch), but a box
        // that only overlaps a triangle at its edge -- standing on the top edge of a ramp whose back
        // face it touches, or half over a roof edge whose side face it overlaps -- is separated along
        // the much shallower axis instead of being pushed through the face.
        const float side = tri_normal.dot(-v0);  // box centre relative to the triangle plane
        const Vec3 face_n = (side >= 0.0f) ? tri_normal : -tri_normal;
        const float face_r = extent.x * std::abs(tri_normal.x) + extent.y * std::abs(tri_normal.y) +
                             extent.z * std::abs(tri_normal.z);
        const float face_depth = std::max(0.0f, face_r - std::abs(side));
        // "Pushes further in" must be judged relative to the sweep length. Normals of world-space
        // triangles carry ~1e-4 of float noise (a vertical duct wall at y = -2809 has n.z = 3.6e-7 and
        // edge axes with n.z = 1.1e-4), so with an absolute epsilon a box merely grazing such a wall had
        // its 47 uu downward floor probe "blocked" by the wall at t = 0 and the pawn fell off a solid
        // floor. Motion within ~0.06 degrees of the contact plane is treated as sliding along it.
        const float into_eps = std::max(1e-6f, 1e-3f * delta.length());
        Vec3 n = face_n;
        float best_depth = face_depth - 0.25f;  // other axes must be clearly shallower to win
        auto consider_axis = [&](Vec3 axis) {
            const float len2 = axis.length_sq();
            if (len2 < 1e-10f) return;
            axis = axis / std::sqrt(len2);
            const float p0 = axis.dot(v0);
            const float p1 = axis.dot(v1);
            const float p2 = axis.dot(v2);
            const float tri_min = std::min(p0, std::min(p1, p2));
            const float tri_max = std::max(p0, std::max(p1, p2));
            const float r_box = extent.x * std::abs(axis.x) + extent.y * std::abs(axis.y) + extent.z * std::abs(axis.z);
            const float push_pos = std::max(0.0f, tri_max + r_box);  // move the box along +axis
            const float push_neg = std::max(0.0f, r_box - tri_min);  // move the box along -axis
            const float depth = std::min(push_pos, push_neg);
            if (depth < best_depth) {
                const Vec3 cand_n = (push_pos <= push_neg) ? axis : -axis;
                // Only let an edge/seam axis override the triangle face normal when either:
                //  (a) it supports the box from below (cand_n.z >= 0.7f, e.g. standing on a roof/ramp top edge), or
                //  (b) the sweep is actually moving into that edge (cand_n.dot(delta) < -into_eps), or
                //  (c) the box only touches the triangle along it (depth within float noise of zero). The box
                //      is then outside the triangle on that axis and can only collide by moving into it; a box
                //      standing against an air duct whose chamfer face plane slices through its top otherwise
                //      took a deeper edge axis instead and was "blocked" while sliding away from the duct.
                // Otherwise an internal seam between two wall/prop triangles perpendicular to delta would
                // replace face_n and cause n.dot(delta) >= 0 to falsely discard a solid wall!
                if (cand_n.z >= 0.7f || cand_n.dot(delta) < -into_eps || depth <= kTouchDepth) {
                    best_depth = depth;
                    n = cand_n;
                }
            }
        };
        for (const Vec3& a : ax) consider_axis(a);
        for (const Vec3& a : ax) {
            consider_axis(a.cross(e0));
            consider_axis(a.cross(e1));
            consider_axis(a.cross(e2));
        }
        if (n.dot(delta) >= -into_eps) return r;
        r.hit = true;
        r.start_penetrating = true;
        r.time = 0.0f;
        r.normal = n;
        return r;
    }

    r.hit = true;
    r.time = t_enter;
    r.normal = n_enter;
    if (r.normal.dot(delta) > 0.0f) r.normal = -r.normal;
    return r;
}

// Static SAT overlap of a box (centred at the origin) with a triangle.
bool box_overlaps_triangle(const Vec3& v0, const Vec3& v1, const Vec3& v2, const Vec3& tri_normal, const Vec3& extent) {
    auto separated = [&](const Vec3& axis) -> bool {
        const float len2 = axis.length_sq();
        if (len2 < 1e-10f) return false;
        const float p0 = axis.dot(v0);
        const float p1 = axis.dot(v1);
        const float p2 = axis.dot(v2);
        const float r_box = extent.x * std::abs(axis.x) + extent.y * std::abs(axis.y) + extent.z * std::abs(axis.z);
        return std::min(p0, std::min(p1, p2)) > r_box || std::max(p0, std::max(p1, p2)) < -r_box;
    };
    const Vec3 ax[3] = {Vec3(1, 0, 0), Vec3(0, 1, 0), Vec3(0, 0, 1)};
    for (const Vec3& a : ax) {
        if (separated(a)) return false;
    }
    if (separated(tri_normal)) return false;
    const Vec3 e[3] = {v1 - v0, v2 - v1, v0 - v2};
    for (const Vec3& a : ax) {
        for (const Vec3& ed : e) {
            if (separated(a.cross(ed))) return false;
        }
    }
    return true;
}

}  // namespace

void CollisionWorld::clear() {
    tris_.clear();
    nodes_.clear();
}

void CollisionWorld::reserve(size_t triangle_count) { tris_.reserve(triangle_count); }

void CollisionWorld::add_triangle(const Vec3& a, const Vec3& b, const Vec3& c, int32_t actor, uint8_t channels, uint16_t element) {
    add_triangle(a, b, c, actor, channels, element, channels);
}

void CollisionWorld::add_triangle(const Vec3& a, const Vec3& b, const Vec3& c, int32_t actor, uint8_t channels, uint16_t element,
                                  uint8_t role) {
    const Vec3 n = (b - a).cross(c - a);
    const float len2 = n.length_sq();
    if (!(len2 > 1e-8f) || !std::isfinite(len2)) return;
    Triangle t;
    t.a = a;
    t.b = b;
    t.c = c;
    t.normal = n / std::sqrt(len2);
    t.actor = actor;
    t.channels = channels;
    t.element = element;
    t.role = role;
    tris_.push_back(t);
}

void CollisionWorld::set_actor_channels(int32_t actor, uint8_t channels) {
    if (actor < 0) return;
    for (Triangle& t : tris_) {
        if (t.actor != actor) continue;
        t.channels = static_cast<uint8_t>((channels & COLL_BlockAll & t.role) | (channels != 0 ? (t.role & COLL_ShadowCast) : 0));
    }
}

void CollisionWorld::build() {
    nodes_.clear();
    if (tris_.empty()) return;

    const size_t n = tris_.size();
    std::vector<AABB> boxes(n);
    std::vector<Vec3> centroids(n);
    for (size_t i = 0; i < n; ++i) {
        boxes[i] = triangle_box(tris_[i]);
        centroids[i] = (tris_[i].a + tris_[i].b + tris_[i].c) * (1.0f / 3.0f);
    }
    std::vector<uint32_t> order(n);
    std::iota(order.begin(), order.end(), 0u);
    nodes_.reserve(2 * n / kLeafSize + 8);

    struct Task {
        uint32_t node;
        uint32_t begin;
        uint32_t end;
    };
    std::vector<Task> stack;
    nodes_.push_back(Node{});
    stack.push_back({0, 0, static_cast<uint32_t>(n)});
    while (!stack.empty()) {
        const Task task = stack.back();
        stack.pop_back();

        AABB box(Vec3(FLT_MAX, FLT_MAX, FLT_MAX), Vec3(-FLT_MAX, -FLT_MAX, -FLT_MAX));
        AABB cbox = box;
        for (uint32_t i = task.begin; i < task.end; ++i) {
            box.min_pt = vmin(box.min_pt, boxes[order[i]].min_pt);
            box.max_pt = vmax(box.max_pt, boxes[order[i]].max_pt);
            cbox.min_pt = vmin(cbox.min_pt, centroids[order[i]]);
            cbox.max_pt = vmax(cbox.max_pt, centroids[order[i]]);
        }
        nodes_[task.node].box = box;

        const uint32_t count = task.end - task.begin;
        const Vec3 cext = cbox.max_pt - cbox.min_pt;
        if (count <= kLeafSize || std::max(cext.x, std::max(cext.y, cext.z)) < 1e-3f) {
            nodes_[task.node].first = task.begin;
            nodes_[task.node].count = count;
            continue;
        }
        const int axis = (cext.x >= cext.y && cext.x >= cext.z) ? 0 : (cext.y >= cext.z ? 1 : 2);
        const uint32_t mid = task.begin + count / 2;
        std::nth_element(order.begin() + task.begin, order.begin() + mid, order.begin() + task.end,
                         [&](uint32_t l, uint32_t r) { return axis_of(centroids[l], axis) < axis_of(centroids[r], axis); });

        // Depth-first layout: the left child immediately follows its parent.
        const uint32_t left = static_cast<uint32_t>(nodes_.size());
        nodes_.push_back(Node{});
        const uint32_t right = static_cast<uint32_t>(nodes_.size());
        nodes_.push_back(Node{});
        nodes_[task.node].first = right;
        nodes_[task.node].count = 0;
        (void)left;
        stack.push_back({right, mid, task.end});
        stack.push_back({left, task.begin, mid});
    }

    // NOTE: with an explicit stack the left child is not always "this + 1"; store both children.
    // Re-lay the tree out depth-first so traversal can rely on left = this + 1.
    std::vector<Node> laid;
    laid.reserve(nodes_.size());
    struct Relay {
        uint32_t src;
        uint32_t dst_parent;  // index in `laid` whose right-child pointer must be patched (or UINT32_MAX)
    };
    std::vector<Relay> rstack;
    rstack.push_back({0, UINT32_MAX});
    while (!rstack.empty()) {
        const Relay r = rstack.back();
        rstack.pop_back();
        const uint32_t dst = static_cast<uint32_t>(laid.size());
        if (r.dst_parent != UINT32_MAX) laid[r.dst_parent].first = dst;
        const Node& src = nodes_[r.src];
        laid.push_back(src);
        if (src.count == 0) {
            const uint32_t src_left = src.first - 1;  // left was allocated right before right
            const uint32_t src_right = src.first;
            rstack.push_back({src_right, dst});        // patched when the right subtree is emitted
            rstack.push_back({src_left, UINT32_MAX});  // emitted next => dst + 1
        }
    }
    nodes_.swap(laid);

    std::vector<Triangle> sorted(n);
    for (size_t i = 0; i < n; ++i) sorted[i] = tris_[order[i]];
    tris_.swap(sorted);
}

CollisionHit CollisionWorld::sweep_box(const Vec3& start, const Vec3& delta, const Vec3& extent, uint8_t channels) const {
    CollisionHit best;
    best.time = 1.0f;
    best.location = start + delta;
    if (nodes_.empty()) return best;

    const bool parallel[3] = {std::abs(delta.x) < 1e-12f, std::abs(delta.y) < 1e-12f, std::abs(delta.z) < 1e-12f};
    const Vec3 inv(parallel[0] ? 0.0f : 1.0f / delta.x, parallel[1] ? 0.0f : 1.0f / delta.y,
                   parallel[2] ? 0.0f : 1.0f / delta.z);
    // Small inflation keeps grazing contacts inside the broad phase.
    const Vec3 grow = extent + Vec3(0.5f, 0.5f, 0.5f);

    struct StackEntry {
        uint32_t node;
        float t_entry;
    };
    StackEntry stack[64];
    int sp = 0;
    float root_entry = 0.0f;
    if (!segment_hits_box(start, inv, parallel, AABB(nodes_[0].box.min_pt - grow, nodes_[0].box.max_pt + grow), 1.0f,
                          root_entry)) {
        return best;
    }
    stack[sp++] = {0u, root_entry};
    float best_time = 1.0f;
    bool have_hit = false;
    while (sp > 0) {
        const StackEntry top = stack[--sp];
        if (have_hit && top.t_entry > best_time) continue;
        const Node& node = nodes_[top.node];
        if (node.count == 0) {
            const uint32_t left = top.node + 1;
            const uint32_t right = node.first;
            const float limit = have_hit ? best_time : 1.0f;
            float tl = 0.0f, tr = 0.0f;
            const bool hit_l = segment_hits_box(
                start, inv, parallel, AABB(nodes_[left].box.min_pt - grow, nodes_[left].box.max_pt + grow), limit, tl);
            const bool hit_r = segment_hits_box(
                start, inv, parallel, AABB(nodes_[right].box.min_pt - grow, nodes_[right].box.max_pt + grow), limit, tr);
            if (hit_l && hit_r) {
                if (sp + 2 <= 64) {
                    if (tr < tl) {
                        stack[sp++] = {left, tl};
                        stack[sp++] = {right, tr};  // right strictly closer -> visited first
                    } else {
                        stack[sp++] = {right, tr};
                        stack[sp++] = {left, tl};   // left visited first (preserves tie order)
                    }
                }
            } else if (hit_l && sp + 1 <= 64) {
                stack[sp++] = {left, tl};
            } else if (hit_r && sp + 1 <= 64) {
                stack[sp++] = {right, tr};
            }
            continue;
        }
        for (uint32_t i = node.first; i < node.first + node.count; ++i) {
            const Triangle& t = tris_[i];
            if ((t.channels & channels) == 0) continue;
            const SweepResult sr =
                sweep_box_triangle(t.a - start, t.b - start, t.c - start, t.normal, delta, extent,
                                   have_hit ? best_time : 1.0f);
            if (!sr.hit) continue;
            if (!have_hit || sr.time < best_time ||
                (sr.time == best_time && sr.normal.z > best.normal.z)) {
                have_hit = true;
                best_time = sr.time;
                best.hit = true;
                best.start_penetrating = sr.start_penetrating;
                best.time = sr.time;
                best.normal = sr.normal;
                best.location = start + delta * sr.time;
                best.actor = t.actor;
                best.element = t.element;
            }
        }
    }
    return best;
}

CollisionHit CollisionWorld::line_check(const Vec3& start, const Vec3& end, uint8_t channels) const {
    CollisionHit best;
    best.time = 1.0f;
    best.location = end;
    if (nodes_.empty()) return best;

    const Vec3 dir = end - start;
    const bool parallel[3] = {std::abs(dir.x) < 1e-12f, std::abs(dir.y) < 1e-12f, std::abs(dir.z) < 1e-12f};
    const Vec3 inv(parallel[0] ? 0.0f : 1.0f / dir.x, parallel[1] ? 0.0f : 1.0f / dir.y, parallel[2] ? 0.0f : 1.0f / dir.z);
    const Vec3 eps(0.01f, 0.01f, 0.01f);

    struct StackEntry {
        uint32_t node;
        float t_entry;
    };
    StackEntry stack[64];
    int sp = 0;
    float root_entry = 0.0f;
    if (!segment_hits_box(start, inv, parallel, AABB(nodes_[0].box.min_pt - eps, nodes_[0].box.max_pt + eps), 1.0f,
                          root_entry)) {
        return best;
    }
    stack[sp++] = {0u, root_entry};
    float best_time = 1.0f;
    bool have_hit = false;
    while (sp > 0) {
        const StackEntry top = stack[--sp];
        if (top.t_entry > best_time) continue;
        const Node& node = nodes_[top.node];
        if (node.count == 0) {
            const uint32_t left = top.node + 1;
            const uint32_t right = node.first;
            float tl = 0.0f, tr = 0.0f;
            const bool hit_l = segment_hits_box(
                start, inv, parallel, AABB(nodes_[left].box.min_pt - eps, nodes_[left].box.max_pt + eps), best_time, tl);
            const bool hit_r = segment_hits_box(
                start, inv, parallel, AABB(nodes_[right].box.min_pt - eps, nodes_[right].box.max_pt + eps), best_time, tr);
            if (hit_l && hit_r) {
                if (sp + 2 <= 64) {
                    if (tr < tl) {
                        stack[sp++] = {left, tl};
                        stack[sp++] = {right, tr};
                    } else {
                        stack[sp++] = {right, tr};
                        stack[sp++] = {left, tl};
                    }
                }
            } else if (hit_l && sp + 1 <= 64) {
                stack[sp++] = {left, tl};
            } else if (hit_r && sp + 1 <= 64) {
                stack[sp++] = {right, tr};
            }
            continue;
        }
        for (uint32_t i = node.first; i < node.first + node.count; ++i) {
            const Triangle& t = tris_[i];
            if ((t.channels & channels) == 0) continue;
            // Moller-Trumbore, double sided.
            const Vec3 e1 = t.b - t.a;
            const Vec3 e2 = t.c - t.a;
            const Vec3 p = dir.cross(e2);
            const float det = e1.dot(p);
            if (std::abs(det) < 1e-12f) continue;
            const float inv_det = 1.0f / det;
            const Vec3 s = start - t.a;
            const float u = s.dot(p) * inv_det;
            if (u < -1e-5f || u > 1.0f + 1e-5f) continue;
            const Vec3 q = s.cross(e1);
            const float v = dir.dot(q) * inv_det;
            if (v < -1e-5f || u + v > 1.0f + 1e-5f) continue;
            const float tt = e2.dot(q) * inv_det;
            if (tt < 0.0f || tt > best_time) continue;
            have_hit = true;
            best_time = tt;
            best.hit = true;
            best.time = tt;
            best.normal = (t.normal.dot(dir) > 0.0f) ? -t.normal : t.normal;
            best.location = start + dir * tt;
            best.actor = t.actor;
            best.element = t.element;
        }
    }
    (void)have_hit;
    return best;
}

bool CollisionWorld::overlap_box(const Vec3& center, const Vec3& extent, uint8_t channels) const {
    if (nodes_.empty()) return false;
    const AABB query(center - extent, center + extent);
    uint32_t stack[64];
    int sp = 0;
    stack[sp++] = 0;
    while (sp > 0) {
        const Node& node = nodes_[stack[--sp]];
        if (!node.box.intersects(query)) continue;
        if (node.count == 0) {
            const uint32_t self = static_cast<uint32_t>(&node - nodes_.data());
            if (sp + 2 <= 64) {
                stack[sp++] = node.first;
                stack[sp++] = self + 1;
            }
            continue;
        }
        for (uint32_t i = node.first; i < node.first + node.count; ++i) {
            const Triangle& t = tris_[i];
            if ((t.channels & channels) == 0) continue;
            if (box_overlaps_triangle(t.a - center, t.b - center, t.c - center, t.normal, extent)) return true;
        }
    }
    return false;
}

void CollisionWorld::query_box(const Vec3& center, const Vec3& extent, uint8_t channels, std::vector<uint32_t>& out) const {
    if (nodes_.empty()) return;
    const Vec3 lo = center - extent, hi = center + extent;
    const AABB query(lo, hi);
    uint32_t stack[64];
    int sp = 0;
    stack[sp++] = 0;
    while (sp > 0) {
        const Node& node = nodes_[stack[--sp]];
        if (!node.box.intersects(query)) continue;
        if (node.count == 0) {
            const uint32_t self = static_cast<uint32_t>(&node - nodes_.data());
            if (sp + 2 <= 64) {
                stack[sp++] = node.first;
                stack[sp++] = self + 1;
            }
            continue;
        }
        for (uint32_t i = node.first; i < node.first + node.count; ++i) {
            const Triangle& t = tris_[i];
            if ((t.channels & channels) == 0) continue;
            if (std::min({t.a.x, t.b.x, t.c.x}) > hi.x || std::max({t.a.x, t.b.x, t.c.x}) < lo.x) continue;
            if (std::min({t.a.y, t.b.y, t.c.y}) > hi.y || std::max({t.a.y, t.b.y, t.c.y}) < lo.y) continue;
            if (std::min({t.a.z, t.b.z, t.c.z}) > hi.z || std::max({t.a.z, t.b.z, t.c.z}) < lo.z) continue;
            out.push_back(i);
        }
    }
}

}  // namespace me
