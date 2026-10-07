#include "metal_renderer.hpp"
#include "sun_shadow.hpp"
#include "../anim/anim_system.hpp"
#include "../assets/scene_materials.hpp"
#include "../cutscene/cutscene_player.hpp"
#include "../ui/main_menu.hpp"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <simd/simd.h>

#include <iostream>
#include <fstream>
#include <vector>
#include <cmath>
#include <string>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <thread>

namespace me {

// -----------------------------------------------------------------------------
// 5x7 Bitmap ASCII Font Table (ASCII 32 ' ' to 126 '~')
// Each character is 5 columns, 7 bits high (LSB at top, row 0).
// -----------------------------------------------------------------------------
static const uint8_t FONT_5X7[95][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, // 32 ' '
    {0x00, 0x00, 0x5F, 0x00, 0x00}, // 33 '!'
    {0x00, 0x07, 0x00, 0x07, 0x00}, // 34 '"'
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, // 35 '#'
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, // 36 '$'
    {0x23, 0x13, 0x08, 0x64, 0x62}, // 37 '%'
    {0x36, 0x49, 0x55, 0x22, 0x50}, // 38 '&'
    {0x00, 0x05, 0x03, 0x00, 0x00}, // 39 '\''
    {0x00, 0x1C, 0x22, 0x41, 0x00}, // 40 '('
    {0x00, 0x41, 0x22, 0x1C, 0x00}, // 41 ')'
    {0x14, 0x08, 0x3E, 0x08, 0x14}, // 42 '*'
    {0x08, 0x08, 0x3E, 0x08, 0x08}, // 43 '+'
    {0x00, 0x50, 0x30, 0x00, 0x00}, // 44 ','
    {0x08, 0x08, 0x08, 0x08, 0x08}, // 45 '-'
    {0x00, 0x60, 0x60, 0x00, 0x00}, // 46 '.'
    {0x20, 0x10, 0x08, 0x04, 0x02}, // 47 '/'
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, // 48 '0'
    {0x00, 0x42, 0x7F, 0x40, 0x00}, // 49 '1'
    {0x42, 0x61, 0x51, 0x49, 0x46}, // 50 '2'
    {0x21, 0x41, 0x45, 0x4B, 0x31}, // 51 '3'
    {0x18, 0x14, 0x12, 0x7F, 0x10}, // 52 '4'
    {0x27, 0x45, 0x45, 0x45, 0x39}, // 53 '5'
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, // 54 '6'
    {0x01, 0x71, 0x09, 0x05, 0x03}, // 55 '7'
    {0x36, 0x49, 0x49, 0x49, 0x36}, // 56 '8'
    {0x06, 0x49, 0x49, 0x29, 0x1E}, // 57 '9'
    {0x00, 0x36, 0x36, 0x00, 0x00}, // 58 ':'
    {0x00, 0x56, 0x36, 0x00, 0x00}, // 59 ';'
    {0x08, 0x14, 0x22, 0x41, 0x00}, // 60 '<'
    {0x14, 0x14, 0x14, 0x14, 0x14}, // 61 '='
    {0x00, 0x41, 0x22, 0x14, 0x08}, // 62 '>'
    {0x02, 0x01, 0x51, 0x09, 0x06}, // 63 '?'
    {0x32, 0x49, 0x79, 0x41, 0x3E}, // 64 '@'
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, // 65 'A'
    {0x7F, 0x49, 0x49, 0x49, 0x36}, // 66 'B'
    {0x3E, 0x41, 0x41, 0x41, 0x22}, // 67 'C'
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, // 68 'D'
    {0x7F, 0x49, 0x49, 0x49, 0x41}, // 69 'E'
    {0x7F, 0x09, 0x09, 0x09, 0x01}, // 70 'F'
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, // 71 'G'
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, // 72 'H'
    {0x00, 0x41, 0x7F, 0x41, 0x00}, // 73 'I'
    {0x20, 0x40, 0x41, 0x3F, 0x01}, // 74 'J'
    {0x7F, 0x08, 0x14, 0x22, 0x41}, // 75 'K'
    {0x7F, 0x40, 0x40, 0x40, 0x40}, // 76 'L'
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, // 77 'M'
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, // 78 'N'
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, // 79 'O'
    {0x7F, 0x09, 0x09, 0x09, 0x06}, // 80 'P'
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, // 81 'Q'
    {0x7F, 0x09, 0x19, 0x29, 0x46}, // 82 'R'
    {0x46, 0x49, 0x49, 0x49, 0x31}, // 83 'S'
    {0x01, 0x01, 0x7F, 0x01, 0x01}, // 84 'T'
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, // 85 'U'
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, // 86 'V'
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, // 87 'W'
    {0x63, 0x14, 0x08, 0x14, 0x63}, // 88 'X'
    {0x07, 0x08, 0x70, 0x08, 0x07}, // 89 'Y'
    {0x61, 0x51, 0x49, 0x45, 0x43}, // 90 'Z'
    {0x00, 0x7F, 0x41, 0x41, 0x00}, // 91 '['
    {0x02, 0x04, 0x08, 0x10, 0x20}, // 92 '\'
    {0x00, 0x41, 0x41, 0x7F, 0x00}, // 93 ']'
    {0x04, 0x02, 0x01, 0x02, 0x04}, // 94 '^'
    {0x40, 0x40, 0x40, 0x40, 0x40}, // 95 '_'
    {0x00, 0x01, 0x02, 0x04, 0x00}, // 96 '`'
    {0x20, 0x54, 0x54, 0x54, 0x78}, // 97 'a'
    {0x7F, 0x48, 0x44, 0x44, 0x38}, // 98 'b'
    {0x38, 0x44, 0x44, 0x44, 0x20}, // 99 'c'
    {0x38, 0x44, 0x44, 0x48, 0x7F}, // 100 'd'
    {0x38, 0x54, 0x54, 0x54, 0x18}, // 101 'e'
    {0x08, 0x7E, 0x09, 0x01, 0x02}, // 102 'f'
    {0x0C, 0x52, 0x52, 0x52, 0x3E}, // 103 'g'
    {0x7F, 0x08, 0x04, 0x04, 0x78}, // 104 'h'
    {0x00, 0x44, 0x7D, 0x40, 0x00}, // 105 'i'
    {0x20, 0x40, 0x44, 0x3D, 0x00}, // 106 'j'
    {0x7F, 0x10, 0x28, 0x44, 0x00}, // 107 'k'
    {0x00, 0x41, 0x7F, 0x40, 0x00}, // 108 'l'
    {0x7C, 0x04, 0x18, 0x04, 0x78}, // 109 'm'
    {0x7C, 0x08, 0x04, 0x04, 0x78}, // 110 'n'
    {0x38, 0x44, 0x44, 0x44, 0x38}, // 111 'o'
    {0x7C, 0x14, 0x14, 0x14, 0x08}, // 112 'p'
    {0x08, 0x14, 0x14, 0x18, 0x7C}, // 113 'q'
    {0x7C, 0x08, 0x04, 0x04, 0x08}, // 114 'r'
    {0x48, 0x54, 0x54, 0x54, 0x20}, // 115 's'
    {0x04, 0x3F, 0x44, 0x40, 0x20}, // 116 't'
    {0x3C, 0x40, 0x40, 0x20, 0x7C}, // 117 'u'
    {0x1C, 0x20, 0x40, 0x20, 0x1C}, // 118 'v'
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, // 119 'w'
    {0x44, 0x28, 0x10, 0x28, 0x44}, // 120 'x'
    {0x0C, 0x50, 0x50, 0x50, 0x3C}, // 121 'y'
    {0x44, 0x64, 0x54, 0x4C, 0x44}, // 122 'z'
    {0x00, 0x08, 0x36, 0x41, 0x00}, // 123 '{'
    {0x00, 0x00, 0x7F, 0x00, 0x00}, // 124 '|'
    {0x00, 0x41, 0x36, 0x08, 0x00}, // 125 '}'
    {0x08, 0x08, 0x2A, 0x1C, 0x08}  // 126 '~'
};

// -----------------------------------------------------------------------------
// MSL Shader Source Code (Authentic Mirror's Edge USF Shaders Translated)
// -----------------------------------------------------------------------------
static const char* MSL_SHADERS = R"msl(
#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    packed_float3 position;
    packed_float3 normal;
    packed_float3 tangent;
    float u, v;
    float u2, v2;
    uint color;
    float tangent_sign;
};

struct FrameUniforms {
    float4x4 view_proj;
    float4x4 model;
    packed_float3 camera_pos;
    float sim_time;
    packed_float3 sun_dir;
    float runner_vision_strength;
    packed_float3 sun_color;
    float exposure;
    packed_float3 sky_color;
    float speed_2d;
    packed_float3 ground_color;
    float reaction_active;
    packed_float3 actor_tint;
    float is_runner_vision;
    packed_float3 cam_forward;
    float fov_tan;
    packed_float3 cam_right;
    float aspect;
    packed_float3 cam_up;
    float health;
    float4x4 sun_view_proj;
    packed_float3 mod_shadow_color;
    float shadow_enabled;
    float4x4 sun_view_proj_far;
};

struct ShadowVertexOut {
    float4 clip_pos [[position]];
};

vertex ShadowVertexOut shadow_vertex(constant VertexIn* vertices [[buffer(0)]],
                                     constant FrameUniforms& uniforms [[buffer(1)]],
                                     uint vertex_id [[vertex_id]]) {
    ShadowVertexOut out;
    float4 world_pos4 = uniforms.model * float4(float3(vertices[vertex_id].position), 1.0);
    out.clip_pos = uniforms.sun_view_proj * world_pos4;
    return out;
}

struct VertexOut {
    float4 clip_pos [[position]];
    float3 world_pos;
    float3 world_norm;
    float2 uv;
    float2 uv2;
    float4 color;
};

// -----------------------------------------------------------------------------
// 1. Sky & Atmospheric Directional Haze Pass (TdDirHaze.usf + City Skyline)
// -----------------------------------------------------------------------------
struct SkyVertexOut {
    float4 position [[position]];
    float2 uv;
};

vertex SkyVertexOut sky_vertex(uint vertex_id [[vertex_id]]) {
    SkyVertexOut out;
    float2 grid = float2((vertex_id << 1) & 2, vertex_id & 2);
    out.position = float4(grid * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.99999, 1.0);
    out.uv = grid;
    return out;
}

fragment float4 sky_fragment(SkyVertexOut in [[stage_in]],
                             constant FrameUniforms& uniforms [[buffer(0)]]) {
    float2 ndc = in.uv * 2.0 - 1.0;
    ndc.y = -ndc.y;

    float3 forward = normalize(float3(uniforms.cam_forward));
    float3 right   = normalize(float3(uniforms.cam_right));
    float3 up      = normalize(float3(uniforms.cam_up));
    float3 sun_dir = normalize(float3(uniforms.sun_dir));

    // Exact world-space view ray from camera basis (supports pitch, yaw, and 15° wallrun roll)
    float3 V = normalize(forward + right * (ndc.x * uniforms.aspect * uniforms.fov_tan) + up * (ndc.y * uniforms.fov_tan));

    // Authentic Mirror's Edge City of Glass Sky Palette:
    // Deep cerulean blue zenith -> vibrant cyan mid-sky -> crisp sunlit horizon haze
    float3 zenith_col  = float3(0.10, 0.38, 0.82);
    float3 mid_sky_col = float3(0.32, 0.62, 0.95);
    float3 horizon_col = float3(0.84, 0.91, 0.98);

    float z_up = clamp(V.z, 0.0, 1.0);
    float3 sky_gradient = mix(horizon_col, mid_sky_col, smoothstep(0.0, 0.28, z_up));
    sky_gradient = mix(sky_gradient, zenith_col, smoothstep(0.22, 0.85, z_up));

    // Directional sun corona & atmospheric scattering
    float sun_dot = max(dot(V, sun_dir), 0.0);
    float sun_disc = smoothstep(0.9985, 0.9996, sun_dot) * 2.5 + pow(sun_dot, 96.0) * 0.45;
    float sun_haze = pow(sun_dot, 6.0) * 0.18 * (1.0 - z_up * 0.7);
    float3 final_sky = sky_gradient + float3(1.0, 0.97, 0.90) * (sun_disc + sun_haze);

    // Subtle high-altitude cirrus cloud streaks
    if (V.z > 0.04) {
        float2 cloud_uv = V.xy / (V.z + 0.15) * 1.8;
        float c1 = sin(cloud_uv.x * 2.3 + cloud_uv.y * 1.1) * cos(cloud_uv.x * 1.7 - cloud_uv.y * 2.9);
        float cloud = smoothstep(0.35, 0.85, c1) * smoothstep(0.04, 0.25, V.z) * (1.0 - smoothstep(0.5, 0.9, V.z));
        final_sky = mix(final_sky, float3(0.96, 0.98, 1.0), cloud * 0.25);
    }

    // Distant City of Glass Horizon Skyline Silhouette (multi-layered high-rise blocks near horizon)
    float az = atan2(V.y, V.x); // [-pi, pi]
    float b1 = fract(sin(floor(az * 28.0) * 127.1) * 43758.5453);
    float b2 = fract(sin(floor(az * 46.0 + 1.7) * 269.5) * 18342.231);
    float h_back = 0.015 + 0.085 * step(0.25, b1) * b1;
    float h_mid  = 0.005 + 0.060 * step(0.35, b2) * b2;

    if (V.z < h_back && V.z > -0.25) {
        // Distant haze-tinted white/blue skyscraper silhouette
        float3 tower_back = mix(float3(0.74, 0.84, 0.94), float3(0.82, 0.89, 0.96), step(0.0, sin(az * 28.0 * 3.14159)));
        final_sky = mix(final_sky, tower_back, smoothstep(h_back, h_back - 0.003, V.z));
    }
    if (V.z < h_mid && V.z > -0.25) {
        float3 tower_mid = mix(float3(0.64, 0.75, 0.88), float3(0.78, 0.85, 0.93), step(0.0, sin(az * 46.0 * 3.14159)));
        // Subtle window grid on distant towers
        float win_v = step(0.45, fract(V.z * 220.0));
        float win_u = step(0.35, fract(az * 46.0 * 6.0));
        tower_mid -= (win_v * win_u) * 0.05;
        final_sky = mix(final_sky, tower_mid, smoothstep(h_mid, h_mid - 0.003, V.z));
    }
    if (V.z <= -0.02) {
        // Below-horizon street canyon haze
        float down_t = saturate((-V.z - 0.02) * 3.5);
        final_sky = mix(final_sky, float3(0.55, 0.66, 0.78), down_t);
    }

    return float4(final_sky, 1.0);
}

// -----------------------------------------------------------------------------
// 2. 3D World Geometry Pass (BasePass.usf + Beast Radiosity Emulation)
// -----------------------------------------------------------------------------
vertex VertexOut world_vertex(constant VertexIn* vertices [[buffer(0)]],
                              constant FrameUniforms& uniforms [[buffer(1)]],
                              uint vertex_id [[vertex_id]]) {
    VertexIn v = vertices[vertex_id];
    VertexOut out;

    float3 pos = float3(v.position);
    float3 norm = float3(v.normal);

    float4 world_pos4 = uniforms.model * float4(pos, 1.0);
    out.world_pos = world_pos4.xyz;
    out.world_norm = normalize((uniforms.model * float4(norm, 0.0)).xyz);
    out.clip_pos = uniforms.view_proj * world_pos4;
    out.uv = float2(v.u, v.v);
    out.uv2 = float2(v.u2, v.v2);

    // Unpack vertex color (ABGR / RGBA)
    float r = float((v.color >> 0) & 0xFF) / 255.0;
    float g = float((v.color >> 8) & 0xFF) / 255.0;
    float b = float((v.color >> 16) & 0xFF) / 255.0;
    float a = float((v.color >> 24) & 0xFF) / 255.0;
    out.color = float4(r, g, b, a);

    return out;
}

// Sun visibility through the shared, world-stable shadow cascade lookup (renderer/sun_shadow.hpp).
inline float sample_world_shadow(float3 world_pos, float3 N, constant FrameUniforms& uniforms, depth2d_array<float> shadow_map) {
    if (uniforms.shadow_enabled < 0.5) return 1.0;
    float3 Lw = normalize(float3(uniforms.sun_dir));
    float ndl = saturate(dot(N, Lw));
    if (ndl <= 0.001) return 0.0;
    float3 biased_pos = world_pos + N * mix(18.0, 5.0, ndl) + Lw * 6.0;
    return sun_shadow_visibility(shadow_map, uniforms.sun_view_proj, uniforms.sun_view_proj_far,
                                 /*use_far=*/uniforms.shadow_enabled < 1.5, biased_pos, N, ndl, mix(30.8, 8.4, ndl));
}

fragment float4 world_fragment(VertexOut in [[stage_in]],
                               constant FrameUniforms& uniforms [[buffer(0)]],
                               texture2d<float> swat_d_tex [[texture(0)]],
                               texture2d<float> wep_d_tex  [[texture(1)]],
                               texture2d<float> wep_s_tex  [[texture(2)]],
                               texture2d<float> ammo_d_tex [[texture(3)]],
                               texture2d<float> swat_s_tex [[texture(4)]],
                               texture2d<float> swat_n_tex [[texture(5)]],
                               depth2d_array<float> shadow_map [[texture(27)]],
                               sampler world_tex_sampler   [[sampler(0)]]) {
    // Compute geometric facet normal from screen-space derivatives to guarantee crisp architectural planes
    float3 dpdx = dfdx(in.world_pos);
    float3 dpdy = dfdy(in.world_pos);
    float3 geo_N = cross(dpdx, dpdy);
    float geo_len = length(geo_N);
    geo_N = (geo_len > 1e-6) ? (geo_N / geo_len) : float3(0.0, 0.0, 1.0);

    float3 V = normalize(float3(uniforms.camera_pos) - in.world_pos);
    if (dot(geo_N, V) < 0.0) geo_N = -geo_N;

    float3 vtx_N = in.world_norm;
    float vtx_len = length(vtx_N);
    vtx_N = (vtx_len > 1e-4) ? (vtx_N / vtx_len) : geo_N;

    float3 N;
    if (in.uv2.x > 0.5) {
        // 3D skinned characters & weapons: preserve smooth vertex normals (no flat geo_N faceting)
        if (dot(vtx_N, V) < -0.25) vtx_N = -vtx_N;
        N = vtx_N;
        if (in.uv2.x < 1.12) {
            // Apply high-resolution 2048x2048 tangent-space normal map (T_TKY_Cop_SWAT_N)
            float2 duvdx = dfdx(in.uv);
            float2 duvdy = dfdy(in.uv);
            float det_uv = duvdx.x * duvdy.y - duvdx.y * duvdy.x;
            float3 T = (abs(det_uv) > 1e-10)
                           ? normalize((dpdx * duvdy.y - dpdy * duvdx.y) * sign(det_uv))
                           : normalize(dpdx - vtx_N * dot(vtx_N, dpdx));
            T = normalize(T - vtx_N * dot(vtx_N, T));
            float3 B = normalize(cross(vtx_N, T));
            float3 n_ts = swat_n_tex.sample(world_tex_sampler, in.uv).rgb * 2.0 - 1.0;
            n_ts.xy *= 0.85;
            n_ts.z = sqrt(max(1.0 - dot(n_ts.xy, n_ts.xy), 0.04));
            N = normalize(T * n_ts.x + B * n_ts.y + vtx_N * n_ts.z);
        }
    } else {
        if (dot(vtx_N, geo_N) < 0.0) vtx_N = -vtx_N;
        N = normalize(mix(geo_N, vtx_N, 0.55));
    }

    float3 L = normalize(float3(uniforms.sun_dir));
    float shadow = sample_world_shadow(in.world_pos, N, uniforms, shadow_map);

    // 1. Directional Sun + UE3 BasePass GetMaterialHemisphereLightTransferFull (MaterialTemplate.usf)
    float NdotL = max(dot(N, L), 0.0) * shadow;
    float side_contrast = 0.5 + 0.5 * N.x - 0.25 * N.y;

    float3 sun_light = float3(0.56, 0.53, 0.48) * NdotL;
    float sky_hemi = saturate(N.z * 0.5 + 0.5);
    float2 hemi_w = float2(0.5, 0.5) + float2(0.5, -0.5) * N.z;
    hemi_w *= hemi_w;
    float3 mod_azure = max(float3(uniforms.mod_shadow_color), float3(0.42, 0.65, 0.92)) * float3(0.65, 0.88, 1.18);
    float3 upper_sky = mix(mod_azure * 0.56, float3(uniforms.sky_color) * 0.64, NdotL);
    float3 lower_sky = float3(uniforms.ground_color) * 0.44;
    float3 sky_bounce = (upper_sky * hemi_w.x + lower_sky * hemi_w.y + float3(0.06, 0.10, 0.16)) * (0.78 + 0.22 * side_contrast);
    float3 lighting = sun_light + sky_bounce;

    // 2. Base Albedo & High-Res GPU Character/Weapon Textures vs Architectural Detailing
    float3 base_albedo = in.color.rgb * float3(uniforms.actor_tint);
    float3 spec_add = float3(0.0);
    float dist = length(float3(uniforms.camera_pos) - in.world_pos);
    float rv_factor = saturate(max(uniforms.is_runner_vision, uniforms.runner_vision_strength));

    if (in.uv2.x > 0.5) {
        // Dynamic UE3 Spherical Harmonics (_SH) hemisphere + half-Lambert wrap + rim lighting for 3D characters & weapons
        float wrap_sun = saturate(dot(N, L) * 0.5 + 0.5) * mix(0.68, 1.0, shadow);
        float view_fill = saturate(dot(N, V)) * 0.34;
        float rim_light = pow(1.0 - saturate(dot(N, V)), 2.6) * 0.44;
        lighting = float3(0.52, 0.56, 0.64) + float3(0.68, 0.65, 0.58) * wrap_sun + view_fill + float3(0.44, 0.55, 0.72) * rim_light;

        float3 H = normalize(L + V);
        float3 H_sky = normalize(normalize(float3(0.25, -0.35, 0.90)) + V);
        float3 R_env = reflect(-V, N);
        float env_h = saturate(R_env.z * 0.5 + 0.5);
        float3 env_col = mix(float3(0.14, 0.19, 0.28), float3(0.72, 0.82, 0.96), env_h);
        float fresnel = pow(1.0 - saturate(dot(N, V)), 2.5);

        if (in.uv2.x < 1.12) {
            // UE3 MI_TKY_Cop_SWAT_SH (Diffuse Multiply = 2.4, Specular Power = 40.0, Mask quadrant at uv*0.5+0.5)
            float3 d_swat = swat_d_tex.sample(world_tex_sampler, in.uv).rgb;
            float3 s_swat = swat_s_tex.sample(world_tex_sampler, in.uv).rgb;
            float3 mask_swat = swat_d_tex.sample(world_tex_sampler, clamp(in.uv, 0.0, 0.998) * 0.5 + 0.5).rgb;
            base_albedo = d_swat * 2.4 + s_swat * 0.24 + float3(0.085, 0.018, 0.010) * mask_swat.b * 0.40;

            float3 refl_add = env_col * s_swat * (0.22 + 0.65 * mask_swat.g) * (0.35 + 0.65 * fresnel);
            float3 cloth_rim = float3(0.07, 0.10, 0.16) * mask_swat.r * pow(1.0 - saturate(dot(N, V)), 3.0);
            float3 spec_lobe = s_swat * (pow(max(dot(N, H), 0.0), 32.0) * 0.55 + pow(max(dot(N, H_sky), 0.0), 20.0) * 0.30);
            spec_add = spec_lobe + refl_add + cloth_rim;
        } else if (in.uv2.x < 1.5) {
            // UE3 MI_TKY_Cop_SWAT_eye_SH (EyeColor = (0.164, 0.184, 0.302), Whiteness = 0.25, Specular = 2.0)
            base_albedo = float3(0.09, 0.10, 0.14);
            spec_add = float3(0.85, 0.90, 0.98) * pow(max(dot(N, H), 0.0), 64.0) * 0.75;
        } else if (in.uv2.x < 2.5) {
            // 3D Enemy Weapon / Dropped Weapon main high-res GPU texture (linear sRGB + cubemap sheen)
            if (in.color.r > 0.85 && in.color.g < 0.20) {
                base_albedo = in.color.rgb;
            } else {
                float3 d_wep = wep_d_tex.sample(world_tex_sampler, in.uv).rgb;
                float3 s_wep = wep_s_tex.sample(world_tex_sampler, in.uv).rgb;
                base_albedo = d_wep * 2.4 + s_wep * 0.24;
                spec_add = s_wep * (pow(max(dot(N, H), 0.0), 20.0) * 0.48 + pow(max(dot(N, H_sky), 0.0), 36.0) * 0.38)
                           + env_col * s_wep * (0.20 + 0.35 * fresnel);
            }
        } else {
            // 3D Weapon M_Ammo brass cartridge / belt section (linear sRGB)
            float3 d_ammo = ammo_d_tex.sample(world_tex_sampler, in.uv).rgb;
            base_albedo = d_ammo * float3(1.65, 1.35, 0.88) + float3(0.05, 0.035, 0.012);
            spec_add = float3(0.85, 0.68, 0.36) * pow(max(dot(N, H), 0.0), 32.0) * 0.55;
        }
    } else if (rv_factor < 0.5) {
        if (abs(N.z) > 0.72) {
            // Horizontal Rooftop / Walkway Concrete Tile Seams (200cm expansion joints + 50cm sub-tiles)
            float2 major_grid = abs(fract(in.world_pos.xy * 0.005) - 0.5);
            float2 minor_grid = abs(fract(in.world_pos.xy * 0.020) - 0.5);
            float major_seam = smoothstep(0.475, 0.492, max(major_grid.x, major_grid.y));
            float minor_seam = smoothstep(0.482, 0.495, max(minor_grid.x, minor_grid.y));

            // Subtle checkerboard concrete panel tone variation
            float2 tile_id = floor(in.world_pos.xy * 0.005);
            float tile_var = fract(sin(dot(tile_id, float2(12.9898, 78.233))) * 43758.5453);
            base_albedo *= (0.93 + 0.06 * tile_var);
            base_albedo -= major_seam * 0.14 + minor_seam * 0.05;
        } else {
            // Vertical Building Facade / Parapet / Wall Detailing
            float horiz_coord = (abs(N.x) > abs(N.y)) ? in.world_pos.y : in.world_pos.x;
            float vert_coord  = in.world_pos.z;

            // Architectural concrete panel seams (every 250cm horizontally, 320cm vertically)
            float u_panel = abs(fract(horiz_coord * 0.004) - 0.5);
            float v_panel = abs(fract(vert_coord * 0.003125) - 0.5);
            float wall_seam = smoothstep(0.475, 0.492, max(u_panel, v_panel));
            base_albedo -= wall_seam * 0.12;

            // High-rise curtain wall recessed glass windows on distant skyscraper facades (> 22m away)
            float dist_facade = smoothstep(2200.0, 3600.0, dist);
            if (dist_facade > 0.01) {
                float win_u = smoothstep(0.18, 0.22, u_panel) * (1.0 - smoothstep(0.44, 0.47, u_panel));
                float win_v = smoothstep(0.16, 0.20, v_panel) * (1.0 - smoothstep(0.42, 0.46, v_panel));
                float is_window = win_u * win_v * dist_facade;
                if (is_window > 0.01) {
                    float3 refl_dir = reflect(-V, N);
                    float3 glass_col = mix(float3(0.22, 0.42, 0.68), float3(0.65, 0.82, 0.96), saturate(refl_dir.z * 0.5 + 0.5));
                    float fresnel = 0.25 + 0.65 * pow(1.0 - max(dot(N, V), 0.0), 3.0);
                    base_albedo = mix(base_albedo, glass_col * (0.7 + 0.5 * fresnel), is_window * 0.65);
                }
            }
        }
    }

    float3 lit_color = base_albedo * lighting + spec_add;

    // 3. Runner Vision (LOI) Saturated Red Highlight (#E61414)
    if (rv_factor > 0.001) {
        float pulse = 0.94 + 0.06 * sin(uniforms.sim_time * 5.0);
        float3 runner_red = float3(0.91, 0.075, 0.065) * pulse;

        // Strong 3D directional shading + edge seam definition on Runner Vision objects
        float3 H = normalize(L + V);
        float spec = pow(max(dot(N, H), 0.0), 28.0) * 0.35;
        float rim  = pow(1.0 - max(dot(N, V), 0.0), 3.0) * 0.22;

        // Subtle industrial panel seams on red pipes/ramps/walls
        float3 p_grid = abs(fract(in.world_pos * 0.01) - 0.5);
        float p_seam = smoothstep(0.47, 0.495, max(p_grid.x, max(p_grid.y, p_grid.z)));

        float3 rv_lit = runner_red * (0.48 + 0.52 * NdotL + 0.15 * sky_hemi - p_seam * 0.12) + float3(spec + rim);
        lit_color = mix(lit_color, rv_lit, rv_factor);
    }

    // 4. Atmospheric Distance Aerial Perspective Haze (TdDirHaze)
    float fog_factor = saturate((dist - 2500.0) / 38000.0);
    float3 fog_col = float3(0.76, 0.86, 0.96);
    lit_color = mix(lit_color, fog_col, fog_factor * 0.65);

    return float4(lit_color, 1.0);
}

