#include "level_lensflares.hpp"

#include "ue3_props.hpp"
#include "upk_loader.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>

namespace me {

namespace {

// A RawDistributionFloat / RawDistributionVector struct value.
RawDistribution read_distribution(const UProperty* p) {
    RawDistribution d;
    if (!p) return d;
    const UPropertyList& f = p->fields;
    if (const UProperty* table = find_prop(f, "LookupTable")) {
        d.table.reserve(table->ints.size());
        for (int32_t bits : table->ints) {
            float v = 0.0f;
            std::memcpy(&v, &bits, sizeof(v));
            d.table.push_back(v);
        }
    }
    d.chunk = std::max(1, prop_int(f, "LookupTableChunkSize", 1));
    d.time_scale = prop_float(f, "LookupTableTimeScale", 0.0f);
    d.start_time = prop_float(f, "LookupTableStartTime", 0.0f);
    // Op 1 is a constant or a curve; nothing else is used by a lens flare (and Op 0 gives 0).
    d.valid = prop_int(f, "Op", 0) == 1 && d.table.size() >= static_cast<size_t>(2 + d.chunk);
    d.op = d.valid ? 1 : 0;
    return d;
}

int32_t material_index(const std::string& path, std::vector<std::string>& material_paths) {
    if (path.empty()) return -1;
    const std::string key = to_lower(path);
    size_t at = 0;
    while (at < material_paths.size() && to_lower(material_paths[at]) != key) ++at;
    if (at == material_paths.size()) material_paths.push_back(path);
    return static_cast<int32_t>(at);
}

LensFlareElement read_element(const UPKPackage& pkg, const UPropertyList& f, std::vector<std::string>& material_paths) {
    LensFlareElement e;
    e.name = prop_name(f, "ElementName", "");
    e.ray_distance = prop_float(f, "RayDistance", 0.0f);
    e.enabled = prop_bool(f, "bIsEnabled", false);
    e.use_source_distance = prop_bool(f, "bUseSourceDistance", false);
    e.normalize_radial_distance = prop_bool(f, "bNormalizeRadialDistance", false);
    e.modulate_color_by_source = prop_bool(f, "bModulateColorBySource", false);
    if (const UProperty* size = find_prop(f, "Size")) {
        e.size_x = size->v[0];
        e.size_y = size->v[1];
    }
    if (const UProperty* mats = find_prop(f, "LFMaterials")) {
        for (int32_t ref : mats->ints) {
            e.materials.push_back(ref != 0 ? material_index(object_canonical_path(pkg, ref), material_paths) : -1);
        }
    }
    e.material_index = read_distribution(find_prop(f, "LFMaterialIndex"));
    e.scaling = read_distribution(find_prop(f, "Scaling"));
    e.axis_scaling = read_distribution(find_prop(f, "AxisScaling"));
    e.rotation = read_distribution(find_prop(f, "Rotation"));
    e.color = read_distribution(find_prop(f, "Color"));
    e.alpha = read_distribution(find_prop(f, "Alpha"));
    e.dist_scale = read_distribution(find_prop(f, "DistMap_Scale"));
    e.dist_color = read_distribution(find_prop(f, "DistMap_Color"));
    e.dist_alpha = read_distribution(find_prop(f, "DistMap_Alpha"));
    return e;
}

LensFlareTemplate read_template(const UPKPackage& pkg, int32_t export_index, std::vector<std::string>& material_paths) {
    LensFlareTemplate t;
    t.path = object_canonical_path(pkg, export_index);
    UPropertyList props;
    parse_export_properties(pkg, export_index, props);
    t.outer_cone = prop_float(props, "OuterCone", 0.0f);
    t.inner_cone = prop_float(props, "InnerCone", 0.0f);
    t.cone_fudge_factor = prop_float(props, "ConeFudgeFactor", 0.5f);
    t.radius = prop_float(props, "Radius", 0.0f);
    if (const UProperty* map = find_prop(props, "ScreenPercentageMap")) {
        // Saved against the class's own map, a constant 1: a member left out keeps that.
        const RawDistribution saved = read_distribution(map);
        if (find_prop(map->fields, "LookupTable")) t.screen_percentage_map.table = saved.table;
        if (find_prop(map->fields, "LookupTableChunkSize")) t.screen_percentage_map.chunk = saved.chunk;
        if (find_prop(map->fields, "LookupTableTimeScale")) t.screen_percentage_map.time_scale = saved.time_scale;
        if (find_prop(map->fields, "LookupTableStartTime")) t.screen_percentage_map.start_time = saved.start_time;
    }
    if (prop_bool(props, "bUseFixedRelativeBoundingBox", false)) {
        if (const UProperty* box = find_prop(props, "FixedRelativeBoundingBox")) {
            // FBox, native: Min, Max, IsValid.
            const auto& data = pkg.get_data();
            if (box->value_offset + 24 <= data.size()) {
                float v[6];
                std::memcpy(v, data.data() + box->value_offset, sizeof(v));
                const Vec3 extent(std::abs(v[3] - v[0]) * 0.5f, std::abs(v[4] - v[1]) * 0.5f, std::abs(v[5] - v[2]) * 0.5f);
                t.box_center = Vec3((v[0] + v[3]) * 0.5f, (v[1] + v[4]) * 0.5f, (v[2] + v[5]) * 0.5f);
                t.box_extent = extent;
                t.box_inverted = v[3] < v[0];
            }
        }
    }
    // The source element is drawn only when it has a material; no template of the game gives it one.
    if (const UProperty* reflections = find_prop(props, "Reflections")) {
        for (const UPropertyList& f : reflections->elements) {
            LensFlareElement e = read_element(pkg, f, material_paths);
            if (!e.materials.empty()) t.elements.push_back(std::move(e));
        }
    }
    // Drawn in ascending RayDistance (0x010a1580).
    std::stable_sort(t.elements.begin(), t.elements.end(),
                     [](const LensFlareElement& a, const LensFlareElement& b) { return a.ray_distance < b.ray_distance; });
    return t;
}

}  // namespace

void extract_level_lens_flares(const std::vector<std::shared_ptr<UPKPackage>>& packages, LevelScene& scene,
                               std::vector<std::string>& material_paths) {
    scene.lens_flare_templates.clear();
    scene.lens_flares.clear();
    std::map<std::string, int32_t> template_by_path;
    size_t inactive = 0, hidden = 0, without_template = 0, based = 0;
    for (const auto& pkg_ptr : packages) {
        if (!pkg_ptr) continue;
        const UPKPackage& pkg = *pkg_ptr;
        const auto& exports = pkg.get_exports();
        for (size_t i = 0; i < exports.size(); ++i) {
            if (pkg.get_export_class(exports[i]) != "LensFlareSource") continue;
            UPropertyList actor;
            parse_export_properties(pkg, static_cast<int32_t>(i) + 1, actor);
            const int32_t component = prop_object(actor, "LensFlareComp");
            if (component <= 0 || static_cast<size_t>(component) > exports.size()) continue;
            UPropertyList c;
            parse_export_properties(pkg, component, c);
            const int32_t template_ref = prop_object(c, "Template");
            if (template_ref <= 0 || static_cast<size_t>(template_ref) > exports.size()) {
                ++without_template;  // draws nothing
                continue;
            }

            LensFlareSourceInfo source;
            source.name = package_name_of(pkg) + "." + exports[i].object_name;
            const std::string path = to_lower(object_canonical_path(pkg, template_ref));
            auto known = template_by_path.find(path);
            if (known == template_by_path.end()) {
                scene.lens_flare_templates.push_back(read_template(pkg, template_ref, material_paths));
                known = template_by_path.emplace(path, static_cast<int32_t>(scene.lens_flare_templates.size()) - 1).first;
            }
            source.template_index = known->second;
            if (const UProperty* p = find_prop(actor, "Location")) source.location = Vec3(p->v[0], p->v[1], p->v[2]);
            if (const UProperty* r = find_prop(actor, "Rotation")) {
                const float unit = 3.14159265358979f / 32768.0f;
                const float pitch = static_cast<float>(r->vi[0]) * unit;
                const float yaw = static_cast<float>(r->vi[1]) * unit;
                source.forward = Vec3(std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch));
            }
            source.active = prop_bool(c, "bAutoActivate", true);
            source.hidden = prop_bool(actor, "bHidden", false) || prop_bool(c, "HiddenGame", false);
            if (const UProperty* color = find_prop(c, "SourceColor")) {
                for (int k = 0; k < 4; ++k) source.source_color[k] = color->v[k];
            }
            source.has_base = prop_object(actor, "Base") != 0;
            inactive += source.active ? 0 : 1;
            hidden += source.hidden ? 1 : 0;
            based += source.has_base ? 1 : 0;
            scene.lens_flares.push_back(std::move(source));
        }
    }
    if (!scene.lens_flares.empty() || without_template > 0) {
        std::cout << "[Level] Lens flares: " << scene.lens_flares.size() << " sources of " << scene.lens_flare_templates.size()
                  << " templates (" << inactive << " waiting to be switched on, " << hidden << " hidden, " << based
                  << " on a base, " << without_template << " without a template)" << std::endl;
    }
}

}  // namespace me
