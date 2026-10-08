#pragma once

// -----------------------------------------------------------------------------
// The front end: the "Press Any Key" screen (TdUIScene_Start) and the main menu
// (TdUIScene_MainMenu with its TdMenuPostProcesWrapper), as a state machine that
// takes input and time and produces one Frame per call: where the menu level's
// camera is, and the list of 2D things to draw on top. It draws nothing itself;
// soft_render.hpp is the reference renderer for a Frame.
//
// docs/MAIN_MENU_SYSTEM_RE.md is the description of what retail does.
// -----------------------------------------------------------------------------

#include "frame.hpp"
#include "frontend_assets.hpp"
#include "kismet.hpp"
#include "ui_scene.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace me::fe {

class SubMenu;

enum class Screen : uint8_t { Start, MainMenu };
// PrevPage and NextPage are the gamepad's shoulders (a tab control's pages); Reset is its X (DEFAULTS).
enum class Key : uint8_t { Other, Left, Right, Up, Down, Accept, Escape, PrevPage, NextPage, Reset };

// The four columns, ETdMainMenuPanel.
enum Panel : int { kStory = 0, kTimeTrial = 1, kOptions = 2, kExtras = 3, kPanelCount = 4 };

// What the save file would tell TdUIScene_MainMenu.InitializeWidgetsData.
struct Profile {
    bool can_continue = true;         // CONTINUE GAME
    bool chapters_unlocked = true;    // PLAY CHAPTER
    bool all_levels_unlocked = false; // SPEED RUN
    bool controller = false;          // GAMEPAD SETUP, and "Accept" in the button bar
    uint32_t unlocked_levels = 0x7FF; // PLAY CHAPTER's list: bit i is Assets::maps[i]
    bool hard_unlocked = false;       // the story was finished: HARD is offered
    uint32_t time_trials = 0xFFFFFFFFu;  // TIME TRIAL: bit i is Assets::time_trials[i], unlocked
    uint32_t level_races = 0xFFFFFFFFu;  // SPEED RUN: bit i is Assets::level_races[i], unlocked
    int stars = 0;                       // GetTimeTrialRating: stars earned over all the courses
    std::string player_name = "Player";
};

// One list of UIDataStore_TdStringList: what "<TdStringList:VSync>" binds an option button to.
struct StringList {
    std::vector<std::string> values;
    int index = 0;
};

class Frontend {
public:
    Frontend();
    ~Frontend();
    bool init(const std::string& game_root, int width, int height, std::string& error);

    void set_profile(const Profile& p) { profile_ = p; }
    // LeftSideOffset / RightSideOffset are FRand() per boot in retail. Fixing them makes frames repeatable.
    void set_stick_offsets(const std::array<float, kPanelCount>& left, const std::array<float, kPanelCount>& right);

    void update(float dt);
    // `name` is the key's engine name ("W", "SpaceBar", "LeftShift", "RightMouseButton"), for the
    // screens that care which key it was: CONTROLS binds it. Without it only Escape can be told.
    void key_down(Key key, const std::string& name = {});
    void key_up(Key key, const std::string& name = {});
    void mouse_move(float x, float y);
    void mouse_click(float x, float y);  // the left button

    // TdUIScene.ActivateLevelEvent: the level's Kismet does the rest (the camera, the fades).
    void level_event(const std::string& name) { kismet_.fire_event(name); }

    // Jumps straight to the main menu, as if a key had been released on the start screen.
    void open_main_menu();

    [[nodiscard]] const Frame& frame();

    [[nodiscard]] Screen screen() const { return screen_; }
    [[nodiscard]] int panel() const { return current_panel_; }
    // The focused sub-button's widget name ("LoadGameButton"), or "" while a column is opening.
    [[nodiscard]] std::string focused_button() const;
    [[nodiscard]] bool animating() const { return animating_; }
    // The scene open on top of the main menu ("TdGameSettings", "TdMessageBox"), or "" if none, and its focused widget.
    [[nodiscard]] std::string scene_name() const;
    [[nodiscard]] std::string scene_focus() const;
    [[nodiscard]] const UiScene* scene() const;
    // The profile's settings (TdProfileSettings) and the PC string lists (resolution, texture detail, ...).
    [[nodiscard]] ProfileSettings& settings() { return settings_; }
    [[nodiscard]] StringList& string_list(const std::string& tag) { return string_lists_[tag]; }
    // PlayerInput.Bindings as the CONTROLS screen last saved them; the retail defaults until then.
    [[nodiscard]] const std::vector<KeyBinding>& bindings() const { return bindings_; }
    [[nodiscard]] const Assets& assets() const { return assets_; }
    // Where a map of the chapter list is, relative to CookedPC ("edge_p" -> "Maps/SP01/Edge_p.me1"); "" if it is not installed.
    [[nodiscard]] std::string map_path(const std::string& file) const;
    // A Texture2D of the retail packages by object path, read on first use (Assets::image).
    const Image* image(const std::string& object_path) { return assets_.image(object_path); }
    const Font* font(const std::string& name) { return assets_.font(name); }
    [[nodiscard]] const KismetRunner& kismet() const { return kismet_; }

