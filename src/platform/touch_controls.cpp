#include "touch_controls.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace me {

namespace {

// Everything is measured in screen heights: x runs 0..aspect, y 0..1, so circles are round and
// the layout is the same on every phone. The right-hand buttons hang off the right edge.
constexpr float kStickRegion = 0.40f;    // fraction of the width that is the stick's
constexpr float kStickRadius = 0.12f;    // screen heights
constexpr float kStickDeadzone = 0.015f;

TouchOverlayState g_overlay;
bool g_overlay_visible = false;

}  // namespace

const TouchOverlayState* touch_overlay_for_drawing() {
    return g_overlay_visible ? &g_overlay : nullptr;
}

TouchControls::TouchControls() = default;

bool TouchControls::should_activate() {
    if (const char* env = std::getenv("ME_TOUCH")) {
        if (env[0] == '1') return true;
        if (env[0] == '0') return false;
    }
#if defined(__ANDROID__)
    return true;
#else
    // A Mac trackpad is not a finger to SDL; the desktop builds only get the controls on request.
    return false;
#endif
}

const TouchControls::ButtonShape& TouchControls::button_shape(int button) {
    // cx for the circles is the distance from the RIGHT edge (negative sign applied in widgets()).
    static const ButtonShape kShapes[kButtonCount] = {
        /* kMenu     */ {TouchWidget::Shape::Rect,   0.03f, 0.03f, 0.0f,   0.24f, 0.09f, "MENU"},
        /* kReaction */ {TouchWidget::Shape::Circle, 0.20f, 0.26f, 0.065f, 0.0f,  0.0f,  "RT"},
        /* kAttack   */ {TouchWidget::Shape::Circle, 0.20f, 0.50f, 0.075f, 0.0f,  0.0f,  "ATTACK"},
        /* kAction   */ {TouchWidget::Shape::Circle, 0.44f, 0.64f, 0.075f, 0.0f,  0.0f,  "ACTION"},
        /* kTurn180  */ {TouchWidget::Shape::Circle, 0.68f, 0.87f, 0.065f, 0.0f,  0.0f,  "180"},
        /* kJump     */ {TouchWidget::Shape::Circle, 0.20f, 0.78f, 0.11f,  0.0f,  0.0f,  "JUMP"},
        /* kCrouch   */ {TouchWidget::Shape::Circle, 0.44f, 0.87f, 0.075f, 0.0f,  0.0f,  "CROUCH"},
    };
    return kShapes[std::clamp(button, 0, kButtonCount - 1)];
}

int TouchControls::hit_button(float nx, float ny, float aspect) const {
    const float hx = nx * aspect;  // in screen heights
    const float hy = ny;
    for (int b = 0; b < kButtonCount; ++b) {
        const ButtonShape& s = button_shape(b);
        if (s.shape == TouchWidget::Shape::Rect) {
            // Rects are anchored to the left edge; a touch margin around them.
            const float m = 0.02f;
            if (hx >= s.cx - m && hx <= s.cx + s.w + m && hy >= s.cy - m && hy <= s.cy + s.h + m) return b;
        } else {
            const float cx = aspect - s.cx;
            const float dx = hx - cx;
            const float dy = hy - s.cy;
            const float r = s.r * 1.15f;  // a little slack: thumbs are not precise
            if (dx * dx + dy * dy <= r * r) return b;
        }
    }
    return -1;
}

const TouchControls::Finger* TouchControls::finger_with_role(Role role) const {
    for (const Finger& f : fingers_) {
        if (f.role == role) return &f;
    }
    return nullptr;
}

TouchControls::Finger* TouchControls::find_finger(SDL_FingerID id) {
    for (Finger& f : fingers_) {
        if (f.id == id) return &f;
    }
    return nullptr;
}

void TouchControls::handle_event(const SDL_Event& ev, int w, int h) {
    if (!active_) return;
    if (ev.type != SDL_FINGERDOWN && ev.type != SDL_FINGERUP && ev.type != SDL_FINGERMOTION) return;
    if (w > 0 && h > 0) aspect_ = static_cast<float>(w) / static_cast<float>(h);
    const SDL_TouchFingerEvent& tf = ev.tfinger;
    const float nx = std::clamp(tf.x, 0.0f, 1.0f);
    const float ny = std::clamp(tf.y, 0.0f, 1.0f);

    if (ev.type == SDL_FINGERDOWN) {
        if (find_finger(tf.fingerId)) return;  // a repeat of one already down
        Finger f;
        f.id = tf.fingerId;
        f.x0 = f.x = nx;
        f.y0 = f.y = ny;
        const int button = hit_button(nx, ny, aspect_);
        if (button >= 0) {
            f.role = Role::Button;
            f.button = button;
            held_[button] += 1;
            if (button == kMenu) menu_edge_ = true;
        } else if (nx < kStickRegion && !finger_with_role(Role::Stick)) {
            f.role = Role::Stick;
        } else if (!finger_with_role(Role::Look)) {
            f.role = Role::Look;
        } else {
            f.role = Role::None;
        }
        fingers_.push_back(f);
        return;
    }

    Finger* f = find_finger(tf.fingerId);
    if (!f) return;
    if (ev.type == SDL_FINGERMOTION) {
        if (f->role == Role::Look) {
            // dx in screen heights so a diagonal drag turns evenly; dy the same.
            const float dx = (nx - f->x) * aspect_;
            const float dy = ny - f->y;
            pending_yaw_ += dx * look_sens_;
            pending_pitch_ -= dy * look_sens_;
        }
        f->x = nx;
        f->y = ny;
        return;
    }

    // SDL_FINGERUP
    if (f->role == Role::Button && f->button >= 0 && f->button < kButtonCount) {
        held_[f->button] = std::max(0, held_[f->button] - 1);
    }
    fingers_.erase(std::remove_if(fingers_.begin(), fingers_.end(),
                                  [&](const Finger& g) { return g.id == tf.fingerId; }),
                   fingers_.end());
}

