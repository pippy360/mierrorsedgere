#include "upk_loader.hpp"
#include "level_intro.hpp"
#include "level_lightmaps.hpp"
#include "level_postprocess.hpp"
#include "material_system.hpp"
#include "package_manager.hpp"
#include "ue3_props.hpp"
#include "../anim/anim_system.hpp"
#include "../game/level_script.hpp"
#include "../physics/collision_world.hpp"
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_map>
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

// UE3 AActor::LocalToWorld(): Translation(-PrePivot) * Scale(DrawScale3D * DrawScale) *
// FRotationMatrix(Rotation) * Translation(Location).
struct ActorTransform {
    Vec3 axis_x{1.0f, 0.0f, 0.0f};
    Vec3 axis_y{0.0f, 1.0f, 0.0f};
    Vec3 axis_z{0.0f, 0.0f, 1.0f};
    Vec3 comp_axis_x{1.0f, 0.0f, 0.0f};
    Vec3 comp_axis_y{0.0f, 1.0f, 0.0f};
    Vec3 comp_axis_z{0.0f, 0.0f, 1.0f};
    Vec3 scale{1.0f, 1.0f, 1.0f};
    Vec3 comp_scale{1.0f, 1.0f, 1.0f};
    Vec3 comp_translation{0.0f, 0.0f, 0.0f};
    Vec3 location{0.0f, 0.0f, 0.0f};
    Vec3 pre_pivot{0.0f, 0.0f, 0.0f};
    bool has_comp_xform = false;

    static void compute_axes(const Rotator& rot, Vec3& out_x, Vec3& out_y, Vec3& out_z) {
        const Vec3 rad = rot.to_radians();
        const float sp = std::sin(rad.x), cp = std::cos(rad.x);
        const float sy = std::sin(rad.y), cy = std::cos(rad.y);
        const float sr = std::sin(rad.z), cr = std::cos(rad.z);
        out_x = Vec3(cp * cy, cp * sy, sp);
        out_y = Vec3(sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp);
        out_z = Vec3(-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp);
    }

    ActorTransform(const Vec3& loc, const Rotator& rot, const Vec3& scale3,
                   const Vec3& prepivot = Vec3(0.0f, 0.0f, 0.0f))
        : scale(scale3), location(loc), pre_pivot(prepivot) {
        compute_axes(rot, axis_x, axis_y, axis_z);
    }

    static ActorTransform of(const LevelActor& a) {
        ActorTransform xf(a.location, a.rotation,
                          Vec3(a.draw_scale * a.draw_scale_3d.x,
                               a.draw_scale * a.draw_scale_3d.y,
                               a.draw_scale * a.draw_scale_3d.z),
                          a.pre_pivot);
        const Vec3 cs(a.comp_scale * a.comp_scale_3d.x,
                      a.comp_scale * a.comp_scale_3d.y,
                      a.comp_scale * a.comp_scale_3d.z);
        if (std::abs(a.comp_rotation.pitch) > 1e-3f || std::abs(a.comp_rotation.yaw) > 1e-3f ||
            std::abs(a.comp_rotation.roll) > 1e-3f || a.comp_translation.length_sq() > 1e-6f ||
            std::abs(cs.x - 1.0f) > 1e-4f || std::abs(cs.y - 1.0f) > 1e-4f || std::abs(cs.z - 1.0f) > 1e-4f) {
            xf.has_comp_xform = true;
            xf.comp_scale = cs;
            xf.comp_translation = a.comp_translation;
            compute_axes(a.comp_rotation, xf.comp_axis_x, xf.comp_axis_y, xf.comp_axis_z);
        }
        return xf;
    }

    [[nodiscard]] Vec3 apply(const Vec3& local) const {
        Vec3 p = local;
        if (has_comp_xform) {
            p = comp_translation + comp_axis_x * (local.x * comp_scale.x) +
                comp_axis_y * (local.y * comp_scale.y) + comp_axis_z * (local.z * comp_scale.z);
        }
        const Vec3 l = p - pre_pivot;
        return location + axis_x * (l.x * scale.x) + axis_y * (l.y * scale.y) + axis_z * (l.z * scale.z);
    }
};

// FMatrix (row-vector convention): p' = p.x * XPlane + p.y * YPlane + p.z * ZPlane + WPlane.
struct ElemMatrix {
    Vec3 rows[4] = {Vec3(1, 0, 0), Vec3(0, 1, 0), Vec3(0, 0, 1), Vec3(0, 0, 0)};
    [[nodiscard]] Vec3 apply(const Vec3& p) const { return rows[0] * p.x + rows[1] * p.y + rows[2] * p.z + rows[3]; }
};

// The immutable FMatrix is serialized as four FPlane rows, and Mirror's Edge writes each FPlane
// W-first: (W, X, Y, Z). FPlane extends FVector, and the binary struct serializer emits the
// derived struct's own property (W) before the inherited X/Y/Z. So an identity TM is stored as
// [0 1 0 0 | 0 0 1 0 | 0 0 0 1 | 1 0 0 0]. Every KBoxElem/KSphereElem/KSphylElem TM in the
// retail SP00-SP09 packages is affine and orthonormal only in this layout. Reading it as
// X, Y, Z, W collapses each box onto x = W = 1, which removed the roof collision of meshes such
// as S_RooftopStructure_04 (Escape) so the pawn fell through roofs that retail walks across.
ElemMatrix read_elem_matrix(const UPKPackage& pkg, const UPropertyList& fields) {
    ElemMatrix m;
    const UProperty* tm = find_prop(fields, "TM");
    const auto& d = pkg.get_data();
    if (!tm || tm->size != 64 || tm->value_offset + 64 > d.size()) return m;
    float f[16];
    std::memcpy(f, d.data() + tm->value_offset, 64);
    for (int r = 0; r < 4; ++r) m.rows[r] = Vec3(f[r * 4 + 1], f[r * 4 + 2], f[r * 4 + 3]);
    return m;
}

void push_tri(std::vector<Vec3>& out, const Vec3& a, const Vec3& b, const Vec3& c) {
    out.push_back(a);
    out.push_back(b);
    out.push_back(c);
}

// Brute-force convex hull faces for small point sets (KConvexElem without cooked FaceTriData).
void convex_hull_triangles(const std::vector<Vec3>& pts, std::vector<Vec3>& out) {
    const size_t n = pts.size();
    if (n < 4 || n > 64) return;
    Vec3 centroid(0.0f, 0.0f, 0.0f);
    for (const Vec3& p : pts) centroid = centroid + p;
    centroid = centroid * (1.0f / static_cast<float>(n));
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = i + 1; j < n; ++j) {
            for (size_t k = j + 1; k < n; ++k) {
                Vec3 nrm = (pts[j] - pts[i]).cross(pts[k] - pts[i]);
                const float len = nrm.length();
                if (len < 1e-4f) continue;
                nrm = nrm / len;
                bool pos = false, neg = false;
                for (size_t m = 0; m < n && !(pos && neg); ++m) {
                    const float s = nrm.dot(pts[m] - pts[i]);
                    if (s > 0.05f) pos = true;
                    if (s < -0.05f) neg = true;
                }
                if (pos && neg) continue;
                push_tri(out, pts[i], pts[j], pts[k]);
            }
        }
    }
}

// Low-poly ellipsoid / capsule tessellation for KSphereElem / KSphylElem.
void append_capsule(std::vector<Vec3>& out, const ElemMatrix& tm, float radius, float half_length) {
    constexpr int kSeg = 12;
    constexpr int kRings = 6;  // per hemisphere
    auto ring_point = [&](int ring, int seg) {
        // ring 0 = bottom pole ... 2*kRings = top pole; the cylinder sits between the hemispheres.
        const bool top = ring > kRings;
        const int r = top ? ring - 1 : ring;
        const float phi = -0.5f * PI + PI * static_cast<float>(r) / static_cast<float>(2 * kRings - 1 + 1);
        const float theta = 2.0f * PI * static_cast<float>(seg) / static_cast<float>(kSeg);
        const float z = radius * std::sin(phi) + (ring > kRings ? half_length : -half_length);
        const float rr = radius * std::cos(phi);
        return tm.apply(Vec3(rr * std::cos(theta), rr * std::sin(theta), z));
    };
    const int rings = 2 * kRings + 1;
    for (int ring = 0; ring < rings; ++ring) {
        for (int seg = 0; seg < kSeg; ++seg) {
            const Vec3 a = ring_point(ring, seg);
            const Vec3 b = ring_point(ring, seg + 1);
            const Vec3 c = ring_point(ring + 1, seg + 1);
            const Vec3 d = ring_point(ring + 1, seg);
            push_tri(out, a, b, c);
            push_tri(out, a, c, d);
        }
    }
}

} // namespace

namespace {

// One KConvexElem: its faces as triangles, or the hull of its vertices when it lists none.
void append_convex_elem_triangles(const UPKPackage& pkg, const UPropertyList& el, std::vector<Vec3>& out) {
    const auto& d = pkg.get_data();
    std::vector<Vec3> verts;
    if (const UProperty* vd = find_prop(el, "VertexData")) {
        if (vd->value_offset + 4 <= d.size()) {
            int32_t count = 0;
            std::memcpy(&count, d.data() + vd->value_offset, 4);
            if (count > 0 && count < 100000 &&
                vd->value_offset + 4 + static_cast<size_t>(count) * 12 <= d.size() &&
                static_cast<size_t>(vd->size) == 4 + static_cast<size_t>(count) * 12) {
                verts.resize(static_cast<size_t>(count));
                for (int32_t k = 0; k < count; ++k) {
                    std::memcpy(&verts[k], d.data() + vd->value_offset + 4 + static_cast<size_t>(k) * 12, 12);
                }
            }
        }
    }
    if (verts.size() < 3) return;
    const UProperty* ft = find_prop(el, "FaceTriData");
    if (ft && ft->ints.size() >= 3) {
        for (size_t t = 0; t + 2 < ft->ints.size(); t += 3) {
            const int32_t i0 = ft->ints[t], i1 = ft->ints[t + 1], i2 = ft->ints[t + 2];
            if (i0 < 0 || i1 < 0 || i2 < 0 || static_cast<size_t>(std::max({i0, i1, i2})) >= verts.size()) continue;
            push_tri(out, verts[i0], verts[i1], verts[i2]);
        }
    } else {
        convex_hull_triangles(verts, out);
    }
}

}  // namespace

void read_actor_brush_hulls(const UPKPackage& pkg, int32_t actor_export_1based, std::vector<std::vector<Vec3>>& out) {
    UPropertyList actor;
    parse_export_properties(pkg, actor_export_1based, actor);
    const int32_t component = prop_object(actor, "BrushComponent");
    if (component <= 0 || static_cast<size_t>(component) > pkg.get_exports().size()) return;
    UPropertyList comp;
    parse_export_properties(pkg, component, comp);
    const UProperty* agg = find_prop(comp, "BrushAggGeom");
    if (!agg) return;

    Vec3 location(0.0f, 0.0f, 0.0f), pre_pivot(0.0f, 0.0f, 0.0f), scale3(1.0f, 1.0f, 1.0f);
    Rotator rotation(0, 0, 0);
    if (const UProperty* p = find_prop(actor, "Location")) location = Vec3(p->v[0], p->v[1], p->v[2]);
    if (const UProperty* p = find_prop(actor, "PrePivot")) pre_pivot = Vec3(p->v[0], p->v[1], p->v[2]);
    if (const UProperty* p = find_prop(actor, "DrawScale3D")) scale3 = Vec3(p->v[0], p->v[1], p->v[2]);
    if (const UProperty* p = find_prop(actor, "Rotation")) rotation = Rotator(p->vi[0], p->vi[1], p->vi[2]);
    const float scale = prop_float(actor, "DrawScale", 1.0f);
    const ActorTransform xf(location, rotation, scale3 * scale, pre_pivot);

    for (const UProperty& f : agg->fields) {
        if (f.name != "ConvexElems") continue;
        for (const UPropertyList& el : f.elements) {
            std::vector<Vec3> local;
            append_convex_elem_triangles(pkg, el, local);
            if (local.size() < 12) continue;  // a closed piece has four faces at least
            for (Vec3& v : local) v = xf.apply(v);
            out.push_back(std::move(local));
        }
    }
}

