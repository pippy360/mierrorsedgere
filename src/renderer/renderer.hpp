#pragma once

// me::Renderer: the platform's renderer backend. All expose the same interface apart
// from how they attach to a window (a CAMetalLayer on macOS, an HWND on Windows, an
// SDL_Window with a GL context on Linux).
//
// ME_RENDERER_OPENGL selects the OpenGL backend explicitly; it is the default on
// Linux and can be built on macOS too (CMake option ME_OPENGL) to check it there.

#if defined(ME_RENDERER_OPENGL) || (!defined(_WIN32) && !defined(__APPLE__))
#ifndef ME_RENDERER_OPENGL
#define ME_RENDERER_OPENGL 1
#endif
#include "opengl_renderer.hpp"
namespace me {
using Renderer = OpenGLRenderer;
}
#elif defined(_WIN32)
#include "d3d11_renderer.hpp"
namespace me {
using Renderer = D3D11Renderer;
}
#else
#include "metal_renderer.hpp"
namespace me {
using Renderer = MetalRenderer;
}
#endif
