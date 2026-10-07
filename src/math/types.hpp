#pragma once

#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <memory>
#include <string_view>

namespace me {

struct SceneMaterialLibrary;  // assets/scene_materials.hpp
class CollisionWorld;         // physics/collision_world.hpp

constexpr float PI = 3.14159265358979323846f;
constexpr float DEG2RAD = PI / 180.0f;
constexpr float RAD2DEG = 180.0f / PI;
constexpr float UE_ROT2DEG = 360.0f / 65536.0f;
constexpr float DEG2UE_ROT = 65536.0f / 360.0f;
constexpr float UE_ROT2RAD = (2.0f * PI) / 65536.0f;
constexpr float RAD2UE_ROT = 65536.0f / (2.0f * PI);

// -----------------------------------------------------------------------------
// 3D Vector with Unreal Engine 3 (X forward, Y right, Z up) conventions
// -----------------------------------------------------------------------------
struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    constexpr Vec3() = default;
    constexpr Vec3(float in_x, float in_y, float in_z) : x(in_x), y(in_y), z(in_z) {}

    constexpr Vec3 operator+(const Vec3& o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
    constexpr Vec3 operator-(const Vec3& o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
    constexpr Vec3 operator*(float s) const { return Vec3(x * s, y * s, z * s); }
    constexpr Vec3 operator/(float s) const { float inv = 1.0f / s; return Vec3(x * inv, y * inv, z * inv); }
    constexpr Vec3 operator-() const { return Vec3(-x, -y, -z); }

    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
    Vec3& operator/=(float s) { float inv = 1.0f / s; x *= inv; y *= inv; z *= inv; return *this; }

    constexpr bool operator==(const Vec3& o) const { return x == o.x && y == o.y && z == o.z; }
    constexpr bool operator!=(const Vec3& o) const { return !(*this == o); }

    [[nodiscard]] constexpr float dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }

    [[nodiscard]] constexpr Vec3 cross(const Vec3& o) const {
        return Vec3(
            y * o.z - z * o.y,
            z * o.x - x * o.z,
            x * o.y - y * o.x
        );
    }

    [[nodiscard]] float length() const { return std::sqrt(x * x + y * y + z * z); }
    [[nodiscard]] constexpr float length_sq() const { return x * x + y * y + z * z; }
    [[nodiscard]] float length_xy() const { return std::sqrt(x * x + y * y); }
    [[nodiscard]] constexpr float length_xy_sq() const { return x * x + y * y; }

    [[nodiscard]] Vec3 normalized() const {
        float len = length();
        return (len > 1e-6f) ? (*this / len) : Vec3(0.0f, 0.0f, 0.0f);
    }

    [[nodiscard]] Vec3 normalized_xy() const {
        float len = length_xy();
        return (len > 1e-6f) ? Vec3(x / len, y / len, 0.0f) : Vec3(0.0f, 0.0f, 0.0f);
    }

    [[nodiscard]] float distance(const Vec3& o) const { return (*this - o).length(); }
    [[nodiscard]] float distance_xy(const Vec3& o) const { return (*this - o).length_xy(); }
};

inline Vec3 operator*(float s, const Vec3& v) { return v * s; }

// -----------------------------------------------------------------------------
// Rotator (Pitch, Yaw, Roll in Unreal Engine 3 65536 = 360° units)
// -----------------------------------------------------------------------------
struct Rotator {
    float pitch = 0.0f; // Look up/down (elevation)
    float yaw = 0.0f;   // Look left/right (azimuth)
    float roll = 0.0f;  // Camera tilt/banking

    constexpr Rotator() = default;
    constexpr Rotator(float p, float y, float r) : pitch(p), yaw(y), roll(r) {}

    static Rotator from_degrees(float p_deg, float y_deg, float r_deg) {
        return Rotator(p_deg * DEG2UE_ROT, y_deg * DEG2UE_ROT, r_deg * DEG2UE_ROT);
    }

    static Rotator from_radians(float p_rad, float y_rad, float r_rad) {
        return Rotator(p_rad * RAD2UE_ROT, y_rad * RAD2UE_ROT, r_rad * RAD2UE_ROT);
    }

    [[nodiscard]] Vec3 to_degrees() const {
        return Vec3(pitch * UE_ROT2DEG, yaw * UE_ROT2DEG, roll * UE_ROT2DEG);
    }

    [[nodiscard]] Vec3 to_radians() const {
        return Vec3(pitch * UE_ROT2RAD, yaw * UE_ROT2RAD, roll * UE_ROT2RAD);
    }

    [[nodiscard]] Vec3 forward() const {
        Vec3 rad = to_radians();
        float p = rad.x;
        float y = rad.y;
        return Vec3(
            std::cos(p) * std::cos(y),
            std::cos(p) * std::sin(y),
            std::sin(p)
        );
    }

    [[nodiscard]] Vec3 right() const {
        Vec3 rad = to_radians();
        float p = rad.x;
        float y = rad.y;
        float r = rad.z;
        return Vec3(
            -std::cos(r) * std::sin(y) + std::sin(r) * std::sin(p) * std::cos(y),
             std::cos(r) * std::cos(y) + std::sin(r) * std::sin(p) * std::sin(y),
            -std::sin(r) * std::cos(p)
        );
    }

    [[nodiscard]] Vec3 up() const {
        Vec3 rad = to_radians();
        float p = rad.x;
        float y = rad.y;
        float r = rad.z;
        return Vec3(
            -std::sin(r) * std::sin(y) - std::cos(r) * std::sin(p) * std::cos(y),
             std::sin(r) * std::cos(y) - std::cos(r) * std::sin(p) * std::sin(y),
             std::cos(r) * std::cos(p)
        );
    }
};

// -----------------------------------------------------------------------------
// 4x4 Matrix (Column-major format for standard GPU / Metal compatibility)
// -----------------------------------------------------------------------------
struct Mat4 {
    float m[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };

    static Mat4 identity() {
        return Mat4{};
    }

    static Mat4 translation(const Vec3& t) {
        Mat4 r = identity();
        r.m[12] = t.x;
        r.m[13] = t.y;
        r.m[14] = t.z;
        return r;
    }

    static Mat4 scale(const Vec3& s) {
        Mat4 r = identity();
        r.m[0] = s.x;
        r.m[5] = s.y;
        r.m[10] = s.z;
        return r;
    }

    static Mat4 rotation_x(float rad) {
        Mat4 r = identity();
        float c = std::cos(rad);
        float s = std::sin(rad);
        r.m[5] = c;
        r.m[6] = s;
        r.m[9] = -s;
        r.m[10] = c;
        return r;
    }

