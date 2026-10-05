#include "audio_engine.hpp"
#include "../assets/upk_loader.hpp"
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <algorithm>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

#ifndef ME_NO_OPENAL
#include <OpenAL/al.h>
#include <OpenAL/alc.h>
#endif

#if defined(__has_include)
#if __has_include(<vorbis/vorbisfile.h>)
#include <vorbis/vorbisfile.h>
#define ME_HAS_VORBIS 1
#endif
#endif

namespace me {

namespace {

constexpr float kPi = 3.14159265358979323846f;

inline float rand_normalized() {
    return static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX);
}

// Memory read callbacks for libvorbisfile
#ifdef ME_HAS_VORBIS
struct MemOgg {
    const uint8_t* ptr;
    size_t size;
    size_t offset;
};

size_t mem_read(void* ptr, size_t size, size_t nmemb, void* datasource) {
    auto* mem = static_cast<MemOgg*>(datasource);
    size_t total = size * nmemb;
    size_t avail = (mem->offset < mem->size) ? (mem->size - mem->offset) : 0;
    size_t to_read = std::min(total, avail);
    if (to_read > 0) {
        std::memcpy(ptr, mem->ptr + mem->offset, to_read);
        mem->offset += to_read;
    }
    return to_read / size;
}

int mem_seek(void* datasource, ogg_int64_t offset, int whence) {
    auto* mem = static_cast<MemOgg*>(datasource);
    ogg_int64_t target = 0;
    if (whence == SEEK_SET) target = offset;
    else if (whence == SEEK_CUR) target = static_cast<ogg_int64_t>(mem->offset) + offset;
    else if (whence == SEEK_END) target = static_cast<ogg_int64_t>(mem->size) + offset;
    if (target < 0 || target > static_cast<ogg_int64_t>(mem->size)) return -1;
    mem->offset = static_cast<size_t>(target);
    return 0;
}

long mem_tell(void* datasource) {
    auto* mem = static_cast<MemOgg*>(datasource);
    return static_cast<long>(mem->offset);
}

int mem_close(void* datasource) {
    return 0;
}
#endif

// Convert a vector of float audio samples [-1.0, 1.0] to interleaved stereo 16-bit PCM bytes
std::vector<uint8_t> float_stereo_to_pcm16(const std::vector<float>& left, const std::vector<float>& right) {
    size_t n = std::min(left.size(), right.size());
    std::vector<uint8_t> bytes(n * 4);
    int16_t* out = reinterpret_cast<int16_t*>(bytes.data());
    for (size_t i = 0; i < n; ++i) {
        float l = std::clamp(left[i], -1.0f, 1.0f);
        float r = std::clamp(right[i], -1.0f, 1.0f);
        out[i * 2 + 0] = static_cast<int16_t>(l * 32767.0f);
        out[i * 2 + 1] = static_cast<int16_t>(r * 32767.0f);
    }
    return bytes;
}

} // namespace

AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine() {
    shutdown();
}

bool AudioEngine::init(bool headless) {
    headless_ = headless;
    initialized_ = true;

    // Synthesize high-quality procedural fallback clips and 4 dynamic Solar Fields stems
    synthesize_fallback_clips();

    if (!headless_) {
        init_openal();
    }

    return true;
}

void AudioEngine::shutdown() {
    if (!initialized_) return;
    cleanup_openal();
    sound_clips_.clear();
    al_buffers_.clear();
    initialized_ = false;
}

bool AudioEngine::init_openal() {
#ifndef ME_NO_OPENAL
    ALCdevice* dev = alcOpenDevice(nullptr);
    if (!dev) {
        headless_ = true;
        return false;
    }
    alc_device_ = dev;

    ALCcontext* ctx = alcCreateContext(dev, nullptr);
    if (!ctx) {
        alcCloseDevice(dev);
        alc_device_ = nullptr;
        headless_ = true;
        return false;
    }
    alc_context_ = ctx;
    alcMakeContextCurrent(ctx);

    // Create 32 spatial sources
    for (size_t i = 0; i < kSourcePoolSize; ++i) {
        ALuint src = 0;
        alGenSources(1, &src);
        sources_[i] = src;
    }

    // Create 4 dynamic music stem sources
    for (int i = 0; i < 4; ++i) {
        ALuint src = 0;
        alGenSources(1, &src);
        music_stem_sources_[i] = src;
        alSourcei(src, AL_LOOPING, AL_TRUE);
        alSourcef(src, AL_GAIN, (i == 0) ? 1.0f : 0.0f);

        // Preload stem buffers
        std::string stem_name = "Stem_" + std::to_string(i);
        if (sound_clips_.find(stem_name) != sound_clips_.end()) {
            uint32_t buf = get_or_create_buffer(sound_clips_[stem_name]);
            music_stem_buffers_[i] = buf;
            alSourcei(src, AL_BUFFER, static_cast<ALint>(buf));
            alSourcePlay(src);
        }
    }

    return true;
#else
    headless_ = true;
    return false;
#endif
}

