// -----------------------------------------------------------------------------
// me_glsl: runs the Linux renderer's shader pipeline without the game. The built-in
// shaders (renderer/builtin_shaders_msl.hpp behind renderer/sun_shadow.hpp) and every
// chapter's generated material shaders are translated to GLSL with me::MslToGlsl and
// compiled and linked on the GPU through an OpenGL 4.1 core context, exactly as
// me::OpenGLRenderer does it. Everything that fails is printed with the driver's log.
//
//   me_glsl [--game-root <dir>] [--chapter N] [--dump <dir> | --dump-all <dir>] [--builtin-only]
//           [--es [300|310|320]]
//
// --dump writes the built-in shaders and every failed shader as GLSL; --dump-all every shader.
// --es emits the GLSL ES dialect (OpenGL ES on Android; `#version 300 es` unless a version is
// given) and, since a desktop GL cannot compile it, validates every shader offline with the
// Android NDK's glslc (path from ME_GLSLC, default the NDK 27 install under ~/Library/Android).
//
// Exit code 1 when any shader failed. docs/LINUX_PORT.md.
// -----------------------------------------------------------------------------

#include "assets/scene_materials.hpp"
#include "assets/upk_loader.hpp"
#include "platform/platform.hpp"
#include "renderer/builtin_shaders_msl.hpp"
#include "renderer/msl_to_glsl.hpp"
#include "renderer/sun_shadow.hpp"

#include <SDL2/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

// ---- the few GL entry points this tool needs, resolved through SDL ----------------
using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLchar = char;
using GLubyte = unsigned char;

constexpr GLenum GL_VERSION_ = 0x1F02;
constexpr GLenum GL_RENDERER_ = 0x1F01;
constexpr GLenum GL_FRAGMENT_SHADER_ = 0x8B30;
constexpr GLenum GL_VERTEX_SHADER_ = 0x8B31;
constexpr GLenum GL_COMPILE_STATUS_ = 0x8B81;
constexpr GLenum GL_LINK_STATUS_ = 0x8B82;
constexpr GLenum GL_INFO_LOG_LENGTH_ = 0x8B84;

