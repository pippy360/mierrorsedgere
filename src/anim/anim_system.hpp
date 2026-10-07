#pragma once

#include "../math/types.hpp"
#include "../assets/upk_loader.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace me {

struct Quat4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;

    constexpr Quat4() = default;
    constexpr Quat4(float ix, float iy, float iz, float iw) : x(ix), y(iy), z(iz), w(iw) {}

    [[nodiscard]] Quat4 conjugate() const { return Quat4(-x, -y, -z, w); }
    [[nodiscard]] Quat4 negated() const { return Quat4(-x, -y, -z, -w); }

    [[nodiscard]] Quat4 normalized() const {
        float m2 = x * x + y * y + z * z + w * w;
        if (m2 <= 1e-12f) return Quat4(0.0f, 0.0f, 0.0f, 1.0f);
        float inv = 1.0f / std::sqrt(m2);
        return Quat4(x * inv, y * inv, z * inv, w * inv);
    }

    [[nodiscard]] Vec3 rotate(const Vec3& v) const {
        float tx = 2.0f * (y * v.z - z * v.y);
        float ty = 2.0f * (z * v.x - x * v.z);
        float tz = 2.0f * (x * v.y - y * v.x);
        return Vec3(
            v.x + w * tx + (y * tz - z * ty),
            v.y + w * ty + (z * tx - x * tz),
            v.z + w * tz + (x * ty - y * tx)
        );
    }

    [[nodiscard]] static Quat4 multiply(const Quat4& a, const Quat4& b) {
        return Quat4(
            a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
        );
    }

    [[nodiscard]] static Quat4 slerp(const Quat4& a, Quat4 b, float t) {
        t = std::clamp(t, 0.0f, 1.0f);
        float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
        if (dot < 0.0f) {
            b = b.negated();
            dot = -dot;
        }
        if (dot > 0.9995f) {
            return Quat4(
                a.x + (b.x - a.x) * t,
                a.y + (b.y - a.y) * t,
                a.z + (b.z - a.z) * t,
                a.w + (b.w - a.w) * t
            ).normalized();
        }
        float theta_0 = std::acos(std::clamp(dot, -1.0f, 1.0f));
        float theta = theta_0 * t;
        float sin_theta = std::sin(theta);
        float sin_theta_0 = std::sin(theta_0);
        float s0 = std::cos(theta) - dot * sin_theta / sin_theta_0;
        float s1 = sin_theta / sin_theta_0;
        return Quat4(
            s0 * a.x + s1 * b.x,
            s0 * a.y + s1 * b.y,
            s0 * a.z + s1 * b.z,
            s0 * a.w + s1 * b.w
        ).normalized();
    }
};

struct SkeletalBone {
    std::string name;
    std::string name_lower;
    int32_t parent_index = 0;
    uint32_t flags = 0;
    Vec3 bind_pos{0.0f, 0.0f, 0.0f};
    Quat4 bind_quat{};
    Vec3 comp_ref_pos{0.0f, 0.0f, 0.0f};
    Quat4 comp_ref_quat{};
};

struct SkelMeshSection {
    uint16_t material_index = 0;
    uint16_t chunk_index = 0;
    int32_t base_index = 0;
    uint16_t num_triangles = 0;
    std::string material_name;
};

struct SkinnedVertex {
    Vec3 bind_pos{0.0f, 0.0f, 0.0f};
    Vec3 bind_norm{0.0f, 0.0f, 1.0f};
    Vec3 bind_tangent{1.0f, 0.0f, 0.0f};
    float u = 0.0f;
    float v = 0.0f;
    float un = 0.0f;
    float vn = 0.0f;
    uint8_t bones[4] = {0, 0, 0, 0};
    uint8_t weights[4] = {255, 0, 0, 0};
    uint8_t chunk_index = 0;
    uint8_t mat_type = 0; // 0 = main material, 1 = M_Ammo, 2 = M_M95_Sight
    uint32_t color = 0xFFFFFFFF;
};

