#pragma once

// -----------------------------------------------------------------------------
// The renderer's built-in shaders (authentic Mirror's Edge USF shaders translated),
// in Metal Shading Language: sun shadow depth, sky, world geometry, the first-person
// viewmodel, post-processing and the 2D HUD / UI passes.
//
// The Metal renderer compiles this text as it is. The Direct3D 11 renderer runs the
// same text through renderer/msl_to_hlsl.hpp, so there is one copy of every shader.
// Both prepend sun_shadow_msl() (renderer/sun_shadow.hpp).
// -----------------------------------------------------------------------------

namespace me {

inline constexpr const char* kBuiltinShadersMSL = R"msl(
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

}  // namespace me