// -----------------------------------------------------------------------------
// 3. First-Person Faith Viewmodel (CH_Faith_1P + High-Res 1P Weapons)
// -----------------------------------------------------------------------------
vertex VertexOut viewmodel_vertex(constant VertexIn* vertices [[buffer(0)]],
                                  constant FrameUniforms& uniforms [[buffer(1)]],
                                  uint vertex_id [[vertex_id]]) {
    VertexIn v = vertices[vertex_id];
    VertexOut out;

    float3 pos = float3(v.position);
    float3 norm = float3(v.normal);

    float4 world_pos4 = uniforms.model * float4(pos, 1.0);
    out.world_pos = world_pos4.xyz;
    out.world_norm = normalize((uniforms.model * float4(norm, 0.0)).xyz);
    out.clip_pos = uniforms.view_proj * world_pos4;
    out.uv = float2(v.u, v.v);
    out.uv2 = float2(v.u2, v.v2);

    float r = float((v.color >> 0) & 0xFF) / 255.0;
    float g = float((v.color >> 8) & 0xFF) / 255.0;
    float b = float((v.color >> 16) & 0xFF) / 255.0;
    float a = float((v.color >> 24) & 0xFF) / 255.0;
    out.color = float4(r, g, b, a);

    return out;
}

fragment float4 viewmodel_fragment(VertexOut in [[stage_in]],
                                   constant FrameUniforms& uniforms [[buffer(0)]],
                                   texture2d<float> vm_skin_tex  [[texture(0)]],
                                   texture2d<float> vm_glove_tex [[texture(1)]],
                                   texture2d<float> vm_lower_tex [[texture(2)]],
                                   texture2d<float> vm_wep_d_tex [[texture(3)]],
                                   texture2d<float> vm_wep_s_tex [[texture(4)]],
                                   texture2d<float> vm_wep_n_tex [[texture(5)]],
                                   texture2d<float> vm_ammo_tex  [[texture(6)]],
                                   texture2d<float> vm_wep_m_tex [[texture(7)]],
                                   sampler vm_sampler            [[sampler(0)]]) {
    int slot = int(round(in.uv2.x));
    if (slot == 0) {
        // Emissive muzzle flash star
        return float4(in.color.rgb * 1.35, 1.0);
    }

    float3 dpdx = dfdx(in.world_pos);
    float3 dpdy = dfdy(in.world_pos);
    float3 geo_N = cross(dpdx, dpdy);
    float geo_len = length(geo_N);
    geo_N = (geo_len > 1e-6) ? (geo_N / geo_len) : float3(0.0, -1.0, 0.0);

    float3 V = normalize(-in.world_pos);
    if (dot(geo_N, V) < 0.0) geo_N = -geo_N;

    float3 vtx_N = in.world_norm;
    float vtx_len = length(vtx_N);
    float3 N = (vtx_len > 1e-4) ? (vtx_N / vtx_len) : geo_N;
    if (dot(N, V) < -0.25) N = -N;

    float3 L = normalize(float3(0.35, -0.45, 0.82));
    float3 H = normalize(L + V);

    if (slot == 1) {
        // Faith 1P Arms, Hands, Fingers & Tattoo (Female_1p_C, 2048x2048)
        float3 c = vm_skin_tex.sample(vm_sampler, in.uv).rgb;
        float3 albedo = pow(max(c, float3(0.0)), float3(0.85, 0.88, 0.90)) * float3(1.06, 1.01, 0.96);
        float wrap = saturate((dot(N, L) + 0.35) / 1.35);
        float spec = pow(max(dot(N, H), 0.0), 20.0) * 0.14;
        float rim  = pow(1.0 - max(dot(N, V), 0.0), 3.0) * 0.10;
        float3 col = albedo * (0.44 + 0.56 * wrap) + float3(spec + rim) * float3(1.0, 0.95, 0.90);
        return float4(col, 1.0);
    }

    if (slot == 2) {
        // Faith 1P Red/Black Leather Runner Glove (Faith_Glove_C, 1024x1024)
        float3 c = vm_glove_tex.sample(vm_sampler, in.uv).rgb;
        float3 albedo;
        if (c.x > c.y * 1.32 && c.x > 0.11) {
            albedo = clamp(float3(c.x * 1.62, c.y * 0.65, c.z * 0.65), float3(0.05), float3(0.95));
        } else {
            albedo = pow(max(c, float3(0.0)), float3(0.82)) * 1.16 + float3(0.03, 0.03, 0.04);
        }
        float NdotL = max(dot(N, L), 0.0);
        float spec = pow(max(dot(N, H), 0.0), 26.0) * 0.24;
        float rim  = pow(1.0 - max(dot(N, V), 0.0), 3.0) * 0.12;
        float3 col = albedo * (0.44 + 0.56 * NdotL) + float3(spec + rim);
        return float4(col, 1.0);
    }

    if (slot == 3) {
        // Faith 1P Cargo Pants & Split-Toe Tabi Boots (Faith_Cine_Lower_C, 1024x1024)
        float3 c = vm_lower_tex.sample(vm_sampler, in.uv).rgb;
        float3 albedo;
        if (c.x > c.y * 1.32 && c.x > 0.14) {
            albedo = clamp(float3(c.x * 1.55, c.y * 0.65, c.z * 0.65), float3(0.06), float3(0.94));
        } else {
            albedo = pow(max(c, float3(0.0)), float3(0.78)) * 1.15;
        }
        float NdotL = max(dot(N, L), 0.0);
        float spec = pow(max(dot(N, H), 0.0), 22.0) * 0.15;
        return float4(albedo * (0.46 + 0.54 * NdotL) + float3(spec), 1.0);
    }

    if (slot == 5) {
        // Weapon M_Ammo Brass Cartridges & Belt (T_Ammo_D, sRGB decoded)
        float3 a_lin = vm_ammo_tex.sample(vm_sampler, in.uv).rgb;
        float3 albedo = a_lin * float3(1.55, 1.25, 0.78) + float3(0.04, 0.03, 0.01);
        float NdotL = max(dot(N, L), 0.0);
        float spec_brass = pow(max(dot(N, H), 0.0), 36.0) * 0.75 + pow(max(dot(N, H), 0.0), 12.0) * 0.25;
        float3 col = albedo * (0.35 + 0.65 * NdotL) + float3(0.95, 0.78, 0.40) * spec_brass;
        return float4(col, 1.0);
    }

    if (slot == 6) {
        // Barrett M95 Scope Optical Glass Lens (M_M95_Sight)
        float fresnel = pow(1.0 - max(dot(N, V), 0.0), 2.5);
        float glint = pow(max(dot(N, H), 0.0), 96.0);
        float2 centered = abs(fract(in.uv) - 0.5);
        float crosshair = (min(centered.x, centered.y) < 0.006 && max(centered.x, centered.y) < 0.35) ? 0.18 : 0.0;
        float3 glass_col = float3(0.03, 0.08, 0.14) + float3(0.30, 0.56, 0.88) * fresnel * 0.65 + float3(glint * 0.95) + float3(crosshair * 0.12);
        return float4(glass_col, 1.0);
    }

    // Slot 4: Equipped Weapon High-Resolution UE3 Material Graph (WP_*.upk M_<Weapon>)
    // - Diffuse/Specular/Mask: CoordinateIndex = 2 (in.uv), hardware sRGB -> linear
    // - Normal Map (T_<Weapon>_N): CoordinateIndex = 1 (unpacked 16-bit unorm from in.color), linear [-1, 1]
    float2 uv_n = float2(in.color.r * 255.0 + in.color.g * 65280.0,
                         in.color.b * 255.0 + in.color.a * 65280.0) * (1.0 / 65535.0);

    float3 d_lin = vm_wep_d_tex.sample(vm_sampler, in.uv).rgb;
    float3 s_lin = vm_wep_s_tex.sample(vm_sampler, in.uv).rgb;
    float3 m_lin = vm_wep_m_tex.sample(vm_sampler, in.uv).rgb;
    float3 n_ts  = vm_wep_n_tex.sample(vm_sampler, uv_n).rgb * 2.0 - 1.0;

    // Screen-space cotangent normal perturbation using the Normal Map UV channel (uv_n)
    float2 duv1 = dfdx(uv_n);
    float2 duv2 = dfdy(uv_n);
    float3 dp2perp = cross(dpdy, N);
    float3 dp1perp = cross(N, dpdx);
    float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float t_len2 = max(dot(T, T), dot(B, B));
    if (t_len2 > 1e-10) {
        float invmax = rsqrt(t_len2);
        N = normalize(N + (T * n_ts.x + B * n_ts.y) * (invmax * 0.85));
    }

    // UE3 ReflectionVector (-V reflected about perturbed N) sampling CM_GenericHighlights_01 studio/sky cubemap
    float3 R = reflect(-V, N);
    float sky_band   = saturate(R.z * 0.5 + 0.5);
    float studio_key = pow(saturate(dot(R, normalize(float3(0.25, -0.55, 0.80)))), 6.0);
    float3 cm_highlights = mix(float3(0.06, 0.07, 0.09), float3(0.62, 0.76, 0.95), pow(sky_band, 1.6))
                         + float3(0.85, 0.90, 0.98) * studio_key;

    // Exact UE3 Material Graph:
    //   DiffuseColor  = T_Weapon_D + CM_GenericHighlights_01(ReflectionVector) * T_Weapon_S * T_Weapon_M * 1.5
    //   SpecularColor = T_Weapon_S
    //   SpecularPower = 20.0
    float3 ue3_diffuse_color = d_lin * 1.35 + cm_highlights * s_lin * m_lin * 1.65;

    float NdotL = max(dot(N, L), 0.0);
    float sky_hemi  = saturate(N.z * 0.5 + 0.5);
    float side_fill = saturate(-N.x * 0.5 + 0.5);

    // Linear scene lighting (matches UE3 Tutorial_lgts DirectionalLight + SkyLight before Pass 2 gamma 2.2)
    float3 ambient_light = float3(0.24, 0.27, 0.33) + float3(0.28, 0.38, 0.52) * sky_hemi + float3(0.09, 0.10, 0.12) * side_fill;
    float3 direct_sun    = float3(1.15, 1.08, 0.96) * NdotL;

    float spec_lobe = pow(max(dot(N, H), 0.0), 20.0);
    float fresnel   = pow(1.0 - max(dot(N, V), 0.0), 3.5);
    float3 spec_col = s_lin * (spec_lobe * 0.95 + float3(0.55, 0.70, 0.92) * fresnel * (0.25 + m_lin * 0.75));

    float3 col = ue3_diffuse_color * (ambient_light + direct_sun) + spec_col;
    return float4(col, 1.0);
}

// -----------------------------------------------------------------------------
// 4. Post-Processing, SSAO & Tone Mapping (AmbientOcclusionShader + TdToneMapping + TdMotionBlur)
// -----------------------------------------------------------------------------
struct PostVertexOut {
    float4 position [[position]];
    float2 uv;
};

vertex PostVertexOut post_vertex(uint vertex_id [[vertex_id]]) {
    PostVertexOut out;
    float2 grid = float2((vertex_id << 1) & 2, vertex_id & 2);
    out.position = float4(grid * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    out.uv = grid;
    return out;
}

inline float post_linear_depth(float d) {
    float ndc = saturate((d - 0.05) / 0.95);
    return (5.0 * 65000.0) / (65000.0 - ndc * (65000.0 - 5.0));
}

// Symmetric-pair horizon Screen-Space Ambient Occlusion (AmbientOcclusionShader.usf):
// Opposite sample pairs (uv + off, uv - off) cancel linear surface slope on flat floors/walls
// so flat surfaces have zero self-occlusion while concave corners, curbs, solar panel bases,
// and wall-floor junctions receive smooth cool-azure contact darkening.
inline float compute_ssao(float2 uv, float2 frag_xy, depth2d<float> depth_tex, float aspect) {
    constexpr sampler dsmp(coord::normalized, filter::nearest, address::clamp_to_edge);
    float d0 = depth_tex.sample(dsmp, uv);
    if (d0 < 0.052 || d0 > 0.9992) return 1.0;
    float z0 = post_linear_depth(d0);
    if (z0 > 10000.0) return 1.0;

    float ign = fract(52.9829189 * fract(dot(frag_xy, float2(0.06711056, 0.00583715))));
    float base_ang = ign * 3.14159265;
    float max_rad = clamp(95.0 / z0, 0.0025, 0.018);
    float max_range = clamp(z0 * 0.042, 14.0, 48.0);
    float bias = max(1.4, z0 * 0.0016);
    float norm_scale = max(18.0, z0 * 0.018);

    float occ = 0.0;
    for (int i = 0; i < 8; ++i) {
        float t = (float(i) + 0.5) * (1.0 / 8.0);
        float r = max_rad * (0.20 + 0.80 * t);
        float theta = base_ang + float(i) * 1.1780972;
        float2 off = float2(cos(theta) / aspect, sin(theta)) * r;
        float dp = depth_tex.sample(dsmp, uv + off);
        float dn = depth_tex.sample(dsmp, uv - off);
        if (dp < 0.052 || dn < 0.052) continue;
        float zp = post_linear_depth(dp);
        float zn = post_linear_depth(dn);
        if (abs(zp - z0) < max_range && abs(zn - z0) < max_range) {
            float concavity = z0 - 0.5 * (zp + zn);
            occ += saturate((concavity - bias) / norm_scale);
        }
    }
    float dist_fade = 1.0 - smoothstep(4500.0, 9500.0, z0);
    return 1.0 - saturate(occ * (1.0 / 8.0) * 1.85) * dist_fade;
}

fragment float4 post_fragment(PostVertexOut in [[stage_in]],
                              texture2d<float> scene_tex [[texture(0)]],
                              depth2d<float> depth_tex [[texture(1)]],
                              sampler smp [[sampler(0)]],
                              constant FrameUniforms& uniforms [[buffer(0)]]) {
    float2 uv = in.uv;
    float speed = uniforms.speed_2d;

    // 1. Radial Velocity Motion Blur & Aerodynamic Dispersion (TdMotionBlurShader.usf - peripheral only)
    float3 scene_color = float3(0.0);
    float2 D = uv - float2(0.5, 0.5);
    float r = length(D);
    float max_wind = smoothstep(640.0, 715.0, speed);
    if (speed > 420.0 && r > 0.20) {
        float speed_factor = saturate((speed - 420.0) / 300.0);
        float periph = smoothstep(0.20, 0.68, r);
        float delta_r = min((0.024 * speed_factor + 0.018 * max_wind) * periph, 0.044);
        float2 dir_uv = (r > 1e-4) ? (D / r) : float2(0.0);
        float2 v_step = dir_uv * (delta_r / 8.0);

        float total_weight = 0.0;
        for (int k = 0; k < 8; ++k) {
            float w = 1.0 - float(k) / 8.0;
            float2 sample_uv = clamp(uv - float(k) * v_step, 0.001, 0.999);
            scene_color += scene_tex.sample(smp, sample_uv).rgb * w;
            total_weight += w;
        }
        scene_color /= total_weight;

        // Subtle peripheral R/B aerodynamic dispersion at max speed
        if (max_wind > 0.01) {
            float2 disp = dir_uv * (0.0032 * max_wind * periph);
            scene_color.r = mix(scene_color.r, scene_tex.sample(smp, clamp(uv - disp, 0.001, 0.999)).r, 0.45);
            scene_color.b = mix(scene_color.b, scene_tex.sample(smp, clamp(uv + disp, 0.001, 0.999)).b, 0.45);
        }
    } else {
        scene_color = scene_tex.sample(smp, uv).rgb;
    }

    // 1b. Screen-Space Ambient Occlusion (AmbientOcclusionShader.usf - cool azure-slate crevice AO)
    if (uniforms.shadow_enabled > 0.5) {
        float ao = compute_ssao(uv, in.position.xy, depth_tex, max(uniforms.aspect, 1.0));
        float3 ao_tint = mix(float3(0.24, 0.37, 0.56), float3(1.0), ao);
        scene_color *= ao_tint;
    }

    // 2. Balanced Filmic DICE Shoulder Tone Mapping (preserves highlight detail without #FFFFFF clipping)
    float3 x = max(scene_color * 1.02, 0.0);
    float3 toned = (x * (1.06 * x + 0.16)) / (x * (1.08 * x + 0.44) + 0.14);
    toned = saturate(toned);

    // 2b. On-Screen Max-Speed Wind Streamlines (FX_Wind_Streaks peripheral airflow rays)
    if (max_wind > 0.01) {
        float2 D_asp = D * float2(max(uniforms.aspect, 1.0), 1.0);
        float r_asp = length(D_asp);
        float periph_mask = smoothstep(0.32, 0.72, r_asp);
        if (periph_mask > 0.001) {
            float theta = atan2(D_asp.y, D_asp.x);

            // Layer 1: Primary fast aerodynamic wind filaments
            float a1 = theta * 14.0;
            float id1 = floor(a1);
            float f1 = abs(fract(a1) - 0.5) * 2.0;
            float h1 = fract(sin(id1 * 127.1 + 311.7) * 43758.5453);
            float streak_t1 = fract(r_asp * 2.1 - uniforms.sim_time * (2.6 + 1.4 * h1) + h1 * 6.2831);
            float ray1 = smoothstep(0.45, 0.0, f1) * smoothstep(0.0, 0.25, streak_t1) * smoothstep(0.95, 0.35, streak_t1);

            // Layer 2: Secondary fine high-frequency air slipstream threads
            float a2 = theta * 26.0 + 1.7;
            float id2 = floor(a2);
            float f2 = abs(fract(a2) - 0.5) * 2.0;
            float h2 = fract(sin(id2 * 269.5 + 183.3) * 43758.5453);
            float streak_t2 = fract(r_asp * 2.8 - uniforms.sim_time * (3.4 + 1.6 * h2) + h2 * 6.2831);
            float ray2 = smoothstep(0.38, 0.0, f2) * smoothstep(0.0, 0.22, streak_t2) * smoothstep(0.92, 0.40, streak_t2);

            float wind_streak = saturate((ray1 * 0.65 + ray2 * 0.45) * periph_mask * max_wind);
            toned = saturate(toned + float3(0.88, 0.95, 1.0) * (wind_streak * 0.28));
        }
    }

    // Subtle vignette for crisp screen framing
    float vig_dist = length(D * float2(1.0, 0.85));
    toned *= (1.0 - smoothstep(0.45, 0.95, vig_dist) * 0.15);

    // 3. Reaction Time Cool Blue / Cyan Slow-Mo Tint
    if (uniforms.reaction_active > 0.5) {
        float luma = dot(toned, float3(0.299, 0.587, 0.114));
        float3 cool_grade = mix(float3(luma) * float3(0.70, 0.88, 1.08), toned, 0.65);
        toned = saturate(cool_grade);
    }

    // 4. Low-Health Red Vignette (pulsing border when health < 50)
    if (uniforms.health < 50.0) {
        float dmg = saturate((50.0 - uniforms.health) / 50.0);
        float dist = length(uv - 0.5) * 1.414;
        float vig = pow(saturate(dist - 0.4), 2.5) * dmg * (0.8 + 0.2 * sin(uniforms.sim_time * 8.0));
        toned = mix(toned, float3(0.85, 0.02, 0.02), vig);
    }

    return float4(toned, 1.0);
}

// -----------------------------------------------------------------------------
// 5. 2D Screen-Space HUD & Vector Font Overlay
// -----------------------------------------------------------------------------
struct HUDVertexIn {
    float2 position [[attribute(0)]];
    float4 color    [[attribute(1)]];
};

struct HUDVertexOut {
    float4 position [[position]];
    float4 color;
};

vertex HUDVertexOut hud_vertex(HUDVertexIn in [[stage_in]],
                              constant float2& screen_size [[buffer(1)]]) {
    HUDVertexOut out;
    out.position = float4((in.position.x / screen_size.x) * 2.0 - 1.0,
                          1.0 - (in.position.y / screen_size.y) * 2.0,
                          0.0, 1.0);
    out.color = in.color;
    return out;
}

fragment float4 hud_fragment(HUDVertexOut in [[stage_in]]) {
    return in.color;
}

// -----------------------------------------------------------------------------
// 6. Textured 2D UI Pipeline (TdUIScene / UI/TdUIResources.upk / TdMainMenu.me1)
// -----------------------------------------------------------------------------
struct UITexVertexIn {
    float2 position [[attribute(0)]];
    float2 uv       [[attribute(1)]];
    float4 color    [[attribute(2)]];
};

struct UITexVertexOut {
    float4 position [[position]];
    float2 uv;
    float4 color;
};

vertex UITexVertexOut ui_tex_vertex(UITexVertexIn in [[stage_in]],
                                    constant float2& screen_size [[buffer(1)]]) {
    UITexVertexOut out;
    out.position = float4((in.position.x / screen_size.x) * 2.0 - 1.0,
                          1.0 - (in.position.y / screen_size.y) * 2.0,
                          0.0, 1.0);
    out.uv = in.uv;
    out.color = in.color;
    return out;
}

fragment float4 ui_tex_fragment(UITexVertexOut in [[stage_in]],
                                texture2d<float> tex [[texture(0)]],
                                sampler samp [[sampler(0)]]) {
    float4 s = tex.sample(samp, in.uv);
    if (in.color.w < 0.0) {
        return float4(in.color.rgb, s.a * (-in.color.w));
    }
    return s * in.color;
}
)msl";

