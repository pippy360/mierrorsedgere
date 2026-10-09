#pragma once

// -----------------------------------------------------------------------------
// The constants of the post-process passes (builtin_shaders_msl.hpp, section 4),
// worked out from the level's settings the way the game's effects hand them to
// their shaders. Shared by the Metal and Direct3D renderers.
//
// Height fog (the game's InitFogConstants, read from its executable at 0x0104d0e0): the scene keeps
//   its fogs highest first and uses at most four. A fog fills the slab from its own height down to
//   the next fog's (the lowest one, down to -262144), whichever side the eye is on:
//     FogMaxHeight = Height        FogMinHeight = next lower fog's Height
//     FogDistanceScale = log2(1 - Density)       FogInScattering = LightColor / ln(0.5)
//   The layers are handed over back to front: the fogs under the eye from the lowest up, then the
//   fogs over it from the highest down.
// Bloom (DOFAndBloomEffect of FX_PostProcess, which does not take the world's settings):
//     BloomScale 0.15, BlurKernelSize 16, no depth of field.
//   The blur is a Gaussian of variance = radius, radius = kernel * (view width / 1280) / 4
//   texels of the quarter-size buffer, taken two texels at a time with bilinear taps
//   (Compute1DGaussianFilterKernel).
// Exposure (TdToneMapExposurePixelShader.usf): Manual, speed up * dt, low, high; speed down * dt,
//   the speeds no more than 2.5 up and 3 down (0x012d3b1e). What it meters is the scene the tone
//   mapper is given, shrunk to one texel through 16-bit fixed-point buffers 512, 128, 32, 8, 2 and 1
//   across (DownSampleBuffer512.. and ToneMapingExposureBuffer, 0x012d65bf): nothing in the picture
//   counts for more than 1. Each step averages n x n taps a source texel apart, n the reduction
//   rounded, at most 4 (0x012cffa0). The exposure buffers start at 0.25, which is past every
//   level's high clamp: a level opens at its brightest and comes down.
// Tone mapping (TdToneMappingPixelShader.usf): shadows and 1 - desaturation, 1 / highlights,
//   midtones, (0.3, 0.59, 0.11) * desaturation, colour scale 1 and 1 / 2.2, no overlay, the curves.
// -----------------------------------------------------------------------------

#include "../math/types.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

