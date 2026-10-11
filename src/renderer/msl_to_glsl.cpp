#include "msl_to_glsl.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace me {

namespace {

// ---- tokens (as in msl_to_hlsl.cpp) ------------------------------------------------
struct Tok {
    enum Kind { Ident, Number, Punct } kind = Punct;
    std::string text;
    std::string ws;  // whitespace before the token (comments removed, line breaks kept)
    bool glsl = false;  // inserted by the translator: already GLSL, not to be renamed
};
using Toks = std::vector<Tok>;

struct TranslateError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

bool is_ident_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool is_ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
bool is_digit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }

Toks tokenize(const std::string& s) {
    static const char* const kThree[] = {"<<=", ">>="};
    static const char* const kTwo[] = {"::", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "++",
                                       "--", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "->"};
    Toks out;
    std::string ws;
    const size_t n = s.size();
    size_t i = 0;
    bool line_start = true;
    while (i < n) {
        const char c = s[i];
        if (c == '\n') {
            ws += '\n';
            ++i;
            line_start = true;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') {
            if (c != '\r') ws += c;
            ++i;
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            while (i < n && s[i] != '\n') ++i;
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) {
                if (s[i] == '\n') ws += '\n';
                ++i;
            }
            i = std::min(n, i + 2);
            continue;
        }
        if (c == '#' && line_start) {  // #include <metal_stdlib>
            while (i < n && s[i] != '\n') ++i;
            continue;
        }
        line_start = false;

        Tok t;
        t.ws = std::move(ws);
        ws.clear();
        if (is_ident_start(c)) {
            size_t j = i;
            while (j < n && is_ident_char(s[j])) ++j;
            t.kind = Tok::Ident;
            t.text = s.substr(i, j - i);
            i = j;
        } else if (is_digit(c) || (c == '.' && i + 1 < n && is_digit(s[i + 1]))) {
            const bool hex = c == '0' && i + 1 < n && (s[i + 1] == 'x' || s[i + 1] == 'X');
            size_t j = i;
            while (j < n) {
                const char d = s[j];
                if (is_ident_char(d) || d == '.') {
                    ++j;
                } else if ((d == '+' || d == '-') && !hex && j > i && (s[j - 1] == 'e' || s[j - 1] == 'E')) {
                    ++j;
                } else {
                    break;
                }
            }
            t.kind = Tok::Number;
            t.text = s.substr(i, j - i);
            i = j;
        } else {
            size_t len = 1;
            for (const char* p : kThree) {
                if (s.compare(i, 3, p) == 0) len = 3;
            }
            if (len == 1) {
                for (const char* p : kTwo) {
                    if (s.compare(i, 2, p) == 0) len = 2;
                }
            }
            t.kind = Tok::Punct;
            t.text = s.substr(i, len);
            i += len;
        }
        out.push_back(std::move(t));
    }
    return out;
}

bool is(const Tok& t, const char* text) { return t.text == text; }
bool is_ident(const Tok& t) { return t.kind == Tok::Ident; }

Tok make(Tok::Kind kind, const std::string& text, const std::string& ws = "") {
    Tok t;
    t.kind = kind;
    t.text = text;
    t.ws = ws;
    return t;
}
Tok punct(const std::string& text, const std::string& ws = "") { return make(Tok::Punct, text, ws); }
Tok ident(const std::string& text, const std::string& ws = "") { return make(Tok::Ident, text, ws); }
Tok number(const std::string& text, const std::string& ws = "") { return make(Tok::Number, text, ws); }
Tok glsl(const std::string& text, const std::string& ws = "") {
    Tok t = make(Tok::Ident, text, ws);
    t.glsl = true;
    return t;
}

std::string join(const Toks& t, size_t from, size_t to) {
    std::string out;
    for (size_t i = from; i < to && i < t.size(); ++i) {
        out += t[i].ws;
        out += t[i].text;
    }
    return out;
}
std::string join(const Toks& t) { return join(t, 0, t.size()); }

// Index of the token that closes the bracket opened at `open`.
size_t match_close(const Toks& t, size_t open) {
    const std::string o = t[open].text;
    const std::string c = (o == "(") ? ")" : (o == "[") ? "]" : "}";
    int depth = 0;
    for (size_t i = open; i < t.size(); ++i) {
        if (t[i].kind != Tok::Punct) continue;
        if (t[i].text == o) {
            ++depth;
        } else if (t[i].text == c) {
            if (--depth == 0) return i;
        }
    }
    throw TranslateError("unbalanced '" + o + "'");
}

// Splits [from, to) at commas that are not inside brackets.
std::vector<Toks> split_args(const Toks& t, size_t from, size_t to) {
    std::vector<Toks> args;
    Toks cur;
    int depth = 0;
    for (size_t i = from; i < to; ++i) {
        const Tok& k = t[i];
        if (k.kind == Tok::Punct) {
            if (k.text == "(" || k.text == "[" || k.text == "{") ++depth;
            if (k.text == ")" || k.text == "]" || k.text == "}") --depth;
            if (k.text == "," && depth == 0) {
                args.push_back(std::move(cur));
                cur.clear();
                continue;
            }
        }
        cur.push_back(k);
    }
    if (!cur.empty() || !args.empty()) args.push_back(std::move(cur));
    return args;
}

void erase(Toks& t, size_t from, size_t to) {
    t.erase(t.begin() + static_cast<std::ptrdiff_t>(from), t.begin() + static_cast<std::ptrdiff_t>(to));
}
void insert(Toks& t, size_t at, const Toks& what) {
    t.insert(t.begin() + static_cast<std::ptrdiff_t>(at), what.begin(), what.end());
}

// ---- types ---------------------------------------------------------------------
enum class TexKind { None, Color2D, ColorArray, Cube, Depth2D, DepthArray };

std::string type_base(const std::string& msl_type) { return msl_type.substr(0, msl_type.find('<')); }

TexKind tex_kind(const std::string& msl_type) {
    const std::string b = type_base(msl_type);
    if (b == "texture2d") return TexKind::Color2D;
    if (b == "texture2d_array") return TexKind::ColorArray;
    if (b == "texturecube") return TexKind::Cube;
    if (b == "depth2d") return TexKind::Depth2D;
    if (b == "depth2d_array") return TexKind::DepthArray;
    return TexKind::None;
}
bool is_depth(TexKind k) { return k == TexKind::Depth2D || k == TexKind::DepthArray; }
bool is_array(TexKind k) { return k == TexKind::ColorArray || k == TexKind::DepthArray; }

// The combined sampler type a texture of `kind` becomes; `shadow` when it is sampled with
// sample_compare (the sampler object bound to it then has a comparison function).
std::string sampler_type(TexKind kind, bool shadow) {
    std::string s;
    switch (kind) {
        case TexKind::Color2D:
        case TexKind::Depth2D: s = "sampler2D"; break;
        case TexKind::ColorArray:
        case TexKind::DepthArray: s = "sampler2DArray"; break;
        case TexKind::Cube: s = "samplerCube"; break;
        case TexKind::None: throw TranslateError("not a texture type");
    }
    return shadow ? s + "Shadow" : s;
}

const std::map<std::string, std::string>& type_renames() {
    static const std::map<std::string, std::string> table = {
        {"float2", "vec2"},         {"float3", "vec3"},         {"float4", "vec4"},
        {"half", "float"},          {"half2", "vec2"},          {"half3", "vec3"},
        {"half4", "vec4"},          {"packed_float2", "vec2"},  {"packed_float3", "vec3"},
        {"packed_float4", "vec4"},  {"packed_half2", "vec2"},   {"packed_half3", "vec3"},
        {"packed_half4", "vec4"},   {"float2x2", "mat2"},       {"float3x3", "mat3"},
        {"float4x4", "mat4"},       {"half2x2", "mat2"},        {"half3x3", "mat3"},
        {"half4x4", "mat4"},        {"int2", "ivec2"},          {"int3", "ivec3"},
        {"int4", "ivec4"},          {"uint2", "uvec2"},         {"uint3", "uvec3"},
        {"uint4", "uvec4"},         {"bool2", "bvec2"},         {"bool3", "bvec3"},
        {"bool4", "bvec4"},         {"short", "int"},           {"ushort", "uint"},
        {"packed_int3", "ivec3"},   {"packed_uint3", "uvec3"},
    };
    return table;
}

std::string map_type(const std::string& msl_type) {
    const auto& table = type_renames();
    const auto it = table.find(msl_type);
    return it == table.end() ? msl_type : it->second;
}

