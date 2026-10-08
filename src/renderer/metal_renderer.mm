#include "metal_renderer.hpp"
#include "builtin_shaders_msl.hpp"
#include "hud_font.hpp"
#include "render_common.hpp"
#include "sun_shadow.hpp"
#include "../anim/anim_system.hpp"
#include "../assets/scene_materials.hpp"
#include "../cutscene/cutscene_player.hpp"
#include "../ui/main_menu.hpp"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <simd/simd.h>

#include <iostream>
#include <fstream>
#include <vector>
#include <cmath>
#include <string>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <thread>

namespace me {

// -----------------------------------------------------------------------------
// MSL Shader Source Code: renderer/builtin_shaders_msl.hpp (shared with the Direct3D 11 renderer)
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Internal Uniforms Structure for Shader Binding
// -----------------------------------------------------------------------------
struct PackedFloat3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    constexpr PackedFloat3() = default;
    constexpr PackedFloat3(float in_x, float in_y, float in_z) : x(in_x), y(in_y), z(in_z) {}
    PackedFloat3(simd_float3 v) : x(v.x), y(v.y), z(v.z) {}
    operator simd_float3() const { return simd_make_float3(x, y, z); }
};

struct FrameUniformsGPU {
    simd_float4x4 view_proj;
    simd_float4x4 model;
    PackedFloat3 camera_pos;
    float sim_time;
    PackedFloat3 sun_dir;
    float runner_vision_strength;
    PackedFloat3 sun_color;
    float exposure;
    PackedFloat3 sky_color;
    float speed_2d;
    PackedFloat3 ground_color;
    float reaction_active;
    PackedFloat3 actor_tint;
    float is_runner_vision;
    PackedFloat3 cam_forward;
    float fov_tan;
    PackedFloat3 cam_right;
    float aspect;
    PackedFloat3 cam_up;
    float health;
    simd_float4x4 sun_view_proj;
    PackedFloat3 mod_shadow_color;
    float shadow_enabled;
    simd_float4x4 sun_view_proj_far;
};

struct HUDVertex {
    simd_float2 position;
    simd_float4 color;
};

struct UITexVertex {
    simd_float2 position;
    simd_float2 uv;
    simd_float4 color;
};

// The names the shared overlay code (renderer/overlay_ui.inl) is written against.
using UIColor = simd_float4;
static inline UIColor ui_color(float r, float g, float b, float a) { return simd_make_float4(r, g, b, a); }
using UITexture = id<MTLTexture>;
static inline float ui_texture_width(UITexture t) { return static_cast<float>(t.width); }
static inline float ui_texture_height(UITexture t) { return static_cast<float>(t.height); }

// -----------------------------------------------------------------------------
// Material System GPU Helpers (scene_materials.hpp -> Metal)
// -----------------------------------------------------------------------------
static MTLPixelFormat mtl_pixel_format(TexFormat f, bool srgb) {
    switch (f) {
        case TexFormat::DXT1: return srgb ? MTLPixelFormatBC1_RGBA_sRGB : MTLPixelFormatBC1_RGBA;
        case TexFormat::DXT3: return srgb ? MTLPixelFormatBC2_RGBA_sRGB : MTLPixelFormatBC2_RGBA;
        case TexFormat::DXT5: return srgb ? MTLPixelFormatBC3_RGBA_sRGB : MTLPixelFormatBC3_RGBA;
        case TexFormat::BGRA8: return srgb ? MTLPixelFormatBGRA8Unorm_sRGB : MTLPixelFormatBGRA8Unorm;
        case TexFormat::G8: return srgb ? MTLPixelFormatR8Unorm_sRGB : MTLPixelFormatR8Unorm;
        case TexFormat::V8U8: return MTLPixelFormatRG8Snorm;
        default: return MTLPixelFormatInvalid;
    }
}

static MTLSamplerAddressMode mtl_address_mode(TexAddress a) {
    switch (a) {
        case TexAddress::Clamp: return MTLSamplerAddressModeClampToEdge;
        case TexAddress::Mirror: return MTLSamplerAddressModeMirrorRepeat;
        default: return MTLSamplerAddressModeRepeat;
    }
}

// -----------------------------------------------------------------------------
// PIMPL Implementation
// -----------------------------------------------------------------------------
struct MetalRenderer::Impl {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> command_queue = nil;
    id<MTLLibrary> shader_library = nil;

    // Render Pipeline States
    id<MTLRenderPipelineState> shadow_pipeline = nil;
    id<MTLRenderPipelineState> sky_pipeline = nil;
    id<MTLRenderPipelineState> world_pipeline = nil;
    id<MTLRenderPipelineState> viewmodel_pipeline = nil;
    id<MTLRenderPipelineState> post_pipeline = nil;
    id<MTLRenderPipelineState> hud_pipeline = nil;
    id<MTLRenderPipelineState> ui_tex_pipeline = nil;

    // Depth Stencil States
    id<MTLDepthStencilState> depth_write_state = nil;
    id<MTLDepthStencilState> depth_test_only_state = nil;
    id<MTLDepthStencilState> depth_disabled_state = nil;

    // Texture Sampler
    id<MTLSamplerState> linear_sampler = nil;

    // Framebuffer targets
    CAMetalLayer* metal_layer = nil;
    id<MTLTexture> offscreen_color_tex = nil;
    id<MTLTexture> offscreen_depth_tex = nil;
    id<MTLTexture> scene_hdr_tex = nil; // Intermediate HDR buffer for tone mapping
    id<MTLTexture> shadow_depth_tex = nil; // sun shadow cascades: 2-slice 4096x4096 depth array (renderer/sun_shadow.hpp)
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
    std::string game_root = "/Users/tomnom/mirrorsedge";  // set_game_root()

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
    id<MTLTexture> ui_logo_tex = nil;
    id<MTLTexture> ui_bag_tex = nil;
    id<MTLTexture> ui_time_tex = nil;
    id<MTLTexture> ui_panel_bg_tex = nil;
    id<MTLTexture> ui_faith_art_tex = nil;
    id<MTLTexture> ui_chapter_tex[10] = {nil};
    std::vector<id<MTLTexture>> ui_font_headline_thick_tex;
    std::vector<id<MTLTexture>> ui_font_headline_light_tex;
    std::vector<id<MTLTexture>> ui_font_medium_italic_tex;
    std::vector<id<MTLTexture>> ui_font_small_italic_tex;

    struct UITextureBatch {
        id<MTLTexture> tex = nil;
        std::vector<UITexVertex> verts;
    };

    // First-Person Faith Viewmodel Mesh & 3D Enemy Guard Mesh
    std::vector<Vertex> faith_viewmodel_mesh;
    std::vector<Vertex> enemy_guard_mesh;
    // This frame's posed enemies (index = enemy index). Each one's triangle list (exactly evaluate_enemy_swat()'s)
    // is written to enemy_vertex_buffers[slot][i] only when the enemy can reach a pass's render target.
    struct EnemyFrameDraw {
        size_t corner_count = 0;  // triangle-list vertices (0 = nothing to draw)
        uint32_t archetype_id = 0;
        bool in_view = false;     // may cover pixels of the camera view (world pass)
        bool in_shadow = false;   // may cover texels of the near shadow cascade (shadow pass)
    };
    std::vector<EnemyFrameDraw> frame_enemy_draws;
    AnimSystem anim_system;

    // Triple-buffered CPU/GPU frame synchronization & reusable dynamic vertex buffers
    static constexpr int kMaxFramesInFlight = 3;
    dispatch_semaphore_t in_flight_sem = dispatch_semaphore_create(kMaxFramesInFlight);
    id<MTLCommandBuffer> last_cmd_buffer = nil;
    std::vector<id<MTLBuffer>> dyn_vertex_buffers[kMaxFramesInFlight];
    size_t dyn_vertex_cursor[kMaxFramesInFlight] = {0, 0, 0};
    // Per-frame-slot GPU vertex buffers for the posed enemies (index = enemy index): each worker writes its enemy's
    // triangle list straight into this memory, and the shadow and world passes draw from the same buffer.
    std::vector<id<MTLBuffer>> enemy_vertex_buffers[kMaxFramesInFlight];

    // Vertex data up to this size is passed inline with setVertexBytes instead of through a buffer.
    static constexpr size_t kMaxInlineVertexBytes = 4096;

    static size_t dynamic_buffer_capacity(size_t length) {
        return std::max<size_t>((length + 4095u) & ~size_t(4095u), 65536u);
    }

    id<MTLBuffer> acquire_dynamic_vertex_buffer(int slot, const void* data, size_t length) {
        auto& pool = dyn_vertex_buffers[slot];
        size_t idx = dyn_vertex_cursor[slot]++;
        if (idx >= pool.size()) {
            pool.push_back(nil);
        }
        id<MTLBuffer> buf = pool[idx];
        if (!buf || buf.length < length) {
            buf = [device newBufferWithLength:dynamic_buffer_capacity(length) options:MTLResourceStorageModeShared];
            pool[idx] = buf;
        }
        std::memcpy(buf.contents, data, length);
        return buf;
    }

    ~Impl() {
        if (last_cmd_buffer) {
            [last_cmd_buffer waitUntilCompleted];
            last_cmd_buffer = nil;
        }
    }

    // Cutscene Bink Video & Matinee Renderer State
    const CutscenePlayer* cutscene_player = nullptr;
    id<MTLTexture> bink_video_tex = nil;
    uint64_t bink_uploaded_serial = 0;

    // The CPU-rendered front end frame (set_frontend_frame), uploaded every frame it is set.
    const uint8_t* frontend_rgba = nullptr;
    int frontend_w = 0;
    int frontend_h = 0;
    id<MTLTexture> frontend_tex = nil;

    // Cached GPU Vertex Buffers & Per-Section Material/Shadow Metadata for Scene Meshes
    std::string cached_map_name;
    size_t cached_total_verts = 0;
    std::vector<id<MTLBuffer>> cached_mesh_buffers;
    std::shared_ptr<const SceneMaterialLibrary> cached_section_mat_lib;
    bool cached_has_translucent = false;
    bool cached_needs_scene_copies = false;
    static constexpr uint8_t kSecShadowCaster = 1u << 0;
    static constexpr uint8_t kSecTranslucent  = 1u << 1;
    std::vector<std::vector<uint8_t>> cached_section_flags;

    // -------------------------------------------------------------------------
    // Mirror's Edge material system (LevelScene::materials) GPU cache
    // -------------------------------------------------------------------------
    std::shared_ptr<const SceneMaterialLibrary> mat_lib;     // library currently resident on the GPU
    std::vector<id<MTLTexture>> mat_textures;                // per SceneTexture (nil = missing -> default)
    std::vector<id<MTLRenderPipelineState>> mat_pipelines;   // per MaterialShader (nil = failed -> legacy)
    id<MTLTexture> tex_default_white = nil;
    id<MTLTexture> tex_default_flat_normal = nil;
    id<MTLTexture> tex_default_black = nil;
    id<MTLTexture> tex_default_cube = nil;
    id<MTLSamplerState> mat_samplers[3][3];                  // [TexAddress X][TexAddress Y]
    id<MTLSamplerState> mat_cube_sampler = nil;
    id<MTLSamplerState> scene_copy_sampler = nil;
    id<MTLTexture> scene_color_copy = nil;                   // opaque scene color (SceneTexture / DestColor)
    id<MTLTexture> scene_depth_copy = nil;                   // opaque scene depth (SceneDepth / DepthBiased*)

    // UE3 culls back faces of non-two-sided materials. ME_CULL=off|cw|ccw overrides (debugging).
    // With this renderer's view/projection, UE3 front faces are counter-clockwise on screen
    // (verified: CCW culling only removes hidden back faces, CW culling removes ~16-58% of the image).
    bool mat_cull_enabled = true;
    MTLWinding mat_front_winding = MTLWindingCounterClockwise;

    void configure_material_culling() {
        if (const char* env = std::getenv("ME_CULL")) {
            const std::string v = env;
            if (v == "0" || v == "off" || v == "none") {
                mat_cull_enabled = false;
            } else if (v == "ccw") {
                mat_front_winding = MTLWindingCounterClockwise;
            } else if (v == "cw") {
                mat_front_winding = MTLWindingClockwise;
            }
        }
    }

    MTLCompileOptions* make_compile_options() {
        MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
        if (@available(macOS 15.0, *)) {
            options.mathMode = MTLMathModeFast;
        }
        if (@available(macOS 11.0, *)) {
            options.languageVersion = MTLLanguageVersion2_4;
        }
        return options;
    }

    id<MTLTexture> make_solid_texture(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                     width:4
                                                                                    height:4
                                                                                 mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead;
        d.storageMode = MTLStorageModeShared;
        id<MTLTexture> t = [device newTextureWithDescriptor:d];
        uint8_t px[4 * 4 * 4];
        for (int i = 0; i < 16; ++i) {
            px[i * 4 + 0] = r;
            px[i * 4 + 1] = g;
            px[i * 4 + 2] = b;
            px[i * 4 + 3] = a;
        }
        [t replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0 withBytes:px bytesPerRow:16];
        return t;
    }

