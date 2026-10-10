#pragma once

#include "../math/types.hpp"
#include "../assets/upk_loader.hpp"
#include "../physics/body_shapes.hpp"
#include <array>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <cstdint>
#include <functional>

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

// TdPlayerPawn.CalcCamera puts the first-person camera on Mesh1p's EyeJoint and adds the camera
// animation (native TdPawn.GetCameraAnimation) to the view rotation. The 1P bone frame is raw +X left,
// -Y up, +Z forward, so the bone's FMatrix::Rotator is swizzled into the view's:
// Pitch += -Rot.Roll, Yaw += Rot.Pitch, Roll += -Rot.Yaw.
struct CameraAnimation {
    float weight = 0.0f;              // blend weight of the animated camera (0 = none playing)
    Vec3 eye{0.0f, 0.0f, 0.0f};       // the animation's EyeJoint above the feet: x forward, y right, z up
    float pitch_deg = 0.0f;           // added to the view rotation (already blended by weight)
    float yaw_deg = 0.0f;
    float roll_deg = 0.0f;
};

class AnimSystem {
public:
    AnimSystem() = default;

    // Load all Faith 1P, Weapon, and KrugerSec SWAT skeletal meshes, animation sets, textures, and DefaultAnimation.ini
    bool init_from_game_root(const std::string& game_root);

    [[nodiscard]] bool is_loaded() const { return loaded_; }

    // Evaluate Faith's first-person skeletal viewmodel (SK_UpperBody + SK_LowerBody + equipped weapon + 1P muzzle flash)
    void evaluate_faith_1p(const PlayerTelemetry& telemetry, std::vector<Vertex>& out_triangles) const;

    // The camera animation of the move `telemetry` is in, at the time its viewmodel animation is at.
    // Only TdMove_SkillRoll's fallinglandroll (a full forward somersault of the view) so far; other
    // moves return weight 0.
    [[nodiscard]] CameraAnimation camera_animation(const PlayerTelemetry& telemetry) const;

    // The first-person camera for `telemetry`. In play it is the first-person tree's: at its EyeJoint
    // (bobbing with the run, dipping on a landing, craning forward looking down), looking along the
    // view rotation turned by what the animation does to the camera bone. In a level intro, the death
    // fall, or without the tree: the eyes eye_height above the feet, with camera_animation().
    void player_camera(const PlayerTelemetry& telemetry, Vec3& out_pos, Rotator& out_rot) const;
    // A line check through the level (no extent), for the first-person mesh's controls that put
    // her feet on the floor: true with where the line from `from` to `to` meets something.
    using WorldTrace = std::function<bool(const Vec3& from, const Vec3& to, Vec3& hit, Vec3& normal)>;
    void set_world_trace(WorldTrace trace) { world_trace_ = std::move(trace); }

    enum EnemyArchetypeId : uint32_t {
        EnemyArch_SWAT     = 0,
        EnemyArch_Patrol   = 1,
        EnemyArch_Support  = 2,
        EnemyArch_Riot     = 3,
        EnemyArch_Pursuit  = 4,
        EnemyArch_Celeste  = 5,
        EnemyArch_Kate     = 6,
        EnemyArch_Jacknife = 7,
        EnemyArch_Ropeburn = 8,
        EnemyArch_Miller   = 9,
        EnemyArch_Kreeg    = 10,
        EnemyArch_Count    = 11
    };

    struct EnemyCharacterModel {
        SkeletalMeshAsset mesh;
        DXT1Texture tex_diffuse;
        DXT1Texture tex_specular;
        DXT1Texture tex_normal;
        std::vector<size_t> chunk_vert_counts;
    };

    [[nodiscard]] EnemyArchetypeId resolve_enemy_archetype(const std::string& archetype_name) const;
    [[nodiscard]] const EnemyCharacterModel& enemy_character_model(uint32_t arch_id) const {
        return enemy_models_[arch_id < EnemyArch_Count ? arch_id : 0];
    }

    // Evaluate KrugerSec / CPF Officer / Celeste 3D skeletal mesh + equipped weapon + 3P muzzle flash
    void evaluate_enemy_swat(const EnemyBot& bot, float sim_time, bool reaction_disarm, std::vector<Vertex>& out_triangles) const;

