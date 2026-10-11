#include "gl_api.hpp"

#include <SDL2/SDL.h>
#if defined(__EMSCRIPTEN__)
#include <emscripten/html5.h>
#endif

#include <cstring>
#include <string>
#include <vector>

namespace me {
namespace gl {

#define ME_GL_DEFINE(type, name) type name = nullptr;
ME_GL_FUNCTIONS(ME_GL_DEFINE)
ME_GL_OPTIONAL_FUNCTIONS(ME_GL_DEFINE)
#undef ME_GL_DEFINE

namespace {
bool g_loaded = false;
bool g_es = false;
int g_major = 0;
int g_minor = 0;
std::vector<std::string> g_extensions;

// Resolves `name`, trying the ARB / EXT / KHR / OES spellings too: the same function is exported
// under its extension name on drivers that predate its promotion to core, and OpenGL ES drivers
// export extension entry points (glDebugMessageCallbackKHR, glGetQueryObjectui64vEXT) that way only.
void* resolve(const char* name) {
    if (void* p = SDL_GL_GetProcAddress(name)) return p;
    static const char* const suffixes[] = {"ARB", "EXT", "KHR", "OES"};
    for (const char* suffix : suffixes) {
        const std::string alt = std::string(name) + suffix;
        if (void* p = SDL_GL_GetProcAddress(alt.c_str())) return p;
    }
    return nullptr;
}
}  // namespace

bool load(std::vector<std::string>* missing) {
    g_loaded = false;
    g_es = false;
    g_major = g_minor = 0;
    g_extensions.clear();
    bool ok = true;
#define ME_GL_LOAD(type, name)                                      \
    name = reinterpret_cast<type>(resolve(#name));                  \
    if (!name) {                                                    \
        ok = false;                                                 \
        if (missing) missing->emplace_back(#name);                  \
    }
    ME_GL_FUNCTIONS(ME_GL_LOAD)
#undef ME_GL_LOAD
#define ME_GL_LOAD_OPTIONAL(type, name) name = reinterpret_cast<type>(resolve(#name));
    ME_GL_OPTIONAL_FUNCTIONS(ME_GL_LOAD_OPTIONAL)
#undef ME_GL_LOAD_OPTIONAL
    if (!ok) return false;

#if defined(__EMSCRIPTEN__)
    // WebGL 2 requires explicit extension activation for float renderability, linear float filtering, and S3TC.
    const EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx = emscripten_webgl_get_current_context();
    if (ctx > 0) {
        static const char* const kWebGlExts[] = {
            "EXT_color_buffer_float",
            "EXT_color_buffer_half_float",
            "OES_texture_float_linear",
            "WEBGL_compressed_texture_s3tc",
            "WEBGL_compressed_texture_s3tc_srgb",
            "EXT_texture_filter_anisotropic",
            "EXT_texture_norm16",
        };
        for (const char* ext_name : kWebGlExts) {
            emscripten_webgl_enable_extension(ctx, ext_name);
        }
    }
#endif

    // "OpenGL ES 3.0 ..." / "OpenGL ES-CM 1.1" on ES; a bare version string on desktop. Apple's
    // SDL may hand out a desktop context when ES was asked for, so the string decides, not the build.
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    g_es = version && std::strncmp(version, "OpenGL ES", 9) == 0;
    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    g_major = major;
    g_minor = minor;

    GLint count = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (GLint i = 0; i < count; ++i) {
        const GLubyte* ext = glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i));
        if (ext) g_extensions.emplace_back(reinterpret_cast<const char*>(ext));
    }
    g_loaded = true;
    return true;
}

bool loaded() { return g_loaded; }

void unload() {
    g_loaded = false;
    g_extensions.clear();
}

bool has_extension(const char* name) {
    if (!name) return false;
    const std::string want(name);
    const std::string bare = (want.rfind("GL_", 0) == 0) ? want.substr(3) : want;
    std::string webgl_alt;
    if (bare == "EXT_texture_compression_s3tc") webgl_alt = "WEBGL_compressed_texture_s3tc";
    else if (bare == "EXT_texture_compression_s3tc_srgb") webgl_alt = "WEBGL_compressed_texture_s3tc_srgb";
    for (const std::string& e : g_extensions) {
        if (e == want || e == bare || (!webgl_alt.empty() && (e == webgl_alt || e == "GL_" + webgl_alt))) return true;
    }
    return false;
}

bool is_es() { return g_es; }
int version_major() { return g_major; }
int version_minor() { return g_minor; }

}  // namespace gl
}  // namespace me
