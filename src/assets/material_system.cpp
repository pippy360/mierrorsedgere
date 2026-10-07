// -----------------------------------------------------------------------------
// Mirror's Edge material system: UE3 material graph -> Metal Shading Language.
//
// Pipeline (per level):
//   1. Resolve every referenced material object across packages (level packages,
//      their sub-levels and the original content packages in CookedPC).
//   2. Walk MaterialInstanceConstant chains up to the root UMaterial, collecting
//      scalar / vector / texture parameter overrides and the static permutation
//      (static switches + static component masks) serialized in each MIC.
//   3. Translate the root material's expression graph exactly like UE3's
//      FHLSLMaterialTranslator (UnMaterial.cpp) did for MaterialTemplate.usf:
//      ForceCast / arithmetic typing rules, input component masks, texture
//      unpack (UnpackMin/UnpackMax) baking, tangent-space camera/reflection
//      vectors, etc.  Each distinct translation becomes one MSL fragment shader.
//   4. Load all referenced Texture2D / TextureCube objects (mips may live in
//      "separate file" bulk data inside the original content packages).
// -----------------------------------------------------------------------------

#include "material_system.hpp"

#include "package_manager.hpp"
#include "texture_loader.hpp"
#include "ue3_props.hpp"
#include "upk_loader.hpp"
#include "../renderer/sun_shadow.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace me {

namespace {

constexpr const char* kDefaultMaterialPath = "EngineMaterials.DefaultMaterial";
constexpr const char* kDefaultTexturePath = "EngineResources.DefaultTexture";
constexpr const char* kDefaultTextureCubePath = "EngineResources.DefaultTextureCube";

// FLinearColor(FColor) uses a pow(x, 2.2) table in UE3 (PowOneOver255Table).
inline float color_byte_to_linear(float unorm) { return std::pow(std::clamp(unorm, 0.0f, 1.0f), 2.2f); }

// Default FColor(128,128,128) for DiffuseColor / SpecularColor converted to linear.
const float kDefaultGrey = std::pow(128.0f / 255.0f, 2.2f);

// =============================================================================
// Object resolution across packages
// =============================================================================
struct ObjRef {
    std::shared_ptr<UPKPackage> pkg;
    int32_t index = 0;  // 1-based export index in pkg
    [[nodiscard]] bool valid() const { return pkg != nullptr && index > 0; }
};

bool export_has_data(const UPKPackage& pkg, int32_t index) {
    const auto& ex = pkg.get_exports();
    if (index <= 0 || static_cast<size_t>(index) > ex.size()) return false;
    const auto& e = ex[static_cast<size_t>(index) - 1];
    return e.serial_size > 0 && e.serial_offset > 0;
}

class Resolver {
public:
    explicit Resolver(PackageManager& pm) : pm_(pm) {}

    // Finds an object (with serialized data) by its full path, e.g.
    // "B_BD_Commercial.BD_Commercial_01.M_BD_15_01".
    ObjRef by_path(const std::string& path) {
        if (path.empty()) return {};
        std::string key = to_lower(path);
        auto it = cache_.find(key);
        if (it != cache_.end()) return it->second;
        ObjRef r = lookup(key);
        cache_[key] = r;
        return r;
    }

    // Resolves an object reference (export > 0, import < 0) stored inside `from.pkg`.
    ObjRef by_ref(const ObjRef& from, int32_t ref) {
        if (!from.pkg || ref == 0) return {};
        if (ref > 0 && export_has_data(*from.pkg, ref)) return ObjRef{from.pkg, ref};
        return by_path(object_canonical_path(*from.pkg, ref));
    }

private:
    ObjRef lookup(const std::string& key) {
        auto pkgs = pm_.loaded_packages();
        std::sort(pkgs.begin(), pkgs.end(), [](const auto& a, const auto& b) {
            return a->get_file_path() < b->get_file_path();
        });
        for (const auto& pkg : pkgs) {
            int32_t idx = pm_.find_export(*pkg, key);
            if (idx > 0 && export_has_data(*pkg, idx)) return ObjRef{pkg, idx};
        }
        // Not cooked into any loaded package: load the object's own package.
        size_t dot = key.find('.');
        std::string outer = dot == std::string::npos ? key : key.substr(0, dot);
        if (auto pkg = pm_.load(outer)) {
            int32_t idx = pm_.find_export(*pkg, key);
            if (idx > 0 && export_has_data(*pkg, idx)) return ObjRef{pkg, idx};
        }
        return {};
    }

    PackageManager& pm_;
    std::unordered_map<std::string, ObjRef> cache_;
};

// =============================================================================
// Material graph (UMaterial + lazily parsed UMaterialExpression nodes)
// =============================================================================
struct ExprInput {
    int32_t expr = 0;  // object reference (export index in the material's package)
    bool mask = false;
    bool ch[4] = {false, false, false, false};
    [[nodiscard]] bool connected() const { return expr != 0; }
};

int32_t field_int(const UPropertyList& fields, const char* name) {
    const UProperty* p = find_prop(fields, name);
    if (!p) return 0;
    if (p->type == "BoolProperty") return p->b ? 1 : 0;
    return p->i;
}

ExprInput read_input_struct(const UPropertyList& fields) {
    ExprInput in;
    in.expr = prop_object(fields, "Expression");
    in.mask = field_int(fields, "Mask") != 0;
    in.ch[0] = field_int(fields, "MaskR") != 0;
    in.ch[1] = field_int(fields, "MaskG") != 0;
    in.ch[2] = field_int(fields, "MaskB") != 0;
    in.ch[3] = field_int(fields, "MaskA") != 0;
    return in;
}

ExprInput read_input(const UPropertyList& props, const char* name) {
    const UProperty* p = find_prop(props, name);
    if (!p || p->type != "StructProperty") return {};
    return read_input_struct(p->fields);
}

// Reads a ByteProperty enum either as its FName ("BLEND_Masked") or raw byte.
std::string enum_prop(const UPropertyList& props, const char* name, int* raw = nullptr) {
    const UProperty* p = find_prop(props, name);
    if (raw) *raw = p ? p->i : 0;
    if (!p) return "";
    if (!p->s.empty()) return p->s;
    return std::to_string(p->i);
}

struct ExprNode {
    std::string cls;  // class name without the "MaterialExpression" prefix
    UPropertyList props;
    ObjRef ref;
};

struct MaterialGraph {
    ObjRef ref;
    std::string path;
    UPropertyList props;
    std::unordered_map<int32_t, std::unique_ptr<ExprNode>> nodes;
    std::set<std::string> static_switch_names;
    std::set<std::string> static_mask_names;

    const ExprNode* node(int32_t idx) {
        auto it = nodes.find(idx);
        if (it != nodes.end()) return it->second.get();
        std::unique_ptr<ExprNode> n;
        if (idx > 0 && export_has_data(*ref.pkg, idx)) {
            n = std::make_unique<ExprNode>();
            n->ref = ObjRef{ref.pkg, idx};
            std::string cls = object_class_name(*ref.pkg, idx);
            static const std::string kPrefix = "MaterialExpression";
            if (cls.rfind(kPrefix, 0) == 0) cls = cls.substr(kPrefix.size());
            n->cls = cls;
            parse_export_properties(*ref.pkg, idx, n->props);
        }
        const ExprNode* raw = n.get();
        nodes.emplace(idx, std::move(n));
        return raw;
    }

    void collect_static_parameter_names() {
        const UProperty* exprs = find_prop(props, "Expressions");
        if (!exprs) return;
        for (int32_t idx : exprs->ints) {
            const ExprNode* n = node(idx);
            if (!n) continue;
            if (n->cls == "StaticSwitchParameter") {
                static_switch_names.insert(prop_name(n->props, "ParameterName", "None"));
            } else if (n->cls == "StaticComponentMaskParameter") {
                static_mask_names.insert(prop_name(n->props, "ParameterName", "None"));
            }
        }
    }
};

// =============================================================================
// Static permutation parameters (FStaticParameterSet in the MIC native tail)
// =============================================================================
struct StaticSwitchEntry {
    std::string name;
    bool value = false;
    bool override_ = false;
};
struct StaticMaskEntry {
    std::string name;
    std::array<bool, 4> rgba{false, false, false, false};
    bool override_ = false;
};
struct StaticSet {
    std::vector<StaticSwitchEntry> switches;
    std::vector<StaticMaskEntry> masks;
};

struct StaticParams {
    std::map<std::string, bool> switches;
    std::map<std::string, std::array<bool, 4>> masks;

    [[nodiscard]] std::string signature() const {
        std::string s;
        for (const auto& [k, v] : switches) s += k + "=" + (v ? "1" : "0") + ";";
        for (const auto& [k, v] : masks) {
            s += k + "=";
            for (bool b : v) s += b ? "1" : "0";
            s += ";";
        }
        return s;
    }
};

// UMaterialInstance::Serialize (v536) writes, when bHasStaticPermutationResource:
//   2 x { FMaterialResource (uniform expressions etc.), FStaticParameterSet }
// FStaticParameterSet = FGuid BaseMaterialId;
//                       TArray<{FName Name; UBOOL Value; UBOOL bOverride; FGuid Id}>        (32 bytes)
//                       TArray<{FName Name; UBOOL R,G,B,A; UBOOL bOverride; FGuid Id}>     (44 bytes)
// The FMaterialResource part contains polymorphic uniform expressions, so the
// set is located by a validated scan: counts must be small, bools must be 0/1
// and every name must be a static parameter of the base material.
bool scan_static_parameter_set(const UPKPackage& pkg, size_t from, size_t to, const std::set<std::string>& sw_names,
                               const std::set<std::string>& mask_names, StaticSet& out) {
    const auto& d = pkg.get_data();
    const auto& names = pkg.get_names();
    to = std::min(to, d.size());
    auto rd = [&](size_t o) {
        int32_t v = 0;
        std::memcpy(&v, d.data() + o, 4);
        return v;
    };
    auto name_at = [&](size_t o, std::string& s) {
        int32_t idx = rd(o);
        int32_t num = rd(o + 4);
        if (idx < 0 || static_cast<size_t>(idx) >= names.size() || num < 0 || num > 100000) return false;
        s = names[static_cast<size_t>(idx)];
        if (num > 0) s += "_" + std::to_string(num - 1);
        return true;
    };
    auto is_bool = [](int32_t v) { return v == 0 || v == 1; };

    for (size_t o = from; o + 24 <= to; ++o) {
        int32_t n1 = rd(o + 16);
        if (n1 < 0 || n1 > 64) continue;
        size_t q = o + 20;
        StaticSet set;
        bool ok = true;
        for (int32_t k = 0; k < n1; ++k) {
            if (q + 32 > to) { ok = false; break; }
            std::string nm;
            if (!name_at(q, nm) || sw_names.find(nm) == sw_names.end()) { ok = false; break; }
            int32_t val = rd(q + 8), ov = rd(q + 12);
            if (!is_bool(val) || !is_bool(ov)) { ok = false; break; }
            set.switches.push_back({nm, val != 0, ov != 0});
            q += 32;
        }
        if (!ok || q + 4 > to) continue;
        int32_t n2 = rd(q);
        if (n2 < 0 || n2 > 64) continue;
        q += 4;
        for (int32_t k = 0; k < n2; ++k) {
            if (q + 44 > to) { ok = false; break; }
            std::string nm;
            if (!name_at(q, nm) || mask_names.find(nm) == mask_names.end()) { ok = false; break; }
            StaticMaskEntry m;
            m.name = nm;
            for (int c = 0; c < 4; ++c) {
                int32_t v = rd(q + 8 + static_cast<size_t>(c) * 4);
                if (!is_bool(v)) ok = false;
                m.rgba[static_cast<size_t>(c)] = v != 0;
            }
            int32_t ov = rd(q + 24);
            if (!is_bool(ov)) ok = false;
            if (!ok) break;
            m.override_ = ov != 0;
            set.masks.push_back(m);
            q += 44;
        }
        if (!ok || n1 + n2 == 0) continue;
        out = std::move(set);
        return true;
    }
    return false;
}

// =============================================================================
// Shader code generation
// =============================================================================
struct Val {
    std::string code;
    int n = 0;  // number of float components (1..4), 0 = invalid
    [[nodiscard]] bool ok() const { return n > 0; }
};

std::string fmt_float(float f) {
    if (!std::isfinite(f)) f = 0.0f;
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(f));
    std::string s(buf);
    if (s.find_first_of(".eE") == std::string::npos) s += ".0";
    return s;
}

