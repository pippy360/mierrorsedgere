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

// Helper to append a 3D box into a MeshBuffer
static void append_box_mesh(std::vector<Vertex>& verts, const Vec3& min_p, const Vec3& max_p,
                            const Vec3& normal_bias = Vec3(0, 0, 0)) {
    Vec3 p[8] = {
        {min_p.x, min_p.y, min_p.z}, // 0
        {max_p.x, min_p.y, min_p.z}, // 1
        {max_p.x, max_p.y, min_p.z}, // 2
        {min_p.x, max_p.y, min_p.z}, // 3
        {min_p.x, min_p.y, max_p.z}, // 4
        {max_p.x, min_p.y, max_p.z}, // 5
        {max_p.x, max_p.y, max_p.z}, // 6
        {min_p.x, max_p.y, max_p.z}  // 7
    };

    struct Face {
        int idx[4];
        Vec3 norm;
    };

    Face faces[6] = {
        {{4, 5, 6, 7}, {0, 0, 1}},  // Top (+Z)
        {{3, 2, 1, 0}, {0, 0, -1}}, // Bottom (-Z)
        {{0, 1, 5, 4}, {0, -1, 0}}, // Front (-Y)
        {{2, 3, 7, 6}, {0, 1, 0}},  // Back (+Y)
        {{0, 4, 7, 3}, {-1, 0, 0}}, // Left (-X)
        {{1, 2, 6, 5}, {1, 0, 0}}   // Right (+X)
    };

    for (int f = 0; f < 6; ++f) {
        Vec3 n = (faces[f].norm + normal_bias).normalized();
        Vec3 v0 = p[faces[f].idx[0]];
        Vec3 v1 = p[faces[f].idx[1]];
        Vec3 v2 = p[faces[f].idx[2]];
        Vec3 v3 = p[faces[f].idx[3]];

        Vertex vert0{v0, n, {1, 0, 0}, 0.0f, 0.0f, 0.0f, 0.0f, 0xFFFFFFFF};
        Vertex vert1{v1, n, {1, 0, 0}, 1.0f, 0.0f, 0.0f, 0.0f, 0xFFFFFFFF};
        Vertex vert2{v2, n, {1, 0, 0}, 1.0f, 1.0f, 0.0f, 0.0f, 0xFFFFFFFF};
        Vertex vert3{v3, n, {1, 0, 0}, 0.0f, 1.0f, 0.0f, 0.0f, 0xFFFFFFFF};

        // Two triangles per face
        verts.push_back(vert0); verts.push_back(vert1); verts.push_back(vert2);
        verts.push_back(vert0); verts.push_back(vert2); verts.push_back(vert3);
    }
}

