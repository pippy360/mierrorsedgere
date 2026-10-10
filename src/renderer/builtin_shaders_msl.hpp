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
    float lm_u, lm_v;  // baked lighting: see me::Vertex
    uint lm0;
    uint lm1;
    uint lm2;
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

// The scene buffer is linear. The stand-in passes below (sky, untextured geometry, characters) were
// written in display values: this takes one of their results to scene values.
inline float4 scene_out(float4 display) {
    return float4(pow(max(display.rgb, float3(0.0)), float3(2.2)), display.a);
}

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

    return scene_out(float4(final_sky, 1.0));
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
                               constant SceneUniforms& S [[buffer(2)]],
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
            // Apply high-resolution tangent-space normal map using screen-space cotangent frame
            // (handles mirrored character UV shells cleanly without seam flips)
            float3 dp2perp = cross(dpdy, vtx_N);
            float3 dp1perp = cross(vtx_N, dpdx);
            float2 duv1 = dfdx(in.uv);
            float2 duv2 = dfdy(in.uv);
            float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
            float3 B = dp2perp * duv1.y + dp1perp * duv2.y;
            float invmax = rsqrt(max(dot(T, T), dot(B, B)) + 1e-12);
            float3 n_ts = swat_n_tex.sample(world_tex_sampler, in.uv).rgb * 2.0 - 1.0;
            n_ts.xy *= 0.85;
            n_ts.z = sqrt(max(1.0 - dot(n_ts.xy, n_ts.xy), 0.04));
            N = normalize(T * (n_ts.x * invmax) + B * (n_ts.y * invmax) + vtx_N * n_ts.z);
        }
    } else {
        if (dot(vtx_N, geo_N) < 0.0) vtx_N = -vtx_N;
        N = normalize(mix(geo_N, vtx_N, 0.55));
    }

    // A character or a weapon under its light environment (renderer/scene_shading_msl.hpp): the
    // diffuse and specular maps, linear, times the multipliers of its material instance.
    if (in.uv2.x > 0.5 && env_given(S)) {
        float3 env_diffuse = in.color.rgb * float3(uniforms.actor_tint);
        float3 env_specular = float3(0.0);
        float env_power = 20.0;
        if (in.uv2.x < 1.12) {
            int env_arch = int(in.uv2.y + 0.5);
            float dm = 4.20;
            float sm = 0.85;
            env_power = 36.0;
            if (env_arch == 1) { dm = 1.25; sm = 0.95; env_power = 18.0; }
            else if (env_arch == 2) { dm = 3.80; sm = 0.85; env_power = 36.0; }
            else if (env_arch == 3) { dm = 1.25; sm = 1.05; env_power = 48.0; }
            else if (env_arch == 4) { dm = 1.48; sm = 1.15; env_power = 48.0; }
            else if (env_arch == 5) { dm = 1.20; sm = 0.60; env_power = 18.0; }
            env_diffuse = swat_d_tex.sample(world_tex_sampler, in.uv).rgb * dm;
            env_specular = swat_s_tex.sample(world_tex_sampler, in.uv).rgb * sm;
        } else if (in.uv2.x < 1.5) {
            env_diffuse = float3(0.006, 0.008, 0.013);  // the visor
            env_specular = float3(0.85, 0.90, 0.98);
            env_power = 64.0;
        } else if (in.uv2.x < 2.5) {
            if (!(in.color.r > 0.85 && in.color.g < 0.20)) {
                env_diffuse = wep_d_tex.sample(world_tex_sampler, in.uv).rgb;
                env_specular = wep_s_tex.sample(world_tex_sampler, in.uv).rgb;
            }
        } else {
            env_diffuse = ammo_d_tex.sample(world_tex_sampler, in.uv).rgb * float3(1.65, 1.35, 0.88);
            env_specular = float3(0.85, 0.68, 0.36);
            env_power = 32.0;
        }
        return float4(env_lighting(S, in.world_pos, N, V, env_diffuse, 1.0, env_specular, env_power), 1.0);
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
        // Dynamic UE3 Spherical Harmonics (_SH) character & weapon lighting
        float char_shadow = mix(0.42, 1.0, shadow);
        float ndl_direct = max(dot(N, L), 0.0) * char_shadow;
        float ndl_wrap = saturate(dot(N, L) * 0.5 + 0.5) * mix(0.65, 1.0, shadow);
        float3 sh_sky = float3(0.30, 0.36, 0.46) * (0.65 + 0.35 * sky_hemi);
        float3 sh_ground = float3(0.14, 0.12, 0.10) * (1.0 - sky_hemi);
        float rim_light = pow(1.0 - saturate(dot(N, V)), 2.8) * 0.38;
        lighting = sh_sky + sh_ground
                 + float3(0.76, 0.72, 0.64) * (0.55 * ndl_direct + 0.45 * ndl_wrap)
                 + float3(0.38, 0.48, 0.66) * rim_light;

        float3 H = normalize(L + V);
        float3 H_sky = normalize(normalize(float3(0.25, -0.35, 0.90)) + V);
        float3 R_env = reflect(-V, N);
        float env_h = saturate(R_env.z * 0.5 + 0.5);
        float3 env_col = mix(float3(0.14, 0.19, 0.28), float3(0.72, 0.82, 0.96), env_h);
        float fresnel = pow(1.0 - saturate(dot(N, V)), 2.8);

        if (in.uv2.x < 1.12) {
            // Per-archetype UE3 MaterialInstanceConstant parameters from cooked CH_TKY_Cop_* / CH_Celeste UPKs:
            // 0 = SWAT, 1 = Patrol, 2 = Support, 3 = Riot, 4 = Pursuit, 5 = Celeste
            int arch = int(in.uv2.y + 0.5);
            float diff_mult = 4.20;
            float spec_pow = 36.0;
            float spec_mult = 0.85;
            float refl_mult = 0.80;
            float3 sss_col = float3(0.078, 0.013, 0.007);
            if (arch == 1) {
                // CH_TKY_Cop_Patrol (white/blue CPF armor)
                diff_mult = 1.25; spec_pow = 18.0; spec_mult = 0.95; refl_mult = 0.48;
                sss_col = float3(0.08, 0.03, 0.02);
            } else if (arch == 2) {
                // CH_TKY_Cop_Support (heavy armor)
                diff_mult = 3.80; spec_pow = 36.0; spec_mult = 0.85; refl_mult = 0.75;
                sss_col = float3(0.075, 0.012, 0.008);
            } else if (arch == 3) {
                // CH_TKY_Cop_Riot
                diff_mult = 1.25; spec_pow = 48.0; spec_mult = 1.05; refl_mult = 0.72;
                sss_col = float3(0.32, 0.09, 0.03);
            } else if (arch == 4) {
                // CH_TKY_Cop_Pursuit
                diff_mult = 1.48; spec_pow = 48.0; spec_mult = 1.15; refl_mult = 0.58;
                sss_col = float3(0.20, 0.06, 0.01);
            } else if (arch == 5) {
                // CH_Celeste
                diff_mult = 1.20; spec_pow = 18.0; spec_mult = 0.60; refl_mult = 0.32;
                sss_col = float3(0.28, 0.05, 0.035);
            }

            float3 d_char = swat_d_tex.sample(world_tex_sampler, in.uv).rgb;
            float3 s_char = swat_s_tex.sample(world_tex_sampler, in.uv).rgb;
            float back_wrap = saturate(-dot(N, L) * 0.5 + 0.35);
            base_albedo = d_char * diff_mult + s_char * 0.14 + sss_col * back_wrap * 0.35;

            float3 refl_add = env_col * s_char * refl_mult * (0.28 + 0.72 * fresnel);
            float3 spec_lobe = s_char * spec_mult * (
                pow(max(dot(N, H), 0.0), spec_pow) * char_shadow +
                pow(max(dot(N, H_sky), 0.0), max(spec_pow * 0.5, 12.0)) * 0.35);
            spec_add = spec_lobe + refl_add;
        } else if (in.uv2.x < 1.5) {
            // UE3 MI_TKY_Cop_SWAT_eye_SH / visor sub-material
            base_albedo = float3(0.08, 0.09, 0.12);
            spec_add = float3(0.85, 0.90, 0.98) * pow(max(dot(N, H), 0.0), 64.0) * 0.80
                     + env_col * 0.35 * fresnel;
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

    return scene_out(float4(lit_color, 1.0));
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
                                   constant SceneUniforms& S [[buffer(2)]],
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
        return scene_out(float4(in.color.rgb * 1.35, 1.0));
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

    // Under the first-person light environment (renderer/scene_shading_msl.hpp; its lights are given
    // relative to the eye, as this mesh is): skin, glove, trousers and brass from their own maps.
    if (env_given(S) && (slot == 1 || slot == 2 || slot == 3 || slot == 5)) {
        float3 env_diffuse;
        float3 env_specular = float3(0.04);
        float env_power = 20.0;
        if (slot == 1) {
            env_diffuse = pow(max(vm_skin_tex.sample(vm_sampler, in.uv).rgb, float3(0.0)), float3(2.2));
        } else if (slot == 2) {
            env_diffuse = pow(max(vm_glove_tex.sample(vm_sampler, in.uv).rgb, float3(0.0)), float3(2.2));
            env_specular = float3(0.08);
            env_power = 26.0;
        } else if (slot == 3) {
            env_diffuse = pow(max(vm_lower_tex.sample(vm_sampler, in.uv).rgb, float3(0.0)), float3(2.2));
        } else {
            env_diffuse = vm_ammo_tex.sample(vm_sampler, in.uv).rgb * float3(1.55, 1.25, 0.78);
            env_specular = float3(0.95, 0.78, 0.40) * 0.5;
            env_power = 36.0;
        }
        return float4(env_lighting(S, in.world_pos, N, V, env_diffuse, 1.0, env_specular, env_power), 1.0);
    }

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
        return scene_out(float4(col, 1.0));
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
        return scene_out(float4(col, 1.0));
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
        return scene_out(float4(albedo * (0.46 + 0.54 * NdotL) + float3(spec), 1.0));
    }

    if (slot == 5) {
        // Weapon M_Ammo Brass Cartridges & Belt (T_Ammo_D, sRGB decoded)
        float3 a_lin = vm_ammo_tex.sample(vm_sampler, in.uv).rgb;
        float3 albedo = a_lin * float3(1.55, 1.25, 0.78) + float3(0.04, 0.03, 0.01);
        float NdotL = max(dot(N, L), 0.0);
        float spec_brass = pow(max(dot(N, H), 0.0), 36.0) * 0.75 + pow(max(dot(N, H), 0.0), 12.0) * 0.25;
        float3 col = albedo * (0.35 + 0.65 * NdotL) + float3(0.95, 0.78, 0.40) * spec_brass;
        return scene_out(float4(col, 1.0));
    }

    if (slot == 6) {
        // Barrett M95 Scope Optical Glass Lens (M_M95_Sight)
        float fresnel = pow(1.0 - max(dot(N, V), 0.0), 2.5);
        float glint = pow(max(dot(N, H), 0.0), 96.0);
        float2 centered = abs(fract(in.uv) - 0.5);
        float crosshair = (min(centered.x, centered.y) < 0.006 && max(centered.x, centered.y) < 0.35) ? 0.18 : 0.0;
        float3 glass_col = float3(0.03, 0.08, 0.14) + float3(0.30, 0.56, 0.88) * fresnel * 0.65 + float3(glint * 0.95) + float3(crosshair * 0.12);
        return scene_out(float4(glass_col, 1.0));
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
    return scene_out(float4(col, 1.0));
}

// -----------------------------------------------------------------------------
// 4. Post-processing: the game's chain (docs/RENDERING_RE.md), from its own shaders.
//    HeightFogPixelShader.usf, TdDirHazePixelShader.usf, DOFAndBloomGather/Blend,
//    FilterPixelShader.usf, TdToneMapExposurePixelShader.usf, TdToneMappingPixelShader.usf
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

// Must match PostUniformsGPU in the renderers.
struct PostUniforms {
    float4 fog_distance_scale;   // per layer: log2(1 - Density)
    float4 fog_extinction;
    float4 fog_start;
    float4 fog_min_height;       // world z
    float4 fog_max_height;
    float4 fog_inscatter[4];     // LightColor / ln(0.5)
    float4 haze_sun;             // xyz: unit vector to HazeSunLocation; w: 1 when the haze is on
    float4 haze_color;           // rgb: HazeColor; w: HazeMultiplier
    float4 haze_packed;          // AngleCurve, AngleStart, DistanceCurve, DistanceDivider
    float4 haze_packed2;         // AngleClampHigh, TotalClampCloseHigh, TotalClampFarHigh, TotalClampFarDistance
    float4 dof_packed;           // FocusDistance, 1 / FocusRadius, FocusExponent, BloomScale
    float4 misc;                 // haze TotalClampLow, near blur clamp, far blur clamp, 0
    float4 exposure;             // Manual, MaxDeltaUp, LowClamp, HighClamp
    float4 exposure2;            // MaxDeltaDown, 1 = settle on the target at once (2 = hold z), held exposure, 0
    float4 shadows_desat;        // SceneShadows, 1 - SceneDesaturation
    float4 inv_highlights;       // 1 / SceneHighLights
    float4 midtones;
    float4 lum_weights;          // (0.3, 0.59, 0.11) * SceneDesaturation
    float4 gamma;                // GammaColorScale, 1 / gamma
    float4 overlay;
    float4 fade;                 // rgb: FadeColor; a: FadeInAmount (1 = the picture, 0 = the colour)
    float4 motion;               // TdMotionBlur: x the blur's amount, 0 = none
    float4 motion_centre;        // xy: where on screen the blur streams from
    float4 curve_m[16];
    float4 curve_b[16];
    float4 filter_taps[16];      // x: offset in texels along the axis, y: weight
    float4 filter_axis;          // xy: one texel along the blur axis, z: tap count
    float4 texel;                // xy: one scene texel, zw: one texel of the pass's source
};

// View-space depth (clip w) of a depth-buffer value: the renderer's projection has near 5,
// far 10000000 (kFarPlane, render_common.hpp) and a depth range of [0.05, 1].
inline float post_linear_depth(float d) {
    float ndc = saturate((d - 0.05) / 0.95);
    return (5.0 * 10000000.0) / (10000000.0 - ndc * (10000000.0 - 5.0));
}
inline float post_scene_depth(depth2d<float> depth_tex, float2 uv) {
    constexpr sampler dsmp(coord::normalized, filter::nearest, address::clamp_to_edge);
    return post_linear_depth(depth_tex.sample(dsmp, uv));
}
// The world-space vector from the eye through a pixel, of view depth 1.
inline float3 post_screen_vector(float2 uv, constant FrameUniforms& F) {
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    return float3(F.cam_forward) + float3(F.cam_right) * (ndc.x * F.fov_tan * F.aspect) + float3(F.cam_up) * (ndc.y * F.fov_tan);
}
// Common.usf: pow is taken of max(|x|, 0.0001).
inline float3 post_pow(float3 x, float3 y) { return pow(max(abs(x), float3(0.0001)), y); }
inline float post_pow(float x, float y) { return pow(max(abs(x), 0.0001), y); }

// HeightFogPixelShader.usf, FourLayerMain. Blended One, SrcAlpha: scene * scattering + fog.
fragment float4 fog_fragment(PostVertexOut in [[stage_in]],
                             depth2d<float> depth_tex [[texture(1)]],
                             constant FrameUniforms& F [[buffer(0)]],
                             constant PostUniforms& P [[buffer(1)]]) {
    float depth = clamp(post_scene_depth(depth_tex, in.uv), 1.0, 65535.0);
    float3 world = post_screen_vector(in.uv, F) * depth;
    float cam_z = float3(F.camera_pos).z;
    float wz = (abs(world.z) <= 0.001) ? 0.001 : world.z;
    float4 min_pct = (P.fog_min_height - cam_z) / wz;
    float4 max_pct = (P.fog_max_height - cam_z) / wz;
    float4 layer = max(float4(0.0), float4(depth) - P.fog_start) * abs(saturate(max_pct) - saturate(min_pct));
    float4 scattering = exp2(P.fog_distance_scale * layer);
    if (layer.x >= P.fog_extinction.x) scattering.x = 0.0;
    if (layer.y >= P.fog_extinction.y) scattering.y = 0.0;
    if (layer.z >= P.fog_extinction.z) scattering.z = 0.0;
    if (layer.w >= P.fog_extinction.w) scattering.w = 0.0;
    float4 in_scattering = scattering - 1.0;
    float a4 = scattering.w;
    float a34 = a4 * scattering.z;
    float a234 = a34 * scattering.y;
    float a1234 = a234 * scattering.x;
    float3 fog = in_scattering.w * P.fog_inscatter[3].rgb + a4 * in_scattering.z * P.fog_inscatter[2].rgb +
                 a34 * in_scattering.y * P.fog_inscatter[1].rgb + a234 * in_scattering.x * P.fog_inscatter[0].rgb;
    return float4(fog, a1234);
}

// The dynamic objects' shadows (renderer/mod_shadow.hpp; ModShadowProjectionPixelShader.usf,
// HardwarePCFMain). Must match ModShadowUniformsGPU.
struct ModShadowUniforms {
    float4 count;       // x: shadows in use
    float4 row0[8];     // world -> the shadow's clip space, a row a member
    float4 row1[8];
    float4 row2[8];
    float4 row3[8];
    float4 light[8];    // xyz: the shadow light's place; w: 1 / its radius
    float4 color[8];    // rgb: lerp(1, ModShadowColor, FadeAlpha); a: the depth bias, of the subject's depth
    float4 cell[8];     // xy: the cell's centre in the map, z: its half-size, w: one texel (uv)
    float4 depth[8];    // x: MinSubjectZ, y: MaxSubjectZ
};

// Blended dest * src: what is returned multiplies the scene.
fragment float4 mod_shadow_fragment(PostVertexOut in [[stage_in]],
                                    depth2d<float> depth_tex [[texture(1)]],
                                    depth2d_array<float> shadow_map [[texture(27)]],
                                    constant FrameUniforms& F [[buffer(0)]],
                                    constant ModShadowUniforms& M [[buffer(1)]]) {
    constexpr sampler cmp(coord::normalized, filter::linear, address::clamp_to_edge, compare_func::less_equal);
    float depth = post_scene_depth(depth_tex, in.uv);
    float3 world = float3(F.camera_pos) + post_screen_vector(in.uv, F) * depth;
    float4 p = float4(world, 1.0);
    float3 result = float3(1.0);
    int n = int(M.count.x + 0.5);
    for (int i = 0; i < n; ++i) {
        float w = dot(M.row3[i], p);
        float min_z = M.depth[i].x;
        float max_z = M.depth[i].y;
        if (w <= min_z) continue;  // on the light's side of the subject
        float2 xy = float2(dot(M.row0[i], p), dot(M.row1[i], p)) / w;
        if (abs(xy.x) >= 1.0 || abs(xy.y) >= 1.0) continue;
        float3 lv = (M.light[i].xyz - world) * M.light[i].w;
        float att = sqrt(saturate(1.0 - dot(lv, lv)));
        if (att <= 0.0) continue;
        // Depth runs 0..1 across the subject's sphere, along the light's axis; what lies beyond is
        // held at 0.999, so that it is tested against the subject's own depth.
        float lin = min((w - min_z) / (max_z - min_z), 0.999) - M.color[i].a;
        float zc = min_z + lin * (max_z - min_z);
        float ref = (max_z / (max_z - min_z)) * (zc - min_z) / max(zc, 1e-4);
        float2 uv = M.cell[i].xy + float2(xy.x, -xy.y) * M.cell[i].z;
        float t = M.cell[i].w;
        // The game's sixteen taps are eight, each taken twice (its offset table is read with a fixed
        // column), turned by 45 degrees.
        float pcf = shadow_map.sample_compare(cmp, uv + float2(-2.121, 0.000) * t, 2, ref)
                  + shadow_map.sample_compare(cmp, uv + float2(-1.414, -0.707) * t, 2, ref)
                  + shadow_map.sample_compare(cmp, uv + float2(-1.414, 0.707) * t, 2, ref)
                  + shadow_map.sample_compare(cmp, uv + float2(-0.707, 0.000) * t, 2, ref)
                  + shadow_map.sample_compare(cmp, uv + float2(-0.707, 1.414) * t, 2, ref)
                  + shadow_map.sample_compare(cmp, uv + float2(0.000, 0.707) * t, 2, ref)
                  + shadow_map.sample_compare(cmp, uv + float2(0.000, 2.121) * t, 2, ref)
                  + shadow_map.sample_compare(cmp, uv + float2(0.707, 1.414) * t, 2, ref);
        pcf *= 0.125;
        float3 shadowed = mix(float3(1.0), M.color[i].rgb, att);
        result *= mix(shadowed, float3(1.0), pcf * pcf);
    }
    return float4(result, 1.0);
}

// TdDirHazePixelShader.usf: a glow towards the sun that grows with distance, added to the scene.
fragment float4 haze_fragment(PostVertexOut in [[stage_in]],
                              texture2d<float> scene_tex [[texture(0)]],
                              depth2d<float> depth_tex [[texture(1)]],
                              sampler smp [[sampler(0)]],
                              constant FrameUniforms& F [[buffer(0)]],
                              constant PostUniforms& P [[buffer(1)]]) {
    float4 color = scene_tex.sample(smp, in.uv);
    if (P.haze_sun.w < 0.5) return color;
    float3 ray = post_screen_vector(in.uv, F);
    float z_correction = length(ray);  // the forward vector is of length 1
    float3 world_vector = ray / z_correction;
    float device_z = min(65535.0, post_scene_depth(depth_tex, in.uv) * z_correction);
    float scene_depth = clamp(post_pow(device_z / P.haze_packed.w, P.haze_packed.z), 0.0, 500.0);
    float sun_view = post_pow(clamp((dot(P.haze_sun.xyz, world_vector) + P.haze_packed.y) / (1.0 + P.haze_packed.y), 0.0,
                                    P.haze_packed2.x), P.haze_packed.x);
    float high = (device_z > P.haze_packed2.w) ? P.haze_packed2.z : P.haze_packed2.y;
    float3 haze = clamp(sun_view * P.haze_color.rgb * scene_depth, float3(P.misc.x), float3(high));
    return float4(P.haze_color.w * haze + color.rgb, color.a);
}

// DepthOfFieldCommon.usf
inline float post_unfocused(float scene_depth, constant PostUniforms& P) {
    float relative = scene_depth - P.dof_packed.x;
    float most = (relative < 0.0) ? P.misc.y : P.misc.z;
    return min(most, post_pow(saturate(abs(relative) * P.dof_packed.y), P.dof_packed.z));
}

// DOFAndBloomGatherPixelShader.usf, Main: a quarter-size picture of what blooms (any channel above
// 1, nearer than 60000) and of what is out of focus, over MAX_SCENE_COLOR.
fragment float4 bloom_gather_fragment(PostVertexOut in [[stage_in]],
                                      texture2d<float> scene_tex [[texture(0)]],
                                      depth2d<float> depth_tex [[texture(1)]],
                                      sampler smp [[sampler(0)]],
                                      constant PostUniforms& P [[buffer(1)]]) {
    float3 bloom = float3(0.0);
    float4 average = float4(0.0);
    for (int i = 0; i < 4; ++i) {
        float2 offset = float2((i & 1) != 0 ? 1.0 : -1.0, (i & 2) != 0 ? 1.0 : -1.0) * P.texel.xy;
        float2 uv = in.uv + offset;
        float3 c = scene_tex.sample(smp, uv).rgb;
        float depth = post_scene_depth(depth_tex, uv);
        average += float4(c, depth);
        if (c.r > 1.0 || c.g > 1.0 || c.b > 1.0) bloom += c * clamp(60000.0 - depth, 0.0, 1.0);
    }
    bloom *= P.dof_packed.w * 0.25;
    average *= 0.25;
    float unfocused = post_unfocused(average.a, P);
    return float4(unfocused * average.rgb + bloom, unfocused) * 0.25;
}

// FilterPixelShader.usf: one axis of the Gaussian, its taps and weights worked out on the CPU.
fragment float4 filter_fragment(PostVertexOut in [[stage_in]],
                                texture2d<float> source_tex [[texture(0)]],
                                sampler smp [[sampler(0)]],
                                constant PostUniforms& P [[buffer(1)]]) {
    float4 sum = float4(0.0);
    int taps = int(P.filter_axis.z);
    for (int i = 0; i < taps; ++i) {
        sum += source_tex.sample(smp, in.uv + P.filter_axis.xy * P.filter_taps[i].x) * P.filter_taps[i].y;
    }
    return sum;
}

// DOFAndBloomBlendPixelShader.usf: the blurred bloom (and what is out of focus) over the scene.
inline float3 post_bloom_blend(texture2d<float> scene_tex, depth2d<float> depth_tex, texture2d<float> blurred_tex,
                               sampler smp, float2 uv, constant PostUniforms& P) {
    float3 focused = scene_tex.sample(smp, uv).rgb;
    float focused_weight = saturate(1.0 - post_unfocused(post_scene_depth(depth_tex, uv), P));
    float4 unfocused = 4.0 * blurred_tex.sample(smp, uv);
    return (focused * focused_weight + unfocused.rgb) / max(focused_weight + unfocused.a, 0.001);
}

// The exposure's metering, first step: the scene as the tone mapper is given it, into the first of
// the fixed-point buffers. texel.xy is a source texel, texel.z the taps per axis (n): n x n taps a
// source texel apart from -n/2, taken here as 4 x 4 over the same span. Nothing leaves above 1.
fragment float4 meter_scene_fragment(PostVertexOut in [[stage_in]],
                                     texture2d<float> scene_tex [[texture(0)]],
                                     depth2d<float> depth_tex [[texture(1)]],
                                     texture2d<float> blurred_tex [[texture(2)]],
                                     sampler smp [[sampler(0)]],
                                     constant PostUniforms& P [[buffer(1)]]) {
    float3 sum = float3(0.0);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            float2 offset = (float2(float(x), float(y)) * 0.25 - 0.5) * P.texel.z * P.texel.xy;
            sum += post_bloom_blend(scene_tex, depth_tex, blurred_tex, smp, in.uv + offset, P);
        }
    }
    return float4(saturate(sum * (1.0 / 16.0)), 1.0);
}

