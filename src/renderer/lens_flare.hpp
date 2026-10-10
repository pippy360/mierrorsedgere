#pragma once

// -----------------------------------------------------------------------------
// Lens flares, as the game's executable draws them (docs/RENDERING_RE.md, "Lens flares";
// FLensFlareSceneProxy::DrawDynamicElements 0x010a0d80, RenderReflections 0x010a0230,
// GetElementValues 0x0109efc0, CheckViewStatus 0x0109f450).
//
// A flare is its template's quads strung along the line from the source's place on the
// screen, S, through the screen's centre: the quad of ray distance r sits at
// E = S * (1 - 2 r) in normalized device coordinates. A quad's size on screen does not
// depend on how far the source is: its width in pixels is
//   (view width / 2) * Proj[0][0] * Size.x * Scaling * AxisScaling.x
// with AxisScaling already times DistMap_Scale(distance to the source); a positive
// Rotation turns it clockwise. Each quad is drawn with its own additive material, which is
// given the quad's colour (unclamped, linear) and three numbers:
//   LensFlareRadialDistance   |S| * |r|            (the shipped shader swaps the two
//   LensFlareSourceDistance   |E|                   distances the native code fills in)
//   LensFlareOcclusion        ScreenPercentageMap(part of the view the source's bounds cover, unhidden)
//   LensFlareIntensity        the cone's strength, 1 for a template without a cone
//
// The one thing done another way: the game takes the covered part from an occlusion query of
// the source's bounding box (bounds * 1.1 + 1.1), a frame late. Here it is the box's outline
// on the screen, measured, times the part of a grid of sight lines to it that the level's
// meshes leave open (their own triangles, as for a light's line of sight; what has no
// collision at all does not hide a flare here).
// -----------------------------------------------------------------------------

#include "../math/types.hpp"
#include "../physics/collision_world.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace me {

// FRawDistribution::GetEntry (0x00d72100) and the value of an Op 1 table: up to three components.
inline void raw_distribution_value(const RawDistribution& d, float x, float out[3]) {
    out[0] = out[1] = out[2] = 0.0f;
    if (!d.valid) return;
    const int chunk = d.chunk;
    const int last = static_cast<int>(d.table.size()) - chunk;
    float t = (x - d.start_time) * d.time_scale;
    if (!(t >= 0.0f)) t = 0.0f;
    const float top = static_cast<float>((last - 2) / chunk);
    int i = static_cast<int>(top);
    float alpha = 0.0f;
    if (t < top) {
        i = static_cast<int>(t);
        alpha = t - static_cast<float>(i);
    }
    const int e1 = std::min(2 + chunk * i, last);
    const int e2 = std::min(2 + chunk * i + chunk, last);
    for (int c = 0; c < std::min(chunk, 3); ++c) {
        const float a = d.table[static_cast<size_t>(e1 + c)];
        const float b = d.table[static_cast<size_t>(e2 + c)];
        out[c] = a + (b - a) * alpha;
    }
}
inline float raw_distribution_float(const RawDistribution& d, float x) {
    float v[3];
    raw_distribution_value(d, x, v);
    return v[0];
}

// The view a frame is drawn from.
struct LensFlareView {
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 forward{1.0f, 0.0f, 0.0f};  // unit; right and up as the screen has them
    Vec3 right{0.0f, 1.0f, 0.0f};
    Vec3 up{0.0f, 0.0f, 1.0f};
    float proj_x = 1.0f;   // Proj[0][0]: 1 / tan(half the horizontal field of view)
    float proj_y = 1.0f;   // Proj[1][1]
    float width = 1280.0f;  // in pixels
    float height = 720.0f;
    float near_plane = 5.0f;
};

// One quad to draw, additive, with its material.
struct LensFlareQuad {
    int32_t material = -1;  // SceneMaterialLibrary::materials
    float corner[4][2];     // normalized device coordinates (x right, y up) of the quad's uv (0,0), (0,1), (1,0), (1,1)
    float color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float radial_distance = 0.0f;  // what the material's LensFlareRadialDistance node gives
    float source_distance = 0.0f;  // LensFlareSourceDistance
    float ray_distance = 0.0f;
    float occlusion = 1.0f;
    float intensity = 1.0f;
};