bool is_integer_type(const std::string& t) {
    const std::string g = map_type(t);
    return g == "int" || g == "uint" || g.rfind("ivec", 0) == 0 || g.rfind("uvec", 0) == 0;
}

// The type strings of GlslShader::vertex_member_types: the HLSL translator's spelling, which the
// renderers' vertex layouts are keyed on.
std::string vertex_member_type(const std::string& msl_type) {
    std::string t = msl_type;
    if (t.rfind("packed_", 0) == 0) t = t.substr(7);
    if (t.rfind("half", 0) == 0) t = "float" + t.substr(4);
    return t;
}

// MSL functions with another name in GLSL, and MSL identifiers that are GLSL keywords, reserved
// words or built-in functions (never MSL function names, so renaming every occurrence is safe).
const std::map<std::string, std::string>& identifier_renames() {
    static const std::map<std::string, std::string> table = [] {
        std::map<std::string, std::string> m = {
            {"dfdx", "dFdx"}, {"dfdy", "dFdy"},   {"rsqrt", "inversesqrt"}, {"atan2", "atan"},
            {"fmod", "me_fmod"}, {"fabs", "abs"}, {"fmin", "min"},          {"fmax", "max"},
        };
        static const char* const kReserved[] = {
            "in",         "out",       "inout",      "input",     "output",    "sample",     "filter",
            "texture",    "mod",       "smooth",     "flat",      "common",    "partition",  "active",
            "precise",    "patch",     "buffer",     "shared",    "coherent",  "volatile",   "restrict",
            "readonly",   "writeonly", "layout",     "centroid",  "noperspective", "invariant", "subroutine",
            "lowp",       "mediump",   "highp",      "precision", "attribute", "varying",    "double",
            "superp",     "fixed",     "unsigned",   "long",      "asm",       "class",      "union",
            "enum",       "typedef",   "this",       "resource",  "goto",      "noinline",   "public",
            "extern",     "external",  "interface",  "sizeof",    "cast",      "namespace",  "uniform",
            "discard",    "atomic_uint", "main",     "dvec2",     "dvec3",     "dvec4",      "dmat2",
            "dmat3",      "dmat4",     "hvec2",      "hvec3",     "hvec4",     "fvec2",      "fvec3",
            "fvec4",      "sampler",   "image2D",    "iimage2D",  "uimage2D",  "imageCube",  "image1D",
            "image3D",    "sampler1D", "sampler2D",  "sampler3D", "samplerCube", "sampler2DArray",
            "sampler2DShadow", "sampler2DArrayShadow", "samplerCubeShadow", "sampler1DShadow",
        };
        for (const char* k : kReserved) m.emplace(k, std::string(k) + "_");
        return m;
    }();
    return table;
}

std::string rename_ident(const std::string& name) {
    const auto& types = type_renames();
    const auto t = types.find(name);
    if (t != types.end()) return t->second;
    const auto& table = identifier_renames();
    const auto it = table.find(name);
    return it == table.end() ? name : it->second;
}

// ---- declarations --------------------------------------------------------------
struct Member {
    std::string type;
    std::string name;
    std::string array;  // "[4]" or empty
    std::string attr;   // position / attribute / flat / ...
    int attr_index = -1;
};

struct Struct {
    std::string name;
    std::vector<Member> members;
    bool stage_io = false;      // has [[position]] / [[attribute(n)]] members
    bool vertex_input = false;  // read as `vertices[vertex_id]` by a vertex stage
};

struct Param {
    std::string type;  // MSL type, e.g. "texture2d<float>"
    std::string name;
    bool is_ref = false;
    bool is_ptr = false;
    bool is_const = false;
    std::string attr;
    int attr_index = -1;
};

// Which sampler a helper function's texture parameter is sampled with, found by following the
// body's sample() calls and the helpers it passes the texture on to. A caller supplies the
// combined sampler accordingly.
struct TexBinding {
    enum Kind { None, Param, Static } kind = None;
    std::string sampler;  // the sampler parameter's name, or the static sampler's name
    bool shadow = false;  // sampled with sample_compare somewhere down the chain
};

struct Func {
    enum Stage { Helper, Vertex, Fragment } stage = Helper;
    std::string ret;
    std::string name;
    std::vector<Param> params;
    Toks body;  // tokens between the braces
    std::vector<TexBinding> tex_bindings;  // per parameter (meaningful for texture parameters)
    bool analyzed = false;
    bool analyzing = false;

    [[nodiscard]] bool has_texture_params() const {
        for (const Param& p : params) {
            if (p.type == "sampler" || tex_kind(p.type) != TexKind::None) return true;
        }
        return false;
    }
    [[nodiscard]] int param_index(const std::string& n) const {
        for (size_t i = 0; i < params.size(); ++i) {
            if (params[i].name == n) return static_cast<int>(i);
        }
        return -1;
    }
};

struct Item {
    enum Kind { StructItem, GlobalItem, FuncItem } kind = FuncItem;
    size_t index = 0;
};

struct Unit {
    std::vector<Item> items;
    std::vector<Struct> structs;
    std::vector<Toks> globals;  // TYPE NAME [= expr] ;
    std::vector<Func> funcs;
};

bool is_qualifier(const std::string& s) {
    return s == "constant" || s == "const" || s == "constexpr" || s == "static" || s == "inline" || s == "device" ||
           s == "thread";
}

// Reads `name` or `name<args>` starting at i; leaves i after it.
std::string read_type(const Toks& t, size_t& i) {
    if (i >= t.size() || !is_ident(t[i])) throw TranslateError("expected a type near '" + join(t, i, i + 3) + "'");
    std::string type = t[i++].text;
    if (i < t.size() && is(t[i], "<")) {
        while (i < t.size()) {
            type += t[i].text;
            if (is(t[i++], ">")) break;
        }
    }
    return type;
}

// Parses a trailing [[name]] / [[name(n)]] in `t` and removes it.
void take_attribute(Toks& t, std::string& attr, int& attr_index) {
    for (size_t a = 0; a + 1 < t.size(); ++a) {
        if (!is(t[a], "[") || !is(t[a + 1], "[")) continue;
        if (a + 2 < t.size()) attr = t[a + 2].text;
        if (a + 4 < t.size() && is(t[a + 3], "(")) attr_index = std::atoi(t[a + 4].text.c_str());
        t.resize(a);
        return;
    }
}

Param parse_param(Toks t) {
    Param p;
    take_attribute(t, p.attr, p.attr_index);
    if (t.empty() || !is_ident(t.back())) throw TranslateError("cannot parse the argument '" + join(t) + "'");
    p.name = t.back().text;
    t.pop_back();
    while (!t.empty() && (is(t.back(), "&") || is(t.back(), "*"))) {
        (is(t.back(), "&") ? p.is_ref : p.is_ptr) = true;
        t.pop_back();
    }
    for (const Tok& k : t) {
        if (is_qualifier(k.text)) {
            if (k.text == "constant" || k.text == "const") p.is_const = true;
        } else {
            p.type += k.text;
        }
    }
    if (p.type.empty()) throw TranslateError("argument '" + p.name + "' has no type");
    return p;
}

Struct parse_struct(const Toks& t, size_t& i) {
    Struct s;
    ++i;  // struct
    s.name = t.at(i++).text;
    if (!is(t.at(i), "{")) throw TranslateError("expected '{' after struct " + s.name);
    const size_t close = match_close(t, i);
    size_t k = i + 1;
    while (k < close) {
        size_t end = k;
        while (end < close && !is(t[end], ";")) ++end;
        size_t d = k;
        const std::string type = read_type(t, d);
        for (Toks decl : split_args(t, d, end)) {
            Member m;
            m.type = type;
            take_attribute(decl, m.attr, m.attr_index);
            if (decl.empty() || !is_ident(decl[0])) throw TranslateError("cannot parse a member of struct " + s.name);
            m.name = decl[0].text;
            for (size_t a = 1; a < decl.size(); ++a) m.array += decl[a].text;
            if (m.attr == "position" || m.attr == "attribute") s.stage_io = true;
            s.members.push_back(std::move(m));
        }
        k = end + 1;
    }
    i = close + 1;
    if (i < t.size() && is(t[i], ";")) ++i;
    return s;
}

void replace_identifier(Toks& t, const std::string& from, const std::string& to) {
    for (Tok& k : t) {
        if (is_ident(k) && k.text == from) k.text = to;
    }
}

