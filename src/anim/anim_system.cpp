#include "anim_system.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cctype>

namespace me {

namespace {

template <typename T>
T read_le(const uint8_t*& ptr, const uint8_t* end) {
    if (ptr + sizeof(T) > end) {
        ptr = end;
        return T{};
    }
    T val{};
    std::memcpy(&val, ptr, sizeof(T));
    ptr += sizeof(T);
    return val;
}

std::string to_lower_str(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

uint32_t pack_rgba8(float r, float g, float b, float a = 1.0f) {
    uint8_t R = static_cast<uint8_t>(std::clamp(r * 255.0f, 0.0f, 255.0f));
    uint8_t G = static_cast<uint8_t>(std::clamp(g * 255.0f, 0.0f, 255.0f));
    uint8_t B = static_cast<uint8_t>(std::clamp(b * 255.0f, 0.0f, 255.0f));
    uint8_t A = static_cast<uint8_t>(std::clamp(a * 255.0f, 0.0f, 255.0f));
    return (static_cast<uint32_t>(A) << 24) |
           (static_cast<uint32_t>(B) << 16) |
           (static_cast<uint32_t>(G) << 8)  |
           static_cast<uint32_t>(R);
}

std::string read_fname_str(const UPKPackage& pkg, int32_t name_idx, int32_t name_num) {
    const auto& names = pkg.get_names();
    std::string s = (name_idx >= 0 && static_cast<size_t>(name_idx) < names.size())
                        ? names[name_idx]
                        : std::to_string(name_idx);
    if (name_num > 0) {
        s += "_" + std::to_string(name_num - 1);
    }
    return s;
}

// Compute forward kinematics for a skeleton given local bone positions and rotations
void compute_skeleton_fk(const std::vector<SkeletalBone>& bones,
                         const std::vector<Vec3>& local_pos,
                         const std::vector<Quat4>& local_quat,
                         std::vector<Vec3>& out_comp_pos,
                         std::vector<Quat4>& out_comp_quat) {
    const size_t n = bones.size();
    out_comp_pos.resize(n);
    out_comp_quat.resize(n);
    for (size_t i = 0; i < n; ++i) {
        if (i == 0 || bones[i].parent_index < 0 || static_cast<size_t>(bones[i].parent_index) >= i) {
            out_comp_pos[i] = local_pos[i];
            out_comp_quat[i] = local_quat[i];
        } else {
            size_t p = static_cast<size_t>(bones[i].parent_index);
            Vec3 rp = out_comp_quat[p].rotate(local_pos[i]);
            out_comp_pos[i] = out_comp_pos[p] + rp;
            out_comp_quat[i] = Quat4::multiply(out_comp_quat[p], local_quat[i]).normalized();
        }
    }
}

// Sample a single AnimSequence at normalized time [0, 1] into local bone transforms
void sample_sequence_pose(const SkeletalMeshAsset& mesh,
                          const AnimSetAsset& anim_set,
                          const AnimSequenceAsset* seq,
                          float norm_time,
                          std::vector<Vec3>& out_local_pos,
                          std::vector<Quat4>& out_local_quat) {
    const size_t num_bones = mesh.bones.size();
    out_local_pos.resize(num_bones);
    out_local_quat.resize(num_bones);

    // Initialize from RefSkeleton bind pose
    for (size_t i = 0; i < num_bones; ++i) {
        out_local_pos[i] = mesh.bones[i].bind_pos;
        out_local_quat[i] = mesh.bones[i].bind_quat;
    }

    if (!seq || seq->tracks.empty()) return;

    float t_clamped = norm_time - std::floor(norm_time);
    if (t_clamped < 0.0f) t_clamped += 1.0f;

    for (size_t b = 0; b < num_bones; ++b) {
        auto it = anim_set.bone_to_track.find(to_lower_str(mesh.bones[b].name));
        if (it == anim_set.bone_to_track.end()) continue;
        int32_t track_idx = it->second;
        if (track_idx < 0 || static_cast<size_t>(track_idx) >= seq->tracks.size()) continue;

        const AnimTrack& tr = seq->tracks[track_idx];

        // 1. Translation interpolation
        if (tr.positions.size() == 1) {
            out_local_pos[b] = tr.positions[0];
        } else if (tr.positions.size() > 1) {
            float f_idx = t_clamped * static_cast<float>(tr.positions.size() - 1);
            size_t i0 = static_cast<size_t>(f_idx);
            size_t i1 = std::min(i0 + 1, tr.positions.size() - 1);
            float frac = f_idx - static_cast<float>(i0);
            out_local_pos[b] = tr.positions[i0] + (tr.positions[i1] - tr.positions[i0]) * frac;
        }

        // 2. Rotation SLERP interpolation
        if (tr.rotations.size() == 1) {
            out_local_quat[b] = tr.rotations[0];
        } else if (tr.rotations.size() > 1) {
            float f_idx = t_clamped * static_cast<float>(tr.rotations.size() - 1);
            size_t i0 = static_cast<size_t>(f_idx);
            size_t i1 = std::min(i0 + 1, tr.rotations.size() - 1);
            float frac = f_idx - static_cast<float>(i0);
            out_local_quat[b] = Quat4::slerp(tr.rotations[i0], tr.rotations[i1], frac);
        }
    }
}

// Blend two local bone poses using LERP + shortest-path SLERP
void blend_local_poses(const std::vector<Vec3>& pos_a,
                       const std::vector<Quat4>& quat_a,
                       const std::vector<Vec3>& pos_b,
                       const std::vector<Quat4>& quat_b,
                       float alpha,
                       std::vector<Vec3>& out_pos,
                       std::vector<Quat4>& out_quat) {
    const size_t n = std::min(pos_a.size(), pos_b.size());
    out_pos.resize(n);
    out_quat.resize(n);
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    for (size_t i = 0; i < n; ++i) {
        out_pos[i] = pos_a[i] + (pos_b[i] - pos_a[i]) * alpha;
        out_quat[i] = Quat4::slerp(quat_a[i], quat_b[i], alpha);
    }
}

// Compute per-bone relative Linear Blend Skinning transforms (RefPose -> AnimPose)
void compute_skin_deltas(const SkeletalMeshAsset& mesh,
                         const std::vector<Vec3>& anim_comp_pos,
                         const std::vector<Quat4>& anim_comp_quat,
                         std::vector<Vec3>& out_delta_pos,
                         std::vector<Quat4>& out_delta_quat) {
    const size_t n = mesh.bones.size();
    out_delta_pos.resize(n);
    out_delta_quat.resize(n);
    for (size_t b = 0; b < n; ++b) {
        Quat4 dq = Quat4::multiply(anim_comp_quat[b], mesh.bones[b].comp_ref_quat.conjugate()).normalized();
        Vec3 dp = anim_comp_pos[b] - dq.rotate(mesh.bones[b].comp_ref_pos);
        out_delta_quat[b] = dq;
        out_delta_pos[b] = dp;
    }
}

} // namespace

// -----------------------------------------------------------------------------
// DXT1 Texture Sampling
// -----------------------------------------------------------------------------
Vec3 DXT1Texture::sample_rgb01(float u, float v) const {
    if (!is_valid()) return Vec3(0.8f, 0.8f, 0.8f);
    float uf = u - std::floor(u);
    if (uf < 0.0f) uf += 1.0f;
    float vf = v - std::floor(v);
    if (vf < 0.0f) vf += 1.0f;

    int32_t px = std::clamp(static_cast<int32_t>(uf * static_cast<float>(width)), 0, width - 1);
    int32_t py = std::clamp(static_cast<int32_t>(vf * static_cast<float>(height)), 0, height - 1);
    int32_t bx = px / 4;
    int32_t by = py / 4;
    int32_t bw = std::max(1, width / 4);
    size_t blk_off = static_cast<size_t>(by * bw + bx) * 8;
    if (blk_off + 8 > dxt1_blocks.size()) return Vec3(0.8f, 0.8f, 0.8f);

    uint16_t c0 = 0, c1 = 0;
    uint32_t bits = 0;
    std::memcpy(&c0, dxt1_blocks.data() + blk_off, 2);
    std::memcpy(&c1, dxt1_blocks.data() + blk_off + 2, 2);
    std::memcpy(&bits, dxt1_blocks.data() + blk_off + 4, 4);

    auto unpack_565 = [](uint16_t c) -> Vec3 {
        float r = static_cast<float>((c >> 11) & 31) * (1.0f / 31.0f);
        float g = static_cast<float>((c >> 5) & 63) * (1.0f / 63.0f);
        float b = static_cast<float>(c & 31) * (1.0f / 31.0f);
        return Vec3(r, g, b);
    };

    Vec3 col0 = unpack_565(c0);
    Vec3 col1 = unpack_565(c1);
    Vec3 pal[4];
    pal[0] = col0;
    pal[1] = col1;
    if (c0 > c1) {
        pal[2] = (col0 * 2.0f + col1) * (1.0f / 3.0f);
        pal[3] = (col0 + col1 * 2.0f) * (1.0f / 3.0f);
    } else {
        pal[2] = (col0 + col1) * 0.5f;
        pal[3] = Vec3(0.0f, 0.0f, 0.0f);
    }

    int32_t bit_shift = 2 * ((py & 3) * 4 + (px & 3));
    uint32_t code = (bits >> bit_shift) & 3u;
    return pal[code];
}

const AnimSequenceAsset* AnimSetAsset::find_sequence(const std::string& seq_name) const {
    auto it = sequences.find(to_lower_str(seq_name));
    if (it != sequences.end()) return &it->second;
    return nullptr;
}

// -----------------------------------------------------------------------------
// Cooked Texture2D PF_DXT1 Mip-0 Parser (with LZOChunked Decompression)
// -----------------------------------------------------------------------------
bool AnimSystem::parse_dxt1_texture(const UPKPackage& pkg, const std::string& tex_name, DXT1Texture& out_tex) {
    out_tex = DXT1Texture{};
    out_tex.name = tex_name;

    const auto& data = pkg.get_data();
    for (const auto& exp : pkg.get_exports()) {
        if (pkg.get_export_class(exp) != "Texture2D" || exp.object_name != tex_name) continue;

        size_t prop_start = pkg.find_property_start(exp);
        size_t bytes_read = 0;
        pkg.parse_properties(prop_start, exp.serial_size, &bytes_read);
        size_t tail_off = prop_start + bytes_read;
        size_t exp_end = static_cast<size_t>(exp.serial_offset) + static_cast<size_t>(exp.serial_size);
        if (tail_off + 20 > exp_end || exp_end > data.size()) return false;

        const uint8_t* ptr = data.data() + tail_off;
        const uint8_t* end = data.data() + exp_end;

        // Skip FByteBulkData SourceArt (16 bytes: flags, elem_cnt, size_on_disk, offset)
        int32_t sa_flags = read_le<int32_t>(ptr, end);
        read_le<int32_t>(ptr, end);
        int32_t sa_disk = read_le<int32_t>(ptr, end);
        read_le<int32_t>(ptr, end);
        if ((sa_flags & 0x20) == 0 && sa_disk > 0 && ptr + sa_disk <= end) {
            ptr += sa_disk;
        }

        int32_t mip_count = read_le<int32_t>(ptr, end);
        if (mip_count <= 0 || mip_count > 16) return false;

        // Read Mip[0]
        uint32_t bulk_flags = read_le<uint32_t>(ptr, end);
        int32_t elem_count = read_le<int32_t>(ptr, end);
        int32_t disk_size = read_le<int32_t>(ptr, end);
        read_le<int32_t>(ptr, end); // offset in file

        if ((bulk_flags & 0x20) != 0 || elem_count <= 0 || disk_size <= 0 || ptr + disk_size + 8 > end) {
            return false;
        }

        const uint8_t* mip_payload = ptr;
        ptr += disk_size;
        int32_t size_x = read_le<int32_t>(ptr, end);
        int32_t size_y = read_le<int32_t>(ptr, end);

        if ((bulk_flags & 0x10) != 0) {
            // LZO-compressed bulk data block
            const uint8_t* lzo_ptr = mip_payload;
            const uint8_t* lzo_end = mip_payload + disk_size;
            uint32_t magic = read_le<uint32_t>(lzo_ptr, lzo_end);
            if (magic != 0x9E2A83C1u) return false;
            uint32_t block_size = read_le<uint32_t>(lzo_ptr, lzo_end);
            read_le<uint32_t>(lzo_ptr, lzo_end); // comp_total
            uint32_t uncomp_total = read_le<uint32_t>(lzo_ptr, lzo_end);
            if (block_size == 0 || uncomp_total == 0 || uncomp_total > 32 * 1024 * 1024) return false;

            uint32_t num_chunks = (uncomp_total + block_size - 1) / block_size;
            std::vector<std::pair<uint32_t, uint32_t>> chunks(num_chunks);
            for (uint32_t c = 0; c < num_chunks; ++c) {
                uint32_t cs = read_le<uint32_t>(lzo_ptr, lzo_end);
                uint32_t us = read_le<uint32_t>(lzo_ptr, lzo_end);
                chunks[c] = {cs, us};
            }

            out_tex.dxt1_blocks.resize(uncomp_total);
            size_t dst_off = 0;
            for (const auto& [cs, us] : chunks) {
                if (lzo_ptr + cs > lzo_end || dst_off + us > out_tex.dxt1_blocks.size()) return false;
                if (cs == us) {
                    std::memcpy(out_tex.dxt1_blocks.data() + dst_off, lzo_ptr, us);
                } else {
                    if (!UPKPackage::lzo1x_decompress(lzo_ptr, cs, out_tex.dxt1_blocks.data() + dst_off, us)) {
                        return false;
                    }
                }
                lzo_ptr += cs;
                dst_off += us;
            }
        } else {
            out_tex.dxt1_blocks.assign(mip_payload, mip_payload + disk_size);
        }

        out_tex.width = size_x;
        out_tex.height = size_y;
        return out_tex.is_valid();
    }
    return false;
}

// -----------------------------------------------------------------------------
// USkeletalMesh Binary Parser (UE3 v536)
// -----------------------------------------------------------------------------
bool AnimSystem::parse_skeletal_mesh(const UPKPackage& pkg, const FObjectExport& exp, SkeletalMeshAsset& out_mesh) {
    out_mesh = SkeletalMeshAsset{};
    out_mesh.name = exp.object_name;

    const auto& data = pkg.get_data();
    size_t prop_start = pkg.find_property_start(exp);
    size_t bytes_read = 0;
    pkg.parse_properties(prop_start, exp.serial_size, &bytes_read);

    size_t tail_off = prop_start + bytes_read;
    size_t exp_end = static_cast<size_t>(exp.serial_offset) + static_cast<size_t>(exp.serial_size);
    if (tail_off + 64 > exp_end || exp_end > data.size()) return false;

    const uint8_t* ptr = data.data() + tail_off;
    const uint8_t* end = data.data() + exp_end;

    // 1. Version & FBoxSphereBounds (32B)
    out_mesh.version = read_le<int32_t>(ptr, end);
    out_mesh.bounds_origin.x = read_le<float>(ptr, end);
    out_mesh.bounds_origin.y = read_le<float>(ptr, end);
    out_mesh.bounds_origin.z = read_le<float>(ptr, end);
    out_mesh.bounds_extent.x = read_le<float>(ptr, end);
    out_mesh.bounds_extent.y = read_le<float>(ptr, end);
    out_mesh.bounds_extent.z = read_le<float>(ptr, end);
    out_mesh.bounds_radius = read_le<float>(ptr, end);

    // 2. Materials (TArray<UMaterialInterface*>)
    int32_t mat_count = read_le<int32_t>(ptr, end);
    if (mat_count < 0 || mat_count > 256 || ptr + mat_count * 4 > end) return false;
    for (int32_t i = 0; i < mat_count; ++i) {
        int32_t mat_ref = read_le<int32_t>(ptr, end);
        out_mesh.materials.push_back(pkg.resolve_object_index(mat_ref).first);
    }

    // 3. Origin (FVector 12B) & RotOrigin (FRotator 12B)
    out_mesh.origin.x = read_le<float>(ptr, end);
    out_mesh.origin.y = read_le<float>(ptr, end);
    out_mesh.origin.z = read_le<float>(ptr, end);
    int32_t rot_p = read_le<int32_t>(ptr, end);
    int32_t rot_y = read_le<int32_t>(ptr, end);
    int32_t rot_r = read_le<int32_t>(ptr, end);
    out_mesh.rot_origin = Rotator(static_cast<float>(rot_p), static_cast<float>(rot_y), static_cast<float>(rot_r));

    // 4. RefSkeleton (TArray<FMeshBone>: 52 bytes per bone)
    int32_t bone_count = read_le<int32_t>(ptr, end);
    if (bone_count <= 0 || bone_count > 512 || ptr + bone_count * 52 > end) return false;
    out_mesh.bones.resize(bone_count);
    for (int32_t i = 0; i < bone_count; ++i) {
        int32_t n_idx = read_le<int32_t>(ptr, end);
        int32_t n_num = read_le<int32_t>(ptr, end);
        uint32_t flags = read_le<uint32_t>(ptr, end);
        float qx = read_le<float>(ptr, end);
        float qy = read_le<float>(ptr, end);
        float qz = read_le<float>(ptr, end);
        float qw = read_le<float>(ptr, end);
        float px = read_le<float>(ptr, end);
        float py = read_le<float>(ptr, end);
        float pz = read_le<float>(ptr, end);
        read_le<int32_t>(ptr, end); // num_children
        int32_t parent_idx = read_le<int32_t>(ptr, end);
        read_le<int32_t>(ptr, end); // bone_color

        SkeletalBone& bone = out_mesh.bones[i];
        bone.name = read_fname_str(pkg, n_idx, n_num);
        bone.flags = flags;
        bone.parent_index = (i == 0) ? -1 : parent_idx;
        bone.bind_pos = Vec3(px, py, pz);
        bone.bind_quat = Quat4(qx, qy, qz, qw).normalized();
        out_mesh.bone_name_to_index[to_lower_str(bone.name)] = i;
    }

    // Precompute bind-pose component-space bone transforms
    for (int32_t i = 0; i < bone_count; ++i) {
        SkeletalBone& b = out_mesh.bones[i];
        if (i == 0 || b.parent_index < 0 || b.parent_index >= i) {
            b.comp_ref_pos = b.bind_pos;
            b.comp_ref_quat = b.bind_quat;
        } else {
            const SkeletalBone& parent = out_mesh.bones[b.parent_index];
            b.comp_ref_pos = parent.comp_ref_pos + parent.comp_ref_quat.rotate(b.bind_pos);
            b.comp_ref_quat = Quat4::multiply(parent.comp_ref_quat, b.bind_quat).normalized();
        }
    }

    // 5. SkeletalDepth & LODModels Count
    read_le<int32_t>(ptr, end); // skeletal_depth
    int32_t lod_count = read_le<int32_t>(ptr, end);
    if (lod_count <= 0 || lod_count > 8) return false;

    // 6. Parse LODModels[0]
    int32_t sec_count = read_le<int32_t>(ptr, end);
    if (sec_count <= 0 || sec_count > 64 || ptr + sec_count * 10 > end) return false;
    out_mesh.sections.resize(sec_count);
    for (int32_t s = 0; s < sec_count; ++s) {
        SkelMeshSection& sec = out_mesh.sections[s];
        sec.material_index = read_le<uint16_t>(ptr, end);
        sec.chunk_index = read_le<uint16_t>(ptr, end);
        sec.base_index = read_le<int32_t>(ptr, end);
        sec.num_triangles = read_le<uint16_t>(ptr, end);
        if (sec.material_index < out_mesh.materials.size()) {
            sec.material_name = out_mesh.materials[sec.material_index];
        }
    }

    // IndexBuffer (FMultiSizeIndexContainer)
    int32_t idx_type_size = read_le<int32_t>(ptr, end);
    int32_t idx_count = read_le<int32_t>(ptr, end);
    if ((idx_type_size != 2 && idx_type_size != 4) || idx_count <= 0 || idx_count > 500000 ||
        ptr + static_cast<size_t>(idx_type_size) * idx_count > end) {
        return false;
    }
    out_mesh.indices.resize(idx_count);
    for (int32_t i = 0; i < idx_count; ++i) {
        if (idx_type_size == 2) {
            out_mesh.indices[i] = read_le<uint16_t>(ptr, end);
        } else {
            out_mesh.indices[i] = static_cast<uint16_t>(read_le<uint32_t>(ptr, end));
        }
    }

    // ShadowIndices (TArray<WORD>)
    int32_t sh_idx_cnt = read_le<int32_t>(ptr, end);
    if (sh_idx_cnt < 0 || ptr + static_cast<size_t>(sh_idx_cnt) * 2 > end) return false;
    ptr += static_cast<size_t>(sh_idx_cnt) * 2;

    // UsedBones (TArray<WORD>)
    int32_t used_bones_cnt = read_le<int32_t>(ptr, end);
    if (used_bones_cnt < 0 || ptr + static_cast<size_t>(used_bones_cnt) * 2 > end) return false;
    ptr += static_cast<size_t>(used_bones_cnt) * 2;

    // ShadowTriangleDoubleSided (TArray<BYTE>)
    int32_t sh_tri_cnt = read_le<int32_t>(ptr, end);
    if (sh_tri_cnt < 0 || ptr + static_cast<size_t>(sh_tri_cnt) > end) return false;
    ptr += static_cast<size_t>(sh_tri_cnt);

    // Chunks (TArray<FSkelMeshChunk>)
    int32_t chunk_count = read_le<int32_t>(ptr, end);
    if (chunk_count <= 0 || chunk_count > 64) return false;

    auto unpack_normal = [](const uint8_t* p) -> Vec3 {
        float nx = (static_cast<float>(p[0]) - 127.5f) * (1.0f / 127.5f);
        float ny = (static_cast<float>(p[1]) - 127.5f) * (1.0f / 127.5f);
        float nz = (static_cast<float>(p[2]) - 127.5f) * (1.0f / 127.5f);
        return Vec3(nx, ny, nz).normalized();
    };

    for (int32_t c = 0; c < chunk_count; ++c) {
        uint32_t base_vertex_index = read_le<uint32_t>(ptr, end);
        (void)base_vertex_index;

        int32_t rigid_cnt = read_le<int32_t>(ptr, end);
        if (rigid_cnt < 0 || rigid_cnt > 200000 || ptr + static_cast<size_t>(rigid_cnt) * 49 > end) return false;
        const uint8_t* rigid_ptr = ptr;
        ptr += static_cast<size_t>(rigid_cnt) * 49;

        int32_t soft_cnt = read_le<int32_t>(ptr, end);
        if (soft_cnt < 0 || soft_cnt > 200000 || ptr + static_cast<size_t>(soft_cnt) * 56 > end) return false;
        const uint8_t* soft_ptr = ptr;
        ptr += static_cast<size_t>(soft_cnt) * 56;

        int32_t bmap_cnt = read_le<int32_t>(ptr, end);
        if (bmap_cnt < 0 || bmap_cnt > 512 || ptr + static_cast<size_t>(bmap_cnt) * 2 + 12 > end) return false;
        std::vector<uint16_t> bone_map(bmap_cnt);
        for (int32_t b = 0; b < bmap_cnt; ++b) {
            bone_map[b] = read_le<uint16_t>(ptr, end);
        }

        // Skip NumRigidVertices, NumSoftVertices, MaxBoneInfluences (12B)
        ptr += 12;

        // Decode RigidVertices (49B each)
        for (int32_t v = 0; v < rigid_cnt; ++v) {
            const uint8_t* vp = rigid_ptr + static_cast<size_t>(v) * 49;
            SkinnedVertex sv{};
            std::memcpy(&sv.bind_pos.x, vp + 0, 4);
            std::memcpy(&sv.bind_pos.y, vp + 4, 4);
            std::memcpy(&sv.bind_pos.z, vp + 8, 4);
            sv.bind_tangent = unpack_normal(vp + 12);
            sv.bind_norm = unpack_normal(vp + 20);
            std::memcpy(&sv.u, vp + 24, 4);
            std::memcpy(&sv.v, vp + 28, 4);
            uint8_t local_b = vp[48];
            uint8_t real_b = (local_b < bone_map.size() && bone_map[local_b] < bone_count)
                                 ? static_cast<uint8_t>(bone_map[local_b])
                                 : 0;
            sv.bones[0] = real_b;
            sv.weights[0] = 255;
            sv.chunk_index = static_cast<uint8_t>(c);
            out_mesh.vertices.push_back(sv);
        }

        // Decode SoftVertices (56B each)
        for (int32_t v = 0; v < soft_cnt; ++v) {
            const uint8_t* vp = soft_ptr + static_cast<size_t>(v) * 56;
            SkinnedVertex sv{};
            std::memcpy(&sv.bind_pos.x, vp + 0, 4);
            std::memcpy(&sv.bind_pos.y, vp + 4, 4);
            std::memcpy(&sv.bind_pos.z, vp + 8, 4);
            sv.bind_tangent = unpack_normal(vp + 12);
            sv.bind_norm = unpack_normal(vp + 20);
            std::memcpy(&sv.u, vp + 24, 4);
            std::memcpy(&sv.v, vp + 28, 4);
            for (int k = 0; k < 4; ++k) {
                uint8_t local_b = vp[48 + k];
                uint8_t real_b = (local_b < bone_map.size() && bone_map[local_b] < bone_count)
                                     ? static_cast<uint8_t>(bone_map[local_b])
                                     : 0;
                sv.bones[k] = real_b;
                sv.weights[k] = vp[52 + k];
            }
            sv.chunk_index = static_cast<uint8_t>(c);
            out_mesh.vertices.push_back(sv);
        }
    }

    return out_mesh.is_valid();
}

// -----------------------------------------------------------------------------
// TdAnimSet & UAnimSequence Binary Parser (UE3 v536 ACF_Fixed48NoW / ACF_Float96NoW)
// -----------------------------------------------------------------------------
bool AnimSystem::parse_anim_set_package(const UPKPackage& pkg, AnimSetAsset& out_anim_set) {
    out_anim_set = AnimSetAsset{};
    const auto& data = pkg.get_data();

    // 1. Find primary AnimSet / TdAnimSet export with TrackBoneNames
    for (const auto& exp : pkg.get_exports()) {
        std::string cls = pkg.get_export_class(exp);
        if (cls != "TdAnimSet" && cls != "AnimSet") continue;

        size_t prop_start = pkg.find_property_start(exp);
        auto props = pkg.parse_properties(prop_start, exp.serial_size);
        auto it = props.find("TrackBoneNames");
        if (it == props.end() || it->second.raw_bytes.size() < 4) continue;

        const auto& raw = it->second.raw_bytes;
        const uint8_t* ptr = raw.data();
        const uint8_t* end = raw.data() + raw.size();
        int32_t count = read_le<int32_t>(ptr, end);
        if (count <= 0 || count > 512 || ptr + count * 8 > end) continue;

        out_anim_set.name = exp.object_name;
        out_anim_set.track_bone_names.resize(count);
        for (int32_t i = 0; i < count; ++i) {
            int32_t n_idx = read_le<int32_t>(ptr, end);
            int32_t n_num = read_le<int32_t>(ptr, end);
            std::string bname = read_fname_str(pkg, n_idx, n_num);
            out_anim_set.track_bone_names[i] = bname;
            out_anim_set.bone_to_track[to_lower_str(bname)] = i;
        }
        break;
    }

    if (out_anim_set.track_bone_names.empty()) return false;
    const size_t expected_tracks = out_anim_set.track_bone_names.size();

    // 2. Parse all AnimSequence exports matching this AnimSet's track count
    for (const auto& exp : pkg.get_exports()) {
        if (pkg.get_export_class(exp) != "AnimSequence") continue;

        size_t prop_start = pkg.find_property_start(exp);
        size_t bytes_read = 0;
        auto props = pkg.parse_properties(prop_start, exp.serial_size, &bytes_read);

        auto it_offs = props.find("CompressedTrackOffsets");
        if (it_offs == props.end() || it_offs->second.raw_bytes.size() < 4) continue;

        const auto& raw_offs = it_offs->second.raw_bytes;
        const uint8_t* op = raw_offs.data();
        const uint8_t* oend = raw_offs.data() + raw_offs.size();
        int32_t num_ints = read_le<int32_t>(op, oend);
        if (num_ints <= 0 || (num_ints % 4) != 0 || op + num_ints * 4 > oend) continue;

        size_t num_tracks = static_cast<size_t>(num_ints / 4);
        if (num_tracks != expected_tracks && !out_anim_set.sequences.empty()) {
            // Skip sequences belonging to secondary weapon sub-AnimSets with different track counts
            continue;
        }

        std::vector<int32_t> track_offs(num_ints);
        for (int32_t i = 0; i < num_ints; ++i) {
            track_offs[i] = read_le<int32_t>(op, oend);
        }

        size_t tail_off = prop_start + bytes_read;
        size_t exp_end = static_cast<size_t>(exp.serial_offset) + static_cast<size_t>(exp.serial_size);
        if (tail_off + 4 > exp_end || exp_end > data.size()) continue;

        const uint8_t* tail_ptr = data.data() + tail_off;
        const uint8_t* tail_end = data.data() + exp_end;
        int32_t stream_size = read_le<int32_t>(tail_ptr, tail_end);
        if (stream_size <= 0 || tail_ptr + stream_size > tail_end) continue;

        const uint8_t* stream = tail_ptr;

        AnimSequenceAsset seq{};
        seq.name = props.count("SequenceName") ? props["SequenceName"].str_val : exp.object_name;
        seq.length = props.count("SequenceLength") ? props["SequenceLength"].float_val : 1.0f;
        seq.num_frames = props.count("NumFrames") ? props["NumFrames"].int_val : 1;
        seq.rate_scale = props.count("RateScale") ? props["RateScale"].float_val : 1.0f;
        if (props.count("TranslationCompressionFormat") && !props["TranslationCompressionFormat"].str_val.empty()) {
            seq.trans_compression = props["TranslationCompressionFormat"].str_val;
        }
        if (props.count("RotationCompressionFormat") && !props["RotationCompressionFormat"].str_val.empty()) {
            seq.rot_compression = props["RotationCompressionFormat"].str_val;
        }

        seq.tracks.resize(num_tracks);
        bool is_float96 = (seq.rot_compression == "ACF_Float96NoW");

        for (size_t t = 0; t < num_tracks; ++t) {
            int32_t tr_off = track_offs[t * 4 + 0];
            int32_t tr_keys = track_offs[t * 4 + 1];
            int32_t rot_off = track_offs[t * 4 + 2];
            int32_t rot_keys = track_offs[t * 4 + 3];

            AnimTrack& dst = seq.tracks[t];

            // Decompress Translation Track (ACF_None: 12B FVector per key)
            if (tr_keys > 0 && tr_off >= 0 && tr_off + tr_keys * 12 <= stream_size) {
                dst.positions.resize(tr_keys);
                for (int32_t k = 0; k < tr_keys; ++k) {
                    const uint8_t* kp = stream + tr_off + k * 12;
                    float vx = 0.0f, vy = 0.0f, vz = 0.0f;
                    std::memcpy(&vx, kp + 0, 4);
                    std::memcpy(&vy, kp + 4, 4);
                    std::memcpy(&vz, kp + 8, 4);
                    dst.positions[k] = Vec3(vx, vy, vz);
                }
            }

            // Decompress Rotation Track
            if (rot_keys == 1 && rot_off >= 0 && rot_off + 12 <= stream_size) {
                // Single-key rotation is always stored as FQuatFloat96NoW (12B: 3 floats)
                const uint8_t* kp = stream + rot_off;
                float rx = 0.0f, ry = 0.0f, rz = 0.0f;
                std::memcpy(&rx, kp + 0, 4);
                std::memcpy(&ry, kp + 4, 4);
                std::memcpy(&rz, kp + 8, 4);
                float rw = std::sqrt(std::max(0.0f, 1.0f - (rx * rx + ry * ry + rz * rz)));
                // Convert to RefSkeleton raw quaternion convention by negating rw
                dst.rotations.push_back(Quat4(rx, ry, rz, -rw).normalized());
            } else if (rot_keys > 1 && rot_off >= 0) {
                if (is_float96 && rot_off + 24 + rot_keys * 12 <= stream_size) {
                    dst.rotations.resize(rot_keys);
                    for (int32_t k = 0; k < rot_keys; ++k) {
                        const uint8_t* kp = stream + rot_off + 24 + k * 12;
                        float rx = 0.0f, ry = 0.0f, rz = 0.0f;
                        std::memcpy(&rx, kp + 0, 4);
                        std::memcpy(&ry, kp + 4, 4);
                        std::memcpy(&rz, kp + 8, 4);
                        float rw = std::sqrt(std::max(0.0f, 1.0f - (rx * rx + ry * ry + rz * rz)));
                        dst.rotations[k] = Quat4(rx, ry, rz, -rw).normalized();
                    }
                } else if (!is_float96 && rot_off + 24 + rot_keys * 6 <= stream_size) {
                    // ACF_Fixed48NoW: 24B Mins+Ranges header + 6B (3 * uint16) per key
                    dst.rotations.resize(rot_keys);
                    for (int32_t k = 0; k < rot_keys; ++k) {
                        const uint8_t* kp = stream + rot_off + 24 + k * 6;
                        uint16_t qx = 0, qy = 0, qz = 0;
                        std::memcpy(&qx, kp + 0, 2);
                        std::memcpy(&qy, kp + 2, 2);
                        std::memcpy(&qz, kp + 4, 2);
                        float rx = (static_cast<float>(qx) - 32767.0f) * (1.0f / 32767.0f);
                        float ry = (static_cast<float>(qy) - 32767.0f) * (1.0f / 32767.0f);
                        float rz = (static_cast<float>(qz) - 32767.0f) * (1.0f / 32767.0f);
                        float rw = std::sqrt(std::max(0.0f, 1.0f - (rx * rx + ry * ry + rz * rz)));
                        dst.rotations[k] = Quat4(rx, ry, rz, -rw).normalized();
                    }
                }
            }
        }

        out_anim_set.sequences[to_lower_str(seq.name)] = std::move(seq);
    }

    return !out_anim_set.sequences.empty();
}

// -----------------------------------------------------------------------------
// Initialize AnimSystem from Mirror's Edge Game Root
// -----------------------------------------------------------------------------
bool AnimSystem::init_from_game_root(const std::string& game_root) {
    game_root_ = game_root;
    std::string cooked = game_root + "/TdGame/CookedPC/";

    // 1. Parse DefaultAnimation.ini [TdGame.TdAnimNode*] / [TdGame.TdSkelControl*] sections
    {
        std::ifstream ini_f(game_root + "/TdGame/Config/DefaultAnimation.ini");
        std::string line;
        std::string current_class;
        while (std::getline(ini_f, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t')) {
                line.pop_back();
            }
            if (line.empty() || line[0] == '#' || line[0] == ';') continue;
            if (line.front() == '[' && line.back() == ']') {
                current_class = line.substr(1, line.size() - 2);
                continue;
            }
            size_t eq = line.find('=');
            if (eq != std::string::npos && !current_class.empty()) {
                std::string key = line.substr(0, eq);
                std::string val = line.substr(eq + 1);
                while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
                while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(val.begin());
                if (key.find("Blend") != std::string::npos || key.find("Vel") != std::string::npos) {
                    AnimBlendConfig cfg{};
                    cfg.node_name = key;
                    cfg.node_class = current_class;
                    try {
                        cfg.blend_in_time = std::stof(val);
                    } catch (...) {
                        cfg.blend_in_time = 0.2f;
                    }
                    blend_configs_.push_back(cfg);
                }
            }
        }
    }

    // 2. Load Faith 1P Skeletal Meshes & Textures (CH_TKY_Crim_Fixer_1P.upk + CH_Faith_Cinematic.upk)
    UPKPackage pkg_1p(cooked + "Characters/CH_TKY_Crim_Fixer_1P.upk");
    if (pkg_1p.is_valid()) {
        for (const auto& exp : pkg_1p.get_exports()) {
            if (pkg_1p.get_export_class(exp) == "SkeletalMesh") {
                if (exp.object_name == "SK_UpperBody") {
                    parse_skeletal_mesh(pkg_1p, exp, faith_upper_);
                } else if (exp.object_name == "SK_LowerBody") {
                    parse_skeletal_mesh(pkg_1p, exp, faith_lower_);
                }
            }
        }

        DXT1Texture tex_skin{}, tex_glove{};
        parse_dxt1_texture(pkg_1p, "Female_1p_C", tex_skin);
        parse_dxt1_texture(pkg_1p, "Faith_Glove_C", tex_glove);

        for (auto& v : faith_upper_.vertices) {
            if (v.chunk_index == 0 && tex_skin.is_valid()) {
                Vec3 c = tex_skin.sample_rgb01(v.u, v.v);
                // Warm natural skin calibration matching DICE's M_skinJocTest3_SH subsurface shader
                c.x = std::clamp(std::pow(c.x, 0.85f) * 1.06f, 0.15f, 0.96f);
                c.y = std::clamp(std::pow(c.y, 0.88f) * 1.02f, 0.12f, 0.88f);
                c.z = std::clamp(std::pow(c.z, 0.90f) * 0.98f, 0.10f, 0.82f);
                v.color = pack_rgba8(c.x, c.y, c.z);
            } else if (tex_glove.is_valid()) {
                Vec3 c = tex_glove.sample_rgb01(v.u, v.v);
                // Enhance red Runner glove leather vs black leather strap contrast
                if (c.x > c.y * 1.35f && c.x > 0.12f) {
                    c = Vec3(std::clamp(c.x * 1.65f, 0.55f, 0.94f),
                             std::clamp(c.y * 0.65f, 0.05f, 0.18f),
                             std::clamp(c.z * 0.65f, 0.05f, 0.18f));
                } else {
                    c = Vec3(std::clamp(c.x * 1.15f + 0.06f, 0.12f, 0.85f),
                             std::clamp(c.y * 1.15f + 0.06f, 0.12f, 0.85f),
                             std::clamp(c.z * 1.18f + 0.07f, 0.14f, 0.88f));
                }
                v.color = pack_rgba8(c.x, c.y, c.z);
            }
        }
    }

    UPKPackage pkg_cine(cooked + "Characters/CH_Faith_Cinematic.upk");
    if (pkg_cine.is_valid() && faith_lower_.is_valid()) {
        DXT1Texture tex_lower{}, tex_upper{};
        parse_dxt1_texture(pkg_cine, "Faith_Cine_Lower_C", tex_lower);
        parse_dxt1_texture(pkg_cine, "Faith_Cine_Upper_C", tex_upper);

        for (auto& v : faith_lower_.vertices) {
            if (v.chunk_index == 1 && tex_lower.is_valid()) {
                Vec3 c = tex_lower.sample_rgb01(v.u, v.v);
                // Boost white/grey Runner cargo pants and red/black split-toe Tabi shoes
                if (c.x > c.y * 1.35f && c.x > 0.15f) {
                    c = Vec3(std::clamp(c.x * 1.55f, 0.50f, 0.92f),
                             std::clamp(c.y * 0.65f, 0.06f, 0.20f),
                             std::clamp(c.z * 0.65f, 0.06f, 0.20f));
                } else {
                    c.x = std::clamp(std::pow(c.x, 0.75f) * 1.15f, 0.16f, 0.95f);
                    c.y = std::clamp(std::pow(c.y, 0.75f) * 1.15f, 0.16f, 0.95f);
                    c.z = std::clamp(std::pow(c.z, 0.75f) * 1.18f, 0.18f, 0.96f);
                }
                v.color = pack_rgba8(c.x, c.y, c.z);
            } else if (tex_upper.is_valid()) {
                Vec3 c = tex_upper.sample_rgb01(v.u, v.v);
                c = Vec3(std::clamp(c.x * 1.25f + 0.10f, 0.15f, 0.85f),
                         std::clamp(c.y * 1.25f + 0.10f, 0.15f, 0.85f),
                         std::clamp(c.z * 1.28f + 0.12f, 0.16f, 0.88f));
                v.color = pack_rgba8(c.x, c.y, c.z);
            }
        }
    }

    // 3. Load KrugerSec / CPF SWAT Officer Skeletal Mesh & Textures (CH_TKY_Cop_SWAT.upk)
    UPKPackage pkg_swat(cooked + "Characters/CH_TKY_Cop_SWAT.upk");
    if (pkg_swat.is_valid()) {
        for (const auto& exp : pkg_swat.get_exports()) {
            if (pkg_swat.get_export_class(exp) == "SkeletalMesh" && exp.object_name == "CH_TKY_Cop_SWAT") {
                parse_skeletal_mesh(pkg_swat, exp, swat_mesh_);
                break;
            }
        }
        DXT1Texture tex_swat_d{}, tex_swat_s{};
        parse_dxt1_texture(pkg_swat, "T_TKY_Cop_SWAT_D", tex_swat_d);
        parse_dxt1_texture(pkg_swat, "T_TKY_Cop_SWAT_S", tex_swat_s);

        for (auto& v : swat_mesh_.vertices) {
            if (tex_swat_d.is_valid()) {
                Vec3 d = tex_swat_d.sample_rgb01(v.u, v.v);
                Vec3 s = tex_swat_s.is_valid() ? tex_swat_s.sample_rgb01(v.u, v.v) : Vec3(0.2f, 0.2f, 0.2f);
                // Apply M_Generic_CharMaterial_SH sRGB + Spherical Harmonic tactical armor lift
                float r = std::pow(std::max(d.x, 0.0f), 0.45f) * 0.72f + s.x * 0.35f + 0.12f;
                float g = std::pow(std::max(d.y, 0.0f), 0.45f) * 0.75f + s.y * 0.38f + 0.14f;
                float b = std::pow(std::max(d.z, 0.0f), 0.45f) * 0.82f + s.z * 0.44f + 0.18f;
                // Highlight white CPF chest/shoulder armor plates where diffuse/spec luminance is higher
                float lum = (d.x + d.y + d.z + s.x + s.y + s.z) * 0.1667f;
                if (lum > 0.08f && v.bind_pos.y < -95.0f && v.bind_pos.y > -148.0f) {
                    r = std::clamp(r * 1.45f + 0.16f, 0.25f, 0.92f);
                    g = std::clamp(g * 1.45f + 0.17f, 0.26f, 0.93f);
                    b = std::clamp(b * 1.48f + 0.19f, 0.28f, 0.95f);
                }
                v.color = pack_rgba8(std::clamp(r, 0.12f, 0.94f),
                                     std::clamp(g, 0.14f, 0.95f),
                                     std::clamp(b, 0.17f, 0.96f));
            }
        }
    }

    // 4. Load Colt 1911 Weapon Skeletal Mesh & Textures (Weapons/WP_Colt1911.upk)
    UPKPackage pkg_colt(cooked + "Weapons/WP_Colt1911.upk");
    if (pkg_colt.is_valid()) {
        for (const auto& exp : pkg_colt.get_exports()) {
            if (pkg_colt.get_export_class(exp) == "SkeletalMesh" && exp.object_name == "SK_Colt1911") {
                parse_skeletal_mesh(pkg_colt, exp, colt1911_mesh_);
                break;
            }
        }
        DXT1Texture tex_colt_d{}, tex_colt_s{};
        parse_dxt1_texture(pkg_colt, "T_Colt1911_D", tex_colt_d);
        parse_dxt1_texture(pkg_colt, "T_Colt1911_S", tex_colt_s);
        for (auto& v : colt1911_mesh_.vertices) {
            Vec3 d = tex_colt_d.is_valid() ? tex_colt_d.sample_rgb01(v.u, v.v) : Vec3(0.15f, 0.16f, 0.18f);
            Vec3 s = tex_colt_s.is_valid() ? tex_colt_s.sample_rgb01(v.u, v.v) : Vec3(0.25f, 0.26f, 0.28f);
            float r = std::pow(std::max(d.x, 0.0f), 0.45f) * 0.65f + s.x * 0.40f + 0.16f;
            float g = std::pow(std::max(d.y, 0.0f), 0.45f) * 0.67f + s.y * 0.42f + 0.17f;
            float b = std::pow(std::max(d.z, 0.0f), 0.45f) * 0.72f + s.z * 0.46f + 0.20f;
            v.color = pack_rgba8(std::clamp(r, 0.16f, 0.75f),
                                 std::clamp(g, 0.17f, 0.77f),
                                 std::clamp(b, 0.19f, 0.82f));
        }
    }

    // 5. Load Animation Sets (AS_C1P_Unarmed, AS_C1P_OneHanded_Common, AS_C1P_OneHanded_Colt1911, AS_AI_PatrolCop_OneHanded)
    UPKPackage pkg_as_unarmed(cooked + "Animations/AS_C1P_Unarmed.upk");
    if (pkg_as_unarmed.is_valid()) parse_anim_set_package(pkg_as_unarmed, faith_unarmed_set_);

    UPKPackage pkg_as_common(cooked + "Animations/AS_C1P_OneHanded_Common.upk");
    if (pkg_as_common.is_valid()) parse_anim_set_package(pkg_as_common, faith_common_set_);

    UPKPackage pkg_as_colt(cooked + "Animations/AS_C1P_OneHanded_Colt1911.upk");
    if (pkg_as_colt.is_valid()) parse_anim_set_package(pkg_as_colt, faith_colt_set_);

    UPKPackage pkg_as_swat(cooked + "Animations/AS_AI_PatrolCop_OneHanded.upk");
    if (pkg_as_swat.is_valid()) parse_anim_set_package(pkg_as_swat, swat_set_);

    loaded_ = faith_upper_.is_valid() && faith_lower_.is_valid() &&
              swat_mesh_.is_valid() && !faith_unarmed_set_.sequences.empty() &&
              !swat_set_.sequences.empty();

    if (loaded_) {
        std::cout << "[AnimSystem] Loaded real UE3 USkeletalMesh & TdAnimSet assets:\n"
                  << "  - SK_UpperBody: " << faith_upper_.bones.size() << " bones, "
                  << faith_upper_.vertices.size() << " skinned verts, "
                  << (faith_upper_.indices.size() / 3) << " tris\n"
                  << "  - SK_LowerBody: " << faith_lower_.bones.size() << " bones, "
                  << faith_lower_.vertices.size() << " skinned verts, "
                  << (faith_lower_.indices.size() / 3) << " tris\n"
                  << "  - CH_TKY_Cop_SWAT: " << swat_mesh_.bones.size() << " bones, "
                  << swat_mesh_.vertices.size() << " skinned verts, "
                  << (swat_mesh_.indices.size() / 3) << " tris\n"
                  << "  - SK_Colt1911: " << colt1911_mesh_.bones.size() << " bones, "
                  << colt1911_mesh_.vertices.size() << " skinned verts, "
                  << (colt1911_mesh_.indices.size() / 3) << " tris\n"
                  << "  - AS_C1P_Unarmed: " << faith_unarmed_set_.sequences.size() << " sequences\n"
                  << "  - AS_C1P_OneHanded_Common: " << faith_common_set_.sequences.size() << " sequences\n"
                  << "  - AS_C1P_OneHanded_Colt1911: " << faith_colt_set_.sequences.size() << " sequences\n"
                  << "  - AS_AI_PatrolCop_OneHanded: " << swat_set_.sequences.size() << " sequences\n"
                  << "  - DefaultAnimation.ini CustomAnimNodes: " << blend_configs_.size() << std::endl;
    }

    return loaded_;
}

// -----------------------------------------------------------------------------
// Evaluate Faith's 1P Skeletal Viewmodel (AT_C1P Blend Tree + Linear Blend Skinning)
// -----------------------------------------------------------------------------
void AnimSystem::evaluate_faith_1p(const PlayerTelemetry& telemetry, std::vector<Vertex>& out_triangles) const {
    out_triangles.clear();
    if (!loaded_ || !faith_upper_.is_valid()) return;

    const AnimSetAsset* active_set = &faith_unarmed_set_;
    const AnimSequenceAsset* seq_a = nullptr;
    const AnimSequenceAsset* seq_b = nullptr;
    float blend_alpha = 0.0f;
    float norm_time = 0.0f;

    // Viewmodel camera framing offset (places Faith's 1P forearms/hands in lower peripheral frustum)
    Vec3 vm_offset(0.0f, 16.0f, 14.0f);
    bool show_lower_body = false;

    const float sim_t = telemetry.sim_time;
    const float speed = telemetry.speed_2d;
    const EMovement state = telemetry.move_state;

    if (telemetry.weapon.equipped) {
        active_set = &faith_colt_set_;
        seq_a = active_set->find_sequence("standfire");
        norm_time = (telemetry.weapon.cooldown > 0.0f) ? std::fmod(sim_t * 2.0f, 1.0f) : 0.0f;
        vm_offset = Vec3(2.0f, 10.0f, 4.0f);
    } else {
        switch (state) {
            case EMovement::MOVE_SpringBoarding:
                seq_a = active_set->find_sequence("SpringBoardRightLeg");
                seq_b = active_set->find_sequence("VaultOver");
                norm_time = 0.55f; // Frame 31/57: both hands vaulted forward into view
                blend_alpha = 0.25f;
                vm_offset = Vec3(0.0f, 8.0f, -6.0f);
                break;
            case EMovement::MOVE_SpeedVaulting:
            case EMovement::MOVE_VaultOver:
                seq_a = active_set->find_sequence("VaultOver");
                norm_time = 0.45f;
                vm_offset = Vec3(0.0f, 10.0f, -4.0f);
                break;
            case EMovement::MOVE_WallRunningRight:
                seq_a = active_set->find_sequence("WallrunRight");
                seq_b = active_set->find_sequence("SprintFwd");
                norm_time = 0.12f; // Frame 2/16: right hand touching wall, left hand pumping
                blend_alpha = 0.35f;
                vm_offset = Vec3(4.0f, 14.0f, 14.0f);
                break;
            case EMovement::MOVE_WallRunningLeft:
                seq_a = active_set->find_sequence("WallrunLeft");
                seq_b = active_set->find_sequence("SprintFwd");
                norm_time = 0.12f;
                blend_alpha = 0.35f;
                vm_offset = Vec3(-4.0f, 14.0f, 14.0f);
                break;
            case EMovement::MOVE_WallClimbing:
                seq_a = active_set->find_sequence("WallRunVertical");
                norm_time = std::fmod(sim_t * 1.8f, 1.0f);
                vm_offset = Vec3(0.0f, 16.0f, 14.0f);
                break;
            case EMovement::MOVE_ZipLine:
                seq_a = active_set->find_sequence("ZipLine");
                norm_time = 0.10f;
                vm_offset = Vec3(2.0f, 62.0f, -4.0f);
                break;
            case EMovement::MOVE_Slide:
            case EMovement::MOVE_MeleeSlide:
            case EMovement::MOVE_Coil:
                seq_a = active_set->find_sequence("CrouchSlide");
                norm_time = 0.33f;
                vm_offset = Vec3(0.0f, 6.0f, 14.0f);
                show_lower_body = true;
                break;
            case EMovement::MOVE_Jump:
            case EMovement::MOVE_Falling:
            case EMovement::MOVE_WallRunJump:
                seq_a = active_set->find_sequence("jumpfast");
                if (!seq_a) seq_a = active_set->find_sequence("jumpair");
                norm_time = 0.30f;
                vm_offset = Vec3(0.0f, 16.0f, 14.0f);
                break;
            case EMovement::MOVE_Crouch:
                seq_a = (speed > 20.0f) ? active_set->find_sequence("crouchfwd")
                                        : active_set->find_sequence("crouchstill");
                norm_time = std::fmod(sim_t * 1.2f, 1.0f);
                vm_offset = Vec3(0.0f, 16.0f, 14.0f);
                break;
            case EMovement::MOVE_SkillRoll:
                seq_a = active_set->find_sequence("fallinglandroll");
                norm_time = std::fmod(sim_t * 1.5f, 1.0f);
                vm_offset = Vec3(0.0f, 16.0f, 14.0f);
                break;
            case EMovement::MOVE_LedgeWalk:
                seq_a = active_set->find_sequence("walkbalancefwd");
                norm_time = std::fmod(sim_t * 1.2f, 1.0f);
                vm_offset = Vec3(0.0f, 16.0f, 14.0f);
                break;
            default: {
                // TdAnimNodeWalkingState: blend Stand -> walkfwd -> runfwd -> SprintFwd by speed_2d
                const AnimSequenceAsset* s_walk  = active_set->find_sequence("walkfwd");
                const AnimSequenceAsset* s_run   = active_set->find_sequence("runfwd");
                const AnimSequenceAsset* s_spr   = active_set->find_sequence("SprintFwd");
                float cycle = std::fmod(sim_t * 1.6f + 0.38f, 1.0f);
                norm_time = cycle;
                if (speed < 40.0f) {
                    seq_a = s_run;
                    seq_b = s_spr;
                    blend_alpha = 0.72f;
                    norm_time = 0.78f;
                    vm_offset = Vec3(0.0f, 16.0f, 16.0f);
                } else if (speed < 280.0f) {
                    seq_a = s_walk;
                    seq_b = s_run;
                    blend_alpha = std::clamp((speed - 40.0f) / 240.0f, 0.0f, 1.0f);
                    vm_offset = Vec3(0.0f, 16.0f, 18.0f);
                } else {
                    seq_a = s_run;
                    seq_b = s_spr;
                    blend_alpha = std::clamp((speed - 280.0f) / 320.0f, 0.0f, 1.0f);
                    vm_offset = Vec3(0.0f, 16.0f, 18.0f);
                }
                break;
            }
        }
    }

    if (!seq_a) seq_a = faith_unarmed_set_.find_sequence("Stand");

    std::vector<Vec3> local_pos, local_pos_b;
    std::vector<Quat4> local_quat, local_quat_b;
    sample_sequence_pose(faith_upper_, *active_set, seq_a, norm_time, local_pos, local_quat);

    if (seq_b && blend_alpha > 0.001f) {
        const AnimSetAsset* set_b = (&faith_colt_set_.sequences == &active_set->sequences ||
                                     faith_colt_set_.find_sequence(seq_b->name) == seq_b)
                                        ? &faith_colt_set_
                                        : active_set;
        sample_sequence_pose(faith_upper_, *set_b, seq_b, norm_time, local_pos_b, local_quat_b);
        blend_local_poses(local_pos, local_quat, local_pos_b, local_quat_b, blend_alpha, local_pos, local_quat);
    }

    // Forward kinematics in component space
    std::vector<Vec3> comp_pos;
    std::vector<Quat4> comp_quat;
    compute_skeleton_fk(faith_upper_.bones, local_pos, local_quat, comp_pos, comp_quat);

    // Compute per-bone Linear Blend Skinning deltas
    std::vector<Vec3> delta_pos;
    std::vector<Quat4> delta_quat;
    compute_skin_deltas(faith_upper_, comp_pos, comp_quat, delta_pos, delta_quat);

    // EyeJoint (Bone 72) reference frame
    const size_t eye_idx = (faith_upper_.bones.size() > 72) ? 72 : 0;
    const Vec3 eye_pos = comp_pos[eye_idx];
    const Quat4 eye_inv_quat = comp_quat[eye_idx].conjugate();

    // Note: vm_view = look_at((0,0,0), (0,100,0), (0,0,1)) has s = (-1,0,0),
    // so +rel.x maps to Screen Left (LeftHand) and -rel.x maps to Screen Right (RightHand Red Glove).
    // (rel.x, rel.z, -rel.y) is a proper right-handed 90-deg rotation (det = +1).
    auto raw_to_vm_pos = [&](const Vec3& raw_p, bool is_lower) -> Vec3 {
        Vec3 rel = eye_inv_quat.rotate(raw_p - eye_pos);
        if (!is_lower) {
            return Vec3(rel.x + vm_offset.x, rel.z + vm_offset.y, -rel.y + vm_offset.z);
        }
        // For lower-body slide kick: shift hips behind near plane and pitch shins/boots into lower foreground
        float lx = rel.x - 6.0f;
        float ly = rel.z - 54.0f;
        float lz = -rel.y - 2.0f;
        const float sin_p = 0.28f;
        const float cos_p = 0.96f;
        return Vec3(lx, ly * cos_p - lz * sin_p, ly * sin_p + lz * cos_p - 8.0f);
    };
    auto raw_to_vm_dir = [&](const Vec3& raw_d) -> Vec3 {
        Vec3 rel = eye_inv_quat.rotate(raw_d);
        return Vec3(rel.x, rel.z, -rel.y).normalized();
    };

    auto append_skinned_mesh = [&](const SkeletalMeshAsset& mesh, bool is_lower) {
        std::vector<Vec3> skinned_pos(mesh.vertices.size());
        std::vector<Vec3> skinned_norm(mesh.vertices.size());
        std::vector<uint8_t> vert_valid(mesh.vertices.size(), 1);

        for (size_t i = 0; i < mesh.vertices.size(); ++i) {
            const SkinnedVertex& sv = mesh.vertices[i];
            uint8_t dom_bone = sv.bones[0];
            if (!is_lower) {
                if (sv.chunk_index == 1) {
                    // Chunk 1 (Faith_Glove): keep only RightHand & finger bones (48..70),
                    // culling the rolled-up elbow shirt cuff (17,18,41,46,47,71)
                    if (dom_bone < 48 || dom_bone > 70) {
                        vert_valid[i] = 0;
                        continue;
                    }
                } else {
                    // Chunk 0 (Skin): keep forearms, wrists, hands, and fingers (18..41, 47..71)
                    bool is_forearm_or_hand = (dom_bone >= 18 && dom_bone <= 41) || (dom_bone >= 47 && dom_bone <= 71);
                    if (!is_forearm_or_hand) {
                        vert_valid[i] = 0;
                        continue;
                    }
                }
            } else {
                // In SK_LowerBody, keep Chunk 1 (cargo pants & split-toe Runner boots) across leg bones (4..13)
                if (sv.chunk_index != 1 || dom_bone < 4 || dom_bone > 13) {
                    vert_valid[i] = 0;
                    continue;
                }
            }

            Vec3 p_acc(0.0f, 0.0f, 0.0f);
            Vec3 n_acc(0.0f, 0.0f, 0.0f);
            for (int k = 0; k < 4; ++k) {
                if (sv.weights[k] == 0) continue;
                float w = static_cast<float>(sv.weights[k]) * (1.0f / 255.0f);
                size_t b = std::min(static_cast<size_t>(sv.bones[k]), delta_pos.size() - 1);
                p_acc += (delta_quat[b].rotate(sv.bind_pos) + delta_pos[b]) * w;
                n_acc += delta_quat[b].rotate(sv.bind_norm) * w;
            }
            skinned_pos[i] = raw_to_vm_pos(p_acc, is_lower);
            skinned_norm[i] = raw_to_vm_dir(n_acc);
            if (skinned_pos[i].y < (is_lower ? 10.0f : 4.0f)) {
                vert_valid[i] = 0; // Behind near plane
            }
        }

        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            uint16_t i0 = mesh.indices[i + 0];
            uint16_t i1 = mesh.indices[i + 1];
            uint16_t i2 = mesh.indices[i + 2];
            if (i0 >= mesh.vertices.size() || i1 >= mesh.vertices.size() || i2 >= mesh.vertices.size()) continue;
            if (!vert_valid[i0] || !vert_valid[i1] || !vert_valid[i2]) continue;

            const uint16_t idx_tri[3] = {i0, i1, i2};
            for (int k = 0; k < 3; ++k) {
                uint16_t vi = idx_tri[k];
                const SkinnedVertex& sv = mesh.vertices[vi];
                Vertex out_v{};
                out_v.position = skinned_pos[vi];
                out_v.normal = skinned_norm[vi];
                out_v.tangent = Vec3(1.0f, 0.0f, 0.0f);
                out_v.u = sv.u;
                out_v.v = sv.v;
                out_v.color = sv.color;
                out_triangles.push_back(out_v);
            }
        }
    };