void append_agg_geom_triangles(const UPKPackage& pkg, const UProperty& agg_geom, std::vector<Vec3>& out) {
    for (const UProperty& f : agg_geom.fields) {
        if (f.name == "ConvexElems") {
            for (const UPropertyList& el : f.elements) append_convex_elem_triangles(pkg, el, out);
        } else if (f.name == "BoxElems") {
            for (const UPropertyList& el : f.elements) {
                const ElemMatrix tm = read_elem_matrix(pkg, el);
                // KBoxElem X/Y/Z are full lengths, not radii.
                const Vec3 h(0.5f * prop_float(el, "X"), 0.5f * prop_float(el, "Y"), 0.5f * prop_float(el, "Z"));
                if (h.x <= 0.0f || h.y <= 0.0f || h.z <= 0.0f) continue;
                Vec3 c[8];
                for (int k = 0; k < 8; ++k) {
                    c[k] = tm.apply(Vec3((k & 1) ? h.x : -h.x, (k & 2) ? h.y : -h.y, (k & 4) ? h.z : -h.z));
                }
                static const int kFaces[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1},
                                                 {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
                for (const auto& q : kFaces) {
                    push_tri(out, c[q[0]], c[q[1]], c[q[2]]);
                    push_tri(out, c[q[0]], c[q[2]], c[q[3]]);
                }
            }
        } else if (f.name == "SphereElems") {
            for (const UPropertyList& el : f.elements) {
                const float r = prop_float(el, "Radius");
                if (r > 0.0f) append_capsule(out, read_elem_matrix(pkg, el), r, 0.0f);
            }
        } else if (f.name == "SphylElems") {
            for (const UPropertyList& el : f.elements) {
                const float r = prop_float(el, "Radius");
                // KSphylElem: capsule along local Z, Length = distance between the sphere centres.
                if (r > 0.0f) append_capsule(out, read_elem_matrix(pkg, el), r, 0.5f * std::max(0.0f, prop_float(el, "Length")));
            }
        }
    }
}


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
            std::string name = exp.object_name;
            if (exp.object_number > 0) {
                name += "_" + std::to_string(exp.object_number - 1);
            }
            return {name, cls_name};
        }
        return {"Export_" + std::to_string(exp_i), "Unknown"};
    } else if (idx < 0) {
        size_t imp_i = -idx - 1;
        if (imp_i < imports_.size()) {
            const auto& imp = imports_[imp_i];
            std::string name = imp.object_name;
            if (imp.object_number > 0) {
                name += "_" + std::to_string(imp.object_number - 1);
            }
            return {name, imp.class_name};
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

// -----------------------------------------------------------------------------
// Script class defaults (Engine.u / GameFramework.u / TdGame.u / Td*Content.u)
// -----------------------------------------------------------------------------
// Level exports only serialize the properties that differ from their archetype: the class
// default object (Default__InterpActor) for actors, and for components the template inside it
// (Default__InterpActor.StaticMeshComponent0 -> Default__DynamicSMActor.StaticMeshComponent0 ->
// Default__StaticMeshComponent -> ...). Collision flags therefore have to be resolved through the
// script packages; e.g. Default__StaticMeshActorBase sets bCollideActors/bBlockActors while
// Actor/InterpActor leave them false, so a placed InterpActor only collides when the level
// designer enabled it on the instance.
namespace {

class ScriptDefaults {
public:
    // Property lists along an archetype chain, most-derived first.
    using Chain = std::vector<const UPropertyList*>;

    static ScriptDefaults& instance() {
        static ScriptDefaults s;
        return s;
    }

    // Indexes every class default object ("Default__Class") and component template
    // ("Default__Class.Template") of the script packages in `cooked_dir`. The ~3.7k objects are
    // parsed eagerly (a few ms) so the packages themselves (TdGame.u alone is ~57 MB) are released.
    void init(const std::string& cooked_dir) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (dir_ == cooked_dir) return;
        dir_ = cooked_dir;
        objects_.clear();
        cache_.clear();
        namespace fs = std::filesystem;
        size_t package_count = 0;
        for (const char* file : {"Engine.u", "GameFramework.u", "TdGame.u", "TdSharedContent.u", "TdSpContent.u",
                                 "TdTuContent.u", "TdSpBossContent.u", "TdTTContent.u", "TdMpContent.u"}) {
            const fs::path p = fs::path(cooked_dir) / file;
            if (!fs::exists(p)) continue;
            const UPKPackage pkg(p.string());
            if (!pkg.is_valid()) continue;
            ++package_count;
            const auto& ex = pkg.get_exports();
            for (size_t i = 0; i < ex.size(); ++i) {
                const int32_t idx = static_cast<int32_t>(i) + 1;
                std::string key = export_key(pkg, idx);
                if (key.empty() || objects_.count(key) != 0) continue;
                Entry entry;
                parse_export_properties(pkg, idx, entry.props);
                const int32_t arch = ex[i].archetype;
                entry.archetype = arch > 0 ? export_key(pkg, arch) : (arch < 0 ? import_key(pkg, arch) : std::string());
                if (ex[i].outer_index != 0) entry.template_class = pkg.get_export_class(ex[i]);
                objects_.emplace(std::move(key), std::move(entry));
            }
        }
        if (package_count > 0) {
            std::cout << "[Level] Script class defaults: " << objects_.size() << " default objects / templates from "
                      << package_count << " script packages" << std::endl;
        }
    }

    // Chain of "Default__Class" or "Default__Class.Template". Empty when unknown.
    const Chain& chain(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        return chain_locked(key, 0);
    }

    // Chain of an object a level package references through its import table.
    const Chain& import_chain(const UPKPackage& pkg, int32_t import_index) { return chain(import_key(pkg, import_index)); }

private:
    struct Entry {
        UPropertyList props;
        std::string archetype;       // key of the archetype object; empty = none (class default)
        std::string template_class;  // component class when this is a template inside a Default__ object
    };

    static bool is_default_name(const std::string& n) { return n.rfind("Default__", 0) == 0; }

    static std::string import_name(const FObjectImport& im) {
        return im.object_number > 0 ? im.object_name + "_" + std::to_string(im.object_number - 1) : im.object_name;
    }

    // "Default__Class" for a top-level class default object, "Default__Class.Template" for a
    // component template inside one; empty for anything else.
    static std::string export_key(const UPKPackage& pkg, int32_t idx) {
        const auto& ex = pkg.get_exports();
        if (idx <= 0 || static_cast<size_t>(idx) > ex.size()) return {};
        const std::string name = export_object_name(pkg, idx);
        const int32_t outer = ex[static_cast<size_t>(idx - 1)].outer_index;
        if (outer == 0) return is_default_name(name) ? name : std::string();
        if (outer < 0 || static_cast<size_t>(outer) > ex.size() || ex[static_cast<size_t>(outer - 1)].outer_index != 0) return {};
        const std::string outer_name = export_object_name(pkg, outer);
        return is_default_name(outer_name) ? outer_name + "." + name : std::string();
    }

    static std::string import_key(const UPKPackage& pkg, int32_t import_index) {
        const auto& imps = pkg.get_imports();
        const int32_t i = -import_index - 1;
        if (import_index >= 0 || i >= static_cast<int32_t>(imps.size())) return {};
        const FObjectImport& im = imps[static_cast<size_t>(i)];
        const std::string name = import_name(im);
        const int32_t outer = im.outer_index;
        if (outer < 0 && -outer - 1 < static_cast<int32_t>(imps.size())) {
            const std::string outer_name = import_name(imps[static_cast<size_t>(-outer - 1)]);
            if (is_default_name(outer_name)) return outer_name + "." + name;
        }
        return name;
    }

    const Chain& chain_locked(const std::string& key, int depth) {
        static const Chain kEmpty;
        if (key.empty() || depth > 4) return kEmpty;
        if (auto it = cache_.find(key); it != cache_.end()) return it->second;
        Chain out;
        std::string template_class;  // class of the last visited component template
        std::string cur = key;
        for (int guard = 0; !cur.empty() && guard < 16; ++guard) {
            auto it = objects_.find(cur);
            if (it == objects_.end()) break;
            out.push_back(&it->second.props);
            template_class = it->second.template_class;
            cur = it->second.archetype;
        }
        // A component template without an archetype derives from its class default object.
        if (!template_class.empty()) {
            const Chain& base = chain_locked("Default__" + template_class, depth + 1);
            out.insert(out.end(), base.begin(), base.end());
        }
        return cache_.emplace(key, std::move(out)).first->second;
    }

    std::mutex mutex_;
    std::string dir_;
    std::unordered_map<std::string, Entry> objects_;
    std::unordered_map<std::string, Chain> cache_;
};

// First value of a bool property along a property-list chain.
std::optional<bool> chain_bool(const ScriptDefaults::Chain& chain, const char* name) {
    for (const UPropertyList* l : chain) {
        if (const UProperty* p = find_prop(*l, name)) return p->b;
    }
    return std::nullopt;
}

}  // namespace

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

        // Follow the full in-package Prefab Archetype chain (some actors inherit 2+ levels deep)
        std::unordered_map<std::string, PropertyValue> arch_props;
        int32_t cur_arch = exp.archetype;
        int arch_guard = 0;
        while (cur_arch > 0 && static_cast<size_t>(cur_arch) <= exports_.size() && arch_guard++ < 8) {
            const auto& aexp = exports_[cur_arch - 1];
            if (aexp.serial_offset >= 0 && aexp.serial_size >= 8) {
                size_t ap_start = find_property_start(aexp);
                size_t aexp_end = static_cast<size_t>(aexp.serial_offset) + static_cast<size_t>(aexp.serial_size);
                if (ap_start < data_.size() && ap_start < aexp_end) {
                    auto ap = parse_properties(ap_start, std::min<size_t>(aexp_end - ap_start, data_.size() - ap_start));
                    for (auto& [k, v] : ap) {
                        if (arch_props.find(k) == arch_props.end()) {
                            arch_props.emplace(k, std::move(v));
                        }
                    }
                }
            }
            cur_arch = aexp.archetype;
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
        const int32_t exp_index = static_cast<int32_t>(&exp - exports_.data()) + 1;
        a.unique_name = export_object_name(*this, exp_index);
        if (const auto* p = get_prop("Base"); p && p->obj_ref_index > 0) {
            a.base_name = export_object_name(*this, p->obj_ref_index);
        }
        a.source_package = package_name_of(*this);

        if (const auto* p = get_prop("Location")) a.location = p->vec_val;
        if (const auto* p = get_prop("Rotation")) a.rotation = p->rot_val;
        if (const auto* p = get_prop("PrePivot")) a.pre_pivot = p->vec_val;
        if (const auto* p = get_prop("DrawScale")) a.draw_scale = p->float_val;
        if (const auto* p = get_prop("DrawScale3D")) a.draw_scale_3d = p->vec_val;
        if (const auto* p = get_prop("Tag")) a.tag = p->str_val;

        bool b_loi = false;
        if (const auto* p = get_prop("bLOIObject")) b_loi = p->bool_val;

        bool b_hidden = false;
        if (const auto* p = get_prop("bHidden")) b_hidden = p->bool_val;

        // InterpActor (elevators, doors, buttons, moving platforms) frequently stores its
        // mesh reference in ReplicatedMesh on the Actor / Archetype.
        if (const auto* rm = get_prop("ReplicatedMesh")) {
            if (!rm->obj_ref_name.empty() && rm->obj_ref_name != "None") {
                a.mesh_name = rm->obj_ref_name;
                if (rm->obj_ref_index != 0) {
                    a.mesh_path = object_canonical_path(*this, rm->obj_ref_index);
                }
            }
        }

        // Resolve StaticMeshComponent -> StaticMesh (checking instance and full archetype chain)
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
            bool seen_mesh = false;
            bool seen_scale = false;
            bool seen_scale3d = false;
            bool seen_trans = false;
            bool seen_rot = false;
            int32_t cur_comp = comp_idx;
            int c_guard = 0;
            while (cur_comp > 0 && static_cast<size_t>(cur_comp) <= exports_.size() && c_guard++ < 8) {
                const auto& cexp = exports_[cur_comp - 1];
                size_t cp_start = find_property_start(cexp);
                size_t cexp_end = static_cast<size_t>(cexp.serial_offset) + static_cast<size_t>(cexp.serial_size);
                if (cp_start < data_.size() && cp_start < cexp_end) {
                    auto cprops = parse_properties(cp_start, std::min<size_t>(cexp_end - cp_start, data_.size() - cp_start));
                    if (!seen_mesh) {
                        if (auto it = cprops.find("StaticMesh");
                            it != cprops.end() && !it->second.obj_ref_name.empty() && it->second.obj_ref_name != "None") {
                            a.mesh_name = it->second.obj_ref_name;
                            if (it->second.obj_ref_index != 0) {
                                a.mesh_path = object_canonical_path(*this, it->second.obj_ref_index);
                            }
                            seen_mesh = true;
                        }
                    }
                    if (auto it = cprops.find("HiddenGame"); it != cprops.end() && it->second.bool_val) {
                        b_hidden = true;
                    }
                    if (!seen_scale) {
                        if (auto it = cprops.find("Scale"); it != cprops.end() && it->second.float_val > 0.0f) {
                            a.comp_scale = it->second.float_val;
                            seen_scale = true;
                        }
                    }
                    if (!seen_scale3d) {
                        if (auto it = cprops.find("Scale3D"); it != cprops.end()) {
                            a.comp_scale_3d = it->second.vec_val;
                            seen_scale3d = true;
                        }
                    }
                    if (!seen_trans) {
                        if (auto it = cprops.find("Translation"); it != cprops.end()) {
                            a.comp_translation = it->second.vec_val;
                            seen_trans = true;
                        }
                    }
                    if (!seen_rot) {
                        if (auto it = cprops.find("Rotation"); it != cprops.end()) {
                            a.comp_rotation = it->second.rot_val;
                            seen_rot = true;
                        }
                    }
                }
                cur_comp = cexp.archetype;
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
            a.lightmap = read_component_lightmap(*this, comp_idx);
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

        // Hidden actors (bHidden / HiddenGame) and dedicated collision meshes are never drawn,
        // but UE3 still collides against them.
        if (b_hidden || low_mesh.find("blockingbox") != std::string::npos || low_mesh.find("_colmesh") != std::string::npos) {
            a.is_hidden = true;
        }

        bool is_elev_button = (low_mesh.find("elevatorbutton") != std::string::npos);
        bool is_elev_mesh = (low_mesh.find("elevator") != std::string::npos ||
                             low_mesh.find("s_lift") != std::string::npos);

        a.is_checkpoint = (low_class.find("checkpoint") != std::string::npos || low_obj.find("checkpoint") != std::string::npos);
        a.is_trigger = (low_class.find("trigger") != std::string::npos);
        a.is_zipline = (low_class.find("ziplinevolume") != std::string::npos);
        a.is_ladder = (low_class.find("laddervolume") != std::string::npos);
        a.is_ledge = (low_class.find("ledgewalkvolume") != std::string::npos || low_class.find("ledge") != std::string::npos);
        a.is_springboard = (low_class.find("springboard") != std::string::npos || low_obj.find("springboard") != std::string::npos ||
                            low_mesh.find("springboard") != std::string::npos ||
                            (b_loi && (low_mesh.find("constructionpackages") != std::string::npos ||
                                       low_mesh.find("runnerramp") != std::string::npos)));
        a.is_balance_beam = (low_class.find("balancewalkvolume") != std::string::npos || low_class.find("balance") != std::string::npos);
        a.is_swing_bar = (low_class.find("swingvolume") != std::string::npos || low_mesh.find("swingpole") != std::string::npos);
        a.is_enemy = (low_class.find("botpawn") != std::string::npos);
        a.is_bag = (low_class.find("bag") != std::string::npos || low_obj.find("bag") != std::string::npos || low_mesh.find("s_bag") != std::string::npos);
        a.is_elevator_part = is_elev_mesh || is_elev_button;
        a.is_runner_vision = b_loi || is_elev_button || a.is_springboard || a.is_zipline ||
                             a.is_ladder || (low_mesh.find("ladder") != std::string::npos) ||
                             a.is_swing_bar || a.is_bag || (low_obj.find("runner") != std::string::npos);

        // --- UE3 collision flags -------------------------------------------------------------
        // Blocks pawn movement: Actor.bCollideActors && Actor.bBlockActors &&
        //   CollisionComponent.CollideActors && BlockActors && BlockNonZeroExtent.
        // Blocks zero-extent traces: ... && BlockZeroExtent.
        // Each flag resolves instance -> in-package archetypes -> script archetype chain (the imported
        // archetype, else the class default object). Unset everywhere means false, as in UE3. Only when
        // the script packages are unavailable do the class-based fallbacks below apply.
        a.is_blocking_volume = (low_class == "blockingvolume" || low_class == "dynamicblockingvolume");
        ScriptDefaults& script_defaults = ScriptDefaults::instance();
        auto script_chain = [&](int32_t idx, std::string cls) -> const ScriptDefaults::Chain& {
            for (int guard = 0; idx > 0 && static_cast<size_t>(idx) <= exports_.size() && guard < 8; ++guard) {
                cls = get_export_class(exports_[idx - 1]);
                idx = exports_[idx - 1].archetype;
            }
            if (idx < 0) {
                const auto& c = script_defaults.import_chain(*this, idx);
                if (!c.empty()) return c;
            }
            return script_defaults.chain("Default__" + cls);
        };
        const ScriptDefaults::Chain& actor_defaults = script_chain(exp.archetype, cls_name);
        const bool is_volume = low_class.find("volume") != std::string::npos;
        const bool non_blocking_class = a.is_trigger || a.is_checkpoint || (is_volume && !a.is_blocking_volume);
        auto actor_bool = [&](const char* name, bool fallback) {
            if (const auto* p = get_prop(name)) return p->bool_val;
            if (!actor_defaults.empty()) return chain_bool(actor_defaults, name).value_or(false);
            return fallback;
        };
        const bool actor_collide = actor_bool("bCollideActors", !non_blocking_class);
        const bool actor_block = actor_bool("bBlockActors", !non_blocking_class);
        a.collide_complex = actor_bool("bCollideComplex", false);

        // Collision component property chain (instance, in-package archetypes, script templates).
        std::vector<UPropertyList> comp_chain;
        const ScriptDefaults::Chain* comp_defaults = nullptr;
        auto load_comp_chain = [&](int32_t idx) {
            if (idx <= 0 || static_cast<size_t>(idx) > exports_.size()) return;
            const std::string comp_cls = get_export_class(exports_[idx - 1]);
            const int32_t first = idx;
            for (int guard = 0; idx > 0 && static_cast<size_t>(idx) <= exports_.size() && guard < 8; ++guard) {
                comp_chain.emplace_back();
                parse_export_properties(*this, idx, comp_chain.back());
                idx = exports_[idx - 1].archetype;
            }
            comp_defaults = &script_chain(first, comp_cls);
        };
        auto comp_bool = [&](const char* name, bool fallback) {
            for (const auto& l : comp_chain) {
                if (const UProperty* p = find_prop(l, name)) return p->b;
            }
            if (comp_defaults && !comp_defaults->empty()) return chain_bool(*comp_defaults, name).value_or(false);
            return fallback;
        };

        auto find_brush_component_index = [&]() -> int32_t {
            int32_t b_idx = 0;
            if (const auto* p = get_prop("BrushComponent")) b_idx = p->obj_ref_index;
            if (b_idx <= 0) {
                if (const auto* p = get_prop("CollisionComponent")) b_idx = p->obj_ref_index;
            }
            if (b_idx <= 0) {
                for (const auto& [comp_name, c_idx] : exp.component_map) {
                    if (comp_name.find("Brush") != std::string::npos) {
                        b_idx = c_idx;
                        break;
                    }
                }
            }
            return b_idx;
        };

        bool block_zero_default = true;
        if (a.is_blocking_volume) {
            // Fallback mirrors BlockingVolume.BrushComponent0: BlockZeroExtent=false, BlockNonZeroExtent=true.
            block_zero_default = false;
            load_comp_chain(find_brush_component_index());
            for (const auto& l : comp_chain) {
                if (const UProperty* agg = find_prop(l, "BrushAggGeom")) {
                    std::vector<Vec3> local;
                    append_agg_geom_triangles(*this, *agg, local);
                    Vec3 pre_pivot(0.0f, 0.0f, 0.0f);
                    if (const auto* p = get_prop("PrePivot")) pre_pivot = p->vec_val;
                    const ActorTransform xf(a.location, a.rotation,
                                            Vec3(a.draw_scale * a.draw_scale_3d.x, a.draw_scale * a.draw_scale_3d.y,
                                                 a.draw_scale * a.draw_scale_3d.z),
                                                 pre_pivot);
                    a.brush_triangles.reserve(local.size());
                    for (const Vec3& v : local) a.brush_triangles.push_back(xf.apply(v));
                    break;
                }
            }
        } else if (comp_idx > 0) {
            load_comp_chain(comp_idx);
        }
        const bool comp_collide = comp_bool("CollideActors", true);
        const bool comp_block = comp_bool("BlockActors", true);
        const bool comp_block_nonzero = comp_bool("BlockNonZeroExtent", true);
        const bool comp_block_zero = comp_bool("BlockZeroExtent", block_zero_default);
        const bool comp_block_rb = comp_bool("BlockRigidBody", low_class == "kactor");
        a.is_collidable = actor_collide && comp_collide &&
                          ((actor_block && comp_block && comp_block_nonzero) ||
                           (low_class == "kactor" && comp_block_rb));
        a.blocks_traces = actor_collide && actor_block && comp_collide && comp_block_zero;

        // TdTutorialStart.BelongToChallenge: array of EMovementChallenge names.
        if (low_class == "tdtutorialstart") {
            UPropertyList tl;
            parse_export_properties(*this, exp_index, tl);
            if (const UProperty* ch = find_prop(tl, "BelongToChallenge")) a.tutorial_challenges = ch->names;
        }

        // Movement volumes (TdZiplineVolume, TdBalanceWalkVolume, TdLadderVolume, TdLedgeWalkVolume, TdSwingVolume)
        // store their world-space spline endpoints in Start and End properties.
        // Some cooked sublevels (e.g. Stormdrain_p.me1) contain unbaked TdLadderVolume brushes whose Start/End
        // were serialized as NaN or unclipped raycast endpoints (±1e7..1e9); fall back to BrushAggGeom world bounds.
        if (a.is_zipline || a.is_balance_beam || a.is_ladder || a.is_ledge || a.is_swing_bar) {
            auto is_valid_world_vec = [](const Vec3& v) {
                return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
                       std::abs(v.x) < 262144.0f && std::abs(v.y) < 262144.0f && std::abs(v.z) < 262144.0f;
            };
            const Vec3 orig_loc = is_valid_world_vec(a.location) ? a.location : Vec3(0.0f, 0.0f, 0.0f);
            a.location = orig_loc;

            const auto* ps = get_prop("Start");
            const auto* pe = get_prop("End");
            const auto* pc = get_prop("Center");
            const auto* pm = get_prop("Middle");
            if (const auto* pwn = get_prop("WallNormal"); pwn && is_valid_world_vec(pwn->vec_val) && pwn->vec_val.length_sq() > 0.25f) {
                a.wall_normal = pwn->vec_val.normalized();
            }
            const bool valid_se = ps && pe && is_valid_world_vec(ps->vec_val) && is_valid_world_vec(pe->vec_val) &&
                                  (pe->vec_val - ps->vec_val).length_sq() >= 100.0f;

            if (a.is_ladder) {
                if (const auto* pcet = get_prop("bCanExitAtTop")) {
                    a.can_exit_at_top = pcet->bool_val;
                }
                // ELadderType: LT_Ladder, LT_Pipe (a byte, or the enum's name).
                if (const auto* plt = get_prop("LadderType")) {
                    a.is_pipe = plt->int_val == 1 || plt->enum_name == "LT_Pipe";
                }
                const float dz = valid_se ? std::abs(pe->vec_val.z - ps->vec_val.z) : 0.0f;
                const float dxy = valid_se ? (pe->vec_val - ps->vec_val).length_xy() : 1e9f;
                if (valid_se && dz >= 60.0f && dz <= 5000.0f && dxy < 500.0f) {
                    a.location = ps->vec_val;
                    a.end_point = pe->vec_val;
                } else {
                    // Reconstruct vertical ladder span from BrushAggGeom convex hull vertices around orig_loc
                    std::vector<UPropertyList> bchain;
                    int32_t b_idx = find_brush_component_index();
                    for (int guard = 0; b_idx > 0 && static_cast<size_t>(b_idx) <= exports_.size() && guard < 8; ++guard) {
                        bchain.emplace_back();
                        parse_export_properties(*this, b_idx, bchain.back());
                        b_idx = exports_[b_idx - 1].archetype;
                    }
                    bool rebuilt = false;
                    for (const auto& l : bchain) {
                        if (const UProperty* agg = find_prop(l, "BrushAggGeom")) {
                            std::vector<Vec3> local;
                            append_agg_geom_triangles(*this, *agg, local);
                            if (!local.empty()) {
                                Vec3 pre_pivot(0.0f, 0.0f, 0.0f);
                                if (const auto* p = get_prop("PrePivot")) pre_pivot = p->vec_val;
                                const ActorTransform xf(orig_loc, a.rotation,
                                                        Vec3(a.draw_scale * a.draw_scale_3d.x,
                                                             a.draw_scale * a.draw_scale_3d.y,
                                                             a.draw_scale * a.draw_scale_3d.z),
                                                        pre_pivot);
                                float min_z = 1e9f, max_z = -1e9f;
                                for (const Vec3& v : local) {
                                    Vec3 wv = xf.apply(v);
                                    if (std::isfinite(wv.z)) {
                                        min_z = std::min(min_z, wv.z);
                                        max_z = std::max(max_z, wv.z);
                                    }
                                }
                                if (max_z - min_z >= 60.0f && max_z - min_z <= 5000.0f) {
                                    const float lx = (ps && std::isfinite(ps->vec_val.x) && std::abs(ps->vec_val.x - orig_loc.x) < 200.0f)
                                                         ? ps->vec_val.x
                                                         : orig_loc.x;
                                    const float ly = (ps && std::isfinite(ps->vec_val.y) && std::abs(ps->vec_val.y - orig_loc.y) < 200.0f)
                                                         ? ps->vec_val.y
                                                         : orig_loc.y;
                                    a.location = Vec3(lx, ly, min_z);
                                    a.end_point = Vec3(lx, ly, max_z);
                                    rebuilt = true;
                                }
                            }
                            break;
                        }
                    }
                    if (!rebuilt) {
                        a.is_ladder = false;
                    }
                }
            } else if (a.is_swing_bar && low_class.find("swingvolume") != std::string::npos) {
                // TdSwingVolume serializes Center/Location at the bar midpoint and WallNormal perpendicular
                // to the horizontal swing bar in XY (while Start/End span the vertical brush height or unbaked local coords).
                Vec3 bar_center = orig_loc;
                if (pc && is_valid_world_vec(pc->vec_val) && (pc->vec_val - orig_loc).length() < 250.0f) {
                    bar_center = pc->vec_val;
                } else if (pm && is_valid_world_vec(pm->vec_val) && (pm->vec_val - orig_loc).length() < 250.0f) {
                    bar_center = pm->vec_val;
                }
                Vec3 wn(a.wall_normal.x, a.wall_normal.y, 0.0f);
                if (wn.length_sq() < 1e-4f) {
                    const Vec3 rf = a.rotation.forward();
                    wn = Vec3(rf.x, rf.y, 0.0f);
                }
                if (wn.length_sq() < 1e-4f) wn = Vec3(1.0f, 0.0f, 0.0f);
                wn = wn.normalized();
                a.wall_normal = wn;
                const Vec3 bar_axis = Vec3(-wn.y, wn.x, 0.0f).normalized();
                float half_w = 115.0f;
                if (valid_se) {
                    const float se_xy = (pe->vec_val - ps->vec_val).length_xy();
                    const float se_len = (pe->vec_val - ps->vec_val).length();
                    if (se_xy >= 60.0f && (0.5f * (ps->vec_val + pe->vec_val) - bar_center).length() < 250.0f) {
                        a.location = ps->vec_val;
                        a.end_point = pe->vec_val;
                    } else {
                        half_w = std::clamp(0.5f * se_len, 80.0f, 200.0f);
                        a.location = bar_center - bar_axis * half_w;
                        a.end_point = bar_center + bar_axis * half_w;
                    }
                } else {
                    a.location = bar_center - bar_axis * half_w;
                    a.end_point = bar_center + bar_axis * half_w;
                }
            } else if (valid_se) {
                a.location = ps->vec_val;
                a.end_point = pe->vec_val;
                if (a.is_zipline) {
                    // TdZiplineVolume.SplineLocations (NumSplineSegments = 10): the editor bakes the
                    // cable as 11 points on the quadratic Bezier Start -> Middle -> End (the cooked
                    // SplineLocations match (1-t)^2 S + 2t(1-t) M + t^2 E at t = i/10 to 0.001 uu), and
                    // the native TdMove_ZipLine rides the polyline through them. Without a Middle the
                    // cable is the straight Start -> End line.
                    const Vec3 s = ps->vec_val;
                    const Vec3 e = pe->vec_val;
                    const Vec3 m = (pm && is_valid_world_vec(pm->vec_val) && pm->vec_val.length_sq() > 1.0f)
                                       ? pm->vec_val
                                       : (s + e) * 0.5f;
                    constexpr int kNumSplineSegments = 10;
                    a.spline_points.reserve(kNumSplineSegments + 1);
                    for (int i = 0; i <= kNumSplineSegments; ++i) {
                        const float t = static_cast<float>(i) / kNumSplineSegments;
                        a.spline_points.push_back(s * ((1.0f - t) * (1.0f - t)) + m * (2.0f * t * (1.0f - t)) + e * (t * t));
                    }
                    if (const auto* pmd = get_prop("MoveDirection");
                        pmd && is_valid_world_vec(pmd->vec_val) && pmd->vec_val.length_sq() > 0.25f) {
                        a.move_direction = pmd->vec_val.normalized();
                    }
                }
            } else {
                a.is_zipline = false;
                a.is_balance_beam = false;
                a.is_ledge = false;
                a.is_swing_bar = false;
            }
        }

        // Approximate initial world bounds (refined later when StaticMeshAsset is bound)
        float r_xy = 150.0f * std::abs(a.draw_scale) * std::max(std::abs(a.draw_scale_3d.x), std::abs(a.draw_scale_3d.y));
        float r_z = 100.0f * std::abs(a.draw_scale) * std::abs(a.draw_scale_3d.z);
        a.world_bounds = AABB(
            a.location - Vec3(r_xy, r_xy, r_z),
            a.location + Vec3(r_xy, r_xy, r_z)
        );

        a.is_soft_landing =
            low_mesh.find("constructiontent") != std::string::npos ||
            low_mesh.find("constructionpackage") != std::string::npos ||
            (low_mesh.find("cardboardbox") != std::string::npos &&
             low_mesh.find("cardboardtrash") == std::string::npos) ||
            low_mesh.find("mattress") != std::string::npos ||
            low_mesh.find("softlanding") != std::string::npos ||
            low_mesh.find("airbag") != std::string::npos ||
            low_obj.find("softlanding") != std::string::npos;
        if (!a.is_soft_landing) {
            for (const auto& mat : a.material_overrides) {
                std::string low_mat = mat;
                std::transform(low_mat.begin(), low_mat.end(), low_mat.begin(), ::tolower);
                if (low_mat.find("softlanding") != std::string::npos ||
                    low_mat.find("constructiontent") != std::string::npos) {
                    a.is_soft_landing = true;
                    break;
                }
            }
        }

        a.is_fall_height_volume = (low_class.find("fallheightvolume") != std::string::npos);
        if (a.is_fall_height_volume) {
            Vec3 center = a.location;
            if (const auto* pc = get_prop("Center"); pc && std::isfinite(pc->vec_val.z)) {
                center = pc->vec_val;
            }
            float offset = 0.0f;
            if (const auto* po = get_prop("FallHeightOffset"); po && std::isfinite(po->float_val)) {
                offset = po->float_val;
            }
            a.fall_height_target_z = center.z + offset;

            std::vector<UPropertyList> bchain;
            int32_t b_idx = find_brush_component_index();
            for (int guard = 0; b_idx > 0 && static_cast<size_t>(b_idx) <= exports_.size() && guard < 8; ++guard) {
                bchain.emplace_back();
                parse_export_properties(*this, b_idx, bchain.back());
                b_idx = exports_[b_idx - 1].archetype;
            }
            for (const auto& l : bchain) {
                if (const UProperty* agg = find_prop(l, "BrushAggGeom")) {
                    std::vector<Vec3> local;
                    append_agg_geom_triangles(*this, *agg, local);
                    if (!local.empty()) {
                        Vec3 pre_pivot(0.0f, 0.0f, 0.0f);
                        if (const auto* p = get_prop("PrePivot")) pre_pivot = p->vec_val;
                        const ActorTransform xf(a.location, a.rotation,
                                                a.draw_scale_3d * a.draw_scale, pre_pivot);
                        Vec3 bmin(1e9f, 1e9f, 1e9f), bmax(-1e9f, -1e9f, -1e9f);
                        for (const Vec3& v : local) {
                            const Vec3 w = xf.apply(v);
                            bmin.x = std::min(bmin.x, w.x); bmin.y = std::min(bmin.y, w.y); bmin.z = std::min(bmin.z, w.z);
                            bmax.x = std::max(bmax.x, w.x); bmax.y = std::max(bmax.y, w.y); bmax.z = std::max(bmax.z, w.z);
                        }
                        a.world_bounds = AABB(bmin, bmax);
                        break;
                    }
                }
            }
        }

        actors.push_back(a);
    }

    return actors;
}

void UPKPackage::extract_level_streaming_and_checkpoints(
    std::vector<LevelCheckpointInfo>& out_checkpoints,
    std::vector<LevelStreamingActionInfo>& out_streaming_actions,
    std::vector<std::string>& out_streaming_packages) const {

    // 1. Map all LevelStreamingKismet exports (1-based export index -> PackageName)
    std::unordered_map<int32_t, std::string> lsk_map;
    std::set<std::string> seen_pkgs(out_streaming_packages.begin(), out_streaming_packages.end());
    auto add_pkg_name = [&](const std::string& pname) {
        if (pname.empty() || pname == "None") return;
        if (seen_pkgs.insert(pname).second) {
            out_streaming_packages.push_back(pname);
        }
    };

    for (size_t i = 0; i < exports_.size(); ++i) {
        int32_t idx_1 = static_cast<int32_t>(i) + 1;
        std::string cls = get_export_class(exports_[i]);
        if (cls.find("LevelStreaming") == std::string::npos) continue;

        UPropertyList props;
        parse_export_properties(*this, idx_1, props);
        std::string pkg_name = prop_name(props, "PackageName");
        if (!pkg_name.empty() && pkg_name != "None") {
            lsk_map[idx_1] = pkg_name;
            add_pkg_name(pkg_name);
        }
    }

    // 2. Extract TdCheckpoint / TdPlaceableCheckpoint exports in PersistentLevel
    for (size_t i = 0; i < exports_.size(); ++i) {
        int32_t idx_1 = static_cast<int32_t>(i) + 1;
        std::string cls = get_export_class(exports_[i]);
        if (cls.find("Checkpoint") == std::string::npos && cls.find("CheckPoint") == std::string::npos) continue;
        if (cls.find("Volume") != std::string::npos || cls.find("Manager") != std::string::npos ||
            cls.find("SeqAct") != std::string::npos) continue;

        auto [outer_name, _] = resolve_object_index(exports_[i].outer_index);
        if (outer_name != "PersistentLevel") continue;

        UPropertyList props;
        parse_export_properties(*this, idx_1, props);

        LevelCheckpointInfo cp;
        cp.object_name = export_object_name(*this, idx_1);
        cp.checkpoint_name = prop_name(props, "CheckpointName", cp.object_name);
        cp.checkpoint_weight = prop_int(props, "CheckpointWeight", 0);
        cp.default_checkpoint = prop_bool(props, "DefaultCheckpoint", false);
        if (const UProperty* loc = find_prop(props, "Location")) {
            cp.location = Vec3(loc->v[0], loc->v[1], loc->v[2]);
        }
        if (const UProperty* rot = find_prop(props, "Rotation")) {
            cp.rotation = Rotator(static_cast<float>(rot->vi[0]),
                                  static_cast<float>(rot->vi[1]),
                                  static_cast<float>(rot->vi[2]));
        }
        if (const UProperty* sl = find_prop(props, "StreamingLevels")) {
            for (int32_t ref : sl->ints) {
                auto it = lsk_map.find(ref);
                if (it != lsk_map.end()) {
                    cp.streaming_levels.push_back(it->second);
                } else if (ref > 0 && static_cast<size_t>(ref) <= exports_.size()) {
                    UPropertyList lprops;
                    parse_export_properties(*this, ref, lprops);
                    std::string pname = prop_name(lprops, "PackageName");
                    if (!pname.empty() && pname != "None") {
                        cp.streaming_levels.push_back(pname);
                        add_pkg_name(pname);
                    }
                }
            }
        }
        out_checkpoints.push_back(std::move(cp));
    }

    std::sort(out_checkpoints.begin(), out_checkpoints.end(),
              [](const LevelCheckpointInfo& a, const LevelCheckpointInfo& b) {
                  if (a.default_checkpoint != b.default_checkpoint) return a.default_checkpoint;
                  if (a.checkpoint_weight != b.checkpoint_weight) return a.checkpoint_weight < b.checkpoint_weight;
                  return a.checkpoint_name < b.checkpoint_name;
              });

    // 3. Extract SeqAct_MultiLevelStreaming / SeqAct_LevelStreaming Kismet actions
    for (size_t i = 0; i < exports_.size(); ++i) {
        int32_t idx_1 = static_cast<int32_t>(i) + 1;
        std::string cls = get_export_class(exports_[i]);
        if (cls.find("SeqAct_MultiLevelStreaming") == std::string::npos &&
            cls.find("SeqAct_LevelStreaming") == std::string::npos) continue;

        UPropertyList props;
        parse_export_properties(*this, idx_1, props);

        LevelStreamingActionInfo act;
        act.object_name = export_object_name(*this, idx_1);

        if (const UProperty* levels = find_prop(props, "Levels")) {
            for (const auto& el : levels->elements) {
                int32_t lref = prop_object(el, "Level");
                std::string lname = prop_name(el, "LevelName");
                if (lref > 0 && lsk_map.find(lref) != lsk_map.end()) {
                    lname = lsk_map[lref];
                }
                if (!lname.empty() && lname != "None") {
                    act.package_names.push_back(lname);
                    add_pkg_name(lname);
                }
            }
        }
        int32_t single_level = prop_object(props, "Level");
        if (single_level > 0 && lsk_map.find(single_level) != lsk_map.end()) {
            act.package_names.push_back(lsk_map[single_level]);
            add_pkg_name(lsk_map[single_level]);
        }

        if (!act.package_names.empty()) {
            out_streaming_actions.push_back(std::move(act));
        }
    }
}

void UPKPackage::extract_elevators(
    const std::unordered_map<std::string, StaticMeshAsset>& mesh_lib,
    std::vector<ElevatorInstance>& out_elevators,
    std::vector<InterpDoorInfo>* out_doors) const {

    std::string pkg_name = package_name_of(*this);

    // 1. Collect all InterpActor / StaticMeshActor elevator cab candidates and buttons in this package
    struct CabCandidate {
        int32_t exp_idx_1 = 0;
        std::string obj_name;
        std::string mesh_name;
        Vec3 location{0.0f, 0.0f, 0.0f};
        Rotator rotation{0.0f, 0.0f, 0.0f};
        bool is_cab_mesh = false;
    };
    std::unordered_map<int32_t, CabCandidate> interp_actors;
    std::vector<int32_t> cab_actor_indices;
    std::vector<Vec3> button_positions;

    auto resolve_actor_mesh = [&](int32_t exp_idx_1, const UPropertyList& props) -> std::string {
        int32_t rm = prop_object(props, "ReplicatedMesh");
        if (rm != 0) {
            auto [mname, _] = resolve_object_index(rm);
            if (!mname.empty() && mname != "None") return mname;
        }
        int32_t comp = prop_object(props, "StaticMeshComponent");
        int guard = 0;
        while (comp > 0 && static_cast<size_t>(comp) <= exports_.size() && guard++ < 8) {
            UPropertyList cprops;
            parse_export_properties(*this, comp, cprops);
            int32_t sm = prop_object(cprops, "StaticMesh");
            if (sm != 0) {
                auto [mname, _] = resolve_object_index(sm);
                if (!mname.empty() && mname != "None") return mname;
            }
            comp = exports_[comp - 1].archetype;
        }
        return "";
    };

    for (size_t i = 0; i < exports_.size(); ++i) {
        int32_t idx_1 = static_cast<int32_t>(i) + 1;
        std::string cls = get_export_class(exports_[i]);
        auto [outer_name, _] = resolve_object_index(exports_[i].outer_index);
        if (outer_name != "PersistentLevel") continue;

        if (cls == "InterpActor" || cls == "StaticMeshActor" || cls == "TdTrigger") {
            UPropertyList props;
            parse_export_properties(*this, idx_1, props);
            Vec3 loc(0.0f, 0.0f, 0.0f);
            if (const UProperty* lp = find_prop(props, "Location")) {
                loc = Vec3(lp->v[0], lp->v[1], lp->v[2]);
            }
            Rotator rot(0.0f, 0.0f, 0.0f);
            if (const UProperty* rp = find_prop(props, "Rotation")) {
                rot = Rotator(static_cast<float>(rp->vi[0]),
                              static_cast<float>(rp->vi[1]),
                              static_cast<float>(rp->vi[2]));
            }
            std::string mname = resolve_actor_mesh(idx_1, props);
            if (mname.empty() && exports_[i].archetype > 0 && static_cast<size_t>(exports_[i].archetype) <= exports_.size()) {
                UPropertyList aprops;
                parse_export_properties(*this, exports_[i].archetype, aprops);
                mname = resolve_actor_mesh(exports_[i].archetype, aprops);
            }
            std::string low_m = to_lower(mname);

            if (low_m.find("elevatorbutton") != std::string::npos || cls == "TdTrigger") {
                button_positions.push_back(loc);
            }

            if (cls == "InterpActor") {
                CabCandidate cc;
                cc.exp_idx_1 = idx_1;
                cc.obj_name = export_object_name(*this, idx_1);
                cc.mesh_name = mname;
                cc.location = loc;
                cc.rotation = rot;
                cc.is_cab_mesh = (low_m.find("s_elevator_01") != std::string::npos ||
                                  low_m.find("elevatorwithtop") != std::string::npos ||
                                  low_m.find("constructionelevator") != std::string::npos ||
                                  low_m.find("cargoelevator") != std::string::npos ||
                                  low_m.find("elevator_01_carriage") != std::string::npos ||
                                  low_m.find("mall_elevator") != std::string::npos ||
                                  low_m.find("s_lift") != std::string::npos);
                interp_actors[idx_1] = cc;
                if (cc.is_cab_mesh) {
                    cab_actor_indices.push_back(idx_1);
                }
            }
        }
    }

    // 2. Map SeqAct_Interp VariableLinks (LinkDesc -> ObjValue export index)
    std::unordered_map<std::string, std::vector<int32_t>> group_to_actors;
    for (size_t i = 0; i < exports_.size(); ++i) {
        int32_t idx_1 = static_cast<int32_t>(i) + 1;
        if (get_export_class(exports_[i]) != "SeqAct_Interp") continue;

        UPropertyList saprops;
        parse_export_properties(*this, idx_1, saprops);
        const UProperty* vlinks = find_prop(saprops, "VariableLinks");
        if (!vlinks) continue;

        for (const auto& vl : vlinks->elements) {
            std::string desc = to_lower(prop_name(vl, "LinkDesc"));
            const UProperty* lvars = find_prop(vl, "LinkedVariables");
            if (!lvars || desc.empty()) continue;
            for (int32_t sv_ref : lvars->ints) {
                if (sv_ref <= 0 || static_cast<size_t>(sv_ref) > exports_.size()) continue;
                UPropertyList svprops;
                parse_export_properties(*this, sv_ref, svprops);
                int32_t obj_val = prop_object(svprops, "ObjValue");
                if (obj_val > 0) {
                    group_to_actors[desc].push_back(obj_val);
                }
            }
        }
    }

    // 3. Scan InterpTrackMove exports for vertical elevator trajectories
    for (size_t i = 0; i < exports_.size(); ++i) {
        int32_t track_idx_1 = static_cast<int32_t>(i) + 1;
        if (get_export_class(exports_[i]) != "InterpTrackMove") continue;

        int32_t group_idx_1 = exports_[i].outer_index;
        if (group_idx_1 <= 0 || static_cast<size_t>(group_idx_1) > exports_.size()) continue;
        if (get_export_class(exports_[group_idx_1 - 1]) != "InterpGroup") continue;

        UPropertyList gprops;
        parse_export_properties(*this, group_idx_1, gprops);
        std::string group_name = prop_name(gprops, "GroupName", "Elevator");
        std::string low_group = to_lower(group_name);
        const bool is_door_group = low_group.find("door") != std::string::npos;

        // Skip non-elevator Matinee tracks (cameras, helicopters, trains, boats, etc.)
        if (low_group.find("cam") != std::string::npos ||
            low_group.find("heli") != std::string::npos || low_group.find("chopper") != std::string::npos ||
            low_group.find("train") != std::string::npos || low_group.find("wagen") != std::string::npos ||
            low_group.find("boat") != std::string::npos || low_group.find("truck") != std::string::npos ||
            low_group.find("faith") != std::string::npos || low_group.find("pawn") != std::string::npos ||
            low_group.find("cop") != std::string::npos || low_group.find("gate") != std::string::npos) {
            continue;
        }
        if (is_door_group && !out_doors) continue;

        UPropertyList tprops;
        parse_export_properties(*this, track_idx_1, tprops);
        const UProperty* pos_track = find_prop(tprops, "PosTrack");
        if (!pos_track) continue;
        const UProperty* pts = find_prop(pos_track->fields, "Points");
        if (!pts || pts->elements.size() < 2) continue;

        std::vector<ElevatorKeyframe> raw_keys;
        for (const auto& pt : pts->elements) {
            ElevatorKeyframe kf;
            kf.time = prop_float(pt, "InVal", 0.0f);
            if (const UProperty* ov = find_prop(pt, "OutVal")) {
                kf.pos = Vec3(ov->v[0], ov->v[1], ov->v[2]);
            }
            raw_keys.push_back(kf);
        }

        std::string mf = prop_name(tprops, "MoveFrame", "IMF_RelativeToInitial");
        bool is_world_frame = (mf == "IMF_World");

        if (is_door_group) {
            // Sliding door leaves are placed closed; the end of the door matinee is the open
            // state. IMF_RelativeToInitial keys are expressed in the actor's initial rotation
            // frame (InterpTrackInstMove::InitialTM).
            auto git = group_to_actors.find(low_group);
            if (git == group_to_actors.end()) continue;
            for (int32_t a_idx : git->second) {
                auto ait = interp_actors.find(a_idx);
                if (ait == interp_actors.end()) continue;
                const CabCandidate& door = ait->second;
                Vec3 open = raw_keys.back().pos - raw_keys.front().pos;
                if (is_world_frame) {
                    open = raw_keys.back().pos - door.location;
                } else {
                    const ActorTransform rot_only(Vec3(0.0f, 0.0f, 0.0f), door.rotation, Vec3(1.0f, 1.0f, 1.0f));
                    open = rot_only.apply(open);
                }
                InterpDoorInfo info;
                info.package = pkg_name;
                info.actor_name = door.obj_name;
                info.group = group_name;
                info.open_offset = open;
                info.open_time = std::max(0.05f, raw_keys.back().time);
                out_doors->push_back(std::move(info));
            }
            continue;
        }

        float total_dz = raw_keys.back().pos.z - raw_keys.front().pos.z;
        if (std::abs(total_dz) < 400.0f) continue; // Floor-to-floor elevators travel at least 400 UE3 units vertically

        // Resolve linked InterpActor cab
        const CabCandidate* chosen_cab = nullptr;
        auto git = group_to_actors.find(low_group);
        if (git != group_to_actors.end()) {
            for (int32_t a_idx : git->second) {
                auto ait = interp_actors.find(a_idx);
                if (ait != interp_actors.end()) {
                    chosen_cab = &ait->second;
                    if (chosen_cab->is_cab_mesh) break;
                }
            }
        }
        bool is_named_elevator_group = (low_group.find("lift") != std::string::npos ||
                                        low_group.find("elev") != std::string::npos);
        if (!chosen_cab && is_named_elevator_group && !cab_actor_indices.empty()) {
            chosen_cab = &interp_actors[cab_actor_indices.front()];
        }
        if (!chosen_cab && !is_named_elevator_group) {
            continue;
        }
        if (chosen_cab && !chosen_cab->is_cab_mesh && !is_named_elevator_group) {
            continue;
        }
        // IMF_RelativeToInitial keys are offsets from the cab's initial location: without a
        // resolved cab InterpActor there is nothing to move (and no valid world position).
        if (!chosen_cab && !is_world_frame) {
            continue;
        }

        ElevatorInstance elev;
        elev.source_package = pkg_name;
        elev.name = pkg_name + ":" + group_name;
        elev.cab_actor_name = chosen_cab ? chosen_cab->obj_name : std::string();
        elev.move_frame_world = is_world_frame;
        elev.cab_mesh_name = (chosen_cab && !chosen_cab->mesh_name.empty()) ? chosen_cab->mesh_name : "S_Elevator_01";
        elev.rotation = chosen_cab ? chosen_cab->rotation : Rotator(0.0f, 0.0f, 0.0f);
        elev.start_pos = is_world_frame ? raw_keys.front().pos
                                        : (chosen_cab ? chosen_cab->location : raw_keys.front().pos);

        for (const auto& rk : raw_keys) {
            ElevatorKeyframe wk;
            wk.time = rk.time;
            wk.pos = is_world_frame ? rk.pos : (elev.start_pos + rk.pos);
            elev.keyframes.push_back(wk);
        }
        elev.end_pos = elev.keyframes.back().pos;
        elev.current_pos = elev.start_pos;
        elev.prev_pos = elev.start_pos;
        elev.ride_duration = std::max(1.0f, elev.keyframes.back().time);

        // Compute world-space offset from actor pivot to cab interior floor center
        // using the extracted UStaticMesh Bounds Origin & Extent (e.g. S_Elevator_01 has
        // local Origin=(-120, -132.5, 131), Extent=(120, 132.5, 131)).
        Vec3 local_origin(-120.0f, -132.5f, 131.0f);
        Vec3 local_extent(120.0f, 132.5f, 131.0f);
        std::string low_cab_mesh = to_lower(elev.cab_mesh_name);
        auto mit = mesh_lib.find(low_cab_mesh);
        if (mit != mesh_lib.end()) {
            local_origin = mit->second.bounds_origin;
            local_extent = mit->second.bounds_extent;
        } else if (low_cab_mesh.find("elevatorwithtop") != std::string::npos) {
            local_origin = Vec3(161.0f, -160.0f, 138.0f);
            local_extent = Vec3(159.0f, 200.0f, 154.0f);
        }

        Vec3 rad = elev.rotation.to_radians();
        float cy = std::cos(rad.y), sy = std::sin(rad.y);
        Vec3 axis_x(cy, sy, 0.0f);
        Vec3 axis_y(-sy, cy, 0.0f);
        elev.cab_local_offset = axis_x * local_origin.x + axis_y * local_origin.y;
        elev.cab_half_extents = Vec3(
            std::max(90.0f, std::abs(local_extent.x * cy) + std::abs(local_extent.y * sy)),
            std::max(90.0f, std::abs(local_extent.x * sy) + std::abs(local_extent.y * cy)),
            std::max(120.0f, local_extent.z)
        );

        // Find nearest elevator button or trigger to the cab floor center
        Vec3 floor_center = elev.start_pos + elev.cab_local_offset;
        elev.button_pos = floor_center + Vec3(0.0f, 0.0f, 130.0f);
        float best_btn_d2 = 1e18f;
        for (const auto& bp : button_positions) {
            float d2 = (bp - floor_center).length_sq();
            if (d2 < best_btn_d2 && d2 < (600.0f * 600.0f)) {
                best_btn_d2 = d2;
                elev.button_pos = bp;
            }
        }

        out_elevators.push_back(std::move(elev));
    }
}


std::string UPKPackage::get_full_export_path(int32_t exp_zero_idx) const {
    if (exp_zero_idx < 0 || static_cast<size_t>(exp_zero_idx) >= exports_.size()) {
        return "";
    }
    std::vector<std::string> parts;
    int32_t cur = exp_zero_idx;
    int guard = 0;
    while (cur >= 0 && static_cast<size_t>(cur) < exports_.size() && guard++ < 16) {
        const auto& e = exports_[cur];
        std::string nm = e.object_name;
        if (e.object_number > 0) {
            nm += "_" + std::to_string(e.object_number - 1);
        }
        parts.push_back(nm);
        if (e.outer_index > 0) {
            cur = e.outer_index - 1;
        } else {
            break;
        }
    }
    std::reverse(parts.begin(), parts.end());
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out += ".";
        out += parts[i];
    }
    return out;
}