struct DXT1Texture {
    std::string name;
    int32_t width = 0;
    int32_t height = 0;
    std::vector<uint8_t> dxt1_blocks;

    [[nodiscard]] bool is_valid() const { return width > 0 && height > 0 && !dxt1_blocks.empty(); }
    [[nodiscard]] Vec3 sample_rgb01(float u, float v) const;
    [[nodiscard]] bool decode_rgba8(std::vector<uint8_t>& out_rgba) const;
};

struct SkeletalMeshAsset {
    std::string name;
    int32_t version = 1;
    Vec3 bounds_origin{0.0f, 0.0f, 0.0f};
    Vec3 bounds_extent{100.0f, 100.0f, 100.0f};
    float bounds_radius = 100.0f;
    Vec3 origin{0.0f, 0.0f, 0.0f};
    Rotator rot_origin{0.0f, 0.0f, 0.0f};
    std::vector<std::string> materials;
    std::vector<SkeletalBone> bones;
    std::unordered_map<std::string, int32_t> bone_name_to_index;
    std::vector<SkelMeshSection> sections;
    std::vector<SkinnedVertex> vertices;
    std::vector<uint16_t> indices;
    DXT1Texture tex_diffuse;
    DXT1Texture tex_specular;
    DXT1Texture tex_normal;
    DXT1Texture tex_mask;

    [[nodiscard]] bool is_valid() const {
        return !bones.empty() && !vertices.empty() && !indices.empty();
    }
};

struct AnimTrack {
    std::vector<Vec3> positions;
    std::vector<Quat4> rotations;
};

struct AnimSequenceAsset {
    std::string name;
    float length = 0.0f;
    int32_t num_frames = 0;
    float rate_scale = 1.0f;
    std::string trans_compression = "ACF_None";
    std::string rot_compression = "ACF_Fixed48NoW";
    std::vector<AnimTrack> tracks;
};

struct AnimSetAsset {
    std::string name;
    std::vector<std::string> track_bone_names;
    std::unordered_map<std::string, int32_t> bone_to_track;
    std::unordered_map<std::string, AnimSequenceAsset> sequences; // lowercase key -> sequence

    [[nodiscard]] const AnimSequenceAsset* find_sequence(const std::string& seq_name) const;
};

struct AnimBlendConfig {
    std::string node_name;
    std::string node_class;
    float blend_in_time = 0.15f;
    float blend_out_time = 0.15f;
};

class AnimSystem {
public:
    AnimSystem() = default;

    // Load all Faith 1P, Weapon, and KrugerSec SWAT skeletal meshes, animation sets, textures, and DefaultAnimation.ini
    bool init_from_game_root(const std::string& game_root);

    [[nodiscard]] bool is_loaded() const { return loaded_; }

    // Evaluate Faith's first-person skeletal viewmodel (SK_UpperBody + SK_LowerBody + equipped weapon + 1P muzzle flash)
    void evaluate_faith_1p(const PlayerTelemetry& telemetry, std::vector<Vertex>& out_triangles) const;

    // Evaluate KrugerSec / CPF Officer / Celeste 3D skeletal mesh + equipped weapon + 3P muzzle flash
    void evaluate_enemy_swat(const EnemyBot& bot, float sim_time, bool reaction_disarm, std::vector<Vertex>& out_triangles) const;

    // Evaluate 3D dropped weapons on the ground and active ballistic tracers / impact sparks
    void evaluate_combat_world_fx(const LevelScene& scene, float sim_time,
                                  std::vector<Vertex>& out_world_tris,
                                  std::vector<Vertex>& out_rv_tris) const;

    // Lookup a loaded weapon skeletal mesh by weapon name (Colt1911, Glock18, BerettaM93R, SteyrTMP, MP5K, G36C, FNSCARL, Remington870, Neostead, FNMinimi, M95)
    [[nodiscard]] const SkeletalMeshAsset* get_weapon_mesh(const std::string& weapon_name) const;