    out_triangles.reserve(faith_upper_.indices.size() + faith_lower_.indices.size() + colt1911_mesh_.indices.size());
    append_skinned_mesh(faith_upper_, false);
    if (show_lower_body && faith_lower_.is_valid()) {
        append_skinned_mesh(faith_lower_, true);
    }

    // Attach SK_Colt1911 to RightHand (Bone 48) when armed
    if (telemetry.weapon.equipped && colt1911_mesh_.is_valid()) {
        int32_t rh_idx = 48;
        auto it_rh = faith_upper_.bone_name_to_index.find("righthand");
        if (it_rh != faith_upper_.bone_name_to_index.end()) rh_idx = it_rh->second;
        Vec3 hand_vm = raw_to_vm_pos(comp_pos[rh_idx], false);
        // SK_Colt1911 uses the same (x=Left, -y=Up, +z=Forward) Maya bind axes; scale by 0.62x into RightHand palm
        const float gun_scale = 0.62f;
        Vec3 gun_anchor = hand_vm + Vec3(1.5f, 4.5f, 4.8f);

        for (size_t i = 0; i + 2 < colt1911_mesh_.indices.size(); i += 3) {
            for (int k = 0; k < 3; ++k) {
                uint16_t vi = colt1911_mesh_.indices[i + k];
                if (vi >= colt1911_mesh_.vertices.size()) continue;
                const SkinnedVertex& sv = colt1911_mesh_.vertices[vi];
                Vertex out_v{};
                out_v.position = gun_anchor + Vec3(sv.bind_pos.x * gun_scale,
                                                   sv.bind_pos.z * gun_scale,
                                                   -sv.bind_pos.y * gun_scale);
                out_v.normal = Vec3(sv.bind_norm.x, sv.bind_norm.z, -sv.bind_norm.y).normalized();
                out_v.tangent = Vec3(1.0f, 0.0f, 0.0f);
                out_v.u = sv.u;
                out_v.v = sv.v;
                out_v.color = sv.color;
                out_triangles.push_back(out_v);
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Evaluate KrugerSec / CPF SWAT Officer Skeletal Mesh (AT_Cop + LBS)
// -----------------------------------------------------------------------------
void AnimSystem::evaluate_enemy_swat(const EnemyBot& bot, float sim_time, bool reaction_disarm, std::vector<Vertex>& out_triangles) const {
    out_triangles.clear();
    if (!loaded_ || !swat_mesh_.is_valid()) return;

    const AnimSequenceAsset* seq_a = nullptr;
    const AnimSequenceAsset* seq_b = nullptr;
    float blend_alpha = 0.0f;
    float norm_time = 0.0f;

    if (bot.stunned) {
        seq_a = swat_set_.find_sequence("HitHeavyHead");
        if (!seq_a) seq_a = swat_set_.find_sequence("HitMeleeSlide");
        norm_time = 0.45f;
    } else if (reaction_disarm && bot.disarm_window) {
        seq_a = swat_set_.find_sequence("SnatchFwd");
        seq_b = swat_set_.find_sequence("standfire");
        norm_time = 0.24f;
        blend_alpha = 0.45f;
    } else {
        seq_a = swat_set_.find_sequence("standfire");
        seq_b = swat_set_.find_sequence("Stand");
        norm_time = std::fmod(sim_time * 1.2f, 1.0f);
        blend_alpha = 0.25f;
    }

    if (!seq_a) seq_a = swat_set_.find_sequence("Stand");

    std::vector<Vec3> local_pos, local_pos_b;
    std::vector<Quat4> local_quat, local_quat_b;
    sample_sequence_pose(swat_mesh_, swat_set_, seq_a, norm_time, local_pos, local_quat);
    if (seq_b && blend_alpha > 0.001f) {
        sample_sequence_pose(swat_mesh_, swat_set_, seq_b, norm_time, local_pos_b, local_quat_b);
        blend_local_poses(local_pos, local_quat, local_pos_b, local_quat_b, blend_alpha, local_pos, local_quat);
    }

    std::vector<Vec3> comp_pos;
    std::vector<Quat4> comp_quat;
    compute_skeleton_fk(swat_mesh_.bones, local_pos, local_quat, comp_pos, comp_quat);

    std::vector<Vec3> delta_pos;
    std::vector<Quat4> delta_quat;
    compute_skin_deltas(swat_mesh_, comp_pos, comp_quat, delta_pos, delta_quat);

    // Map raw component coordinates (X=Left, -Y=Up, +Z=Forward) to Unreal/Engine bot local space (+X=Forward, +Y=Right, +Z=Up)
    std::vector<Vec3> skinned_pos(swat_mesh_.vertices.size());
    std::vector<Vec3> skinned_norm(swat_mesh_.vertices.size());
    for (size_t i = 0; i < swat_mesh_.vertices.size(); ++i) {
        const SkinnedVertex& sv = swat_mesh_.vertices[i];
        Vec3 p_acc(0.0f, 0.0f, 0.0f);
        Vec3 n_acc(0.0f, 0.0f, 0.0f);
        for (int k = 0; k < 4; ++k) {
            if (sv.weights[k] == 0) continue;
            float w = static_cast<float>(sv.weights[k]) * (1.0f / 255.0f);
            size_t b = std::min(static_cast<size_t>(sv.bones[k]), delta_pos.size() - 1);
            p_acc += (delta_quat[b].rotate(sv.bind_pos) + delta_pos[b]) * w;
            n_acc += delta_quat[b].rotate(sv.bind_norm) * w;
        }
        skinned_pos[i] = Vec3(p_acc.z, -p_acc.x, -p_acc.y);
        skinned_norm[i] = Vec3(n_acc.z, -n_acc.x, -n_acc.y).normalized();
    }

    out_triangles.reserve(swat_mesh_.indices.size() + colt1911_mesh_.indices.size());
    for (size_t i = 0; i + 2 < swat_mesh_.indices.size(); i += 3) {
        for (int k = 0; k < 3; ++k) {
            uint16_t vi = swat_mesh_.indices[i + k];
            if (vi >= swat_mesh_.vertices.size()) continue;
            const SkinnedVertex& sv = swat_mesh_.vertices[vi];
            Vertex out_v{};
            out_v.position = skinned_pos[vi];
            out_v.normal = skinned_norm[vi];
            out_v.tangent = Vec3(1.0f, 0.0f, 0.0f);
            out_v.u = sv.u;
            out_v.v = sv.v;
            out_v.color = sv.color;
            out_triangles.push_back(out_v);
        }
    }

    // Attach weapon to SWAT officer's RightWeapon / RightHand bone when armed (!bot.stunned)
    if (!bot.stunned && colt1911_mesh_.is_valid()) {
        int32_t rw_idx = 0;
        auto it_rw = swat_mesh_.bone_name_to_index.find("rightweapon");
        if (it_rw == swat_mesh_.bone_name_to_index.end()) {
            it_rw = swat_mesh_.bone_name_to_index.find("righthand");
        }
        if (it_rw != swat_mesh_.bone_name_to_index.end()) rw_idx = it_rw->second;

        Vec3 raw_hand = comp_pos[rw_idx];
        Vec3 hand_local(raw_hand.z, -raw_hand.x, -raw_hand.y);
        uint32_t disarm_red = pack_rgba8(0.94f, 0.08f, 0.08f);

        const float bot_gun_scale = 0.65f;
        for (size_t i = 0; i + 2 < colt1911_mesh_.indices.size(); i += 3) {
            for (int k = 0; k < 3; ++k) {
                uint16_t vi = colt1911_mesh_.indices[i + k];
                if (vi >= colt1911_mesh_.vertices.size()) continue;
                const SkinnedVertex& sv = colt1911_mesh_.vertices[vi];
                Vertex out_v{};
                out_v.position = hand_local + Vec3(sv.bind_pos.z * bot_gun_scale + 4.0f,
                                                   -sv.bind_pos.x * bot_gun_scale,
                                                   -sv.bind_pos.y * bot_gun_scale);
                out_v.normal = Vec3(sv.bind_norm.z, -sv.bind_norm.x, -sv.bind_norm.y).normalized();
                out_v.tangent = Vec3(1.0f, 0.0f, 0.0f);
                out_v.u = sv.u;
                out_v.v = sv.v;
                out_v.color = bot.disarm_window ? disarm_red : sv.color;
                out_triangles.push_back(out_v);
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Automated Verification Suite for USkeletalMesh, TdAnimSet, & LBS Skinning
// -----------------------------------------------------------------------------
bool AnimSystem::verify_all() const {
    if (!loaded_) return false;
    if (faith_upper_.bones.size() != 74 || faith_lower_.bones.size() != 74) return false;
    if (swat_mesh_.bones.size() != 88 || colt1911_mesh_.bones.size() != 8) return false;
    if (faith_unarmed_set_.sequences.size() < 250) return false;
    if (swat_set_.sequences.size() < 90) return false;

    PlayerTelemetry dummy_tel{};
    dummy_tel.move_state = EMovement::MOVE_Walking;
    dummy_tel.speed_2d = 600.0f;
    dummy_tel.sim_time = 0.5f;
    std::vector<Vertex> tris_1p;
    evaluate_faith_1p(dummy_tel, tris_1p);
    if (tris_1p.size() < 6000) return false;

    EnemyBot dummy_bot{};
    dummy_bot.alive = true;
    dummy_bot.disarm_window = true;
    std::vector<Vertex> tris_swat;
    evaluate_enemy_swat(dummy_bot, 0.5f, true, tris_swat);
    if (tris_swat.size() < 35000) return false;

    return true;
}

} // namespace me
