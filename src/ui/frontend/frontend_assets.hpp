#pragma once

// -----------------------------------------------------------------------------
// What the front end ("Press Any Key" and the main menu) reads out of the retail packages:
// the MultiFonts, the UI textures, the two scenes' widget rectangles, the localized strings,
// the camera Matinees and the menu level's city. Plain C++, no window, GPU or audio.
//
// docs/MAIN_MENU_SYSTEM_RE.md says where each of these comes from.
// -----------------------------------------------------------------------------

#include "../../math/types.hpp"
#include "curve.hpp"
#include "kismet.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace me {
class PackageManager;
}

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

enum class CityMaterial : uint8_t { Buildings, Base, Water, Waves, Sky };

struct CityVertex {
    Vec3 pos{0.0f, 0.0f, 0.0f};
    Vec3 normal{0.0f, 0.0f, 1.0f};
    float uv[3][2] = {{0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}};
    float color[3] = {1.0f, 1.0f, 1.0f};  // vertex colour, 0..1
};

// One StaticMeshComponent's baked lighting: its three directional light-map textures already
// summed for a surface whose normal is the vertex normal (which is all the menu's materials use).
struct LightMap {
    int w = 0;
    int h = 0;
    std::vector<float> rgb;  // linear radiance, three per texel
    float scale[2] = {1.0f, 1.0f};  // FLightMap2D CoordinateScale
    float bias[2] = {0.0f, 0.0f};   // FLightMap2D CoordinateBias
    [[nodiscard]] bool valid() const { return w > 0 && h > 0 && rgb.size() == static_cast<size_t>(w) * h * 3; }
};

// The triangles of one mesh element, in world space.
struct CityBatch {
    std::string mesh;
    CityMaterial material = CityMaterial::Buildings;
    int lightmap = -1;             // index into City::lightmaps, sampled with uv[1]
    int district = -1;             // index into City::districts: which MI_SP<nn>_01 a building batch is drawn with
    std::vector<CityVertex> tris;  // three per triangle
};

struct City {
    std::vector<CityBatch> batches;
    std::vector<LightMap> lightmaps;
    // The instances of M_CityBuildings_01 in use. Kismet sets their "Selected" parameter on the
    // chapter-select screen: the chapter's district turns red.
    std::vector<std::string> districts;
    Image fade;   // UI_City.T_CityFade_01_A
    Image sky;    // UI_City.T_Skydome_Menu
    Image waves;  // UI_City.T_Waves_01_A
    Vec3 sun_dir{0.0f, 0.0f, 1.0f};  // towards the sun
    float water_z = 0.0f;            // the reflection plane (SceneCaptureReflectActor)
    // WorldInfo.DefaultPostProcessSettings: Bloom_Scale, and Curves, the colour curve as linear
    // pieces per channel: out = m[i] * in + b[i] with i = floor(in * 15). (Consecutive pieces
    // meet exactly at the fifteenths; piece 15 is only reached at in = 1.)
    float bloom_scale = 0.0f;
    float curve_m[16][3];
    float curve_b[16][3];
    City() {
        for (int i = 0; i < 16; ++i) {
            for (int c = 0; c < 3; ++c) {
                curve_m[i][c] = 1.0f;
                curve_b[i][c] = 0.0f;
            }
        }
    }
    [[nodiscard]] bool valid() const { return !batches.empty(); }
};

// One UIDataProvider_TdMaps of DefaultGame.ini: a chapter of the PLAY CHAPTER list.
struct MapCheckpoint {
    std::string name;      // CheckpointName, what the level is started at
    std::string image;     // CheckpointImageMarkup's texture path
    std::string friendly;  // "CHECKPOINT B"
    std::string description;
};

struct MapProvider {
    std::string id;           // "SP01a"
    std::string file;         // FileName: "edge_p"
    std::string level_event;  // the level event the chapter-select screen fires: "LoadLevel_Edge"
    std::string game_mode;
    std::string name;         // localized MapName: "PROLOGUE - THE EDGE"
    std::vector<MapCheckpoint> checkpoints;
};

