#pragma once

// -----------------------------------------------------------------------------
// The shadows of dynamic objects (docs/RENDERING_RE.md, "Dynamic shadows"): the game's
// modulated shadows, as its executable makes them (CreateRepresentativeLight 0x00ff3e50,
// CreateProjectedShadow 0x0106e8c0, the point light's frustum 0x00f2b670, CalcTransforms
// 0x010687e0, ModShadowProjectionPixelShader.usf).
//
// Every dynamic shadow of the game is the shadow of a light environment: beside its
// lighting, an environment that casts makes an invisible point light towards the
// brightest direction of its SHADOW environment (light_environment.hpp), and that light
// projects a shadow of the object onto the lit scene, by multiplication:
//
//   colour   ModShadowColor = min(1, Rest / (Rest + Dominant)) a channel: the share of the
//            shadow environment that is not the dominant light. A warm sun over a blue sky
//            gives a blue shadow.
//   frustum  from the light through the subject's bounding sphere (centre S, radius Rs):
//            half angle asin(Rs / D), the depth range the sphere's own.
//   map      Res = clamp(trunc(2 x the sphere's radius on screen), 32, 1014) texels square.
//   fade     FadeAlpha = ((Res - 32) / 992)^0.2: gone when the subject is 16 pixels in radius.
//   reach    ShadowAtt = sqrt(1 - (distance to the light / its radius)^2): the shadow dies
//            out a few bounds-radii behind the subject.
//   result   scene *= lerp(lerp(1, lerp(1, ModShadowColor, FadeAlpha), ShadowAtt), 1, PCF^2)
//
// drawn after the opaque scene and before height fog and translucency.
//
// Here the depth maps share the third slice of the sun shadow map array, a cell each, and
// one pass over the picture applies them all. The player's shadow is cast by the
// first-person body (the game casts it from the third-person body, which the port has not),
// and since that body is arms and legs, by a head and a torso standing in with it.
// -----------------------------------------------------------------------------

#include "light_environment.hpp"
#include "post_process.hpp"
#include "sun_shadow.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace me {

constexpr int kModShadowSlice = 2;      // of the shadow map array
constexpr int kModShadowCell = 1024;    // a caster's cell in that slice, with its border
constexpr int kModShadowBorder = 5;
constexpr int kModShadowMinRes = 32;
constexpr int kModShadowMaxRes = kModShadowCell - 2 * kModShadowBorder;  // 1014
constexpr int kMaxModShadows = 8;       // in one frame

// Must match ModShadowUniforms in builtin_shaders_msl.hpp: float4s only. It rides in the
// post-process passes' constant buffer, so it is padded to that size.
struct ModShadowUniformsGPU {
    float count[4];                  // x: shadows in use
    float row0[kMaxModShadows][4];   // world -> the shadow's clip space, a row a member
    float row1[kMaxModShadows][4];
    float row2[kMaxModShadows][4];
    float row3[kMaxModShadows][4];
    float light[kMaxModShadows][4];  // xyz: the shadow light's place; w: 1 / its radius
    float color[kMaxModShadows][4];  // rgb: lerp(1, ModShadowColor, FadeAlpha); a: the depth bias, of the subject's depth
    float cell[kMaxModShadows][4];   // xy: the cell's centre in the map, z: its half-size, w: one texel (all in uv)
    float depth[kMaxModShadows][4];  // x: MinSubjectZ, y: MaxSubjectZ, along the light's axis
    float pad[11][4];
};
static_assert(sizeof(ModShadowUniformsGPU) == sizeof(PostUniformsGPU), "ModShadowUniformsGPU rides in the post constant buffer");

// One shadow of this frame.
struct ModShadow {
    enum class Kind : uint8_t { Player, Enemy, Mesh };
    Kind kind = Kind::Mesh;
    size_t index = 0;              // enemy, or scene mesh
    Vec3 center{0.0f, 0.0f, 0.0f}; // the subject's bounding sphere
    float radius = 1.0f;
    ShadowLight light;
    int resolution = 0;
    float fade = 0.0f;
    // Filled by layout_mod_shadows():
    Mat4 subject_matrix;           // world -> the shadow's clip space (depth 0 at the sphere's near side, 1 at its far side)
    float min_z = 0.1f;
    float max_z = 1.0f;
    int cell_x = 0;                // the map's viewport: origin and size are in texels
    int cell_y = 0;
};

