#include "level_impacts.hpp"

#include "level_particles.hpp"
#include "package_manager.hpp"
#include "ue3_props.hpp"
#include "upk_loader.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace me {

namespace {

// TdWeapon's defaults (TdGame.u): what a surface without a physical material of its own is hit as.
const char* const kDefaultImpactMaterial = "TDPhysicalMaterials.Concrete.PM_Concrete";
// What a bullet meets in a bot: every bot's physics asset is CH_TKY_Cop_SWAT.Male3p_Physics (the
// only one TdSpContent.u and TdSpBossContent.u import), whose bodies are PM_Character_Body, the
// neck and the hands PM_Character_Head; the two have the same impact effect and the same sound.
const char* const kCharacterMaterial = "TDPhysicalMaterials.Character.Body.PM_Character_Body";
const char* const kCharacterHeadMaterial = "TDPhysicalMaterials.Character.Head.PM_Character_Head";
const char* const kDefaultDecalMaterial = "FX_ImpactEffects.Materials.M_FX_Decals_BulletImpacts_Generic_01";

bool export_has_data(const UPKPackage& pkg, int32_t index) {
    const auto& ex = pkg.get_exports();
    if (index <= 0 || static_cast<size_t>(index) > ex.size()) return false;
    const auto& e = ex[static_cast<size_t>(index) - 1];
    return e.serial_size > 0 && e.serial_offset > 0;
}

// A property of an object, or of its class's default object when the object does not save it.
const UProperty* own_or_default(const UPropertyList& props, const char* cls_default, const char* name) {
    if (const UProperty* p = find_prop(props, name)) return p;
    for (const UPropertyList* list : script_default_chain(cls_default)) {
        if (const UProperty* p = find_prop(*list, name)) return p;
    }
    return nullptr;
}

}  // namespace

ImpactLibrary::ImpactLibrary(PackageManager& pm, LevelScene& scene, std::vector<std::string>& material_paths, MeshFor mesh_for)
    : pm_(pm), scene_(scene), material_paths_(material_paths), mesh_for_(std::move(mesh_for)) {
    scene_.physical_materials.clear();
    scene_.default_physical = -1;
    scene_.character_physical = -1;
    scene_.character_head_physical = -1;
    scene_.default_impact_decal = ImpactDecalInfo{};
    scene_.effect_factories.clear();
    scene_.spawned_effects.clear();
    scene_.dynamic_decals.clear();
    scene_.actor_damage.clear();
}

ImpactLibrary::Ref ImpactLibrary::by_path(const std::string& path) {
    if (path.empty()) return {};
    const std::string key = to_lower(path);
    if (auto it = objects_.find(key); it != objects_.end()) return it->second;
    Ref found;
    // The copy cooked into the level first, then the object's own package.
    auto pkgs = pm_.loaded_packages();
    std::sort(pkgs.begin(), pkgs.end(), [](const auto& a, const auto& b) { return a->get_file_path() < b->get_file_path(); });
    for (const auto& pkg : pkgs) {
        const int32_t index = pm_.find_export(*pkg, key);
        if (index > 0 && export_has_data(*pkg, index)) {
            found = Ref{pkg, index};
            break;
        }
    }
    if (!found) {
        const size_t dot = key.find('.');
        if (auto pkg = pm_.load(dot == std::string::npos ? key : key.substr(0, dot))) {
            const int32_t index = pm_.find_export(*pkg, key);
            if (index > 0 && export_has_data(*pkg, index)) found = Ref{pkg, index};
        }
    }
    objects_.emplace(key, found);
    return found;
}

ImpactLibrary::Ref ImpactLibrary::by_ref(const Ref& from, int32_t ref) {
    if (!from.pkg || ref == 0) return {};
    if (ref > 0 && export_has_data(*from.pkg, ref)) return Ref{from.pkg, ref};
    return by_path(object_canonical_path(*from.pkg, ref));
}

int32_t ImpactLibrary::material(const std::string& path) {
    if (path.empty()) return -1;
    const std::string key = to_lower(path);
    size_t at = 0;
    while (at < material_paths_.size() && to_lower(material_paths_[at]) != key) ++at;
    if (at == material_paths_.size()) material_paths_.push_back(path);
    return static_cast<int32_t>(at);
}

int32_t ImpactLibrary::effect(const Ref& from, int32_t ref) {
    const Ref system = by_ref(from, ref);
    if (!system) return -1;
    const std::string cls = object_class_name(*system.pkg, system.index);
    if (cls != "ParticleSystem" && cls != "TdParticleSystem") return -1;
    return particle_template_for(*system.pkg, system.index, scene_, material_paths_, mesh_for_);
}