    static Mat4 rotation_y(float rad) {
        Mat4 r = identity();
        float c = std::cos(rad);
        float s = std::sin(rad);
        r.m[0] = c;
        r.m[2] = -s;
        r.m[8] = s;
        r.m[10] = c;
        return r;
    }

    static Mat4 rotation_z(float rad) {
        Mat4 r = identity();
        float c = std::cos(rad);
        float s = std::sin(rad);
        r.m[0] = c;
        r.m[1] = s;
        r.m[4] = -s;
        r.m[5] = c;
        return r;
    }

    static Mat4 perspective(float fov_y_rad, float aspect, float z_near, float z_far) {
        Mat4 r{};
        float tan_half_fov = std::tan(fov_y_rad * 0.5f);
        r.m[0] = 1.0f / (aspect * tan_half_fov);
        r.m[5] = 1.0f / tan_half_fov;
        r.m[10] = z_far / (z_far - z_near);
        r.m[11] = 1.0f;
        r.m[14] = -(z_far * z_near) / (z_far - z_near);
        r.m[15] = 0.0f;
        return r;
    }

    static Mat4 ortho(float left, float right, float bottom, float top, float z_near, float z_far) {
        Mat4 r{};
        r.m[0] = 2.0f / (right - left);
        r.m[5] = 2.0f / (top - bottom);
        r.m[10] = 1.0f / (z_far - z_near);
        r.m[12] = -(right + left) / (right - left);
        r.m[13] = -(top + bottom) / (top - bottom);
        r.m[14] = -z_near / (z_far - z_near);
        r.m[15] = 1.0f;
        return r;
    }

    static Mat4 look_at(const Vec3& eye, const Vec3& target, const Vec3& up) {
        Vec3 f = (target - eye).normalized();
        Vec3 s = up.cross(f).normalized();
        Vec3 u = f.cross(s);

        Mat4 r = identity();
        r.m[0] = s.x;
        r.m[4] = s.y;
        r.m[8] = s.z;
        r.m[12] = -s.dot(eye);

        r.m[1] = u.x;
        r.m[5] = u.y;
        r.m[9] = u.z;
        r.m[13] = -u.dot(eye);

        r.m[2] = f.x;
        r.m[6] = f.y;
        r.m[10] = f.z;
        r.m[14] = -f.dot(eye);

        return r;
    }

    Mat4 operator*(const Mat4& b) const {
        Mat4 res{};
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    sum += m[k * 4 + row] * b.m[col * 4 + k];
                }
                res.m[col * 4 + row] = sum;
            }
        }
        return res;
    }

    [[nodiscard]] Vec3 transform_point(const Vec3& p) const {
        float x = m[0] * p.x + m[4] * p.y + m[8]  * p.z + m[12];
        float y = m[1] * p.x + m[5] * p.y + m[9]  * p.z + m[13];
        float z = m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14];
        float w = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
        if (std::abs(w) > 1e-6f && std::abs(w - 1.0f) > 1e-6f) {
            float inv_w = 1.0f / w;
            return Vec3(x * inv_w, y * inv_w, z * inv_w);
        }
        return Vec3(x, y, z);
    }

    [[nodiscard]] Vec3 transform_vector(const Vec3& v) const {
        return Vec3(
            m[0] * v.x + m[4] * v.y + m[8]  * v.z,
            m[1] * v.x + m[5] * v.y + m[9]  * v.z,
            m[2] * v.x + m[6] * v.y + m[10] * v.z
        );
    }
};

// -----------------------------------------------------------------------------
// Axis-Aligned Bounding Box (AABB)
// -----------------------------------------------------------------------------
struct AABB {
    Vec3 min_pt{0.0f, 0.0f, 0.0f};
    Vec3 max_pt{0.0f, 0.0f, 0.0f};

    constexpr AABB() = default;
    constexpr AABB(const Vec3& min_val, const Vec3& max_val) : min_pt(min_val), max_pt(max_val) {}

    [[nodiscard]] constexpr bool intersects(const AABB& o) const {
        return (min_pt.x <= o.max_pt.x && max_pt.x >= o.min_pt.x) &&
               (min_pt.y <= o.max_pt.y && max_pt.y >= o.min_pt.y) &&
               (min_pt.z <= o.max_pt.z && max_pt.z >= o.min_pt.z);
    }

    void expand(float amount) {
        min_pt.x -= amount; min_pt.y -= amount; min_pt.z -= amount;
        max_pt.x += amount; max_pt.y += amount; max_pt.z += amount;
    }

    void expand(const Vec3& pt) {
        min_pt.x = std::min(min_pt.x, pt.x);
        min_pt.y = std::min(min_pt.y, pt.y);
        min_pt.z = std::min(min_pt.z, pt.z);
        max_pt.x = std::max(max_pt.x, pt.x);
        max_pt.y = std::max(max_pt.y, pt.y);
        max_pt.z = std::max(max_pt.z, pt.z);
    }

    [[nodiscard]] constexpr Vec3 center() const {
        return (min_pt + max_pt) * 0.5f;
    }

    [[nodiscard]] constexpr Vec3 extent() const {
        return (max_pt - min_pt) * 0.5f;
    }

    [[nodiscard]] constexpr bool contains(const Vec3& pt) const {
        return (pt.x >= min_pt.x && pt.x <= max_pt.x &&
                pt.y >= min_pt.y && pt.y <= max_pt.y &&
                pt.z >= min_pt.z && pt.z <= max_pt.z);
    }

    [[nodiscard]] bool ray_intersect(const Vec3& ray_orig, const Vec3& ray_dir, float& t_out) const {
        float tmin = -1e30f;
        float tmax = 1e30f;

        auto check_axis = [&](float orig, float dir, float bmin, float bmax) -> bool {
            if (std::abs(dir) < 1e-7f) {
                return (orig >= bmin && orig <= bmax);
            }
            float t1 = (bmin - orig) / dir;
            float t2 = (bmax - orig) / dir;
            if (t1 > t2) std::swap(t1, t2);
            tmin = std::max(tmin, t1);
            tmax = std::min(tmax, t2);
            return tmin <= tmax;
        };

        if (!check_axis(ray_orig.x, ray_dir.x, min_pt.x, max_pt.x)) return false;
        if (!check_axis(ray_orig.y, ray_dir.y, min_pt.y, max_pt.y)) return false;
        if (!check_axis(ray_orig.z, ray_dir.z, min_pt.z, max_pt.z)) return false;

        if (tmax < 0.0f) return false;
        t_out = (tmin >= 0.0f) ? tmin : 0.0f;
        return true;
    }
};