// One list of UIDataStore_TdStringList: its tag (DefaultGame.ini) and its strings (TdGame.int).
struct StringListData {
    std::string tag;
    int default_index = 0;
    std::vector<std::string> strings;
};

// One UIDataProvider_TdKeyBinding of DefaultGame.ini: a row of the CONTROLS screen.
struct KeyAction {
    std::string id;        // "MoveForward": the widgets are KeyBindLabel_<id> and KeyBindButton_<id>_0 / _1
    std::string command;   // "GBA_MoveForward"
    std::string friendly;  // "MOVE FORWARD"
};

// One UIDataProvider_TdTimeTrialStretch or UIDataProvider_TdLevelRaceStretch of DefaultGame.ini: a
// course of TIME TRIAL, a chapter of SPEED RUN.
struct RaceStretch {
    std::string id;        // "TT_STRETCH0"
    std::string map;       // MapFilename: "tt_TutorialA01_p", "edge_p?LoadCheckpoint=Edge_Start"
    std::string name;      // FriendlyName: "PLAYGROUND ONE"
    std::string unlock;    // UnlockDesc
    float qualifying = 0.0f;                 // QualifyingTime, seconds
    float rating[3] = {0.0f, 0.0f, 0.0f};    // Rating1Time..Rating3Time: one, two and three stars
};

// One PlayerInput.Bindings entry.
struct KeyBinding {
    std::string key;      // "W", "SpaceBar", "LeftMouseButton"
    std::string command;  // "GBA_MoveForward"
};

struct Assets {
    // `viewport_height` picks the font tier. Returns false and sets `error` when the retail
    // install is missing something the front end cannot do without.
    bool load(const std::string& game_root, int viewport_height, std::string& error);

    // A string of Localization/INT/TdGameUI.int, as Latin-1 (the fonts' encoding).
    [[nodiscard]] std::string text(const std::string& section, const std::string& key) const;
    // A string by the path a "<Strings:File.Section.Key>" markup carries ("TdGameUI.TdMainMenu.StoryCaptionText").
    [[nodiscard]] std::string localized(const std::string& path) const;
    // Any MultiFont of UI/UI_Fonts_Final.upk, read on first use ("Helvetica_Headline_Thick_Italic"). Null if it is not there.
    const Font* font(const std::string& name);
    // A Texture2D by object path ("TdUIResources.Scene.Panel512x512"), read on first use. Null if it cannot be read.
    const Image* image(const std::string& object_path);
    // The packages under CookedPC, for the UI scene loader.
    [[nodiscard]] PackageManager* packages() const { return pm_.get(); }
    [[nodiscard]] const std::string& game_root() const { return game_root_; }
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

    std::vector<MapProvider> maps;  // in DefaultGame.ini order: the Training Area, then the story
    std::vector<StringListData> string_lists;
    std::vector<KeyAction> key_actions;
    std::vector<RaceStretch> time_trials;
    std::vector<RaceStretch> level_races;
    std::vector<KeyBinding> default_bindings;  // DefaultInput.ini [Engine.PlayerInput] Bindings, in file order
    // What a key is called on the CONTROLS screen ("SpaceBar" -> "SPACE"); "" for a key that cannot be bound.
    [[nodiscard]] std::string key_label(const std::string& key) const;

    City city;
    KismetGraph kismet;  // the menu level's Main_Sequence and its Matinees

    float time_till_start_button = 4.0f;    // DefaultUI.ini [TdGame.TdUIScene_Start]
    float time_till_attract_movie = 90.0f;

    std::vector<std::string> warnings;  // things that were missing but not fatal

private:
    std::shared_ptr<PackageManager> pm_;
    std::string game_root_;
    int viewport_height_ = 720;
    std::unordered_map<std::string, std::unique_ptr<Font>> fonts_;
    std::unordered_map<std::string, std::unique_ptr<Image>> images_;
    std::unordered_map<std::string, std::string> localized_;  // "file.section.key", lower case -> text
    std::unordered_map<std::string, Rect> start_rects_;
    std::unordered_map<std::string, Rect> menu_rects_;
    std::unordered_map<std::string, std::string> strings_;  // "Section.Key" -> text
};

}  // namespace me::fe
