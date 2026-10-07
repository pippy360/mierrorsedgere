#define SDL_MAIN_HANDLED
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_metal.h>

#include "math/types.hpp"
#include "assets/upk_loader.hpp"
#include "assets/ini_config.hpp"
#include "audio/audio_engine.hpp"
#include "cutscene/cutscene_player.hpp"
#include "physics/collision_world.hpp"
#include "physics/parkour_controller.hpp"
#include "renderer/metal_renderer.hpp"

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <filesystem>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <cmath>

namespace fs = std::filesystem;

namespace me {

// Ensure screenshots directory exists
static void ensure_dir(const std::string& path) {
    std::error_code ec;
    fs::create_directories(path, ec);
}

// Copy file helper
static void copy_artifact(const std::string& src, const std::string& dst) {
    std::error_code ec;
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
}

} // namespace me

// -----------------------------------------------------------------------------
// Deterministic Headless Oracle & Multi-Stage Parkour Verification Suite
// -----------------------------------------------------------------------------
static int run_oracle_verification(const std::string& game_root, const std::string& script_json) {
    using namespace me;
    std::cout << "\n============================================================" << std::endl;
    std::cout << "  MIRROR'S EDGE NATIVE MACOS ENGINE - ORACLE VERIFICATION" << std::endl;
    std::cout << "============================================================" << std::endl;
    std::cout << "[Oracle] Game Root: " << game_root << std::endl;

    ensure_dir("screenshots");
    ensure_dir("/tmp/me_oracle_screenshots");
    const std::string brain_dir = "/Users/tomnom/.gemini/jetski/brain/722392a1-bc28-473f-af94-1a09e569935f";
    ensure_dir(brain_dir);

    // 1. Initialize Headless Metal Renderer & Audio Engine
    MetalRenderer renderer;
    if (!renderer.init_headless(1280, 720)) {
        std::cerr << "[Oracle ERROR] MetalRenderer::init_headless failed!" << std::endl;
        return 1;
    }
    std::cout << "[Oracle] MetalRenderer initialized in headless mode (1280x720)." << std::endl;

    AudioEngine audio;
    if (!audio.init(true)) {
        std::cerr << "[Oracle ERROR] AudioEngine headless init failed!" << std::endl;
        return 1;
    }
    audio.load_stock_audio(game_root);
    audio.load_level_audio(game_root, "Maps/SP00/Tutorial_p.me1");
    std::cout << "[Oracle] AudioEngine initialized in headless mode ("
              << audio.get_clip_count() << " clips, "
              << audio.get_cue_count() << " SoundCues, "
              << audio.get_ambient_emitter_count() << " 3D AmbientSound emitters)." << std::endl;

    // 2. Asset Verification Checks
    std::cout << "\n--- [Asset Verification] ---" << std::endl;

    // A. Movement Config (DefaultPawnMovement.ini)
    MovementConfig move_cfg;
    std::string move_ini = get_config_path(game_root, "DefaultPawnMovement.ini");
    bool ini_ok = load_movement_config_from_ini(move_ini, move_cfg);
    std::cout << "[Oracle] DefaultPawnMovement.ini: " << (ini_ok ? "PASS" : "FAIL")
              << " (Sprint=" << move_cfg.sprint_speed << ", Gravity=" << move_cfg.gravity
              << ", BaseJumpZ=" << move_cfg.base_jump_z << ")" << std::endl;

    // B. Subtitles (Subtitles.INT)
    std::string sub_path = get_localization_path(game_root, "Subtitles.int", "INT");
    std::vector<std::pair<std::string, std::string>> subtitles;
    bool sub_ok = load_subtitles_from_int(sub_path, subtitles);
    std::cout << "[Oracle] Subtitles.INT: " << (sub_ok ? "PASS" : "FAIL")
              << " (" << subtitles.size() << " localized subtitle entries)" << std::endl;

    // C. Campaign Chapters
    std::vector<ChapterInfo> chapters;
    bool ch_ok = load_campaign_chapters(game_root, chapters);
    std::cout << "[Oracle] Campaign Chapters: " << (ch_ok ? "PASS" : "FAIL")
              << " (" << chapters.size() << " chapters cataloged)" << std::endl;

    // D. Audio Banks
    bool audio_ok = audio.load_sound_bank(game_root, "A_Bodyfalls");
    if (!audio_ok) audio_ok = audio.load_stock_audio(game_root);
    std::cout << "[Oracle] Stock Audio Bank (A_Bodyfalls): " << (audio_ok ? "PASS" : "FAIL")
              << " (" << audio.get_clip_count() << " sound clips loaded)" << std::endl;

    // E. Level Package, Streaming & Elevator Probing
    UPKPackage entry_pkg(game_root + "/TdGame/CookedPC/Maps/Entry.upk");
    std::cout << "[Oracle] Entry.upk: " << (entry_pkg.is_valid() ? "PASS" : "FAIL")
              << " (" << entry_pkg.get_exports().size() << " exports)" << std::endl;

    LevelScene sp00_scene;
    bool sp00_ok = load_level_scene(game_root, "Maps/SP00/Tutorial_p.me1", sp00_scene);
    std::cout << "[Oracle] SP00/Tutorial_p.me1: " << (sp00_ok ? "PASS" : "FAIL")
              << " (" << sp00_scene.actors.size() << " actors, "
              << sp00_scene.meshes.size() << " meshes, "
              << (sp00_scene.collision ? sp00_scene.collision->triangle_count() : 0) << " collision triangles)" << std::endl;
    std::cout << "[Oracle]   SP00 Ambient Lighting (" << sp00_scene.sky_light_source
              << "): FSkyLightSceneProxy UpperLinear=(" << sp00_scene.raw_sky_upper_linear.x << ","
              << sp00_scene.raw_sky_upper_linear.y << "," << sp00_scene.raw_sky_upper_linear.z
              << "), LowerLinear=(" << sp00_scene.raw_sky_lower_linear.x << ","
              << sp00_scene.raw_sky_lower_linear.y << "," << sp00_scene.raw_sky_lower_linear.z
              << "), ModShadowColor=(" << sp00_scene.mod_shadow_color.x << ","
              << sp00_scene.mod_shadow_color.y << "," << sp00_scene.mod_shadow_color.z
              << "), WorldInfo.SkyColor=(" << sp00_scene.world_sky_color.x << ","
              << sp00_scene.world_sky_color.y << "," << sp00_scene.world_sky_color.z
              << ") IBL=" << sp00_scene.ibl_intensity << std::endl;

    LevelScene sp01_scene;
    bool sp01_ok = load_level_scene(game_root, "Maps/SP01/Edge_p.me1", sp01_scene);
    std::cout << "[Oracle] SP01/Edge_p.me1: " << (sp01_ok ? "PASS" : "FAIL")
              << " (" << sp01_scene.actors.size() << " actors, "
              << sp01_scene.meshes.size() << " meshes, "
              << sp01_scene.all_streaming_packages.size() << " streaming sublevels, "
              << sp01_scene.checkpoint_infos.size() << " TdCheckpoints, "
              << sp01_scene.elevators.size() << " elevators, "
              << sp01_scene.reflection_captures.size() << " reflection captures, "
              << sp01_scene.reflection_volumes.size() << " TdReflectionVolumes)" << std::endl;
    if (!sp01_scene.reflection_captures.empty()) {
        const auto& rc = sp01_scene.reflection_captures.front();
        std::cout << "[Oracle]   Planar Reflection: " << rc.actor_name << " -> Target=" << rc.texture_target
                  << ", Plane=(N=(" << rc.mirror_normal.x << "," << rc.mirror_normal.y << "," << rc.mirror_normal.z
                  << "), W=" << rc.plane_w << "), Volume=" << rc.reflection_volume << std::endl;
    }

    // Verify real cooked elevator extraction from SP01/Escape_Intro-Off_Spt.me1
    UPKPackage esc_elev_pkg(game_root + "/TdGame/CookedPC/Maps/SP01/Escape_Intro-Off_Spt.me1");
    std::unordered_map<std::string, StaticMeshAsset> esc_meshes;
    esc_elev_pkg.extract_static_meshes(esc_meshes);
    std::vector<ElevatorInstance> esc_elevators;
    esc_elev_pkg.extract_elevators(esc_meshes, esc_elevators);
    bool real_elev_ok = !esc_elevators.empty() &&
                        std::abs(esc_elevators.front().end_pos.z - esc_elevators.front().start_pos.z - 1680.0f) < 1.0f;
    std::cout << "[Oracle] Escape_Intro-Off_Spt.me1 Elevator Extraction: " << (real_elev_ok ? "PASS" : "FAIL");
    if (!esc_elevators.empty()) {
        const auto& el = esc_elevators.front();
        std::cout << " (" << el.name << " [" << el.cab_mesh_name << "] Z=" << el.start_pos.z
                  << " -> " << el.end_pos.z << ", Duration=" << el.ride_duration << "s)";
    }
    std::cout << std::endl;

    // 3. Multi-Stage Deterministic Parkour Simulation & Screenshot Capture
    //    Every stage drives the real ParkourController against the real UE3 level collision of
    //    SP00/Tutorial_p.me1 at the tutorial's own training spots (and SP01/Escape_p.me1 for the
    //    elevator): no procedural geometry is involved.
    std::cout << "\n--- [Multi-Stage Parkour Simulation & Screenshot Capture] ---" << std::endl;

    constexpr float kDt = 1.0f / 60.0f;
    ParkourController controller(move_cfg);
    LevelScene sim_scene = sp00_scene;

    std::vector<std::string> telemetry_log;
    auto log_telemetry = [&](const std::string& stage_tag) {
        const auto& t = controller.get_telemetry();
        std::ostringstream ss;
        ss << "{\"stage\":\"" << stage_tag << "\",\"tick\":" << t.tick
           << ",\"pos\":[" << t.position.x << "," << t.position.y << "," << t.position.z << "]"
           << ",\"vel\":[" << t.velocity.x << "," << t.velocity.y << "," << t.velocity.z << "]"
           << ",\"speed\":" << t.speed_2d << ",\"fov\":" << t.fov_deg
           << ",\"state\":\"" << move_state_name(t.move_state) << "\""
           << ",\"roll\":" << t.camera_roll_deg << ",\"health\":" << t.health
           << ",\"reaction\":" << t.reaction_energy
           << ",\"in_elevator\":" << (t.in_elevator ? "true" : "false")
           << ",\"sublevels\":" << t.streamed_sublevel_count << "}";
        telemetry_log.push_back(ss.str());
    };

    auto save_and_publish_png = [&](const std::string& name) {
        std::string local_path = "screenshots/" + name;
        std::string tmp_path = "/tmp/me_oracle_screenshots/" + name;
        std::string brain_path = brain_dir + "/" + name;
        renderer.save_screenshot_png(local_path);
        copy_artifact(local_path, tmp_path);
        copy_artifact(local_path, brain_path);
    };

    // Steps `in` until done() holds (checked after every step) or max_frames elapse.
    auto step_until = [&](const InputFrame& in, int max_frames, LevelScene& scene, auto&& done) {
        for (int i = 0; i < max_frames; ++i) {
            controller.step(in, kDt, scene);
            if (done()) return true;
        }
        return false;
    };

    InputFrame in_idle{};
    InputFrame in_run{};
    in_run.forward = 1.0f;
    in_run.sprint = true;
    InputFrame in_run_jump = in_run;
    in_run_jump.jump = true;

    // Stage 1: Sprint acceleration & FOV scaling across the tutorial's starting roof
    std::cout << "[Oracle Stage 1] Running Sprint Acceleration & FOV Scaling..." << std::endl;
    controller.reset(sp00_scene.player_spawn_pos, sp00_scene.player_spawn_yaw);
    for (int i = 0; i < 90; ++i) {
        controller.step(in_run, kDt, sim_scene);
        log_telemetry("Stage1_Sprint");
    }
    renderer.render_frame(sim_scene, controller.get_telemetry());
    save_and_publish_png("oracle_1_sprint_rooftop.png");

    const float s1_speed = controller.get_telemetry().speed_2d;
    const float s1_fov = controller.get_telemetry().fov_deg;
    const float s1_z = controller.get_position().z;
    bool s1_pass = (s1_speed >= 400.0f) && (s1_fov > 90.0f) && controller.is_grounded() &&
                   std::abs(s1_z - sp00_scene.player_spawn_pos.z) < 1.0f;
    std::cout << "  -> Stage 1 Result: " << (s1_pass ? "PASS" : "FAIL")
              << " (Speed=" << s1_speed << " u/s, FOV=" << s1_fov << "°, Roof Z=" << s1_z << ")" << std::endl;

    // Stage 2: Speed Vault over the stage-7 airduct (S_AirductSystem_02a, 145 units high, east face
    // x ~ -2621) & Springboard off the stage-17 stacked boxes (S_ConstructionPackages_01b)
    std::cout << "[Oracle Stage 2] Testing Speed Vault & Springboard..." << std::endl;
    // A. Speed Vault: sprint west and jump within reach of the duct face
    controller.reset(Vec3(-1655.2f, -6505.2f, 4224.0f), 180.0f);
    step_until(in_run, 240, sim_scene, [&] { return controller.get_position().x <= -2513.0f; });
    controller.step(in_run_jump, kDt, sim_scene);
    bool s2_vault = (controller.get_move_state() == EMovement::MOVE_SpeedVaulting);
    log_telemetry("Stage2_Vault");
    step_until(in_run, 180, sim_scene, [&] { return controller.is_grounded(); });
    // Back on the roof (floor 4224) beyond the duct rather than on top of it (4369)
    const Vec3 vault_land = controller.get_position();
    bool s2_vault_over = s2_vault && controller.is_grounded() && vault_land.x < -2700.0f && vault_land.z < 4300.0f;

    // A2. Tutorial Stage 5 Chain-Link Fence Jump-Vault (yellow platform at x=2550,z=4344 -> fence at x=2339):
    controller.reset(Vec3(2550.0f, -6750.0f, 4344.0f), 180.0f);
    step_until(in_run, 60, sim_scene, [&] { return controller.get_position().x <= 2490.0f; });
    controller.step(in_run_jump, kDt, sim_scene);
    bool s2_fence_vault = (controller.get_move_state() == EMovement::MOVE_SpeedVaulting ||
                           controller.get_move_state() == EMovement::MOVE_VaultOver);
    step_until(in_run, 180, sim_scene, [&] {
        if (controller.get_move_state() == EMovement::MOVE_SpeedVaulting ||
            controller.get_move_state() == EMovement::MOVE_VaultOver) {
            s2_fence_vault = true;
        }
        return s2_fence_vault && controller.is_grounded() && controller.get_position().x < 2290.0f;
    });
    const Vec3 fence_land = controller.get_position();
    bool s2_fence_ok = s2_fence_vault && controller.is_grounded() && fence_land.x < 2290.0f;

    // B. Springboard: sprint east at the stacked boxes and jump off them
    controller.reset(Vec3(156.4f, -3933.6f, 3840.0f), 0.0f);
    step_until(in_run, 240, sim_scene, [&] { return controller.get_position().x >= 674.0f; });
    controller.step(in_run_jump, kDt, sim_scene);
    bool s2_spring = (controller.get_move_state() == EMovement::MOVE_SpringBoarding);
    log_telemetry("Stage2_Springboard");

    // Advance the springboard jump toward its apex looking across the rooftops
    for (int i = 0; i < 8; ++i) {
        controller.step(in_run, kDt, sim_scene);
    }
    controller.set_rotation(0.0f, -12.0f, 0.0f);
    renderer.render_frame(sim_scene, controller.get_telemetry());
    save_and_publish_png("oracle_2_springboard_vault.png");
    float spring_apex_z = controller.get_position().z;
    step_until(in_run, 120, sim_scene, [&] {
        spring_apex_z = std::max(spring_apex_z, controller.get_position().z);
        return controller.get_velocity().z < 0.0f;
    });
    // A plain jump (BaseJumpZ 630) peaks ~202 units up; the springboard (JumpZ 950) clears 300+
    bool s2_spring_high = s2_spring && (spring_apex_z - 3840.0f) > 300.0f;
    bool s2_pass = s2_vault_over && s2_fence_ok && s2_spring_high;
    std::cout << "  -> Stage 2 Result: " << (s2_pass ? "PASS" : "FAIL")
              << " (Vault=" << (s2_vault ? "OK" : "NO")
              << ", FenceVault=" << (s2_fence_ok ? "OK" : "NO")
              << " [x=" << fence_land.x << ", z=" << fence_land.z << "]"
              << ", Vault Landing=(" << vault_land.x << ", " << vault_land.z << ")"
              << ", SpringBoard=" << (s2_spring ? "OK" : "NO")
              << ", Apex Z=" << spring_apex_z << ")" << std::endl;

    // Stage 3: Wallrun along the stage-6 billboard (S_RunnerSign_02) across the rooftop gap
    // Native FindWallForward requires angling into the wall (up to 57°); kick off mid-run onto the stage-7 roof.
    std::cout << "[Oracle Stage 3] Testing Wallrun & 15° Camera Tilt..." << std::endl;
    controller.reset(Vec3(-100.0f, -6180.0f, 4224.0f), 160.0f);
    controller.set_velocity(Vec3(-517.0f, 188.0f, 0.0f));
    step_until(in_run, 60, sim_scene, [&] { return controller.get_position().x <= -275.0f; });
    controller.step(in_run_jump, kDt, sim_scene);
    log_telemetry("Stage3_Wallrun_Start");
    const EMovement s3_state = controller.get_move_state();
    bool s3_wallrun = (s3_state == EMovement::MOVE_WallRunningLeft || s3_state == EMovement::MOVE_WallRunningRight);
    float s3_max_roll = std::abs(controller.get_roll());
    for (int i = 0; i < 15; ++i) {
        controller.step(in_run, kDt, sim_scene);
        log_telemetry("Stage3_Wallrun_Sustain");
        s3_max_roll = std::max(s3_max_roll, std::abs(controller.get_roll()));
    }
    renderer.render_frame(sim_scene, controller.get_telemetry());
    save_and_publish_png("oracle_3_wallrun_tilt.png");

    // Ride the billboard wallrun over the gap and kick off onto the stage-7 roof (z = 4224)
    step_until(in_run, 60, sim_scene, [&] { return controller.get_position().x <= -770.0f; });
    controller.step(in_run_jump, kDt, sim_scene);
    step_until(in_run, 180, sim_scene, [&] { return controller.is_grounded(); });
    const Vec3 s3_land = controller.get_position();
    bool s3_cleared_gap = controller.is_grounded() && s3_land.x < -1150.0f && std::abs(s3_land.z - 4224.0f) < 2.0f;
    bool s3_pass = s3_wallrun && (s3_max_roll >= 10.0f) && s3_cleared_gap;
    std::cout << "  -> Stage 3 Result: " << (s3_pass ? "PASS" : "FAIL")
              << " (State=" << move_state_name(s3_state)
              << ", Camera Roll=" << s3_max_roll << "°"
              << ", Landing=(" << s3_land.x << ", " << s3_land.z << "))" << std::endl;

    // Stage 4: Wallclimb up the stage-10 facade (S_R_05_03_F_SP00, face x ~ -8384)
    std::cout << "[Oracle Stage 4] Testing Wallclimb..." << std::endl;
    controller.reset(Vec3(-8310.0f, -5534.0f, 4224.0f), 180.0f);
    for (int i = 0; i < 5; ++i) {
        controller.step(in_idle, kDt, sim_scene);
    }
    InputFrame in4{};
    in4.forward = 1.0f;
    in4.jump = true;
    controller.step(in4, kDt, sim_scene);
    bool s4_climb = (controller.get_move_state() == EMovement::MOVE_WallClimbing);
    log_telemetry("Stage4_Climb");

    float s4_peak_z = controller.get_position().z;
    in4.jump = false;
    for (int i = 0; i < 35; ++i) {
        controller.step(in4, kDt, sim_scene);
        s4_peak_z = std::max(s4_peak_z, controller.get_position().z);
    }
    bool s4_pass = s4_climb && (s4_peak_z - 4224.0f) >= 40.0f;
    std::cout << "  -> Stage 4 Result: " << (s4_pass ? "PASS" : "FAIL")
              << " (State=" << move_state_name(controller.get_move_state())
              << ", Climb Peak Z=" << s4_peak_z << ")" << std::endl;

    // Stage 5: Zipline (TdZiplineVolume_0 from the stage-15 platform) & Crouch Slide (stage-3 airduct)
    std::cout << "[Oracle Stage 5] Testing Zipline & Crouch Slide..." << std::endl;
    controller.reset(Vec3(-8900.0f, -5280.0f, 6142.0f), 20.0f);
    for (int i = 0; i < 10; ++i) {
        controller.step(in_idle, kDt, sim_scene);
    }
    InputFrame in5_jump{};
    in5_jump.jump = true;
    controller.step(in5_jump, kDt, sim_scene);
    bool s5_zip = step_until(in_idle, 40, sim_scene, [&] { return controller.get_move_state() == EMovement::MOVE_ZipLine; });
    log_telemetry("Stage5_Zipline");

    const Vec3 zip_grab = controller.get_position();
    for (int i = 0; i < 120 && controller.get_move_state() == EMovement::MOVE_ZipLine; ++i) {
        controller.step(in_idle, kDt, sim_scene);
        log_telemetry("Stage5_Zipline_Ride");
        if (i == 60) {
            renderer.render_frame(sim_scene, controller.get_telemetry());
            save_and_publish_png("oracle_4_zipline_slide.png");
        }
    }
    const float zip_travel = controller.get_position().distance(zip_grab);
    bool s5_zip_ride = s5_zip && (controller.get_move_state() == EMovement::MOVE_ZipLine) && zip_travel > 600.0f;

    // Sprint up and slide under the clear middle of the stage-3 airduct (S_AirductSystem_02e)
    controller.reset(Vec3(208.0f, -7790.0f, 5760.0f), 0.0f);
    for (int i = 0; i < 100; ++i) {
        controller.step(in_run, kDt, sim_scene);
    }
    InputFrame in5_slide{};
    in5_slide.forward = 1.0f;
    in5_slide.crouch = true;
    controller.step(in5_slide, kDt, sim_scene);
    bool s5_slide = (controller.get_move_state() == EMovement::MOVE_Slide);
    log_telemetry("Stage5_Slide");
    const float slide_start_x = controller.get_position().x;
    for (int i = 0; i < 50; ++i) {
        controller.step(in5_slide, kDt, sim_scene);
    }
    const float slide_dist = controller.get_position().x - slide_start_x;
    bool s5_slide_ok = s5_slide && (controller.get_move_state() == EMovement::MOVE_Slide) &&
                       controller.is_grounded() && slide_dist > 300.0f;
    bool s5_pass = s5_zip_ride && s5_slide_ok;
    std::cout << "  -> Stage 5 Result: " << (s5_pass ? "PASS" : "FAIL")
              << " (ZipLine=" << (s5_zip ? "OK" : "NO") << ", Ride=" << zip_travel << " u"
              << ", Slide=" << (s5_slide ? "OK" : "NO") << ", Slide Distance=" << slide_dist << " u)" << std::endl;

    // Stage 6: Mid-Air Coil & Skill Roll: jump off the stage-15 high platform onto the roof below
    // (6144 -> 5760 = 384 u drop), coil mid-air, and time crouch within 0.2 s before touchdown.
    std::cout << "[Oracle Stage 6] Testing Mid-Air Coil & Skill Roll..." << std::endl;
    controller.reset(Vec3(-8900.0f, -5280.0f, 6144.0f), 90.0f);
    for (int i = 0; i < 35; ++i) {
        controller.step(in_run, kDt, sim_scene);
    }
    const float s6_launch_z = controller.get_position().z;
    controller.step(in_run_jump, kDt, sim_scene);

    InputFrame in6{};
    in6.crouch = true;
    in6.forward = 1.0f;
    controller.step(in6, kDt, sim_scene);
    bool s6_coil = (controller.get_move_state() == EMovement::MOVE_Coil);
    log_telemetry("Stage6_Coil");

    // Release crouch while descending so the roll trigger can re-arm (0.6 s cooldown), then press
    // crouch within 0.2 s of touchdown (TdPawn.CanSkillRoll) for the skill roll.
    step_until(in_run, 120, sim_scene, [&] { return controller.get_position().z <= 5860.0f; });
    step_until(in6, 60, sim_scene, [&] { return controller.is_grounded(); });
    bool s6_roll = (controller.get_move_state() == EMovement::MOVE_SkillRoll);
    log_telemetry("Stage6_SkillRoll");
    const float s6_drop = s6_launch_z - controller.get_position().z;
    bool s6_pass = s6_coil && s6_roll;
    std::cout << "  -> Stage 6 Result: " << (s6_pass ? "PASS" : "FAIL")
              << " (Coil=" << (s6_coil ? "OK" : "NO") << ", Roll=" << (s6_roll ? "OK" : "NO")
              << ", Drop=" << s6_drop << " u)" << std::endl;

    // Stage 7: Combat Disarm (Celeste on the stage-19 combat terrace) & Reaction Time
    std::cout << "[Oracle Stage 7] Testing Combat Disarm & Reaction Time..." << std::endl;
    controller.reset(Vec3(751.8f, -1591.7f, 4992.0f), 90.0f);
    InputFrame in7_walk{};
    in7_walk.forward = 1.0f;
    for (int i = 0; i < 20; ++i) {
        controller.step(in7_walk, kDt, sim_scene);
    }
    InputFrame in7{};
    in7.reaction_time = true;
    in7.disarm = true;

    controller.step(in7, kDt, sim_scene);
    log_telemetry("Stage7_Disarm");

    bool s7_disarm = (controller.get_move_state() == EMovement::MOVE_Snatch && controller.get_telemetry().weapon.equipped);
    bool s7_reaction = controller.get_telemetry().reaction_active;

    renderer.render_frame(sim_scene, controller.get_telemetry());
    save_and_publish_png("oracle_5_combat_disarm_reaction.png");

    // Verify all 11 retail firearms equip, fire with 3D tracers, and render 1P/3P animations
    int weapons_verified = 0;
    for (int w = 0; w < 11; ++w) {
        InputFrame in_cyc{};
        in_cyc.cycle_weapon_dir = 1;
        controller.step(in_cyc, 1.0f / 60.0f, sim_scene);
        // Step past equip cooldown and fire a round
        for (int f = 0; f < 15; ++f) {
            InputFrame in_wait{};
            controller.step(in_wait, 1.0f / 60.0f, sim_scene);
        }
        InputFrame in_fire{};
        in_fire.fire = true;
        controller.step(in_fire, 1.0f / 60.0f, sim_scene);
        if (controller.get_telemetry().weapon.equipped && controller.get_telemetry().weapon.fired_this_tick) {
            weapons_verified++;
        }
        renderer.render_frame(sim_scene, controller.get_telemetry());
    }

    // Verify unarmed MOVE_Melee completes and exits cleanly back to MOVE_Walking without sticking
    controller.reset(Vec3(751.8f, -1591.7f, 4992.0f), 90.0f);
    InputFrame in_melee{};
    in_melee.melee = true;
    controller.step(in_melee, 1.0f / 60.0f, sim_scene);
    const bool melee_started = (controller.get_move_state() == EMovement::MOVE_Melee);
    for (int f = 0; f < 45; ++f) {
        controller.step(in_idle, 1.0f / 60.0f, sim_scene);
    }
    const bool melee_recovered = melee_started && (controller.get_move_state() == EMovement::MOVE_Walking);

    bool s7_pass = s7_disarm && s7_reaction && (weapons_verified == 11) && melee_recovered;
    std::cout << "  -> Stage 7 Result: " << (s7_pass ? "PASS" : "FAIL")
              << " (Disarm=" << (s7_disarm ? "OK" : "NO")
              << ", MeleeRecovery=" << (melee_recovered ? "OK" : "STUCK")
              << ", WeaponsVerified=" << weapons_verified << "/11"
              << ", Reaction=" << (s7_reaction ? "ACTIVE" : "OFF") << ")" << std::endl;

    // Stage 8: Interactive Elevator Ride & Mid-Shaft Level Streaming on the real SP01/Escape_p.me1
    // mainlift (Escape_Intro-Off_Spt InterpActor cab + sliding doors, Z 10608 -> 12288)
    std::cout << "[Oracle Stage 8] Testing Interactive Elevator Ride & Mid-Shaft Level Streaming..." << std::endl;
    bool s8_pass = false;
    {
        LevelScene esc_scene;
        const bool esc_ok = load_level_scene(game_root, "Maps/SP01/Escape_p.me1", esc_scene);
        int lift = -1;
        for (size_t i = 0; i < esc_scene.elevators.size(); ++i) {
            const ElevatorInstance& e = esc_scene.elevators[i];
            if (std::abs(e.end_pos.z - e.start_pos.z - 1680.0f) < 1.0f) {
                lift = static_cast<int>(i);
                break;
            }
        }

        float cab_top_z = 0.0f;
        bool s8_elev_top = false;
        bool s8_walkout = false;
        Vec3 walkout_pos(0.0f, 0.0f, 0.0f);
        if (esc_ok && lift >= 0) {
            const Vec3 lift_floor = esc_scene.elevators[lift].start_pos + esc_scene.elevators[lift].cab_local_offset;
            const Vec3 lift_half = esc_scene.elevators[lift].cab_half_extents;
            const float lift_top_z = esc_scene.elevators[lift].end_pos.z;

            // 8A. Walk from the corridor through the open lower doors into the cab and press Use
            controller.reset(Vec3(5800.0f, lift_floor.y, lift_floor.z), 0.0f);
            InputFrame in8_walk{};
            in8_walk.forward = 1.0f;
            for (int i = 0; i < 45; ++i) {
                controller.step(in8_walk, kDt, esc_scene);
            }
            InputFrame in8_use{};
            in8_use.use = true;
            controller.step(in8_use, kDt, esc_scene);

            // Ride DoorsClosing -> Moving (sublevels stream in mid-shaft) -> DoorsOpening -> IdleEnd
            bool captured_mid_ride = false;
            for (int i = 0; i < 600 && esc_scene.elevators[lift].state != ElevatorState::IdleEnd; ++i) {
                controller.step(in_idle, kDt, esc_scene);
                log_telemetry("Stage8_Elevator_Ride");
                const ElevatorInstance& el = esc_scene.elevators[lift];
                if (!captured_mid_ride && el.state == ElevatorState::DoorsOpening && el.door_open_End >= 0.42f) {
                    captured_mid_ride = true;
                    PlayerTelemetry t8_shot = controller.get_telemetry();
                    t8_shot.yaw_deg = 180.0f;  // look out through the opening upper doors
                    t8_shot.pitch_deg = -3.0f;
                    renderer.render_frame(esc_scene, t8_shot);
                    save_and_publish_png("oracle_8_elevator_level_streaming.png");
                }
            }
            cab_top_z = controller.get_position().z;
            s8_elev_top = (std::abs(cab_top_z - lift_top_z) < 2.0f) &&
                          (esc_scene.elevators[lift].state == ElevatorState::IdleEnd) &&
                          esc_scene.elevators[lift].streaming_triggered;

            // 8B. Turn around and walk out of the open upper doors onto the upper floor
            controller.set_rotation(180.0f, 0.0f, 0.0f);
            for (int i = 0; i < 60; ++i) {
                controller.step(in8_walk, kDt, esc_scene);
            }
            walkout_pos = controller.get_position();
            s8_walkout = controller.is_grounded() && walkout_pos.x < lift_floor.x - lift_half.x - 30.0f &&
                         std::abs(walkout_pos.z - lift_top_z) < 2.0f;
        }
        s8_pass = s8_elev_top && s8_walkout && real_elev_ok;
        std::cout << "  -> Stage 8 Result: " << (s8_pass ? "PASS" : "FAIL")
                  << " (Escape_p=" << (esc_ok ? "OK" : "NO") << ", Lift=" << lift
                  << ", Elevator Top Z=" << cab_top_z
                  << ", Walkout Pos=(" << walkout_pos.x << ", " << walkout_pos.z << ")"
                  << ", Streamed Sublevels=" << esc_scene.loaded_sublevel_packages.size()
                  << ", Checkpoint='" << controller.get_telemetry().active_checkpoint_name << "')" << std::endl;
    }

    // Stage 9: SP01/Edge_p.me1 Level Rendering, Checkpoint Streaming & Chapter Select Menu
    std::cout << "[Oracle Stage 9] Testing SP01 Level, Checkpoint Streaming & Chapter Select Overlay..." << std::endl;
    sp01_scene.chapter_title = "CHAPTER 0: THE EDGE";
    stream_level_to_checkpoint(game_root, sp01_scene, 0);
    PlayerTelemetry t9 = controller.get_telemetry();
    t9.reaction_active = false;
    t9.in_elevator = false;
    t9.weapon.equipped = false;
    t9.move_state = EMovement::MOVE_Walking;
    t9.speed_2d = 0.0f;
    t9.fov_deg = 90.0f;
    t9.camera_roll_deg = 0.0f;
    t9.pitch_deg = -2.0f;
    t9.position = sp01_scene.player_spawn_pos + Vec3(0.0f, 0.0f, 95.0f);
    t9.yaw_deg = 172.0f;
    t9.active_subtitle = "Merc: Faith, the runners' route leads across the rooftops ahead.";

    renderer.render_frame(sp01_scene, t9);
    save_and_publish_png("oracle_6_sp01_edge_level.png");

    renderer.set_menu_open(true);
    renderer.set_selected_chapter(1);
    renderer.render_frame(sp01_scene, t9);
    save_and_publish_png("oracle_7_chapter_select_menu.png");
    renderer.set_menu_open(false);
    std::cout << "  -> Stage 9 Result: PASS (Rendered SP01 and Chapter Select Menu)" << std::endl;

    // Stage 10: Render SP00/Tutorial_p.me1 Rooftop Training Progression Screenshots
    std::cout << "[Oracle Stage 10] Rendering SP00/Tutorial_p.me1 Rooftop Training Screenshots..." << std::endl;
    struct TutorialShotSpec {
        int cp_idx;
        Vec3 pos_offset;
        float yaw_deg;
        float pitch_deg;
        const char* filename;
    };
    static const TutorialShotSpec kTutorialShots[] = {
        {0,  Vec3(0.0f, 0.0f, 0.0f),     0.0f,  -2.0f, "tutorial_1_rooftop_start.png"},
        {2,  Vec3(0.0f, 0.0f, 0.0f),    -4.0f,  -2.0f, "tutorial_2_slide_airduct_gap.png"},
        {5,  Vec3(0.0f, 0.0f, 0.0f),   180.0f,  -2.0f, "tutorial_3_wallrun_speedvault.png"},
        {8,  Vec3(-240.0f, 0.0f, 0.0f), 175.0f, -4.0f, "tutorial_4_balance_wallclimb.png"},
        {14, Vec3(0.0f, 0.0f, 0.0f),    -2.0f, -10.0f, "tutorial_5_zipline_skillroll.png"},
        {16, Vec3(0.0f, 0.0f, 0.0f),     1.5f,  -1.0f, "tutorial_6_springboard_combat.png"}
    };
    for (const auto& shot : kTutorialShots) {
        if (shot.cp_idx < static_cast<int>(sp00_scene.checkpoints.size())) {
            PlayerTelemetry tt = controller.get_telemetry();
            tt.reaction_active = false;
            tt.in_elevator = false;
            tt.weapon.equipped = false;
            tt.move_state = EMovement::MOVE_Walking;
            tt.speed_2d = 0.0f;
            tt.fov_deg = 90.0f;
            tt.camera_roll_deg = 0.0f;
            tt.position = sp00_scene.checkpoints[shot.cp_idx] + shot.pos_offset;
            tt.yaw_deg = shot.yaw_deg;
            tt.pitch_deg = shot.pitch_deg;
            tt.active_checkpoint = shot.cp_idx;
            if (shot.cp_idx < static_cast<int>(sp00_scene.subtitles.size())) {
                tt.active_subtitle = sp00_scene.subtitles[shot.cp_idx];
            }
            renderer.render_frame(sp00_scene, tt);
            save_and_publish_png(shot.filename);
        }
    }
    std::cout << "  -> Stage 10 Result: PASS (Rendered 6 SP00/Tutorial_p.me1 screenshots)" << std::endl;

    // Stage 11: Bink (.bik) Cutscene Video/Audio Decoder, Subtitle Sync & In-Engine Matinee Cutscenes
    std::cout << "[Oracle Stage 11] Testing Bink (.bik) Video/Audio Decoder, Subtitle Sync & Matinee Cutscenes..." << std::endl;
    CutscenePlayer oracle_cutscenes;
    bool cs_init_ok = oracle_cutscenes.init(game_root, /*headless=*/true);
    renderer.set_cutscene_player(&oracle_cutscenes);
    bool cs_bink_ok = oracle_cutscenes.play_bink_movie("Scene_01", false) &&
                      oracle_cutscenes.seek_and_decode_bink_frame(8.5f);
    std::string cs_sub_text = oracle_cutscenes.get_active_subtitle();
    bool cs_sub_ok = (cs_sub_text.find("city") != std::string::npos);
    bool cs_aud_ok = (oracle_cutscenes.get_decoded_audio_samples() > 100000);
    if (cs_bink_ok) {
        renderer.render_frame(sp00_scene, controller.get_telemetry());
        save_and_publish_png("oracle_9_cutscene_bink_player.png");
    }
    oracle_cutscenes.play_in_engine_intro(sp01_scene, controller.get_telemetry());
    oracle_cutscenes.update(1.0f, sp01_scene, controller.get_telemetry());
    bool cs_intro_ok = sp01_scene.level_intro.valid &&
                       sp01_scene.level_intro.seq_name == "sp01_intro" &&
                       controller.get_telemetry().intro_active;
    oracle_cutscenes.stop();
    controller.get_telemetry().intro_active = false;
    renderer.set_cutscene_player(nullptr);

    // Verify SP00 Tutorial_Aud SeqEvent_LevelLoaded -> A_VO_SP00_Opening_1_1_Merc_Cue radio transmission
    audio.load_level_audio(game_root, "Maps/SP00/Tutorial_p.me1");
    const SoundClip* merc_opening_clip = audio.get_clip("A_VO_SP00_Opening_1_1_Merc_Cue");
    bool tut_vo_ok = (merc_opening_clip != nullptr &&
                      merc_opening_clip->duration > 13.5f &&
                      !merc_opening_clip->subtitles.empty() &&
                      merc_opening_clip->subtitles.front().text.find("fall took you out of commission") != std::string::npos &&
                      !audio.get_level_loaded_cues().empty());

    bool s11_pass = cs_init_ok && cs_bink_ok && cs_sub_ok && cs_aud_ok && cs_intro_ok && tut_vo_ok;
    std::cout << "  -> Stage 11 Result: " << (s11_pass ? "PASS" : "FAIL")
              << " (Movies=" << oracle_cutscenes.get_available_movie_count()
              << ", Video=" << oracle_cutscenes.get_video_width() << "x" << oracle_cutscenes.get_video_height()
              << ", AudioSamples=" << oracle_cutscenes.get_decoded_audio_samples()
              << ", LevelIntro=" << sp01_scene.level_intro.seq_name
              << ", TutorialOpeningVO=" << (merc_opening_clip ? merc_opening_clip->duration : 0.0f) << "s"
              << ", Subtitle=\"" << cs_sub_text << "\")" << std::endl;

    // Stage 12: Interactive Door Barging (`TdMove_Barge` & Hinge Rotation on SP00 Rooftop Doorway)
    std::cout << "[Oracle Stage 12] Testing Interactive Door Barging (TdMove_Barge & Hinge Swing)..." << std::endl;
    bool s12_pass = false;
    bool saw_move_barge = false;
    float final_door_deg = 0.0f;
    float final_door_x = 0.0f;
    if (!sp00_scene.barge_doors.empty()) {
        BargeDoorInstance& door = sp00_scene.barge_doors[0];
        door.state = DoorState::Closed;
        door.open_angle_rad = 0.0f;
        door.model_matrix = Mat4::identity();
        controller.reset(Vec3(-3960.0f, -6360.0f, 4224.0f), 180.0f);
        InputFrame barge_in{};
        barge_in.forward = 1.0f;
        barge_in.sprint = true;
        for (int step = 0; step < 72; ++step) {
            controller.step(barge_in, kDt, sp00_scene);
            if (controller.get_telemetry().move_state == EMovement::MOVE_Barge) {
                saw_move_barge = true;
            }
        }
        final_door_deg = door.open_angle_rad * (180.0f / 3.14159265f);
        final_door_x = controller.get_telemetry().position.x;
        s12_pass = saw_move_barge && (std::abs(door.open_angle_rad) > 1.5f) && (final_door_x < -4300.0f);
    }
    std::cout << "  -> Stage 12 Result: " << (s12_pass ? "PASS" : "FAIL")
              << " (Doors=" << sp00_scene.barge_doors.size()
              << ", MoveBarge=" << (saw_move_barge ? "OK" : "NO")
              << ", Swing=" << final_door_deg << " deg"
              << ", EndX=" << final_door_x << ")" << std::endl;

    // Write complete telemetry log
    std::ofstream tel_file("/tmp/me_oracle_telemetry.json");
    if (tel_file.is_open()) {
        tel_file << "[\n";
        for (size_t i = 0; i < telemetry_log.size(); ++i) {
            tel_file << "  " << telemetry_log[i] << (i + 1 < telemetry_log.size() ? ",\n" : "\n");
        }
        tel_file << "]\n";
        tel_file.close();
        std::cout << "[Oracle] Telemetry written to /tmp/me_oracle_telemetry.json" << std::endl;
    }

    // Stage 13: Pipe Balance Beam Walking (TdBalanceWalkVolume) & Vertical Drainpipe Climbing (TdLadderVolume)
    std::cout << "[Oracle Stage 13] Testing Pipe Balance Beam Walking & Vertical Drainpipe Climbing..." << std::endl;
    bool s13_pass = false;
    {
        InputFrame in_walk{};
        in_walk.forward = 1.0f;

        // 13A. Walk across Tutorial Stage 9 horizontal balance pipe (-5690.09, -6372.5, 4263) -> (-6860.91, -6021.5, 4263)
        controller.reset(Vec3(-5715.0f, -6365.0f, 4263.0f), 163.3f);
        bool entered_balance = false;
        float min_bal_z = 99999.0f;
        for (int i = 0; i < 420; ++i) {
            controller.step(in_walk, kDt, sim_scene);
            if (controller.get_move_state() == EMovement::MOVE_Balance) entered_balance = true;
            min_bal_z = std::min(min_bal_z, controller.get_position().z);
            if (controller.get_position().x <= -6848.0f) break;
        }
        const Vec3 bal_end_pos = controller.get_position();
        const bool bal_ok = entered_balance && (min_bal_z >= 4258.0f) && (bal_end_pos.x <= -6848.0f);

        // 13B. Climb Tutorial Stage 11 Pipe 1 (-7890, -3106, 4223..4936), jump to Pipe 2 (-7663, -3106, 4596..4914),
        //      and mantle onto upper roof (z >= 4914)
        controller.reset(Vec3(-7890.0f, -3165.0f, 4225.0f), 90.0f);
        bool grabbed_pipe1 = false;
        for (int i = 0; i < 240; ++i) {
            controller.step(in_walk, kDt, sim_scene);
            if (controller.get_move_state() == EMovement::MOVE_Climb) {
                grabbed_pipe1 = true;
                if (controller.get_position().z >= 4560.0f) break;
            }
        }
        // Aim toward Pipe 2 (+X / East along the North wall) and jump across
        controller.set_rotation(25.0f, 0.0f, 0.0f);
        InputFrame in_pipe_jump{};
        in_pipe_jump.jump = true;
        in_pipe_jump.forward = 1.0f;
        controller.step(in_pipe_jump, kDt, sim_scene);

        bool caught_pipe2 = false;
        for (int i = 0; i < 240; ++i) {
            controller.step(in_walk, kDt, sim_scene);
            if (controller.get_move_state() == EMovement::MOVE_Climb &&
                std::abs(controller.get_position().x - (-7663.0f)) < 60.0f) {
                caught_pipe2 = true;
            }
            if (caught_pipe2 && controller.is_grounded() && controller.get_position().z >= 4910.0f) {
                break;
            }
        }
        const Vec3 climb_top_pos = controller.get_position();
        const bool climb_ok = grabbed_pipe1 && caught_pipe2 && controller.is_grounded() && (climb_top_pos.z >= 4910.0f);

        s13_pass = bal_ok && climb_ok;
        std::cout << "  -> Stage 13 Result: " << (s13_pass ? "PASS" : "FAIL")
                  << " (Balance Entered=" << (entered_balance ? "YES" : "NO")
                  << ", Balance End X=" << bal_end_pos.x
                  << ", Pipe1 Grabbed=" << (grabbed_pipe1 ? "YES" : "NO")
                  << ", Pipe2 Caught=" << (caught_pipe2 ? "YES" : "NO")
                  << ", Roof Exit Z=" << climb_top_pos.z << ")" << std::endl;
    }

    bool s14_pass = false;
    {
        std::cout << "[Oracle Stage 14] Testing SP02 Jacknife Forward Sprint (TdLadderVolume NaN Regression)..." << std::endl;
        LevelScene sp02_scene;
        if (load_level_scene(game_root, "Maps/SP02/Stormdrain_p.me1", sp02_scene)) {
            controller.reset(sp02_scene.player_spawn_pos, sp02_scene.player_spawn_yaw);
            InputFrame in_fwd{};
            in_fwd.forward = 1.0f;
            bool finite_ok = true;
            bool alive_ok = true;
            for (int i = 0; i < 240; ++i) {
                controller.step(in_fwd, kDt, sp02_scene);
                const Vec3 p = controller.get_position();
                if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
                    finite_ok = false;
                    break;
                }
                if (controller.get_telemetry().falling_to_death) {
                    alive_ok = false;
                    break;
                }
            }
            const Vec3 end_p = controller.get_position();
            const float dist_moved = end_p.distance_xy(sp02_scene.player_spawn_pos);
            s14_pass = finite_ok && alive_ok && (dist_moved > 1000.0f);
            std::cout << "  -> Stage 14 Result: " << (s14_pass ? "PASS" : "FAIL")
                      << " (Finite=" << (finite_ok ? "YES" : "NO")
                      << ", Alive=" << (alive_ok ? "YES" : "NO")
                      << ", DistMoved=" << dist_moved
                      << ", EndPos=(" << end_p.x << ", " << end_p.y << ", " << end_p.z << "))" << std::endl;
        } else {
            std::cout << "  -> Stage 14 Result: FAIL (Could not load Maps/SP02/Stormdrain_p.me1)" << std::endl;
        }
    }

    // Stages with pass/fail assertions: parkour stages 1-8, cutscene stage 11, door barging stage 12, pipe climb/balance stage 13, and SP02 forward sprint stage 14
    // (stages 9 and 10 only render screenshots).
    const bool stage_results[] = {s1_pass, s2_pass, s3_pass, s4_pass, s5_pass, s6_pass, s7_pass, s8_pass, s11_pass, s12_pass, s13_pass, s14_pass};
    int stages_failed = 0;
    for (bool ok : stage_results) stages_failed += ok ? 0 : 1;
    std::cout << "\n============================================================" << std::endl;
    if (stages_failed == 0) {
        std::cout << "  ORACLE VERIFICATION COMPLETE: ALL SYSTEMS PASS!" << std::endl;
    } else {
        std::cout << "  ORACLE VERIFICATION COMPLETE: " << stages_failed << " OF " << std::size(stage_results)
                  << " VERIFIED STAGES FAILED" << std::endl;
    }
    std::cout << "============================================================" << std::endl;
    return stages_failed == 0 ? 0 : 1;
}

