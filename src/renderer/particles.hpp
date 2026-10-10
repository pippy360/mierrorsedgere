#pragma once

// -----------------------------------------------------------------------------
// Particle systems (docs/RENDERING_RE.md, "Particles"): the level's sprite emitters,
// run and turned into quads as the game does it.
//
// An emitter instance, each frame (the engine's FParticleEmitterInstance::Tick):
//   1. the emitter's clock moves on, looping at EmitterDuration;
//   2. particles past the end of their life go;
//   3. new ones are spawned at the spawn module's rate (and its bursts), each run through
//      the modules that act at spawn, with the emitter's time as their input;
//   4. every particle's velocity, size, rotation rate and colour go back to their base
//      values, and its relative time moves on by dt / lifetime;
//   5. the modules that act every frame run, with the particle's relative time as input;
//   6. position and rotation are integrated.
// A value is read from its raw distribution's lookup table (FRawDistribution::GetEntry,
// 0x00d72100 in the game's executable): linear between entries; operation 2 picks
// uniformly between the entry's minimum and maximum with the engine's appSRand().
//
// A sprite is a quad around the particle (ParticleSpriteVertexFactory.usf). With R and U
// the world directions of screen right and up and a the particle's rotation:
//   square, rectangle   Right = cos(a) R - sin(a) U,  Up = -sin(a) R - cos(a) U
//   velocity            Right = normalize(cross(to the camera, travel)),  Up = -travel
//   corner (cx, cy)     P + Size.x (cx - 0.5) Right + Size.y (cy - 0.5) Up
// A square's height is its width. A sub-image emitter's two texture coordinate sets are
// its current and next image, with their blend.
//
// The vertices are ordinary scene vertices, drawn with the emitter's own translated
// material in the translucency pass. The particle's colour (linear, unclamped) rides where
// a mesh vertex has its light map's first coefficient, its alpha in lm_v and the sub-image
// blend in lm_u: a particle material reads its vertex colour from there
// (material_system.cpp).
// -----------------------------------------------------------------------------

#include "../assets/level_lightmaps.hpp"
#include "../math/types.hpp"
#include "lens_flare.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace me {

// The engine's appSRand(): one global sequence (0x01169647).
class ParticleRandom {
public:
    float next() {
        seed_ = seed_ * 196314165u + 907633515u;
        const uint32_t bits = 0x3F800000u | (seed_ & 0x007FFFFFu);
        float f;
        std::memcpy(&f, &bits, sizeof(f));
        return f - 1.0f;
    }
    void reset() { seed_ = 0; }

private:
    uint32_t seed_ = 0;
};

// A distribution's value: up to three components.
inline void particle_value(const RawDistribution& d, float x, ParticleRandom& rnd, float out[3]) {
    out[0] = out[1] = out[2] = 0.0f;
    if (d.op <= 0 || d.op > 3) return;
    const int chunk = d.chunk;
    const int last = static_cast<int>(d.table.size()) - chunk;
    if (last < 2) return;
    float t = (x - d.start_time) * d.time_scale;
    if (!(t >= 0.0f)) t = 0.0f;
    const float top = static_cast<float>((last - 2) / chunk);
    int i = static_cast<int>(top);
    float alpha = 0.0f;
    if (t < top) {
        i = static_cast<int>(t);
        alpha = t - static_cast<float>(i);
    }
    const float* e1 = d.table.data() + std::min(2 + chunk * i, last);
    const float* e2 = d.table.data() + std::min(2 + chunk * i + chunk, last);
    const auto lerp = [&](int c) { return e1[c] + (e2[c] - e1[c]) * alpha; };
    if (d.op == 1) {
        for (int c = 0; c < std::min(chunk, 3); ++c) out[c] = lerp(c);
        return;
    }
    // The entry holds the minimum of every component, then the maximum.
    const int n = std::min(chunk / 2, 3);
    for (int c = 0; c < n; ++c) {
        const float lo = lerp(c);
        const float hi = lerp(n + c);
        const float r = rnd.next();
        out[c] = d.op == 2 ? lo + (hi - lo) * r : (r > 0.5f ? hi : lo);
    }
}
inline float particle_float(const RawDistribution& d, float x, ParticleRandom& rnd) {
    float v[3];
    particle_value(d, x, rnd, v);
    return v[0];
}
inline Vec3 particle_vector(const RawDistribution& d, float x, ParticleRandom& rnd) {
    float v[3];
    particle_value(d, x, rnd, v);
    return Vec3(v[0], v[1], v[2]);
}