// -----------------------------------------------------------------------------
// Movement Enum & State Names matching TdPawn movement IDs
// -----------------------------------------------------------------------------
enum class EMovement : uint8_t {
    MOVE_None = 0,
    MOVE_Walking = 1,
    MOVE_Falling = 2,
    MOVE_Grabbing = 3,
    MOVE_WallRunningRight = 4,
    MOVE_WallRunningLeft = 5,
    MOVE_WallClimbing = 6,
    MOVE_SpringBoarding = 7,
    MOVE_SpeedVaulting = 8,
    MOVE_VaultOver = 9,
    MOVE_GrabPullUp = 10,
    MOVE_Jump = 11,
    MOVE_WallRunJump = 12,
    MOVE_GrabJump = 13,
    MOVE_IntoGrab = 14,
    MOVE_Crouch = 15,
    MOVE_Slide = 16,
    MOVE_Melee = 17,
    MOVE_Snatch = 18,
    MOVE_Barge = 19,
    MOVE_Landing = 20,
    MOVE_Climb = 21,
    MOVE_180Turn = 24,
    MOVE_180TurnInAir = 25,
    MOVE_LayOnGround = 26,
    MOVE_ZipLine = 28,
    MOVE_Balance = 29,
    MOVE_LedgeWalk = 30,
    MOVE_MeleeAir = 32,
    MOVE_DodgeJump = 33,
    MOVE_StepUp = 37,
    MOVE_RumpSlide = 38,
    MOVE_MeleeSlide = 48,
    MOVE_WallClimb180TurnJump = 50,
    MOVE_Swing = 60,
    MOVE_Coil = 61,
    MOVE_MeleeWallrun = 62,
    MOVE_SoftLanding = 78,
    MOVE_AutoStepUp = 81,
    MOVE_SkillRoll = 91
};

inline const char* move_state_name(EMovement m) {
    switch (m) {
        case EMovement::MOVE_None: return "MOVE_None";
        case EMovement::MOVE_Walking: return "MOVE_Walking";
        case EMovement::MOVE_Falling: return "MOVE_Falling";
        case EMovement::MOVE_Grabbing: return "MOVE_Grabbing";
        case EMovement::MOVE_WallRunningRight: return "MOVE_WallRunningRight";
        case EMovement::MOVE_WallRunningLeft: return "MOVE_WallRunningLeft";
        case EMovement::MOVE_WallClimbing: return "MOVE_WallClimbing";
        case EMovement::MOVE_SpringBoarding: return "MOVE_SpringBoarding";
        case EMovement::MOVE_SpeedVaulting: return "MOVE_SpeedVaulting";
        case EMovement::MOVE_VaultOver: return "MOVE_VaultOver";
        case EMovement::MOVE_GrabPullUp: return "MOVE_GrabPullUp";
        case EMovement::MOVE_Jump: return "MOVE_Jump";
        case EMovement::MOVE_WallRunJump: return "MOVE_WallRunJump";
        case EMovement::MOVE_GrabJump: return "MOVE_GrabJump";
        case EMovement::MOVE_IntoGrab: return "MOVE_IntoGrab";
        case EMovement::MOVE_Crouch: return "MOVE_Crouch";
        case EMovement::MOVE_Slide: return "MOVE_Slide";
        case EMovement::MOVE_Melee: return "MOVE_Melee";
        case EMovement::MOVE_Snatch: return "MOVE_Snatch";
        case EMovement::MOVE_Barge: return "MOVE_Barge";
        case EMovement::MOVE_Landing: return "MOVE_Landing";
        case EMovement::MOVE_Climb: return "MOVE_Climb";
        case EMovement::MOVE_180Turn: return "MOVE_180Turn";
        case EMovement::MOVE_180TurnInAir: return "MOVE_180TurnInAir";
        case EMovement::MOVE_LayOnGround: return "MOVE_LayOnGround";
        case EMovement::MOVE_ZipLine: return "MOVE_ZipLine";
        case EMovement::MOVE_Balance: return "MOVE_Balance";
        case EMovement::MOVE_LedgeWalk: return "MOVE_LedgeWalk";
        case EMovement::MOVE_MeleeAir: return "MOVE_MeleeAir";
        case EMovement::MOVE_DodgeJump: return "MOVE_DodgeJump";
        case EMovement::MOVE_StepUp: return "MOVE_StepUp";
        case EMovement::MOVE_RumpSlide: return "MOVE_RumpSlide";
        case EMovement::MOVE_MeleeSlide: return "MOVE_MeleeSlide";
        case EMovement::MOVE_WallClimb180TurnJump: return "MOVE_WallClimb180TurnJump";
        case EMovement::MOVE_Swing: return "MOVE_Swing";
        case EMovement::MOVE_Coil: return "MOVE_Coil";
        case EMovement::MOVE_MeleeWallrun: return "MOVE_MeleeWallrun";
        case EMovement::MOVE_SoftLanding: return "MOVE_SoftLanding";
        case EMovement::MOVE_AutoStepUp: return "MOVE_AutoStepUp";
        case EMovement::MOVE_SkillRoll: return "MOVE_SkillRoll";
        default: return "MOVE_Unknown";
    }
}

// -----------------------------------------------------------------------------
// Mesh & Graphics Types
// -----------------------------------------------------------------------------
struct Vertex {
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 normal{0.0f, 0.0f, 1.0f};
    Vec3 tangent{1.0f, 0.0f, 0.0f};
    float u = 0.0f;
    float v = 0.0f;
    float u2 = 0.0f; // Lightmap UV
    float v2 = 0.0f;
    uint32_t color = 0xFFFFFFFF;
    float tangent_sign = 1.0f; // Binormal = cross(normal, tangent) * tangent_sign (UE3 TangentZ.w)
};

// A contiguous range of MeshBuffer::vertices drawn with one scene material
// (index into LevelScene::materials->materials, -1 = legacy procedural shading).
struct MeshSection {
    uint32_t first_vertex = 0;
    uint32_t vertex_count = 0;
    int32_t material = -1;
};

struct MeshBuffer {
    std::string name;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    AABB bounds;
    bool is_runner_vision = false;
    std::vector<MeshSection> sections; // empty = whole buffer uses legacy procedural shading
    // Moving (InterpActor) geometry: vertices are stored at the actor's initial pose and drawn
    // with a translation model matrix of LevelScene::elevators[elevator].parts[elevator_part].offset.
    int32_t elevator = -1;
    int32_t elevator_part = -1;
    int32_t barge_door = -1;
};

struct SoundClip {
    std::string name;
    std::string full_path;
    std::vector<uint8_t> pcm_data;
    int sample_rate = 44100;
    int channels = 2;
    float duration = 0.0f;
};

