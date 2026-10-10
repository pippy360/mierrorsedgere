#pragma once

#include <array>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <utility>
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
    MOVE_IntoClimb = 22,
    MOVE_180Turn = 24,
    MOVE_180TurnInAir = 25,
    MOVE_LayOnGround = 26,
    MOVE_IntoZipLine = 27,
    MOVE_ZipLine = 28,
    MOVE_Balance = 29,
    MOVE_LedgeWalk = 30,
    MOVE_GrabTransfer = 31,
    MOVE_MeleeAir = 32,
    MOVE_DodgeJump = 33,
    MOVE_WallRunDodgeJump = 34,
    MOVE_Stumble = 35,
    MOVE_StepUp = 37,
    MOVE_RumpSlide = 38,
    MOVE_Vertigo = 47,
    MOVE_MeleeSlide = 48,
    MOVE_WallClimbDodgeJump = 49,
    MOVE_WallClimb180TurnJump = 50,
    MOVE_Swing = 60,
    MOVE_Coil = 61,
    MOVE_MeleeWallrun = 62,
    MOVE_MeleeCrouch = 63,
    MOVE_FallingUncontrolled = 72,
    MOVE_SwingJump = 73,
    MOVE_SoftLanding = 78,
    MOVE_AutoStepUp = 81,
    MOVE_AirBarge = 85,
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
        case EMovement::MOVE_IntoClimb: return "MOVE_IntoClimb";
        case EMovement::MOVE_180Turn: return "MOVE_180Turn";
        case EMovement::MOVE_180TurnInAir: return "MOVE_180TurnInAir";
        case EMovement::MOVE_LayOnGround: return "MOVE_LayOnGround";
        case EMovement::MOVE_IntoZipLine: return "MOVE_IntoZipLine";
        case EMovement::MOVE_ZipLine: return "MOVE_ZipLine";
        case EMovement::MOVE_Balance: return "MOVE_Balance";
        case EMovement::MOVE_LedgeWalk: return "MOVE_LedgeWalk";
        case EMovement::MOVE_GrabTransfer: return "MOVE_GrabTransfer";
        case EMovement::MOVE_MeleeAir: return "MOVE_MeleeAir";
        case EMovement::MOVE_DodgeJump: return "MOVE_DodgeJump";
        case EMovement::MOVE_WallRunDodgeJump: return "MOVE_WallRunDodgeJump";
        case EMovement::MOVE_Stumble: return "MOVE_Stumble";
        case EMovement::MOVE_StepUp: return "MOVE_StepUp";
        case EMovement::MOVE_RumpSlide: return "MOVE_RumpSlide";
        case EMovement::MOVE_Vertigo: return "MOVE_Vertigo";
        case EMovement::MOVE_MeleeSlide: return "MOVE_MeleeSlide";
        case EMovement::MOVE_WallClimbDodgeJump: return "MOVE_WallClimbDodgeJump";
        case EMovement::MOVE_WallClimb180TurnJump: return "MOVE_WallClimb180TurnJump";
        case EMovement::MOVE_Swing: return "MOVE_Swing";
        case EMovement::MOVE_Coil: return "MOVE_Coil";
        case EMovement::MOVE_MeleeWallrun: return "MOVE_MeleeWallrun";
        case EMovement::MOVE_MeleeCrouch: return "MOVE_MeleeCrouch";
        case EMovement::MOVE_FallingUncontrolled: return "MOVE_FallingUncontrolled";
        case EMovement::MOVE_SwingJump: return "MOVE_SwingJump";
        case EMovement::MOVE_SoftLanding: return "MOVE_SoftLanding";
        case EMovement::MOVE_AutoStepUp: return "MOVE_AutoStepUp";
        case EMovement::MOVE_AirBarge: return "MOVE_AirBarge";
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
    float u2 = 0.0f; // second UV set: mesh set 1 unless the section's material reads another (MaterialUVSlots)
    float v2 = 0.0f;
    uint32_t color = 0xFFFFFFFF;
    float tangent_sign = 1.0f; // Binormal = cross(normal, tangent) * tangent_sign (UE3 TangentZ.w)
    // Baked lighting (assets/level_lightmaps.hpp). lm_u >= 0: (lm_u, lm_v) is where the vertex sits in
    // its section's light-map textures, and lm0..2 are the three coefficients' scales. lm_u == -1: the
    // vertex has its own samples, lm0..2 the three coefficients. lm_u == -2: nothing was baked for it.
    // lm0..2 are linear RGB packed as RGB9E5 (pack_rgb9e5).
    float lm_u = -2.0f;
    float lm_v = 0.0f;
    uint32_t lm0 = 0;
    uint32_t lm1 = 0;
    uint32_t lm2 = 0;
};

// A light of the level that a light environment can gather (assets/level_lights.hpp).
struct LevelLight {
    enum class Kind : uint8_t { Directional, Point, Spot, Sky };
    Kind kind = Kind::Point;
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 direction{1.0f, 0.0f, 0.0f};    // the way the light travels (directional, spot)
    Vec3 color{0.0f, 0.0f, 0.0f};        // pow(LightColor, 2.2) * Brightness
    Vec3 lower_color{0.0f, 0.0f, 0.0f};  // sky: LowerColor, LowerBrightness
    float radius = 1024.0f;
    float falloff = 2.0f;
    float cos_inner = 1.0f;
    float cos_outer = 0.7193f;
    uint32_t channels = 0;               // LightingChannelContainer bits
    bool cast_shadows = true;
    bool cast_static_shadows = true;
    bool cast_composite = false;
    // On the world's dynamic light list (a *Movable light, bForceDynamicLight, a SkyLightToggleable):
    // no light environment gathers it; it lights whatever shares a channel with it, directly.
    bool dynamic_list = false;
};

// How something that is not baked level geometry is lit (renderer/light_environment.hpp): by its own
// light environment (a DynamicLightEnvironmentComponent, with these settings), or, where the level
// left the environment switched off, straight by the lights that share a channel with it.
struct DynamicLighting {
    enum class Mode : uint8_t { Environment, Direct, Unlit };
    Mode mode = Mode::Environment;
    float light_distance = 1.5f;
    float shadow_distance = 1.0f;
    float bounce = 0.1f;                // BouncedLightingIntensity
    float bounce_desaturation = -1.0f;  // BouncedLightingDesaturation
    float light_desaturation = 0.0f;
    Vec3 ambient_glow{0.0f, 0.0f, 0.0f};
    bool synthesize_point = true;
    bool synthesize_sh = true;
    // Its shadow (renderer/mod_shadow.hpp): bCastShadows, and the faint light from above that every
    // shadow environment has beside the level's lights.
    bool cast_shadows = true;
    Vec3 ambient_shadow_color{0.15f, 0.15f, 0.15f};
    Vec3 ambient_shadow_dir{0.0f, 0.0f, 1.0f};
    uint32_t channels = 1u << 3;        // the owner's LightingChannels: Dynamic
};

