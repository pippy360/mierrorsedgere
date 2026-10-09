#pragma once

// -----------------------------------------------------------------------------
// Mirror's Edge material system (UE3 v536 / licensee 43).
//
// Resolves every material referenced by a level (UMaterial, DecalMaterial and
// MaterialInstanceConstant chains, including static switch / component-mask
// permutations), translates each material expression graph to Metal Shading
// Language the same way UE3's FHLSLMaterialTranslator emits HLSL for
// MaterialTemplate.usf, and loads every referenced Texture2D / TextureCube.
//
// Shading follows the shipped BasePassPixelShader.usf: directional light-map
// transfer in the HL2 basis (DiffusePower / SpecularPower), hemisphere sky
// light, emissive, and the five UE3 blend modes. Until real light-maps are
// decoded a "virtual light-map" built from the level's sun is used.
// -----------------------------------------------------------------------------

#include "scene_materials.hpp"

#include <memory>
#include <string>
#include <vector>

namespace me {

class PackageManager;

struct MaterialBuildOptions {
    int max_texture_size = 1024;  // largest mip uploaded (keeps memory bounded)
    int num_threads = 0;          // texture loading threads (0 = hardware concurrency)
    bool verbose = false;         // print per-material diagnostics
};

// Resolves, compiles and loads every material in `material_paths` (full object
// paths such as "B_BD_Commercial.BD_Commercial_01.M_BD_15_01"; "" = engine
// default material). The returned library's materials[i] corresponds to
// material_paths[i]. Never returns nullptr.
std::shared_ptr<SceneMaterialLibrary> build_scene_materials(PackageManager& pm,
                                                           const std::vector<std::string>& material_paths,
                                                           const MaterialBuildOptions& opts = {});

// Answers "which UV sets does this material read?" while the level geometry is being built,
// before build_scene_materials runs: it resolves and translates the material's graph but loads
// no textures. Results are cached per path.
class MaterialUVResolver {
public:
    explicit MaterialUVResolver(PackageManager& pm);
    ~MaterialUVResolver();
    MaterialUVResolver(const MaterialUVResolver&) = delete;
    MaterialUVResolver& operator=(const MaterialUVResolver&) = delete;

    // `material_path` as in build_scene_materials; "" (the engine default material) reads set 0.
    MaterialUVSlots slots(const std::string& material_path);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Shared MSL prelude (vertex function, FMaterialParameters equivalent, lighting).
const char* material_common_msl();

// The prelude followed by a material of no graph that calls each of its entry points: what a
// generated shader looks like to the compiler, for checking the prelude where there is no game
// data to generate one from (--dump-shaders, docs/RENDERING_RE.md).
std::string material_check_msl();

}  // namespace me
