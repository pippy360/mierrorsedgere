#pragma once

#include "../math/types.hpp"
#include "attenuation.hpp"
#include <cstddef>
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
    // A sound at a place in the world. The next update() starts it, when it knows where the
    // listener is: each wave the cue plays gets the fall-off with distance of the attenuation
    // nodes above it in the cue's graph, followed while it plays, and is placed in the world only
    // if one of them spatialises it. A cue that lasts at most a second is not started at all when
    // the listener is not within its MaxAudibleDistance, as UAudioDevice::CreateComponent refuses it.
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
    // The length in seconds of the wave the cue resolves to (0 when it is not loaded).
    [[nodiscard]] float cue_duration(const std::string& group_and_name) const;
    // Loads the content package a cue lives in ("A_Props_Interactive", "A_VO_CS01_CUE") from
    // CookedPC/Audio or its int/ voice folder, once. A voice cue package brings its waves along.
    bool load_cue_bank(const std::string& game_root, const std::string& package);
    // Stops every source that is playing that cue.
    void stop_cue(const std::string& group_and_name);
    // USoundCue::MaxAudibleDistance of a cue (uu): the largest MaxRadius of its attenuation nodes,
    // kSoundWorldMax when it has none or is not loaded. What APlayerController::HearSound holds
    // an Actor.PlaySound against, whatever the cue's length.
    [[nodiscard]] float cue_max_audible_distance(const std::string& group_and_name) const;

    // What the sounds at a place are doing now: one entry per wave playing from the pool, and one
    // per level emitter that has a source. It is the same with and without an audio device, so the
    // oracle can hold the gains against the curves; ME_AUDIO_DEBUG=1 prints them as they change.
    struct PositionalVoice {
        std::string name;            // the cue (or wave) asked for
        std::string clip;            // the wave playing
        Vec3 position{0.0f, 0.0f, 0.0f};
        float distance = 0.0f;       // from the listener of the last update (uu)
        float distance_gain = 1.0f;  // the product of its attenuation nodes' factors
        float gain = 0.0f;           // what the source is set to
        bool spatialized = false;    // placed in the world; false: source-relative, at the listener
        size_t attenuation_nodes = 0;
        bool emitter = false;        // a level AmbientSound
    };
    [[nodiscard]] std::vector<PositionalVoice> positional_voices() const;

    // A log of every sound asked to play (the cue or wave name it resolved from), for --trace.
    // It records the request, so it is the same with and without an audio device. A sound at a
    // place is entered by the update() that starts it, and not at all if it is refused there.
    struct PlayEvent {
        std::string name;
        const char* kind = "sound";  // "sound", "sound3d" or "vo"
        float duration = 0.0f;       // of the wave that was picked; a layered cue's longest layer, delay included
    };
    void set_play_log(bool on) { play_log_on_ = on; }
    [[nodiscard]] std::vector<PlayEvent> take_play_log() { return std::exchange(play_log_, {}); }

    // Access loaded clips, cues, level-loaded VO cues, and spatial ambient emitters
    [[nodiscard]] const SoundClip* get_clip(const std::string& name) const;
    // True when `name` (a SoundCue name / package-relative path, or a wave) resolves to loaded audio.
    [[nodiscard]] bool has_sound(const std::string& name) const;
    // Waves one play of `name` starts: every wave a layered (mixer) cue's graph reaches, 1 for any
    // other sound that resolves, 0 if it does not.
    [[nodiscard]] size_t count_sound_layers(const std::string& name) const;
    [[nodiscard]] bool is_vo_playing() const { return vo_duration_ > 0.0f && vo_elapsed_ < vo_duration_; }
    [[nodiscard]] const std::string& get_active_vo_subtitle() const { return active_vo_subtitle_; }
    [[nodiscard]] const std::vector<std::string>& get_level_loaded_cues() const { return level_loaded_cues_; }
    [[nodiscard]] size_t get_clip_count() const { return sound_clips_.size(); }
    [[nodiscard]] size_t get_cue_count() const { return sound_cues_.size(); }
    [[nodiscard]] size_t get_ambient_emitter_count() const { return ambient_emitters_.size(); }
    [[nodiscard]] const std::vector<AmbientEmitterInfo>& get_ambient_emitters() const { return ambient_emitters_; }
    // The cue a name ("Group.Name" or bare) is loaded as, null if none.
    [[nodiscard]] const SoundCueDef* find_cue(const std::string& name) const {
        const auto it = sound_cues_.find(name);
        return it != sound_cues_.end() ? &it->second : nullptr;
    }

    // Synthesize procedural fallback clips for 100% offline/fallback reliability
    void synthesize_fallback_clips();