// The MaxRadius of a cue's SoundNodeAttenuation: where it has faded to nothing.
float ImpactLibrary::cue_max_radius(const Ref& cue) {
    UPropertyList props;
    parse_export_properties(*cue.pkg, cue.index, props);
    Ref node = by_ref(cue, prop_object(props, "FirstNode"));
    for (int depth = 0; node && depth < 8; ++depth) {
        UPropertyList np;
        parse_export_properties(*node.pkg, node.index, np);
        if (object_class_name(*node.pkg, node.index) == "SoundNodeAttenuation") {
            if (const UProperty* p = find_prop(np, "MaxRadius")) {
                if (const UProperty* table = find_prop(p->fields, "LookupTable"); table && table->ints.size() >= 2) {
                    float hi = 0.0f;
                    std::memcpy(&hi, &table->ints[1], sizeof(float));
                    if (hi > 0.0f) return hi;
                }
            }
            return 5000.0f;  // Default__SoundNodeAttenuation
        }
        const UProperty* children = find_prop(np, "ChildNodes");
        if (!children || children->ints.empty()) break;
        node = by_ref(node, children->ints[0]);
    }
    return 2000.0f;
}

void ImpactLibrary::decals(const Ref& from, const std::vector<int32_t>& refs, std::vector<ImpactDecalInfo>& out) {
    for (int32_t ref : refs) {
        const Ref component = by_ref(from, ref);
        if (!component) continue;
        UPropertyList props;
        parse_export_properties(*component.pkg, component.index, props);
        const char* const cls = "Default__DecalComponent";
        ImpactDecalInfo d;
        const int32_t mat = prop_object(props, "DecalMaterial");
        if (mat == 0) continue;
        d.material = material(object_canonical_path(*component.pkg, mat));
        if (const UProperty* p = own_or_default(props, cls, "Width")) d.width = p->f;
        if (const UProperty* p = own_or_default(props, cls, "Height")) d.height = p->f;
        d.rotation = 0.0f;
        if (const UProperty* p = own_or_default(props, cls, "DecalRotation")) d.rotation = p->f;
        if (const UProperty* p = own_or_default(props, cls, "bNoClip")) d.no_clip = p->b;
        if (d.material >= 0 && d.width > 0.0f && d.height > 0.0f) out.push_back(d);
    }
}

int32_t ImpactLibrary::physical(const Ref& r) {
    if (!r || object_class_name(*r.pkg, r.index) != "PhysicalMaterial") return -1;
    const std::string path = object_canonical_path(*r.pkg, r.index);
    const std::string key = to_lower(path);
    if (auto it = physical_by_path_.find(key); it != physical_by_path_.end()) return it->second;
    const int32_t index = static_cast<int32_t>(scene_.physical_materials.size());
    physical_by_path_.emplace(key, index);
    scene_.physical_materials.emplace_back();
    scene_.physical_materials.back().path = path;

    // The list grows while this one is read: it is filled in a copy and stored at the end.
    PhysicalMaterialInfo info;
    info.path = path;
    UPropertyList props;
    parse_export_properties(*r.pkg, r.index, props);
    info.parent = physical(by_ref(r, prop_object(props, "Parent")));
    if (info.parent == index) info.parent = -1;
    const Ref property = by_ref(r, prop_object(props, "PhysicalMaterialProperty"));
    if (property && object_class_name(*property.pkg, property.index) == "TdPhysicalMaterialProperty") {
        UPropertyList pp;
        parse_export_properties(*property.pkg, property.index, pp);
        if (const Ref fx = by_ref(property, prop_object(pp, "TdPhysicalMaterialImpactEffects"))) {
            UPropertyList fp;
            parse_export_properties(*fx.pkg, fx.index, fp);
            static const char* const kAmmo[4] = {"LightAmmo", "HeavyAmmo", "HeliAmmo", "ShotgunPellet"};
            for (int a = 0; a < 4; ++a) info.effects[a] = effect(fx, prop_object(fp, kAmmo[a]));
        }
        if (const Ref snd = by_ref(property, prop_object(pp, "TdPhysicalMaterialImpactSounds"))) {
            UPropertyList sp;
            parse_export_properties(*snd.pkg, snd.index, sp);
            if (const int32_t cue = prop_object(sp, "LightAmmo")) {
                // "A_Effects_Bullet_Impacts.Concrete.9mm_Concrete_Impact": the package, then the cue.
                const std::string cue_path = object_canonical_path(*snd.pkg, cue);
                const size_t dot = cue_path.find('.');
                if (dot != std::string::npos && dot + 1 < cue_path.size()) {
                    info.impact_sound_package = cue_path.substr(0, dot);
                    info.impact_sound = cue_path.substr(dot + 1);
                    if (const Ref c = by_ref(snd, cue)) info.impact_sound_radius = cue_max_radius(c);
                }
            }
        }
        if (const Ref dc = by_ref(property, prop_object(pp, "TdPhysicalMaterialDecals"))) {
            UPropertyList dp;
            parse_export_properties(*dc.pkg, dc.index, dp);
            info.has_decals = true;
            if (const UProperty* p = own_or_default(dp, "Default__TdPhysicalMaterialDecals", "CriticalAngle")) info.critical_angle = p->f;
            static const char* const kImpact[3] = {"Light_Weapon_Impact", "Heavy_Weapon_Impact", "ShotGun_Impact"};
            static const char* const kRicochet[3] = {"Light_Weapon_Ricochet", "Heavy_Weapon_Ricochet", "ShotGun_Ricochet"};
            for (int t = 0; t < 3; ++t) {
                if (const UProperty* list = find_prop(dp, kImpact[t])) decals(dc, list->ints, info.impact[t]);
                if (const UProperty* list = find_prop(dp, kRicochet[t])) decals(dc, list->ints, info.ricochet[t]);
            }
        }
    }
    scene_.physical_materials[static_cast<size_t>(index)] = std::move(info);
    return index;
}

