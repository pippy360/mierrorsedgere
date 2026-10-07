#pragma once

// -----------------------------------------------------------------------------
// Metal Shading Language -> HLSL (Direct3D 11, shader model 5).
//
// The renderer's shaders exist once, as MSL: the built-in passes
// (renderer/builtin_shaders_msl.hpp), the sun shadow lookup (renderer/sun_shadow.hpp)
// and the UE3 material shaders that assets/material_system.cpp generates. The
// Direct3D renderer feeds that same text through this translator, so a shader
// changed for Metal changes on Windows too.
//
// Metal and Direct3D agree on clip space (y up, z in [0, 1]), on texture and
// framebuffer origins and on column-vector matrix storage, so only the syntax
// differs. The translator works on tokens and covers the subset of MSL those
// sources use:
//   * structs, `constant` globals, helper functions (overloads and
//     `template <typename T>` over float..float4), vertex/fragment entry points;
//   * entry point arguments: [[stage_in]], [[buffer(n)]] (a struct reference
//     becomes cbuffer bN, a `float4*` an array in cbuffer bN), [[texture(n)]],
//     [[sampler(n)]], [[vertex_id]]; a vertex stage that reads
//     `vertices[vertex_id]` from buffer(0) takes that struct from the input
//     assembler instead, semantics VTX0..VTXn in member order;
//   * struct members: [[position]] -> SV_Position, [[attribute(n)]] -> ATTRn,
//     the remaining members of such a struct -> TEXCOORDn;
//   * texture2d / depth2d / depth2d_array / texturecube, sample(), sample(level()),
//     sample_compare(), get_width() / get_height(), `constexpr sampler`;
//   * matrix * vector (-> mul), one-argument vector constructors (-> casts),
//     mix / fract / dfdx / dfdy / discard_fragment, packed_float3, reference
//     parameters, and identifiers that are HLSL keywords (`in`, `out`, ...).
// Anything else reaches the HLSL compiler unchanged and fails there, loudly.
// -----------------------------------------------------------------------------

#include <memory>
#include <string>
#include <vector>

namespace me {

// A `constexpr sampler` from the MSL. The renderer creates it and binds it at `slot`
// for every shader of the translator it came from.
struct HlslStaticSampler {
    std::string name;
    int slot = 0;
    bool linear = false;   // filter::linear (else nearest)
    bool repeat = false;   // address::repeat (else clamp_to_edge)
    bool compare = false;  // compare_func::less_equal
};

struct HlslShader {
    std::string source;  // empty when `error` is set
    std::string entry;   // HLSL entry point (same name as the MSL function)
    std::string error;
    bool vertex_stage = false;
    // Types of the vertex struct's members when the stage reads its vertices from the
    // input assembler (semantic VTX<i> for member i); empty otherwise.
    std::vector<std::string> vertex_member_types;
};

class MslToHlsl {
public:
    MslToHlsl();
    ~MslToHlsl();
    MslToHlsl(const MslToHlsl&) = delete;
    MslToHlsl& operator=(const MslToHlsl&) = delete;

    // Adds MSL source: structs, constants, helper functions and entry points.
    bool add_source(const std::string& msl, std::string* error = nullptr);

    // HLSL for one entry point added with add_source().
    [[nodiscard]] HlslShader emit(const std::string& entry, int pointer_array_size = 1) const;

    // HLSL for an entry point defined in `entry_msl`, a separate piece of source that relies
    // on everything added so far (one generated material shader on top of the shared prelude).
    // `pointer_array_size`: elements of a `constant float4* [[buffer(n)]]` argument.
    [[nodiscard]] HlslShader emit_from(const std::string& entry_msl, const std::string& entry,
                                       int pointer_array_size) const;

    [[nodiscard]] const std::vector<HlslStaticSampler>& static_samplers() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace me