struct GL {
    const GLubyte* (*GetString)(GLenum) = nullptr;
    GLuint (*CreateShader)(GLenum) = nullptr;
    void (*ShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*) = nullptr;
    void (*CompileShader)(GLuint) = nullptr;
    void (*GetShaderiv)(GLuint, GLenum, GLint*) = nullptr;
    void (*GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
    void (*DeleteShader)(GLuint) = nullptr;
    GLuint (*CreateProgram)() = nullptr;
    void (*AttachShader)(GLuint, GLuint) = nullptr;
    void (*LinkProgram)(GLuint) = nullptr;
    void (*GetProgramiv)(GLuint, GLenum, GLint*) = nullptr;
    void (*GetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
    void (*DeleteProgram)(GLuint) = nullptr;

    bool load() {
        bool ok = true;
        auto get = [&ok](auto& fn, const char* name) {
            void* p = SDL_GL_GetProcAddress(name);
            if (!p) {
                std::cerr << "me_glsl: cannot resolve " << name << std::endl;
                ok = false;
            }
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(p);
        };
        get(GetString, "glGetString");
        get(CreateShader, "glCreateShader");
        get(ShaderSource, "glShaderSource");
        get(CompileShader, "glCompileShader");
        get(GetShaderiv, "glGetShaderiv");
        get(GetShaderInfoLog, "glGetShaderInfoLog");
        get(DeleteShader, "glDeleteShader");
        get(CreateProgram, "glCreateProgram");
        get(AttachShader, "glAttachShader");
        get(LinkProgram, "glLinkProgram");
        get(GetProgramiv, "glGetProgramiv");
        get(GetProgramInfoLog, "glGetProgramInfoLog");
        get(DeleteProgram, "glDeleteProgram");
        return ok;
    }
} gl;

std::string shader_log(GLuint shader) {
    GLint len = 0;
    gl.GetShaderiv(shader, GL_INFO_LOG_LENGTH_, &len);
    std::string log(static_cast<size_t>(std::max(len, 1)), '\0');
    GLsizei written = 0;
    gl.GetShaderInfoLog(shader, len, &written, log.data());
    log.resize(static_cast<size_t>(std::max(written, 0)));
    return log;
}

std::string program_log(GLuint program) {
    GLint len = 0;
    gl.GetProgramiv(program, GL_INFO_LOG_LENGTH_, &len);
    std::string log(static_cast<size_t>(std::max(len, 1)), '\0');
    GLsizei written = 0;
    gl.GetProgramInfoLog(program, len, &written, log.data());
    log.resize(static_cast<size_t>(std::max(written, 0)));
    return log;
}

// Compiles `source`; 0 and the driver's log on failure.
GLuint compile(GLenum stage, const std::string& source, std::string& log) {
    const GLuint shader = gl.CreateShader(stage);
    const GLchar* text = source.c_str();
    const GLint length = static_cast<GLint>(source.size());
    gl.ShaderSource(shader, 1, &text, &length);
    gl.CompileShader(shader);
    GLint ok = 0;
    gl.GetShaderiv(shader, GL_COMPILE_STATUS_, &ok);
    if (!ok) {
        log = shader_log(shader);
        gl.DeleteShader(shader);
        return 0;
    }
    return shader;
}

bool link(GLuint vs, GLuint fs, std::string& log) {
    const GLuint program = gl.CreateProgram();
    gl.AttachShader(program, vs);
    if (fs) gl.AttachShader(program, fs);
    gl.LinkProgram(program);
    GLint ok = 0;
    gl.GetProgramiv(program, GL_LINK_STATUS_, &ok);
    if (!ok) log = program_log(program);
    gl.DeleteProgram(program);
    return ok != 0;
}

struct Options {
    std::string game_root;
    std::string dump_dir;   // failures (and the built-ins) are written here as GLSL
    bool dump_all = false;  // ... and every material shader too
    int chapter = -1;  // -1 = all
    bool builtin_only = false;
    bool es = false;         // --es: emit GLSL ES and validate with glslc instead of the GPU
    int es_version = 300;    // 300 | 310 | 320
};

void dump(const Options& opt, const std::string& name, const std::string& text) {
    if (opt.dump_dir.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(opt.dump_dir, ec);
    std::ofstream f(std::filesystem::path(opt.dump_dir) / name);
    f << text;
}

void print_failure(const std::string& what, const std::string& message) {
    std::cout << "  FAIL " << what << "\n";
    std::istringstream lines(message);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty()) std::cout << "       " << line << "\n";
    }
}

// ---- --es: offline validation with glslc ---------------------------------------
// Apple's OpenGL cannot compile GLSL ES, so the ES dialect is checked with the NDK's glslc
// (shaderc / glslang), the reference front end, as it would be for an OpenGL ES target:
//   glslc -fshader-stage=<stage> --target-env=opengl -fauto-bind-uniforms -fauto-map-locations -c
// --target-env=opengl keeps OpenGL semantics (uniform blocks, combined samplers, gl_VertexID);
// the two -fauto flags supply the SPIR-V-only bindings and locations the GLSL does not carry.
// glslc only writes SPIR-V for ES 3.10 and up, so `#version 300 es` shaders are validated as
// 310: GLSL ES 3.10 is a superset of 3.00 for everything the translator emits (no textureGather,
// bitfield, image or SSBO use), so what passes as 310 passes as 300 unless it uses a 310 feature.
// Shaders are checked in batches of one glslc run each; the errors are attributed by file name.
struct EsValidator {
    std::string glslc;
    std::filesystem::path dir;
    int version = 300;
    bool usable = false;
    struct Pending {
        std::string file;   // in `dir`
        std::string label;  // what to print
        bool vertex = false;
    };
    std::vector<Pending> pending;
    std::map<std::string, std::string> errors;  // file -> glslc's lines
    std::set<std::string> checked;