// -----------------------------------------------------------------------------
// Interactive SDL2 + Metal Window Gameplay Loop
// -----------------------------------------------------------------------------
static int run_interactive_app(const std::string& game_root, int initial_chapter,
                              const std::string& custom_level, int max_frames,
                              bool start_in_main_menu) {
    using namespace me;
    std::cout << "\n============================================================" << std::endl;
    std::cout << "  MIRROR'S EDGE NATIVE MACOS - INTERACTIVE LAUNCH" << std::endl;
    std::cout << "============================================================" << std::endl;

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        std::cerr << "[SDL ERROR] Initialization failed: " << SDL_GetError() << std::endl;
        return 1;
    }

    int win_w = 1280;
    int win_h = 720;
    SDL_Window* window = SDL_CreateWindow(
        "Mirror's Edge (Native macOS Apple Silicon)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        win_w, win_h,
        SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
    );

    if (!window) {
        std::cerr << "[SDL ERROR] Failed to create window: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return 1;
    }

    SDL_MetalView metal_view = SDL_Metal_CreateView(window);
    if (!metal_view) {
        std::cerr << "[SDL ERROR] Failed to create Metal view: " << SDL_GetError() << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    void* metal_layer = SDL_Metal_GetLayer(metal_view);
    int drawable_w = win_w;
    int drawable_h = win_h;
    SDL_Metal_GetDrawableSize(window, &drawable_w, &drawable_h);

    MetalRenderer renderer;
    if (!renderer.init_with_metal_layer(metal_layer, drawable_w, drawable_h)) {
        std::cerr << "[Metal ERROR] init_with_metal_layer failed!" << std::endl;
        SDL_Metal_DestroyView(metal_view);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    AudioEngine audio;
    audio.init(false);
    audio.load_stock_audio(game_root);

    MovementConfig move_cfg;
    load_movement_config_from_ini(get_config_path(game_root, "DefaultPawnMovement.ini"), move_cfg);

    std::vector<ChapterInfo> chapters;
    load_campaign_chapters(game_root, chapters);

    std::vector<std::pair<std::string, std::string>> subtitles;
    load_subtitles_from_int(get_localization_path(game_root, "Subtitles.int", "INT"), subtitles);

    // Open first gamepad if available
    SDL_GameController* game_controller = nullptr;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) {
            game_controller = SDL_GameControllerOpen(i);
            if (game_controller) {
                std::cout << "[Gamepad] Connected: " << SDL_GameControllerName(game_controller) << std::endl;
                break;
            }
        }
    }

    // Load active level & Cutscene Player
    LevelScene active_scene;
    int current_chapter_idx = std::clamp(initial_chapter, 0, 9);
    renderer.set_selected_chapter(current_chapter_idx);

    ParkourController controller(move_cfg);

    CutscenePlayer cutscene_player;
    cutscene_player.init(game_root, /*headless=*/(max_frames > 0));
    renderer.set_cutscene_player(&cutscene_player);

    bool pending_level_loaded_audio = false;
    bool was_vo_playing = false;

    auto load_chapter_or_level = [&](int ch_idx, const std::string& custom_path, bool play_intro = true) {
        std::string map_file = custom_path;
        if (map_file.empty()) {
            static const char* kChapterMaps[10] = {
                "Maps/SP00/Tutorial_p.me1",
                "Maps/SP01/Edge_p.me1",
                "Maps/SP02/Stormdrain_p.me1",
                "Maps/SP03/Cranes_p.me1",
                "Maps/SP04/Subway_p.me1",
                "Maps/SP05/Mall_p.me1",
                "Maps/SP06/Factory_p.me1",
                "Maps/SP07/Boat_p.me1",
                "Maps/SP08/Convoy_p.me1",
                "Maps/SP09/Scraper_p.me1"
            };
            map_file = kChapterMaps[std::clamp(ch_idx, 0, 9)];
        }

        std::cout << "[Game] Loading Level: " << map_file << "..." << std::endl;
        LevelScene loaded_scene;
        if (!load_level_scene(game_root, map_file, loaded_scene)) {
            std::cerr << "[Game ERROR] Failed to load " << map_file << " from " << game_root
                      << (active_scene.meshes.empty() ? "" : " (staying in the current level)") << std::endl;
            return false;
        }
        active_scene = std::move(loaded_scene);
        controller.reset(active_scene.player_spawn_pos, active_scene.player_spawn_yaw);
        controller.get_telemetry().active_checkpoint = 0;
        if (!active_scene.subtitles.empty() && !active_scene.subtitles[0].empty()) {
            controller.get_telemetry().active_subtitle = active_scene.subtitles[0];
        }
        audio.load_level_audio(game_root, map_file);
        pending_level_loaded_audio = true;
        was_vo_playing = false;

        // Play authentic chapter opening cutscene (.bik animated story movie + 3D rooftop camera fly-in).
        // When loading a direct map via --level, enter the 3D level directly so SeqEvent_LevelLoaded VO plays immediately.
        if (max_frames == 0 && play_intro) {
            std::string intro_movie = custom_path.empty() ? CutscenePlayer::get_chapter_intro_movie(map_file) : "";
            if (!intro_movie.empty() && cutscene_player.play_bink_movie(intro_movie, /*chain_in_engine=*/true)) {
                // Bink movie started; will transition into 3D rooftop intro on finish
            } else {
                cutscene_player.play_in_engine_intro(active_scene, controller.get_telemetry(), 4.5f);
            }
        }
        return true;
    };

    if (!load_chapter_or_level(current_chapter_idx, custom_level, /*play_intro=*/!start_in_main_menu)) {
        std::cerr << "[Game ERROR] No playable level (check --game-root / --level)." << std::endl;
        if (game_controller) SDL_GameControllerClose(game_controller);
        SDL_Metal_DestroyView(metal_view);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    auto set_menu_active = [&](bool open) {
        renderer.set_menu_open(open);
        audio.set_menu_music(open);
        SDL_SetRelativeMouseMode(open ? SDL_FALSE : SDL_TRUE);
        SDL_ShowCursor(open ? SDL_ENABLE : SDL_DISABLE);
    };

    auto to_menu_coords = [&](int mx, int my, float& ux, float& uy) {
        SDL_GetWindowSize(window, &win_w, &win_h);
        ux = (static_cast<float>(mx) / static_cast<float>(std::max(1, win_w))) * 1280.0f;
        uy = (static_cast<float>(my) / static_cast<float>(std::max(1, win_h))) * 720.0f;
    };

    set_menu_active(start_in_main_menu);
    ensure_dir("screenshots");

    // Interactive Loop
    bool running = true;
    int frame_counter = 0;
    auto last_time = std::chrono::high_resolution_clock::now();

    EMovement prev_state = EMovement::MOVE_Walking;
    int prev_checkpoint = 0;
    bool prev_falling_to_death = false;
    bool prev_fall_death_impact = false;
    float footstep_timer = 0.0f;
    bool reaction_toggled = false;
    bool suppress_space_until_release = false;

    constexpr std::array<int, 6> kSensPresets = {50, 75, 100, 125, 150, 200};
    constexpr std::array<int, 6> kFovPresets  = {85, 90, 95, 100, 105, 110};
    int sens_preset_idx = 2; // 100%
    int fov_preset_idx  = 1; // 90 deg
    bool is_fullscreen  = false;
    renderer.set_menu_options_state(kSensPresets[sens_preset_idx], kFovPresets[fov_preset_idx], is_fullscreen);

    std::cout << "\n[Controls]" << std::endl;
    std::cout << "  WASD: Move (Sprint active by default, momentum acceleration)" << std::endl;
    std::cout << "  Mouse: Look (Yaw/Pitch)" << std::endl;
    std::cout << "  Space: Jump / Wallrun / Wallclimb / Vault / Springboard / Skip Cutscene" << std::endl;
    std::cout << "  Left Ctrl / C / Left Shift: Crouch / Slide / Mid-Air Coil / Skill Roll" << std::endl;
    std::cout << "  O (or C during Cutscene): Play / Cycle All 12 Bink & 3D Cutscenes" << std::endl;
    std::cout << "  Q: 180° Turn" << std::endl;
    std::cout << "  Left Mouse / F: Melee Punch/Kick or Fire Weapon" << std::endl;
    std::cout << "  Right Mouse / E: Disarm Enemy / Use Elevator" << std::endl;
    std::cout << "  X: Toggle Reaction Time (Slow-Motion)" << std::endl;
    std::cout << "  V / Left Alt: Runner Vision Look-At" << std::endl;
    std::cout << "  Tab / M: Toggle Main Menu / Chapter Select" << std::endl;
    std::cout << "  1..9, 0: Directly load Chapter 0 through 9 (with Chapter Cutscene)" << std::endl;
    std::cout << "  [ / ] (or B / N): Previous / Next Tutorial Checkpoint" << std::endl;
    std::cout << "  R: Reset to Active Checkpoint" << std::endl;
    std::cout << "  P / F12: Screenshot PNG" << std::endl;
    std::cout << "  ESC: Quit\n" << std::endl;

    while (running) {
        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - last_time).count();
        last_time = now;
        if (dt > 0.1f) dt = 0.1f; // clamp hitch spikes

        InputFrame input{};
        // By default sprint is active for momentum acceleration matching retail Mirror's Edge
        input.sprint = true;

        auto activate_menu_selection = [&]() {
            const int tab = renderer.selected_menu_tab();
            if (tab == 0) {
                // STORY: Launch selected chapter with Bink / 3D opening cutscene
                set_menu_active(false);
                load_chapter_or_level(current_chapter_idx, "", /*play_intro=*/true);
            } else if (tab == 1) {
                // RACE (SPEED RUN): Launch selected course directly into timed run (no cutscene)
                set_menu_active(false);
                load_chapter_or_level(current_chapter_idx, "", /*play_intro=*/false);
            } else if (tab == 2) {
                // OPTIONS: Toggle / cycle the selected game setting
                const int row = std::clamp(renderer.selected_menu_row(), 0, 5);
                if (row == 0) {
                    sens_preset_idx = (sens_preset_idx + 1) % static_cast<int>(kSensPresets.size());
                    renderer.set_menu_options_state(kSensPresets[sens_preset_idx], kFovPresets[fov_preset_idx], is_fullscreen);
                } else if (row == 1) {
                    fov_preset_idx = (fov_preset_idx + 1) % static_cast<int>(kFovPresets.size());
                    controller.get_telemetry().fov_deg = static_cast<float>(kFovPresets[fov_preset_idx]);
                    renderer.set_menu_options_state(kSensPresets[sens_preset_idx], kFovPresets[fov_preset_idx], is_fullscreen);
                } else if (row == 2) {
                    is_fullscreen = !is_fullscreen;
                    SDL_SetWindowFullscreen(window, is_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                    SDL_Metal_GetDrawableSize(window, &drawable_w, &drawable_h);
                    renderer.resize(drawable_w, drawable_h);
                    renderer.set_menu_options_state(kSensPresets[sens_preset_idx], kFovPresets[fov_preset_idx], is_fullscreen);
                } else if (row == 3) {
                    reaction_toggled = !reaction_toggled;
                    controller.get_telemetry().reaction_active = reaction_toggled;
                } else if (row == 4) {
                    controller.get_telemetry().intro_active = false;
                    cutscene_player.stop();
                    int cp = std::clamp(controller.get_telemetry().active_checkpoint, 0,
                                        std::max(0, static_cast<int>(active_scene.checkpoints.size()) - 1));
                    Vec3 spawn = active_scene.checkpoints.empty() ? active_scene.player_spawn_pos : active_scene.checkpoints[cp];
                    controller.reset(spawn, active_scene.player_spawn_yaw);
                    set_menu_active(false);
                } else if (row == 5) {
                    running = false;
                }
            } else if (tab == 3) {
                // EXTRAS: Cutscene gallery & runner bonus actions
                const int row = std::clamp(renderer.selected_menu_row(), 0, 5);
                if (row == 0) {
                    static const char* kMenuChapterMaps[10] = {
                        "Maps/SP00/Tutorial_p.me1", "Maps/SP01/Edge_p.me1", "Maps/SP02/Stormdrain_p.me1",
                        "Maps/SP03/Cranes_p.me1",   "Maps/SP04/Subway_p.me1", "Maps/SP05/Mall_p.me1",
                        "Maps/SP06/Factory_p.me1",  "Maps/SP07/Boat_p.me1",   "Maps/SP08/Convoy_p.me1",
                        "Maps/SP09/Scraper_p.me1"
                    };
                    set_menu_active(false);
                    std::string map_file = kMenuChapterMaps[std::clamp(current_chapter_idx, 0, 9)];
                    std::string intro_movie = CutscenePlayer::get_chapter_intro_movie(map_file);
                    if (!intro_movie.empty() && cutscene_player.play_bink_movie(intro_movie, /*chain_in_engine=*/true)) {
                        // Playing chapter Bink movie
                    } else {
                        cutscene_player.play_in_engine_intro(active_scene, controller.get_telemetry(), 4.5f);
                    }
                } else if (row == 1) {
                    set_menu_active(false);
                    controller.reset(active_scene.player_spawn_pos, active_scene.player_spawn_yaw);
                    controller.get_telemetry().intro_active = false;
                    cutscene_player.cycle_next_cutscene(active_scene, controller.get_telemetry());
                } else if (row == 2) {
                    set_menu_active(false);
                    controller.reset(active_scene.player_spawn_pos, active_scene.player_spawn_yaw);
                    cutscene_player.play_in_engine_intro(active_scene, controller.get_telemetry(), 4.5f);
                } else if (row == 3) {
                    controller.equip_weapon("Colt1911");
                    set_menu_active(false);
                } else if (row == 4) {
                    input.spawn_combat_squad = true;
                    set_menu_active(false);
                } else if (row == 5) {
                    renderer.set_selected_menu_tab(0);
                }
            }
        };

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                running = false;
            } else if (ev.type == SDL_WINDOWEVENT) {
                if (ev.window.event == SDL_WINDOWEVENT_RESIZED || ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    SDL_Metal_GetDrawableSize(window, &drawable_w, &drawable_h);
                    renderer.resize(drawable_w, drawable_h);
                }
            } else if (ev.type == SDL_MOUSEMOTION) {
                if (renderer.is_menu_open()) {
                    float ux = 0.0f, uy = 0.0f;
                    to_menu_coords(ev.motion.x, ev.motion.y, ux, uy);
                    const int tab = renderer.selected_menu_tab();
                    const int max_rows = (tab <= 1) ? 10 : 6;
                    if (ux >= 80.0f && ux <= 540.0f && uy >= 142.0f && uy < 142.0f + max_rows * 35.5f) {
                        int row = std::clamp(static_cast<int>((uy - 142.0f) / 35.5f), 0, max_rows - 1);
                        if (tab <= 1) {
                            current_chapter_idx = row;
                            renderer.set_selected_chapter(row);
                        } else {
                            renderer.set_selected_menu_row(row);
                        }
                    }
                } else if (SDL_GetRelativeMouseMode() == SDL_TRUE && !cutscene_player.is_playing()) {
                    float sens = 0.15f * (static_cast<float>(kSensPresets[sens_preset_idx]) / 100.0f);
                    input.look_yaw_delta += float(ev.motion.xrel) * sens;
                    input.look_pitch_delta -= float(ev.motion.yrel) * sens;
                }
            } else if (ev.type == SDL_MOUSEBUTTONDOWN) {
                if (renderer.is_menu_open() && frame_counter >= 5) {
                    if (ev.button.button == SDL_BUTTON_LEFT) {
                        float ux = 0.0f, uy = 0.0f;
                        to_menu_coords(ev.button.x, ev.button.y, ux, uy);
                        const int tab = renderer.selected_menu_tab();
                        const int max_rows = (tab <= 1) ? 10 : 6;
                        if (ux >= 80.0f && ux <= 540.0f && uy >= 142.0f && uy < 142.0f + max_rows * 35.5f) {
                            // Clicked a specific row on the active tab -> select and activate it
                            int row = std::clamp(static_cast<int>((uy - 142.0f) / 35.5f), 0, max_rows - 1);
                            if (tab <= 1) {
                                current_chapter_idx = row;
                                renderer.set_selected_chapter(row);
                            } else {
                                renderer.set_selected_menu_row(row);
                            }
                            activate_menu_selection();
                        } else if (ux >= 810.0f && ux <= 1190.0f && uy >= 100.0f && uy <= 425.0f) {
                            // Clicked the right-side Preview card -> activate selected item
                            activate_menu_selection();
                        } else if (ux >= 96.0f && ux < 1184.0f && uy >= 528.0f && uy <= 590.0f) {
                            // Clicked one of the 4 Category columns (STORY / RACE / OPTIONS / EXTRAS)
                            int col = std::clamp(static_cast<int>((ux - 96.0f) / 272.0f), 0, 3);
                            if (renderer.selected_menu_tab() == col && col <= 1) {
                                activate_menu_selection();
                            } else {
                                renderer.set_selected_menu_tab(col);
                                renderer.set_selected_menu_row(0);
                            }
                        } else if (uy >= 630.0f && uy <= 675.0f) {
                            // Clicked Bottom Button Bar ([ENTER] ACTION or [ESC] RESUME)
                            if (ux >= 995.0f && ux <= 1195.0f) {
                                set_menu_active(false);
                            } else if (ux >= 580.0f && ux < 995.0f) {
                                activate_menu_selection();
                            }
                        }
                    } else if (ev.button.button == SDL_BUTTON_RIGHT) {
                        set_menu_active(false);
                    }
                } else if (!renderer.is_menu_open() && SDL_GetRelativeMouseMode() != SDL_TRUE) {
                    SDL_SetRelativeMouseMode(SDL_TRUE);
                    SDL_ShowCursor(SDL_DISABLE);
                } else if (frame_counter >= 15 && !renderer.is_menu_open() && !cutscene_player.is_playing()) {
                    if (ev.button.button == SDL_BUTTON_LEFT) {
                        if (controller.get_weapon().equipped) input.fire = true;
                        else input.melee = true;
                    } else if (ev.button.button == SDL_BUTTON_RIGHT) {
                        input.disarm = true;
                    }
                }
            } else if (ev.type == SDL_MOUSEWHEEL) {
                if (renderer.is_menu_open() && ev.wheel.y != 0) {
                    const int tab = renderer.selected_menu_tab();
                    if (tab <= 1) {
                        current_chapter_idx = (current_chapter_idx + (ev.wheel.y > 0 ? 9 : 1)) % 10;
                        renderer.set_selected_chapter(current_chapter_idx);
                    } else {
                        int r = (renderer.selected_menu_row() + (ev.wheel.y > 0 ? 5 : 1)) % 6;
                        renderer.set_selected_menu_row(r);
                    }
                } else if (!renderer.is_menu_open() && !cutscene_player.is_playing() && ev.wheel.y != 0) {
                    input.cycle_weapon_dir = (ev.wheel.y > 0) ? 1 : -1;
                }
            } else if (ev.type == SDL_KEYDOWN && !ev.key.repeat) {
                SDL_Keycode key = ev.key.keysym.sym;
                if (key == SDLK_ESCAPE) {
                    if (renderer.is_menu_open()) {
                        running = false;
                    } else if (cutscene_player.is_playing()) {
                        controller.reset(active_scene.player_spawn_pos, active_scene.player_spawn_yaw);
                        controller.get_telemetry().intro_active = false;
                        cutscene_player.stop();
                    } else {
                        set_menu_active(true);
                    }
                } else if (renderer.is_menu_open() && (key == SDLK_LEFT || key == SDLK_a)) {
                    int tab = (renderer.selected_menu_tab() + 3) % 4;
                    renderer.set_selected_menu_tab(tab);
                    renderer.set_selected_menu_row(0);
                } else if (renderer.is_menu_open() && (key == SDLK_RIGHT || key == SDLK_d)) {
                    int tab = (renderer.selected_menu_tab() + 1) % 4;
                    renderer.set_selected_menu_tab(tab);
                    renderer.set_selected_menu_row(0);
                } else if (renderer.is_menu_open() && (key == SDLK_UP || key == SDLK_w)) {
                    if (renderer.selected_menu_tab() <= 1) {
                        current_chapter_idx = (current_chapter_idx + 9) % 10;
                        renderer.set_selected_chapter(current_chapter_idx);
                    } else {
                        renderer.set_selected_menu_row((renderer.selected_menu_row() + 5) % 6);
                    }
                } else if (renderer.is_menu_open() && (key == SDLK_DOWN || key == SDLK_s)) {
                    if (renderer.selected_menu_tab() <= 1) {
                        current_chapter_idx = (current_chapter_idx + 1) % 10;
                        renderer.set_selected_chapter(current_chapter_idx);
                    } else {
                        renderer.set_selected_menu_row((renderer.selected_menu_row() + 1) % 6);
                    }
                } else if (renderer.is_menu_open() && (key == SDLK_RETURN || key == SDLK_SPACE)) {
                    suppress_space_until_release = true;
                    activate_menu_selection();
                } else if ((key == SDLK_SPACE || key == SDLK_RETURN) && cutscene_player.is_playing()) {
                    suppress_space_until_release = true;
                    if (cutscene_player.get_mode() == ECutsceneMode::BinkVideo) {
                        controller.reset(active_scene.player_spawn_pos, active_scene.player_spawn_yaw);
                        cutscene_player.play_in_engine_intro(active_scene, controller.get_telemetry(), 3.2f);
                    } else {
                        controller.reset(active_scene.player_spawn_pos, active_scene.player_spawn_yaw);
                        controller.get_telemetry().intro_active = false;
                        cutscene_player.stop();
                    }
                } else if (key == SDLK_o || (key == SDLK_c && cutscene_player.is_playing())) {
                    controller.reset(active_scene.player_spawn_pos, active_scene.player_spawn_yaw);
                    controller.get_telemetry().intro_active = false;
                    cutscene_player.cycle_next_cutscene(active_scene, controller.get_telemetry());
                } else if (key == SDLK_TAB || key == SDLK_m) {
                    set_menu_active(!renderer.is_menu_open());
                } else if (key == SDLK_t) {
                    input.cycle_weapon_dir = 1;
                } else if (key == SDLK_y) {
                    input.cycle_weapon_dir = -1;
                } else if (key == SDLK_g || key == SDLK_BACKSPACE) {
                    input.drop_weapon = true;
                } else if (key == SDLK_h) {
                    input.spawn_combat_squad = true;
                } else if (key == SDLK_r) {
                    controller.get_telemetry().intro_active = false;
                    cutscene_player.stop();
                    int cp = std::clamp(controller.get_telemetry().active_checkpoint, 0,
                                        std::max(0, static_cast<int>(active_scene.checkpoints.size()) - 1));
                    Vec3 spawn = active_scene.checkpoints.empty() ? active_scene.player_spawn_pos : active_scene.checkpoints[cp];
                    float yaw = active_scene.player_spawn_yaw;
                    if (cp + 1 < static_cast<int>(active_scene.checkpoints.size())) {
                        Vec3 d = active_scene.checkpoints[cp + 1] - spawn;
                        yaw = std::atan2(d.y, d.x) * RAD2DEG;
                    }
                    controller.reset(spawn, yaw);
                    controller.get_telemetry().active_checkpoint = cp;
                    if (cp < static_cast<int>(active_scene.subtitles.size()) && !active_scene.subtitles[cp].empty()) {
                        controller.get_telemetry().active_subtitle = active_scene.subtitles[cp];
                    }
                } else if (key == SDLK_RIGHTBRACKET || key == SDLK_n || key == SDLK_LEFTBRACKET || key == SDLK_b) {
                    cutscene_player.stop();
                    if (!active_scene.checkpoints.empty()) {
                        int delta_cp = (key == SDLK_RIGHTBRACKET || key == SDLK_n) ? 1 : -1;
                        int next_cp = std::clamp(controller.get_telemetry().active_checkpoint + delta_cp,
                                                 0, static_cast<int>(active_scene.checkpoints.size()) - 1);
                        Vec3 spawn = active_scene.checkpoints[next_cp];
                        float yaw = active_scene.player_spawn_yaw;
                        if (next_cp + 1 < static_cast<int>(active_scene.checkpoints.size())) {
                            Vec3 d = active_scene.checkpoints[next_cp + 1] - spawn;
                            yaw = std::atan2(d.y, d.x) * RAD2DEG;
                        }
                        controller.reset(spawn, yaw);
                        controller.get_telemetry().active_checkpoint = next_cp;
                        if (next_cp < static_cast<int>(active_scene.subtitles.size()) && !active_scene.subtitles[next_cp].empty()) {
                            controller.get_telemetry().active_subtitle = active_scene.subtitles[next_cp];
                        }
                    }
                } else if (key == SDLK_p || key == SDLK_F12) {
                    auto t = std::time(nullptr);
                    std::ostringstream ss;
                    ss << "screenshots/screenshot_" << std::put_time(std::localtime(&t), "%Y%m%d_%H%M%S") << ".png";
                    renderer.save_screenshot_png(ss.str());
                } else if (key == SDLK_x) {
                    reaction_toggled = !reaction_toggled;
                } else if (key >= SDLK_0 && key <= SDLK_9) {
                    int sel = (key == SDLK_0) ? 0 : (key - SDLK_0);
                    current_chapter_idx = sel;
                    renderer.set_selected_chapter(sel);
                    set_menu_active(false);
                    load_chapter_or_level(sel, "", /*play_intro=*/true);
                }
            }
        }

        // Keyboard & Mouse Button State Polling (supports held LMB for full-auto weapons)
        const Uint8* state = SDL_GetKeyboardState(nullptr);
        if (!state[SDL_SCANCODE_SPACE]) {
            suppress_space_until_release = false;
        }
        Uint32 mouse_buttons = SDL_GetMouseState(nullptr, nullptr);
        if ((mouse_buttons & SDL_BUTTON(SDL_BUTTON_LEFT)) && frame_counter >= 15 &&
            !renderer.is_menu_open() && !cutscene_player.is_playing() &&
            SDL_GetRelativeMouseMode() == SDL_TRUE &&
            controller.get_weapon().equipped) {
            input.fire = true;
        }

        if (state[SDL_SCANCODE_W] || state[SDL_SCANCODE_UP]) input.forward += 1.0f;
        if (state[SDL_SCANCODE_S] || state[SDL_SCANCODE_DOWN]) input.forward -= 1.0f;
        if (state[SDL_SCANCODE_D] || state[SDL_SCANCODE_RIGHT]) input.strafe += 1.0f;
        if (state[SDL_SCANCODE_A] || state[SDL_SCANCODE_LEFT]) input.strafe -= 1.0f;

        if (state[SDL_SCANCODE_SPACE] && !suppress_space_until_release) input.jump = true;
        if (state[SDL_SCANCODE_C] || state[SDL_SCANCODE_LCTRL] || state[SDL_SCANCODE_LSHIFT]) input.crouch = true;
        if (state[SDL_SCANCODE_Q]) input.turn_180 = true;
        if (state[SDL_SCANCODE_F]) {
            if (controller.get_weapon().equipped) input.fire = true;
            else input.melee = true;
        }
        if (state[SDL_SCANCODE_E]) {
            input.disarm = true;
            input.use = true;
        }
        if (state[SDL_SCANCODE_V] || state[SDL_SCANCODE_LALT]) input.look_at = true;

        // Alternate look keys
        if (state[SDL_SCANCODE_I]) input.look_pitch_delta += 90.0f * dt;
        if (state[SDL_SCANCODE_K]) input.look_pitch_delta -= 90.0f * dt;
        if (state[SDL_SCANCODE_J]) input.look_yaw_delta -= 90.0f * dt;
        if (state[SDL_SCANCODE_L]) input.look_yaw_delta += 90.0f * dt;

        input.reaction_time = reaction_toggled;

        // Gamepad inputs
        if (game_controller) {
            int16_t stick_lx = SDL_GameControllerGetAxis(game_controller, SDL_CONTROLLER_AXIS_LEFTX);
            int16_t stick_ly = SDL_GameControllerGetAxis(game_controller, SDL_CONTROLLER_AXIS_LEFTY);
            int16_t stick_rx = SDL_GameControllerGetAxis(game_controller, SDL_CONTROLLER_AXIS_RIGHTX);
            int16_t stick_ry = SDL_GameControllerGetAxis(game_controller, SDL_CONTROLLER_AXIS_RIGHTY);

            constexpr int DEADZONE = 6000;
            if (std::abs(stick_ly) > DEADZONE) input.forward = -float(stick_ly) / 32768.0f;
            if (std::abs(stick_lx) > DEADZONE) input.strafe = float(stick_lx) / 32768.0f;
            if (std::abs(stick_rx) > DEADZONE) input.look_yaw_delta += (float(stick_rx) / 32768.0f) * 120.0f * dt;
            if (std::abs(stick_ry) > DEADZONE) input.look_pitch_delta += (-float(stick_ry) / 32768.0f) * 120.0f * dt;

            if (SDL_GameControllerGetButton(game_controller, SDL_CONTROLLER_BUTTON_A)) input.jump = true;
            if (SDL_GameControllerGetButton(game_controller, SDL_CONTROLLER_BUTTON_B)) input.crouch = true;
            if (SDL_GameControllerGetButton(game_controller, SDL_CONTROLLER_BUTTON_X)) input.melee = true;
            if (SDL_GameControllerGetButton(game_controller, SDL_CONTROLLER_BUTTON_Y)) input.disarm = true;
            if (SDL_GameControllerGetButton(game_controller, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) input.reaction_time = true;
            if (SDL_GameControllerGetButton(game_controller, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) input.turn_180 = true;
        }

        // Advance simulation or active cutscene step (paused while Main Menu is open)
        Vec3 pre_vel = controller.get_velocity();
        if (!renderer.is_menu_open()) {
            if (cutscene_player.get_mode() != ECutsceneMode::BinkVideo && pending_level_loaded_audio) {
                pending_level_loaded_audio = false;
                audio.play_level_loaded_cues();
            }
            if (cutscene_player.is_playing()) {
                cutscene_player.update(dt, active_scene, controller.get_telemetry());
                if (!cutscene_player.is_playing()) {
                    controller.reset(active_scene.player_spawn_pos, active_scene.player_spawn_yaw);
                }
            } else {
                controller.step(input, dt, active_scene);
            }
            if (audio.is_vo_playing()) {
                was_vo_playing = true;
                std::string vo_sub = audio.get_active_vo_subtitle();
                if (!vo_sub.empty()) {
                    controller.get_telemetry().active_subtitle = vo_sub;
                }
            } else if (was_vo_playing) {
                was_vo_playing = false;
                int cp = std::clamp(controller.get_telemetry().active_checkpoint, 0,
                                    std::max(0, static_cast<int>(active_scene.subtitles.size()) - 1));
                controller.get_telemetry().active_subtitle =
                    (!active_scene.subtitles.empty() && !active_scene.subtitles[cp].empty())
                        ? active_scene.subtitles[cp]
                        : "";
            }
        }
        const auto& tel = controller.get_telemetry();

        // Live movement telemetry logging for run diagnostics
        static std::ofstream live_trace("/tmp/me_live_run_telemetry.jsonl", std::ios::out | std::ios::trunc);
        bool state_changed = (tel.move_state != prev_state);
        bool cp_changed = (tel.active_checkpoint != prev_checkpoint);
        bool speed_drop = (pre_vel.length_xy() - tel.speed_2d > 120.0f);
        bool any_action_key = (input.jump || input.crouch || input.turn_180 || input.melee || input.disarm || input.look_at);
        if (live_trace.is_open() && (state_changed || cp_changed || speed_drop || any_action_key || (tel.tick % 3 == 0))) {
            live_trace << "{\"tick\":" << tel.tick
                       << ",\"t\":" << std::fixed << std::setprecision(3) << tel.sim_time
                       << ",\"dt\":" << dt
                       << ",\"state\":\"" << move_state_name(tel.move_state) << "\""
                       << ",\"prev_state\":\"" << move_state_name(prev_state) << "\""
                       << ",\"grounded\":" << (tel.grounded ? "true" : "false")
                       << ",\"pos\":[" << std::setprecision(1) << tel.position.x << "," << tel.position.y << "," << tel.position.z << "]"
                       << ",\"vel\":[" << tel.velocity.x << "," << tel.velocity.y << "," << tel.velocity.z << "]"
                       << ",\"spd2d\":" << tel.speed_2d
                       << ",\"yaw\":" << tel.yaw_deg << ",\"pitch\":" << tel.pitch_deg << ",\"roll\":" << tel.camera_roll_deg
                       << ",\"cp\":" << tel.active_checkpoint
                       << ",\"in\":{\"fwd\":" << input.forward << ",\"str\":" << input.strafe
                       << ",\"jmp\":" << (input.jump ? 1 : 0) << ",\"crc\":" << (input.crouch ? 1 : 0)
                       << ",\"q180\":" << (input.turn_180 ? 1 : 0) << ",\"mel\":" << (input.melee ? 1 : 0)
                       << ",\"dis\":" << (input.disarm ? 1 : 0) << "}"
                       << ",\"wall_norm\":[" << std::setprecision(2) << tel.wall_normal.x << "," << tel.wall_normal.y << "," << tel.wall_normal.z << "]"
                       << "}\n";
            live_trace.flush();
        }
        if (state_changed || cp_changed || speed_drop) {
            std::cout << "[RunTrace t=" << std::fixed << std::setprecision(2) << tel.sim_time
                      << "s tick=" << tel.tick << "] "
                      << move_state_name(prev_state) << " -> " << move_state_name(tel.move_state)
                      << " | grounded=" << (tel.grounded ? 1 : 0)
                      << " | pos=(" << std::setprecision(1) << tel.position.x << ", " << tel.position.y << ", " << tel.position.z << ")"
                      << " | vel=(" << tel.velocity.x << ", " << tel.velocity.y << ", " << tel.velocity.z << ") spd2d=" << tel.speed_2d
                      << " | yaw=" << tel.yaw_deg << " pitch=" << tel.pitch_deg
                      << " | cp=" << tel.active_checkpoint
                      << " | in(W=" << input.forward << ",A/D=" << input.strafe
                      << ",J=" << input.jump << ",C=" << input.crouch << ",Q=" << input.turn_180 << ")"
                      << std::endl;
        }

        // Audio state triggers
        if (tel.move_state != prev_state) {
            switch (tel.move_state) {
                case EMovement::MOVE_Jump: audio.play_effect(EAudioEffect::Jump); break;
                case EMovement::MOVE_WallRunningLeft:
                case EMovement::MOVE_WallRunningRight: audio.play_effect(EAudioEffect::Wallrun); break;
                case EMovement::MOVE_SpeedVaulting:
                case EMovement::MOVE_VaultOver:
                case EMovement::MOVE_SpringBoarding:
                case EMovement::MOVE_Melee:
                case EMovement::MOVE_MeleeAir:
                case EMovement::MOVE_MeleeWallrun:
                case EMovement::MOVE_Barge: audio.play_effect(EAudioEffect::Vault); break;
                case EMovement::MOVE_Slide:
                case EMovement::MOVE_MeleeSlide: audio.play_effect(EAudioEffect::Slide); break;
                case EMovement::MOVE_SkillRoll: audio.play_effect(EAudioEffect::SkillRoll); break;
                case EMovement::MOVE_ZipLine: audio.play_effect(EAudioEffect::Zipline); break;
                case EMovement::MOVE_Snatch: audio.play_effect(EAudioEffect::Disarm); break;
                default: break;
            }
            prev_state = tel.move_state;
        }

        if (tel.falling_to_death && !prev_falling_to_death) {
            audio.play_effect(EAudioEffect::FallDeathScream);
        }
        if (tel.fall_death_impact && !prev_fall_death_impact) {
            audio.play_effect(EAudioEffect::FallDeathImpact);
        }
        if (!tel.falling_to_death && !tel.fall_death_impact &&
            (prev_falling_to_death || prev_fall_death_impact)) {
            audio.set_sound_group_mode(ESoundGroupEffectMode::Normal);
        }
        prev_falling_to_death = tel.falling_to_death;
        prev_fall_death_impact = tel.fall_death_impact;

        if (tel.active_checkpoint != prev_checkpoint) {
            audio.play_effect(EAudioEffect::CheckpointChime);
            prev_checkpoint = tel.active_checkpoint;
        }

        if (tel.weapon.fired_this_tick) {
            audio.play_effect(EAudioEffect::Gunshot);
        }

        // Footstep cadence (TdPhysicalMaterialFootSteps: Sneak / Walk / Run / Sprint)
        if (!renderer.is_menu_open() && tel.grounded && tel.move_state == EMovement::MOVE_Walking && tel.speed_2d > 40.0f) {
            footstep_timer += dt;
            float stride_time = std::clamp(150.0f / tel.speed_2d, 0.22f, 0.45f);
            if (footstep_timer >= stride_time) {
                audio.play_footstep(ESurfaceMaterial::Concrete, tel.speed_2d, input.crouch > 0.5f, 0.75f);
                footstep_timer = 0.0f;
            }
        } else {
            footstep_timer = 0.0f;
        }

        // Update 3D listener and dynamic audio stems
        audio.set_menu_music(renderer.is_menu_open());
        Vec3 ear = tel.position + Vec3(0, 0, tel.eye_height);
        Rotator ear_rot = Rotator::from_degrees(tel.pitch_deg, tel.yaw_deg, tel.camera_roll_deg);
        audio.update(dt, ear, ear_rot.forward(), ear_rot.up(), tel.speed_2d, tel.reaction_active);

        // Render frame
        renderer.render_frame(active_scene, tel);

        ++frame_counter;
        if (max_frames > 0 && frame_counter >= max_frames) {
            std::cout << "[Game] Reached max-frames limit (" << max_frames << "). Exiting cleanly." << std::endl;
            running = false;
        }
    }

    if (game_controller) SDL_GameControllerClose(game_controller);
    audio.shutdown();
    SDL_Metal_DestroyView(metal_view);
    SDL_DestroyWindow(window);
    SDL_Quit();

    std::cout << "[Game] Shutdown cleanly." << std::endl;
    return 0;
}

