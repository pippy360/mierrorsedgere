#pragma once

// me::Renderer: the platform's renderer backend. Both expose the same interface apart
// from how they attach to a window (a CAMetalLayer on macOS, an HWND on Windows).

#if defined(_WIN32)
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