    bool init(int es_version) {
        version = es_version;
        if (const char* env = std::getenv("ME_GLSLC")) glslc = env;
        if (glslc.empty()) {
            glslc = "/Users/tomnom/Library/Android/sdk/ndk/27.0.12077973/shader-tools/darwin-x86_64/glslc";
        }
        std::error_code ec;
        if (!std::filesystem::exists(glslc, ec)) {
            std::cerr << "me_glsl: glslc not found at " << glslc << " (set ME_GLSLC)" << std::endl;
            return false;
        }
        dir = std::filesystem::temp_directory_path(ec) / ("me_glsl_es_" + std::to_string(static_cast<long long>(std::time(nullptr))));
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            std::cerr << "me_glsl: cannot create " << dir << std::endl;
            return false;
        }
        usable = true;
        return true;
    }

    // Queues `source` (already in the ES dialect) for the next flush().
    void add(const std::string& label, const std::string& file, bool vertex, const std::string& source) {
        std::string text = source;
        if (version < 310) {
            const size_t eol = text.find('\n');
            if (text.rfind("#version 300 es", 0) == 0 && eol != std::string::npos) text = "#version 310 es" + text.substr(eol);
        }
        std::ofstream f(dir / file);
        f << text;
        pending.push_back({file, label, vertex});
    }

    // Runs glslc over everything queued; errors land in `errors`.
    void flush() {
        for (int stage = 0; stage < 2; ++stage) {
            const bool vertex = stage == 0;
            std::vector<const Pending*> files;
            for (const Pending& p : pending) {
                if (p.vertex == vertex) files.push_back(&p);
            }
            for (size_t at = 0; at < files.size();) {
                std::string cmd = "cd \"" + dir.string() + "\" && \"" + glslc + "\" -fshader-stage=" + (vertex ? "vertex" : "fragment") +
                                  " --target-env=opengl -fauto-bind-uniforms -fauto-map-locations -c";
                const size_t end = std::min(files.size(), at + 64);
                for (size_t i = at; i < end; ++i) cmd += " " + files[i]->file;
                cmd += " 2>&1";
                at = end;
#ifdef _WIN32
                FILE* pipe = _popen(cmd.c_str(), "r");
#else
                FILE* pipe = popen(cmd.c_str(), "r");
#endif
                if (!pipe) {
                    for (size_t i = at; i < end; ++i) errors[files[i]->file] = "cannot run glslc";
                    continue;
                }
                std::string output;
                char buf[4096];
                while (std::fgets(buf, sizeof(buf), pipe)) output += buf;
#ifdef _WIN32
                _pclose(pipe);
#else
                pclose(pipe);
#endif
                std::istringstream lines(output);
                std::string line;
                while (std::getline(lines, line)) {
                    const size_t colon = line.find(':');
                    if (colon == std::string::npos || line.find("error") == std::string::npos) continue;
                    const std::string file = line.substr(0, colon);
                    if (errors.count(file) || std::find_if(files.begin(), files.end(), [&](const Pending* p) { return p->file == file; }) != files.end()) {
                        errors[file] += line + "\n";
                    }
                }
            }
        }
        for (const Pending& p : pending) checked.insert(p.file);
        pending.clear();
    }

    [[nodiscard]] bool failed(const std::string& file) const { return errors.count(file) != 0; }
    [[nodiscard]] std::string log(const std::string& file) const {
        const auto it = errors.find(file);
        return it == errors.end() ? std::string() : it->second;
    }

    ~EsValidator() {
        std::error_code ec;
        if (usable) std::filesystem::remove_all(dir, ec);
    }
} es_validator;

