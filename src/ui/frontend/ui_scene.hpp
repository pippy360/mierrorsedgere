#pragma once

// -----------------------------------------------------------------------------
// UE3 UI scenes, read out of the retail packages and laid out and drawn the way the
// engine does: the widget tree with its docking, the skin's styles per widget state,
// the strings behind the markup. The screens the main menu opens (options, chapter
// select, time trial, unlockables, credits, message boxes) all run on this; what a
// scene *does* (its UnrealScript) lives in frontend_menus.cpp.
//
// docs/SUB_MENUS_RE.md describes the format.
// -----------------------------------------------------------------------------

#include "frame.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace me::fe {

// The widget states a style carries data for (UIState_* and TdUIState_FakeActive).
enum class UiState : uint8_t { Enabled, Disabled, Focused, Active, Pressed, FakeActive, TargetedTab, Count };
constexpr size_t kUiStates = static_cast<size_t>(UiState::Count);

enum UiAlign : uint8_t { kAlignLeft = 0, kAlignCenter = 1, kAlignRight = 2, kAlignDefault = 3 };
enum UiAdjust : uint8_t { kAdjustNone = 0, kAdjustNormal = 1, kAdjustJustified = 2, kAdjustBound = 3, kAdjustStretch = 4 };

// UIStyle_Text.
struct UiTextStyle {
    bool valid = false;
    const Font* font = nullptr;
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};  // linear, as stored
    uint8_t align[2] = {kAlignLeft, kAlignCenter};
    bool wrap = false;  // ClipMode == CLIP_Wrap
};

// UIStyle_Image.
struct UiImageStyle {
    bool valid = false;
    const Image* image = nullptr;  // DefaultImage
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    uint8_t adjust[2] = {kAdjustNormal, kAdjustNormal};
    uint8_t align[2] = {kAlignLeft, kAlignLeft};
    float uv[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // Coordinates: U, V, UL, VL in texels; UL/VL 0 = the whole texture
    float padding[2] = {0.0f, 0.0f};         // StylePadding: pixels taken off each side; negative grows the image
};

// One UIStyle of the skin, resolved for every state.
struct UiStyle {
    std::string tag;
    std::array<UiTextStyle, kUiStates> text{};
    std::array<UiImageStyle, kUiStates> image{};
    [[nodiscard]] const UiTextStyle& text_for(UiState s) const {
        const UiTextStyle& t = text[static_cast<size_t>(s)];
        return t.valid ? t : text[0];
    }
    [[nodiscard]] const UiImageStyle& image_for(UiState s) const {
        const UiImageStyle& t = image[static_cast<size_t>(s)];
        return t.valid ? t : image[0];
    }
};

// UIComp_DrawString / UIComp_TdDropShadowString.
struct UiStringComp {
    bool present = false;
    const UiStyle* style = nullptr;
    const UiStyle* shadow = nullptr;  // DropShadowStyle
    float shadow_h = 0.06f;           // HorizontalPctOffset
    float shadow_v = 0.06f;           // VerticalPctOffset
    int8_t align[2] = {-1, -1};       // TextStyleCustomization.TextAlignment where it overrides the style
    bool autosize[2] = {false, false};
};

// UIComp_DrawImage.
struct UiImageComp {
    bool present = false;
    const UiStyle* style = nullptr;
    const Image* texture = nullptr;  // ImageRef.ImageTexture, which replaces the style's DefaultImage
    float opacity = 1.0f;
    std::string resolver;            // StyleResolverTag: "IncrementStyle" means the owner widget supplies the style
};

struct UiWidget {
    std::string name;
    std::string cls;
    int parent = -1;
    std::vector<int> children;