const char* type_name(int n) {
    static const char* kNames[5] = {"void", "float", "float2", "float3", "float4"};
    return kNames[std::clamp(n, 0, 4)];
}

Val vec_const(int n, const float* v) {
    if (n == 1) return {fmt_float(v[0]), 1};
    std::string s = std::string(type_name(n)) + "(";
    for (int i = 0; i < n; ++i) {
        if (i) s += ", ";
        s += fmt_float(v[i]);
    }
    s += ")";
    return {s, n};
}

Val scalar_const(float f) { return {fmt_float(f), 1}; }

bool is_swizzle_str(const std::string& s) {
    if (s.empty() || s.size() > 4) return false;
    for (char c : s) {
        if (c != 'x' && c != 'y' && c != 'z' && c != 'w') return false;
    }
    return true;
}

// Component selection with HLSL semantics (scalars broadcast, missing components are 0).
Val swizzle(const Val& v, const std::vector<int>& ch) {
    const int k = static_cast<int>(ch.size());
    if (!v.ok() || k == 0 || k > 4) return {};
    if (v.n == 1) {
        if (k == 1) return v;
        return {std::string(type_name(k)) + "(" + v.code + ")", k};
    }
    std::string base = v.code;
    std::string existing;
    size_t dot = base.rfind('.');
    if (dot != std::string::npos) {
        std::string suf = base.substr(dot + 1);
        if (is_swizzle_str(suf) && static_cast<int>(suf.size()) == v.n) {
            existing = suf;
            base = base.substr(0, dot);
        }
    }
    static const char kXYZW[] = "xyzw";
    auto comp = [&](int c) -> char { return existing.empty() ? kXYZW[c] : existing[static_cast<size_t>(c)]; };
    bool in_range = true;
    for (int c : ch) {
        if (c < 0 || c >= v.n) in_range = false;
    }
    if (in_range) {
        std::string s;
        for (int c : ch) s += comp(c);
        return {base + "." + s, k};
    }
    std::string s = std::string(type_name(k)) + "(";
    for (int i = 0; i < k; ++i) {
        if (i) s += ", ";
        int c = ch[static_cast<size_t>(i)];
        if (c >= 0 && c < v.n) {
            s += base + ".";
            s += comp(c);
        } else {
            s += "0.0";
        }
    }
    s += ")";
    return {s, k};
}

// UE3 ForceCast: scalars broadcast, wider vectors are truncated, narrower zero-padded.
Val cast_to(const Val& v, int n) {
    if (!v.ok() || v.n == n) return v;
    if (v.n == 1) return {std::string(type_name(n)) + "(" + v.code + ")", n};
    if (v.n > n) {
        std::vector<int> ch;
        for (int i = 0; i < n; ++i) ch.push_back(i);
        return swizzle(v, ch);
    }
    std::string s = std::string(type_name(n)) + "(" + v.code;
    for (int i = v.n; i < n; ++i) s += ", 0.0";
    s += ")";
    return {s, n};
}

struct TexSlotSpec {
    bool is_cube = false;
    bool is_param = false;
    std::string param;
    std::string default_path;  // fixed texture or the parameter's default texture
    TexDefault fallback = TexDefault::White;
};

struct UniformSlotSpec {
    std::string param;
    bool is_vector = false;
    std::array<float, 4> def{0.0f, 0.0f, 0.0f, 0.0f};
};

struct CompiledMaterial {
    std::string base_path;
    std::string source;  // fragment function with "@FN@" placeholder for the entry point name
    std::vector<TexSlotSpec> tex2d;
    std::vector<TexSlotSpec> texcube;
    std::vector<UniformSlotSpec> uniforms;
    MatBlendMode blend = MatBlendMode::Opaque;
    MatLightingModel lighting = MatLightingModel::Phong;
    bool two_sided = false;
    bool uses_scene_color = false;
    bool uses_scene_depth = false;
    std::vector<std::string> warnings;
    std::map<std::string, int> unknown_classes;
    int shader_index = -1;
};

MatBlendMode parse_blend(const UPropertyList& props) {
    int raw = 0;
    std::string s = enum_prop(props, "BlendMode", &raw);
    if (s == "BLEND_Masked" || s == "1") return MatBlendMode::Masked;
    if (s == "BLEND_Translucent" || s == "2") return MatBlendMode::Translucent;
    if (s == "BLEND_Additive" || s == "3") return MatBlendMode::Additive;
    if (s == "BLEND_Modulate" || s == "4") return MatBlendMode::Modulate;
    return MatBlendMode::Opaque;
}

MatLightingModel parse_lighting(const UPropertyList& props) {
    int raw = 0;
    std::string s = enum_prop(props, "LightingModel", &raw);
    if (s == "MLM_NonDirectional" || s == "1") return MatLightingModel::NonDirectional;
    if (s == "MLM_Unlit" || s == "2") return MatLightingModel::Unlit;
    if (s == "MLM_Custom" || s == "4") return MatLightingModel::Custom;
    return MatLightingModel::Phong;  // MLM_Phong, MLM_SHPRT
}

TexDefault guess_texture_default(const std::string& path, const TextureInfo* info) {
    if (info && info->is_normal_map) return TexDefault::FlatNormal;
    std::string low = to_lower(path);
    size_t dot = low.rfind('.');
    std::string leaf = dot == std::string::npos ? low : low.substr(dot + 1);
    auto ends_with = [&](const char* s) {
        size_t n = std::strlen(s);
        return leaf.size() >= n && leaf.compare(leaf.size() - n, n, s) == 0;
    };
    if (ends_with("_n") || ends_with("_nm") || leaf.find("normal") != std::string::npos) return TexDefault::FlatNormal;
    if (ends_with("_e") || ends_with("_em") || leaf.find("emissive") != std::string::npos ||
        leaf.find("glow") != std::string::npos || leaf.find("black") != std::string::npos) {
        return TexDefault::Black;
    }
    return TexDefault::White;
}

class GraphCompiler {
public:
    GraphCompiler(Resolver& res, MaterialGraph& g, const StaticParams& statics, CompiledMaterial& out)
        : res_(res), g_(g), statics_(statics), out_(out) {}

    void run();

private:
    void begin_phase(char prefix, std::vector<std::string>* stmts) {
        prefix_ = prefix;
        stmts_ = stmts;
        cache_.clear();
        visiting_.clear();
    }

    void warn(const std::string& w) {
        if (out_.warnings.size() < 32) out_.warnings.push_back(w);
    }

    Val emit(int n, const std::string& expr) {
        std::string name = std::string("l_") + prefix_ + std::to_string(counter_++);
        stmts_->push_back(std::string(indent_) + type_name(n) + " " + name + " = " + expr + ";");
        return {name, n};
    }

    int arith_type(int a, int b) {
        if (a == b) return a;
        if (a == 1) return b;
        if (b == 1) return a;
        warn("arithmetic between float" + std::to_string(a) + " and float" + std::to_string(b) + " (truncated)");
        return std::min(a, b);
    }

    // CoerceParameter: scalars broadcast silently; vector size mismatches are UE3
    // compile errors, handled leniently by truncation / zero padding.
    Val coerce(const Val& v, int n) {
        if (!v.ok() || v.n == n || v.n == 1) return cast_to(v, n);
        warn("coercion float" + std::to_string(v.n) + " -> float" + std::to_string(n));
        return cast_to(v, n);
    }

    Val arith(const Val& a, const Val& b, const char* op) {
        int n = arith_type(a.n, b.n);
        Val A = (a.n == 1) ? a : cast_to(a, n);
        Val B = (b.n == 1) ? b : cast_to(b, n);
        return emit(n, "(" + A.code + " " + op + " " + B.code + ")");
    }

    Val func1(const char* fn, const Val& v) {
        if (!v.ok()) return {};
        return emit(v.n, std::string(fn) + "(" + v.code + ")");
    }

    Val node(int32_t idx);
    Val input(const ExprInput& in);
    Val input(const UPropertyList& props, const char* name) { return input(read_input(props, name)); }
    Val compile_expr(const ExprNode& nd);
    Val texture_sample(const ExprNode& nd, int32_t tex_ref, const std::string& param, bool is_param, bool param_cube,
                       const ExprInput& coords);
    int tex_slot(bool cube, bool is_param, const std::string& param, const std::string& path, TexDefault fb);
    int uniform_slot(bool vec, const std::string& name, const std::array<float, 4>& def);
    Val material_input(const char* name, int n, const std::array<float, 4>& def);

    bool static_switch(const std::string& name, bool def) const {
        auto it = statics_.switches.find(name);
        return it == statics_.switches.end() ? def : it->second;
    }
    std::array<bool, 4> static_mask(const std::string& name, const std::array<bool, 4>& def) const {
        auto it = statics_.masks.find(name);
        return it == statics_.masks.end() ? def : it->second;
    }

    Resolver& res_;
    MaterialGraph& g_;
    const StaticParams& statics_;
    CompiledMaterial& out_;
    std::vector<std::string>* stmts_ = nullptr;
    const char* indent_ = "    ";
    char prefix_ = 'm';
    int counter_ = 0;
    int depth_ = 0;
    bool translucent_ = false;
    std::unordered_map<int32_t, Val> cache_;
    std::unordered_set<int32_t> visiting_;
    std::unordered_map<std::string, int> tex_slot_keys_;
    std::unordered_map<std::string, int> uniform_keys_;
};

Val GraphCompiler::node(int32_t idx) {
    if (idx <= 0) {
        warn("expression reference " + std::to_string(idx) + " is not a local export");
        return {};
    }
    auto it = cache_.find(idx);
    if (it != cache_.end()) return it->second;
    if (visiting_.count(idx) || depth_ > 128) {
        warn("expression graph cycle / depth limit");
        return {};
    }
    const ExprNode* n = g_.node(idx);
    if (!n) {
        warn("missing expression export " + std::to_string(idx));
        return {};
    }
    visiting_.insert(idx);
    ++depth_;
    Val v = compile_expr(*n);
    --depth_;
    visiting_.erase(idx);
    cache_[idx] = v;
    return v;
}

Val GraphCompiler::input(const ExprInput& in) {
    if (!in.connected()) return {};
    Val v = node(in.expr);
    if (!v.ok()) return {};
    if (in.mask) {
        std::vector<int> ch;
        for (int i = 0; i < 4; ++i) {
            if (in.ch[i]) ch.push_back(i);
        }
        if (!ch.empty()) v = swizzle(v, ch);
    }
    return v;
}

