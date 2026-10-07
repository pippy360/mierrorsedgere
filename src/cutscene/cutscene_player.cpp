#include "cutscene_player.hpp"
#include "../anim/anim_system.hpp"
#include "../assets/upk_loader.hpp"

#include <SDL2/SDL.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
}

#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <iostream>

namespace fs = std::filesystem;

namespace me {

namespace {

static std::string to_lower_copy(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

static std::string trim_copy(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

static std::string read_text_file_any_encoding(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (raw.size() >= 2 &&
        static_cast<uint8_t>(raw[0]) == 0xFF &&
        static_cast<uint8_t>(raw[1]) == 0xFE) {
        // UTF-16LE -> UTF-8 / ASCII
        std::string out;
        out.reserve(raw.size() / 2);
        for (size_t i = 2; i + 1 < raw.size(); i += 2) {
            uint16_t ch = static_cast<uint16_t>(static_cast<uint8_t>(raw[i])) |
                          (static_cast<uint16_t>(static_cast<uint8_t>(raw[i + 1])) << 8);
            if (ch == 0) continue;
            if (ch < 0x80) {
                out.push_back(static_cast<char>(ch));
            } else if (ch < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (ch >> 6)));
                out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
            } else {
                out.push_back(static_cast<char>(0xE0 | (ch >> 12)));
                out.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
            }
        }
        return out;
    }
    return raw;
}

} // namespace

struct CutscenePlayer::DecoderImpl {
    AVFormatContext* fmt_ctx = nullptr;
    AVCodecContext* video_ctx = nullptr;
    AVCodecContext* audio_ctx = nullptr;
    SwsContext* sws_ctx = nullptr;
    SwrContext* swr_ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    int video_stream_idx = -1;
    int audio_stream_idx = -1;
    bool eof_reached = false;

    SDL_AudioDeviceID sdl_audio_dev = 0;
    int audio_sample_rate = 48000;
};

CutscenePlayer::CutscenePlayer() : dec_(new DecoderImpl()) {}

CutscenePlayer::~CutscenePlayer() {
    shutdown();
    delete dec_;
    dec_ = nullptr;
}

bool CutscenePlayer::init(const std::string& game_root, bool headless) {
    game_root_ = game_root;
    headless_ = headless;
    movie_files_.clear();

    // 1. Discover all Bink (.bik) movies in TdGame/Movies
    fs::path movies_dir = fs::path(game_root) / "TdGame" / "Movies";
    if (fs::exists(movies_dir)) {
        for (const auto& entry : fs::directory_iterator(movies_dir)) {
            if (!entry.is_regular_file()) continue;
            fs::path p = entry.path();
            if (to_lower_copy(p.extension().string()) == ".bik") {
                std::string stem_low = to_lower_copy(p.stem().string());
                movie_files_[stem_low] = p.string();
            }
        }
    }

    // 2. Load localized cutscene dialogue from TdGame/Localization/INT/Subtitles.INT
    load_subtitles_int(game_root);

    // 3. Open dedicated stereo 48kHz SDL2 audio queue for real-time Bink audio playback when windowed
    if (!headless_ && dec_->sdl_audio_dev == 0) {
        if ((SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) == 0) {
            SDL_InitSubSystem(SDL_INIT_AUDIO);
        }
        SDL_AudioSpec want{}, have{};
        want.freq = 48000;
        want.format = AUDIO_F32SYS;
        want.channels = 2;
        want.samples = 2048;
        want.callback = nullptr; // Push mode via SDL_QueueAudio
        dec_->sdl_audio_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (dec_->sdl_audio_dev != 0) {
            dec_->audio_sample_rate = (have.freq > 0) ? have.freq : 48000;
            SDL_PauseAudioDevice(dec_->sdl_audio_dev, 0);
        }
    }

    std::cout << "[Cutscene] Initialized Bink & Matinee Cutscene System ("
              << movie_files_.size() << " .bik movies, "
              << subtitle_table_.size() << " subtitle lines)" << std::endl;
    return !movie_files_.empty();
}

void CutscenePlayer::shutdown() {
    close_bink_streams();
    if (dec_ && dec_->sdl_audio_dev != 0) {
        SDL_ClearQueuedAudio(dec_->sdl_audio_dev);
        SDL_CloseAudioDevice(dec_->sdl_audio_dev);
        dec_->sdl_audio_dev = 0;
    }
    mode_ = ECutsceneMode::None;
}

void CutscenePlayer::load_subtitles_int(const std::string& game_root) {
    subtitle_table_.clear();
    fs::path int_dir = fs::path(game_root) / "TdGame" / "Localization" / "INT";
    fs::path sub_path = int_dir / "Subtitles.INT";
    if (!fs::exists(sub_path)) {
        sub_path = int_dir / "Subtitles.int";
    }
    std::string content = read_text_file_any_encoding(sub_path.string());
    std::istringstream iss(content);
    std::string line;
    while (std::getline(iss, line)) {
        line = trim_copy(line);
        if (line.empty() || line[0] == ';' || line[0] == '[') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim_copy(line.substr(0, eq));
        std::string val = trim_copy(line.substr(eq + 1));
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
            val = val.substr(1, val.size() - 2);
        }
        subtitle_table_[to_lower_copy(key)] = val;
    }
}