private:
    bool init_openal();
    void cleanup_openal();
    // True when sounds reach an OpenAL device (false headless, and in a build without OpenAL).
    [[nodiscard]] bool has_device() const;
    // The pool slot a new sound gets: the first that is not playing, else the next in turn, whose
    // sound is cut. What the slot was playing is forgotten.
    size_t acquire_slot();
    uint32_t acquire_source();
    uint32_t get_or_create_buffer(const SoundClip& clip, bool force_mono_for_3d = false);
    void invalidate_cached_buffer(const std::string& key);
    void clear_chapter_music_clips();
    void rebind_music_stem_buffers();
    // Binds `buf` to `src`, stopping it first; false while the bind has not taken yet (Apple's OpenAL
    // applies a bind made just after stopping a playing source a moment later), to be checked again.
    bool bind_source_buffer(uint32_t src, uint32_t buf);
    void sync_music_stem(int i);
    void stitch_concatenator_cues();

    bool load_package_audio_and_cues(const std::string& pkg_path,
                                     std::vector<std::string>* out_clip_keys = nullptr,
                                     bool extract_level_loaded = false);
    // Decodes an extracted clip (Ogg Vorbis -> PCM) and files it under its name and full path.
    void store_clip(SoundClip clip, std::vector<std::string>* out_clip_keys);
    // SoundCues can play waves that live in another audio package (imports: A_Props_Interactive's
    // Doors.Door_Hit plays A_CXP_Plaza.Door_RAW.Door_Hit). UE3 loads the import's package with
    // the cue; this loads just the referenced waves that are not loaded yet.
    void load_imported_waves(const std::string& game_root);
    // `out_cue`, when given, is set to the cue the name resolved through (null for a bare wave).
    const SoundClip* resolve_cue_or_clip(const std::string& name, float& io_vol, float& io_pitch,
                                         const SoundCueDef** out_cue = nullptr) const;
    const SoundClip* pick_first_available_clip(std::initializer_list<const char*> candidates) const;

    // Layered cues (a SoundNodeMixer in the graph, e.g. Doors.Door_Barge = impact + a delayed
    // random bash) play every wave the graph reaches, after its delays and modulators.
    struct CueVoice {
        std::string clip;  // sound_clips_ key
        float delay = 0.0f;
        float volume = 1.0f;
        float pitch = 1.0f;
        std::vector<int> attenuations;  // the attenuation nodes above the wave (SoundCueDef::attenuations), root first
    };
    struct PendingVoice {
        CueVoice voice;
        bool positional = false;
        Vec3 position{0.0f, 0.0f, 0.0f};
        VoiceAttenuation attenuation;  // positional: what `voice.attenuations` came to for this play
        std::string name;              // the cue asked for
    };
    // A sound asked for at a place, waiting for the update that knows where the listener is.
    struct PositionalRequest {
        std::string name;
        Vec3 position{0.0f, 0.0f, 0.0f};
        float volume = 1.0f;
        float pitch = 1.0f;
    };
    // What a pool source is playing, when it is a sound at a place: update() gives it the gain its
    // distance from the listener asks for, every time, as retail's node graph does.
    struct SourceVoice {
        bool positional = false;
        Vec3 position{0.0f, 0.0f, 0.0f};
        float volume = 1.0f;           // before the distance: the cue's, its nodes' and the bus's
        VoiceAttenuation attenuation;
        float remaining = 0.0f;        // without a device: the seconds it would still play
        float logged_gain = -1.0f;     // ME_AUDIO_DEBUG: the gain last printed
        PositionalVoice info;
    };
    // A MinRadius and a MaxRadius drawn for one play (and a TdSoundNodeAttenuation's SpeedOfSound).
    struct DrawnRadii {
        float min_radius = 0.0f;
        float max_radius = 0.0f;
        float speed_of_sound = 0.0f;
    };
    // One play's draw for every attenuation node of the cue, in the order of SoundCueDef::attenuations.
    [[nodiscard]] std::vector<DrawnRadii> draw_cue_radii(const SoundCueDef& cue) const;
    // What the attenuation nodes `path` (root first) do to a wave played at `world_pos` with those
    // radii. A TdSoundNodeAttenuation's hold-back, its first distance over the speed of sound, is
    // added to `io_delay`.
    [[nodiscard]] VoiceAttenuation make_voice_attenuation(const SoundCueDef& cue, const std::vector<int>& path,
                                                          const std::vector<DrawnRadii>& radii, const Vec3& world_pos,
                                                          float* io_delay) const;
    // The attenuation nodes above the wave `wave` in the cue's graph, root first, where the graph
    // first reaches it; above the graph's first wave when it has no wave of that name (a
    // concatenator's stitched clip carries the cue's name).
    [[nodiscard]] std::vector<int> wave_attenuations(const SoundCueDef& cue, const std::string& wave) const;
    // UAudioDevice::CreateComponent's test (through USoundCue::IsAudibleSimple) for a cue at a place.
    [[nodiscard]] bool location_is_audible(const SoundCueDef& cue, const Vec3& at) const;
    void start_positional(const PositionalRequest& request);
    void update_voice_gains(float dt);
    void update_ambient_emitters(float dt);
    // A level AmbientSound whose cue waits between sounds (a SoundNodeDelay under its
    // SoundNodeLooping, e.g. the vehicle packs: 7 to 15 s, then one of 38 passes, brakes and
    // horns): the next sound it will play and how long until then. False for a cue that just loops.
    bool next_ambient_voice(size_t slot, const AmbientEmitterInfo& em);
    // `path` is the walk's own list of the attenuation nodes above `node`; pass it empty.
    void collect_cue_voices(const SoundCueDef& cue, int node, float delay, float volume, float pitch,
                            std::vector<CueVoice>& out, std::vector<int>& path) const;
    bool play_layered_cue(const std::string& name, const Vec3* world_pos, float volume, float pitch);
    // Starts a wave on a pool source: in 2D, or at `world_pos` with `attenuation`.
    void start_voice(const SoundClip& clip, const Vec3* world_pos, float volume, float pitch,
                     const VoiceAttenuation* attenuation = nullptr, const std::string& name = std::string());

    bool decode_ogg_to_pcm(const uint8_t* ogg_data, size_t ogg_size,
                           std::vector<int16_t>& out_pcm, int& out_rate, int& out_channels);

    bool initialized_ = false;
    bool headless_ = false;
    bool playback_started_ = false;
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
    SourceVoice source_voices_[kSourcePoolSize];              // what each is playing, when it is a sound at a place
    std::vector<PositionalRequest> positional_requests_;      // play_sound_3d calls since the last update
    Vec3 listener_pos_{0.0f, 0.0f, 0.0f};                     // the listener of the last update (uu)
    bool debug_log_ = false;                                  // ME_AUDIO_DEBUG

    // 4 Dynamic Solar Fields music stem sources:
    // 0: Ambient (ambience_01 / Menu), 1: Tension/Puzzle (ambience_011 / Puzzle_01),
    // 2: Chase (chase_01), 3: Combat/Reaction (combat_01 / Stem_3)
    uint32_t music_stem_sources_[4] = {0};
    uint32_t music_stem_buffers_[4] = {0};
    std::string music_stem_clip_names_[4] = {"Stem_0", "Stem_1", "Stem_2", "Stem_3"};
    float current_stem_vols_[4] = {0.0f, 0.0f, 0.0f, 0.0f};
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
    // What each of those sources is doing: looping its cue's wave, waiting out the cue's delay,
    // or playing the one sound that followed it.
    enum class AmbientMode : uint8_t { Loop, Waiting, Playing, Silent };
    AmbientMode ambient_mode_[kAmbientPoolSize] = {};
    CueVoice ambient_voice_[kAmbientPoolSize];
    VoiceAttenuation ambient_attenuation_[kAmbientPoolSize];  // of the wave each is playing
    bool ambient_sounding_[kAmbientPoolSize] = {};            // a wave was started on it
    float ambient_remaining_[kAmbientPoolSize] = {};          // without a device: the seconds a one-shot would still play
    float ambient_logged_gain_[kAmbientPoolSize] = {};        // ME_AUDIO_DEBUG: the gain last printed
    PositionalVoice ambient_info_[kAmbientPoolSize];
    // An emitter's cue is one play that starts with the level and lasts as long as it does: the
    // radii of its attenuation nodes are drawn once, the first time the emitter is looked at, and
    // not again when it comes back into the pool. `reach` is the largest MaxRadius drawn, past
    // which every wave of the cue is silent (kSoundWorldMax for a cue with no radius).
    struct EmitterRadii {
        bool drawn = false;
        bool has_cue = false;
        std::vector<DrawnRadii> radii;
        float reach = 0.0f;
    };
    std::vector<EmitterRadii> ambient_radii_;                 // by ambient_emitters_ index
    const EmitterRadii& emitter_radii(size_t emitter);

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
    std::vector<PendingVoice> pending_voices_;                 // layered cue waves waiting out their delay
    std::unordered_set<std::string> imported_wave_attempts_;   // "Package.Wave" imports already looked for
};

} // namespace me