int GraphCompiler::tex_slot(bool cube, bool is_param, const std::string& param, const std::string& path,
                            TexDefault fb) {
    std::string key = std::string(cube ? "c|" : "t|") + (is_param ? "p|" + param : "f|" + to_lower(path));
    auto it = tex_slot_keys_.find(key);
    if (it != tex_slot_keys_.end()) return it->second;
    if (out_.tex2d.size() + out_.texcube.size() >= static_cast<size_t>(matbind::kMaxTextureSlots)) return -1;
    TexSlotSpec s;
    s.is_cube = cube;
    s.is_param = is_param;
    s.param = param;
    s.default_path = path;
    s.fallback = fb;
    auto& list = cube ? out_.texcube : out_.tex2d;
    int idx = static_cast<int>(list.size());
    list.push_back(std::move(s));
    tex_slot_keys_[key] = idx;
    return idx;
}

int GraphCompiler::uniform_slot(bool vec, const std::string& name, const std::array<float, 4>& def) {
    std::string key = std::string(vec ? "v|" : "s|") + name;
    auto it = uniform_keys_.find(key);
    if (it != uniform_keys_.end()) return it->second;
    if (out_.uniforms.size() >= 240) return -1;  // keeps setFragmentBytes under 4 KB
    UniformSlotSpec u;
    u.param = name;
    u.is_vector = vec;
    u.def = def;
    int idx = static_cast<int>(out_.uniforms.size());
    out_.uniforms.push_back(u);
    uniform_keys_[key] = idx;
    return idx;
}

Val GraphCompiler::texture_sample(const ExprNode& nd, int32_t tex_ref, const std::string& param, bool is_param,
                                  bool param_cube, const ExprInput& coords) {
    std::string path;
    std::string cls;
    if (tex_ref != 0) {
        path = object_canonical_path(*nd.ref.pkg, tex_ref);
        cls = object_class_name(*nd.ref.pkg, tex_ref);
    }
    if (path.empty() && is_param) {
        path = param_cube ? kDefaultTextureCubePath : kDefaultTexturePath;
        cls = param_cube ? "TextureCube" : "Texture2D";
    }
    if (path.empty()) {
        warn("texture sample (" + nd.cls + ") without a texture");
        return {};
    }
    const bool cube = param_cube || cls == "TextureCube" || cls == "TextureRenderTargetCube";

    // UE3 bakes the unpack scale/bias of the expression's own texture into the shader.
    TextureInfo info;
    ObjRef tref = tex_ref != 0 ? res_.by_ref(nd.ref, tex_ref) : res_.by_path(path);
    const bool have_info = tref.valid() && read_texture_info(*tref.pkg, tref.index, info);
    const TexDefault fb = guess_texture_default(path, have_info ? &info : nullptr);
    if (!have_info && fb == TexDefault::FlatNormal) {
        for (int i = 0; i < 3; ++i) info.unpack_min[i] = -1.0f;
    }

    int slot = tex_slot(cube, is_param, param, path, fb);
    if (slot < 0) {
        warn("texture slot limit reached");
        return {"float4(1.0)", 4};
    }

    Val c = input(coords);
    Val uv;
    if (cube) {
        uv = c.ok() ? coerce(c, 3) : Val{"P.trefl", 3};
    } else {
        uv = c.ok() ? coerce(c, 2) : Val{"P.uv0", 2};
    }
    const std::string k = std::to_string(slot);
    std::string s = cube ? ("c" + k + ".sample(sc" + k + ", " + uv.code + ")")
                         : ("t" + k + ".sample(s" + k + ", " + uv.code + ")");
    float scale[4];
    float bias[4];
    bool identity = true;
    for (int i = 0; i < 4; ++i) {
        scale[i] = info.unpack_max[i] - info.unpack_min[i];
        bias[i] = info.unpack_min[i];
        if (scale[i] != 1.0f || bias[i] != 0.0f) identity = false;
    }
    if (!identity) s = "(" + s + " * " + vec_const(4, scale).code + " + " + vec_const(4, bias).code + ")";
    return emit(4, s);
}

