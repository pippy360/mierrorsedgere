#pragma once

// -----------------------------------------------------------------------------
// The decals placed in a level (DecalActor / DecalComponent): the dirt, the stains,
// the painted arrows and stripes, the drains (docs/RENDERING_RE.md, "Decals").
//
// A placed decal carries, after its component's tagged properties, the triangles it
// was clipped to on each thing it lies on, made when the level was built
// (UDecalComponent::Serialize, 0x00fc6dc0 in the game's executable):
//
//   int32 NumStaticReceivers
//   per receiver:  int32 Component          the receiving component, an export of the same package
//                  int32 52, int32 NumVertices, the vertices
//                  int32 2,  int32 NumIndices,  uint16 indices (a triangle list)
//                  int32 NumTriangles
//                  int32 light-map type (0 none, 1 FLightMap1D, 2 FLightMap2D), then that light map
//   vertex, 52 bytes: float3 Position, packed TangentX, packed TangentZ (w: the basis' sign),
//                  float2 UV, float2 LightMapCoordinate, 2 x float2 (texture coordinates 1 and 2)
//
// Nothing is projected when the decal is drawn: the stored texture coordinates are used
// as they are, with the ordinary vertex factory. Positions are in the receiver's own space
// on a static mesh, and in the decal's frame on BSP:
//   world = HitLocation - x HitNormal + y HitTangent + z HitBinormal.
//
// The many DecalComponents that belong to no DecalActor are the bullet-hole templates of
// the physical materials; they have no receivers and are not read.
// -----------------------------------------------------------------------------

#include "../math/types.hpp"

#include <memory>
#include <string>
#include <vector>

namespace me {

class UPKPackage;

struct DecalVertex {
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 tangent{1.0f, 0.0f, 0.0f};  // TangentX
    Vec3 normal{0.0f, 0.0f, 1.0f};   // TangentZ
    float tangent_sign = 1.0f;
    float uv[3][2] = {{0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}};  // texture coordinates 0 (the decal's), 1, 2
    float lightmap_uv[2] = {0.0f, 0.0f};                           // before the receiver's light map's scale and bias
};

struct DecalReceiver {
    int32_t component = 0;  // 1-based export of the decal's package
    bool on_bsp = false;    // a ModelComponent: positions in the decal's frame
    std::vector<DecalVertex> vertices;
    std::vector<uint16_t> indices;
};

struct LevelDecal {
    std::string package;        // package_name_of() the level package it is placed in
    std::string material_path;  // DecalMaterial
    Vec3 hit_location{0.0f, 0.0f, 0.0f};
    Vec3 hit_normal{0.0f, 0.0f, 1.0f};
    Vec3 hit_tangent{1.0f, 0.0f, 0.0f};
    Vec3 hit_binormal{0.0f, 1.0f, 0.0f};
    int32_t sort_order = 0;  // the decals of one receiver are drawn in ascending order
    bool hidden = false;
    std::vector<DecalReceiver> receivers;
};

void extract_level_decals(const std::vector<std::shared_ptr<UPKPackage>>& packages, std::vector<LevelDecal>& out);

}  // namespace me
