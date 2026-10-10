#pragma once

// -----------------------------------------------------------------------------
// The post-process chain's material effects, as the game's TdHudEffectManager
// drives them (docs/RENDERING_RE.md, section 8): which are shown this frame and
// with what material parameters. The renderers draw them (LevelScene::post_effects,
// PlayerTelemetry::screen_effects).
//
//   UncontrolledFallingEffect  FXFScreen_UncontrolledFalling: on while the pawn falls
//       faster than 2000 uu/s; from 0, +0.5 a second, held at 0.99; off at once.
//   HealthEffect               Health = Health / 100, shown below 100 (0 while dead).
//   ReactionTimeEffect         FXFScreen_ReactionTime = 1 - time dilation (0.75 at the
//       full slow motion; the dilation takes 0.8 s to get there and 1.6 s back), and
//       FXFScreen_ReactionTimeCharged, a 0.5 / 0.5 / 0.5 s pulse when the energy fills.
//   MeleeEffect                FXFScreen_MeleeDamage, damage / 100 through a
//       0.06 / 0.06 / 0.4 s envelope; FXFScreen_MeleeHitDirection, the hit's angle in turns.
//   FallDamageEffect           FXFScreen_FallDamage, damage / 100 through 0.03 / 0.12 / 0.75 s
//       (a hard landing is 15).
//   DeathEffect                DeathAmount 0.25 at death, +0.25 a second to 1.
//
// An envelope (TdHudEffect::ActivatePP / UpdatePP): a hit aims at min(1, current +
// strength); the value eases there (3a^2 - 2a^3), holds, and eases out to 0; the
// effect is shown while the value is above 0.
// -----------------------------------------------------------------------------

#include "../math/types.hpp"

#include <algorithm>
#include <cstdint>

namespace me {

class ScreenEffects {
public:
    // TdHUD.Reset at a restart: the envelopes back to nothing. The falling effect is left to the new
    // pawn's velocity, as in the game.
    void restart() {
        melee_ = Envelope{0.06f, 0.06f, 0.4f};
        fall_ = Envelope{0.03f, 0.12f, 0.75f};
        charged_ = Envelope{0.5f, 0.5f, 0.5f};
        death_ = 0.0f;
        reaction_ = 0.0f;
    }

