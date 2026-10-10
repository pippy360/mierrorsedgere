#include "level_postprocess.hpp"

#include "package_manager.hpp"
#include "ue3_props.hpp"
#include "upk_loader.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace me {

namespace {

Vec3 vec_of(const UProperty& p) { return Vec3(p.v[0], p.v[1], p.v[2]); }

// PostProcessSettings: the members the game's chain reads from the world.
void read_settings(const UPropertyList& f, PostProcessSettings& s) {
    s.scene_desaturation = prop_float(f, "Scene_Desaturation", s.scene_desaturation);
    if (const UProperty* p = find_prop(f, "Scene_HighLights")) s.scene_highlights = vec_of(*p);
    if (const UProperty* p = find_prop(f, "Scene_MidTones")) s.scene_midtones = vec_of(*p);
    if (const UProperty* p = find_prop(f, "Scene_Shadows")) s.scene_shadows = vec_of(*p);

    if (const UProperty* p = find_prop(f, "HazeColor")) s.haze_color = vec_of(*p);
    s.haze_angle_curve = prop_float(f, "HazeAngleCurve", s.haze_angle_curve);
    s.haze_angle_start = prop_float(f, "HazeAngleStart", s.haze_angle_start);
    s.haze_distance_curve = prop_float(f, "HazeDistanceCurve", s.haze_distance_curve);
    s.haze_distance_divider = prop_float(f, "HazeDistanceDivider", s.haze_distance_divider);
    s.haze_angle_clamp_high = prop_float(f, "HazeAngleClampHigh", s.haze_angle_clamp_high);
    s.haze_total_clamp_close_high = prop_float(f, "HazeTotalClampCloseHigh", s.haze_total_clamp_close_high);
    s.haze_total_clamp_far_high = prop_float(f, "HazeTotalClampFarHigh", s.haze_total_clamp_far_high);
    s.haze_total_clamp_far_distance = prop_float(f, "HazeTotalClampFarDistance", s.haze_total_clamp_far_distance);
    s.haze_multiplier = prop_float(f, "HazeMultiplier", s.haze_multiplier);
    s.haze_total_clamp_low = prop_float(f, "HazeTotalClampLow", s.haze_total_clamp_low);
    s.haze_enabled = prop_bool(f, "HazeEnabled", s.haze_enabled);
    if (const UProperty* p = find_prop(f, "HazeSunLocation")) s.haze_sun_location = vec_of(*p);

    s.exposure_manual = prop_float(f, "Scene_ExposureManual", s.exposure_manual);
    s.exposure_speed_up = prop_float(f, "Scene_ExposureSpeedUp", s.exposure_speed_up);
    s.exposure_speed_down = prop_float(f, "Scene_ExposureSpeedDown", s.exposure_speed_down);
    s.exposure_high = prop_float(f, "Scene_ExposureHigh", s.exposure_high);
    s.exposure_low = prop_float(f, "Scene_ExposureLow", s.exposure_low);
    s.interpolation_duration = prop_float(f, "Scene_InterpolationDuration", s.interpolation_duration);

    // Curves: Ms[16] and Bs[16], the slope and intercept of each segment, per channel.
    if (const UProperty* curves = find_prop(f, "Curves")) {
        for (int i = 0; i < 16; ++i) {
            if (const UProperty* p = find_prop(curves->fields, "Ms", i)) s.curve_m[i] = vec_of(*p);
            if (const UProperty* p = find_prop(curves->fields, "Bs", i)) s.curve_b[i] = vec_of(*p);
        }
        s.has_curves = true;
    }
}

}  // namespace

