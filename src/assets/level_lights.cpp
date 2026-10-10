#include "level_lights.hpp"

#include "ue3_props.hpp"
#include "upk_loader.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace me {

namespace {

constexpr float kDegToRad = 3.14159265358979f / 180.0f;

Vec3 linear_color(const UProperty* color, float brightness) {
    Vec3 c(1.0f, 1.0f, 1.0f);
    if (color) c = Vec3(color->v[0], color->v[1], color->v[2]);
    return Vec3(std::pow(std::max(c.x, 0.0f), 2.2f), std::pow(std::max(c.y, 0.0f), 2.2f), std::pow(std::max(c.z, 0.0f), 2.2f)) *
           brightness;
}

}  // namespace

uint32_t read_lighting_channels(const UPropertyList& component_props, uint32_t defaults) {
    static const char* kNames[22] = {"bInitialized", "BSP",         "Static",      "Dynamic",     "CompositeDynamic", "Skybox",
                                     "Unnamed_1",    "Unnamed_2",   "Unnamed_3",   "Unnamed_4",   "Unnamed_5",        "Unnamed_6",
                                     "Cinematic_1",  "Cinematic_2", "Cinematic_3", "Cinematic_4", "Cinematic_5",      "Cinematic_6",
                                     "Gameplay_1",   "Gameplay_2",  "Gameplay_3",  "Gameplay_4"};
    uint32_t channels = defaults;
    const UProperty* saved = find_prop(component_props, "LightingChannels");
    if (!saved) return channels;
    for (int bit = 0; bit < 22; ++bit) {
        const UProperty* member = find_prop(saved->fields, kNames[bit]);
        if (!member) continue;
        if (member->b) {
            channels |= 1u << bit;
        } else {
            channels &= ~(1u << bit);
        }
    }
    return channels;
}

DynamicLighting read_dynamic_lighting(const UPKPackage& pkg, int32_t environment_export, int32_t mesh_component_export) {
    const auto& exports = pkg.get_exports();
    // An export's saved properties and those of its archetypes inside this package, nearest first.
    auto chain = [&](int32_t index) {
        std::vector<UPropertyList> lists;
        for (int guard = 0; index > 0 && static_cast<size_t>(index) <= exports.size() && guard < 8; ++guard) {
            lists.emplace_back();
            parse_export_properties(pkg, index, lists.back());
            index = exports[static_cast<size_t>(index) - 1].archetype;
        }
        return lists;
    };
    auto first = [](const std::vector<UPropertyList>& lists, const char* name) -> const UProperty* {
        for (const UPropertyList& list : lists) {
            if (const UProperty* p = find_prop(list, name)) return p;
        }
        return nullptr;
    };

    DynamicLighting l;
    const std::vector<UPropertyList> env = chain(environment_export);
    const std::vector<UPropertyList> mesh = chain(mesh_component_export);
    bool enabled = false;
    if (const UProperty* p = first(env, "bEnabled")) enabled = p->b;
    if (const UProperty* p = first(env, "LightDistance")) l.light_distance = p->f;
    if (const UProperty* p = first(env, "ShadowDistance")) l.shadow_distance = p->f;
    if (const UProperty* p = first(env, "BouncedLightingIntensity")) l.bounce = p->f;
    if (const UProperty* p = first(env, "BouncedLightingDesaturation")) l.bounce_desaturation = p->f;
    if (const UProperty* p = first(env, "LightDesaturation")) l.light_desaturation = p->f;
    if (const UProperty* p = first(env, "AmbientGlow")) l.ambient_glow = Vec3(p->v[0], p->v[1], p->v[2]);
    if (const UProperty* p = first(env, "bSynthesizePointLight")) l.synthesize_point = p->b;
    if (const UProperty* p = first(env, "bSynthesizeSHLight")) l.synthesize_sh = p->b;
    if (const UProperty* p = first(env, "bCastShadows")) l.cast_shadows = p->b;
    if (const UProperty* p = first(env, "AmbientShadowColor")) l.ambient_shadow_color = Vec3(p->v[0], p->v[1], p->v[2]);
    if (const UProperty* p = first(env, "AmbientShadowSourceDirection")) l.ambient_shadow_dir = Vec3(p->v[0], p->v[1], p->v[2]);
    // The mesh casts unless it says not (MeshComponent: CastShadow, bCastDynamicShadow both on).
    if (const UProperty* p = first(mesh, "CastShadow")) l.cast_shadows = l.cast_shadows && p->b;
    if (const UProperty* p = first(mesh, "bCastDynamicShadow")) l.cast_shadows = l.cast_shadows && p->b;

    // A mesh that never saved its channels is put on Dynamic alone when it attaches.
    l.channels = lightchannel::kDynamic;
    for (const UPropertyList& list : mesh) {
        if (find_prop(list, "LightingChannels")) {
            l.channels = read_lighting_channels(list, 0u);
            break;
        }
    }
    bool accepts = true;
    if (const UProperty* p = first(mesh, "bAcceptsLights")) accepts = p->b;
    l.mode = !accepts ? DynamicLighting::Mode::Unlit
                      : (enabled ? DynamicLighting::Mode::Environment : DynamicLighting::Mode::Direct);
    return l;
}

