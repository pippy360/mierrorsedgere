#pragma once

// -----------------------------------------------------------------------------
// Baked lighting: the light maps Beast wrote for the level's static geometry.
//
// Mirror's Edge is lit almost entirely offline. Every StaticMeshComponent carries,
// after its tagged properties, its LODData: per LOD the static shadow maps of the
// lights that are not in the light map, and an FLightMap, one of
//
//   FLightMap2D  three DXT1 "directional coefficient" textures and a simple one, all
//                LightMapTexture2D exports of the same level package, each with a scale
//                vector, and where the mesh's light-map UVs sit in them
//                (CoordinateScale / CoordinateBias). Many components share one set of
//                textures: they are atlases.
//   FLightMap1D  one sample per mesh vertex: three colours and three scale vectors.
//
// The level's BSP is lit the same way. A UModel is drawn by ModelComponents, each a
// list of elements (one material, some of the model's nodes), and every element has
// an FLightMap2D of its own; the model's vertices carry where they sit in it
// (ShadowTexCoord, 0..1 across the element's rectangle of the atlas).
//
// The three coefficients are the light arriving along the three directions of the
// Half-Life 2 basis in tangent space; BasePassPixelShader.usf weights them by the
// material's normal (docs/RENDERING_RE.md). What is read here is exactly what that
// shader is given: sRGB-encoded 8-bit texels or pow-2.2-encoded vertex colours, and
// the scales that take them to linear light.
// -----------------------------------------------------------------------------

#include "scene_materials.hpp"
#include "../math/types.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace me {

class PackageManager;
class UPKPackage;

// A linear RGB value in 32 bits: 9-bit mantissas sharing a 5-bit exponent (the layout of
// DXGI_FORMAT_R9G9B9E5_SHAREDEXP). How a vertex carries a light-map scale or sample.
uint32_t pack_rgb9e5(const Vec3& rgb);

// The light map of a StaticMeshComponent export's LODData[0].
ActorLightMap read_component_lightmap(const UPKPackage& pkg, int32_t component_export_1based);

// The light maps of the ModelComponents that draw a UModel export. `node_map[n]` is the place in
// `maps` of the light map of the element that draws node n, or -1: no element draws it, or the
// element was baked none (it then takes no light at all).
void read_model_lightmaps(const UPKPackage& pkg, int32_t model_export_1based, std::vector<ActorLightMap>& maps,
                          std::vector<int32_t>& node_map);

// The texture sets the level's components use, numbered in the order they are first asked for.
class LightMapSets {
public:
    // The set made of those three LightMapTexture2D exports of the package at `package_path`.
    int32_t index_of(const std::string& package_path, const int32_t textures[3]);

    // Loads every set's three textures into `out` (3 * set count entries, set by set). A texture
    // that cannot be read is left invalid; the renderer then lights that set with nothing.
    void load(PackageManager& pm, const std::vector<std::shared_ptr<UPKPackage>>& packages, std::vector<SceneTexture>& out) const;

    [[nodiscard]] size_t size() const { return sets_.size(); }

private:
    struct Set {
        std::string package_path;
        int32_t textures[3] = {0, 0, 0};
    };
    std::vector<Set> sets_;
    std::unordered_map<std::string, int32_t> by_key_;
};

}  // namespace me
