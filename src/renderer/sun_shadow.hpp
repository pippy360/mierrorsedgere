#pragma once

// -----------------------------------------------------------------------------
// Real-time directional sun shadows: two orthographic cascades in one 2-slice
// Depth32Float array texture.
//   slice 0 (near): 7200 UU square around the camera, 1.76 UU texels, every caster
//                   (static level geometry, moving elevator parts, barge doors, enemies);
//                   re-rendered every frame.
//   slice 1 (far) : 49152 UU square, 12 UU texels, static level geometry only;
//                   re-rendered only when the camera has moved far enough for the
//                   square's centre to step, or the scene changed.
// Beyond the near square the far cascade takes over, so distant shadows do not
// pop in or out with the camera.
//
// The projections (built here on the CPU) and the MSL that filters the maps are
// shared by the renderer's built-in shaders (metal_renderer.mm) and by the
// generated UE3 material shaders (material_system.cpp), so every surface
// resolves the sun's shadow the same way.
//
// Stability: the cascades follow the camera, yet nothing about a world point's
// shadow may depend on where the camera is or which way it looks:
//  * A map must never slide across the world by a fraction of a texel, or every
//    shadow edge is rasterised differently each frame and crawls whenever the
//    camera moves. So the light's basis depends on the sun direction alone, and
//    each map's centre is snapped to whole texels along the light's own axes.
//    Snapping on the world X/Y/Z axes is not enough, because the light's axes
//    are oblique to them.
//  * The squares are centred on the camera position, not ahead of it, so turning
//    the camera changes nothing at all.
//  * Every caster has to land in a map's depth range. A near plane only a few
//    thousand UU sunward of the camera cuts through the towers and cranes around
//    the rooftops, and because that plane moves with the camera, their shadows
//    grew and shrank as the player moved or turned. The ranges reach far enough
//    towards the sun to hold every building, but not the sky domes, which sit
//    hundreds of thousands of UU out and would shade everything.
//  * The filter's footprint is fixed in shadow-map texels and nothing in it
//    depends on screen pixels.
// -----------------------------------------------------------------------------

#include "scene_shading_msl.hpp"
#include "math/types.hpp"

#include <cmath>
#include <string>