// A contiguous range of MeshBuffer::vertices drawn with one scene material
// (index into LevelScene::materials->materials, -1 = legacy procedural shading).
struct MeshSection {
    uint32_t first_vertex = 0;
    uint32_t vertex_count = 0;
    int32_t material = -1;
    int32_t lightmap = -1;  // light-map texture set (SceneMaterialLibrary::lightmap_textures[3 * lightmap ..]), -1 = none
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
    // A dynamic object (a lift's part, a door, an InterpActor or KActor the port leaves in place):
    // never light-mapped, lit as `lighting` says.
    bool dynamic_lit = false;
    DynamicLighting lighting;
    // The level's decals (assets/level_decals.hpp): they lie in their receivers' surfaces, so they are
    // drawn with a depth bias, after what they lie on and before the other translucent surfaces, and
    // cast no shadow.
    bool is_decal = false;
    // The one actor it draws, when the level's script can hide that actor (LevelActor::script_switched):
    // not drawn, and casting no shadow, while LevelScene::actors[actor].is_hidden.
    int32_t actor = -1;
};

struct SoundSubtitleLine {
    float time = 0.0f;
    std::string text;
};

struct SoundClip {
    std::string name;
    std::string full_path;
    std::vector<uint8_t> pcm_data;
    int sample_rate = 44100;
    int channels = 2;
    float duration = 0.0f;
    std::vector<SoundSubtitleLine> subtitles;
};

// One node of a SoundCue's graph, as USoundNode::ParseNodes walks it when the cue plays: a mixer
// plays all of its inputs, a random node one weighted child, delays and modulators offset / scale
// what is below them. Other node types (attenuation, mix groups, ...) pass through to their child.
struct SoundCueNode {
    enum class Kind : uint8_t { Passthrough, Wave, Mixer, Random, Delay, Modulator };
    Kind kind = Kind::Passthrough;
    std::string wave;            // Wave: the clip's name
    std::vector<int> children;   // indices into SoundCueDef::nodes (-1: missing)
    std::vector<float> weights;  // Mixer: InputVolume per child; Random: Weights per child
    float min_value = 0.0f;      // Delay: DelayDuration (s); Modulator: VolumeModulation
    float max_value = 0.0f;
    float min_pitch = 1.0f;      // Modulator: PitchModulation
    float max_pitch = 1.0f;
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
    bool is_concatenator = false;
    bool has_modulator = false;
    bool has_mixer = false;             // layers waves: played by evaluating `nodes`
    std::vector<std::string> wave_names;
    std::vector<float> wave_weights;
    std::vector<SoundCueNode> nodes;    // nodes[0] is the cue's FirstNode
    // Waves the graph plays from other packages (imports) as {package, wave name}, e.g.
    // {"A_CXP_Plaza", "Door_Hit"} for A_Props_Interactive's Doors.Door_Hit.
    std::vector<std::pair<std::string, std::string>> imported_waves;
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
// The level's post-process settings (assets/level_postprocess.hpp): the members of UE3's
// PostProcessSettings, as DICE extended it, that the game's chain takes from the world. The values
// here are the struct's defaults.
struct PostProcessSettings {
    // TdToneMappingPostProcess
    float scene_desaturation = 0.0f;
    Vec3 scene_highlights{1.0f, 1.0f, 1.0f};
    Vec3 scene_midtones{1.0f, 1.0f, 1.0f};
    Vec3 scene_shadows{0.0f, 0.0f, 0.0f};
    float exposure_manual = 1.0f;
    float exposure_speed_up = 3.5f;
    float exposure_speed_down = 4.5f;
    float exposure_high = 1.65f;
    float exposure_low = 0.85f;
    bool has_curves = false;
    Vec3 curve_m[16];  // per segment and channel: out = in * m + b
    Vec3 curve_b[16];
    // TdDirectionalHazePostProcess
    bool haze_enabled = false;
    Vec3 haze_color{1.0f, 1.0f, 0.8f};
    float haze_angle_curve = 5.0f;
    float haze_angle_start = 0.5f;
    float haze_distance_curve = 1.5f;
    float haze_distance_divider = 7500.0f;
    float haze_angle_clamp_high = 2.0f;
    float haze_total_clamp_close_high = 10.0f;
    float haze_total_clamp_far_high = 10.0f;
    float haze_total_clamp_far_distance = 1.0f;
    float haze_multiplier = 1.0f;
    float haze_total_clamp_low = 0.0f;
    Vec3 haze_sun_location{0.0f, 0.0f, 0.0f};
    // How long the view takes to go over to these settings (Scene_InterpolationDuration).
    float interpolation_duration = 1.0f;
};

// A material effect of the game's post-process chain (Effects/FX_PostProcess.upk): a material drawn
// over the whole picture, reading the picture so far as its scene colour.
struct PostEffectInfo {
    std::string name;                // EffectName: "HealthEffect", "UncontrolledFallingEffect", ...
    std::string material_path;       // the effect's material
    bool show_in_game = true;        // bShowInGame as shipped: on without script asking
    bool after_tone_mapping = true;  // where it stands in the chain against TdToneMappingPostProcess
    int32_t material = -1;           // its place in the scene's material library
};

// One of them switched on for a frame, with the parameters script has given its material.
struct ScreenEffect {
    std::string name;
    std::vector<std::pair<std::string, std::array<float, 4>>> params;
};

// A PostProcessVolume: its settings hold wherever the view is inside its brush, over the world's
// and over every volume of lower priority.
struct PostProcessVolumeInfo {
    struct Plane {
        Vec3 normal{0.0f, 0.0f, 1.0f};  // outward
        float d = 0.0f;                 // dot(normal, p) + d <= 0 inside
    };
    float priority = 0.0f;
    bool enabled = true;
    PostProcessSettings settings;
    std::vector<std::vector<Plane>> hulls;  // the brush's convex pieces, world space
    AABB bounds{Vec3(0.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, 0.0f)};

