#include "frontend.hpp"

#include "frontend_menus.hpp"

#include "../../assets/package_manager.hpp"

#include <algorithm>
#include <cmath>

namespace me::fe {

namespace {

// TdMenuPostProcesWrapper defaults.
constexpr float kPanelAnimDuration = 0.3f;
constexpr float kUnfocusedPanelWidth = 0.02f;
// The default of the StickWidth parameter in M_MainMenuStick_01.
constexpr float kMaterialStickWidth = 0.005f;
// TdUIScene_MainMenu defaults.
constexpr float kTimeToFadeStart = 2.0f;
constexpr float kFadeTime = 1.0f;
// The scenes' CurrentViewportSize.
constexpr float kSceneWidth = 1280.0f;
constexpr float kSceneHeight = 720.0f;

constexpr float kPi = 3.14159265358979f;

// "<StringAliasMap:Conditional1>QUIT GAME" -> "QUIT GAME": the alias is a gamepad glyph, empty on PC.
std::string strip_markup(const std::string& s) {
    std::string out;
    bool in_tag = false;
    for (char c : s) {
        if (c == '<') in_tag = true;
        else if (c == '>') in_tag = false;
        else if (!in_tag) out.push_back(c);
    }
    return out;
}

// TdUtils.CosineInterp.
float cosine_interp(float a, float b, float t) {
    const float f = (1.0f - std::cos(t * kPi)) * 0.5f;
    return a * (1.0f - f) + b * f;
}

struct ButtonSpec {
    const char* widget;
    const char* caption_key;      // TdGameUI.int [TdMainMenu]
    const char* description_key;  // nullptr: the column shows no description
};

// Each column's sub-buttons in TabIndex order, which is top to bottom.
const ButtonSpec kStoryButtons[] = {{"LoadGameButton", "LoadGameText", "LoadGameDescText"},
                                    {"LoadLevelButton", "LoadLevelText", "LoadLevelDescText"},
                                    {"NewGameButton", "NewGameText", "NewGameDescText"}};
const ButtonSpec kTimeTrialButtons[] = {{"LevelRaceButton", "LevelRaceText", "LevelRaceDescText"},
                                        {"TimeTrialOnlineButton", "TimeTrialOnlineText", "TimeTrialOnlineDescText"},
                                        {"LeaderboardsButton", "LeaderboardsText", "LeaderboardsDescText"}};
const ButtonSpec kOptionsButtons[] = {{"GamepadButton", "GamepadSettingsText", nullptr},
                                      {"VideoButton", "VideoText", nullptr},
                                      {"AudioButton", "AudioText", nullptr},
                                      {"ControlsButton", "ControlsText", nullptr},
                                      {"GameSettingsButton", "GameSettingsText", nullptr}};
// DownloadsButton and XBoxAchievementsButton are hidden unless IsConsole().
const ButtonSpec kExtrasButtons[] = {{"UnlocksButton", "UnlocksText", nullptr}, {"CreditsButton", "CreditsText", nullptr}};

struct PanelSpec {
    const char* caption_widget;
    const char* background_widget;
    const char* caption_key;
    const ButtonSpec* buttons;
    int button_count;
};

const PanelSpec kPanels[kPanelCount] = {
    {"StoryCaptionButton", "StoryPanelBGImage", "StoryCaptionText", kStoryButtons, 3},
    {"TimeTrialCaptionButton", "TimeTrialPanelBGImage", "TimeTrialCaptionText", kTimeTrialButtons, 3},
    {"OptionsCaptionButton", "OptionsPanelBGImage", "OptionsCaptionText", kOptionsButtons, 5},
    {"ExtrasCaptionButton", "ExtrasPanelBGImage", "ExtrasCaptionText", kExtrasButtons, 2},
};

// Style colours from UI_Skins.UI_Skins_TDUISkins2, linear as stored.
constexpr float kShadowRGB[3] = {0.0f, 0.07806f, 0.22714f};  // every drop-shadow style
constexpr float kNavyRGB[3] = {0.0f, 0.00369724f, 0.0277553f};  // TdLabelText_CommonText

}  // namespace

// A UI colour as the canvas writes it (see frontend.hpp, canvas_encode).
void Frontend::ui_color(float r, float g, float b, float a, float out[4]) const {
    out[0] = canvas_encode(r, gamma());
    out[1] = canvas_encode(g, gamma());
    out[2] = canvas_encode(b, gamma());
    out[3] = a;
}

Frontend::Frontend() = default;
Frontend::~Frontend() = default;

bool Frontend::init(const std::string& game_root, int width, int height, std::string& error) {
    width_ = std::max(width, 16);
    height_ = std::max(height, 16);
    // bForce16x9AspectRatio: the scene keeps its 16:9 shape, centred, at the viewport's height.
    scale_ = static_cast<float>(height_) / kSceneHeight;
    origin_x_ = (static_cast<float>(width_) - kSceneWidth * scale_) * 0.5f;
    if (!assets_.load(game_root, height_, error)) return false;

    for (int i = 0; i < kPanelCount; ++i) {
        // FRand() per boot in retail. These are fixed so a frame can be reproduced; a host that
        // wants retail's variety calls set_stick_offsets().
        panels_[static_cast<size_t>(i)].left_offset = 0.13f + 0.21f * static_cast<float>(i);
        panels_[static_cast<size_t>(i)].right_offset = 0.71f - 0.17f * static_cast<float>(i);
    }
    build_panels();

    // The UI scenes behind the sub-buttons, the profile settings and the PC string lists.
    ui_.init(&assets_, scale_);
    if (!ui_.load_profile_settings(settings_)) assets_.warnings.push_back("TdGame.Default__TdProfileSettings not found: the option screens have no values");
    for (const StringListData& list : assets_.string_lists) {
        StringList& l = string_lists_[list.tag];
        l.values = list.strings;
        l.index = list.default_index;
    }
    // What a PC reports at run time; a host replaces these with its own.
    string_lists_["ScreenResolution"].values = {std::to_string(width_) + "x" + std::to_string(height_)};
    string_lists_["ScreenResolution"].index = 0;
    string_lists_["Antialiasing"].values = {ui_.resolve_markup("<Strings:TdGameUI.TdSettingsMappings.Off>"), "2X", "4X", "8X"};
    string_lists_["VSync"].index = 1;
    string_lists_["TextureDetail"].index = 3;
    string_lists_["GraphicsQuality"].index = 3;

    // SeqEvent_LevelLoaded and the root's SeqEvent_SequenceActivated: the opening shot, looping,
    // under a fade from white, and the music's sublevel.
    kismet_.init(&assets_.kismet);
    kismet_.begin_play();
    return true;
}

void Frontend::set_stick_offsets(const std::array<float, kPanelCount>& left, const std::array<float, kPanelCount>& right) {
    for (size_t i = 0; i < kPanelCount; ++i) {
        panels_[i].left_offset = left[i];
        panels_[i].right_offset = right[i];
    }
}

// TdUIScene_MainMenu.InitializeWidgetsData.
void Frontend::build_panels() {
    for (int i = 0; i < kPanelCount; ++i) {
        PanelState& p = panels_[static_cast<size_t>(i)];
        const PanelSpec& spec = kPanels[i];
        p.buttons.clear();
        assets_.menu_rect(spec.caption_widget, p.caption);
        assets_.menu_rect(spec.background_widget, p.background);
        p.caption_text = assets_.text("TdMainMenu", spec.caption_key);
        for (int b = 0; b < spec.button_count; ++b) {
            const ButtonSpec& bs = spec.buttons[b];
            const std::string widget = bs.widget;
            if (widget == "LoadGameButton" && !profile_.can_continue) continue;
            if (widget == "LoadLevelButton" && !profile_.chapters_unlocked) continue;
            if (widget == "LevelRaceButton" && !profile_.all_levels_unlocked) continue;
            if (widget == "GamepadButton" && !profile_.controller) continue;
            Button btn;
            btn.widget = widget;
            btn.caption = assets_.text("TdMainMenu", bs.caption_key);
            if (bs.description_key) btn.description = assets_.text("TdMainMenu", bs.description_key);
            if (!assets_.menu_rect(widget, btn.rect)) continue;
            p.buttons.push_back(std::move(btn));
        }
        p.focus = 0;
    }
}

void Frontend::update(float dt) {
    dt = std::clamp(dt, 0.0f, 0.25f);
    time_ += dt;
    // Level events fired since the last update (a key was handled) run in this one.
    kismet_.update(dt);
    for (const std::string& level : kismet_.take_streamed_levels()) {
        // SeqEvent_LevelLoaded -> DefaultMenuMusic -> TdMainMenu_Audio0, whose own Kismet fires PlayMenuMusic.
        if (level == "TdMainMenu_Audio0") sound("Music");
    }

    if (screen_ == Screen::Start) {
        time_in_scene_ += dt;
        return;
    }
    if (!scenes_.empty()) scenes_.back()->tick(dt);
    closed_.clear();

    // TdUIScene_MainMenu.Tick
    if (initial_tick_) {
        initial_tick_ = false;
        set_active_panel(kStory, false);
    }
    // TdMenuPostProcesWrapper.Tick
    for (int i = 0; i < kPanelCount; ++i) {
        PanelState& p = panels_[static_cast<size_t>(i)];
        if (p.active) {
            if (p.anim < kPanelAnimDuration) {
                p.anim += dt;
                if (p.anim >= kPanelAnimDuration) {
                    p.anim = kPanelAnimDuration;
                    panel_anim_finished(i);
                }
            }
        } else if (p.anim > 0.0f) {
            p.anim -= dt;
            if (p.anim <= 0.0f) {
                p.anim = 0.0f;
                panel_anim_finished(i);
            }
        }
    }
    if (fade_timer_ <= kTimeToFadeStart + kFadeTime) fade_timer_ += dt;
}

// TdUIScene_MainMenu.SetActivePanel
void Frontend::set_active_panel(int index, bool silent) {
    if (index == current_panel_ || index < 0 || index >= kPanelCount) return;
    animating_ = true;
    last_panel_ = current_panel_;
    current_panel_ = index;
    for (PanelState& p : panels_) {
        p.active = false;
        p.shown = false;
    }
    PanelState& p = panels_[static_cast<size_t>(index)];
    p.active = true;
    p.animated = true;
    if (last_panel_ >= 0) panels_[static_cast<size_t>(last_panel_)].animated = true;
    if (silent) p.anim = kPanelAnimDuration - 1.0e-5f;
    // The column's intro camera, then its loop.
    level_event("panel" + std::to_string(index + 1));
    hovered_ = -1;
    fade_timer_ = 0.0f;
    if (!silent) sound("TabChangeRight");
}

// TdUIScene_MainMenu.PanelAnimFinished
void Frontend::panel_anim_finished(int index) {
    if (index != current_panel_) return;
    PanelState& p = panels_[static_cast<size_t>(index)];
    p.shown = true;
    // SetFocusToChild(): the first enabled button in TabIndex order.
    p.focus = 0;
    description_ = p.buttons.empty() ? std::string() : p.buttons.front().description;
    fade_timer_ = 0.0f;
    animating_ = false;
}

void Frontend::open_main_menu() {
    if (screen_ == Screen::MainMenu) return;
    // TdUIScene_Start.StartGame -> OpenScene(TdMainMenu)
    screen_ = Screen::MainMenu;
    initial_tick_ = true;
    current_panel_ = -1;
    last_panel_ = -1;
    build_panels();
}

void Frontend::key_down(Key key) {
    if (screen_ == Screen::MainMenu && !scenes_.empty()) {
        scenes_.back()->key_pressed(key);
        return;
    }
    if (screen_ != Screen::MainMenu || current_panel_ < 0) return;
    PanelState& p = panels_[static_cast<size_t>(current_panel_)];
    switch (key) {
        case Key::Right:  // SwitchTab(1)
            set_active_panel((current_panel_ + 1) % kPanelCount, false);
            break;
        case Key::Left:  // SwitchTab(-1)
            set_active_panel((current_panel_ + kPanelCount - 1) % kPanelCount, false);
            break;
        case Key::Up:
        case Key::Down: {
            if (!p.shown || p.buttons.size() < 2) break;
            const int n = static_cast<int>(p.buttons.size());
            p.focus = (p.focus + (key == Key::Down ? 1 : n - 1)) % n;
            description_ = p.buttons[static_cast<size_t>(p.focus)].description;
            fade_timer_ = 0.0f;  // ButtonStateChange: a button gained focus
            sound(key == Key::Down ? "NavigateDown" : "NavigateUp");
            break;
        }
        default:
            break;
    }
}

void Frontend::key_up(Key key) {
    if (screen_ == Screen::Start) {
        // TdUIScene_Start.HandleInputKey: any key released, once the start button is up.
        if (time_in_scene_ >= assets_.time_till_start_button) open_main_menu();
        return;
    }
    if (!scenes_.empty()) {
        scenes_.back()->key_released(key);
        return;
    }
    if (current_panel_ < 0) return;
    if (key == Key::Escape) {
        quit_clicked();
    } else if (key == Key::Accept) {
        // A button is clicked when Enter comes up on it.
        const PanelState& p = panels_[static_cast<size_t>(current_panel_)];
        if (p.shown && !p.buttons.empty()) button_clicked(p.buttons[static_cast<size_t>(p.focus)].widget);
    }
}

void Frontend::open_scene(std::unique_ptr<SubMenu> menu) {
    if (!menu || !menu->valid()) return;
    scenes_.push_back(std::move(menu));
    scenes_.back()->opened();
}

// CloseScene, then the delegate, then SceneActivated on whatever is on top now, unless the
// delegate opened a scene of its own.
void Frontend::close_scene(SubMenu* menu, const std::function<void()>& then) {
    auto it = std::find_if(scenes_.begin(), scenes_.end(), [&](const std::unique_ptr<SubMenu>& m) { return m.get() == menu; });
    if (it == scenes_.end()) return;
    closed_.push_back(std::move(*it));
    scenes_.erase(it);
    const size_t depth = scenes_.size();
    if (then) then();
    if (scenes_.size() > depth) return;
    if (!scenes_.empty()) {
        scenes_.back()->reactivated();
    } else if (current_panel_ >= 0) {
        // TdUIScene_MainMenu.SceneActivated: the column camera again.
        level_event("panel" + std::to_string(current_panel_ + 1));
    }
}

std::string Frontend::scene_name() const { return scenes_.empty() || !scenes_.back()->scene() ? std::string() : scenes_.back()->scene()->name; }
std::string Frontend::map_path(const std::string& file) const {
    const PackageManager* pm = assets_.packages();
    if (!pm) return {};
    std::string path = pm->find_package_path(file);
    const std::string& root = pm->cooked_root();
    if (path.empty() || path.size() <= root.size() || path.compare(0, root.size(), root) != 0) return {};
    path.erase(0, root.size());
    while (!path.empty() && (path.front() == '/' || path.front() == '\\')) path.erase(0, 1);
    for (char& c : path) {
        if (c == '\\') c = '/';
    }
    return path;
}

const UiScene* Frontend::scene() const { return scenes_.empty() ? nullptr : scenes_.back()->scene(); }
std::string Frontend::scene_focus() const { return scenes_.empty() ? std::string() : scenes_.back()->focused_name(); }

// TdUIScene_MainMenu.HandleButtonClicked.
void Frontend::button_clicked(const std::string& widget) {
    if (animating_) return;
    level_event(widget + "_Clicked");
    sound("Accept");
    if (widget == "LoadGameButton") {
        action_ = "Continue";
    } else if (widget == "NewGameButton") {
        // OnStartGame: with a game in progress, a warning first.
        if (profile_.can_continue) {
            open_scene(make_message_box(*this, "<Strings:TdGameUI.TdMessageBox.NewGameWarningTitle>", "<Strings:TdGameUI.TdMessageBox.NewGameWarningMessage>",
                                        {{"<Strings:TdGameUI.TdButtonCallouts.Cancel>", Key::Escape}, {"<Strings:TdGameUI.TdButtonCallouts.OK>", Key::Accept}},
                                        [this](int option) {
                                            if (option == 1) open_scene(make_sub_menu(*this, "TdDifficultySettings"));
                                        }));
        } else {
            open_scene(make_sub_menu(*this, "TdDifficultySettings"));
        }
    } else if (widget == "LoadLevelButton") {
        open_scene(make_sub_menu(*this, "TdLoadLevel"));
    } else if (widget == "TimeTrialOnlineButton") {
        online_check("TdTTSelectStretchOffline");
    } else if (widget == "LevelRaceButton") {
        online_check("TdLRSelectLevelOffline");
    } else if (widget == "LeaderboardsButton") {
        online_check("");
    } else if (widget == "ControlsButton") {
        open_scene(make_sub_menu(*this, "TdKeyMappings"));
    } else if (widget == "AudioButton") {
        open_scene(make_sub_menu(*this, "TdAudioSettings"));
    } else if (widget == "VideoButton") {
        open_scene(make_sub_menu(*this, "TdVideoSettingsPC"));
    } else if (widget == "GameSettingsButton") {
        open_scene(make_sub_menu(*this, "TdGameSettings"));
    } else if (widget == "GamepadButton") {
        open_scene(make_sub_menu(*this, "TdControlsSettings"));
    } else if (widget == "UnlocksButton") {
        open_scene(make_sub_menu(*this, "TdUnlocks"));
    } else if (widget == "CreditsButton") {
        open_scene(make_sub_menu(*this, "TdCredits"));
    }
    // A scene that could not be opened leaves the camera where the click sent it; put it back.
    if (scenes_.empty() && action_.empty() && current_panel_ >= 0) level_event("panel" + std::to_string(current_panel_ + 1));
}

// OnQuitGame.
void Frontend::quit_clicked() {
    open_scene(make_message_box(*this, "<Strings:TdGameUI.TdMessageBox.QuitConfirm_Title>", "<Strings:TdGameUI.TdMessageBox.QuitConfirm_Message>",
                                {{"<Strings:TdGameUI.TdButtonCallouts.Cancel>", Key::Escape}, {"<Strings:TdGameUI.TdButtonCallouts.OK>", Key::Accept}},
                                [this](int option) {
                                    if (option == 1) action_ = "Quit";
                                }));
}

// TdOnlineLoginHandler.StartConnection with no EA servers to reach: "Connecting", then the
// connection error, then (for the race modes) the offer to play offline.
void Frontend::online_check(const std::string& offline_scene) {
    open_scene(make_message_box(
        *this, "<Strings:TdGameUI.TdModalConnectingMessageBox.TitleText>", "<Strings:TdGameUI.TdModalConnectingMessageBox.DescriptionText>",
        {{"<Strings:TdGameUI.TdButtonCallouts.Cancel>", Key::Escape}},
        [this, offline_scene](int option) {
            if (option != -1) return;  // cancelled
            open_scene(make_message_box(*this, "<Strings:TdGameUI.TpErrors.Failed_Connect_Title>", "<Strings:TdGameUI.TpErrors.Error_-203>",
                                        {{"<Strings:TdGameUI.TdButtonCallouts.OK>", Key::Accept}}, [this, offline_scene](int) {
                                            if (offline_scene.empty()) return;
                                            open_scene(make_online_check(*this, [this, offline_scene] {
                                                open_scene(make_sub_menu(*this, offline_scene));
                                            }));
                                        }));
        },
        1.5f));
}

int Frontend::button_at(float x, float y) const {
    if (current_panel_ < 0) return -1;
    const PanelState& p = panels_[static_cast<size_t>(current_panel_)];
    if (!p.shown) return -1;
    for (size_t i = 0; i < p.buttons.size(); ++i) {
        const Rect r = to_view(p.buttons[i].rect);
        if (x >= r.l && x < r.r && y >= r.t && y < r.b) return static_cast<int>(i);
    }
    return -1;
}

void Frontend::mouse_move(float x, float y) {
    if (screen_ != Screen::MainMenu || current_panel_ < 0) return;
    // A button entering the Active state rebinds the description; focus stays where it was.
    const int b = button_at(x, y);
    if (b != hovered_ && b >= 0) description_ = panels_[static_cast<size_t>(current_panel_)].buttons[static_cast<size_t>(b)].description;
    hovered_ = b;
}

void Frontend::mouse_click(float x, float y) {
    if (screen_ == Screen::Start) {
        if (time_in_scene_ >= assets_.time_till_start_button) open_main_menu();
        return;
    }
    if (current_panel_ < 0) return;
    const int b = button_at(x, y);
    if (b >= 0) {
        PanelState& p = panels_[static_cast<size_t>(current_panel_)];
        if (p.focus != b) fade_timer_ = 0.0f;
        p.focus = b;
        description_ = p.buttons[static_cast<size_t>(b)].description;
        if (!animating_) {
            action_ = p.buttons[static_cast<size_t>(b)].widget;
            sound("Accept");
        }
        return;
    }
    // The small caption of another column: OnButtonClicked_Panel<N>.
    for (int i = 0; i < kPanelCount; ++i) {
        const Rect r = to_view(panels_[static_cast<size_t>(i)].caption);
        if (i != current_panel_ && x >= r.l && x < r.r && y >= r.t && y < r.b) {
            set_active_panel(i, false);
            return;
        }
    }
}

std::string Frontend::focused_button() const {
    if (screen_ != Screen::MainMenu || current_panel_ < 0) return {};
    const PanelState& p = panels_[static_cast<size_t>(current_panel_)];
    if (!p.shown || p.buttons.empty()) return {};
    return p.buttons[static_cast<size_t>(p.focus)].widget;
}

std::vector<std::string> Frontend::take_sounds() {
    std::vector<std::string> out;
    out.swap(sounds_);
    return out;
}

std::string Frontend::take_action() {
    std::string out;
    out.swap(action_);
    return out;
}

Rect Frontend::to_view(const Rect& s) const {
    return Rect{origin_x_ + s.l * scale_, s.t * scale_, origin_x_ + s.r * scale_, s.b * scale_};
}

// --- drawing ------------------------------------------------------------------------------

void Frontend::draw_text(Frame& f, const Font& font, const std::string& text, float x, float y, const float color[4]) const {
    if (!font.valid() || text.empty() || color[3] <= 0.0f) return;
    // One op per font page, in first-use order.
    const size_t first_op = f.ui.size();
    float pen = x;
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        const Glyph& g = font.glyphs[c];
        if (g.w > 0 && g.h > 0 && g.page >= 0 && static_cast<size_t>(g.page) < font.pages.size() &&
            font.pages[static_cast<size_t>(g.page)].valid()) {
            const Image* page = &font.pages[static_cast<size_t>(g.page)];
            DrawOp* op = nullptr;
            for (size_t k = first_op; k < f.ui.size(); ++k) {
                if (f.ui[k].image == page) op = &f.ui[k];
            }
            if (!op) {
                f.ui.emplace_back();
                op = &f.ui.back();
                op->kind = DrawOp::Kind::Glyphs;
                op->image = page;
                std::copy(color, color + 4, op->color);
            }
            Quad q;
            q.x0 = pen;
            q.y0 = y + static_cast<float>(g.voff) * font.scale;
            q.x1 = q.x0 + static_cast<float>(g.w) * font.scale;
            q.y1 = q.y0 + static_cast<float>(g.h) * font.scale;
            q.u0 = static_cast<float>(g.u) / static_cast<float>(page->w);
            q.v0 = static_cast<float>(g.v) / static_cast<float>(page->h);
            q.u1 = static_cast<float>(g.u + g.w) / static_cast<float>(page->w);
            q.v1 = static_cast<float>(g.v + g.h) / static_cast<float>(page->h);
            op->quads.push_back(q);
        }
        pen += font.advance(c, i + 1 < text.size() ? static_cast<unsigned char>(text[i + 1]) : 0);
    }
}