void CutscenePlayer::load_movie_subtitle_cues(const std::string& txt_path) {
    current_cues_.clear();
    if (!fs::exists(txt_path)) return;

    std::string content = read_text_file_any_encoding(txt_path);
    std::istringstream iss(content);
    std::string line;
    float ticks_per_sec = 1000.0f;
    bool first_line = true;

    while (std::getline(iss, line)) {
        line = trim_copy(line);
        if (line.empty()) continue;
        if (first_line) {
            first_line = false;
            try {
                float v = std::stof(line);
                if (v > 0.0f) ticks_per_sec = v;
            } catch (...) {}
            continue;
        }
        size_t c1 = line.find(',');
        size_t c2 = (c1 != std::string::npos) ? line.find(',', c1 + 1) : std::string::npos;
        if (c1 == std::string::npos || c2 == std::string::npos) continue;

        try {
            float t_start = std::stof(trim_copy(line.substr(0, c1))) / ticks_per_sec;
            float t_end   = std::stof(trim_copy(line.substr(c1 + 1, c2 - c1 - 1))) / ticks_per_sec;
            std::string key = trim_copy(line.substr(c2 + 1));
            std::string key_low = to_lower_copy(key);

            SubtitleCue cue;
            cue.start_sec = t_start;
            cue.end_sec = t_end;
            cue.key = key;
            auto it = subtitle_table_.find(key_low);
            cue.text = (it != subtitle_table_.end()) ? it->second : key;
            current_cues_.push_back(std::move(cue));
        } catch (...) {}
    }
}