namespace me {

// Both cascades share one array texture, so they share its resolution.
constexpr int kSunShadowMapSize = 4096;   // texels per side
constexpr int kSunShadowNearSlice = 0;
constexpr int kSunShadowFarSlice = 1;

// Near cascade.
constexpr float kSunShadowHalfExtent = 3600.0f;  // light-space half-width of the square (UU)
constexpr float kSunShadowTexel =                // 1.7578125 UU, exact in float
    2.0f * kSunShadowHalfExtent / static_cast<float>(kSunShadowMapSize);
constexpr float kSunShadowSunwardReach = 60000.0f;  // casters up to this far towards the sun from the centre
constexpr float kSunShadowDepthBehind = 20000.0f;   // receivers up to this far beyond the centre along the ray
constexpr float kSunShadowDepthRange = kSunShadowSunwardReach + kSunShadowDepthBehind;  // UU per unit of map depth
constexpr float kSunShadowSlopeBiasCap = 210.0f;    // cap of the shadow pass's slope-scaled depth bias (UU)

// Far cascade. Its centre snaps to a coarse grid, so the map stays valid (and is not re-rendered) until
// the camera has moved about kSunShadowFarStep. Receivers can sit far from the centre, where the light-space
// depth of the ground differs a lot from the centre's under a low sun, hence the deeper range.
constexpr float kSunShadowFarTexel = 12.0f;
constexpr float kSunShadowFarHalfExtent = 0.5f * kSunShadowFarTexel * static_cast<float>(kSunShadowMapSize);  // 24576
constexpr float kSunShadowFarStep = 128.0f * kSunShadowFarTexel;  // 1536 UU
constexpr float kSunShadowFarSunwardReach = 120000.0f;
constexpr float kSunShadowFarDepthBehind = 60000.0f;
constexpr float kSunShadowFarDepthRange = kSunShadowFarSunwardReach + kSunShadowFarDepthBehind;
constexpr float kSunShadowFarSlopeBiasCap = kSunShadowSlopeBiasCap * (kSunShadowFarTexel / kSunShadowTexel);

// Rotation-only light view: light-space x, y and depth are fixed linear functions of the world position,
// independent of the camera. Depth grows away from the sun. `to_sun` points from the scene towards the sun.
inline Mat4 sun_light_view(const Vec3& to_sun) {
    const Vec3 sun = to_sun.normalized();
    const Vec3 up = (std::abs(sun.z) < 0.95f) ? Vec3(0.0f, 0.0f, 1.0f) : Vec3(1.0f, 0.0f, 0.0f);
    return Mat4::look_at(Vec3(0.0f, 0.0f, 0.0f), -sun, up);
}

// Orthographic projection of a square of half-width `half_extent` around `focus`, whose light-space centre
// is snapped to multiples of `step` (a whole number of texels). The result only changes when the focus
// crosses a step boundary, and then by whole texels, so fixed world points keep their sub-texel position.
inline Mat4 sun_shadow_cascade(const Mat4& light_view, const Vec3& focus, float step, float half_extent,
                               float sunward_reach, float depth_behind) {
    const Vec3 centre = light_view.transform_point(focus);
    auto snap = [step](float v) { return std::round(v / step) * step; };
    const float cx = snap(centre.x);
    const float cy = snap(centre.y);
    const float cz = snap(centre.z);  // depth too, so the whole matrix is piecewise constant
    const Mat4 proj = Mat4::ortho(cx - half_extent, cx + half_extent, cy - half_extent, cy + half_extent,
                                  cz - sunward_reach, cz + depth_behind);
    return proj * light_view;
}

// Near cascade view-projection for a camera at `cam_pos`.
inline Mat4 sun_shadow_view_proj(const Vec3& to_sun, const Vec3& cam_pos) {
    return sun_shadow_cascade(sun_light_view(to_sun), cam_pos, kSunShadowTexel, kSunShadowHalfExtent,
                              kSunShadowSunwardReach, kSunShadowDepthBehind);
}

// Far cascade view-projection for a camera at `cam_pos`. Constant while the camera stays in one
// kSunShadowFarStep cell, so the renderer only re-renders the far map when this matrix changes.
inline Mat4 sun_shadow_far_view_proj(const Vec3& to_sun, const Vec3& cam_pos) {
    return sun_shadow_cascade(sun_light_view(to_sun), cam_pos, kSunShadowFarStep, kSunShadowFarHalfExtent,
                              kSunShadowFarSunwardReach, kSunShadowFarDepthBehind);
}

// Main menu (TdMainMenu's miniature City of Glass): one fixed square around the model in slice 0, no far
// cascade. The camera never changes it, so there is nothing to keep stable.
constexpr float kSunShadowMenuHalfExtent = 2400.0f;
inline Mat4 sun_shadow_menu_view_proj(const Vec3& to_sun) {
    return sun_shadow_cascade(sun_light_view(to_sun), Vec3(0.0f, 0.0f, 120.0f),
                              2.0f * kSunShadowMenuHalfExtent / static_cast<float>(kSunShadowMapSize),
                              kSunShadowMenuHalfExtent, kSunShadowSunwardReach, kSunShadowDepthBehind);
}

// MSL lookup into the cascades, prepended to both shader sources (needs only metal_stdlib):
//   float sun_shadow_visibility(depth2d_array<float> maps, float4x4 near_vp, float4x4 far_vp, bool use_far,
//                               float3 biased_wpos, float3 N, float ndl, float depth_bias_uu)
//     use_far       : false in the main menu, where slice 0 is the only map and fades out towards its edge
//     biased_wpos   : receiver world position, already pushed off the surface by the caller's offsets
//     N, ndl        : receiver normal and saturate(dot(N, to_sun)); the far cascade adds its own normal offset
//     depth_bias_uu : extra depth bias towards the sun, in UU
//   Returns sun visibility in [0, 1] (1 = lit). It is 1 beyond the last map, fading out towards its edge.
inline const std::string& sun_shadow_msl() {
    static const std::string source = std::string(R"msl(
#include <metal_stdlib>
using namespace metal;

constant float kSunShadowDepthRange = )msl") + std::to_string(kSunShadowDepthRange) + R"msl(;     // near: UU per unit of map depth
constant float kSunShadowFarDepthRange = )msl" + std::to_string(kSunShadowFarDepthRange) + R"msl(;  // far: UU per unit of map depth
constant float kSunShadowFarTexel = )msl" + std::to_string(kSunShadowFarTexel) + R"msl(;            // far texel size (UU)

// 3x3 grid of hardware 2x2 comparison (bilinear PCF) taps, one texel apart: a 4x4-texel footprint fixed to
// the shadow map, and so to the world. The result varies smoothly with the receiver's position.
inline float sun_shadow_pcf(depth2d_array<float> maps, uint slice, float3 ndc, float ref_z) {
    constexpr sampler cmp(coord::normalized, filter::linear, address::clamp_to_edge, compare_func::less_equal);
    float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    float2 texel = 1.0 / float2(maps.get_width(), maps.get_height());
    float vis = 0.0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            vis += maps.sample_compare(cmp, uv + float2(dx, dy) * texel, slice, ref_z);
        }
    }
    return vis * (1.0 / 9.0);
}

inline float sun_shadow_visibility(depth2d_array<float> maps, float4x4 near_vp, float4x4 far_vp, bool use_far,
                                   float3 biased_wpos, float3 N, float ndl, float depth_bias_uu) {
    // Near cascade (orthographic projections: w == 1).
    float3 n = (near_vp * float4(biased_wpos, 1.0)).xyz;
    float near_edge = max(abs(n.x), abs(n.y));
    bool near_depth_ok = n.z > 0.001 && n.z < 0.999;
    if (!use_far) {
        // The only map: fades to lit towards its edge.
        if (!near_depth_ok || near_edge >= 0.99) return 1.0;
        float v = sun_shadow_pcf(maps, 0, n, n.z - depth_bias_uu / kSunShadowDepthRange);
        return mix(v, 1.0, smoothstep(0.88, 0.98, near_edge));
    }
    // Hands over to the far cascade across the outer part of its square.
    float near_w = near_depth_ok ? 1.0 - smoothstep(0.75, 0.97, near_edge) : 0.0;
    float vis_near = 1.0;
    if (near_w > 0.0) {
        vis_near = sun_shadow_pcf(maps, 0, n, n.z - depth_bias_uu / kSunShadowDepthRange);
        if (near_w >= 1.0) return vis_near;
    }

    // Far cascade: its texels are ~7x larger, so the receiver needs a proportionally larger offset.
    float3 f = (far_vp * float4(biased_wpos + N * (kSunShadowFarTexel * mix(1.5, 0.5, ndl)), 1.0)).xyz;
    float far_edge = max(abs(f.x), abs(f.y));
    float vis_far = 1.0;
    if (far_edge < 0.99 && f.z > 0.001 && f.z < 0.999) {
        float v = sun_shadow_pcf(maps, 1, f, f.z - (depth_bias_uu + kSunShadowFarTexel) / kSunShadowFarDepthRange);
        vis_far = mix(v, 1.0, smoothstep(0.88, 0.98, far_edge));
    }
    return mix(vis_far, vis_near, near_w);
}
)msl" + kSceneShadingMSL;
    return source;
}

}  // namespace me