Unit parse_unit(const Toks& t) {
    Unit u;
    size_t i = 0;
    while (i < t.size()) {
        if (is(t[i], ";")) {
            ++i;
            continue;
        }
        if (is(t[i], "using")) {  // using namespace metal;
            while (i < t.size() && !is(t[i], ";")) ++i;
            continue;
        }
        if (is(t[i], "struct")) {
            u.structs.push_back(parse_struct(t, i));
            u.items.push_back({Item::StructItem, u.structs.size() - 1});
            continue;
        }

        std::string template_param;
        if (is(t[i], "template")) {  // template <typename T>
            if (i + 4 >= t.size() || !is(t[i + 1], "<") || !is(t[i + 4], ">")) {
                throw TranslateError("only `template <typename T>` is supported");
            }
            template_param = t[i + 3].text;
            i += 5;
        }

        Func::Stage stage = Func::Helper;
        while (i < t.size() && (is_qualifier(t[i].text) || is(t[i], "vertex") || is(t[i], "fragment"))) {
            if (is(t[i], "vertex")) stage = Func::Vertex;
            if (is(t[i], "fragment")) stage = Func::Fragment;
            ++i;
        }
        const size_t decl_start = i;
        const std::string type = read_type(t, i);
        if (i + 1 >= t.size() || !is_ident(t[i])) throw TranslateError("expected a name after '" + type + "'");
        const std::string name = t[i].text;

        if (!is(t[i + 1], "(")) {  // global constant
            size_t end = i;
            while (end < t.size() && !is(t[end], ";")) ++end;
            u.globals.emplace_back(t.begin() + static_cast<std::ptrdiff_t>(decl_start),
                                   t.begin() + static_cast<std::ptrdiff_t>(std::min(end + 1, t.size())));
            u.items.push_back({Item::GlobalItem, u.globals.size() - 1});
            i = end + 1;
            continue;
        }

        Func f;
        f.stage = stage;
        f.ret = type;
        f.name = name;
        const size_t popen = i + 1;
        const size_t pclose = match_close(t, popen);
        for (const Toks& pt : split_args(t, popen + 1, pclose)) {
            if (!pt.empty()) f.params.push_back(parse_param(pt));
        }
        f.tex_bindings.resize(f.params.size());
        if (pclose + 1 >= t.size() || !is(t[pclose + 1], "{")) {
            throw TranslateError("expected a body for function " + name);
        }
        const size_t bclose = match_close(t, pclose + 1);
        f.body.assign(t.begin() + static_cast<std::ptrdiff_t>(pclose + 2), t.begin() + static_cast<std::ptrdiff_t>(bclose));
        i = bclose + 1;

        if (template_param.empty()) {
            u.funcs.push_back(std::move(f));
            u.items.push_back({Item::FuncItem, u.funcs.size() - 1});
        } else {
            for (const char* concrete : {"float", "float2", "float3", "float4"}) {
                Func g = f;
                if (g.ret == template_param) g.ret = concrete;
                for (Param& p : g.params) {
                    if (p.type == template_param) p.type = concrete;
                }
                replace_identifier(g.body, template_param, concrete);
                u.funcs.push_back(std::move(g));
                u.items.push_back({Item::FuncItem, u.funcs.size() - 1});
            }
        }
    }
    return u;
}

// Everything a GLSL shader starts with, after the dialect's `#version` line (preamble_header):
// Metal built-ins GLSL lacks or spells differently.
const char* const kDesktopHeader = "#version 410 core\n";
// GLSL ES: default precisions for both stages. The fragment stage has no default float
// precision, and sampler types other than sampler2D / samplerCube have none in either stage.
const char* const kEsPrecision = R"glsl(precision highp float;
precision highp int;
precision highp sampler2D;
precision highp sampler2DArray;
precision highp sampler2DShadow;
precision highp sampler2DArrayShadow;
precision highp samplerCube;
precision highp samplerCubeShadow;
)glsl";
const char* const kPreambleBody = R"glsl(// Generated from Metal Shading Language by me::MslToGlsl (src/renderer/msl_to_glsl.cpp).
float saturate(float x) { return clamp(x, 0.0, 1.0); }
vec2 saturate(vec2 x) { return clamp(x, 0.0, 1.0); }
vec3 saturate(vec3 x) { return clamp(x, 0.0, 1.0); }
vec4 saturate(vec4 x) { return clamp(x, 0.0, 1.0); }
// Metal's fmod truncates the quotient where GLSL's mod floors it.
float me_fmod(float x, float y) { return x - y * trunc(x / y); }
vec2 me_fmod(vec2 x, vec2 y) { return x - y * trunc(x / y); }
vec3 me_fmod(vec3 x, vec3 y) { return x - y * trunc(x / y); }
vec4 me_fmod(vec4 x, vec4 y) { return x - y * trunc(x / y); }
vec2 me_fmod(vec2 x, float y) { return x - y * trunc(x / y); }
vec3 me_fmod(vec3 x, float y) { return x - y * trunc(x / y); }
vec4 me_fmod(vec4 x, float y) { return x - y * trunc(x / y); }
// Metal's select(a, b, c) is c ? b : a, per component.
float select(float a, float b, bool c) { return c ? b : a; }
int select(int a, int b, bool c) { return c ? b : a; }
vec2 select(vec2 a, vec2 b, bool c) { return c ? b : a; }
vec3 select(vec3 a, vec3 b, bool c) { return c ? b : a; }
vec4 select(vec4 a, vec4 b, bool c) { return c ? b : a; }
vec2 select(vec2 a, vec2 b, bvec2 c) { return mix(a, b, c); }
vec3 select(vec3 a, vec3 b, bvec3 c) { return mix(a, b, c); }
vec4 select(vec4 a, vec4 b, bvec4 c) { return mix(a, b, c); }
)glsl";

bool is_float_scalar_type(const std::string& msl_type) { return map_type(msl_type) == "float"; }
bool is_unsigned_type(const std::string& msl_type) {
    const std::string g = map_type(msl_type);
    return g == "uint" || g.rfind("uvec", 0) == 0;
}

}  // namespace

struct MslToGlsl::Impl {
    Unit unit;
    std::map<std::string, size_t> struct_index;
    std::vector<GlslStaticSampler> samplers;
    std::map<std::string, std::map<std::string, std::string>> sampler_alias;  // function -> MSL name -> static name
    GlslDialect dialect = GlslDialect::Desktop;
    int version = 410;

    [[nodiscard]] bool es() const { return dialect == GlslDialect::ES; }
    [[nodiscard]] std::string header() const {
        if (!es()) return kDesktopHeader;
        return "#version " + std::to_string(version) + " es\n" + kEsPrecision;
    }

    [[nodiscard]] bool is_struct(const std::string& name) const { return struct_index.count(name) != 0; }

    // The functions a body may call: the unit's, plus the ones of the piece of source
    // emit_from() is translating (analyzed on demand).
    struct FuncTable {
        const Impl* impl = nullptr;
        std::vector<Func>* extra = nullptr;

        // The helper `name` called with `nargs` arguments, if it takes textures or samplers.
        [[nodiscard]] const Func* texture_helper(const std::string& name, size_t nargs) const {
            if (extra) {
                for (Func& f : *extra) {
                    if (f.stage != Func::Helper || f.name != name || f.params.size() != nargs || !f.has_texture_params()) {
                        continue;
                    }
                    impl->analyze_textures(f, *this);
                    return &f;
                }
            }
            for (const Func& f : impl->unit.funcs) {
                if (f.stage != Func::Helper || f.name != name || f.params.size() != nargs || !f.has_texture_params()) {
                    continue;
                }
                if (!f.analyzed) throw TranslateError("helper " + name + " was not analyzed");
                return &f;
            }
            return nullptr;
        }

        // The MSL parameter types of the helper(s) named `name` taking `nargs` arguments, or an
        // empty vector when there is none. Where overloads (a template's instances) disagree on a
        // position's type, that position is "".
        [[nodiscard]] std::vector<std::string> param_types(const std::string& name, size_t nargs) const {
            std::vector<std::string> types;
            bool any = false;
            auto consider = [&](const Func& f) {
                if (f.stage != Func::Helper || f.name != name || f.params.size() != nargs) return;
                if (!any) {
                    any = true;
                    for (const Param& p : f.params) types.push_back(p.type);
                    return;
                }
                for (size_t i = 0; i < nargs; ++i) {
                    if (types[i] != f.params[i].type) types[i].clear();
                }
            };
            if (extra) {
                for (const Func& f : *extra) consider(f);
            }
            for (const Func& f : impl->unit.funcs) consider(f);
            return types;
        }
    };