void extract_post_chain(PackageManager& pm, std::vector<PostEffectInfo>& out) {
    out.clear();
    const std::shared_ptr<UPKPackage> pkg = pm.load("FX_PostProcess");
    if (!pkg) return;
    const auto& exports = pkg->get_exports();
    for (size_t i = 0; i < exports.size(); ++i) {
        if (pkg->get_export_class(exports[i]) != "PostProcessChain") continue;
        if (export_object_name(*pkg, static_cast<int32_t>(i) + 1) != "FX_PostProcess") continue;
        UPropertyList chain;
        parse_export_properties(*pkg, static_cast<int32_t>(i) + 1, chain);
        const UProperty* effects = find_prop(chain, "Effects");
        if (!effects) break;
        bool after_tone_mapping = false;
        for (int32_t effect : effects->ints) {
            if (effect <= 0 || static_cast<size_t>(effect) > exports.size()) continue;
            const std::string cls = pkg->get_export_class(exports[static_cast<size_t>(effect) - 1]);
            if (cls == "TdToneMappingPostProcess") after_tone_mapping = true;
            if (cls != "MaterialEffect") continue;
            UPropertyList props;
            parse_export_properties(*pkg, effect, props);
            const int32_t material = prop_object(props, "Material");
            if (material == 0) continue;
            PostEffectInfo info;
            info.name = prop_name(props, "EffectName");
            info.material_path = object_canonical_path(*pkg, material);
            info.show_in_game = prop_bool(props, "bShowInGame", true);
            info.after_tone_mapping = after_tone_mapping;
            out.push_back(std::move(info));
        }
        break;
    }
}

