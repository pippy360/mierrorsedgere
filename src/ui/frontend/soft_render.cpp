#include "soft_render.hpp"

#include "soft_city.hpp"
#include "soft_sample.hpp"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace me::fe {

namespace {

// M_MainMenuStick_01's constants (UI/UI_Menus.upk).
constexpr float kStickColor[3] = {0.91575f, 0.0f, 0.0f};  // VectorParameter StickColor default
constexpr float kSelectColor[3] = {1.0f, 1.0f, 1.0f};     // TdMenuPostProcesWrapper.SelectionColor
constexpr float kShadowColor[3] = {0.1f, 0.2f, 0.4f};     // Constant3Vector "Shadow Color"
constexpr float kUVTilingX = 40.0f;                       // "Set R to tweak stick edge width"
constexpr float kUVTilingY = 0.15f;
constexpr float kStickWidthFactor = 0.9f;                 // "To keep stick within the widget"
constexpr float kSelectTiling = 512.0f;                   // "UVTiling 512"

struct Target {
    int w = 0, h = 0;
    std::vector<float>* rgb = nullptr;  // display space, three per pixel

    void blend(int x, int y, const float c[3], float a) const {
        float* d = &(*rgb)[(static_cast<size_t>(y) * w + x) * 3];
        d[0] += (c[0] - d[0]) * a;
        d[1] += (c[1] - d[1]) * a;
        d[2] += (c[2] - d[2]) * a;
    }
};

// An sRGB texel as the canvas draws it at a display gamma.
const float* canvas_lut(float gamma) {
    static float lut[256];
    static float built_for = 0.0f;
    if (built_for != gamma) {
        for (int i = 0; i < 256; ++i) lut[i] = canvas_encode(srgb_to_linear(static_cast<float>(i) / 255.0f), gamma);
        built_for = gamma;
    }
    return lut;
}

void draw_quads(const Target& t, const DrawOp& op, float gamma) {
    if (!op.image || !op.image->valid()) return;
    const bool glyphs = op.kind == DrawOp::Kind::Glyphs;
    const float* lut = canvas_lut(gamma);
    for (const Quad& q : op.quads) {
        const float qw = q.x1 - q.x0, qh = q.y1 - q.y0;
        if (qw <= 0.0f || qh <= 0.0f) continue;
        int x0 = std::max(0, static_cast<int>(std::ceil(q.x0 - 0.5f)));
        int y0 = std::max(0, static_cast<int>(std::ceil(q.y0 - 0.5f)));
        int x1 = std::min(t.w, static_cast<int>(std::ceil(q.x1 - 0.5f)));
        int y1 = std::min(t.h, static_cast<int>(std::ceil(q.y1 - 0.5f)));
        if (op.clipped) {
            x0 = std::max(x0, static_cast<int>(std::ceil(op.clip.l - 0.5f)));
            y0 = std::max(y0, static_cast<int>(std::ceil(op.clip.t - 0.5f)));
            x1 = std::min(x1, static_cast<int>(std::ceil(op.clip.r - 0.5f)));
            y1 = std::min(y1, static_cast<int>(std::ceil(op.clip.b - 0.5f)));
        }
        for (int y = y0; y < y1; ++y) {
            const float v = q.v0 + (static_cast<float>(y) + 0.5f - q.y0) / qh * (q.v1 - q.v0);
            for (int x = x0; x < x1; ++x) {
                const float u = q.u0 + (static_cast<float>(x) + 0.5f - q.x0) / qw * (q.u1 - q.u0);
                float s[4];
                sample(*op.image, u, v, s);
                if (glyphs) {
                    t.blend(x, y, op.color, s[3] * op.color[3]);
                } else {
                    float c[3];
                    for (int k = 0; k < 3; ++k) {
                        // `s` is a filtered 0..1 texel value: interpolate in the table.
                        const float f = std::clamp(s[k], 0.0f, 1.0f) * 255.0f;
                        const int i0 = static_cast<int>(f);
                        const int i1 = std::min(i0 + 1, 255);
                        c[k] = (lut[i0] + (lut[i1] - lut[i0]) * (f - static_cast<float>(i0))) * op.color[k];
                    }
                    t.blend(x, y, c, s[3] * op.color[3]);
                }
            }
        }
    }
}

// M_MainMenuStick_01, evaluated per pixel of the widget (docs/MAIN_MENU_SYSTEM_RE.md, 3.4).
void draw_stick(const Target& t, const Assets& a, const DrawOp& op, double time) {
    const Rect& r = op.rect;
    if (r.w() <= 0.0f || r.h() <= 0.0f) return;
    const StickParams& p = op.stick;
    const float tm = static_cast<float>(time);

    // "Movement": one sample of the timeline texture, shared by the whole stick.
    const float mcoord = p.move_offset * 5.0f + tm * 0.015f;
    float timeline[4];
    sample(a.stick_timeline, mcoord, mcoord, timeline, a.stick_timeline.srgb);
    const float move = (timeline[1] - 0.5f) * 5.0f * p.move_amount;

    const float half = kUVTilingX * 0.5f;
    const float width = p.width * kStickWidthFactor;
    const float right_x = width * half + half + move;
    const float left_x = half * (1.0f - width) - 1.0f + move;
    const float right_pan = kUVTilingY * (p.right_offset * 5.0f + tm);
    const float left_pan = -kUVTilingY * (p.left_offset * 5.0f + tm);

    const int x0 = std::max(0, static_cast<int>(std::ceil(r.l - 0.5f)));
    const int y0 = std::max(0, static_cast<int>(std::ceil(r.t - 0.5f)));
    const int x1 = std::min(t.w, static_cast<int>(std::ceil(r.r - 0.5f)));
    const int y1 = std::min(t.h, static_cast<int>(std::ceil(r.b - 0.5f)));
    parallel_rows(y1 - y0, [&](int band0, int band1) {
    for (int y = y0 + band0; y < y0 + band1; ++y) {
        const float v = (static_cast<float>(y) + 0.5f - r.t) / r.h();
        // "Selection field"
        const float field = saturate(saturate(p.select_top * kSelectTiling - v * kSelectTiling) +
                                     saturate(v * kSelectTiling - p.select_bottom * kSelectTiling) + (1.0f - p.select_opacity));
        float body[3];
        for (int k = 0; k < 3; ++k) body[k] = kSelectColor[k] + (kStickColor[k] - kSelectColor[k]) * field;
        const float tv = v * kUVTilingY;
        for (int x = x0; x < x1; ++x) {
            const float u = (static_cast<float>(x) + 0.5f - r.l) / r.w() * kUVTilingX;
            float left[4];
            sample(a.stick_left, u - left_x, tv + left_pan, left, a.stick_left.srgb);
            if (left[0] <= 0.0f) continue;
            float right[4], shadow[4];
            sample(a.stick_right, u - right_x, tv + right_pan, right, a.stick_right.srgb);
            sample(a.stick_shadow, u - right_x - 0.5f, tv + right_pan, shadow, a.stick_shadow.srgb);
            const float opacity = saturate(right[0] + shadow[0]) * left[0];
            if (opacity <= 0.0f) continue;
            float c[3];
            for (int k = 0; k < 3; ++k) c[k] = kShadowColor[k] + (body[k] - kShadowColor[k]) * right[1];
            t.blend(x, y, c, opacity);
        }
    }
    });
}

uint32_t be32(uint32_t v) { return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24); }

