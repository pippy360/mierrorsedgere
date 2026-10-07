#include "soft_city.hpp"

#include "soft_sample.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

namespace me::fe {

namespace {

// Per-vertex values carried across a triangle: uv0, uv1, uv2, colour, world position.
constexpr int kAttrs = 12;
constexpr float kNear = 2.0f;

// M_CityBuildings_01 / M_CityBase_01: Constant3Vector diffuse.
constexpr float kBuildingDiffuse[3] = {0.8f, 0.83f, 0.9f};
// M_Skydome_Menu.
constexpr float kSkyTint[3] = {0.75f, 0.88f, 1.0f};

struct Camera {
    Vec3 pos, right, up, fwd;
    float tan_x = 1.0f, tan_y = 1.0f;
};

// UE3 is left-handed with Z up: right = up x forward.
Camera make_camera(const Vec3& pos, const Vec3& target, float fov_deg, int w, int h) {
    Camera c;
    c.pos = pos;
    c.fwd = (target - pos).normalized();
    if (c.fwd.length_sq() < 0.5f) c.fwd = Vec3{0.0f, 1.0f, 0.0f};
    c.right = Vec3{0.0f, 0.0f, 1.0f}.cross(c.fwd).normalized();
    if (c.right.length_sq() < 0.5f) c.right = Vec3{0.0f, 1.0f, 0.0f};
    c.up = c.fwd.cross(c.right);
    // CameraActor.FOVAngle is the horizontal field of view.
    c.tan_x = std::tan(std::clamp(fov_deg, 5.0f, 170.0f) * 0.5f * 3.14159265f / 180.0f);
    c.tan_y = c.tan_x * static_cast<float>(h) / static_cast<float>(w);
    return c;
}

struct Vtx {
    float x, y, iz;       // screen position and 1 / view depth
    float a[kAttrs];      // attributes, already divided by view depth
};

struct Surface {
    int w = 0, h = 0;
    std::vector<float> rgb;    // linear radiance
    std::vector<float> depth;  // 1 / view depth, 0 = nothing drawn
    void reset(int width, int height) {
        w = width;
        h = height;
        rgb.assign(static_cast<size_t>(w) * h * 3, 0.0f);
        depth.assign(static_cast<size_t>(w) * h, 0.0f);
    }
};

void sample_lightmap(const LightMap& lm, float u, float v, float out[3]) {
    const float fx = std::clamp(u * lm.scale[0] + lm.bias[0], 0.0f, 1.0f) * static_cast<float>(lm.w) - 0.5f;
    const float fy = std::clamp(v * lm.scale[1] + lm.bias[1], 0.0f, 1.0f) * static_cast<float>(lm.h) - 0.5f;
    const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, lm.w - 1), x1 = std::min(x0 + 1, lm.w - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, lm.h - 1), y1 = std::min(y0 + 1, lm.h - 1);
    const float tx = std::clamp(fx - std::floor(fx), 0.0f, 1.0f), ty = std::clamp(fy - std::floor(fy), 0.0f, 1.0f);
    const float* p00 = &lm.rgb[(static_cast<size_t>(y0) * lm.w + x0) * 3];
    const float* p10 = &lm.rgb[(static_cast<size_t>(y0) * lm.w + x1) * 3];
    const float* p01 = &lm.rgb[(static_cast<size_t>(y1) * lm.w + x0) * 3];
    const float* p11 = &lm.rgb[(static_cast<size_t>(y1) * lm.w + x1) * 3];
    for (int k = 0; k < 3; ++k) out[k] = (p00[k] * (1.0f - tx) + p10[k] * tx) * (1.0f - ty) + (p01[k] * (1.0f - tx) + p11[k] * tx) * ty;
}

struct Pass {
    const City* city = nullptr;
    Camera cam;
    Surface* target = nullptr;
    const Surface* reflection = nullptr;  // null while the reflection itself is being drawn
    Camera reflection_cam;
    float time = 0.0f;
    bool mirror = false;  // drawing the reflection: skip the water and everything below it
};

// One pixel of one material. `a` holds the interpolated attributes.
bool shade(const Pass& pass, const CityBatch& batch, const float a[kAttrs], float out[3]) {
    const City& city = *pass.city;
    if (pass.mirror && a[11] < city.water_z) return false;
    float s[4];
    switch (batch.material) {
        case CityMaterial::Sky: {
            // Desaturation(((VertexColor * 2.4) - 0.55) * (0.75, 0.88, 1.0), 0.7)
            float c[3];
            for (int k = 0; k < 3; ++k) c[k] = (a[6 + k] * 2.4f - 0.55f) * kSkyTint[k];
            const float lum = c[0] * 0.3f + c[1] * 0.59f + c[2] * 0.11f;
            for (int k = 0; k < 3; ++k) out[k] = c[k] + (lum - c[k]) * 0.7f;
            return true;
        }
        case CityMaterial::Buildings: {
            float light[3] = {1.0f, 1.0f, 1.0f};
            if (batch.lightmap >= 0) sample_lightmap(city.lightmaps[static_cast<size_t>(batch.lightmap)], a[0], a[1], light);
            for (int k = 0; k < 3; ++k) out[k] = kBuildingDiffuse[k] * light[k];
            return true;
        }
        case CityMaterial::Base: {
            // Diffuse = T_CityFade(uv1) * (0.8, 0.83, 0.9); Emissive = (1 - fade) * (T_Skydome(uv2) * 2.2 - 0.5)
            float fade[4], sky[4], light[3] = {1.0f, 1.0f, 1.0f};
            sample(city.fade, a[2], a[3], fade, city.fade.srgb);
            sample(city.sky, a[4], a[5], sky, city.sky.srgb);
            if (batch.lightmap >= 0) sample_lightmap(city.lightmaps[static_cast<size_t>(batch.lightmap)], a[0], a[1], light);
            for (int k = 0; k < 3; ++k) {
                out[k] = fade[k] * kBuildingDiffuse[k] * light[k] + (1.0f - fade[k]) * (sky[k] * 2.2f - 0.5f);
            }
            return true;
        }
        case CityMaterial::Water: {
            if (pass.mirror) return false;
            // Emissive = lerp(Reflection(screen) * 1.4 - 0.05, T_Skydome(uv2) * 2.0 - 0.3, 1 - T_CityFade(uv1).g)
            float fade[4], sky[4];
            sample(city.fade, a[2], a[3], fade, city.fade.srgb);
            sample(city.sky, a[4], a[5], sky, city.sky.srgb);
            float refl[3] = {sky[0], sky[1], sky[2]};
            if (pass.reflection) {
                // Where the mirrored camera sees this point of the water plane.
                const Camera& rc = pass.reflection_cam;
                const Vec3 d = Vec3{a[9], a[10], a[11]} - rc.pos;
                const float z = d.dot(rc.fwd);
                if (z > 1.0e-3f) {
                    const Surface& r = *pass.reflection;
                    const float px = (d.dot(rc.right) / (z * rc.tan_x) * 0.5f + 0.5f) * static_cast<float>(r.w) - 0.5f;
                    const float py = (0.5f - d.dot(rc.up) / (z * rc.tan_y) * 0.5f) * static_cast<float>(r.h) - 0.5f;
                    const int x0 = std::clamp(static_cast<int>(std::floor(px)), 0, r.w - 1), x1 = std::min(x0 + 1, r.w - 1);
                    const int y0 = std::clamp(static_cast<int>(std::floor(py)), 0, r.h - 1), y1 = std::min(y0 + 1, r.h - 1);
                    const float tx = std::clamp(px - std::floor(px), 0.0f, 1.0f), ty = std::clamp(py - std::floor(py), 0.0f, 1.0f);
                    for (int k = 0; k < 3; ++k) {
                        const float top = r.rgb[(static_cast<size_t>(y0) * r.w + x0) * 3 + k] * (1.0f - tx) +
                                          r.rgb[(static_cast<size_t>(y0) * r.w + x1) * 3 + k] * tx;
                        const float bot = r.rgb[(static_cast<size_t>(y1) * r.w + x0) * 3 + k] * (1.0f - tx) +
                                          r.rgb[(static_cast<size_t>(y1) * r.w + x1) * 3 + k] * tx;
                        // The capture target is an 8-bit texture: it holds 0..1.
                        refl[k] = std::clamp(top * (1.0f - ty) + bot * ty, 0.0f, 1.0f);
                    }
                }
            }
            const float alpha = 1.0f - fade[1];
            for (int k = 0; k < 3; ++k) {
                const float near_c = refl[k] * 1.4f - 0.05f;
                const float far_c = sky[k] * 2.0f - 0.3f;
                out[k] = near_c + (far_c - near_c) * alpha;
            }
            return true;
        }
        case CityMaterial::Waves: {
            if (pass.mirror) return false;
            // Additive, Emissive 200, Opacity = Waves(uv*25 panned 0.005) * Waves(uv*15 panned 0.015)
            sample(city.waves, a[0] * 25.0f, a[1] * 25.0f + pass.time * 0.005f, s, city.waves.srgb);
            const float w1 = s[0];
            sample(city.waves, a[0] * 15.0f, a[1] * 15.0f + pass.time * 0.015f, s, city.waves.srgb);
            const float o = 200.0f * w1 * s[0];
            out[0] = out[1] = out[2] = o;
            return true;
        }
    }
    return false;
}

inline float edge(const Vtx& a, const Vtx& b, float x, float y) { return (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x); }

// Rasterises one screen-space triangle into rows [row0, row1).
void raster(const Pass& pass, const CityBatch& batch, const Vtx& v0, const Vtx& v1, const Vtx& v2, int row0, int row1) {
    Surface& t = *pass.target;
    float area = edge(v0, v1, v2.x, v2.y);
    if (area == 0.0f) return;
    const Vtx* p0 = &v0;
    const Vtx* p1 = &v1;
    const Vtx* p2 = &v2;
    if (area < 0.0f) {
        std::swap(p1, p2);
        area = -area;
    }
    const float minx = std::min({p0->x, p1->x, p2->x}), maxx = std::max({p0->x, p1->x, p2->x});
    const float miny = std::min({p0->y, p1->y, p2->y}), maxy = std::max({p0->y, p1->y, p2->y});
    const int x0 = std::max(0, static_cast<int>(std::ceil(minx - 0.5f)));
    const int x1 = std::min(t.w - 1, static_cast<int>(std::floor(maxx - 0.5f)));
    const int y0 = std::max(row0, static_cast<int>(std::ceil(miny - 0.5f)));
    const int y1 = std::min(row1 - 1, static_cast<int>(std::floor(maxy - 0.5f)));
    if (x0 > x1 || y0 > y1) return;
    const bool additive = batch.material == CityMaterial::Waves;
    const float inv_area = 1.0f / area;
    for (int y = y0; y <= y1; ++y) {
        const float py = static_cast<float>(y) + 0.5f;
        for (int x = x0; x <= x1; ++x) {
            const float px = static_cast<float>(x) + 0.5f;
            const float w0 = edge(*p1, *p2, px, py), w1 = edge(*p2, *p0, px, py), w2 = edge(*p0, *p1, px, py);
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
            const float b0 = w0 * inv_area, b1 = w1 * inv_area, b2 = w2 * inv_area;
            const float iz = b0 * p0->iz + b1 * p1->iz + b2 * p2->iz;
            const size_t pix = static_cast<size_t>(y) * t.w + x;
            if (additive) {
                if (iz < t.depth[pix]) continue;  // behind something solid
            } else if (iz <= t.depth[pix]) {
                continue;
            }
            float a[kAttrs];
            const float z = 1.0f / iz;
            for (int k = 0; k < kAttrs; ++k) a[k] = (b0 * p0->a[k] + b1 * p1->a[k] + b2 * p2->a[k]) * z;
            float c[3];
            if (!shade(pass, batch, a, c)) continue;
            float* d = &t.rgb[pix * 3];
            if (additive) {
                d[0] += c[0];
                d[1] += c[1];
                d[2] += c[2];
            } else {
                d[0] = c[0];
                d[1] = c[1];
                d[2] = c[2];
                t.depth[pix] = iz;
            }
        }
    }
}

struct ViewVtx {
    float vx, vy, vz;  // camera space: right, up, forward
    float a[kAttrs];
};

ViewVtx to_view(const Camera& cam, const CityVertex& v) {
    ViewVtx o;
    const Vec3 d = v.pos - cam.pos;
    o.vx = d.dot(cam.right);
    o.vy = d.dot(cam.up);
    o.vz = d.dot(cam.fwd);
    o.a[0] = v.uv[0][0];
    o.a[1] = v.uv[0][1];
    o.a[2] = v.uv[1][0];
    o.a[3] = v.uv[1][1];
    o.a[4] = v.uv[2][0];
    o.a[5] = v.uv[2][1];
    o.a[6] = v.color[0];
    o.a[7] = v.color[1];
    o.a[8] = v.color[2];
    o.a[9] = v.pos.x;
    o.a[10] = v.pos.y;
    o.a[11] = v.pos.z;
    return o;
}

Vtx project(const Camera& cam, const ViewVtx& v, int w, int h) {
    Vtx o;
    o.iz = 1.0f / v.vz;
    o.x = (v.vx * o.iz / cam.tan_x * 0.5f + 0.5f) * static_cast<float>(w);
    o.y = (0.5f - v.vy * o.iz / cam.tan_y * 0.5f) * static_cast<float>(h);
    for (int k = 0; k < kAttrs; ++k) o.a[k] = v.a[k] * o.iz;
    return o;
}

// Draws every batch of `additive`-ness into rows [row0, row1) of the pass's target.
void draw_rows(const Pass& pass, bool additive, int row0, int row1) {
    const Surface& t = *pass.target;
    const Camera& cam = pass.cam;
    const float fw = static_cast<float>(t.w), fh = static_cast<float>(t.h);
    for (const CityBatch& batch : pass.city->batches) {
        if ((batch.material == CityMaterial::Waves) != additive) continue;
        if (pass.mirror && (batch.material == CityMaterial::Water || batch.material == CityMaterial::Waves)) continue;
        const size_t n = batch.tris.size() / 3;
        for (size_t i = 0; i < n; ++i) {
            const CityVertex* cv = &batch.tris[i * 3];
            // Cheap rejects on depth and on the row band before any attribute work.
            float vz[3], sy[3], sx[3];
            int behind = 0;
            for (int k = 0; k < 3; ++k) {
                const Vec3 d = cv[k].pos - cam.pos;
                vz[k] = d.dot(cam.fwd);
                if (vz[k] < kNear) {
                    ++behind;
                    continue;
                }
                sx[k] = (d.dot(cam.right) / (vz[k] * cam.tan_x) * 0.5f + 0.5f) * fw;
                sy[k] = (0.5f - d.dot(cam.up) / (vz[k] * cam.tan_y) * 0.5f) * fh;
            }
            if (behind == 3) continue;
            if (behind == 0) {
                if (std::max({sy[0], sy[1], sy[2]}) < static_cast<float>(row0) || std::min({sy[0], sy[1], sy[2]}) > static_cast<float>(row1)) continue;
                if (std::max({sx[0], sx[1], sx[2]}) < 0.0f || std::min({sx[0], sx[1], sx[2]}) > fw) continue;
                const Vtx p0 = project(cam, to_view(cam, cv[0]), t.w, t.h);
                const Vtx p1 = project(cam, to_view(cam, cv[1]), t.w, t.h);
                const Vtx p2 = project(cam, to_view(cam, cv[2]), t.w, t.h);
                raster(pass, batch, p0, p1, p2, row0, row1);
                continue;
            }
            // Crosses the near plane: clip the triangle to it (at most a quad).
            ViewVtx in[3] = {to_view(cam, cv[0]), to_view(cam, cv[1]), to_view(cam, cv[2])};
            ViewVtx poly[4];
            int count = 0;
            for (int k = 0; k < 3; ++k) {
                const ViewVtx& a = in[k];
                const ViewVtx& b = in[(k + 1) % 3];
                const bool ain = a.vz >= kNear, bin = b.vz >= kNear;
                if (ain) poly[count++] = a;
                if (ain != bin) {
                    const float s = (kNear - a.vz) / (b.vz - a.vz);
                    ViewVtx c;
                    c.vx = a.vx + (b.vx - a.vx) * s;
                    c.vy = a.vy + (b.vy - a.vy) * s;
                    c.vz = kNear;
                    for (int j = 0; j < kAttrs; ++j) c.a[j] = a.a[j] + (b.a[j] - a.a[j]) * s;
                    poly[count++] = c;
                }
            }
            if (count < 3) continue;
            const Vtx p0 = project(cam, poly[0], t.w, t.h);
            for (int k = 1; k + 1 < count; ++k) {
                raster(pass, batch, p0, project(cam, poly[k], t.w, t.h), project(cam, poly[k + 1], t.w, t.h), row0, row1);
            }
        }
    }
}

void draw_pass(const Pass& pass) {
    const int h = pass.target->h;
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const int threads = static_cast<int>(std::min<unsigned>(hw, 16u));
    // Bands, not a work queue: each thread owns its rows, so nothing is shared while drawing.
    // Opaque geometry first, then the additive water sparkle against the finished depth.
    for (bool additive : {false, true}) {
        std::vector<std::thread> pool;
        for (int i = 0; i < threads; ++i) {
            const int row0 = h * i / threads, row1 = h * (i + 1) / threads;
            if (row0 == row1) continue;
            pool.emplace_back([&pass, additive, row0, row1] { draw_rows(pass, additive, row0, row1); });
        }
        for (std::thread& th : pool) th.join();
    }
}

// The scene's exposure, measured: with the level's bloom and colour curve in place this is the
// one number left that makes a retail frame's sky and buildings land where they do. It is
// 0.61 in the mid-tones and 0.55 at the top of the range; the level asks for
// Scene_ExposureManual 0.83 inside an automatic range of 0.79 to 0.95, so the tone mapper
// evidently does more than multiply.
constexpr float kExposure = 0.58f;

// Bloom, exposure, gamma and the level's colour curve: linear scene colour to display values.
void tone_map(const City& city, const Surface& scene, std::vector<float>& bloom, std::vector<float>& out) {
    const int w = scene.w, h = scene.h;
    // UE3 bloom: a wide blur of the quarter-resolution scene, scaled by Bloom_Scale and added.
    const int bw = std::max(1, w / 4), bh = std::max(1, h / 4);
    std::vector<float> small(static_cast<size_t>(bw) * bh * 3, 0.0f), tmp(small.size());
    for (int y = 0; y < bh; ++y) {
        for (int x = 0; x < bw; ++x) {
            float sum[3] = {0.0f, 0.0f, 0.0f};
            int n = 0;
            for (int j = 0; j < 4; ++j) {
                for (int i = 0; i < 4; ++i) {
                    const int sx = x * 4 + i, sy = y * 4 + j;
                    if (sx >= w || sy >= h) continue;
                    const float* p = &scene.rgb[(static_cast<size_t>(sy) * w + sx) * 3];
                    // The sparkle on the water is 200 times white; one pixel of it must not flood the blur.
                    for (int c = 0; c < 3; ++c) sum[c] += std::min(p[c], 4.0f);
                    ++n;
                }
            }
            for (int c = 0; c < 3; ++c) small[(static_cast<size_t>(y) * bw + x) * 3 + c] = n ? sum[c] / static_cast<float>(n) : 0.0f;
        }
    }
    const int radius = std::max(2, bw / 26);  // about 50 pixels of a 1280-wide frame, the level's blur kernel
    std::vector<float> kernel(static_cast<size_t>(radius) * 2 + 1);
    float ksum = 0.0f;
    for (int i = -radius; i <= radius; ++i) {
        const float sigma = static_cast<float>(radius) * 0.5f;
        kernel[static_cast<size_t>(i + radius)] = std::exp(-0.5f * static_cast<float>(i * i) / (sigma * sigma));
        ksum += kernel[static_cast<size_t>(i + radius)];
    }
    for (float& k : kernel) k /= ksum;
    for (int pass = 0; pass < 2; ++pass) {
        const std::vector<float>& src = pass == 0 ? small : tmp;
        std::vector<float>& dst = pass == 0 ? tmp : small;
        for (int y = 0; y < bh; ++y) {
            for (int x = 0; x < bw; ++x) {
                float sum[3] = {0.0f, 0.0f, 0.0f};
                for (int i = -radius; i <= radius; ++i) {
                    const int sx = pass == 0 ? std::clamp(x + i, 0, bw - 1) : x;
                    const int sy = pass == 0 ? y : std::clamp(y + i, 0, bh - 1);
                    const float* p = &src[(static_cast<size_t>(sy) * bw + sx) * 3];
                    const float k = kernel[static_cast<size_t>(i + radius)];
                    sum[0] += p[0] * k;
                    sum[1] += p[1] * k;
                    sum[2] += p[2] * k;
                }
                float* d = &dst[(static_cast<size_t>(y) * bw + x) * 3];
                d[0] = sum[0];
                d[1] = sum[1];
                d[2] = sum[2];
            }
        }
    }
    bloom.swap(small);

    out.resize(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y) {
        const float fy = (static_cast<float>(y) + 0.5f) / 4.0f - 0.5f;
        const int y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, bh - 1), y1 = std::min(y0 + 1, bh - 1);
        const float ty = std::clamp(fy - std::floor(fy), 0.0f, 1.0f);
        for (int x = 0; x < w; ++x) {
            const float fx = (static_cast<float>(x) + 0.5f) / 4.0f - 0.5f;
            const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, bw - 1), x1 = std::min(x0 + 1, bw - 1);
            const float tx = std::clamp(fx - std::floor(fx), 0.0f, 1.0f);
            const float* s = &scene.rgb[(static_cast<size_t>(y) * w + x) * 3];
            float* o = &out[(static_cast<size_t>(y) * w + x) * 3];
            for (int c = 0; c < 3; ++c) {
                const float b = (bloom[(static_cast<size_t>(y0) * bw + x0) * 3 + c] * (1.0f - tx) + bloom[(static_cast<size_t>(y0) * bw + x1) * 3 + c] * tx) * (1.0f - ty) +
                                (bloom[(static_cast<size_t>(y1) * bw + x0) * 3 + c] * (1.0f - tx) + bloom[(static_cast<size_t>(y1) * bw + x1) * 3 + c] * tx) * ty;
                const float v = std::clamp((s[c] + b * city.bloom_scale) * kExposure, 0.0f, 1.0f);
                const float g = std::pow(v, 1.0f / 2.2f);
                const int i = std::min(static_cast<int>(g * 16.0f), 15);
                o[c] = std::clamp(city.curve_m[i][c] * g + city.curve_b[i][c], 0.0f, 1.0f);
            }
        }
    }
}

}  // namespace

