#pragma once

#include "../math/types.hpp"
#include "attenuation.hpp"
#include "sound_groups.hpp"
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

    // The front end: the Main Menu theme (A_M_Menu.upk) in place of the chapter's Solar Fields
    // stems, and the level's own sounds stopped. Retail's front end is a map of its own, so nothing
    // of a level sounds behind it; it is not a paused game (TdHUD.PauseGame returns at once there).
    void set_menu_music(bool active);
    [[nodiscard]] const std::string& get_active_music_bank() const { return active_music_bank_; }

    // The game paused (the pause menu, a hint card: WorldInfo.Pauser set), as retail's audio device
    // takes it. Every source that is not playing a sound of a UI sound group (HudSFX, DialogueHUD,
    // HudMusic, InGameMusic, TimeTrialMusic) is paused where it is and played on from there when the
    // pause ends (UAudioDevice::HandlePause, 0x00B652B0); while it lasts no such sound is started or
    // updated and none of their clocks runs (UAudioDevice::StartSources, 0x00B6EFE4): what is asked
    // for meanwhile starts with the first update after it. The level's music plays on, at the volume
    // TdPlayerController.SetPause gives its group: 0.25 paused, 1.0 when it is let go.
    void set_game_paused(bool paused);
    [[nodiscard]] bool is_game_paused() const { return game_paused_; }
    // The Volume of the sound group InGameMusic now: the ini's 1.2 until the first set_game_paused.
    [[nodiscard]] float ingame_music_volume() const { return ingame_music_volume_; }

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
    // per wave of a level emitter that has a source. It is the same with and without an audio device,
    // so the oracle can hold the gains against the curves; ME_AUDIO_DEBUG=1 prints them as they change.
    struct PositionalVoice {
        std::string name;            // the cue (or wave) asked for
        std::string clip;            // the wave playing
        Vec3 position{0.0f, 0.0f, 0.0f};
        float distance = 0.0f;       // from the listener of the last update (uu)
        float distance_gain = 1.0f;  // the product of its attenuation nodes' factors
        float gain = 0.0f;           // what the source is set to
        // What the source's low-pass filter is set to (AL_LOWPASS_GAINHF): the product of its
        // attenuation nodes' low passes, held between 0.01 and 1. Worked out with and without EFX.
        float high_frequency_gain = 1.0f;
        bool spatialized = false;    // placed in the world; false: source-relative, at the listener
        size_t attenuation_nodes = 0;
        bool emitter = false;        // a level AmbientSound
        int emitter_index = -1;      // which one (get_ambient_emitters)
        int emitter_wave = -1;       // and which of the waves its cue's graph has reached
        float priority = 0.0f;       // an emitter's wave: what it is ranked by for a source
        bool looping = false;        // an emitter's wave: it loops inside its source (false: once a round)
        bool sourced = true;         // it has a source (false only in emitter_candidates)
        float age = 0.0f;            // seconds it has been playing on its source
        bool paused = false;         // paused with the game
    };
    [[nodiscard]] std::vector<PositionalVoice> positional_voices() const;
    // Every wave of a level emitter that asked for a source in the last update (its volume above
    // retail's threshold of 0.0001), with and without one: the oracle holds the dealing against the rule.
    [[nodiscard]] std::vector<PositionalVoice> emitter_candidates() const;
    // How many level emitters have stopped for good (nothing left in their cue to play).
    [[nodiscard]] size_t stopped_emitter_count() const;
    // How many sources the level emitters' waves had between them in the last update: what the
    // port's other sounds left of retail's 32 (or of what the device gave).
    [[nodiscard]] size_t emitter_source_budget() const { return emitter_budget_; }
    // True when the low pass with distance reaches the device (it has the EFX extension).
    [[nodiscard]] bool has_low_pass_filters() const { return efx_; }

    // The master volume, as the listener's gain. At 0 the device is silent while everything plays.
    void set_master_volume(float volume);
    // What the audio device is doing now, in OpenAL's own words, for --audio-device-check.
    struct DeviceReport {
        bool device = false;          // sounds reach an OpenAL device
        std::string name;             // the device OpenAL opened
        bool low_pass = false;        // it has EFX and gave every source its low-pass filter
        size_t emitter_sources = 0;   // how many of the emitters' sources it gave
        size_t emitters_playing = 0;  // the emitters' sources that are AL_PLAYING
        size_t emitters_paused = 0;   // and AL_PAUSED
        size_t pool_playing = 0;      // the pool's
        size_t pool_paused = 0;
        size_t music_playing = 0;     // the music stems'
        bool wind_paused = false;     // the run wind's source is AL_PAUSED
        bool error = false;           // OpenAL has had an error since the last report
    };
    [[nodiscard]] DeviceReport device_report() const;

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
        bool ui = false;               // of a UI sound group: its delay runs on while the game is paused
    };
    // A 2D sound or a voice line asked for while the game was paused: it starts when the pause ends.
    struct DeferredSound {
        std::string name;
        float volume = 1.0f;
        float pitch = 1.0f;
        bool voice = false;
    };
    // A sound asked for at a place, waiting for the update that knows where the listener is.
    struct PositionalRequest {
        std::string name;
        Vec3 position{0.0f, 0.0f, 0.0f};
        float volume = 1.0f;
        float pitch = 1.0f;
    };
    // What a pool source is playing. When it is a sound at a place, update() gives it the gain and
    // the low pass its distance from the listener asks for, every time, as retail's node graph does.
    struct SourceVoice {
        bool ui = false;               // a sound of a UI sound group: the game's pause leaves it alone
        bool paused = false;           // paused with the game (FALSoundSource::Pause, 0x010D67D0)
        bool positional = false;
        Vec3 position{0.0f, 0.0f, 0.0f};
        float volume = 1.0f;           // before the distance: the cue's, its nodes' and the bus's
        VoiceAttenuation attenuation;
        float remaining = 0.0f;        // without a device: the seconds it would still play
        float logged_gain = -1.0f;     // ME_AUDIO_DEBUG: the gain last printed
        PositionalVoice info;
    };
    // The radii of one attenuation node drawn for one play (and a TdSoundNodeAttenuation's SpeedOfSound).
    struct DrawnRadii {
        float min_radius = 0.0f;
        float max_radius = 0.0f;
        float lpf_min_radius = 0.0f;
        float lpf_max_radius = 0.0f;
        float speed_of_sound = 0.0f;
    };
    [[nodiscard]] DrawnRadii draw_radii(const SoundAttenuation& a) const;
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
    // `path` is the walk's own list of the attenuation nodes above `node`; pass it empty.
    void collect_cue_voices(const SoundCueDef& cue, int node, float delay, float volume, float pitch,
                            std::vector<CueVoice>& out, std::vector<int>& path) const;
    bool play_layered_cue(const std::string& name, const Vec3* world_pos, float volume, float pitch);
    // Starts a wave on a pool source: in 2D, or at `world_pos` with `attenuation`. `ui`: a sound of
    // a UI sound group, which a paused game does not pause.
    void start_voice(const SoundClip& clip, const Vec3* world_pos, float volume, float pitch,
                     const VoiceAttenuation* attenuation = nullptr, const std::string& name = std::string(), bool ui = false);
    // True for a cue whose sound group has bIsUISound (false for a wave with no cue, or no such cue).
    [[nodiscard]] bool is_ui_sound(const std::string& name) const;
    // What a level's sounds are when the front end takes over: stopped, not paused.
    void stop_level_sounds();
    // The low pass of a source: `high_frequency_gain` on its filter and the filter on the source
    // (FALAudioEffectsManager::UpdateEffect, 0x010DBCB0). Nothing without EFX.
    void set_source_low_pass(uint32_t src, uint32_t filter, float high_frequency_gain);
    // No filter on a source that plays a sound with no place.
    void clear_source_low_pass(uint32_t src);

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

    // The level's AmbientSound emitters (the actors of the *_Aud.me1 sublevels).
    //
    // Retail starts each one's cue when its level comes in and parses the cue's graph on every audio
    // update, as it does every playing sound's (UAudioComponent::UpdateWaveInstances, 0x00B6F570).
    // Each wave the parse reaches is a wave instance of its own, with the volume, pitch, low pass
    // and place that the nodes above it on its own branch have left (USoundNodeWave::ParseNodes,
    // 0x00B77880; a mixer parses every input from the state it was reached with, 0x00B767A0): a
    // vent's hiss and its hum are two voices, each on its own curve. The device then gives its
    // sources to the instances of the highest PlayPriority (UAudioDevice::Update, 0x00B70440).
    // The port keeps the same per emitter: a payload for every node of its cue, a record for every
    // wave its parses have reached, and one pool of sources for the waves of all the emitters.
    //
    // Retail has MaxChannels sources for everything that sounds, 32 in the ini (UALAudioDevice::Init,
    // 0x010D9760), and ranks every wave for them. The port's other sounds (the one-shots of the pool,
    // a voice line, the run wind, the music) are not ranked: they have sources of their own, and what
    // they are using of the 32 at an update is what the emitters' waves do not get (count_other_voices).
    static constexpr size_t kMaxChannels = 32;
    static constexpr size_t kEmitterSourceBudget = kMaxChannels;
    uint32_t emitter_sources_[kEmitterSourceBudget] = {0};
    uint32_t emitter_filters_[kEmitterSourceBudget] = {0};       // their low-pass filters (EFX)
    size_t emitter_source_count_ = kEmitterSourceBudget;         // how many of them the device gave
    size_t emitter_budget_ = kEmitterSourceBudget;               // how many the last update had to deal
    [[nodiscard]] size_t count_other_voices() const;
    struct EmitterSourceUse {
        int32_t emitter = -1;
        int32_t wave = -1;
    };
    EmitterSourceUse emitter_source_use_[kEmitterSourceBudget];  // the wave each one is playing

    // A node's payload for the emitter's play (UAudioComponent::SoundNodeData).
    struct EmitterNode {
        bool requires_init = true;   // RequiresInit: the next parse draws its values again
        float value = 0.0f;          // Delay: the wait drawn (s); Modulator, Ambient: the volume drawn; Looping: UsedLoopCount
        float pitch = 1.0f;          // Modulator, Ambient: the pitch drawn
        double start = 0.0;          // Delay: the game time its wait began at
        int pick = -1;               // Random: the input chosen; Looping: FinishedCount
        int wave = -1;               // Wave: its record in EmitterState::waves
    };
    // A wave one of the emitter's parses has reached (FWaveInstance).
    struct EmitterWave {
        int node = -1;               // its wave node in the cue (-1: the one wave of an emitter with no cue)
        std::string clip;            // sound_clips_ key
        int hook = -1;               // the SoundNodeLooping that is told when it ends (CurrentNotifyBufferFinishedHook)
        bool seamless = false;       // LoopingMode 1: it loops inside its source
        bool finished = false;       // bIsFinished: it has played to its end and no loop has put it back
        bool parsed = false;         // the last parse reached it
        bool wants_source = false;   // and its volume was above the threshold
        bool active = false;         // and it ranked among the waves the sources go to
        float graph_volume = 1.0f;   // CurrentVolume before the distance: mixer inputs, modulators, mix groups
        float pitch = 1.0f;          // CurrentPitch
        std::vector<int> path;       // the attenuation nodes above it (SoundCueDef::attenuations), root first
        VoiceAttenuation attenuation;
        uint32_t radii_stamp = 0;    // EmitterState::radii_stamp when `attenuation` was made
        int source = -1;             // the emitter source it holds
        float remaining = 0.0f;      // without a device: the seconds its pass would still play
        float offset = 0.0f;         // with one, a looping wave: where its source was at the last update (s)
        float logged_gain = -1.0f;   // ME_AUDIO_DEBUG: the gain last printed
        PositionalVoice info;        // the last update's numbers
    };
    struct EmitterState {
        bool started = false;        // its cue has been looked up
        bool has_cue = false;
        // The component has stopped: a parse found nothing in the graph playing or waiting
        // (UAudioComponent::UpdateWaveInstances, 0x00B6FCF1). Nothing starts a level's emitter again.
        bool dead = false;
        size_t node_count = 0;       // of the cue it was started with
        std::vector<DrawnRadii> radii;       // by SoundCueDef::attenuations index
        std::vector<uint8_t> radii_drawn;    // the node's own RequiresInit
        uint32_t radii_stamp = 0;            // counts the draws
        std::vector<EmitterNode> nodes;      // by SoundCueDef::nodes index
        std::vector<EmitterWave> waves;
    };
    std::vector<EmitterState> emitter_states_;                   // by ambient_emitters_ index
    // What the nodes above have left for the ones below (FAudioComponentSavedState).
    struct EmitterParse {
        float volume = 1.0f;         // CurrentVolume
        float pitch = 1.0f;          // CurrentPitch
        bool seamless = false;       // CurrentUseSeamlessLooping
        int hook = -1;               // CurrentNotifyBufferFinishedHook
    };
    void parse_emitter_node(EmitterState& st, const SoundCueDef& cue, int node, EmitterParse state, std::vector<int>& path,
                            bool& finished);
    // A wave's source has played to its end (FWaveInstance::NotifyFinished, 0x00B64640).
    void emitter_wave_finished(EmitterState& st, const SoundCueDef* cue, int wave);
    void stop_emitter_source(size_t source);
    // Every emitter silent and as it is before its first parse.
    void reset_emitters();
    // The game's clock, WorldInfo.TimeSeconds: what a SoundNodeDelay counts on (0x00B7F2A0). It
    // stands still while the game is paused and behind the front end.
    double game_time_ = 0.0;

    // Pause (set_game_paused).
    bool game_paused_ = false;
    float ingame_music_volume_ = kInGameMusicIniVolume;          // the sound group InGameMusic's Volume
    // The Volume of the sound group of the music that is playing: a level's is InGameMusic's (the
    // stems' fixed level stands for its 1.0); the menu's theme is HudMusic's, which no pause touches.
    [[nodiscard]] float music_group_volume() const { return is_menu_music_ ? 1.0f : ingame_music_volume_; }
    bool wind_paused_ = false;
    bool vo_paused_ = false;
    bool vo_ui_ = false;                                         // the line playing is of a UI sound group (DialogueHUD)
    std::vector<DeferredSound> deferred_sounds_;                 // asked for while paused

    // The low pass with distance (EFX): one low-pass filter for every pool source.
    bool efx_ = false;
    uint32_t source_filters_[kSourcePoolSize] = {0};

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
