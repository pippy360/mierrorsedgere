#include "parkour_controller.hpp"
#include <cmath>
#include <algorithm>
#include <iostream>

namespace me {

ParkourController::ParkourController(const MovementConfig& config)
    : m_config(config) {
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
    m_telemetry.fov_deg = 100.0f;
    m_telemetry.eye_height = 84.0f;
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

    m_momentum_timer = 0.0f;
    m_state_timer = 0.0f;
    m_wallrun_timer = 0.0f;
    m_wallrun_cooldown = 0.0f;
    m_wallrun_begin_speed = 0.0f;
    m_slide_timer = 0.0f;
    m_coil_timer = 0.0f;
    m_turn_180_timer = 0.0f;
    m_roll_anim_timer = 0.0f;
    m_damage_cooldown = 0.0f;
    m_air_fall_start_z = spawn_pos.z;
    m_fall_peak_z = spawn_pos.z;
    m_crouch_landing_buffer = 0.0f;
    m_melee_cooldown = 0.0f;
    m_melee_combo_index = 0;
    m_melee_combo_reset_timer = 0.0f;
    m_weapon_cycle_index = -1;
    m_jump_consumed = false;
    m_prev_turn_180 = false;
    m_last_wallrun_normal = Vec3(0.0f, 0.0f, 0.0f);

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

    if (!input.jump) {
        m_jump_consumed = false;
    }

    // 1. Reaction Time Slow-Motion
    float effective_dt = dt;
    update_reaction_time(input, dt, effective_dt);

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
        if (input.crouch) {
            m_crouch_landing_buffer = 0.35f;
        } else {
            m_crouch_landing_buffer = std::max(0.0f, m_crouch_landing_buffer - step_dt);
        }

        // Step interactive elevators (InterpActor + InterpTrackMove) and carry player with cab_delta
        update_elevators(input, step_dt, scene);

        // State Machine Dispatch
        switch (m_telemetry.move_state) {
            case EMovement::MOVE_Walking:
            case EMovement::MOVE_Crouch:
            case EMovement::MOVE_StepUp:
            case EMovement::MOVE_AutoStepUp:
                update_ground_locomotion(input, step_dt, scene);
                break;

            case EMovement::MOVE_Jump:
            case EMovement::MOVE_Falling:
            case EMovement::MOVE_Coil:
            case EMovement::MOVE_WallRunJump:
            case EMovement::MOVE_GrabJump:
            case EMovement::MOVE_WallClimb180TurnJump:
            case EMovement::MOVE_DodgeJump:
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
            case EMovement::MOVE_SoftLanding:
            case EMovement::MOVE_Landing:
                update_skill_roll(input, step_dt);
                break;

            case EMovement::MOVE_SpeedVaulting:
            case EMovement::MOVE_VaultOver:
            case EMovement::MOVE_SpringBoarding:
            case EMovement::MOVE_180Turn:
            case EMovement::MOVE_180TurnInAir:
                if (m_state_timer >= 0.35f) {
                    m_telemetry.move_state = m_telemetry.grounded ? EMovement::MOVE_Walking : EMovement::MOVE_Falling;
                }
                break;

            default:
                m_telemetry.move_state = m_telemetry.grounded ? EMovement::MOVE_Walking : EMovement::MOVE_Falling;
                break;
        }

        // Check if grounded status changed or floor is present
        float floor_z = 0.0f;
        Vec3 floor_norm;
        bool has_floor = check_ground(scene, floor_z, floor_norm);

        if (has_floor && m_telemetry.velocity.z <= 50.0f &&
            m_telemetry.move_state != EMovement::MOVE_Grabbing &&
            m_telemetry.move_state != EMovement::MOVE_ZipLine &&
            m_telemetry.move_state != EMovement::MOVE_Swing &&
            m_telemetry.move_state != EMovement::MOVE_WallClimbing) {

            if (!m_telemetry.grounded) {
                // Landing event from air!
                float fall_dist = std::max(0.0f, m_fall_peak_z - floor_z);

                if (m_crouch_landing_buffer > 0.0f && fall_dist >= m_config.skill_roll_min_fall && fall_dist < m_config.uncontrolled_fall) {
                    // Skill Roll! Convert downward impact into forward sprint momentum
                    m_telemetry.move_state = EMovement::MOVE_SkillRoll;
                    m_state_timer = 0.0f;
                    m_roll_anim_timer = 0.5f;

                    // Preserve or boost horizontal velocity
                    float spd = m_telemetry.velocity.length_xy();
                    spd = std::max(spd, m_config.run_speed);
                    Vec3 fwd = Rotator::from_degrees(0.0f, m_telemetry.yaw_deg, 0.0f).forward();
                    m_telemetry.velocity = fwd * spd;
                    m_telemetry.velocity.z = 0.0f;
                } else if (fall_dist >= m_config.hard_landing_min_fall) {
                    // Hard Landing! Damage and momentum stall
                    m_telemetry.move_state = EMovement::MOVE_Landing;
                    m_state_timer = 0.0f;
                    m_telemetry.health = std::max(1.0f, m_telemetry.health - 15.0f);
                    m_damage_cooldown = m_config.health_regen_delay;

                    float spd = m_telemetry.velocity.length_xy();
                    spd = std::max(0.0f, spd - 65.0f);
                    Vec3 fwd = m_telemetry.velocity.normalized_xy();
                    m_telemetry.velocity = fwd * spd;
                    m_telemetry.velocity.z = 0.0f;
                } else {
                    // Soft Landing
                    m_telemetry.move_state = EMovement::MOVE_SoftLanding;
                    m_state_timer = 0.0f;
                    m_telemetry.velocity.z = 0.0f;
                }
            }

            m_telemetry.position.z = floor_z;
            m_telemetry.grounded = true;
            m_last_wallrun_normal = Vec3(0.0f, 0.0f, 0.0f);
            if (m_telemetry.velocity.z < 0.0f) {
                m_telemetry.velocity.z = 0.0f;
            }
        } else if (m_telemetry.move_state != EMovement::MOVE_Grabbing &&
                   m_telemetry.move_state != EMovement::MOVE_ZipLine &&
                   m_telemetry.move_state != EMovement::MOVE_Swing &&
                   m_telemetry.move_state != EMovement::MOVE_WallClimbing &&
                   m_telemetry.move_state != EMovement::MOVE_WallRunningLeft &&
                   m_telemetry.move_state != EMovement::MOVE_WallRunningRight) {
            if (m_telemetry.grounded) {
                m_telemetry.grounded = false;
                m_air_fall_start_z = m_telemetry.position.z;
                m_fall_peak_z = m_telemetry.position.z;
                if (m_telemetry.move_state == EMovement::MOVE_Walking ||
                    m_telemetry.move_state == EMovement::MOVE_Crouch) {
                    m_telemetry.move_state = EMovement::MOVE_Falling;
                }
            }
            if (m_telemetry.position.z > m_fall_peak_z) {
                m_fall_peak_z = m_telemetry.position.z;
            }
        }
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

    // Dynamic FOV scaling: 100° at base, up to 108° at max sprint (630 u/s)
    float speed_ratio = std::clamp((m_telemetry.speed_2d - m_config.run_speed) / (m_config.sprint_speed - m_config.run_speed), 0.0f, 1.0f);
    m_telemetry.fov_deg = 100.0f + 8.0f * speed_ratio;
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
    // 180° Quick Turn (edge-triggered on fresh press)
    if (input.turn_180 && !m_prev_turn_180 && m_turn_180_timer <= 0.0f) {
        m_turn_180_timer = m_config.turn_180_time;
        m_turn_180_target_yaw = m_telemetry.yaw_deg + 180.0f;
        if (m_telemetry.move_state != EMovement::MOVE_WallClimbing) {
            m_telemetry.move_state = m_telemetry.grounded ? EMovement::MOVE_180Turn : EMovement::MOVE_180TurnInAir;
        }
    }

    if (m_turn_180_timer > 0.0f) {
        float step = (180.0f / m_config.turn_180_time) * dt;
        m_telemetry.yaw_deg += step;
        m_turn_180_timer -= dt;
        if (m_turn_180_timer <= 0.0f) {
            m_telemetry.yaw_deg = m_turn_180_target_yaw;
        }
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
// Swept Continuous Collision Detection (CCD) & Tracing Subsystem
// -----------------------------------------------------------------------------
ParkourController::TraceHit ParkourController::sweep_capsule(const Capsule& capsule, const Vec3& delta, const LevelScene& scene) const {
    TraceHit best_hit;
    best_hit.fraction = 1.0f;

    Vec3 half_extent(capsule.radius, capsule.radius, (capsule.height - capsule.bottom_offset) * 0.5f);
    Vec3 start_center = capsule.base + Vec3(0.0f, 0.0f, capsule.bottom_offset + half_extent.z);
    Vec3 end_center = start_center + delta;
    Vec3 sweep_min(std::min(start_center.x, end_center.x) - half_extent.x - 2.0f,
                   std::min(start_center.y, end_center.y) - half_extent.y - 2.0f,
                   std::min(start_center.z, end_center.z) - half_extent.z - 2.0f);
    Vec3 sweep_max(std::max(start_center.x, end_center.x) + half_extent.x + 2.0f,
                   std::max(start_center.y, end_center.y) + half_extent.y + 2.0f,
                   std::max(start_center.z, end_center.z) + half_extent.z + 2.0f);

    auto test_box = [&](const AABB& box, const LevelActor* actor) {
        if (box.max_pt.x < sweep_min.x || box.min_pt.x > sweep_max.x ||
            box.max_pt.y < sweep_min.y || box.min_pt.y > sweep_max.y ||
            box.max_pt.z < sweep_min.z || box.min_pt.z > sweep_max.z) {
            return;
        }
        // If movement is horizontal or upward and the box top is at or below capsule base,
        // it represents the floor under feet, not a blocking wall.
        if (delta.z >= -1e-4f && box.max_pt.z <= capsule.base.z + 2.0f) {
            return;
        }
        // If box bottom is at or above capsule top:
        if (delta.z <= 1e-4f && box.min_pt.z >= capsule.base.z + capsule.height - 2.0f) {
            return;
        }

        // Minkowski expansion of target box by capsule half-extent
        AABB exp_box(box.min_pt - half_extent, box.max_pt + half_extent);

        // Ray vs AABB intersection
        float t_in = -1e30f;
        float t_out = 1e30f;
        Vec3 hit_norm(0.0f, 0.0f, 1.0f);

        auto test_axis = [&](float s, float d, float bmin, float bmax, const Vec3& norm_neg, const Vec3& norm_pos) -> bool {
            if (std::abs(d) < 1e-7f) {
                return s >= bmin && s <= bmax;
            }
            float t1 = (bmin - s) / d;
            float t2 = (bmax - s) / d;
            Vec3 n1 = norm_neg;
            Vec3 n2 = norm_pos;
            if (t1 > t2) {
                std::swap(t1, t2);
                std::swap(n1, n2);
            }
            if (t1 > t_in) {
                t_in = t1;
                hit_norm = n1;
            }
            t_out = std::min(t_out, t2);
            return t_in <= t_out;
        };

        if (!test_axis(start_center.x, delta.x, exp_box.min_pt.x, exp_box.max_pt.x, Vec3(-1, 0, 0), Vec3(1, 0, 0))) return;
        if (!test_axis(start_center.y, delta.y, exp_box.min_pt.y, exp_box.max_pt.y, Vec3(0, -1, 0), Vec3(0, 1, 0))) return;
        if (!test_axis(start_center.z, delta.z, exp_box.min_pt.z, exp_box.max_pt.z, Vec3(0, 0, -1), Vec3(0, 0, 1))) return;

        if (t_in >= 0.0f && t_in < best_hit.fraction) {
            best_hit.hit = true;
            best_hit.fraction = t_in;
            best_hit.normal = hit_norm;
            best_hit.point = start_center + delta * t_in;
            best_hit.actor = actor;
        }
    };

    for (const auto& col : scene.colliders) {
        test_box(col, nullptr);
    }
    for (const auto& act : scene.actors) {
        if (act.is_collidable) {
            test_box(act.world_bounds, &act);
        }
    }
    for (const auto& elev : scene.elevators) {
        Vec3 fc = elev.current_pos + elev.cab_local_offset;
        Vec3 he = elev.cab_half_extents;
        float h_cab = he.z * 2.0f;
        // Hollow elevator cab floor slab and ceiling slab
        test_box(AABB(Vec3(fc.x - he.x, fc.y - he.y, fc.z - 24.0f),
                      Vec3(fc.x + he.x, fc.y + he.y, fc.z)), nullptr);
        test_box(AABB(Vec3(fc.x - he.x, fc.y - he.y, fc.z + h_cab),
                      Vec3(fc.x + he.x, fc.y + he.y, fc.z + h_cab + 24.0f)), nullptr);
        // While doors are closing or the cab is moving, enclose all 4 cab walls so Faith stays safely inside
        if (elev.state == ElevatorState::DoorsClosing || elev.state == ElevatorState::Moving) {
            test_box(AABB(Vec3(fc.x - he.x - 12.0f, fc.y - he.y, fc.z),
                          Vec3(fc.x - he.x + 4.0f, fc.y + he.y, fc.z + h_cab)), nullptr);
            test_box(AABB(Vec3(fc.x + he.x - 4.0f, fc.y - he.y, fc.z),
                          Vec3(fc.x + he.x + 12.0f, fc.y + he.y, fc.z + h_cab)), nullptr);
            test_box(AABB(Vec3(fc.x - he.x, fc.y - he.y - 12.0f, fc.z),
                          Vec3(fc.x + he.x, fc.y - he.y + 4.0f, fc.z + h_cab)), nullptr);
            test_box(AABB(Vec3(fc.x - he.x, fc.y + he.y - 4.0f, fc.z),
                          Vec3(fc.x + he.x, fc.y + he.y + 12.0f, fc.z + h_cab)), nullptr);
        }
    }

    return best_hit;
}

ParkourController::TraceHit ParkourController::trace_ray(const Vec3& start, const Vec3& end, const LevelScene& scene) const {
    TraceHit best;
    best.fraction = 1.0f;
    Vec3 dir = end - start;
    float dist = dir.length();
    if (dist < 1e-6f) return best;
    Vec3 dir_norm = dir / dist;
    Vec3 ray_min(std::min(start.x, end.x) - 1.0f, std::min(start.y, end.y) - 1.0f, std::min(start.z, end.z) - 1.0f);
    Vec3 ray_max(std::max(start.x, end.x) + 1.0f, std::max(start.y, end.y) + 1.0f, std::max(start.z, end.z) + 1.0f);

    auto test_box = [&](const AABB& box, const LevelActor* act) {
        if (box.max_pt.x < ray_min.x || box.min_pt.x > ray_max.x ||
            box.max_pt.y < ray_min.y || box.min_pt.y > ray_max.y ||
            box.max_pt.z < ray_min.z || box.min_pt.z > ray_max.z) {
            return;
        }
        float t_hit = 0.0f;
        if (box.ray_intersect(start, dir_norm, t_hit)) {
            float frac = t_hit / dist;
            if (frac >= 0.0f && frac < best.fraction) {
                best.hit = true;
                best.fraction = frac;
                best.point = start + dir * frac;
                best.actor = act;

                // Estimate surface normal from box face
                Vec3 c = box.center();
                Vec3 ext = box.extent();
                Vec3 p_rel = best.point - c;
                float dx = std::abs(p_rel.x) - ext.x;
                float dy = std::abs(p_rel.y) - ext.y;
                float dz = std::abs(p_rel.z) - ext.z;

                if (dx >= dy && dx >= dz) {
                    best.normal = Vec3(p_rel.x > 0 ? 1.0f : -1.0f, 0.0f, 0.0f);
                } else if (dy >= dx && dy >= dz) {
                    best.normal = Vec3(0.0f, p_rel.y > 0 ? 1.0f : -1.0f, 0.0f);
                } else {
                    best.normal = Vec3(0.0f, 0.0f, p_rel.z > 0 ? 1.0f : -1.0f);
                }
            }
        }
    };

    for (const auto& b : scene.colliders) test_box(b, nullptr);
    for (const auto& a : scene.actors) {
        if (a.is_collidable) test_box(a.world_bounds, &a);
    }
    for (const auto& elev : scene.elevators) {
        Vec3 fc = elev.current_pos + elev.cab_local_offset;
        Vec3 he = elev.cab_half_extents;
        float h_cab = he.z * 2.0f;
        test_box(AABB(Vec3(fc.x - he.x, fc.y - he.y, fc.z - 24.0f),
                      Vec3(fc.x + he.x, fc.y + he.y, fc.z)), nullptr);
        test_box(AABB(Vec3(fc.x - he.x, fc.y - he.y, fc.z + h_cab),
                      Vec3(fc.x + he.x, fc.y + he.y, fc.z + h_cab + 24.0f)), nullptr);
    }
    return best;
}

bool ParkourController::check_ground(const LevelScene& scene, float& floor_z, Vec3& floor_normal) {
    Capsule cap;
    cap.base = m_telemetry.position + Vec3(0.0f, 0.0f, 10.0f); // slight upward probe
    cap.radius = 30.0f;
    cap.height = 90.0f;
    cap.bottom_offset = 0.0f;

    Vec3 delta(0.0f, 0.0f, -35.0f); // probe down beneath feet
    TraceHit hit = sweep_capsule(cap, delta, scene);

    if (hit.hit && hit.normal.z >= 0.707f) {
        floor_z = cap.base.z + delta.z * hit.fraction;
        floor_normal = hit.normal;
        return true;
    }
    return false;
}

// -----------------------------------------------------------------------------
// Ground Locomotion: Walk, Jog, Run, Sprint & Auto Step-Up
// -----------------------------------------------------------------------------
void ParkourController::update_ground_locomotion(const InputFrame& input, float dt, const LevelScene& scene) {
    if (try_initiate_zipline(scene)) return;

    // Jump initiation
    if (input.jump && !m_jump_consumed) {
        m_jump_consumed = true;
        if (try_initiate_springboard(input, scene)) return;
        if (try_initiate_vault(input, scene)) return;

        float jump_z = m_telemetry.weapon.is_heavy ? m_config.base_jump_z_heavy : m_config.base_jump_z;
        m_telemetry.velocity.z = jump_z;

        // JumpAddXY adds forward momentum along view vector
        Vec3 fwd = Rotator::from_degrees(0.0f, m_telemetry.yaw_deg, 0.0f).forward();
        m_telemetry.velocity += fwd * m_config.jump_add_xy;

        m_telemetry.grounded = false;
        m_telemetry.move_state = EMovement::MOVE_Jump;
        m_state_timer = 0.0f;
        m_fall_peak_z = m_telemetry.position.z;
        return;
    }

    // Crouch / Slide transition
    if (input.crouch) {
        float current_spd = m_telemetry.velocity.length_xy();
        if (current_spd >= m_config.slide_min_speed) {
            m_telemetry.move_state = EMovement::MOVE_Slide;
            m_slide_timer = 0.0f;
            m_state_timer = 0.0f;
            m_telemetry.eye_height = 36.0f;
            return;
        } else {
            m_telemetry.move_state = EMovement::MOVE_Crouch;
            m_telemetry.eye_height = 48.0f;
        }
    } else {
        m_telemetry.move_state = EMovement::MOVE_Walking;
        m_telemetry.eye_height = 84.0f;
    }

    // Target Speed Calculation
    float target_speed = m_config.jog_speed;

    if (m_telemetry.move_state == EMovement::MOVE_Crouch) {
        target_speed = m_config.walk_speed * 1.5f; // ~75 u/s
    } else if (input.sprint && input.forward > 0.0f) {
        // Accumulate momentum for full sprint (up to 630 u/s)
        m_momentum_timer += dt;
        float sprint_progress = std::clamp(m_momentum_timer / 2.5f, 0.0f, 1.0f);
        target_speed = m_config.run_speed + (m_config.sprint_speed - m_config.run_speed) * sprint_progress;
    } else {
        m_momentum_timer = std::max(0.0f, m_momentum_timer - dt * 2.0f);
        if (input.forward != 0.0f || input.strafe != 0.0f) {
            target_speed = (input.forward > 0.0f) ? m_config.run_speed : m_config.jog_speed;
        } else {
            target_speed = 0.0f;
        }
    }

    // Weapon mobility penalties (from DefaultWeapons.ini MovementSpeedMultiplier)
    if (m_telemetry.weapon.equipped) {
        target_speed *= m_telemetry.weapon.mobility_scale;
    }

    // Movement direction from view angles
    Rotator view_rot = Rotator::from_degrees(0.0f, m_telemetry.yaw_deg, 0.0f);
    Vec3 fwd = view_rot.forward();
    Vec3 right = view_rot.right();
    Vec3 desired_dir = (fwd * input.forward + right * input.strafe).normalized();

    // Accelerate horizontal velocity
    Vec3 target_vel = desired_dir * target_speed;
    constexpr float ACCEL = 6144.0f;
    constexpr float FRICTION = 8.0f;

    if (desired_dir.length_sq() > 1e-4f) {
        Vec3 diff = target_vel - Vec3(m_telemetry.velocity.x, m_telemetry.velocity.y, 0.0f);
        float step = ACCEL * dt;
        if (diff.length() <= step) {
            m_telemetry.velocity.x = target_vel.x;
            m_telemetry.velocity.y = target_vel.y;
        } else {
            Vec3 dir = diff.normalized();
            m_telemetry.velocity.x += dir.x * step;
            m_telemetry.velocity.y += dir.y * step;
        }
    } else {
        // Decelerate / friction
        float speed = m_telemetry.velocity.length_xy();
        speed = std::max(0.0f, speed - speed * FRICTION * dt);
        Vec3 dir = m_telemetry.velocity.normalized_xy();
        m_telemetry.velocity.x = dir.x * speed;
        m_telemetry.velocity.y = dir.y * speed;
    }

    // Continuous Swept Movement with Auto Step-Up (TdPawn: Radius=30, Height=90, MaxWallStepHeight=35)
    Capsule cap;
    cap.base = m_telemetry.position;
    cap.radius = 30.0f;
    cap.height = (m_telemetry.move_state == EMovement::MOVE_Crouch) ? 45.0f : 90.0f;
    cap.bottom_offset = 0.0f;

    Vec3 move_delta = Vec3(m_telemetry.velocity.x, m_telemetry.velocity.y, 0.0f) * dt;
    TraceHit hit = sweep_capsule(cap, move_delta, scene);

    if (!hit.hit) {
        m_telemetry.position += move_delta;
    } else {
        // Wall hit: test Auto Step-Up (curbs / steps up to 35 units high)
        constexpr float MAX_STEP_UP = 35.0f;
        Capsule step_cap = cap;
        step_cap.base.z += MAX_STEP_UP;

        TraceHit step_hit = sweep_capsule(step_cap, move_delta, scene);
        if (!step_hit.hit) {
            // Can step over! Elevate and proceed
            m_telemetry.position.z += MAX_STEP_UP;
            m_telemetry.position += move_delta;
            m_telemetry.move_state = EMovement::MOVE_AutoStepUp;
        } else {
            // Slide along collision normal with secondary sweep check
            m_telemetry.position += move_delta * std::max(0.0f, hit.fraction - 0.001f);
            Vec3 remaining = move_delta * (1.0f - hit.fraction);
            remaining -= hit.normal * remaining.dot(hit.normal);
            if (remaining.length_sq() > 1e-6f) {
                Capsule slide_cap = cap;
                slide_cap.base = m_telemetry.position;
                TraceHit slide_hit = sweep_capsule(slide_cap, remaining, scene);
                if (!slide_hit.hit) {
                    m_telemetry.position += remaining;
                } else {
                    m_telemetry.position += remaining * std::max(0.0f, slide_hit.fraction - 0.001f);
                }
            }

            // Dampen velocity along normal
            if (m_telemetry.velocity.dot(hit.normal) < 0.0f) {
                m_telemetry.velocity -= hit.normal * m_telemetry.velocity.dot(hit.normal);
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Air Locomotion: Jumps, Coils & Trajectory
// -----------------------------------------------------------------------------
void ParkourController::update_air_locomotion(const InputFrame& input, float dt, const LevelScene& scene) {
    // Check for Mid-Air Coil (lifting legs over obstacles +60 units)
    if (input.crouch && m_coil_timer <= 0.0f && m_telemetry.velocity.length_xy() >= 100.0f) {
        m_telemetry.move_state = EMovement::MOVE_Coil;
        m_coil_timer = m_config.coil_duration;
    }
    if (m_coil_timer > 0.0f) {
        m_coil_timer -= dt;
    }

    // Check for interactive transitions while airborne
    if (try_initiate_zipline(scene)) return;
    if (try_initiate_ledge_grab(scene)) return;
    if (try_initiate_wallrun(input, scene)) return;
    if (try_initiate_wallclimb(input, scene)) return;

    // Apply gravity
    m_telemetry.velocity.z -= m_config.gravity * dt;

    // Subtle air control (TdPawn AirControl = 0.025..0.09, capped at max(current_spd_2d, SprintVelocity))
    Rotator view_rot = Rotator::from_degrees(0.0f, m_telemetry.yaw_deg, 0.0f);
    Vec3 fwd = view_rot.forward();
    Vec3 right = view_rot.right();
    Vec3 air_dir = (fwd * input.forward + right * input.strafe).normalized();

    if (air_dir.length_sq() > 1e-4f) {
        float cur_spd_2d = m_telemetry.velocity.length_xy();
        float max_air_spd = std::max(cur_spd_2d, m_config.sprint_speed);
        constexpr float AIR_CONTROL = 350.0f;
        m_telemetry.velocity.x += air_dir.x * AIR_CONTROL * dt;
        m_telemetry.velocity.y += air_dir.y * AIR_CONTROL * dt;
        float new_spd_2d = m_telemetry.velocity.length_xy();
        if (new_spd_2d > max_air_spd && new_spd_2d > 1e-4f) {
            float scale = max_air_spd / new_spd_2d;
            m_telemetry.velocity.x *= scale;
            m_telemetry.velocity.y *= scale;
        }
    }

    // Swept displacement with slide-along-normal so touching a ledge/wall at fraction=0 never freezes Faith
    Capsule cap;
    cap.base = m_telemetry.position;
    cap.radius = 30.0f;
    cap.height = 90.0f;
    cap.bottom_offset = (m_telemetry.move_state == EMovement::MOVE_Coil) ? m_config.coil_height_boost : 0.0f;

    Vec3 delta = m_telemetry.velocity * dt;
    TraceHit hit = sweep_capsule(cap, delta, scene);

    if (!hit.hit) {
        m_telemetry.position += delta;
    } else {
        m_telemetry.position += delta * std::max(0.0f, hit.fraction - 0.001f);
        Vec3 remaining = delta * (1.0f - hit.fraction);
        remaining -= hit.normal * remaining.dot(hit.normal);
        if (remaining.length_sq() > 1e-6f) {
            Capsule slide_cap = cap;
            slide_cap.base = m_telemetry.position;
            TraceHit slide_hit = sweep_capsule(slide_cap, remaining, scene);
            if (!slide_hit.hit) {
                m_telemetry.position += remaining;
            } else {
                m_telemetry.position += remaining * std::max(0.0f, slide_hit.fraction - 0.001f);
            }
        }
        if (m_telemetry.velocity.dot(hit.normal) < 0.0f) {
            m_telemetry.velocity -= hit.normal * m_telemetry.velocity.dot(hit.normal);
        }
    }
}

// -----------------------------------------------------------------------------
// Wallrun Subsystem (Left & Right with 15° Camera Roll - TdMove_WallRun)
// -----------------------------------------------------------------------------
bool ParkourController::try_initiate_wallrun(const InputFrame& input, const LevelScene& scene) {
    (void)input;
    if (m_telemetry.weapon.is_heavy) return false;
    if (m_wallrun_cooldown > 0.0f) return false;
    if (m_telemetry.velocity.length_xy() < m_config.wallrun_min_speed) return false;

    Rotator view_rot = Rotator::from_degrees(0.0f, m_telemetry.yaw_deg, 0.0f);
    Vec3 fwd = view_rot.forward();
    Vec3 right = view_rot.right();

    // Check right wall then left wall
    Vec3 probe_start = m_telemetry.position + Vec3(0, 0, 70);
    TraceHit hit_right = trace_ray(probe_start, probe_start + right * 70.0f, scene);
    TraceHit hit_left = trace_ray(probe_start, probe_start - right * 70.0f, scene);

    auto can_run = [&](const TraceHit& hit, bool is_right) -> bool {
        if (!hit.hit || std::abs(hit.normal.z) > 0.2f) return false;

        // TdMove_WallRun: cannot re-attach to the same wall normal in mid-air without landing first
        if (m_last_wallrun_normal.length_sq() > 0.5f && hit.normal.dot(m_last_wallrun_normal) > 0.85f) {
            return false;
        }

        // Tangent along wall in player's forward direction
        Vec3 up(0, 0, 1);
        Vec3 tangent = is_right ? up.cross(hit.normal) : hit.normal.cross(up);
        tangent = tangent.normalized();

        float dot = fwd.dot(tangent);
        float angle_deg = std::acos(std::clamp(dot, -1.0f, 1.0f)) * RAD2DEG;

        if (angle_deg <= m_config.wallrun_max_angle_deg) {
            m_telemetry.move_state = is_right ? EMovement::MOVE_WallRunningRight : EMovement::MOVE_WallRunningLeft;
            m_telemetry.wall_normal = hit.normal;
            m_last_wallrun_normal = hit.normal;
            m_wall_tangent = tangent;
            m_wallrun_timer = 0.0f;
            m_telemetry.camera_roll_deg = is_right ? -15.0f : 15.0f;
            if (input.jump) {
                m_jump_consumed = true;
            }

            // Initial vertical boost (WallRunningHorisontalInitialZHeight = 170)
            m_telemetry.velocity.z = std::max(m_telemetry.velocity.z, m_config.wallrun_initial_z);

            // Align velocity with wall tangent (capped at SprintVelocity)
            float spd = std::clamp(m_telemetry.velocity.length_xy(), 350.0f, m_config.sprint_speed);
            m_wallrun_begin_speed = spd;
            m_telemetry.velocity.x = tangent.x * spd;
            m_telemetry.velocity.y = tangent.y * spd;
            return true;
        }
        return false;
    };

    if (can_run(hit_right, true)) return true;
    if (can_run(hit_left, false)) return true;
    return false;
}

void ParkourController::update_wallrun(const InputFrame& input, float dt, const LevelScene& scene) {
    m_wallrun_timer += dt;

    // Wallrun Jump (TdMove_WallrunJump: requires fresh Jump press after attaching to wall)
    if (input.jump && !m_jump_consumed && m_wallrun_timer >= 0.08f) {
        m_jump_consumed = true;
        m_wallrun_cooldown = 0.15f;
        m_telemetry.move_state = EMovement::MOVE_WallRunJump;
        m_telemetry.velocity = m_telemetry.wall_normal * 320.0f + m_wall_tangent * 400.0f + Vec3(0, 0, 450.0f);
        m_telemetry.camera_roll_deg = 0.0f;
        return;
    }

    // Wallrun Kick (TdMove_MeleeWallrun)
    if (input.melee) {
        m_wallrun_cooldown = 0.20f;
        m_telemetry.move_state = EMovement::MOVE_MeleeWallrun;
        m_state_timer = 0.0f;
        m_telemetry.combat_anim_time = 0.0f;
        m_telemetry.combat_anim_duration = 0.55f;
        m_melee_cooldown = 0.55f;
        m_telemetry.velocity = m_telemetry.wall_normal * 260.0f + m_wall_tangent * 380.0f + Vec3(0, 0, 140.0f);
        m_telemetry.camera_roll_deg = 0.0f;
        return;
    }

    // Kinematics: early forward alignment (capped at SprintVelocity = 630 u/s) then deceleration
    float spd = m_telemetry.velocity.length_xy();
    float max_wr_spd = std::max(m_wallrun_begin_speed, m_config.sprint_speed);
    if (m_wallrun_timer < 0.6f) {
        spd = std::min(max_wr_spd, spd + m_config.wallrun_accel * dt);
    } else {
        spd = std::max(0.0f, spd - m_config.wallrun_decel * dt);
    }
    m_telemetry.velocity.x = m_wall_tangent.x * spd;
    m_telemetry.velocity.y = m_wall_tangent.y * spd;

    // Reduced gravity while wallrunning
    m_telemetry.velocity.z -= (m_config.gravity * 0.35f) * dt;

    // Check if wall has ended
    Vec3 probe_start = m_telemetry.position + Vec3(0, 0, 70);
    TraceHit wall_check = trace_ray(probe_start, probe_start - m_telemetry.wall_normal * 80.0f, scene);

    if (!wall_check.hit || m_wallrun_timer >= m_config.wallrun_duration || spd < 150.0f) {
        m_wallrun_cooldown = 0.15f;
        m_telemetry.move_state = EMovement::MOVE_Falling;
        m_telemetry.camera_roll_deg = 0.0f;
        return;
    }

    // Sweep movement along wall (TdPawn: Radius=30, Height=90)
    Capsule cap;
    cap.base = m_telemetry.position;
    cap.radius = 30.0f;
    cap.height = 90.0f;
    Vec3 delta = m_telemetry.velocity * dt;
    TraceHit hit = sweep_capsule(cap, delta, scene);
    if (!hit.hit) {
        m_telemetry.position += delta;
    } else {
        m_telemetry.position += delta * std::max(0.0f, hit.fraction - 0.001f);
        if (m_telemetry.velocity.dot(hit.normal) < 0.0f) {
            m_telemetry.velocity -= hit.normal * m_telemetry.velocity.dot(hit.normal);
        }
        m_wallrun_cooldown = 0.20f;
        m_telemetry.move_state = EMovement::MOVE_Falling;
        m_telemetry.camera_roll_deg = 0.0f;
    }
}

// -----------------------------------------------------------------------------
// Wallclimb & 180° Turn Jump Subsystem (TdMove_WallClimb / TdMove_WallClimb180TurnJump)
// -----------------------------------------------------------------------------
bool ParkourController::try_initiate_wallclimb(const InputFrame& input, const LevelScene& scene) {
    if (m_telemetry.weapon.is_heavy || !input.jump) return false;

    Rotator view_rot = Rotator::from_degrees(0.0f, m_telemetry.yaw_deg, 0.0f);
    Vec3 fwd = view_rot.forward();
    Vec3 probe_start = m_telemetry.position + Vec3(0, 0, 70);

    // Check TdLadderVolume actors (`is_ladder`) for ladder/pipe climbing
    for (const auto& act : scene.actors) {
        if (act.is_ladder) {
            if (m_telemetry.position.distance_xy(act.location) < 110.0f &&
                m_telemetry.position.z >= act.world_bounds.min_pt.z - 80.0f &&
                m_telemetry.position.z <= act.world_bounds.max_pt.z + 80.0f) {
                m_telemetry.move_state = EMovement::MOVE_WallClimbing;
                m_telemetry.wall_normal = -fwd;
                m_last_wallrun_normal = Vec3(0.0f, 0.0f, 0.0f);
                m_telemetry.velocity.x = 0.0f;
                m_telemetry.velocity.y = 0.0f;
                m_telemetry.velocity.z = m_config.wallclimb_boost_z;
                m_state_timer = 0.0f;
                m_jump_consumed = true;
                return true;
            }
        }
    }

    TraceHit hit = trace_ray(probe_start, probe_start + fwd * 90.0f, scene);
    if (!hit.hit || std::abs(hit.normal.z) > 0.2f) return false;

    float facing_dot = fwd.dot(-hit.normal);
    float angle_deg = std::acos(std::clamp(facing_dot, -1.0f, 1.0f)) * RAD2DEG;

    if (angle_deg <= m_config.wallclimb_max_angle_deg) {
        m_telemetry.move_state = EMovement::MOVE_WallClimbing;
        m_telemetry.wall_normal = hit.normal;
        m_last_wallrun_normal = Vec3(0.0f, 0.0f, 0.0f);
        m_telemetry.velocity.x = 0.0f;
        m_telemetry.velocity.y = 0.0f;
        m_telemetry.velocity.z = m_config.wallclimb_boost_z;
        m_state_timer = 0.0f;
        m_jump_consumed = true;
        return true;
    }
    return false;
}

void ParkourController::update_wallclimb(const InputFrame& input, float dt, const LevelScene& scene) {
    m_state_timer += dt;

    // Check for top ledge grab during climb (TdMove_WallClimb.bCheckForGrab = true)
    if (try_initiate_ledge_grab(scene)) {
        return;
    }

    // 180° Turn Jump off vertical climb (TdMove_WallClimb180TurnJump: triggered by Q / turn_180 or a fresh Jump press)
    if ((input.turn_180 && !m_prev_turn_180) || (input.jump && !m_jump_consumed && m_state_timer > 0.15f)) {
        m_jump_consumed = true;
        m_telemetry.move_state = EMovement::MOVE_WallClimb180TurnJump;
        m_telemetry.yaw_deg += 180.0f;
        m_telemetry.velocity = m_telemetry.wall_normal * 400.0f + Vec3(0, 0, 450.0f);
        return;
    }

    // Decelerate under vertical wallclimb gravity
    m_telemetry.velocity.z -= m_config.wallclimb_gravity * dt;

    // Check if player reaches the top ledge of the wall
    Vec3 chest = m_telemetry.position + Vec3(0, 0, 70);
    TraceHit top_check = trace_ray(chest, chest - m_telemetry.wall_normal * 80.0f, scene);

    if (!top_check.hit && m_telemetry.velocity.z <= 120.0f) {
        // Clear top of ledge: auto pull up!
        m_telemetry.position += -m_telemetry.wall_normal * 60.0f + Vec3(0, 0, 40.0f);
        m_telemetry.move_state = EMovement::MOVE_Walking;
        m_telemetry.velocity = -m_telemetry.wall_normal * m_config.jog_speed;
        return;
    }

    if (m_telemetry.velocity.z <= -50.0f) {
        m_telemetry.move_state = EMovement::MOVE_Falling;
        return;
    }

    m_telemetry.position.z += m_telemetry.velocity.z * dt;
}

// -----------------------------------------------------------------------------
// Slide & Low-Friction Crouch Movement Subsystem (TdMove_Slide / TdMove_MeleeSlide)
// -----------------------------------------------------------------------------
void ParkourController::update_slide(const InputFrame& input, float dt, LevelScene& scene) {
    m_slide_timer += dt;

    // Slide Melee Kick (`MeleeSlide`: sweeps enemies off their feet with `HitMeleeSlide`)
    if (input.melee && m_telemetry.move_state != EMovement::MOVE_MeleeSlide) {
        m_telemetry.move_state = EMovement::MOVE_MeleeSlide;
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

    // Slide friction
    float spd = m_telemetry.velocity.length_xy();
    spd = std::max(0.0f, spd - (spd * m_config.slide_friction + 80.0f) * dt);

    Vec3 dir = m_telemetry.velocity.normalized_xy();
    m_telemetry.velocity.x = dir.x * spd;
    m_telemetry.velocity.y = dir.y * spd;

    // Jump cancels slide into ground jump
    if (input.jump && !m_jump_consumed) {
        m_jump_consumed = true;
        m_telemetry.move_state = EMovement::MOVE_Jump;
        m_telemetry.velocity.z = m_config.base_jump_z;
        m_telemetry.eye_height = 84.0f;
        return;
    }

    // Abort conditions
    if (spd < 100.0f || m_slide_timer >= m_config.slide_max_duration || !input.crouch) {
        m_telemetry.move_state = input.crouch ? EMovement::MOVE_Crouch : EMovement::MOVE_Walking;
        m_telemetry.eye_height = input.crouch ? 48.0f : 84.0f;
        return;
    }

    Capsule cap;
    cap.base = m_telemetry.position;
    cap.radius = 30.0f;
    cap.height = 45.0f; // low slide clearance!
    cap.bottom_offset = 0.0f;

    Vec3 delta = m_telemetry.velocity * dt;
    TraceHit hit = sweep_capsule(cap, delta, scene);
    if (!hit.hit) {
        m_telemetry.position += delta;
    } else {
        m_telemetry.position += delta * hit.fraction;
        m_telemetry.move_state = EMovement::MOVE_Crouch;
    }
}

// -----------------------------------------------------------------------------
// Ledge Grab, Shimmy & Pull-Up Subsystem
// -----------------------------------------------------------------------------
bool ParkourController::try_initiate_ledge_grab(const LevelScene& scene) {
    if (m_telemetry.move_state == EMovement::MOVE_Grabbing ||
        m_telemetry.move_state == EMovement::MOVE_GrabPullUp) return false;

    Vec3 fwd = Rotator::from_degrees(0.0f, m_telemetry.yaw_deg, 0.0f).forward();
    Vec3 eye = m_telemetry.position + Vec3(0, 0, 70);

    // Forward trace to wall
    TraceHit fwd_hit = trace_ray(eye, eye + fwd * 100.0f, scene);
    if (!fwd_hit.hit || std::abs(fwd_hit.normal.z) > 0.2f) return false;

    // Downward trace onto top of ledge
    Vec3 ledge_top_start = fwd_hit.point - fwd_hit.normal * 15.0f + Vec3(0, 0, 50.0f);
    TraceHit down_hit = trace_ray(ledge_top_start, ledge_top_start + Vec3(0, 0, -80.0f), scene);

    if (down_hit.hit && down_hit.normal.z >= 0.707f) {
        float ledge_z = down_hit.point.z;
        float diff_z = ledge_z - m_telemetry.position.z;

        if (diff_z >= 40.0f && diff_z <= 120.0f) {
            m_telemetry.move_state = EMovement::MOVE_Grabbing;
            m_telemetry.position.z = ledge_z - 80.0f; // hand hanging position
            m_telemetry.velocity = Vec3(0, 0, 0);
            m_telemetry.wall_normal = fwd_hit.normal;
            return true;
        }
    }
    return false;
}

void ParkourController::update_ledge_grab(const InputFrame& input, float dt, const LevelScene& scene) {
    (void)dt;
    (void)scene;
    m_telemetry.velocity = Vec3(0, 0, 0);

    // Pull Up
    if (m_telemetry.move_state == EMovement::MOVE_Grabbing && (input.forward > 0.0f || input.jump)) {
        m_telemetry.move_state = EMovement::MOVE_GrabPullUp;
        m_state_timer = 0.0f;
    }
    // Drop down
    else if (m_telemetry.move_state == EMovement::MOVE_Grabbing && (input.forward < 0.0f || input.crouch)) {
        m_telemetry.move_state = EMovement::MOVE_Falling;
        m_telemetry.velocity = m_telemetry.wall_normal * 100.0f;
        return;
    }

    if (m_telemetry.move_state == EMovement::MOVE_GrabPullUp) {
        if (m_state_timer >= 0.25f) {
            m_telemetry.position += -m_telemetry.wall_normal * 60.0f + Vec3(0, 0, 85.0f);
            m_telemetry.move_state = EMovement::MOVE_Walking;
        }
    }
}

// -----------------------------------------------------------------------------
// Speed Vault & Springboard Subsystems
// -----------------------------------------------------------------------------
bool ParkourController::try_initiate_springboard(const InputFrame& input, const LevelScene& scene) {
    (void)input;
    Vec3 fwd = Rotator::from_degrees(0.0f, m_telemetry.yaw_deg, 0.0f).forward();
    Vec3 check_pt = m_telemetry.position + fwd * 90.0f;

    for (const auto& act : scene.actors) {
        if (act.is_springboard || act.tag == "springboard") {
            if (act.world_bounds.contains(check_pt) || act.location.distance_xy(m_telemetry.position) < 140.0f) {
                m_telemetry.move_state = EMovement::MOVE_SpringBoarding;
                m_state_timer = 0.0f;
                m_telemetry.position += fwd * 65.0f + Vec3(0.0f, 0.0f, 92.0f);
                m_telemetry.velocity.z = m_config.springboard_jump_z; // 950 u/s!
                float spd = std::max(m_telemetry.velocity.length_xy(), 400.0f);
                m_telemetry.velocity.x = fwd.x * spd;
                m_telemetry.velocity.y = fwd.y * spd;
                m_telemetry.grounded = false;
                m_fall_peak_z = m_telemetry.position.z;
                return true;
            }
        }
    }
    return false;
}

bool ParkourController::try_initiate_vault(const InputFrame& input, const LevelScene& scene) {
    (void)input;
    Vec3 fwd = Rotator::from_degrees(0.0f, m_telemetry.yaw_deg, 0.0f).forward();
    Vec3 waist = m_telemetry.position + Vec3(0, 0, 50.0f);

    TraceHit hit = trace_ray(waist, waist + fwd * 100.0f, scene);
    if (hit.hit && std::abs(hit.normal.z) < 0.2f) {
        // Check top of obstacle
        Vec3 over_start = hit.point + fwd * 40.0f + Vec3(0, 0, 100.0f);
        TraceHit top_hit = trace_ray(over_start, over_start + Vec3(0, 0, -120.0f), scene);

        if (top_hit.hit && top_hit.point.z - m_telemetry.position.z <= 110.0f) {
            bool is_sprinting = m_telemetry.velocity.length_xy() >= 350.0f;
            m_telemetry.move_state = is_sprinting ? EMovement::MOVE_SpeedVaulting : EMovement::MOVE_VaultOver;

            float boost = is_sprinting ? 80.0f : 0.0f;
            float spd = m_telemetry.velocity.length_xy() + boost;
            m_telemetry.velocity = fwd * spd;
            m_telemetry.velocity.z = 250.0f;
            m_telemetry.position += fwd * 80.0f + Vec3(0, 0, 30.0f);
            return true;
        }
    }
    return false;
}

// -----------------------------------------------------------------------------
// Zipline, Swing Bar & Balance Beam Subsystems
// -----------------------------------------------------------------------------
bool ParkourController::try_initiate_zipline(const LevelScene& scene) {
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
                if (closest_pt.distance(m_telemetry.position + Vec3(0, 0, 80)) < 135.0f) {
                    m_telemetry.move_state = EMovement::MOVE_ZipLine;
                    m_zipline_start = start;
                    m_zipline_end = end;
                    m_telemetry.position = closest_pt - Vec3(0.0f, 0.0f, 80.0f);
                    m_telemetry.velocity = line_dir * 350.0f;
                    m_telemetry.grounded = false;
                    return true;
                }
            }
        }
    }
    return false;
}

void ParkourController::update_zipline(const InputFrame& input, float dt, const LevelScene& scene) {
    (void)scene;
    Vec3 zip_dir = (m_zipline_end - m_zipline_start).normalized();

    // Detach with jump
    if (input.jump) {
        m_telemetry.move_state = EMovement::MOVE_Jump;
        m_telemetry.velocity = zip_dir * 500.0f + Vec3(0, 0, 250.0f);
        return;
    }

    // Accelerate down zipline
    float spd = m_telemetry.velocity.length();
    spd = std::min(850.0f, spd + 450.0f * dt);
    m_telemetry.velocity = zip_dir * spd;
    m_telemetry.position += m_telemetry.velocity * dt;

    Vec3 hand_pos = m_telemetry.position + Vec3(0.0f, 0.0f, 80.0f);
    if (hand_pos.distance(m_zipline_end) < 90.0f || m_telemetry.position.distance(m_zipline_end) < 90.0f) {
        m_telemetry.move_state = EMovement::MOVE_Falling;
        m_telemetry.velocity = Vec3(zip_dir.x * 320.0f, zip_dir.y * 320.0f, 0.0f);
        m_fall_peak_z = m_telemetry.position.z;
    }
}

void ParkourController::update_swing_bar(const InputFrame& input, float dt, const LevelScene& scene) {
    (void)scene;
    m_swing_angular_vel += -std::sin(m_swing_angle) * 8.0f * dt;
    m_swing_angle += m_swing_angular_vel * dt;

    if (input.jump) {
        m_telemetry.move_state = EMovement::MOVE_Jump;
        Vec3 fwd = Rotator::from_degrees(0.0f, m_telemetry.yaw_deg, 0.0f).forward();
        m_telemetry.velocity = fwd * 600.0f + Vec3(0, 0, 300.0f);
        return;
    }
}

void ParkourController::update_balance(const InputFrame& input, float dt, const LevelScene& scene) {
    (void)scene;
    // Walking on balance beam caps speed at 0.34x
    float target_speed = m_config.run_speed * 0.34f;
    Vec3 fwd = Rotator::from_degrees(0.0f, m_telemetry.yaw_deg, 0.0f).forward();
    m_telemetry.velocity = fwd * (input.forward * target_speed);
    m_telemetry.position += m_telemetry.velocity * dt;

    if (input.jump) {
        m_telemetry.move_state = EMovement::MOVE_Jump;
        m_telemetry.velocity.z = m_config.base_jump_z;
    }
}

void ParkourController::update_skill_roll(const InputFrame& input, float dt) {
    (void)input;
    m_roll_anim_timer -= dt;
    if (m_roll_anim_timer <= 0.0f) {
        m_telemetry.move_state = EMovement::MOVE_Walking;
        return;
    }

    // Camera pitch animation for skill roll
    float roll_progress = 1.0f - (m_roll_anim_timer / 0.5f);
    m_telemetry.pitch_deg = -30.0f * std::sin(roll_progress * PI);
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

            TraceHit wall_hit = trace_ray(eye, ray_end, scene);
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
                TraceHit los = trace_ray(bot_muzzle, target_pt, scene);
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
    // 1. Fall Death / Kill Volume -> Respawn
    if (m_telemetry.position.z < -2000.0f ||
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
void ParkourController::update_elevators(const InputFrame& input, float dt, LevelScene& scene) {
    m_telemetry.in_elevator = false;
    m_telemetry.active_elevator_idx = -1;

    for (size_t i = 0; i < scene.elevators.size(); ++i) {
        ElevatorInstance& elev = scene.elevators[i];
        elev.prev_pos = elev.current_pos;

        Vec3 fc_prev = elev.prev_pos + elev.cab_local_offset;
        Vec3 he = elev.cab_half_extents;
        float h_cab = he.z * 2.0f;

        bool player_inside = (std::abs(m_telemetry.position.x - fc_prev.x) <= he.x - 8.0f) &&
                             (std::abs(m_telemetry.position.y - fc_prev.y) <= he.y - 8.0f) &&
                             (m_telemetry.position.z >= fc_prev.z - 30.0f) &&
                             (m_telemetry.position.z <= fc_prev.z + h_cab + 10.0f);

        bool button_pressed = input.use && (player_inside || m_telemetry.position.distance(elev.button_pos) < 180.0f);

        switch (elev.state) {
            case ElevatorState::IdleStart: {
                elev.door_open_Start = 1.0f;
                elev.door_open_End = 0.0f;
                // Trigger when Faith walks inside the cab and passes its center or presses E on the button
                bool deep_inside = player_inside &&
                                   (std::abs(m_telemetry.position.x - fc_prev.x) <= he.x * 0.65f) &&
                                   (std::abs(m_telemetry.position.y - fc_prev.y) <= he.y * 0.65f);
                if ((elev.auto_trigger_on_enter && deep_inside) || button_pressed) {
                    elev.state = ElevatorState::DoorsClosing;
                    elev.timer = 0.0f;
                    m_telemetry.active_subtitle = "Elevator Activated - Doors Closing";
                }
                break;
            }

            case ElevatorState::DoorsClosing: {
                elev.timer += dt;
                float t01 = std::clamp(elev.timer / std::max(0.1f, elev.door_duration), 0.0f, 1.0f);
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
                float dur = std::max(0.1f, elev.ride_duration);

                // Evaluate UE3 InterpTrackMove PosTrack curve (Hermite smoothstep between keyframes)
                if (elev.keyframes.size() >= 2) {
                    if (elev.timer <= elev.keyframes.front().time) {
                        elev.current_pos = elev.keyframes.front().pos;
                    } else if (elev.timer >= elev.keyframes.back().time) {
                        elev.current_pos = elev.keyframes.back().pos;
                    } else {
                        for (size_t k = 0; k + 1 < elev.keyframes.size(); ++k) {
                            float t0 = elev.keyframes[k].time;
                            float t1 = elev.keyframes[k + 1].time;
                            if (elev.timer >= t0 && elev.timer <= t1) {
                                float seg_u = (t1 > t0 + 1e-5f) ? (elev.timer - t0) / (t1 - t0) : 1.0f;
                                float s = seg_u * seg_u * (3.0f - 2.0f * seg_u);
                                elev.current_pos = elev.keyframes[k].pos + (elev.keyframes[k + 1].pos - elev.keyframes[k].pos) * s;
                                break;
                            }
                        }
                    }
                } else {
                    float u = std::clamp(elev.timer / dur, 0.0f, 1.0f);
                    float s = u * u * (3.0f - 2.0f * u);
                    elev.current_pos = elev.start_pos + (elev.end_pos - elev.start_pos) * s;
                }

                float progress = std::clamp(elev.timer / dur, 0.0f, 1.0f);

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
                float t01 = std::clamp(elev.timer / std::max(0.1f, elev.door_duration), 0.0f, 1.0f);
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

        // Carry Faith smoothly with the moving elevator cab
        if (player_inside) {
            Vec3 cab_delta = elev.current_pos - elev.prev_pos;
            m_telemetry.position += cab_delta;
            float new_floor_z = (elev.current_pos + elev.cab_local_offset).z;
            if (m_telemetry.position.z < new_floor_z) {
                m_telemetry.position.z = new_floor_z;
                m_telemetry.grounded = true;
                if (m_telemetry.velocity.z < 0.0f) {
                    m_telemetry.velocity.z = 0.0f;
                }
            }
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

// -----------------------------------------------------------------------------
// Parkour Test Course Builder: Creates a Guaranteed Rich Gauntlet
// -----------------------------------------------------------------------------
void ParkourController::build_parkour_test_course(LevelScene& scene) {
    scene.map_name = "Parkour_Gauntlet_Oracle";
    scene.chapter_title = "PROLOGUE: THE EDGE";
    scene.player_spawn_pos = Vec3(0.0f, 0.0f, 100.0f);
    scene.player_spawn_yaw = 0.0f;

    // Clear colliders and build fresh test course
    scene.colliders.clear();
    scene.actors.clear();
    scene.enemies.clear();
    scene.checkpoints.clear();
    scene.checkpoint_infos.clear();
    scene.streaming_actions.clear();
    scene.all_streaming_packages.clear();
    scene.loaded_sublevel_packages.clear();
    scene.elevators.clear();

    // 1. Start Platform & Unobstructed 3800-unit Sprint Runway
    scene.colliders.emplace_back(Vec3(-500.0f, -300.0f, 0.0f), Vec3(3800.0f, 300.0f, 50.0f));

    // 2. Low Vault Hurdle (height 70)
    scene.colliders.emplace_back(Vec3(3200.0f, -150.0f, 50.0f), Vec3(3250.0f, 150.0f, 120.0f));

    // 3. Springboard Box (height 90, flagged springboard)
    LevelActor springboard_actor;
    springboard_actor.object_name = "Springboard_Box";
    springboard_actor.location = Vec3(3600.0f, 0.0f, 95.0f);
    springboard_actor.world_bounds = AABB(Vec3(3550.0f, -80.0f, 50.0f), Vec3(3650.0f, 80.0f, 140.0f));
    springboard_actor.is_springboard = true;
    springboard_actor.is_runner_vision = true;
    springboard_actor.is_collidable = true;
    scene.actors.push_back(springboard_actor);
    scene.colliders.push_back(springboard_actor.world_bounds);

    // 4. Canyon Gap & Wallrun Walls (Red Runner Vision)
    scene.colliders.emplace_back(Vec3(3800.0f, -300.0f, 0.0f), Vec3(5000.0f, 300.0f, 50.0f)); // canyon floor

    LevelActor left_wall;
    left_wall.object_name = "Wallrun_Left";
    left_wall.location = Vec3(4500.0f, -220.0f, 250.0f);
    left_wall.world_bounds = AABB(Vec3(4000.0f, -260.0f, 50.0f), Vec3(5000.0f, -200.0f, 450.0f));
    left_wall.is_runner_vision = true;
    left_wall.is_collidable = true;
    scene.actors.push_back(left_wall);
    scene.colliders.push_back(left_wall.world_bounds);

    LevelActor right_wall;
    right_wall.object_name = "Wallrun_Right";
    right_wall.location = Vec3(4500.0f, 220.0f, 250.0f);
    right_wall.world_bounds = AABB(Vec3(4000.0f, 200.0f, 50.0f), Vec3(5000.0f, 260.0f, 450.0f));
    right_wall.is_runner_vision = true;
    right_wall.is_collidable = true;
    scene.actors.push_back(right_wall);
    scene.colliders.push_back(right_wall.world_bounds);

    // 5. Wallclimb Tower with Top Ledge Grab
    LevelActor tower;
    tower.object_name = "Climb_Tower";
    tower.location = Vec3(5400.0f, 0.0f, 275.0f);
    tower.world_bounds = AABB(Vec3(5350.0f, -150.0f, 50.0f), Vec3(5450.0f, 150.0f, 500.0f));
    tower.is_runner_vision = true;
    tower.is_ledge = true;
    tower.is_collidable = true;
    scene.actors.push_back(tower);
    scene.colliders.push_back(tower.world_bounds);

    // Platform at top of tower
    scene.colliders.emplace_back(Vec3(5350.0f, -200.0f, 480.0f), Vec3(5600.0f, 200.0f, 500.0f));

    // 6. Zipline Cable from Tower Top to Far Rooftop
    LevelActor zipline;
    zipline.object_name = "Zipline_Cable";
    zipline.location = Vec3(5500.0f, 0.0f, 520.0f);
    zipline.end_point = Vec3(7000.0f, 0.0f, 120.0f);
    zipline.is_zipline = true;
    zipline.is_runner_vision = true;
    zipline.is_collidable = false;
    scene.actors.push_back(zipline);

    // Far rooftop landing
    scene.colliders.emplace_back(Vec3(6800.0f, -300.0f, 0.0f), Vec3(8000.0f, 300.0f, 100.0f));

    // 7. Slide Barrier under Low Ventilation Duct (clearance 60 units)
    LevelActor slide_duct;
    slide_duct.object_name = "Slide_Duct";
    slide_duct.location = Vec3(7300.0f, 0.0f, 220.0f);
    slide_duct.world_bounds = AABB(Vec3(7280.0f, -250.0f, 160.0f), Vec3(7340.0f, 250.0f, 300.0f));
    slide_duct.is_collidable = true;
    scene.actors.push_back(slide_duct);
    scene.colliders.push_back(slide_duct.world_bounds);

    // 8. Balance Beam across Rooftop Gap
    LevelActor balance_beam;
    balance_beam.object_name = "Balance_Pipe";
    balance_beam.location = Vec3(8400.0f, 0.0f, 110.0f);
    balance_beam.world_bounds = AABB(Vec3(8000.0f, -15.0f, 90.0f), Vec3(8800.0f, 15.0f, 110.0f));
    balance_beam.is_balance_beam = true;
    balance_beam.is_runner_vision = true;
    balance_beam.is_collidable = true;
    scene.actors.push_back(balance_beam);
    scene.colliders.push_back(balance_beam.world_bounds);

    // Destination combat arena leading into the Penthouse Elevator lobby
    scene.colliders.emplace_back(Vec3(8800.0f, -400.0f, 0.0f), Vec3(9840.0f, 400.0f, 100.0f));

    // 9. Enemy Guard for Disarm Training
    EnemyBot guard;
    guard.archetype = "PatrolCop";
    guard.position = Vec3(9300.0f, 0.0f, 100.0f);
    guard.weapon_name = "Colt1911";
    guard.health = 100.0f;
    guard.disarm_window = true;
    scene.enemies.push_back(guard);

    // 10. Interactive Penthouse Transition Elevator (S_Elevator_01: X=9840..10080, Y=-132.5..+132.5, Z=100 -> 680)
    // Connects the lower Combat Arena (Z=100) to the streamed Upper Penthouse Helipad Deck (Z=680).
    ElevatorInstance penthouse_elev;
    penthouse_elev.name = "Penthouse_Slc:mainlift";
    penthouse_elev.source_package = "Penthouse_Spt";
    penthouse_elev.cab_mesh_name = "S_Elevator_01";
    penthouse_elev.start_pos = Vec3(9960.0f, 0.0f, 100.0f);
    penthouse_elev.end_pos = Vec3(9960.0f, 0.0f, 680.0f);
    penthouse_elev.current_pos = penthouse_elev.start_pos;
    penthouse_elev.prev_pos = penthouse_elev.start_pos;
    penthouse_elev.cab_local_offset = Vec3(0.0f, 0.0f, 0.0f);
    penthouse_elev.cab_half_extents = Vec3(120.0f, 132.5f, 131.0f);
    penthouse_elev.ride_duration = 2.0f;
    penthouse_elev.door_duration = 0.4f;
    penthouse_elev.keyframes = {
        {0.0f, Vec3(9960.0f, 0.0f, 100.0f)},
        {2.0f, Vec3(9960.0f, 0.0f, 680.0f)}
    };
    penthouse_elev.button_pos = Vec3(10040.0f, 95.0f, 220.0f);
    penthouse_elev.target_checkpoint_idx = 5;
    penthouse_elev.stream_out_packages = {"Edge_Pt1_Art", "Edge_Pt1_Lw"};
    penthouse_elev.stream_in_packages = {"Edge_Pt2_Art", "Edge_Pt2_Bac", "Penthouse_Helipad_Art"};
    scene.elevators.push_back(penthouse_elev);

    // Streamed Upper Penthouse Helipad Deck at Z = 680 (walk out of the elevator at X = 10080..11200)
    scene.colliders.emplace_back(Vec3(10080.0f, -450.0f, 640.0f), Vec3(11200.0f, 450.0f, 680.0f));

    // 11. Checkpoints & TdCheckpoint.StreamingLevels along the Gauntlet
    scene.checkpoints.push_back(Vec3(0.0f, 0.0f, 100.0f));
    scene.checkpoints.push_back(Vec3(3000.0f, 0.0f, 100.0f));
    scene.checkpoints.push_back(Vec3(5400.0f, 0.0f, 550.0f));
    scene.checkpoints.push_back(Vec3(7000.0f, 0.0f, 150.0f));
    scene.checkpoints.push_back(Vec3(9000.0f, 0.0f, 150.0f));
    scene.checkpoints.push_back(Vec3(10400.0f, 0.0f, 680.0f)); // Upper Penthouse Helipad checkpoint after Elevator ride

    auto add_cp_info = [&](const std::string& name, int weight, bool def, const Vec3& pos,
                           const std::vector<std::string>& levels) {
        LevelCheckpointInfo cp;
        cp.object_name = "TdCheckpoint_" + std::to_string(scene.checkpoint_infos.size());
        cp.checkpoint_name = name;
        cp.checkpoint_weight = weight;
        cp.default_checkpoint = def;
        cp.location = pos;
        cp.streaming_levels = levels;
        scene.checkpoint_infos.push_back(cp);
    };
    add_cp_info("Start_Runway", 1, true, Vec3(0.0f, 0.0f, 100.0f), {"Edge_Pt1_Art", "Edge_Pt1_Lw", "Edge_Sky"});
    add_cp_info("Vault_Springboard", 2, false, Vec3(3000.0f, 0.0f, 100.0f), {"Edge_Pt1_Art", "Edge_Pt1_Lw", "Edge_Sky"});
    add_cp_info("Climb_Tower_Top", 3, false, Vec3(5400.0f, 0.0f, 550.0f), {"Edge_Pt1_Art", "Edge_Slc", "Edge_Sky"});
    add_cp_info("Zipline_Landing", 4, false, Vec3(7000.0f, 0.0f, 150.0f), {"Edge_Pt1_Art", "Edge_Slc", "Edge_Sky"});
    add_cp_info("Elevator_Lobby", 5, false, Vec3(9000.0f, 0.0f, 150.0f), {"Edge_Pt1_Art", "Penthouse_Slc", "Penthouse_Spt", "Edge_Sky"});
    add_cp_info("Penthouse_Helipad", 6, false, Vec3(10400.0f, 0.0f, 680.0f), {"Penthouse_Slc", "Penthouse_Spt", "Edge_Pt2_Art", "Edge_Pt2_Bac", "Penthouse_Helipad_Art", "Edge_Sky"});

    scene.all_streaming_packages = {
        "Edge_Pt1_Art", "Edge_Pt1_Lw", "Edge_Slc", "Penthouse_Slc",
        "Penthouse_Spt", "Edge_Pt2_Art", "Edge_Pt2_Bac", "Penthouse_Helipad_Art", "Edge_Sky"
    };
    scene.loaded_sublevel_packages = scene.checkpoint_infos.front().streaming_levels;

    // 12. Secret Courier Bag on Top of Tower
    LevelActor bag;
    bag.object_name = "Secret_Runner_Bag";
    bag.location = Vec3(5400.0f, 50.0f, 520.0f);
    bag.world_bounds = AABB(Vec3(5380.0f, 30.0f, 500.0f), Vec3(5420.0f, 70.0f, 540.0f));
    bag.is_bag = true;
    bag.is_runner_vision = true;
    scene.actors.push_back(bag);
}

} // namespace me

