#pragma once

// What the renderers draw for the touch controls (src/platform/touch_controls.hpp): the widgets'
// shapes in drawable pixels, published by the game loop once a frame. Declared apart from the
// controls themselves so the backends' shared overlay code (renderer/overlay_ui.inl) needs only
// this; the renderer files pull it in through platform/platform.hpp.

#include <string>
#include <vector>

namespace me {

struct TouchWidget {
    enum class Shape { Circle, Rect };
    Shape shape = Shape::Circle;
    // Circle: centre (x, y) and radius r. Rect: top-left (x, y), size (w, h).
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f, r = 0.0f;
    std::string label;
    bool pressed = false;
    // The move stick: drawn as its base circle plus a knob at (knob_x, knob_y).
    bool is_stick = false;
    float knob_x = 0.0f, knob_y = 0.0f;
};

struct TouchOverlayState {
    int width = 0;      // the drawable the widgets were laid out for
    int height = 0;
    std::vector<TouchWidget> widgets;
};

// The widgets to draw this frame, or null when the touch controls are inactive.
const TouchOverlayState* touch_overlay_for_drawing();

}  // namespace me
