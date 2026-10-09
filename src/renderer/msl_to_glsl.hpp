#pragma once

// -----------------------------------------------------------------------------
// Metal Shading Language -> GLSL (OpenGL 4.1 core, `#version 410 core`).
//
// The renderer's shaders exist once, as MSL: the built-in passes
// (renderer/builtin_shaders_msl.hpp), the sun shadow lookup (renderer/sun_shadow.hpp)
// and the UE3 material shaders that assets/material_system.cpp generates. The
// OpenGL renderer (renderer/opengl_renderer.hpp, the Linux backend) feeds that same
// text through this translator, so a shader changed for Metal changes on Linux too.
// It is the GLSL counterpart of renderer/msl_to_hlsl.hpp and covers the same subset
// of MSL.
//
// OpenGL 4.1 is the target because it is what every Linux driver (Mesa included)
// and macOS offer, so the backend can be checked on the macOS development machine.
// 4.1 has no `layout(binding = n)` for uniform blocks and samplers: the translator
// therefore reports the names it emitted and the MSL slots they came from
// (GlslShader::uniform_blocks, GlslShader::samplers) and the renderer binds them by
// name after linking.
//
// Where OpenGL and Metal disagree and how the emitted GLSL papers over it:
//
//   * Clip space. Metal: y up, z in [0, 1]. OpenGL: y up, z in [-1, 1], and its
//     framebuffer rows run bottom-up. Every vertex entry point ends with
//         gl_Position.y = -gl_Position.y;
//         gl_Position.z = 2.0 * gl_Position.z - gl_Position.w;
//     The y flip makes row 0 of an OpenGL render target hold what row 0 (the top)
//     of the Metal one holds, so render targets sampled by later passes, uploaded
//     textures (row 0 = top), gl_FragCoord.y, dFdy and screenshot readback all
//     match Metal without further flips. It also mirrors triangle winding: the
//     renderer must declare front faces the opposite way round (glFrontFace) from
//     what the Metal backend sets, and blit the finished frame to the window
//     with the y axis inverted (glBlitFramebuffer).
//   * Separate textures and samplers. GLSL 4.1 only has combined samplers. Each
//     (texture, sampler) pair a shader samples becomes one `sampler*` uniform
//     named `<texture>__<sampler>`; the renderer binds the texture to a unit, the
//     sampler object (glBindSampler) to the same unit and points the uniform at it.
//     A texture used only through get_width() / get_height() is paired with a
//     default sampler (sampler_slot = -1).
//   * Uniform buffers. `constant T& u [[buffer(n)]]` becomes
//         layout(std140) uniform <VS|FS>B<n> { T u; };
//     and `constant float4* U [[buffer(n)]]`
//         layout(std140) uniform <VS|FS>B<n> { vec4 U[<pointer_array_size>]; };
//     std140 lays out the renderer's uniform structs (float4x4, packed_float3 +
//     float pairs, float4 arrays) exactly as MSL does, so the CPU structs are
//     shared with the other backends unchanged. Vertex and fragment stages have
//     separate slot spaces in MSL; the block names carry the stage so the renderer
//     can give them separate binding points.
//   * Vertices. A vertex stage reading `vertices[vertex_id]` from buffer(0) takes
//     its vertex from attributes instead: member i of the struct is
//     `layout(location = i) in <type> in_<member>` (packed_float3 -> vec3, uint ->
//     uint). [[stage_in]] structs with [[attribute(n)]] use location n. The
//     renderer sets the vertex array up from GlslShader::vertex_member_types
//     (same contract as the HLSL translator).
//   * Varyings. The members of the vertex stage's return struct become
//     `out <type> v_<member>` ([[flat]] -> `flat out`), the [[position]] member
//     gl_Position; the fragment stage's [[stage_in]] struct reads the matching
//     `in` variables, its [[position]] member gl_FragCoord. A fragment stage that
//     returns float4 writes `layout(location = 0) out vec4 frag_out0`.
//   * Names and built-ins. float4 -> vec4, float4x4 -> mat4, half -> float,
//     int2 -> ivec2, [[vertex_id]] -> uint(gl_VertexID), [[front_facing]] ->
//     gl_FrontFacing, dfdx/dfdy -> dFdx/dFdy, rsqrt -> inversesqrt,
//     discard_fragment() -> discard, `constant` globals -> const, `const T&` ->
//     by value, `T&` -> inout, `template <typename T>` helpers -> one overload
//     each for float..vec4, `{...}` array initialisers -> `T[](...)`. saturate,
//     fmod (truncating, unlike GLSL mod) and select get helper functions in a
//     prelude. Identifiers that are GLSL keywords or built-in function names
//     (`in`, `out`, `sample`, `filter`, `input`, `texture`, ...) get `_` appended.
//     Matrix products need no rewriting: GLSL's `*` is Metal's.
//
// Anything else reaches the GLSL compiler unchanged and fails there, loudly.
// -----------------------------------------------------------------------------

#include <memory>
#include <string>
#include <vector>

namespace me {

// A `constexpr sampler` from the MSL. The renderer creates a sampler object for it and
// binds it to the units of the combined samplers whose `sampler_slot` names it
// (`static_sampler` set).
struct GlslStaticSampler {
    std::string name;
    int slot = 0;          // in the renderer's static sampler table
    bool linear = false;   // filter::linear (else nearest)
    bool repeat = false;   // address::repeat (else clamp_to_edge)
    bool compare = false;  // compare_func::less_equal
};

// One combined `sampler*` uniform in the emitted GLSL.
struct GlslSamplerUnit {
    std::string uniform;        // the GLSL uniform's name, e.g. "base_tex__smp"
    int texture_slot = 0;       // the MSL [[texture(n)]]
    int sampler_slot = -1;      // the MSL [[sampler(n)]], or a static sampler's slot; -1 = none (default sampler)
    bool static_sampler = false;
    bool shadow = false;        // declared as a *Shadow sampler (sampled with sample_compare)
};

// One uniform block in the emitted GLSL.
struct GlslUniformBlock {
    std::string block_name;  // "VSB1", "FSB0", ...
    int buffer_slot = 0;     // the MSL [[buffer(n)]]
};

struct GlslShader {
    std::string source;  // empty when `error` is set
    std::string entry;   // the MSL entry point's name (GLSL's main() wraps it)
    std::string error;
    bool vertex_stage = false;
    // Types of the vertex struct's members when the stage reads its vertices from
    // attributes (location i for member i); empty otherwise.
    std::vector<std::string> vertex_member_types;
    std::vector<GlslUniformBlock> uniform_blocks;
    std::vector<GlslSamplerUnit> samplers;
};

class MslToGlsl {
public:
    MslToGlsl();
    ~MslToGlsl();
    MslToGlsl(const MslToGlsl&) = delete;
    MslToGlsl& operator=(const MslToGlsl&) = delete;

    // Adds MSL source: structs, constants, helper functions and entry points.
    bool add_source(const std::string& msl, std::string* error = nullptr);

    // GLSL for one entry point added with add_source().
    [[nodiscard]] GlslShader emit(const std::string& entry, int pointer_array_size = 1) const;

    // GLSL for an entry point defined in `entry_msl`, a separate piece of source that relies
    // on everything added so far (one generated material shader on top of the shared prelude).
    // `pointer_array_size`: elements of a `constant float4* [[buffer(n)]]` argument.
    [[nodiscard]] GlslShader emit_from(const std::string& entry_msl, const std::string& entry,
                                       int pointer_array_size) const;

    [[nodiscard]] const std::vector<GlslStaticSampler>& static_samplers() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace me
