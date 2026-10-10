#include "d3d11_renderer.hpp"
#include "builtin_shaders_msl.hpp"
#include "hud_font.hpp"
#include "msl_to_hlsl.hpp"
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

#include <algorithm>
#include <cctype>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

// <windows.h> macros that are ordinary identifiers in this code base.
#undef near
#undef far
#undef small

namespace me {

namespace {

template <class T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

// A texture the shaders can sample.
struct GpuTexture {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    int width = 0;
    int height = 0;
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
// material_system.cpp (the HLSL cbuffer packs a float3 and the float after it
// into one register, as MSL's packed_float3 does).
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

std::string hresult_text(HRESULT hr) {
    std::ostringstream ss;
    ss << "HRESULT 0x" << std::hex << std::setw(8) << std::setfill('0') << static_cast<uint32_t>(hr);
    return ss.str();
}

// Runs fn(i) for i in [0, count) on the process's worker threads (the front end's row pool). They
// persist between calls, so thread_local scratch buffers in fn are allocated once, not every frame.
// Must be called from one thread at a time, and fn must not call it again.
template <class Fn>
void parallel_for(size_t count, Fn&& fn) {
    fe::parallel_rows(static_cast<int>(count), [&fn](int first, int last) {
        for (int i = first; i < last; ++i) fn(static_cast<size_t>(i));
    });
}

// -----------------------------------------------------------------------------
// Material System GPU Helpers (scene_materials.hpp -> Direct3D)
// -----------------------------------------------------------------------------
// Formats Direct3D 11 samples differently from D3D9 are widened on upload: L8 as (L, L, L, 1)
// becomes BGRA8, V8U8 as (U, V, 1, 1) becomes RGBA8_SNORM.
DXGI_FORMAT dxgi_format(TexFormat f, bool srgb) {
    switch (f) {
        case TexFormat::DXT1: return srgb ? DXGI_FORMAT_BC1_UNORM_SRGB : DXGI_FORMAT_BC1_UNORM;
        case TexFormat::DXT3: return srgb ? DXGI_FORMAT_BC2_UNORM_SRGB : DXGI_FORMAT_BC2_UNORM;
        case TexFormat::DXT5: return srgb ? DXGI_FORMAT_BC3_UNORM_SRGB : DXGI_FORMAT_BC3_UNORM;
        case TexFormat::BGRA8:
        case TexFormat::G8: return srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
        case TexFormat::V8U8: return DXGI_FORMAT_R8G8B8A8_SNORM;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

// One mip level as Direct3D takes it: `data` points at `row_bytes` per row.
struct MipUpload {
    const uint8_t* data = nullptr;
    UINT row_bytes = 0;
    std::vector<uint8_t> widened;  // owns the data for G8 / V8U8
};

MipUpload mip_upload(TexFormat f, const TextureMip& m) {
    MipUpload up;
    const size_t pixels = static_cast<size_t>(m.width) * static_cast<size_t>(m.height);
    if (f == TexFormat::G8) {
        up.widened.resize(pixels * 4);
        for (size_t i = 0; i < pixels; ++i) {
            const uint8_t l = m.data[i];
            up.widened[i * 4 + 0] = l;
            up.widened[i * 4 + 1] = l;
            up.widened[i * 4 + 2] = l;
            up.widened[i * 4 + 3] = 255;
        }
        up.data = up.widened.data();
        up.row_bytes = static_cast<UINT>(m.width) * 4;
    } else if (f == TexFormat::V8U8) {
        up.widened.resize(pixels * 4);
        for (size_t i = 0; i < pixels; ++i) {
            up.widened[i * 4 + 0] = m.data[i * 2 + 0];
            up.widened[i * 4 + 1] = m.data[i * 2 + 1];
            up.widened[i * 4 + 2] = 127;
            up.widened[i * 4 + 3] = 127;
        }
        up.data = up.widened.data();
        up.row_bytes = static_cast<UINT>(m.width) * 4;
    } else {
        up.data = m.data.data();
        up.row_bytes = static_cast<UINT>(tex_row_bytes(f, m.width));
    }
    return up;
}

D3D11_TEXTURE_ADDRESS_MODE d3d_address_mode(TexAddress a) {
    switch (a) {
        case TexAddress::Clamp: return D3D11_TEXTURE_ADDRESS_CLAMP;
        case TexAddress::Mirror: return D3D11_TEXTURE_ADDRESS_MIRROR;
        default: return D3D11_TEXTURE_ADDRESS_WRAP;
    }
}

uint64_t fnv1a(const std::string& s, uint64_t h = 1469598103934665603ull) {
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

}  // namespace

// -----------------------------------------------------------------------------
// PIMPL Implementation
// -----------------------------------------------------------------------------
struct D3D11Renderer::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGISwapChain> swapchain;
    ComPtr<ID3D11InfoQueue> info_queue;  // ME_D3D_DEBUG=1: the debug layer's messages go to stderr
    int debug_messages_printed = 0;
    std::string adapter_name;
    std::string game_root;

    // The shaders, translated from MSL (renderer/msl_to_hlsl.hpp)
    MslToHlsl builtin_shaders;                    // sun_shadow_msl() + kBuiltinShadersMSL
    std::unique_ptr<MslToHlsl> material_shaders;  // the resident material library's prelude
    std::string shader_cache_dir;                 // compiled bytecode by source hash ("" = no cache)

    struct Program {
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps;
        ComPtr<ID3D11InputLayout> layout;
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
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11RenderTargetView> rtv;
        ComPtr<ID3D11ShaderResourceView> srv;
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
    ComPtr<ID3D11Buffer> cb_post;     // PostUniforms: pixel b1 during the chain
    ComPtr<ID3D11BlendState> blend_fog;  // One, SrcAlpha: scene * scattering + fog
    Program hud_program;
    Program ui_tex_program;

    // Depth Stencil States
    ComPtr<ID3D11DepthStencilState> depth_write_state;
    ComPtr<ID3D11DepthStencilState> depth_test_only_state;
    ComPtr<ID3D11DepthStencilState> depth_disabled_state;

    // Rasterizer & blend states
    ComPtr<ID3D11RasterizerState> raster_no_cull;
    ComPtr<ID3D11RasterizerState> raster_cull_back;
    ComPtr<ID3D11RasterizerState> raster_decal;  // no culling, the decals' depth bias
    ComPtr<ID3D11RasterizerState> raster_shadow_near;
    ComPtr<ID3D11RasterizerState> raster_shadow_far;
    ComPtr<ID3D11BlendState> blend_opaque;
    ComPtr<ID3D11BlendState> blend_translucent;
    ComPtr<ID3D11BlendState> blend_additive;
    ComPtr<ID3D11BlendState> blend_modulate;
    ComPtr<ID3D11BlendState> blend_ui;

    // Samplers
    ComPtr<ID3D11SamplerState> linear_sampler;      // clamp, top mip only: post-processing and UI
    ComPtr<ID3D11SamplerState> mat_samplers[3][3];  // [TexAddress X][TexAddress Y]
    ComPtr<ID3D11SamplerState> mat_cube_sampler;
    ComPtr<ID3D11SamplerState> scene_copy_sampler;
    std::unordered_map<int, ComPtr<ID3D11SamplerState>> static_samplers;  // the MSL's constexpr samplers, by configuration

    // Constant buffers
    ComPtr<ID3D11Buffer> cb_frame;     // FrameUniforms: vertex b1, pixel b0
    ComPtr<ID3D11Buffer> cb_screen;    // float2 screen size: HUD / UI vertex b1
    ComPtr<ID3D11Buffer> cb_material;  // float4 material uniforms: pixel b1
    ComPtr<ID3D11Buffer> cb_scene;     // SceneUniforms: pixel b2 of the material shaders
    SceneUniformsGPU scene_constants{};  // what it holds
    SceneLightEnvironments light_envs;   // of the dynamic objects, kept between frames
    std::vector<LensFlareQuad> flare_quads;  // this frame's
    std::vector<ModShadow> mod_shadows;      // this frame's dynamic shadows (mod_shadow.hpp)
    ParticleWorld particles;                 // the level's particle systems, kept between frames
    std::vector<uint8_t> mod_enemy_ready;
    bool viewmodel_built = false;            // the first-person body is already posed for this frame
    std::vector<Vertex> flare_vertices;
    static constexpr int kMaxMaterialUniforms = 256;

    // Framebuffer targets
    ComPtr<ID3D11Texture2D> offscreen_color_tex;  // the finished frame (RGBA8); a window's back buffer is a copy of it
    ComPtr<ID3D11RenderTargetView> offscreen_color_rtv;
    ComPtr<ID3D11Texture2D> readback_tex;
    ComPtr<ID3D11Texture2D> scene_hdr_tex;  // Intermediate HDR buffer for tone mapping
    ComPtr<ID3D11RenderTargetView> scene_hdr_rtv;
    ComPtr<ID3D11ShaderResourceView> scene_hdr_srv;
    ComPtr<ID3D11Texture2D> depth_tex;
    ComPtr<ID3D11DepthStencilView> depth_dsv;
    ComPtr<ID3D11ShaderResourceView> depth_srv;
    ComPtr<ID3D11Texture2D> shadow_depth_tex;  // sun shadow cascades: 2-slice 4096x4096 depth array (renderer/sun_shadow.hpp)
    ComPtr<ID3D11DepthStencilView> shadow_dsv[3];
    ComPtr<ID3D11ShaderResourceView> shadow_srv;
    ComPtr<ID3D11Texture2D> scene_color_copy;  // opaque scene color (SceneTexture / DestColor)
    ComPtr<ID3D11ShaderResourceView> scene_color_copy_srv;
    ComPtr<ID3D11Texture2D> scene_depth_copy;  // opaque scene depth (SceneDepth / DepthBiased*)
    ComPtr<ID3D11ShaderResourceView> scene_depth_copy_srv;
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
    std::vector<ComPtr<ID3D11Buffer>> enemy_index_buffers;
    static constexpr float kEnemyReach = 1024.0f;  // UU: a posed enemy's vertices are within this of its origin
    AnimSystem anim_system;

    // Reusable dynamic vertex buffers for per-frame geometry (viewmodel, combat effects, HUD) and the enemies.
    struct DynamicBuffer {
        ComPtr<ID3D11Buffer> buffer;
        size_t capacity = 0;
    };
    std::vector<DynamicBuffer> dyn_vertex_buffers;
    size_t dyn_vertex_cursor = 0;
    std::vector<DynamicBuffer> enemy_vertex_buffers;

    static size_t dynamic_buffer_capacity(size_t length) {
        return std::max<size_t>((length + 4095u) & ~size_t(4095u), 65536u);
    }

    // Maps `b` for writing `length` bytes from the start (its old contents are discarded), growing it
    // first if needed. Returns null on failure; otherwise the caller unmaps b.buffer.
    void* map_dynamic_buffer(DynamicBuffer& b, size_t length) {
        if (!b.buffer || b.capacity < length) {
            D3D11_BUFFER_DESC d{};
            d.ByteWidth = static_cast<UINT>(dynamic_buffer_capacity(length));
            d.Usage = D3D11_USAGE_DYNAMIC;
            d.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            b.buffer.Reset();
            b.capacity = SUCCEEDED(device->CreateBuffer(&d, nullptr, &b.buffer)) ? d.ByteWidth : 0;
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (!b.buffer || FAILED(ctx->Map(b.buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return nullptr;
        return mapped.pData;
    }

    void fill_dynamic_buffer(DynamicBuffer& b, const void* data, size_t length) {
        if (void* dst = map_dynamic_buffer(b, length)) {
            std::memcpy(dst, data, length);
            ctx->Unmap(b.buffer.Get(), 0);
        }
    }

    ID3D11Buffer* acquire_dynamic_vertex_buffer(const void* data, size_t length) {
        const size_t idx = dyn_vertex_cursor++;
        if (idx >= dyn_vertex_buffers.size()) dyn_vertex_buffers.emplace_back();
        fill_dynamic_buffer(dyn_vertex_buffers[idx], data, length);
        return dyn_vertex_buffers[idx].buffer.Get();
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

    void draw(UINT vertex_count, UINT first_vertex) {
        ctx->Draw(vertex_count, first_vertex);
        ++prof_draws;
    }

    void update_constants(const ComPtr<ID3D11Buffer>& buffer, const void* data, size_t length) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (buffer && SUCCEEDED(ctx->Map(buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            std::memcpy(mapped.pData, data, length);
            ctx->Unmap(buffer.Get(), 0);
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
    UITexture frontend_tex;

    // Cached GPU Vertex Buffers & Per-Section Material/Shadow Metadata for Scene Meshes
    std::string cached_map_name;
    size_t cached_total_verts = 0;
    std::vector<ComPtr<ID3D11Buffer>> cached_mesh_buffers;
    std::shared_ptr<const SceneMaterialLibrary> cached_section_mat_lib;
    bool cached_has_translucent = false;
    bool cached_needs_scene_copies = false;
    static constexpr uint8_t kSecShadowCaster = 1u << 0;
    static constexpr uint8_t kSecTranslucent = 1u << 1;
    std::vector<std::vector<uint8_t>> cached_section_flags;

    // -------------------------------------------------------------------------
    // Mirror's Edge material system (LevelScene::materials) GPU cache
    // -------------------------------------------------------------------------
    std::shared_ptr<const SceneMaterialLibrary> mat_lib;  // library currently resident on the GPU
    std::vector<UITexture> mat_textures;                  // per SceneTexture (null = missing -> default)
    std::vector<UITexture> lm_textures;                   // SceneMaterialLibrary::lightmap_textures (null = unreadable)
    std::vector<ComPtr<ID3D11PixelShader>> mat_pixel_shaders;  // per MaterialShader (null = failed -> legacy)
    ComPtr<ID3D11VertexShader> mat_vertex_shader;
    ComPtr<ID3D11InputLayout> mat_input_layout;
    UITexture tex_default_white;
    UITexture tex_default_flat_normal;
    UITexture tex_default_black;
    UITexture tex_default_cube;

    // UE3 culls back faces of non-two-sided materials. ME_CULL=off|cw|ccw overrides (debugging).
    // With this renderer's view/projection, UE3 front faces are counter-clockwise on screen.
    bool mat_cull_enabled = true;
    bool mat_front_ccw = true;

    void configure_material_culling() {
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
    // Device
    // -------------------------------------------------------------------------
    bool create_device() {
        UINT flags = 0;
        const char* debug_env = std::getenv("ME_D3D_DEBUG");
        const bool want_debug = debug_env && debug_env[0] != '\0' && debug_env[0] != '0';
        if (want_debug) flags |= D3D11_CREATE_DEVICE_DEBUG;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
        HRESULT hr = E_FAIL;

        // On a machine with two GPUs the default adapter is the one that drives the display, usually the
        // integrated one. Ask for the fast one instead (ME_GPU=integrated asks for the other).
        ComPtr<IDXGIAdapter1> preferred;
        {
            const char* gpu_env = std::getenv("ME_GPU");
            const bool low_power = gpu_env && std::string(gpu_env) == "integrated";
            ComPtr<IDXGIFactory6> factory6;
            if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory6), reinterpret_cast<void**>(factory6.GetAddressOf())))) {
                factory6->EnumAdapterByGpuPreference(
                    0, low_power ? DXGI_GPU_PREFERENCE_MINIMUM_POWER : DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                    __uuidof(IDXGIAdapter1), reinterpret_cast<void**>(preferred.GetAddressOf()));
            }
        }

        // The preferred adapter, else the default one, else the software rasterizer (WARP), which is
        // enough for the headless oracle.
        struct Attempt {
            IDXGIAdapter* adapter;
            D3D_DRIVER_TYPE type;
        };
        const Attempt attempts[] = {{preferred.Get(), D3D_DRIVER_TYPE_UNKNOWN},
                                    {nullptr, D3D_DRIVER_TYPE_HARDWARE},
                                    {nullptr, D3D_DRIVER_TYPE_WARP}};
        for (const Attempt& a : attempts) {
            if (a.type == D3D_DRIVER_TYPE_UNKNOWN && !a.adapter) continue;
            hr = D3D11CreateDevice(a.adapter, a.type, nullptr, flags, levels, 1, D3D11_SDK_VERSION, &device, &got, &ctx);
            if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG)) {
                // The debug layer is an optional Windows feature ("Graphics Tools").
                std::cerr << "[D3D11Renderer] ME_D3D_DEBUG: the Direct3D debug layer is not installed." << std::endl;
                flags &= ~static_cast<UINT>(D3D11_CREATE_DEVICE_DEBUG);
                hr = D3D11CreateDevice(a.adapter, a.type, nullptr, flags, levels, 1, D3D11_SDK_VERSION, &device, &got, &ctx);
            }
            if (SUCCEEDED(hr)) break;
        }
        if (FAILED(hr)) {
            std::cerr << "[D3D11Renderer] No Direct3D 11 (feature level 11_0) device: " << hresult_text(hr) << std::endl;
            return false;
        }
        if (flags & D3D11_CREATE_DEVICE_DEBUG) device.As(&info_queue);

        adapter_name = "unknown adapter";
        ComPtr<IDXGIDevice> dxgi_device;
        ComPtr<IDXGIAdapter> adapter;
        if (SUCCEEDED(device.As(&dxgi_device)) && SUCCEEDED(dxgi_device->GetAdapter(&adapter))) {
            DXGI_ADAPTER_DESC desc{};
            if (SUCCEEDED(adapter->GetDesc(&desc))) {
                char name[256] = {};
                WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
                adapter_name = name;
            }
        }

        if (std::getenv("ME_NO_SHADER_CACHE") == nullptr) {
            const std::string base = cache_dir();
            if (!base.empty()) {
                shader_cache_dir = base + "/shader_cache";
                std::error_code ec;
                std::filesystem::create_directories(shader_cache_dir, ec);
                if (ec) shader_cache_dir.clear();
            }
        }
        return true;
    }

    bool create_swapchain(HWND hwnd) {
        ComPtr<IDXGIDevice> dxgi_device;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory> factory;
        if (FAILED(device.As(&dxgi_device)) || FAILED(dxgi_device->GetAdapter(&adapter)) ||
            FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
            return false;
        }
        DXGI_SWAP_CHAIN_DESC d{};
        d.BufferDesc.Width = static_cast<UINT>(width);
        d.BufferDesc.Height = static_cast<UINT>(height);
        d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.OutputWindow = hwnd;
        d.Windowed = TRUE;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        HRESULT hr = factory->CreateSwapChain(device.Get(), &d, &swapchain);
        if (FAILED(hr)) {  // before Windows 10
            d.BufferCount = 1;
            d.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            hr = factory->CreateSwapChain(device.Get(), &d, &swapchain);
        }
        if (FAILED(hr)) {
            std::cerr << "[D3D11Renderer] CreateSwapChain failed: " << hresult_text(hr) << std::endl;
            return false;
        }
        // SDL owns the window, including the fullscreen toggle.
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        return true;
    }

    // With ME_D3D_DEBUG=1, prints what the debug layer has to say about the frame.
    void drain_debug_messages() {
        if (!info_queue) return;
        const UINT64 count = info_queue->GetNumStoredMessages();
        for (UINT64 i = 0; i < count && debug_messages_printed < 40; ++i) {
            SIZE_T length = 0;
            info_queue->GetMessage(i, nullptr, &length);
            std::vector<uint8_t> storage(length);
            auto* msg = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
            if (SUCCEEDED(info_queue->GetMessage(i, msg, &length)) && msg->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
                std::cerr << "[D3D11 debug] " << std::string(msg->pDescription, msg->DescriptionByteLength) << std::endl;
                ++debug_messages_printed;
            }
        }
        info_queue->ClearStoredMessages();
    }

    // -------------------------------------------------------------------------
    // Shaders
    // -------------------------------------------------------------------------
    // Bytecode for a translated shader: from the on-disk cache when this exact HLSL was compiled before,
    // else from the HLSL compiler (d3dcompiler_47.dll, part of Windows). Thread-safe.
    std::vector<uint8_t> compile(const HlslShader& sh, std::string* error) const {
        if (!sh.error.empty()) {
            if (error) *error = sh.error;
            return {};
        }
        const char* target = sh.vertex_stage ? "vs_5_0" : "ps_5_0";
        const UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL1;
        std::string cache_path;
        if (!shader_cache_dir.empty()) {
            const uint64_t h = fnv1a(sh.source, fnv1a(sh.entry + "|" + target + "|" + std::to_string(flags) + "|v1"));
            std::ostringstream name;
            name << shader_cache_dir << "/" << std::hex << std::setw(16) << std::setfill('0') << h << "." << target << ".cso";
            cache_path = name.str();
            std::ifstream in(cache_path, std::ios::binary);
            if (in) {
                std::vector<uint8_t> code((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                if (code.size() > 4 && std::memcmp(code.data(), "DXBC", 4) == 0) return code;
            }
        }
        ComPtr<ID3DBlob> code;
        ComPtr<ID3DBlob> messages;
        const HRESULT hr = D3DCompile(sh.source.data(), sh.source.size(), sh.entry.c_str(), nullptr, nullptr, sh.entry.c_str(),
                                      target, flags, 0, &code, &messages);
        if (FAILED(hr) || !code) {
            if (error) {
                *error = messages ? std::string(static_cast<const char*>(messages->GetBufferPointer()), messages->GetBufferSize())
                                  : hresult_text(hr);
            }
            return {};
        }
        const auto* bytes = static_cast<const uint8_t*>(code->GetBufferPointer());
        std::vector<uint8_t> out(bytes, bytes + code->GetBufferSize());
        if (!cache_path.empty()) {
            // Written under a private name first, so a reader never sees half a file.
            std::ostringstream tmp;
            tmp << cache_path << "." << GetCurrentProcessId() << "." << std::this_thread::get_id() << ".tmp";
            {
                std::ofstream f(tmp.str(), std::ios::binary);
                f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
            }
            std::error_code ec;
            std::filesystem::rename(tmp.str(), cache_path, ec);
            if (ec) std::filesystem::remove(tmp.str(), ec);
        }
        return out;
    }

    // Input layout for a vertex stage that reads me::Vertex (or the material prelude's copy of it)
    // through the semantics VTX0..n the translator assigned to the struct's members.
    ComPtr<ID3D11InputLayout> vertex_input_layout(const HlslShader& sh, const std::vector<uint8_t>& code) {
        std::vector<D3D11_INPUT_ELEMENT_DESC> elements;
        UINT offset = 0;
        for (size_t i = 0; i < sh.vertex_member_types.size(); ++i) {
            const std::string& type = sh.vertex_member_types[i];
            DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
            UINT size = 0;
            if (type == "float") { format = DXGI_FORMAT_R32_FLOAT; size = 4; }
            else if (type == "float2") { format = DXGI_FORMAT_R32G32_FLOAT; size = 8; }
            else if (type == "float3") { format = DXGI_FORMAT_R32G32B32_FLOAT; size = 12; }
            else if (type == "float4") { format = DXGI_FORMAT_R32G32B32A32_FLOAT; size = 16; }
            else if (type == "uint") { format = DXGI_FORMAT_R32_UINT; size = 4; }
            else if (type == "int") { format = DXGI_FORMAT_R32_SINT; size = 4; }
            else return nullptr;
            elements.push_back({"VTX", static_cast<UINT>(i), format, 0, offset, D3D11_INPUT_PER_VERTEX_DATA, 0});
            offset += size;
        }
        if (offset != sizeof(Vertex)) {
            std::cerr << "[D3D11Renderer] The shaders' vertex struct is " << offset << " bytes, me::Vertex is " << sizeof(Vertex)
                      << std::endl;
            return nullptr;
        }
        ComPtr<ID3D11InputLayout> layout;
        device->CreateInputLayout(elements.data(), static_cast<UINT>(elements.size()), code.data(), code.size(), &layout);
        return layout;
    }

    enum class VertexKind { None, Scene, Hud, UiTex };

    bool make_program(const char* vertex_fn, const char* pixel_fn, VertexKind kind, Program& out) {
        std::string err;
        const HlslShader vs = builtin_shaders.emit(vertex_fn);
        const std::vector<uint8_t> vs_code = compile(vs, &err);
        if (vs_code.empty() || FAILED(device->CreateVertexShader(vs_code.data(), vs_code.size(), nullptr, &out.vs))) {
            std::cerr << "[D3D11Renderer] Shader compilation failed (" << vertex_fn << "): " << err << std::endl;
            return false;
        }
        if (pixel_fn) {
            const std::vector<uint8_t> ps_code = compile(builtin_shaders.emit(pixel_fn), &err);
            if (ps_code.empty() || FAILED(device->CreatePixelShader(ps_code.data(), ps_code.size(), nullptr, &out.ps))) {
                std::cerr << "[D3D11Renderer] Shader compilation failed (" << pixel_fn << "): " << err << std::endl;
                return false;
            }
        }
        switch (kind) {
            case VertexKind::None: break;
            case VertexKind::Scene: out.layout = vertex_input_layout(vs, vs_code); break;
            case VertexKind::Hud: {
                const D3D11_INPUT_ELEMENT_DESC e[] = {
                    {"ATTR", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(HUDVertex, position), D3D11_INPUT_PER_VERTEX_DATA, 0},
                    {"ATTR", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(HUDVertex, color), D3D11_INPUT_PER_VERTEX_DATA, 0},
                };
                device->CreateInputLayout(e, 2, vs_code.data(), vs_code.size(), &out.layout);
                break;
            }
            case VertexKind::UiTex: {
                const D3D11_INPUT_ELEMENT_DESC e[] = {
                    {"ATTR", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(UITexVertex, position), D3D11_INPUT_PER_VERTEX_DATA, 0},
                    {"ATTR", 1, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(UITexVertex, uv), D3D11_INPUT_PER_VERTEX_DATA, 0},
                    {"ATTR", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(UITexVertex, color), D3D11_INPUT_PER_VERTEX_DATA, 0},
                };
                device->CreateInputLayout(e, 3, vs_code.data(), vs_code.size(), &out.layout);
                break;
            }
        }
        if (kind != VertexKind::None && !out.layout) {
            std::cerr << "[D3D11Renderer] No input layout for " << vertex_fn << std::endl;
            return false;
        }
        return true;
    }

    ComPtr<ID3D11SamplerState> make_sampler(D3D11_FILTER filter, D3D11_TEXTURE_ADDRESS_MODE u, D3D11_TEXTURE_ADDRESS_MODE v,
                                            D3D11_TEXTURE_ADDRESS_MODE w, float max_lod, UINT anisotropy = 1,
                                            D3D11_COMPARISON_FUNC compare = D3D11_COMPARISON_NEVER) {
        D3D11_SAMPLER_DESC d{};
        d.Filter = filter;
        d.AddressU = u;
        d.AddressV = v;
        d.AddressW = w;
        d.MaxAnisotropy = anisotropy;
        d.ComparisonFunc = compare;
        d.MinLOD = 0.0f;
        d.MaxLOD = max_lod;
        ComPtr<ID3D11SamplerState> s;
        device->CreateSamplerState(&d, &s);
        return s;
    }

    // Binds the `constexpr sampler`s of the MSL a translator was given at their slots.
    void bind_static_samplers(const MslToHlsl& translator) {
        for (const HlslStaticSampler& s : translator.static_samplers()) {
            const int key = (s.linear ? 1 : 0) | (s.repeat ? 2 : 0) | (s.compare ? 4 : 0);
            ComPtr<ID3D11SamplerState>& state = static_samplers[key];
            if (!state) {
                const D3D11_TEXTURE_ADDRESS_MODE address = s.repeat ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
                D3D11_FILTER filter = s.linear ? D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT : D3D11_FILTER_MIN_MAG_MIP_POINT;
                if (s.compare) {
                    filter = s.linear ? D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT : D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
                }
                state = make_sampler(filter, address, address, address, 0.0f, 1,
                                     s.compare ? D3D11_COMPARISON_LESS_EQUAL : D3D11_COMPARISON_NEVER);
            }
            ctx->PSSetSamplers(static_cast<UINT>(s.slot), 1, state.GetAddressOf());
        }
    }

    bool compile_shaders() {
        std::string err;
        // The sun shadow lookup is shared with the generated material shaders (renderer/sun_shadow.hpp).
        if (!builtin_shaders.add_source(sun_shadow_msl() + kBuiltinShadersMSL, &err)) {
            std::cerr << "[D3D11Renderer] Cannot translate the built-in shaders: " << err << std::endl;
            return false;
        }
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

        // Depth Stencil States
        D3D11_DEPTH_STENCIL_DESC ds{};
        ds.DepthEnable = TRUE;
        ds.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        device->CreateDepthStencilState(&ds, &depth_write_state);
        ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        device->CreateDepthStencilState(&ds, &depth_test_only_state);
        ds.DepthEnable = FALSE;
        ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
        device->CreateDepthStencilState(&ds, &depth_disabled_state);

        // Rasterizer states
        D3D11_RASTERIZER_DESC rs{};
        rs.FillMode = D3D11_FILL_SOLID;
        rs.CullMode = D3D11_CULL_NONE;
        rs.DepthClipEnable = TRUE;
        device->CreateRasterizerState(&rs, &raster_no_cull);
        rs.CullMode = D3D11_CULL_BACK;
        rs.FrontCounterClockwise = mat_front_ccw ? TRUE : FALSE;
        device->CreateRasterizerState(&rs, &raster_cull_back);
        rs.CullMode = D3D11_CULL_NONE;
        rs.DepthBias = kDecalDepthBias;
        rs.SlopeScaledDepthBias = kDecalSlopeBias;
        device->CreateRasterizerState(&rs, &raster_decal);
        rs.DepthBias = 0;
        // Shadow passes: slope-scaled bias, capped in UU (the cap is in map depth units).
        rs.CullMode = D3D11_CULL_NONE;
        rs.FrontCounterClockwise = FALSE;
        rs.SlopeScaledDepthBias = 1.75f;
        rs.DepthBiasClamp = kSunShadowSlopeBiasCap / kSunShadowDepthRange;
        device->CreateRasterizerState(&rs, &raster_shadow_near);
        rs.DepthBiasClamp = kSunShadowFarSlopeBiasCap / kSunShadowFarDepthRange;
        device->CreateRasterizerState(&rs, &raster_shadow_far);

        // Blend states
        auto make_blend = [&](bool enable, D3D11_BLEND src, D3D11_BLEND dst, D3D11_BLEND src_a, D3D11_BLEND dst_a) {
            D3D11_BLEND_DESC b{};
            auto& rt = b.RenderTarget[0];
            rt.BlendEnable = enable ? TRUE : FALSE;
            rt.SrcBlend = src;
            rt.DestBlend = dst;
            rt.BlendOp = D3D11_BLEND_OP_ADD;
            rt.SrcBlendAlpha = src_a;
            rt.DestBlendAlpha = dst_a;
            rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            ComPtr<ID3D11BlendState> state;
            device->CreateBlendState(&b, &state);
            return state;
        };
        blend_opaque = make_blend(false, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_ONE, D3D11_BLEND_ZERO);
        blend_translucent = make_blend(true, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_ZERO, D3D11_BLEND_ONE);
        blend_additive = make_blend(true, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_ONE);
        blend_modulate = make_blend(true, D3D11_BLEND_DEST_COLOR, D3D11_BLEND_ZERO, D3D11_BLEND_ZERO, D3D11_BLEND_ONE);
        blend_fog = make_blend(true, D3D11_BLEND_ONE, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_ZERO, D3D11_BLEND_ONE);
        // Alpha blending for UI overlay
        blend_ui = make_blend(true, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_SRC_ALPHA,
                              D3D11_BLEND_INV_SRC_ALPHA);

        // Linear Texture Sampler
        linear_sampler = make_sampler(D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP,
                                      D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP, 0.0f);

        // Constant buffers
        auto make_constants = [&](size_t bytes) {
            D3D11_BUFFER_DESC d{};
            d.ByteWidth = static_cast<UINT>((bytes + 15u) & ~size_t(15u));
            d.Usage = D3D11_USAGE_DYNAMIC;
            d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            ComPtr<ID3D11Buffer> b;
            device->CreateBuffer(&d, nullptr, &b);
            return b;
        };
        cb_frame = make_constants(sizeof(FrameUniformsGPU));
        cb_post = make_constants(sizeof(PostUniformsGPU));
        cb_screen = make_constants(16);
        cb_material = make_constants(static_cast<size_t>(kMaxMaterialUniforms) * 16);
        cb_scene = make_constants(sizeof(SceneUniformsGPU));
        return cb_frame && cb_screen && cb_material && linear_sampler && blend_ui && raster_no_cull && depth_write_state;
    }

    // -------------------------------------------------------------------------
    // Textures
    // -------------------------------------------------------------------------
    UITexture make_texture(const D3D11_TEXTURE2D_DESC& desc, const D3D11_SUBRESOURCE_DATA* initial) {
        auto t = std::make_shared<GpuTexture>();
        if (FAILED(device->CreateTexture2D(&desc, initial, &t->texture))) return nullptr;
        if (FAILED(device->CreateShaderResourceView(t->texture.Get(), nullptr, &t->srv))) return nullptr;
        t->width = static_cast<int>(desc.Width);
        t->height = static_cast<int>(desc.Height);
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
        D3D11_TEXTURE2D_DESC d{};
        d.Width = 4;
        d.Height = 4;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_IMMUTABLE;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = px;
        init.SysMemPitch = 16;
        return make_texture(d, &init);
    }

    // A texture the CPU rewrites: decoded video and front end frames (RGBA8, top row first).
    UITexture make_dynamic_texture(int w, int h, bool srgb) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = static_cast<UINT>(w);
        d.Height = static_cast<UINT>(h);
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DYNAMIC;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        return make_texture(d, nullptr);
    }

    void write_dynamic_texture(const UITexture& t, const uint8_t* rgba) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (!t || FAILED(ctx->Map(t->texture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
        const size_t row = static_cast<size_t>(t->width) * 4;
        for (int y = 0; y < t->height; ++y) {
            std::memcpy(static_cast<uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
                        rgba + static_cast<size_t>(y) * row, row);
        }
        ctx->Unmap(t->texture.Get(), 0);
    }

    void create_material_defaults() {
        tex_default_white = make_solid_texture(255, 255, 255, 255);
        tex_default_flat_normal = make_solid_texture(128, 128, 255, 255);
        tex_default_black = make_solid_texture(0, 0, 0, 255);

        // The stand-in for a missing cubemap: a sky gradient over a ground haze.
        constexpr int kCubeDim = 64;
        std::vector<std::vector<uint8_t>> levels;
        std::vector<D3D11_SUBRESOURCE_DATA> init;
        int mip_count = 0;
        for (int dim = kCubeDim; dim >= 1; dim /= 2) ++mip_count;
        for (int face = 0; face < 6; ++face) {
            for (int dim = kCubeDim; dim >= 1; dim /= 2) {
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
                levels.push_back(std::move(px));
                D3D11_SUBRESOURCE_DATA sd{};
                sd.SysMemPitch = static_cast<UINT>(dim) * 4;
                init.push_back(sd);
            }
        }
        for (size_t i = 0; i < levels.size(); ++i) init[i].pSysMem = levels[i].data();
        D3D11_TEXTURE2D_DESC cd{};
        cd.Width = kCubeDim;
        cd.Height = kCubeDim;
        cd.MipLevels = static_cast<UINT>(mip_count);
        cd.ArraySize = 6;
        cd.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        cd.SampleDesc.Count = 1;
        cd.Usage = D3D11_USAGE_IMMUTABLE;
        cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        cd.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
        tex_default_cube = make_texture(cd, init.data());

        // UE3 samples each texture with its own AddressX/AddressY and trilinear/anisotropic filtering.
        for (int x = 0; x < 3; ++x) {
            for (int y = 0; y < 3; ++y) {
                mat_samplers[x][y] = make_sampler(D3D11_FILTER_ANISOTROPIC, d3d_address_mode(static_cast<TexAddress>(x)),
                                                  d3d_address_mode(static_cast<TexAddress>(y)), D3D11_TEXTURE_ADDRESS_WRAP,
                                                  D3D11_FLOAT32_MAX, 8);
            }
        }
        mat_cube_sampler = make_sampler(D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_CLAMP,
                                        D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_FLOAT32_MAX);
        // The scene copies have one mip; the light maps bound beside them have a full chain.
        scene_copy_sampler = make_sampler(D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_CLAMP,
                                          D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_FLOAT32_MAX);
    }

    // Uploads one decoded UE3 texture (2D or cube, full mip chain). Thread-safe.
    UITexture upload_scene_texture(const SceneTexture& st) {
        if (!st.valid()) return nullptr;
        const DXGI_FORMAT fmt = dxgi_format(st.format, st.srgb);
        if (fmt == DXGI_FORMAT_UNKNOWN) return nullptr;

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
        // Direct3D wants block-compressed textures in whole 4x4 blocks at the top level.
        if (tex_format_is_bc(st.format) && ((top.width % 4) != 0 || (top.height % 4) != 0)) return nullptr;

        std::vector<MipUpload> uploads;
        std::vector<D3D11_SUBRESOURCE_DATA> init;
        uploads.reserve(static_cast<size_t>(face_count) * static_cast<size_t>(levels));
        for (int f = 0; f < face_count; ++f) {
            const std::vector<TextureMip>& chain = st.is_cube ? st.faces[static_cast<size_t>(f)] : st.mips;
            for (int i = 0; i < levels; ++i) uploads.push_back(mip_upload(st.format, chain[static_cast<size_t>(i)]));
        }
        for (const MipUpload& up : uploads) {
            D3D11_SUBRESOURCE_DATA sd{};
            sd.pSysMem = up.data;
            sd.SysMemPitch = up.row_bytes;
            init.push_back(sd);
        }
        D3D11_TEXTURE2D_DESC d{};
        d.Width = static_cast<UINT>(top.width);
        d.Height = static_cast<UINT>(top.height);
        d.MipLevels = static_cast<UINT>(levels);
        d.ArraySize = static_cast<UINT>(face_count);
        d.Format = fmt;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_IMMUTABLE;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags = st.is_cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
        return make_texture(d, init.data());
    }

    // Compiles every generated material fragment shader (in parallel) from the library's MSL.
    // Failing shaders fall back to the legacy pipeline.
    void compile_material_shaders(const SceneMaterialLibrary& lib) {
        const size_t n = lib.shaders.size();
        mat_pixel_shaders.assign(n, nullptr);
        mat_vertex_shader.Reset();
        mat_input_layout.Reset();
        material_shaders = std::make_unique<MslToHlsl>();
        if (n == 0) return;

        std::string err;
        if (!material_shaders->add_source(lib.common_source, &err)) {
            std::cerr << "[D3D11Renderer] Cannot translate the material prelude: " << err << std::endl;
            return;
        }
        const HlslShader vs = material_shaders->emit(lib.vertex_function);
        const std::vector<uint8_t> vs_code = compile(vs, &err);
        if (vs_code.empty() || FAILED(device->CreateVertexShader(vs_code.data(), vs_code.size(), nullptr, &mat_vertex_shader))) {
            std::cerr << "[D3D11Renderer] Material vertex shader failed: " << err << std::endl;
            return;
        }
        mat_input_layout = vertex_input_layout(vs, vs_code);
        if (!mat_input_layout) {
            mat_vertex_shader.Reset();
            return;
        }

        std::mutex err_mutex;
        std::vector<std::string> errors;
        std::vector<ComPtr<ID3D11PixelShader>> shaders(n);
        const MslToHlsl* translator = material_shaders.get();
        parallel_for(n, [&](size_t i) {
            const MaterialShader& sh = lib.shaders[i];
            std::string message;
            if (sh.num_uniforms > kMaxMaterialUniforms) {
                message = "more than " + std::to_string(kMaxMaterialUniforms) + " uniforms";
            } else {
                const std::vector<uint8_t> code =
                    compile(translator->emit_from(sh.source, sh.function_name, sh.num_uniforms), &message);
                if (!code.empty() && FAILED(device->CreatePixelShader(code.data(), code.size(), nullptr, &shaders[i]))) {
                    message = "CreatePixelShader failed";
                }
            }
            if (!shaders[i]) {
                std::lock_guard<std::mutex> lock(err_mutex);
                errors.push_back(sh.function_name + " (" + sh.base_material + "): " + message);
            }
        });

        size_t ok = 0;
        for (size_t i = 0; i < n; ++i) {
            mat_pixel_shaders[i] = shaders[i];
            if (shaders[i]) ok++;
        }
        std::cout << "[D3D11Renderer] Material shaders: " << ok << "/" << n << " compiled" << std::endl;
        for (size_t e = 0; e < errors.size() && e < 4; ++e) {
            std::string msg = errors[e];
            if (msg.size() > 900) msg = msg.substr(0, 900) + " ...";
            std::cerr << "[D3D11Renderer]   shader error: " << msg << std::endl;
        }
    }

    // Makes `lib` the resident material library (uploads textures, compiles shaders) if it changed.
    void sync_material_library(const std::shared_ptr<const SceneMaterialLibrary>& lib) {
        if (lib == mat_lib) return;
        mat_lib = lib;
        mat_textures.clear();
        lm_textures.clear();
        mat_pixel_shaders.clear();
        if (!lib) return;

        const auto t0 = std::chrono::steady_clock::now();
        const size_t n_tex = lib->textures.size();
        mat_textures.assign(n_tex, nullptr);
        std::atomic<size_t> uploaded{0};
        parallel_for(n_tex, [&](size_t i) {
            mat_textures[i] = upload_scene_texture(lib->textures[i]);
            if (mat_textures[i]) uploaded.fetch_add(1, std::memory_order_relaxed);
        });
        lm_textures.assign(lib->lightmap_textures.size(), nullptr);
        parallel_for(lm_textures.size(), [&](size_t i) {
            if (lib->lightmap_textures[i].valid()) lm_textures[i] = upload_scene_texture(lib->lightmap_textures[i]);
        });
        compile_material_shaders(*lib);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::cout << "[D3D11Renderer] Material library resident: " << lib->materials.size() << " materials, "
                  << uploaded.load() << "/" << n_tex << " textures uploaded in " << secs << " s" << std::endl;
    }

    // Returns the pixel shader for a mesh section, or null when it must use legacy procedural shading.
    ID3D11PixelShader* section_shader(const MeshSection& s, const MaterialShader** out_shader,
                                      const SceneMaterial** out_material) const {
        if (!mat_lib || !mat_vertex_shader || s.material < 0 || static_cast<size_t>(s.material) >= mat_lib->materials.size()) {
            return nullptr;
        }
        const SceneMaterial& m = mat_lib->materials[static_cast<size_t>(s.material)];
        if (m.shader < 0 || static_cast<size_t>(m.shader) >= mat_pixel_shaders.size()) return nullptr;
        ID3D11PixelShader* ps = mat_pixel_shaders[static_cast<size_t>(m.shader)].Get();
        if (!ps) return nullptr;
        if (out_shader) *out_shader = &mat_lib->shaders[static_cast<size_t>(m.shader)];
        if (out_material) *out_material = &m;
        return ps;
    }

    void set_material_uniforms(const std::vector<std::array<float, 4>>& values, int count) {
        std::array<std::array<float, 4>, kMaxMaterialUniforms> padded;
        const size_t n = static_cast<size_t>(std::min(count, kMaxMaterialUniforms));
        for (size_t i = 0; i < n; ++i) padded[i] = (i < values.size()) ? values[i] : std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f};
        update_constants(cb_material, padded.data(), n * 16);
    }

    // Binds a section's three light-map coefficient textures (texture(24..26), sampler(15)). A
    // section without a set gets white: its vertices carry their own samples, or none. A texture
    // that could not be read is black, so what it would have lit stays unlit.
    void bind_lightmap(int32_t set) {
        ID3D11ShaderResourceView* srvs[3];
        for (int k = 0; k < 3; ++k) {
            const GpuTexture* t = tex_default_white.get();
            if (set >= 0) {
                const size_t i = static_cast<size_t>(set) * 3 + static_cast<size_t>(k);
                t = (i < lm_textures.size() && lm_textures[i]) ? lm_textures[i].get() : tex_default_black.get();
            }
            srvs[k] = t ? t->srv.Get() : nullptr;
        }
        ctx->PSSetShaderResources(matbind::kLightMapTexture, 3, srvs);
        ctx->PSSetSamplers(matbind::kSceneSampler, 1, scene_copy_sampler.GetAddressOf());
    }

    // Binds a material instance's textures, samplers and parameter uniforms.
    void bind_material(const SceneMaterial& m, const MaterialShader& sh) {
        ID3D11ShaderResourceView* srvs[matbind::kMaxTextureSlots] = {};
        ID3D11SamplerState* samplers[matbind::kMaxTextureSlots] = {};
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
            srvs[k] = t ? t->srv.Get() : nullptr;
            samplers[k] = mat_samplers[static_cast<int>(ax) % 3][static_cast<int>(ay) % 3].Get();
        }
        for (int j = 0; j < ncube; ++j) {
            const int ti = (static_cast<size_t>(j) < m.texcube.size()) ? m.texcube[static_cast<size_t>(j)] : -1;
            const GpuTexture* t = (ti >= 0 && static_cast<size_t>(ti) < mat_textures.size()) ? mat_textures[static_cast<size_t>(ti)].get() : nullptr;
            if (!t) t = tex_default_cube.get();
            srvs[n2d + j] = t ? t->srv.Get() : nullptr;
            samplers[n2d + j] = mat_cube_sampler.Get();
        }
        if (n2d + ncube > 0) {
            ctx->PSSetShaderResources(0, static_cast<UINT>(n2d + ncube), srvs);
            ctx->PSSetSamplers(0, static_cast<UINT>(n2d + ncube), samplers);
        }
        if (sh.num_uniforms > 0) set_material_uniforms(m.uniforms, sh.num_uniforms);
        if (sh.uses_scene_color || sh.uses_scene_depth) {
            ID3D11ShaderResourceView* copies[2] = {scene_color_copy_srv.Get(), scene_depth_copy_srv.Get()};
            ctx->PSSetShaderResources(matbind::kSceneColorTexture, 2, copies);
            ctx->PSSetSamplers(matbind::kSceneSampler, 1, scene_copy_sampler.GetAddressOf());
        }
    }

    // -------------------------------------------------------------------------
    // Render targets
    // -------------------------------------------------------------------------
    bool make_color_target(ColorTarget& t, int w, int h, DXGI_FORMAT format) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = static_cast<UINT>(std::max(1, w));
        d.Height = static_cast<UINT>(std::max(1, h));
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = format;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        t = ColorTarget{};
        t.w = static_cast<int>(d.Width);
        t.h = static_cast<int>(d.Height);
        return SUCCEEDED(device->CreateTexture2D(&d, nullptr, &t.tex)) &&
               SUCCEEDED(device->CreateRenderTargetView(t.tex.Get(), nullptr, &t.rtv)) &&
               SUCCEEDED(device->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv));
    }

    bool allocate_render_targets() {
        // The views of the old targets may still be bound.
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        unbind_shader_resources();

        auto make_target = [&](DXGI_FORMAT format, UINT bind, ComPtr<ID3D11Texture2D>& tex) {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = static_cast<UINT>(width);
            d.Height = static_cast<UINT>(height);
            d.MipLevels = 1;
            d.ArraySize = 1;
            d.Format = format;
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = bind;
            tex.Reset();
            return SUCCEEDED(device->CreateTexture2D(&d, nullptr, &tex));
        };
        D3D11_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = DXGI_FORMAT_D32_FLOAT;
        dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        D3D11_SHADER_RESOURCE_VIEW_DESC depth_view{};
        depth_view.Format = DXGI_FORMAT_R32_FLOAT;
        depth_view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        depth_view.Texture2D.MipLevels = 1;

        bool ok = make_target(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, scene_hdr_tex);
        ok = ok && SUCCEEDED(device->CreateRenderTargetView(scene_hdr_tex.Get(), nullptr, scene_hdr_rtv.ReleaseAndGetAddressOf()));
        ok = ok && SUCCEEDED(device->CreateShaderResourceView(scene_hdr_tex.Get(), nullptr, scene_hdr_srv.ReleaseAndGetAddressOf()));

        ok = ok && make_target(DXGI_FORMAT_R32_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, depth_tex);
        ok = ok && SUCCEEDED(device->CreateDepthStencilView(depth_tex.Get(), &dsv, depth_dsv.ReleaseAndGetAddressOf()));
        ok = ok && SUCCEEDED(device->CreateShaderResourceView(depth_tex.Get(), &depth_view, depth_srv.ReleaseAndGetAddressOf()));

        ok = ok && make_target(DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, offscreen_color_tex);
        ok = ok && SUCCEEDED(device->CreateRenderTargetView(offscreen_color_tex.Get(), nullptr, offscreen_color_rtv.ReleaseAndGetAddressOf()));
        readback_tex.Reset();

        // The post-process chain's own targets.
        ok = ok && make_color_target(scene_hazed, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
        for (auto& p : picture) ok = ok && make_color_target(p, width, height, DXGI_FORMAT_R8G8B8A8_UNORM);
        ok = ok && make_color_target(scene_effect, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
        ok = ok && make_color_target(filter_a, width / kFilterDownsample, height / kFilterDownsample, DXGI_FORMAT_R16G16B16A16_FLOAT);
        ok = ok && make_color_target(filter_b, width / kFilterDownsample, height / kFilterDownsample, DXGI_FORMAT_R16G16B16A16_FLOAT);
        for (int i = 0; i < kMeterSteps; ++i) {
            ok = ok && make_color_target(meter[i], kMeterSizes[i], kMeterSizes[i], DXGI_FORMAT_R16G16B16A16_UNORM);
        }
        for (auto& e : exposure) ok = ok && make_color_target(e, 1, 1, DXGI_FORMAT_R32_FLOAT);
        exposure_sim_time = -1.0f;

        // Copies of the opaque scene (UE3 "resolved" SceneColor / SceneDepth) sampled by
        // translucent materials (SceneTexture, DestColor, DepthBiasedAlpha/Blend).
        ok = ok && make_target(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, scene_color_copy);
        ok = ok && SUCCEEDED(device->CreateShaderResourceView(scene_color_copy.Get(), nullptr, scene_color_copy_srv.ReleaseAndGetAddressOf()));
        ok = ok && make_target(DXGI_FORMAT_R32_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, scene_depth_copy);
        ok = ok && SUCCEEDED(device->CreateShaderResourceView(scene_depth_copy.Get(), &depth_view, scene_depth_copy_srv.ReleaseAndGetAddressOf()));

        if (ok && !shadow_depth_tex) {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = kSunShadowMapSize;
            d.Height = kSunShadowMapSize;
            d.MipLevels = 1;
            d.ArraySize = 3;  // kSunShadowNearSlice, kSunShadowFarSlice, kModShadowSlice
            d.Format = DXGI_FORMAT_R32_TYPELESS;
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
            ok = SUCCEEDED(device->CreateTexture2D(&d, nullptr, &shadow_depth_tex));
            for (UINT slice = 0; ok && slice < 3; ++slice) {
                D3D11_DEPTH_STENCIL_VIEW_DESC sv{};
                sv.Format = DXGI_FORMAT_D32_FLOAT;
                sv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
                sv.Texture2DArray.FirstArraySlice = slice;
                sv.Texture2DArray.ArraySize = 1;
                ok = SUCCEEDED(device->CreateDepthStencilView(shadow_depth_tex.Get(), &sv, &shadow_dsv[slice]));
            }
            D3D11_SHADER_RESOURCE_VIEW_DESC shv{};
            shv.Format = DXGI_FORMAT_R32_FLOAT;
            shv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            shv.Texture2DArray.MipLevels = 1;
            shv.Texture2DArray.ArraySize = 3;
            ok = ok && SUCCEEDED(device->CreateShaderResourceView(shadow_depth_tex.Get(), &shv, &shadow_srv));
            shadow_far_valid = false;
        }
        if (!ok) std::cerr << "[D3D11Renderer] Failed to allocate the render targets (" << width << "x" << height << ")" << std::endl;
        return ok;
    }

    // A texture cannot be a render target and a shader input at once, so inputs are released between passes.
    void unbind_shader_resources() {
        ID3D11ShaderResourceView* none[matbind::kMaxTextureSlots] = {};
        ctx->PSSetShaderResources(0, matbind::kMaxTextureSlots, none);
        ctx->PSSetShaderResources(matbind::kShadowMapTexture, 3, none);  // shadow map and the two scene copies
    }

    // The finished frame as RGBA8 rows, top row first.
    bool read_back(std::vector<uint8_t>& pixels) {
        if (!initialized || !offscreen_color_tex) return false;
        if (!readback_tex) {
            D3D11_TEXTURE2D_DESC d{};
            offscreen_color_tex->GetDesc(&d);
            d.Usage = D3D11_USAGE_STAGING;
            d.BindFlags = 0;
            d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(device->CreateTexture2D(&d, nullptr, &readback_tex))) return false;
        }
        ctx->CopyResource(readback_tex.Get(), offscreen_color_tex.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(ctx->Map(readback_tex.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
        const size_t row = static_cast<size_t>(width) * 4;
        pixels.resize(row * static_cast<size_t>(height));
        for (int y = 0; y < height; ++y) {
            std::memcpy(pixels.data() + static_cast<size_t>(y) * row,
                        static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch, row);
        }
        ctx->Unmap(readback_tex.Get(), 0);
        // What a window shows: the swap chain has no alpha.
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
        D3D11_TEXTURE2D_DESC d{};
        d.Width = static_cast<UINT>(dxt.width);
        d.Height = static_cast<UINT>(dxt.height);
        d.MipLevels = mipmapped ? 0 : 1;
        d.ArraySize = 1;
        d.Format = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (mipmapped ? D3D11_BIND_RENDER_TARGET : 0);
        d.MiscFlags = mipmapped ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
        UITexture t = make_texture(d, nullptr);
        if (!t) return nullptr;
        ctx->UpdateSubresource(t->texture.Get(), 0, nullptr, rgba.data(), static_cast<UINT>(dxt.width) * 4, 0);
        if (mipmapped) ctx->GenerateMips(t->srv.Get());
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

        enemy_index_buffers.clear();
        for (const std::vector<uint32_t>& list : anim_system.enemy_swat_index_lists()) {
            ComPtr<ID3D11Buffer> buffer;
            if (!list.empty()) {
                D3D11_BUFFER_DESC d{};
                d.ByteWidth = static_cast<UINT>(list.size() * sizeof(uint32_t));
                d.Usage = D3D11_USAGE_IMMUTABLE;
                d.BindFlags = D3D11_BIND_INDEX_BUFFER;
                D3D11_SUBRESOURCE_DATA init{};
                init.pSysMem = list.data();
                device->CreateBuffer(&d, &init, &buffer);
            }
            enemy_index_buffers.push_back(buffer);
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
        initialized = true;
        return true;
    }
};

// -----------------------------------------------------------------------------
// D3D11Renderer Public Interface
// -----------------------------------------------------------------------------
D3D11Renderer::D3D11Renderer() : impl_(std::make_unique<Impl>()) {}
D3D11Renderer::~D3D11Renderer() = default;
D3D11Renderer::D3D11Renderer(D3D11Renderer&&) noexcept = default;
D3D11Renderer& D3D11Renderer::operator=(D3D11Renderer&&) noexcept = default;

void D3D11Renderer::set_game_root(const std::string& game_root) { impl_->game_root = game_root; }

bool D3D11Renderer::init_headless(int width, int height) {
    impl_->headless = true;
    if (!impl_->create_device()) return false;
    if (!impl_->init_common(width, height)) return false;
    std::cout << "[D3D11Renderer] Initialized in Headless Mode (" << width << "x" << height << ") on " << impl_->adapter_name
              << std::endl;
    return true;
}

bool D3D11Renderer::init_with_window(void* hwnd, int width, int height) {
    if (!hwnd) return false;
    impl_->headless = false;
    impl_->width = width;
    impl_->height = height;
    if (!impl_->create_device()) return false;
    if (!impl_->create_swapchain(static_cast<HWND>(hwnd))) return false;
    if (!impl_->init_common(width, height)) return false;
    std::cout << "[D3D11Renderer] Initialized with a window (" << width << "x" << height << ") on " << impl_->adapter_name
              << std::endl;
    return true;
}

void D3D11Renderer::resize(int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (impl_->width == width && impl_->height == height) return;

    impl_->width = width;
    impl_->height = height;
    if (impl_->swapchain) {
        impl_->ctx->OMSetRenderTargets(0, nullptr, nullptr);
        impl_->swapchain->ResizeBuffers(0, static_cast<UINT>(width), static_cast<UINT>(height), DXGI_FORMAT_UNKNOWN, 0);
    }
    impl_->allocate_render_targets();
}

void D3D11Renderer::render_frame(const LevelScene& scene, const PlayerTelemetry& telemetry) {
    if (!impl_->initialized) return;
    Impl* const impl = impl_.get();
    ID3D11DeviceContext* const ctx = impl->ctx.Get();
    auto prof_mark = std::chrono::steady_clock::now();
    // Charges the time since the previous call to `phase` (ME_RENDER_PROF).
    auto prof = [&](Impl::ProfPhase phase) {
        if (!impl->profile) return;
        const auto now = std::chrono::steady_clock::now();
        impl->prof_seconds[phase] += std::chrono::duration<double>(now - prof_mark).count();
        prof_mark = now;
    };

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
    // Camera View & Projection Matrices (Unreal Engine to Direct3D clip space)
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
    const bool scene_hidden = impl->frontend_rgba != nullptr && impl->frontend_w > 0 && impl->frontend_h > 0;

    // The frame uniforms as the shaders see them from here on (vertex b1 and pixel b0 share one buffer).
    auto push_uniforms = [&]() { impl->update_constants(impl->cb_frame, &uniforms, sizeof(uniforms)); };
    auto set_viewport = [&](float w, float h, float min_depth, float max_depth) {
        const D3D11_VIEWPORT viewport{0.0f, 0.0f, w, h, min_depth, max_depth};
        ctx->RSSetViewports(1, &viewport);
    };
    auto use_program = [&](const Impl::Program& p) {
        ctx->IASetInputLayout(p.layout.Get());
        ctx->VSSetShader(p.vs.Get(), nullptr, 0);
        ctx->PSSetShader(p.ps.Get(), nullptr, 0);
    };
    const UINT scene_stride = sizeof(Vertex);
    const UINT zero_offset = 0;
    auto set_scene_vertices = [&](ID3D11Buffer* buffer) {
        ctx->IASetVertexBuffers(0, 1, &buffer, &scene_stride, &zero_offset);
    };
    auto draw_scene_vertices = [&](const std::vector<Vertex>& verts) {
        set_scene_vertices(impl->acquire_dynamic_vertex_buffer(verts.data(), verts.size() * sizeof(Vertex)));
        impl->draw(static_cast<UINT>(verts.size()), 0);
    };
    const float blend_factor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    auto set_blend = [&](const ComPtr<ID3D11BlendState>& state) { ctx->OMSetBlendState(state.Get(), blend_factor, 0xFFFFFFFFu); };

    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetConstantBuffers(1, 1, impl->cb_frame.GetAddressOf());
    ID3D11Buffer* pixel_constants[3] = {impl->cb_frame.Get(), impl->cb_material.Get(), impl->cb_scene.Get()};
    ctx->PSSetConstantBuffers(0, 3, pixel_constants);
    {
        // What a material's pixel shader is given beside the frame's constants: the height fog a
        // translucent surface takes, and (per dynamic object, below) its light environment.
        impl->scene_constants = SceneUniformsGPU{};
        fill_fog_uniforms(active_scene, cam_pos, impl->scene_constants.fog);
        impl->update_constants(impl->cb_scene, &impl->scene_constants, sizeof(impl->scene_constants));
        impl->light_envs.begin_frame(active_scene);
    }
    // The light environment of what is drawn next; none for the level's own geometry.
    auto set_light_env = [&](const LightEnvLighting* lighting, const Vec3& relative_to) {
        if (!light_env_uniforms(lighting, relative_to, impl->scene_constants.env)) return;
        impl->update_constants(impl->cb_scene, &impl->scene_constants, sizeof(impl->scene_constants));
    };
    impl->unbind_shader_resources();
    impl->bind_static_samplers(impl->builtin_shaders);

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
        impl->cached_mesh_buffers.clear();
        ++impl->scene_generation;  // the far shadow cascade must be redrawn from the new geometry
        for (const auto& m : active_scene.meshes) {
            ComPtr<ID3D11Buffer> b;
            if (!m.vertices.empty()) {
                D3D11_BUFFER_DESC d{};
                d.ByteWidth = static_cast<UINT>(m.vertices.size() * sizeof(Vertex));
                d.Usage = D3D11_USAGE_IMMUTABLE;
                d.BindFlags = D3D11_BIND_VERTEX_BUFFER;
                D3D11_SUBRESOURCE_DATA init{};
                init.pSysMem = m.vertices.data();
                impl->device->CreateBuffer(&d, &init, &b);
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
        for (size_t i = 0; i < active_scene.meshes.size(); ++i) {
            const auto& mesh = active_scene.meshes[i];
            auto& sflags = impl->cached_section_flags[i];
            sflags.assign(mesh.sections.size(), 0u);
            for (size_t si = 0; si < mesh.sections.size(); ++si) {
                const auto& s = mesh.sections[si];
                const MaterialShader* sh = nullptr;
                const SceneMaterial* m = nullptr;
                ID3D11PixelShader* ps = impl->section_shader(s, &sh, &m);
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
                }
                sflags[si] = fl;
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
    const bool shadow_pass = impl->shadow_depth_tex && impl->shadow_program.vs;
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

        // The device context belongs to this thread, so it decides here which enemies get posed and maps
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
            if (impl->enemy_mapped[ei]) ctx->Unmap(impl->enemy_vertex_buffers[ei].buffer.Get(), 0);
        }
    }
    prof(Impl::kProfEnemies);
    // Draws enemy ei's triangle list; it must be in_view or in_shadow.
    auto draw_enemy_mesh = [&](size_t ei) {
        const Impl::EnemyFrameDraw& draw = impl->frame_enemy_draws[ei];
        set_scene_vertices(impl->enemy_vertex_buffers[ei].buffer.Get());
        ctx->IASetIndexBuffer(impl->enemy_index_buffers[draw.index_list].Get(), DXGI_FORMAT_R32_UINT, 0);
        ctx->DrawIndexed(static_cast<UINT>(draw.index_count), 0, 0);
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
        auto render_shadow_cascade = [&](int slice, const Mat4& cascade_vp, const ComPtr<ID3D11RasterizerState>& raster,
                                         bool dynamic_casters) {
            ID3D11DepthStencilView* dsv = impl->shadow_dsv[slice].Get();
            ctx->OMSetRenderTargets(0, nullptr, dsv);
            ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);

            // shadow_vertex projects with uniforms.sun_view_proj.
            float near_vp_saved[16];
            std::memcpy(near_vp_saved, uniforms.sun_view_proj, sizeof(near_vp_saved));
            set_matrix(uniforms.sun_view_proj, cascade_vp);

            set_viewport(static_cast<float>(kSunShadowMapSize), static_cast<float>(kSunShadowMapSize), 0.0f, 1.0f);
            use_program(impl->shadow_program);
            ctx->OMSetDepthStencilState(impl->depth_write_state.Get(), 0);
            ctx->RSSetState(raster.Get());
            set_blend(impl->blend_opaque);
            push_uniforms();

            bool sh_prev_moved = false;
            for (size_t i = 0; i < active_scene.meshes.size(); ++i) {
                const auto& mesh = active_scene.meshes[i];
                if (mesh.vertices.empty() || !impl->cached_mesh_buffers[i]) continue;
                if (!dynamic_casters && (mesh.elevator >= 0 || mesh.barge_door >= 0)) continue;
                if (mesh.is_decal) continue;
                bool moved = apply_scene_mesh_model(i);
                if (moved || sh_prev_moved) push_uniforms();
                sh_prev_moved = moved;
                set_scene_vertices(impl->cached_mesh_buffers[i].Get());
                if (mesh.sections.empty()) {
                    impl->draw(static_cast<UINT>(mesh.vertices.size()), 0);
                    continue;
                }
                const auto& sflags = impl->cached_section_flags[i];
                for (size_t si = 0; si < mesh.sections.size(); ++si) {
                    const auto& s = mesh.sections[si];
                    if (!section_in_range(mesh, s)) continue;
                    if ((sflags[si] & Impl::kSecShadowCaster) == 0) continue;
                    impl->draw(s.vertex_count, s.first_vertex);
                }
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

            std::memcpy(uniforms.sun_view_proj, near_vp_saved, sizeof(near_vp_saved));
        };

        // Far cascade: static geometry only, so it stays valid until its coarsely snapped square steps
        // (the camera moved ~kSunShadowFarStep), the sun direction changes or the scene is rebuilt.
        // The main menu has no far cascade: one fixed square frames its miniature city.
        if (!in_main_menu &&
            (!impl->shadow_far_valid || impl->shadow_far_generation != impl->scene_generation ||
             std::memcmp(impl->shadow_far_vp, sun_far_vp.m, sizeof(impl->shadow_far_vp)) != 0)) {
            render_shadow_cascade(kSunShadowFarSlice, sun_far_vp, impl->raster_shadow_far, /*dynamic_casters=*/false);
            std::memcpy(impl->shadow_far_vp, sun_far_vp.m, sizeof(impl->shadow_far_vp));
            impl->shadow_far_generation = impl->scene_generation;
            impl->shadow_far_valid = true;
        }
        // Near cascade (or the menu's fixed square): every caster, every frame.
        render_shadow_cascade(kSunShadowNearSlice, sun_near_vp, impl->raster_shadow_near, /*dynamic_casters=*/true);
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
            ID3D11DepthStencilView* dsv = impl->shadow_dsv[kModShadowSlice].Get();
            ctx->OMSetRenderTargets(0, nullptr, dsv);
            ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
            float sun_vp_saved[16];
            std::memcpy(sun_vp_saved, uniforms.sun_view_proj, sizeof(sun_vp_saved));
            use_program(impl->shadow_program);
            ctx->OMSetDepthStencilState(impl->depth_write_state.Get(), 0);
            ctx->RSSetState(impl->raster_no_cull.Get());
            set_blend(impl->blend_opaque);
            for (const ModShadow& s : impl->mod_shadows) {
                const D3D11_VIEWPORT cell{static_cast<float>(s.cell_x), static_cast<float>(s.cell_y), static_cast<float>(s.resolution),
                                          static_cast<float>(s.resolution), 0.0f, 1.0f};
                ctx->RSSetViewports(1, &cell);
                set_matrix(uniforms.sun_view_proj, s.subject_matrix);  // shadow_vertex projects with it
                if (s.kind == ModShadow::Kind::Enemy) {
                    const auto& bot = active_scene.enemies[s.index];
                    set_matrix(uniforms.model, Mat4::translation(bot.position) * Mat4::rotation_z(bot.yaw_deg * DEG2RAD));
                    push_uniforms();
                    draw_enemy_mesh(s.index);
                } else if (s.kind == ModShadow::Kind::Mesh) {
                    const auto& mesh = active_scene.meshes[s.index];
                    if (!impl->cached_mesh_buffers[s.index]) continue;
                    apply_scene_mesh_model(s.index);
                    push_uniforms();
                    set_scene_vertices(impl->cached_mesh_buffers[s.index].Get());
                    if (mesh.sections.empty()) {
                        impl->draw(static_cast<UINT>(mesh.vertices.size()), 0);
                        continue;
                    }
                    const auto& sflags = impl->cached_section_flags[s.index];
                    for (size_t si = 0; si < mesh.sections.size(); ++si) {
                        const auto& sec = mesh.sections[si];
                        if (!section_in_range(mesh, sec) || (sflags[si] & Impl::kSecShadowCaster) == 0) continue;
                        impl->draw(sec.vertex_count, sec.first_vertex);
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
        ID3D11RenderTargetView* scene_rtv = impl->scene_hdr_rtv.Get();
        ctx->OMSetRenderTargets(1, &scene_rtv, impl->depth_dsv.Get());
        ctx->ClearRenderTargetView(scene_rtv, scene_clear);
        ctx->ClearDepthStencilView(impl->depth_dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
        set_viewport(fw, fh, 0.0f, 1.0f);
        auto bind_shadow_map = [&]() { ctx->PSSetShaderResources(matbind::kShadowMapTexture, 1, impl->shadow_srv.GetAddressOf()); };
        bind_shadow_map();
        set_blend(impl->blend_opaque);
        ctx->RSSetState(impl->raster_no_cull.Get());

        // A. Draw Sky Dome (TdDirHaze + Distant City Skyline)
        use_program(impl->sky_program);
        ctx->OMSetDepthStencilState(impl->depth_disabled_state.Get(), 0);
        push_uniforms();
        impl->draw(3, 0);

        // B. Draw World Meshes (BasePass + Beast Radiosity)
        set_viewport(fw, fh, 0.05f, 1.0f);
        ctx->OMSetDepthStencilState(impl->depth_write_state.Get(), 0);

        auto srv_or = [](const UITexture& t, const UITexture& fallback) -> ID3D11ShaderResourceView* {
            return t ? t->srv.Get() : (fallback ? fallback->srv.Get() : nullptr);
        };
        auto bind_world_char_wep_textures = [&](const std::string& wname, uint32_t arch_id = 0) {
            uint32_t safe_arch = (arch_id < AnimSystem::EnemyArch_Count) ? arch_id : 0;
            const auto& ctex = impl->enemy_gpu_textures[safe_arch];
            ID3D11ShaderResourceView* t_wep_d = srv_or(nullptr, impl->tex_default_white);
            ID3D11ShaderResourceView* t_wep_s = srv_or(nullptr, impl->tex_default_black);
            auto it_w = impl->weapon_gpu_textures.find(wname);
            if (it_w == impl->weapon_gpu_textures.end() && !impl->weapon_gpu_textures.empty()) {
                it_w = impl->weapon_gpu_textures.find("Colt1911");
            }
            if (it_w != impl->weapon_gpu_textures.end()) {
                if (it_w->second.diffuse)  t_wep_d = it_w->second.diffuse->srv.Get();
                if (it_w->second.specular) t_wep_s = it_w->second.specular->srv.Get();
            }
            ID3D11ShaderResourceView* srvs[6] = {
                srv_or(ctex.diffuse ? ctex.diffuse : impl->swat_d_gpu_tex, impl->tex_default_white),        // texture(0) swat_d_tex
                t_wep_d,                                                                                    // texture(1) wep_d_tex
                t_wep_s,                                                                                    // texture(2) wep_s_tex
                srv_or(impl->ammo_d_gpu_tex, impl->tex_default_white),                                      // texture(3) ammo_d_tex
                srv_or(ctex.specular ? ctex.specular : impl->swat_s_gpu_tex, impl->tex_default_black),      // texture(4) swat_s_tex
                srv_or(ctex.normal ? ctex.normal : impl->swat_n_gpu_tex, impl->tex_default_flat_normal),    // texture(5) swat_n_tex
            };
            ctx->PSSetShaderResources(0, 6, srvs);
            ctx->PSSetSamplers(0, 1, impl->mat_samplers[0][0].GetAddressOf());
        };
        // The legacy world pipeline (meshes and sections without a material shader, enemies, effects).
        auto use_world_pipeline = [&]() {
            use_program(impl->world_program);
            set_blend(impl->blend_opaque);
            ctx->RSSetState(impl->raster_no_cull.Get());
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
            set_scene_vertices(impl->cached_mesh_buffers[i].Get());
            push_uniforms();
        };
        bool decal_bias = false;  // what is being drawn is a decal buffer (MeshBuffer::is_decal)
        // Switches to a material's pipeline: its pixel shader on the shared material vertex stage.
        auto use_material_pipeline = [&](ID3D11PixelShader* ps, const MaterialShader& sh, const SceneMaterial& m, bool cull) {
            ctx->IASetInputLayout(impl->mat_input_layout.Get());
            ctx->VSSetShader(impl->mat_vertex_shader.Get(), nullptr, 0);
            ctx->PSSetShader(ps, nullptr, 0);
            switch (sh.blend) {
                case MatBlendMode::Translucent: set_blend(impl->blend_translucent); break;
                case MatBlendMode::Additive: set_blend(impl->blend_additive); break;
                case MatBlendMode::Modulate: set_blend(impl->blend_modulate); break;
                default: set_blend(impl->blend_opaque); break;
            }
            ctx->RSSetState(decal_bias ? impl->raster_decal.Get()
                                       : (cull ? impl->raster_cull_back.Get() : impl->raster_no_cull.Get()));
            impl->bind_material(m, sh);
        };

        if (!active_scene.meshes.empty()) {
            if (impl->material_shaders) impl->bind_static_samplers(*impl->material_shaders);
            std::string active_mi_tag = in_main_menu
                ? impl->main_menu.get_chapter(impl->selected_chapter).material_instance_tag
                : "";
            for (char& c : active_mi_tag) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            for (size_t i = 0; i < active_scene.meshes.size(); ++i) {
                const auto& mesh = active_scene.meshes[i];
                if (mesh.vertices.empty() || !impl->cached_mesh_buffers[i]) continue;
                decal_bias = mesh.is_decal;
                bind_scene_mesh(i);
                if (mesh.sections.empty()) {
                    use_world_pipeline();
                    impl->draw(static_cast<UINT>(mesh.vertices.size()), 0);
                    continue;
                }
                // Opaque + masked material sections (UE3 base pass). Translucent ones are deferred.
                for (const auto& s : mesh.sections) {
                    if (!section_in_range(mesh, s)) continue;
                    const MaterialShader* sh = nullptr;
                    const SceneMaterial* m = nullptr;
                    ID3D11PixelShader* ps = impl->section_shader(s, &sh, &m);
                    if (ps && mat_blend_is_translucent(sh->blend)) continue;
                    if (ps) {
                        use_material_pipeline(ps, *sh, *m, impl->mat_cull_enabled && !sh->two_sided && !in_main_menu);
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
                    impl->draw(s.vertex_count, s.first_vertex);
                }
            }
            decal_bias = false;
            set_matrix(uniforms.model, identity);
            uniforms.is_runner_vision = 0.0f;
        }

        // B2. Render 3D Articulated KrugerSec / CPF SWAT Enemies & 3D Weapons/Tracers (only during gameplay)
        // (the material sections above leave their own pipeline/depth state bound)
        use_world_pipeline();
        ctx->OMSetDepthStencilState(impl->depth_write_state.Get(), 0);
        impl->bind_static_samplers(impl->builtin_shaders);
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

        // The dynamic objects' shadows multiply the lit scene, before fog and translucency (mod_shadow.hpp).
        if (!impl->mod_shadows.empty()) {
            ModShadowUniformsGPU shadow_constants;
            fill_mod_shadow_uniforms(impl->mod_shadows, shadow_constants);
            impl->update_constants(impl->cb_post, &shadow_constants, sizeof(shadow_constants));
            ctx->PSSetConstantBuffers(1, 1, impl->cb_post.GetAddressOf());
            ctx->OMSetRenderTargets(1, &scene_rtv, nullptr);  // the depth buffer is read, not tested
            impl->unbind_shader_resources();
            ctx->PSSetShaderResources(1, 1, impl->depth_srv.GetAddressOf());
            bind_shadow_map();
            impl->bind_static_samplers(impl->builtin_shaders);
            ctx->OMSetDepthStencilState(impl->depth_disabled_state.Get(), 0);
            ctx->RSSetState(impl->raster_no_cull.Get());
            set_blend(impl->blend_modulate);
            set_viewport(fw, fh, 0.0f, 1.0f);
            use_program(impl->mod_shadow_program);
            push_uniforms();
            impl->draw(3, 0);
            impl->unbind_shader_resources();
            set_blend(impl->blend_opaque);
            ctx->OMSetRenderTargets(1, &scene_rtv, impl->depth_dsv.Get());
            set_viewport(fw, fh, 0.05f, 1.0f);
            bind_shadow_map();
            ctx->PSSetConstantBuffers(1, 1, impl->cb_material.GetAddressOf());
        }

        // Height fog over the opaque scene, before anything translucent is drawn on it
        // (HeightFogPixelShader.usf): scene * scattering + fog, read off the depth buffer.
        if (!active_scene.height_fog.empty()) {
            PostUniformsGPU fog_constants{};
            fill_post_uniforms(active_scene, view_post, cam_pos, 0.0f, false, fog_constants);
            auto push_post = [&]() { impl->update_constants(impl->cb_post, &fog_constants, sizeof(fog_constants)); };
            ctx->PSSetConstantBuffers(1, 1, impl->cb_post.GetAddressOf());
            ctx->OMSetRenderTargets(1, &scene_rtv, nullptr);  // the depth buffer is read, not tested
            impl->unbind_shader_resources();
            ctx->PSSetShaderResources(1, 1, impl->depth_srv.GetAddressOf());
            impl->bind_static_samplers(impl->builtin_shaders);
            ctx->OMSetDepthStencilState(impl->depth_disabled_state.Get(), 0);
            ctx->RSSetState(impl->raster_no_cull.Get());
            set_blend(impl->blend_fog);
            set_viewport(fw, fh, 0.0f, 1.0f);
            use_program(impl->fog_program);
            push_uniforms();
            push_post();
            impl->draw(3, 0);
            impl->unbind_shader_resources();
            set_blend(impl->blend_opaque);
            ctx->OMSetRenderTargets(1, &scene_rtv, impl->depth_dsv.Get());
            set_viewport(fw, fh, 0.05f, 1.0f);
            bind_shadow_map();
            ctx->PSSetConstantBuffers(1, 1, impl->cb_material.GetAddressOf());
        }

        // B3. Translucent / additive / modulated materials (UE3 translucency pass): drawn after
        // all opaque geometry, depth-tested without depth writes. Materials that read the scene
        // (SceneTexture, DestColor, DepthBiasedAlpha) sample copies of the opaque scene.
        if (has_translucent) {
            if (needs_scene_copies) {
                ctx->OMSetRenderTargets(0, nullptr, nullptr);
                ctx->CopyResource(impl->scene_color_copy.Get(), impl->scene_hdr_tex.Get());
                ctx->CopyResource(impl->scene_depth_copy.Get(), impl->depth_tex.Get());
                ctx->OMSetRenderTargets(1, &scene_rtv, impl->depth_dsv.Get());
            }
            ctx->OMSetDepthStencilState(impl->depth_test_only_state.Get(), 0);
            if (impl->material_shaders) impl->bind_static_samplers(*impl->material_shaders);
            // The decals first: they belong to the opaque surfaces they lie on.
            for (int decals = 1; decals >= 0; --decals)
            for (size_t i = 0; i < active_scene.meshes.size(); ++i) {
                const auto& mesh = active_scene.meshes[i];
                if (mesh.vertices.empty() || mesh.sections.empty() || !impl->cached_mesh_buffers[i]) continue;
                if (mesh.is_decal != (decals == 1)) continue;
                decal_bias = mesh.is_decal;
                bool mesh_bound = false;
                for (const auto& s : mesh.sections) {
                    if (!section_in_range(mesh, s)) continue;
                    const MaterialShader* sh = nullptr;
                    const SceneMaterial* m = nullptr;
                    ID3D11PixelShader* ps = impl->section_shader(s, &sh, &m);
                    if (!ps || !mat_blend_is_translucent(sh->blend)) continue;
                    if (!mesh_bound) {
                        bind_scene_mesh(i);
                        mesh_bound = true;
                    }
                    use_material_pipeline(ps, *sh, *m, impl->mat_cull_enabled && !sh->two_sided);
                    impl->bind_lightmap(s.lightmap);
                    impl->draw(s.vertex_count, s.first_vertex);
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
                    ID3D11PixelShader* ps = impl->section_shader(section, &sh, &m);
                    if (!ps) continue;
                    use_material_pipeline(ps, *sh, *m, false);
                    impl->bind_lightmap(-1);
                    draw_scene_vertices(b.vertices);
                }
            }
            set_matrix(uniforms.model, identity);
            set_blend(impl->blend_opaque);
            ctx->RSSetState(impl->raster_no_cull.Get());
            impl->bind_static_samplers(impl->builtin_shaders);
        }

        decal_bias = false;
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
                ctx->OMSetDepthStencilState(impl->depth_test_only_state.Get(), 0);
                impl->bind_static_samplers(*impl->material_shaders);
                for (const LensFlareQuad& q : impl->flare_quads) {
                    MeshSection section;
                    section.material = q.material;
                    const MaterialShader* sh = nullptr;
                    const SceneMaterial* m = nullptr;
                    ID3D11PixelShader* ps = impl->section_shader(section, &sh, &m);
                    if (!ps) continue;
                    use_material_pipeline(ps, *sh, *m, false);
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
                ctx->RSSetState(impl->raster_no_cull.Get());
                impl->bind_static_samplers(impl->builtin_shaders);
            }
        }

        prof(Impl::kProfTranslucent);

        // C. Draw First-Person Faith Viewmodel (CH_Faith_1P in DPG_Foreground depth range [0.0, 0.05])
        if (!impl->menu_open && !bink_video_active) {
            if (!impl->viewmodel_built) impl->build_faith_viewmodel(telemetry);
            if (!impl->faith_viewmodel_mesh.empty()) {
                set_viewport(fw, fh, 0.0f, 0.05f);
                use_program(impl->viewmodel_program);
                ctx->OMSetDepthStencilState(impl->depth_write_state.Get(), 0);
                set_matrix(uniforms.view_proj, vm_vp);
                set_matrix(uniforms.model, identity);
                uniforms.camera_pos = Float3(0.0f, 0.0f, 0.0f);
                // Its lights are given around the eye, as the mesh is.
                set_light_env(&impl->light_envs.first_person(active_scene, telemetry.position, telemetry.sim_time), cam_pos);
                uniforms.is_runner_vision = 0.0f;
                uniforms.actor_tint = Float3(1.0f, 1.0f, 1.0f);

                ID3D11ShaderResourceView* t_wep_d = srv_or(nullptr, impl->tex_default_white);
                ID3D11ShaderResourceView* t_wep_s = srv_or(nullptr, impl->tex_default_black);
                ID3D11ShaderResourceView* t_wep_n = srv_or(nullptr, impl->tex_default_flat_normal);
                ID3D11ShaderResourceView* t_wep_m = srv_or(nullptr, impl->tex_default_white);
                auto it_w = impl->weapon_gpu_textures.find(telemetry.weapon.name);
                if (it_w == impl->weapon_gpu_textures.end() && !impl->weapon_gpu_textures.empty()) {
                    it_w = impl->weapon_gpu_textures.find("Colt1911");
                }
                if (it_w != impl->weapon_gpu_textures.end()) {
                    if (it_w->second.diffuse)  t_wep_d = it_w->second.diffuse->srv.Get();
                    if (it_w->second.specular) t_wep_s = it_w->second.specular->srv.Get();
                    if (it_w->second.normal)   t_wep_n = it_w->second.normal->srv.Get();
                    if (it_w->second.mask)     t_wep_m = it_w->second.mask->srv.Get();
                }
                ID3D11ShaderResourceView* srvs[8] = {
                    srv_or(impl->vm_skin_gpu_tex, impl->tex_default_white),   // texture(0) vm_skin_tex
                    srv_or(impl->vm_glove_gpu_tex, impl->tex_default_white),  // texture(1) vm_glove_tex
                    srv_or(impl->vm_lower_gpu_tex, impl->tex_default_white),  // texture(2) vm_lower_tex
                    t_wep_d,                                                  // texture(3) vm_wep_d_tex
                    t_wep_s,                                                  // texture(4) vm_wep_s_tex
                    t_wep_n,                                                  // texture(5) vm_wep_n_tex
                    srv_or(impl->ammo_d_gpu_tex, impl->tex_default_white),    // texture(6) vm_ammo_tex
                    t_wep_m,                                                  // texture(7) vm_wep_m_tex
                };
                ctx->PSSetShaderResources(0, 8, srvs);
                ctx->PSSetSamplers(0, 1, impl->mat_samplers[0][0].GetAddressOf());

                push_uniforms();
                draw_scene_vertices(impl->faith_viewmodel_mesh);
                uniforms.camera_pos = cam_pos;
            }
        }
    }

    // ---------------------------------------------------------------------
    // Pass 2: Post-Processing, SSAO & Tone Mapping (HDR -> Final Output)
    // ---------------------------------------------------------------------
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    ID3D11RenderTargetView* final_rtv = impl->offscreen_color_rtv.Get();
    ctx->OMSetRenderTargets(1, &final_rtv, nullptr);
    ctx->ClearRenderTargetView(final_rtv, black);
    impl->unbind_shader_resources();
    set_viewport(fw, fh, 0.0f, 1.0f);
    ctx->OMSetDepthStencilState(impl->depth_disabled_state.Get(), 0);
    ctx->RSSetState(impl->raster_no_cull.Get());
    set_blend(impl->blend_opaque);
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
            for (auto& e : impl->exposure) ctx->ClearRenderTargetView(e.rtv.Get(), start);
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
        post.fade[0] = telemetry.fade_color.x;
        post.fade[1] = telemetry.fade_color.y;
        post.fade[2] = telemetry.fade_color.z;
        post.fade[3] = telemetry.fade_amount;
        post.motion[0] = motion_amount;
        impl->exposure_sim_time = telemetry.sim_time;
        impl->exposure_map = active_scene.map_name;
        auto push_post = [&]() { impl->update_constants(impl->cb_post, &post, sizeof(post)); };
        auto post_pass = [&](const Impl::Program& program, const Impl::ColorTarget& target, std::initializer_list<ID3D11ShaderResourceView*> inputs) {
            ID3D11RenderTargetView* rtv = target.rtv.Get();
            ctx->OMSetRenderTargets(1, &rtv, nullptr);
            set_viewport(static_cast<float>(target.w), static_cast<float>(target.h), 0.0f, 1.0f);
            impl->unbind_shader_resources();
            UINT slot = 0;
            for (ID3D11ShaderResourceView* srv : inputs) ctx->PSSetShaderResources(slot++, 1, &srv);
            use_program(program);
            push_post();
            impl->draw(3, 0);
        };
        ctx->PSSetConstantBuffers(1, 1, impl->cb_post.GetAddressOf());
        ctx->PSSetSamplers(0, 1, impl->linear_sampler.GetAddressOf());
        impl->bind_static_samplers(impl->builtin_shaders);
        push_uniforms();

        post.texel[0] = 1.0f / fw;
        post.texel[1] = 1.0f / fh;
        post_pass(impl->haze_program, impl->scene_hazed, {impl->scene_hdr_srv.Get(), impl->depth_srv.Get()});
        post_pass(impl->bloom_gather_program, impl->filter_a, {impl->scene_hazed.srv.Get(), impl->depth_srv.Get()});
        fill_filter_taps(impl->width, impl->filter_a.w, impl->filter_a.h, /*horizontal=*/true, post);
        post_pass(impl->filter_program, impl->filter_b, {impl->filter_a.srv.Get()});
        fill_filter_taps(impl->width, impl->filter_a.w, impl->filter_a.h, /*horizontal=*/false, post);
        post_pass(impl->filter_program, impl->filter_a, {impl->filter_b.srv.Get()});

        // The metering: the scene with its bloom into 512 x 512, then down to one texel.
        post.texel[0] = 1.0f / fw;
        post.texel[1] = 1.0f / fh;
        post.texel[2] = static_cast<float>(meter_taps(impl->width, kMeterSizes[0]));
        post_pass(impl->meter_scene_program, impl->meter[0],
                  {impl->scene_hazed.srv.Get(), impl->depth_srv.Get(), impl->filter_a.srv.Get()});
        for (int i = 1; i < kMeterSteps; ++i) {
            post.texel[0] = post.texel[1] = 1.0f / static_cast<float>(kMeterSizes[i - 1]);
            post.texel[2] = static_cast<float>(meter_taps(kMeterSizes[i - 1], kMeterSizes[i]));
            post_pass(impl->meter_program, impl->meter[i], {impl->meter[i - 1].srv.Get()});
        }
        post.texel[0] = 1.0f / fw;
        post.texel[1] = 1.0f / fh;
        const int previous = impl->exposure_current;
        impl->exposure_current = 1 - previous;
        post_pass(impl->exposure_program, impl->exposure[impl->exposure_current],
                  {impl->meter[kMeterSteps - 1].srv.Get(), impl->exposure[previous].srv.Get()});

        // A material effect of the chain: its material over the whole target, through the material
        // vertex stage with nothing to transform, reading `source` as the scene colour.
        auto effect_pass = [&](const PostEffectInfo& fx, const ScreenEffect* state, ID3D11ShaderResourceView* source,
                               const Impl::ColorTarget& target) -> bool {
            MeshSection section;
            section.material = fx.material;
            const MaterialShader* sh = nullptr;
            const SceneMaterial* m = nullptr;
            ID3D11PixelShader* ps = impl->section_shader(section, &sh, &m);
            if (!ps) return false;
            ID3D11RenderTargetView* rtv = target.rtv.Get();
            ctx->OMSetRenderTargets(1, &rtv, nullptr);
            set_viewport(static_cast<float>(target.w), static_cast<float>(target.h), 0.0f, 1.0f);
            impl->unbind_shader_resources();
            ctx->IASetInputLayout(impl->mat_input_layout.Get());
            ctx->VSSetShader(impl->mat_vertex_shader.Get(), nullptr, 0);
            ctx->PSSetShader(ps, nullptr, 0);
            if (impl->material_shaders) impl->bind_static_samplers(*impl->material_shaders);
            ctx->PSSetConstantBuffers(1, 1, impl->cb_material.GetAddressOf());
            impl->bind_material(*m, *sh);
            ID3D11ShaderResourceView* seen[2] = {source, impl->depth_srv.Get()};
            ctx->PSSetShaderResources(matbind::kSceneColorTexture, 2, seen);
            ctx->PSSetSamplers(matbind::kSceneSampler, 1, impl->scene_copy_sampler.GetAddressOf());
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
            ctx->PSSetConstantBuffers(1, 1, impl->cb_post.GetAddressOf());
            ctx->PSSetSamplers(0, 1, impl->linear_sampler.GetAddressOf());
            impl->bind_static_samplers(impl->builtin_shaders);
            return true;
        };
        const auto effect_state = [&](const PostEffectInfo& fx) -> const ScreenEffect* {
            for (const ScreenEffect& e : telemetry.screen_effects) {
                if (e.name == fx.name) return &e;
            }
            return nullptr;
        };

        // The effects that stand before the tone mapper work on the scene itself.
        ID3D11ShaderResourceView* scene_for_tone = impl->scene_hazed.srv.Get();
        for (const PostEffectInfo& fx : active_scene.post_effects) {
            if (fx.after_tone_mapping) continue;
            const ScreenEffect* state = effect_state(fx);
            if (!state) continue;
            // one such effect at a time is all the game ever shows; a second would need a third buffer
            if (scene_for_tone != impl->scene_hazed.srv.Get()) break;
            if (effect_pass(fx, state, scene_for_tone, impl->scene_effect)) scene_for_tone = impl->scene_effect.srv.Get();
        }

        // The tone-mapped picture, then the passes that work on it in turn; the last writes the frame.
        post_pass(impl->tonemap_program, impl->picture[0],
                  {scene_for_tone, impl->depth_srv.Get(), impl->filter_a.srv.Get(),
                   impl->exposure[impl->exposure_current].srv.Get()});
        int picture_now = 0;
        for (const PostEffectInfo& fx : active_scene.post_effects) {
            if (!fx.after_tone_mapping) continue;
            const ScreenEffect* state = effect_state(fx);
            if (!state) continue;
            if (effect_pass(fx, state, impl->picture[picture_now].srv.Get(), impl->picture[1 - picture_now])) {
                picture_now = 1 - picture_now;
            }
        }

        ctx->OMSetRenderTargets(1, &final_rtv, nullptr);
        set_viewport(fw, fh, 0.0f, 1.0f);
        impl->unbind_shader_resources();
        ctx->PSSetShaderResources(0, 1, impl->picture[picture_now].srv.GetAddressOf());
        use_program(impl->finish_program);
        push_post();
        impl->draw(3, 0);
        ctx->PSSetConstantBuffers(1, 1, impl->cb_material.GetAddressOf());
    }

    // ---------------------------------------------------------------------
    // Pass 3: 2D HUD, Cutscene Video/Letterbox Overlay & Frontend UI
    // ---------------------------------------------------------------------
    impl->unbind_shader_resources();
    set_blend(impl->blend_ui);
    const float screen_size[4] = {fw, fh, 0.0f, 0.0f};
    impl->update_constants(impl->cb_screen, screen_size, sizeof(screen_size));
    ctx->VSSetConstantBuffers(1, 1, impl->cb_screen.GetAddressOf());
    ctx->PSSetSamplers(0, 1, impl->linear_sampler.GetAddressOf());

    auto draw_hud_vertices = [&](const std::vector<HUDVertex>& verts) {
        if (verts.empty()) return;
        use_program(impl->hud_program);
        ID3D11Buffer* buffer = impl->acquire_dynamic_vertex_buffer(verts.data(), verts.size() * sizeof(HUDVertex));
        const UINT stride = sizeof(HUDVertex);
        ctx->IASetVertexBuffers(0, 1, &buffer, &stride, &zero_offset);
        impl->draw(static_cast<UINT>(verts.size()), 0);
    };
    auto draw_ui_tex_vertices = [&](const UITexture& tex, const std::vector<UITexVertex>& verts) {
        if (!tex || verts.empty()) return;
        use_program(impl->ui_tex_program);
        ctx->PSSetShaderResources(0, 1, tex->srv.GetAddressOf());
        ID3D11Buffer* buffer = impl->acquire_dynamic_vertex_buffer(verts.data(), verts.size() * sizeof(UITexVertex));
        const UINT stride = sizeof(UITexVertex);
        ctx->IASetVertexBuffers(0, 1, &buffer, &stride, &zero_offset);
        impl->draw(static_cast<UINT>(verts.size()), 0);
    };
    // A full-screen picture (a front end frame, a Bink frame) aspect-fitted over a black backdrop.
    auto draw_fitted_picture = [&](const UITexture& tex, int src_w, int src_h) {
        std::vector<HUDVertex> black_bg;
        impl->draw_ui_quad(black_bg, 0.0f, 0.0f, fw, fh, ui_color(0.0f, 0.0f, 0.0f, 1.0f));
        draw_hud_vertices(black_bg);

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

    if (scene_hidden) {
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
            draw_fitted_picture(impl->frontend_tex, src_w, src_h);
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
            // A level's own cutscene carries what retail shows over it: the skip prompt, top
            // left, when the Matinee is skippable (telemetry.skip_prompt), and the script's text.
            impl->draw_script_text(cs_hud, telemetry);
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
    ctx->OMSetRenderTargets(0, nullptr, nullptr);

    prof(Impl::kProfOverlays);
    if (impl->swapchain) {
        ComPtr<ID3D11Texture2D> back_buffer;
        if (SUCCEEDED(impl->swapchain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) {
            ctx->CopyResource(back_buffer.Get(), impl->offscreen_color_tex.Get());
        }
        impl->swapchain->Present(1, 0);
    } else {
        ctx->Flush();
    }
    impl->drain_debug_messages();

    prof(Impl::kProfPresent);
    if (impl->profile && ++impl->prof_frames == 120) {
        std::fprintf(stderr, "[D3D11Renderer] 120 frames, ms per frame:");
        for (int i = 0; i < Impl::kProfPhases; ++i) {
            std::fprintf(stderr, " %s %.2f", Impl::kProfNames[i], impl->prof_seconds[i] / 120.0 * 1000.0);
            impl->prof_seconds[i] = 0.0;
        }
        std::fprintf(stderr, "; %llu draws, %.1f of %zu enemies posed\n", static_cast<unsigned long long>(impl->prof_draws / 120),
                     static_cast<double>(impl->prof_posed.exchange(0)) / 120.0, active_scene.enemies.size());
        impl->prof_frames = 0;
        impl->prof_draws = 0;
    }

    impl->frame_index++;
}

bool D3D11Renderer::save_screenshot_ppm(const std::string& path) {
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

    std::cout << "[D3D11Renderer] Exported PPM screenshot: " << path << " (" << w << "x" << h << ")" << std::endl;
    return true;
}

bool D3D11Renderer::save_screenshot_png(const std::string& path) {
    std::vector<uint8_t> pixels;
    if (!impl_->read_back(pixels)) return false;
    const bool success = fe::write_png(path, impl_->width, impl_->height, pixels.data());
    if (success) {
        std::cout << "[D3D11Renderer] Exported PNG screenshot: " << path << " (" << impl_->width << "x" << impl_->height << ")"
                  << std::endl;
    } else {
        std::cerr << "[D3D11Renderer] Failed to export PNG: " << path << std::endl;
    }
    return success;
}

void D3D11Renderer::set_world_trace(std::function<bool(const Vec3& from, const Vec3& to, Vec3& hit, Vec3& normal)> trace) {
    impl_->anim_system.set_world_trace(std::move(trace));
}

void D3D11Renderer::player_camera(const PlayerTelemetry& telemetry, Vec3& out_pos, Rotator& out_rot) const {
    // Without the character assets camera_animation() plays nothing: the plain eyes and view rotation.
    impl_->anim_system.player_camera(telemetry, out_pos, out_rot);
}

bool D3D11Renderer::is_initialized() const { return impl_->initialized; }
bool D3D11Renderer::is_headless() const { return impl_->headless; }
int D3D11Renderer::width() const { return impl_->width; }
int D3D11Renderer::height() const { return impl_->height; }
uint64_t D3D11Renderer::frame_count() const { return impl_->frame_index; }

void D3D11Renderer::set_menu_open(bool open) { impl_->menu_open = open; }
bool D3D11Renderer::is_menu_open() const { return impl_->menu_open; }
void D3D11Renderer::set_selected_chapter(int idx) { impl_->selected_chapter = std::clamp(idx, 0, 9); }
int D3D11Renderer::selected_chapter() const { return impl_->selected_chapter; }
void D3D11Renderer::set_selected_menu_tab(int tab) { impl_->selected_menu_tab = std::clamp(tab, 0, 3); }
int D3D11Renderer::selected_menu_tab() const { return impl_->selected_menu_tab; }
void D3D11Renderer::set_selected_menu_row(int row) { impl_->selected_menu_row = std::clamp(row, 0, 9); }
int D3D11Renderer::selected_menu_row() const { return impl_->selected_menu_row; }
void D3D11Renderer::set_menu_options_state(int sens_pct, int fov_deg, bool fullscreen) {
    impl_->opt_sens_pct = sens_pct;
    impl_->opt_fov_deg = fov_deg;
    impl_->opt_fullscreen = fullscreen;
}
void D3D11Renderer::set_cutscene_player(const CutscenePlayer* player) { impl_->cutscene_player = player; }

void D3D11Renderer::set_frontend_frame(const uint8_t* rgba, int width, int height) {
    impl_->frontend_rgba = rgba;
    impl_->frontend_w = width;
    impl_->frontend_h = height;
}

void* D3D11Renderer::raw_device() const { return impl_->device.Get(); }

}  // namespace me
