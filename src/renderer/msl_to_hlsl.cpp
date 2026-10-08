#include "msl_to_hlsl.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace me {

namespace {

struct Tok {
    enum Kind { Ident, Number, Punct } kind = Punct;
    std::string text;
    std::string ws;  // whitespace before the token (comments removed, line breaks kept)
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

bool is_matrix_type(const std::string& t) { return t == "float2x2" || t == "float3x3" || t == "float4x4"; }

std::string map_type(const std::string& msl_type) {
    switch (tex_kind(msl_type)) {
        case TexKind::Color2D: return "Texture2D<float4>";
        case TexKind::ColorArray: return "Texture2DArray<float4>";
        case TexKind::Cube: return "TextureCube<float4>";
        case TexKind::Depth2D: return "Texture2D<float>";
        case TexKind::DepthArray: return "Texture2DArray<float>";
        case TexKind::None: break;
    }
    if (msl_type == "sampler") return "SamplerState";
    if (msl_type == "packed_float2") return "float2";
    if (msl_type == "packed_float3") return "float3";
    if (msl_type == "packed_float4") return "float4";
    return msl_type;
}

// MSL functions with another name in HLSL, and MSL identifiers that are HLSL keywords.
const std::map<std::string, std::string>& identifier_renames() {
    static const std::map<std::string, std::string> table = {
        {"mix", "lerp"},
        {"fract", "frac"},
        {"dfdx", "ddx"},
        {"dfdy", "ddy"},
        {"packed_float2", "float2"},
        {"packed_float3", "float3"},
        {"packed_float4", "float4"},
        {"constexpr", "const"},
        {"in", "in_"},
        {"out", "out_"},
        {"inout", "inout_"},
        {"line", "line_"},
        {"point", "point_"},
        {"triangle", "triangle_"},
        {"linear", "linear_"},
        {"sample", "sample_"},
        {"centroid", "centroid_"},
        {"precise", "precise_"},
        {"half", "half_"},
        {"fixed", "fixed_"},
        {"vector", "vector_"},
        {"matrix", "matrix_"},
        {"texture", "texture_"},
        {"pass", "pass_"},
        {"technique", "technique_"},
        {"string", "string_"},
        {"shared", "shared_"},
        {"uniform", "uniform_"},
        {"register", "register_"},
        {"discard", "discard_"},
    };
    return table;
}

std::string rename_ident(const std::string& name) {
    const auto& table = identifier_renames();
    const auto it = table.find(name);
    return it == table.end() ? name : it->second;
}

struct Member {
    std::string type;
    std::string name;
    std::string array;  // "[4]" or empty
    std::string attr;   // position / attribute / ...
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

struct Func {
    enum Stage { Helper, Vertex, Fragment } stage = Helper;
    std::string ret;
    std::string name;
    std::vector<Param> params;
    Toks body;  // tokens between the braces
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

const char* const kPreamble = R"hlsl(// Generated from Metal Shading Language by me::MslToHlsl (src/renderer/msl_to_hlsl.cpp).
#pragma warning(disable : 3571)  // pow(f, e): f may be negative (as in the MSL)
#pragma warning(disable : 3078)  // loop control variable conflicts with a previous declaration
#pragma warning(disable : 4000)  // use of potentially uninitialized variable
float me_tex_width(Texture2D<float4> t) { uint w, h; t.GetDimensions(w, h); return (float)w; }
float me_tex_height(Texture2D<float4> t) { uint w, h; t.GetDimensions(w, h); return (float)h; }
float me_tex_width(Texture2D<float> t) { uint w, h; t.GetDimensions(w, h); return (float)w; }
float me_tex_height(Texture2D<float> t) { uint w, h; t.GetDimensions(w, h); return (float)h; }
float me_tex_width(Texture2DArray<float> t) { uint w, h, n; t.GetDimensions(w, h, n); return (float)w; }
float me_tex_height(Texture2DArray<float> t) { uint w, h, n; t.GetDimensions(w, h, n); return (float)h; }
float me_tex_width(Texture2DArray<float4> t) { uint w, h, n; t.GetDimensions(w, h, n); return (float)w; }
float me_tex_height(Texture2DArray<float4> t) { uint w, h, n; t.GetDimensions(w, h, n); return (float)h; }
)hlsl";

}  // namespace

struct MslToHlsl::Impl {
    Unit unit;
    std::map<std::string, size_t> struct_index;
    std::set<std::string> matrix_members;  // struct members of matrix type, by name
    std::vector<HlslStaticSampler> samplers;
    std::map<std::string, std::map<std::string, std::string>> sampler_alias;  // function -> MSL name -> HLSL name
    std::string library;  // HLSL of everything but the entry points

