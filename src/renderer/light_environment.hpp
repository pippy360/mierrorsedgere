#pragma once

// -----------------------------------------------------------------------------
// Light for what the light maps do not light: a DynamicLightEnvironmentComponent,
// as the game's executable runs it (docs/RENDERING_RE.md; FDynamicLightEnvironmentState,
// constructor 0x00ff3120, UpdateStaticEnvironment 0x00ff4b60, CreateRepresentativeLight
// 0x00ff3e50).
//
// An environment gathers the level's lights at the centre of its owner's bounds into
// nine spherical-harmonic coefficients a colour: each light that shares a channel with
// the owner and is not hidden from that point (one line check a light) adds
// SHBasis(direction to it) * its colour there; a sky light adds its upper and lower
// colours. DICE's bounce comes on top: the light pushed away from grey, mirrored, times
// BouncedLightingIntensity. Then the brightest direction is taken out as a POINT light
// standing LightDistance bounds-radii from the centre, and what is left lights the
// object as spherical harmonics (diffuse only).
//
// The same code serves both renderers and the bake of dynamic-class actors that the
// port draws with the level's geometry.
// -----------------------------------------------------------------------------

#include "../math/types.hpp"
#include "../physics/collision_world.hpp"
#include "post_process.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

namespace me {

// Nine coefficients a colour: c[i] is coefficient i = l * (l + 1) + m, as (r, g, b).
struct SH9RGB {
    Vec3 c[9];
};

// SHBasisFunction (0x0117f6d0): the real basis with the Condon-Shortley sign.
inline void sh_basis(const Vec3& d, float b[9]) {
    b[0] = 0.282095f;
    b[1] = -0.488603f * d.y;
    b[2] = 0.488603f * d.z;
    b[3] = -0.488603f * d.x;
    b[4] = 1.092548f * d.x * d.y;
    b[5] = -1.092548f * d.y * d.z;
    b[6] = 0.315392f * (3.0f * d.z * d.z - 1.0f);
    b[7] = -1.092548f * d.x * d.z;
    b[8] = 0.546274f * (d.x * d.x - d.y * d.y);
}

inline void sh_add(SH9RGB& e, const float b[9], const Vec3& color) {
    for (int i = 0; i < 9; ++i) e.c[i] = e.c[i] + color * b[i];
}

// A DynamicLightEnvironmentComponent's settings: DynamicLighting (math/types.hpp), with the class's
// defaults; pawns and the first-person arms have their own.
using LightEnvSettings = DynamicLighting;

// TdPawn.MyLightEnvironment (the third-person body, enemies, the first-person legs).
inline LightEnvSettings pawn_light_environment() {
    LightEnvSettings s;
    s.light_distance = 8.0f;
    s.bounce = 0.2f;
    return s;
}
// TdPlayerPawn.MyLightEnvironment1P (the arms and the held weapon).
inline LightEnvSettings arms_light_environment() {
    LightEnvSettings s;
    s.light_distance = 8.0f;
    s.shadow_distance = 2.5f;
    return s;
}

// What a pixel shader is given: SceneUniforms' env_ members.
struct LightEnvLighting {
    bool has_point = false;
    Vec3 point_pos{0.0f, 0.0f, 0.0f};
    float point_radius = 1.0f;
    Vec3 point_color{0.0f, 0.0f, 0.0f};
    SH9RGB sh;                 // what is left after the point light took its share
    bool sky_instead = false;  // bSynthesizeSHLight off: an upper and a lower colour instead
    Vec3 sky_upper{0.0f, 0.0f, 0.0f};
    Vec3 sky_lower{0.0f, 0.0f, 0.0f};
};

// ComputeAndFixedColorAndIntensity (0x00ff3080): a synthesized light's colour goes through eight
// bits a channel, against its largest.
inline Vec3 light_env_quantize(const Vec3& c) {
    const float m = std::max({1.0e-5f, c.x, c.y, c.z});
    const auto q = [m](float v) {
        const float byte = std::floor(std::clamp(std::pow(std::max(v / m, 0.0f), 1.0f / 2.2f) * 255.0f, 0.0f, 255.0f));
        return std::pow(byte / 255.0f, 2.2f) * m;
    };
    return Vec3(q(c.x), q(c.y), q(c.z));
}

// UpdateStaticEnvironment: the level's lights at `origin`, for an owner of bounds radius `radius`.
// `collision` (may be null) answers the one line check a light gets.
inline SH9RGB gather_light_environment(const Vec3& origin, float radius, const LightEnvSettings& s,
                                       const std::vector<LevelLight>& lights, const CollisionWorld* collision) {
    SH9RGB e{};
    float b[9];
    for (const LevelLight& l : lights) {
        if (l.dynamic_list) continue;
        // DoesLightAffectOwner (0x00ff1490): the light's own Dynamic bit does not count, its
        // CompositeDynamic bit counts as Dynamic.
        uint32_t ch = l.channels & ~(1u << 3);
        if (ch & (1u << 4)) ch = (ch & ~(1u << 4)) | (1u << 3);
        if (!(ch & s.channels & ~1u)) continue;

        if (l.kind == LevelLight::Kind::Sky) {
            // UpperSkyFunction / LowerSkyFunction: V[0] = 0.564190, V[2] = +-0.488603.
            e.c[0] = e.c[0] + (l.color + l.lower_color) * 0.564190f;
            e.c[2] = e.c[2] + (l.color - l.lower_color) * 0.488603f;
            continue;
        }
        const bool directional = l.kind == LevelLight::Kind::Directional;
        Vec3 to_light;
        Vec3 from_point;
        Vec3 intensity = l.color;
        if (directional) {
            to_light = l.direction * -1.0f;
            from_point = origin + to_light * 100000.0f;  // TraceDistance
        } else {
            const Vec3 d = l.position - origin;
            const float dist_sq = d.length_sq();
            const float reach = l.radius + radius;
            if (dist_sq > reach * reach) continue;  // AffectsBounds
            to_light = dist_sq > 1.0e-8f ? d * (1.0f / std::sqrt(dist_sq)) : Vec3(0.0f, 0.0f, 1.0f);
            from_point = l.position;
            intensity = intensity * std::pow(std::max(1.0f - dist_sq / (l.radius * l.radius), 0.0f), l.falloff);
            if (l.kind == LevelLight::Kind::Spot) {
                const float along = (to_light * -1.0f).dot(l.direction);
                const float cone = std::clamp((along - l.cos_outer) / std::max(l.cos_inner - l.cos_outer, 1.0e-6f), 0.0f, 1.0f);
                intensity = intensity * (cone * cone);
            }
        }
        if (intensity.x <= 0.0f && intensity.y <= 0.0f && intensity.z <= 0.0f) continue;
        // IsLightVisible (0x00ff11e0): one line, from the light to the centre of the bounds.
        if (collision && l.cast_shadows && l.cast_static_shadows && collision->line_check(from_point, origin, COLL_ShadowCast).hit) {
            continue;
        }
        sh_basis(to_light, b);
        sh_add(e, b, intensity);
    }

    // DICE's bounced light: X = E * (1 - D) + luminance(E) * D, its directional part mirrored.
    const float d = s.bounce_desaturation;
    for (int i = 0; i < 9; ++i) {
        const float lum = 0.3f * e.c[i].x + 0.59f * e.c[i].y + 0.11f * e.c[i].z;
        Vec3 x = e.c[i] * (1.0f - d) + Vec3(lum, lum, lum) * d;
        if (i > 0) x = x * -1.0f;
        e.c[i] = e.c[i] + x * s.bounce;
    }
    e.c[0] = e.c[0] + s.ambient_glow * (4.0f * 0.282095f);
    return e;
}

// A light's colour at a point, and the unit vector to it; false when it does not reach.
inline bool light_at(const LevelLight& l, const Vec3& origin, float radius, Vec3& to_light, Vec3& intensity) {
    intensity = l.color;
    if (l.kind == LevelLight::Kind::Directional) {
        to_light = l.direction * -1.0f;
        return true;
    }
    const Vec3 d = l.position - origin;
    const float dist_sq = d.length_sq();
    const float reach = l.radius + radius;
    if (dist_sq > reach * reach) return false;
    to_light = dist_sq > 1.0e-8f ? d * (1.0f / std::sqrt(dist_sq)) : Vec3(0.0f, 0.0f, 1.0f);
    intensity = intensity * std::pow(std::max(1.0f - dist_sq / (l.radius * l.radius), 0.0f), l.falloff);
    if (l.kind == LevelLight::Kind::Spot) {
        const float along = (to_light * -1.0f).dot(l.direction);
        const float cone = std::clamp((along - l.cos_outer) / std::max(l.cos_inner - l.cos_outer, 1.0e-6f), 0.0f, 1.0f);
        intensity = intensity * (cone * cone);
    }
    return intensity.x > 0.0f || intensity.y > 0.0f || intensity.z > 0.0f;
}

// The lights of the world's dynamic list that reach an object: those that share one of its mesh's
// own channels (FLightSceneInfo's pairing, 0x0103dd70). They are no part of a light environment and
// are not traced; here they are put with the environment's, as if gathered. `origin` is where the
// object was placed: such a light rides with what it lights (a lift's lamps), and the port leaves
// the light where the level put it.
inline SH9RGB gather_dynamic_lights(const Vec3& origin, float radius, uint32_t channels, const std::vector<LevelLight>& lights) {
    SH9RGB e{};
    float b[9];
    for (const LevelLight& l : lights) {
        if (!l.dynamic_list || !(l.channels & channels & ~1u)) continue;
        if (l.kind == LevelLight::Kind::Sky) {
            e.c[0] = e.c[0] + (l.color + l.lower_color) * 0.564190f;
            e.c[2] = e.c[2] + (l.color - l.lower_color) * 0.488603f;
            continue;
        }
        Vec3 to_light, intensity;
        if (!light_at(l, origin, radius, to_light, intensity)) continue;
        sh_basis(to_light, b);
        sh_add(e, b, intensity);
    }
    return e;
}

// The SHADOW environment of UpdateStaticEnvironment: the lights that cast a composite shadow
// (bCastCompositeShadow: the sun and the sky by class, lamps where the level says), with no bounce.
// The component's ambient shadow term is added when the light is made (it can turn with the owner).
inline SH9RGB gather_shadow_environment(const Vec3& origin, float radius, const LightEnvSettings& s,
                                        const std::vector<LevelLight>& lights, const CollisionWorld* collision) {
    SH9RGB e{};
    float b[9];
    for (const LevelLight& l : lights) {
        if (!l.cast_composite || l.dynamic_list) continue;
        uint32_t ch = l.channels & ~(1u << 3);
        if (ch & (1u << 4)) ch = (ch & ~(1u << 4)) | (1u << 3);
        if (!(ch & s.channels & ~1u)) continue;
        if (l.kind == LevelLight::Kind::Sky) {
            e.c[0] = e.c[0] + (l.color + l.lower_color) * 0.564190f;
            e.c[2] = e.c[2] + (l.color - l.lower_color) * 0.488603f;
            continue;
        }
        Vec3 to_light;
        Vec3 from_point;
        Vec3 intensity = l.color;
        if (l.kind == LevelLight::Kind::Directional) {
            to_light = l.direction * -1.0f;
            from_point = origin + to_light * 100000.0f;
        } else {
            const Vec3 d = l.position - origin;
            const float dist_sq = d.length_sq();
            const float reach = l.radius + radius;
            if (dist_sq > reach * reach) continue;
            to_light = dist_sq > 1.0e-8f ? d * (1.0f / std::sqrt(dist_sq)) : Vec3(0.0f, 0.0f, 1.0f);
            from_point = l.position;
            intensity = intensity * std::pow(std::max(1.0f - dist_sq / (l.radius * l.radius), 0.0f), l.falloff);
            if (l.kind == LevelLight::Kind::Spot) {
                const float along = (to_light * -1.0f).dot(l.direction);
                const float cone = std::clamp((along - l.cos_outer) / std::max(l.cos_inner - l.cos_outer, 1.0e-6f), 0.0f, 1.0f);
                intensity = intensity * (cone * cone);
            }
        }
        if (intensity.x <= 0.0f && intensity.y <= 0.0f && intensity.z <= 0.0f) continue;
        if (collision && l.cast_shadows && l.cast_static_shadows && collision->line_check(from_point, origin, COLL_ShadowCast).hit) {
            continue;
        }
        sh_basis(to_light, b);
        sh_add(e, b, intensity);
    }
    return e;
}

// The invisible point light an environment's shadow is cast from, and the colour that shadow
// multiplies the scene by (CreateRepresentativeLight(S, 0, 1)).
struct ShadowLight {
    bool valid = false;
    Vec3 position{0.0f, 0.0f, 0.0f};
    float radius = 1.0f;
    Vec3 mod_color{1.0f, 1.0f, 1.0f};  // ModShadowColor
};

inline ShadowLight shadow_light(SH9RGB shadow, const Vec3& origin, float radius, const LightEnvSettings& s) {
    ShadowLight out;
    {
        // The ambient shadow source: a faint light, from above unless the owner says otherwise.
        const Vec3 dir = s.ambient_shadow_dir.length_sq() > 1.0e-8f ? s.ambient_shadow_dir.normalized() : Vec3(0.0f, 0.0f, 1.0f);
        float b[9];
        sh_basis(dir, b);
        sh_add(shadow, b, s.ambient_shadow_color);
    }
    const auto lum = [&shadow](int i) { return 0.3f * shadow.c[i].x + 0.59f * shadow.c[i].y + 0.11f * shadow.c[i].z; };
    const Vec3 d(-lum(3), -lum(1), lum(2));
    if (d.length_sq() < 1.0e-5f) return out;  // no direction: no shadow light, no shadow
    const Vec3 dir = d.normalized();
    float b[9];
    sh_basis(dir, b);
    Vec3 dominant(0.0f, 0.0f, 0.0f);
    for (int i = 0; i < 9; ++i) dominant = dominant + shadow.c[i] * b[i];
    dominant = dominant * (4.0f * 3.14159265f / 9.0f);  // / dot(B, B)
    dominant = Vec3(std::max(dominant.x, 0.0f), std::max(dominant.y, 0.0f), std::max(dominant.z, 0.0f));
    const float grey = 0.3f * dominant.x + 0.59f * dominant.y + 0.11f * dominant.z;
    dominant = dominant + (Vec3(grey, grey, grey) - dominant) * s.light_desaturation;
    if (dominant.x <= 0.0f && dominant.y <= 0.0f && dominant.z <= 0.0f) return out;
    // What is left once the dominant light is taken out, as an ambient level.
    const Vec3 rest_sh = shadow.c[0] - dominant * b[0];
    const Vec3 rest(std::max(rest_sh.x / 0.282095f, 0.0f), std::max(rest_sh.y / 0.282095f, 0.0f), std::max(rest_sh.z / 0.282095f, 0.0f));
    const auto share = [](float r, float i) { return std::min(r / std::max(r + i, 1.0e-5f), 1.0f); };
    out.valid = true;
    out.mod_color = Vec3(share(rest.x, dominant.x), share(rest.y, dominant.y), share(rest.z, dominant.z));
    out.position = origin + dir * (radius * s.light_distance);
    out.radius = radius * (s.light_distance + s.shadow_distance + 2.0f);
    return out;
}

// CreateEnvironmentLightList: the environment as the lights the owner is drawn with.
inline LightEnvLighting light_environment_lighting(SH9RGB r, const Vec3& origin, float radius, const LightEnvSettings& s) {
    LightEnvLighting out;
    if (s.synthesize_point) {
        const auto lum = [&r](int i) { return 0.3f * r.c[i].x + 0.59f * r.c[i].y + 0.11f * r.c[i].z; };
        const Vec3 d(-lum(3), -lum(1), lum(2));
        if (d.length_sq() >= 1.0e-5f) {
            const Vec3 dir = d.normalized();
            float b[9];
            sh_basis(dir, b);
            Vec3 intensity(0.0f, 0.0f, 0.0f);
            for (int i = 0; i < 9; ++i) intensity = intensity + r.c[i] * b[i];
            intensity = intensity * (4.0f * 3.14159265f / 9.0f);  // / dot(B, B)
            intensity = Vec3(std::max(intensity.x, 0.0f), std::max(intensity.y, 0.0f), std::max(intensity.z, 0.0f));
            const float grey = 0.3f * intensity.x + 0.59f * intensity.y + 0.11f * intensity.z;
            intensity = intensity + (Vec3(grey, grey, grey) - intensity) * s.light_desaturation;
            if (intensity.x > 0.0f || intensity.y > 0.0f || intensity.z > 0.0f) {
                for (int i = 0; i < 9; ++i) r.c[i] = r.c[i] - intensity * b[i];
                const float reach = s.light_distance + s.shadow_distance + 2.0f;
                const float at_centre = 1.0f - (s.light_distance / reach) * (s.light_distance / reach);
                out.has_point = true;
                out.point_pos = origin + dir * (radius * s.light_distance);
                out.point_radius = radius * reach;
                out.point_color = light_env_quantize(intensity * (1.0f / std::max(at_centre * at_centre, 1.0e-6f)));
            }
        }
    }
    if (s.synthesize_sh) {
        out.sh = r;
    } else {
        // What is left becomes a sky light's two colours; the rest is dropped.
        const float upper_scale = 1.0f / 0.557042f;
        Vec3 upper = (r.c[0] * 0.564190f + r.c[2] * 0.488603f) * upper_scale;
        upper = Vec3(std::max(upper.x, 0.0f), std::max(upper.y, 0.0f), std::max(upper.z, 0.0f));
        r.c[0] = r.c[0] - upper * 0.564190f;
        r.c[2] = r.c[2] - upper * 0.488603f;
        Vec3 lower = (r.c[0] * 0.564190f - r.c[2] * 0.488603f) * upper_scale;
        lower = Vec3(std::max(lower.x, 0.0f), std::max(lower.y, 0.0f), std::max(lower.z, 0.0f));
        out.sky_instead = true;
        out.sky_upper = light_env_quantize(upper);
        out.sky_lower = light_env_quantize(lower);
    }
    return out;
}

// The light a point of a surface takes from an environment's lights: the arithmetic of
// PointLightPixelShader.usf and SphericalHarmonicLightPixelShader.usf for a diffuse surface
// (DiffusePower 1, no specular), on the CPU. The bake of dynamic-class actors uses it.
inline Vec3 light_environment_irradiance(const LightEnvLighting& l, const Vec3& pos, const Vec3& n) {
    Vec3 e(0.0f, 0.0f, 0.0f);
    if (l.has_point) {
        const Vec3 lv = l.point_pos - pos;
        const float dist_sq = lv.length_sq();
        const float a = std::clamp(1.0f - dist_sq / (l.point_radius * l.point_radius), 0.0f, 1.0f);
        const float ndl = dist_sq > 1.0e-8f ? std::max(n.dot(lv) / std::sqrt(dist_sq), 0.0f) : 0.0f;
        e = e + l.point_color * (ndl * a * a);
    }
    if (l.sky_instead) {
        const float up = 0.5f + 0.5f * n.z, down = 0.5f - 0.5f * n.z;
        e = e + l.sky_upper * (up * up) + l.sky_lower * (down * down);
    } else {
        float b[9];
        sh_basis(n, b);
        const float k0 = 3.14159265f, k1 = 2.0f * 3.14159265f / 3.0f, k2 = 3.14159265f / 4.0f;  // DiffusePower 1
        Vec3 sum = l.sh.c[0] * (b[0] * k0);
        for (int i = 1; i < 4; ++i) sum = sum + l.sh.c[i] * (b[i] * k1);
        for (int i = 4; i < 9; ++i) sum = sum + l.sh.c[i] * (b[i] * k2);
        e = e + Vec3(std::max(sum.x, 0.0f), std::max(sum.y, 0.0f), std::max(sum.z, 0.0f));
    }
    return e;
}

// An object whose environment the level left switched off: lit straight by the lights that share a
// channel with it (the light's Dynamic bit counts here), with no line check: the sky lights through
// the base pass's hemisphere term, every other light as a pass of its own
// (ULightComponent::AffectsPrimitive, 0x00ed9f70). The brightest of those others stands for them here.
inline LightEnvLighting direct_lighting(const Vec3& now_at, const Vec3& placed_at, float radius, const DynamicLighting& s,
                                        const std::vector<LevelLight>& lights) {
    LightEnvLighting out;
    out.sky_instead = true;
    float best = 0.0f;
    for (const LevelLight& l : lights) {
        if (!(l.channels & s.channels & ~1u)) continue;
        if (l.kind == LevelLight::Kind::Sky) {
            out.sky_upper = out.sky_upper + l.color;
            out.sky_lower = out.sky_lower + l.lower_color;
            continue;
        }
        // A light of the dynamic list rides with what it lights; the port leaves it where it was placed.
        const Vec3 origin = l.dynamic_list ? placed_at : now_at;
        const Vec3 shift = now_at - origin;
        Vec3 pos;
        float reach = 1.0e9f;
        Vec3 here = l.color;
        if (l.kind == LevelLight::Kind::Directional) {
            pos = now_at - l.direction * 1.0e6f;
        } else {
            const float dist_sq = (l.position - origin).length_sq();
            const float limit = l.radius + radius;
            if (dist_sq > limit * limit) continue;
            pos = l.position + shift;
            reach = l.radius;
            here = here * std::pow(std::max(1.0f - dist_sq / (l.radius * l.radius), 0.0f), l.falloff);
            if (l.kind == LevelLight::Kind::Spot && dist_sq > 1.0e-8f) {
                const float along = ((origin - l.position) * (1.0f / std::sqrt(dist_sq))).dot(l.direction);
                const float cone = std::clamp((along - l.cos_outer) / std::max(l.cos_inner - l.cos_outer, 1.0e-6f), 0.0f, 1.0f);
                here = here * (cone * cone);
            }
        }
        const float lum = 0.3f * here.x + 0.59f * here.y + 0.11f * here.z;
        if (lum <= best) continue;
        best = lum;
        out.has_point = true;
        out.point_pos = pos;
        out.point_radius = reach;
        out.point_color = l.color;
    }
    return out;
}

// The lighting of a dynamic object for this frame: its environment's, the lights' own, or none.
// `env` keeps the environment between frames.
class LightEnvironment;
inline LightEnvLighting unlit_lighting() {
    LightEnvLighting out;
    return out;  // an environment "given" with nothing in it: the object shows what it emits
}

// An owner's environment in time (Tick, 0x00ff5b40): gathered again at most every 0.3 s and only
// when the owner has moved, the lights in use moving to the new ones in a straight line meanwhile.
class LightEnvironment {
public:
    // `now` in seconds. A clock that ran back, or the first call, sets the environment at once.
    // `direct`: light to add to the environment's own before it is split (gather_dynamic_lights).
    void update(const Vec3& origin, float radius, const LightEnvSettings& s, const std::vector<LevelLight>& lights,
                const CollisionWorld* collision, float now, const SH9RGB* direct = nullptr) {
        const float dt = now - last_time_;
        if (!valid_ || !(dt >= 0.0f) || dt > 2.0f) {
            current_ = target_ = gather_light_environment(origin, radius, s, lights, collision);
            if (s.cast_shadows) shadow_current_ = shadow_target_ = gather_shadow_environment(origin, radius, s, lights, collision);
            gathered_at_ = origin;
            gathered_time_ = now;
            valid_ = true;
        } else {
            if (now - gathered_time_ > kInterval) {
                if ((origin - gathered_at_).length_sq() > 0.01f) {
                    target_ = gather_light_environment(origin, radius, s, lights, collision);
                    if (s.cast_shadows) shadow_target_ = gather_shadow_environment(origin, radius, s, lights, collision);
                    gathered_at_ = origin;
                }
                gathered_time_ = now;
            }
            const float left = std::max(1.0e-5f, gathered_time_ + kInterval - now);
            const float alpha = std::clamp(dt / left, 0.0f, 1.0f);
            for (int i = 0; i < 9; ++i) {
                current_.c[i] = current_.c[i] + (target_.c[i] - current_.c[i]) * alpha;
                shadow_current_.c[i] = shadow_current_.c[i] + (shadow_target_.c[i] - shadow_current_.c[i]) * alpha;
            }
        }
        last_time_ = now;
        SH9RGB lit = current_;
        if (direct) {
            for (int i = 0; i < 9; ++i) lit.c[i] = lit.c[i] + direct->c[i];
        }
        lighting_ = light_environment_lighting(lit, origin, radius, s);
        shadow_ = s.cast_shadows ? shadow_light(shadow_current_, origin, radius, s) : ShadowLight{};
    }
    [[nodiscard]] const LightEnvLighting& lighting() const { return lighting_; }
    [[nodiscard]] const ShadowLight& shadow() const { return shadow_; }

private:
    static constexpr float kInterval = 0.3f;  // MinTimeBetweenFullUpdates
    SH9RGB current_{};
    SH9RGB target_{};
    SH9RGB shadow_current_{};
    SH9RGB shadow_target_{};
    LightEnvLighting lighting_{};
    ShadowLight shadow_{};
    Vec3 gathered_at_{0.0f, 0.0f, 0.0f};
    float gathered_time_ = 0.0f;
    float last_time_ = 0.0f;
    bool valid_ = false;
};

// ME_LIGHT_ENV_DEBUG=1: what an environment at `origin` gathers, light by light, twice a second.
inline void debug_light_environment(const char* who, const Vec3& origin, float radius, const LightEnvSettings& s,
                                    const std::vector<LevelLight>& lights, const CollisionWorld* collision,
                                    const std::vector<LevelActor>& actors, const LightEnvLighting& result, float now) {
    static const bool on = std::getenv("ME_LIGHT_ENV_DEBUG") != nullptr;
    static float last = -1.0e9f;
    if (!on || std::abs(now - last) < 0.5f) return;
    last = now;
    std::printf("[LightEnv] %s t=%.2f at (%.0f, %.0f, %.0f) r=%.0f channels=%#x: %zu lights in the level\n", who, now, origin.x,
                origin.y, origin.z, radius, s.channels, lights.size());
    static const char* kKinds[4] = {"directional", "point", "spot", "sky"};
    for (const LevelLight& l : lights) {
        uint32_t ch = l.channels & ~(1u << 3);
        if (ch & (1u << 4)) ch = (ch & ~(1u << 4)) | (1u << 3);
        const bool shares = l.dynamic_list ? (l.channels & s.channels & ~1u) != 0 : (ch & s.channels & ~1u) != 0;
        const bool far_light = l.kind != LevelLight::Kind::Sky && l.kind != LevelLight::Kind::Directional;
        if (far_light) {
            const float reach = l.radius + radius;
            if ((l.position - origin).length_sq() > reach * reach) continue;
        }
        std::string hidden;
        if (collision && l.kind != LevelLight::Kind::Sky && l.cast_shadows && l.cast_static_shadows) {
            const Vec3 from = far_light ? l.position : origin - l.direction * 100000.0f;
            const CollisionHit hit = collision->line_check(from, origin, COLL_ShadowCast);
            if (hit.hit) {
                hidden = ", hidden by ";
                if (hit.actor >= 0 && static_cast<size_t>(hit.actor) < actors.size()) {
                    const LevelActor& a = actors[static_cast<size_t>(hit.actor)];
                    hidden += a.class_name + " " + a.unique_name + " (" + a.mesh_name + ")";
                } else {
                    hidden += "the level's own geometry";
                }
                hidden += " " + std::to_string(static_cast<int>((hit.location - origin).length())) + " uu away";
            }
        }
        std::printf("[LightEnv]   %-11s colour (%.2f, %.2f, %.2f) channels=%#x %s%s\n", kKinds[static_cast<int>(l.kind)], l.color.x,
                    l.color.y, l.color.z, l.channels, shares ? "shares a channel" : "no shared channel", hidden.c_str());
    }
    std::printf("[LightEnv]   -> point %s colour (%.2f, %.2f, %.2f) radius %.0f; sh[0] (%.3f, %.3f, %.3f)\n",
                result.has_point ? "yes" : "no", result.point_color.x, result.point_color.y, result.point_color.z, result.point_radius,
                result.sh.c[0].x, result.sh.c[0].y, result.sh.c[0].z);
    std::fflush(stdout);
}

// The light environments of a scene's dynamic objects, kept between frames: what a renderer asks
// for the lighting of each one it draws.
class SceneLightEnvironments {
public:
    // Once a frame, before anything is asked: a new level starts from nothing.
    void begin_frame(const LevelScene& scene) {
        if (map_ == scene.map_name) return;
        map_ = scene.map_name;
        player_ = LightEnvironment{};
        player_body_ = LightEnvironment{};
        enemies_.clear();
        meshes_.clear();
    }

