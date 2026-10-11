#pragma once

// -----------------------------------------------------------------------------
// Native Vulkan 1.0/1.1 Backend State & Command-Buffer Layer (me::vk_gl)
//
// Provides a standalone, zero-OpenGL-dependency Vulkan execution engine for
// VulkanRenderer (renderer/vulkan_renderer.cpp). Resolves all Vulkan entry points
// dynamically via SDL_Vulkan_GetVkGetInstanceProcAddr / dlopen so no link-time
// libvulkan dependency is required on macOS (MoltenVK), Linux, Windows, or Android.
// -----------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#if defined(_WIN32)
#define VK_GL_APIENTRY __stdcall
#else
#define VK_GL_APIENTRY
#endif
#ifndef APIENTRY
#define APIENTRY VK_GL_APIENTRY
#endif

namespace me {
namespace vk_gl {

using GLenum = uint32_t;
using GLboolean = uint8_t;
using GLbitfield = uint32_t;
using GLint = int32_t;
using GLuint = uint32_t;
using GLsizei = int32_t;
using GLfloat = float;
using GLdouble = double;
using GLchar = char;
using GLubyte = uint8_t;
using GLuint64 = uint64_t;
using GLsizeiptr = ptrdiff_t;
using GLintptr = ptrdiff_t;

constexpr GLboolean GL_FALSE = 0;
constexpr GLboolean GL_TRUE = 1;

// Errors & strings
constexpr GLenum GL_NO_ERROR = 0;
constexpr GLenum GL_VENDOR = 0x1F00;
constexpr GLenum GL_RENDERER = 0x1F01;
constexpr GLenum GL_VERSION = 0x1F02;
constexpr GLenum GL_SHADING_LANGUAGE_VERSION = 0x8B8C;

// Queries & limits
constexpr GLenum GL_MAX_TEXTURE_IMAGE_UNITS = 0x8872;
constexpr GLenum GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS = 0x8B4D;
constexpr GLenum GL_MAX_VARYING_VECTORS = 0x8DFC;
constexpr GLenum GL_MAX_TEXTURE_SIZE = 0x0D33;
constexpr GLenum GL_MAX_3D_TEXTURE_SIZE = 0x8073;
constexpr GLenum GL_MAX_ARRAY_TEXTURE_LAYERS = 0x88FF;
constexpr GLenum GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT = 0x8A34;
constexpr GLenum GL_MAX_UNIFORM_BUFFER_BINDINGS = 0x8A2F;
constexpr GLenum GL_MAX_VERTEX_UNIFORM_BLOCKS = 0x8A2B;
constexpr GLenum GL_MAX_FRAGMENT_UNIFORM_BLOCKS = 0x8A2D;
constexpr GLenum GL_MAX_UNIFORM_BLOCK_SIZE = 0x8A30;
constexpr GLenum GL_TEXTURE_BASE_LEVEL = 0x813C;
constexpr GLenum GL_TIME_ELAPSED = 0x88BF;
constexpr GLenum GL_TIME_ELAPSED_EXT = 0x88BF;
constexpr GLenum GL_GPU_DISJOINT_EXT = 0x8FBB;
constexpr GLenum GL_QUERY_RESULT = 0x8866;
constexpr GLenum GL_QUERY_RESULT_AVAILABLE = 0x8867;

// Enable/disable caps
constexpr GLenum GL_CULL_FACE = 0x0B44;
constexpr GLenum GL_DEPTH_TEST = 0x0B71;
constexpr GLenum GL_BLEND = 0x0BE2;
constexpr GLenum GL_POLYGON_OFFSET_FILL = 0x8037;
constexpr GLenum GL_FRAMEBUFFER_SRGB = 0x8DB9;
constexpr GLenum GL_SCISSOR_TEST = 0x0C11;
constexpr GLenum GL_DEBUG_OUTPUT = 0x92E0;
constexpr GLenum GL_DONT_CARE = 0x1100;

// Depth / blend / face enums
constexpr GLenum GL_NEVER = 0x0200;
constexpr GLenum GL_LESS = 0x0201;
constexpr GLenum GL_EQUAL = 0x0202;
constexpr GLenum GL_LEQUAL = 0x0203;
constexpr GLenum GL_GREATER = 0x0204;
constexpr GLenum GL_NOTEQUAL = 0x0205;
constexpr GLenum GL_GEQUAL = 0x0206;
constexpr GLenum GL_ALWAYS = 0x0207;

constexpr GLenum GL_ZERO = 0;
constexpr GLenum GL_ONE = 1;
constexpr GLenum GL_SRC_COLOR = 0x0300;
constexpr GLenum GL_ONE_MINUS_SRC_COLOR = 0x0301;
constexpr GLenum GL_SRC_ALPHA = 0x0302;
constexpr GLenum GL_ONE_MINUS_SRC_ALPHA = 0x0303;
constexpr GLenum GL_DST_ALPHA = 0x0304;
constexpr GLenum GL_ONE_MINUS_DST_ALPHA = 0x0305;
constexpr GLenum GL_DST_COLOR = 0x0306;
constexpr GLenum GL_ONE_MINUS_DST_COLOR = 0x0307;

constexpr GLenum GL_FUNC_ADD = 0x8006;
constexpr GLenum GL_MIN = 0x8007;
constexpr GLenum GL_MAX = 0x8008;
constexpr GLenum GL_FUNC_SUBTRACT = 0x800A;
constexpr GLenum GL_FUNC_REVERSE_SUBTRACT = 0x800B;

constexpr GLenum GL_FRONT = 0x0404;
constexpr GLenum GL_BACK = 0x0405;
constexpr GLenum GL_CW = 0x0900;
constexpr GLenum GL_CCW = 0x0901;

// Clear bits
constexpr GLbitfield GL_DEPTH_BUFFER_BIT = 0x00000100;
constexpr GLbitfield GL_STENCIL_BUFFER_BIT = 0x00000400;
constexpr GLbitfield GL_COLOR_BUFFER_BIT = 0x00004000;

// Primitives & indices
constexpr GLenum GL_TRIANGLES = 0x0004;
constexpr GLenum GL_TRIANGLE_STRIP = 0x0005;
constexpr GLenum GL_LINES = 0x0001;
constexpr GLenum GL_BYTE = 0x1400;
constexpr GLenum GL_UNSIGNED_BYTE = 0x1401;
constexpr GLenum GL_SHORT = 0x1402;
constexpr GLenum GL_UNSIGNED_SHORT = 0x1403;
constexpr GLenum GL_INT = 0x1404;
constexpr GLenum GL_UNSIGNED_INT = 0x1405;
constexpr GLenum GL_HALF_FLOAT = 0x140B;
constexpr GLenum GL_FLOAT = 0x1406;

// Buffers & mapping
constexpr GLenum GL_ARRAY_BUFFER = 0x8892;
constexpr GLenum GL_ELEMENT_ARRAY_BUFFER = 0x8893;
constexpr GLenum GL_UNIFORM_BUFFER = 0x8A11;
constexpr GLenum GL_STATIC_DRAW = 0x88E4;
constexpr GLenum GL_DYNAMIC_DRAW = 0x88E8;
constexpr GLenum GL_STREAM_DRAW = 0x88E0;
constexpr GLbitfield GL_MAP_READ_BIT = 0x0001;
constexpr GLbitfield GL_MAP_WRITE_BIT = 0x0002;
constexpr GLbitfield GL_MAP_INVALIDATE_RANGE_BIT = 0x0004;
constexpr GLbitfield GL_MAP_INVALIDATE_BUFFER_BIT = 0x0008;
constexpr GLbitfield GL_MAP_UNSYNCHRONIZED_BIT = 0x0020;

// Textures & samplers
constexpr GLenum GL_TEXTURE_2D = 0x0DE1;
constexpr GLenum GL_TEXTURE_2D_ARRAY = 0x8C1A;
constexpr GLenum GL_TEXTURE_CUBE_MAP = 0x8513;
constexpr GLenum GL_TEXTURE_CUBE_MAP_POSITIVE_X = 0x8515;
constexpr GLenum GL_TEXTURE0 = 0x84C0;
constexpr GLenum GL_TEXTURE_MAG_FILTER = 0x2800;
constexpr GLenum GL_TEXTURE_MIN_FILTER = 0x2801;
constexpr GLenum GL_TEXTURE_WRAP_S = 0x2802;
constexpr GLenum GL_TEXTURE_WRAP_T = 0x2803;
constexpr GLenum GL_TEXTURE_WRAP_R = 0x8072;
constexpr GLenum GL_TEXTURE_MIN_LOD = 0x813A;
constexpr GLenum GL_TEXTURE_MAX_LOD = 0x813B;
constexpr GLenum GL_TEXTURE_MAX_LEVEL = 0x813D;
constexpr GLenum GL_TEXTURE_COMPARE_MODE = 0x884C;
constexpr GLenum GL_TEXTURE_COMPARE_FUNC = 0x884D;
constexpr GLenum GL_COMPARE_REF_TO_TEXTURE = 0x884E;
constexpr GLenum GL_TEXTURE_MAX_ANISOTROPY = 0x84FE;
constexpr GLenum GL_TEXTURE_MAX_ANISOTROPY_EXT = 0x84FE;
constexpr GLenum GL_TEXTURE_SWIZZLE_R = 0x8E42;
constexpr GLenum GL_TEXTURE_SWIZZLE_G = 0x8E43;
constexpr GLenum GL_TEXTURE_SWIZZLE_B = 0x8E44;
constexpr GLenum GL_TEXTURE_SWIZZLE_A = 0x8E45;
constexpr GLenum GL_TEXTURE_SWIZZLE_RGBA = 0x8E46;
constexpr GLenum GL_NEAREST = 0x2600;
constexpr GLenum GL_LINEAR = 0x2601;
constexpr GLenum GL_NEAREST_MIPMAP_NEAREST = 0x2700;
constexpr GLenum GL_LINEAR_MIPMAP_NEAREST = 0x2701;
constexpr GLenum GL_NEAREST_MIPMAP_LINEAR = 0x2702;
constexpr GLenum GL_LINEAR_MIPMAP_LINEAR = 0x2703;
constexpr GLenum GL_REPEAT = 0x2901;
constexpr GLenum GL_CLAMP_TO_EDGE = 0x812F;
constexpr GLenum GL_MIRRORED_REPEAT = 0x8370;
constexpr GLenum GL_UNPACK_ALIGNMENT = 0x0CF5;
constexpr GLenum GL_PACK_ALIGNMENT = 0x0D05;

// Pixel & internal formats
constexpr GLenum GL_RED = 0x1903;
constexpr GLenum GL_GREEN = 0x1904;
constexpr GLenum GL_BLUE = 0x1905;
constexpr GLenum GL_ALPHA = 0x1906;
constexpr GLenum GL_RG = 0x8227;
constexpr GLenum GL_RGB = 0x1907;
constexpr GLenum GL_RGBA = 0x1908;
constexpr GLenum GL_BGRA = 0x80E1;
constexpr GLenum GL_BGRA_EXT = 0x80E1;
constexpr GLenum GL_DEPTH_COMPONENT = 0x1902;
constexpr GLenum GL_DEPTH_STENCIL = 0x84F9;

constexpr GLenum GL_R8 = 0x8229;
constexpr GLenum GL_RG8 = 0x822B;
constexpr GLenum GL_RG8_SNORM = 0x8F95;
constexpr GLenum GL_RGB8 = 0x8051;
constexpr GLenum GL_RGBA8 = 0x8058;
constexpr GLenum GL_SRGB8 = 0x8C41;
constexpr GLenum GL_SRGB8_ALPHA8 = 0x8C43;
constexpr GLenum GL_R16F = 0x822D;
constexpr GLenum GL_RG16F = 0x822F;
constexpr GLenum GL_RGBA16 = 0x805B;
constexpr GLenum GL_RGBA16F = 0x881A;
constexpr GLenum GL_R32F = 0x822E;
constexpr GLenum GL_RGBA32F = 0x8814;
constexpr GLenum GL_DEPTH_COMPONENT24 = 0x81A6;
constexpr GLenum GL_DEPTH_COMPONENT32F = 0x8CAC;

constexpr GLenum GL_COMPRESSED_RGBA_S3TC_DXT1_EXT = 0x83F1;
constexpr GLenum GL_COMPRESSED_RGBA_S3TC_DXT3_EXT = 0x83F2;
constexpr GLenum GL_COMPRESSED_RGBA_S3TC_DXT5_EXT = 0x83F3;
constexpr GLenum GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT = 0x8C4D;
constexpr GLenum GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT = 0x8C4E;
constexpr GLenum GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT = 0x8C4F;

// Shaders & programs
constexpr GLenum GL_FRAGMENT_SHADER = 0x8B30;
constexpr GLenum GL_VERTEX_SHADER = 0x8B31;
constexpr GLenum GL_COMPILE_STATUS = 0x8B81;
constexpr GLenum GL_LINK_STATUS = 0x8B82;
constexpr GLenum GL_INFO_LOG_LENGTH = 0x8B84;
constexpr GLuint GL_INVALID_INDEX = 0xFFFFFFFFu;

// Framebuffers
constexpr GLenum GL_FRAMEBUFFER = 0x8D40;
constexpr GLenum GL_READ_FRAMEBUFFER = 0x8CA8;
constexpr GLenum GL_DRAW_FRAMEBUFFER = 0x8CA9;
constexpr GLenum GL_COLOR_ATTACHMENT0 = 0x8CE0;
constexpr GLenum GL_DEPTH_ATTACHMENT = 0x8D00;
constexpr GLenum GL_FRAMEBUFFER_COMPLETE = 0x8CD5;
constexpr GLenum GL_NONE = 0;
constexpr GLenum GL_COLOR = 0x1800;
constexpr GLenum GL_DEPTH = 0x1801;

// Debug callback placeholders (unused on Vulkan validation layers)
constexpr GLenum GL_DEBUG_SEVERITY_NOTIFICATION = 0x826B;
constexpr GLenum GL_DEBUG_TYPE_ERROR = 0x824C;
constexpr GLenum GL_DEBUG_TYPE_PERFORMANCE = 0x8250;
using GLDEBUGPROC = void(APIENTRY*)(GLenum, GLenum, GLuint, GLenum, GLsizei, const GLchar*, const void*);

// Lifecycle management on Vulkan
void prepare_vulkan_library();
bool init_context(void* sdl_window, int width, int height, bool headless, const std::string& cache_dir);
void shutdown_context();
bool loaded();
void unload();
void resize_swapchain(int width, int height);
void begin_frame();
void end_frame(GLuint present_tex, int src_w, int src_h);
void prewarm_spirv_parallel(const std::vector<std::pair<std::string, bool>>& sources);
void* native_vk_device();
std::string device_name_string();

bool has_extension(const char* name);
bool is_es();
int version_major();
int version_minor();

// Vulkan-backed implementations of GPU state, resource, shader, FBO, and draw commands
const GLubyte* glGetString(GLenum name);
void glGetIntegerv(GLenum pname, GLint* data);
GLenum glGetError();
void glEnable(GLenum cap);
void glDisable(GLenum cap);
void glViewport(GLint x, GLint y, GLsizei width, GLsizei height);
void glDepthRange(GLdouble n, GLdouble f);
void glDepthRangef(GLfloat n, GLfloat f);
void glDepthFunc(GLenum func);
void glDepthMask(GLboolean flag);
void glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void glClearDepth(GLdouble d);
void glClearDepthf(GLfloat d);
void glClear(GLbitfield mask);
void glClearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat* value);
void glBlendFuncSeparate(GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha, GLenum dfactorAlpha);
void glBlendEquation(GLenum mode);
void glCullFace(GLenum mode);
void glFrontFace(GLenum mode);
void glPolygonOffset(GLfloat factor, GLfloat units);
void glPolygonOffsetClamp(GLfloat factor, GLfloat units, GLfloat clamp);
void glPixelStorei(GLenum pname, GLint param);
void glFlush();
void glFinish();
void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels);
void glReadBuffer(GLenum src);
void glDrawBuffer(GLenum buf);
void glDrawBuffers(GLsizei n, const GLenum* bufs);
void glActiveTexture(GLenum texture);