void Frontend::draw_label(Frame& f, const Font& font, const std::string& text, const Rect& box, int halign, int valign,
                          const float color[4], const float* shadow_color, float shadow_dx, float shadow_dy) const {
    if (text.empty() || !font.valid()) return;
    const float line = static_cast<float>(font.line_height) * font.scale;
    const float width = font.width(text);
    float x = box.l;
    if (halign == 1) x = box.l + (box.w() - width) * 0.5f;
    if (halign == 2) x = box.r - width;
    float y = box.t;
    if (valign == 1) y = box.t + (box.h() - line) * 0.5f;
    if (valign == 2) y = box.b - line;
    // The canvas places a string on whole pixels.
    x = std::round(x);
    y = std::round(y);
    if (shadow_color && shadow_color[3] > 0.0f) {
        // UIComp_TdDropShadowString: the offsets are fractions of the line height.
        draw_text(f, font, text, x + std::round(shadow_dx * line), y + std::round(shadow_dy * line), shadow_color);
    }
    draw_text(f, font, text, x, y, color);
}

void Frontend::draw_start(Frame& f) const {
    // TdUIScene_Start hides its SafeRegionPanel when a key is taken, so nothing here survives it.
    Rect panel{96.0f, 54.0f, 1184.0f, 666.0f};
    assets_.start_rect("ContentPanel", panel);
    (void)panel;

    // SceneActivated sets Opacity to 0 and the scene's native tick brings it up. Measured on
    // retail: nothing for the first second, then a linear second to full.
    const float opacity = std::clamp(time_in_scene_ - 1.0f, 0.0f, 1.0f);
    if (opacity <= 0.0f) return;

    // TitleImage: ADJUST_Justified on both axes scales the texture to fit the widget, keeping its
    // shape. It is centred across (UIALIGN_Center) and, with no vertical alignment set, sits at the top.
    if (assets_.title.valid()) {
        Rect r{96.0f, 54.0f, 1184.0f, 666.0f};
        assets_.start_rect("TitleImage", r);
        const Rect box = to_view(r);
        const float fit = std::min(box.w() / static_cast<float>(assets_.title.w), box.h() / static_cast<float>(assets_.title.h));
        const float w = static_cast<float>(assets_.title.w) * fit;
        const float h = static_cast<float>(assets_.title.h) * fit;
        DrawOp op;
        op.kind = DrawOp::Kind::Image;
        op.image = &assets_.title;
        op.color[3] = opacity;
        const float x = box.l + (box.w() - w) * 0.5f;
        op.quads.push_back(Quad{x, box.t, x + w, box.t + h, 0.0f, 0.0f, 1.0f, 1.0f});
        f.ui.push_back(std::move(op));
    }

    // Both labels: TdLabelTextCommonTextBold with DrawColor (1, 0, 0, 1), centred both ways.
    float red[4];
    ui_color(1.0f, 0.0f, 0.0f, opacity, red);
    Rect copyright{96.0f, 631.021f, 1184.0f, 666.0f};
    assets_.start_rect("CopyrightLabel", copyright);
    draw_label(f, assets_.small_bold, assets_.text("TdStart", "CopyrightText"), to_view(copyright), 1, 1, red, nullptr, 0.0f, 0.0f);

    if (time_in_scene_ >= assets_.time_till_start_button) {
        Rect press{96.0f, 115.2f, 1184.0f, 666.0f};
        assets_.start_rect("PressStartLabel", press);
        draw_label(f, assets_.small_bold, assets_.text("TdStart", "PressAnyKeyText"), to_view(press), 1, 1, red, nullptr, 0.0f, 0.0f);
    }
}

