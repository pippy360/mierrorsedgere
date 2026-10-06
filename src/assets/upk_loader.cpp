#include "upk_loader.hpp"
#include "material_system.hpp"
#include "package_manager.hpp"
#include "ue3_props.hpp"
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <zlib.h>

namespace me {

namespace {

// IEEE 754 binary16 -> float (FVector2DHalf texture coordinates)
float half_to_float(uint16_t h) {
    const uint32_t sign = (h >> 15) & 1u;
    const uint32_t exp = (h >> 10) & 0x1Fu;
    const uint32_t mant = h & 0x3FFu;
    float f;
    if (exp == 0) {
        f = std::ldexp(static_cast<float>(mant), -24);
    } else if (exp == 31) {
        f = mant ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
    } else {
        f = std::ldexp(static_cast<float>(mant | 0x400u), static_cast<int>(exp) - 25);
    }
    return sign ? -f : f;
}

// Helper to safely read little-endian primitives
template <typename T>
T read_val(const uint8_t*& ptr, const uint8_t* end) {
    if (!ptr || ptr >= end || ptr + sizeof(T) > end) {
        ptr = end;
        return T{};
    }
    T val;
    std::memcpy(&val, ptr, sizeof(T));
    ptr += sizeof(T);
    return val;
}

std::string read_fstring(const uint8_t*& ptr, const uint8_t* end) {
    if (!ptr || ptr + 4 > end) return "";
    int32_t len = read_val<int32_t>(ptr, end);
    if (len > 0) {
        if (ptr + len > end) {
            ptr = end;
            return "";
        }
        std::string s(reinterpret_cast<const char*>(ptr), len - 1);
        ptr += len;
        return s;
    } else if (len < 0) {
        int32_t chars = -len;
        if (ptr + chars * 2 > end) {
            ptr = end;
            return "";
        }
        std::string utf8;
        utf8.reserve(chars);
        for (int32_t i = 0; i < chars - 1; ++i) {
            uint16_t w = static_cast<uint16_t>(ptr[i * 2]) | (static_cast<uint16_t>(ptr[i * 2 + 1]) << 8);
            if (w < 0x80) {
                utf8.push_back(static_cast<char>(w));
            } else if (w < 0x800) {
                utf8.push_back(static_cast<char>(0xC0 | (w >> 6)));
                utf8.push_back(static_cast<char>(0x80 | (w & 0x3F)));
            } else {
                utf8.push_back(static_cast<char>(0xE0 | (w >> 12)));
                utf8.push_back(static_cast<char>(0x80 | ((w >> 6) & 0x3F)));
                utf8.push_back(static_cast<char>(0x80 | (w & 0x3F)));
            }
        }
        ptr += chars * 2;
        return utf8;
    }
    return "";
}

// Generate box vertices for visual and physical geometry (6 triangle vertices per face)
void add_box_mesh(std::vector<Vertex>& verts, std::vector<uint32_t>& indices,
                  const Vec3& min_p, const Vec3& max_p, uint32_t color = 0xFFCCCCCC) {
    // 8 box corners
    Vec3 p[8] = {
        {min_p.x, min_p.y, min_p.z}, // 0
        {max_p.x, min_p.y, min_p.z}, // 1
        {max_p.x, max_p.y, min_p.z}, // 2
        {min_p.x, max_p.y, min_p.z}, // 3
        {min_p.x, min_p.y, max_p.z}, // 4
        {max_p.x, min_p.y, max_p.z}, // 5
        {max_p.x, max_p.y, max_p.z}, // 6
        {min_p.x, max_p.y, max_p.z}  // 7
    };

    struct Face {
        int idx[4];
        Vec3 normal;
    };

    Face faces[6] = {
        {{4, 5, 6, 7}, {0, 0, 1}},  // Top (+Z)
        {{3, 2, 1, 0}, {0, 0, -1}}, // Bottom (-Z)
        {{0, 1, 5, 4}, {0, -1, 0}}, // Front (-Y)
        {{2, 3, 7, 6}, {0, 1, 0}},  // Back (+Y)
        {{0, 4, 7, 3}, {-1, 0, 0}}, // Left (-X)
        {{1, 2, 6, 5}, {1, 0, 0}}   // Right (+X)
    };

    static const int tri_order[6] = {0, 1, 2, 0, 2, 3};
    for (int f = 0; f < 6; ++f) {
        for (int t = 0; t < 6; ++t) {
            int i = tri_order[t];
            Vertex v;
            v.position = p[faces[f].idx[i]];
            v.normal = faces[f].normal;
            v.tangent = Vec3(1, 0, 0);
            v.u = (i == 1 || i == 2) ? 1.0f : 0.0f;
            v.v = (i == 2 || i == 3) ? 1.0f : 0.0f;
            v.color = color;
            indices.push_back(static_cast<uint32_t>(verts.size()));
            verts.push_back(v);
        }
    }
}

} // namespace

// -----------------------------------------------------------------------------
// Pure C++20 LZO1X Decompressor
//
// Faithful port of the reference decoder (lzo1x_d.ch, "safe" variant: every input
// read, output write and look-behind distance is bounds checked). Handles all
// LZO1X instruction forms, including the 3-byte M1 match that may follow a
// literal run (distance 0x801 + ...), which LZO1X-999 compressed packages use.
// Returns true only if the stream decodes to exactly `expected_len` bytes.
// -----------------------------------------------------------------------------
bool UPKPackage::lzo1x_decompress(const uint8_t* src, size_t src_len, uint8_t* dst, size_t expected_len) {
    if (!src || src_len == 0 || !dst || expected_len == 0) {
        return false;
    }

    const uint8_t* ip = src;
    const uint8_t* const ip_end = src + src_len;
    uint8_t* op = dst;
    uint8_t* const op_end = dst + expected_len;
    size_t t = 0;
    size_t dist = 0;
    const uint8_t* m_pos = nullptr;

#define LZO_NEED_IP(n)                                                                  \
    do {                                                                                \
        if (static_cast<size_t>(ip_end - ip) < static_cast<size_t>(n)) return false;    \
    } while (0)
#define LZO_NEED_OP(n)                                                                  \
    do {                                                                                \
        if (static_cast<size_t>(op_end - op) < static_cast<size_t>(n)) return false;    \
    } while (0)
#define LZO_LOOKBEHIND(d)                                                               \
    do {                                                                                \
        if ((d) == 0 || (d) > static_cast<size_t>(op - dst)) return false;              \
    } while (0)

    if (*ip > 17) {
        t = static_cast<size_t>(*ip++) - 17;
        if (t < 4) goto match_next;
        LZO_NEED_OP(t);
        LZO_NEED_IP(t + 1);
        std::memcpy(op, ip, t);
        op += t;
        ip += t;
        goto first_literal_run;
    }

    for (;;) {
        LZO_NEED_IP(1);
        t = *ip++;
        if (t >= 16) goto match;
        // Literal run of t + 3 bytes (t == 0: extended length)
        if (t == 0) {
            for (;;) {
                LZO_NEED_IP(1);
                if (*ip != 0) break;
                t += 255;
                ip++;
            }
            t += 15 + static_cast<size_t>(*ip++);
        }
        LZO_NEED_OP(t + 3);
        LZO_NEED_IP(t + 3 + 1);  // literals + next instruction
        std::memcpy(op, ip, t + 3);
        op += t + 3;
        ip += t + 3;

    first_literal_run:
        t = *ip++;
        if (t >= 16) goto match;
        // M1 after a literal run: 3 bytes at distance 1 + 0x0800 + (t >> 2) + (next << 2)
        LZO_NEED_IP(1);
        dist = 1 + 0x0800 + (t >> 2) + (static_cast<size_t>(*ip++) << 2);
        LZO_LOOKBEHIND(dist);
        LZO_NEED_OP(3);
        m_pos = op - dist;
        op[0] = m_pos[0];
        op[1] = m_pos[1];
        op[2] = m_pos[2];
        op += 3;
        goto match_done;

        for (;;) {
        match:
            if (t >= 64) {
                // M2: 3..8 bytes, distance 1..0x800
                LZO_NEED_IP(1);
                dist = 1 + ((t >> 2) & 7) + (static_cast<size_t>(*ip++) << 3);
                t = (t >> 5) - 1;
            } else if (t >= 32) {
                // M3: distance 1..0x4000
                t &= 31;
                if (t == 0) {
                    for (;;) {
                        LZO_NEED_IP(1);
                        if (*ip != 0) break;
                        t += 255;
                        ip++;
                    }
                    t += 31 + static_cast<size_t>(*ip++);
                }
                LZO_NEED_IP(2);
                dist = 1 + (static_cast<size_t>(ip[0]) >> 2) + (static_cast<size_t>(ip[1]) << 6);
                ip += 2;
            } else if (t >= 16) {
                // M4: distance 0x4001..0xBFFF (offset 0 = end of stream)
                const size_t high = (t & 8) << 11;
                t &= 7;
                if (t == 0) {
                    for (;;) {
                        LZO_NEED_IP(1);
                        if (*ip != 0) break;
                        t += 255;
                        ip++;
                    }
                    t += 7 + static_cast<size_t>(*ip++);
                }
                LZO_NEED_IP(2);
                dist = high + (static_cast<size_t>(ip[0]) >> 2) + (static_cast<size_t>(ip[1]) << 6);
                ip += 2;
                if (dist == 0) goto eof_found;
                dist += 0x4000;
            } else {
                // M1 after a match's trailing literals: 2 bytes at distance 1 + (t >> 2) + (next << 2)
                LZO_NEED_IP(1);
                dist = 1 + (t >> 2) + (static_cast<size_t>(*ip++) << 2);
                LZO_LOOKBEHIND(dist);
                LZO_NEED_OP(2);
                m_pos = op - dist;
                op[0] = m_pos[0];
                op[1] = m_pos[1];
                op += 2;
                goto match_done;
            }

            // Copy t + 2 bytes; source and destination may overlap (run-length style), so go bytewise.
            LZO_LOOKBEHIND(dist);
            LZO_NEED_OP(t + 2);
            m_pos = op - dist;
            for (size_t k = 0; k < t + 2; ++k) op[k] = m_pos[k];
            op += t + 2;

        match_done:
            t = ip[-2] & 3;  // trailing literal count lives in the low bits of the instruction / offset byte
            if (t == 0) break;

        match_next:
            LZO_NEED_OP(t);
            LZO_NEED_IP(t + 1);  // trailing literals + next instruction
            for (size_t k = 0; k < t; ++k) op[k] = ip[k];
            op += t;
            ip += t;
            t = *ip++;
        }
    }

eof_found:
#undef LZO_NEED_IP
#undef LZO_NEED_OP
#undef LZO_LOOKBEHIND
    return op == op_end;
}

// -----------------------------------------------------------------------------
// UPKPackage Loader
// -----------------------------------------------------------------------------
UPKPackage::UPKPackage(const std::string& file_path) {
    load_from_file(file_path);
}

bool UPKPackage::load_from_file(const std::string& file_path) {
    file_path_ = file_path;
    valid_ = false;

    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    file.seekg(0, std::ios::end);
    size_t file_len = file.tellg();
    file.seekg(0, std::ios::beg);

    if (file_len < 64) {
        return false;
    }

    // Read initial header chunk
    size_t initial_read = std::min<size_t>(file_len, 65536);
    std::vector<uint8_t> header_buf(initial_read);
    file.read(reinterpret_cast<char*>(header_buf.data()), initial_read);

    const uint8_t* ptr = header_buf.data();
    const uint8_t* end = ptr + initial_read;

    uint32_t tag = read_val<uint32_t>(ptr, end);
    if (tag != 0x9E2A83C1) {
        return false;
    }

    uint16_t file_version = read_val<uint16_t>(ptr, end);
    uint16_t licensee_version = read_val<uint16_t>(ptr, end);
    int32_t total_header_size = read_val<int32_t>(ptr, end);
    std::string folder_name = read_fstring(ptr, end);

    package_flags_ = read_val<uint32_t>(ptr, end);
    int32_t name_count = read_val<int32_t>(ptr, end);
    int32_t name_offset = read_val<int32_t>(ptr, end);
    int32_t export_count = read_val<int32_t>(ptr, end);
    int32_t export_offset = read_val<int32_t>(ptr, end);
    int32_t import_count = read_val<int32_t>(ptr, end);
    int32_t import_offset = read_val<int32_t>(ptr, end);
    int32_t depends_offset = read_val<int32_t>(ptr, end);

    if (ptr + 16 <= end) ptr += 16; // GUID
    int32_t gen_count = read_val<int32_t>(ptr, end);
    if (gen_count > 0 && ptr + gen_count * 12 <= end) {
        ptr += gen_count * 12;
    }

    int32_t engine_version = read_val<int32_t>(ptr, end);
    int32_t cooker_version = read_val<int32_t>(ptr, end);
    compression_flags_ = read_val<uint32_t>(ptr, end);

    int32_t chunk_count = read_val<int32_t>(ptr, end);

    struct ChunkDesc {
        int32_t u_off;
        int32_t u_sz;
        int32_t c_off;
        int32_t c_sz;
    };
    std::vector<ChunkDesc> chunks;
    if (chunk_count > 0) {
        chunks.reserve(chunk_count);
        for (int32_t i = 0; i < chunk_count; ++i) {
            ChunkDesc c;
            c.u_off = read_val<int32_t>(ptr, end);
            c.u_sz = read_val<int32_t>(ptr, end);
            c.c_off = read_val<int32_t>(ptr, end);
            c.c_sz = read_val<int32_t>(ptr, end);
            chunks.push_back(c);
        }

        read_val<uint32_t>(ptr, end); // PackageSource
        int32_t add_count = read_val<int32_t>(ptr, end);
        for (int32_t i = 0; i < add_count; ++i) {
            additional_packages_.push_back(read_fstring(ptr, end));
        }
    }

    // Decompress / load full payload
    if (chunk_count <= 0 || chunks.empty()) {
        data_.resize(file_len);
        file.seekg(0, std::ios::beg);
        file.read(reinterpret_cast<char*>(data_.data()), file_len);
    } else {
        const auto& last_c = chunks.back();
        size_t total_uncomp = static_cast<size_t>(last_c.u_off) + static_cast<size_t>(last_c.u_sz);
        data_.resize(total_uncomp);

        size_t first_u_off = chunks.front().u_off;
        file.seekg(0, std::ios::beg);
        file.read(reinterpret_cast<char*>(data_.data()), first_u_off);

        size_t failed_blocks = 0;
        size_t total_blocks = 0;
        for (const auto& c : chunks) {
            file.seekg(c.c_off, std::ios::beg);
            uint32_t c_magic, blk_sz, tot_comp, tot_uncomp;
            file.read(reinterpret_cast<char*>(&c_magic), 4);
            file.read(reinterpret_cast<char*>(&blk_sz), 4);
            file.read(reinterpret_cast<char*>(&tot_comp), 4);
            file.read(reinterpret_cast<char*>(&tot_uncomp), 4);

            if (c_magic != 0x9E2A83C1 || blk_sz == 0) {
                return false;
            }

            uint32_t num_blks = (c.u_sz + blk_sz - 1) / blk_sz;
            std::vector<std::pair<uint32_t, uint32_t>> sub_blks(num_blks);
            for (uint32_t b = 0; b < num_blks; ++b) {
                file.read(reinterpret_cast<char*>(&sub_blks[b].first), 4);
                file.read(reinterpret_cast<char*>(&sub_blks[b].second), 4);
            }

            size_t cur_u_off = c.u_off;
            for (const auto& [s_comp, s_uncomp] : sub_blks) {
                std::vector<uint8_t> comp_buf(s_comp);
                file.read(reinterpret_cast<char*>(comp_buf.data()), s_comp);
                total_blocks++;

                bool ok = false;
                if (cur_u_off + s_uncomp <= data_.size()) {
                    if (compression_flags_ & 0x02) {
                        // LZO
                        ok = lzo1x_decompress(comp_buf.data(), s_comp, data_.data() + cur_u_off, s_uncomp);
                    } else if (compression_flags_ & 0x01) {
                        // ZLIB
                        uLongf dest_len = s_uncomp;
                        ok = uncompress(data_.data() + cur_u_off, &dest_len, comp_buf.data(), s_comp) == Z_OK &&
                             dest_len == s_uncomp;
                    } else {
                        std::memcpy(data_.data() + cur_u_off, comp_buf.data(), std::min<size_t>(s_comp, s_uncomp));
                        ok = true;
                    }
                }
                if (!ok) failed_blocks++;
                cur_u_off += s_uncomp;
            }
        }
        if (failed_blocks > 0) {
            std::cerr << "[UPKPackage] " << file_path << ": " << failed_blocks << "/" << total_blocks
                      << " compressed blocks failed to decompress" << std::endl;
        }
    }

    file.close();

    // Parse tables
    return parse_header_and_tables(data_.data(), data_.size());
}

bool UPKPackage::load_from_memory(const uint8_t* raw_data, size_t size, const std::string& name) {
    file_path_ = name;
    valid_ = false;
    if (!raw_data || size < 64) return false;
    data_.assign(raw_data, raw_data + size);
    return parse_header_and_tables(data_.data(), data_.size());
}

bool UPKPackage::parse_header_and_tables(const uint8_t* buf, size_t len) {
    if (!buf || len < 64) return false;

    const uint8_t* ptr = buf;
    const uint8_t* end = buf + len;

    uint32_t tag = read_val<uint32_t>(ptr, end);
    if (tag != 0x9E2A83C1) return false;

    read_val<uint16_t>(ptr, end); // file_version
    read_val<uint16_t>(ptr, end); // licensee_version
    int32_t total_header_size = read_val<int32_t>(ptr, end);
    std::string folder_name = read_fstring(ptr, end);

    package_flags_ = read_val<uint32_t>(ptr, end);
    int32_t name_count = read_val<int32_t>(ptr, end);
    int32_t name_offset = read_val<int32_t>(ptr, end);
    int32_t export_count = read_val<int32_t>(ptr, end);
    int32_t export_offset = read_val<int32_t>(ptr, end);
    int32_t import_count = read_val<int32_t>(ptr, end);
    int32_t import_offset = read_val<int32_t>(ptr, end);
    int32_t depends_offset = read_val<int32_t>(ptr, end);

    // 1. Name Table
    names_.clear();
    if (name_count > 0 && name_offset > 0 && name_offset < len) {
        names_.reserve(name_count);
        const uint8_t* n_ptr = buf + name_offset;
        for (int32_t i = 0; i < name_count && n_ptr < end; ++i) {
            std::string s = read_fstring(n_ptr, end);
            read_val<uint64_t>(n_ptr, end);
            names_.push_back(s);
        }
    }

    // 2. Import Table
    imports_.clear();
    if (import_count > 0 && import_offset > 0 && import_offset < len) {
        imports_.reserve(import_count);
        const uint8_t* imp_ptr = buf + import_offset;
        for (int32_t i = 0; i < import_count && imp_ptr + 28 <= end; ++i) {
            int32_t pkg_idx = read_val<int32_t>(imp_ptr, end);
            read_val<int32_t>(imp_ptr, end);
            int32_t cls_idx = read_val<int32_t>(imp_ptr, end);
            read_val<int32_t>(imp_ptr, end);
            int32_t outer_idx = read_val<int32_t>(imp_ptr, end);
            int32_t obj_idx = read_val<int32_t>(imp_ptr, end);
            int32_t obj_num = read_val<int32_t>(imp_ptr, end);

            FObjectImport imp;
            imp.index = i;
            imp.class_package = (pkg_idx >= 0 && pkg_idx < names_.size()) ? names_[pkg_idx] : std::to_string(pkg_idx);
            imp.class_name = (cls_idx >= 0 && cls_idx < names_.size()) ? names_[cls_idx] : std::to_string(cls_idx);
            imp.outer_index = outer_idx;
            imp.object_name = (obj_idx >= 0 && obj_idx < names_.size()) ? names_[obj_idx] : std::to_string(obj_idx);
            imp.object_number = obj_num;
            imports_.push_back(imp);
        }
    }

    // 3. Export Table
    exports_.clear();
    if (export_count > 0 && export_offset > 0 && export_offset < len) {
        exports_.reserve(export_count);
        const uint8_t* exp_ptr = buf + export_offset;
        for (int32_t i = 0; i < export_count && exp_ptr < end; ++i) {
            FObjectExport exp;
            exp.index = i;
            exp.class_index = read_val<int32_t>(exp_ptr, end);
            exp.super_index = read_val<int32_t>(exp_ptr, end);
            exp.outer_index = read_val<int32_t>(exp_ptr, end);
            int32_t name_idx = read_val<int32_t>(exp_ptr, end);
            exp.object_number = read_val<int32_t>(exp_ptr, end);
            exp.archetype = read_val<int32_t>(exp_ptr, end);
            exp.object_flags = read_val<uint64_t>(exp_ptr, end);
            exp.serial_size = read_val<int32_t>(exp_ptr, end);
            exp.serial_offset = read_val<int32_t>(exp_ptr, end);

            int32_t comp_count = read_val<int32_t>(exp_ptr, end);
            if (comp_count > 0 && comp_count < 1000) {
                for (int32_t c = 0; c < comp_count && exp_ptr + 12 <= end; ++c) {
                    int32_t c_n_idx = read_val<int32_t>(exp_ptr, end);
                    read_val<int32_t>(exp_ptr, end);
                    int32_t c_obj_idx = read_val<int32_t>(exp_ptr, end);
                    std::string c_name = (c_n_idx >= 0 && c_n_idx < names_.size()) ? names_[c_n_idx] : std::to_string(c_n_idx);
                    exp.component_map.emplace_back(c_name, c_obj_idx + 1);
                }
            }

            exp.export_flags = read_val<uint32_t>(exp_ptr, end);
            int32_t gen_cnt = read_val<int32_t>(exp_ptr, end);
            if (gen_cnt > 0 && gen_cnt < 100) {
                for (int32_t g = 0; g < gen_cnt && exp_ptr + 4 <= end; ++g) {
                    exp.gen_net_count.push_back(read_val<int32_t>(exp_ptr, end));
                }
            }

            if (exp_ptr + 16 <= end) exp_ptr += 16; // GUID
            exp.package_flags = read_val<uint32_t>(exp_ptr, end);
            exp.object_name = (name_idx >= 0 && name_idx < names_.size()) ? names_[name_idx] : std::to_string(name_idx);

            exports_.push_back(exp);
        }
    }

    valid_ = !names_.empty() && (!exports_.empty() || !imports_.empty());
    return valid_;
}

std::pair<std::string, std::string> UPKPackage::resolve_object_index(int32_t idx) const {
    if (idx > 0) {
        size_t exp_i = idx - 1;
        if (exp_i < exports_.size()) {
            const auto& exp = exports_[exp_i];
            auto [cls_name, _] = resolve_object_index(exp.class_index);
            return {exp.object_name, cls_name};
        }
        return {"Export_" + std::to_string(exp_i), "Unknown"};
    } else if (idx < 0) {
        size_t imp_i = -idx - 1;
        if (imp_i < imports_.size()) {
            const auto& imp = imports_[imp_i];
            return {imp.object_name, imp.class_name};
        }
        return {"Import_" + std::to_string(imp_i), "Unknown"};
    }
    return {"None", "None"};
}

std::string UPKPackage::get_export_class(const FObjectExport& exp) const {
    if (exp.class_index == 0) return "Class";
    auto [cls_name, _] = resolve_object_index(exp.class_index);
    return cls_name;
}

std::unordered_map<std::string, PropertyValue> UPKPackage::parse_properties(size_t offset, size_t size, size_t* out_bytes_read) const {
    std::unordered_map<std::string, PropertyValue> props;
    if (offset >= data_.size() || size == 0) return props;

    const uint8_t* ptr = data_.data() + offset;
    const uint8_t* start_ptr = ptr;
    size_t clamped_size = std::min(size, data_.size() - offset);
    const uint8_t* end = ptr + clamped_size;

    while (ptr + 8 <= end) {
        int32_t n_idx = read_val<int32_t>(ptr, end);
        read_val<int32_t>(ptr, end); // num
        if (n_idx < 0 || n_idx >= names_.size()) break;
        std::string prop_name = names_[n_idx];
        if (prop_name == "None") break;

        if (ptr + 16 > end) break;
        int32_t t_idx = read_val<int32_t>(ptr, end);
        read_val<int32_t>(ptr, end);
        std::string prop_type = (t_idx >= 0 && t_idx < names_.size()) ? names_[t_idx] : "Unknown";

        int32_t p_size = read_val<int32_t>(ptr, end);
        int32_t arr_idx = read_val<int32_t>(ptr, end);

        if (p_size < 0) break;

        PropertyValue pv;
        pv.name = prop_name;
        pv.type = prop_type;
        pv.size = p_size;
        pv.array_index = arr_idx;

        if (prop_type == "StructProperty") {
            if (ptr + 8 > end) break;
            int32_t s_idx = read_val<int32_t>(ptr, end);
            read_val<int32_t>(ptr, end);
            pv.struct_name = (s_idx >= 0 && s_idx < names_.size()) ? names_[s_idx] : "";
        } else if (prop_type == "BoolProperty") {
            if (ptr + 4 > end) break;
            pv.bool_val = (read_val<int32_t>(ptr, end) != 0);
        }
        // Note: In UE3 PackageVersion 536 (EngineVersion 3716), ByteProperty does NOT
        // store an 8-byte EnumName in FPropertyTag.

        if (ptr + p_size > end) break;
        const uint8_t* val_ptr = ptr;
        ptr += p_size;
        if (p_size > 0) {
            pv.raw_bytes.assign(val_ptr, val_ptr + p_size);
        }

        if (prop_type == "IntProperty" && p_size == 4) {
            std::memcpy(&pv.int_val, val_ptr, 4);
        } else if (prop_type == "FloatProperty" && p_size == 4) {
            std::memcpy(&pv.float_val, val_ptr, 4);
        } else if (prop_type == "ByteProperty") {
            if (p_size == 1) {
                pv.int_val = static_cast<int32_t>(val_ptr[0]);
            } else if (p_size == 8) {
                int32_t bn_idx = 0, bn_num = 0;
                std::memcpy(&bn_idx, val_ptr, 4);
                std::memcpy(&bn_num, val_ptr + 4, 4);
                if (bn_idx >= 0 && static_cast<size_t>(bn_idx) < names_.size()) {
                    pv.str_val = names_[bn_idx];
                    if (bn_num > 0) pv.str_val += "_" + std::to_string(bn_num - 1);
                    pv.enum_name = pv.str_val;
                }
            }
        } else if (prop_type == "ObjectProperty" && p_size == 4) {
            int32_t obj_ref = 0;
            std::memcpy(&obj_ref, val_ptr, 4);
            pv.obj_ref_index = obj_ref;
            auto [oname, ocls] = resolve_object_index(obj_ref);
            pv.obj_ref_name = oname;
            pv.obj_ref_class = ocls;
        } else if (prop_type == "NameProperty" && p_size == 8) {
            int32_t pn_idx = 0, pn_num = 0;
            std::memcpy(&pn_idx, val_ptr, 4);
            std::memcpy(&pn_num, val_ptr + 4, 4);
            if (pn_idx >= 0 && static_cast<size_t>(pn_idx) < names_.size()) {
                pv.str_val = names_[pn_idx];
                if (pn_num > 0) pv.str_val += "_" + std::to_string(pn_num - 1);
            }
        } else if (prop_type == "StrProperty") {
            const uint8_t* s_read = val_ptr;
            pv.str_val = read_fstring(s_read, val_ptr + p_size);
        } else if (prop_type == "StructProperty") {
            if (pv.struct_name == "Vector" && p_size == 12) {
                std::memcpy(&pv.vec_val.x, val_ptr, 4);
                std::memcpy(&pv.vec_val.y, val_ptr + 4, 4);
                std::memcpy(&pv.vec_val.z, val_ptr + 8, 4);
            } else if (pv.struct_name == "Rotator" && p_size == 12) {
                int32_t p = 0, y = 0, r = 0;
                std::memcpy(&p, val_ptr, 4);
                std::memcpy(&y, val_ptr + 4, 4);
                std::memcpy(&r, val_ptr + 8, 4);
                pv.rot_val = Rotator(static_cast<float>(p), static_cast<float>(y), static_cast<float>(r));
            }
        }

        props[prop_name] = pv;
    }

    if (out_bytes_read) {
        *out_bytes_read = ptr - start_ptr;
    }
    return props;
}

size_t UPKPackage::find_property_start(const FObjectExport& exp) const {
    if (exp.serial_offset < 0 || exp.serial_size < 8) return static_cast<size_t>(std::max(0, exp.serial_offset));
    size_t so = static_cast<size_t>(exp.serial_offset);
    size_t ss = static_cast<size_t>(exp.serial_size);
    if (so + ss > data_.size()) return so;

    static const std::set<std::string> kValidTypes = {
        "IntProperty", "FloatProperty", "BoolProperty", "ByteProperty",
        "ObjectProperty", "ComponentProperty", "NameProperty", "StrProperty",
        "StructProperty", "ArrayProperty", "DelegateProperty", "InterfaceProperty"
    };

    bool has_stack = (exp.object_flags & 0x0200000000000000ULL) != 0;
    const size_t candidates_stack[] = {32, 4, 8, 12, 16, 20, 24, 28, 36, 40, 44};
    const size_t candidates_no_stack[] = {4, 8, 32, 12, 16, 20, 24, 28};

    const size_t* cands = has_stack ? candidates_stack : candidates_no_stack;
    size_t num_cands = has_stack ? (sizeof(candidates_stack) / sizeof(size_t))
                                 : (sizeof(candidates_no_stack) / sizeof(size_t));

    for (size_t i = 0; i < num_cands; ++i) {
        size_t c = cands[i];
        if (c + 8 > ss) continue;
        int32_t n_idx = 0, n_num = 0;
        std::memcpy(&n_idx, data_.data() + so + c, 4);
        std::memcpy(&n_num, data_.data() + so + c + 4, 4);
        if (n_idx >= 0 && static_cast<size_t>(n_idx) < names_.size() && n_num >= 0) {
            const std::string& pname = names_[n_idx];
            if (pname == "None") {
                return so + c;
            }
            if (c + 24 <= ss) {
                int32_t t_idx = 0, t_num = 0, p_sz = 0;
                std::memcpy(&t_idx, data_.data() + so + c + 8, 4);
                std::memcpy(&t_num, data_.data() + so + c + 12, 4);
                std::memcpy(&p_sz, data_.data() + so + c + 16, 4);
                if (t_idx >= 0 && static_cast<size_t>(t_idx) < names_.size() &&
                    t_num == 0 && p_sz >= 0 && static_cast<size_t>(p_sz) <= ss &&
                    kValidTypes.find(names_[t_idx]) != kValidTypes.end()) {
                    return so + c;
                }
            }
        }
    }
    return so + (has_stack ? 32 : 4);
}

std::vector<LevelActor> UPKPackage::extract_actors() const {
    std::vector<LevelActor> actors;

    for (const auto& exp : exports_) {
        std::string cls_name = get_export_class(exp);
        if (cls_name.ends_with("Component") || cls_name == "Level" || cls_name == "World" ||
            cls_name == "Package" || cls_name == "Model" || cls_name == "Polys" ||
            cls_name == "StaticMesh" || cls_name.find("Material") != std::string::npos ||
            cls_name.find("Texture") != std::string::npos || cls_name.find("Sequence") != std::string::npos ||
            cls_name.find("SeqAct") != std::string::npos || cls_name.find("SeqEvent") != std::string::npos ||
            cls_name.find("SeqVar") != std::string::npos || cls_name.find("KMeshProps") != std::string::npos) {
            continue;
        }

        // Only extract actual instances placed in PersistentLevel (skip Prefab Archetypes!)
        auto [outer_name, _] = resolve_object_index(exp.outer_index);
        if (outer_name != "PersistentLevel") {
            continue;
        }

        if (exp.serial_size < 12 || exp.serial_offset < 0) continue;
        size_t prop_start = find_property_start(exp);
        size_t exp_end = static_cast<size_t>(exp.serial_offset) + static_cast<size_t>(exp.serial_size);
        if (prop_start >= data_.size() || prop_start >= exp_end) continue;

        size_t avail_sz = std::min<size_t>(exp_end - prop_start, data_.size() - prop_start);
        auto props = parse_properties(prop_start, avail_sz);

        // Also parse Archetype properties if this Actor inherits from an in-package Prefab Archetype
        std::unordered_map<std::string, PropertyValue> arch_props;
        if (exp.archetype > 0 && static_cast<size_t>(exp.archetype) <= exports_.size()) {
            const auto& aexp = exports_[exp.archetype - 1];
            size_t ap_start = find_property_start(aexp);
            size_t aexp_end = static_cast<size_t>(aexp.serial_offset) + static_cast<size_t>(aexp.serial_size);
            if (ap_start < data_.size() && ap_start < aexp_end) {
                arch_props = parse_properties(ap_start, std::min<size_t>(aexp_end - ap_start, data_.size() - ap_start));
            }
        }

        auto get_prop = [&](const std::string& key) -> const PropertyValue* {
            auto it = props.find(key);
            if (it != props.end()) return &it->second;
            auto ait = arch_props.find(key);
            if (ait != arch_props.end()) return &ait->second;
            return nullptr;
        };

        LevelActor a;
        a.class_name = cls_name;
        a.object_name = exp.object_name;

        if (const auto* p = get_prop("Location")) a.location = p->vec_val;
        if (const auto* p = get_prop("Rotation")) a.rotation = p->rot_val;
        if (const auto* p = get_prop("DrawScale")) a.draw_scale = p->float_val;
        if (const auto* p = get_prop("DrawScale3D")) a.draw_scale_3d = p->vec_val;
        if (const auto* p = get_prop("Tag")) a.tag = p->str_val;

        bool b_loi = false;
        if (const auto* p = get_prop("bLOIObject")) b_loi = p->bool_val;

        bool b_hidden = false;
        if (const auto* p = get_prop("bHidden")) b_hidden = p->bool_val;

        // Resolve StaticMeshComponent -> StaticMesh (checking both instance and archetype)
        int32_t comp_idx = 0;
        if (const auto* p = get_prop("StaticMeshComponent")) {
            comp_idx = p->obj_ref_index;
        }
        if (comp_idx <= 0) {
            for (const auto& [comp_name, c_idx] : exp.component_map) {
                if (comp_name.find("StaticMesh") != std::string::npos || comp_name.find("Mesh") != std::string::npos) {
                    comp_idx = c_idx;
                    break;
                }
            }
        }

        if (comp_idx > 0 && static_cast<size_t>(comp_idx) <= exports_.size()) {
            const auto& comp_exp = exports_[comp_idx - 1];
            size_t cp_start = find_property_start(comp_exp);
            size_t cexp_end = static_cast<size_t>(comp_exp.serial_offset) + static_cast<size_t>(comp_exp.serial_size);
            if (cp_start < data_.size() && cp_start < cexp_end) {
                auto comp_props = parse_properties(cp_start, std::min<size_t>(cexp_end - cp_start, data_.size() - cp_start));
                if (comp_props.find("StaticMesh") != comp_props.end()) {
                    a.mesh_name = comp_props["StaticMesh"].obj_ref_name;
                }
                if (comp_props.find("HiddenGame") != comp_props.end() && comp_props["HiddenGame"].bool_val) {
                    b_hidden = true;
                }
                if (comp_props.find("Scale") != comp_props.end() && comp_props["Scale"].float_val > 0.0f) {
                    a.draw_scale *= comp_props["Scale"].float_val;
                }
                if (comp_props.find("Scale3D") != comp_props.end()) {
                    a.draw_scale_3d.x *= comp_props["Scale3D"].vec_val.x;
                    a.draw_scale_3d.y *= comp_props["Scale3D"].vec_val.y;
                    a.draw_scale_3d.z *= comp_props["Scale3D"].vec_val.z;
                }
            }
            if (a.mesh_name.empty() && comp_exp.archetype > 0 && static_cast<size_t>(comp_exp.archetype) <= exports_.size()) {
                const auto& acomp_exp = exports_[comp_exp.archetype - 1];
                size_t acp_start = find_property_start(acomp_exp);
                size_t acexp_end = static_cast<size_t>(acomp_exp.serial_offset) + static_cast<size_t>(acomp_exp.serial_size);
                if (acp_start < data_.size() && acp_start < acexp_end) {
                    auto acomp_props = parse_properties(acp_start, std::min<size_t>(acexp_end - acp_start, data_.size() - acp_start));
                    if (acomp_props.find("StaticMesh") != acomp_props.end()) {
                        a.mesh_name = acomp_props["StaticMesh"].obj_ref_name;
                    }
                    if (acomp_props.find("HiddenGame") != acomp_props.end() && acomp_props["HiddenGame"].bool_val) {
                        b_hidden = true;
                    }
                }
            }

            // StaticMeshComponent.Materials[] overrides the mesh's per-element materials
            // (UStaticMeshComponent::GetMaterial). An instance that serializes the array replaces
            // the archetype's array entirely; otherwise the archetype's value is inherited.
            auto read_component_materials = [&](int32_t idx, bool& found) {
                std::vector<std::string> out;
                UPropertyList list;
                parse_export_properties(*this, idx, list);
                if (const UProperty* m = find_prop(list, "Materials")) {
                    found = true;
                    for (int32_t ref : m->ints) out.push_back(ref != 0 ? object_canonical_path(*this, ref) : std::string());
                }
                return out;
            };
            bool has_materials = false;
            a.material_overrides = read_component_materials(comp_idx, has_materials);
            if (!has_materials && comp_exp.archetype > 0 && static_cast<size_t>(comp_exp.archetype) <= exports_.size()) {
                a.material_overrides = read_component_materials(comp_exp.archetype, has_materials);
            }
        }

        // Classification
        std::string low_class = cls_name;
        std::transform(low_class.begin(), low_class.end(), low_class.begin(), ::tolower);
        std::string low_obj = exp.object_name;
        std::transform(low_obj.begin(), low_obj.end(), low_obj.begin(), ::tolower);
        std::string low_mesh = a.mesh_name;
        std::transform(low_mesh.begin(), low_mesh.end(), low_mesh.begin(), ::tolower);

        if (b_hidden || low_mesh.find("blockingbox") != std::string::npos || low_mesh.find("_colmesh") != std::string::npos) {
            a.mesh_name.clear();
            low_mesh.clear();
        }

        a.is_checkpoint = (low_class.find("checkpoint") != std::string::npos || low_obj.find("checkpoint") != std::string::npos);
        a.is_trigger = (low_class.find("trigger") != std::string::npos);
        a.is_zipline = (low_class.find("zipline") != std::string::npos || low_mesh.find("zipline") != std::string::npos);
        a.is_ladder = (low_class.find("ladder") != std::string::npos || low_mesh.find("ladder") != std::string::npos);
        a.is_ledge = (low_class.find("ledge") != std::string::npos);
        a.is_springboard = (low_class.find("springboard") != std::string::npos || low_obj.find("springboard") != std::string::npos || low_mesh.find("springboard") != std::string::npos);
        a.is_balance_beam = (low_class.find("balance") != std::string::npos);
        a.is_swing_bar = (low_class.find("swing") != std::string::npos || low_mesh.find("swingpole") != std::string::npos);
        a.is_enemy = (low_class.find("ai") != std::string::npos || low_class.find("botpawn") != std::string::npos || low_class.find("cop") != std::string::npos);
        a.is_bag = (low_class.find("bag") != std::string::npos || low_obj.find("bag") != std::string::npos || low_mesh.find("s_bag") != std::string::npos);
        a.is_runner_vision = b_loi || a.is_springboard || a.is_zipline || a.is_ladder || a.is_swing_bar || a.is_bag || (low_obj.find("runner") != std::string::npos);

        // Approximate initial world bounds (refined later when StaticMeshAsset is bound)
        float r_xy = 150.0f * std::abs(a.draw_scale) * std::max(std::abs(a.draw_scale_3d.x), std::abs(a.draw_scale_3d.y));
        float r_z = 100.0f * std::abs(a.draw_scale) * std::abs(a.draw_scale_3d.z);
        a.world_bounds = AABB(
            a.location - Vec3(r_xy, r_xy, r_z),
            a.location + Vec3(r_xy, r_xy, r_z)
        );

        actors.push_back(a);
    }

    return actors;
}

std::vector<SoundClip> UPKPackage::extract_audio() const {
    std::vector<SoundClip> sounds;

    for (const auto& exp : exports_) {
        std::string cls_name = get_export_class(exp);
        if (cls_name.find("SoundNodeWave") == std::string::npos) {
            continue;
        }

        if (exp.serial_size < 16 || exp.serial_offset < 0 ||
            static_cast<size_t>(exp.serial_offset) + exp.serial_size > data_.size()) {
            continue;
        }

        const uint8_t* p = data_.data() + exp.serial_offset;
        size_t sz = exp.serial_size;

        // Search for OggS magic: 0x4F, 0x67, 0x67, 0x53
        for (size_t i = 0; i + 4 <= sz; ++i) {
            if (p[i] == 'O' && p[i+1] == 'g' && p[i+2] == 'g' && p[i+3] == 'S') {
                size_t ogg_sz = sz - i;
                if (i >= 12) {
                    int32_t elem_cnt = 0;
                    int32_t disk_sz = 0;
                    std::memcpy(&elem_cnt, p + i - 12, 4);
                    std::memcpy(&disk_sz, p + i - 8, 4);
                    if (disk_sz > 0 && disk_sz <= sz - i) {
                        ogg_sz = disk_sz;
                    }
                }

                SoundClip clip;
                clip.name = exp.object_name;
                clip.pcm_data.assign(p + i, p + i + ogg_sz);
                clip.sample_rate = 44100;
                clip.channels = 2;
                clip.duration = static_cast<float>(ogg_sz) / (44100.0f * 4.0f);
                sounds.push_back(clip);
                break;
            }
        }
    }

    return sounds;
}

bool UPKPackage::extract_static_mesh_bounds(int32_t exp_idx, Vec3& out_origin, Vec3& out_extent, float& out_radius) const {
    if (exp_idx < 0 || static_cast<size_t>(exp_idx) >= exports_.size()) return false;
    const auto& exp = exports_[exp_idx];
    if (exp.serial_size < 36 || exp.serial_offset < 0) return false;

    size_t prop_start = find_property_start(exp);
    size_t exp_end = static_cast<size_t>(exp.serial_offset) + static_cast<size_t>(exp.serial_size);
    if (prop_start >= data_.size() || prop_start >= exp_end) return false;

    size_t bytes_read = 0;
    size_t avail = std::min<size_t>(exp_end - prop_start, data_.size() - prop_start);
    auto props = parse_properties(prop_start, avail, &bytes_read);

    size_t rem_off = prop_start + bytes_read;
    if (rem_off + 28 <= exp_end && rem_off + 28 <= data_.size()) {
        const uint8_t* p = data_.data() + rem_off;
        float ox, oy, oz, ex, ey, ez, r;
        std::memcpy(&ox, p, 4);
        std::memcpy(&oy, p + 4, 4);
        std::memcpy(&oz, p + 8, 4);
        std::memcpy(&ex, p + 12, 4);
        std::memcpy(&ey, p + 16, 4);
        std::memcpy(&ez, p + 20, 4);
        std::memcpy(&r,  p + 24, 4);
        out_origin = Vec3(ox, oy, oz);
        out_extent = Vec3(ex, ey, ez);
        out_radius = r;
        return true;
    }
    return false;
}

// -----------------------------------------------------------------------------
// Binary UStaticMesh LOD0 Vertex & Index Buffer Extractor (UE3 v536 / lic43)
// -----------------------------------------------------------------------------
void UPKPackage::extract_static_meshes(std::unordered_map<std::string, StaticMeshAsset>& out_meshes) const {
    for (const auto& exp : exports_) {
        if (get_export_class(exp) != "StaticMesh") continue;
        if (exp.serial_size < 64 || exp.serial_offset < 0) continue;

        std::string key = exp.object_name;
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        if (out_meshes.find(key) != out_meshes.end()) continue;

        size_t prop_start = find_property_start(exp);
        size_t exp_end = static_cast<size_t>(exp.serial_offset) + static_cast<size_t>(exp.serial_size);
        if (prop_start >= data_.size() || prop_start >= exp_end || exp_end > data_.size()) continue;

        size_t bytes_read = 0;
        parse_properties(prop_start, exp_end - prop_start, &bytes_read);

        size_t rem_start = prop_start + bytes_read;
        if (rem_start + 48 > exp_end) continue;
        const uint8_t* rem = data_.data() + rem_start;
        size_t rem_len = exp_end - rem_start;

        StaticMeshAsset asset;
        asset.name = exp.object_name;
        std::memcpy(&asset.bounds_origin.x, rem + 0, 4);
        std::memcpy(&asset.bounds_origin.y, rem + 4, 4);
        std::memcpy(&asset.bounds_origin.z, rem + 8, 4);
        std::memcpy(&asset.bounds_extent.x, rem + 12, 4);
        std::memcpy(&asset.bounds_extent.y, rem + 16, 4);
        std::memcpy(&asset.bounds_extent.z, rem + 20, 4);
        std::memcpy(&asset.bounds_radius,   rem + 24, 4);

        // Skip kDOP Nodes (rem + 32) and kDOP Triangles
        int32_t kdop_ns = 0, kdop_nc = 0;
        std::memcpy(&kdop_ns, rem + 32, 4);
        std::memcpy(&kdop_nc, rem + 36, 4);
        if (kdop_ns < 0 || kdop_nc < 0) continue;
        size_t off = 40 + static_cast<size_t>(kdop_ns) * static_cast<size_t>(kdop_nc);
        if (off + 8 > rem_len) continue;

        int32_t kdop_ts = 0, kdop_tc = 0;
        std::memcpy(&kdop_ts, rem + off, 4);
        std::memcpy(&kdop_tc, rem + off + 4, 4);
        if (kdop_ts < 0 || kdop_tc < 0) continue;
        off += 8 + static_cast<size_t>(kdop_ts) * static_cast<size_t>(kdop_tc);
        if (off + 24 > rem_len) continue;

        int32_t internal_ver = 0, lod_cnt = 0;
        std::memcpy(&internal_ver, rem + off, 4);
        std::memcpy(&lod_cnt, rem + off + 4, 4);
        if (lod_cnt <= 0 || lod_cnt > 8) continue;

        size_t lod0_off = off + 8;
        int32_t rt_flags = 0, rt_cnt = 0, rt_sz = 0;
        std::memcpy(&rt_flags, rem + lod0_off, 4);
        std::memcpy(&rt_cnt,   rem + lod0_off + 4, 4);
        std::memcpy(&rt_sz,    rem + lod0_off + 8, 4);
        size_t cur = lod0_off + 16;
        if ((rt_flags & 0x20) == 0 && rt_cnt > 0 && rt_sz > 0) {
            cur += static_cast<size_t>(rt_sz);
        }
        if (cur + 4 > rem_len) continue;

        int32_t elem_cnt = 0;
        std::memcpy(&elem_cnt, rem + cur, 4);
        cur += 4;
        if (elem_cnt < 0 || elem_cnt > 64) continue;
        // FStaticMeshElement: Material, EnableCollision, OldEnableCollision, bEnableShadowCasting,
        // FirstIndex, NumTriangles, MinVertexIndex, MaxVertexIndex, MaterialIndex, Fragments[]
        struct RawElement {
            int32_t material_ref = 0;
            int32_t first_index = 0;
            int32_t num_triangles = 0;
        };
        std::vector<RawElement> raw_elems;
        bool elem_ok = true;
        for (int32_t e = 0; e < elem_cnt; ++e) {
            if (cur + 40 > rem_len) { elem_ok = false; break; }
            RawElement re;
            std::memcpy(&re.material_ref, rem + cur, 4);
            std::memcpy(&re.first_index, rem + cur + 16, 4);
            std::memcpy(&re.num_triangles, rem + cur + 20, 4);
            int32_t frag_cnt = 0;
            std::memcpy(&frag_cnt, rem + cur + 36, 4);
            if (frag_cnt < 0 || frag_cnt > 10000) { elem_ok = false; break; }
            raw_elems.push_back(re);
            cur += 40 + static_cast<size_t>(frag_cnt) * 8;
        }
        if (!elem_ok || cur + 16 > rem_len) continue;

        // PositionVertexBuffer
        int32_t pos_stride = 0, pos_num = 0, pos_bsz = 0, pos_bcnt = 0;
        std::memcpy(&pos_stride, rem + cur, 4);
        std::memcpy(&pos_num,    rem + cur + 4, 4);
        std::memcpy(&pos_bsz,    rem + cur + 8, 4);
        std::memcpy(&pos_bcnt,   rem + cur + 12, 4);
        if (pos_stride != 12 || pos_bsz != 12 || pos_num != pos_bcnt || pos_num <= 0 || pos_num > 100000) continue;

        size_t pos_data_off = cur + 16;
        cur += 16 + static_cast<size_t>(pos_bsz) * static_cast<size_t>(pos_bcnt);
        if (cur + 24 > rem_len) continue;

        // StaticMeshVertexBuffer (TangentX, TangentZ/Normal, VertexColor, UVs)
        int32_t num_uv = 0, smvb_stride = 0, smvb_num = 0, full_prec = 0, smvb_bsz = 0, smvb_bcnt = 0;
        std::memcpy(&num_uv,      rem + cur, 4);
        std::memcpy(&smvb_stride, rem + cur + 4, 4);
        std::memcpy(&smvb_num,    rem + cur + 8, 4);
        std::memcpy(&full_prec,   rem + cur + 12, 4);
        std::memcpy(&smvb_bsz,    rem + cur + 16, 4);
        std::memcpy(&smvb_bcnt,   rem + cur + 20, 4);
        if (smvb_stride != smvb_bsz || smvb_num != smvb_bcnt || smvb_stride < 8 || smvb_num != pos_num) continue;

        size_t smvb_data_off = cur + 24;
        cur += 24 + static_cast<size_t>(smvb_bsz) * static_cast<size_t>(smvb_bcnt);
        if (cur + 8 > rem_len) continue;

        // ColorVertexBuffer / ShadowExtrusionVertexBuffer
        int32_t cvb_stride = 0, cvb_num = 0;
        std::memcpy(&cvb_stride, rem + cur, 4);
        std::memcpy(&cvb_num,    rem + cur + 4, 4);
        cur += 8;
        if (cvb_num > 0) {
            if (cur + 8 > rem_len) continue;
            int32_t cvb_bsz = 0, cvb_bcnt = 0;
            std::memcpy(&cvb_bsz,  rem + cur, 4);
            std::memcpy(&cvb_bcnt, rem + cur + 4, 4);
            if (cvb_bsz < 0 || cvb_bcnt < 0) continue;
            cur += 8 + static_cast<size_t>(cvb_bsz) * static_cast<size_t>(cvb_bcnt);
        }
        if (cur + 12 > rem_len) continue;

        // IndexBuffer
        int32_t lod_num_verts = 0, ib_sz = 0, ib_cnt = 0;
        std::memcpy(&lod_num_verts, rem + cur, 4);
        std::memcpy(&ib_sz,         rem + cur + 4, 4);
        std::memcpy(&ib_cnt,        rem + cur + 8, 4);
        size_t ib_data_off = cur + 12;
        if ((ib_sz != 2 && ib_sz != 4) || ib_cnt < 3 || ib_data_off + static_cast<size_t>(ib_sz) * static_cast<size_t>(ib_cnt) > rem_len) {
            continue;
        }

        int32_t num_tris = ib_cnt / 3;
        asset.triangles.reserve(static_cast<size_t>(num_tris) * 3);

        // v536 FStaticMeshFullVertex: TangentX(FPackedNormal) TangentZ(FPackedNormal, W = binormal sign)
        // FColor(B,G,R,A; inline until the separate color stream of v615) UV[NumTexCoords]
        // (FVector2DHalf, or FVector2D when bUseFullPrecisionUVs).
        const size_t uv_size = full_prec ? 8 : 4;
        const int64_t uv_off_signed = static_cast<int64_t>(smvb_stride) - static_cast<int64_t>(num_uv) * static_cast<int64_t>(uv_size);
        const bool uv_layout_ok = num_uv >= 1 && num_uv <= 8 && uv_off_signed >= 8;
        const size_t uv_off = uv_layout_ok ? static_cast<size_t>(uv_off_signed) : 0;
        const bool has_color = uv_layout_ok && uv_off >= 12;

        auto read_uv = [&](const uint8_t* sv, int32_t channel, float& u, float& v) {
            u = 0.0f;
            v = 0.0f;
            if (!uv_layout_ok || channel >= num_uv) return;
            const uint8_t* p = sv + uv_off + static_cast<size_t>(channel) * uv_size;
            if (full_prec) {
                std::memcpy(&u, p, 4);
                std::memcpy(&v, p + 4, 4);
            } else {
                uint16_t h[2];
                std::memcpy(h, p, 4);
                u = half_to_float(h[0]);
                v = half_to_float(h[1]);
            }
        };
        auto unpack_normal = [](const uint8_t* b) {
            return Vec3(static_cast<float>(b[0]) / 127.5f - 1.0f,
                        static_cast<float>(b[1]) / 127.5f - 1.0f,
                        static_cast<float>(b[2]) / 127.5f - 1.0f);
        };

        auto emit_triangle = [&](int32_t t) {
            uint32_t idx[3] = {0, 0, 0};
            if (ib_sz == 2) {
                uint16_t s[3];
                std::memcpy(s, rem + ib_data_off + static_cast<size_t>(t) * 6, 6);
                idx[0] = s[0]; idx[1] = s[1]; idx[2] = s[2];
            } else {
                std::memcpy(idx, rem + ib_data_off + static_cast<size_t>(t) * 12, 12);
            }
            if (idx[0] >= static_cast<uint32_t>(pos_num) ||
                idx[1] >= static_cast<uint32_t>(pos_num) ||
                idx[2] >= static_cast<uint32_t>(pos_num)) {
                return;
            }

            Vec3 p[3];
            for (int k = 0; k < 3; ++k) {
                std::memcpy(&p[k].x, rem + pos_data_off + idx[k] * 12 + 0, 4);
                std::memcpy(&p[k].y, rem + pos_data_off + idx[k] * 12 + 4, 4);
                std::memcpy(&p[k].z, rem + pos_data_off + idx[k] * 12 + 8, 4);
            }

            // Geometric face normal in UE3 left-handed coords
            Vec3 e1 = p[1] - p[0];
            Vec3 e2 = p[2] - p[0];
            Vec3 face_n = e2.cross(e1).normalized();

            for (int k = 0; k < 3; ++k) {
                Vertex v{};
                v.position = p[k];
                const uint8_t* sv = rem + smvb_data_off + idx[k] * static_cast<size_t>(smvb_stride);
                Vec3 tz = unpack_normal(sv + 4);
                if (tz.length_sq() > 0.25f) {
                    v.normal = tz.normalized();
                } else {
                    v.normal = (face_n.length_sq() > 0.1f) ? face_n : Vec3(0.0f, 0.0f, 1.0f);
                }
                Vec3 tx = unpack_normal(sv);
                v.tangent = (tx.length_sq() > 0.25f) ? tx.normalized() : Vec3(1.0f, 0.0f, 0.0f);
                v.tangent_sign = (sv[7] >= 128) ? 1.0f : -1.0f;
                read_uv(sv, 0, v.u, v.v);
                read_uv(sv, 1, v.u2, v.v2);
                v.color = has_color ? (static_cast<uint32_t>(sv[10]) | (static_cast<uint32_t>(sv[9]) << 8) |
                                       (static_cast<uint32_t>(sv[8]) << 16) | (static_cast<uint32_t>(sv[11]) << 24))
                                    : 0xFFFFFFFFu;
                asset.triangles.push_back(v);
            }
        };

        // Emit triangles grouped by LOD0 element (one material each). Element indices are kept
        // stable (even when empty) because StaticMeshComponent.Materials[] is indexed by them.
        for (const auto& re : raw_elems) {
            StaticMeshElement el;
            el.material = (re.material_ref != 0) ? object_canonical_path(*this, re.material_ref) : std::string();
            el.first_vertex = static_cast<uint32_t>(asset.triangles.size());
            const int64_t t0 = std::clamp<int64_t>(re.first_index / 3, 0, num_tris);
            const int64_t t1 = std::clamp<int64_t>(t0 + std::max(re.num_triangles, 0), t0, num_tris);
            for (int64_t t = t0; t < t1; ++t) emit_triangle(static_cast<int32_t>(t));
            el.vertex_count = static_cast<uint32_t>(asset.triangles.size()) - el.first_vertex;
            asset.elements.push_back(std::move(el));
        }
        if (asset.triangles.empty()) {
            // No usable element table: draw the whole index buffer with the first element's material.
            StaticMeshElement el;
            if (!raw_elems.empty() && raw_elems.front().material_ref != 0) {
                el.material = object_canonical_path(*this, raw_elems.front().material_ref);
            }
            for (int32_t t = 0; t < num_tris; ++t) emit_triangle(t);
            el.vertex_count = static_cast<uint32_t>(asset.triangles.size());
            asset.elements.clear();
            asset.elements.push_back(std::move(el));
        }

        if (!asset.triangles.empty()) {
            out_meshes[key] = std::move(asset);
        }
    }
}

// -----------------------------------------------------------------------------
// Contiguous Rooftop & Parkour Level Geometry Construction
// -----------------------------------------------------------------------------
void generate_rooftop_level_geometry(std::vector<LevelActor>& actors,
                                     std::vector<MeshBuffer>& out_meshes,
                                     std::vector<AABB>& out_colliders,
                                     const std::unordered_map<std::string, StaticMeshAsset>* mesh_lib,
                                     std::vector<std::string>* out_material_paths) {
    out_meshes.clear();
    out_colliders.clear();

    if (actors.empty()) {
        MeshBuffer floor_mesh;
        floor_mesh.name = "Fallback_Rooftop";
        Vec3 floor_min(-2000.0f, -2000.0f, -50.0f);
        Vec3 floor_max(2000.0f, 2000.0f, 0.0f);
        add_box_mesh(floor_mesh.vertices, floor_mesh.indices, floor_min, floor_max, 0xFFEEEEEE);
        floor_mesh.bounds = AABB(floor_min, floor_max);
        out_meshes.push_back(floor_mesh);
        out_colliders.push_back(floor_mesh.bounds);
        return;
    }

    // Batched world buffers for ultra-fast single-draw-call rendering
    MeshBuffer world_batch;
    world_batch.name = "UE3_Level_World_Geometry";
    world_batch.is_runner_vision = false;

    MeshBuffer rv_batch;
    rv_batch.name = "UE3_Level_RunnerVision_Geometry";
    rv_batch.is_runner_vision = true;

    // Material sections: vertices are binned per scene material (-1 = procedural palette shading)
    // and concatenated at the end so every material is one contiguous draw.
    const bool use_materials = (out_material_paths != nullptr);
    std::unordered_map<std::string, int32_t> material_ids;
    if (use_materials) {
        for (size_t i = 0; i < out_material_paths->size(); ++i) {
            material_ids.emplace(to_lower((*out_material_paths)[i]), static_cast<int32_t>(i));
        }
    }
    auto material_id = [&](const std::string& path) -> int32_t {
        std::string key = to_lower(path);
        auto it = material_ids.find(key);
        if (it != material_ids.end()) return it->second;
        const int32_t id = static_cast<int32_t>(out_material_paths->size());
        out_material_paths->push_back(path);
        material_ids.emplace(std::move(key), id);
        return id;
    };
    std::map<int32_t, std::vector<Vertex>> world_bins;
    std::map<int32_t, std::vector<Vertex>> rv_bins;
    std::vector<uint32_t> scratch_indices;

    Vec3 overall_min(1e9f, 1e9f, 1e9f);
    Vec3 overall_max(-1e9f, -1e9f, -1e9f);
    bool has_real_meshes = (mesh_lib && !mesh_lib->empty());
    size_t placed_meshes = 0;
    size_t missing_meshes = 0;
    size_t fallback_boxes = 0;
    std::map<std::string, int> missing_names;

    for (auto& a : actors) {
        if (std::abs(a.location.x) > 150000.0f || std::abs(a.location.y) > 150000.0f) continue;
        if (a.location.x == 0.0f && a.location.y == 0.0f && a.location.z == 0.0f && a.mesh_name.empty()) continue;

        // Determine architectural material palette color (ABGR packed uint32)
        std::string low_mesh = a.mesh_name;
        std::transform(low_mesh.begin(), low_mesh.end(), low_mesh.begin(), ::tolower);

        uint32_t color = 0xFFF2F0EE; // Clean Mirror's Edge white architectural concrete
        if (a.is_runner_vision) {
            color = 0xFF1414E6; // Runner Vision Red (#E61414)
        } else if (low_mesh.find("glass") != std::string::npos || low_mesh.find("window") != std::string::npos ||
                   low_mesh.find("skylight") != std::string::npos) {
            color = 0xFFE8C890; // Reflective cyan-blue architectural glass
        } else if (low_mesh.find("catwalk") != std::string::npos || low_mesh.find("fence") != std::string::npos ||
                   low_mesh.find("railing") != std::string::npos || low_mesh.find("stair") != std::string::npos ||
                   low_mesh.find("scaffold") != std::string::npos) {
            color = 0xFFA8A098; // Dark steel catwalk/railing
        } else if (low_mesh.find("airduct") != std::string::npos || low_mesh.find("vent") != std::string::npos ||
                   low_mesh.find("ac") != std::string::npos || low_mesh.find("pipe") != std::string::npos) {
            color = 0xFFDCD8D4; // Galvanized metallic silver HVAC/ducting
        } else if (low_mesh.find("crane") != std::string::npos || a.is_checkpoint) {
            color = 0xFF2898F0; // Industrial orange/gold accent
        } else if (low_mesh.find("bd_") != std::string::npos || low_mesh.find("building") != std::string::npos ||
                   low_mesh.find("s_c_") != std::string::npos || low_mesh.find("s_r_") != std::string::npos ||
                   low_mesh.find("sky") != std::string::npos) {
            // Subtle architectural variation across city blocks using hash of location
            uint32_t h_idx = static_cast<uint32_t>(std::abs(int(a.location.x * 0.01f) + int(a.location.y * 0.01f))) % 4;
            static const uint32_t kBuildingPalette[4] = {
                0xFFF6F4F2, // Stark white tower
                0xFFF2ECE4, // Cool Ice-Blue glass/concrete tower
                0xFFEAE6E2, // Light warm limestone/concrete tower
                0xFFE8DED2  // Deep sky-tinted glass facade tower
            };
            color = kBuildingPalette[h_idx];
        }

        // Check if we have the real extracted UStaticMesh geometry
        const StaticMeshAsset* sm = nullptr;
        if (mesh_lib && !low_mesh.empty()) {
            auto it = mesh_lib->find(low_mesh);
            if (it != mesh_lib->end() && !it->second.triangles.empty()) {
                sm = &it->second;
            } else {
                missing_meshes++;
                missing_names[low_mesh]++;
            }
        }

        if (sm) {
            placed_meshes++;
            // UE3 FScaleRotationTranslationMatrix exact transformation:
            // Local Scale -> Rotation(Pitch, Yaw, Roll) -> Translation(Location)
            Vec3 scale(a.draw_scale * a.draw_scale_3d.x,
                       a.draw_scale * a.draw_scale_3d.y,
                       a.draw_scale * a.draw_scale_3d.z);

            Vec3 rad = a.rotation.to_radians();
            float sp = std::sin(rad.x), cp = std::cos(rad.x);
            float sy = std::sin(rad.y), cy = std::cos(rad.y);
            float sr = std::sin(rad.z), cr = std::cos(rad.z);

            // UE3 FRotationMatrix axes
            Vec3 axis_x(cp * cy, cp * sy, sp);
            Vec3 axis_y(sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp);
            Vec3 axis_z(-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp);

            // Normals transform with the inverse transpose (R * S^-1); tangents with R * S.
            // A mirroring scale (negative determinant) flips the binormal handedness.
            auto safe_inv = [](float s) { return (std::abs(s) > 1e-12f) ? 1.0f / s : 0.0f; };
            const Vec3 inv_scale(safe_inv(scale.x), safe_inv(scale.y), safe_inv(scale.z));
            const float det_sign = (scale.x * scale.y * scale.z < 0.0f) ? -1.0f : 1.0f;

            AABB actor_aabb(Vec3(1e9f, 1e9f, 1e9f), Vec3(-1e9f, -1e9f, -1e9f));

            auto emit_vertex = [&](std::vector<Vertex>& dst, const Vertex& lv, bool keep_vertex_color) {
                Vec3 sp_pos(lv.position.x * scale.x, lv.position.y * scale.y, lv.position.z * scale.z);
                Vec3 wp = a.location + axis_x * sp_pos.x + axis_y * sp_pos.y + axis_z * sp_pos.z;

                Vec3 wn = axis_x * (lv.normal.x * inv_scale.x) + axis_y * (lv.normal.y * inv_scale.y) +
                          axis_z * (lv.normal.z * inv_scale.z);
                Vec3 wt = axis_x * (lv.tangent.x * scale.x) + axis_y * (lv.tangent.y * scale.y) +
                          axis_z * (lv.tangent.z * scale.z);

                Vertex wv = lv;
                wv.position = wp;
                wv.normal = (wn.length_sq() > 1e-20f) ? wn.normalized() : Vec3(0.0f, 0.0f, 1.0f);
                wv.tangent = (wt.length_sq() > 1e-20f) ? wt.normalized() : Vec3(1.0f, 0.0f, 0.0f);
                wv.tangent_sign = lv.tangent_sign * det_sign;
                if (!keep_vertex_color) wv.color = color;
                dst.push_back(wv);

                actor_aabb.expand(wp);
            };
            // Emits whole triangles; mirrored instances (negative determinant) reverse the vertex
            // order so world-space winding stays consistent (UE3 flips the cull mode instead).
            auto emit_range = [&](std::vector<Vertex>& dst, uint32_t first, uint32_t count, bool keep_vertex_color) {
                const uint32_t end = std::min<uint32_t>(first + count, static_cast<uint32_t>(sm->triangles.size()));
                for (uint32_t i = first; i + 2 < end; i += 3) {
                    emit_vertex(dst, sm->triangles[i], keep_vertex_color);
                    if (det_sign < 0.0f) {
                        emit_vertex(dst, sm->triangles[i + 2], keep_vertex_color);
                        emit_vertex(dst, sm->triangles[i + 1], keep_vertex_color);
                    } else {
                        emit_vertex(dst, sm->triangles[i + 1], keep_vertex_color);
                        emit_vertex(dst, sm->triangles[i + 2], keep_vertex_color);
                    }
                }
            };

            if (use_materials) {
                // UStaticMeshComponent::GetMaterial(ElementIndex): component override, else the
                // element's material, else the engine default material ("" here).
                auto& bins = a.is_runner_vision ? rv_bins : world_bins;
                for (size_t e = 0; e < sm->elements.size(); ++e) {
                    const auto& el = sm->elements[e];
                    if (el.vertex_count == 0) continue;
                    const bool overridden = e < a.material_overrides.size() && !a.material_overrides[e].empty();
                    const int32_t mat = material_id(overridden ? a.material_overrides[e] : el.material);
                    emit_range(bins[mat], el.first_vertex, el.vertex_count, true);
                }
            } else {
                std::vector<Vertex>& dst_verts = a.is_runner_vision ? rv_batch.vertices : world_batch.vertices;
                emit_range(dst_verts, 0, static_cast<uint32_t>(sm->triangles.size()), false);
            }

            if (actor_aabb.min_pt.x <= actor_aabb.max_pt.x) {
                a.world_bounds = actor_aabb;
                overall_min.x = std::min(overall_min.x, actor_aabb.min_pt.x);
                overall_min.y = std::min(overall_min.y, actor_aabb.min_pt.y);
                overall_min.z = std::min(overall_min.z, actor_aabb.min_pt.z);
                overall_max.x = std::max(overall_max.x, actor_aabb.max_pt.x);
                overall_max.y = std::max(overall_max.y, actor_aabb.max_pt.y);
                overall_max.z = std::max(overall_max.z, actor_aabb.max_pt.z);

                if (a.is_collidable) {
                    out_colliders.push_back(actor_aabb);
                }
            }
        } else if (!has_real_meshes || a.is_springboard || a.is_zipline || a.is_bag) {
            // Fallback box only when no mesh library is provided or for interactive parkour items
            float w = 60.0f * std::abs(a.draw_scale) * std::max(0.2f, std::abs(a.draw_scale_3d.x));
            float d = 60.0f * std::abs(a.draw_scale) * std::max(0.2f, std::abs(a.draw_scale_3d.y));
            float h = 45.0f * std::abs(a.draw_scale) * std::max(0.2f, std::abs(a.draw_scale_3d.z));

            Vec3 b_min = a.location - Vec3(w, d, h);
            Vec3 b_max = a.location + Vec3(w, d, h);
            AABB box_bounds(b_min, b_max);
            a.world_bounds = box_bounds;
            fallback_boxes++;

            if (use_materials) {
                auto& bins = a.is_runner_vision ? rv_bins : world_bins;
                add_box_mesh(bins[-1], scratch_indices, b_min, b_max, color);
            } else {
                std::vector<Vertex>& dst_verts = a.is_runner_vision ? rv_batch.vertices : world_batch.vertices;
                std::vector<uint32_t>& dst_idx = a.is_runner_vision ? rv_batch.indices : world_batch.indices;
                add_box_mesh(dst_verts, dst_idx, b_min, b_max, color);
            }

            if (a.is_collidable) {
                out_colliders.push_back(box_bounds);
            }
        }
    }

    if (use_materials) {
        auto flush_bins = [](MeshBuffer& mb, std::map<int32_t, std::vector<Vertex>>& bins) {
            size_t total = 0;
            for (const auto& [mat, verts] : bins) total += verts.size();
            mb.vertices.reserve(total);
            for (auto& [mat, verts] : bins) {
                if (verts.empty()) continue;
                MeshSection s;
                s.first_vertex = static_cast<uint32_t>(mb.vertices.size());
                s.vertex_count = static_cast<uint32_t>(verts.size());
                s.material = mat;
                mb.vertices.insert(mb.vertices.end(), verts.begin(), verts.end());
                mb.sections.push_back(s);
                std::vector<Vertex>().swap(verts);
            }
        };
        flush_bins(world_batch, world_bins);
        flush_bins(rv_batch, rv_bins);
    }

    if (has_real_meshes) {
        size_t sections = world_batch.sections.size() + rv_batch.sections.size();
        std::cout << "[Level] " << placed_meshes << " static meshes placed, " << missing_meshes
                  << " missing (" << missing_names.size() << " unique), " << fallback_boxes << " fallback boxes, "
                  << sections << " material sections" << std::endl;
        if (std::getenv("ME_MATERIAL_VERBOSE") && !missing_names.empty()) {
            std::vector<std::pair<int, std::string>> top;
            for (const auto& [n, c] : missing_names) top.emplace_back(c, n);
            std::sort(top.rbegin(), top.rend());
            for (size_t i = 0; i < top.size() && i < 12; ++i) {
                std::cout << "[Level]   missing mesh '" << top[i].second << "' x" << top[i].first << std::endl;
            }
        }
    }

    if (!world_batch.vertices.empty()) {
        world_batch.bounds = AABB(overall_min, overall_max);
        out_meshes.push_back(std::move(world_batch));
    }
    if (!rv_batch.vertices.empty()) {
        rv_batch.bounds = AABB(overall_min, overall_max);
        out_meshes.push_back(std::move(rv_batch));
    }
}

// -----------------------------------------------------------------------------
// Level sun: the dominant DirectionalLight of the level
// -----------------------------------------------------------------------------
// Mirror's Edge keeps its sun in the *_Lgts streaming packages (e.g. Tutorial_lgts,
// Edge_Ext_Lgts). The actor stores Rotation (UE units, 65536 = 360 deg) plus optional
// Beast overrides (bUseBakerColorAndBrightness, BakerColor, BakerBrightness); the
// DirectionalLightComponent stores Brightness, LightColor, LightingChannels and
// bHasLightEverBeenBuiltIntoLightMap. Cinematic-only / PhysX-only lights clear the
// Static lighting channel, so the light that was baked into the light-maps wins.
namespace {
struct LevelSun {
    bool found = false;
    float score = -1e30f;
    Vec3 direction{-0.4f, 0.6f, 0.7f};  // world-space direction *towards* the sun
    Vec3 color{2.0f, 1.96f, 1.9f};      // linear RGB * brightness
    std::string source;
};
}  // namespace

static float srgb_byte_to_linear(float c01) {
    // FLinearColor(FColor) uses a pow(x, 2.2) table in UE3.
    return std::pow(std::max(c01, 0.0f), 2.2f);
}

static void scan_level_suns(const UPKPackage& pkg, LevelSun& best) {
    const auto& exports = pkg.get_exports();
    for (size_t i = 0; i < exports.size(); ++i) {
        const int32_t idx = static_cast<int32_t>(i) + 1;
        const std::string cls = object_class_name(pkg, idx);
        if (cls.find("DirectionalLight") == std::string::npos || cls.find("Component") != std::string::npos) continue;

        UPropertyList actor_props;
        parse_export_properties(pkg, idx, actor_props);
        const int32_t comp_idx = prop_object(actor_props, "LightComponent");
        if (comp_idx <= 0) continue;  // components always live in the same package as the actor
        UPropertyList comp_props;
        parse_export_properties(pkg, comp_idx, comp_props);

        if (!prop_bool(comp_props, "bEnabled", true)) continue;
        bool affects_static = true;  // LightComponent default: LightingChannels=(BSP,Static,Dynamic)
        if (const UProperty* lc = find_prop(comp_props, "LightingChannels")) {
            affects_static = prop_bool(lc->fields, "Static", true);
        }
        const bool baked = prop_bool(comp_props, "bHasLightEverBeenBuiltIntoLightMap", false);

        // Colour: Beast overrides when present (they are what the light-maps were baked with).
        float rgb[3] = {1.0f, 1.0f, 1.0f};
        float brightness = prop_float(comp_props, "Brightness", 1.0f);
        if (const UProperty* c = find_prop(comp_props, "LightColor")) {
            for (int k = 0; k < 3; ++k) rgb[k] = c->v[k];
        }
        if (prop_bool(actor_props, "bUseBakerColorAndBrightness", false)) {
            brightness = prop_float(actor_props, "BakerBrightness", brightness);
            if (const UProperty* c = find_prop(actor_props, "BakerColor")) {
                for (int k = 0; k < 3; ++k) rgb[k] = c->v[k];
            }
        }

        const float score = (baked ? 1000.0f : 0.0f) + (affects_static ? 100.0f : 0.0f) + brightness;
        if (best.found && score <= best.score) continue;

        // FRotationMatrix(Rotation).GetAxis(0) is the direction the light travels.
        int32_t pitch = 0, yaw = 0;
        if (const UProperty* r = find_prop(actor_props, "Rotation")) {
            pitch = r->vi[0];
            yaw = r->vi[1];
        }
        const float kUnit = 3.14159265358979f / 32768.0f;
        const float p = static_cast<float>(pitch) * kUnit;
        const float y = static_cast<float>(yaw) * kUnit;
        const Vec3 forward(std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), std::sin(p));
        if (-forward.z < 0.05f) continue;  // a sun below the horizon cannot be the key light

        best.found = true;
        best.score = score;
        best.direction = Vec3(-forward.x, -forward.y, -forward.z);
        best.color = Vec3(srgb_byte_to_linear(rgb[0]) * brightness, srgb_byte_to_linear(rgb[1]) * brightness,
                          srgb_byte_to_linear(rgb[2]) * brightness);
        best.source = package_name_of(pkg) + "." + export_object_name(pkg, idx);
    }
}