    // Names visible in a function body whose type decides how the body is rewritten.
    struct Scope {
        std::string function;
        std::map<std::string, TexKind> textures;
        std::set<std::string> matrices;
        std::string pull_pointer;  // `pull_pointer[pull_index]` is the stage's input vertex
        std::string pull_index;
    };

    [[nodiscard]] bool is_struct(const std::string& name) const { return struct_index.count(name) != 0; }

    // ---- constexpr samplers -----------------------------------------------------
    void register_samplers(const Func& f) {
        const Toks& t = f.body;
        for (size_t i = 0; i + 3 < t.size(); ++i) {
            if (!is(t[i], "constexpr") || !is(t[i + 1], "sampler") || !is(t[i + 3], "(")) continue;
            HlslStaticSampler s;
            s.name = "me_ss_" + f.name + "_" + t[i + 2].text;
            s.slot = 14 - static_cast<int>(samplers.size());
            const size_t close = match_close(t, i + 3);
            for (size_t k = i + 4; k + 2 < close + 1 && k < close; ++k) {
                if (!is(t[k], "::") || k == 0 || k + 1 >= close) continue;
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

    // ---- function bodies --------------------------------------------------------
    [[nodiscard]] std::string translate_body(const Toks& body, Scope scope) const {
        Toks t = body;

        // `constexpr sampler name(...);` -> a global sampler (declared in the library).
        for (size_t i = 0; i + 3 < t.size(); ++i) {
            if (!is(t[i], "constexpr") || !is(t[i + 1], "sampler") || !is(t[i + 3], "(")) continue;
            const std::string local = t[i + 2].text;
            const auto fn = sampler_alias.find(scope.function);
            if (fn == sampler_alias.end() || fn->second.count(local) == 0) {
                throw TranslateError("constexpr sampler '" + local + "' in " + scope.function + " was not registered");
            }
            size_t end = match_close(t, i + 3) + 1;
            if (end < t.size() && is(t[end], ";")) ++end;
            const std::string ws = t[i].ws;
            t.erase(t.begin() + static_cast<std::ptrdiff_t>(i), t.begin() + static_cast<std::ptrdiff_t>(end));
            if (i < t.size()) t[i].ws = ws + t[i].ws;
            for (size_t k = 0; k < t.size(); ++k) {
                if (is_ident(t[k]) && t[k].text == local && (k == 0 || !is(t[k - 1], "."))) t[k].text = fn->second.at(local);
            }
            --i;
        }

        // `vertices[vertex_id]` -> the input-assembler vertex.
        if (!scope.pull_pointer.empty()) {
            for (size_t i = 0; i < t.size(); ++i) {
                if (!is_ident(t[i]) || t[i].text != scope.pull_pointer) continue;
                if (i + 3 >= t.size() || !is(t[i + 1], "[") || t[i + 2].text != scope.pull_index || !is(t[i + 3], "]")) {
                    throw TranslateError("'" + scope.pull_pointer + "' may only be read as " + scope.pull_pointer + "[" +
                                         scope.pull_index + "]");
                }
                t[i].text = "me_vin";
                t.erase(t.begin() + static_cast<std::ptrdiff_t>(i + 1), t.begin() + static_cast<std::ptrdiff_t>(i + 4));
            }
        }

        rewrite_texture_calls(t, scope);

        for (size_t i = 0; i + 1 < t.size(); ++i) {
            if (is_ident(t[i]) && is_matrix_type(t[i].text) && is_ident(t[i + 1])) scope.matrices.insert(t[i + 1].text);
        }

        rewrite_scalar_constructors(t);
        rewrite_matrix_products(t, scope);

        // `Struct name;` -> zero-initialised, as HLSL wants every member of a returned struct written.
        for (size_t i = 0; i + 2 < t.size(); ++i) {
            if (!is_ident(t[i]) || !is_struct(t[i].text) || !is_ident(t[i + 1]) || !is(t[i + 2], ";")) continue;
            if (i > 0 && !is(t[i - 1], "{") && !is(t[i - 1], "}") && !is(t[i - 1], ";")) continue;
            const std::string type = t[i].text;
            const Toks init = {punct("=", " "), punct("(", " "), ident(type), punct(")"), make(Tok::Number, "0")};
            t.insert(t.begin() + static_cast<std::ptrdiff_t>(i + 2), init.begin(), init.end());
        }

        for (size_t i = 0; i < t.size(); ++i) {
            if (!is_ident(t[i])) continue;
            if (t[i].text == "discard_fragment" && i + 2 < t.size() && is(t[i + 1], "(") && is(t[i + 2], ")")) {
                t[i].text = "discard";
                t.erase(t.begin() + static_cast<std::ptrdiff_t>(i + 1), t.begin() + static_cast<std::ptrdiff_t>(i + 3));
                continue;
            }
            t[i].text = rename_ident(t[i].text);
        }
        return join(t);
    }

    // tex.sample(...) / sample_compare(...) / get_width() / get_height()
    static void rewrite_texture_calls(Toks& t, const Scope& scope) {
        for (size_t i = 1; i + 2 < t.size(); ++i) {
            if (!is(t[i], ".") || !is_ident(t[i + 1]) || !is(t[i + 2], "(")) continue;
            const std::string method = t[i + 1].text;
            if (method != "sample" && method != "sample_compare" && method != "get_width" && method != "get_height") {
                continue;
            }
            if (!is_ident(t[i - 1])) throw TranslateError("." + method + "() on something that is not a named texture");
            const std::string receiver = t[i - 1].text;
            const auto kind_it = scope.textures.find(receiver);
            const TexKind kind = kind_it == scope.textures.end() ? TexKind::Color2D : kind_it->second;
            const size_t close = match_close(t, i + 2);
            const std::vector<Toks> args = split_args(t, i + 3, close);

            Toks repl;
            auto add = [&repl](const Toks& a) { repl.insert(repl.end(), a.begin(), a.end()); };
            auto call = [&repl](const char* name) {
                repl.push_back(punct("."));
                repl.push_back(ident(name));
                repl.push_back(punct("("));
            };
            // Texture arrays take the slice as the coordinate's third component.
            auto coord_with_slice = [&](const Toks& uv, const Toks& slice) {
                repl.push_back(ident("float3", " "));
                repl.push_back(punct("("));
                add(uv);
                repl.push_back(punct(","));
                add(slice);
                repl.push_back(punct(")"));
            };

            size_t replace_from = i;
            if (method == "get_width" || method == "get_height") {
                replace_from = i - 1;
                repl.push_back(ident(method == "get_width" ? "me_tex_width" : "me_tex_height", t[i - 1].ws));
                repl.push_back(punct("("));
                repl.push_back(ident(receiver));
                repl.push_back(punct(")"));
            } else if (method == "sample_compare") {
                const bool array = kind == TexKind::DepthArray;
                if (args.size() != (array ? 4u : 3u)) throw TranslateError("unsupported sample_compare() form on " + receiver);
                call("SampleCmpLevelZero");
                add(args[0]);
                repl.push_back(punct(","));
                if (array) {
                    coord_with_slice(args[1], args[2]);
                } else {
                    add(args[1]);
                }
                repl.push_back(punct(","));
                add(args.back());
                repl.push_back(punct(")"));
            } else if (kind == TexKind::Depth2D || kind == TexKind::DepthArray) {
                // Depth maps have one level; an explicit level also keeps the lookup legal inside loops.
                const bool array = kind == TexKind::DepthArray;
                if (args.size() != (array ? 3u : 2u)) throw TranslateError("unsupported sample() form on " + receiver);
                call("SampleLevel");
                add(args[0]);
                repl.push_back(punct(","));
                if (array) {
                    coord_with_slice(args[1], args[2]);
                } else {
                    add(args[1]);
                }
                repl.push_back(punct(","));
                repl.push_back(make(Tok::Number, "0", " "));
                repl.push_back(punct(")"));
            } else {
                const bool array = kind == TexKind::ColorArray;
                const size_t base = array ? 3u : 2u;
                if (args.size() != base && args.size() != base + 1) {
                    throw TranslateError("unsupported sample() form on " + receiver);
                }
                const char* hlsl = "Sample";
                Toks option;
                if (args.size() == base + 1) {  // level(x) / bias(x)
                    const Toks& o = args.back();
                    if (o.size() < 4 || !is(o[1], "(") || (!is(o[0], "level") && !is(o[0], "bias"))) {
                        throw TranslateError("unsupported sample() option on " + receiver);
                    }
                    hlsl = is(o[0], "level") ? "SampleLevel" : "SampleBias";
                    option.assign(o.begin() + 2, o.end() - 1);
                }
                call(hlsl);
                add(args[0]);
                repl.push_back(punct(","));
                if (array) {
                    coord_with_slice(args[1], args[2]);
                } else {
                    add(args[1]);
                }
                if (!option.empty()) {
                    repl.push_back(punct(","));
                    add(option);
                }
                repl.push_back(punct(")"));
            }
            t.erase(t.begin() + static_cast<std::ptrdiff_t>(replace_from), t.begin() + static_cast<std::ptrdiff_t>(close + 1));
            t.insert(t.begin() + static_cast<std::ptrdiff_t>(replace_from), repl.begin(), repl.end());
            // Scanning continues inside the arguments, which may hold further texture calls.
        }
    }

    // floatN(x) -> ((floatN)(x)): HLSL constructors do not replicate a scalar.
    static void rewrite_scalar_constructors(Toks& t) {
        static const std::set<std::string> kVectors = {"float2", "float3", "float4", "int2",  "int3",
                                                       "int4",   "uint2",  "uint3",  "uint4"};
        for (size_t i = 0; i + 1 < t.size(); ++i) {
            if (!is_ident(t[i]) || kVectors.count(t[i].text) == 0 || !is(t[i + 1], "(")) continue;
            if (i > 0 && is(t[i - 1], ")") && i > 1 && is(t[i - 2], t[i].text.c_str())) continue;  // already a cast
            const size_t close = match_close(t, i + 1);
            if (close == i + 2 || split_args(t, i + 2, close).size() != 1) continue;
            t.insert(t.begin() + static_cast<std::ptrdiff_t>(close + 1), punct(")"));
            const std::string ws = t[i].ws;
            t[i].ws.clear();
            const Toks head = {punct("(", ws), punct("("), t[i], punct(")")};
            t.erase(t.begin() + static_cast<std::ptrdiff_t>(i));
            t.insert(t.begin() + static_cast<std::ptrdiff_t>(i), head.begin(), head.end());
            i += 3;  // at the cast's ')'; the argument list is scanned next
        }
    }

    // matrix * x -> mul(matrix, x)
    void rewrite_matrix_products(Toks& t, const Scope& scope) const {
        for (size_t i = 1; i + 1 < t.size(); ++i) {
            if (!is(t[i], "*") || !is_ident(t[i - 1])) continue;
            size_t start = i - 1;
            size_t parts = 1;
            while (start >= 2 && is(t[start - 1], ".") && is_ident(t[start - 2])) {
                start -= 2;
                ++parts;
            }
            const std::string& last = t[i - 1].text;
            const bool matrix = parts > 1 ? matrix_members.count(last) != 0 : scope.matrices.count(last) != 0;
            if (!matrix) continue;
            if (start > 0 && is(t[start - 1], ".")) throw TranslateError("unsupported matrix operand before '*'");

            size_t end = i + 1;  // one past the right operand: a unary-prefixed postfix expression
            while (end < t.size() && (is(t[end], "-") || is(t[end], "+"))) ++end;
            if (end >= t.size()) throw TranslateError("matrix product without a right operand");
            if (is(t[end], "(")) {
                end = match_close(t, end) + 1;
            } else if (t[end].kind == Tok::Ident || t[end].kind == Tok::Number) {
                ++end;
            } else {
                throw TranslateError("unsupported right operand of a matrix product");
            }
            while (end < t.size()) {
                if (is(t[end], "(") || is(t[end], "[")) {
                    end = match_close(t, end) + 1;
                } else if (is(t[end], ".") && end + 1 < t.size() && is_ident(t[end + 1])) {
                    end += 2;
                } else {
                    break;
                }
            }
            t.insert(t.begin() + static_cast<std::ptrdiff_t>(end), punct(")"));
            t[i] = punct(",");
            const std::string ws = t[start].ws;
            t[start].ws.clear();
            const Toks head = {ident("mul", ws), punct("(")};
            t.insert(t.begin() + static_cast<std::ptrdiff_t>(start), head.begin(), head.end());
            i += 2;  // at the ','; the right operand is scanned next
        }
    }

    // ---- declarations -----------------------------------------------------------
    [[nodiscard]] std::string struct_hlsl(const Struct& s) const {
        if (s.stage_io && s.vertex_input) throw TranslateError("struct " + s.name + " is both stage data and a vertex buffer");
        std::ostringstream o;
        o << "struct " << s.name << " {\n";
        int texcoord = 0;
        int index = 0;
        for (const Member& m : s.members) {
            o << "    " << map_type(m.type) << " " << rename_ident(m.name) << m.array;
            if (s.vertex_input) {
                o << " : VTX" << index;
            } else if (s.stage_io) {
                if (m.attr == "position") {
                    o << " : SV_Position";
                } else if (m.attr == "attribute") {
                    o << " : ATTR" << m.attr_index;
                } else {
                    o << " : TEXCOORD" << texcoord++;
                }
            }
            o << ";\n";
            ++index;
        }
        o << "};\n";
        return o.str();
    }

    [[nodiscard]] std::string global_hlsl(const Toks& decl) const {
        size_t i = 0;
        const std::string type = read_type(decl, i);
        Scope scope;
        const Toks rest(decl.begin() + static_cast<std::ptrdiff_t>(i), decl.end());
        return "static const " + map_type(type) + translate_body(rest, scope) + "\n";
    }

    [[nodiscard]] static Scope scope_of(const Func& f) {
        Scope scope;
        scope.function = f.name;
        for (const Param& p : f.params) {
            const TexKind kind = tex_kind(p.type);
            if (kind != TexKind::None) scope.textures[p.name] = kind;
            if (is_matrix_type(p.type)) scope.matrices.insert(p.name);
        }
        return scope;
    }

    [[nodiscard]] std::string helper_hlsl(const Func& f) const {
        std::ostringstream o;
        o << map_type(f.ret) << " " << rename_ident(f.name) << "(";
        for (size_t i = 0; i < f.params.size(); ++i) {
            const Param& p = f.params[i];
            if (p.is_ptr) throw TranslateError("pointer argument '" + p.name + "' in helper function " + f.name);
            if (i > 0) o << ", ";
            if (p.is_ref && !p.is_const) o << "inout ";
            o << map_type(p.type) << " " << rename_ident(p.name);
        }
        o << ") {" << translate_body(f.body, scope_of(f)) << "}\n";
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

    [[nodiscard]] std::string entry_hlsl(const Func& f, int pointer_array_size, HlslShader& out) const {
        if (f.stage == Func::Helper) throw TranslateError(f.name + " is not a vertex or fragment function");
        Scope scope = scope_of(f);
        std::string pull_index;
        const Param* pulled = pulled_vertices(f, &pull_index);

        std::ostringstream globals;
        std::vector<std::string> args;
        for (const Param& p : f.params) {
            const std::string name = rename_ident(p.name);
            const std::string type = map_type(p.type);
            if (p.attr == "stage_in") {
                args.push_back(type + " " + name);
            } else if (p.attr == "vertex_id") {
                args.push_back("uint " + name + " : SV_VertexID");
            } else if (p.attr == "buffer" && &p == pulled) {
                const Struct& s = unit.structs[struct_index.at(p.type)];
                if (!s.vertex_input) throw TranslateError("vertex struct " + s.name + " was not declared with add_source()");
                scope.pull_pointer = p.name;
                scope.pull_index = pull_index;
                args.push_back(type + " me_vin");
                for (const Member& m : s.members) out.vertex_member_types.push_back(map_type(m.type));
            } else if (p.attr == "buffer") {
                globals << "cbuffer me_cb" << p.attr_index << " : register(b" << p.attr_index << ") { " << type << " " << name;
                if (p.is_ptr) globals << "[" << std::max(1, pointer_array_size) << "]";
                globals << "; };\n";
            } else if (p.attr == "texture") {
                globals << type << " " << name << " : register(t" << p.attr_index << ");\n";
            } else if (p.attr == "sampler") {
                for (const HlslStaticSampler& s : samplers) {
                    if (s.slot == p.attr_index) {
                        throw TranslateError("sampler slot " + std::to_string(s.slot) + " of " + f.name +
                                             " is taken by a constexpr sampler");
                    }
                }
                globals << "SamplerState " << name << " : register(s" << p.attr_index << ");\n";
            } else {
                throw TranslateError("unsupported argument '" + p.name + "' of " + f.name);
            }
        }

        std::ostringstream o;
        o << globals.str() << map_type(f.ret) << " " << rename_ident(f.name) << "(";
        for (size_t i = 0; i < args.size(); ++i) o << (i ? ", " : "") << args[i];
        o << ")";
        if (!is_struct(f.ret)) o << (f.stage == Func::Fragment ? " : SV_Target0" : " : SV_Position");
        o << " {" << translate_body(f.body, scope) << "}\n";
        out.vertex_stage = f.stage == Func::Vertex;
        out.entry = rename_ident(f.name);
        return o.str();
    }

    void rebuild_library() {
        std::ostringstream o;
        o << kPreamble;
        for (const HlslStaticSampler& s : samplers) {
            o << (s.compare ? "SamplerComparisonState " : "SamplerState ") << s.name << " : register(s" << s.slot << ");\n";
        }
        for (const Item& item : unit.items) {
            switch (item.kind) {
                case Item::StructItem: o << struct_hlsl(unit.structs[item.index]); break;
                case Item::GlobalItem: o << global_hlsl(unit.globals[item.index]); break;
                case Item::FuncItem:
                    if (unit.funcs[item.index].stage == Func::Helper) o << helper_hlsl(unit.funcs[item.index]);
                    break;
            }
        }
        library = o.str();
    }
};

MslToHlsl::MslToHlsl() : impl_(std::make_unique<Impl>()) {}
MslToHlsl::~MslToHlsl() = default;

bool MslToHlsl::add_source(const std::string& msl, std::string* error) {
    try {
        Unit added = parse_unit(tokenize(msl));
        Unit& u = impl_->unit;
        for (const Item& item : added.items) {
            switch (item.kind) {
                case Item::StructItem: {
                    Struct& s = added.structs[item.index];
                    if (impl_->is_struct(s.name)) break;  // the sources repeat shared structs
                    for (const Member& m : s.members) {
                        if (is_matrix_type(m.type)) impl_->matrix_members.insert(m.name);
                    }
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
        impl_->rebuild_library();
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

HlslShader MslToHlsl::emit(const std::string& entry, int pointer_array_size) const {
    HlslShader out;
    try {
        for (const Func& f : impl_->unit.funcs) {
            if (f.name != entry) continue;
            const std::string body = impl_->entry_hlsl(f, pointer_array_size, out);
            out.source = impl_->library + body;
            return out;
        }
        out.error = "no entry point named " + entry;
    } catch (const std::exception& e) {
        out = HlslShader{};
        out.error = entry + ": " + e.what();
    }
    return out;
}

HlslShader MslToHlsl::emit_from(const std::string& entry_msl, const std::string& entry, int pointer_array_size) const {
    HlslShader out;
    try {
        const Unit extra = parse_unit(tokenize(entry_msl));
        if (!extra.structs.empty() || !extra.globals.empty()) {
            throw TranslateError("only functions may be added with emit_from()");
        }
        std::string text = impl_->library;
        bool found = false;
        for (const Func& f : extra.funcs) {
            if (f.stage == Func::Helper) {
                text += impl_->helper_hlsl(f);
            } else if (f.name == entry) {
                text += impl_->entry_hlsl(f, pointer_array_size, out);
                found = true;
            }
        }
        if (!found) throw TranslateError("no entry point named " + entry);
        out.source = std::move(text);
    } catch (const std::exception& e) {
        out = HlslShader{};
        out.error = entry + ": " + e.what();
    }
    return out;
}

const std::vector<HlslStaticSampler>& MslToHlsl::static_samplers() const { return impl_->samplers; }

}  // namespace me
