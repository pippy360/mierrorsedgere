#pragma once

#include "../math/types.hpp"
#include <string>
#include <utility>
#include <vector>
#include <unordered_map>
#include <unordered_set>
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
    FallDeathScream,
    FallDeathImpact,
    Count
};

// Matches [Engine.AudioDevice] +SoundGroupEffects presets 0..10 in DefaultEngine.ini
enum class ESoundGroupEffectMode : uint8_t {
    Normal = 0,
    IngameCutScenes = 1,
    IngameVO = 2,
    ReactionTime = 3,
    Pause = 4,
    AllTurnedOff = 5,
    FallingToDeath = 6,
    CustomCutsceneTrack = 7,
    DeathByFall = 8,
    DeathGeneric = 9,
    EndCredit = 10
};

// Matches the 9 physical surface material packages in A_Material_Footstep.upk
enum class ESurfaceMaterial : uint8_t {
    Concrete = 0,
    Metal,
    MetalGantry,
    MetalAirduct,
    MetalLadder,
    Wood,
    Glass,
    Water,
    Cardboard,
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

    // Update 3D listener orientation, global SoundGroupEffects ducking,
    // TdSoundNodeVelocity (RunWind), Faith breathing cadence, 3D level ambients,
    // and dynamic Solar Fields music stems
    void update(float dt,
                const Vec3& listener_pos,
                const Vec3& listener_forward,
                const Vec3& listener_up,
                float player_speed,
                bool reaction_active);

    // Load real Ogg Vorbis audio banks & SoundCue graphs from game CookedPC/Audio directory
    bool load_sound_bank(const std::string& game_root, const std::string& bank_name);
    bool load_stock_audio(const std::string& game_root);

    // Load streaming level audio sublevel (*_Aud.me1) and chapter Solar Fields music stems
    bool load_level_audio(const std::string& game_root, const std::string& map_file);

    // Playback APIs (supports both direct wave names and UE3 SoundCue names/paths)
    void play_sound(const std::string& name, float volume = 1.0f, float pitch = 1.0f);
    void play_sound_3d(const std::string& name, const Vec3& world_pos, float volume = 1.0f, float pitch = 1.0f);
    void play_vo(const std::string& name, float volume = 1.0f);
    void play_level_loaded_cues();
    void play_effect(EAudioEffect effect, float volume = 1.0f, float pitch = 1.0f);
    void play_effect_3d(EAudioEffect effect, const Vec3& world_pos, float volume = 1.0f, float pitch = 1.0f);

    // Surface-aware TdPhysicalMaterialFootSteps / HandSteps playback
    void play_footstep(ESurfaceMaterial surface, float speed, bool crouch, float volume = 0.75f);
    // The footstep an AnimNotify_Footstep asks for by number: 1 Sneak, 2 Walk, 3 Run, 4 Sprint,
    // 5 SprintRelease, 6 WallRun, 7 WallrunRelease, 8 LandSoft, 9 LandMedium, 10 LandHard, 11 Slide.
    void play_footstep_number(ESurfaceMaterial surface, int number, float volume = 0.75f);
    // A SoundCue named as the level data names it, "Group.Name" (or just "Name"); `voice` plays it
    // as dialogue, with its subtitle. Returns false if no such cue is loaded.
    bool play_cue(const std::string& group_and_name, bool voice = false, float volume = 1.0f);
    void play_handstep(ESurfaceMaterial surface, bool hard_impact, float volume = 0.75f);

    // Global SoundGroupEffects mix state (DefaultEngine.ini modes 0..10)
    void set_sound_group_mode(ESoundGroupEffectMode mode);
    [[nodiscard]] ESoundGroupEffectMode get_sound_group_mode() const { return sound_mode_; }

    // Dynamic 4-track Solar Fields stem volume control (0.0 to 1.0)
    void set_music_stems(float ambient_vol, float tension_vol, float chase_vol, float reaction_vol);

    // Switch between Main Menu theme (A_M_Menu.upk) and active chapter Solar Fields stems
    void set_menu_music(bool active);
    [[nodiscard]] const std::string& get_active_music_bank() const { return active_music_bank_; }

    // Stop all playing sound sources
    void stop_all();

    // SoundCues by "Group.Name" (or bare name), as Kismet and animation notifies refer to them.
    [[nodiscard]] bool has_cue(const std::string& group_and_name) const;
    // Loads the content package a cue lives in ("A_Props_Interactive", "A_VO_CS01_CUE") from
    // CookedPC/Audio or its int/ voice folder, once. A voice cue package brings its waves along.
    bool load_cue_bank(const std::string& game_root, const std::string& package);
    // Stops every source that is playing that cue.
    void stop_cue(const std::string& group_and_name);

    // A log of every sound asked to play (the cue or wave name it resolved from), for --trace.
    // It records the request, so it is the same with and without an audio device.
    struct PlayEvent {
        std::string name;
        const char* kind = "sound";  // "sound", "sound3d" or "vo"
        float duration = 0.0f;       // of the wave that was picked
    };
    void set_play_log(bool on) { play_log_on_ = on; }
    [[nodiscard]] std::vector<PlayEvent> take_play_log() { return std::exchange(play_log_, {}); }