bool CutscenePlayer::open_bink_streams(const std::string& bik_path) {
    close_bink_streams();

    if (avformat_open_input(&dec_->fmt_ctx, bik_path.c_str(), nullptr, nullptr) < 0) {
        std::cerr << "[Cutscene] Failed to open Bink file: " << bik_path << std::endl;
        return false;
    }
    if (avformat_find_stream_info(dec_->fmt_ctx, nullptr) < 0) {
        close_bink_streams();
        return false;
    }

    // 1. Open Bink Video Stream
    dec_->video_stream_idx = av_find_best_stream(dec_->fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (dec_->video_stream_idx < 0) {
        close_bink_streams();
        return false;
    }

    AVStream* vstream = dec_->fmt_ctx->streams[dec_->video_stream_idx];
    const AVCodec* vcodec = avcodec_find_decoder(vstream->codecpar->codec_id);
    if (!vcodec) {
        close_bink_streams();
        return false;
    }
    dec_->video_ctx = avcodec_alloc_context3(vcodec);
    avcodec_parameters_to_context(dec_->video_ctx, vstream->codecpar);
    dec_->video_ctx->thread_count = 2;
    if (avcodec_open2(dec_->video_ctx, vcodec, nullptr) < 0) {
        close_bink_streams();
        return false;
    }

    video_width_ = dec_->video_ctx->width;
    video_height_ = dec_->video_ctx->height;
    rgba_buffer_.assign(static_cast<size_t>(video_width_) * static_cast<size_t>(video_height_) * 4, 0);

    dec_->sws_ctx = sws_getContext(
        video_width_, video_height_, dec_->video_ctx->pix_fmt,
        video_width_, video_height_, AV_PIX_FMT_RGBA,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );

    if (dec_->fmt_ctx->duration > 0) {
        duration_sec_ = static_cast<float>(dec_->fmt_ctx->duration) / static_cast<float>(AV_TIME_BASE);
    } else if (vstream->duration > 0) {
        duration_sec_ = static_cast<float>(vstream->duration * av_q2d(vstream->time_base));
    } else {
        duration_sec_ = 30.0f;
    }

    // 2. Open English Master Audio Stream (first AVMEDIA_TYPE_AUDIO track in UE3 .bik files)
    for (unsigned int i = 0; i < dec_->fmt_ctx->nb_streams; ++i) {
        if (dec_->fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            dec_->audio_stream_idx = static_cast<int>(i);
            break;
        }
    }
    if (dec_->audio_stream_idx >= 0) {
        AVStream* astream = dec_->fmt_ctx->streams[dec_->audio_stream_idx];
        const AVCodec* acodec = avcodec_find_decoder(astream->codecpar->codec_id);
        if (acodec) {
            dec_->audio_ctx = avcodec_alloc_context3(acodec);
            avcodec_parameters_to_context(dec_->audio_ctx, astream->codecpar);
            if (avcodec_open2(dec_->audio_ctx, acodec, nullptr) >= 0) {
                AVChannelLayout out_ch_layout = AV_CHANNEL_LAYOUT_STEREO;
                swr_alloc_set_opts2(
                    &dec_->swr_ctx,
                    &out_ch_layout,
                    AV_SAMPLE_FMT_FLT,
                    dec_->audio_sample_rate,
                    &dec_->audio_ctx->ch_layout,
                    dec_->audio_ctx->sample_fmt,
                    dec_->audio_ctx->sample_rate,
                    0, nullptr
                );
                if (dec_->swr_ctx) {
                    swr_init(dec_->swr_ctx);
                }
            }
        }
    }

    dec_->frame = av_frame_alloc();
    dec_->pkt = av_packet_alloc();
    dec_->eof_reached = false;
    last_video_pts_sec_ = -1.0;
    total_audio_samples_ = 0;

    if (dec_->sdl_audio_dev != 0) {
        SDL_ClearQueuedAudio(dec_->sdl_audio_dev);
        SDL_PauseAudioDevice(dec_->sdl_audio_dev, 0);
    }

    return true;
}

void CutscenePlayer::close_bink_streams() {
    if (!dec_) return;
    if (dec_->sdl_audio_dev != 0) {
        SDL_ClearQueuedAudio(dec_->sdl_audio_dev);
    }
    if (dec_->pkt) {
        av_packet_free(&dec_->pkt);
    }
    if (dec_->frame) {
        av_frame_free(&dec_->frame);
    }
    if (dec_->swr_ctx) {
        swr_free(&dec_->swr_ctx);
    }
    if (dec_->sws_ctx) {
        sws_freeContext(dec_->sws_ctx);
        dec_->sws_ctx = nullptr;
    }
    if (dec_->audio_ctx) {
        avcodec_free_context(&dec_->audio_ctx);
    }
    if (dec_->video_ctx) {
        avcodec_free_context(&dec_->video_ctx);
    }
    if (dec_->fmt_ctx) {
        avformat_close_input(&dec_->fmt_ctx);
    }
    dec_->video_stream_idx = -1;
    dec_->audio_stream_idx = -1;
    dec_->eof_reached = true;
}

bool CutscenePlayer::play_bink_movie(const std::string& movie_name, bool chain_in_engine_intro) {
    std::string key = to_lower_copy(movie_name);
    if (key.size() > 4 && key.substr(key.size() - 4) == ".bik") {
        key = key.substr(0, key.size() - 4);
    }
    auto it = movie_files_.find(key);
    if (it == movie_files_.end()) {
        return false;
    }

    if (!open_bink_streams(it->second)) {
        return false;
    }

    // Load companion .txt subtitle timings if present (e.g. Scene_01.txt)
    fs::path txt_path = fs::path(it->second).replace_extension(".txt");
    load_movie_subtitle_cues(txt_path.string());

    current_movie_name_ = fs::path(it->second).stem().string();
    mode_ = ECutsceneMode::BinkVideo;
    chain_in_engine_after_bink_ = chain_in_engine_intro;
    elapsed_sec_ = 0.0f;
    letterbox_amount_ = 1.0f;
    active_subtitle_.clear();

    // Immediately decode the opening video frame so frame 0 is never black
    decode_until_time(0.04f);

    std::cout << "[Cutscene] Playing Bink movie '" << current_movie_name_
              << "' (" << video_width_ << "x" << video_height_
              << ", Duration=" << duration_sec_ << "s, Subtitles="
              << current_cues_.size() << ")" << std::endl;
    return true;
}

namespace {

static const AnimSequenceAsset* resolve_cooked_intro_seq(const LevelIntroSequence& intro) {
    if (!intro.valid || intro.anim_export_index_1 <= 0 || intro.package_path.empty()) {
        return nullptr;
    }
    static std::unordered_map<std::string, AnimSetAsset> s_cache;
    std::string key = intro.package_path + "#" + std::to_string(intro.anim_export_index_1);
    auto it = s_cache.find(key);
    if (it == s_cache.end()) {
        UPKPackage pkg(intro.package_path);
        AnimSetAsset aset{};
        if (pkg.is_valid() && AnimSystem::parse_single_anim_sequence(pkg, intro.anim_export_index_1, aset)) {
            it = s_cache.emplace(key, std::move(aset)).first;
        }
    }
    if (it == s_cache.end() || it->second.sequences.empty()) {
        return nullptr;
    }
    const AnimSequenceAsset* seq = it->second.find_sequence(intro.seq_name);
    return seq ? seq : &it->second.sequences.begin()->second;
}

static Vec3 eval_track_pos(const AnimTrack& tr, float norm_t) {
    if (tr.positions.empty()) return Vec3(0.0f, 0.0f, 0.0f);
    if (tr.positions.size() == 1) return tr.positions[0];
    float fidx = std::clamp(norm_t, 0.0f, 1.0f) * float(tr.positions.size() - 1);
    size_t i0 = static_cast<size_t>(std::floor(fidx));
    size_t i1 = std::min(i0 + 1, tr.positions.size() - 1);
    float alpha = fidx - float(i0);
    return tr.positions[i0] + (tr.positions[i1] - tr.positions[i0]) * alpha;
}

static Quat4 eval_track_quat(const AnimTrack& tr, float norm_t) {
    if (tr.rotations.empty()) return Quat4();
    if (tr.rotations.size() == 1) return tr.rotations[0];
    float fidx = std::clamp(norm_t, 0.0f, 1.0f) * float(tr.rotations.size() - 1);
    size_t i0 = static_cast<size_t>(std::floor(fidx));
    size_t i1 = std::min(i0 + 1, tr.rotations.size() - 1);
    float alpha = fidx - float(i0);
    return Quat4::slerp(tr.rotations[i0], tr.rotations[i1], alpha);
}

static Vec3 intro_rig_to_world_pos(const Vec3& actor_loc, float actor_yaw_deg, const Vec3& rig_p) {
    float yaw_rad = actor_yaw_deg * DEG2RAD;
    float cy = std::cos(yaw_rad);
    float sy = std::sin(yaw_rad);
    Vec3 fwd(cy, sy, 0.0f);
    Vec3 left(sy, -cy, 0.0f);
    Vec3 up(0.0f, 0.0f, 1.0f);
    return actor_loc + left * rig_p.x + up * (-rig_p.y) + fwd * rig_p.z;
}

static Vec3 intro_rig_to_world_vec(float actor_yaw_deg, const Vec3& rig_v) {
    float yaw_rad = actor_yaw_deg * DEG2RAD;
    float cy = std::cos(yaw_rad);
    float sy = std::sin(yaw_rad);
    Vec3 fwd(cy, sy, 0.0f);
    Vec3 left(sy, -cy, 0.0f);
    Vec3 up(0.0f, 0.0f, 1.0f);
    return left * rig_v.x + up * (-rig_v.y) + fwd * rig_v.z;
}

} // namespace

void CutscenePlayer::play_in_engine_intro(const LevelScene& scene,
                                          const PlayerTelemetry& telemetry,
                                          float duration_sec) {
    close_bink_streams();

    mode_ = ECutsceneMode::InEngineMatinee;
    chain_in_engine_after_bink_ = false;
    elapsed_sec_ = 0.0f;
    letterbox_amount_ = 1.0f;

    const AnimSequenceAsset* cooked_seq = resolve_cooked_intro_seq(scene.level_intro);
    if (cooked_seq && cooked_seq->length > 0.5f) {
        current_movie_name_ = scene.level_intro.seq_name;
        duration_sec_ = cooked_seq->length;
        matinee_keys_.clear();
        active_subtitle_.clear();
        std::cout << "[Cutscene] Playing cooked level intro Matinee '" << current_movie_name_
                  << "' (" << duration_sec_ << "s, " << cooked_seq->num_frames << " frames)" << std::endl;
        return;
    }

    current_movie_name_ = scene.map_name + "_MatineeIntro";
    duration_sec_ = std::max(2.5f, duration_sec);

    Vec3 eye_end = telemetry.position + Vec3(0.0f, 0.0f, telemetry.eye_height);
    float end_yaw = telemetry.yaw_deg;
    float end_pitch = telemetry.pitch_deg;

    Rotator end_rot = Rotator::from_degrees(0.0f, end_yaw, 0.0f);
    Vec3 fwd = end_rot.forward();
    Vec3 right = end_rot.right();

    // Rooftop camera crane + Faith scripted run-in ending cleanly in Faith's eyes
    matinee_keys_.clear();
    MatineeKeyframe k0;
    k0.time = 0.0f;
    k0.position = eye_end - fwd * 480.0f + right * 140.0f + Vec3(0.0f, 0.0f, 210.0f);
    k0.yaw_deg = end_yaw - 18.0f;
    k0.pitch_deg = -14.0f;
    k0.roll_deg = -3.5f;
    k0.fov_deg = 82.0f;

    MatineeKeyframe k1;
    k1.time = duration_sec_ * 0.45f;
    k1.position = eye_end - fwd * 260.0f;
    k1.yaw_deg = end_yaw;
    k1.pitch_deg = -3.0f;
    k1.roll_deg = 0.0f;
    k1.fov_deg = telemetry.fov_deg;

    MatineeKeyframe k2;
    k2.time = duration_sec_;
    k2.position = eye_end;
    k2.yaw_deg = end_yaw;
    k2.pitch_deg = end_pitch;
    k2.roll_deg = 0.0f;
    k2.fov_deg = telemetry.fov_deg;

    matinee_keys_ = {k0, k1, k2};
    active_subtitle_ = "CELESTE: Looking good today, Faith. Ready for a run across the rooftops?";
}

void CutscenePlayer::stop() {
    close_bink_streams();
    mode_ = ECutsceneMode::None;
    letterbox_amount_ = 0.0f;
    active_subtitle_.clear();
}

void CutscenePlayer::decode_until_time(float target_sec) {
    if (!dec_ || !dec_->fmt_ctx || !dec_->video_ctx || dec_->eof_reached) return;

    AVStream* vstream = dec_->fmt_ctx->streams[dec_->video_stream_idx];
    while (!dec_->eof_reached && last_video_pts_sec_ < static_cast<double>(target_sec)) {
        int ret = av_read_frame(dec_->fmt_ctx, dec_->pkt);
        if (ret < 0) {
            dec_->eof_reached = true;
            break;
        }

        if (dec_->pkt->stream_index == dec_->video_stream_idx) {
            if (avcodec_send_packet(dec_->video_ctx, dec_->pkt) == 0) {
                while (avcodec_receive_frame(dec_->video_ctx, dec_->frame) == 0) {
                    int64_t pts = (dec_->frame->best_effort_timestamp != AV_NOPTS_VALUE)
                                      ? dec_->frame->best_effort_timestamp
                                      : dec_->frame->pts;
                    if (pts != AV_NOPTS_VALUE) {
                        last_video_pts_sec_ = static_cast<double>(pts) * av_q2d(vstream->time_base);
                    } else {
                        last_video_pts_sec_ += (1.0 / 29.97);
                    }

                    uint8_t* dst_slices[4] = {rgba_buffer_.data(), nullptr, nullptr, nullptr};
                    int dst_strides[4] = {video_width_ * 4, 0, 0, 0};
                    sws_scale(
                        dec_->sws_ctx,
                        dec_->frame->data,
                        dec_->frame->linesize,
                        0,
                        video_height_,
                        dst_slices,
                        dst_strides
                    );
                    frame_serial_++;
                }
            }
        } else if (dec_->pkt->stream_index == dec_->audio_stream_idx &&
                   dec_->audio_ctx != nullptr && dec_->swr_ctx != nullptr) {
            if (avcodec_send_packet(dec_->audio_ctx, dec_->pkt) == 0) {
                while (avcodec_receive_frame(dec_->audio_ctx, dec_->frame) == 0) {
                    int out_samples = av_rescale_rnd(
                        swr_get_delay(dec_->swr_ctx, dec_->audio_ctx->sample_rate) + dec_->frame->nb_samples,
                        dec_->audio_sample_rate,
                        dec_->audio_ctx->sample_rate,
                        AV_ROUND_UP
                    );
                    if (out_samples > 0) {
                        std::vector<float> interleaved(static_cast<size_t>(out_samples) * 2);
                        uint8_t* out_ptr[1] = {reinterpret_cast<uint8_t*>(interleaved.data())};
                        int converted = swr_convert(
                            dec_->swr_ctx,
                            out_ptr,
                            out_samples,
                            const_cast<const uint8_t**>(dec_->frame->data),
                            dec_->frame->nb_samples
                        );
                        if (converted > 0) {
                            total_audio_samples_ += static_cast<uint64_t>(converted);
                            if (!headless_ && dec_->sdl_audio_dev != 0) {
                                SDL_QueueAudio(
                                    dec_->sdl_audio_dev,
                                    interleaved.data(),
                                    static_cast<Uint32>(converted * 2 * sizeof(float))
                                );
                            }
                        }
                    }
                }
            }
        }

        av_packet_unref(dec_->pkt);
    }
}

bool CutscenePlayer::seek_and_decode_bink_frame(float target_sec) {
    if (mode_ != ECutsceneMode::BinkVideo || !dec_ || !dec_->fmt_ctx) return false;
    elapsed_sec_ = std::max(0.0f, target_sec);
    decode_until_time(elapsed_sec_);

    active_subtitle_.clear();
    for (const auto& cue : current_cues_) {
        if (elapsed_sec_ >= cue.start_sec && elapsed_sec_ <= cue.end_sec) {
            active_subtitle_ = cue.text;
            break;
        }
    }
    return (frame_serial_ > 0 && !rgba_buffer_.empty());
}

void CutscenePlayer::update(float dt, const LevelScene& scene, PlayerTelemetry& io_telemetry) {
    if (mode_ == ECutsceneMode::None) {
        letterbox_amount_ = 0.0f;
        return;
    }

    elapsed_sec_ += std::max(0.0f, dt);

    if (mode_ == ECutsceneMode::BinkVideo) {
        decode_until_time(elapsed_sec_);

        // Update synchronized subtitle cue
        active_subtitle_.clear();
        for (const auto& cue : current_cues_) {
            if (elapsed_sec_ >= cue.start_sec && elapsed_sec_ <= cue.end_sec) {
                active_subtitle_ = cue.text;
                break;
            }
        }
        if (!active_subtitle_.empty()) {
            io_telemetry.active_subtitle = active_subtitle_;
        }

        if (dec_->eof_reached || elapsed_sec_ >= duration_sec_) {
            bool do_chain = chain_in_engine_after_bink_;
            close_bink_streams();
            if (do_chain) {
                play_in_engine_intro(scene, io_telemetry, 4.5f);
            } else {
                mode_ = ECutsceneMode::None;
                letterbox_amount_ = 0.0f;
            }
        }
    } else if (mode_ == ECutsceneMode::InEngineMatinee) {
        if (elapsed_sec_ >= duration_sec_) {
            mode_ = ECutsceneMode::None;
            letterbox_amount_ = 0.0f;
            io_telemetry.intro_active = false;
            io_telemetry.camera_roll_deg = 0.0f;
            return;
        }

        float norm = std::clamp(elapsed_sec_ / std::max(0.01f, duration_sec_), 0.0f, 1.0f);
        // Smooth fade-out of widescreen bars over the final 25% of the level intro
        letterbox_amount_ = (norm > 0.75f) ? ((1.0f - norm) / 0.25f) : 1.0f;

        const AnimSequenceAsset* cooked_seq = resolve_cooked_intro_seq(scene.level_intro);
        if (cooked_seq && cooked_seq->tracks.size() >= 73) {
            const float actor_yaw = scene.level_intro.actor_yaw_deg;
            const Vec3 actor_loc = scene.level_intro.actor_location;

            Vec3 r_pos = eval_track_pos(cooked_seq->tracks[0], norm);
            Quat4 r_q  = eval_track_quat(cooked_seq->tracks[0], norm).conjugate();
            Vec3 e_pos = eval_track_pos(cooked_seq->tracks[72], norm);
            Quat4 e_q  = eval_track_quat(cooked_seq->tracks[72], norm).conjugate();
            Quat4 c_q  = (cooked_seq->tracks.size() > 73)
                             ? eval_track_quat(cooked_seq->tracks[73], norm).conjugate()
                             : Quat4();

            Vec3 comp_eye = r_pos + r_q.rotate(e_pos);
            Quat4 comp_q  = Quat4::multiply(Quat4::multiply(r_q, e_q), c_q).normalized();

            Vec3 world_eye = intro_rig_to_world_pos(actor_loc, actor_yaw, comp_eye);
            Vec3 world_fwd = intro_rig_to_world_vec(actor_yaw, comp_q.rotate(Vec3(0.0f, 0.0f, 1.0f))).normalized();
            Vec3 world_up  = intro_rig_to_world_vec(actor_yaw, comp_q.rotate(Vec3(0.0f, -1.0f, 0.0f))).normalized();

            float yaw_deg = std::atan2(world_fwd.y, world_fwd.x) * RAD2DEG;
            float pitch_deg = std::asin(std::clamp(world_fwd.z, -1.0f, 1.0f)) * RAD2DEG;
            Rotator base_rot = Rotator::from_degrees(pitch_deg, yaw_deg, 0.0f);
            float roll_deg = std::atan2(world_up.dot(base_rot.right()), world_up.dot(base_rot.up())) * RAD2DEG;

            // Compute instantaneous horizontal root velocity for authentic run state & HUD speedometer
            const float eps = std::min(1.0f / 30.0f, duration_sec_ * 0.02f);
            float norm_prev = std::clamp((elapsed_sec_ - eps) / duration_sec_, 0.0f, 1.0f);
            Vec3 r_prev = intro_rig_to_world_pos(actor_loc, actor_yaw, eval_track_pos(cooked_seq->tracks[0], norm_prev));
            Vec3 r_curr = intro_rig_to_world_pos(actor_loc, actor_yaw, r_pos);
            float spd_2d = (eps > 1e-4f) ? ((r_curr - r_prev).length_xy() / eps) : 0.0f;

            io_telemetry.position = world_eye - Vec3(0.0f, 0.0f, io_telemetry.eye_height);
            io_telemetry.yaw_deg = yaw_deg;
            io_telemetry.pitch_deg = pitch_deg;
            io_telemetry.camera_roll_deg = std::clamp(roll_deg, -35.0f, 35.0f);
            io_telemetry.speed_2d = spd_2d;
            io_telemetry.grounded = (std::abs(r_curr.z - r_prev.z) < eps * 260.0f);
            io_telemetry.move_state = io_telemetry.grounded ? EMovement::MOVE_Walking : EMovement::MOVE_Falling;
            io_telemetry.intro_active = true;
            io_telemetry.intro_anim_name = scene.level_intro.seq_name;
            io_telemetry.intro_pkg_path = scene.level_intro.package_path;
            io_telemetry.intro_anim_exp_1 = scene.level_intro.anim_export_index_1;
            io_telemetry.intro_anim_time = elapsed_sec_;
            if (!active_subtitle_.empty()) {
                io_telemetry.active_subtitle = active_subtitle_;
            }
            return;
        }

        if (matinee_keys_.size() < 2) {
            mode_ = ECutsceneMode::None;
            letterbox_amount_ = 0.0f;
            io_telemetry.intro_active = false;
            return;
        }

        // Fallback rooftop camera crane + Faith run-in along matinee_keys_
        size_t seg = 0;
        while (seg + 2 < matinee_keys_.size() && elapsed_sec_ > matinee_keys_[seg + 1].time) {
            ++seg;
        }
        const MatineeKeyframe& a = matinee_keys_[seg];
        const MatineeKeyframe& b = matinee_keys_[seg + 1];
        float seg_dur = std::max(1e-4f, b.time - a.time);
        float u = std::clamp((elapsed_sec_ - a.time) / seg_dur, 0.0f, 1.0f);
        float s = u * u * (3.0f - 2.0f * u); // Smoothstep easing into Faith's eyes

        Vec3 cam_eye = a.position + (b.position - a.position) * s;
        float run_spd = (norm > 0.35f) ? (380.0f * std::sin(std::clamp((1.0f - norm) / 0.65f, 0.0f, 1.0f) * 1.570796f)) : 0.0f;
        io_telemetry.position = cam_eye - Vec3(0.0f, 0.0f, io_telemetry.eye_height);
        io_telemetry.yaw_deg = a.yaw_deg + (b.yaw_deg - a.yaw_deg) * s;
        io_telemetry.pitch_deg = a.pitch_deg + (b.pitch_deg - a.pitch_deg) * s;
        io_telemetry.camera_roll_deg = a.roll_deg + (b.roll_deg - a.roll_deg) * s;
        io_telemetry.fov_deg = a.fov_deg + (b.fov_deg - a.fov_deg) * s;
        io_telemetry.speed_2d = run_spd;
        io_telemetry.grounded = true;
        io_telemetry.move_state = EMovement::MOVE_Walking;
        io_telemetry.sim_time += dt;
        if (!active_subtitle_.empty()) {
            io_telemetry.active_subtitle = active_subtitle_;
        }
    }
}

bool CutscenePlayer::cycle_next_cutscene(const LevelScene& scene, const PlayerTelemetry& telemetry) {
    static const char* kPlaylist[] = {
        "Scene_01",
        "Scene_02",
        "Scene_04",
        "Scene_06",
        "Scene_07",
        "Scene_09",
        "Scene_10",
        "Scene_11",
        "Scene_12",
        "Scene_13",
        "Attract_Movie",
        "EndCredits",
        "InEngine_Intro"
    };
    constexpr int kCount = static_cast<int>(sizeof(kPlaylist) / sizeof(kPlaylist[0]));
    cycle_index_ = (cycle_index_ + 1) % kCount;
    const char* target = kPlaylist[cycle_index_];
    if (std::string(target) == "InEngine_Intro") {
        play_in_engine_intro(scene, telemetry, 5.0f);
        return true;
    }
    return play_bink_movie(target, false);
}

std::string CutscenePlayer::get_chapter_intro_movie(const std::string& map_name) {
    std::string m = to_lower_copy(map_name);
    if (m.find("tutorial") != std::string::npos || m.find("sp00") != std::string::npos) return "Scene_01";
    if (m.find("escape") != std::string::npos || m.find("jacknife") != std::string::npos || m.find("sp02") != std::string::npos) return "Scene_02";
    if (m.find("stormdrain") != std::string::npos || m.find("heat") != std::string::npos || m.find("sp03") != std::string::npos) return "Scene_04";
    if (m.find("cranes") != std::string::npos || m.find("ropeburn") != std::string::npos || m.find("sp04") != std::string::npos) return "Scene_06";
    if (m.find("subway") != std::string::npos || m.find("new_eden") != std::string::npos || m.find("sp05") != std::string::npos) return "Scene_07";
    if (m.find("mall") != std::string::npos || m.find("pirandello") != std::string::npos || m.find("sp06") != std::string::npos) return "Scene_09";
    if (m.find("factory") != std::string::npos || m.find("sp07") != std::string::npos) return "Scene_10";
    if (m.find("boat") != std::string::npos || m.find("sp08") != std::string::npos) return "Scene_11";
    if (m.find("convoy") != std::string::npos || m.find("kate") != std::string::npos || m.find("sp09") != std::string::npos) return "Scene_12";
    if (m.find("scraper") != std::string::npos || m.find("shard") != std::string::npos) return "Scene_13";
    return "";
}

} // namespace me
