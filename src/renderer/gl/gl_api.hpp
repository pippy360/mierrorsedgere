#pragma once

// -----------------------------------------------------------------------------
// The OpenGL entry points the renderer uses (renderer/opengl_renderer.cpp), as
// function pointers resolved through SDL_GL_GetProcAddress once a context exists.
// Nothing links libGL or OpenGL.framework: SDL loads the driver, and this file asks
// it for the functions by name. The pointers live in namespace me::gl, so they are
// C++ symbols and cannot collide with the C functions of a driver that happens to
// be in the process.
//
// glcorearb.h (Khronos registry) supplies the types, enums and PFNGL*PROC typedefs
// of the core profile; it declares no prototypes unless GL_GLEXT_PROTOTYPES is set,
// which it is not here. Apple's own gl3.h is never included.
//
// The same loader serves OpenGL ES 3.x (Android, ME_GLES): every enum the renderer
// shares with ES has the same value there, so glcorearb.h stays the source of the
// numbers, and the few ES-only names are defined below. Functions that exist only
// on desktop GL are optional (ME_GL_OPTIONAL_FUNCTIONS) and the renderer takes the
// ES spelling where they are missing; functions an ES driver exports under an
// OES / EXT / KHR suffix are found by the resolver.
// -----------------------------------------------------------------------------

#include "glcorearb.h"

#include <string>
#include <vector>

// OpenGL ES names the renderer uses that glcorearb.h does not carry. The values are the
// desktop ones (the registry gives an extension's enum the same number on both APIs).
#ifndef GL_TIME_ELAPSED_EXT
#define GL_TIME_ELAPSED_EXT 0x88BF  // GL_EXT_disjoint_timer_query == GL_TIME_ELAPSED
#endif
#ifndef GL_GPU_DISJOINT_EXT
#define GL_GPU_DISJOINT_EXT 0x8FBB  // GL_EXT_disjoint_timer_query
#endif
#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1  // GL_EXT_texture_format_BGRA8888 == GL_BGRA
#endif

