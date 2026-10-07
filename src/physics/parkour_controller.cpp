#include "parkour_controller.hpp"
#include <cmath>
#include <algorithm>
#include <iostream>

namespace me {

namespace {
// TdPawn collision cylinder: Radius=30, CollisionHeight=90. CollisionHeight is a UE3 half-height,
// so Faith is 180 tall; crouching, sliding and coiling shrink the cylinder to 122
// (TdPawn.ShrinkCollision). UE3 PHYS_Walking / PHYS_Falling sweep the axis-aligned box of the
// cylinder ("sweep out the axis aligned bounding box of just the CollisionComponent").
constexpr float kPawnRadius = 30.0f;
constexpr float kPawnHeight = 180.0f;
constexpr float kCrouchHeight = 122.0f;
// Camera heights above the feet: BaseEyeHeight 76 above the cylinder centre (90) when standing;
// the crouch / slide cameras ride the lowered first-person skeleton.
constexpr float kEyeHeightStand = 166.0f;
constexpr float kEyeHeightCrouch = 108.0f;
constexpr float kEyeHeightSlide = 70.0f;
// UE3 APawn walking constants.
constexpr float kMaxStepHeight = 35.0f;   // Pawn.MaxStepHeight
constexpr float kMaxFloorDist = 2.4f;     // MAXFLOORDIST
constexpr float kWalkableFloorZ = 0.71f;  // TdPawn.WalkableFloorZ
// Floor probes start slightly above the feet so a pawn that ends a move marginally inside a floor
// (float rounding at large world coordinates) still finds it.
constexpr float kFloorProbeLift = 10.0f;
// Distance kept from a blocking surface after a swept move (world units).
constexpr float kContactSkin = 0.1f;
// [TdGame.TdMove_SpeedVault] VaultTypes (DefaultPawnMovement.ini): obstacles up to 48 are stepped,
// 48..148 are vaulted onto / over from the ground, 145..192 need the VaultOverHigh moves started
// from a jump.
constexpr float kVaultMaxHeight = 148.0f;
constexpr float kVaultHighMinHeight = 145.0f;
constexpr float kVaultHighMaxHeight = 192.0f;
// Hands: a ledge is caught (TdMove_IntoGrab) when its top is between the chest and a little above
// the head; the hang then puts the top GrabHangDepth (182.8) above the feet.
constexpr float kGrabMinRise = 100.0f;
constexpr float kGrabMaxRise = 215.0f;
// Ledges this close below the hands are mantled straight over instead of hung from.
constexpr float kMantleMaxRise = 125.0f;
// Camera roll while wallrunning (TdMove_WallRun camera modifier).
constexpr float kWallrunCameraRoll = 15.0f;

// TdPawn.SpeedCurve_LightWeapon: seconds of sprinting -> speed (uu/s), CIM_Linear keys.
const std::vector<std::pair<float, float>> kSpeedCurveLightWeapon = {
    {0.0f, 0.0f}, {0.4f, 400.0f}, {1.0f, 520.0f}, {3.5f, 650.0f}, {7.0f, 720.0f}};

// Fraction of a move of length `move_len` that stops kContactSkin short of the contact.
float safe_fraction(float fraction, float move_len) {
    if (move_len <= 1e-6f) return 0.0f;
    return std::clamp(fraction - kContactSkin / move_len, 0.0f, 1.0f);
}

// ATdPawn builds AccelCurve_LightWeapon from the speed curve on load (called with 10 steps):
// sample the speed curve at keys x 10 + 1 even times to get time-at-speed, then at 11 speeds from 0
// to the top one take the speed gained over the next 0.5 / 10 s. Sprinting straight then follows
// the designer's speed curve exactly.
LinearCurve accel_curve_from(const std::vector<std::pair<float, float>>& speed, int steps) {
    LinearCurve s{speed};
    const float t_last = speed.back().first;
    const float v_last = speed.back().second;
    const int samples = static_cast<int>(speed.size()) * 10;
    LinearCurve inverse;
    for (int k = 0; k <= samples; ++k) {
        const float t = static_cast<float>(k) * t_last / static_cast<float>(samples);
        inverse.points.emplace_back(s.eval(t), t);
    }
    const float dt = 0.5f / static_cast<float>(steps);
    LinearCurve out;
    for (int i = 0; i <= steps; ++i) {
        const float v = static_cast<float>(i) * v_last / static_cast<float>(steps);
        const float t = inverse.eval(v);
        out.points.emplace_back(v, (s.eval(t + dt) - s.eval(t)) / dt);
    }
    return out;
}

// The game rounds requested accelerations to 0.1 uu/s^2 (floor(x * 10 + 0.5) / 10).
Vec3 round_accel(const Vec3& a) {
    auto r = [](float x) { return std::floor(x * 10.0f + 0.5f) / 10.0f; };
    return Vec3(r(a.x), r(a.y), r(a.z));
}

Vec3 horiz(const Vec3& v) { return Vec3(v.x, v.y, 0.0f); }

float wrap_deg(float a) {
    while (a < -180.0f) a += 360.0f;
    while (a >= 180.0f) a -= 360.0f;
    return a;
}

float yaw_of(const Vec3& v) { return std::atan2(v.y, v.x) * RAD2DEG; }

float sign_of(float v) { return v < 0.0f ? -1.0f : 1.0f; }

bool is_air_move(EMovement m) {
    switch (m) {
        case EMovement::MOVE_Jump:
        case EMovement::MOVE_Falling:
        case EMovement::MOVE_Coil:
        case EMovement::MOVE_WallRunJump:
        case EMovement::MOVE_GrabJump:
        case EMovement::MOVE_WallClimb180TurnJump:
        case EMovement::MOVE_DodgeJump:
        case EMovement::MOVE_180TurnInAir:
        case EMovement::MOVE_MeleeAir:
        case EMovement::MOVE_MeleeWallrun:
            return true;
        default:
            return false;
    }
}

// Moves the coil may start from (TdMove_Coil.CanDoMove): jumps, not plain falls or dodges.
bool coil_allowed_from(EMovement m) {
    switch (m) {
        case EMovement::MOVE_Jump:
        case EMovement::MOVE_WallRunJump:
        case EMovement::MOVE_GrabJump:
        case EMovement::MOVE_WallClimb180TurnJump:
        case EMovement::MOVE_SpringBoarding:
            return true;
        default:
            return false;
    }
}
}  // namespace

float LinearCurve::eval(float x) const {
    const auto& p = points;
    if (p.empty()) return 0.0f;
    if (p.size() == 1 || x <= p.front().first) return p.front().second;
    if (x >= p.back().first) return p.back().second;
    for (size_t i = 0; i + 1 < p.size(); ++i) {
        if (x < p[i + 1].first) {
            const float d = p[i + 1].first - p[i].first;
            if (d <= 0.0f) return p[i].second;
            return p[i].second + (p[i + 1].second - p[i].second) * (x - p[i].first) / d;
        }
    }
    return p.back().second;
}

ParkourController::ParkourController(const MovementConfig& config)
    : m_config(config), m_accel_curve(accel_curve_from(kSpeedCurveLightWeapon, 10)) {
    reset(Vec3(0.0f, 0.0f, 100.0f), 0.0f);
}

void ParkourController::reset(const Vec3& spawn_pos, float spawn_yaw) {
    m_telemetry.tick = 0;
    m_telemetry.sim_time = 0.0f;
    m_telemetry.position = spawn_pos;
    m_telemetry.velocity = Vec3(0.0f, 0.0f, 0.0f);
    m_telemetry.speed_2d = 0.0f;
    m_telemetry.speed_3d = 0.0f;
    m_telemetry.yaw_deg = spawn_yaw;
    m_telemetry.pitch_deg = 0.0f;
    m_telemetry.camera_roll_deg = 0.0f;
    m_telemetry.fov_deg = 90.0f;
    m_telemetry.eye_height = kEyeHeightStand;
    m_telemetry.health = 100.0f;
    m_telemetry.reaction_energy = 100.0f;
    m_telemetry.reaction_active = false;
    m_telemetry.grounded = true;
    m_telemetry.move_state = EMovement::MOVE_Walking;
    m_telemetry.wall_normal = Vec3(0.0f, 0.0f, 0.0f);
    m_telemetry.active_checkpoint = 0;
    m_telemetry.active_checkpoint_name = "";
    m_telemetry.bags_collected = 0;
    m_telemetry.in_elevator = false;
    m_telemetry.active_elevator_idx = -1;
    m_telemetry.elevator_progress = 0.0f;
    m_telemetry.weapon = WeaponState{};
    m_telemetry.combat_anim_time = 0.0f;
    m_telemetry.combat_anim_duration = 0.45f;
    m_telemetry.melee_variant = 0;
    m_telemetry.snatch_from_back = false;
    m_telemetry.melee_hit_confirmed = false;
    m_telemetry.disarm_prompt_visible = false;
    m_telemetry.hit_marker_timer = 0.0f;
    m_telemetry.damage_flash_timer = 0.0f;
    m_telemetry.active_subtitle = "";

    m_sprint_energy = 0.0f;
    m_accel_time = 0.0f;
    m_stop_timer = 0.0f;
    m_frame_turn_uu = 0.0f;
    m_frame_dt = 1.0f / 60.0f;

    m_prev_jump = false;
    m_prev_crouch = false;
    m_prev_turn_180 = false;
    m_crouch_pressed = false;
    m_jump_buffer = 0.0f;
    m_roll_trigger_time = -100.0f;

    m_state_timer = 0.0f;
    m_wallrun_cooldown = 0.0f;
    m_wallrun_begin_speed = 0.0f;
    m_slide_timer = 0.0f;
    m_slide_yaw = spawn_yaw;
    m_coil_timer = 0.0f;
    m_turn_timer = 0.0f;
    m_turn_total = 0.0f;
    m_turn_target_yaw = spawn_yaw;
    m_landing_timer = 0.0f;
    m_damage_cooldown = 0.0f;
    m_air_fall_start_z = spawn_pos.z;
    m_fall_peak_z = spawn_pos.z;
    m_melee_cooldown = 0.0f;
    m_melee_combo_index = 0;
    m_melee_combo_reset_timer = 0.0f;
    m_weapon_cycle_index = -1;
    m_jump_consumed = false;

    m_last_jump_location = spawn_pos;
    m_pre_jump_momentum = 0.0f;
    m_takeoff_move = EMovement::MOVE_Falling;
    m_turned_in_air = false;
    m_consecutive_wallruns = 0;
    m_wall_turned = false;
    m_illegal_wall_timer = 0.0f;

    m_wall_tangent = Vec3(0.0f, 0.0f, 0.0f);
    m_last_wallrun_normal = Vec3(0.0f, 0.0f, 0.0f);
    m_into_wallclimb_speed = 0.0f;
    m_wallclimb_reached = false;
    m_ledge_z = 0.0f;
    m_hang_time = 0.0f;
    m_base_actor = -1;

    m_last_checkpoint_pos = spawn_pos;
    m_last_checkpoint_yaw = spawn_yaw;
}

void ParkourController::equip_weapon(const std::string& weapon_name) {
    struct WeaponSpec {
        const char* id;
        const char* display;
        EWeaponFireMode fire_mode;
        int max_ammo;
        float fire_interval;
        float damage_near;
        float damage_far;
        float falloff_start;
        float falloff_end;
        float max_range;
        int pellet_count;
        float spread_rad;
        float recoil_pitch;
        bool is_heavy;
        bool is_two_handed;
        float mobility_scale;
    };

    // Reverse-engineered from TdGame/Config/DefaultWeapons.ini & TdSharedContent.u
    static const WeaponSpec kWeaponSpecs[] = {
        {"Colt1911",     "M1911 .45 Pistol",        EWeaponFireMode::SemiAuto,   8,   0.22f, 45.0f, 25.0f, 1200.0f, 3000.0f, 10000.0f, 1,  0.018f, 1.8f, false, false, 0.92f},
        {"Glock18",      "G18C Machine Pistol",     EWeaponFireMode::FullAuto,   19,  0.08f, 22.0f, 10.0f,  800.0f, 2200.0f,  8000.0f, 1,  0.045f, 1.0f, false, false, 0.92f},
        {"BerettaM93R",  "93R Burst Pistol",        EWeaponFireMode::Burst3,     20,  0.085f,30.0f, 15.0f, 1000.0f, 2500.0f,  9000.0f, 1,  0.028f, 1.1f, false, false, 0.92f},
        {"SteyrTMP",     "Steyr TMP Tactical SMG",  EWeaponFireMode::FullAuto,   30,  0.066f,20.0f,  9.0f,  800.0f, 2200.0f,  8000.0f, 1,  0.048f, 0.85f,false, false, 0.90f},
        {"MP5K",         "HK MP5K Submachine Gun",  EWeaponFireMode::FullAuto,   30,  0.075f,26.0f, 13.0f, 1000.0f, 2800.0f,  9000.0f, 1,  0.035f, 1.0f, false, true,  0.86f},
        {"G36C",         "HK G36C Assault Rifle",   EWeaponFireMode::FullAuto,   30,  0.08f, 36.0f, 18.0f, 1800.0f, 4500.0f, 12000.0f, 1,  0.024f, 1.3f, true,  true,  0.72f},
        {"FNSCARL",      "FN SCAR-L Carbine",       EWeaponFireMode::FullAuto,   20,  0.096f,42.0f, 24.0f, 2000.0f, 5000.0f, 13000.0f, 1,  0.020f, 1.55f,true,  true,  0.70f},
        {"Remington870", "Remington 870 Shotgun",   EWeaponFireMode::PumpAction, 7,   0.82f, 18.0f,  4.0f,  400.0f, 1500.0f,  3000.0f, 10, 0.092f, 4.0f, true,  true,  0.72f},
        {"Neostead",     "Neostead 2000 Shotgun",   EWeaponFireMode::PumpAction, 12,  0.62f, 16.0f,  4.0f,  450.0f, 1600.0f,  3200.0f, 10, 0.082f, 3.4f, true,  true,  0.72f},
        {"FNMinimi",     "M249 SAW Squad LMG",      EWeaponFireMode::FullAuto,   100, 0.075f,38.0f, 20.0f, 2000.0f, 5000.0f, 13000.0f, 1,  0.046f, 1.4f, true,  true,  0.62f},
        {"M95",          "Barrett M95 .50 BMG",     EWeaponFireMode::BoltAction, 5,   1.25f, 160.0f,120.0f,4000.0f, 9000.0f, 15000.0f, 1,  0.004f, 6.0f, true,  true,  0.58f}
    };

    const WeaponSpec* matched = &kWeaponSpecs[0];
    int matched_idx = 0;
    for (int i = 0; i < static_cast<int>(sizeof(kWeaponSpecs) / sizeof(kWeaponSpecs[0])); ++i) {
        if (weapon_name.find(kWeaponSpecs[i].id) != std::string::npos) {
            matched = &kWeaponSpecs[i];
            matched_idx = i;
            break;
        }
    }
    // Additional substring aliases
    if (weapon_name.find("Glock") != std::string::npos) { matched = &kWeaponSpecs[1]; matched_idx = 1; }
    else if (weapon_name.find("Beretta") != std::string::npos || weapon_name.find("93R") != std::string::npos) { matched = &kWeaponSpecs[2]; matched_idx = 2; }
    else if (weapon_name.find("TMP") != std::string::npos || weapon_name.find("Steyr") != std::string::npos) { matched = &kWeaponSpecs[3]; matched_idx = 3; }
    else if (weapon_name.find("SCAR") != std::string::npos) { matched = &kWeaponSpecs[6]; matched_idx = 6; }
    else if (weapon_name.find("Remington") != std::string::npos || weapon_name.find("870") != std::string::npos) { matched = &kWeaponSpecs[7]; matched_idx = 7; }
    else if (weapon_name.find("Minimi") != std::string::npos || weapon_name.find("M249") != std::string::npos) { matched = &kWeaponSpecs[9]; matched_idx = 9; }
    else if (weapon_name.find("Barret") != std::string::npos || weapon_name.find("M95") != std::string::npos) { matched = &kWeaponSpecs[10]; matched_idx = 10; }

    m_weapon_cycle_index = matched_idx;

    WeaponState ws{};
    ws.equipped = true;
    ws.name = matched->id;
    ws.display_name = matched->display;
    ws.fire_mode = matched->fire_mode;
    ws.ammo = matched->max_ammo;
    ws.max_ammo = matched->max_ammo;
    ws.cooldown = 0.15f;
    ws.fire_interval = matched->fire_interval;
    ws.range = matched->max_range;
    ws.falloff_start = matched->falloff_start;
    ws.falloff_end = matched->falloff_end;
    ws.damage = matched->damage_near;
    ws.damage_far = matched->damage_far;
    ws.pellet_count = matched->pellet_count;
    ws.spread_rad = matched->spread_rad;
    ws.recoil_pitch_deg = matched->recoil_pitch;
    ws.is_heavy = matched->is_heavy;
    ws.is_two_handed = matched->is_two_handed;
    ws.mobility_scale = matched->mobility_scale;
    ws.equip_timer = 0.35f; // trigger 1P unholster animation
    m_telemetry.weapon = ws;
}

void ParkourController::step(const InputFrame& input, float dt, LevelScene& scene) {
    if (dt <= 0.0f) return;

    // Input edges. TdPlayerInput: Jump and Crouch are press actions (holding a key never
    // retriggers a move); a jump press is buffered for JumpTapTime.
    const bool jump_press = input.jump && !m_prev_jump;
    m_crouch_pressed = input.crouch && !m_prev_crouch;
    m_prev_jump = input.jump;
    m_prev_crouch = input.crouch;
    m_jump_consumed = input.jump && !jump_press;
    if (jump_press) m_jump_buffer = m_config.jump_tap_time;
    // TdPawn.Tick: a crouch press arms the skill roll (RollTriggerTime) unless a press armed it
    // within the last 0.6 s (so holding / spamming crouch does not keep it armed).
    if (m_crouch_pressed && m_telemetry.sim_time - m_roll_trigger_time >= m_config.roll_trigger_rearm) {
        m_roll_trigger_time = m_telemetry.sim_time;
    }
    // PlayerInput.aTurn this frame, in rotation units (65536 per turn): sprint turn damping.
    m_frame_turn_uu = std::abs(input.look_yaw_delta) * (65536.0f / 360.0f);

    // 1. Reaction Time Slow-Motion
    float effective_dt = dt;
    update_reaction_time(input, dt, effective_dt);
    m_frame_dt = effective_dt;

    // 2. Camera, Rotation & Look-At Hint
    update_camera_and_inputs(input, effective_dt, scene);

    // 3. Substepped Kinematic Physics (fixed ~120Hz substepping for precision)
    constexpr float MAX_SUBSTEP = 1.0f / 120.0f;
    float remaining_time = effective_dt;

    while (remaining_time > 1e-6f) {
        float step_dt = std::min(remaining_time, MAX_SUBSTEP);
        remaining_time -= step_dt;

        m_state_timer += step_dt;
        m_telemetry.combat_anim_time = m_state_timer;
        m_wallrun_cooldown = std::max(0.0f, m_wallrun_cooldown - step_dt);
        if (m_illegal_wall_timer > 0.0f) {
            m_illegal_wall_timer = std::max(0.0f, m_illegal_wall_timer - step_dt);
            if (m_illegal_wall_timer <= 0.0f) m_last_wallrun_normal = Vec3(0.0f, 0.0f, 0.0f);
        }

        // Step interactive elevators (InterpActor + InterpTrackMove) and carry player with cab_delta
        update_elevators(input, step_dt, scene);

        // State Machine Dispatch
        switch (m_telemetry.move_state) {
            case EMovement::MOVE_Walking:
            case EMovement::MOVE_Crouch:
            case EMovement::MOVE_StepUp:
            case EMovement::MOVE_AutoStepUp:
            case EMovement::MOVE_SoftLanding:
            case EMovement::MOVE_180Turn:
                update_ground_locomotion(input, step_dt, scene);
                break;

            case EMovement::MOVE_Jump:
            case EMovement::MOVE_Falling:
            case EMovement::MOVE_Coil:
            case EMovement::MOVE_WallRunJump:
            case EMovement::MOVE_GrabJump:
            case EMovement::MOVE_WallClimb180TurnJump:
            case EMovement::MOVE_DodgeJump:
            case EMovement::MOVE_180TurnInAir:
                update_air_locomotion(input, step_dt, scene);
                break;

            case EMovement::MOVE_MeleeAir:
            case EMovement::MOVE_MeleeWallrun: {
                EMovement combat_air_state = m_telemetry.move_state;
                float saved_timer = m_state_timer;
                update_air_locomotion(input, step_dt, scene);
                if (m_telemetry.move_state == EMovement::MOVE_Falling ||
                    m_telemetry.move_state == EMovement::MOVE_Jump) {
                    if (saved_timer < m_telemetry.combat_anim_duration) {
                        m_telemetry.move_state = combat_air_state;
                        m_state_timer = saved_timer;
                        m_telemetry.combat_anim_time = saved_timer;
                    }
                }
                break;
            }

            case EMovement::MOVE_Melee:
            case EMovement::MOVE_Barge: {
                EMovement combat_gnd_state = m_telemetry.move_state;
                float saved_timer = m_state_timer;
                update_ground_locomotion(input, step_dt, scene);
                if (m_telemetry.move_state == EMovement::MOVE_Walking ||
                    m_telemetry.move_state == EMovement::MOVE_Crouch ||
                    m_telemetry.move_state == EMovement::MOVE_AutoStepUp) {
                    if (saved_timer < m_telemetry.combat_anim_duration) {
                        m_telemetry.move_state = combat_gnd_state;
                        m_state_timer = saved_timer;
                        m_telemetry.combat_anim_time = saved_timer;
                    }
                }
                break;
            }

            case EMovement::MOVE_Snatch: {
                // Smoothly damp velocity during weapon disarm animation (SnatchFwd / SnatchBack)
                m_telemetry.velocity.x *= std::max(0.0f, 1.0f - 10.0f * step_dt);
                m_telemetry.velocity.y *= std::max(0.0f, 1.0f - 10.0f * step_dt);
                if (m_state_timer >= m_telemetry.combat_anim_duration) {
                    m_telemetry.move_state = m_telemetry.grounded ? EMovement::MOVE_Walking : EMovement::MOVE_Falling;
                }
                break;
            }

            case EMovement::MOVE_WallRunningLeft:
            case EMovement::MOVE_WallRunningRight:
                update_wallrun(input, step_dt, scene);
                break;

            case EMovement::MOVE_WallClimbing:
                update_wallclimb(input, step_dt, scene);
                break;

            case EMovement::MOVE_Slide:
            case EMovement::MOVE_MeleeSlide:
                update_slide(input, step_dt, scene);
                break;

            case EMovement::MOVE_Grabbing:
            case EMovement::MOVE_GrabPullUp:
            case EMovement::MOVE_IntoGrab:
                update_ledge_grab(input, step_dt, scene);
                break;

            case EMovement::MOVE_ZipLine:
                update_zipline(input, step_dt, scene);
                break;

            case EMovement::MOVE_Swing:
                update_swing_bar(input, step_dt, scene);
                break;

            case EMovement::MOVE_Balance:
                update_balance(input, step_dt, scene);
                break;

            case EMovement::MOVE_SkillRoll:
            case EMovement::MOVE_Landing:
            case EMovement::MOVE_LayOnGround:
                update_landing_moves(input, step_dt, scene);
                break;

            case EMovement::MOVE_SpeedVaulting:
            case EMovement::MOVE_VaultOver:
            case EMovement::MOVE_SpringBoarding:
                update_vault(input, step_dt, scene);
                break;

            default:
                m_telemetry.move_state = m_telemetry.grounded ? EMovement::MOVE_Walking : EMovement::MOVE_Falling;
                break;
        }

        // UE3 floor check: a walking pawn follows floors up to MaxStepHeight + MAXFLOORDIST below its
        // feet (stairs, kerbs, seams between meshes); an airborne pawn lands only on floors it touches.
        const EMovement state = m_telemetry.move_state;
        const bool low_profile = (state == EMovement::MOVE_Crouch || state == EMovement::MOVE_Slide ||
                                  state == EMovement::MOVE_MeleeSlide || state == EMovement::MOVE_SkillRoll ||
                                  state == EMovement::MOVE_Coil);
        const bool attached = (state == EMovement::MOVE_Grabbing || state == EMovement::MOVE_GrabPullUp ||
                               state == EMovement::MOVE_IntoGrab || state == EMovement::MOVE_ZipLine ||
                               state == EMovement::MOVE_Swing || state == EMovement::MOVE_WallClimbing ||
                               state == EMovement::MOVE_SpeedVaulting || state == EMovement::MOVE_VaultOver ||
                               state == EMovement::MOVE_SpringBoarding);
        const bool wallrunning = (state == EMovement::MOVE_WallRunningLeft || state == EMovement::MOVE_WallRunningRight);
        FloorHit floor;
        const float probe_depth = m_telemetry.grounded ? (kMaxStepHeight + kMaxFloorDist) : kMaxFloorDist;
        const bool has_floor = !attached &&
                               check_ground(scene, probe_depth, low_profile ? kCrouchHeight : kPawnHeight, floor);

        if (has_floor && m_telemetry.velocity.z <= 50.0f) {
            if (!m_telemetry.grounded) {
                land(floor, scene);
            }
            m_telemetry.position.z = floor.z;
            m_telemetry.grounded = true;
            m_base_actor = floor.actor_index;
            if (m_telemetry.velocity.z < 0.0f) {
                m_telemetry.velocity.z = 0.0f;
            }
        } else if (!attached && !wallrunning) {
            m_base_actor = -1;
            if (m_telemetry.grounded) {
                // Walked / slid / rolled off an edge (TdMove_Falling.StartMove).
                const EMovement from = m_telemetry.move_state;
                const Vec3 fwd = facing_forward();
                if (from == EMovement::MOVE_Slide || from == EMovement::MOVE_MeleeSlide) {
                    // TdMove_Slide.StopMove halves the velocity.
                    m_telemetry.velocity.x *= 0.5f;
                    m_telemetry.velocity.y *= 0.5f;
                } else if (from == EMovement::MOVE_Walking && m_telemetry.velocity.dot(fwd) < 0.0f) {
                    // Backing off a drop of two body heights or more: the horizontal velocity is
                    // zeroed so the pawn drops straight down.
                    const Vec3 s = m_telemetry.position;
                    const TraceHit drop = trace_ray(s, s - Vec3(0.0f, 0.0f, 2.0f * kPawnHeight), scene);
                    if (!drop.hit) {
                        m_telemetry.velocity.x = 0.0f;
                        m_telemetry.velocity.y = 0.0f;
                    }
                }
                if (!is_air_move(from)) {
                    leave_ground(EMovement::MOVE_Falling);
                    set_stance(kEyeHeightStand);
                } else {
                    m_telemetry.grounded = false;
                }
            }
            if (m_telemetry.position.z > m_fall_peak_z) {
                m_fall_peak_z = m_telemetry.position.z;
            }
        } else {
            m_base_actor = -1;
        }

        m_jump_buffer = std::max(0.0f, m_jump_buffer - step_dt);
    }

    // 4. Combat, Weapons & Disarms
    update_combat_and_weapons(input, effective_dt, scene);

    // 5. AI Bots Simulation
    update_ai_bots(effective_dt, scene);

    // 6. Health Regeneration
    update_health_and_regen(effective_dt);

    // 7. Checkpoints, Kill Volumes & Collectibles
    update_checkpoints_and_volumes(scene);

    // 8. Final Telemetry Update
    m_prev_turn_180 = input.turn_180;
    m_telemetry.tick++;
    m_telemetry.sim_time += effective_dt;
    m_telemetry.speed_2d = m_telemetry.velocity.length_xy();
    m_telemetry.speed_3d = m_telemetry.velocity.length();

    // Dynamic FOV scaling: 90° horizontal at base (BaseEngine.ini FOVAngle=90), up to 98° at full sprint
    float speed_ratio = std::clamp((m_telemetry.speed_2d - m_config.run_speed) / (m_config.sprint_speed - m_config.run_speed), 0.0f, 1.0f);
    m_telemetry.fov_deg = 90.0f + 8.0f * speed_ratio;
}

// -----------------------------------------------------------------------------
// Reaction Time Slow-Motion Subsystem
// -----------------------------------------------------------------------------
void ParkourController::update_reaction_time(const InputFrame& input, float dt, float& effective_dt) {
    if (input.reaction_time && m_telemetry.reaction_energy > 5.0f) {
        m_telemetry.reaction_active = true;
    } else if (m_telemetry.reaction_energy <= 0.0f || !input.reaction_time) {
        m_telemetry.reaction_active = false;
    }

    if (m_telemetry.reaction_active) {
        effective_dt = dt * m_config.reaction_time_dilation;
        m_telemetry.reaction_energy = std::max(0.0f, m_telemetry.reaction_energy - m_config.reaction_time_drain * dt);
    } else {
        // Slowly recharge energy when running and chaining moves
        if (m_telemetry.speed_2d > m_config.jog_speed) {
            m_telemetry.reaction_energy = std::min(100.0f, m_telemetry.reaction_energy + 4.0f * dt);
        }
    }
}

// -----------------------------------------------------------------------------
// Camera, Orientation & Look-At Objective Hint Subsystem
// -----------------------------------------------------------------------------
void ParkourController::update_camera_and_inputs(const InputFrame& input, float dt, const LevelScene& scene) {
    // Q: 180° turn (TdMove_180Turn on the ground from Walking only; TdMove_180TurnInAir in the air
    // when moving the way you face). Both are root-rotation moves: the view swings TurnTime.
    if (input.turn_180 && !m_prev_turn_180 && m_turn_timer <= 0.0f) {
        const EMovement st = m_telemetry.move_state;
        bool start = false;
        if (st == EMovement::MOVE_Walking || st == EMovement::MOVE_AutoStepUp || st == EMovement::MOVE_SoftLanding) {
            m_telemetry.move_state = EMovement::MOVE_180Turn;
            m_state_timer = 0.0f;
            start = true;
        } else if (st == EMovement::MOVE_Jump || st == EMovement::MOVE_Falling || st == EMovement::MOVE_Coil ||
                   st == EMovement::MOVE_WallRunJump || st == EMovement::MOVE_GrabJump ||
                   st == EMovement::MOVE_WallClimb180TurnJump) {
            const Vec3 h = horiz(m_telemetry.velocity);
            if (h.length() > 1.0f && facing_forward().dot(h.normalized()) >= 0.2f) {
                m_telemetry.move_state = EMovement::MOVE_180TurnInAir;
                m_turned_in_air = true;
                m_coil_timer = 0.0f;
                start = true;
            }
        }
        if (start) {
            m_turn_timer = m_config.turn_180_time;
            m_turn_total = m_config.turn_180_time;
            m_turn_target_yaw = m_telemetry.yaw_deg + 180.0f;
        }
    }

    if (m_turn_timer > 0.0f) {
        const float step = (wrap_deg(m_turn_target_yaw - m_telemetry.yaw_deg) /
                            std::max(m_turn_timer, 1e-4f)) * std::min(dt, m_turn_timer);
        m_telemetry.yaw_deg += step;
        m_turn_timer -= dt;
        if (m_turn_timer <= 0.0f) {
            m_turn_timer = 0.0f;
            m_telemetry.yaw_deg = m_turn_target_yaw;
        }
        m_telemetry.pitch_deg = std::clamp(m_telemetry.pitch_deg + input.look_pitch_delta, -85.0f, 85.0f);
    } else {
        m_telemetry.yaw_deg += input.look_yaw_delta;
        m_telemetry.pitch_deg = std::clamp(m_telemetry.pitch_deg + input.look_pitch_delta, -85.0f, 85.0f);
    }

    // Keep yaw normalized in [0, 360)
    while (m_telemetry.yaw_deg < 0.0f) m_telemetry.yaw_deg += 360.0f;
    while (m_telemetry.yaw_deg >= 360.0f) m_telemetry.yaw_deg -= 360.0f;

    // Look-At Route Hint: Smoothly swivels view towards next checkpoint / runner objective
    if (input.look_at) {
        Vec3 target_pos = m_telemetry.position + Rotator::from_degrees(0.0f, m_telemetry.yaw_deg, 0.0f).forward() * 1000.0f;

        if (m_telemetry.active_checkpoint < static_cast<int>(scene.checkpoints.size())) {
            target_pos = scene.checkpoints[m_telemetry.active_checkpoint];
        } else {
            // Find nearest runner vision actor
            float closest_d = 1e9f;
            for (const auto& act : scene.actors) {
                if (act.is_runner_vision || act.is_checkpoint) {
                    float d = m_telemetry.position.distance(act.location);
                    if (d < closest_d) {
                        closest_d = d;
                        target_pos = act.location;
                    }
                }
            }
        }

        Vec3 diff = target_pos - (m_telemetry.position + Vec3(0, 0, m_telemetry.eye_height));
        float dist_xy = diff.length_xy();
        if (dist_xy > 10.0f) {
            float desired_yaw = std::atan2(diff.y, diff.x) * RAD2DEG;
            float desired_pitch = std::atan2(diff.z, dist_xy) * RAD2DEG;

            // Smooth interpolation
            float yaw_diff = desired_yaw - m_telemetry.yaw_deg;
            while (yaw_diff < -180.0f) yaw_diff += 360.0f;
            while (yaw_diff > 180.0f) yaw_diff -= 360.0f;

            m_telemetry.yaw_deg += yaw_diff * std::min(1.0f, 8.0f * dt);
            m_telemetry.pitch_deg += (desired_pitch - m_telemetry.pitch_deg) * std::min(1.0f, 8.0f * dt);
        }
    }

    // Camera roll interpolation: smoothly relax roll unless in wallrun
    if (m_telemetry.move_state != EMovement::MOVE_WallRunningLeft &&
        m_telemetry.move_state != EMovement::MOVE_WallRunningRight) {
        m_telemetry.camera_roll_deg += (0.0f - m_telemetry.camera_roll_deg) * std::min(1.0f, 10.0f * dt);
    }
}

// -----------------------------------------------------------------------------
// Swept Collision & Tracing Subsystem (real UE3 level collision)
// -----------------------------------------------------------------------------
// The pawn box is swept against LevelScene::collision (StaticMesh BodySetup hulls / kDOP triangles,
// BlockingVolume brushes, BSP) and against every moving elevator part, whose collision is stored at
// the part's initial pose and is therefore queried shifted by -offset.
ParkourController::TraceHit ParkourController::sweep_capsule(const Capsule& capsule, const Vec3& delta, const LevelScene& scene) const {
    TraceHit best;
    const float half_z = std::max(1.0f, 0.5f * (capsule.height - capsule.bottom_offset));
    const Vec3 extent(capsule.radius, capsule.radius, half_z);
    const Vec3 centre = capsule.base + Vec3(0.0f, 0.0f, capsule.bottom_offset + half_z);

    auto consider = [&](const CollisionHit& h, const Vec3& shift) {
        if (!h.hit) return;
        // Earliest contact wins; ties prefer the most floor-like normal (as CollisionWorld does).
        if (best.hit && (h.time > best.fraction || (h.time == best.fraction && h.normal.z <= best.normal.z))) return;
        best.hit = true;
        best.start_penetrating = h.start_penetrating;
        best.fraction = h.time;
        best.normal = h.normal;
        best.point = h.location + shift;
        best.actor_index = h.actor;
        best.actor = (h.actor >= 0 && static_cast<size_t>(h.actor) < scene.actors.size()) ? &scene.actors[h.actor] : nullptr;
    };

    if (scene.collision) {
        consider(scene.collision->sweep_box(centre, delta, extent, COLL_BlockNonZeroExtent), Vec3(0.0f, 0.0f, 0.0f));
    }
    for (const auto& elev : scene.elevators) {
        for (const auto& part : elev.parts) {
            if (!part.collision) continue;
            consider(part.collision->sweep_box(centre - part.offset, delta, extent, COLL_BlockNonZeroExtent), part.offset);
        }
    }
    return best;
}

ParkourController::TraceHit ParkourController::trace_ray(const Vec3& start, const Vec3& end, const LevelScene& scene,
                                                         uint8_t channels) const {
    TraceHit best;
    if ((end - start).length_sq() < 1e-8f) return best;

    auto consider = [&](const CollisionHit& h, const Vec3& shift) {
        if (!h.hit || (best.hit && h.time >= best.fraction)) return;
        best.hit = true;
        best.start_penetrating = h.start_penetrating;
        best.fraction = h.time;
        best.normal = h.normal;
        best.point = h.location + shift;
        best.actor_index = h.actor;
        best.actor = (h.actor >= 0 && static_cast<size_t>(h.actor) < scene.actors.size()) ? &scene.actors[h.actor] : nullptr;
    };

    if (scene.collision) consider(scene.collision->line_check(start, end, channels), Vec3(0.0f, 0.0f, 0.0f));
    for (const auto& elev : scene.elevators) {
        for (const auto& part : elev.parts) {
            if (!part.collision) continue;
            consider(part.collision->line_check(start - part.offset, end - part.offset, channels), part.offset);
        }
    }
    return best;
}

bool ParkourController::check_ground(const LevelScene& scene, float probe_depth, float height, FloorHit& out) const {
    Capsule cap;
    cap.base = m_telemetry.position + Vec3(0.0f, 0.0f, kFloorProbeLift);
    cap.radius = kPawnRadius;
    cap.height = height;
    const Vec3 delta(0.0f, 0.0f, -(kFloorProbeLift + probe_depth));
    const TraceHit hit = sweep_capsule(cap, delta, scene);
    if (!hit.hit || hit.normal.z < kWalkableFloorZ) return false;
    out.z = cap.base.z + delta.z * hit.fraction;
    out.normal = hit.normal;
    out.actor_index = hit.actor_index;
    return true;
}

bool ParkourController::has_room(float height, const LevelScene& scene) const {
    return has_room_at(m_telemetry.position, height, scene);
}

bool ParkourController::has_room_at(const Vec3& feet, float height, const LevelScene& scene) const {
    // Lifted off the floor it rests on and shrunk slightly so resting / sliding contacts do not count.
    constexpr float kLift = 1.0f;
    const float half_z = 0.5f * (height - kLift);
    const Vec3 extent(kPawnRadius - 1.0f, kPawnRadius - 1.0f, half_z);
    const Vec3 centre = feet + Vec3(0.0f, 0.0f, kLift + half_z);
    return box_free(centre, extent, scene);
}

bool ParkourController::box_free(const Vec3& centre, const Vec3& extent, const LevelScene& scene) const {
    if (scene.collision && scene.collision->overlap_box(centre, extent, COLL_BlockNonZeroExtent)) return false;
    for (const auto& elev : scene.elevators) {
        for (const auto& part : elev.parts) {
            if (part.collision && part.collision->overlap_box(centre - part.offset, extent, COLL_BlockNonZeroExtent)) {
                return false;
            }
        }
    }
    return true;
}

ParkourController::TraceHit ParkourController::sweep_box(const Vec3& centre, const Vec3& extent, const Vec3& delta,
                                                         const LevelScene& scene) const {
    TraceHit best;
    auto consider = [&](const CollisionHit& h, const Vec3& shift) {
        if (!h.hit) return;
        if (best.hit && (h.time > best.fraction || (h.time == best.fraction && h.normal.z <= best.normal.z))) return;
        best.hit = true;
        best.start_penetrating = h.start_penetrating;
        best.fraction = h.time;
        best.normal = h.normal;
        best.point = h.location + shift;
        best.actor_index = h.actor;
        best.actor = (h.actor >= 0 && static_cast<size_t>(h.actor) < scene.actors.size()) ? &scene.actors[h.actor] : nullptr;
    };
    if (scene.collision) consider(scene.collision->sweep_box(centre, delta, extent, COLL_BlockNonZeroExtent), Vec3(0.0f, 0.0f, 0.0f));
    for (const auto& elev : scene.elevators) {
        for (const auto& part : elev.parts) {
            if (!part.collision) continue;
            consider(part.collision->sweep_box(centre - part.offset, delta, extent, COLL_BlockNonZeroExtent), part.offset);
        }
    }
    return best;
}


float ParkourController::headroom(float max_rise, const LevelScene& scene) const {
    if (max_rise <= 0.0f) return 0.0f;
    Capsule cap;
    cap.base = m_telemetry.position;
    cap.radius = kPawnRadius;
    cap.height = kPawnHeight;
    const TraceHit hit = sweep_capsule(cap, Vec3(0.0f, 0.0f, max_rise), scene);
    if (!hit.hit) return max_rise;
    return std::max(0.0f, max_rise * hit.fraction - kContactSkin);
}

// The game's wall checks trace with the pawn's extent at one height: a thin slab of the footprint
// swept along `dir` for `reach` beyond the pawn centre. A face is reported with its outward
// (horizontal) normal and its distance from the pawn centre.
ParkourController::WallFace ParkourController::probe_wall(const Vec3& dir_in, float reach, float height,
                                                          const LevelScene& scene) const {
    WallFace out;
    Vec3 dir = horiz(dir_in);
    if (dir.length_sq() < 1e-6f || reach <= 0.0f) return out;
    dir = dir.normalized();

    Capsule cap;
    cap.base = m_telemetry.position;
    cap.radius = kPawnRadius;
    cap.height = height + 1.0f;
    cap.bottom_offset = height - 1.0f;
    const TraceHit hit = sweep_capsule(cap, dir * reach, scene);
    if (!hit.hit || std::abs(hit.normal.z) > 0.3f) return out;
    Vec3 n = horiz(hit.normal);
    if (n.length_sq() < 1e-6f) return out;
    n = n.normalized();
    if (n.dot(dir) > -0.05f) return out;  // a face we are moving along / away from, not into

    const Vec3 centre = m_telemetry.position + Vec3(0.0f, 0.0f, height);
    const float support = kPawnRadius * (std::abs(n.x) + std::abs(n.y));
    out.found = true;
    out.normal = n;
    out.point = hit.point - n * support;  // a point on the face plane
    out.distance = std::max(0.0f, (out.point - centre).dot(-n));
    return out;
}

// A ledge ahead: the wall face whose walkable top lies between min_rise and max_rise above the
// feet (TdPawn.FindLedge: a forward trace for the wall, a downward trace just beyond it for the top).
ParkourController::Ledge ParkourController::find_ledge(const Vec3& dir_in, float reach, float min_rise, float max_rise,
                                                       const LevelScene& scene) const {
    Ledge out;
    Vec3 dir = horiz(dir_in);
    if (dir.length_sq() < 1e-6f || max_rise <= min_rise) return out;
    dir = dir.normalized();

    // The lowest probe that finds a face between the two heights (so a low rail counts as well as
    // a chest-high wall).
    WallFace wall;
    constexpr int kSamples = 4;
    for (int i = 0; i < kSamples && !wall.found; ++i) {
        const float h = min_rise + (max_rise - min_rise) * (static_cast<float>(i) + 0.5f) / kSamples;
        wall = probe_wall(dir, reach, h, scene);
    }
    if (!wall.found) return out;

    const Vec3 into = -wall.normal;
    const Vec3& feet = m_telemetry.position;
    // TdPhysicsMove.HandPlantExtentCheckWidth / Height: the hands need a 10 x 10 x 80 clear box
    // standing on the ledge top, so a thin rail with a panel right behind it is not a ledge.
    constexpr float kHandPlantWidth = 10.0f;
    constexpr float kHandPlantHeight = 80.0f;
    for (float extra : {6.0f, 20.0f, 40.0f}) {
        Vec3 column = wall.point + into * extra;
        column.z = feet.z;
        const TraceHit top = trace_ray(column + Vec3(0.0f, 0.0f, max_rise + 10.0f),
                                       column + Vec3(0.0f, 0.0f, std::max(0.0f, min_rise - 2.0f)), scene);
        if (!top.hit || top.normal.z < kWalkableFloorZ) continue;
        const float rise = top.point.z - feet.z;
        if (rise < min_rise || rise > max_rise) continue;
        const Vec3 hands(column.x, column.y, top.point.z + 1.0f + 0.5f * kHandPlantHeight);
        if (!box_free(hands, Vec3(kHandPlantWidth, kHandPlantWidth, 0.5f * kHandPlantHeight), scene)) continue;
        out.found = true;
        out.normal = wall.normal;
        out.top_z = top.point.z;
        out.wall_distance = wall.distance;
        out.top_point = Vec3(column.x, column.y, top.point.z);
        return out;
    }
    return out;
}

// -----------------------------------------------------------------------------
// Swept Movement (UE3 MoveActor / SlideAlongSurface / APawn::stepUp)
// -----------------------------------------------------------------------------
ParkourController::TraceHit ParkourController::move_swept(const Vec3& delta, float height, float bottom_offset,
                                                          const LevelScene& scene) {
    Capsule cap;
    cap.base = m_telemetry.position;
    cap.radius = kPawnRadius;
    cap.height = height;
    cap.bottom_offset = bottom_offset;
    const TraceHit hit = sweep_capsule(cap, delta, scene);
    m_telemetry.position += delta * (hit.hit ? safe_fraction(hit.fraction, delta.length()) : 1.0f);
    return hit;
}

ParkourController::TraceHit ParkourController::move_and_slide(const Vec3& delta, float height, float bottom_offset,
                                                              const LevelScene& scene) {
    const TraceHit first = move_swept(delta, height, bottom_offset, scene);
    if (!first.hit) return first;
    // The blocked remainder continues along the surface, nudged off it so the next sweep does not
    // re-detect the resting contact.
    Vec3 remaining = delta * (1.0f - first.fraction);
    remaining -= first.normal * remaining.dot(first.normal);
    if (remaining.length_sq() < 1e-6f) return first;
    const TraceHit second = move_swept(remaining + first.normal * 0.01f, height, bottom_offset, scene);
    if (second.hit) {
        // Crease between two surfaces: continue along their intersection line.
        Vec3 crease = first.normal.cross(second.normal);
        if (crease.length_sq() > 1e-6f) {
            crease = crease.normalized();
            const Vec3 rest = crease * (remaining * (1.0f - second.fraction)).dot(crease);
            if (rest.length_sq() > 1e-6f) move_swept(rest, height, bottom_offset, scene);
        }
    }
    return first;
}

// UE3 APawn::physWalking horizontal move: walkable ramps are followed, kerbs / stairs / seams between
// meshes are stepped up (stepUp), walls are slid along.
void ParkourController::walk_move(const Vec3& delta, float height, const LevelScene& scene) {
    Vec3 remaining = delta;
    for (int iter = 0; iter < 3 && remaining.length_sq() > 1e-6f; ++iter) {
        const TraceHit hit = move_swept(remaining, height, 0.0f, scene);
        if (!hit.hit) return;
        remaining *= (1.0f - hit.fraction);
        if (hit.normal.z >= kWalkableFloorZ) {
            // Walkable ramp: continue parallel to the surface.
            remaining -= hit.normal * remaining.dot(hit.normal);
            remaining += hit.normal * 0.01f;
            continue;
        }
        const float z_before = m_telemetry.position.z;
        if (step_up(remaining, height, scene)) {
            if (m_telemetry.position.z - z_before > 8.0f && m_telemetry.move_state == EMovement::MOVE_Walking) {
                m_telemetry.move_state = EMovement::MOVE_AutoStepUp;
            }
            return;
        }
        // Wall: slide along it and drop the velocity component into it.
        Vec3 n(hit.normal.x, hit.normal.y, 0.0f);
        if (n.length_sq() < 1e-6f) return;
        n = n.normalized();
        remaining -= n * remaining.dot(n);
        remaining.z = 0.0f;
        const float vn = m_telemetry.velocity.x * n.x + m_telemetry.velocity.y * n.y;
        if (vn < 0.0f) {
            m_telemetry.velocity.x -= n.x * vn;
            m_telemetry.velocity.y -= n.y * vn;
        }
        // Nothing left to slide (running straight into the wall): stay in contact.
        if (remaining.length_sq() < 1e-4f) return;
        remaining += n * 0.01f;
    }
}

// UE3 APawn::stepUp: move up MaxStepHeight, retry the blocked move at that height, step back down.
bool ParkourController::step_up(const Vec3& delta, float height, const LevelScene& scene) {
    const float len = delta.length();
    if (len < 1e-3f) return false;
    const Vec3 start = m_telemetry.position;
    move_swept(Vec3(0.0f, 0.0f, kMaxStepHeight), height, 0.0f, scene);
    const float rise = m_telemetry.position.z - start.z;
    if (rise < 1.0f) {
        m_telemetry.position = start;
        return false;
    }
    const Vec3 raised = m_telemetry.position;
    const TraceHit fwd = move_swept(delta, height, 0.0f, scene);
    const float advanced = (m_telemetry.position - raised).length();
    if (fwd.hit && advanced < std::min(1.0f, 0.5f * len)) {
        // Still blocked straight away at the raised height: a wall, not a step.
        m_telemetry.position = start;
        return false;
    }
    const TraceHit down = move_swept(Vec3(0.0f, 0.0f, -rise), height, 0.0f, scene);
    if (down.hit && down.normal.z < kWalkableFloorZ) {
        m_telemetry.position = start;
        return false;
    }
    return true;
}

// Free ballistic flight: gravity plus a swept move with sliding.
void ParkourController::integrate_ballistic(float dt, const LevelScene& scene) {
    m_telemetry.velocity.z -= m_config.gravity * dt;
    const TraceHit hit = move_and_slide(m_telemetry.velocity * dt, kPawnHeight, 0.0f, scene);
    if (hit.hit) {
        const float vn = m_telemetry.velocity.dot(hit.normal);
        if (vn < 0.0f) m_telemetry.velocity -= hit.normal * vn;
    }
}

bool ParkourController::find_ledge_top(const Vec3& wall_normal, float max_rise, const LevelScene& scene, float& ledge_z) const {
    Vec3 into(-wall_normal.x, -wall_normal.y, 0.0f);
    if (into.length_sq() < 1e-6f) return false;
    into = into.normalized();
    const Vec3& p = m_telemetry.position;
    // Distance to the wall face, probed low on the wall (its top may already be below the chest).
    float wall_dist = -1.0f;
    for (float h : {90.0f, 50.0f, 20.0f}) {
        const Vec3 s = p + Vec3(0.0f, 0.0f, h);
        const TraceHit w = trace_ray(s, s + into * 120.0f, scene);
        if (w.hit && std::abs(w.normal.z) < 0.5f) {
            wall_dist = 120.0f * w.fraction;
            break;
        }
    }
    if (wall_dist < 0.0f) return false;
    // Walkable top just beyond the wall face.
    for (float extra : {6.0f, 20.0f, 36.0f}) {
        const Vec3 column = p + into * (wall_dist + extra);
        const TraceHit h = trace_ray(column + Vec3(0.0f, 0.0f, max_rise + 10.0f), column + Vec3(0.0f, 0.0f, 5.0f), scene);
        if (!h.hit || h.normal.z < kWalkableFloorZ || h.point.z > p.z + max_rise) continue;
        if (!has_room_at(Vec3(column.x, column.y, h.point.z + 0.5f), kPawnHeight, scene)) continue;
        ledge_z = h.point.z;
        return true;
    }
    return false;
}

bool ParkourController::climb_onto_ledge(const Vec3& wall_normal, float ledge_z, const LevelScene& scene) {
    Vec3 into(-wall_normal.x, -wall_normal.y, 0.0f);
    if (into.length_sq() < 1e-6f) return false;
    into = into.normalized();
    const Vec3 start = m_telemetry.position;
    // Distance to the wall face just below the ledge lip.
    float wall_dist = 40.0f;
    {
        const Vec3 s(start.x, start.y, std::max(start.z + 5.0f, ledge_z - 8.0f));
        const TraceHit w = trace_ray(s, s + into * 120.0f, scene);
        if (w.hit) wall_dist = 120.0f * w.fraction;
    }
    // Rise until the feet clear the ledge, then move over it.
    const float rise = ledge_z + 2.0f - start.z;
    if (rise > 0.0f) {
        move_swept(Vec3(0.0f, 0.0f, rise), kPawnHeight, 0.0f, scene);
        if (m_telemetry.position.z < ledge_z - 0.5f) {
            m_telemetry.position = start;  // no headroom above the ledge
            return false;
        }
    }
    move_swept(into * (wall_dist + kPawnRadius + 6.0f), kPawnHeight, 0.0f, scene);
    return true;
}

// -----------------------------------------------------------------------------
// TdPawn ground model (native ATdPawn::GetSprintAcceleration / GetWalkAcceleration /
// CalcVelocity and TdPlayerController.PlayerWalking.PlayerMove)
// -----------------------------------------------------------------------------
Vec3 ParkourController::facing_forward() const {
    return Rotator::from_degrees(0.0f, m_telemetry.yaw_deg, 0.0f).forward();
}

Vec3 ParkourController::facing_right() const {
    return Rotator::from_degrees(0.0f, m_telemetry.yaw_deg, 0.0f).right();
}

Vec3 ParkourController::input_direction(const InputFrame& input) const {
    const Vec3 d = facing_forward() * input.forward + facing_right() * input.strafe;
    if (d.length_sq() < 1e-8f) return Vec3(0.0f, 0.0f, 0.0f);
    return d.normalized();
}

// Speed that rises `height` under the pawn's gravity. The scripts write this as
// Gravity * 2 * Sqrt(Height / Gravity) with Gravity = |GetGravityZ()| = 800.
float ParkourController::speed_for_height(float height) const {
    return std::sqrt(2.0f * m_config.gravity * std::max(height, 0.0f));
}

// ATdPawn::GetSprintAcceleration: the acceleration curve along the input direction, steering onto
// it, friction compensation (so sprinting straight follows the speed curve exactly) and a damping
// term for how fast the view turns.
Vec3 ParkourController::sprint_acceleration(const Vec3& dir, const Vec3& vel, float dt, bool falling, float turn_uu) {
    const MovementConfig& c = m_config;
    const Vec3 h = horiz(vel);
    const float speed = h.length();
    if (dir.length_sq() < 1e-8f) {
        m_sprint_energy = 0.0f;
        return Vec3(0.0f, 0.0f, 0.0f);
    }
    m_sprint_energy = std::max(0.0f, speed - c.speed_max_base_velocity);
    Vec3 accel = dir * m_accel_curve.eval(speed);
    if (!falling) {
        // Steer the velocity onto the input direction, keeping its speed.
        const Vec3 diff = dir * speed - h;
        const float d = diff.length();
        if (d > 1e-4f) {
            const float steer = std::min(c.sprint_accel_factor * d, dt > 0.0f ? d / dt : 0.0f);
            accel += diff * (steer / d);
        }
        accel += h * (c.ground_friction * 0.1f);
    }
    accel -= h * (c.turn_decel_factor * turn_uu / 32768.0f);
    return round_accel(accel);
}

// ATdPawn::GetWalkAcceleration: aim for SpeedMinBaseVelocity along the input plus
// (SpeedMaxBaseVelocity - SpeedMinBaseVelocity + sprint energy) scaled by the stick, per axis, and
// close the gap at the walk (forward) and strafe rates. Sprint energy drains unless you keep
// pushing the way you face.
Vec3 ParkourController::walk_acceleration(const Vec3& dir, const InputFrame& input, const Vec3& vel, bool falling) {
    const MovementConfig& c = m_config;
    if (dir.length_sq() < 1e-8f) {
        m_sprint_energy = 0.0f;
        return Vec3(0.0f, 0.0f, 0.0f);
    }
    const Vec3 fwd = facing_forward();
    const Vec3 right = facing_right();
    if (m_sprint_energy > 0.0f) {
        const float along = std::clamp(dir.dot(fwd), 0.0f, 0.9f);
        const float drain = (c.ground_speed - c.speed_max_base_velocity) *
                            (1.0f - std::pow(along, c.energy_decel_exponent)) / c.energy_decel_time;
        m_sprint_energy = std::max(0.0f, m_sprint_energy - drain * m_frame_dt);
    }
    const float top = c.speed_max_base_velocity - c.speed_min_base_velocity + m_sprint_energy;
    const float want_f = dir.dot(fwd) * c.speed_min_base_velocity + top * input.forward;
    const float want_s = dir.dot(right) * c.speed_min_base_velocity + top * input.strafe;
    const float have_f = vel.dot(fwd);
    const float have_s = vel.dot(right);
    Vec3 accel = fwd * ((want_f - have_f) * c.walk_accel_factor) + right * ((want_s - have_s) * c.strafe_accel_factor);
    if (!falling) {
        accel += horiz(vel) * (c.ground_friction * 0.1f);
    }
    return round_accel(accel);
}

// TdPlayerController.PlayerWalking.PlayerMove: sprint acceleration when pushing forward hard
// (aForward > InputMaxSprintHeightLimit and the stick past InputMaxSprintRaduisLimit) and moving
// that way, walk acceleration otherwise. Letting go within 0.15 s of starting stops you dead for
// 0.25 s at 35 uu/s (the tap-stop). There is no sprint button in Mirror's Edge.
Vec3 ParkourController::controller_acceleration(const InputFrame& input, float dt, float turn_uu, bool falling) {
    const MovementConfig& c = m_config;
    Vec3& vel = m_telemetry.velocity;
    if (!falling && m_stop_timer > 0.0f) {
        m_stop_timer -= dt;
        const Vec3 d = horiz(vel);
        const Vec3 stopped = (d.length_sq() > 1e-6f) ? d.normalized() * 35.0f : Vec3(0.0f, 0.0f, 0.0f);
        vel.x = stopped.x;
        vel.y = stopped.y;
        return Vec3(0.0f, 0.0f, 0.0f);
    }
    const Vec3 dir = input_direction(input);
    if (dir.length_sq() > 0.0f) {
        if (!falling) m_accel_time += dt;
        const float size = std::sqrt(input.forward * input.forward + input.strafe * input.strafe);
        const bool sprint = input.forward > c.sprint_input_threshold && size > c.sprint_input_threshold &&
                            horiz(vel).dot(dir) > 0.0f;
        return sprint ? sprint_acceleration(dir, vel, dt, falling, turn_uu)
                      : walk_acceleration(dir, input, vel, falling);
    }
    const Vec3 a = walk_acceleration(dir, input, vel, falling);
    if (!falling) {
        if (m_accel_time > 0.0f && m_accel_time < 0.15f) {
            m_stop_timer = 0.25f;
            const Vec3 d = horiz(vel);
            const Vec3 stopped = (d.length_sq() > 1e-6f) ? d.normalized() * 35.0f : Vec3(0.0f, 0.0f, 0.0f);
            vel.x = stopped.x;
            vel.y = stopped.y;
        }
        m_accel_time = 0.0f;
    }
    return a;
}

// ATdPawn::CalcVelocity for walking: `speed_mod` is the move's SpeedModifier (TdMove_Crouch 0.2),
// `friction` is GroundFriction x the move's FrictionModifier. Accelerating takes velocity x
// friction x 0.1 off per second; braking runs in steps of at most 0.03 s at 2 x friction x
// BrakingFrictionStrength, averaged over the steps, and stops dead once reversed or slow.
void ParkourController::calc_velocity(const Vec3& accel_in, float dt, float speed_mod, float friction) {
    const MovementConfig& c = m_config;
    const float max_accel = c.accel_rate * speed_mod;
    const float max_speed = c.ground_speed * speed_mod;
    Vec3 accel = horiz(accel_in);
    if (accel.length_sq() > max_accel * max_accel) accel = accel.normalized() * max_accel;

    Vec3 v = horiz(m_telemetry.velocity);
    if (accel.length_sq() > 0.0f) {
        v -= v * (dt * friction * 0.1f);
    } else {
        const Vec3 before = v;
        float left = dt;
        Vec3 avg(0.0f, 0.0f, 0.0f);
        while (left > 0.0f) {
            const float step = std::min(left, 0.03f);
            left -= step;
            v -= v * (2.0f * step * friction * c.braking_friction_strength);
            if (v.dot(before) > 0.0f && dt > 0.0f) {
                avg += v * (step / dt);
            }
        }
        v = avg;
        if (v.dot(before) < 0.0f || v.length_sq() < 10.0f * 10.0f) {
            v = Vec3(0.0f, 0.0f, 0.0f);
        }
    }
    v += accel * dt;
    if (v.length_sq() > max_speed * max_speed) v = v.normalized() * max_speed;
    m_telemetry.velocity.x = v.x;
    m_telemetry.velocity.y = v.y;
}

void ParkourController::set_stance(float eye_height) {
    m_telemetry.eye_height = eye_height;
}

// TdPawn.CanSkillRoll: a crouch press within the last RollTriggerWindow (0.2 s).
bool ParkourController::can_skill_roll() const {
    const float age = m_telemetry.sim_time - m_roll_trigger_time;
    return age >= 0.0f && age < m_config.roll_trigger_window;
}

// The pawn leaves the floor into `air_move`: fall tracking starts here (TdMove_Falling.StartMove
// records EnterFallingHeight, the peak the landing height is measured from).
void ParkourController::leave_ground(EMovement air_move) {
    m_telemetry.grounded = false;
    m_base_actor = -1;
    m_air_fall_start_z = m_telemetry.position.z;
    m_fall_peak_z = m_telemetry.position.z;
    m_takeoff_move = air_move;
    m_telemetry.move_state = air_move;
    m_state_timer = 0.0f;
    m_coil_timer = 0.0f;
}

// TdMove_Jump.StartMove: PreJumpMomentum, JumpAddXY along the facing when already moving that
// way, BaseJumpZ (BaseJumpZHeavy with a heavy weapon), LastJumpLocation. Then the ledge assist: a
// ledge less than LedgeAssistHeight above the feet that the arc would fall short of gets the jump
// aimed onto it (the game flies the pawn there; here the take-off speed is raised to reach it).
void ParkourController::start_jump(const LevelScene& scene) {
    const MovementConfig& c = m_config;
    const Vec3 fwd = facing_forward();
    Vec3 vel = m_telemetry.velocity;
    m_pre_jump_momentum = vel.length_xy();
    if (fwd.dot(vel) > 10.0f) {
        vel += fwd * c.jump_add_xy;
    }
    vel.z = m_telemetry.weapon.is_heavy ? c.base_jump_z_heavy : c.base_jump_z;

    const Vec3 h = horiz(vel);
    const float speed = h.length();
    if (speed > 50.0f) {
        const Vec3 dir = h.normalized();
        const Ledge ledge = find_ledge(dir, std::min(300.0f, speed * 0.6f), kMaxStepHeight, c.ledge_assist_height, scene);
        if (ledge.found && dir.dot(-ledge.normal) > 0.7071f) {
            const float rise = ledge.top_z - m_telemetry.position.z;
            float dist = std::max(0.0f, ledge.wall_distance - kPawnRadius - 8.0f);
            const float t = dist / std::max(speed, 1.0f);
            const float z_at = vel.z * t - 0.5f * c.gravity * t * t;
            if (z_at < rise && has_room_at(ledge.top_point, kPawnHeight, scene)) {
                vel.z = std::max(vel.z, speed_for_height(rise + 4.0f));
            }
        }
    }

    m_telemetry.velocity = vel;
    m_last_jump_location = m_telemetry.position;
    set_stance(kEyeHeightStand);
    leave_ground(EMovement::MOVE_Jump);
}

// TdMove_DodgeJump: strafing hard (MoveActionHint left / right at bMoveActionMax) when jumping
// hops sideways: +-600 sideways plus 0.3 of the old velocity, 300 up. No air control, no grabs.
bool ParkourController::try_initiate_dodge_jump(const InputFrame& input) {
    if (std::abs(input.strafe) <= 0.96f) return false;
    const MovementConfig& c = m_config;
    const Vec3 side = facing_right() * sign_of(input.strafe);
    Vec3 vel = side * c.dodge_jump_side_speed + m_telemetry.velocity * c.dodge_jump_inertia;
    vel.z = c.dodge_jump_z;
    m_pre_jump_momentum = m_telemetry.velocity.length_xy();
    m_telemetry.velocity = vel;
    m_last_jump_location = m_telemetry.position;
    set_stance(kEyeHeightStand);
    leave_ground(EMovement::MOVE_DodgeJump);
    return true;
}

// TdMove_Landing.StartMove: FallingHeight = EnterFallingHeight - Z decides LandHard (>= 530),
// SkillRoll (>= 200 with a fresh crouch press, not backwards), SoftLanding (>= 300, animation
// only) or a plain landing. Landing a plain jump caps the speed at PreJumpMomentum -
// LandingSpeedReduction (SubtractLandingSpeed). Out of a 180 in the air you land on your back.
void ParkourController::land(const FloorHit& floor, const LevelScene& scene) {
    (void)scene;
    const MovementConfig& c = m_config;
    const float fall = std::max(0.0f, m_fall_peak_z - floor.z);
    const EMovement air_move = m_telemetry.move_state;
    const Vec3 fwd = facing_forward();
    Vec3 h = horiz(m_telemetry.velocity);
    float speed = h.length();

    m_consecutive_wallruns = 0;
    m_wall_turned = false;
    m_coil_timer = 0.0f;
    m_last_wallrun_normal = Vec3(0.0f, 0.0f, 0.0f);
    m_illegal_wall_timer = 0.0f;
    m_telemetry.velocity.z = 0.0f;
    m_telemetry.grounded = true;

    if (fall >= c.uncontrolled_fall) {
        // TdPawn.UncontrolledFall: lethal (respawn at the last checkpoint).
        m_telemetry.health = 0.0f;
        m_telemetry.move_state = EMovement::MOVE_Landing;
        m_landing_timer = c.hard_landing_time;
        m_telemetry.velocity = Vec3(0.0f, 0.0f, 0.0f);
        return;
    }

    if (m_turned_in_air || air_move == EMovement::MOVE_180TurnInAir) {
        // LandBackwards: dead stop, on your back (TdMove_LayOnGround).
        m_turned_in_air = false;
        m_turn_timer = 0.0f;
        m_telemetry.velocity = Vec3(0.0f, 0.0f, 0.0f);
        m_telemetry.move_state = EMovement::MOVE_LayOnGround;
        m_landing_timer = c.lay_on_ground_time;
        m_sprint_energy = 0.0f;
        set_stance(kEyeHeightSlide);
        return;
    }

    const bool backwards = speed > 1.0f && h.dot(fwd) < 0.0f;
    if (fall >= c.skill_roll_min_fall && can_skill_roll() && !backwards && air_move != EMovement::MOVE_MeleeAir) {
        // Skill roll: the impact becomes forward momentum (at least SpeedMaxBaseVelocity).
        const Vec3 dir = (speed > 1.0f) ? h.normalized() : fwd;
        speed = std::max(speed, c.run_speed);
        m_telemetry.velocity = dir * speed;
        m_telemetry.move_state = EMovement::MOVE_SkillRoll;
        m_landing_timer = c.skill_roll_time;
        m_state_timer = 0.0f;
        m_roll_trigger_time = -100.0f;
        m_sprint_energy = std::max(0.0f, speed - c.speed_max_base_velocity);
        set_stance(kEyeHeightCrouch);
        return;
    }

    if (fall >= c.hard_landing_min_fall) {
        // TdMove_Landing.LandHard: dead stop, no input until the animation ends, damage.
        m_telemetry.move_state = EMovement::MOVE_Landing;
        m_landing_timer = c.hard_landing_time;
        m_state_timer = 0.0f;
        m_telemetry.health = std::max(1.0f, m_telemetry.health - 15.0f);
        m_damage_cooldown = c.health_regen_delay;
        m_telemetry.velocity = Vec3(0.0f, 0.0f, 0.0f);
        m_sprint_energy = 0.0f;
        set_stance(kEyeHeightCrouch);
        return;
    }

    // SubtractLandingSpeed: a plain jump (or the coil / air kick out of one) lands at most
    // PreJumpMomentum - LandingSpeedReduction.
    if (m_takeoff_move == EMovement::MOVE_Jump &&
        (air_move == EMovement::MOVE_Jump || air_move == EMovement::MOVE_Falling || air_move == EMovement::MOVE_Coil ||
         air_move == EMovement::MOVE_MeleeAir)) {
        const float cap = std::max(0.0f, m_pre_jump_momentum - c.landing_speed_reduction);
        if (speed > cap) {
            h = (speed > 1e-4f) ? h * (cap / speed) : Vec3(0.0f, 0.0f, 0.0f);
            speed = cap;
            m_telemetry.velocity.x = h.x;
            m_telemetry.velocity.y = h.y;
        }
    }
    m_sprint_energy = std::max(0.0f, speed - c.speed_max_base_velocity);

    if (fall >= c.soft_landing_min_fall) {
        m_telemetry.move_state = EMovement::MOVE_SoftLanding;  // animation only: input works
        m_landing_timer = 0.2f;
    } else {
        m_telemetry.move_state = EMovement::MOVE_Walking;
    }
    set_stance(kEyeHeightStand);
}

// -----------------------------------------------------------------------------
// Ground Locomotion: TdMove_Walking / TdMove_Crouch / TdMove_180Turn + the ground jump chain
// -----------------------------------------------------------------------------
void ParkourController::update_ground_locomotion(const InputFrame& input, float dt, const LevelScene& scene) {
    if (try_initiate_zipline(scene)) return;

    const MovementConfig& c = m_config;
    EMovement& st = m_telemetry.move_state;
    const bool combat = (st == EMovement::MOVE_Melee || st == EMovement::MOVE_Barge);
    const bool turning = (st == EMovement::MOVE_180Turn);
    bool crouched = (st == EMovement::MOVE_Crouch);

    if (turning && m_turn_timer <= 0.0f) {
        st = EMovement::MOVE_Walking;
    }

    // Jump (TdPlayerMoveManager, ground: DodgeJump -> SpringBoard -> SpeedVault -> Jump). A crouched
    // pawn stands up first; under a low ceiling the press is dropped.
    if (jump_pressed() && !combat && !turning) {
        consume_jump();
        if (!crouched || has_room(kPawnHeight, scene)) {
            if (crouched) {
                crouched = false;
                st = EMovement::MOVE_Walking;
                set_stance(kEyeHeightStand);
            }
            if (try_initiate_dodge_jump(input)) return;
            if (try_initiate_springboard(input, scene)) return;
            if (try_initiate_vault(input, scene)) return;
            start_jump(scene);
            return;
        }
    }

    // Crouch press (TdPlayerMoveManager: Slide when TdMove_Slide.CanDoMove, else Crouch).
    if (m_crouch_pressed && !combat && !turning && !crouched) {
        m_crouch_pressed = false;
        const Vec3 fwd = facing_forward();
        const float along = m_telemetry.velocity.dot(fwd);
        FloorHit floor;
        bool steep = false;
        if (check_ground(scene, kMaxFloorDist + 1.0f, kPawnHeight, floor)) {
            const Vec3 h = horiz(m_telemetry.velocity);
            if (h.length_sq() > 1.0f) {
                // Incline along the velocity: uphill slopes steeper than 0.5 refuse the slide.
                const Vec3 d = h.normalized();
                steep = (-(floor.normal.dot(d)) / std::max(floor.normal.z, 0.1f)) > 0.5f;
            }
        }
        if (along >= c.slide_min_speed && !steep) {
            st = EMovement::MOVE_Slide;
            m_slide_timer = 0.0f;
            m_state_timer = 0.0f;
            m_slide_yaw = yaw_of(horiz(m_telemetry.velocity));
            set_stance(kEyeHeightSlide);
            return;
        }
    }

    // Stance: crouch while held (or while there is no room to stand up).
    if (!combat && !turning) {
        if (input.crouch || (crouched && !has_room(kPawnHeight, scene))) {
            st = EMovement::MOVE_Crouch;
            crouched = true;
            set_stance(kEyeHeightCrouch);
        } else {
            crouched = false;
            if (st == EMovement::MOVE_SoftLanding) {
                m_landing_timer -= dt;
                if (m_landing_timer <= 0.0f) st = EMovement::MOVE_Walking;
            } else {
                st = EMovement::MOVE_Walking;
            }
            set_stance(kEyeHeightStand);
        }
    }

    // What the controller asks for, and how the pawn turns it into velocity.
    float speed_mod = crouched ? c.crouch_speed_modifier : 1.0f;
    if (m_telemetry.weapon.equipped) speed_mod *= m_telemetry.weapon.mobility_scale;
    float friction = c.ground_friction;
    Vec3 accel(0.0f, 0.0f, 0.0f);
    if (turning) {
        friction *= c.turn_180_friction;  // TdMove_180Turn.FrictionModifier: coast through the turn
    } else {
        const float turn_uu = m_frame_turn_uu * (dt / std::max(m_frame_dt, 1e-6f));
        accel = controller_acceleration(input, dt, turn_uu, false);
    }
    calc_velocity(accel, dt, speed_mod, friction);

    // UE3 physWalking: swept horizontal move with stepUp (MaxStepHeight 35).
    const float height = crouched ? kCrouchHeight : kPawnHeight;
    walk_move(horiz(m_telemetry.velocity) * dt, height, scene);
}

// -----------------------------------------------------------------------------
// Air Locomotion: TdMove_Jump / Falling / Coil / DodgeJump and the context moves out of them
// -----------------------------------------------------------------------------
void ParkourController::update_air_locomotion(const InputFrame& input, float dt, const LevelScene& scene) {
    const MovementConfig& c = m_config;
    EMovement& st = m_telemetry.move_state;
    if (m_telemetry.position.z > m_fall_peak_z) m_fall_peak_z = m_telemetry.position.z;

    if (st == EMovement::MOVE_180TurnInAir && m_turn_timer <= 0.0f) {
        st = EMovement::MOVE_Falling;  // the turn is done; the landing still knows (m_turned_in_air)
    }
    const bool dodge = (st == EMovement::MOVE_DodgeJump);
    const bool turning = (st == EMovement::MOVE_180TurnInAir);
    const bool kicking = (st == EMovement::MOVE_MeleeAir || st == EMovement::MOVE_MeleeWallrun);

    // TdMove_Coil.CanDoMove: a crouch press out of a jump (never a plain fall or a dodge), moving
    // the way you face, without a heavy weapon. The legs lift TotalHeightBoost over
    // HeightBoostDuration and stay tucked until the landing.
    if (m_crouch_pressed && !m_telemetry.weapon.is_heavy && coil_allowed_from(st) &&
        facing_forward().dot(m_telemetry.velocity) >= 100.0f) {
        m_crouch_pressed = false;
        st = EMovement::MOVE_Coil;
        m_coil_timer = 0.0f;
    }
    if (st == EMovement::MOVE_Coil) m_coil_timer += dt;

    // Context moves (TdPlayerMoveManager auto moves while airborne).
    if (!dodge && !turning && !kicking) {
        if (try_initiate_zipline(scene)) return;
        if (try_initiate_ledge_grab(input, scene)) return;
        if (try_initiate_vault(input, scene)) return;
        if (try_initiate_wallclimb(input, scene)) return;
        if (try_initiate_wallrun(input, scene)) return;
    }
    if (dodge && m_telemetry.velocity.z < -190.0f) {
        st = EMovement::MOVE_Falling;
    }

    // Gravity
    m_telemetry.velocity.z -= c.gravity * dt;

    // Air control: the controller keeps asking for its ground acceleration; physFalling clamps it
    // to AccelRate x AirControl (6144 x 0.025 = 153.6 uu/s^2). Dodges and turns have none.
    if (!dodge && !turning) {
        const float turn_uu = m_frame_turn_uu * (dt / std::max(m_frame_dt, 1e-6f));
        Vec3 a = horiz(controller_acceleration(input, dt, turn_uu, true));
        const float max_a = c.accel_rate * c.air_control;
        if (a.length_sq() > max_a * max_a) a = a.normalized() * max_a;
        m_telemetry.velocity.x += a.x * dt;
        m_telemetry.velocity.y += a.y * dt;
    }

    // Swept displacement with slide-along-surface; the coil lifts the bottom of the box.
    float bottom = 0.0f;
    if (st == EMovement::MOVE_Coil) {
        bottom = c.coil_height_boost * std::clamp(m_coil_timer / std::max(c.coil_duration, 1e-3f), 0.0f, 1.0f);
    }
    const TraceHit hit = move_and_slide(m_telemetry.velocity * dt, kPawnHeight, bottom, scene);
    if (hit.hit) {
        const float vn = m_telemetry.velocity.dot(hit.normal);
        if (vn < 0.0f) m_telemetry.velocity -= hit.normal * vn;
    }
}

// -----------------------------------------------------------------------------
// Wallrun Subsystem (TdMove_WallRun / TdMove_WallrunJump, native UTdMove_WallRun update and
// ATdPawn::physWallRunning)
// -----------------------------------------------------------------------------
bool ParkourController::try_initiate_wallrun(const InputFrame& input, const LevelScene& scene) {
    const MovementConfig& c = m_config;
    if (m_telemetry.weapon.is_heavy) return false;
    if (m_wallrun_cooldown > 0.0f) return false;

    const Vec3 h = horiz(m_telemetry.velocity);
    const float speed = h.length();
    if (speed < c.wallrun_min_speed) return false;
    const Vec3 fwd = facing_forward();
    const Vec3 right = facing_right();
    if (h.dot(fwd) < 0.0f) return false;  // CanDoMove: moving the way you face

    const float top = c.wallrun_min_wall_height - 2.0f;
    auto same_wall = [&](const Vec3& n) {
        return m_last_wallrun_normal.length_sq() > 0.5f && n.dot(m_last_wallrun_normal) > 0.9f;
    };
    // Which way along the wall you would run: the tangent on the side you face.
    auto along_of = [&](const Vec3& n) {
        Vec3 t(-n.y, n.x, 0.0f);
        if (t.dot(fwd) < 0.0f) t = -t;
        return t;
    };

    WallFace hit;
    // MoveActionHint left / right only counts within 30 units of LastJumpLocation (FindWallSide).
    const bool side_hint = (m_telemetry.position - m_last_jump_location).length() < 30.0f && std::abs(input.strafe) > 0.3f;
    if (side_hint) {
        const Vec3 dir = right * sign_of(input.strafe);
        const WallFace hi = probe_wall(dir, c.wallrun_check_distance, top, scene);
        if (hi.found && std::abs(hi.normal.dot(dir)) >= std::cos(c.wallrun_side_angle_deg * DEG2RAD) &&
            !same_wall(hi.normal) && m_telemetry.velocity.dot(along_of(hi.normal)) >= 0.0f) {
            // The same wall again just above step height (a real wall, not a rail).
            const WallFace lo = probe_wall(dir, c.wallrun_check_distance, kMaxStepHeight + 2.0f, scene);
            if (lo.found && lo.normal.dot(hi.normal) > 0.999f) hit = hi;
        }
    }
    if (!hit.found) {
        // FindWallForward: the reach grows from WallRunningForwardCheckDistance at SpeedMaxBase to
        // ContextMoveDistanceMultiplier times it at GroundSpeed.
        const float frac = std::clamp((speed - c.speed_max_base_velocity) /
                                      std::max(c.ground_speed - c.speed_max_base_velocity, 1.0f), 0.0f, 1.0f);
        const float reach = ((c.wallrun_check_distance_mult - 1.0f) * frac + 1.0f) * c.wallrun_check_distance;
        const WallFace mid = probe_wall(fwd, reach, kPawnHeight * 0.5f, scene);
        if (mid.found) {
            // Facing at most WallRunningForwardMaxStartAngle into the wall (MinStartAngle is 0).
            const float d = -mid.normal.dot(fwd);
            if (d <= std::cos((90.0f - c.wallrun_max_angle_deg) * DEG2RAD) && !same_wall(mid.normal)) {
                // Tall enough: the wall again up at MinWallHeight, 50 lower the squarer you face it.
                const WallFace tall = probe_wall(fwd, reach, top - (1.0f - d) * 50.0f, scene);
                if (tall.found && tall.normal.dot(mid.normal) > 0.9f &&
                    m_telemetry.velocity.dot(along_of(mid.normal)) >= 0.0f) {
                    hit = mid;
                }
            }
        }
    }
    if (!hit.found) return false;
    if (h.dot(hit.normal) >= 0.0f) return false;  // must be moving into the wall

    const Vec3 n = hit.normal;
    const Vec3 along = along_of(n);
    // Falling: no wallrun if there is ground just under where the run would take you (half a
    // second of travel ahead, 45 units down).
    if (m_telemetry.velocity.z < 0.0f) {
        const Vec3 start = m_telemetry.position + n * kPawnRadius + Vec3(0.0f, 0.0f, 2.0f);
        const Vec3 end = start + along * (speed * 0.5f) - Vec3(0.0f, 0.0f, 45.0f);
        if (trace_ray(start, end, scene).hit) return false;
    }

    // --- TdMove_WallRun.StartMove ---
    const bool from_wallrun_jump = (m_telemetry.move_state == EMovement::MOVE_WallRunJump);
    m_consecutive_wallruns = from_wallrun_jump ? m_consecutive_wallruns + 1 : 0;
    const float chain = 1.0f + static_cast<float>(m_consecutive_wallruns);

    float begin_speed = speed;
    if (m_telemetry.velocity.z > 0.0f) begin_speed = std::max(begin_speed, 300.0f);
    m_wallrun_begin_speed = begin_speed;

    // WallRunHeight: the InitialZHeight budget (cut by the ceiling) less what the jump already rose.
    float height = std::min(c.wallrun_initial_z, headroom(c.wallrun_initial_z, scene));
    height -= (m_telemetry.position.z - m_last_jump_location.z);
    height = std::max(0.0f, height);
    if (m_telemetry.velocity.z > 0.0f && !from_wallrun_jump) {
        m_telemetry.velocity.z = std::sqrt(2.0f * height * c.wallrun_accel * chain);
    }
    // ReachedWall: the horizontal velocity runs along the wall at BeginSpeed.
    m_telemetry.velocity.x = along.x * begin_speed;
    m_telemetry.velocity.y = along.y * begin_speed;

    const bool is_right = (n.dot(right) < 0.0f);
    m_telemetry.move_state = is_right ? EMovement::MOVE_WallRunningRight : EMovement::MOVE_WallRunningLeft;
    m_telemetry.wall_normal = n;
    m_telemetry.grounded = false;
    m_base_actor = -1;
    m_last_wallrun_normal = n;
    m_illegal_wall_timer = 0.0f;
    m_wall_tangent = along;
    m_wall_turned = false;
    m_state_timer = 0.0f;
    m_coil_timer = 0.0f;
    m_telemetry.camera_roll_deg = is_right ? -kWallrunCameraRoll : kWallrunCameraRoll;
    set_stance(kEyeHeightStand);

    // Keep the box's AABB clear of the (possibly rotated) wall so modular seams do not snag the sweeps.
    const float aabb_support = kPawnRadius * (std::abs(n.x) + std::abs(n.y)) + 4.0f;
    const float cur_wall_dist = (m_telemetry.position - hit.point).dot(n);
    if (cur_wall_dist < aabb_support) {
        move_swept(n * (aabb_support - cur_wall_dist), kPawnHeight, 0.0f, scene);
    }
    return true;
}

void ParkourController::update_wallrun(const InputFrame& input, float dt, const LevelScene& scene) {
    const MovementConfig& c = m_config;
    const Vec3 n = m_telemetry.wall_normal;
    const Vec3 along = m_wall_tangent;
    const Vec3 h = horiz(m_telemetry.velocity);
    float speed = h.dot(along);
    const float chain = 1.0f + static_cast<float>(m_consecutive_wallruns);

    auto end_wallrun = [&](float cooldown) {
        m_wallrun_cooldown = cooldown;
        m_telemetry.move_state = EMovement::MOVE_Falling;
        m_takeoff_move = EMovement::MOVE_Falling;
        m_telemetry.camera_roll_deg = 0.0f;
        m_illegal_wall_timer = 2.0f;
    };

    // Q: look square at the wall (bTurned90FromWall): the view swings to the wall normal.
    if (input.turn_180 && !m_prev_turn_180 && !m_wall_turned) {
        m_wall_turned = true;
        m_turn_timer = 0.15f;
        m_turn_total = 0.15f;
        m_turn_target_yaw = yaw_of(n);
    }

    // Jump off (TdMove_WallrunJump.StartMove) after the first 0.1 s on the wall. Strafing hard away
    // from the wall is the WallrunDodge instead.
    if (jump_pressed() && m_state_timer > 0.1f) {
        consume_jump();
        const Vec3 fwd = facing_forward();
        const Vec3 strafe_dir = facing_right() * sign_of(input.strafe);
        if (std::abs(input.strafe) >= 0.8f && strafe_dir.dot(n) > 0.0f) {
            Vec3 vel = strafe_dir * 300.0f + m_telemetry.velocity * 0.3f;
            vel.z = 600.0f;
            m_telemetry.velocity = vel;
            m_telemetry.camera_roll_deg = 0.0f;
            m_last_jump_location = m_telemetry.position;
            m_last_wallrun_normal = n;
            m_illegal_wall_timer = 2.0f;
            m_wallrun_cooldown = 0.15f;
            leave_ground(EMovement::MOVE_DodgeJump);
            return;
        }
        // `push`: how far you look away from the wall. It pushes you out harder and higher and
        // trades away along-wall speed; after Q it is the full push-off.
        const float push = m_wall_turned ? 1.0f : std::max(0.0f, fwd.dot(n));
        const float height = headroom(c.wallrun_jump_height + c.wallrun_jump_height_look_add * push, scene);
        const float up = speed_for_height(height) / chain;
        const float out = c.wallrun_jump_out + c.wallrun_jump_out_look_add * push;
        const float keep = c.wallrun_jump_forward_min + (1.0f - c.wallrun_jump_forward_min) * (1.0f - push);
        const Vec3 along_v = h - n * h.dot(n);
        m_telemetry.velocity = n * out + along_v * keep + Vec3(0.0f, 0.0f, up);
        m_telemetry.camera_roll_deg = 0.0f;
        m_last_jump_location = m_telemetry.position;
        m_last_wallrun_normal = n;
        m_illegal_wall_timer = 2.0f;
        m_wallrun_cooldown = 0.15f;
        leave_ground(EMovement::MOVE_WallRunJump);
        return;
    }

    // Wallrun Kick (TdMove_MeleeWallrun)
    if (input.melee && m_melee_cooldown <= 0.0f) {
        m_wallrun_cooldown = 0.20f;
        m_telemetry.move_state = EMovement::MOVE_MeleeWallrun;
        m_takeoff_move = EMovement::MOVE_MeleeWallrun;
        m_state_timer = 0.0f;
        m_telemetry.combat_anim_time = 0.0f;
        m_telemetry.combat_anim_duration = 0.55f;
        m_melee_cooldown = 0.55f;
        m_telemetry.velocity = n * 260.0f + along * 380.0f + Vec3(0, 0, 140.0f);
        m_telemetry.camera_roll_deg = 0.0f;
        m_illegal_wall_timer = 2.0f;
        return;
    }

    // Still on the wall? The pawn's extent touches the wall anywhere between the knees and the head
    // (the reach tolerates modular wall seams).
    TraceHit wall_check;
    bool still_wall = false;
    for (float h : {kPawnHeight * 0.5f, 40.0f, kPawnHeight - 20.0f}) {
        const Vec3 probe_start = m_telemetry.position + Vec3(0.0f, 0.0f, h);
        const TraceHit t = trace_ray(probe_start, probe_start - n * (kPawnRadius + 65.0f), scene);
        if (t.hit && horiz(t.normal).normalized().dot(n) > 0.7f) {
            wall_check = t;
            still_wall = true;
            break;
        }
    }
    const bool falling_too_fast = m_telemetry.velocity.z < -c.wallrun_stop_fall_speed;
    if (!still_wall || falling_too_fast || m_crouch_pressed || input.forward < -0.3f) {
        if (m_crouch_pressed) m_crouch_pressed = false;
        end_wallrun(0.15f);
        return;
    }

    // UTdMove_WallRun's per-frame update and ATdPawn::physWallRunning: the controller asks for no
    // acceleration, so holding forward does not speed you up. The velocity is kept along the wall
    // with a slight drag (WallRunningHorisontalFriction); vertically you are pulled down at
    // WallRunningHorisontalAcceleration while rising and WallRunningHorisontalDeceleration while
    // falling, both x (1 + ConsequtiveWallruns).
    speed -= speed * c.wallrun_friction * dt;
    if (speed < 0.1f) {
        end_wallrun(0.15f);
        return;
    }
    const float pull = (m_telemetry.velocity.z > 0.0f ? c.wallrun_accel : c.wallrun_decel) * chain;
    m_telemetry.velocity = along * speed - n * 0.5f + Vec3(0.0f, 0.0f, m_telemetry.velocity.z - pull * dt);

    // Keep the box's AABB slightly clear of angled wall faces so modular static mesh seams never snag.
    const float aabb_support = kPawnRadius * (std::abs(n.x) + std::abs(n.y)) + 4.0f;
    const float cur_wall_dist = (m_telemetry.position - wall_check.point).dot(n);
    if (cur_wall_dist < aabb_support) {
        move_swept(n * (aabb_support - cur_wall_dist), kPawnHeight, 0.0f, scene);
    }

    // Swept movement along the wall. If a modular wall seam, window frame or thin pilaster
    // (<= 28u along +normal) blocks the tangent sweep, step outward along +normal just like
    // APawn::stepUp does for floor kerbs.
    const Vec3 full_delta = m_telemetry.velocity * dt;
    const TraceHit first = move_swept(full_delta, kPawnHeight, 0.0f, scene);
    if (first.hit) {
        bool stepped_over_seam = false;
        const Vec3 remaining = full_delta * (1.0f - first.fraction);
        if (first.normal.z < kWalkableFloorZ && remaining.length_sq() > 1e-6f) {
            constexpr float kMaxWallSeamStep = 28.0f;
            const Vec3 pos_at_contact = m_telemetry.position;
            move_swept(n * kMaxWallSeamStep, kPawnHeight, 0.0f, scene);
            const float step_out = (m_telemetry.position - pos_at_contact).dot(n);
            if (step_out > 0.5f) {
                const Vec3 pos_stepped = m_telemetry.position;
                const TraceHit retry = move_swept(remaining, kPawnHeight, 0.0f, scene);
                const float advanced = (m_telemetry.position - pos_stepped).dot(along);
                const float expected = remaining.dot(along);
                if (!retry.hit || (expected > 1e-3f && advanced > 0.5f * expected)) {
                    // Settle back toward the new wall panel plane while keeping clean standoff
                    move_swept(-n * std::max(0.0f, step_out - 2.0f), kPawnHeight, 0.0f, scene);
                    stepped_over_seam = true;
                } else {
                    m_telemetry.position = pos_at_contact;
                }
            } else {
                m_telemetry.position = pos_at_contact;
            }
        }

        if (!stepped_over_seam) {
            // Slide the remaining movement along the blocking surface; a wall across the run or a
            // floor ends the wallrun.
            Vec3 slide_rem = full_delta * (1.0f - first.fraction);
            slide_rem -= first.normal * slide_rem.dot(first.normal);
            if (slide_rem.length_sq() > 1e-6f) {
                move_swept(slide_rem + first.normal * 0.01f, kPawnHeight, 0.0f, scene);
            }
            const float vn = m_telemetry.velocity.dot(first.normal);
            if (vn < 0.0f) m_telemetry.velocity -= first.normal * vn;
            if (first.normal.dot(along) < -0.5f || first.normal.z >= kWalkableFloorZ) {
                end_wallrun(0.20f);
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Wallclimb & 180° Turn Jump Subsystem (TdMove_WallClimb / TdMove_WallClimb180TurnJump)
// -----------------------------------------------------------------------------
bool ParkourController::try_initiate_wallclimb(const InputFrame& input, const LevelScene& scene) {
    const MovementConfig& c = m_config;
    if (m_telemetry.weapon.is_heavy) return false;
    // Native CanDoMove: rising (VelocityStartLimit 0), MoveActionHint forward, facing the wall
    // within WallClimbingVerticalStartAngle, the wall within WallClimbingMaxDistance2D and at
    // least MinWallHeight tall.
    if (m_telemetry.velocity.z <= 0.0f) return false;
    if (input.forward <= 0.8f) return false;
    const Vec3 fwd = facing_forward();
    const WallFace wall = probe_wall(fwd, c.wallclimb_max_distance, kPawnHeight * 0.5f, scene);
    if (!wall.found || wall.distance > c.wallclimb_max_distance) return false;
    if (fwd.dot(-wall.normal) < std::cos(c.wallclimb_max_angle_deg * DEG2RAD)) return false;
    if (m_last_wallrun_normal.length_sq() > 0.5f && wall.normal.dot(m_last_wallrun_normal) > 0.9f) return false;
    const WallFace tall = probe_wall(fwd, c.wallclimb_max_distance, c.wallclimb_min_wall_height - 2.0f, scene);
    if (!tall.found || tall.normal.dot(wall.normal) < 0.9f) return false;

    // --- TdMove_WallClimb.StartMove ---
    const Vec3 into = -wall.normal;
    m_into_wallclimb_speed = m_telemetry.velocity.length_xy() - 100.0f;
    if (m_into_wallclimb_speed < 100.0f) {
        // Too slow to carry into the wall: the pawn is sucked in at 400.
        m_telemetry.velocity.x = into.x * c.wallclimb_suck_in_speed;
        m_telemetry.velocity.y = into.y * c.wallclimb_suck_in_speed;
    }
    m_telemetry.move_state = EMovement::MOVE_WallClimbing;
    m_telemetry.wall_normal = wall.normal;
    m_telemetry.grounded = false;
    m_base_actor = -1;
    m_wallclimb_reached = false;
    m_wall_turned = false;
    m_state_timer = 0.0f;
    m_coil_timer = 0.0f;
    set_stance(kEyeHeightStand);
    return true;
}

void ParkourController::update_wallclimb(const InputFrame& input, float dt, const LevelScene& scene) {
    const MovementConfig& c = m_config;
    const Vec3 n = m_telemetry.wall_normal;
    const Vec3 into = -n;
    if (m_telemetry.position.z > m_fall_peak_z) m_fall_peak_z = m_telemetry.position.z;

    auto fall_off = [&]() {
        m_telemetry.move_state = EMovement::MOVE_Falling;
        m_takeoff_move = EMovement::MOVE_Falling;
        m_last_wallrun_normal = n;
        m_illegal_wall_timer = 0.5f;
    };

    if (m_wall_turned) {
        // TdMove_WallClimb180TurnJump: turned to face away, JumpTimeWindow to push off (a ceiling
        // cuts the height); otherwise the pawn slides down the wall and drops.
        if (jump_pressed()) {
            consume_jump();
            Vec3 vel = n * c.wallclimb_turn_jump_out;
            vel.z = speed_for_height(headroom(c.wallclimb_turn_jump_height, scene));
            m_telemetry.velocity = vel;
            m_last_jump_location = m_telemetry.position;
            m_last_wallrun_normal = n;
            m_illegal_wall_timer = 2.0f;
            leave_ground(EMovement::MOVE_WallClimb180TurnJump);
            return;
        }
        if (m_state_timer > c.wallclimb_turn_jump_window) {
            fall_off();
            return;
        }
        m_telemetry.velocity = into * 0.3f + Vec3(0.0f, 0.0f, std::min(m_telemetry.velocity.z, 50.0f) - c.gravity * 0.25f * dt);
        move_and_slide(m_telemetry.velocity * dt, kPawnHeight, 0.0f, scene);
        return;
    }

    // Q: 180° turn on the wall (root rotation) then the jump window.
    if (input.turn_180 && !m_prev_turn_180 && m_wallclimb_reached) {
        m_wall_turned = true;
        m_turn_timer = c.turn_180_time;
        m_turn_total = c.turn_180_time;
        m_turn_target_yaw = m_telemetry.yaw_deg + 180.0f;
        m_state_timer = 0.0f;
        return;
    }

    // WallClimbDodge: a jump press while strafing hard hops sideways along the wall face.
    if (jump_pressed() && std::abs(input.strafe) >= 0.8f && m_wallclimb_reached) {
        consume_jump();
        Vec3 along(-n.y, n.x, 0.0f);
        if (along.dot(facing_right() * sign_of(input.strafe)) < 0.0f) along = -along;
        Vec3 vel = along * c.wallclimb_dodge_side_speed + n * 30.0f;
        vel.z = c.wallclimb_dodge_z;
        m_telemetry.velocity = vel;
        m_last_jump_location = m_telemetry.position;
        m_last_wallrun_normal = n;
        m_illegal_wall_timer = 2.0f;
        leave_ground(EMovement::MOVE_DodgeJump);
        return;
    }

    // Reaching the wall (ReachedWall): the climb speed is set from how fast you ran in and how
    // fast you were rising: height = AddOnSpeed2DHeight x f(IntoWallClimbSpeed) +
    // AddOnSpeedZHeight x f(Velocity.Z), Velocity.Z = Sqrt(4 x height x WallClimbingGravity).
    if (!m_wallclimb_reached) {
        const WallFace face = probe_wall(into, c.wallclimb_max_distance, kPawnHeight * 0.5f, scene);
        if (!face.found) {
            fall_off();
            return;
        }
        if (face.distance <= kPawnRadius + 4.0f) {
            const float f_xy = std::clamp((m_into_wallclimb_speed - c.speed_max_base_velocity) /
                                          std::max(c.wallclimb_add_xy_max_speed - c.speed_max_base_velocity, 1.0f), 0.0f, 1.0f);
            const float f_z = std::clamp(m_telemetry.velocity.z / std::max(c.wallclimb_boost_z, 1.0f), 0.0f, 1.0f);
            const float height = c.wallclimb_add_xy_height * f_xy + c.wallclimb_add_z_height * f_z;
            m_telemetry.velocity = Vec3(0.0f, 0.0f, std::sqrt(std::max(4.0f * height * c.wallclimb_gravity, 0.01f)));
            m_wallclimb_reached = true;
        } else {
            // Still flying in: ordinary falling physics until the body touches the wall.
            m_telemetry.velocity.z -= c.gravity * dt;
            move_and_slide(m_telemetry.velocity * dt, kPawnHeight, 0.0f, scene);
            if (m_telemetry.velocity.z <= 0.0f) fall_off();
            return;
        }
    }

    // The ledge at the top: hands catch it (IntoGrab) or, when it is already low, mantle over.
    if (try_initiate_ledge_grab(input, scene)) return;

    // Still a wall ahead? Otherwise the top has been passed with nothing to grab.
    const WallFace still = probe_wall(into, kPawnRadius + 20.0f, kPawnHeight * 0.5f, scene);
    if (!still.found) {
        fall_off();
        return;
    }

    // Climb: the pawn's acceleration is straight down at the climb gravity (x2, like every
    // vertical move); no drag on the climb itself.
    m_telemetry.velocity.z -= 2.0f * c.wallclimb_gravity * dt;
    if (m_telemetry.velocity.z <= 0.0f) {
        fall_off();
        return;
    }
    const TraceHit climb_hit = move_swept(Vec3(0.0f, 0.0f, m_telemetry.velocity.z * dt), kPawnHeight, 0.0f, scene);
    if (climb_hit.hit) {
        // An overhang stops the ascent.
        m_telemetry.velocity.z = 0.0f;
        fall_off();
    }
}

// -----------------------------------------------------------------------------
// Slide Subsystem (TdMove_Slide / TdMove_MeleeSlide)
// -----------------------------------------------------------------------------
void ParkourController::update_slide(const InputFrame& input, float dt, LevelScene& scene) {
    const MovementConfig& c = m_config;
    m_slide_timer += dt;
    EMovement& st = m_telemetry.move_state;

    // Slide Melee Kick (`MeleeSlide`: sweeps enemies off their feet with `HitMeleeSlide`)
    if (input.melee && st != EMovement::MOVE_MeleeSlide && m_melee_cooldown <= 0.0f) {
        st = EMovement::MOVE_MeleeSlide;
        m_state_timer = 0.0f;
        m_telemetry.combat_anim_time = 0.0f;
        m_telemetry.combat_anim_duration = 0.55f;
        m_melee_cooldown = 0.55f;

        for (auto& bot : scene.enemies) {
            if (bot.alive && m_telemetry.position.distance(bot.position) < 200.0f) {
                bot.health -= 65.0f;
                bot.stunned = true;
                bot.disarm_window = true;
                bot.attack_timer = 0.0f;
                bot.anim_timer = 0.0f;
                bot.active_anim_seq = "HitMeleeSlide";
                m_telemetry.melee_hit_confirmed = true;
                m_telemetry.hit_marker_timer = 0.25f;
                if (bot.health <= 0.0f) {
                    bot.alive = false;
                    bot.anim_state = EEnemyAnimState::KnockedOut;
                    m_telemetry.active_subtitle = "Slide Kick Knockout!";
                } else {
                    bot.anim_state = EEnemyAnimState::HitStagger;
                    m_telemetry.active_subtitle = "Slide Kick Hit! Enemy Staggered (Press Right-Click / E to Disarm)";
                }
            }
        }
    }

    // Move input is ignored for the whole slide (DisableMovementTime -1). The body turns toward
    // the view at SlideLookTurn x the angle per second, A / D (MoveActionHint left / right) turn it
    // 2000 rotation units per second, and the velocity is pointed along the body.
    const float hint_side = (input.strafe > 0.3f) ? 1.0f : (input.strafe < -0.3f ? -1.0f : 0.0f);
    const bool hint_down = hint_side == 0.0f && input.forward < -0.8f;
    const float d = wrap_deg(m_telemetry.yaw_deg - m_slide_yaw);
    m_slide_yaw += dt * c.slide_look_turn * d - hint_side * c.slide_strafe_turn_deg * dt;
    const Vec3 body = Rotator::from_degrees(0.0f, m_slide_yaw, 0.0f).forward();
    float speed = m_telemetry.velocity.length_xy();
    m_telemetry.velocity.x = body.x * speed;
    m_telemetry.velocity.y = body.y * speed;

    // CalcVelocity brakes it with GroundFriction x FrictionModifier.
    calc_velocity(Vec3(0.0f, 0.0f, 0.0f), dt, 1.0f, c.ground_friction * c.slide_friction);
    speed = m_telemetry.velocity.length_xy();

    // It aborts into a crouch below SlideAbortSpeed, after SlideAbortTime, or when you pull back;
    // letting go of crouch ends it into a walk (a crouch with no room), but not inside the first
    // 0.5 s (the request waits for StartMove's timer). Jump does nothing in a slide.
    const bool abort = hint_down || speed < c.slide_abort_speed || m_slide_timer >= c.slide_max_duration;
    const bool uncrouch = !input.crouch && m_slide_timer >= c.slide_min_duration;
    if (abort || uncrouch) {
        // StopMove halves the velocity, however the slide ends.
        m_telemetry.velocity.x *= 0.5f;
        m_telemetry.velocity.y *= 0.5f;
        const bool stand = !input.crouch && has_room(kPawnHeight, scene);
        st = stand ? EMovement::MOVE_Walking : EMovement::MOVE_Crouch;
        set_stance(stand ? kEyeHeightStand : kEyeHeightCrouch);
        m_sprint_energy = std::max(0.0f, m_telemetry.velocity.length_xy() - c.speed_max_base_velocity);
        m_accel_time = 1.0f;
        return;
    }

    // Low (crouch-height) swept move: passes under ducts, steps over seams, glances off walls.
    walk_move(horiz(m_telemetry.velocity) * dt, kCrouchHeight, scene);
}

// -----------------------------------------------------------------------------
// Ledge Grab, Shimmy & Pull-Up Subsystem (TdMove_IntoGrab / Grab / GrabPullUp / GrabJump)
// -----------------------------------------------------------------------------
bool ParkourController::try_initiate_ledge_grab(const InputFrame& input, const LevelScene& scene) {
    (void)input;
    const MovementConfig& c = m_config;
    const EMovement st = m_telemetry.move_state;
    if (st == EMovement::MOVE_Grabbing || st == EMovement::MOVE_GrabPullUp || st == EMovement::MOVE_IntoGrab ||
        st == EMovement::MOVE_DodgeJump) {
        return false;
    }

    const Vec3 fwd = facing_forward();
    const Ledge ledge = find_ledge(fwd, 45.0f, kGrabMinRise, kGrabMaxRise, scene);
    if (!ledge.found) return false;
    const Vec3 into = -ledge.normal;
    // GrabMaxAngle: facing the wall; and not moving away from it.
    if (fwd.dot(into) < std::cos(c.grab_max_angle_deg * DEG2RAD)) return false;
    if (horiz(m_telemetry.velocity).dot(ledge.normal) > 50.0f) return false;
    // No re-grabbing the wall just jumped off.
    if (m_last_wallrun_normal.length_sq() > 0.5f && ledge.normal.dot(m_last_wallrun_normal) > 0.9f &&
        m_illegal_wall_timer > 0.0f && st != EMovement::MOVE_WallClimbing) {
        return false;
    }
    // Room for the body on top (standing, or crouched under a low ceiling).
    const Vec3 on_top = ledge.top_point + into * (kPawnRadius + 2.0f) + Vec3(0.0f, 0.0f, 0.5f);
    const bool stand_room = has_room_at(on_top, kPawnHeight, scene);
    if (!stand_room && !has_room_at(on_top, kCrouchHeight, scene)) return false;

    const float rise = ledge.top_z - m_telemetry.position.z;
    if (rise <= kMantleMaxRise && m_telemetry.velocity.z > -50.0f) {
        // Already up to the hands: mantle straight over (the top of a wallclimb / a high jump).
        if (climb_onto_ledge(ledge.normal, ledge.top_z, scene)) {
            const float speed = std::max(200.0f, std::min(m_telemetry.velocity.length_xy(), c.speed_max_base_velocity));
            m_telemetry.velocity = into * speed;
            m_telemetry.grounded = true;
            m_telemetry.move_state = stand_room ? EMovement::MOVE_Walking : EMovement::MOVE_Crouch;
            m_state_timer = 0.0f;
            m_sprint_energy = 0.0f;
            m_last_wallrun_normal = Vec3(0.0f, 0.0f, 0.0f);
            set_stance(stand_room ? kEyeHeightStand : kEyeHeightCrouch);
            return true;
        }
        return false;
    }

    // Hang: hands on the lip, the top GrabHangDepth above the feet, the body against the wall.
    const float target_z = ledge.top_z - c.grab_hang_depth;
    move_swept(Vec3(0.0f, 0.0f, target_z - m_telemetry.position.z), kPawnHeight, 0.0f, scene);
    const float gap = ledge.wall_distance - kPawnRadius - 1.0f;
    if (gap > 0.0f) move_swept(into * gap, kPawnHeight, 0.0f, scene);

    m_telemetry.move_state = EMovement::MOVE_Grabbing;
    m_telemetry.velocity = Vec3(0.0f, 0.0f, 0.0f);
    m_telemetry.wall_normal = ledge.normal;
    m_telemetry.grounded = false;
    m_telemetry.camera_roll_deg = 0.0f;
    m_ledge_z = ledge.top_z;
    m_hang_time = 0.0f;
    m_state_timer = 0.0f;
    m_base_actor = -1;
    m_coil_timer = 0.0f;
    m_consecutive_wallruns = 0;
    m_wall_turned = false;
    set_stance(kEyeHeightStand);
    return true;
}

void ParkourController::update_ledge_grab(const InputFrame& input, float dt, const LevelScene& scene) {
    const MovementConfig& c = m_config;
    EMovement& st = m_telemetry.move_state;
    const Vec3 n = m_telemetry.wall_normal;
    const Vec3 into = -n;
    const Vec3 fwd = facing_forward();
    m_hang_time += dt;
    m_telemetry.velocity = Vec3(0.0f, 0.0f, 0.0f);

    if (st == EMovement::MOVE_GrabPullUp) {
        // The camera rises with the body over the pull-up; the body lands on the ledge at the end.
        const float progress = std::clamp(m_state_timer / std::max(c.grab_pull_up_time, 1e-3f), 0.0f, 1.0f);
        set_stance(kEyeHeightStand + progress * (m_ledge_z - m_telemetry.position.z));
        if (m_state_timer >= c.grab_pull_up_time) {
            if (climb_onto_ledge(n, m_ledge_z, scene)) {
                const bool stand = has_room(kPawnHeight, scene);
                st = stand ? EMovement::MOVE_Walking : EMovement::MOVE_Crouch;
                m_telemetry.grounded = true;
                m_telemetry.velocity = into * 100.0f;
                set_stance(stand ? kEyeHeightStand : kEyeHeightCrouch);
            } else {
                m_telemetry.velocity = n * 50.0f;
                set_stance(kEyeHeightStand);
                leave_ground(EMovement::MOVE_Falling);
            }
            m_sprint_energy = 0.0f;
        }
        return;
    }

    // TdMove_Grab: facing within 45° of the wall a jump (or pushing forward) pulls up; looking
    // away from it a jump is a GrabJump; crouch (or pulling back) lets go; A / D shimmy along.
    const bool facing_wall = fwd.dot(into) >= std::cos(45.0f * DEG2RAD);
    if (jump_pressed()) {
        consume_jump();
        if (facing_wall) {
            st = EMovement::MOVE_GrabPullUp;
            m_state_timer = 0.0f;
            return;
        }
        const float push = std::clamp(fwd.dot(n), 0.0f, 1.0f);
        const Vec3 dir = horiz(fwd).normalized();
        Vec3 vel = dir * (c.grab_jump_push_min + (c.grab_jump_push_max - c.grab_jump_push_min) * push);
        vel.z = speed_for_height(c.grab_jump_height);
        m_telemetry.velocity = vel;
        m_last_jump_location = m_telemetry.position;
        m_last_wallrun_normal = n;
        m_illegal_wall_timer = 2.0f;
        leave_ground(EMovement::MOVE_GrabJump);
        return;
    }
    if (input.forward > 0.8f && facing_wall && m_hang_time > 0.1f) {
        st = EMovement::MOVE_GrabPullUp;
        m_state_timer = 0.0f;
        return;
    }
    if (m_crouch_pressed || (input.forward < -0.8f && m_hang_time > 0.2f)) {
        m_crouch_pressed = false;
        m_telemetry.velocity = n * 50.0f;
        m_last_wallrun_normal = n;
        m_illegal_wall_timer = 0.5f;
        leave_ground(EMovement::MOVE_Falling);
        return;
    }
    if (std::abs(input.strafe) > 0.3f && m_hang_time > c.grab_shimmy_delay) {
        Vec3 along(-n.y, n.x, 0.0f);
        if (along.dot(facing_right() * sign_of(input.strafe)) < 0.0f) along = -along;
        // The ledge has to continue that way: its top again a body width along.
        const Vec3 column = m_telemetry.position + along * (kPawnRadius + 8.0f) + into * (kPawnRadius + 10.0f);
        const TraceHit top = trace_ray(Vec3(column.x, column.y, m_ledge_z + 10.0f),
                                       Vec3(column.x, column.y, m_ledge_z - 10.0f), scene);
        if (top.hit && top.normal.z >= kWalkableFloorZ && std::abs(top.point.z - m_ledge_z) < 4.0f) {
            move_swept(along * (c.grab_shimmy_speed * dt), kPawnHeight, 0.0f, scene);
        }
    }
}

// -----------------------------------------------------------------------------
// Speed Vault & Springboard Subsystems (TdMove_SpeedVault / TdMove_SpringBoard): timed root
// motion over a pre-computed path, collision off (the game plays the pawn along the animation).
// -----------------------------------------------------------------------------
bool ParkourController::try_initiate_springboard(const InputFrame& input, const LevelScene& scene) {
    const MovementConfig& c = m_config;
    if (m_telemetry.weapon.is_heavy) return false;
    if (input.forward <= 0.8f) return false;
    const Vec3 fwd = facing_forward();
    const Vec3 h = horiz(m_telemetry.velocity);
    const float speed = h.length();
    if (speed < 200.0f) return false;
    const Vec3 dir = h.normalized();
    if (dir.dot(fwd) < 0.8f) return false;

    // A step StepHeight +- tolerance above the feet, at least 20 away, reached within
    // CheckDistanceTime at the current speed.
    const float reach = std::min(speed * c.springboard_check_time, 400.0f);
    const Ledge step = find_ledge(dir, reach, c.springboard_step_height - c.springboard_step_tolerance,
                                  c.springboard_step_height + c.springboard_step_tolerance, scene);
    if (!step.found) return false;
    if (step.wall_distance < kPawnRadius + 20.0f) return false;
    if (dir.dot(-step.normal) < 0.7f) return false;

    // A second obstacle ObstacleDistance +- 20 beyond the step's face, 80..148 above the feet.
    const Vec3 step_face = step.top_point;
    const float obstacle_z_min = m_telemetry.position.z + c.springboard_obstacle_min;
    const float obstacle_z_max = m_telemetry.position.z + c.springboard_obstacle_max;
    bool found_obstacle = false;
    Vec3 obstacle_top(0.0f, 0.0f, 0.0f);
    for (float d = c.springboard_obstacle_distance - 20.0f; d <= c.springboard_obstacle_distance + 40.0f && !found_obstacle; d += 10.0f) {
        const Vec3 column = step_face + dir * d;
        const TraceHit top = trace_ray(Vec3(column.x, column.y, obstacle_z_max + 10.0f),
                                       Vec3(column.x, column.y, step.top_z + 10.0f), scene);
        if (!top.hit || top.normal.z < kWalkableFloorZ) continue;
        if (top.point.z < obstacle_z_min || top.point.z > obstacle_z_max) continue;
        if (!has_room_at(Vec3(column.x, column.y, top.point.z + 0.5f), kCrouchHeight, scene)) continue;
        found_obstacle = true;
        obstacle_top = Vec3(column.x, column.y, top.point.z);
    }
    if (!found_obstacle) return false;

    // Run in to the step (1.2 x the speed, at least 200), plant on it, plant on the obstacle,
    // launch at SpringBoardJumpZ with facing x max(400, speed - 100).
    const float run_speed = 1.2f * std::max(200.0f, speed);
    m_path_p0 = m_telemetry.position;
    m_path_p1 = Vec3(step_face.x, step_face.y, step.top_z) + dir * 12.0f;
    m_path_p2 = obstacle_top + Vec3(0.0f, 0.0f, 1.0f);
    m_path_t1 = std::max(0.08f, (m_path_p1 - m_path_p0).length() / run_speed);
    m_path_t2 = std::max(0.12f, (m_path_p2 - m_path_p1).length() / run_speed);
    Vec3 exit = fwd * std::max(400.0f, speed - 100.0f);
    exit.z = c.springboard_jump_z;
    m_path_exit_velocity = exit;
    m_path_end_move = EMovement::MOVE_Jump;

    m_pre_jump_momentum = speed;
    m_telemetry.move_state = EMovement::MOVE_SpringBoarding;
    m_takeoff_move = EMovement::MOVE_SpringBoarding;
    m_state_timer = 0.0f;
    m_telemetry.grounded = false;
    m_base_actor = -1;
    m_fall_peak_z = m_telemetry.position.z;
    set_stance(kEyeHeightStand);
    return true;
}

bool ParkourController::try_initiate_vault(const InputFrame& input, const LevelScene& scene) {
    const MovementConfig& c = m_config;
    if (input.forward <= 0.8f) return false;
    const bool grounded = m_telemetry.grounded;
    const Vec3 fwd = facing_forward();
    const Vec3 h = horiz(m_telemetry.velocity);
    const float speed = h.length();
    // Only from a jump (rising) in the air: the VaultOverHigh / VaultOntoHigh moves.
    if (!grounded && (m_telemetry.velocity.z < 50.0f || m_telemetry.move_state != EMovement::MOVE_Jump)) return false;

    // TimeToHandPlant: the ledge must be reachable within 0.4 s at the current speed (300 at least).
    const float reach = std::max(speed, 300.0f) * c.vault_max_handplant_time - kPawnRadius;
    const float min_rise = grounded ? kMaxStepHeight : kVaultHighMinHeight;
    const float max_rise = grounded ? kVaultMaxHeight : kVaultHighMaxHeight;
    const Ledge ledge = find_ledge(fwd, reach, min_rise, max_rise, scene);
    if (!ledge.found) return false;
    if (fwd.dot(ledge.normal) > -0.2f) return false;  // view facing the obstacle
    const float handplant = ledge.top_z - m_telemetry.position.z;
    if (handplant < min_rise || handplant > max_rise) return false;

    const Vec3 dir = horiz(fwd).normalized();
    const Vec3 into = -ledge.normal;
    const float end_dist = std::max(48.0f, speed * 0.3f);
    const float top_z = ledge.top_z;
    Vec3 face = ledge.top_point - into * 6.0f;  // back on the face line
    face.z = top_z;

    // TdMove_SpeedVault.FindValidOntoEndLocation: the body (a 1.4 x radius box, full height) has to
    // pass VaultClearObjectHeight above the ledge for at least 32 (low ledge) / 64 units along the
    // move before anything blocks it; a wall or panel right behind the ledge means no vault.
    {
        constexpr float kVaultClearObjectHeight = 35.0f;
        const Vec3 body_extent(kPawnRadius * 1.4f, kPawnRadius * 1.4f, kPawnHeight * 0.5f);
        Vec3 start = face;
        start.z = top_z + kVaultClearObjectHeight + body_extent.z;
        const TraceHit block = sweep_box(start, body_extent, into * end_dist, scene);
        if (block.hit) {
            float width = block.start_penetrating ? 0.0f : (block.point - start).dot(into) + body_extent.x;
            if (width <= body_extent.x + 1.0f) {
                const Vec3 small_extent(10.0f, 10.0f, 10.0f);
                Vec3 small_start = start;
                small_start.z = top_z + kVaultClearObjectHeight + small_extent.z;
                const TraceHit small = sweep_box(small_start, small_extent, into * end_dist, scene);
                if (!small.hit) width = end_dist;
                else width = small.start_penetrating ? 0.0f : (small.point - small_start).dot(into) + small_extent.x;
            }
            if (width < (handplant <= 48.0f ? 32.0f : 64.0f)) return false;
        }
    }

    // Onto or over: the far edge within MaxLedgeWidth = clamp(speed x 0.2, 60, 180) and the floor
    // beyond (down to 240 below the top) lower than the top - 64 means over.
    const float max_width = std::clamp(speed * 0.2f, 60.0f, 180.0f);
    bool over = false;
    float far_d = max_width;
    float floor_beyond = top_z - 240.0f;
    bool floor_found = false;
    for (float d = 10.0f; d <= max_width; d += 10.0f) {
        const Vec3 column = face + into * d;
        const TraceHit t = trace_ray(Vec3(column.x, column.y, top_z + 8.0f), Vec3(column.x, column.y, top_z - 240.0f), scene);
        if (!t.hit || t.point.z < top_z - 64.0f) {
            over = true;
            far_d = d;
            floor_found = t.hit;
            floor_beyond = t.hit ? t.point.z : top_z - 240.0f;
            break;
        }
    }

    const float exit_speed = std::clamp(speed + c.vault_speed_add, c.vault_min_speed, c.ground_speed);
    m_path_p0 = m_telemetry.position;
    if (over) {
        m_path_p1 = face + into * (far_d * 0.5f) + Vec3(0.0f, 0.0f, c.vault_ledge_offset_z);
        const Vec3 end = face + into * (far_d + end_dist);
        m_path_p2 = Vec3(end.x, end.y, floor_found ? floor_beyond : top_z - 40.0f);
        m_path_end_move = floor_found ? EMovement::MOVE_Landing : EMovement::MOVE_Falling;
        if (!floor_found) m_path_p2.z = top_z - 20.0f;
    } else {
        // Onto: room for the body on top.
        if (!has_room_at(ledge.top_point + into * (kPawnRadius + 4.0f) + Vec3(0.0f, 0.0f, 0.5f), kCrouchHeight, scene)) {
            return false;
        }
        m_path_p1 = face + into * (kPawnRadius + 6.0f) + Vec3(0.0f, 0.0f, c.vault_ledge_offset_z);
        const Vec3 end = face + into * (kPawnRadius + 6.0f + end_dist * 0.5f);
        m_path_p2 = Vec3(end.x, end.y, top_z);
        m_path_end_move = EMovement::MOVE_Walking;
    }
    m_path_t1 = c.vault_time_over;
    m_path_t2 = c.vault_time_down;
    const Vec3 last_leg = horiz(m_path_p2 - m_path_p1);
    const float leg_speed = std::max(exit_speed * 0.5f, last_leg.length() / c.vault_time_down);
    m_path_exit_velocity = dir * std::min(leg_speed, c.ground_speed);
    if (over) m_path_exit_velocity = dir * exit_speed;

    m_pre_jump_momentum = speed;
    m_telemetry.move_state = (speed >= 350.0f) ? EMovement::MOVE_SpeedVaulting : EMovement::MOVE_VaultOver;
    m_takeoff_move = EMovement::MOVE_SpeedVaulting;
    m_state_timer = 0.0f;
    m_telemetry.grounded = false;
    m_base_actor = -1;
    m_fall_peak_z = top_z + c.vault_ledge_offset_z;
    m_air_fall_start_z = m_fall_peak_z;
    m_coil_timer = 0.0f;
    set_stance(kEyeHeightStand);
    return true;
}

void ParkourController::update_vault(const InputFrame& input, float dt, const LevelScene& scene) {
    (void)input;
    const float t = m_state_timer;
    Vec3 target;
    if (t < m_path_t1) {
        target = m_path_p0 + (m_path_p1 - m_path_p0) * (t / std::max(m_path_t1, 1e-4f));
    } else if (t < m_path_t1 + m_path_t2) {
        target = m_path_p1 + (m_path_p2 - m_path_p1) * ((t - m_path_t1) / std::max(m_path_t2, 1e-4f));
    } else {
        // End of the animation: the pawn carries on with the move's exit velocity.
        m_telemetry.position = m_path_p2;
        m_telemetry.velocity = m_path_exit_velocity;
        const EMovement end = m_path_end_move;
        if (end == EMovement::MOVE_Jump) {
            // Springboard launch.
            m_last_jump_location = m_telemetry.position;
            leave_ground(EMovement::MOVE_Jump);
            m_takeoff_move = EMovement::MOVE_SpringBoarding;
        } else if (end == EMovement::MOVE_Walking) {
            m_telemetry.move_state = EMovement::MOVE_Walking;
            m_telemetry.grounded = true;
            m_sprint_energy = std::max(0.0f, m_telemetry.velocity.length_xy() - m_config.speed_max_base_velocity);
        } else {
            // Over: a normal landing on the floor beyond (or a fall if there was none).
            m_telemetry.grounded = false;
            m_telemetry.move_state = EMovement::MOVE_Falling;
            m_takeoff_move = EMovement::MOVE_SpeedVaulting;
            m_telemetry.velocity.z = -50.0f;
        }
        m_state_timer = 0.0f;
        return;
    }
    m_telemetry.velocity = (target - m_telemetry.position) / std::max(dt, 1e-5f);
    m_telemetry.position = target;
    if (m_telemetry.position.z > m_fall_peak_z) m_fall_peak_z = m_telemetry.position.z;
    (void)scene;
}

// -----------------------------------------------------------------------------
// Zipline, Swing Bar & Balance Beam Subsystems
// -----------------------------------------------------------------------------
bool ParkourController::try_initiate_zipline(const LevelScene& scene) {
    // The hands reach up to about 200 above the feet; the pawn hangs with them on the handle.
    constexpr float kHandHeight = 200.0f;
    for (const auto& act : scene.actors) {
        if (act.is_zipline && act.end_point.length_sq() > 1.0f) {
            Vec3 start = act.location;
            Vec3 end = act.end_point;
            float line_len = (end - start).length();
            if (line_len < 50.0f) continue;
            Vec3 line_dir = (end - start) / line_len;
            Vec3 to_player = m_telemetry.position - start;
            float t = to_player.dot(line_dir);

            if (t >= -60.0f && t <= line_len - 100.0f) {
                Vec3 closest_pt = start + line_dir * std::max(0.0f, t);
                if (closest_pt.distance(m_telemetry.position + Vec3(0, 0, kHandHeight)) < 135.0f) {
                    m_telemetry.move_state = EMovement::MOVE_ZipLine;
                    m_zipline_start = start;
                    m_zipline_end = end;
                    m_telemetry.position = closest_pt - Vec3(0.0f, 0.0f, kHandHeight);
                    m_telemetry.velocity = line_dir * 350.0f;
                    m_telemetry.grounded = false;
                    m_base_actor = -1;
                    m_coil_timer = 0.0f;
                    set_stance(kEyeHeightStand);
                    return true;
                }
            }
        }
    }
    return false;
}

void ParkourController::update_zipline(const InputFrame& input, float dt, const LevelScene& scene) {
    (void)input;
    (void)scene;
    constexpr float kHandHeight = 200.0f;
    const Vec3 zip_vec = m_zipline_end - m_zipline_start;
    const float line_len = zip_vec.length();
    const Vec3 zip_dir = line_len > 1e-4f ? zip_vec / line_len : Vec3(1.0f, 0.0f, 0.0f);

    // Detach with jump or crouch (TdMove_ZipLine.HandleMoveAction)
    if (jump_pressed()) {
        consume_jump();
        m_telemetry.velocity = zip_dir * 500.0f + Vec3(0, 0, 250.0f);
        m_last_jump_location = m_telemetry.position;
        leave_ground(EMovement::MOVE_Jump);
        return;
    }
    if (m_crouch_pressed) {
        m_crouch_pressed = false;
        m_telemetry.velocity = Vec3(zip_dir.x * 420.0f, zip_dir.y * 420.0f, m_telemetry.velocity.z);
        leave_ground(EMovement::MOVE_Falling);
        return;
    }

    // Accelerate down zipline
    float spd = m_telemetry.velocity.length();
    spd = std::min(850.0f, spd + 450.0f * dt);
    m_telemetry.velocity = zip_dir * spd;
    m_telemetry.position += m_telemetry.velocity * dt;

    const Vec3 hand_pos = m_telemetry.position + Vec3(0.0f, 0.0f, kHandHeight);
    const float along_dist = (hand_pos - m_zipline_start).dot(zip_dir);
    if (along_dist >= line_len - 90.0f || hand_pos.distance(m_zipline_end) < 90.0f) {
        m_telemetry.velocity = Vec3(zip_dir.x * 320.0f, zip_dir.y * 320.0f, 0.0f);
        leave_ground(EMovement::MOVE_Falling);
    }
}

void ParkourController::update_swing_bar(const InputFrame& input, float dt, const LevelScene& scene) {
    (void)input;
    (void)scene;
    m_swing_angular_vel += -std::sin(m_swing_angle) * 8.0f * dt;
    m_swing_angle += m_swing_angular_vel * dt;

    if (jump_pressed()) {
        consume_jump();
        Vec3 fwd = facing_forward();
        m_telemetry.velocity = fwd * 600.0f + Vec3(0, 0, 300.0f);
        m_last_jump_location = m_telemetry.position;
        leave_ground(EMovement::MOVE_Jump);
        return;
    }
}

void ParkourController::update_balance(const InputFrame& input, float dt, const LevelScene& scene) {
    (void)scene;
    // Walking on balance beam caps speed at 0.34x
    float target_speed = m_config.run_speed * 0.34f;
    Vec3 fwd = facing_forward();
    m_telemetry.velocity = fwd * (input.forward * target_speed);
    m_telemetry.position += m_telemetry.velocity * dt;

    if (jump_pressed()) {
        consume_jump();
        start_jump(scene);
    }
}

// -----------------------------------------------------------------------------
// Landing moves: TdMove_SkillRoll, TdMove_Landing (LandHard), TdMove_LayOnGround
// -----------------------------------------------------------------------------
void ParkourController::update_landing_moves(const InputFrame& input, float dt, const LevelScene& scene) {
    const MovementConfig& c = m_config;
    EMovement& st = m_telemetry.move_state;
    m_landing_timer -= dt;

    if (st == EMovement::MOVE_SkillRoll) {
        // The roll carries the landing speed forward (a little friction), crouch height.
        calc_velocity(Vec3(0.0f, 0.0f, 0.0f), dt, 1.0f, c.ground_friction * 0.1f);
        walk_move(horiz(m_telemetry.velocity) * dt, kCrouchHeight, scene);
        const float progress = 1.0f - std::clamp(m_landing_timer / std::max(c.skill_roll_time, 1e-3f), 0.0f, 1.0f);
        set_stance(kEyeHeightCrouch - 40.0f * std::sin(progress * PI));
        if (m_landing_timer <= 0.0f) {
            const bool stand = !input.crouch && has_room(kPawnHeight, scene);
            st = stand ? EMovement::MOVE_Walking : EMovement::MOVE_Crouch;
            set_stance(stand ? kEyeHeightStand : kEyeHeightCrouch);
            m_sprint_energy = std::max(0.0f, m_telemetry.velocity.length_xy() - c.speed_max_base_velocity);
            m_accel_time = 1.0f;
        }
        return;
    }

    // LandHard / LayOnGround: dead stop, input ignored until the animation ends (a jump press gets
    // you up off the ground early once the fall has played out).
    m_telemetry.velocity.x = 0.0f;
    m_telemetry.velocity.y = 0.0f;
    const float total = (st == EMovement::MOVE_LayOnGround) ? c.lay_on_ground_time : c.hard_landing_time;
    const float progress = 1.0f - std::clamp(m_landing_timer / std::max(total, 1e-3f), 0.0f, 1.0f);
    const float low = (st == EMovement::MOVE_LayOnGround) ? kEyeHeightSlide : kEyeHeightCrouch;
    set_stance(low + (kEyeHeightStand - low) * std::clamp((progress - 0.6f) / 0.4f, 0.0f, 1.0f));
    const bool get_up = (st == EMovement::MOVE_LayOnGround && jump_pressed() && progress > 0.5f);
    if (m_landing_timer <= 0.0f || get_up) {
        if (get_up) consume_jump();
        st = EMovement::MOVE_Walking;
        set_stance(kEyeHeightStand);
        m_sprint_energy = 0.0f;
        m_accel_time = 0.0f;
    }
}

// -----------------------------------------------------------------------------
// Combat, Firearms, Ballistics & Disarm Subsystem
// (Reverse-engineered from TdGame.u TdMove_Melee*, TdMove_Disarm, TdWeapon & DefaultWeapons.ini)
// -----------------------------------------------------------------------------
void ParkourController::update_combat_and_weapons(const InputFrame& input, float dt, LevelScene& scene) {
    WeaponState& ws = m_telemetry.weapon;
    ws.fired_this_tick = false;

    if (ws.cooldown > 0.0f) ws.cooldown = std::max(0.0f, ws.cooldown - dt);
    if (ws.fire_anim_timer > 0.0f) ws.fire_anim_timer = std::max(0.0f, ws.fire_anim_timer - dt);
    if (ws.equip_timer > 0.0f) ws.equip_timer = std::max(0.0f, ws.equip_timer - dt);
    if (ws.muzzle_flash_timer > 0.0f) ws.muzzle_flash_timer = std::max(0.0f, ws.muzzle_flash_timer - dt);
    if (m_melee_cooldown > 0.0f) m_melee_cooldown = std::max(0.0f, m_melee_cooldown - dt);
    if (m_melee_combo_reset_timer > 0.0f) {
        m_melee_combo_reset_timer -= dt;
        if (m_melee_combo_reset_timer <= 0.0f) m_melee_combo_index = 0;
    }
    if (m_telemetry.hit_marker_timer > 0.0f) m_telemetry.hit_marker_timer = std::max(0.0f, m_telemetry.hit_marker_timer - dt);
    if (m_telemetry.damage_flash_timer > 0.0f) m_telemetry.damage_flash_timer = std::max(0.0f, m_telemetry.damage_flash_timer - dt);

    // Finish weapon throwaway drop animation
    if (ws.drop_timer > 0.0f) {
        ws.drop_timer -= dt;
        if (ws.drop_timer <= 0.0f) {
            ws.drop_timer = 0.0f;
            ws.equipped = false;
            ws.name = "None";
            ws.display_name = "Unarmed";
            ws.is_heavy = false;
            ws.is_two_handed = false;
        }
    }

    // Update 3D bullet tracers in the level scene
    for (auto it = scene.active_tracers.begin(); it != scene.active_tracers.end();) {
        it->timer -= dt;
        if (it->timer <= 0.0f) {
            it = scene.active_tracers.erase(it);
        } else {
            ++it;
        }
    }

    // Update 3D dropped weapons physics on the ground
    for (auto& dw : scene.dropped_weapons) {
        if (!dw.grounded) {
            dw.velocity.z -= m_config.gravity * dt;
            dw.position += dw.velocity * dt;
            dw.yaw_deg += 180.0f * dt;
            TraceHit ghit = trace_ray(dw.position + Vec3(0, 0, 25.0f), dw.position - Vec3(0, 0, 35.0f), scene);
            if (ghit.hit && dw.position.z <= ghit.point.z + 4.0f) {
                dw.position.z = ghit.point.z + 3.0f;
                dw.velocity = Vec3(0, 0, 0);
                dw.grounded = true;
            } else if (dw.position.z < m_telemetry.position.z - 600.0f) {
                dw.grounded = true;
            }
        }
    }

    // 0A. Cycle through all 11 retail Mirror's Edge weapons (`T` / `Y` / MouseWheel)
    static const char* kAllWeapons[11] = {
        "Colt1911", "Glock18", "BerettaM93R", "SteyrTMP", "MP5K",
        "G36C", "FNSCARL", "Remington870", "Neostead", "FNMinimi", "M95"
    };
    if (input.cycle_weapon_dir != 0) {
        int next_idx = m_weapon_cycle_index + input.cycle_weapon_dir;
        if (next_idx < 0) next_idx = 10;
        if (next_idx >= 11) next_idx = 0;
        equip_weapon(kAllWeapons[next_idx]);
        m_telemetry.active_subtitle = "Equipped: " + m_telemetry.weapon.display_name +
                                      " (" + std::to_string(m_telemetry.weapon.ammo) + " RDS)";
    }

    // 0B. Spawn KrugerSec Combat Squad ahead of Faith (`H` key) for live combat testing
    if (input.spawn_combat_squad) {
        Vec3 fwd = Rotator::from_degrees(0.0f, m_telemetry.yaw_deg, 0.0f).forward();
        Vec3 right = Rotator::from_degrees(0.0f, m_telemetry.yaw_deg, 0.0f).right();
        static const char* kSquadWeapons[4] = {"G36C", "Remington870", "MP5K", "Colt1911"};
        static const char* kSquadArchetypes[4] = {"Assault_SWAT", "Support_Shotgun", "PatrolCop_SMG", "PatrolCop"};
        for (int i = 0; i < 3; ++i) {
            EnemyBot guard{};
            guard.archetype = kSquadArchetypes[i % 4];
            guard.weapon_name = kSquadWeapons[(i + static_cast<int>(m_telemetry.tick)) % 4];
            float lateral = (i - 1) * 140.0f;
            Vec3 spawn_pt = m_telemetry.position + fwd * (420.0f + i * 90.0f) + right * lateral;
            TraceHit f_hit = trace_ray(spawn_pt + Vec3(0, 0, 120.0f), spawn_pt - Vec3(0, 0, 220.0f), scene);
            if (f_hit.hit) spawn_pt.z = f_hit.point.z;
            guard.position = spawn_pt;
            guard.home_position = spawn_pt;
            guard.yaw_deg = m_telemetry.yaw_deg + 180.0f;
            guard.health = 100.0f;
            guard.max_health = 100.0f;
            guard.alive = true;
            guard.disarm_window = (i == 0);
            guard.anim_state = EEnemyAnimState::AimFire;
            scene.enemies.push_back(guard);
        }
        m_telemetry.active_subtitle = "KrugerSec Tactical Squad Deployed Ahead!";
    }

    // 0C. Manual Weapon Drop / Throwaway (`G` / `Backspace` or Right-Click when no disarm target is in range)
    auto drop_current_weapon = [&](const std::string& reason) {
        if (!ws.equipped || ws.drop_timer > 0.0f) return;
        DroppedWeapon dw{};
        dw.weapon_name = ws.name;
        dw.ammo = ws.ammo;
        Vec3 fwd = Rotator::from_degrees(0.0f, m_telemetry.yaw_deg, 0.0f).forward();
        Vec3 right = Rotator::from_degrees(0.0f, m_telemetry.yaw_deg, 0.0f).right();
        dw.position = m_telemetry.position + Vec3(0, 0, 55.0f) + fwd * 32.0f + right * 14.0f;
        dw.velocity = fwd * 220.0f + right * 45.0f + Vec3(0, 0, 90.0f);
        dw.yaw_deg = m_telemetry.yaw_deg + 35.0f;
        dw.grounded = false;
        scene.dropped_weapons.push_back(dw);

        ws.drop_timer = 0.25f; // plays 1P `throwaway` animation before clearing `ws.equipped`
        ws.is_heavy = false;
        ws.mobility_scale = 1.0f;
        m_telemetry.active_subtitle = reason;
    };

    if (input.drop_weapon && ws.equipped) {
        drop_current_weapon("Dropped " + ws.display_name);
    }

    // 0D. Pick up a DroppedWeapon from the ground when pressing Use (`E`) or Disarm while unarmed
    if (!ws.equipped && (input.use || input.disarm)) {
        for (auto it = scene.dropped_weapons.begin(); it != scene.dropped_weapons.end(); ++it) {
            if (it->ammo > 0 && m_telemetry.position.distance(it->position) < 135.0f) {
                std::string picked_name = it->weapon_name;
                int picked_ammo = it->ammo;
                scene.dropped_weapons.erase(it);
                equip_weapon(picked_name);
                ws.ammo = picked_ammo;
                m_telemetry.active_subtitle = "Picked up " + ws.display_name + " (" + std::to_string(ws.ammo) + " RDS)";
                break;
            }
        }
    }

    // Check if any nearby enemy is currently in a Disarmable state (or can be stealth-snatched from behind)
    m_telemetry.disarm_prompt_visible = false;
    EnemyBot* disarm_candidate = nullptr;
    bool candidate_from_back = false;
    for (auto& bot : scene.enemies) {
        if (!bot.alive || bot.weapon_name == "None" || bot.weapon_name.empty()) continue;
        float dist = m_telemetry.position.distance(bot.position);
        if (dist < 210.0f) {
            Vec3 bot_fwd(std::cos(bot.yaw_deg * DEG2RAD), std::sin(bot.yaw_deg * DEG2RAD), 0.0f);
            Vec3 bot_to_player = (m_telemetry.position - bot.position).normalized_xy();
            bool behind_enemy = (bot_fwd.dot(bot_to_player) < -0.25f);
            if (bot.disarm_window || bot.stunned || behind_enemy) {
                m_telemetry.disarm_prompt_visible = true;
                disarm_candidate = &bot;
                candidate_from_back = behind_enemy;
                break;
            }
        }
    }

    // 1. Weapon Disarm QTE (`input.disarm` -> `TdMove_Disarm` / `MOVE_Snatch`: `SnatchFwd` or `SnatchBack`)
    if (input.disarm) {
        if (disarm_candidate != nullptr) {
            EnemyBot& bot = *disarm_candidate;
            m_telemetry.move_state = EMovement::MOVE_Snatch;
            m_state_timer = 0.0f;
            m_telemetry.combat_anim_time = 0.0f;
            m_telemetry.combat_anim_duration = 0.68f;
            m_telemetry.snatch_from_back = candidate_from_back;
            m_telemetry.hit_marker_timer = 0.35f;

            // Orient Faith toward the enemy being disarmed
            Vec3 to_bot = (bot.position - m_telemetry.position).normalized_xy();
            if (to_bot.length_sq() > 1e-4f) {
                m_telemetry.yaw_deg = std::atan2(to_bot.y, to_bot.x) * RAD2DEG;
            }

            std::string snatched_wep = bot.weapon_name;
            bot.disarm_window = false;
            bot.stunned = true;
            bot.attack_timer = 0.0f;
            bot.anim_timer = 0.0f;
            bot.active_anim_seq = candidate_from_back ? "SnatchBack" : "SnatchFwd";

            bool is_trainer = (bot.archetype.find("TutorialTrainer") != std::string::npos ||
                               bot.archetype.find("Celeste") != std::string::npos);
            if (is_trainer) {
                // Celeste sparring partner gets disarmed and recovers after the drill
                bot.anim_state = EEnemyAnimState::BeingDisarmed;
                bot.health = std::max(25.0f, bot.health - 25.0f);
            } else {
                // Standard KrugerSec guard is knocked out cold by Faith's disarm takedown!
                bot.health = 0.0f;
                bot.alive = false;
                bot.weapon_name = "None";
                bot.anim_state = EEnemyAnimState::KnockedOut;
            }

            equip_weapon(snatched_wep);
            m_telemetry.active_subtitle = std::string(candidate_from_back ? "Stealth Disarm (" : "Weapon Disarmed (") +
                                          ws.display_name + ")!";
        } else if (ws.equipped && ws.drop_timer <= 0.0f && !input.use) {
            // In Mirror's Edge, pressing the Disarm/Secondary button while holding a gun with no enemy in range tosses the gun
            drop_current_weapon("Tossed " + ws.display_name);
        }
    }

    // 2. Firearm Shooting & Ballistics (`input.fire`)
    if (!input.fire) {
        ws.trigger_released = true;
    }

    auto fire_single_shot = [&]() {
        if (ws.ammo <= 0) return;
        ws.ammo--;
        ws.fired_this_tick = true;
        ws.fire_anim_timer = std::min(0.32f, std::max(0.14f, ws.fire_interval));
        ws.muzzle_flash_timer = 0.065f;

        Rotator view_rot = Rotator::from_degrees(m_telemetry.pitch_deg, m_telemetry.yaw_deg, 0.0f);
        Vec3 fwd = view_rot.forward();
        Vec3 right = view_rot.right();
        Vec3 up = view_rot.up();
        Vec3 eye = m_telemetry.position + Vec3(0, 0, m_telemetry.eye_height);
        Vec3 muzzle_world = eye + fwd * 36.0f + right * 11.0f - up * 9.0f;

        int pellets = std::max(1, ws.pellet_count);
        bool any_hit = false;
        bool any_kill = false;

        for (int p = 0; p < pellets; ++p) {
            // Deterministic golden-ratio spiral cone spread for multi-pellet shotguns and automatic fire
            float spread = ws.spread_rad * (m_telemetry.reaction_active ? 0.45f : 1.0f);
            float angle = static_cast<float>(p) * 2.3999632f + static_cast<float>(m_telemetry.tick) * 0.71f;
            float radius = (pellets > 1)
                ? spread * std::sqrt((static_cast<float>(p) + 0.5f) / static_cast<float>(pellets))
                : spread * 0.35f * std::sin(static_cast<float>(m_telemetry.tick) * 1.7f);
            Vec3 ray_dir = (fwd + right * ( std::cos(angle) * radius ) + up * ( std::sin(angle) * radius )).normalized();
            Vec3 ray_end = eye + ray_dir * ws.range;

            TraceHit wall_hit = trace_ray(eye, ray_end, scene, COLL_BlockZeroExtent);
            float max_dist = wall_hit.hit ? eye.distance(wall_hit.point) : ws.range;
            Vec3 tracer_end = wall_hit.hit ? wall_hit.point : (eye + ray_dir * std::min(ws.range, 2500.0f));

            // Ray-Capsule intersection against living enemies (Headshot & Torso hitboxes)
            EnemyBot* hit_bot = nullptr;
            float best_bot_dist = max_dist;
            bool is_headshot = false;

            for (auto& bot : scene.enemies) {
                if (!bot.alive) continue;
                Vec3 bot_center = bot.position + Vec3(0, 0, 52.0f);
                Vec3 to_bot = bot_center - eye;
                float proj = to_bot.dot(ray_dir);
                if (proj > 0.0f && proj < best_bot_dist) {
                    Vec3 closest = eye + ray_dir * proj;
                    float dist_xy = closest.distance_xy(bot.position);
                    float rel_z = closest.z - bot.position.z;
                    if (dist_xy < 48.0f && rel_z >= -10.0f && rel_z <= 105.0f) {
                        best_bot_dist = proj;
                        hit_bot = &bot;
                        is_headshot = (rel_z >= 72.0f);
                        tracer_end = closest;
                    }
                }
            }

            if (hit_bot != nullptr) {
                any_hit = true;
                // Distance damage falloff from DefaultWeapons.ini
                float t_falloff = 0.0f;
                if (best_bot_dist > ws.falloff_start && ws.falloff_end > ws.falloff_start) {
                    t_falloff = std::clamp((best_bot_dist - ws.falloff_start) / (ws.falloff_end - ws.falloff_start), 0.0f, 1.0f);
                }
                float dmg = ws.damage + (ws.damage_far - ws.damage) * t_falloff;
                if (is_headshot) dmg *= 2.0f;

                hit_bot->health -= dmg;
                hit_bot->stunned = true;
                hit_bot->attack_timer = 0.0f;
                hit_bot->anim_timer = 0.0f;
                hit_bot->active_anim_seq = (p % 2 == 0) ? "HitMeleeRight" : "HitMeleeLeft";

                if (hit_bot->health <= 0.0f) {
                    bool is_trainer = (hit_bot->archetype.find("TutorialTrainer") != std::string::npos ||
                                       hit_bot->archetype.find("Celeste") != std::string::npos);
                    if (is_trainer) {
                        hit_bot->health = 100.0f;
                        hit_bot->anim_state = EEnemyAnimState::HitStagger;
                    } else {
                        hit_bot->alive = false;
                        hit_bot->anim_state = EEnemyAnimState::KnockedOut;
                        any_kill = true;
                        // Drop the enemy's weapon onto the ground if they had one
                        if (!hit_bot->weapon_name.empty() && hit_bot->weapon_name != "None") {
                            DroppedWeapon dw{};
                            dw.weapon_name = hit_bot->weapon_name;
                            dw.ammo = 15;
                            dw.position = hit_bot->position + Vec3(0, 0, 45.0f);
                            dw.velocity = ray_dir * 90.0f + Vec3(0, 0, 80.0f);
                            dw.yaw_deg = hit_bot->yaw_deg + 45.0f;
                            dw.grounded = false;
                            scene.dropped_weapons.push_back(dw);
                            hit_bot->weapon_name = "None";
                        }
                    }
                } else {
                    hit_bot->anim_state = EEnemyAnimState::HitStagger;
                }
            }

            // Spawn 3D BulletTracer in world
            BulletTracer tr{};
            tr.start_pos = muzzle_world;
            tr.end_pos = tracer_end;
            tr.timer = 0.09f;
            tr.max_time = 0.09f;
            tr.hit_enemy = (hit_bot != nullptr);
            tr.from_player = true;
            scene.active_tracers.push_back(tr);
        }

        if (any_hit) {
            m_telemetry.hit_marker_timer = 0.22f;
            if (any_kill) {
                m_telemetry.active_subtitle = "Target Neutralized (" + ws.display_name + ")";
            }
        }

        // Apply authentic camera recoil kick (TdSkelControlRecoil + view pitch kick)
        m_telemetry.pitch_deg = std::clamp(m_telemetry.pitch_deg + ws.recoil_pitch_deg * 0.45f, -85.0f, 85.0f);
        float yaw_jitter = ((m_telemetry.tick % 2 == 0) ? 1.0f : -1.0f) * ws.recoil_pitch_deg * 0.12f;
        m_telemetry.yaw_deg += yaw_jitter;
    };

    // Continue active 3-round burst for Beretta M93R
    if (ws.equipped && ws.drop_timer <= 0.0f && ws.burst_remaining > 0 && ws.cooldown <= 0.0f) {
        if (ws.ammo > 0) {
            ws.burst_remaining--;
            fire_single_shot();
            ws.cooldown = (ws.burst_remaining > 0) ? ws.fire_interval : 0.24f;
        } else {
            ws.burst_remaining = 0;
        }
    } else if (input.fire && ws.equipped && ws.drop_timer <= 0.0f && ws.cooldown <= 0.0f) {
        bool can_pull = (ws.fire_mode == EWeaponFireMode::FullAuto) || ws.trigger_released;
        if (can_pull) {
            ws.trigger_released = false;
            ws.equip_timer = 0.0f;
            if (ws.ammo > 0) {
                if (ws.fire_mode == EWeaponFireMode::Burst3) {
                    ws.burst_remaining = std::min(2, ws.ammo - 1);
                }
                fire_single_shot();
                ws.cooldown = ws.fire_interval;
            } else {
                // Empty magazine: play `standfireempty` click and toss empty weapon (`throwaway`)
                drop_current_weapon(ws.display_name + " Empty - Tossed");
            }
        }
    }

    // 3. Wallrun Kick Hit Detection (while airborne in MOVE_MeleeWallrun)
    if (m_telemetry.move_state == EMovement::MOVE_MeleeWallrun && m_state_timer < 0.35f) {
        for (auto& bot : scene.enemies) {
            if (bot.alive && m_telemetry.position.distance(bot.position) < 195.0f) {
                bot.health -= 90.0f;
                bot.stunned = true;
                bot.disarm_window = true;
                bot.attack_timer = 0.0f;
                bot.anim_timer = 0.0f;
                bot.active_anim_seq = "HitMeleeWallrunRight";
                m_telemetry.melee_hit_confirmed = true;
                m_telemetry.hit_marker_timer = 0.28f;
                if (bot.health <= 0.0f) {
                    bot.alive = false;
                    bot.anim_state = EEnemyAnimState::KnockedOut;
                    m_telemetry.active_subtitle = "Wallrun Kick Knockout!";
                } else {
                    bot.anim_state = EEnemyAnimState::HitStagger;
                }
            }
        }
    }

    // 4. Context-Sensitive Unarmed Melee Strikes (`input.melee`: Combo Punch/Kick, Crouch Uppercut, Jump Kick, Barge)
    if (input.melee && m_melee_cooldown <= 0.0f && (!ws.equipped || ws.drop_timer > 0.0f) &&
        m_telemetry.move_state != EMovement::MOVE_MeleeSlide &&
        m_telemetry.move_state != EMovement::MOVE_MeleeWallrun) {

        Vec3 fwd = Rotator::from_degrees(0.0f, m_telemetry.yaw_deg, 0.0f).forward();
        m_telemetry.melee_hit_confirmed = false;

        if (!m_telemetry.grounded) {
            // Airborne Flying Jump Kick (`TdMove_MeleeAir`: `JumpKickStart` -> `JumpKickEnd`)
            m_telemetry.move_state = EMovement::MOVE_MeleeAir;
            m_state_timer = 0.0f;
            m_telemetry.combat_anim_time = 0.0f;
            m_telemetry.combat_anim_duration = 0.62f;
            m_melee_cooldown = 0.62f;

            // Lunge forward in mid-air toward target
            m_telemetry.velocity += fwd * 140.0f + Vec3(0, 0, 60.0f);
        } else if (m_telemetry.move_state == EMovement::MOVE_Crouch || input.crouch) {
            // Crouch Uppercut (`MeleeCrouchHitUppercut`)
            m_telemetry.move_state = EMovement::MOVE_Melee;
            m_telemetry.melee_variant = 3;
            m_state_timer = 0.0f;
            m_telemetry.combat_anim_time = 0.0f;
            m_telemetry.combat_anim_duration = 0.48f;
            m_melee_cooldown = 0.48f;
            m_telemetry.velocity += fwd * 160.0f;
        } else {
            // Standing 3-Hit Combo (`MeleeStartRight` -> `MeleeHitLeft` -> `MeleeStartKick`)
            m_telemetry.move_state = EMovement::MOVE_Melee;
            m_telemetry.melee_variant = m_melee_combo_index % 3;
            m_melee_combo_index = (m_melee_combo_index + 1) % 3;
            m_melee_combo_reset_timer = 1.35f;

            float dur = (m_telemetry.melee_variant == 2) ? 0.56f : 0.42f;
            m_state_timer = 0.0f;
            m_telemetry.combat_anim_time = 0.0f;
            m_telemetry.combat_anim_duration = dur;
            m_melee_cooldown = dur * 0.90f;
            m_telemetry.velocity += fwd * ((m_telemetry.melee_variant == 2) ? 220.0f : 140.0f);
        }

        // Melee Hit Detection & Target Magnetism (from DefaultAIMeleeAttacks.ini)
        for (auto& bot : scene.enemies) {
            if (!bot.alive) continue;
            float reach = (m_telemetry.move_state == EMovement::MOVE_MeleeAir) ? 225.0f : 195.0f;
            float dist = m_telemetry.position.distance(bot.position);
            if (dist < reach) {
                Vec3 dir_to_bot = (bot.position - m_telemetry.position).normalized_xy();
                float dot = fwd.dot(dir_to_bot);
                if (dot > 0.35f || dist < 95.0f) {
                    m_telemetry.melee_hit_confirmed = true;
                    m_telemetry.hit_marker_timer = 0.25f;

                    float dmg = 34.0f;
                    const char* hit_seq = "HitMeleeRight";
                    if (m_telemetry.move_state == EMovement::MOVE_MeleeAir) {
                        dmg = 100.0f;
                        hit_seq = "HitMeleeInAir_High";
                        // Bounce off enemy chest slightly after landing a flying jump kick
                        m_telemetry.velocity = -dir_to_bot * 160.0f + Vec3(0, 0, 210.0f);
                    } else if (m_telemetry.melee_variant == 3) {
                        dmg = 50.0f;
                        hit_seq = "HitMeleeCrouchSweep";
                    } else if (m_telemetry.melee_variant == 2) {
                        dmg = 55.0f;
                        hit_seq = "HitMeleeSoccerKick";
                    } else if (m_telemetry.melee_variant == 1) {
                        dmg = 38.0f;
                        hit_seq = "HitMeleeLeft";
                    }

                    bot.health -= dmg;
                    bot.stunned = true;
                    bot.disarm_window = true; // Staggering an enemy opens their red disarm window!
                    bot.attack_timer = 0.0f;
                    bot.anim_timer = 0.0f;
                    bot.active_anim_seq = hit_seq;

                    bool is_trainer = (bot.archetype.find("TutorialTrainer") != std::string::npos ||
                                       bot.archetype.find("Celeste") != std::string::npos);
                    if (bot.health <= 0.0f) {
                        if (is_trainer) {
                            bot.health = 100.0f;
                            bot.anim_state = EEnemyAnimState::HitStagger;
                        } else {
                            bot.alive = false;
                            bot.anim_state = EEnemyAnimState::KnockedOut;
                            if (!bot.weapon_name.empty() && bot.weapon_name != "None") {
                                DroppedWeapon dw{};
                                dw.weapon_name = bot.weapon_name;
                                dw.ammo = 15;
                                dw.position = bot.position + Vec3(0, 0, 45.0f);
                                dw.velocity = dir_to_bot * 110.0f + Vec3(0, 0, 90.0f);
                                dw.yaw_deg = bot.yaw_deg + 30.0f;
                                dw.grounded = false;
                                scene.dropped_weapons.push_back(dw);
                                bot.weapon_name = "None";
                            }
                            m_telemetry.active_subtitle = (m_telemetry.move_state == EMovement::MOVE_MeleeAir)
                                ? "Flying Jump Kick Knockout!"
                                : "Melee Combo Knockout!";
                        }
                    } else {
                        bot.anim_state = EEnemyAnimState::HitStagger;
                        if (m_telemetry.active_subtitle.empty()) {
                            m_telemetry.active_subtitle = "Enemy Staggered - Weapon Red (Press Right-Click / E to Disarm)";
                        }
                    }
                }
            }
        }
    }
}

// -----------------------------------------------------------------------------
// AI Bots Pursuit, Ranged Fire, Melee Windup & Disarm Window Simulation
// -----------------------------------------------------------------------------
void ParkourController::update_ai_bots(float dt, LevelScene& scene) {
    for (auto& bot : scene.enemies) {
        bot.anim_timer += dt;
        if (bot.muzzle_flash_timer > 0.0f) {
            bot.muzzle_flash_timer = std::max(0.0f, bot.muzzle_flash_timer - dt);
        }

        if (!bot.alive) {
            bot.anim_state = EEnemyAnimState::KnockedOut;
            bot.disarm_window = false;
            continue;
        }

        if (bot.home_position.length_sq() < 1e-3f) {
            bot.home_position = bot.position;
        }

        bool is_trainer = (bot.archetype.find("TutorialTrainer") != std::string::npos ||
                           bot.archetype.find("Celeste") != std::string::npos ||
                           bot.weapon_name.find("TutorialTrainer") != std::string::npos);

        // Handle stunned / staggered / disarmed recovery
        if (bot.stunned) {
            bot.attack_timer += dt;
            float stun_dur = is_trainer ? 2.2f : 1.6f;
            if (bot.attack_timer >= stun_dur) {
                bot.stunned = false;
                bot.attack_timer = 0.0f;
                bot.anim_timer = 0.0f;
                bot.anim_state = is_trainer ? EEnemyAnimState::MeleeWindup : EEnemyAnimState::AimFire;
                if (is_trainer && (bot.weapon_name.empty() || bot.weapon_name == "None")) {
                    bot.weapon_name = "Colt1911";
                }
            }
            continue;
        }

        float dist = bot.position.distance(m_telemetry.position);
        Vec3 dir_to_player = (m_telemetry.position - bot.position).normalized_xy();
        if (dir_to_player.length_sq() > 1e-4f) {
            bot.yaw_deg = std::atan2(dir_to_player.y, dir_to_player.x) * RAD2DEG;
        }

        bot.attack_timer += dt;

        // Tutorial sparring trainer (Celeste) stays at her training post with disarm window ready
        if (is_trainer) {
            bot.disarm_window = true;
            if (bot.weapon_name.empty() || bot.weapon_name == "None") {
                bot.weapon_name = "Colt1911";
            }
            bot.anim_state = (dist < 260.0f) ? EEnemyAnimState::MeleeWindup : EEnemyAnimState::Idle;
            continue;
        }

        if (dist > 1800.0f) {
            // Patrol / Guard Idle outside engagement radius
            bot.disarm_window = false;
            bot.anim_state = EEnemyAnimState::Idle;
        } else if (dist > 220.0f) {
            // Ranged engagement or closing distance
            bot.disarm_window = false;

            if (dist > 750.0f && bot.position.distance_xy(bot.home_position) < 450.0f) {
                // Advance toward Faith (`RunFwd`)
                bot.anim_state = EEnemyAnimState::Chase;
                Vec3 step_move = dir_to_player * (220.0f * dt);
                TraceHit wall_chk = trace_ray(bot.position + Vec3(0, 0, 50.0f),
                                              bot.position + Vec3(0, 0, 50.0f) + dir_to_player * 55.0f, scene);
                if (!wall_chk.hit) {
                    bot.position += step_move;
                }
            } else {
                bot.anim_state = EEnemyAnimState::AimFire;
            }

            // Fire weapon bursts at Faith
            float fire_cadence = (bot.weapon_name.find("Remington") != std::string::npos ||
                                  bot.weapon_name.find("Neostead") != std::string::npos) ? 1.45f : 0.95f;
            if (bot.attack_timer >= fire_cadence) {
                bot.attack_timer = 0.0f;
                bot.muzzle_flash_timer = 0.08f;

                Vec3 bot_muzzle = bot.position + Vec3(0, 0, 62.0f) + dir_to_player * 35.0f;
                Vec3 target_pt = m_telemetry.position + Vec3(0, 0, m_telemetry.eye_height * 0.75f);

                // Check line of sight so enemies don't shoot through solid walls
                TraceHit los = trace_ray(bot_muzzle, target_pt, scene, COLL_BlockZeroExtent);
                Vec3 tracer_end = los.hit ? los.point : target_pt;

                BulletTracer tr{};
                tr.start_pos = bot_muzzle;
                tr.end_pos = tracer_end;
                tr.timer = 0.085f;
                tr.max_time = 0.085f;
                tr.hit_enemy = false;
                tr.from_player = false;
                scene.active_tracers.push_back(tr);

                // Deal damage if line-of-sight is clear and Faith isn't actively evading
                bool evading = (m_telemetry.move_state == EMovement::MOVE_Slide ||
                                m_telemetry.move_state == EMovement::MOVE_MeleeSlide ||
                                m_telemetry.move_state == EMovement::MOVE_SkillRoll ||
                                m_telemetry.move_state == EMovement::MOVE_WallRunningLeft ||
                                m_telemetry.move_state == EMovement::MOVE_WallRunningRight ||
                                m_telemetry.move_state == EMovement::MOVE_Snatch ||
                                m_telemetry.speed_2d > 540.0f);
                if (!los.hit && !evading) {
                    m_telemetry.health = std::max(0.0f, m_telemetry.health - 10.0f);
                    m_telemetry.damage_flash_timer = 0.25f;
                    m_damage_cooldown = m_config.health_regen_delay;
                }
            }
        } else {
            // Close-quarters melee range (< 220 units): wind up rifle/pistol butt strike and open Red Disarm Window!
            if (bot.attack_timer < 0.45f) {
                bot.disarm_window = false;
                bot.anim_state = EEnemyAnimState::AimFire;
            } else if (bot.attack_timer <= 1.45f) {
                if (!bot.disarm_window) {
                    bot.anim_timer = 0.0f;
                }
                bot.disarm_window = true;
                bot.anim_state = EEnemyAnimState::MeleeWindup;
            } else {
                bot.disarm_window = false;
                bot.attack_timer = 0.0f;
                bot.anim_timer = 0.0f;
                bot.anim_state = EEnemyAnimState::MeleeStrike;
                if (m_telemetry.move_state != EMovement::MOVE_Snatch) {
                    m_telemetry.health = std::max(0.0f, m_telemetry.health - 22.0f);
                    m_telemetry.damage_flash_timer = 0.35f;
                    m_damage_cooldown = m_config.health_regen_delay;
                }
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Health Regeneration Subsystem
// -----------------------------------------------------------------------------
void ParkourController::update_health_and_regen(float dt) {
    if (m_damage_cooldown > 0.0f) {
        m_damage_cooldown -= dt;
    } else if (m_telemetry.health < 100.0f && m_telemetry.health > 0.0f) {
        m_telemetry.health = std::min(100.0f, m_telemetry.health + m_config.health_regen_rate * dt);
    }
}

// -----------------------------------------------------------------------------
// Checkpoints, Kill Volumes & Collectibles Subsystem
// -----------------------------------------------------------------------------
void ParkourController::update_checkpoints_and_volumes(LevelScene& scene) {
    // 1. Fall Death (WorldInfo.KillZ, or a lethal drop below the last checkpoint) -> Respawn
    if (m_telemetry.position.z < scene.kill_z ||
        m_telemetry.position.z < m_last_checkpoint_pos.z - 2200.0f ||
        m_telemetry.health <= 0.0f) {
        reset(m_last_checkpoint_pos, m_last_checkpoint_yaw);
        m_telemetry.active_subtitle = "Respawned at Checkpoint";
        return;
    }

    // 2. Checkpoints & TdCheckpoint.StreamingLevels
    for (size_t i = 0; i < scene.checkpoints.size(); ++i) {
        if (m_telemetry.position.distance(scene.checkpoints[i]) < 240.0f) {
            if (static_cast<int>(i) > m_telemetry.active_checkpoint) {
                m_telemetry.active_checkpoint = static_cast<int>(i);
                m_last_checkpoint_pos = scene.checkpoints[i];
                m_last_checkpoint_yaw = m_telemetry.yaw_deg;
                if (i < scene.checkpoint_infos.size()) {
                    const auto& cp = scene.checkpoint_infos[i];
                    m_telemetry.active_checkpoint_name = cp.checkpoint_name;
                    if (!cp.streaming_levels.empty()) {
                        scene.loaded_sublevel_packages = cp.streaming_levels;
                    }
                    m_telemetry.active_subtitle = "Checkpoint: " + cp.checkpoint_name;
                }
                if (i < scene.subtitles.size() && !scene.subtitles[i].empty()) {
                    m_telemetry.active_subtitle = scene.subtitles[i];
                } else if (i >= scene.checkpoint_infos.size()) {
                    m_telemetry.active_subtitle = "Checkpoint Reached";
                }
            }
        }
    }
    m_telemetry.streamed_sublevel_count = static_cast<int>(scene.loaded_sublevel_packages.size());

    // 3. Courier Bags (`is_bag`)
    for (auto& act : scene.actors) {
        if (act.is_bag && m_telemetry.position.distance(act.location) < 100.0f) {
            act.is_bag = false; // collected
            m_telemetry.bags_collected++;
            m_telemetry.active_subtitle = "Runner Bag Collected! (" + std::to_string(m_telemetry.bags_collected) + ")";
        }
    }
}

// -----------------------------------------------------------------------------
// Interactive Elevator & Mid-Shaft Multi-Level Streaming Subsystem
// (Reverse-engineered from *_Slc.me1 / *_Spt.me1 InterpActor + InterpTrackMove)
// -----------------------------------------------------------------------------
// Drives the real elevator InterpActors: the cab and the actors attached to it follow the cab
// PosTrack, the sliding doors follow their door matinees. A pawn standing on a moving part
// (Pawn.Base) is carried with it, exactly like UE3 based movement.
void ParkourController::update_elevators(const InputFrame& input, float dt, LevelScene& scene) {
    m_telemetry.in_elevator = false;
    m_telemetry.active_elevator_idx = -1;

    for (size_t i = 0; i < scene.elevators.size(); ++i) {
        ElevatorInstance& elev = scene.elevators[i];
        elev.prev_pos = elev.current_pos;

        // Cab interior (S_Elevator_01 mesh bounds) at the cab's current position.
        const Vec3 fc_prev = elev.prev_pos + elev.cab_local_offset;
        const Vec3 he = elev.cab_half_extents;
        const float h_cab = he.z * 2.0f;
        const Vec3& p = m_telemetry.position;
        const bool player_inside = (std::abs(p.x - fc_prev.x) <= he.x - 8.0f) &&
                                   (std::abs(p.y - fc_prev.y) <= he.y - 8.0f) &&
                                   (p.z >= fc_prev.z - 30.0f) &&
                                   (p.z <= fc_prev.z + h_cab + 10.0f);

        const bool button_pressed = input.use && (player_inside || p.distance(elev.button_pos) < 180.0f);

        switch (elev.state) {
            case ElevatorState::IdleStart: {
                elev.door_open_Start = 1.0f;
                elev.door_open_End = 0.0f;
                // Trigger when Faith walks inside the cab and passes its center or presses E on the button
                const bool deep_inside = player_inside &&
                                         (std::abs(p.x - fc_prev.x) <= he.x * 0.65f) &&
                                         (std::abs(p.y - fc_prev.y) <= he.y * 0.65f);
                if ((elev.auto_trigger_on_enter && deep_inside) || button_pressed) {
                    elev.state = ElevatorState::DoorsClosing;
                    elev.timer = 0.0f;
                    m_telemetry.active_subtitle = "Elevator Activated - Doors Closing";
                }
                break;
            }

            case ElevatorState::DoorsClosing: {
                elev.timer += dt;
                const float t01 = std::clamp(elev.timer / std::max(0.1f, elev.door_duration), 0.0f, 1.0f);
                elev.door_open_Start = 1.0f - t01;
                elev.door_open_End = 0.0f;
                if (elev.timer >= elev.door_duration) {
                    elev.door_open_Start = 0.0f;
                    elev.state = ElevatorState::Moving;
                    elev.timer = 0.0f;
                    m_telemetry.active_subtitle = "Elevator In Transit - Streaming Sublevels";
                }
                break;
            }

            case ElevatorState::Moving: {
                elev.timer += dt;
                const float dur = std::max(0.1f, elev.ride_duration);

                // Evaluate UE3 InterpTrackMove PosTrack curve (Hermite smoothstep between keyframes)
                if (elev.keyframes.size() >= 2) {
                    if (elev.timer <= elev.keyframes.front().time) {
                        elev.current_pos = elev.keyframes.front().pos;
                    } else if (elev.timer >= elev.keyframes.back().time) {
                        elev.current_pos = elev.keyframes.back().pos;
                    } else {
                        for (size_t k = 0; k + 1 < elev.keyframes.size(); ++k) {
                            const float t0 = elev.keyframes[k].time;
                            const float t1 = elev.keyframes[k + 1].time;
                            if (elev.timer >= t0 && elev.timer <= t1) {
                                const float seg_u = (t1 > t0 + 1e-5f) ? (elev.timer - t0) / (t1 - t0) : 1.0f;
                                const float s = seg_u * seg_u * (3.0f - 2.0f * seg_u);
                                elev.current_pos = elev.keyframes[k].pos + (elev.keyframes[k + 1].pos - elev.keyframes[k].pos) * s;
                                break;
                            }
                        }
                    }
                } else {
                    const float u = std::clamp(elev.timer / dur, 0.0f, 1.0f);
                    const float s = u * u * (3.0f - 2.0f * u);
                    elev.current_pos = elev.start_pos + (elev.end_pos - elev.start_pos) * s;
                }

                const float progress = std::clamp(elev.timer / dur, 0.0f, 1.0f);

                // Mid-shaft Kismet SeqAct_MultiLevelStreaming / TdCheckpoint.StreamingLevels transition
                if (!elev.streaming_triggered && progress >= 0.5f) {
                    elev.streaming_triggered = true;
                    for (const auto& out_pkg : elev.stream_out_packages) {
                        scene.loaded_sublevel_packages.erase(
                            std::remove(scene.loaded_sublevel_packages.begin(),
                                        scene.loaded_sublevel_packages.end(), out_pkg),
                            scene.loaded_sublevel_packages.end());
                    }
                    for (const auto& in_pkg : elev.stream_in_packages) {
                        if (std::find(scene.loaded_sublevel_packages.begin(),
                                      scene.loaded_sublevel_packages.end(), in_pkg) == scene.loaded_sublevel_packages.end()) {
                            scene.loaded_sublevel_packages.push_back(in_pkg);
                        }
                    }
                    if (elev.target_checkpoint_idx >= 0 &&
                        static_cast<size_t>(elev.target_checkpoint_idx) < scene.checkpoint_infos.size()) {
                        const auto& dst_cp = scene.checkpoint_infos[static_cast<size_t>(elev.target_checkpoint_idx)];
                        if (!dst_cp.streaming_levels.empty()) {
                            scene.loaded_sublevel_packages = dst_cp.streaming_levels;
                        }
                        m_telemetry.active_checkpoint = std::max(m_telemetry.active_checkpoint, elev.target_checkpoint_idx);
                        m_telemetry.active_checkpoint_name = dst_cp.checkpoint_name;
                    }
                    m_last_checkpoint_pos = elev.end_pos + elev.cab_local_offset + Vec3(0.0f, 0.0f, 35.0f);
                    m_telemetry.active_subtitle = "Streamed Sublevels: " +
                        (elev.stream_in_packages.empty() ? elev.name : elev.stream_in_packages.front());
                }

                if (elev.timer >= dur) {
                    elev.current_pos = elev.end_pos;
                    elev.state = ElevatorState::DoorsOpening;
                    elev.timer = 0.0f;
                }
                break;
            }

            case ElevatorState::DoorsOpening: {
                elev.timer += dt;
                const float t01 = std::clamp(elev.timer / std::max(0.1f, elev.door_duration), 0.0f, 1.0f);
                elev.door_open_End = t01;
                if (elev.timer >= elev.door_duration) {
                    elev.door_open_End = 1.0f;
                    elev.state = ElevatorState::IdleEnd;
                    elev.timer = 0.0f;
                    m_telemetry.active_subtitle = "Elevator Arrived - Upper Zone Loaded";
                }
                break;
            }

            case ElevatorState::IdleEnd: {
                elev.door_open_End = 1.0f;
                break;
            }
        }

        // Pose the real InterpActors: the cab (and everything based on it) is displaced along the cab
        // PosTrack; sliding door leaves move along their door-matinee open offsets.
        const Vec3 cab_offset = elev.current_pos - elev.start_pos;
        float cab_doors_open = 0.0f;
        if (elev.state == ElevatorState::IdleStart || elev.state == ElevatorState::DoorsClosing) {
            cab_doors_open = elev.door_open_Start;
        } else if (elev.state == ElevatorState::DoorsOpening || elev.state == ElevatorState::IdleEnd) {
            cab_doors_open = elev.door_open_End;
        }
        for (auto& part : elev.parts) {
            part.prev_offset = part.offset;
            switch (part.role) {
                case ElevatorPartRole::Cab:
                case ElevatorPartRole::CabAttached:
                    part.offset = cab_offset;
                    break;
                case ElevatorPartRole::CabDoor:
                    part.offset = cab_offset + part.door_open_offset * cab_doors_open;
                    break;
                case ElevatorPartRole::StartDoor:
                    part.offset = part.door_open_offset * elev.door_open_Start;
                    break;
                case ElevatorPartRole::EndDoor:
                    part.offset = part.door_open_offset * elev.door_open_End;
                    break;
            }
        }

        // UE3 based movement: a pawn standing on a moving InterpActor rides with it.
        if (m_telemetry.grounded && m_base_actor >= 0) {
            for (const auto& part : elev.parts) {
                if (part.actor_index == m_base_actor) {
                    m_telemetry.position += part.offset - part.prev_offset;
                    break;
                }
            }
        }

        if (player_inside) {
            // Prevent false fall-damage accumulation during downward elevator rides
            m_fall_peak_z = m_telemetry.position.z;
            m_air_fall_start_z = m_telemetry.position.z;

            m_telemetry.in_elevator = true;
            m_telemetry.active_elevator_idx = static_cast<int>(i);
            if (elev.state == ElevatorState::Moving) {
                m_telemetry.elevator_progress = std::clamp(elev.timer / std::max(0.1f, elev.ride_duration), 0.0f, 1.0f);
            } else if (elev.state == ElevatorState::DoorsOpening || elev.state == ElevatorState::IdleEnd) {
                m_telemetry.elevator_progress = 1.0f;
            } else {
                m_telemetry.elevator_progress = 0.0f;
            }
        }
    }

    m_telemetry.streamed_sublevel_count = static_cast<int>(scene.loaded_sublevel_packages.size());
}

} // namespace me

