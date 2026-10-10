#include "opengl_renderer.hpp"
#include "builtin_shaders_msl.hpp"
#include "gl/gl_api.hpp"
#include "hud_font.hpp"
#include "msl_to_glsl.hpp"
#include "lens_flare.hpp"
#include "light_environment.hpp"
#include "mod_shadow.hpp"
#include "particles.hpp"
#include "post_process.hpp"
#include "render_common.hpp"
#include "sun_shadow.hpp"
#include "../anim/anim_system.hpp"
#include "../assets/scene_materials.hpp"
#include "../cutscene/cutscene_player.hpp"
#include "../platform/platform.hpp"
#include "../ui/frontend/soft_render.hpp"
#include "../ui/frontend/soft_sample.hpp"
#include "../ui/main_menu.hpp"

#include <SDL2/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace me {

using namespace me::gl;

namespace {

// A texture the shaders can sample.
struct GpuTexture {
    GLuint name = 0;
    GLenum target = GL_TEXTURE_2D;
    int width = 0;
    int height = 0;
    GpuTexture() = default;
    GpuTexture(const GpuTexture&) = delete;
    GpuTexture& operator=(const GpuTexture&) = delete;
    ~GpuTexture() {
        // The context owns the storage; once it is gone there is nothing to delete.
        if (name != 0 && gl::loaded()) glDeleteTextures(1, &name);
    }
};

// The names the shared overlay code (renderer/overlay_ui.inl) is written against.
struct UIFloat2 {
    float x = 0.0f;
    float y = 0.0f;
};
struct UIColor {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;
};
inline UIColor ui_color(float r, float g, float b, float a) { return UIColor{r, g, b, a}; }
using UITexture = std::shared_ptr<GpuTexture>;
inline float ui_texture_width(const UITexture& t) { return static_cast<float>(t->width); }
inline float ui_texture_height(const UITexture& t) { return static_cast<float>(t->height); }

struct HUDVertex {
    UIFloat2 position;
    UIColor color;
};

struct UITexVertex {
    UIFloat2 position;
    UIFloat2 uv;
    UIColor color;
};

// -----------------------------------------------------------------------------
// Frame uniforms: must match `FrameUniforms` in builtin_shaders_msl.hpp and
// material_system.cpp. std140 lays a vec3 followed by a float out as MSL's
// packed_float3 + float, so this is byte for byte the Direct3D backend's struct.
// -----------------------------------------------------------------------------
struct Float3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    constexpr Float3() = default;
    constexpr Float3(float in_x, float in_y, float in_z) : x(in_x), y(in_y), z(in_z) {}
    Float3(const Vec3& v) : x(v.x), y(v.y), z(v.z) {}
};

struct FrameUniformsGPU {
    float view_proj[16];
    float model[16];
    Float3 camera_pos;
    float sim_time;
    Float3 sun_dir;
    float runner_vision_strength;
    Float3 sun_color;
    float exposure;
    Float3 sky_color;
    float speed_2d;
    Float3 ground_color;
    float reaction_active;
    Float3 actor_tint;
    float is_runner_vision;
    Float3 cam_forward;
    float fov_tan;
    Float3 cam_right;
    float aspect;
    Float3 cam_up;
    float health;
    float sun_view_proj[16];
    Float3 mod_shadow_color;
    float shadow_enabled;
    float sun_view_proj_far[16];
};
static_assert(sizeof(FrameUniformsGPU) == 416, "FrameUniformsGPU must match the shaders' FrameUniforms");

void set_matrix(float dst[16], const Mat4& m) { std::memcpy(dst, m.m, sizeof(float) * 16); }

// Runs fn(i) for i in [0, count) on the process's worker threads (the front end's row pool). They
// persist between calls, so thread_local scratch buffers in fn are allocated once, not every frame.
// Must be called from one thread at a time, and fn must not call it again. fn must not call OpenGL:
// the context belongs to the thread that made it.
template <class Fn>
void parallel_for(size_t count, Fn&& fn) {
    fe::parallel_rows(static_cast<int>(count), [&fn](int first, int last) {
        for (int i = first; i < last; ++i) fn(static_cast<size_t>(i));
    });
}

// -----------------------------------------------------------------------------
// Material System GPU Helpers (scene_materials.hpp -> OpenGL)
// -----------------------------------------------------------------------------
// How a decoded UE3 texture format is uploaded. L8 and V8U8 keep their one or two channels and
// are widened by a texture swizzle, as on Metal: L8 reads as (L, L, L, 1), V8U8 as (U, V, 1, 1).
// An sRGB L8 goes into an SRGB8 texture through the red channel (core OpenGL has no sRGB R8).
struct GlTexFormat {
    GLenum internal = 0;
    GLenum format = GL_RGBA;
    GLenum type = GL_UNSIGNED_BYTE;
    bool compressed = false;
    bool swizzled = false;
    GLint swizzle[4] = {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA};
};

GlTexFormat gl_tex_format(TexFormat f, bool srgb) {
    GlTexFormat r;
    switch (f) {
        case TexFormat::DXT1:
            r.internal = srgb ? GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT : GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
            r.compressed = true;
            break;
        case TexFormat::DXT3:
            r.internal = srgb ? GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT : GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
            r.compressed = true;
            break;
        case TexFormat::DXT5:
            r.internal = srgb ? GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
            r.compressed = true;
            break;
        case TexFormat::BGRA8:
            r.internal = srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;
            r.format = GL_BGRA;
            break;
        case TexFormat::G8:
            r.internal = srgb ? GL_SRGB8 : GL_R8;
            r.format = GL_RED;
            r.swizzled = true;
            r.swizzle[0] = r.swizzle[1] = r.swizzle[2] = GL_RED;
            r.swizzle[3] = GL_ONE;
            break;
        case TexFormat::V8U8:
            r.internal = GL_RG8_SNORM;
            r.format = GL_RG;
            r.type = GL_BYTE;
            r.swizzled = true;
            r.swizzle[0] = GL_RED;
            r.swizzle[1] = GL_GREEN;
            r.swizzle[2] = GL_ONE;
            r.swizzle[3] = GL_ONE;
            break;
        default: break;
    }
    return r;
}

GLenum gl_address_mode(TexAddress a) {
    switch (a) {
        case TexAddress::Clamp: return GL_CLAMP_TO_EDGE;
        case TexAddress::Mirror: return GL_MIRRORED_REPEAT;
        default: return GL_REPEAT;
    }
}

// Uniform block binding points: vertex-stage MSL buffer(n) -> n, fragment-stage buffer(n) -> 16 + n.
// The two stages have separate slot spaces in MSL (FrameUniforms is vertex buffer(1) and fragment
// buffer(0)); the block names the translator emits (VSBn / FSBn) say which.
constexpr GLuint kFragmentBlockBase = 16;
GLuint block_binding(bool vertex_stage, int slot) {
    return static_cast<GLuint>(slot) + (vertex_stage ? 0u : kFragmentBlockBase);
}

// Texture image units: a combined sampler for MSL texture(n) lives on unit n; a texture sampled
// through two different samplers in one shader gets a second unit from kExtraUnitBase up.
constexpr int kTextureSlots = 32;    // MSL texture(0..29) are in use
constexpr int kSamplerSlots = 16;    // MSL sampler(0..15)
constexpr int kExtraUnitBase = 32;
constexpr int kMaxUnits = 48;        // GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS is at least 48 since 3.3

void APIENTRY gl_debug_callback(GLenum /*source*/, GLenum type, GLuint id, GLenum severity, GLsizei length,
                                const GLchar* message, const void* user) {
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) return;
    auto* printed = static_cast<std::atomic<int>*>(const_cast<void*>(user));
    if (printed->fetch_add(1) >= 40) return;
    const char* kind = (type == GL_DEBUG_TYPE_ERROR) ? "error"
                       : (type == GL_DEBUG_TYPE_PERFORMANCE) ? "performance"
                                                             : "message";
    std::cerr << "[OpenGL debug] " << kind << " " << id << ": " << std::string(message, length > 0 ? static_cast<size_t>(length) : std::strlen(message))
              << std::endl;
}

}  // namespace

// -----------------------------------------------------------------------------
// PIMPL Implementation
// -----------------------------------------------------------------------------
struct OpenGLRenderer::Impl {
    SDL_Window* window = nullptr;      // the window the context draws to (own_window when headless)
    SDL_Window* own_window = nullptr;  // the hidden window init_headless() made
    SDL_GLContext context = nullptr;
    bool debug = false;                // ME_GL_DEBUG=1
    std::atomic<int> debug_messages_printed{0};
    bool has_s3tc = false;
    bool has_anisotropy = false;
    GLint max_units = kMaxUnits;
    std::string adapter_name;
    std::string game_root;

    // The shaders, translated from MSL (renderer/msl_to_glsl.hpp)
    MslToGlsl builtin_shaders;                    // sun_shadow_msl() + kBuiltinShadersMSL
    std::unique_ptr<MslToGlsl> material_shaders;  // the resident material library's prelude

    // One (texture, sampler) pair of a program: the GLSL combined sampler on `unit` reads the texture
    // the renderer put in MSL texture slot `texture_slot` through the sampler in MSL sampler slot
    // `sampler_slot` (or `fixed_sampler`, the object made for one of the MSL's constexpr samplers).
    struct UnitBinding {
        GLuint unit = 0;
        int texture_slot = 0;
        int sampler_slot = -1;
        GLuint fixed_sampler = 0;
    };
    enum class VertexKind { None, Scene, Hud, UiTex };
    struct Program {
        GLuint prog = 0;
        VertexKind kind = VertexKind::None;
        std::vector<UnitBinding> units;
    };
    Program shadow_program;
    Program sky_program;
    Program world_program;
    Program viewmodel_program;
    // The post-process chain (builtin_shaders_msl.hpp, section 4)
    Program fog_program;
    Program mod_shadow_program;
    Program haze_program;
    Program bloom_gather_program;
    Program filter_program;
    Program meter_scene_program;
    Program meter_program;
    Program exposure_program;
    Program tonemap_program;
    Program finish_program;
    struct ColorTarget {
        UITexture tex;
        GLuint fbo = 0;
        int w = 0;
        int h = 0;
    };
    ColorTarget scene_hazed;    // the scene with the haze added: what bloom and the tone mapper read
    ColorTarget filter_a;       // quarter size: the bloom gather, then the blurred result
    ColorTarget filter_b;       // quarter size: the blur's first axis
    ColorTarget meter[kMeterSteps];  // the exposure's metering: 512 .. 1 across, 16-bit fixed point
    ColorTarget exposure[2];    // 1 x 1: exposure squared over 64, this frame's and the last's
    ColorTarget picture[2];     // the tone-mapped picture (RGBA8), for the passes that work on it in turn
    ColorTarget scene_effect;   // the scene after a material effect that stands before the tone mapper
    PostSettingsBlend post_settings;  // the post-process settings in force at the view
    MotionBlurState motion_blur;      // TdMotionBlur: the camera's velocity, smoothed
    int exposure_current = 0;
    float exposure_sim_time = -1.0f;  // telemetry.sim_time the exposure was last moved at
    std::string exposure_map;         // the level it adapted in
    Vec3 exposure_view_pos{0.0f, 0.0f, 0.0f};  // where the view was when it last moved
    GLuint cb_post = 0;               // PostUniforms: fragment b1 during the chain
    Program hud_program;
    Program ui_tex_program;

    // Depth states (glDepthFunc LESS_EQUAL throughout, as the other backends)
    enum class DepthState { Write, TestOnly, Disabled };

    // Blend states
    struct BlendState {
        bool enable = false;
        GLenum src = GL_ONE;
        GLenum dst = GL_ZERO;
        GLenum src_a = GL_ONE;
        GLenum dst_a = GL_ZERO;
    };
    BlendState blend_opaque;
    BlendState blend_translucent;
    BlendState blend_additive;
    BlendState blend_modulate;
    BlendState blend_fog;  // One, SrcAlpha: scene * scattering + fog
    BlendState blend_ui;

    // Samplers (sampler objects)
    GLuint linear_sampler = 0;        // clamp, top mip only: post-processing and UI
    GLuint mat_samplers[3][3] = {};   // [TexAddress X][TexAddress Y]
    GLuint mat_cube_sampler = 0;
    GLuint scene_copy_sampler = 0;
    std::unordered_map<int, GLuint> static_samplers;  // the MSL's constexpr samplers, by configuration

    // Uniform buffers
    GLuint cb_frame = 0;     // FrameUniforms: vertex b1, fragment b0
    GLuint cb_screen = 0;    // float2 screen size: HUD / UI vertex b1
    GLuint cb_material = 0;  // float4 material uniforms: fragment b1 (the menu's per-frame override)
    GLuint cb_scene = 0;     // SceneUniforms: fragment b2 of the material shaders
    SceneUniformsGPU scene_constants{};  // what it holds
    SceneLightEnvironments light_envs;   // of the dynamic objects, kept between frames
    std::vector<LensFlareQuad> flare_quads;  // this frame's
    std::vector<ModShadow> mod_shadows;      // this frame's dynamic shadows (mod_shadow.hpp)
    ParticleWorld particles;                 // the level's particle systems, kept between frames
    std::vector<uint8_t> mod_enemy_ready;
    bool viewmodel_built = false;            // the first-person body is already posed for this frame
    std::vector<Vertex> head_vertices;       // the head and torso her shadow is given (mod_shadow.hpp)
    std::vector<Vertex> flare_vertices;
    static constexpr int kMaxMaterialUniforms = 256;
    // Every material instance's float4 uniforms, uploaded once when its library becomes resident,
    // so a section binds a range of this buffer instead of uploading its values (mat_uniform_offsets).
    GLuint mat_uniform_buffer = 0;
    std::vector<GLintptr> mat_uniform_offsets;  // per SceneMaterial; -1 = none
    GLint uniform_offset_alignment = 256;

    // Framebuffer targets
    UITexture offscreen_color_tex;  // the finished frame (RGBA8); the window shows a blit of it
    GLuint offscreen_fbo = 0;
    UITexture scene_hdr_tex;        // Intermediate HDR buffer for tone mapping
    UITexture depth_tex;
    GLuint scene_fbo = 0;           // scene_hdr_tex + depth_tex
    GLuint scene_color_fbo = 0;     // scene_hdr_tex alone: for passes that read the depth buffer
    UITexture shadow_depth_tex;     // sun shadow cascades: 2-slice 4096x4096 depth array (renderer/sun_shadow.hpp)
    GLuint shadow_fbo[3] = {0, 0, 0};
    UITexture scene_color_copy;     // opaque scene color (SceneTexture / DestColor)
    UITexture scene_depth_copy;     // opaque scene depth (SceneDepth / DepthBiased*)
    GLuint scene_copy_fbo = 0;      // both copies, the blit's destination
    // The far cascade slice only holds static level geometry and is redrawn only when its (coarsely
    // snapped) matrix changes or the scene's vertex buffers are rebuilt.
    bool shadow_far_valid = false;
    float shadow_far_vp[16] = {};
    uint64_t scene_generation = 0;       // bumped whenever cached_mesh_buffers is rebuilt
    uint64_t shadow_far_generation = 0;  // scene_generation the far slice was rendered from

    int width = 1280;
    int height = 720;
    bool headless = true;
    bool initialized = false;
    uint64_t frame_index = 0;

    // Menu state & Frontend UI System (TdMainMenu.me1 + UI/TdUI_FrontEnd.upk)
    bool menu_open = false;
    int selected_chapter = 1;
    int selected_menu_tab = 0;
    int selected_menu_row = 0;
    int opt_sens_pct = 100;
    int opt_fov_deg = 90;
    bool opt_fullscreen = false;
    MainMenuSystem main_menu;
    bool main_menu_gpu_ready = false;
    UITexture ui_logo_tex;
    UITexture ui_bag_tex;
    UITexture ui_time_tex;
    UITexture ui_panel_bg_tex;
    UITexture ui_faith_art_tex;
    UITexture ui_chapter_tex[10];
    std::vector<UITexture> ui_font_headline_thick_tex;
    std::vector<UITexture> ui_font_headline_light_tex;
    std::vector<UITexture> ui_font_medium_italic_tex;
    std::vector<UITexture> ui_font_small_italic_tex;

    struct UITextureBatch {
        UITexture tex;
        std::vector<UITexVertex> verts;
    };

    // First-Person Faith Viewmodel Mesh & 3D Enemy Guard Mesh
    std::vector<Vertex> faith_viewmodel_mesh;
    // This frame's posed enemies (index = enemy index). An enemy is drawn indexed: its posed unique
    // vertices go to enemy_vertex_buffers[i], and enemy_index_buffers holds the animation system's static
    // triangle lists (AnimSystem::enemy_swat_index_lists()). That is the triangle list
    // evaluate_enemy_swat() returns, without expanding it on the CPU every frame.
    struct EnemyFrameDraw {
        size_t index_count = 0;  // leading indices of the list to draw (0 = nothing to draw)
        size_t index_list = 0;   // enemy_index_buffers entry
        uint32_t archetype_id = 0;
        bool in_view = false;    // may cover pixels of the camera view (world pass)
        bool in_shadow = false;  // may cover texels of the near shadow cascade (shadow pass)
    };
    std::vector<EnemyFrameDraw> frame_enemy_draws;
    std::vector<Vertex*> enemy_mapped;  // this frame's write pointer into each enemy's vertex buffer (null = not posed)
    std::vector<GLuint> enemy_index_buffers;
    static constexpr float kEnemyReach = 1024.0f;  // UU: a posed enemy's vertices are within this of its origin
    AnimSystem anim_system;

    // -------------------------------------------------------------------------
    // Vertex buffers. OpenGL 4.1 reads vertices through a vertex array object whose attribute
    // pointers name a buffer, so each buffer carries its own VAO, set up for the layout it was
    // last drawn with (the Direct3D input layouts, as the translator describes them).
    // -------------------------------------------------------------------------
    struct VertexAttrib {
        GLuint index = 0;
        GLint size = 1;
        GLenum type = GL_FLOAT;
        bool integer = false;
        GLsizei offset = 0;
    };
    struct VertexLayout {
        std::vector<VertexAttrib> attribs;
        GLsizei stride = 0;
    };
    VertexLayout layouts[4];  // by VertexKind
    GLuint empty_vao = 0;     // for the passes that draw from vertex_id alone

    struct GpuBuffer {
        GLuint vbo = 0;
        GLuint vao = 0;
        VertexKind vao_kind = VertexKind::None;
        size_t capacity = 0;
    };
    void destroy_buffer(GpuBuffer& b) {
        if (!gl::loaded()) return;
        if (b.vao) glDeleteVertexArrays(1, &b.vao);
        if (b.vbo) glDeleteBuffers(1, &b.vbo);
        if (cur_vao == b.vao) cur_vao = 0;
        b = GpuBuffer{};
    }

    // Reusable dynamic vertex buffers for per-frame geometry (viewmodel, combat effects, HUD) and the enemies.
    std::vector<GpuBuffer> dyn_vertex_buffers;
    size_t dyn_vertex_cursor = 0;
    std::vector<GpuBuffer> enemy_vertex_buffers;

    static size_t dynamic_buffer_capacity(size_t length) {
        return std::max<size_t>((length + 4095u) & ~size_t(4095u), 65536u);
    }

    // Maps `b` for writing `length` bytes from the start (its old contents are discarded), growing it
    // first if needed. Returns null on failure; otherwise the caller unmaps with unmap_dynamic_buffer().
    // The storage is orphaned first, so the map never waits for a draw that still reads the old contents.
    void* map_dynamic_buffer(GpuBuffer& b, size_t length) {
        if (!b.vbo) glGenBuffers(1, &b.vbo);
        glBindBuffer(GL_ARRAY_BUFFER, b.vbo);
        if (b.capacity < length) b.capacity = dynamic_buffer_capacity(length);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(b.capacity), nullptr, GL_STREAM_DRAW);
        return glMapBufferRange(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(length),
                                GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
    }

    void unmap_dynamic_buffer(GpuBuffer& b) {
        glBindBuffer(GL_ARRAY_BUFFER, b.vbo);
        glUnmapBuffer(GL_ARRAY_BUFFER);
    }

    void fill_dynamic_buffer(GpuBuffer& b, const void* data, size_t length) {
        if (void* dst = map_dynamic_buffer(b, length)) {
            std::memcpy(dst, data, length);
            unmap_dynamic_buffer(b);
        }
    }

    GpuBuffer* acquire_dynamic_vertex_buffer(const void* data, size_t length) {
        const size_t idx = dyn_vertex_cursor++;
        if (idx >= dyn_vertex_buffers.size()) dyn_vertex_buffers.emplace_back();
        fill_dynamic_buffer(dyn_vertex_buffers[idx], data, length);
        return &dyn_vertex_buffers[idx];
    }

