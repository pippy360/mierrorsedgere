#pragma once

// -----------------------------------------------------------------------------
// A bot's fire at the player, a bot's armour, the stumble a bullet puts him in, and the helicopter's
// box: the rules and the tables, with nothing of the level in them (ParkourController traces).
//
// A bot does not roll a die for each shot. He keeps an aim offset, in uu at her place, that shrinks
// while he sees her:
//
//   UTdAimBot::UpdateDispersion (MirrorsEdge.exe 0x012537f0, every tick)
//       BaseOffset, BaseDispersion and BaseYOffset go up by the one-shot penalty of the move she has
//       just changed to (ATdPawn vtbl[0x458]), down by GetImprovementRate() x DeltaTime while
//       EnemyVisible and the bot is not suppressed, and are clamped: BaseOffset to
//       [HorizontalOffset_Min, HorizontalOffset_Max of the range band he is in].
//   UTdAimBot::GetImprovementRate (0x01252610)
//       the template's rate for the band and the difficulty, x MoveTowardMultiplier while she moves
//       at 200 uu/s or less; faster, by the cosine between her velocity and the way to the bot:
//       under 0.7 either way MoveSidewaysMultiplier, else MoveAway- or MoveTowardMultiplier; and,
//       when the result is above nothing, x the AiAimPenalty of the move she is in (vtbl[0x454]).
//   ATdBotPawn::ClassifyAIFiringRange (0x01236340)
//       far beyond the weapon's CombatRange_Max, near under 489.9 uu (240000 squared), else medium.
//   TdAIController.GetAdjustedAimFor -> TdAimBot.GetAimLocation
//       where a shot is aimed: StartBurst on the first shot (the three values thrown back to their
//       worst, a fresh PickPoint), GetNextLocation on every later one: the point sweeps across her
//       from one side to the other and climbs to her eyes. bFirstShot is set again whenever the bot
//       has no line of fire (0x011ea200): out of his sight for longer than KeepFiringTime she gets
//       his worst aim back.
//   Weapon.AddSpread, Weapon.InstantFire
//       the weapon's own Spread on top, and the line from his trigger finger through the point.
//   TdPlayerPawn.TakeDamage -> TdAimBot.IsHitRelevant
//       a bullet that lands on her counts only while BaseOffset is at or under the template's
//       OffsetThreshold for the band (a pistol's, a rifle's and a shotgun's bullet; a sniper's and
//       a taser's always count). One that lands sooner does nothing and is not heard.
//   TdWeapon.GetInstantHitDamage, TdPawn.AdjustDamage
//       the weapon's damage with its fall-off, x AIDamageMultiplier, then x (1 - her armour): 0.7,
//       0.5, 0.3 by difficulty on every part of her.
//
// No move makes her immune. A move slows the bot's aim (a wall run to 0.4, 0.5, 0.7 of its rate by
// difficulty) or throws it back at once (a vault by 50 uu, a swing or a zip line by 200).
//
// The numbers are DefaultAITemplates.ini's, DefaultWeapons.ini's and DefaultGame.ini's, the weapon
// classes' own defaults (TdSharedContent.u) and the moves' (TdGame.u, Default__TdMove_*).
// -----------------------------------------------------------------------------

#include "../math/types.hpp"
#include "body_shapes.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

