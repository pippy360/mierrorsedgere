#pragma once

#include "../math/types.hpp"
#include "collision_world.hpp"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace me {

// Piecewise-linear FInterpCurveFloat (CIM_Linear keys), evaluated like FInterpCurve::Eval: clamped
// to the first / last key outside the key range.
struct LinearCurve {
    std::vector<std::pair<float, float>> points;
    [[nodiscard]] float eval(float x) const;
};

// =============================================================================
// ParkourController: Discrete Kinematic Simulation Engine for Faith Connors
//
// Implements the Mirror's Edge movement set as the decompiled TdGame.u scripts (TdMove_*, TdPawn,
// TdPlayerController) and the native TdPawn ground model (GetSprintAcceleration /
// GetWalkAcceleration / CalcVelocity) describe it, with swept collision against the real UE3 level
// collision (LevelScene::collision + the moving elevator parts), weapons, combat disarm, and AI.
// =============================================================================
class ParkourController {
public:
    explicit ParkourController(const MovementConfig& config = MovementConfig());
    ~ParkourController() = default;

    // Reset player position and orientation
    void reset(const Vec3& spawn_pos, float spawn_yaw = 0.0f);

    // Retail replay harness (tools/retail/replay.py, src/tools/replay_main.cpp): reset the pawn to a
    // recorded retail frame - feet, velocity, view - and hand it the state retail's PlayerMove carries
    // into that frame, which no recording holds and the harness rebuilds by retail's own rules:
    // TdPawn.SpeedSprintEnergy, TdPlayerController.AccelerationTime, the walk-stop still to run
    // (seconds, 0 = none), the jump chain (0 none, 1 Falling after a jump, 2 in MOVE_Jump) with its
    // PreJumpMomentum, and which of jump / crouch the frame before held (so a held key is no press).
    struct AnchorState {
        float sprint_energy = 0.0f;
        float accel_time = 0.0f;
        float stop_left = 0.0f;
        int jump = 0;
        float pre_jump_momentum = 0.0f;
        bool held_jump = false;
        bool held_crouch = false;
    };
    void anchor(const Vec3& feet, const Vec3& velocity, float yaw, float pitch, bool grounded,
                const AnchorState& state);

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
    void update_barge_doors(const InputFrame& input, float dt, LevelScene& scene);

    // Collision Detection and Swept Physics. TdPawn's cylinder is Radius=30, CollisionHeight=90
    // (a half-height: the pawn is 180 tall, 122 when crouched / sliding / coiled). UE3 PHYS_Walking
    // and PHYS_Falling sweep the axis-aligned box of the collision cylinder.
    struct Capsule {
        Vec3 base;
        float radius = 30.0f;
        float height = 180.0f;
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

    // A vertical wall face found by a probe: its outward normal and the distance from the pawn
    // centre to the face along -normal.
    struct WallFace {
        bool found = false;
        Vec3 normal{0.0f, 0.0f, 0.0f};
        Vec3 point{0.0f, 0.0f, 0.0f};
        float distance = 0.0f;
    };

    // A ledge (walkable top of a wall ahead): where to hang / vault / climb.
    struct Ledge {
        bool found = false;
        Vec3 normal{0.0f, 0.0f, 0.0f};  // wall normal (faces the pawn)
        float top_z = 0.0f;             // walkable top
        float wall_distance = 0.0f;     // pawn centre -> wall face, along -normal
        Vec3 top_point{0.0f, 0.0f, 0.0f};
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
    // True when an arbitrary box (centre, half extents) overlaps nothing that blocks the pawn.
    bool box_free(const Vec3& centre, const Vec3& extent, const LevelScene& scene) const;
    // Arbitrary box (centre, half extents) swept by `delta` (the game's MovementTrace with an extent).
    TraceHit sweep_box(const Vec3& centre, const Vec3& extent, const Vec3& delta, const LevelScene& scene) const;
    // Headroom above the feet, at most `max_rise` (ceilings cut jump heights).
    float headroom(float max_rise, const LevelScene& scene) const;
    // Thin horizontal slab of the pawn's footprint swept along `dir` at `height` above the feet
    // (the game's wall checks: a trace with the pawn's extent at one height).
    WallFace probe_wall(const Vec3& dir, float reach, float height, const LevelScene& scene) const;
    // Walkable top of the obstacle whose face is `wall`, between min_rise and max_rise above the feet.
    Ledge find_ledge(const Vec3& dir, float reach, float min_rise, float max_rise, const LevelScene& scene) const;

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

