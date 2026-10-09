#include "level_lightmaps.hpp"

#include "package_manager.hpp"
#include "texture_loader.hpp"
#include "ue3_props.hpp"
#include "upk_loader.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iostream>
#include <thread>

namespace me {

uint32_t pack_rgb9e5(const Vec3& rgb) {
    // The largest value the format holds: (2^9 - 1) / 2^9 * 2^16.
    constexpr float kMax = 65408.0f;
    const float r = std::clamp(rgb.x, 0.0f, kMax);
    const float g = std::clamp(rgb.y, 0.0f, kMax);
    const float b = std::clamp(rgb.z, 0.0f, kMax);
    const float top = std::max({r, g, b});
    if (!(top > 0.0f)) return 0u;
    // Exponent so that the largest channel's mantissa is below 2^9: value = mantissa * 2^(e - 15 - 9).
    int e = std::max(-16, static_cast<int>(std::floor(std::log2(top)))) + 1 + 15;
    float scale = std::exp2(static_cast<float>(e - 15 - 9));
    if (static_cast<int>(std::lround(top / scale)) == 512) {
        ++e;
        scale *= 2.0f;
    }
    e = std::clamp(e, 0, 31);
    scale = std::exp2(static_cast<float>(e - 15 - 9));
    const auto mantissa = [scale](float v) { return static_cast<uint32_t>(std::clamp<long>(std::lround(v / scale), 0, 511)); };
    return mantissa(r) | (mantissa(g) << 9) | (mantissa(b) << 18) | (static_cast<uint32_t>(e) << 27);
}

namespace {

// A cursor over one export's bytes that never runs past them.
struct Tail {
    const uint8_t* data = nullptr;
    size_t pos = 0;
    size_t end = 0;
    bool ok = true;

