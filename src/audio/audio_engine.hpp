#pragma once

#include "../math/types.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <cstdint>

namespace me {

enum class EAudioEffect : uint8_t {
    Footstep = 0,
    Jump,
    Wallrun,
    Vault,
    Slide,
    SkillRoll,
    Zipline,
    CheckpointChime,
    Disarm,
    Gunshot,
    Count
};

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    // Initialize OpenAL context and audio pools (headless skips hardware AL device)
    bool init(bool headless = false);

    // Shutdown and release OpenAL resources
    void shutdown();

    [[nodiscard]] bool is_initialized() const { return initialized_; }
    [[nodiscard]] bool is_headless() const { return headless_; }

    // Update 3D listener orientation and dynamic Solar Fields stems
    void update(float dt,
                const Vec3& listener_pos,
                const Vec3& listener_forward,
                const Vec3& listener_up,
                float player_speed,
                bool reaction_active);

    // Load real Ogg Vorbis audio banks from game CookedPC/Audio directory
    bool load_sound_bank(const std::string& game_root, const std::string& bank_name);
    bool load_stock_audio(const std::string& game_root);

    // Playback APIs
    void play_sound(const std::string& name, float volume = 1.0f, float pitch = 1.0f);
    void play_sound_3d(const std::string& name, const Vec3& world_pos, float volume = 1.0f, float pitch = 1.0f);
    void play_effect(EAudioEffect effect, float volume = 1.0f, float pitch = 1.0f);
    void play_effect_3d(EAudioEffect effect, const Vec3& world_pos, float volume = 1.0f, float pitch = 1.0f);

    // Dynamic 4-track Solar Fields stem volume control (0.0 to 1.0)
    void set_music_stems(float ambient_vol, float tension_vol, float chase_vol, float reaction_vol);

    // Stop all playing sound sources
    void stop_all();

    // Access loaded clip
    [[nodiscard]] const SoundClip* get_clip(const std::string& name) const;
    [[nodiscard]] size_t get_clip_count() const { return sound_clips_.size(); }

    // Synthesize procedural fallback clips for 100% offline/fallback reliability
    void synthesize_fallback_clips();

private:
    bool init_openal();
    void cleanup_openal();
    uint32_t acquire_source();
    uint32_t get_or_create_buffer(const SoundClip& clip);

    bool decode_ogg_to_pcm(const uint8_t* ogg_data, size_t ogg_size,
                           std::vector<int16_t>& out_pcm, int& out_rate, int& out_channels);

    bool initialized_ = false;
    bool headless_ = false;

    // OpenAL context handles (opaque pointers)
    void* alc_device_ = nullptr;
    void* alc_context_ = nullptr;

    // Source pool (32 spatial sources)
    static constexpr size_t kSourcePoolSize = 32;
    uint32_t sources_[kSourcePoolSize] = {0};
    size_t next_source_ = 0;

    // 4 Dynamic Solar Fields music stem sources:
    // 0: Ambient, 1: Tension, 2: Chase, 3: Reaction
    uint32_t music_stem_sources_[4] = {0};
    uint32_t music_stem_buffers_[4] = {0};
    float current_stem_vols_[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    float target_stem_vols_[4] = {1.0f, 0.0f, 0.0f, 0.0f};

    // Sound clips and OpenAL buffer cache
    std::unordered_map<std::string, SoundClip> sound_clips_;
    std::unordered_map<std::string, uint32_t> al_buffers_;
};

} // namespace me