Val GraphCompiler::compile_expr(const ExprNode& nd) {
    const UPropertyList& p = nd.props;
    const std::string& c = nd.cls;
    auto fprop = [&](const char* name, float def) { return prop_float(p, name, def); };

    // ---- Constants & parameters ----------------------------------------------
    if (c == "Constant") return scalar_const(fprop("R", 0.0f));
    if (c == "Constant2Vector") {
        float v[2] = {fprop("R", 0.0f), fprop("G", 0.0f)};
        return vec_const(2, v);
    }
    if (c == "Constant3Vector") {
        float v[3] = {fprop("R", 0.0f), fprop("G", 0.0f), fprop("B", 0.0f)};
        return vec_const(3, v);
    }
    if (c == "Constant4Vector") {
        float v[4] = {fprop("R", 0.0f), fprop("G", 0.0f), fprop("B", 0.0f), fprop("A", 0.0f)};
        return vec_const(4, v);
    }
    if (c == "ScalarParameter") {
        std::string name = prop_name(p, "ParameterName", "None");
        float def = fprop("DefaultValue", 0.0f);
        int k = uniform_slot(false, name, {def, 0.0f, 0.0f, 0.0f});
        if (k < 0) return scalar_const(def);
        return {"U[" + std::to_string(k) + "].x", 1};
    }
    if (c == "VectorParameter") {
        std::string name = prop_name(p, "ParameterName", "None");
        std::array<float, 4> dv{0.0f, 0.0f, 0.0f, 1.0f};
        if (const UProperty* d = find_prop(p, "DefaultValue")) {
            if (d->immutable_struct) {
                dv = {d->v[0], d->v[1], d->v[2], d->v[3]};
            } else {
                dv = {prop_float(d->fields, "R", 0.0f), prop_float(d->fields, "G", 0.0f),
                      prop_float(d->fields, "B", 0.0f), prop_float(d->fields, "A", 0.0f)};
            }
        }
        int k = uniform_slot(true, name, dv);
        if (k < 0) return vec_const(4, dv.data());
        return {"U[" + std::to_string(k) + "]", 4};
    }
    if (c == "Time") return {"P.time", 1};

    // ---- Geometry / interpolants -------------------------------------------
    if (c == "TextureCoordinate") {
        int idx = prop_int(p, "CoordinateIndex", 0);
        float ut = fprop("UTiling", 1.0f);
        float vt = fprop("VTiling", 1.0f);
        if (idx > 1) warn("texture coordinate index " + std::to_string(idx) + " mapped to UV1");
        std::string base = idx <= 0 ? "P.uv0" : "P.uv1";
        if (ut == 1.0f && vt == 1.0f) return {base, 2};
        if (ut == vt) return emit(2, "(" + base + " * " + fmt_float(ut) + ")");
        float t[2] = {ut, vt};
        return emit(2, "(" + base + " * " + vec_const(2, t).code + ")");
    }
    if (c == "ReflectionVector") return {"P.trefl", 3};
    if (c == "CameraVector") return {"P.tcam", 3};
    if (c == "LightVector") return {"P.tlight", 3};
    if (c == "VertexColor") return {"P.vcolor", 4};
    if (c == "MeshEmitterVertexColor") return {"float4(1.0)", 4};
    if (c == "ScreenPosition") {
        if (prop_bool(p, "ScreenAlign", false)) {
            return emit(4, "float4(P.screen_uv, P.screen_pos.z / P.screen_pos.w, 1.0)");
        }
        return {"P.screen_pos", 4};
    }
    if (c == "PixelDepth") return {"P.screen_pos.w", 1};
    if (c == "LensFlareIntensity" || c == "LensFlareOcclusion") return {"1.0", 1};
    if (c.rfind("LensFlare", 0) == 0) return {"0.0", 1};

    // ---- Arithmetic ----------------------------------------------------------
    if (c == "Add" || c == "Subtract" || c == "Multiply" || c == "Divide") {
        const bool mul = c == "Multiply" || c == "Divide";
        Val a = input(p, "A");
        Val b = input(p, "B");
        if (!a.ok() && !b.ok()) return {};
        if (!a.ok()) a = {mul ? "1.0" : "0.0", 1};
        if (!b.ok()) b = {mul ? "1.0" : "0.0", 1};
        const char* op = c == "Add" ? "+" : c == "Subtract" ? "-" : c == "Multiply" ? "*" : "/";
        return arith(a, b, op);
    }
    if (c == "LinearInterpolate") {
        Val a = input(p, "A");
        Val b = input(p, "B");
        Val t = input(p, "Alpha");
        if (!a.ok()) a = {"0.0", 1};
        if (!b.ok()) b = {"1.0", 1};
        if (!t.ok()) t = {"0.5", 1};
        int n = arith_type(a.n, b.n);
        if (a.n == 1 && b.n == 1 && t.n > 1) n = t.n;  // HLSL lerp promotes to the alpha type
        return emit(n, "mix(" + coerce(a, n).code + ", " + coerce(b, n).code + ", " + coerce(t, n).code + ")");
    }
    if (c == "OneMinus") {
        Val v = input(p, "Input");
        if (!v.ok()) return {};
        return emit(v.n, "(1.0 - " + v.code + ")");
    }
    if (c == "Abs") return func1("abs", input(p, "Input"));
    if (c == "Floor") return func1("floor", input(p, "Input"));
    if (c == "Ceil") return func1("ceil", input(p, "Input"));
    if (c == "Frac") return func1("fract", input(p, "Input"));
    if (c == "SquareRoot") return func1("mat_sqrt", input(p, "Input"));
    if (c == "Normalize") return func1("mat_normalize", input(p, "VectorInput"));
    if (c == "Sine" || c == "Cosine") {
        Val v = input(p, "Input");
        if (!v.ok()) return {};
        float period = fprop("Period", 1.0f);
        std::string arg = v.code;
        if (period > 0.0f) arg = "(" + v.code + " * " + fmt_float(6.28318530718f / period) + ")";
        return emit(v.n, std::string(c == "Sine" ? "sin(" : "cos(") + arg + ")");
    }
    if (c == "Power") {
        Val b = input(p, "Base");
        if (!b.ok()) return {};
        Val e = input(p, "Exponent");
        if (!e.ok()) e = {"1.0", 1};
        if (e.n != 1 && e.n != b.n) e = coerce(e, 1);
        return emit(b.n, "mat_pow(" + b.code + ", " + e.code + ")");
    }
    if (c == "Min" || c == "Max") {
        Val a = input(p, "A");
        Val b = input(p, "B");
        if (!a.ok() || !b.ok()) return a.ok() ? a : b;
        int n = arith_type(a.n, b.n);
        return emit(n, std::string(c == "Min" ? "min(" : "max(") + cast_to(a, n).code + ", " + cast_to(b, n).code + ")");
    }
    if (c == "ConstantClamp") {
        Val v = input(p, "Input");
        if (!v.ok()) return {};
        Val lo = cast_to(scalar_const(fprop("Min", 0.0f)), v.n);
        Val hi = cast_to(scalar_const(fprop("Max", 1.0f)), v.n);
        return emit(v.n, "min(max(" + v.code + ", " + lo.code + "), " + hi.code + ")");
    }
    if (c == "Clamp") {
        Val v = input(p, "Input");
        if (!v.ok()) return {};
        Val lo = input(p, "Min");
        Val hi = input(p, "Max");
        if (!lo.ok() && !hi.ok()) return v;
        std::string s = v.code;
        if (lo.ok()) s = "max(" + s + ", " + coerce(lo, v.n).code + ")";
        if (hi.ok()) s = "min(" + s + ", " + coerce(hi, v.n).code + ")";
        return emit(v.n, s);
    }
    if (c == "ConstantBiasScale") {
        Val v = input(p, "Input");
        if (!v.ok()) return {};
        return emit(v.n, "((" + fmt_float(fprop("Bias", 1.0f)) + " + " + v.code + ") * " +
                             fmt_float(fprop("Scale", 0.5f)) + ")");
    }
    if (c == "AppendVector") {
        Val a = input(p, "A");
        Val b = input(p, "B");
        if (!a.ok()) return b;
        if (!b.ok()) return a;
        int n = a.n + b.n;
        if (n > 4) {
            warn("AppendVector result exceeds 4 components");
            if (a.n >= 4) return a;
            b = cast_to(b, 4 - a.n);
            n = 4;
        }
        return emit(n, std::string(type_name(n)) + "(" + a.code + ", " + b.code + ")");
    }
    if (c == "DotProduct") {
        Val a = input(p, "A");
        Val b = input(p, "B");
        if (!a.ok() || !b.ok()) return {};
        int n = (a.n == 1 || b.n == 1) ? std::max(a.n, b.n) : std::min(a.n, b.n);
        if (n == 1) return emit(1, "(" + a.code + " * " + b.code + ")");
        return emit(1, "dot(" + cast_to(a, n).code + ", " + cast_to(b, n).code + ")");
    }
    if (c == "CrossProduct") {
        Val a = input(p, "A");
        Val b = input(p, "B");
        if (!a.ok() || !b.ok()) return {};
        return emit(3, "cross(" + cast_to(a, 3).code + ", " + cast_to(b, 3).code + ")");
    }
    if (c == "ComponentMask") {
        Val v = input(p, "Input");
        if (!v.ok()) return {};
        std::vector<int> ch;
        if (prop_bool(p, "R", false)) ch.push_back(0);
        if (prop_bool(p, "G", false)) ch.push_back(1);
        if (prop_bool(p, "B", false)) ch.push_back(2);
        if (prop_bool(p, "A", false)) ch.push_back(3);
        if (ch.empty()) {
            warn("ComponentMask without channels");
            return v;
        }
        return swizzle(v, ch);
    }
    if (c == "StaticComponentMaskParameter") {
        Val v = input(p, "Input");
        if (!v.ok()) return {};
        std::string name = prop_name(p, "ParameterName", "None");
        std::array<bool, 4> m = static_mask(name, {prop_bool(p, "DefaultR", false), prop_bool(p, "DefaultG", false),
                                                   prop_bool(p, "DefaultB", false), prop_bool(p, "DefaultA", false)});
        std::vector<int> ch;
        for (int i = 0; i < 4; ++i) {
            if (m[static_cast<size_t>(i)]) ch.push_back(i);
        }
        if (ch.empty()) {
            warn("StaticComponentMaskParameter '" + name + "' without channels");
            return v;
        }
        return swizzle(v, ch);
    }
    if (c == "StaticSwitchParameter") {
        std::string name = prop_name(p, "ParameterName", "None");
        bool value = static_switch(name, prop_bool(p, "DefaultValue", false));
        Val r = input(p, value ? "A" : "B");
        if (!r.ok()) r = input(p, value ? "B" : "A");
        return r;
    }
    if (c == "If") {
        Val a = input(p, "A");
        Val b = input(p, "B");
        if (!a.ok() || !b.ok()) return {};
        Val gt = input(p, "AGreaterThanB");
        Val eq = input(p, "AEqualsB");
        Val lt = input(p, "ALessThanB");
        if (!gt.ok()) gt = {"0.0", 1};
        if (!eq.ok()) eq = {"0.0", 1};
        if (!lt.ok()) lt = {"0.0", 1};
        int n = arith_type(gt.n, arith_type(eq.n, lt.n));
        Val a1 = cast_to(a, 1);
        Val b1 = cast_to(b, 1);
        return emit(n, "((" + a1.code + " >= " + b1.code + ") ? ((" + a1.code + " > " + b1.code + ") ? " +
                           cast_to(gt, n).code + " : " + cast_to(eq, n).code + ") : " + cast_to(lt, n).code + ")");
    }

    // ---- Coordinate animation ------------------------------------------------
    if (c == "Panner") {
        Val coord = input(p, "Coordinate");
        if (!coord.ok()) coord = {"P.uv0", 2};
        const float sx = fprop("SpeedX", 0.0f);
        const float sy = fprop("SpeedY", 0.0f);
        Val t = input(p, "Time");
        std::string ox, oy;
        if (t.ok()) {
            Val t1 = coerce(t, 1);
            ox = "(" + t1.code + " * " + fmt_float(sx) + ")";
            oy = "(" + t1.code + " * " + fmt_float(sy) + ")";
        } else {
            // PeriodicHint(GameTime * Speed) -> fmod(x, 1) evaluated as a uniform expression in UE3.
            ox = "fmod(P.time * " + fmt_float(sx) + ", 1.0)";
            oy = "fmod(P.time * " + fmt_float(sy) + ", 1.0)";
        }
        Val off = emit(2, "float2(" + ox + ", " + oy + ")");
        return arith(off, coord, "+");
    }
    if (c == "Rotator") {
        Val coord = input(p, "Coordinate");
        if (!coord.ok()) coord = {"P.uv0", 2};
        Val t = input(p, "Time");
        std::string tc = t.ok() ? coerce(t, 1).code : "P.time";
        const float speed = fprop("Speed", 0.25f);
        const float cx = fprop("CenterX", 0.5f);
        const float cy = fprop("CenterY", 0.5f);
        Val ang = emit(1, "(" + tc + " * " + fmt_float(speed) + ")");
        Val cs = emit(1, "cos(" + ang.code + ")");
        Val sn = emit(1, "sin(" + ang.code + ")");
        float o[2] = {cx, cy};
        const std::string origin = vec_const(2, o).code;
        Val xy = coord.n >= 2 ? swizzle(coord, {0, 1}) : cast_to(coord, 2);
        Val d = emit(2, "(" + xy.code + " - " + origin + ")");
        Val r = emit(2, "(float2(dot(float2(" + cs.code + ", -" + sn.code + "), " + d.code + "), dot(float2(" +
                            sn.code + ", " + cs.code + "), " + d.code + ")) + " + origin + ")");
        if (coord.n == 3) return emit(3, "float3(" + r.code + ", " + swizzle(coord, {2}).code + ")");
        return r;
    }
    if (c == "BumpOffset") {
        Val h = input(p, "Height");
        if (!h.ok()) return {};
        Val coord = input(p, "Coordinate");
        if (!coord.ok()) coord = {"P.uv0", 2};
        const float hr = fprop("HeightRatio", 0.05f);
        const float rp = fprop("ReferencePlane", 0.5f);
        Val h1 = cast_to(h, 1);
        Val off = emit(2, "(P.tcam.xy * ((" + fmt_float(hr) + " * " + h1.code + ") + " + fmt_float(-rp * hr) + "))");
        return arith(off, coord, "+");
    }

    // ---- Color ---------------------------------------------------------------
    if (c == "Desaturation") {
        Val v = input(p, "Input");
        if (!v.ok()) return {};
        float lum[3] = {0.3f, 0.59f, 0.11f};
        if (const UProperty* lf = find_prop(p, "LuminanceFactors")) {
            if (lf->immutable_struct) {
                for (int i = 0; i < 3; ++i) lum[i] = lf->v[i];
            } else {
                lum[0] = prop_float(lf->fields, "R", 0.0f);
                lum[1] = prop_float(lf->fields, "G", 0.0f);
                lum[2] = prop_float(lf->fields, "B", 0.0f);
            }
        }
        Val grey = emit(1, "dot(" + cast_to(v, 3).code + ", " + vec_const(3, lum).code + ")");
        Val pct = input(p, "Percent");
        if (!pct.ok()) return grey;
        const int n = v.n;
        return emit(n, "mix(" + v.code + ", " + cast_to(grey, n).code + ", " + coerce(pct, n).code + ")");
    }
    if (c == "Fresnel") {
        const float e = fprop("Exponent", 3.0f);
        Val nrm = input(p, "Normal");
        const std::string nc = nrm.ok() ? cast_to(nrm, 3).code : "float3(0.0, 0.0, 1.0)";
        return emit(1, "mat_pow((1.0 - max(0.0, dot(" + nc + ", P.tcam))), " + fmt_float(e) + ")");
    }
    if (c == "Transform") {
        Val v = input(p, "Input");
        if (!v.ok()) return {};
        const std::string src_tt = enum_prop(p, "TransformSourceType");
        const std::string tt = enum_prop(p, "TransformType");
        const bool src_world = src_tt == "TRANSFORMSOURCE_World" || src_tt == "2";
        const bool view = tt == "TRANSFORM_View" || tt == "1";
        const bool tangent = tt == "TRANSFORM_Tangent" || tt == "3";
        Val v3 = v.n == 4 ? swizzle(v, {0, 1, 2}) : coerce(v, 3);
        std::string w = src_world ? v3.code : ("mat_tangent_to_world(P, " + v3.code + ")");
        if (view) {
            w = "mat_world_to_view(F, " + w + ")";
        } else if (tangent && src_world) {
            w = "float3(dot(P.T, " + w + "), dot(P.B, " + w + "), dot(P.N, " + w + "))";
        }
        if (v.n == 4) return emit(4, "float4(" + w + ", " + swizzle(v, {3}).code + ")");
        return emit(3, w);
    }

    // ---- Textures ------------------------------------------------------------
    if (c.rfind("TextureSample", 0) == 0 || c == "ParticleSubUV" || c == "MeshSubUV" || c == "FlipBookSample") {
        const bool is_param = c.rfind("TextureSampleParameter", 0) == 0;
        const bool param_cube = c == "TextureSampleParameterCube";
        const std::string pname = is_param ? prop_name(p, "ParameterName", "None") : std::string();
        return texture_sample(nd, prop_object(p, "Texture"), pname, is_param, param_cube, read_input(p, "Coordinates"));
    }

    // ---- Scene color / depth (translucent materials only) -----------------
    if (c == "SceneTexture" || c == "DestColor") {
        if (!translucent_) return {"float4(0.0)", 4};
        out_.uses_scene_color = true;
        Val co = c == "SceneTexture" ? input(p, "Coordinates") : Val{};
        const std::string uv = co.ok() ? coerce(co, 2).code : "P.screen_uv";
        return emit(4, "mat_scene_color(scene_color, scene_smp, " + uv + ")");
    }
    if (c == "SceneDepth" || c == "DestDepth") {
        if (!translucent_) return {"65000.0", 1};
        out_.uses_scene_depth = true;
        Val co = c == "SceneDepth" ? input(p, "Coordinates") : Val{};
        const std::string uv = co.ok() ? coerce(co, 2).code : "P.screen_uv";
        return emit(1, "mat_linear_depth(scene_depth.sample(scene_smp, " + uv + "))");
    }
    if (c == "DepthBiasedAlpha") {
        Val a = input(p, "Alpha");
        if (!a.ok()) return {};
        Val b = input(p, "Bias");
        if (!b.ok()) b = {"0.0", 1};
        if (!translucent_) return cast_to(a, 1);
        out_.uses_scene_depth = true;
        return emit(1, "mat_depth_biased_alpha(P, mat_linear_depth(scene_depth.sample(scene_smp, P.screen_uv)), " +
                           cast_to(a, 1).code + ", " + cast_to(b, 1).code + ", " +
                           fmt_float(fprop("BiasScale", 1.0f)) + ")");
    }
    if (c == "DepthBiasedBlend") {
        Val rgb = input(p, "RGB");
        if (!rgb.ok()) return {};
        Val b = input(p, "Bias");
        if (!b.ok()) b = {"0.0", 1};
        if (!translucent_) return cast_to(rgb, 3);
        out_.uses_scene_color = true;
        out_.uses_scene_depth = true;
        Val blend = emit(1, "saturate((mat_linear_depth(scene_depth.sample(scene_smp, P.screen_uv)) - P.screen_pos.w) / "
                            "max((1.0 - " + cast_to(b, 1).code + ") * " + fmt_float(fprop("BiasScale", 1.0f)) +
                                ", 0.001))");
        return emit(3, "mix(mat_scene_color(scene_color, scene_smp, P.screen_uv).rgb, " + cast_to(rgb, 3).code + ", " +
                           blend.code + ")");
    }

    out_.unknown_classes[c]++;
    warn("unsupported expression MaterialExpression" + c);
    return {};
}