    // A buffer with fixed contents (scene meshes, the enemies' index lists). Index data is uploaded
    // through GL_ARRAY_BUFFER too: binding GL_ELEMENT_ARRAY_BUFFER would alter the current VAO.
    GLuint make_static_buffer(const void* data, size_t length) {
        GLuint b = 0;
        glGenBuffers(1, &b);
        glBindBuffer(GL_ARRAY_BUFFER, b);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(length), data, GL_STATIC_DRAW);
        return b;
    }

    // Makes `b` (or, for null, nothing) the vertex source, laid out as `kind`.
    void bind_vertices(GpuBuffer* b, VertexKind kind) {
        if (!b || kind == VertexKind::None) {
            if (cur_vao != empty_vao) glBindVertexArray(empty_vao);
            cur_vao = empty_vao;
            return;
        }
        if (!b->vao) {
            glGenVertexArrays(1, &b->vao);
            b->vao_kind = VertexKind::None;
        }
        if (cur_vao != b->vao) glBindVertexArray(b->vao);
        cur_vao = b->vao;
        if (b->vao_kind == kind) return;
        const VertexLayout& layout = layouts[static_cast<int>(kind)];
        const VertexLayout& old = layouts[static_cast<int>(b->vao_kind)];
        for (const VertexAttrib& a : old.attribs) glDisableVertexAttribArray(a.index);
        glBindBuffer(GL_ARRAY_BUFFER, b->vbo);
        for (const VertexAttrib& a : layout.attribs) {
            glEnableVertexAttribArray(a.index);
            const void* offset = reinterpret_cast<const void*>(static_cast<uintptr_t>(a.offset));
            if (a.integer) {
                glVertexAttribIPointer(a.index, a.size, a.type, layout.stride, offset);
            } else {
                glVertexAttribPointer(a.index, a.size, a.type, GL_FALSE, layout.stride, offset);
            }
        }
        b->vao_kind = kind;
    }

    // -------------------------------------------------------------------------
    // Texture and sampler slots. The passes bind textures and samplers to the MSL slots the shaders
    // name, as the other backends do; which GL units the current program reads them through is
    // settled per program at link time (Program::units), and flush_bindings() applies it at each draw.
    // -------------------------------------------------------------------------
    GLuint tex_slot[kTextureSlots] = {};
    GLenum tex_slot_target[kTextureSlots] = {};
    GLuint smp_slot[kSamplerSlots] = {};
    struct UnitState {
        GLuint tex = 0;
        GLenum target = 0;
        GLuint sampler = 0;
    };
    UnitState unit_state[kMaxUnits];
    const Program* current_program = nullptr;

    void bind_texture(int slot, const GpuTexture* t) {
        if (slot < 0 || slot >= kTextureSlots) return;
        tex_slot[slot] = t ? t->name : 0;
        tex_slot_target[slot] = t ? t->target : GL_TEXTURE_2D;
    }
    void bind_textures(int first, std::initializer_list<const GpuTexture*> textures) {
        int slot = first;
        for (const GpuTexture* t : textures) bind_texture(slot++, t);
    }
    void bind_sampler(int slot, GLuint sampler) {
        if (slot >= 0 && slot < kSamplerSlots) smp_slot[slot] = sampler;
    }

    void flush_bindings() {
        if (!current_program) return;
        for (const UnitBinding& u : current_program->units) {
            UnitState& s = unit_state[u.unit];
            const GLuint tex = tex_slot[u.texture_slot];
            const GLenum target = tex_slot_target[u.texture_slot];
            if (tex != s.tex || (tex != 0 && target != s.target)) {
                glActiveTexture(GL_TEXTURE0 + u.unit);
                if (s.tex != 0 && s.target != target) glBindTexture(s.target, 0);
                if (tex != 0) {
                    glBindTexture(target, tex);
                } else if (s.tex != 0) {
                    glBindTexture(s.target, 0);
                }
                s.tex = tex;
                s.target = tex != 0 ? target : s.target;
            }
            GLuint sampler = u.fixed_sampler;
            if (sampler == 0 && u.sampler_slot >= 0 && u.sampler_slot < kSamplerSlots) sampler = smp_slot[u.sampler_slot];
            if (sampler == 0) sampler = linear_sampler;  // a texture only measured, or an unset slot
            if (sampler != s.sampler) {
                glBindSampler(u.unit, sampler);
                s.sampler = sampler;
            }
        }
    }

    // A texture cannot be a render target and a shader input at once, so inputs are released between passes.
    void unbind_shader_resources() {
        for (int i = 0; i < kTextureSlots; ++i) tex_slot[i] = 0;
    }

    void use_program(const Program& p) {
        if (current_program != &p) {
            glUseProgram(p.prog);
            current_program = &p;
        }
    }

    // ME_RENDER_PROF=1 prints, every 120 frames, where a frame's time goes on this thread, by phase.
    // "present" is the wait for the GPU and the display.
    bool profile = std::getenv("ME_RENDER_PROF") != nullptr;
    enum ProfPhase { kProfPrepare, kProfEnemies, kProfShadows, kProfScene, kProfTranslucent, kProfOverlays, kProfPresent, kProfPhases };
    static constexpr const char* kProfNames[kProfPhases] = {"prepare", "enemies", "shadows", "scene",
                                                           "translucent", "viewmodel+post+hud", "present"};
    uint64_t prof_draws = 0;
    std::atomic<uint64_t> prof_posed{0};  // enemies skinned
    double prof_seconds[kProfPhases] = {};
    int prof_frames = 0;
    // The GPU's time per frame (one GL_TIME_ELAPSED query; several per frame would split the
    // driver's command buffers and inflate the result), read a frame late so the CPU never waits for
    // it. The wall-clock time between frames gives the frame rate itself.
    GLuint prof_queries[2] = {0, 0};
    bool prof_query_pending[2] = {false, false};
    double prof_gpu_seconds = 0.0;
    int prof_gpu_frames = 0;
    std::chrono::steady_clock::time_point prof_last_frame{};
    double prof_wall_seconds = 0.0;

    void prof_gpu_begin() {
        if (!profile) return;
        const auto now = std::chrono::steady_clock::now();
        if (prof_last_frame.time_since_epoch().count() != 0) prof_wall_seconds += std::chrono::duration<double>(now - prof_last_frame).count();
        prof_last_frame = now;
        if (!prof_queries[0]) glGenQueries(2, prof_queries);
        const int slot = static_cast<int>(frame_index & 1u);
        if (prof_query_pending[slot]) {
            GLuint64 ns = 0;
            glGetQueryObjectui64v(prof_queries[slot], GL_QUERY_RESULT, &ns);
            prof_gpu_seconds += static_cast<double>(ns) * 1e-9;
            ++prof_gpu_frames;
            prof_query_pending[slot] = false;
        }
        glBeginQuery(GL_TIME_ELAPSED, prof_queries[slot]);
    }
    void prof_gpu_end() {
        if (!profile || !prof_queries[0]) return;
        glEndQuery(GL_TIME_ELAPSED);
        prof_query_pending[frame_index & 1u] = true;
    }

    void draw(GLsizei vertex_count, GLint first_vertex) {
        flush_bindings();
        glDrawArrays(GL_TRIANGLES, first_vertex, vertex_count);
        ++prof_draws;
    }

    // Uploads `length` bytes to a uniform buffer; the old storage is orphaned, so a draw still reading
    // it is not waited for.
    void update_constants(GLuint buffer, const void* data, size_t length) {
        if (!buffer) return;
        glBindBuffer(GL_UNIFORM_BUFFER, buffer);
        const GLsizeiptr capacity = static_cast<GLsizeiptr>((length + 15u) & ~size_t(15u));
        glBufferData(GL_UNIFORM_BUFFER, capacity, nullptr, GL_STREAM_DRAW);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, static_cast<GLsizeiptr>(length), data);
    }

    // Fixed-function state helpers. Each remembers what it last set and skips a repeat: the passes
    // set the full state per section, as the other backends do, and a GL call costs several
    // microseconds on some drivers even when it changes nothing.
    int cur_depth = -1;
    BlendState cur_blend;
    bool cur_blend_valid = false;
    int cur_cull = -1;
    int cur_viewport[2] = {-1, -1};
    float cur_depth_range[2] = {-1.0f, -1.0f};
    GLuint cur_vao = 0;
    struct UniformBinding {
        GLuint buffer = 0;
        GLintptr offset = 0;
        GLsizeiptr size = 0;  // 0 = the whole buffer (glBindBufferBase)
    };
    UniformBinding cur_uniform_binding[kFragmentBlockBase * 2];

    void set_depth(DepthState s) {
        if (cur_depth == static_cast<int>(s)) return;
        cur_depth = static_cast<int>(s);
        switch (s) {
            case DepthState::Write:
                glEnable(GL_DEPTH_TEST);
                glDepthFunc(GL_LEQUAL);
                glDepthMask(GL_TRUE);
                break;
            case DepthState::TestOnly:
                glEnable(GL_DEPTH_TEST);
                glDepthFunc(GL_LEQUAL);
                glDepthMask(GL_FALSE);
                break;
            case DepthState::Disabled:
                glDisable(GL_DEPTH_TEST);
                glDepthMask(GL_FALSE);
                break;
        }
    }
    void set_blend(const BlendState& b) {
        if (cur_blend_valid && b.enable == cur_blend.enable &&
            (!b.enable || (b.src == cur_blend.src && b.dst == cur_blend.dst && b.src_a == cur_blend.src_a && b.dst_a == cur_blend.dst_a))) {
            return;
        }
        cur_blend = b;
        cur_blend_valid = true;
        if (b.enable) {
            glEnable(GL_BLEND);
            glBlendEquation(GL_FUNC_ADD);
            glBlendFuncSeparate(b.src, b.dst, b.src_a, b.dst_a);
        } else {
            glDisable(GL_BLEND);
        }
    }
    void set_cull(bool cull_back) {
        if (cur_cull == (cull_back ? 1 : 0)) return;
        cur_cull = cull_back ? 1 : 0;
        if (cull_back) {
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
        } else {
            glDisable(GL_CULL_FACE);
        }
    }
    // Shadow passes: slope-scaled bias, capped in UU (the cap is in map depth units). The cap needs
    // GL_ARB/EXT_polygon_offset_clamp; without it (Apple) the bias is uncapped on steep slopes.
    void set_shadow_bias(float clamp) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        if (glPolygonOffsetClamp) {
            glPolygonOffsetClamp(1.75f, 0.0f, clamp);
        } else {
            glPolygonOffset(1.75f, 0.0f);
        }
    }
    void clear_shadow_bias() { glDisable(GL_POLYGON_OFFSET_FILL); }
    // The level's decals lie in their receivers' surfaces (render_common.hpp).
    void set_decal_bias(bool on) {
        if (on == cur_decal_bias) return;
        cur_decal_bias = on;
        if (on) {
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(kDecalSlopeBias, static_cast<float>(kDecalDepthBias));
        } else {
            glDisable(GL_POLYGON_OFFSET_FILL);
        }
    }
    bool cur_decal_bias = false;
    // The viewport with its depth range (the viewmodel draws into [0, 0.05], the world into [0.05, 1]).
    void set_viewport(int w, int h, float min_depth, float max_depth) {
        if (w != cur_viewport[0] || h != cur_viewport[1]) {
            glViewport(0, 0, w, h);
            cur_viewport[0] = w;
            cur_viewport[1] = h;
        }
        if (min_depth != cur_depth_range[0] || max_depth != cur_depth_range[1]) {
            glDepthRange(min_depth, max_depth);
            cur_depth_range[0] = min_depth;
            cur_depth_range[1] = max_depth;
        }
    }
    void set_render_target(GLuint fbo) { glBindFramebuffer(GL_FRAMEBUFFER, fbo); }
    void clear_color(const float rgba[4]) { glClearBufferfv(GL_COLOR, 0, rgba); }
    void clear_depth() {
        glDepthMask(GL_TRUE);  // glClear honours the depth write mask
        cur_depth = -1;        // so the next set_depth() restores the mask it wants
        const GLfloat one = 1.0f;
        glClearBufferfv(GL_DEPTH, 0, &one);
    }
    void bind_uniform_block(GLuint binding, GLuint buffer) { bind_uniform_range(binding, buffer, 0, 0); }
    // `size` 0 binds the whole buffer. Repeats of the current binding are skipped.
    void bind_uniform_range(GLuint binding, GLuint buffer, GLintptr offset, GLsizeiptr size) {
        UniformBinding& cur = cur_uniform_binding[binding];
        if (cur.buffer == buffer && cur.offset == offset && cur.size == size) return;
        cur.buffer = buffer;
        cur.offset = offset;
        cur.size = size;
        if (size == 0) {
            glBindBufferBase(GL_UNIFORM_BUFFER, binding, buffer);
        } else {
            glBindBufferRange(GL_UNIFORM_BUFFER, binding, buffer, offset, size);
        }
    }

    // With ME_GL_DEBUG=1 (and no GL_KHR_debug to report them as they happen), the errors a pass left.
    void check_errors(const char* where) {
        if (!debug) return;
        for (GLenum e = glGetError(); e != GL_NO_ERROR; e = glGetError()) {
            if (debug_messages_printed.fetch_add(1) < 40) {
                std::cerr << "[OpenGLRenderer] glGetError 0x" << std::hex << e << std::dec << " after " << where << std::endl;
            }
        }
    }

    // Cutscene Bink Video & Matinee Renderer State
    const CutscenePlayer* cutscene_player = nullptr;
    UITexture bink_video_tex;
    uint64_t bink_uploaded_serial = 0;

    // The CPU-rendered front end frame (set_frontend_frame), uploaded every frame it is set.
    const uint8_t* frontend_rgba = nullptr;
    int frontend_w = 0;
    int frontend_h = 0;
    bool frontend_overlay = false;
    float frontend_saturation = 0.0f;
    UITexture frontend_tex;

    // Cached GPU Vertex Buffers & Per-Section Material/Shadow Metadata for Scene Meshes
    std::string cached_map_name;
    size_t cached_total_verts = 0;
    std::vector<GpuBuffer> cached_mesh_buffers;
    std::shared_ptr<const SceneMaterialLibrary> cached_section_mat_lib;
    bool cached_has_translucent = false;
    bool cached_needs_scene_copies = false;
    static constexpr uint8_t kSecShadowCaster = 1u << 0;
    static constexpr uint8_t kSecTranslucent = 1u << 1;
    std::vector<std::vector<uint8_t>> cached_section_flags;
    // The shadow pass draws no material state, so a mesh's casting sections that follow each other in
    // its vertex buffer are drawn as one range (the depth result is the same whatever the order).
    struct VertexRange {
        GLint first = 0;
        GLsizei count = 0;
    };
    std::vector<std::vector<VertexRange>> cached_shadow_ranges;
    // The opaque pass's order of each mesh's sections: by material shader, then material, then light
    // map (ME_GL_SORT=0: the buffer's order, as the other backends draw). A level is a handful of
    // vertex buffers holding hundreds of sections, nearly all with a different material, and the GL
    // driver pays for every program change; sorted, a program is switched to once per mesh. Opaque
    // geometry depth-tests the same whatever the order, except at exact depth ties. UE3 sorts its base
    // pass the same way.
    std::vector<std::vector<uint32_t>> cached_opaque_order;
    bool sort_opaque = true;

    // -------------------------------------------------------------------------
    // Mirror's Edge material system (LevelScene::materials) GPU cache
    // -------------------------------------------------------------------------
    std::shared_ptr<const SceneMaterialLibrary> mat_lib;  // library currently resident on the GPU
    std::vector<UITexture> mat_textures;                  // per SceneTexture (null = missing -> default)
    std::vector<UITexture> lm_textures;                   // SceneMaterialLibrary::lightmap_textures (null = unreadable)
    std::vector<Program> mat_programs;                    // per MaterialShader (prog 0 = failed -> legacy)
    GLuint mat_vertex_shader = 0;                         // the shared material vertex stage, linked into each
    bool mat_vertex_ok = false;
    UITexture tex_default_white;
    UITexture tex_default_flat_normal;
    UITexture tex_default_black;
    UITexture tex_default_cube;

    // UE3 culls back faces of non-two-sided materials. ME_CULL=off|cw|ccw overrides (debugging).
    // With this renderer's view/projection, UE3 front faces are counter-clockwise on screen in Metal's
    // and Direct3D's terms. The translated vertex shaders flip clip-space y (so the framebuffer's rows
    // run top-down as theirs do), which mirrors the winding: glFrontFace gets the opposite value.
    bool mat_cull_enabled = true;
    bool mat_front_ccw = true;

    void configure_material_culling() {
        if (const char* env = std::getenv("ME_GL_SORT")) sort_opaque = !(env[0] == '0');
        if (const char* env = std::getenv("ME_CULL")) {
            const std::string v = env;
            if (v == "0" || v == "off" || v == "none") {
                mat_cull_enabled = false;
            } else if (v == "ccw") {
                mat_front_ccw = true;
            } else if (v == "cw") {
                mat_front_ccw = false;
            }
        }
    }

    // -------------------------------------------------------------------------
    // Device: the GL context
    // -------------------------------------------------------------------------
    static bool want_debug() {
        const char* env = std::getenv("ME_GL_DEBUG");
        return env && env[0] != '\0' && env[0] != '0';
    }

    static void set_context_attributes() {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        // Forward-compatible is what macOS needs for a core profile; the debug flag is optional.
        int flags = SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG;
        if (want_debug()) flags |= SDL_GL_CONTEXT_DEBUG_FLAG;
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, flags);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
        // The frame is blitted to the window as display values; no conversion on the way out.
        SDL_GL_SetAttribute(SDL_GL_FRAMEBUFFER_SRGB_CAPABLE, 0);
    }

    // Creates the context on `given` or, for null, on a hidden window of its own (the headless oracle).
    bool create_context(SDL_Window* given) {
        debug = want_debug();
        if (!given) {
            if (!SDL_WasInit(SDL_INIT_VIDEO)) {
                if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
                    // No display (a server, CI): SDL's off-screen driver renders through EGL on Mesa.
                    const std::string first_error = SDL_GetError();
                    if (SDL_VideoInit("offscreen") != 0) {
                        std::cerr << "[OpenGLRenderer] SDL video: " << first_error << "; offscreen: " << SDL_GetError() << std::endl;
                        return false;
                    }
                }
            }
            set_context_attributes();
            own_window = SDL_CreateWindow("Mirror's Edge (off-screen)", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, width,
                                          height, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
            if (!own_window) {
                std::cerr << "[OpenGLRenderer] Cannot create the hidden window: " << SDL_GetError() << std::endl;
                return false;
            }
            given = own_window;
        }
        window = given;
        // The version, profile and flags are read when the context is made (the window's pixel
        // format was fixed by prepare_window_attributes() before the window was created).
        set_context_attributes();
        context = SDL_GL_CreateContext(window);
        if (!context && debug) {
            // Not every driver gives a debug context; the game runs without one.
            std::cerr << "[OpenGLRenderer] ME_GL_DEBUG: no debug context (" << SDL_GetError() << "), trying without." << std::endl;
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
            context = SDL_GL_CreateContext(window);
        }
        if (!context) {
            std::cerr << "[OpenGLRenderer] No OpenGL 4.1 core context: " << SDL_GetError() << std::endl;
            return false;
        }
        SDL_GL_MakeCurrent(window, context);
        std::vector<std::string> missing;
        if (!gl::load(&missing)) {
            std::cerr << "[OpenGLRenderer] The driver lacks OpenGL 4.1 core functions:";
            for (const std::string& m : missing) std::cerr << " " << m;
            std::cerr << std::endl;
            return false;
        }
        const char* renderer_str = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        const char* version_str = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        const char* glsl_str = reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION));
        adapter_name = renderer_str ? renderer_str : "unknown renderer";
        std::cout << "[OpenGLRenderer] " << adapter_name << ", OpenGL " << (version_str ? version_str : "?") << ", GLSL "
                  << (glsl_str ? glsl_str : "?") << std::endl;
        GLint major = 0, minor = 0;
        glGetIntegerv(GL_MAJOR_VERSION, &major);
        glGetIntegerv(GL_MINOR_VERSION, &minor);
        if (major * 10 + minor < 41) {
            std::cerr << "[OpenGLRenderer] OpenGL " << major << "." << minor << " context; 4.1 core is needed." << std::endl;
            return false;
        }
        // ME_VSYNC=0 lets the frame rate run free (to measure it); otherwise the display's.
        if (!headless) {
            const char* vsync = std::getenv("ME_VSYNC");
            SDL_GL_SetSwapInterval((vsync && vsync[0] == '0') ? 0 : 1);
        }

        has_s3tc = gl::has_extension("GL_EXT_texture_compression_s3tc");
        has_anisotropy = gl::has_extension("GL_EXT_texture_filter_anisotropic") || gl::has_extension("GL_ARB_texture_filter_anisotropic");
        if (!has_s3tc) {
            std::cerr << "[OpenGLRenderer] No GL_EXT_texture_compression_s3tc: the level's DXT textures will be missing." << std::endl;
        }
        glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &max_units);
        max_units = std::min<GLint>(max_units, kMaxUnits);
        glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &uniform_offset_alignment);
        if (uniform_offset_alignment < 16) uniform_offset_alignment = 16;
        GLint max_blocks = 0;
        glGetIntegerv(GL_MAX_UNIFORM_BUFFER_BINDINGS, &max_blocks);
        if (max_blocks < static_cast<GLint>(kFragmentBlockBase) + 3) {
            std::cerr << "[OpenGLRenderer] Only " << max_blocks << " uniform buffer binding points." << std::endl;
            return false;
        }

        if (debug && glDebugMessageCallback) {
            glEnable(GL_DEBUG_OUTPUT);
            glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
            glDebugMessageCallback(gl_debug_callback, &debug_messages_printed);
        } else if (debug) {
            std::cerr << "[OpenGLRenderer] ME_GL_DEBUG: no GL_KHR_debug on this driver; glGetError is polled instead." << std::endl;
        }

        // State that never changes.
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
        glGenVertexArrays(1, &empty_vao);
        glBindVertexArray(empty_vao);
        cur_vao = empty_vao;
        return true;
    }

    // -------------------------------------------------------------------------
    // Shaders
    // -------------------------------------------------------------------------
    // A compiled shader object for a translated stage, or 0 with `error` set.
    GLuint compile(const GlslShader& sh, std::string* error) const {
        if (!sh.error.empty()) {
            if (error) *error = sh.error;
            return 0;
        }
        const GLuint shader = glCreateShader(sh.vertex_stage ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER);
        const char* src = sh.source.c_str();
        const GLint len = static_cast<GLint>(sh.source.size());
        glShaderSource(shader, 1, &src, &len);
        glCompileShader(shader);
        GLint ok = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            if (error) *error = shader_log(shader, true);
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    }

    static std::string shader_log(GLuint object, bool is_shader) {
        GLint length = 0;
        if (is_shader) glGetShaderiv(object, GL_INFO_LOG_LENGTH, &length);
        else glGetProgramiv(object, GL_INFO_LOG_LENGTH, &length);
        std::string log(static_cast<size_t>(std::max(length, 1)), '\0');
        GLsizei written = 0;
        if (is_shader) glGetShaderInfoLog(object, length, &written, log.data());
        else glGetProgramInfoLog(object, length, &written, log.data());
        log.resize(static_cast<size_t>(std::max<GLsizei>(written, 0)));
        return log.empty() ? std::string("(no log)") : log;
    }

    // The sampler object for one of the MSL's `constexpr sampler`s, shared by configuration.
    GLuint static_sampler_object(const GlslStaticSampler& s) {
        const int key = (s.linear ? 1 : 0) | (s.repeat ? 2 : 0) | (s.compare ? 4 : 0);
        GLuint& obj = static_samplers[key];
        if (!obj) {
            const GLenum address = s.repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
            const GLenum filter = s.linear ? GL_LINEAR : GL_NEAREST;
            obj = make_sampler(filter, filter, address, address, address, 0.0f, 1, s.compare);
        }
        return obj;
    }

    // Links a vertex and a fragment stage, then wires the program up by name: each uniform block to
    // its stage's binding point range, each combined sampler to a texture unit. Deletes nothing it was
    // given. Returns false with `error` set.
    bool link_program(GLuint vs, const GlslShader& vsh, GLuint fs, const GlslShader& fsh, const MslToGlsl& translator,
                      Program& out, std::string* error) {
        out.prog = glCreateProgram();
        glAttachShader(out.prog, vs);
        glAttachShader(out.prog, fs);
        glLinkProgram(out.prog);
        GLint ok = GL_FALSE;
        glGetProgramiv(out.prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            if (error) *error = "link: " + shader_log(out.prog, false);
            glDeleteProgram(out.prog);
            out.prog = 0;
            return false;
        }
        for (const GlslShader* sh : {&vsh, &fsh}) {
            for (const GlslUniformBlock& b : sh->uniform_blocks) {
                const GLuint index = glGetUniformBlockIndex(out.prog, b.block_name.c_str());
                if (index != GL_INVALID_INDEX) glUniformBlockBinding(out.prog, index, block_binding(sh->vertex_stage, b.buffer_slot));
            }
        }
        // Units: MSL texture(n) on unit n; a second sampler on the same texture takes a spare unit.
        bool taken[kMaxUnits] = {};
        int next_extra = kExtraUnitBase;
        out.units.clear();
        glUseProgram(out.prog);
        for (const GlslShader* sh : {&vsh, &fsh}) {
            for (const GlslSamplerUnit& s : sh->samplers) {
                const GLint loc = glGetUniformLocation(out.prog, s.uniform.c_str());
                if (loc < 0) continue;  // optimised away
                int unit = -1;
                if (s.texture_slot >= 0 && s.texture_slot < kTextureSlots && s.texture_slot < max_units && !taken[s.texture_slot]) {
                    unit = s.texture_slot;
                } else {
                    while (next_extra < max_units && taken[next_extra]) ++next_extra;
                    if (next_extra < max_units) unit = next_extra;
                }
                if (unit < 0 || s.texture_slot < 0 || s.texture_slot >= kTextureSlots) {
                    if (error) *error = "no texture unit for " + s.uniform;
                    glUseProgram(current_program ? current_program->prog : 0);
                    glDeleteProgram(out.prog);
                    out.prog = 0;
                    return false;
                }
                taken[unit] = true;
                UnitBinding u;
                u.unit = static_cast<GLuint>(unit);
                u.texture_slot = s.texture_slot;
                u.sampler_slot = s.static_sampler ? -1 : s.sampler_slot;
                if (s.static_sampler) {
                    for (const GlslStaticSampler& ss : translator.static_samplers()) {
                        if (ss.slot == s.sampler_slot) u.fixed_sampler = static_sampler_object(ss);
                    }
                }
                out.units.push_back(u);
                glUniform1i(loc, unit);
            }
        }
        glUseProgram(current_program ? current_program->prog : 0);
        return true;
    }

    void destroy_program(Program& p) {
        if (p.prog && gl::loaded()) glDeleteProgram(p.prog);
        if (current_program == &p) current_program = nullptr;
        p = Program{};
    }

    // Vertex layout for a vertex stage that reads me::Vertex (or the material prelude's copy of it)
    // through the attributes the translator assigned to the struct's members (location i = member i).
    bool vertex_layout(const GlslShader& sh, VertexLayout& out) const {
        out = VertexLayout{};
        GLsizei offset = 0;
        for (size_t i = 0; i < sh.vertex_member_types.size(); ++i) {
            const std::string& type = sh.vertex_member_types[i];
            VertexAttrib a;
            a.index = static_cast<GLuint>(i);
            a.offset = offset;
            GLsizei size = 0;
            if (type == "float") { a.size = 1; size = 4; }
            else if (type == "float2") { a.size = 2; size = 8; }
            else if (type == "float3" || type == "packed_float3") { a.size = 3; size = 12; }
            else if (type == "float4") { a.size = 4; size = 16; }
            else if (type == "uint") { a.size = 1; a.type = GL_UNSIGNED_INT; a.integer = true; size = 4; }
            else if (type == "int") { a.size = 1; a.type = GL_INT; a.integer = true; size = 4; }
            else return false;
            out.attribs.push_back(a);
            offset += size;
        }
        if (offset != static_cast<GLsizei>(sizeof(Vertex))) {
            std::cerr << "[OpenGLRenderer] The shaders' vertex struct is " << offset << " bytes, me::Vertex is " << sizeof(Vertex)
                      << std::endl;
            return false;
        }
        out.stride = offset;
        return true;
    }

    static bool same_layout(const VertexLayout& a, const VertexLayout& b) {
        if (a.stride != b.stride || a.attribs.size() != b.attribs.size()) return false;
        for (size_t i = 0; i < a.attribs.size(); ++i) {
            const VertexAttrib& x = a.attribs[i];
            const VertexAttrib& y = b.attribs[i];
            if (x.index != y.index || x.size != y.size || x.type != y.type || x.integer != y.integer || x.offset != y.offset) return false;
        }
        return true;
    }

    // `pixel_fn` may be null: the shadow pass has no fragment function in the MSL (depth is all it
    // writes). Core OpenGL accepts a program without one, but a stand-in keeps every driver happy.
    bool make_program(const char* vertex_fn, const char* pixel_fn, VertexKind kind, Program& out) {
        std::string err;
        const GlslShader vs = builtin_shaders.emit(vertex_fn);
        const GLuint vs_obj = compile(vs, &err);
        if (!vs_obj) {
            std::cerr << "[OpenGLRenderer] Shader compilation failed (" << vertex_fn << "): " << err << std::endl;
            return false;
        }
        GlslShader fs;
        if (pixel_fn) {
            fs = builtin_shaders.emit(pixel_fn);
        } else {
            fs.entry = "depth_only";
            fs.source = "#version 410 core\nvoid main() {}\n";
        }
        const GLuint fs_obj = compile(fs, &err);
        if (!fs_obj) {
            std::cerr << "[OpenGLRenderer] Shader compilation failed (" << (pixel_fn ? pixel_fn : fs.entry.c_str()) << "): " << err
                      << std::endl;
            glDeleteShader(vs_obj);
            return false;
        }
        const bool linked = link_program(vs_obj, vs, fs_obj, fs, builtin_shaders, out, &err);
        glDeleteShader(vs_obj);
        glDeleteShader(fs_obj);
        if (!linked) {
            std::cerr << "[OpenGLRenderer] Shader compilation failed (" << vertex_fn << "): " << err << std::endl;
            return false;
        }
        out.kind = kind;
        if (kind == VertexKind::Scene) {
            VertexLayout layout;
            if (!vertex_layout(vs, layout)) {
                std::cerr << "[OpenGLRenderer] No vertex layout for " << vertex_fn << std::endl;
                destroy_program(out);
                return false;
            }
            VertexLayout& scene = layouts[static_cast<int>(VertexKind::Scene)];
            if (scene.attribs.empty()) {
                scene = layout;
            } else if (!same_layout(scene, layout)) {
                std::cerr << "[OpenGLRenderer] " << vertex_fn << " reads a different vertex layout from the other scene shaders"
                          << std::endl;
                destroy_program(out);
                return false;
            }
        }
        return true;
    }

    GLuint make_sampler(GLenum min_filter, GLenum mag_filter, GLenum u, GLenum v, GLenum w, float max_lod, int anisotropy = 1,
                        bool compare = false) {
        GLuint s = 0;
        glGenSamplers(1, &s);
        glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(min_filter));
        glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(mag_filter));
        glSamplerParameteri(s, GL_TEXTURE_WRAP_S, static_cast<GLint>(u));
        glSamplerParameteri(s, GL_TEXTURE_WRAP_T, static_cast<GLint>(v));
        glSamplerParameteri(s, GL_TEXTURE_WRAP_R, static_cast<GLint>(w));
        glSamplerParameterf(s, GL_TEXTURE_MIN_LOD, 0.0f);
        glSamplerParameterf(s, GL_TEXTURE_MAX_LOD, max_lod);
        if (anisotropy > 1 && has_anisotropy) glSamplerParameterf(s, GL_TEXTURE_MAX_ANISOTROPY, static_cast<float>(anisotropy));
        if (compare) {
            glSamplerParameteri(s, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
            glSamplerParameteri(s, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
        } else {
            glSamplerParameteri(s, GL_TEXTURE_COMPARE_MODE, GL_NONE);
        }
        return s;
    }

    // The MSL's `constexpr sampler`s are wired to their units when a program is linked
    // (link_program), so unlike the other backends nothing has to be bound per pass here.

    bool compile_shaders() {
        std::string err;
        // The sun shadow lookup is shared with the generated material shaders (renderer/sun_shadow.hpp).
        if (!builtin_shaders.add_source(sun_shadow_msl() + kBuiltinShadersMSL, &err)) {
            std::cerr << "[OpenGLRenderer] Cannot translate the built-in shaders: " << err << std::endl;
            return false;
        }
        // The HUD and UI layouts (fixed structs); the scene layout comes from the first scene program.
        {
            VertexLayout& hud = layouts[static_cast<int>(VertexKind::Hud)];
            hud.stride = sizeof(HUDVertex);
            hud.attribs = {{0, 2, GL_FLOAT, false, static_cast<GLsizei>(offsetof(HUDVertex, position))},
                           {1, 4, GL_FLOAT, false, static_cast<GLsizei>(offsetof(HUDVertex, color))}};
            VertexLayout& ui = layouts[static_cast<int>(VertexKind::UiTex)];
            ui.stride = sizeof(UITexVertex);
            ui.attribs = {{0, 2, GL_FLOAT, false, static_cast<GLsizei>(offsetof(UITexVertex, position))},
                          {1, 2, GL_FLOAT, false, static_cast<GLsizei>(offsetof(UITexVertex, uv))},
                          {2, 4, GL_FLOAT, false, static_cast<GLsizei>(offsetof(UITexVertex, color))}};
        }
        // Linear Texture Sampler (made first: link_program() falls back to it for unsampled textures)
        linear_sampler = make_sampler(GL_LINEAR, GL_LINEAR, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, 0.0f);

        if (!make_program("shadow_vertex", nullptr, VertexKind::Scene, shadow_program)) return false;
        if (!make_program("sky_vertex", "sky_fragment", VertexKind::None, sky_program)) return false;
        if (!make_program("world_vertex", "world_fragment", VertexKind::Scene, world_program)) return false;
        if (!make_program("viewmodel_vertex", "viewmodel_fragment", VertexKind::Scene, viewmodel_program)) return false;
        if (!make_program("post_vertex", "fog_fragment", VertexKind::None, fog_program)) return false;
        if (!make_program("post_vertex", "mod_shadow_fragment", VertexKind::None, mod_shadow_program)) return false;
        if (!make_program("post_vertex", "haze_fragment", VertexKind::None, haze_program)) return false;
        if (!make_program("post_vertex", "bloom_gather_fragment", VertexKind::None, bloom_gather_program)) return false;
        if (!make_program("post_vertex", "filter_fragment", VertexKind::None, filter_program)) return false;
        if (!make_program("post_vertex", "meter_scene_fragment", VertexKind::None, meter_scene_program)) return false;
        if (!make_program("post_vertex", "meter_fragment", VertexKind::None, meter_program)) return false;
        if (!make_program("post_vertex", "exposure_fragment", VertexKind::None, exposure_program)) return false;
        if (!make_program("post_vertex", "tonemap_fragment", VertexKind::None, tonemap_program)) return false;
        if (!make_program("post_vertex", "finish_fragment", VertexKind::None, finish_program)) return false;
        if (!make_program("hud_vertex", "hud_fragment", VertexKind::Hud, hud_program)) return false;
        if (!make_program("ui_tex_vertex", "ui_tex_fragment", VertexKind::UiTex, ui_tex_program)) return false;

        // Rasterizer: the clip-space y flip in every translated vertex shader mirrors the winding, so
        // front faces are declared the other way round from the Metal backend.
        glFrontFace(mat_front_ccw ? GL_CW : GL_CCW);

        // Blend states
        auto make_blend = [](bool enable, GLenum src, GLenum dst, GLenum src_a, GLenum dst_a) {
            BlendState b;
            b.enable = enable;
            b.src = src;
            b.dst = dst;
            b.src_a = src_a;
            b.dst_a = dst_a;
            return b;
        };
        blend_opaque = make_blend(false, GL_ONE, GL_ZERO, GL_ONE, GL_ZERO);
        blend_translucent = make_blend(true, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
        blend_additive = make_blend(true, GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
        blend_modulate = make_blend(true, GL_DST_COLOR, GL_ZERO, GL_ZERO, GL_ONE);
        blend_fog = make_blend(true, GL_ONE, GL_SRC_ALPHA, GL_ZERO, GL_ONE);
        // Alpha blending for UI overlay
        blend_ui = make_blend(true, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        // Uniform buffers
        auto make_constants = [&](size_t bytes) {
            GLuint b = 0;
            glGenBuffers(1, &b);
            glBindBuffer(GL_UNIFORM_BUFFER, b);
            glBufferData(GL_UNIFORM_BUFFER, static_cast<GLsizeiptr>((bytes + 15u) & ~size_t(15u)), nullptr, GL_STREAM_DRAW);
            return b;
        };
        cb_frame = make_constants(sizeof(FrameUniformsGPU));
        cb_post = make_constants(sizeof(PostUniformsGPU));
        cb_screen = make_constants(16);
        cb_material = make_constants(static_cast<size_t>(kMaxMaterialUniforms) * 16);
        cb_scene = make_constants(sizeof(SceneUniformsGPU));
        return cb_frame && cb_screen && cb_material && linear_sampler && glGetError() == GL_NO_ERROR;
    }

    // -------------------------------------------------------------------------
    // Textures
    // -------------------------------------------------------------------------
    // One complete 2D texture with `levels` mips allocated (and filled when `data` is given at level 0).
    UITexture make_texture_2d(int w, int h, GLenum internal, GLenum format, GLenum type, const void* data, int levels = 1) {
        auto t = std::make_shared<GpuTexture>();
        t->target = GL_TEXTURE_2D;
        t->width = w;
        t->height = h;
        glGenTextures(1, &t->name);
        glBindTexture(GL_TEXTURE_2D, t->name);
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internal), w, h, 0, format, type, data);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels - 1);
        return t;
    }

    UITexture make_solid_texture(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        uint8_t px[4 * 4 * 4];
        for (int i = 0; i < 16; ++i) {
            px[i * 4 + 0] = r;
            px[i * 4 + 1] = g;
            px[i * 4 + 2] = b;
            px[i * 4 + 3] = a;
        }
        return make_texture_2d(4, 4, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, px);
    }

    // A texture the CPU rewrites: decoded video and front end frames (RGBA8, top row first).
    UITexture make_dynamic_texture(int w, int h, bool srgb) {
        return make_texture_2d(w, h, srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    }

    void write_dynamic_texture(const UITexture& t, const uint8_t* rgba) {
        if (!t) return;
        glBindTexture(GL_TEXTURE_2D, t->name);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, t->width, t->height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }

    void create_material_defaults() {
        tex_default_white = make_solid_texture(255, 255, 255, 255);
        tex_default_flat_normal = make_solid_texture(128, 128, 255, 255);
        tex_default_black = make_solid_texture(0, 0, 0, 255);

        // The stand-in for a missing cubemap: a sky gradient over a ground haze.
        constexpr int kCubeDim = 64;
        auto cube = std::make_shared<GpuTexture>();
        cube->target = GL_TEXTURE_CUBE_MAP;
        cube->width = cube->height = kCubeDim;
        glGenTextures(1, &cube->name);
        glBindTexture(GL_TEXTURE_CUBE_MAP, cube->name);
        int mip_count = 0;
        for (int dim = kCubeDim; dim >= 1; dim /= 2) ++mip_count;
        for (int face = 0; face < 6; ++face) {
            int level = 0;
            for (int dim = kCubeDim; dim >= 1; dim /= 2, ++level) {
                std::vector<uint8_t> px(static_cast<size_t>(dim) * static_cast<size_t>(dim) * 4);
                for (int y = 0; y < dim; ++y) {
                    const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(dim);
                    for (int x = 0; x < dim; ++x) {
                        const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(dim);
                        const float sc = 2.0f * u - 1.0f;
                        const float tc = 2.0f * v - 1.0f;
                        float dx = 0.0f, dy = 0.0f, dz = 1.0f;
                        switch (face) {
                            case 0: dx =  1.0f; dy = -tc;   dz = -sc;   break;
                            case 1: dx = -1.0f; dy = -tc;   dz =  sc;   break;
                            case 2: dx =  sc;   dy =  1.0f; dz =  tc;   break;
                            case 3: dx =  sc;   dy = -1.0f; dz = -tc;   break;
                            case 4: dx =  sc;   dy = -tc;   dz =  1.0f; break;
                            default:dx = -sc;   dy = -tc;   dz = -1.0f; break;
                        }
                        const float len = std::sqrt(std::max(dx * dx + dy * dy + dz * dz, 1e-12f));
                        dz /= len;
                        float r = 0.0f, g = 0.0f, b = 0.0f;
                        if (dz >= 0.0f) {
                            const float t = std::pow(1.0f - dz, 2.2f);
                            r = 54.0f * (1.0f - t) + 224.0f * t;
                            g = 126.0f * (1.0f - t) + 238.0f * t;
                            b = 228.0f * (1.0f - t) + 254.0f * t;
                        } else {
                            const float t = std::min(1.0f, -dz * 2.5f);
                            r = 224.0f * (1.0f - t) + 148.0f * t;
                            g = 238.0f * (1.0f - t) + 168.0f * t;
                            b = 254.0f * (1.0f - t) + 196.0f * t;
                        }
                        const size_t idx = (static_cast<size_t>(y) * static_cast<size_t>(dim) + static_cast<size_t>(x)) * 4;
                        px[idx + 0] = static_cast<uint8_t>(std::clamp(r, 0.0f, 255.0f));
                        px[idx + 1] = static_cast<uint8_t>(std::clamp(g, 0.0f, 255.0f));
                        px[idx + 2] = static_cast<uint8_t>(std::clamp(b, 0.0f, 255.0f));
                        px[idx + 3] = 255;
                    }
                }
                glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + static_cast<GLenum>(face), level, GL_SRGB8_ALPHA8, dim, dim, 0, GL_RGBA,
                             GL_UNSIGNED_BYTE, px.data());
            }
        }
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, mip_count - 1);
        tex_default_cube = cube;

        // UE3 samples each texture with its own AddressX/AddressY and trilinear/anisotropic filtering.
        for (int x = 0; x < 3; ++x) {
            for (int y = 0; y < 3; ++y) {
                mat_samplers[x][y] = make_sampler(GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR, gl_address_mode(static_cast<TexAddress>(x)),
                                                  gl_address_mode(static_cast<TexAddress>(y)), GL_REPEAT, 1000.0f, 8);
            }
        }
        mat_cube_sampler = make_sampler(GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, 1000.0f);
        // The scene copies have one mip; the light maps bound beside them have a full chain.
        scene_copy_sampler = make_sampler(GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, 1000.0f);
    }

    // Uploads one decoded UE3 texture (2D or cube, full mip chain). Must run on the context's thread.
    UITexture upload_scene_texture(const SceneTexture& st) {
        if (!st.valid()) return nullptr;
        const GlTexFormat fmt = gl_tex_format(st.format, st.srgb);
        if (fmt.internal == 0) return nullptr;
        if (fmt.compressed && !has_s3tc) return nullptr;

        const int face_count = st.is_cube ? 6 : 1;
        int levels = INT_MAX;
        for (int f = 0; f < face_count; ++f) {
            const std::vector<TextureMip>& chain = st.is_cube ? st.faces[static_cast<size_t>(f)] : st.mips;
            levels = std::min(levels, tex_valid_chain(st.format, chain));
            if (st.is_cube && (chain.empty() || chain[0].width != st.faces[0][0].width || chain[0].height != chain[0].width)) {
                return nullptr;
            }
        }
        if (levels <= 0 || levels == INT_MAX) return nullptr;
        const TextureMip& top = st.is_cube ? st.faces[0][0] : st.mips[0];

        auto t = std::make_shared<GpuTexture>();
        t->target = st.is_cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
        t->width = top.width;
        t->height = top.height;
        glGenTextures(1, &t->name);
        glBindTexture(t->target, t->name);
        for (int f = 0; f < face_count; ++f) {
            const std::vector<TextureMip>& chain = st.is_cube ? st.faces[static_cast<size_t>(f)] : st.mips;
            const GLenum face_target = st.is_cube ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + static_cast<GLenum>(f) : GL_TEXTURE_2D;
            for (int i = 0; i < levels; ++i) {
                const TextureMip& m = chain[static_cast<size_t>(i)];
                if (fmt.compressed) {
                    const GLsizei bytes = static_cast<GLsizei>(tex_row_bytes(st.format, m.width) * tex_rows(st.format, m.height));
                    glCompressedTexImage2D(face_target, i, fmt.internal, m.width, m.height, 0, bytes, m.data.data());
                } else {
                    glTexImage2D(face_target, i, static_cast<GLint>(fmt.internal), m.width, m.height, 0, fmt.format, fmt.type, m.data.data());
                }
            }
        }
        glTexParameteri(t->target, GL_TEXTURE_MAX_LEVEL, levels - 1);
        if (fmt.swizzled) glTexParameteriv(t->target, GL_TEXTURE_SWIZZLE_RGBA, fmt.swizzle);
        return t;
    }

    // Compiles every generated material fragment shader from the library's MSL and links each with
    // the shared material vertex stage. The translation runs in parallel; the GL compiles cannot
    // (the context belongs to this thread). Failing shaders fall back to the legacy pipeline.
    void compile_material_shaders(const SceneMaterialLibrary& lib) {
        const size_t n = lib.shaders.size();
        for (Program& p : mat_programs) destroy_program(p);
        mat_programs.assign(n, Program{});
        if (mat_vertex_shader) glDeleteShader(mat_vertex_shader);
        mat_vertex_shader = 0;
        mat_vertex_ok = false;
        material_shaders = std::make_unique<MslToGlsl>();
        if (n == 0) return;

        std::string err;
        if (!material_shaders->add_source(lib.common_source, &err)) {
            std::cerr << "[OpenGLRenderer] Cannot translate the material prelude: " << err << std::endl;
            return;
        }
        const GlslShader vs = material_shaders->emit(lib.vertex_function);
        mat_vertex_shader = compile(vs, &err);
        if (!mat_vertex_shader) {
            std::cerr << "[OpenGLRenderer] Material vertex shader failed: " << err << std::endl;
            return;
        }
        VertexLayout layout;
        if (!vertex_layout(vs, layout) || !same_layout(layout, layouts[static_cast<int>(VertexKind::Scene)])) {
            std::cerr << "[OpenGLRenderer] The material vertex shader reads a different vertex layout from the built-in ones"
                      << std::endl;
            glDeleteShader(mat_vertex_shader);
            mat_vertex_shader = 0;
            return;
        }
        mat_vertex_ok = true;

        std::vector<GlslShader> translated(n);
        const MslToGlsl* translator = material_shaders.get();
        parallel_for(n, [&](size_t i) {
            const MaterialShader& sh = lib.shaders[i];
            if (sh.num_uniforms > kMaxMaterialUniforms) {
                translated[i].error = "more than " + std::to_string(kMaxMaterialUniforms) + " uniforms";
            } else {
                translated[i] = translator->emit_from(sh.source, sh.function_name, sh.num_uniforms);
            }
        });

        std::vector<std::string> errors;
        size_t ok = 0;
        for (size_t i = 0; i < n; ++i) {
            const MaterialShader& sh = lib.shaders[i];
            std::string message;
            const GLuint fs = compile(translated[i], &message);
            if (fs) {
                if (link_program(mat_vertex_shader, vs, fs, translated[i], *translator, mat_programs[i], &message)) {
                    mat_programs[i].kind = VertexKind::Scene;
                    ++ok;
                }
                glDeleteShader(fs);
            }
            if (!mat_programs[i].prog) errors.push_back(sh.function_name + " (" + sh.base_material + "): " + message);
        }
        std::cout << "[OpenGLRenderer] Material shaders: " << ok << "/" << n << " compiled" << std::endl;
        for (size_t e = 0; e < errors.size() && e < 4; ++e) {
            std::string msg = errors[e];
            if (msg.size() > 900) msg = msg.substr(0, 900) + " ...";
            std::cerr << "[OpenGLRenderer]   shader error: " << msg << std::endl;
        }
    }

    // Makes `lib` the resident material library (uploads textures, compiles shaders) if it changed.
    void sync_material_library(const std::shared_ptr<const SceneMaterialLibrary>& lib) {
        if (lib == mat_lib) return;
        mat_lib = lib;
        mat_textures.clear();
        lm_textures.clear();
        for (Program& p : mat_programs) destroy_program(p);
        mat_programs.clear();
        if (!lib) return;

        const auto t0 = std::chrono::steady_clock::now();
        const size_t n_tex = lib->textures.size();
        mat_textures.assign(n_tex, nullptr);
        size_t uploaded = 0;
        for (size_t i = 0; i < n_tex; ++i) {
            mat_textures[i] = upload_scene_texture(lib->textures[i]);
            if (mat_textures[i]) ++uploaded;
        }
        lm_textures.assign(lib->lightmap_textures.size(), nullptr);
        for (size_t i = 0; i < lm_textures.size(); ++i) {
            if (lib->lightmap_textures[i].valid()) lm_textures[i] = upload_scene_texture(lib->lightmap_textures[i]);
        }
        compile_material_shaders(*lib);
        upload_material_uniforms(*lib);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::cout << "[OpenGLRenderer] Material library resident: " << lib->materials.size() << " materials, "
                  << uploaded << "/" << n_tex << " textures uploaded in " << secs << " s" << std::endl;
    }

    // Returns the program for a mesh section, or null when it must use legacy procedural shading.
    const Program* section_shader(const MeshSection& s, const MaterialShader** out_shader,
                                  const SceneMaterial** out_material) const {
        if (!mat_lib || !mat_vertex_ok || s.material < 0 || static_cast<size_t>(s.material) >= mat_lib->materials.size()) {
            return nullptr;
        }
        const SceneMaterial& m = mat_lib->materials[static_cast<size_t>(s.material)];
        if (m.shader < 0 || static_cast<size_t>(m.shader) >= mat_programs.size()) return nullptr;
        const Program* p = &mat_programs[static_cast<size_t>(m.shader)];
        if (!p->prog) return nullptr;
        if (out_shader) *out_shader = &mat_lib->shaders[static_cast<size_t>(m.shader)];
        if (out_material) *out_material = &m;
        return p;
    }

    // Uploads values that differ from the material's own (the menu's 'Selected' parameter) and binds
    // them in place of its range.
    void set_material_uniforms(const std::vector<std::array<float, 4>>& values, int count) {
        std::array<std::array<float, 4>, kMaxMaterialUniforms> padded;
        const size_t n = static_cast<size_t>(std::min(count, kMaxMaterialUniforms));
        for (size_t i = 0; i < n; ++i) padded[i] = (i < values.size()) ? values[i] : std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f};
        update_constants(cb_material, padded.data(), n * 16);
        bind_uniform_block(block_binding(false, matbind::kMaterialBuffer), cb_material);
    }

    // One buffer with every material's uniforms, each shader's count of float4s at an offset the
    // driver accepts for glBindBufferRange.
    void upload_material_uniforms(const SceneMaterialLibrary& lib) {
        if (mat_uniform_buffer) glDeleteBuffers(1, &mat_uniform_buffer);
        mat_uniform_buffer = 0;
        mat_uniform_offsets.assign(lib.materials.size(), -1);
        std::vector<float> data;
        const size_t align_floats = static_cast<size_t>(uniform_offset_alignment) / sizeof(float);
        for (size_t mi = 0; mi < lib.materials.size(); ++mi) {
            const SceneMaterial& m = lib.materials[mi];
            if (m.shader < 0 || static_cast<size_t>(m.shader) >= lib.shaders.size()) continue;
            const int count = std::min(lib.shaders[static_cast<size_t>(m.shader)].num_uniforms, kMaxMaterialUniforms);
            if (count <= 0) continue;
            const size_t start = (data.size() + align_floats - 1) / align_floats * align_floats;
            data.resize(start + static_cast<size_t>(count) * 4, 0.0f);
            for (size_t i = 0; i < static_cast<size_t>(count) && i < m.uniforms.size(); ++i) {
                std::memcpy(&data[start + i * 4], m.uniforms[i].data(), 16);
            }
            mat_uniform_offsets[mi] = static_cast<GLintptr>(start * sizeof(float));
        }
        if (data.empty()) return;
        glGenBuffers(1, &mat_uniform_buffer);
        glBindBuffer(GL_UNIFORM_BUFFER, mat_uniform_buffer);
        glBufferData(GL_UNIFORM_BUFFER, static_cast<GLsizeiptr>(data.size() * sizeof(float)), data.data(), GL_STATIC_DRAW);
    }

    // Binds a section's three light-map coefficient textures (texture(24..26), sampler(15)). A
    // section without a set gets white: its vertices carry their own samples, or none. A texture
    // that could not be read is black, so what it would have lit stays unlit.
    void bind_lightmap(int32_t set) {
        for (int k = 0; k < 3; ++k) {
            const GpuTexture* t = tex_default_white.get();
            if (set >= 0) {
                const size_t i = static_cast<size_t>(set) * 3 + static_cast<size_t>(k);
                t = (i < lm_textures.size() && lm_textures[i]) ? lm_textures[i].get() : tex_default_black.get();
            }
            bind_texture(matbind::kLightMapTexture + k, t);
        }
        bind_sampler(matbind::kSceneSampler, scene_copy_sampler);
    }

    // Binds a material instance's textures, samplers and parameter uniforms.
    void bind_material(const SceneMaterial& m, const MaterialShader& sh) {
        const int n2d = std::min(sh.num_tex2d, static_cast<int>(matbind::kMaxTextureSlots));
        const int ncube = std::min(sh.num_texcube, static_cast<int>(matbind::kMaxTextureSlots) - n2d);
        for (int k = 0; k < n2d; ++k) {
            const int ti = (static_cast<size_t>(k) < m.tex2d.size()) ? m.tex2d[static_cast<size_t>(k)] : -1;
            const GpuTexture* t = (ti >= 0 && static_cast<size_t>(ti) < mat_textures.size()) ? mat_textures[static_cast<size_t>(ti)].get() : nullptr;
            TexAddress ax = TexAddress::Wrap;
            TexAddress ay = TexAddress::Wrap;
            if (t) {
                ax = mat_lib->textures[static_cast<size_t>(ti)].address_x;
                ay = mat_lib->textures[static_cast<size_t>(ti)].address_y;
            } else {
                const TexDefault def = (static_cast<size_t>(k) < m.tex2d_default.size()) ? m.tex2d_default[static_cast<size_t>(k)]
                                                                                       : TexDefault::White;
                t = (def == TexDefault::FlatNormal) ? tex_default_flat_normal.get()
                    : (def == TexDefault::Black)    ? tex_default_black.get()
                                                    : tex_default_white.get();
            }
            bind_texture(k, t);
            bind_sampler(k, mat_samplers[static_cast<int>(ax) % 3][static_cast<int>(ay) % 3]);
        }
        for (int j = 0; j < ncube; ++j) {
            const int ti = (static_cast<size_t>(j) < m.texcube.size()) ? m.texcube[static_cast<size_t>(j)] : -1;
            const GpuTexture* t = (ti >= 0 && static_cast<size_t>(ti) < mat_textures.size()) ? mat_textures[static_cast<size_t>(ti)].get() : nullptr;
            if (!t || t->target != GL_TEXTURE_CUBE_MAP) t = tex_default_cube.get();
            bind_texture(n2d + j, t);
            bind_sampler(n2d + j, mat_cube_sampler);
        }
        if (sh.num_uniforms > 0) {
            const size_t mi = static_cast<size_t>(&m - mat_lib->materials.data());
            const GLintptr offset = (mi < mat_uniform_offsets.size()) ? mat_uniform_offsets[mi] : -1;
            if (offset >= 0 && mat_uniform_buffer) {
                bind_uniform_range(block_binding(false, matbind::kMaterialBuffer), mat_uniform_buffer, offset,
                                   static_cast<GLsizeiptr>(std::min(sh.num_uniforms, kMaxMaterialUniforms)) * 16);
            } else {
                set_material_uniforms(m.uniforms, sh.num_uniforms);
            }
        }
        if (sh.uses_scene_color || sh.uses_scene_depth) {
            bind_texture(matbind::kSceneColorTexture, scene_color_copy.get());
            bind_texture(matbind::kSceneDepthTexture, scene_depth_copy.get());
            bind_sampler(matbind::kSceneSampler, scene_copy_sampler);
        }
    }

    // -------------------------------------------------------------------------
    // Render targets
    // -------------------------------------------------------------------------
    // A framebuffer over `color` (level 0) and, when given, `depth`. 0 if incomplete.
    GLuint make_framebuffer(const GpuTexture* color, const GpuTexture* depth) {
        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        if (color) {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color->name, 0);
            glDrawBuffer(GL_COLOR_ATTACHMENT0);
            glReadBuffer(GL_COLOR_ATTACHMENT0);
        } else {
            glDrawBuffer(GL_NONE);
            glReadBuffer(GL_NONE);
        }
        if (depth) glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth->name, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            glDeleteFramebuffers(1, &fbo);
            return 0;
        }
        return fbo;
    }

    void destroy_framebuffer(GLuint& fbo) {
        if (fbo && gl::loaded()) glDeleteFramebuffers(1, &fbo);
        fbo = 0;
    }

    bool make_color_target(ColorTarget& t, int w, int h, GLenum internal, GLenum format, GLenum type) {
        destroy_framebuffer(t.fbo);
        t = ColorTarget{};
        t.w = std::max(1, w);
        t.h = std::max(1, h);
        t.tex = make_texture_2d(t.w, t.h, internal, format, type, nullptr);
        t.fbo = make_framebuffer(t.tex.get(), nullptr);
        return t.fbo != 0;
    }

    bool allocate_render_targets() {
        // The old targets may still be bound.
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        unbind_shader_resources();
        destroy_framebuffer(offscreen_fbo);
        destroy_framebuffer(scene_fbo);
        destroy_framebuffer(scene_color_fbo);
        destroy_framebuffer(scene_copy_fbo);

        scene_hdr_tex = make_texture_2d(width, height, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, nullptr);
        depth_tex = make_texture_2d(width, height, GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        scene_fbo = make_framebuffer(scene_hdr_tex.get(), depth_tex.get());
        scene_color_fbo = make_framebuffer(scene_hdr_tex.get(), nullptr);
        bool ok = scene_fbo != 0 && scene_color_fbo != 0;

        offscreen_color_tex = make_texture_2d(width, height, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        offscreen_fbo = make_framebuffer(offscreen_color_tex.get(), nullptr);
        ok = ok && offscreen_fbo != 0;

        // The post-process chain's own targets.
        ok = ok && make_color_target(scene_hazed, width, height, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT);
        for (auto& p : picture) ok = ok && make_color_target(p, width, height, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
        ok = ok && make_color_target(scene_effect, width, height, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT);
        ok = ok && make_color_target(filter_a, width / kFilterDownsample, height / kFilterDownsample, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT);
        ok = ok && make_color_target(filter_b, width / kFilterDownsample, height / kFilterDownsample, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT);
        for (int i = 0; i < kMeterSteps; ++i) {
            ok = ok && make_color_target(meter[i], kMeterSizes[i], kMeterSizes[i], GL_RGBA16, GL_RGBA, GL_UNSIGNED_SHORT);
        }
        for (auto& e : exposure) ok = ok && make_color_target(e, 1, 1, GL_R32F, GL_RED, GL_FLOAT);
        exposure_sim_time = -1.0f;

        // Copies of the opaque scene (UE3 "resolved" SceneColor / SceneDepth) sampled by
        // translucent materials (SceneTexture, DestColor, DepthBiasedAlpha/Blend).
        scene_color_copy = make_texture_2d(width, height, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, nullptr);
        scene_depth_copy = make_texture_2d(width, height, GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        scene_copy_fbo = make_framebuffer(scene_color_copy.get(), scene_depth_copy.get());
        ok = ok && scene_copy_fbo != 0;

        if (ok && !shadow_depth_tex) {
            auto t = std::make_shared<GpuTexture>();
            t->target = GL_TEXTURE_2D_ARRAY;
            t->width = t->height = kSunShadowMapSize;
            glGenTextures(1, &t->name);
            glBindTexture(GL_TEXTURE_2D_ARRAY, t->name);
            glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT32F, kSunShadowMapSize, kSunShadowMapSize, 3,  // kSunShadowNearSlice, kSunShadowFarSlice, kModShadowSlice
                         0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 0);
            shadow_depth_tex = t;
            for (int slice = 0; ok && slice < 3; ++slice) {
                glGenFramebuffers(1, &shadow_fbo[slice]);
                glBindFramebuffer(GL_FRAMEBUFFER, shadow_fbo[slice]);
                glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, t->name, 0, slice);
                glDrawBuffer(GL_NONE);
                glReadBuffer(GL_NONE);
                ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
            }
            shadow_far_valid = false;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (!ok) std::cerr << "[OpenGLRenderer] Failed to allocate the render targets (" << width << "x" << height << ")" << std::endl;
        return ok;
    }

    // The finished frame as RGBA8 rows, top row first. The translated vertex shaders flip clip-space
    // y, so row 0 of the off-screen target is the top of the picture and glReadPixels needs no flip.
    bool read_back(std::vector<uint8_t>& pixels) {
        if (!initialized || !offscreen_fbo) return false;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, offscreen_fbo);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        const size_t row = static_cast<size_t>(width) * 4;
        pixels.resize(row * static_cast<size_t>(height));
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        // What a window shows: the blit to it carries no alpha.
        for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
        return true;
    }

    // -------------------------------------------------------------------------
    // Character & menu assets
    // -------------------------------------------------------------------------
    struct WeaponGPUTextures {
        UITexture diffuse;
        UITexture specular;
        UITexture normal;
        UITexture mask;
    };
    UITexture vm_skin_gpu_tex;
    UITexture vm_glove_gpu_tex;
    UITexture vm_lower_gpu_tex;
    struct CharacterGPUTextures {
        UITexture diffuse;
        UITexture specular;
        UITexture normal;
    };
    CharacterGPUTextures enemy_gpu_textures[AnimSystem::EnemyArch_Count];
    UITexture swat_d_gpu_tex;
    UITexture swat_s_gpu_tex;
    UITexture swat_n_gpu_tex;
    UITexture ammo_d_gpu_tex;
    std::unordered_map<std::string, WeaponGPUTextures> weapon_gpu_textures;

    // Decodes a DXT1 texture to RGBA8 and lets the GPU build its mip chain.
    UITexture upload_dxt1_texture(const DXT1Texture& dxt, bool srgb = false) {
        if (!dxt.is_valid()) return nullptr;
        std::vector<uint8_t> rgba;
        if (!dxt.decode_rgba8(rgba) || rgba.empty()) return nullptr;
        const bool mipmapped = dxt.width >= 4 && dxt.height >= 4;
        int levels = 1;
        if (mipmapped) {
            for (int d = std::max(dxt.width, dxt.height); d > 1; d /= 2) ++levels;
        }
        UITexture t = make_texture_2d(dxt.width, dxt.height, srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data(), levels);
        if (mipmapped) glGenerateMipmap(GL_TEXTURE_2D);
        return t;
    }

    // Real UE3 USkeletalMesh & TdAnimSet assets (CH_Faith_1P, KrugerSec/CPF SWAT, weapons) used for
    // the first-person viewmodel, enemies and dropped weapons.
    void load_character_assets() {
        anim_system.init_from_game_root(game_root);
        if (!anim_system.is_loaded()) return;

        vm_skin_gpu_tex  = upload_dxt1_texture(anim_system.faith_skin_tex(), false);
        vm_glove_gpu_tex = upload_dxt1_texture(anim_system.faith_glove_tex(), false);
        vm_lower_gpu_tex = upload_dxt1_texture(anim_system.faith_lower_tex(), false);
        for (size_t a = 0; a < AnimSystem::EnemyArch_Count; ++a) {
            auto arch = static_cast<AnimSystem::EnemyArchetypeId>(a);
            enemy_gpu_textures[a].diffuse  = upload_dxt1_texture(anim_system.enemy_diffuse_tex(arch), true);
            enemy_gpu_textures[a].specular = upload_dxt1_texture(anim_system.enemy_specular_tex(arch), true);
            enemy_gpu_textures[a].normal   = upload_dxt1_texture(anim_system.enemy_normal_tex(arch), false);
        }
        swat_d_gpu_tex   = enemy_gpu_textures[AnimSystem::EnemyArch_SWAT].diffuse;
        swat_s_gpu_tex   = enemy_gpu_textures[AnimSystem::EnemyArch_SWAT].specular;
        swat_n_gpu_tex   = enemy_gpu_textures[AnimSystem::EnemyArch_SWAT].normal;
        ammo_d_gpu_tex   = upload_dxt1_texture(anim_system.ammo_diffuse_tex(), true);

        for (const auto& [wname, wmesh] : anim_system.weapon_meshes()) {
            WeaponGPUTextures wtex{};
            wtex.diffuse  = upload_dxt1_texture(wmesh.tex_diffuse, true);
            wtex.specular = upload_dxt1_texture(wmesh.tex_specular, true);
            wtex.normal   = upload_dxt1_texture(wmesh.tex_normal, false);
            wtex.mask     = upload_dxt1_texture(wmesh.tex_mask, true);
            weapon_gpu_textures[wname] = wtex;
        }

        for (GLuint b : enemy_index_buffers) {
            if (b) glDeleteBuffers(1, &b);
        }
        enemy_index_buffers.clear();
        for (const std::vector<uint32_t>& list : anim_system.enemy_swat_index_lists()) {
            enemy_index_buffers.push_back(list.empty() ? 0 : make_static_buffer(list.data(), list.size() * sizeof(uint32_t)));
        }
    }

    void build_faith_viewmodel(const PlayerTelemetry& telemetry) {
        // CH_Faith_1P skinned by the AnimSystem; nothing is drawn without the real assets.
        if (anim_system.is_loaded()) {
            anim_system.evaluate_faith_1p(telemetry, faith_viewmodel_mesh);
        } else {
            faith_viewmodel_mesh.clear();
        }
    }

    void ensure_main_menu_loaded() {
        if (!main_menu.is_loaded()) {
            main_menu.init(game_root);
        }
        if (!main_menu_gpu_ready) {
            ui_logo_tex      = upload_scene_texture(main_menu.logo_texture());
            ui_bag_tex       = upload_scene_texture(main_menu.icon_bag_texture());
            ui_time_tex      = upload_scene_texture(main_menu.icon_time_texture());
            ui_panel_bg_tex  = upload_scene_texture(main_menu.panel_bg_texture());
            ui_faith_art_tex = upload_scene_texture(main_menu.faith_art_texture());
            for (int i = 0; i < 10; ++i) {
                ui_chapter_tex[i] = upload_scene_texture(main_menu.chapter_preview_texture(i));
            }
            auto upload_font_pages = [&](const UIMultiFont& f, std::vector<UITexture>& out) {
                out.clear();
                out.reserve(f.pages.size());
                for (const auto& st : f.pages) {
                    out.push_back(upload_scene_texture(st));
                }
            };
            upload_font_pages(main_menu.headline_thick_font(), ui_font_headline_thick_tex);
            upload_font_pages(main_menu.headline_light_font(), ui_font_headline_light_tex);
            upload_font_pages(main_menu.medium_italic_font(),  ui_font_medium_italic_tex);
            upload_font_pages(main_menu.small_italic_font(),   ui_font_small_italic_tex);
            main_menu_gpu_ready = true;
        }
    }

    // The HUD, cutscene overlay and chapter-select draw lists: shared with the Metal renderer.
#include "overlay_ui.inl"

    bool init_common(int in_width, int in_height) {
        width = in_width;
        height = in_height;
        if (game_root.empty()) game_root = default_game_root();
        configure_material_culling();
        if (!compile_shaders()) return false;
        create_material_defaults();
        if (!allocate_render_targets()) return false;
        load_character_assets();
        check_errors("init");
        initialized = true;
        return true;
    }

    ~Impl() {
        if (context) {
            // Deleting the context frees every GL object; the texture handles still alive in the
            // members destroyed after this see gl::loaded() == false and leave them be.
            SDL_GL_MakeCurrent(window, context);
            gl::unload();
            SDL_GL_DeleteContext(context);
            context = nullptr;
        }
        if (own_window) {
            SDL_DestroyWindow(own_window);
            own_window = nullptr;
        }
    }
};

// -----------------------------------------------------------------------------
// OpenGLRenderer Public Interface
// -----------------------------------------------------------------------------
OpenGLRenderer::OpenGLRenderer() : impl_(std::make_unique<Impl>()) {}
OpenGLRenderer::~OpenGLRenderer() = default;
OpenGLRenderer::OpenGLRenderer(OpenGLRenderer&&) noexcept = default;
OpenGLRenderer& OpenGLRenderer::operator=(OpenGLRenderer&&) noexcept = default;

void OpenGLRenderer::set_game_root(const std::string& game_root) { impl_->game_root = game_root; }

void OpenGLRenderer::prepare_window_attributes() { Impl::set_context_attributes(); }

bool OpenGLRenderer::init_headless(int width, int height) {
    impl_->headless = true;
    impl_->width = width;
    impl_->height = height;
    if (!impl_->create_context(nullptr)) return false;
    if (!impl_->init_common(width, height)) return false;
    std::cout << "[OpenGLRenderer] Initialized in Headless Mode (" << width << "x" << height << ") on " << impl_->adapter_name
              << std::endl;
    return true;
}

bool OpenGLRenderer::init_with_window(void* sdl_window, int width, int height) {
    if (!sdl_window) return false;
    impl_->headless = false;
    impl_->width = width;
    impl_->height = height;
    if (!impl_->create_context(static_cast<SDL_Window*>(sdl_window))) return false;
    if (!impl_->init_common(width, height)) return false;
    std::cout << "[OpenGLRenderer] Initialized with a window (" << width << "x" << height << ") on " << impl_->adapter_name
              << std::endl;
    return true;
}

void OpenGLRenderer::resize(int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (impl_->width == width && impl_->height == height) return;

    impl_->width = width;
    impl_->height = height;
    if (impl_->initialized) impl_->allocate_render_targets();
}

void OpenGLRenderer::render_frame(const LevelScene& scene, const PlayerTelemetry& telemetry) {
    if (!impl_->initialized) return;
    Impl* const impl = impl_.get();
    auto prof_mark = std::chrono::steady_clock::now();
    // Charges the time since the previous call to `phase` (ME_RENDER_PROF).
    auto prof = [&](Impl::ProfPhase phase) {
        if (!impl->profile) return;
        const auto now = std::chrono::steady_clock::now();
        impl->prof_seconds[phase] += std::chrono::duration<double>(now - prof_mark).count();
        prof_mark = now;
    };

    impl->prof_gpu_begin();

    // ---------------------------------------------------------------------
    // Main Menu / Load Chapter State: Switch to TdMainMenu.me1 3D City
    // ---------------------------------------------------------------------
    bool in_main_menu = false;
    if (impl->menu_open) {
        impl->ensure_main_menu_loaded();
        impl->main_menu.update_selected_chapter_highlight(impl->selected_chapter);
        in_main_menu = impl->main_menu.has_city_scene();
    }
    const LevelScene& active_scene = in_main_menu ? impl->main_menu.city_scene() : scene;

    // ---------------------------------------------------------------------
    // Camera View & Projection Matrices (Unreal Engine to Metal clip space; the translated
    // vertex shaders take it on to OpenGL's)
    // ---------------------------------------------------------------------
    // TdPlayerPawn.CalcCamera: the eyes and the view rotation, moved and turned by the current move's
    // camera animation (the skill roll's somersault).
    Vec3 cam_pos;
    Rotator rot;
    player_camera(telemetry, cam_pos, rot);
    float fov_deg = telemetry.fov_deg;
    float near_plane = 5.0f;
    float far_plane = kFarPlane;

    if (in_main_menu) {
        const MenuChapterEntry& cam_ch = impl->main_menu.get_chapter(impl->selected_chapter);
        cam_pos = cam_ch.camera_location;
        rot = cam_ch.camera_rotation;
        fov_deg = 90.0f;
        near_plane = 10.0f;
        far_plane = 400000.0f;
    }

    // The post-process settings in force where the view is (the world's, or a volume's).
    const PostProcessSettings& view_post = impl->post_settings.update(active_scene, cam_pos, telemetry.sim_time);

    Vec3 fwd = rot.forward();
    Vec3 right = rot.right();
    Vec3 up = rot.up();
    Vec3 target = cam_pos + fwd * 100.0f;

    Mat4 view = Mat4::look_at(cam_pos, target, up);
    float aspect = float(impl->width) / float(impl->height);
    float fov_h_rad = fov_deg * DEG2RAD;
    float fov_y_rad = 2.0f * std::atan(std::tan(fov_h_rad * 0.5f) / aspect);
    Mat4 proj = Mat4::perspective(fov_y_rad, aspect, near_plane, far_plane);
    Mat4 vp = proj * view;

    // Viewmodel camera matrix (DefaultGame.ini Model1pFOV = 100 horizontal)
    Mat4 vm_view = Mat4::look_at(Vec3(0, 0, 0), Vec3(0, 100.0f, 0), Vec3(0, 0, 1));
    float vm_fov_y_rad = 2.0f * std::atan(std::tan(100.0f * DEG2RAD * 0.5f) / aspect);
    Mat4 vm_proj = Mat4::perspective(vm_fov_y_rad, aspect, 1.0f, 500.0f);
    Mat4 vm_vp = vm_proj * vm_view;

    // Pack frame uniforms
    FrameUniformsGPU uniforms{};
    set_matrix(uniforms.view_proj, vp);
    const Mat4 identity = Mat4::identity();
    set_matrix(uniforms.model, identity);

    uniforms.camera_pos = cam_pos;
    uniforms.sim_time = telemetry.sim_time;

    Vec3 sun_d = active_scene.sun_direction.normalized();
    uniforms.sun_dir = sun_d;
    uniforms.sun_color = active_scene.sun_color;
    uniforms.sky_color = active_scene.sky_upper_color;
    uniforms.ground_color = active_scene.sky_lower_color;
    uniforms.speed_2d = impl->menu_open ? 0.0f : telemetry.speed_2d;
    uniforms.reaction_active = (!impl->menu_open && telemetry.reaction_active) ? 1.0f : 0.0f;
    uniforms.health = impl->menu_open ? 100.0f : telemetry.health;
    uniforms.exposure = 1.0f;
    uniforms.actor_tint = Float3(1.0f, 1.0f, 1.0f);
    uniforms.runner_vision_strength = 0.0f;
    uniforms.is_runner_vision = 0.0f;

    uniforms.cam_forward = fwd;
    uniforms.fov_tan = std::tan(fov_y_rad * 0.5f);
    uniforms.cam_right = right;
    uniforms.aspect = aspect;
    uniforms.cam_up = up;

    // Directional sun shadow cascades (renderer/sun_shadow.hpp): orthographic squares centred on the
    // camera and snapped to whole texels in light space, so neither map slides across the world by part
    // of a texel as the camera moves, and turning the camera changes nothing. The main menu instead
    // frames TdMainMenu's miniature City of Glass with one fixed square and has no far cascade.
    const Mat4 sun_near_vp = in_main_menu ? sun_shadow_menu_view_proj(sun_d) : sun_shadow_view_proj(sun_d, cam_pos);
    const Mat4 sun_far_vp = in_main_menu ? sun_near_vp : sun_shadow_far_view_proj(sun_d, cam_pos);
    set_matrix(uniforms.sun_view_proj, sun_near_vp);
    set_matrix(uniforms.sun_view_proj_far, sun_far_vp);
    uniforms.mod_shadow_color = active_scene.mod_shadow_color;
    uniforms.shadow_enabled = in_main_menu ? 2.0f : 1.0f;

    impl->dyn_vertex_cursor = 0;
    const float fw = static_cast<float>(impl->width);
    const float fh = static_cast<float>(impl->height);
    const bool cutscene_active = (impl->cutscene_player != nullptr && impl->cutscene_player->is_playing());
    const bool bink_video_active = (cutscene_active && impl->cutscene_player->get_mode() == ECutsceneMode::BinkVideo);
    // While the front end's frame covers the window, the level under it is not drawn. It is still made
    // resident below, so entering the game does not wait for its shaders and textures.
    const bool frontend_active = impl->frontend_rgba != nullptr && impl->frontend_w > 0 && impl->frontend_h > 0;
    const bool scene_hidden = frontend_active && !impl->frontend_overlay;

    // The frame uniforms as the shaders see them from here on (vertex b1 and fragment b0 share one buffer).
    // The passes push them per mesh; most meshes change nothing (identity model matrix), so only a
    // change is uploaded.
    FrameUniformsGPU pushed_uniforms{};
    bool pushed_valid = false;
    auto push_uniforms = [&]() {
        if (pushed_valid && std::memcmp(&pushed_uniforms, &uniforms, sizeof(uniforms)) == 0) return;
        impl->update_constants(impl->cb_frame, &uniforms, sizeof(uniforms));
        pushed_uniforms = uniforms;
        pushed_valid = true;
    };
    auto set_viewport = [&](float w, float h, float min_depth, float max_depth) {
        impl->set_viewport(static_cast<int>(w), static_cast<int>(h), min_depth, max_depth);
    };
    auto use_program = [&](const Impl::Program& p) { impl->use_program(p); };
    auto set_scene_vertices = [&](Impl::GpuBuffer* buffer) { impl->bind_vertices(buffer, Impl::VertexKind::Scene); };
    auto draw_scene_vertices = [&](const std::vector<Vertex>& verts) {
        set_scene_vertices(impl->acquire_dynamic_vertex_buffer(verts.data(), verts.size() * sizeof(Vertex)));
        impl->draw(static_cast<GLsizei>(verts.size()), 0);
    };
    auto set_blend = [&](const Impl::BlendState& state) { impl->set_blend(state); };

    impl->bind_uniform_block(block_binding(true, 1), impl->cb_frame);
    impl->bind_uniform_block(block_binding(false, 0), impl->cb_frame);
    impl->bind_uniform_block(block_binding(false, 1), impl->cb_material);
    {
        // What a material's fragment shader is given beside the frame's constants: the height fog
        // a translucent surface takes, and (per dynamic object, below) its light environment.
        impl->scene_constants = SceneUniformsGPU{};
        fill_fog_uniforms(active_scene, cam_pos, impl->scene_constants.fog);
        impl->update_constants(impl->cb_scene, &impl->scene_constants, sizeof(impl->scene_constants));
        impl->bind_uniform_block(block_binding(false, matbind::kSceneBuffer), impl->cb_scene);
        impl->light_envs.begin_frame(active_scene);
    }
    // The light environment of what is drawn next; none for the level's own geometry.
    auto set_light_env = [&](const LightEnvLighting* lighting, const Vec3& relative_to) {
        if (!light_env_uniforms(lighting, relative_to, impl->scene_constants.env)) return;
        impl->update_constants(impl->cb_scene, &impl->scene_constants, sizeof(impl->scene_constants));
    };
    impl->unbind_shader_resources();

    // ---------------------------------------------------------------------
    // Mirror's Edge materials: make the scene's material library resident
    // (texture upload + shader translation and compile happen once per
    // library) and find out whether this frame needs a translucency pass /
    // opaque scene copies.
    // ---------------------------------------------------------------------
    impl->sync_material_library(active_scene.materials);

    // Rebuild GPU vertex buffer cache only when scene meshes change
    size_t total_scene_verts = 0;
    for (const auto& m : active_scene.meshes) total_scene_verts += m.vertices.size();

    const bool meshes_changed = (active_scene.map_name != impl->cached_map_name ||
                                 total_scene_verts != impl->cached_total_verts ||
                                 impl->cached_mesh_buffers.size() != active_scene.meshes.size());
    if (meshes_changed) {
        impl->cached_map_name = active_scene.map_name;
        impl->cached_total_verts = total_scene_verts;
        for (Impl::GpuBuffer& b : impl->cached_mesh_buffers) impl->destroy_buffer(b);
        impl->cached_mesh_buffers.clear();
        ++impl->scene_generation;  // the far shadow cascade must be redrawn from the new geometry
        for (const auto& m : active_scene.meshes) {
            Impl::GpuBuffer b;
            if (!m.vertices.empty()) {
                b.vbo = impl->make_static_buffer(m.vertices.data(), m.vertices.size() * sizeof(Vertex));
                b.capacity = m.vertices.size() * sizeof(Vertex);
            }
            impl->cached_mesh_buffers.push_back(b);
        }
    }

    if (meshes_changed || impl->cached_section_mat_lib != impl->mat_lib ||
        impl->cached_section_flags.size() != active_scene.meshes.size()) {
        impl->cached_section_mat_lib = impl->mat_lib;
        impl->cached_has_translucent = false;
        impl->cached_needs_scene_copies = false;
        impl->cached_section_flags.assign(active_scene.meshes.size(), {});
        impl->cached_shadow_ranges.assign(active_scene.meshes.size(), {});
        impl->cached_opaque_order.assign(active_scene.meshes.size(), {});
        for (size_t i = 0; i < active_scene.meshes.size(); ++i) {
            const auto& mesh = active_scene.meshes[i];
            auto& sflags = impl->cached_section_flags[i];
            sflags.assign(mesh.sections.size(), 0u);
            auto& ranges = impl->cached_shadow_ranges[i];
            auto& order = impl->cached_opaque_order[i];
            order.reserve(mesh.sections.size());
            std::vector<int> order_shader(mesh.sections.size(), -1);
            for (size_t si = 0; si < mesh.sections.size(); ++si) {
                const auto& s = mesh.sections[si];
                const MaterialShader* sh = nullptr;
                const SceneMaterial* m = nullptr;
                const Impl::Program* ps = impl->section_shader(s, &sh, &m);
                uint8_t fl = Impl::kSecShadowCaster;
                if (sh && (mat_blend_is_translucent(sh->blend) || sh->lighting == MatLightingModel::Unlit)) {
                    fl &= ~Impl::kSecShadowCaster;
                }
                if (m && (m->name.find("Skydome") != std::string::npos ||
                          m->name.find("skydome") != std::string::npos)) {
                    fl &= ~Impl::kSecShadowCaster;
                }
                if (ps && sh && mat_blend_is_translucent(sh->blend)) {
                    fl |= Impl::kSecTranslucent;
                    impl->cached_has_translucent = true;
                    if (sh->uses_scene_color || sh->uses_scene_depth) {
                        impl->cached_needs_scene_copies = true;
                    }
                } else if (s.vertex_count > 0 &&
                           static_cast<size_t>(s.first_vertex) + static_cast<size_t>(s.vertex_count) <= mesh.vertices.size()) {
                    order.push_back(static_cast<uint32_t>(si));
                    order_shader[si] = (ps && m) ? m->shader : -1;
                }
                sflags[si] = fl;
                if ((fl & Impl::kSecShadowCaster) && s.vertex_count > 0 &&
                    static_cast<size_t>(s.first_vertex) + static_cast<size_t>(s.vertex_count) <= mesh.vertices.size()) {
                    if (!ranges.empty() && ranges.back().first + ranges.back().count == static_cast<GLint>(s.first_vertex)) {
                        ranges.back().count += static_cast<GLsizei>(s.vertex_count);
                    } else {
                        ranges.push_back({static_cast<GLint>(s.first_vertex), static_cast<GLsizei>(s.vertex_count)});
                    }
                }
            }
            if (impl->sort_opaque) {
                std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
                    const MeshSection& sa = mesh.sections[a];
                    const MeshSection& sb = mesh.sections[b];
                    if (order_shader[a] != order_shader[b]) return order_shader[a] < order_shader[b];
                    if (sa.material != sb.material) return sa.material < sb.material;
                    return sa.lightmap < sb.lightmap;
                });
            }
        }
    }
    // The level's particle systems, run up to this frame (particles.hpp). Their sprites are drawn
    // with the translucent surfaces.
    bool particle_copies = false;
    if (!in_main_menu && !scene_hidden && !bink_video_active && !active_scene.particle_systems.empty()) {
        LensFlareView particle_view;
        particle_view.position = cam_pos;
        particle_view.forward = fwd;
        particle_view.right = right;
        particle_view.up = up;
        impl->particles.update(active_scene, particle_view, telemetry.sim_time);
        for (const ParticleBatch& b : impl->particles.batches()) {
            MeshSection section;
            section.material = b.material;
            const MaterialShader* sh = nullptr;
            const SceneMaterial* m = nullptr;
            if (impl->section_shader(section, &sh, &m) && sh && (sh->uses_scene_color || sh->uses_scene_depth)) particle_copies = true;
        }
    } else {
        impl->particles.rest();
    }
    const bool has_translucent = impl->cached_has_translucent || !impl->particles.batches().empty();
    const bool needs_scene_copies = impl->cached_needs_scene_copies || particle_copies;

    prof(Impl::kProfPrepare);

    // Pose active SWAT/CPF enemies once per frame and share between Pass 0 (Shadow) and Pass 1 (World).
    // Enemies are independent (evaluate_enemy_swat_indexed is const and keeps its scratch buffers thread_local),
    // so they are posed in parallel, and an enemy's vertices are uploaded only if its posed bounds can reach
    // the camera view or the near shadow cascade. The GPU would clip an enemy outside both away completely,
    // so skipping it leaves both passes' output unchanged.
    const bool shadow_pass = impl->shadow_depth_tex && impl->shadow_program.prog;
    const bool need_enemies = !scene_hidden && impl->anim_system.is_loaded() && !active_scene.enemies.empty() &&
                              (!impl->menu_open || shadow_pass);
    if (need_enemies) {
        const size_t enemy_count = active_scene.enemies.size();
        if (impl->frame_enemy_draws.size() < enemy_count) impl->frame_enemy_draws.resize(enemy_count);
        if (impl->enemy_mapped.size() < enemy_count) impl->enemy_mapped.resize(enemy_count);
        if (impl->enemy_vertex_buffers.size() < enemy_count) impl->enemy_vertex_buffers.resize(enemy_count);
        const std::vector<EnemyBot>& bots = active_scene.enemies;
        const ClipVolume view_volume(vp);
        const ClipVolume shadow_volume(sun_near_vp);
        const float sim_time = telemetry.sim_time;
        const bool reaction_active = telemetry.reaction_active;
        const bool menu_open = impl->menu_open;
        const size_t max_vertices = impl->anim_system.enemy_swat_max_vertices();

        // The context belongs to this thread, so it decides here which enemies get posed and maps
        // their vertex buffers; the workers then write through the mapped pointers.
        for (size_t ei = 0; ei < enemy_count; ++ei) {
            const auto& bot = bots[ei];
            impl->frame_enemy_draws[ei] = Impl::EnemyFrameDraw{};
            impl->enemy_mapped[ei] = nullptr;
            if (!bot.alive && menu_open) continue;

            // Skinning every enemy of the level costs more than the rest of the frame (there are 29 to 71
            // of them in a chapter), so one that cannot matter is not posed at all. A posed enemy stays
            // close to its origin: the furthest vertex across the ten chapters is 224 UU out. If a sphere
            // several times that size around the origin misses both volumes, so do the posed bounds tested
            // below, and the enemy would not have been drawn.
            const double origin_margin = 16.0 + 1e-4 * (static_cast<double>(bot.position.length()) + cam_pos.length());
            if (!(!menu_open && view_volume.may_cover(bot.position, Impl::kEnemyReach, origin_margin)) &&
                !(shadow_pass && bot.alive && shadow_volume.may_cover(bot.position, Impl::kEnemyReach, origin_margin))) {
                continue;
            }
            impl->enemy_mapped[ei] =
                static_cast<Vertex*>(impl->map_dynamic_buffer(impl->enemy_vertex_buffers[ei], max_vertices * sizeof(Vertex)));
        }

        // Each worker touches only element ei of frame_enemy_draws and enemy ei's mapped vertices.
        parallel_for(enemy_count, [&](size_t ei) {
            Vertex* const gpu_vertices = impl->enemy_mapped[ei];
            if (!gpu_vertices) return;
            const auto& bot = bots[ei];
            auto& draw = impl->frame_enemy_draws[ei];

            // Posed in ordinary memory first: the bounds below read the vertices back, and reading
            // mapped GPU memory is slow.
            thread_local std::vector<Vertex> posed;
            posed.resize(max_vertices);
            if (impl->profile) impl->prof_posed.fetch_add(1, std::memory_order_relaxed);
            const AnimSystem::EnemySwatDraw mesh =
                impl->anim_system.evaluate_enemy_swat_indexed(bot, sim_time, reaction_active, posed.data());
            if (mesh.index_count == 0 || mesh.index_list >= impl->enemy_index_buffers.size() ||
                !impl->enemy_index_buffers[mesh.index_list]) {
                return;
            }
            draw.archetype_id = mesh.archetype_id;

            // World-space bounding sphere of the posed vertices (the triangle list is built from them alone).
            // fmin/fmax skip NaN operands, so a NaN coordinate instead makes the bounds NaN explicitly, and
            // ClipVolume never rejects a NaN sphere.
            float lox = posed[0].position.x, loy = posed[0].position.y, loz = posed[0].position.z;
            float hix = lox, hiy = loy, hiz = loz;
            bool has_nan = false;
            for (size_t v = 0; v < mesh.vertex_count; ++v) {
                const Vec3& p = posed[v].position;
                lox = std::fmin(lox, p.x);
                loy = std::fmin(loy, p.y);
                loz = std::fmin(loz, p.z);
                hix = std::fmax(hix, p.x);
                hiy = std::fmax(hiy, p.y);
                hiz = std::fmax(hiz, p.z);
                has_nan |= (p.x != p.x) | (p.y != p.y) | (p.z != p.z);
            }
            if (has_nan) lox = NAN;
            const Vec3 lo(lox, loy, loz);
            const Vec3 hi(hix, hiy, hiz);
            const Mat4 bot_model = Mat4::translation(bot.position) * Mat4::rotation_z(bot.yaw_deg * DEG2RAD);
            const Vec3 center = bot_model.transform_point((lo + hi) * 0.5f);
            const float radius = (hi - lo).length() * 0.5f;
            // Far beyond the rounding of the GPU's float transforms, which stays well under 1e-5 of the
            // coordinates' magnitudes.
            const double margin = 16.0 + 1e-4 * (static_cast<double>(center.length()) + cam_pos.length());
            draw.in_view = !menu_open && view_volume.may_cover(center, radius, margin);  // world pass: gameplay only
            draw.in_shadow = shadow_pass && bot.alive && shadow_volume.may_cover(center, radius, margin);
            if (!draw.in_view && !draw.in_shadow) return;

            std::memcpy(gpu_vertices, posed.data(), mesh.vertex_count * sizeof(Vertex));
            draw.index_list = mesh.index_list;
            draw.index_count = mesh.index_count;
        });

        for (size_t ei = 0; ei < enemy_count; ++ei) {
            if (impl->enemy_mapped[ei]) impl->unmap_dynamic_buffer(impl->enemy_vertex_buffers[ei]);
        }
    }
    prof(Impl::kProfEnemies);
    // Draws enemy ei's triangle list; it must be in_view or in_shadow.
    auto draw_enemy_mesh = [&](size_t ei) {
        const Impl::EnemyFrameDraw& draw = impl->frame_enemy_draws[ei];
        set_scene_vertices(&impl->enemy_vertex_buffers[ei]);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, impl->enemy_index_buffers[draw.index_list]);
        impl->flush_bindings();
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(draw.index_count), GL_UNSIGNED_INT, nullptr);
        ++impl->prof_draws;
    };

    auto section_in_range = [](const MeshBuffer& mesh, const MeshSection& s) {
        return s.vertex_count > 0 &&
               static_cast<size_t>(s.first_vertex) + static_cast<size_t>(s.vertex_count) <= mesh.vertices.size();
    };

    // Moving elevator parts (InterpActors driven by the elevator matinees) are baked at their
    // initial pose; they are drawn translated by the part's current matinee offset. Sets
    // uniforms.model for scene mesh i and returns true when it is not the identity.
    auto apply_scene_mesh_model = [&](size_t i) -> bool {
        const MeshBuffer& mb = active_scene.meshes[i];
        if (mb.elevator >= 0 && static_cast<size_t>(mb.elevator) < active_scene.elevators.size()) {
            const auto& parts = active_scene.elevators[static_cast<size_t>(mb.elevator)].parts;
            if (mb.elevator_part >= 0 && static_cast<size_t>(mb.elevator_part) < parts.size()) {
                set_matrix(uniforms.model, Mat4::translation(parts[static_cast<size_t>(mb.elevator_part)].offset));
                return true;
            }
        }
        if (mb.barge_door >= 0 && static_cast<size_t>(mb.barge_door) < active_scene.barge_doors.size()) {
            set_matrix(uniforms.model, active_scene.barge_doors[static_cast<size_t>(mb.barge_door)].model_matrix);
            return true;
        }
        set_matrix(uniforms.model, identity);
        return false;
    };

    // ---------------------------------------------------------------------
    // Pass 0: Real-Time Directional Sun Shadow Cascades (2 x 4096x4096 Depth)
    // ---------------------------------------------------------------------
    if (shadow_pass && !scene_hidden) {
        // Renders one cascade into its slice of the shadow map array. `dynamic_casters` adds the moving
        // elevator parts, the barge doors and the enemies; without it only static level geometry is drawn.
        auto render_shadow_cascade = [&](int slice, const Mat4& cascade_vp, float bias_clamp, bool dynamic_casters) {
            impl->set_render_target(impl->shadow_fbo[slice]);
            impl->clear_depth();

            // shadow_vertex projects with uniforms.sun_view_proj.
            float near_vp_saved[16];
            std::memcpy(near_vp_saved, uniforms.sun_view_proj, sizeof(near_vp_saved));
            set_matrix(uniforms.sun_view_proj, cascade_vp);

            set_viewport(static_cast<float>(kSunShadowMapSize), static_cast<float>(kSunShadowMapSize), 0.0f, 1.0f);
            use_program(impl->shadow_program);
            impl->set_depth(Impl::DepthState::Write);
            impl->set_cull(false);
            impl->set_shadow_bias(bias_clamp);
            set_blend(impl->blend_opaque);
            push_uniforms();

            bool sh_prev_moved = false;
            for (size_t i = 0; i < active_scene.meshes.size(); ++i) {
                const auto& mesh = active_scene.meshes[i];
                if (mesh.vertices.empty() || !impl->cached_mesh_buffers[i].vbo) continue;
                if (!dynamic_casters && (mesh.elevator >= 0 || mesh.barge_door >= 0)) continue;
                if (mesh.is_decal) continue;
                bool moved = apply_scene_mesh_model(i);
                if (moved || sh_prev_moved) push_uniforms();
                sh_prev_moved = moved;
                set_scene_vertices(&impl->cached_mesh_buffers[i]);
                if (mesh.sections.empty()) {
                    impl->draw(static_cast<GLsizei>(mesh.vertices.size()), 0);
                    continue;
                }
                for (const Impl::VertexRange& r : impl->cached_shadow_ranges[i]) impl->draw(r.count, r.first);
            }
            set_matrix(uniforms.model, identity);

            if (dynamic_casters && need_enemies) {
                // Only the near cascade has dynamic casters; in_shadow was tested against its matrix (and
                // implies a live, posed enemy).
                for (size_t ei = 0; ei < active_scene.enemies.size(); ++ei) {
                    if (!impl->frame_enemy_draws[ei].in_shadow) continue;
                    const auto& bot = active_scene.enemies[ei];
                    set_matrix(uniforms.model, Mat4::translation(bot.position) * Mat4::rotation_z(bot.yaw_deg * DEG2RAD));
                    push_uniforms();
                    draw_enemy_mesh(ei);
                }
                set_matrix(uniforms.model, identity);
            }

            impl->clear_shadow_bias();
            std::memcpy(uniforms.sun_view_proj, near_vp_saved, sizeof(near_vp_saved));
        };

        // Far cascade: static geometry only, so it stays valid until its coarsely snapped square steps
        // (the camera moved ~kSunShadowFarStep), the sun direction changes or the scene is rebuilt.
        // The main menu has no far cascade: one fixed square frames its miniature city.
        if (!in_main_menu &&
            (!impl->shadow_far_valid || impl->shadow_far_generation != impl->scene_generation ||
             std::memcmp(impl->shadow_far_vp, sun_far_vp.m, sizeof(impl->shadow_far_vp)) != 0)) {
            render_shadow_cascade(kSunShadowFarSlice, sun_far_vp, kSunShadowFarSlopeBiasCap / kSunShadowFarDepthRange,
                                  /*dynamic_casters=*/false);
            std::memcpy(impl->shadow_far_vp, sun_far_vp.m, sizeof(impl->shadow_far_vp));
            impl->shadow_far_generation = impl->scene_generation;
            impl->shadow_far_valid = true;
        }
        // Near cascade (or the menu's fixed square): every caster, every frame.
        render_shadow_cascade(kSunShadowNearSlice, sun_near_vp, kSunShadowSlopeBiasCap / kSunShadowDepthRange,
                              /*dynamic_casters=*/true);
        impl->check_errors("shadow pass");
    }

    // Pass 0b: the dynamic objects' own shadows (mod_shadow.hpp): a depth map each, from its light
    // environment's shadow light, in cells of the shadow map array's third slice.
    impl->mod_shadows.clear();
    impl->viewmodel_built = false;
    if (shadow_pass && !scene_hidden && !in_main_menu && !impl->menu_open) {
        ModShadowView shadow_view;
        shadow_view.position = cam_pos;
        shadow_view.forward = fwd;
        shadow_view.proj_x = proj.m[0];
        shadow_view.proj_y = proj.m[5];
        shadow_view.width = fw;
        shadow_view.height = fh;
        impl->mod_enemy_ready.assign(active_scene.enemies.size(), 0);
        for (size_t ei = 0; need_enemies && ei < active_scene.enemies.size() && ei < impl->frame_enemy_draws.size(); ++ei) {
            impl->mod_enemy_ready[ei] = (impl->frame_enemy_draws[ei].in_view || impl->frame_enemy_draws[ei].in_shadow) ? 1 : 0;
        }
        const bool body_casts = !bink_video_active && impl->anim_system.is_loaded();
        collect_mod_shadows(active_scene, impl->light_envs, shadow_view, body_casts ? &telemetry.position : nullptr,
                            telemetry.yaw_deg, impl->mod_enemy_ready, telemetry.sim_time, impl->mod_shadows);
        if (!impl->mod_shadows.empty()) {
            impl->set_render_target(impl->shadow_fbo[kModShadowSlice]);
            // The whole slice is cleared, whatever viewport the last pass left.
            set_viewport(static_cast<float>(kSunShadowMapSize), static_cast<float>(kSunShadowMapSize), 0.0f, 1.0f);
            impl->clear_depth();
            float sun_vp_saved[16];
            std::memcpy(sun_vp_saved, uniforms.sun_view_proj, sizeof(sun_vp_saved));
            use_program(impl->shadow_program);
            impl->set_depth(Impl::DepthState::Write);
            impl->set_cull(false);
            impl->clear_shadow_bias();
            set_blend(impl->blend_opaque);
            for (const ModShadow& s : impl->mod_shadows) {
                // Clip space is turned over on its way to the window (msl_to_glsl), so a cell's rows
                // count from the top here as they do in the map.
                glViewport(s.cell_x, s.cell_y, s.resolution, s.resolution);
                impl->cur_viewport[0] = -1;  // not what set_viewport() last asked for
                set_matrix(uniforms.sun_view_proj, s.subject_matrix);  // shadow_vertex projects with it
                if (s.kind == ModShadow::Kind::Enemy) {
                    const auto& bot = active_scene.enemies[s.index];
                    set_matrix(uniforms.model, Mat4::translation(bot.position) * Mat4::rotation_z(bot.yaw_deg * DEG2RAD));
                    push_uniforms();
                    draw_enemy_mesh(s.index);
                } else if (s.kind == ModShadow::Kind::Mesh) {
                    const auto& mesh = active_scene.meshes[s.index];
                    if (!impl->cached_mesh_buffers[s.index].vbo) continue;
                    apply_scene_mesh_model(s.index);
                    push_uniforms();
                    set_scene_vertices(&impl->cached_mesh_buffers[s.index]);
                    if (mesh.sections.empty()) {
                        impl->draw(static_cast<GLsizei>(mesh.vertices.size()), 0);
                        continue;
                    }
                    const auto& sflags = impl->cached_section_flags[s.index];
                    for (size_t si = 0; si < mesh.sections.size(); ++si) {
                        const auto& sec = mesh.sections[si];
                        if (!section_in_range(mesh, sec) || (sflags[si] & Impl::kSecShadowCaster) == 0) continue;
                        impl->draw(static_cast<GLsizei>(sec.vertex_count), static_cast<GLint>(sec.first_vertex));
                    }
                } else {
                    // The first-person body, which is posed in the view's own frame (x to the left,
                    // y ahead, z up).
                    impl->build_faith_viewmodel(telemetry);
                    impl->viewmodel_built = true;
                    if (impl->faith_viewmodel_mesh.empty()) continue;
                    Mat4 body;
                    const Vec3 axes[3] = {right * -1.0f, fwd, up};
                    for (int c = 0; c < 3; ++c) {
                        body.m[c * 4 + 0] = axes[c].x;
                        body.m[c * 4 + 1] = axes[c].y;
                        body.m[c * 4 + 2] = axes[c].z;
                    }
                    body.m[12] = cam_pos.x;
                    body.m[13] = cam_pos.y;
                    body.m[14] = cam_pos.z;
                    set_matrix(uniforms.model, body);
                    push_uniforms();
                    draw_scene_vertices(impl->faith_viewmodel_mesh);
                    // The head and the torso that body lacks.
                    player_body_stand_in(cam_pos, telemetry.position, fwd, right, up, impl->head_vertices);
                    set_matrix(uniforms.model, identity);
                    push_uniforms();
                    draw_scene_vertices(impl->head_vertices);
                }
            }
            std::memcpy(uniforms.sun_view_proj, sun_vp_saved, sizeof(sun_vp_saved));
            set_matrix(uniforms.model, identity);
        }
    }

    prof(Impl::kProfShadows);

    // ---------------------------------------------------------------------
    // Pass 1: 3D Scene Geometry & Sky -> HDR Texture
    // ---------------------------------------------------------------------
    if (!scene_hidden) {
        const float scene_clear[4] = {0.65f, 0.82f, 0.98f, 1.0f};
        impl->set_render_target(impl->scene_fbo);
        impl->clear_color(scene_clear);
        impl->clear_depth();
        set_viewport(fw, fh, 0.0f, 1.0f);
        auto bind_shadow_map = [&]() { impl->bind_texture(matbind::kShadowMapTexture, impl->shadow_depth_tex.get()); };
        bind_shadow_map();
        set_blend(impl->blend_opaque);
        impl->set_cull(false);

        // A. Draw Sky Dome (TdDirHaze + Distant City Skyline)
        use_program(impl->sky_program);
        impl->bind_vertices(nullptr, Impl::VertexKind::None);
        impl->set_depth(Impl::DepthState::Disabled);
        push_uniforms();
        impl->draw(3, 0);

        // B. Draw World Meshes (BasePass + Beast Radiosity)
        set_viewport(fw, fh, 0.05f, 1.0f);
        impl->set_depth(Impl::DepthState::Write);

        auto tex_or = [](const UITexture& t, const UITexture& fallback) -> const GpuTexture* {
            return t ? t.get() : fallback.get();
        };
        auto bind_world_char_wep_textures = [&](const std::string& wname, uint32_t arch_id = 0) {
            uint32_t safe_arch = (arch_id < AnimSystem::EnemyArch_Count) ? arch_id : 0;
            const auto& ctex = impl->enemy_gpu_textures[safe_arch];
            const GpuTexture* t_wep_d = tex_or(nullptr, impl->tex_default_white);
            const GpuTexture* t_wep_s = tex_or(nullptr, impl->tex_default_black);
            auto it_w = impl->weapon_gpu_textures.find(wname);
            if (it_w == impl->weapon_gpu_textures.end() && !impl->weapon_gpu_textures.empty()) {
                it_w = impl->weapon_gpu_textures.find("Colt1911");
            }
            if (it_w != impl->weapon_gpu_textures.end()) {
                if (it_w->second.diffuse)  t_wep_d = it_w->second.diffuse.get();
                if (it_w->second.specular) t_wep_s = it_w->second.specular.get();
            }
            impl->bind_textures(0, {
                tex_or(ctex.diffuse ? ctex.diffuse : impl->swat_d_gpu_tex, impl->tex_default_white),        // texture(0) swat_d_tex
                t_wep_d,                                                                                    // texture(1) wep_d_tex
                t_wep_s,                                                                                    // texture(2) wep_s_tex
                tex_or(impl->ammo_d_gpu_tex, impl->tex_default_white),                                      // texture(3) ammo_d_tex
                tex_or(ctex.specular ? ctex.specular : impl->swat_s_gpu_tex, impl->tex_default_black),      // texture(4) swat_s_tex
                tex_or(ctex.normal ? ctex.normal : impl->swat_n_gpu_tex, impl->tex_default_flat_normal),    // texture(5) swat_n_tex
            });
            impl->bind_sampler(0, impl->mat_samplers[0][0]);
        };
        // The legacy world pipeline (meshes and sections without a material shader, enemies, effects).
        auto use_world_pipeline = [&]() {
            use_program(impl->world_program);
            set_blend(impl->blend_opaque);
            impl->set_cull(false);
            bind_world_char_wep_textures("Colt1911");
        };
        use_world_pipeline();

        // Binds mesh i's vertex buffer + frame uniforms (incl. its model matrix).
        auto bind_scene_mesh = [&](size_t i) {
            uniforms.is_runner_vision = active_scene.meshes[i].is_runner_vision ? 1.0f : 0.0f;
            apply_scene_mesh_model(i);
            if (active_scene.meshes[i].dynamic_lit && !in_main_menu) {
                const LightEnvLighting lighting = impl->light_envs.mesh(active_scene, i, telemetry.sim_time);
                set_light_env(&lighting, Vec3(0.0f, 0.0f, 0.0f));
            } else {
                set_light_env(nullptr, Vec3(0.0f, 0.0f, 0.0f));
            }
            set_scene_vertices(&impl->cached_mesh_buffers[i]);
            push_uniforms();
        };
        bool decal_bias = false;  // what is being drawn is a decal buffer (MeshBuffer::is_decal)
        // Switches to a material's pipeline: its fragment shader linked with the shared material vertex stage.
        auto use_material_pipeline = [&](const Impl::Program& ps, const MaterialShader& sh, const SceneMaterial& m, bool cull) {
            use_program(ps);
            switch (sh.blend) {
                case MatBlendMode::Translucent: set_blend(impl->blend_translucent); break;
                case MatBlendMode::Additive: set_blend(impl->blend_additive); break;
                case MatBlendMode::Modulate: set_blend(impl->blend_modulate); break;
                default: set_blend(impl->blend_opaque); break;
            }
            impl->set_cull(cull && !decal_bias);
            impl->set_decal_bias(decal_bias);
            impl->bind_material(m, sh);
        };

        if (!active_scene.meshes.empty()) {
            std::string active_mi_tag = in_main_menu
                ? impl->main_menu.get_chapter(impl->selected_chapter).material_instance_tag
                : "";
            for (char& c : active_mi_tag) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            for (size_t i = 0; i < active_scene.meshes.size(); ++i) {
                const auto& mesh = active_scene.meshes[i];
                if (mesh.vertices.empty() || !impl->cached_mesh_buffers[i].vbo) continue;
                decal_bias = mesh.is_decal;
                bind_scene_mesh(i);
                if (mesh.sections.empty()) {
                    use_world_pipeline();
                    impl->draw(static_cast<GLsizei>(mesh.vertices.size()), 0);
                    continue;
                }
                // Opaque + masked material sections (UE3 base pass). Translucent ones are deferred.
                // Sections that follow each other with the same material and light map (one mesh
                // element batched for several placements) are one draw: the order is kept.
                const std::vector<uint32_t>& order = impl->cached_opaque_order[i];
                for (size_t oi = 0; oi < order.size(); ++oi) {
                    const auto& s = mesh.sections[order[oi]];
                    const MaterialShader* sh = nullptr;
                    const SceneMaterial* m = nullptr;
                    const Impl::Program* ps = impl->section_shader(s, &sh, &m);
                    GLsizei run_count = static_cast<GLsizei>(s.vertex_count);
                    while (oi + 1 < order.size()) {
                        const auto& n = mesh.sections[order[oi + 1]];
                        if (n.material != s.material || n.lightmap != s.lightmap ||
                            static_cast<GLint>(s.first_vertex) + run_count != static_cast<GLint>(n.first_vertex)) {
                            break;
                        }
                        run_count += static_cast<GLsizei>(n.vertex_count);
                        ++oi;
                    }
                    if (ps) {
                        use_material_pipeline(*ps, *sh, *m, impl->mat_cull_enabled && !sh->two_sided && !in_main_menu);
                        impl->bind_lightmap(s.lightmap);
                        if (in_main_menu && sh->num_uniforms > 0 && !active_mi_tag.empty()) {
                            std::string mname = m->name;
                            for (char& c : mname) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                            if (mname.find(active_mi_tag) != std::string::npos) {
                                std::vector<std::array<float, 4>> dyn_u = m->uniforms;
                                dyn_u.resize(static_cast<size_t>(sh->num_uniforms), {0.0f, 0.0f, 0.0f, 0.0f});
                                dyn_u[0][0] = 1.0f; // UE3 MaterialInstanceConstant scalar parameter 'Selected' = 1.0
                                impl->set_material_uniforms(dyn_u, sh->num_uniforms);
                            }
                        }
                    } else {
                        use_world_pipeline();
                    }
                    impl->draw(run_count, static_cast<GLint>(s.first_vertex));
                }
            }
            set_matrix(uniforms.model, identity);
            uniforms.is_runner_vision = 0.0f;
        }

        // B2. Render 3D Articulated KrugerSec / CPF SWAT Enemies & 3D Weapons/Tracers (only during gameplay)
        // (the material sections above leave their own pipeline/depth state bound)
        decal_bias = false;
        impl->set_decal_bias(false);
        use_world_pipeline();
        impl->set_depth(Impl::DepthState::Write);
        bind_shadow_map();
        if (!impl->menu_open && need_enemies) {
            for (size_t ei = 0; ei < active_scene.enemies.size(); ++ei) {
                if (!impl->frame_enemy_draws[ei].in_view) continue;
                const auto& bot = active_scene.enemies[ei];
                bind_world_char_wep_textures(bot.disarm_weapon.empty() ? bot.weapon_name : bot.disarm_weapon, impl->frame_enemy_draws[ei].archetype_id);
                set_matrix(uniforms.model, Mat4::translation(bot.position) * Mat4::rotation_z(bot.yaw_deg * DEG2RAD));
                uniforms.is_runner_vision = 0.0f;
                uniforms.actor_tint = Float3(1.0f, 1.0f, 1.0f);
                push_uniforms();
                set_light_env(&impl->light_envs.enemy(active_scene, ei, telemetry.sim_time), Vec3(0.0f, 0.0f, 0.0f));
                draw_enemy_mesh(ei);
            }
            set_light_env(nullptr, Vec3(0.0f, 0.0f, 0.0f));
            set_matrix(uniforms.model, identity);
        }

        // B2a. Render 3D Dropped Weapons on Ground & 3D Bullet Tracers / Impact Sparks
        if (!impl->menu_open && impl->anim_system.is_loaded() &&
            (!active_scene.dropped_weapons.empty() || !active_scene.active_tracers.empty())) {
            std::string pickup_wname = !active_scene.dropped_weapons.empty()
                                           ? active_scene.dropped_weapons.front().weapon_name
                                           : "Colt1911";
            bind_world_char_wep_textures(pickup_wname);
            std::vector<Vertex> combat_fx_verts;
            std::vector<Vertex> combat_rv_verts;
            impl->anim_system.evaluate_combat_world_fx(active_scene, telemetry.sim_time, combat_fx_verts, combat_rv_verts);
            if (!combat_fx_verts.empty()) {
                set_matrix(uniforms.model, identity);
                uniforms.is_runner_vision = 0.0f;
                uniforms.actor_tint = Float3(1.0f, 1.0f, 1.0f);
                push_uniforms();
                draw_scene_vertices(combat_fx_verts);
            }
            if (!combat_rv_verts.empty()) {
                set_matrix(uniforms.model, identity);
                uniforms.is_runner_vision = 1.0f;
                uniforms.actor_tint = Float3(1.0f, 1.0f, 1.0f);
                push_uniforms();
                draw_scene_vertices(combat_rv_verts);
                uniforms.is_runner_vision = 0.0f;
            }
        }

        prof(Impl::kProfScene);
        impl->check_errors("scene pass");

        // The dynamic objects' shadows multiply the lit scene, before fog and translucency (mod_shadow.hpp).
        if (!impl->mod_shadows.empty()) {
            ModShadowUniformsGPU shadow_constants;
            fill_mod_shadow_uniforms(impl->mod_shadows, shadow_constants);
            impl->update_constants(impl->cb_post, &shadow_constants, sizeof(shadow_constants));
            impl->bind_uniform_block(block_binding(false, 1), impl->cb_post);
            impl->set_render_target(impl->scene_color_fbo);  // the depth buffer is read, not tested
            impl->unbind_shader_resources();
            impl->bind_texture(1, impl->depth_tex.get());
            bind_shadow_map();
            impl->set_depth(Impl::DepthState::Disabled);
            impl->set_cull(false);
            set_blend(impl->blend_modulate);
            set_viewport(fw, fh, 0.0f, 1.0f);
            use_program(impl->mod_shadow_program);
            impl->bind_vertices(nullptr, Impl::VertexKind::None);
            push_uniforms();
            impl->draw(3, 0);
            impl->unbind_shader_resources();
            set_blend(impl->blend_opaque);
            impl->set_render_target(impl->scene_fbo);
            set_viewport(fw, fh, 0.05f, 1.0f);
            bind_shadow_map();
            impl->bind_uniform_block(block_binding(false, 1), impl->cb_material);
        }

        // Height fog over the opaque scene, before anything translucent is drawn on it
        // (HeightFogPixelShader.usf): scene * scattering + fog, read off the depth buffer.
        if (!active_scene.height_fog.empty()) {
            PostUniformsGPU fog_constants{};
            fill_post_uniforms(active_scene, view_post, cam_pos, 0.0f, false, fog_constants);
            auto push_post = [&]() { impl->update_constants(impl->cb_post, &fog_constants, sizeof(fog_constants)); };
            impl->bind_uniform_block(block_binding(false, 1), impl->cb_post);
            impl->set_render_target(impl->scene_color_fbo);  // the depth buffer is read, not tested
            impl->unbind_shader_resources();
            impl->bind_texture(1, impl->depth_tex.get());
            impl->set_depth(Impl::DepthState::Disabled);
            impl->set_cull(false);
            set_blend(impl->blend_fog);
            set_viewport(fw, fh, 0.0f, 1.0f);
            use_program(impl->fog_program);
            impl->bind_vertices(nullptr, Impl::VertexKind::None);
            push_uniforms();
            push_post();
            impl->draw(3, 0);
            impl->unbind_shader_resources();
            set_blend(impl->blend_opaque);
            impl->set_render_target(impl->scene_fbo);
            set_viewport(fw, fh, 0.05f, 1.0f);
            bind_shadow_map();
            impl->bind_uniform_block(block_binding(false, 1), impl->cb_material);
        }

        // B3. Translucent / additive / modulated materials (UE3 translucency pass): drawn after
        // all opaque geometry, depth-tested without depth writes. Materials that read the scene
        // (SceneTexture, DestColor, DepthBiasedAlpha) sample copies of the opaque scene.
        if (has_translucent) {
            if (needs_scene_copies) {
                glBindFramebuffer(GL_READ_FRAMEBUFFER, impl->scene_fbo);
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, impl->scene_copy_fbo);
                glBlitFramebuffer(0, 0, impl->width, impl->height, 0, 0, impl->width, impl->height,
                                  GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST);
                impl->set_render_target(impl->scene_fbo);
            }
            impl->set_depth(Impl::DepthState::TestOnly);
            // The decals first: they belong to the opaque surfaces they lie on.
            for (int decals = 1; decals >= 0; --decals)
            for (size_t i = 0; i < active_scene.meshes.size(); ++i) {
                const auto& mesh = active_scene.meshes[i];
                if (mesh.vertices.empty() || mesh.sections.empty() || !impl->cached_mesh_buffers[i].vbo) continue;
                if (mesh.is_decal != (decals == 1)) continue;
                decal_bias = mesh.is_decal;
                bool mesh_bound = false;
                for (const auto& s : mesh.sections) {
                    if (!section_in_range(mesh, s)) continue;
                    const MaterialShader* sh = nullptr;
                    const SceneMaterial* m = nullptr;
                    const Impl::Program* ps = impl->section_shader(s, &sh, &m);
                    if (!ps || !mat_blend_is_translucent(sh->blend)) continue;
                    if (!mesh_bound) {
                        bind_scene_mesh(i);
                        mesh_bound = true;
                    }
                    use_material_pipeline(*ps, *sh, *m, impl->mat_cull_enabled && !sh->two_sided);
                    impl->bind_lightmap(s.lightmap);
                    impl->draw(static_cast<GLsizei>(s.vertex_count), static_cast<GLint>(s.first_vertex));
                }
            }
            // The particle systems' sprites, the far systems first.
            if (!impl->particles.batches().empty()) {
                decal_bias = false;
                set_matrix(uniforms.model, identity);
                uniforms.is_runner_vision = 0.0f;
                push_uniforms();
                set_light_env(nullptr, Vec3(0.0f, 0.0f, 0.0f));
                for (const ParticleBatch& b : impl->particles.batches()) {
                    MeshSection section;
                    section.material = b.material;
                    const MaterialShader* sh = nullptr;
                    const SceneMaterial* m = nullptr;
                    const Impl::Program* ps = impl->section_shader(section, &sh, &m);
                    if (!ps) continue;
                    decal_bias = b.decal;  // a bullet hole lies in its surface
                    use_material_pipeline(*ps, *sh, *m, false);
                    impl->bind_lightmap(-1);
                    draw_scene_vertices(b.vertices);
                }
            }
            set_matrix(uniforms.model, identity);
            set_blend(impl->blend_opaque);
            impl->set_cull(false);
        }

        decal_bias = false;
        impl->set_decal_bias(false);
        // B4. Lens flares (lens_flare.hpp): additive quads over the world, under the first-person mesh.
        if (!in_main_menu && !bink_video_active && impl->material_shaders && !active_scene.lens_flares.empty()) {
            LensFlareView flare_view;
            flare_view.position = cam_pos;
            flare_view.forward = fwd;
            flare_view.right = right;
            flare_view.up = up;
            flare_view.proj_x = proj.m[0];
            flare_view.proj_y = proj.m[5];
            flare_view.width = fw;
            flare_view.height = fh;
            flare_view.near_plane = near_plane;
            build_lens_flare_quads(active_scene, flare_view, impl->flare_quads);
            if (!impl->flare_quads.empty()) {
                const FrameUniformsGPU kept = uniforms;
                std::memcpy(&uniforms.view_proj, identity.m, sizeof(float) * 16);
                std::memcpy(&uniforms.model, identity.m, sizeof(float) * 16);
                push_uniforms();
                set_light_env(nullptr, Vec3(0.0f, 0.0f, 0.0f));
                impl->set_depth(Impl::DepthState::TestOnly);
                for (const LensFlareQuad& q : impl->flare_quads) {
                    MeshSection section;
                    section.material = q.material;
                    const MaterialShader* sh = nullptr;
                    const SceneMaterial* m = nullptr;
                    const Impl::Program* ps = impl->section_shader(section, &sh, &m);
                    if (!ps) continue;
                    use_material_pipeline(*ps, *sh, *m, false);
                    std::memcpy(impl->scene_constants.flare_color, q.color, sizeof(q.color));
                    const float inputs[4] = {q.radial_distance, q.source_distance, q.occlusion, q.intensity};
                    std::memcpy(impl->scene_constants.flare_inputs, inputs, sizeof(inputs));
                    impl->scene_constants.flare_ray[0] = q.ray_distance;
                    impl->update_constants(impl->cb_scene, &impl->scene_constants, sizeof(impl->scene_constants));
                    lens_flare_vertices(q, impl->flare_vertices);
                    draw_scene_vertices(impl->flare_vertices);
                }
                uniforms = kept;
                push_uniforms();
                set_blend(impl->blend_opaque);
                impl->set_cull(false);
            }
        }

        prof(Impl::kProfTranslucent);

        // C. Draw First-Person Faith Viewmodel (CH_Faith_1P in DPG_Foreground depth range [0.0, 0.05])
        if (!impl->menu_open && !bink_video_active) {
            if (!impl->viewmodel_built) impl->build_faith_viewmodel(telemetry);
            if (!impl->faith_viewmodel_mesh.empty()) {
                set_viewport(fw, fh, 0.0f, 0.05f);
                use_program(impl->viewmodel_program);
                impl->set_depth(Impl::DepthState::Write);
                set_matrix(uniforms.view_proj, vm_vp);
                set_matrix(uniforms.model, identity);
                uniforms.camera_pos = Float3(0.0f, 0.0f, 0.0f);
                // Its lights are given around the eye, as the mesh is.
                set_light_env(&impl->light_envs.first_person(active_scene, telemetry.position, telemetry.sim_time), cam_pos);
                uniforms.is_runner_vision = 0.0f;
                uniforms.actor_tint = Float3(1.0f, 1.0f, 1.0f);

                const GpuTexture* t_wep_d = tex_or(nullptr, impl->tex_default_white);
                const GpuTexture* t_wep_s = tex_or(nullptr, impl->tex_default_black);
                const GpuTexture* t_wep_n = tex_or(nullptr, impl->tex_default_flat_normal);
                const GpuTexture* t_wep_m = tex_or(nullptr, impl->tex_default_white);
                auto it_w = impl->weapon_gpu_textures.find(telemetry.weapon.name);
                if (it_w == impl->weapon_gpu_textures.end() && !impl->weapon_gpu_textures.empty()) {
                    it_w = impl->weapon_gpu_textures.find("Colt1911");
                }
                if (it_w != impl->weapon_gpu_textures.end()) {
                    if (it_w->second.diffuse)  t_wep_d = it_w->second.diffuse.get();
                    if (it_w->second.specular) t_wep_s = it_w->second.specular.get();
                    if (it_w->second.normal)   t_wep_n = it_w->second.normal.get();
                    if (it_w->second.mask)     t_wep_m = it_w->second.mask.get();
                }
                impl->bind_textures(0, {
                    tex_or(impl->vm_skin_gpu_tex, impl->tex_default_white),   // texture(0) vm_skin_tex
                    tex_or(impl->vm_glove_gpu_tex, impl->tex_default_white),  // texture(1) vm_glove_tex
                    tex_or(impl->vm_lower_gpu_tex, impl->tex_default_white),  // texture(2) vm_lower_tex
                    t_wep_d,                                                  // texture(3) vm_wep_d_tex
                    t_wep_s,                                                  // texture(4) vm_wep_s_tex
                    t_wep_n,                                                  // texture(5) vm_wep_n_tex
                    tex_or(impl->ammo_d_gpu_tex, impl->tex_default_white),    // texture(6) vm_ammo_tex
                    t_wep_m,                                                  // texture(7) vm_wep_m_tex
                });
                impl->bind_sampler(0, impl->mat_samplers[0][0]);

                push_uniforms();
                draw_scene_vertices(impl->faith_viewmodel_mesh);
                uniforms.camera_pos = cam_pos;
            }
        }
        impl->check_errors("translucent + viewmodel");
    }

    // ---------------------------------------------------------------------
    // Pass 2: Post-Processing, SSAO & Tone Mapping (HDR -> Final Output)
    // ---------------------------------------------------------------------
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    impl->set_render_target(impl->offscreen_fbo);
    impl->clear_color(black);
    impl->unbind_shader_resources();
    set_viewport(fw, fh, 0.0f, 1.0f);
    impl->set_depth(Impl::DepthState::Disabled);
    impl->set_cull(false);
    set_blend(impl->blend_opaque);
    impl->bind_vertices(nullptr, Impl::VertexKind::None);
    if (!scene_hidden) {
        // The chain, in the game's order: haze, bloom (gather, blur across, blur down), exposure,
        // then the blend of the bloom with the scene and the tone mapping, in one pass.
        PostUniformsGPU post{};
        // When the game asks (a level opening), the exposure buffers go back to the game's starting
        // value and adapt from there. Otherwise the exposure follows the view in time, and settles at
        // once for a still: the same moment again, a first picture of a level nobody asked to open, a
        // gap no eye followed, or a view that is suddenly somewhere else (the oracle renders now and
        // then, from wherever its stage put the player).
        const bool opening = telemetry.exposure_reset;
        if (opening) {
            const float start[4] = {kExposureStart, kExposureStart, kExposureStart, kExposureStart};
            for (auto& e : impl->exposure) {
                impl->set_render_target(e.fbo);
                impl->clear_color(start);
            }
        }
        const bool new_level = impl->exposure_map != active_scene.map_name;
        const bool fresh = impl->exposure_sim_time < 0.0f;  // the buffers were just made (a resize)
        const float post_dt = (opening || new_level || fresh) ? 0.0f : telemetry.sim_time - impl->exposure_sim_time;
        const bool view_jumped = (cam_pos - impl->exposure_view_pos).length_sq() > 300.0f * 300.0f;
        const bool still = !opening && (new_level || fresh || post_dt == 0.0f || post_dt >= 0.5f || view_jumped);
        impl->exposure_view_pos = cam_pos;
        // TdMotionBlur's amount, from how the camera itself moved since the last frame.
        const float motion_amount = impl->motion_blur.update(cam_pos, fwd, post_dt, still || opening);
        fill_post_uniforms(active_scene, view_post, cam_pos, (post_dt > 0.0f && post_dt < 0.5f) ? post_dt : 0.0f, still, post);
        apply_hud_damage_uniforms(telemetry, post);
        post.overlay[3] = impl->frontend_overlay ? impl->frontend_saturation : 0.0f;
        post.fade[0] = telemetry.fade_color.x;
        post.fade[1] = telemetry.fade_color.y;
        post.fade[2] = telemetry.fade_color.z;
        post.fade[3] = telemetry.fade_amount;
        post.motion[0] = motion_amount;
        impl->exposure_sim_time = telemetry.sim_time;
        impl->exposure_map = active_scene.map_name;
        auto push_post = [&]() { impl->update_constants(impl->cb_post, &post, sizeof(post)); };
        auto post_pass = [&](const Impl::Program& program, const Impl::ColorTarget& target, std::initializer_list<const GpuTexture*> inputs) {
            impl->set_render_target(target.fbo);
            set_viewport(static_cast<float>(target.w), static_cast<float>(target.h), 0.0f, 1.0f);
            impl->unbind_shader_resources();
            impl->bind_textures(0, inputs);
            use_program(program);
            push_post();
            impl->draw(3, 0);
        };
        impl->bind_uniform_block(block_binding(false, 1), impl->cb_post);
        impl->bind_sampler(0, impl->linear_sampler);
        push_uniforms();

        post.texel[0] = 1.0f / fw;
        post.texel[1] = 1.0f / fh;
        post_pass(impl->haze_program, impl->scene_hazed, {impl->scene_hdr_tex.get(), impl->depth_tex.get()});
        post_pass(impl->bloom_gather_program, impl->filter_a, {impl->scene_hazed.tex.get(), impl->depth_tex.get()});
        fill_filter_taps(impl->width, impl->filter_a.w, impl->filter_a.h, /*horizontal=*/true, post);
        post_pass(impl->filter_program, impl->filter_b, {impl->filter_a.tex.get()});
        fill_filter_taps(impl->width, impl->filter_a.w, impl->filter_a.h, /*horizontal=*/false, post);
        post_pass(impl->filter_program, impl->filter_a, {impl->filter_b.tex.get()});

        // The metering: the scene with its bloom into 512 x 512, then down to one texel.
        post.texel[0] = 1.0f / fw;
        post.texel[1] = 1.0f / fh;
        post.texel[2] = static_cast<float>(meter_taps(impl->width, kMeterSizes[0]));
        post_pass(impl->meter_scene_program, impl->meter[0],
                  {impl->scene_hazed.tex.get(), impl->depth_tex.get(), impl->filter_a.tex.get()});
        for (int i = 1; i < kMeterSteps; ++i) {
            post.texel[0] = post.texel[1] = 1.0f / static_cast<float>(kMeterSizes[i - 1]);
            post.texel[2] = static_cast<float>(meter_taps(kMeterSizes[i - 1], kMeterSizes[i]));
            post_pass(impl->meter_program, impl->meter[i], {impl->meter[i - 1].tex.get()});
        }
        post.texel[0] = 1.0f / fw;
        post.texel[1] = 1.0f / fh;
        const int previous = impl->exposure_current;
        impl->exposure_current = 1 - previous;
        post_pass(impl->exposure_program, impl->exposure[impl->exposure_current],
                  {impl->meter[kMeterSteps - 1].tex.get(), impl->exposure[previous].tex.get()});

        // A material effect of the chain: its material over the whole target, through the material
        // vertex stage with nothing to transform, reading `source` as the scene colour.
        auto effect_pass = [&](const PostEffectInfo& fx, const ScreenEffect* state, const GpuTexture* source,
                               const Impl::ColorTarget& target) -> bool {
            MeshSection section;
            section.material = fx.material;
            const MaterialShader* sh = nullptr;
            const SceneMaterial* m = nullptr;
            const Impl::Program* ps = impl->section_shader(section, &sh, &m);
            if (!ps) return false;
            impl->set_render_target(target.fbo);
            set_viewport(static_cast<float>(target.w), static_cast<float>(target.h), 0.0f, 1.0f);
            impl->unbind_shader_resources();
            use_program(*ps);
            set_blend(impl->blend_opaque);
            impl->set_cull(false);
            impl->bind_uniform_block(block_binding(false, 1), impl->cb_material);
            impl->bind_material(*m, *sh);
            impl->bind_texture(matbind::kSceneColorTexture, source);
            impl->bind_texture(matbind::kSceneDepthTexture, impl->depth_tex.get());
            impl->bind_sampler(matbind::kSceneSampler, impl->scene_copy_sampler);
            if (sh->num_uniforms > 0 && state && !state->params.empty()) {
                std::vector<std::array<float, 4>> values = m->uniforms;
                values.resize(static_cast<size_t>(sh->num_uniforms), {0.0f, 0.0f, 0.0f, 0.0f});
                for (const auto& [name, value] : state->params) {
                    const int at = m->uniform_index(name);
                    if (at >= 0 && static_cast<size_t>(at) < values.size()) values[static_cast<size_t>(at)] = value;
                }
                impl->set_material_uniforms(values, sh->num_uniforms);
            }
            const FrameUniformsGPU kept = uniforms;
            std::memcpy(&uniforms.view_proj, identity.m, sizeof(float) * 16);
            std::memcpy(&uniforms.model, identity.m, sizeof(float) * 16);
            push_uniforms();
            draw_scene_vertices(screen_quad_vertices());
            uniforms = kept;
            push_uniforms();
            // back to the chain's own passes
            impl->bind_uniform_block(block_binding(false, 1), impl->cb_post);
            impl->bind_sampler(0, impl->linear_sampler);
            return true;
        };
        const auto effect_state = [&](const PostEffectInfo& fx) -> const ScreenEffect* {
            for (const ScreenEffect& e : telemetry.screen_effects) {
                if (e.name == fx.name) return &e;
            }
            return nullptr;
        };

        // The effects that stand before the tone mapper work on the scene itself.
        const GpuTexture* scene_for_tone = impl->scene_hazed.tex.get();
        for (const PostEffectInfo& fx : active_scene.post_effects) {
            if (fx.after_tone_mapping) continue;
            const ScreenEffect* state = effect_state(fx);
            if (!state) continue;
            // one such effect at a time is all the game ever shows; a second would need a third buffer
            if (scene_for_tone != impl->scene_hazed.tex.get()) break;
            if (effect_pass(fx, state, scene_for_tone, impl->scene_effect)) scene_for_tone = impl->scene_effect.tex.get();
        }

        // The tone-mapped picture, then the passes that work on it in turn; the last writes the frame.
        post_pass(impl->tonemap_program, impl->picture[0],
                  {scene_for_tone, impl->depth_tex.get(), impl->filter_a.tex.get(),
                   impl->exposure[impl->exposure_current].tex.get()});
        int picture_now = 0;
        for (const PostEffectInfo& fx : active_scene.post_effects) {
            if (!fx.after_tone_mapping) continue;
            const ScreenEffect* state = effect_state(fx);
            if (!state) continue;
            if (effect_pass(fx, state, impl->picture[picture_now].tex.get(), impl->picture[1 - picture_now])) {
                picture_now = 1 - picture_now;
            }
        }

        impl->set_render_target(impl->offscreen_fbo);
        set_viewport(fw, fh, 0.0f, 1.0f);
        impl->unbind_shader_resources();
        impl->bind_textures(0, {impl->picture[picture_now].tex.get()});
        use_program(impl->finish_program);
        push_post();
        impl->draw(3, 0);
        impl->bind_uniform_block(block_binding(false, 1), impl->cb_material);
        impl->check_errors("post chain");
    }

    // ---------------------------------------------------------------------
    // Pass 3: 2D HUD, Cutscene Video/Letterbox Overlay & Frontend UI
    // ---------------------------------------------------------------------
    impl->unbind_shader_resources();
    set_blend(impl->blend_ui);
    const float screen_size[4] = {fw, fh, 0.0f, 0.0f};
    impl->update_constants(impl->cb_screen, screen_size, sizeof(screen_size));
    impl->bind_uniform_block(block_binding(true, 1), impl->cb_screen);
    impl->bind_sampler(0, impl->linear_sampler);

    auto draw_hud_vertices = [&](const std::vector<HUDVertex>& verts) {
        if (verts.empty()) return;
        use_program(impl->hud_program);
        Impl::GpuBuffer* buffer = impl->acquire_dynamic_vertex_buffer(verts.data(), verts.size() * sizeof(HUDVertex));
        impl->bind_vertices(buffer, Impl::VertexKind::Hud);
        impl->draw(static_cast<GLsizei>(verts.size()), 0);
    };
    auto draw_ui_tex_vertices = [&](const UITexture& tex, const std::vector<UITexVertex>& verts) {
        if (!tex || verts.empty()) return;
        use_program(impl->ui_tex_program);
        impl->bind_texture(0, tex.get());
        Impl::GpuBuffer* buffer = impl->acquire_dynamic_vertex_buffer(verts.data(), verts.size() * sizeof(UITexVertex));
        impl->bind_vertices(buffer, Impl::VertexKind::UiTex);
        impl->draw(static_cast<GLsizei>(verts.size()), 0);
    };
    // A full-screen picture (a front end frame, a Bink frame) aspect-fitted over an optional black backdrop.
    auto draw_fitted_picture = [&](const UITexture& tex, int src_w, int src_h, bool backdrop = true) {
        if (backdrop) {
            std::vector<HUDVertex> black_bg;
            impl->draw_ui_quad(black_bg, 0.0f, 0.0f, fw, fh, ui_color(0.0f, 0.0f, 0.0f, 1.0f));
            draw_hud_vertices(black_bg);
        }

        float src_aspect = float(src_w) / float(src_h);
        float scr_aspect = fw / fh;
        float draw_w = fw, draw_h = fh, draw_x = 0.0f, draw_y = 0.0f;
        if (scr_aspect > src_aspect) {
            draw_w = fh * src_aspect;
            draw_x = (fw - draw_w) * 0.5f;
        } else {
            draw_h = fw / src_aspect;
            draw_y = (fh - draw_h) * 0.5f;
        }
        UIColor white = ui_color(1.0f, 1.0f, 1.0f, 1.0f);
        UITexVertex v0{{draw_x, draw_y}, {0.0f, 0.0f}, white};
        UITexVertex v1{{draw_x + draw_w, draw_y}, {1.0f, 0.0f}, white};
        UITexVertex v2{{draw_x + draw_w, draw_y + draw_h}, {1.0f, 1.0f}, white};
        UITexVertex v3{{draw_x, draw_y + draw_h}, {0.0f, 1.0f}, white};
        draw_ui_tex_vertices(tex, {v0, v1, v2, v0, v2, v3});
    };

    if (frontend_active) {
        // The front end: one CPU-rendered frame over everything, the same way a Bink frame is shown.
        const int src_w = impl->frontend_w;
        const int src_h = impl->frontend_h;
        if (!impl->frontend_tex || impl->frontend_tex->width != src_w || impl->frontend_tex->height != src_h) {
            // Not an sRGB texture: the frame is display values already, and this pass writes
            // straight to an RGBA8 target, as the other UI textures do.
            impl->frontend_tex = impl->make_dynamic_texture(src_w, src_h, /*srgb=*/false);
        }
        if (impl->frontend_tex) {
            impl->write_dynamic_texture(impl->frontend_tex, impl->frontend_rgba);
            draw_fitted_picture(impl->frontend_tex, src_w, src_h, !impl->frontend_overlay);
        }
    } else if (impl->menu_open) {
        std::vector<HUDVertex> bg_verts;
        std::vector<Impl::UITextureBatch> tex_batches;
        std::vector<HUDVertex> fg_verts;
        impl->draw_main_menu_ui(bg_verts, tex_batches, fg_verts, telemetry);

        draw_hud_vertices(bg_verts);
        for (const auto& batch : tex_batches) draw_ui_tex_vertices(batch.tex, batch.verts);
        draw_hud_vertices(fg_verts);
    } else if (cutscene_active) {
        const CutscenePlayer* cp = impl->cutscene_player;
        const float w = fw;
        const float h = fh;

        // 1. If playing a Bink (.bik) video movie, upload decoded RGBA frame and draw full-screen 16:9 quad
        if (cp->get_mode() == ECutsceneMode::BinkVideo) {
            int vw = cp->get_video_width();
            int vh = cp->get_video_height();
            const auto& rgba = cp->get_rgba_frame();
            if (vw > 0 && vh > 0 && rgba.size() == static_cast<size_t>(vw * vh * 4)) {
                if (!impl->bink_video_tex || impl->bink_video_tex->width != vw || impl->bink_video_tex->height != vh) {
                    impl->bink_video_tex = impl->make_dynamic_texture(vw, vh, /*srgb=*/true);
                    impl->bink_uploaded_serial = 0;
                }
                if (impl->bink_video_tex && impl->bink_uploaded_serial != cp->get_frame_serial()) {
                    impl->write_dynamic_texture(impl->bink_video_tex, rgba.data());
                    impl->bink_uploaded_serial = cp->get_frame_serial();
                }
                // Draw solid black backdrop + letterboxed 16:9 Bink video frame
                if (impl->bink_video_tex) draw_fitted_picture(impl->bink_video_tex, vw, vh);
            }
        }

        // 2. Cinema Letterbox Bars, Cutscene Progress & Synchronized Subtitles
        std::vector<HUDVertex> cs_hud;
        float lb = cp->get_letterbox_amount();
        float bar_h = ((cp->get_mode() == ECutsceneMode::InEngineMatinee) ? 64.0f : 44.0f) * lb;
        if (bar_h > 1.0f) {
            impl->draw_ui_quad(cs_hud, 0.0f, 0.0f, w, bar_h, ui_color(0.0f, 0.0f, 0.0f, 0.88f));
            impl->draw_ui_quad(cs_hud, 0.0f, h - bar_h, w, bar_h, ui_color(0.0f, 0.0f, 0.0f, 0.88f));
        }

        if (cp->is_level_intro()) {
            // A level's own intro carries what retail shows over it: the skip prompt, top left.
            impl->draw_ui_text(cs_hud, "Press SPACE to skip", w * 0.074f, h * 0.105f, 1.9f,
                               ui_color(0.86f, 0.88f, 0.90f, 0.85f));
        } else {
            // Top-right Skip / Next Cutscene controls + progress bar
            std::string ctrl_str = "[SPACE / ENTER] SKIP CUTSCENE   |   [C] NEXT CUTSCENE";
            impl->draw_ui_text(cs_hud, ctrl_str, w - 535.0f, 14.0f, 1.55f, ui_color(0.88f, 0.90f, 0.94f, 0.88f));
            float prog = (cp->get_duration() > 0.0f)
                             ? std::clamp(cp->get_current_time() / cp->get_duration(), 0.0f, 1.0f)
                             : 0.0f;
            impl->draw_ui_quad(cs_hud, 28.0f, 16.0f, 220.0f, 6.0f, ui_color(0.18f, 0.20f, 0.24f, 0.75f));
            impl->draw_ui_quad(cs_hud, 28.0f, 16.0f, 220.0f * prog, 6.0f, ui_color(0.902f, 0.078f, 0.078f, 0.95f));
            impl->draw_ui_text(cs_hud, "CUTSCENE: " + cp->get_movie_name(), 28.0f, 26.0f, 1.5f,
                               ui_color(0.85f, 0.88f, 0.92f, 0.85f));
        }

        // Synchronized localized dialogue subtitle
        const std::string& sub = cp->get_active_subtitle();
        if (!sub.empty()) {
            // Split long subtitle lines across 2 lines if > 82 chars
            std::string line1 = sub;
            std::string line2;
            if (sub.size() > 82) {
                size_t split = sub.rfind(' ', 82);
                if (split != std::string::npos) {
                    line1 = sub.substr(0, split);
                    line2 = sub.substr(split + 1);
                }
            }
            float max_chars = static_cast<float>(std::max(line1.size(), line2.size()));
            float box_w = std::min(w - 80.0f, max_chars * 10.6f + 40.0f);
            float box_h = line2.empty() ? 30.0f : 52.0f;
            float box_x = (w - box_w) * 0.5f;
            float box_y = h - std::max(bar_h + box_h + 10.0f, 56.0f);
            impl->draw_ui_quad(cs_hud, box_x, box_y, box_w, box_h, ui_color(0.03f, 0.05f, 0.08f, 0.82f));
            impl->draw_ui_quad(cs_hud, box_x, box_y, 3.0f, box_h, ui_color(0.902f, 0.078f, 0.078f, 0.95f));
            impl->draw_ui_text(cs_hud, line1, box_x + 18.0f, box_y + 8.0f, 1.75f, ui_color(0.99f, 0.99f, 1.0f, 1.0f));
            if (!line2.empty()) {
                impl->draw_ui_text(cs_hud, line2, box_x + 18.0f, box_y + 29.0f, 1.75f, ui_color(0.99f, 0.99f, 1.0f, 1.0f));
            }
        }
        draw_hud_vertices(cs_hud);
    } else {
        std::vector<HUDVertex> hud_verts;
        impl->draw_hud(hud_verts, scene, telemetry);
        draw_hud_vertices(hud_verts);
    }

    // Leave nothing bound that the next frame renders to.
    impl->unbind_shader_resources();
    impl->bind_uniform_block(block_binding(true, 1), impl->cb_frame);
    impl->check_errors("overlays");

    prof(Impl::kProfOverlays);
    impl->prof_gpu_end();
    if (!impl->headless) {
        // The off-screen frame to the window. Its rows run top-down (the vertex shaders' y flip), the
        // window's bottom-up, so the blit inverts y.
        int dw = impl->width;
        int dh = impl->height;
        SDL_GL_GetDrawableSize(impl->window, &dw, &dh);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, impl->offscreen_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, impl->width, impl->height, 0, dh, dw, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        SDL_GL_SwapWindow(impl->window);
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glFlush();
    }
    impl->check_errors("present");

    prof(Impl::kProfPresent);
    if (impl->profile && ++impl->prof_frames == 120) {
        std::fprintf(stderr, "[OpenGLRenderer] 120 frames, ms per frame:");
        for (int i = 0; i < Impl::kProfPhases; ++i) {
            std::fprintf(stderr, " %s %.2f", Impl::kProfNames[i], impl->prof_seconds[i] / 120.0 * 1000.0);
            impl->prof_seconds[i] = 0.0;
        }
        const double wall_ms = impl->prof_wall_seconds / 120.0 * 1000.0;
        std::fprintf(stderr, "; gpu %.2f; frame %.2f (%.0f fps); %llu draws, %.1f of %zu enemies posed\n",
                     impl->prof_gpu_frames > 0 ? impl->prof_gpu_seconds / impl->prof_gpu_frames * 1000.0 : 0.0, wall_ms,
                     wall_ms > 0.0 ? 1000.0 / wall_ms : 0.0, static_cast<unsigned long long>(impl->prof_draws / 120),
                     static_cast<double>(impl->prof_posed.exchange(0)) / 120.0, active_scene.enemies.size());
        impl->prof_frames = 0;
        impl->prof_draws = 0;
        impl->prof_gpu_seconds = 0.0;
        impl->prof_gpu_frames = 0;
        impl->prof_wall_seconds = 0.0;
    }

    impl->frame_index++;
}

