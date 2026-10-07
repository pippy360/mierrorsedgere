#pragma once

#include "../math/types.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <cstdint>
#include <utility>

namespace me {

struct FNameEntry {
    std::string name;
    uint64_t flags = 0;
};

struct FObjectImport {
    int32_t index = 0;
    std::string class_package;
    std::string class_name;
    int32_t outer_index = 0;
    std::string object_name;
    int32_t object_number = 0; // FName number (N > 0 means suffix "_{N-1}")
};

struct FObjectExport {
    int32_t index = 0;
    int32_t class_index = 0;
    int32_t super_index = 0;
    int32_t outer_index = 0;
    std::string object_name;
    int32_t object_number = 0;
    int32_t archetype = 0;
    uint64_t object_flags = 0;
    int32_t serial_size = 0;
    int32_t serial_offset = 0;
    std::vector<std::pair<std::string, int32_t>> component_map;
    uint32_t export_flags = 0;
    std::vector<int32_t> gen_net_count;
    std::string package_guid;
    uint32_t package_flags = 0;
};

struct PropertyValue {
    std::string name;
    std::string type;
    int32_t size = 0;
    int32_t array_index = 0;
    std::string struct_name;
    std::string enum_name;

    // Decoded variants
    int32_t int_val = 0;
    float float_val = 0.0f;
    bool bool_val = false;
    std::string str_val;
    Vec3 vec_val{0.0f, 0.0f, 0.0f};
    Rotator rot_val{0.0f, 0.0f, 0.0f};
    std::string obj_ref_name;
    std::string obj_ref_class;
    int32_t obj_ref_index = 0;
    std::vector<uint8_t> raw_bytes;
};

// One LOD0 FStaticMeshElement: a material and the triangles drawn with it.
struct StaticMeshElement {
    std::string material;       // full object path of Element.Material ("" = None -> engine default material)
    uint32_t first_vertex = 0;  // range in StaticMeshAsset::triangles
    uint32_t vertex_count = 0;
};

struct StaticMeshAsset {
    std::string name;
    Vec3 bounds_origin{0.0f, 0.0f, 0.0f};
    Vec3 bounds_extent{100.0f, 100.0f, 100.0f};
    float bounds_radius = 173.2f;
    std::vector<Vertex> triangles; // 3 vertices per triangle in local space, grouped by element
    std::vector<StaticMeshElement> elements;

    // Collision (local space, 3 vertices per triangle). UStaticMeshComponent::LineCheck uses the
    // RB_BodySetup simple geometry when the mesh has a BodySetup and UseSimpleBoxCollision
    // (extent checks) / UseSimpleLineCollision (zero-extent checks) is set, otherwise the
    // per-poly kDOP tree (FkDOPCollisionTriangle over the LOD0 PositionVertexBuffer).
    bool has_body_setup = false;
    bool use_simple_box_collision = true;
    bool use_simple_line_collision = true;
    std::vector<Vec3> simple_collision;   // KConvexElem / KBoxElem / KSphereElem / KSphylElem
    std::vector<Vec3> complex_collision;  // kDOP triangles
};

// Appends the triangles of a UE3 KAggregateGeom struct (RB_BodySetup.AggGeom or
// BrushComponent.BrushAggGeom) in its local space, 3 vertices per triangle.
struct UProperty;
class UPKPackage;
void append_agg_geom_triangles(const UPKPackage& pkg, const UProperty& agg_geom, std::vector<Vec3>& out);

// A sliding door InterpActor driven by a door Matinee group (e.g. "lowerdoors").
struct InterpDoorInfo {
    std::string package;     // source package stem
    std::string actor_name;  // unique export name of the InterpActor
    std::string group;       // InterpGroup.GroupName
    Vec3 open_offset{0.0f, 0.0f, 0.0f};  // world displacement at the end of the matinee (open)
    float open_time = 0.7f;              // PosTrack length
};

class UPKPackage {
public:
    UPKPackage() = default;
    explicit UPKPackage(const std::string& file_path);

    bool load_from_file(const std::string& file_path);
    bool load_from_memory(const uint8_t* raw_data, size_t size, const std::string& name = "");

    [[nodiscard]] bool is_valid() const { return valid_; }
    [[nodiscard]] const std::string& get_file_path() const { return file_path_; }

    [[nodiscard]] const std::vector<std::string>& get_names() const { return names_; }
    [[nodiscard]] const std::vector<FObjectImport>& get_imports() const { return imports_; }
    [[nodiscard]] const std::vector<FObjectExport>& get_exports() const { return exports_; }
    [[nodiscard]] const std::vector<std::string>& get_additional_packages() const { return additional_packages_; }
    [[nodiscard]] const std::vector<uint8_t>& get_data() const { return data_; }