struct Particle {
    Vec3 old_location{0.0f, 0.0f, 0.0f};
    Vec3 location{0.0f, 0.0f, 0.0f};
    Vec3 base_velocity{0.0f, 0.0f, 0.0f};
    Vec3 velocity{0.0f, 0.0f, 0.0f};
    Vec3 base_size{0.0f, 0.0f, 0.0f};
    Vec3 size{0.0f, 0.0f, 0.0f};
    Vec3 acceleration{0.0f, 0.0f, 0.0f};  // the Acceleration module's pick
    float relative_time = 0.0f;
    float one_over_max_lifetime = 0.0f;
    float rotation = 0.0f;
    float base_rotation_rate = 0.0f;
    float rotation_rate = 0.0f;
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float base_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float sub_image = 0.0f;  // image index and, in its fraction, the blend to the next
};

// One emitter of a placed system.
class ParticleEmitterInstance {
public:
    std::vector<Particle> particles;

    void tick(const ParticleEmitterInfo& e, const ParticleSystemPlacement& at, float dt, ParticleRandom& rnd) {
        if (dt <= 0.0f) return;
        // 1. The emitter's clock.
        seconds_ += dt;
        float emitter_time = seconds_;
        int loop = 0;
        if (e.duration > 0.0f) {
            loop = static_cast<int>(seconds_ / e.duration);
            emitter_time = seconds_ - static_cast<float>(loop) * e.duration;
        }
        if (loop != loop_) {
            loop_ = loop;
            bursts_fired_.assign(e.bursts.size(), 0);
        }
        if (bursts_fired_.size() != e.bursts.size()) bursts_fired_.assign(e.bursts.size(), 0);
        if (!e.delay_first_loop_only || loop == 0) emitter_time -= e.delay;

        // 2. The dead go.
        for (size_t i = 0; i < particles.size();) {
            if (particles[i].relative_time > 1.0f) {
                particles[i] = particles.back();
                particles.pop_back();
            } else {
                ++i;
            }
        }

        // 3. Spawn.
        const bool spawning = emitter_time >= 0.0f && (e.loops == 0 || loop < e.loops);
        if (spawning) {
            float rate = 0.0f;
            if (e.process_rate) rate = particle_float(e.rate, emitter_time, rnd) * particle_float(e.rate_scale, emitter_time, rnd);
            int number = 0;
            float start_time = dt;
            float increment = 0.0f;
            if (rate > 0.0f) {
                const float fraction = leftover_ + dt * rate;
                number = static_cast<int>(fraction);
                increment = 1.0f / rate;
                start_time = dt + leftover_ * increment - increment;
                leftover_ = fraction - static_cast<float>(number);
            }
            int burst = 0;
            if (e.process_bursts) {
                for (size_t b = 0; b < e.bursts.size(); ++b) {
                    if (bursts_fired_[b] || emitter_time < e.bursts[b].time * std::max(e.duration, 0.0f)) continue;
                    bursts_fired_[b] = 1;
                    float count = e.bursts[b].count;
                    if (e.bursts[b].count_low >= 0.0f) count = e.bursts[b].count_low + (count - e.bursts[b].count_low) * rnd.next();
                    burst += static_cast<int>(count);
                }
            }
            const int limit = e.max_draw_count > 0 ? e.max_draw_count : 4096;
            for (int i = 0; i < number + burst && static_cast<int>(particles.size()) < limit; ++i) {
                const float spawn_time = i < number ? start_time - static_cast<float>(i) * increment : 0.0f;
                spawn(e, at, emitter_time, std::max(spawn_time, 0.0f), rnd);
            }
        }

        // 4. Back to the base values; time moves on.
        for (Particle& p : particles) {
            p.velocity = p.base_velocity;
            p.size = p.base_size;
            p.rotation_rate = p.base_rotation_rate;
            std::memcpy(p.color, p.base_color, sizeof(p.color));
            p.relative_time += p.one_over_max_lifetime * dt;
        }

        // 5. The modules that act every frame.
        for (const ParticleModuleInfo& m : e.modules) {
            switch (m.kind) {
                case ParticleModuleInfo::Kind::ColorOverLife:
                    for (Particle& p : particles) color_over_life(m, p, rnd);
                    break;
                case ParticleModuleInfo::Kind::SizeMultiplyLife:
                    for (Particle& p : particles) size_multiply(m, p, rnd);
                    break;
                case ParticleModuleInfo::Kind::SubUV:
                    if (e.interpolation == 1 || e.interpolation == 2) {
                        for (Particle& p : particles) p.sub_image = particle_float(m.a, p.relative_time, rnd);
                    }
                    break;
                case ParticleModuleInfo::Kind::AccelerationOverLifetime:
                    for (Particle& p : particles) {
                        Vec3 a = particle_vector(m.a, p.relative_time, rnd);
                        if (!e.local_space && !m.flag[0]) a = to_world(at, a);
                        p.velocity = p.velocity + a * dt;
                        p.base_velocity = p.base_velocity + a * dt;
                    }
                    break;
                case ParticleModuleInfo::Kind::Acceleration:
                    for (Particle& p : particles) {
                        p.velocity = p.velocity + p.acceleration * dt;
                        p.base_velocity = p.base_velocity + p.acceleration * dt;
                    }
                    break;
                case ParticleModuleInfo::Kind::VelocityOverLifetime:
                    for (Particle& p : particles) {
                        const Vec3 v = particle_vector(m.a, p.relative_time, rnd);
                        p.velocity = m.flag[0] ? v : Vec3(p.velocity.x * v.x, p.velocity.y * v.y, p.velocity.z * v.z);
                    }
                    break;
                case ParticleModuleInfo::Kind::RotationRateMultiplyLife:
                    for (Particle& p : particles) p.rotation_rate *= particle_float(m.a, p.relative_time, rnd);
                    break;
                default:
                    break;
            }
        }

        // 6. Integrate.
        for (Particle& p : particles) {
            p.old_location = p.location;
            p.location = p.location + p.velocity * dt;
            p.rotation = std::fmod(p.rotation + dt * p.rotation_rate, 6.28318531f);
        }
    }

private:
    // A vector of the emitter's own frame, in the world's (no translation).
    static Vec3 to_world(const ParticleSystemPlacement& at, const Vec3& v) {
        return at.axis_x * (v.x * at.scale.x) + at.axis_y * (v.y * at.scale.y) + at.axis_z * (v.z * at.scale.z);
    }