struct SoundCueDef {
    std::string name;
    std::string full_path;
    std::string sound_group = "InGameSFX";
    float volume_multiplier = 0.75f;
    float pitch_multiplier = 1.0f;
    float min_radius = 200.0f;
    float max_radius = 3000.0f;
    bool looping = false;
    std::vector<std::string> wave_names;
    std::vector<float> wave_weights;
};

struct AmbientEmitterInfo {
    Vec3 location{0.0f, 0.0f, 0.0f};
    std::string cue_name;
    std::string wave_name;
    float min_radius = 200.0f;
    float max_radius = 3000.0f;
    float volume = 1.0f;
    float pitch = 1.0f;
};

// -----------------------------------------------------------------------------
// Level Actor (Shared Contract across Agents 5, 6, 7)
// -----------------------------------------------------------------------------
struct LevelActor {
    std::string class_name;
    std::string object_name;
    std::string unique_name;   // export name including the FName number suffix (e.g. "InterpActor_3")
    std::string base_name;     // unique_name of the Actor.Base this actor is attached to ("" = none)
    std::string mesh_name;
    std::string tag;
    Vec3 location{0.0f, 0.0f, 0.0f};
    Rotator rotation{0.0f, 0.0f, 0.0f};
    Vec3 draw_scale_3d{1.0f, 1.0f, 1.0f};
    float draw_scale = 1.0f;
    AABB world_bounds;
    // UE3 collision: bCollideActors && bBlockActors && CollisionComponent.CollideActors &&
    // BlockActors && BlockNonZeroExtent (pawn movement) / BlockZeroExtent (traces).
    bool is_collidable = true;   // blocks non-zero-extent (player / AI movement) checks
    bool blocks_traces = true;   // blocks zero-extent line checks
    bool is_hidden = false;      // bHidden / HiddenGame: collides but is never drawn
    bool collide_complex = false; // Actor.bCollideComplex: ignore simple collision, collide per poly
    bool is_blocking_volume = false;
    int32_t elevator = -1;       // >= 0: moving part of LevelScene::elevators[elevator]
    int32_t barge_door = -1;     // >= 0: hinged door part of LevelScene::barge_doors[barge_door]
    bool is_runner_vision = false;
    bool is_checkpoint = false;
    bool is_trigger = false;
    bool is_zipline = false;
    bool is_ladder = false;
    bool is_ledge = false;
    bool is_springboard = false;
    bool is_balance_beam = false;
    bool is_swing_bar = false;
    bool is_enemy = false;
    bool is_bag = false;
    bool is_elevator_part = false;
    std::string source_package;
    Vec3 end_point{0.0f, 0.0f, 0.0f};
    // StaticMeshComponent.Materials overrides (full object paths, "" = use the mesh element's material)
    std::vector<std::string> material_overrides;
    // BlockingVolume BrushComponent.BrushAggGeom hulls, world space, 3 vertices per triangle.
    std::vector<Vec3> brush_triangles;
    // TdTutorialStart.BelongToChallenge (EMovementChallenge names, e.g. "EMC_SlideOne").
    std::vector<std::string> tutorial_challenges;
};

// -----------------------------------------------------------------------------
// Weapon State, Tracers, Dropped Weapons & Enemy Bot
// -----------------------------------------------------------------------------
enum class EWeaponFireMode : uint8_t {
    SemiAuto = 0,
    Burst3 = 1,
    FullAuto = 2,
    PumpAction = 3,
    BoltAction = 4
};

struct WeaponState {
    std::string name;
    std::string display_name = "Unarmed";
    bool equipped = false;
    bool is_heavy = false;
    bool is_two_handed = false;
    EWeaponFireMode fire_mode = EWeaponFireMode::SemiAuto;
    int ammo = 0;
    int max_ammo = 0;
    int pellet_count = 1;
    int burst_remaining = 0;
    float burst_timer = 0.0f;
    float damage = 35.0f;
    float damage_far = 20.0f;
    float falloff_damage = 20.0f;
    float falloff_distance = 1000.0f;
    float falloff_start = 1000.0f;
    float falloff_end = 3000.0f;
    float range = 4500.0f;
    float fire_interval = 0.2f;
    float spread = 0.02f;
    float spread_rad = 0.02f;
    float recoil_pitch = 1.5f;
    float recoil_pitch_deg = 1.5f;
    float recoil_yaw = 0.4f;
    float kickback_amount = 15.0f;
    float mobility_scale = 0.95f;
    float cooldown = 0.0f;
    float fire_anim_timer = 0.0f;
    float fire_anim_duration = 0.65f;
    float equip_timer = 0.0f;
    float drop_timer = 0.0f;
    float muzzle_flash_timer = 0.0f;
    bool trigger_released = true;
    bool fired_this_tick = false;
};

struct BulletTracer {
    Vec3 start_pos{0.0f, 0.0f, 0.0f};
    Vec3 end_pos{0.0f, 0.0f, 0.0f};
    float timer = 0.08f;
    float max_time = 0.08f;
    bool hit_enemy = false;
    bool from_player = true;
};

struct DroppedWeapon {
    std::string weapon_name;
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 velocity{0.0f, 0.0f, 0.0f};
    float yaw_deg = 0.0f;
    float pitch_deg = 0.0f;
    float roll_deg = 85.0f;
    int ammo = 0;
    bool is_heavy = false;
    bool grounded = false;
    float lifetime = 60.0f;
};

enum class EEnemyAnimState : uint8_t {
    Idle = 0,
    Patrol = 1,
    Chase = 2,
    AimFire = 3,
    MeleeWindup = 4,     // Disarm window active (Runner Vision red)
    MeleeStrike = 5,
    HitStagger = 6,
    BeingDisarmed = 7,
    KnockedOut = 8
};

struct EnemyBot {
    std::string archetype; // PatrolCop, PursuitCop, RiotCop, SWAT, Assault, Support, Celeste
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 home_position{0.0f, 0.0f, 0.0f};
    Vec3 velocity{0.0f, 0.0f, 0.0f};
    float yaw_deg = 0.0f;
    float health = 100.0f;
    float max_health = 100.0f;
    bool alive = true;
    bool stunned = false;
    bool disarm_window = false;
    float attack_timer = 0.0f;
    std::string weapon_name = "Colt1911";
    EEnemyAnimState anim_state = EEnemyAnimState::Idle;
    std::string active_anim_seq;
    float anim_timer = 0.0f;
    float anim_duration = 1.0f;
    float muzzle_flash_timer = 0.0f;
    int burst_shots_left = 0;
    float burst_cooldown = 0.0f;
};