std::vector<SoundClip> UPKPackage::extract_audio() const {
    std::vector<SoundClip> sounds;

    for (size_t exp_idx = 0; exp_idx < exports_.size(); ++exp_idx) {
        const auto& exp = exports_[exp_idx];
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

        // Parse UE3 SoundNodeWave properties (SampleRate, NumChannels, Duration, Subtitles)
        int prop_rate = 44100;
        int prop_channels = 2;
        float prop_duration = 0.0f;
        std::vector<SoundSubtitleLine> parsed_subtitles;
        size_t prop_start = find_property_start(exp);
        if (prop_start < data_.size() && prop_start < static_cast<size_t>(exp.serial_offset) + sz) {
            auto props = parse_properties(prop_start, static_cast<size_t>(exp.serial_offset) + sz - prop_start);
            if (auto it = props.find("SampleRate"); it != props.end() && it->second.int_val > 0) {
                prop_rate = it->second.int_val;
            }
            if (auto it = props.find("NumChannels"); it != props.end() && it->second.int_val > 0) {
                prop_channels = it->second.int_val;
            }
            if (auto it = props.find("Duration"); it != props.end() && it->second.float_val > 0.0f) {
                prop_duration = it->second.float_val;
            }
        }
        {
            UPropertyList uprops;
            parse_export_properties(*this, static_cast<int32_t>(exp_idx) + 1, uprops);
            if (const UProperty* subs = find_prop(uprops, "Subtitles")) {
                for (const auto& el : subs->elements) {
                    const UProperty* txt = find_prop(el, "Text");
                    if (txt && !txt->s.empty() && txt->s != " ") {
                        SoundSubtitleLine line;
                        line.time = prop_float(el, "Time", 0.0f);
                        line.text = txt->s;
                        parsed_subtitles.push_back(std::move(line));
                    }
                }
            }
        }

        // Search for OggS magic: 0x4F, 0x67, 0x67, 0x53
        for (size_t i = 0; i + 4 <= sz; ++i) {
            if (p[i] == 'O' && p[i+1] == 'g' && p[i+2] == 'g' && p[i+3] == 'S') {
                size_t ogg_sz = sz - i;
                if (i >= 12) {
                    int32_t elem_cnt = 0;
                    int32_t disk_sz = 0;
                    std::memcpy(&elem_cnt, p + i - 12, 4);
                    std::memcpy(&disk_sz, p + i - 8, 4);
                    if (disk_sz > 0 && static_cast<size_t>(disk_sz) <= sz - i) {
                        ogg_sz = static_cast<size_t>(disk_sz);
                    }
                }

                SoundClip clip;
                clip.name = exp.object_name;
                if (exp.object_number > 0) {
                    clip.name += "_" + std::to_string(exp.object_number - 1);
                }
                clip.full_path = get_full_export_path(static_cast<int32_t>(exp_idx));
                clip.pcm_data.assign(p + i, p + i + ogg_sz);
                clip.sample_rate = prop_rate;
                clip.channels = prop_channels;
                clip.duration = (prop_duration > 0.0f)
                    ? prop_duration
                    : static_cast<float>(ogg_sz) / (static_cast<float>(prop_rate) * 4.0f);
                clip.subtitles = std::move(parsed_subtitles);
                sounds.push_back(std::move(clip));
                break;
            }
        }
    }

    return sounds;
}

void UPKPackage::extract_level_loaded_sound_cues(std::vector<std::string>& out_cue_names) const {
    for (size_t i = 0; i < exports_.size(); ++i) {
        std::string cls = get_export_class(exports_[i]);
        if (cls != "SeqEvent_LevelLoaded" && cls != "SeqEvent_LevelBeginning") continue;

        UPropertyList props;
        parse_export_properties(*this, static_cast<int32_t>(i) + 1, props);
        const UProperty* ol = find_prop(props, "OutputLinks");
        if (!ol) continue;

        for (size_t k = 0; k < ol->elements.size(); ++k) {
            const UProperty* lnks = find_prop(ol->elements[k], "Links");
            if (!lnks) continue;
            for (const auto& l : lnks->elements) {
                int32_t op = prop_object(l, "LinkedOp");
                if (op <= 0 || static_cast<size_t>(op) > exports_.size()) continue;
                std::string ocls = get_export_class(exports_[op - 1]);
                if (ocls.find("PlaySound") != std::string::npos) {
                    UPropertyList sp;
                    parse_export_properties(*this, op, sp);
                    int32_t ps = prop_object(sp, "PlaySound");
                    if (ps != 0) {
                        auto [ps_name, ps_cls] = resolve_object_index(ps);
                        if (!ps_name.empty() && ps_name != "None" &&
                            std::find(out_cue_names.begin(), out_cue_names.end(), ps_name) == out_cue_names.end()) {
                            out_cue_names.push_back(ps_name);
                        }
                    }
                }
            }
        }
    }
}