    [[nodiscard]] bool holds(const Vec3& p) const {
        if (p.x < bounds.min_pt.x || p.y < bounds.min_pt.y || p.z < bounds.min_pt.z || p.x > bounds.max_pt.x || p.y > bounds.max_pt.y ||
            p.z > bounds.max_pt.z) {
            return false;
        }
        for (const auto& hull : hulls) {
            bool inside = !hull.empty();
            for (const Plane& plane : hull) {
                if (plane.normal.dot(p) + plane.d > 0.01f) {
                    inside = false;
                    break;
                }
            }
            if (inside) return true;
        }
        return false;
    }
};

// One HeightFog actor.
struct HeightFogLayer {
    float height = 0.0f;                       // the fog plane: the actor's z
    float density = 0.00005f;
    float start_distance = 0.0f;
    float extinction_distance = 100000000.0f;
    Vec3 color{1.0f, 1.0f, 1.0f};              // LightColor (linear) * LightBrightness
};

// The baked lighting of an actor's StaticMeshComponent (assets/level_lightmaps.hpp).
struct ActorLightMap {
    enum class Kind : uint8_t { None = 0, Texture, Vertex };
    Kind kind = Kind::None;
    // Texture: three LightMapTexture2D exports of the package file, their scales, and where the
    // mesh's light-map UVs sit in them.
    std::string package_path;
    int32_t textures[3] = {0, 0, 0};
    Vec3 scale[3];
    float coord_scale[2] = {1.0f, 1.0f};
    float coord_bias[2] = {0.0f, 0.0f};
    // Vertex: three coefficients per LOD0 vertex of the mesh, scaled, as RGB9E5.
    std::vector<uint32_t> vertex_samples;
};

struct LevelActor {
    std::string class_name;
    std::string object_name;
    std::string unique_name;   // export name including the FName number suffix (e.g. "InterpActor_3")
    std::string base_name;     // unique_name of the Actor.Base this actor is attached to ("" = none)
    std::string mesh_name;
    std::string mesh_path;       // canonical UE3 object path (e.g. "VH_NYC_PoliceVehicles.PoliceCar_01.S_Policecar_01")
    std::string tag;
    Vec3 location{0.0f, 0.0f, 0.0f};
    Rotator rotation{0.0f, 0.0f, 0.0f};
    Vec3 pre_pivot{0.0f, 0.0f, 0.0f};
    Vec3 draw_scale_3d{1.0f, 1.0f, 1.0f};
    float draw_scale = 1.0f;
    Vec3 comp_translation{0.0f, 0.0f, 0.0f};
    Rotator comp_rotation{0.0f, 0.0f, 0.0f};
    Vec3 comp_scale_3d{1.0f, 1.0f, 1.0f};
    float comp_scale = 1.0f;
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
    bool can_exit_at_top = true;
    bool is_pipe = false;        // TdLadderVolume.LadderType: a drain pipe, climbed with its own animations
    bool is_ledge = false;
    bool is_springboard = false;
    bool is_balance_beam = false;
    bool is_swing_bar = false;
    bool is_soft_landing = false;
    bool is_fall_height_volume = false;
    float fall_height_target_z = 0.0f;
    bool is_movement_exclusion_volume = false;
    bool is_electric_volume = false;
    bool is_barbed_wire_volume = false;
    bool exclude_hand_moves = false;
    bool exclude_foot_moves = false;
    float damage_per_sec = 0.0f;
    bool is_enemy = false;
    bool is_bag = false;
    bool is_elevator_part = false;
    std::string source_package;
    ActorLightMap lightmap;
    int32_t component_export = 0;  // its mesh component, a 1-based export of source_package (decals name their receivers by it)
    bool accepts_decals = true;    // the component's bAcceptsDecals
    bool accepts_decals_in_game = true;  // its bAcceptsDecalsDuringGameplay: a bullet hole needs both
    bool dynamic_class = false;  // an InterpActor, a KActor...: never light-mapped
    // The level's script hides, shows or destroys it (SeqAct_ToggleHidden, SeqAct_Destroy): it is
    // drawn from a mesh buffer of its own (MeshBuffer::actor), whether or not it starts hidden.
    bool script_switched = false;
    bool script_damage = false;  // a SeqEvent_TakeDamage of the level's script listens on it
    // The script changes its collision (SeqAct_ChangeCollision, SeqAct_Destroy): its triangles are in
    // the collision world even when it starts with none (a pane's broken twin). And how it started,
    // for a checkpoint's reload.
    bool script_collision = false;
    bool initial_hidden = false;
    bool initial_collidable = false;
    bool initial_blocks_traces = false;
    bool interactable = false;   // Actor.bInteractable: what a barge can be thrown at
    DynamicLighting lighting;    // how it is lit then
    Vec3 end_point{0.0f, 0.0f, 0.0f};
    Vec3 wall_normal{0.0f, 0.0f, 0.0f};
    // TdZiplineVolume.SplineLocations: the cable the pawn rides, NumSplineSegments + 1 points on the
    // quadratic Bezier Start -> Middle -> End (location / end_point keep Start / End). Empty for
    // every other actor; a zipline without it is ridden along the straight Start -> End line.
    std::vector<Vec3> spline_points;
    // TdZiplineVolume.MoveDirection: the way the cable is ridden (the volume's X axis); the pawn grabs
    // it only facing within 90 deg of this (TdMove_IntoZipLine.CanDoMove). Zero when not serialized.
    Vec3 move_direction{0.0f, 0.0f, 0.0f};
    // StaticMeshComponent.Materials overrides (full object paths, "" = use the mesh element's material)
    std::vector<std::string> material_overrides;
    // By mesh element, the physical material of its material (LevelScene::physical_materials, -1 none).
    std::vector<int16_t> element_physical;
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
    // What fired it, for the mark it leaves (game/impact_effects.hpp): 0 a light weapon, 1 a heavy
    // one, 2 a helicopter's gun, 3 a shotgun's pellet.
    uint8_t ammo = 0;
    bool impact_done = false;  // its impact effect and its bullet hole have been made
    float damage = 20.0f;      // what it does to the level actor it hits (the level's script hears of it)
    // The person it stopped in (TdWeapon.RegisterPendingImpact / PlayImpactEffects): 0 nobody;
    // 1 a bot the player shot, who gets his body's impact effect and sound; 2 the player, who
    // gets the sound alone; 3 a bot that gets nothing (shot by another bot, or dying).
    uint8_t pawn_hit = 0;
    Vec3 pawn_normal{0.0f, 0.0f, 0.0f};  // out of the body where the bullet went in (zero: not known)
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
    // The weapon still in his hands while it is being taken off him, and for how much longer
    // (TdMove_Disarm: hers is attached to her hand only as the move ends, a shotgun's part way in).
    std::string disarm_weapon;
    float disarm_weapon_time = 0.0f;
    EEnemyAnimState anim_state = EEnemyAnimState::Idle;
    std::string active_anim_seq;
    float anim_timer = 0.0f;
    float anim_duration = 1.0f;
    float muzzle_flash_timer = 0.0f;
    int burst_shots_left = 0;
    float burst_cooldown = 0.0f;
    bool is_story_npc = false;      // Non-combat character mesh (Kate, Celeste, Jacknife, Ropeburn, Miller, Kreeg)
    bool cutscene_only = false;     // Shown only while matching Matinee cutscene (`cutscene_label`) is playing
    std::string sublevel_pkg;       // Owning sublevel stem (e.g. "Escape_Off_Spt" / "Escape_Off_CS")
    std::string cutscene_label;     // Matinee label (e.g. "Escape_Off_CS.SeqAct_Interp_8")
    std::string cutscene_pkg_path;  // Full filesystem path to .me1 containing cutscene AnimSequence
    int32_t cutscene_anim_exp_1 = 0;// 1-based export index of cutscene AnimSequence (for parse_single_anim_sequence)
    float cutscene_start_sec = 0.0f;
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
    // (TdMove_SkillRoll lasts as long as its fallinglandroll animation: kSkillRollLength in parkour_controller.cpp.)
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
    Vec3 hinge_pos{0.0f, 0.0f, 0.0f};      // World-space vertical hinge pin position (the leaf actor's Location)
    Vec3 center_pos{0.0f, 0.0f, 0.0f};     // World-space center of closed door slab
    AABB closed_bounds;                    // World-space AABB of doorway (for barge traces & triggers)
    DoorState state = DoorState::Closed;
    float open_angle_rad = 0.0f;           // Current signed Z-rotation around hinge_pos (radians)
    // The level's door Kismet (SPT_OnewayDoor_Seq): SeqEvent_TakeDamage plays the open matinee
    // (0.6 s), a Delay of 3 s, then the close matinee (0.6 s). anim_time is the playing matinee's
    // position; swing_sign is the side the leaf swings to (away from whoever opened it).
    float anim_time = 0.0f;
    float swing_sign = 1.0f;
    float hold_timer = 0.0f;               // Delay left while held open (Open state)
    bool barged = false;                   // True when slammed open via MOVE_Barge
    std::vector<DoorPart> parts;           // Door leaf mesh, attached closer bar, and hidden doorway slab
    Mat4 model_matrix = Mat4::identity();  // T(hinge_pos) * Rz(open_angle_rad) * T(-hinge_pos)
};

// -----------------------------------------------------------------------------
// Reverse-Engineered Helicopter & Scripted Gunfire Encounters
// (TdVehicle_Helicopter, TdAI_HeliController, TdAttackPathNode, SeqAct_TdHelicopterFactory,
//  SeqAct_SetHeliTarget, SeqAct_SetHeliSpeed, SeqAct_TdDummyWeaponFire in TdGame.u)
// -----------------------------------------------------------------------------
enum class EHeliAttackSide : uint8_t {
    Right = 0,               // ESide_Right
    Left = 1,                // ESide_Left
    UseLeftWhenHovering = 2, // ESide_UseLeftWhenHovering
    UseRightWhenHovering = 3,// ESide_UseRightWhenHovering
    Both = 4,                // ESide_Both
    None = 5                 // ESide_None
};

enum class EHeliSpeed : uint8_t {
    FastestDefault = 0,      // EHSpeed_Fastest_Default (2000 uu/s)
    Fast = 1,                // EHSpeed_Fast (1500 uu/s)
    Slow = 2,                // EHSpeed_Slow (1000 uu/s)
    Slower = 3,              // EHSpeed_Slower (650 uu/s)
    Slowest = 4              // EHSpeed_Slowest (350 uu/s)
};

enum class EHeliState : uint8_t {
    Dormant = 0,             // Waiting for Kismet SeqEvent_TdTouch / checkpoint trigger
    Arriving = 1,            // Flying in; HoldFire delay active (e.g. 6.0s in Escape_R1_Spt)
    Engaging = 2,            // Selecting TdAttackPathNode & firing FNMinimi bursts at Faith
    Retreating = 3,          // SeqAct_AIHoldFire + SeqAct_AIMoveToActor retreat when Faith reaches cover
    Destroyed = 4            // SeqAct_Destroy completed
};

struct HeliAttackNode {
    std::string object_name;
    Vec3 location{0.0f, 0.0f, 0.0f};
    float yaw_deg = 0.0f;
    float attack_radius = 3000.0f;  // TdAttackPathNode.AttackVolumeRadius
    float attack_height = 2000.0f;  // TdAttackPathNode.AttackVolumeHeight
    float attack_angle = 45.0f;     // TdAttackPathNode.AttackVolumeAngle
    int32_t exposure = 80;          // TdAttackPathNode.Exposure
    float last_visit_time = -1000.0f;
};

struct DummyFireBarrage {
    std::string object_name;
    Vec3 origin{0.0f, 0.0f, 0.0f};  // SeqAct_TdDummyWeaponFire.Origin actor world location
    Vec3 target{0.0f, 0.0f, 0.0f};  // SeqAct_TdDummyWeaponFire.Target actor world location
    Vec3 trigger_pos{0.0f, 0.0f, 0.0f};
    float trigger_radius = 1200.0f;
    int32_t shots_to_fire = 18;
    float spread_deg = 12.0f;       // MaxSpread FRotator units converted to degrees
    float delay_sec = 0.25f;
    bool activated = false;
    float timer = 0.0f;
    int32_t shots_fired = 0;
};

struct HelicopterInstance {
    std::string object_name;
    std::string mesh_name = "SK_SWAT_Blackhawk_01";
    Vec3 spawn_pos{0.0f, 0.0f, 0.0f};
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 velocity{0.0f, 0.0f, 0.0f};
    float yaw_deg = 0.0f;
    float pitch_deg = 0.0f;
    float roll_deg = 0.0f;