// -----------------------------------------------------------------------------
// High-Level Level Loader
// -----------------------------------------------------------------------------
bool load_level_scene(const std::string& game_root, const std::string& map_rel_path, LevelScene& out_scene) {
    namespace fs = std::filesystem;

    fs::path root(game_root);
    fs::path main_path = root / "TdGame" / "CookedPC" / map_rel_path;
    if (!fs::exists(main_path)) {
        main_path = root / "CookedPC" / map_rel_path;
    }
    if (!fs::exists(main_path)) {
        main_path = root / map_rel_path;
    }
    if (!fs::exists(main_path)) {
        return false;
    }

    auto master_pkg = std::make_shared<UPKPackage>(main_path.string());
    if (!master_pkg->is_valid()) {
        return false;
    }

    out_scene.map_name = main_path.stem().string();
    out_scene.actors.clear();
    out_scene.meshes.clear();
    out_scene.colliders.clear();
    out_scene.checkpoints.clear();
    out_scene.sounds.clear();
    out_scene.enemies.clear();
    out_scene.materials.reset();

    // Package manager rooted at CookedPC: keeps the level packages alive and resolves material /
    // texture objects that live in other packages (content packages, engine packages).
    // Set ME_NO_MATERIALS=1 to skip the material system (procedural shading only).
    fs::path cooked_root = root / "TdGame" / "CookedPC";
    for (fs::path p = main_path.parent_path(); !p.empty() && p != p.root_path(); p = p.parent_path()) {
        if (to_lower(p.filename().string()) == "cookedpc") {
            cooked_root = p;
            break;
        }
    }
    std::unique_ptr<PackageManager> pm;
    if (std::getenv("ME_NO_MATERIALS") == nullptr) {
        pm = std::make_unique<PackageManager>(cooked_root.string());
        pm->add_loaded(main_path.stem().string(), master_pkg);
    }

    std::unordered_map<std::string, StaticMeshAsset> mesh_library;

    // Extract from master package
    master_pkg->extract_static_meshes(mesh_library);
    auto master_actors = master_pkg->extract_actors();
    auto master_sounds = master_pkg->extract_audio();
    LevelSun level_sun;
    scan_level_suns(*master_pkg, level_sun);

    out_scene.actors.insert(out_scene.actors.end(), master_actors.begin(), master_actors.end());
    out_scene.sounds.insert(out_scene.sounds.end(), master_sounds.begin(), master_sounds.end());

    // Automatically discover and load adjacent sub-level geometry & art slices (*_Art, *_Bac, *_Slc, *_Pt1, *_Pt2)
    fs::path map_dir = main_path.parent_path();
    std::string stem_prefix = main_path.stem().string();
    size_t underscore_p = stem_prefix.find("_p");
    if (underscore_p == std::string::npos) {
        underscore_p = stem_prefix.find("_P");
    }
    std::string base_prefix = (underscore_p != std::string::npos) ? stem_prefix.substr(0, underscore_p) : stem_prefix;
    std::string low_prefix = base_prefix;
    std::transform(low_prefix.begin(), low_prefix.end(), low_prefix.begin(), ::tolower);

    std::set<std::string> sub_packages;

    auto should_load_subpkg = [&](const std::string& fname) -> bool {
        std::string low = fname;
        std::transform(low.begin(), low.end(), low.begin(), ::tolower);
        if (low.rfind(low_prefix, 0) != 0) {
            return false;
        }
        // Skip localization, music, audio, lightmap-only, script-only, kismet, and time-trial sub-packages
        if (low.find("_loc_") != std::string::npos || low.find("_mus") != std::string::npos ||
            low.find("_aud") != std::string::npos || low.find("_peds") != std::string::npos ||
            low.find("_lookat") != std::string::npos || low.find("_lgts") != std::string::npos ||
            low.find("_spt") != std::string::npos || low.find("_cs") != std::string::npos ||
            low.rfind("tt_", 0) == 0) {
            return false;
        }
        return true;
    };

    // 1. From AdditionalPackagesToCook in master package header
    for (const auto& add_pkg : master_pkg->get_additional_packages()) {
        if (!should_load_subpkg(add_pkg)) continue;
        fs::path p = map_dir / (add_pkg + ".me1");
        if (fs::exists(p)) sub_packages.insert(p.string());
        p = map_dir / (add_pkg + ".upk");
        if (fs::exists(p)) sub_packages.insert(p.string());
    }

    // 2. Directory scan for matching sub-levels
    if (fs::exists(map_dir)) {
        for (const auto& entry : fs::directory_iterator(map_dir)) {
            if (!entry.is_regular_file()) continue;
            std::string ext = entry.path().extension().string();
            if (ext != ".me1" && ext != ".upk") continue;

            std::string fstem = entry.path().stem().string();
            if (!should_load_subpkg(fstem)) continue;
            if (entry.path() != main_path) {
                sub_packages.insert(entry.path().string());
            }
        }
    }

    // Load geometry & art sub-packages
    size_t loaded_sub = 0;
    for (const auto& sub_path : sub_packages) {
        if (loaded_sub++ >= 32) break;
        auto sub_pkg = std::make_shared<UPKPackage>(sub_path);
        if (!sub_pkg->is_valid()) continue;

        sub_pkg->extract_static_meshes(mesh_library);
        auto sub_actors = sub_pkg->extract_actors();
        out_scene.actors.insert(out_scene.actors.end(), sub_actors.begin(), sub_actors.end());
        scan_level_suns(*sub_pkg, level_sun);
        if (pm) pm->add_loaded(fs::path(sub_path).stem().string(), sub_pkg);
    }

    // The sun lives in the *_Lgts lighting packages, which carry no geometry: open them for lights only.
    if (fs::exists(map_dir)) {
        std::vector<fs::path> light_packages;
        for (const auto& entry : fs::directory_iterator(map_dir)) {
            if (!entry.is_regular_file()) continue;
            const std::string ext = entry.path().extension().string();
            if (ext != ".me1" && ext != ".upk") continue;
            const std::string low = to_lower(entry.path().stem().string());
            // Covers *_Lgts, *_lgts, *_LGTs, *_Lgts_Pt1 and the odd *_Lgt (SP07 Boat_Chase_Lgt).
            if (low.rfind(low_prefix, 0) != 0 || low.find("_lgt") == std::string::npos) continue;
            if (low.find("_loc_") != std::string::npos) continue;
            light_packages.push_back(entry.path());
        }
        std::sort(light_packages.begin(), light_packages.end());
        for (const auto& lp : light_packages) {
            UPKPackage light_pkg(lp.string());
            if (light_pkg.is_valid()) scan_level_suns(light_pkg, level_sun);
        }
    }
    if (level_sun.found) {
        out_scene.sun_direction = level_sun.direction.normalized();
        out_scene.sun_color = level_sun.color;
        std::cout << "[Level] Sun from " << level_sun.source << ": direction (" << out_scene.sun_direction.x << ", "
                  << out_scene.sun_direction.y << ", " << out_scene.sun_direction.z << "), linear colour ("
                  << out_scene.sun_color.x << ", " << out_scene.sun_color.y << ", " << out_scene.sun_color.z << ")"
                  << std::endl;
    }

    // Construct real 3D UStaticMesh rooftop geometry and colliders (populating each actor's transformed world_bounds)
    std::vector<std::string> material_paths;
    generate_rooftop_level_geometry(out_scene.actors, out_scene.meshes, out_scene.colliders, &mesh_library,
                                    pm ? &material_paths : nullptr);

    // Resolve, translate and load every material referenced by the level geometry
    // (ME_MATERIAL_VERBOSE=1 prints per-material diagnostics, ME_MAX_TEXTURE_SIZE caps mip size).
    if (pm && !material_paths.empty()) {
        MaterialBuildOptions mopts;
        if (const char* v = std::getenv("ME_MATERIAL_VERBOSE")) mopts.verbose = (v[0] != '\0' && v[0] != '0');
        if (const char* s = std::getenv("ME_MAX_TEXTURE_SIZE")) mopts.max_texture_size = std::max(16, std::atoi(s));
        out_scene.materials = build_scene_materials(*pm, material_paths, mopts);
    }
    pm.reset();

    // Find the best outdoor rooftop PlayerStart / TdTutorialStart / TdCheckpoint surrounded by dense 3D geometry
    bool found_start = false;
    int best_score = -100000;
    for (const auto& a : out_scene.actors) {
        bool is_spawn_candidate = (a.class_name.find("PlayerStart") != std::string::npos ||
                                   a.class_name.find("TutorialStart") != std::string::npos ||
                                   a.class_name.find("Checkpoint") != std::string::npos ||
                                   a.class_name.find("CheckPoint") != std::string::npos);
        if (!is_spawn_candidate) continue;
        if (a.location.x == 0.0f && a.location.y == 0.0f && a.location.z == 0.0f) continue;

        int nearby = 0;
        bool has_floor_below = false;
        for (const auto& other : out_scene.actors) {
            if (other.mesh_name.empty()) continue;
            Vec3 c = other.world_bounds.center();
            float dx = c.x - a.location.x;
            float dy = c.y - a.location.y;
            float dz = std::abs(c.z - a.location.z);
            if ((dx * dx + dy * dy) < (6000.0f * 6000.0f) && dz < 2500.0f) {
                nearby++;
            }
            if (a.location.x >= other.world_bounds.min_pt.x - 150.0f &&
                a.location.x <= other.world_bounds.max_pt.x + 150.0f &&
                a.location.y >= other.world_bounds.min_pt.y - 150.0f &&
                a.location.y <= other.world_bounds.max_pt.y + 150.0f &&
                other.world_bounds.max_pt.z >= a.location.z - 250.0f &&
                other.world_bounds.max_pt.z <= a.location.z + 80.0f) {
                has_floor_below = true;
            }
        }
        int score = nearby + (has_floor_below ? 400 : 0) +
                    ((a.class_name.find("TutorialStart") != std::string::npos && nearby > 150) ? 500 : 0);
        if (score > best_score) {
            best_score = score;
            out_scene.player_spawn_pos = a.location + Vec3(0.0f, 0.0f, 35.0f);
            out_scene.player_spawn_yaw = a.rotation.to_degrees().y;
            found_start = true;
        }
    }
    if (!found_start && !out_scene.actors.empty()) {
        out_scene.player_spawn_pos = out_scene.actors.front().location + Vec3(0, 0, 96.0f);
    }

    // Collect checkpoints and enemies
    for (const auto& a : out_scene.actors) {
        if (a.is_checkpoint) {
            out_scene.checkpoints.push_back(a.location);
        }
        if (a.is_enemy) {
            EnemyBot bot;
            bot.archetype = a.class_name;
            bot.position = a.location;
            bot.yaw_deg = a.rotation.to_degrees().y;
            out_scene.enemies.push_back(bot);
        }
    }

    // Ensure a walkable rooftop collider sits directly beneath player_spawn_pos so the player never falls through uncollided art
    Vec3 sp = out_scene.player_spawn_pos;
    out_scene.colliders.emplace_back(Vec3(sp.x - 600.0f, sp.y - 600.0f, sp.z - 120.0f),
                                     Vec3(sp.x + 600.0f, sp.y + 600.0f, sp.z - 40.0f));

    return true;
}

} // namespace me
