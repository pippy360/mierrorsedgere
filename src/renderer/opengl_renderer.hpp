#pragma once

#include "../math/types.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace me {

/**
 * OpenGLRenderer: the OpenGL 4.1 core counterpart of MetalRenderer, for Linux (and, for
 * checking it, macOS).
 *
 * It draws the same passes from the same data: sun shadow cascades, sky, the level's
 * UE3 materials, enemies and weapons, the first-person viewmodel, post-processing and
 * the 2D HUD / UI. The shaders are the Metal ones, translated to GLSL when the
 * renderer starts (renderer/msl_to_glsl.hpp); the overlay draw lists are shared code
 * (renderer/overlay_ui.inl).
 *
 * Every frame is rendered to an off-screen framebuffer, which is then blitted to the
 * window, so screenshots work the same with and without a window. The renderer owns
 * the GL context: init_with_window() creates it on the SDL window it is given (which
 * must have been created with SDL_WINDOW_OPENGL after prepare_window_attributes()),
 * init_headless() on a hidden window of its own.
 */
class OpenGLRenderer {
public:
    OpenGLRenderer();
    ~OpenGLRenderer();

    OpenGLRenderer(const OpenGLRenderer&) = delete;
    OpenGLRenderer& operator=(const OpenGLRenderer&) = delete;
    OpenGLRenderer(OpenGLRenderer&&) noexcept;
    OpenGLRenderer& operator=(OpenGLRenderer&&) noexcept;

    // The retail install the character, weapon and menu assets are read from. Call before init.
    void set_game_root(const std::string& game_root);

    // Sets the SDL_GL_* attributes (4.1 core profile, depth, double buffer) a window for
    // init_with_window() needs. Call after SDL_Init and before SDL_CreateWindow.
    static void prepare_window_attributes();

    // Initialization modes
    bool init_headless(int width, int height);
    bool init_with_window(void* sdl_window, int width, int height);

    // Frame management & viewport resize
    void resize(int width, int height);
    void render_frame(const LevelScene& scene, const PlayerTelemetry& telemetry);

    // Screenshots of the last rendered frame
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

    // The front end's CPU-rendered frame, shown full screen in place of the HUD while set
    // (see MetalRenderer::set_frontend_frame).
    void set_frontend_frame(const uint8_t* rgba, int width, int height);

    // The SDL_GLContext (for external probes)
    [[nodiscard]] void* raw_device() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace me