    void create_material_defaults() {
        configure_material_culling();
        tex_default_white = make_solid_texture(255, 255, 255, 255);
        tex_default_flat_normal = make_solid_texture(128, 128, 255, 255);
        tex_default_black = make_solid_texture(0, 0, 0, 255);

        constexpr int kCubeDim = 64;
        MTLTextureDescriptor* cd = [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB
                                                                                         size:kCubeDim
                                                                                    mipmapped:YES];
        cd.usage = MTLTextureUsageShaderRead;
        cd.storageMode = MTLStorageModeShared;
        tex_default_cube = [device newTextureWithDescriptor:cd];
        for (NSUInteger face = 0; face < 6; ++face) {
            int dim = kCubeDim;
            NSUInteger mip = 0;
            for (;;) {
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
                        dx /= len; dy /= len; dz /= len;
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
                        size_t idx = (static_cast<size_t>(y) * static_cast<size_t>(dim) + static_cast<size_t>(x)) * 4;
                        px[idx + 0] = static_cast<uint8_t>(std::clamp(r, 0.0f, 255.0f));
                        px[idx + 1] = static_cast<uint8_t>(std::clamp(g, 0.0f, 255.0f));
                        px[idx + 2] = static_cast<uint8_t>(std::clamp(b, 0.0f, 255.0f));
                        px[idx + 3] = 255;
                    }
                }
                [tex_default_cube replaceRegion:MTLRegionMake2D(0, 0, static_cast<NSUInteger>(dim), static_cast<NSUInteger>(dim))
                                    mipmapLevel:mip
                                          slice:face
                                      withBytes:px.data()
                                    bytesPerRow:static_cast<NSUInteger>(dim) * 4
                                  bytesPerImage:px.size()];
                if (dim == 1) break;
                dim = std::max(1, dim / 2);
                mip++;
            }
        }

        // UE3 samples each texture with its own AddressX/AddressY and trilinear/anisotropic filtering.
        for (int x = 0; x < 3; ++x) {
            for (int y = 0; y < 3; ++y) {
                MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
                sd.minFilter = MTLSamplerMinMagFilterLinear;
                sd.magFilter = MTLSamplerMinMagFilterLinear;
                sd.mipFilter = MTLSamplerMipFilterLinear;
                sd.maxAnisotropy = 8;
                sd.sAddressMode = mtl_address_mode(static_cast<TexAddress>(x));
                sd.tAddressMode = mtl_address_mode(static_cast<TexAddress>(y));
                sd.rAddressMode = MTLSamplerAddressModeRepeat;
                mat_samplers[x][y] = [device newSamplerStateWithDescriptor:sd];
            }
        }
        MTLSamplerDescriptor* cs = [[MTLSamplerDescriptor alloc] init];
        cs.minFilter = MTLSamplerMinMagFilterLinear;
        cs.magFilter = MTLSamplerMinMagFilterLinear;
        cs.mipFilter = MTLSamplerMipFilterLinear;
        cs.sAddressMode = MTLSamplerAddressModeClampToEdge;
        cs.tAddressMode = MTLSamplerAddressModeClampToEdge;
        cs.rAddressMode = MTLSamplerAddressModeClampToEdge;
        mat_cube_sampler = [device newSamplerStateWithDescriptor:cs];

        MTLSamplerDescriptor* ss = [[MTLSamplerDescriptor alloc] init];
        ss.minFilter = MTLSamplerMinMagFilterLinear;
        ss.magFilter = MTLSamplerMinMagFilterLinear;
        ss.sAddressMode = MTLSamplerAddressModeClampToEdge;
        ss.tAddressMode = MTLSamplerAddressModeClampToEdge;
        scene_copy_sampler = [device newSamplerStateWithDescriptor:ss];
    }

    // Uploads one decoded UE3 texture (2D or cube, full mip chain) as a shared Metal texture.
    id<MTLTexture> upload_scene_texture(const SceneTexture& st) {
        if (!st.valid()) return nil;
        const MTLPixelFormat fmt = mtl_pixel_format(st.format, st.srgb);
        if (fmt == MTLPixelFormatInvalid) return nil;

        auto apply_swizzle = [&](MTLTextureDescriptor* d) {
            if (@available(macOS 10.15, *)) {
                if (st.format == TexFormat::G8) {
                    // D3DFMT_L8 samples as (L, L, L, 1)
                    d.swizzle = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleRed,
                                                              MTLTextureSwizzleRed, MTLTextureSwizzleOne);
                } else if (st.format == TexFormat::V8U8) {
                    // D3DFMT_V8U8 samples as (U, V, 1, 1)
                    d.swizzle = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleGreen,
                                                              MTLTextureSwizzleOne, MTLTextureSwizzleOne);
                }
            }
        };

        if (!st.is_cube) {
            const int levels = tex_valid_chain(st.format, st.mips);
            if (levels <= 0) return nil;
            MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt
                                                                                         width:st.mips[0].width
                                                                                        height:st.mips[0].height
                                                                                     mipmapped:(levels > 1)];
            d.mipmapLevelCount = levels;
            d.usage = MTLTextureUsageShaderRead;
            d.storageMode = MTLStorageModeShared;
            apply_swizzle(d);
            id<MTLTexture> t = [device newTextureWithDescriptor:d];
            if (!t) return nil;
            for (int i = 0; i < levels; ++i) {
                const TextureMip& m = st.mips[i];
                [t replaceRegion:MTLRegionMake2D(0, 0, m.width, m.height)
                     mipmapLevel:i
                       withBytes:m.data.data()
                     bytesPerRow:tex_row_bytes(st.format, m.width)];
            }
            return t;
        }

        int levels = INT_MAX;
        for (const auto& face : st.faces) {
            levels = std::min(levels, tex_valid_chain(st.format, face));
            if (face.empty() || face[0].width != st.faces[0][0].width || face[0].height != face[0].width) return nil;
        }
        if (levels <= 0 || levels == INT_MAX) return nil;
        MTLTextureDescriptor* d = [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:fmt
                                                                                       size:st.faces[0][0].width
                                                                                  mipmapped:(levels > 1)];
        d.mipmapLevelCount = levels;
        d.usage = MTLTextureUsageShaderRead;
        d.storageMode = MTLStorageModeShared;
        apply_swizzle(d);
        id<MTLTexture> t = [device newTextureWithDescriptor:d];
        if (!t) return nil;
        for (NSUInteger f = 0; f < 6; ++f) {
            for (int i = 0; i < levels; ++i) {
                const TextureMip& m = st.faces[f][i];
                const size_t row = tex_row_bytes(st.format, m.width);
                [t replaceRegion:MTLRegionMake2D(0, 0, m.width, m.height)
                     mipmapLevel:i
                           slice:f
                       withBytes:m.data.data()
                     bytesPerRow:row
                   bytesPerImage:row * tex_rows(st.format, m.height)];
            }
        }
        return t;
    }

    // Compiles every generated material fragment shader (grouped per library, in parallel) and
    // builds one render pipeline per shader. Failing shaders fall back to the legacy pipeline.
    void compile_material_shaders(const SceneMaterialLibrary& lib) {
        const size_t n = lib.shaders.size();
        mat_pipelines.assign(n, nil);
        if (n == 0) return;

        MTLCompileOptions* options = make_compile_options();
        const size_t kGroupSize = 16;
        const size_t num_groups = (n + kGroupSize - 1) / kGroupSize;
        std::vector<id<MTLLibrary>> shader_lib(n, nil);
        std::vector<id<MTLRenderPipelineState>> pipelines(n, nil);
        std::mutex err_mutex;
        std::vector<std::string> errors;

        auto compile_source = [&](const std::string& body, std::string* err_out) -> id<MTLLibrary> {
            @autoreleasepool {
                std::string full = lib.common_source;
                full += "\n";
                full += body;
                NSError* err = nil;
                NSString* src = [NSString stringWithUTF8String:full.c_str()];
                id<MTLLibrary> L = src ? [device newLibraryWithSource:src options:options error:&err] : nil;
                if (!L && err_out) {
                    *err_out = err ? [[err localizedDescription] UTF8String] : "invalid UTF-8 source";
                }
                return L;
            }
        };

        auto build_pipeline = [&](size_t i) -> id<MTLRenderPipelineState> {
            @autoreleasepool {
                id<MTLLibrary> L = shader_lib[i];
                if (!L) return nil;
                const MaterialShader& sh = lib.shaders[i];
                id<MTLFunction> vf = [L newFunctionWithName:[NSString stringWithUTF8String:lib.vertex_function.c_str()]];
                id<MTLFunction> ff = [L newFunctionWithName:[NSString stringWithUTF8String:sh.function_name.c_str()]];
                if (!vf || !ff) return nil;
                MTLRenderPipelineDescriptor* d = [[MTLRenderPipelineDescriptor alloc] init];
                d.vertexFunction = vf;
                d.fragmentFunction = ff;
                d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
                d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
                if (mat_blend_is_translucent(sh.blend)) {
                    auto* ca = d.colorAttachments[0];
                    ca.blendingEnabled = YES;
                    ca.rgbBlendOperation = MTLBlendOperationAdd;
                    ca.alphaBlendOperation = MTLBlendOperationAdd;
                    ca.sourceAlphaBlendFactor = MTLBlendFactorZero;
                    ca.destinationAlphaBlendFactor = MTLBlendFactorOne;
                    if (sh.blend == MatBlendMode::Translucent) {
                        ca.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
                        ca.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                    } else if (sh.blend == MatBlendMode::Additive) {
                        ca.sourceRGBBlendFactor = MTLBlendFactorOne;
                        ca.destinationRGBBlendFactor = MTLBlendFactorOne;
                    } else {  // Modulate
                        ca.sourceRGBBlendFactor = MTLBlendFactorDestinationColor;
                        ca.destinationRGBBlendFactor = MTLBlendFactorZero;
                    }
                }
                NSError* err = nil;
                id<MTLRenderPipelineState> ps = [device newRenderPipelineStateWithDescriptor:d error:&err];
                if (!ps) {
                    std::lock_guard<std::mutex> lock(err_mutex);
                    errors.push_back(sh.function_name + " (" + sh.base_material + ") pipeline: " +
                                     (err ? [[err localizedDescription] UTF8String] : "unknown error"));
                }
                return ps;
            }
        };

        std::atomic<size_t> next_group{0};
        auto worker = [&]() {
            for (;;) {
                const size_t g = next_group.fetch_add(1);
                if (g >= num_groups) return;
                const size_t first = g * kGroupSize;
                const size_t last = std::min(n, first + kGroupSize);
                std::string body;
                for (size_t i = first; i < last; ++i) {
                    body += lib.shaders[i].source;
                    body += "\n";
                }
                id<MTLLibrary> L = compile_source(body, nullptr);
                if (L) {
                    for (size_t i = first; i < last; ++i) shader_lib[i] = L;
                } else {
                    // Isolate the failing shader(s) of this group.
                    for (size_t i = first; i < last; ++i) {
                        std::string err;
                        shader_lib[i] = compile_source(lib.shaders[i].source, &err);
                        if (!shader_lib[i]) {
                            std::lock_guard<std::mutex> lock(err_mutex);
                            errors.push_back(lib.shaders[i].function_name + " (" + lib.shaders[i].base_material +
                                             "): " + err);
                        }
                    }
                }
                for (size_t i = first; i < last; ++i) pipelines[i] = build_pipeline(i);
            }
        };
        const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
        const unsigned num_threads = static_cast<unsigned>(std::min<size_t>(num_groups, hw));
        std::vector<std::thread> pool;
        pool.reserve(num_threads);
        for (unsigned t = 0; t < num_threads; ++t) pool.emplace_back(worker);
        for (auto& th : pool) th.join();

        size_t ok = 0;
        for (size_t i = 0; i < n; ++i) {
            mat_pipelines[i] = pipelines[i];
            if (pipelines[i]) ok++;
        }
        std::cout << "[MetalRenderer] Material shaders: " << ok << "/" << n << " compiled" << std::endl;
        for (size_t e = 0; e < errors.size() && e < 4; ++e) {
            std::string msg = errors[e];
            if (msg.size() > 900) msg = msg.substr(0, 900) + " ...";
            std::cerr << "[MetalRenderer]   shader error: " << msg << std::endl;
        }
    }

    // Makes `lib` the resident material library (uploads textures, compiles shaders) if it changed.
    void sync_material_library(const std::shared_ptr<const SceneMaterialLibrary>& lib) {
        if (lib == mat_lib) return;
        mat_lib = lib;
        mat_textures.clear();
        mat_pipelines.clear();
        if (!lib) return;

        const auto t0 = std::chrono::steady_clock::now();
        const size_t n_tex = lib->textures.size();
        mat_textures.assign(n_tex, nil);
        std::atomic<size_t> next_tex{0};
        std::atomic<size_t> uploaded{0};
        const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
        const unsigned tex_threads = static_cast<unsigned>(std::min<size_t>(std::max<size_t>(1, n_tex), hw));
        auto tex_worker = [&]() {
            while (true) {
                const size_t i = next_tex.fetch_add(1, std::memory_order_relaxed);
                if (i >= n_tex) break;
                @autoreleasepool {
                    id<MTLTexture> tex = upload_scene_texture(lib->textures[i]);
                    mat_textures[i] = tex;
                    if (tex) uploaded.fetch_add(1, std::memory_order_relaxed);
                }
            }
        };
        if (n_tex > 0) {
            std::vector<std::thread> tex_pool;
            tex_pool.reserve(tex_threads);
            for (unsigned t = 0; t < tex_threads; ++t) tex_pool.emplace_back(tex_worker);
            for (auto& th : tex_pool) th.join();
        }
        compile_material_shaders(*lib);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::cout << "[MetalRenderer] Material library resident: " << lib->materials.size() << " materials, "
                  << uploaded.load() << "/" << n_tex << " textures uploaded in " << secs << " s" << std::endl;
    }

    // Returns the pipeline for a mesh section, or nil when it must use legacy procedural shading.
    id<MTLRenderPipelineState> section_pipeline(const MeshSection& s, const MaterialShader** out_shader,
                                                const SceneMaterial** out_material) const {
        if (!mat_lib || s.material < 0 || static_cast<size_t>(s.material) >= mat_lib->materials.size()) return nil;
        const SceneMaterial& m = mat_lib->materials[static_cast<size_t>(s.material)];
        if (m.shader < 0 || static_cast<size_t>(m.shader) >= mat_pipelines.size()) return nil;
        id<MTLRenderPipelineState> ps = mat_pipelines[static_cast<size_t>(m.shader)];
        if (!ps) return nil;
        if (out_shader) *out_shader = &mat_lib->shaders[static_cast<size_t>(m.shader)];
        if (out_material) *out_material = &m;
        return ps;
    }

    // Binds a material instance's textures, samplers and parameter uniforms.
    void bind_material(id<MTLRenderCommandEncoder> enc, const SceneMaterial& m, const MaterialShader& sh) {
        for (int k = 0; k < sh.num_tex2d; ++k) {
            const int ti = (static_cast<size_t>(k) < m.tex2d.size()) ? m.tex2d[static_cast<size_t>(k)] : -1;
            id<MTLTexture> t = (ti >= 0 && static_cast<size_t>(ti) < mat_textures.size()) ? mat_textures[static_cast<size_t>(ti)] : nil;
            TexAddress ax = TexAddress::Wrap;
            TexAddress ay = TexAddress::Wrap;
            if (t) {
                ax = mat_lib->textures[static_cast<size_t>(ti)].address_x;
                ay = mat_lib->textures[static_cast<size_t>(ti)].address_y;
            } else {
                const TexDefault def = (static_cast<size_t>(k) < m.tex2d_default.size()) ? m.tex2d_default[static_cast<size_t>(k)]
                                                                                       : TexDefault::White;
                t = (def == TexDefault::FlatNormal) ? tex_default_flat_normal
                    : (def == TexDefault::Black)    ? tex_default_black
                                                    : tex_default_white;
            }
            [enc setFragmentTexture:t atIndex:static_cast<NSUInteger>(k)];
            [enc setFragmentSamplerState:mat_samplers[static_cast<int>(ax) % 3][static_cast<int>(ay) % 3]
                                 atIndex:static_cast<NSUInteger>(k)];
        }
        for (int j = 0; j < sh.num_texcube; ++j) {
            const int ti = (static_cast<size_t>(j) < m.texcube.size()) ? m.texcube[static_cast<size_t>(j)] : -1;
            id<MTLTexture> t = (ti >= 0 && static_cast<size_t>(ti) < mat_textures.size()) ? mat_textures[static_cast<size_t>(ti)] : nil;
            if (!t) t = tex_default_cube;
            [enc setFragmentTexture:t atIndex:static_cast<NSUInteger>(sh.num_tex2d + j)];
            [enc setFragmentSamplerState:mat_cube_sampler atIndex:static_cast<NSUInteger>(sh.num_tex2d + j)];
        }
        if (sh.num_uniforms > 0) {
            float zero_pad[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            std::vector<std::array<float, 4>> padded;
            const void* data = m.uniforms.data();
            if (m.uniforms.size() < static_cast<size_t>(sh.num_uniforms)) {
                padded = m.uniforms;
                padded.resize(static_cast<size_t>(sh.num_uniforms), {zero_pad[0], zero_pad[1], zero_pad[2], zero_pad[3]});
                data = padded.data();
            }
            [enc setFragmentBytes:data length:static_cast<NSUInteger>(sh.num_uniforms) * 16 atIndex:matbind::kMaterialBuffer];
        }
        if (sh.uses_scene_color || sh.uses_scene_depth) {
            [enc setFragmentTexture:scene_color_copy atIndex:matbind::kSceneColorTexture];
            [enc setFragmentTexture:scene_depth_copy atIndex:matbind::kSceneDepthTexture];
            [enc setFragmentSamplerState:scene_copy_sampler atIndex:matbind::kSceneSampler];
        }
        [enc setFragmentTexture:shadow_depth_tex atIndex:matbind::kShadowMapTexture];
    }

    bool compile_shaders() {
        NSError* error = nil;
        // The sun shadow lookup is shared with the generated material shaders (renderer/sun_shadow.hpp).
        const std::string full_source = sun_shadow_msl() + kBuiltinShadersMSL;
        NSString* source = [NSString stringWithUTF8String:full_source.c_str()];
        MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
        if (@available(macOS 15.0, *)) {
            options.mathMode = MTLMathModeFast;
        }
        if (@available(macOS 11.0, *)) {
            options.languageVersion = MTLLanguageVersion2_4;
        }

        shader_library = [device newLibraryWithSource:source options:options error:&error];
        if (!shader_library) {
            std::cerr << "[MetalRenderer] Shader compilation failed: "
                      << [[error localizedDescription] UTF8String] << std::endl;
            return false;
        }

        // 0. Shadow Map Depth Pipeline
        id<MTLFunction> shadowVert = [shader_library newFunctionWithName:@"shadow_vertex"];
        MTLRenderPipelineDescriptor* shadowDesc = [[MTLRenderPipelineDescriptor alloc] init];
        shadowDesc.vertexFunction = shadowVert;
        shadowDesc.fragmentFunction = nil;
        shadowDesc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        shadow_pipeline = [device newRenderPipelineStateWithDescriptor:shadowDesc error:&error];
        if (!shadow_pipeline) return false;

        // 1. Sky Pipeline
        id<MTLFunction> skyVert = [shader_library newFunctionWithName:@"sky_vertex"];
        id<MTLFunction> skyFrag = [shader_library newFunctionWithName:@"sky_fragment"];
        MTLRenderPipelineDescriptor* skyDesc = [[MTLRenderPipelineDescriptor alloc] init];
        skyDesc.vertexFunction = skyVert;
        skyDesc.fragmentFunction = skyFrag;
        skyDesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
        skyDesc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        sky_pipeline = [device newRenderPipelineStateWithDescriptor:skyDesc error:&error];
        if (!sky_pipeline) return false;

        // 2. World Pipeline
        id<MTLFunction> worldVert = [shader_library newFunctionWithName:@"world_vertex"];
        id<MTLFunction> worldFrag = [shader_library newFunctionWithName:@"world_fragment"];
        MTLRenderPipelineDescriptor* worldDesc = [[MTLRenderPipelineDescriptor alloc] init];
        worldDesc.vertexFunction = worldVert;
        worldDesc.fragmentFunction = worldFrag;
        worldDesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
        worldDesc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        world_pipeline = [device newRenderPipelineStateWithDescriptor:worldDesc error:&error];
        if (!world_pipeline) return false;

        // 3. Viewmodel Pipeline
        id<MTLFunction> vmVert = [shader_library newFunctionWithName:@"viewmodel_vertex"];
        id<MTLFunction> vmFrag = [shader_library newFunctionWithName:@"viewmodel_fragment"];
        MTLRenderPipelineDescriptor* vmDesc = [[MTLRenderPipelineDescriptor alloc] init];
        vmDesc.vertexFunction = vmVert;
        vmDesc.fragmentFunction = vmFrag;
        vmDesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
        vmDesc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        viewmodel_pipeline = [device newRenderPipelineStateWithDescriptor:vmDesc error:&error];
        if (!viewmodel_pipeline) return false;

        // 4. Post-Processing Pipeline
        id<MTLFunction> postVert = [shader_library newFunctionWithName:@"post_vertex"];
        id<MTLFunction> postFrag = [shader_library newFunctionWithName:@"post_fragment"];
        MTLRenderPipelineDescriptor* postDesc = [[MTLRenderPipelineDescriptor alloc] init];
        postDesc.vertexFunction = postVert;
        postDesc.fragmentFunction = postFrag;
        postDesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        post_pipeline = [device newRenderPipelineStateWithDescriptor:postDesc error:&error];
        if (!post_pipeline) return false;

        // 5. 2D HUD Pipeline
        id<MTLFunction> hudVert = [shader_library newFunctionWithName:@"hud_vertex"];
        id<MTLFunction> hudFrag = [shader_library newFunctionWithName:@"hud_fragment"];

        MTLVertexDescriptor* hudVertexDesc = [MTLVertexDescriptor vertexDescriptor];
        hudVertexDesc.attributes[0].format = MTLVertexFormatFloat2;
        hudVertexDesc.attributes[0].offset = offsetof(HUDVertex, position);
        hudVertexDesc.attributes[0].bufferIndex = 0;
        hudVertexDesc.attributes[1].format = MTLVertexFormatFloat4;
        hudVertexDesc.attributes[1].offset = offsetof(HUDVertex, color);
        hudVertexDesc.attributes[1].bufferIndex = 0;
        hudVertexDesc.layouts[0].stride = sizeof(HUDVertex);
        hudVertexDesc.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

        MTLRenderPipelineDescriptor* hudDesc = [[MTLRenderPipelineDescriptor alloc] init];
        hudDesc.vertexFunction = hudVert;
        hudDesc.fragmentFunction = hudFrag;
        hudDesc.vertexDescriptor = hudVertexDesc;
        hudDesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;

        // Alpha blending for UI overlay
        hudDesc.colorAttachments[0].blendingEnabled = YES;
        hudDesc.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
        hudDesc.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
        hudDesc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        hudDesc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha;
        hudDesc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        hudDesc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;

        hud_pipeline = [device newRenderPipelineStateWithDescriptor:hudDesc error:&error];
        if (!hud_pipeline) return false;

        // 6. Textured 2D UI Pipeline (TdUIScene / UI/TdUIResources.upk)
        id<MTLFunction> uiTexVert = [shader_library newFunctionWithName:@"ui_tex_vertex"];
        id<MTLFunction> uiTexFrag = [shader_library newFunctionWithName:@"ui_tex_fragment"];

        MTLVertexDescriptor* uiTexVertexDesc = [MTLVertexDescriptor vertexDescriptor];
        uiTexVertexDesc.attributes[0].format = MTLVertexFormatFloat2;
        uiTexVertexDesc.attributes[0].offset = offsetof(UITexVertex, position);
        uiTexVertexDesc.attributes[0].bufferIndex = 0;
        uiTexVertexDesc.attributes[1].format = MTLVertexFormatFloat2;
        uiTexVertexDesc.attributes[1].offset = offsetof(UITexVertex, uv);
        uiTexVertexDesc.attributes[1].bufferIndex = 0;
        uiTexVertexDesc.attributes[2].format = MTLVertexFormatFloat4;
        uiTexVertexDesc.attributes[2].offset = offsetof(UITexVertex, color);
        uiTexVertexDesc.attributes[2].bufferIndex = 0;
        uiTexVertexDesc.layouts[0].stride = sizeof(UITexVertex);
        uiTexVertexDesc.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

        MTLRenderPipelineDescriptor* uiTexDesc = [[MTLRenderPipelineDescriptor alloc] init];
        uiTexDesc.vertexFunction = uiTexVert;
        uiTexDesc.fragmentFunction = uiTexFrag;
        uiTexDesc.vertexDescriptor = uiTexVertexDesc;
        uiTexDesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        uiTexDesc.colorAttachments[0].blendingEnabled = YES;
        uiTexDesc.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
        uiTexDesc.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
        uiTexDesc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        uiTexDesc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha;
        uiTexDesc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        uiTexDesc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;

        ui_tex_pipeline = [device newRenderPipelineStateWithDescriptor:uiTexDesc error:&error];
        if (!ui_tex_pipeline) return false;

        // Depth Stencil States
        MTLDepthStencilDescriptor* dsWrite = [[MTLDepthStencilDescriptor alloc] init];
        dsWrite.depthCompareFunction = MTLCompareFunctionLessEqual;
        dsWrite.depthWriteEnabled = YES;
        depth_write_state = [device newDepthStencilStateWithDescriptor:dsWrite];

        MTLDepthStencilDescriptor* dsTest = [[MTLDepthStencilDescriptor alloc] init];
        dsTest.depthCompareFunction = MTLCompareFunctionLessEqual;
        dsTest.depthWriteEnabled = NO;
        depth_test_only_state = [device newDepthStencilStateWithDescriptor:dsTest];

        MTLDepthStencilDescriptor* dsDisabled = [[MTLDepthStencilDescriptor alloc] init];
        dsDisabled.depthCompareFunction = MTLCompareFunctionAlways;
        dsDisabled.depthWriteEnabled = NO;
        depth_disabled_state = [device newDepthStencilStateWithDescriptor:dsDisabled];

        // Linear Texture Sampler
        MTLSamplerDescriptor* sampDesc = [[MTLSamplerDescriptor alloc] init];
        sampDesc.minFilter = MTLSamplerMinMagFilterLinear;
        sampDesc.magFilter = MTLSamplerMinMagFilterLinear;
        sampDesc.sAddressMode = MTLSamplerAddressModeClampToEdge;
        sampDesc.tAddressMode = MTLSamplerAddressModeClampToEdge;
        linear_sampler = [device newSamplerStateWithDescriptor:sampDesc];

        return true;
    }

    void allocate_render_targets() {
        MTLTextureDescriptor* hdrDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                                          width:width
                                                                                         height:height
                                                                                      mipmapped:NO];
        hdrDesc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        hdrDesc.storageMode = MTLStorageModePrivate;
        scene_hdr_tex = [device newTextureWithDescriptor:hdrDesc];

        MTLTextureDescriptor* depthDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                            width:width
                                                                                           height:height
                                                                                        mipmapped:NO];
        depthDesc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        depthDesc.storageMode = MTLStorageModePrivate;
        offscreen_depth_tex = [device newTextureWithDescriptor:depthDesc];

        if (!shadow_depth_tex) {
            MTLTextureDescriptor* shDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                              width:kSunShadowMapSize
                                                                                             height:kSunShadowMapSize
                                                                                          mipmapped:NO];
            shDesc.textureType = MTLTextureType2DArray;
            shDesc.arrayLength = 2;  // kSunShadowNearSlice, kSunShadowFarSlice
            shDesc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            shDesc.storageMode = MTLStorageModePrivate;
            shadow_depth_tex = [device newTextureWithDescriptor:shDesc];
            shadow_far_valid = false;
        }

        MTLTextureDescriptor* colorDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                            width:width
                                                                                           height:height
                                                                                        mipmapped:NO];
        colorDesc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        colorDesc.storageMode = MTLStorageModeShared;
        offscreen_color_tex = [device newTextureWithDescriptor:colorDesc];

        // Copies of the opaque scene (UE3 "resolved" SceneColor / SceneDepth) sampled by
        // translucent materials (SceneTexture, DestColor, DepthBiasedAlpha/Blend).
        MTLTextureDescriptor* copyColorDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                                                 width:width
                                                                                                height:height
                                                                                             mipmapped:NO];
        copyColorDesc.usage = MTLTextureUsageShaderRead;
        copyColorDesc.storageMode = MTLStorageModePrivate;
        scene_color_copy = [device newTextureWithDescriptor:copyColorDesc];

        MTLTextureDescriptor* copyDepthDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                                 width:width
                                                                                                height:height
                                                                                             mipmapped:NO];
        copyDepthDesc.usage = MTLTextureUsageShaderRead;
        copyDepthDesc.storageMode = MTLStorageModePrivate;
        scene_depth_copy = [device newTextureWithDescriptor:copyDepthDesc];
    }

    struct WeaponGPUTextures {
        id<MTLTexture> diffuse = nil;
        id<MTLTexture> specular = nil;
        id<MTLTexture> normal = nil;
        id<MTLTexture> mask = nil;
    };
    id<MTLTexture> vm_skin_gpu_tex = nil;
    id<MTLTexture> vm_glove_gpu_tex = nil;
    id<MTLTexture> vm_lower_gpu_tex = nil;
    struct CharacterGPUTextures {
        id<MTLTexture> diffuse = nil;
        id<MTLTexture> specular = nil;
        id<MTLTexture> normal = nil;
    };
    CharacterGPUTextures enemy_gpu_textures[AnimSystem::EnemyArch_Count];
    id<MTLTexture> swat_d_gpu_tex = nil;
    id<MTLTexture> swat_s_gpu_tex = nil;
    id<MTLTexture> swat_n_gpu_tex = nil;
    id<MTLTexture> ammo_d_gpu_tex = nil;
    std::unordered_map<std::string, WeaponGPUTextures> weapon_gpu_textures;

    id<MTLTexture> upload_dxt1_texture(const DXT1Texture& dxt, bool srgb = false) {
        if (!dxt.is_valid()) return nil;
        std::vector<uint8_t> rgba;
        if (!dxt.decode_rgba8(rgba) || rgba.empty()) return nil;
        MTLPixelFormat fmt = srgb ? MTLPixelFormatRGBA8Unorm_sRGB : MTLPixelFormatRGBA8Unorm;
        MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt
                                                                                        width:static_cast<NSUInteger>(dxt.width)
                                                                                       height:static_cast<NSUInteger>(dxt.height)
                                                                                    mipmapped:YES];
        desc.usage = MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModeShared;
        id<MTLTexture> tex = [device newTextureWithDescriptor:desc];
        if (!tex) return nil;
        [tex replaceRegion:MTLRegionMake2D(0, 0, static_cast<NSUInteger>(dxt.width), static_cast<NSUInteger>(dxt.height))
               mipmapLevel:0
                 withBytes:rgba.data()
               bytesPerRow:static_cast<NSUInteger>(dxt.width) * 4];
        if (dxt.width >= 4 && dxt.height >= 4) {
            id<MTLCommandBuffer> cb = [command_queue commandBuffer];
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            [blit generateMipmapsForTexture:tex];
            [blit endEncoding];
            [cb commit];
        }
        return tex;
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
            auto upload_font_pages = [&](const UIMultiFont& f, std::vector<id<MTLTexture>>& out) {
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

    // The HUD, cutscene overlay and chapter-select draw lists: shared with the Direct3D 11 renderer.
#include "overlay_ui.inl"
};

