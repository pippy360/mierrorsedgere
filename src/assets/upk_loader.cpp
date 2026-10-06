#include "upk_loader.hpp"
#include <fstream>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <set>
#include <zlib.h>

namespace me {

namespace {

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
// Pure C++20 LZO1X-1 Decompressor
// -----------------------------------------------------------------------------
bool UPKPackage::lzo1x_decompress(const uint8_t* src, size_t src_len, uint8_t* dst, size_t expected_len) {
    if (!src || src_len == 0 || !dst || expected_len == 0) {
        return false;
    }

    size_t ip = 0;
    size_t op = 0;
    size_t state = 0;

    uint8_t b = src[ip++];

    if (b > 17) {
        size_t t = b - 17;
        if (ip + t > src_len || op + t > expected_len) return false;
        std::memcpy(dst + op, src + ip, t);
        op += t;
        ip += t;
        if (ip >= src_len) return (op <= expected_len);
        b = src[ip++];
        state = 0;
    }

    while (ip <= src_len) {
        if (b < 16) {
            if (state == 0) {
                size_t t = b;
                if (t == 0) {
                    while (ip < src_len && src[ip] == 0) {
                        t += 255;
                        ip++;
                    }
                    if (ip >= src_len) return false;
                    t += 15 + src[ip++];
                }
                t += 3;
                if (ip + t > src_len || op + t > expected_len) return false;
                std::memcpy(dst + op, src + ip, t);
                op += t;
                ip += t;
                if (ip >= src_len) break;
                b = src[ip++];
                if (b < 16) {
                    if (ip >= src_len) return false;
                    size_t m_dist = 1 + (b >> 2) + (static_cast<size_t>(src[ip++]) << 2);
                    if (m_dist > op || op + 2 > expected_len) return false;
                    dst[op] = dst[op - m_dist];
                    dst[op + 1] = dst[op - m_dist + 1];
                    op += 2;
                    size_t trailing = b & 3;
                    if (trailing > 0) {
                        if (ip + trailing > src_len || op + trailing > expected_len) return false;
                        std::memcpy(dst + op, src + ip, trailing);
                        op += trailing;
                        ip += trailing;
                    }
                    state = trailing;
                    if (ip >= src_len) break;
                    b = src[ip++];
                    continue;
                } else {
                    state = 0;
                }
            } else {
                if (ip >= src_len) return false;
                size_t m_dist = 1 + (b >> 2) + (static_cast<size_t>(src[ip++]) << 2);
                if (m_dist > op || op + 2 > expected_len) return false;
                dst[op] = dst[op - m_dist];
                dst[op + 1] = dst[op - m_dist + 1];
                op += 2;
                size_t trailing = b & 3;
                if (trailing > 0) {
                    if (ip + trailing > src_len || op + trailing > expected_len) return false;
                    std::memcpy(dst + op, src + ip, trailing);
                    op += trailing;
                    ip += trailing;
                }
                state = trailing;
                if (ip >= src_len) break;
                b = src[ip++];
                continue;
            }
        }

        size_t m_len = 0;
        size_t m_dist = 0;
        size_t trailing = 0;

        if (b >= 64) {
            // M2
            m_len = ((b >> 5) - 1) + 2;
            if (ip >= src_len) return false;
            m_dist = 1 + ((b >> 2) & 7) + (static_cast<size_t>(src[ip++]) << 3);
            trailing = b & 3;
        } else if (b >= 32) {
            // M3
            m_len = b & 31;
            if (m_len == 0) {
                while (ip < src_len && src[ip] == 0) {
                    m_len += 255;
                    ip++;
                }
                if (ip >= src_len) return false;
                m_len += 31 + src[ip++];
            }
            m_len += 2;
            if (ip + 2 > src_len) return false;
            uint8_t lo = src[ip++];
            uint8_t hi = src[ip++];
            m_dist = 1 + (lo >> 2) + (static_cast<size_t>(hi) << 6);
            trailing = lo & 3;
        } else if (b >= 16) {
            // M4
            m_len = b & 7;
            if (m_len == 0) {
                while (ip < src_len && src[ip] == 0) {
                    m_len += 255;
                    ip++;
                }
                if (ip >= src_len) return false;
                m_len += 7 + src[ip++];
            }
            m_len += 2;
            size_t dist_high = static_cast<size_t>(b & 8) << 11;
            if (ip + 2 > src_len) return false;
            uint8_t lo = src[ip++];
            uint8_t hi = src[ip++];
            size_t m_off = (lo >> 2) | (static_cast<size_t>(hi) << 6);
            if (m_off == 0) {
                // EOF marker
                break;
            }
            m_dist = 0x4000 + dist_high + m_off;
            trailing = lo & 3;
        } else {
            return false;
        }

        if (m_dist > op || op + m_len > expected_len) return false;
        for (size_t i = 0; i < m_len; ++i) {
            dst[op + i] = dst[op - m_dist + i];
        }
        op += m_len;

        if (trailing > 0) {
            if (ip + trailing > src_len || op + trailing > expected_len) return false;
            std::memcpy(dst + op, src + ip, trailing);
            op += trailing;
            ip += trailing;
        }

        state = trailing;
        if (ip >= src_len) break;
        b = src[ip++];
    }

    return true;
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

                if (cur_u_off + s_uncomp <= data_.size()) {
                    if (compression_flags_ & 0x02) {
                        // LZO
                        lzo1x_decompress(comp_buf.data(), s_comp, data_.data() + cur_u_off, s_uncomp);
                    } else if (compression_flags_ & 0x01) {
                        // ZLIB
                        uLongf dest_len = s_uncomp;
                        uncompress(data_.data() + cur_u_off, &dest_len, comp_buf.data(), s_comp);
                    } else {
                        std::memcpy(data_.data() + cur_u_off, comp_buf.data(), std::min<size_t>(s_comp, s_uncomp));
                    }
                }
                cur_u_off += s_uncomp;
            }
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
            read_val<int32_t>(imp_ptr, end);