// The view a frame is drawn from, as far as a shadow's resolution needs it.
struct ModShadowView {
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 forward{1.0f, 0.0f, 0.0f};
    float proj_x = 1.0f;   // Proj[0][0]
    float proj_y = 1.0f;   // Proj[1][1]
    float width = 1280.0f;
    float height = 720.0f;
};

// CreateProjectedShadow's resolution and fade for a subject sphere. False: no shadow.
inline bool mod_shadow_resolution(const ModShadowView& view, const Vec3& center, float radius, const ShadowLight& light,
                                  int& resolution, float& fade) {
    if (!light.valid) return false;
    const float w = (center - view.position).dot(view.forward);
    // Nothing of it can show when the light's whole reach is behind the view.
    if (w + light.radius + radius < 0.0f) return false;
    const float on_screen = std::max(view.width * 0.5f * view.proj_x, view.height * 0.5f * view.proj_y) * radius / std::max(w, 1.0f);
    resolution = std::clamp(static_cast<int>(2.0f * on_screen), kModShadowMinRes, kModShadowMaxRes);
    fade = std::pow(static_cast<float>(resolution - kModShadowMinRes) / 992.0f, 0.2f);
    return fade > 0.0001f;
}

// Keeps the largest kMaxModShadows, gives each its cell, and builds the frustums (the point
// light's, 0x00f2b670, then CalcTransforms).
inline void layout_mod_shadows(std::vector<ModShadow>& shadows) {
    std::stable_sort(shadows.begin(), shadows.end(),
                     [](const ModShadow& a, const ModShadow& b) { return a.resolution > b.resolution; });
    if (shadows.size() > static_cast<size_t>(kMaxModShadows)) shadows.resize(static_cast<size_t>(kMaxModShadows));
    const int per_side = kSunShadowMapSize / kModShadowCell;
    for (size_t i = 0; i < shadows.size(); ++i) {
        ModShadow& s = shadows[i];
        s.cell_x = (static_cast<int>(i) % per_side) * kModShadowCell + kModShadowBorder;
        s.cell_y = (static_cast<int>(i) / per_side) * kModShadowCell + kModShadowBorder;

        const float rs = std::max(s.radius, 1.0f);
        Vec3 l = s.light.position;
        Vec3 v = s.center - l;
        float d = v.length();
        float sil = 0.0f;
        if (d > rs) sil = rs / std::sqrt((d - rs) * (d + rs));  // tan of the half angle the sphere subtends
        const float kRadiusMultiplier = 1.1f;                   // PointLightComponent.ShadowRadiusMultiplier
        if (rs * kRadiusMultiplier >= d) {
            // The light is inside the sphere, or nearly: it steps back to 1.1 radii.
            const Vec3 dir = d > 1.0e-4f ? v * (1.0f / d) : Vec3(0.0f, 0.0f, -1.0f);
            d = rs * kRadiusMultiplier;
            l = s.center - dir * d;
            v = dir * d;
            sil = 1.0f;
        } else {
            sil = std::min(sil, 1.0f);
        }
        const Vec3 axis = v * (1.0f / d);
        const Vec3 helper = std::abs(axis.z) < 0.9f ? Vec3(0.0f, 0.0f, 1.0f) : Vec3(1.0f, 0.0f, 0.0f);
        const Vec3 x = axis.cross(helper).normalized();
        const Vec3 y = x.cross(axis);
        s.max_z = d + rs;
        s.min_z = std::max(s.max_z - 2.0f * rs, 0.1f);
        const float k = s.max_z / (s.max_z - s.min_z);
        // clip = (x . (p - L) / sil, y . (p - L) / sil, k (z - MinZ), z), z = axis . (p - L).
        Mat4& m = s.subject_matrix;
        const auto set_row = [&m](int row, const Vec3& a, float w) {
            m.m[0 + row] = a.x;
            m.m[4 + row] = a.y;
            m.m[8 + row] = a.z;
            m.m[12 + row] = w;
        };
        set_row(0, x * (1.0f / sil), -x.dot(l) / sil);
        set_row(1, y * (1.0f / sil), -y.dot(l) / sil);
        set_row(2, axis * k, -k * (axis.dot(l) + s.min_z));
        set_row(3, axis, -axis.dot(l));
    }
}