// -----------------------------------------------------------------------------
// MetalRenderer Public Interface
// -----------------------------------------------------------------------------
MetalRenderer::MetalRenderer() : impl_(std::make_unique<Impl>()) {}
MetalRenderer::~MetalRenderer() = default;
MetalRenderer::MetalRenderer(MetalRenderer&&) noexcept = default;
MetalRenderer& MetalRenderer::operator=(MetalRenderer&&) noexcept = default;

void MetalRenderer::set_game_root(const std::string& game_root) {
    if (!game_root.empty()) impl_->game_root = game_root;
}

bool MetalRenderer::init_headless(int width, int height) {
    impl_->width = width;
    impl_->height = height;
    impl_->headless = true;

    impl_->device = MTLCreateSystemDefaultDevice();
    if (!impl_->device) {
        std::cerr << "[MetalRenderer] Failed to acquire default Metal device!" << std::endl;
        return false;
    }

    impl_->command_queue = [impl_->device newCommandQueue];
    if (!impl_->command_queue) return false;

    if (!impl_->compile_shaders()) return false;
    impl_->create_material_defaults();
    impl_->allocate_render_targets();
    impl_->load_character_assets();

    impl_->initialized = true;
    std::cout << "[MetalRenderer] Initialized in Headless Mode (" << width << "x" << height
              << ") on " << [[impl_->device name] UTF8String] << std::endl;
    return true;
}