    // A sampler named in a function body: a sampler parameter or a local constexpr sampler.
    struct SamplerRef {
        std::string name;
        bool is_static = false;
        bool any = false;  // no sampler involved (get_width / get_height)
    };

    // Names visible in a function body whose type decides how the body is rewritten.
    struct Scope {
        std::string function;
        std::map<std::string, TexKind> textures;          // texture parameters, by name
        std::set<std::string> samplers;                   // sampler parameters, by name
        std::map<std::string, std::string> static_alias;  // local constexpr sampler -> static sampler name
        std::map<std::string, std::string> types;         // parameters, by name -> MSL type
        std::string pull_pointer;  // `pull_pointer[pull_index]` is the stage's input vertex
        std::string pull_index;
        // The GLSL name of texture `tex` sampled through `s` (`shadow`: with sample_compare).
        std::function<std::string(const std::string& tex, const SamplerRef& s, bool shadow)> combined;

        [[nodiscard]] SamplerRef resolve_sampler(const Toks& arg) const {
            if (arg.size() != 1 || !is_ident(arg[0])) {
                throw TranslateError("the sampler argument '" + join(arg) + "' in " + function + " is not a name");
            }
            SamplerRef r;
            r.name = arg[0].text;
            if (samplers.count(r.name)) return r;
            const auto st = static_alias.find(r.name);
            if (st != static_alias.end()) {
                r.name = st->second;
                r.is_static = true;
                return r;
            }
            throw TranslateError("'" + r.name + "' in " + function + " is not a sampler argument or a constexpr sampler");
        }
    };

    [[nodiscard]] Scope scope_of(const Func& f) const {
        Scope scope;
        scope.function = f.name;
        for (const Param& p : f.params) {
            const TexKind kind = tex_kind(p.type);
            if (kind != TexKind::None) scope.textures[p.name] = kind;
            if (p.type == "sampler") scope.samplers.insert(p.name);
            scope.types[p.name] = p.type;
        }
        const auto fn = sampler_alias.find(f.name);
        if (fn != sampler_alias.end()) scope.static_alias = fn->second;
        return scope;
    }

    // ---- constexpr samplers -----------------------------------------------------
    void register_samplers(const Func& f) {
        const Toks& t = f.body;
        for (size_t i = 0; i + 3 < t.size(); ++i) {
            if (!is(t[i], "constexpr") || !is(t[i + 1], "sampler") || !is(t[i + 3], "(")) continue;
            GlslStaticSampler s;
            s.name = "me_ss_" + f.name + "_" + t[i + 2].text;
            s.slot = 14 - static_cast<int>(samplers.size());  // the HLSL translator's scheme
            const size_t close = match_close(t, i + 3);
            for (size_t k = i + 4; k < close; ++k) {
                if (!is(t[k], "::") || k + 1 >= close) continue;
                const std::string& key = t[k - 1].text;
                const std::string& value = t[k + 1].text;
                if (key == "filter" || key == "mag_filter" || key == "min_filter") s.linear = (value == "linear");
                if (key == "address" || key == "s_address" || key == "t_address") s.repeat = (value == "repeat");
                if (key == "compare_func") s.compare = true;
            }
            if (s.slot < 0) throw TranslateError("too many constexpr samplers");
            sampler_alias[f.name][t[i + 2].text] = s.name;
            samplers.push_back(std::move(s));
        }
    }

    [[nodiscard]] const GlslStaticSampler& static_sampler(const std::string& name) const {
        for (const GlslStaticSampler& s : samplers) {
            if (s.name == name) return s;
        }
        throw TranslateError("unknown constexpr sampler " + name);
    }

    // ---- texture uses -----------------------------------------------------------
    // Walks a body's texture uses: `tex.sample(s, ...)`, `tex.sample_compare(...)`, `tex.get_width()`
    // and calls of helpers that take textures. Each use is reported to scope.combined(); with
    // `rewrite` the tokens are replaced by their GLSL (combined sampler) form.
    void walk_texture_uses(Toks& t, Scope& scope, const FuncTable& table, bool rewrite) const {
        for (size_t i = 0; i < t.size(); ++i) {
            if (!is_ident(t[i])) continue;
            if (i > 0 && is(t[i - 1], ".")) continue;

            // tex.method(...)
            if (i + 3 < t.size() && is(t[i + 1], ".") && is_ident(t[i + 2]) && is(t[i + 3], "(") &&
                scope.textures.count(t[i].text)) {
                const std::string receiver = t[i].text;
                const std::string method = t[i + 2].text;
                const TexKind kind = scope.textures.at(receiver);
                const size_t close = match_close(t, i + 3);
                const std::vector<Toks> args = split_args(t, i + 4, close);
                Toks repl;
                auto add = [&repl](const Toks& a) { repl.insert(repl.end(), a.begin(), a.end()); };
                auto vec = [&](const char* type, std::initializer_list<const Toks*> parts) {
                    repl.push_back(ident(type));
                    repl.push_back(punct("("));
                    bool first = true;
                    for (const Toks* p : parts) {
                        if (!first) repl.push_back(punct(","));
                        first = false;
                        add(*p);
                    }
                    repl.push_back(punct(")"));
                };

                if (method == "get_width" || method == "get_height") {
                    const std::string name = scope.combined(receiver, SamplerRef{"", false, true}, false);
                    repl = {glsl("textureSize", t[i].ws), punct("("), glsl(name), punct(","), number("0", " "),
                            punct(")"), punct("."), glsl(method == "get_width" ? "x" : "y")};
                } else if (method == "sample_compare") {
                    if (args.size() != (is_array(kind) ? 4u : 3u)) {
                        throw TranslateError("unsupported sample_compare() form on " + receiver);
                    }
                    const std::string name = scope.combined(receiver, scope.resolve_sampler(args[0]), true);
                    repl = {glsl("texture", t[i].ws), punct("("), glsl(name), punct(",")};
                    if (is_array(kind)) {
                        vec("vec4", {&args[1], &args[2], &args[3]});
                    } else {
                        vec(kind == TexKind::Cube ? "vec4" : "vec3", {&args[1], &args[2]});
                    }
                    repl.push_back(punct(")"));
                } else if (method == "sample") {
                    const size_t base = is_array(kind) ? 3u : 2u;
                    if (args.size() != base && args.size() != base + 1) {
                        throw TranslateError("unsupported sample() form on " + receiver);
                    }
                    const std::string name = scope.combined(receiver, scope.resolve_sampler(args[0]), false);
                    Toks option;
                    bool lod = false;
                    if (args.size() == base + 1) {  // level(x) / bias(x)
                        const Toks& o = args.back();
                        if (o.size() < 4 || !is(o[1], "(") || (!is(o[0], "level") && !is(o[0], "bias"))) {
                            throw TranslateError("unsupported sample() option on " + receiver);
                        }
                        lod = is(o[0], "level");
                        option.assign(o.begin() + 2, o.end() - 1);
                    }
                    // Depth maps have one level; an explicit level also keeps the lookup legal in loops.
                    if (is_depth(kind) && option.empty()) {
                        lod = true;
                        option = {number("0.0", " ")};
                    }
                    repl = {glsl(lod ? "textureLod" : "texture", t[i].ws), punct("("), glsl(name), punct(",")};
                    if (is_array(kind)) {
                        vec("vec3", {&args[1], &args[2]});
                    } else {
                        add(args[1]);
                    }
                    if (!option.empty()) {
                        repl.push_back(punct(","));
                        add(option);
                    }
                    repl.push_back(punct(")"));
                    if (is_depth(kind)) {
                        repl.push_back(punct("."));
                        repl.push_back(ident("r"));
                    }
                } else {
                    throw TranslateError("unsupported texture method ." + method + "() on " + receiver);
                }
                if (rewrite) {
                    erase(t, i, close + 1);
                    insert(t, i, repl);
                }
                continue;  // the arguments are scanned next (they may hold further texture uses)
            }

            // helper(..., tex, ..., sampler, ...)
            if (i + 1 < t.size() && is(t[i + 1], "(")) {
                const size_t close = match_close(t, i + 1);
                const std::vector<Toks> args = split_args(t, i + 2, close);
                const Func* callee = table.texture_helper(t[i].text, args.size());
                if (!callee) continue;
                Toks repl = {t[i], punct("(")};
                bool first = true;
                for (size_t j = 0; j < args.size(); ++j) {
                    const Param& p = callee->params[j];
                    if (p.type == "sampler") continue;  // folded into the texture it samples
                    if (!first) repl.push_back(punct(","));
                    first = false;
                    const TexKind kind = tex_kind(p.type);
                    if (kind == TexKind::None) {
                        repl.insert(repl.end(), args[j].begin(), args[j].end());
                        continue;
                    }
                    if (args[j].size() != 1 || !is_ident(args[j][0]) || !scope.textures.count(args[j][0].text)) {
                        throw TranslateError("the texture argument '" + join(args[j]) + "' passed to " + callee->name +
                                             " in " + scope.function + " is not a texture argument of " + scope.function);
                    }
                    const std::string tex = args[j][0].text;
                    const TexBinding& b = callee->tex_bindings[j];
                    SamplerRef s;
                    if (b.kind == TexBinding::None) {
                        s.any = true;
                    } else if (b.kind == TexBinding::Static) {
                        s.name = b.sampler;
                        s.is_static = true;
                    } else {
                        const int k = callee->param_index(b.sampler);
                        if (k < 0 || static_cast<size_t>(k) >= args.size()) {
                            throw TranslateError("sampler " + b.sampler + " of " + callee->name + " has no argument");
                        }
                        s = scope.resolve_sampler(args[static_cast<size_t>(k)]);
                    }
                    repl.push_back(ident(scope.combined(tex, s, b.shadow), args[j][0].ws));
                }
                repl.push_back(punct(")"));
                if (rewrite) {
                    erase(t, i, close + 1);
                    insert(t, i, repl);
                }
            }
        }
    }