    // One HUD tick. `dt` is game time.
    void update(const PlayerTelemetry& tel, float dt, std::vector<ScreenEffect>& out) {
        out.clear();
        dt = std::max(dt, 0.0f);

        // Uncontrolled falling: a test on the velocity and nothing else.
        if (tel.velocity.z < -2000.0f) {
            falling_ = falling_shown_ ? std::clamp(falling_ + dt * 0.5f, 0.0f, 0.99f) : 0.0f;
            falling_shown_ = true;
            out.push_back(effect("UncontrolledFallingEffect", "FXFScreen_UncontrolledFalling", falling_));
        } else {
            falling_shown_ = false;
            falling_ = 0.0f;
        }

        // Health (M_FX_FullScreenFX_HealthEffect_01 + PPHealthSaturationSettings impulse on hit).
        const bool dead = tel.health <= 0.0f;
        if (tel.health < 100.0f || tel.health_desat > 0.001f) {
            const float h_val = dead ? 0.0f : std::clamp(std::min(tel.health * 0.01f, 1.0f - tel.health_desat * 0.65f), 0.0f, 1.0f);
            ScreenEffect fx = effect("HealthEffect", "Health", h_val);
            fx.params.emplace_back("FXFScreen_HealthDesaturate", std::array<float, 4>{tel.health_desat, 0.0f, 0.0f, 0.0f});
            out.push_back(std::move(fx));
        }

        // Death.
        if (dead) {
            death_ = (death_ <= 0.0f) ? 0.25f : std::min(1.0f, death_ + 0.25f * dt);
            out.push_back(effect("DeathEffect", "DeathAmount", death_));
        } else {
            death_ = 0.0f;
        }

        // Reaction time: the picture follows the time dilation, which script ramps.
        const float target = tel.reaction_active ? 0.75f : 0.0f;
        const float pace = (target > reaction_) ? 0.75f / 0.8f : 0.75f / 1.6f;
        reaction_ += std::clamp(target - reaction_, -pace * dt, pace * dt);
        if (tel.reaction_energy >= 100.0f && energy_before_ < 100.0f) charged_.hit(1.0f);
        energy_before_ = tel.reaction_energy;
        const float charged = tel.reaction_active ? charged_.current : charged_.step(dt);  // the pulse waits during a use
        if (reaction_ > 0.0f || charged > 0.0f) {
            ScreenEffect fx = effect("ReactionTimeEffect", "FXFScreen_ReactionTime", reaction_);
            fx.params.emplace_back("FXFScreen_ReactionTimeCharged", std::array<float, 4>{charged, 0.0f, 0.0f, 0.0f});
            out.push_back(std::move(fx));
        }

        // Hits.
        if (tel.melee_hit_count != melee_seen_) {
            melee_seen_ = tel.melee_hit_count;
            melee_direction_ = tel.melee_hit_turns;
            melee_.hit(tel.melee_hit_damage * 0.01f);
        }
        if (tel.fall_hit_count != fall_seen_) {
            fall_seen_ = tel.fall_hit_count;
            if (tel.fall_hit_damage < 100.0f) fall_.hit(tel.fall_hit_damage * 0.01f);
        }
        if (const float v = melee_.step(dt); v > 0.0f) {
            ScreenEffect fx = effect("MeleeEffect", "FXFScreen_MeleeDamage", v);
            fx.params.emplace_back("FXFScreen_MeleeHitDirection", std::array<float, 4>{melee_direction_, 0.0f, 0.0f, 0.0f});
            out.push_back(std::move(fx));
        }
        if (const float v = fall_.step(dt); v > 0.0f) out.push_back(effect("FallDamageEffect", "FXFScreen_FallDamage", v));
    }

private:
    struct Envelope {
        float in = 0.1f, hold = 0.5f, out = 1.5f;  // PPSettings: FadeInDuration, Duration, FadeOutDuration
        float in_timer = 0.0f, in_start = 0.0f, hold_timer = 0.0f, out_timer = 0.0f, target = 0.0f, current = 0.0f;
        bool active = false;

        void hit(float strength) {
            in_timer = in;
            in_start = current;
            out_timer = out;
            hold_timer = hold;
            active = true;
            target = std::min(1.0f, current + strength);
        }
        // The timers count down, each phase tested before it is stepped.
        float step(float dt) {
            if (!active) return 0.0f;
            const auto ease = [](float a) { return a * a * (3.0f - 2.0f * a); };
            float value = 0.0f;
            if (in_timer > 0.0f && in > 0.0f) {
                value = std::max(0.001f, target + (in_start - target) * ease(in_timer / in));
                in_timer -= dt;
            } else if (hold_timer > 0.0f && hold > 0.0f) {
                value = target;
                hold_timer -= dt;
            } else if (out_timer > 0.0f && out > 0.0f) {
                value = target * ease(out_timer / out);
                out_timer -= dt;
            } else {
                target = 0.0f;
            }
            current = std::clamp(value, 0.0f, 1.0f);
            if (!(current > 0.0f)) active = false;
            return current;
        }
    };

    static ScreenEffect effect(const char* name, const char* parameter, float value) {
        ScreenEffect fx;
        fx.name = name;
        fx.params.emplace_back(parameter, std::array<float, 4>{value, 0.0f, 0.0f, 0.0f});
        return fx;
    }

    bool falling_shown_ = false;
    float falling_ = 0.0f;
    float death_ = 0.0f;
    float reaction_ = 0.0f;
    float energy_before_ = 100.0f;
    Envelope melee_{0.06f, 0.06f, 0.4f};
    Envelope fall_{0.03f, 0.12f, 0.75f};
    Envelope charged_{0.5f, 0.5f, 0.5f};
    float melee_direction_ = 0.0f;
    uint32_t melee_seen_ = 0;
    uint32_t fall_seen_ = 0;
};

}  // namespace me