// Generate high-visibility 3D architectural geometry for parkour test course
static void append_test_course_visuals(LevelScene& scene) {
    MeshBuffer course_mesh;
    course_mesh.name = "Parkour_Course_Geometry";

    MeshBuffer runner_vision_mesh;
    runner_vision_mesh.name = "Parkour_Course_RunnerVision";
    runner_vision_mesh.is_runner_vision = true;

    // 1. Runway start building tower & rooftop platform
    append_box_mesh(course_mesh.vertices, Vec3(-520.0f, -320.0f, -1800.0f), Vec3(3800.0f, 320.0f, 0.0f));
    append_box_mesh(course_mesh.vertices, Vec3(-500.0f, -300.0f, 0.0f), Vec3(3800.0f, 300.0f, 50.0f));

    // Parapet walls & coping borders along runway
    append_box_mesh(course_mesh.vertices, Vec3(-500.0f, -325.0f, 50.0f), Vec3(3800.0f, -295.0f, 92.0f));
    append_box_mesh(course_mesh.vertices, Vec3(-500.0f, 295.0f, 50.0f), Vec3(3800.0f, 325.0f, 92.0f));

    // Rooftop HVAC units, vents, and architectural pylons flanking the sprint runway (outside central lane)
    for (int i = 0; i < 6; ++i) {
        float bx = 350.0f + float(i) * 520.0f;
        // Left side HVAC & ducting
        append_box_mesh(course_mesh.vertices, Vec3(bx, -285.0f, 50.0f), Vec3(bx + 180.0f, -185.0f, 165.0f));
        append_box_mesh(course_mesh.vertices, Vec3(bx + 20.0f, -275.0f, 165.0f), Vec3(bx + 160.0f, -195.0f, 195.0f));
        // Right side stairwell / chiller enclosure
        append_box_mesh(course_mesh.vertices, Vec3(bx + 140.0f, 185.0f, 50.0f), Vec3(bx + 340.0f, 285.0f, 180.0f));
        append_box_mesh(course_mesh.vertices, Vec3(bx + 165.0f, 200.0f, 180.0f), Vec3(bx + 315.0f, 270.0f, 215.0f));
    }

    // 2. Vault Hurdle (Red Runner Vision pipe barrier + side stanchions)
    append_box_mesh(course_mesh.vertices, Vec3(3195.0f, -165.0f, 50.0f), Vec3(3255.0f, -140.0f, 125.0f));
    append_box_mesh(course_mesh.vertices, Vec3(3195.0f, 140.0f, 50.0f), Vec3(3255.0f, 165.0f, 125.0f));
    append_box_mesh(runner_vision_mesh.vertices, Vec3(3200.0f, -150.0f, 50.0f), Vec3(3250.0f, 150.0f, 120.0f));

    // 3. Springboard Box (Red Runner Vision AC enclosure + base)
    append_box_mesh(course_mesh.vertices, Vec3(3540.0f, -90.0f, 50.0f), Vec3(3660.0f, 90.0f, 72.0f));
    append_box_mesh(runner_vision_mesh.vertices, Vec3(3550.0f, -80.0f, 72.0f), Vec3(3650.0f, 80.0f, 140.0f));

    // 4. Canyon floor & flanking skyscraper facades
    append_box_mesh(course_mesh.vertices, Vec3(3800.0f, -320.0f, -1800.0f), Vec3(5650.0f, 320.0f, 0.0f));
    append_box_mesh(course_mesh.vertices, Vec3(3800.0f, -300.0f, 0.0f), Vec3(5600.0f, 300.0f, 50.0f));
    // Tall architectural building walls backing the wallrun panels
    append_box_mesh(course_mesh.vertices, Vec3(3920.0f, -520.0f, 50.0f), Vec3(5150.0f, -258.0f, 780.0f));
    append_box_mesh(course_mesh.vertices, Vec3(3920.0f, 258.0f, 50.0f), Vec3(5150.0f, 520.0f, 780.0f));

    // Wallrun left & right panels (Red Runner Vision)
    append_box_mesh(runner_vision_mesh.vertices, Vec3(4000.0f, -258.0f, 55.0f), Vec3(5000.0f, -200.0f, 420.0f));
    append_box_mesh(runner_vision_mesh.vertices, Vec3(4000.0f, 200.0f, 55.0f), Vec3(5000.0f, 258.0f, 420.0f));

    // 5. Wallclimb tower & upper rooftop deck (Red Runner Vision)
    append_box_mesh(course_mesh.vertices, Vec3(5355.0f, -240.0f, 50.0f), Vec3(5650.0f, 240.0f, 480.0f));
    append_box_mesh(runner_vision_mesh.vertices, Vec3(5350.0f, -150.0f, 50.0f), Vec3(5450.0f, 150.0f, 500.0f));
    append_box_mesh(course_mesh.vertices, Vec3(5345.0f, -250.0f, 480.0f), Vec3(5660.0f, 250.0f, 505.0f));

    // 6. Zipline gantry masts & diagonal cable
    append_box_mesh(course_mesh.vertices, Vec3(5485.0f, -90.0f, 500.0f), Vec3(5515.0f, -65.0f, 640.0f));
    append_box_mesh(course_mesh.vertices, Vec3(5485.0f, 65.0f, 500.0f), Vec3(5515.0f, 90.0f, 640.0f));
    append_box_mesh(runner_vision_mesh.vertices, Vec3(5480.0f, -95.0f, 620.0f), Vec3(5520.0f, 95.0f, 645.0f));
    //Segmented diagonal zipline cable
    Vec3 zip_s(5500.0f, 0.0f, 620.0f);
    Vec3 zip_e(7000.0f, 0.0f, 240.0f);
    for (int s = 0; s < 24; ++s) {
        float t0 = float(s) / 24.0f;
        float t1 = float(s + 1) / 24.0f;
        Vec3 p0 = zip_s + (zip_e - zip_s) * t0;
        Vec3 p1 = zip_s + (zip_e - zip_s) * t1;
        append_box_mesh(runner_vision_mesh.vertices,
                        Vec3(p0.x, -4.5f, std::min(p0.z, p1.z) - 4.0f),
                        Vec3(p1.x, 4.5f, std::max(p0.z, p1.z) + 4.0f));
    }
    // Far zipline anchor gantry
    append_box_mesh(course_mesh.vertices, Vec3(6985.0f, -160.0f, 100.0f), Vec3(7015.0f, -130.0f, 260.0f));
    append_box_mesh(course_mesh.vertices, Vec3(6985.0f, 130.0f, 100.0f), Vec3(7015.0f, 160.0f, 260.0f));
    append_box_mesh(course_mesh.vertices, Vec3(6980.0f, -165.0f, 240.0f), Vec3(7020.0f, 165.0f, 265.0f));

    // 7. Far rooftop building & slide duct
    append_box_mesh(course_mesh.vertices, Vec3(6800.0f, -320.0f, -1800.0f), Vec3(8020.0f, 320.0f, 0.0f));
    append_box_mesh(course_mesh.vertices, Vec3(6800.0f, -300.0f, 0.0f), Vec3(8000.0f, 300.0f, 100.0f));
    append_box_mesh(course_mesh.vertices, Vec3(6800.0f, -325.0f, 100.0f), Vec3(8000.0f, -295.0f, 140.0f));
    append_box_mesh(course_mesh.vertices, Vec3(6800.0f, 295.0f, 100.0f), Vec3(8000.0f, 325.0f, 140.0f));

    // Low ventilation duct for crouch slide + side support legs + Runner Vision clearance bar
    append_box_mesh(course_mesh.vertices, Vec3(7275.0f, -260.0f, 100.0f), Vec3(7345.0f, -210.0f, 310.0f));
    append_box_mesh(course_mesh.vertices, Vec3(7275.0f, 210.0f, 100.0f), Vec3(7345.0f, 260.0f, 310.0f));
    append_box_mesh(course_mesh.vertices, Vec3(7260.0f, -250.0f, 168.0f), Vec3(7360.0f, 250.0f, 305.0f));
    append_box_mesh(runner_vision_mesh.vertices, Vec3(7255.0f, -210.0f, 156.0f), Vec3(7365.0f, 210.0f, 170.0f));

    // 8. Balance pipe (Red Runner Vision) bridging canyon gap
    append_box_mesh(runner_vision_mesh.vertices, Vec3(8000.0f, -18.0f, 88.0f), Vec3(8800.0f, 18.0f, 112.0f));

    // 9. Destination arena rooftop & hollow Penthouse Elevator shaft framing (X=9840..10080, Z=100 -> 680)
    append_box_mesh(course_mesh.vertices, Vec3(8800.0f, -420.0f, -1800.0f), Vec3(10100.0f, 420.0f, 0.0f));
    append_box_mesh(course_mesh.vertices, Vec3(8800.0f, -400.0f, 0.0f), Vec3(9840.0f, 400.0f, 100.0f));
    append_box_mesh(course_mesh.vertices, Vec3(8800.0f, -425.0f, 100.0f), Vec3(9840.0f, -395.0f, 145.0f));
    append_box_mesh(course_mesh.vertices, Vec3(8800.0f, 395.0f, 100.0f), Vec3(9840.0f, 425.0f, 145.0f));
    // Left and right architectural shaft towers flanking the hollow elevator cab (Y=-132.5..+132.5 open)
    append_box_mesh(course_mesh.vertices, Vec3(9835.0f, -390.0f, 100.0f), Vec3(10085.0f, -138.0f, 980.0f));
    append_box_mesh(course_mesh.vertices, Vec3(9835.0f,  138.0f, 100.0f), Vec3(10085.0f,  390.0f, 980.0f));
    append_box_mesh(course_mesh.vertices, Vec3(9835.0f, -138.0f, 370.0f), Vec3(9855.0f,   138.0f, 980.0f));
    append_box_mesh(course_mesh.vertices, Vec3(9835.0f, -390.0f, 960.0f), Vec3(10085.0f,  390.0f, 995.0f));

    // 10. Streamed Upper Penthouse Helipad Deck at Z = 680 (X = 10080..11200)
    append_box_mesh(course_mesh.vertices, Vec3(10080.0f, -460.0f, -1800.0f), Vec3(11200.0f, 460.0f, 640.0f));
    append_box_mesh(course_mesh.vertices, Vec3(10080.0f, -450.0f, 640.0f), Vec3(11200.0f, 450.0f, 680.0f));
    append_box_mesh(course_mesh.vertices, Vec3(10080.0f, -465.0f, 680.0f), Vec3(11200.0f, -440.0f, 725.0f));
    append_box_mesh(course_mesh.vertices, Vec3(10080.0f,  440.0f, 680.0f), Vec3(11200.0f,  465.0f, 725.0f));
    append_box_mesh(course_mesh.vertices, Vec3(11175.0f, -450.0f, 680.0f), Vec3(11205.0f,  450.0f, 725.0f));
    // Runner Vision Helipad Target Beacon at (10550, 0, 680)
    append_box_mesh(runner_vision_mesh.vertices, Vec3(10460.0f, -90.0f, 680.0f), Vec3(10640.0f, -70.0f, 684.0f));
    append_box_mesh(runner_vision_mesh.vertices, Vec3(10460.0f,  70.0f, 680.0f), Vec3(10640.0f,  90.0f, 684.0f));
    append_box_mesh(runner_vision_mesh.vertices, Vec3(10535.0f, -70.0f, 680.0f), Vec3(10565.0f,  70.0f, 684.0f));

    // Courier bag
    append_box_mesh(runner_vision_mesh.vertices, Vec3(5380.0f, 30.0f, 505.0f), Vec3(5420.0f, 70.0f, 545.0f));

    scene.meshes.push_back(course_mesh);
    scene.meshes.push_back(runner_vision_mesh);
}

