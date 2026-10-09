// Placeholder: replaced by the real backend (see opengl_renderer.hpp).
#include "opengl_renderer.hpp"

#include <iostream>

namespace me {

struct OpenGLRenderer::Impl {
    std::string game_root;
    int width = 1280;
    int height = 720;
    bool headless = true;
    bool menu_open = false;
    int selected_chapter = 1;
    int selected_menu_tab = 0;
    int selected_menu_row = 0;
};

OpenGLRenderer::OpenGLRenderer() : impl_(std::make_unique<Impl>()) {}
OpenGLRenderer::~OpenGLRenderer() = default;
OpenGLRenderer::OpenGLRenderer(OpenGLRenderer&&) noexcept = default;
OpenGLRenderer& OpenGLRenderer::operator=(OpenGLRenderer&&) noexcept = default;

void OpenGLRenderer::set_game_root(const std::string& game_root) { impl_->game_root = game_root; }
void OpenGLRenderer::prepare_window_attributes() {}
bool OpenGLRenderer::init_headless(int, int) {
    std::cerr << "[OpenGLRenderer] not implemented" << std::endl;
    return false;
}
bool OpenGLRenderer::init_with_window(void*, int, int) { return init_headless(0, 0); }
void OpenGLRenderer::resize(int w, int h) { impl_->width = w; impl_->height = h; }
void OpenGLRenderer::render_frame(const LevelScene&, const PlayerTelemetry&) {}
bool OpenGLRenderer::save_screenshot_ppm(const std::string&) { return false; }
bool OpenGLRenderer::save_screenshot_png(const std::string&) { return false; }
void OpenGLRenderer::player_camera(const PlayerTelemetry&, Vec3&, Rotator&) const {}
void OpenGLRenderer::set_world_trace(std::function<bool(const Vec3&, const Vec3&, Vec3&, Vec3&)>) {}
bool OpenGLRenderer::is_initialized() const { return false; }
bool OpenGLRenderer::is_headless() const { return impl_->headless; }
int OpenGLRenderer::width() const { return impl_->width; }
int OpenGLRenderer::height() const { return impl_->height; }
uint64_t OpenGLRenderer::frame_count() const { return 0; }
void OpenGLRenderer::set_menu_open(bool open) { impl_->menu_open = open; }
bool OpenGLRenderer::is_menu_open() const { return impl_->menu_open; }
void OpenGLRenderer::set_selected_chapter(int idx) { impl_->selected_chapter = idx; }
int OpenGLRenderer::selected_chapter() const { return impl_->selected_chapter; }
void OpenGLRenderer::set_selected_menu_tab(int tab) { impl_->selected_menu_tab = tab; }
int OpenGLRenderer::selected_menu_tab() const { return impl_->selected_menu_tab; }
void OpenGLRenderer::set_selected_menu_row(int row) { impl_->selected_menu_row = row; }
int OpenGLRenderer::selected_menu_row() const { return impl_->selected_menu_row; }
void OpenGLRenderer::set_menu_options_state(int, int, bool) {}
void OpenGLRenderer::set_cutscene_player(const CutscenePlayer*) {}
void OpenGLRenderer::set_frontend_frame(const uint8_t*, int, int) {}
void* OpenGLRenderer::raw_device() const { return nullptr; }

}  // namespace me