// The later steps: one buffer into the next, the same taps.
fragment float4 meter_fragment(PostVertexOut in [[stage_in]],
                               texture2d<float> source_tex [[texture(0)]],
                               sampler smp [[sampler(0)]],
                               constant PostUniforms& P [[buffer(1)]]) {
    float3 sum = float3(0.0);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            float2 offset = (float2(float(x), float(y)) * 0.25 - 0.5) * P.texel.z * P.texel.xy;
            sum += source_tex.sample(smp, in.uv + offset).rgb;
        }
    }
    return float4(saturate(sum * (1.0 / 16.0)), 1.0);
}

// TdToneMapExposurePixelShader.usf: the exposure moves towards sqrt(0.25 / luminance), between its
// clamps, at a pace set by how far it has to go. Kept as exposure squared, over 64.
fragment float4 exposure_fragment(PostVertexOut in [[stage_in]],
                                  texture2d<float> small_tex [[texture(0)]],
                                  texture2d<float> previous_tex [[texture(1)]],
                                  sampler smp [[sampler(0)]],
                                  constant PostUniforms& P [[buffer(1)]]) {
    // SceneDownsampledTexture: the one texel the metering ends in.
    float3 mean = small_tex.sample(smp, float2(0.5, 0.5)).rgb;
    if (!(mean.r > 0.0) && !(mean.r < 0.0)) mean = float3(0.25);
    float luminosity = dot(mean, float3(0.3, 0.59, 0.11));
    float target = clamp(sqrt(0.25 / clamp(luminosity, 0.0000001, 5000.0)), P.exposure.z, P.exposure.w);
    float last = clamp(sqrt(previous_tex.sample(smp, float2(0.5, 0.5)).r * 64.0), P.exposure.z, P.exposure.w);
    float a = abs(target - last);
    float next = last + clamp((target - last) * a, -P.exposure2.x * a * a, P.exposure.y * a * a);
    if (P.exposure2.y > 0.5) next = target;
    if (P.exposure2.y > 1.5) return float4(P.exposure2.z / 64.0);
    return float4(next * next * P.exposure.x / 64.0);
}

