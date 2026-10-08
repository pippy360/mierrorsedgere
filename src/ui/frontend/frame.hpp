#pragma once

// -----------------------------------------------------------------------------
// One frame of the front end as a renderer receives it: where the menu level's camera
// is, and the list of 2D things to draw on top. soft_render.hpp is the reference renderer.
// -----------------------------------------------------------------------------

#include "frontend_assets.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

namespace me::fe {

// The display gamma the game encodes the frame with, measured on retail frames. The start screen
// is drawn with the DisplayGamma of TdEngine.ini, 2.2. Taking a key there loads the profile, and
// TdPlayerController.SetVideoProfileSettings then calls SetGamma(Brightness / 10): from the main
// menu on, a default profile gives 2.73.
constexpr float kStartGamma = 2.2f;
constexpr float kProfileGamma = 2.73f;

// What the canvas does to a linear colour on its way to the back buffer: pow(c, 1 / gamma), with
// a floor under it. "Black" canvas pixels are (2, 2, 2) on the start screen and (9, 9, 9) on the
// main menu; the floor is taken between those two measurements. Materials drawn in the UI (the
// columns) do not go through this.
inline float canvas_encode(float linear, float gamma) {
    const float c = linear < 0.0f ? 0.0f : (linear > 1.0f ? 1.0f : linear);
    const float t = (gamma - kStartGamma) / (kProfileGamma - kStartGamma);
    const float floor_out = (2.0f + (9.0f - 2.0f) * (t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t))) / 255.0f;
    const float out = std::pow(c, 1.0f / gamma);
    return out < floor_out ? floor_out : out;
}

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
    bool clipped = false;  // only the part of the quads inside `clip` (viewport pixels) is drawn
    Rect clip;
};

// Everything a renderer needs for one frame.
struct Frame {
    int width = 1280;
    int height = 720;
    double time = 0.0;  // seconds the front end has run: the stick material's Time
    Vec3 camera{0.0f, 0.0f, 0.0f};
    Vec3 target{0.0f, 1.0f, 0.0f};
    float roll = 0.0f;   // degrees about the view direction (FRotator.Roll)
    float fov = 90.0f;   // horizontal, degrees (CameraActor.FOVAngle)
    float white = 0.0f;  // SeqAct_TdFadeEffect: 0 = clear, 1 = white
    float display_gamma = kStartGamma;  // what the scene and the canvas are encoded with this frame
    std::vector<float> district_selected;  // per City::districts: the "Selected" parameter, 0..1
    std::vector<DrawOp> ui;
};

}  // namespace me::fe