    // Indexed form of evaluate_enemy_swat() (used by the renderer): writes the posed officer's unique vertices
    // (skinned body, then the attached weapon, then the muzzle-flash corners) to out_vertices, which must have
    // room for enemy_swat_max_vertices() entries. Gathering them through the first index_count entries of
    // enemy_swat_index_lists()[index_list] gives exactly the triangle list evaluate_enemy_swat() returns, but
    // each shared vertex is posed only once, and the caller can bound the mesh before building the list.
    struct EnemySwatDraw {
        size_t vertex_count = 0;  // vertices written to out_vertices
        size_t index_list = 0;    // enemy_swat_index_lists() entry to draw
        size_t index_count = 0;   // leading indices of that list to draw (0 = nothing to draw)
        uint32_t archetype_id = 0; // EnemyArchetypeId (0..EnemyArch_Count-1) for GPU texture binding
    };
    EnemySwatDraw evaluate_enemy_swat_indexed(const EnemyBot& bot, float sim_time, bool reaction_disarm,
                                              Vertex* out_vertices) const;
    [[nodiscard]] size_t enemy_swat_max_vertices() const { return enemy_swat_max_vertices_; }
    // Static triangle-list indices for evaluate_enemy_swat_indexed(): rebuilt by init_from_game_root().
    [[nodiscard]] const std::vector<std::vector<uint32_t>>& enemy_swat_index_lists() const { return enemy_swat_index_lists_; }

    // The bodies a bullet meets in a bot (physics/body_shapes.hpp): the shapes of
    // CH_TKY_Cop_SWAT.Male3p_Physics, the physics asset of every bot's third-person mesh, each placed
    // in the world by its bone as evaluate_enemy_swat_indexed() poses the skeleton for the same bot,
    // sim_time and reaction_disarm (UPhysicsAsset::LineCheck: SkelComp->GetBoneMatrix of the body's
    // BoneName). It draws nothing and keeps nothing: it can be asked at any moment, a render frame
    // or not. False, and out.valid false, without the character assets or for a skeleton with none
    // of the bodies' bones.
    bool pose_enemy_bodies(const EnemyBot& bot, float sim_time, bool reaction_disarm, EnemyBodySet& out) const;

    // A body of the bots' physics asset as it is stored: an RB_BodySetup.
    struct EnemyBodyTemplate {
        std::string bone;      // BoneName
        std::string material;  // PhysMaterial, by its path
        ECharacterSurface surface = ECharacterSurface::Body;
        std::vector<AggShape> shapes;  // AggGeom, in the bone's space
    };
    [[nodiscard]] const std::vector<EnemyBodyTemplate>& enemy_bodies() const { return enemy_bodies_; }
    // ME_SHOW_BODIES=1: evaluate_combat_world_fx() draws, as lines, the bodies of every enemy a
    // bullet can meet who is near the view: the head's material in the runner-vision red, the
    // body's green.
    [[nodiscard]] bool shows_enemy_bodies() const { return show_enemy_bodies_; }

    // Evaluate 3D dropped weapons on the ground and active ballistic tracers / impact sparks
    // (and, asked for, the enemies' bodies: shows_enemy_bodies(), as seen from view_pos)
    void evaluate_combat_world_fx(const LevelScene& scene, const Vec3& view_pos, float sim_time, bool reaction_disarm,
                                  std::vector<Vertex>& out_world_tris,
                                  std::vector<Vertex>& out_rv_tris) const;

    // Lookup a loaded weapon skeletal mesh by weapon name (Colt1911, Glock18, BerettaM93R, SteyrTMP, MP5K, G36C, FNSCARL, Remington870, Neostead, FNMinimi, M95)
    [[nodiscard]] const SkeletalMeshAsset* get_weapon_mesh(const std::string& weapon_name) const;

    // Standalone binary parsers exposed for inspection and unit verification
    static bool parse_skeletal_mesh(const UPKPackage& pkg, const FObjectExport& exp, SkeletalMeshAsset& out_mesh);

