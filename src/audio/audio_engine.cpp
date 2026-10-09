#include "audio_engine.hpp"
#include "../assets/upk_loader.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <algorithm>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

#ifndef ME_NO_OPENAL
#if defined(__APPLE__)
#include <OpenAL/al.h>
#include <OpenAL/alc.h>
#else  // OpenAL Soft
#include <AL/al.h>
#include <AL/alc.h>
#endif
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
    (void)datasource;
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

static const char* surface_prefix(ESurfaceMaterial surf) {
    switch (surf) {
        case ESurfaceMaterial::Concrete:     return "Concrete";
        case ESurfaceMaterial::Metal:        return "Metal";
        case ESurfaceMaterial::MetalGantry:  return "MetalGantry";
        case ESurfaceMaterial::MetalAirduct: return "Metal_Airduct";
        case ESurfaceMaterial::MetalLadder:  return "Metal_Ladder";
        case ESurfaceMaterial::Wood:         return "Wood";
        case ESurfaceMaterial::Glass:        return "Glass";
        case ESurfaceMaterial::Water:        return "Water";
        case ESurfaceMaterial::Cardboard:    return "Cardboard";
        default:                             return "Concrete";
    }
}

} // namespace

AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine() {
    shutdown();
}

bool AudioEngine::init(bool headless) {
    headless_ = headless;
    initialized_ = true;

    // Synthesize procedural fallback clips and 4 dynamic Solar Fields stems
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
    sound_cues_.clear();
    ambient_emitters_.clear();
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

    // Match UE3 ALAudioDevice distance attenuation model
    alDistanceModel(AL_INVERSE_DISTANCE_CLAMPED);

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
        alSourcei(src, AL_SOURCE_RELATIVE, AL_TRUE);
        alSource3f(src, AL_POSITION, 0.0f, 0.0f, 0.0f);
        alSourcef(src, AL_GAIN, (i == 0) ? 1.0f : 0.0f);
    }

    // Create dedicated continuous TdSoundNodeVelocity (RunWind) loop source before rebind
    {
        ALuint wsrc = 0;
        alGenSources(1, &wsrc);
        run_wind_source_ = wsrc;
        alSourcei(wsrc, AL_LOOPING, AL_TRUE);
        alSourcei(wsrc, AL_SOURCE_RELATIVE, AL_TRUE);
        alSource3f(wsrc, AL_POSITION, 0.0f, 0.0f, 0.0f);
        alSourcef(wsrc, AL_GAIN, 0.0f);
    }

    rebind_music_stem_buffers();

    // Create dedicated non-stealable 2D VO source (DialogueRadio / DialogueFaith / DialogueOther)
    {
        ALuint vsrc = 0;
        alGenSources(1, &vsrc);
        vo_source_ = vsrc;
        alSourcei(vsrc, AL_LOOPING, AL_FALSE);
        alSourcei(vsrc, AL_SOURCE_RELATIVE, AL_TRUE);
        alSource3f(vsrc, AL_POSITION, 0.0f, 0.0f, 0.0f);
        alSourcef(vsrc, AL_GAIN, 1.0f);
    }

    // Create 4 dedicated 3D ambient emitter sources (*_Aud.me1 AmbientSound pool)
    for (size_t i = 0; i < kAmbientPoolSize; ++i) {
        ALuint asrc = 0;
        alGenSources(1, &asrc);
        ambient_sources_[i] = asrc;
        active_ambient_indices_[i] = -1;
        alSourcei(asrc, AL_LOOPING, AL_TRUE);
        alSourcei(asrc, AL_SOURCE_RELATIVE, AL_FALSE);
        alSourcef(asrc, AL_GAIN, 0.0f);
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

        if (run_wind_source_) {
            alSourceStop(run_wind_source_);
            alDeleteSources(1, &run_wind_source_);
            run_wind_source_ = 0;
        }

        if (vo_source_) {
            alSourceStop(vo_source_);
            alDeleteSources(1, &vo_source_);
            vo_source_ = 0;
        }

        for (size_t i = 0; i < kAmbientPoolSize; ++i) {
            if (ambient_sources_[i]) {
                alSourceStop(ambient_sources_[i]);
                alDeleteSources(1, &ambient_sources_[i]);
                ambient_sources_[i] = 0;
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

void AudioEngine::invalidate_cached_buffer(const std::string& key) {
#ifndef ME_NO_OPENAL
    if (!alc_context_ || key.empty()) return;
    for (const std::string& variant : {key, key + ":mono3d"}) {
        auto it = al_buffers_.find(variant);
        if (it != al_buffers_.end()) {
            ALuint b = it->second;
            if (b) {
                if (vo_source_) {
                    ALint cur_vo = 0;
                    alGetSourcei(vo_source_, AL_BUFFER, &cur_vo);
                    if (static_cast<ALuint>(cur_vo) == b) {
                        alSourceStop(vo_source_);
                        alSourcei(vo_source_, AL_BUFFER, 0);
                    }
                }
                for (int i = 0; i < 4; ++i) {
                    if (music_stem_buffers_[i] == b) {
                        alSourceStop(music_stem_sources_[i]);
                        alSourcei(music_stem_sources_[i], AL_BUFFER, 0);
                        music_stem_buffers_[i] = 0;
                    }
                }
                for (size_t i = 0; i < kAmbientPoolSize; ++i) {
                    if (ambient_sources_[i]) {
                        ALint cur = 0;
                        alGetSourcei(ambient_sources_[i], AL_BUFFER, &cur);
                        if (static_cast<ALuint>(cur) == b) {
                            alSourceStop(ambient_sources_[i]);
                            alSourcei(ambient_sources_[i], AL_BUFFER, 0);
                            active_ambient_indices_[i] = -1;
                        }
                    }
                }
                alDeleteBuffers(1, &b);
            }
            al_buffers_.erase(it);
        }
    }
#else
    (void)key;
#endif
}

uint32_t AudioEngine::get_or_create_buffer(const SoundClip& clip, bool force_mono_for_3d) {
#ifndef ME_NO_OPENAL
    if (headless_ || !alc_context_) return 0;

    std::string base_key = !clip.full_path.empty() ? clip.full_path : clip.name;
    bool downmix_mono = force_mono_for_3d && (clip.channels == 2);
    std::string key = downmix_mono ? (base_key + ":mono3d") : base_key;

    auto it = al_buffers_.find(key);
    if (it != al_buffers_.end()) {
        return it->second;
    }

    if (clip.pcm_data.empty()) {
        return 0;
    }

    ALuint buf = 0;
    alGenBuffers(1, &buf);

    if (downmix_mono && clip.pcm_data.size() >= 4) {
        // OpenAL 1.1 only spatializes & distance-attenuates MONO buffers!
        // Downmix stereo 16-bit PCM (L + R) / 2 into AL_FORMAT_MONO16 for true 3D positioning.
        size_t frames = clip.pcm_data.size() / 4;
        std::vector<int16_t> mono(frames);
        const int16_t* lr = reinterpret_cast<const int16_t*>(clip.pcm_data.data());
        for (size_t i = 0; i < frames; ++i) {
            int32_t sum = static_cast<int32_t>(lr[i * 2 + 0]) + static_cast<int32_t>(lr[i * 2 + 1]);
            mono[i] = static_cast<int16_t>(sum / 2);
        }
        alBufferData(buf, AL_FORMAT_MONO16, mono.data(), static_cast<ALsizei>(mono.size() * sizeof(int16_t)), clip.sample_rate);
    } else {
        ALenum format = (clip.channels == 2) ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16;
        alBufferData(buf, format, clip.pcm_data.data(), static_cast<ALsizei>(clip.pcm_data.size()), clip.sample_rate);
    }

    al_buffers_[key] = buf;
    return buf;
#else
    (void)clip; (void)force_mono_for_3d;
    return 0;
#endif
}

void AudioEngine::clear_chapter_music_clips() {
#ifndef ME_NO_OPENAL
    if (alc_context_) {
        for (int i = 0; i < 4; ++i) {
            if (music_stem_sources_[i]) {
                alSourceStop(music_stem_sources_[i]);
                alSourcei(music_stem_sources_[i], AL_BUFFER, 0);
            }
            music_stem_buffers_[i] = 0;
            music_stem_clip_names_[i].clear();
        }
    }
#endif

    for (const auto& key : active_music_clip_keys_) {
        if (key == "A_M_Menu" || key == "RAW.A_M_Menu") continue;
        invalidate_cached_buffer(key);
        sound_clips_.erase(key);
    }
    active_music_clip_keys_.clear();
}

void AudioEngine::rebind_music_stem_buffers() {
    // Prefer Menu theme on Main Menu, or active chapter's Solar Fields UPK tracks in-game
    const SoundClip* amb = nullptr;
    const SoundClip* tension = nullptr;
    const SoundClip* chase = nullptr;
    const SoundClip* combat = nullptr;

    if (is_menu_music_) {
        amb = pick_first_available_clip({"RAW.A_M_Menu", "A_M_Menu", "Stem_0"});
    } else {
        amb = pick_first_available_clip({
            "RAW.ambience_01", "ambience_01", "RAW.ME_THEME_Ambience", "RAW.A_TT_Music", "A_TT_Music", "Stem_0"
        });
        tension = pick_first_available_clip({
            "RAW.ambience_011", "ambience_011", "RAW.Puzzle_01", "Puzzle_01",
            "RAW.chase_011", "chase_011", "RAW.ambience_02", "ambience_02",
            "RAW.A_TT_Music", "A_TT_Music", "Stem_1"
        });
        chase = pick_first_available_clip({
            "RAW.chase_01", "chase_01", "RAW.chase_02", "chase_02",
            "RAW.ambience_03", "ambience_03", "RAW.A_TT_Music", "A_TT_Music", "Stem_2"
        });
        combat = pick_first_available_clip({
            "RAW.combat_01", "combat_01", "RAW.Combat_New", "Combat_New",
            "RAW.Combat_011", "Combat_011", "RAW.chase_01", "chase_01",
            "RAW.A_TT_Music", "A_TT_Music", "Stem_3"
        });
    }

    const SoundClip* chosen[4] = {amb, tension, chase, combat};

#ifndef ME_NO_OPENAL
    if (headless_ || !alc_context_) return;

    for (int i = 0; i < 4; ++i) {
        if (!music_stem_sources_[i]) continue;
        if (!chosen[i]) {
            alSourceStop(music_stem_sources_[i]);
            alSourcei(music_stem_sources_[i], AL_BUFFER, 0);
            music_stem_buffers_[i] = 0;
            music_stem_clip_names_[i].clear();
            continue;
        }
        std::string new_name = !chosen[i]->full_path.empty() ? chosen[i]->full_path : chosen[i]->name;
        uint32_t buf = get_or_create_buffer(*chosen[i], false);
        if (!buf) continue;
        if (new_name == music_stem_clip_names_[i] && music_stem_buffers_[i] == buf) {
            continue;
        }
        music_stem_clip_names_[i] = new_name;
        music_stem_buffers_[i] = buf;
        alSourceStop(music_stem_sources_[i]);
        alSourcei(music_stem_sources_[i], AL_BUFFER, static_cast<ALint>(buf));
        alSourcePlay(music_stem_sources_[i]);
    }

    // Bind RunWind (TdSoundNodeVelocity) preferring CharacterRunWind or dedicated FX_RunWind fallback
    if (run_wind_source_) {
        const SoundClip* wind_clip = pick_first_available_clip({"RAW.CharacterRunWind", "CharacterRunWind", "FX_RunWind", "FX_Wallrun"});
        if (wind_clip) {
            uint32_t wbuf = get_or_create_buffer(*wind_clip, false);
            if (wbuf) {
                ALint cur_buf = 0;
                ALint cur_state = 0;
                alGetSourcei(run_wind_source_, AL_BUFFER, &cur_buf);
                alGetSourcei(run_wind_source_, AL_SOURCE_STATE, &cur_state);
                if (static_cast<uint32_t>(cur_buf) != wbuf) {
                    alSourceStop(run_wind_source_);
                    alSourcei(run_wind_source_, AL_BUFFER, static_cast<ALint>(wbuf));
                    alSourcePlay(run_wind_source_);
                } else if (cur_state != AL_PLAYING) {
                    alSourcePlay(run_wind_source_);
                }
            }
        }
    }
#else
    (void)chosen;
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

void AudioEngine::set_sound_group_mode(ESoundGroupEffectMode mode) {
    sound_mode_ = mode;
}

void AudioEngine::update(float dt,
                         const Vec3& listener_pos,
                         const Vec3& listener_forward,
                         const Vec3& listener_up,
                         float player_speed,
                         bool reaction_active) {
    // Layered cue waves whose SoundNodeDelay has run out.
    if (!pending_voices_.empty()) {
        const float step = std::max(0.0f, dt);
        std::vector<PendingVoice> due;
        for (auto it = pending_voices_.begin(); it != pending_voices_.end();) {
            it->voice.delay -= step;
            if (it->voice.delay <= 0.0f) {
                due.push_back(std::move(*it));
                it = pending_voices_.erase(it);
            } else {
                ++it;
            }
        }
        for (const PendingVoice& p : due) {
            auto cit = sound_clips_.find(p.voice.clip);
            if (cit != sound_clips_.end()) {
                start_voice(cit->second, p.positional ? &p.position : nullptr, p.voice.volume, p.voice.pitch);
            }
        }
    }

    // Advance active voice-over playback timer and synchronized subtitle lines
    if (vo_duration_ > 0.0f) {
        vo_elapsed_ += std::max(0.0f, dt);
        if (vo_elapsed_ >= vo_duration_) {
            vo_elapsed_ = 0.0f;
            vo_duration_ = 0.0f;
            active_vo_clip_.clear();
            active_vo_subtitle_.clear();
            if (sound_mode_ == ESoundGroupEffectMode::IngameVO) {
                sound_mode_ = ESoundGroupEffectMode::Normal;
            }
        } else {
            auto it = sound_clips_.find(active_vo_clip_);
            if (it != sound_clips_.end() && !it->second.subtitles.empty()) {
                for (const auto& sub : it->second.subtitles) {
                    if (vo_elapsed_ >= sub.time) {
                        active_vo_subtitle_ = sub.text;
                    }
                }
            }
        }
    }

    // Automatically toggle ReactionTime (Mode 3) or IngameVO (Mode 2) SoundGroupEffects
    if (reaction_active && sound_mode_ == ESoundGroupEffectMode::Normal) {
        sound_mode_ = ESoundGroupEffectMode::ReactionTime;
    } else if (!reaction_active && sound_mode_ == ESoundGroupEffectMode::ReactionTime) {
        sound_mode_ = is_vo_playing() ? ESoundGroupEffectMode::IngameVO : ESoundGroupEffectMode::Normal;
    } else if (!reaction_active && is_vo_playing() && sound_mode_ == ESoundGroupEffectMode::Normal) {
        sound_mode_ = ESoundGroupEffectMode::IngameVO;
    }

    // Evaluate target SoundGroup bus adjusters matching DefaultEngine.ini lines 177-209
    float target_sfx_gain = 1.0f;
    float target_music_gain = 1.0f;
    float target_breath_gain = 1.0f;
    float target_slomo_pitch = 1.0f;

    switch (sound_mode_) {
        case ESoundGroupEffectMode::ReactionTime:
            // Mode 3: InGameSFX=0.4, InGameMusic=0.7, FaithBreath=1.5 + TdSoundNodeSlowMotion pitch drop
            target_sfx_gain = 0.45f;
            target_music_gain = 0.70f;
            target_breath_gain = 1.50f;
            target_slomo_pitch = 0.65f;
            break;
        case ESoundGroupEffectMode::Pause:
            // Mode 4: SFX=0.0, HudMusic/InGameMusic=0.3
            target_sfx_gain = 0.0f;
            target_music_gain = 0.30f;
            target_breath_gain = 0.0f;
            break;
        case ESoundGroupEffectMode::IngameVO:
            // Mode 2: InGameSFX=0.7, InGameMusic=0.6, FaithBreath=0.1
            target_sfx_gain = 0.70f;
            target_music_gain = 0.60f;
            target_breath_gain = 0.10f;
            break;
        case ESoundGroupEffectMode::FallingToDeath:
            // Mode 6 (DefaultEngine.ini): freefall wind active at 1.0 pitch, vocals muted
            target_sfx_gain = 1.00f;
            target_music_gain = 0.85f;
            target_breath_gain = 0.0f;
            target_slomo_pitch = 1.0f;
            break;
        case ESoundGroupEffectMode::DeathByFall:
            // Mode 8 (DefaultEngine.ini): Music=0, SFX=0, InGameSFX=0, Dead=1.0 at normal pitch
            target_sfx_gain = 0.0f;
            target_music_gain = 0.0f;
            target_breath_gain = 0.0f;
            target_slomo_pitch = 1.0f;
            break;
        case ESoundGroupEffectMode::DeathGeneric:
            // Mode 9 (DefaultEngine.ini): low-passed fade to near silence at normal pitch
            target_sfx_gain = 0.05f;
            target_music_gain = 0.02f;
            target_breath_gain = 0.0f;
            target_slomo_pitch = 1.0f;
            break;
        default:
            break;
    }

    float lerp_factor = std::clamp(dt * 4.0f, 0.0f, 1.0f);
    if (sound_mode_ == ESoundGroupEffectMode::DeathByFall) {
        sfx_bus_gain_ = 0.0f;
        music_bus_gain_ = 0.0f;
        breath_bus_gain_ = 0.0f;
        slomo_pitch_scale_ = 1.0f;
    } else {
        sfx_bus_gain_ += (target_sfx_gain - sfx_bus_gain_) * lerp_factor;
        music_bus_gain_ += (target_music_gain - music_bus_gain_) * lerp_factor;
        breath_bus_gain_ += (target_breath_gain - breath_bus_gain_) * lerp_factor;
        slomo_pitch_scale_ += (target_slomo_pitch - slomo_pitch_scale_) * lerp_factor;
    }

#ifndef ME_NO_OPENAL
    if (headless_ || !alc_context_) return;

    // 1. Update 3D OpenAL listener position & orientation (1 UU = 0.01m)
    ALfloat pos[3] = {listener_pos.x * 0.01f, listener_pos.y * 0.01f, listener_pos.z * 0.01f};
    alListenerfv(AL_POSITION, pos);

    Vec3 f = listener_forward.normalized();
    Vec3 u = listener_up.normalized();
    ALfloat ori[6] = {f.x, f.y, f.z, u.x, u.y, u.z};
    alListenerfv(AL_ORIENTATION, ori);

    // 2. Evaluate target Solar Fields stem volumes based on gameplay speed & state
    if (is_menu_music_) {
        target_stem_vols_[0] = 1.00f; // Pure Solar Fields Main Menu theme (A_M_Menu)
        target_stem_vols_[1] = 0.00f;
        target_stem_vols_[2] = 0.00f;
        target_stem_vols_[3] = 0.00f;
    } else if (reaction_active) {
        target_stem_vols_[0] = 0.25f; // Ambient attenuated
        target_stem_vols_[1] = 0.25f; // Tension attenuated
        target_stem_vols_[2] = 0.10f; // Chase attenuated
        target_stem_vols_[3] = 1.00f; // Reaction/Combat stem active
    } else if (player_speed > 550.0f) {
        target_stem_vols_[0] = 0.80f; // Ambient pad
        target_stem_vols_[1] = 0.65f; // Tension pulse
        target_stem_vols_[2] = 1.00f; // Full Solar Fields Chase layer!
        target_stem_vols_[3] = 0.00f;
    } else if (player_speed > 260.0f) {
        target_stem_vols_[0] = 1.00f; // Ambient active
        target_stem_vols_[1] = 0.60f; // Puzzle/Tension layer
        target_stem_vols_[2] = 0.25f; // Subtle Chase build
        target_stem_vols_[3] = 0.00f;
    } else {
        target_stem_vols_[0] = 1.00f; // Ambient full
        target_stem_vols_[1] = 0.15f; // Subtle pulse
        target_stem_vols_[2] = 0.00f;
        target_stem_vols_[3] = 0.00f;
    }

    float stem_lerp = std::clamp(dt * 3.0f, 0.0f, 1.0f);
    for (int i = 0; i < 4; ++i) {
        current_stem_vols_[i] += (target_stem_vols_[i] - current_stem_vols_[i]) * stem_lerp;
        if (music_stem_sources_[i]) {
            alSourcef(music_stem_sources_[i], AL_GAIN, current_stem_vols_[i] * music_bus_gain_ * 0.45f);
        }
    }

    // 3. Update TdSoundNodeVelocity (1P RunWind peaking at max sprint speed 720 UU/s + max-speed wind rush)
    if (run_wind_source_) {
        // Trigger a distinct aerodynamic wind rush surge upon crossing max sprint speed (~695-720 UU/s)
        if (!max_speed_wind_active_ && player_speed >= 695.0f) {
            max_speed_wind_active_ = true;
            wind_surge_env_ = 1.0f;
            play_sound("FX_WindGust", 0.55f, 1.04f);
        } else if (max_speed_wind_active_ && player_speed < 660.0f) {
            max_speed_wind_active_ = false;
        }
        wind_surge_env_ = std::max(0.0f, wind_surge_env_ - dt * 1.6f);

        float norm_spd = std::clamp((player_speed - 480.0f) / 240.0f, 0.0f, 1.0f);
        float sq_env = norm_spd * norm_spd; // INTERPOLATION_Square ramping cleanly to 1.0 at 720 UU/s
        float overdrive = std::clamp((player_speed - 720.0f) / 130.0f, 0.0f, 1.0f);
        float target_wind_vol = std::min(1.0f, sq_env * 0.85f + overdrive * 0.15f + wind_surge_env_ * 0.25f);
        current_wind_vol_ += (target_wind_vol - current_wind_vol_) * lerp_factor;
        float wind_pitch = (0.96f + 0.24f * sq_env + 0.08f * overdrive + 0.08f * wind_surge_env_) * slomo_pitch_scale_;

        ALint cur_state = 0;
        alGetSourcei(run_wind_source_, AL_SOURCE_STATE, &cur_state);
        if (cur_state != AL_PLAYING) {
            rebind_music_stem_buffers();
        }
        alSourcef(run_wind_source_, AL_GAIN, current_wind_vol_ * sfx_bus_gain_);
        alSourcef(run_wind_source_, AL_PITCH, wind_pitch);
    }

    // 4. Retail Faith breathing cadence (A_Character_Female_01.upk + AS_C1P_Unarmed.upk notifies):
    // Walk (notifierdummywalk): Breath_Soft.Breath_Soft_Short_{In,Out}
    // Jog/Run (notifierdummyrun): Breath_Medium.Breath_Medium_Long_{In,Out}
    // Sprint (notifierdummysprint, >= 550 UU/s): Breath_Medium.Breath_Medium_Short_{In,Out}
    // (Note: Breath_Hard.* exports in A_Character_Female_01.upk are empty 48-byte stubs with no waves.)
    if ((player_speed > 180.0f || reaction_active) && breath_bus_gain_ > 0.02f) {
        breath_timer_ += dt;
        float breath_interval = reaction_active
            ? 0.78f
            : (player_speed >= 550.0f ? 0.68f : (player_speed >= 320.0f ? 1.05f : 1.15f));
        if (breath_timer_ >= breath_interval) {
            breath_timer_ = 0.0f;
            const char* cue_name = nullptr;
            if (player_speed >= 550.0f || reaction_active) {
                cue_name = breath_inhale_next_ ? "Breath_Medium.Breath_Medium_Short_In"
                                               : "Breath_Medium.Breath_Medium_Short_Out";
            } else if (player_speed >= 320.0f) {
                cue_name = breath_inhale_next_ ? "Breath_Medium.Breath_Medium_Long_In"
                                               : "Breath_Medium.Breath_Medium_Long_Out";
            } else {
                cue_name = breath_inhale_next_ ? "Breath_Soft.Breath_Soft_Short_In"
                                               : "Breath_Soft.Breath_Soft_Short_Out";
            }
            breath_inhale_next_ = !breath_inhale_next_;
            float spd_scale = std::clamp((player_speed - 180.0f) / 520.0f, 0.35f, 1.0f);
            float breath_gain = (reaction_active ? 0.90f : (0.55f + 0.40f * spd_scale)) * breath_bus_gain_;
            play_sound(cue_name, breath_gain, 1.0f);
        }
    } else {
        breath_timer_ = 0.25f;
    }

    // 5. Spatialize nearest 4 3D AmbientSound emitters from *_Aud.me1 sublevels (using MONO 3D buffers!)
    // With a menu up they are silent: retail's front end is a map of its own (TdMainMenu) with no
    // AmbientSound in it, only its music and the UI's cues, and a paused game's sounds are paused.
    if (is_menu_music_) {
        for (size_t slot = 0; slot < kAmbientPoolSize; ++slot) {
            if (active_ambient_indices_[slot] == -1 || !ambient_sources_[slot]) continue;
            active_ambient_indices_[slot] = -1;
            alSourceStop(ambient_sources_[slot]);
        }
    } else if (!ambient_emitters_.empty()) {
        struct Cand { int32_t idx; float dist_sq; };
        std::vector<Cand> cands;
        cands.reserve(ambient_emitters_.size());
        for (size_t i = 0; i < ambient_emitters_.size(); ++i) {
            Vec3 d = ambient_emitters_[i].location - listener_pos;
            float d2 = d.dot(d);
            float max_r = std::max(ambient_emitters_[i].max_radius, 500.0f);
            if (d2 <= max_r * max_r * 1.5f) {
                cands.push_back({static_cast<int32_t>(i), d2});
            }
        }
        std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.dist_sq < b.dist_sq; });

        for (size_t slot = 0; slot < kAmbientPoolSize; ++slot) {
            ALuint asrc = ambient_sources_[slot];
            if (!asrc) continue;
            if (slot < cands.size()) {
                int32_t eidx = cands[slot].idx;
                const auto& em = ambient_emitters_[eidx];
                auto start = [&](const SoundClip* clip, bool loop) {
                    if (!clip) return false;
                    uint32_t buf = get_or_create_buffer(*clip, /*force_mono_for_3d=*/true);
                    if (!buf) return false;
                    alSourceStop(asrc);
                    alSourcei(asrc, AL_BUFFER, static_cast<ALint>(buf));
                    alSourcei(asrc, AL_LOOPING, loop ? AL_TRUE : AL_FALSE);
                    alSourcef(asrc, AL_REFERENCE_DISTANCE, std::max(em.min_radius * 0.01f, 1.0f));
                    alSourcef(asrc, AL_MAX_DISTANCE, std::max(em.max_radius * 0.01f, 10.0f));
                    alSource3f(asrc, AL_POSITION, em.location.x * 0.01f, em.location.y * 0.01f, em.location.z * 0.01f);
                    alSourcePlay(asrc);
                    return true;
                };
                if (active_ambient_indices_[slot] != eidx) {
                    active_ambient_indices_[slot] = eidx;
                    alSourceStop(asrc);
                    // The cue's own graph says how it repeats: a wave under a SoundNodeLooping
                    // plays end to end; with a SoundNodeDelay between them each round waits first.
                    if (!next_ambient_voice(slot, em)) {
                        float dummy_v = em.volume, dummy_p = em.pitch;
                        start(resolve_cue_or_clip(!em.cue_name.empty() ? em.cue_name : em.wave_name, dummy_v, dummy_p), true);
                    }
                }
                float voice_volume = 1.0f, voice_pitch = 1.0f;
                if (ambient_mode_[slot] == AmbientMode::Waiting) {
                    ambient_voice_[slot].delay -= dt;
                    if (ambient_voice_[slot].delay <= 0.0f) {
                        auto it = sound_clips_.find(ambient_voice_[slot].clip);
                        // (A wave that is not loaded is skipped: the next round picks again.)
                        if (it != sound_clips_.end() && start(&it->second, false)) ambient_mode_[slot] = AmbientMode::Playing;
                        else next_ambient_voice(slot, em);
                    }
                } else if (ambient_mode_[slot] == AmbientMode::Playing) {
                    ALint state = AL_STOPPED;
                    alGetSourcei(asrc, AL_SOURCE_STATE, &state);
                    if (state != AL_PLAYING) next_ambient_voice(slot, em);
                }
                if (ambient_mode_[slot] != AmbientMode::Loop) {
                    voice_volume = ambient_voice_[slot].volume;
                    voice_pitch = ambient_voice_[slot].pitch;
                }
                alSourcef(asrc, AL_GAIN, em.volume * voice_volume * sfx_bus_gain_ * 0.55f);
                alSourcef(asrc, AL_PITCH, em.pitch * voice_pitch * slomo_pitch_scale_);
            } else if (active_ambient_indices_[slot] != -1) {
                active_ambient_indices_[slot] = -1;
                alSourceStop(asrc);
            }
        }
    }
#else
    (void)listener_pos; (void)listener_forward; (void)listener_up; (void)player_speed;
#endif
}

bool AudioEngine::load_package_audio_and_cues(const std::string& pkg_path,
                                              std::vector<std::string>* out_clip_keys,
                                              bool extract_level_loaded) {
    namespace fs = std::filesystem;
    if (!fs::exists(pkg_path)) {
        return false;
    }

    UPKPackage pkg(pkg_path);
    if (!pkg.is_valid()) {
        return false;
    }

    auto clips = pkg.extract_audio();
    const bool any_clips = !clips.empty();
    for (auto& clip : clips) {
        store_clip(std::move(clip), out_clip_keys);
    }

    std::vector<SoundCueDef> cues;
    std::vector<AmbientEmitterInfo> ambients;
    pkg.extract_sound_cues_and_ambients(cues, ambients);
    for (auto& cue : cues) {
        sound_cues_[cue.name] = cue;
        if (!cue.full_path.empty()) {
            sound_cues_[cue.full_path] = cue;
        }
    }
    for (auto& em : ambients) {
        ambient_emitters_.push_back(std::move(em));
    }

    if (extract_level_loaded) {
        pkg.extract_level_loaded_sound_cues(level_loaded_cues_);
    }

    return any_clips || !cues.empty();
}

void AudioEngine::store_clip(SoundClip clip, std::vector<std::string>* out_clip_keys) {
    if (clip.pcm_data.size() >= 4 &&
        clip.pcm_data[0] == 'O' && clip.pcm_data[1] == 'g' &&
        clip.pcm_data[2] == 'g' && clip.pcm_data[3] == 'S') {
        std::vector<int16_t> pcm;
        int rate = clip.sample_rate;
        int channels = clip.channels;
        if (decode_ogg_to_pcm(clip.pcm_data.data(), clip.pcm_data.size(), pcm, rate, channels)) {
            clip.pcm_data.resize(pcm.size() * sizeof(int16_t));
            std::memcpy(clip.pcm_data.data(), pcm.data(), clip.pcm_data.size());
            clip.sample_rate = rate;
            clip.channels = channels;
            clip.duration = static_cast<float>(pcm.size()) / (static_cast<float>(rate) * static_cast<float>(channels));
        }
    }
    invalidate_cached_buffer(clip.name);
    sound_clips_[clip.name] = clip;
    if (out_clip_keys) out_clip_keys->push_back(clip.name);
    if (!clip.full_path.empty()) {
        invalidate_cached_buffer(clip.full_path);
        sound_clips_[clip.full_path] = clip;
        if (out_clip_keys) out_clip_keys->push_back(clip.full_path);
    }
}

void AudioEngine::load_imported_waves(const std::string& game_root) {
    namespace fs = std::filesystem;
    std::unordered_map<std::string, std::unordered_set<std::string>> wanted;  // package -> waves
    for (const auto& [key, cue] : sound_cues_) {
        for (const auto& [package, wave] : cue.imported_waves) {
            if (package.empty() || wave.empty()) continue;
            auto it = sound_clips_.find(wave);
            if (it != sound_clips_.end() && !it->second.pcm_data.empty()) continue;
            if (!imported_wave_attempts_.insert(package + "." + wave).second) continue;
            wanted[package].insert(wave);
        }
    }
    for (const auto& [package, waves] : wanted) {
        const fs::path audio_dir = fs::path(game_root) / "TdGame" / "CookedPC" / "Audio";
        fs::path path = audio_dir / (package + ".upk");
        if (!fs::exists(path)) path = audio_dir / "int" / (package + ".upk");  // localized (VO) packages
        if (!fs::exists(path)) continue;
        UPKPackage pkg(path.string());
        if (!pkg.is_valid()) continue;
        for (auto& clip : pkg.extract_audio()) {
            if (waves.count(clip.name)) store_clip(std::move(clip), nullptr);
        }
    }
}

void AudioEngine::stitch_concatenator_cues() {
    for (const auto& [key, cue] : sound_cues_) {
        if (!cue.is_concatenator || cue.wave_names.empty()) continue;
        if (key != cue.name) continue; // Only synthesize once per unique SoundCue

        int target_rate = 0;
        std::vector<const SoundClip*> parts;
        for (const auto& wname : cue.wave_names) {
            auto it = sound_clips_.find(wname);
            if (it != sound_clips_.end() && !it->second.pcm_data.empty()) {
                parts.push_back(&it->second);
                target_rate = std::max(target_rate, it->second.sample_rate);
            }
        }
        if (parts.empty() || target_rate <= 0) continue;

        std::vector<int16_t> stitched_mono;
        std::vector<SoundSubtitleLine> stitched_subs;
        double offset_sec = 0.0;

        for (const SoundClip* part : parts) {
            size_t frame_bytes = static_cast<size_t>(std::max(1, part->channels)) * sizeof(int16_t);
            size_t src_frames = part->pcm_data.size() / frame_bytes;
            if (src_frames == 0) continue;

            const int16_t* raw = reinterpret_cast<const int16_t*>(part->pcm_data.data());
            std::vector<int16_t> mono(src_frames);
            if (part->channels >= 2) {
                for (size_t i = 0; i < src_frames; ++i) {
                    int32_t s = static_cast<int32_t>(raw[i * 2 + 0]) + static_cast<int32_t>(raw[i * 2 + 1]);
                    mono[i] = static_cast<int16_t>(s / 2);
                }
            } else {
                std::memcpy(mono.data(), raw, src_frames * sizeof(int16_t));
            }

            for (const auto& sub : part->subtitles) {
                SoundSubtitleLine s;
                s.time = static_cast<float>(offset_sec) + sub.time;
                s.text = sub.text;
                stitched_subs.push_back(std::move(s));
            }

            if (part->sample_rate == target_rate) {
                stitched_mono.insert(stitched_mono.end(), mono.begin(), mono.end());
                offset_sec += static_cast<double>(src_frames) / static_cast<double>(target_rate);
            } else {
                size_t dst_frames = std::max<size_t>(
                    1, static_cast<size_t>(std::llround(static_cast<double>(src_frames) *
                                                        static_cast<double>(target_rate) /
                                                        static_cast<double>(part->sample_rate))));
                double ratio = static_cast<double>(src_frames - 1) / static_cast<double>(std::max<size_t>(1, dst_frames - 1));
                for (size_t d = 0; d < dst_frames; ++d) {
                    double src_pos = d * ratio;
                    size_t i0 = static_cast<size_t>(src_pos);
                    size_t i1 = std::min(i0 + 1, src_frames - 1);
                    double frac = src_pos - static_cast<double>(i0);
                    double v = static_cast<double>(mono[i0]) * (1.0 - frac) + static_cast<double>(mono[i1]) * frac;
                    stitched_mono.push_back(static_cast<int16_t>(std::clamp(v, -32768.0, 32767.0)));
                }
                offset_sec += static_cast<double>(dst_frames) / static_cast<double>(target_rate);
            }
        }

        if (stitched_mono.empty()) continue;

        SoundClip combined;
        combined.name = cue.name;
        combined.full_path = cue.full_path;
        combined.sample_rate = target_rate;
        combined.channels = 1;
        combined.duration = static_cast<float>(stitched_mono.size()) / static_cast<float>(target_rate);
        combined.pcm_data.resize(stitched_mono.size() * sizeof(int16_t));
        std::memcpy(combined.pcm_data.data(), stitched_mono.data(), combined.pcm_data.size());
        combined.subtitles = std::move(stitched_subs);

        invalidate_cached_buffer(combined.name);
        sound_clips_[combined.name] = combined;
        if (!combined.full_path.empty()) {
            invalidate_cached_buffer(combined.full_path);
            sound_clips_[combined.full_path] = combined;
        }
    }
}

bool AudioEngine::load_sound_bank(const std::string& game_root, const std::string& bank_name) {
    namespace fs = std::filesystem;
    fs::path base(game_root);
    fs::path bank_path = base / "TdGame" / "CookedPC" / "Audio" / bank_name;
    if (!fs::exists(bank_path)) {
        bank_path = base / "TdGame" / "CookedPC" / "Audio" / (bank_name + ".upk");
    }
    if (!fs::exists(bank_path)) {
        bank_path = base / "Audio" / bank_name;
    }
    if (!fs::exists(bank_path)) {
        bank_path = base / "Audio" / (bank_name + ".upk");
    }
    bool ok = load_package_audio_and_cues(bank_path.string());
    if (ok) {
        rebind_music_stem_buffers();
    }
    return ok;
}

bool AudioEngine::load_stock_audio(const std::string& game_root) {
    bool any_loaded = false;
    static const char* kStockBanks[] = {
        "A_M_Menu.upk",
        "A_Material_Footstep.upk",
        "A_Material_Handstep.upk",
        "A_Bodyfalls.upk",
        "A_Character_Female_01.upk",
        "A_Character_Effects.upk",
        "A_Character_Oral.upk",
        "A_Character_Disarm.upk",
        "A_Character_Melee.upk",
        "A_WP_Pistol_BerettaM93R.upk",
        "A_HUD.upk",
        "A_Ambience.upk",
        "A_Props_Interactive.upk"  // door Kismet / matinee sounds (Doors.Door_Barge, Door_Hit, hatch.Squek)
    };

    for (const char* bank : kStockBanks) {
        if (load_sound_bank(game_root, bank)) {
            any_loaded = true;
        }
    }
    load_imported_waves(game_root);
    rebind_music_stem_buffers();
    return any_loaded;
}

bool AudioEngine::load_level_audio(const std::string& game_root, const std::string& map_file) {
    namespace fs = std::filesystem;
    ambient_emitters_.clear();
    level_loaded_cues_.clear();
    vo_elapsed_ = 0.0f;
    vo_duration_ = 0.0f;
    active_vo_clip_.clear();
    active_vo_subtitle_.clear();
    for (size_t i = 0; i < kAmbientPoolSize; ++i) {
        active_ambient_indices_[i] = -1;
    }

    // Purge any previous chapter's Solar Fields music clips so unqualified wave names
    // (ambience_01, ambience_011, Puzzle_01, chase_01, combat_01, etc.) never bleed across levels
    clear_chapter_music_clips();

    is_menu_music_ = (map_file.find("MainMenu") != std::string::npos);

    // Determine exact chapter-matched Solar Fields interactive music bank
    std::string music_bank = "A_M_SP01A.upk";
    if (is_menu_music_)                                               music_bank = "A_M_Menu.upk";
    else if (map_file.find("TT_") != std::string::npos)               music_bank = "A_M_TimeTrial.upk";
    else if (map_file.find("Escape") != std::string::npos ||
             map_file.find("SP01B") != std::string::npos ||
             map_file.find("SP01b") != std::string::npos)             music_bank = "A_M_SP01B.upk";
    else if (map_file.find("SP02") != std::string::npos ||
             map_file.find("Stormdrain") != std::string::npos)        music_bank = "A_M_SP02.upk";
    else if (map_file.find("SP03") != std::string::npos ||
             map_file.find("Cranes") != std::string::npos)            music_bank = "A_M_SP03.upk";
    else if (map_file.find("SP04") != std::string::npos ||
             map_file.find("Subway") != std::string::npos)            music_bank = "A_M_SP04.upk";
    else if (map_file.find("SP05") != std::string::npos ||
             map_file.find("Mall") != std::string::npos)              music_bank = "A_M_SP05.upk";
    else if (map_file.find("SP06") != std::string::npos ||
             map_file.find("Factory") != std::string::npos)           music_bank = "A_M_SP06.upk";
    else if (map_file.find("SP07") != std::string::npos ||
             map_file.find("Boat") != std::string::npos)              music_bank = "A_M_SP07.upk";
    else if (map_file.find("SP08") != std::string::npos ||
             map_file.find("Convoy") != std::string::npos)            music_bank = "A_M_SP08.upk";
    else if (map_file.find("SP09") != std::string::npos ||
             map_file.find("Scraper") != std::string::npos)           music_bank = "A_M_SP09.upk";

    active_music_bank_ = music_bank;
    {
        fs::path base(game_root);
        fs::path bank_path = base / "TdGame" / "CookedPC" / "Audio" / music_bank;
        if (!fs::exists(bank_path)) {
            bank_path = base / "Audio" / music_bank;
        }
        load_package_audio_and_cues(bank_path.string(), &active_music_clip_keys_);
    }

    // Load chapter-matched English voice-over banks from CookedPC/Audio/int/ (e.g. A_VO_SP00.upk + A_VO_SP00_CUE.upk)
    std::string vo_prefix;
    if (map_file.find("SP00") != std::string::npos || map_file.find("Tutorial") != std::string::npos) vo_prefix = "A_VO_SP00";
    else if (map_file.find("SP01") != std::string::npos) vo_prefix = "A_VO_SP01";
    else if (map_file.find("SP02") != std::string::npos) vo_prefix = "A_VO_SP02";
    else if (map_file.find("SP03") != std::string::npos) vo_prefix = "A_VO_SP03";
    else if (map_file.find("SP04") != std::string::npos) vo_prefix = "A_VO_SP04";
    else if (map_file.find("SP05") != std::string::npos) vo_prefix = "A_VO_SP05";
    else if (map_file.find("SP06") != std::string::npos) vo_prefix = "A_VO_SP06";
    else if (map_file.find("SP07") != std::string::npos) vo_prefix = "A_VO_SP07";
    else if (map_file.find("SP08") != std::string::npos) vo_prefix = "A_VO_SP08";
    else if (map_file.find("SP09") != std::string::npos) vo_prefix = "A_VO_SP09";

    fs::path vo_dir = fs::path(game_root) / "TdGame" / "CookedPC" / "Audio" / "int";
    if (!vo_prefix.empty() && fs::exists(vo_dir)) {
        for (const auto& entry : fs::directory_iterator(vo_dir)) {
            if (!entry.is_regular_file()) continue;
            std::string fn = entry.path().filename().string();
            if (fn.rfind(vo_prefix, 0) == 0 && entry.path().extension() == ".upk") {
                load_package_audio_and_cues(entry.path().string());
            }
        }
    }

    // Scan map directory for streaming *_LOC_int.upk dialogue packages and *_Aud.me1 sublevels matching this level's prefix
    fs::path full_map = fs::path(game_root) / "TdGame" / "CookedPC" / map_file;
    if (!fs::exists(full_map)) {
        full_map = fs::path(map_file);
    }

    std::string map_stem = full_map.stem().string();
    if (map_stem.size() > 2 &&
        map_stem.compare(map_stem.size() - 2, 2, "_p") == 0) {
        map_stem.resize(map_stem.size() - 2);
    }
    std::string prefix_underscore = map_stem + "_";

    bool loaded_any_aud = false;
    if (fs::exists(full_map.parent_path())) {
        for (const auto& entry : fs::directory_iterator(full_map.parent_path())) {
            if (!entry.is_regular_file()) continue;
            std::string fn = entry.path().filename().string();
            if (!map_stem.empty() && fn.rfind(prefix_underscore, 0) != 0) continue;
            std::string fn_low = fn;
            std::transform(fn_low.begin(), fn_low.end(), fn_low.begin(), ::tolower);
            if (fn_low.find("_loc_int.upk") != std::string::npos) {
                if (load_package_audio_and_cues(entry.path().string())) {
                    loaded_any_aud = true;
                }
            }
        }
        for (const auto& entry : fs::directory_iterator(full_map.parent_path())) {
            if (!entry.is_regular_file()) continue;
            std::string fn = entry.path().filename().string();
            if (!map_stem.empty() && fn.rfind(prefix_underscore, 0) != 0) continue;
            if (fn.find("_Aud.me1") != std::string::npos || fn.find("_Audio0.me1") != std::string::npos) {
                if (load_package_audio_and_cues(entry.path().string(), nullptr, /*extract_level_loaded=*/true)) {
                    loaded_any_aud = true;
                }
            }
        }
    }

    load_imported_waves(game_root);
    stitch_concatenator_cues();
    rebind_music_stem_buffers();
    return loaded_any_aud;
}

const SoundClip* AudioEngine::pick_first_available_clip(std::initializer_list<const char*> candidates) const {
    for (const char* name : candidates) {
        auto it = sound_clips_.find(name);
        if (it != sound_clips_.end() && !it->second.pcm_data.empty()) {
            return &it->second;
        }
    }
    return nullptr;
}

const SoundClip* AudioEngine::resolve_cue_or_clip(const std::string& name, float& io_vol, float& io_pitch) const {
    // 1. Check UE3 SoundCue graph first (supports SoundNodeConcatenator, SoundNodeRandom + SoundNodeModulator)
    auto cue_it = sound_cues_.find(name);
    if (cue_it != sound_cues_.end() && !cue_it->second.wave_names.empty()) {
        const auto& cue = cue_it->second;
        io_vol *= cue.volume_multiplier;
        if (cue.has_modulator && !cue.is_concatenator && cue.sound_group.find("Dialogue") == std::string::npos) {
            io_pitch *= cue.pitch_multiplier * (0.96f + 0.08f * rand_normalized());
        } else {
            io_pitch *= cue.pitch_multiplier;
        }

        // If this SoundCue is a stitched SoundNodeConcatenator (e.g. radio squelch + Merc VO + squelch),
        // return the complete concatenated SoundClip directly.
        if (cue.is_concatenator) {
            auto sit = sound_clips_.find(cue.name);
            if (sit != sound_clips_.end() && !sit->second.pcm_data.empty()) {
                return &sit->second;
            }
        }

        size_t idx = static_cast<size_t>(std::rand()) % cue.wave_names.size();
        const std::string& wname = cue.wave_names[idx];
        auto wit = sound_clips_.find(wname);
        if (wit != sound_clips_.end() && !wit->second.pcm_data.empty()) {
            return &wit->second;
        }
        // Scan all wave_names if random choice was an unloaded import
        for (const auto& alt : cue.wave_names) {
            auto ait = sound_clips_.find(alt);
            if (ait != sound_clips_.end() && !ait->second.pcm_data.empty()) {
                return &ait->second;
            }
        }
    }

    // 2. Direct SoundClip lookup
    auto it = sound_clips_.find(name);
    if (it != sound_clips_.end() && !it->second.pcm_data.empty()) {
        return &it->second;
    }

    return nullptr;
}

bool AudioEngine::has_sound(const std::string& name) const {
    float vol = 1.0f;
    float pitch = 1.0f;
    return resolve_cue_or_clip(name, vol, pitch) != nullptr;
}

size_t AudioEngine::count_sound_layers(const std::string& name) const {
    auto cue_it = sound_cues_.find(name);
    if (cue_it != sound_cues_.end() && cue_it->second.has_mixer && !cue_it->second.is_concatenator &&
        !cue_it->second.looping && !cue_it->second.nodes.empty()) {
        std::vector<CueVoice> voices;
        collect_cue_voices(cue_it->second, 0, 0.0f, 1.0f, 1.0f, voices);
        if (!voices.empty()) return voices.size();
    }
    return has_sound(name) ? 1 : 0;
}

// USoundNodeLooping over a USoundNodeDelay: every round the delay is drawn again (and whatever
// is under it: the modulation, which wave), the sound plays once, and the next round starts when
// it ends. The first round waits too, as the cue does when the level starts it.
bool AudioEngine::next_ambient_voice(size_t slot, const AmbientEmitterInfo& em) {
    ambient_mode_[slot] = AmbientMode::Loop;
    if (em.cue_name.empty()) return false;
    auto it = sound_cues_.find(em.cue_name);
    if (it == sound_cues_.end() || !it->second.looping || it->second.is_concatenator || it->second.nodes.empty()) return false;
    std::vector<CueVoice> voices;
    collect_cue_voices(it->second, 0, 0.0f, 1.0f, 1.0f, voices);
    if (voices.empty() || voices[0].delay <= 0.0f) return false;
    ambient_voice_[slot] = voices[0];
    ambient_mode_[slot] = AmbientMode::Waiting;
    return true;
}

void AudioEngine::collect_cue_voices(const SoundCueDef& cue, int node, float delay, float volume, float pitch,
                                     std::vector<CueVoice>& out) const {
    if (node < 0 || static_cast<size_t>(node) >= cue.nodes.size() || out.size() >= 8) return;
    const SoundCueNode& n = cue.nodes[static_cast<size_t>(node)];
    auto in_range = [](float lo, float hi) { return lo + (hi - lo) * rand_normalized(); };
    switch (n.kind) {
        case SoundCueNode::Kind::Wave: {
            auto it = sound_clips_.find(n.wave);
            if (it != sound_clips_.end() && !it->second.pcm_data.empty()) {
                out.push_back({n.wave, delay, volume, pitch});
            }
            return;
        }
        case SoundCueNode::Kind::Mixer:
            // USoundNodeMixer: every input, scaled by its InputVolume.
            for (size_t i = 0; i < n.children.size(); ++i) {
                const float input = (i < n.weights.size()) ? n.weights[i] : 1.0f;
                collect_cue_voices(cue, n.children[i], delay, volume * input, pitch, out);
            }
            return;
        case SoundCueNode::Kind::Random: {
            // USoundNodeRandom: one child, picked by weight.
            if (n.children.empty()) return;
            auto weight = [&n](size_t i) { return (i < n.weights.size()) ? std::max(n.weights[i], 0.0f) : 1.0f; };
            float total = 0.0f;
            for (size_t i = 0; i < n.children.size(); ++i) total += weight(i);
            float pick = rand_normalized() * total;
            size_t chosen = n.children.size() - 1;
            for (size_t i = 0; i < n.children.size(); ++i) {
                if (pick < weight(i)) {
                    chosen = i;
                    break;
                }
                pick -= weight(i);
            }
            collect_cue_voices(cue, n.children[chosen], delay, volume, pitch, out);
            return;
        }
        case SoundCueNode::Kind::Delay:
            if (!n.children.empty()) {
                collect_cue_voices(cue, n.children[0], delay + in_range(n.min_value, n.max_value), volume, pitch, out);
            }
            return;
        case SoundCueNode::Kind::Modulator:
            if (!n.children.empty()) {
                collect_cue_voices(cue, n.children[0], delay, volume * in_range(n.min_value, n.max_value),
                                   pitch * in_range(n.min_pitch, n.max_pitch), out);
            }
            return;
        case SoundCueNode::Kind::Passthrough:
        default:
            if (!n.children.empty()) collect_cue_voices(cue, n.children[0], delay, volume, pitch, out);
            return;
    }
}

bool AudioEngine::play_layered_cue(const std::string& name, const Vec3* world_pos, float volume, float pitch) {
    auto cue_it = sound_cues_.find(name);
    if (cue_it == sound_cues_.end()) return false;
    const SoundCueDef& cue = cue_it->second;
    if (!cue.has_mixer || cue.is_concatenator || cue.looping || cue.nodes.empty()) return false;
    std::vector<CueVoice> voices;
    collect_cue_voices(cue, 0, 0.0f, volume * cue.volume_multiplier, pitch * cue.pitch_multiplier, voices);
    if (voices.empty()) return false;
    if (play_log_on_) {
        // As long as its longest layer, delay included.
        float duration = 0.0f;
        for (const CueVoice& v : voices) {
            auto it = sound_clips_.find(v.clip);
            if (it != sound_clips_.end()) duration = std::max(duration, v.delay + it->second.duration / std::max(v.pitch, 0.01f));
        }
        play_log_.push_back({name, world_pos ? "sound3d" : "sound", duration});
    }
    if (headless_ || !alc_context_) return true;
    for (CueVoice& v : voices) {
        if (v.delay <= 0.0f) {
            auto it = sound_clips_.find(v.clip);
            if (it != sound_clips_.end()) start_voice(it->second, world_pos, v.volume, v.pitch);
        } else {
            PendingVoice p;
            p.voice = std::move(v);
            p.positional = (world_pos != nullptr);
            if (world_pos) p.position = *world_pos;
            pending_voices_.push_back(std::move(p));
        }
    }
    return true;
}

void AudioEngine::start_voice(const SoundClip& clip, const Vec3* world_pos, float volume, float pitch) {
#ifndef ME_NO_OPENAL
    if (headless_ || !alc_context_) return;
    uint32_t src = acquire_source();
    if (!src) return;

    uint32_t buf = get_or_create_buffer(clip, /*force_mono_for_3d=*/world_pos != nullptr);
    if (!buf) return;

    alSourcei(src, AL_BUFFER, static_cast<ALint>(buf));
    alSourcef(src, AL_GAIN, std::clamp(volume * sfx_bus_gain_, 0.0f, 1.5f));
    alSourcef(src, AL_PITCH, std::clamp(pitch * slomo_pitch_scale_, 0.25f, 2.0f));
    if (world_pos) {
        alSourcei(src, AL_SOURCE_RELATIVE, AL_FALSE);
        alSourcef(src, AL_REFERENCE_DISTANCE, 2.5f);
        alSourcef(src, AL_MAX_DISTANCE, 35.0f);
        alSource3f(src, AL_POSITION, world_pos->x * 0.01f, world_pos->y * 0.01f, world_pos->z * 0.01f);
    } else {
        alSourcei(src, AL_SOURCE_RELATIVE, AL_TRUE);
        alSource3f(src, AL_POSITION, 0.0f, 0.0f, 0.0f);
    }
    alSourcePlay(src);
#else
    (void)clip; (void)world_pos; (void)volume; (void)pitch;
#endif
}

void AudioEngine::play_sound(const std::string& name, float volume, float pitch) {
    if (play_layered_cue(name, nullptr, volume, pitch)) return;
    float final_vol = volume * sfx_bus_gain_;
    float final_pitch = pitch * slomo_pitch_scale_;
    const SoundClip* clip = resolve_cue_or_clip(name, final_vol, final_pitch);
    if (!clip) return;
    if (play_log_on_) play_log_.push_back({name, "sound", clip->duration});

#ifndef ME_NO_OPENAL
    if (headless_ || !alc_context_) return;
    uint32_t src = acquire_source();
    if (!src) return;

    uint32_t buf = get_or_create_buffer(*clip, false);
    if (!buf) return;

    alSourcei(src, AL_BUFFER, static_cast<ALint>(buf));
    alSourcef(src, AL_GAIN, std::clamp(final_vol, 0.0f, 1.5f));
    alSourcef(src, AL_PITCH, std::clamp(final_pitch, 0.25f, 2.0f));
    alSourcei(src, AL_SOURCE_RELATIVE, AL_TRUE);
    alSource3f(src, AL_POSITION, 0.0f, 0.0f, 0.0f);
    alSourcePlay(src);
#else
    (void)final_vol; (void)final_pitch;
#endif
}

void AudioEngine::play_vo(const std::string& name, float volume) {
    float final_vol = volume;
    float final_pitch = 1.0f;
    const SoundClip* clip = resolve_cue_or_clip(name, final_vol, final_pitch);
    if (!clip) return;
    if (play_log_on_) play_log_.push_back({name, "vo", clip->duration});

    active_vo_clip_ = clip->name;
    vo_duration_ = clip->duration;
    vo_elapsed_ = 0.0f;
    active_vo_subtitle_.clear();
    if (!clip->subtitles.empty()) {
        active_vo_subtitle_ = clip->subtitles.front().text;
    }

    std::cout << "[Audio] Playing VO '" << name << "' (" << clip->duration << "s, "
              << clip->sample_rate << " Hz)" << std::endl;

#ifndef ME_NO_OPENAL
    if (headless_ || !alc_context_ || !vo_source_) return;

    uint32_t buf = get_or_create_buffer(*clip, false);
    if (!buf) return;

    alSourceStop(vo_source_);
    alSourcei(vo_source_, AL_BUFFER, static_cast<ALint>(buf));
    alSourcef(vo_source_, AL_GAIN, std::clamp(volume, 0.0f, 1.5f));
    alSourcef(vo_source_, AL_PITCH, 1.0f);
    alSourcei(vo_source_, AL_SOURCE_RELATIVE, AL_TRUE);
    alSource3f(vo_source_, AL_POSITION, 0.0f, 0.0f, 0.0f);
    alSourcePlay(vo_source_);
#endif
}

void AudioEngine::play_level_loaded_cues() {
    for (const auto& cue_name : level_loaded_cues_) {
        play_vo(cue_name, 1.0f);
    }
}

void AudioEngine::play_sound_3d(const std::string& name, const Vec3& world_pos, float volume, float pitch) {
    if (play_layered_cue(name, &world_pos, volume, pitch)) return;
    float final_vol = volume * sfx_bus_gain_;
    float final_pitch = pitch * slomo_pitch_scale_;
    const SoundClip* clip = resolve_cue_or_clip(name, final_vol, final_pitch);
    if (!clip) return;
    if (play_log_on_) play_log_.push_back({name, "sound3d", clip->duration});

#ifndef ME_NO_OPENAL
    if (headless_ || !alc_context_) return;
    uint32_t src = acquire_source();
    if (!src) return;

    uint32_t buf = get_or_create_buffer(*clip, /*force_mono_for_3d=*/true);
    if (!buf) return;

    alSourcei(src, AL_BUFFER, static_cast<ALint>(buf));
    alSourcef(src, AL_GAIN, std::clamp(final_vol, 0.0f, 1.5f));
    alSourcef(src, AL_PITCH, std::clamp(final_pitch, 0.25f, 2.0f));
    alSourcei(src, AL_SOURCE_RELATIVE, AL_FALSE);
    alSourcef(src, AL_REFERENCE_DISTANCE, 2.5f);
    alSourcef(src, AL_MAX_DISTANCE, 35.0f);
    alSource3f(src, AL_POSITION, world_pos.x * 0.01f, world_pos.y * 0.01f, world_pos.z * 0.01f);
    alSourcePlay(src);
#else
    (void)world_pos; (void)final_vol; (void)final_pitch;
#endif
}

void AudioEngine::play_footstep(ESurfaceMaterial surface, float speed, bool crouch, float volume) {
    std::string prefix = surface_prefix(surface);
    std::string action = "_03_Female_FootStepRun";
    if (crouch) action = "_01_Female_FootStepSneak";
    else if (speed > 620.0f) action = "_04_Female_FootStepSprint";
    else if (speed < 250.0f) action = "_02_Female_FootStepWalk";

    std::string full_cue = prefix + "." + action;
    float v = volume, p = 1.0f;
    if (resolve_cue_or_clip(full_cue, v, p)) {
        play_sound(full_cue, volume, 1.0f);
    } else if (resolve_cue_or_clip(action, v, p)) {
        play_sound(action, volume, 1.0f);
    } else {
        play_sound("FX_Footstep", volume, 0.95f + 0.1f * rand_normalized());
    }
}

void AudioEngine::play_footstep_number(ESurfaceMaterial surface, int number, float volume) {
    // A_Material_Footstep names its cues <Surface>._NN_Female_FootStep<Kind>.
    static const char* const kKinds[] = {nullptr, "Sneak", "Walk", "Run", "Sprint", "SprintRelease", "WallRun",
                                         "WallrunRelease", "LandSoft", "LandMedium", "LandHard", "Slide"};
    if (number < 1 || number > 11) return;
    char action[64];
    std::snprintf(action, sizeof(action), "_%02d_Female_FootStep%s", number, kKinds[number]);
    const std::string full_cue = std::string(surface_prefix(surface)) + "." + action;
    float v = volume, p = 1.0f;
    if (resolve_cue_or_clip(full_cue, v, p)) {
        play_sound(full_cue, volume, 1.0f);
    } else if (resolve_cue_or_clip(action, v, p)) {
        play_sound(action, volume, 1.0f);
    }
}

bool AudioEngine::play_cue(const std::string& group_and_name, bool voice, float volume) {
    const size_t dot = group_and_name.rfind('.');
    const std::string bare = dot == std::string::npos ? group_and_name : group_and_name.substr(dot + 1);
    for (const std::string& name : {group_and_name, bare}) {
        float v = volume, p = 1.0f;
        if (!resolve_cue_or_clip(name, v, p)) continue;
        if (voice) {
            play_vo(name, volume);
        } else {
            play_sound(name, volume, 1.0f);
        }
        return true;
    }
    return false;
}

bool AudioEngine::has_cue(const std::string& group_and_name) const {
    const size_t dot = group_and_name.rfind('.');
    const std::string bare = dot == std::string::npos ? group_and_name : group_and_name.substr(dot + 1);
    for (const std::string& name : {group_and_name, bare}) {
        float v = 1.0f, p = 1.0f;
        if (resolve_cue_or_clip(name, v, p)) return true;
    }
    return false;
}

float AudioEngine::cue_duration(const std::string& group_and_name) const {
    const size_t dot = group_and_name.rfind('.');
    const std::string bare = dot == std::string::npos ? group_and_name : group_and_name.substr(dot + 1);
    for (const std::string& name : {group_and_name, bare}) {
        float v = 1.0f, p = 1.0f;
        if (const SoundClip* clip = resolve_cue_or_clip(name, v, p)) return clip->duration;
    }
    return 0.0f;
}

bool AudioEngine::load_cue_bank(const std::string& game_root, const std::string& package) {
    namespace fs = std::filesystem;
    if (package.empty() || !cue_banks_tried_.insert(package).second) return false;
    const fs::path audio = fs::path(game_root) / "TdGame" / "CookedPC" / "Audio";
    fs::path file = audio / (package + ".upk");
    if (!fs::exists(file)) file = audio / "int" / (package + ".upk");
    if (!fs::exists(file)) return false;
    bool any = load_package_audio_and_cues(file.string());

    // A cue can play waves that live in another package: dialogue keeps its cues in <name>_CUE.upk
    // and the waves in <name>.upk, and A_Props_Interactive's door hits are A_CXP_Plaza's. Whichever
    // of those is not loaded yet comes along.
    std::vector<std::string> wave_packages;
    if (const UPKPackage pkg(file.string()); pkg.is_valid()) {
        const auto& imports = pkg.get_imports();
        for (const FObjectImport& imp : imports) {
            if (imp.class_name != "SoundNodeWave" || sound_clips_.count(imp.object_name)) continue;
            const FObjectImport* top = &imp;
            for (int guard = 0; top->outer_index < 0 && guard < 16; ++guard) {
                const size_t outer = static_cast<size_t>(-top->outer_index - 1);
                if (outer >= imports.size()) break;
                top = &imports[outer];
            }
            if (top != &imp) wave_packages.push_back(top->object_name);
        }
    }
    for (const std::string& name : wave_packages) any |= load_cue_bank(game_root, name);
    if (any) {
        stitch_concatenator_cues();
        rebind_music_stem_buffers();
    }
    return any;
}

void AudioEngine::stop_cue(const std::string& group_and_name) {
#ifndef ME_NO_OPENAL
    if (headless_ || !alc_context_) return;
    const size_t dot = group_and_name.rfind('.');
    const std::string bare = dot == std::string::npos ? group_and_name : group_and_name.substr(dot + 1);
    for (const std::string& name : {group_and_name, bare}) {
        float v = 1.0f, p = 1.0f;
        const SoundClip* clip = resolve_cue_or_clip(name, v, p);
        if (!clip) continue;
        const std::string key = !clip->full_path.empty() ? clip->full_path : clip->name;
        for (const std::string& variant : {key, key + ":mono3d"}) {
            const auto it = al_buffers_.find(variant);
            if (it == al_buffers_.end() || !it->second) continue;
            for (size_t i = 0; i < kSourcePoolSize; ++i) {
                ALint buffer = 0;
                alGetSourcei(sources_[i], AL_BUFFER, &buffer);
                if (static_cast<ALuint>(buffer) == it->second) alSourceStop(sources_[i]);
            }
        }
        return;
    }
#else
    (void)group_and_name;
#endif
}

void AudioEngine::play_handstep(ESurfaceMaterial surface, bool hard_impact, float volume) {
    std::string prefix = surface_prefix(surface);
    std::string action = hard_impact ? "_23_Female_HandStepHard" : "_21_Female_HandStepSoft";
    std::string full_cue = prefix + "." + action;
    float v = volume, p = 1.0f;
    if (resolve_cue_or_clip(full_cue, v, p)) {
        play_sound(full_cue, volume, 1.0f);
    } else if (resolve_cue_or_clip(action, v, p)) {
        play_sound(action, volume, 1.0f);
    } else {
        play_sound("FX_Vault", volume, 1.0f);
    }
}

void AudioEngine::play_effect(EAudioEffect effect, float volume, float pitch) {
    // Prefer real Mirror's Edge UE3 SoundCue / SoundNodeWave assets when present
    switch (effect) {
        case EAudioEffect::Footstep:
            play_footstep(ESurfaceMaterial::Concrete, 550.0f, false, volume);
            return;
        case EAudioEffect::Jump: {
            float v = volume, p = pitch;
            if (resolve_cue_or_clip("Strain_Medium_Cue", v, p)) { play_sound("Strain_Medium_Cue", volume, pitch); return; }
            if (resolve_cue_or_clip("Vault", v, p)) { play_sound("Vault", volume, pitch); return; }
            break;
        }
        case EAudioEffect::Wallrun: {
            float v = volume, p = pitch;
            if (resolve_cue_or_clip("Concrete._06_Female_FootStepWallRun", v, p)) {
                play_sound("Concrete._06_Female_FootStepWallRun", volume, pitch);
                return;
            }
            break;
        }
        case EAudioEffect::Vault: {
            float v = volume, p = pitch;
            if (resolve_cue_or_clip("Vault", v, p)) { play_sound("Vault", volume, pitch); return; }
            play_handstep(ESurfaceMaterial::Concrete, true, volume);
            return;
        }
        case EAudioEffect::Slide: {
            float v = volume, p = pitch;
            if (resolve_cue_or_clip("Body.BodySlide", v, p)) { play_sound("Body.BodySlide", volume, pitch); return; }
            break;
        }
        case EAudioEffect::SkillRoll: {
            float v = volume, p = pitch;
            if (resolve_cue_or_clip("Body.Roll", v, p)) { play_sound("Body.Roll", volume, pitch); return; }
            break;
        }
        case EAudioEffect::CheckpointChime: {
            float v = volume, p = pitch;
            if (resolve_cue_or_clip("BagFound", v, p)) { play_sound("BagFound", volume, pitch); return; }
            break;
        }
        case EAudioEffect::Disarm: {
            float v = volume, p = pitch;
            if (resolve_cue_or_clip("Arm_Break_01", v, p)) { play_sound("Arm_Break_01", volume, pitch); return; }
            break;
        }
        case EAudioEffect::Gunshot: {
            float v = volume, p = pitch;
            if (resolve_cue_or_clip("Fire1P", v, p)) { play_sound("Fire1P", volume, pitch); return; }
            break;
        }
        case EAudioEffect::FallDeathScream: {
            // Retail TdPlayerPawn.UncontrolledFall.BeginState: SetSoundMode(6) + Death_Fall
            // (Freefall_Loop + LOD stereo wind rush; retail plays NO vocal scream or impact on entry)
            set_sound_group_mode(ESoundGroupEffectMode::FallingToDeath);
            float v = volume, p = pitch;
            if (resolve_cue_or_clip("Freefall_Loop", v, p)) {
                play_sound("Freefall_Loop", volume * 1.10f, 1.0f);
                if (resolve_cue_or_clip("LOD", v, p)) play_sound("LOD", volume * 0.55f, 1.0f);
                return;
            }
            if (resolve_cue_or_clip("Death_Fall", v, p)) {
                play_sound("Death_Fall", volume * 1.10f, 1.0f);
                return;
            }
            break;
        }
        case EAudioEffect::FallDeathImpact: {
            // Retail TdPlayerPawn.UncontrolledFall.Landed: fade out FallingSound immediately,
            // cut all Music/SFX/VO via SetSoundMode(8), and play ONLY Faith.Death_Impact (Bodyfall01..05)
            // at 1.0x pitch in complete silence (NEVER Misc.ArmCrack!).
            stop_cue("Death_Fall");
            stop_cue("Freefall_Loop");
            stop_cue("LOD");
            set_sound_group_mode(ESoundGroupEffectMode::DeathByFall);
            float v = volume, p = 1.0f;
            if (resolve_cue_or_clip("Death_Impact", v, p)) {
                // Temporarily ensure Dead bus sound plays cleanly even while sfx_bus_gain_ = 0
                float saved_sfx = sfx_bus_gain_;
                sfx_bus_gain_ = 1.0f;
                play_sound("Death_Impact", volume * 1.15f, 1.0f);
                sfx_bus_gain_ = saved_sfx;
                return;
            }
            if (resolve_cue_or_clip("BodyFall", v, p)) {
                float saved_sfx = sfx_bus_gain_;
                sfx_bus_gain_ = 1.0f;
                play_sound("BodyFall", volume * 1.15f, 1.0f);
                sfx_bus_gain_ = saved_sfx;
                return;
            }
            break;
        }
        default:
            break;
    }

    static const char* kFallbackEffectNames[] = {
        "FX_Footstep", "FX_Jump", "FX_Wallrun", "FX_Vault", "FX_Slide",
        "FX_SkillRoll", "FX_Zipline", "FX_CheckpointChime", "FX_Disarm", "FX_Gunshot",
        "FX_FallDeathScream", "FX_FallDeathImpact"
    };
    size_t idx = static_cast<size_t>(effect);
    if (idx < static_cast<size_t>(EAudioEffect::Count)) {
        play_sound(kFallbackEffectNames[idx], volume, pitch);
    }
}

void AudioEngine::play_effect_3d(EAudioEffect effect, const Vec3& world_pos, float volume, float pitch) {
    float v = volume, p = pitch;
    switch (effect) {
        case EAudioEffect::Footstep:
            if (resolve_cue_or_clip("Concrete._03_Female_FootStepRun", v, p)) {
                play_sound_3d("Concrete._03_Female_FootStepRun", world_pos, volume, pitch);
                return;
            }
            break;
        case EAudioEffect::Gunshot:
            if (resolve_cue_or_clip("Fire3P", v, p)) {
                play_sound_3d("Fire3P", world_pos, volume, pitch);
                return;
            }
            if (resolve_cue_or_clip("Fire1P", v, p)) {
                play_sound_3d("Fire1P", world_pos, volume, pitch);
                return;
            }
            break;
        case EAudioEffect::Disarm:
            if (resolve_cue_or_clip("Arm_Break_01", v, p)) {
                play_sound_3d("Arm_Break_01", world_pos, volume, pitch);
                return;
            }
            break;
        default:
            break;
    }

    static const char* kFallbackEffectNames[] = {
        "FX_Footstep", "FX_Jump", "FX_Wallrun", "FX_Vault", "FX_Slide",
        "FX_SkillRoll", "FX_Zipline", "FX_CheckpointChime", "FX_Disarm", "FX_Gunshot",
        "FX_FallDeathScream", "FX_FallDeathImpact"
    };
    size_t idx = static_cast<size_t>(effect);
    if (idx < static_cast<size_t>(EAudioEffect::Count)) {
        play_sound_3d(kFallbackEffectNames[idx], world_pos, volume, pitch);
    }
}

void AudioEngine::set_music_stems(float ambient_vol, float tension_vol, float chase_vol, float reaction_vol) {
    target_stem_vols_[0] = std::clamp(ambient_vol, 0.0f, 1.0f);
    target_stem_vols_[1] = std::clamp(tension_vol, 0.0f, 1.0f);
    target_stem_vols_[2] = std::clamp(chase_vol, 0.0f, 1.0f);
    target_stem_vols_[3] = std::clamp(reaction_vol, 0.0f, 1.0f);
}

void AudioEngine::set_menu_music(bool active) {
    if (is_menu_music_ != active) {
        is_menu_music_ = active;
        rebind_music_stem_buffers();
    }
}

void AudioEngine::stop_all() {
    pending_voices_.clear();
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
// Procedural Sound & Dynamic Solar Fields Music Synthesis (Offline Fallback)
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
        clip.full_path = name;
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

    // 11. FX_RunWind (2.0s seamless stereo aerodynamic wind rush + cloth flutter loop)
    {
        constexpr float kWindDur = 2.0f;
        size_t samples = static_cast<size_t>(kWindDur * kSampleRate);
        std::vector<float> left(samples), right(samples);
        float lp_l1 = 0.0f, lp_l2 = 0.0f, lp_r1 = 0.0f, lp_r2 = 0.0f;
        for (size_t i = 0; i < samples; ++i) {
            float t = static_cast<float>(i) / kSampleRate;
            float n_l = rand_normalized() * 2.0f - 1.0f;
            float n_r = rand_normalized() * 2.0f - 1.0f;
            float sweep = 0.085f + 0.025f * std::sin(2.0f * kPi * 1.0f * t);
            lp_l1 += sweep * (n_l - lp_l1);
            lp_l2 += sweep * (lp_l1 - lp_l2);
            lp_r1 += sweep * (n_r - lp_r1);
            lp_r2 += sweep * (lp_r1 - lp_r2);
            float flutter = 0.06f * std::sin(2.0f * kPi * 14.0f * t) * (n_l + n_r) * 0.5f;
            // Crossfade loop ends for click-free seamless looping
            float edge = std::min(std::min(t / 0.05f, (kWindDur - t) / 0.05f), 1.0f);
            left[i]  = (lp_l2 * 1.45f + flutter) * edge;
            right[i] = (lp_r2 * 1.45f - flutter) * edge;
        }
        SoundClip clip;
        clip.name = "FX_RunWind";
        clip.full_path = "FX_RunWind";
        clip.sample_rate = kSampleRate;
        clip.channels = 2;
        clip.duration = kWindDur;
        clip.pcm_data = float_stereo_to_pcm16(left, right);
        sound_clips_["FX_RunWind"] = clip;
    }

    // 12. FX_WindGust (450ms high-speed aerodynamic wind surge when reaching max speed)
    {
        constexpr float kGustDur = 0.45f;
        size_t samples = static_cast<size_t>(kGustDur * kSampleRate);
        std::vector<float> left(samples), right(samples);
        float lp_l = 0.0f, lp_r = 0.0f;
        for (size_t i = 0; i < samples; ++i) {
            float t = static_cast<float>(i) / kSampleRate;
            float u = t / kGustDur;
            float env = std::sin(kPi * std::pow(u, 0.65f));
            float cutoff = 0.06f + 0.14f * std::sin(kPi * u);
            float n_l = rand_normalized() * 2.0f - 1.0f;
            float n_r = rand_normalized() * 2.0f - 1.0f;
            lp_l += cutoff * (n_l - lp_l);
            lp_r += cutoff * (n_r - lp_r);
            float whistle = 0.08f * std::sin(2.0f * kPi * (520.0f + 180.0f * u) * t) * env;
            left[i]  = (lp_l * 1.35f + whistle) * env * 0.75f;
            right[i] = (lp_r * 1.35f + whistle) * env * 0.75f;
        }
        SoundClip clip;
        clip.name = "FX_WindGust";
        clip.full_path = "FX_WindGust";
        clip.sample_rate = kSampleRate;
        clip.channels = 2;
        clip.duration = kGustDur;
        clip.pcm_data = float_stereo_to_pcm16(left, right);
        sound_clips_["FX_WindGust"] = clip;
    }

    // -------------------------------------------------------------------------
    // 4 Dynamic Solar Fields Music Stems (Seamless 5.0-second fallback loops)
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

    // 13. FX_FallDeathScream (1.1s high-speed terminal wind shriek + panicked vocal pitch drop)
    make_clip("FX_FallDeathScream", 1.10f, [](float t, size_t, size_t) -> std::pair<float, float> {
        float u = t / 1.10f;
        float env = std::sin(kPi * std::pow(u, 0.55f));
        float f0 = 760.0f - 290.0f * u + 12.0f * std::sin(2.0f * kPi * 7.5f * t);
        float vocal = (std::sin(2.0f * kPi * f0 * t) * 0.45f +
                       std::sin(2.0f * kPi * f0 * 2.0f * t) * 0.28f +
                       std::sin(2.0f * kPi * f0 * 3.0f * t) * 0.14f) * env;
        float wind = (rand_normalized() * 2.0f - 1.0f) * (0.25f + 0.45f * u) * env;
        float s = (vocal + wind) * 0.72f;
        return {s * 0.96f, s * 1.04f};
    });

    // 14. FX_FallDeathImpact (1.25s lethal ground impact thud + bone crunch + ear tinnitus)
    make_clip("FX_FallDeathImpact", 1.25f, [](float t, size_t, size_t) -> std::pair<float, float> {
        float sub_thud = std::sin(2.0f * kPi * (58.0f * std::exp(-4.0f * t)) * t) * std::exp(-7.5f * t) * 0.95f;
        float crunch = (rand_normalized() * 2.0f - 1.0f) * std::exp(-22.0f * t) * 0.85f;
        float ring = std::sin(2.0f * kPi * 3840.0f * t) * std::exp(-2.2f * t) * 0.14f;
        float s = std::clamp(sub_thud + crunch + ring, -1.0f, 1.0f);
        return {s, s};
    });
}

} // namespace me

#pragma clang diagnostic pop