// Counts of what GLSL ES 3.0 guarantees least of: the fragment stage's sampler uniforms
// (GL_MAX_TEXTURE_IMAGE_UNITS >= 16) and the vertex stage's output vectors
// (GL_MAX_VARYING_VECTORS >= 15, counting each `out` as whole vec4 slots). Reported, not failed:
// a device may offer more, and the link on the device is what decides.
int varying_vectors(const std::string& vertex_source) {
    int vectors = 0;
    std::istringstream lines(vertex_source);
    std::string line;
    while (std::getline(lines, line)) {
        std::string decl = line;
        if (decl.rfind("flat out ", 0) == 0) decl = decl.substr(9);
        else if (decl.rfind("out ", 0) == 0) decl = decl.substr(4);
        else continue;
        int rows = 1;
        const size_t lb = decl.find('[');
        if (lb != std::string::npos) rows = std::max(1, std::atoi(decl.c_str() + lb + 1));
        if (decl.rfind("mat4", 0) == 0) rows *= 4;
        else if (decl.rfind("mat3", 0) == 0) rows *= 3;
        else if (decl.rfind("mat2", 0) == 0) rows *= 2;
        vectors += rows;
    }
    return vectors;
}

void report_es_limits(const std::string& label, const me::GlslShader& vs, const me::GlslShader* fs) {
    if (vs.error.empty()) {
        const int v = varying_vectors(vs.source);
        if (v > 15) std::cout << "  LIMIT " << label << " / " << vs.entry << ": " << v << " varying vectors (GLSL ES 3.0 guarantees 15)\n";
    }
    if (fs && fs->error.empty() && fs->samplers.size() > 16) {
        std::cout << "  LIMIT " << label << " / " << fs->entry << ": " << fs->samplers.size()
                  << " sampler uniforms (GLSL ES 3.0 guarantees 16 per fragment stage)\n";
    }
}

std::string file_name(const std::string& label, const std::string& entry, bool vertex) {
    std::string s = label + "." + entry;
    for (char& c : s) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '_' && c != '-') c = '_';
    }
    return s + (vertex ? ".vert" : ".frag");
}

// Translates and compiles one vertex / fragment pair. Returns false on any failure.
bool check_program(const Options& opt, const std::string& label, const me::GlslShader& vs, const me::GlslShader* fs) {
    bool ok = true;
    std::string log;
    GLuint vso = 0;
    GLuint fso = 0;
    if (opt.es) {
        // Offline: glslc checks each stage; there is no link step, the counts stand in for it.
        if (!vs.error.empty()) {
            print_failure(label + " / " + vs.entry + " (translate)", vs.error);
            ok = false;
        } else {
            dump(opt, label + "." + vs.entry + ".vert", vs.source);
            const std::string file = file_name(label, vs.entry, true);
            if (!es_validator.checked.count(file)) {
                es_validator.add(label, file, true, vs.source);
                es_validator.flush();
            }
            if (es_validator.failed(file)) {
                print_failure(label + " / " + vs.entry + " (vertex)", es_validator.log(file));
                ok = false;
            }
        }
        if (fs) {
            if (!fs->error.empty()) {
                print_failure(label + " / " + fs->entry + " (translate)", fs->error);
                ok = false;
            } else {
                dump(opt, label + "." + fs->entry + ".frag", fs->source);
                const std::string file = file_name(label, fs->entry, false);
                es_validator.add(label, file, false, fs->source);
                es_validator.flush();
                if (es_validator.failed(file)) {
                    print_failure(label + " / " + fs->entry + " (fragment)", es_validator.log(file));
                    ok = false;
                }
            }
        }
        report_es_limits(label, vs, fs);
        return ok;
    }
    if (!vs.error.empty()) {
        print_failure(label + " / " + vs.entry + " (translate)", vs.error);
        ok = false;
    } else {
        dump(opt, label + "." + vs.entry + ".vert", vs.source);
        vso = compile(GL_VERTEX_SHADER_, vs.source, log);
        if (!vso) {
            print_failure(label + " / " + vs.entry + " (vertex)", log);
            ok = false;
        }
    }
    if (fs) {
        if (!fs->error.empty()) {
            print_failure(label + " / " + fs->entry + " (translate)", fs->error);
            ok = false;
        } else {
            dump(opt, label + "." + fs->entry + ".frag", fs->source);
            fso = compile(GL_FRAGMENT_SHADER_, fs->source, log);
            if (!fso) {
                print_failure(label + " / " + fs->entry + " (fragment)", log);
                ok = false;
            }
        }
    }
    if (ok && !link(vso, fso, log)) {
        print_failure(label + " (link)", log);
        ok = false;
    }
    if (vso) gl.DeleteShader(vso);
    if (fso) gl.DeleteShader(fso);
    return ok;
}