void AudioEngine::cleanup_openal() {
#ifndef ME_NO_OPENAL
    if (alc_context_) {
        // Stop music stems
        for (int i = 0; i < 4; ++i) {
            if (music_stem_sources_[i]) {
                alSourceStop(music_stem_sources_[i]);
                alDeleteSources(1, &music_stem_sources_[i]);
                music_stem_sources_[i] = 0;
            }
        }

        // Delete source pool
        for (size_t i = 0; i < kSourcePoolSize; ++i) {
            if (sources_[i]) {
                alSourceStop(sources_[i]);
                alDeleteSources(1, &sources_[i]);
                sources_[i] = 0;
            }
        }

        // Delete buffers
        for (auto& [_, buf_id] : al_buffers_) {
            if (buf_id) {
                ALuint b = buf_id;
                alDeleteBuffers(1, &b);
            }
        }
        al_buffers_.clear();

        alcMakeContextCurrent(nullptr);
        alcDestroyContext(static_cast<ALCcontext*>(alc_context_));
        alc_context_ = nullptr;
    }

    if (alc_device_) {
        alcCloseDevice(static_cast<ALCdevice*>(alc_device_));
        alc_device_ = nullptr;
    }
#endif
}

uint32_t AudioEngine::acquire_source() {
#ifndef ME_NO_OPENAL
    if (headless_ || !alc_context_) return 0;
    // Check if any source is stopped
    for (size_t i = 0; i < kSourcePoolSize; ++i) {
        ALint state = 0;
        alGetSourcei(sources_[i], AL_SOURCE_STATE, &state);
        if (state != AL_PLAYING && state != AL_PAUSED) {
            return sources_[i];
        }
    }
    // Round-robin steal
    uint32_t s = sources_[next_source_];
    next_source_ = (next_source_ + 1) % kSourcePoolSize;
    alSourceStop(s);
    return s;
#else
    return 0;
#endif
}

uint32_t AudioEngine::get_or_create_buffer(const SoundClip& clip) {
#ifndef ME_NO_OPENAL
    if (headless_ || !alc_context_) return 0;

    auto it = al_buffers_.find(clip.name);
    if (it != al_buffers_.end()) {
        return it->second;
    }

    if (clip.pcm_data.empty()) {
        return 0;
    }

    ALuint buf = 0;
    alGenBuffers(1, &buf);

    ALenum format = (clip.channels == 2) ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16;
    alBufferData(buf, format, clip.pcm_data.data(), static_cast<ALsizei>(clip.pcm_data.size()), clip.sample_rate);

    al_buffers_[clip.name] = buf;
    return buf;
#else
    return 0;
#endif
}

bool AudioEngine::decode_ogg_to_pcm(const uint8_t* ogg_data, size_t ogg_size,
                                    std::vector<int16_t>& out_pcm, int& out_rate, int& out_channels) {
#ifdef ME_HAS_VORBIS
    if (!ogg_data || ogg_size == 0) return false;

    MemOgg mem{ogg_data, ogg_size, 0};
    ov_callbacks cb{mem_read, mem_seek, mem_close, mem_tell};
    OggVorbis_File vf;
    if (ov_open_callbacks(&mem, &vf, nullptr, 0, cb) != 0) {
        return false;
    }

    vorbis_info* vi = ov_info(&vf, -1);
    if (!vi) {
        ov_clear(&vf);
        return false;
    }

    out_rate = vi->rate;
    out_channels = vi->channels;

    char pcm_buf[4096];
    int current_section = 0;
    long read_bytes = 0;
    while ((read_bytes = ov_read(&vf, pcm_buf, sizeof(pcm_buf), 0, 2, 1, &current_section)) > 0) {
        size_t samples = read_bytes / sizeof(int16_t);
        const int16_t* s_ptr = reinterpret_cast<const int16_t*>(pcm_buf);
        out_pcm.insert(out_pcm.end(), s_ptr, s_ptr + samples);
    }

    ov_clear(&vf);
    return !out_pcm.empty();
#else
    (void)ogg_data; (void)ogg_size; (void)out_pcm; (void)out_rate; (void)out_channels;
    return false;
#endif
}