    // Scene mesh `i`, a dynamic object (MeshBuffer::dynamic_lit): a lift's part, a door, an
    // InterpActor or KActor left in place. Its bounds are followed where the port moves it.
    // Where scene mesh `i` is now, and (`placed`) where the level put it.
    static void mesh_bounds(const LevelScene& scene, size_t i, Vec3& origin, float& radius, Vec3* placed = nullptr) {
        const MeshBuffer& mb = scene.meshes[i];
        origin = (mb.bounds.min_pt + mb.bounds.max_pt) * 0.5f;
        radius = std::max(50.0f, (mb.bounds.max_pt - mb.bounds.min_pt).length() * 0.5f);
        if (placed) *placed = origin;
        if (mb.elevator >= 0 && static_cast<size_t>(mb.elevator) < scene.elevators.size()) {
            const auto& parts = scene.elevators[static_cast<size_t>(mb.elevator)].parts;
            if (mb.elevator_part >= 0 && static_cast<size_t>(mb.elevator_part) < parts.size()) {
                origin = origin + parts[static_cast<size_t>(mb.elevator_part)].offset;
            }
        } else if (mb.barge_door >= 0 && static_cast<size_t>(mb.barge_door) < scene.barge_doors.size()) {
            origin = scene.barge_doors[static_cast<size_t>(mb.barge_door)].model_matrix.transform_point(origin);
        }
    }