    static void color_over_life(const ParticleModuleInfo& m, Particle& p, ParticleRandom& rnd) {
        const Vec3 c = particle_vector(m.a, p.relative_time, rnd);
        float a = particle_float(m.b, p.relative_time, rnd);
        if (m.flag[0]) a = std::clamp(a, 0.0f, 1.0f);
        p.color[0] = c.x;
        p.color[1] = c.y;
        p.color[2] = c.z;
        p.color[3] = a;
    }
    static void size_multiply(const ParticleModuleInfo& m, Particle& p, ParticleRandom& rnd) {
        const Vec3 k = particle_vector(m.a, p.relative_time, rnd);
        if (m.flag[0]) p.size.x *= k.x;
        if (m.flag[1]) p.size.y *= k.y;
        if (m.flag[2]) p.size.z *= k.z;
    }

    void spawn(const ParticleEmitterInfo& e, const ParticleSystemPlacement& at, float emitter_time, float spawn_time,
               ParticleRandom& rnd) {
        Particle p;
        const Vec3 origin = e.local_space ? Vec3(0.0f, 0.0f, 0.0f) : at.location;
        p.location = origin;
        for (const ParticleModuleInfo& m : e.modules) {
            switch (m.kind) {
                case ParticleModuleInfo::Kind::Lifetime: {
                    // ParticleModuleLifetime::Spawn (0x00d70340). A lifetime of 0: it never ages.
                    const float life = particle_float(m.a, emitter_time, rnd);
                    if (p.one_over_max_lifetime > 0.0f) {
                        p.one_over_max_lifetime = 1.0f / (life + 1.0f / p.one_over_max_lifetime);
                    } else {
                        p.one_over_max_lifetime = life > 0.0f ? 1.0f / life : 0.0f;
                    }
                    p.relative_time = spawn_time * p.one_over_max_lifetime;
                    break;
                }
                case ParticleModuleInfo::Kind::Size: {
                    const Vec3 v = particle_vector(m.a, emitter_time, rnd);
                    p.size = p.size + v;
                    p.base_size = p.base_size + v;
                    break;
                }
                case ParticleModuleInfo::Kind::Velocity: {
                    Vec3 v = particle_vector(m.a, emitter_time, rnd);
                    if (!e.local_space && !m.flag[0]) v = to_world(at, v);
                    const Vec3 out = p.location - origin;
                    if (out.length_sq() > 1.0e-8f) v = v + out.normalized() * particle_float(m.b, emitter_time, rnd);
                    p.velocity = p.velocity + v;
                    p.base_velocity = p.base_velocity + v;
                    break;
                }
                case ParticleModuleInfo::Kind::Rotation:
                    p.rotation += 6.28318531f * particle_float(m.a, emitter_time, rnd);
                    break;
                case ParticleModuleInfo::Kind::RotationRate: {
                    const float r = 6.28318531f * particle_float(m.a, emitter_time, rnd);
                    p.rotation_rate += r;
                    p.base_rotation_rate += r;
                    break;
                }
                case ParticleModuleInfo::Kind::Color: {
                    const Vec3 c = particle_vector(m.a, emitter_time, rnd);
                    float a = particle_float(m.b, emitter_time, rnd);
                    if (m.flag[0]) a = std::clamp(a, 0.0f, 1.0f);
                    p.color[0] = p.base_color[0] = c.x;
                    p.color[1] = p.base_color[1] = c.y;
                    p.color[2] = p.base_color[2] = c.z;
                    p.color[3] = p.base_color[3] = a;
                    break;
                }
                case ParticleModuleInfo::Kind::ColorOverLife:
                    color_over_life(m, p, rnd);
                    std::memcpy(p.base_color, p.color, sizeof(p.color));
                    break;
                case ParticleModuleInfo::Kind::SizeMultiplyLife:
                    size_multiply(m, p, rnd);
                    break;
                case ParticleModuleInfo::Kind::SubUV:
                    if (e.interpolation == 1 || e.interpolation == 2) p.sub_image = particle_float(m.a, p.relative_time, rnd);
                    break;
                case ParticleModuleInfo::Kind::Acceleration: {
                    Vec3 a = particle_vector(m.a, emitter_time, rnd);
                    if (!e.local_space) a = to_world(at, a);
                    p.acceleration = a;
                    p.velocity = p.velocity + a * spawn_time;
                    p.base_velocity = p.base_velocity + a * spawn_time;
                    break;
                }
                case ParticleModuleInfo::Kind::Location: {
                    const Vec3 v = particle_vector(m.a, emitter_time, rnd);
                    p.location = p.location + (e.local_space ? v : to_world(at, v));
                    break;
                }
                case ParticleModuleInfo::Kind::RotationRateMultiplyLife:
                    p.rotation_rate *= particle_float(m.a, p.relative_time, rnd);
                    break;
                case ParticleModuleInfo::Kind::LocationSphere:
                case ParticleModuleInfo::Kind::LocationCylinder: {
                    // A point in (or on) the primitive, around its StartLocation.
                    const float radius = particle_float(m.a, emitter_time, rnd);
                    const Vec3 centre = particle_vector(m.b, emitter_time, rnd);
                    Vec3 dir(rnd.next() * 2.0f - 1.0f, rnd.next() * 2.0f - 1.0f, rnd.next() * 2.0f - 1.0f);
                    Vec3 offset;
                    if (m.kind == ParticleModuleInfo::Kind::LocationSphere) {
                        dir = dir.length_sq() > 1.0e-8f ? dir.normalized() : Vec3(0.0f, 0.0f, 1.0f);
                        offset = dir * (m.flag[0] ? radius : radius * rnd.next());
                    } else {
                        const float height = particle_float(m.c, emitter_time, rnd);
                        Vec3 flat(dir.x, dir.y, 0.0f);
                        flat = flat.length_sq() > 1.0e-8f ? flat.normalized() : Vec3(1.0f, 0.0f, 0.0f);
                        offset = flat * (m.flag[0] ? radius : radius * rnd.next()) + Vec3(0.0f, 0.0f, dir.z * height * 0.5f);
                    }
                    const Vec3 local = centre + offset;
                    p.location = p.location + (e.local_space ? local : to_world(at, local));
                    break;
                }
                default:
                    break;
            }
        }
        if (e.interpolation == 3 || e.interpolation == 4) {
            p.sub_image = std::floor(rnd.next() * static_cast<float>(e.sub_images_h * e.sub_images_v));
        }
        p.old_location = p.location;
        p.location = p.location + p.velocity * spawn_time;
        particles.push_back(p);
    }