void apply_dialect(const Options& opt, me::MslToGlsl& translator) {
    if (opt.es) translator.set_dialect(me::GlslDialect::ES, opt.es_version);
}

// The pairs the renderers build from the built-in shaders (d3d11_renderer.cpp, opengl_renderer.cpp).
bool check_builtins(const Options& opt) {
    me::MslToGlsl translator;
    apply_dialect(opt, translator);
    std::string err;
    if (!translator.add_source(me::sun_shadow_msl() + me::kBuiltinShadersMSL, &err)) {
        std::cout << "built-in shaders: cannot translate: " << err << std::endl;
        return false;
    }
    static const struct {
        const char* vs;
        const char* fs;
    } kPrograms[] = {
        {"shadow_vertex", nullptr},
        {"sky_vertex", "sky_fragment"},
        {"world_vertex", "world_fragment"},
        {"viewmodel_vertex", "viewmodel_fragment"},
        {"post_vertex", "fog_fragment"},
        {"post_vertex", "haze_fragment"},
        {"post_vertex", "bloom_gather_fragment"},
        {"post_vertex", "filter_fragment"},
        {"post_vertex", "meter_scene_fragment"},
        {"post_vertex", "meter_fragment"},
        {"post_vertex", "exposure_fragment"},
        {"post_vertex", "tonemap_fragment"},
        {"hud_vertex", "hud_fragment"},
        {"ui_tex_vertex", "ui_tex_fragment"},
    };
    int ok = 0;
    int total = 0;
    for (const auto& p : kPrograms) {
        ++total;
        const me::GlslShader vs = translator.emit(p.vs);
        me::GlslShader fs;
        if (p.fs) fs = translator.emit(p.fs);
        if (check_program(opt, std::string("builtin"), vs, p.fs ? &fs : nullptr)) ++ok;
    }
    std::cout << "built-in shaders: " << (opt.es ? "validated " : "compiled ") << ok << "/" << total << " programs, "
              << translator.static_samplers().size() << " static samplers" << std::endl;
    return ok == total;
}

// --es: every material fragment shader of the chapter through glslc, in batches.
bool check_chapter_es(const Options& opt, const std::string& tag, const me::SceneMaterialLibrary& lib,
                      const me::MslToGlsl& translator, const me::GlslShader& vs) {
    bool vs_ok = vs.error.empty();
    if (!vs_ok) {
        print_failure(tag + " / " + lib.vertex_function + " (translate)", vs.error);
    } else {
        dump(opt, tag + "." + vs.entry + ".vert", vs.source);
        es_validator.add(tag, file_name(tag, vs.entry, true), true, vs.source);
    }
    struct Item {
        const me::MaterialShader* sh = nullptr;
        me::GlslShader fs;
        std::string file;
    };
    std::vector<Item> items;
    int ok = 0;
    const int total = static_cast<int>(lib.shaders.size());
    for (const me::MaterialShader& sh : lib.shaders) {
        Item it;
        it.sh = &sh;
        it.fs = translator.emit_from(sh.source, sh.function_name, sh.num_uniforms);
        const std::string label = tag + " / " + sh.function_name + " (" + sh.base_material + ")";
        if (!it.fs.error.empty()) {
            print_failure(label + " (translate)", it.fs.error);
            continue;
        }
        if (opt.dump_all) dump(opt, tag + "." + sh.function_name + ".frag", it.fs.source);
        it.file = file_name(tag, sh.function_name, false);
        es_validator.add(label, it.file, false, it.fs.source);
        items.push_back(std::move(it));
    }
    es_validator.flush();
    if (vs_ok && es_validator.failed(file_name(tag, vs.entry, true))) {
        print_failure(tag + " / " + lib.vertex_function + " (vertex)", es_validator.log(file_name(tag, vs.entry, true)));
        vs_ok = false;
    }
    if (vs_ok) report_es_limits(tag, vs, nullptr);
    for (const Item& it : items) {
        const std::string label = tag + " / " + it.sh->function_name + " (" + it.sh->base_material + ")";
        if (es_validator.failed(it.file)) {
            print_failure(label + " (fragment)", es_validator.log(it.file));
            dump(opt, tag + "." + it.sh->function_name + ".frag", it.fs.source);
            continue;
        }
        report_es_limits(tag, vs, &it.fs);
        ++ok;
    }
    std::cout << tag << ": validated " << ok << "/" << total << " material shaders (" << lib.materials.size()
              << " materials), GLSL ES " << opt.es_version << std::endl;
    return ok == total && vs_ok;
}

