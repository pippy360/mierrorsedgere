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
// -----------------------------------------------------------------------------

#include "glcorearb.h"

#include <string>
#include <vector>

namespace me {
namespace gl {

// Every function the renderer calls. X(typedef, name).
#define ME_GL_FUNCTIONS(X)                                                  \
    /* state */                                                              \
    X(PFNGLGETSTRINGPROC, glGetString)                                       \
    X(PFNGLGETSTRINGIPROC, glGetStringi)                                     \
    X(PFNGLGETINTEGERVPROC, glGetIntegerv)                                   \
    X(PFNGLGETERRORPROC, glGetError)                                         \
    X(PFNGLENABLEPROC, glEnable)                                             \
    X(PFNGLDISABLEPROC, glDisable)                                           \
    X(PFNGLVIEWPORTPROC, glViewport)                                         \
    X(PFNGLDEPTHRANGEPROC, glDepthRange)                                     \
    X(PFNGLDEPTHFUNCPROC, glDepthFunc)                                       \
    X(PFNGLDEPTHMASKPROC, glDepthMask)                                       \
    X(PFNGLCLEARCOLORPROC, glClearColor)                                     \
    X(PFNGLCLEARDEPTHPROC, glClearDepth)                                     \
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
    X(PFNGLDRAWBUFFERPROC, glDrawBuffer)                                     \
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
    X(PFNGLDRAWELEMENTSPROC, glDrawElements)

// Entry points of extensions the renderer can do without: null when the driver lacks them.
//   glDebugMessageCallback / glDebugMessageControl : GL_KHR_debug (ME_GL_DEBUG=1; Apple has neither)
//   glPolygonOffsetClamp : GL_ARB_polygon_offset_clamp / GL_EXT_polygon_offset_clamp (core in 4.6);
//                          without it the shadow passes' slope-scaled bias is not capped.
#define ME_GL_OPTIONAL_FUNCTIONS(X)                                         \
    X(PFNGLDEBUGMESSAGECALLBACKPROC, glDebugMessageCallback)                 \
    X(PFNGLDEBUGMESSAGECONTROLPROC, glDebugMessageControl)                   \
    X(PFNGLPOLYGONOFFSETCLAMPPROC, glPolygonOffsetClamp)

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

}  // namespace gl
}  // namespace me
