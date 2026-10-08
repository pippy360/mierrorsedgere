#include "soft_city.hpp"

#include "soft_sample.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace me::fe {

namespace {

// Per-vertex values carried across a triangle: uv0, uv1, uv2, colour, world position.
constexpr int kAttrs = 12;
constexpr float kNear = 2.0f;

// M_CityBuildings_01 / M_CityBase_01: Constant3Vector diffuse.
constexpr float kBuildingDiffuse[3] = {0.8f, 0.83f, 0.9f};
constexpr float kSelectedDiffuse[3] = {0.5f, 0.0f, 0.0f};
constexpr float kSelectedEmissive[3] = {0.1f, 0.0f, 0.0f};
// M_Skydome_Menu.
constexpr float kSkyTint[3] = {0.75f, 0.88f, 1.0f};

struct Camera {
    Vec3 pos, right, up, fwd;
    float tan_x = 1.0f, tan_y = 1.0f;
};

// UE3 is left-handed with Z up: right = up x forward.
Camera make_camera(const Vec3& pos, const Vec3& target, float roll_deg, float fov_deg, int w, int h) {
    Camera c;
    c.pos = pos;
    c.fwd = (target - pos).normalized();
    if (c.fwd.length_sq() < 0.5f) c.fwd = Vec3{0.0f, 1.0f, 0.0f};
    c.right = Vec3{0.0f, 0.0f, 1.0f}.cross(c.fwd).normalized();
    if (c.right.length_sq() < 0.5f) c.right = Vec3{0.0f, 1.0f, 0.0f};
    c.up = c.fwd.cross(c.right);
    if (roll_deg != 0.0f) {
        // FRotationMatrix: a positive roll turns the right axis down and the up axis to the right.
        const float r = roll_deg * 3.14159265f / 180.0f, cr = std::cos(r), sr = std::sin(r);
        const Vec3 right = c.right * cr - c.up * sr;
        c.up = c.right * sr + c.up * cr;
        c.right = right;
    }
    // CameraActor.FOVAngle is the horizontal field of view.
    c.tan_x = std::tan(std::clamp(fov_deg, 5.0f, 170.0f) * 0.5f * 3.14159265f / 180.0f);
    c.tan_y = c.tan_x * static_cast<float>(h) / static_cast<float>(w);
    return c;
}

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
    const std::vector<float>* selected = nullptr;  // Frame::district_selected
};