bool MetalRenderer::init_with_metal_layer(void* ca_metal_layer, int width, int height) {
    if (!ca_metal_layer) return false;

    impl_->width = width;
    impl_->height = height;
    impl_->headless = false;
    impl_->metal_layer = (__bridge CAMetalLayer*)ca_metal_layer;

    impl_->device = impl_->metal_layer.device;
    if (!impl_->device) {
        impl_->device = MTLCreateSystemDefaultDevice();
        impl_->metal_layer.device = impl_->device;
    }

    impl_->metal_layer.pixelFormat = MTLPixelFormatRGBA8Unorm;
    impl_->metal_layer.drawableSize = CGSizeMake(width, height);

    impl_->command_queue = [impl_->device newCommandQueue];
    if (!impl_->command_queue) return false;

    if (!impl_->compile_shaders()) return false;
    impl_->create_material_defaults();
    impl_->allocate_render_targets();
    impl_->load_character_assets();

    impl_->initialized = true;
    std::cout << "[MetalRenderer] Initialized with CAMetalLayer (" << width << "x" << height
              << ") on " << [[impl_->device name] UTF8String] << std::endl;
    return true;
}

void MetalRenderer::resize(int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (impl_->width == width && impl_->height == height) return;

    impl_->width = width;
    impl_->height = height;

    if (impl_->metal_layer) {
        impl_->metal_layer.drawableSize = CGSizeMake(width, height);
    }
    impl_->allocate_render_targets();
}