    // Works out which sampler each texture parameter of helper `f` is sampled with.
    void analyze_textures(Func& f, const FuncTable& table) const {
        if (f.analyzed) return;
        if (f.analyzing) throw TranslateError("helper " + f.name + " calls itself");
        f.analyzing = true;
        Scope scope = scope_of(f);
        scope.combined = [&](const std::string& tex, const SamplerRef& s, bool shadow) {
            const int idx = f.param_index(tex);
            if (idx < 0) throw TranslateError("'" + tex + "' is not an argument of " + f.name);
            TexBinding& b = f.tex_bindings[static_cast<size_t>(idx)];
            b.shadow = b.shadow || shadow;
            if (!s.any) {
                const TexBinding::Kind kind = s.is_static ? TexBinding::Static : TexBinding::Param;
                if (b.kind != TexBinding::None && (b.kind != kind || b.sampler != s.name)) {
                    throw TranslateError("texture '" + tex + "' of " + f.name + " is sampled with more than one sampler");
                }
                b.kind = kind;
                b.sampler = s.name;
            }
            return tex;
        };
        Toks body = f.body;
        walk_texture_uses(body, scope, table, false);
        f.analyzing = false;
        f.analyzed = true;
    }

    // ---- function bodies --------------------------------------------------------
    [[nodiscard]] std::string translate_body(const Toks& body, Scope& scope, const FuncTable& table) const {
        Toks t = body;

        // `constexpr sampler name(...);` goes: the sampler lives in the combined sampler uniform.
        for (size_t i = 0; i + 3 < t.size(); ++i) {
            if (!is(t[i], "constexpr") || !is(t[i + 1], "sampler") || !is(t[i + 3], "(")) continue;
            if (scope.static_alias.count(t[i + 2].text) == 0) {
                throw TranslateError("constexpr sampler '" + t[i + 2].text + "' in " + scope.function + " was not registered");
            }
            size_t end = match_close(t, i + 3) + 1;
            if (end < t.size() && is(t[end], ";")) ++end;
            const std::string ws = t[i].ws;
            erase(t, i, end);
            if (i < t.size()) t[i].ws = ws + t[i].ws;
            --i;
        }

        // `vertices[vertex_id]` -> the vertex assembled from the attributes.
        if (!scope.pull_pointer.empty()) {
            for (size_t i = 0; i < t.size(); ++i) {
                if (!is_ident(t[i]) || t[i].text != scope.pull_pointer) continue;
                if (i + 3 >= t.size() || !is(t[i + 1], "[") || t[i + 2].text != scope.pull_index || !is(t[i + 3], "]")) {
                    throw TranslateError("'" + scope.pull_pointer + "' may only be read as " + scope.pull_pointer + "[" +
                                         scope.pull_index + "]");
                }
                t[i].text = "me_vin";
                erase(t, i + 1, i + 4);
            }
        }

        // metal::x -> x
        for (size_t i = 0; i + 1 < t.size(); ++i) {
            if (is(t[i], "metal") && is(t[i + 1], "::")) {
                const std::string ws = t[i].ws;
                erase(t, i, i + 2);
                if (i < t.size()) t[i].ws = ws + t[i].ws;
                --i;
            }
        }

        // as_type<T>(x) -> floatBitsToUint(x) and friends
        for (size_t i = 0; i + 4 < t.size(); ++i) {
            if (!is(t[i], "as_type") || !is(t[i + 1], "<") || !is(t[i + 3], ">") || !is(t[i + 4], "(")) continue;
            const std::string& to = t[i + 2].text;
            const char* fn = nullptr;
            if (to == "uint") fn = "floatBitsToUint";
            if (to == "int") fn = "floatBitsToInt";
            if (to == "float") fn = "uintBitsToFloat";  // from int as well: GLSL converts the argument
            if (!fn) throw TranslateError("unsupported as_type<" + to + ">");
            t[i].text = fn;
            erase(t, i + 1, i + 4);
        }

        // GLSL ES converts nothing implicitly: `helper(maps, 0, ...)` for a uint parameter, or
        // `helper(1)` for a float one, needs the literal spelt in the parameter's type. Done
        // before the texture rewrite below drops sampler arguments, so positions still match.
        if (es()) match_call_literals(t, table);

        walk_texture_uses(t, scope, table, true);

        // `T a[N] = {...}` -> `T a[N] = T[N](...)`
        for (size_t i = 0; i + 5 < t.size(); ++i) {
            if (!is_ident(t[i]) || !is_ident(t[i + 1]) || !is(t[i + 2], "[")) continue;
            const size_t rb = match_close(t, i + 2);
            if (rb + 2 >= t.size() || !is(t[rb + 1], "=") || !is(t[rb + 2], "{")) continue;
            const size_t close = match_close(t, rb + 2);
            t[close] = punct(")");
            Toks ctor = {ident(map_type(t[i].text), " ")};
            for (size_t k = i + 2; k <= rb; ++k) ctor.push_back(punct(t[k].text));
            ctor.push_back(punct("("));
            erase(t, rb + 2, rb + 3);
            insert(t, rb + 2, ctor);
        }

        match_literal_signedness(t, scope);

        for (size_t i = 0; i < t.size(); ++i) {
            if (!is_ident(t[i]) || t[i].glsl) continue;
            if (t[i].text == "discard_fragment" && i + 2 < t.size() && is(t[i + 1], "(") && is(t[i + 2], ")")) {
                t[i].text = "discard";
                erase(t, i + 1, i + 3);
                continue;
            }
            t[i].text = rename_ident(t[i].text);
        }
        return join(t);
    }