// One pixel of one material. `a` holds the interpolated attributes.
bool shade(const Pass& pass, const CityBatch& batch, const float a[kAttrs], float out[3]) {
    const City& city = *pass.city;
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
            // Diffuse lerp((0.8, 0.83, 0.9), (0.5, 0, 0), Selected); emissive (0.1, 0, 0) * Selected.
            float sel = 0.0f;
            if (pass.selected && batch.district >= 0 && static_cast<size_t>(batch.district) < pass.selected->size()) {
                sel = std::clamp((*pass.selected)[static_cast<size_t>(batch.district)], 0.0f, 1.0f);
            }
            for (int k = 0; k < 3; ++k) {
                const float diffuse = kBuildingDiffuse[k] + (kSelectedDiffuse[k] - kBuildingDiffuse[k]) * sel;
                out[k] = diffuse * light[k] + kSelectedEmissive[k] * sel;
            }
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

// --- rasterisation ------------------------------------------------------------------------
//
// A visibility buffer: every opaque triangle is first drawn as depth and an id only, then each
// pixel is shaded once from the triangle that won it. The city overdraws itself several times
// from most cameras, and interpolating a dozen attributes for pixels that end up hidden was
// most of a frame.

// A triangle on screen. `first` is its first vertex in the batch, or its slot in the pass's
// clipped attributes when the near plane cut it.
struct ScreenTri {
    float x[3], y[3], iz[3];
    float wz[3];           // world height at each corner, for the reflection's water-plane cut
    float ymin, ymax;
    uint32_t batch;
    uint32_t first;
    bool clipped;
    bool crosses_water;
};

struct ClippedAttrs {
    float a[3][kAttrs];
};

struct TriList {
    std::vector<ScreenTri> tris;
    std::vector<ClippedAttrs> clipped;
};

struct ViewVtx {
    float vx, vy, vz;  // camera space: right, up, forward
    float a[kAttrs];
};

void vertex_attrs(const CityVertex& v, float a[kAttrs]) {
    a[0] = v.uv[0][0];
    a[1] = v.uv[0][1];
    a[2] = v.uv[1][0];
    a[3] = v.uv[1][1];
    a[4] = v.uv[2][0];
    a[5] = v.uv[2][1];
    a[6] = v.color[0];
    a[7] = v.color[1];
    a[8] = v.color[2];
    a[9] = v.pos.x;
    a[10] = v.pos.y;
    a[11] = v.pos.z;
}

// Transforms, clips and projects the triangles [t0, t1) of the flattened batch list.
void build_tris(const Pass& pass, const std::vector<size_t>& batch_start, size_t t0, size_t t1, TriList& out) {
    const City& city = *pass.city;
    const Camera& cam = pass.cam;
    const float fw = static_cast<float>(pass.target->w), fh = static_cast<float>(pass.target->h);
    auto to_screen = [&](float vx, float vy, float vz, float& sx, float& sy, float& iz) {
        iz = 1.0f / vz;
        sx = (vx * iz / cam.tan_x * 0.5f + 0.5f) * fw;
        sy = (0.5f - vy * iz / cam.tan_y * 0.5f) * fh;
    };
    size_t b = 0;
    while (b + 1 < batch_start.size() && batch_start[b + 1] <= t0) ++b;
    for (size_t t = t0; t < t1; ++t) {
        while (b + 1 < batch_start.size() && batch_start[b + 1] <= t) ++b;
        const CityBatch& batch = city.batches[b];
        if (pass.mirror && (batch.material == CityMaterial::Water || batch.material == CityMaterial::Waves)) continue;
        const uint32_t first = static_cast<uint32_t>((t - batch_start[b]) * 3);
        const CityVertex* cv = &batch.tris[first];
        float vx[3], vy[3], vz[3];
        int behind = 0;
        for (int k = 0; k < 3; ++k) {
            const Vec3 d = cv[k].pos - cam.pos;
            vx[k] = d.dot(cam.right);
            vy[k] = d.dot(cam.up);
            vz[k] = d.dot(cam.fwd);
            behind += vz[k] < kNear ? 1 : 0;
        }
        if (behind == 3) continue;
        const float zmin = std::min({cv[0].pos.z, cv[1].pos.z, cv[2].pos.z});
        const float zmax = std::max({cv[0].pos.z, cv[1].pos.z, cv[2].pos.z});
        if (pass.mirror && zmax < city.water_z) continue;

        ScreenTri st;
        st.batch = static_cast<uint32_t>(b);
        st.crosses_water = pass.mirror && zmin < city.water_z;
        if (behind == 0) {
            for (int k = 0; k < 3; ++k) {
                to_screen(vx[k], vy[k], vz[k], st.x[k], st.y[k], st.iz[k]);
                st.wz[k] = cv[k].pos.z;
            }
            st.first = first;
            st.clipped = false;
            st.ymin = std::min({st.y[0], st.y[1], st.y[2]});
            st.ymax = std::max({st.y[0], st.y[1], st.y[2]});
            if (st.ymax < 0.0f || st.ymin > fh) continue;
            if (std::max({st.x[0], st.x[1], st.x[2]}) < 0.0f || std::min({st.x[0], st.x[1], st.x[2]}) > fw) continue;
            out.tris.push_back(st);
            continue;
        }
        // Crosses the near plane: clip to it (at most a quad) and keep the corners' attributes.
        ViewVtx in[3];
        for (int k = 0; k < 3; ++k) {
            in[k].vx = vx[k];
            in[k].vy = vy[k];
            in[k].vz = vz[k];
            vertex_attrs(cv[k], in[k].a);
        }
        ViewVtx poly[4];
        int count = 0;
        for (int k = 0; k < 3; ++k) {
            const ViewVtx& p = in[k];
            const ViewVtx& q = in[(k + 1) % 3];
            const bool pin = p.vz >= kNear, qin = q.vz >= kNear;
            if (pin) poly[count++] = p;
            if (pin != qin) {
                const float s = (kNear - p.vz) / (q.vz - p.vz);
                ViewVtx c;
                c.vx = p.vx + (q.vx - p.vx) * s;
                c.vy = p.vy + (q.vy - p.vy) * s;
                c.vz = kNear;
                for (int j = 0; j < kAttrs; ++j) c.a[j] = p.a[j] + (q.a[j] - p.a[j]) * s;
                poly[count++] = c;
            }
        }
        for (int k = 1; k + 1 < count; ++k) {
            const ViewVtx* corner[3] = {&poly[0], &poly[k], &poly[k + 1]};
            ClippedAttrs ca;
            for (int c = 0; c < 3; ++c) {
                to_screen(corner[c]->vx, corner[c]->vy, corner[c]->vz, st.x[c], st.y[c], st.iz[c]);
                st.wz[c] = corner[c]->a[11];
                std::memcpy(ca.a[c], corner[c]->a, sizeof(float) * kAttrs);
            }
            st.first = static_cast<uint32_t>(out.clipped.size());
            st.clipped = true;
            st.ymin = std::min({st.y[0], st.y[1], st.y[2]});
            st.ymax = std::max({st.y[0], st.y[1], st.y[2]});
            out.clipped.push_back(ca);
            out.tris.push_back(st);
        }
    }
}

// Barycentric weights of pixel centre (px, py) in `t`; false when it is outside.
inline bool barycentric(const ScreenTri& t, float px, float py, float b[3]) {
    const float area = (t.x[1] - t.x[0]) * (t.y[2] - t.y[0]) - (t.y[1] - t.y[0]) * (t.x[2] - t.x[0]);
    if (area == 0.0f) return false;
    const float inv = 1.0f / area;
    b[0] = ((t.x[2] - t.x[1]) * (py - t.y[1]) - (t.y[2] - t.y[1]) * (px - t.x[1])) * inv;
    b[1] = ((t.x[0] - t.x[2]) * (py - t.y[2]) - (t.y[0] - t.y[2]) * (px - t.x[2])) * inv;
    b[2] = 1.0f - b[0] - b[1];
    return b[0] >= 0.0f && b[1] >= 0.0f && b[2] >= 0.0f;
}

// The attributes at a point of a triangle, perspective-correct.
void interpolate(const Pass& pass, const TriList& list, const ScreenTri& t, const float b[3], float a[kAttrs]) {
    const float w0 = b[0] * t.iz[0], w1 = b[1] * t.iz[1], w2 = b[2] * t.iz[2];
    const float z = 1.0f / (w0 + w1 + w2);
    if (t.clipped) {
        const ClippedAttrs& ca = list.clipped[t.first];
        for (int k = 0; k < kAttrs; ++k) a[k] = (w0 * ca.a[0][k] + w1 * ca.a[1][k] + w2 * ca.a[2][k]) * z;
        return;
    }
    const CityVertex* cv = &pass.city->batches[t.batch].tris[t.first];
    float a0[kAttrs], a1[kAttrs], a2[kAttrs];
    vertex_attrs(cv[0], a0);
    vertex_attrs(cv[1], a1);
    vertex_attrs(cv[2], a2);
    for (int k = 0; k < kAttrs; ++k) a[k] = (w0 * a0[k] + w1 * a1[k] + w2 * a2[k]) * z;
}

// ME_MENU_PROF=1 prints where a frame's time goes.
struct StageTimer {
    std::chrono::steady_clock::time_point t = std::chrono::steady_clock::now();
    bool on = std::getenv("ME_MENU_PROF") != nullptr;
    void mark(const char* what) {
        if (!on) return;
        const auto now = std::chrono::steady_clock::now();
        std::fprintf(stderr, "  %-22s %7.2f ms\n", what, std::chrono::duration<double, std::milli>(now - t).count());
        t = now;
    }
};

void draw_pass(const Pass& pass, TriList& list, std::vector<uint32_t>& visibility) {
    StageTimer timer;
    const City& city = *pass.city;
    Surface& target = *pass.target;
    const int w = target.w, h = target.h;

    // 1. Every triangle to the screen, in parallel over the flattened batch list.
    std::vector<size_t> batch_start(city.batches.size() + 1, 0);
    for (size_t b = 0; b < city.batches.size(); ++b) batch_start[b + 1] = batch_start[b] + city.batches[b].tris.size() / 3;
    const size_t total = batch_start.back();
    const int workers = worker_count();
    std::vector<TriList> parts(static_cast<size_t>(workers));
    parallel_rows(workers, [&](int p0, int p1) {
        for (int p = p0; p < p1; ++p) {
            build_tris(pass, batch_start, total * static_cast<size_t>(p) / workers, total * static_cast<size_t>(p + 1) / workers,
                       parts[static_cast<size_t>(p)]);
        }
    });
    list.tris.clear();
    list.clipped.clear();
    for (TriList& part : parts) {
        const uint32_t offset = static_cast<uint32_t>(list.clipped.size());
        for (ScreenTri& t : part.tris) {
            if (t.clipped) t.first += offset;
            list.tris.push_back(t);
        }
        list.clipped.insert(list.clipped.end(), part.clipped.begin(), part.clipped.end());
    }

    timer.mark(pass.mirror ? "mirror: triangles" : "main: triangles");
    // 2. Depth and triangle id.
    visibility.assign(static_cast<size_t>(w) * h, 0u);
    parallel_rows(h, [&](int row0, int row1) {
        for (size_t i = 0; i < list.tris.size(); ++i) {
            const ScreenTri& t = list.tris[i];
            if (city.batches[t.batch].material == CityMaterial::Waves) continue;
            if (t.ymax < static_cast<float>(row0) || t.ymin > static_cast<float>(row1)) continue;
            const int x0 = std::max(0, static_cast<int>(std::ceil(std::min({t.x[0], t.x[1], t.x[2]}) - 0.5f)));
            const int x1 = std::min(w - 1, static_cast<int>(std::floor(std::max({t.x[0], t.x[1], t.x[2]}) - 0.5f)));
            const int y0 = std::max(row0, static_cast<int>(std::ceil(t.ymin - 0.5f)));
            const int y1 = std::min(row1 - 1, static_cast<int>(std::floor(t.ymax - 0.5f)));
            if (x0 > x1 || y0 > y1) continue;
            const float area = (t.x[1] - t.x[0]) * (t.y[2] - t.y[0]) - (t.y[1] - t.y[0]) * (t.x[2] - t.x[0]);
            if (area == 0.0f) continue;
            const float inv = 1.0f / area;
            // b0 and b1 are linear across the screen: value at a pixel centre and the step per pixel in x.
            const float b0dx = -(t.y[2] - t.y[1]) * inv, b1dx = -(t.y[0] - t.y[2]) * inv;
            const float sx = static_cast<float>(x0) + 0.5f;
            for (int y = y0; y <= y1; ++y) {
                const float py = static_cast<float>(y) + 0.5f;
                float b0 = ((t.x[2] - t.x[1]) * (py - t.y[1]) - (t.y[2] - t.y[1]) * (sx - t.x[1])) * inv;
                float b1 = ((t.x[0] - t.x[2]) * (py - t.y[2]) - (t.y[0] - t.y[2]) * (sx - t.x[2])) * inv;
                float* depth_row = &target.depth[static_cast<size_t>(y) * w];
                uint32_t* vis_row = &visibility[static_cast<size_t>(y) * w];
                for (int x = x0; x <= x1; ++x, b0 += b0dx, b1 += b1dx) {
                    const float b2 = 1.0f - b0 - b1;
                    if (b0 < 0.0f || b1 < 0.0f || b2 < 0.0f) continue;
                    const float iz = b0 * t.iz[0] + b1 * t.iz[1] + b2 * t.iz[2];
                    if (iz <= depth_row[x]) continue;
                    if (t.crosses_water) {
                        // The reflection only shows what stands above the water.
                        const float wz = (b0 * t.iz[0] * t.wz[0] + b1 * t.iz[1] * t.wz[1] + b2 * t.iz[2] * t.wz[2]) / iz;
                        if (wz < city.water_z) continue;
                    }
                    depth_row[x] = iz;
                    vis_row[x] = static_cast<uint32_t>(i + 1);
                }
            }
        }
    });

    timer.mark(pass.mirror ? "mirror: depth" : "main: depth");
    // 3. One shade per pixel.
    parallel_rows(h, [&](int row0, int row1) {
        for (int y = row0; y < row1; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t pix = static_cast<size_t>(y) * w + x;
                const uint32_t id = visibility[pix];
                if (id == 0) continue;
                const ScreenTri& t = list.tris[id - 1];
                float b[3], a[kAttrs], c[3];
                barycentric(t, static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, b);
                interpolate(pass, list, t, b, a);
                if (!shade(pass, city.batches[t.batch], a, c)) continue;
                float* d = &target.rgb[pix * 3];
                d[0] = c[0];
                d[1] = c[1];
                d[2] = c[2];
            }
        }
    });

    timer.mark(pass.mirror ? "mirror: shade" : "main: shade");
    // 4. The additive water sparkle, against the finished depth.
    if (pass.mirror) return;
    parallel_rows(h, [&](int row0, int row1) {
        for (const ScreenTri& t : list.tris) {
            const CityBatch& batch = city.batches[t.batch];
            if (batch.material != CityMaterial::Waves) continue;
            if (t.ymax < static_cast<float>(row0) || t.ymin > static_cast<float>(row1)) continue;
            const int x0 = std::max(0, static_cast<int>(std::ceil(std::min({t.x[0], t.x[1], t.x[2]}) - 0.5f)));
            const int x1 = std::min(w - 1, static_cast<int>(std::floor(std::max({t.x[0], t.x[1], t.x[2]}) - 0.5f)));
            const int y0 = std::max(row0, static_cast<int>(std::ceil(t.ymin - 0.5f)));
            const int y1 = std::min(row1 - 1, static_cast<int>(std::floor(t.ymax - 0.5f)));
            for (int y = y0; y <= y1; ++y) {
                for (int x = x0; x <= x1; ++x) {
                    float b[3], a[kAttrs], c[3];
                    if (!barycentric(t, static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, b)) continue;
                    const size_t pix = static_cast<size_t>(y) * w + x;
                    if (b[0] * t.iz[0] + b[1] * t.iz[1] + b[2] * t.iz[2] < target.depth[pix]) continue;
                    interpolate(pass, list, t, b, a);
                    if (!shade(pass, batch, a, c)) continue;
                    float* d = &target.rgb[pix * 3];
                    d[0] += c[0];
                    d[1] += c[1];
                    d[2] += c[2];
                }
            }
        }
    });
    timer.mark("main: sparkle");
}