void png_chunk(std::FILE* f, const char type[4], const uint8_t* data, size_t size) {
    const uint32_t len = be32(static_cast<uint32_t>(size));
    std::fwrite(&len, 4, 1, f);
    std::fwrite(type, 1, 4, f);
    if (size) std::fwrite(data, 1, size, f);
    uLong crc = crc32(0L, reinterpret_cast<const Bytef*>(type), 4);
    if (size) crc = crc32(crc, data, static_cast<uInt>(size));
    const uint32_t c = be32(static_cast<uint32_t>(crc));
    std::fwrite(&c, 4, 1, f);
}

}  // namespace

struct SoftRenderer::Impl {
    explicit Impl(const City& city) : city(city) {}
    CityRenderer city;
    std::vector<float> color;
};

SoftRenderer::SoftRenderer(const Assets& assets) : impl_(std::make_unique<Impl>(assets.city)), assets_(assets) {}
SoftRenderer::~SoftRenderer() = default;

const std::vector<float>& SoftRenderer::linear_scene() const { return impl_->city.linear(); }

void SoftRenderer::render(const Frame& frame, std::vector<uint8_t>& rgba) {
    const int w = frame.width, h = frame.height;
    std::vector<float>& color = impl_->color;
    color.assign(static_cast<size_t>(w) * h * 3, 0.0f);

    if (background_ && assets_.city.valid()) {
        impl_->city.render(frame, w, h, color);
    } else {
        // The menu sky's colour near the top of a retail frame.
        for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
            color[i * 3 + 0] = 227.0f / 255.0f;
            color[i * 3 + 1] = 238.0f / 255.0f;
            color[i * 3 + 2] = 241.0f / 255.0f;
        }
    }

    // SeqAct_TdFadeEffect: the scene, not the UI, goes to white.
    if (frame.white > 0.0f) {
        for (float& c : color) c += (1.0f - c) * frame.white;
    }

    if (ui_) {
        const Target target{w, h, &color};
        for (const DrawOp& op : frame.ui) {
            if (op.kind == DrawOp::Kind::Stick) draw_stick(target, assets_, op, frame.time);
            else draw_quads(target, op, frame.display_gamma);
        }
    }

    rgba.resize(static_cast<size_t>(w) * h * 4);
    parallel_rows(h, [&](int row0, int row1) {
        for (size_t i = static_cast<size_t>(row0) * w; i < static_cast<size_t>(row1) * w; ++i) {
            for (int k = 0; k < 3; ++k) {
                rgba[i * 4 + k] = static_cast<uint8_t>(std::clamp(color[i * 3 + k], 0.0f, 1.0f) * 255.0f + 0.5f);
            }
            rgba[i * 4 + 3] = 255;
        }
    });
}

