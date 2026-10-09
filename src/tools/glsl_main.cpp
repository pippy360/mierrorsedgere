// -----------------------------------------------------------------------------
// me_glsl: runs the Linux renderer's shader pipeline without the game. The built-in
// shaders (renderer/builtin_shaders_msl.hpp behind renderer/sun_shadow.hpp) and every
// chapter's generated material shaders are translated to GLSL with me::MslToGlsl and
// compiled and linked on the GPU through an OpenGL 4.1 core context, exactly as
// me::OpenGLRenderer does it. Everything that fails is printed with the driver's log.
//
//   me_glsl [--game-root <dir>] [--chapter N] [--dump <dir> | --dump-all <dir>] [--builtin-only]
//
// --dump writes the built-in shaders and every failed shader as GLSL; --dump-all every shader.
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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
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

// Translates and compiles one vertex / fragment pair. Returns false on any failure.
bool check_program(const Options& opt, const std::string& label, const me::GlslShader& vs, const me::GlslShader* fs) {
    bool ok = true;
    std::string log;
    GLuint vso = 0;
    GLuint fso = 0;
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

// The pairs the renderers build from the built-in shaders (d3d11_renderer.cpp, opengl_renderer.cpp).
bool check_builtins(const Options& opt) {
    me::MslToGlsl translator;
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
    std::cout << "built-in shaders: compiled " << ok << "/" << total << " programs, "
              << translator.static_samplers().size() << " static samplers" << std::endl;
    return ok == total;
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
    std::string err;
    if (!translator.add_source(lib->common_source, &err)) {
        std::cout << map << ": cannot translate the material prelude: " << err << std::endl;
        return false;
    }
    const me::GlslShader vs = translator.emit(lib->vertex_function);
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
        } else {
            std::cout << "usage: me_glsl [--game-root <dir>] [--chapter N] [--dump <dir> | --dump-all <dir>] [--builtin-only]"
                      << std::endl;
            return a == "--help" || a == "-h" ? 0 : 2;
        }
    }
    if (opt.game_root.empty()) opt.game_root = me::default_game_root();

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
    SDL_Window* window = SDL_CreateWindow("me_glsl", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 64, 64,
                                          SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!window) {
        std::cerr << "me_glsl: SDL_CreateWindow failed: " << SDL_GetError() << std::endl;
        return 1;
    }
    SDL_GLContext context = SDL_GL_CreateContext(window);
    if (!context) {
        std::cerr << "me_glsl: cannot create an OpenGL 4.1 core context: " << SDL_GetError() << std::endl;
        return 1;
    }
    if (!gl.load()) return 1;
    std::cout << "OpenGL " << gl.GetString(GL_VERSION_) << " on " << gl.GetString(GL_RENDERER_) << std::endl;

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

    SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    std::cout << (all_ok ? "me_glsl: all shaders compiled" : "me_glsl: FAILURES") << std::endl;
    return all_ok ? 0 : 1;
}
