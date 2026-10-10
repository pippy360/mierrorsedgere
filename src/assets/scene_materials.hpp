#pragma once

// -----------------------------------------------------------------------------
// Scene material library: the CPU-side result of resolving and compiling every
// Mirror's Edge material used by a level. Produced by the asset side
// (material_system.cpp), consumed by the Metal renderer.
//
// Each UE3 material graph is translated to an MSL fragment function, the same
// way UE3's FHLSLMaterialTranslator turns it into HLSL (MaterialTemplate.usf).
// Material instances (MaterialInstanceConstant chains) that share a parent and
// a static permutation share one shader; their scalar/vector parameters are
// fed through a per-material float4 uniform array and their texture parameters
// through per-material texture slots.
// -----------------------------------------------------------------------------

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace me {

enum class TexFormat : uint8_t { Unknown = 0, DXT1, DXT3, DXT5, BGRA8, G8, V8U8 };

enum class TexAddress : uint8_t { Wrap = 0, Clamp = 1, Mirror = 2 };

struct TextureMip {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> data;
};

struct SceneTexture {
    std::string name;  // full object path
    TexFormat format = TexFormat::Unknown;
    bool srgb = true;
    bool is_cube = false;
    TexAddress address_x = TexAddress::Wrap;
    TexAddress address_y = TexAddress::Wrap;
    std::vector<TextureMip> mips;                    // 2D mip chain (largest first)
    std::array<std::vector<TextureMip>, 6> faces;    // cube faces (+X,-X,+Y,-Y,+Z,-Z), mip chains

    [[nodiscard]] bool valid() const {
        if (is_cube) {
            for (const auto& f : faces) {
                if (f.empty()) return false;
            }
            return true;
        }
        return !mips.empty();
    }
};

enum class MatBlendMode : uint8_t { Opaque = 0, Masked, Translucent, Additive, Modulate };
enum class MatLightingModel : uint8_t { Phong = 0, NonDirectional, Unlit, Custom };

// Default texture used when a slot's texture could not be resolved/loaded.
enum class TexDefault : uint8_t { White = 0, FlatNormal = 1, Black = 2 };

// Fixed binding points used by every generated material fragment function.
namespace matbind {
constexpr int kMaxTextureSlots = 14;     // texture(0..13) / sampler(0..13): material textures
constexpr int kLightMapTexture = 24;     // texture(24..26): the section's three light-map coefficient textures
constexpr int kShadowMapTexture = 27;    // texture(27): directional sun shadow cascades (2-slice depth array)
constexpr int kSceneColorTexture = 28;   // texture(28): copy of opaque scene color (SceneTexture expressions)
constexpr int kSceneDepthTexture = 29;   // texture(29): copy of opaque scene depth (DepthBiasedAlpha)
constexpr int kSceneSampler = 15;        // sampler(15): linear, clamped, all mips: the scene copies and the light maps
constexpr int kFrameBuffer = 0;          // buffer(0): FrameUniforms
constexpr int kMaterialBuffer = 1;       // buffer(1): float4 material uniforms
constexpr int kSceneBuffer = 2;          // buffer(2): SceneUniforms (renderer/post_process.hpp): the height fog
}  // namespace matbind

// One generated MSL fragment shader (shared by all material instances with identical code).
struct MaterialShader {
    std::string function_name;  // MSL fragment entry point, e.g. "mat_ps_12"
    std::string source;         // MSL source of the fragment entry point (relies on common_source)
    int num_tex2d = 0;          // 2D slots occupy texture(0 .. num_tex2d-1)
    int num_texcube = 0;        // cube slots follow at texture(num_tex2d ..)
    int num_uniforms = 0;       // float4 slots in buffer(1)
    MatBlendMode blend = MatBlendMode::Opaque;
    MatLightingModel lighting = MatLightingModel::Phong;
    bool two_sided = false;
    bool uses_scene_color = false;
    bool uses_scene_depth = false;
    std::string base_material;  // for diagnostics
};

// A vertex carries two UV sets (uv0, uv1). A UE3 material may read any of a mesh's sets through
// TextureCoordinate's CoordinateIndex, but almost none reads more than two, so each compiled
// material says which two indices its slots stand for and the level builder fills each mesh
// section's vertices to match. Indices 0 and 1 keep their own slot whenever they are read.
struct MaterialUVSlots {
    int8_t index[2] = {0, 1};  // the TextureCoordinate index slot 0 / slot 1 carries
    [[nodiscard]] bool is_default() const { return index[0] == 0 && index[1] == 1; }
};

// `texcoord_mask`: bit i set when the graph reads TextureCoordinate index i.
inline MaterialUVSlots material_uv_slots(uint32_t texcoord_mask) {
    int a = (texcoord_mask & 1u) ? 0 : -1;
    int b = (texcoord_mask & 2u) ? 1 : -1;
    for (int i = 2; i < 8; ++i) {
        if (!(texcoord_mask & (1u << i))) continue;
        if (b < 0) b = i;
        else if (a < 0) a = i;
    }
    MaterialUVSlots s;
    s.index[0] = static_cast<int8_t>(a < 0 ? 0 : a);
    s.index[1] = static_cast<int8_t>(b < 0 ? 1 : b);
    return s;
}

// The slot that serves TextureCoordinate `index`. A third set has no slot of its own: the slot
// holding the higher index stands in for it.
inline int material_uv_slot(const MaterialUVSlots& s, int index) {
    if (index == s.index[0]) return 0;
    if (index == s.index[1]) return 1;
    return s.index[1] >= s.index[0] ? 1 : 0;
}

// A resolved material instance (Material or MaterialInstanceConstant chain leaf).
struct SceneMaterial {
    std::string name;            // full path of the leaf material object
    std::string base_material;   // full path of the root UMaterial
    std::string error;           // non-empty if resolution/compilation fell back
    int shader = -1;             // index into SceneMaterialLibrary::shaders (-1 = fallback shading)
    MatBlendMode blend = MatBlendMode::Opaque;
    MatLightingModel lighting = MatLightingModel::Phong;
    bool two_sided = false;
    uint32_t texcoord_mask = 0;                // bit i: the graph reads TextureCoordinate index i
    MaterialUVSlots uv_slots;                  // which of those the vertex's two UV sets carry
    std::vector<int> tex2d;                    // scene texture index per 2D slot (-1 => default)
    std::vector<TexDefault> tex2d_default;     // default per 2D slot
    std::vector<int> texcube;                  // scene texture index per cube slot (-1 => default)
    std::vector<std::array<float, 4>> uniforms;  // parameter values per uniform slot
    std::vector<std::string> uniform_names;      // the scalar or vector parameter each slot holds

    // The slot of a named parameter, or -1: what script sets at run time (a fade amount, a colour).
    [[nodiscard]] int uniform_index(const std::string& parameter) const {
        for (size_t i = 0; i < uniform_names.size(); ++i) {
            if (uniform_names[i] == parameter) return static_cast<int>(i);
        }
        return -1;
    }
};

struct SceneMaterialLibrary {
    std::string common_source;               // shared MSL prelude (structs, vertex function, lighting helpers)
    std::string vertex_function = "mat_vertex";
    std::vector<MaterialShader> shaders;
    std::vector<SceneMaterial> materials;
    std::vector<SceneTexture> textures;
    // The level's baked light maps: three coefficient textures per set, set by set
    // (MeshSection::lightmap). An invalid entry is a texture that could not be read.
    std::vector<SceneTexture> lightmap_textures;

    // Diagnostics
    int materials_resolved = 0;
    int materials_failed = 0;
    int textures_loaded = 0;
    int textures_failed = 0;
    size_t texture_bytes = 0;
    double build_seconds = 0.0;
    std::vector<std::string> errors;
};

}  // namespace me
