#pragma once

// -----------------------------------------------------------------------------
// What the built-in shaders and the generated material shaders share beside the sun
// shadow lookup: the scene block (fragment buffer 2: SceneUniformsGPU in
// post_process.hpp) and the light a dynamic object takes from its light environment
// (light_environment.hpp). sun_shadow_msl() carries this text after its own, so every
// shader source starts with it.
//
// The arithmetic is the game's own shaders':
//   PointLightPixelShader.usf        Phong times a radial falloff, for the environment's
//                                    synthesized point light
//   SphericalHarmonicLightPixelShader.usf   the rest of the environment, diffuse only:
//                                    band factors 2pi/(1+p), 2pi/(2+p), p 2pi/(3+4p+p^2)
//   BasePassPixelShader.usf          the hemisphere term, when the environment was told
//                                    to make a sky light and not spherical harmonics
// with the basis constants as the executable has them (SHBasisFunction, 0x0117f6d0).
// -----------------------------------------------------------------------------

namespace me {

inline constexpr const char* kSceneShadingMSL = R"msl(
// Must match SceneUniformsGPU (renderer/post_process.hpp): float4s only.
struct SceneUniforms {
    float4 fog_distance_scale;   // per layer: log2(1 - Density)
    float4 fog_extinction;
    float4 fog_start;
    float4 fog_min_height;       // world z
    float4 fog_max_height;
    float4 fog_inscatter[4];     // LightColor / ln(0.5)
    float4 env_point;            // the light environment's point light: xyz where, w its radius (0: none)
    float4 env_point_color;      // rgb; a: 1 when the object being drawn has an environment
    float4 env_sh[9];            // what is left of the environment, as spherical harmonics (rgb each)
    float4 env_sky_upper;        // rgb; a: 1 when the two sky colours stand in for the harmonics
    float4 env_sky_lower;
    float4 flare_color;          // the lens-flare quad being drawn: its colour,
    float4 flare_inputs;         // its radial distance, source distance, occlusion and intensity,
    float4 flare_ray;            // and (x) its ray distance
};

inline bool env_given(constant SceneUniforms& S) { return S.env_point_color.a > 0.5; }

// The environment's light without the surface's colours: the irradiance of the harmonics (or of the
// two sky colours) on a surface facing n, for DiffusePower p.
inline float3 env_ambient(constant SceneUniforms& S, float3 n, float p) {
    if (S.env_sky_upper.a > 0.5) {
        float2 w = float2(0.5 + 0.5 * n.z, 0.5 - 0.5 * n.z);
        w *= w;
        return w.x * S.env_sky_upper.rgb + w.y * S.env_sky_lower.rgb;
    }
    float k0 = 6.2831853 / (1.0 + p);
    float k1 = 6.2831853 / (2.0 + p);
    float k2 = p * 6.2831853 / (3.0 + 4.0 * p + p * p);
    float3 e = S.env_sh[0].rgb * (0.282095 * k0);
    e += S.env_sh[1].rgb * (-0.488603 * n.y * k1);
    e += S.env_sh[2].rgb * (0.488603 * n.z * k1);
    e += S.env_sh[3].rgb * (-0.488603 * n.x * k1);
    e += S.env_sh[4].rgb * (1.092548 * n.x * n.y * k2);
    e += S.env_sh[5].rgb * (-1.092548 * n.y * n.z * k2);
    e += S.env_sh[6].rgb * (0.315392 * (3.0 * n.z * n.z - 1.0) * k2);
    e += S.env_sh[7].rgb * (-1.092548 * n.x * n.z * k2);
    e += S.env_sh[8].rgb * (0.546274 * (n.x * n.x - n.y * n.y) * k2);
    return max(e, float3(0.0)) * ((1.0 + p) * 0.5);
}

// The point light's falloff at a world position, and the unit vector to it.
inline float env_point_falloff(constant SceneUniforms& S, float3 wpos, thread float3& to_light) {
    to_light = float3(0.0, 0.0, 1.0);
    if (S.env_point.w <= 0.0) return 0.0;
    float3 lv = S.env_point.xyz - wpos;
    float d2 = dot(lv, lv);
    to_light = lv * rsqrt(max(d2, 1e-8));
    float a = saturate(1.0 - d2 / (S.env_point.w * S.env_point.w));
    return a * a;
}

// A surface under its object's light environment. n and to_eye are unit, in world space.
inline float3 env_lighting(constant SceneUniforms& S, float3 wpos, float3 n, float3 to_eye,
                           float3 diffuse, float diffuse_power, float3 specular, float specular_power) {
    float p = max(diffuse_power, 0.0);
    float3 l;
    float falloff = env_point_falloff(S, wpos, l);
    float3 c = float3(0.0);
    if (falloff > 0.0) {
        float3 r = 2.0 * n * dot(n, to_eye) - to_eye;
        float3 phong = diffuse * ((1.0 + p) * 0.5) * pow(max(saturate(dot(n, l)), 0.0001), max(p, 0.0001))
                     + specular * pow(max(saturate(dot(r, l)), 0.0001), max(specular_power, 0.0001));
        c += phong * (falloff * S.env_point_color.rgb);
    }
    c += env_ambient(S, n, p) * diffuse;
    return c;
}
)msl";

}  // namespace me
