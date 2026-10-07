#pragma once

// -----------------------------------------------------------------------------
// What the front end ("Press Any Key" and the main menu) reads out of the retail packages:
// the MultiFonts, the UI textures, the two scenes' widget rectangles, the localized strings,
// the camera Matinees and the menu level's city. Plain C++, no window, GPU or audio.
//
// docs/MAIN_MENU_SYSTEM_RE.md says where each of these comes from.
// -----------------------------------------------------------------------------

#include "../../math/types.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace me::fe {

// RGBA8, top row first.
struct Image {
    int w = 0;
    int h = 0;
    bool wrap_x = true;  // TA_Wrap, else TA_Clamp
    bool wrap_y = true;
    bool srgb = true;    // Texture2D.SRGB: a material sampling it gets linear values
    std::vector<uint8_t> px;
    [[nodiscard]] bool valid() const { return w > 0 && h > 0 && px.size() == static_cast<size_t>(w) * h * 4; }
};

// One FontCharacter of a MultiFont tier.
struct Glyph {
    int u = 0, v = 0, w = 0, h = 0;
    int page = 0;
    int voff = 0;
};

// One resolution tier of a MultiFont, picked for the viewport height.
struct Font {
    std::string name;
    float scale = 1.0f;   // viewport height / tier height: 1 at 480, 720 and 1080 lines
    int line_height = 0;  // the tier's tallest glyph cell (UFont MaxCharHeight), unscaled
    int spacing = 0;      // Font.Kerning: pixels after every glyph
    std::array<Glyph, 256> glyphs{};
    std::vector<Image> pages;                    // glyph coverage is the alpha channel
    std::unordered_map<uint32_t, float> pairs;   // (first << 16 | second) -> kerning, unscaled

    [[nodiscard]] bool valid() const { return !pages.empty() && line_height > 0; }
    // Pen advance after `c` when `next` follows it (0 = nothing follows), scaled.
    [[nodiscard]] float advance(unsigned char c, unsigned char next) const;
    [[nodiscard]] float width(const std::string& latin1) const;
};

struct Rect {
    float l = 0.0f, t = 0.0f, r = 0.0f, b = 0.0f;
    [[nodiscard]] float w() const { return r - l; }
    [[nodiscard]] float h() const { return b - t; }
};

// UE3 EInterpCurveMode.
enum class CurveMode : uint8_t { Linear, CurveAuto, Constant, CurveUser, CurveBreak };

struct CurveKey {
    float t = 0.0f;
    Vec3 v{0.0f, 0.0f, 0.0f};
    Vec3 arrive{0.0f, 0.0f, 0.0f};
    Vec3 leave{0.0f, 0.0f, 0.0f};
    CurveMode mode = CurveMode::Linear;
};

// FInterpCurve<FVector>. A float curve keeps its value in x.
struct Curve {
    std::vector<CurveKey> keys;
    [[nodiscard]] Vec3 eval(float t, const Vec3& fallback = Vec3{0.0f, 0.0f, 0.0f}) const;
};

// One camera Matinee: the Camera group's position and FOV, and the Target group it looks at.
struct Matinee {
    std::string name;
    float length = 0.0f;
    Curve camera;
    Curve target;
    Curve fov;
    std::vector<std::pair<float, std::string>> events;
    [[nodiscard]] bool valid() const { return length > 0.0f && !camera.keys.empty(); }
};

enum class CityMaterial : uint8_t { Buildings, Base, Water, Waves, Sky };

struct CityVertex {
    Vec3 pos{0.0f, 0.0f, 0.0f};
    Vec3 normal{0.0f, 0.0f, 1.0f};
    float uv[3][2] = {{0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}};
    float color[3] = {1.0f, 1.0f, 1.0f};  // vertex colour, 0..1
};

// The triangles of one mesh element, in world space.
struct CityBatch {
    std::string mesh;
    CityMaterial material = CityMaterial::Buildings;
    std::vector<CityVertex> tris;  // three per triangle
};

struct City {
    std::vector<CityBatch> batches;
    Image fade;   // UI_City.T_CityFade_01_A
    Image sky;    // UI_City.T_Skydome_Menu
    Image waves;  // UI_City.T_Waves_01_A
    Vec3 sun_dir{0.0f, 0.0f, 1.0f};  // towards the sun
    float water_z = 0.0f;            // the reflection plane (SceneCaptureReflectActor)
    [[nodiscard]] bool valid() const { return !batches.empty(); }
};

struct Assets {
    // `viewport_height` picks the font tier. Returns false and sets `error` when the retail
    // install is missing something the front end cannot do without.
    bool load(const std::string& game_root, int viewport_height, std::string& error);

    // A string of Localization/INT/TdGameUI.int, as Latin-1 (the fonts' encoding).
    [[nodiscard]] std::string text(const std::string& section, const std::string& key) const;
    // A widget's rectangle in the scene's own 1280x720 pixels. False if the scene has no such widget.
    bool start_rect(const std::string& widget, Rect& out) const;
    bool menu_rect(const std::string& widget, Rect& out) const;

    Font small_bold;      // Helvetica_Small_Bold: the start screen
    Font small_normal;    // Helvetica_Small_Normal: the description, the button bar
    Font medium_italic;   // Helvetica_Medium_Italic: sub-buttons, the unselected captions
    Font headline;        // Helvetica_Headline_Light_Italic: the selected caption

    Image title;           // TdUIResources.Scene.StartTitleImage
    Image button;          // TdUIResources.button_full
    Image stick_left;      // UI_Menus.T_StickMaskLeft_01
    Image stick_right;     // UI_Menus.T_StickMaskRight_01
    Image stick_shadow;    // UI_Menus.T_StickMaskRightShadow_01
    Image stick_timeline;  // UI_Menus.T_StickMovementTimeline_01

    Matinee opening;                 // the start screen's shot
    std::array<Matinee, 4> intro{};  // per column: the 0.35 s move in
    std::array<Matinee, 4> loop{};   // per column: the 60 s loop

    City city;

    float time_till_start_button = 4.0f;    // DefaultUI.ini [TdGame.TdUIScene_Start]
    float time_till_attract_movie = 90.0f;

    std::vector<std::string> warnings;  // things that were missing but not fatal

private:
    std::unordered_map<std::string, Rect> start_rects_;
    std::unordered_map<std::string, Rect> menu_rects_;
    std::unordered_map<std::string, std::string> strings_;  // "Section.Key" -> text
};

}  // namespace me::fe