    // UI sound cue names played since the last call ("TabChangeRight", "NavigateDown", "Accept"),
    // and "Music" once when the menu music should start.
    std::vector<std::string> take_sounds();
    // What the host should do, since the last call; "" if nothing:
    //   "Continue"                      CONTINUE GAME
    //   "NewGame"                       a new game at the difficulty now in settings()
    //   "StartLevel <map> [checkpoint]" PLAY CHAPTER ("StartLevel edge_p After_Intro")
    //   "TimeTrial <stretch>"           START RACE
    //   "ApplySettings"                 an options screen was saved
    //   "Quit"
    std::string take_action();

private:
    friend class SubMenu;
    // Scenes on top of the main menu. The last one is drawn and takes the input.
    void open_scene(std::unique_ptr<SubMenu> menu);
    void close_scene(SubMenu* menu, const std::function<void()>& then);
    void button_clicked(const std::string& widget);  // TdUIScene_MainMenu.HandleButtonClicked
    void quit_clicked();                             // OnQuitGame
    void online_check(const std::string& offline_scene);  // TdUIScene_MainMenu's LoginHandler

    struct Button {
        std::string widget;       // "LoadGameButton"
        std::string caption;
        std::string description;  // "" on the columns that show none
        Rect rect;
    };
    struct PanelState {
        std::vector<Button> buttons;  // top to bottom, hidden ones left out
        Rect caption;                 // StoryCaptionButton
        Rect background;              // StoryPanelBGImage
        std::string caption_text;
        float anim = 0.0f;            // TdMenuPostProcesWrapper AnimPosition
        bool active = false;
        bool shown = false;           // the panel's buttons are visible (its animation finished)
        bool animated = false;        // UpdatePanelAnimation has set StickWidth at least once
        int focus = 0;
        float left_offset = 0.0f;
        float right_offset = 0.0f;
    };

    void build_panels();
    void set_active_panel(int index, bool silent);
    void panel_anim_finished(int index);
    void sound(const char* cue) { sounds_.emplace_back(cue); }
    int button_at(float x, float y) const;

    [[nodiscard]] float gamma() const { return screen_ == Screen::Start ? kStartGamma : kProfileGamma; }
    void ui_color(float r, float g, float b, float a, float out[4]) const;
    void draw_start(Frame& f) const;
    void draw_menu(Frame& f) const;
    void draw_text(Frame& f, const Font& font, const std::string& text, float x, float y, const float color[4]) const;
    // Text in `box`, aligned 0 = left/top, 1 = centre, 2 = right/bottom, with an optional drop shadow.
    void draw_label(Frame& f, const Font& font, const std::string& text, const Rect& box, int halign, int valign,
                    const float color[4], const float* shadow_color, float shadow_dx, float shadow_dy) const;
    [[nodiscard]] Rect to_view(const Rect& scene) const;

    Assets assets_;
    UiSystem ui_;
    ProfileSettings settings_;
    std::unordered_map<std::string, StringList> string_lists_;
    std::vector<KeyBinding> bindings_;
    std::vector<std::unique_ptr<SubMenu>> scenes_;
    std::vector<std::unique_ptr<SubMenu>> closed_;  // closed during this update; destroyed at its end
    Profile profile_;
    Frame frame_;
    int width_ = 1280;
    int height_ = 720;
    float scale_ = 1.0f;     // viewport pixels per scene pixel
    float origin_x_ = 0.0f;  // where the 16:9 scene starts in a wider viewport

    Screen screen_ = Screen::Start;
    double time_ = 0.0;
    float time_in_scene_ = 0.0f;  // TdUIScene_Start.TimeElapsedInScene

    KismetRunner kismet_;  // the menu level's Kismet: the camera, the fades

    std::array<PanelState, kPanelCount> panels_{};
    int current_panel_ = -1;
    int last_panel_ = -1;
    bool animating_ = false;
    bool initial_tick_ = true;
    float fade_timer_ = 0.0f;  // TdUIScene_MainMenu.FadeTimer
    std::string description_;
    int hovered_ = -1;

    std::vector<std::string> sounds_;
    std::string action_;
};

}  // namespace me::fe
