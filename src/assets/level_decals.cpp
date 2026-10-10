#include "level_decals.hpp"

#include "ue3_props.hpp"
#include "upk_loader.hpp"

#include <cstring>
#include <iostream>

namespace me {

namespace {

// A cursor over one export's bytes; `ok` goes false at the first read past its end.
struct Reader {
    const std::vector<uint8_t>& data;
    size_t pos;
    size_t end;
    bool ok = true;

    bool has(size_t n) {
        if (!ok || n > end || pos > end - n) ok = false;
        return ok;
    }
    int32_t i32() {
        int32_t v = 0;
        if (has(4)) {
            std::memcpy(&v, data.data() + pos, 4);
            pos += 4;
        }
        return v;
    }
    float f32() {
        float v = 0.0f;
        if (has(4)) {
            std::memcpy(&v, data.data() + pos, 4);
            pos += 4;
        }
        return v;
    }
    void skip(int64_t n) {
        if (n < 0 || !has(static_cast<size_t>(n))) {
            ok = false;
            return;
        }
        pos += static_cast<size_t>(n);
    }
};

// FUntypedBulkData with its payload inline.
void skip_bulk(Reader& r) {
    r.i32();  // flags
    r.i32();  // elements
    const int32_t size = r.i32();
    r.i32();  // file offset
    if (size > 0) r.skip(size);
}

// A receiver's own light map (FLightMap1D on ten receivers of the game; never an FLightMap2D).
void skip_lightmap(Reader& r) {
    const int32_t kind = r.i32();
    if (kind == 0) return;
    const int32_t guids = r.i32();
    if (guids < 0 || guids > 256) {
        r.ok = false;
        return;
    }
    r.skip(static_cast<int64_t>(guids) * 16);
    if (kind == 1) {
        r.i32();           // Owner
        skip_bulk(r);      // DirectionalSamples
        r.skip(4 * 12);    // four scale vectors
        skip_bulk(r);      // SimpleSamples
    } else if (kind == 2) {
        r.skip(4 * (4 + 12));  // four textures, each with its scale vector
        r.skip(16);            // CoordinateScale, CoordinateBias
    } else {
        r.ok = false;
    }
}

Vec3 unpack_normal(uint32_t packed, float* w = nullptr) {
    const auto c = [packed](int shift) { return static_cast<float>((packed >> shift) & 0xffu) / 127.5f - 1.0f; };
    if (w) *w = c(24);
    return Vec3(c(0), c(8), c(16));
}

// The native data after a DecalComponent's properties. True when it reads to the export's last byte.
bool read_receivers(const UPKPackage& pkg, size_t tail, size_t end, std::vector<DecalReceiver>& out) {
    out.clear();
    const auto& exports = pkg.get_exports();
    Reader r{pkg.get_data(), tail, end};
    const int32_t count = r.i32();
    if (!r.ok || count < 0 || count > 4096) return false;
    for (int32_t k = 0; k < count && r.ok; ++k) {
        DecalReceiver rec;
        rec.component = r.i32();
        const int32_t vertex_size = r.i32();
        const int32_t vertex_count = r.i32();
        if (!r.ok || vertex_size != 52 || vertex_count < 0 || !r.has(static_cast<size_t>(vertex_count) * 52)) return false;
        rec.vertices.resize(static_cast<size_t>(vertex_count));
        for (DecalVertex& v : rec.vertices) {
            v.position.x = r.f32();
            v.position.y = r.f32();
            v.position.z = r.f32();
            const uint32_t tangent_x = static_cast<uint32_t>(r.i32());
            const uint32_t tangent_z = static_cast<uint32_t>(r.i32());
            v.tangent = unpack_normal(tangent_x);
            float w = 1.0f;
            v.normal = unpack_normal(tangent_z, &w);
            v.tangent_sign = w < 0.0f ? -1.0f : 1.0f;
            v.uv[0][0] = r.f32();
            v.uv[0][1] = r.f32();
            v.lightmap_uv[0] = r.f32();
            v.lightmap_uv[1] = r.f32();
            v.uv[1][0] = r.f32();
            v.uv[1][1] = r.f32();
            v.uv[2][0] = r.f32();
            v.uv[2][1] = r.f32();
        }
        const int32_t index_size = r.i32();
        const int32_t index_count = r.i32();
        if (!r.ok || index_size != 2 || index_count < 0 || !r.has(static_cast<size_t>(index_count) * 2)) return false;
        rec.indices.resize(static_cast<size_t>(index_count));
        if (index_count > 0) std::memcpy(rec.indices.data(), r.data.data() + r.pos, static_cast<size_t>(index_count) * 2);
        r.pos += static_cast<size_t>(index_count) * 2;
        r.i32();  // NumTriangles
        skip_lightmap(r);
        if (!r.ok) return false;
        for (uint16_t index : rec.indices) {
            if (index >= rec.vertices.size()) return false;
        }
        if (rec.component > 0 && static_cast<size_t>(rec.component) <= exports.size()) {
            const std::string cls = pkg.get_export_class(exports[static_cast<size_t>(rec.component) - 1]);
            rec.on_bsp = cls == "ModelComponent";
            if (rec.on_bsp || cls.find("StaticMeshComponent") != std::string::npos) out.push_back(std::move(rec));
        }
    }
    return r.ok && r.pos == end;
}

}  // namespace

void extract_level_decals(const std::vector<std::shared_ptr<UPKPackage>>& packages, std::vector<LevelDecal>& out) {
    out.clear();
    size_t unread = 0, triangles = 0, without_receiver = 0, to_compute = 0;
    for (const auto& pkg_ptr : packages) {
        if (!pkg_ptr) continue;
        const UPKPackage& pkg = *pkg_ptr;
        const auto& exports = pkg.get_exports();
        for (size_t i = 0; i < exports.size(); ++i) {
            if (pkg.get_export_class(exports[i]) != "DecalActor") continue;
            UPropertyList actor;
            parse_export_properties(pkg, static_cast<int32_t>(i) + 1, actor);
            const int32_t component = prop_object(actor, "Decal");
            if (component <= 0 || static_cast<size_t>(component) > exports.size()) continue;
            const auto& exp = exports[static_cast<size_t>(component) - 1];
            if (exp.serial_offset < 0 || exp.serial_size <= 8) continue;
            const size_t start = static_cast<size_t>(exp.serial_offset);
            const size_t end = start + static_cast<size_t>(exp.serial_size);
            if (end > pkg.get_data().size()) continue;

            // A component export opens with its template owner class and its net index, then the
            // tags. The general guess at where the tags start slips on a few of these, so the
            // plain place is tried first and the read is checked by its reaching the export's end.
            LevelDecal decal;
            UPropertyList c;
            bool read = false;
            for (const size_t first : {start + 8, pkg.find_property_start(exp)}) {
                if (first < start || first >= end) continue;
                c.clear();
                const size_t tail = parse_property_tree(pkg, first, end, c, 0);
                if (read_receivers(pkg, tail, end, decal.receivers)) {
                    read = true;
                    break;
                }
            }
            if (!read) {
                ++unread;
                continue;
            }
            const int32_t material = prop_object(c, "DecalMaterial");
            if (material == 0) continue;  // a few decals of the game name none
            decal.package = package_name_of(pkg);
            decal.material_path = object_canonical_path(pkg, material);
            const auto vec = [&c](const char* name, const Vec3& fallback) {
                const UProperty* p = find_prop(c, name);
                return p ? Vec3(p->v[0], p->v[1], p->v[2]) : fallback;
            };
            decal.hit_location = vec("HitLocation", Vec3(0.0f, 0.0f, 0.0f));
            decal.hit_normal = vec("HitNormal", Vec3(0.0f, 0.0f, 0.0f));
            decal.hit_tangent = vec("HitTangent", Vec3(0.0f, 0.0f, 0.0f));
            decal.hit_binormal = vec("HitBinormal", Vec3(0.0f, 0.0f, 0.0f));
            decal.sort_order = prop_int(c, "SortOrder", 0);
            decal.hidden = prop_bool(actor, "bHidden", false) || prop_bool(c, "HiddenGame", false);
            decal.width = prop_float(c, "Width", 200.0f);
            decal.height = prop_float(c, "Height", 200.0f);
            decal.near_plane = prop_float(c, "NearPlane", 0.0f);
            decal.far_plane = prop_float(c, "FarPlane", 300.0f);
            decal.tile_x = prop_float(c, "TileX", 1.0f);
            decal.tile_y = prop_float(c, "TileY", 1.0f);
            decal.offset_x = prop_float(c, "OffsetX", 0.0f);
            decal.offset_y = prop_float(c, "OffsetY", 0.0f);
            decal.backface_angle = prop_float(c, "BackfaceAngle", 0.001f);
            decal.project_on_backfaces = prop_bool(c, "bProjectOnBackfaces", false);
            decal.flip_backface_direction = prop_bool(c, "bFlipBackfaceDirection", false);
            decal.project_on_bsp = prop_bool(c, "bProjectOnBSP", true);
            decal.project_on_static_meshes = prop_bool(c, "bProjectOnStaticMeshes", true);
            size_t own = 0;
            for (const DecalReceiver& r : decal.receivers) own += r.indices.size() / 3;
            if (own == 0) {
                if (!decal.receivers.empty() || decal.width <= 0.0f || decal.height <= 0.0f) {
                    ++without_receiver;  // its stored receivers are empty: nothing was clipped onto anything for it
                    continue;
                }
                decal.compute_receivers = true;  // none stored: the level's builder clips it
                ++to_compute;
            }
            triangles += own;
            out.push_back(std::move(decal));
        }
    }
    if (!out.empty() || unread > 0) {
        std::cout << "[Level] Decals: " << out.size() << " placed, " << triangles << " stored triangles; " << to_compute
                  << " store no receiver and are clipped here (" << without_receiver << " with empty receivers left out, " << unread
                  << " not read)" << std::endl;
    }
}

}  // namespace me
