#pragma once

// Texture sampling shared by the reference renderer's 2D and 3D halves.

#include "frontend_assets.hpp"

#include <algorithm>
#include <cmath>

namespace me::fe {

inline float srgb_to_linear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

inline int wrap_or_clamp(int i, int n, bool wrap) {
    if (wrap) {
        i %= n;
        return i < 0 ? i + n : i;
    }
    return std::clamp(i, 0, n - 1);
}

// Bilinear, texel centres at (i + 0.5) / size. Returns 0..1 per channel; `linear` undoes the
// sRGB encoding of the colour channels the way a material's sampler does for an SRGB texture.
inline void sample(const Image& img, float u, float v, float out[4], bool linear = false) {
    if (!img.valid()) {
        out[0] = out[1] = out[2] = out[3] = 1.0f;
        return;
    }
    const float fx = u * static_cast<float>(img.w) - 0.5f;
    const float fy = v * static_cast<float>(img.h) - 0.5f;
    const float flx = std::floor(fx), fly = std::floor(fy);
    const float tx = fx - flx, ty = fy - fly;
    const int x0 = wrap_or_clamp(static_cast<int>(flx), img.w, img.wrap_x);
    const int x1 = wrap_or_clamp(static_cast<int>(flx) + 1, img.w, img.wrap_x);
    const int y0 = wrap_or_clamp(static_cast<int>(fly), img.h, img.wrap_y);
    const int y1 = wrap_or_clamp(static_cast<int>(fly) + 1, img.h, img.wrap_y);
    const uint8_t* p00 = &img.px[(static_cast<size_t>(y0) * img.w + x0) * 4];
    const uint8_t* p10 = &img.px[(static_cast<size_t>(y0) * img.w + x1) * 4];
    const uint8_t* p01 = &img.px[(static_cast<size_t>(y1) * img.w + x0) * 4];
    const uint8_t* p11 = &img.px[(static_cast<size_t>(y1) * img.w + x1) * 4];
    for (int k = 0; k < 4; ++k) {
        float a = p00[k] / 255.0f, b = p10[k] / 255.0f, c = p01[k] / 255.0f, d = p11[k] / 255.0f;
        if (linear && k < 3) {
            a = srgb_to_linear(a);
            b = srgb_to_linear(b);
            c = srgb_to_linear(c);
            d = srgb_to_linear(d);
        }
        out[k] = (a * (1.0f - tx) + b * tx) * (1.0f - ty) + (c * (1.0f - tx) + d * tx) * ty;
    }
}

inline float saturate(float x) { return std::clamp(x, 0.0f, 1.0f); }

}  // namespace me::fe