bool OpenGLRenderer::save_screenshot_ppm(const std::string& path) {
    std::vector<uint8_t> pixels;
    if (!impl_->read_back(pixels)) return false;
    const int w = impl_->width;
    const int h = impl_->height;

    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;

    out << "P6\n" << w << " " << h << "\n255\n";
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
    for (size_t i = 0; i < static_cast<size_t>(w) * static_cast<size_t>(h); ++i) {
        rgb[i * 3 + 0] = pixels[i * 4 + 0];
        rgb[i * 3 + 1] = pixels[i * 4 + 1];
        rgb[i * 3 + 2] = pixels[i * 4 + 2];
    }
    out.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
    out.close();

    std::cout << "[OpenGLRenderer] Exported PPM screenshot: " << path << " (" << w << "x" << h << ")" << std::endl;
    return true;
}

bool OpenGLRenderer::save_screenshot_png(const std::string& path) {
    std::vector<uint8_t> pixels;
    if (!impl_->read_back(pixels)) return false;
    const bool success = fe::write_png(path, impl_->width, impl_->height, pixels.data());
    if (success) {
        std::cout << "[OpenGLRenderer] Exported PNG screenshot: " << path << " (" << impl_->width << "x" << impl_->height << ")"
                  << std::endl;
    } else {
        std::cerr << "[OpenGLRenderer] Failed to export PNG: " << path << std::endl;
    }
    return success;
}