// -----------------------------------------------------------------------------
// Movement Configuration (DefaultPawnMovement.ini / TdGame.u defaults)
// Every value names the TdPawn / TdMove_* property it comes from.
// -----------------------------------------------------------------------------
struct MovementConfig {
    // Effective pawn gravity. WorldInfo.DefaultGravityZ is -800, but every TdMove height -> speed
    // formula reads Speed = Gravity * 2 * Sqrt(Height / Gravity) = 2 * Sqrt(800 * Height), which only
    // reaches Height when the pawn decelerates at 1600, and DefaultGame.ini notes "1600 is the
    // downward speed after falling 780 cm" (1600^2 / (2 * 780) = 1641). The native move code adds
    // -GetGravityZ to the pawn's own acceleration, doubling the world value.
    float gravity = 1600.0f;
    float script_gravity = 800.0f;             // |GetGravityZ()| as the scripts see it

    // TdPawn ground model (native GetSprintAcceleration / GetWalkAcceleration / CalcVelocity)
    float ground_speed = 720.0f;               // Pawn.GroundSpeed (top of SpeedCurve_LightWeapon)
    float accel_rate = 6144.0f;                // Pawn.AccelRate
    float ground_friction = 8.0f;              // PhysicsVolume.GroundFriction
    float braking_friction_strength = 0.5f;    // TdPlayerPawn.BrakingFrictionStrength
    float speed_max_base_velocity = 400.0f;    // TdPawn.SpeedMaxBaseVelocity (above it: sprint energy)
    float speed_min_base_velocity = 10.0f;     // TdPawn.SpeedMinBaseVelocity
    float sprint_accel_factor = 30.0f;         // SpeedSprintVelocityAccelerationFactor
    float walk_accel_factor = 7.0f;            // SpeedWalkVelocityAccelerationFactor
    float strafe_accel_factor = 10.0f;         // SpeedStrafeVelocityAccelerationFactor
    float energy_decel_time = 3.0f;            // SpeedEnergyDecelerationTime
    float energy_decel_exponent = 0.5f;        // SpeedEnergyDecelerationExponent
    float turn_decel_factor = 10.0f;           // SpeedTurnDecelerationFactor
    float sprint_input_threshold = 0.7f;       // TdPlayerController.InputMaxSprintHeightLimit / RaduisLimit
    float air_control = 0.025f;                // Pawn.AirControl (x AccelRate = air acceleration cap)
    float crouch_speed_modifier = 0.2f;        // TdMove_Crouch.SpeedModifier
    float walk_speed = 50.0f;                  // legacy TdPawn.WalkVelocity (telemetry only)
    float jog_speed = 260.0f;                  // legacy jog threshold (telemetry only)
    float run_speed = 400.0f;                  // = SpeedMaxBaseVelocity (FOV / skill roll floor)
    float sprint_speed = 630.0f;               // legacy TdPawn.SprintVelocity (FOV scaling top)

    // [TdGame.TdMove_Jump]
    float base_jump_z = 630.0f;
    float base_jump_z_heavy = 430.0f;
    float jump_add_xy = 100.0f;
    float jump_tap_time = 0.15f;               // TdPlayerController.JumpTapTime (jump buffer)
    float ledge_assist_height = 112.0f;        // TdMove_Jump: precise jump onto a ledge below this

    // [TdGame.TdMove_DodgeJump]
    float dodge_jump_side_speed = 600.0f;
    float dodge_jump_z = 300.0f;
    float dodge_jump_inertia = 0.3f;

    // [TdGame.TdMove_WallRun]
    float wallrun_min_speed = 200.0f;          // WallRunningMinSpeed
    float wallrun_initial_z = 170.0f;          // WallRunningHorisontalInitialZHeight: a HEIGHT budget
    float wallrun_accel = 820.0f;              // WallRunningHorisontalAcceleration: pull-down while rising
    float wallrun_decel = 500.0f;              // WallRunningHorisontalDeceleration: pull-down while falling
    float wallrun_friction = 0.05f;            // WallRunningHorisontalFriction
    float wallrun_stop_fall_speed = 500.0f;    // the run ends once falling faster than this
    float wallrun_max_angle_deg = 57.0f;       // WallRunningForwardMaxStartAngle
    float wallrun_side_angle_deg = 60.0f;      // WallRunningStrafeStartAngle
    float wallrun_check_distance = 50.0f;      // WallRunningForwardCheckDistance
    float wallrun_check_distance_mult = 1.8f;  // ContextMoveDistanceMultiplier (at GroundSpeed)
    float wallrun_min_wall_height = 192.0f;    // MinWallHeight
    float wallrun_jump_height = 100.0f;        // TdMove_WallrunJump.JumpHeight
    float wallrun_jump_height_look_add = 60.0f;
    float wallrun_jump_out = 120.0f;           // WallRunningPushOutSpeedMin
    float wallrun_jump_out_look_add = 400.0f;
    float wallrun_jump_forward_min = 0.1f;     // WallRunningPushForwardSpeedMin
    float wallrun_duration = 1.6f;             // legacy telemetry only (the game has no timer)

    // [TdGame.TdMove_WallClimb]
    float wallclimb_max_angle_deg = 33.0f;     // WallClimbingVerticalStartAngle
    float wallclimb_gravity = 800.0f;          // WallClimbingGravity (script constant, climb decel = 2x)
    float wallclimb_max_distance = 120.0f;     // WallClimbingMaxDistance2D
    float wallclimb_min_wall_height = 180.0f;  // MinWallHeight
    float wallclimb_suck_in_speed = 400.0f;    // horizontal speed into the wall when entering slowly
    float wallclimb_add_xy_height = 60.0f;     // AddOnSpeed2DHeight
    float wallclimb_add_xy_max_speed = 650.0f; // AddOnSpeed2DMaxLimit
    float wallclimb_add_z_height = 130.0f;     // AddOnSpeedZHeight
    float wallclimb_boost_z = 320.0f;          // AddOnSpeedZMaxLimit
    float wallclimb_turn_jump_window = 0.6f;   // TdMove_WallClimb180TurnJump.JumpTimeWindow
    float wallclimb_turn_jump_out = 400.0f;
    float wallclimb_turn_jump_height = 250.0f;
    float wallclimb_dodge_side_speed = 150.0f; // WallClimbDodge JumpAddXY
    float wallclimb_dodge_z = 700.0f;          // WallClimbDodge BaseJumpZ