    // TdPawn ground model (native): what the controller asks for and how it becomes velocity.
    [[nodiscard]] Vec3 input_direction(const InputFrame& input) const;
    [[nodiscard]] Vec3 facing_forward() const;
    [[nodiscard]] Vec3 facing_right() const;
    Vec3 sprint_acceleration(const Vec3& dir, const Vec3& vel, float dt, bool falling, float turn_uu);
    Vec3 walk_acceleration(const Vec3& dir, const InputFrame& input, const Vec3& vel, bool falling);
    // TdPlayerController.PlayerWalking.PlayerMove: evaluated once per frame (frame dt and turn),
    // then held for every physics sub-step of that frame.
    Vec3 controller_acceleration(const InputFrame& input, bool falling);
    void calc_velocity(const Vec3& accel, float dt, float speed_mod, float friction);
    [[nodiscard]] float speed_for_height(float height) const;

    // Parkour Movement Resolvers
    void update_ground_locomotion(const InputFrame& input, float dt, const LevelScene& scene);
    void update_air_locomotion(const InputFrame& input, float dt, const LevelScene& scene);
    void update_wallrun(const InputFrame& input, float dt, const LevelScene& scene);
    void update_wallclimb(const InputFrame& input, float dt, const LevelScene& scene);
    void update_slide(const InputFrame& input, float dt, LevelScene& scene);
    void update_ledge_grab(const InputFrame& input, float dt, const LevelScene& scene);
    void update_vault(const InputFrame& input, float dt, const LevelScene& scene);
    void update_zipline(const InputFrame& input, float dt, const LevelScene& scene);
    void update_swing_bar(const InputFrame& input, float dt, const LevelScene& scene);
    void update_climb(const InputFrame& input, float dt, const LevelScene& scene);
    void update_balance(const InputFrame& input, float dt, const LevelScene& scene);
    void update_landing_moves(const InputFrame& input, float dt, const LevelScene& scene);

    // Transition Helpers
    bool try_initiate_wallrun(const InputFrame& input, const LevelScene& scene);
    bool try_initiate_wallclimb(const InputFrame& input, const LevelScene& scene);
    bool try_initiate_vault(const InputFrame& input, const LevelScene& scene);
    bool try_initiate_springboard(const InputFrame& input, const LevelScene& scene);
    bool try_initiate_ledge_grab(const InputFrame& input, const LevelScene& scene);
    bool try_initiate_zipline(const LevelScene& scene);
    bool try_initiate_climb(const InputFrame& input, const LevelScene& scene);
    bool try_initiate_balance(const LevelScene& scene);
    bool try_initiate_dodge_jump(const InputFrame& input);
    void start_jump(const LevelScene& scene);
    void land(const FloorHit& floor, const LevelScene& scene);
    void leave_ground(EMovement air_move);
    void set_stance(float eye_height);
    [[nodiscard]] bool can_skill_roll() const;
    [[nodiscard]] bool jump_pressed() const { return m_jump_buffer > 0.0f; }
    void consume_jump() { m_jump_buffer = 0.0f; }

    // Internal Simulation State
    MovementConfig m_config;
    PlayerTelemetry m_telemetry;
    LinearCurve m_accel_curve;  // AccelCurve_LightWeapon: acceleration against speed

    // TdPawn / TdPlayerController ground model state
    float m_sprint_energy = 0.0f;      // TdPawn.SpeedSprintEnergy (speed above SpeedMaxBaseVelocity)
    float m_accel_time = 0.0f;         // TdPlayerController.AccelerationTime
    float m_stop_timer = 0.0f;         // TdPlayerController.bIsStopping (tap-stop)
    float m_frame_turn_uu = 0.0f;      // |aTurn| this frame, in rotation units (65536 per turn)
    float m_frame_dt = 1.0f / 60.0f;
    Vec3 m_frame_accel{0.0f, 0.0f, 0.0f};  // this frame's PlayerMove acceleration (Pawn.Acceleration)
    bool m_frame_accel_valid = false;

    // Input edges and buffers
    bool m_prev_jump = false;
    bool m_prev_crouch = false;
    bool m_prev_turn_180 = false;
    bool m_crouch_pressed = false;     // fresh crouch press this frame
    float m_jump_buffer = 0.0f;        // JumpTapTime window after a fresh jump press
    float m_roll_trigger_time = -100.0f; // TdPawn.RollTriggerTime (sim time of the arming press)