            FObjectImport imp;
            imp.index = i;
            imp.class_package = (pkg_idx >= 0 && pkg_idx < names_.size()) ? names_[pkg_idx] : std::to_string(pkg_idx);
            imp.class_name = (cls_idx >= 0 && cls_idx < names_.size()) ? names_[cls_idx] : std::to_string(cls_idx);
            imp.outer_index = outer_idx;
            imp.object_name = (obj_idx >= 0 && obj_idx < names_.size()) ? names_[obj_idx] : std::to_string(obj_idx);
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
        bool elem_ok = true;
        for (int32_t e = 0; e < elem_cnt; ++e) {
            if (cur + 40 > rem_len) { elem_ok = false; break; }
            int32_t frag_cnt = 0;
            std::memcpy(&frag_cnt, rem + cur + 36, 4);
            if (frag_cnt < 0 || frag_cnt > 10000) { elem_ok = false; break; }
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

        for (int32_t t = 0; t < num_tris; ++t) {
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
                continue;
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
                // Unpack TangentZ at +4
                Vec3 tz(
                    (static_cast<float>(sv[4]) - 127.5f) / 127.5f,
                    (static_cast<float>(sv[5]) - 127.5f) / 127.5f,
                    (static_cast<float>(sv[6]) - 127.5f) / 127.5f
                );
                if (tz.length_sq() > 0.25f) {
                    v.normal = tz.normalized();
                } else {
                    v.normal = (face_n.length_sq() > 0.1f) ? face_n : Vec3(0.0f, 0.0f, 1.0f);
                }
                v.tangent = Vec3(1.0f, 0.0f, 0.0f);
                if (full_prec != 0 && smvb_stride >= 20) {
                    std::memcpy(&v.u, sv + 12, 4);
                    std::memcpy(&v.v, sv + 16, 4);
                }
                v.color = 0xFFFFFFFF;
                asset.triangles.push_back(v);
            }
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
                                     const std::unordered_map<std::string, StaticMeshAsset>* mesh_lib) {
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

    Vec3 overall_min(1e9f, 1e9f, 1e9f);
    Vec3 overall_max(-1e9f, -1e9f, -1e9f);
    bool has_real_meshes = (mesh_lib && !mesh_lib->empty());

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
            }
        }

        if (sm) {
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

            Vec3 norm_sign(scale.x < 0.0f ? -1.0f : 1.0f,
                           scale.y < 0.0f ? -1.0f : 1.0f,
                           scale.z < 0.0f ? -1.0f : 1.0f);

            std::vector<Vertex>& dst_verts = a.is_runner_vision ? rv_batch.vertices : world_batch.vertices;
            AABB actor_aabb(Vec3(1e9f, 1e9f, 1e9f), Vec3(-1e9f, -1e9f, -1e9f));

            for (const auto& lv : sm->triangles) {
                Vec3 sp_pos(lv.position.x * scale.x, lv.position.y * scale.y, lv.position.z * scale.z);
                Vec3 wp = a.location + axis_x * sp_pos.x + axis_y * sp_pos.y + axis_z * sp_pos.z;

                Vec3 sn(lv.normal.x * norm_sign.x, lv.normal.y * norm_sign.y, lv.normal.z * norm_sign.z);
                Vec3 wn = (axis_x * sn.x + axis_y * sn.y + axis_z * sn.z).normalized();

                Vertex wv = lv;
                wv.position = wp;
                wv.normal = wn;
                wv.color = color;
                dst_verts.push_back(wv);

                actor_aabb.expand(wp);
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

            std::vector<Vertex>& dst_verts = a.is_runner_vision ? rv_batch.vertices : world_batch.vertices;
            std::vector<uint32_t>& dst_idx = a.is_runner_vision ? rv_batch.indices : world_batch.indices;
            add_box_mesh(dst_verts, dst_idx, b_min, b_max, color);

            if (a.is_collidable) {
                out_colliders.push_back(box_bounds);
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

    UPKPackage master_pkg(main_path.string());
    if (!master_pkg.is_valid()) {
        return false;
    }

    out_scene.map_name = main_path.stem().string();
    out_scene.actors.clear();
    out_scene.meshes.clear();
    out_scene.colliders.clear();
    out_scene.checkpoints.clear();
    out_scene.sounds.clear();
    out_scene.enemies.clear();

    std::unordered_map<std::string, StaticMeshAsset> mesh_library;

    // Extract from master package
    master_pkg.extract_static_meshes(mesh_library);
    auto master_actors = master_pkg.extract_actors();
    auto master_sounds = master_pkg.extract_audio();

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
    for (const auto& add_pkg : master_pkg.get_additional_packages()) {
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
        UPKPackage sub_pkg(sub_path);
        if (!sub_pkg.is_valid()) continue;

        sub_pkg.extract_static_meshes(mesh_library);
        auto sub_actors = sub_pkg.extract_actors();
        out_scene.actors.insert(out_scene.actors.end(), sub_actors.begin(), sub_actors.end());
    }

    // Construct real 3D UStaticMesh rooftop geometry and colliders (populating each actor's transformed world_bounds)
    generate_rooftop_level_geometry(out_scene.actors, out_scene.meshes, out_scene.colliders, &mesh_library);

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