    // [TdGame.TdMove_Grab] / GrabPullUp / GrabJump
    float grab_hang_depth = 182.8f;            // ledge top above the feet while hanging (92.8 + 90)
    float grab_max_angle_deg = 40.0f;          // GrabMaxAngle
    float grab_pull_up_time = 1.0f;
    float grab_jump_height = 160.0f;
    float grab_jump_push_min = 200.0f;
    float grab_jump_push_max = 400.0f;
    float grab_shimmy_speed = 56.0f;           // 60 uu per 1.07 s
    float grab_shimmy_delay = 0.6f;            // DisableShimmyTime

    // [TdGame.TdMove_SpeedVault]
    float vault_max_handplant_time = 0.4f;     // TimeToHandPlant limit
    float vault_time_over = 0.35f;
    float vault_time_down = 0.3f;
    float vault_ledge_offset_z = 25.0f;
    float vault_min_speed = 400.0f;
    float vault_speed_add = 80.0f;

    // [TdGame.TdMove_SpringBoard]
    float springboard_jump_z = 950.0f;         // SpringBoardJumpZ
    float springboard_step_height = 64.0f;
    float springboard_step_tolerance = 20.0f;
    float springboard_obstacle_min = 80.0f;
    float springboard_obstacle_max = 148.0f;
    float springboard_obstacle_distance = 112.0f;
    float springboard_check_time = 1.0f;       // CheckDistanceTime

    // [TdGame.TdMove_Slide]
    float slide_min_speed = 350.0f;            // CanDoMove: speed along the facing
    float slide_abort_speed = 250.0f;          // SlideAbortSpeed
    float slide_friction = 0.1f;               // FrictionModifier (x GroundFriction)
    float slide_max_duration = 2.0f;           // SlideAbortTime
    float slide_min_duration = 0.5f;           // an uncrouch request waits this long
    float slide_look_turn = 0.2f;              // SlideLookTurn (fraction of the view angle per second)
    float slide_strafe_turn_deg = 11.0f;       // 2000 rotation units per second

    // [TdGame.TdMove_Coil]
    float coil_height_boost = 60.0f;           // TotalHeightBoost
    float coil_duration = 0.25f;               // HeightBoostDuration

    // [TdGame.TdMove_Landing] / TdPawn
    float skill_roll_min_fall = 200.0f;        // SkillRollLandingHeight
    float soft_landing_min_fall = 300.0f;      // SoftLandingHeight
    float hard_landing_min_fall = 530.0f;      // HardLandingHeight
    float uncontrolled_fall = 1000.0f;         // FallingUncontrolledHeight (lethal)
    float landing_speed_reduction = 65.0f;     // LandingSpeedReduction
    float roll_trigger_window = 0.2f;          // TdPawn.CanSkillRoll: crouch pressed this recently
    float roll_trigger_rearm = 0.6f;           // a press only re-arms after this long
    float skill_roll_time = 0.5f;
    float hard_landing_time = 1.8f;
    float lay_on_ground_time = 1.5f;

    // [TdGame.TdMove_180Turn]
    float turn_180_time = 0.25f;               // TurnTime
    float turn_180_friction = 0.3f;            // FrictionModifier

    // Reaction time / health
    float reaction_time_dilation = 0.25f;
    float reaction_time_drain = 8.0f;
    float health_regen_delay = 5.0f;
    float health_regen_rate = 25.0f;
};

// -----------------------------------------------------------------------------
// Campaign Chapter Info
// -----------------------------------------------------------------------------
struct ChapterInfo {
    std::string code;
    std::string title;
    std::string primary_map;
};

// -----------------------------------------------------------------------------
// Level Streaming & Checkpoint Info (Reverse-engineered from TdCheckpoint &
// LevelStreamingKismet / SeqAct_MultiLevelStreaming in *_p.me1)
// -----------------------------------------------------------------------------
struct LevelCheckpointInfo {
    std::string object_name;
    std::string checkpoint_name;
    int checkpoint_weight = 0;
    bool default_checkpoint = false;
    Vec3 location{0.0f, 0.0f, 0.0f};
    Rotator rotation{0.0f, 0.0f, 0.0f};
    std::vector<std::string> streaming_levels; // LevelStreamingKismet.PackageName list
};

struct LevelStreamingActionInfo {
    std::string object_name;
    std::vector<std::string> package_names;    // Sublevel packages controlled by SeqAct_MultiLevelStreaming
};

// -----------------------------------------------------------------------------
// Interactive Elevator System (Reverse-engineered from *_Slc.me1 / *_Spt.me1
// InterpActor + SeqAct_Interp + InterpTrackMove)
// -----------------------------------------------------------------------------
enum class ElevatorState : uint8_t {
    IdleStart = 0,     // Waiting at start floor with lower doors open
    DoorsClosing = 1,  // Player entered cab / pressed button; lower doors sliding shut (0.7s)
    Moving = 2,        // Cab riding along InterpTrackMove PosTrack curve + streaming sublevels
    DoorsOpening = 3,  // Arrived at destination floor; upper doors sliding open (0.7s)
    IdleEnd = 4        // Arrived at destination floor with upper doors open
};

struct ElevatorKeyframe {
    float time = 0.0f;
    Vec3 pos{0.0f, 0.0f, 0.0f};
};

// A moving InterpActor driven by the elevator matinees: the cab itself, actors hard-attached to
// it (Base == cab, e.g. the cab doors) and the landing doors at either floor.
enum class ElevatorPartRole : uint8_t {
    Cab = 0,          // moves along the cab PosTrack
    CabAttached = 1,  // Base == cab: rides with the cab
    CabDoor = 2,      // Base == cab sliding door: rides with the cab and opens at either floor
    StartDoor = 3,    // landing door at the start floor
    EndDoor = 4       // landing door at the destination floor
};

struct ElevatorPart {
    std::string actor_name;    // LevelActor::unique_name
    int32_t actor_index = -1;  // LevelScene::actors index
    ElevatorPartRole role = ElevatorPartRole::Cab;
    int32_t mesh_index = -1;   // LevelScene::meshes index (vertices at the initial pose, -1 = none)
    Vec3 door_open_offset{0.0f, 0.0f, 0.0f};  // world offset of a fully open door (matinee end key)
    Vec3 offset{0.0f, 0.0f, 0.0f};            // current world displacement from the initial pose
    Vec3 prev_offset{0.0f, 0.0f, 0.0f};
    // Collision triangles at the initial pose (queries are shifted by -offset)
    std::shared_ptr<const CollisionWorld> collision;
};

