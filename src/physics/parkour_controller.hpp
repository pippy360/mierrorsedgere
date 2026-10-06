#pragma once

#include "../math/types.hpp"
#include "collision_world.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace me {

// =============================================================================
// ParkourController: Discrete Kinematic Simulation Engine for Faith Connors
// Implements 100% authentic Mirror's Edge (TdGame.u / DefaultPawnMovement.ini)
// parkour mechanics, swept collision against the real UE3 level collision
// (LevelScene::collision + the moving elevator parts), weapons, combat disarm, and AI.
// =============================================================================
class ParkourController {
public:
    explicit ParkourController(const MovementConfig& config = MovementConfig());
    ~ParkourController() = default;

    // Reset player position and orientation
    void reset(const Vec3& spawn_pos, float spawn_yaw = 0.0f);

    // Primary simulation step: fixed 120Hz/60Hz substepped physics
    void step(const InputFrame& input, float dt, LevelScene& scene);

    // Telemetry and State Accessors
    [[nodiscard]] const PlayerTelemetry& get_telemetry() const { return m_telemetry; }
    [[nodiscard]] PlayerTelemetry& get_telemetry() { return m_telemetry; }

    [[nodiscard]] const MovementConfig& get_config() const { return m_config; }
    [[nodiscard]] MovementConfig& get_config() { return m_config; }

    [[nodiscard]] Vec3 get_position() const { return m_telemetry.position; }
    [[nodiscard]] Vec3 get_velocity() const { return m_telemetry.velocity; }
    [[nodiscard]] EMovement get_move_state() const { return m_telemetry.move_state; }
    [[nodiscard]] float get_yaw() const { return m_telemetry.yaw_deg; }
    [[nodiscard]] float get_pitch() const { return m_telemetry.pitch_deg; }
    [[nodiscard]] float get_roll() const { return m_telemetry.camera_roll_deg; }
    [[nodiscard]] float get_fov() const { return m_telemetry.fov_deg; }
    [[nodiscard]] bool is_grounded() const { return m_telemetry.grounded; }
    [[nodiscard]] int get_active_checkpoint() const { return m_telemetry.active_checkpoint; }
    [[nodiscard]] int get_bags_collected() const { return m_telemetry.bags_collected; }
    [[nodiscard]] const WeaponState& get_weapon() const { return m_telemetry.weapon; }
    void equip_weapon(const std::string& weapon_name);

    void set_position(const Vec3& pos) { m_telemetry.position = pos; }
    void set_velocity(const Vec3& vel) { m_telemetry.velocity = vel; }
    void set_rotation(float yaw, float pitch, float roll) {
        m_telemetry.yaw_deg = yaw;
        m_telemetry.pitch_deg = pitch;
        m_telemetry.camera_roll_deg = roll;
    }

private:
    // Core Subsystems
    void update_camera_and_inputs(const InputFrame& input, float dt, const LevelScene& scene);
    void update_reaction_time(const InputFrame& input, float dt, float& effective_dt);
    void update_combat_and_weapons(const InputFrame& input, float dt, LevelScene& scene);
    void update_ai_bots(float dt, LevelScene& scene);
    void update_health_and_regen(float dt);
    void update_checkpoints_and_volumes(LevelScene& scene);
    void update_elevators(const InputFrame& input, float dt, LevelScene& scene);

    // Collision Detection and Swept Physics (TdPawn default cylinder: Radius=30, Height=90).
    // UE3 PHYS_Walking / PHYS_Falling sweep the axis-aligned box of the collision cylinder.
    struct Capsule {
        Vec3 base;
        float radius = 30.0f;
        float height = 90.0f;
        float bottom_offset = 0.0f;

        [[nodiscard]] AABB to_aabb() const {
            return AABB(
                Vec3(base.x - radius, base.y - radius, base.z + bottom_offset),
                Vec3(base.x + radius, base.y + radius, base.z + height)
            );
        }
    };

    struct TraceHit {
        bool hit = false;
        bool start_penetrating = false;  // the query began overlapping the blocking surface
        float fraction = 1.0f;
        Vec3 normal{0.0f, 0.0f, 1.0f};
        Vec3 point{0.0f, 0.0f, 0.0f};    // box centre (sweeps) / impact point (traces) at contact
        const LevelActor* actor = nullptr;
        int32_t actor_index = -1;        // LevelScene::actors index (-1 = BSP / none)
    };

    struct FloorHit {
        float z = 0.0f;
        Vec3 normal{0.0f, 0.0f, 1.0f};
        int32_t actor_index = -1;
    };

