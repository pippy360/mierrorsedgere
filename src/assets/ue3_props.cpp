#include "ue3_props.hpp"
#include "upk_loader.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace me {

namespace {

const std::unordered_map<std::string, int32_t>& immutable_struct_sizes() {
    static const std::unordered_map<std::string, int32_t> k = {
        {"Vector", 12},  {"Vector2D", 8},     {"Vector4", 16}, {"Rotator", 12},   {"Color", 4},
        {"LinearColor", 16}, {"Guid", 16},    {"Plane", 16},   {"Quat", 16},      {"Matrix", 64},
        {"Box", 25},     {"IntPoint", 8},     {"Sphere", 16},  {"TwoVectors", 24}, {"BoxSphereBounds", 28},
    };
    return k;
}

bool is_property_type_name(const std::string& t) {
    static const char* kTypes[] = {"IntProperty",    "FloatProperty",  "BoolProperty",      "ByteProperty",
                                   "ObjectProperty", "ClassProperty",  "ComponentProperty", "NameProperty",
                                   "StrProperty",    "StructProperty", "ArrayProperty",     "DelegateProperty",
                                   "InterfaceProperty", "MapProperty"};
    for (const char* k : kTypes) {
        if (t == k) return true;
    }
    return false;
}

inline int32_t rd_i32(const std::vector<uint8_t>& d, size_t off) {
    int32_t v = 0;
    if (off + 4 <= d.size()) std::memcpy(&v, d.data() + off, 4);
    return v;
}

inline float rd_f32(const std::vector<uint8_t>& d, size_t off) {
    float v = 0.0f;
    if (off + 4 <= d.size()) std::memcpy(&v, d.data() + off, 4);
    return v;
}

std::string fname_at(const UPKPackage& pkg, size_t off) {
    const auto& names = pkg.get_names();
    const auto& d = pkg.get_data();
    int32_t idx = rd_i32(d, off);
    int32_t num = rd_i32(d, off + 4);
    if (idx < 0 || static_cast<size_t>(idx) >= names.size()) return "";
    if (num > 0) return names[idx] + "_" + std::to_string(num - 1);
    return names[idx];
}

std::string read_fstring_at(const std::vector<uint8_t>& d, size_t off, size_t end) {
    int32_t len = rd_i32(d, off);
    off += 4;
    if (len > 0) {
        if (off + static_cast<size_t>(len) > end) return "";
        return std::string(reinterpret_cast<const char*>(d.data() + off), static_cast<size_t>(len) - 1);
    }
    if (len < 0) {
        size_t chars = static_cast<size_t>(-len);
        if (off + chars * 2 > end) return "";
        std::string s;
        for (size_t i = 0; i + 1 < chars; ++i) {
            uint16_t w = static_cast<uint16_t>(d[off + i * 2] | (d[off + i * 2 + 1] << 8));
            s.push_back(w < 0x80 ? static_cast<char>(w) : '?');
        }
        return s;
    }
    return "";
}

// Checks whether a tagged property list plausibly starts at `off`.
bool looks_like_tag(const UPKPackage& pkg, size_t off, size_t end) {
    const auto& names = pkg.get_names();
    const auto& d = pkg.get_data();
    if (off + 8 > end) return false;
    int32_t ni = rd_i32(d, off);
    int32_t nn = rd_i32(d, off + 4);
    if (ni < 0 || static_cast<size_t>(ni) >= names.size() || nn < 0) return false;
    if (names[ni] == "None") return true;
    if (off + 24 > end) return false;
    int32_t ti = rd_i32(d, off + 8);
    int32_t tn = rd_i32(d, off + 12);
    int32_t sz = rd_i32(d, off + 16);
    if (ti < 0 || static_cast<size_t>(ti) >= names.size() || tn != 0 || sz < 0) return false;
    return is_property_type_name(names[ti]);
}

void decode_array(const UPKPackage& pkg, UProperty& p, size_t vpos, size_t vend, int depth) {
    const auto& d = pkg.get_data();
    const auto& names = pkg.get_names();
    if (vpos + 4 > vend) return;
    int32_t count = rd_i32(d, vpos);
    p.array_count = count;
    if (count <= 0) return;
    size_t body = vpos + 4;
    size_t body_len = vend - body;

    if (body_len == static_cast<size_t>(count) * 4) {
        p.ints.reserve(count);
        for (int32_t k = 0; k < count; ++k) p.ints.push_back(rd_i32(d, body + k * 4));
        return;
    }
    // Array of tagged structs?
    if (looks_like_tag(pkg, body, vend)) {
        size_t cur = body;
        std::vector<std::vector<UProperty>> elems;
        bool ok = true;
        for (int32_t k = 0; k < count; ++k) {
            if (!looks_like_tag(pkg, cur, vend)) { ok = false; break; }
            UPropertyList el;
            cur = parse_property_tree(pkg, cur, vend, el, depth + 1);
            elems.push_back(std::move(el));
        }
        if (ok && cur == vend) {
            p.elements = std::move(elems);
            return;
        }
    }
    if (body_len == static_cast<size_t>(count) * 8) {
        bool all_names = true;
        for (int32_t k = 0; k < count; ++k) {
            int32_t ni = rd_i32(d, body + k * 8);
            int32_t nn = rd_i32(d, body + k * 8 + 4);
            if (ni < 0 || static_cast<size_t>(ni) >= names.size() || nn < 0 || nn > 100000) { all_names = false; break; }
        }
        if (all_names) {
            for (int32_t k = 0; k < count; ++k) p.names.push_back(fname_at(pkg, body + k * 8));
            return;
        }
    }
    // Otherwise leave raw (value_offset/size available to callers).
}

}  // namespace

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