void Frontend::draw_menu(Frame& f) const {
    if (current_panel_ < 0) return;
    float white[4], black[4], navy[4];
    ui_color(1.0f, 1.0f, 1.0f, 1.0f, white);
    ui_color(0.0f, 0.0f, 0.0f, 1.0f, black);

    // The four columns: one M_MainMenuStick_01 instance each, left to right.
    for (int i = 0; i < kPanelCount; ++i) {
        const PanelState& p = panels_[static_cast<size_t>(i)];
        DrawOp op;
        op.kind = DrawOp::Kind::Stick;
        op.rect = to_view(p.background);
        // UpdatePanelAnimation only runs while a column animates, so a column that has never been
        // opened still has the StickWidth the material itself declares.
        op.stick.width = p.animated ? cosine_interp(kUnfocusedPanelWidth, 1.0f, p.anim / kPanelAnimDuration) : kMaterialStickWidth;
        op.stick.move_offset = static_cast<float>(i) / static_cast<float>(kPanelCount);
        op.stick.move_amount = p.active ? 0.0f : 1.0f;
        op.stick.left_offset = p.left_offset;
        op.stick.right_offset = p.right_offset;
        if (p.active && p.shown && !p.buttons.empty()) {
            // UpdateSelectionField: the focused button's top and bottom over the screen height.
            const Rect& b = p.buttons[static_cast<size_t>(p.focus)].rect;
            op.stick.select_top = b.t / kSceneHeight;
            op.stick.select_bottom = b.b / kSceneHeight;
            op.stick.select_opacity = 1.0f;
        }
        f.ui.push_back(std::move(op));
    }

    // Captions: the big one on the open column once it has finished opening, the small one elsewhere.
    for (int i = 0; i < kPanelCount; ++i) {
        const PanelState& p = panels_[static_cast<size_t>(i)];
        const Rect box = to_view(p.caption);
        float shadow[4];
        if (p.active && p.shown) {
            // TdLabelTextTitleThickWhite over TdLabelTextTitleThickDropShadow.
            ui_color(kShadowRGB[0], kShadowRGB[1], kShadowRGB[2], 0.5f, shadow);
            draw_label(f, assets_.headline, p.caption_text, box, 1, 1, white, shadow, 0.035f, 0.055f);
        } else {
            // TdLabelText_MenuButton_Normal over TdLabelText_MenuButton_DropShadow.
            ui_color(kShadowRGB[0], kShadowRGB[1], kShadowRGB[2], 0.7f, shadow);
            draw_label(f, assets_.medium_italic, p.caption_text, box, 1, 1, white, shadow, 0.06f, 0.1f);
        }
    }

    // The open column's sub-buttons: TdMainMenu_Sub-Button, black once focused.
    const PanelState& cur = panels_[static_cast<size_t>(current_panel_)];
    if (cur.shown) {
        float shadow[4];
        ui_color(kShadowRGB[0], kShadowRGB[1], kShadowRGB[2], 0.5f, shadow);
        for (size_t b = 0; b < cur.buttons.size(); ++b) {
            const bool focused = static_cast<int>(b) == cur.focus;
            draw_label(f, assets_.medium_italic, cur.buttons[b].caption, to_view(cur.buttons[b].rect), 0, 1,
                       focused ? black : white, shadow, 0.06f, 0.1f);
        }
    }

    // DescriptionLabel: columns 0 and 1 only, fading in two seconds after the selection settles.
    if (current_panel_ < 2 && !description_.empty()) {
        const float opacity = std::clamp((fade_timer_ - kTimeToFadeStart) / kFadeTime, 0.0f, 1.0f);
        if (opacity > 0.0f) {
            Rect r{748.8f, 54.0f, 1184.0f, 176.4f};
            assets_.menu_rect("DescriptionLabel", r);
            const Rect box = to_view(r);
            float shadow[4];
            ui_color(kNavyRGB[0], kNavyRGB[1], kNavyRGB[2], opacity, navy);
            ui_color(kShadowRGB[0], kShadowRGB[1], kShadowRGB[2], 0.34f * opacity, shadow);
            // CLIP_Wrap, right-aligned: break on spaces to the label's width.
            const Font& font = assets_.small_normal;
            std::vector<std::string> lines;
            std::string line;
            size_t pos = 0;
            while (pos <= description_.size()) {
                const size_t sp = description_.find(' ', pos);
                const std::string word = description_.substr(pos, sp == std::string::npos ? std::string::npos : sp - pos);
                const std::string trial = line.empty() ? word : line + " " + word;
                if (!line.empty() && font.width(trial) > box.w()) {
                    lines.push_back(line);
                    line = word;
                } else {
                    line = trial;
                }
                if (sp == std::string::npos) break;
                pos = sp + 1;
            }
            if (!line.empty()) lines.push_back(line);
            const float lh = static_cast<float>(font.line_height) * font.scale;
            for (size_t i = 0; i < lines.size(); ++i) {
                const Rect row{box.l, box.t + lh * static_cast<float>(i), box.r, box.t + lh * static_cast<float>(i + 1)};
                draw_label(f, font, lines[i], row, 2, 0, navy, shadow, 0.06f, 0.06f);
            }
        }
    }

    // TdUIButtonBar on PC without a controller: "Friends" at the right, then "Quit" 50 px to its left.
    Rect bar{96.0f, 635.4f, 1184.0f, 666.0f};
    assets_.menu_rect("ButtonBar", bar);
    const Rect bar_view = to_view(bar);
    const Font& bfont = assets_.small_normal;
    float right = bar_view.r;
    float shadow[4];
    ui_color(kShadowRGB[0], kShadowRGB[1], kShadowRGB[2], 0.5f, shadow);
    for (const char* key : {"Friends", "Quit"}) {
        const std::string label = strip_markup(assets_.text("TdButtonCallouts", key));
        if (label.empty()) continue;
        const float w = bfont.width(label);
        const Rect text{right - w, bar_view.t, right, bar_view.b};
        if (assets_.button.valid()) {
            // TdImageButtonBarBackground: button_full stretched around the auto-sized label.
            // StylePadding is -20; measured on a retail frame that is 20 px a side and 4.7 px
            // above and below at 720 lines.
            DrawOp op;
            op.kind = DrawOp::Kind::Image;
            op.image = &assets_.button;
            op.quads.push_back(Quad{text.l - 20.0f * scale_, text.t - 4.7f * scale_, text.r + 20.0f * scale_,
                                    text.b + 4.7f * scale_, 0.0f, 0.0f, 1.0f, 1.0f});
            f.ui.push_back(std::move(op));
        }
        draw_label(f, bfont, label, text, 0, 1, white, shadow, 0.06f, 0.06f);
        right = text.l - 50.0f * scale_;
    }
}

const Frame& Frontend::frame() {
    Frame& f = frame_;
    f.width = width_;
    f.height = height_;
    f.time = time_;
    f.white = kismet_.fade();
    f.district_selected.resize(assets_.city.districts.size());
    for (size_t i = 0; i < assets_.city.districts.size(); ++i) {
        f.district_selected[i] = kismet_.material_param(assets_.city.districts[i], "Selected", 0.0f);
    }
    f.display_gamma = gamma();
    f.ui.clear();
    if (const KismetActor* view = kismet_.view()) {
        const float pitch = view->euler.y * kPi / 180.0f, yaw = view->euler.z * kPi / 180.0f;
        f.camera = view->pos;
        f.target = view->pos + Vec3{std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch)} * 100.0f;
        f.roll = view->euler.x;
        f.fov = view->fov;
    }
    if (screen_ == Screen::Start) draw_start(f);
    else if (!scenes_.empty()) scenes_.back()->draw(f, scale_, origin_x_, gamma());
    else draw_menu(f);
    return f;
}

}  // namespace me::fe