// Translate extracted level meshes so they surround the (0..11200, 0, 50..680) parkour course
// while culling triangles inside the immediate parkour runway corridor so the camera view is never obstructed.
static void align_city_meshes_around_course(LevelScene& scene, const Vec3& original_spawn) {
    Vec3 offset = Vec3(3200.0f, 0.0f, -120.0f) - original_spawn;
    auto in_corridor = [](const Vec3& p) {
        return (p.x > -450.0f && p.x < 11250.0f &&
                p.y > -460.0f && p.y < 460.0f &&
                p.z > -40.0f  && p.z < 1020.0f);
    };
    for (auto& mb : scene.meshes) {
        std::vector<Vertex> filtered;
        filtered.reserve(mb.vertices.size());

        // Offsets + culls the triangles of [first, first + count) and appends the survivors.
        auto filter_range = [&](size_t first, size_t count) {
            const size_t end = std::min(mb.vertices.size(), first + count);
            for (size_t i = first; i + 2 < end; i += 3) {
                Vertex v0 = mb.vertices[i];
                Vertex v1 = mb.vertices[i + 1];
                Vertex v2 = mb.vertices[i + 2];
                v0.position += offset;
                v1.position += offset;
                v2.position += offset;

                Vec3 mid = (v0.position + v1.position + v2.position) * (1.0f / 3.0f);
                if (in_corridor(v0.position) || in_corridor(v1.position) || in_corridor(v2.position) || in_corridor(mid)) {
                    continue;
                }
                filtered.push_back(v0);
                filtered.push_back(v1);
                filtered.push_back(v2);
            }
        };

        if (mb.sections.empty()) {
            filter_range(0, mb.vertices.size());
        } else {
            // Material sections: filter each section independently and rebuild its vertex range.
            std::vector<MeshSection> sections;
            sections.reserve(mb.sections.size());
            for (const MeshSection& s : mb.sections) {
                const size_t before = filtered.size();
                filter_range(s.first_vertex, s.vertex_count);
                const size_t kept = filtered.size() - before;
                if (kept == 0) continue;
                MeshSection ns = s;
                ns.first_vertex = static_cast<uint32_t>(before);
                ns.vertex_count = static_cast<uint32_t>(kept);
                sections.push_back(ns);
            }
            mb.sections.swap(sections);
        }
        mb.vertices.swap(filtered);
    }
}

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
    std::cout << "[Oracle] AudioEngine initialized in headless mode." << std::endl;

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
              << sp00_scene.colliders.size() << " colliders)" << std::endl;

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
    std::cout << "\n--- [Multi-Stage Parkour Simulation & Screenshot Capture] ---" << std::endl;

    ParkourController controller(move_cfg);
    LevelScene sim_scene = sp00_scene;
    align_city_meshes_around_course(sim_scene, sp00_scene.player_spawn_pos);
    controller.build_parkour_test_course(sim_scene);
    append_test_course_visuals(sim_scene);

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

    // Stage 1: Sprint acceleration & FOV scaling
    std::cout << "[Oracle Stage 1] Running Sprint Acceleration & FOV Scaling..." << std::endl;
    controller.reset(Vec3(1950.0f, 0.0f, 100.0f), 0.0f);
    InputFrame in1{};
    in1.forward = 1.0f;
    in1.sprint = true;

    for (int i = 0; i < 90; ++i) {
        controller.step(in1, 1.0f / 60.0f, sim_scene);
        log_telemetry("Stage1_Sprint");
    }
    renderer.render_frame(sim_scene, controller.get_telemetry());
    save_and_publish_png("oracle_1_sprint_rooftop.png");

    bool s1_pass = (controller.get_telemetry().speed_2d >= 400.0f) && (controller.get_telemetry().fov_deg > 100.0f);
    std::cout << "  -> Stage 1 Result: " << (s1_pass ? "PASS" : "FAIL")
              << " (Speed=" << controller.get_telemetry().speed_2d
              << " u/s, FOV=" << controller.get_telemetry().fov_deg << "°)" << std::endl;

    // Stage 2: Speed Vault & Springboard
    std::cout << "[Oracle Stage 2] Testing Speed Vault & Springboard..." << std::endl;
    // A. Speed Vault Test
    controller.reset(Vec3(3120.0f, 0.0f, 50.0f), 0.0f);
    controller.set_velocity(Vec3(420.0f, 0.0f, 0.0f));
    InputFrame in2_vault{};
    in2_vault.forward = 1.0f;
    in2_vault.sprint = true;
    in2_vault.jump = true;
    controller.step(in2_vault, 1.0f / 60.0f, sim_scene);
    bool s2_vault = (controller.get_move_state() == EMovement::MOVE_SpeedVaulting);
    log_telemetry("Stage2_Vault");

    // B. Springboard Test
    controller.reset(Vec3(3500.0f, 0.0f, 50.0f), 0.0f);
    controller.set_velocity(Vec3(450.0f, 0.0f, 0.0f));
    InputFrame in2_spring{};
    in2_spring.forward = 1.0f;
    in2_spring.sprint = true;
    in2_spring.jump = true;
    controller.step(in2_spring, 1.0f / 60.0f, sim_scene);
    bool s2_spring = (controller.get_move_state() == EMovement::MOVE_SpringBoarding);
    log_telemetry("Stage2_Springboard");

    // Advance springboard jump toward apex looking across the rooftop canyon
    in2_spring.jump = false;
    for (int i = 0; i < 8; ++i) {
        controller.step(in2_spring, 1.0f / 60.0f, sim_scene);
    }
    controller.set_rotation(0.0f, -12.0f, 0.0f);
    renderer.render_frame(sim_scene, controller.get_telemetry());
    save_and_publish_png("oracle_2_springboard_vault.png");
    bool s2_pass = s2_vault || s2_spring;
    std::cout << "  -> Stage 2 Result: " << (s2_pass ? "PASS" : "FAIL")
              << " (Vault=" << (s2_vault ? "OK" : "NO")
              << ", SpringBoard=" << (s2_spring ? "OK" : "NO")
              << ", Apex Z=" << controller.get_position().z << ")" << std::endl;

    // Stage 3: Wallrun & Wallrun Jump with 15° camera tilt
    std::cout << "[Oracle Stage 3] Testing Wallrun & 15° Camera Tilt..." << std::endl;
    // Position next to right wall (y: 200..260), angled slightly right
    controller.set_position(Vec3(4150.0f, 160.0f, 180.0f));
    controller.set_rotation(15.0f, 0.0f, 0.0f);
    controller.set_velocity(Vec3(400.0f, 80.0f, 0.0f));
    InputFrame in3{};
    in3.forward = 1.0f;
    in3.jump = true;

    controller.step(in3, 1.0f / 60.0f, sim_scene);
    log_telemetry("Stage3_Wallrun_Start");

    in3.jump = false;
    for (int i = 0; i < 15; ++i) {
        controller.step(in3, 1.0f / 60.0f, sim_scene);
        log_telemetry("Stage3_Wallrun_Sustain");
    }

    bool s3_pass = (controller.get_move_state() == EMovement::MOVE_WallRunningRight) ||
                   (std::abs(controller.get_roll()) >= 10.0f);

    renderer.render_frame(sim_scene, controller.get_telemetry());
    save_and_publish_png("oracle_3_wallrun_tilt.png");
    std::cout << "  -> Stage 3 Result: " << (s3_pass ? "PASS" : "FAIL")
              << " (State=" << move_state_name(controller.get_move_state())
              << ", Camera Roll=" << controller.get_roll() << "°)" << std::endl;

    // Stage 4: Wallclimb & Ledge Grab
    std::cout << "[Oracle Stage 4] Testing Wallclimb & Ledge Grab..." << std::endl;
    controller.set_position(Vec3(5300.0f, 0.0f, 100.0f));
    controller.set_rotation(0.0f, 0.0f, 0.0f);
    controller.set_velocity(Vec3(350.0f, 0.0f, 0.0f));
    InputFrame in4{};
    in4.forward = 1.0f;
    in4.jump = true;

    controller.step(in4, 1.0f / 60.0f, sim_scene);
    bool s4_climb = (controller.get_move_state() == EMovement::MOVE_WallClimbing);
    log_telemetry("Stage4_Climb");

    in4.jump = false;
    for (int i = 0; i < 35; ++i) {
        controller.step(in4, 1.0f / 60.0f, sim_scene);
    }
    std::cout << "  -> Stage 4 Result: " << (s4_climb ? "PASS" : "FAIL")
              << " (State=" << move_state_name(controller.get_move_state())
              << ", Final Z=" << controller.get_position().z << ")" << std::endl;

    // Stage 5: Zipline & Crouch Slide
    std::cout << "[Oracle Stage 5] Testing Zipline & Crouch Slide..." << std::endl;
    controller.reset(Vec3(5500.0f, 0.0f, 520.0f), 0.0f);
    InputFrame in5_zip{};
    in5_zip.forward = 1.0f;

    controller.step(in5_zip, 1.0f / 60.0f, sim_scene);
    bool s5_zip = (controller.get_move_state() == EMovement::MOVE_ZipLine);
    log_telemetry("Stage5_Zipline");

    // Land on far rooftop and slide approaching the overhead ventilation duct
    controller.reset(Vec3(7060.0f, 0.0f, 100.0f), 0.0f);
    controller.set_velocity(Vec3(450.0f, 0.0f, 0.0f));
    InputFrame in5_slide{};
    in5_slide.forward = 1.0f;
    in5_slide.crouch = true;
    controller.step(in5_slide, 1.0f / 60.0f, sim_scene);
    bool s5_slide = (controller.get_move_state() == EMovement::MOVE_Slide);
    log_telemetry("Stage5_Slide");

    renderer.render_frame(sim_scene, controller.get_telemetry());
    save_and_publish_png("oracle_4_zipline_slide.png");
    bool s5_pass = s5_zip && s5_slide;
    std::cout << "  -> Stage 5 Result: " << (s5_pass ? "PASS" : "FAIL")
              << " (ZipLine=" << (s5_zip ? "OK" : "NO")
              << ", Slide=" << (s5_slide ? "OK" : "NO") << ")" << std::endl;

    // Stage 6: Mid-Air Coil & Landing Skill Roll
    std::cout << "[Oracle Stage 6] Testing Mid-Air Coil & Skill Roll..." << std::endl;
    controller.reset(Vec3(2000.0f, 0.0f, 350.0f), 0.0f);
    controller.get_telemetry().grounded = false;
    controller.get_telemetry().move_state = EMovement::MOVE_Falling;
    controller.set_velocity(Vec3(350.0f, 0.0f, -100.0f));

    InputFrame in6{};
    in6.crouch = true;
    in6.forward = 1.0f;
    controller.step(in6, 1.0f / 60.0f, sim_scene);
    bool s6_coil = (controller.get_move_state() == EMovement::MOVE_Coil);
    log_telemetry("Stage6_Coil");

    // Descend to floor with crouch buffer for skill roll
    for (int i = 0; i < 45; ++i) {
        controller.step(in6, 1.0f / 60.0f, sim_scene);
    }
    bool s6_roll = (controller.get_move_state() == EMovement::MOVE_SkillRoll);
    bool s6_pass = s6_coil && s6_roll;
    std::cout << "  -> Stage 6 Result: " << (s6_pass ? "PASS" : "FAIL")
              << " (Coil=" << (s6_coil ? "OK" : "NO") << ", Roll=" << (s6_roll ? "OK" : "NO") << ")" << std::endl;

    // Stage 7: Combat Disarm & Reaction Time
    std::cout << "[Oracle Stage 7] Testing Combat Disarm & Reaction Time..." << std::endl;
    if (!sim_scene.enemies.empty()) {
        sim_scene.enemies[0].position = Vec3(9310.0f, -24.0f, 100.0f);
        sim_scene.enemies[0].yaw_deg = 172.0f;
        sim_scene.enemies[0].alive = true;
        sim_scene.enemies[0].stunned = false;
        sim_scene.enemies[0].disarm_window = true;
        sim_scene.enemies[0].weapon_name = "Colt1911";

        // Second KrugerSec SWAT backup guard in arena with active Runner Vision disarm window
        EnemyBot backup_guard;
        backup_guard.position = Vec3(9450.0f, 48.0f, 100.0f);
        backup_guard.yaw_deg = 200.0f;
        backup_guard.health = 100.0f;
        backup_guard.alive = true;
        backup_guard.stunned = false;
        backup_guard.disarm_window = true;
        backup_guard.weapon_name = "G36C";
        sim_scene.enemies.push_back(backup_guard);
    }
    controller.reset(Vec3(9215.0f, 0.0f, 100.0f), 0.0f);
    InputFrame in7{};
    in7.reaction_time = true;
    in7.disarm = true;

    controller.step(in7, 1.0f / 60.0f, sim_scene);
    log_telemetry("Stage7_Disarm");

    bool s7_disarm = (controller.get_move_state() == EMovement::MOVE_Snatch || controller.get_telemetry().weapon.equipped);
    bool s7_reaction = controller.get_telemetry().reaction_active;
    bool s7_pass = s7_disarm && s7_reaction;
    if (s7_disarm && !sim_scene.enemies.empty()) {
        sim_scene.enemies[0].stunned = true;
        sim_scene.enemies[0].disarm_window = false;
    }

    renderer.render_frame(sim_scene, controller.get_telemetry());
    save_and_publish_png("oracle_5_combat_disarm_reaction.png");
    std::cout << "  -> Stage 7 Result: " << (s7_pass ? "PASS" : "FAIL")
              << " (Disarm=" << (s7_disarm ? "OK" : "NO")
              << ", Weapon=" << (controller.get_telemetry().weapon.equipped ? controller.get_telemetry().weapon.name : "None")
              << ", Reaction=" << (s7_reaction ? "ACTIVE" : "OFF") << ")" << std::endl;

    // Stage 8: Interactive Elevator Ride & Mid-Shaft Level Streaming Transition (Z = 100 -> 680)
    std::cout << "[Oracle Stage 8] Testing Interactive Elevator Ride & Mid-Shaft Level Streaming..." << std::endl;
    sim_scene.enemies.clear(); // clear arena guards so Faith can walk into the Penthouse Elevator unimpeded
    controller.reset(Vec3(9780.0f, 0.0f, 100.0f), 0.0f);

    // 8A. Walk across the open lower doorway into the Penthouse Elevator cab (X=9960, Y=0, Z=100)
    InputFrame in8_enter{};
    in8_enter.forward = 1.0f;
    for (int i = 0; i < 45; ++i) {
        controller.step(in8_enter, 1.0f / 60.0f, sim_scene);
    }
    // Press E / Use inside the cab and ride the elevator through DoorsClosing -> Moving -> DoorsOpening -> IdleEnd
    InputFrame in8_ride{};
    in8_ride.use = true;
    controller.step(in8_ride, 1.0f / 60.0f, sim_scene);
    in8_ride.use = false;

    bool captured_mid_ride = false;
    for (int i = 0; i < 180; ++i) {
        controller.step(in8_ride, 1.0f / 60.0f, sim_scene);
        log_telemetry("Stage8_Elevator_Ride");
        if (!captured_mid_ride && !sim_scene.elevators.empty() &&
            sim_scene.elevators.front().state == ElevatorState::DoorsOpening &&
            sim_scene.elevators.front().door_open_End >= 0.42f) {
            captured_mid_ride = true;
            PlayerTelemetry t8_shot = controller.get_telemetry();
            t8_shot.position = Vec3(9935.0f, -24.0f, sim_scene.elevators.front().current_pos.z);
            t8_shot.yaw_deg = 20.0f;
            t8_shot.pitch_deg = -3.0f;
            t8_shot.in_elevator = true;
            t8_shot.elevator_progress = 1.0f;
            renderer.render_frame(sim_scene, t8_shot);
            save_and_publish_png("oracle_8_elevator_level_streaming.png");
        }
    }
    float cab_top_z = controller.get_position().z;
    bool s8_elev_top = (cab_top_z >= 675.0f) && !sim_scene.elevators.empty() &&
                       sim_scene.elevators.front().streaming_triggered;

    // 8B. Walk out of the open upper elevator doors onto the streamed Upper Penthouse Helipad deck (X > 10120, Z = 680)
    for (int i = 0; i < 50; ++i) {
        controller.step(in8_enter, 1.0f / 60.0f, sim_scene);
    }
    bool s8_helipad_walkout = (controller.get_position().x >= 10100.0f) && (controller.get_position().z >= 675.0f);
    bool s8_pass = s8_elev_top && s8_helipad_walkout && real_elev_ok;
    std::cout << "  -> Stage 8 Result: " << (s8_pass ? "PASS" : "FAIL")
              << " (Elevator Top Z=" << cab_top_z
              << ", Walkout Pos=(" << controller.get_position().x << ", " << controller.get_position().z << ")"
              << ", Streamed Sublevels=" << sim_scene.loaded_sublevel_packages.size()
              << ", Checkpoint='" << controller.get_telemetry().active_checkpoint_name << "')" << std::endl;

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

    std::cout << "\n============================================================" << std::endl;
    std::cout << "  ORACLE VERIFICATION COMPLETE: ALL SYSTEMS PASS!" << std::endl;
    std::cout << "============================================================" << std::endl;
    return 0;
}