void MetalRenderer::render_frame(const LevelScene& scene, const PlayerTelemetry& telemetry) {
    if (!impl_->initialized) return;

    @autoreleasepool {
        id<CAMetalDrawable> drawable = nil;
        id<MTLTexture> final_target = impl_->offscreen_color_tex;

        if (!impl_->headless && impl_->metal_layer) {
            drawable = [impl_->metal_layer nextDrawable];
            if (drawable) {
                final_target = drawable.texture;
            }
        }

        id<MTLCommandBuffer> cmd_buffer = [impl_->command_queue commandBuffer];

        // ---------------------------------------------------------------------
        // Main Menu / Load Chapter State: Switch to TdMainMenu.me1 3D City
        // ---------------------------------------------------------------------
        bool in_main_menu = false;
        if (impl_->menu_open) {
            impl_->ensure_main_menu_loaded();
            impl_->main_menu.update_selected_chapter_highlight(impl_->selected_chapter);
            in_main_menu = impl_->main_menu.has_city_scene();
        }
        const LevelScene& active_scene = in_main_menu ? impl_->main_menu.city_scene() : scene;

        // ---------------------------------------------------------------------
        // Camera View & Projection Matrices (Unreal Engine to Metal Canonical)
        // ---------------------------------------------------------------------
        // TdPlayerPawn.CalcCamera: the eyes and the view rotation, moved and turned by the current move's
        // camera animation (the skill roll's somersault).
        Vec3 cam_pos;
        Rotator rot;
        player_camera(telemetry, cam_pos, rot);
        float fov_deg = telemetry.fov_deg;
        float near_plane = 5.0f;
        float far_plane = 65000.0f;

        if (in_main_menu) {
            const MenuChapterEntry& cam_ch = impl_->main_menu.get_chapter(impl_->selected_chapter);
            cam_pos = cam_ch.camera_location;
            rot = cam_ch.camera_rotation;
            fov_deg = 90.0f;
            near_plane = 10.0f;
            far_plane = 400000.0f;
        }

        Vec3 fwd = rot.forward();
        Vec3 right = rot.right();
        Vec3 up = rot.up();
        Vec3 target = cam_pos + fwd * 100.0f;

        Mat4 view = Mat4::look_at(cam_pos, target, up);
        float aspect = float(impl_->width) / float(impl_->height);
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
        std::memcpy(&uniforms.view_proj, vp.m, sizeof(float) * 16);
        Mat4 identity = Mat4::identity();
        std::memcpy(&uniforms.model, identity.m, sizeof(float) * 16);

        uniforms.camera_pos = simd_make_float3(cam_pos.x, cam_pos.y, cam_pos.z);
        uniforms.sim_time = telemetry.sim_time;

        Vec3 sun_d = active_scene.sun_direction.normalized();
        uniforms.sun_dir = simd_make_float3(sun_d.x, sun_d.y, sun_d.z);
        uniforms.sun_color = simd_make_float3(active_scene.sun_color.x, active_scene.sun_color.y, active_scene.sun_color.z);
        uniforms.sky_color = simd_make_float3(active_scene.sky_upper_color.x, active_scene.sky_upper_color.y, active_scene.sky_upper_color.z);
        uniforms.ground_color = simd_make_float3(active_scene.sky_lower_color.x, active_scene.sky_lower_color.y, active_scene.sky_lower_color.z);
        uniforms.speed_2d = impl_->menu_open ? 0.0f : telemetry.speed_2d;
        uniforms.reaction_active = (!impl_->menu_open && telemetry.reaction_active) ? 1.0f : 0.0f;
        uniforms.health = impl_->menu_open ? 100.0f : telemetry.health;
        uniforms.exposure = 1.0f;
        uniforms.actor_tint = simd_make_float3(1.0f, 1.0f, 1.0f);
        uniforms.runner_vision_strength = 0.0f;
        uniforms.is_runner_vision = 0.0f;

        uniforms.cam_forward = simd_make_float3(fwd.x, fwd.y, fwd.z);
        uniforms.fov_tan = std::tan(fov_y_rad * 0.5f);
        uniforms.cam_right = simd_make_float3(right.x, right.y, right.z);
        uniforms.aspect = aspect;
        uniforms.cam_up = simd_make_float3(up.x, up.y, up.z);

        // Directional sun shadow cascades (renderer/sun_shadow.hpp): orthographic squares centred on the
        // camera and snapped to whole texels in light space, so neither map slides across the world by part
        // of a texel as the camera moves, and turning the camera changes nothing. The main menu instead
        // frames TdMainMenu's miniature City of Glass with one fixed square and has no far cascade.
        const Mat4 sun_near_vp = in_main_menu ? sun_shadow_menu_view_proj(sun_d) : sun_shadow_view_proj(sun_d, cam_pos);
        const Mat4 sun_far_vp = in_main_menu ? sun_near_vp : sun_shadow_far_view_proj(sun_d, cam_pos);
        std::memcpy(&uniforms.sun_view_proj, sun_near_vp.m, sizeof(float) * 16);
        std::memcpy(&uniforms.sun_view_proj_far, sun_far_vp.m, sizeof(float) * 16);
        uniforms.mod_shadow_color = simd_make_float3(active_scene.mod_shadow_color.x,
                                                     active_scene.mod_shadow_color.y,
                                                     active_scene.mod_shadow_color.z);
        uniforms.shadow_enabled = in_main_menu ? 2.0f : 1.0f;

        const int frame_slot = static_cast<int>(impl_->frame_index % MetalRenderer::Impl::kMaxFramesInFlight);
        if (!impl_->headless && impl_->in_flight_sem) {
            dispatch_semaphore_wait(impl_->in_flight_sem, DISPATCH_TIME_FOREVER);
        }
        impl_->dyn_vertex_cursor[frame_slot] = 0;

        // ---------------------------------------------------------------------
        // Mirror's Edge materials: make the scene's material library resident
        // (texture upload + MSL compile happen once per library) and find out
        // whether this frame needs a translucency pass / opaque scene copies.
        // ---------------------------------------------------------------------
        impl_->sync_material_library(active_scene.materials);

        auto bind_vertex_bytes_or_buffer = [&](id<MTLRenderCommandEncoder> encoder, const void* data, size_t length, NSUInteger index) {
            if (length <= MetalRenderer::Impl::kMaxInlineVertexBytes) {
                [encoder setVertexBytes:data length:length atIndex:index];
            } else {
                id<MTLBuffer> buf = impl_->acquire_dynamic_vertex_buffer(frame_slot, data, length);
                [encoder setVertexBuffer:buf offset:0 atIndex:index];
            }
        };

        // Rebuild GPU vertex buffer cache only when scene meshes change
        size_t total_scene_verts = 0;
        for (const auto& m : active_scene.meshes) total_scene_verts += m.vertices.size();

        const bool meshes_changed = (active_scene.map_name != impl_->cached_map_name ||
                                     total_scene_verts != impl_->cached_total_verts ||
                                     impl_->cached_mesh_buffers.size() != active_scene.meshes.size());
        if (meshes_changed) {
            impl_->cached_map_name = active_scene.map_name;
            impl_->cached_total_verts = total_scene_verts;
            impl_->cached_mesh_buffers.clear();
            ++impl_->scene_generation;  // the far shadow cascade must be redrawn from the new geometry
            for (const auto& m : active_scene.meshes) {
                if (m.vertices.empty()) {
                    impl_->cached_mesh_buffers.push_back(nil);
                } else {
                    id<MTLBuffer> b = [impl_->device newBufferWithBytes:m.vertices.data()
                                                                 length:m.vertices.size() * sizeof(Vertex)
                                                                options:MTLResourceStorageModeShared];
                    impl_->cached_mesh_buffers.push_back(b);
                }
            }
        }

        if (meshes_changed || impl_->cached_section_mat_lib != impl_->mat_lib ||
            impl_->cached_section_flags.size() != active_scene.meshes.size()) {
            impl_->cached_section_mat_lib = impl_->mat_lib;
            impl_->cached_has_translucent = false;
            impl_->cached_needs_scene_copies = false;
            impl_->cached_section_flags.assign(active_scene.meshes.size(), {});
            for (size_t i = 0; i < active_scene.meshes.size(); ++i) {
                const auto& mesh = active_scene.meshes[i];
                auto& sflags = impl_->cached_section_flags[i];
                sflags.assign(mesh.sections.size(), 0u);
                for (size_t si = 0; si < mesh.sections.size(); ++si) {
                    const auto& s = mesh.sections[si];
                    const MaterialShader* sh = nullptr;
                    const SceneMaterial* m = nullptr;
                    id<MTLRenderPipelineState> ps = impl_->section_pipeline(s, &sh, &m);
                    uint8_t fl = MetalRenderer::Impl::kSecShadowCaster;
                    if (sh && (mat_blend_is_translucent(sh->blend) || sh->lighting == MatLightingModel::Unlit)) {
                        fl &= ~MetalRenderer::Impl::kSecShadowCaster;
                    }
                    if (m && (m->name.find("Skydome") != std::string::npos ||
                              m->name.find("skydome") != std::string::npos)) {
                        fl &= ~MetalRenderer::Impl::kSecShadowCaster;
                    }
                    if (ps && sh && mat_blend_is_translucent(sh->blend)) {
                        fl |= MetalRenderer::Impl::kSecTranslucent;
                        impl_->cached_has_translucent = true;
                        if (sh->uses_scene_color || sh->uses_scene_depth) {
                            impl_->cached_needs_scene_copies = true;
                        }
                    }
                    sflags[si] = fl;
                }
            }
        }
        const bool has_translucent = impl_->cached_has_translucent;
        const bool needs_scene_copies = impl_->cached_needs_scene_copies;

        // Pose active SWAT/CPF enemies once per frame and share between Pass 0 (Shadow) and Pass 1 (World).
        // Enemies are independent (evaluate_enemy_swat_indexed is const and keeps its scratch buffers thread_local),
        // so they are posed in parallel. An enemy's triangle list (exactly evaluate_enemy_swat()'s) is assembled,
        // straight into this frame slot's GPU buffer for it, only if its posed bounds can reach the camera view or
        // the near shadow cascade. The GPU would clip an enemy outside both away completely, so skipping it leaves
        // both passes' output unchanged.
        const bool need_enemies = impl_->anim_system.is_loaded() && !active_scene.enemies.empty() &&
                                  (!impl_->menu_open || (impl_->shadow_depth_tex && impl_->shadow_pipeline));
        if (need_enemies) {
            const size_t enemy_count = active_scene.enemies.size();
            if (impl_->frame_enemy_draws.size() < enemy_count) {
                impl_->frame_enemy_draws.resize(enemy_count);
            }
            auto& slot_enemy_buffers = impl_->enemy_vertex_buffers[frame_slot];
            if (slot_enemy_buffers.size() < enemy_count) {
                slot_enemy_buffers.resize(enemy_count);
            }
            MetalRenderer::Impl* const impl = impl_.get();
            const std::vector<EnemyBot>* const bots = &active_scene.enemies;
            std::vector<id<MTLBuffer>>* const gpu_meshes = &slot_enemy_buffers;
            const ClipVolume view_volume(vp);
            const ClipVolume shadow_volume(sun_near_vp);
            const bool shadow_pass = impl_->shadow_depth_tex && impl_->shadow_pipeline;
            const float sim_time = telemetry.sim_time;
            const bool reaction_active = telemetry.reaction_active;
            const bool menu_open = impl_->menu_open;
            // Each worker touches only element ei of frame_enemy_draws / slot_enemy_buffers (both sized above).
            auto pose_enemy = [impl, bots, gpu_meshes, &view_volume, &shadow_volume, cam_pos, shadow_pass, sim_time,
                               reaction_active, menu_open](size_t ei) {
                const auto& bot = (*bots)[ei];
                auto& draw = impl->frame_enemy_draws[ei];
                draw = MetalRenderer::Impl::EnemyFrameDraw{};
                if (!bot.alive && menu_open) return;

                thread_local std::vector<Vertex> posed;
                posed.resize(impl->anim_system.enemy_swat_max_vertices());
                const AnimSystem::EnemySwatDraw mesh =
                    impl->anim_system.evaluate_enemy_swat_indexed(bot, sim_time, reaction_active, posed.data());
                if (mesh.index_count == 0) return;
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

                const std::vector<uint32_t>& corners = impl->anim_system.enemy_swat_index_lists()[mesh.index_list];
                const size_t bytes = mesh.index_count * sizeof(Vertex);
                @autoreleasepool {
                    id<MTLBuffer> buf = (*gpu_meshes)[ei];
                    if (!buf || buf.length < bytes) {
                        buf = [impl->device newBufferWithLength:MetalRenderer::Impl::dynamic_buffer_capacity(bytes)
                                                        options:MTLResourceStorageModeShared];
                        (*gpu_meshes)[ei] = buf;
                    }
                    Vertex* out = static_cast<Vertex*>(buf.contents);
                    for (size_t k = 0; k < mesh.index_count; ++k) out[k] = posed[corners[k]];
                }
                draw.corner_count = mesh.index_count;
            };
            const auto* const pose_enemy_fn = &pose_enemy;
            dispatch_apply(enemy_count, dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^(size_t ei) {
                (*pose_enemy_fn)(ei);
            });
        }
        // Draws enemy ei's triangle list from this frame slot's buffer; it must be in_view or in_shadow.
        auto draw_enemy_mesh = [&](id<MTLRenderCommandEncoder> encoder, size_t ei) {
            [encoder setVertexBuffer:impl_->enemy_vertex_buffers[frame_slot][ei] offset:0 atIndex:0];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                        vertexStart:0
                        vertexCount:impl_->frame_enemy_draws[ei].corner_count];
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
                    Mat4 part_model = Mat4::translation(parts[static_cast<size_t>(mb.elevator_part)].offset);
                    std::memcpy(&uniforms.model, part_model.m, sizeof(float) * 16);
                    return true;
                }
            }
            if (mb.barge_door >= 0 && static_cast<size_t>(mb.barge_door) < active_scene.barge_doors.size()) {
                const Mat4& door_model = active_scene.barge_doors[static_cast<size_t>(mb.barge_door)].model_matrix;
                std::memcpy(&uniforms.model, door_model.m, sizeof(float) * 16);
                return true;
            }
            std::memcpy(&uniforms.model, identity.m, sizeof(float) * 16);
            return false;
        };

        // ---------------------------------------------------------------------
        // Pass 0: Real-Time Directional Sun Shadow Cascades (2 x 4096x4096 Depth)
        // ---------------------------------------------------------------------
        if (impl_->shadow_depth_tex && impl_->shadow_pipeline) {
            // Renders one cascade into its slice of the shadow map array. `dynamic_casters` adds the moving
            // elevator parts, the barge doors and the enemies; without it only static level geometry is drawn.
            auto encode_shadow_cascade = [&](NSUInteger slice, const Mat4& cascade_vp, float slope_bias_cap_uu,
                                             float depth_range_uu, bool dynamic_casters) {
                MTLRenderPassDescriptor* shadowPass = [MTLRenderPassDescriptor renderPassDescriptor];
                shadowPass.depthAttachment.texture = impl_->shadow_depth_tex;
                shadowPass.depthAttachment.slice = slice;
                shadowPass.depthAttachment.loadAction = MTLLoadActionClear;
                shadowPass.depthAttachment.storeAction = MTLStoreActionStore;
                shadowPass.depthAttachment.clearDepth = 1.0;

                // shadow_vertex projects with uniforms.sun_view_proj.
                const simd_float4x4 near_vp_saved = uniforms.sun_view_proj;
                std::memcpy(&uniforms.sun_view_proj, cascade_vp.m, sizeof(float) * 16);

                id<MTLRenderCommandEncoder> shEnc = [cmd_buffer renderCommandEncoderWithDescriptor:shadowPass];
                [shEnc setViewport:(MTLViewport){0.0, 0.0, (double)kSunShadowMapSize, (double)kSunShadowMapSize, 0.0, 1.0}];
                [shEnc setRenderPipelineState:impl_->shadow_pipeline];
                [shEnc setDepthStencilState:impl_->depth_write_state];
                // Slope-scaled bias, capped in UU (the cap is in map depth units: depth_range_uu UU each).
                [shEnc setDepthBias:0.0012f slopeScale:1.75f clamp:slope_bias_cap_uu / depth_range_uu];
                [shEnc setCullMode:MTLCullModeNone];
                [shEnc setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1];

                bool sh_prev_moved = false;
                for (size_t i = 0; i < active_scene.meshes.size(); ++i) {
                    const auto& mesh = active_scene.meshes[i];
                    if (mesh.vertices.empty() || !impl_->cached_mesh_buffers[i]) continue;
                    if (!dynamic_casters && (mesh.elevator >= 0 || mesh.barge_door >= 0)) continue;
                    bool moved = apply_scene_mesh_model(i);
                    if (moved || sh_prev_moved) [shEnc setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1];
                    sh_prev_moved = moved;
                    [shEnc setVertexBuffer:impl_->cached_mesh_buffers[i] offset:0 atIndex:0];
                    if (mesh.sections.empty()) {
                        [shEnc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:mesh.vertices.size()];
                        continue;
                    }
                    const auto& sflags = impl_->cached_section_flags[i];
                    for (size_t si = 0; si < mesh.sections.size(); ++si) {
                        const auto& s = mesh.sections[si];
                        if (!section_in_range(mesh, s)) continue;
                        if ((sflags[si] & MetalRenderer::Impl::kSecShadowCaster) == 0) continue;
                        [shEnc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:s.first_vertex vertexCount:s.vertex_count];
                    }
                }
                std::memcpy(&uniforms.model, identity.m, sizeof(float) * 16);

                if (dynamic_casters && need_enemies) {
                    // Only the near cascade has dynamic casters; in_shadow was tested against its matrix (and
                    // implies a live, posed enemy).
                    for (size_t ei = 0; ei < active_scene.enemies.size(); ++ei) {
                        if (!impl_->frame_enemy_draws[ei].in_shadow) continue;
                        const auto& bot = active_scene.enemies[ei];
                        Mat4 bot_model = Mat4::translation(bot.position) * Mat4::rotation_z(bot.yaw_deg * DEG2RAD);
                        std::memcpy(&uniforms.model, bot_model.m, sizeof(float) * 16);
                        [shEnc setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1];
                        draw_enemy_mesh(shEnc, ei);
                    }
                    std::memcpy(&uniforms.model, identity.m, sizeof(float) * 16);
                }

                [shEnc endEncoding];
                uniforms.sun_view_proj = near_vp_saved;
            };

            // Far cascade: static geometry only, so it stays valid until its coarsely snapped square steps
            // (the camera moved ~kSunShadowFarStep), the sun direction changes or the scene is rebuilt.
            // The main menu has no far cascade: one fixed square frames its miniature city.
            if (!in_main_menu &&
                (!impl_->shadow_far_valid || impl_->shadow_far_generation != impl_->scene_generation ||
                 std::memcmp(impl_->shadow_far_vp, sun_far_vp.m, sizeof(impl_->shadow_far_vp)) != 0)) {
                encode_shadow_cascade(kSunShadowFarSlice, sun_far_vp, kSunShadowFarSlopeBiasCap, kSunShadowFarDepthRange,
                                      /*dynamic_casters=*/false);
                std::memcpy(impl_->shadow_far_vp, sun_far_vp.m, sizeof(impl_->shadow_far_vp));
                impl_->shadow_far_generation = impl_->scene_generation;
                impl_->shadow_far_valid = true;
            }
            // Near cascade (or the menu's fixed square): every caster, every frame.
            encode_shadow_cascade(kSunShadowNearSlice, sun_near_vp, kSunShadowSlopeBiasCap, kSunShadowDepthRange,
                                  /*dynamic_casters=*/true);
        }

        // ---------------------------------------------------------------------
        // Pass 1: 3D Scene Geometry & Sky -> HDR Texture
        // ---------------------------------------------------------------------
        MTLRenderPassDescriptor* scenePass = [MTLRenderPassDescriptor renderPassDescriptor];
        scenePass.colorAttachments[0].texture = impl_->scene_hdr_tex;
        scenePass.colorAttachments[0].loadAction = MTLLoadActionClear;
        scenePass.colorAttachments[0].storeAction = MTLStoreActionStore;
        scenePass.colorAttachments[0].clearColor = MTLClearColorMake(0.65, 0.82, 0.98, 1.0);

        scenePass.depthAttachment.texture = impl_->offscreen_depth_tex;
        scenePass.depthAttachment.loadAction = MTLLoadActionClear;
        scenePass.depthAttachment.storeAction = MTLStoreActionStore;
        scenePass.depthAttachment.clearDepth = 1.0;

        id<MTLRenderCommandEncoder> enc = [cmd_buffer renderCommandEncoderWithDescriptor:scenePass];
        [enc setViewport:(MTLViewport){0.0, 0.0, (double)impl_->width, (double)impl_->height, 0.0, 1.0}];
        [enc setFragmentTexture:impl_->shadow_depth_tex atIndex:matbind::kShadowMapTexture];

        // A. Draw Sky Dome (TdDirHaze + Distant City Skyline)
        [enc setRenderPipelineState:impl_->sky_pipeline];
        [enc setDepthStencilState:impl_->depth_disabled_state];
        [enc setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];

        // B. Draw World Meshes (BasePass + Beast Radiosity)
        [enc setViewport:(MTLViewport){0.0, 0.0, (double)impl_->width, (double)impl_->height, 0.05, 1.0}];
        [enc setRenderPipelineState:impl_->world_pipeline];
        [enc setDepthStencilState:impl_->depth_write_state];

        auto bind_world_char_wep_textures = [&](const std::string& wname, uint32_t arch_id = 0) {
            uint32_t safe_arch = (arch_id < AnimSystem::EnemyArch_Count) ? arch_id : 0;
            const auto& ctex = impl_->enemy_gpu_textures[safe_arch];
            id<MTLTexture> t_swat   = ctex.diffuse  ? ctex.diffuse  : (impl_->swat_d_gpu_tex ? impl_->swat_d_gpu_tex : impl_->tex_default_white);
            id<MTLTexture> t_swat_s = ctex.specular ? ctex.specular : (impl_->swat_s_gpu_tex ? impl_->swat_s_gpu_tex : impl_->tex_default_black);
            id<MTLTexture> t_swat_n = ctex.normal   ? ctex.normal   : (impl_->swat_n_gpu_tex ? impl_->swat_n_gpu_tex : impl_->tex_default_flat_normal);
            id<MTLTexture> t_wep_d  = impl_->tex_default_white;
            id<MTLTexture> t_wep_s  = impl_->tex_default_black;
            auto it_w = impl_->weapon_gpu_textures.find(wname);
            if (it_w == impl_->weapon_gpu_textures.end() && !impl_->weapon_gpu_textures.empty()) {
                it_w = impl_->weapon_gpu_textures.find("Colt1911");
            }
            if (it_w != impl_->weapon_gpu_textures.end()) {
                if (it_w->second.diffuse)  t_wep_d = it_w->second.diffuse;
                if (it_w->second.specular) t_wep_s = it_w->second.specular;
            }
            id<MTLTexture> t_ammo = impl_->ammo_d_gpu_tex ? impl_->ammo_d_gpu_tex : impl_->tex_default_white;
            [enc setFragmentTexture:t_swat   atIndex:0];
            [enc setFragmentTexture:t_wep_d  atIndex:1];
            [enc setFragmentTexture:t_wep_s  atIndex:2];
            [enc setFragmentTexture:t_ammo   atIndex:3];
            [enc setFragmentTexture:t_swat_s atIndex:4];
            [enc setFragmentTexture:t_swat_n atIndex:5];
            [enc setFragmentSamplerState:impl_->mat_samplers[0][0] atIndex:0];
        };
        bind_world_char_wep_textures("Colt1911");

        // Binds mesh i's vertex buffer + frame uniforms (incl. its model matrix) on the current encoder.
        auto bind_scene_mesh = [&](size_t i) {
            uniforms.is_runner_vision = active_scene.meshes[i].is_runner_vision ? 1.0f : 0.0f;
            apply_scene_mesh_model(i);
            [enc setVertexBuffer:impl_->cached_mesh_buffers[i] offset:0 atIndex:0];
            [enc setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1];
            [enc setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
        };

        if (!active_scene.meshes.empty()) {
            [enc setFrontFacingWinding:impl_->mat_front_winding];
            std::string active_mi_tag = in_main_menu
                ? impl_->main_menu.get_chapter(impl_->selected_chapter).material_instance_tag
                : "";
            for (char& c : active_mi_tag) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            for (size_t i = 0; i < active_scene.meshes.size(); ++i) {
                const auto& mesh = active_scene.meshes[i];
                if (mesh.vertices.empty() || !impl_->cached_mesh_buffers[i]) continue;
                bind_scene_mesh(i);
                if (mesh.sections.empty()) {
                    [enc setRenderPipelineState:impl_->world_pipeline];
                    [enc setCullMode:MTLCullModeNone];
                    bind_world_char_wep_textures("Colt1911");
                    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:mesh.vertices.size()];
                    continue;
                }
                // Opaque + masked material sections (UE3 base pass). Translucent ones are deferred.
                for (const auto& s : mesh.sections) {
                    if (!section_in_range(mesh, s)) continue;
                    const MaterialShader* sh = nullptr;
                    const SceneMaterial* m = nullptr;
                    id<MTLRenderPipelineState> ps = impl_->section_pipeline(s, &sh, &m);
                    if (ps && mat_blend_is_translucent(sh->blend)) continue;
                    if (ps) {
                        [enc setRenderPipelineState:ps];
                        [enc setCullMode:(impl_->mat_cull_enabled && !sh->two_sided && !in_main_menu) ? MTLCullModeBack : MTLCullModeNone];
                        impl_->bind_material(enc, *m, *sh);
                        if (in_main_menu && sh->num_uniforms > 0 && !active_mi_tag.empty()) {
                            std::string mname = m->name;
                            for (char& c : mname) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                            if (mname.find(active_mi_tag) != std::string::npos) {
                                std::vector<std::array<float, 4>> dyn_u = m->uniforms;
                                dyn_u.resize(static_cast<size_t>(sh->num_uniforms), {0.0f, 0.0f, 0.0f, 0.0f});
                                dyn_u[0][0] = 1.0f; // UE3 MaterialInstanceConstant scalar parameter 'Selected' = 1.0
                                [enc setFragmentBytes:dyn_u.data()
                                               length:static_cast<NSUInteger>(sh->num_uniforms) * 16
                                              atIndex:matbind::kMaterialBuffer];
                            }
                        }
                    } else {
                        [enc setRenderPipelineState:impl_->world_pipeline];
                        [enc setCullMode:MTLCullModeNone];
                        bind_world_char_wep_textures("Colt1911");
                    }
                    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:s.first_vertex vertexCount:s.vertex_count];
                }
            }
            [enc setCullMode:MTLCullModeNone];
            std::memcpy(&uniforms.model, identity.m, sizeof(float) * 16);
            uniforms.is_runner_vision = 0.0f;
        }

        // B2. Render 3D Articulated KrugerSec / CPF SWAT Enemies & 3D Weapons/Tracers (only during gameplay)
        // (the material sections above leave their own pipeline/depth state bound)
        [enc setRenderPipelineState:impl_->world_pipeline];
        [enc setDepthStencilState:impl_->depth_write_state];
        [enc setFragmentTexture:impl_->shadow_depth_tex atIndex:matbind::kShadowMapTexture];
        bind_world_char_wep_textures("Colt1911");
        if (!impl_->menu_open && need_enemies) {
            for (size_t ei = 0; ei < active_scene.enemies.size(); ++ei) {
                if (!impl_->frame_enemy_draws[ei].in_view) continue;
                const auto& bot = active_scene.enemies[ei];
                bind_world_char_wep_textures(bot.weapon_name, impl_->frame_enemy_draws[ei].archetype_id);
                Mat4 bot_model = Mat4::translation(bot.position) * Mat4::rotation_z(bot.yaw_deg * DEG2RAD);
                std::memcpy(&uniforms.model, bot_model.m, sizeof(float) * 16);
                uniforms.is_runner_vision = 0.0f;
                uniforms.actor_tint = simd_make_float3(1.0f, 1.0f, 1.0f);
                [enc setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1];
                [enc setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
                draw_enemy_mesh(enc, ei);
            }
            std::memcpy(&uniforms.model, identity.m, sizeof(float) * 16);
        }

        // B2a. Render 3D Dropped Weapons on Ground & 3D Bullet Tracers / Impact Sparks
        if (!impl_->menu_open && impl_->anim_system.is_loaded() &&
            (!active_scene.dropped_weapons.empty() || !active_scene.active_tracers.empty())) {
            std::string pickup_wname = !active_scene.dropped_weapons.empty()
                                           ? active_scene.dropped_weapons.front().weapon_name
                                           : "Colt1911";
            bind_world_char_wep_textures(pickup_wname);
            std::vector<Vertex> combat_fx_verts;
            std::vector<Vertex> combat_rv_verts;
            impl_->anim_system.evaluate_combat_world_fx(active_scene, telemetry.sim_time, combat_fx_verts, combat_rv_verts);
            if (!combat_fx_verts.empty()) {
                std::memcpy(&uniforms.model, identity.m, sizeof(float) * 16);
                uniforms.is_runner_vision = 0.0f;
                uniforms.actor_tint = simd_make_float3(1.0f, 1.0f, 1.0f);
                bind_vertex_bytes_or_buffer(enc, combat_fx_verts.data(),
                                            combat_fx_verts.size() * sizeof(Vertex), 0);
                [enc setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1];
                [enc setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
                [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:combat_fx_verts.size()];
            }
            if (!combat_rv_verts.empty()) {
                std::memcpy(&uniforms.model, identity.m, sizeof(float) * 16);
                uniforms.is_runner_vision = 1.0f;
                uniforms.actor_tint = simd_make_float3(1.0f, 1.0f, 1.0f);
                bind_vertex_bytes_or_buffer(enc, combat_rv_verts.data(),
                                            combat_rv_verts.size() * sizeof(Vertex), 0);
                [enc setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1];
                [enc setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
                [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:combat_rv_verts.size()];
                uniforms.is_runner_vision = 0.0f;
            }
        }

        // B3. Translucent / additive / modulated materials (UE3 translucency pass): drawn after
        // all opaque geometry, depth-tested without depth writes. Materials that read the scene
        // (SceneTexture, DestColor, DepthBiasedAlpha) sample copies of the opaque scene.
        if (has_translucent) {
            if (needs_scene_copies) {
                [enc endEncoding];
                id<MTLBlitCommandEncoder> blit = [cmd_buffer blitCommandEncoder];
                [blit copyFromTexture:impl_->scene_hdr_tex toTexture:impl_->scene_color_copy];
                [blit copyFromTexture:impl_->offscreen_depth_tex toTexture:impl_->scene_depth_copy];
                [blit endEncoding];

                MTLRenderPassDescriptor* transPass = [MTLRenderPassDescriptor renderPassDescriptor];
                transPass.colorAttachments[0].texture = impl_->scene_hdr_tex;
                transPass.colorAttachments[0].loadAction = MTLLoadActionLoad;
                transPass.colorAttachments[0].storeAction = MTLStoreActionStore;
                transPass.depthAttachment.texture = impl_->offscreen_depth_tex;
                transPass.depthAttachment.loadAction = MTLLoadActionLoad;
                transPass.depthAttachment.storeAction = MTLStoreActionStore;
                enc = [cmd_buffer renderCommandEncoderWithDescriptor:transPass];
                [enc setViewport:(MTLViewport){0.0, 0.0, (double)impl_->width, (double)impl_->height, 0.05, 1.0}];
                [enc setFragmentTexture:impl_->shadow_depth_tex atIndex:matbind::kShadowMapTexture];
            }
            [enc setDepthStencilState:impl_->depth_test_only_state];
            [enc setFrontFacingWinding:impl_->mat_front_winding];
            for (size_t i = 0; i < active_scene.meshes.size(); ++i) {
                const auto& mesh = active_scene.meshes[i];
                if (mesh.vertices.empty() || mesh.sections.empty() || !impl_->cached_mesh_buffers[i]) continue;
                bool mesh_bound = false;
                for (const auto& s : mesh.sections) {
                    if (!section_in_range(mesh, s)) continue;
                    const MaterialShader* sh = nullptr;
                    const SceneMaterial* m = nullptr;
                    id<MTLRenderPipelineState> ps = impl_->section_pipeline(s, &sh, &m);
                    if (!ps || !mat_blend_is_translucent(sh->blend)) continue;
                    if (!mesh_bound) {
                        bind_scene_mesh(i);
                        mesh_bound = true;
                    }
                    [enc setRenderPipelineState:ps];
                    [enc setCullMode:(impl_->mat_cull_enabled && !sh->two_sided) ? MTLCullModeBack : MTLCullModeNone];
                    impl_->bind_material(enc, *m, *sh);
                    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:s.first_vertex vertexCount:s.vertex_count];
                }
            }
            [enc setCullMode:MTLCullModeNone];
            std::memcpy(&uniforms.model, identity.m, sizeof(float) * 16);
        }

        // C. Draw First-Person Faith Viewmodel (CH_Faith_1P in DPG_Foreground depth range [0.0, 0.05])
        const bool cutscene_active = (impl_->cutscene_player != nullptr && impl_->cutscene_player->is_playing());
        const bool bink_video_active = (cutscene_active && impl_->cutscene_player->get_mode() == ECutsceneMode::BinkVideo);
        if (!impl_->menu_open && !bink_video_active) {
            impl_->build_faith_viewmodel(telemetry);
            if (!impl_->faith_viewmodel_mesh.empty()) {
                [enc setViewport:(MTLViewport){0.0, 0.0, (double)impl_->width, (double)impl_->height, 0.0, 0.05}];
                [enc setRenderPipelineState:impl_->viewmodel_pipeline];
                [enc setDepthStencilState:impl_->depth_write_state];
                std::memcpy(&uniforms.view_proj, vm_vp.m, sizeof(float) * 16);
                std::memcpy(&uniforms.model, identity.m, sizeof(float) * 16);
                uniforms.camera_pos = simd_make_float3(0.0f, 0.0f, 0.0f);
                uniforms.is_runner_vision = 0.0f;
                uniforms.actor_tint = simd_make_float3(1.0f, 1.0f, 1.0f);

                id<MTLTexture> t_skin  = impl_->vm_skin_gpu_tex  ? impl_->vm_skin_gpu_tex  : impl_->tex_default_white;
                id<MTLTexture> t_glove = impl_->vm_glove_gpu_tex ? impl_->vm_glove_gpu_tex : impl_->tex_default_white;
                id<MTLTexture> t_lower = impl_->vm_lower_gpu_tex ? impl_->vm_lower_gpu_tex : impl_->tex_default_white;
                id<MTLTexture> t_wep_d = impl_->tex_default_white;
                id<MTLTexture> t_wep_s = impl_->tex_default_black;
                id<MTLTexture> t_wep_n = impl_->tex_default_flat_normal;
                id<MTLTexture> t_wep_m = impl_->tex_default_white;
                auto it_w = impl_->weapon_gpu_textures.find(telemetry.weapon.name);
                if (it_w == impl_->weapon_gpu_textures.end() && !impl_->weapon_gpu_textures.empty()) {
                    it_w = impl_->weapon_gpu_textures.find("Colt1911");
                }
                if (it_w != impl_->weapon_gpu_textures.end()) {
                    if (it_w->second.diffuse)  t_wep_d = it_w->second.diffuse;
                    if (it_w->second.specular) t_wep_s = it_w->second.specular;
                    if (it_w->second.normal)   t_wep_n = it_w->second.normal;
                    if (it_w->second.mask)     t_wep_m = it_w->second.mask;
                }
                id<MTLTexture> t_ammo = impl_->ammo_d_gpu_tex ? impl_->ammo_d_gpu_tex : impl_->tex_default_white;

                [enc setFragmentTexture:t_skin  atIndex:0];
                [enc setFragmentTexture:t_glove atIndex:1];
                [enc setFragmentTexture:t_lower atIndex:2];
                [enc setFragmentTexture:t_wep_d atIndex:3];
                [enc setFragmentTexture:t_wep_s atIndex:4];
                [enc setFragmentTexture:t_wep_n atIndex:5];
                [enc setFragmentTexture:t_ammo  atIndex:6];
                [enc setFragmentTexture:t_wep_m atIndex:7];
                [enc setFragmentSamplerState:impl_->mat_samplers[0][0] atIndex:0];

                bind_vertex_bytes_or_buffer(enc, impl_->faith_viewmodel_mesh.data(),
                                            impl_->faith_viewmodel_mesh.size() * sizeof(Vertex), 0);
                [enc setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1];
                [enc setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
                [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:impl_->faith_viewmodel_mesh.size()];
                uniforms.camera_pos = simd_make_float3(cam_pos.x, cam_pos.y, cam_pos.z);
            }
        }

        [enc endEncoding];

        // ---------------------------------------------------------------------
        // Pass 2: Post-Processing, SSAO & Tone Mapping (HDR -> Final Output)
        // ---------------------------------------------------------------------
        MTLRenderPassDescriptor* postPass = [MTLRenderPassDescriptor renderPassDescriptor];
        postPass.colorAttachments[0].texture = final_target;
        postPass.colorAttachments[0].loadAction = MTLLoadActionClear;
        postPass.colorAttachments[0].storeAction = MTLStoreActionStore;
        postPass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

        id<MTLRenderCommandEncoder> postEnc = [cmd_buffer renderCommandEncoderWithDescriptor:postPass];
        [postEnc setRenderPipelineState:impl_->post_pipeline];
        [postEnc setFragmentTexture:impl_->scene_hdr_tex atIndex:0];
        [postEnc setFragmentTexture:impl_->offscreen_depth_tex atIndex:1];
        [postEnc setFragmentSamplerState:impl_->linear_sampler atIndex:0];
        [postEnc setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
        [postEnc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];

        // ---------------------------------------------------------------------
        // Pass 3: 2D HUD, Cutscene Video/Letterbox Overlay & Frontend UI
        // ---------------------------------------------------------------------
        simd_float2 screen_size = simd_make_float2(float(impl_->width), float(impl_->height));
        if (impl_->frontend_rgba != nullptr && impl_->frontend_w > 0 && impl_->frontend_h > 0 && impl_->ui_tex_pipeline) {
            // The front end: one CPU-rendered frame over everything, the same way a Bink frame is shown.
            const int fw = impl_->frontend_w;
            const int fh = impl_->frontend_h;
            if (!impl_->frontend_tex ||
                (int)impl_->frontend_tex.width != fw ||
                (int)impl_->frontend_tex.height != fh) {
                // Not an sRGB texture: the frame is display values already, and this pass writes
                // straight to an RGBA8Unorm target, as the other UI textures do.
                MTLTextureDescriptor* td = [MTLTextureDescriptor
                    texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                 width:fw
                                                height:fh
                                             mipmapped:NO];
                td.usage = MTLTextureUsageShaderRead;
                td.storageMode = MTLStorageModeShared;
                impl_->frontend_tex = [impl_->device newTextureWithDescriptor:td];
            }
            if (impl_->frontend_tex) {
                [impl_->frontend_tex replaceRegion:MTLRegionMake2D(0, 0, fw, fh)
                                       mipmapLevel:0
                                         withBytes:impl_->frontend_rgba
                                       bytesPerRow:fw * 4];

                float w = float(impl_->width);
                float h = float(impl_->height);
                std::vector<HUDVertex> black_bg;
                impl_->draw_ui_quad(black_bg, 0.0f, 0.0f, w, h, simd_make_float4(0.0f, 0.0f, 0.0f, 1.0f));
                [postEnc setRenderPipelineState:impl_->hud_pipeline];
                bind_vertex_bytes_or_buffer(postEnc, black_bg.data(), black_bg.size() * sizeof(HUDVertex), 0);
                [postEnc setVertexBytes:&screen_size length:sizeof(screen_size) atIndex:1];
                [postEnc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:black_bg.size()];

                float src_aspect = float(fw) / float(fh);
                float scr_aspect = w / h;
                float draw_w = w, draw_h = h, draw_x = 0.0f, draw_y = 0.0f;
                if (scr_aspect > src_aspect) {
                    draw_w = h * src_aspect;
                    draw_x = (w - draw_w) * 0.5f;
                } else {
                    draw_h = w / src_aspect;
                    draw_y = (h - draw_h) * 0.5f;
                }

                simd_float4 white = simd_make_float4(1.0f, 1.0f, 1.0f, 1.0f);
                UITexVertex v0{{draw_x, draw_y}, {0.0f, 0.0f}, white};
                UITexVertex v1{{draw_x + draw_w, draw_y}, {1.0f, 0.0f}, white};
                UITexVertex v2{{draw_x + draw_w, draw_y + draw_h}, {1.0f, 1.0f}, white};
                UITexVertex v3{{draw_x, draw_y + draw_h}, {0.0f, 1.0f}, white};
                std::vector<UITexVertex> qv = {v0, v1, v2, v0, v2, v3};

                [postEnc setRenderPipelineState:impl_->ui_tex_pipeline];
                [postEnc setVertexBytes:&screen_size length:sizeof(screen_size) atIndex:1];
                [postEnc setFragmentSamplerState:impl_->linear_sampler atIndex:0];
                [postEnc setFragmentTexture:impl_->frontend_tex atIndex:0];
                bind_vertex_bytes_or_buffer(postEnc, qv.data(), qv.size() * sizeof(UITexVertex), 0);
                [postEnc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:qv.size()];
            }
        } else if (impl_->menu_open) {
            std::vector<HUDVertex> bg_verts;
            std::vector<MetalRenderer::Impl::UITextureBatch> tex_batches;
            std::vector<HUDVertex> fg_verts;
            impl_->draw_main_menu_ui(bg_verts, tex_batches, fg_verts, telemetry);

            if (!bg_verts.empty()) {
                [postEnc setRenderPipelineState:impl_->hud_pipeline];
                bind_vertex_bytes_or_buffer(postEnc, bg_verts.data(), bg_verts.size() * sizeof(HUDVertex), 0);
                [postEnc setVertexBytes:&screen_size length:sizeof(screen_size) atIndex:1];
                [postEnc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:bg_verts.size()];
            }
            if (!tex_batches.empty() && impl_->ui_tex_pipeline) {
                [postEnc setRenderPipelineState:impl_->ui_tex_pipeline];
                [postEnc setVertexBytes:&screen_size length:sizeof(screen_size) atIndex:1];
                [postEnc setFragmentSamplerState:impl_->linear_sampler atIndex:0];
                for (const auto& batch : tex_batches) {
                    if (!batch.tex || batch.verts.empty()) continue;
                    [postEnc setFragmentTexture:batch.tex atIndex:0];
                    bind_vertex_bytes_or_buffer(postEnc, batch.verts.data(), batch.verts.size() * sizeof(UITexVertex), 0);
                    [postEnc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:batch.verts.size()];
                }
            }
            if (!fg_verts.empty()) {
                [postEnc setRenderPipelineState:impl_->hud_pipeline];
                bind_vertex_bytes_or_buffer(postEnc, fg_verts.data(), fg_verts.size() * sizeof(HUDVertex), 0);
                [postEnc setVertexBytes:&screen_size length:sizeof(screen_size) atIndex:1];
                [postEnc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:fg_verts.size()];
            }
        } else if (cutscene_active) {
            const CutscenePlayer* cp = impl_->cutscene_player;
            float w = float(impl_->width);
            float h = float(impl_->height);

            // 1. If playing a Bink (.bik) video movie, upload decoded RGBA frame and draw full-screen 16:9 quad
            if (cp->get_mode() == ECutsceneMode::BinkVideo) {
                int vw = cp->get_video_width();
                int vh = cp->get_video_height();
                const auto& rgba = cp->get_rgba_frame();
                if (vw > 0 && vh > 0 && rgba.size() == static_cast<size_t>(vw * vh * 4)) {
                    if (!impl_->bink_video_tex ||
                        (int)impl_->bink_video_tex.width != vw ||
                        (int)impl_->bink_video_tex.height != vh) {
                        MTLTextureDescriptor* td = [MTLTextureDescriptor
                            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB
                                                         width:vw
                                                        height:vh
                                                     mipmapped:NO];
                        td.usage = MTLTextureUsageShaderRead;
                        td.storageMode = MTLStorageModeShared;
                        impl_->bink_video_tex = [impl_->device newTextureWithDescriptor:td];
                        impl_->bink_uploaded_serial = 0;
                    }
                    if (impl_->bink_video_tex && impl_->bink_uploaded_serial != cp->get_frame_serial()) {
                        [impl_->bink_video_tex replaceRegion:MTLRegionMake2D(0, 0, vw, vh)
                                                 mipmapLevel:0
                                                   withBytes:rgba.data()
                                                 bytesPerRow:vw * 4];
                        impl_->bink_uploaded_serial = cp->get_frame_serial();
                    }

                    if (impl_->bink_video_tex && impl_->ui_tex_pipeline) {
                        // Draw solid black backdrop + letterboxed 16:9 Bink video frame
                        std::vector<HUDVertex> black_bg;
                        impl_->draw_ui_quad(black_bg, 0.0f, 0.0f, w, h, simd_make_float4(0.0f, 0.0f, 0.0f, 1.0f));
                        [postEnc setRenderPipelineState:impl_->hud_pipeline];
                        bind_vertex_bytes_or_buffer(postEnc, black_bg.data(), black_bg.size() * sizeof(HUDVertex), 0);
                        [postEnc setVertexBytes:&screen_size length:sizeof(screen_size) atIndex:1];
                        [postEnc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:black_bg.size()];

                        float vid_aspect = float(vw) / float(vh);
                        float scr_aspect = w / h;
                        float draw_w = w, draw_h = h, draw_x = 0.0f, draw_y = 0.0f;
                        if (scr_aspect > vid_aspect) {
                            draw_w = h * vid_aspect;
                            draw_x = (w - draw_w) * 0.5f;
                        } else {
                            draw_h = w / vid_aspect;
                            draw_y = (h - draw_h) * 0.5f;
                        }

                        std::vector<UITexVertex> qv;
                        simd_float4 white = simd_make_float4(1.0f, 1.0f, 1.0f, 1.0f);
                        UITexVertex v0{{draw_x, draw_y}, {0.0f, 0.0f}, white};
                        UITexVertex v1{{draw_x + draw_w, draw_y}, {1.0f, 0.0f}, white};
                        UITexVertex v2{{draw_x + draw_w, draw_y + draw_h}, {1.0f, 1.0f}, white};
                        UITexVertex v3{{draw_x, draw_y + draw_h}, {0.0f, 1.0f}, white};
                        qv = {v0, v1, v2, v0, v2, v3};

                        [postEnc setRenderPipelineState:impl_->ui_tex_pipeline];
                        [postEnc setVertexBytes:&screen_size length:sizeof(screen_size) atIndex:1];
                        [postEnc setFragmentSamplerState:impl_->linear_sampler atIndex:0];
                        [postEnc setFragmentTexture:impl_->bink_video_tex atIndex:0];
                        bind_vertex_bytes_or_buffer(postEnc, qv.data(), qv.size() * sizeof(UITexVertex), 0);
                        [postEnc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:qv.size()];
                    }
                }
            }

            // 2. Cinema Letterbox Bars, Cutscene Progress & Synchronized Subtitles
            std::vector<HUDVertex> cs_hud;
            float lb = cp->get_letterbox_amount();
            float bar_h = ((cp->get_mode() == ECutsceneMode::InEngineMatinee) ? 64.0f : 44.0f) * lb;
            if (bar_h > 1.0f) {
                impl_->draw_ui_quad(cs_hud, 0.0f, 0.0f, w, bar_h, simd_make_float4(0.0f, 0.0f, 0.0f, 0.88f));
                impl_->draw_ui_quad(cs_hud, 0.0f, h - bar_h, w, bar_h, simd_make_float4(0.0f, 0.0f, 0.0f, 0.88f));
            }

            if (cp->is_level_intro()) {
                // A level's own intro carries what retail shows over it: the skip prompt, top left.
                impl_->draw_ui_text(cs_hud, "Press SPACE to skip", w * 0.074f, h * 0.105f, 1.9f,
                                    simd_make_float4(0.86f, 0.88f, 0.90f, 0.85f));
            } else {
                // Top-right Skip / Next Cutscene controls + progress bar
                std::string ctrl_str = "[SPACE / ENTER] SKIP CUTSCENE   |   [C] NEXT CUTSCENE";
                impl_->draw_ui_text(cs_hud, ctrl_str, w - 535.0f, 14.0f, 1.55f,
                                    simd_make_float4(0.88f, 0.90f, 0.94f, 0.88f));
                float prog = (cp->get_duration() > 0.0f)
                                 ? std::clamp(cp->get_current_time() / cp->get_duration(), 0.0f, 1.0f)
                                 : 0.0f;
                impl_->draw_ui_quad(cs_hud, 28.0f, 16.0f, 220.0f, 6.0f, simd_make_float4(0.18f, 0.20f, 0.24f, 0.75f));
                impl_->draw_ui_quad(cs_hud, 28.0f, 16.0f, 220.0f * prog, 6.0f, simd_make_float4(0.902f, 0.078f, 0.078f, 0.95f));
                impl_->draw_ui_text(cs_hud, "CUTSCENE: " + cp->get_movie_name(), 28.0f, 26.0f, 1.5f,
                                    simd_make_float4(0.85f, 0.88f, 0.92f, 0.85f));
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
                impl_->draw_ui_quad(cs_hud, box_x, box_y, box_w, box_h, simd_make_float4(0.03f, 0.05f, 0.08f, 0.82f));
                impl_->draw_ui_quad(cs_hud, box_x, box_y, 3.0f, box_h, simd_make_float4(0.902f, 0.078f, 0.078f, 0.95f));
                impl_->draw_ui_text(cs_hud, line1, box_x + 18.0f, box_y + 8.0f, 1.75f,
                                    simd_make_float4(0.99f, 0.99f, 1.0f, 1.0f));
                if (!line2.empty()) {
                    impl_->draw_ui_text(cs_hud, line2, box_x + 18.0f, box_y + 29.0f, 1.75f,
                                        simd_make_float4(0.99f, 0.99f, 1.0f, 1.0f));
                }
            }

            if (!cs_hud.empty()) {
                [postEnc setRenderPipelineState:impl_->hud_pipeline];
                bind_vertex_bytes_or_buffer(postEnc, cs_hud.data(), cs_hud.size() * sizeof(HUDVertex), 0);
                [postEnc setVertexBytes:&screen_size length:sizeof(screen_size) atIndex:1];
                [postEnc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:cs_hud.size()];
            }
        } else {
            std::vector<HUDVertex> hud_verts;
            impl_->draw_hud(hud_verts, scene, telemetry);

            if (!hud_verts.empty()) {
                [postEnc setRenderPipelineState:impl_->hud_pipeline];
                bind_vertex_bytes_or_buffer(postEnc, hud_verts.data(), hud_verts.size() * sizeof(HUDVertex), 0);
                [postEnc setVertexBytes:&screen_size length:sizeof(screen_size) atIndex:1];
                [postEnc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:hud_verts.size()];
            }
        }

        [postEnc endEncoding];

        if (drawable) {
            [cmd_buffer presentDrawable:drawable];
        }

        impl_->last_cmd_buffer = cmd_buffer;
        if (!impl_->headless && impl_->in_flight_sem) {
            dispatch_semaphore_t sem = impl_->in_flight_sem;
            [cmd_buffer addCompletedHandler:^(id<MTLCommandBuffer> _Nonnull) {
                dispatch_semaphore_signal(sem);
            }];
            [cmd_buffer commit];
        } else {
            [cmd_buffer commit];
            [cmd_buffer waitUntilCompleted];
        }

        impl_->frame_index++;
    }
}

bool MetalRenderer::save_screenshot_ppm(const std::string& path) {
    if (!impl_->initialized || !impl_->offscreen_color_tex) return false;
    if (impl_->last_cmd_buffer) {
        [impl_->last_cmd_buffer waitUntilCompleted];
    }

    int w = impl_->width;
    int h = impl_->height;
    std::vector<uint8_t> pixels(w * h * 4);

    [impl_->offscreen_color_tex getBytes:pixels.data()
                             bytesPerRow:w * 4
                            fromRegion:MTLRegionMake2D(0, 0, w, h)
                           mipmapLevel:0];

    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;

    out << "P6\n" << w << " " << h << "\n255\n";
    std::vector<uint8_t> rgb(w * h * 3);
    for (int i = 0; i < w * h; ++i) {
        rgb[i * 3 + 0] = pixels[i * 4 + 0];
        rgb[i * 3 + 1] = pixels[i * 4 + 1];
        rgb[i * 3 + 2] = pixels[i * 4 + 2];
    }
    out.write(reinterpret_cast<const char*>(rgb.data()), rgb.size());
    out.close();

    std::cout << "[MetalRenderer] Exported PPM screenshot: " << path << " (" << w << "x" << h << ")" << std::endl;
    return true;
}

bool MetalRenderer::save_screenshot_png(const std::string& path) {
    if (!impl_->initialized || !impl_->offscreen_color_tex) return false;
    if (impl_->last_cmd_buffer) {
        [impl_->last_cmd_buffer waitUntilCompleted];
    }

    int w = impl_->width;
    int h = impl_->height;
    std::vector<uint8_t> pixels(w * h * 4);

    [impl_->offscreen_color_tex getBytes:pixels.data()
                             bytesPerRow:w * 4
                            fromRegion:MTLRegionMake2D(0, 0, w, h)
                           mipmapLevel:0];

    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, pixels.data(), pixels.size(), NULL);
    CGImageRef imageRef = CGImageCreate(
        w, h,
        8, 32,
        w * 4,
        colorSpace,
        (CGBitmapInfo)((uint32_t)kCGBitmapByteOrder32Big | (uint32_t)kCGImageAlphaPremultipliedLast),
        provider,
        NULL,
        false,
        kCGRenderingIntentDefault
    );

    CFURLRef url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
                                                           (const UInt8*)path.c_str(),
                                                           path.length(),
                                                           false);
    CGImageDestinationRef dest = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
    bool success = false;
    if (dest) {
        CGImageDestinationAddImage(dest, imageRef, NULL);
        success = CGImageDestinationFinalize(dest);
        CFRelease(dest);
    }

    if (url) CFRelease(url);
    if (imageRef) CGImageRelease(imageRef);
    if (provider) CGDataProviderRelease(provider);
    if (colorSpace) CGColorSpaceRelease(colorSpace);

    if (success) {
        std::cout << "[MetalRenderer] Exported native PNG screenshot: " << path << " (" << w << "x" << h << ")" << std::endl;
    } else {
        std::cerr << "[MetalRenderer] Failed to export PNG: " << path << std::endl;
    }
    return success;
}