// DOFAndBloomBlendPixelShader.usf, then TdToneMappingPixelShader.usf: the blurred bloom over the
// scene, exposure, shadows / highlights / midtones, desaturation, gamma, and the level's curves.
fragment float4 tonemap_fragment(PostVertexOut in [[stage_in]],
                                 texture2d<float> scene_tex [[texture(0)]],
                                 depth2d<float> depth_tex [[texture(1)]],
                                 texture2d<float> blurred_tex [[texture(2)]],
                                 texture2d<float> exposure_tex [[texture(3)]],
                                 sampler smp [[sampler(0)]],
                                 constant FrameUniforms& F [[buffer(0)]],
                                 constant PostUniforms& P [[buffer(1)]]) {
    float3 color = post_bloom_blend(scene_tex, depth_tex, blurred_tex, smp, in.uv, P);

    float exposure = exposure_tex.sample(smp, float2(0.5, 0.5)).r * 64.0;
    color = post_pow(saturate(color * exposure) * P.inv_highlights.rgb - P.shadows_desat.rgb, P.midtones.rgb);
    float scaled_luminance = dot(color, P.lum_weights.rgb);
    float3 graded = P.overlay.rgb + color * P.shadows_desat.a + float3(scaled_luminance);
    float3 toned = post_pow(saturate(graded * P.gamma.rgb), float3(P.gamma.a));

    // The curves: sixteen texels of slope and intercept per channel, looked up at value * 15 / 16.
    int sr = clamp(int(saturate(toned.r) * 15.0), 0, 15);
    int sg = clamp(int(saturate(toned.g) * 15.0), 0, 15);
    int sb = clamp(int(saturate(toned.b) * 15.0), 0, 15);
    toned = float3(toned.r * P.curve_m[sr].r + P.curve_b[sr].r,
                   toned.g * P.curve_m[sg].g + P.curve_b[sg].g,
                   toned.b * P.curve_m[sb].b + P.curve_b[sb].b);
    return float4(saturate(toned), 1.0);
}

