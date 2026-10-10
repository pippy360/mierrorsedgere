#pragma once

// -----------------------------------------------------------------------------
// What a bullet leaves where it lands, and the emitters the level's script makes
// (docs/RENDERING_RE.md, "Particles" and "Decals"; game/impact_effects.hpp spawns them).
//
// A material names a PhysicalMaterial (Material.PhysMaterial, a material instance's own or
// its parent's). A physical material has a Parent and a TdPhysicalMaterialProperty, which
// holds
//   TdPhysicalMaterialImpactEffects   a ParticleSystem by ammunition: LightAmmo, HeavyAmmo,
//                                     HeliAmmo, ShotgunPellet (the *PhysX ones need PhysX)
//   TdPhysicalMaterialDecals          CriticalAngle and six lists of DecalComponent
//                                     templates: Light_Weapon_, Heavy_Weapon_ and ShotGun_
//                                     Impact / Ricochet
// TdWeapon.SpawnImpactEffects and SpawnImpactDecal look the effect and the decal up on the
// hit surface's physical material, then up its parents, then fall back on the weapon's
// DefaultImpactMaterial (TDPhysicalMaterials.Concrete.PM_Concrete) and on a 16 x 16 decal
// of DefaultDecalMaterial.
//
// The objects are cooked into the level that uses them, or stay in their own packages
// (TDPhysicalMaterials, FX_ImpactEffects) when an always-loaded package holds them.
//
// An ActorFactoryEmitter is the factory of a SeqAct_ActorFactory: it makes an Emitter of
// its ParticleSystem at the action's spawn points.
// -----------------------------------------------------------------------------

#include "../math/types.hpp"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace me {

class PackageManager;
class UPKPackage;

class ImpactLibrary {
public:
    using MeshFor = std::function<int32_t(const std::string& path, const std::string& name)>;

    // Fills scene.physical_materials as they are asked for; their particle templates join
    // scene.particle_templates and their decals' materials `material_paths`.
    ImpactLibrary(PackageManager& pm, LevelScene& scene, std::vector<std::string>& material_paths, MeshFor mesh_for);

    // The physical material of a material, by the material's path: its place in
    // scene.physical_materials, -1 when it names none.
    int32_t physical_of_material(const std::string& material_path);

    // TdWeapon's defaults: scene.default_physical and scene.default_impact_decal.
    void read_weapon_defaults();

    // The ActorFactoryEmitter objects of the level's packages: scene.effect_factories.
    void read_factories(const std::vector<std::shared_ptr<UPKPackage>>& packages);

    void report() const;

private:
    struct Ref {
        std::shared_ptr<UPKPackage> pkg;
        int32_t index = 0;
        explicit operator bool() const { return pkg && index > 0; }
    };

    Ref by_path(const std::string& path);
    Ref by_ref(const Ref& from, int32_t ref);
    int32_t physical(const Ref& r);
    int32_t material(const std::string& path);
    int32_t effect(const Ref& from, int32_t ref);
    float cue_max_radius(const Ref& cue);
    void decals(const Ref& from, const std::vector<int32_t>& refs, std::vector<ImpactDecalInfo>& out);

    PackageManager& pm_;
    LevelScene& scene_;
    std::vector<std::string>& material_paths_;
    MeshFor mesh_for_;
    std::unordered_map<std::string, Ref> objects_;
    std::unordered_map<std::string, int32_t> physical_by_path_;
    std::unordered_map<std::string, int32_t> physical_by_material_;
};

}  // namespace me