namespace me {
namespace gl {

// Every function the renderer calls; all of them exist on OpenGL 4.1 core and OpenGL ES 3.0.
// X(typedef, name).
#define ME_GL_FUNCTIONS(X)                                                  \
    /* state */                                                              \
    X(PFNGLGETSTRINGPROC, glGetString)                                       \
    X(PFNGLGETSTRINGIPROC, glGetStringi)                                     \
    X(PFNGLGETINTEGERVPROC, glGetIntegerv)                                   \
    X(PFNGLGETERRORPROC, glGetError)                                         \
    X(PFNGLENABLEPROC, glEnable)                                             \
    X(PFNGLDISABLEPROC, glDisable)                                           \
    X(PFNGLVIEWPORTPROC, glViewport)                                         \
    X(PFNGLDEPTHRANGEFPROC, glDepthRangef)                                   \
    X(PFNGLDEPTHFUNCPROC, glDepthFunc)                                       \
    X(PFNGLDEPTHMASKPROC, glDepthMask)                                       \
    X(PFNGLCLEARCOLORPROC, glClearColor)                                     \
    X(PFNGLCLEARDEPTHFPROC, glClearDepthf)                                   \
    X(PFNGLCLEARPROC, glClear)                                               \
    X(PFNGLCLEARBUFFERFVPROC, glClearBufferfv)                               \
    X(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate)                       \
    X(PFNGLBLENDEQUATIONPROC, glBlendEquation)                               \
    X(PFNGLCULLFACEPROC, glCullFace)                                         \
    X(PFNGLFRONTFACEPROC, glFrontFace)                                       \
    X(PFNGLPOLYGONOFFSETPROC, glPolygonOffset)                               \
    X(PFNGLPIXELSTOREIPROC, glPixelStorei)                                   \
    X(PFNGLFLUSHPROC, glFlush)                                               \
    X(PFNGLFINISHPROC, glFinish)                                             \
    X(PFNGLREADPIXELSPROC, glReadPixels)                                     \
    X(PFNGLREADBUFFERPROC, glReadBuffer)                                     \
    X(PFNGLDRAWBUFFERSPROC, glDrawBuffers)                                   \
    X(PFNGLACTIVETEXTUREPROC, glActiveTexture)                               \
    /* textures */                                                           \
    X(PFNGLGENTEXTURESPROC, glGenTextures)                                   \
    X(PFNGLDELETETEXTURESPROC, glDeleteTextures)                             \
    X(PFNGLBINDTEXTUREPROC, glBindTexture)                                   \
    X(PFNGLTEXIMAGE2DPROC, glTexImage2D)                                     \
    X(PFNGLTEXIMAGE3DPROC, glTexImage3D)                                     \
    X(PFNGLTEXSUBIMAGE2DPROC, glTexSubImage2D)                               \
    X(PFNGLCOMPRESSEDTEXIMAGE2DPROC, glCompressedTexImage2D)                 \
    X(PFNGLTEXPARAMETERIPROC, glTexParameteri)                               \
    X(PFNGLTEXPARAMETERIVPROC, glTexParameteriv)                             \
    X(PFNGLGENERATEMIPMAPPROC, glGenerateMipmap)                             \
    /* sampler objects */                                                    \
    X(PFNGLGENSAMPLERSPROC, glGenSamplers)                                   \
    X(PFNGLDELETESAMPLERSPROC, glDeleteSamplers)                             \
    X(PFNGLBINDSAMPLERPROC, glBindSampler)                                   \
    X(PFNGLSAMPLERPARAMETERIPROC, glSamplerParameteri)                       \
    X(PFNGLSAMPLERPARAMETERFPROC, glSamplerParameterf)                       \
    /* buffers */                                                            \
    X(PFNGLGENBUFFERSPROC, glGenBuffers)                                     \
    X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers)                               \
    X(PFNGLBINDBUFFERPROC, glBindBuffer)                                     \
    X(PFNGLBUFFERDATAPROC, glBufferData)                                     \
    X(PFNGLBUFFERSUBDATAPROC, glBufferSubData)                               \
    X(PFNGLMAPBUFFERRANGEPROC, glMapBufferRange)                             \
    X(PFNGLUNMAPBUFFERPROC, glUnmapBuffer)                                   \
    X(PFNGLBINDBUFFERBASEPROC, glBindBufferBase)                             \
    X(PFNGLBINDBUFFERRANGEPROC, glBindBufferRange)                           \
    /* vertex arrays */                                                      \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays)                           \
    X(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays)                     \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray)                           \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray)           \
    X(PFNGLDISABLEVERTEXATTRIBARRAYPROC, glDisableVertexAttribArray)         \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer)                   \
    X(PFNGLVERTEXATTRIBIPOINTERPROC, glVertexAttribIPointer)                 \
    /* shaders and programs */                                               \
    X(PFNGLCREATESHADERPROC, glCreateShader)                                 \
    X(PFNGLDELETESHADERPROC, glDeleteShader)                                 \
    X(PFNGLSHADERSOURCEPROC, glShaderSource)                                 \
    X(PFNGLCOMPILESHADERPROC, glCompileShader)                               \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv)                                   \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog)                         \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram)                               \
    X(PFNGLDELETEPROGRAMPROC, glDeleteProgram)                               \
    X(PFNGLATTACHSHADERPROC, glAttachShader)                                 \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram)                                   \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv)                                 \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog)                       \
    X(PFNGLUSEPROGRAMPROC, glUseProgram)                                     \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation)                     \
    X(PFNGLUNIFORM1IPROC, glUniform1i)                                       \
    X(PFNGLGETUNIFORMBLOCKINDEXPROC, glGetUniformBlockIndex)                 \
    X(PFNGLUNIFORMBLOCKBINDINGPROC, glUniformBlockBinding)                   \
    /* framebuffers */                                                       \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers)                           \
    X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers)                     \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer)                           \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D)                 \
    X(PFNGLFRAMEBUFFERTEXTURELAYERPROC, glFramebufferTextureLayer)           \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus)             \
    X(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer)                           \
    /* drawing */                                                            \
    X(PFNGLDRAWARRAYSPROC, glDrawArrays)                                     \
    X(PFNGLDRAWELEMENTSPROC, glDrawElements)                                 \
    /* queries (ME_RENDER_PROF: GPU time per frame; ES 3.0 has the objects, not the timer) */ \
    X(PFNGLGENQUERIESPROC, glGenQueries)                                     \
    X(PFNGLDELETEQUERIESPROC, glDeleteQueries)                               \
    X(PFNGLBEGINQUERYPROC, glBeginQuery)                                     \
    X(PFNGLENDQUERYPROC, glEndQuery)                                         \
    X(PFNGLGETQUERYOBJECTUIVPROC, glGetQueryObjectuiv)