namespace lensflare_detail {

struct P2 {
    float x, y;
};

inline float cross(const P2& o, const P2& a, const P2& b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }

// The area of the convex hull of `pts` inside the rectangle (0, 0)..(w, h).
inline float hull_area_in_view(std::vector<P2> pts, float w, float h) {
    if (pts.size() < 3) return 0.0f;
    std::sort(pts.begin(), pts.end(), [](const P2& a, const P2& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    std::vector<P2> hull(pts.size() * 2);
    size_t k = 0;
    for (size_t i = 0; i < pts.size(); ++i) {
        while (k >= 2 && cross(hull[k - 2], hull[k - 1], pts[i]) <= 0.0f) --k;
        hull[k++] = pts[i];
    }
    for (size_t i = pts.size() - 1, lower = k + 1; i > 0; --i) {
        while (k >= lower && cross(hull[k - 2], hull[k - 1], pts[i - 1]) <= 0.0f) --k;
        hull[k++] = pts[i - 1];
    }
    hull.resize(k > 0 ? k - 1 : 0);
    // Sutherland-Hodgman against the four sides of the view.
    for (int side = 0; side < 4 && hull.size() >= 3; ++side) {
        const auto inside = [&](const P2& p) {
            switch (side) {
                case 0: return p.x >= 0.0f;
                case 1: return p.x <= w;
                case 2: return p.y >= 0.0f;
                default: return p.y <= h;
            }
        };
        const auto cut = [&](const P2& a, const P2& b) {
            float t = 0.0f;
            switch (side) {
                case 0: t = (0.0f - a.x) / (b.x - a.x); break;
                case 1: t = (w - a.x) / (b.x - a.x); break;
                case 2: t = (0.0f - a.y) / (b.y - a.y); break;
                default: t = (h - a.y) / (b.y - a.y); break;
            }
            return P2{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
        };
        std::vector<P2> next;
        for (size_t i = 0; i < hull.size(); ++i) {
            const P2& a = hull[i];
            const P2& b = hull[(i + 1) % hull.size()];
            const bool in_a = inside(a), in_b = inside(b);
            if (in_a && in_b) {
                next.push_back(b);
            } else if (in_a && !in_b) {
                next.push_back(cut(a, b));
            } else if (!in_a && in_b) {
                next.push_back(cut(a, b));
                next.push_back(b);
            }
        }
        hull.swap(next);
    }
    if (hull.size() < 3) return 0.0f;
    float twice = 0.0f;
    for (size_t i = 0; i < hull.size(); ++i) {
        const P2& a = hull[i];
        const P2& b = hull[(i + 1) % hull.size()];
        twice += a.x * b.y - b.x * a.y;
    }
    return std::abs(twice) * 0.5f;
}

}  // namespace lensflare_detail

// The part of the view's pixels the source's bounding box covers, unhidden (0..1): what the game
// reads back from the box's occlusion query. 1 when the view is inside the box or touches it (the
// game issues no query then, and takes 1).
inline float lens_flare_coverage(const LensFlareTemplate& t, const Vec3& location, const LensFlareView& view,
                                 const CollisionWorld* collision) {
    using lensflare_detail::P2;
    const Vec3 centre = location + t.box_center;
    const Vec3 half = t.box_extent * 1.1f + Vec3(1.1f, 1.1f, 1.1f);
    std::vector<P2> pts;
    pts.reserve(8);
    for (int c = 0; c < 8; ++c) {
        const Vec3 corner = centre + Vec3((c & 1) ? half.x : -half.x, (c & 2) ? half.y : -half.y, (c & 4) ? half.z : -half.z);
        const Vec3 d = corner - view.position;
        const float z = d.dot(view.forward);
        if (z <= view.near_plane) return 1.0f;
        const float nx = d.dot(view.right) * view.proj_x / z;
        const float ny = d.dot(view.up) * view.proj_y / z;
        pts.push_back(P2{(nx * 0.5f + 0.5f) * view.width, (0.5f - ny * 0.5f) * view.height});
    }
    const float area = lensflare_detail::hull_area_in_view(pts, view.width, view.height);
    if (area <= 0.0f) return 0.0f;
    float open = 1.0f;
    if (collision) {
        // Sight lines to a grid over the box's face towards the view (its far side, for a box the
        // template stores inside out: the game then rasterizes the faces turned away).
        const float size = (half.x + half.y + half.z) / 3.0f;
        const int n = size > 1000.0f ? 5 : 3;
        const Vec3 to_view = (view.position - centre).normalized();
        const Vec3 face = centre + to_view * (t.box_inverted ? -size : size);
        int clear = 0;
        for (int iy = 0; iy < n; ++iy) {
            for (int ix = 0; ix < n; ++ix) {
                const float a = (static_cast<float>(ix) / static_cast<float>(n - 1) - 0.5f) * 1.4f * size;
                const float b = (static_cast<float>(iy) / static_cast<float>(n - 1) - 0.5f) * 1.4f * size;
                const Vec3 target = face + view.right * a + view.up * b;
                if (!collision->line_check(view.position, target, COLL_ShadowCast).hit) ++clear;
            }
        }
        open = static_cast<float>(clear) / static_cast<float>(n * n);
    }
    return std::clamp(area / (view.width * view.height) * open, 0.0f, 1.0f);
}

// CheckViewStatus (0x0109f450): the cone's strength towards the view, or a negative number when
// the flare is not drawn at all.
inline float lens_flare_cone_strength(const LensFlareTemplate& t, const LensFlareSourceInfo& source, const LensFlareView& view) {
    if (t.outer_cone == 0.0f && t.radius == 0.0f) return 1.0f;
    const Vec3 to_source = source.location - view.position;
    if (t.radius != 0.0f && to_source.length() > t.radius) return -1.0f;
    if (t.outer_cone == 0.0f) return 1.0f;
    const float deg = 180.0f / 3.14159265358979f;
    const Vec3 l = source.forward.normalized();
    const Vec3 v = view.forward;
    const Vec3 d = to_source.normalized();
    const float a1_abs = std::acos(std::clamp(-d.dot(l), -1.0f, 1.0f)) * deg;
    if (a1_abs > 90.0f) return -1.0f;  // the view is behind the lamp
    const float a2_abs = std::acos(std::clamp(d.dot(v), -1.0f, 1.0f)) * deg;
    const float a1 = (d.x * l.y - d.y * l.x < 0.0f) ? -a1_abs : a1_abs;
    const float a2 = (d.y * v.x - d.x * v.y < 0.0f) ? -a2_abs : a2_abs;
    const float total = std::abs((a1 + a2) * t.cone_fudge_factor);
    const float inner = std::clamp(t.inner_cone, 0.0f, 89.99f);
    const float outer = std::min(std::max(t.outer_cone, inner + 0.001f), 89.991f);
    if (total <= inner) return 1.0f;
    if (total <= outer) return 1.0f - (total - inner) / (outer - inner);
    return -1.0f;
}

// Every quad of every flare in view, in drawing order.
inline void build_lens_flare_quads(const LevelScene& scene, const LensFlareView& view, std::vector<LensFlareQuad>& out) {
    out.clear();
    static const bool off = std::getenv("ME_NO_LENS_FLARES") != nullptr;  // to see a picture without them
    if (off) return;
    const float tan_x = 1.0f / view.proj_x;
    const float tan_y = 1.0f / view.proj_y;
    const float side_x = std::sqrt(1.0f + tan_x * tan_x);
    const float side_y = std::sqrt(1.0f + tan_y * tan_y);
    const float f_px = view.width * 0.5f * view.proj_x;
    // ME_LENS_FLARE_DEBUG=1: what becomes of every source, once a second or so.
    static const bool debug_on = std::getenv("ME_LENS_FLARE_DEBUG") != nullptr;
    static int debug_frame = 0;
    const bool debug = debug_on && (debug_frame++ % 30) == 0;
    const auto say = [&](const LensFlareSourceInfo& source, const char* what, float a = 0.0f, float b = 0.0f) {
        if (!debug) return;
        const LensFlareTemplate* t = source.template_index >= 0 ? &scene.lens_flare_templates[static_cast<size_t>(source.template_index)] : nullptr;
        std::printf("[LensFlare] %s (%s): %s %g %g\n", source.name.c_str(), t ? t->path.c_str() : "-", what, a, b);
        std::fflush(stdout);
    };
    for (const LensFlareSourceInfo& source : scene.lens_flares) {
        if (!source.active || source.hidden || source.template_index < 0) {
            say(source, "switched off or hidden");
            continue;
        }
        const LensFlareTemplate& t = scene.lens_flare_templates[static_cast<size_t>(source.template_index)];
        if (t.elements.empty()) {
            say(source, "a template with nothing to draw");
            continue;
        }

        // The bounds' sphere against the view's planes (0x0103c970).
        const float radius = t.box_extent.length();
        {
            const Vec3 d = source.location + t.box_center - view.position;
            const float z = d.dot(view.forward);
            const float x = d.dot(view.right);
            const float y = d.dot(view.up);
            const bool outside = z + radius < view.near_plane || (std::abs(x) - z * tan_x) / side_x > radius ||
                                 (std::abs(y) - z * tan_y) / side_y > radius;
            if (outside) {
                say(source, "out of view; at (x / z, y / z)", x / z, y / z);
                if (debug) {
                    std::printf("    at (%g, %g, %g), radius %g, view-space (%g, %g, %g); view at (%g, %g, %g) forward (%.3f, %.3f, %.3f) "
                                "right (%.3f, %.3f, %.3f) up (%.3f, %.3f, %.3f)\n",
                                source.location.x, source.location.y, source.location.z, radius, x, y, z, view.position.x,
                                view.position.y, view.position.z, view.forward.x, view.forward.y, view.forward.z, view.right.x,
                                view.right.y, view.right.z, view.up.x, view.up.y, view.up.z);
                }
                continue;
            }
        }
        const float intensity = lens_flare_cone_strength(t, source, view);
        if (intensity < 0.0f) {
            say(source, "outside its cone");
            continue;
        }
        const float coverage = lens_flare_coverage(t, source.location, view, scene.collision.get());
        if (coverage <= 0.0f) {  // no pixel of the box passed: nothing of the flare is drawn
            say(source, "hidden; its box uncovered would take",
                lens_flare_coverage(t, source.location, view, nullptr));
            continue;
        }
        const float occlusion = raw_distribution_float(t.screen_percentage_map, coverage);
        say(source, "drawn: coverage, occlusion", coverage, occlusion);

        const Vec3 d = source.location - view.position;
        const float z = d.dot(view.forward);
        if (z <= 1.0e-3f) continue;
        const float sx = d.dot(view.right) * view.proj_x / z;
        const float sy = d.dot(view.up) * view.proj_y / z;
        const float distance = d.length();

        for (const LensFlareElement& e : t.elements) {
            if (!e.enabled) continue;
            const float r = e.ray_distance;
            const float ex = sx * (1.0f - 2.0f * r);
            const float ey = sy * (1.0f - 2.0f * r);
            float radial = std::sqrt(ex * ex + ey * ey);
            if (e.normalize_radial_distance) radial = std::max(std::abs(ex), std::abs(ey));
            const float source_distance = std::sqrt((sx - ex) * (sx - ex) + (sy - ey) * (sy - ey)) * 0.5f;
            const float x = e.use_source_distance ? source_distance : radial;

            const int count = static_cast<int>(e.materials.size());
            const int mi = static_cast<int>(raw_distribution_float(e.material_index, x));
            const int32_t material = e.materials[static_cast<size_t>((mi >= 0 && mi < count) ? mi : 0)];
            if (material < 0) continue;

            const float scale = raw_distribution_float(e.scaling, x);
            float axis[3], dist_scale[3], color[3], dist_color[3];
            raw_distribution_value(e.axis_scaling, x, axis);
            raw_distribution_value(e.dist_scale, distance, dist_scale);
            raw_distribution_value(e.color, x, color);
            raw_distribution_value(e.dist_color, distance, dist_color);
            const float theta = raw_distribution_float(e.rotation, x);
            const float alpha = raw_distribution_float(e.alpha, x) * raw_distribution_float(e.dist_alpha, distance);

            LensFlareQuad q;
            q.material = material;
            q.color[0] = color[0] * dist_color[0];
            q.color[1] = color[1] * dist_color[1];
            q.color[2] = color[2] * dist_color[2];
            q.color[3] = alpha;
            if (e.modulate_color_by_source) {
                for (int c = 0; c < 4; ++c) q.color[c] *= source.source_color[c];
            }
            q.radial_distance = source_distance;  // the two are swapped on their way to the material
            q.source_distance = radial;
            q.ray_distance = r;
            q.occlusion = occlusion;
            q.intensity = intensity;

            const float w_px = f_px * e.size_x * scale * axis[0] * dist_scale[0];
            const float h_px = f_px * e.size_y * scale * axis[1] * dist_scale[1];
            if (!(std::abs(w_px) > 0.0f) || !(std::abs(h_px) > 0.0f)) continue;
            const float cx = (ex * 0.5f + 0.5f) * view.width;
            const float cy = (0.5f - ey * 0.5f) * view.height;
            const float cs = std::cos(theta), sn = std::sin(theta);
            static const float kUV[4][2] = {{0.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}};
            for (int c = 0; c < 4; ++c) {
                const float a = w_px * (kUV[c][0] - 0.5f);
                const float b = h_px * (kUV[c][1] - 0.5f);
                const float px = cx + a * cs - b * sn;  // y down
                const float py = cy + a * sn + b * cs;
                q.corner[c][0] = px / view.width * 2.0f - 1.0f;
                q.corner[c][1] = 1.0f - py / view.height * 2.0f;
            }
            out.push_back(q);
        }
    }
}

// A quad as six vertices in clip space, for the material vertex stage with an identity
// view-projection. The game puts the quad at clip depth 0.1: just past the near plane.
inline void lens_flare_vertices(const LensFlareQuad& q, std::vector<Vertex>& out) {
    static const int kOrder[6] = {0, 1, 2, 2, 1, 3};
    static const float kUV[4][2] = {{0.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}};
    out.resize(6);
    for (int i = 0; i < 6; ++i) {
        const int c = kOrder[i];
        Vertex& v = out[static_cast<size_t>(i)];
        v = Vertex{};
        v.position = Vec3(q.corner[c][0], q.corner[c][1], 0.1f);
        v.u = v.u2 = kUV[c][0];
        v.v = v.v2 = kUV[c][1];
    }
}

}  // namespace me