void AudioEngine::update(float dt,
                         const Vec3& listener_pos,
                         const Vec3& listener_forward,
                         const Vec3& listener_up,
                         float player_speed,
                         bool reaction_active) {
#ifndef ME_NO_OPENAL
    if (headless_ || !alc_context_) return;

    // 1. Update listener orientation
    ALfloat pos[3] = {listener_pos.x * 0.01f, listener_pos.y * 0.01f, listener_pos.z * 0.01f};
    alListenerfv(AL_POSITION, pos);

    Vec3 f = listener_forward.normalized();
    Vec3 u = listener_up.normalized();
    ALfloat ori[6] = {f.x, f.y, f.z, u.x, u.y, u.z};
    alListenerfv(AL_ORIENTATION, ori);

    // 2. Evaluate target stem volumes based on gameplay state
    if (reaction_active) {
        target_stem_vols_[0] = 0.2f; // Ambient attenuated
        target_stem_vols_[1] = 0.2f; // Tension attenuated
        target_stem_vols_[2] = 0.1f; // Chase attenuated
        target_stem_vols_[3] = 1.0f; // Reaction stem max
    } else if (player_speed > 300.0f) {
        target_stem_vols_[0] = 0.8f; // Ambient active
        target_stem_vols_[1] = 0.5f; // Tension pulse
        target_stem_vols_[2] = 1.0f; // Full Chase breakbeat!
        target_stem_vols_[3] = 0.0f;
    } else {
        target_stem_vols_[0] = 1.0f; // Ambient full
        target_stem_vols_[1] = 0.1f; // Subtle tension
        target_stem_vols_[2] = 0.0f;
        target_stem_vols_[3] = 0.0f;
    }

    // Smooth lerp on stem gains
    float lerp_factor = std::clamp(dt * 3.0f, 0.0f, 1.0f);
    for (int i = 0; i < 4; ++i) {
        current_stem_vols_[i] += (target_stem_vols_[i] - current_stem_vols_[i]) * lerp_factor;
        if (music_stem_sources_[i]) {
            alSourcef(music_stem_sources_[i], AL_GAIN, current_stem_vols_[i]);
        }
    }
#else
    (void)dt; (void)listener_pos; (void)listener_forward; (void)listener_up;
    (void)player_speed; (void)reaction_active;
#endif
}

bool AudioEngine::load_sound_bank(const std::string& game_root, const std::string& bank_name) {
    namespace fs = std::filesystem;
    fs::path base(game_root);
    fs::path bank_path = base / "TdGame" / "CookedPC" / "Audio" / bank_name;
    if (!fs::exists(bank_path)) {
        bank_path = base / "Audio" / bank_name;
    }
    if (!fs::exists(bank_path)) {
        return false;
    }

    UPKPackage pkg(bank_path.string());
    if (!pkg.is_valid()) {
        return false;
    }

    auto clips = pkg.extract_audio();
    for (auto& clip : clips) {
        // If raw Ogg Vorbis stream is present, decode to PCM
        if (clip.pcm_data.size() >= 4 &&
            clip.pcm_data[0] == 'O' && clip.pcm_data[1] == 'g' &&
            clip.pcm_data[2] == 'g' && clip.pcm_data[3] == 'S') {
            std::vector<int16_t> pcm;
            int rate = 44100;
            int channels = 2;
            if (decode_ogg_to_pcm(clip.pcm_data.data(), clip.pcm_data.size(), pcm, rate, channels)) {
                clip.pcm_data.resize(pcm.size() * sizeof(int16_t));
                std::memcpy(clip.pcm_data.data(), pcm.data(), clip.pcm_data.size());
                clip.sample_rate = rate;
                clip.channels = channels;
                clip.duration = static_cast<float>(pcm.size()) / (static_cast<float>(rate) * static_cast<float>(channels));
            }
        }
        sound_clips_[clip.name] = clip;
    }

    return !clips.empty();
}