// The chain's FadeInEffect (FX_PostProcess.FadeInEffect): the picture towards FadeColor as
// FadeInAmount falls from 1 to 0, what is already near the colour first.
inline float3 post_fade(float3 picture, constant PostUniforms& P) {
    float away = 1.0 - P.fade.a;
    if (away <= 0.0) return picture;
    float3 diff = P.fade.rgb - picture;
    float weight = pow(away, 1.5);
    float near_first = saturate((1.0 - saturate(0.577 * dot(diff, diff))) * 2.5 * away);
    return mix(mix(picture, P.fade.rgb, near_first), picture + diff * weight, weight);
}

// The end of the chain, on the tone-mapped picture: the fade, then TdMotionBlur
// (TdMotionBlurShader.usf, MainPixelShader). The blur streams from the middle of the view and
// touches only its outer part (nothing within 0.6 of the half-diagonal), eight taps of falling
// weight. In the game's shader the step is a scalar, the direction's x, added to both u and v:
// the smear runs along one diagonal everywhere and the middle column has none. Kept as it is.
fragment float4 finish_fragment(PostVertexOut in [[stage_in]],
                                texture2d<float> picture_tex [[texture(0)]],
                                sampler smp [[sampler(0)]],
                                constant PostUniforms& P [[buffer(1)]]) {
    float2 from_centre = float2(1.0 - 2.0 * in.uv.x, 1.0 - 2.0 * in.uv.y);
    float len = length(from_centre);
    float reach = clamp(pow(max(len, 1e-6), 0.1) - 0.95, 0.0, 0.07);
    float step_uv = (len > 1e-6 ? from_centre.x / len : 0.0) * reach * P.motion.x * 0.125;
    if (step_uv == 0.0) return float4(post_fade(picture_tex.sample(smp, in.uv).rgb, P), 1.0);
    float2 lo = 0.5 * P.texel.xy;
    float2 hi = 1.0 - lo;
    float2 uv = in.uv;
    float3 sum = float3(0.0);
    float weight = 1.0;
    for (int k = 0; k < 8; ++k) {
        sum += weight * post_fade(picture_tex.sample(smp, clamp(uv, lo, hi)).rgb, P);
        uv += float2(step_uv, step_uv);
        weight -= 0.125;
    }
    return float4(sum * 0.222222, 1.0);
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