// The scene's exposure, measured on the main menu: with the level's bloom, the display gamma and the level's colour
// curve in place, this is the one number left that puts a retail frame's sky and buildings where
// they are. It holds within a few percent from mid-grey to white. The level asks for
// Scene_ExposureManual 0.83 inside an automatic range of 0.79 to 0.95; how the tone mapper gets
// from that to this is not established.
constexpr float kExposure = 0.52f;

// Bloom, exposure, gamma and the level's colour curve: linear scene colour to display values.
void tone_map(const City& city, const Surface& scene, float gamma, std::vector<float>& bloom, std::vector<float>& out) {
    const int w = scene.w, h = scene.h;
    // UE3 bloom: a wide blur of the quarter-resolution scene, scaled by Bloom_Scale and added.
    const int bw = std::max(1, w / 4), bh = std::max(1, h / 4);
    std::vector<float> small(static_cast<size_t>(bw) * bh * 3, 0.0f), tmp(small.size());
    parallel_rows(bh, [&](int brow0, int brow1) {
    for (int y = brow0; y < brow1; ++y) {
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
    });
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
        parallel_rows(bh, [&](int blur0, int blur1) {
        for (int y = blur0; y < blur1; ++y) {
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
        });
    }
    bloom.swap(small);

    // pow(v, 1 / gamma) from a table: three of them a pixel is most of this function otherwise.
    constexpr int kGammaSteps = 4096;
    static std::vector<float> gamma_table;
    static float gamma_table_for = 0.0f;
    if (gamma_table_for != gamma) {
        gamma_table.resize(kGammaSteps + 2);
        for (int i = 0; i <= kGammaSteps + 1; ++i) {
            gamma_table[static_cast<size_t>(i)] = std::pow(std::min(static_cast<float>(i) / kGammaSteps, 1.0f), 1.0f / gamma);
        }
        gamma_table_for = gamma;
    }

    out.resize(static_cast<size_t>(w) * h * 3);
    parallel_rows(h, [&](int row0, int row1) {
    for (int y = row0; y < row1; ++y) {
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
                const float v = std::clamp((s[c] + b * city.bloom_scale) * kExposure, 0.0f, 1.0f) * kGammaSteps;
                const int vi = static_cast<int>(v);
                const float g = gamma_table[static_cast<size_t>(vi)] +
                                (gamma_table[static_cast<size_t>(vi) + 1] - gamma_table[static_cast<size_t>(vi)]) * (v - static_cast<float>(vi));
                const int i = std::min(static_cast<int>(g * 15.0f), 15);
                o[c] = std::clamp(city.curve_m[i][c] * g + city.curve_b[i][c], 0.0f, 1.0f);
            }
        }
    }
    });
}

}  // namespace