// Builds the chapter's material library the way the game does when it loads the level, then
// translates and compiles every material fragment shader against the material vertex shader.
bool check_chapter(const Options& opt, const std::string& map) {
    std::cout << "== " << map << std::endl;
    me::LevelScene scene;
    if (!me::load_level_scene(opt.game_root, map, scene)) {
        std::cout << map << ": cannot load the level" << std::endl;
        return false;
    }
    const std::shared_ptr<const me::SceneMaterialLibrary> lib = scene.materials;
    if (!lib) {
        std::cout << map << ": no material library" << std::endl;
        return false;
    }
    const std::string tag = std::filesystem::path(map).stem().string();

    me::MslToGlsl translator;
    apply_dialect(opt, translator);
    std::string err;
    if (!translator.add_source(lib->common_source, &err)) {
        std::cout << map << ": cannot translate the material prelude: " << err << std::endl;
        return false;
    }
    const me::GlslShader vs = translator.emit(lib->vertex_function);
    if (opt.es) return check_chapter_es(opt, tag, *lib, translator, vs);
    std::string log;
    GLuint vso = 0;
    if (!vs.error.empty()) {
        print_failure(tag + " / " + lib->vertex_function + " (translate)", vs.error);
    } else {
        dump(opt, tag + "." + vs.entry + ".vert", vs.source);
        vso = compile(GL_VERTEX_SHADER_, vs.source, log);
        if (!vso) print_failure(tag + " / " + lib->vertex_function + " (vertex)", log);
    }

    int ok = 0;
    const int total = static_cast<int>(lib->shaders.size());
    for (const me::MaterialShader& sh : lib->shaders) {
        const me::GlslShader fs = translator.emit_from(sh.source, sh.function_name, sh.num_uniforms);
        const std::string label = tag + " / " + sh.function_name + " (" + sh.base_material + ")";
        if (!fs.error.empty()) {
            print_failure(label + " (translate)", fs.error);
            continue;
        }
        if (opt.dump_all) dump(opt, tag + "." + sh.function_name + ".frag", fs.source);
        GLuint fso = compile(GL_FRAGMENT_SHADER_, fs.source, log);
        if (!fso) {
            print_failure(label + " (fragment)", log);
            dump(opt, tag + "." + sh.function_name + ".frag", fs.source);
            continue;
        }
        if (vso && !link(vso, fso, log)) {
            print_failure(label + " (link)", log);
            dump(opt, tag + "." + sh.function_name + ".frag", fs.source);
        } else if (vso) {
            ++ok;
        }
        gl.DeleteShader(fso);
    }
    if (vso) gl.DeleteShader(vso);
    std::cout << tag << ": compiled " << ok << "/" << total << " material shaders (" << lib->materials.size()
              << " materials)" << std::endl;
    return ok == total && vso != 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "me_glsl: " << a << " needs a value" << std::endl;
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--game-root") {
            opt.game_root = value();
        } else if (a == "--chapter") {
            opt.chapter = std::atoi(value().c_str());
        } else if (a == "--dump") {
            opt.dump_dir = value();
        } else if (a == "--dump-all") {
            opt.dump_dir = value();
            opt.dump_all = true;
        } else if (a == "--builtin-only") {
            opt.builtin_only = true;
        } else if (a == "--es") {
            opt.es = true;
            if (i + 1 < argc && std::isdigit(static_cast<unsigned char>(argv[i + 1][0]))) {
                opt.es_version = std::atoi(argv[++i]);
                if (opt.es_version != 300 && opt.es_version != 310 && opt.es_version != 320) {
                    std::cerr << "me_glsl: --es takes 300, 310 or 320" << std::endl;
                    return 2;
                }
            }
        } else {
            std::cout << "usage: me_glsl [--game-root <dir>] [--chapter N] [--dump <dir> | --dump-all <dir>] [--builtin-only]"
                         " [--es [300|310|320]]"
                      << std::endl;
            return a == "--help" || a == "-h" ? 0 : 2;
        }
    }
    if (opt.game_root.empty()) opt.game_root = me::default_game_root();

    SDL_Window* window = nullptr;
    SDL_GLContext context = nullptr;
    if (opt.es) {
        // No GL context: Apple's (and Mesa's desktop) GL cannot compile GLSL ES; glslc checks it.
        if (!es_validator.init(opt.es_version)) return 1;
        std::cout << "GLSL ES " << opt.es_version << " validated with " << es_validator.glslc
                  << (opt.es_version < 310 ? " (as #version 310 es: glslc writes SPIR-V for ES 3.10 and up only)" : "")
                  << std::endl;
    } else {
        // Headless: the smallest hidden window that gives an OpenGL 4.1 core context (forward
        // compatible, which macOS requires for anything past 2.1).
        if (SDL_Init(SDL_INIT_VIDEO) != 0) {
            std::cerr << "me_glsl: SDL_Init failed: " << SDL_GetError() << std::endl;
            return 1;
        }
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
        window = SDL_CreateWindow("me_glsl", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 64, 64,
                                  SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
        if (!window) {
            std::cerr << "me_glsl: SDL_CreateWindow failed: " << SDL_GetError() << std::endl;
            return 1;
        }
        context = SDL_GL_CreateContext(window);
        if (!context) {
            std::cerr << "me_glsl: cannot create an OpenGL 4.1 core context: " << SDL_GetError() << std::endl;
            return 1;
        }
        if (!gl.load()) return 1;
        std::cout << "OpenGL " << gl.GetString(GL_VERSION_) << " on " << gl.GetString(GL_RENDERER_) << std::endl;
    }

    bool all_ok = check_builtins(opt);

    if (!opt.builtin_only) {
        static const char* const kChapterMaps[] = {
            "Maps/SP00/Tutorial_p.me1",   "Maps/SP01/Edge_p.me1",   "Maps/SP02/Stormdrain_p.me1",
            "Maps/SP03/Cranes_p.me1",     "Maps/SP04/Subway_p.me1", "Maps/SP05/Mall_p.me1",
            "Maps/SP06/Factory_p.me1",    "Maps/SP07/Boat_p.me1",   "Maps/SP08/Convoy_p.me1",
            "Maps/SP09/Scraper_p.me1",
        };
        std::vector<std::string> maps;
        for (int i = 0; i < 10; ++i) {
            if (opt.chapter < 0 || opt.chapter == i) maps.emplace_back(kChapterMaps[i]);
        }
        // Chapter 1's second level, when the install has it (not in every build of the game).
        const std::string escape = "Maps/SP01/Escape_p.me1";
        if ((opt.chapter < 0 || opt.chapter == 1) &&
            (std::filesystem::exists(std::filesystem::path(opt.game_root) / "TdGame" / "CookedPC" / escape) ||
             std::filesystem::exists(std::filesystem::path(opt.game_root) / "CookedPC" / escape))) {
            maps.push_back(escape);
        }
        for (const std::string& map : maps) {
            if (!check_chapter(opt, map)) all_ok = false;
        }
    }

    if (context) {
        SDL_GL_DeleteContext(context);
        SDL_DestroyWindow(window);
        SDL_Quit();
    }
    std::cout << (all_ok ? (opt.es ? "me_glsl: all shaders validated" : "me_glsl: all shaders compiled") : "me_glsl: FAILURES")
              << std::endl;
    return all_ok ? 0 : 1;
}