    bool need(size_t n) {
        if (pos + n > end) ok = false;
        return ok;
    }
    int32_t i32() {
        int32_t v = 0;
        if (need(4)) std::memcpy(&v, data + pos, 4);
        pos += 4;
        return v;
    }
    float f32() {
        float v = 0.0f;
        if (need(4)) std::memcpy(&v, data + pos, 4);
        pos += 4;
        return v;
    }
    Vec3 vec3() {
        const float x = f32(), y = f32(), z = f32();
        return Vec3(x, y, z);
    }
    void skip(size_t n) {
        need(n);
        pos += n;
    }
};

// The FLightMap2D at the cursor, after its type: the lights baked into it, four textures each
// followed by its scale (the three directional coefficients, then the simple light map, unused:
// the game runs with DirectionalLightmaps on), and the rectangle of the atlas it has.
bool read_lightmap_2d(const UPKPackage& pkg, Tail& t, ActorLightMap& out) {
    const int32_t guids = t.i32();
    if (guids < 0 || guids > 1024) return false;
    t.skip(static_cast<size_t>(guids) * 16);
    int32_t textures[4] = {0, 0, 0, 0};
    Vec3 scales[4];
    for (int k = 0; k < 4; ++k) {
        textures[k] = t.i32();
        scales[k] = t.vec3();
    }
    const float su = t.f32(), sv = t.f32(), bu = t.f32(), bv = t.f32();
    if (!t.ok) return false;
    const size_t export_count = pkg.get_exports().size();
    for (int k = 0; k < 3; ++k) {
        if (textures[k] <= 0 || static_cast<size_t>(textures[k]) > export_count) return true;  // read, but no use
        out.textures[k] = textures[k];
        out.scale[k] = scales[k];
    }
    out.coord_scale[0] = su;
    out.coord_scale[1] = sv;
    out.coord_bias[0] = bu;
    out.coord_bias[1] = bv;
    out.package_path = pkg.get_file_path();
    out.kind = ActorLightMap::Kind::Texture;
    return true;
}

}  // namespace

ActorLightMap read_component_lightmap(const UPKPackage& pkg, int32_t component_export_1based) {
    ActorLightMap out;
    const auto& exports = pkg.get_exports();
    if (component_export_1based <= 0 || static_cast<size_t>(component_export_1based) > exports.size()) return out;
    const auto& exp = exports[static_cast<size_t>(component_export_1based - 1)];
    const auto& bytes = pkg.get_data();

    UPropertyList props;
    Tail t;
    t.data = bytes.data();
    t.pos = parse_export_properties(pkg, component_export_1based, props);
    t.end = std::min(bytes.size(), static_cast<size_t>(exp.serial_offset) + static_cast<size_t>(exp.serial_size));
    if (t.pos == 0 || t.pos >= t.end) return out;

    // LODData: only LOD 0 is drawn here, and it comes first.
    const int32_t lods = t.i32();
    if (lods < 1 || lods > 8) return out;
    const int32_t shadow_maps = t.i32();
    if (shadow_maps < 0 || shadow_maps > 4096) return out;
    t.skip(static_cast<size_t>(shadow_maps) * 4);
    const int32_t shadow_vertex_buffers = t.i32();
    if (shadow_vertex_buffers < 0 || shadow_vertex_buffers > 4096) return out;
    t.skip(static_cast<size_t>(shadow_vertex_buffers) * 4);

    const int32_t kind = t.i32();
    if (!t.ok || (kind != 1 && kind != 2)) return out;
    if (kind == 2) {
        read_lightmap_2d(pkg, t, out);
        return out;
    }
    const int32_t guids = t.i32();  // the lights baked into it
    if (guids < 0 || guids > 1024) return out;
    t.skip(static_cast<size_t>(guids) * 16);

    // FLightMap1D: Owner, the directional samples as bulk data (flags, count, bytes, file offset,
    // then the samples in place), four scale vectors, the simple samples.
    t.skip(4);  // Owner
    const int32_t flags = t.i32();
    const int32_t count = t.i32();
    const int32_t size = t.i32();
    const int32_t offset = t.i32();
    if (!t.ok || flags != 0 || count <= 0 || size != count * 12 || static_cast<size_t>(offset) != t.pos) return out;
    const size_t samples_at = t.pos;
    t.skip(static_cast<size_t>(size));
    Vec3 scales[3];
    for (auto& s : scales) s = t.vec3();
    if (!t.ok) return out;

    // A sample is three FColors (B, G, R, A bytes). BasePassVertexShader.usf: pow(c.rgb, 2.2) * scale.
    static float decode[256];
    static bool decode_ready = false;
    if (!decode_ready) {
        for (int i = 0; i < 256; ++i) decode[i] = std::pow(static_cast<float>(i) / 255.0f, 2.2f);
        decode_ready = true;
    }
    out.vertex_samples.resize(static_cast<size_t>(count) * 3);
    for (int32_t v = 0; v < count; ++v) {
        const uint8_t* s = bytes.data() + samples_at + static_cast<size_t>(v) * 12;
        for (int k = 0; k < 3; ++k) {
            const uint8_t* c = s + k * 4;
            const Vec3 linear(decode[c[2]] * scales[k].x, decode[c[1]] * scales[k].y, decode[c[0]] * scales[k].z);
            out.vertex_samples[static_cast<size_t>(v) * 3 + static_cast<size_t>(k)] = pack_rgb9e5(linear);
        }
    }
    out.kind = ActorLightMap::Kind::Vertex;
    return out;
}

void read_model_lightmaps(const UPKPackage& pkg, int32_t model_export_1based, std::vector<ActorLightMap>& maps,
                          std::vector<int32_t>& node_map) {
    const auto& exports = pkg.get_exports();
    const auto& bytes = pkg.get_data();
    for (size_t i = 0; i < exports.size(); ++i) {
        if (pkg.get_export_class(exports[i]) != "ModelComponent") continue;
        UPropertyList props;
        Tail t;
        t.data = bytes.data();
        t.pos = parse_export_properties(pkg, static_cast<int32_t>(i) + 1, props);
        t.end = std::min(bytes.size(), static_cast<size_t>(exports[i].serial_offset) + static_cast<size_t>(exports[i].serial_size));
        if (t.pos == 0 || t.pos >= t.end) continue;

        // UModelComponent::Serialize: Model, ZoneIndex, Elements, ComponentIndex, Nodes.
        if (t.i32() != model_export_1based) continue;
        t.skip(4);
        const int32_t elements = t.i32();
        if (!t.ok || elements < 0 || elements > 65536) continue;
        for (int32_t e = 0; e < elements && t.ok; ++e) {
            // FModelElement: LightMap, Component, Material, Nodes (16-bit), ShadowMaps (light guid ->
            // shadow map), IrrelevantLights (guids).
            ActorLightMap lm;
            const int32_t kind = t.i32();
            if (kind == 2) {
                if (!read_lightmap_2d(pkg, t, lm)) break;
            } else if (kind != 0) {
                break;  // a per-vertex light map: no BSP has one
            }
            t.skip(8);
            const int32_t nodes = t.i32();
            if (!t.ok || nodes < 0 || !t.need(static_cast<size_t>(nodes) * 2)) break;
            const size_t nodes_at = t.pos;
            t.skip(static_cast<size_t>(nodes) * 2);
            const int32_t shadow_maps = t.i32();
            if (!t.ok || shadow_maps < 0 || shadow_maps > 4096) break;
            t.skip(static_cast<size_t>(shadow_maps) * 20);
            const int32_t irrelevant = t.i32();
            if (!t.ok || irrelevant < 0 || irrelevant > 65536) break;
            t.skip(static_cast<size_t>(irrelevant) * 16);
            if (!t.ok) break;

            int32_t index = -1;
            if (lm.kind == ActorLightMap::Kind::Texture) {
                index = static_cast<int32_t>(maps.size());
                maps.push_back(std::move(lm));
            }
            for (int32_t n = 0; n < nodes; ++n) {
                uint16_t node = 0;
                std::memcpy(&node, bytes.data() + nodes_at + static_cast<size_t>(n) * 2, 2);
                if (node >= node_map.size()) node_map.resize(static_cast<size_t>(node) + 1, -1);
                node_map[node] = index;
            }
        }
    }
}

int32_t LightMapSets::index_of(const std::string& package_path, const int32_t textures[3]) {
    const std::string key = package_path + "#" + std::to_string(textures[0]) + "," + std::to_string(textures[1]) + "," +
                            std::to_string(textures[2]);
    const auto it = by_key_.find(key);
    if (it != by_key_.end()) return it->second;
    Set set;
    set.package_path = package_path;
    for (int k = 0; k < 3; ++k) set.textures[k] = textures[k];
    const int32_t index = static_cast<int32_t>(sets_.size());
    sets_.push_back(std::move(set));
    by_key_.emplace(key, index);
    return index;
}

void LightMapSets::load(PackageManager& pm, const std::vector<std::shared_ptr<UPKPackage>>& packages,
                        std::vector<SceneTexture>& out) const {
    out.assign(sets_.size() * 3, SceneTexture{});
    if (sets_.empty()) return;
    std::unordered_map<std::string, const UPKPackage*> by_path;
    for (const auto& p : packages) {
        if (p) by_path.emplace(p->get_file_path(), p.get());
    }

    // Decompressing the mips is the cost; the textures are independent of each other.
    const size_t total = out.size();
    const unsigned workers = std::max(1u, std::min(std::thread::hardware_concurrency(), 16u));
    std::vector<std::thread> pool;
    std::atomic<size_t> next{0};
    std::atomic<size_t> loaded{0};
    for (unsigned w = 0; w < workers; ++w) {
        pool.emplace_back([&]() {
            for (size_t i = next.fetch_add(1); i < total; i = next.fetch_add(1)) {
                const Set& set = sets_[i / 3];
                const auto pkg = by_path.find(set.package_path);
                if (pkg == by_path.end()) continue;
                SceneTexture tex;
                // Light maps keep their full size: their texels are the lighting's resolution.
                if (!load_texture2d(pm, *pkg->second, set.textures[i % 3], 4096, tex, nullptr) || !tex.valid()) continue;
                tex.srgb = true;  // sampled through an sRGB lookup on PC (BasePassPixelShader.usf)
                tex.address_x = TexAddress::Clamp;
                tex.address_y = TexAddress::Clamp;
                out[i] = std::move(tex);
                loaded.fetch_add(1);
            }
        });
    }
    for (auto& th : pool) th.join();
    size_t bytes = 0;
    for (const auto& tex : out) {
        for (const auto& mip : tex.mips) bytes += mip.data.size();
    }
    std::cout << "[Level] Light maps: " << sets_.size() << " texture sets, " << loaded.load() << "/" << total
              << " textures loaded (" << bytes / (1024 * 1024) << " MB)" << std::endl;
}

}  // namespace me