    // Pawn box swept against the level collision and the moving elevator parts (BlockNonZeroExtent).
    TraceHit sweep_capsule(const Capsule& capsule, const Vec3& delta, const LevelScene& scene) const;
    // Zero-extent probe. Movement probes test what blocks pawn movement (BlockNonZeroExtent);
    // weapon traces pass COLL_BlockZeroExtent.
    TraceHit trace_ray(const Vec3& start, const Vec3& end, const LevelScene& scene,
                       uint8_t channels = COLL_BlockNonZeroExtent) const;
    // UE3 physWalking floor check: sweeps the pawn box down to `probe_depth` below the feet.
    bool check_ground(const LevelScene& scene, float probe_depth, float height, FloorHit& out) const;
    // True when the pawn box of `height` fits at the current position (e.g. room to stand up).
    bool has_room(float height, const LevelScene& scene) const;
    // True when the pawn box of `height` fits with its feet at `feet` (e.g. on top of a ledge).
    bool has_room_at(const Vec3& feet, float height, const LevelScene& scene) const;

    // Swept movement helpers: the pawn position only ever changes through collision sweeps.
    TraceHit move_swept(const Vec3& delta, float height, float bottom_offset, const LevelScene& scene);
    TraceHit move_and_slide(const Vec3& delta, float height, float bottom_offset, const LevelScene& scene);
    void walk_move(const Vec3& delta, float height, const LevelScene& scene);
    bool step_up(const Vec3& delta, float height, const LevelScene& scene);
    void integrate_ballistic(float dt, const LevelScene& scene);
    // Top of the ledge in front of a wall (wall_normal faces the pawn), at most max_rise above the feet.
    bool find_ledge_top(const Vec3& wall_normal, float max_rise, const LevelScene& scene, float& ledge_z) const;
    // Mantle: rise to ledge_z, then move over the ledge away from the wall.
    bool climb_onto_ledge(const Vec3& wall_normal, float ledge_z, const LevelScene& scene);

    // Parkour Movement Resolvers
    void update_ground_locomotion(const InputFrame& input, float dt, const LevelScene& scene);
    void update_air_locomotion(const InputFrame& input, float dt, const LevelScene& scene);
    void update_wallrun(const InputFrame& input, float dt, const LevelScene& scene);
    void update_wallclimb(const InputFrame& input, float dt, const LevelScene& scene);
    void update_slide(const InputFrame& input, float dt, LevelScene& scene);
    void update_ledge_grab(const InputFrame& input, float dt, const LevelScene& scene);
    void update_zipline(const InputFrame& input, float dt, const LevelScene& scene);
    void update_swing_bar(const InputFrame& input, float dt, const LevelScene& scene);
    void update_balance(const InputFrame& input, float dt, const LevelScene& scene);
    void update_skill_roll(const InputFrame& input, float dt);

    // Transition Helpers
    bool try_initiate_wallrun(const InputFrame& input, const LevelScene& scene);
    bool try_initiate_wallclimb(const InputFrame& input, const LevelScene& scene);
    bool try_initiate_vault(const InputFrame& input, const LevelScene& scene);
    bool try_initiate_springboard(const InputFrame& input, const LevelScene& scene);
    bool try_initiate_ledge_grab(const LevelScene& scene);
    bool try_initiate_zipline(const LevelScene& scene);

    // Internal Simulation State
    MovementConfig m_config;
    PlayerTelemetry m_telemetry;

    // Movement Timers & Accumulators
    float m_momentum_timer = 0.0f;
    float m_state_timer = 0.0f;
    float m_wallrun_timer = 0.0f;
    float m_wallrun_cooldown = 0.0f;
    float m_wallrun_begin_speed = 0.0f;
    float m_slide_timer = 0.0f;
    float m_coil_timer = 0.0f;
    float m_turn_180_timer = 0.0f;
    float m_turn_180_target_yaw = 0.0f;
    float m_roll_anim_timer = 0.0f;
    float m_damage_cooldown = 0.0f;
    float m_air_fall_start_z = 0.0f;
    float m_fall_peak_z = 0.0f;
    float m_crouch_landing_buffer = 0.0f;
    float m_melee_cooldown = 0.0f;
    int m_melee_combo_index = 0;
    float m_melee_combo_reset_timer = 0.0f;
    int m_weapon_cycle_index = 0;
    bool m_jump_consumed = false;
    bool m_prev_turn_180 = false;

    // Wallrun / Climb / Zipline vectors
    Vec3 m_wall_tangent{0.0f, 0.0f, 0.0f};
    Vec3 m_last_wallrun_normal{0.0f, 0.0f, 0.0f};
    Vec3 m_zipline_start{0.0f, 0.0f, 0.0f};
    Vec3 m_zipline_end{0.0f, 0.0f, 0.0f};
    Vec3 m_swing_anchor{0.0f, 0.0f, 0.0f};
    float m_swing_angle = 0.0f;
    float m_swing_angular_vel = 0.0f;
    float m_ledge_z = 0.0f;  // top of the grabbed ledge (MOVE_Grabbing / MOVE_GrabPullUp)

    // UE3 Pawn.Base: the actor the pawn stands on (moving elevator parts carry the pawn).
    int32_t m_base_actor = -1;

    // Spawn / Respawn tracking
    Vec3 m_last_checkpoint_pos{0.0f, 0.0f, 100.0f};
    float m_last_checkpoint_yaw = 0.0f;
};

} // namespace me