void MetalRenderer::player_camera(const PlayerTelemetry& telemetry, Vec3& out_pos, Rotator& out_rot) const {
    // Without the character assets camera_animation() plays nothing: the plain eyes and view rotation.
    impl_->anim_system.player_camera(telemetry, out_pos, out_rot);
}

bool MetalRenderer::is_initialized() const { return impl_->initialized; }
bool MetalRenderer::is_headless() const { return impl_->headless; }
int MetalRenderer::width() const { return impl_->width; }
int MetalRenderer::height() const { return impl_->height; }
uint64_t MetalRenderer::frame_count() const { return impl_->frame_index; }

void MetalRenderer::set_menu_open(bool open) { impl_->menu_open = open; }
bool MetalRenderer::is_menu_open() const { return impl_->menu_open; }
void MetalRenderer::set_selected_chapter(int idx) { impl_->selected_chapter = std::clamp(idx, 0, 9); }
int MetalRenderer::selected_chapter() const { return impl_->selected_chapter; }
void MetalRenderer::set_selected_menu_tab(int tab) { impl_->selected_menu_tab = std::clamp(tab, 0, 3); }
int MetalRenderer::selected_menu_tab() const { return impl_->selected_menu_tab; }
void MetalRenderer::set_selected_menu_row(int row) { impl_->selected_menu_row = std::clamp(row, 0, 9); }
int MetalRenderer::selected_menu_row() const { return impl_->selected_menu_row; }
void MetalRenderer::set_menu_options_state(int sens_pct, int fov_deg, bool fullscreen) {
    impl_->opt_sens_pct = sens_pct;
    impl_->opt_fov_deg = fov_deg;
    impl_->opt_fullscreen = fullscreen;
}
void MetalRenderer::set_cutscene_player(const CutscenePlayer* player) { impl_->cutscene_player = player; }

void MetalRenderer::set_frontend_frame(const uint8_t* rgba, int width, int height) {
    impl_->frontend_rgba = rgba;
    impl_->frontend_w = rgba ? width : 0;
    impl_->frontend_h = rgba ? height : 0;
}

void* MetalRenderer::raw_device() const { return (__bridge void*)impl_->device; }
void* MetalRenderer::raw_command_queue() const { return (__bridge void*)impl_->command_queue; }
void* MetalRenderer::raw_color_texture() const { return (__bridge void*)impl_->offscreen_color_tex; }
void* MetalRenderer::raw_depth_texture() const { return (__bridge void*)impl_->offscreen_depth_tex; }

} // namespace me