    LightEnvLighting mesh(const LevelScene& scene, size_t i, float now) {
        const MeshBuffer& mb = scene.meshes[i];
        Vec3 origin, placed;
        float radius = 50.0f;
        mesh_bounds(scene, i, origin, radius, &placed);
        if (mb.lighting.mode == DynamicLighting::Mode::Environment) {
            LightEnvironment& env = meshes_[i];
            const SH9RGB direct = gather_dynamic_lights(placed, radius, mb.lighting.channels, scene.lights);
            env.update(origin, radius, mb.lighting, scene.lights, scene.collision.get(), now, &direct);
            return env.lighting();
        }
        if (mb.lighting.mode == DynamicLighting::Mode::Direct) {
            return direct_lighting(origin, placed, radius, mb.lighting, scene.lights);
        }
        return unlit_lighting();
    }

    // Enemy `ei`: TdPawn.MyLightEnvironment, at the middle of the pawn.
    const LightEnvLighting& enemy(const LevelScene& scene, size_t ei, float now) {
        if (enemies_.size() <= ei) enemies_.resize(ei + 1);
        const EnemyBot& bot = scene.enemies[ei];
        const Vec3 origin = bot.position + Vec3(0.0f, 0.0f, 90.0f);
        const SH9RGB direct = gather_dynamic_lights(origin, 110.0f, pawn_light_environment().channels, scene.lights);
        enemies_[ei].update(origin, 110.0f, pawn_light_environment(), scene.lights, scene.collision.get(), now, &direct);
        return enemies_[ei].lighting();
    }

