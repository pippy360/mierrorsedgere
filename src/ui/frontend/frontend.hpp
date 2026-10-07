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

#include "frontend_assets.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace me::fe {

// What the canvas does to a linear colour on its way to the back buffer:
// pow(max(c, KINDA_SMALL_NUMBER), 1 / DisplayGamma). Measured on retail frames the gamma is 2.73,
// not the 2.2 of TdEngine.ini: TdPlayerController.SetVideoProfileSettings replaces it with the
// profile's Brightness, and this is what a default profile gives. The floor is why "black" UI
// text is (9, 9, 9). Materials drawn in the UI (the columns) do not go through it.
inline float canvas_encode(float linear) {
    const float c = linear < 1.0e-4f ? 1.0e-4f : (linear > 1.0f ? 1.0f : linear);
    return std::pow(c, 1.0f / 2.73f);
}

enum class Screen : uint8_t { Start, MainMenu };
enum class Key : uint8_t { Other, Left, Right, Up, Down, Accept, Escape };

// The four columns, ETdMainMenuPanel.
enum Panel : int { kStory = 0, kTimeTrial = 1, kOptions = 2, kExtras = 3, kPanelCount = 4 };

// The scalar parameters TdMenuPostProcesWrapper sets on one M_MainMenuStick_01 instance.
struct StickParams {
    float width = 0.02f;         // StickWidth
    float select_top = 0.0f;     // SelectTop, fraction of the screen height
    float select_bottom = 0.0f;  // SelectBottom
    float select_opacity = 0.0f; // SelectOpacity
    float move_offset = 0.0f;    // MovementOffset
    float move_amount = 1.0f;    // MovementAmount
    float left_offset = 0.0f;    // LeftSideOffset
    float right_offset = 0.0f;   // RightSideOffset
};

struct Quad {
    float x0, y0, x1, y1;  // viewport pixels
    float u0, v0, u1, v1;  // 0..1 across the image
};

struct DrawOp {
    enum class Kind : uint8_t {
        Image,   // quads of `image` through the canvas (sRGB texel -> linear -> canvas_encode), times `color`
        Glyphs,  // quads of a font page: `color` with the page's alpha as coverage
        Stick,   // M_MainMenuStick_01 over `rect` with `stick`
    };
    Kind kind = Kind::Image;
    const Image* image = nullptr;
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};  // display space, straight alpha
    std::vector<Quad> quads;
    Rect rect;
    StickParams stick;
};

// Everything a renderer needs for one frame.
struct Frame {
    int width = 1280;
    int height = 720;
    double time = 0.0;  // seconds the front end has run: the stick material's Time
    Vec3 camera{0.0f, 0.0f, 0.0f};
    Vec3 target{0.0f, 1.0f, 0.0f};
    float fov = 90.0f;   // horizontal, degrees (CameraActor.FOVAngle)
    float white = 0.0f;  // SeqAct_TdFadeEffect: 0 = clear, 1 = white
    std::vector<DrawOp> ui;
};

// What the save file would tell TdUIScene_MainMenu.InitializeWidgetsData.
struct Profile {
    bool can_continue = true;         // CONTINUE GAME
    bool chapters_unlocked = true;    // PLAY CHAPTER
    bool all_levels_unlocked = true;  // SPEED RUN
    bool controller = false;          // GAMEPAD SETUP, and "Accept" in the button bar
};

class Frontend {
public:
    bool init(const std::string& game_root, int width, int height, std::string& error);

    void set_profile(const Profile& p) { profile_ = p; }
    // LeftSideOffset / RightSideOffset are FRand() per boot in retail. Fixing them makes frames repeatable.
    void set_stick_offsets(const std::array<float, kPanelCount>& left, const std::array<float, kPanelCount>& right);

    void update(float dt);
    void key_down(Key key);
    void key_up(Key key);
    void mouse_move(float x, float y);
    void mouse_click(float x, float y);

    // Jumps straight to the main menu, as if a key had been released on the start screen.
    void open_main_menu();

    [[nodiscard]] const Frame& frame();

    [[nodiscard]] Screen screen() const { return screen_; }
    [[nodiscard]] int panel() const { return current_panel_; }
    // The focused sub-button's widget name ("LoadGameButton"), or "" while a column is opening.
    [[nodiscard]] std::string focused_button() const;
    [[nodiscard]] bool animating() const { return animating_; }
    [[nodiscard]] const Assets& assets() const { return assets_; }

    // UI sound cue names played since the last call ("TabChangeRight", "NavigateDown", "Accept"),
    // and "Music" once when the menu music should start.
    std::vector<std::string> take_sounds();
    // The sub-button chosen since the last call ("NewGameButton", ...), "Quit" or "Friends"; "" if none.
    std::string take_action();

private:
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
    void play_camera(const Matinee* first, const Matinee* then);
    void update_camera(float dt);
    void sound(const char* cue) { sounds_.emplace_back(cue); }
    int button_at(float x, float y) const;

    void draw_start(Frame& f) const;
    void draw_menu(Frame& f) const;
    void draw_text(Frame& f, const Font& font, const std::string& text, float x, float y, const float color[4]) const;
    // Text in `box`, aligned 0 = left/top, 1 = centre, 2 = right/bottom, with an optional drop shadow.
    void draw_label(Frame& f, const Font& font, const std::string& text, const Rect& box, int halign, int valign,
                    const float color[4], const float* shadow_color, float shadow_dx, float shadow_dy) const;
    [[nodiscard]] Rect to_view(const Rect& scene) const;

    Assets assets_;
    Profile profile_;
    Frame frame_;
    int width_ = 1280;
    int height_ = 720;
    float scale_ = 1.0f;     // viewport pixels per scene pixel
    float origin_x_ = 0.0f;  // where the 16:9 scene starts in a wider viewport

    Screen screen_ = Screen::Start;
    double time_ = 0.0;
    float time_in_scene_ = 0.0f;  // TdUIScene_Start.TimeElapsedInScene
    bool music_started_ = false;

    // Camera: the Matinee playing now and the one that follows it (a column's intro, then its loop).
    const Matinee* matinee_ = nullptr;
    const Matinee* next_matinee_ = nullptr;
    float matinee_time_ = 0.0f;
    bool matinee_loops_ = false;
    float white_ = 1.0f;       // current SeqAct_TdFadeEffect level
    float white_rate_ = 0.0f;  // per second; negative fades in

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