inline void fill_mod_shadow_uniforms(const std::vector<ModShadow>& shadows, ModShadowUniformsGPU& u) {
    u = ModShadowUniformsGPU{};
    const float inv = 1.0f / static_cast<float>(kSunShadowMapSize);
    const size_t n = std::min(shadows.size(), static_cast<size_t>(kMaxModShadows));
    u.count[0] = static_cast<float>(n);
    for (size_t i = 0; i < n; ++i) {
        const ModShadow& s = shadows[i];
        for (int c = 0; c < 4; ++c) {
            u.row0[i][c] = s.subject_matrix.m[c * 4 + 0];
            u.row1[i][c] = s.subject_matrix.m[c * 4 + 1];
            u.row2[i][c] = s.subject_matrix.m[c * 4 + 2];
            u.row3[i][c] = s.subject_matrix.m[c * 4 + 3];
        }
        u.light[i][0] = s.light.position.x;
        u.light[i][1] = s.light.position.y;
        u.light[i][2] = s.light.position.z;
        u.light[i][3] = 1.0f / std::max(s.light.radius, 1.0f);
        // ShadowModulateColor = white + (ModShadowColor - white) * FadeAlpha.
        u.color[i][0] = 1.0f + (s.light.mod_color.x - 1.0f) * s.fade;
        u.color[i][1] = 1.0f + (s.light.mod_color.y - 1.0f) * s.fade;
        u.color[i][2] = 1.0f + (s.light.mod_color.z - 1.0f) * s.fade;
        // The depth pass's bias: 0.02 * 512 / Res of the subject's depth range.
        u.color[i][3] = 10.24f / static_cast<float>(s.resolution);
        const float half = 0.5f * static_cast<float>(s.resolution);
        u.cell[i][0] = (static_cast<float>(s.cell_x) + half) * inv;
        u.cell[i][1] = (static_cast<float>(s.cell_y) + half) * inv;
        u.cell[i][2] = half * inv;
        u.cell[i][3] = inv;  // ShadowFilterRadius 2 * 0.5: the taps are a texel apart
        u.depth[i][0] = s.min_z;
        u.depth[i][1] = s.max_z;
    }
}

