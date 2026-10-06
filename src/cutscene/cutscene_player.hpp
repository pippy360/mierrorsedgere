#pragma once

#include "../math/types.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace me {

enum class ECutsceneMode : uint8_t {
    None = 0,
    BinkVideo = 1,        // Full-screen Bink (.bik) HD video + 48kHz stereo Bink audio + synced subtitles
    InEngineMatinee = 2   // Real-time 3D Matinee camera fly-in into Faith's 1P view with letterbox bars
};

struct SubtitleCue {
    float start_sec = 0.0f;
    float end_sec = 0.0f;
    std::string key;
    std::string text;
};

struct MatineeKeyframe {
    float time = 0.0f;
    Vec3 position{0.0f, 0.0f, 0.0f};
    float yaw_deg = 0.0f;
    float pitch_deg = 0.0f;
    float roll_deg = 0.0f;
    float fov_deg = 90.0f;
};

class CutscenePlayer {
public:
    CutscenePlayer();
    ~CutscenePlayer();

    CutscenePlayer(const CutscenePlayer&) = delete;
    CutscenePlayer& operator=(const CutscenePlayer&) = delete;

    // Initialize with game root (loads Subtitles.INT and discovers TdGame/Movies/*.bik)
    bool init(const std::string& game_root, bool headless = false);

    // Shutdown active decoders and SDL2 streaming audio device
    void shutdown();

    // Start playing a Bink (.bik) movie by name (e.g. "Scene_01", "StartupMovie", "Attract_Movie")
    bool play_bink_movie(const std::string& movie_name, bool chain_in_engine_intro = false);

    // Start playing an in-engine 3D Matinee camera fly-in ending at Faith's spawn viewpoint
    void play_in_engine_intro(const LevelScene& scene, const PlayerTelemetry& telemetry, float duration_sec = 5.5f);

    // Stop / skip current cutscene immediately (if Bink movie has chain_in_engine_intro, transitions or skips cleanly)
    void stop();

    // Advance active cutscene playback by dt seconds; updates camera override if InEngineMatinee is active
    void update(float dt, const LevelScene& scene, PlayerTelemetry& io_telemetry);

    // Deterministic seek & decode helper for --verify-all Oracle tests
    bool seek_and_decode_bink_frame(float target_sec);

    // Query active state
    [[nodiscard]] bool is_playing() const { return mode_ != ECutsceneMode::None; }
    [[nodiscard]] ECutsceneMode get_mode() const { return mode_; }
    [[nodiscard]] const std::string& get_movie_name() const { return current_movie_name_; }
    [[nodiscard]] float get_current_time() const { return elapsed_sec_; }
    [[nodiscard]] float get_duration() const { return duration_sec_; }
    [[nodiscard]] const std::string& get_active_subtitle() const { return active_subtitle_; }
    [[nodiscard]] float get_letterbox_amount() const { return letterbox_amount_; }

    // Decoded RGBA8 video frame access for MetalRenderer upload
    [[nodiscard]] int get_video_width() const { return video_width_; }
    [[nodiscard]] int get_video_height() const { return video_height_; }
    [[nodiscard]] uint64_t get_frame_serial() const { return frame_serial_; }
    [[nodiscard]] const std::vector<uint8_t>& get_rgba_frame() const { return rgba_buffer_; }
    [[nodiscard]] uint64_t get_decoded_audio_samples() const { return total_audio_samples_; }

    // Cycle through all 12 campaign & bonus Bink cutscenes (for interactive [C] key testing)
    bool cycle_next_cutscene(const LevelScene& scene, const PlayerTelemetry& telemetry);

    // Lookup the official chapter intro movie from DefaultEngine.ini [LoadMovies]
    [[nodiscard]] static std::string get_chapter_intro_movie(const std::string& map_name);

    // Count how many valid .bik movies exist in TdGame/Movies
    [[nodiscard]] size_t get_available_movie_count() const { return movie_files_.size(); }

private:
    bool open_bink_streams(const std::string& bik_path);
    void close_bink_streams();
    void decode_until_time(float target_sec);
    void load_subtitles_int(const std::string& game_root);
    void load_movie_subtitle_cues(const std::string& txt_path);

    std::string game_root_;
    bool headless_ = false;
    ECutsceneMode mode_ = ECutsceneMode::None;
    std::string current_movie_name_;
    bool chain_in_engine_after_bink_ = false;

    float elapsed_sec_ = 0.0f;
    float duration_sec_ = 0.0f;
    float letterbox_amount_ = 0.0f;
    std::string active_subtitle_;

    // Discovered case-insensitive movie name -> full .bik path
    std::unordered_map<std::string, std::string> movie_files_;
    // Localized Subtitles.INT key -> dialogue text
    std::unordered_map<std::string, std::string> subtitle_table_;
    // Active movie subtitle cues sorted by timestamp
    std::vector<SubtitleCue> current_cues_;

    // Decoded video frame state
    int video_width_ = 0;
    int video_height_ = 0;
    uint64_t frame_serial_ = 0;
    double last_video_pts_sec_ = -1.0;
    std::vector<uint8_t> rgba_buffer_;
    uint64_t total_audio_samples_ = 0;

    // In-engine 3D Matinee camera keyframes
    std::vector<MatineeKeyframe> matinee_keys_;
    int cycle_index_ = -1;

    // Opaque FFmpeg + SDL2 audio state
    struct DecoderImpl;
    DecoderImpl* dec_ = nullptr;
};

} // namespace me