bool AudioEngine::load_stock_audio(const std::string& game_root) {
    bool any_loaded = false;
    static const char* kStockBanks[] = {
        "A_Bodyfalls.upk",
        "A_Material_Footstep.upk",
        "A_Character_Female_01.upk",
        "A_HUD.upk",
        "A_Music.upk"
    };

    for (const char* bank : kStockBanks) {
        if (load_sound_bank(game_root, bank)) {
            any_loaded = true;
        }
    }
    return any_loaded;
}

void AudioEngine::play_sound(const std::string& name, float volume, float pitch) {
    auto it = sound_clips_.find(name);
    if (it == sound_clips_.end()) return;

#ifndef ME_NO_OPENAL
    if (headless_ || !alc_context_) return;
    uint32_t src = acquire_source();
    if (!src) return;

    uint32_t buf = get_or_create_buffer(it->second);
    if (!buf) return;

    alSourcei(src, AL_BUFFER, static_cast<ALint>(buf));
    alSourcef(src, AL_GAIN, volume);
    alSourcef(src, AL_PITCH, pitch);
    alSourcei(src, AL_SOURCE_RELATIVE, AL_TRUE);
    alSource3f(src, AL_POSITION, 0.0f, 0.0f, 0.0f);
    alSourcePlay(src);
#else
    (void)volume; (void)pitch;
#endif
}

void AudioEngine::play_sound_3d(const std::string& name, const Vec3& world_pos, float volume, float pitch) {
    auto it = sound_clips_.find(name);
    if (it == sound_clips_.end()) return;

#ifndef ME_NO_OPENAL
    if (headless_ || !alc_context_) return;
    uint32_t src = acquire_source();
    if (!src) return;

    uint32_t buf = get_or_create_buffer(it->second);
    if (!buf) return;

    alSourcei(src, AL_BUFFER, static_cast<ALint>(buf));
    alSourcef(src, AL_GAIN, volume);
    alSourcef(src, AL_PITCH, pitch);
    alSourcei(src, AL_SOURCE_RELATIVE, AL_FALSE);
    alSource3f(src, AL_POSITION, world_pos.x * 0.01f, world_pos.y * 0.01f, world_pos.z * 0.01f);
    alSourcePlay(src);
#else
    (void)world_pos; (void)volume; (void)pitch;
#endif
}

void AudioEngine::play_effect(EAudioEffect effect, float volume, float pitch) {
    static const char* kEffectNames[] = {
        "FX_Footstep", "FX_Jump", "FX_Wallrun", "FX_Vault", "FX_Slide",
        "FX_SkillRoll", "FX_Zipline", "FX_CheckpointChime", "FX_Disarm", "FX_Gunshot"
    };
    size_t idx = static_cast<size_t>(effect);
    if (idx < static_cast<size_t>(EAudioEffect::Count)) {
        play_sound(kEffectNames[idx], volume, pitch);
    }
}

void AudioEngine::play_effect_3d(EAudioEffect effect, const Vec3& world_pos, float volume, float pitch) {
    static const char* kEffectNames[] = {
        "FX_Footstep", "FX_Jump", "FX_Wallrun", "FX_Vault", "FX_Slide",
        "FX_SkillRoll", "FX_Zipline", "FX_CheckpointChime", "FX_Disarm", "FX_Gunshot"
    };
    size_t idx = static_cast<size_t>(effect);
    if (idx < static_cast<size_t>(EAudioEffect::Count)) {
        play_sound_3d(kEffectNames[idx], world_pos, volume, pitch);
    }
}

void AudioEngine::set_music_stems(float ambient_vol, float tension_vol, float chase_vol, float reaction_vol) {
    target_stem_vols_[0] = std::clamp(ambient_vol, 0.0f, 1.0f);
    target_stem_vols_[1] = std::clamp(tension_vol, 0.0f, 1.0f);
    target_stem_vols_[2] = std::clamp(chase_vol, 0.0f, 1.0f);
    target_stem_vols_[3] = std::clamp(reaction_vol, 0.0f, 1.0f);
}