Val GraphCompiler::material_input(const char* name, int n, const std::array<float, 4>& def) {
    Val v;
    const UProperty* p = find_prop(g_.props, name);
    if (p && p->type == "StructProperty") {
        const UProperty* use_const = find_prop(p->fields, "UseConstant");
        if (use_const && use_const->b) {
            if (const UProperty* k = find_prop(p->fields, "Constant")) {
                if (k->type == "FloatProperty") {
                    v = scalar_const(k->f);
                } else if (k->struct_name == "Color") {
                    float cv[3] = {color_byte_to_linear(k->v[0]), color_byte_to_linear(k->v[1]),
                                   color_byte_to_linear(k->v[2])};
                    v = vec_const(3, cv);
                } else if (k->struct_name == "Vector") {
                    v = vec_const(3, k->v);
                } else if (k->struct_name == "Vector2D") {
                    v = vec_const(2, k->v);
                }
            }
        } else {
            ExprInput in = read_input_struct(p->fields);
            if (in.connected()) {
                v = input(in);
                if (!v.ok()) warn(std::string("material input ") + name + " failed to compile; using default");
            }
        }
    }
    if (!v.ok()) v = vec_const(n, def.data());
    return cast_to(v, n);
}

void GraphCompiler::run() {
    const UPropertyList& mp = g_.props;
    MatBlendMode blend = parse_blend(mp);
    const MatLightingModel lighting = parse_lighting(mp);
    const bool is_masked_flag = prop_bool(mp, "bIsMasked", false);
    const bool masked = blend == MatBlendMode::Masked || is_masked_flag;
    if (blend == MatBlendMode::Opaque && is_masked_flag) blend = MatBlendMode::Masked;
    const float clip_value = prop_float(mp, "OpacityMaskClipValue", 0.3333f);
    out_.blend = blend;
    out_.lighting = lighting;
    out_.two_sided = prop_bool(mp, "TwoSided", false);
    translucent_ = blend == MatBlendMode::Translucent || blend == MatBlendMode::Additive ||
                   blend == MatBlendMode::Modulate;

    std::vector<std::string> normal_stmts;
    std::vector<std::string> main_stmts;
    std::vector<std::string> custom_stmts;

    // Phase 1: GetMaterialNormal (TangentNormal / TangentReflectionVector are still 0 here, as in UE3).
    begin_phase('n', &normal_stmts);
    const Val normal = material_input("Normal", 3, {0.0f, 0.0f, 1.0f, 0.0f});

    // Phase 2: everything else, evaluated after CalcMaterialParameters.
    begin_phase('m', &main_stmts);
    const Val emissive = material_input("EmissiveColor", 3, {0.0f, 0.0f, 0.0f, 0.0f});
    const Val opacity = translucent_ ? material_input("Opacity", 1, {1.0f, 0.0f, 0.0f, 0.0f}) : Val{"1.0", 1};
    const Val mask = masked ? material_input("OpacityMask", 1, {1.0f, 0.0f, 0.0f, 0.0f}) : Val{};
    Val diffuse, diffuse_power, specular, specular_power, tslm;
    if (lighting != MatLightingModel::Unlit) {
        diffuse = material_input("DiffuseColor", 3, {kDefaultGrey, kDefaultGrey, kDefaultGrey, 0.0f});
        const Val tsl_mask = material_input("TwoSidedLightingMask", 1, {0.0f, 0.0f, 0.0f, 0.0f});
        float tslc[3] = {1.0f, 1.0f, 1.0f};
        if (const UProperty* col = find_prop(mp, "TwoSidedLightingColor")) {
            if (col->struct_name == "Color") {
                for (int i = 0; i < 3; ++i) tslc[i] = color_byte_to_linear(col->v[i]);
            } else if (col->immutable_struct) {
                for (int i = 0; i < 3; ++i) tslc[i] = col->v[i];
            }
        }
        tslm = emit(3, "(" + tsl_mask.code + " * " + vec_const(3, tslc).code + ")");
        if (lighting == MatLightingModel::Phong) {
            diffuse_power = material_input("DiffusePower", 1, {1.0f, 0.0f, 0.0f, 0.0f});
            specular = material_input("SpecularColor", 3, {kDefaultGrey, kDefaultGrey, kDefaultGrey, 0.0f});
            specular_power = material_input("SpecularPower", 1, {15.0f, 0.0f, 0.0f, 0.0f});
        } else {
            diffuse_power = material_input("DiffusePower", 1, {1.0f, 0.0f, 0.0f, 0.0f});
            specular = {"float3(0.0)", 3};
            specular_power = {"15.0", 1};
        }
    }

    // Phase 3: CustomLighting, evaluated once per light-map basis direction (TangentLightVector).
    Val custom;
    if (lighting == MatLightingModel::Custom) {
        indent_ = "            ";
        begin_phase('c', &custom_stmts);
        custom = material_input("CustomLighting", 3, {0.0f, 0.0f, 0.0f, 0.0f});
        indent_ = "    ";
    }

    // ---- Assemble the fragment function ----------------------------------------
    std::ostringstream src;
    const int n2d = static_cast<int>(out_.tex2d.size());
    src << "fragment float4 @FN@(MatVSOut in [[stage_in]], constant FrameUniforms& F [[buffer(0)]]";
    if (!out_.uniforms.empty()) src << ", constant float4* U [[buffer(1)]]";
    for (int k = 0; k < n2d; ++k) {
        src << ", texture2d<float> t" << k << " [[texture(" << k << ")]], sampler s" << k << " [[sampler(" << k << ")]]";
    }
    for (int j = 0; j < static_cast<int>(out_.texcube.size()); ++j) {
        src << ", texturecube<float> c" << j << " [[texture(" << (n2d + j) << ")]], sampler sc" << j << " [[sampler("
            << (n2d + j) << ")]]";
    }
    if (out_.uses_scene_color) src << ", texture2d<float> scene_color [[texture(" << matbind::kSceneColorTexture << ")]]";
    if (out_.uses_scene_depth) src << ", depth2d<float> scene_depth [[texture(" << matbind::kSceneDepthTexture << ")]]";
    if (out_.uses_scene_color || out_.uses_scene_depth) src << ", sampler scene_smp [[sampler(" << matbind::kSceneSampler << ")]]";
    src << ", depth2d_array<float> shadow_map [[texture(" << matbind::kShadowMapTexture << ")]]";
    src << ") {\n";
    src << "    MatParams P = mat_setup(in, F);\n";
    for (const auto& s : normal_stmts) src << s << "\n";
    src << "    mat_finish_normal(P, " << normal.code << ");\n";
    for (const auto& s : main_stmts) src << s << "\n";
    src << "    float3 m_emissive = " << emissive.code << ";\n";
    if (masked) src << "    if ((" << mask.code << ") - " << fmt_float(clip_value) << " < 0.0) discard_fragment();\n";
    switch (lighting) {
        case MatLightingModel::Unlit:
            src << "    float3 m_color = m_emissive;\n";
            break;
        case MatLightingModel::Phong:
        case MatLightingModel::NonDirectional:
            src << "    float3 m_color = m_emissive + mat_lighting(P, F, shadow_map, " << diffuse.code << ", " << diffuse_power.code
                << ", " << specular.code << ", " << specular_power.code << ", " << tslm.code << ", "
                << (lighting == MatLightingModel::NonDirectional ? 1 : 0) << ");\n";
            break;
        case MatLightingModel::Custom:
            src << "    float3 m_custom = float3(0.0);\n";
            src << "    float m_sh = mat_shadow(P, F, shadow_map);\n";
            src << "    {\n";
            src << "        MatLightMap m_lm = mat_virtual_lightmap(P, F, m_sh);\n";
            src << "        for (int m_j = 0; m_j < 3; ++m_j) {\n";
            src << "            P.tlight = mat_lmb(m_j);\n";
            for (const auto& s : custom_stmts) src << s << "\n";
            src << "            m_custom += mat_lm(m_lm, m_j) * (" << custom.code << ");\n";
            src << "        }\n";
            src << "        P.tlight = float3(0.0, 0.0, 1.0);\n";
            src << "    }\n";
            src << "    float3 m_color = m_emissive + mat_lighting_custom(P, F, " << diffuse.code << ", " << tslm.code
                << ", m_custom, m_sh);\n";
            break;
    }
    switch (blend) {
        case MatBlendMode::Opaque:
        case MatBlendMode::Masked:
            src << "    return mat_out_opaque(P, F, m_color);\n";
            break;
        case MatBlendMode::Translucent:
            src << "    return mat_out_translucent(P, F, m_color, " << opacity.code << ");\n";
            break;
        case MatBlendMode::Additive:
            src << "    return mat_out_additive(P, F, m_color, " << opacity.code << ");\n";
            break;
        case MatBlendMode::Modulate:
            src << "    return mat_out_modulate(P, F, m_color, " << opacity.code << ");\n";
            break;
    }
    src << "}\n";
    out_.source = src.str();
}

// =============================================================================
// Material builder (resolution, compilation cache, textures)
// =============================================================================
struct MicInfo {
    ObjRef ref;
    UPropertyList props;
    size_t props_end = 0;
    bool has_static = false;
};

class MaterialBuilder {
public:
    MaterialBuilder(PackageManager& pm, SceneMaterialLibrary& lib, const MaterialBuildOptions& opts)
        : pm_(pm), lib_(lib), opts_(opts), res_(pm) {}

    SceneMaterial build(const std::string& leaf_path);
    void load_textures();
    void finalize();

    [[nodiscard]] const std::map<std::string, int>& unknown_classes() const { return unknown_; }
    [[nodiscard]] int warning_materials() const { return warning_materials_; }

private:
    bool resolve_chain(const std::string& leaf, ObjRef& root, std::vector<MicInfo>& chain, std::string& err);
    MaterialGraph* graph(const ObjRef& root);
    StaticParams resolve_statics(const MaterialGraph& g, const std::vector<MicInfo>& chain);
    CompiledMaterial* compile(MaterialGraph& g, const StaticParams& statics);
    int shader_index(CompiledMaterial& cm);
    int texture_index(const std::string& path, bool cube);

