#include "metal_renderer.hpp"
#include "../anim/anim_system.hpp"
#include "../assets/scene_materials.hpp"
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
};

struct VertexOut {
    float4 clip_pos [[position]];
    float3 world_pos;
    float3 world_norm;
    float2 uv;
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

    // Unpack vertex color (ABGR / RGBA)
    float r = float((v.color >> 0) & 0xFF) / 255.0;
    float g = float((v.color >> 8) & 0xFF) / 255.0;
    float b = float((v.color >> 16) & 0xFF) / 255.0;
    float a = float((v.color >> 24) & 0xFF) / 255.0;
    out.color = float4(r, g, b, a);

    return out;
}

fragment float4 world_fragment(VertexOut in [[stage_in]],
                               constant FrameUniforms& uniforms [[buffer(0)]]) {
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
    if (dot(vtx_N, geo_N) < 0.0) vtx_N = -vtx_N;

    // Blend smooth vertex normal with crisp geometric face normal
    float3 N = normalize(mix(geo_N, vtx_N, 0.55));
    float3 L = normalize(float3(-0.42, -0.58, 0.70));

    // 1. Directional Sun + Beast Sky/Ground Radiosity (carefully calibrated so whites never blow out)
    float NdotL = max(dot(N, L), 0.0);
    // Secondary directional fill so X-facing and Y-facing shadow walls have distinct tonal separation
    float side_contrast = 0.5 + 0.5 * N.x - 0.25 * N.y;

    float3 sun_light = float3(0.54, 0.52, 0.49) * NdotL;
    float sky_hemi = saturate(N.z * 0.5 + 0.5);
    // Cool azure shadow bounce in shadowed planes (signature Mirror's Edge blue shadows)
    float3 sky_bounce = mix(float3(0.32, 0.42, 0.56), float3(0.46, 0.52, 0.58), sky_hemi) * (0.78 + 0.22 * side_contrast);
    float3 lighting = sun_light + sky_bounce;

    // 2. Base Albedo & Procedural Architectural Material Detailing
    float3 base_albedo = in.color.rgb * float3(uniforms.actor_tint);
    float dist = length(float3(uniforms.camera_pos) - in.world_pos);

    float rv_factor = saturate(max(uniforms.is_runner_vision, uniforms.runner_vision_strength));
    if (rv_factor < 0.5) {
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

    float3 lit_color = base_albedo * lighting;

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
// 3. First-Person Faith Viewmodel (CH_Faith_1P)
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

    float r = float((v.color >> 0) & 0xFF) / 255.0;
    float g = float((v.color >> 8) & 0xFF) / 255.0;
    float b = float((v.color >> 16) & 0xFF) / 255.0;
    float a = float((v.color >> 24) & 0xFF) / 255.0;
    out.color = float4(r, g, b, a);

    return out;
}

fragment float4 viewmodel_fragment(VertexOut in [[stage_in]],
                                   constant FrameUniforms& uniforms [[buffer(0)]]) {
    float3 dpdx = dfdx(in.world_pos);
    float3 dpdy = dfdy(in.world_pos);
    float3 geo_N = normalize(cross(dpdx, dpdy));
    float3 N = normalize(mix(in.world_norm, geo_N, 0.4));
    float3 L = normalize(float3(0.35, -0.45, 0.82));
    float3 V = normalize(-in.world_pos);

    float NdotL = max(dot(N, L), 0.0);
    float3 H = normalize(L + V);
    float spec = pow(max(dot(N, H), 0.0), 24.0) * 0.18;
    float rim = pow(1.0 - max(dot(N, V), 0.0), 3.0) * 0.12;

    float3 col = in.color.rgb * (0.48 + 0.52 * NdotL) + float3(spec + rim);
    return float4(col, 1.0);
}

// -----------------------------------------------------------------------------
// 4. Post-Processing & Tone Mapping (TdToneMapping + TdMotionBlur)
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

fragment float4 post_fragment(PostVertexOut in [[stage_in]],
                              texture2d<float> scene_tex [[texture(0)]],
                              sampler smp [[sampler(0)]],
                              constant FrameUniforms& uniforms [[buffer(0)]]) {
    float2 uv = in.uv;
    float speed = uniforms.speed_2d;

    // 1. Radial Velocity Motion Blur (TdMotionBlurShader.usf - peripheral only)
    float3 scene_color = float3(0.0);
    float2 D = uv - float2(0.5, 0.5);
    float r = length(D);
    if (speed > 420.0 && r > 0.22) {
        float speed_factor = saturate((speed - 420.0) / 230.0);
        float periph = smoothstep(0.22, 0.65, r);
        float delta_r = min(0.025 * speed_factor * periph, 0.03);
        float2 v_step = (r > 1e-4) ? ((D / r) * (delta_r / 6.0)) : float2(0.0);

        float total_weight = 0.0;
        for (int k = 0; k < 6; ++k) {
            float w = 1.0 - float(k) / 6.0;
            scene_color += scene_tex.sample(smp, clamp(uv - float(k) * v_step, 0.001, 0.999)).rgb * w;
            total_weight += w;
        }
        scene_color /= total_weight;
    } else {
        scene_color = scene_tex.sample(smp, uv).rgb;
    }

    // 2. Balanced Filmic DICE Shoulder Tone Mapping (preserves highlight detail without #FFFFFF clipping)
    float3 x = max(scene_color * 1.04, 0.0);
    float3 toned = (x * (1.12 * x + 0.18)) / (x * (1.08 * x + 0.42) + 0.14);
    toned = saturate(toned);

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
// Procedural Geometry Helpers
// -----------------------------------------------------------------------------
static void append_box(std::vector<Vertex>& verts, const Vec3& center, const Vec3& ext,
                       const Vec3& color_rgb, bool is_runner_red = false) {
    uint32_t c = is_runner_red ? 0xFF0E14E6 : 0xFFFFFFFF;
    if (!is_runner_red) {
        uint8_t r = (uint8_t)std::clamp(color_rgb.x * 255.0f, 0.0f, 255.0f);
        uint8_t g = (uint8_t)std::clamp(color_rgb.y * 255.0f, 0.0f, 255.0f);
        uint8_t b = (uint8_t)std::clamp(color_rgb.z * 255.0f, 0.0f, 255.0f);
        c = (0xFF << 24) | (b << 16) | (g << 8) | r;
    }

    Vec3 p0 = center + Vec3(-ext.x, -ext.y, -ext.z);
    Vec3 p1 = center + Vec3( ext.x, -ext.y, -ext.z);
    Vec3 p2 = center + Vec3( ext.x,  ext.y, -ext.z);
    Vec3 p3 = center + Vec3(-ext.x,  ext.y, -ext.z);
    Vec3 p4 = center + Vec3(-ext.x, -ext.y,  ext.z);
    Vec3 p5 = center + Vec3( ext.x, -ext.y,  ext.z);
    Vec3 p6 = center + Vec3( ext.x,  ext.y,  ext.z);
    Vec3 p7 = center + Vec3(-ext.x,  ext.y,  ext.z);

    auto add_quad = [&](const Vec3& a, const Vec3& b, const Vec3& d, const Vec3& e, const Vec3& n) {
        Vertex v0{a, n, Vec3(1,0,0), 0, 0, 0, 0, c};
        Vertex v1{b, n, Vec3(1,0,0), 1, 0, 0, 0, c};
        Vertex v2{d, n, Vec3(1,0,0), 1, 1, 0, 0, c};
        Vertex v3{e, n, Vec3(1,0,0), 0, 1, 0, 0, c};
        verts.push_back(v0); verts.push_back(v1); verts.push_back(v2);
        verts.push_back(v0); verts.push_back(v2); verts.push_back(v3);
    };

    add_quad(p4, p5, p6, p7, Vec3(0, 0, 1));  // Top (+Z)
    add_quad(p3, p2, p1, p0, Vec3(0, 0, -1)); // Bottom (-Z)
    add_quad(p0, p1, p5, p4, Vec3(0, -1, 0)); // Front (-Y)
    add_quad(p2, p3, p7, p6, Vec3(0, 1, 0));  // Back (+Y)
    add_quad(p1, p2, p6, p5, Vec3(1, 0, 0));  // Right (+X)
    add_quad(p3, p0, p4, p7, Vec3(-1, 0, 0)); // Left (-X)
}

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

// -----------------------------------------------------------------------------
// PIMPL Implementation
// -----------------------------------------------------------------------------
struct MetalRenderer::Impl {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> command_queue = nil;
    id<MTLLibrary> shader_library = nil;

    // Render Pipeline States
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

    int width = 1280;
    int height = 720;
    bool headless = true;
    bool initialized = false;
    uint64_t frame_index = 0;

    // Menu state & Frontend UI System (TdMainMenu.me1 + UI/TdUI_FrontEnd.upk)
    bool menu_open = false;
    int selected_chapter = 1;
    MainMenuSystem main_menu;
    bool main_menu_gpu_ready = false;
    id<MTLTexture> ui_logo_tex = nil;
    id<MTLTexture> ui_bag_tex = nil;
    id<MTLTexture> ui_time_tex = nil;
    id<MTLTexture> ui_panel_bg_tex = nil;
    id<MTLTexture> ui_faith_art_tex = nil;
    id<MTLTexture> ui_chapter_tex[10] = {nil};

    struct UITextureBatch {
        id<MTLTexture> tex = nil;
        std::vector<UITexVertex> verts;
    };

    // Procedural Fallback Cityscape Meshes
    std::vector<Vertex> rooftop_mesh;
    std::vector<Vertex> runner_vision_mesh;

    // First-Person Faith Viewmodel Mesh & 3D Enemy Guard Mesh
    std::vector<Vertex> faith_viewmodel_mesh;
    std::vector<Vertex> enemy_guard_mesh;
    AnimSystem anim_system;

    // Cached GPU Vertex Buffers for Scene Meshes
    std::string cached_map_name;
    size_t cached_total_verts = 0;
    std::vector<id<MTLBuffer>> cached_mesh_buffers;

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
        const unsigned num_threads = static_cast<unsigned>(std::min<size_t>(num_groups, std::min(hw, 8u)));
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
        size_t uploaded = 0;
        mat_textures.assign(lib->textures.size(), nil);
        for (size_t i = 0; i < lib->textures.size(); ++i) {
            @autoreleasepool {
                mat_textures[i] = upload_scene_texture(lib->textures[i]);
                if (mat_textures[i]) uploaded++;
            }
        }
        compile_material_shaders(*lib);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::cout << "[MetalRenderer] Material library resident: " << lib->materials.size() << " materials, "
                  << uploaded << "/" << lib->textures.size() << " textures uploaded in " << secs << " s" << std::endl;
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
    }

    bool compile_shaders() {
        NSError* error = nil;
        NSString* source = [NSString stringWithUTF8String:MSL_SHADERS];
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
        depthDesc.usage = MTLTextureUsageRenderTarget;
        depthDesc.storageMode = MTLStorageModePrivate;
        offscreen_depth_tex = [device newTextureWithDescriptor:depthDesc];

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

    void generate_procedural_scene() {
        rooftop_mesh.clear();
        runner_vision_mesh.clear();

        append_box(rooftop_mesh, Vec3(0, 0, -20), Vec3(2500, 2500, 20), Vec3(0.95f, 0.95f, 0.96f));
        append_box(rooftop_mesh, Vec3(0, 2500, 60), Vec3(2500, 40, 60), Vec3(0.90f, 0.92f, 0.94f));
        append_box(rooftop_mesh, Vec3(0, -2500, 60), Vec3(2500, 40, 60), Vec3(0.90f, 0.92f, 0.94f));
        append_box(rooftop_mesh, Vec3(2500, 0, 60), Vec3(40, 2500, 60), Vec3(0.90f, 0.92f, 0.94f));
        append_box(rooftop_mesh, Vec3(-2500, 0, 60), Vec3(40, 2500, 60), Vec3(0.90f, 0.92f, 0.94f));
        append_box(rooftop_mesh, Vec3(600, 800, 75), Vec3(180, 280, 75), Vec3(0.88f, 0.90f, 0.92f));
        append_box(rooftop_mesh, Vec3(-800, -500, 90), Vec3(250, 160, 90), Vec3(0.86f, 0.88f, 0.90f));
        append_box(rooftop_mesh, Vec3(3500, 1200, 600), Vec3(600, 800, 1200), Vec3(0.92f, 0.94f, 0.96f));
        append_box(rooftop_mesh, Vec3(-3200, 2400, 800), Vec3(500, 500, 1500), Vec3(0.89f, 0.92f, 0.95f));
        append_box(rooftop_mesh, Vec3(1200, -3800, 400), Vec3(800, 700, 900), Vec3(0.94f, 0.95f, 0.97f));

        append_box(runner_vision_mesh, Vec3(400, 300, 35), Vec3(60, 140, 35), Vec3(0.95f, 0.05f, 0.05f), true);
        append_box(runner_vision_mesh, Vec3(1200, 500, 300), Vec3(15, 15, 300), Vec3(0.95f, 0.05f, 0.05f), true);
        append_box(runner_vision_mesh, Vec3(1200, 650, 600), Vec3(15, 150, 15), Vec3(0.95f, 0.05f, 0.05f), true);
        append_box(runner_vision_mesh, Vec3(800, 2480, 120), Vec3(400, 10, 50), Vec3(0.95f, 0.05f, 0.05f), true);
        append_box(runner_vision_mesh, Vec3(-200, 600, 45), Vec3(50, 160, 45), Vec3(0.95f, 0.05f, 0.05f), true);

        // Articulated 3D KrugerSec / CPF SWAT Guard Mesh (local origin at feet)
        enemy_guard_mesh.clear();
        // Tactical boots & shin guards
        append_box(enemy_guard_mesh, Vec3(0, -10, 22), Vec3(7, 6, 22), Vec3(0.14f, 0.16f, 0.20f));
        append_box(enemy_guard_mesh, Vec3(0,  10, 22), Vec3(7, 6, 22), Vec3(0.14f, 0.16f, 0.20f));
        // Thighs (navy tactical trousers)
        append_box(enemy_guard_mesh, Vec3(0, -10, 60), Vec3(8, 7, 18), Vec3(0.18f, 0.22f, 0.30f));
        append_box(enemy_guard_mesh, Vec3(0,  10, 60), Vec3(8, 7, 18), Vec3(0.18f, 0.22f, 0.30f));
        // Utility belt & holster
        append_box(enemy_guard_mesh, Vec3(0, 0, 80), Vec3(10, 19, 5), Vec3(0.10f, 0.10f, 0.12f));
        // Armored Kevlar Torso Vest (CPF White/Dark Grey panels facing +X)
        append_box(enemy_guard_mesh, Vec3(0, 0, 112), Vec3(11, 20, 26), Vec3(0.22f, 0.26f, 0.34f));
        append_box(enemy_guard_mesh, Vec3(3, 0, 115), Vec3(10, 16, 18), Vec3(0.88f, 0.90f, 0.94f));
        // Shoulder pads & Arms aiming forward (+X in local space)
        append_box(enemy_guard_mesh, Vec3(0, -24, 128), Vec3(8, 6, 8), Vec3(0.88f, 0.90f, 0.94f));
        append_box(enemy_guard_mesh, Vec3(0,  24, 128), Vec3(8, 6, 8), Vec3(0.88f, 0.90f, 0.94f));
        append_box(enemy_guard_mesh, Vec3(16, -18, 122), Vec3(16, 5, 5), Vec3(0.18f, 0.22f, 0.30f));
        append_box(enemy_guard_mesh, Vec3(16,  14, 122), Vec3(16, 5, 5), Vec3(0.18f, 0.22f, 0.30f));
        // Tactical Helmet & Reflective Black Visor (+X)
        append_box(enemy_guard_mesh, Vec3(0, 0, 152), Vec3(10, 10, 12), Vec3(0.16f, 0.18f, 0.22f));
        append_box(enemy_guard_mesh, Vec3(6, 0, 153), Vec3(6, 9, 5), Vec3(0.05f, 0.08f, 0.12f));
        // Extended G36C Carbine Rifle (+X at hand level z=118, highlighted in scarlet red for Disarm window)
        append_box(enemy_guard_mesh, Vec3(28, 0, 118), Vec3(12, 2.5f, 3.5f), Vec3(0.95f, 0.08f, 0.08f), true);
        append_box(enemy_guard_mesh, Vec3(42, 0, 119), Vec3(8,  1.2f, 1.5f), Vec3(0.95f, 0.08f, 0.08f), true);
        append_box(enemy_guard_mesh, Vec3(26, 0, 111), Vec3(2.5f, 1.8f, 5.0f), Vec3(0.95f, 0.08f, 0.08f), true);

        // Load real UE3 USkeletalMesh & TdAnimSet assets from Mirror's Edge
        anim_system.init_from_game_root("/Users/tomnom/mirrorsedge");
    }

    void build_faith_viewmodel(const PlayerTelemetry& telemetry) {
        if (anim_system.is_loaded()) {
            anim_system.evaluate_faith_1p(telemetry, faith_viewmodel_mesh);
            if (!faith_viewmodel_mesh.empty()) return;
        }
        faith_viewmodel_mesh.clear();

        float sim_time = telemetry.sim_time;
        float speed = telemetry.speed_2d;
        EMovement state = telemetry.move_state;

        // Natural runner arm pumping cadence (framed cleanly in lower-left / lower-right peripheral view)
        float stride_freq = std::clamp(speed * 0.022f, 2.5f, 14.0f);
        float swing = (speed > 40.0f) ? std::sin(sim_time * stride_freq) * 5.5f : std::sin(sim_time * 2.0f) * 0.6f;
        float bob_z = (speed > 40.0f) ? std::cos(sim_time * stride_freq) * 2.2f : 0.0f;

        float r_arm_x = 19.0f;
        float r_arm_y = 32.0f + swing;
        float r_arm_z = -20.0f + bob_z;

        float l_arm_x = -19.0f;
        float l_arm_y = 32.0f - swing;
        float l_arm_z = -20.0f - bob_z;

        if (state == EMovement::MOVE_WallRunningRight) {
            r_arm_x = 24.0f; r_arm_y = 36.0f; r_arm_z = -12.0f;
        } else if (state == EMovement::MOVE_WallRunningLeft) {
            l_arm_x = -24.0f; l_arm_y = 36.0f; l_arm_z = -12.0f;
        } else if (state == EMovement::MOVE_SpeedVaulting || state == EMovement::MOVE_VaultOver || state == EMovement::MOVE_SpringBoarding) {
            r_arm_y = 36.0f; r_arm_z = -14.0f;
            l_arm_y = 34.0f; l_arm_z = -15.0f;
        } else if (state == EMovement::MOVE_ZipLine) {
            // Arms raised gripping overhead zipline handle
            r_arm_x = 8.0f;  r_arm_y = 28.0f; r_arm_z = 14.0f;
            l_arm_x = -8.0f; l_arm_y = 28.0f; l_arm_z = 14.0f;
        }

        // 1. Right Forearm, Wrist Strap & Iconic Scarlet Red Runner Glove (#E61414)
        append_box(faith_viewmodel_mesh, Vec3(r_arm_x + 2.0f, r_arm_y - 10.0f, r_arm_z - 5.0f),
                   Vec3(2.4f, 8.5f, 2.2f), Vec3(0.91f, 0.74f, 0.63f));
        append_box(faith_viewmodel_mesh, Vec3(r_arm_x, r_arm_y - 1.5f, r_arm_z - 2.8f),
                   Vec3(2.6f, 1.4f, 2.4f), Vec3(0.95f, 0.95f, 0.97f));
        append_box(faith_viewmodel_mesh, Vec3(r_arm_x - 0.5f, r_arm_y + 3.5f, r_arm_z - 1.8f),
                   Vec3(2.7f, 4.0f, 1.8f), Vec3(0.902f, 0.078f, 0.078f));
        // Articulated fingers on right glove
        for (int f = 0; f < 4; ++f) {
            float fx = r_arm_x - 2.2f + float(f) * 1.15f;
            append_box(faith_viewmodel_mesh, Vec3(fx, r_arm_y + 8.5f, r_arm_z - 1.6f),
                       Vec3(0.48f, 2.0f, 0.55f), Vec3(0.88f, 0.08f, 0.08f));
            append_box(faith_viewmodel_mesh, Vec3(fx, r_arm_y + 11.0f, r_arm_z - 1.9f),
                       Vec3(0.44f, 1.0f, 0.48f), Vec3(0.91f, 0.74f, 0.63f));
        }

        // 2. Left Forearm, Geometric Tattoo & Black Fingerless Glove
        append_box(faith_viewmodel_mesh, Vec3(l_arm_x - 2.0f, l_arm_y - 10.0f, l_arm_z - 5.0f),
                   Vec3(2.4f, 8.5f, 2.2f), Vec3(0.91f, 0.74f, 0.63f));
        append_box(faith_viewmodel_mesh, Vec3(l_arm_x - 1.5f, l_arm_y - 7.5f, l_arm_z - 3.6f),
                   Vec3(2.5f, 2.2f, 2.3f), Vec3(0.12f, 0.13f, 0.16f));
        append_box(faith_viewmodel_mesh, Vec3(l_arm_x + 0.5f, l_arm_y + 3.5f, l_arm_z - 1.8f),
                   Vec3(2.6f, 4.0f, 1.8f), Vec3(0.16f, 0.17f, 0.20f));
        for (int f = 0; f < 4; ++f) {
            float fx = l_arm_x - 1.2f + float(f) * 1.15f;
            append_box(faith_viewmodel_mesh, Vec3(fx, l_arm_y + 8.5f, l_arm_z - 1.6f),
                       Vec3(0.46f, 1.8f, 0.52f), Vec3(0.91f, 0.74f, 0.63f));
        }

        // 3. Lower Legs & Split-Toe Tabi Shoes during Slide / JumpKick
        if (state == EMovement::MOVE_Slide || state == EMovement::MOVE_MeleeSlide || state == EMovement::MOVE_Coil) {
            // Extended slide kick leg in lower-center view (framed below horizon so sky & buildings remain clear)
            float leg_y = 44.0f;
            float leg_z = -24.0f;
            append_box(faith_viewmodel_mesh, Vec3(6.0f, leg_y - 8.0f, leg_z),
                       Vec3(3.2f, 12.0f, 3.0f), Vec3(0.94f, 0.95f, 0.97f));
            append_box(faith_viewmodel_mesh, Vec3(9.3f, leg_y - 8.0f, leg_z),
                       Vec3(0.4f, 12.0f, 1.2f), Vec3(0.902f, 0.078f, 0.078f));
            append_box(faith_viewmodel_mesh, Vec3(6.0f, leg_y + 6.0f, leg_z + 1.5f),
                       Vec3(2.8f, 4.5f, 3.5f), Vec3(0.15f, 0.16f, 0.19f));
            append_box(faith_viewmodel_mesh, Vec3(6.0f, leg_y + 9.5f, leg_z + 2.5f),
                       Vec3(2.6f, 1.8f, 2.8f), Vec3(0.902f, 0.078f, 0.078f));
        }

        // 4. Equipped Handgun Model (Colt 1911 / P28 when disarmed)
        if (telemetry.weapon.equipped) {
            float gx = 10.0f;
            float gy = 34.0f;
            float gz = -13.0f;
            // Matte gunmetal slide & barrel
            append_box(faith_viewmodel_mesh, Vec3(gx, gy + 5.0f, gz + 2.2f),
                       Vec3(1.3f, 7.5f, 1.6f), Vec3(0.22f, 0.25f, 0.29f));
            append_box(faith_viewmodel_mesh, Vec3(gx, gy + 12.8f, gz + 2.2f),
                       Vec3(0.5f, 1.2f, 0.5f), Vec3(0.10f, 0.11f, 0.13f));
            // Ergonomic black grip & trigger guard
            append_box(faith_viewmodel_mesh, Vec3(gx, gy - 0.5f, gz - 2.2f),
                       Vec3(1.15f, 2.2f, 3.8f), Vec3(0.12f, 0.13f, 0.15f));
            // Red Runner Glove gripping weapon handle
            append_box(faith_viewmodel_mesh, Vec3(gx + 0.5f, gy + 0.5f, gz - 1.8f),
                       Vec3(1.8f, 3.0f, 2.6f), Vec3(0.902f, 0.078f, 0.078f));
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
    // Authentic Mirror's Edge Frontend UI (TdMainMenu + TdLoadLevel + TdLoadCheckpoint)
    // Rendered over the live 3D City of Glass (Maps/Menu/TdMainMenu.me1)
    // -------------------------------------------------------------------------
    void draw_main_menu_ui(std::vector<HUDVertex>& bg_verts,
                           std::vector<UITextureBatch>& tex_batches,
                           std::vector<HUDVertex>& fg_verts,
                           const PlayerTelemetry& telemetry) {
        bg_verts.clear();
        tex_batches.clear();
        fg_verts.clear();

        float w = float(width);
        float h = float(height);

        const simd_float4 runner_red   = simd_make_float4(0.902f, 0.078f, 0.078f, 0.96f); // #E61414
        const simd_float4 dark_slate   = simd_make_float4(0.09f,  0.11f,  0.14f,  0.94f); // #171C24
        const simd_float4 frost_panel  = simd_make_float4(0.94f,  0.96f,  0.98f,  0.86f);
        const simd_float4 frost_row    = simd_make_float4(0.84f,  0.87f,  0.91f,  0.82f);
        const simd_float4 pure_white   = simd_make_float4(1.0f,   1.0f,   1.0f,   1.0f);
        const simd_float4 text_dark    = simd_make_float4(0.09f,  0.11f,  0.15f,  0.98f);
        const simd_float4 text_muted   = simd_make_float4(0.32f,  0.37f,  0.44f,  0.95f);
        const simd_float4 text_silver  = simd_make_float4(0.82f,  0.86f,  0.92f,  0.95f);

        // =====================================================================
        // 1. TOP HEADER BAR (TdMainMenu Navigation Strip + StartTitleImage Logo)
        // =====================================================================
        draw_ui_skew_quad(bg_verts, 20.0f, 16.0f, w - 40.0f, 56.0f, 12.0f, frost_panel);
        draw_ui_skew_quad(fg_verts, 20.0f, 70.0f, w - 40.0f, 3.0f,  1.0f,  runner_red);
        draw_ui_skew_quad(fg_verts, 32.0f, 16.0f, w - 40.0f, 2.0f,  1.0f,  dark_slate);

        // Left Logo Block (Dark charcoal skewed badge + StartTitleImage / Runner Star)
        draw_ui_skew_quad(bg_verts, 26.0f, 20.0f, 212.0f, 48.0f, 10.0f, dark_slate);
        draw_ui_skew_quad(fg_verts, 26.0f, 20.0f, 5.0f,   48.0f, 10.0f, runner_red);
        if (ui_logo_tex) {
            // Render authentic StartTitleImage Runner Star using alpha-mask tint mode (-1.0 alpha)
            add_ui_tex_quad(tex_batches, ui_logo_tex, 38.0f, 22.0f, 44.0f, 44.0f,
                            0.0f, 0.0f, 0.24f, 1.0f,
                            simd_make_float4(0.95f, 0.10f, 0.10f, -1.0f));
        }
        draw_ui_text_italic(fg_verts, "MIRROR'S", 90.0f, 27.0f, 1.9f, pure_white, true);
        draw_ui_text_italic(fg_verts, "EDGE",     90.0f, 45.0f, 2.1f, runner_red, true);

        // 4 Main Menu Category Tabs (TdGameUI.int [TdUIScene_MainMenu])
        static const char* MENU_TABS[4] = {"STORY", "RACE", "OPTIONS", "EXTRAS"};
        float tab_x = 256.0f;
        for (int t = 0; t < 4; ++t) {
            bool is_active_tab = (t == 0); // STORY tab active
            float tab_w = (t == 2) ? 132.0f : 114.0f;
            draw_ui_skew_quad(bg_verts, tab_x, 25.0f, tab_w, 38.0f, 9.0f,
                              is_active_tab ? runner_red : frost_row);
            draw_ui_text_italic(fg_verts, MENU_TABS[t], tab_x + 22.0f, 37.0f, 2.0f,
                                is_active_tab ? pure_white : text_dark, is_active_tab);
            tab_x += tab_w + 12.0f;
        }

        // Top-Right Scene & Camera Breadcrumb
        int sel = std::clamp(selected_chapter, 0, 9);
        const MenuChapterEntry& cur_ch = main_menu.get_chapter(sel);

        draw_ui_text_italic(fg_verts, "STORY  /  LOAD CHAPTER", w - 434.0f, 26.0f, 1.95f, text_dark, false);
        std::string map_crumb = "3D CITY: TDMAINMENU.ME1 | " + cur_ch.map_filename;
        for (char& c : map_crumb) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        draw_ui_text_italic(fg_verts, map_crumb, w - 434.0f, 49.0f, 1.25f, text_muted, false);

        // =====================================================================
        // 2. LEFT COLUMN: STORY SUBMENU & 10-CHAPTER SELECTOR (TdLoadLevel)
        // =====================================================================
        float lx = 26.0f;
        float ly = 86.0f;
        float lw = 382.0f;
        float lh = 566.0f;

        draw_ui_quad(bg_verts, lx, ly, lw, lh, frost_panel);
        draw_ui_quad(fg_verts, lx, ly, 4.0f, lh, runner_red);
        draw_ui_quad(fg_verts, lx + lw - 2.0f, ly, 2.0f, lh, dark_slate);
        draw_ui_quad(fg_verts, lx, ly + lh - 2.0f, lw, 2.0f, dark_slate);

        // Subtle Faith Vector Art Watermark (UI/TdUIResources_FrontEnd.upk -> T_Faith_03)
        if (ui_faith_art_tex) {
            add_ui_tex_quad(tex_batches, ui_faith_art_tex,
                            lx + 10.0f, ly + lh - 290.0f, 280.0f, 280.0f,
                            0.0f, 0.0f, 1.0f, 1.0f,
                            simd_make_float4(1.0f, 1.0f, 1.0f, 0.16f));
        }

        // Section Header
        draw_ui_skew_quad(bg_verts, lx + 12.0f, ly + 10.0f, lw - 24.0f, 30.0f, 7.0f, dark_slate);
        draw_ui_text_italic(fg_verts, "SELECT CHAPTER", lx + 26.0f, ly + 18.0f, 1.85f, pure_white, true);
        draw_ui_text_italic(fg_verts, "CAMPAIGN", lx + lw - 118.0f, ly + 19.0f, 1.6f, runner_red, true);

        // 10 Playable Chapters (Prologue + Chapters 1..9 from UIDataProvider_TdMaps)
        for (int i = 0; i < 10; ++i) {
            const MenuChapterEntry& ch = main_menu.get_chapter(i);
            float row_y = ly + 48.0f + float(i) * 43.0f;
            bool is_sel = (i == sel);

            if (is_sel) {
                // Active Chapter: Forward-slanted Scarlet Red parallelogram bar
                draw_ui_skew_quad(fg_verts, lx + 8.0f, row_y, lw - 14.0f, 38.0f, 9.0f, runner_red);
                draw_ui_skew_quad(fg_verts, lx + 8.0f, row_y, 5.0f, 38.0f, 9.0f, pure_white);

                std::string label = "> " + ch.map_name;
                for (char& c : label) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                draw_ui_text_italic(fg_verts, label, lx + 22.0f, row_y + 7.0f, 1.85f, pure_white, true);

                std::string sub = "MAP: " + ch.map_filename + " | " +
                                  std::to_string(ch.checkpoints.size()) + " CHECKPOINTS";
                for (char& c : sub) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                draw_ui_text_italic(fg_verts, sub, lx + 22.0f, row_y + 24.0f, 1.15f,
                                    simd_make_float4(1.0f, 0.92f, 0.92f, 0.96f), true);
            } else {
                // Unselected Chapter: Frosted slate parallelogram with high-contrast dark text
                draw_ui_skew_quad(bg_verts, lx + 14.0f, row_y + 3.0f, lw - 26.0f, 33.0f, 7.5f, frost_row);
                draw_ui_skew_quad(fg_verts, lx + 14.0f, row_y + 3.0f, 3.0f, 33.0f, 7.5f, dark_slate);
                std::string ch_upper = ch.map_name;
                for (char& c : ch_upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                draw_ui_text_italic(fg_verts, ch_upper, lx + 26.0f, row_y + 13.0f, 1.68f, text_dark, false);
            }
        }

        // Tutorial Row + Runner Vision Status Footer inside Left Panel
        float tut_y = ly + 48.0f + 10.0f * 43.0f + 4.0f;
        draw_ui_skew_quad(bg_verts, lx + 14.0f, tut_y, lw - 26.0f, 28.0f, 6.0f,
                          simd_make_float4(0.16f, 0.19f, 0.24f, 0.88f));
        draw_ui_text_italic(fg_verts, "TRAINING: TUTORIAL_P.ME1 (RUNNER BASICS)",
                            lx + 24.0f, tut_y + 8.0f, 1.4f, text_silver, true);

        draw_ui_quad(bg_verts, lx + 10.0f, ly + lh - 46.0f, lw - 20.0f, 36.0f, dark_slate);
        draw_ui_text_italic(fg_verts, "RUNNER VISION: FULL RED HIGHLIGHT",
                            lx + 20.0f, ly + lh - 39.0f, 1.35f, pure_white, true);
        std::string mi_upper = cur_ch.material_instance_tag;
        for (char& c : mi_upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        draw_ui_text_italic(fg_verts, "3D DISTRICT PARAM: " + mi_upper + " [SELECTED=1]",
                            lx + 20.0f, ly + lh - 23.0f, 1.25f, runner_red, true);

        // =====================================================================
        // 3. CENTER VIEWPORT: 3D CITY OF GLASS CALLOUT (TdSupersMessage)
        // =====================================================================
        // Keep the center of the screen open so TdMainMenu.me1's 3D skyscrapers
        // and the Runner Vision Red highlighted district shine unobstructed!
        float cx_badge = 424.0f;
        float cy_badge = h - 128.0f;
        float cw_badge = 384.0f;
        float ch_badge = 60.0f;

        draw_ui_skew_quad(bg_verts, cx_badge, cy_badge, cw_badge, ch_badge, 12.0f,
                          simd_make_float4(0.06f, 0.08f, 0.11f, 0.84f));
        draw_ui_skew_quad(fg_verts, cx_badge, cy_badge, 5.0f, ch_badge, 12.0f, runner_red);

        std::string dist_hdr = "CITY OF GLASS  //  " + cur_ch.map_name;
        for (char& c : dist_hdr) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        draw_ui_text_italic(fg_verts, dist_hdr, cx_badge + 18.0f, cy_badge + 10.0f, 1.6f, pure_white, true);

        // Flatten newlines in TdSupersMessage into single-line telemetry string
        std::string supers_flat = cur_ch.district_timestamp;
        for (char& c : supers_flat) {
            if (c == '\n') c = ' ';
        }
        if (supers_flat.length() > 44) supers_flat = supers_flat.substr(0, 44);
        draw_ui_text_italic(fg_verts, supers_flat, cx_badge + 18.0f, cy_badge + 30.0f, 1.35f,
                            simd_make_float4(0.45f, 0.85f, 1.0f, 0.98f), true);
        draw_ui_text_italic(fg_verts, "KISMET EVENT: " + cur_ch.level_event,
                            cx_badge + 18.0f, cy_badge + 45.0f, 1.2f, text_silver, true);

        // =====================================================================
        // 4. RIGHT COLUMN: CHAPTER PREVIEW, CHECKPOINTS & STATS (TdLoadLevel)
        // =====================================================================
        float rx = w - 434.0f;
        float ry = 86.0f;
        float rw = 408.0f;
        float rh = 566.0f;

        draw_ui_quad(bg_verts, rx, ry, rw, rh, frost_panel);
        draw_ui_quad(fg_verts, rx, ry, 2.0f, rh, dark_slate);
        draw_ui_quad(fg_verts, rx + rw - 4.0f, ry, 4.0f, rh, runner_red);
        draw_ui_quad(fg_verts, rx, ry + rh - 2.0f, rw, 2.0f, dark_slate);

        // Chapter Title Header Bar
        draw_ui_skew_quad(fg_verts, rx + 10.0f, ry + 10.0f, rw - 24.0f, 32.0f, 8.0f, runner_red);
        std::string cur_title_upper = cur_ch.map_name;
        for (char& c : cur_title_upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        draw_ui_text_italic(fg_verts, cur_title_upper, rx + 24.0f, ry + 19.0f, 1.95f, pure_white, true);

        // Chapter Halftone Preview Photograph (Maps/Menu/TdMainMenu.me1 -> Level1b_CP1..Level9_CP1)
        float img_x = rx + 14.0f;
        float img_y = ry + 50.0f;
        float img_w = rw - 28.0f;
        float img_h = 192.0f;

        draw_ui_quad(bg_verts, img_x - 2.0f, img_y - 2.0f, img_w + 4.0f, img_h + 4.0f, dark_slate);
        if (ui_chapter_tex[sel]) {
            add_ui_tex_quad(tex_batches, ui_chapter_tex[sel],
                            img_x, img_y, img_w, img_h,
                            0.0f, 0.05f, 1.0f, 0.98f, pure_white);
        } else {
            draw_ui_quad(bg_verts, img_x, img_y, img_w, img_h, simd_make_float4(0.18f, 0.22f, 0.28f, 1.0f));
        }
        // Photo caption pill
        draw_ui_skew_quad(fg_verts, img_x + 8.0f, img_y + img_h - 26.0f, 224.0f, 20.0f, 5.0f,
                          simd_make_float4(0.06f, 0.08f, 0.11f, 0.88f));
        draw_ui_text_italic(fg_verts, "PREVIEW: " + cur_ch.preview_tex_name,
                            img_x + 16.0f, img_y + img_h - 21.0f, 1.3f, pure_white, true);

        // Checkpoints Sub-List (TdLoadCheckpoint / DefaultGame.ini Checkpoints[])
        float cp_hdr_y = img_y + img_h + 10.0f;
        draw_ui_skew_quad(bg_verts, rx + 12.0f, cp_hdr_y, rw - 26.0f, 26.0f, 6.0f, dark_slate);
        draw_ui_text_italic(fg_verts, "CHAPTER CHECKPOINTS  (TDLOADCHECKPOINT)",
                            rx + 22.0f, cp_hdr_y + 7.0f, 1.5f, pure_white, true);

        size_t max_cp = std::min<size_t>(cur_ch.checkpoints.size(), 5);
        for (size_t c = 0; c < max_cp; ++c) {
            const auto& cp = cur_ch.checkpoints[c];
            float cy = cp_hdr_y + 32.0f + float(c) * 34.0f;
            bool cp_sel = (c == 0);

            if (cp_sel) {
                draw_ui_skew_quad(bg_verts, rx + 12.0f, cy, rw - 26.0f, 30.0f, 6.5f, dark_slate);
                draw_ui_skew_quad(fg_verts, rx + 12.0f, cy, 4.0f, 30.0f, 6.5f, runner_red);
            } else {
                draw_ui_skew_quad(bg_verts, rx + 14.0f, cy + 1.0f, rw - 30.0f, 28.0f, 6.0f, frost_row);
            }

            char badge = static_cast<char>('A' + c);
            std::string cp_title = std::string("[") + badge + "] " + cp.friendly_name;
            for (char& ch_c : cp_title) ch_c = static_cast<char>(std::toupper(static_cast<unsigned char>(ch_c)));

            draw_ui_text_italic(fg_verts, cp_title, rx + 22.0f, cy + 4.0f, 1.45f,
                                cp_sel ? runner_red : text_dark, cp_sel);

            std::string desc = cp.description;
            if (desc.length() > 44) desc = desc.substr(0, 41) + "...";
            draw_ui_text_italic(fg_verts, desc, rx + 24.0f, cy + 17.0f, 1.2f,
                                cp_sel ? pure_white : text_muted, cp_sel);
        }

        // Chapter Statistics Footer Card (SpeedRunTime + BagsFound with Icon_Time & Icon_Bag)
        float st_x = rx + 12.0f;
        float st_y = ry + rh - 108.0f;
        float st_w = rw - 24.0f;
        float st_h = 96.0f;

        draw_ui_quad(bg_verts, st_x, st_y, st_w, st_h, dark_slate);
        draw_ui_quad(fg_verts, st_x, st_y, st_w, 3.0f, runner_red);

        // Left Stat: Speed Run Time + Icon_Time (UI/TdUIResources.upk alpha-mask tinted pure white)
        if (ui_time_tex) {
            add_ui_tex_quad(tex_batches, ui_time_tex,
                            st_x + 10.0f, st_y + 14.0f, 36.0f, 36.0f,
                            0.0f, 0.0f, 1.0f, 1.0f,
                            simd_make_float4(1.0f, 1.0f, 1.0f, -1.0f));
        }
        draw_ui_text_italic(fg_verts, "QUALIFYING TIME", st_x + 52.0f, st_y + 14.0f, 1.35f, text_silver, true);
        draw_ui_text_italic(fg_verts, cur_ch.speedrun_target_time, st_x + 52.0f, st_y + 30.0f, 2.05f, pure_white, true);
        draw_ui_text_italic(fg_verts, "BEST TIME:  05:14:82", st_x + 14.0f, st_y + 58.0f, 1.45f, runner_red, true);
        draw_ui_text_italic(fg_verts, "SPEED RUN:  UNLOCKED", st_x + 14.0f, st_y + 76.0f, 1.3f, text_silver, true);

        // Divider
        draw_ui_quad(fg_verts, st_x + st_w * 0.54f, st_y + 12.0f, 2.0f, st_h - 24.0f,
                     simd_make_float4(0.25f, 0.29f, 0.35f, 0.9f));

        // Right Stat: Runner Bags Found + Icon_Bag (UI/TdUIResources.upk alpha-mask tinted Runner Gold)
        float bag_x = st_x + st_w * 0.57f;
        if (ui_bag_tex) {
            add_ui_tex_quad(tex_batches, ui_bag_tex,
                            bag_x, st_y + 12.0f, 40.0f, 40.0f,
                            0.0f, 0.0f, 1.0f, 1.0f,
                            simd_make_float4(1.0f, 0.85f, 0.20f, -1.0f));
        }
        int bags_found = std::clamp(telemetry.bags_collected, 1, 3);
        draw_ui_text_italic(fg_verts, "BAGS FOUND", bag_x + 46.0f, st_y + 14.0f, 1.35f, text_silver, true);
        std::string bag_str = std::to_string(bags_found) + " / 3";
        draw_ui_text_italic(fg_verts, bag_str, bag_x + 46.0f, st_y + 31.0f, 2.15f,
                            simd_make_float4(1.0f, 0.86f, 0.22f, 1.0f), true);

        // 3 Runner Bag Slot Indicators
        for (int b = 0; b < 3; ++b) {
            bool collected = (b < bags_found);
            float bx = bag_x + float(b) * 48.0f;
            float by = st_y + 62.0f;
            draw_ui_skew_quad(fg_verts, bx, by, 40.0f, 20.0f, 4.0f,
                              collected ? runner_red : simd_make_float4(0.20f, 0.24f, 0.30f, 0.95f));
            draw_ui_text_italic(fg_verts, collected ? "BAG" : "---", bx + 7.0f, by + 5.0f, 1.25f, pure_white, true);
        }

        // =====================================================================
        // 5. BOTTOM ACTION CALLOUT BUTTONS (TdUIScene ButtonBar)
        // =====================================================================
        float btn_y = h - 52.0f;
        draw_ui_skew_quad(fg_verts, 424.0f, btn_y, 250.0f, 36.0f, 8.0f, runner_red);
        draw_ui_text_italic(fg_verts, "[ENTER] LAUNCH CHAPTER", 442.0f, btn_y + 11.0f, 1.75f, pure_white, true);

        draw_ui_skew_quad(fg_verts, 688.0f, btn_y, 262.0f, 36.0f, 8.0f, dark_slate);
        draw_ui_text_italic(fg_verts, "[UP / DOWN] SELECT CHAPTER", 704.0f, btn_y + 11.0f, 1.70f, pure_white, true);

        draw_ui_skew_quad(fg_verts, 964.0f, btn_y, 266.0f, 36.0f, 8.0f, dark_slate);
        draw_ui_text_italic(fg_verts, "[TAB / ESC] RESUME GAME", 982.0f, btn_y + 11.0f, 1.70f, pure_white, true);
    }

    void draw_hud(std::vector<HUDVertex>& verts, const LevelScene& scene, const PlayerTelemetry& telemetry) {
        verts.clear();
        float w = float(width);
        float h = float(height);
        float sim_time = telemetry.sim_time;

        // 1. Center Runner Reticle Dot
        bool is_interactive_target = (telemetry.weapon.name != "") ||
                                     (telemetry.speed_2d > 450.0f) ||
                                     (telemetry.move_state == EMovement::MOVE_Snatch);
        draw_ui_reticle(verts, w * 0.5f, h * 0.5f, is_interactive_target, std::sin(sim_time * 8.0f));

        // 2. Bottom-Left Telemetry Panel Backdrop (Sleek Translucent Dark Glass + Red Runner Accent)
        draw_ui_quad(verts, 22.0f, h - 164.0f, 340.0f, 112.0f, simd_make_float4(0.04f, 0.06f, 0.09f, 0.72f));
        draw_ui_quad(verts, 22.0f, h - 164.0f, 4.0f, 112.0f, simd_make_float4(0.902f, 0.078f, 0.078f, 0.95f));

        float speed = telemetry.speed_2d;
        float kmh = speed * 0.06f;
        std::ostringstream ss_spd;
        ss_spd << "SPEED: " << std::fixed << std::setprecision(0) << speed << " u/s ("
               << std::setprecision(1) << kmh << " km/h)";
        draw_ui_text(verts, ss_spd.str(), 35.0f, h - 88.0f, 2.0f, simd_make_float4(1.0f, 1.0f, 1.0f, 0.98f));

        // Flow momentum bar frame
        draw_ui_quad(verts, 35.0f, h - 68.0f, 240.0f, 10.0f, simd_make_float4(0.12f, 0.15f, 0.20f, 0.85f));
        float bar_fill = std::clamp(speed / 630.0f, 0.0f, 1.0f);
        simd_float4 bar_col = (speed > 400.0f) ? simd_make_float4(0.902f, 0.078f, 0.078f, 1.0f)
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
                              std::to_string(telemetry.weapon.max_ammo) + "]") : "UNARMED";
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
            ? "[SPACE] JUMP / WALLRUN  |  [LSHIFT] CROUCH / SLIDE  |  [R] REACTION TIME"
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
    impl_->generate_procedural_scene();

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
    impl_->generate_procedural_scene();

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
            fov_deg = 70.0f;
            near_plane = 10.0f;
            far_plane = 400000.0f;
        }

        Vec3 fwd = rot.forward();
        Vec3 right = rot.right();
        Vec3 up = rot.up();
        Vec3 target = cam_pos + fwd * 100.0f;

        Mat4 view = Mat4::look_at(cam_pos, target, up);
        float aspect = float(impl_->width) / float(impl_->height);
        float fov_rad = fov_deg * DEG2RAD;
        Mat4 proj = Mat4::perspective(fov_rad, aspect, near_plane, far_plane);
        Mat4 vp = proj * view;

        // Viewmodel camera matrix
        Mat4 vm_view = Mat4::look_at(Vec3(0, 0, 0), Vec3(0, 100.0f, 0), Vec3(0, 0, 1));
        Mat4 vm_proj = Mat4::perspective(80.0f * DEG2RAD, aspect, 1.0f, 500.0f);
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
        uniforms.sky_color = simd_make_float3(0.68f, 0.84f, 1.0f);
        uniforms.ground_color = simd_make_float3(0.82f, 0.84f, 0.88f);
        uniforms.speed_2d = impl_->menu_open ? 0.0f : telemetry.speed_2d;
        uniforms.reaction_active = (!impl_->menu_open && telemetry.reaction_active) ? 1.0f : 0.0f;
        uniforms.health = impl_->menu_open ? 100.0f : telemetry.health;
        uniforms.exposure = 1.0f;
        uniforms.actor_tint = simd_make_float3(1.0f, 1.0f, 1.0f);
        uniforms.runner_vision_strength = 0.0f;
        uniforms.is_runner_vision = 0.0f;

        uniforms.cam_forward = simd_make_float3(fwd.x, fwd.y, fwd.z);
        uniforms.fov_tan = std::tan(fov_rad * 0.5f);
        uniforms.cam_right = simd_make_float3(right.x, right.y, right.z);
        uniforms.aspect = aspect;
        uniforms.cam_up = simd_make_float3(up.x, up.y, up.z);

        // ---------------------------------------------------------------------
        // Mirror's Edge materials: make the scene's material library resident
        // (texture upload + MSL compile happen once per library) and find out
        // whether this frame needs a translucency pass / opaque scene copies.
        // ---------------------------------------------------------------------
        impl_->sync_material_library(active_scene.materials);
        bool has_translucent = false;
        bool needs_scene_copies = false;
        if (impl_->mat_lib && !in_main_menu) {
            for (const auto& mesh : active_scene.meshes) {
                for (const auto& s : mesh.sections) {
                    const MaterialShader* sh = nullptr;
                    if (!impl_->section_pipeline(s, &sh, nullptr) || !mat_blend_is_translucent(sh->blend)) continue;
                    has_translucent = true;
                    if (sh->uses_scene_color || sh->uses_scene_depth) needs_scene_copies = true;
                }
            }
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
        scenePass.depthAttachment.storeAction = needs_scene_copies ? MTLStoreActionStore : MTLStoreActionDontCare;
        scenePass.depthAttachment.clearDepth = 1.0;

        id<MTLRenderCommandEncoder> enc = [cmd_buffer renderCommandEncoderWithDescriptor:scenePass];
        [enc setViewport:(MTLViewport){0.0, 0.0, (double)impl_->width, (double)impl_->height, 0.0, 1.0}];

        // A. Draw Sky Dome (TdDirHaze + Distant City Skyline)
        [enc setRenderPipelineState:impl_->sky_pipeline];
        [enc setDepthStencilState:impl_->depth_disabled_state];
        [enc setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];

        // B. Draw World Meshes (BasePass + Beast Radiosity)
        [enc setViewport:(MTLViewport){0.0, 0.0, (double)impl_->width, (double)impl_->height, 0.05, 1.0}];
        [enc setRenderPipelineState:impl_->world_pipeline];
        [enc setDepthStencilState:impl_->depth_write_state];

        auto bind_vertex_bytes_or_buffer = [&](id<MTLRenderCommandEncoder> encoder, const void* data, size_t length, NSUInteger index) {
            if (length <= 4096) {
                [encoder setVertexBytes:data length:length atIndex:index];
            } else {
                id<MTLBuffer> buf = [impl_->device newBufferWithBytes:data length:length options:MTLResourceStorageModeShared];
                [encoder setVertexBuffer:buf offset:0 atIndex:index];
            }
        };

        // Rebuild GPU vertex buffer cache only when scene meshes change
        size_t total_scene_verts = 0;
        for (const auto& m : active_scene.meshes) total_scene_verts += m.vertices.size();

        if (active_scene.map_name != impl_->cached_map_name || total_scene_verts != impl_->cached_total_verts ||
            impl_->cached_mesh_buffers.size() != active_scene.meshes.size()) {
            impl_->cached_map_name = active_scene.map_name;
            impl_->cached_total_verts = total_scene_verts;
            impl_->cached_mesh_buffers.clear();
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

        // Binds mesh i's vertex buffer + frame uniforms on the current encoder.
        auto bind_scene_mesh = [&](size_t i) {
            uniforms.is_runner_vision = active_scene.meshes[i].is_runner_vision ? 1.0f : 0.0f;
            [enc setVertexBuffer:impl_->cached_mesh_buffers[i] offset:0 atIndex:0];
            [enc setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1];
            [enc setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
        };
        auto section_in_range = [](const MeshBuffer& mesh, const MeshSection& s) {
            return s.vertex_count > 0 &&
                   static_cast<size_t>(s.first_vertex) + static_cast<size_t>(s.vertex_count) <= mesh.vertices.size();
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
                    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:mesh.vertices.size()];
                    continue;
                }
                if (in_main_menu) {
                    // For TdMainMenu.me1's 3D City of Glass, use directional Sun + Beast azure sky-bounce
                    // + Runner Vision Scarlet Red highlighting on the selected chapter's MI_SP0*_01 section.
                    [enc setRenderPipelineState:impl_->world_pipeline];
                    [enc setCullMode:MTLCullModeNone];
                    for (const auto& s : mesh.sections) {
                        if (!section_in_range(mesh, s)) continue;
                        std::string mname;
                        if (active_scene.materials && s.material >= 0 &&
                            static_cast<size_t>(s.material) < active_scene.materials->materials.size()) {
                            mname = active_scene.materials->materials[static_cast<size_t>(s.material)].name;
                            for (char& c : mname) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        }
                        bool is_sel_district = (!active_mi_tag.empty() && mname.find(active_mi_tag) != std::string::npos);
                        uniforms.is_runner_vision = is_sel_district ? 1.0f : 0.0f;
                        if (mname.find("water") != std::string::npos) {
                            uniforms.actor_tint = simd_make_float3(0.22f, 0.56f, 0.90f);
                        } else if (mname.find("mountain") != std::string::npos) {
                            uniforms.actor_tint = simd_make_float3(0.76f, 0.84f, 0.94f);
                        } else {
                            uniforms.actor_tint = simd_make_float3(0.96f, 0.97f, 0.99f);
                        }
                        [enc setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1];
                        [enc setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
                        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:s.first_vertex vertexCount:s.vertex_count];
                    }
                    uniforms.is_runner_vision = 0.0f;
                    uniforms.actor_tint = simd_make_float3(1.0f, 1.0f, 1.0f);
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
                        [enc setCullMode:(impl_->mat_cull_enabled && !sh->two_sided) ? MTLCullModeBack : MTLCullModeNone];
                        impl_->bind_material(enc, *m, *sh);
                    } else {
                        [enc setRenderPipelineState:impl_->world_pipeline];
                        [enc setCullMode:MTLCullModeNone];
                    }
                    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:s.first_vertex vertexCount:s.vertex_count];
                }
            }
            [enc setCullMode:MTLCullModeNone];
        } else {
            uniforms.is_runner_vision = 0.0f;
            bind_vertex_bytes_or_buffer(enc, impl_->rooftop_mesh.data(), impl_->rooftop_mesh.size() * sizeof(Vertex), 0);
            [enc setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1];
            [enc setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:impl_->rooftop_mesh.size()];

            uniforms.is_runner_vision = 1.0f;
            bind_vertex_bytes_or_buffer(enc, impl_->runner_vision_mesh.data(), impl_->runner_vision_mesh.size() * sizeof(Vertex), 0);
            [enc setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1];
            [enc setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:impl_->runner_vision_mesh.size()];
        }

        // B2. Render 3D Articulated KrugerSec / CPF SWAT Enemies (only during gameplay)
        // (the material sections above leave their own pipeline/depth state bound)
        [enc setRenderPipelineState:impl_->world_pipeline];
        [enc setDepthStencilState:impl_->depth_write_state];
        if (!impl_->menu_open && !active_scene.enemies.empty()) {
            for (const auto& bot : active_scene.enemies) {
                if (!bot.alive) continue;
                if (impl_->anim_system.is_loaded()) {
                    impl_->anim_system.evaluate_enemy_swat(bot, telemetry.sim_time, telemetry.reaction_active, impl_->enemy_guard_mesh);
                }
                if (impl_->enemy_guard_mesh.empty()) continue;
                bind_vertex_bytes_or_buffer(enc, impl_->enemy_guard_mesh.data(),
                                            impl_->enemy_guard_mesh.size() * sizeof(Vertex), 0);
                Mat4 bot_model = Mat4::translation(bot.position) * Mat4::rotation_z(bot.yaw_deg * DEG2RAD);
                std::memcpy(&uniforms.model, bot_model.m, sizeof(float) * 16);
                uniforms.is_runner_vision = (bot.disarm_window && !bot.stunned && !impl_->anim_system.is_loaded()) ? 0.35f : 0.0f;
                uniforms.actor_tint = simd_make_float3(1.0f, 1.0f, 1.0f);
                [enc setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1];
                [enc setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
                size_t v_count = impl_->enemy_guard_mesh.size();
                if (!impl_->anim_system.is_loaded() && bot.stunned && v_count > 108) v_count -= 108;
                [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:v_count];
            }
            std::memcpy(&uniforms.model, identity.m, sizeof(float) * 16);
        }

        // B2b. Render Interactive 3D Elevator Cabs, Sliding Doors & Runner Vision Buttons
        if (!impl_->menu_open && !active_scene.elevators.empty()) {
            std::vector<Vertex> elev_mesh;
            std::vector<Vertex> elev_rv_mesh;
            for (const auto& elev : active_scene.elevators) {
                Vec3 fc = elev.current_pos + elev.cab_local_offset;
                Vec3 he = elev.cab_half_extents;
                float h_cab = he.z * 2.0f;

                // Brushed stainless steel floor & ceiling with illuminated light diffuser
                append_box(elev_mesh, Vec3(fc.x, fc.y, fc.z - 8.0f),
                           Vec3(he.x, he.y, 8.0f), Vec3(0.80f, 0.83f, 0.87f));
                append_box(elev_mesh, Vec3(fc.x, fc.y, fc.z + h_cab + 8.0f),
                           Vec3(he.x, he.y, 8.0f), Vec3(0.90f, 0.92f, 0.95f));
                append_box(elev_mesh, Vec3(fc.x, fc.y, fc.z + h_cab - 1.5f),
                           Vec3(he.x * 0.65f, he.y * 0.65f, 1.5f), Vec3(0.99f, 0.99f, 1.0f));

                // Side walls (-Y and +Y) and stainless steel interior handrails
                append_box(elev_mesh, Vec3(fc.x, fc.y - he.y - 6.0f, fc.z + h_cab * 0.5f),
                           Vec3(he.x, 6.0f, h_cab * 0.5f), Vec3(0.90f, 0.92f, 0.95f));
                append_box(elev_mesh, Vec3(fc.x, fc.y + he.y + 6.0f, fc.z + h_cab * 0.5f),
                           Vec3(he.x, 6.0f, h_cab * 0.5f), Vec3(0.90f, 0.92f, 0.95f));
                append_box(elev_mesh, Vec3(fc.x, fc.y - he.y + 6.0f, fc.z + 95.0f),
                           Vec3(he.x * 0.82f, 3.0f, 2.5f), Vec3(0.68f, 0.72f, 0.77f));
                append_box(elev_mesh, Vec3(fc.x, fc.y + he.y - 6.0f, fc.z + 95.0f),
                           Vec3(he.x * 0.82f, 3.0f, 2.5f), Vec3(0.68f, 0.72f, 0.77f));

                // Sliding Lower Entry Doors (-X, S_ElevatorDoor_01: 76-unit slide per leaf)
                float slide_start = 76.0f * elev.door_open_Start;
                append_box(elev_mesh, Vec3(fc.x - he.x - 4.0f, fc.y - 38.5f - slide_start, fc.z + 120.0f),
                           Vec3(4.0f, 38.5f, 120.0f), Vec3(0.84f, 0.87f, 0.91f));
                append_box(elev_mesh, Vec3(fc.x - he.x - 4.0f, fc.y + 38.5f + slide_start, fc.z + 120.0f),
                           Vec3(4.0f, 38.5f, 120.0f), Vec3(0.84f, 0.87f, 0.91f));

                // Sliding Upper Exit Doors (+X, S_ElevatorDoor_01: 76-unit slide per leaf)
                float slide_end = 76.0f * elev.door_open_End;
                append_box(elev_mesh, Vec3(fc.x + he.x + 4.0f, fc.y - 38.5f - slide_end, fc.z + 120.0f),
                           Vec3(4.0f, 38.5f, 120.0f), Vec3(0.84f, 0.87f, 0.91f));
                append_box(elev_mesh, Vec3(fc.x + he.x + 4.0f, fc.y + 38.5f + slide_end, fc.z + 120.0f),
                           Vec3(4.0f, 38.5f, 120.0f), Vec3(0.84f, 0.87f, 0.91f));

                // Interior Control Panel & Glowing Runner Vision Elevator Button (S_ElevatorButton_Single)
                append_box(elev_mesh, Vec3(fc.x + he.x * 0.55f, fc.y + he.y - 3.0f, fc.z + 125.0f),
                           Vec3(14.0f, 2.0f, 24.0f), Vec3(0.22f, 0.25f, 0.30f));
                append_box(elev_rv_mesh, Vec3(fc.x + he.x * 0.55f, fc.y + he.y - 5.5f, fc.z + 125.0f),
                           Vec3(6.5f, 3.0f, 6.5f), Vec3(0.95f, 0.08f, 0.08f), true);
                // Runner Vision Doorway Arch Indicator
                append_box(elev_rv_mesh, Vec3(fc.x - he.x - 5.0f, fc.y, fc.z + h_cab - 8.0f),
                           Vec3(5.0f, 78.0f, 6.0f), Vec3(0.95f, 0.08f, 0.08f), true);
            }
            std::memcpy(&uniforms.model, identity.m, sizeof(float) * 16);
            uniforms.actor_tint = simd_make_float3(1.0f, 1.0f, 1.0f);
            if (!elev_mesh.empty()) {
                uniforms.is_runner_vision = 0.0f;
                bind_vertex_bytes_or_buffer(enc, elev_mesh.data(), elev_mesh.size() * sizeof(Vertex), 0);
                [enc setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1];
                [enc setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
                [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:elev_mesh.size()];
            }
            if (!elev_rv_mesh.empty()) {
                uniforms.is_runner_vision = 1.0f;
                bind_vertex_bytes_or_buffer(enc, elev_rv_mesh.data(), elev_rv_mesh.size() * sizeof(Vertex), 0);
                [enc setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1];
                [enc setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
                [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:elev_rv_mesh.size()];
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
                transPass.depthAttachment.storeAction = MTLStoreActionDontCare;
                enc = [cmd_buffer renderCommandEncoderWithDescriptor:transPass];
                [enc setViewport:(MTLViewport){0.0, 0.0, (double)impl_->width, (double)impl_->height, 0.05, 1.0}];
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
        }

        // C. Draw First-Person Faith Viewmodel (CH_Faith_1P in DPG_Foreground depth range [0.0, 0.05])
        if (!impl_->menu_open) {
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
        // Pass 2: Post-Processing & Tone Mapping (HDR -> Final Output)
        // ---------------------------------------------------------------------
        MTLRenderPassDescriptor* postPass = [MTLRenderPassDescriptor renderPassDescriptor];
        postPass.colorAttachments[0].texture = final_target;
        postPass.colorAttachments[0].loadAction = MTLLoadActionClear;
        postPass.colorAttachments[0].storeAction = MTLStoreActionStore;
        postPass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

        id<MTLRenderCommandEncoder> postEnc = [cmd_buffer renderCommandEncoderWithDescriptor:postPass];
        [postEnc setRenderPipelineState:impl_->post_pipeline];
        [postEnc setFragmentTexture:impl_->scene_hdr_tex atIndex:0];
        [postEnc setFragmentSamplerState:impl_->linear_sampler atIndex:0];
        [postEnc setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
        [postEnc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];

        // ---------------------------------------------------------------------
        // Pass 3: 2D HUD & Frontend UI Overlay (TdMainMenu / TdLoadLevel / HUD)
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

        [cmd_buffer commit];
        [cmd_buffer waitUntilCompleted];

        impl_->frame_index++;
    }
}

bool MetalRenderer::save_screenshot_ppm(const std::string& path) {
    if (!impl_->initialized || !impl_->offscreen_color_tex) return false;

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

void* MetalRenderer::raw_device() const { return (__bridge void*)impl_->device; }
void* MetalRenderer::raw_command_queue() const { return (__bridge void*)impl_->command_queue; }
void* MetalRenderer::raw_color_texture() const { return (__bridge void*)impl_->offscreen_color_tex; }
void* MetalRenderer::raw_depth_texture() const { return (__bridge void*)impl_->offscreen_depth_tex; }

} // namespace me