    // Access loaded clips, cues, level-loaded VO cues, and spatial ambient emitters
    [[nodiscard]] const SoundClip* get_clip(const std::string& name) const;
    [[nodiscard]] bool is_vo_playing() const { return vo_duration_ > 0.0f && vo_elapsed_ < vo_duration_; }
    [[nodiscard]] const std::string& get_active_vo_subtitle() const { return active_vo_subtitle_; }
    [[nodiscard]] const std::vector<std::string>& get_level_loaded_cues() const { return level_loaded_cues_; }
    [[nodiscard]] size_t get_clip_count() const { return sound_clips_.size(); }
    [[nodiscard]] size_t get_cue_count() const { return sound_cues_.size(); }
    [[nodiscard]] size_t get_ambient_emitter_count() const { return ambient_emitters_.size(); }

    // Synthesize procedural fallback clips for 100% offline/fallback reliability
    void synthesize_fallback_clips();

private:
    bool init_openal();
    void cleanup_openal();
    uint32_t acquire_source();
    uint32_t get_or_create_buffer(const SoundClip& clip, bool force_mono_for_3d = false);
    void invalidate_cached_buffer(const std::string& key);
    void clear_chapter_music_clips();
    void rebind_music_stem_buffers();
    void stitch_concatenator_cues();

    bool load_package_audio_and_cues(const std::string& pkg_path,
                                     std::vector<std::string>* out_clip_keys = nullptr,
                                     bool extract_level_loaded = false);
    const SoundClip* resolve_cue_or_clip(const std::string& name, float& io_vol, float& io_pitch) const;
    const SoundClip* pick_first_available_clip(std::initializer_list<const char*> candidates) const;

    bool decode_ogg_to_pcm(const uint8_t* ogg_data, size_t ogg_size,
                           std::vector<int16_t>& out_pcm, int& out_rate, int& out_channels);

    bool initialized_ = false;
    bool headless_ = false;
    bool play_log_on_ = false;
    std::unordered_set<std::string> cue_banks_tried_;
    std::vector<PlayEvent> play_log_;
    bool is_menu_music_ = false;
    std::string active_music_bank_;
    std::vector<std::string> active_music_clip_keys_;

    // OpenAL context handles (opaque pointers)
    void* alc_device_ = nullptr;
    void* alc_context_ = nullptr;

    // Source pool (32 spatial sources)
    static constexpr size_t kSourcePoolSize = 32;
    uint32_t sources_[kSourcePoolSize] = {0};
    size_t next_source_ = 0;

    // 4 Dynamic Solar Fields music stem sources:
    // 0: Ambient (ambience_01 / Menu), 1: Tension/Puzzle (ambience_011 / Puzzle_01),
    // 2: Chase (chase_01), 3: Combat/Reaction (combat_01 / Stem_3)
    uint32_t music_stem_sources_[4] = {0};
    uint32_t music_stem_buffers_[4] = {0};
    std::string music_stem_clip_names_[4] = {"Stem_0", "Stem_1", "Stem_2", "Stem_3"};
    float current_stem_vols_[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    float target_stem_vols_[4] = {1.0f, 0.0f, 0.0f, 0.0f};

    // Dedicated continuous TdSoundNodeVelocity source for 1P RunWind + max-speed wind surge
    uint32_t run_wind_source_ = 0;
    float current_wind_vol_ = 0.0f;
    float wind_surge_env_ = 0.0f;
    bool max_speed_wind_active_ = false;

    // Dedicated non-stealable 2D radio/dialogue VO source (DialogueRadio / DialogueFaith / DialogueOther)
    uint32_t vo_source_ = 0;
    float vo_elapsed_ = 0.0f;
    float vo_duration_ = 0.0f;
    std::string active_vo_clip_;
    std::string active_vo_subtitle_;

    // 4 Dedicated looping 3D sources for level *_Aud.me1 AmbientSound emitters
    static constexpr size_t kAmbientPoolSize = 4;
    uint32_t ambient_sources_[kAmbientPoolSize] = {0};
    int32_t active_ambient_indices_[kAmbientPoolSize] = {-1, -1, -1, -1};

    // Stamina-coupled Faith breathing cadence state (A_Character_Female_01.upk)
    float breath_timer_ = 0.0f;
    bool breath_inhale_next_ = true;

    // Global SoundGroupEffects ducking state (DefaultEngine.ini)
    ESoundGroupEffectMode sound_mode_ = ESoundGroupEffectMode::Normal;
    float sfx_bus_gain_ = 1.0f;
    float music_bus_gain_ = 1.0f;
    float breath_bus_gain_ = 1.0f;
    float slomo_pitch_scale_ = 1.0f;

    // Sound clips, UE3 SoundCue graphs, Kismet SeqEvent_LevelLoaded cues, level 3D ambients, and OpenAL buffer cache
    std::unordered_map<std::string, SoundClip> sound_clips_;
    std::unordered_map<std::string, SoundCueDef> sound_cues_;
    std::vector<std::string> level_loaded_cues_;
    std::vector<AmbientEmitterInfo> ambient_emitters_;
    std::unordered_map<std::string, uint32_t> al_buffers_;
};

} // namespace me