bool write_png(const std::string& path, int width, int height, const uint8_t* rgba) {
    if (width <= 0 || height <= 0 || !rgba) return false;
    // Filter type 0 on every row, RGB8.
    std::vector<uint8_t> raw(static_cast<size_t>(height) * (static_cast<size_t>(width) * 3 + 1));
    for (int y = 0; y < height; ++y) {
        uint8_t* row = &raw[static_cast<size_t>(y) * (static_cast<size_t>(width) * 3 + 1)];
        *row++ = 0;
        for (int x = 0; x < width; ++x) {
            const uint8_t* p = rgba + (static_cast<size_t>(y) * width + x) * 4;
            *row++ = p[0];
            *row++ = p[1];
            *row++ = p[2];
        }
    }
    uLongf packed_size = compressBound(static_cast<uLong>(raw.size()));
    std::vector<uint8_t> packed(packed_size);
    if (compress2(packed.data(), &packed_size, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK) return false;

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    static const uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::fwrite(kSignature, 1, 8, f);
    uint8_t ihdr[13];
    const uint32_t bw = be32(static_cast<uint32_t>(width)), bh = be32(static_cast<uint32_t>(height));
    std::memcpy(ihdr, &bw, 4);
    std::memcpy(ihdr + 4, &bh, 4);
    ihdr[8] = 8;   // bit depth
    ihdr[9] = 2;   // truecolour
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", packed.data(), packed_size);
    png_chunk(f, "IEND", nullptr, 0);
    return std::fclose(f) == 0;
}

}  // namespace me::fe