size_t parse_property_tree(const UPKPackage& pkg, size_t offset, size_t end, UPropertyList& out, int depth) {
    const auto& d = pkg.get_data();
    const auto& names = pkg.get_names();
    end = std::min(end, d.size());
    size_t pos = offset;
    if (depth > 12) return end;

    while (pos + 8 <= end) {
        int32_t ni = rd_i32(d, pos);
        if (ni < 0 || static_cast<size_t>(ni) >= names.size()) break;
        std::string pname = fname_at(pkg, pos);
        pos += 8;
        if (names[ni] == "None") break;
        if (pos + 16 > end) break;

        int32_t ti = rd_i32(d, pos);
        if (ti < 0 || static_cast<size_t>(ti) >= names.size()) break;
        UProperty p;
        p.name = pname;
        p.type = names[ti];
        p.size = rd_i32(d, pos + 8);
        p.array_index = rd_i32(d, pos + 12);
        pos += 16;
        if (p.size < 0 || !is_property_type_name(p.type)) break;

        if (p.type == "StructProperty") {
            if (pos + 8 > end) break;
            p.struct_name = fname_at(pkg, pos);
            pos += 8;
        } else if (p.type == "BoolProperty") {
            if (pos + 4 > end) break;
            p.b = rd_i32(d, pos) != 0;
            p.value_offset = pos;
            pos += 4;
            out.push_back(std::move(p));
            continue;
        }

        size_t vpos = pos;
        size_t vend = pos + static_cast<size_t>(p.size);
        if (vend > end) break;
        p.value_offset = vpos;
        pos = vend;

        if (p.type == "IntProperty") {
            p.i = rd_i32(d, vpos);
            p.f = static_cast<float>(p.i);
        } else if (p.type == "FloatProperty") {
            p.f = rd_f32(d, vpos);
        } else if (p.type == "ObjectProperty" || p.type == "ClassProperty" || p.type == "ComponentProperty" ||
                   p.type == "InterfaceProperty") {
            p.i = rd_i32(d, vpos);
        } else if (p.type == "NameProperty") {
            p.s = fname_at(pkg, vpos);
        } else if (p.type == "ByteProperty") {
            if (p.size == 8) {
                p.s = fname_at(pkg, vpos);
            } else if (p.size >= 1) {
                p.i = d[vpos];
            }
        } else if (p.type == "StrProperty") {
            p.s = read_fstring_at(d, vpos, vend);
        } else if (p.type == "StructProperty") {
            const auto& imm = immutable_struct_sizes();
            auto it = imm.find(p.struct_name);
            if (it != imm.end() && it->second == p.size) {
                p.immutable_struct = true;
                if (p.struct_name == "Color") {
                    // FColor is serialized B, G, R, A
                    p.v[0] = d[vpos + 2] / 255.0f;
                    p.v[1] = d[vpos + 1] / 255.0f;
                    p.v[2] = d[vpos + 0] / 255.0f;
                    p.v[3] = d[vpos + 3] / 255.0f;
                } else if (p.struct_name == "Rotator" || p.struct_name == "IntPoint") {
                    for (int k = 0; k < p.size / 4 && k < 3; ++k) p.vi[k] = rd_i32(d, vpos + k * 4);
                } else if (p.struct_name != "Guid") {
                    for (int k = 0; k < p.size / 4 && k < 4; ++k) p.v[k] = rd_f32(d, vpos + k * 4);
                }
            } else if (p.size > 0) {
                parse_property_tree(pkg, vpos, vend, p.fields, depth + 1);
            }
        } else if (p.type == "ArrayProperty") {
            decode_array(pkg, p, vpos, vend, depth);
        }
        out.push_back(std::move(p));
    }
    return pos;
}

size_t parse_export_properties(const UPKPackage& pkg, int32_t export_index_1based, UPropertyList& out) {
    const auto& exports = pkg.get_exports();
    if (export_index_1based <= 0 || static_cast<size_t>(export_index_1based) > exports.size()) return 0;
    const auto& exp = exports[export_index_1based - 1];
    if (exp.serial_offset < 0 || exp.serial_size <= 0) return 0;
    size_t so = static_cast<size_t>(exp.serial_offset);
    size_t se = so + static_cast<size_t>(exp.serial_size);
    if (se > pkg.get_data().size()) return 0;
    size_t start = pkg.find_property_start(exp);
    if (start < so || start >= se) return se;
    return parse_property_tree(pkg, start, se, out, 0);
}