    // UIScreenObject.Position and UIObject.DockTargets.
    float pos[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    uint8_t pos_type[4] = {0, 0, 0, 0};
    int dock_widget[4] = {-1, -1, -1, -1};  // -1 not docked, -2 the scene
    uint8_t dock_face[4] = {4, 4, 4, 4};    // 4 = UIFACE_MAX
    float dock_pad[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    uint8_t dock_pad_type[4] = {0, 0, 0, 0};
    Rect rect;  // resolved, in the scene's 1280x720 pixels

    bool hidden = false;
    float opacity = 1.0f;
    float zdepth = 0.0f;
    int tab_index = 0;
    int forced_nav[4] = {-1, -1, -1, -1};  // NavigationTargets.ForcedNavigationTarget by face

    std::string markup;  // DataSource.MarkupString as authored
    std::string text;    // what the string component draws now
    UiStringComp string;
    UiImageComp image;       // BackgroundImageComponent / ImageComponent
    UiImageComp bar;         // UISlider.SliderBarImageComponent
    UiImageComp marker;      // UISlider.MarkerImageComponent
    const UiStyle* increment = nullptr;  // UITdOptionButton.IncrementStyle
    const UiStyle* decrement = nullptr;
    float slider[4] = {0.0f, 1.0f, 0.0f, 1.0f};  // UISlider.SliderValue: min, max, current, nudge
    float bar_size = 8.0f;
    float marker_width = 0.1f;   // of the slider's width
    float marker_height = 8.0f;

    // State.
    bool disabled = false;
    bool focused = false;
    bool hover = false;    // UIState_Active
    bool pressed = false;
    bool fake_active = false;
    [[nodiscard]] UiState state() const {
        if (disabled) return UiState::Disabled;
        if (pressed) return UiState::Pressed;
        if (hover) return UiState::Active;
        if (focused) return UiState::Focused;
        if (fake_active) return UiState::FakeActive;
        return UiState::Enabled;
    }
};

// One TdProfileSettings entry: what "<OnlinePlayerData:ProfileData.GameDifficulty>" binds a widget to.
struct ProfileSetting {
    int id = 0;
    std::string name;
    bool id_mapped = false;                // PVMT_IdMapped: one of value_ids; else a raw integer
    std::vector<int> value_ids;
    std::vector<std::string> value_names;  // markup, in ValueMappings order
    int value = 0;
    int default_value = 0;
};

struct ProfileSettings {
    std::vector<ProfileSetting> settings;
    ProfileSetting* find(const std::string& name) {
        for (ProfileSetting& s : settings) {
            if (s.name == name) return &s;
        }
        return nullptr;
    }
    [[nodiscard]] int get(const std::string& name, int fallback = 0) const {
        for (const ProfileSetting& s : settings) {
            if (s.name == name) return s.value;
        }
        return fallback;
    }
};

class UiSystem;

// One loaded scene. Widget 0 is the scene itself.
class UiScene {
public:
    std::string name;
    std::string cls;
    std::vector<UiWidget> widgets;

    [[nodiscard]] int find(const std::string& widget) const;  // FindChild(name, true); -1 if absent
    UiWidget* get(const std::string& widget) {
        const int i = find(widget);
        return i < 0 ? nullptr : &widgets[static_cast<size_t>(i)];
    }
    // SetDataStoreBinding / SetValue: markup is resolved, plain text taken as it is.
    void set_text(UiSystem& ui, const std::string& widget, const std::string& markup);
    void set_visible(const std::string& widget, bool visible);
    [[nodiscard]] bool visible(int widget) const;  // it and every ancestor

    // Resolves every widget's rectangle. Call after text or visibility changed.
    void layout();
    // The scene as draw operations. `scale` and `origin_x` place the 1280x720 scene in the viewport.
    void draw(Frame& f, float scale, float origin_x, float gamma) const;

    // A TdUIButtonBar's visible buttons, right to left as AppendButton fills them.
    struct BarButton {
        std::string label;
        bool disabled = false;
        Rect rect;  // the red box, scene pixels (set by draw's layout)
    };
    std::vector<std::pair<int, std::vector<BarButton>>> button_bars;  // widget index, its buttons
    std::vector<BarButton>& bar(const std::string& widget);

    float view_scale = 1.0f;           // viewport pixels per scene pixel: fonts are measured in viewport pixels
    const Font* bar_font = nullptr;    // TdUIButtonBarButton's string style
    const Image* bar_image = nullptr;  // TdImageButtonBarBackground
    float bar_padding[2] = {20.0f, 3.0f};  // how far the box reaches past the label (the StylePadding of that style)
    float bar_text[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float bar_shadow[4] = {0.0f, 0.0f, 0.0f, 0.0f};

private:
    friend class UiSystem;
    void draw_widget(Frame& f, int index, float scale, float origin_x, float gamma, float opacity) const;
    std::vector<float> face_value_;  // every widget's four faces as last resolved
};

// Loads scenes and styles. One per front end; it borrows the Assets.
class UiSystem {
public:
    UiSystem();
    ~UiSystem();
    void init(Assets* assets, float view_scale);

    // A scene by package and name ("TdUI_Options", "TdGameSettings"). Null if it cannot be read.
    std::unique_ptr<UiScene> load_scene(const std::string& package, const std::string& scene);
    // "<Strings:TdGameUI.TdMainMenu.StoryCaptionText>" -> "STORY". Unknown tags are dropped; "\n" becomes a line break.
    [[nodiscard]] std::string resolve_markup(const std::string& markup) const;
    const UiStyle* style_by_tag(const std::string& tag);
    // TdGame.Default__TdProfileSettings: every setting with its values and default.
    bool load_profile_settings(ProfileSettings& out);
    [[nodiscard]] Assets* assets() const { return assets_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Assets* assets_ = nullptr;
    float view_scale_ = 1.0f;
};

// Text helpers shared by layout and drawing.
std::vector<std::string> ui_wrap(const Font& font, const std::string& text, float width, bool wrap);
// Draws `text` in `box` (viewport pixels): alignment 0 left/top, 1 centre, 2 right/bottom. `color` is linear.
void ui_draw_text(Frame& f, const Font& font, const std::string& text, const Rect& box, int halign, int valign, bool wrap,
                  const float color[4], const float* shadow_color, float shadow_h, float shadow_v, float gamma, const Rect* clip = nullptr);
// Draws `image` over `box` (viewport pixels) scaled to fit, tinted with a linear colour.
void ui_draw_image(Frame& f, const Image& image, const Rect& box, const float uv[4], const float color[4], float gamma, const Rect* clip = nullptr);
// UCanvas::DrawTileStretched: the four quarters of the image keep their size in the corners of
// `box` and its middle row and column are stretched between them.
void ui_draw_image_stretched(Frame& f, const Image& image, const Rect& box, const float uv[4], const float color[4], float gamma,
                             const Rect* clip = nullptr);

}  // namespace me::fe