// -----------------------------------------------------------------------------
// Internal Uniforms Structure for Shader Binding
// -----------------------------------------------------------------------------
struct PackedFloat3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    constexpr PackedFloat3() = default;
    constexpr PackedFloat3(float in_x, float in_y, float in_z) : x(in_x), y(in_y), z(in_z) {}
    PackedFloat3(simd_float3 v) : x(v.x), y(v.y), z(v.z) {}
    operator simd_float3() const { return simd_make_float3(x, y, z); }
};

struct FrameUniformsGPU {
    simd_float4x4 view_proj;
    simd_float4x4 model;
    PackedFloat3 camera_pos;
    float sim_time;
    PackedFloat3 sun_dir;
    float runner_vision_strength;
    PackedFloat3 sun_color;
    float exposure;
    PackedFloat3 sky_color;
    float speed_2d;
    PackedFloat3 ground_color;
    float reaction_active;
    PackedFloat3 actor_tint;
    float is_runner_vision;
    PackedFloat3 cam_forward;
    float fov_tan;
    PackedFloat3 cam_right;
    float aspect;
    PackedFloat3 cam_up;
    float health;
    simd_float4x4 sun_view_proj;
    PackedFloat3 mod_shadow_color;
    float shadow_enabled;
    simd_float4x4 sun_view_proj_far;
};

struct HUDVertex {
    simd_float2 position;
    simd_float4 color;
};

struct UITexVertex {
    simd_float2 position;
    simd_float2 uv;
    simd_float4 color;
};

// -----------------------------------------------------------------------------
// Material System GPU Helpers (scene_materials.hpp -> Metal)
// -----------------------------------------------------------------------------
static bool tex_format_is_bc(TexFormat f) {
    return f == TexFormat::DXT1 || f == TexFormat::DXT3 || f == TexFormat::DXT5;
}

static MTLPixelFormat mtl_pixel_format(TexFormat f, bool srgb) {
    switch (f) {
        case TexFormat::DXT1: return srgb ? MTLPixelFormatBC1_RGBA_sRGB : MTLPixelFormatBC1_RGBA;
        case TexFormat::DXT3: return srgb ? MTLPixelFormatBC2_RGBA_sRGB : MTLPixelFormatBC2_RGBA;
        case TexFormat::DXT5: return srgb ? MTLPixelFormatBC3_RGBA_sRGB : MTLPixelFormatBC3_RGBA;
        case TexFormat::BGRA8: return srgb ? MTLPixelFormatBGRA8Unorm_sRGB : MTLPixelFormatBGRA8Unorm;
        case TexFormat::G8: return srgb ? MTLPixelFormatR8Unorm_sRGB : MTLPixelFormatR8Unorm;
        case TexFormat::V8U8: return MTLPixelFormatRG8Snorm;
        default: return MTLPixelFormatInvalid;
    }
}

// Bytes per row of 4x4 blocks (BC) or pixels (uncompressed) and number of such rows.
static size_t tex_row_bytes(TexFormat f, int w) {
    switch (f) {
        case TexFormat::DXT1: return static_cast<size_t>(std::max(1, (w + 3) / 4)) * 8;
        case TexFormat::DXT3:
        case TexFormat::DXT5: return static_cast<size_t>(std::max(1, (w + 3) / 4)) * 16;
        case TexFormat::BGRA8: return static_cast<size_t>(w) * 4;
        case TexFormat::V8U8: return static_cast<size_t>(w) * 2;
        default: return static_cast<size_t>(w);
    }
}
static size_t tex_rows(TexFormat f, int h) {
    return tex_format_is_bc(f) ? static_cast<size_t>(std::max(1, (h + 3) / 4)) : static_cast<size_t>(h);
}

// Number of leading mips forming a valid Metal mip chain (exact halving, enough data).
static int tex_valid_chain(TexFormat f, const std::vector<TextureMip>& mips) {
    if (mips.empty() || mips[0].width <= 0 || mips[0].height <= 0) return 0;
    int n = 0;
    for (size_t i = 0; i < mips.size(); ++i) {
        const int w = std::max(1, mips[0].width >> i);
        const int h = std::max(1, mips[0].height >> i);
        if (mips[i].width != w || mips[i].height != h) break;
        if (mips[i].data.size() < tex_row_bytes(f, w) * tex_rows(f, h)) break;
        ++n;
    }
    return n;
}

static MTLSamplerAddressMode mtl_address_mode(TexAddress a) {
    switch (a) {
        case TexAddress::Clamp: return MTLSamplerAddressModeClampToEdge;
        case TexAddress::Mirror: return MTLSamplerAddressModeMirrorRepeat;
        default: return MTLSamplerAddressModeRepeat;
    }
}

static bool mat_blend_is_translucent(MatBlendMode b) {
    return b == MatBlendMode::Translucent || b == MatBlendMode::Additive || b == MatBlendMode::Modulate;
}

// World-space side and near planes of a view-projection matrix's clip volume (Metal clips to -w <= x <= w,
// -w <= y <= w and 0 <= z <= w; no pipeline here changes the depth clip mode). A sphere wholly outside one of
// these planes rasterizes nothing in that pass: every triangle inside it is clipped away completely, so not
// drawing it leaves the render target unchanged. The far plane is left out because its float test is poorly
// conditioned (w - z cancels), so the GPU's rounding there cannot be bounded tightly.
struct ClipVolume {
    explicit ClipVolume(const Mat4& view_proj) {
        // Row r of the column-major matrix: clip[r] = sum over c of m[c * 4 + r] * (x, y, z, 1)[c].
        auto m = [&](int r, int c) { return static_cast<double>(view_proj.m[c * 4 + r]); };
        for (int c = 0; c < 4; ++c) {
            planes[0][c] = m(3, c) + m(0, c);  // -w <= x
            planes[1][c] = m(3, c) - m(0, c);  //  x <= w
            planes[2][c] = m(3, c) + m(1, c);  // -w <= y
            planes[3][c] = m(3, c) - m(1, c);  //  y <= w
            planes[4][c] = m(2, c);            //  0 <= z
        }
        for (auto& p : planes) {
            const double len = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
            if (len > 0.0) {
                for (double& v : p) v /= len;
            }
        }
    }

    // False only if the sphere lies outside some plane by more than `margin` UU (so a NaN centre or radius is
    // never rejected).
    [[nodiscard]] bool may_cover(const Vec3& center, float radius, double margin) const {
        for (const auto& p : planes) {
            const double dist = p[0] * center.x + p[1] * center.y + p[2] * center.z + p[3];
            if (dist < -(static_cast<double>(radius) + margin)) return false;
        }
        return true;
    }

    double planes[5][4];
};

// -----------------------------------------------------------------------------
// PIMPL Implementation
// -----------------------------------------------------------------------------
struct MetalRenderer::Impl {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> command_queue = nil;
    id<MTLLibrary> shader_library = nil;

    // Render Pipeline States
    id<MTLRenderPipelineState> shadow_pipeline = nil;
    id<MTLRenderPipelineState> sky_pipeline = nil;
    id<MTLRenderPipelineState> world_pipeline = nil;
    id<MTLRenderPipelineState> viewmodel_pipeline = nil;
    id<MTLRenderPipelineState> post_pipeline = nil;
    id<MTLRenderPipelineState> hud_pipeline = nil;
    id<MTLRenderPipelineState> ui_tex_pipeline = nil;

    // Depth Stencil States
    id<MTLDepthStencilState> depth_write_state = nil;
    id<MTLDepthStencilState> depth_test_only_state = nil;
    id<MTLDepthStencilState> depth_disabled_state = nil;

    // Texture Sampler
    id<MTLSamplerState> linear_sampler = nil;

    // Framebuffer targets
    CAMetalLayer* metal_layer = nil;
    id<MTLTexture> offscreen_color_tex = nil;
    id<MTLTexture> offscreen_depth_tex = nil;
    id<MTLTexture> scene_hdr_tex = nil; // Intermediate HDR buffer for tone mapping
    id<MTLTexture> shadow_depth_tex = nil; // sun shadow cascades: 2-slice 4096x4096 depth array (renderer/sun_shadow.hpp)
    // The far cascade slice only holds static level geometry and is redrawn only when its (coarsely
    // snapped) matrix changes or the scene's vertex buffers are rebuilt.
    bool shadow_far_valid = false;
    float shadow_far_vp[16] = {};
    uint64_t scene_generation = 0;       // bumped whenever cached_mesh_buffers is rebuilt
    uint64_t shadow_far_generation = 0;  // scene_generation the far slice was rendered from

    int width = 1280;
    int height = 720;
    bool headless = true;
    bool initialized = false;
    uint64_t frame_index = 0;

    // Menu state & Frontend UI System (TdMainMenu.me1 + UI/TdUI_FrontEnd.upk)
    bool menu_open = false;
    int selected_chapter = 1;
    int selected_menu_tab = 0;
    int selected_menu_row = 0;
    int opt_sens_pct = 100;
    int opt_fov_deg = 90;
    bool opt_fullscreen = false;
    MainMenuSystem main_menu;
    bool main_menu_gpu_ready = false;
    id<MTLTexture> ui_logo_tex = nil;
    id<MTLTexture> ui_bag_tex = nil;
    id<MTLTexture> ui_time_tex = nil;
    id<MTLTexture> ui_panel_bg_tex = nil;
    id<MTLTexture> ui_faith_art_tex = nil;
    id<MTLTexture> ui_chapter_tex[10] = {nil};
    std::vector<id<MTLTexture>> ui_font_headline_thick_tex;
    std::vector<id<MTLTexture>> ui_font_headline_light_tex;
    std::vector<id<MTLTexture>> ui_font_medium_italic_tex;
    std::vector<id<MTLTexture>> ui_font_small_italic_tex;

    struct UITextureBatch {
        id<MTLTexture> tex = nil;
        std::vector<UITexVertex> verts;
    };

    // First-Person Faith Viewmodel Mesh & 3D Enemy Guard Mesh
    std::vector<Vertex> faith_viewmodel_mesh;
    std::vector<Vertex> enemy_guard_mesh;
    // This frame's posed enemies (index = enemy index). Each one's triangle list (exactly evaluate_enemy_swat()'s)
    // is written to enemy_vertex_buffers[slot][i] only when the enemy can reach a pass's render target.
    struct EnemyFrameDraw {
        size_t corner_count = 0;  // triangle-list vertices (0 = nothing to draw)
        bool in_view = false;     // may cover pixels of the camera view (world pass)
        bool in_shadow = false;   // may cover texels of the near shadow cascade (shadow pass)
    };
    std::vector<EnemyFrameDraw> frame_enemy_draws;
    AnimSystem anim_system;

    // Triple-buffered CPU/GPU frame synchronization & reusable dynamic vertex buffers
    static constexpr int kMaxFramesInFlight = 3;
    dispatch_semaphore_t in_flight_sem = dispatch_semaphore_create(kMaxFramesInFlight);
    id<MTLCommandBuffer> last_cmd_buffer = nil;
    std::vector<id<MTLBuffer>> dyn_vertex_buffers[kMaxFramesInFlight];
    size_t dyn_vertex_cursor[kMaxFramesInFlight] = {0, 0, 0};
    // Per-frame-slot GPU vertex buffers for the posed enemies (index = enemy index): each worker writes its enemy's
    // triangle list straight into this memory, and the shadow and world passes draw from the same buffer.
    std::vector<id<MTLBuffer>> enemy_vertex_buffers[kMaxFramesInFlight];

    // Vertex data up to this size is passed inline with setVertexBytes instead of through a buffer.
    static constexpr size_t kMaxInlineVertexBytes = 4096;

    static size_t dynamic_buffer_capacity(size_t length) {
        return std::max<size_t>((length + 4095u) & ~size_t(4095u), 65536u);
    }

    id<MTLBuffer> acquire_dynamic_vertex_buffer(int slot, const void* data, size_t length) {
        auto& pool = dyn_vertex_buffers[slot];
        size_t idx = dyn_vertex_cursor[slot]++;
        if (idx >= pool.size()) {
            pool.push_back(nil);
        }
        id<MTLBuffer> buf = pool[idx];
        if (!buf || buf.length < length) {
            buf = [device newBufferWithLength:dynamic_buffer_capacity(length) options:MTLResourceStorageModeShared];
            pool[idx] = buf;
        }
        std::memcpy(buf.contents, data, length);
        return buf;
    }

    ~Impl() {
        if (last_cmd_buffer) {
            [last_cmd_buffer waitUntilCompleted];
            last_cmd_buffer = nil;
        }
    }

    // Cutscene Bink Video & Matinee Renderer State
    const CutscenePlayer* cutscene_player = nullptr;
    id<MTLTexture> bink_video_tex = nil;
    uint64_t bink_uploaded_serial = 0;

    // Cached GPU Vertex Buffers & Per-Section Material/Shadow Metadata for Scene Meshes
    std::string cached_map_name;
    size_t cached_total_verts = 0;
    std::vector<id<MTLBuffer>> cached_mesh_buffers;
    std::shared_ptr<const SceneMaterialLibrary> cached_section_mat_lib;
    bool cached_has_translucent = false;
    bool cached_needs_scene_copies = false;
    static constexpr uint8_t kSecShadowCaster = 1u << 0;
    static constexpr uint8_t kSecTranslucent  = 1u << 1;
    std::vector<std::vector<uint8_t>> cached_section_flags;

    // -------------------------------------------------------------------------
    // Mirror's Edge material system (LevelScene::materials) GPU cache
    // -------------------------------------------------------------------------
    std::shared_ptr<const SceneMaterialLibrary> mat_lib;     // library currently resident on the GPU
    std::vector<id<MTLTexture>> mat_textures;                // per SceneTexture (nil = missing -> default)
    std::vector<id<MTLRenderPipelineState>> mat_pipelines;   // per MaterialShader (nil = failed -> legacy)
    id<MTLTexture> tex_default_white = nil;
    id<MTLTexture> tex_default_flat_normal = nil;
    id<MTLTexture> tex_default_black = nil;
    id<MTLTexture> tex_default_cube = nil;
    id<MTLSamplerState> mat_samplers[3][3];                  // [TexAddress X][TexAddress Y]
    id<MTLSamplerState> mat_cube_sampler = nil;
    id<MTLSamplerState> scene_copy_sampler = nil;
    id<MTLTexture> scene_color_copy = nil;                   // opaque scene color (SceneTexture / DestColor)
    id<MTLTexture> scene_depth_copy = nil;                   // opaque scene depth (SceneDepth / DepthBiased*)

    // UE3 culls back faces of non-two-sided materials. ME_CULL=off|cw|ccw overrides (debugging).
    // With this renderer's view/projection, UE3 front faces are counter-clockwise on screen
    // (verified: CCW culling only removes hidden back faces, CW culling removes ~16-58% of the image).
    bool mat_cull_enabled = true;
    MTLWinding mat_front_winding = MTLWindingCounterClockwise;

    void configure_material_culling() {
        if (const char* env = std::getenv("ME_CULL")) {
            const std::string v = env;
            if (v == "0" || v == "off" || v == "none") {
                mat_cull_enabled = false;
            } else if (v == "ccw") {
                mat_front_winding = MTLWindingCounterClockwise;
            } else if (v == "cw") {
                mat_front_winding = MTLWindingClockwise;
            }
        }
    }

    MTLCompileOptions* make_compile_options() {
        MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
        if (@available(macOS 15.0, *)) {
            options.mathMode = MTLMathModeFast;
        }
        if (@available(macOS 11.0, *)) {
            options.languageVersion = MTLLanguageVersion2_4;
        }
        return options;
    }

    id<MTLTexture> make_solid_texture(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                     width:4
                                                                                    height:4
                                                                                 mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead;
        d.storageMode = MTLStorageModeShared;
        id<MTLTexture> t = [device newTextureWithDescriptor:d];
        uint8_t px[4 * 4 * 4];
        for (int i = 0; i < 16; ++i) {
            px[i * 4 + 0] = r;
            px[i * 4 + 1] = g;
            px[i * 4 + 2] = b;
            px[i * 4 + 3] = a;
        }
        [t replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0 withBytes:px bytesPerRow:16];
        return t;
    }

    void create_material_defaults() {
        configure_material_culling();
        tex_default_white = make_solid_texture(255, 255, 255, 255);
        tex_default_flat_normal = make_solid_texture(128, 128, 255, 255);
        tex_default_black = make_solid_texture(0, 0, 0, 255);

        constexpr int kCubeDim = 64;
        MTLTextureDescriptor* cd = [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB
                                                                                         size:kCubeDim
                                                                                    mipmapped:YES];
        cd.usage = MTLTextureUsageShaderRead;
        cd.storageMode = MTLStorageModeShared;
        tex_default_cube = [device newTextureWithDescriptor:cd];
        for (NSUInteger face = 0; face < 6; ++face) {
            int dim = kCubeDim;
            NSUInteger mip = 0;
            for (;;) {
                std::vector<uint8_t> px(static_cast<size_t>(dim) * static_cast<size_t>(dim) * 4);
                for (int y = 0; y < dim; ++y) {
                    const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(dim);
                    for (int x = 0; x < dim; ++x) {
                        const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(dim);
                        const float sc = 2.0f * u - 1.0f;
                        const float tc = 2.0f * v - 1.0f;
                        float dx = 0.0f, dy = 0.0f, dz = 1.0f;
                        switch (face) {
                            case 0: dx =  1.0f; dy = -tc;   dz = -sc;   break;
                            case 1: dx = -1.0f; dy = -tc;   dz =  sc;   break;
                            case 2: dx =  sc;   dy =  1.0f; dz =  tc;   break;
                            case 3: dx =  sc;   dy = -1.0f; dz = -tc;   break;
                            case 4: dx =  sc;   dy = -tc;   dz =  1.0f; break;
                            default:dx = -sc;   dy = -tc;   dz = -1.0f; break;
                        }
                        const float len = std::sqrt(std::max(dx * dx + dy * dy + dz * dz, 1e-12f));
                        dx /= len; dy /= len; dz /= len;
                        float r = 0.0f, g = 0.0f, b = 0.0f;
                        if (dz >= 0.0f) {
                            const float t = std::pow(1.0f - dz, 2.2f);
                            r = 54.0f * (1.0f - t) + 224.0f * t;
                            g = 126.0f * (1.0f - t) + 238.0f * t;
                            b = 228.0f * (1.0f - t) + 254.0f * t;
                        } else {
                            const float t = std::min(1.0f, -dz * 2.5f);
                            r = 224.0f * (1.0f - t) + 148.0f * t;
                            g = 238.0f * (1.0f - t) + 168.0f * t;
                            b = 254.0f * (1.0f - t) + 196.0f * t;
                        }
                        size_t idx = (static_cast<size_t>(y) * static_cast<size_t>(dim) + static_cast<size_t>(x)) * 4;
                        px[idx + 0] = static_cast<uint8_t>(std::clamp(r, 0.0f, 255.0f));
                        px[idx + 1] = static_cast<uint8_t>(std::clamp(g, 0.0f, 255.0f));
                        px[idx + 2] = static_cast<uint8_t>(std::clamp(b, 0.0f, 255.0f));
                        px[idx + 3] = 255;
                    }
                }
                [tex_default_cube replaceRegion:MTLRegionMake2D(0, 0, static_cast<NSUInteger>(dim), static_cast<NSUInteger>(dim))
                                    mipmapLevel:mip
                                          slice:face
                                      withBytes:px.data()
                                    bytesPerRow:static_cast<NSUInteger>(dim) * 4
                                  bytesPerImage:px.size()];
                if (dim == 1) break;
                dim = std::max(1, dim / 2);
                mip++;
            }
        }

        // UE3 samples each texture with its own AddressX/AddressY and trilinear/anisotropic filtering.
        for (int x = 0; x < 3; ++x) {
            for (int y = 0; y < 3; ++y) {
                MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
                sd.minFilter = MTLSamplerMinMagFilterLinear;
                sd.magFilter = MTLSamplerMinMagFilterLinear;
                sd.mipFilter = MTLSamplerMipFilterLinear;
                sd.maxAnisotropy = 8;
                sd.sAddressMode = mtl_address_mode(static_cast<TexAddress>(x));
                sd.tAddressMode = mtl_address_mode(static_cast<TexAddress>(y));
                sd.rAddressMode = MTLSamplerAddressModeRepeat;
                mat_samplers[x][y] = [device newSamplerStateWithDescriptor:sd];
            }
        }
        MTLSamplerDescriptor* cs = [[MTLSamplerDescriptor alloc] init];
        cs.minFilter = MTLSamplerMinMagFilterLinear;
        cs.magFilter = MTLSamplerMinMagFilterLinear;
        cs.mipFilter = MTLSamplerMipFilterLinear;
        cs.sAddressMode = MTLSamplerAddressModeClampToEdge;
        cs.tAddressMode = MTLSamplerAddressModeClampToEdge;
        cs.rAddressMode = MTLSamplerAddressModeClampToEdge;
        mat_cube_sampler = [device newSamplerStateWithDescriptor:cs];

        MTLSamplerDescriptor* ss = [[MTLSamplerDescriptor alloc] init];
        ss.minFilter = MTLSamplerMinMagFilterLinear;
        ss.magFilter = MTLSamplerMinMagFilterLinear;
        ss.sAddressMode = MTLSamplerAddressModeClampToEdge;
        ss.tAddressMode = MTLSamplerAddressModeClampToEdge;
        scene_copy_sampler = [device newSamplerStateWithDescriptor:ss];
    }

