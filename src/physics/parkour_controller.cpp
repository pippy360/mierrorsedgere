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
    m_telemetry.active_subtitle = "";

    m_momentum_timer = 0.0f;
    m_state_timer = 0.0f;
    m_wallrun_timer = 0.0f;
    m_slide_timer = 0.0f;
    m_coil_timer = 0.0f;
    m_turn_180_timer = 0.0f;
    m_roll_anim_timer = 0.0f;
    m_damage_cooldown = 0.0f;
    m_air_fall_start_z = spawn_pos.z;
    m_fall_peak_z = spawn_pos.z;
    m_crouch_landing_buffer = 0.0f;
    m_melee_cooldown = 0.0f;

    m_last_checkpoint_pos = spawn_pos;
    m_last_checkpoint_yaw = spawn_yaw;
}

void ParkourController::step(const InputFrame& input, float dt, LevelScene& scene) {
    if (dt <= 0.0f) return;

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
            case EMovement::MOVE_Snatch:
            case EMovement::MOVE_Melee:
            case EMovement::MOVE_MeleeAir:
            case EMovement::MOVE_MeleeWallrun:
            case EMovement::MOVE_Barge:
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
    // 180° Quick Turn
    if (input.turn_180 && m_turn_180_timer <= 0.0f) {
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
    cap.radius = 28.0f;
    cap.height = 96.0f;
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
    if (input.jump) {
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

    // Weapon mobility penalties
    if (m_telemetry.weapon.equipped) {
        target_speed *= m_telemetry.weapon.is_heavy ? 0.70f : 0.90f;
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

    // Continuous Swept Movement with Auto Step-Up
    Capsule cap;
    cap.base = m_telemetry.position;
    cap.radius = 34.0f;
    cap.height = (m_telemetry.move_state == EMovement::MOVE_Crouch) ? 48.0f : 96.0f;
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
            // Slide along collision normal
            m_telemetry.position += move_delta * hit.fraction;
            Vec3 remaining = move_delta * (1.0f - hit.fraction);
            remaining -= hit.normal * remaining.dot(hit.normal);
            m_telemetry.position += remaining;

            // Dampen velocity along normal
            m_telemetry.velocity -= hit.normal * m_telemetry.velocity.dot(hit.normal);
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

    // Subtle air control (0.025 - 0.09)
    Rotator view_rot = Rotator::from_degrees(0.0f, m_telemetry.yaw_deg, 0.0f);
    Vec3 fwd = view_rot.forward();
    Vec3 right = view_rot.right();
    Vec3 air_dir = (fwd * input.forward + right * input.strafe).normalized();

    constexpr float AIR_CONTROL = 450.0f;
    m_telemetry.velocity.x += air_dir.x * AIR_CONTROL * dt;
    m_telemetry.velocity.y += air_dir.y * AIR_CONTROL * dt;

    // Swept displacement
    Capsule cap;
    cap.base = m_telemetry.position;
    cap.radius = 34.0f;
    cap.height = 96.0f;
    cap.bottom_offset = (m_telemetry.move_state == EMovement::MOVE_Coil) ? m_config.coil_height_boost : 0.0f;

    Vec3 delta = m_telemetry.velocity * dt;
    TraceHit hit = sweep_capsule(cap, delta, scene);

    if (!hit.hit) {
        m_telemetry.position += delta;
    } else {
        m_telemetry.position += delta * std::max(0.0f, hit.fraction - 0.001f);
        m_telemetry.velocity -= hit.normal * m_telemetry.velocity.dot(hit.normal);
    }
}

// -----------------------------------------------------------------------------
// Wallrun Subsystem (Left & Right with 15° Camera Roll)
// -----------------------------------------------------------------------------
bool ParkourController::try_initiate_wallrun(const InputFrame& input, const LevelScene& scene) {
    (void)input;
    if (m_telemetry.weapon.is_heavy) return false;
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

        // Tangent along wall in player's forward direction
        Vec3 up(0, 0, 1);
        Vec3 tangent = is_right ? up.cross(hit.normal) : hit.normal.cross(up);
        tangent = tangent.normalized();

        float dot = fwd.dot(tangent);
        float angle_deg = std::acos(std::clamp(dot, -1.0f, 1.0f)) * RAD2DEG;

        if (angle_deg <= m_config.wallrun_max_angle_deg) {
            m_telemetry.move_state = is_right ? EMovement::MOVE_WallRunningRight : EMovement::MOVE_WallRunningLeft;
            m_telemetry.wall_normal = hit.normal;
            m_wall_tangent = tangent;
            m_wallrun_timer = 0.0f;
            m_telemetry.camera_roll_deg = is_right ? -15.0f : 15.0f;

            // Initial vertical boost
            m_telemetry.velocity.z = std::max(m_telemetry.velocity.z, m_config.wallrun_initial_z);

            // Align velocity with wall tangent
            float spd = std::max(m_telemetry.velocity.length_xy(), 350.0f);
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

    // Wallrun Jump
    if (input.jump) {
        m_telemetry.move_state = EMovement::MOVE_WallRunJump;
        m_telemetry.velocity = m_telemetry.wall_normal * 320.0f + m_wall_tangent * 400.0f + Vec3(0, 0, 450.0f);
        m_telemetry.camera_roll_deg = 0.0f;
        return;
    }

    // Wallrun Kick
    if (input.melee) {
        m_telemetry.move_state = EMovement::MOVE_MeleeWallrun;
        m_telemetry.velocity = m_telemetry.wall_normal * 200.0f + m_wall_tangent * 350.0f + Vec3(0, 0, -200.0f);
        m_telemetry.camera_roll_deg = 0.0f;
        return;
    }

    // Kinematics: early forward acceleration then decay
    float spd = m_telemetry.velocity.length_xy();
    if (m_wallrun_timer < 0.6f) {
        spd += m_config.wallrun_accel * dt;
    } else {
        spd -= m_config.wallrun_decel * dt;
    }
    m_telemetry.velocity.x = m_wall_tangent.x * spd;
    m_telemetry.velocity.y = m_wall_tangent.y * spd;

    // Reduced gravity while wallrunning
    m_telemetry.velocity.z -= (m_config.gravity * 0.35f) * dt;

    // Check if wall has ended
    Vec3 probe_start = m_telemetry.position + Vec3(0, 0, 70);
    TraceHit wall_check = trace_ray(probe_start, probe_start - m_telemetry.wall_normal * 80.0f, scene);

    if (!wall_check.hit || m_wallrun_timer >= m_config.wallrun_duration || spd < 150.0f) {
        m_telemetry.move_state = EMovement::MOVE_Falling;
        m_telemetry.camera_roll_deg = 0.0f;
        return;
    }

    // Sweep movement along wall
    Capsule cap;
    cap.base = m_telemetry.position;
    cap.radius = 34.0f;
    cap.height = 96.0f;
    Vec3 delta = m_telemetry.velocity * dt;
    TraceHit hit = sweep_capsule(cap, delta, scene);
    if (!hit.hit) {
        m_telemetry.position += delta;
    } else {
        m_telemetry.position += delta * hit.fraction;
        m_telemetry.move_state = EMovement::MOVE_Falling;
        m_telemetry.camera_roll_deg = 0.0f;
    }
}

// -----------------------------------------------------------------------------
// Wallclimb & 180° Turn Jump Subsystem
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
                m_telemetry.velocity.x = 0.0f;
                m_telemetry.velocity.y = 0.0f;
                m_telemetry.velocity.z = m_config.wallclimb_boost_z;
                m_state_timer = 0.0f;
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
        m_telemetry.velocity.x = 0.0f;
        m_telemetry.velocity.y = 0.0f;
        m_telemetry.velocity.z = m_config.wallclimb_boost_z;
        m_state_timer = 0.0f;
        return true;
    }
    return false;
}

void ParkourController::update_wallclimb(const InputFrame& input, float dt, const LevelScene& scene) {
    m_state_timer += dt;

    // 180° Turn Jump off vertical climb
    if (input.turn_180 || (input.jump && m_state_timer > 0.15f)) {
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

    if (!top_check.hit && m_telemetry.velocity.z <= 100.0f) {
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
// Slide & Low-Friction Crouch Movement Subsystem
// -----------------------------------------------------------------------------
void ParkourController::update_slide(const InputFrame& input, float dt, LevelScene& scene) {
    m_slide_timer += dt;

    // Slide Melee Kick
    if (input.melee) {
        m_telemetry.move_state = EMovement::MOVE_MeleeSlide;
        m_melee_cooldown = 0.5f;

        // Hit nearby bot
        for (auto& bot : scene.enemies) {
            if (bot.alive && m_telemetry.position.distance(bot.position) < 180.0f) {
                bot.health -= 60.0f;
                bot.stunned = true;
                if (bot.health <= 0.0f) bot.alive = false;
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
    if (input.jump) {
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
    cap.radius = 34.0f;
    cap.height = 48.0f; // low slide clearance!
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
// Combat, Firearms & Disarm Subsystem
// -----------------------------------------------------------------------------
void ParkourController::update_combat_and_weapons(const InputFrame& input, float dt, LevelScene& scene) {
    if (m_telemetry.weapon.cooldown > 0.0f) {
        m_telemetry.weapon.cooldown -= dt;
    }
    if (m_melee_cooldown > 0.0f) {
        m_melee_cooldown -= dt;
    }

    // 1. Weapon Disarm QTE (`input.disarm`)
    if (input.disarm) {
        for (auto& bot : scene.enemies) {
            if (bot.alive && bot.disarm_window && m_telemetry.position.distance(bot.position) < 200.0f) {
                // Execute MOVE_Snatch: snatch weapon into Faith's hands!
                m_telemetry.move_state = EMovement::MOVE_Snatch;
                m_state_timer = 0.0f;

                bot.disarm_window = false;
                bot.stunned = true;
                bot.health -= 40.0f;
                if (bot.health <= 0.0f) bot.alive = false;

                // Equip enemy's weapon
                m_telemetry.weapon.equipped = true;
                m_telemetry.weapon.name = bot.weapon_name;
                m_telemetry.weapon.ammo = (bot.weapon_name.find("Colt") != std::string::npos) ? 7 : 30;
                m_telemetry.weapon.max_ammo = m_telemetry.weapon.ammo;
                m_telemetry.weapon.damage = (bot.weapon_name.find("Colt") != std::string::npos) ? 45.0f : 35.0f;
                m_telemetry.weapon.is_heavy = (bot.weapon_name.find("G36") != std::string::npos ||
                                              bot.weapon_name.find("Remington") != std::string::npos ||
                                              bot.weapon_name.find("Minimi") != std::string::npos);

                m_telemetry.active_subtitle = "Weapon Disarmed: " + m_telemetry.weapon.name;
                break;
            }
        }
    }

    // 2. Firearm Shooting (`input.fire`)
    if (input.fire && m_telemetry.weapon.equipped && m_telemetry.weapon.cooldown <= 0.0f) {
        if (m_telemetry.weapon.ammo > 0) {
            m_telemetry.weapon.ammo--;
            m_telemetry.weapon.cooldown = 0.20f;

            // Trace bullet ray
            Rotator rot = Rotator::from_degrees(m_telemetry.pitch_deg, m_telemetry.yaw_deg, 0.0f);
            Vec3 eye = m_telemetry.position + Vec3(0, 0, m_telemetry.eye_height);
            Vec3 bullet_end = eye + rot.forward() * m_telemetry.weapon.range;

            TraceHit wall_hit = trace_ray(eye, bullet_end, scene);
            float max_dist = wall_hit.hit ? eye.distance(wall_hit.point) : m_telemetry.weapon.range;

            // Damage enemies intersecting ray
            for (auto& bot : scene.enemies) {
                if (bot.alive) {
                    Vec3 to_bot = bot.position - eye;
                    float proj = to_bot.dot(rot.forward());
                    if (proj > 0.0f && proj <= max_dist) {
                        Vec3 closest = eye + rot.forward() * proj;
                        if (closest.distance(bot.position + Vec3(0, 0, 50)) < 60.0f) {
                            bot.health -= m_telemetry.weapon.damage;
                            if (bot.health <= 0.0f) bot.alive = false;
                        }
                    }
                }
            }
        } else {
            // Weapon empty: auto drop!
            m_telemetry.weapon.equipped = false;
            m_telemetry.active_subtitle = "Weapon Empty - Dropped";
        }
    }

    // 3. Melee Strikes (`input.melee`)
    if (input.melee && m_melee_cooldown <= 0.0f && !m_telemetry.weapon.equipped) {
        m_melee_cooldown = 0.40f;
        if (!m_telemetry.grounded) {
            m_telemetry.move_state = EMovement::MOVE_MeleeAir;
        } else {
            m_telemetry.move_state = EMovement::MOVE_Melee;
        }

        Vec3 fwd = Rotator::from_degrees(0.0f, m_telemetry.yaw_deg, 0.0f).forward();
        for (auto& bot : scene.enemies) {
            if (bot.alive && m_telemetry.position.distance(bot.position) < 180.0f) {
                float dot = fwd.dot((bot.position - m_telemetry.position).normalized());
                if (dot > 0.5f) {
                    float dmg = (m_telemetry.move_state == EMovement::MOVE_MeleeAir) ? 100.0f : 35.0f;
                    bot.health -= dmg;
                    bot.stunned = true;
                    if (bot.health <= 0.0f) bot.alive = false;
                }
            }
        }
    }
}

// -----------------------------------------------------------------------------
// AI Bots Pursuit, Attack & Disarm Window Simulation
// -----------------------------------------------------------------------------
void ParkourController::update_ai_bots(float dt, LevelScene& scene) {
    for (auto& bot : scene.enemies) {
        if (!bot.alive) continue;

        if (bot.stunned) {
            bot.attack_timer += dt;
            if (bot.attack_timer >= 2.0f) {
                bot.stunned = false;
                bot.attack_timer = 0.0f;
            }
            continue;
        }

        float dist = bot.position.distance(m_telemetry.position);
        Vec3 dir_to_player = (m_telemetry.position - bot.position).normalized();
        bot.yaw_deg = std::atan2(dir_to_player.y, dir_to_player.x) * RAD2DEG;

        bot.attack_timer += dt;

        // Tutorial sparring trainer (Celeste) stays at her training post with disarm window ready
        if (bot.weapon_name.find("TutorialTrainer") != std::string::npos) {
            bot.disarm_window = true;
            continue;
        }

        if (dist > 1500.0f) {
            // Hold guard post until player enters engagement radius
            bot.disarm_window = false;
        } else if (dist > 180.0f) {
            // Ranged engagement
            bot.disarm_window = false;
            if (bot.attack_timer >= 1.5f) {
                bot.attack_timer = 0.0f;
                // Deal damage if Faith isn't evading
                if (m_telemetry.move_state != EMovement::MOVE_Slide &&
                    m_telemetry.move_state != EMovement::MOVE_SkillRoll &&
                    m_telemetry.move_state != EMovement::MOVE_WallRunningLeft &&
                    m_telemetry.move_state != EMovement::MOVE_WallRunningRight) {
                    m_telemetry.health = std::max(0.0f, m_telemetry.health - 12.0f);
                    m_damage_cooldown = m_config.health_regen_delay;
                }
            }
        } else {
            // Melee range: open Red Flashing Disarm Window!
            if (bot.attack_timer >= 1.0f && bot.attack_timer <= 1.6f) {
                bot.disarm_window = true;
            } else if (bot.attack_timer > 1.6f) {
                bot.disarm_window = false;
                bot.attack_timer = 0.0f;
                // Melee strike hit player
                m_telemetry.health = std::max(0.0f, m_telemetry.health - 25.0f);
                m_damage_cooldown = m_config.health_regen_delay;
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