    Vec3 trigger_pos{0.0f, 0.0f, 0.0f};
    float trigger_radius = 2800.0f;
    bool has_escape_trigger = false;
    Vec3 escape_pos{0.0f, 0.0f, 0.0f};
    float escape_radius = 1200.0f;
    Vec3 retreat_dest{0.0f, 0.0f, 0.0f};

    float hold_fire_delay = 6.0f;    // SeqAct_Delay before releasing SeqAct_AIHoldFire
    float perfect_aim_delay = 30.0f; // Anti-camping SeqAct_Delay -> SeqAct_TdAIPerfectAim
    int32_t gunner_count = 1;
    EHeliAttackSide side_preference = EHeliAttackSide::UseLeftWhenHovering;
    EHeliSpeed speed_setting = EHeliSpeed::FastestDefault;

    EHeliState state = EHeliState::Dormant;
    float active_timer = 0.0f;
    float main_rotor_rad = 0.0f;
    float tail_rotor_rad = 0.0f;
    float burst_timer = 0.0f;
    float muzzle_flash_timer = 0.0f;
    int32_t current_node_idx = -1;
    bool perfect_aim_active = false;
    bool just_spawned = false;
    bool just_fired = false;
};

// -----------------------------------------------------------------------------
// Input Frame (Mapped from Keyboard, Mouse, or Gamepad)
// -----------------------------------------------------------------------------
struct InputFrame {
    float forward = 0.0f;          // +1 = forward, -1 = backward
    float strafe = 0.0f;           // +1 = right, -1 = left
    float look_yaw_delta = 0.0f;   // Horizontal mouse delta (degrees)
    float look_pitch_delta = 0.0f; // Vertical mouse delta (degrees)
    // The look deltas steer to a recorded view (the retail replay harness): that view already went
    // through retail's per-move camera rules, so the look locks / constraints are not applied again.
    bool view_recorded = false;
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

// A sound the simulation started this frame (animation notifies, the door Kismet / matinee sound
// tracks). `cue` is a SoundCue name or package-relative path (e.g. "Doors.Door_Barge").
struct SimSoundEvent {
    std::string cue;
    Vec3 location{0.0f, 0.0f, 0.0f};
    bool at_pawn = true;  // true: played on the player (2D); false: 3D at `location`
};

// -----------------------------------------------------------------------------
// Player Telemetry (Every simulation tick)
// -----------------------------------------------------------------------------
struct PlayerTelemetry {
    // The screen fade (TdHUD's FadeInEffect): 1 = the picture, 0 = all fade_color.
    float fade_amount = 1.0f;
    // The chain's material effects that are on this frame (game/screen_effects.hpp).
    std::vector<ScreenEffect> screen_effects;
    Vec3 fade_color{1.0f, 1.0f, 1.0f};
    // TdHUD.PostBeginPlay's WorldInfo.SetSceneExposureReset: for this frame, the exposure goes back
    // to where a level opens (its high clamp) and adapts from there.
    bool exposure_reset = false;
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
    bool move_input = false;  // the player is pushing a direction (the pawn's Acceleration is not zero)
    // What the current move read off the level, for the first-person animation (fp::PawnFrame).
    // `move_anim` names the animation a move with a choice picked ("VaultOverHigh", "HangFreeHeaveUp"),
    // or is "@reached" when it gets to the place it was steering for; `move_anim_serial` counts them,
    // so each is played once however often the telemetry is read.
    std::string move_anim;
    uint32_t move_anim_serial = 0;
    float move_anim_rate = 0.0f;    // the rate the move worked out for move_anim (TdMove_Barge's AnimPlayRate); 0: its script's own
    float ground_distance = -1.0f;  // falling fast: the feet above what is under them; -1 not known
    bool jump_over_gap = false;     // TdMove_Jump.StartJump: nothing to land on 1.1 x the speed ahead
    bool move_left = false;         // a dodge jump going left
    bool hanging_free = false;      // hanging with no wall for the legs
    // The first-person mesh, and the eye in it, away from where the pawn's place puts them: kept off
    // a wall (TdPawn.OffsetMeshXY), and in z held back over a step (TdPawn.SmoothOffset) and let
    // down as the pawn's own animation comes back after a cutscene (physics/parkour_controller.cpp).
    Vec3 camera_mesh_offset{0.0f, 0.0f, 0.0f};
    float mesh_smooth_z = 0.0f;     // TdPawn.SmoothOffset alone: what the feet's placement reads
    bool ledge_sloped = false;      // TdMove_Grab.bSlopedLedge: the ledge's top is not level
    float ledge_slope_deg = 0.0f;   // how steeply the ledge runs up to her right
    float swing_angle = 0.0f;       // radians from hanging straight down, positive ahead of the bar
    float body_yaw_deg = 0.0f;      // TdPawn.Rotation.Yaw: where the body faces while the view looks round
    bool climbing_pipe = false;     // the ladder volume being climbed is a pipe
    float climb_top = -1.0f;        // uu up to the ladder's last step (-1 off a ladder)
    float climb_bottom = -1.0f;     // uu down to its first
    float balance_lean = 0.0f;      // -1 .. 1 off the beam
    int balance_danger = 0;         // losing her balance to the left (-1) or the right (1)
    int against_wall = 0;           // TdPlayerPawn.AgainstWallState: 0 no, 1 both hands, 2 the left, 3 the right
    Vec3 against_wall_hand[2];      // TdPawn.AgainstWallLeftHand / AgainstWallRightHand: where each hand's trace met the wall
    // The ledge of the hang or vault under way (TdPawn.MoveLedgeLocation, MoveNormal, MoveLedgeNormal).
    bool ledge_known = false;
    Vec3 ledge_point{0.0f, 0.0f, 0.0f};
    Vec3 ledge_wall_normal{0.0f, 0.0f, 0.0f};
    Vec3 ledge_top_normal{0.0f, 0.0f, 1.0f};
    bool floor_sloped = false;      // Pawn.Floor.Z != 1: the floor she stands on is not level
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
    bool snatch_weapon_attached = true;  // TdMove_Disarm: the weapon is in her hand yet (AttachWeaponToHand)
    bool melee_hit_confirmed = false;
    bool disarm_prompt_visible = false;
    float hit_marker_timer = 0.0f;
    // Hits the pawn took, for the screen effects (game/screen_effects.hpp): a count that goes up
    // with each, and the last one's damage. A melee hit also has where it came from, in turns
    // from straight ahead (0.5 = from behind).
    uint32_t melee_hit_count = 0;
    float melee_hit_damage = 0.0f;
    float melee_hit_turns = 0.0f;
    uint32_t fall_hit_count = 0;
    float fall_hit_damage = 0.0f;
    float damage_flash_timer = 0.0f;
    // First-person camera slot animation (ATdPlayerPawn::PlayHitCameraShake / PlayTaserCameraShake):
    // separate from move_anim so taking a hit on the same frame a move starts does not clobber move_anim.
    std::string camera_anim;
    uint32_t camera_anim_serial = 0;
    // UTdHudEffectManager (DefaultHudEffects.ini + FX_PostProcess.upk + FX_FirstPEffects.upk):
    // - health_desat: FXFScreen_HealthDesaturate (PPHealthSaturationSettings: +Damage*0.01 up to 1.0,
    //   FadeIn 0.06s, Hold 0.5s, FadeOut 0.5s)
    // - hit_blur: DOFAndBloomPP MaxFarBlurAmount (TdHudEffect_Bullet/Melee/FallDamage: up to 0.95,
    //   FocusDistance = -500, FadeIn 0.03s, Hold 0.25s, FadeOut 0.15s)
    // - hit_focus_distance: DOFAndBloomPP FocusDistance (1600 at rest, -500 during hit blur)
    // - melee_damage_strength: FXFScreen_MeleeDamage (FadeIn 0.05s, Hold 0.15s, FadeOut 0.4s)
    // - melee_hit_dir: FXFScreen_MeleeHitDirection in [0, 1) (0=front, 0.25=right, 0.5=back, 0.75=left)
    // - fall_damage_strength: FXFScreen_FallDamage (FadeIn 0.05s, Hold 0.1s, FadeOut 0.75s)
    // - bullet_hit_angles / bullet_hit_timers: active PS_FX_FullScreenFX_BulletHit_01 directional blood
    //   splatter bursts around the screen periphery (up to 4 concurrent slots, 0.25s lifetime).
    float health_desat = 0.0f;
    float hit_blur = 0.0f;
    float hit_focus_distance = 1600.0f;
    float melee_damage_strength = 0.0f;
    float melee_hit_dir = 0.0f;
    float fall_damage_strength = 0.0f;
    float bullet_hit_angles[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float bullet_hit_timers[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    uint32_t bullet_hit_seeds[4] = {0u, 0u, 0u, 0u};
    bool falling_to_death = false;
    bool fall_death_impact = false;
    float death_anim_progress = 0.0f;
    std::string active_subtitle;
    // The controller put the player back at the checkpoint this frame (a death). The game resets
    // the level script on it, as retail reloads the level.
    bool respawned = false;

    // What the level's Kismet puts on the screen (docs/GAMEPLAY_SCRIPTING_RE.md, section 4):
    // TdUIScene_TutorialHUDMessage's card (SeqAct_TdTutorialMessage), the supers line with the
    // district and time of day (SeqAct_TdSupersMessage), a sign's text when it is looked at
    // (SeqAct_TdTriggerSubtitle) and the pausing hint card (SeqAct_TdTriggerSplashHint).
    std::string tutorial_text;
    std::string supers_text;
    float supers_time_left = 0.0f;
    std::string sign_text;
    float sign_time_left = 0.0f;
    std::string splash_hint_title;
    std::string splash_hint_text;
    // The skip prompt (TdPopUps.PopUp4): a skippable cutscene is playing.
    bool skip_prompt = false;

    // TdMove_Barge custom animation slot: 0 none, 1 BargeInLeft, 2 BargeOutLeft, 3 MeleeKickObject;
    // the sequence position (seconds) and the slot's blend weight over the locomotion pose.
    int barge_anim = 0;
    float barge_anim_pos = 0.0f;
    float barge_anim_weight = 0.0f;
    // Sounds the simulation started during the last step() (drained by the game loop).
    std::vector<SimSoundEvent> sound_events;

    // Cooked level intro Matinee / 1P skeletal animation playback state
    bool intro_active = false;
    // The cutscene is seen through a placed CameraActor (a Matinee director cut: the training
    // area's pan across the roofs), not through the pawn: there is no first-person body in it.
    bool intro_camera_only = false;
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

// One sound a level intro asks for: a Kismet action behind one of the Matinee's event keys, or a
// notify of its animation.
struct IntroSoundEvent {
    float time = 0.0f;   // seconds from the start of the Matinee
    std::string cue;     // SoundCue, with its group ("Cloth.Run", "Movement.Vault")
    std::string bank;    // the content package the cue lives in ("A_Props_Interactive"); empty for a footstep
    int footstep = 0;    // AnimNotify_Footstep: the footstep cue's number (1 Sneak .. 10 LandHard); 0 = play `cue`
    bool voice = false;  // a dialogue line
    // From the animation's own notifies rather than from Kismet. When the level script runs the
    // Kismet itself, only these are played from the baked list; the rest come from the script.
    bool from_notify = false;
};

// One screen fade a level intro asks for (SeqAct_TdFadeEffect): at its start, behind one of the
// Matinee's event keys, or when another fade completes.
struct IntroFadeEvent {
    float time = 0.0f;      // seconds from the start of the Matinee
    bool fade_out = false;  // towards the colour; else back to the picture
    float duration = 0.5f;  // FadeTime
    Vec3 color{0.0f, 0.0f, 0.0f};
};

// A door the intro swings: the InterpActor one of its movement tracks turns (the door Faith kicks
// open in Boat, barges through in Subway and the Mall), as its yaw from closed through the Matinee.
struct IntroDoorSwing {
    Vec3 hinge{0.0f, 0.0f, 0.0f};                    // the InterpActor's Location
    std::vector<std::pair<float, float>> yaw_keys;   // (seconds from the start of the Matinee, degrees from closed)
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
    float matinee_length_sec = 0.0f;       // InterpData.InterpLength: how long the intro holds the player

    // The first-person view through the animation, in world space, one entry per animation frame
    // (evenly spaced over duration_sec): the EyeJoint's position, its view direction and up vector,
    // and the root bone (the feet).
    std::vector<Vec3> cam_pos;
    std::vector<Vec3> cam_forward;
    std::vector<Vec3> cam_up;
    std::vector<Vec3> root_pos;

    std::vector<IntroSoundEvent> sounds;   // sorted by time
    std::vector<IntroFadeEvent> fades;     // sorted by time
    std::vector<std::string> stop_cues;    // cues the Matinee stops when it completes or is skipped
    std::vector<IntroDoorSwing> door_swings;

    // The animations the pawn's group plays in turn (InterpTrackAnimControl.AnimSeqs), for the
    // first-person mesh: which sequence is on at a Matinee time. The intro has one; the Flight
    // outro two (sp01b_outro_part1, then part2 from 1.83 s). Empty for a pre-segment sequence
    // (seq_name / anim_export_index_1 / start_offset_sec / duration_sec describe the one).
    struct Segment {
        std::string seq_name;
        int32_t anim_export_index_1 = 0;
        float start_sec = 0.0f;   // StartTime in the Matinee
        float length_sec = 0.0f;  // SequenceLength
    };
    std::vector<Segment> segments;
    std::string interp_package;        // stem of the package holding the SeqAct_Interp, lower case
    int32_t interp_export_index_1 = 0; // the SeqAct_Interp
    bool skippable = true;             // SeqAct_Interp.bIsSkippable
};

// -----------------------------------------------------------------------------
// Level Scene representation
// -----------------------------------------------------------------------------
// A raw distribution's lookup table, as the cooked data carries it (FRawDistribution):
// [min, max, entries of `chunk` floats ...], read at (x - start_time) * time_scale.
struct RawDistribution {
    std::vector<float> table;
    int32_t chunk = 1;
    float time_scale = 0.0f;
    float start_time = 0.0f;
    bool valid = false;  // a constant or a curve (op 1); a lens flare's are all that
    // RawDistributionOperation: 0 nothing (the value is 0), 1 a plain value, 2 uniform between the
    // entry's minimum and maximum halves, 3 one of the two.
    int32_t op = 0;
};

// A particle emitter's module (assets/level_particles.hpp): what it does to a particle, when it
// is spawned, every frame, or both. `a`, `b`, `c` and `flag` are the module's own properties, in
// the order level_particles.cpp names them.
struct ParticleModuleInfo {
    enum class Kind : uint8_t {
        Lifetime, Size, Velocity, Rotation, RotationRate, Color, ColorOverLife, SizeMultiplyLife, SubUV,
        AccelerationOverLifetime, Acceleration, Location, VelocityOverLifetime, RotationRateMultiplyLife,
        LocationSphere, LocationCylinder, ColorScaleOverLife,
        MeshRotation, MeshRotationRate, MeshRotationRateMultiplyLife,  // a mesh particle's three-axis rotation
        Orbit,            // an offset turning about the particle's path
        LocationEmitter,  // spawned where another emitter's particles are
        AxisLock,         // the sprite's up axis held to an axis of the world
    };
    Kind kind = Kind::Lifetime;
    RawDistribution a, b, c;
    bool flag[3] = {false, false, false};
    std::string name;   // LocationEmitter: the emitter whose particles it follows (lower case)
    int32_t link = 0;   // LocationEmitter: 1 picks its source in turn, 0 at random. AxisLock: 1..6 = +X +Y +Z -X -Y -Z
    float scale = 1.0f; // LocationEmitter: InheritSourceVelocityScale
};

struct ParticleBurst {
    float count = 0.0f;
    float count_low = -1.0f;  // below 0: always `count`
    float time = 0.0f;        // of the emitter's duration, 0..1
};

// A sprite emitter: its first LOD level.
struct ParticleEmitterInfo {
    std::string name;
    int32_t material = -1;         // scene material
    uint8_t screen_alignment = 0;  // 0 PSA_Square, 1 PSA_Rectangle, 2 PSA_Velocity
    bool local_space = false;      // bUseLocalSpace: the particles move with the emitter
    float duration = 1.0f;         // EmitterDuration, seconds a loop
    int32_t loops = 0;             // EmitterLoops, 0: for ever
    float delay = 0.0f;
    bool delay_first_loop_only = false;
    uint8_t interpolation = 0;     // sub-images: 0 none, 1 linear, 2 linear blend, 3 random, 4 random blend
    int32_t sub_images_h = 1;
    int32_t sub_images_v = 1;
    int32_t max_draw_count = 500;  // 0: no limit
    RawDistribution rate;          // particles a second
    RawDistribution rate_scale;
    bool process_rate = true;
    bool process_bursts = true;
    std::vector<ParticleBurst> bursts;
    std::vector<ParticleModuleInfo> modules;  // the enabled ones, in the level's order
    bool enabled = true;              // this LOD level's bEnabled: off, it spawns nothing
    bool kill_on_deactivate = false;  // its particles go at once when the system is switched off
    // A mesh emitter (ParticleModuleTypeDataMesh) draws a static mesh a particle.
    int32_t mesh = -1;                    // LevelScene::particle_meshes
    std::vector<int32_t> mesh_materials;  // ParticleModuleMeshMaterial, by mesh section; -1: the mesh's own
    // The LOD levels after the first, each a whole emitter of its own values; the system's distance
    // from the view picks one (ParticleSystemTemplate::lod_distances).
    std::vector<ParticleEmitterInfo> lower_lods;
};

// A static mesh a mesh emitter draws, in its own space.
struct ParticleMeshInfo {
    std::string path;
    std::vector<Vertex> vertices;       // a triangle list
    std::vector<MeshSection> sections;  // by material
};

struct ParticleSystemTemplate {
    std::string path;
    float warmup_time = 0.0f;  // seconds run before it is first seen
    std::vector<float> lod_distances;  // LODDistances: level i from this far
    float lod_check_time = 0.25f;      // seconds between looks at the distance
    std::vector<ParticleEmitterInfo> emitters;
    int32_t emitters_left_out = 0;  // mesh or PhysX emitters, or a module the port does not run
};

// An Emitter actor.
struct ParticleSystemPlacement {
    std::string name;
    int32_t template_index = -1;
    Vec3 location{0.0f, 0.0f, 0.0f};
    Vec3 axis_x{1.0f, 0.0f, 0.0f};  // the actor's axes in the world
    Vec3 axis_y{0.0f, 1.0f, 0.0f};
    Vec3 axis_z{0.0f, 0.0f, 1.0f};
    Vec3 scale{1.0f, 1.0f, 1.0f};   // DrawScale * DrawScale3D
    bool active = true;             // bAutoActivate; off: waits for the level's script
    // The level's script switches it by its actor (game/level_script.hpp): the package's stem in
    // lower case and the actor's export. Each "turn on" counts, so that a system that has run out
    // starts again.
    std::string package;
    int32_t export_index = 0;
    uint32_t activations = 0;
};

// A bullet hole's template: a DecalComponent of a physical material's lists (assets/level_impacts.hpp).
struct ImpactDecalInfo {
    int32_t material = -1;     // the scene's material
    float width = 16.0f;
    float height = 16.0f;
    float rotation = -360.0f;  // DecalRotation: 360 any angle, -360 along the bullet's way, else that angle
    bool no_clip = false;
};

// A surface's physical material, as far as a bullet's impact reads it.
struct PhysicalMaterialInfo {
    std::string path;
    int32_t parent = -1;
    // TdPhysicalMaterialImpactEffects: the particle template by ammunition (BulletTracer::ammo), -1 none.
    int32_t effects[4] = {-1, -1, -1, -1};
    // TdPhysicalMaterialImpactSounds.LightAmmo, the one every weapon plays (TdWeapon.
    // GetWeaponSpecificImpactSound): the cue as "Group.Name", its package, and the MaxRadius of
    // its attenuation node, past which it is not heard.
    std::string impact_sound;
    std::string impact_sound_package;
    float impact_sound_radius = 2000.0f;
    // TdPhysicalMaterialDecals, by the weapon's decal type: light, heavy, shotgun.
    bool has_decals = false;
    float critical_angle = 0.0f;  // degrees from the surface's normal: under it an impact, over it a ricochet
    std::vector<ImpactDecalInfo> impact[3];
    std::vector<ImpactDecalInfo> ricochet[3];
};

// A particle system made while the game runs: a bullet's impact, an actor factory's emitter.
struct SpawnedEffect {
    uint32_t id = 0;  // never used twice in a level
    ParticleSystemPlacement at;
    float life = 0.0f;     // seconds left
    bool forever = false;  // an actor factory's: it stays
};

// What a bullet did to a level actor, for the level's script (SeqEvent_TakeDamage, SeqEvent_Death).
struct ActorDamage {
    int32_t actor = -1;  // LevelScene::actors
    float amount = 0.0f;
    bool by_player = false;
    uint8_t type = 0;    // 0 a bullet (TdDmgType_Bullet), 1 a barge (TdDmgType_Barge), 2 a blow (TdDmgType_Melee)
};

// A decal made while the game runs (a bullet hole), already clipped to what it lies on.
struct DynamicDecal {
    int32_t material = -1;
    float life = 30.0f;  // DecalManager.DecalLifeSpan
    std::vector<Vertex> vertices;
    // What it lies on, when that moves (the decal is attached to its HitComponent's actor): 0 the
    // level, 1 a part of a lift (elevators[mover].parts[part]), 2 a door (barge_doors[mover]).
    // The vertices are then in the mover's place as the level has it, and go with it when drawn.
    uint8_t mover_kind = 0;
    int32_t mover = -1;
    int32_t part = -1;
};

// An emitter an actor factory of the level's script makes (ActorFactoryEmitter).
struct EffectFactory {
    std::string package;       // the package's stem in lower case
    int32_t export_index = 0;  // the factory object
    int32_t template_index = -1;
};

// One quad of a lens flare (LensFlareElement), assets/level_lensflares.hpp.
struct LensFlareElement {
    std::string name;
    float ray_distance = 0.0f;  // where along the line source -> screen centre -> beyond: 0 the source, 0.5 the centre
    bool enabled = false;
    bool use_source_distance = false;
    bool normalize_radial_distance = false;
    bool modulate_color_by_source = false;
    float size_x = 0.0f;
    float size_y = 0.0f;
    std::vector<int32_t> materials;  // LFMaterials, as scene materials (-1: none)
    RawDistribution material_index, scaling, axis_scaling, rotation, color, alpha;
    RawDistribution dist_scale, dist_color, dist_alpha;  // by the distance to the source
};

// A LensFlare template.
struct LensFlareTemplate {
    std::string path;
    std::vector<LensFlareElement> elements;  // the reflections that have a material, in drawing order
    float outer_cone = 0.0f;                 // degrees; 0: no cone
    float inner_cone = 0.0f;
    float cone_fudge_factor = 0.5f;
    float radius = 0.0f;
    // Coverage of the view -> LensFlareOcclusion. The class's own is a constant 1.
    RawDistribution screen_percentage_map{{1.0f, 1.0f, 1.0f, 1.0f}, 1, 0.0f, 0.0f, true};
    Vec3 box_center{0.0f, 0.0f, 0.0f};  // FixedRelativeBoundingBox, around the source
    Vec3 box_extent{0.0f, 0.0f, 0.0f};
    bool box_inverted = false;          // stored with Min above Max
};

// A LensFlareSource actor.
struct LensFlareSourceInfo {
    std::string name;
    int32_t template_index = -1;
    Vec3 location{0.0f, 0.0f, 0.0f};
    Vec3 forward{1.0f, 0.0f, 0.0f};  // the actor's X axis: what a template's cone is measured from
    float source_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    bool active = true;     // bAutoActivate; off: waits for the level's script
    bool hidden = false;
    bool has_base = false;  // rides something that moves
    std::string package;    // as ParticleSystemPlacement's: how the level's script names it
    int32_t export_index = 0;
};

struct ScriptGraph;

struct LevelScene {
    std::string map_name;
    std::string chapter_title;
    Vec3 player_spawn_pos{0.0f, 0.0f, 100.0f};
    float player_spawn_yaw = 0.0f;
    LevelIntroSequence level_intro{};
    // Every Matinee that moves the local pawn (SeqVar_TdLocalPawn in one of its groups), baked
    // like the intro: the intro itself, the cutscenes the level's triggers start, the outro.
    // LevelScene::script plays them by index (ScriptMatinee::player_cutscene).
    std::vector<LevelIntroSequence> cutscenes;
    // The level's Kismet (src/game/level_script.hpp); null when the level has none.
    std::shared_ptr<const ScriptGraph> script;
    // The script sets the checkpoints (SeqAct_TdCheckpoint): the controller's proximity
    // checkpoints are off. False for a level without a script (the tutorial's staged list).
    bool script_checkpoints = false;
    std::vector<PostProcessVolumeInfo> post_volumes;  // highest priority first
    std::vector<PostEffectInfo> post_effects;         // the chain's material effects, in its order
    std::vector<LevelLight> lights;                   // what lights the dynamic objects
    std::vector<LensFlareTemplate> lens_flare_templates;
    std::vector<LensFlareSourceInfo> lens_flares;
    std::vector<ParticleSystemTemplate> particle_templates;
    std::vector<ParticleSystemPlacement> particle_systems;
    std::vector<ParticleMeshInfo> particle_meshes;
    // What a bullet leaves where it lands (assets/level_impacts.hpp, game/impact_effects.hpp).
    std::vector<PhysicalMaterialInfo> physical_materials;
    int32_t default_physical = -1;         // TdWeapon.DefaultImpactMaterial
    int32_t character_physical = -1;       // the bodies of the bots' physics asset: PM_Character_Body
    ImpactDecalInfo default_impact_decal;  // TdWeapon.InitDefaultDecalProperties
    std::vector<EffectFactory> effect_factories;
    std::vector<SpawnedEffect> spawned_effects;
    uint32_t next_effect_id = 1;
    std::vector<DynamicDecal> dynamic_decals;
    std::vector<ActorDamage> actor_damage;  // since the script was last told (game/script_effects.hpp)
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
    std::shared_ptr<CollisionWorld> collision;
    float kill_z = -1.0e30f;  // falling below this kills the player (WorldInfo.KillZ or geometry floor)
    std::vector<EnemyBot> enemies;
    std::vector<BulletTracer> active_tracers;
    std::vector<DroppedWeapon> dropped_weapons;
    std::vector<HeliAttackNode> heli_attack_nodes;
    std::vector<HelicopterInstance> helicopters;
    std::vector<DummyFireBarrage> dummy_fire_barrages;

    struct KismetValveProp {
        std::string object_name;
        Vec3 position{0.0f, 0.0f, 0.0f};
        float yaw_deg = 0.0f;
        int32_t required_revs = 1;
    };
    struct KismetLookAtPoint {
        std::string object_name;
        Vec3 position{0.0f, 0.0f, 0.0f};
        float duration_sec = 0.5f;
        float interp_time_sec = 0.1f;
    };
    struct KismetLevelTransition {
        std::string object_name;
        std::string next_level_name;
        std::string next_checkpoint_name;
    };
    std::vector<KismetValveProp> kismet_valves;
    std::vector<KismetLookAtPoint> kismet_lookat_points;
    std::vector<KismetLevelTransition> kismet_level_transitions;
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
    PostProcessSettings post;
    std::vector<HeightFogLayer> height_fog;  // highest first, at most four
};

} // namespace me