struct ElevatorInstance {
    std::string name;                  // e.g. "Escape_Intro-Off_Slc:mainlift"
    std::string source_package;        // e.g. "Escape_Intro-Off_Spt"
    std::string cab_mesh_name;         // e.g. "S_Elevator_01" or "S_SP09_ElevatorWithTop_01"
    Vec3 start_pos{0.0f, 0.0f, 0.0f};  // Initial cab world position (floor center/origin)
    Vec3 end_pos{0.0f, 0.0f, 0.0f};    // Target cab world position after InterpTrackMove
    Vec3 current_pos{0.0f, 0.0f, 0.0f};
    Vec3 prev_pos{0.0f, 0.0f, 0.0f};
    Rotator rotation{0.0f, 0.0f, 0.0f};
    Vec3 cab_half_extents{120.0f, 132.5f, 131.0f}; // Interior half-extents (S_Elevator_01: 240x265x262)
    Vec3 cab_local_offset{0.0f, 0.0f, 0.0f};       // Offset from actor Location to cab floor center
    float ride_duration = 5.0f;        // Matinee InterpLength (default 5.0s, Subway_Elev = 12.0s)
    float door_duration = 0.7f;        // Matinee lowerdoors InterpLength (0.7s, slides 76 units each)
    float timer = 0.0f;
    float door_open_Start = 1.0f;      // 1.0 = fully open (+76u apart), 0.0 = closed
    float door_open_End = 0.0f;        // 1.0 = fully open (+76u apart), 0.0 = closed
    ElevatorState state = ElevatorState::IdleStart;
    bool move_frame_world = false;     // true if IMF_World, false if IMF_RelativeToInitial
    bool auto_trigger_on_enter = true; // Trigger automatically when Faith steps inside cab / presses button
    bool streaming_triggered = false;  // Whether mid-shaft sublevel streaming has fired
    int target_checkpoint_idx = -1;    // Optional target checkpoint whose StreamingLevels are loaded mid-ride
    std::vector<std::string> stream_in_packages;
    std::vector<std::string> stream_out_packages;
    std::vector<ElevatorKeyframe> keyframes; // Full InterpTrackMove PosTrack curve
    Vec3 button_pos{0.0f, 0.0f, 0.0f};       // Interior/exterior S_ElevatorButton_Single world position
    std::string cab_actor_name;              // LevelActor::unique_name of the cab InterpActor
    std::vector<ElevatorPart> parts;         // real moving meshes + collision (cab, doors)
};

// -----------------------------------------------------------------------------
// Interactive Hinged / Bargeable Door System (`TdMove_Barge` + `InterpActor` Doors)
// -----------------------------------------------------------------------------
enum class DoorState : uint8_t {
    Closed = 0,
    Opening = 1,
    Open = 2,
    Closing = 3
};

struct DoorPart {
    std::string actor_name;
    int32_t actor_index = -1;
    int32_t mesh_index = -1;       // LevelScene::meshes index (-1 if hidden blocker like S_DoorClosingMech_02)
    bool is_blocker_only = false;  // true for hidden doorway trigger/blocker slab
    std::shared_ptr<const CollisionWorld> collision;
};

struct BargeDoorInstance {
    std::string name;
    std::string source_package;
    Vec3 hinge_pos{0.0f, 0.0f, 0.0f};      // World-space vertical hinge pin position
    Vec3 center_pos{0.0f, 0.0f, 0.0f};     // World-space center of closed door slab
    AABB closed_bounds;                    // World-space AABB of doorway (for barge traces & triggers)
    DoorState state = DoorState::Closed;
    float open_angle_rad = 0.0f;           // Current signed Z-rotation around hinge_pos (radians)
    float target_angle_rad = 0.0f;         // Target open angle (+/- ~1.66 rad = ~95 deg away from player)
    float open_speed = 11.5f;              // Angular velocity (rad/s): fast slam on Barge, smooth on Interact
    float hold_timer = 0.0f;               // Time remaining while held open before optional slow return
    bool barged = false;                   // True when slammed open via MOVE_Barge
    std::vector<DoorPart> parts;           // Door leaf mesh, attached closer bar, and hidden doorway slab
    Mat4 model_matrix = Mat4::identity();  // T(hinge_pos) * Rz(open_angle_rad) * T(-hinge_pos)
};

// -----------------------------------------------------------------------------
// Input Frame (Mapped from Keyboard, Mouse, or Gamepad)
// -----------------------------------------------------------------------------
struct InputFrame {
    float forward = 0.0f;          // +1 = forward, -1 = backward
    float strafe = 0.0f;           // +1 = right, -1 = left
    float look_yaw_delta = 0.0f;   // Horizontal mouse delta (degrees)
    float look_pitch_delta = 0.0f; // Vertical mouse delta (degrees)
    bool sprint = false;
    bool jump = false;
    bool crouch = false;
    bool turn_180 = false;
    bool melee = false;
    bool disarm = false;
    bool fire = false;
    bool reaction_time = false;
    bool look_at = false;
    bool use = false;              // E / Interact button (e.g. Elevator button)
    bool drop_weapon = false;      // G / Backspace: Throw away equipped weapon
    int cycle_weapon_dir = 0;      // +1 / -1: Cycle through retail firearm arsenal
    bool spawn_combat_squad = false; // H: Spawn combat sparring squad in front of player
};

// -----------------------------------------------------------------------------
// Player Telemetry (Every simulation tick)
// -----------------------------------------------------------------------------
struct PlayerTelemetry {
    uint32_t tick = 0;
    float sim_time = 0.0f;
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 velocity{0.0f, 0.0f, 0.0f};
    float speed_2d = 0.0f;
    float speed_3d = 0.0f;
    float yaw_deg = 0.0f;
    float pitch_deg = 0.0f;
    float camera_roll_deg = 0.0f;
    float fov_deg = 90.0f;
    float eye_height = 166.0f;  // TdPawn: centre 90 + BaseEyeHeight 76 above the feet
    float health = 100.0f;
    float reaction_energy = 100.0f;
    bool reaction_active = false;
    bool grounded = true;
    EMovement move_state = EMovement::MOVE_Walking;
    Vec3 wall_normal{0.0f, 0.0f, 0.0f};
    int active_checkpoint = 0;
    std::string active_checkpoint_name;
    int bags_collected = 0;
    bool in_elevator = false;
    int active_elevator_idx = -1;
    float elevator_progress = 0.0f;
    int streamed_sublevel_count = 0;
    WeaponState weapon;
    float combat_anim_time = 0.0f;
    float combat_anim_duration = 0.6f;
    int melee_variant = 0;
    bool snatch_from_back = false;
    bool melee_hit_confirmed = false;
    bool disarm_prompt_visible = false;
    float hit_marker_timer = 0.0f;
    float damage_flash_timer = 0.0f;
    std::string active_subtitle;

    // Cooked level intro Matinee / 1P skeletal animation playback state
    bool intro_active = false;
    std::string intro_anim_name;
    std::string intro_pkg_path;
    int32_t intro_anim_exp_1 = 0;
    float intro_anim_time = 0.0f;
};