namespace me {

// The settings in force at a view point: those of the enabled volume of the highest priority that
// holds it, else the world's. `volume` gets the volume's place in the scene's list, or -1.
inline const PostProcessSettings& post_settings_at(const LevelScene& scene, const Vec3& view, int& volume) {
    for (size_t i = 0; i < scene.post_volumes.size(); ++i) {
        const PostProcessVolumeInfo& v = scene.post_volumes[i];
        if (v.enabled && v.holds(view)) {
            volume = static_cast<int>(i);
            return v.settings;
        }
    }
    volume = -1;
    return scene.post;
}

// The view's settings over time (ULocalPlayer::UpdatePostProcessSettings): when the volume in force
// changes, the settings go over to the new ones in a straight line over the new ones'
// Scene_InterpolationDuration. Switches (the haze being on) change at once.
class PostSettingsBlend {
public:
    // `time` in seconds; a still (the same time again), a new level or time running back snaps.
    const PostProcessSettings& update(const LevelScene& scene, const Vec3& view, float time) {
        int volume = -1;
        const PostProcessSettings& target = post_settings_at(scene, view, volume);
        if (!valid_ || map_ != scene.map_name || !(time > last_time_)) {
            current_ = target;
            valid_ = true;
            map_ = scene.map_name;
            volume_ = volume;
            blend_start_ = last_time_ = time;
            return current_;
        }
        if (volume != volume_) {
            volume_ = volume;
            blend_start_ = time;
        }
        const float dt = time - last_time_;
        const float elapsed = std::max(last_time_ - blend_start_, 0.0f);
        const float remaining = std::max(target.interpolation_duration - elapsed, 0.0f);
        const float k = (remaining > dt) ? std::clamp(dt / remaining, 0.0f, 1.0f) : 1.0f;
        const auto mix = [k](float a, float b) { return a + (b - a) * k; };
        const auto mix3 = [k](const Vec3& a, const Vec3& b) { return a + (b - a) * k; };
        PostProcessSettings& c = current_;
        c.scene_desaturation = mix(c.scene_desaturation, target.scene_desaturation);
        c.scene_highlights = mix3(c.scene_highlights, target.scene_highlights);
        c.scene_midtones = mix3(c.scene_midtones, target.scene_midtones);
        c.scene_shadows = mix3(c.scene_shadows, target.scene_shadows);
        c.exposure_manual = mix(c.exposure_manual, target.exposure_manual);
        c.exposure_speed_up = mix(c.exposure_speed_up, target.exposure_speed_up);
        c.exposure_speed_down = mix(c.exposure_speed_down, target.exposure_speed_down);
        c.exposure_high = mix(c.exposure_high, target.exposure_high);
        c.exposure_low = mix(c.exposure_low, target.exposure_low);
        for (int i = 0; i < 16; ++i) {
            c.curve_m[i] = mix3(c.curve_m[i], target.curve_m[i]);
            c.curve_b[i] = mix3(c.curve_b[i], target.curve_b[i]);
        }
        c.has_curves = target.has_curves;
        c.haze_enabled = target.haze_enabled;
        c.haze_color = mix3(c.haze_color, target.haze_color);
        c.haze_angle_curve = mix(c.haze_angle_curve, target.haze_angle_curve);
        c.haze_angle_start = mix(c.haze_angle_start, target.haze_angle_start);
        c.haze_distance_curve = mix(c.haze_distance_curve, target.haze_distance_curve);
        c.haze_distance_divider = mix(c.haze_distance_divider, target.haze_distance_divider);
        c.haze_angle_clamp_high = mix(c.haze_angle_clamp_high, target.haze_angle_clamp_high);
        c.haze_total_clamp_close_high = mix(c.haze_total_clamp_close_high, target.haze_total_clamp_close_high);
        c.haze_total_clamp_far_high = mix(c.haze_total_clamp_far_high, target.haze_total_clamp_far_high);
        c.haze_total_clamp_far_distance = mix(c.haze_total_clamp_far_distance, target.haze_total_clamp_far_distance);
        c.haze_multiplier = mix(c.haze_multiplier, target.haze_multiplier);
        c.haze_total_clamp_low = mix(c.haze_total_clamp_low, target.haze_total_clamp_low);
        c.haze_sun_location = target.haze_sun_location;
        c.interpolation_duration = target.interpolation_duration;
        last_time_ = time;
        return current_;
    }

private:
    PostProcessSettings current_;
    bool valid_ = false;
    std::string map_;
    int volume_ = -1;
    float blend_start_ = 0.0f;
    float last_time_ = 0.0f;
};

// Must match PostUniforms in builtin_shaders_msl.hpp: float4s only.
struct PostUniformsGPU {
    float fog_distance_scale[4];
    float fog_extinction[4];
    float fog_start[4];
    float fog_min_height[4];
    float fog_max_height[4];
    float fog_inscatter[4][4];
    float haze_sun[4];
    float haze_color[4];
    float haze_packed[4];
    float haze_packed2[4];
    float dof_packed[4];
    float misc[4];
    float exposure[4];
    float exposure2[4];
    float shadows_desat[4];
    float inv_highlights[4];
    float midtones[4];
    float lum_weights[4];
    float gamma[4];
    float overlay[4];
    float fade[4];
    float curve_m[16][4];
    float curve_b[16][4];
    float filter_taps[16][4];
    float filter_axis[4];
    float texel[4];
};
static_assert(sizeof(PostUniformsGPU) == (5 + 4 + 15 + 16 * 3 + 2) * 16, "PostUniformsGPU layout");

constexpr float kBloomScale = 0.15f;        // DOFAndBloomEffect_0.BloomScale
constexpr float kBloomKernelSize = 16.0f;   // DOFAndBloomEffect.BlurKernelSize (class default)
constexpr int kFilterDownsample = 4;        // the filter buffer is a quarter of the view each way
constexpr int kMeterSteps = 6;              // the buffers the exposure is metered through
constexpr int kMeterSizes[kMeterSteps] = {512, 128, 32, 8, 2, 1};
constexpr float kExposureStart = 0.25f;     // what the exposure buffers hold when a level opens

// The taps per axis of a metering step from `from` texels across to `to`: the reduction, rounded
// the way the game's float-to-int does (to nearest, halves to even), between 1 and 4.
inline int meter_taps(int from, int to) {
    return std::clamp(static_cast<int>(std::nearbyint(static_cast<float>(from) / static_cast<float>(std::max(1, to)))), 1, 4);
}

// The level's part of the constants, for the settings `s` in force at the view: everything but the
// blur taps, the texel sizes and the fade.
// `settle` puts the exposure on its target at once (a still).
inline void fill_post_uniforms(const LevelScene& scene, const PostProcessSettings& s, const Vec3& camera_pos, float dt, bool settle,
                               PostUniformsGPU& u) {
    u = PostUniformsGPU{};

    // A layer no fog fills: a slab of no height.
    for (int i = 0; i < 4; ++i) {
        u.fog_extinction[i] = 1.0e30f;
        u.fog_min_height[i] = -262144.0f;
        u.fog_max_height[i] = -262144.0f;
    }
    const int fogs = std::min<int>(4, static_cast<int>(scene.height_fog.size()));
    int order[4] = {0, 0, 0, 0};
    int layers = 0;
    {
        int i = fogs - 1;
        while (i >= 0 && !(scene.height_fog[static_cast<size_t>(i)].height > camera_pos.z)) order[layers++] = i--;
        for (int above = 0; above <= i; ++above) order[layers++] = above;
    }
    for (int layer = 0; layer < layers; ++layer) {
        const int at = order[layer];
        const HeightFogLayer& fog = scene.height_fog[static_cast<size_t>(at)];
        u.fog_distance_scale[layer] = std::log2(std::max(1.0e-6f, 1.0f - fog.density));
        u.fog_extinction[layer] = fog.extinction_distance;
        u.fog_start[layer] = std::max(0.0f, fog.start_distance);
        u.fog_max_height[layer] = fog.height;
        u.fog_min_height[layer] = (at + 1 < fogs) ? scene.height_fog[static_cast<size_t>(at) + 1].height : -262144.0f;
        const float k = 1.0f / std::log(0.5f);
        u.fog_inscatter[layer][0] = fog.color.x * k;
        u.fog_inscatter[layer][1] = fog.color.y * k;
        u.fog_inscatter[layer][2] = fog.color.z * k;
    }

    const Vec3 to_sun = (s.haze_sun_location - camera_pos).normalized();
    u.haze_sun[0] = to_sun.x;
    u.haze_sun[1] = to_sun.y;
    u.haze_sun[2] = to_sun.z;
    u.haze_sun[3] = (s.haze_enabled && s.haze_multiplier != 0.0f) ? 1.0f : 0.0f;
    u.haze_color[0] = s.haze_color.x;
    u.haze_color[1] = s.haze_color.y;
    u.haze_color[2] = s.haze_color.z;
    u.haze_color[3] = s.haze_multiplier;
    u.haze_packed[0] = s.haze_angle_curve;
    u.haze_packed[1] = s.haze_angle_start;
    u.haze_packed[2] = s.haze_distance_curve;
    u.haze_packed[3] = std::max(1.0e-3f, s.haze_distance_divider);
    u.haze_packed2[0] = s.haze_angle_clamp_high;
    u.haze_packed2[1] = s.haze_total_clamp_close_high;
    u.haze_packed2[2] = s.haze_total_clamp_far_high;
    u.haze_packed2[3] = s.haze_total_clamp_far_distance;
    u.misc[0] = s.haze_total_clamp_low;

    // No depth of field: both blur clamps stay 0, so nothing is ever out of focus.
    u.dof_packed[0] = 1600.0f;
    u.dof_packed[1] = 1.0f / 2000.0f;
    u.dof_packed[2] = 4.0f;
    u.dof_packed[3] = kBloomScale;

    u.exposure[0] = s.exposure_manual;
    u.exposure[1] = std::min(s.exposure_speed_up, 2.5f) * std::max(dt, 0.0f);
    u.exposure[2] = s.exposure_low;
    u.exposure[3] = s.exposure_high;
    u.exposure2[0] = std::min(s.exposure_speed_down, 3.0f) * std::max(dt, 0.0f);
    u.exposure2[1] = settle ? 1.0f : 0.0f;
    // ME_EXPOSURE=<value>: the exposure held there (the factor the scene is multiplied by), to see
    // what the metering would have to give.
    if (const char* fixed = std::getenv("ME_EXPOSURE")) {
        u.exposure2[1] = 2.0f;
        u.exposure2[2] = static_cast<float>(std::atof(fixed));
    }

    const float keep = 1.0f - s.scene_desaturation;
    u.shadows_desat[0] = s.scene_shadows.x;
    u.shadows_desat[1] = s.scene_shadows.y;
    u.shadows_desat[2] = s.scene_shadows.z;
    u.shadows_desat[3] = keep;
    const auto inv = [](float v) { return std::abs(v) > 1.0e-6f ? 1.0f / v : 1.0e6f; };
    u.inv_highlights[0] = inv(s.scene_highlights.x);
    u.inv_highlights[1] = inv(s.scene_highlights.y);
    u.inv_highlights[2] = inv(s.scene_highlights.z);
    u.midtones[0] = s.scene_midtones.x;
    u.midtones[1] = s.scene_midtones.y;
    u.midtones[2] = s.scene_midtones.z;
    u.lum_weights[0] = 0.3f * s.scene_desaturation;
    u.lum_weights[1] = 0.59f * s.scene_desaturation;
    u.lum_weights[2] = 0.11f * s.scene_desaturation;
    u.gamma[0] = u.gamma[1] = u.gamma[2] = 1.0f;
    u.gamma[3] = 1.0f / 2.2f;
    u.fade[3] = 1.0f;  // FadeInAmount: nothing over the picture
    for (int i = 0; i < 16; ++i) {
        u.curve_m[i][0] = s.curve_m[i].x;
        u.curve_m[i][1] = s.curve_m[i].y;
        u.curve_m[i][2] = s.curve_m[i].z;
        u.curve_b[i][0] = s.curve_b[i].x;
        u.curve_b[i][1] = s.curve_b[i].y;
        u.curve_b[i][2] = s.curve_b[i].z;
    }
}

// The bloom blur's taps along one axis of a filter buffer `buffer_w` x `buffer_h` texels in size,
// for a view `view_w` pixels wide. `horizontal` picks the axis.
inline void fill_filter_taps(int view_w, int buffer_w, int buffer_h, bool horizontal, PostUniformsGPU& u) {
    const float radius = std::clamp(kBloomKernelSize * (static_cast<float>(view_w) / 1280.0f) / static_cast<float>(kFilterDownsample),
                                    1.0e-4f, 15.0f);
    const int reach = std::min(static_cast<int>(std::ceil(radius)), 15);
    const auto normal = [radius](float x) { return std::exp(-x * x / (2.0f * radius)) / std::sqrt(2.0f * 3.14159265f * radius); };
    int count = 0;
    float sum = 0.0f;
    for (int i = -reach; i <= reach && count < 16; i += 2) {
        const float w0 = normal(static_cast<float>(i));
        const float w1 = (i != reach) ? normal(static_cast<float>(i + 1)) : 0.0f;
        const float w = w0 + w1;
        u.filter_taps[count][0] = static_cast<float>(i) + w1 / w;
        u.filter_taps[count][1] = w;
        sum += w;
        ++count;
    }
    for (int i = 0; i < count; ++i) u.filter_taps[i][1] /= sum;
    u.filter_axis[0] = horizontal ? 1.0f / static_cast<float>(buffer_w) : 0.0f;
    u.filter_axis[1] = horizontal ? 0.0f : 1.0f / static_cast<float>(buffer_h);
    u.filter_axis[2] = static_cast<float>(count);
}

}  // namespace me