    float seconds_ = 0.0f;
    float leftover_ = 0.0f;
    int loop_ = 0;
    std::vector<uint8_t> bursts_fired_;
};

// What a renderer draws: one emitter's sprites, with its material.
struct ParticleBatch {
    int32_t material = -1;
    float distance = 0.0f;  // of its system from the view: the far ones are drawn first
    std::vector<Vertex> vertices;
};

// The scene's particle systems, kept between frames.
class ParticleWorld {
public:
    // Runs the systems near the view up to `now` (seconds) and builds their sprites for it.
    void update(const LevelScene& scene, const LensFlareView& view, float now) {
        batches_.clear();
        static const bool off = std::getenv("ME_NO_PARTICLES") != nullptr;  // to see a picture without them
        if (off) return;
        if (map_ != scene.map_name || systems_.size() != scene.particle_systems.size()) {
            map_ = scene.map_name;
            systems_.assign(scene.particle_systems.size(), System{});
            random_.reset();
            last_time_ = now;
        }
        float dt = now - last_time_;
        if (!(dt >= 0.0f) || dt > 1.0f) dt = 0.0f;  // the clock ran back, or jumped: stand still this frame
        last_time_ = now;

        for (size_t i = 0; i < systems_.size(); ++i) {
            const ParticleSystemPlacement& at = scene.particle_systems[i];
            System& system = systems_[i];
            const float distance = (at.location - view.position).length();
            // The game stops drawing a system past its last LOD distance; here one out of range rests.
            if (!at.active || at.template_index < 0 || distance > kRange) {
                if (system.running) system = System{};
                continue;
            }
            const ParticleSystemTemplate& t = scene.particle_templates[static_cast<size_t>(at.template_index)];
            if (!system.running) {
                system.running = true;
                system.emitters.assign(t.emitters.size(), ParticleEmitterInstance{});
                // WarmupTime: the system has already run that long when it is first seen.
                const int steps = static_cast<int>(std::min(t.warmup_time, 30.0f) / kWarmupStep);
                for (int s = 0; s < steps; ++s) {
                    for (size_t e = 0; e < t.emitters.size(); ++e) system.emitters[e].tick(t.emitters[e], at, kWarmupStep, random_);
                }
            } else {
                for (size_t e = 0; e < t.emitters.size(); ++e) system.emitters[e].tick(t.emitters[e], at, dt, random_);
            }
            for (size_t e = 0; e < t.emitters.size(); ++e) build(t.emitters[e], at, system.emitters[e], view, distance);
        }
        std::stable_sort(batches_.begin(), batches_.end(),
                         [](const ParticleBatch& a, const ParticleBatch& b) { return a.distance > b.distance; });
    }