void UPKPackage::extract_sound_cues_and_ambients(std::vector<SoundCueDef>& out_cues,
                                                 std::vector<AmbientEmitterInfo>& out_ambients) const {
    auto parse_exp_props = [&](int32_t one_based_idx) -> std::unordered_map<std::string, PropertyValue> {
        if (one_based_idx <= 0 || static_cast<size_t>(one_based_idx) > exports_.size()) return {};
        const auto& e = exports_[one_based_idx - 1];
        if (e.serial_size <= 0 || e.serial_offset < 0) return {};
        size_t p_start = find_property_start(e);
        size_t p_end = static_cast<size_t>(e.serial_offset) + static_cast<size_t>(e.serial_size);
        if (p_start >= data_.size() || p_start >= p_end) return {};
        return parse_properties(p_start, std::min(p_end - p_start, data_.size() - p_start));
    };

    // RawDistributionFloat -> {min, max}: the cooked LookupTable when present (it starts with the
    // distribution's output range), else the distribution object's Min / Max (Uniform) or Constant,
    // over the node class's defaults (values equal to its default subobject's are not serialized).
    auto distribution_range = [&](const UPropertyList& node_props, const char* name, float def_min,
                                  float def_max) -> std::pair<float, float> {
        const UProperty* p = find_prop(node_props, name);
        if (!p) return {def_min, def_max};
        if (const UProperty* lt = find_prop(p->fields, "LookupTable"); lt && lt->ints.size() >= 2) {
            float lo = 0.0f;
            float hi = 0.0f;
            std::memcpy(&lo, &lt->ints[0], sizeof(float));
            std::memcpy(&hi, &lt->ints[1], sizeof(float));
            return {lo, hi};
        }
        const int32_t dist = prop_object(p->fields, "Distribution");
        if (dist <= 0 || static_cast<size_t>(dist) > exports_.size()) return {def_min, def_max};
        UPropertyList dprops;
        parse_export_properties(*this, dist, dprops);
        if (get_export_class(exports_[dist - 1]) == "DistributionFloatConstant") {
            const float c = prop_float(dprops, "Constant", def_min);
            return {c, c};
        }
        return {prop_float(dprops, "Min", def_min), prop_float(dprops, "Max", def_max)};
    };

    // Recursive helper to traverse SoundNode graphs inside this package. Flattens the waves into
    // cue.wave_names and builds cue.nodes; returns the node's index there (-1 if none was added).
    auto walk_sound_node = [&](auto& self, int32_t obj_idx, SoundCueDef& cue, int depth) -> int {
        if (depth > 12 || obj_idx == 0) return -1;

        if (obj_idx < 0) {
            // Import reference (e.g. external SoundNodeWave)
            auto [imp_name, imp_cls] = resolve_object_index(obj_idx);
            if (imp_name.empty()) return -1;
            cue.wave_names.push_back(imp_name);
            cue.wave_weights.push_back(1.0f);
            if (imp_cls.find("SoundNodeWave") != std::string::npos) {
                cue.imported_waves.emplace_back(object_outermost_name(*this, obj_idx), imp_name);
            }
            SoundCueNode node;
            node.kind = SoundCueNode::Kind::Wave;
            node.wave = imp_name;
            cue.nodes.push_back(std::move(node));
            return static_cast<int>(cue.nodes.size()) - 1;
        }

        if (static_cast<size_t>(obj_idx) > exports_.size()) return -1;
        const auto& nexp = exports_[obj_idx - 1];
        std::string ncls = get_export_class(nexp);

        if (ncls.find("SoundNodeWave") != std::string::npos) {
            std::string wname = nexp.object_name;
            if (nexp.object_number > 0) {
                wname += "_" + std::to_string(nexp.object_number - 1);
            }
            cue.wave_names.push_back(wname);
            cue.wave_weights.push_back(1.0f);
            SoundCueNode node;
            node.kind = SoundCueNode::Kind::Wave;
            node.wave = wname;
            cue.nodes.push_back(std::move(node));
            return static_cast<int>(cue.nodes.size()) - 1;
        }

        if (ncls.find("SoundNodeLooping") != std::string::npos) {
            cue.looping = true;
        }
        if (ncls.find("SoundNodeConcatenator") != std::string::npos) {
            cue.is_concatenator = true;
        }
        if (ncls.find("SoundNodeModulator") != std::string::npos) {
            cue.has_modulator = true;
        }

        auto nprops = parse_exp_props(obj_idx);
        if (ncls.find("Attenuation") != std::string::npos) {
            // Extract MinRadius / MaxRadius from DistributionFloatUniform subobjects if present
            if (auto it = nprops.find("MinRadius"); it != nprops.end() && it->second.obj_ref_index > 0) {
                auto dprops = parse_exp_props(it->second.obj_ref_index);
                if (auto dit = dprops.find("Min"); dit != dprops.end()) cue.min_radius = dit->second.float_val;
            }
            if (auto it = nprops.find("MaxRadius"); it != nprops.end() && it->second.obj_ref_index > 0) {
                auto dprops = parse_exp_props(it->second.obj_ref_index);
                if (auto dit = dprops.find("Max"); dit != dprops.end()) cue.max_radius = dit->second.float_val;
            }
        }

        // The node itself: USoundNodeMixer / Random / Delay / Modulator (the rest pass through).
        const int node_index = static_cast<int>(cue.nodes.size());
        {
            SoundCueNode node;
            UPropertyList uprops;
            parse_export_properties(*this, obj_idx, uprops);
            auto float_array = [&uprops](const char* name) {
                std::vector<float> values;
                if (const UProperty* a = find_prop(uprops, name)) {
                    for (int32_t bits : a->ints) {
                        float f = 0.0f;
                        std::memcpy(&f, &bits, sizeof(float));
                        values.push_back(f);
                    }
                }
                return values;
            };
            if (ncls == "SoundNodeMixer") {
                node.kind = SoundCueNode::Kind::Mixer;
                node.weights = float_array("InputVolume");
                cue.has_mixer = true;
            } else if (ncls == "SoundNodeRandom") {
                node.kind = SoundCueNode::Kind::Random;
                node.weights = float_array("Weights");
            } else if (ncls == "SoundNodeDelay") {
                node.kind = SoundCueNode::Kind::Delay;
                const auto [lo, hi] = distribution_range(uprops, "DelayDuration", 0.0f, 0.0f);
                node.min_value = lo;
                node.max_value = hi;
            } else if (ncls == "SoundNodeModulator") {
                // Default__SoundNodeModulator's distributions: uniform 0.9 .. 1.1.
                node.kind = SoundCueNode::Kind::Modulator;
                const auto [vlo, vhi] = distribution_range(uprops, "VolumeModulation", 0.9f, 1.1f);
                const auto [plo, phi] = distribution_range(uprops, "PitchModulation", 0.9f, 1.1f);
                node.min_value = vlo;
                node.max_value = vhi;
                node.min_pitch = plo;
                node.max_pitch = phi;
            }
            cue.nodes.push_back(std::move(node));
        }

        // Follow ChildNodes array
        if (auto it = nprops.find("ChildNodes"); it != nprops.end() && it->second.raw_bytes.size() >= 4) {
            int32_t cnt = 0;
            std::memcpy(&cnt, it->second.raw_bytes.data(), 4);
            if (cnt > 0 && it->second.raw_bytes.size() >= 4 + static_cast<size_t>(cnt) * 4) {
                for (int32_t k = 0; k < cnt; ++k) {
                    int32_t child_idx = 0;
                    std::memcpy(&child_idx, it->second.raw_bytes.data() + 4 + static_cast<size_t>(k) * 4, 4);
                    const int child = self(self, child_idx, cue, depth + 1);
                    cue.nodes[static_cast<size_t>(node_index)].children.push_back(child);
                }
            }
        }
        return node_index;
    };

    // 1. Extract all SoundCues
    std::unordered_map<int32_t, size_t> cue_exp_to_idx;
    for (size_t i = 0; i < exports_.size(); ++i) {
        const auto& exp = exports_[i];
        std::string cls = get_export_class(exp);
        if (cls != "SoundCue") continue;

        SoundCueDef cue;
        cue.name = exp.object_name;
        cue.full_path = get_full_export_path(static_cast<int32_t>(i));

        auto props = parse_exp_props(static_cast<int32_t>(i) + 1);
        if (auto it = props.find("SoundGroup"); it != props.end() && !it->second.str_val.empty()) {
            cue.sound_group = it->second.str_val;
        }
        if (auto it = props.find("VolumeMultiplier"); it != props.end()) {
            cue.volume_multiplier = it->second.float_val;
        }
        if (auto it = props.find("PitchMultiplier"); it != props.end()) {
            cue.pitch_multiplier = it->second.float_val;
        }
        if (auto it = props.find("FirstNode"); it != props.end() && it->second.obj_ref_index != 0) {
            walk_sound_node(walk_sound_node, it->second.obj_ref_index, cue, 0);
        }

        cue_exp_to_idx[static_cast<int32_t>(i) + 1] = out_cues.size();
        out_cues.push_back(std::move(cue));
    }

    // 2. Extract 3D AmbientSound / AmbientSoundSimple emitters
    for (size_t i = 0; i < exports_.size(); ++i) {
        const auto& exp = exports_[i];
        std::string cls = get_export_class(exp);
        if (cls.find("AmbientSound") == std::string::npos) continue;

        auto props = parse_exp_props(static_cast<int32_t>(i) + 1);
        AmbientEmitterInfo em;
        if (auto it = props.find("Location"); it != props.end()) {
            em.location = it->second.vec_val;
        }

        if (auto it = props.find("AudioComponent"); it != props.end() && it->second.obj_ref_index > 0) {
            auto ac_props = parse_exp_props(it->second.obj_ref_index);
            if (auto cit = ac_props.find("VolumeMultiplier"); cit != ac_props.end()) {
                em.volume = cit->second.float_val;
            }
            if (auto cit = ac_props.find("PitchMultiplier"); cit != ac_props.end()) {
                em.pitch = cit->second.float_val;
            }
            if (auto cit = ac_props.find("SoundCue"); cit != ac_props.end() && cit->second.obj_ref_index != 0) {
                int32_t cue_obj = cit->second.obj_ref_index;
                if (auto map_it = cue_exp_to_idx.find(cue_obj); map_it != cue_exp_to_idx.end()) {
                    const auto& cdef = out_cues[map_it->second];
                    em.cue_name = cdef.name;
                    em.min_radius = cdef.min_radius;
                    em.max_radius = cdef.max_radius;
                    if (!cdef.wave_names.empty()) {
                        em.wave_name = cdef.wave_names.front();
                    }
                } else {
                    em.cue_name = cit->second.obj_ref_name;
                }
            }
        }

        if (!em.cue_name.empty() || !em.wave_name.empty()) {
            out_ambients.push_back(std::move(em));
        }
    }
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

        std::string full_obj_name = exp.object_name;
        if (exp.object_number > 0) {
            full_obj_name += "_" + std::to_string(exp.object_number - 1);
        }
        std::string key = full_obj_name;
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        const int32_t exp_index = static_cast<int32_t>(&exp - exports_.data()) + 1;
        const std::string canon_key = to_lower(object_canonical_path(*this, exp_index));
        if (!canon_key.empty() && out_meshes.find(canon_key) != out_meshes.end()) {
            continue;
        }

        size_t prop_start = find_property_start(exp);
        size_t exp_end = static_cast<size_t>(exp.serial_offset) + static_cast<size_t>(exp.serial_size);
        if (prop_start >= data_.size() || prop_start >= exp_end || exp_end > data_.size()) continue;

        size_t bytes_read = 0;
        const auto mesh_props = parse_properties(prop_start, exp_end - prop_start, &bytes_read);

        size_t rem_start = prop_start + bytes_read;
        if (rem_start + 48 > exp_end) continue;
        const uint8_t* rem = data_.data() + rem_start;
        size_t rem_len = exp_end - rem_start;

        StaticMeshAsset asset;
        asset.name = full_obj_name;
        std::memcpy(&asset.bounds_origin.x, rem + 0, 4);
        std::memcpy(&asset.bounds_origin.y, rem + 4, 4);
        std::memcpy(&asset.bounds_origin.z, rem + 8, 4);
        std::memcpy(&asset.bounds_extent.x, rem + 12, 4);
        std::memcpy(&asset.bounds_extent.y, rem + 16, 4);
        std::memcpy(&asset.bounds_extent.z, rem + 20, 4);
        std::memcpy(&asset.bounds_radius,   rem + 24, 4);

        // UStaticMesh collision: UseSimple{Box,Line}Collision (default TRUE) select the
        // RB_BodySetup (native tail + 28) aggregate geometry over the per-poly kDOP tree.
        if (auto it = mesh_props.find("UseSimpleBoxCollision"); it != mesh_props.end()) {
            asset.use_simple_box_collision = it->second.bool_val;
        }
        if (auto it = mesh_props.find("UseSimpleLineCollision"); it != mesh_props.end()) {
            asset.use_simple_line_collision = it->second.bool_val;
        }
        if (auto it = mesh_props.find("LightMapCoordinateIndex"); it != mesh_props.end()) {
            asset.lightmap_uv_index = std::clamp(it->second.int_val, 0, 7);
        }
        int32_t body_setup_ref = 0;
        std::memcpy(&body_setup_ref, rem + 28, 4);
        if (body_setup_ref > 0 && static_cast<size_t>(body_setup_ref) <= exports_.size()) {
            UPropertyList body_props;
            parse_export_properties(*this, body_setup_ref, body_props);
            asset.has_body_setup = true;
            if (const UProperty* agg = find_prop(body_props, "AggGeom")) {
                append_agg_geom_triangles(*this, *agg, asset.simple_collision);
            }
        }

        // kDOP Nodes (rem + 32) and kDOP Triangles (FkDOPCollisionTriangle<WORD>: v1, v2, v3, MaterialIndex)
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
        const size_t kdop_tri_off = off + 8;
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
            int32_t enable_collision = 1;
            int32_t first_index = 0;
            int32_t num_triangles = 0;
        };
        std::vector<RawElement> raw_elems;
        bool elem_ok = true;
        for (int32_t e = 0; e < elem_cnt; ++e) {
            if (cur + 40 > rem_len) { elem_ok = false; break; }
            RawElement re;
            std::memcpy(&re.material_ref, rem + cur, 4);
            std::memcpy(&re.enable_collision, rem + cur + 4, 4);
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

        // Per-poly collision: kDOP triangles index the LOD0 PositionVertexBuffer. Like
        // FStaticMeshCollisionDataProvider::ShouldCheckMaterial, triangles of elements whose
        // EnableCollision is off (light shafts, decals, glass panes...) never collide.
        if (kdop_ts == 8 && kdop_tc > 0 && kdop_tri_off + static_cast<size_t>(kdop_tc) * 8 <= rem_len) {
            asset.complex_collision.reserve(static_cast<size_t>(kdop_tc) * 3);
            for (int32_t t = 0; t < kdop_tc; ++t) {
                uint16_t tri[4];
                std::memcpy(tri, rem + kdop_tri_off + static_cast<size_t>(t) * 8, 8);
                if (tri[0] >= pos_num || tri[1] >= pos_num || tri[2] >= pos_num) continue;
                if (tri[3] < raw_elems.size() && raw_elems[tri[3]].enable_collision == 0) continue;
                for (int k = 0; k < 3; ++k) {
                    Vec3 p;
                    std::memcpy(&p, rem + pos_data_off + static_cast<size_t>(tri[k]) * 12, 12);
                    asset.complex_collision.push_back(p);
                }
            }
        }

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
        asset.num_uv_channels = uv_layout_ok ? num_uv : 0;

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
                // A set the mesh lacks repeats its last one (FLocalVertexFactory::InitRHI).
                read_uv(sv, std::min<int32_t>(1, num_uv - 1), v.u2, v.v2);
                if (num_uv >= 3 && uv_layout_ok) {
                    float e[4];
                    read_uv(sv, 2, e[0], e[1]);
                    read_uv(sv, std::min<int32_t>(3, num_uv - 1), e[2], e[3]);
                    asset.uv_extra.insert(asset.uv_extra.end(), e, e + 4);
                }
                v.color = has_color ? (static_cast<uint32_t>(sv[10]) | (static_cast<uint32_t>(sv[9]) << 8) |
                                       (static_cast<uint32_t>(sv[8]) << 16) | (static_cast<uint32_t>(sv[11]) << 24))
                                    : 0xFFFFFFFFu;
                asset.triangles.push_back(v);
                asset.source_vertex.push_back(idx[k]);
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

        if (!asset.triangles.empty() || !asset.simple_collision.empty() || !asset.complex_collision.empty()) {
            const bool asset_has_coll = asset.use_simple_box_collision
                                            ? !asset.simple_collision.empty()
                                            : !asset.complex_collision.empty();
            if (!canon_key.empty()) {
                out_meshes[canon_key] = asset;
            }
            if (exp.object_number > 0) {
                std::string base_key = exp.object_name;
                std::transform(base_key.begin(), base_key.end(), base_key.begin(), ::tolower);
                auto bit = out_meshes.find(base_key);
                if (bit == out_meshes.end() ||
                    (asset_has_coll && bit->second.simple_collision.empty() && bit->second.complex_collision.empty())) {
                    out_meshes[base_key] = asset;
                }
            }
            auto sit = out_meshes.find(key);
            if (sit == out_meshes.end() || asset_has_coll ||
                (sit->second.simple_collision.empty() && sit->second.complex_collision.empty())) {
                out_meshes[key] = std::move(asset);
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Level BSP collision (UModel, v536)
//
//   UObject tagged properties
//   FBoxSphereBounds Bounds                       (28 bytes)
//   Vectors  BulkSerialize<FVector>               (int32 elem size, int32 count, data)
//   Points   BulkSerialize<FVector>
//   Nodes    BulkSerialize<FBspNode>              (64 bytes: Plane, iVertPool@16, iSurf@20, ...,
//                                                  NumVertices@54, NodeFlags@55)
//   Surfs    TTransArray<FBspSurf>                (int32 Owner, int32 count, 56 bytes each:
//                                                  Material, PolyFlags@4, ...)
//   Verts    BulkSerialize<FVert>                 (24 bytes: pVertex@0, iSide, 2x FVector2D)
// -----------------------------------------------------------------------------
void UPKPackage::extract_bsp_collision(std::vector<Vec3>& out_triangles) const {
    constexpr uint8_t NF_NotCsg = 0x01;
    constexpr uint32_t PF_NotSolid = 0x00000008;
    for (size_t i = 0; i < exports_.size(); ++i) {
        const auto& exp = exports_[i];
        if (get_export_class(exp) != "Model") continue;
        if (resolve_object_index(exp.outer_index).first != "PersistentLevel") continue;
        UPropertyList props;
        size_t off = parse_export_properties(*this, static_cast<int32_t>(i) + 1, props);
        const size_t end = std::min(data_.size(), static_cast<size_t>(exp.serial_offset) + static_cast<size_t>(exp.serial_size));
        if (off == 0 || off + 28 > end) continue;
        off += 28;  // Bounds

        auto rd = [&](size_t o) {
            int32_t v = 0;
            std::memcpy(&v, data_.data() + o, 4);
            return v;
        };
        struct Bulk {
            size_t data = 0;
            int32_t elem = 0;
            int32_t count = 0;
        };
        auto bulk = [&](Bulk& b) -> bool {
            if (off + 8 > end) return false;
            b.elem = rd(off);
            b.count = rd(off + 4);
            if (b.elem <= 0 || b.elem > 256 || b.count < 0 ||
                off + 8 + static_cast<size_t>(b.elem) * static_cast<size_t>(b.count) > end) {
                return false;
            }
            b.data = off + 8;
            off = b.data + static_cast<size_t>(b.elem) * static_cast<size_t>(b.count);
            return true;
        };
        Bulk vectors, points, nodes, verts;
        if (!bulk(vectors) || !bulk(points) || !bulk(nodes)) continue;
        if (points.elem != 12 || nodes.elem != 64 || nodes.count == 0) continue;
        if (off + 8 > end) continue;
        const int32_t surf_count = rd(off + 4);  // TTransArray: Owner, then the TArray
        constexpr size_t kSurfSize = 56;
        const size_t surfs = off + 8;
        if (surf_count < 0 || surfs + kSurfSize * static_cast<size_t>(surf_count) > end) continue;
        off = surfs + kSurfSize * static_cast<size_t>(surf_count);
        if (!bulk(verts) || verts.elem < 4) continue;

        size_t emitted = 0;
        for (int32_t n = 0; n < nodes.count; ++n) {
            const uint8_t* nd = data_.data() + nodes.data + static_cast<size_t>(n) * 64;
            int32_t vert_pool = 0, surf = 0;
            std::memcpy(&vert_pool, nd + 16, 4);
            std::memcpy(&surf, nd + 20, 4);
            const uint8_t num_verts = nd[54];
            const uint8_t node_flags = nd[55];
            if (num_verts < 3 || (node_flags & NF_NotCsg)) continue;
            if (vert_pool < 0 || vert_pool + num_verts > verts.count) continue;
            if (surf >= 0 && surf < surf_count) {
                uint32_t poly_flags = 0;
                std::memcpy(&poly_flags, data_.data() + surfs + kSurfSize * static_cast<size_t>(surf) + 4, 4);
                if (poly_flags & PF_NotSolid) continue;
            }
            Vec3 poly[256];
            int count = 0;
            for (int k = 0; k < num_verts; ++k) {
                const int32_t pv = rd(verts.data + static_cast<size_t>(verts.elem) * static_cast<size_t>(vert_pool + k));
                if (pv < 0 || pv >= points.count) continue;
                std::memcpy(&poly[count++], data_.data() + points.data + static_cast<size_t>(pv) * 12, 12);
            }
            for (int k = 1; k + 1 < count; ++k) {
                out_triangles.push_back(poly[0]);
                out_triangles.push_back(poly[k]);
                out_triangles.push_back(poly[k + 1]);
                ++emitted;
            }
        }
        if (emitted > 0) {
            std::cout << "[Level] BSP " << package_name_of(*this) << ": " << nodes.count << " nodes -> " << emitted
                      << " collision triangles" << std::endl;
        }
    }
}

void UPKPackage::extract_bsp_render_geometry(std::vector<BspRenderBin>& out_bins, AABB& inout_bounds,
                                             LightMapSets* lightmaps) const {
    constexpr uint32_t PF_Invisible = 0x00000001u;
    constexpr uint32_t PF_TwoSided  = 0x00000100u;
    constexpr uint32_t PF_Portal    = 0x04000000u;

    std::unordered_map<std::string, size_t> bin_index;
    const auto bin_name = [](const std::string& mat_path, int32_t set) { return to_lower(mat_path) + "#" + std::to_string(set); };
    for (size_t i = 0; i < out_bins.size(); ++i) {
        bin_index.emplace(bin_name(out_bins[i].material, out_bins[i].lightmap_set), i);
    }
    auto get_bin = [&](const std::string& mat_path, int32_t set) -> std::vector<Vertex>& {
        const std::string key = bin_name(mat_path, set);
        auto it = bin_index.find(key);
        if (it != bin_index.end()) return out_bins[it->second].vertices;
        const size_t idx = out_bins.size();
        out_bins.push_back(BspRenderBin{mat_path, set, {}});
        bin_index.emplace(key, idx);
        return out_bins.back().vertices;
    };

    auto unpack_normal = [](const uint8_t* b) {
        return Vec3(static_cast<float>(b[0]) / 127.5f - 1.0f,
                    static_cast<float>(b[1]) / 127.5f - 1.0f,
                    static_cast<float>(b[2]) / 127.5f - 1.0f);
    };

    for (size_t i = 0; i < exports_.size(); ++i) {
        const auto& exp = exports_[i];
        if (get_export_class(exp) != "Model") continue;
        if (resolve_object_index(exp.outer_index).first != "PersistentLevel") continue;
        UPropertyList props;
        size_t off = parse_export_properties(*this, static_cast<int32_t>(i) + 1, props);
        const size_t end = std::min(data_.size(), static_cast<size_t>(exp.serial_offset) + static_cast<size_t>(exp.serial_size));
        if (off == 0 || off + 28 > end) continue;
        off += 28;  // Bounds

        auto rd = [&](size_t o) {
            int32_t v = 0;
            std::memcpy(&v, data_.data() + o, 4);
            return v;
        };
        struct Bulk {
            size_t data = 0;
            int32_t elem = 0;
            int32_t count = 0;
        };
        auto bulk = [&](Bulk& b) -> bool {
            if (off + 8 > end) return false;
            b.elem = rd(off);
            b.count = rd(off + 4);
            if (b.elem <= 0 || b.elem > 256 || b.count < 0 ||
                off + 8 + static_cast<size_t>(b.elem) * static_cast<size_t>(b.count) > end) {
                return false;
            }
            b.data = off + 8;
            off = b.data + static_cast<size_t>(b.elem) * static_cast<size_t>(b.count);
            return true;
        };
        Bulk vectors, points, nodes, verts;
        if (!bulk(vectors) || !bulk(points) || !bulk(nodes)) continue;
        if (points.elem != 12 || nodes.elem != 64 || nodes.count == 0) continue;
        if (off + 8 > end) continue;
        const int32_t surf_count = rd(off + 4);  // TTransArray: Owner, then the TArray
        constexpr size_t kSurfSize = 56;
        const size_t surfs = off + 8;
        if (surf_count < 0 || surfs + kSurfSize * static_cast<size_t>(surf_count) > end) continue;
        off = surfs + kSurfSize * static_cast<size_t>(surf_count);
        if (!bulk(verts) || verts.elem < 4) continue;

        // Locate the cooked FModelVertexBuffer at the end of UModel (36 bytes/vertex:
        // Position(12), TangentX(4), TangentZ(4), TexCoord(8), ShadowTexCoord(8)).
        const uint8_t* vb_data = nullptr;
        int32_t vb_count = 0;
        for (size_t scan = off; scan + 8 <= end; scan += 4) {
            const int32_t esz = rd(scan);
            const int32_t cnt = rd(scan + 4);
            if (esz == 36 && cnt >= 0 && scan + 8 + 36ULL * static_cast<size_t>(cnt) == end) {
                vb_data = data_.data() + scan + 8;
                vb_count = cnt;
                break;
            }
        }

        // The baked lighting of the elements that draw this model's nodes, and the texture set
        // each is in.
        std::vector<ActorLightMap> light_maps;
        std::vector<int32_t> node_light_map;
        std::vector<int32_t> light_map_set;
        if (lightmaps) {
            read_model_lightmaps(*this, static_cast<int32_t>(i) + 1, light_maps, node_light_map);
            light_map_set.reserve(light_maps.size());
            for (const ActorLightMap& lm : light_maps) {
                const int32_t set = lightmaps->index_of(lm.package_path, lm.textures);
                light_map_set.push_back(set < 2047 ? set : -1);
            }
        }

        for (int32_t n = 0; n < nodes.count; ++n) {
            const uint8_t* nd = data_.data() + nodes.data + static_cast<size_t>(n) * 64;
            int32_t vert_pool = 0, surf = 0, vert_idx = 0;
            std::memcpy(&vert_pool, nd + 16, 4);
            std::memcpy(&surf, nd + 20, 4);
            std::memcpy(&vert_idx, nd + 24, 4);
            const uint8_t num_verts = nd[54];
            if (num_verts < 3) continue;
            if (vert_pool < 0 || vert_pool + num_verts > verts.count) continue;
            if (surf < 0 || surf >= surf_count) continue;

            const size_t so = surfs + kSurfSize * static_cast<size_t>(surf);
            const int32_t mat_ref = rd(so);
            uint32_t poly_flags = 0;
            std::memcpy(&poly_flags, data_.data() + so + 4, 4);
            if (poly_flags & (PF_Invisible | PF_Portal)) continue;

            std::string mat_path = (mat_ref != 0) ? object_canonical_path(*this, mat_ref) : std::string();
            if (mat_path == "EngineMaterials.RemoveSurfaceMaterial") continue;
            if (mat_path.empty()) mat_path = "EngineMaterials.DefaultMaterial";

            const int32_t p_base = rd(so + 8);
            const int32_t v_normal = rd(so + 12);
            const int32_t v_tex_u = rd(so + 16);
            const int32_t v_tex_v = rd(so + 20);

            float px = 0.0f, py = 0.0f, pz = 1.0f;
            std::memcpy(&px, nd + 0, 4);
            std::memcpy(&py, nd + 4, 4);
            std::memcpy(&pz, nd + 8, 4);
            Vec3 surf_n(px, py, pz);
            if (vectors.elem == 12 && v_normal >= 0 && v_normal < vectors.count) {
                std::memcpy(&surf_n, data_.data() + vectors.data + static_cast<size_t>(v_normal) * 12, 12);
            }
            surf_n = (surf_n.length_sq() > 1e-12f) ? surf_n.normalized() : Vec3(0.0f, 0.0f, 1.0f);

            Vec3 base_pt(0.0f, 0.0f, 0.0f), tex_u(1.0f, 0.0f, 0.0f), tex_v(0.0f, 1.0f, 0.0f);
            if (p_base >= 0 && p_base < points.count) {
                std::memcpy(&base_pt, data_.data() + points.data + static_cast<size_t>(p_base) * 12, 12);
            }
            if (vectors.elem == 12 && v_tex_u >= 0 && v_tex_u < vectors.count) {
                std::memcpy(&tex_u, data_.data() + vectors.data + static_cast<size_t>(v_tex_u) * 12, 12);
            }
            if (vectors.elem == 12 && v_tex_v >= 0 && v_tex_v < vectors.count) {
                std::memcpy(&tex_v, data_.data() + vectors.data + static_cast<size_t>(v_tex_v) * 12, 12);
            }

            // The node's light map. Its vertices' ShadowTexCoord runs 0..1 across the element's
            // rectangle of the atlas, and only the cooked vertex buffer has it.
            const ActorLightMap* lm = nullptr;
            int32_t lm_set = -1;
            if (static_cast<size_t>(n) < node_light_map.size() && node_light_map[static_cast<size_t>(n)] >= 0 && vb_data) {
                const size_t at = static_cast<size_t>(node_light_map[static_cast<size_t>(n)]);
                if (light_map_set[at] >= 0) {
                    lm = &light_maps[at];
                    lm_set = light_map_set[at];
                }
            }
            uint32_t lm_scale[3] = {0u, 0u, 0u};
            if (lm) {
                for (int k = 0; k < 3; ++k) lm_scale[k] = pack_rgb9e5(lm->scale[k]);
            }

            Vertex poly_v[256];
            int count = 0;
            for (int k = 0; k < num_verts; ++k) {
                Vertex v{};
                v.color = 0xFFFFFFFFu;
                if (vb_data && vert_idx >= 0 && vert_idx + num_verts <= vb_count) {
                    const uint8_t* vptr = vb_data + static_cast<size_t>(vert_idx + k) * 36;
                    std::memcpy(&v.position, vptr + 0, 12);
                    const Vec3 tx = unpack_normal(vptr + 12);
                    const Vec3 tz = unpack_normal(vptr + 16);
                    v.normal = (tz.length_sq() > 0.25f) ? tz.normalized() : surf_n;
                    v.tangent = (tx.length_sq() > 0.25f) ? tx.normalized() : Vec3(1.0f, 0.0f, 0.0f);
                    v.tangent_sign = (vptr[19] >= 128) ? 1.0f : -1.0f;
                    std::memcpy(&v.u, vptr + 20, 4);
                    std::memcpy(&v.v, vptr + 24, 4);
                    std::memcpy(&v.u2, vptr + 28, 4);
                    std::memcpy(&v.v2, vptr + 32, 4);
                } else {
                    const int32_t pv = rd(verts.data + static_cast<size_t>(verts.elem) * static_cast<size_t>(vert_pool + k));
                    if (pv < 0 || pv >= points.count) continue;
                    std::memcpy(&v.position, data_.data() + points.data + static_cast<size_t>(pv) * 12, 12);
                    v.normal = surf_n;
                    v.tangent = (tex_u.length_sq() > 1e-12f) ? tex_u.normalized() : Vec3(1.0f, 0.0f, 0.0f);
                    v.tangent_sign = 1.0f;
                    const Vec3 dp = v.position - base_pt;
                    v.u = dp.dot(tex_u) / 128.0f;
                    v.v = dp.dot(tex_v) / 128.0f;
                    if (verts.elem >= 16) {
                        const uint8_t* fv = data_.data() + verts.data + static_cast<size_t>(verts.elem) * static_cast<size_t>(vert_pool + k);
                        std::memcpy(&v.u2, fv + 8, 4);
                        std::memcpy(&v.v2, fv + 12, 4);
                    }
                }
                if (lm) {
                    v.lm_u = std::max(0.0f, v.u2 * lm->coord_scale[0] + lm->coord_bias[0]);
                    v.lm_v = v.v2 * lm->coord_scale[1] + lm->coord_bias[1];
                    v.lm0 = lm_scale[0];
                    v.lm1 = lm_scale[1];
                    v.lm2 = lm_scale[2];
                } else if (lightmaps) {
                    // Baked no light map: it takes no light (three samples of nothing).
                    v.lm_u = -1.0f;
                    v.lm0 = v.lm1 = v.lm2 = 0u;
                }
                // A model's vertex factory gives materials one texture coordinate set, TexCoord
                // (UModelComponent binds ShadowTexCoord as the light-map coordinate only), so
                // every TextureCoordinate index reads it, as the last set does on any mesh.
                v.u2 = v.u;
                v.v2 = v.v;
                inout_bounds.expand(v.position);
                poly_v[count++] = v;
            }
            if (count < 3) continue;

            std::vector<Vertex>& dst = get_bin(mat_path, lm_set);
            for (int k = 1; k + 1 < count; ++k) {
                const Vertex& a = poly_v[0];
                const Vertex& b = poly_v[k];
                const Vertex& c = poly_v[k + 1];
                // Ensure counter-clockwise screen winding when viewed along +surf_n.
                const Vec3 cr = (b.position - a.position).cross(c.position - a.position);
                const bool flip = (cr.dot(surf_n) > 0.0f);
                const Vertex& v1 = flip ? c : b;
                const Vertex& v2 = flip ? b : c;
                dst.push_back(a);
                dst.push_back(v1);
                dst.push_back(v2);
                if (poly_flags & PF_TwoSided) {
                    Vertex ba = a, bv1 = v2, bv2 = v1;
                    ba.normal = ba.normal * -1.0f;
                    bv1.normal = bv1.normal * -1.0f;
                    bv2.normal = bv2.normal * -1.0f;
                    ba.tangent_sign = -ba.tangent_sign;
                    bv1.tangent_sign = -bv1.tangent_sign;
                    bv2.tangent_sign = -bv2.tangent_sign;
                    dst.push_back(ba);
                    dst.push_back(bv1);
                    dst.push_back(bv2);
                }
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Level geometry: real UStaticMesh render batches + UE3 collision
// -----------------------------------------------------------------------------
namespace {

// Legacy procedural palette (ME_NO_MATERIALS=1 / no material library): vertex colour per
// architectural class (ABGR packed).
uint32_t legacy_palette_color(const LevelActor& a, const std::string& low_mesh) {
    if (a.is_runner_vision) return 0xFF1414E6;  // Runner Vision Red (#E61414)
    if (low_mesh.find("glass") != std::string::npos || low_mesh.find("window") != std::string::npos ||
        low_mesh.find("skylight") != std::string::npos) {
        return 0xFFE8C890;  // Reflective cyan-blue architectural glass
    }
    if (low_mesh.find("catwalk") != std::string::npos || low_mesh.find("fence") != std::string::npos ||
        low_mesh.find("railing") != std::string::npos || low_mesh.find("stair") != std::string::npos ||
        low_mesh.find("scaffold") != std::string::npos) {
        return 0xFFA8A098;  // Dark steel catwalk/railing
    }
    if (low_mesh.find("airduct") != std::string::npos || low_mesh.find("vent") != std::string::npos ||
        low_mesh.find("ac") != std::string::npos || low_mesh.find("pipe") != std::string::npos) {
        return 0xFFDCD8D4;  // Galvanized metallic silver HVAC/ducting
    }
    if (low_mesh.find("crane") != std::string::npos || a.is_checkpoint) return 0xFF2898F0;  // Industrial orange
    if (low_mesh.find("bd_") != std::string::npos || low_mesh.find("building") != std::string::npos ||
        low_mesh.find("s_c_") != std::string::npos || low_mesh.find("s_r_") != std::string::npos ||
        low_mesh.find("sky") != std::string::npos) {
        // Subtle architectural variation across city blocks using a hash of the location
        static const uint32_t kBuildingPalette[4] = {0xFFF6F4F2, 0xFFF2ECE4, 0xFFEAE6E2, 0xFFE8DED2};
        const uint32_t h_idx = static_cast<uint32_t>(std::abs(int(a.location.x * 0.01f) + int(a.location.y * 0.01f))) % 4;
        return kBuildingPalette[h_idx];
    }
    return 0xFFF2F0EE;  // Clean Mirror's Edge white architectural concrete
}

const StaticMeshAsset* find_mesh(const std::unordered_map<std::string, StaticMeshAsset>& lib, const std::string& name) {
    if (name.empty()) return nullptr;
    auto it = lib.find(to_lower(name));
    return it != lib.end() ? &it->second : nullptr;
}

const StaticMeshAsset* find_mesh(const std::unordered_map<std::string, StaticMeshAsset>& lib, const LevelActor& a) {
    if (!a.mesh_path.empty()) {
        if (auto it = lib.find(to_lower(a.mesh_path)); it != lib.end()) {
            return &it->second;
        }
    }
    return find_mesh(lib, a.mesh_name);
}

// World AABB of a mesh actor from the transformed corners of the mesh bounds box.
AABB transformed_mesh_bounds(const LevelActor& a, const StaticMeshAsset& sm) {
    const ActorTransform xf = ActorTransform::of(a);
    AABB box(Vec3(1e30f, 1e30f, 1e30f), Vec3(-1e30f, -1e30f, -1e30f));
    for (int k = 0; k < 8; ++k) {
        const Vec3 c(sm.bounds_origin.x + ((k & 1) ? sm.bounds_extent.x : -sm.bounds_extent.x),
                     sm.bounds_origin.y + ((k & 2) ? sm.bounds_extent.y : -sm.bounds_extent.y),
                     sm.bounds_origin.z + ((k & 4) ? sm.bounds_extent.z : -sm.bounds_extent.z));
        box.expand(xf.apply(c));
    }
    return box;
}

// Emits StaticMeshActor LOD0 triangles in world space, binned per scene material.
class MeshEmitter {
public:
    // A bin is the geometry of one material lit by one light-map texture set.
    static int32_t bin_key(int32_t material, int32_t lightmap_set) { return material | ((lightmap_set + 1) << 20); }

    explicit MeshEmitter(std::vector<std::string>* material_paths, MaterialUVResolver* material_uvs = nullptr,
                         LightMapSets* lightmaps = nullptr)
        : material_paths_(material_paths), material_uvs_(material_uvs), lightmaps_(lightmaps) {
        if (material_paths_) {
            for (size_t i = 0; i < material_paths_->size(); ++i) {
                ids_.emplace(to_lower((*material_paths_)[i]), static_cast<int32_t>(i));
            }
        }
    }
    [[nodiscard]] bool use_materials() const { return material_paths_ != nullptr; }

    void emit_bsp(const std::vector<BspRenderBin>& bsp_bins,
                  std::map<int32_t, std::vector<Vertex>>& bins,
                  std::vector<Vertex>& flat) {
        for (const BspRenderBin& bin : bsp_bins) {
            const std::vector<Vertex>& verts = bin.vertices;
            if (verts.empty()) continue;
            if (use_materials()) {
                const int32_t mat = material_id(bin.material);
                auto& dst = bins[bin_key(mat, bin.lightmap_set)];
                dst.insert(dst.end(), verts.begin(), verts.end());
            } else {
                const size_t base = flat.size();
                flat.insert(flat.end(), verts.begin(), verts.end());
                for (size_t i = base; i < flat.size(); ++i) {
                    flat[i].color = 0xFFF2F0EEu;
                }
            }
        }
    }

    // Material mode: appends to `bins` (material id -> vertices). Legacy mode: appends palette
    // coloured vertices to `flat`. Returns the world AABB of the emitted vertices.
    AABB emit(const LevelActor& a, const StaticMeshAsset& sm, std::map<int32_t, std::vector<Vertex>>& bins,
              std::vector<Vertex>& flat) {
        const ActorTransform xf = ActorTransform::of(a);
        // Normals transform with the inverse transpose (R * S^-1); tangents with R * S.
        // A mirroring scale (negative determinant) flips the binormal handedness.
        auto safe_inv = [](float s) { return (std::abs(s) > 1e-12f) ? 1.0f / s : 0.0f; };
        const Vec3 inv_scale(safe_inv(xf.scale.x), safe_inv(xf.scale.y), safe_inv(xf.scale.z));
        const float det_sign = (xf.scale.x * xf.scale.y * xf.scale.z < 0.0f) ? -1.0f : 1.0f;
        const uint32_t color = legacy_palette_color(a, to_lower(a.mesh_name));
        AABB box(Vec3(1e30f, 1e30f, 1e30f), Vec3(-1e30f, -1e30f, -1e30f));

        // The component's baked lighting: a place in a set of light-map textures, or a sample per
        // mesh vertex. Without either the vertices say so (lm_u = -2).
        const ActorLightMap& lm = a.lightmap;
        static const bool show_unbaked = std::getenv("ME_SHOW_UNBAKED") != nullptr;
        int32_t lm_set = -1;
        uint32_t lm_scale[3] = {0u, 0u, 0u};
        bool lm_texture = false;
        if (lightmaps_ && lm.kind == ActorLightMap::Kind::Texture) {
            lm_set = lightmaps_->index_of(lm.package_path, lm.textures);
            for (int k = 0; k < 3; ++k) lm_scale[k] = pack_rgb9e5(lm.scale[k]);
            lm_texture = lm_set >= 0 && lm_set < 2047;
            if (!lm_texture) lm_set = -1;
        }
        const bool lm_vertex = lightmaps_ && lm.kind == ActorLightMap::Kind::Vertex &&
                               sm.source_vertex.size() == sm.triangles.size();

        // The section's material decides which two of the mesh's UV sets the vertex carries.
        MaterialUVSlots slots;
        auto emit_vertex = [&](std::vector<Vertex>& dst, uint32_t index, bool keep_vertex_color) {
            const Vertex& lv = sm.triangles[index];
            const Vec3 wp = xf.apply(lv.position);
            const Vec3 wn = xf.axis_x * (lv.normal.x * inv_scale.x) + xf.axis_y * (lv.normal.y * inv_scale.y) +
                            xf.axis_z * (lv.normal.z * inv_scale.z);
            const Vec3 wt = xf.axis_x * (lv.tangent.x * xf.scale.x) + xf.axis_y * (lv.tangent.y * xf.scale.y) +
                            xf.axis_z * (lv.tangent.z * xf.scale.z);
            Vertex wv = lv;
            if (!slots.is_default()) {
                sm.uv(index, slots.index[0], wv.u, wv.v);
                sm.uv(index, slots.index[1], wv.u2, wv.v2);
            }
            wv.position = wp;
            wv.normal = (wn.length_sq() > 1e-20f) ? wn.normalized() : Vec3(0.0f, 0.0f, 1.0f);
            wv.tangent = (wt.length_sq() > 1e-20f) ? wt.normalized() : Vec3(1.0f, 0.0f, 0.0f);
            wv.tangent_sign = lv.tangent_sign * det_sign;
            if (!keep_vertex_color) wv.color = color;
            if (lm_texture) {
                float lu = 0.0f, lv2 = 0.0f;
                sm.uv(index, sm.lightmap_uv_index, lu, lv2);
                wv.lm_u = std::max(0.0f, lu * lm.coord_scale[0] + lm.coord_bias[0]);
                wv.lm_v = lv2 * lm.coord_scale[1] + lm.coord_bias[1];
                wv.lm0 = lm_scale[0];
                wv.lm1 = lm_scale[1];
                wv.lm2 = lm_scale[2];
            } else if (lm_vertex) {
                const size_t sample = static_cast<size_t>(sm.source_vertex[index]) * 3;
                if (sample + 2 < lm.vertex_samples.size()) {
                    wv.lm_u = -1.0f;
                    wv.lm0 = lm.vertex_samples[sample];
                    wv.lm1 = lm.vertex_samples[sample + 1];
                    wv.lm2 = lm.vertex_samples[sample + 2];
                }
            } else if (lightmaps_) {
                // Level geometry nothing was baked for (the sky dome, the far skyline's cards) is lit
                // by nothing: every light of these levels is in the light maps, and the lights left
                // dynamic do not reach the static channels. It shows what it emits.
                wv.lm_u = -1.0f;
                if (show_unbaked) wv.lm0 = wv.lm1 = wv.lm2 = pack_rgb9e5(Vec3(3.0f, 0.0f, 3.0f));
            }
            dst.push_back(wv);
            box.expand(wp);
        };
        // Whole triangles; mirrored instances reverse the vertex order so world-space winding
        // stays consistent (UE3 flips the cull mode instead).
        auto emit_range = [&](std::vector<Vertex>& dst, uint32_t first, uint32_t count, bool keep_vertex_color) {
            const uint32_t end = std::min<uint32_t>(first + count, static_cast<uint32_t>(sm.triangles.size()));
            for (uint32_t i = first; i + 2 < end; i += 3) {
                emit_vertex(dst, i, keep_vertex_color);
                if (det_sign < 0.0f) {
                    emit_vertex(dst, i + 2, keep_vertex_color);
                    emit_vertex(dst, i + 1, keep_vertex_color);
                } else {
                    emit_vertex(dst, i + 1, keep_vertex_color);
                    emit_vertex(dst, i + 2, keep_vertex_color);
                }
            }
        };

        if (use_materials()) {
            // UStaticMeshComponent::GetMaterial(ElementIndex): component override, else the
            // element's material, else the engine default material ("" here).
            for (size_t e = 0; e < sm.elements.size(); ++e) {
                const auto& el = sm.elements[e];
                if (el.vertex_count == 0) continue;
                const bool overridden = e < a.material_overrides.size() && !a.material_overrides[e].empty();
                const std::string& mat_path = overridden ? a.material_overrides[e] : el.material;
                const int32_t mat = material_id(mat_path);
                slots = uv_slots(mat, mat_path);
                emit_range(bins[bin_key(mat, lm_set)], el.first_vertex, el.vertex_count, true);
            }
        } else {
            emit_range(flat, 0, static_cast<uint32_t>(sm.triangles.size()), false);
        }
        return box;
    }

    static void flush(MeshBuffer& mb, std::map<int32_t, std::vector<Vertex>>& bins) {
        size_t total = mb.vertices.size();
        for (const auto& [mat, verts] : bins) total += verts.size();
        mb.vertices.reserve(total);
        for (auto& [key, verts] : bins) {
            if (verts.empty()) continue;
            MeshSection s;
            s.first_vertex = static_cast<uint32_t>(mb.vertices.size());
            s.vertex_count = static_cast<uint32_t>(verts.size());
            s.material = key & 0xFFFFF;
            s.lightmap = (key >> 20) - 1;
            mb.vertices.insert(mb.vertices.end(), verts.begin(), verts.end());
            mb.sections.push_back(s);
            std::vector<Vertex>().swap(verts);
        }
        bins.clear();
    }

private:
    int32_t material_id(const std::string& path) {
        std::string key = to_lower(path);
        auto it = ids_.find(key);
        if (it != ids_.end()) return it->second;
        const int32_t id = static_cast<int32_t>(material_paths_->size());
        material_paths_->push_back(path);
        ids_.emplace(std::move(key), id);
        return id;
    }

    MaterialUVSlots uv_slots(int32_t material, const std::string& path) {
        if (!material_uvs_) return {};
        auto it = uv_slots_.find(material);
        if (it != uv_slots_.end()) return it->second;
        const MaterialUVSlots s = material_uvs_->slots(path);
        uv_slots_.emplace(material, s);
        return s;
    }

    std::vector<std::string>* material_paths_;
    MaterialUVResolver* material_uvs_;
    LightMapSets* lightmaps_;
    std::unordered_map<std::string, int32_t> ids_;
    std::unordered_map<int32_t, MaterialUVSlots> uv_slots_;
};

bool valid_box(const AABB& b) { return b.min_pt.x <= b.max_pt.x && b.min_pt.y <= b.max_pt.y && b.min_pt.z <= b.max_pt.z; }

} // namespace

void append_actor_collision(const LevelActor& a, int32_t actor_index, const StaticMeshAsset* sm, CollisionWorld& out) {
    uint8_t actor_channels = 0;
    if (a.is_collidable) actor_channels |= COLL_BlockNonZeroExtent;
    if (a.blocks_traces) actor_channels |= COLL_BlockZeroExtent;
    if (actor_channels == 0) return;

    // BlockingVolume brush hulls are already in world space.
    for (size_t i = 0; i + 2 < a.brush_triangles.size(); i += 3) {
        out.add_triangle(a.brush_triangles[i], a.brush_triangles[i + 1], a.brush_triangles[i + 2], actor_index,
                         actor_channels);
    }
    if (!sm) return;

    // UStaticMeshComponent::LineCheck (UDN CollisionTechnicalGuide, UStaticMesh::UseSimple*Collision):
    //  - extent checks (pawn movement): with UseSimpleBoxCollision only the collision hull
    //    (RB_BodySetup.AggGeom) is tested, so a mesh without a hull never blocks movement; without
    //    it, per-poly kDOP.
    //  - zero-extent traces: the hull when UseSimpleLineCollision is set and the mesh has one,
    //    per-poly kDOP otherwise.
    //  The owner's bCollideComplex forces per-poly collision for both.
    const bool use_hull_extent = sm->use_simple_box_collision && !a.collide_complex;
    const bool use_hull_zero = sm->use_simple_line_collision && sm->has_body_setup && !a.collide_complex;
    uint8_t simple_ch = 0;
    uint8_t complex_ch = 0;
    (use_hull_extent ? simple_ch : complex_ch) |= COLL_BlockNonZeroExtent;
    (use_hull_zero ? simple_ch : complex_ch) |= COLL_BlockZeroExtent;
    simple_ch &= actor_channels;
    complex_ch &= actor_channels;

    const ActorTransform xf = ActorTransform::of(a);
    auto emit = [&](const std::vector<Vec3>& tris, uint8_t channels) {
        if (channels == 0) return;
        for (size_t i = 0; i + 2 < tris.size(); i += 3) {
            out.add_triangle(xf.apply(tris[i]), xf.apply(tris[i + 1]), xf.apply(tris[i + 2]), actor_index, channels);
        }
    };
    emit(sm->simple_collision, simple_ch);
    emit(sm->complex_collision, complex_ch);
}

void build_level_geometry(std::vector<LevelActor>& actors,
                          std::vector<MeshBuffer>& out_meshes,
                          CollisionWorld& out_collision,
                          const std::unordered_map<std::string, StaticMeshAsset>& mesh_lib,
                          std::vector<std::string>* out_material_paths,
                          const std::vector<BspRenderBin>* bsp_render_bins,
                          const AABB* bsp_bounds,
                          MaterialUVResolver* material_uvs,
                          LightMapSets* lightmaps) {
    out_meshes.clear();

    // Batched world buffers for single-draw-call-per-material rendering
    MeshBuffer world_batch;
    world_batch.name = "UE3_Level_World_Geometry";
    MeshBuffer rv_batch;
    rv_batch.name = "UE3_Level_RunnerVision_Geometry";
    rv_batch.is_runner_vision = true;

    MeshEmitter emitter(out_material_paths, material_uvs, lightmaps);
    std::map<int32_t, std::vector<Vertex>> world_bins;
    std::map<int32_t, std::vector<Vertex>> rv_bins;

    AABB overall(Vec3(1e30f, 1e30f, 1e30f), Vec3(-1e30f, -1e30f, -1e30f));
    size_t placed_meshes = 0;
    size_t hidden_meshes = 0;
    size_t missing_meshes = 0;
    size_t blocking_volumes = 0;
    std::map<std::string, int> missing_names;

    for (size_t i = 0; i < actors.size(); ++i) {
        LevelActor& a = actors[i];
        if (std::abs(a.location.x) > 150000.0f || std::abs(a.location.y) > 150000.0f) continue;
        if (a.elevator >= 0 || a.barge_door >= 0) continue;  // moving InterpActor / door: built dynamically

        const StaticMeshAsset* sm = find_mesh(mesh_lib, a);
        if (!a.mesh_name.empty() && !sm) {
            ++missing_meshes;
            missing_names[to_lower(a.mesh_name)]++;
        }
        if (!a.brush_triangles.empty()) ++blocking_volumes;
        append_actor_collision(a, static_cast<int32_t>(i), sm, out_collision);
        if (!sm) continue;

        if (a.is_hidden || sm->triangles.empty()) {
            a.world_bounds = transformed_mesh_bounds(a, *sm);
            ++hidden_meshes;
            continue;
        }
        ++placed_meshes;
        const AABB box = emitter.emit(a, *sm, a.is_runner_vision ? rv_bins : world_bins,
                                      a.is_runner_vision ? rv_batch.vertices : world_batch.vertices);
        if (valid_box(box)) {
            a.world_bounds = box;
            overall.expand(box.min_pt);
            overall.expand(box.max_pt);
        }
    }

    if (bsp_render_bins && !bsp_render_bins->empty()) {
        emitter.emit_bsp(*bsp_render_bins, world_bins, world_batch.vertices);
        if (bsp_bounds && valid_box(*bsp_bounds)) {
            overall.expand(bsp_bounds->min_pt);
            overall.expand(bsp_bounds->max_pt);
        }
    }

    if (emitter.use_materials()) {
        MeshEmitter::flush(world_batch, world_bins);
        MeshEmitter::flush(rv_batch, rv_bins);
    }

    const size_t sections = world_batch.sections.size() + rv_batch.sections.size();
    std::cout << "[Level] " << placed_meshes << " static meshes placed, " << hidden_meshes << " hidden (collision only), "
              << missing_meshes << " missing (" << missing_names.size() << " unique), " << sections
              << " material sections; collision: " << out_collision.triangle_count() << " triangles ("
              << blocking_volumes << " BlockingVolumes)" << std::endl;
    if (std::getenv("ME_MATERIAL_VERBOSE") && !missing_names.empty()) {
        std::vector<std::pair<int, std::string>> top;
        for (const auto& [n, c] : missing_names) top.emplace_back(c, n);
        std::sort(top.rbegin(), top.rend());
        for (size_t i = 0; i < top.size() && i < 12; ++i) {
            std::cout << "[Level]   missing mesh '" << top[i].second << "' x" << top[i].first << std::endl;
        }
    }

    if (!world_batch.vertices.empty()) {
        world_batch.bounds = overall;
        out_meshes.push_back(std::move(world_batch));
    }
    if (!rv_batch.vertices.empty()) {
        rv_batch.bounds = overall;
        out_meshes.push_back(std::move(rv_batch));
    }
}

namespace {

// Assigns the real moving InterpActors of every elevator: the cab, actors hard-attached to it
// (Actor.Base == cab, e.g. the cab doors) and the landing door leaves of the door matinees.
// Duplicate elevators / InterpActors (the same lift cooked into both *_Slc and *_Spt) are merged.
void assign_elevator_parts(LevelScene& scene, const std::vector<InterpDoorInfo>& doors,
                           const std::unordered_map<std::string, StaticMeshAsset>& mesh_lib) {
    // 1. Dedupe elevators extracted from several streaming packages.
    std::vector<ElevatorInstance> unique;
    for (auto& e : scene.elevators) {
        bool dup = false;
        for (const auto& u : unique) {
            if ((u.start_pos - e.start_pos).length() < 16.0f && (u.end_pos - e.end_pos).length() < 16.0f) {
                dup = true;
                break;
            }
        }
        if (!dup) unique.push_back(std::move(e));
    }
    scene.elevators.swap(unique);

    auto& actors = scene.actors;
    std::unordered_map<std::string, int32_t> by_name;
    for (size_t i = 0; i < actors.size(); ++i) {
        by_name[actors[i].source_package + ":" + actors[i].unique_name] = static_cast<int32_t>(i);
    }
    std::unordered_map<std::string, const InterpDoorInfo*> door_by_name;
    for (const auto& d : doors) door_by_name.emplace(d.package + ":" + d.actor_name, &d);

    auto same_placement = [](const LevelActor& x, const LevelActor& y) {
        return to_lower(x.mesh_name) == to_lower(y.mesh_name) && (x.location - y.location).length() < 1.0f &&
               std::abs(x.rotation.pitch - y.rotation.pitch) < 1.0f && std::abs(x.rotation.yaw - y.rotation.yaw) < 1.0f &&
               std::abs(x.rotation.roll - y.rotation.roll) < 1.0f;
    };
    auto claim = [&](int32_t e, int32_t ai, ElevatorPartRole role, const Vec3& open) {
        if (actors[ai].elevator >= 0) return;
        actors[ai].elevator = e;
        ElevatorPart part;
        part.actor_name = actors[ai].unique_name;
        part.actor_index = ai;
        part.role = role;
        part.door_open_offset = open;
        scene.elevators[e].parts.push_back(std::move(part));
        // The same InterpActor cooked into another streaming package moves with this one.
        for (size_t j = 0; j < actors.size(); ++j) {
            if (static_cast<int32_t>(j) == ai || actors[j].elevator >= 0) continue;
            if (actors[j].class_name == actors[ai].class_name && same_placement(actors[j], actors[ai])) {
                actors[j].elevator = e;
            }
        }
    };

    for (size_t ei = 0; ei < scene.elevators.size(); ++ei) {
        const int32_t e = static_cast<int32_t>(ei);
        const ElevatorInstance& el = scene.elevators[ei];
        auto cab_it = by_name.find(el.source_package + ":" + el.cab_actor_name);
        if (el.cab_actor_name.empty() || cab_it == by_name.end()) continue;
        claim(e, cab_it->second, ElevatorPartRole::Cab, Vec3(0.0f, 0.0f, 0.0f));

        for (size_t i = 0; i < actors.size(); ++i) {
            const LevelActor& a = actors[i];
            if (a.base_name != el.cab_actor_name || a.source_package != el.source_package) continue;
            auto d = door_by_name.find(a.source_package + ":" + a.unique_name);
            if (d != door_by_name.end()) {
                claim(e, static_cast<int32_t>(i), ElevatorPartRole::CabDoor, d->second->open_offset);
            } else {
                claim(e, static_cast<int32_t>(i), ElevatorPartRole::CabAttached, Vec3(0.0f, 0.0f, 0.0f));
            }
        }

        // Landing doors: door-matinee leaves at the shaft's start / destination floor.
        const LevelActor& cab = actors[cab_it->second];
        float floor_offset = 0.0f;
        float cab_height = 262.0f;
        if (const StaticMeshAsset* sm = find_mesh(mesh_lib, cab)) {
            floor_offset = sm->bounds_origin.z - sm->bounds_extent.z;
            cab_height = 2.0f * sm->bounds_extent.z;
        }
        // The landing leaves sit in the shaft wall directly in front of the cab doorway, i.e.
        // inside the cab's footprint (+ a few units of door frame). Neighbouring shafts' doors
        // (Escape_Off has a second, unused shaft 320 units over) must stay put.
        const Vec3 center = el.start_pos + el.cab_local_offset;
        constexpr float kFootprintMargin = 64.0f;
        for (const auto& d : doors) {
            auto it = by_name.find(d.package + ":" + d.actor_name);
            if (it == by_name.end()) continue;
            const LevelActor& a = actors[it->second];
            if (a.elevator >= 0 || !a.base_name.empty()) continue;
            const float dx = a.location.x - center.x;
            const float dy = a.location.y - center.y;
            if (std::abs(dx) > el.cab_half_extents.x + kFootprintMargin ||
                std::abs(dy) > el.cab_half_extents.y + kFootprintMargin) {
                continue;
            }
            const float dz_start = a.location.z - (el.start_pos.z + floor_offset);
            const float dz_end = a.location.z - (el.end_pos.z + floor_offset);
            if (dz_start > -120.0f && dz_start < cab_height) {
                claim(e, it->second, ElevatorPartRole::StartDoor, d.open_offset);
            } else if (dz_end > -120.0f && dz_end < cab_height) {
                claim(e, it->second, ElevatorPartRole::EndDoor, d.open_offset);
            }
        }
    }
}

// Builds the per-part render buffer (appended to scene.meshes) and collision of every elevator part.
void build_elevator_part_geometry(LevelScene& scene, const std::unordered_map<std::string, StaticMeshAsset>& mesh_lib,
                                  std::vector<std::string>* material_paths, MaterialUVResolver* material_uvs = nullptr) {
    MeshEmitter emitter(material_paths, material_uvs);
    for (size_t e = 0; e < scene.elevators.size(); ++e) {
        auto& el = scene.elevators[e];
        for (size_t p = 0; p < el.parts.size(); ++p) {
            ElevatorPart& part = el.parts[p];
            // Pose for the elevator's initial state (IdleStart: cab at the start floor, start-floor
            // doors open), exactly as ParkourController::update_elevators() poses it.
            const Vec3 cab_offset = el.current_pos - el.start_pos;
            switch (part.role) {
                case ElevatorPartRole::Cab:
                case ElevatorPartRole::CabAttached: part.offset = cab_offset; break;
                case ElevatorPartRole::CabDoor: part.offset = cab_offset + part.door_open_offset * el.door_open_Start; break;
                case ElevatorPartRole::StartDoor: part.offset = part.door_open_offset * el.door_open_Start; break;
                case ElevatorPartRole::EndDoor: part.offset = part.door_open_offset * el.door_open_End; break;
            }
            part.prev_offset = part.offset;
            LevelActor& a = scene.actors[part.actor_index];
            const StaticMeshAsset* sm = find_mesh(mesh_lib, a);
            auto cw = std::make_shared<CollisionWorld>();
            append_actor_collision(a, part.actor_index, sm, *cw);
            cw->build();
            if (!cw->empty()) part.collision = std::move(cw);
            if (!sm) continue;
            a.world_bounds = transformed_mesh_bounds(a, *sm);
            if (a.is_hidden || sm->triangles.empty()) continue;

            MeshBuffer mb;
            mb.name = "UE3_Elevator_" + a.source_package + "_" + a.unique_name;
            mb.is_runner_vision = a.is_runner_vision;
            mb.elevator = static_cast<int32_t>(e);
            mb.elevator_part = static_cast<int32_t>(p);
            std::map<int32_t, std::vector<Vertex>> bins;
            const AABB box = emitter.emit(a, *sm, bins, mb.vertices);
            if (emitter.use_materials()) MeshEmitter::flush(mb, bins);
            if (mb.vertices.empty()) continue;
            mb.bounds = box;
            a.world_bounds = box;
            part.mesh_index = static_cast<int32_t>(scene.meshes.size());
            scene.meshes.push_back(std::move(mb));
        }
    }
}

void assign_barge_doors(LevelScene& scene) {
    auto& actors = scene.actors;
    auto is_hinged_door_leaf = [](const LevelActor& a) -> bool {
        if (a.elevator >= 0 || a.is_elevator_part) return false;
        const std::string low_mesh = to_lower(a.mesh_name);
        const std::string low_obj = to_lower(a.object_name);
        const std::string low_cls = to_lower(a.class_name);
        // Only interactive InterpActor doors can swing open in Mirror's Edge; StaticMeshActor doors
        // and all door frames / combined wall-doors are static blocking world geometry.
        if (low_cls != "interpactor") return false;
        if (low_mesh.find("elevatordoor") != std::string::npos ||
            low_mesh.find("doorlocked") != std::string::npos ||
            low_mesh.find("doorstorefront") != std::string::npos ||
            low_mesh.find("doorrollup") != std::string::npos ||
            low_mesh.find("frame") != std::string::npos ||
            low_mesh.find("combined") != std::string::npos ||
            low_mesh.find("policecar") != std::string::npos ||
            low_mesh.find("doorclosingmech") != std::string::npos) {
            return false;
        }
        if (low_mesh.find("barge") != std::string::npos || low_obj.find("barge") != std::string::npos) return true;
        if (low_mesh.find("officedoorglass") != std::string::npos || low_obj.find("officedoorglass") != std::string::npos) return true;
        if (low_mesh.find("onewaydoor") != std::string::npos || low_obj.find("onewaydoor") != std::string::npos) return true;
        if (a.is_runner_vision && low_mesh.find("door") != std::string::npos) return true;
        return false;
    };

    for (size_t i = 0; i < actors.size(); ++i) {
        LevelActor& a = actors[i];
        if (!is_hinged_door_leaf(a) || a.barge_door >= 0) continue;

        // Deduplicate identical hinged doors across streaming packages
        int32_t d_idx = -1;
        for (size_t k = 0; k < scene.barge_doors.size(); ++k) {
            if ((scene.barge_doors[k].hinge_pos - a.location).length() < 8.0f) {
                d_idx = static_cast<int32_t>(k);
                break;
            }
        }
        if (d_idx < 0) {
            BargeDoorInstance d;
            d.name = a.source_package + ":" + a.unique_name;
            d.source_package = a.source_package;
            d.hinge_pos = a.location;
            d.center_pos = a.location;
            d_idx = static_cast<int32_t>(scene.barge_doors.size());
            scene.barge_doors.push_back(std::move(d));
        }

        if (to_lower(a.mesh_name).find("barge") != std::string::npos) {
            a.is_runner_vision = true;
        }
        a.barge_door = d_idx;
        DoorPart part;
        part.actor_name = a.unique_name;
        part.actor_index = static_cast<int32_t>(i);
        part.is_blocker_only = false;
        scene.barge_doors[d_idx].parts.push_back(std::move(part));
    }

    // Attach hard-attached closer hardware (S_DoorClosingMech_01) and hidden doorway trigger slabs (S_DoorClosingMech_02)
    for (size_t i = 0; i < actors.size(); ++i) {
        LevelActor& a = actors[i];
        if (a.elevator >= 0 || a.barge_door >= 0) continue;
        const std::string low_mesh = to_lower(a.mesh_name);
        const bool is_closer_bar = (low_mesh.find("doorclosingmech_01") != std::string::npos);
        const bool is_blocker_slab = (low_mesh.find("doorclosingmech_02") != std::string::npos);
        // S_DoorClosingMech_01 has bIgnoreBaseRotation=True on its InterpActor component: it stays bolted to the static lintel.
        if (is_closer_bar) continue;
        if (!is_blocker_slab && a.base_name.empty()) continue;

        for (size_t d = 0; d < scene.barge_doors.size(); ++d) {
            BargeDoorInstance& door = scene.barge_doors[d];
            bool match = false;
            if (!a.base_name.empty()) {
                for (const auto& p : door.parts) {
                    if (!p.is_blocker_only && p.actor_name == a.base_name &&
                        scene.actors[p.actor_index].source_package == a.source_package) {
                        match = true;
                        break;
                    }
                }
            }
            if (!match && is_blocker_slab) {
                const float dx = a.location.x - door.hinge_pos.x;
                const float dy = a.location.y - door.hinge_pos.y;
                const float dz = std::abs(a.location.z - door.hinge_pos.z);
                if (std::sqrt(dx * dx + dy * dy) < 200.0f && dz < 320.0f) {
                    match = true;
                }
            }
            if (match) {
                a.barge_door = static_cast<int32_t>(d);
                DoorPart part;
                part.actor_name = a.unique_name;
                part.actor_index = static_cast<int32_t>(i);
                part.is_blocker_only = is_blocker_slab || a.is_hidden;
                door.parts.push_back(std::move(part));
                break;
            }
        }
    }
}

void build_barge_door_geometry(LevelScene& scene, const std::unordered_map<std::string, StaticMeshAsset>& mesh_lib,
                               std::vector<std::string>* material_paths, MaterialUVResolver* material_uvs = nullptr) {
    MeshEmitter emitter(material_paths, material_uvs);
    for (size_t d = 0; d < scene.barge_doors.size(); ++d) {
        BargeDoorInstance& door = scene.barge_doors[d];
        AABB leaf_bounds(Vec3(1e30f, 1e30f, 1e30f), Vec3(-1e30f, -1e30f, -1e30f));
        bool has_leaf_bounds = false;

        for (size_t p = 0; p < door.parts.size(); ++p) {
            DoorPart& part = door.parts[p];
            LevelActor& a = scene.actors[part.actor_index];
            const StaticMeshAsset* sm = find_mesh(mesh_lib, a);
            auto cw = std::make_shared<CollisionWorld>();
            append_actor_collision(a, part.actor_index, sm, *cw);
            cw->build();
            if (!cw->empty()) part.collision = std::move(cw);
            if (!sm) continue;

            AABB box = transformed_mesh_bounds(a, *sm);
            a.world_bounds = box;
            if (!part.is_blocker_only && valid_box(box)) {
                leaf_bounds.expand(box.min_pt);
                leaf_bounds.expand(box.max_pt);
                has_leaf_bounds = true;
            }

            if (a.is_hidden || sm->triangles.empty()) continue;

            MeshBuffer mb;
            mb.name = "UE3_Door_" + a.source_package + "_" + a.unique_name;
            mb.is_runner_vision = a.is_runner_vision;
            mb.barge_door = static_cast<int32_t>(d);
            std::map<int32_t, std::vector<Vertex>> bins;
            const AABB emitted_box = emitter.emit(a, *sm, bins, mb.vertices);
            if (emitter.use_materials()) MeshEmitter::flush(mb, bins);
            if (mb.vertices.empty()) continue;
            mb.bounds = emitted_box;
            a.world_bounds = emitted_box;
            part.mesh_index = static_cast<int32_t>(scene.meshes.size());
            scene.meshes.push_back(std::move(mb));
        }

        if (has_leaf_bounds) {
            door.center_pos = leaf_bounds.center();
            leaf_bounds.min_pt -= Vec3(24.0f, 24.0f, 16.0f);
            leaf_bounds.max_pt += Vec3(24.0f, 24.0f, 16.0f);
            door.closed_bounds = leaf_bounds;
        } else {
            door.center_pos = door.hinge_pos + Vec3(0.0f, 0.0f, 100.0f);
            door.closed_bounds = AABB(door.hinge_pos - Vec3(90.0f, 90.0f, 20.0f),
                                      door.hinge_pos + Vec3(90.0f, 90.0f, 240.0f));
        }
    }
}

} // namespace

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
    Vec3 mod_shadow_color{0.494f, 0.659f, 0.875f}; // DirectionalLightComponent.ModShadowColor (sRGB 0..1)
    std::string source;
};

// Reverse-engineered ambient lighting state from WorldInfo, SkyLightComponent, and HeightFogComponent
// (FSkyLightSceneProxy::FSkyLightSceneProxy @ VA 0x00ED4100, FLightSceneInfo @ VA 0x0103F0D0).
struct LevelAmbient {
    bool has_skylight = false;
    float sky_score = -1e30f;
    Vec3 sky_light_color{0.722f, 0.835f, 1.0f};      // SkyLightComponent.LightColor (sRGB 0..1)
    float sky_brightness = 0.4f;                     // SkyLightComponent.Brightness
    Vec3 sky_lower_color{0.737f, 0.627f, 0.447f};    // SkyLightComponent.LowerColor (sRGB 0..1)
    float sky_lower_brightness = 0.45f;              // SkyLightComponent.LowerBrightness
    std::string sky_source;

    bool has_world_sky = false;
    Vec3 world_sky_color{0.30f, 0.52f, 0.65f};       // WorldInfo.SkyColor (Beast environment sky radiosity)
    float ibl_intensity = 1.0f;                      // WorldInfo.IBLIntensity
    Vec3 haze_color{0.76f, 0.86f, 0.96f};            // DefaultPostProcessSettings.HazeColor / HeightFog LightColor
    std::string world_source;
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

        // ModShadowColor: DICE's azure shadow fill color on DirectionalLightComponent
        Vec3 mod_shadow(0.494f, 0.659f, 0.875f);
        if (const UProperty* ms = find_prop(comp_props, "ModShadowColor")) {
            mod_shadow = Vec3(ms->v[0], ms->v[1], ms->v[2]);
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
        best.mod_shadow_color = mod_shadow;
        best.source = package_name_of(pkg) + "." + export_object_name(pkg, idx);
    }
}

static Vec3 read_color_or_vec3(const UProperty* p, const Vec3& def) {
    if (!p) return def;
    if (p->immutable_struct) {
        return Vec3(p->v[0], p->v[1], p->v[2]);
    }
    if (!p->fields.empty()) {
        if (find_prop(p->fields, "R") || find_prop(p->fields, "G") || find_prop(p->fields, "B")) {
            float r = prop_float(p->fields, "R", def.x);
            float g = prop_float(p->fields, "G", def.y);
            float b = prop_float(p->fields, "B", def.z);
            if (p->struct_name == "Color" && (r > 1.0f || g > 1.0f || b > 1.0f)) {
                return Vec3(r / 255.0f, g / 255.0f, b / 255.0f);
            }
            return Vec3(r, g, b);
        }
        return Vec3(prop_float(p->fields, "X", def.x),
                    prop_float(p->fields, "Y", def.y),
                    prop_float(p->fields, "Z", def.z));
    }
    return def;
}

static void scan_level_ambient(const UPKPackage& pkg, LevelAmbient& amb) {
    const auto& exports = pkg.get_exports();
    for (size_t i = 0; i < exports.size(); ++i) {
        const int32_t idx = static_cast<int32_t>(i) + 1;
        const std::string cls = object_class_name(pkg, idx);

        if (cls == "WorldInfo") {
            // Lighting artists often set WorldInfo.SkyColor in the *_Lgts package; prefer any non-default override.
            UPropertyList wi_props;
            parse_export_properties(pkg, idx, wi_props);
            if (const UProperty* sc = find_prop(wi_props, "SkyColor")) {
                Vec3 c = read_color_or_vec3(sc, amb.world_sky_color);
                const bool is_default_sky = (std::abs(c.x - 0.3f) < 0.01f && std::abs(c.y - 0.7f) < 0.01f && std::abs(c.z - 1.0f) < 0.01f);
                if (!amb.has_world_sky || !is_default_sky) {
                    amb.has_world_sky = true;
                    amb.world_sky_color = c;
                    amb.world_source = package_name_of(pkg) + "." + export_object_name(pkg, idx);
                }
            }
            if (find_prop(wi_props, "IBLIntensity")) {
                amb.ibl_intensity = prop_float(wi_props, "IBLIntensity", amb.ibl_intensity);
                if (amb.world_source.empty()) {
                    amb.has_world_sky = true;
                    amb.world_source = package_name_of(pkg) + "." + export_object_name(pkg, idx);
                }
            }
            if (const UProperty* pp = find_prop(wi_props, "DefaultPostProcessSettings")) {
                if (const UProperty* hc = find_prop(pp->fields, "HazeColor")) {
                    amb.haze_color = read_color_or_vec3(hc, amb.haze_color);
                }
            }
        } else if (cls == "SkyLight" || cls == "SkyLightToggleable") {
            UPropertyList actor_props;
            parse_export_properties(pkg, idx, actor_props);
            const int32_t comp_idx = prop_object(actor_props, "LightComponent");
            if (comp_idx <= 0) continue;
            UPropertyList comp_props;
            parse_export_properties(pkg, comp_idx, comp_props);

            const bool enabled = prop_bool(comp_props, "bEnabled", true) && prop_bool(actor_props, "bEnabled", true);
            if (!enabled) continue;

            bool affects_static = true;
            bool affects_dynamic = true;
            bool special_channel_only = false;
            if (const UProperty* lc = find_prop(comp_props, "LightingChannels")) {
                affects_static = prop_bool(lc->fields, "Static", true);
                affects_dynamic = prop_bool(lc->fields, "Dynamic", true);
                for (const auto& f : lc->fields) {
                    if ((f.name.find("Cinematic") != std::string::npos ||
                         f.name.find("Gameplay") != std::string::npos) && f.b) {
                        special_channel_only = true;
                    }
                }
                if (!affects_dynamic) special_channel_only = true;
            }
            const bool baked = prop_bool(comp_props, "bHasLightEverBeenBuiltIntoLightMap", false);
            const float brightness = prop_float(comp_props, "Brightness", 1.0f);
            const float lower_brightness = prop_float(comp_props, "LowerBrightness", 0.5f);

            // USkyLightComponent archetype defaults in Engine.u:
            // LightColor=(R=184,G=213,B=255), Brightness=1.0, LowerColor=(R=188,G=160,B=114), LowerBrightness=0.0
            Vec3 upper_col(184.0f / 255.0f, 213.0f / 255.0f, 1.0f);
            Vec3 lower_col(188.0f / 255.0f, 160.0f / 255.0f, 114.0f / 255.0f);
            if (const UProperty* uc = find_prop(comp_props, "LightColor")) {
                upper_col = read_color_or_vec3(uc, upper_col);
            }
            if (const UProperty* lc = find_prop(comp_props, "LowerColor")) {
                lower_col = read_color_or_vec3(lc, lower_col);
            }

            // Prefer the world SkyLight (Static + Dynamic channels) over Cinematic/Gameplay-only channel lights.
            const float score = (baked ? 1000.0f : 0.0f) + (affects_static ? 200.0f : 0.0f) +
                                (affects_dynamic ? 200.0f : 0.0f) - (special_channel_only ? 2000.0f : 0.0f) +
                                brightness + lower_brightness;
            if (amb.has_skylight && score <= amb.sky_score) continue;

            amb.has_skylight = true;
            amb.sky_score = score;
            amb.sky_light_color = upper_col;
            amb.sky_brightness = brightness;
            amb.sky_lower_color = lower_col;
            amb.sky_lower_brightness = lower_brightness;
            amb.sky_source = package_name_of(pkg) + "." + export_object_name(pkg, idx);
        } else if (cls == "HeightFogComponent") {
            UPropertyList fog_props;
            parse_export_properties(pkg, idx, fog_props);
            if (prop_bool(fog_props, "bEnabled", true)) {
                if (const UProperty* fc = find_prop(fog_props, "LightColor")) {
                    amb.haze_color = read_color_or_vec3(fc, amb.haze_color);
                }
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Real-Time Planar Reflection Capture & Volume Extractor
// (Reverse-engineered from SceneCaptureReflectActor / SceneCaptureReflectComponent
// in UnSceneCapture.cpp @ VA 0x00f99390..0x00f9d312 and TdReflectionVolume @ VA 0x00f9bdde..0x00f9bed9)
// -----------------------------------------------------------------------------
void UPKPackage::extract_reflections(std::vector<SceneCaptureReflectInfo>& out_captures,
                                     std::vector<ReflectionVolumeInfo>& out_volumes) const {
    const std::string pkg_stem = package_name_of(*this);
    for (size_t i = 0; i < exports_.size(); ++i) {
        const int32_t idx = static_cast<int32_t>(i) + 1;
        const std::string cls = object_class_name(*this, idx);
        if (cls == "SceneCaptureReflectActor") {
            UPropertyList actor_props;
            parse_export_properties(*this, idx, actor_props);

            SceneCaptureReflectInfo cap;
            cap.actor_name = pkg_stem + "." + export_object_name(*this, idx);

            if (const UProperty* loc = find_prop(actor_props, "Location")) {
                cap.location = Vec3(loc->v[0], loc->v[1], loc->v[2]);
            }
            // SceneCaptureReflectActor default archetype rotation is Pitch=16384 (+90 deg -> Normal=(0,0,1))
            int32_t pitch = 16384, yaw = 0, roll = 0;
            if (const UProperty* rot = find_prop(actor_props, "Rotation")) {
                pitch = rot->vi[0];
                yaw = rot->vi[1];
                roll = rot->vi[2];
            }
            cap.rotation = Rotator(static_cast<float>(pitch), static_cast<float>(yaw), static_cast<float>(roll));

            // USceneCaptureReflectComponent::UpdateTransform (VA 0x00f99390):
            // MirrorNormal = FRotator(Owner->Rotation).Vector(), MirrorPlane = FPlane(Owner->Location, MirrorNormal)
            const float kUnit = 3.14159265358979f / 32768.0f;
            const float p = static_cast<float>(pitch) * kUnit;
            const float y = static_cast<float>(yaw) * kUnit;
            cap.mirror_normal = Vec3(std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), std::sin(p)).normalized();
            cap.plane_w = cap.location.dot(cap.mirror_normal);

            const int32_t vol_ref = prop_object(actor_props, "ReflectionVolume");
            if (vol_ref != 0) {
                cap.reflection_volume = object_canonical_path(*this, vol_ref);
            }

            const int32_t comp_ref = prop_object(actor_props, "SceneCapture");
            if (comp_ref > 0) {
                cap.component_name = export_object_name(*this, comp_ref);
                UPropertyList comp_props;
                parse_export_properties(*this, comp_ref, comp_props);
                const int32_t rt_ref = prop_object(comp_props, "TextureTarget");
                if (rt_ref != 0) {
                    cap.texture_target = object_canonical_path(*this, rt_ref);
                }
                cap.scale_fov = prop_float(comp_props, "ScaleFOV", 1.0f);
                cap.near_plane = prop_float(comp_props, "NearPlane", 20.0f);
                cap.far_plane = prop_float(comp_props, "FarPlane", 500.0f);
                cap.far_culling_distance = prop_float(comp_props, "FarCullingDistance", 0.0f);
                cap.max_update_dist = prop_float(comp_props, "MaxUpdateDist", 0.0f);
                cap.max_streaming_update_dist = prop_float(comp_props, "MaxStreamingUpdateDist", 0.0f);
                cap.frame_rate = prop_float(comp_props, "FrameRate", 1000.0f);
            }
            out_captures.push_back(std::move(cap));
        } else if (cls == "TdReflectionVolume") {
            UPropertyList vol_props;
            parse_export_properties(*this, idx, vol_props);

            ReflectionVolumeInfo vol;
            vol.object_name = pkg_stem + "." + export_object_name(*this, idx);
            if (const UProperty* loc = find_prop(vol_props, "Location")) {
                vol.location = Vec3(loc->v[0], loc->v[1], loc->v[2]);
            }
            if (const UProperty* rot = find_prop(vol_props, "Rotation")) {
                vol.rotation = Rotator(static_cast<float>(rot->vi[0]),
                                       static_cast<float>(rot->vi[1]),
                                       static_cast<float>(rot->vi[2]));
            }
            vol.enabled = prop_bool(vol_props, "bEnabled", true);
            out_volumes.push_back(std::move(vol));
        }
    }
}

void UPKPackage::extract_helicopter_encounters(std::vector<HeliAttackNode>& out_nodes,
                                               std::vector<HelicopterInstance>& out_helicopters,
                                               std::vector<DummyFireBarrage>& out_barrages,
                                               std::vector<LevelScene::KismetValveProp>* out_valves,
                                               std::vector<LevelScene::KismetLookAtPoint>* out_lookats,
                                               std::vector<LevelScene::KismetLevelTransition>* out_transitions,
                                               std::vector<EnemyBot>* out_enemies) const {
    if (!valid_) return;
    std::string pkg_stem = std::filesystem::path(file_path_).stem().string();

    auto resolve_actor_location = [&](int32_t obj_idx, Vec3& out_loc) -> bool {
        if (obj_idx <= 0 || static_cast<size_t>(obj_idx) > exports_.size()) return false;
        UPropertyList props;
        parse_export_properties(*this, obj_idx, props);
        if (const UProperty* loc = find_prop(props, "Location")) {
            out_loc = Vec3(loc->v[0], loc->v[1], loc->v[2]);
            return true;
        }
        int32_t inner = prop_object(props, "ObjValue");
        if (inner > 0 && static_cast<size_t>(inner) <= exports_.size()) {
            UPropertyList inner_props;
            parse_export_properties(*this, inner, inner_props);
            if (const UProperty* loc = find_prop(inner_props, "Location")) {
                out_loc = Vec3(loc->v[0], loc->v[1], loc->v[2]);
                return true;
            }
        }
        return false;
    };

    auto parse_side_enum = [](const std::string& s) -> EHeliAttackSide {
        if (s == "ESide_Right") return EHeliAttackSide::Right;
        if (s == "ESide_Left") return EHeliAttackSide::Left;
        if (s == "ESide_UseLeftWhenHovering") return EHeliAttackSide::UseLeftWhenHovering;
        if (s == "ESide_UseRightWhenHovering") return EHeliAttackSide::UseRightWhenHovering;
        if (s == "ESide_Both") return EHeliAttackSide::Both;
        if (s == "ESide_None") return EHeliAttackSide::None;
        return EHeliAttackSide::UseLeftWhenHovering;
    };

    // Map kismet node index -> earliest trigger actor world location that activates it (up to 3 hops)
    std::unordered_map<int32_t, Vec3> node_trigger_pos;
    for (size_t i = 0; i < exports_.size(); ++i) {
        int32_t idx = static_cast<int32_t>(i + 1);
        std::string cls = get_export_class(exports_[i]);
        if (cls != "SeqEvent_TdTouch" && cls != "SeqEvent_Touch") continue;
        UPropertyList ev_props;
        parse_export_properties(*this, idx, ev_props);
        int32_t orig_ref = prop_object(ev_props, "Originator");
        Vec3 trig_loc{};
        if (!resolve_actor_location(orig_ref, trig_loc)) continue;

        std::vector<int32_t> frontier = {idx};
        for (int hop = 0; hop < 3 && !frontier.empty(); ++hop) {
            std::vector<int32_t> next_frontier;
            for (int32_t cur : frontier) {
                UPropertyList cprops;
                parse_export_properties(*this, cur, cprops);
                const UProperty* outs = find_prop(cprops, "OutputLinks");
                if (!outs) continue;
                for (const auto& out_el : outs->elements) {
                    const UProperty* links = find_prop(out_el, "Links");
                    if (!links) continue;
                    for (const auto& l_el : links->elements) {
                        int32_t target_op = prop_object(l_el, "LinkedOp");
                        if (target_op > 0 && static_cast<size_t>(target_op) <= exports_.size()) {
                            if (node_trigger_pos.find(target_op) == node_trigger_pos.end()) {
                                node_trigger_pos[target_op] = trig_loc;
                                next_frontier.push_back(target_op);
                            }
                        }
                    }
                }
            }
            frontier = std::move(next_frontier);
        }
    }

    for (size_t i = 0; i < exports_.size(); ++i) {
        int32_t idx = static_cast<int32_t>(i + 1);
        const auto& exp = exports_[i];
        std::string cls = get_export_class(exp);

        if (cls == "TdAttackPathNode") {
            UPropertyList props;
            parse_export_properties(*this, idx, props);
            HeliAttackNode node{};
            node.object_name = pkg_stem + "." + export_object_name(*this, idx);
            if (const UProperty* loc = find_prop(props, "Location")) {
                node.location = Vec3(loc->v[0], loc->v[1], loc->v[2]);
            }
            if (const UProperty* rot = find_prop(props, "Rotation")) {
                node.yaw_deg = static_cast<float>(rot->vi[1]) * (360.0f / 65536.0f);
            }
            node.attack_radius = prop_float(props, "AttackVolumeRadius", 3000.0f);
            node.attack_height = prop_float(props, "AttackVolumeHeight", 2000.0f);
            node.attack_angle = prop_float(props, "AttackVolumeAngle", 45.0f);
            node.exposure = prop_int(props, "Exposure", 80);
            out_nodes.push_back(std::move(node));
        } else if (cls == "SeqAct_TdHelicopterFactory") {
            UPropertyList props;
            parse_export_properties(*this, idx, props);
            HelicopterInstance heli{};
            heli.object_name = pkg_stem + "." + export_object_name(*this, idx);

            if (const UProperty* vlinks = find_prop(props, "VariableLinks")) {
                for (const auto& vl : vlinks->elements) {
                    const UProperty* desc_p = find_prop(vl, "LinkDesc");
                    const UProperty* lvars = find_prop(vl, "LinkedVariables");
                    std::string desc = desc_p ? desc_p->s : "";
                    if (!lvars) continue;
                    if (desc == "Spawn Point" && !lvars->ints.empty()) {
                        Vec3 sp{};
                        if (resolve_actor_location(lvars->ints[0], sp)) {
                            heli.spawn_pos = sp;
                            heli.position = sp;
                        }
                    } else if (desc == "Crew") {
                        heli.gunner_count = std::max<int32_t>(1, static_cast<int32_t>(lvars->ints.size()));
                    }
                }
            }

            if (const UProperty* olinks = find_prop(props, "OutputLinks")) {
                for (const auto& ol : olinks->elements) {
                    const UProperty* links = find_prop(ol, "Links");
                    if (!links) continue;
                    for (const auto& l_el : links->elements) {
                        int32_t top = prop_object(l_el, "LinkedOp");
                        if (top <= 0 || static_cast<size_t>(top) > exports_.size()) continue;
                        std::string tcls = get_export_class(exports_[static_cast<size_t>(top - 1)]);
                        UPropertyList tprops;
                        parse_export_properties(*this, top, tprops);
                        if (tcls == "SeqAct_SetHeliTarget") {
                            heli.side_preference = parse_side_enum(prop_name(tprops, "SideOfHelicopter"));
                        } else if (tcls == "SeqAct_Delay") {
                            heli.hold_fire_delay = std::max(3.5f, prop_float(tprops, "Duration", 6.0f));
                        }
                    }
                }
            }

            auto trig_it = node_trigger_pos.find(idx);
            if (trig_it != node_trigger_pos.end()) {
                heli.trigger_pos = trig_it->second;
                heli.trigger_radius = 520.0f;
            } else {
                heli.trigger_pos = heli.spawn_pos;
                heli.trigger_radius = 1800.0f;
            }
            heli.retreat_dest = heli.spawn_pos + Vec3(0.0f, 0.0f, 4500.0f);
            out_helicopters.push_back(std::move(heli));
        } else if (cls == "SeqAct_TdDummyWeaponFire") {
            UPropertyList props;
            parse_export_properties(*this, idx, props);
            DummyFireBarrage bar{};
            bar.object_name = pkg_stem + "." + export_object_name(*this, idx);
            bar.shots_to_fire = std::clamp(prop_int(props, "ShotsToFire", 14), 1, 64);
            if (const UProperty* spread = find_prop(props, "MaxSpread")) {
                bar.spread_deg = std::max(std::abs(spread->vi[0]), std::abs(spread->vi[1])) * (360.0f / 65536.0f);
                if (bar.spread_deg <= 0.1f) bar.spread_deg = 8.0f;
            }
            bool got_origin = false;
            bool got_target = false;
            if (const UProperty* vlinks = find_prop(props, "VariableLinks")) {
                for (const auto& vl : vlinks->elements) {
                    const UProperty* desc_p = find_prop(vl, "LinkDesc");
                    const UProperty* lvars = find_prop(vl, "LinkedVariables");
                    std::string desc = desc_p ? desc_p->s : "";
                    if (!lvars || lvars->ints.empty()) continue;
                    int32_t var_ref = lvars->ints[0];
                    if (desc == "Origin") {
                        got_origin = resolve_actor_location(var_ref, bar.origin);
                    } else if (desc == "Target") {
                        got_target = resolve_actor_location(var_ref, bar.target);
                    }
                }
            }
            if (got_origin && got_target) {
                auto trig_it = node_trigger_pos.find(idx);
                if (trig_it != node_trigger_pos.end()) {
                    bar.trigger_pos = trig_it->second;
                    bar.trigger_radius = 1800.0f;
                } else {
                    bar.trigger_pos = (bar.origin + bar.target) * 0.5f;
                    bar.trigger_radius = 1800.0f;
                }
                out_barrages.push_back(std::move(bar));
            }
        } else if (out_valves && cls == "TdValveSkeletalMeshActor") {
            UPropertyList props;
            parse_export_properties(*this, idx, props);
            LevelScene::KismetValveProp valve{};
            valve.object_name = pkg_stem + "." + export_object_name(*this, idx);
            if (const UProperty* loc = find_prop(props, "Location")) {
                valve.position = Vec3(loc->v[0], loc->v[1], loc->v[2]);
            }
            if (const UProperty* rot = find_prop(props, "Rotation")) {
                valve.yaw_deg = static_cast<float>(rot->vi[1]) * (360.0f / 65536.0f);
            }
            out_valves->push_back(std::move(valve));
        } else if (out_lookats && (cls == "TdLookAtPoint" || cls == "TdLookAtPointSpawnable")) {
            UPropertyList props;
            parse_export_properties(*this, idx, props);
            LevelScene::KismetLookAtPoint lap{};
            lap.object_name = pkg_stem + "." + export_object_name(*this, idx);
            if (const UProperty* loc = find_prop(props, "Location")) {
                lap.position = Vec3(loc->v[0], loc->v[1], loc->v[2]);
            }
            lap.duration_sec = prop_float(props, "LookAtDurationTimer", 0.5f);
            lap.interp_time_sec = prop_float(props, "LookAtInterpolationTimer", 0.1f);
            out_lookats->push_back(std::move(lap));
        } else if (out_transitions && cls == "SeqAct_TdLevelCompleted") {
            UPropertyList props;
            parse_export_properties(*this, idx, props);
            LevelScene::KismetLevelTransition tr{};
            tr.object_name = pkg_stem + "." + export_object_name(*this, idx);
            if (const UProperty* nl = find_prop(props, "NextLevelName")) {
                tr.next_level_name = nl->s;
            }
            if (const UProperty* nc = find_prop(props, "NextCheckpointName")) {
                tr.next_checkpoint_name = nc->s;
            }
            out_transitions->push_back(std::move(tr));
        } else if (out_enemies && cls == "SeqAct_TdActorFactory") {
            UPropertyList props;
            parse_export_properties(*this, idx, props);
            std::string bot_template = "AITemplate_PatrolCop_Colt1911";
            int32_t tpl_ref = prop_object(props, "BotTemplate");
            if (tpl_ref < 0 && -tpl_ref - 1 < static_cast<int32_t>(imports_.size())) {
                const auto& imp = imports_[static_cast<size_t>(-tpl_ref - 1)];
                bot_template = imp.object_number > 0 ? imp.object_name + "_" + std::to_string(imp.object_number - 1) : imp.object_name;
            } else if (tpl_ref > 0 && static_cast<size_t>(tpl_ref) <= exports_.size()) {
                bot_template = export_object_name(*this, tpl_ref);
            }

            std::string weapon = "Colt1911";
            std::string low_tpl = bot_template;
            for (char& ch : low_tpl) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (low_tpl.find("sniper") != std::string::npos || low_tpl.find("m95") != std::string::npos) {
                weapon = "M95";
            } else if (low_tpl.find("support") != std::string::npos || low_tpl.find("minimi") != std::string::npos) {
                weapon = "FNMinimi";
            } else if (low_tpl.find("g36") != std::string::npos) {
                weapon = "G36C";
            } else if (low_tpl.find("scar") != std::string::npos) {
                weapon = "FNSCARL";
            } else if (low_tpl.find("mp5") != std::string::npos) {
                weapon = "MP5K";
            } else if (low_tpl.find("tmp") != std::string::npos) {
                weapon = "SteyrTMP";
            } else if (low_tpl.find("shotgun") != std::string::npos || low_tpl.find("neostead") != std::string::npos) {
                weapon = (low_tpl.find("patrol") != std::string::npos) ? "Remington870" : "Neostead";
            } else if (low_tpl.find("m93") != std::string::npos) {
                weapon = "BerettaM93R";
            }

            if (const UProperty* vlinks = find_prop(props, "VariableLinks")) {
                for (const auto& vl : vlinks->elements) {
                    const UProperty* desc_p = find_prop(vl, "LinkDesc");
                    const UProperty* lvars = find_prop(vl, "LinkedVariables");
                    std::string desc = desc_p ? desc_p->s : "";
                    if (desc != "Spawn Point" || !lvars) continue;
                    for (int32_t sp_ref : lvars->ints) {
                        Vec3 sp{};
                        if (!resolve_actor_location(sp_ref, sp)) continue;
                        if (sp.length_sq() < 1.0f) continue;

                        float yaw_deg = 0.0f;
                        int32_t actual_actor = sp_ref;
                        if (sp_ref > 0 && static_cast<size_t>(sp_ref) <= exports_.size()) {
                            UPropertyList sp_props;
                            parse_export_properties(*this, sp_ref, sp_props);
                            int32_t inner = prop_object(sp_props, "ObjValue");
                            if (inner > 0 && static_cast<size_t>(inner) <= exports_.size()) {
                                actual_actor = inner;
                            }
                            UPropertyList act_props;
                            parse_export_properties(*this, actual_actor, act_props);
                            if (const UProperty* rot = find_prop(act_props, "Rotation")) {
                                yaw_deg = static_cast<float>(rot->vi[1]) * (360.0f / 65536.0f);
                            }
                        }

                        // Avoid duplicating an already-registered spawn point within 25 cm
                        bool duplicate = false;
                        for (const auto& existing : *out_enemies) {
                            if ((existing.home_position - sp).length_sq() < 625.0f) {
                                duplicate = true;
                                break;
                            }
                        }
                        if (duplicate) continue;

                        EnemyBot bot{};
                        bot.archetype = bot_template;
                        bot.weapon_name = weapon;
                        bot.position = sp;
                        bot.home_position = sp;
                        bot.yaw_deg = yaw_deg;
                        out_enemies->push_back(std::move(bot));
                    }
                }
            }
        } else if (out_enemies && cls == "SkeletalMeshActor") {
            UPropertyList props;
            parse_export_properties(*this, idx, props);
            if (prop_bool(props, "bHidden", false)) continue;
            int32_t sk_comp = prop_object(props, "SkeletalMeshComponent");
            if (sk_comp <= 0 || static_cast<size_t>(sk_comp) > exports_.size()) continue;
            UPropertyList cprops;
            parse_export_properties(*this, sk_comp, cprops);
            int32_t sk_ref = prop_object(cprops, "SkeletalMesh");
            if (sk_ref == 0) continue;
            std::string sk_path = object_canonical_path(*this, sk_ref);
            std::string low_sk = sk_path;
            for (char& ch : low_sk) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            std::string arch;
            if (low_sk.find("cop_patrol_female") != std::string::npos || low_sk.find("kate") != std::string::npos) {
                arch = "Kate";
            } else if (low_sk.find("sk_celeste") != std::string::npos || low_sk.find("pursuit_female") != std::string::npos) {
                arch = "Celeste";
            } else if (low_sk.find("jacknife") != std::string::npos) {
                arch = "Jacknife";
            } else if (low_sk.find("crim_rb") != std::string::npos) {
                arch = "Ropeburn";
            } else if (low_sk.find("sk_miller") != std::string::npos) {
                arch = "Miller";
            } else if (low_sk.find("sk_kreeg") != std::string::npos) {
                arch = "Kreeg";
            }
            if (arch.empty()) continue;

            Vec3 loc{};
            if (const UProperty* lp = find_prop(props, "Location")) loc = Vec3(lp->v[0], lp->v[1], lp->v[2]);
            if (loc.length_sq() < 1.0f) continue;
            float yaw_deg = 0.0f;
            if (const UProperty* rp = find_prop(props, "Rotation")) {
                yaw_deg = static_cast<float>(rp->vi[1]) * (360.0f / 65536.0f);
            }
            EnemyBot npc{};
            npc.archetype = arch;
            npc.weapon_name = "None";
            // Placed 3P SkeletalMeshActors have their origin at waist/capsule pivot (~72 uu above floor)
            npc.position = loc - Vec3(0.0f, 0.0f, 72.0f);
            npc.home_position = npc.position;
            npc.yaw_deg = yaw_deg;
            npc.is_story_npc = true;
            npc.cutscene_only = false;
            npc.sublevel_pkg = pkg_stem;
            out_enemies->push_back(std::move(npc));
        } else if (out_enemies && cls == "SeqAct_Interp") {
            UPropertyList props;
            parse_export_properties(*this, idx, props);
            const UProperty* vlinks = find_prop(props, "VariableLinks");
            if (!vlinks) continue;

            int32_t interp_data = 0;
            std::unordered_map<std::string, int32_t> group_actors;
            for (const auto& vl : vlinks->elements) {
                const UProperty* desc_p = find_prop(vl, "LinkDesc");
                const UProperty* lvars = find_prop(vl, "LinkedVariables");
                if (!desc_p || !lvars || lvars->ints.empty()) continue;
                int32_t target = lvars->ints[0];
                if (desc_p->s == "Data") {
                    interp_data = target;
                } else {
                    if (target > 0 && static_cast<size_t>(target) <= exports_.size()) {
                        UPropertyList vp;
                        parse_export_properties(*this, target, vp);
                        int32_t obj = prop_object(vp, "ObjValue");
                        if (obj > 0 && static_cast<size_t>(obj) <= exports_.size()) {
                            group_actors[desc_p->s] = obj;
                        }
                    }
                }
            }
            if (interp_data <= 0 || static_cast<size_t>(interp_data) > exports_.size()) continue;
            UPropertyList dprops;
            parse_export_properties(*this, interp_data, dprops);
            const UProperty* groups = find_prop(dprops, "InterpGroups");
            if (!groups) continue;

            const std::string matinee_label = pkg_stem + "." + export_object_name(*this, idx);
            for (int32_t g_ref : groups->ints) {
                if (g_ref <= 0 || static_cast<size_t>(g_ref) > exports_.size()) continue;
                UPropertyList gprops;
                parse_export_properties(*this, g_ref, gprops);
                std::string gname = prop_name(gprops, "GroupName");
                auto it_act = group_actors.find(gname);
                if (it_act == group_actors.end()) continue;
                int32_t actor_exp = it_act->second;
                std::string actor_cls = get_export_class(exports_[static_cast<size_t>(actor_exp - 1)]);
                if (actor_cls != "SkeletalMeshActor") continue;

                UPropertyList aprops;
                parse_export_properties(*this, actor_exp, aprops);
                int32_t sk_comp = prop_object(aprops, "SkeletalMeshComponent");
                if (sk_comp <= 0 || static_cast<size_t>(sk_comp) > exports_.size()) continue;
                UPropertyList cprops;
                parse_export_properties(*this, sk_comp, cprops);
                std::string sk_path = object_canonical_path(*this, prop_object(cprops, "SkeletalMesh"));
                std::string low_sk = sk_path;
                for (char& ch : low_sk) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                // Skip Faith's 3P stand-in (SK_TKY_Crim_Fixer) and non-character props (Paper/Bag/Blackhawk/Handcuffs)
                std::string arch;
                if (low_sk.find("cop_patrol_female") != std::string::npos || low_sk.find("kate") != std::string::npos) {
                    arch = "Kate";
                } else if (low_sk.find("sk_celeste") != std::string::npos || low_sk.find("pursuit_female") != std::string::npos) {
                    arch = "Celeste";
                } else if (low_sk.find("jacknife") != std::string::npos) {
                    arch = "Jacknife";
                } else if (low_sk.find("crim_rb") != std::string::npos) {
                    arch = "Ropeburn";
                } else if (low_sk.find("sk_miller") != std::string::npos) {
                    arch = "Miller";
                } else if (low_sk.find("sk_kreeg") != std::string::npos) {
                    arch = "Kreeg";
                } else if (low_sk.find("cop_swat") != std::string::npos) {
                    arch = "SWAT";
                }
                if (arch.empty()) continue;

                Vec3 aloc{};
                if (const UProperty* lp = find_prop(aprops, "Location")) aloc = Vec3(lp->v[0], lp->v[1], lp->v[2]);
                float ayaw = 0.0f;
                if (const UProperty* rp = find_prop(aprops, "Rotation")) {
                    ayaw = static_cast<float>(rp->vi[1]) * (360.0f / 65536.0f);
                }

                std::string anim_seq;
                float start_sec = 0.0f;
                if (const UProperty* tracks = find_prop(gprops, "InterpTracks")) {
                    for (int32_t t_ref : tracks->ints) {
                        if (t_ref <= 0 || static_cast<size_t>(t_ref) > exports_.size()) continue;
                        if (get_export_class(exports_[static_cast<size_t>(t_ref - 1)]) != "InterpTrackAnimControl") continue;
                        UPropertyList tprops;
                        parse_export_properties(*this, t_ref, tprops);
                        if (const UProperty* keys = find_prop(tprops, "AnimSeqs")) {
                            for (const auto& k_el : keys->elements) {
                                std::string sname = prop_name(k_el, "AnimSeqName");
                                if (!sname.empty()) {
                                    anim_seq = sname;
                                    start_sec = prop_float(k_el, "StartTime", 0.0f);
                                    break;
                                }
                            }
                        }
                        if (!anim_seq.empty()) break;
                    }
                }

                // Resolve 1-based AnimSequence export index inside GroupAnimSets in this package
                int32_t anim_exp_1 = 0;
                if (!anim_seq.empty()) {
                    std::string low_want = anim_seq;
                    for (char& ch : low_want) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    if (const UProperty* asets = find_prop(gprops, "GroupAnimSets")) {
                        for (int32_t as_ref : asets->ints) {
                            if (as_ref <= 0 || static_cast<size_t>(as_ref) > exports_.size()) continue;
                            UPropertyList as_props;
                            parse_export_properties(*this, as_ref, as_props);
                            if (const UProperty* seqs = find_prop(as_props, "Sequences")) {
                                for (int32_t seq_ref : seqs->ints) {
                                    if (seq_ref <= 0 || static_cast<size_t>(seq_ref) > exports_.size()) continue;
                                    UPropertyList sprops;
                                    parse_export_properties(*this, seq_ref, sprops);
                                    std::string sname = prop_name(sprops, "SequenceName");
                                    for (char& ch : sname) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                                    if (sname == low_want) {
                                        anim_exp_1 = seq_ref;
                                        break;
                                    }
                                }
                            }
                            if (anim_exp_1 > 0) break;
                        }
                    }
                }

                if (anim_exp_1 <= 0 || anim_seq.empty()) continue;

                EnemyBot cs_npc{};
                cs_npc.archetype = arch;
                cs_npc.weapon_name = "None";
                cs_npc.position = aloc;
                cs_npc.home_position = aloc;
                cs_npc.yaw_deg = ayaw;
                cs_npc.alive = false; // Enabled only while matinee_label is actively playing!
                cs_npc.is_story_npc = true;
                cs_npc.cutscene_only = true;
                cs_npc.sublevel_pkg = pkg_stem;
                cs_npc.cutscene_label = matinee_label;
                cs_npc.cutscene_pkg_path = file_path_;
                cs_npc.cutscene_anim_exp_1 = anim_exp_1;
                cs_npc.active_anim_seq = anim_seq;
                cs_npc.cutscene_start_sec = start_sec;
                out_enemies->push_back(std::move(cs_npc));
            }
        }
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
    out_scene.collision.reset();
    out_scene.kill_z = -1.0e30f;
    out_scene.checkpoints.clear();
    out_scene.subtitles.clear();
    out_scene.checkpoint_infos.clear();
    out_scene.streaming_actions.clear();
    out_scene.all_streaming_packages.clear();
    out_scene.loaded_sublevel_packages.clear();
    out_scene.elevators.clear();
    out_scene.reflection_captures.clear();
    out_scene.reflection_volumes.clear();
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
    // Class default objects / component templates for resolving unserialized (default) actor and
    // component properties such as the collision flags.
    ScriptDefaults::instance().init(cooked_root.string());

    std::unordered_map<std::string, StaticMeshAsset> mesh_library;
    std::vector<std::shared_ptr<UPKPackage>> loaded_packages;
    loaded_packages.push_back(master_pkg);

    // Extract LevelStreamingKismet, TdCheckpoint (with StreamingLevels), and SeqAct_MultiLevelStreaming
    master_pkg->extract_level_streaming_and_checkpoints(
        out_scene.checkpoint_infos, out_scene.streaming_actions, out_scene.all_streaming_packages);

    // Extract from master package
    master_pkg->extract_static_meshes(mesh_library);
    auto master_actors = master_pkg->extract_actors();
    auto master_sounds = master_pkg->extract_audio();
    LevelSun level_sun;
    LevelAmbient level_ambient;
    scan_level_suns(*master_pkg, level_sun);
    scan_level_ambient(*master_pkg, level_ambient);

    out_scene.actors.insert(out_scene.actors.end(), master_actors.begin(), master_actors.end());
    out_scene.sounds.insert(out_scene.sounds.end(), master_sounds.begin(), master_sounds.end());

    // Automatically discover and load sub-level geometry, art slices, transition slices (*_Slc),
    // and script/elevator packages (*_Spt) referenced by LevelStreamingKismet or adjacent in map_dir.
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

    auto should_load_subpkg = [&](const std::string& fname, bool require_prefix = true) -> bool {
        std::string low = fname;
        std::transform(low.begin(), low.end(), low.begin(), ::tolower);
        if (require_prefix && low.rfind(low_prefix, 0) != 0) {
            return false;
        }
        // Skip localization, music, audio, lookat, and time-trial sub-packages.
        // Note: *_spt, *_slc, *_cs, and *_lgts MUST be loaded because Mirror's Edge places interactive
        // elevator cabs, Matinee sequences, and 2,100+ collidable StaticMeshActors (flowerbeds, billboards,
        // roof props) inside those sub-packages.
        if (low.find("_loc_") != std::string::npos || low.find("_mus") != std::string::npos ||
            low.find("_aud") != std::string::npos || low.find("_peds") != std::string::npos ||
            low.find("_lookat") != std::string::npos ||
            low.rfind("tt_", 0) == 0) {
            return false;
        }
        return true;
    };

    // 1. From LevelStreamingKismet.PackageName and AdditionalPackagesToCook in master package
    for (const auto& stream_pkg : out_scene.all_streaming_packages) {
        if (!should_load_subpkg(stream_pkg, false)) continue;
        fs::path p = map_dir / (stream_pkg + ".me1");
        if (fs::exists(p)) sub_packages.insert(p.string());
        p = map_dir / (stream_pkg + ".upk");
        if (fs::exists(p)) sub_packages.insert(p.string());
    }
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

    // Load geometry, art, slice (*_Slc), lighting/props (*_Lgts), and script/elevator (*_Spt) sub-packages
    size_t loaded_sub = 0;
    for (const auto& sub_path : sub_packages) {
        if (loaded_sub++ >= 160) break;
        auto sub_pkg = std::make_shared<UPKPackage>(sub_path);
        if (!sub_pkg->is_valid()) continue;

        std::string sub_stem = fs::path(sub_path).stem().string();

        loaded_packages.push_back(sub_pkg);
        sub_pkg->extract_static_meshes(mesh_library);
        auto sub_actors = sub_pkg->extract_actors();
        out_scene.actors.insert(out_scene.actors.end(), sub_actors.begin(), sub_actors.end());
        sub_pkg->extract_level_streaming_and_checkpoints(
            out_scene.checkpoint_infos, out_scene.streaming_actions, out_scene.all_streaming_packages);
        scan_level_suns(*sub_pkg, level_sun);
        scan_level_ambient(*sub_pkg, level_ambient);
        out_scene.loaded_sublevel_packages.push_back(sub_stem);
        if (pm) pm->add_loaded(sub_stem, sub_pkg);
    }

    // Extract all interactive elevators (InterpActor + SeqAct_Interp + InterpTrackMove)
    // now that mesh_library has all UStaticMesh bounds (S_Elevator_01, S_SP09_ElevatorWithTop_01, etc.),
    // and real-time planar reflection actors/volumes (SceneCaptureReflectActor + TdReflectionVolume)
    std::vector<InterpDoorInfo> door_infos;
    for (const auto& pkg : loaded_packages) {
        pkg->extract_elevators(mesh_library, out_scene.elevators, &door_infos);
        pkg->extract_reflections(out_scene.reflection_captures, out_scene.reflection_volumes);
        pkg->extract_helicopter_encounters(out_scene.heli_attack_nodes, out_scene.helicopters, out_scene.dummy_fire_barrages,
                                           &out_scene.kismet_valves, &out_scene.kismet_lookat_points, &out_scene.kismet_level_transitions,
                                           &out_scene.enemies);
    }
    // Bind each elevator to its real moving InterpActors (cab, attached cab doors, landing doors).
    assign_elevator_parts(out_scene, door_infos, mesh_library);
    assign_barge_doors(out_scene);

    // The chapter's start-of-level intro: its Matinee, baked first-person camera and sounds.
    extract_level_intro(game_root, loaded_packages, out_scene.level_intro);

    // Every Matinee that drives the pawn (the intro among them), and the level's Kismet that
    // starts them, sets the checkpoints, puts text on the screen and ends the chapter.
    std::unordered_map<std::string, int> cutscene_of;
    extract_player_cutscenes(game_root, loaded_packages, out_scene.level_intro, out_scene.cutscenes, cutscene_of);
    // The training area's Kismet hangs on its movement-challenge system (SeqAct_TdStartMovementChallenge,
    // SeqEvt_TdMovementChallenge*), which the port does not run; it keeps its staged tutorial.
    if (low_prefix != "tutorial") {
        auto graph = std::make_shared<ScriptGraph>();
        std::vector<std::string> warnings;
        if (graph->load(loaded_packages, cutscene_of, warnings)) {
            size_t events = 0, triggers = 0;
            for (const ScriptGraph::Node& n : graph->nodes) {
                if (n.cls.rfind("SeqEvent", 0) == 0 || n.cls.rfind("SeqEvt", 0) == 0) ++events;
                if (n.cls == "SeqEvent_Touch" || n.cls == "SeqEvent_TdTouch") ++triggers;
            }
            std::cout << "[Level] Kismet: " << graph->nodes.size() << " sequence objects in " << graph->packages.size()
                      << " packages, " << events << " events (" << triggers << " touch), " << graph->actors.size()
                      << " actors, " << graph->matinees.size() << " Matinees, " << out_scene.cutscenes.size()
                      << " player cutscenes" << (warnings.empty() ? "" : ", " + std::to_string(warnings.size()) + " warnings")
                      << std::endl;
            out_scene.script = std::move(graph);
            out_scene.script_checkpoints = true;
        }
    }

    // Link each extracted elevator to the checkpoints at its start and destination floors so
    // riding the elevator streams in the destination zone's sub-packages (SeqAct_MultiLevelStreaming)
    for (auto& elev : out_scene.elevators) {
        int start_cp = -1;
        int end_cp = -1;
        float best_start_d2 = 1e18f;
        float best_end_d2 = 1e18f;
        for (size_t c = 0; c < out_scene.checkpoint_infos.size(); ++c) {
            const auto& cp = out_scene.checkpoint_infos[c];
            if (cp.streaming_levels.empty()) continue;
            float ds2 = (cp.location - elev.start_pos).length_sq();
            float de2 = (cp.location - elev.end_pos).length_sq();
            if (ds2 < best_start_d2) {
                best_start_d2 = ds2;
                start_cp = static_cast<int>(c);
            }
            if (de2 < best_end_d2 && static_cast<int>(c) != start_cp) {
                best_end_d2 = de2;
                end_cp = static_cast<int>(c);
            }
        }
        if (end_cp >= 0) {
            elev.target_checkpoint_idx = end_cp;
            const auto& dst_levels = out_scene.checkpoint_infos[end_cp].streaming_levels;
            std::set<std::string> src_set;
            if (start_cp >= 0) {
                for (const auto& s : out_scene.checkpoint_infos[start_cp].streaming_levels) {
                    src_set.insert(to_lower(s));
                }
            }
            std::set<std::string> dst_set;
            for (const auto& d : dst_levels) {
                dst_set.insert(to_lower(d));
                if (src_set.find(to_lower(d)) == src_set.end()) {
                    elev.stream_in_packages.push_back(d);
                }
            }
            if (start_cp >= 0) {
                for (const auto& s : out_scene.checkpoint_infos[start_cp].streaming_levels) {
                    if (dst_set.find(to_lower(s)) == dst_set.end()) {
                        elev.stream_out_packages.push_back(s);
                    }
                }
            }
        }
        if (elev.stream_in_packages.empty() && !out_scene.streaming_actions.empty()) {
            elev.stream_in_packages = out_scene.streaming_actions.front().package_names;
        }
    }

    // Initialize loaded_sublevel_packages from DefaultCheckpoint.StreamingLevels when available
    if (!out_scene.checkpoint_infos.empty() && !out_scene.checkpoint_infos.front().streaming_levels.empty()) {
        out_scene.loaded_sublevel_packages = out_scene.checkpoint_infos.front().streaming_levels;
    }

    // The sun and SkyLight live in the *_Lgts lighting packages, which carry no geometry: open them for lights only.
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
            if (light_pkg.is_valid()) {
                scan_level_suns(light_pkg, level_sun);
                scan_level_ambient(light_pkg, level_ambient);
            }
        }
    }
    if (level_sun.found) {
        out_scene.sun_direction = level_sun.direction.normalized();
        out_scene.sun_color = level_sun.color;
        out_scene.mod_shadow_color = level_sun.mod_shadow_color;
        std::cout << "[Level] Sun from " << level_sun.source << ": direction (" << out_scene.sun_direction.x << ", "
                  << out_scene.sun_direction.y << ", " << out_scene.sun_direction.z << "), linear colour ("
                  << out_scene.sun_color.x << ", " << out_scene.sun_color.y << ", " << out_scene.sun_color.z
                  << "), ModShadowColor (" << out_scene.mod_shadow_color.x << ", " << out_scene.mod_shadow_color.y
                  << ", " << out_scene.mod_shadow_color.z << ")" << std::endl;
    }

    // Populate reverse-engineered ambient lighting (FSkyLightSceneProxy @ VA 0x00ED4100 + WorldInfo.SkyColor)
    out_scene.world_sky_color = level_ambient.world_sky_color;
    out_scene.ibl_intensity = level_ambient.ibl_intensity;
    out_scene.haze_color = level_ambient.haze_color;
    out_scene.sky_light_source = level_ambient.has_skylight ? level_ambient.sky_source : level_ambient.world_source;
    out_scene.raw_sky_upper_linear = Vec3(
        srgb_byte_to_linear(level_ambient.sky_light_color.x) * level_ambient.sky_brightness,
        srgb_byte_to_linear(level_ambient.sky_light_color.y) * level_ambient.sky_brightness,
        srgb_byte_to_linear(level_ambient.sky_light_color.z) * level_ambient.sky_brightness);
    out_scene.raw_sky_lower_linear = Vec3(
        srgb_byte_to_linear(level_ambient.sky_lower_color.x) * level_ambient.sky_lower_brightness,
        srgb_byte_to_linear(level_ambient.sky_lower_color.y) * level_ambient.sky_lower_brightness,
        srgb_byte_to_linear(level_ambient.sky_lower_color.z) * level_ambient.sky_lower_brightness);

    // Calibrate real-time UpperSkyColor & LowerSkyColor for BasePass GetMaterialHemisphereLightTransferFull
    // (combining SkyLightComponent.LightColor/LowerColor, DirectionalLight.ModShadowColor, and WorldInfo.SkyColor
    // so unbaked real-time rooftops receive authentic City of Glass cool azure shadows + warm ground bounce).
    {
        Vec3 up_src = level_ambient.has_skylight ? level_ambient.sky_light_color : level_ambient.world_sky_color;
        float up_max = std::max({up_src.x, up_src.y, up_src.z, 1e-4f});
        Vec3 up_norm = up_src * (1.0f / up_max);

        Vec3 ms = out_scene.mod_shadow_color;
        float ms_max = std::max({ms.x, ms.y, ms.z, 1e-4f});
        Vec3 ms_norm = (ms_max > 0.05f) ? (ms * (1.0f / ms_max)) : Vec3(0.494f, 0.659f, 0.875f);

        // Preserve Mirror's Edge signature cool cerulean-azure sky fill (ModShadowColor + SkyLight)
        Vec3 upper_blend = up_norm * 0.40f + ms_norm * 0.60f;
        out_scene.sky_upper_color = Vec3(
            std::clamp(upper_blend.x * 0.82f, 0.36f, 0.62f),
            std::clamp(upper_blend.y * 0.94f, 0.62f, 0.82f),
            std::clamp(upper_blend.z, 0.92f, 1.00f));

        // Preserve warm sunlit concrete ground/wall radiosity bounce (FSkyLightSceneProxy LowerColor)
        Vec3 lo_src = level_ambient.sky_lower_color;
        float lo_max = std::max({lo_src.x, lo_src.y, lo_src.z, 1e-4f});
        Vec3 lo_norm = (lo_max > 0.05f) ? (lo_src * (1.0f / lo_max)) : Vec3(1.0f, 0.88f, 0.74f);
        out_scene.sky_lower_color = Vec3(
            std::clamp(lo_norm.x * 0.96f, 0.82f, 0.98f),
            std::clamp(lo_norm.y * 0.82f, 0.68f, 0.84f),
            std::clamp(lo_norm.z * 0.64f, 0.46f, 0.66f));
    }
    if (level_ambient.has_skylight || level_ambient.has_world_sky) {
        std::cout << "[Level] Ambient from " << out_scene.sky_light_source
                  << ": FSkyLightSceneProxy UpperLinear=(" << out_scene.raw_sky_upper_linear.x << ", "
                  << out_scene.raw_sky_upper_linear.y << ", " << out_scene.raw_sky_upper_linear.z
                  << "), LowerLinear=(" << out_scene.raw_sky_lower_linear.x << ", "
                  << out_scene.raw_sky_lower_linear.y << ", " << out_scene.raw_sky_lower_linear.z
                  << "), WorldInfo.SkyColor=(" << out_scene.world_sky_color.x << ", "
                  << out_scene.world_sky_color.y << ", " << out_scene.world_sky_color.z
                  << ") IBL=" << out_scene.ibl_intensity
                  << ", Calibrated Hemisphere Upper=(" << out_scene.sky_upper_color.x << ", "
                  << out_scene.sky_upper_color.y << ", " << out_scene.sky_upper_color.z
                  << ") Lower=(" << out_scene.sky_lower_color.x << ", "
                  << out_scene.sky_lower_color.y << ", " << out_scene.sky_lower_color.z << ")" << std::endl;
    }

    // Real UStaticMesh + BSP UModel render batches + UE3 collision (populating each actor's transformed world_bounds),
    // then the moving elevator parts with their own buffers and collision.
    std::vector<std::string> material_paths;
    auto collision = std::make_shared<CollisionWorld>();
    std::vector<BspRenderBin> bsp_render_bins;
    AABB bsp_bounds(Vec3(1e30f, 1e30f, 1e30f), Vec3(-1e30f, -1e30f, -1e30f));
    LightMapSets lightmap_sets;
    {
        // Level BSP (rooms, interiors blocked out with brushes) collides and renders as world geometry.
        std::vector<Vec3> bsp;
        for (const auto& pkg : loaded_packages) {
            pkg->extract_bsp_collision(bsp);
            pkg->extract_bsp_render_geometry(bsp_render_bins, bsp_bounds, pm ? &lightmap_sets : nullptr);
        }
        collision->reserve(bsp.size() / 3 + 1024);
        for (size_t i = 0; i + 2 < bsp.size(); i += 3) {
            collision->add_triangle(bsp[i], bsp[i + 1], bsp[i + 2], -1, COLL_BlockAll);
        }
    }
    // Each section's vertices carry the two UV sets its material reads, which have to be known
    // while the geometry is emitted: this translates the graphs once, without their textures.
    std::unique_ptr<MaterialUVResolver> material_uvs;
    if (pm) material_uvs = std::make_unique<MaterialUVResolver>(*pm);
    build_level_geometry(out_scene.actors, out_scene.meshes, *collision, mesh_library, pm ? &material_paths : nullptr,
                         &bsp_render_bins, &bsp_bounds, material_uvs.get(), pm ? &lightmap_sets : nullptr);
    collision->build();
    build_elevator_part_geometry(out_scene, mesh_library, pm ? &material_paths : nullptr, material_uvs.get());
    build_barge_door_geometry(out_scene, mesh_library, pm ? &material_paths : nullptr, material_uvs.get());
    material_uvs.reset();

    // WorldInfo.KillZ when the level sets it (otherwise lethal falls are handled by fall height).
    for (size_t i = 0; i < master_pkg->get_exports().size(); ++i) {
        if (master_pkg->get_export_class(master_pkg->get_exports()[i]) != "WorldInfo") continue;
        UPropertyList wprops;
        parse_export_properties(*master_pkg, static_cast<int32_t>(i) + 1, wprops);
        if (const UProperty* kz = find_prop(wprops, "KillZ")) out_scene.kill_z = kz->f;
        break;
    }
    out_scene.collision = std::move(collision);
    extract_level_postprocess(*master_pkg, loaded_packages, out_scene);

    // Resolve, translate and load every material referenced by the level geometry
    // (ME_MATERIAL_VERBOSE=1 prints per-material diagnostics, ME_MAX_TEXTURE_SIZE caps mip size).
    if (pm && !material_paths.empty()) {
        MaterialBuildOptions mopts;
        if (const char* v = std::getenv("ME_MATERIAL_VERBOSE")) mopts.verbose = (v[0] != '\0' && v[0] != '0');
        if (const char* s = std::getenv("ME_MAX_TEXTURE_SIZE")) mopts.max_texture_size = std::max(16, std::atoi(s));
        std::shared_ptr<SceneMaterialLibrary> library = build_scene_materials(*pm, material_paths, mopts);
        // The baked light maps the geometry was emitted against.
        if (library) lightmap_sets.load(*pm, loaded_packages, library->lightmap_textures);
        out_scene.materials = std::move(library);
    }
    pm.reset();

    if (low_prefix == "tutorial") {
        // Exact 19-stage Tutorial progression extracted from Tutorial_p.me1 TdTutorialStart exports
        // (feet Z = TdTutorialStart.Location.z - 94.0f)
        out_scene.chapter_title = "TRAINING: TUTORIAL";
        out_scene.player_spawn_pos = Vec3(-4813.65f, -7903.79f, 5760.0f);
        out_scene.player_spawn_yaw = 0.0f;

        struct TutorialStage {
            Vec3 pos;
            const char* subtitle;
        };
        static const TutorialStage kTutorialStages[] = {
            {Vec3(-4813.6f, -7903.8f, 5760.0f), "Celeste: Follow me across the roof! (WASD + Mouse, V: Look-At Hint, [/]: Skip Stage)"},
            {Vec3(-3332.3f, -7914.6f, 5760.0f), "Stage 2/19: Press SPACE to Jump over the rooftop curbing"},
            {Vec3(208.0f, -7744.0f, 5760.0f),   "Stage 3/19: Hold C / Left Ctrl while running to Slide under the airduct"},
            {Vec3(1473.0f, -7869.0f, 5760.0f),  "Stage 4/19: Sprint and press SPACE at the edge to Jump the rooftop gap"},
            {Vec3(2015.9f, -6260.9f, 4247.0f),  "Stage 5/19: Turn West (180 deg) and press SPACE to Vault the fence"},
            {Vec3(758.2f, -6460.0f, 4224.0f),   "Stage 6/19: Angle into the wall and hold SPACE to Horizontal Wallrun"},
            {Vec3(-1655.2f, -6505.2f, 4224.0f), "Stage 7/19: Sprint and press SPACE to Speed Vault over the obstacle"},
            {Vec3(-3748.7f, -6363.9f, 4224.0f), "Stage 8/19: Barge through the rooftop doorway and continue West"},
            {Vec3(-5017.8f, -6434.3f, 4224.0f), "Stage 9/19: Walk across the narrow Balance Beam to the far roof"},
            {Vec3(-7184.1f, -5744.1f, 4224.0f), "Stage 10/19: Face the wall and hold SPACE to Vertical Wallclimb"},
            {Vec3(-7902.2f, -5026.3f, 4720.0f), "Stage 11/19: Climb the pipe and leap across the Swingpole"},
            {Vec3(-7112.2f, -3216.2f, 4704.0f), "Stage 12/19: Wallclimb (SPACE), press Q to Turn 180, then SPACE to Jump"},
            {Vec3(-6967.4f, -2757.4f, 4992.0f), "Stage 13/19: Jump to Grab the upper ledge, then press SPACE/W to Pull Up"},
            {Vec3(-8448.0f, -2840.0f, 5792.0f), "Stage 14/19: Carefully edge along the narrow Ledge Walk"},
            {Vec3(-8503.6f, -3787.3f, 5760.0f), "Stage 15/19: Press C before landing for a Skill Roll, or ride the Zipline!"},
            {Vec3(-2982.6f, -3933.6f, 4410.0f), "Stage 16/19: Jump (SPACE) then tuck legs in mid-air (C) for a Coil Jump"},
            {Vec3(156.4f, -3933.6f, 3840.0f),   "Stage 17/19: Sprint at the stacked boxes and press SPACE to Springboard"},
            {Vec3(2221.7f, -3884.2f, 4992.0f),  "Stage 18/19: Leap up toward the combat training terrace"},
            {Vec3(751.8f, -1591.7f, 4992.0f),   "Stage 19/19: Combat Training - Press Left Click/F to Melee or Right Click/E to Disarm Celeste!"}
        };
        for (const auto& st : kTutorialStages) {
            out_scene.checkpoints.push_back(st.pos);
            out_scene.subtitles.emplace_back(st.subtitle);
        }

        // Spawn Celeste training partner on the combat rooftop for Stage 19 Melee & Disarm training
        EnemyBot celeste;
        celeste.archetype = "TutorialTrainer_Celeste";
        celeste.position = Vec3(751.8f, -1320.0f, 4992.0f);
        celeste.home_position = celeste.position;
        celeste.yaw_deg = -90.0f;
        celeste.health = 100.0f;
        celeste.weapon_name = "Colt1911";
        celeste.disarm_window = true;
        celeste.anim_state = EEnemyAnimState::MeleeWindup;
        out_scene.enemies.push_back(celeste);

        // Spawn KrugerSec armed sparring guards on the Stage 19 combat terrace (carrying 2H Assault Rifle, Shotgun, SMG)
        struct TerraceGuardSpec {
            const char* archetype;
            const char* weapon;
            Vec3 pos;
            float yaw;
        };
        static const TerraceGuardSpec kTerraceGuards[] = {
            {"Assault_SWAT",    "G36C",         Vec3(1020.0f, -1040.0f, 4992.0f), -110.0f},
            {"Support_Shotgun", "Remington870", Vec3(480.0f,  -1040.0f, 4992.0f), -70.0f},
            {"PatrolCop_SMG",   "MP5K",         Vec3(751.8f,   -820.0f, 4992.0f), -90.0f}
        };
        for (const auto& tg : kTerraceGuards) {
            EnemyBot guard{};
            guard.archetype = tg.archetype;
            guard.weapon_name = tg.weapon;
            guard.position = tg.pos;
            guard.home_position = tg.pos;
            guard.yaw_deg = tg.yaw;
            guard.health = 100.0f;
            guard.max_health = 100.0f;
            guard.alive = true;
            guard.disarm_window = false;
            guard.anim_state = EEnemyAnimState::AimFire;
            out_scene.enemies.push_back(guard);
        }
    } else {
        // Find the best outdoor rooftop PlayerStart / TdTutorialStart / TdCheckpoint surrounded by dense 3D geometry.
        // Prefer the chapter's DefaultCheckpoint when it has valid coordinates.
        bool found_start = false;
        for (const auto& cp : out_scene.checkpoint_infos) {
            if (cp.default_checkpoint && (cp.location.x != 0.0f || cp.location.y != 0.0f || cp.location.z != 0.0f)) {
                out_scene.player_spawn_pos = cp.location + Vec3(0.0f, 0.0f, 35.0f);
                out_scene.player_spawn_yaw = cp.rotation.to_degrees().y;
                found_start = true;
                break;
            }
        }
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
            if (!found_start && score > best_score) {
                best_score = score;
                out_scene.player_spawn_pos = a.location + Vec3(0.0f, 0.0f, 35.0f);
                out_scene.player_spawn_yaw = a.rotation.to_degrees().y;
                found_start = true;
            }
        }
        if (!found_start && !out_scene.actors.empty()) {
            out_scene.player_spawn_pos = out_scene.actors.front().location + Vec3(0, 0, 96.0f);
        }

        // If the chapter has a cooked Matinee level intro ending on a valid world-space rooftop
        // (e.g. Edge_Pt1_CS sp01_intro where DefaultCheckpoint is the pre-intro sky camera point at Z=15277
        // while Faith runs to the rooftop ledge at Z=8424), set post-intro gameplay spawn to that exact floor pose.
        // The animation's root ends on the floor, so that is where the player stands: lifting the
        // spawn point off it would drop the view by that much the moment the intro hands over.
        if (out_scene.level_intro.valid && out_scene.level_intro.end_feet_pos.length_xy() > 100.0f) {
            out_scene.player_spawn_pos = out_scene.level_intro.end_feet_pos + Vec3(0.0f, 0.0f, 2.0f);
            out_scene.player_spawn_yaw = out_scene.level_intro.end_yaw_deg;
        }

        // Collect checkpoints (ordered by TdCheckpoint weight first) and enemies
        for (const auto& cp : out_scene.checkpoint_infos) {
            if (cp.location.x != 0.0f || cp.location.y != 0.0f || cp.location.z != 0.0f) {
                out_scene.checkpoints.push_back(cp.location);
            }
        }
        static const char* kCampaignWeapons[] = {
            "Colt1911", "MP5K", "G36C", "Remington870", "FNSCARL",
            "SteyrTMP", "BerettaM93R", "Neostead", "FNMinimi", "M95"
        };
        size_t enemy_ord = 0;
        for (const auto& a : out_scene.actors) {
            if (a.is_checkpoint && out_scene.checkpoints.empty()) {
                out_scene.checkpoints.push_back(a.location);
            }
            if (a.is_enemy) {
                EnemyBot bot{};
                bot.archetype = a.class_name;
                bot.position = a.location;
                bot.home_position = a.location;
                bot.yaw_deg = a.rotation.to_degrees().y;
                if (a.class_name.find("Sniper") != std::string::npos) {
                    bot.weapon_name = "M95";
                } else if (a.class_name.find("Support") != std::string::npos) {
                    bot.weapon_name = "FNMinimi";
                } else if (a.class_name.find("Assault") != std::string::npos) {
                    bot.weapon_name = (enemy_ord % 2 == 0) ? "G36C" : "Remington870";
                } else {
                    bot.weapon_name = kCampaignWeapons[enemy_ord % 10];
                }
                enemy_ord++;
                out_scene.enemies.push_back(bot);
            }
        }
        if (out_scene.collision) {
            for (auto& bot : out_scene.enemies) {
                CollisionHit floor_hit = out_scene.collision->line_check(
                    bot.position + Vec3(0.0f, 0.0f, 80.0f), bot.position - Vec3(0.0f, 0.0f, 180.0f));
                if (floor_hit.hit) {
                    bot.position.z = floor_hit.location.z;
                    bot.home_position.z = floor_hit.location.z;
                }
            }
        }
    }

    std::cout << "[Level] Streaming summary for " << out_scene.map_name << ": "
              << out_scene.all_streaming_packages.size() << " LevelStreamingKismet sublevels, "
              << out_scene.checkpoint_infos.size() << " TdCheckpoints, "
              << out_scene.streaming_actions.size() << " SeqAct_MultiLevelStreaming actions, "
              << out_scene.elevators.size() << " interactive elevators, "
              << out_scene.reflection_captures.size() << " SceneCaptureReflectActors, "
              << out_scene.reflection_volumes.size() << " TdReflectionVolumes" << std::endl;

    return true;
}

bool stream_level_to_checkpoint(const std::string& /*game_root*/, LevelScene& scene, int checkpoint_idx) {
    if (checkpoint_idx < 0 || static_cast<size_t>(checkpoint_idx) >= scene.checkpoint_infos.size()) {
        return false;
    }
    const auto& cp = scene.checkpoint_infos[static_cast<size_t>(checkpoint_idx)];
    if (!cp.streaming_levels.empty()) {
        scene.loaded_sublevel_packages = cp.streaming_levels;
    }
    if (cp.location.x != 0.0f || cp.location.y != 0.0f || cp.location.z != 0.0f) {
        scene.player_spawn_pos = cp.location + Vec3(0.0f, 0.0f, 35.0f);
        scene.player_spawn_yaw = cp.rotation.to_degrees().y;
    }
    std::cout << "[Streaming] TdCheckpoint '" << cp.checkpoint_name << "' (weight " << cp.checkpoint_weight
              << ") active -> " << scene.loaded_sublevel_packages.size() << " sublevels streamed in" << std::endl;
    return true;
}

} // namespace me

