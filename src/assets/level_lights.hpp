#pragma once

// -----------------------------------------------------------------------------
// The level's lights, for what the light maps do not light: the dynamic objects
// (docs/RENDERING_RE.md, "Light for what is not level geometry").
//
// A light environment gathers the lights the game puts on its world's static light
// list: enabled, the owner not moving (`*Movable` classes do), not `bForceDynamicLight`;
// a `SkyLightToggleable` never is (ULightComponent::HasStaticLighting, 0x00ed9210 in
// the game's executable). The others are on the dynamic list (LevelLight::dynamic_list):
// they light what shares a channel with them directly, environment or not. A lift's cab
// is lit that way, by lamps that ride with it on a channel of their own.
//
// Colour is pow(LightColor / 255, 2.2) * Brightness, for every class
// (FLightSceneInfo's constructor, 0x0103f0d0). Channels are the bits of
// LightingChannelContainer: bInitialized 0, BSP 1, Static 2, Dynamic 3,
// CompositeDynamic 4, Skybox 5, Unnamed_1..6 6..11, Cinematic_1..6 12..17,
// Gameplay_1..4 18..21. A level saves only the members that differ from the class's
// template: PointLight, SpotLight and TdAreaLight start from BSP + Static +
// CompositeDynamic, DirectionalLight and SkyLight from those plus Dynamic.
// -----------------------------------------------------------------------------

#include "../math/types.hpp"

#include <memory>
#include <vector>

namespace me {

class UPKPackage;

namespace lightchannel {
constexpr uint32_t kBSP = 1u << 1;
constexpr uint32_t kStatic = 1u << 2;
constexpr uint32_t kDynamic = 1u << 3;
constexpr uint32_t kCompositeDynamic = 1u << 4;
}  // namespace lightchannel

// The LightingChannels a component saved, over `defaults`.
struct UProperty;
uint32_t read_lighting_channels(const std::vector<UProperty>& component_props, uint32_t defaults);

// How an actor of the DynamicSMActor family is lit: by its light environment where the level
// switched that on (DynamicSMActor.MyLightEnvironment is bEnabled=False, and InterpActor, KActor
// and their kin override nothing), else straight by the lights that share a channel with its mesh;
// not at all when the mesh has bAcceptsLights=False. Either export may be 0 (not saved in the
// level: the class's own).
DynamicLighting read_dynamic_lighting(const UPKPackage& pkg, int32_t environment_export, int32_t mesh_component_export);

void extract_level_lights(const std::vector<std::shared_ptr<UPKPackage>>& packages, std::vector<LevelLight>& out);

}  // namespace me