int32_t ImpactLibrary::physical_of_material(const std::string& material_path) {
    if (material_path.empty()) return -1;
    const std::string key = to_lower(material_path);
    if (auto it = physical_by_material_.find(key); it != physical_by_material_.end()) return it->second;
    int32_t found = -1;
    // UMaterialInstance::GetPhysicalMaterial: its own, or its parent's.
    Ref r = by_path(material_path);
    for (int guard = 0; r && guard < 8; ++guard) {
        UPropertyList props;
        parse_export_properties(*r.pkg, r.index, props);
        if (const int32_t ref = prop_object(props, "PhysMaterial")) {
            found = physical(by_ref(r, ref));
            break;
        }
        r = by_ref(r, prop_object(props, "Parent"));
    }
    physical_by_material_.emplace(key, found);
    return found;
}

void ImpactLibrary::read_weapon_defaults() {
    scene_.default_physical = physical(by_path(kDefaultImpactMaterial));
    scene_.character_physical = physical(by_path(kCharacterMaterial));
    scene_.character_head_physical = physical(by_path(kCharacterHeadMaterial));
    // TdWeapon.InitDefaultDecalProperties
    ImpactDecalInfo d;
    d.material = by_path(kDefaultDecalMaterial) ? material(kDefaultDecalMaterial) : -1;
    d.width = 16.0f;
    d.height = 16.0f;
    d.rotation = -360.0f;
    d.no_clip = false;
    scene_.default_impact_decal = d;
}

void ImpactLibrary::read_factories(const std::vector<std::shared_ptr<UPKPackage>>& packages) {
    for (const auto& pkg : packages) {
        if (!pkg) continue;
        const auto& exports = pkg->get_exports();
        for (size_t i = 0; i < exports.size(); ++i) {
            if (pkg->get_export_class(exports[i]) != "ActorFactoryEmitter") continue;
            const Ref factory{pkg, static_cast<int32_t>(i) + 1};
            UPropertyList props;
            parse_export_properties(*pkg, factory.index, props);
            EffectFactory f;
            f.package = to_lower(package_name_of(*pkg));
            f.export_index = factory.index;
            f.template_index = effect(factory, prop_object(props, "ParticleSystem"));
            if (f.template_index >= 0) scene_.effect_factories.push_back(std::move(f));
        }
    }
}

void ImpactLibrary::report() const {
    size_t with_effects = 0, with_decals = 0, decal_templates = 0;
    for (const PhysicalMaterialInfo& p : scene_.physical_materials) {
        bool any = false;
        for (int32_t e : p.effects) any = any || e >= 0;
        with_effects += any ? 1 : 0;
        with_decals += p.has_decals ? 1 : 0;
        for (int t = 0; t < 3; ++t) decal_templates += p.impact[t].size() + p.ricochet[t].size();
    }
    size_t surfaces = 0;
    for (const auto& [path, index] : physical_by_material_) surfaces += index >= 0 ? 1 : 0;
    std::cout << "[Level] Impacts: " << scene_.physical_materials.size() << " physical materials (" << with_effects
              << " with impact effects, " << with_decals << " with decals: " << decal_templates << " templates), named by "
              << surfaces << " of " << physical_by_material_.size() << " materials; default "
              << (scene_.default_physical >= 0 ? "found" : "missing") << "; " << scene_.effect_factories.size()
              << " factory emitters" << std::endl;
    if (std::getenv("ME_PARTICLE_DEBUG")) {
        for (const PhysicalMaterialInfo& p : scene_.physical_materials) {
            std::cout << "[Level]   " << p.path << " parent " << p.parent << " effects " << p.effects[0] << " " << p.effects[1] << " "
                      << p.effects[2] << " " << p.effects[3] << " critical " << p.critical_angle << " decals";
            for (int t = 0; t < 3; ++t) std::cout << " " << p.impact[t].size() << "/" << p.ricochet[t].size();
            std::cout << std::endl;
        }
    }
}

}  // namespace me