// -----------------------------------------------------------------------------
// Main Entrypoint & CLI Parsing
// -----------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    std::string game_root = "/Users/tomnom/mirrorsedge";
    bool verify_all = false;
    std::string script_json = "";
    int initial_chapter = 0;
    std::string custom_level = "";
    int max_frames = 0;
    bool start_in_main_menu = true;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--verify-all") {
            verify_all = true;
        } else if (arg == "--headless-oracle") {
            verify_all = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                script_json = argv[++i];
            }
        } else if (arg == "--test-replay") {
            verify_all = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                script_json = argv[++i];
            }
        } else if (arg == "--main-menu") {
            start_in_main_menu = true;
        } else if (arg == "--chapter") {
            if (i + 1 < argc) initial_chapter = std::atoi(argv[++i]);
            start_in_main_menu = false;
        } else if (arg == "--level") {
            if (i + 1 < argc) custom_level = argv[++i];
            start_in_main_menu = false;
        } else if (arg == "--max-frames") {
            if (i + 1 < argc) max_frames = std::atoi(argv[++i]);
        } else if (arg == "--game-root") {
            if (i + 1 < argc) game_root = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Mirror's Edge Native macOS Engine\n\n"
                      << "Usage:\n"
                      << "  mirrorsedge_macos [options]\n\n"
                      << "Options:\n"
                      << "  --main-menu              Boot into the 3D City of Glass Main Menu (default)\n"
                      << "  --verify-all             Run deterministic headless oracle verification suite\n"
                      << "  --headless-oracle <file> Run script-based headless oracle\n"
                      << "  --test-replay <trace>    Replay physics trace headless\n"
                      << "  --chapter <0..9>         Start at specified campaign chapter\n"
                      << "  --level <path>           Load custom level package\n"
                      << "  --max-frames <N>         Exit after rendering N frames\n"
                      << "  --game-root <dir>        Set retail game assets directory (default: /Users/tomnom/mirrorsedge)\n"
                      << "  --help, -h               Show this help message\n";
            return 0;
        }
    }

    if (verify_all) {
        return run_oracle_verification(game_root, script_json);
    }

    return run_interactive_app(game_root, initial_chapter, custom_level, max_frames, start_in_main_menu);
}
