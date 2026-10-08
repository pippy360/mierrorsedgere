#pragma once

#include "../math/types.hpp"
#include <cstdint>
#include <memory>
#include <string>

namespace me {

/**
 * D3D11Renderer: the Direct3D 11 counterpart of MetalRenderer, for Windows.
 *
 * It draws the same passes from the same data: sun shadow cascades, sky, the level's
 * UE3 materials, enemies and weapons, the first-person viewmodel, post-processing and
 * the 2D HUD / UI. The shaders are the Metal ones, translated to HLSL when the
 * renderer starts (renderer/msl_to_hlsl.hpp); the overlay draw lists are shared code
 * (renderer/overlay_ui.inl).
 *
 * Every frame is rendered to an off-screen target, which a window's swap chain then
 * shows, so screenshots work the same with and without a window.
 */
class D3D11Renderer {
public:
    D3D11Renderer();
    ~D3D11Renderer();

    D3D11Renderer(const D3D11Renderer&) = delete;
    D3D11Renderer& operator=(const D3D11Renderer&) = delete;
    D3D11Renderer(D3D11Renderer&&) noexcept;
    D3D11Renderer& operator=(D3D11Renderer&&) noexcept;

    // The retail install the character, weapon and menu assets are read from. Call before init.
    void set_game_root(const std::string& game_root);

    // Initialization modes
    bool init_headless(int width, int height);
    bool init_with_window(void* hwnd, int width, int height);

    // Frame management & viewport resize
    void resize(int width, int height);
    void render_frame(const LevelScene& scene, const PlayerTelemetry& telemetry);

    // Screenshots of the last rendered frame
    bool save_screenshot_ppm(const std::string& path);
    bool save_screenshot_png(const std::string& path);

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

    // The front end's CPU-rendered frame, shown full screen in place of the HUD while set
    // (see MetalRenderer::set_frontend_frame).
    void set_frontend_frame(const uint8_t* rgba, int width, int height);

    // Raw ID3D11Device* (for external probes)
    [[nodiscard]] void* raw_device() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace me