TouchInput TouchControls::consume(float /*dt*/) {
    TouchInput in;
    if (!active_) return in;
    if (const Finger* s = finger_with_role(Role::Stick)) {
        const float dx = (s->x - s->x0) * aspect_;
        const float dy = s->y - s->y0;
        const float len = std::sqrt(dx * dx + dy * dy);
        in.stick_active = true;
        if (len > kStickDeadzone) {
            // Linear from the deadzone to the rim, clamped at the rim: a full push is a full run.
            const float mag = std::clamp((len - kStickDeadzone) / (kStickRadius - kStickDeadzone), 0.0f, 1.0f);
            in.strafe = dx / len * mag;
            in.forward = -dy / len * mag;
        }
    }
    in.look_yaw_delta_deg = pending_yaw_;
    in.look_pitch_delta_deg = pending_pitch_;
    pending_yaw_ = 0.0f;
    pending_pitch_ = 0.0f;
    in.jump = held_[kJump] > 0;
    in.crouch = held_[kCrouch] > 0;
    in.action = held_[kAction] > 0;
    in.attack = held_[kAttack] > 0;
    in.turn_180 = held_[kTurn180] > 0;
    in.reaction = held_[kReaction] > 0;
    in.menu_pressed_edge = menu_edge_;
    menu_edge_ = false;
    return in;
}

std::vector<TouchWidget> TouchControls::widgets(int w, int h) const {
    std::vector<TouchWidget> out;
    if (!active_ || w <= 0 || h <= 0) return out;
    const float fw = static_cast<float>(w);
    const float fh = static_cast<float>(h);
    const float aspect = fw / fh;

    // The stick: at the finger while one is down, else resting in the lower left.
    {
        TouchWidget stick;
        stick.shape = TouchWidget::Shape::Circle;
        stick.is_stick = true;
        stick.r = kStickRadius * fh;
        stick.label = "MOVE";
        if (const Finger* s = finger_with_role(Role::Stick)) {
            stick.pressed = true;
            stick.x = s->x0 * fw;
            stick.y = s->y0 * fh;
            float dx = (s->x - s->x0) * aspect;
            float dy = s->y - s->y0;
            const float len = std::sqrt(dx * dx + dy * dy);
            if (len > kStickRadius) {
                dx *= kStickRadius / len;
                dy *= kStickRadius / len;
            }
            stick.knob_x = stick.x + dx * fh;
            stick.knob_y = stick.y + dy * fh;
        } else {
            stick.x = 0.20f * fh;
            stick.y = fh - 0.26f * fh;
            stick.knob_x = stick.x;
            stick.knob_y = stick.y;
        }
        out.push_back(stick);
    }

    for (int b = 0; b < kButtonCount; ++b) {
        const ButtonShape& s = button_shape(b);
        TouchWidget wdg;
        wdg.shape = s.shape;
        wdg.label = s.label;
        wdg.pressed = held_[b] > 0;
        if (s.shape == TouchWidget::Shape::Rect) {
            wdg.x = s.cx * fh;
            wdg.y = s.cy * fh;
            wdg.w = s.w * fh;
            wdg.h = s.h * fh;
        } else {
            wdg.x = fw - s.cx * fh;
            wdg.y = s.cy * fh;
            wdg.r = s.r * fh;
        }
        out.push_back(wdg);
    }
    return out;
}

void TouchControls::publish_overlay(int w, int h, bool visible) const {
    if (!active_ || !visible || w <= 0 || h <= 0) {
        g_overlay_visible = false;
        return;
    }
    g_overlay.width = w;
    g_overlay.height = h;
    g_overlay.widgets = widgets(w, h);
    g_overlay_visible = true;
}

void TouchControls::reset() {
    fingers_.clear();
    pending_yaw_ = pending_pitch_ = 0.0f;
    menu_edge_ = false;
    std::fill(std::begin(held_), std::end(held_), 0);
}

}  // namespace me