    // `uint_value & 0xFF` -> `uint_value & 0xFFu`: C++ converts the int literal, GLSL's bitwise
    // operators want both operands of one signedness (and Apple's compiler enforces it). Only
    // literals next to an operand whose declared type is known to be unsigned are touched.
    void match_literal_signedness(Toks& t, const Scope& scope) const {
        std::map<std::string, std::string> types = scope.types;  // name -> MSL type
        for (size_t i = 0; i + 2 < t.size(); ++i) {
            if (!is_ident(t[i]) || !is_ident(t[i + 1])) continue;
            if (!(is(t[i + 2], "=") || is(t[i + 2], ";") || is(t[i + 2], ",") || is(t[i + 2], "["))) continue;
            if (type_renames().count(t[i].text) || t[i].text == "uint" || t[i].text == "int" || t[i].text == "float" ||
                t[i].text == "bool" || is_struct(t[i].text)) {
                types[t[i + 1].text] = t[i].text;
            }
        }
        auto is_unsigned = [](const std::string& type) {
            const std::string g = map_type(type);
            return g == "uint" || g.rfind("uvec", 0) == 0;
        };
        // The type of the postfix expression (name, .member chain, [index]) at `from`, past
        // parentheses and unary operators; "" when unknown.
        std::function<std::string(size_t, size_t)> leading_type = [&](size_t from, size_t to) -> std::string {
            while (from < to && (is(t[from], "(") || is(t[from], "-") || is(t[from], "+") || is(t[from], "~"))) ++from;
            if (from >= to || !is_ident(t[from])) return "";
            const auto it = types.find(t[from].text);
            if (it == types.end()) return "";
            std::string type = it->second;
            size_t k = from + 1;
            while (k < to) {
                if (is(t[k], "[")) {
                    k = match_close(t, k) + 1;
                    const size_t lb = type.find('[');
                    if (lb != std::string::npos) type = type.substr(0, lb);
                    else if (map_type(type).rfind("uvec", 0) == 0) type = "uint";
                    else if (map_type(type).rfind("ivec", 0) == 0) type = "int";
                    else return "";
                } else if (is(t[k], ".") && k + 1 < to && is_ident(t[k + 1])) {
                    const std::string& m = t[k + 1].text;
                    k += 2;
                    if (is_struct(type)) {
                        std::string member_type;
                        for (const Member& mem : unit.structs[struct_index.at(type)].members) {
                            if (mem.name == m) member_type = mem.type + mem.array;
                        }
                        if (member_type.empty()) return "";
                        type = member_type;
                    } else if (m.size() == 1 && map_type(type).rfind("uvec", 0) == 0) {
                        type = "uint";
                    } else if (m.size() == 1 && map_type(type).rfind("ivec", 0) == 0) {
                        type = "int";
                    } else if (m.size() > 1) {
                        // a swizzle keeps the base type's signedness
                    } else {
                        return "";
                    }
                } else {
                    break;
                }
            }
            return type;
        };
        auto is_int_literal = [](const Tok& k) {
            if (k.kind != Tok::Number) return false;
            const bool hex = k.text.rfind("0x", 0) == 0 || k.text.rfind("0X", 0) == 0;
            const char last = k.text.back();
            if (last == 'u' || last == 'U') return false;
            if (hex) return true;
            return k.text.find_first_of(".eEfF") == std::string::npos;
        };
        for (size_t i = 1; i + 1 < t.size(); ++i) {
            if (!(is(t[i], "&") || is(t[i], "|") || is(t[i], "^") || is(t[i], "%"))) continue;
            // The left operand's start: back over a postfix expression.
            size_t start = i;
            while (start > 0) {
                const Tok& k = t[start - 1];
                if (is(k, ")") || is(k, "]")) {
                    const std::string open = is(k, ")") ? "(" : "[";
                    int depth = 0;
                    size_t j = start - 1;
                    for (;; --j) {
                        if (t[j].text == k.text) ++depth;
                        if (t[j].text == open && --depth == 0) break;
                        if (j == 0) break;
                    }
                    start = j;
                } else if (is_ident(k) || k.kind == Tok::Number || is(k, ".")) {
                    --start;
                } else {
                    break;
                }
            }
            const bool left_unsigned = is_unsigned(leading_type(start, i));
            const bool right_unsigned = is_unsigned(leading_type(i + 1, t.size()));
            if (right_unsigned && is_int_literal(t[start]) && start + 1 == i) t[start].text += "u";
            if (left_unsigned && is_int_literal(t[i + 1])) t[i + 1].text += "u";
        }
    }

    // GLSL ES only: an integer literal given to a helper whose parameter is `uint` becomes `0u`,
    // one given for a `float` parameter `0.0`. Desktop GLSL (4.10) converts both by itself; GLSL
    // ES rejects the call ("no matching overloaded function"). A literal with a unary minus is
    // handled; anything else is left to the compiler.
    void match_call_literals(Toks& t, const FuncTable& table) const {
        auto is_int_literal = [](const Tok& k) {
            if (k.kind != Tok::Number) return false;
            const bool hex = k.text.rfind("0x", 0) == 0 || k.text.rfind("0X", 0) == 0;
            const char last = k.text.back();
            if (last == 'u' || last == 'U') return false;
            if (hex) return true;
            return k.text.find_first_of(".eEfF") == std::string::npos;
        };
        for (size_t i = 0; i + 1 < t.size(); ++i) {
            if (!is_ident(t[i]) || t[i].glsl || !is(t[i + 1], "(")) continue;
            if (i > 0 && is(t[i - 1], ".")) continue;  // a texture method
            const size_t close = match_close(t, i + 1);
            // The arguments' token ranges, split at the commas of this call.
            std::vector<std::pair<size_t, size_t>> args;
            size_t from = i + 2;
            int depth = 0;
            for (size_t k = i + 2; k < close; ++k) {
                if (t[k].kind != Tok::Punct) continue;
                if (is(t[k], "(") || is(t[k], "[") || is(t[k], "{")) ++depth;
                if (is(t[k], ")") || is(t[k], "]") || is(t[k], "}")) --depth;
                if (is(t[k], ",") && depth == 0) {
                    args.emplace_back(from, k);
                    from = k + 1;
                }
            }
            if (from < close || !args.empty()) args.emplace_back(from, close);
            const std::vector<std::string> types = table.param_types(t[i].text, args.size());
            if (types.empty()) continue;
            for (size_t j = 0; j < args.size(); ++j) {
                size_t lit = args[j].first;
                if (lit < args[j].second && (is(t[lit], "-") || is(t[lit], "+"))) ++lit;
                if (lit + 1 != args[j].second || !is_int_literal(t[lit])) continue;
                if (is_unsigned_type(types[j])) {
                    t[lit].text += "u";
                } else if (is_float_scalar_type(types[j])) {
                    const bool hex = t[lit].text.rfind("0x", 0) == 0 || t[lit].text.rfind("0X", 0) == 0;
                    if (hex) t[lit].text = std::to_string(std::strtoul(t[lit].text.c_str(), nullptr, 16));
                    t[lit].text += ".0";
                }
            }
        }
    }

    // ---- declarations -----------------------------------------------------------
    [[nodiscard]] static std::string struct_glsl(const Struct& s) {
        if (s.stage_io && s.vertex_input) throw TranslateError("struct " + s.name + " is both stage data and a vertex buffer");
        std::ostringstream o;
        o << "struct " << s.name << " {\n";
        for (const Member& m : s.members) o << "    " << map_type(m.type) << " " << rename_ident(m.name) << m.array << ";\n";
        o << "};\n";
        return o.str();
    }

    [[nodiscard]] std::string global_glsl(const Toks& decl, const FuncTable& table) const {
        size_t i = 0;
        const std::string type = read_type(decl, i);
        Scope scope;
        scope.function = "<global>";
        const Toks rest(decl.begin() + static_cast<std::ptrdiff_t>(i), decl.end());
        return "const " + map_type(type) + translate_body(rest, scope, table) + "\n";
    }

    [[nodiscard]] std::string helper_glsl(const Func& f, const FuncTable& table) const {
        if (!f.analyzed) throw TranslateError("helper " + f.name + " was not analyzed");
        std::ostringstream o;
        o << map_type(f.ret) << " " << rename_ident(f.name) << "(";
        bool first = true;
        for (size_t i = 0; i < f.params.size(); ++i) {
            const Param& p = f.params[i];
            if (p.is_ptr) throw TranslateError("pointer argument '" + p.name + "' in helper function " + f.name);
            if (p.type == "sampler") continue;  // folded into the texture it samples
            if (!first) o << ", ";
            first = false;
            const TexKind kind = tex_kind(p.type);
            if (kind != TexKind::None) {
                o << sampler_type(kind, f.tex_bindings[i].shadow) << " " << rename_ident(p.name);
                continue;
            }
            if (p.is_ref && !p.is_const) o << "inout ";
            o << map_type(p.type) << " " << rename_ident(p.name);
        }
        Scope scope = scope_of(f);
        scope.combined = [](const std::string& tex, const SamplerRef&, bool) { return rename_ident(tex); };
        o << ") {" << translate_body(f.body, scope, table) << "}\n";
        return o.str();
    }

    // The stage's vertex struct, when it reads `pointer[vertex_id]` from a [[buffer(n)]] argument.
    [[nodiscard]] const Param* pulled_vertices(const Func& f, std::string* index_name) const {
        if (f.stage != Func::Vertex) return nullptr;
        const Param* vid = nullptr;
        for (const Param& p : f.params) {
            if (p.attr == "vertex_id") vid = &p;
        }
        if (!vid) return nullptr;
        for (const Param& p : f.params) {
            if (p.attr == "buffer" && p.is_ptr && is_struct(p.type)) {
                if (index_name) *index_name = vid->name;
                return &p;
            }
        }
        return nullptr;
    }