    PackageManager& pm_;
    SceneMaterialLibrary& lib_;
    MaterialBuildOptions opts_;
    Resolver res_;
    std::unordered_map<std::string, std::unique_ptr<MaterialGraph>> graphs_;
    std::unordered_map<std::string, std::unique_ptr<CompiledMaterial>> compiled_;
    std::unordered_map<std::string, int> shader_by_source_;
    std::unordered_map<std::string, int> texture_by_key_;
    std::map<std::string, int> unknown_;
    int warning_materials_ = 0;
};

bool MaterialBuilder::resolve_chain(const std::string& leaf, ObjRef& root, std::vector<MicInfo>& chain,
                                    std::string& err) {
    chain.clear();
    root = {};
    ObjRef cur = res_.by_path(leaf);
    if (!cur.valid()) {
        err = "material '" + leaf + "' not found";
        return false;
    }
    for (int depth = 0; depth < 16; ++depth) {
        const std::string cls = object_class_name(*cur.pkg, cur.index);
        if (cls == "Material" || cls == "DecalMaterial") {
            root = cur;
            return true;
        }
        if (cls.find("MaterialInstance") == std::string::npos) {
            err = "unsupported material class '" + cls + "' for '" + leaf + "'";
            return false;
        }
        MicInfo mic;
        mic.ref = cur;
        mic.props_end = parse_export_properties(*cur.pkg, cur.index, mic.props);
        mic.has_static = prop_bool(mic.props, "bHasStaticPermutationResource", false);
        const int32_t parent = prop_object(mic.props, "Parent");
        chain.push_back(std::move(mic));
        if (parent == 0) {
            err = "material instance '" + object_canonical_path(*cur.pkg, cur.index) + "' has no parent";
            return false;
        }
        ObjRef next = res_.by_ref(cur, parent);
        if (!next.valid()) {
            err = "parent '" + object_canonical_path(*cur.pkg, parent) + "' of '" + leaf + "' not found";
            return false;
        }
        cur = next;
    }
    err = "material instance chain too deep for '" + leaf + "'";
    return false;
}

MaterialGraph* MaterialBuilder::graph(const ObjRef& root) {
    std::string path = object_canonical_path(*root.pkg, root.index);
    std::string key = to_lower(path);
    auto it = graphs_.find(key);
    if (it != graphs_.end()) return it->second.get();
    auto g = std::make_unique<MaterialGraph>();
    g->ref = root;
    g->path = path;
    parse_export_properties(*root.pkg, root.index, g->props);
    g->collect_static_parameter_names();
    MaterialGraph* raw = g.get();
    graphs_.emplace(key, std::move(g));
    return raw;
}

StaticParams MaterialBuilder::resolve_statics(const MaterialGraph& g, const std::vector<MicInfo>& chain) {
    StaticParams sp;
    if (g.static_switch_names.empty() && g.static_mask_names.empty()) return sp;
    std::map<std::string, bool> sw_override, sw_any;
    std::map<std::string, std::array<bool, 4>> mask_override, mask_any;
    for (const auto& mic : chain) {
        if (!mic.has_static) continue;
        const auto& ex = mic.ref.pkg->get_exports()[static_cast<size_t>(mic.ref.index) - 1];
        const size_t end = static_cast<size_t>(ex.serial_offset) + static_cast<size_t>(ex.serial_size);
        StaticSet set;
        if (!scan_static_parameter_set(*mic.ref.pkg, mic.props_end, end, g.static_switch_names, g.static_mask_names, set)) {
            continue;
        }
        for (const auto& s : set.switches) {
            if (s.override_ && !sw_override.count(s.name)) sw_override[s.name] = s.value;
            if (!sw_any.count(s.name)) sw_any[s.name] = s.value;
        }
        for (const auto& m : set.masks) {
            if (m.override_ && !mask_override.count(m.name)) mask_override[m.name] = m.rgba;
            if (!mask_any.count(m.name)) mask_any[m.name] = m.rgba;
        }
    }
    for (const auto& name : g.static_switch_names) {
        if (auto it = sw_override.find(name); it != sw_override.end()) {
            sp.switches[name] = it->second;
        } else if (auto it2 = sw_any.find(name); it2 != sw_any.end()) {
            sp.switches[name] = it2->second;
        }
    }
    for (const auto& name : g.static_mask_names) {
        if (auto it = mask_override.find(name); it != mask_override.end()) {
            sp.masks[name] = it->second;
        } else if (auto it2 = mask_any.find(name); it2 != mask_any.end()) {
            sp.masks[name] = it2->second;
        }
    }
    return sp;
}

CompiledMaterial* MaterialBuilder::compile(MaterialGraph& g, const StaticParams& statics) {
    const std::string key = to_lower(g.path) + "|" + statics.signature();
    auto it = compiled_.find(key);
    if (it != compiled_.end()) return it->second.get();
    auto cm = std::make_unique<CompiledMaterial>();
    cm->base_path = g.path;
    GraphCompiler gc(res_, g, statics, *cm);
    gc.run();
    for (const auto& [cls, count] : cm->unknown_classes) unknown_[cls] += count;
    cm->shader_index = shader_index(*cm);
    CompiledMaterial* raw = cm.get();
    compiled_.emplace(key, std::move(cm));
    return raw;
}

int MaterialBuilder::shader_index(CompiledMaterial& cm) {
    auto it = shader_by_source_.find(cm.source);
    if (it != shader_by_source_.end()) return it->second;
    const int idx = static_cast<int>(lib_.shaders.size());
    MaterialShader sh;
    sh.function_name = "mat_ps_" + std::to_string(idx);
    sh.source = "// base material: " + cm.base_path + "\n" + cm.source;
    const size_t pos = sh.source.find("@FN@");
    if (pos != std::string::npos) sh.source.replace(pos, 4, sh.function_name);
    sh.num_tex2d = static_cast<int>(cm.tex2d.size());
    sh.num_texcube = static_cast<int>(cm.texcube.size());
    sh.num_uniforms = static_cast<int>(cm.uniforms.size());
    sh.blend = cm.blend;
    sh.lighting = cm.lighting;
    sh.two_sided = cm.two_sided;
    sh.uses_scene_color = cm.uses_scene_color;
    sh.uses_scene_depth = cm.uses_scene_depth;
    sh.base_material = cm.base_path;
    lib_.shaders.push_back(std::move(sh));
    shader_by_source_.emplace(cm.source, idx);
    return idx;
}

int MaterialBuilder::texture_index(const std::string& path, bool cube) {
    if (path.empty()) return -1;
    const std::string key = std::string(cube ? "c|" : "t|") + to_lower(path);
    auto it = texture_by_key_.find(key);
    if (it != texture_by_key_.end()) return it->second;
    SceneTexture t;
    t.name = path;
    t.is_cube = cube;
    const int idx = static_cast<int>(lib_.textures.size());
    lib_.textures.push_back(std::move(t));
    texture_by_key_.emplace(key, idx);
    return idx;
}

SceneMaterial MaterialBuilder::build(const std::string& leaf_path) {
    SceneMaterial sm;
    sm.name = leaf_path.empty() ? std::string(kDefaultMaterialPath) : leaf_path;

    ObjRef root;
    std::vector<MicInfo> chain;
    std::string err;
    bool fallback = false;
    if (!resolve_chain(sm.name, root, chain, err)) {
        sm.error = err;
        fallback = true;
        std::string err2;
        if (!resolve_chain(kDefaultMaterialPath, root, chain, err2)) {
            sm.error += "; " + err2;
            lib_.materials_failed++;
            if (lib_.errors.size() < 256) lib_.errors.push_back(sm.error);
            return sm;
        }
    }

    MaterialGraph* g = graph(root);
    const StaticParams statics = resolve_statics(*g, chain);
    CompiledMaterial* cm = compile(*g, statics);

    // Instance parameters: the first instance in the chain (leaf -> root) that defines a value wins.
    std::map<std::string, float> scalars;
    std::map<std::string, std::array<float, 4>> vectors;
    std::map<std::string, std::string> textures;
    for (const auto& mic : chain) {
        if (const UProperty* arr = find_prop(mic.props, "ScalarParameterValues")) {
            for (const auto& el : arr->elements) {
                const std::string name = prop_name(el, "ParameterName", "None");
                if (!scalars.count(name)) scalars[name] = prop_float(el, "ParameterValue", 0.0f);
            }
        }
        if (const UProperty* arr = find_prop(mic.props, "VectorParameterValues")) {
            for (const auto& el : arr->elements) {
                const std::string name = prop_name(el, "ParameterName", "None");
                if (vectors.count(name)) continue;
                std::array<float, 4> v{0.0f, 0.0f, 0.0f, 0.0f};
                if (const UProperty* pv = find_prop(el, "ParameterValue")) {
                    if (pv->immutable_struct) {
                        v = {pv->v[0], pv->v[1], pv->v[2], pv->v[3]};
                    } else {
                        v = {prop_float(pv->fields, "R", 0.0f), prop_float(pv->fields, "G", 0.0f),
                             prop_float(pv->fields, "B", 0.0f), prop_float(pv->fields, "A", 0.0f)};
                    }
                }
                vectors[name] = v;
            }
        }
        if (const UProperty* arr = find_prop(mic.props, "TextureParameterValues")) {
            for (const auto& el : arr->elements) {
                const std::string name = prop_name(el, "ParameterName", "None");
                const int32_t ref = prop_object(el, "ParameterValue");
                if (ref != 0 && !textures.count(name)) textures[name] = object_canonical_path(*mic.ref.pkg, ref);
            }
        }
    }

    sm.base_material = g->path;
    sm.shader = cm->shader_index;
    sm.blend = cm->blend;
    sm.lighting = cm->lighting;
    sm.two_sided = cm->two_sided;
    for (const auto& slot : cm->tex2d) {
        std::string path = slot.default_path;
        if (slot.is_param) {
            if (auto it = textures.find(slot.param); it != textures.end()) path = it->second;
        }
        sm.tex2d.push_back(texture_index(path, false));
        sm.tex2d_default.push_back(slot.fallback);
    }
    for (const auto& slot : cm->texcube) {
        std::string path = slot.default_path;
        if (slot.is_param) {
            if (auto it = textures.find(slot.param); it != textures.end()) path = it->second;
        }
        sm.texcube.push_back(texture_index(path, true));
    }
    for (const auto& u : cm->uniforms) {
        std::array<float, 4> v = u.def;
        if (u.is_vector) {
            if (auto it = vectors.find(u.param); it != vectors.end()) v = it->second;
        } else {
            if (auto it = scalars.find(u.param); it != scalars.end()) v = {it->second, 0.0f, 0.0f, 0.0f};
        }
        sm.uniforms.push_back(v);
    }

    if (!cm->warnings.empty()) {
        warning_materials_++;
        if (sm.error.empty()) sm.error = cm->warnings.front();
    }
    if (fallback) {
        lib_.materials_failed++;
        if (lib_.errors.size() < 256) lib_.errors.push_back(sm.error);
    } else {
        lib_.materials_resolved++;
    }
    if (opts_.verbose) {
        std::cout << "[Materials] " << sm.name << " -> " << sm.base_material << " (shader " << sm.shader << ", "
                  << sm.tex2d.size() << " tex, " << sm.texcube.size() << " cube, " << sm.uniforms.size()
                  << " params" << (sm.error.empty() ? "" : ", note: " + sm.error) << ")" << std::endl;
    }
    return sm;
}