void extract_level_lights(const std::vector<std::shared_ptr<UPKPackage>>& packages, std::vector<LevelLight>& out) {
    out.clear();
    size_t skipped = 0;
    for (const auto& pkg_ptr : packages) {
        if (!pkg_ptr) continue;
        const UPKPackage& pkg = *pkg_ptr;
        const auto& exports = pkg.get_exports();
        for (size_t i = 0; i < exports.size(); ++i) {
            const std::string cls = pkg.get_export_class(exports[i]);
            // PointLight, SpotLight, DirectionalLight, SkyLight, their Toggleable and Movable kinds, TdAreaLight.
            if (cls.find("Light") == std::string::npos || cls.find("Component") != std::string::npos ||
                cls.find("LightMap") != std::string::npos || cls.find("Environment") != std::string::npos) {
                continue;
            }
            UPropertyList actor;
            parse_export_properties(pkg, static_cast<int32_t>(i) + 1, actor);
            const int32_t component = prop_object(actor, "LightComponent");
            if (component <= 0 || static_cast<size_t>(component) > exports.size()) continue;
            const std::string component_cls = pkg.get_export_class(exports[static_cast<size_t>(component) - 1]);
            UPropertyList c;
            parse_export_properties(pkg, component, c);

            LevelLight light;
            if (component_cls.find("Directional") != std::string::npos) {
                light.kind = LevelLight::Kind::Directional;
            } else if (component_cls.find("Sky") != std::string::npos) {
                light.kind = LevelLight::Kind::Sky;
            } else if (component_cls.find("Spot") != std::string::npos) {
                light.kind = LevelLight::Kind::Spot;
            } else {
                light.kind = LevelLight::Kind::Point;
            }
            const bool sky = light.kind == LevelLight::Kind::Sky;
            const bool directional = light.kind == LevelLight::Kind::Directional;

            if (!prop_bool(c, "bEnabled", true)) {
                ++skipped;
                continue;
            }
            // The world's static light list is what a light environment gathers from; the rest
            // light what shares a channel with them directly.
            const bool movable = cls.find("Movable") != std::string::npos;
            const bool toggleable_sky = sky && cls.find("Toggleable") != std::string::npos;
            light.dynamic_list = movable || toggleable_sky || prop_bool(c, "bForceDynamicLight", false);

            light.color = linear_color(find_prop(c, "LightColor"), prop_float(c, "Brightness", 1.0f));
            if (sky) light.lower_color = linear_color(find_prop(c, "LowerColor"), prop_float(c, "LowerBrightness", 0.0f));
            const uint32_t template_channels =
                1u | lightchannel::kBSP | lightchannel::kStatic | lightchannel::kCompositeDynamic |
                ((sky || directional) ? lightchannel::kDynamic : 0u);
            light.channels = read_lighting_channels(c, template_channels);
            light.cast_shadows = prop_bool(c, "CastShadows", !sky);
            light.cast_static_shadows = prop_bool(c, "CastStaticShadows", true);
            light.cast_composite = prop_bool(c, "bCastCompositeShadow", sky || directional);

            if (const UProperty* p = find_prop(actor, "Location")) light.position = Vec3(p->v[0], p->v[1], p->v[2]);
            if (const UProperty* r = find_prop(actor, "Rotation")) {
                // The light travels along its actor's X axis.
                const float unit = 3.14159265358979f / 32768.0f;
                const float pitch = static_cast<float>(r->vi[0]) * unit;
                const float yaw = static_cast<float>(r->vi[1]) * unit;
                light.direction = Vec3(std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch));
            }
            light.radius = std::max(1.0f, prop_float(c, "Radius", 1024.0f));
            light.falloff = prop_float(c, "FalloffExponent", 2.0f);
            if (light.kind == LevelLight::Kind::Spot) {
                // inner = clamp(InnerConeAngle, 0, 89) degrees; outer at least a thousandth of a radian wider.
                const float inner = std::clamp(prop_float(c, "InnerConeAngle", 0.0f), 0.0f, 89.0f) * kDegToRad;
                const float outer = std::clamp(prop_float(c, "OuterConeAngle", 44.0f) * kDegToRad, inner + 0.001f, 89.0f * kDegToRad + 0.001f);
                light.cos_inner = std::cos(inner);
                light.cos_outer = std::cos(outer);
            }
            out.push_back(light);
        }
    }
    size_t counts[4] = {0, 0, 0, 0};
    size_t dynamic = 0;
    for (const LevelLight& l : out) {
        ++counts[static_cast<size_t>(l.kind)];
        dynamic += l.dynamic_list ? 1 : 0;
    }
    std::cout << "[Level] Lights for dynamic objects: " << counts[0] << " directional, " << counts[1] << " point, " << counts[2]
              << " spot, " << counts[3] << " sky; " << dynamic << " of them on the dynamic list (" << skipped
              << " disabled left out)" << std::endl;
}

}  // namespace me