    // The first-person mesh: TdPlayerPawn.MyLightEnvironment1P (the arms and the held weapon; the
    // legs' own environment differs only in its shadow distance and bounce), at the middle of the pawn.
    const LightEnvLighting& first_person(const LevelScene& scene, const Vec3& pawn_position, float now) {
        const Vec3 origin = pawn_position + Vec3(0.0f, 0.0f, 100.0f);
        const SH9RGB direct = gather_dynamic_lights(origin, 100.0f, arms_light_environment().channels, scene.lights);
        player_.update(origin, 100.0f, arms_light_environment(), scene.lights, scene.collision.get(), now, &direct);
        debug_light_environment("first person", origin, 100.0f, arms_light_environment(), scene.lights, scene.collision.get(),
                                scene.actors, player_.lighting(), now);
        return player_.lighting();
    }

    // The shadow lights (mod_shadow.hpp). The player's is that of the body's own environment
    // (TdPawn.MyLightEnvironment), whose ambient shadow source leans behind her:
    // Normal(vect(0,0,10) - vector(Rotation)).
    const ShadowLight& player_shadow(const LevelScene& scene, const Vec3& pawn_position, float yaw_deg, float now) {
        LightEnvSettings s = pawn_light_environment();
        const float yaw = yaw_deg * 3.14159265f / 180.0f;
        s.ambient_shadow_dir = Vec3(-std::cos(yaw), -std::sin(yaw), 10.0f);
        player_body_.update(pawn_position + Vec3(0.0f, 0.0f, 90.0f), 110.0f, s, scene.lights, scene.collision.get(), now);
        return player_body_.shadow();
    }
    const ShadowLight& enemy_shadow(const LevelScene& scene, size_t ei, float now) {
        (void)enemy(scene, ei, now);
        return enemies_[ei].shadow();
    }
    // Scene mesh `i`, which has an environment; also gives the subject's sphere.
    ShadowLight mesh_shadow(const LevelScene& scene, size_t i, float now, Vec3& center, float& radius) {
        Vec3 placed;
        mesh_bounds(scene, i, center, radius, &placed);
        LightEnvironment& env = meshes_[i];
        const SH9RGB direct = gather_dynamic_lights(placed, radius, scene.meshes[i].lighting.channels, scene.lights);
        env.update(center, radius, scene.meshes[i].lighting, scene.lights, scene.collision.get(), now, &direct);
        return env.shadow();
    }

private:
    std::string map_;
    LightEnvironment player_;
    LightEnvironment player_body_;
    std::vector<LightEnvironment> enemies_;
    std::unordered_map<size_t, LightEnvironment> meshes_;  // by scene mesh
};

// SceneUniforms' environment members for one object.
inline void fill_light_env_uniforms(const LightEnvLighting& l, LightEnvUniformsGPU& u) {
    u = LightEnvUniformsGPU{};
    u.point[0] = l.point_pos.x;
    u.point[1] = l.point_pos.y;
    u.point[2] = l.point_pos.z;
    u.point[3] = l.has_point ? l.point_radius : 0.0f;
    u.point_color[0] = l.point_color.x;
    u.point_color[1] = l.point_color.y;
    u.point_color[2] = l.point_color.z;
    u.point_color[3] = 1.0f;  // an environment is given
    for (int i = 0; i < 9; ++i) {
        u.sh[i][0] = l.sh.c[i].x;
        u.sh[i][1] = l.sh.c[i].y;
        u.sh[i][2] = l.sh.c[i].z;
    }
    u.sky_upper[0] = l.sky_upper.x;
    u.sky_upper[1] = l.sky_upper.y;
    u.sky_upper[2] = l.sky_upper.z;
    u.sky_upper[3] = l.sky_instead ? 1.0f : 0.0f;
    u.sky_lower[0] = l.sky_lower.x;
    u.sky_lower[1] = l.sky_lower.y;
    u.sky_lower[2] = l.sky_lower.z;
}

// SceneUniforms' environment members for what is drawn next: `lighting` (null: the level's own
// geometry, which has none), its point light's place taken relative to `relative_to` (the
// first-person mesh is drawn around the eye). False when `env` already says so.
inline bool light_env_uniforms(const LightEnvLighting* lighting, const Vec3& relative_to, LightEnvUniformsGPU& env) {
    LightEnvUniformsGPU next{};
    if (lighting) {
        fill_light_env_uniforms(*lighting, next);
        next.point[0] -= relative_to.x;
        next.point[1] -= relative_to.y;
        next.point[2] -= relative_to.z;
    }
    if (std::memcmp(&next, &env, sizeof(env)) == 0) return false;
    env = next;
    return true;
}

}  // namespace me
