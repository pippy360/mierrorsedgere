#pragma once

#include "../math/types.hpp"
#include "../physics/body_shapes.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace me {

/**
 * VulkanRenderer: Native Vulkan 1.0/1.1+ backend for macOS (MoltenVK), Linux, Windows,
 * and Android (Emulator gfxstream & physical ARM Mali / Qualcomm Adreno GPUs).
 *
 * Translates all built-in and UE3 material shaders from MSL to Vulkan GLSL 450
 * (GlslDialect::Vulkan), compiles them in parallel to SPIR-V (shaderc on desktop,
 * glslang on Android NDK) with a persistent disk cache, and executes the complete
 * multi-pass HDR deferred/forward shading, shadow cascade array, bloom/metering/LUT
 * post-processing, Bink video playback, and 2D HUD / touch overlay pipeline on native
 * Vulkan command buffers and swapchains.
 */
class VulkanRenderer {
public:
    VulkanRenderer();
    ~VulkanRenderer();

    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;
    VulkanRenderer(VulkanRenderer&&) noexcept;
    VulkanRenderer& operator=(VulkanRenderer&&) noexcept;

    void set_game_root(const std::string& game_root);

    // Ensures SDL loads the Vulkan ICD/loader library prior to SDL_CreateWindow(..., SDL_WINDOW_VULKAN).
    static void prepare_window_attributes();

    bool init_headless(int width, int height);
    bool init_with_window(void* sdl_window, int width, int height);

    void resize(int width, int height);
    void render_frame(const LevelScene& scene, const PlayerTelemetry& telemetry);

    bool save_screenshot_ppm(const std::string& path);
    bool save_screenshot_png(const std::string& path);

    void player_camera(const PlayerTelemetry& telemetry, Vec3& out_pos, Rotator& out_rot) const;
    void set_world_trace(std::function<bool(const Vec3& from, const Vec3& to, Vec3& hit, Vec3& normal)> trace);
    bool pose_enemy_bodies(const EnemyBot& bot, float sim_time, bool reaction_disarm, EnemyBodySet& out) const;

    [[nodiscard]] bool is_initialized() const;
    [[nodiscard]] bool is_headless() const;
    [[nodiscard]] int width() const;
    [[nodiscard]] int height() const;
    [[nodiscard]] uint64_t frame_count() const;

    void set_menu_open(bool open);
    [[nodiscard]] bool is_menu_open() const;
    void set_selected_chapter(int idx);
    [[nodiscard]] int selected_chapter() const;
    void set_selected_menu_tab(int tab);
    [[nodiscard]] int selected_menu_tab() const;
    void set_selected_menu_row(int row);
    [[nodiscard]] int selected_menu_row() const;
    void set_menu_options_state(int sens_pct, int fov_deg, bool fullscreen);
    void set_cutscene_player(const class CutscenePlayer* player);

    void set_frontend_frame(const uint8_t* rgba, int width, int height, bool overlay = false, float saturation = 0.0f);

    [[nodiscard]] void* raw_device() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace me
