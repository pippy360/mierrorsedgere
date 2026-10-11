#pragma once

// Touch controls (Android, or ME_TOUCH=1 anywhere SDL reports fingers): a virtual move stick on
// the left of the screen, look by dragging on the right, and on-screen buttons. SDL finger events
// in, an InputFrame-shaped TouchInput out once a frame; the widgets' shapes for the HUD to draw
// through touch_overlay_for_drawing() (touch_overlay.hpp).
//
// Layout, in landscape: the left 40% of the screen is the stick (the finger's first touch is its
// centre, radius 12% of the screen height), the rest is look, except the buttons:
//   MENU (top left, Escape), RT (reaction time, X), ATTACK (F), ACTION (E), 180 (Q),
//   JUMP (large, bottom right, Space), CROUCH (C).
// A button is held while a finger is on it; every finger is tracked by its id.

#include "touch_overlay.hpp"

#include <SDL2/SDL.h>

#include <vector>

namespace me {

struct TouchInput {
    float forward = 0.0f;              // -1..1 from the stick
    float strafe = 0.0f;
    float look_yaw_delta_deg = 0.0f;   // this frame's look, degrees
    float look_pitch_delta_deg = 0.0f;
    bool stick_active = false;         // a finger is on the stick: forward/strafe are meant
    bool jump = false;
    bool crouch = false;
    bool action = false;               // use / disarm (E)
    bool attack = false;               // fire / melee (F)
    bool turn_180 = false;             // Q
    bool reaction = false;             // reaction time (X), held
    bool menu_pressed_edge = false;    // MENU went down this frame (Escape)
};

class TouchControls {
public:
    TouchControls();

    // Whether events are taken and widgets drawn. Decided once at start-up: on when a touch device
    // is present (SDL_GetNumTouchDevices) or ME_TOUCH=1; always on Android.
    void set_active(bool on) { active_ = on; }
    [[nodiscard]] bool active() const { return active_; }

    // Decides from the environment and SDL's touch devices (see set_active).
    static bool should_activate();

    // Feeds a finger event; w, h are the drawable's size (for the aspect ratio: SDL's finger
    // coordinates are normalised). Other events are ignored.
    void handle_event(const SDL_Event& ev, int w, int h);

    // This frame's input; the look deltas and the MENU edge are consumed, the rest reflects the
    // fingers that are down.
    TouchInput consume(float dt);

    // Degrees the view turns for a drag of one screen height (default 150).
    void set_look_sensitivity(float deg_per_screen_height) { look_sens_ = deg_per_screen_height; }
    [[nodiscard]] float look_sensitivity() const { return look_sens_; }

    // The widgets laid out for a w x h drawable, in pixels.
    [[nodiscard]] std::vector<TouchWidget> widgets(int w, int h) const;

    // Publishes widgets(w, h) for the renderer (touch_overlay_for_drawing()); clears it when not
    // active or `visible` is false (a menu is up).
    void publish_overlay(int w, int h, bool visible) const;

    // Drops every finger (the app went to the background).
    void reset();

    enum Button { kMenu = 0, kReaction, kAttack, kAction, kTurn180, kJump, kCrouch, kButtonCount };

private:
    enum class Role { None, Stick, Look, Button };
    struct Finger {
        SDL_FingerID id = 0;
        Role role = Role::None;
        int button = -1;
        float x0 = 0.0f, y0 = 0.0f;   // where it went down (the stick's centre), normalised
        float x = 0.0f, y = 0.0f;     // where it is now
    };
    struct ButtonShape {
        TouchWidget::Shape shape;
        float cx, cy;      // circle: centre, in screen fractions of x / y; rect: top-left
        float r;           // circle radius, as a fraction of the screen height
        float w, h;        // rect size, as fractions of the screen height
        const char* label;
    };

    [[nodiscard]] int hit_button(float nx, float ny, float aspect) const;
    [[nodiscard]] const Finger* finger_with_role(Role role) const;
    [[nodiscard]] Finger* find_finger(SDL_FingerID id);
    [[nodiscard]] static const ButtonShape& button_shape(int button);

    bool active_ = false;
    float look_sens_ = 150.0f;
    float aspect_ = 16.0f / 9.0f;
    std::vector<Finger> fingers_;
    float pending_yaw_ = 0.0f;
    float pending_pitch_ = 0.0f;
    bool menu_edge_ = false;
    int held_[kButtonCount] = {};
};

}  // namespace me