void glGenTextures(GLsizei n, GLuint* textures);
void glDeleteTextures(GLsizei n, const GLuint* textures);
void glBindTexture(GLenum target, GLuint texture);
void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border,
                  GLenum format, GLenum type, const void* pixels);
void glTexImage3D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLsizei depth,
                  GLint border, GLenum format, GLenum type, const void* pixels);
void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
                     GLenum format, GLenum type, const void* pixels);
void glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height,
                            GLint border, GLsizei imageSize, const void* data);
void glTexParameteri(GLenum target, GLenum pname, GLint param);
void glTexParameteriv(GLenum target, GLenum pname, const GLint* params);
void glGenerateMipmap(GLenum target);

void glGenSamplers(GLsizei count, GLuint* samplers);
void glDeleteSamplers(GLsizei count, const GLuint* samplers);
void glBindSampler(GLuint unit, GLuint sampler);
void glSamplerParameteri(GLuint sampler, GLenum pname, GLint param);
void glSamplerParameterf(GLuint sampler, GLenum pname, GLfloat param);

void glGenBuffers(GLsizei n, GLuint* buffers);
void glDeleteBuffers(GLsizei n, const GLuint* buffers);
void glBindBuffer(GLenum target, GLuint buffer);
void glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage);
void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data);
void* glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access);
GLboolean glUnmapBuffer(GLenum target);
void glBindBufferBase(GLenum target, GLuint index, GLuint buffer);
void glBindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size);