namespace me {

// TdPlayerController.GetDifficultyLevel: the profile's GameDifficulty, 0 Easy, 1 Medium, 2 Hard;
// 1 with no profile, and always 1 in a race (TdSPLevelRace).
inline constexpr int kDifficultyEasy = 0, kDifficultyMedium = 1, kDifficultyHard = 2;
inline int clamp_difficulty(int level) { return std::clamp(level, kDifficultyEasy, kDifficultyHard); }

// A value by difficulty (AIAimingModifierSettings, FloatDifficultySettings: Easy, Medium, Hard).
struct ByDifficulty {
    float easy = 0.0f, medium = 0.0f, hard = 0.0f;
    [[nodiscard]] constexpr float at(int difficulty) const { return difficulty <= kDifficultyEasy ? easy : difficulty >= kDifficultyHard ? hard : medium; }
};

// AIFiringRange, as ATdBotPawn::GetFiringRange's users read it: 0 near, 1 medium, 2 far.
inline constexpr uint8_t kFiringRangeNear = 0, kFiringRangeMedium = 1, kFiringRangeFar = 2;

// A value by range band (HorizontalOffset_Max, OffsetThreshold: Near, Medium, Far).
struct ByRange {
    float near_band = 0.0f, medium = 0.0f, far_band = 0.0f;
    [[nodiscard]] constexpr float at(uint8_t range) const { return range == kFiringRangeNear ? near_band : range == kFiringRangeFar ? far_band : medium; }
};

// ATdBotPawn::ClassifyAIFiringRange (0x01236340). CombatRange_Min is read by nothing.
inline uint8_t classify_firing_range(float distance_sq, float combat_range_max) {
    if (distance_sq > combat_range_max * combat_range_max) return kFiringRangeFar;
    if (distance_sq < 240000.0f) return kFiringRangeNear;
    return kFiringRangeMedium;
}

// -----------------------------------------------------------------------------
// The weapons, as a bot fires them
// -----------------------------------------------------------------------------

// InstantHitDamageTypes[0]. TdAimBot.IsBulletDamage is true of the first three alone.
enum class EBulletKind : uint8_t { LowCaliber = 0, HighCaliber, Shotgun, Sniper, Taser };

// AIBurstInfo: how many shots a burst is and how long the pause after it.
struct BurstPattern {
    int32_t length_min = 1, length_max = 1;
    float pause_min = 0.5f, pause_max = 1.0f;
};

struct BotWeapon {
    const char* name;            // the port's name for it (WeaponState::name, EnemyBot::weapon_name)
    EBulletKind kind;            // InstantHitDamageTypes[0]
    float damage;                // InstantHitDamage[0]
    float falloff_distance;      // FallOffDistance
    float range;                 // WeaponRange: the trace's length, and no damage from there on
    uint8_t falloff;             // WeaponFallOffTypes[0]: 0 EWFOT_Linear, 1 Logarithmic, 2 Inverse, 3 LogReverse
    float spread;                // Spread[0]
    float fire_interval;         // FireInterval[0]
    float ai_damage_multiplier;  // AIDamageMultiplier
    int32_t pellets;             // PelletCount (the shotguns' CustomFire)
    float combat_range_max;      // CombatRange_Max
    BurstPattern bursts[3];      // AimedBurst_Near, _Mid, _Far
    uint8_t tracer_ammo;         // BulletTracer::ammo: TdWeapon_Light 0, TdWeapon_Heavy 1, a shotgun's pellet 3
};

// The class defaults (Default__TdWeapon_* of TdSharedContent.u; a value a class does not write is
// TdWeapon's: Spread 0.03, EWFOT_Linear, TdDmgType_LowCaliber_Bullet) and DefaultWeapons.ini's
// sections. The ini's Damages, FallOffDistances and Ranges by difficulty are all 0: every difficulty
// has the class's own (TdWeapon.SetDifficultyLevel).
inline constexpr BotWeapon kBotWeapons[] = {
    {"Colt1911", EBulletKind::LowCaliber, 42.0f, 600.0f, 4500.0f, 0, 0.03f, 0.2f, 1.0f, 1, 2000.0f,
     {{1, 1, 0.3f, 0.5f}, {1, 1, 0.5f, 1.0f}, {1, 1, 1.0f, 1.5f}}, 0},
    {"Glock18", EBulletKind::LowCaliber, 17.0f, 600.0f, 4500.0f, 0, 0.03f, 0.1f, 1.0f, 1, 2500.0f,
     {{3, 3, 0.5f, 1.0f}, {2, 3, 1.0f, 1.2f}, {1, 2, 1.0f, 1.5f}}, 0},
    {"BerettaM93R", EBulletKind::LowCaliber, 19.0f, 600.0f, 4500.0f, 0, 0.03f, 0.1f, 1.0f, 1, 2500.0f,
     {{6, 6, 0.5f, 1.0f}, {3, 5, 1.0f, 1.2f}, {2, 4, 1.0f, 1.5f}}, 0},
    {"SteyrTMP", EBulletKind::LowCaliber, 15.5f, 800.0f, 3000.0f, 0, 0.008f, 0.06f, 1.0f, 1, 3000.0f,
     {{6, 6, 0.5f, 1.0f}, {3, 5, 0.5f, 1.0f}, {2, 5, 1.0f, 2.0f}}, 0},
    {"MP5K", EBulletKind::HighCaliber, 25.0f, 2000.0f, 8000.0f, 2, 0.04f, 0.1f, 1.0f, 1, 4000.0f,
     {{6, 6, 0.3f, 0.5f}, {3, 6, 0.8f, 1.2f}, {2, 5, 1.0f, 2.0f}}, 1},
    {"G36C", EBulletKind::HighCaliber, 30.0f, 2000.0f, 9000.0f, 2, 0.03f, 0.085f, 1.0f, 1, 5000.0f,
     {{6, 9, 0.3f, 0.8f}, {3, 6, 0.5f, 1.0f}, {2, 4, 1.0f, 2.0f}}, 1},
    {"FNSCARL", EBulletKind::HighCaliber, 35.0f, 2000.0f, 9000.0f, 2, 0.02f, 0.11f, 1.0f, 1, 5000.0f,
     {{6, 9, 0.3f, 0.8f}, {3, 6, 0.5f, 1.0f}, {2, 4, 1.0f, 2.0f}}, 1},
    {"Remington870", EBulletKind::Shotgun, 16.0f, 1500.0f, 6000.0f, 0, 0.12f, 1.2f, 1.0f, 10, 2000.0f,
     {{1, 1, 0.8f, 1.2f}, {1, 1, 1.0f, 1.5f}, {1, 1, 1.5f, 2.5f}}, 3},
    {"Neostead", EBulletKind::Shotgun, 16.0f, 1500.0f, 6000.0f, 0, 0.1f, 1.1f, 1.0f, 10, 4000.0f,
     {{1, 1, 0.6f, 0.8f}, {1, 1, 0.8f, 1.2f}, {1, 1, 1.5f, 2.0f}}, 3},
    {"FNMinimi", EBulletKind::HighCaliber, 36.0f, 6000.0f, 20000.0f, 2, 0.04f, 0.12f, 1.0f, 1, 15000.0f,
     {{15, 25, 1.5f, 2.0f}, {15, 25, 1.5f, 2.0f}, {20, 30, 1.5f, 2.0f}}, 1},
    // (Its FallOffDistance is past its WeaponRange: a sniper's bullet does not fall off.)
    {"M95", EBulletKind::Sniper, 100.0f, 180000.0f, 20000.0f, 3, 0.001f, 1.7f, 0.8f, 1, 20000.0f,
     {{1, 1, 2.5f, 2.5f}, {1, 1, 2.5f, 2.5f}, {1, 1, 2.5f, 2.5f}}, 1},
};

// TdWeapon.MinKeepFiringTime, MaxKeepFiringTime: how long a bot goes on firing at where he last saw
// her (TdAIController.EnemyNotVisible draws a time between them). No weapon's section changes them.
inline constexpr float kMinKeepFiringTime = 0.2f, kMaxKeepFiringTime = 0.8f;

// The weapon a bot or the player holds, by the port's name for it; null for none, and for one that
// fires no bullet here (the pursuit cop's taser).
inline const BotWeapon* find_bot_weapon(std::string_view name) {
    const auto has = [&](std::string_view part) { return name.find(part) != std::string_view::npos; };
    const auto row = [](size_t i) { return &kBotWeapons[i]; };
    if (has("Glock")) return row(1);
    if (has("Beretta") || has("93R")) return row(2);
    if (has("TMP") || has("Steyr")) return row(3);
    if (has("MP5")) return row(4);
    if (has("G36")) return row(5);
    if (has("SCAR")) return row(6);
    if (has("Remington") || has("870")) return row(7);
    if (has("Neostead")) return row(8);
    if (has("Minimi") || has("M249")) return row(9);
    if (has("Barret") || has("M95")) return row(10);
    if (has("Colt") || has("1911")) return row(0);
    return nullptr;
}

// TdWeapon.GetInstantHitDamage(Mode, Distance) in single player: the class's damage, between
// FallOffDistance and WeaponRange lessened by the weapon's kind of fall-off, nothing from
// WeaponRange on, and x AIDamageMultiplier when the one who fires is not the player.
inline float weapon_damage_at(const BotWeapon& w, float distance, bool fired_by_ai) {
    float damage = w.damage;
    if (distance > w.falloff_distance && distance < w.range) {
        // FPctByRange(Distance, FallOffDistance, WeaponRange)
        const float t = (distance - w.falloff_distance) / (w.range - w.falloff_distance);
        switch (w.falloff) {
            case 0: damage *= 1.0f - t; break;
            case 1: damage *= std::min(-0.2f * std::log(t), 1.0f); break;
            case 2: damage /= 3.0f * t + 1.0f; break;
            case 3: damage *= 1.0f - std::min(-0.2f * std::log(1.0f - t), 1.0f); break;
            default: break;
        }
    } else if (distance >= w.range) {
        damage = 0.0f;
    }
    if (fired_by_ai) damage *= w.ai_damage_multiplier;
    return damage;
}

// TdPawn.AdjustDamage on TdPlayerPawn (DefaultGame.ini: ArmorBulletsHeadSettings, ...BodySettings
// and ...LegsSettings are the same three numbers): what is left of a bullet's damage on her.
inline constexpr ByDifficulty kPlayerBulletArmour{0.7f, 0.5f, 0.3f};

// TdPawn.AdjustDamage: Max(0, int(float(Damage) * (1 - ArmorValue))).
inline int armoured_damage(int damage, float armour) { return std::max(0, static_cast<int>(static_cast<float>(damage) * (1.0f - armour))); }

// What a bot's bullet that lands and counts takes from her: TdWeapon.ProcessInstantHit hands
// TakeDamage int(GetInstantHitDamage), and her armour comes off that.
inline int player_bullet_damage(const BotWeapon& w, float distance, int difficulty) {
    return armoured_damage(static_cast<int>(weapon_damage_at(w, distance, true)), kPlayerBulletArmour.at(difficulty));
}

// -----------------------------------------------------------------------------
// The AI templates: what each one's bot is (his pawn class, for the armour; his weapon) and how he aims
// -----------------------------------------------------------------------------

// The pawn class a template names (DefaultAITemplates.ini, PawnClass). The armour is the class's.
enum class EBotPawnClass : uint8_t {
    Assault = 0,         // TdSpContent.TdBotPawn_Assault
    PatrolCop,           // TdBotPawn_PatrolCop
    PatrolCopSteyr,      // TdBotPawn_PatrolCop_Steyr
    PatrolCopRemington,  // TdBotPawn_PatrolCop_Remington
    Support,             // TdBotPawn_Support
    RiotCop,             // TdBotPawn_RiotCop
    PursuitCop,          // TdBotPawn_PursuitCop
    SniperCop,           // TdBotPawn_SniperCop
    Celeste,             // TdSpBossContent.TdBotPawn_Celeste
    SniperCeleste,       // TdSpBossContent.TdBotPawn_SniperCeleste
    Gunner,              // TdBotPawn_SupportHelicopterGunner
    JKSniper,            // TdBotPawn_JKSniper
    Tutorial,            // TdGame.TdBotPawn_Tutorial
    Dummy,               // TdBotPawn_Dummy
    Count
};

// ArmorBullets{Head,Body,Legs}Settings of each pawn class (TdSpContent.u, TdSpBossContent.u; a
// class that writes none has none). In a chase the _CHASE sets are read instead, which differ for
// Celeste alone (PursuitCop's) and SniperCeleste (none); nothing here is ever in a chase.
struct BotArmour {
    ByDifficulty head, body, legs;
};
inline constexpr BotArmour kBotArmour[static_cast<size_t>(EBotPawnClass::Count)] = {
    {{0.2f, 0.3f, 0.5f}, {0.2f, 0.3f, 0.5f}, {0.2f, 0.3f, 0.5f}},  // Assault
    {{0.0f, 0.0f, 0.2f}, {0.0f, 0.0f, 0.2f}, {0.0f, 0.0f, 0.2f}},  // PatrolCop
    {{0.0f, 0.1f, 0.3f}, {0.0f, 0.1f, 0.3f}, {0.0f, 0.1f, 0.3f}},  // PatrolCop_Steyr
    {{0.0f, 0.1f, 0.3f}, {0.0f, 0.1f, 0.3f}, {0.0f, 0.1f, 0.3f}},  // PatrolCop_Remington
    {{0.4f, 0.5f, 0.7f}, {0.4f, 0.5f, 0.7f}, {0.4f, 0.5f, 0.7f}},  // Support
    {{0.5f, 0.6f, 0.8f}, {0.5f, 0.6f, 0.8f}, {0.5f, 0.6f, 0.8f}},  // RiotCop
    {{0.1f, 0.2f, 0.4f}, {0.1f, 0.2f, 0.4f}, {0.1f, 0.2f, 0.4f}},  // PursuitCop
    {{0.1f, 0.2f, 0.4f}, {0.1f, 0.2f, 0.4f}, {0.1f, 0.2f, 0.4f}},  // SniperCop
    {{0.2f, 0.2f, 0.2f}, {0.6f, 0.6f, 0.6f}, {0.5f, 0.5f, 0.5f}},  // Celeste
    {{0.2f, 0.2f, 0.2f}, {0.6f, 0.6f, 0.6f}, {0.5f, 0.5f, 0.5f}},  // SniperCeleste
    {},                                                            // the door gunner
    {},                                                            // Jacknife in the helicopter
    {},                                                            // the Training Area's partner
    {},                                                            // the dummies
};

struct AimTemplate {
    const char* name;            // the class's name without "AITemplate_"
    EBotPawnClass pawn;          // PawnClass
    const char* weapon;          // MainWeaponClass, by the port's name; "" for the taser
    float dispersion_max, dispersion_min;  // Dispersion_Max, Dispersion_Min
    ByRange offset_max;          // HorizontalOffset_Max
    float offset_min;            // HorizontalOffset_Min
    float y_offset_max, y_offset_min;      // VerticalOffset_Max, VerticalOffset_Min
    ByRange threshold;           // OffsetThreshold: a hit counts at or under it
    float move_sideways, move_away, move_toward;  // MoveSidewaysMultiplier, MoveAwayMultiplier, MoveTowardMultiplier
    ByDifficulty rate[3];        // ImprovementRates_Near, _Medium, _Far (uu/s)
    float movement_rate_x, movement_rate_y;       // MovementRate_Horizontal, MovementRate_Vertical
};

// Every section of DefaultAITemplates.ini as it comes out with its parents' values (a key a section
// does not write is its parent class's; every class reaches AITemplate_Default). MiniUzi has no
// section and writes nothing: it is PatrolCop_SteyrTMP, its parent.
#define ME_AIM_DEFAULT_RATES {{20.0f, 35.0f, 40.0f}, {20.0f, 35.0f, 40.0f}, {20.0f, 35.0f, 40.0f}}
#define ME_AIM_DEFAULT(name, pawn, weapon) \
    {name, pawn, weapon, 35.0f, 10.0f, {50.0f, 90.0f, 200.0f}, 5.0f, 40.0f, 10.0f, {40.0f, 70.0f, 130.0f}, 0.8f, 0.15f, 1.3f, ME_AIM_DEFAULT_RATES, 0.9f, 25.0f}
inline constexpr AimTemplate kAimTemplates[] = {
    ME_AIM_DEFAULT("Default", EBotPawnClass::Assault, "FNSCARL"),
    ME_AIM_DEFAULT("Assault", EBotPawnClass::Assault, "FNSCARL"),
    ME_AIM_DEFAULT("Assault_MP5K", EBotPawnClass::Assault, "MP5K"),
    ME_AIM_DEFAULT("Assault_HKG36C", EBotPawnClass::Assault, "G36C"),
    {"Assault_Neostead", EBotPawnClass::Assault, "Neostead", 35.0f, 10.0f, {15.0f, 40.0f, 200.0f}, 5.0f, 40.0f, 10.0f, {10.0f, 20.0f, 130.0f}, 0.8f, 0.15f,
     1.3f, ME_AIM_DEFAULT_RATES, 0.9f, 25.0f},
    ME_AIM_DEFAULT("PatrolCop", EBotPawnClass::PatrolCop, "Colt1911"),
    ME_AIM_DEFAULT("PatrolCop1", EBotPawnClass::PatrolCop, "Colt1911"),
    ME_AIM_DEFAULT("PatrolCop2", EBotPawnClass::PatrolCop, "Colt1911"),
    ME_AIM_DEFAULT("PatrolCop3", EBotPawnClass::PatrolCop, "Colt1911"),
    ME_AIM_DEFAULT("PatrolCop_Glock", EBotPawnClass::PatrolCop, "Glock18"),
    ME_AIM_DEFAULT("PatrolCop_SteyrTMP", EBotPawnClass::PatrolCopSteyr, "SteyrTMP"),
    ME_AIM_DEFAULT("PatrolCop_MiniUzi", EBotPawnClass::PatrolCopSteyr, "SteyrTMP"),
    {"PatrolCop_Remington", EBotPawnClass::PatrolCopRemington, "Remington870", 35.0f, 10.0f, {15.0f, 40.0f, 200.0f}, 5.0f, 40.0f, 10.0f,
     {10.0f, 20.0f, 130.0f}, 0.8f, 0.15f, 1.3f, ME_AIM_DEFAULT_RATES, 0.9f, 25.0f},
    {"Support", EBotPawnClass::Support, "FNMinimi", 35.0f, 10.0f, {60.0f, 140.0f, 200.0f}, 5.0f, 40.0f, 10.0f, {40.0f, 70.0f, 120.0f}, 1.0f, -0.2f, 2.0f,
     ME_AIM_DEFAULT_RATES, 0.9f, 25.0f},
    ME_AIM_DEFAULT("RiotCop", EBotPawnClass::RiotCop, "SteyrTMP"),
    {"PursuitCop", EBotPawnClass::PursuitCop, "", 1.0f, 1.0f, {1.0f, 1.0f, 1.0f}, 1.0f, 1.0f, 1.0f, {40.0f, 70.0f, 130.0f}, 0.8f, 0.15f, 1.3f,
     ME_AIM_DEFAULT_RATES, 0.9f, 25.0f},
    {"SniperCop", EBotPawnClass::SniperCop, "M95", 0.0f, 0.0f, {100.0f, 100.0f, 100.0f}, 5.0f, 40.0f, 10.0f, {75.0f, 75.0f, 75.0f}, 0.8f, 0.15f, 1.3f,
     ME_AIM_DEFAULT_RATES, 0.9f, 25.0f},
    {"SniperIdiot", EBotPawnClass::SniperCop, "M95", 0.0f, 0.0f, {100.0f, 100.0f, 100.0f}, 5.0f, 40.0f, 10.0f, {75.0f, 75.0f, 75.0f}, 0.8f, 0.15f, 1.3f,
     ME_AIM_DEFAULT_RATES, 0.9f, 25.0f},
    {"SniperCeleste", EBotPawnClass::SniperCeleste, "M95", 0.0f, 0.0f, {100.0f, 100.0f, 100.0f}, 5.0f, 40.0f, 10.0f, {75.0f, 75.0f, 75.0f}, 0.8f, 0.15f,
     1.3f, ME_AIM_DEFAULT_RATES, 0.9f, 25.0f},
    {"Celeste", EBotPawnClass::Celeste, "BerettaM93R", 35.0f, 25.0f, {50.0f, 50.0f, 50.0f}, 5.0f, 40.0f, 10.0f, {45.0f, 45.0f, 45.0f}, 0.5f, 0.1f, 1.0f,
     {{15.0f, 30.0f, 25.0f}, {10.0f, 15.0f, 20.0f}, {5.0f, 10.0f, 15.0f}}, 0.9f, 25.0f},
    {"Gunner", EBotPawnClass::Gunner, "FNMinimi", 35.0f, 10.0f, {200.0f, 200.0f, 200.0f}, 5.0f, 40.0f, 10.0f, {80.0f, 80.0f, 80.0f}, 0.55f, -0.2f, 1.0f,
     {{15.0f, 30.0f, 35.0f}, {10.0f, 25.0f, 30.0f}, {5.0f, 15.0f, 25.0f}}, 0.9f, 25.0f},
    {"HeliSniper", EBotPawnClass::JKSniper, "FNSCARL", 35.0f, 10.0f, {50.0f, 90.0f, 200.0f}, 5.0f, 40.0f, 10.0f, {40.0f, 70.0f, 130.0f}, 0.5f, 0.15f, 0.5f,
     ME_AIM_DEFAULT_RATES, 0.9f, 25.0f},
    ME_AIM_DEFAULT("Tutorial", EBotPawnClass::Tutorial, "Colt1911"),
    ME_AIM_DEFAULT("Dummy", EBotPawnClass::Dummy, "FNSCARL"),
};
#undef ME_AIM_DEFAULT
#undef ME_AIM_DEFAULT_RATES

// The template a bot was made from, by EnemyBot::archetype: the class name the level's
// SeqAct_TdActorFactory names as its BotTemplate (the loader keeps it whole, "AITemplate_Assault_MP5K"),
// found by its exact name. The mesh's choice (AnimSystem::resolve_enemy_archetype) cannot serve: it
// goes by parts of the name, and one mesh is several pawn classes. The names the port makes up
// itself are told apart here: its debug squad's and the Training Area's guards' ("Assault_SWAT",
// "Support_Shotgun", "PatrolCop_SMG"), the staged tutorial's partner ("TutorialTrainer_Celeste": the
// mesh is Celeste's, the pawn TdBotPawn_Tutorial), and a placed pawn's class name
// ("TdBotPawn_Assault"). A factory that names no template has AITemplate_Default (the loader).
inline const AimTemplate& find_aim_template(std::string_view archetype) {
    const auto row = [](std::string_view name) -> const AimTemplate* {
        for (const AimTemplate& t : kAimTemplates) {
            if (name == t.name) return &t;
        }
        return nullptr;
    };
    std::string_view name = archetype;
    for (std::string_view prefix : {std::string_view("AITemplate_"), std::string_view("TdBotPawn_")}) {
        if (name.substr(0, prefix.size()) == prefix) name.remove_prefix(prefix.size());
    }
    if (const AimTemplate* t = row(name)) return *t;
    static constexpr struct {
        std::string_view name, is;
    } kOwn[] = {
        {"Assault_SWAT", "Assault"},
        {"Support_Shotgun", "Support"},
        {"PatrolCop_SMG", "PatrolCop_SteyrTMP"},
        {"TutorialTrainer_Celeste", "Tutorial"},
        // Pawn classes that are no template's name.
        {"PatrolCop_Steyr", "PatrolCop_SteyrTMP"},
        {"SupportHelicopterGunner", "Gunner"},
        {"JKSniper", "HeliSniper"},
        {"TempPursuit", "PursuitCop"},
        {"MeleeDummy", "Dummy"},
        {"PatrolDummy", "Dummy"},
        {"PatrolMeleeDummy", "Dummy"},
        {"PursuitDummy", "Dummy"},
    };
    for (const auto& own : kOwn) {
        if (name == own.name) return *row(own.is);
    }
    // Anything else by what its name holds, as the mesh is chosen; a name that says nothing is the
    // class every template extends.
    std::string low(name);
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto has = [&](const char* part) { return low.find(part) != std::string::npos; };
    if (has("tutorial")) return *row("Tutorial");
    if (has("sniper")) return *row(has("celeste") ? "SniperCeleste" : "SniperCop");
    if (has("celeste")) return *row("Celeste");
    if (has("pursuit")) return *row("PursuitCop");
    if (has("riot")) return *row("RiotCop");
    if (has("gunner")) return *row("Gunner");
    if (has("support")) return *row("Support");
    if (has("patrol")) return *row(has("remington") ? "PatrolCop_Remington" : (has("steyr") || has("tmp") || has("uzi")) ? "PatrolCop_SteyrTMP" : "PatrolCop");
    if (has("assault") || has("swat")) return *row("Assault");
    return *row("Default");
}

// TdAI_Tutorial.IsHitRelevant overrides the controller's and answers true: the Training Area's
// partner's hits are never held back by her aim.
inline bool is_tutorial_template(const AimTemplate& t) { return t.pawn == EBotPawnClass::Tutorial; }

// A helicopter's riders (TdVehicle_Helicopter.AddGunner -> TdPawn.SetAsGunner: seated at a socket,
// their collision off). Their fire is the door gun's, which the helicopter has here
// (HelicopterInstance::gunner): the bot the level's factory makes for one fires nothing of his own.
inline bool is_helicopter_rider(const AimTemplate& t) { return t.pawn == EBotPawnClass::Gunner || t.pawn == EBotPawnClass::JKSniper; }

// -----------------------------------------------------------------------------
// A bot's armour (TdBotPawn.TakeDamage, TdBotPawn.AdjustDamage, TdPawn.AdjustDamage)
// -----------------------------------------------------------------------------

// The armour a bullet meets, chosen by the bone of the body it went into (HitInfo.BoneName): 'Neck'
// (the body round the head) the head's; LeftUpLeg, RightUpLeg and the bones under them (the shins'
// and the feet's bodies) the legs'; anything else, the hands' bodies too, the body's.
inline float bot_bullet_armour(EBotPawnClass pawn, std::string_view bone, int difficulty) {
    const BotArmour& a = kBotArmour[static_cast<size_t>(pawn)];
    if (bone == "Neck") return a.head.at(difficulty);
    for (std::string_view leg : {std::string_view("LeftUpLeg"), std::string_view("LeftLeg"), std::string_view("LeftFoot"), std::string_view("RightUpLeg"),
                                 std::string_view("RightLeg"), std::string_view("RightFoot")}) {
        if (bone == leg) return a.legs.at(difficulty);
    }
    return a.body.at(difficulty);
}

// What the player's bullet takes from a bot. TdWeapon.ProcessInstantHit: int(GetInstantHitDamage);
// TdBotPawn.TakeDamage: x DamageMultiplier_Head (2.0) on a body whose material is
// PM_Character_Head, x DamageMultiplier_Body (1.0) on any other, to an int again;
// TdBotPawn.AdjustDamage: a sniper's bullet is 99999 wherever it lands; else TdPawn.AdjustDamage,
// the armour of the bone. `in_mesh` is whether the hit has a bone at all (HitInfo.HitComponent is
// the pawn's TdSkeletalMeshComponent): without one no armour is taken off.
inline int bot_bullet_damage(const AimTemplate& t, float weapon_damage, EBulletKind kind, bool head_material, std::string_view bone, bool in_mesh,
                             int difficulty) {
    int damage = static_cast<int>(weapon_damage);
    damage = static_cast<int>(static_cast<float>(damage) * (head_material ? 2.0f : 1.0f));
    if (kind == EBulletKind::Sniper) return 99999;
    if (!in_mesh) return std::max(0, damage);
    return armoured_damage(damage, bot_bullet_armour(t.pawn, bone, difficulty));
}

// -----------------------------------------------------------------------------
// What her moves do to a bot's aim (TdMove.AiAimPenalties, AiAimOneShotPenalties)
// -----------------------------------------------------------------------------

struct MoveAimPenalty {
    EMovement move;
    ByDifficulty rate;  // AiAimPenalties: multiplies the improvement rate while she is in the move
    float one_shot;     // AiAimOneShotPenalties (the same at every difficulty): uu added as she enters it
};
// The 23 of her 63 moves that write either (Default__TdMove_* in TdGame.u; Default__TdMove has
// 1, 1, 1 and no one-shot penalty). Walking, sprinting, falling, crouching, landing, the skill roll,
// grabbing, climbing, melee and the stumbles leave the aim alone. Not in the port's list of moves:
// 40 MOVE_WallRun (as the wall runs), 51 and 52 (as 49).
inline constexpr MoveAimPenalty kMoveAimPenalties[] = {
    {EMovement::MOVE_WallRunningRight, {0.4f, 0.5f, 0.7f}, 0.0f},
    {EMovement::MOVE_WallRunningLeft, {0.4f, 0.5f, 0.7f}, 0.0f},
    {EMovement::MOVE_WallClimbing, {0.4f, 0.5f, 0.7f}, 0.0f},
    {EMovement::MOVE_SpringBoarding, {0.3f, 0.4f, 0.6f}, 75.0f},
    {EMovement::MOVE_SpeedVaulting, {1.0f, 1.0f, 1.0f}, 50.0f},
    {EMovement::MOVE_VaultOver, {1.0f, 1.0f, 1.0f}, 50.0f},
    {EMovement::MOVE_Jump, {0.7f, 0.8f, 1.0f}, 0.0f},
    {EMovement::MOVE_WallRunJump, {1.0f, 1.0f, 1.0f}, 75.0f},
    {EMovement::MOVE_Slide, {0.7f, 0.8f, 0.9f}, 0.0f},
    {EMovement::MOVE_Snatch, {0.01f, 0.08f, 0.3f}, 200.0f},
    {EMovement::MOVE_ZipLine, {0.01f, 0.1f, 0.3f}, 200.0f},
    {EMovement::MOVE_Balance, {1.0f, 1.0f, 1.0f}, 50.0f},
    {EMovement::MOVE_GrabTransfer, {1.0f, 1.0f, 1.0f}, 75.0f},
    {EMovement::MOVE_DodgeJump, {1.0f, 1.0f, 1.0f}, 75.0f},
    {EMovement::MOVE_WallRunDodgeJump, {1.0f, 1.0f, 1.0f}, 75.0f},
    {EMovement::MOVE_RumpSlide, {0.01f, 0.04f, 0.1f}, 200.0f},
    {EMovement::MOVE_WallClimbDodgeJump, {1.0f, 1.0f, 1.0f}, 75.0f},
    {EMovement::MOVE_WallClimb180TurnJump, {0.3f, 0.4f, 0.6f}, 100.0f},
    {EMovement::MOVE_Swing, {0.1f, 0.2f, 0.4f}, 200.0f},
    {EMovement::MOVE_SwingJump, {0.2f, 0.3f, 0.5f}, 200.0f},
};
// ATdPawn vtbl[0x454] (0x012ba8b0): Moves[MovementState].AiAimPenalty, as TdMove.SetDifficulty set it.
inline float move_aim_penalty(EMovement move, int difficulty) {
    for (const MoveAimPenalty& p : kMoveAimPenalties) {
        if (p.move == move) return p.rate.at(difficulty);
    }
    return 1.0f;
}
// ATdPawn vtbl[0x458] (0x012ba8d0): Moves[MovementState].AiAimOneShotPenalty, on the tick her move
// is not the one TdPawn.Tick last noted (AIAimOldMovementState); the caller knows whether it is.
inline float move_aim_one_shot(EMovement move) {
    for (const MoveAimPenalty& p : kMoveAimPenalties) {
        if (p.move == move) return p.one_shot;
    }
    return 0.0f;
}

// -----------------------------------------------------------------------------
// TdAimBot
// -----------------------------------------------------------------------------

// What the aim bot reads of its enemy.
struct AimTarget {
    Vec3 location{0.0f, 0.0f, 0.0f};   // Target.Location: the middle of her capsule
    Vec3 viewpoint{0.0f, 0.0f, 0.0f};  // TdPawn.GetViewpointLocation: her third-person mesh's EyeJoint
    Vec3 velocity{0.0f, 0.0f, 0.0f};
    float yaw_deg = 0.0f;              // Rotation.Yaw
    float max_speed = 720.0f;          // SpeedCurve_LightWeapon's last point
    EMovement move = EMovement::MOVE_Walking;  // MovementState
    bool move_changed = false;         // ...is not AIAimOldMovementState this tick
};

// FRand(), a bot's own: 24 bits of a linear congruential step.
inline float bot_frand(uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return static_cast<float>(state >> 8) * (1.0f / 16777216.0f);
}

inline float aim_sign(float x) { return x < 0.0f ? -1.0f : 1.0f; }  // TdAimBot.sign

// TdAimBot.GetTargetLocation, GetTargetViewpointLocation: where she is, or, out of his sight, where
// he last saw her (and 50 over it).
inline Vec3 aim_target_location(const BotCombatState& s, const AimTarget& target) { return s.enemy_visible ? target.location : s.last_seen; }
inline Vec3 aim_target_viewpoint(const BotCombatState& s, const AimTarget& target) {
    return s.enemy_visible ? target.viewpoint : s.last_seen + Vec3(0.0f, 0.0f, 50.0f);
}

// UTdAimBot::GetImprovementRate (0x01252610). `pawn_location` is the bot's.
inline float aim_improvement_rate(const BotCombatState& s, const AimTemplate& tpl, int difficulty, const Vec3& pawn_location, const AimTarget& target) {
    if (s.improvement_override != 0.0f) return s.improvement_override;
    float rate = tpl.rate[s.firing_range == kFiringRangeNear ? 0 : s.firing_range == kFiringRangeFar ? 2 : 1].at(difficulty);
    float mult = tpl.move_toward;
    if (target.velocity.length_xy() > 200.0f) {
        const float d = target.velocity.normalized_xy().dot((pawn_location - target.location).normalized_xy());
        mult = std::abs(d) < 0.7f ? tpl.move_sideways : d < 0.0f ? tpl.move_away : tpl.move_toward;
    }
    rate *= mult;
    // (A rate under nothing, MoveAwayMultiplier -0.2, is not multiplied: her move cannot turn it round.)
    if (rate > 0.0f) rate *= move_aim_penalty(target.move, difficulty);
    return rate;
}

// UTdAimBot::UpdateDispersion (0x012537f0), every tick. A suppressed bot's aim does not improve
// (0x011e7f90: Suppression under nothing); nothing here suppresses a bot.
inline void aim_update_dispersion(BotCombatState& s, const AimTemplate& tpl, int difficulty, const Vec3& pawn_location, const AimTarget& target, float dt) {
    if (s.has_enemy) {
        const float penalty = target.move_changed ? move_aim_one_shot(target.move) : 0.0f;
        s.base_dispersion += penalty;
        s.base_offset += penalty;
        s.base_y_offset += penalty;
        if (s.enemy_visible) {
            const float step = aim_improvement_rate(s, tpl, difficulty, pawn_location, target) * dt;
            s.base_dispersion -= step;
            s.base_offset -= step;
            s.base_y_offset -= step;
        }
    }
    s.base_dispersion = std::clamp(s.base_dispersion, tpl.dispersion_min, tpl.dispersion_max);
    s.base_offset = std::clamp(s.base_offset, tpl.offset_min, tpl.offset_max.at(s.firing_range));
    s.base_y_offset = std::clamp(s.base_y_offset, tpl.y_offset_min, tpl.y_offset_max);
}

// TdAimBot.GetOffsetPoint: X uu to the side of the line from the bot to her (level, square to it),
// Y uu over her Location.
inline Vec3 aim_offset_point(const Vec3& pawn_location, const Vec3& target_location, float x, float y) {
    const Vec3 dir = (target_location - pawn_location).normalized();
    return Vec3(dir.y * x, -dir.x * x, y);  // TargetDir cross (0, 0, X), then Z += Y
}

// TdAimBot.GetDispersionOffset: BaseOffset, more by her speed over her top speed of
// HorizontalOffset_Max when she crosses his view at 200 uu/s or more, and to the other side when
// her yaw is clockwise of the line from him to her.
inline float aim_dispersion_offset(const BotCombatState& s, const AimTemplate& tpl, const Vec3& pawn_location, const AimTarget& target) {
    const Vec3 target_location = aim_target_location(s, target);
    // TdAIController.GetLeftEnemyDir: (EnemyDir.Y, -EnemyDir.X, 0)
    const Vec3 enemy_dir = (target.location - pawn_location).normalized();
    const float across = std::abs(Vec3(enemy_dir.y, -enemy_dir.x, 0.0f).dot(target.velocity));
    float offset = s.base_offset;
    if (across >= 200.0f) offset = s.base_offset + tpl.offset_max.at(s.firing_range) * target.velocity.length() / std::max(target.max_speed, 1.0f);
    // ClockwiseFrom(Target.Rotation.Yaw, rotator(TargetLocation - Pawn.Location).Yaw), on yaws of 65536 a turn
    const Vec3 to_target = target_location - pawn_location;
    const int a = static_cast<int>(std::lround(target.yaw_deg * DEG2UE_ROT)) & 0xFFFF;
    const int b = static_cast<int>(std::lround(std::atan2(to_target.y, to_target.x) * RAD2UE_ROT)) & 0xFFFF;
    const bool facing_left = std::abs(a - b) > 32768 ? a < b : a > b;
    return facing_left ? -offset : offset;
}

// TdAimBot.PickPoint: a new sweep. It starts the offset and up to the dispersion more to one side,
// between her eyes and BaseYOffset under them, and is to end as far on the other side.
inline Vec3 aim_pick_point(BotCombatState& s, const AimTemplate& tpl, const Vec3& pawn_location, const AimTarget& target) {
    const Vec3 target_location = aim_target_location(s, target);
    const float offset = aim_dispersion_offset(s, tpl, pawn_location, target);
    s.current_x = offset + aim_sign(offset) * (s.base_dispersion * bot_frand(s.random));
    s.current_y = (aim_target_viewpoint(s, target).z - target_location.z) - bot_frand(s.random) * s.base_y_offset;
    float direction = 1.0f;
    if (!(target.velocity.length() > 10.0f) && bot_frand(s.random) < 0.5f) direction = -1.0f;
    s.current_x *= direction;
    s.target_x = -s.current_x;
    s.diff_x = s.current_x - s.target_x;
    return target_location + aim_offset_point(pawn_location, target_location, s.current_x, s.current_y);
}

// TdAimBot.StartBurst: the aim at its worst.
inline Vec3 aim_start_burst(BotCombatState& s, const AimTemplate& tpl, const Vec3& pawn_location, const AimTarget& target, float now) {
    s.time_of_last_shot = now;
    s.base_dispersion = tpl.dispersion_max;
    s.base_offset = tpl.offset_max.at(s.firing_range);  // GetMaxOffset (0x012525c0)
    s.base_y_offset = tpl.y_offset_max;
    s.current_location = aim_pick_point(s, tpl, pawn_location, target);
    return s.current_location;
}

// TdAimBot.GetNextLocation: the sweep goes on by MovementRateX of its width a second of the time
// since the last shot, a quarter more or less at random, and the point climbs at MovementRateY to
// her eyes (and drops 10 once it is over them); across and far enough out, a new sweep is picked.
inline Vec3 aim_next_location(BotCombatState& s, const AimTemplate& tpl, const Vec3& pawn_location, const AimTarget& target, float now) {
    const float since = now - s.time_of_last_shot;
    const float new_aim_y = tpl.movement_rate_y * since;
    const float error_z = aim_target_viewpoint(s, target).z - s.current_location.z;
    float delta_y = error_z > 1.0f ? new_aim_y * aim_sign(s.current_y) : -10.0f;
    const float new_aim_x = tpl.movement_rate_x * s.diff_x * since;
    float delta_x = 0.0f;
    if (std::abs(s.target_x - s.current_x) > 1.0f) delta_x = -new_aim_x;
    {
        const float part = bot_frand(s.random);
        delta_x += part * delta_x * (bot_frand(s.random) < 0.5f ? -0.25f : 0.25f);
    }
    {
        const float part = bot_frand(s.random);
        delta_y += part * delta_y * (bot_frand(s.random) < 0.5f ? -1.0f : 1.0f);
    }
    s.current_x += delta_x;
    s.current_y += delta_y;
    if (aim_sign(s.target_x) == aim_sign(s.current_x) &&
        std::abs(s.current_x) >= std::min(std::abs(s.target_x), std::abs(aim_dispersion_offset(s, tpl, pawn_location, target)))) {
        s.current_location = aim_pick_point(s, tpl, pawn_location, target);
    } else {
        const Vec3 target_location = aim_target_location(s, target);
        s.current_location = target_location + aim_offset_point(pawn_location, target_location, s.current_x, s.current_y);
    }
    s.time_of_last_shot = now;
    return s.current_location;
}

// TdAIController.GetAdjustedAimFor: the point the next shot is aimed at. TdAimBot_Perfect's is her
// Location itself.
inline Vec3 aim_location(BotCombatState& s, const AimTemplate& tpl, const Vec3& pawn_location, const AimTarget& target, float now) {
    Vec3 point;
    if (s.perfect) {
        point = target.location;
    } else if (s.first_shot) {
        point = aim_start_burst(s, tpl, pawn_location, target, now);
    } else {
        point = aim_next_location(s, tpl, pawn_location, target, now);
    }
    s.first_shot = false;
    return point;
}

// TdAimBot.IsHitRelevant (TdPlayerPawn.TakeDamage's first statement): whether a bullet that landed
// on her counts. TdAimBot.ShouldMiss is BaseOffset > GetOffsetThreshold().
inline bool aim_hit_relevant(const BotCombatState& s, const AimTemplate& tpl, EBulletKind kind) {
    if (s.perfect || is_tutorial_template(tpl)) return true;
    if (kind != EBulletKind::LowCaliber && kind != EBulletKind::HighCaliber && kind != EBulletKind::Shotgun) return true;
    return !(s.base_offset > tpl.threshold.at(s.firing_range));
}

// Weapon.AddSpread: the aim, off by up to half of Spread to the side and less than that up or down.
inline Vec3 add_weapon_spread(const Vec3& aim, float spread, uint32_t& random) {
    if (spread == 0.0f) return aim;
    // GetAxes(BaseAim, X, Y, Z)
    const float yaw = std::atan2(aim.y, aim.x), pitch = std::asin(std::clamp(aim.z, -1.0f, 1.0f));
    const Rotator rot = Rotator::from_radians(pitch, yaw, 0.0f);
    const float rand_y = bot_frand(random) - 0.5f;
    const float rand_z = std::sqrt(0.5f - rand_y * rand_y) * (bot_frand(random) - 0.5f);
    return (aim + rot.right() * (rand_y * spread) + rot.up() * (rand_z * spread)).normalized();
}

// One tick of what a bot's controller and pawn keep for his fire, before his trigger is looked at:
// the sight of her (TdAIController.EnemyNotVisible draws KeepFiringTime as it is lost), the range
// band and its burst pattern (ATdBotPawn::SetAIFiringState, 0x01236380), the flags of 0x011ea200
// (a bot with no line of fire has bFirstShot set), and the aim's own tick. `distance_2d` is from his
// pawn to hers on the level (the one place script writes EnemyDistance it is VSize2D). Returns
// whether he has a line of fire and is within WeaponRange of her, which is when he fires.
inline bool aim_tick(BotCombatState& s, const AimTemplate& tpl, const BotWeapon& weapon, int difficulty, const Vec3& pawn_location, const AimTarget& target,
                     bool visible, float distance_2d, float now, float dt) {
    if (!s.has_enemy) {
        // TdAIController.SetEnemy
        s.has_enemy = true;
        s.enemy_visible = false;
        s.enemy_lost_time = -1.0e9f;
    }
    if (s.random == 0) s.random = 0x9E3779B9u;
    if (visible) {
        s.enemy_visible = true;
        s.last_seen = target.location;
    } else if (s.enemy_visible) {
        s.enemy_visible = false;
        s.enemy_lost_time = now;
        s.keep_firing_time = kMinKeepFiringTime + (kMaxKeepFiringTime - kMinKeepFiringTime) * bot_frand(s.random);
    }
    const uint8_t range = classify_firing_range(distance_2d * distance_2d, weapon.combat_range_max);
    if (!s.range_known || range != s.firing_range) {
        const BurstPattern& b = weapon.bursts[range];
        s.burst_min = b.length_min;
        s.burst_max = b.length_max;
        s.burst_delay_min = b.pause_min;
        s.burst_delay_max = b.pause_max;
        s.firing_range = range;
        s.range_known = true;
    }
    const bool line_of_fire = s.enemy_visible || s.keep_firing_time + s.enemy_lost_time > now;
    if (!line_of_fire) s.first_shot = true;
    aim_update_dispersion(s, tpl, difficulty, pawn_location, target, dt);
    return line_of_fire && weapon.range >= distance_2d;
}

// A bot with no enemy (TdAIController.SetEnemy(None)): he sees nobody and has no line of fire.
inline void aim_lose_enemy(BotCombatState& s) {
    s.has_enemy = false;
    s.enemy_visible = false;
    s.first_shot = true;
}

// -----------------------------------------------------------------------------
// Faith under fire: the stand-in for her third-person bodies
// -----------------------------------------------------------------------------

// A bot's bullet can meet her only in the bodies of CH_TKY_Crim_Fixer.Faith3p_Physics on her
// third-person mesh (TdPlayerPawn's TdPawnMesh3p; her cylinders and her first-person mesh stop no
// line without extent). The port has neither that mesh nor the tree that poses it (AT_C3P). Of the
// asset's 21 bodies these are the 11 that are fixed to the skeleton and do not hang on what the
// arms do (Hips, Spine1, Spine2, Neck, Head, the thighs, shins and feet), 12 shapes, as they stand
// in the asset's reference pose: x her forward, y her right, z up from her soles. Left out: the
// six bodies of the arms, which the reference pose holds straight out to the sides, and the four
// of her hair, which hang free. Which of them a bullet meets changes nothing: her armour is the
// same on every part and IsHitRelevant does not look at the bone.
struct PlayerBodyShape {
    const char* bone;
    EBodyShape kind;
    bool head;  // PM_Character_Head
    Vec3 centre;
    Vec3 axis[3];
    Vec3 half;
};
inline constexpr PlayerBodyShape kPlayerBodies[] = {
    {"Hips", EBodyShape::Box, false, {2.422f, -0.000f, 106.600f}, {{0.090f, 0.000f, 0.996f}, {0.000f, -1.000f, 0.000f}, {0.996f, -0.000f, -0.090f}}, {6.500f, 19.971f, 10.488f}},
    {"Hips", EBodyShape::Capsule, false, {-4.179f, -0.000f, 97.918f}, {{0.101f, 0.000f, 0.995f}, {-0.995f, -0.000f, 0.101f}, {0.000f, -1.000f, 0.000f}}, {9.001f, 9.001f, 14.470f}},
    {"Spine1", EBodyShape::Box, false, {4.569f, 0.000f, 129.590f}, {{-0.062f, 0.000f, 0.998f}, {0.000f, -1.000f, 0.000f}, {0.998f, 0.000f, 0.062f}}, {10.504f, 17.662f, 12.598f}},
    {"Spine2", EBodyShape::Capsule, false, {1.682f, 0.000f, 143.222f}, {{-0.031f, 0.000f, 1.000f}, {-1.000f, -0.000f, -0.031f}, {0.000f, -1.000f, 0.000f}}, {12.029f, 12.029f, 10.750f}},
    {"Neck", EBodyShape::Capsule, true, {-0.900f, -0.013f, 157.984f}, {{0.269f, 0.000f, 0.963f}, {-0.050f, 0.999f, 0.014f}, {-0.962f, -0.052f, 0.269f}}, {7.242f, 7.242f, 0.250f}},
    {"LeftUpLeg", EBodyShape::Capsule, false, {-0.306f, -12.735f, 80.990f}, {{-0.008f, -0.995f, 0.104f}, {0.997f, 0.000f, 0.073f}, {-0.073f, 0.105f, 0.992f}}, {11.880f, 11.880f, 12.967f}},
    {"LeftLeg", EBodyShape::Capsule, false, {-2.173f, -12.242f, 39.242f}, {{0.000f, -1.000f, -0.000f}, {0.989f, 0.000f, -0.145f}, {0.145f, 0.000f, 0.989f}}, {10.265f, 10.265f, 14.470f}},
    {"LeftFoot", EBodyShape::Box, false, {2.378f, -10.992f, 4.308f}, {{0.000f, -1.000f, -0.000f}, {-0.999f, -0.000f, 0.050f}, {-0.050f, -0.000f, -0.999f}}, {7.504f, 14.672f, 3.750f}},
    {"RightUpLeg", EBodyShape::Box, false, {1.260f, 11.402f, 80.092f}, {{-0.001f, 0.999f, 0.035f}, {1.000f, 0.000f, 0.021f}, {0.021f, 0.035f, -0.999f}}, {9.703f, 12.089f, 20.707f}},
    {"RightLeg", EBodyShape::Capsule, false, {-1.696f, 10.739f, 37.333f}, {{0.010f, -0.998f, 0.069f}, {-0.989f, -0.000f, 0.145f}, {-0.145f, -0.070f, -0.987f}}, {9.803f, 9.803f, 10.496f}},
    {"RightFoot", EBodyShape::Box, false, {2.252f, 10.742f, 4.237f}, {{0.000f, -1.000f, -0.000f}, {0.999f, 0.000f, -0.050f}, {0.050f, 0.000f, 0.999f}}, {7.236f, 15.298f, 4.256f}},
    {"Head", EBodyShape::Capsule, true, {1.348f, 0.000f, 170.904f}, {{0.082f, 0.000f, 0.997f}, {0.000f, -1.000f, 0.000f}, {0.997f, -0.000f, -0.082f}}, {11.885f, 11.885f, 3.123f}},
};
// Her mesh's Origin is (0, 94, 0) (SK_TKY_Crim_Fixer): its soles are 94 under the pawn's Location,
// which is 90 (CollisionHeight) over the capsule's bottom, where the controller has her feet.
inline constexpr float kPlayerSolesUnderFeet = 4.0f;
// The reference pose is 183.04 from soles to the top of the head, for a capsule 180 high.
inline constexpr float kPlayerStandingHeight = 180.0f;

// The stand-in as it stands in the world: her feet (the capsule's bottom), her body's yaw, and the
// height of her capsule now. Lower than standing (crouched, sliding, rolling: 122) every shape is
// brought down by that part of its height over her soles; retail's own pose there is her
// animation's, which nothing here has.
inline void pose_player_stand_in(const Vec3& feet, float yaw_deg, float capsule_height, EnemyBodySet& out) {
    const float squash = std::clamp(capsule_height / kPlayerStandingHeight, 0.25f, 1.0f);
    const float cy = std::cos(yaw_deg * DEG2RAD), sy = std::sin(yaw_deg * DEG2RAD);
    const auto turn = [&](const Vec3& v) { return Vec3(v.x * cy - v.y * sy, v.x * sy + v.y * cy, v.z); };
    out.bodies.clear();
    out.shapes.clear();
    Vec3 sum(0.0f, 0.0f, 0.0f);
    for (const PlayerBodyShape& p : kPlayerBodies) {
        if (out.bodies.empty() || out.bodies.back().bone != p.bone) out.bodies.push_back(EnemyBody{p.bone, p.head ? ECharacterSurface::Head : ECharacterSurface::Body});
        BodyShape s;
        s.kind = p.kind;
        s.body = static_cast<uint8_t>(out.bodies.size() - 1);
        s.centre = feet + turn(Vec3(p.centre.x, p.centre.y, p.centre.z * squash - kPlayerSolesUnderFeet));
        for (int i = 0; i < 3; ++i) s.axis[i] = turn(p.axis[i]).normalized();
        s.half = p.half;
        sum += s.centre;
        out.shapes.push_back(s);
    }
    out.bound_centre = sum * (1.0f / static_cast<float>(out.shapes.size()));
    out.bound_radius = 0.0f;
    for (const BodyShape& s : out.shapes) {
        const float reach = s.kind == EBodyShape::Box ? s.half.length() : s.half.x + s.half.z;
        out.bound_radius = std::max(out.bound_radius, (s.centre - out.bound_centre).length() + reach);
    }
    out.valid = true;
}

// -----------------------------------------------------------------------------
// The helicopter under the player's fire
// -----------------------------------------------------------------------------

// What a bullet meets of a TdVehicle_Helicopter is its hidden SVehicleMesh
// (VH_Common.SK_Helicopter_Placeholder; CollideActors and BlockZeroExtent from Engine's SVehicle),
// through that mesh's physics asset, PA_Helicopter_Placeholder: one body on the root bone Chassis
// with one box, 487.401 x 202.882 x 137.5 about (0, 0, 434). The mesh has no Origin, no RotOrigin
// and the bone no turn, so the box is square to the actor: 243.7 fore and aft, 101.44 to each
// side, from 365.25 to 502.75 over its Location. The hull that is drawn (the factory's
// HelicopterSkeletalMesh) has no collision of its own. The body names no physical material: the
// hit has the engine's default one, which has no impact properties, so the weapon's
// DefaultImpactMaterial gives the effect and the sound, and a skeletal mesh takes no bullet hole.
// TdVehicle_Helicopter has Health 300 and no TakeDamage of its own; what its death would be was
// not found, and nothing is taken from it here. Its riders have no collision while they are seated
// (TdPawn.SetAsGunner: SetCollision(false, false)).
inline constexpr Vec3 kHelicopterBoxCentre{0.0f, 0.0f, 434.0f};
inline constexpr Vec3 kHelicopterBoxHalf{243.7005f, 101.441f, 68.75f};

// Whether a helicopter is there to be hit: as it is drawn.
inline bool helicopter_stops_bullets(const HelicopterInstance& heli) { return heli.state != EHeliState::Dormant && heli.state != EHeliState::Destroyed; }

// The box as it stands in the world, turned as the hull is drawn (roll, then pitch, then yaw).
inline BodyShape helicopter_box(const HelicopterInstance& heli) {
    const float cy = std::cos(heli.yaw_deg * DEG2RAD), sy = std::sin(heli.yaw_deg * DEG2RAD);
    const float cp = std::cos(heli.pitch_deg * DEG2RAD), sp = std::sin(heli.pitch_deg * DEG2RAD);
    const float cr = std::cos(heli.roll_deg * DEG2RAD), sr = std::sin(heli.roll_deg * DEG2RAD);
    const auto to_world = [&](const Vec3& l) {
        const float r_y = l.y * cr - l.z * sr, r_z = l.y * sr + l.z * cr;
        const float p_x = l.x * cp + r_z * sp, p_z = -l.x * sp + r_z * cp;
        return Vec3(p_x * cy - r_y * sy, p_x * sy + r_y * cy, p_z);
    };
    BodyShape s;
    s.kind = EBodyShape::Box;
    s.axis[0] = to_world(Vec3(1.0f, 0.0f, 0.0f));
    s.axis[1] = to_world(Vec3(0.0f, 1.0f, 0.0f));
    s.axis[2] = to_world(Vec3(0.0f, 0.0f, 1.0f));
    s.centre = heli.position + to_world(kHelicopterBoxCentre);
    s.half = kHelicopterBoxHalf;
    return s;
}

// -----------------------------------------------------------------------------
// The stumble a bullet's hit plays on a bot
// -----------------------------------------------------------------------------

// TdPawn.TakeDamage -> PlayPhysicsBodyImpact -> StumbleDamage -> TdMove_StumbleBot, for a bot that
// lives through the hit and is not in a stumble already. TdMove_StumbleBase.GetStumbleState picks
// by the damage type and by where the shooter stands, not by the bone:
//   a pistol's or a rifle's bullet   14 with the shooter in front of him, 16 behind
//   a shotgun's                      from in front 18 when the hit is more than 145 over the bottom
//                                    of his capsule, else 17; from behind none at all
//                                    (TdMove_StumbleBot.CanDoMove: DiscardHitReaction)
// and TdMove_StumbleBot.PlayStumbleAnimation plays (PlayMoveAnim: rate 1, 0.1 s in):
//   14  WalkBwdStop, and he turns in 0.3 s to face the way opposite the shooter's facing
//   16  WalkFwdStop, and he turns in 0.3 s to the shooter's own facing
//   17  HitHeavyStomach      18  HitHeavyHead
// (A sniper's bullet is grouped with the blows, 3 from in front; it kills, and the dead play none.)
// HitMeleeLeft, HitMeleeRight and HitMeleeBack are a blow's alone.
// `hit_height` is the hit over the bottom of his capsule.
inline uint8_t bullet_stumble_state(EBulletKind kind, const Vec3& shooter, const Vec3& bot_location, float bot_yaw_deg, float hit_height) {
    // HitSide: Normal2D(InstigatorLocation - Location) dot vector(Rotation)
    const Vec3 to_shooter = (shooter - bot_location).normalized_xy();
    const float side = to_shooter.x * std::cos(bot_yaw_deg * DEG2RAD) + to_shooter.y * std::sin(bot_yaw_deg * DEG2RAD);
    switch (kind) {
        case EBulletKind::LowCaliber:
        case EBulletKind::HighCaliber:
            return side < 0.0f ? 16 : 14;
        case EBulletKind::Shotgun:
            if (side < 0.0f) return 0;  // 19 for a taser, which leaves the stumble at once; a shotgun's is discarded before it
            return hit_height > 145.0f ? 18 : 17;
        case EBulletKind::Taser:
            return side < 0.0f ? 19 : hit_height > 145.0f ? 18 : 17;
        case EBulletKind::Sniper:
            return side > 0.0f ? 3 : hit_height > 145.0f ? 2 : 1;
    }
    return 0;
}

struct StumbleAnim {
    const char* sequence;
    float length;  // SequenceLength in the bots' sets (AS_AI_PatrolCop_OneHanded, AS_AI_Assault_TwoHanded)
};
// The sequence of a bullet's stumble; none for the states that play none here.
inline StumbleAnim bullet_stumble_anim(uint8_t state) {
    switch (state) {
        case 14: return {"WalkBwdStop", 0.7f};
        case 16: return {"WalkFwdStop", 0.7f};
        case 17: return {"HitHeavyStomach", 1.0f};
        case 18: return {"HitHeavyHead", 1.0f};
        default: return {"", 0.0f};
    }
}
// TdMove_StumbleBot.PlayStumbleAnimation's SetPreciseRotation(.., 0.3).
inline constexpr float kStumbleTurnTime = 0.3f;
// A bot's mesh hangs 94 under his Location (its Origin), which is 90 over his capsule's bottom:
// EnemyBot::position, where his soles are drawn, is 4 under the bottom the hit's height is taken from.
inline constexpr float kBotSolesUnderCapsule = 4.0f;

}  // namespace me