void extract_level_postprocess(const UPKPackage& persistent_level, const std::vector<std::shared_ptr<UPKPackage>>& packages,
                               LevelScene& out) {
    out.post = PostProcessSettings{};
    out.height_fog.clear();

    const auto& exports = persistent_level.get_exports();
    for (size_t i = 0; i < exports.size(); ++i) {
        if (persistent_level.get_export_class(exports[i]) != "WorldInfo") continue;
        UPropertyList props;
        parse_export_properties(persistent_level, static_cast<int32_t>(i) + 1, props);
        if (const UProperty* settings = find_prop(props, "DefaultPostProcessSettings")) read_settings(settings->fields, out.post);
        break;
    }
    // A level that sets no curve gets the straight one (the struct's own default is all zero,
    // which the game cannot mean: it would draw black).
    const auto straighten = [](PostProcessSettings& s) {
        if (s.has_curves) return;
        for (auto& m : s.curve_m) m = Vec3(1.0f, 1.0f, 1.0f);
    };
    straighten(out.post);

    // The volumes: each starts from the struct's defaults, not from the world's settings, so a
    // volume that sets only its curves also turns the haze off and opens the exposure to 0.85..1.65.
    out.post_volumes.clear();
    size_t disabled = 0;
    for (const auto& pkg_ptr : packages) {
        if (!pkg_ptr) continue;
        const UPKPackage& pkg = *pkg_ptr;
        const auto& ex = pkg.get_exports();
        for (size_t i = 0; i < ex.size(); ++i) {
            if (pkg.get_export_class(ex[i]) != "PostProcessVolume") continue;
            UPropertyList actor;
            parse_export_properties(pkg, static_cast<int32_t>(i) + 1, actor);
            PostProcessVolumeInfo volume;
            volume.priority = prop_float(actor, "Priority", 0.0f);
            volume.enabled = prop_bool(actor, "bEnabled", true);
            if (const UProperty* settings = find_prop(actor, "Settings")) read_settings(settings->fields, volume.settings);
            straighten(volume.settings);

            std::vector<std::vector<Vec3>> hulls;
            read_actor_brush_hulls(pkg, static_cast<int32_t>(i) + 1, hulls);
            AABB box(Vec3(1e30f, 1e30f, 1e30f), Vec3(-1e30f, -1e30f, -1e30f));
            for (const auto& triangles : hulls) {
                Vec3 centre(0.0f, 0.0f, 0.0f);
                for (const Vec3& v : triangles) {
                    centre = centre + v;
                    box.expand(v);
                }
                centre = centre * (1.0f / static_cast<float>(triangles.size()));
                std::vector<PostProcessVolumeInfo::Plane> planes;
                for (size_t t = 0; t + 2 < triangles.size(); t += 3) {
                    Vec3 n = (triangles[t + 1] - triangles[t]).cross(triangles[t + 2] - triangles[t]);
                    if (n.length_sq() < 1e-6f) continue;
                    n = n.normalized();
                    if (n.dot(centre - triangles[t]) > 0.0f) n = n * -1.0f;
                    planes.push_back({n, -n.dot(triangles[t])});
                }
                if (planes.size() >= 4) volume.hulls.push_back(std::move(planes));
            }
            if (volume.hulls.empty()) continue;
            volume.bounds = box;
            if (!volume.enabled) ++disabled;
            out.post_volumes.push_back(std::move(volume));
        }
    }
    std::stable_sort(out.post_volumes.begin(), out.post_volumes.end(),
                     [](const PostProcessVolumeInfo& a, const PostProcessVolumeInfo& b) { return a.priority > b.priority; });

    for (const auto& pkg_ptr : packages) {
        if (!pkg_ptr) continue;
        const UPKPackage& pkg = *pkg_ptr;
        const auto& ex = pkg.get_exports();
        for (size_t i = 0; i < ex.size(); ++i) {
            if (pkg.get_export_class(ex[i]) != "HeightFog") continue;
            UPropertyList actor;
            parse_export_properties(pkg, static_cast<int32_t>(i) + 1, actor);
            const int32_t component = prop_object(actor, "Component");
            if (component <= 0 || static_cast<size_t>(component) > ex.size()) continue;
            UPropertyList c;
            parse_export_properties(pkg, component, c);
            if (!prop_bool(c, "bEnabled", true)) continue;

            // HeightFogComponent defaults: Density 0.00005, LightBrightness 1, white, no start
            // distance, an extinction distance past anything. Its height is where the actor stands.
            HeightFogLayer layer;
            if (const UProperty* loc = find_prop(actor, "Location")) layer.height = loc->v[2];
            layer.density = prop_float(c, "Density", 0.00005f);
            layer.start_distance = std::max(0.0f, prop_float(c, "StartDistance", 0.0f));
            layer.extinction_distance = prop_float(c, "ExtinctionDistance", 100000000.0f);
            Vec3 color(1.0f, 1.0f, 1.0f);
            if (const UProperty* lc = find_prop(c, "LightColor")) color = vec_of(*lc);
            const float brightness = prop_float(c, "LightBrightness", 1.0f);
            // FLinearColor(FColor): pow 2.2.
            layer.color = Vec3(std::pow(std::max(color.x, 0.0f), 2.2f), std::pow(std::max(color.y, 0.0f), 2.2f),
                               std::pow(std::max(color.z, 0.0f), 2.2f)) * brightness;
            out.height_fog.push_back(layer);
        }
    }
    // The scene keeps its fogs highest first and draws at most four.
    std::stable_sort(out.height_fog.begin(), out.height_fog.end(),
                     [](const HeightFogLayer& a, const HeightFogLayer& b) { return a.height > b.height; });
    if (out.height_fog.size() > 4) out.height_fog.resize(4);

    const PostProcessSettings& s = out.post;
    std::cout << "[Level] Post-process: exposure " << s.exposure_low << ".." << s.exposure_high << " x " << s.exposure_manual
              << ", highlights (" << s.scene_highlights.x << "," << s.scene_highlights.y << "," << s.scene_highlights.z
              << ") midtones (" << s.scene_midtones.x << "," << s.scene_midtones.y << "," << s.scene_midtones.z << ") shadows ("
              << s.scene_shadows.x << "," << s.scene_shadows.y << "," << s.scene_shadows.z << ") desaturation "
              << s.scene_desaturation << ", curves " << (s.has_curves ? "set" : "straight") << ", haze "
              << (s.haze_enabled ? "on" : "off") << " x " << s.haze_multiplier << "; " << out.post_volumes.size()
              << " volume(s), " << disabled << " switched off; " << out.height_fog.size() << " height fog layer(s)";
    for (const auto& l : out.height_fog) {
        std::cout << " [z " << l.height << ", density " << l.density << ", from " << l.start_distance << ", colour (" << l.color.x
                  << "," << l.color.y << "," << l.color.z << ")]";
    }
    std::cout << std::endl;
}

}  // namespace me