    // A frame with no particles in it (a menu, a film).
    void rest() { batches_.clear(); }

    [[nodiscard]] const std::vector<ParticleBatch>& batches() const { return batches_; }

private:
    static constexpr float kRange = 20000.0f;         // uu from the view
    static constexpr float kWarmupStep = 1.0f / 30.0f;

    struct System {
        bool running = false;
        std::vector<ParticleEmitterInstance> emitters;
    };

    void build(const ParticleEmitterInfo& e, const ParticleSystemPlacement& at, const ParticleEmitterInstance& instance,
               const LensFlareView& view, float distance) {
        if (instance.particles.empty() || e.material < 0) return;
        ParticleBatch batch;
        batch.material = e.material;
        batch.distance = distance;
        batch.vertices.reserve(instance.particles.size() * 6);
        const auto world = [&](const Vec3& p) {
            if (!e.local_space) return p;
            return at.location + at.axis_x * (p.x * at.scale.x) + at.axis_y * (p.y * at.scale.y) + at.axis_z * (p.z * at.scale.z);
        };
        // Back to front within the emitter.
        order_.resize(instance.particles.size());
        for (size_t i = 0; i < order_.size(); ++i) {
            order_[i] = {(world(instance.particles[i].location) - view.position).dot(view.forward), i};
        }
        std::sort(order_.begin(), order_.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

        const int images = e.sub_images_h * e.sub_images_v;
        for (const auto& [depth, index] : order_) {
            const Particle& p = instance.particles[index];
            const Vec3 centre = world(p.location);
            float size_x = p.size.x * at.scale.x;
            float size_y = (e.screen_alignment == 0 ? p.size.x : p.size.y) * (e.screen_alignment == 0 ? at.scale.x : at.scale.y);
            Vec3 right, up;
            if (e.screen_alignment == 2) {
                const auto safe = [](const Vec3& v) { return v * (1.0f / std::sqrt(std::max(v.length_sq(), 0.01f))); };
                const Vec3 travel = safe(centre - world(p.old_location));
                right = safe(safe(view.position - centre).cross(travel));
                up = travel * -1.0f;
            } else {
                const float cs = std::cos(p.rotation), sn = std::sin(p.rotation);
                right = view.right * cs - view.up * sn;
                up = view.right * -sn - view.up * cs;
            }
            // The two sub-images and their blend.
            float uv[2][4] = {{0.0f, 0.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 1.0f, 1.0f}};  // u0, v0, u scale, v scale
            float blend = 0.0f;
            if (images > 1) {
                int first = std::clamp(static_cast<int>(p.sub_image), 0, images - 1);
                if (e.interpolation == 2 || e.interpolation == 4) blend = std::abs(p.sub_image - static_cast<float>(first));
                const int second = (first + 1) % images;
                const int pick[2] = {first, second};
                for (int k = 0; k < 2; ++k) {
                    uv[k][0] = static_cast<float>(pick[k] % e.sub_images_h) / static_cast<float>(e.sub_images_h);
                    uv[k][1] = static_cast<float>(pick[k] / e.sub_images_h) / static_cast<float>(e.sub_images_v);
                    uv[k][2] = 1.0f / static_cast<float>(e.sub_images_h);
                    uv[k][3] = 1.0f / static_cast<float>(e.sub_images_v);
                }
            }
            const uint32_t color = pack_rgb9e5(Vec3(std::max(p.color[0], 0.0f), std::max(p.color[1], 0.0f), std::max(p.color[2], 0.0f)));
            static const float kCorner[4][2] = {{0.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}};
            static const int kOrder[6] = {0, 1, 2, 0, 2, 3};
            const Vec3 normal = view.forward * -1.0f;
            for (int k = 0; k < 6; ++k) {
                const float cx = kCorner[kOrder[k]][0], cy = kCorner[kOrder[k]][1];
                Vertex v;
                v.position = centre + right * (size_x * (cx - 0.5f)) + up * (size_y * (cy - 0.5f));
                v.normal = normal;
                v.tangent = view.right;
                v.u = uv[0][0] + cx * uv[0][2];
                v.v = uv[0][1] + cy * uv[0][3];
                v.u2 = uv[1][0] + cx * uv[1][2];
                v.v2 = uv[1][1] + cy * uv[1][3];
                v.lm_u = blend;
                v.lm_v = p.color[3];
                v.lm0 = color;
                batch.vertices.push_back(v);
            }
        }
        batches_.push_back(std::move(batch));
    }

    std::string map_;
    std::vector<System> systems_;
    std::vector<ParticleBatch> batches_;
    std::vector<std::pair<float, size_t>> order_;
    ParticleRandom random_;
    float last_time_ = 0.0f;
};

}  // namespace me
