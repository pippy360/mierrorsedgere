#pragma once

// -----------------------------------------------------------------------------
// The screen fade: TdHUD's FadeAmount, which the post-process chain's FadeInEffect
// draws (renderer/builtin_shaders_msl.hpp, tonemap_fragment).
//
// TdHUD (TdGame.u) keeps FadeAmount between 0, all FadeColor, and 1, the picture,
// and moves it over FadeInTime or FadeOutTime, whichever is set:
//   PlayerOwnerRestart        white, FadeAmount forced to 0, in over 1 s
//   TriggerCustomColorFadeIn  the colour; if FadeAmount < 1, in over max(Time, 1/60)
//   TriggerCustomColorFadeOut the colour; if FadeAmount > 0, out over max(Time, 1/60)
// SeqAct_TdFadeEffect, the Kismet action levels fade with, calls the last two with
// each channel of its FadeColor divided by 255 as integers: 1 at 255, else 0.
// The step itself is native; it is taken here as a straight line over the time.
// -----------------------------------------------------------------------------

#include "../math/types.hpp"

#include <algorithm>

namespace me {

struct ScreenFade {
    float amount = 1.0f;  // FadeInAmount: 1 = the picture
    Vec3 color{1.0f, 1.0f, 1.0f};
    float in_time = 0.0f;
    float out_time = 0.0f;

    // The player (re)starts: the picture comes in from white.
    void restart() {
        amount = 0.0f;
        color = Vec3(1.0f, 1.0f, 1.0f);
        in_time = 1.0f;
        out_time = 0.0f;
    }
    void fade_in(float time, const Vec3& to) {
        color = to;
        if (amount < 1.0f) {
            in_time = std::max(time, 0.0166f);
            out_time = 0.0f;
        }
    }
    void fade_out(float time, const Vec3& to) {
        color = to;
        if (amount > 0.0f) {
            out_time = std::max(time, 0.0166f);
            in_time = 0.0f;
        }
    }
    void apply(const IntroFadeEvent& e) {
        if (e.fade_out) {
            fade_out(e.duration, e.color);
        } else {
            fade_in(e.duration, e.color);
        }
    }
    void update(float dt) {
        if (in_time > 0.0f) {
            amount = std::min(1.0f, amount + dt / in_time);
            if (amount >= 1.0f) in_time = 0.0f;
        } else if (out_time > 0.0f) {
            amount = std::max(0.0f, amount - dt / out_time);
            if (amount <= 0.0f) out_time = 0.0f;
        }
    }
};

}  // namespace me