    // The combined sampler uniforms of one entry point.
    struct EntryUnits {
        std::map<std::string, std::pair<int, TexKind>> textures;  // entry texture name -> slot, kind
        std::map<std::string, int> sampler_slots;                 // entry sampler name -> slot
        std::vector<GlslSamplerUnit> units;
        std::vector<std::string> unit_tex;  // units[i]'s texture, for its GLSL type

        // Finds or declares the uniform for (tex, s); `s.any` picks any uniform of `tex`, declaring
        // one with the default sampler only if `declare_any`.
        std::string resolve(const std::string& tex, const SamplerRef& s, bool shadow, bool declare_any,
                            const Impl& impl) {
            const auto tex_it = textures.find(tex);
            if (tex_it == textures.end()) throw TranslateError("'" + tex + "' is not a texture argument");
            std::string suffix;
            int sampler_slot = -1;
            bool is_static = false;
            if (s.any) {
                for (size_t i = 0; i < units.size(); ++i) {
                    if (unit_tex[i] == tex) return units[i].uniform;
                }
                if (!declare_any) return "";
                suffix = "default";
            } else if (s.is_static) {
                suffix = s.name;
                sampler_slot = impl.static_sampler(s.name).slot;
                is_static = true;
            } else {
                const auto sm = sampler_slots.find(s.name);
                if (sm == sampler_slots.end()) throw TranslateError("'" + s.name + "' is not a sampler argument");
                suffix = rename_ident(s.name);
                sampler_slot = sm->second;
            }
            const std::string name = rename_ident(tex) + "_smp_" + suffix;
            for (size_t i = 0; i < units.size(); ++i) {
                if (units[i].uniform != name) continue;
                if (units[i].shadow != shadow) {
                    throw TranslateError("texture '" + tex + "' is sampled both with and without a comparison");
                }
                return name;
            }
            GlslSamplerUnit u;
            u.uniform = name;
            u.texture_slot = tex_it->second.first;
            u.sampler_slot = sampler_slot;
            u.static_sampler = is_static;
            u.shadow = shadow;
            units.push_back(u);
            unit_tex.push_back(tex);
            return name;
        }
    };

    [[nodiscard]] std::string entry_glsl(const Func& f, int pointer_array_size, const FuncTable& table,
                                         GlslShader& out) const {
        if (f.stage == Func::Helper) throw TranslateError(f.name + " is not a vertex or fragment function");
        const bool vertex = f.stage == Func::Vertex;
        const char* stage = vertex ? "VS" : "FS";
        Scope scope = scope_of(f);
        std::string pull_index;
        const Param* pulled = pulled_vertices(f, &pull_index);

        // Resources become globals; what the GLSL entry function keeps as arguments.
        std::ostringstream globals;
        std::vector<std::string> args;       // the GLSL function's parameters
        std::vector<std::string> call_args;  // what main() passes for them
        std::vector<std::pair<std::string, std::string>> block_aliases;  // argument name -> block member
        const Struct* stage_in = nullptr;
        EntryUnits units;
        for (const Param& p : f.params) {
            const std::string name = rename_ident(p.name);
            const TexKind kind = tex_kind(p.type);
            if (p.attr == "stage_in") {
                if (!is_struct(p.type)) throw TranslateError("[[stage_in]] argument '" + p.name + "' is not a struct");
                stage_in = &unit.structs[struct_index.at(p.type)];
                args.push_back(p.type + " " + name);
                call_args.push_back("me_in");
            } else if (p.attr == "vertex_id") {
                args.push_back("uint " + name);
                call_args.push_back("uint(gl_VertexID)");
            } else if (p.attr == "front_facing") {
                args.push_back("bool " + name);
                call_args.push_back("gl_FrontFacing");
            } else if (p.attr == "position" && !vertex) {
                args.push_back("vec4 " + name);
                call_args.push_back("gl_FragCoord");
            } else if (p.attr == "buffer" && &p == pulled) {
                const Struct& s = unit.structs[struct_index.at(p.type)];
                if (!s.vertex_input) throw TranslateError("vertex struct " + s.name + " was not declared with add_source()");
                scope.pull_pointer = p.name;
                scope.pull_index = pull_index;
                args.push_back(p.type + " me_vin");
                call_args.push_back("me_vin");
                for (const Member& m : s.members) out.vertex_member_types.push_back(vertex_member_type(m.type));
            } else if (p.attr == "buffer") {
                if (p.is_ptr && is_struct(p.type)) {
                    throw TranslateError("buffer argument '" + p.name + "' of " + f.name + ": only the vertex struct may be a pointer");
                }
                // A block's member is a global of the linked program, so the two stages' members must
                // not share a name (NVIDIA's and Intel's linkers refuse "uniforms" in both VSB1 and
                // FSB0). The member carries its block's name; the entry function sees it under the
                // argument's name through a #define that lasts for that function only.
                const std::string block = std::string(stage) + "B" + std::to_string(p.attr_index);
                const std::string member = block + "_" + name;
                globals << "layout(std140) uniform " << block << " { " << map_type(p.type) << " " << member;
                if (p.is_ptr) globals << "[" << std::max(1, pointer_array_size) << "]";
                globals << "; };\n";
                block_aliases.emplace_back(name, member);
                out.uniform_blocks.push_back({block, p.attr_index});
            } else if (p.attr == "texture") {
                if (kind == TexKind::None) throw TranslateError("texture argument '" + p.name + "' of unknown type " + p.type);
                units.textures[p.name] = {p.attr_index, kind};
            } else if (p.attr == "sampler") {
                for (const GlslStaticSampler& s : samplers) {
                    if (s.slot == p.attr_index) {
                        throw TranslateError("sampler slot " + std::to_string(s.slot) + " of " + f.name +
                                             " is taken by a constexpr sampler");
                    }
                }
                units.sampler_slots[p.name] = p.attr_index;
            } else {
                throw TranslateError("unsupported argument '" + p.name + "' of " + f.name);
            }
        }

        // The body: first every sampled (texture, sampler) pair is collected, so that a texture
        // only measured with get_width() is paired with a sampler it is sampled with elsewhere.
        scope.combined = [&](const std::string& tex, const SamplerRef& s, bool shadow) {
            return units.resolve(tex, s, shadow, false, *this);
        };
        {
            Toks probe = f.body;
            walk_texture_uses(probe, scope, table, false);
        }
        scope.combined = [&](const std::string& tex, const SamplerRef& s, bool shadow) {
            return units.resolve(tex, s, shadow, true, *this);
        };
        const std::string body = translate_body(f.body, scope, table);
        for (size_t i = 0; i < units.units.size(); ++i) {
            const GlslSamplerUnit& u = units.units[i];
            globals << "uniform " << sampler_type(units.textures.at(units.unit_tex[i]).second, u.shadow) << " " << u.uniform
                    << ";\n";
        }
        out.samplers = units.units;

        // Attributes, varyings and the colour output.
        std::ostringstream io;
        std::ostringstream main;
        main << "void main() {\n";
        if (vertex) {
            if (pulled) {
                const Struct& s = unit.structs[struct_index.at(pulled->type)];
                main << "    " << s.name << " me_vin;\n";
                for (size_t i = 0; i < s.members.size(); ++i) {
                    const Member& m = s.members[i];
                    if (!m.array.empty()) throw TranslateError("array member " + m.name + " in vertex struct " + s.name);
                    io << "layout(location = " << i << ") in " << map_type(m.type) << " in_" << m.name << ";\n";
                    main << "    me_vin." << rename_ident(m.name) << " = in_" << m.name << ";\n";
                }
            }
            if (stage_in) {
                main << "    " << stage_in->name << " me_in;\n";
                for (const Member& m : stage_in->members) {
                    if (m.attr != "attribute") throw TranslateError("vertex [[stage_in]] member " + m.name + " has no [[attribute(n)]]");
                    io << "layout(location = " << m.attr_index << ") in " << map_type(m.type) << " in_" << m.name << ";\n";
                    main << "    me_in." << rename_ident(m.name) << " = in_" << m.name << ";\n";
                }
            }
        } else if (stage_in) {
            main << "    " << stage_in->name << " me_in;\n";
            for (const Member& m : stage_in->members) {
                if (m.attr == "position") {
                    main << "    me_in." << rename_ident(m.name) << " = gl_FragCoord;\n";
                    continue;
                }
                const bool flat = m.attr == "flat" || is_integer_type(m.type);
                io << (flat ? "flat in " : "in ") << map_type(m.type) << " v_" << m.name << m.array << ";\n";
                main << "    me_in." << rename_ident(m.name) << " = v_" << m.name << ";\n";
            }
        }

        std::string call = rename_ident(f.name) + "(";
        for (size_t i = 0; i < call_args.size(); ++i) call += (i ? ", " : "") + call_args[i];
        call += ")";
        if (vertex) {
            if (is_struct(f.ret)) {
                const Struct& s = unit.structs[struct_index.at(f.ret)];
                main << "    " << s.name << " me_out = " << call << ";\n";
                for (const Member& m : s.members) {
                    if (m.attr == "position") {
                        main << "    gl_Position = me_out." << rename_ident(m.name) << ";\n";
                        continue;
                    }
                    const bool flat = m.attr == "flat" || is_integer_type(m.type);
                    io << (flat ? "flat out " : "out ") << map_type(m.type) << " v_" << m.name << m.array << ";\n";
                    main << "    v_" << m.name << " = me_out." << rename_ident(m.name) << ";\n";
                }
            } else if (map_type(f.ret) == "vec4") {
                main << "    gl_Position = " << call << ";\n";
            } else {
                throw TranslateError(f.name + " must return a struct or float4");
            }
            // Metal clip space (y up, z in [0, 1], rows top-down) to OpenGL's: see msl_to_glsl.hpp.
            main << "    gl_Position.y = -gl_Position.y;\n";
            main << "    gl_Position.z = 2.0 * gl_Position.z - gl_Position.w;\n";
        } else {
            const std::string ret = map_type(f.ret);
            if (ret == "vec4") {
                io << "layout(location = 0) out vec4 frag_out0;\n";
                main << "    frag_out0 = " << call << ";\n";
            } else if (ret == "void") {
                main << "    " << call << ";\n";
            } else {
                throw TranslateError(f.name + " must return float4 (or nothing)");
            }
        }
        main << "}\n";

        std::ostringstream o;
        o << globals.str() << io.str();
        for (const auto& [name, member] : block_aliases) o << "#define " << name << " " << member << "\n";
        o << map_type(f.ret) << " " << rename_ident(f.name) << "(";
        for (size_t i = 0; i < args.size(); ++i) o << (i ? ", " : "") << args[i];
        o << ") {" << body << "}\n";
        for (const auto& [name, member] : block_aliases) o << "#undef " << name << "\n";
        o << main.str();
        out.vertex_stage = vertex;
        out.entry = f.name;
        return o.str();
    }