    // Movement Timers & Accumulators
    float m_state_timer = 0.0f;
    float m_wallrun_cooldown = 0.0f;
    float m_wallrun_begin_speed = 0.0f;
    float m_slide_timer = 0.0f;
    float m_slide_yaw = 0.0f;          // body yaw during the slide (the velocity follows it)
    float m_coil_timer = 0.0f;
    float m_turn_timer = 0.0f;
    float m_turn_total = 0.0f;
    float m_turn_target_yaw = 0.0f;
    float m_landing_timer = 0.0f;
    float m_damage_cooldown = 0.0f;
    float m_air_fall_start_z = 0.0f;
    float m_fall_peak_z = 0.0f;
    float m_melee_cooldown = 0.0f;
    int m_melee_combo_index = 0;
    float m_melee_combo_reset_timer = 0.0f;
    int m_weapon_cycle_index = 0;
    bool m_jump_consumed = false;

    // Jump / fall bookkeeping (TdMove_Jump / TdMove_Landing)
    Vec3 m_last_jump_location{0.0f, 0.0f, 0.0f};  // TdPawn.LastJumpLocation
    float m_pre_jump_momentum = 0.0f;              // TdMove_Jump.PreJumpMomentum
    EMovement m_takeoff_move = EMovement::MOVE_Falling;  // the move that put the pawn in the air
    bool m_turned_in_air = false;                  // landing out of TdMove_180TurnInAir = LandBackwards
    int m_consecutive_wallruns = 0;                // TdMove_WallRun.ConsequtiveWallruns
    bool m_wall_turned = false;                    // TdMove_WallRun.bTurned90FromWall
    float m_illegal_wall_timer = 0.0f;             // bIllegalLedgeTimer: no re-attach to the last wall

    // Wallrun / Climb / Balance / Zipline vectors
    Vec3 m_wall_tangent{0.0f, 0.0f, 0.0f};
    Vec3 m_last_wallrun_normal{0.0f, 0.0f, 0.0f};
    float m_into_wallclimb_speed = 0.0f;           // TdMove_WallClimb.IntoWallClimbSpeed
    bool m_wallclimb_reached = false;              // TdMove_WallClimb.bHasReachedWall
    Vec3 m_zipline_start{0.0f, 0.0f, 0.0f};
    Vec3 m_zipline_end{0.0f, 0.0f, 0.0f};
    Vec3 m_climb_base{0.0f, 0.0f, 0.0f};
    Vec3 m_climb_top{0.0f, 0.0f, 0.0f};
    Vec3 m_climb_normal{0.0f, 0.0f, 0.0f};
    float m_climb_cooldown = 0.0f;
    Vec3 m_balance_start{0.0f, 0.0f, 0.0f};
    Vec3 m_balance_end{0.0f, 0.0f, 0.0f};
    float m_balance_lean = 0.0f;
    float m_balance_cooldown = 0.0f;
    Vec3 m_swing_anchor{0.0f, 0.0f, 0.0f};
    float m_swing_angle = 0.0f;
    float m_swing_angular_vel = 0.0f;
    float m_ledge_z = 0.0f;  // top of the grabbed ledge (MOVE_Grabbing / MOVE_GrabPullUp)
    float m_hang_time = 0.0f;

    // Timed root-motion paths (TdMove_SpeedVault / TdMove_SpringBoard)
    Vec3 m_path_p0{0.0f, 0.0f, 0.0f};
    Vec3 m_path_p1{0.0f, 0.0f, 0.0f};
    Vec3 m_path_p2{0.0f, 0.0f, 0.0f};
    float m_path_t1 = 0.0f;
    float m_path_t2 = 0.0f;
    Vec3 m_path_exit_velocity{0.0f, 0.0f, 0.0f};
    EMovement m_path_end_move = EMovement::MOVE_Walking;

    // UE3 Pawn.Base: the actor the pawn stands on (moving elevator parts carry the pawn).
    int32_t m_base_actor = -1;

    // Spawn / Respawn tracking
    Vec3 m_last_checkpoint_pos{0.0f, 0.0f, 100.0f};
    float m_last_checkpoint_yaw = 0.0f;
    float m_death_timer = 0.0f;
    float m_death_total_duration = 1.35f;
};

} // namespace me
