#pragma once

#include "../math/types.hpp"
#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <functional>

namespace me {

/**
 * MetalRenderer: Native Apple Metal 3.0 3D graphics renderer for macOS.
 *
 * Implements:
 * 1. Dual-Mode Rendering:
 *    - Headless Offscreen Mode (zero-copy shared CPU readback for automated oracle tests)
 *    - Interactive Window Mode (direct rendering to SDL2 / Cocoa CAMetalLayer)
 * 2. Authentic Mirror's Edge MSL Shaders:
 *    - TdDirHaze: Directional atmospheric sun haze, sky dome, sun disc, and horizon glare
 *    - BasePass + Beast Radiosity: Architectural edge highlights, sky/ground radiosity bounce,
 *      contact ambient occlusion, and procedural rooftop paneling
 *    - Runner Vision (LOI): Dynamic breathing scarlet red (#E61414) pulse on conduits & targets
 *    - CH_Faith_1P: Procedural articulated first-person viewmodel (red glove, runner tattoo,
 *      lower legs/shoes, weapon handling, and momentum animations)
 *    - TdToneMapping & TdMotionBlur: DICE high-key photographic tone curve, radial speed blur,
 *      Reaction Time cyan dilation tint, and low-health vignette
 *    - 2D HUD & Font Overlay: Built-in 5x7 ASCII bitmap font, dynamic center reticle,
 *      speedometer & flow momentum bar, parkour state, health/reaction meters, subtitles,
 *      and chapter select overlay.
 * 3. Screenshot Oracle Exporter:
 *    - PPM (binary P6) and PNG (macOS ImageIO / CoreGraphics native zero-dependency writer).
 */
class MetalRenderer {
public:
    MetalRenderer();
    ~MetalRenderer();

    // Non-copyable, movable
    MetalRenderer(const MetalRenderer&) = delete;
    MetalRenderer& operator=(const MetalRenderer&) = delete;
    MetalRenderer(MetalRenderer&&) noexcept;
    MetalRenderer& operator=(MetalRenderer&&) noexcept;

    // The retail install the character, weapon and menu assets are read from. Call before init.
    void set_game_root(const std::string& game_root);

    // Initialization modes
    bool init_headless(int width, int height);
    bool init_with_metal_layer(void* ca_metal_layer, int width, int height);

    // Frame management & viewport resize
    void resize(int width, int height);
    void render_frame(const LevelScene& scene, const PlayerTelemetry& telemetry);

    // Screenshot Oracle Exporters
    bool save_screenshot_ppm(const std::string& path);
    bool save_screenshot_png(const std::string& path);

    // The first-person camera render_frame() draws `telemetry` from (outside the menu): the eyes and
    // view rotation with the current move's camera animation (TdPlayerPawn.CalcCamera) applied.
    void player_camera(const PlayerTelemetry& telemetry, Vec3& out_pos, Rotator& out_rot) const;
    // The level, as the first-person mesh's foot placement asks about it (AnimSystem::set_world_trace).
    void set_world_trace(std::function<bool(const Vec3& from, const Vec3& to, Vec3& hit, Vec3& normal)> trace);

    // State inspection
    [[nodiscard]] bool is_initialized() const;
    [[nodiscard]] bool is_headless() const;
    [[nodiscard]] int width() const;
    [[nodiscard]] int height() const;
    [[nodiscard]] uint64_t frame_count() const;

    // Interactive UI, Cutscene & Chapter Select controls
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

    // The front end ("Press Any Key", the main menu, and the in-game pause menu) is drawn on the
    // CPU by me::fe::SoftRenderer. When `overlay` is true (pause menu), the 3D scene stays visible
    // underneath with `saturation` (FX_PostProcess.SaturationFilter) applied.
    void set_frontend_frame(const uint8_t* rgba, int width, int height, bool overlay = false, float saturation = 0.0f);

    // Raw Metal device handles (for external toolchain probes)
    [[nodiscard]] void* raw_device() const;
    [[nodiscard]] void* raw_command_queue() const;
    [[nodiscard]] void* raw_color_texture() const;
    [[nodiscard]] void* raw_depth_texture() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace me