struct CityRenderer::Impl {
    explicit Impl(const City& c) : city(c) {}
    const City& city;
    Surface scene;
    Surface reflection;
    std::vector<float> bloom;
};

CityRenderer::CityRenderer(const City& city) : impl_(std::make_unique<Impl>(city)) {}
CityRenderer::~CityRenderer() = default;

const std::vector<float>& CityRenderer::linear() const { return impl_->scene.rgb; }

void CityRenderer::render(const Frame& frame, int w, int h, std::vector<float>& rgb) {
    const City& city = impl_->city;

    // SceneCaptureReflectActor: the scene from the camera mirrored in the water plane.
    const int rw = std::max(16, w / 2), rh = std::max(16, h / 2);
    impl_->reflection.reset(rw, rh);
    Pass mirror;
    mirror.city = &city;
    mirror.mirror = true;
    mirror.time = static_cast<float>(frame.time);
    mirror.target = &impl_->reflection;
    const Vec3 mpos{frame.camera.x, frame.camera.y, 2.0f * city.water_z - frame.camera.z};
    const Vec3 mtarget{frame.target.x, frame.target.y, 2.0f * city.water_z - frame.target.z};
    mirror.cam = make_camera(mpos, mtarget, frame.fov, rw, rh);
    draw_pass(mirror);

    impl_->scene.reset(w, h);
    Pass main;
    main.city = &city;
    main.time = static_cast<float>(frame.time);
    main.target = &impl_->scene;
    main.cam = make_camera(frame.camera, frame.target, frame.fov, w, h);
    main.reflection = &impl_->reflection;
    main.reflection_cam = mirror.cam;
    draw_pass(main);

    tone_map(city, impl_->scene, impl_->bloom, rgb);
}

}  // namespace me::fe
