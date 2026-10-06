#include "texture_loader.hpp"
#include "package_manager.hpp"
#include "ue3_props.hpp"
#include "upk_loader.hpp"

#include <algorithm>
#include <cstring>
#include <zlib.h>

namespace me {

namespace {

constexpr uint32_t BULK_SEPARATE_FILE = 0x01;
constexpr uint32_t BULK_ZLIB = 0x02;
constexpr uint32_t BULK_LZO = 0x10;
constexpr uint32_t BULK_UNUSED = 0x20;
constexpr uint32_t BULK_LZX = 0x80;

inline int32_t rd_i32(const std::vector<uint8_t>& d, size_t off) {
    int32_t v = 0;
    if (off + 4 <= d.size()) std::memcpy(&v, d.data() + off, 4);
    return v;
}

TexFormat parse_format(const std::string& s) {
    if (s == "PF_DXT1") return TexFormat::DXT1;
    if (s == "PF_DXT3") return TexFormat::DXT3;
    if (s == "PF_DXT5") return TexFormat::DXT5;
    if (s == "PF_A8R8G8B8") return TexFormat::BGRA8;
    if (s == "PF_G8") return TexFormat::G8;
    if (s == "PF_V8U8") return TexFormat::V8U8;
    return TexFormat::Unknown;
}

TexAddress parse_address(const std::string& s) {
    if (s == "TA_Clamp") return TexAddress::Clamp;
    if (s == "TA_Mirror") return TexAddress::Mirror;
    return TexAddress::Wrap;
}

struct BulkHeader {
    uint32_t flags = 0;
    int32_t element_count = 0;
    int32_t size_on_disk = 0;
    int32_t offset_in_file = 0;
    size_t payload_offset = 0;  // inline payload position (absolute in package data)
    size_t next = 0;            // position after header (+ inline payload)
};

bool read_bulk_header(const std::vector<uint8_t>& d, size_t off, size_t end, BulkHeader& h) {
    if (off + 16 > end) return false;
    h.flags = static_cast<uint32_t>(rd_i32(d, off));
    h.element_count = rd_i32(d, off + 4);
    h.size_on_disk = rd_i32(d, off + 8);
    h.offset_in_file = rd_i32(d, off + 12);
    h.payload_offset = off + 16;
    h.next = off + 16;
    if (h.element_count < 0 || h.size_on_disk < 0) return false;
    if (!(h.flags & BULK_SEPARATE_FILE) && !(h.flags & BULK_UNUSED)) {
        h.next += static_cast<size_t>(h.size_on_disk);
        if (h.next > end) return false;
    }
    return true;
}

// Fetches and decompresses the payload described by a bulk header.
bool fetch_bulk(PackageManager& pm, const UPKPackage& pkg, const std::string& separate_pkg, const BulkHeader& h,
                std::vector<uint8_t>& out, std::string* err) {
    if (h.flags & BULK_UNUSED) return false;
    if (h.flags & BULK_LZX) {
        if (err) *err = "LZX bulk data unsupported";
        return false;
    }
    std::vector<uint8_t> raw;
    const uint8_t* src = nullptr;
    size_t src_len = static_cast<size_t>(h.size_on_disk);
    const auto& d = pkg.get_data();
    if (h.flags & BULK_SEPARATE_FILE) {
        if (!pm.read_raw(separate_pkg, static_cast<uint64_t>(h.offset_in_file), src_len, raw)) {
            if (err) *err = "missing separate bulk data in package '" + separate_pkg + "'";
            return false;
        }
        src = raw.data();
    } else {
        if (h.payload_offset + src_len > d.size()) return false;
        src = d.data() + h.payload_offset;
    }
    const size_t expected = static_cast<size_t>(h.element_count);
    if (h.flags & (BULK_LZO | BULK_ZLIB)) {
        return decompress_ue3_chunks(src, src_len, (h.flags & BULK_LZO) != 0, expected, out);
    }
    out.assign(src, src + std::min(src_len, expected ? expected : src_len));
    return true;
}

}  // namespace

size_t texture_mip_bytes(TexFormat fmt, int w, int h) {
    const size_t bx = static_cast<size_t>(std::max(1, (w + 3) / 4));
    const size_t by = static_cast<size_t>(std::max(1, (h + 3) / 4));
    switch (fmt) {
        case TexFormat::DXT1: return bx * by * 8;
        case TexFormat::DXT3:
        case TexFormat::DXT5: return bx * by * 16;
        case TexFormat::BGRA8: return static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
        case TexFormat::G8: return static_cast<size_t>(w) * static_cast<size_t>(h);
        case TexFormat::V8U8: return static_cast<size_t>(w) * static_cast<size_t>(h) * 2;
        default: return 0;
    }
}

bool decompress_ue3_chunks(const uint8_t* src, size_t src_len, bool lzo, size_t expected_size, std::vector<uint8_t>& out) {
    if (src_len < 16) return false;
    uint32_t tag = 0, block_size = 0, comp_total = 0, uncomp_total = 0;
    std::memcpy(&tag, src, 4);
    std::memcpy(&block_size, src + 4, 4);
    std::memcpy(&comp_total, src + 8, 4);
    std::memcpy(&uncomp_total, src + 12, 4);
    if (tag != 0x9E2A83C1u || block_size == 0) return false;
    if (expected_size && uncomp_total != expected_size) {
        // Trust the chunk header but keep the caller's expectation as an upper bound.
        if (uncomp_total > expected_size * 4 + 65536) return false;
    }
    const size_t num_blocks = (uncomp_total + block_size - 1) / block_size;
    size_t hdr = 16 + num_blocks * 8;
    if (hdr > src_len) return false;
    out.assign(uncomp_total, 0);
    size_t in_pos = hdr;
    size_t out_pos = 0;
    for (size_t b = 0; b < num_blocks; ++b) {
        uint32_t csz = 0, usz = 0;
        std::memcpy(&csz, src + 16 + b * 8, 4);
        std::memcpy(&usz, src + 16 + b * 8 + 4, 4);
        if (in_pos + csz > src_len || out_pos + usz > out.size()) return false;
        bool ok;
        if (lzo) {
            ok = UPKPackage::lzo1x_decompress(src + in_pos, csz, out.data() + out_pos, usz);
        } else {
            uLongf dl = usz;
            ok = uncompress(out.data() + out_pos, &dl, src + in_pos, csz) == Z_OK && dl == usz;
        }
        if (!ok) return false;
        in_pos += csz;
        out_pos += usz;
    }
    return out_pos == uncomp_total;
}

bool read_texture_info(const UPKPackage& pkg, int32_t export_index_1based, TextureInfo& out) {
    UPropertyList props;
    if (parse_export_properties(pkg, export_index_1based, props) == 0) return false;
    out.size_x = prop_int(props, "SizeX", 0);
    out.size_y = prop_int(props, "SizeY", out.size_x);
    out.format_name = prop_name(props, "Format", "PF_A8R8G8B8");
    out.format = parse_format(out.format_name);
    out.srgb = prop_bool(props, "SRGB", true);
    out.address_x = parse_address(prop_name(props, "AddressX", "TA_Wrap"));
    out.address_y = parse_address(prop_name(props, "AddressY", "TA_Wrap"));
    for (int k = 0; k < 4; ++k) {
        if (const UProperty* p = find_prop(props, "UnpackMin", k)) out.unpack_min[k] = p->f;
        if (const UProperty* p = find_prop(props, "UnpackMax", k)) out.unpack_max[k] = p->f;
    }
    out.compression_settings = prop_name(props, "CompressionSettings", "TC_Default");
    out.lod_group = prop_name(props, "LODGroup", "");
    out.is_normal_map = out.compression_settings == "TC_Normalmap" || out.compression_settings == "TC_NormalmapAlpha" ||
                        out.lod_group.find("NormalMap") != std::string::npos;
    return true;
}

bool load_texture2d(PackageManager& pm, const UPKPackage& pkg, int32_t export_index_1based, int max_size,
                    SceneTexture& out, std::string* error) {
    const auto& exports = pkg.get_exports();
    if (export_index_1based <= 0 || static_cast<size_t>(export_index_1based) > exports.size()) return false;
    const auto& exp = exports[export_index_1based - 1];
    const auto& d = pkg.get_data();

    UPropertyList props;
    size_t pend = parse_export_properties(pkg, export_index_1based, props);
    if (pend == 0) return false;
    TextureInfo info;
    read_texture_info(pkg, export_index_1based, info);

    out.name = object_canonical_path(pkg, export_index_1based);
    out.format = info.format;
    out.srgb = info.srgb;
    out.address_x = info.address_x;
    out.address_y = info.address_y;
    out.is_cube = false;
    out.mips.clear();
    if (out.format == TexFormat::Unknown) {
        if (error) *error = "unsupported pixel format " + info.format_name;
        return false;
    }

    const size_t end = static_cast<size_t>(exp.serial_offset) + static_cast<size_t>(exp.serial_size);
    BulkHeader source_art;
    if (!read_bulk_header(d, pend, end, source_art)) {
        if (error) *error = "bad SourceArt bulk header";
        return false;
    }
    size_t cur = source_art.next;
    int32_t num_mips = rd_i32(d, cur);
    cur += 4;
    if (num_mips <= 0 || num_mips > 16) {
        if (error) *error = "bad mip count";
        return false;
    }

    const std::string separate_pkg = object_outermost_name(pkg, export_index_1based);
    struct MipRef { BulkHeader h; int w; int h2; };
    std::vector<MipRef> refs;
    for (int32_t m = 0; m < num_mips; ++m) {
        MipRef r;
        if (!read_bulk_header(d, cur, end, r.h)) break;
        cur = r.h.next;
        if (cur + 8 > end) break;
        r.w = rd_i32(d, cur);
        r.h2 = rd_i32(d, cur + 4);
        cur += 8;
        refs.push_back(r);
    }

    std::string last_err;
    for (const auto& r : refs) {
        if (r.w <= 0 || r.h2 <= 0) continue;
        if (!out.mips.empty()) {
            // Keep a contiguous chain: each subsequent mip must be the next smaller level.
            const auto& prev = out.mips.back();
            if (r.w != std::max(1, prev.width / 2) || r.h2 != std::max(1, prev.height / 2)) break;
        } else if (std::max(r.w, r.h2) > max_size) {
            continue;
        }
        // Block-compressed formats: stop below one 4x4 block.
        if ((out.format == TexFormat::DXT1 || out.format == TexFormat::DXT3 || out.format == TexFormat::DXT5) &&
            (r.w < 4 || r.h2 < 4)) {
            break;
        }
        if (r.h.flags & BULK_UNUSED) {
            if (!out.mips.empty()) break;
            continue;
        }
        TextureMip mip;
        mip.width = r.w;
        mip.height = r.h2;
        std::string err;
        if (!fetch_bulk(pm, pkg, separate_pkg, r.h, mip.data, &err)) {
            last_err = err;
            if (!out.mips.empty()) break;  // chain broken: keep what we have
            continue;                      // try next (smaller, maybe inline) mip
        }
        const size_t need = texture_mip_bytes(out.format, r.w, r.h2);
        if (mip.data.size() < need) {
            last_err = "short mip data";
            if (!out.mips.empty()) break;
            continue;
        }
        mip.data.resize(need);
        out.mips.push_back(std::move(mip));
    }
    if (out.mips.empty()) {
        if (error) *error = last_err.empty() ? "no loadable mips" : last_err;
        return false;
    }
    return true;
}

bool load_texture_cube(PackageManager& pm, const UPKPackage& pkg, int32_t export_index_1based, int max_size,
                       SceneTexture& out, std::string* error) {
    UPropertyList props;
    if (parse_export_properties(pkg, export_index_1based, props) == 0) return false;
    static const char* kFaces[6] = {"FacePosX", "FaceNegX", "FacePosY", "FaceNegY", "FacePosZ", "FaceNegZ"};
    out.name = object_canonical_path(pkg, export_index_1based);
    out.is_cube = true;
    out.address_x = TexAddress::Clamp;
    out.address_y = TexAddress::Clamp;
    for (int f = 0; f < 6; ++f) {
        int32_t face_idx = prop_object(props, kFaces[f]);
        if (face_idx <= 0) {
            if (error) *error = std::string("cube face missing/imported: ") + kFaces[f];
            return false;
        }
        SceneTexture face;
        if (!load_texture2d(pm, pkg, face_idx, max_size, face, error)) return false;
        if (f == 0) {
            out.format = face.format;
            out.srgb = face.srgb;
        } else if (face.format != out.format || face.mips.size() != out.faces[0].size() ||
                   face.mips[0].width != out.faces[0][0].width) {
            // Normalise mip counts across faces (use the minimum common chain).
            size_t n = std::min(face.mips.size(), out.faces[0].size());
            if (face.format != out.format || n == 0 || face.mips[0].width != out.faces[0][0].width) {
                if (error) *error = "inconsistent cube faces";
                return false;
            }
            face.mips.resize(n);
            for (int k = 0; k < f; ++k) out.faces[k].resize(n);
        }
        out.faces[f] = std::move(face.mips);
    }
    return true;
}

namespace {

int floor_pow2(int v) {
    int p = 1;
    while (p * 2 <= v) p *= 2;
    return p;
}

// Evaluates a procedural City of Glass environment color (BGRA8 sRGB) for planar screen-space
// reflection targets (T_EdgeReflection_01_R, T_CityReflection_01_R, T_ElevatorReflection_01_R).
void eval_planar_reflection_bgra(float u, float v, uint8_t bgra[4]) {
    const float uf = u - std::floor(u);
    const float vf = std::clamp(v, 0.0f, 1.0f);

    // Base sky-to-horizon-to-water reflection gradient
    float r = 0.0f, g = 0.0f, b = 0.0f;
    if (vf < 0.52f) {
        const float t = vf / 0.52f;
        const float ts = t * t * (3.0f - 2.0f * t);
        r = (52.0f * (1.0f - ts) + 216.0f * ts);
        g = (124.0f * (1.0f - ts) + 234.0f * ts);
        b = (224.0f * (1.0f - ts) + 252.0f * ts);
    } else {
        const float t = (vf - 0.52f) / 0.48f;
        const float ts = t * t * (3.0f - 2.0f * t);
        r = (216.0f * (1.0f - ts) + 128.0f * ts);
        g = (234.0f * (1.0f - ts) + 166.0f * ts);
        b = (252.0f * (1.0f - ts) + 212.0f * ts);
    }

    // Reflected City of Glass skyscraper silhouettes across the horizon band
    const int col = static_cast<int>(uf * 24.0f);
    const float col_frac = uf * 24.0f - static_cast<float>(col);
    const uint32_t h = static_cast<uint32_t>(col * 2654435761u);
    const float tower_top = 0.18f + static_cast<float>(h & 0xFFu) / 255.0f * 0.28f;
    const float tower_bot = 0.78f;
    const bool in_gap = (col_frac < 0.08f || col_frac > 0.92f);
    if (!in_gap && vf >= tower_top && vf <= tower_bot) {
        const bool sunlit = (col_frac < 0.58f);
        const float win_x = std::sin(uf * 24.0f * 6.0f * 3.14159265f);
        const float win_y = std::sin(vf * 48.0f * 3.14159265f);
        const float mullion = (win_x > 0.75f || win_y > 0.78f) ? 1.0f : 0.0f;
        float tr = sunlit ? 236.0f : 152.0f;
        float tg = sunlit ? 240.0f : 182.0f;
        float tb = sunlit ? 246.0f : 220.0f;
        tr = tr * (0.90f + 0.10f * mullion);
        tg = tg * (0.91f + 0.09f * mullion);
        tb = tb * (0.93f + 0.07f * mullion);
        const float fade = std::clamp(1.0f - std::abs(vf - 0.48f) * 1.6f, 0.25f, 0.88f);
        r = r * (1.0f - fade) + tr * fade;
        g = g * (1.0f - fade) + tg * fade;
        b = b * (1.0f - fade) + tb * fade;
    }

    bgra[0] = static_cast<uint8_t>(std::clamp(b, 0.0f, 255.0f));
    bgra[1] = static_cast<uint8_t>(std::clamp(g, 0.0f, 255.0f));
    bgra[2] = static_cast<uint8_t>(std::clamp(r, 0.0f, 255.0f));
    bgra[3] = 255;
}

// Evaluates a 3D direction (dx, dy, dz in UE3 Z-up world space) into a City of Glass cubemap texel.
void eval_cube_reflection_bgra(float dx, float dy, float dz, uint8_t bgra[4]) {
    const float len = std::sqrt(std::max(dx * dx + dy * dy + dz * dz, 1e-12f));
    dx /= len;
    dy /= len;
    dz /= len;

    float r = 0.0f, g = 0.0f, b = 0.0f;
    if (dz >= 0.0f) {
        // Upper hemisphere: zenith cobalt blue to bright horizon cyan-white
        const float t = std::pow(1.0f - dz, 2.2f);
        r = 54.0f * (1.0f - t) + 224.0f * t;
        g = 126.0f * (1.0f - t) + 238.0f * t;
        b = 228.0f * (1.0f - t) + 254.0f * t;
    } else {
        // Lower hemisphere: horizon haze into cool white/slate rooftop bounce
        const float t = std::min(1.0f, -dz * 2.5f);
        r = 224.0f * (1.0f - t) + 148.0f * t;
        g = 238.0f * (1.0f - t) + 168.0f * t;
        b = 254.0f * (1.0f - t) + 196.0f * t;
    }

    // Horizon skyline band (-0.18 <= dz <= +0.35)
    if (dz >= -0.18f && dz <= 0.35f) {
        const float az = (std::atan2(dy, dx) + 3.14159265f) / (2.0f * 3.14159265f);
        const int sector = static_cast<int>(az * 32.0f);
        const float sfrac = az * 32.0f - static_cast<float>(sector);
        const uint32_t h = static_cast<uint32_t>(sector * 2654435761u);
        const float roof_z = 0.05f + static_cast<float>(h & 0xFFu) / 255.0f * 0.26f;
        if (sfrac > 0.08f && sfrac < 0.92f && dz <= roof_z) {
            const float sun_dot = std::max(0.0f, -0.4f * dx + 0.6f * dy + 0.7f * dz);
            const float tr = 175.0f + 70.0f * sun_dot;
            const float tg = 196.0f + 52.0f * sun_dot;
            const float tb = 224.0f + 28.0f * sun_dot;
            const float blend = std::clamp(1.0f - std::abs(dz) * 2.2f, 0.3f, 0.85f);
            r = r * (1.0f - blend) + tr * blend;
            g = g * (1.0f - blend) + tg * blend;
            b = b * (1.0f - blend) + tb * blend;
        }
    }

    bgra[0] = static_cast<uint8_t>(std::clamp(b, 0.0f, 255.0f));
    bgra[1] = static_cast<uint8_t>(std::clamp(g, 0.0f, 255.0f));
    bgra[2] = static_cast<uint8_t>(std::clamp(r, 0.0f, 255.0f));
    bgra[3] = 255;
}

void cube_face_direction(int face, float u, float v, float& dx, float& dy, float& dz) {
    // Standard cube face mapping in [0,1]^2 -> [-1,+1]^2
    const float sc = 2.0f * u - 1.0f;
    const float tc = 2.0f * v - 1.0f;
    switch (face) {
        case 0: dx =  1.0f; dy = -tc;   dz = -sc;   break; // +X
        case 1: dx = -1.0f; dy = -tc;   dz =  sc;   break; // -X
        case 2: dx =  sc;   dy =  1.0f; dz =  tc;   break; // +Y
        case 3: dx =  sc;   dy = -1.0f; dz = -tc;   break; // -Y
        case 4: dx =  sc;   dy = -tc;   dz =  1.0f; break; // +Z
        default:dx = -sc;   dy = -tc;   dz = -1.0f; break; // -Z
    }
}

}  // namespace

bool load_texture_render_target2d(const UPKPackage& pkg, int32_t export_index_1based, int max_size,
                                  SceneTexture& out, std::string* /*error*/) {
    TextureInfo info;
    read_texture_info(pkg, export_index_1based, info);

    out.name = object_canonical_path(pkg, export_index_1based);
    out.format = TexFormat::BGRA8;
    out.srgb = info.srgb;
    out.address_x = info.address_x;
    out.address_y = info.address_y;
    out.is_cube = false;
    out.mips.clear();

    const int cap = std::clamp(max_size, 16, 256);
    int w = floor_pow2(std::clamp(info.size_x > 0 ? info.size_x : 256, 16, cap));
    int h = floor_pow2(std::clamp(info.size_y > 0 ? info.size_y : 256, 16, cap));

    for (;;) {
        TextureMip mip;
        mip.width = w;
        mip.height = h;
        mip.data.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
        for (int y = 0; y < h; ++y) {
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(h);
            for (int x = 0; x < w; ++x) {
                const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(w);
                eval_planar_reflection_bgra(u, v, &mip.data[(static_cast<size_t>(y) * w + x) * 4]);
            }
        }
        out.mips.push_back(std::move(mip));
        if (w == 1 && h == 1) break;
        w = std::max(1, w / 2);
        h = std::max(1, h / 2);
    }
    return true;
}

bool load_texture_render_target_cube(const UPKPackage& pkg, int32_t export_index_1based, int max_size,
                                     SceneTexture& out, std::string* /*error*/) {
    TextureInfo info;
    read_texture_info(pkg, export_index_1based, info);

    out.name = object_canonical_path(pkg, export_index_1based);
    out.format = TexFormat::BGRA8;
    out.srgb = info.srgb;
    out.address_x = TexAddress::Clamp;
    out.address_y = TexAddress::Clamp;
    out.is_cube = true;
    out.mips.clear();

    const int cap = std::clamp(max_size, 16, 128);
    const int base_dim = floor_pow2(std::clamp(info.size_x > 0 ? info.size_x : 128, 16, cap));

    for (int f = 0; f < 6; ++f) {
        out.faces[f].clear();
        int dim = base_dim;
        for (;;) {
            TextureMip mip;
            mip.width = dim;
            mip.height = dim;
            mip.data.resize(static_cast<size_t>(dim) * static_cast<size_t>(dim) * 4);
            for (int y = 0; y < dim; ++y) {
                const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(dim);
                for (int x = 0; x < dim; ++x) {
                    const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(dim);
                    float dx = 0.0f, dy = 0.0f, dz = 1.0f;
                    cube_face_direction(f, u, v, dx, dy, dz);
                    eval_cube_reflection_bgra(dx, dy, dz, &mip.data[(static_cast<size_t>(y) * dim + x) * 4]);
                }
            }
            out.faces[f].push_back(std::move(mip));
            if (dim == 1) break;
            dim = std::max(1, dim / 2);
        }
    }
    return true;
}

}  // namespace me