    // Standalone binary parsers exposed for inspection and unit verification
    static bool parse_skeletal_mesh(const UPKPackage& pkg, const FObjectExport& exp, SkeletalMeshAsset& out_mesh);
    static bool parse_anim_set_package(const UPKPackage& pkg, AnimSetAsset& out_anim_set);
    static bool parse_single_anim_sequence(const UPKPackage& pkg, int32_t seq_export_index_1, AnimSetAsset& out_anim_set);
    static bool parse_dxt1_texture(const UPKPackage& pkg, const std::string& tex_name, DXT1Texture& out_tex);

    // Verification & diagnostics summary
    bool verify_all() const;

    [[nodiscard]] const SkeletalMeshAsset& faith_upper_mesh() const { return faith_upper_; }
    [[nodiscard]] const SkeletalMeshAsset& faith_lower_mesh() const { return faith_lower_; }
    [[nodiscard]] const SkeletalMeshAsset& swat_mesh() const { return swat_mesh_; }
    [[nodiscard]] const SkeletalMeshAsset& colt1911_mesh() const { return colt1911_mesh_; }
    [[nodiscard]] const std::unordered_map<std::string, SkeletalMeshAsset>& weapon_meshes() const { return weapon_meshes_; }
    [[nodiscard]] const DXT1Texture& faith_skin_tex() const { return faith_skin_tex_; }
    [[nodiscard]] const DXT1Texture& faith_glove_tex() const { return faith_glove_tex_; }
    [[nodiscard]] const DXT1Texture& faith_lower_tex() const { return faith_lower_tex_; }
    [[nodiscard]] const DXT1Texture& swat_diffuse_tex() const { return swat_diffuse_tex_; }
    [[nodiscard]] const DXT1Texture& swat_specular_tex() const { return swat_specular_tex_; }
    [[nodiscard]] const DXT1Texture& swat_normal_tex() const { return swat_normal_tex_; }
    [[nodiscard]] const DXT1Texture& ammo_diffuse_tex() const { return ammo_diffuse_tex_; }
    [[nodiscard]] const AnimSetAsset& faith_unarmed_anims() const { return faith_unarmed_set_; }
    [[nodiscard]] const AnimSetAsset& faith_common_anims() const { return faith_common_set_; }
    [[nodiscard]] const AnimSetAsset& faith_2h_common_anims() const { return faith_2h_common_set_; }
    [[nodiscard]] const AnimSetAsset& faith_colt_anims() const { return faith_colt_set_; }
    [[nodiscard]] const AnimSetAsset& swat_anims() const { return swat_set_; }
    [[nodiscard]] const AnimSetAsset& swat_2h_anims() const { return swat_2h_set_; }
    [[nodiscard]] const std::vector<AnimBlendConfig>& blend_configs() const { return blend_configs_; }

private:
    bool loaded_ = false;
    std::string game_root_;

    SkeletalMeshAsset faith_upper_;
    SkeletalMeshAsset faith_lower_;
    SkeletalMeshAsset swat_mesh_;
    SkeletalMeshAsset colt1911_mesh_;
    std::unordered_map<std::string, SkeletalMeshAsset> weapon_meshes_;

    DXT1Texture faith_skin_tex_;
    DXT1Texture faith_glove_tex_;
    DXT1Texture faith_lower_tex_;
    DXT1Texture swat_diffuse_tex_;
    DXT1Texture swat_specular_tex_;
    DXT1Texture swat_normal_tex_;
    DXT1Texture ammo_diffuse_tex_;

    AnimSetAsset faith_unarmed_set_;
    AnimSetAsset faith_common_set_;
    AnimSetAsset faith_2h_common_set_;
    AnimSetAsset faith_colt_set_;
    std::unordered_map<std::string, AnimSetAsset> faith_weapon_sets_;
    mutable std::unordered_map<std::string, AnimSetAsset> level_intro_sets_;
    AnimSetAsset swat_set_;
    AnimSetAsset swat_2h_set_;

    std::vector<AnimBlendConfig> blend_configs_;
};

} // namespace me