const UProperty* find_prop(const UPropertyList& props, const std::string& name, int32_t array_index) {
    for (const auto& p : props) {
        if (p.array_index == array_index && p.name == name) return &p;
    }
    return nullptr;
}

int32_t prop_int(const UPropertyList& props, const std::string& name, int32_t def) {
    const UProperty* p = find_prop(props, name);
    return p ? p->i : def;
}

float prop_float(const UPropertyList& props, const std::string& name, float def) {
    const UProperty* p = find_prop(props, name);
    if (!p) return def;
    if (p->type == "IntProperty") return static_cast<float>(p->i);
    return p->f;
}

bool prop_bool(const UPropertyList& props, const std::string& name, bool def) {
    const UProperty* p = find_prop(props, name);
    return p ? p->b : def;
}

int32_t prop_object(const UPropertyList& props, const std::string& name) {
    const UProperty* p = find_prop(props, name);
    return p ? p->i : 0;
}

std::string prop_name(const UPropertyList& props, const std::string& name, const std::string& def) {
    const UProperty* p = find_prop(props, name);
    return p ? p->s : def;
}

std::string export_object_name(const UPKPackage& pkg, int32_t export_index_1based) {
    const auto& exports = pkg.get_exports();
    if (export_index_1based <= 0 || static_cast<size_t>(export_index_1based) > exports.size()) return "";
    const auto& e = exports[export_index_1based - 1];
    if (e.object_number > 0) return e.object_name + "_" + std::to_string(e.object_number - 1);
    return e.object_name;
}

std::string object_full_path(const UPKPackage& pkg, int32_t index) {
    const auto& exports = pkg.get_exports();
    const auto& imports = pkg.get_imports();
    std::vector<std::string> parts;
    int guard = 0;
    while (index != 0 && guard++ < 64) {
        if (index > 0) {
            if (static_cast<size_t>(index) > exports.size()) break;
            parts.push_back(export_object_name(pkg, index));
            index = exports[index - 1].outer_index;
        } else {
            size_t ii = static_cast<size_t>(-index - 1);
            if (ii >= imports.size()) break;
            const auto& imp = imports[ii];
            if (imp.object_number > 0) {
                parts.push_back(imp.object_name + "_" + std::to_string(imp.object_number - 1));
            } else {
                parts.push_back(imp.object_name);
            }
            index = imp.outer_index;
        }
    }
    std::string out;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        if (!out.empty()) out += ".";
        out += *it;
    }
    return out;
}

std::string package_name_of(const UPKPackage& pkg) {
    const std::string& path = pkg.get_file_path();
    size_t slash = path.find_last_of("/\\");
    std::string file = slash == std::string::npos ? path : path.substr(slash + 1);
    size_t dot = file.rfind('.');
    return dot == std::string::npos ? file : file.substr(0, dot);
}

// True if the outer chain of `index` is rooted at an import or at an EF_ForcedExport package export,
// i.e. object_full_path() already starts with the object's real package name.
static bool path_is_package_rooted(const UPKPackage& pkg, int32_t index) {
    constexpr uint32_t kForcedExport = 0x1;  // EF_ForcedExport
    const auto& exports = pkg.get_exports();
    int guard = 0;
    while (index > 0 && guard++ < 64) {
        if (static_cast<size_t>(index) > exports.size()) return false;
        const auto& e = exports[static_cast<size_t>(index - 1)];
        if (e.outer_index == 0) return (e.export_flags & kForcedExport) != 0;
        index = e.outer_index;
    }
    return index < 0;  // rooted at an import (imports always carry their package)
}

std::string object_canonical_path(const UPKPackage& pkg, int32_t index) {
    std::string path = object_full_path(pkg, index);
    if (index == 0 || path_is_package_rooted(pkg, index)) return path;
    const std::string pkg_name = package_name_of(pkg);
    if (pkg_name.empty()) return path;
    return path.empty() ? pkg_name : pkg_name + "." + path;
}

std::string object_outermost_name(const UPKPackage& pkg, int32_t index) {
    std::string path = object_canonical_path(pkg, index);
    size_t dot = path.find('.');
    return dot == std::string::npos ? path : path.substr(0, dot);
}

std::string object_class_name(const UPKPackage& pkg, int32_t index) {
    if (index > 0) {
        const auto& exports = pkg.get_exports();
        if (static_cast<size_t>(index) > exports.size()) return "";
        return pkg.get_export_class(exports[index - 1]);
    }
    if (index < 0) {
        const auto& imports = pkg.get_imports();
        size_t ii = static_cast<size_t>(-index - 1);
        if (ii >= imports.size()) return "";
        return imports[ii].class_name;
    }
    return "";
}

}  // namespace me