struct CityRenderer::Impl {
    explicit Impl(const City& c) : city(c) {}
    const City& city;
    Surface scene;
    Surface reflection;
    std::vector<float> bloom;
    TriList tris;
    std::vector<uint32_t> visibility;
};

CityRenderer::CityRenderer(const City& city) : impl_(std::make_unique<Impl>(city)) {}
CityRenderer::~CityRenderer() = default;

const std::vector<float>& CityRenderer::linear() const { return impl_->scene.rgb; }

void CityRenderer::render(const Frame& frame, int w, int h, std::vector<float>& rgb) {
    const City& city = impl_->city;

    // SceneCaptureReflectActor: the scene from the camera mirrored in the water plane.
    const int rw = std::max(16, w / 2), rh = std::max(16, h / 2);
    StageTimer reset_timer;
    impl_->reflection.reset(rw, rh);
    Pass mirror;
    mirror.city = &city;
    mirror.mirror = true;
    mirror.time = static_cast<float>(frame.time);
    mirror.target = &impl_->reflection;
    mirror.selected = &frame.district_selected;
    const Vec3 mpos{frame.camera.x, frame.camera.y, 2.0f * city.water_z - frame.camera.z};
    const Vec3 mtarget{frame.target.x, frame.target.y, 2.0f * city.water_z - frame.target.z};
    mirror.cam = make_camera(mpos, mtarget, -frame.roll, frame.fov, rw, rh);
    draw_pass(mirror, impl_->tris, impl_->visibility);

    impl_->scene.reset(w, h);
    reset_timer.mark("(mirror pass + resets)");
    Pass main;
    main.city = &city;
    main.time = static_cast<float>(frame.time);
    main.target = &impl_->scene;
    main.selected = &frame.district_selected;
    main.cam = make_camera(frame.camera, frame.target, frame.roll, frame.fov, w, h);
    main.reflection = &impl_->reflection;
    main.reflection_cam = mirror.cam;
    draw_pass(main, impl_->tris, impl_->visibility);

    StageTimer timer;
    tone_map(city, impl_->scene, frame.display_gamma, impl_->bloom, rgb);
    timer.mark("tone map");
}

}  // namespace me::fe