// Entry points the renderer can do without: null when the driver lacks them.
//   glDebugMessageCallback / glDebugMessageControl : GL_KHR_debug (ME_GL_DEBUG=1; Apple has neither;
//                          on ES the KHR-suffixed names)
//   glPolygonOffsetClamp : GL_ARB_polygon_offset_clamp / GL_EXT_polygon_offset_clamp (core in 4.6);
//                          without it the shadow passes' slope-scaled bias is not capped.
//   glDepthRange, glClearDepth, glDrawBuffer : desktop only; ES has the f / s forms above, which
//                          the renderer uses where these are missing.
//   glGetQueryObjectui64v : desktop 3.3+; on ES GL_EXT_disjoint_timer_query's ...EXT. Without it
//                          (and GL_TIME_ELAPSED) ME_RENDER_PROF reports no GPU time.
#define ME_GL_OPTIONAL_FUNCTIONS(X)                                         \
    X(PFNGLDEBUGMESSAGECALLBACKPROC, glDebugMessageCallback)                 \
    X(PFNGLDEBUGMESSAGECONTROLPROC, glDebugMessageControl)                   \
    X(PFNGLPOLYGONOFFSETCLAMPPROC, glPolygonOffsetClamp)                     \
    X(PFNGLDEPTHRANGEPROC, glDepthRange)                                     \
    X(PFNGLCLEARDEPTHPROC, glClearDepth)                                     \
    X(PFNGLDRAWBUFFERPROC, glDrawBuffer)                                     \
    X(PFNGLGETQUERYOBJECTUI64VPROC, glGetQueryObjectui64v)

#define ME_GL_DECLARE(type, name) extern type name;
ME_GL_FUNCTIONS(ME_GL_DECLARE)
ME_GL_OPTIONAL_FUNCTIONS(ME_GL_DECLARE)
#undef ME_GL_DECLARE

// Resolves every entry point through SDL_GL_GetProcAddress. Call with the context current.
// Returns false if a required function is missing; `missing` (optional) gets their names.
bool load(std::vector<std::string>* missing);

// True once load() succeeded and until unload() is called (GL objects may be deleted).
bool loaded();
// Marks the entry points unusable (the context is going away); the object destructors
// of the renderer check this before deleting GL names.
void unload();

// The extension strings of the current context (GL_NUM_EXTENSIONS / glGetStringi).
bool has_extension(const char* name);

// True when the context load() saw is an OpenGL ES context (GL_VERSION starts with
// "OpenGL ES"), whatever the build was configured for.
bool is_es();
// The context's version (GL_MAJOR_VERSION / GL_MINOR_VERSION; 3.0 for ES 3.0, 4.1 for desktop 4.1).
int version_major();
int version_minor();

}  // namespace gl
}  // namespace me