    // The first-person view through a canned full-body animation (a level intro). For each frame of
    // `seq`: the EyeJoint's position, view direction and up vector and the root bone's position, in
    // the animation's own space, from the pose of Faith's first-person skeleton (SK_UpperBody).
    struct CannedCameraFrame {
        Vec3 eye_pos{0.0f, 0.0f, 0.0f};
        Vec3 forward{0.0f, 0.0f, 1.0f};
        Vec3 up{0.0f, -1.0f, 0.0f};
        Vec3 root_pos{0.0f, 0.0f, 0.0f};
    };
    // `pawn_root`, when given, is the root track the pawn itself follows (the full-body version of
    // the animation, which drives the pawn's own mesh): the first-person pose is carried on it in
    // place of its own root.
    static bool bake_canned_camera(const std::string& game_root, const AnimSetAsset& set, const AnimSequenceAsset& seq,
                                   std::vector<CannedCameraFrame>& out_frames, const AnimTrack* pawn_root = nullptr);
    // How many of `set`'s tracks drive a bone of that skeleton, and whether EyeJoint is one of them.
    static int count_first_person_tracks(const std::string& game_root, const AnimSetAsset& set, bool* has_eye_joint);
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
    [[nodiscard]] const DXT1Texture& enemy_diffuse_tex(EnemyArchetypeId id) const {
        return enemy_models_[static_cast<size_t>(id) % EnemyArch_Count].tex_diffuse;
    }
    [[nodiscard]] const DXT1Texture& enemy_specular_tex(EnemyArchetypeId id) const {
        return enemy_models_[static_cast<size_t>(id) % EnemyArch_Count].tex_specular;
    }
    [[nodiscard]] const DXT1Texture& enemy_normal_tex(EnemyArchetypeId id) const {
        return enemy_models_[static_cast<size_t>(id) % EnemyArch_Count].tex_normal;
    }
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
    // Faith's first-person animation tree, the director that drives it from the pawn, and its pose
    // (fp_anim / fp_director / fp_pose). Ticked once per simulation time from evaluate_faith_1p.
    struct FirstPerson;
    mutable std::shared_ptr<FirstPerson> fp_;
    WorldTrace world_trace_;
    // What of the tree is used this frame: its eye for the world camera, its whole body for the
    // viewmodel, or only its legs under arms that are still posed by hand (a weapon in hand).
    struct FirstPersonUse {
        bool camera = false;
        bool body = false;
        bool legs = false;
    };
    FirstPersonUse tick_first_person(const PlayerTelemetry& telemetry) const;

    SkeletalMeshAsset faith_upper_;
    SkeletalMeshAsset faith_lower_;
    SkeletalMeshAsset swat_mesh_;
    std::array<EnemyCharacterModel, EnemyArch_Count> enemy_models_{};
    SkeletalMeshAsset heli_mesh_;
    DXT1Texture heli_diffuse_tex_;
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
    mutable std::mutex level_intro_mutex_;
    mutable std::unordered_map<std::string, AnimSetAsset> level_intro_sets_;
    AnimSetAsset swat_set_;
    AnimSetAsset swat_2h_set_;
    AnimSetAsset celeste_set_;

    std::vector<AnimBlendConfig> blend_configs_;

    // The bones of a bot as he is at sim_time: his archetype, its skeleton, and every bone's place
    // and rotation in the mesh's own space. The one pose both what is drawn
    // (evaluate_enemy_swat_indexed) and what a bullet meets (pose_enemy_bodies) are made from.
    bool pose_enemy_skeleton(const EnemyBot& bot, float sim_time, bool reaction_disarm, EnemyArchetypeId& out_arch,
                             const SkeletalMeshAsset*& out_mesh, std::vector<Vec3>& out_comp_pos,
                             std::vector<Quat4>& out_comp_quat) const;
    // The bots' physics asset (init_from_game_root): its bodies, and for each archetype the bone of
    // its skeleton that carries each body (-1: that skeleton has no bone of the name, and the body
    // is left out, as USkeletalMeshComponent::MatchRefBone leaves it out).
    void load_enemy_bodies(const UPKPackage& pkg);
    void bind_enemy_bodies();
    std::vector<EnemyBodyTemplate> enemy_bodies_;
    std::array<std::vector<int32_t>, EnemyArch_Count> enemy_body_bones_{};
    bool show_enemy_bodies_ = false;

    // evaluate_enemy_swat_indexed() support, built by build_enemy_swat_index_lists() once the meshes are loaded.
    void build_enemy_swat_index_lists();
    std::vector<std::vector<uint32_t>> enemy_swat_index_lists_;
    std::array<size_t, EnemyArch_Count> enemy_body_only_index_list_{};
    std::array<std::unordered_map<const SkeletalMeshAsset*, size_t>, EnemyArch_Count> enemy_weapon_index_lists_{};
    size_t enemy_swat_max_vertices_ = 0;
};

} // namespace me