// What the first-person body lacks, for her shadow: a head behind the eye and a torso from the
// neck down to the hips, as two ellipsoids in the world. `eye` and `feet` are the view's and the
// pawn's; `forward`, `right` and `up` the view's axes. Both keep level whatever the view's pitch.
inline void player_body_stand_in(const Vec3& eye, const Vec3& feet, const Vec3& forward, const Vec3& right, const Vec3& up,
                                 std::vector<Vertex>& out) {
    // The way she faces, on the ground: a quarter turn from the view's right.
    Vec3 ahead(-right.y, right.x, 0.0f);
    ahead = ahead.length_sq() > 1.0e-6f ? ahead.normalized() : Vec3(1.0f, 0.0f, 0.0f);
    const Vec3 level = std::abs(forward.z) < 0.95f ? forward : up * (forward.z < 0.0f ? 1.0f : -1.0f);
    if (ahead.dot(level) < 0.0f) ahead = ahead * -1.0f;
    const Vec3 side(-ahead.y, ahead.x, 0.0f);
    out.clear();
    const auto ellipsoid = [&out](const Vec3& centre, const Vec3& a, float ra, const Vec3& b, float rb, const Vec3& c, float rc) {
        const int rings = 6, segments = 10;
        const auto point = [&](int ring, int segment) {
            const float lat = (static_cast<float>(ring) / static_cast<float>(rings) - 0.5f) * 3.14159265f;
            const float lon = static_cast<float>(segment) / static_cast<float>(segments) * 6.28318531f;
            Vertex v;
            v.position = centre + a * (std::cos(lat) * std::cos(lon) * ra) + b * (std::cos(lat) * std::sin(lon) * rb) + c * (std::sin(lat) * rc);
            return v;
        };
        for (int r = 0; r < rings; ++r) {
            for (int s = 0; s < segments; ++s) {
                const Vertex p0 = point(r, s), p1 = point(r, s + 1), p2 = point(r + 1, s), p3 = point(r + 1, s + 1);
                out.push_back(p0);
                out.push_back(p1);
                out.push_back(p2);
                out.push_back(p2);
                out.push_back(p1);
                out.push_back(p3);
            }
        }
    };
    const Vec3 z(0.0f, 0.0f, 1.0f);
    ellipsoid(eye - ahead * 8.5f + z * 0.5f, ahead, 9.5f, side, 8.0f, z, 11.5f);
    // The torso: from under the head to the hips, which sit a little over half way up from her feet.
    const Vec3 neck = eye - ahead * 9.0f - z * 12.0f;
    const Vec3 hips = feet + z * std::max(0.57f * (eye.z - feet.z), 10.0f) - ahead * 3.0f;
    const Vec3 spine = neck - hips;
    const float length = spine.length();
    if (length > 8.0f) {
        const Vec3 along = spine * (1.0f / length);
        Vec3 depth = side.cross(along);
        depth = depth.length_sq() > 1.0e-6f ? depth.normalized() : ahead;
        ellipsoid((neck + hips) * 0.5f, depth, 10.0f, side, 16.0f, along, length * 0.5f + 4.0f);
    }
}

// This frame's shadows: the player's, the enemies' whose mesh is posed (`enemy_ready`, by enemy),
// and those of the scene's dynamic objects that have a light environment that casts.
inline void collect_mod_shadows(const LevelScene& scene, SceneLightEnvironments& envs, const ModShadowView& view,
                                const Vec3* player_position, float player_yaw_deg, const std::vector<uint8_t>& enemy_ready,
                                float now, std::vector<ModShadow>& out) {
    out.clear();
    static const bool off = std::getenv("ME_NO_DYNAMIC_SHADOWS") != nullptr;  // to see a picture without them
    if (off) return;
    const auto add = [&](ModShadow::Kind kind, size_t index, const Vec3& center, float radius, const ShadowLight& light) {
        ModShadow s;
        if (!mod_shadow_resolution(view, center, radius, light, s.resolution, s.fade)) return;
        s.kind = kind;
        s.index = index;
        s.center = center;
        s.radius = radius;
        s.light = light;
        out.push_back(s);
    };
    if (player_position) {
        add(ModShadow::Kind::Player, 0, *player_position + Vec3(0.0f, 0.0f, 90.0f), 110.0f,
            envs.player_shadow(scene, *player_position, player_yaw_deg, now));
    }
    for (size_t ei = 0; ei < scene.enemies.size() && ei < enemy_ready.size(); ++ei) {
        if (!enemy_ready[ei]) continue;
        add(ModShadow::Kind::Enemy, ei, scene.enemies[ei].position + Vec3(0.0f, 0.0f, 90.0f), 110.0f,
            envs.enemy_shadow(scene, ei, now));
    }
    for (size_t i = 0; i < scene.meshes.size(); ++i) {
        const MeshBuffer& mb = scene.meshes[i];
        if (!mb.dynamic_lit || mb.vertices.empty() || mb.lighting.mode != DynamicLighting::Mode::Environment ||
            !mb.lighting.cast_shadows) {
            continue;
        }
        // What the level's script has hidden casts nothing.
        if (mb.actor >= 0 && static_cast<size_t>(mb.actor) < scene.actors.size() && scene.actors[static_cast<size_t>(mb.actor)].is_hidden) continue;
        Vec3 center;
        float radius = 1.0f;
        const ShadowLight light = envs.mesh_shadow(scene, i, now, center, radius);
        add(ModShadow::Kind::Mesh, i, center, radius, light);
    }
    layout_mod_shadows(out);
}

}  // namespace me