void MaterialBuilder::load_textures() {
    struct Job {
        size_t index;
        ObjRef ref;
        bool cube;
        bool render_target;
    };
    std::vector<Job> jobs;
    for (size_t i = 0; i < lib_.textures.size(); ++i) {
        const SceneTexture& t = lib_.textures[i];
        ObjRef r = res_.by_path(t.name);
        if (!r.valid()) {
            if (lib_.errors.size() < 256) lib_.errors.push_back("texture '" + t.name + "' not found");
            continue;
        }
        const std::string cls = object_class_name(*r.pkg, r.index);
        const bool rt_cube = (cls == "TextureRenderTargetCube");
        const bool rt_2d = (cls == "TextureRenderTarget2D" || cls == "TextureMovie");
        const bool cube_cls = (cls == "TextureCube" || rt_cube);
        const bool tex2d_cls = (cls.find("Texture2D") != std::string::npos || cls == "TextureFlipBook" || rt_2d);
        if ((t.is_cube && !cube_cls) || (!t.is_cube && !tex2d_cls)) {
            if (lib_.errors.size() < 256) lib_.errors.push_back("texture '" + t.name + "' has unsupported class " + cls);
            continue;
        }
        jobs.push_back({i, r, t.is_cube, rt_cube || rt_2d});
    }

    unsigned threads = opts_.num_threads > 0 ? static_cast<unsigned>(opts_.num_threads)
                                             : std::max(1u, std::thread::hardware_concurrency());
    threads = std::max(1u, std::min<unsigned>(threads, static_cast<unsigned>(std::max<size_t>(jobs.size(), 1))));
    std::atomic<size_t> next{0};
    std::mutex mu;
    std::vector<std::string> errors;
    auto worker = [&]() {
        for (;;) {
            const size_t j = next.fetch_add(1);
            if (j >= jobs.size()) return;
            const Job& job = jobs[j];
            SceneTexture tex;
            std::string err;
            bool ok = false;
            if (job.cube) {
                if (job.render_target) {
                    ok = load_texture_render_target_cube(*job.ref.pkg, job.ref.index, opts_.max_texture_size, tex, &err);
                } else {
                    ok = load_texture_cube(pm_, *job.ref.pkg, job.ref.index, opts_.max_texture_size, tex, &err);
                    if (!ok) {
                        ok = load_texture_render_target_cube(*job.ref.pkg, job.ref.index, opts_.max_texture_size, tex, &err);
                    }
                }
            } else {
                if (job.render_target) {
                    ok = load_texture_render_target2d(*job.ref.pkg, job.ref.index, opts_.max_texture_size, tex, &err);
                } else {
                    ok = load_texture2d(pm_, *job.ref.pkg, job.ref.index, opts_.max_texture_size, tex, &err);
                }
            }
            if (ok && tex.valid()) {
                tex.name = lib_.textures[job.index].name;
                tex.is_cube = job.cube;
                lib_.textures[job.index] = std::move(tex);
            } else {
                std::lock_guard<std::mutex> lock(mu);
                errors.push_back("texture '" + lib_.textures[job.index].name + "': " + (err.empty() ? "load failed" : err));
            }
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (unsigned t = 0; t < threads; ++t) pool.emplace_back(worker);
    for (auto& th : pool) th.join();
    for (auto& e : errors) {
        if (lib_.errors.size() < 256) lib_.errors.push_back(std::move(e));
    }
}

void MaterialBuilder::finalize() {
    lib_.textures_loaded = 0;
    lib_.textures_failed = 0;
    lib_.texture_bytes = 0;
    for (const auto& t : lib_.textures) {
        if (!t.valid()) {
            lib_.textures_failed++;
            continue;
        }
        lib_.textures_loaded++;
        for (const auto& m : t.mips) lib_.texture_bytes += m.data.size();
        for (const auto& f : t.faces) {
            for (const auto& m : f) lib_.texture_bytes += m.data.size();
        }
    }
    for (auto& m : lib_.materials) {
        for (auto& t : m.tex2d) {
            if (t >= 0 && !lib_.textures[static_cast<size_t>(t)].valid()) t = -1;
        }
        for (auto& t : m.texcube) {
            if (t >= 0 && !lib_.textures[static_cast<size_t>(t)].valid()) t = -1;
        }
    }
}

}  // namespace

// =============================================================================
// Shared MSL prelude
// =============================================================================
const char* material_common_msl() {
    // The sun shadow lookup (renderer/sun_shadow.hpp) goes first: mat_shadow calls it, and the
    // renderer's built-in world shader uses the same code.
    static const std::string source = sun_shadow_msl() + R"msl(
#include <metal_stdlib>
using namespace metal;

// Must match me::Vertex (60 bytes).
struct MatVertexIn {
    packed_float3 position;
    packed_float3 normal;
    packed_float3 tangent;
    float u, v;
    float u2, v2;
    uint color;
    float tangent_sign;
};

// Must match FrameUniformsGPU in metal_renderer.mm.
struct FrameUniforms {
    float4x4 view_proj;
    float4x4 model;
    packed_float3 camera_pos;
    float sim_time;
    packed_float3 sun_dir;
    float runner_vision_strength;
    packed_float3 sun_color;
    float exposure;
    packed_float3 sky_color;
    float speed_2d;
    packed_float3 ground_color;
    float reaction_active;
    packed_float3 actor_tint;
    float is_runner_vision;
    packed_float3 cam_forward;
    float fov_tan;
    packed_float3 cam_right;
    float aspect;
    packed_float3 cam_up;
    float health;
    float4x4 sun_view_proj;
    packed_float3 mod_shadow_color;
    float shadow_enabled;
    float4x4 sun_view_proj_far;
};

struct MatVSOut {
    float4 clip_pos [[position]];
    float3 world_pos;
    float3 tangent_w;
    float3 binormal_w;
    float3 normal_w;
    float4 uv01;
    float4 color;
    float4 screen_pos;
};

// LocalVertexFactory equivalent: binormal = cross(TangentZ, TangentX) * TangentZ.w
vertex MatVSOut mat_vertex(const device MatVertexIn* vertices [[buffer(0)]],
                           constant FrameUniforms& F [[buffer(1)]],
                           uint vid [[vertex_id]]) {
    MatVertexIn v = vertices[vid];
    MatVSOut o;
    float4 wp = F.model * float4(float3(v.position), 1.0);
    o.world_pos = wp.xyz;
    o.clip_pos = F.view_proj * wp;
    o.screen_pos = o.clip_pos;
    float3 n = (F.model * float4(float3(v.normal), 0.0)).xyz;
    float3 t = (F.model * float4(float3(v.tangent), 0.0)).xyz;
    o.normal_w = n;
    o.tangent_w = t;
    o.binormal_w = cross(n, t) * v.tangent_sign;
    o.uv01 = float4(v.u, v.v, v.u2, v.v2);
    o.color = float4(float(v.color & 0xFFu), float((v.color >> 8) & 0xFFu),
                     float((v.color >> 16) & 0xFFu), float((v.color >> 24) & 0xFFu)) * (1.0 / 255.0);
    return o;
}

// FMaterialParameters equivalent (all vectors in tangent space like UE3).
struct MatParams {
    float2 uv0;
    float2 uv1;
    float4 vcolor;
    float3 tnormal;
    float3 trefl;
    float3 tcam;
    float3 tlight;
    float3 tsky;
    float4 screen_pos;
    float2 screen_uv;
    float3 wpos;
    float3 T;
    float3 B;
    float3 N;
    float time;
};

inline float3 mat_perp(float3 n) {
    float3 a = (abs(n.z) < 0.999) ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    return normalize(cross(a, n));
}

inline MatParams mat_setup(MatVSOut in, constant FrameUniforms& F) {
    MatParams P;
    P.uv0 = in.uv01.xy;
    P.uv1 = in.uv01.zw;
    P.vcolor = in.color;
    float3 N = in.normal_w;
    float nl = length(N);
    N = (nl > 1e-6) ? N / nl : float3(0.0, 0.0, 1.0);
    float3 T = in.tangent_w - N * dot(N, in.tangent_w);
    float tl = length(T);
    T = (tl > 1e-6) ? T / tl : mat_perp(N);
    float3 C = cross(N, T);
    float3 B = (dot(C, in.binormal_w) < 0.0) ? -C : C;
    P.T = T;
    P.B = B;
    P.N = N;
    float3 cam = float3(F.camera_pos) - in.world_pos;
    float3 tc = float3(dot(T, cam), dot(B, cam), dot(N, cam));
    float tcl = length(tc);
    P.tcam = (tcl > 1e-6) ? tc / tcl : float3(0.0, 0.0, 1.0);
    P.tlight = float3(0.0, 0.0, 1.0);  // base pass: CalcMaterialParameters(..., half3(0,0,1))
    P.tsky = float3(T.z, B.z, N.z);
    P.tnormal = float3(0.0);
    P.trefl = float3(0.0);
    P.screen_pos = in.screen_pos;
    float2 ndc = in.screen_pos.xy / in.screen_pos.w;
    P.screen_uv = ndc * float2(0.5, -0.5) + 0.5;
    P.wpos = in.world_pos;
    P.time = F.sim_time;
    return P;
}

inline void mat_finish_normal(thread MatParams& P, float3 n) {
    float l2 = dot(n, n);
    P.tnormal = (l2 > 1e-12) ? n * rsqrt(l2) : float3(0.0, 0.0, 1.0);
    P.trefl = -P.tcam + P.tnormal * dot(P.tnormal, P.tcam) * 2.0;
}

inline float3 mat_tangent_to_world(MatParams P, float3 v) { return P.T * v.x + P.B * v.y + P.N * v.z; }
inline float3 mat_world_to_view(constant FrameUniforms& F, float3 w) {
    return float3(dot(w, float3(F.cam_right)), dot(w, float3(F.cam_up)), dot(w, float3(F.cam_forward)));
}

// D3D9 pow/sqrt operate on |x|.
inline float  mat_pow(float  b, float  e) { return pow(max(abs(b), 1e-8), e); }
inline float2 mat_pow(float2 b, float2 e) { return pow(max(abs(b), float2(1e-8)), e); }
inline float3 mat_pow(float3 b, float3 e) { return pow(max(abs(b), float3(1e-8)), e); }
inline float4 mat_pow(float4 b, float4 e) { return pow(max(abs(b), float4(1e-8)), e); }
inline float2 mat_pow(float2 b, float e) { return mat_pow(b, float2(e)); }
inline float3 mat_pow(float3 b, float e) { return mat_pow(b, float3(e)); }
inline float4 mat_pow(float4 b, float e) { return mat_pow(b, float4(e)); }
template <typename T> inline T mat_sqrt(T x) { return sqrt(abs(x)); }
inline float  mat_normalize(float  v) { return sign(v); }
inline float2 mat_normalize(float2 v) { return v * rsqrt(max(dot(v, v), 1e-20)); }
inline float3 mat_normalize(float3 v) { return v * rsqrt(max(dot(v, v), 1e-20)); }
inline float4 mat_normalize(float4 v) { return v * rsqrt(max(dot(v, v), 1e-20)); }

// ---- Lighting (BasePassPixelShader.usf) ------------------------------------
// HL2 light-map basis (columns of LightMapBasis in BasePassPixelShader.usf).
constant float3 kLMB0 = float3(0.0, 0.81649658, 0.57735027);
constant float3 kLMB1 = float3(-0.70710678, -0.40824829, 0.57735027);
constant float3 kLMB2 = float3(0.70710678, -0.40824829, 0.57735027);

// Virtual light-map tuning (with real-time directional sun shadow map + Beast hemisphere GI).
constant float kSunIntensity = 0.88;
constant float kSkyUpper = 0.48;
constant float kSkyLower = 0.34;
constant float kAmbient = 0.025;
constant float3 kHazeColor = float3(0.76, 0.86, 0.96);

inline float3 mat_lmb(int j) { return j == 0 ? kLMB0 : (j == 1 ? kLMB1 : kLMB2); }

struct MatLightMap {
    float3 c0;
    float3 c1;
    float3 c2;
};
inline float3 mat_lm(MatLightMap m, int j) { return j == 0 ? m.c0 : (j == 1 ? m.c1 : m.c2); }

// Real-time directional sun shadow cascades, through the shared world-stable PCF lookup
// (sun_shadow_visibility, renderer/sun_shadow.hpp, prepended to this prelude).
inline float mat_shadow(MatParams P, constant FrameUniforms& F, depth2d_array<float> shadow_map) {
    if (F.shadow_enabled < 0.5) return 1.0;
    float bias_scale = (F.shadow_enabled > 1.5) ? 0.06 : 1.0;
    float3 Lw = normalize(float3(F.sun_dir));
    float ndl_geo = saturate(dot(P.N, Lw));
    if (ndl_geo <= 0.001) return 0.0;
    float3 biased_wpos = P.wpos + P.N * (mix(14.0, 4.0, ndl_geo) * bias_scale) + Lw * (5.0 * bias_scale);
    return sun_shadow_visibility(shadow_map, F.sun_view_proj, F.sun_view_proj_far, /*use_far=*/F.shadow_enabled < 1.5,
                                 biased_wpos, P.N, ndl_geo, mix(25.2, 7.0, ndl_geo));
}

// Directional light-map coefficients modulated by real-time sun shadow visibility.
inline MatLightMap mat_virtual_lightmap(MatParams P, constant FrameUniforms& F, float shadow) {
    float3 Lw = normalize(float3(F.sun_dir));
    float3 Lt = float3(dot(P.T, Lw), dot(P.B, Lw), dot(P.N, Lw));
    float ndl = saturate(Lt.z) * shadow;
    float3 w = saturate(float3(dot(Lt, kLMB0), dot(Lt, kLMB1), dot(Lt, kLMB2)));
    w *= w;
    w /= max(w.x + w.y + w.z, 1e-4);
    float3 sun = float3(F.sun_color) * (kSunIntensity * ndl * 3.0);
    MatLightMap m;
    m.c0 = sun * w.x;
    m.c1 = sun * w.y;
    m.c2 = sun * w.z;
    return m;
}

// GetMaterialHemisphereLightTransferFull (model: 0 Phong, 1 NonDirectional, 2 Custom)
// Combines FSkyLightSceneProxy UpperSkyColor/LowerSkyColor with DirectionalLight.ModShadowColor
// so cast shadows and shadowed walls exhibit Mirror's Edge's signature cool azure fill and warm ground bounce.
inline float3 mat_hemisphere(MatParams P, constant FrameUniforms& F, float3 diffuse, float3 tslm, int model, float shadow) {
    tslm = saturate(tslm);
    float3 Nw = mat_tangent_to_world(P, P.tnormal);
    float3 Lw = normalize(float3(F.sun_dir));
    float sun_lit = saturate(dot(Nw, Lw)) * shadow;

    // In shadow (sun_lit -> 0), tint upper hemisphere strongly toward cool cerulean ModShadowColor
    float3 mod_azure = max(float3(F.mod_shadow_color), float3(0.42, 0.65, 0.92)) * float3(0.55, 0.86, 1.25);
    float3 upper_c = mix(mod_azure * 0.44, float3(F.sky_color) * kSkyUpper, sun_lit);
    float3 lower_c = float3(F.ground_color) * kSkyLower;

    // Directional lateral bounce on vertical walls so orthogonal shadowed walls have distinct Beast GI contrast
    float wall_factor = saturate(1.0 - abs(Nw.z));
    float lateral_sky = 0.84 + 0.16 * (Nw.x * 0.65 - Nw.y * 0.75);
    float sun_opp = saturate(0.5 - 0.5 * dot(Nw.xy, Lw.xy));
    upper_c *= mix(1.0, lateral_sky * (0.90 + 0.22 * sun_opp), wall_factor);
    lower_c *= mix(1.0, lateral_sky * (1.12 - 0.22 * sun_opp), wall_factor);

    float3 tsl = tslm * diffuse;
    float3 up_l = float3(0.0);
    float3 lo_l = float3(0.0);
    if (model == 1) {
        up_l = diffuse;
        lo_l = diffuse;
    } else if (model == 0) {
        float nc = clamp(dot(P.tsky, P.tnormal), -1.0, 1.0);
        float2 w = float2(0.5, 0.5) + float2(0.5, -0.5) * nc;
        w *= w;
        up_l = diffuse * w.x;
        lo_l = diffuse * w.y;
    }
    return mix(up_l, tsl, tslm) * upper_c + mix(lo_l, tsl, tslm) * lower_c;
}

inline float3 mat_lighting(MatParams P, constant FrameUniforms& F, depth2d_array<float> shadow_map,
                           float3 diffuse, float diffuse_power,
                           float3 specular, float specular_power, float3 tslm, int model) {
    float shadow = mat_shadow(P, F, shadow_map);
    MatLightMap lm = mat_virtual_lightmap(P, F, shadow);
    float3 m = (model == 1) ? float3(1.0) : saturate(tslm);
    float3 lmn = saturate(float3(dot(P.tnormal, kLMB0), dot(P.tnormal, kLMB1), dot(P.tnormal, kLMB2)));
    float3 lmr = saturate(float3(dot(P.trefl, kLMB0), dot(P.trefl, kLMB1), dot(P.trefl, kLMB2)));
    float3 dt = pow(max(lmn * lmn, float3(1e-8)), float3(diffuse_power)) * (1.0 - m) + m;
    float3 st = pow(max(lmr, float3(1e-8)), float3(specular_power + 1.0)) * (1.0 - m);
    float3 dn = diffuse * ((1.0 + diffuse_power) * 0.5);
    float3 c = lm.c0 * (dt.x * dn + st.x * specular)
             + lm.c1 * (dt.y * dn + st.y * specular)
             + lm.c2 * (dt.z * dn + st.z * specular);
    c += mat_hemisphere(P, F, diffuse, tslm, model, shadow);
    c += diffuse * kAmbient;
    return c;
}

inline float3 mat_lighting_custom(MatParams P, constant FrameUniforms& F, float3 diffuse, float3 tslm,
                                  float3 custom_sum, float shadow) {
    return custom_sum + mat_hemisphere(P, F, diffuse, tslm, 2, shadow) + diffuse * kAmbient;
}

// ---- Output (scene color is display-referred in this renderer) -------------
inline float mat_haze(MatParams P, constant FrameUniforms& F) {
    float dist = length(float3(F.camera_pos) - P.wpos);
    return saturate((dist - 2500.0) / 38000.0) * 0.65;
}
// Filmic HDR highlight compression before gamma encoding prevents sunlit white concrete (c ~ 1.8)
// from clipping to flat #FFFFFF, preserving warm sun vs cool azure shadow contrast.
inline float3 mat_encode(float3 c) {
    float3 x = max(c, float3(0.0));
    float3 mapped = float3(1.0) - exp(-x * 0.76);
    return pow(mapped, float3(1.0 / 2.2));
}

inline float4 mat_out_opaque(MatParams P, constant FrameUniforms& F, float3 c) {
    return float4(mix(mat_encode(c), kHazeColor, mat_haze(P, F)), 1.0);
}
inline float4 mat_out_translucent(MatParams P, constant FrameUniforms& F, float3 c, float opacity) {
    return float4(mix(mat_encode(c), kHazeColor, mat_haze(P, F)), saturate(opacity));
}
inline float4 mat_out_additive(MatParams P, constant FrameUniforms& F, float3 c, float opacity) {
    return float4(max(c, float3(0.0)) * ((1.0 - mat_haze(P, F)) * max(opacity, 0.0)), 0.0);
}
inline float4 mat_out_modulate(MatParams P, constant FrameUniforms& F, float3 c, float opacity) {
    return float4(mix(mat_encode(c), float3(1.0), mat_haze(P, F)), opacity);
}

// ---- Scene depth (matches the renderer's projection: near 5, far 65000, depth range [0.05, 1]) ----
inline float mat_linear_depth(float d) {
    float ndc = saturate((d - 0.05) / 0.95);
    return (5.0 * 65000.0) / (65000.0 - ndc * (65000.0 - 5.0));
}
inline float mat_depth_biased_alpha(MatParams P, float scene_depth, float alpha, float bias, float bias_scale) {
    float depth_bias = (1.0 - bias) * bias_scale;
    float blend = saturate((scene_depth - P.screen_pos.w) / max(depth_bias, 0.001));
    return alpha * blend;
}
// The opaque scene color copy is display-referred: decode to linear for the material graph.
inline float4 mat_scene_color(texture2d<float> t, sampler s, float2 uv) {
    float4 c = t.sample(s, uv);
    return float4(pow(max(c.rgb, float3(0.0)), float3(2.2)), c.a);
}
)msl";
    return source.c_str();
}