// -----------------------------------------------------------------------------
// Real-Time Planar Reflection Capture & Volume Info
// (Reverse-engineered from SceneCaptureReflectActor / SceneCaptureReflectComponent
// in Engine.u & UnSceneCapture.cpp @ VA 0x00f99390..0x00f9d312, and
// TdReflectionVolume in TdGame.u @ VA 0x00f9bdde..0x00f9bed9)
// -----------------------------------------------------------------------------
struct SceneCaptureReflectInfo {
    std::string actor_name;                 // e.g. "Edge_Pt2.SceneCaptureReflectActor_0"
    std::string component_name;             // e.g. "SceneCaptureReflectComponent_0"
    std::string texture_target;             // e.g. "M_SP01.T_EdgeReflection_01_R"
    std::string reflection_volume;          // e.g. "Edge_Pt2.TdReflectionVolume_1" (optional DICE camera culling volume)
    Vec3 location{0.0f, 0.0f, 0.0f};        // World position on the mirror plane
    Rotator rotation{16384.0f, 0.0f, 0.0f}; // Default Pitch=16384 (+90 deg -> MirrorNormal = +Z)
    Vec3 mirror_normal{0.0f, 0.0f, 1.0f};   // FRotator(Rotation).Vector()
    float plane_w = 0.0f;                   // FPlane::W = dot(Location, MirrorNormal)
    float scale_fov = 1.0f;
    float near_plane = 20.0f;
    float far_plane = 500.0f;
    float far_culling_distance = 0.0f;
    float max_update_dist = 0.0f;
    float max_streaming_update_dist = 0.0f;
    float frame_rate = 1000.0f;
};

struct ReflectionVolumeInfo {
    std::string object_name;                // e.g. "Edge_Pt2.TdReflectionVolume_1"
    Vec3 location{0.0f, 0.0f, 0.0f};
    Rotator rotation{0.0f, 0.0f, 0.0f};
    bool enabled = true;
};

struct LevelIntroSequence {
    bool valid = false;
    std::string seq_name;                  // e.g. "sp01_intro", "sp02_intro", ..., "sp09_intro"
    std::string package_path;              // full path to cooked .me1 package owning the 82-bone AnimSequence
    int32_t anim_export_index_1 = 0;       // 1-based export index of the 82-bone UAnimSequence
    Vec3 actor_location{0.0f, 0.0f, 0.0f}; // SkeletalMeshActorMAT world Location
    float actor_yaw_deg = 90.0f;           // SkeletalMeshActorMAT world Rotation Yaw (degrees)
    float start_offset_sec = 0.0f;         // InterpTrackAnimControl.StartTime
    float duration_sec = 0.0f;             // UAnimSequence.SequenceLength
    Vec3 start_feet_pos{0.0f, 0.0f, 0.0f}; // World-space root position at t = 0
    Vec3 end_feet_pos{0.0f, 0.0f, 0.0f};   // World-space root position at end of intro (post-intro floor)
    float end_yaw_deg = 0.0f;              // World-space camera/body facing yaw at end of intro
};

// -----------------------------------------------------------------------------
// Level Scene representation
// -----------------------------------------------------------------------------
struct LevelScene {
    std::string map_name;
    std::string chapter_title;
    Vec3 player_spawn_pos{0.0f, 0.0f, 100.0f};
    float player_spawn_yaw = 0.0f;
    LevelIntroSequence level_intro{};
    Vec3 sun_direction{-0.4f, 0.6f, 0.7f};  // world-space direction towards the sun (level DirectionalLight)
    Vec3 sun_color{2.0f, 1.96f, 1.9f};       // linear RGB * Brightness of the level's DirectionalLight
    // Reverse-engineered ambient & hemisphere lighting (SkyLightComponent + DirectionalLight.ModShadowColor + WorldInfo.SkyColor)
    Vec3 sky_upper_color{0.68f, 0.84f, 1.0f};       // Calibrated UpperSkyColor for BasePass GetMaterialHemisphereLightTransferFull
    Vec3 sky_lower_color{0.82f, 0.84f, 0.88f};      // Calibrated LowerSkyColor for BasePass GetMaterialHemisphereLightTransferFull
    Vec3 raw_sky_upper_linear{0.194f, 0.269f, 0.400f}; // Exact FSkyLightSceneProxy UpperColor = pow(LightColor, 2.2) * Brightness
    Vec3 raw_sky_lower_linear{0.230f, 0.161f, 0.076f}; // Exact FSkyLightSceneProxy LowerColor = pow(LowerColor, 2.2) * LowerBrightness
    Vec3 mod_shadow_color{0.494f, 0.659f, 0.875f};  // DirectionalLightComponent.ModShadowColor (sRGB 0..1 azure shadow tint)
    Vec3 world_sky_color{0.30f, 0.52f, 0.65f};      // WorldInfo.SkyColor (Beast environment sky radiosity color)
    Vec3 haze_color{0.76f, 0.86f, 0.96f};           // PostProcessSettings.HazeColor / HeightFog LightColor
    float ibl_intensity = 1.0f;                     // WorldInfo.IBLIntensity
    std::string sky_light_source;                   // Package.ObjectName of resolved SkyLightComponent
    std::vector<LevelActor> actors;
    std::vector<MeshBuffer> meshes;
    // Static world collision built from the real UE3 data (StaticMesh BodySetup / kDOP triangles,
    // BlockingVolume brushes). Moving elevator parts carry their own CollisionWorld.
    std::shared_ptr<const CollisionWorld> collision;
    float kill_z = -1.0e30f;  // falling below this kills the player (WorldInfo.KillZ or geometry floor)
    std::vector<EnemyBot> enemies;
    std::vector<BulletTracer> active_tracers;
    std::vector<DroppedWeapon> dropped_weapons;
    std::vector<Vec3> checkpoints;
    std::vector<LevelCheckpointInfo> checkpoint_infos;
    std::vector<LevelStreamingActionInfo> streaming_actions;
    std::vector<std::string> all_streaming_packages;
    std::vector<std::string> loaded_sublevel_packages;
    std::vector<ElevatorInstance> elevators;
    std::vector<BargeDoorInstance> barge_doors;
    std::vector<SceneCaptureReflectInfo> reflection_captures;
    std::vector<ReflectionVolumeInfo> reflection_volumes;
    std::vector<SoundClip> sounds;
    std::vector<std::string> subtitles;
    // Resolved + compiled Mirror's Edge materials and textures referenced by MeshSection::material
    std::shared_ptr<const SceneMaterialLibrary> materials;
};

} // namespace me