void AudioEngine::stop_all() {
#ifndef ME_NO_OPENAL
    if (headless_ || !alc_context_) return;
    for (size_t i = 0; i < kSourcePoolSize; ++i) {
        if (sources_[i]) alSourceStop(sources_[i]);
    }
#endif
}

const SoundClip* AudioEngine::get_clip(const std::string& name) const {
    auto it = sound_clips_.find(name);
    return (it != sound_clips_.end()) ? &it->second : nullptr;
}

// -----------------------------------------------------------------------------
// Procedural Sound & Dynamic Solar Fields Music Synthesis
// -----------------------------------------------------------------------------
void AudioEngine::synthesize_fallback_clips() {
    constexpr int kSampleRate = 44100;

    auto make_clip = [&](const std::string& name, float duration, auto generator) {
        size_t samples = static_cast<size_t>(duration * kSampleRate);
        std::vector<float> left(samples);
        std::vector<float> right(samples);
        for (size_t i = 0; i < samples; ++i) {
            float t = static_cast<float>(i) / kSampleRate;
            auto [l, r] = generator(t, i, samples);
            left[i] = l;
            right[i] = r;
        }
        SoundClip clip;
        clip.name = name;
        clip.sample_rate = kSampleRate;
        clip.channels = 2;
        clip.duration = duration;
        clip.pcm_data = float_stereo_to_pcm16(left, right);
        sound_clips_[name] = clip;
    };

    // 1. FX_Footstep (45ms filtered impact thump)
    make_clip("FX_Footstep", 0.045f, [](float t, size_t, size_t) -> std::pair<float, float> {
        float noise = (rand_normalized() * 2.0f - 1.0f) * 0.4f;
        float thump = std::sin(2.0f * kPi * 65.0f * t);
        float env = std::exp(-55.0f * t);
        float s = (thump + noise) * env;
        return {s, s};
    });

    // 2. FX_Jump (80ms ascending sweep)
    make_clip("FX_Jump", 0.08f, [](float t, size_t, size_t) -> std::pair<float, float> {
        float freq = 120.0f + 250.0f * (t / 0.08f);
        float s = std::sin(2.0f * kPi * freq * t) * std::sin(kPi * t / 0.08f) * 0.7f;
        return {s, s};
    });

    // 3. FX_Wallrun (300ms continuous friction scuff)
    make_clip("FX_Wallrun", 0.30f, [](float t, size_t, size_t) -> std::pair<float, float> {
        float noise = (rand_normalized() * 2.0f - 1.0f) * 0.5f;
        float rumble = std::sin(2.0f * kPi * 18.0f * t) * 0.5f;
        float s = (noise + rumble) * 0.6f;
        return {s * 0.9f, s * 1.1f};
    });

    // 4. FX_Vault (120ms mid-frequency body plant impact)
    make_clip("FX_Vault", 0.12f, [](float t, size_t, size_t) -> std::pair<float, float> {
        float s = std::sin(2.0f * kPi * 110.0f * t) * std::exp(-25.0f * t) * 0.8f;
        return {s, s};
    });

    // 5. FX_Slide (400ms sustained shoe scraping sound)
    make_clip("FX_Slide", 0.40f, [](float t, size_t, size_t) -> std::pair<float, float> {
        float noise = (rand_normalized() * 2.0f - 1.0f);
        float env = (t < 0.05f) ? (t / 0.05f) : (1.0f - (t - 0.05f) / 0.35f);
        float s = noise * env * 0.5f;
        return {s, s};
    });

    // 6. FX_SkillRoll (350ms whoosh and smooth muffled body roll)
    make_clip("FX_SkillRoll", 0.35f, [](float t, size_t, size_t) -> std::pair<float, float> {
        float s = std::sin(2.0f * kPi * 75.0f * t) * std::exp(-10.0f * t) * 0.7f;
        float whoosh = (rand_normalized() * 2.0f - 1.0f) * std::sin(kPi * t / 0.35f) * 0.3f;
        return {s + whoosh, s + whoosh};
    });

    // 7. FX_Zipline (500ms high-speed steel cable drone)
    make_clip("FX_Zipline", 0.50f, [](float t, size_t, size_t) -> std::pair<float, float> {
        float tone = std::sin(2.0f * kPi * 820.0f * t) * 0.5f + std::sin(2.0f * kPi * 1640.0f * t) * 0.2f;
        float mod = 1.0f + 0.3f * std::sin(2.0f * kPi * 35.0f * t);
        float s = tone * mod * 0.5f;
        return {s * 0.8f, s * 1.2f};
    });

    // 8. FX_CheckpointChime (650ms pristine two-tone chime: E5 -> A5)
    make_clip("FX_CheckpointChime", 0.65f, [](float t, size_t, size_t) -> std::pair<float, float> {
        float note1 = std::sin(2.0f * kPi * 659.25f * t) * std::exp(-7.0f * t) * 0.6f;
        float note2 = (t > 0.12f) ? (std::sin(2.0f * kPi * 880.00f * (t - 0.12f)) * std::exp(-5.0f * (t - 0.12f)) * 0.7f) : 0.0f;
        float chime = note1 + note2;
        return {chime, chime};
    });

    // 9. FX_Disarm (150ms crisp metallic catch and latch click)
    make_clip("FX_Disarm", 0.15f, [](float t, size_t, size_t) -> std::pair<float, float> {
        float s = std::sin(2.0f * kPi * 1760.0f * t) * std::exp(-35.0f * t) * 0.8f;
        return {s, s};
    });

    // 10. FX_Gunshot (200ms explosive transient + acoustic decay)
    make_clip("FX_Gunshot", 0.20f, [](float t, size_t, size_t) -> std::pair<float, float> {
        float thump = std::sin(2.0f * kPi * 85.0f * t) * std::exp(-20.0f * t);
        float noise = (rand_normalized() * 2.0f - 1.0f) * std::exp(-15.0f * t);
        float s = (thump * 0.6f + noise * 0.8f);
        return {s, s};
    });

    // -------------------------------------------------------------------------
    // 4 Dynamic Solar Fields Music Stems (Seamless 5.0-second loops)
    // -------------------------------------------------------------------------

    // Stem 0: Ambient (Ethereal synthesizer chords Am -> F -> C -> G)
    make_clip("Stem_0", 5.0f, [](float t, size_t, size_t) -> std::pair<float, float> {
        float chord = (std::sin(2.0f * kPi * 220.0f * t) +
                       std::sin(2.0f * kPi * 261.63f * t) +
                       std::sin(2.0f * kPi * 329.63f * t) +
                       std::sin(2.0f * kPi * 440.5f * t) * 0.5f) * 0.15f;
        float shimmer = std::sin(2.0f * kPi * 0.4f * t) * 0.05f;
        return {chord + shimmer, chord - shimmer};
    });

    // Stem 1: Tension (55 Hz sub-bass pulse + rhythmic thrum)
    make_clip("Stem_1", 5.0f, [](float t, size_t, size_t) -> std::pair<float, float> {
        float sub = std::sin(2.0f * kPi * 55.0f * t);
        float pulse = std::sin(2.0f * kPi * 2.0f * t) * 0.5f + 0.5f;
        float s = sub * pulse * 0.4f;
        return {s, s};
    });

    // Stem 2: Chase (140 BPM driving electronic breakbeat & bassline)
    make_clip("Stem_2", 5.0f, [](float t, size_t, size_t) -> std::pair<float, float> {
        float beat_t = std::fmod(t, 0.42857f);
        float kick = std::sin(2.0f * kPi * (140.0f - 100.0f * (beat_t / 0.15f)) * beat_t) * std::exp(-25.0f * beat_t) * 0.7f;
        float sub_t = std::fmod(t, 0.10714f);
        float hat = (rand_normalized() * 2.0f - 1.0f) * std::exp(-70.0f * sub_t) * 0.2f;
        float bass = std::sin(2.0f * kPi * 110.0f * t) * 0.35f;
        float s = kick + hat + bass;
        return {s * 0.95f, s * 1.05f};
    });

    // Stem 3: Reaction (Pitch-dropped slowed atmospheric texture)
    make_clip("Stem_3", 5.0f, [](float t, size_t, size_t) -> std::pair<float, float> {
        float slow_pad = std::sin(2.0f * kPi * 110.0f * t) * 0.3f + std::sin(2.0f * kPi * 130.81f * t) * 0.2f;
        float flutter = std::sin(2.0f * kPi * 6.0f * t) * 0.05f;
        float s = (slow_pad + flutter) * 0.45f;
        return {s, s};
    });
}

} // namespace me

#pragma clang diagnostic pop