std::shared_ptr<SceneMaterialLibrary> build_scene_materials(PackageManager& pm,
                                                           const std::vector<std::string>& material_paths,
                                                           const MaterialBuildOptions& opts) {
    const auto t0 = std::chrono::steady_clock::now();
    auto lib = std::make_shared<SceneMaterialLibrary>();
    lib->common_source = material_common_msl();

    MaterialBuilder builder(pm, *lib, opts);
    lib->materials.reserve(material_paths.size());
    for (const auto& path : material_paths) lib->materials.push_back(builder.build(path));
    const auto t1 = std::chrono::steady_clock::now();
    builder.load_textures();
    builder.finalize();
    const auto t2 = std::chrono::steady_clock::now();
    lib->build_seconds = std::chrono::duration<double>(t2 - t0).count();

    std::cout << "[Materials] " << lib->materials.size() << " materials (" << lib->materials_resolved << " resolved, "
              << lib->materials_failed << " fallback, " << builder.warning_materials() << " with warnings), "
              << lib->shaders.size() << " shaders, " << lib->textures.size() << " textures (" << lib->textures_loaded
              << " loaded, " << lib->textures_failed << " failed, " << (lib->texture_bytes / (1024 * 1024))
              << " MB) in " << std::chrono::duration<double>(t1 - t0).count() << " s + "
              << std::chrono::duration<double>(t2 - t1).count() << " s textures" << std::endl;
    if (!builder.unknown_classes().empty()) {
        std::cout << "[Materials] Unsupported expressions:";
        for (const auto& [cls, n] : builder.unknown_classes()) std::cout << " " << cls << "(" << n << ")";
        std::cout << std::endl;
    }
    const size_t show = opts.verbose ? lib->errors.size() : std::min<size_t>(lib->errors.size(), 8);
    for (size_t i = 0; i < show; ++i) std::cout << "[Materials]   " << lib->errors[i] << std::endl;

    if (const char* dump = std::getenv("ME_MATERIAL_DUMP")) {
        std::ofstream f(dump);
        if (f.is_open()) {
            f << lib->common_source << "\n";
            for (const auto& sh : lib->shaders) f << "\n" << sh.source;
            std::cout << "[Materials] Wrote generated MSL to " << dump << std::endl;
        }
    }
    return lib;
}

}  // namespace me
