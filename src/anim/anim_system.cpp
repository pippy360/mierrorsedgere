#include "anim_system.hpp"
#include <mutex>
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
        const std::string& key = mesh.bones[b].name_lower.empty() ? to_lower_str(mesh.bones[b].name) : mesh.bones[b].name_lower;
        auto it = anim_set.bone_to_track.find(key);
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

bool DXT1Texture::decode_rgba8(std::vector<uint8_t>& out_rgba) const {
    if (!is_valid()) return false;
    const int32_t w = width;
    const int32_t h = height;
    const int32_t bw = std::max(1, (w + 3) / 4);
    const int32_t bh = std::max(1, (h + 3) / 4);
    out_rgba.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 255);

    auto unpack_565_u8 = [](uint16_t c, uint8_t out[4]) {
        uint32_t r5 = (c >> 11) & 31u;
        uint32_t g6 = (c >> 5) & 63u;
        uint32_t b5 = c & 31u;
        int r8 = static_cast<int>((r5 * 255u + 15u) / 31u);
        int g8 = static_cast<int>((g6 * 255u + 31u) / 63u);
        int b8 = static_cast<int>((b5 * 255u + 15u) / 31u);
        // Eliminate DXT1 RGB565 (5-bit R/B vs 6-bit G) green/magenta quantization banding on neutral gunmetal texels
        if (std::abs(r8 - b8) <= 9 && std::abs(g8 - ((r8 + b8) >> 1)) <= 10) {
            uint8_t lum = static_cast<uint8_t>((r8 + 2 * g8 + b8 + 2) >> 2);
            out[0] = lum;
            out[1] = lum;
            out[2] = lum;
        } else {
            out[0] = static_cast<uint8_t>(r8);
            out[1] = static_cast<uint8_t>(g8);
            out[2] = static_cast<uint8_t>(b8);
        }
        out[3] = 255;
    };

    for (int32_t by = 0; by < bh; ++by) {
        for (int32_t bx = 0; bx < bw; ++bx) {
            size_t blk_off = static_cast<size_t>(by * bw + bx) * 8;
            if (blk_off + 8 > dxt1_blocks.size()) break;
            uint16_t c0 = 0, c1 = 0;
            uint32_t bits = 0;
            std::memcpy(&c0, dxt1_blocks.data() + blk_off, 2);
            std::memcpy(&c1, dxt1_blocks.data() + blk_off + 2, 2);
            std::memcpy(&bits, dxt1_blocks.data() + blk_off + 4, 4);

            uint8_t pal[4][4];
            unpack_565_u8(c0, pal[0]);
            unpack_565_u8(c1, pal[1]);
            if (c0 > c1) {
                for (int ch = 0; ch < 3; ++ch) {
                    pal[2][ch] = static_cast<uint8_t>((2u * pal[0][ch] + pal[1][ch] + 1u) / 3u);
                    pal[3][ch] = static_cast<uint8_t>((pal[0][ch] + 2u * pal[1][ch] + 1u) / 3u);
                }
                pal[2][3] = 255;
                pal[3][3] = 255;
            } else {
                for (int ch = 0; ch < 3; ++ch) {
                    pal[2][ch] = static_cast<uint8_t>((static_cast<uint32_t>(pal[0][ch]) + pal[1][ch]) >> 1);
                    pal[3][ch] = 0;
                }
                pal[2][3] = 255;
                pal[3][3] = 0;
            }

            for (int32_t ly = 0; ly < 4; ++ly) {
                int32_t py = by * 4 + ly;
                if (py >= h) break;
                for (int32_t lx = 0; lx < 4; ++lx) {
                    int32_t px = bx * 4 + lx;
                    if (px >= w) break;
                    uint32_t code = (bits >> (2 * (ly * 4 + lx))) & 3u;
                    uint8_t* dst = out_rgba.data() + (static_cast<size_t>(py) * w + px) * 4;
                    dst[0] = pal[code][0];
                    dst[1] = pal[code][1];
                    dst[2] = pal[code][2];
                    dst[3] = pal[code][3];
                }
            }
        }
    }
    return true;
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
        bone.name_lower = to_lower_str(bone.name);
        bone.flags = flags;
        bone.parent_index = (i == 0) ? -1 : parent_idx;
        bone.bind_pos = Vec3(px, py, pz);
        bone.bind_quat = Quat4(qx, qy, qz, qw).normalized();
        out_mesh.bone_name_to_index[bone.name_lower] = i;
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

        // Decode RigidVertices (49B each: Pos[12], TangentBasis[12], UVs[3*8=24], Bone[1])
        for (int32_t v = 0; v < rigid_cnt; ++v) {
            const uint8_t* vp = rigid_ptr + static_cast<size_t>(v) * 49;
            SkinnedVertex sv{};
            std::memcpy(&sv.bind_pos.x, vp + 0, 4);
            std::memcpy(&sv.bind_pos.y, vp + 4, 4);
            std::memcpy(&sv.bind_pos.z, vp + 8, 4);
            sv.bind_tangent = unpack_normal(vp + 12);
            sv.bind_norm = unpack_normal(vp + 20);
            float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f, u2 = 0.0f, v2 = 0.0f;
            std::memcpy(&u0, vp + 24, 4);
            std::memcpy(&v0, vp + 28, 4);
            std::memcpy(&u1, vp + 32, 4);
            std::memcpy(&v1, vp + 36, 4);
            std::memcpy(&u2, vp + 40, 4);
            std::memcpy(&v2, vp + 44, 4);
            // UE3 weapon materials bind CoordinateIndex=2 (UVs[2]) for Diffuse/Spec/Mask
            // and CoordinateIndex=1 (UVs[1], or UVs[0] on FNSCARL) for Normal Map T_<Weapon>_N.
            if (std::abs(u2) > 1e-6f || std::abs(v2) > 1e-6f) {
                sv.u = u2;
                sv.v = v2;
            } else {
                sv.u = u0;
                sv.v = v0;
            }
            if (std::abs(u1) > 1e-6f || std::abs(v1) > 1e-6f) {
                sv.un = u1;
                sv.vn = v1;
            } else {
                sv.un = u0;
                sv.vn = v0;
            }
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
            float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f, u2 = 0.0f, v2 = 0.0f;
            std::memcpy(&u0, vp + 24, 4);
            std::memcpy(&v0, vp + 28, 4);
            std::memcpy(&u1, vp + 32, 4);
            std::memcpy(&v1, vp + 36, 4);
            std::memcpy(&u2, vp + 40, 4);
            std::memcpy(&v2, vp + 44, 4);
            if (std::abs(u2) > 1e-6f || std::abs(v2) > 1e-6f) {
                sv.u = u2;
                sv.v = v2;
            } else {
                sv.u = u0;
                sv.v = v0;
            }
            if (std::abs(u1) > 1e-6f || std::abs(v1) > 1e-6f) {
                sv.un = u1;
                sv.vn = v1;
            } else {
                sv.un = u0;
                sv.vn = v0;
            }
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

    for (const auto& sec : out_mesh.sections) {
        uint8_t mtype = 0;
        std::string mlow = to_lower_str(sec.material_name);
        if (mlow.find("ammo") != std::string::npos) {
            mtype = 1;
        } else if (mlow.find("sight") != std::string::npos || mlow.find("lens") != std::string::npos) {
            mtype = 2;
        }
        size_t start_i = static_cast<size_t>(std::max(0, sec.base_index));
        size_t end_i = std::min(out_mesh.indices.size(), start_i + static_cast<size_t>(sec.num_triangles) * 3);
        for (size_t k = start_i; k < end_i; ++k) {
            uint16_t vi = out_mesh.indices[k];
            if (vi < out_mesh.vertices.size()) {
                out_mesh.vertices[vi].mat_type = mtype;
            }
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

// Faith's first-person skeleton, parsed once per game root.
static const SkeletalMeshAsset* first_person_skeleton(const std::string& game_root) {
    static std::mutex mutex;
    static std::unordered_map<std::string, SkeletalMeshAsset> cache;
    std::lock_guard<std::mutex> lock(mutex);
    auto it = cache.find(game_root);
    if (it == cache.end()) {
        SkeletalMeshAsset mesh;
        UPKPackage pkg(game_root + "/TdGame/CookedPC/Characters/CH_TKY_Crim_Fixer_1P.upk");
        if (pkg.is_valid()) {
            for (const auto& exp : pkg.get_exports()) {
                if (pkg.get_export_class(exp) == "SkeletalMesh" && exp.object_name == "SK_UpperBody") {
                    AnimSystem::parse_skeletal_mesh(pkg, exp, mesh);
                    break;
                }
            }
        }
        it = cache.emplace(game_root, std::move(mesh)).first;
    }
    return it->second.bones.empty() ? nullptr : &it->second;
}

int AnimSystem::count_first_person_tracks(const std::string& game_root, const AnimSetAsset& set, bool* has_eye_joint) {
    if (has_eye_joint) *has_eye_joint = false;
    const SkeletalMeshAsset* mesh = first_person_skeleton(game_root);
    if (!mesh) return 0;
    int count = 0;
    for (const std::string& bone : set.track_bone_names) {
        const std::string key = to_lower_str(bone);
        if (mesh->bone_name_to_index.count(key) == 0) continue;
        ++count;
        if (has_eye_joint && key == "eyejoint") *has_eye_joint = true;
    }
    return count;
}

bool AnimSystem::bake_canned_camera(const std::string& game_root, const AnimSetAsset& set, const AnimSequenceAsset& seq,
                                    std::vector<CannedCameraFrame>& out_frames, const AnimTrack* pawn_root) {
    out_frames.clear();
    const SkeletalMeshAsset* mesh = first_person_skeleton(game_root);
    if (!mesh || seq.tracks.empty()) return false;
    // The view is the CameraJoint, the EyeJoint's child (it faces the other way along the eye's axis).
    auto eye_it = mesh->bone_name_to_index.find("camerajoint");
    if (eye_it == mesh->bone_name_to_index.end()) eye_it = mesh->bone_name_to_index.find("eyejoint");
    if (eye_it == mesh->bone_name_to_index.end()) return false;
    const size_t eye = static_cast<size_t>(eye_it->second);

    const int frames = std::max(2, seq.num_frames);
    out_frames.resize(static_cast<size_t>(frames));
    std::vector<Vec3> local_pos, comp_pos;
    std::vector<Quat4> local_quat, comp_quat;
    for (int f = 0; f < frames; ++f) {
        // sample_sequence_pose wraps 1.0 back to the first key, as a looping animation wants.
        const float norm = std::min(static_cast<float>(f) / static_cast<float>(frames - 1), 0.999999f);
        sample_sequence_pose(*mesh, set, &seq, norm, local_pos, local_quat);
        if (pawn_root) {
            // The same keys-over-the-length reading sample_sequence_pose gives every track.
            if (const size_t n = pawn_root->positions.size(); n > 0) {
                const float fi = norm * static_cast<float>(n - 1);
                const size_t i0 = static_cast<size_t>(fi);
                const size_t i1 = std::min(i0 + 1, n - 1);
                local_pos[0] = pawn_root->positions[i0] + (pawn_root->positions[i1] - pawn_root->positions[i0]) * (fi - static_cast<float>(i0));
            }
            if (const size_t n = pawn_root->rotations.size(); n > 0) {
                const float fi = norm * static_cast<float>(n - 1);
                const size_t i0 = static_cast<size_t>(fi);
                const size_t i1 = std::min(i0 + 1, n - 1);
                local_quat[0] = Quat4::slerp(pawn_root->rotations[i0], pawn_root->rotations[i1], fi - static_cast<float>(i0));
            }
        }
        // UE3 stores every bone's rotation but the root's with W negated, so in the convention the
        // other tracks are read in, the root's is the inverse. Poses taken relative to a bone (the
        // first-person body around the eye) never notice; a pose placed in the world does.
        local_quat[0] = local_quat[0].conjugate();
        compute_skeleton_fk(mesh->bones, local_pos, local_quat, comp_pos, comp_quat);
        // The camera looks along its +Z with -Y up.
        CannedCameraFrame& out = out_frames[static_cast<size_t>(f)];
        out.eye_pos = comp_pos[eye];
        out.forward = comp_quat[eye].rotate(Vec3(0.0f, 0.0f, 1.0f));
        out.up = comp_quat[eye].rotate(Vec3(0.0f, -1.0f, 0.0f));
        out.root_pos = comp_pos[0];
    }
    return true;
}

bool AnimSystem::parse_single_anim_sequence(const UPKPackage& pkg, int32_t seq_export_index_1,
                                            AnimSetAsset& out_anim_set) {
    out_anim_set = AnimSetAsset{};
    const auto& exps = pkg.get_exports();
    const auto& data = pkg.get_data();
    if (seq_export_index_1 <= 0 || static_cast<size_t>(seq_export_index_1) > exps.size()) {
        return false;
    }
    const auto& exp = exps[seq_export_index_1 - 1];
    if (pkg.get_export_class(exp) != "AnimSequence") {
        return false;
    }

    // Extract TrackBoneNames from the sequence's exact owning AnimSet export (exp.outer_index)
    if (exp.outer_index > 0 && static_cast<size_t>(exp.outer_index) <= exps.size()) {
        const auto& set_exp = exps[exp.outer_index - 1];
        size_t s_prop_start = pkg.find_property_start(set_exp);
        auto s_props = pkg.parse_properties(s_prop_start, set_exp.serial_size);
        auto it_tbn = s_props.find("TrackBoneNames");
        if (it_tbn != s_props.end() && it_tbn->second.raw_bytes.size() >= 4) {
            const auto& raw = it_tbn->second.raw_bytes;
            const uint8_t* ptr = raw.data();
            const uint8_t* end = raw.data() + raw.size();
            int32_t count = read_le<int32_t>(ptr, end);
            if (count > 0 && count <= 512 && ptr + count * 8 <= end) {
                out_anim_set.name = set_exp.object_name;
                out_anim_set.track_bone_names.resize(count);
                for (int32_t i = 0; i < count; ++i) {
                    int32_t n_idx = read_le<int32_t>(ptr, end);
                    int32_t n_num = read_le<int32_t>(ptr, end);
                    std::string bname = read_fname_str(pkg, n_idx, n_num);
                    out_anim_set.track_bone_names[i] = bname;
                    out_anim_set.bone_to_track[to_lower_str(bname)] = i;
                }
            }
        }
    }

    size_t prop_start = pkg.find_property_start(exp);
    size_t bytes_read = 0;
    auto props = pkg.parse_properties(prop_start, exp.serial_size, &bytes_read);
    auto it_offs = props.find("CompressedTrackOffsets");
    if (it_offs == props.end() || it_offs->second.raw_bytes.size() < 4) return false;

    const auto& raw_offs = it_offs->second.raw_bytes;
    const uint8_t* op = raw_offs.data();
    const uint8_t* oend = raw_offs.data() + raw_offs.size();
    int32_t num_ints = read_le<int32_t>(op, oend);
    if (num_ints <= 0 || (num_ints % 4) != 0 || op + num_ints * 4 > oend) return false;
    size_t num_tracks = static_cast<size_t>(num_ints / 4);
    std::vector<int32_t> track_offs(num_ints);
    for (int32_t i = 0; i < num_ints; ++i) {
        track_offs[i] = read_le<int32_t>(op, oend);
    }

    size_t tail_off = prop_start + bytes_read;
    size_t exp_end = static_cast<size_t>(exp.serial_offset) + static_cast<size_t>(exp.serial_size);
    if (tail_off + 4 > exp_end || exp_end > data.size()) return false;
    const uint8_t* tail_ptr = data.data() + tail_off;
    const uint8_t* tail_end = data.data() + exp_end;
    int32_t stream_size = read_le<int32_t>(tail_ptr, tail_end);
    if (stream_size <= 0 || tail_ptr + stream_size > tail_end) return false;
    const uint8_t* stream = tail_ptr;

    AnimSequenceAsset seq{};
    seq.name = props.count("SequenceName") ? props["SequenceName"].str_val : exp.object_name;
    seq.length = props.count("SequenceLength") ? props["SequenceLength"].float_val : 1.0f;
    seq.num_frames = props.count("NumFrames") ? props["NumFrames"].int_val : 1;
    seq.rate_scale = props.count("RateScale") ? props["RateScale"].float_val : 1.0f;
    if (props.count("TranslationCompressionFormat")) seq.trans_compression = props["TranslationCompressionFormat"].str_val;
    if (props.count("RotationCompressionFormat")) seq.rot_compression = props["RotationCompressionFormat"].str_val;
    seq.tracks.resize(num_tracks);
    const bool is_float96 = (seq.rot_compression == "ACF_Float96NoW");

    for (size_t t = 0; t < num_tracks; ++t) {
        int32_t trans_off  = track_offs[t * 4 + 0];
        int32_t trans_keys = track_offs[t * 4 + 1];
        int32_t rot_off    = track_offs[t * 4 + 2];
        int32_t rot_keys   = track_offs[t * 4 + 3];
        AnimTrack& dst = seq.tracks[t];
        if (trans_keys > 0 && trans_off >= 0 && trans_off + trans_keys * 12 <= stream_size) {
            dst.positions.resize(trans_keys);
            for (int32_t k = 0; k < trans_keys; ++k) {
                const uint8_t* kp = stream + trans_off + k * 12;
                float px = 0.0f, py = 0.0f, pz = 0.0f;
                std::memcpy(&px, kp + 0, 4);
                std::memcpy(&py, kp + 4, 4);
                std::memcpy(&pz, kp + 8, 4);
                dst.positions[k] = Vec3(px, py, pz);
            }
        }
        if (rot_keys == 1 && rot_off >= 0 && rot_off + 12 <= stream_size) {
            const uint8_t* kp = stream + rot_off;
            float rx = 0.0f, ry = 0.0f, rz = 0.0f;
            std::memcpy(&rx, kp + 0, 4);
            std::memcpy(&ry, kp + 4, 4);
            std::memcpy(&rz, kp + 8, 4);
            float rw = std::sqrt(std::max(0.0f, 1.0f - (rx * rx + ry * ry + rz * rz)));
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

        parse_dxt1_texture(pkg_1p, "Female_1p_C", faith_skin_tex_);
        parse_dxt1_texture(pkg_1p, "Faith_Glove_C", faith_glove_tex_);
        const DXT1Texture& tex_skin = faith_skin_tex_;
        const DXT1Texture& tex_glove = faith_glove_tex_;

        for (auto& v : faith_upper_.vertices) {
            if (v.chunk_index == 0 && tex_skin.is_valid()) {
                Vec3 c = tex_skin.sample_rgb01(v.u, v.v);
                c.x = std::clamp(std::pow(c.x, 0.85f) * 1.06f, 0.15f, 0.96f);
                c.y = std::clamp(std::pow(c.y, 0.88f) * 1.02f, 0.12f, 0.88f);
                c.z = std::clamp(std::pow(c.z, 0.90f) * 0.98f, 0.10f, 0.82f);
                v.color = pack_rgba8(c.x, c.y, c.z);
            } else if (tex_glove.is_valid()) {
                Vec3 c = tex_glove.sample_rgb01(v.u, v.v);
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
        DXT1Texture tex_upper{};
        parse_dxt1_texture(pkg_cine, "Faith_Cine_Lower_C", faith_lower_tex_);
        parse_dxt1_texture(pkg_cine, "Faith_Cine_Upper_C", tex_upper);
        const DXT1Texture& tex_lower = faith_lower_tex_;

        for (auto& v : faith_lower_.vertices) {
            if (v.chunk_index == 1 && tex_lower.is_valid()) {
                Vec3 c = tex_lower.sample_rgb01(v.u, v.v);
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

    // 3. Load KrugerSec / CPF / Runner / Celeste Enemy Character Skeletal Meshes & Textures
    struct EnemyCharacterSpec {
        EnemyArchetypeId id;
        const char* upk_file;
        const char* skel_name;
        const char* tex_d;
        const char* tex_s;
        const char* tex_n;
    };
    static const EnemyCharacterSpec kEnemySpecs[] = {
        {EnemyArch_SWAT,    "Characters/CH_TKY_Cop_SWAT.upk",    "CH_TKY_Cop_SWAT",       "T_TKY_Cop_SWAT_D",      "T_TKY_Cop_SWAT_S",      "T_TKY_Cop_SWAT_N"},
        {EnemyArch_Patrol,  "Characters/CH_TKY_Cop_Patrol.upk",  "SK_TKY_Cop_Patrol_PK",  "T_TKY_CopPatrol_02_D",  "T_TKY_CopPatrol_01_S",  "T_TKY_CopPatrol_01_N"},
        {EnemyArch_Support, "Characters/CH_TKY_Cop_Support.upk", "SK_TKY_Cop_Support",    "T_TKY_CopSupport_01_D", "T_TKY_CopSupport_01_S", "T_TKY_CopSupport_01_N"},
        {EnemyArch_Riot,    "Characters/CH_TKY_Cop_Riot.upk",    "SK_TKY_Cop_Riot",       "T_TKY_CopRiot_01_D",    "T_TKY_CopRiot_01_S",    "T_TKY_CopRiot_01_N"},
        {EnemyArch_Pursuit, "Characters/CH_TKY_Cop_Pursuit.upk", "SK_TKY_Cop_Pursuit",    "T_CopPursuit01_D",      "T_CopPursuit01_S",      "T_CopPursuit01_N"},
        {EnemyArch_Celeste, "Characters/CH_Celeste.upk",         "SK_Celeste",            "Celeste_Merged_D",      "Celeste_Merged_S_2k",   "Celeste_Merged_N"}
    };
    for (const auto& es : kEnemySpecs) {
        UPKPackage pkg_e(cooked + es.upk_file);
        if (!pkg_e.is_valid()) continue;
        EnemyCharacterModel& mdl = enemy_models_[es.id];
        for (const auto& exp : pkg_e.get_exports()) {
            if (pkg_e.get_export_class(exp) == "SkeletalMesh" && exp.object_name == es.skel_name) {
                parse_skeletal_mesh(pkg_e, exp, mdl.mesh);
                break;
            }
        }
        parse_dxt1_texture(pkg_e, es.tex_d, mdl.tex_diffuse);
        parse_dxt1_texture(pkg_e, es.tex_s, mdl.tex_specular);
        parse_dxt1_texture(pkg_e, es.tex_n, mdl.tex_normal);
        mdl.mesh.tex_diffuse = mdl.tex_diffuse;
        mdl.mesh.tex_specular = mdl.tex_specular;
        mdl.mesh.tex_normal = mdl.tex_normal;
        mdl.chunk_vert_counts.clear();
        for (const auto& v : mdl.mesh.vertices) {
            if (v.chunk_index >= mdl.chunk_vert_counts.size()) {
                mdl.chunk_vert_counts.resize(static_cast<size_t>(v.chunk_index) + 1, 0);
            }
            mdl.chunk_vert_counts[v.chunk_index]++;
        }
    }
    swat_mesh_ = enemy_models_[EnemyArch_SWAT].mesh;
    swat_diffuse_tex_ = enemy_models_[EnemyArch_SWAT].tex_diffuse;
    swat_specular_tex_ = enemy_models_[EnemyArch_SWAT].tex_specular;
    swat_normal_tex_ = enemy_models_[EnemyArch_SWAT].tex_normal;


    // Load KrugerSec / CPF SWAT Blackhawk Helicopter Skeletal Mesh & Diffuse Texture (Vehicles/SWAT_Blackhawk.upk)
    {
        UPKPackage pkg_heli(cooked + "Vehicles/SWAT_Blackhawk.upk");
        if (pkg_heli.is_valid()) {
            for (const auto& exp : pkg_heli.get_exports()) {
                if (pkg_heli.get_export_class(exp) == "SkeletalMesh" &&
                    exp.object_name == "SK_SWAT_Blackhawk_01") {
                    parse_skeletal_mesh(pkg_heli, exp, heli_mesh_);
                    break;
                }
            }
            parse_dxt1_texture(pkg_heli, "T_blackhawk_outside_D", heli_diffuse_tex_);
            if (heli_diffuse_tex_.is_valid()) {
                heli_mesh_.tex_diffuse = heli_diffuse_tex_;
                for (auto& v : heli_mesh_.vertices) {
                    Vec3 c = heli_diffuse_tex_.sample_rgb01(v.u, v.v);
                    v.color = pack_rgba8(c.x, c.y, c.z);
                }
            }
        }
    }

    // Load shared brass cartridge texture from Weapons/WP_Ammo.upk (used by M_Ammo sections on Glock18/FNSCARL/FNMinimi)
    {
        UPKPackage pkg_ammo(cooked + "Weapons/WP_Ammo.upk");
        if (pkg_ammo.is_valid()) {
            for (const auto& exp : pkg_ammo.get_exports()) {
                if (pkg_ammo.get_export_class(exp) == "Texture2D" &&
                    (exp.object_name.find("_D") != std::string::npos || exp.object_name.find("Ammo") != std::string::npos)) {
                    if (parse_dxt1_texture(pkg_ammo, exp.object_name, ammo_diffuse_tex_) && ammo_diffuse_tex_.is_valid()) {
                        if (exp.object_name.find("_D") != std::string::npos) break;
                    }
                }
            }
        }
    }

    // 4. Load Full 11-Weapon Retail Arsenal (CookedPC/Weapons/WP_*.upk)
    struct WeaponPackageSpec {
        const char* key;
        const char* upk_file;
        const char* skel_name;
        const char* tex_d;
        const char* tex_s;
        const char* tex_n;
    };
    static const WeaponPackageSpec kWeaponSpecs[] = {
        {"Colt1911",     "Weapons/WP_Colt1911.upk",     "SK_Colt1911",     "T_Colt1911_D",     "T_Colt1911_S",     "T_Colt1911_N"},
        {"Glock18",      "Weapons/WP_Glock18.upk",      "SK_Glock18",      "T_Glock18_D",      "T_Glock18_S",      "T_Glock18_N"},
        {"BerettaM93R",  "Weapons/WP_BerettaM93R.upk",  "SK_BerettaM93R",  "T_BerettaM93R_D",  "T_BerettaM93R_S",  "T_BerettaM93R_N"},
        {"SteyrTMP",     "Weapons/WP_SteyrTMP.upk",     "SK_SteyrTMP",     "T_SteyrTMP_D",     "T_SteyrTMP_S",     "T_SteyrTMP_N"},
        {"MP5K",         "Weapons/WP_MP5K.upk",         "SK_MP5K",         "T_MP5K_D",         "T_MP5K_S",         "T_MP5K_N"},
        {"G36C",         "Weapons/WP_G36C.upk",         "SK_G36C",         "T_G36C_D",         "T_G36C_S",         "T_G36C_N"},
        {"FNSCARL",      "Weapons/WP_FNSCARL.upk",      "SK_FNSCARL",      "T_FNSCARL_D",      "T_FNSCARL_S",      "T_FNSCARL_N"},
        {"Remington870", "Weapons/WP_Remington870.upk", "SK_Remington870", "T_Remington870_D", "T_Remington870_S", "T_Remington870_N"},
        {"Neostead",     "Weapons/WP_Neostead.upk",     "SK_Neostead",     "T_Neostead_D",     "T_Neostead_S",     "T_Neostead_N"},
        {"FNMinimi",     "Weapons/WP_FNMinimi.upk",     "SK_FNMinimi",     "T_FNMinimi_D",     "T_FNMinimi_S",     "T_FNMinimi_N"},
        {"M95",          "Weapons/WP_M95.upk",          "SK_M95",          "T_M95_D",          "T_M95_S",          "T_M95_N"}
    };

    for (const auto& ws : kWeaponSpecs) {
        UPKPackage pkg_w(cooked + ws.upk_file);
        if (!pkg_w.is_valid()) continue;
        SkeletalMeshAsset w_mesh{};
        for (const auto& exp : pkg_w.get_exports()) {
            if (pkg_w.get_export_class(exp) == "SkeletalMesh" && exp.object_name == ws.skel_name) {
                parse_skeletal_mesh(pkg_w, exp, w_mesh);
                break;
            }
        }
        if (!w_mesh.is_valid()) continue;

        parse_dxt1_texture(pkg_w, ws.tex_d, w_mesh.tex_diffuse);
        parse_dxt1_texture(pkg_w, ws.tex_s, w_mesh.tex_specular);
        parse_dxt1_texture(pkg_w, ws.tex_n, w_mesh.tex_normal);
        std::string tex_m = std::string("T_") + ws.key + "_M";
        parse_dxt1_texture(pkg_w, tex_m, w_mesh.tex_mask);

        for (auto& v : w_mesh.vertices) {
            v.color = 0xFFFFFFFF;
        }
        if (std::string(ws.key) == "Colt1911") {
            colt1911_mesh_ = w_mesh;
        }
        weapon_meshes_[ws.key] = std::move(w_mesh);
    }

    // 5. Load Animation Sets (Unarmed, 1H Common + Weapon Sets, 2H Common + Weapon Sets, AI 1H & 2H)
    UPKPackage pkg_as_unarmed(cooked + "Animations/AS_C1P_Unarmed.upk");
    if (pkg_as_unarmed.is_valid()) parse_anim_set_package(pkg_as_unarmed, faith_unarmed_set_);

    UPKPackage pkg_as_common(cooked + "Animations/AS_C1P_OneHanded_Common.upk");
    if (pkg_as_common.is_valid()) parse_anim_set_package(pkg_as_common, faith_common_set_);

    UPKPackage pkg_as_2h_common(cooked + "Animations/AS_C1P_TwoHanded_Common.upk");
    if (pkg_as_2h_common.is_valid()) parse_anim_set_package(pkg_as_2h_common, faith_2h_common_set_);

    UPKPackage pkg_as_colt(cooked + "Animations/AS_C1P_OneHanded_Colt1911.upk");
    if (pkg_as_colt.is_valid()) {
        parse_anim_set_package(pkg_as_colt, faith_colt_set_);
        faith_weapon_sets_["Colt1911"] = faith_colt_set_;
    }

    struct WeaponAnimSpec {
        const char* key;
        const char* upk_file;
    };
    static const WeaponAnimSpec kWeaponAnimSpecs[] = {
        {"Glock18",      "Animations/AS_C1P_OneHanded_Glock18.upk"},
        {"BerettaM93R",  "Animations/AS_C1P_OneHanded_BerettaM93R.upk"},
        {"SteyrTMP",     "Animations/AS_C1P_OneHanded_SteyrTMP.upk"},
        {"G36C",         "Animations/AS_C1P_TwoHanded_G36C.upk"},
        {"Remington870", "Animations/AS_C1P_TwoHanded_Remington.upk"},
        {"Neostead",     "Animations/AS_C1P_TwoHanded_Neostead.upk"},
        {"FNSCARL",      "Animations/AS_C1P_TwoHanded_FNSCARL.upk"},
        {"MP5K",         "Animations/AS_C1P_TwoHanded_MP5K.upk"},
        {"FNMinimi",     "Animations/AS_C1P_TwoHanded_FNMinimi.upk"},
        {"M95",          "Animations/AS_C1P_TwoHanded_M95.upk"}
    };
    for (const auto& was : kWeaponAnimSpecs) {
        UPKPackage pkg_wa(cooked + was.upk_file);
        if (pkg_wa.is_valid()) {
            AnimSetAsset aset{};
            if (parse_anim_set_package(pkg_wa, aset)) {
                faith_weapon_sets_[was.key] = std::move(aset);
            }
        }
    }

    UPKPackage pkg_as_swat(cooked + "Animations/AS_AI_PatrolCop_OneHanded.upk");
    if (pkg_as_swat.is_valid()) parse_anim_set_package(pkg_as_swat, swat_set_);

    UPKPackage pkg_as_swat_2h(cooked + "Animations/AS_AI_Assault_TwoHanded.upk");
    if (pkg_as_swat_2h.is_valid()) parse_anim_set_package(pkg_as_swat_2h, swat_2h_set_);

    UPKPackage pkg_as_celeste(cooked + "Animations/AS_AI_Celeste_Unarmed.upk");
    if (pkg_as_celeste.is_valid()) parse_anim_set_package(pkg_as_celeste, celeste_set_);

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
                  << "  - Weapon SkeletalMeshes loaded: " << weapon_meshes_.size() << " ("
                  << "Colt1911, Glock18, BerettaM93R, SteyrTMP, MP5K, G36C, FNSCARL, Remington870, Neostead, FNMinimi, M95)\n"
                  << "  - AS_C1P_Unarmed: " << faith_unarmed_set_.sequences.size() << " sequences\n"
                  << "  - AS_C1P_OneHanded_Common: " << faith_common_set_.sequences.size() << " sequences\n"
                  << "  - AS_C1P_TwoHanded_Common: " << faith_2h_common_set_.sequences.size() << " sequences\n"
                  << "  - Weapon-specific 1P AnimSets: " << faith_weapon_sets_.size() << " sets\n"
                  << "  - AS_AI_PatrolCop_OneHanded: " << swat_set_.sequences.size() << " sequences\n"
                  << "  - AS_AI_Assault_TwoHanded: " << swat_2h_set_.sequences.size() << " sequences\n"
                  << "  - DefaultAnimation.ini CustomAnimNodes: " << blend_configs_.size() << std::endl;
    }

    build_enemy_swat_index_lists();
    return loaded_;
}

const SkeletalMeshAsset* AnimSystem::get_weapon_mesh(const std::string& weapon_name) const {
    if (weapon_name.empty() || weapon_name == "None" || weapon_name == "Unarmed") return nullptr;
    auto it = weapon_meshes_.find(weapon_name);
    if (it != weapon_meshes_.end()) return &it->second;
    std::string low = to_lower_str(weapon_name);
    if (low.find("g36") != std::string::npos) {
        it = weapon_meshes_.find("G36C");
    } else if (low.find("remington") != std::string::npos || low.find("870") != std::string::npos) {
        it = weapon_meshes_.find("Remington870");
    } else if (low.find("neostead") != std::string::npos) {
        it = weapon_meshes_.find("Neostead");
    } else if (low.find("scar") != std::string::npos) {
        it = weapon_meshes_.find("FNSCARL");
    } else if (low.find("mp5") != std::string::npos) {
        it = weapon_meshes_.find("MP5K");
    } else if (low.find("minimi") != std::string::npos || low.find("m249") != std::string::npos) {
        it = weapon_meshes_.find("FNMinimi");
    } else if (low.find("m95") != std::string::npos || low.find("barret") != std::string::npos) {
        it = weapon_meshes_.find("M95");
    } else if (low.find("glock") != std::string::npos) {
        it = weapon_meshes_.find("Glock18");
    } else if (low.find("beretta") != std::string::npos || low.find("m93") != std::string::npos) {
        it = weapon_meshes_.find("BerettaM93R");
    } else if (low.find("tmp") != std::string::npos || low.find("steyr") != std::string::npos) {
        it = weapon_meshes_.find("SteyrTMP");
    }
    if (it != weapon_meshes_.end()) return &it->second;
    return colt1911_mesh_.is_valid() ? &colt1911_mesh_ : nullptr;
}

namespace {

// append_muzzle_flash_mesh() always emits 4 fins x 2 double-sided triangles = 48 corners.
constexpr size_t kMuzzleFlashVertexCount = 48;

// Helper to append a 3D 4-fin muzzle flash star cone at `tip_pos` pointing along `forward_dir`
void append_muzzle_flash_mesh(std::vector<Vertex>& out_tris, const Vec3& tip_pos,
                              const Vec3& forward_dir, const Vec3& right_dir, const Vec3& up_dir,
                              float radius, float length) {
    uint32_t core_col = pack_rgba8(1.0f, 0.96f, 0.72f);
    uint32_t outer_col = pack_rgba8(1.0f, 0.58f, 0.14f);
    Vec3 apex = tip_pos + forward_dir * length;

    auto add_tri = [&](const Vec3& a, const Vec3& b, const Vec3& c, uint32_t col_a, uint32_t col_bc) {
        Vec3 n = (b - a).cross(c - a).normalized();
        Vertex va{a, n, {1, 0, 0}, 0.5f, 0.5f, 0.0f, 0.0f, col_a};
        Vertex vb{b, n, {1, 0, 0}, 1.0f, 0.0f, 0.0f, 0.0f, col_bc};
        Vertex vc{c, n, {1, 0, 0}, 0.0f, 1.0f, 0.0f, 0.0f, col_bc};
        out_tris.push_back(va); out_tris.push_back(vb); out_tris.push_back(vc);
        // Backface so flash is visible from all angles
        Vertex vb2 = vb; vb2.normal = n * -1.0f;
        Vertex vc2 = vc; vc2.normal = n * -1.0f;
        Vertex va2 = va; va2.normal = n * -1.0f;
        out_tris.push_back(va2); out_tris.push_back(vc2); out_tris.push_back(vb2);
    };

    const Vec3 dirs[4] = {
        right_dir,
        up_dir,
        (right_dir + up_dir).normalized(),
        (right_dir - up_dir).normalized()
    };
    for (const Vec3& d : dirs) {
        Vec3 p_pos = tip_pos + d * radius;
        Vec3 p_neg = tip_pos - d * radius;
        add_tri(tip_pos, p_pos, apex, core_col, outer_col);
        add_tri(tip_pos, apex, p_neg, core_col, outer_col);
    }
}

bool is_heavy_weapon_name(const std::string& wname) {
    std::string low = to_lower_str(wname);
    return (low.find("g36") != std::string::npos ||
            low.find("scar") != std::string::npos ||
            low.find("mp5") != std::string::npos ||
            low.find("remington") != std::string::npos ||
            low.find("870") != std::string::npos ||
            low.find("neostead") != std::string::npos ||
            low.find("minimi") != std::string::npos ||
            low.find("m95") != std::string::npos ||
            low.find("barret") != std::string::npos);
}

} // namespace

// -----------------------------------------------------------------------------
// Evaluate Faith's 1P Skeletal Viewmodel (AT_C1P Blend Tree + Linear Blend Skinning)
// -----------------------------------------------------------------------------
void AnimSystem::evaluate_faith_1p(const PlayerTelemetry& telemetry, std::vector<Vertex>& out_triangles) const {
    out_triangles.clear();
    if (!loaded_ || !faith_upper_.is_valid()) return;

    const AnimSetAsset* active_set = &faith_unarmed_set_;
    const AnimSetAsset* set_b_ptr = &faith_unarmed_set_;
    const AnimSequenceAsset* seq_a = nullptr;
    const AnimSequenceAsset* seq_b = nullptr;
    float blend_alpha = 0.0f;
    float norm_time = 0.0f;

    // Viewmodel camera framing offset (places Faith's 1P forearms/hands in lower peripheral frustum)
    Vec3 vm_offset(0.0f, 16.0f, 14.0f);
    bool show_lower_body = false;
    bool lower_body_high_kick = false;

    const float sim_t = telemetry.sim_time;
    const float speed = telemetry.speed_2d;
    const EMovement state = telemetry.move_state;
    const float combat_progress = std::clamp(
        telemetry.combat_anim_time / std::max(0.10f, telemetry.combat_anim_duration), 0.0f, 1.0f);

    // Helper to resolve a sequence across weapon-specific -> 1H/2H common -> unarmed sets
    auto resolve_seq = [&](const AnimSetAsset* pri, const AnimSetAsset* com,
                           const std::string& seq_name, const AnimSetAsset** out_owner) -> const AnimSequenceAsset* {
        if (pri) {
            if (const auto* s = pri->find_sequence(seq_name)) {
                if (out_owner) *out_owner = pri;
                return s;
            }
        }
        if (com) {
            if (const auto* s = com->find_sequence(seq_name)) {
                if (out_owner) *out_owner = com;
                return s;
            }
        }
        if (const auto* s = faith_unarmed_set_.find_sequence(seq_name)) {
            if (out_owner) *out_owner = &faith_unarmed_set_;
            return s;
        }
        return nullptr;
    };

    const AnimSetAsset* w_spec_set = nullptr;
    const AnimSetAsset* w_comm_set = &faith_common_set_;
    if (telemetry.weapon.equipped || state == EMovement::MOVE_Snatch) {
        auto it_ws = faith_weapon_sets_.find(telemetry.weapon.name);
        if (it_ws != faith_weapon_sets_.end()) w_spec_set = &it_ws->second;
        bool heavy = telemetry.weapon.is_heavy || is_heavy_weapon_name(telemetry.weapon.name);
        w_comm_set = (heavy && !faith_2h_common_set_.sequences.empty())
                         ? &faith_2h_common_set_
                         : &faith_common_set_;
    }

    // 0. Cooked Matinee level intro sequence override (e.g. sp01_intro..sp09_intro)
    if (telemetry.intro_active && telemetry.intro_anim_exp_1 > 0 && !telemetry.intro_pkg_path.empty()) {
        std::string cache_key = telemetry.intro_pkg_path + "#" + std::to_string(telemetry.intro_anim_exp_1);
        auto it_intro = level_intro_sets_.find(cache_key);
        if (it_intro == level_intro_sets_.end()) {
            UPKPackage intro_pkg(telemetry.intro_pkg_path);
            AnimSetAsset parsed_set{};
            if (intro_pkg.is_valid() && parse_single_anim_sequence(intro_pkg, telemetry.intro_anim_exp_1, parsed_set)) {
                it_intro = level_intro_sets_.emplace(cache_key, std::move(parsed_set)).first;
            }
        }
        if (it_intro != level_intro_sets_.end()) {
            const AnimSequenceAsset* intro_seq = it_intro->second.find_sequence(telemetry.intro_anim_name);
            if (!intro_seq && !it_intro->second.sequences.empty()) {
                intro_seq = &it_intro->second.sequences.begin()->second;
            }
            if (intro_seq && intro_seq->length > 0.0f) {
                active_set = &it_intro->second;
                seq_a = intro_seq;
                norm_time = std::clamp(telemetry.intro_anim_time / intro_seq->length, 0.0f, 1.0f);
                vm_offset = Vec3(0.0f, 10.0f, 12.0f);
                show_lower_body = true;
            }
        }
    } else if (telemetry.fall_death_impact) {
        // Lethal fall ground impact / blackout: play UE3 1P death landing sequence (FallingLandDie)
        seq_a = resolve_seq(w_spec_set, w_comm_set, "FallingLandDie", &active_set);
        if (!seq_a) seq_a = resolve_seq(nullptr, nullptr, "fallinglandhard", &active_set);
        norm_time = std::clamp(telemetry.death_anim_progress, 0.0f, 0.98f);
        vm_offset = Vec3(0.0f, 12.0f, 10.0f);
        show_lower_body = true;
    } else if (telemetry.falling_to_death) {
        // Uncontrolled lethal freefall: play UE3 1P flailing arms/body sequence (fallinguncontrolled)
        seq_a = resolve_seq(w_spec_set, w_comm_set, "fallinguncontrolled", &active_set);
        if (!seq_a) seq_a = resolve_seq(nullptr, nullptr, "jumpfast", &active_set);
        norm_time = std::fmod(sim_t * 2.2f, 1.0f);
        vm_offset = Vec3(0.0f, 14.0f, 10.0f);
        show_lower_body = true;
    } else if (state == EMovement::MOVE_Snatch) {
        const char* snatch_seq = telemetry.snatch_from_back ? "SnatchBack" : "SnatchFwd";
        seq_a = resolve_seq(w_spec_set, w_comm_set, snatch_seq, &active_set);
        if (!seq_a) seq_a = resolve_seq(w_spec_set, &faith_common_set_, "SnatchFwd", &active_set);
        norm_time = std::clamp(0.15f + combat_progress * 0.75f, 0.0f, 0.96f);
        float lunge = std::sin(combat_progress * PI);
        vm_offset = Vec3(0.0f, 14.0f + lunge * 5.0f, 5.0f + lunge * 3.0f);
    } else if (state == EMovement::MOVE_Melee) {
        float strike_arc = std::sin(combat_progress * PI);
        norm_time = combat_progress;
        if (telemetry.melee_variant == 3) {
            // Crouch Sweep / Uppercut
            seq_a = resolve_seq(nullptr, nullptr, "MeleeCrouchHitUppercut", &active_set);
            seq_b = resolve_seq(nullptr, nullptr, "MeleeCrouchStartUpperCut", &set_b_ptr);
            blend_alpha = (combat_progress < 0.25f) ? (1.0f - combat_progress * 4.0f) : 0.0f;
            vm_offset = Vec3(-2.0f, 12.0f + strike_arc * 10.0f, 10.0f + strike_arc * 8.0f);
        } else if (telemetry.melee_variant == 2) {
            // Standing High Kick (shows lower body boot kicking across foreground!)
            seq_a = resolve_seq(nullptr, nullptr, "meleekickobject", &active_set);
            if (!seq_a) seq_a = resolve_seq(nullptr, nullptr, "MeleeHit2Right", &active_set);
            vm_offset = Vec3(0.0f, 12.0f + strike_arc * 8.0f, 12.0f);
            show_lower_body = true;
            lower_body_high_kick = true;
        } else if (telemetry.melee_variant == 1) {
            // Left Hook / Jab
            const char* hit_name = telemetry.melee_hit_confirmed ? "MeleeHitLeft" : "MeleeMissedLeft";
            seq_a = resolve_seq(nullptr, nullptr, hit_name, &active_set);
            seq_b = resolve_seq(nullptr, nullptr, "MeleeStartLeft", &set_b_ptr);
            blend_alpha = (combat_progress < 0.20f) ? (1.0f - combat_progress * 5.0f) : 0.0f;
            vm_offset = Vec3(4.0f - strike_arc * 6.0f, 10.0f + strike_arc * 12.0f, 12.0f + strike_arc * 5.0f);
        } else {
            // Right Cross / Hook (Red Runner Glove punch)
            const char* hit_name = telemetry.melee_hit_confirmed ? "MeleeHitRight" : "MeleeMissedRight";
            seq_a = resolve_seq(nullptr, nullptr, hit_name, &active_set);
            seq_b = resolve_seq(nullptr, nullptr, "MeleeStartRight", &set_b_ptr);
            blend_alpha = (combat_progress < 0.20f) ? (1.0f - combat_progress * 5.0f) : 0.0f;
            vm_offset = Vec3(-4.0f + strike_arc * 6.0f, 10.0f + strike_arc * 12.0f, 12.0f + strike_arc * 5.0f);
        }
    } else if (state == EMovement::MOVE_MeleeAir) {
        // Flying Jump Kick (FirstPersonLowerBodyDPG = SDPG_Foreground)
        const char* air_name = telemetry.melee_hit_confirmed ? "MeleeInAirHit" : "MeleeInAir";
        seq_a = resolve_seq(nullptr, nullptr, air_name, &active_set);
        seq_b = resolve_seq(nullptr, nullptr, "MeleeInAir2", &set_b_ptr);
        norm_time = std::clamp(0.15f + combat_progress * 0.75f, 0.0f, 0.95f);
        blend_alpha = 0.25f;
        vm_offset = Vec3(0.0f, 10.0f, 12.0f);
        show_lower_body = true;
        lower_body_high_kick = true;
    } else if (state == EMovement::MOVE_MeleeSlide) {
        // Crouch Slide Kick
        seq_a = resolve_seq(nullptr, nullptr, "MeleeSlideHard", &active_set);
        if (!seq_a) seq_a = resolve_seq(nullptr, nullptr, "MeleeSlide", &active_set);
        seq_b = resolve_seq(nullptr, nullptr, "CrouchSlide", &set_b_ptr);
        norm_time = std::clamp(0.15f + combat_progress * 0.75f, 0.0f, 0.95f);
        blend_alpha = 0.30f;
        vm_offset = Vec3(0.0f, 6.0f, 14.0f);
        show_lower_body = true;
    } else if (state == EMovement::MOVE_MeleeWallrun) {
        // Wallrun Spinning Kick
        const char* wr_name = (telemetry.melee_variant == 6) ? "MeleeWallRunLeft" : "MeleeWallRunRight";
        seq_a = resolve_seq(nullptr, nullptr, wr_name, &active_set);
        norm_time = std::clamp(0.15f + combat_progress * 0.75f, 0.0f, 0.95f);
        vm_offset = Vec3(0.0f, 10.0f, 12.0f);
        show_lower_body = true;
        lower_body_high_kick = true;
    } else if (state == EMovement::MOVE_Barge) {
        seq_a = resolve_seq(nullptr, nullptr, "bargeinleft", &active_set);
        seq_b = resolve_seq(nullptr, nullptr, "MeleeHitShove", &set_b_ptr);
        norm_time = combat_progress;
        blend_alpha = 0.35f;
        vm_offset = Vec3(2.0f, 10.0f, 10.0f);
    } else if (telemetry.weapon.equipped) {
        // 2. Armed 1P Animation Tree (TdAnimNodeBlendByArmed + TdAnimNodeBlendByFire + TdAnimNodeWeaponPose)
        bool heavy = telemetry.weapon.is_heavy || is_heavy_weapon_name(telemetry.weapon.name);
        Vec3 base_armed_offset = heavy ? Vec3(2.0f, 15.0f, 3.5f) : Vec3(2.0f, 11.0f, 3.5f);

        if (telemetry.weapon.drop_timer > 0.0f) {
            seq_a = resolve_seq(w_spec_set, w_comm_set, "throwaway", &active_set);
            norm_time = std::clamp(1.0f - (telemetry.weapon.drop_timer / 0.35f), 0.0f, 1.0f);
            vm_offset = base_armed_offset + Vec3(0.0f, 3.0f * norm_time, -4.0f * norm_time);
        } else if (telemetry.weapon.equip_timer > 0.0f) {
            float eq_dur = heavy ? 0.65f : 0.45f;
            seq_a = resolve_seq(w_spec_set, w_comm_set, "unholster", &active_set);
            if (!seq_a) seq_a = resolve_seq(w_spec_set, w_comm_set, "standfire", &active_set);
            norm_time = std::clamp(1.0f - (telemetry.weapon.equip_timer / eq_dur), 0.0f, 1.0f);
            vm_offset = base_armed_offset;
        } else if (telemetry.weapon.fire_anim_timer > 0.0f) {
            const char* fire_name = (telemetry.weapon.ammo == 0) ? "standfireempty" : "standfire";
            seq_a = resolve_seq(w_spec_set, w_comm_set, fire_name, &active_set);
            if (!seq_a) seq_a = resolve_seq(w_spec_set, w_comm_set, "standfire", &active_set);
            float dur = std::max(0.15f, telemetry.weapon.fire_anim_duration);
            float prog = std::clamp(1.0f - (telemetry.weapon.fire_anim_timer / dur), 0.0f, 1.0f);
            norm_time = prog;
            float recoil_env = std::sin(std::min(prog * 3.5f, 1.0f) * PI);
            vm_offset = base_armed_offset + Vec3(0.0f, -1.2f * recoil_env, 0.8f * recoil_env);
        } else if (state == EMovement::MOVE_Slide || state == EMovement::MOVE_RumpSlide) {
            seq_a = resolve_seq(w_spec_set, w_comm_set, "CrouchSlide", &active_set);
            float dur = (seq_a && seq_a->length > 0.1f) ? seq_a->length : 1.5f;
            norm_time = std::clamp(telemetry.combat_anim_time / dur, 0.0f, 0.96f);
            vm_offset = base_armed_offset + Vec3(0.0f, -4.0f, 4.0f);
            show_lower_body = true;
        } else if (state == EMovement::MOVE_Crouch) {
            const char* cname = (speed > 20.0f) ? "crouchfwdready" : "crouchstill";
            seq_a = resolve_seq(w_spec_set, w_comm_set, cname, &active_set);
            if (!seq_a) seq_a = resolve_seq(w_spec_set, w_comm_set, "standfire", &active_set);
            norm_time = std::fmod(sim_t * 1.2f, 1.0f);
            vm_offset = base_armed_offset + Vec3(0.0f, 0.0f, -1.0f);
        } else if (state == EMovement::MOVE_WallRunningRight || state == EMovement::MOVE_WallRunningLeft) {
            const char* wname = (state == EMovement::MOVE_WallRunningRight) ? "WallrunRight" : "WallrunLeft";
            seq_a = resolve_seq(w_spec_set, &faith_common_set_, wname, &active_set);
            float dur = (seq_a && seq_a->length > 0.1f) ? seq_a->length : 0.533f;
            norm_time = std::fmod(telemetry.combat_anim_time / dur, 1.0f);
            vm_offset = base_armed_offset;
        } else if (state == EMovement::MOVE_GrabPullUp || state == EMovement::MOVE_Grabbing ||
                   state == EMovement::MOVE_IntoGrab || state == EMovement::MOVE_GrabTransfer ||
                   state == EMovement::MOVE_Climb ||
                   state == EMovement::MOVE_SpeedVaulting || state == EMovement::MOVE_VaultOver ||
                   state == EMovement::MOVE_SpringBoarding || state == EMovement::MOVE_Swing ||
                   state == EMovement::MOVE_WallClimbing || state == EMovement::MOVE_SkillRoll) {
            // Two-handed parkour climb / vault / hang / swing maneuvers holster viewmodel to unarmed anim set
            active_set = &faith_unarmed_set_;
            if (state == EMovement::MOVE_GrabPullUp || state == EMovement::MOVE_GrabTransfer) {
                seq_a = active_set->find_sequence("HangHeaveUp");
                float dur = (telemetry.combat_anim_duration > 0.1f)
                                ? telemetry.combat_anim_duration
                                : ((seq_a && seq_a->length > 0.1f) ? seq_a->length : 0.65f);
                norm_time = std::clamp(telemetry.combat_anim_time / dur, 0.0f, 0.98f);
                vm_offset = Vec3(0.0f, 8.0f, 6.0f);
            } else if (state == EMovement::MOVE_IntoGrab || state == EMovement::MOVE_Grabbing) {
                seq_a = (telemetry.combat_anim_time < 0.35f) ? active_set->find_sequence("HangHardStart")
                                                             : active_set->find_sequence("Hang");
                norm_time = std::fmod(telemetry.combat_anim_time * 0.8f, 1.0f);
                vm_offset = Vec3(0.0f, 8.0f, 3.0f);
            } else if (state == EMovement::MOVE_Climb) {
                const bool alternate_hand = (static_cast<int>(telemetry.combat_anim_time * 3.0f) & 1) != 0;
                seq_a = active_set->find_sequence(alternate_hand ? "LadderClimbUpLeftHand" : "LadderClimbUpRightHand");
                norm_time = std::fmod(telemetry.combat_anim_time * 3.0f, 1.0f);
                vm_offset = Vec3(0.0f, 16.0f, 28.0f);
            } else if (state == EMovement::MOVE_WallClimbing) {
                seq_a = active_set->find_sequence("WallRunVertical");
                float dur = (seq_a && seq_a->length > 0.1f) ? seq_a->length : 0.467f;
                norm_time = std::fmod(telemetry.combat_anim_time / dur, 1.0f);
                vm_offset = Vec3(0.0f, 16.0f, 18.0f);
            } else if (state == EMovement::MOVE_SpringBoarding) {
                seq_a = active_set->find_sequence("SpringBoardRightLeg");
                float dur = (telemetry.combat_anim_duration > 0.1f) ? telemetry.combat_anim_duration : 0.72f;
                norm_time = std::clamp(0.18f + 0.72f * (telemetry.combat_anim_time / dur), 0.12f, 0.94f);
                vm_offset = Vec3(2.0f, 14.0f, 10.0f);
                show_lower_body = true;
            } else if (state == EMovement::MOVE_Swing) {
                seq_a = active_set->find_sequence("swingposebacktop");
                if (!seq_a) seq_a = active_set->find_sequence("swinghardstart");
                float dur = (seq_a && seq_a->length > 0.1f) ? seq_a->length : 0.667f;
                norm_time = std::fmod(telemetry.combat_anim_time / dur, 1.0f);
                vm_offset = Vec3(0.0f, 6.0f, -2.0f);
                show_lower_body = true;
            } else if (state == EMovement::MOVE_SkillRoll) {
                seq_a = active_set->find_sequence("fallinglandroll");
                float dur = (seq_a && seq_a->length > 0.1f) ? seq_a->length : 0.85f;
                norm_time = std::clamp(telemetry.combat_anim_time / dur, 0.0f, 0.98f);
                vm_offset = Vec3(0.0f, 16.0f, 14.0f);
                show_lower_body = true;
            } else {
                seq_a = active_set->find_sequence("VaultOver");
                float dur = (telemetry.combat_anim_duration > 0.1f) ? telemetry.combat_anim_duration : 0.50f;
                norm_time = std::clamp(0.10f + 0.82f * (telemetry.combat_anim_time / dur), 0.08f, 0.94f);
                vm_offset = Vec3(2.0f, 14.0f, 12.0f);
                show_lower_body = true;
            }
        } else if (speed > 55.0f) {
            // Armed walking / running ready cycle (runfwdready / walkfwdready blended with WeaponPose)
            const char* move_name = (speed < 240.0f) ? "walkfwdready" : "runfwdready";
            seq_a = resolve_seq(w_spec_set, w_comm_set, move_name, &active_set);
            if (!seq_a) seq_a = resolve_seq(w_spec_set, w_comm_set, "runfwdready", &active_set);
            if (seq_a) {
                float cycle_rate = std::clamp(speed / 380.0f, 0.9f, 1.75f);
                norm_time = std::fmod(sim_t * cycle_rate, 1.0f);
            } else {
                seq_a = resolve_seq(w_spec_set, w_comm_set, "standfire", &active_set);
                norm_time = 0.0f;
            }
            float sway_freq = std::clamp(speed * 0.018f, 2.0f, 11.0f);
            float sway_amp = std::clamp(speed / 480.0f, 0.08f, 0.85f);
            float bob_x = std::sin(sim_t * sway_freq) * 0.9f * sway_amp;
            float bob_z = std::abs(std::cos(sim_t * sway_freq)) * -0.8f * sway_amp;
            vm_offset = base_armed_offset + Vec3(bob_x, 0.0f, bob_z);
        } else {
            // Armed standing idle (uses weaponposeempty when empty on pistols, or WeaponPose / standfire(t=0))
            if (telemetry.weapon.ammo == 0 && w_spec_set && w_spec_set->find_sequence("weaponposeempty")) {
                active_set = w_spec_set;
                seq_a = w_spec_set->find_sequence("weaponposeempty");
            } else {
                seq_a = resolve_seq(w_spec_set, w_comm_set, "standfire", &active_set);
            }
            norm_time = 0.0f;
            float bob_z = std::sin(sim_t * 1.8f) * 0.25f;
            vm_offset = base_armed_offset + Vec3(0.0f, 0.0f, bob_z);
        }
    } else {
        // 3. Unarmed Parkour & Locomotion Tree (cooked UE3 AS_C1P_Unarmed sequences with live state progression)
        const float st = std::max(0.0f, telemetry.combat_anim_time);
        switch (state) {
            case EMovement::MOVE_SpringBoarding: {
                seq_a = active_set->find_sequence("SpringBoardRightLeg");
                seq_b = active_set->find_sequence("VaultOver");
                float dur = (telemetry.combat_anim_duration > 0.1f) ? telemetry.combat_anim_duration : 0.72f;
                norm_time = std::clamp(0.18f + 0.72f * (st / dur), 0.12f, 0.94f);
                blend_alpha = 0.22f;
                vm_offset = Vec3(2.0f, 14.0f, 10.0f);
                show_lower_body = true;
                break;
            }
            case EMovement::MOVE_SpeedVaulting:
            case EMovement::MOVE_VaultOver: {
                seq_a = active_set->find_sequence("VaultOver");
                float dur = (telemetry.combat_anim_duration > 0.1f) ? telemetry.combat_anim_duration : 0.50f;
                norm_time = std::clamp(0.10f + 0.82f * (st / dur), 0.08f, 0.94f);
                vm_offset = Vec3(2.0f, 14.0f, 12.0f);
                show_lower_body = true;
                break;
            }
            case EMovement::MOVE_StepUp:
            case EMovement::MOVE_AutoStepUp: {
                seq_a = active_set->find_sequence("VaultOnto");
                if (!seq_a) seq_a = active_set->find_sequence("stepuprightleg48");
                float dur = (seq_a && seq_a->length > 0.1f) ? seq_a->length : 0.36f;
                norm_time = std::clamp(st / dur, 0.05f, 0.95f);
                vm_offset = Vec3(0.0f, 14.0f, 12.0f);
                show_lower_body = true;
                break;
            }
            case EMovement::MOVE_GrabPullUp:
            case EMovement::MOVE_GrabTransfer: {
                seq_a = active_set->find_sequence("HangHeaveUp");
                seq_b = active_set->find_sequence("HangHeaveOver");
                float dur = (telemetry.combat_anim_duration > 0.1f) ? telemetry.combat_anim_duration : 0.65f;
                norm_time = std::clamp(st / dur, 0.0f, 0.98f);
                blend_alpha = 0.25f;
                vm_offset = Vec3(0.0f, 8.0f, 6.0f);
                break;
            }
            case EMovement::MOVE_IntoGrab: {
                seq_a = active_set->find_sequence("HangHardStart");
                float dur = (seq_a && seq_a->length > 0.1f) ? seq_a->length : 0.55f;
                norm_time = std::clamp(st / dur, 0.0f, 0.96f);
                vm_offset = Vec3(0.0f, 8.0f, 3.0f);
                break;
            }
            case EMovement::MOVE_Grabbing: {
                if (st < 0.32f) {
                    seq_a = active_set->find_sequence("HangHardStart");
                    norm_time = std::clamp(st / 0.65f, 0.0f, 0.65f);
                } else if (speed > 25.0f) {
                    // Lateral shimmy along ledge
                    const Vec3 right = Rotator::from_degrees(0.0f, telemetry.yaw_deg, 0.0f).right();
                    const bool go_left = telemetry.velocity.dot(right) < 0.0f;
                    seq_a = active_set->find_sequence(go_left ? "HangStrafeLeft" : "HangStrafeRight");
                    if (!seq_a) seq_a = active_set->find_sequence("HangStrafeRight");
                    float dur = (seq_a && seq_a->length > 0.1f) ? seq_a->length : 1.067f;
                    norm_time = std::fmod(st / dur, 1.0f);
                } else {
                    seq_a = active_set->find_sequence("Hang");
                    float dur = (seq_a && seq_a->length > 0.1f) ? seq_a->length : 2.0f;
                    norm_time = std::fmod((st - 0.32f) / dur, 1.0f);
                }
                vm_offset = Vec3(0.0f, 8.0f, 3.0f);
                break;
            }
            case EMovement::MOVE_GrabJump: {
                seq_a = active_set->find_sequence("hangturnjump");
                float dur = (seq_a && seq_a->length > 0.1f) ? seq_a->length : 0.85f;
                norm_time = std::clamp(st / dur, 0.0f, 0.95f);
                vm_offset = Vec3(0.0f, 10.0f, 6.0f);
                break;
            }
            case EMovement::MOVE_Climb: {
                const float climb_vz = telemetry.velocity.z;
                if (climb_vz < -15.0f) {
                    seq_a = active_set->find_sequence("LadderClimbDownFast");
                    float dur = (seq_a && seq_a->length > 0.1f) ? seq_a->length : 0.5f;
                    norm_time = std::fmod(st / dur, 1.0f);
                    vm_offset = Vec3(0.0f, 12.0f, 4.0f);
                } else if (std::abs(climb_vz) > 10.0f || speed > 10.0f) {
                    const float rung_cycle = st * 2.8f;
                    const bool left_hand = (static_cast<int>(rung_cycle) & 1) != 0;
                    seq_a = active_set->find_sequence(left_hand ? "LadderClimbUpLeftHand" : "LadderClimbUpRightHand");
                    norm_time = std::fmod(rung_cycle, 1.0f);
                    vm_offset = Vec3(0.0f, 16.0f, 28.0f);
                } else {
                    seq_a = active_set->find_sequence("LadderClimbUpRightHandStill");
                    float dur = (seq_a && seq_a->length > 0.1f) ? seq_a->length : 2.0f;
                    norm_time = std::fmod(st / dur, 1.0f);
                    vm_offset = Vec3(0.0f, 16.0f, 24.0f);
                }
                break;
            }
            case EMovement::MOVE_WallRunningRight: {
                seq_a = active_set->find_sequence("WallrunRight");
                seq_b = active_set->find_sequence("SprintFwd");
                float dur = (seq_a && seq_a->length > 0.1f) ? seq_a->length : 0.533f;
                norm_time = std::fmod(st / dur, 1.0f);
                blend_alpha = 0.25f;
                vm_offset = Vec3(4.0f, 14.0f, 10.0f);
                break;
            }
            case EMovement::MOVE_WallRunningLeft: {
                seq_a = active_set->find_sequence("WallrunLeft");
                seq_b = active_set->find_sequence("SprintFwd");
                float dur = (seq_a && seq_a->length > 0.1f) ? seq_a->length : 0.533f;
                norm_time = std::fmod(st / dur, 1.0f);
                blend_alpha = 0.25f;
                vm_offset = Vec3(-4.0f, 14.0f, 10.0f);
                break;
            }
            case EMovement::MOVE_WallRunJump: {
                seq_a = active_set->find_sequence("wallrunjumpright");
                if (!seq_a) seq_a = active_set->find_sequence("jumpfast");
                float dur = (seq_a && seq_a->length > 0.1f) ? seq_a->length : 0.90f;
                norm_time = std::clamp(st / dur, 0.0f, 0.92f);
                vm_offset = Vec3(0.0f, 16.0f, 14.0f);
                show_lower_body = true;
                break;
            }
            case EMovement::MOVE_WallClimbing: {
                if (st < 0.18f && active_set->find_sequence("wallrunverticalstart")) {
                    seq_a = active_set->find_sequence("wallrunverticalstart");
                    norm_time = std::clamp(st / 0.18f, 0.0f, 1.0f);
                } else {
                    seq_a = active_set->find_sequence("WallRunVertical");
                    float dur = (seq_a && seq_a->length > 0.1f) ? seq_a->length : 0.467f;
                    norm_time = std::fmod(st / dur, 1.0f);
                }
                vm_offset = Vec3(0.0f, 16.0f, 18.0f);
                break;
            }
            case EMovement::MOVE_WallClimb180TurnJump: {
                seq_a = active_set->find_sequence("wallrunvertical180turn");
                float dur = (seq_a && seq_a->length > 0.1f) ? seq_a->length : 0.633f;
                norm_time = std::clamp(st / dur, 0.0f, 0.96f);
                vm_offset = Vec3(0.0f, 16.0f, 16.0f);
                break;
            }
            case EMovement::MOVE_180Turn: {
                seq_a = active_set->find_sequence("RunTurn180");
                float dur = (seq_a && seq_a->length > 0.1f) ? seq_a->length : 0.65f;
                norm_time = std::clamp(st / dur, 0.0f, 0.96f);
                vm_offset = Vec3(0.0f, 16.0f, 14.0f);
                break;
            }
            case EMovement::MOVE_180TurnInAir: {
                seq_a = active_set->find_sequence("JumpTurnFly");
                float dur = (seq_a && seq_a->length > 0.1f) ? seq_a->length : 0.60f;
                norm_time = std::clamp(st / dur, 0.0f, 0.96f);
                vm_offset = Vec3(0.0f, 16.0f, 14.0f);
                break;
            }
            case EMovement::MOVE_DodgeJump: {
                seq_a = active_set->find_sequence("dodgejumpright");
                float dur = (seq_a && seq_a->length > 0.1f) ? seq_a->length : 0.65f;
                norm_time = std::clamp(st / dur, 0.0f, 0.95f);
                vm_offset = Vec3(0.0f, 16.0f, 14.0f);
                break;
            }
            case EMovement::MOVE_Swing: {
                if (st < 0.22f && active_set->find_sequence("swinghardstart")) {
                    seq_a = active_set->find_sequence("swinghardstart");
                    norm_time = std::clamp(st / 0.22f, 0.0f, 1.0f);
                    vm_offset = Vec3(0.0f, 6.0f, -1.0f);
                } else {
                    seq_a = active_set->find_sequence("swingposebacktop");
                    if (!seq_a) seq_a = active_set->find_sequence("swingposefronttop");
                    float dur = (seq_a && seq_a->length > 0.1f) ? seq_a->length : 0.667f;
                    norm_time = std::fmod(st / dur, 1.0f);
                    vm_offset = Vec3(0.0f, 6.0f, -2.0f);
                }
                show_lower_body = true;
                break;
            }
            case EMovement::MOVE_ZipLine: {
                seq_a = active_set->find_sequence("ZipLine");
                float dur = (seq_a && seq_a->length > 0.1f) ? seq_a->length : 1.0f;
                norm_time = std::fmod(st / dur, 1.0f);
                vm_offset = Vec3(1.5f, 38.0f, -30.5f);
                break;
            }
            case EMovement::MOVE_Coil: {
                seq_a = active_set->find_sequence("jumpcoil");
                if (!seq_a) seq_a = active_set->find_sequence("CrouchSlide");
                float dur = (seq_a && seq_a->length > 0.1f) ? seq_a->length : 0.55f;
                norm_time = std::clamp(st / dur, 0.0f, 0.96f);
                vm_offset = Vec3(0.0f, 8.0f, 10.0f);
                show_lower_body = true;
                break;
            }
            case EMovement::MOVE_Slide:
            case EMovement::MOVE_RumpSlide: {
                seq_a = active_set->find_sequence("CrouchSlide");
                float dur = (seq_a && seq_a->length > 0.1f) ? seq_a->length : 1.5f;
                norm_time = std::clamp(st / dur, 0.0f, 0.96f);
                vm_offset = Vec3(0.0f, 6.0f, 14.0f);
                show_lower_body = true;
                break;
            }
            case EMovement::MOVE_Jump: {
                seq_a = active_set->find_sequence("jumpfast");
                if (!seq_a) seq_a = active_set->find_sequence("jumpair");
                float dur = (seq_a && seq_a->length > 0.1f) ? seq_a->length : 0.90f;
                norm_time = std::clamp(st / dur, 0.0f, 0.88f);
                vm_offset = Vec3(0.0f, 16.0f, 14.0f);
                break;
            }
            case EMovement::MOVE_Falling: {
                seq_a = active_set->find_sequence("jumpair");
                if (!seq_a) seq_a = active_set->find_sequence("jumpfast");
                float dur = (seq_a && seq_a->length > 0.1f) ? seq_a->length : 1.0f;
                norm_time = std::fmod(st / dur, 1.0f);
                vm_offset = Vec3(0.0f, 16.0f, 14.0f);
                break;
            }
            case EMovement::MOVE_Crouch: {
                seq_a = (speed > 20.0f) ? active_set->find_sequence("crouchfwd")
                                        : active_set->find_sequence("crouchstill");
                norm_time = std::fmod(sim_t * 1.2f, 1.0f);
                vm_offset = Vec3(0.0f, 16.0f, 14.0f);
                break;
            }
            case EMovement::MOVE_SkillRoll: {
                seq_a = active_set->find_sequence("fallinglandroll");
                float dur = (seq_a && seq_a->length > 0.1f) ? seq_a->length : 0.85f;
                norm_time = std::clamp(st / dur, 0.0f, 0.98f);
                vm_offset = Vec3(0.0f, 16.0f, 14.0f);
                show_lower_body = true;
                break;
            }
            case EMovement::MOVE_SoftLanding: {
                seq_a = active_set->find_sequence("fallinglandsoftlanding");
                if (!seq_a) seq_a = active_set->find_sequence("JumpLand");
                float dur = (seq_a && seq_a->length > 0.1f) ? seq_a->length : 0.45f;
                norm_time = std::clamp(st / dur, 0.0f, 0.96f);
                vm_offset = Vec3(0.0f, 15.0f, 13.0f);
                show_lower_body = true;
                break;
            }
            case EMovement::MOVE_Landing:
            case EMovement::MOVE_LayOnGround: {
                seq_a = active_set->find_sequence("fallinglandhard");
                norm_time = std::clamp(st / 1.8f, 0.0f, 0.96f);
                vm_offset = Vec3(0.0f, 14.0f, 12.0f);
                show_lower_body = true;
                break;
            }
            case EMovement::MOVE_LedgeWalk:
            case EMovement::MOVE_Balance: {
                seq_a = active_set->find_sequence("walkbalancefwd");
                norm_time = std::fmod(sim_t * 1.2f, 1.0f);
                vm_offset = Vec3(0.0f, 16.0f, 14.0f);
                break;
            }
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

    if (!seq_a) {
        active_set = &faith_unarmed_set_;
        seq_a = faith_unarmed_set_.find_sequence("Stand");
    }

    thread_local std::vector<Vec3> local_pos, local_pos_b, wp_pos, comp_pos, delta_pos;
    thread_local std::vector<Quat4> local_quat, local_quat_b, wp_quat, comp_quat, delta_quat;
    sample_sequence_pose(faith_upper_, *active_set, seq_a, norm_time, local_pos, local_quat);

    if (seq_b && blend_alpha > 0.001f) {
        const AnimSetAsset* owner_b = set_b_ptr ? set_b_ptr : active_set;
        sample_sequence_pose(faith_upper_, *owner_b, seq_b, norm_time, local_pos_b, local_quat_b);
        blend_local_poses(local_pos, local_quat, local_pos_b, local_quat_b, blend_alpha, local_pos, local_quat);
    }

    // TdAnimNodeWeaponPose: when playing a shared/common locomotion animation (active_set != w_spec_set),
    // preserve the weapon-specific RightWeapon attachment bone (49) and right-hand grip phalanges (50..68)
    // from w_spec_set's WeaponPose so every weapon stays locked inside Faith's right palm & trigger finger!
    if (w_spec_set && active_set != w_spec_set) {
        const AnimSequenceAsset* wp_seq =
            (telemetry.weapon.ammo == 0 && w_spec_set->find_sequence("weaponposeempty"))
                ? w_spec_set->find_sequence("weaponposeempty")
                : w_spec_set->find_sequence("WeaponPose");
        if (!wp_seq) wp_seq = w_spec_set->find_sequence("standfire");
        if (wp_seq) {
            sample_sequence_pose(faith_upper_, *w_spec_set, wp_seq, 0.0f, wp_pos, wp_quat);
            for (size_t b = 49; b <= 68 && b < local_pos.size(); ++b) {
                local_pos[b] = wp_pos[b];
                local_quat[b] = wp_quat[b];
            }
        }
    }

    // Forward kinematics in component space
    compute_skeleton_fk(faith_upper_.bones, local_pos, local_quat, comp_pos, comp_quat);

    // Compute per-bone Linear Blend Skinning deltas
    compute_skin_deltas(faith_upper_, comp_pos, comp_quat, delta_pos, delta_quat);

    // EyeJoint (Bone 72) reference frame
    const size_t eye_idx = (faith_upper_.bones.size() > 72) ? 72 : 0;
    const Vec3 eye_pos = comp_pos[eye_idx];
    const Quat4 eye_inv_quat = comp_quat[eye_idx].conjugate();

    // Rigid pitch rotation for overhead 1P maneuvers authored with pitched camera view (e.g. ZipLine cable pulley)
    float upper_pitch_rad = 0.0f;
    if (state == EMovement::MOVE_ZipLine) {
        upper_pitch_rad = 55.0f * DEG2RAD;
    }
    const float cos_up = std::cos(upper_pitch_rad);
    const float sin_up = std::sin(upper_pitch_rad);

    // Note: vm_view = look_at((0,0,0), (0,100,0), (0,0,1)) has s = (-1,0,0),
    // so +rel.x maps to Screen Left (LeftHand) and -rel.x maps to Screen Right (RightHand Red Glove).
    // (rel.x, rel.z, -rel.y) is a proper right-handed 90-deg rotation (det = +1).
    float kick_arc = std::sin(combat_progress * PI);
    auto raw_to_vm_pos = [&](const Vec3& raw_p, bool is_lower) -> Vec3 {
        Vec3 rel = eye_inv_quat.rotate(raw_p - eye_pos);
        if (!is_lower) {
            float vy = rel.z;
            float vz = -rel.y;
            float ry = vy * cos_up + vz * sin_up;
            float rz = -vy * sin_up + vz * cos_up;
            return Vec3(rel.x + vm_offset.x, ry + vm_offset.y, rz + vm_offset.z);
        }
        if (lower_body_high_kick) {
            // For flying jump kick / high kick / wallrun kick: pitch legs higher into foreground view
            float lx = rel.x - 4.0f;
            float ly = rel.z - 46.0f + kick_arc * 18.0f;
            float lz = -rel.y + 4.0f + kick_arc * 14.0f;
            const float sin_p = 0.42f;
            const float cos_p = 0.907f;
            return Vec3(lx, ly * cos_p - lz * sin_p, ly * sin_p + lz * cos_p - 2.0f);
        }
        // For lower-body slide / slide-kick: shift hips behind near plane and pitch shins/boots into lower foreground
        float slide_thrust = (state == EMovement::MOVE_MeleeSlide) ? (kick_arc * 14.0f) : 0.0f;
        float lx = rel.x - 6.0f;
        float ly = rel.z - 54.0f + slide_thrust;
        float lz = -rel.y - 2.0f + (state == EMovement::MOVE_MeleeSlide ? kick_arc * 6.0f : 0.0f);
        const float sin_p = 0.28f;
        const float cos_p = 0.96f;
        return Vec3(lx, ly * cos_p - lz * sin_p, ly * sin_p + lz * cos_p - 8.0f);
    };
    auto raw_to_vm_dir = [&](const Vec3& raw_d) -> Vec3 {
        Vec3 rel = eye_inv_quat.rotate(raw_d);
        float vy = rel.z;
        float vz = -rel.y;
        return Vec3(rel.x, vy * cos_up + vz * sin_up, -vy * sin_up + vz * cos_up).normalized();
    };

    thread_local std::vector<Vec3> skinned_pos;
    thread_local std::vector<Vec3> skinned_norm;
    thread_local std::vector<uint8_t> vert_valid;

    auto append_skinned_mesh = [&](const SkeletalMeshAsset& mesh, bool is_lower) {
        skinned_pos.resize(mesh.vertices.size());
        skinned_norm.resize(mesh.vertices.size());
        vert_valid.assign(mesh.vertices.size(), 1);

        for (size_t i = 0; i < mesh.vertices.size(); ++i) {
            const SkinnedVertex& sv = mesh.vertices[i];
            uint8_t dom_bone = sv.bones[0];
            if (!is_lower) {
                if (sv.chunk_index == 1) {
                    // Chunk 1 (Faith_Glove): keep RightForeArm wrap, RightHand, fingers, and RightForeArmRoll strap (47..71),
                    // culling only the shoulder cuff on 46.
                    if (dom_bone < 47 || dom_bone > 71) {
                        vert_valid[i] = 0;
                        continue;
                    }
                } else {
                    // Chunk 0 (Skin): keep full arms (LeftArm 17..41, RightArm 46..71) so raised elbows in
                    // ZipLine, Hang, Swing, Climb, Vault, and WallRun connect smoothly to shoulders.
                    bool is_arm_or_hand = (dom_bone >= 17 && dom_bone <= 41) || (dom_bone >= 46 && dom_bone <= 71);
                    if (!is_arm_or_hand) {
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
            if (skinned_pos[i].y < (is_lower ? 10.0f : 2.0f)) {
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
                out_v.u2 = is_lower ? 3.0f : (sv.chunk_index == 0 ? 1.0f : 2.0f);
                out_v.color = 0xFFFFFFFF;
                out_triangles.push_back(out_v);
            }
        }
    };

    const SkeletalMeshAsset* equipped_wmesh = (telemetry.weapon.equipped || state == EMovement::MOVE_Snatch)
                                                  ? get_weapon_mesh(telemetry.weapon.name)
                                                  : nullptr;
    size_t w_idx_cnt = equipped_wmesh ? equipped_wmesh->indices.size() : 0;
    out_triangles.reserve(faith_upper_.indices.size() + faith_lower_.indices.size() + w_idx_cnt + 48);

    append_skinned_mesh(faith_upper_, false);
    if (show_lower_body && faith_lower_.is_valid()) {
        append_skinned_mesh(faith_lower_, true);
    }

    // Attach & skin equipped weapon skeletal mesh via RightWeapon (bone 49) + cooked weapon tracks (74..)
    if (equipped_wmesh && equipped_wmesh->is_valid()) {
        int32_t rw_idx = 49;
        auto it_rw = faith_upper_.bone_name_to_index.find("rightweapon");
        if (it_rw != faith_upper_.bone_name_to_index.end()) rw_idx = it_rw->second;

        bool heavy = telemetry.weapon.is_heavy || is_heavy_weapon_name(telemetry.weapon.name);
        const size_t wb_cnt = equipped_wmesh->bones.size();
        thread_local std::vector<Vec3> wep_local_pos;
        thread_local std::vector<Quat4> wep_local_quat;
        wep_local_pos.resize(wb_cnt);
        wep_local_quat.resize(wb_cnt);
        for (size_t b = 0; b < wb_cnt; ++b) {
            wep_local_pos[b] = equipped_wmesh->bones[b].bind_pos;
            wep_local_quat[b] = equipped_wmesh->bones[b].bind_quat;
        }

        // 1. Initialize weapon sub-bones (Wep_Extra1..Wep_Mag, Belt_Joint1..12) from weapon's WeaponPose
        const AnimSequenceAsset* base_wep_seq = nullptr;
        if (w_spec_set) {
            if (telemetry.weapon.ammo == 0 && telemetry.weapon.fire_anim_timer <= 0.0f) {
                base_wep_seq = w_spec_set->find_sequence("weaponposeempty");
            }
            if (!base_wep_seq) base_wep_seq = w_spec_set->find_sequence("WeaponPose");
            if (!base_wep_seq) base_wep_seq = w_spec_set->find_sequence("standfire");
        }
        auto apply_weapon_tracks = [&](const AnimSetAsset* aset, const AnimSequenceAsset* seq, float t_norm) {
            if (!aset || !seq) return;
            float tc = t_norm - std::floor(t_norm);
            if (tc < 0.0f) tc += 1.0f;
            for (size_t b = 1; b < wb_cnt; ++b) {
                const std::string& wkey = equipped_wmesh->bones[b].name_lower.empty()
                                              ? to_lower_str(equipped_wmesh->bones[b].name)
                                              : equipped_wmesh->bones[b].name_lower;
                auto it = aset->bone_to_track.find(wkey);
                if (it == aset->bone_to_track.end()) continue;
                int32_t tidx = it->second;
                if (tidx < 0 || static_cast<size_t>(tidx) >= seq->tracks.size()) continue;
                const AnimTrack& tr = seq->tracks[tidx];
                if (tr.positions.size() == 1) {
                    wep_local_pos[b] = tr.positions[0];
                } else if (tr.positions.size() > 1) {
                    float f_idx = tc * static_cast<float>(tr.positions.size() - 1);
                    size_t i0 = static_cast<size_t>(f_idx);
                    size_t i1 = std::min(i0 + 1, tr.positions.size() - 1);
                    float frac = f_idx - static_cast<float>(i0);
                    wep_local_pos[b] = tr.positions[i0] + (tr.positions[i1] - tr.positions[i0]) * frac;
                }
                if (tr.rotations.size() == 1) {
                    wep_local_quat[b] = tr.rotations[0];
                } else if (tr.rotations.size() > 1) {
                    float f_idx = tc * static_cast<float>(tr.rotations.size() - 1);
                    size_t i0 = static_cast<size_t>(f_idx);
                    size_t i1 = std::min(i0 + 1, tr.rotations.size() - 1);
                    float frac = f_idx - static_cast<float>(i0);
                    wep_local_quat[b] = Quat4::slerp(tr.rotations[i0], tr.rotations[i1], frac);
                }
            }
        };

        if (base_wep_seq) {
            apply_weapon_tracks(w_spec_set, base_wep_seq, 0.0f);
        }
        // 2. When playing a weapon-specific animation (standfire, unholster, throwaway, SnatchFwd, etc.),
        // evaluate real cooked weapon tracks (e.g. M95 bolt pull, Remington/Neostead pump, pistol slide blowback, Minimi belt)
        if (w_spec_set && active_set == w_spec_set && seq_a) {
            apply_weapon_tracks(w_spec_set, seq_a, norm_time);
        }

        // 3. Anchor Wep_Root (bone[0]) directly to Faith's RightWeapon bone in component space at 1:1 scale
        if (!wep_local_pos.empty()) {
            wep_local_pos[0] = comp_pos[rw_idx];
            wep_local_quat[0] = comp_quat[rw_idx];
        }

        std::vector<Vec3> wep_comp_pos;
        std::vector<Quat4> wep_comp_quat;
        compute_skeleton_fk(equipped_wmesh->bones, wep_local_pos, wep_local_quat, wep_comp_pos, wep_comp_quat);

        std::vector<Vec3> wep_delta_pos;
        std::vector<Quat4> wep_delta_quat;
        compute_skin_deltas(*equipped_wmesh, wep_comp_pos, wep_comp_quat, wep_delta_pos, wep_delta_quat);

        std::vector<Vec3> w_skinned_pos(equipped_wmesh->vertices.size());
        std::vector<Vec3> w_skinned_norm(equipped_wmesh->vertices.size());
        std::vector<uint8_t> w_vert_valid(equipped_wmesh->vertices.size(), 1);
        for (size_t i = 0; i < equipped_wmesh->vertices.size(); ++i) {
            const SkinnedVertex& sv = equipped_wmesh->vertices[i];
            Vec3 p_acc(0.0f, 0.0f, 0.0f);
            Vec3 n_acc(0.0f, 0.0f, 0.0f);
            for (int k = 0; k < 4; ++k) {
                if (sv.weights[k] == 0) continue;
                float w = static_cast<float>(sv.weights[k]) * (1.0f / 255.0f);
                size_t b = std::min(static_cast<size_t>(sv.bones[k]), wep_delta_pos.size() - 1);
                p_acc += (wep_delta_quat[b].rotate(sv.bind_pos) + wep_delta_pos[b]) * w;
                n_acc += wep_delta_quat[b].rotate(sv.bind_norm) * w;
            }
            w_skinned_pos[i] = raw_to_vm_pos(p_acc, false);
            w_skinned_norm[i] = raw_to_vm_dir(n_acc);
            if (w_skinned_pos[i].y < 1.5f) {
                w_vert_valid[i] = 0;
            }
        }

        for (size_t i = 0; i + 2 < equipped_wmesh->indices.size(); i += 3) {
            uint16_t i0 = equipped_wmesh->indices[i + 0];
            uint16_t i1 = equipped_wmesh->indices[i + 1];
            uint16_t i2 = equipped_wmesh->indices[i + 2];
            if (i0 >= equipped_wmesh->vertices.size() ||
                i1 >= equipped_wmesh->vertices.size() ||
                i2 >= equipped_wmesh->vertices.size()) continue;
            if (!w_vert_valid[i0] || !w_vert_valid[i1] || !w_vert_valid[i2]) continue;

            const uint16_t idx_tri[3] = {i0, i1, i2};
            for (int k = 0; k < 3; ++k) {
                uint16_t vi = idx_tri[k];
                const SkinnedVertex& sv = equipped_wmesh->vertices[vi];
                Vertex out_v{};
                out_v.position = w_skinned_pos[vi];
                out_v.normal = w_skinned_norm[vi];
                out_v.tangent = Vec3(1.0f, 0.0f, 0.0f);
                out_v.u = sv.u;
                out_v.v = sv.v;
                out_v.u2 = (sv.mat_type == 1) ? 5.0f : (sv.mat_type == 2 ? 6.0f : 4.0f);
                uint32_t un16 = static_cast<uint32_t>(std::clamp(sv.un, 0.0f, 1.0f) * 65535.0f + 0.5f);
                uint32_t vn16 = static_cast<uint32_t>(std::clamp(sv.vn, 0.0f, 1.0f) * 65535.0f + 0.5f);
                out_v.color = (un16 & 0xFFFFu) | ((vn16 & 0xFFFFu) << 16);
                out_triangles.push_back(out_v);
            }
        }

        // Emit 3D Muzzle Flash at animated Wep_Flash (bone[3]) along animated barrel forward vector
        if (telemetry.weapon.muzzle_flash_timer > 0.0f && wep_comp_pos.size() > 3) {
            Vec3 flash_vm = raw_to_vm_pos(wep_comp_pos[3], false);
            Vec3 barrel_fwd = raw_to_vm_dir(wep_comp_quat[0].rotate(Vec3(0.0f, 0.0f, 1.0f)));
            Vec3 barrel_right = raw_to_vm_dir(wep_comp_quat[0].rotate(Vec3(1.0f, 0.0f, 0.0f)));
            Vec3 barrel_up = raw_to_vm_dir(wep_comp_quat[0].rotate(Vec3(0.0f, -1.0f, 0.0f)));
            float flash_scale = heavy ? 5.2f : 3.6f;
            append_muzzle_flash_mesh(out_triangles, flash_vm,
                                     barrel_fwd, barrel_right, barrel_up,
                                     flash_scale, flash_scale * 1.8f);
        }
    }
}

// -----------------------------------------------------------------------------
// First-person camera animation (TdPlayerPawn.CalcCamera)
// -----------------------------------------------------------------------------
namespace {

// UE3 FMatrix::Rotator() of the rotation `q`, in degrees: pitch and yaw from where the X axis points,
// roll from the Y and Z axes against the Y axis of that pitch and yaw.
void matrix_rotator_degrees(const Quat4& q, float& pitch, float& yaw, float& roll) {
    const Vec3 x_axis = q.rotate(Vec3(1.0f, 0.0f, 0.0f));
    const Vec3 y_axis = q.rotate(Vec3(0.0f, 1.0f, 0.0f));
    const Vec3 z_axis = q.rotate(Vec3(0.0f, 0.0f, 1.0f));
    const float p = std::atan2(x_axis.z, std::sqrt(x_axis.x * x_axis.x + x_axis.y * x_axis.y));
    const float y = std::atan2(x_axis.y, x_axis.x);
    const Vec3 sy_axis(-std::sin(y), std::cos(y), 0.0f);
    const float r = std::atan2(z_axis.dot(sy_axis), y_axis.dot(sy_axis));
    pitch = p * RAD2DEG;
    yaw = y * RAD2DEG;
    roll = r * RAD2DEG;
}

}  // namespace

CameraAnimation AnimSystem::camera_animation(const PlayerTelemetry& telemetry) const {
    constexpr size_t kRoot = 0, kEyeJoint = 72, kCameraJoint = 73;  // SK_UpperBody
    CameraAnimation out;
    if (!loaded_ || faith_upper_.bones.size() <= kCameraJoint) return out;
    // TdMove_SkillRoll.StartMove: PlayMoveAnim(CNT_FullBody, 'fallinglandroll', 1.0, BlendIn 0.2,
    // BlendOut 0.2, bRootMotion); the move ends with the animation (OnCustomAnimEnd). The armed roll
    // plays the same unarmed sequence (evaluate_faith_1p).
    if (telemetry.move_state != EMovement::MOVE_SkillRoll) return out;
    const AnimSequenceAsset* seq = faith_unarmed_set_.find_sequence("fallinglandroll");
    if (!seq || seq->length <= 0.0f) return out;
    constexpr float kBlendIn = 0.2f;
    constexpr float kBlendOut = 0.2f;
    const float t = std::clamp(telemetry.combat_anim_time, 0.0f, seq->length);
    out.weight = std::clamp(std::min(t / kBlendIn, (seq->length - t) / kBlendOut), 0.0f, 1.0f);
    if (out.weight <= 0.0f) return out;

    thread_local std::vector<Vec3> local_pos, comp_pos;
    thread_local std::vector<Quat4> local_quat, comp_quat;
    // (sample_sequence_pose wraps a normalised time of 1 back round to the first key.)
    sample_sequence_pose(faith_upper_, faith_unarmed_set_, seq, std::min(t / seq->length, 0.9999f), local_pos,
                         local_quat);
    compute_skeleton_fk(faith_upper_.bones, local_pos, local_quat, comp_pos, comp_quat);

    // Root motion moves the root bone's translation into the pawn, so the eye is measured from it.
    const Quat4 root_inv = comp_quat[kRoot].conjugate();
    const Vec3 eye = root_inv.rotate(comp_pos[kEyeJoint] - comp_pos[kRoot]);
    out.eye = Vec3(eye.z, -eye.x, -eye.y);

    // The camera bone's rotation (its bind pose looks straight ahead), blended in from none like the
    // slot's pose: forward through a whole turn and back to level for this sequence.
    const Quat4 cam = Quat4::multiply(root_inv, comp_quat[kCameraJoint]).normalized();
    float p = 0.0f, y = 0.0f, r = 0.0f;
    matrix_rotator_degrees(Quat4::slerp(Quat4(), cam, out.weight), p, y, r);
    out.pitch_deg = -r;
    out.yaw_deg = p;
    out.roll_deg = -y;
    return out;
}

void AnimSystem::player_camera(const PlayerTelemetry& telemetry, Vec3& out_pos, Rotator& out_rot) const {
    const Vec3 still_eye(0.0f, 0.0f, telemetry.eye_height);
    out_pos = telemetry.position + still_eye;
    out_rot = Rotator::from_degrees(telemetry.pitch_deg, telemetry.yaw_deg, telemetry.camera_roll_deg);
    const CameraAnimation ca = camera_animation(telemetry);
    if (ca.weight <= 0.0f) return;
    // The roll locks the view to the body (SetIgnoreLookInput, ResetCameraLook), so the view's yaw is
    // the body's: the frame the mesh, and its EyeJoint, are posed in.
    const Rotator body = Rotator::from_degrees(0.0f, telemetry.yaw_deg, 0.0f);
    const Vec3 anim_eye = body.forward() * ca.eye.x + body.right() * ca.eye.y + Vec3(0.0f, 0.0f, ca.eye.z);
    out_pos = telemetry.position + still_eye + (anim_eye - still_eye) * ca.weight;
    // CalcCamera adds the camera animation to the view rotation component-wise; the rotator's axes stay
    // continuous past +-90 deg of pitch, so the view can turn all the way over.
    out_rot = Rotator::from_degrees(telemetry.pitch_deg + ca.pitch_deg, telemetry.yaw_deg + ca.yaw_deg,
                                    telemetry.camera_roll_deg + ca.roll_deg);
}

// -----------------------------------------------------------------------------
// Static draw-order index lists for evaluate_enemy_swat_indexed()
// -----------------------------------------------------------------------------
AnimSystem::EnemyArchetypeId AnimSystem::resolve_enemy_archetype(const std::string& archetype_name) const {
    std::string low = to_lower_str(archetype_name);
    EnemyArchetypeId cand = EnemyArch_SWAT;
    if (low.find("celeste") != std::string::npos || low.find("tutorial") != std::string::npos) {
        cand = EnemyArch_Celeste;
    } else if (low.find("pursuit") != std::string::npos) {
        cand = EnemyArch_Pursuit;
    } else if (low.find("riot") != std::string::npos) {
        cand = EnemyArch_Riot;
    } else if (low.find("support") != std::string::npos || low.find("gunner") != std::string::npos || low.find("heavy") != std::string::npos) {
        cand = EnemyArch_Support;
    } else if (low.find("patrol") != std::string::npos) {
        cand = EnemyArch_Patrol;
    } else {
        cand = EnemyArch_SWAT;
    }
    if (enemy_models_[cand].mesh.is_valid()) return cand;
    return EnemyArch_SWAT;
}

void AnimSystem::build_enemy_swat_index_lists() {
    enemy_swat_index_lists_.clear();
    enemy_swat_max_vertices_ = 0;
    for (uint32_t a = 0; a < EnemyArch_Count; ++a) {
        enemy_body_only_index_list_[a] = 0;
        enemy_weapon_index_lists_[a].clear();
    }
    if (!swat_mesh_.is_valid()) return;

    auto append_corners = [](const SkeletalMeshAsset& mesh, uint32_t base, std::vector<uint32_t>& out) {
        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            for (int k = 0; k < 3; ++k) {
                uint16_t vi = mesh.indices[i + k];
                if (vi >= mesh.vertices.size()) continue;
                out.push_back(base + vi);
            }
        }
    };

    for (uint32_t a = 0; a < EnemyArch_Count; ++a) {
        const SkeletalMeshAsset& bmesh = enemy_models_[a].mesh.is_valid() ? enemy_models_[a].mesh : swat_mesh_;
        const size_t body_vertices = bmesh.vertices.size();
        std::vector<uint32_t> body;
        append_corners(bmesh, 0, body);
        size_t body_list_idx = enemy_swat_index_lists_.size();
        enemy_body_only_index_list_[a] = body_list_idx;
        enemy_swat_index_lists_.push_back(std::move(body));
        enemy_swat_max_vertices_ = std::max(enemy_swat_max_vertices_, body_vertices);

        auto add_weapon = [&](const SkeletalMeshAsset& wmesh) {
            if (!wmesh.is_valid() || enemy_weapon_index_lists_[a].count(&wmesh)) return;
            std::vector<uint32_t> list = enemy_swat_index_lists_[body_list_idx];
            append_corners(wmesh, static_cast<uint32_t>(body_vertices), list);
            const size_t flash_base = body_vertices + wmesh.vertices.size();
            for (size_t f = 0; f < kMuzzleFlashVertexCount; ++f) list.push_back(static_cast<uint32_t>(flash_base + f));
            enemy_weapon_index_lists_[a][&wmesh] = enemy_swat_index_lists_.size();
            enemy_swat_index_lists_.push_back(std::move(list));
            enemy_swat_max_vertices_ = std::max(enemy_swat_max_vertices_, flash_base + kMuzzleFlashVertexCount);
        };
        add_weapon(colt1911_mesh_);
        for (const auto& [wname, wmesh] : weapon_meshes_) add_weapon(wmesh);
    }
}

// -----------------------------------------------------------------------------
// Evaluate KrugerSec / CPF Officer / Runner / Celeste Skeletal Mesh + Weapon
// -----------------------------------------------------------------------------
void AnimSystem::evaluate_enemy_swat(const EnemyBot& bot, float sim_time, bool reaction_disarm, std::vector<Vertex>& out_triangles) const {
    out_triangles.clear();
    thread_local std::vector<Vertex> unique_vertices;
    unique_vertices.resize(enemy_swat_max_vertices_);
    const EnemySwatDraw draw = evaluate_enemy_swat_indexed(bot, sim_time, reaction_disarm, unique_vertices.data());
    if (draw.index_count == 0) return;
    const std::vector<uint32_t>& indices = enemy_swat_index_lists_[draw.index_list];
    out_triangles.reserve(draw.index_count);
    for (size_t k = 0; k < draw.index_count; ++k) {
        out_triangles.push_back(unique_vertices[indices[k]]);
    }
}

AnimSystem::EnemySwatDraw AnimSystem::evaluate_enemy_swat_indexed(const EnemyBot& bot, float sim_time, bool reaction_disarm,
                                                                  Vertex* out_vertices) const {
    EnemySwatDraw draw;
    if (!loaded_ || !swat_mesh_.is_valid() || enemy_swat_index_lists_.empty()) return draw;

    const EnemyArchetypeId arch_id = resolve_enemy_archetype(bot.archetype);
    const EnemyCharacterModel& char_model = enemy_models_[arch_id];
    const SkeletalMeshAsset& body_mesh = char_model.mesh.is_valid() ? char_model.mesh : swat_mesh_;
    draw.archetype_id = arch_id;

    bool two_handed = is_heavy_weapon_name(bot.weapon_name);
    const AnimSetAsset* active_set = (two_handed && !swat_2h_set_.sequences.empty()) ? &swat_2h_set_ : &swat_set_;
    if (arch_id == EnemyArch_Celeste && !celeste_set_.sequences.empty() && !two_handed) {
        active_set = &celeste_set_;
    }
    const AnimSequenceAsset* seq_a = nullptr;
    const AnimSequenceAsset* seq_b = nullptr;
    float blend_alpha = 0.0f;
    float norm_time = 0.0f;

    auto find_ai_seq = [&](const std::string& sname) -> const AnimSequenceAsset* {
        if (const auto* s = active_set->find_sequence(sname)) return s;
        return swat_set_.find_sequence(sname);
    };

    if (!bot.alive || bot.anim_state == EEnemyAnimState::KnockedOut) {
        std::string dseq = bot.active_anim_seq.empty() ? "DeathByAuto" : bot.active_anim_seq;
        seq_a = find_ai_seq(dseq);
        if (!seq_a) seq_a = find_ai_seq("HitMeleeSlide");
        norm_time = std::clamp(bot.anim_timer / std::max(0.25f, bot.anim_duration), 0.0f, 0.96f);
        if (bot.anim_timer <= 0.0f) norm_time = 0.92f;
    } else if (bot.anim_state == EEnemyAnimState::BeingDisarmed) {
        std::string sseq = bot.active_anim_seq.empty() ? "SnatchFwd" : bot.active_anim_seq;
        seq_a = find_ai_seq(sseq);
        norm_time = std::clamp(bot.anim_timer / std::max(0.25f, bot.anim_duration), 0.0f, 0.95f);
    } else if (bot.stunned || bot.anim_state == EEnemyAnimState::HitStagger) {
        std::string hseq = bot.active_anim_seq.empty() ? "HitHeavyHead" : bot.active_anim_seq;
        seq_a = find_ai_seq(hseq);
        if (!seq_a) seq_a = find_ai_seq("HitHeavyHead");
        norm_time = (bot.anim_timer > 0.0f)
                        ? std::clamp(bot.anim_timer / std::max(0.25f, bot.anim_duration), 0.15f, 0.90f)
                        : 0.45f;
    } else if (bot.anim_state == EEnemyAnimState::MeleeWindup || (reaction_disarm && bot.disarm_window)) {
        seq_a = find_ai_seq("MeleeStart");
        seq_b = find_ai_seq("SnatchFwd");
        if (!seq_a) seq_a = find_ai_seq("SnatchFwd");
        norm_time = (bot.anim_timer > 0.0f)
                        ? std::clamp(bot.anim_timer / std::max(0.20f, bot.anim_duration), 0.15f, 0.85f)
                        : 0.24f;
        blend_alpha = 0.35f;
    } else if (bot.anim_state == EEnemyAnimState::MeleeStrike) {
        seq_a = find_ai_seq("MeleeEnd");
        if (!seq_a) seq_a = find_ai_seq("MeleeMiss");
        norm_time = std::clamp(bot.anim_timer / std::max(0.20f, bot.anim_duration), 0.0f, 0.95f);
    } else if (bot.anim_state == EEnemyAnimState::Chase || bot.velocity.length_xy() > 140.0f) {
        seq_a = find_ai_seq("runfwd");
        norm_time = std::fmod(sim_time * 1.5f, 1.0f);
    } else if (bot.anim_state == EEnemyAnimState::Patrol || bot.velocity.length_xy() > 20.0f) {
        seq_a = find_ai_seq("walkfwd");
        norm_time = std::fmod(sim_time * 1.1f, 1.0f);
    } else {
        seq_a = find_ai_seq("standfire");
        seq_b = find_ai_seq("Stand");
        norm_time = std::fmod(sim_time * 1.2f, 1.0f);
        blend_alpha = 0.25f;
    }

    if (!seq_a) {
        active_set = &swat_set_;
        seq_a = swat_set_.find_sequence("Stand");
    }

    // Ensure active_set owns seq_a for track mapping
    const AnimSetAsset* owner_a = (active_set->find_sequence(seq_a->name) == seq_a) ? active_set : &swat_set_;

    thread_local std::vector<Vec3> local_pos, local_pos_b, comp_pos, delta_pos, skinned_pos, skinned_norm;
    thread_local std::vector<Quat4> local_quat, local_quat_b, comp_quat, delta_quat;
    sample_sequence_pose(body_mesh, *owner_a, seq_a, norm_time, local_pos, local_quat);
    if (seq_b && blend_alpha > 0.001f) {
        const AnimSetAsset* owner_b = (active_set->find_sequence(seq_b->name) == seq_b) ? active_set : &swat_set_;
        sample_sequence_pose(body_mesh, *owner_b, seq_b, norm_time, local_pos_b, local_quat_b);
        blend_local_poses(local_pos, local_quat, local_pos_b, local_quat_b, blend_alpha, local_pos, local_quat);
    }

    compute_skeleton_fk(body_mesh.bones, local_pos, local_quat, comp_pos, comp_quat);
    compute_skin_deltas(body_mesh, comp_pos, comp_quat, delta_pos, delta_quat);

    // Map raw component coordinates (X=Left, -Y=Up, +Z=Forward) to Unreal/Engine bot local space (+X=Forward, +Y=Right, +Z=Up)
    const size_t body_vertices = body_mesh.vertices.size();
    skinned_pos.resize(body_vertices);
    skinned_norm.resize(body_vertices);
    for (size_t i = 0; i < body_vertices; ++i) {
        const SkinnedVertex& sv = body_mesh.vertices[i];
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

    const SkeletalMeshAsset* bot_wmesh = (bot.alive && !bot.stunned && bot.weapon_name != "None" && !bot.weapon_name.empty())
                                             ? get_weapon_mesh(bot.weapon_name)
                                             : nullptr;

    // Body: one output vertex per skinned vertex; u2.x=1.0 for armor/uniform/skin, u2.x=1.25 for small eye/visor sub-chunks; u2.y=archetype_id
    for (size_t vi = 0; vi < body_vertices; ++vi) {
        const SkinnedVertex& sv = body_mesh.vertices[vi];
        const size_t chunk_v = (sv.chunk_index < char_model.chunk_vert_counts.size())
                                   ? char_model.chunk_vert_counts[sv.chunk_index]
                                   : body_vertices;
        const bool is_eye_or_visor = (arch_id != EnemyArch_Celeste && chunk_v > 0 && chunk_v < 400);
        Vertex out_v{};
        out_v.position = skinned_pos[vi];
        out_v.normal = skinned_norm[vi];
        out_v.tangent = Vec3(1.0f, 0.0f, 0.0f);
        out_v.u = sv.u;
        out_v.v = sv.v;
        out_v.u2 = is_eye_or_visor ? 1.25f : 1.0f;
        out_v.v2 = static_cast<float>(arch_id);
        out_v.color = 0xFFFFFFFF;
        out_vertices[vi] = out_v;
    }
    const size_t body_list_idx = enemy_body_only_index_list_[arch_id];
    draw.vertex_count = body_vertices;
    draw.index_list = body_list_idx;
    draw.index_count = enemy_swat_index_lists_[body_list_idx].size();

    // Attach weapon to officer's RightWeapon bone in component space (1:1 scale + animated RightWeapon rotation)
    if (bot_wmesh && bot_wmesh->is_valid()) {
        const auto it_list = enemy_weapon_index_lists_[arch_id].find(bot_wmesh);
        if (it_list == enemy_weapon_index_lists_[arch_id].end()) return draw;

        int32_t rw_idx = 0;
        auto it_rw = body_mesh.bone_name_to_index.find("rightweapon");
        if (it_rw == body_mesh.bone_name_to_index.end()) {
            it_rw = body_mesh.bone_name_to_index.find("righthand");
        }
        if (it_rw != body_mesh.bone_name_to_index.end()) rw_idx = it_rw->second;

        const Vec3 rw_pos = comp_pos[rw_idx];
        const Quat4 rw_quat = comp_quat[rw_idx];
        uint32_t disarm_red = pack_rgba8(0.95f, 0.08f, 0.08f);

        Vertex* weapon_out = out_vertices + body_vertices;
        for (size_t vi = 0; vi < bot_wmesh->vertices.size(); ++vi) {
            const SkinnedVertex& sv = bot_wmesh->vertices[vi];
            Vec3 p_comp = rw_pos + rw_quat.rotate(sv.bind_pos);
            Vec3 n_comp = rw_quat.rotate(sv.bind_norm);
            Vertex out_v{};
            out_v.position = Vec3(p_comp.z, -p_comp.x, -p_comp.y);
            out_v.normal = Vec3(n_comp.z, -n_comp.x, -n_comp.y).normalized();
            out_v.tangent = Vec3(1.0f, 0.0f, 0.0f);
            out_v.u = sv.u;
            out_v.v = sv.v;
            out_v.u2 = (sv.mat_type == 1) ? 3.0f : 2.0f;
            out_v.v2 = 0.0f;
            out_v.color = bot.disarm_window ? disarm_red : 0xFFFFFFFF;
            weapon_out[vi] = out_v;
        }
        draw.vertex_count += bot_wmesh->vertices.size();
        draw.index_list = it_list->second;
        draw.index_count = enemy_swat_index_lists_[draw.index_list].size() - kMuzzleFlashVertexCount;

        // Emit 3D Muzzle Flash at enemy weapon barrel tip when firing (its corners are the list's last indices)
        if (bot.muzzle_flash_timer > 0.0f && bot_wmesh->bones.size() > 3) {
            Vec3 flash_comp = rw_pos + rw_quat.rotate(bot_wmesh->bones[3].bind_pos);
            Vec3 flash_local(flash_comp.z, -flash_comp.x, -flash_comp.y);
            Vec3 fwd_comp = rw_quat.rotate(Vec3(0.0f, 0.0f, 1.0f));
            Vec3 right_comp = rw_quat.rotate(Vec3(-1.0f, 0.0f, 0.0f));
            Vec3 up_comp = rw_quat.rotate(Vec3(0.0f, -1.0f, 0.0f));
            thread_local std::vector<Vertex> flash;
            flash.clear();
            append_muzzle_flash_mesh(flash, flash_local,
                                     Vec3(fwd_comp.z, -fwd_comp.x, -fwd_comp.y).normalized(),
                                     Vec3(right_comp.z, -right_comp.x, -right_comp.y).normalized(),
                                     Vec3(up_comp.z, -up_comp.x, -up_comp.y).normalized(),
                                     9.0f, 16.0f);
            const size_t flash_vertices = std::min(flash.size(), kMuzzleFlashVertexCount);
            std::copy_n(flash.begin(), flash_vertices, out_vertices + draw.vertex_count);
            draw.vertex_count += flash_vertices;
            draw.index_count += flash_vertices;
        }
    }
    return draw;
}

// -----------------------------------------------------------------------------
// Evaluate 3D Dropped Weapons & Active Ballistic Bullet Tracers in World Space
// -----------------------------------------------------------------------------
void AnimSystem::evaluate_combat_world_fx(const LevelScene& scene, float /*sim_time*/,
                                          std::vector<Vertex>& out_world_tris,
                                          std::vector<Vertex>& out_rv_tris) const {
    out_world_tris.clear();
    out_rv_tris.clear();

    // 1. Render 3D Dropped Weapons on the ground / flying through air
    for (const auto& dw : scene.dropped_weapons) {
        const SkeletalMeshAsset* wmesh = get_weapon_mesh(dw.weapon_name);
        if (!wmesh || !wmesh->is_valid()) continue;

        float cy = std::cos(dw.yaw_deg * DEG2RAD);
        float sy = std::sin(dw.yaw_deg * DEG2RAD);
        float cr = std::cos(dw.roll_deg * DEG2RAD);
        float sr = std::sin(dw.roll_deg * DEG2RAD);
        const float scale = dw.is_heavy ? 0.62f : 0.72f;
        Vec3 trigger_bind = (wmesh->bones.size() > 2) ? wmesh->bones[2].bind_pos : Vec3(0.0f, 0.0f, 10.0f);

        for (size_t i = 0; i + 2 < wmesh->indices.size(); i += 3) {
            for (int k = 0; k < 3; ++k) {
                uint16_t vi = wmesh->indices[i + k];
                if (vi >= wmesh->vertices.size()) continue;
                const SkinnedVertex& sv = wmesh->vertices[vi];
                Vec3 rel = (sv.bind_pos - trigger_bind) * scale;
                // Local weapon (+X=Forward, +Y=Right, +Z=Up) then roll on side and yaw in world
                float lx = rel.z;
                float ly = -rel.x * cr - (-rel.y) * sr;
                float lz = -rel.x * sr + (-rel.y) * cr;
                Vec3 wp = dw.position + Vec3(lx * cy - ly * sy, lx * sy + ly * cy, lz + 4.0f);

                float nx = sv.bind_norm.z;
                float ny = -sv.bind_norm.x * cr - (-sv.bind_norm.y) * sr;
                float nz = -sv.bind_norm.x * sr + (-sv.bind_norm.y) * cr;
                Vec3 wn = Vec3(nx * cy - ny * sy, nx * sy + ny * cy, nz).normalized();

                Vertex out_v{};
                out_v.position = wp;
                out_v.normal = wn;
                out_v.tangent = Vec3(1.0f, 0.0f, 0.0f);
                out_v.u = sv.u;
                out_v.v = sv.v;
                out_v.u2 = (sv.mat_type == 1) ? 3.0f : 2.0f;
                out_v.color = 0xFFFFFFFF;
                out_world_tris.push_back(out_v);
            }
        }
    }

    // 2. Render 3D Ballistic Bullet Tracers & Impact Sparks
    for (const auto& tr : scene.active_tracers) {
        Vec3 diff = tr.end_pos - tr.start_pos;
        float len = diff.length();
        if (len < 5.0f) continue;
        Vec3 dir = diff * (1.0f / len);
        Vec3 up = (std::abs(dir.z) < 0.9f) ? Vec3(0.0f, 0.0f, 1.0f) : Vec3(1.0f, 0.0f, 0.0f);
        Vec3 side = dir.cross(up).normalized();
        Vec3 ortho = side.cross(dir).normalized();

        float alpha = std::clamp(tr.timer / std::max(0.01f, tr.max_time), 0.0f, 1.0f);
        // Animate streak segment traveling rapidly from start_pos to end_pos
        float head_t = std::clamp(1.0f - alpha * 0.35f, 0.25f, 1.0f);
        float tail_t = std::max(0.0f, head_t - 0.65f);
        Vec3 p0 = tr.start_pos + diff * tail_t;
        Vec3 p1 = tr.start_pos + diff * head_t;

        float half_w = tr.from_player ? 1.3f : 1.6f;
        uint32_t col = tr.from_player ? pack_rgba8(1.0f, 0.92f, 0.55f) : pack_rgba8(1.0f, 0.45f, 0.18f);

        auto add_quad = [&](const Vec3& axis) {
            Vec3 a = p0 - axis * half_w;
            Vec3 b = p0 + axis * half_w;
            Vec3 c = p1 + axis * half_w;
            Vec3 d = p1 - axis * half_w;
            Vec3 n = Vec3(0.0f, 0.0f, 1.0f);
            Vertex va{a, n, {1, 0, 0}, 0.0f, 0.0f, 0.0f, 0.0f, col};
            Vertex vb{b, n, {1, 0, 0}, 1.0f, 0.0f, 0.0f, 0.0f, col};
            Vertex vc{c, n, {1, 0, 0}, 1.0f, 1.0f, 0.0f, 0.0f, col};
            Vertex vd{d, n, {1, 0, 0}, 0.0f, 1.0f, 0.0f, 0.0f, col};
            out_world_tris.push_back(va); out_world_tris.push_back(vb); out_world_tris.push_back(vc);
            out_world_tris.push_back(va); out_world_tris.push_back(vc); out_world_tris.push_back(vd);
            out_world_tris.push_back(va); out_world_tris.push_back(vc); out_world_tris.push_back(vb);
            out_world_tris.push_back(va); out_world_tris.push_back(vd); out_world_tris.push_back(vc);
        };
        add_quad(side);
        add_quad(ortho);

        // Impact spark burst at end_pos
        if (alpha > 0.3f) {
            append_muzzle_flash_mesh(tr.hit_enemy ? out_rv_tris : out_world_tris,
                                     tr.end_pos - dir * 3.0f,
                                     dir * -1.0f, side, ortho,
                                     tr.hit_enemy ? 10.0f : 6.0f,
                                     tr.hit_enemy ? 16.0f : 10.0f);
        }
    }

    // 3. Render Active SWAT Blackhawk Helicopters (SK_SWAT_Blackhawk_01) with Spinning Main & Tail Rotors
    if (heli_mesh_.is_valid()) {
        for (const auto& heli : scene.helicopters) {
            if (heli.state == EHeliState::Dormant || heli.state == EHeliState::Destroyed) continue;

            const float cy = std::cos(heli.yaw_deg * DEG2RAD);
            const float sy = std::sin(heli.yaw_deg * DEG2RAD);
            const float cp = std::cos(heli.pitch_deg * DEG2RAD);
            const float sp = std::sin(heli.pitch_deg * DEG2RAD);
            const float cr = std::cos(heli.roll_deg * DEG2RAD);
            const float sr = std::sin(heli.roll_deg * DEG2RAD);

            const float cmr = std::cos(heli.main_rotor_rad);
            const float smr = std::sin(heli.main_rotor_rad);
            const float ctr = std::cos(heli.tail_rotor_rad);
            const float str = std::sin(heli.tail_rotor_rad);
            const Vec3 tail_hub(0.0f, -80.2f, -1124.2f); // Bone [4] VH_Extra2 tail rotor mast pivot

            auto xform_heli_pt = [&](Vec3 p, uint8_t b0, bool is_normal) -> Vec3 {
                // Spin main rotor (bones 2 & 3) around local Y (up in Blackhawk rig) and tail rotor (bone 4) around local X
                if (b0 == 2 || b0 == 3) {
                    float rx = p.x * cmr - p.z * smr;
                    float rz = p.x * smr + p.z * cmr;
                    p.x = rx;
                    p.z = rz;
                } else if (b0 == 4) {
                    Vec3 rel = is_normal ? p : (p - tail_hub);
                    float ry = rel.y * ctr - rel.z * str;
                    float rz = rel.y * str + rel.z * ctr;
                    p = is_normal ? Vec3(rel.x, ry, rz) : (tail_hub + Vec3(rel.x, ry, rz));
                }
                // Map Blackhawk skeletal rig (+Z nose forward, +X right, -Y up) to UE world axes (+X forward, +Y right, +Z up)
                float lx = p.z;
                float ly = p.x;
                float lz = -p.y;
                // Apply roll & pitch in local aircraft frame, then yaw into world space
                float r_y = ly * cr - lz * sr;
                float r_z = ly * sr + lz * cr;
                float p_x = lx * cp + r_z * sp;
                float p_z = -lx * sp + r_z * cp;
                float w_x = p_x * cy - r_y * sy;
                float w_y = p_x * sy + r_y * cy;
                return is_normal ? Vec3(w_x, w_y, p_z).normalized() : (heli.position + Vec3(w_x, w_y, p_z));
            };

            for (size_t i = 0; i + 2 < heli_mesh_.indices.size(); i += 3) {
                for (int k = 0; k < 3; ++k) {
                    uint16_t vi = heli_mesh_.indices[i + k];
                    if (vi >= heli_mesh_.vertices.size()) continue;
                    const SkinnedVertex& sv = heli_mesh_.vertices[vi];
                    Vertex out_v{};
                    out_v.position = xform_heli_pt(sv.bind_pos, sv.bones[0], false);
                    out_v.normal = xform_heli_pt(sv.bind_norm, sv.bones[0], true);
                    out_v.tangent = Vec3(cy, sy, 0.0f);
                    out_v.u = sv.u;
                    out_v.v = sv.v;
                    out_v.u2 = 0.0f; // vertex color sampled from T_blackhawk_outside_D with hemisphere + sun lighting
                    out_v.color = sv.color;
                    out_world_tris.push_back(out_v);
                }
            }

            // Door gunner muzzle flash when firing FNMinimi bursts
            if (heli.muzzle_flash_timer > 0.0f) {
                Vec3 fwd(cy, sy, 0.0f);
                Vec3 right(-sy, cy, 0.0f);
                float side_sign = (heli.side_preference == EHeliAttackSide::Right ||
                                   heli.side_preference == EHeliAttackSide::UseRightWhenHovering) ? 1.0f : -1.0f;
                Vec3 gun_mount = heli.position + right * (side_sign * 165.0f) - Vec3(0.0f, 0.0f, 45.0f);
                append_muzzle_flash_mesh(out_world_tris, gun_mount, right * side_sign, fwd, Vec3(0.0f, 0.0f, 1.0f), 26.0f, 42.0f);
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
    if (!heli_mesh_.is_valid() || heli_mesh_.bones.size() != 6) return false;
    if (weapon_meshes_.size() < 10) return false;
    if (faith_unarmed_set_.sequences.size() < 250) return false;
    if (swat_set_.sequences.size() < 90) return false;

    PlayerTelemetry dummy_tel{};
    dummy_tel.move_state = EMovement::MOVE_Walking;
    dummy_tel.speed_2d = 600.0f;
    dummy_tel.sim_time = 0.5f;
    std::vector<Vertex> tris_1p;
    evaluate_faith_1p(dummy_tel, tris_1p);
    if (tris_1p.size() < 6000) return false;

    // Verify fall-death sequences (fallinguncontrolled & FallingLandDie)
    if (!faith_unarmed_set_.find_sequence("fallinguncontrolled") ||
        !faith_unarmed_set_.find_sequence("FallingLandDie")) {
        return false;
    }
    dummy_tel.falling_to_death = true;
    evaluate_faith_1p(dummy_tel, tris_1p);
    if (tris_1p.size() < 6000) return false;
    dummy_tel.fall_death_impact = true;
    dummy_tel.death_anim_progress = 0.5f;
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