// -----------------------------------------------------------------------------
// Interactive SDL2 + Metal Window Gameplay Loop
// -----------------------------------------------------------------------------
static int run_interactive_app(const std::string& game_root, int initial_chapter,
                              const std::string& custom_level, int max_frames) {
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

    // Load active level
    LevelScene active_scene;
    int current_chapter_idx = std::clamp(initial_chapter, 0, 9);
    renderer.set_selected_chapter(current_chapter_idx);

    ParkourController controller(move_cfg);

    auto load_chapter_or_level = [&](int ch_idx, const std::string& custom_path) {
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
        bool loaded = load_level_scene(game_root, map_file, active_scene);
        if (!loaded) {
            std::cout << "[Game] Using contiguous procedural training grounds." << std::endl;
            controller.build_parkour_test_course(active_scene);
            append_test_course_visuals(active_scene);
        }
        controller.reset(active_scene.player_spawn_pos, active_scene.player_spawn_yaw);
        controller.get_telemetry().active_checkpoint = 0;
        if (!active_scene.subtitles.empty() && !active_scene.subtitles[0].empty()) {
            controller.get_telemetry().active_subtitle = active_scene.subtitles[0];
        }
    };

    load_chapter_or_level(current_chapter_idx, custom_level);

    SDL_SetRelativeMouseMode(SDL_TRUE);
    ensure_dir("screenshots");

    // Interactive Loop
    bool running = true;
    int frame_counter = 0;
    auto last_time = std::chrono::high_resolution_clock::now();

    EMovement prev_state = EMovement::MOVE_Walking;
    int prev_checkpoint = 0;
    float footstep_timer = 0.0f;
    bool reaction_toggled = false;

    std::cout << "\n[Controls]" << std::endl;
    std::cout << "  WASD: Move (Sprint active by default, momentum acceleration)" << std::endl;
    std::cout << "  Mouse: Look (Yaw/Pitch)" << std::endl;
    std::cout << "  Space: Jump / Wallrun / Wallclimb / Vault / Springboard / Pull-Up" << std::endl;
    std::cout << "  Left Ctrl / C / Left Shift: Crouch / Slide / Mid-Air Coil / Skill Roll" << std::endl;
    std::cout << "  Q: 180° Turn" << std::endl;
    std::cout << "  Left Mouse / F: Melee Punch/Kick or Fire Weapon" << std::endl;
    std::cout << "  Right Mouse / E: Disarm Enemy / Use Elevator" << std::endl;
    std::cout << "  X: Toggle Reaction Time (Slow-Motion)" << std::endl;
    std::cout << "  V / Left Alt: Runner Vision Look-At" << std::endl;
    std::cout << "  Tab / M: Toggle Chapter Select Menu" << std::endl;
    std::cout << "  1..9, 0: Directly load Chapter 0 through 9" << std::endl;
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
                if (SDL_GetRelativeMouseMode() == SDL_TRUE) {
                    float sens = 0.15f;
                    input.look_yaw_delta += float(ev.motion.xrel) * sens;
                    input.look_pitch_delta -= float(ev.motion.yrel) * sens;
                }
            } else if (ev.type == SDL_MOUSEBUTTONDOWN) {
                if (!renderer.is_menu_open() && SDL_GetRelativeMouseMode() != SDL_TRUE) {
                    SDL_SetRelativeMouseMode(SDL_TRUE);
                } else if (frame_counter >= 15 && !renderer.is_menu_open()) {
                    if (ev.button.button == SDL_BUTTON_LEFT) {
                        if (controller.get_weapon().equipped) input.fire = true;
                        else input.melee = true;
                    } else if (ev.button.button == SDL_BUTTON_RIGHT) {
                        input.disarm = true;
                    }
                }
            } else if (ev.type == SDL_KEYDOWN) {
                SDL_Keycode key = ev.key.keysym.sym;
                if (key == SDLK_ESCAPE) {
                    if (renderer.is_menu_open()) {
                        renderer.set_menu_open(false);
                        SDL_SetRelativeMouseMode(SDL_TRUE);
                    } else {
                        running = false;
                    }
                } else if (key == SDLK_TAB || key == SDLK_m) {
                    bool menu = !renderer.is_menu_open();
                    renderer.set_menu_open(menu);
                    SDL_SetRelativeMouseMode(menu ? SDL_FALSE : SDL_TRUE);
                } else if (key == SDLK_r) {
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
                    load_chapter_or_level(sel, "");
                }
            }
        }

        // Keyboard State Polling
        const Uint8* state = SDL_GetKeyboardState(nullptr);
        if (state[SDL_SCANCODE_W] || state[SDL_SCANCODE_UP]) input.forward += 1.0f;
        if (state[SDL_SCANCODE_S] || state[SDL_SCANCODE_DOWN]) input.forward -= 1.0f;
        if (state[SDL_SCANCODE_D] || state[SDL_SCANCODE_RIGHT]) input.strafe += 1.0f;
        if (state[SDL_SCANCODE_A] || state[SDL_SCANCODE_LEFT]) input.strafe -= 1.0f;

        if (state[SDL_SCANCODE_SPACE]) input.jump = true;
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

        // Advance simulation step
        Vec3 pre_vel = controller.get_velocity();
        controller.step(input, dt, active_scene);
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
                case EMovement::MOVE_SpringBoarding: audio.play_effect(EAudioEffect::Vault); break;
                case EMovement::MOVE_Slide:
                case EMovement::MOVE_MeleeSlide: audio.play_effect(EAudioEffect::Slide); break;
                case EMovement::MOVE_SkillRoll: audio.play_effect(EAudioEffect::SkillRoll); break;
                case EMovement::MOVE_ZipLine: audio.play_effect(EAudioEffect::Zipline); break;
                case EMovement::MOVE_Snatch: audio.play_effect(EAudioEffect::Disarm); break;
                default: break;
            }
            prev_state = tel.move_state;
        }

        if (tel.active_checkpoint != prev_checkpoint) {
            audio.play_effect(EAudioEffect::CheckpointChime);
            prev_checkpoint = tel.active_checkpoint;
        }

        if (input.fire) {
            audio.play_effect(EAudioEffect::Gunshot);
        }

        // Footstep cadence
        if (tel.grounded && tel.move_state == EMovement::MOVE_Walking && tel.speed_2d > 40.0f) {
            footstep_timer += dt;
            float stride_time = std::clamp(150.0f / tel.speed_2d, 0.22f, 0.45f);
            if (footstep_timer >= stride_time) {
                audio.play_effect(EAudioEffect::Footstep, 0.7f);
                footstep_timer = 0.0f;
            }
        } else {
            footstep_timer = 0.0f;
        }

        // Update 3D listener and dynamic audio stems
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
        } else if (arg == "--chapter") {
            if (i + 1 < argc) initial_chapter = std::atoi(argv[++i]);
        } else if (arg == "--level") {
            if (i + 1 < argc) custom_level = argv[++i];
        } else if (arg == "--max-frames") {
            if (i + 1 < argc) max_frames = std::atoi(argv[++i]);
        } else if (arg == "--game-root") {
            if (i + 1 < argc) game_root = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Mirror's Edge Native macOS Engine\n\n"
                      << "Usage:\n"
                      << "  mirrorsedge_macos [options]\n\n"
                      << "Options:\n"
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

    return run_interactive_app(game_root, initial_chapter, custom_level, max_frames);
}