    // The helpers an entry point (and the helpers it calls) can reach, by name: the others are
    // left out, so a vertex shader never sees fragment-only built-ins and the driver compiles less.
    [[nodiscard]] std::set<std::string> reachable_helpers(const Func& entry, const std::vector<Func>* extra) const {
        std::map<std::string, std::vector<const Func*>> by_name;
        for (const Func& f : unit.funcs) {
            if (f.stage == Func::Helper) by_name[f.name].push_back(&f);
        }
        if (extra) {
            for (const Func& f : *extra) {
                if (f.stage == Func::Helper) by_name[f.name].push_back(&f);
            }
        }
        std::set<std::string> needed;
        std::vector<const Toks*> work = {&entry.body};
        while (!work.empty()) {
            const Toks* body = work.back();
            work.pop_back();
            for (const Tok& k : *body) {
                if (!is_ident(k) || needed.count(k.text)) continue;
                const auto it = by_name.find(k.text);
                if (it == by_name.end()) continue;
                needed.insert(k.text);
                for (const Func* g : it->second) work.push_back(&g->body);
            }
        }
        return needed;
    }

    [[nodiscard]] std::string library_glsl(const std::set<std::string>& helpers, const FuncTable& table) const {
        std::ostringstream o;
        o << header() << kPreambleBody;
        for (const Item& item : unit.items) {
            switch (item.kind) {
                case Item::StructItem: o << struct_glsl(unit.structs[item.index]); break;
                case Item::GlobalItem: o << global_glsl(unit.globals[item.index], table); break;
                case Item::FuncItem: {
                    const Func& f = unit.funcs[item.index];
                    if (f.stage == Func::Helper && helpers.count(f.name)) o << helper_glsl(f, table);
                    break;
                }
            }
        }
        return o.str();
    }
};

MslToGlsl::MslToGlsl() : impl_(std::make_unique<Impl>()) {}
MslToGlsl::~MslToGlsl() = default;

void MslToGlsl::set_dialect(GlslDialect dialect, int version) {
    impl_->dialect = dialect;
    if (dialect == GlslDialect::ES) {
        impl_->version = (version == 310 || version == 320) ? version : 300;
    } else {
        impl_->version = version > 0 ? version : 410;
    }
}

GlslDialect MslToGlsl::dialect() const { return impl_->dialect; }
int MslToGlsl::version() const { return impl_->version; }
std::string MslToGlsl::preamble_header() const { return impl_->header(); }

bool MslToGlsl::add_source(const std::string& msl, std::string* error) {
    try {
        Unit added = parse_unit(tokenize(msl));
        Unit& u = impl_->unit;
        for (const Item& item : added.items) {
            switch (item.kind) {
                case Item::StructItem: {
                    Struct& s = added.structs[item.index];
                    if (impl_->is_struct(s.name)) break;  // the sources repeat shared structs
                    impl_->struct_index[s.name] = u.structs.size();
                    u.structs.push_back(std::move(s));
                    u.items.push_back({Item::StructItem, u.structs.size() - 1});
                    break;
                }
                case Item::GlobalItem:
                    u.globals.push_back(std::move(added.globals[item.index]));
                    u.items.push_back({Item::GlobalItem, u.globals.size() - 1});
                    break;
                case Item::FuncItem: {
                    Func& f = added.funcs[item.index];
                    impl_->register_samplers(f);
                    if (const Param* pulled = impl_->pulled_vertices(f, nullptr)) {
                        u.structs[impl_->struct_index.at(pulled->type)].vertex_input = true;
                    }
                    u.funcs.push_back(std::move(f));
                    u.items.push_back({Item::FuncItem, u.funcs.size() - 1});
                    break;
                }
            }
        }
        // Helpers are analyzed in definition order (callees come first in MSL, as in C).
        Impl::FuncTable table{impl_.get(), nullptr};
        for (Func& f : u.funcs) {
            if (f.stage == Func::Helper) impl_->analyze_textures(f, table);
        }
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

GlslShader MslToGlsl::emit(const std::string& entry, int pointer_array_size) const {
    GlslShader out;
    try {
        for (const Func& f : impl_->unit.funcs) {
            if (f.name != entry) continue;
            const Impl::FuncTable table{impl_.get(), nullptr};
            const std::string body = impl_->entry_glsl(f, pointer_array_size, table, out);
            out.source = impl_->library_glsl(impl_->reachable_helpers(f, nullptr), table) + body;
            return out;
        }
        out.error = "no entry point named " + entry;
    } catch (const std::exception& e) {
        out = GlslShader{};
        out.error = entry + ": " + e.what();
    }
    return out;
}

GlslShader MslToGlsl::emit_from(const std::string& entry_msl, const std::string& entry, int pointer_array_size) const {
    GlslShader out;
    try {
        Unit extra = parse_unit(tokenize(entry_msl));
        if (!extra.structs.empty() || !extra.globals.empty()) {
            throw TranslateError("only functions may be added with emit_from()");
        }
        const Impl::FuncTable table{impl_.get(), &extra.funcs};
        const Func* main_func = nullptr;
        for (Func& f : extra.funcs) {
            if (f.stage == Func::Helper) {
                impl_->analyze_textures(f, table);
            } else if (f.name == entry) {
                main_func = &f;
            }
        }
        if (!main_func) throw TranslateError("no entry point named " + entry);
        // Static samplers of the extra helpers are not registered: those live in add_source() text.
        const std::string body = impl_->entry_glsl(*main_func, pointer_array_size, table, out);
        std::string text = impl_->library_glsl(impl_->reachable_helpers(*main_func, &extra.funcs), table);
        for (const Func& f : extra.funcs) {
            if (f.stage == Func::Helper) text += impl_->helper_glsl(f, table);
        }
        out.source = text + body;
    } catch (const std::exception& e) {
        out = GlslShader{};
        out.error = entry + ": " + e.what();
    }
    return out;
}

const std::vector<GlslStaticSampler>& MslToGlsl::static_samplers() const { return impl_->samplers; }

}  // namespace me
