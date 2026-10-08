#pragma once

// -----------------------------------------------------------------------------
// Helpers every renderer backend needs in the same form: the layout of decoded
// UE3 texture data (scene_materials.hpp), which material blend modes belong to
// the translucency pass, and a conservative clip test for whole objects.
// -----------------------------------------------------------------------------

#include "../assets/scene_materials.hpp"
#include "../math/types.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace me {

inline bool tex_format_is_bc(TexFormat f) {
    return f == TexFormat::DXT1 || f == TexFormat::DXT3 || f == TexFormat::DXT5;
}

// Bytes per row of 4x4 blocks (BC) or pixels (uncompressed) and number of such rows.
inline size_t tex_row_bytes(TexFormat f, int w) {
    switch (f) {
        case TexFormat::DXT1: return static_cast<size_t>(std::max(1, (w + 3) / 4)) * 8;
        case TexFormat::DXT3:
        case TexFormat::DXT5: return static_cast<size_t>(std::max(1, (w + 3) / 4)) * 16;
        case TexFormat::BGRA8: return static_cast<size_t>(w) * 4;
        case TexFormat::V8U8: return static_cast<size_t>(w) * 2;
        default: return static_cast<size_t>(w);
    }
}
inline size_t tex_rows(TexFormat f, int h) {
    return tex_format_is_bc(f) ? static_cast<size_t>(std::max(1, (h + 3) / 4)) : static_cast<size_t>(h);
}

// Number of leading mips forming a valid mip chain (exact halving, enough data).
inline int tex_valid_chain(TexFormat f, const std::vector<TextureMip>& mips) {
    if (mips.empty() || mips[0].width <= 0 || mips[0].height <= 0) return 0;
    int n = 0;
    for (size_t i = 0; i < mips.size(); ++i) {
        const int w = std::max(1, mips[0].width >> i);
        const int h = std::max(1, mips[0].height >> i);
        if (mips[i].width != w || mips[i].height != h) break;
        if (mips[i].data.size() < tex_row_bytes(f, w) * tex_rows(f, h)) break;
        ++n;
    }
    return n;
}

inline bool mat_blend_is_translucent(MatBlendMode b) {
    return b == MatBlendMode::Translucent || b == MatBlendMode::Additive || b == MatBlendMode::Modulate;
}

// World-space side and near planes of a view-projection matrix's clip volume (Metal and Direct3D clip to -w <= x <= w,
// -w <= y <= w and 0 <= z <= w; no pipeline here changes the depth clip mode). A sphere wholly outside one of
// these planes rasterizes nothing in that pass: every triangle inside it is clipped away completely, so not
// drawing it leaves the render target unchanged. The far plane is left out because its float test is poorly
// conditioned (w - z cancels), so the GPU's rounding there cannot be bounded tightly.
struct ClipVolume {
    explicit ClipVolume(const Mat4& view_proj) {
        // Row r of the column-major matrix: clip[r] = sum over c of m[c * 4 + r] * (x, y, z, 1)[c].
        auto m = [&](int r, int c) { return static_cast<double>(view_proj.m[c * 4 + r]); };
        for (int c = 0; c < 4; ++c) {
            planes[0][c] = m(3, c) + m(0, c);  // -w <= x
            planes[1][c] = m(3, c) - m(0, c);  //  x <= w
            planes[2][c] = m(3, c) + m(1, c);  // -w <= y
            planes[3][c] = m(3, c) - m(1, c);  //  y <= w
            planes[4][c] = m(2, c);            //  0 <= z
        }
        for (auto& p : planes) {
            const double len = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
            if (len > 0.0) {
                for (double& v : p) v /= len;
            }
        }
    }

    // False only if the sphere lies outside some plane by more than `margin` UU (so a NaN centre or radius is
    // never rejected).
    [[nodiscard]] bool may_cover(const Vec3& center, float radius, double margin) const {
        for (const auto& p : planes) {
            const double dist = p[0] * center.x + p[1] * center.y + p[2] * center.z + p[3];
            if (dist < -(static_cast<double>(radius) + margin)) return false;
        }
        return true;
    }

    double planes[5][4];
};

}  // namespace me