void OpenGLRenderer::set_world_trace(std::function<bool(const Vec3& from, const Vec3& to, Vec3& hit, Vec3& normal)> trace) {
    impl_->anim_system.set_world_trace(std::move(trace));
}

void OpenGLRenderer::player_camera(const PlayerTelemetry& telemetry, Vec3& out_pos, Rotator& out_rot) const {
    // Without the character assets camera_animation() plays nothing: the plain eyes and view rotation.
    impl_->anim_system.player_camera(telemetry, out_pos, out_rot);
}

bool OpenGLRenderer::is_initialized() const { return impl_->initialized; }
bool OpenGLRenderer::is_headless() const { return impl_->headless; }
int OpenGLRenderer::width() const { return impl_->width; }
int OpenGLRenderer::height() const { return impl_->height; }
uint64_t OpenGLRenderer::frame_count() const { return impl_->frame_index; }

void OpenGLRenderer::set_menu_open(bool open) { impl_->menu_open = open; }
bool OpenGLRenderer::is_menu_open() const { return impl_->menu_open; }
void OpenGLRenderer::set_selected_chapter(int idx) { impl_->selected_chapter = std::clamp(idx, 0, 9); }
int OpenGLRenderer::selected_chapter() const { return impl_->selected_chapter; }
void OpenGLRenderer::set_selected_menu_tab(int tab) { impl_->selected_menu_tab = std::clamp(tab, 0, 3); }
int OpenGLRenderer::selected_menu_tab() const { return impl_->selected_menu_tab; }
void OpenGLRenderer::set_selected_menu_row(int row) { impl_->selected_menu_row = std::clamp(row, 0, 9); }
int OpenGLRenderer::selected_menu_row() const { return impl_->selected_menu_row; }
void OpenGLRenderer::set_menu_options_state(int sens_pct, int fov_deg, bool fullscreen) {
    impl_->opt_sens_pct = sens_pct;
    impl_->opt_fov_deg = fov_deg;
    impl_->opt_fullscreen = fullscreen;
}
void OpenGLRenderer::set_cutscene_player(const CutscenePlayer* player) { impl_->cutscene_player = player; }

void OpenGLRenderer::set_frontend_frame(const uint8_t* rgba, int width, int height, bool overlay, float saturation) {
    impl_->frontend_rgba = rgba;
    impl_->frontend_w = rgba ? width : 0;
    impl_->frontend_h = rgba ? height : 0;
    impl_->frontend_overlay = rgba && overlay;
    impl_->frontend_saturation = (rgba && overlay) ? saturation : 0.0f;
}

void* OpenGLRenderer::raw_device() const { return impl_->context; }

}  // namespace me
