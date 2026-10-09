#include "gl_api.hpp"

#include <SDL2/SDL.h>

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
std::vector<std::string> g_extensions;

// Resolves `name`, trying the ARB / EXT / KHR spellings too: the same function is exported
// under its extension name on drivers that predate its promotion to core.
void* resolve(const char* name) {
    if (void* p = SDL_GL_GetProcAddress(name)) return p;
    static const char* const suffixes[] = {"ARB", "EXT", "KHR"};
    for (const char* suffix : suffixes) {
        const std::string alt = std::string(name) + suffix;
        if (void* p = SDL_GL_GetProcAddress(alt.c_str())) return p;
    }
    return nullptr;
}
}  // namespace

bool load(std::vector<std::string>* missing) {
    g_loaded = false;
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
    for (const std::string& e : g_extensions) {
        if (e == name) return true;
    }
    return false;
}

}  // namespace gl
}  // namespace me