    [[nodiscard]] std::pair<std::string, std::string> resolve_object_index(int32_t index) const;
    [[nodiscard]] std::string get_export_class(const FObjectExport& exp) const;
    [[nodiscard]] std::string get_full_export_path(int32_t exp_zero_idx) const;

    // Property stream parser & header offset detector
    size_t find_property_start(const FObjectExport& exp) const;
    std::unordered_map<std::string, PropertyValue> parse_properties(size_t offset, size_t size, size_t* out_bytes_read = nullptr) const;

    // High-level extraction
    std::vector<LevelActor> extract_actors() const;
    std::vector<SoundClip> extract_audio() const;
    void extract_sound_cues_and_ambients(std::vector<SoundCueDef>& out_cues,
                                         std::vector<AmbientEmitterInfo>& out_ambients) const;
    void extract_level_loaded_sound_cues(std::vector<std::string>& out_cue_names) const;
    bool extract_static_mesh_bounds(int32_t export_index, Vec3& out_origin, Vec3& out_extent, float& out_radius) const;
    void extract_static_meshes(std::unordered_map<std::string, StaticMeshAsset>& out_meshes) const;
    void extract_level_streaming_and_checkpoints(std::vector<LevelCheckpointInfo>& out_checkpoints,
                                                 std::vector<LevelStreamingActionInfo>& out_streaming_actions,
                                                 std::vector<std::string>& out_streaming_packages) const;
    // Level BSP (the PersistentLevel UModel) collision polygons, fan-triangulated in world
    // space (3 vertices per triangle). Non-CSG nodes and PF_NotSolid surfaces are skipped.
    void extract_bsp_collision(std::vector<Vec3>& out_triangles) const;
    // Level BSP render geometry (from PersistentLevel UModel Nodes + Surfs + FModelVertexBuffer),
    // binned by canonical material path and oriented to counter-clockwise front-facing winding.
    void extract_bsp_render_geometry(std::vector<std::pair<std::string, std::vector<Vertex>>>& out_bins,
                                     AABB& inout_bounds) const;
    void extract_elevators(const std::unordered_map<std::string, StaticMeshAsset>& mesh_lib,
                           std::vector<ElevatorInstance>& out_elevators,
                           std::vector<InterpDoorInfo>* out_doors = nullptr) const;
    void extract_reflections(std::vector<SceneCaptureReflectInfo>& out_captures,
                             std::vector<ReflectionVolumeInfo>& out_volumes) const;

    // Built-in LZO1X-1 decompressor
    static bool lzo1x_decompress(const uint8_t* src, size_t src_len, uint8_t* dst, size_t expected_len);

private:
    bool parse_header_and_tables(const uint8_t* file_buf, size_t file_len);

    std::string file_path_;
    bool valid_ = false;
    uint32_t package_flags_ = 0;
    uint32_t compression_flags_ = 0;
    std::vector<uint8_t> data_; // Fully uncompressed linear address space

    std::vector<std::string> names_;
    std::vector<FObjectImport> imports_;
    std::vector<FObjectExport> exports_;
    std::vector<std::vector<int32_t>> depends_;
    std::vector<std::string> additional_packages_;
};

// -----------------------------------------------------------------------------
// Level Loading and Contiguous Rooftop / Parkour Geometry Generation
// -----------------------------------------------------------------------------
bool load_level_scene(const std::string& game_root, const std::string& map_rel_path, LevelScene& out_scene);

// Dynamically stream sub-levels in/out to match a TdCheckpoint's StreamingLevels list
// (or an Elevator's mid-shaft SeqAct_MultiLevelStreaming transition).
bool stream_level_to_checkpoint(const std::string& game_root, LevelScene& scene, int checkpoint_idx);

// Builds the level's render batches and appends the static collision triangles of every actor
// (call CollisionWorld::build() afterwards) from the real extracted UStaticMesh geometry. Real
// static meshes are emitted as per-material MeshSections whose `material` indexes into
// *out_material_paths (full object paths, "" = engine default material) when it is non-null.
// Actors that are moving elevator parts (LevelActor::elevator >= 0) are skipped: they get their
// own buffers and collision. Hidden actors collide but are not drawn.
void build_level_geometry(std::vector<LevelActor>& actors,
                          std::vector<MeshBuffer>& out_meshes,
                          CollisionWorld& out_collision,
                          const std::unordered_map<std::string, StaticMeshAsset>& mesh_lib,
                          std::vector<std::string>* out_material_paths = nullptr,
                          const std::vector<std::pair<std::string, std::vector<Vertex>>>* bsp_render_bins = nullptr,
                          const AABB* bsp_bounds = nullptr);

// Appends one actor's UE3 collision triangles (world space) to `out` with the per-triangle
// channels implied by the actor flags and the mesh's UseSimple*Collision settings.
void append_actor_collision(const LevelActor& actor, int32_t actor_index, const StaticMeshAsset* mesh,
                            CollisionWorld& out);

} // namespace me

