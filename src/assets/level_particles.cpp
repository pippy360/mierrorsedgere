#include "level_particles.hpp"

#include "ue3_props.hpp"
#include "upk_loader.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>

namespace me {

namespace {

using Defaults = std::vector<const UPropertyList*>;

// A property of an export, or of its class's default object when the export does not save it.
const UProperty* own_or_default(const UPropertyList& props, const Defaults& defaults, const char* name) {
    if (const UProperty* p = find_prop(props, name)) return p;
    for (const UPropertyList* list : defaults) {
        if (const UProperty* p = find_prop(*list, name)) return p;
    }
    return nullptr;
}

bool flag(const UPropertyList& props, const Defaults& defaults, const char* name, bool fallback) {
    const UProperty* p = own_or_default(props, defaults, name);
    return p ? p->b : fallback;
}

float number(const UPropertyList& props, const Defaults& defaults, const char* name, float fallback) {
    const UProperty* p = own_or_default(props, defaults, name);
    if (!p) return fallback;
    return p->type == "IntProperty" ? static_cast<float>(p->i) : p->f;
}

// A member of a tagged struct property: the export's, or the class default's.
const UProperty* struct_member(const UPropertyList& props, const Defaults& defaults, const char* name, const char* field) {
    if (const UProperty* own = find_prop(props, name)) {
        if (const UProperty* p = find_prop(own->fields, field)) return p;
    }
    for (const UPropertyList* list : defaults) {
        const UProperty* base = find_prop(*list, name);
        if (!base) continue;
        if (const UProperty* p = find_prop(base->fields, field)) return p;
    }
    return nullptr;
}

// A RawDistribution struct value: each member is the export's, or the class default's.
RawDistribution distribution(const UPropertyList& props, const Defaults& defaults, const char* name) {
    RawDistribution d;
    if (const UProperty* table = struct_member(props, defaults, name, "LookupTable")) {
        d.table.reserve(table->ints.size());
        for (int32_t bits : table->ints) {
            float v = 0.0f;
            std::memcpy(&v, &bits, sizeof(v));
            d.table.push_back(v);
        }
    }
    if (const UProperty* p = struct_member(props, defaults, name, "LookupTableChunkSize")) d.chunk = std::max(1, p->i);
    if (const UProperty* p = struct_member(props, defaults, name, "LookupTableTimeScale")) d.time_scale = p->f;
    if (const UProperty* p = struct_member(props, defaults, name, "LookupTableStartTime")) d.start_time = p->f;
    if (const UProperty* p = struct_member(props, defaults, name, "Op")) d.op = p->i;
    if (d.table.size() < static_cast<size_t>(2 + d.chunk)) d.op = 0;  // nothing baked: the value is 0
    d.valid = d.op == 1;
    return d;
}

std::string enum_name(const UPropertyList& props, const Defaults& defaults, const char* name) {
    const UProperty* p = own_or_default(props, defaults, name);
    return p ? p->s : std::string();
}

int32_t material_index(const std::string& path, std::vector<std::string>& material_paths) {
    if (path.empty()) return -1;
    const std::string key = to_lower(path);
    size_t at = 0;
    while (at < material_paths.size() && to_lower(material_paths[at]) != key) ++at;
    if (at == material_paths.size()) material_paths.push_back(path);
    return static_cast<int32_t>(at);
}

struct ModuleShape {
    const char* cls;
    ParticleModuleInfo::Kind kind;
    const char* a;  // distributions, by the module's property names
    const char* b;
    const char* c;
    const char* flags[3];
};

using Kind = ParticleModuleInfo::Kind;

// The modules renderer/particles.hpp runs.
const ModuleShape kModules[] = {
    {"ParticleModuleLifetime", Kind::Lifetime, "Lifetime", nullptr, nullptr, {nullptr, nullptr, nullptr}},
    {"ParticleModuleSize", Kind::Size, "StartSize", nullptr, nullptr, {nullptr, nullptr, nullptr}},
    {"ParticleModuleVelocity", Kind::Velocity, "StartVelocity", "StartVelocityRadial", nullptr, {"bInWorldSpace", nullptr, nullptr}},
    {"ParticleModuleRotation", Kind::Rotation, "StartRotation", nullptr, nullptr, {nullptr, nullptr, nullptr}},
    {"ParticleModuleRotationRate", Kind::RotationRate, "StartRotationRate", nullptr, nullptr, {nullptr, nullptr, nullptr}},
    {"ParticleModuleColor", Kind::Color, "StartColor", "StartAlpha", nullptr, {"bClampAlpha", nullptr, nullptr}},
    {"ParticleModuleColorOverLife", Kind::ColorOverLife, "ColorOverLife", "AlphaOverLife", nullptr, {"bClampAlpha", nullptr, nullptr}},
    {"ParticleModuleColorScaleOverLife", Kind::ColorScaleOverLife, "ColorScaleOverLife", "AlphaScaleOverLife", nullptr,
     {"bEmitterTime", nullptr, nullptr}},
    {"ParticleModuleSizeMultiplyLife", Kind::SizeMultiplyLife, "LifeMultiplier", nullptr, nullptr, {"MultiplyX", "MultiplyY", "MultiplyZ"}},
    {"ParticleModuleSubUV", Kind::SubUV, "SubImageIndex", nullptr, nullptr, {nullptr, nullptr, nullptr}},
    {"ParticleModuleAccelerationOverLifetime", Kind::AccelerationOverLifetime, "AccelOverLife", nullptr, nullptr,
     {"bAlwaysInWorldSpace", nullptr, nullptr}},
    {"ParticleModuleAcceleration", Kind::Acceleration, "Acceleration", nullptr, nullptr, {nullptr, nullptr, nullptr}},
    {"ParticleModuleLocation", Kind::Location, "StartLocation", nullptr, nullptr, {nullptr, nullptr, nullptr}},
    {"ParticleModuleVelocityOverLifetime", Kind::VelocityOverLifetime, "VelOverLife", nullptr, nullptr, {"Absolute", nullptr, nullptr}},
    {"ParticleModuleRotationRateMultiplyLife", Kind::RotationRateMultiplyLife, "LifeMultiplier", nullptr, nullptr,
     {nullptr, nullptr, nullptr}},
    {"ParticleModuleLocationPrimitiveSphere", Kind::LocationSphere, "StartRadius", "StartLocation", "VelocityScale",
     {"SurfaceOnly", "Velocity", nullptr}},
    {"ParticleModuleLocationPrimitiveCylinder", Kind::LocationCylinder, "StartRadius", "StartLocation", "StartHeight",
     {"SurfaceOnly", "Velocity", "RadialVelocity"}},
    {"ParticleModuleMeshRotation", Kind::MeshRotation, "StartRotation", nullptr, nullptr, {nullptr, nullptr, nullptr}},
    {"ParticleModuleMeshRotationRate", Kind::MeshRotationRate, "StartRotationRate", nullptr, nullptr, {nullptr, nullptr, nullptr}},
    {"ParticleModuleMeshRotationRateMultiplyLife", Kind::MeshRotationRateMultiplyLife, "LifeMultiplier", nullptr, nullptr,
     {nullptr, nullptr, nullptr}},
    {"ParticleModuleOrbit", Kind::Orbit, "OffsetAmount", "RotationAmount", "RotationRateAmount", {nullptr, nullptr, nullptr}},
    {"ParticleModuleLocationEmitter", Kind::LocationEmitter, nullptr, nullptr, nullptr, {"InheritSourceVelocity", nullptr, nullptr}},
    {"ParticleModuleOrientationAxisLock", Kind::AxisLock, nullptr, nullptr, nullptr, {nullptr, nullptr, nullptr}},
};

using MeshFor = std::function<int32_t(const std::string& path, const std::string& name)>;

// One LOD level of an emitter. False when the port cannot run it (`why` says what is in the way;
// empty when the level is simply switched off).
bool read_lod(const UPKPackage& pkg, int32_t lod_export, std::vector<std::string>& material_paths, const MeshFor& mesh_for,
              ParticleEmitterInfo& out, std::string& why) {
    const auto& exports = pkg.get_exports();
    const auto valid = [&](int32_t i) { return i > 0 && static_cast<size_t>(i) <= exports.size(); };
    UPropertyList lod;
    parse_export_properties(pkg, lod_export, lod);
    out.enabled = prop_bool(lod, "bEnabled", true);

    // A mesh emitter draws a static mesh a particle; nothing else with type data is run.
    bool override_material = false;
    if (const int32_t type_data = prop_object(lod, "TypeDataModule"); type_data != 0) {
        const std::string cls = valid(type_data) ? pkg.get_export_class(exports[static_cast<size_t>(type_data) - 1]) : std::string("type data");
        if (cls != "ParticleModuleTypeDataMesh") {
            why = cls;
            return false;
        }
        UPropertyList t;
        parse_export_properties(pkg, type_data, t);
        const Defaults d = script_default_chain("Default__ParticleModuleTypeDataMesh");
        const UProperty* mesh = own_or_default(t, d, "Mesh");
        if (!mesh || mesh->i == 0 || !mesh_for) {
            why = "a mesh emitter without a mesh";
            return false;
        }
        out.mesh = mesh_for(object_canonical_path(pkg, mesh->i), object_full_path(pkg, mesh->i));
        if (out.mesh < 0) {
            why = "a mesh emitter whose mesh is not loaded";
            return false;
        }
        override_material = flag(t, d, "bOverrideMaterial", false);
    }

    // The required module.
    const int32_t required = prop_object(lod, "RequiredModule");
    if (!valid(required)) {
        why = "no required module";
        return false;
    }
    {
        UPropertyList r;
        parse_export_properties(pkg, required, r);
        const Defaults d = script_default_chain("Default__ParticleModuleRequired");
        const UProperty* material = own_or_default(r, d, "Material");
        if (material && material->i != 0) out.material = material_index(object_canonical_path(pkg, material->i), material_paths);
        const std::string alignment = enum_name(r, d, "ScreenAlignment");
        out.screen_alignment = alignment == "PSA_Velocity" ? 2 : (alignment == "PSA_Rectangle" ? 1 : 0);
        out.local_space = flag(r, d, "bUseLocalSpace", false);
        out.kill_on_deactivate = flag(r, d, "bKillOnDeactivate", false);
        out.duration = number(r, d, "EmitterDuration", 1.0f);
        out.loops = static_cast<int32_t>(number(r, d, "EmitterLoops", 0.0f));
        out.delay = number(r, d, "EmitterDelay", 0.0f);
        out.delay_first_loop_only = flag(r, d, "bDelayFirstLoopOnly", false);
        const std::string interpolation = enum_name(r, d, "InterpolationMethod");
        out.interpolation = interpolation == "PSUVIM_Linear" ? 1
                            : interpolation == "PSUVIM_Linear_Blend" ? 2
                            : interpolation == "PSUVIM_Random" ? 3
                            : interpolation == "PSUVIM_Random_Blend" ? 4 : 0;
        out.sub_images_h = std::max(1, static_cast<int32_t>(number(r, d, "SubImages_Horizontal", 1.0f)));
        out.sub_images_v = std::max(1, static_cast<int32_t>(number(r, d, "SubImages_Vertical", 1.0f)));
        out.max_draw_count = flag(r, d, "bUseMaxDrawCount", true) ? static_cast<int32_t>(number(r, d, "MaxDrawCount", 500.0f)) : 0;
    }
    if (out.mesh < 0 && out.material < 0) {
        why = "no material";
        return false;
    }
    // A mesh emitter draws its mesh's own materials unless it is told to use the emitter's.
    if (out.mesh >= 0 && !override_material) out.material = -1;

    // The spawn module.
    const int32_t spawn = prop_object(lod, "SpawnModule");
    if (valid(spawn)) {
        UPropertyList s;
        parse_export_properties(pkg, spawn, s);
        const Defaults d = script_default_chain("Default__ParticleModuleSpawn");
        out.rate = distribution(s, d, "Rate");
        out.rate_scale = distribution(s, d, "RateScale");
        out.process_rate = flag(s, d, "bProcessSpawnRate", true);
        out.process_bursts = flag(s, d, "bProcessBurstList", true);
        if (const UProperty* bursts = find_prop(s, "BurstList")) {
            for (const UPropertyList& b : bursts->elements) {
                ParticleBurst burst;
                burst.count = static_cast<float>(prop_int(b, "Count", 0));
                burst.count_low = static_cast<float>(prop_int(b, "CountLow", -1));
                burst.time = prop_float(b, "Time", 0.0f);
                out.bursts.push_back(burst);
            }
        }
    }

    // The modules, in the level's order; one that is switched off is not there.
    if (const UProperty* modules = find_prop(lod, "Modules")) {
        for (int32_t m : modules->ints) {
            if (!valid(m)) continue;
            const std::string cls = pkg.get_export_class(exports[static_cast<size_t>(m) - 1]);
            UPropertyList props;
            parse_export_properties(pkg, m, props);
            const Defaults d = script_default_chain("Default__" + cls);
            if (!flag(props, d, "bEnabled", true)) continue;
            if (cls == "ParticleModuleMeshMaterial") {
                // The mesh's material slots, replaced.
                if (const UProperty* list = own_or_default(props, d, "MeshMaterials")) {
                    for (int32_t ref : list->ints) {
                        out.mesh_materials.push_back(ref != 0 ? material_index(object_canonical_path(pkg, ref), material_paths) : -1);
                    }
                }
                continue;
            }
            const ModuleShape* shape = nullptr;
            for (const ModuleShape& candidate : kModules) {
                if (cls == candidate.cls) shape = &candidate;
            }
            if (!shape) {
                why = cls;
                return false;
            }
            ParticleModuleInfo info;
            info.kind = shape->kind;
            if (shape->a) info.a = distribution(props, d, shape->a);
            if (shape->b) info.b = distribution(props, d, shape->b);
            if (shape->c) info.c = distribution(props, d, shape->c);
            for (int k = 0; k < 3; ++k) {
                // The size multiplier's axes and the alpha clamp are on unless the level says not.
                const bool fallback = shape->kind == Kind::SizeMultiplyLife || shape->kind == Kind::Color ||
                                      shape->kind == Kind::ColorOverLife;
                if (shape->flags[k]) info.flag[k] = flag(props, d, shape->flags[k], fallback);
            }
            if (shape->kind == Kind::Orbit) {
                // Which of the three are read again every frame (all are read at spawn by default).
                static const char* kOptions[3] = {"OffsetOptions", "RotationOptions", "RotationRateOptions"};
                for (int k = 0; k < 3; ++k) {
                    const UProperty* update = struct_member(props, d, kOptions[k], "bProcessDuringUpdate");
                    info.flag[k] = update && update->b;
                }
            } else if (shape->kind == Kind::LocationEmitter) {
                info.name = to_lower(enum_name(props, d, "EmitterName"));
                info.link = enum_name(props, d, "SelectionMethod") == "ELESM_Sequential" ? 1 : 0;
                info.scale = number(props, d, "InheritSourceVelocityScale", 1.0f);
            } else if (shape->kind == Kind::AxisLock) {
                static const char* kAxes[6] = {"EPAL_X", "EPAL_Y", "EPAL_Z", "EPAL_NEGATIVE_X", "EPAL_NEGATIVE_Y", "EPAL_NEGATIVE_Z"};
                const std::string axis = enum_name(props, d, "LockAxisFlags");
                for (int k = 0; k < 6; ++k) {
                    if (axis == kAxes[k]) info.link = k + 1;
                }
                if (info.link == 0) continue;  // EPAL_NONE, or a rotation about an axis: not locked
            }
            out.modules.push_back(std::move(info));
        }
    }
    return true;
}

// An emitter: its first LOD level, and the lower ones behind it.
bool read_emitter(const UPKPackage& pkg, int32_t emitter_export, std::vector<std::string>& material_paths, const MeshFor& mesh_for,
                  ParticleEmitterInfo& out, std::string& why) {
    const auto& exports = pkg.get_exports();
    const auto valid = [&](int32_t i) { return i > 0 && static_cast<size_t>(i) <= exports.size(); };
    UPropertyList emitter;
    parse_export_properties(pkg, emitter_export, emitter);
    const UProperty* lods = find_prop(emitter, "LODLevels");
    if (!lods || lods->ints.empty() || !valid(lods->ints[0])) {
        why = "no LOD level";
        return false;
    }
    if (!read_lod(pkg, lods->ints[0], material_paths, mesh_for, out, why)) return false;
    if (!out.enabled) {
        why = "";  // switched off in the template: nothing to draw, and nothing missing
        return false;
    }
    out.name = to_lower(prop_name(emitter, "EmitterName", ""));
    for (size_t k = 1; k < lods->ints.size(); ++k) {
        ParticleEmitterInfo lower;
        std::string lower_why;
        // A lower level the port cannot run keeps the level above it.
        if (!valid(lods->ints[k]) || !read_lod(pkg, lods->ints[k], material_paths, mesh_for, lower, lower_why)) {
            lower = out.lower_lods.empty() ? out : out.lower_lods.back();
            lower.lower_lods.clear();
        }
        lower.name = out.name;
        out.lower_lods.push_back(std::move(lower));
    }
    return true;
}

// A ParticleSystem export as a template of the scene, read once. `left_out` counts what stood in
// an emitter's way.
int32_t read_template(const UPKPackage& pkg, int32_t template_export, LevelScene& scene, std::vector<std::string>& material_paths,
                      const MeshFor& mesh_for, std::map<std::string, int>* left_out) {
    const auto& exports = pkg.get_exports();
    const auto valid = [&](int32_t i) { return i > 0 && static_cast<size_t>(i) <= exports.size(); };
    if (!valid(template_export)) return -1;
    const std::string path = object_canonical_path(pkg, template_export);
    const std::string key = to_lower(path);
    for (size_t i = 0; i < scene.particle_templates.size(); ++i) {
        if (to_lower(scene.particle_templates[i].path) == key) return static_cast<int32_t>(i);
    }
    ParticleSystemTemplate t;
    t.path = path;
    UPropertyList props;
    parse_export_properties(pkg, template_export, props);
    const Defaults d = script_default_chain("Default__ParticleSystem");
    t.warmup_time = number(props, d, "WarmupTime", 0.0f);
    t.lod_check_time = number(props, d, "LODDistanceCheckTime", 0.25f);
    if (const UProperty* distances = find_prop(props, "LODDistances")) {
        for (int32_t bits : distances->ints) {
            float v = 0.0f;
            std::memcpy(&v, &bits, sizeof(v));
            t.lod_distances.push_back(v);
        }
    }
    if (const UProperty* list = find_prop(props, "Emitters")) {
        for (int32_t e : list->ints) {
            if (!valid(e)) continue;
            ParticleEmitterInfo info;
            std::string why;
            if (read_emitter(pkg, e, material_paths, mesh_for, info, why)) {
                t.emitters.push_back(std::move(info));
            } else if (!why.empty()) {
                ++t.emitters_left_out;
                if (left_out) ++(*left_out)[why];
            }
        }
    }
    scene.particle_templates.push_back(std::move(t));
    return static_cast<int32_t>(scene.particle_templates.size()) - 1;
}

}  // namespace

int32_t particle_template_for(const UPKPackage& pkg, int32_t template_export, LevelScene& scene,
                              std::vector<std::string>& material_paths, const MeshFor& mesh_for) {
    const int32_t index = read_template(pkg, template_export, scene, material_paths, mesh_for, nullptr);
    return index >= 0 && !scene.particle_templates[static_cast<size_t>(index)].emitters.empty() ? index : -1;
}

void extract_level_particles(const std::vector<std::shared_ptr<UPKPackage>>& packages, LevelScene& scene,
                             std::vector<std::string>& material_paths, const MeshFor& mesh_for) {
    scene.particle_templates.clear();
    scene.particle_systems.clear();
    std::map<std::string, int> left_out;  // what stood in an emitter's way -> emitters
    size_t physx_only = 0, waiting = 0, no_template = 0, emitters = 0, mesh_emitters = 0, emitters_left_out = 0;
    for (const auto& pkg_ptr : packages) {
        if (!pkg_ptr) continue;
        const UPKPackage& pkg = *pkg_ptr;
        const auto& exports = pkg.get_exports();
        const auto valid = [&](int32_t i) { return i > 0 && static_cast<size_t>(i) <= exports.size(); };
        // An export's own saved property, or that of an archetype inside this package (a prefab's).
        const auto chained = [&](int32_t index, const char* name, UProperty& out) {
            for (int guard = 0; valid(index) && guard < 8; ++guard) {
                UPropertyList list;
                parse_export_properties(pkg, index, list);
                if (const UProperty* p = find_prop(list, name)) {
                    out = *p;
                    return true;
                }
                index = exports[static_cast<size_t>(index) - 1].archetype;
            }
            return false;
        };
        for (size_t i = 0; i < exports.size(); ++i) {
            const std::string cls = pkg.get_export_class(exports[i]);
            if (cls != "Emitter" && cls != "TdEmitter") continue;
            // A level's own actors only: the prefabs' archetype actors are not placements.
            if (exports[i].outer_index <= 0 || !valid(exports[i].outer_index) ||
                pkg.get_export_class(exports[static_cast<size_t>(exports[i].outer_index) - 1]) != "Level") {
                continue;
            }
            const int32_t actor_index = static_cast<int32_t>(i) + 1;
            UPropertyList actor;
            parse_export_properties(pkg, actor_index, actor);
            UProperty p;
            if (prop_bool(actor, "bPhysXMutatable", false) && to_lower(prop_name(actor, "Group", "")).find("physxonly") != std::string::npos) {
                ++physx_only;  // shut down at PreBeginPlay without hardware PhysX
                continue;
            }
            if (!chained(actor_index, "ParticleSystemComponent", p) || !valid(p.i)) continue;
            const int32_t component = p.i;
            if (!chained(component, "Template", p) || !valid(p.i)) {
                ++no_template;
                continue;
            }
            const int32_t template_export = p.i;

            const int32_t template_index = read_template(pkg, template_export, scene, material_paths, mesh_for, &left_out);
            if (template_index < 0) continue;
            const ParticleSystemTemplate& t = scene.particle_templates[static_cast<size_t>(template_index)];
            emitters += t.emitters.size();
            for (const ParticleEmitterInfo& e : t.emitters) mesh_emitters += e.mesh >= 0 ? 1 : 0;
            emitters_left_out += static_cast<size_t>(t.emitters_left_out);
            if (t.emitters.empty()) continue;

            ParticleSystemPlacement placement;
            placement.package = to_lower(package_name_of(pkg));
            placement.export_index = actor_index;
            placement.name = package_name_of(pkg) + "." + export_object_name(pkg, actor_index);
            placement.template_index = template_index;
            if (const UProperty* v = find_prop(actor, "Location")) placement.location = Vec3(v->v[0], v->v[1], v->v[2]);
            float pitch = 0.0f, yaw = 0.0f, roll = 0.0f;
            if (const UProperty* r = find_prop(actor, "Rotation")) {
                const float unit = 3.14159265358979f / 32768.0f;
                pitch = static_cast<float>(r->vi[0]) * unit;
                yaw = static_cast<float>(r->vi[1]) * unit;
                roll = static_cast<float>(r->vi[2]) * unit;
            }
            // FRotationMatrix: the actor's axes in the world.
            const float sp = std::sin(pitch), cp = std::cos(pitch), sy = std::sin(yaw), cy = std::cos(yaw);
            const float sr = std::sin(roll), cr = std::cos(roll);
            placement.axis_x = Vec3(cp * cy, cp * sy, sp);
            placement.axis_y = Vec3(sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp);
            placement.axis_z = Vec3(-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp);
            float scale = prop_float(actor, "DrawScale", 1.0f);
            Vec3 scale3(1.0f, 1.0f, 1.0f);
            if (const UProperty* v = find_prop(actor, "DrawScale3D")) scale3 = Vec3(v->v[0], v->v[1], v->v[2]);
            placement.scale = scale3 * scale;
            placement.active = true;
            if (chained(component, "bAutoActivate", p)) placement.active = p.b;
            waiting += placement.active ? 0 : 1;
            scene.particle_systems.push_back(std::move(placement));
        }
    }
    if (!scene.particle_systems.empty() || physx_only > 0 || emitters_left_out > 0) {
        std::cout << "[Level] Particles: " << scene.particle_systems.size() << " systems of " << scene.particle_templates.size()
                  << " templates, " << emitters << " emitters run, " << mesh_emitters << " of them mesh emitters (" << waiting
                  << " systems wait to be switched on; " << emitters_left_out << " emitters left out; " << physx_only
                  << " PhysX-only placements, " << no_template << " without a template)" << std::endl;
        if (std::getenv("ME_PARTICLE_DEBUG")) {
            for (const auto& [why, n] : left_out) std::cout << "[Level]   left out, " << n << " template emitters: " << why << std::endl;
        }
    }
}

}  // namespace me