void glGenVertexArrays(GLsizei n, GLuint* arrays);
void glDeleteVertexArrays(GLsizei n, const GLuint* arrays);
void glBindVertexArray(GLuint array);
void glEnableVertexAttribArray(GLuint index);
void glDisableVertexAttribArray(GLuint index);
void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer);
void glVertexAttribIPointer(GLuint index, GLint size, GLenum type, GLsizei stride, const void* pointer);

GLuint glCreateShader(GLenum type);
void glDeleteShader(GLuint shader);
void glShaderSource(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length);
void glCompileShader(GLuint shader);
void glGetShaderiv(GLuint shader, GLenum pname, GLint* params);
void glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* infoLog);
GLuint glCreateProgram();
void glDeleteProgram(GLuint program);
void glAttachShader(GLuint program, GLuint shader);
void glLinkProgram(GLuint program);
void glGetProgramiv(GLuint program, GLenum pname, GLint* params);
void glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei* length, GLchar* infoLog);
void glUseProgram(GLuint program);
GLint glGetUniformLocation(GLuint program, const GLchar* name);
void glUniform1i(GLint location, GLint v0);
GLuint glGetUniformBlockIndex(GLuint program, const GLchar* uniformBlockName);
void glUniformBlockBinding(GLuint program, GLuint uniformBlockIndex, GLuint uniformBlockBinding);

void glGenFramebuffers(GLsizei n, GLuint* framebuffers);
void glDeleteFramebuffers(GLsizei n, const GLuint* framebuffers);
void glBindFramebuffer(GLenum target, GLuint framebuffer);
void glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
void glFramebufferTextureLayer(GLenum target, GLenum attachment, GLuint texture, GLint level, GLint layer);
GLenum glCheckFramebufferStatus(GLenum target);
void glBlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                       GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                       GLbitfield mask, GLenum filter);

void glDrawArrays(GLenum mode, GLint first, GLsizei count);
void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices);

void glGenQueries(GLsizei n, GLuint* ids);
void glDeleteQueries(GLsizei n, const GLuint* ids);
void glBeginQuery(GLenum target, GLuint id);
void glEndQuery(GLenum target);
void glGetQueryObjectuiv(GLuint id, GLenum pname, GLuint* params);
void glGetQueryObjectui64v(GLuint id, GLenum pname, GLuint64* params);

void glDebugMessageCallback(GLDEBUGPROC callback, const void* userParam);
void glDebugMessageControl(GLenum source, GLenum type, GLenum severity, GLsizei count, const GLuint* ids, GLboolean enabled);

}  // namespace vk_gl
}  // namespace me
