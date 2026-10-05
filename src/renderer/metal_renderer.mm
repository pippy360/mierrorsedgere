#include "metal_renderer.hpp"

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
#include <sstream>
#include <iomanip>
#include <cstring>

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

    // Menu state
    bool menu_open = false;
    int selected_chapter = 1;

    // Procedural Fallback Cityscape Meshes
    std::vector<Vertex> rooftop_mesh;
    std::vector<Vertex> runner_vision_mesh;

    // First-Person Faith Viewmodel Mesh & 3D Enemy Guard Mesh
    std::vector<Vertex> faith_viewmodel_mesh;
    std::vector<Vertex> enemy_guard_mesh;

    // Cached GPU Vertex Buffers for Scene Meshes
    std::string cached_map_name;
    size_t cached_total_verts = 0;
    std::vector<id<MTLBuffer>> cached_mesh_buffers;

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
    }

    void build_faith_viewmodel(const PlayerTelemetry& telemetry) {
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

    void draw_ui_quad(std::vector<HUDVertex>& verts, float x, float y, float w, float h, simd_float4 color) {
        HUDVertex v0 = {{x, y}, color};
        HUDVertex v1 = {{x + w, y}, color};
        HUDVertex v2 = {{x + w, y + h}, color};
        HUDVertex v3 = {{x, y + h}, color};
        verts.push_back(v0); verts.push_back(v1); verts.push_back(v2);
        verts.push_back(v0); verts.push_back(v2); verts.push_back(v3);
    }

    void draw_ui_text_raw(std::vector<HUDVertex>& verts, const std::string& text, float start_x, float start_y,
                          float scale, simd_float4 color) {
        float cur_x = start_x;
        float cur_y = start_y;
        float char_w = 5.0f * scale;
        float char_h = 7.0f * scale;
        float spacing = 1.0f * scale;

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
                        float px = cur_x + col * scale;
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
        draw_ui_text_raw(verts, text, start_x + 1.5f, start_y + 1.5f, scale, shadow_col);
        draw_ui_text_raw(verts, text, start_x, start_y, scale, color);
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

        // 5. Top-Right Chapter / Checkpoint / Bags / Weapon Panel Backdrop
        float rx = w - 335.0f;
        draw_ui_quad(verts, rx - 14.0f, 18.0f, 332.0f, 102.0f, simd_make_float4(0.04f, 0.06f, 0.09f, 0.72f));
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

        // 6. Active Subtitle / Tutorial Prompt Banner (Bottom Center)
        std::string prompt = telemetry.active_subtitle.empty()
            ? "[SPACE] JUMP / WALLRUN  |  [LSHIFT] CROUCH / SLIDE  |  [R] REACTION TIME"
            : telemetry.active_subtitle;

        float banner_w = float(prompt.length()) * 11.0f + 40.0f;
        float banner_x = (w - banner_w) * 0.5f;
        draw_ui_quad(verts, banner_x, h - 42.0f, banner_w, 28.0f, simd_make_float4(0.04f, 0.06f, 0.09f, 0.80f));
        draw_ui_text(verts, prompt, banner_x + 20.0f, h - 35.0f, 1.8f, simd_make_float4(0.98f, 0.98f, 0.98f, 1.0f));

        // 7. Chapter Select Menu Overlay (when menu_open is true)
        if (menu_open) {
            draw_ui_quad(verts, 0, 0, w, h, simd_make_float4(0.02f, 0.03f, 0.05f, 0.72f));

            float mw = 460.0f;
            float mh = 380.0f;
            float mx = (w - mw) * 0.5f;
            float my = (h - mh) * 0.5f;

            draw_ui_quad(verts, mx, my, mw, mh, simd_make_float4(0.07f, 0.09f, 0.12f, 0.94f));
            draw_ui_quad(verts, mx, my, mw, 4.0f, simd_make_float4(0.902f, 0.078f, 0.078f, 1.0f));

            draw_ui_text(verts, "MIRROR'S EDGE - CHAPTER SELECT", mx + 25.0f, my + 25.0f, 2.2f,
                         simd_make_float4(1.0f, 1.0f, 1.0f, 1.0f));

            static const char* CHAPTERS[10] = {
                "0. PROLOGUE: TUTORIAL",
                "1. CHAPTER 1: FLIGHT",
                "2. CHAPTER 2: JACKNIFE",
                "3. CHAPTER 3: HEAT",
                "4. CHAPTER 4: ROPEBURN",
                "5. CHAPTER 5: NEW EDEN",
                "6. CHAPTER 6: PIRANDELLO KRUGER",
                "7. CHAPTER 7: THE BOAT",
                "8. CHAPTER 8: KATE",
                "9. CHAPTER 9: THE SHARD"
            };

            for (int i = 0; i < 10; ++i) {
                float iy = my + 70.0f + float(i) * 28.0f;
                bool is_sel = (i == selected_chapter);
                if (is_sel) {
                    draw_ui_quad(verts, mx + 20.0f, iy - 4.0f, mw - 40.0f, 24.0f,
                                 simd_make_float4(0.902f, 0.078f, 0.078f, 0.92f));
                }
                draw_ui_text(verts, CHAPTERS[i], mx + 30.0f, iy, 1.8f,
                             is_sel ? simd_make_float4(1.0f, 1.0f, 1.0f, 1.0f)
                                    : simd_make_float4(0.82f, 0.86f, 0.92f, 0.90f));
            }
        }
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
        // Camera View & Projection Matrices (Unreal Engine to Metal Canonical)
        // ---------------------------------------------------------------------
        Vec3 cam_pos = telemetry.position + Vec3(0.0f, 0.0f, telemetry.eye_height);
        Rotator rot = Rotator::from_degrees(telemetry.pitch_deg, telemetry.yaw_deg, telemetry.camera_roll_deg);
        Vec3 fwd = rot.forward();
        Vec3 right = rot.right();
        Vec3 up = rot.up();
        Vec3 target = cam_pos + fwd * 100.0f;

        Mat4 view = Mat4::look_at(cam_pos, target, up);
        float aspect = float(impl_->width) / float(impl_->height);
        float fov_rad = telemetry.fov_deg * DEG2RAD;
        Mat4 proj = Mat4::perspective(fov_rad, aspect, 5.0f, 65000.0f);
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

        Vec3 sun_d = scene.sun_direction.normalized();
        uniforms.sun_dir = simd_make_float3(sun_d.x, sun_d.y, sun_d.z);
        uniforms.sun_color = simd_make_float3(1.0f, 0.98f, 0.95f);
        uniforms.sky_color = simd_make_float3(0.68f, 0.84f, 1.0f);
        uniforms.ground_color = simd_make_float3(0.82f, 0.84f, 0.88f);
        uniforms.speed_2d = telemetry.speed_2d;
        uniforms.reaction_active = telemetry.reaction_active ? 1.0f : 0.0f;
        uniforms.health = telemetry.health;
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
        // Pass 1: 3D Scene Geometry & Sky -> HDR Texture
        // ---------------------------------------------------------------------
        MTLRenderPassDescriptor* scenePass = [MTLRenderPassDescriptor renderPassDescriptor];
        scenePass.colorAttachments[0].texture = impl_->scene_hdr_tex;
        scenePass.colorAttachments[0].loadAction = MTLLoadActionClear;
        scenePass.colorAttachments[0].storeAction = MTLStoreActionStore;
        scenePass.colorAttachments[0].clearColor = MTLClearColorMake(0.65, 0.82, 0.98, 1.0);

        scenePass.depthAttachment.texture = impl_->offscreen_depth_tex;
        scenePass.depthAttachment.loadAction = MTLLoadActionClear;
        scenePass.depthAttachment.storeAction = MTLStoreActionDontCare;
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
        for (const auto& m : scene.meshes) total_scene_verts += m.vertices.size();

        if (scene.map_name != impl_->cached_map_name || total_scene_verts != impl_->cached_total_verts ||
            impl_->cached_mesh_buffers.size() != scene.meshes.size()) {
            impl_->cached_map_name = scene.map_name;
            impl_->cached_total_verts = total_scene_verts;
            impl_->cached_mesh_buffers.clear();
            for (const auto& m : scene.meshes) {
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

        if (!scene.meshes.empty()) {
            for (size_t i = 0; i < scene.meshes.size(); ++i) {
                const auto& mesh = scene.meshes[i];
                if (mesh.vertices.empty() || !impl_->cached_mesh_buffers[i]) continue;
                uniforms.is_runner_vision = mesh.is_runner_vision ? 1.0f : 0.0f;
                [enc setVertexBuffer:impl_->cached_mesh_buffers[i] offset:0 atIndex:0];
                [enc setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1];
                [enc setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
                [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:mesh.vertices.size()];
            }
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

        // B2. Render 3D Articulated KrugerSec / CPF SWAT Enemies
        if (!scene.enemies.empty() && !impl_->enemy_guard_mesh.empty()) {
            bind_vertex_bytes_or_buffer(enc, impl_->enemy_guard_mesh.data(),
                                        impl_->enemy_guard_mesh.size() * sizeof(Vertex), 0);
            for (const auto& bot : scene.enemies) {
                if (!bot.alive) continue;
                Mat4 bot_model = Mat4::translation(bot.position) * Mat4::rotation_z(bot.yaw_deg * DEG2RAD);
                std::memcpy(&uniforms.model, bot_model.m, sizeof(float) * 16);
                uniforms.is_runner_vision = (bot.disarm_window && !bot.stunned) ? 0.35f : 0.0f;
                uniforms.actor_tint = simd_make_float3(1.0f, 1.0f, 1.0f);
                [enc setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1];
                [enc setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
                size_t v_count = impl_->enemy_guard_mesh.size();
                if (bot.stunned && v_count > 108) v_count -= 108; // Omit disarmed 3-part weapon
                [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:v_count];
            }
            std::memcpy(&uniforms.model, identity.m, sizeof(float) * 16);
        }

        // C. Draw First-Person Faith Viewmodel (CH_Faith_1P in DPG_Foreground depth range [0.0, 0.05])
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
        // Pass 3: 2D HUD & Font Overlay (Vector / ASCII Bitmap Quads)
        // ---------------------------------------------------------------------
        std::vector<HUDVertex> hud_verts;
        impl_->draw_hud(hud_verts, scene, telemetry);

        if (!hud_verts.empty()) {
            [postEnc setRenderPipelineState:impl_->hud_pipeline];
            simd_float2 screen_size = simd_make_float2(float(impl_->width), float(impl_->height));
            bind_vertex_bytes_or_buffer(postEnc, hud_verts.data(), hud_verts.size() * sizeof(HUDVertex), 0);
            [postEnc setVertexBytes:&screen_size length:sizeof(screen_size) atIndex:1];
            [postEnc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:hud_verts.size()];
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