    // Uploads one decoded UE3 texture (2D or cube, full mip chain) as a shared Metal texture.
    id<MTLTexture> upload_scene_texture(const SceneTexture& st) {
        if (!st.valid()) return nil;
        const MTLPixelFormat fmt = mtl_pixel_format(st.format, st.srgb);
        if (fmt == MTLPixelFormatInvalid) return nil;

        auto apply_swizzle = [&](MTLTextureDescriptor* d) {
            if (@available(macOS 10.15, *)) {
                if (st.format == TexFormat::G8) {
                    // D3DFMT_L8 samples as (L, L, L, 1)
                    d.swizzle = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleRed,
                                                              MTLTextureSwizzleRed, MTLTextureSwizzleOne);
                } else if (st.format == TexFormat::V8U8) {
                    // D3DFMT_V8U8 samples as (U, V, 1, 1)
                    d.swizzle = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleGreen,
                                                              MTLTextureSwizzleOne, MTLTextureSwizzleOne);
                }
            }
        };

        if (!st.is_cube) {
            const int levels = tex_valid_chain(st.format, st.mips);
            if (levels <= 0) return nil;
            MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt
                                                                                         width:st.mips[0].width
                                                                                        height:st.mips[0].height
                                                                                     mipmapped:(levels > 1)];
            d.mipmapLevelCount = levels;
            d.usage = MTLTextureUsageShaderRead;
            d.storageMode = MTLStorageModeShared;
            apply_swizzle(d);
            id<MTLTexture> t = [device newTextureWithDescriptor:d];
            if (!t) return nil;
            for (int i = 0; i < levels; ++i) {
                const TextureMip& m = st.mips[i];
                [t replaceRegion:MTLRegionMake2D(0, 0, m.width, m.height)
                     mipmapLevel:i
                       withBytes:m.data.data()
                     bytesPerRow:tex_row_bytes(st.format, m.width)];
            }
            return t;
        }

        int levels = INT_MAX;
        for (const auto& face : st.faces) {
            levels = std::min(levels, tex_valid_chain(st.format, face));
            if (face.empty() || face[0].width != st.faces[0][0].width || face[0].height != face[0].width) return nil;
        }
        if (levels <= 0 || levels == INT_MAX) return nil;
        MTLTextureDescriptor* d = [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:fmt
                                                                                       size:st.faces[0][0].width
                                                                                  mipmapped:(levels > 1)];
        d.mipmapLevelCount = levels;
        d.usage = MTLTextureUsageShaderRead;
        d.storageMode = MTLStorageModeShared;
        apply_swizzle(d);
        id<MTLTexture> t = [device newTextureWithDescriptor:d];
        if (!t) return nil;
        for (NSUInteger f = 0; f < 6; ++f) {
            for (int i = 0; i < levels; ++i) {
                const TextureMip& m = st.faces[f][i];
                const size_t row = tex_row_bytes(st.format, m.width);
                [t replaceRegion:MTLRegionMake2D(0, 0, m.width, m.height)
                     mipmapLevel:i
                           slice:f
                       withBytes:m.data.data()
                     bytesPerRow:row
                   bytesPerImage:row * tex_rows(st.format, m.height)];
            }
        }
        return t;
    }

    // Compiles every generated material fragment shader (grouped per library, in parallel) and
    // builds one render pipeline per shader. Failing shaders fall back to the legacy pipeline.
    void compile_material_shaders(const SceneMaterialLibrary& lib) {
        const size_t n = lib.shaders.size();
        mat_pipelines.assign(n, nil);
        if (n == 0) return;

        MTLCompileOptions* options = make_compile_options();
        const size_t kGroupSize = 16;
        const size_t num_groups = (n + kGroupSize - 1) / kGroupSize;
        std::vector<id<MTLLibrary>> shader_lib(n, nil);
        std::vector<id<MTLRenderPipelineState>> pipelines(n, nil);
        std::mutex err_mutex;
        std::vector<std::string> errors;

        auto compile_source = [&](const std::string& body, std::string* err_out) -> id<MTLLibrary> {
            @autoreleasepool {
                std::string full = lib.common_source;
                full += "\n";
                full += body;
                NSError* err = nil;
                NSString* src = [NSString stringWithUTF8String:full.c_str()];
                id<MTLLibrary> L = src ? [device newLibraryWithSource:src options:options error:&err] : nil;
                if (!L && err_out) {
                    *err_out = err ? [[err localizedDescription] UTF8String] : "invalid UTF-8 source";
                }
                return L;
            }
        };

        auto build_pipeline = [&](size_t i) -> id<MTLRenderPipelineState> {
            @autoreleasepool {
                id<MTLLibrary> L = shader_lib[i];
                if (!L) return nil;
                const MaterialShader& sh = lib.shaders[i];
                id<MTLFunction> vf = [L newFunctionWithName:[NSString stringWithUTF8String:lib.vertex_function.c_str()]];
                id<MTLFunction> ff = [L newFunctionWithName:[NSString stringWithUTF8String:sh.function_name.c_str()]];
                if (!vf || !ff) return nil;
                MTLRenderPipelineDescriptor* d = [[MTLRenderPipelineDescriptor alloc] init];
                d.vertexFunction = vf;
                d.fragmentFunction = ff;
                d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
                d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
                if (mat_blend_is_translucent(sh.blend)) {
                    auto* ca = d.colorAttachments[0];
                    ca.blendingEnabled = YES;
                    ca.rgbBlendOperation = MTLBlendOperationAdd;
                    ca.alphaBlendOperation = MTLBlendOperationAdd;
                    ca.sourceAlphaBlendFactor = MTLBlendFactorZero;
                    ca.destinationAlphaBlendFactor = MTLBlendFactorOne;
                    if (sh.blend == MatBlendMode::Translucent) {
                        ca.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
                        ca.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                    } else if (sh.blend == MatBlendMode::Additive) {
                        ca.sourceRGBBlendFactor = MTLBlendFactorOne;
                        ca.destinationRGBBlendFactor = MTLBlendFactorOne;
                    } else {  // Modulate
                        ca.sourceRGBBlendFactor = MTLBlendFactorDestinationColor;
                        ca.destinationRGBBlendFactor = MTLBlendFactorZero;
                    }
                }
                NSError* err = nil;
                id<MTLRenderPipelineState> ps = [device newRenderPipelineStateWithDescriptor:d error:&err];
                if (!ps) {
                    std::lock_guard<std::mutex> lock(err_mutex);
                    errors.push_back(sh.function_name + " (" + sh.base_material + ") pipeline: " +
                                     (err ? [[err localizedDescription] UTF8String] : "unknown error"));
                }
                return ps;
            }
        };

        std::atomic<size_t> next_group{0};
        auto worker = [&]() {
            for (;;) {
                const size_t g = next_group.fetch_add(1);
                if (g >= num_groups) return;
                const size_t first = g * kGroupSize;
                const size_t last = std::min(n, first + kGroupSize);
                std::string body;
                for (size_t i = first; i < last; ++i) {
                    body += lib.shaders[i].source;
                    body += "\n";
                }
                id<MTLLibrary> L = compile_source(body, nullptr);
                if (L) {
                    for (size_t i = first; i < last; ++i) shader_lib[i] = L;
                } else {
                    // Isolate the failing shader(s) of this group.
                    for (size_t i = first; i < last; ++i) {
                        std::string err;
                        shader_lib[i] = compile_source(lib.shaders[i].source, &err);
                        if (!shader_lib[i]) {
                            std::lock_guard<std::mutex> lock(err_mutex);
                            errors.push_back(lib.shaders[i].function_name + " (" + lib.shaders[i].base_material +
                                             "): " + err);
                        }
                    }
                }
                for (size_t i = first; i < last; ++i) pipelines[i] = build_pipeline(i);
            }
        };
        const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
        const unsigned num_threads = static_cast<unsigned>(std::min<size_t>(num_groups, hw));
        std::vector<std::thread> pool;
        pool.reserve(num_threads);
        for (unsigned t = 0; t < num_threads; ++t) pool.emplace_back(worker);
        for (auto& th : pool) th.join();

        size_t ok = 0;
        for (size_t i = 0; i < n; ++i) {
            mat_pipelines[i] = pipelines[i];
            if (pipelines[i]) ok++;
        }
        std::cout << "[MetalRenderer] Material shaders: " << ok << "/" << n << " compiled" << std::endl;
        for (size_t e = 0; e < errors.size() && e < 4; ++e) {
            std::string msg = errors[e];
            if (msg.size() > 900) msg = msg.substr(0, 900) + " ...";
            std::cerr << "[MetalRenderer]   shader error: " << msg << std::endl;
        }
    }

    // Makes `lib` the resident material library (uploads textures, compiles shaders) if it changed.
    void sync_material_library(const std::shared_ptr<const SceneMaterialLibrary>& lib) {
        if (lib == mat_lib) return;
        mat_lib = lib;
        mat_textures.clear();
        mat_pipelines.clear();
        if (!lib) return;

        const auto t0 = std::chrono::steady_clock::now();
        const size_t n_tex = lib->textures.size();
        mat_textures.assign(n_tex, nil);
        std::atomic<size_t> next_tex{0};
        std::atomic<size_t> uploaded{0};
        const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
        const unsigned tex_threads = static_cast<unsigned>(std::min<size_t>(std::max<size_t>(1, n_tex), hw));
        auto tex_worker = [&]() {
            while (true) {
                const size_t i = next_tex.fetch_add(1, std::memory_order_relaxed);
                if (i >= n_tex) break;
                @autoreleasepool {
                    id<MTLTexture> tex = upload_scene_texture(lib->textures[i]);
                    mat_textures[i] = tex;
                    if (tex) uploaded.fetch_add(1, std::memory_order_relaxed);
                }
            }
        };
        if (n_tex > 0) {
            std::vector<std::thread> tex_pool;
            tex_pool.reserve(tex_threads);
            for (unsigned t = 0; t < tex_threads; ++t) tex_pool.emplace_back(tex_worker);
            for (auto& th : tex_pool) th.join();
        }
        compile_material_shaders(*lib);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::cout << "[MetalRenderer] Material library resident: " << lib->materials.size() << " materials, "
                  << uploaded.load() << "/" << n_tex << " textures uploaded in " << secs << " s" << std::endl;
    }

    // Returns the pipeline for a mesh section, or nil when it must use legacy procedural shading.
    id<MTLRenderPipelineState> section_pipeline(const MeshSection& s, const MaterialShader** out_shader,
                                                const SceneMaterial** out_material) const {
        if (!mat_lib || s.material < 0 || static_cast<size_t>(s.material) >= mat_lib->materials.size()) return nil;
        const SceneMaterial& m = mat_lib->materials[static_cast<size_t>(s.material)];
        if (m.shader < 0 || static_cast<size_t>(m.shader) >= mat_pipelines.size()) return nil;
        id<MTLRenderPipelineState> ps = mat_pipelines[static_cast<size_t>(m.shader)];
        if (!ps) return nil;
        if (out_shader) *out_shader = &mat_lib->shaders[static_cast<size_t>(m.shader)];
        if (out_material) *out_material = &m;
        return ps;
    }

    // Binds a material instance's textures, samplers and parameter uniforms.
    void bind_material(id<MTLRenderCommandEncoder> enc, const SceneMaterial& m, const MaterialShader& sh) {
        for (int k = 0; k < sh.num_tex2d; ++k) {
            const int ti = (static_cast<size_t>(k) < m.tex2d.size()) ? m.tex2d[static_cast<size_t>(k)] : -1;
            id<MTLTexture> t = (ti >= 0 && static_cast<size_t>(ti) < mat_textures.size()) ? mat_textures[static_cast<size_t>(ti)] : nil;
            TexAddress ax = TexAddress::Wrap;
            TexAddress ay = TexAddress::Wrap;
            if (t) {
                ax = mat_lib->textures[static_cast<size_t>(ti)].address_x;
                ay = mat_lib->textures[static_cast<size_t>(ti)].address_y;
            } else {
                const TexDefault def = (static_cast<size_t>(k) < m.tex2d_default.size()) ? m.tex2d_default[static_cast<size_t>(k)]
                                                                                       : TexDefault::White;
                t = (def == TexDefault::FlatNormal) ? tex_default_flat_normal
                    : (def == TexDefault::Black)    ? tex_default_black
                                                    : tex_default_white;
            }
            [enc setFragmentTexture:t atIndex:static_cast<NSUInteger>(k)];
            [enc setFragmentSamplerState:mat_samplers[static_cast<int>(ax) % 3][static_cast<int>(ay) % 3]
                                 atIndex:static_cast<NSUInteger>(k)];
        }
        for (int j = 0; j < sh.num_texcube; ++j) {
            const int ti = (static_cast<size_t>(j) < m.texcube.size()) ? m.texcube[static_cast<size_t>(j)] : -1;
            id<MTLTexture> t = (ti >= 0 && static_cast<size_t>(ti) < mat_textures.size()) ? mat_textures[static_cast<size_t>(ti)] : nil;
            if (!t) t = tex_default_cube;
            [enc setFragmentTexture:t atIndex:static_cast<NSUInteger>(sh.num_tex2d + j)];
            [enc setFragmentSamplerState:mat_cube_sampler atIndex:static_cast<NSUInteger>(sh.num_tex2d + j)];
        }
        if (sh.num_uniforms > 0) {
            float zero_pad[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            std::vector<std::array<float, 4>> padded;
            const void* data = m.uniforms.data();
            if (m.uniforms.size() < static_cast<size_t>(sh.num_uniforms)) {
                padded = m.uniforms;
                padded.resize(static_cast<size_t>(sh.num_uniforms), {zero_pad[0], zero_pad[1], zero_pad[2], zero_pad[3]});
                data = padded.data();
            }
            [enc setFragmentBytes:data length:static_cast<NSUInteger>(sh.num_uniforms) * 16 atIndex:matbind::kMaterialBuffer];
        }
        if (sh.uses_scene_color || sh.uses_scene_depth) {
            [enc setFragmentTexture:scene_color_copy atIndex:matbind::kSceneColorTexture];
            [enc setFragmentTexture:scene_depth_copy atIndex:matbind::kSceneDepthTexture];
            [enc setFragmentSamplerState:scene_copy_sampler atIndex:matbind::kSceneSampler];
        }
        [enc setFragmentTexture:shadow_depth_tex atIndex:matbind::kShadowMapTexture];
    }

    bool compile_shaders() {
        NSError* error = nil;
        // The sun shadow lookup is shared with the generated material shaders (renderer/sun_shadow.hpp).
        const std::string full_source = sun_shadow_msl() + MSL_SHADERS;
        NSString* source = [NSString stringWithUTF8String:full_source.c_str()];
        MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
        if (@available(macOS 15.0, *)) {
            options.mathMode = MTLMathModeFast;
        }
        if (@available(macOS 11.0, *)) {
            options.languageVersion = MTLLanguageVersion2_4;
        }

        shader_library = [device newLibraryWithSource:source options:options error:&error];
        if (!shader_library) {
            std::cerr << "[MetalRenderer] Shader compilation failed: "
                      << [[error localizedDescription] UTF8String] << std::endl;
            return false;
        }

        // 0. Shadow Map Depth Pipeline
        id<MTLFunction> shadowVert = [shader_library newFunctionWithName:@"shadow_vertex"];
        MTLRenderPipelineDescriptor* shadowDesc = [[MTLRenderPipelineDescriptor alloc] init];
        shadowDesc.vertexFunction = shadowVert;
        shadowDesc.fragmentFunction = nil;
        shadowDesc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        shadow_pipeline = [device newRenderPipelineStateWithDescriptor:shadowDesc error:&error];
        if (!shadow_pipeline) return false;

        // 1. Sky Pipeline
        id<MTLFunction> skyVert = [shader_library newFunctionWithName:@"sky_vertex"];
        id<MTLFunction> skyFrag = [shader_library newFunctionWithName:@"sky_fragment"];
        MTLRenderPipelineDescriptor* skyDesc = [[MTLRenderPipelineDescriptor alloc] init];
        skyDesc.vertexFunction = skyVert;
        skyDesc.fragmentFunction = skyFrag;
        skyDesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
        skyDesc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        sky_pipeline = [device newRenderPipelineStateWithDescriptor:skyDesc error:&error];
        if (!sky_pipeline) return false;

        // 2. World Pipeline
        id<MTLFunction> worldVert = [shader_library newFunctionWithName:@"world_vertex"];
        id<MTLFunction> worldFrag = [shader_library newFunctionWithName:@"world_fragment"];
        MTLRenderPipelineDescriptor* worldDesc = [[MTLRenderPipelineDescriptor alloc] init];
        worldDesc.vertexFunction = worldVert;
        worldDesc.fragmentFunction = worldFrag;
        worldDesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
        worldDesc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        world_pipeline = [device newRenderPipelineStateWithDescriptor:worldDesc error:&error];
        if (!world_pipeline) return false;

        // 3. Viewmodel Pipeline
        id<MTLFunction> vmVert = [shader_library newFunctionWithName:@"viewmodel_vertex"];
        id<MTLFunction> vmFrag = [shader_library newFunctionWithName:@"viewmodel_fragment"];
        MTLRenderPipelineDescriptor* vmDesc = [[MTLRenderPipelineDescriptor alloc] init];
        vmDesc.vertexFunction = vmVert;
        vmDesc.fragmentFunction = vmFrag;
        vmDesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
        vmDesc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        viewmodel_pipeline = [device newRenderPipelineStateWithDescriptor:vmDesc error:&error];
        if (!viewmodel_pipeline) return false;

        // 4. Post-Processing Pipeline
        id<MTLFunction> postVert = [shader_library newFunctionWithName:@"post_vertex"];
        id<MTLFunction> postFrag = [shader_library newFunctionWithName:@"post_fragment"];
        MTLRenderPipelineDescriptor* postDesc = [[MTLRenderPipelineDescriptor alloc] init];
        postDesc.vertexFunction = postVert;
        postDesc.fragmentFunction = postFrag;
        postDesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        post_pipeline = [device newRenderPipelineStateWithDescriptor:postDesc error:&error];
        if (!post_pipeline) return false;

        // 5. 2D HUD Pipeline
        id<MTLFunction> hudVert = [shader_library newFunctionWithName:@"hud_vertex"];
        id<MTLFunction> hudFrag = [shader_library newFunctionWithName:@"hud_fragment"];

        MTLVertexDescriptor* hudVertexDesc = [MTLVertexDescriptor vertexDescriptor];
        hudVertexDesc.attributes[0].format = MTLVertexFormatFloat2;
        hudVertexDesc.attributes[0].offset = offsetof(HUDVertex, position);
        hudVertexDesc.attributes[0].bufferIndex = 0;
        hudVertexDesc.attributes[1].format = MTLVertexFormatFloat4;
        hudVertexDesc.attributes[1].offset = offsetof(HUDVertex, color);
        hudVertexDesc.attributes[1].bufferIndex = 0;
        hudVertexDesc.layouts[0].stride = sizeof(HUDVertex);
        hudVertexDesc.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

        MTLRenderPipelineDescriptor* hudDesc = [[MTLRenderPipelineDescriptor alloc] init];
        hudDesc.vertexFunction = hudVert;
        hudDesc.fragmentFunction = hudFrag;
        hudDesc.vertexDescriptor = hudVertexDesc;
        hudDesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;

        // Alpha blending for UI overlay
        hudDesc.colorAttachments[0].blendingEnabled = YES;
        hudDesc.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
        hudDesc.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
        hudDesc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        hudDesc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha;
        hudDesc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        hudDesc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;

        hud_pipeline = [device newRenderPipelineStateWithDescriptor:hudDesc error:&error];
        if (!hud_pipeline) return false;

        // 6. Textured 2D UI Pipeline (TdUIScene / UI/TdUIResources.upk)
        id<MTLFunction> uiTexVert = [shader_library newFunctionWithName:@"ui_tex_vertex"];
        id<MTLFunction> uiTexFrag = [shader_library newFunctionWithName:@"ui_tex_fragment"];

        MTLVertexDescriptor* uiTexVertexDesc = [MTLVertexDescriptor vertexDescriptor];
        uiTexVertexDesc.attributes[0].format = MTLVertexFormatFloat2;
        uiTexVertexDesc.attributes[0].offset = offsetof(UITexVertex, position);
        uiTexVertexDesc.attributes[0].bufferIndex = 0;
        uiTexVertexDesc.attributes[1].format = MTLVertexFormatFloat2;
        uiTexVertexDesc.attributes[1].offset = offsetof(UITexVertex, uv);
        uiTexVertexDesc.attributes[1].bufferIndex = 0;
        uiTexVertexDesc.attributes[2].format = MTLVertexFormatFloat4;
        uiTexVertexDesc.attributes[2].offset = offsetof(UITexVertex, color);
        uiTexVertexDesc.attributes[2].bufferIndex = 0;
        uiTexVertexDesc.layouts[0].stride = sizeof(UITexVertex);
        uiTexVertexDesc.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

        MTLRenderPipelineDescriptor* uiTexDesc = [[MTLRenderPipelineDescriptor alloc] init];
        uiTexDesc.vertexFunction = uiTexVert;
        uiTexDesc.fragmentFunction = uiTexFrag;
        uiTexDesc.vertexDescriptor = uiTexVertexDesc;
        uiTexDesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        uiTexDesc.colorAttachments[0].blendingEnabled = YES;
        uiTexDesc.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
        uiTexDesc.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
        uiTexDesc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        uiTexDesc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha;
        uiTexDesc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        uiTexDesc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;

        ui_tex_pipeline = [device newRenderPipelineStateWithDescriptor:uiTexDesc error:&error];
        if (!ui_tex_pipeline) return false;

        // Depth Stencil States
        MTLDepthStencilDescriptor* dsWrite = [[MTLDepthStencilDescriptor alloc] init];
        dsWrite.depthCompareFunction = MTLCompareFunctionLessEqual;
        dsWrite.depthWriteEnabled = YES;
        depth_write_state = [device newDepthStencilStateWithDescriptor:dsWrite];

        MTLDepthStencilDescriptor* dsTest = [[MTLDepthStencilDescriptor alloc] init];
        dsTest.depthCompareFunction = MTLCompareFunctionLessEqual;
        dsTest.depthWriteEnabled = NO;
        depth_test_only_state = [device newDepthStencilStateWithDescriptor:dsTest];

        MTLDepthStencilDescriptor* dsDisabled = [[MTLDepthStencilDescriptor alloc] init];
        dsDisabled.depthCompareFunction = MTLCompareFunctionAlways;
        dsDisabled.depthWriteEnabled = NO;
        depth_disabled_state = [device newDepthStencilStateWithDescriptor:dsDisabled];

        // Linear Texture Sampler
        MTLSamplerDescriptor* sampDesc = [[MTLSamplerDescriptor alloc] init];
        sampDesc.minFilter = MTLSamplerMinMagFilterLinear;
        sampDesc.magFilter = MTLSamplerMinMagFilterLinear;
        sampDesc.sAddressMode = MTLSamplerAddressModeClampToEdge;
        sampDesc.tAddressMode = MTLSamplerAddressModeClampToEdge;
        linear_sampler = [device newSamplerStateWithDescriptor:sampDesc];

        return true;
    }

    void allocate_render_targets() {
        MTLTextureDescriptor* hdrDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                                          width:width
                                                                                         height:height
                                                                                      mipmapped:NO];
        hdrDesc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        hdrDesc.storageMode = MTLStorageModePrivate;
        scene_hdr_tex = [device newTextureWithDescriptor:hdrDesc];

        MTLTextureDescriptor* depthDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                            width:width
                                                                                           height:height
                                                                                        mipmapped:NO];
        depthDesc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        depthDesc.storageMode = MTLStorageModePrivate;
        offscreen_depth_tex = [device newTextureWithDescriptor:depthDesc];

        if (!shadow_depth_tex) {
            MTLTextureDescriptor* shDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                              width:kSunShadowMapSize
                                                                                             height:kSunShadowMapSize
                                                                                          mipmapped:NO];
            shDesc.textureType = MTLTextureType2DArray;
            shDesc.arrayLength = 2;  // kSunShadowNearSlice, kSunShadowFarSlice
            shDesc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            shDesc.storageMode = MTLStorageModePrivate;
            shadow_depth_tex = [device newTextureWithDescriptor:shDesc];
            shadow_far_valid = false;
        }

        MTLTextureDescriptor* colorDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                            width:width
                                                                                           height:height
                                                                                        mipmapped:NO];
        colorDesc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        colorDesc.storageMode = MTLStorageModeShared;
        offscreen_color_tex = [device newTextureWithDescriptor:colorDesc];

        // Copies of the opaque scene (UE3 "resolved" SceneColor / SceneDepth) sampled by
        // translucent materials (SceneTexture, DestColor, DepthBiasedAlpha/Blend).
        MTLTextureDescriptor* copyColorDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                                                 width:width
                                                                                                height:height
                                                                                             mipmapped:NO];
        copyColorDesc.usage = MTLTextureUsageShaderRead;
        copyColorDesc.storageMode = MTLStorageModePrivate;
        scene_color_copy = [device newTextureWithDescriptor:copyColorDesc];

        MTLTextureDescriptor* copyDepthDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                                 width:width
                                                                                                height:height
                                                                                             mipmapped:NO];
        copyDepthDesc.usage = MTLTextureUsageShaderRead;
        copyDepthDesc.storageMode = MTLStorageModePrivate;
        scene_depth_copy = [device newTextureWithDescriptor:copyDepthDesc];
    }

    struct WeaponGPUTextures {
        id<MTLTexture> diffuse = nil;
        id<MTLTexture> specular = nil;
        id<MTLTexture> normal = nil;
        id<MTLTexture> mask = nil;
    };
    id<MTLTexture> vm_skin_gpu_tex = nil;
    id<MTLTexture> vm_glove_gpu_tex = nil;
    id<MTLTexture> vm_lower_gpu_tex = nil;
    id<MTLTexture> swat_d_gpu_tex = nil;
    id<MTLTexture> swat_s_gpu_tex = nil;
    id<MTLTexture> swat_n_gpu_tex = nil;
    id<MTLTexture> ammo_d_gpu_tex = nil;
    std::unordered_map<std::string, WeaponGPUTextures> weapon_gpu_textures;

    id<MTLTexture> upload_dxt1_texture(const DXT1Texture& dxt, bool srgb = false) {
        if (!dxt.is_valid()) return nil;
        std::vector<uint8_t> rgba;
        if (!dxt.decode_rgba8(rgba) || rgba.empty()) return nil;
        MTLPixelFormat fmt = srgb ? MTLPixelFormatRGBA8Unorm_sRGB : MTLPixelFormatRGBA8Unorm;
        MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt
                                                                                        width:static_cast<NSUInteger>(dxt.width)
                                                                                       height:static_cast<NSUInteger>(dxt.height)
                                                                                    mipmapped:YES];
        desc.usage = MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModeShared;
        id<MTLTexture> tex = [device newTextureWithDescriptor:desc];
        if (!tex) return nil;
        [tex replaceRegion:MTLRegionMake2D(0, 0, static_cast<NSUInteger>(dxt.width), static_cast<NSUInteger>(dxt.height))
               mipmapLevel:0
                 withBytes:rgba.data()
               bytesPerRow:static_cast<NSUInteger>(dxt.width) * 4];
        if (dxt.width >= 4 && dxt.height >= 4) {
            id<MTLCommandBuffer> cb = [command_queue commandBuffer];
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            [blit generateMipmapsForTexture:tex];
            [blit endEncoding];
            [cb commit];
        }
        return tex;
    }

    // Real UE3 USkeletalMesh & TdAnimSet assets (CH_Faith_1P, KrugerSec/CPF SWAT, weapons) used for
    // the first-person viewmodel, enemies and dropped weapons.
    void load_character_assets() {
        anim_system.init_from_game_root("/Users/tomnom/mirrorsedge");
        if (!anim_system.is_loaded()) return;

        vm_skin_gpu_tex  = upload_dxt1_texture(anim_system.faith_skin_tex(), false);
        vm_glove_gpu_tex = upload_dxt1_texture(anim_system.faith_glove_tex(), false);
        vm_lower_gpu_tex = upload_dxt1_texture(anim_system.faith_lower_tex(), false);
        swat_d_gpu_tex   = upload_dxt1_texture(anim_system.swat_diffuse_tex(), true);
        swat_s_gpu_tex   = upload_dxt1_texture(anim_system.swat_specular_tex(), true);
        swat_n_gpu_tex   = upload_dxt1_texture(anim_system.swat_normal_tex(), false);
        ammo_d_gpu_tex   = upload_dxt1_texture(anim_system.ammo_diffuse_tex(), true);

        for (const auto& [wname, wmesh] : anim_system.weapon_meshes()) {
            WeaponGPUTextures wtex{};
            wtex.diffuse  = upload_dxt1_texture(wmesh.tex_diffuse, true);
            wtex.specular = upload_dxt1_texture(wmesh.tex_specular, true);
            wtex.normal   = upload_dxt1_texture(wmesh.tex_normal, false);
            wtex.mask     = upload_dxt1_texture(wmesh.tex_mask, true);
            weapon_gpu_textures[wname] = wtex;
        }
    }

    void build_faith_viewmodel(const PlayerTelemetry& telemetry) {
        // CH_Faith_1P skinned by the AnimSystem; nothing is drawn without the real assets.
        if (anim_system.is_loaded()) {
            anim_system.evaluate_faith_1p(telemetry, faith_viewmodel_mesh);
        } else {
            faith_viewmodel_mesh.clear();
        }
    }

    void ensure_main_menu_loaded() {
        if (!main_menu.is_loaded()) {
            main_menu.init("/Users/tomnom/mirrorsedge");
        }
        if (!main_menu_gpu_ready) {
            ui_logo_tex      = upload_scene_texture(main_menu.logo_texture());
            ui_bag_tex       = upload_scene_texture(main_menu.icon_bag_texture());
            ui_time_tex      = upload_scene_texture(main_menu.icon_time_texture());
            ui_panel_bg_tex  = upload_scene_texture(main_menu.panel_bg_texture());
            ui_faith_art_tex = upload_scene_texture(main_menu.faith_art_texture());
            for (int i = 0; i < 10; ++i) {
                ui_chapter_tex[i] = upload_scene_texture(main_menu.chapter_preview_texture(i));
            }
            auto upload_font_pages = [&](const UIMultiFont& f, std::vector<id<MTLTexture>>& out) {
                out.clear();
                out.reserve(f.pages.size());
                for (const auto& st : f.pages) {
                    out.push_back(upload_scene_texture(st));
                }
            };
            upload_font_pages(main_menu.headline_thick_font(), ui_font_headline_thick_tex);
            upload_font_pages(main_menu.headline_light_font(), ui_font_headline_light_tex);
            upload_font_pages(main_menu.medium_italic_font(),  ui_font_medium_italic_tex);
            upload_font_pages(main_menu.small_italic_font(),   ui_font_small_italic_tex);
            main_menu_gpu_ready = true;
        }
    }

    void draw_ui_quad(std::vector<HUDVertex>& verts, float x, float y, float w, float h, simd_float4 color) {
        HUDVertex v0 = {{x, y}, color};
        HUDVertex v1 = {{x + w, y}, color};
        HUDVertex v2 = {{x + w, y + h}, color};
        HUDVertex v3 = {{x, y + h}, color};
        verts.push_back(v0); verts.push_back(v1); verts.push_back(v2);
        verts.push_back(v0); verts.push_back(v2); verts.push_back(v3);
    }

    // Forward-slanted parallelogram matching Mirror's Edge TdUIScene / StickSlant UI bars
    void draw_ui_skew_quad(std::vector<HUDVertex>& verts, float x, float y, float w, float h,
                           float slant_dx, simd_float4 color) {
        HUDVertex v0 = {{x + slant_dx, y}, color};
        HUDVertex v1 = {{x + w + slant_dx, y}, color};
        HUDVertex v2 = {{x + w, y + h}, color};
        HUDVertex v3 = {{x, y + h}, color};
        verts.push_back(v0); verts.push_back(v1); verts.push_back(v2);
        verts.push_back(v0); verts.push_back(v2); verts.push_back(v3);
    }

    void add_ui_tex_quad(std::vector<UITextureBatch>& batches, id<MTLTexture> tex,
                         float x, float y, float w, float h,
                         float u0 = 0.0f, float v0 = 0.0f, float u1 = 1.0f, float v1 = 1.0f,
                         simd_float4 tint = simd_make_float4(1.0f, 1.0f, 1.0f, 1.0f)) {
        if (!tex) return;
        UITextureBatch* batch = nullptr;
        if (!batches.empty() && batches.back().tex == tex) {
            batch = &batches.back();
        } else {
            batches.push_back(UITextureBatch{tex, {}});
            batch = &batches.back();
        }
        UITexVertex p0 = {{x,     y},     {u0, v0}, tint};
        UITexVertex p1 = {{x + w, y},     {u1, v0}, tint};
        UITexVertex p2 = {{x + w, y + h}, {u1, v1}, tint};
        UITexVertex p3 = {{x,     y + h}, {u0, v1}, tint};
        batch->verts.push_back(p0); batch->verts.push_back(p1); batch->verts.push_back(p2);
        batch->verts.push_back(p0); batch->verts.push_back(p2); batch->verts.push_back(p3);
    }

    // Renders authentic UE3 MultiFont glyph atlases (UI_Fonts_Final.upk: Helvetica_Headline_Thick_Italic,
    // Helvetica_Headline_Light_Italic, Helvetica_Medium_Italic, Helvetica_Small_Bold_Italic).
    float draw_multifont_text(std::vector<UITextureBatch>& tex_batches,
                              std::vector<HUDVertex>& fallback_verts,
                              const UIMultiFont& font,
                              const std::vector<id<MTLTexture>>& pages,
                              const std::string& text,
                              float start_x,
                              float start_y,
                              float target_px_height,
                              simd_float4 color,
                              bool drop_shadow = true) {
        if (!font.valid() || pages.empty()) {
            draw_ui_text_italic(fallback_verts, text, start_x, start_y,
                                std::max(1.0f, target_px_height / 9.0f), color, drop_shadow);
            return static_cast<float>(text.size()) * target_px_height * 0.6f;
        }

        const float scale = target_px_height / std::max(12.0f, font.base_line_height);
        const float space_advance = target_px_height * 0.28f;
        const float tracking = target_px_height * 0.025f;

        auto emit_pass = [&](float ox, float oy, simd_float4 pass_col) -> float {
            float pen_x = start_x + ox;
            float pen_y = start_y + oy;
            for (unsigned char ch : text) {
                if (ch == ' ') {
                    pen_x += space_advance;
                    continue;
                }
                if (ch < 32) continue;
                const UIFontGlyph& g = font.glyphs[ch];
                if (g.w <= 0 || g.h <= 0 || g.page >= pages.size() || !pages[g.page]) {
                    pen_x += space_advance * 0.8f;
                    continue;
                }
                id<MTLTexture> tex = pages[g.page];
                float tw = static_cast<float>(tex.width);
                float th = static_cast<float>(tex.height);
                float u0 = static_cast<float>(g.u) / tw;
                float v0 = static_cast<float>(g.v) / th;
                float u1 = static_cast<float>(g.u + g.w) / tw;
                float v1 = static_cast<float>(g.v + g.h) / th;

                float gw = static_cast<float>(g.w) * scale;
                float gh = static_cast<float>(g.h) * scale;
                float gy = pen_y + static_cast<float>(g.v_offset) * scale;

                // Negative alpha triggers UI alpha-mask tinting (s.a * -tint.w) in ui_tex_fragment
                simd_float4 mask_tint = simd_make_float4(pass_col.x, pass_col.y, pass_col.z, -pass_col.w);
                add_ui_tex_quad(tex_batches, tex, pen_x, gy, gw, gh, u0, v0, u1, v1, mask_tint);
                pen_x += gw + tracking;
            }
            return pen_x - (start_x + ox);
        };

        if (drop_shadow) {
            float sh_off = std::max(1.2f, target_px_height * 0.055f);
            simd_float4 sh_col = simd_make_float4(0.03f, 0.05f, 0.09f, color.w * 0.65f);
            emit_pass(sh_off, sh_off, sh_col);
        }
        return emit_pass(0.0f, 0.0f, color);
    }

    void draw_ui_text_raw(std::vector<HUDVertex>& verts, const std::string& text, float start_x, float start_y,
                          float scale, simd_float4 color, float italic_shear = 0.0f) {
        float cur_x = start_x;
        float cur_y = start_y;
        float char_w = 5.0f * scale;
        float char_h = 7.0f * scale;
        float spacing = 1.15f * scale;

        for (char ch : text) {
            if (ch == '\n') {
                cur_x = start_x;
                cur_y += char_h + 3.0f * scale;
                continue;
            }
            if (ch < 32 || ch > 126) ch = '?';
            int idx = ch - 32;

            for (int col = 0; col < 5; ++col) {
                uint8_t line = FONT_5X7[idx][col];
                for (int row = 0; row < 7; ++row) {
                    if ((line >> row) & 1) {
                        float shear_x = (6.0f - float(row)) * scale * italic_shear;
                        float px = cur_x + col * scale + shear_x;
                        float py = cur_y + row * scale;
                        draw_ui_quad(verts, px, py, scale, scale, color);
                    }
                }
            }
            cur_x += char_w + spacing;
        }
    }

    void draw_ui_text(std::vector<HUDVertex>& verts, const std::string& text, float start_x, float start_y,
                      float scale, simd_float4 color) {
        // High-contrast dark drop shadow for 100% legibility over bright sky & white rooftops
        simd_float4 shadow_col = simd_make_float4(0.02f, 0.03f, 0.05f, color.w * 0.85f);
        draw_ui_text_raw(verts, text, start_x + 1.5f, start_y + 1.5f, scale, shadow_col, 0.0f);
        draw_ui_text_raw(verts, text, start_x, start_y, scale, color, 0.0f);
    }

    // Forward-slanted italic sans-serif typography matching UI_Fonts_Final.Menus.Fonts_Positec
    void draw_ui_text_italic(std::vector<HUDVertex>& verts, const std::string& text, float start_x, float start_y,
                             float scale, simd_float4 color, bool dark_shadow = true, float shear = 0.22f) {
        if (dark_shadow) {
            simd_float4 shadow_col = simd_make_float4(0.02f, 0.03f, 0.05f, color.w * 0.82f);
            draw_ui_text_raw(verts, text, start_x + 1.4f, start_y + 1.4f, scale, shadow_col, shear);
        }
        draw_ui_text_raw(verts, text, start_x, start_y, scale, color, shear);
    }

    void draw_ui_reticle(std::vector<HUDVertex>& verts, float cx, float cy, bool is_target, float pulse) {
        simd_float4 shadow = simd_make_float4(0.05f, 0.07f, 0.10f, 0.75f);
        simd_float4 color = is_target ? simd_make_float4(0.902f, 0.078f, 0.078f, 1.0f)
                                      : simd_make_float4(1.0f, 1.0f, 1.0f, 0.92f);

        float r = 2.5f * (is_target ? (1.0f + 0.3f * pulse) : 1.0f);
        draw_ui_quad(verts, cx - r - 1.0f, cy - r - 1.0f, (r + 1.0f) * 2.0f, (r + 1.0f) * 2.0f, shadow);
        draw_ui_quad(verts, cx - r, cy - r, r * 2.0f, r * 2.0f, color);

        float d = 10.0f;
        float len = 5.0f;
        draw_ui_quad(verts, cx - d - len, cy - 1.0f, len, 2.0f, color);
        draw_ui_quad(verts, cx + d, cy - 1.0f, len, 2.0f, color);
        draw_ui_quad(verts, cx - 1.0f, cy - d - len, 2.0f, len, color);
        draw_ui_quad(verts, cx - 1.0f, cy + d, 2.0f, len, color);
    }

    // -------------------------------------------------------------------------
    // Authentic Mirror's Edge Frontend UI (TdMainMenu + TdLoadLevel)
    // Layout & Typography faithful to TdUI_FrontEnd.upk & UI_Fonts_Final.upk
    // -------------------------------------------------------------------------
    void draw_main_menu_ui(std::vector<HUDVertex>& bg_verts,
                           std::vector<UITextureBatch>& tex_batches,
                           std::vector<HUDVertex>& fg_verts,
                           const PlayerTelemetry& telemetry) {
        bg_verts.clear();
        tex_batches.clear();
        fg_verts.clear();

        const float w = float(width);
        const float h = float(height);
        const float sx = w / 1280.0f;
        const float sy = h / 720.0f;

        const simd_float4 runner_red   = simd_make_float4(0.890f, 0.078f, 0.078f, 0.96f); // #E31414
        const simd_float4 dark_ink     = simd_make_float4(0.110f, 0.135f, 0.175f, 0.96f);
        const simd_float4 muted_ink    = simd_make_float4(0.240f, 0.285f, 0.350f, 0.92f);
        const simd_float4 pure_white   = simd_make_float4(1.000f, 1.000f, 1.000f, 1.00f);
        const simd_float4 row_strip    = simd_make_float4(0.960f, 0.975f, 0.992f, 0.62f);
        const simd_float4 col_veil     = simd_make_float4(0.955f, 0.970f, 0.988f, 0.42f);
        const simd_float4 dark_bar     = simd_make_float4(0.085f, 0.105f, 0.140f, 0.86f);

        const UIMultiFont& f_head_thick = main_menu.headline_thick_font();
        const UIMultiFont& f_head_light = main_menu.headline_light_font();
        const UIMultiFont& f_med_italic = main_menu.medium_italic_font();
        const UIMultiFont& f_sml_italic = main_menu.small_italic_font();

        const int sel = std::clamp(selected_chapter, 0, 9);
        const MenuChapterEntry& cur_ch = main_menu.get_chapter(sel);

        // =====================================================================
        // 1. TOP-LEFT OFFICIAL MIRROR'S EDGE LOGO (StartTitleImage from TdMainMenu.me1)
        // =====================================================================
        const float logo_x = 92.0f * sx;
        const float logo_y = 36.0f * sy;
        const float logo_h = 54.0f * sy;
        const float logo_w = logo_h * 4.0f; // Native 4:1 aspect ratio (256x64 / 512x128)
        if (ui_logo_tex) {
            // Left 24% of StartTitleImage is the iconic Runner Star (tinted Scarlet Red)
            add_ui_tex_quad(tex_batches, ui_logo_tex,
                            logo_x, logo_y, logo_w * 0.24f, logo_h,
                            0.0f, 0.0f, 0.24f, 1.0f,
                            simd_make_float4(0.89f, 0.08f, 0.08f, -1.0f));
            // Right 76% of StartTitleImage is the official MIRROR'S EDGE wordmark
            add_ui_tex_quad(tex_batches, ui_logo_tex,
                            logo_x + logo_w * 0.24f + 1.5f * sx, logo_y + 1.5f * sy, logo_w * 0.76f, logo_h,
                            0.24f, 0.0f, 1.0f, 1.0f,
                            simd_make_float4(1.0f, 1.0f, 1.0f, -0.75f));
            add_ui_tex_quad(tex_batches, ui_logo_tex,
                            logo_x + logo_w * 0.24f, logo_y, logo_w * 0.76f, logo_h,
                            0.24f, 0.0f, 1.0f, 1.0f,
                            simd_make_float4(0.11f, 0.13f, 0.17f, -0.98f));
        }

        // =====================================================================
        // 2. LEFT SAFE-REGION: TAB-SPECIFIC MENU ROWS (STORY / RACE / OPTIONS / EXTRAS)
        // =====================================================================
        const float lx = 96.0f * sx;
        const float ly = 104.0f * sy;
        const float lw = 380.0f * sx;

        const std::string left_heading =
            (selected_menu_tab == 1) ? "SPEED RUN COURSES" :
            (selected_menu_tab == 2) ? "GAME & VIDEO OPTIONS" :
            (selected_menu_tab == 3) ? "EXTRAS & ARCHIVE" :
            config_title_or("LOAD CHAPTER");

        draw_multifont_text(tex_batches, fg_verts, f_head_thick, ui_font_headline_thick_tex,
                            left_heading, lx, ly, 23.0f * sy, dark_ink, false);
        draw_ui_skew_quad(fg_verts, lx - 4.0f * sx, ly + 28.0f * sy, lw, 2.5f * sy, 3.0f * sx, runner_red);

        const float list_top = ly + 38.0f * sy;
        const float row_step = 35.5f * sy;
        const float row_h    = 31.0f * sy;

        auto draw_menu_row = [&](int idx, const std::string& label, bool is_sel) {
            const float ry = list_top + float(idx) * row_step;
            if (is_sel) {
                draw_ui_skew_quad(bg_verts, lx - 8.0f * sx, ry, lw + 18.0f * sx, row_h, 9.0f * sx, runner_red);
                draw_multifont_text(tex_batches, fg_verts, f_head_thick, ui_font_headline_thick_tex,
                                    label, lx + 8.0f * sx, ry + 5.5f * sy, 18.0f * sy, pure_white, true);
            } else {
                draw_ui_skew_quad(bg_verts, lx, ry + 1.5f * sy, lw, row_h - 3.0f * sy, 7.5f * sx, row_strip);
                draw_multifont_text(tex_batches, fg_verts, f_med_italic, ui_font_medium_italic_tex,
                                    label, lx + 8.0f * sx, ry + 6.5f * sy, 15.8f * sy, dark_ink, false);
            }
        };

        if (selected_menu_tab == 0 || selected_menu_tab == 1) {
            // STORY & RACE: 10 Campaign / Speed Run Chapters
            for (int i = 0; i < 10; ++i) {
                const MenuChapterEntry& ch = main_menu.get_chapter(i);
                std::string ch_upper = ch.map_name;
                for (char& c : ch_upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                if (selected_menu_tab == 1) {
                    ch_upper += "   [" + ch.speedrun_target_time + "]";
                }
                draw_menu_row(i, ch_upper, i == sel);
            }
        } else if (selected_menu_tab == 2) {
            // OPTIONS: 6 Interactive Game, Input & Display Settings
            const int rsel = std::clamp(selected_menu_row, 0, 5);
            const std::string opt_rows[6] = {
                "MOUSE SENSITIVITY:   " + std::to_string(opt_sens_pct) + "%",
                "FIELD OF VIEW (FOV): " + std::to_string(opt_fov_deg) + " DEG",
                std::string("DISPLAY MODE:        ") + (opt_fullscreen ? "FULLSCREEN" : "WINDOWED"),
                std::string("REACTION TIME:       ") + (telemetry.reaction_active ? "ACTIVE" : "READY"),
                "RESET TO ACTIVE CHECKPOINT",
                "QUIT TO DESKTOP"
            };
            for (int i = 0; i < 6; ++i) {
                draw_menu_row(i, opt_rows[i], i == rsel);
            }
        } else {
            // EXTRAS: 6 Interactive Cutscene, Weapon & Sandbox Actions
            const int rsel = std::clamp(selected_menu_row, 0, 5);
            static const char* kExtraRows[6] = {
                "PLAY CHAPTER OPENING MOVIE",
                "CYCLE ALL 17 BINK CUTSCENES",
                "PLAY 3D ROOFTOP INTRO FLY-IN",
                "EQUIP RUNNER SIDEARM (M1911)",
                "DEPLOY KRUGERSEC SQUAD AHEAD",
                "ALL 10 CHAPTERS: UNLOCKED"
            };
            for (int i = 0; i < 6; ++i) {
                draw_menu_row(i, kExtraRows[i], i == rsel);
            }
        }

        // =====================================================================
        // 3. RIGHT SAFE-REGION: CHAPTER PREVIEW HALFTONE PHOTO & STATS (TdLoadLevel)
        // =====================================================================
        const float rw = 368.0f * sx;
        const float rx = w - 96.0f * sx - rw;
        const float ry = 104.0f * sy;

        std::string cur_upper = cur_ch.map_name;
        for (char& c : cur_upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        const std::string right_heading =
            (selected_menu_tab == 1) ? ("COURSE: " + cur_upper) :
            (selected_menu_tab == 2) ? "SYSTEM & GRAPHICS" :
            (selected_menu_tab == 3) ? "RUNNER ARCHIVE" :
            cur_upper;

        draw_multifont_text(tex_batches, fg_verts, f_head_thick, ui_font_headline_thick_tex,
                            right_heading, rx, ry, 23.0f * sy, runner_red, false);
        draw_ui_skew_quad(fg_verts, rx - 4.0f * sx, ry + 28.0f * sy, rw, 2.5f * sy, 3.0f * sx, dark_ink);

        std::string supers_flat = cur_ch.district_timestamp;
        for (char& c : supers_flat) {
            if (c == '\n') c = ' ';
        }
        const std::string right_sub =
            (selected_menu_tab == 1) ? "INSTANT START (NO CUTSCENES)" :
            (selected_menu_tab == 2) ? ("APPLE METAL 3.0  •  " + std::to_string(width) + "x" + std::to_string(height)) :
            (selected_menu_tab == 3) ? "17 BINK MOVIES  •  199 SUBTITLES" :
            supers_flat;

        draw_multifont_text(tex_batches, fg_verts, f_head_light, ui_font_headline_light_tex,
                            right_sub, rx + 2.0f * sx, ry + 35.0f * sy, 18.0f * sy, dark_ink, false);

        // Halftone Chapter Preview Photograph (UI/TdUIResources_CheckpointImages.upk)
        const float img_x = rx;
        const float img_y = ry + 64.0f * sy;
        const float img_w = rw;
        const float img_h = 184.0f * sy;
        draw_ui_quad(bg_verts, img_x - 3.0f * sx, img_y - 3.0f * sy, img_w + 6.0f * sx, img_h + 6.0f * sy,
                     simd_make_float4(1.0f, 1.0f, 1.0f, 0.78f));
        if (ui_chapter_tex[sel]) {
            add_ui_tex_quad(tex_batches, ui_chapter_tex[sel],
                            img_x, img_y, img_w, img_h,
                            0.0f, 0.04f, 1.0f, 0.98f, pure_white);
        }

        // Compact Speed Run Time & Runner Bags Stats Strip (TdLoadLevel.LevelStatsPanel)
        const float st_y = img_y + img_h + 12.0f * sy;
        const float st_h = 56.0f * sy;
        draw_ui_skew_quad(bg_verts, rx, st_y, rw, st_h, 12.0f * sx, dark_bar);
        draw_ui_skew_quad(fg_verts, rx, st_y, 4.0f * sx, st_h, 12.0f * sx, runner_red);

        if (ui_time_tex) {
            add_ui_tex_quad(tex_batches, ui_time_tex,
                            rx + 18.0f * sx, st_y + 10.0f * sy, 34.0f * sy, 34.0f * sy,
                            0.0f, 0.0f, 1.0f, 1.0f,
                            simd_make_float4(1.0f, 1.0f, 1.0f, -0.95f));
        }
        draw_multifont_text(tex_batches, fg_verts, f_sml_italic, ui_font_small_italic_tex,
                            main_menu.config().speed_run_time_label,
                            rx + 58.0f * sx, st_y + 9.0f * sy, 12.5f * sy,
                            simd_make_float4(0.76f, 0.81f, 0.88f, 0.95f), true);
        draw_multifont_text(tex_batches, fg_verts, f_head_thick, ui_font_headline_thick_tex,
                            cur_ch.speedrun_target_time,
                            rx + 58.0f * sx, st_y + 24.0f * sy, 20.0f * sy, pure_white, true);

        const float bag_x = rx + rw * 0.56f;
        if (ui_bag_tex) {
            add_ui_tex_quad(tex_batches, ui_bag_tex,
                            bag_x, st_y + 10.0f * sy, 34.0f * sy, 34.0f * sy,
                            0.0f, 0.0f, 1.0f, 1.0f,
                            simd_make_float4(1.0f, 0.84f, 0.16f, -0.98f));
        }
        const int bags_found = std::clamp(telemetry.bags_collected, 1, 3);
        draw_multifont_text(tex_batches, fg_verts, f_sml_italic, ui_font_small_italic_tex,
                            main_menu.config().bags_found_label,
                            bag_x + 42.0f * sx, st_y + 9.0f * sy, 12.5f * sy,
                            simd_make_float4(0.76f, 0.81f, 0.88f, 0.95f), true);
        std::string bag_str = std::to_string(bags_found) + " / 3";
        draw_multifont_text(tex_batches, fg_verts, f_head_thick, ui_font_headline_thick_tex,
                            bag_str, bag_x + 42.0f * sx, st_y + 24.0f * sy, 20.0f * sy,
                            simd_make_float4(1.0f, 0.85f, 0.18f, 1.0f), true);

        // =====================================================================
        // 4. LOWER-THIRD 4 ICONIC MIRROR'S EDGE CATEGORY COLUMNS (TdMainMenu)
        //    Exact X coordinates from TdUI_FrontEnd.upk: 96, 368, 640, 912 .. 1184
        // =====================================================================
        const float nav_y = 528.0f * sy;
        const float nav_h = 62.0f * sy;
        const float col_w = 272.0f * sx;
        static const char* kNavCaptions[4] = {"STORY", "RACE", "OPTIONS", "EXTRAS"};

        for (int c = 0; c < 4; ++c) {
            const float cx = (96.0f + float(c) * 272.0f) * sx;
            const bool active = (c == selected_menu_tab);
            draw_ui_skew_quad(bg_verts, cx, nav_y, col_w - 6.0f * sx, nav_h, 14.0f * sx,
                              active ? runner_red : col_veil);
            draw_ui_skew_quad(fg_verts, cx, nav_y, 3.0f * sx, nav_h, 14.0f * sx,
                              active ? pure_white : dark_ink);

            const std::string& cap = (c < static_cast<int>(main_menu.tabs().size()))
                ? main_menu.tabs()[c].caption
                : std::string(kNavCaptions[c]);
            draw_multifont_text(tex_batches, fg_verts,
                                active ? f_head_thick : f_head_light,
                                active ? ui_font_headline_thick_tex : ui_font_headline_light_tex,
                                cap,
                                cx + 22.0f * sx,
                                nav_y + (active ? 16.0f : 18.0f) * sy,
                                (active ? 28.0f : 25.0f) * sy,
                                active ? pure_white : dark_ink,
                                active);
        }

        // =====================================================================
        // 5. BOTTOM SAFE-REGION BUTTON BAR (TdUIButtonBar at y = 635.4..666)
        // =====================================================================
        const float btn_y = 636.0f * sy;
        const float btn_h = 30.0f * sy;

        draw_ui_skew_quad(bg_verts, 628.0f * sx, btn_y, 556.0f * sx, btn_h, 7.0f * sx, dark_bar);
        draw_ui_skew_quad(fg_verts, 628.0f * sx, btn_y, 4.0f * sx, btn_h, 7.0f * sx, runner_red);

        const char* bar_text =
            (selected_menu_tab == 1) ? "[CLICK / ENTER] START SPEED RUN      [ESC] RESUME" :
            (selected_menu_tab == 2) ? "[CLICK / ENTER] CHANGE OPTION        [ESC] RESUME" :
            (selected_menu_tab == 3) ? "[CLICK / ENTER] ACTIVATE EXTRA       [ESC] RESUME" :
                                       "[CLICK / ENTER] PLAY CHAPTER         [ESC] RESUME";

        draw_multifont_text(tex_batches, fg_verts, f_med_italic, ui_font_medium_italic_tex,
                            bar_text,
                            646.0f * sx, btn_y + 6.5f * sy, 15.0f * sy, pure_white, true);
    }

    std::string config_title_or(const char* fallback) const {
        const std::string& t = main_menu.config().load_chapter_title;
        return t.empty() ? std::string(fallback) : t;
    }

    void draw_hud(std::vector<HUDVertex>& verts, const LevelScene& scene, const PlayerTelemetry& telemetry) {
        verts.clear();
        float w = float(width);
        float h = float(height);
        float sim_time = telemetry.sim_time;

        // 1. Center Runner Reticle Dot + Dynamic Weapon Crosshair & Hit Marker
        float cx = w * 0.5f;
        float cy = h * 0.5f;
        bool is_interactive_target = telemetry.weapon.equipped ||
                                     telemetry.disarm_prompt_visible ||
                                     (telemetry.speed_2d > 450.0f) ||
                                     (telemetry.move_state == EMovement::MOVE_Snatch);
        draw_ui_reticle(verts, cx, cy, is_interactive_target, std::sin(sim_time * 8.0f));

        if (telemetry.weapon.equipped) {
            float gap = 10.0f + telemetry.weapon.spread_rad * 180.0f + telemetry.weapon.fire_anim_timer * 28.0f;
            float tick_len = (telemetry.weapon.pellet_count > 1) ? 10.0f : 7.0f;
            simd_float4 xhair_col = (telemetry.weapon.ammo > 0)
                ? simd_make_float4(0.95f, 0.97f, 1.0f, 0.88f)
                : simd_make_float4(0.95f, 0.18f, 0.18f, 0.92f);
            draw_ui_quad(verts, cx - gap - tick_len, cy - 1.0f, tick_len, 2.0f, xhair_col);
            draw_ui_quad(verts, cx + gap,            cy - 1.0f, tick_len, 2.0f, xhair_col);
            draw_ui_quad(verts, cx - 1.0f, cy - gap - tick_len, 2.0f, tick_len, xhair_col);
            draw_ui_quad(verts, cx - 1.0f, cy + gap,            2.0f, tick_len, xhair_col);
        }

        // Uncontrolled falling wind edge vignette & lethal fall impact crimson-to-black screen fade
        if (telemetry.fall_death_impact) {
            float p = std::clamp(telemetry.death_anim_progress, 0.0f, 1.0f);
            float red_flash = (p < 0.25f) ? (1.0f - p / 0.25f) * 0.55f : 0.0f;
            float blackout  = std::clamp((p - 0.12f) / 0.72f, 0.0f, 1.0f);
            if (red_flash > 0.0f) {
                draw_ui_quad(verts, 0.0f, 0.0f, w, h, simd_make_float4(0.82f, 0.04f, 0.04f, red_flash));
            }
            if (blackout > 0.0f) {
                draw_ui_quad(verts, 0.0f, 0.0f, w, h, simd_make_float4(0.0f, 0.0f, 0.0f, blackout));
            }
        } else if (telemetry.falling_to_death) {
            float rush = std::clamp((-telemetry.velocity.z - 1200.0f) / 1400.0f, 0.25f, 0.85f);
            float edge_w = w * 0.14f;
            float edge_h = h * 0.16f;
            simd_float4 vig = simd_make_float4(0.02f, 0.02f, 0.04f, rush * 0.55f);
            draw_ui_quad(verts, 0.0f, 0.0f, w, edge_h, vig);
            draw_ui_quad(verts, 0.0f, h - edge_h, w, edge_h, vig);
            draw_ui_quad(verts, 0.0f, edge_h, edge_w, h - 2.0f * edge_h, vig);
            draw_ui_quad(verts, w - edge_w, edge_h, edge_w, h - 2.0f * edge_h, vig);
        }

        if (telemetry.hit_marker_timer > 0.0f) {
            float alpha = std::clamp(telemetry.hit_marker_timer / 0.22f, 0.0f, 1.0f);
            simd_float4 hm_col = simd_make_float4(0.96f, 0.14f, 0.14f, alpha);
            for (int d = 5; d <= 12; d += 2) {
                float fd = static_cast<float>(d);
                draw_ui_quad(verts, cx - fd - 1.5f, cy - fd - 1.5f, 3.0f, 3.0f, hm_col);
                draw_ui_quad(verts, cx + fd - 1.5f, cy - fd - 1.5f, 3.0f, 3.0f, hm_col);
                draw_ui_quad(verts, cx - fd - 1.5f, cy + fd - 1.5f, 3.0f, 3.0f, hm_col);
                draw_ui_quad(verts, cx + fd - 1.5f, cy + fd - 1.5f, 3.0f, 3.0f, hm_col);
            }
        }

        if (telemetry.disarm_prompt_visible) {
            std::string dprompt = "[RIGHT CLICK / E] DISARM WEAPON";
            float dw = float(dprompt.length()) * 11.5f + 28.0f;
            float dx = (w - dw) * 0.5f;
            float dy = cy + 46.0f;
            draw_ui_quad(verts, dx, dy, dw, 26.0f, simd_make_float4(0.88f, 0.06f, 0.06f, 0.90f));
            draw_ui_text(verts, dprompt, dx + 14.0f, dy + 6.0f, 1.8f, simd_make_float4(1.0f, 1.0f, 1.0f, 1.0f));
        }

        // 2. Bottom-Left Telemetry Panel Backdrop (Sleek Translucent Dark Glass + Red Runner Accent)
        draw_ui_quad(verts, 22.0f, h - 164.0f, 340.0f, 112.0f, simd_make_float4(0.04f, 0.06f, 0.09f, 0.72f));
        draw_ui_quad(verts, 22.0f, h - 164.0f, 4.0f, 112.0f, simd_make_float4(0.902f, 0.078f, 0.078f, 0.95f));

        float speed = telemetry.speed_2d;
        float kmh = speed * 0.06f;
        bool at_max_speed = (speed >= 695.0f);
        std::ostringstream ss_spd;
        ss_spd << "SPEED: " << std::fixed << std::setprecision(0) << speed << " u/s ("
               << std::setprecision(1) << kmh << " km/h)";
        if (at_max_speed) ss_spd << " MAX";
        draw_ui_text(verts, ss_spd.str(), 35.0f, h - 88.0f, 2.0f, simd_make_float4(1.0f, 1.0f, 1.0f, 0.98f));

        // Flow momentum bar frame (scaled to 720 u/s top ground speed)
        draw_ui_quad(verts, 35.0f, h - 68.0f, 240.0f, 10.0f, simd_make_float4(0.12f, 0.15f, 0.20f, 0.85f));
        float bar_fill = std::clamp(speed / 720.0f, 0.0f, 1.0f);
        simd_float4 bar_col = at_max_speed ? simd_make_float4(0.92f, 0.98f, 1.0f, 1.0f)
                            : (speed > 400.0f) ? simd_make_float4(0.902f, 0.078f, 0.078f, 1.0f)
                                               : simd_make_float4(0.30f, 0.78f, 0.98f, 0.95f);
        draw_ui_quad(verts, 37.0f, h - 66.0f, 236.0f * bar_fill, 6.0f, bar_col);

        // 3. Current Parkour Move State
        std::string move_name = move_state_name(telemetry.move_state);
        std::string move_label = "MOVE: " + move_name;
        draw_ui_text(verts, move_label, 35.0f, h - 114.0f, 2.0f, simd_make_float4(0.95f, 0.97f, 1.0f, 0.95f));

        // 4. Health & Reaction Time Meters
        float hp_pct = std::clamp(telemetry.health / 100.0f, 0.0f, 1.0f);
        float rt_pct = std::clamp(telemetry.reaction_energy / 100.0f, 0.0f, 1.0f);

        draw_ui_text(verts, "HEALTH", 35.0f, h - 154.0f, 1.6f, simd_make_float4(0.92f, 0.94f, 0.96f, 0.95f));
        draw_ui_quad(verts, 118.0f, h - 153.0f, 158.0f, 8.0f, simd_make_float4(0.12f, 0.15f, 0.20f, 0.85f));
        draw_ui_quad(verts, 120.0f, h - 151.0f, 154.0f * hp_pct, 4.0f,
                     (hp_pct < 0.35f) ? simd_make_float4(0.902f, 0.078f, 0.078f, 1.0f)
                                      : simd_make_float4(0.96f, 0.96f, 0.98f, 0.95f));

        draw_ui_text(verts, "REACTION", 35.0f, h - 138.0f, 1.6f, simd_make_float4(0.92f, 0.94f, 0.96f, 0.95f));
        draw_ui_quad(verts, 118.0f, h - 137.0f, 158.0f, 8.0f, simd_make_float4(0.12f, 0.15f, 0.20f, 0.85f));
        draw_ui_quad(verts, 120.0f, h - 135.0f, 154.0f * rt_pct, 4.0f, simd_make_float4(0.22f, 0.82f, 1.0f, 0.98f));

        // 5. Top-Right Chapter / Checkpoint / Streaming / Bags / Weapon Panel Backdrop
        float rx = w - 335.0f;
        draw_ui_quad(verts, rx - 14.0f, 18.0f, 332.0f, 122.0f, simd_make_float4(0.04f, 0.06f, 0.09f, 0.72f));
        draw_ui_quad(verts, rx - 14.0f, 18.0f, 332.0f, 3.0f, simd_make_float4(0.902f, 0.078f, 0.078f, 0.95f));

        std::string ch_title = scene.chapter_title.empty() ? "PROLOGUE: THE EDGE" : scene.chapter_title;
        draw_ui_text(verts, ch_title, rx, 30.0f, 2.1f, simd_make_float4(0.95f, 0.18f, 0.18f, 1.0f));

        std::ostringstream ss_cp;
        ss_cp << "CHECKPOINT " << (telemetry.active_checkpoint + 1) << " / "
              << std::max(1, (int)scene.checkpoints.size());
        draw_ui_text(verts, ss_cp.str(), rx, 55.0f, 1.8f, simd_make_float4(0.96f, 0.97f, 0.99f, 0.95f));

        std::ostringstream ss_bags;
        ss_bags << "COURIER BAGS: " << telemetry.bags_collected << " / 3";
        draw_ui_text(verts, ss_bags.str(), rx, 75.0f, 1.8f, simd_make_float4(0.96f, 0.86f, 0.25f, 0.95f));

        std::string wep_str = telemetry.weapon.equipped ? (telemetry.weapon.name + " [" +
                              std::to_string(telemetry.weapon.ammo) + "/" +
                              std::to_string(telemetry.weapon.max_ammo) + "]") : "UNARMED (T/Y GUNS)";
        draw_ui_text(verts, "WEAPON: " + wep_str, rx, 95.0f, 1.8f,
                     telemetry.weapon.equipped ? simd_make_float4(0.95f, 0.22f, 0.22f, 1.0f)
                                               : simd_make_float4(0.82f, 0.86f, 0.92f, 0.92f));

        std::ostringstream ss_str;
        ss_str << "STREAMED SUBLEVELS: " << std::max<int>(telemetry.streamed_sublevel_count, (int)scene.loaded_sublevel_packages.size());
        draw_ui_text(verts, ss_str.str(), rx, 115.0f, 1.7f, simd_make_float4(0.55f, 0.85f, 1.0f, 0.95f));

        // Elevator Transit Indicator when Faith is riding an interactive elevator
        if (telemetry.in_elevator) {
            float ex = (w - 300.0f) * 0.5f;
            float ey = 28.0f;
            draw_ui_quad(verts, ex, ey, 300.0f, 38.0f, simd_make_float4(0.04f, 0.06f, 0.09f, 0.82f));
            draw_ui_quad(verts, ex, ey, 300.0f, 3.0f, simd_make_float4(0.902f, 0.078f, 0.078f, 0.95f));
            draw_ui_text(verts, "ELEVATOR TRANSIT / STREAMING", ex + 18.0f, ey + 8.0f, 1.7f,
                         simd_make_float4(0.96f, 0.97f, 0.99f, 0.98f));
            draw_ui_quad(verts, ex + 18.0f, ey + 24.0f, 264.0f, 6.0f, simd_make_float4(0.16f, 0.20f, 0.26f, 0.9f));
            draw_ui_quad(verts, ex + 18.0f, ey + 24.0f, 264.0f * std::clamp(telemetry.elevator_progress, 0.0f, 1.0f), 6.0f,
                         simd_make_float4(0.902f, 0.078f, 0.078f, 1.0f));
        }

        // 6. Active Subtitle / Tutorial Prompt Banner (Bottom Center)
        std::string prompt = telemetry.active_subtitle.empty()
            ? "[LMB/F] MELEE/FIRE | [RMB/E] DISARM | [T/Y] CYCLE 11 GUNS | [G] DROP | [H] SPAWN SQUAD"
            : telemetry.active_subtitle;

        float banner_w = float(prompt.length()) * 11.0f + 40.0f;
        float banner_x = (w - banner_w) * 0.5f;
        draw_ui_quad(verts, banner_x, h - 42.0f, banner_w, 28.0f, simd_make_float4(0.04f, 0.06f, 0.09f, 0.80f));
        draw_ui_text(verts, prompt, banner_x + 20.0f, h - 35.0f, 1.8f, simd_make_float4(0.98f, 0.98f, 0.98f, 1.0f));
    }
};

// -----------------------------------------------------------------------------
// MetalRenderer Public Interface
// -----------------------------------------------------------------------------
MetalRenderer::MetalRenderer() : impl_(std::make_unique<Impl>()) {}
MetalRenderer::~MetalRenderer() = default;
MetalRenderer::MetalRenderer(MetalRenderer&&) noexcept = default;
MetalRenderer& MetalRenderer::operator=(MetalRenderer&&) noexcept = default;

bool MetalRenderer::init_headless(int width, int height) {
    impl_->width = width;
    impl_->height = height;
    impl_->headless = true;

    impl_->device = MTLCreateSystemDefaultDevice();
    if (!impl_->device) {
        std::cerr << "[MetalRenderer] Failed to acquire default Metal device!" << std::endl;
        return false;
    }

    impl_->command_queue = [impl_->device newCommandQueue];
    if (!impl_->command_queue) return false;

    if (!impl_->compile_shaders()) return false;
    impl_->create_material_defaults();
    impl_->allocate_render_targets();
    impl_->load_character_assets();

    impl_->initialized = true;
    std::cout << "[MetalRenderer] Initialized in Headless Mode (" << width << "x" << height
              << ") on " << [[impl_->device name] UTF8String] << std::endl;
    return true;
}

bool MetalRenderer::init_with_metal_layer(void* ca_metal_layer, int width, int height) {
    if (!ca_metal_layer) return false;

    impl_->width = width;
    impl_->height = height;
    impl_->headless = false;
    impl_->metal_layer = (__bridge CAMetalLayer*)ca_metal_layer;

    impl_->device = impl_->metal_layer.device;
    if (!impl_->device) {
        impl_->device = MTLCreateSystemDefaultDevice();
        impl_->metal_layer.device = impl_->device;
    }

    impl_->metal_layer.pixelFormat = MTLPixelFormatRGBA8Unorm;
    impl_->metal_layer.drawableSize = CGSizeMake(width, height);

    impl_->command_queue = [impl_->device newCommandQueue];
    if (!impl_->command_queue) return false;

    if (!impl_->compile_shaders()) return false;
    impl_->create_material_defaults();
    impl_->allocate_render_targets();
    impl_->load_character_assets();

    impl_->initialized = true;
    std::cout << "[MetalRenderer] Initialized with CAMetalLayer (" << width << "x" << height
              << ") on " << [[impl_->device name] UTF8String] << std::endl;
    return true;
}

void MetalRenderer::resize(int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (impl_->width == width && impl_->height == height) return;

    impl_->width = width;
    impl_->height = height;

    if (impl_->metal_layer) {
        impl_->metal_layer.drawableSize = CGSizeMake(width, height);
    }
    impl_->allocate_render_targets();
}

void MetalRenderer::render_frame(const LevelScene& scene, const PlayerTelemetry& telemetry) {
    if (!impl_->initialized) return;

    @autoreleasepool {
        id<CAMetalDrawable> drawable = nil;
        id<MTLTexture> final_target = impl_->offscreen_color_tex;

        if (!impl_->headless && impl_->metal_layer) {
            drawable = [impl_->metal_layer nextDrawable];
            if (drawable) {
                final_target = drawable.texture;
            }
        }

        id<MTLCommandBuffer> cmd_buffer = [impl_->command_queue commandBuffer];

        // ---------------------------------------------------------------------
        // Main Menu / Load Chapter State: Switch to TdMainMenu.me1 3D City
        // ---------------------------------------------------------------------
        bool in_main_menu = false;
        if (impl_->menu_open) {
            impl_->ensure_main_menu_loaded();
            impl_->main_menu.update_selected_chapter_highlight(impl_->selected_chapter);
            in_main_menu = impl_->main_menu.has_city_scene();
        }
        const LevelScene& active_scene = in_main_menu ? impl_->main_menu.city_scene() : scene;

        // ---------------------------------------------------------------------
        // Camera View & Projection Matrices (Unreal Engine to Metal Canonical)
        // ---------------------------------------------------------------------
        Vec3 cam_pos = telemetry.position + Vec3(0.0f, 0.0f, telemetry.eye_height);
        Rotator rot = Rotator::from_degrees(telemetry.pitch_deg, telemetry.yaw_deg, telemetry.camera_roll_deg);
        float fov_deg = telemetry.fov_deg;
        float near_plane = 5.0f;
        float far_plane = 65000.0f;

        if (in_main_menu) {
            const MenuChapterEntry& cam_ch = impl_->main_menu.get_chapter(impl_->selected_chapter);
            cam_pos = cam_ch.camera_location;
            rot = cam_ch.camera_rotation;
            fov_deg = 90.0f;
            near_plane = 10.0f;
            far_plane = 400000.0f;
        }

        Vec3 fwd = rot.forward();
        Vec3 right = rot.right();
        Vec3 up = rot.up();
        Vec3 target = cam_pos + fwd * 100.0f;

        Mat4 view = Mat4::look_at(cam_pos, target, up);
        float aspect = float(impl_->width) / float(impl_->height);
        float fov_h_rad = fov_deg * DEG2RAD;
        float fov_y_rad = 2.0f * std::atan(std::tan(fov_h_rad * 0.5f) / aspect);
        Mat4 proj = Mat4::perspective(fov_y_rad, aspect, near_plane, far_plane);
        Mat4 vp = proj * view;

        // Viewmodel camera matrix (DefaultGame.ini Model1pFOV = 100 horizontal)
        Mat4 vm_view = Mat4::look_at(Vec3(0, 0, 0), Vec3(0, 100.0f, 0), Vec3(0, 0, 1));
        float vm_fov_y_rad = 2.0f * std::atan(std::tan(100.0f * DEG2RAD * 0.5f) / aspect);
        Mat4 vm_proj = Mat4::perspective(vm_fov_y_rad, aspect, 1.0f, 500.0f);
        Mat4 vm_vp = vm_proj * vm_view;

        // Pack frame uniforms
        FrameUniformsGPU uniforms{};
        std::memcpy(&uniforms.view_proj, vp.m, sizeof(float) * 16);
        Mat4 identity = Mat4::identity();
        std::memcpy(&uniforms.model, identity.m, sizeof(float) * 16);

        uniforms.camera_pos = simd_make_float3(cam_pos.x, cam_pos.y, cam_pos.z);
        uniforms.sim_time = telemetry.sim_time;

        Vec3 sun_d = active_scene.sun_direction.normalized();
        uniforms.sun_dir = simd_make_float3(sun_d.x, sun_d.y, sun_d.z);
        uniforms.sun_color = simd_make_float3(active_scene.sun_color.x, active_scene.sun_color.y, active_scene.sun_color.z);
        uniforms.sky_color = simd_make_float3(active_scene.sky_upper_color.x, active_scene.sky_upper_color.y, active_scene.sky_upper_color.z);
        uniforms.ground_color = simd_make_float3(active_scene.sky_lower_color.x, active_scene.sky_lower_color.y, active_scene.sky_lower_color.z);
        uniforms.speed_2d = impl_->menu_open ? 0.0f : telemetry.speed_2d;
        uniforms.reaction_active = (!impl_->menu_open && telemetry.reaction_active) ? 1.0f : 0.0f;
        uniforms.health = impl_->menu_open ? 100.0f : telemetry.health;
        uniforms.exposure = 1.0f;
        uniforms.actor_tint = simd_make_float3(1.0f, 1.0f, 1.0f);
        uniforms.runner_vision_strength = 0.0f;
        uniforms.is_runner_vision = 0.0f;

        uniforms.cam_forward = simd_make_float3(fwd.x, fwd.y, fwd.z);
        uniforms.fov_tan = std::tan(fov_y_rad * 0.5f);
        uniforms.cam_right = simd_make_float3(right.x, right.y, right.z);
        uniforms.aspect = aspect;
        uniforms.cam_up = simd_make_float3(up.x, up.y, up.z);

        // Directional sun shadow cascades (renderer/sun_shadow.hpp): orthographic squares centred on the
        // camera and snapped to whole texels in light space, so neither map slides across the world by part
        // of a texel as the camera moves, and turning the camera changes nothing. The main menu instead
        // frames TdMainMenu's miniature City of Glass with one fixed square and has no far cascade.
        const Mat4 sun_near_vp = in_main_menu ? sun_shadow_menu_view_proj(sun_d) : sun_shadow_view_proj(sun_d, cam_pos);
        const Mat4 sun_far_vp = in_main_menu ? sun_near_vp : sun_shadow_far_view_proj(sun_d, cam_pos);
        std::memcpy(&uniforms.sun_view_proj, sun_near_vp.m, sizeof(float) * 16);
        std::memcpy(&uniforms.sun_view_proj_far, sun_far_vp.m, sizeof(float) * 16);
        uniforms.mod_shadow_color = simd_make_float3(active_scene.mod_shadow_color.x,
                                                     active_scene.mod_shadow_color.y,
                                                     active_scene.mod_shadow_color.z);
        uniforms.shadow_enabled = in_main_menu ? 2.0f : 1.0f;

        const int frame_slot = static_cast<int>(impl_->frame_index % MetalRenderer::Impl::kMaxFramesInFlight);
        if (!impl_->headless && impl_->in_flight_sem) {
            dispatch_semaphore_wait(impl_->in_flight_sem, DISPATCH_TIME_FOREVER);
        }
        impl_->dyn_vertex_cursor[frame_slot] = 0;

        // ---------------------------------------------------------------------
        // Mirror's Edge materials: make the scene's material library resident
        // (texture upload + MSL compile happen once per library) and find out
        // whether this frame needs a translucency pass / opaque scene copies.
        // ---------------------------------------------------------------------
        impl_->sync_material_library(active_scene.materials);

        auto bind_vertex_bytes_or_buffer = [&](id<MTLRenderCommandEncoder> encoder, const void* data, size_t length, NSUInteger index) {
            if (length <= MetalRenderer::Impl::kMaxInlineVertexBytes) {
                [encoder setVertexBytes:data length:length atIndex:index];
            } else {
                id<MTLBuffer> buf = impl_->acquire_dynamic_vertex_buffer(frame_slot, data, length);
                [encoder setVertexBuffer:buf offset:0 atIndex:index];
            }
        };

        // Rebuild GPU vertex buffer cache only when scene meshes change
        size_t total_scene_verts = 0;
        for (const auto& m : active_scene.meshes) total_scene_verts += m.vertices.size();

        const bool meshes_changed = (active_scene.map_name != impl_->cached_map_name ||
                                     total_scene_verts != impl_->cached_total_verts ||
                                     impl_->cached_mesh_buffers.size() != active_scene.meshes.size());
        if (meshes_changed) {
            impl_->cached_map_name = active_scene.map_name;
            impl_->cached_total_verts = total_scene_verts;
            impl_->cached_mesh_buffers.clear();
            ++impl_->scene_generation;  // the far shadow cascade must be redrawn from the new geometry
            for (const auto& m : active_scene.meshes) {
                if (m.vertices.empty()) {
                    impl_->cached_mesh_buffers.push_back(nil);
                } else {
                    id<MTLBuffer> b = [impl_->device newBufferWithBytes:m.vertices.data()
                                                                 length:m.vertices.size() * sizeof(Vertex)
                                                                options:MTLResourceStorageModeShared];
                    impl_->cached_mesh_buffers.push_back(b);
                }
            }
        }

        if (meshes_changed || impl_->cached_section_mat_lib != impl_->mat_lib ||
            impl_->cached_section_flags.size() != active_scene.meshes.size()) {
            impl_->cached_section_mat_lib = impl_->mat_lib;
            impl_->cached_has_translucent = false;
            impl_->cached_needs_scene_copies = false;
            impl_->cached_section_flags.assign(active_scene.meshes.size(), {});
            for (size_t i = 0; i < active_scene.meshes.size(); ++i) {
                const auto& mesh = active_scene.meshes[i];
                auto& sflags = impl_->cached_section_flags[i];
                sflags.assign(mesh.sections.size(), 0u);
                for (size_t si = 0; si < mesh.sections.size(); ++si) {
                    const auto& s = mesh.sections[si];
                    const MaterialShader* sh = nullptr;
                    const SceneMaterial* m = nullptr;
                    id<MTLRenderPipelineState> ps = impl_->section_pipeline(s, &sh, &m);
                    uint8_t fl = MetalRenderer::Impl::kSecShadowCaster;
                    if (sh && (mat_blend_is_translucent(sh->blend) || sh->lighting == MatLightingModel::Unlit)) {
                        fl &= ~MetalRenderer::Impl::kSecShadowCaster;
                    }
                    if (m && (m->name.find("Skydome") != std::string::npos ||
                              m->name.find("skydome") != std::string::npos)) {
                        fl &= ~MetalRenderer::Impl::kSecShadowCaster;
                    }
                    if (ps && sh && mat_blend_is_translucent(sh->blend)) {
                        fl |= MetalRenderer::Impl::kSecTranslucent;
                        impl_->cached_has_translucent = true;
                        if (sh->uses_scene_color || sh->uses_scene_depth) {
                            impl_->cached_needs_scene_copies = true;
                        }
                    }
                    sflags[si] = fl;
                }
            }
        }
        const bool has_translucent = impl_->cached_has_translucent;
        const bool needs_scene_copies = impl_->cached_needs_scene_copies;

        // Pose active SWAT/CPF enemies once per frame and share between Pass 0 (Shadow) and Pass 1 (World).
        // Enemies are independent (evaluate_enemy_swat_indexed is const and keeps its scratch buffers thread_local),
        // so they are posed in parallel. An enemy's triangle list (exactly evaluate_enemy_swat()'s) is assembled,
        // straight into this frame slot's GPU buffer for it, only if its posed bounds can reach the camera view or
        // the near shadow cascade. The GPU would clip an enemy outside both away completely, so skipping it leaves
        // both passes' output unchanged.
        const bool need_enemies = impl_->anim_system.is_loaded() && !active_scene.enemies.empty() &&
                                  (!impl_->menu_open || (impl_->shadow_depth_tex && impl_->shadow_pipeline));
        if (need_enemies) {
            const size_t enemy_count = active_scene.enemies.size();
            if (impl_->frame_enemy_draws.size() < enemy_count) {
                impl_->frame_enemy_draws.resize(enemy_count);
            }
            auto& slot_enemy_buffers = impl_->enemy_vertex_buffers[frame_slot];
            if (slot_enemy_buffers.size() < enemy_count) {
                slot_enemy_buffers.resize(enemy_count);
            }
            MetalRenderer::Impl* const impl = impl_.get();
            const std::vector<EnemyBot>* const bots = &active_scene.enemies;
            std::vector<id<MTLBuffer>>* const gpu_meshes = &slot_enemy_buffers;
            const ClipVolume view_volume(vp);
            const ClipVolume shadow_volume(sun_near_vp);
            const bool shadow_pass = impl_->shadow_depth_tex && impl_->shadow_pipeline;
            const float sim_time = telemetry.sim_time;
            const bool reaction_active = telemetry.reaction_active;
            const bool menu_open = impl_->menu_open;
            // Each worker touches only element ei of frame_enemy_draws / slot_enemy_buffers (both sized above).
            auto pose_enemy = [impl, bots, gpu_meshes, &view_volume, &shadow_volume, cam_pos, shadow_pass, sim_time,
                               reaction_active, menu_open](size_t ei) {
                const auto& bot = (*bots)[ei];
                auto& draw = impl->frame_enemy_draws[ei];
                draw = MetalRenderer::Impl::EnemyFrameDraw{};
                if (!bot.alive && menu_open) return;

                thread_local std::vector<Vertex> posed;
                posed.resize(impl->anim_system.enemy_swat_max_vertices());
                const AnimSystem::EnemySwatDraw mesh =
                    impl->anim_system.evaluate_enemy_swat_indexed(bot, sim_time, reaction_active, posed.data());
                if (mesh.index_count == 0) return;

                // World-space bounding sphere of the posed vertices (the triangle list is built from them alone).
                // fmin/fmax skip NaN operands, so a NaN coordinate instead makes the bounds NaN explicitly, and
                // ClipVolume never rejects a NaN sphere.
                float lox = posed[0].position.x, loy = posed[0].position.y, loz = posed[0].position.z;
                float hix = lox, hiy = loy, hiz = loz;
                bool has_nan = false;
                for (size_t v = 0; v < mesh.vertex_count; ++v) {
                    const Vec3& p = posed[v].position;
                    lox = std::fmin(lox, p.x);
                    loy = std::fmin(loy, p.y);
                    loz = std::fmin(loz, p.z);
                    hix = std::fmax(hix, p.x);
                    hiy = std::fmax(hiy, p.y);
                    hiz = std::fmax(hiz, p.z);
                    has_nan |= (p.x != p.x) | (p.y != p.y) | (p.z != p.z);
                }
                if (has_nan) lox = NAN;
                const Vec3 lo(lox, loy, loz);
                const Vec3 hi(hix, hiy, hiz);
                const Mat4 bot_model = Mat4::translation(bot.position) * Mat4::rotation_z(bot.yaw_deg * DEG2RAD);
                const Vec3 center = bot_model.transform_point((lo + hi) * 0.5f);
                const float radius = (hi - lo).length() * 0.5f;
                // Far beyond the rounding of the GPU's float transforms, which stays well under 1e-5 of the
                // coordinates' magnitudes.
                const double margin = 16.0 + 1e-4 * (static_cast<double>(center.length()) + cam_pos.length());
                draw.in_view = !menu_open && view_volume.may_cover(center, radius, margin);  // world pass: gameplay only
                draw.in_shadow = shadow_pass && bot.alive && shadow_volume.may_cover(center, radius, margin);
                if (!draw.in_view && !draw.in_shadow) return;

                const std::vector<uint32_t>& corners = impl->anim_system.enemy_swat_index_lists()[mesh.index_list];
                const size_t bytes = mesh.index_count * sizeof(Vertex);
                @autoreleasepool {
                    id<MTLBuffer> buf = (*gpu_meshes)[ei];
                    if (!buf || buf.length < bytes) {
                        buf = [impl->device newBufferWithLength:MetalRenderer::Impl::dynamic_buffer_capacity(bytes)
                                                        options:MTLResourceStorageModeShared];
                        (*gpu_meshes)[ei] = buf;
                    }
                    Vertex* out = static_cast<Vertex*>(buf.contents);
                    for (size_t k = 0; k < mesh.index_count; ++k) out[k] = posed[corners[k]];
                }
                draw.corner_count = mesh.index_count;
            };
            const auto* const pose_enemy_fn = &pose_enemy;
            dispatch_apply(enemy_count, dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^(size_t ei) {
                (*pose_enemy_fn)(ei);
            });
        }
        // Draws enemy ei's triangle list from this frame slot's buffer; it must be in_view or in_shadow.
        auto draw_enemy_mesh = [&](id<MTLRenderCommandEncoder> encoder, size_t ei) {
            [encoder setVertexBuffer:impl_->enemy_vertex_buffers[frame_slot][ei] offset:0 atIndex:0];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                        vertexStart:0
                        vertexCount:impl_->frame_enemy_draws[ei].corner_count];
        };

        auto section_in_range = [](const MeshBuffer& mesh, const MeshSection& s) {
            return s.vertex_count > 0 &&
                   static_cast<size_t>(s.first_vertex) + static_cast<size_t>(s.vertex_count) <= mesh.vertices.size();
        };

        // Moving elevator parts (InterpActors driven by the elevator matinees) are baked at their
        // initial pose; they are drawn translated by the part's current matinee offset. Sets
        // uniforms.model for scene mesh i and returns true when it is not the identity.
        auto apply_scene_mesh_model = [&](size_t i) -> bool {
            const MeshBuffer& mb = active_scene.meshes[i];
            if (mb.elevator >= 0 && static_cast<size_t>(mb.elevator) < active_scene.elevators.size()) {
                const auto& parts = active_scene.elevators[static_cast<size_t>(mb.elevator)].parts;
                if (mb.elevator_part >= 0 && static_cast<size_t>(mb.elevator_part) < parts.size()) {
                    Mat4 part_model = Mat4::translation(parts[static_cast<size_t>(mb.elevator_part)].offset);
                    std::memcpy(&uniforms.model, part_model.m, sizeof(float) * 16);
                    return true;
                }
            }
            if (mb.barge_door >= 0 && static_cast<size_t>(mb.barge_door) < active_scene.barge_doors.size()) {
                const Mat4& door_model = active_scene.barge_doors[static_cast<size_t>(mb.barge_door)].model_matrix;
                std::memcpy(&uniforms.model, door_model.m, sizeof(float) * 16);
                return true;
            }
            std::memcpy(&uniforms.model, identity.m, sizeof(float) * 16);
            return false;
        };

        // ---------------------------------------------------------------------
        // Pass 0: Real-Time Directional Sun Shadow Cascades (2 x 4096x4096 Depth)
        // ---------------------------------------------------------------------
        if (impl_->shadow_depth_tex && impl_->shadow_pipeline) {
            // Renders one cascade into its slice of the shadow map array. `dynamic_casters` adds the moving
            // elevator parts, the barge doors and the enemies; without it only static level geometry is drawn.
            auto encode_shadow_cascade = [&](NSUInteger slice, const Mat4& cascade_vp, float slope_bias_cap_uu,
                                             float depth_range_uu, bool dynamic_casters) {
                MTLRenderPassDescriptor* shadowPass = [MTLRenderPassDescriptor renderPassDescriptor];
                shadowPass.depthAttachment.texture = impl_->shadow_depth_tex;
                shadowPass.depthAttachment.slice = slice;
                shadowPass.depthAttachment.loadAction = MTLLoadActionClear;
                shadowPass.depthAttachment.storeAction = MTLStoreActionStore;
                shadowPass.depthAttachment.clearDepth = 1.0;

                // shadow_vertex projects with uniforms.sun_view_proj.
                const simd_float4x4 near_vp_saved = uniforms.sun_view_proj;
                std::memcpy(&uniforms.sun_view_proj, cascade_vp.m, sizeof(float) * 16);

                id<MTLRenderCommandEncoder> shEnc = [cmd_buffer renderCommandEncoderWithDescriptor:shadowPass];
                [shEnc setViewport:(MTLViewport){0.0, 0.0, (double)kSunShadowMapSize, (double)kSunShadowMapSize, 0.0, 1.0}];
                [shEnc setRenderPipelineState:impl_->shadow_pipeline];
                [shEnc setDepthStencilState:impl_->depth_write_state];
                // Slope-scaled bias, capped in UU (the cap is in map depth units: depth_range_uu UU each).
                [shEnc setDepthBias:0.0012f slopeScale:1.75f clamp:slope_bias_cap_uu / depth_range_uu];
                [shEnc setCullMode:MTLCullModeNone];
                [shEnc setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1];

                bool sh_prev_moved = false;
                for (size_t i = 0; i < active_scene.meshes.size(); ++i) {
                    const auto& mesh = active_scene.meshes[i];
                    if (mesh.vertices.empty() || !impl_->cached_mesh_buffers[i]) continue;
                    if (!dynamic_casters && (mesh.elevator >= 0 || mesh.barge_door >= 0)) continue;
                    bool moved = apply_scene_mesh_model(i);
                    if (moved || sh_prev_moved) [shEnc setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1];
                    sh_prev_moved = moved;
                    [shEnc setVertexBuffer:impl_->cached_mesh_buffers[i] offset:0 atIndex:0];
                    if (mesh.sections.empty()) {
                        [shEnc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:mesh.vertices.size()];
                        continue;
                    }
                    const auto& sflags = impl_->cached_section_flags[i];
                    for (size_t si = 0; si < mesh.sections.size(); ++si) {
                        const auto& s = mesh.sections[si];
                        if (!section_in_range(mesh, s)) continue;
                        if ((sflags[si] & MetalRenderer::Impl::kSecShadowCaster) == 0) continue;
                        [shEnc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:s.first_vertex vertexCount:s.vertex_count];
                    }
                }
                std::memcpy(&uniforms.model, identity.m, sizeof(float) * 16);

                if (dynamic_casters && need_enemies) {
                    // Only the near cascade has dynamic casters; in_shadow was tested against its matrix (and
                    // implies a live, posed enemy).
                    for (size_t ei = 0; ei < active_scene.enemies.size(); ++ei) {
                        if (!impl_->frame_enemy_draws[ei].in_shadow) continue;
                        const auto& bot = active_scene.enemies[ei];
                        Mat4 bot_model = Mat4::translation(bot.position) * Mat4::rotation_z(bot.yaw_deg * DEG2RAD);
                        std::memcpy(&uniforms.model, bot_model.m, sizeof(float) * 16);
                        [shEnc setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1];
                        draw_enemy_mesh(shEnc, ei);
                    }
                    std::memcpy(&uniforms.model, identity.m, sizeof(float) * 16);
                }

                [shEnc endEncoding];
                uniforms.sun_view_proj = near_vp_saved;
            };

            // Far cascade: static geometry only, so it stays valid until its coarsely snapped square steps
            // (the camera moved ~kSunShadowFarStep), the sun direction changes or the scene is rebuilt.
            // The main menu has no far cascade: one fixed square frames its miniature city.
            if (!in_main_menu &&
                (!impl_->shadow_far_valid || impl_->shadow_far_generation != impl_->scene_generation ||
                 std::memcmp(impl_->shadow_far_vp, sun_far_vp.m, sizeof(impl_->shadow_far_vp)) != 0)) {
                encode_shadow_cascade(kSunShadowFarSlice, sun_far_vp, kSunShadowFarSlopeBiasCap, kSunShadowFarDepthRange,
                                      /*dynamic_casters=*/false);
                std::memcpy(impl_->shadow_far_vp, sun_far_vp.m, sizeof(impl_->shadow_far_vp));
                impl_->shadow_far_generation = impl_->scene_generation;
                impl_->shadow_far_valid = true;
            }
            // Near cascade (or the menu's fixed square): every caster, every frame.
            encode_shadow_cascade(kSunShadowNearSlice, sun_near_vp, kSunShadowSlopeBiasCap, kSunShadowDepthRange,
                                  /*dynamic_casters=*/true);
        }

        // ---------------------------------------------------------------------
        // Pass 1: 3D Scene Geometry & Sky -> HDR Texture
        // ---------------------------------------------------------------------
        MTLRenderPassDescriptor* scenePass = [MTLRenderPassDescriptor renderPassDescriptor];
        scenePass.colorAttachments[0].texture = impl_->scene_hdr_tex;
        scenePass.colorAttachments[0].loadAction = MTLLoadActionClear;
        scenePass.colorAttachments[0].storeAction = MTLStoreActionStore;
        scenePass.colorAttachments[0].clearColor = MTLClearColorMake(0.65, 0.82, 0.98, 1.0);

        scenePass.depthAttachment.texture = impl_->offscreen_depth_tex;
        scenePass.depthAttachment.loadAction = MTLLoadActionClear;
        scenePass.depthAttachment.storeAction = MTLStoreActionStore;
        scenePass.depthAttachment.clearDepth = 1.0;

        id<MTLRenderCommandEncoder> enc = [cmd_buffer renderCommandEncoderWithDescriptor:scenePass];
        [enc setViewport:(MTLViewport){0.0, 0.0, (double)impl_->width, (double)impl_->height, 0.0, 1.0}];
        [enc setFragmentTexture:impl_->shadow_depth_tex atIndex:matbind::kShadowMapTexture];

        // A. Draw Sky Dome (TdDirHaze + Distant City Skyline)
        [enc setRenderPipelineState:impl_->sky_pipeline];
        [enc setDepthStencilState:impl_->depth_disabled_state];
        [enc setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];

        // B. Draw World Meshes (BasePass + Beast Radiosity)
        [enc setViewport:(MTLViewport){0.0, 0.0, (double)impl_->width, (double)impl_->height, 0.05, 1.0}];
        [enc setRenderPipelineState:impl_->world_pipeline];
        [enc setDepthStencilState:impl_->depth_write_state];

        auto bind_world_char_wep_textures = [&](const std::string& wname) {
            id<MTLTexture> t_swat   = impl_->swat_d_gpu_tex ? impl_->swat_d_gpu_tex : impl_->tex_default_white;
            id<MTLTexture> t_swat_s = impl_->swat_s_gpu_tex ? impl_->swat_s_gpu_tex : impl_->tex_default_black;
            id<MTLTexture> t_swat_n = impl_->swat_n_gpu_tex ? impl_->swat_n_gpu_tex : impl_->tex_default_flat_normal;
            id<MTLTexture> t_wep_d  = impl_->tex_default_white;
            id<MTLTexture> t_wep_s  = impl_->tex_default_black;
            auto it_w = impl_->weapon_gpu_textures.find(wname);
            if (it_w == impl_->weapon_gpu_textures.end() && !impl_->weapon_gpu_textures.empty()) {
                it_w = impl_->weapon_gpu_textures.find("Colt1911");
            }
            if (it_w != impl_->weapon_gpu_textures.end()) {
                if (it_w->second.diffuse)  t_wep_d = it_w->second.diffuse;
                if (it_w->second.specular) t_wep_s = it_w->second.specular;
            }
            id<MTLTexture> t_ammo = impl_->ammo_d_gpu_tex ? impl_->ammo_d_gpu_tex : impl_->tex_default_white;
            [enc setFragmentTexture:t_swat   atIndex:0];
            [enc setFragmentTexture:t_wep_d  atIndex:1];
            [enc setFragmentTexture:t_wep_s  atIndex:2];
            [enc setFragmentTexture:t_ammo   atIndex:3];
            [enc setFragmentTexture:t_swat_s atIndex:4];
            [enc setFragmentTexture:t_swat_n atIndex:5];
            [enc setFragmentSamplerState:impl_->mat_samplers[0][0] atIndex:0];
        };
        bind_world_char_wep_textures("Colt1911");

        // Binds mesh i's vertex buffer + frame uniforms (incl. its model matrix) on the current encoder.
        auto bind_scene_mesh = [&](size_t i) {
            uniforms.is_runner_vision = active_scene.meshes[i].is_runner_vision ? 1.0f : 0.0f;
            apply_scene_mesh_model(i);
            [enc setVertexBuffer:impl_->cached_mesh_buffers[i] offset:0 atIndex:0];
            [enc setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1];
            [enc setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
        };

        if (!active_scene.meshes.empty()) {
            [enc setFrontFacingWinding:impl_->mat_front_winding];
            std::string active_mi_tag = in_main_menu
                ? impl_->main_menu.get_chapter(impl_->selected_chapter).material_instance_tag
                : "";
            for (char& c : active_mi_tag) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            for (size_t i = 0; i < active_scene.meshes.size(); ++i) {
                const auto& mesh = active_scene.meshes[i];
                if (mesh.vertices.empty() || !impl_->cached_mesh_buffers[i]) continue;
                bind_scene_mesh(i);
                if (mesh.sections.empty()) {
                    [enc setRenderPipelineState:impl_->world_pipeline];
                    [enc setCullMode:MTLCullModeNone];
                    bind_world_char_wep_textures("Colt1911");
                    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:mesh.vertices.size()];
                    continue;
                }
                // Opaque + masked material sections (UE3 base pass). Translucent ones are deferred.
                for (const auto& s : mesh.sections) {
                    if (!section_in_range(mesh, s)) continue;
                    const MaterialShader* sh = nullptr;
                    const SceneMaterial* m = nullptr;
                    id<MTLRenderPipelineState> ps = impl_->section_pipeline(s, &sh, &m);
                    if (ps && mat_blend_is_translucent(sh->blend)) continue;
                    if (ps) {
                        [enc setRenderPipelineState:ps];
                        [enc setCullMode:(impl_->mat_cull_enabled && !sh->two_sided && !in_main_menu) ? MTLCullModeBack : MTLCullModeNone];
                        impl_->bind_material(enc, *m, *sh);
                        if (in_main_menu && sh->num_uniforms > 0 && !active_mi_tag.empty()) {
                            std::string mname = m->name;
                            for (char& c : mname) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                            if (mname.find(active_mi_tag) != std::string::npos) {
                                std::vector<std::array<float, 4>> dyn_u = m->uniforms;
                                dyn_u.resize(static_cast<size_t>(sh->num_uniforms), {0.0f, 0.0f, 0.0f, 0.0f});
                                dyn_u[0][0] = 1.0f; // UE3 MaterialInstanceConstant scalar parameter 'Selected' = 1.0
                                [enc setFragmentBytes:dyn_u.data()
                                               length:static_cast<NSUInteger>(sh->num_uniforms) * 16
                                              atIndex:matbind::kMaterialBuffer];
                            }
                        }
                    } else {
                        [enc setRenderPipelineState:impl_->world_pipeline];
                        [enc setCullMode:MTLCullModeNone];
                        bind_world_char_wep_textures("Colt1911");
                    }
                    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:s.first_vertex vertexCount:s.vertex_count];
                }
            }
            [enc setCullMode:MTLCullModeNone];
            std::memcpy(&uniforms.model, identity.m, sizeof(float) * 16);
            uniforms.is_runner_vision = 0.0f;
        }

        // B2. Render 3D Articulated KrugerSec / CPF SWAT Enemies & 3D Weapons/Tracers (only during gameplay)
        // (the material sections above leave their own pipeline/depth state bound)
        [enc setRenderPipelineState:impl_->world_pipeline];
        [enc setDepthStencilState:impl_->depth_write_state];
        [enc setFragmentTexture:impl_->shadow_depth_tex atIndex:matbind::kShadowMapTexture];
        bind_world_char_wep_textures("Colt1911");
        if (!impl_->menu_open && need_enemies) {
            for (size_t ei = 0; ei < active_scene.enemies.size(); ++ei) {
                if (!impl_->frame_enemy_draws[ei].in_view) continue;
                const auto& bot = active_scene.enemies[ei];
                bind_world_char_wep_textures(bot.weapon_name);
                Mat4 bot_model = Mat4::translation(bot.position) * Mat4::rotation_z(bot.yaw_deg * DEG2RAD);
                std::memcpy(&uniforms.model, bot_model.m, sizeof(float) * 16);
                uniforms.is_runner_vision = 0.0f;
                uniforms.actor_tint = simd_make_float3(1.0f, 1.0f, 1.0f);
                [enc setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1];
                [enc setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
                draw_enemy_mesh(enc, ei);
            }
            std::memcpy(&uniforms.model, identity.m, sizeof(float) * 16);
        }

        // B2a. Render 3D Dropped Weapons on Ground & 3D Bullet Tracers / Impact Sparks
        if (!impl_->menu_open && impl_->anim_system.is_loaded() &&
            (!active_scene.dropped_weapons.empty() || !active_scene.active_tracers.empty())) {
            std::string pickup_wname = !active_scene.dropped_weapons.empty()
                                           ? active_scene.dropped_weapons.front().weapon_name
                                           : "Colt1911";
            bind_world_char_wep_textures(pickup_wname);
            std::vector<Vertex> combat_fx_verts;
            std::vector<Vertex> combat_rv_verts;
            impl_->anim_system.evaluate_combat_world_fx(active_scene, telemetry.sim_time, combat_fx_verts, combat_rv_verts);
            if (!combat_fx_verts.empty()) {
                std::memcpy(&uniforms.model, identity.m, sizeof(float) * 16);
                uniforms.is_runner_vision = 0.0f;
                uniforms.actor_tint = simd_make_float3(1.0f, 1.0f, 1.0f);
                bind_vertex_bytes_or_buffer(enc, combat_fx_verts.data(),
                                            combat_fx_verts.size() * sizeof(Vertex), 0);
                [enc setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1];
                [enc setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
                [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:combat_fx_verts.size()];
            }
            if (!combat_rv_verts.empty()) {
                std::memcpy(&uniforms.model, identity.m, sizeof(float) * 16);
                uniforms.is_runner_vision = 1.0f;
                uniforms.actor_tint = simd_make_float3(1.0f, 1.0f, 1.0f);
                bind_vertex_bytes_or_buffer(enc, combat_rv_verts.data(),
                                            combat_rv_verts.size() * sizeof(Vertex), 0);
                [enc setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1];
                [enc setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
                [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:combat_rv_verts.size()];
                uniforms.is_runner_vision = 0.0f;
            }
        }

        // B3. Translucent / additive / modulated materials (UE3 translucency pass): drawn after
        // all opaque geometry, depth-tested without depth writes. Materials that read the scene
        // (SceneTexture, DestColor, DepthBiasedAlpha) sample copies of the opaque scene.
        if (has_translucent) {
            if (needs_scene_copies) {
                [enc endEncoding];
                id<MTLBlitCommandEncoder> blit = [cmd_buffer blitCommandEncoder];
                [blit copyFromTexture:impl_->scene_hdr_tex toTexture:impl_->scene_color_copy];
                [blit copyFromTexture:impl_->offscreen_depth_tex toTexture:impl_->scene_depth_copy];
                [blit endEncoding];

                MTLRenderPassDescriptor* transPass = [MTLRenderPassDescriptor renderPassDescriptor];
                transPass.colorAttachments[0].texture = impl_->scene_hdr_tex;
                transPass.colorAttachments[0].loadAction = MTLLoadActionLoad;
                transPass.colorAttachments[0].storeAction = MTLStoreActionStore;
                transPass.depthAttachment.texture = impl_->offscreen_depth_tex;
                transPass.depthAttachment.loadAction = MTLLoadActionLoad;
                transPass.depthAttachment.storeAction = MTLStoreActionStore;
                enc = [cmd_buffer renderCommandEncoderWithDescriptor:transPass];
                [enc setViewport:(MTLViewport){0.0, 0.0, (double)impl_->width, (double)impl_->height, 0.05, 1.0}];
                [enc setFragmentTexture:impl_->shadow_depth_tex atIndex:matbind::kShadowMapTexture];
            }
            [enc setDepthStencilState:impl_->depth_test_only_state];
            [enc setFrontFacingWinding:impl_->mat_front_winding];
            for (size_t i = 0; i < active_scene.meshes.size(); ++i) {
                const auto& mesh = active_scene.meshes[i];
                if (mesh.vertices.empty() || mesh.sections.empty() || !impl_->cached_mesh_buffers[i]) continue;
                bool mesh_bound = false;
                for (const auto& s : mesh.sections) {
                    if (!section_in_range(mesh, s)) continue;
                    const MaterialShader* sh = nullptr;
                    const SceneMaterial* m = nullptr;
                    id<MTLRenderPipelineState> ps = impl_->section_pipeline(s, &sh, &m);
                    if (!ps || !mat_blend_is_translucent(sh->blend)) continue;
                    if (!mesh_bound) {
                        bind_scene_mesh(i);
                        mesh_bound = true;
                    }
                    [enc setRenderPipelineState:ps];
                    [enc setCullMode:(impl_->mat_cull_enabled && !sh->two_sided) ? MTLCullModeBack : MTLCullModeNone];
                    impl_->bind_material(enc, *m, *sh);
                    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:s.first_vertex vertexCount:s.vertex_count];
                }
            }
            [enc setCullMode:MTLCullModeNone];
            std::memcpy(&uniforms.model, identity.m, sizeof(float) * 16);
        }

        // C. Draw First-Person Faith Viewmodel (CH_Faith_1P in DPG_Foreground depth range [0.0, 0.05])
        const bool cutscene_active = (impl_->cutscene_player != nullptr && impl_->cutscene_player->is_playing());
        const bool bink_video_active = (cutscene_active && impl_->cutscene_player->get_mode() == ECutsceneMode::BinkVideo);
        if (!impl_->menu_open && !bink_video_active) {
            impl_->build_faith_viewmodel(telemetry);
            if (!impl_->faith_viewmodel_mesh.empty()) {
                [enc setViewport:(MTLViewport){0.0, 0.0, (double)impl_->width, (double)impl_->height, 0.0, 0.05}];
                [enc setRenderPipelineState:impl_->viewmodel_pipeline];
                [enc setDepthStencilState:impl_->depth_write_state];
                std::memcpy(&uniforms.view_proj, vm_vp.m, sizeof(float) * 16);
                std::memcpy(&uniforms.model, identity.m, sizeof(float) * 16);
                uniforms.camera_pos = simd_make_float3(0.0f, 0.0f, 0.0f);
                uniforms.is_runner_vision = 0.0f;
                uniforms.actor_tint = simd_make_float3(1.0f, 1.0f, 1.0f);

                id<MTLTexture> t_skin  = impl_->vm_skin_gpu_tex  ? impl_->vm_skin_gpu_tex  : impl_->tex_default_white;
                id<MTLTexture> t_glove = impl_->vm_glove_gpu_tex ? impl_->vm_glove_gpu_tex : impl_->tex_default_white;
                id<MTLTexture> t_lower = impl_->vm_lower_gpu_tex ? impl_->vm_lower_gpu_tex : impl_->tex_default_white;
                id<MTLTexture> t_wep_d = impl_->tex_default_white;
                id<MTLTexture> t_wep_s = impl_->tex_default_black;
                id<MTLTexture> t_wep_n = impl_->tex_default_flat_normal;
                id<MTLTexture> t_wep_m = impl_->tex_default_white;
                auto it_w = impl_->weapon_gpu_textures.find(telemetry.weapon.name);
                if (it_w == impl_->weapon_gpu_textures.end() && !impl_->weapon_gpu_textures.empty()) {
                    it_w = impl_->weapon_gpu_textures.find("Colt1911");
                }
                if (it_w != impl_->weapon_gpu_textures.end()) {
                    if (it_w->second.diffuse)  t_wep_d = it_w->second.diffuse;
                    if (it_w->second.specular) t_wep_s = it_w->second.specular;
                    if (it_w->second.normal)   t_wep_n = it_w->second.normal;
                    if (it_w->second.mask)     t_wep_m = it_w->second.mask;
                }
                id<MTLTexture> t_ammo = impl_->ammo_d_gpu_tex ? impl_->ammo_d_gpu_tex : impl_->tex_default_white;

                [enc setFragmentTexture:t_skin  atIndex:0];
                [enc setFragmentTexture:t_glove atIndex:1];
                [enc setFragmentTexture:t_lower atIndex:2];
                [enc setFragmentTexture:t_wep_d atIndex:3];
                [enc setFragmentTexture:t_wep_s atIndex:4];
                [enc setFragmentTexture:t_wep_n atIndex:5];
                [enc setFragmentTexture:t_ammo  atIndex:6];
                [enc setFragmentTexture:t_wep_m atIndex:7];
                [enc setFragmentSamplerState:impl_->mat_samplers[0][0] atIndex:0];

                bind_vertex_bytes_or_buffer(enc, impl_->faith_viewmodel_mesh.data(),
                                            impl_->faith_viewmodel_mesh.size() * sizeof(Vertex), 0);
                [enc setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1];
                [enc setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
                [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:impl_->faith_viewmodel_mesh.size()];
                uniforms.camera_pos = simd_make_float3(cam_pos.x, cam_pos.y, cam_pos.z);
            }
        }

        [enc endEncoding];

        // ---------------------------------------------------------------------
        // Pass 2: Post-Processing, SSAO & Tone Mapping (HDR -> Final Output)
        // ---------------------------------------------------------------------
        MTLRenderPassDescriptor* postPass = [MTLRenderPassDescriptor renderPassDescriptor];
        postPass.colorAttachments[0].texture = final_target;
        postPass.colorAttachments[0].loadAction = MTLLoadActionClear;
        postPass.colorAttachments[0].storeAction = MTLStoreActionStore;
        postPass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

        id<MTLRenderCommandEncoder> postEnc = [cmd_buffer renderCommandEncoderWithDescriptor:postPass];
        [postEnc setRenderPipelineState:impl_->post_pipeline];
        [postEnc setFragmentTexture:impl_->scene_hdr_tex atIndex:0];
        [postEnc setFragmentTexture:impl_->offscreen_depth_tex atIndex:1];
        [postEnc setFragmentSamplerState:impl_->linear_sampler atIndex:0];
        [postEnc setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
        [postEnc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];

        // ---------------------------------------------------------------------
        // Pass 3: 2D HUD, Cutscene Video/Letterbox Overlay & Frontend UI
        // ---------------------------------------------------------------------
        simd_float2 screen_size = simd_make_float2(float(impl_->width), float(impl_->height));
        if (impl_->menu_open) {
            std::vector<HUDVertex> bg_verts;
            std::vector<MetalRenderer::Impl::UITextureBatch> tex_batches;
            std::vector<HUDVertex> fg_verts;
            impl_->draw_main_menu_ui(bg_verts, tex_batches, fg_verts, telemetry);

            if (!bg_verts.empty()) {
                [postEnc setRenderPipelineState:impl_->hud_pipeline];
                bind_vertex_bytes_or_buffer(postEnc, bg_verts.data(), bg_verts.size() * sizeof(HUDVertex), 0);
                [postEnc setVertexBytes:&screen_size length:sizeof(screen_size) atIndex:1];
                [postEnc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:bg_verts.size()];
            }
            if (!tex_batches.empty() && impl_->ui_tex_pipeline) {
                [postEnc setRenderPipelineState:impl_->ui_tex_pipeline];
                [postEnc setVertexBytes:&screen_size length:sizeof(screen_size) atIndex:1];
                [postEnc setFragmentSamplerState:impl_->linear_sampler atIndex:0];
                for (const auto& batch : tex_batches) {
                    if (!batch.tex || batch.verts.empty()) continue;
                    [postEnc setFragmentTexture:batch.tex atIndex:0];
                    bind_vertex_bytes_or_buffer(postEnc, batch.verts.data(), batch.verts.size() * sizeof(UITexVertex), 0);
                    [postEnc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:batch.verts.size()];
                }
            }
            if (!fg_verts.empty()) {
                [postEnc setRenderPipelineState:impl_->hud_pipeline];
                bind_vertex_bytes_or_buffer(postEnc, fg_verts.data(), fg_verts.size() * sizeof(HUDVertex), 0);
                [postEnc setVertexBytes:&screen_size length:sizeof(screen_size) atIndex:1];
                [postEnc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:fg_verts.size()];
            }
        } else if (cutscene_active) {
            const CutscenePlayer* cp = impl_->cutscene_player;
            float w = float(impl_->width);
            float h = float(impl_->height);

            // 1. If playing a Bink (.bik) video movie, upload decoded RGBA frame and draw full-screen 16:9 quad
            if (cp->get_mode() == ECutsceneMode::BinkVideo) {
                int vw = cp->get_video_width();
                int vh = cp->get_video_height();
                const auto& rgba = cp->get_rgba_frame();
                if (vw > 0 && vh > 0 && rgba.size() == static_cast<size_t>(vw * vh * 4)) {
                    if (!impl_->bink_video_tex ||
                        (int)impl_->bink_video_tex.width != vw ||
                        (int)impl_->bink_video_tex.height != vh) {
                        MTLTextureDescriptor* td = [MTLTextureDescriptor
                            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB
                                                         width:vw
                                                        height:vh
                                                     mipmapped:NO];
                        td.usage = MTLTextureUsageShaderRead;
                        td.storageMode = MTLStorageModeShared;
                        impl_->bink_video_tex = [impl_->device newTextureWithDescriptor:td];
                        impl_->bink_uploaded_serial = 0;
                    }
                    if (impl_->bink_video_tex && impl_->bink_uploaded_serial != cp->get_frame_serial()) {
                        [impl_->bink_video_tex replaceRegion:MTLRegionMake2D(0, 0, vw, vh)
                                                 mipmapLevel:0
                                                   withBytes:rgba.data()
                                                 bytesPerRow:vw * 4];
                        impl_->bink_uploaded_serial = cp->get_frame_serial();
                    }

                    if (impl_->bink_video_tex && impl_->ui_tex_pipeline) {
                        // Draw solid black backdrop + letterboxed 16:9 Bink video frame
                        std::vector<HUDVertex> black_bg;
                        impl_->draw_ui_quad(black_bg, 0.0f, 0.0f, w, h, simd_make_float4(0.0f, 0.0f, 0.0f, 1.0f));
                        [postEnc setRenderPipelineState:impl_->hud_pipeline];
                        bind_vertex_bytes_or_buffer(postEnc, black_bg.data(), black_bg.size() * sizeof(HUDVertex), 0);
                        [postEnc setVertexBytes:&screen_size length:sizeof(screen_size) atIndex:1];
                        [postEnc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:black_bg.size()];

                        float vid_aspect = float(vw) / float(vh);
                        float scr_aspect = w / h;
                        float draw_w = w, draw_h = h, draw_x = 0.0f, draw_y = 0.0f;
                        if (scr_aspect > vid_aspect) {
                            draw_w = h * vid_aspect;
                            draw_x = (w - draw_w) * 0.5f;
                        } else {
                            draw_h = w / vid_aspect;
                            draw_y = (h - draw_h) * 0.5f;
                        }

                        std::vector<UITexVertex> qv;
                        simd_float4 white = simd_make_float4(1.0f, 1.0f, 1.0f, 1.0f);
                        UITexVertex v0{{draw_x, draw_y}, {0.0f, 0.0f}, white};
                        UITexVertex v1{{draw_x + draw_w, draw_y}, {1.0f, 0.0f}, white};
                        UITexVertex v2{{draw_x + draw_w, draw_y + draw_h}, {1.0f, 1.0f}, white};
                        UITexVertex v3{{draw_x, draw_y + draw_h}, {0.0f, 1.0f}, white};
                        qv = {v0, v1, v2, v0, v2, v3};

                        [postEnc setRenderPipelineState:impl_->ui_tex_pipeline];
                        [postEnc setVertexBytes:&screen_size length:sizeof(screen_size) atIndex:1];
                        [postEnc setFragmentSamplerState:impl_->linear_sampler atIndex:0];
                        [postEnc setFragmentTexture:impl_->bink_video_tex atIndex:0];
                        bind_vertex_bytes_or_buffer(postEnc, qv.data(), qv.size() * sizeof(UITexVertex), 0);
                        [postEnc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:qv.size()];
                    }
                }
            }

            // 2. Cinema Letterbox Bars, Cutscene Progress & Synchronized Subtitles
            std::vector<HUDVertex> cs_hud;
            float lb = cp->get_letterbox_amount();
            float bar_h = ((cp->get_mode() == ECutsceneMode::InEngineMatinee) ? 64.0f : 44.0f) * lb;
            if (bar_h > 1.0f) {
                impl_->draw_ui_quad(cs_hud, 0.0f, 0.0f, w, bar_h, simd_make_float4(0.0f, 0.0f, 0.0f, 0.88f));
                impl_->draw_ui_quad(cs_hud, 0.0f, h - bar_h, w, bar_h, simd_make_float4(0.0f, 0.0f, 0.0f, 0.88f));
            }

            // Top-right Skip / Next Cutscene controls + progress bar
            std::string ctrl_str = "[SPACE / ENTER] SKIP CUTSCENE   |   [C] NEXT CUTSCENE";
            impl_->draw_ui_text(cs_hud, ctrl_str, w - 535.0f, 14.0f, 1.55f,
                                simd_make_float4(0.88f, 0.90f, 0.94f, 0.88f));
            float prog = (cp->get_duration() > 0.0f)
                             ? std::clamp(cp->get_current_time() / cp->get_duration(), 0.0f, 1.0f)
                             : 0.0f;
            impl_->draw_ui_quad(cs_hud, 28.0f, 16.0f, 220.0f, 6.0f, simd_make_float4(0.18f, 0.20f, 0.24f, 0.75f));
            impl_->draw_ui_quad(cs_hud, 28.0f, 16.0f, 220.0f * prog, 6.0f, simd_make_float4(0.902f, 0.078f, 0.078f, 0.95f));
            impl_->draw_ui_text(cs_hud, "CUTSCENE: " + cp->get_movie_name(), 28.0f, 26.0f, 1.5f,
                                simd_make_float4(0.85f, 0.88f, 0.92f, 0.85f));

            // Synchronized localized dialogue subtitle
            const std::string& sub = cp->get_active_subtitle();
            if (!sub.empty()) {
                // Split long subtitle lines across 2 lines if > 82 chars
                std::string line1 = sub;
                std::string line2;
                if (sub.size() > 82) {
                    size_t split = sub.rfind(' ', 82);
                    if (split != std::string::npos) {
                        line1 = sub.substr(0, split);
                        line2 = sub.substr(split + 1);
                    }
                }
                float max_chars = static_cast<float>(std::max(line1.size(), line2.size()));
                float box_w = std::min(w - 80.0f, max_chars * 10.6f + 40.0f);
                float box_h = line2.empty() ? 30.0f : 52.0f;
                float box_x = (w - box_w) * 0.5f;
                float box_y = h - std::max(bar_h + box_h + 10.0f, 56.0f);
                impl_->draw_ui_quad(cs_hud, box_x, box_y, box_w, box_h, simd_make_float4(0.03f, 0.05f, 0.08f, 0.82f));
                impl_->draw_ui_quad(cs_hud, box_x, box_y, 3.0f, box_h, simd_make_float4(0.902f, 0.078f, 0.078f, 0.95f));
                impl_->draw_ui_text(cs_hud, line1, box_x + 18.0f, box_y + 8.0f, 1.75f,
                                    simd_make_float4(0.99f, 0.99f, 1.0f, 1.0f));
                if (!line2.empty()) {
                    impl_->draw_ui_text(cs_hud, line2, box_x + 18.0f, box_y + 29.0f, 1.75f,
                                        simd_make_float4(0.99f, 0.99f, 1.0f, 1.0f));
                }
            }

            if (!cs_hud.empty()) {
                [postEnc setRenderPipelineState:impl_->hud_pipeline];
                bind_vertex_bytes_or_buffer(postEnc, cs_hud.data(), cs_hud.size() * sizeof(HUDVertex), 0);
                [postEnc setVertexBytes:&screen_size length:sizeof(screen_size) atIndex:1];
                [postEnc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:cs_hud.size()];
            }
        } else {
            std::vector<HUDVertex> hud_verts;
            impl_->draw_hud(hud_verts, scene, telemetry);

            if (!hud_verts.empty()) {
                [postEnc setRenderPipelineState:impl_->hud_pipeline];
                bind_vertex_bytes_or_buffer(postEnc, hud_verts.data(), hud_verts.size() * sizeof(HUDVertex), 0);
                [postEnc setVertexBytes:&screen_size length:sizeof(screen_size) atIndex:1];
                [postEnc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:hud_verts.size()];
            }
        }

        [postEnc endEncoding];

        if (drawable) {
            [cmd_buffer presentDrawable:drawable];
        }

        impl_->last_cmd_buffer = cmd_buffer;
        if (!impl_->headless && impl_->in_flight_sem) {
            dispatch_semaphore_t sem = impl_->in_flight_sem;
            [cmd_buffer addCompletedHandler:^(id<MTLCommandBuffer> _Nonnull) {
                dispatch_semaphore_signal(sem);
            }];
            [cmd_buffer commit];
        } else {
            [cmd_buffer commit];
            [cmd_buffer waitUntilCompleted];
        }

        impl_->frame_index++;
    }
}

bool MetalRenderer::save_screenshot_ppm(const std::string& path) {
    if (!impl_->initialized || !impl_->offscreen_color_tex) return false;
    if (impl_->last_cmd_buffer) {
        [impl_->last_cmd_buffer waitUntilCompleted];
    }

    int w = impl_->width;
    int h = impl_->height;
    std::vector<uint8_t> pixels(w * h * 4);

    [impl_->offscreen_color_tex getBytes:pixels.data()
                             bytesPerRow:w * 4
                            fromRegion:MTLRegionMake2D(0, 0, w, h)
                           mipmapLevel:0];

    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;

    out << "P6\n" << w << " " << h << "\n255\n";
    std::vector<uint8_t> rgb(w * h * 3);
    for (int i = 0; i < w * h; ++i) {
        rgb[i * 3 + 0] = pixels[i * 4 + 0];
        rgb[i * 3 + 1] = pixels[i * 4 + 1];
        rgb[i * 3 + 2] = pixels[i * 4 + 2];
    }
    out.write(reinterpret_cast<const char*>(rgb.data()), rgb.size());
    out.close();

    std::cout << "[MetalRenderer] Exported PPM screenshot: " << path << " (" << w << "x" << h << ")" << std::endl;
    return true;
}

bool MetalRenderer::save_screenshot_png(const std::string& path) {
    if (!impl_->initialized || !impl_->offscreen_color_tex) return false;
    if (impl_->last_cmd_buffer) {
        [impl_->last_cmd_buffer waitUntilCompleted];
    }

    int w = impl_->width;
    int h = impl_->height;
    std::vector<uint8_t> pixels(w * h * 4);

    [impl_->offscreen_color_tex getBytes:pixels.data()
                             bytesPerRow:w * 4
                            fromRegion:MTLRegionMake2D(0, 0, w, h)
                           mipmapLevel:0];

    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, pixels.data(), pixels.size(), NULL);
    CGImageRef imageRef = CGImageCreate(
        w, h,
        8, 32,
        w * 4,
        colorSpace,
        (CGBitmapInfo)((uint32_t)kCGBitmapByteOrder32Big | (uint32_t)kCGImageAlphaPremultipliedLast),
        provider,
        NULL,
        false,
        kCGRenderingIntentDefault
    );

    CFURLRef url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
                                                           (const UInt8*)path.c_str(),
                                                           path.length(),
                                                           false);
    CGImageDestinationRef dest = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
    bool success = false;
    if (dest) {
        CGImageDestinationAddImage(dest, imageRef, NULL);
        success = CGImageDestinationFinalize(dest);
        CFRelease(dest);
    }

    if (url) CFRelease(url);
    if (imageRef) CGImageRelease(imageRef);
    if (provider) CGDataProviderRelease(provider);
    if (colorSpace) CGColorSpaceRelease(colorSpace);

    if (success) {
        std::cout << "[MetalRenderer] Exported native PNG screenshot: " << path << " (" << w << "x" << h << ")" << std::endl;
    } else {
        std::cerr << "[MetalRenderer] Failed to export PNG: " << path << std::endl;
    }
    return success;
}

bool MetalRenderer::is_initialized() const { return impl_->initialized; }
bool MetalRenderer::is_headless() const { return impl_->headless; }
int MetalRenderer::width() const { return impl_->width; }
int MetalRenderer::height() const { return impl_->height; }
uint64_t MetalRenderer::frame_count() const { return impl_->frame_index; }

void MetalRenderer::set_menu_open(bool open) { impl_->menu_open = open; }
bool MetalRenderer::is_menu_open() const { return impl_->menu_open; }
void MetalRenderer::set_selected_chapter(int idx) { impl_->selected_chapter = std::clamp(idx, 0, 9); }
int MetalRenderer::selected_chapter() const { return impl_->selected_chapter; }
void MetalRenderer::set_selected_menu_tab(int tab) { impl_->selected_menu_tab = std::clamp(tab, 0, 3); }
int MetalRenderer::selected_menu_tab() const { return impl_->selected_menu_tab; }
void MetalRenderer::set_selected_menu_row(int row) { impl_->selected_menu_row = std::clamp(row, 0, 9); }
int MetalRenderer::selected_menu_row() const { return impl_->selected_menu_row; }
void MetalRenderer::set_menu_options_state(int sens_pct, int fov_deg, bool fullscreen) {
    impl_->opt_sens_pct = sens_pct;
    impl_->opt_fov_deg = fov_deg;
    impl_->opt_fullscreen = fullscreen;
}
void MetalRenderer::set_cutscene_player(const CutscenePlayer* player) { impl_->cutscene_player = player; }

void* MetalRenderer::raw_device() const { return (__bridge void*)impl_->device; }
void* MetalRenderer::raw_command_queue() const { return (__bridge void*)impl_->command_queue; }
void* MetalRenderer::raw_color_texture() const { return (__bridge void*)impl_->offscreen_color_tex; }
void* MetalRenderer::raw_depth_texture() const { return (__bridge void*)impl_->offscreen_depth_tex; }

} // namespace me
