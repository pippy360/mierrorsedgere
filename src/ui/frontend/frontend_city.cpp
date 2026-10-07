// The menu level's city: the StaticMeshActors of Maps/Menu/TdMainMenu.me1 with their baked
// light maps, flattened into world-space triangle batches for the reference renderer.

#include "frontend_internal.hpp"

#include "../../assets/package_manager.hpp"
#include "../../assets/texture_loader.hpp"
#include "../../assets/ue3_props.hpp"
#include "../../assets/upk_loader.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <unordered_map>

namespace me::fe {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kUnrealAngle = 2.0f * kPi / 65536.0f;

// FRotationMatrix: the actor's X, Y and Z axes in world space.
void rotation_axes(int32_t pitch, int32_t yaw, int32_t roll, Vec3 axes[3]) {
    const float sp = std::sin(pitch * kUnrealAngle), cp = std::cos(pitch * kUnrealAngle);
    const float sy = std::sin(yaw * kUnrealAngle), cy = std::cos(yaw * kUnrealAngle);
    const float sr = std::sin(roll * kUnrealAngle), cr = std::cos(roll * kUnrealAngle);
    axes[0] = Vec3{cp * cy, cp * sy, sp};
    axes[1] = Vec3{sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp};
    axes[2] = Vec3{-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp};
}

bool material_kind(const std::string& path, CityMaterial& out) {
    const std::string low = to_lower(path);
    if (low.find("m_citybuildings") != std::string::npos || low.find("mi_sp") != std::string::npos) {
        out = CityMaterial::Buildings;  // every MI_SP<nn>_01 is an instance of M_CityBuildings_01
    } else if (low.find("m_citybase") != std::string::npos) {
        out = CityMaterial::Base;
    } else if (low.find("m_cityreflection") != std::string::npos) {
        out = CityMaterial::Water;
    } else if (low.find("m_citywaves") != std::string::npos) {
        out = CityMaterial::Waves;
    } else if (low.find("m_skydome") != std::string::npos) {
        out = CityMaterial::Sky;
    } else {
        return false;
    }
    return true;
}

float srgb_decode(uint8_t v) {
    const float c = static_cast<float>(v) / 255.0f;
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// The FLightMap2D in a StaticMeshComponent's native data, after its properties:
//   int32 LODData count (1) { ShadowMaps[] ; ShadowVertexBuffers[] ; int32 light-map type (2 = 2D) ;
//     LightGuids[] ; 4 x { texture ref ; float3 ScaleVector } ; float2 CoordinateScale ; float2 CoordinateBias }
// Textures 0..2 are the directional coefficients, 3 is the simple light map.
bool load_lightmap(PackageManager& pm, const UPKPackage& pkg, int32_t component, size_t tail, LightMap& out) {
    const auto& exp = pkg.get_exports()[static_cast<size_t>(component - 1)];
    const size_t end = static_cast<size_t>(exp.serial_offset) + static_cast<size_t>(exp.serial_size);
    const auto& data = pkg.get_data();
    if (end > data.size()) return false;
    size_t pos = tail;
    auto i32 = [&]() {
        int32_t v = 0;
        if (pos + 4 <= end) std::memcpy(&v, data.data() + pos, 4);
        pos += 4;
        return v;
    };
    auto f32 = [&]() {
        float v = 0.0f;
        if (pos + 4 <= end) std::memcpy(&v, data.data() + pos, 4);
        pos += 4;
        return v;
    };
    if (i32() != 1) return false;                 // one LOD
    if (i32() != 0 || i32() != 0) return false;   // no shadow maps, no shadow vertex buffers
    if (i32() != 2) return false;                 // FLightMap2D
    const int32_t guids = i32();
    if (guids < 0 || guids > 64) return false;
    pos += static_cast<size_t>(guids) * 16;
    int32_t textures[4];
    float scales[4][3];
    for (int k = 0; k < 4; ++k) {
        textures[k] = i32();
        for (int c = 0; c < 3; ++c) scales[k][c] = f32();
    }
    out.scale[0] = f32();
    out.scale[1] = f32();
    out.bias[0] = f32();
    out.bias[1] = f32();
    if (pos > end) return false;

    // The three coefficient textures hold the light arriving along the three Half-Life 2 basis
    // directions, and a normal is lit by them in proportion to dot(N, basis)^2. For the normal
    // straight out of the surface, the only one these untextured materials have, that is a third
    // of each: the plain average. (Weighting by dot(N, basis) instead, 1/sqrt(3) each, makes the
    // buildings 1.73 times too bright against the sky; measured on retail frames.)
    // The result is kept at half resolution: 2048 texels across a city block are far finer than
    // a menu pixel, and three 2048 float images per component is a lot of memory.
    static float decode[256];
    static bool decode_ready = false;
    if (!decode_ready) {
        for (int i = 0; i < 256; ++i) decode[i] = srgb_decode(static_cast<uint8_t>(i));
        decode_ready = true;
    }
    const float weight = 1.0f / 3.0f;
    for (int k = 0; k < 3; ++k) {
        if (textures[k] <= 0) return false;
        SceneTexture tex;
        if (!load_texture2d(pm, pkg, textures[k], 4096, tex, nullptr) || tex.mips.empty()) return false;
        const TextureMip& mip = tex.mips.front();
        if (tex.format != TexFormat::DXT1 || mip.width < 2 || mip.height < 2 ||
            mip.data.size() < texture_mip_bytes(tex.format, mip.width, mip.height)) {
            return false;
        }
        const int w = mip.width / 2, h = mip.height / 2;
        if (k == 0) {
            out.w = w;
            out.h = h;
            out.rgb.assign(static_cast<size_t>(w) * h * 3, 0.0f);
        } else if (w != out.w || h != out.h) {
            return false;
        }
        const int bw = mip.width / 4, bh = mip.height / 4;
        for (int by = 0; by < bh; ++by) {
            for (int bx = 0; bx < bw; ++bx) {
                const uint8_t* b = mip.data.data() + (static_cast<size_t>(by) * bw + bx) * 8;
                const uint16_t c0 = static_cast<uint16_t>(b[0] | (b[1] << 8));
                const uint16_t c1 = static_cast<uint16_t>(b[2] | (b[3] << 8));
                float pal[4][3];
                auto unpack = [&](uint16_t c, float* o) {
                    o[0] = decode[((c >> 11) & 31) * 255 / 31];
                    o[1] = decode[((c >> 5) & 63) * 255 / 63];
                    o[2] = decode[(c & 31) * 255 / 31];
                };
                // The palette is interpolated in the texture's own (sRGB) space, then decoded.
                uint8_t e0[3] = {static_cast<uint8_t>(((c0 >> 11) & 31) * 255 / 31), static_cast<uint8_t>(((c0 >> 5) & 63) * 255 / 63),
                                 static_cast<uint8_t>((c0 & 31) * 255 / 31)};
                uint8_t e1[3] = {static_cast<uint8_t>(((c1 >> 11) & 31) * 255 / 31), static_cast<uint8_t>(((c1 >> 5) & 63) * 255 / 63),
                                 static_cast<uint8_t>((c1 & 31) * 255 / 31)};
                unpack(c0, pal[0]);
                unpack(c1, pal[1]);
                for (int c = 0; c < 3; ++c) {
                    if (c0 > c1) {
                        pal[2][c] = decode[(2 * e0[c] + e1[c]) / 3];
                        pal[3][c] = decode[(e0[c] + 2 * e1[c]) / 3];
                    } else {
                        pal[2][c] = decode[(e0[c] + e1[c]) / 2];
                        pal[3][c] = 0.0f;
                    }
                }
                const uint32_t bits = static_cast<uint32_t>(b[4]) | (static_cast<uint32_t>(b[5]) << 8) |
                                      (static_cast<uint32_t>(b[6]) << 16) | (static_cast<uint32_t>(b[7]) << 24);
                // Each 4x4 block covers 2x2 output texels: average its quadrants.
                for (int q = 0; q < 4; ++q) {
                    const int qx = q & 1, qy = q >> 1;
                    float sum[3] = {0.0f, 0.0f, 0.0f};
                    for (int j = 0; j < 4; ++j) {
                        const int px = qx * 2 + (j & 1), py = qy * 2 + (j >> 1);
                        const float* c = pal[(bits >> (2 * (py * 4 + px))) & 3];
                        sum[0] += c[0];
                        sum[1] += c[1];
                        sum[2] += c[2];
                    }
                    float* o = &out.rgb[(static_cast<size_t>(by * 2 + qy) * w + (bx * 2 + qx)) * 3];
                    for (int c = 0; c < 3; ++c) o[c] += sum[c] * 0.25f * scales[k][c] * weight;
                }
            }
        }
    }
    return out.valid();
}

}  // namespace

bool load_city(PackageManager& pm, const std::shared_ptr<UPKPackage>& menu_map, City& out, std::vector<std::string>& warnings) {
    const UPKPackage& pkg = *menu_map;
    std::unordered_map<std::string, StaticMeshAsset> meshes;
    pkg.extract_static_meshes(meshes);

    const auto& exports = pkg.get_exports();
    for (size_t i = 0; i < exports.size(); ++i) {
        const std::string cls = pkg.get_export_class(exports[i]);
        const int32_t index = static_cast<int32_t>(i + 1);

        if (cls == "DirectionalLight") {
            UPropertyList props;
            parse_export_properties(pkg, index, props);
            if (const UProperty* rot = find_prop(props, "Rotation")) {
                Vec3 axes[3];
                rotation_axes(rot->vi[0], rot->vi[1], rot->vi[2], axes);
                out.sun_dir = axes[0] * -1.0f;  // the light shines along its X axis
            }
            continue;
        }
        if (cls == "WorldInfo") {
            UPropertyList props;
            parse_export_properties(pkg, index, props);
            if (const UProperty* pp = find_prop(props, "DefaultPostProcessSettings")) {
                out.bloom_scale = prop_float(pp->fields, "Bloom_Scale", 0.0f);
                if (const UProperty* curves = find_prop(pp->fields, "Curves")) {
                    for (const UProperty& f : curves->fields) {
                        if (f.array_index < 0 || f.array_index > 15) continue;
                        float (*dst)[3] = f.name == "Ms" ? out.curve_m : (f.name == "Bs" ? out.curve_b : nullptr);
                        if (!dst) continue;
                        for (int c = 0; c < 3; ++c) dst[f.array_index][c] = f.v[c];
                    }
                }
            }
            continue;
        }
        if (cls == "SceneCaptureReflectActor") {
            UPropertyList props;
            parse_export_properties(pkg, index, props);
            if (const UProperty* loc = find_prop(props, "Location")) out.water_z = loc->v[2];
            continue;
        }
        if (cls != "StaticMeshActor") continue;

        UPropertyList actor;
        parse_export_properties(pkg, index, actor);
        if (prop_bool(actor, "bHidden", false)) continue;
        const int32_t component = prop_object(actor, "StaticMeshComponent");
        if (component <= 0) continue;
        UPropertyList comp;
        const size_t tail = parse_export_properties(pkg, component, comp);
        if (prop_bool(comp, "HiddenGame", false)) continue;
        const int32_t mesh_ref = prop_object(comp, "StaticMesh");
        if (mesh_ref == 0) continue;
        auto it = meshes.find(to_lower(object_canonical_path(pkg, mesh_ref)));
        if (it == meshes.end()) {
            warnings.push_back("city mesh " + object_full_path(pkg, mesh_ref) + " could not be read");
            continue;
        }
        const StaticMeshAsset& mesh = it->second;

        Vec3 location{0.0f, 0.0f, 0.0f};
        Vec3 scale{1.0f, 1.0f, 1.0f};
        Vec3 axes[3] = {Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}};
        if (const UProperty* p = find_prop(actor, "Location")) location = Vec3{p->v[0], p->v[1], p->v[2]};
        if (const UProperty* p = find_prop(actor, "Rotation")) rotation_axes(p->vi[0], p->vi[1], p->vi[2], axes);
        if (const UProperty* p = find_prop(actor, "DrawScale3D")) scale = Vec3{p->v[0], p->v[1], p->v[2]};
        const float draw_scale = prop_float(actor, "DrawScale", 1.0f);
        scale = scale * draw_scale;

        int lightmap = -1;
        {
            LightMap lm;
            if (load_lightmap(pm, pkg, component, tail, lm)) {
                lightmap = static_cast<int>(out.lightmaps.size());
                out.lightmaps.push_back(std::move(lm));
            }
        }

        const UProperty* overrides = find_prop(comp, "Materials");
        const bool has_uv2 = mesh.uv_extra.size() == mesh.triangles.size() * 4;
        for (size_t e = 0; e < mesh.elements.size(); ++e) {
            const StaticMeshElement& el = mesh.elements[e];
            std::string material = el.material;
            if (overrides && e < overrides->ints.size() && overrides->ints[e] != 0) {
                material = object_canonical_path(pkg, overrides->ints[e]);
            }
            CityBatch batch;
            if (!material_kind(material, batch.material)) {
                warnings.push_back("city material " + material + " is not one the menu renderer knows");
                continue;
            }
            batch.mesh = mesh.name;
            batch.lightmap = lightmap;
            batch.tris.reserve(el.vertex_count);
            for (uint32_t v = el.first_vertex; v < el.first_vertex + el.vertex_count && v < mesh.triangles.size(); ++v) {
                const Vertex& src = mesh.triangles[v];
                CityVertex dst;
                const Vec3 p{src.position.x * scale.x, src.position.y * scale.y, src.position.z * scale.z};
                dst.pos = location + axes[0] * p.x + axes[1] * p.y + axes[2] * p.z;
                dst.normal = (axes[0] * src.normal.x + axes[1] * src.normal.y + axes[2] * src.normal.z).normalized();
                dst.uv[0][0] = src.u;
                dst.uv[0][1] = src.v;
                dst.uv[1][0] = src.u2;
                dst.uv[1][1] = src.v2;
                if (has_uv2) {
                    dst.uv[2][0] = mesh.uv_extra[static_cast<size_t>(v) * 4];
                    dst.uv[2][1] = mesh.uv_extra[static_cast<size_t>(v) * 4 + 1];
                }
                dst.color[0] = static_cast<float>(src.color & 0xFF) / 255.0f;
                dst.color[1] = static_cast<float>((src.color >> 8) & 0xFF) / 255.0f;
                dst.color[2] = static_cast<float>((src.color >> 16) & 0xFF) / 255.0f;
                batch.tris.push_back(dst);
            }
            if (batch.tris.size() >= 3) out.batches.push_back(std::move(batch));
        }
    }

    auto find_texture = [&](const char* name) -> int32_t {
        const std::string want = to_lower(name);
        for (size_t i = 0; i < exports.size(); ++i) {
            if (pkg.get_export_class(exports[i]) == "Texture2D" && to_lower(exports[i].object_name) == want) return static_cast<int32_t>(i + 1);
        }
        return 0;
    };
    auto load = [&](const char* name, Image& img) {
        SceneTexture tex;
        const int32_t idx = find_texture(name);
        if (idx <= 0 || !load_texture2d(pm, pkg, idx, 4096, tex, nullptr) || tex.mips.empty()) {
            warnings.push_back(std::string("city texture ") + name + " not found");
            return;
        }
        if (!decode_texture_mip(tex.mips.front(), tex.format, img)) {
            warnings.push_back(std::string("city texture ") + name + " is in a format the menu cannot decode");
            return;
        }
        img.wrap_x = tex.address_x != TexAddress::Clamp;
        img.wrap_y = tex.address_y != TexAddress::Clamp;
        img.srgb = tex.srgb;
    };
    load("T_CityFade_01_A", out.fade);
    load("T_Skydome_Menu", out.sky);
    load("T_Waves_01_A", out.waves);
    return out.valid();
}

}  // namespace me::fe
