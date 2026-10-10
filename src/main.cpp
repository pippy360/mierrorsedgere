#define SDL_MAIN_HANDLED
#include <cstdlib>
#include <SDL2/SDL.h>
#if defined(__APPLE__)
#include <SDL2/SDL_metal.h>
#define ME_PLATFORM_TITLE "macOS"
#elif defined(_WIN32)
#define ME_PLATFORM_TITLE "Windows"
#else
#define ME_PLATFORM_TITLE "Linux"
#endif
#if defined(_WIN32) && defined(ME_RENDERER_OPENGL)
// The OpenGL backend built on Windows to check it (docs/LINUX_PORT.md): on a laptop with two GPUs
// an OpenGL context goes to the integrated one unless the executable asks otherwise this way.
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

#include "math/types.hpp"
#include "assets/upk_loader.hpp"
#include "assets/ini_config.hpp"
#include "audio/audio_engine.hpp"
#include "cutscene/cutscene_player.hpp"
#include "cutscene/screen_fade.hpp"
#include "game/screen_effects.hpp"
#include "game/impact_effects.hpp"
#include "game/script_effects.hpp"
#include "game/level_script.hpp"
#include "physics/collision_world.hpp"
#include "physics/parkour_controller.hpp"
#include "platform/platform.hpp"
#include "renderer/renderer.hpp"
#include "renderer/render_common.hpp"
#include "renderer/builtin_shaders_msl.hpp"
#include "renderer/sun_shadow.hpp"
#include "assets/material_system.hpp"
#include "ui/frontend/frontend.hpp"
#include "ui/frontend/soft_render.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <iostream>
#include <memory>
#include <fstream>
#include <string>
#include <vector>
#include <filesystem>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <cmath>
#include <cstdio>
#include <set>
#include <unordered_map>

namespace {
// Lower-case copy, for map and checkpoint names (compared case-insensitively, as UE3 names are).
std::string lower(std::string v) {
    for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return v;
}
}  // namespace

#if defined(_WIN32)
// The window's HWND for the Direct3D swap chain. Last, because <windows.h> defines macros
// (near, far, min, max) that are ordinary names in the headers above.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <SDL2/SDL_syswm.h>
#undef near
#undef far
#endif

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

// "MACOS" / "WINDOWS", for the banners
static std::string platform_upper() {
    std::string name = platform_name();
    for (char& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return name;
}

} // namespace me

// -----------------------------------------------------------------------------
// Deterministic Headless Oracle & Multi-Stage Parkour Verification Suite
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// --handover-check <map>: the level's intro played headless to its end and handed over to the
// controller as the game loop does it, with the camera (Renderer::player_camera, what is drawn from)
// written out around the hand-over: where it is in the pawn's frame, and how far it moves a frame.
// The view should not jump when control comes back (docs/LEVEL_INTROS.md).
// -----------------------------------------------------------------------------
struct HandoverResult {
    bool valid = false;
    float jump = 0.0f;           // the camera's move in the hand-over's frame (uu)
    float settle = 0.0f;         // its largest move in a frame in the second after it
    float drift = 0.0f;          // how far it ends from where the intro left it
    float turn_deg = 0.0f;       // the view's turn in the hand-over's frame
    float first_up = 0.0f;       // the eye in the first frame of play, over the intro's last eye
    float first_side = 0.0f;     // and how far from it level with the floor
    float rest_up = 0.0f;        // the eye a tenth of a second on, when the lift has been let down
    int glide_frames = 0;        // frames from the hand-over until the eye moves less than 0.2 uu
    me::Vec3 intro_eye{0.0f, 0.0f, 0.0f};  // the intro's last eye, ahead / right / above the feet it ends on
    me::Vec3 play_eye{0.0f, 0.0f, 0.0f};   // the standing eye a second later, the same way
};

static HandoverResult measure_intro_handover(me::Renderer& renderer, const std::string& game_root, const std::string& map_rel, bool print) {
    using namespace me;
    HandoverResult result;
    LevelScene scene;
    if (!load_level_scene(game_root, map_rel, scene) || !scene.level_intro.valid) return result;
    stream_level_to_checkpoint(game_root, scene, 0);
    MovementConfig move_cfg;
    load_movement_config_from_ini(get_config_path(game_root, "DefaultPawnMovement.ini"), move_cfg);
    ParkourController controller(move_cfg);
    controller.reset(scene.player_spawn_pos, scene.player_spawn_yaw);
    const LevelIntroSequence& seq = scene.level_intro;
    const Rotator end_frame = Rotator::from_degrees(0.0f, seq.end_yaw_deg, 0.0f);
    const auto in_pawn_frame = [&](const Vec3& p) {
        const Vec3 d = p - seq.end_feet_pos;
        return Vec3(d.dot(end_frame.forward()), d.dot(end_frame.right()), d.z);
    };
    struct Sample {
        float t;
        Vec3 cam;
        Rotator rot;
        bool intro;
        EMovement move;
        float feet_z;
    };
    std::vector<Sample> samples;
    const float dt = 1.0f / 60.0f;
    float now = 0.0f;
    const auto record = [&](bool intro) {
        Vec3 cam;
        Rotator rot;
        renderer.player_camera(controller.get_telemetry(), cam, rot);
        samples.push_back(Sample{now, cam, rot, intro, controller.get_telemetry().move_state, controller.get_telemetry().position.z});
    };
    CutscenePlayer cutscene;
    cutscene.play_in_engine_intro(scene, controller.get_telemetry(), 4.5f);
    for (int guard = 0; guard < 60 * 240 && cutscene.is_playing(); ++guard) {
        cutscene.update(dt, scene, controller.get_telemetry());
        now += dt;
        if (cutscene.is_playing()) record(true);
    }
    if (samples.empty()) return result;
    const size_t last_intro = samples.size() - 1;
    // The Matinee ran out: the player stands where its animation left her (the game loop's hand_over).
    controller.hand_over(seq.end_feet_pos, seq.end_yaw_deg, scene);
    controller.get_telemetry().intro_active = false;
    controller.get_telemetry().intro_camera_only = false;
    record(false);
    InputFrame idle{};
    for (int i = 0; i < 300; ++i) {
        controller.step(idle, dt, scene);
        now += dt;
        record(false);
    }
    result.valid = true;
    result.jump = (samples[last_intro + 1].cam - samples[last_intro].cam).length();
    {
        const Vec3 a = samples[last_intro].rot.forward(), b = samples[last_intro + 1].rot.forward();
        result.turn_deg = std::acos(std::clamp(a.dot(b), -1.0f, 1.0f)) * 57.29578f;
    }
    for (size_t i = last_intro + 2; i < samples.size() && i <= last_intro + 61; ++i) {
        result.settle = std::max(result.settle, (samples[i].cam - samples[i - 1].cam).length());
    }
    result.drift = (samples.back().cam - samples[last_intro].cam).length();
    {
        const Vec3 first = samples[last_intro + 1].cam - samples[last_intro].cam;
        result.first_up = first.z;
        result.first_side = Vec3(first.x, first.y, 0.0f).length();
        size_t i = last_intro + 2;
        while (i + 1 < samples.size() && (samples[i].cam - samples[i - 1].cam).length() > 0.2f) ++i;
        result.glide_frames = static_cast<int>(i - (last_intro + 2));
        result.rest_up = samples[i].cam.z - samples[last_intro].cam.z;
    }
    result.intro_eye = in_pawn_frame(samples[last_intro].cam);
    result.play_eye = in_pawn_frame(samples.back().cam);
    if (print) {
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "[Handover] " << map_rel << ": the intro ends at " << samples[last_intro].t << " s on feet (" << seq.end_feet_pos.x << ", "
                  << seq.end_feet_pos.y << ", " << seq.end_feet_pos.z << "), yaw " << seq.end_yaw_deg << std::endl;
        const size_t from = last_intro >= 6 ? last_intro - 6 : 0;
        for (size_t i = from; i < samples.size(); ++i) {
            if (i > last_intro + 8 && (i - last_intro) % 20 != 0) continue;
            const Vec3 e = in_pawn_frame(samples[i].cam);
            const Vec3 r = samples[i].rot.to_degrees();
            std::cout << "[Handover]   " << (samples[i].intro ? "intro" : "play ") << " t " << samples[i].t << "  eye ahead " << e.x << " right "
                      << e.y << " up " << e.z << "  pitch " << r.x << " yaw " << r.y << " roll " << r.z << "  " << move_state_name(samples[i].move)
                      << " feet " << samples[i].feet_z - seq.end_feet_pos.z;
            if (i > from) std::cout << "  moved " << (samples[i].cam - samples[i - 1].cam).length();
            std::cout << std::endl;
        }
        std::cout << "[Handover] " << map_rel << ": the eye goes up " << result.first_up << " uu (and " << result.first_side
                  << " sideways) in the first frame of play, turns " << result.turn_deg << " deg, comes down in " << result.glide_frames
                  << " frames (largest step " << result.settle << " uu) and rests " << result.rest_up << " uu from the intro's last eye"
                  << std::defaultfloat << std::endl;
    }
    return result;
}

// -----------------------------------------------------------------------------
// The level's script acting on the scene (game/script_effects.hpp), checked on a real level: Flight's
// Kismet is run against a pane of glass (SeqEvent_TakeDamage -> SeqAct_ActorFactory,
// SeqAct_ToggleHidden, SeqAct_ChangeCollision, SeqAct_Destroy) with real bullets and with a barge, and
// against a trigger whose chain switches an emitter or a lens flare (SeqAct_Toggle). Oracle stage 21;
// --verify-script runs it alone, ME_SCRIPT_DEBUG=1 prints the script's log, and with a renderer and a
// directory (--verify-script with ME_SCRIPT_SHOTS=<dir>) the pane is pictured whole, cracked and gone.
// -----------------------------------------------------------------------------
static bool oracle_script_effects(const std::string& game_root, me::Renderer* renderer = nullptr, const std::string& shot_dir = std::string()) {
    using namespace me;
    LevelScene scene;
    const bool loaded = load_level_scene(game_root, "Maps/SP01/Escape_p.me1", scene) && scene.script && scene.script->valid() &&
                        scene.collision;
    int toggles = 0, spawns = 0, hides = 0, collisions = 0;
    std::vector<std::string> script_log;
    LevelScript script;
    ScriptPlayerState ps;
    float now = 0.0f;
    const auto tick = [&](int frames) {
        for (int i = 0; i < frames; ++i) {
            update_impact_effects(scene, 1.0f / 30.0f, ps.position);
            deliver_actor_damage(script, scene);
            script.update(1.0f / 30.0f, ps);
            now += 1.0f / 30.0f;
        }
    };
    // A picture of `look_at` from `from`, for looking at what the script did.
    const auto snap = [&](const Vec3& look_at, const Vec3& from, const char* name) {
        if (!renderer || shot_dir.empty()) return;
        PlayerTelemetry tel{};
        const Vec3 d = (look_at - from).normalized();
        tel.position = from - Vec3(0.0f, 0.0f, tel.eye_height);
        tel.yaw_deg = std::atan2(d.y, d.x) * 57.29578f;
        tel.pitch_deg = std::asin(std::clamp(d.z, -1.0f, 1.0f)) * 57.29578f;
        tel.sim_time = now;
        tel.exposure_reset = true;
        tel.screen_effects = screen_effects_from_environment();
        renderer->render_frame(scene, tel);
        renderer->save_screenshot_png(shot_dir + "/" + name + ".png");
    };
    const auto lower_of = [](std::string v) {
        std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return v;
    };
    // The thin side of an actor's bounds: the way through a pane.
    const auto pane_axis = [&](const LevelActor& a) {
        const Vec3 size = a.world_bounds.max_pt - a.world_bounds.min_pt;
        return (size.x <= size.y && size.x <= size.z) ? Vec3(1.0f, 0.0f, 0.0f) : (size.y <= size.z ? Vec3(0.0f, 1.0f, 0.0f) : Vec3(0.0f, 0.0f, 1.0f));
    };
    // What a bullet through the middle of an actor's bounds meets first, hidden things passed through
    // as a bullet passes them (-1 nothing, -2 the BSP).
    const auto first_on_line = [&](const LevelActor& a) {
        const Vec3 centre = (a.world_bounds.min_pt + a.world_bounds.max_pt) * 0.5f;
        const Vec3 n = pane_axis(a);
        Vec3 from = centre + n * 60.0f;
        const Vec3 to = centre - n * 60.0f;
        for (int tries = 0; tries < 6; ++tries) {
            const CollisionHit hit = scene.collision->line_check(from, to, COLL_BlockZeroExtent);
            if (!hit.hit) return -1;
            if (hit.actor < 0) return -2;
            if (!scene.actors[static_cast<size_t>(hit.actor)].is_hidden) return static_cast<int>(hit.actor);
            from = hit.location - n * 0.25f;
            if ((to - from).dot(n * -1.0f) <= 0.0f) break;
        }
        return -1;
    };
    bool shot_ok = false, barge_ok = false, toggle_ok = false, run_ok = false;
    std::string shot_note = "no pane found", barge_note = "no pane found", toggle_note = "no trigger found", run_note = "no pane found";
    std::string checkpoint_name;
    if (loaded) {
        // Play starts at the checkpoint that has the most panes of glass streamed in (the offices).
        size_t best = 0;
        int best_checkpoint = 0;
        for (size_t c = 0; c < scene.checkpoint_infos.size(); ++c) {
            size_t n = 0;
            for (const LevelActor& a : scene.actors) {
                if (!a.script_damage || !a.script_switched || a.is_hidden) continue;
                for (const std::string& level : scene.checkpoint_infos[c].streaming_levels) n += lower_of(level) == lower_of(a.source_package) ? 1 : 0;
            }
            if (n > best) {
                best = n;
                best_checkpoint = static_cast<int>(c);
            }
        }
        stream_level_to_checkpoint(game_root, scene, best_checkpoint);
        if (!scene.checkpoint_infos.empty()) checkpoint_name = scene.checkpoint_infos[static_cast<size_t>(best_checkpoint)].checkpoint_name;
        ScriptHost host;
        bind_scene_effects(host, scene);
        const auto count = [](auto inner, int* counter) {
            return [inner, counter](auto&&... args) {
                ++*counter;
                inner(args...);
            };
        };
        host.toggle_effect = count(host.toggle_effect, &toggles);
        host.spawn_effect = count(host.spawn_effect, &spawns);
        host.hide_actor = count(host.hide_actor, &hides);
        host.change_collision = count(host.change_collision, &collisions);
        host.line_clear = [&](const Vec3& from, const Vec3& to) { return !scene.collision->line_check(from, to, COLL_BlockZeroExtent).hit; };
        host.stream_levels = [&](const std::vector<std::string>& levels, bool load) {
            auto& streamed_in = scene.loaded_sublevel_packages;
            for (const std::string& l : levels) {
                auto it = std::find_if(streamed_in.begin(), streamed_in.end(), [&](const std::string& s) { return lower_of(s) == lower_of(l); });
                if (load && it == streamed_in.end()) streamed_in.push_back(l);
                if (!load && it != streamed_in.end()) streamed_in.erase(it);
            }
            script.set_loaded_packages(streamed_in);
        };
        const bool debug = std::getenv("ME_SCRIPT_DEBUG") != nullptr;
        host.log = [&script_log, debug](const std::string& line) {
            script_log.push_back(line);
            if (debug) std::cout << "    " << line << std::endl;
        };
        script.init(scene.script, host);
        script.set_loaded_packages(scene.loaded_sublevel_packages);
        script.begin_play(checkpoint_name);
        ps.position = scene.player_spawn_pos;
        ps.eye = ps.position + Vec3(0.0f, 0.0f, 64.0f);
        tick(3);

        // The panes: in a sublevel that is streamed in, shown, listening for damage, hidden by the
        // script, and the first thing a bullet at their middle meets.
        const auto streamed = [&](const std::string& package) {
            const std::string want = lower_of(package);
            for (const std::string& p : script.loaded_packages()) {
                if (lower_of(p) == want) return true;
            }
            return false;
        };
        std::vector<int> panes;
        size_t listening = 0;
        for (size_t i = 0; i < scene.actors.size(); ++i) {
            const LevelActor& a = scene.actors[i];
            if (!a.script_damage || !a.script_switched || a.is_hidden || a.barge_door >= 0 || !streamed(a.source_package)) continue;
            ++listening;
            if (first_on_line(a) == static_cast<int>(i)) panes.push_back(static_cast<int>(i));
        }
        if (debug) std::cout << "    " << listening << " shown panes with damage events in the streamed sublevels, " << panes.size() << " that a bullet reaches" << std::endl;
        const auto shown_switched = [&]() {
            int n = 0;
            for (const LevelActor& a : scene.actors) n += (a.script_switched && !a.is_hidden) ? 1 : 0;
            return n;
        };

        // A: two bullets, on a pane of every kind of glass there is here (up to six). The first cracks
        // it (the pane is hidden, its twin shown and solid, the cracking emitter made); the second
        // shatters the twin, and neither is on the line any more.
        {
            std::vector<std::string> kinds;
            int passed = 0;
            std::string failed;
            for (size_t k = 0; k < panes.size() && kinds.size() < 6; ++k) {
                LevelActor& pane = scene.actors[static_cast<size_t>(panes[k])];
                // Glass only: a door with a damage event (it is barged) is no pane.
                if (lower_of(pane.mesh_name).find("glass") == std::string::npos) continue;
                if (pane.is_hidden || std::find(kinds.begin(), kinds.end(), pane.mesh_name) != kinds.end()) continue;
                kinds.push_back(pane.mesh_name);
                const Vec3 centre = (pane.world_bounds.min_pt + pane.world_bounds.max_pt) * 0.5f;
                const Vec3 n = pane_axis(pane);
                const int spawns_before = spawns;
                const size_t effects_before = scene.spawned_effects.size();
                const auto fire = [&]() {
                    // The weapon's own trace, to where the bullet stops.
                    const CollisionHit hit = scene.collision->line_check(centre + n * 60.0f, centre - n * 60.0f, COLL_BlockZeroExtent);
                    if (!hit.hit) return false;
                    BulletTracer tr;
                    tr.start_pos = centre + n * 60.0f;
                    tr.end_pos = hit.location;
                    tr.damage = 45.0f;
                    scene.active_tracers.push_back(tr);
                    tick(4);
                    scene.active_tracers.clear();
                    return true;
                };
                // Seen from a little to the side of the way through it.
                const Vec3 side = std::abs(n.z) > 0.5f ? Vec3(1.0f, 0.0f, 0.0f) : Vec3(0.0f, 0.0f, 1.0f);
                const Vec3 view_from = centre + n * 150.0f + side * 110.0f;
                const bool pictured = kinds.size() == 1;
                if (pictured) snap(centre, view_from, "glass_0_whole");
                fire();
                if (pictured) snap(centre, view_from, "glass_1_cracked");
                const bool cracked = pane.is_hidden;
                const int twin = first_on_line(pane);
                const bool twin_shown = twin >= 0 && twin != panes[k] && scene.actors[static_cast<size_t>(twin)].script_switched;
                const int first_spawns = spawns - spawns_before;
                bool shattered = false;
                if (cracked && twin_shown) {
                    fire();
                    if (pictured) snap(centre, view_from, "glass_2_shattered");
                    tick(12);
                    if (pictured) snap(centre, view_from, "glass_3_after");
                    const int after = first_on_line(pane);
                    shattered = scene.actors[static_cast<size_t>(twin)].is_hidden && after != twin && after != panes[k];
                }
                const bool ok = cracked && twin_shown && shattered && first_spawns >= 1 && spawns - spawns_before >= 2 &&
                                scene.spawned_effects.size() >= effects_before + 2;
                passed += ok ? 1 : 0;
                if (!ok) {
                    failed += " " + pane.unique_name + " (" + pane.mesh_name + "): cracked " + (cracked ? "yes" : "no") + ", twin there " +
                              (twin_shown ? "yes" : "no") + ", shattered " + (shattered ? "yes" : "no") + ", emitters " +
                              std::to_string(spawns - spawns_before) + ";";
                }
                if (debug) {
                    std::cout << "    " << pane.unique_name << " (" << pane.mesh_name << ") at (" << centre.x << ", " << centre.y << ", " << centre.z
                              << "): " << (ok ? "cracked, then shattered" : "FAILED") << std::endl;
                }
            }
            shot_note = std::to_string(passed) + " of " + std::to_string(kinds.size()) + " kinds of pane cracked by one bullet and shattered by the next" +
                        (failed.empty() ? std::string() : ";" + failed);
            shot_ok = kinds.size() >= 2 && passed == static_cast<int>(kinds.size());
        }

        // B: a barge (100 of TdDmgType_Barge on an interactable pane) goes straight through: the pane is
        // hidden and no script-driven mesh is left on the line.
        int tried = 0;
        for (size_t k = 0; k < panes.size() && !barge_ok && tried < 12; ++k) {
            LevelActor& pane = scene.actors[static_cast<size_t>(panes[k])];
            if (pane.is_hidden || !pane.interactable) continue;
            ++tried;
            const int shown_before = shown_switched();
            scene.actor_damage.push_back(ActorDamage{panes[k], 100.0f, true, 1});
            tick(6);
            const int after = first_on_line(pane);
            const bool through = pane.is_hidden && (after < 0 || !scene.actors[static_cast<size_t>(after)].script_switched);
            barge_note = pane.unique_name + " (" + pane.mesh_name + "): pane hidden " + (pane.is_hidden ? "yes" : "no") + ", way clear " +
                         (through ? "yes" : "no") + ", shown script meshes " + std::to_string(shown_before) + " -> " + std::to_string(shown_switched());
            barge_ok = through && shown_switched() < shown_before;
        }

        // D (before C moves the player about): the same barge thrown by the player. She runs at a standing
        // pane, the melee key down as she nears it: TdMove_Barge starts on the pane, its hit breaks it,
        // and she comes out on the other side.
        {
            MovementConfig move_cfg;
            load_movement_config_from_ini(get_config_path(game_root, "DefaultPawnMovement.ini"), move_cfg);
            ParkourController runner(move_cfg);
            tried = 0;
            for (size_t k = 0; k < panes.size() && !run_ok && tried < 10; ++k) {
                LevelActor& pane = scene.actors[static_cast<size_t>(panes[k])];
                const Vec3 n = pane_axis(pane);
                if (pane.is_hidden || !pane.interactable || std::abs(n.z) > 0.5f) continue;
                const Vec3 centre = (pane.world_bounds.min_pt + pane.world_bounds.max_pt) * 0.5f;
                for (float side : {1.0f, -1.0f}) {
                    if (pane.is_hidden) break;
                    // A floor to run on, 260 uu out, and nothing between her and the pane.
                    const Vec3 out = centre + n * (side * 260.0f);
                    const CollisionHit floor = scene.collision->line_check(out, out - Vec3(0.0f, 0.0f, 500.0f), COLL_BlockNonZeroExtent);
                    if (!floor.hit || floor.normal.z < 0.9f) continue;
                    const Vec3 feet = floor.location + Vec3(0.0f, 0.0f, 2.0f);
                    const Vec3 chest = feet + Vec3(0.0f, 0.0f, 90.0f);
                    const CollisionHit ahead = scene.collision->line_check(chest, chest - n * (side * 400.0f), COLL_BlockZeroExtent);
                    if (!ahead.hit || ahead.actor != panes[k]) continue;
                    ++tried;
                    runner.reset(feet, std::atan2(-n.y * side, -n.x * side) * 57.29578f);
                    bool saw_barge = false;
                    float far_side = 1.0e9f;
                    for (int step = 0; step < 240; ++step) {
                        InputFrame in{};
                        in.forward = 1.0f;
                        in.sprint = true;
                        const float to_pane = (runner.get_position() - centre).dot(n * side);
                        in.melee = to_pane <= 185.0f && !saw_barge && !pane.is_hidden;
                        runner.step(in, 1.0f / 60.0f, scene);
                        ps.position = runner.get_position();
                        if (step % 2 == 1) tick(1);
                        saw_barge = saw_barge || runner.get_telemetry().move_state == EMovement::MOVE_Barge;
                        far_side = std::min(far_side, (runner.get_position() - centre).dot(n * side));
                    }
                    run_note = pane.unique_name + " (" + pane.mesh_name + "): MOVE_Barge " + (saw_barge ? "yes" : "no") + ", pane broken " +
                               (pane.is_hidden ? "yes" : "no") + ", she got " + std::to_string(static_cast<int>(-far_side)) + " uu past it";
                    run_ok = saw_barge && pane.is_hidden && far_side < -40.0f;
                    if (debug) {
                        std::cout << "    the run started at (" << feet.x << ", " << feet.y << ", " << feet.z << ") facing "
                                  << std::atan2(-n.y * side, -n.x * side) * 57.29578f << " degrees" << std::endl;
                    }
                    if (run_ok) break;
                }
            }
        }

        // C: a trigger in a streamed sublevel whose chain reaches a SeqAct_Toggle on an emitter or a lens
        // flare: the player is put in it and the script run until the toggle arrives.
        const ScriptGraph& graph = *scene.script;
        const auto reaches_effect_toggle = [&](int event) {
            std::vector<int> front{event}, seen;
            for (int depth = 0; depth < 8 && !front.empty(); ++depth) {
                std::vector<int> next;
                for (int node : front) {
                    if (std::find(seen.begin(), seen.end(), node) != seen.end()) continue;
                    seen.push_back(node);
                    const ScriptGraph::Node& n = graph.nodes[static_cast<size_t>(node)];
                    if (n.cls == "SeqAct_Toggle") {
                        for (const ScriptGraph::VarLink& link : n.vars) {
                            for (int v : link.vars) {
                                if (v < 0 || graph.nodes[static_cast<size_t>(v)].actor < 0) continue;
                                const std::string& cls = graph.actors[static_cast<size_t>(graph.nodes[static_cast<size_t>(v)].actor)].cls;
                                if (cls.find("Emitter") != std::string::npos || cls == "LensFlareSource") return true;
                            }
                        }
                    }
                    for (const ScriptGraph::Output& o : n.outputs) {
                        for (const ScriptGraph::Link& l : o.links) next.push_back(l.op);
                    }
                }
                front.swap(next);
            }
            return false;
        };
        // What the scene's emitters and lens flares are switched to, as one number.
        const auto effects_state = [&]() {
            size_t h = 0;
            for (const ParticleSystemPlacement& p : scene.particle_systems) h = h * 31u + (p.active ? 1u : 0u) + p.activations * 2u;
            for (const LensFlareSourceInfo& f : scene.lens_flares) h = h * 31u + (f.active ? 1u : 0u) + (f.hidden ? 2u : 0u);
            return h;
        };
        // An event that the test can cause: a trigger the player can stand in, or a remote event.
        tried = 0;
        int candidates = 0;
        for (size_t i = 0; i < graph.nodes.size() && !toggle_ok && tried < 40; ++i) {
            const ScriptGraph::Node& n = graph.nodes[i];
            const bool touch = (n.cls == "SeqEvent_Touch" || n.cls == "SeqEvent_TdTouch") && n.originator >= 0 && n.player_touches;
            const bool remote = n.cls == "SeqEvent_RemoteEvent" && !n.label.empty();
            if ((!touch && !remote) || !n.enabled || !reaches_effect_toggle(static_cast<int>(i))) continue;
            ++candidates;
            ++tried;
            // Its sublevel is streamed in, as the level's own script would have by the time the player is there.
            const std::string& package = graph.packages[static_cast<size_t>(n.package)];
            if (!streamed(package)) host.stream_levels({package}, true);
            const int before = toggles;
            const size_t state_before = effects_state();
            std::string what;
            if (touch) {
                const ScriptActor& trigger = graph.actors[static_cast<size_t>(n.originator)];
                ps.position = (trigger.bounds.min_pt + trigger.bounds.max_pt) * 0.5f;
                ps.eye = ps.position + Vec3(0.0f, 0.0f, 64.0f);
                what = "standing in " + trigger.name;
            } else {
                script.fire_remote_event(n.label);
                what = "remote event '" + n.label + "'";
            }
            for (int f = 0; f < 300 && toggles == before; ++f) tick(1);
            toggle_note = what + " (" + package + ", " + n.name + "): " + std::to_string(toggles - before) + " effect(s) switched (try " +
                          std::to_string(tried) + ")";
            toggle_ok = toggles > before && effects_state() != state_before;
        }
        if (debug) std::cout << "    " << candidates << " events tried that lead to a toggle of an emitter or a lens flare" << std::endl;
    }
    const bool pass = loaded && shot_ok && barge_ok && toggle_ok && run_ok;
    std::cout << "  -> Stage 21 Result: " << (pass ? "PASS" : "FAIL") << " (Level=" << (loaded ? "OK" : "FAIL") << " at '" << checkpoint_name << "', Shots="
              << (shot_ok ? "OK" : "FAIL") << " [" << shot_note << "], Barge=" << (barge_ok ? "OK" : "FAIL") << " [" << barge_note
              << "], RunAndBarge=" << (run_ok ? "OK" : "FAIL") << " [" << run_note << "], Toggle=" << (toggle_ok ? "OK" : "FAIL") << " ["
              << toggle_note << "], calls: " << hides << " hide/show, "
              << collisions << " collision, " << spawns << " emitter, " << toggles << " toggle)" << std::endl;
    if (!pass) {
        const size_t from = script_log.size() > 16 ? script_log.size() - 16 : 0;
        for (size_t i = from; i < script_log.size(); ++i) std::cout << "     " << script_log[i] << std::endl;
    }
    return pass;
}

static int run_oracle_verification(const std::string& game_root, const std::string& script_json) {
    using namespace me;
    std::cout << "\n============================================================" << std::endl;
    std::cout << "  MIRROR'S EDGE NATIVE " << platform_upper() << " ENGINE - ORACLE VERIFICATION" << std::endl;
    std::cout << "============================================================" << std::endl;
    std::cout << "[Oracle] Game Root: " << game_root << std::endl;

    ensure_dir("screenshots");
    const std::string tmp_dir = temp_dir();
    ensure_dir(tmp_dir + "/me_oracle_screenshots");
    // The macOS development machine keeps a third copy of the screenshots here.
    std::string brain_dir;
#if defined(__APPLE__)
    brain_dir = "/Users/tomnom/.gemini/jetski/brain/722392a1-bc28-473f-af94-1a09e569935f";
    ensure_dir(brain_dir);
#endif

    // 1. Initialize Headless Renderer & Audio Engine
    Renderer renderer;
    renderer.set_game_root(game_root);
    if (!renderer.init_headless(1280, 720)) {
        std::cerr << "[Oracle ERROR] Renderer::init_headless failed!" << std::endl;
        return 1;
    }
    std::cout << "[Oracle] Renderer initialized in headless mode (1280x720)." << std::endl;

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
        std::string tmp_path = tmp_dir + "/me_oracle_screenshots/" + name;
        renderer.save_screenshot_png(local_path);
        copy_artifact(local_path, tmp_path);
        if (!brain_dir.empty()) copy_artifact(local_path, brain_dir + "/" + name);
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
                   std::abs(s1_z - (sp00_scene.player_spawn_pos.z + kPawnFloorHover)) < 1.0f;  // on the roof, at her hover over it
    std::cout << "  -> Stage 1 Result: " << (s1_pass ? "PASS" : "FAIL")
              << " (Speed=" << s1_speed << " u/s, FOV=" << s1_fov << "°, Roof Z=" << s1_z << ")" << std::endl;

    // Stage 2: Speed Vault over the stage-7 airduct (S_AirductSystem_02a, 145 units high, east face
    // x ~ -2621) & Springboard off the stage-17 stacked boxes (S_ConstructionPackages_01b)
    std::cout << "[Oracle Stage 2] Testing Speed Vault & Springboard..." << std::endl;
    // A. Speed Vault: sprint west and jump within reach of the duct face
    controller.reset(Vec3(-1655.2f, -6505.2f, 4224.0f), 180.0f);
    step_until(in_run, 240, sim_scene, [&] { return controller.get_position().x <= -2513.0f; });
    controller.step(in_run_jump, kDt, sim_scene);
    bool s2_vault = (controller.get_move_state() == EMovement::MOVE_VaultOver);
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
    bool s3_cleared_gap = controller.is_grounded() && s3_land.x < -1150.0f && std::abs(s3_land.z - (4224.0f + kPawnFloorHover)) < 2.0f;
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

    // The roll itself (TdMove_SkillRoll, retail recording 2026-09-20 14:56): fallinglandroll's root
    // motion carries the pawn 313.5 uu along its facing, the move lasts the whole 1.267 s animation
    // (OnCustomAnimEnd -> MOVE_Walking), and the camera rides the animation's EyeJoint / CameraJoint
    // (TdPlayerPawn.CalcCamera): one full forward somersault, down to the floor and back up level.
    auto wrap_180 = [](float a) {
        while (a > 180.0f) a -= 360.0f;
        while (a <= -180.0f) a += 360.0f;
        return a;
    };
    const Vec3 roll_start = controller.get_position();
    const Vec3 roll_fwd = Rotator::from_degrees(0.0f, controller.get_body_yaw(), 0.0f).forward();
    const float roll_t0 = controller.get_telemetry().combat_anim_time;  // already rolled in the landing frame
    int s6_roll_frames = 0;
    float cam_pitch = 0.0f, cam_pitch_raw_prev = 0.0f, cam_pitch_min = 0.0f, eye_min = 1e9f;
    auto sample_camera = [&](bool first) {
        Vec3 cam_pos;
        Rotator cam_rot;
        renderer.player_camera(controller.get_telemetry(), cam_pos, cam_rot);
        // How far the view has gone round, head over heels: the angle of its forward in the plane
        // of the roll (a rotator's own pitch comes back past straight down).
        const Vec3 look = cam_rot.forward();
        const float raw = std::atan2(look.z, look.dot(roll_fwd)) * RAD2DEG;
        cam_pitch = first ? raw : cam_pitch + wrap_180(raw - cam_pitch_raw_prev);
        cam_pitch_raw_prev = raw;
        cam_pitch_min = first ? cam_pitch : std::min(cam_pitch_min, cam_pitch);
        eye_min = std::min(eye_min, cam_pos.z - controller.get_position().z);
    };
    sample_camera(true);
    while (s6_roll && controller.get_move_state() == EMovement::MOVE_SkillRoll && s6_roll_frames < 150) {
        controller.step(in_run, kDt, sim_scene);
        ++s6_roll_frames;
        sample_camera(false);
        // Mid-roll frames (0.2, 0.42, 0.55, 0.8 s in): looking at the floor, upside down, at the sky
        // behind, and coming back up.
        const int frame_at = static_cast<int>(s6_roll_frames + roll_t0 / kDt + 0.5f);
        if (frame_at == 12 || frame_at == 25 || frame_at == 33 || frame_at == 48) {
            renderer.render_frame(sim_scene, controller.get_telemetry());
            char name[64];
            std::snprintf(name, sizeof(name), "oracle_6_skill_roll_%03dms.png",
                          static_cast<int>(controller.get_telemetry().combat_anim_time * 1000.0f + 0.5f));
            if (frame_at == 25) {
                save_and_publish_png("oracle_6_skill_roll_upside_down.png");
            } else {
                const std::string tmp_path = std::string("/tmp/me_oracle_screenshots/") + name;
                renderer.save_screenshot_png(tmp_path);
                copy_artifact(tmp_path, brain_dir + "/" + name);
            }
        }
    }
    log_telemetry("Stage6_SkillRoll_End");
    const float roll_time = roll_t0 + static_cast<float>(s6_roll_frames) * kDt;
    const Vec3 roll_move = controller.get_position() - roll_start;
    const float roll_dist = roll_move.dot(roll_fwd);
    const float roll_side = std::abs(roll_move.dot(Vec3(-roll_fwd.y, roll_fwd.x, 0.0f)));
    const bool s6_roll_time = std::abs(roll_time - 1.2667f) <= 2.0f * kDt &&
                              controller.get_move_state() == EMovement::MOVE_Walking;
    // Up to a 1/120 s substep of it rolled before roll_start was taken (4 uu) and of walking after it
    // ended (2 uu at its 233 uu/s exit speed).
    const bool s6_roll_dist = roll_dist > 313.5f - 6.0f && roll_dist < 313.5f + 3.0f && roll_side < 1.0f;
    // A full forward turn (the animation overshoots to -373) that ends level, and the eyes near the floor.
    const bool s6_somersault = cam_pitch_min <= -355.0f && std::abs(cam_pitch + 360.0f) < 1.0f && eye_min < 40.0f;
    bool s6_pass = s6_coil && s6_roll && s6_roll_time && s6_roll_dist && s6_somersault;
    std::cout << "  -> Stage 6 Result: " << (s6_pass ? "PASS" : "FAIL")
              << " (Coil=" << (s6_coil ? "OK" : "NO") << ", Roll=" << (s6_roll ? "OK" : "NO")
              << ", Drop=" << s6_drop << " u"
              << ", RollTime=" << (s6_roll_time ? "OK" : "FAIL") << " [" << roll_time << " s -> "
              << move_state_name(controller.get_move_state()) << "]"
              << ", RollDistance=" << (s6_roll_dist ? "OK" : "FAIL") << " [" << roll_dist << " u, " << roll_side
              << " u sideways]"
              << ", CameraSomersault=" << (s6_somersault ? "OK" : "FAIL") << " [pitch min " << cam_pitch_min
              << "°, end " << cam_pitch << "°, eyes down to " << eye_min << " u])" << std::endl;

    // Stage 7: Combat Disarm (Celeste on the stage-19 combat terrace) & Reaction Time
    std::cout << "[Oracle Stage 7] Testing Combat Disarm & Reaction Time..." << std::endl;
    controller.reset(Vec3(751.8f, -1591.7f, 4992.0f), 90.0f);
    InputFrame in7_walk{};
    in7_walk.forward = 1.0f;
    for (int i = 0; i < 20; ++i) {
        controller.step(in7_walk, kDt, sim_scene);
    }
    // A frame before the disarm, as the game has every step: the first-person tree is running
    // when the move starts, and takes the animation the move names.
    renderer.render_frame(sim_scene, controller.get_telemetry());
    InputFrame in7{};
    in7.reaction_time = true;
    in7.disarm = true;

    controller.step(in7, kDt, sim_scene);
    log_telemetry("Stage7_Disarm");

    bool s7_disarm = (controller.get_move_state() == EMovement::MOVE_Snatch && controller.get_telemetry().weapon.equipped);
    bool s7_reaction = controller.get_telemetry().reaction_active;

    // The picture is taken part way through the disarm's animation, not on its first frame.
    for (int f = 0; f < 45 && controller.get_move_state() == EMovement::MOVE_Snatch; ++f) {
        controller.step(InputFrame{}, kDt, sim_scene);
        renderer.render_frame(sim_scene, controller.get_telemetry());
    }
    {
        const Vec3 at = controller.get_position();
        std::cout << "  [Stage 7] disarm picture: player (" << at.x << ", " << at.y << ", " << at.z << ") yaw " << controller.get_telemetry().yaw_deg
                  << " t " << controller.get_telemetry().combat_anim_time << " / " << controller.get_telemetry().combat_anim_duration;
        for (const auto& bot : sim_scene.enemies) std::cout << " | bot " << bot.archetype << " at " << at.distance(bot.position) << (bot.disarm_weapon.empty() ? "" : " (being disarmed)");
        std::cout << std::endl;
    }
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
            s8_elev_top = (std::abs(cab_top_z - (lift_top_z + kPawnFloorHover)) < 2.0f) &&
                          (esc_scene.elevators[lift].state == ElevatorState::IdleEnd) &&
                          esc_scene.elevators[lift].streaming_triggered;

            // 8B. Turn around and walk out of the open upper doors onto the upper floor
            controller.set_rotation(180.0f, 0.0f, 0.0f);
            for (int i = 0; i < 60; ++i) {
                controller.step(in8_walk, kDt, esc_scene);
            }
            walkout_pos = controller.get_position();
            s8_walkout = controller.is_grounded() && walkout_pos.x < lift_floor.x - lift_half.x - 30.0f &&
                         std::abs(walkout_pos.z - (lift_top_z + kPawnFloorHover)) < 2.0f;
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
    bool no_auto_barge = false;
    bool saw_move_barge = false;
    float final_door_deg = 0.0f;
    float final_door_x = 0.0f;
    bool barge_anims_ok = false;   // BargeInLeft, then BargeOutLeft from the impact
    bool barge_speed_ok = false;   // StartBargin: Velocity = Normal(Velocity) * min(500, speed + 200)
    bool barge_swing_ok = false;   // the open matinee slams the leaf to 103.755 deg, away from the player
    bool barge_sounds_ok = false;
    bool kick_ok = false;
    bool kick_sounds_ok = false;
    bool door_cycle_ok = false;
    bool slide_kick_ok = false;
    bool slide_kick_sounds_ok = false;
    float slide_kick_end_x = 0.0f;
    float slide_kick_door_deg = 0.0f;
    bool barge_camera_ok = false;  // the first-person tree's eye turns with BargeInLeft / BargeOutLeft
    bool kick_camera_ok = false;   // and lunges with MeleeKickObject
    bool cues_ok = true;           // every sound the simulation starts resolves to a loaded cue
    float barge_entry_speed = 0.0f;
    float barge_expect_speed = 0.0f;
    float kick_open_t = -1.0f;
    float kick_end_t = -1.0f;
    float kick_max_speed = 0.0f;
    float close_start_t = -1.0f;
    float close_overshoot_deg = 0.0f;
    float barge_in_yaw_max = 0.0f;     // the view's turn from the controller's (degrees, + right)
    float barge_out_yaw_max = 0.0f;
    float barge_cam_pitch_min = 0.0f;
    float barge_cam_yaw_end = 0.0f;    // after the barge
    float kick_eye_rest = 0.0f;        // the eye ahead of the pawn (uu), standing
    float kick_eye_lunge = 0.0f;       // and how much further the kick carries it
    std::string missing_cues;
    if (!sp00_scene.barge_doors.empty()) {
        BargeDoorInstance& door = sp00_scene.barge_doors[0];
        auto reset_door = [&door]() {
            door.state = DoorState::Closed;
            door.open_angle_rad = 0.0f;
            door.anim_time = 0.0f;
            door.hold_timer = 0.0f;
            door.barged = false;
            door.model_matrix = Mat4::identity();
        };
        auto collect_sounds = [&](std::set<std::string>& heard) {
            for (const SimSoundEvent& ev : controller.get_telemetry().sound_events) {
                if (heard.insert(ev.cue).second && !audio.has_sound(ev.cue)) {
                    if (missing_cues.find(ev.cue) == std::string::npos) missing_cues += ev.cue + " ";
                    cues_ok = false;
                }
            }
        };
        auto heard_all = [](const std::set<std::string>& heard, std::initializer_list<const char*> cues) {
            for (const char* c : cues) {
                if (!heard.count(c)) return false;
            }
            return true;
        };
        auto print_heard = [](const char* label, const std::set<std::string>& heard) {
            std::cout << "  [" << label << " sounds]";
            for (const std::string& c : heard) std::cout << " " << c;
            std::cout << std::endl;
        };
        std::cout << "  [Door] hinge=(" << door.hinge_pos.x << ", " << door.hinge_pos.y << ", " << door.hinge_pos.z
                  << ") centre=(" << door.center_pos.x << ", " << door.center_pos.y << ", " << door.center_pos.z
                  << ") bounds=(" << door.closed_bounds.min_pt.x << ", " << door.closed_bounds.min_pt.y << ", "
                  << door.closed_bounds.min_pt.z << ")-(" << door.closed_bounds.max_pt.x << ", "
                  << door.closed_bounds.max_pt.y << ", " << door.closed_bounds.max_pt.z << ")" << std::endl;

        // 12A: Verify running at a closed door WITHOUT melee does NOT automatically barge it open
        reset_door();
        controller.reset(Vec3(-3960.0f, -6360.0f, 4224.0f), 180.0f);
        InputFrame run_only{};
        run_only.forward = 1.0f;
        run_only.sprint = true;
        for (int step = 0; step < 50; ++step) {
            controller.step(run_only, kDt, sp00_scene);
        }
        no_auto_barge = (door.state == DoorState::Closed && controller.get_telemetry().move_state != EMovement::MOVE_Barge);

        // 12B: Melee while running at the door is the shoulder barge: BargeInLeft runs the pawn at
        // BargeSpeed, the impact opens the door and BargeOutLeft plays, then on through the doorway.
        reset_door();
        controller.reset(Vec3(-3960.0f, -6360.0f, 4224.0f), 180.0f);
        std::set<std::string> barge_heard;
        bool saw_in = false;
        bool saw_out = false;
        // The camera is the first-person tree's EyeJoint (TdPlayerPawn.CalcCamera), and AT_C1P's
        // UpperBodySplit gives the UpperBody slot the EyeJoint, so the barge's animations turn the view:
        // right into the left shoulder and down through BargeInLeft, further right from the impact
        // (BargeOutLeft) and back to straight ahead. Sampled every step, the tree sees every move
        // change and every animation the move names, as a rendered frame would.
        auto camera_offsets = [&](float& yaw_off, float& pitch_off, Vec3& eye) {
            const PlayerTelemetry& ct = controller.get_telemetry();
            Vec3 cam_pos;
            Rotator cam_rot;
            renderer.player_camera(ct, cam_pos, cam_rot);
            const Vec3 r = cam_rot.to_degrees();
            yaw_off = wrap_180(r.y - ct.yaw_deg);
            pitch_off = wrap_180(r.x - ct.pitch_deg);
            const Rotator body = Rotator::from_degrees(0.0f, ct.body_yaw_deg, 0.0f);
            const Vec3 d = cam_pos - ct.position;
            eye = Vec3(d.dot(body.forward()), d.dot(body.right()), d.z);
        };
        for (int step = 0; step < 120; ++step) {
            InputFrame barge_in{};
            barge_in.forward = 1.0f;
            barge_in.sprint = true;
            // Press melee as we reach the doorway
            const float dx = std::abs(controller.get_position().x - door.center_pos.x);
            if (dx <= 185.0f && door.state == DoorState::Closed && !saw_move_barge) {
                barge_in.melee = true;
            }
            const float speed_before = controller.get_telemetry().speed_2d;
            controller.step(barge_in, kDt, sp00_scene);
            collect_sounds(barge_heard);
            float yaw_off = 0.0f, pitch_off = 0.0f;
            Vec3 eye;
            camera_offsets(yaw_off, pitch_off, eye);
            const PlayerTelemetry& bt = controller.get_telemetry();
            if (bt.move_state == EMovement::MOVE_Barge) {
                if (!saw_move_barge) {
                    barge_entry_speed = bt.speed_2d;
                    barge_expect_speed = std::min(500.0f, speed_before + 200.0f);
                }
                saw_move_barge = true;
                if (bt.barge_anim == 1 && !saw_out) {
                    saw_in = true;
                    barge_in_yaw_max = std::max(barge_in_yaw_max, yaw_off);
                }
                if (bt.barge_anim == 2 && saw_in) {
                    saw_out = true;
                    barge_out_yaw_max = std::max(barge_out_yaw_max, yaw_off);
                }
                barge_cam_pitch_min = std::min(barge_cam_pitch_min, pitch_off);
            }
            barge_cam_yaw_end = yaw_off;
        }
        final_door_deg = door.open_angle_rad * (180.0f / 3.14159265f);
        final_door_x = controller.get_telemetry().position.x;
        barge_anims_ok = saw_in && saw_out;
        barge_speed_ok = std::abs(barge_entry_speed - barge_expect_speed) < 5.0f;
        // BargeInLeft has turned the view right by the impact (15.9 deg at 0.25 s of it); BargeOutLeft
        // takes it on to 55 at 0.22 s and down past 30, then back to straight ahead before it ends.
        barge_camera_ok = barge_in_yaw_max > 10.0f && barge_out_yaw_max > 45.0f && barge_out_yaw_max < 65.0f &&
                          barge_cam_pitch_min < -15.0f && std::abs(barge_cam_yaw_end) < 2.0f;
        {
            // The leaf's middle ends up past the doorway on the far side (the run went towards -X).
            const float c = std::cos(door.open_angle_rad);
            const float s = std::sin(door.open_angle_rad);
            const Vec3 arm = door.center_pos - door.hinge_pos;
            const float swung_x = c * arm.x - s * arm.y;
            const float arm_len = std::sqrt(arm.x * arm.x + arm.y * arm.y);
            barge_swing_ok = std::abs(std::abs(final_door_deg) - 103.755f) < 1.0f && swung_x < -0.8f * arm_len;
        }
        barge_sounds_ok = heard_all(barge_heard, {"Doors.Door_Barge", "Doors.Door_Hit",
                                                  "Wood._11_Female_FootStepAttack", "Oral_Impact.Medium"});
        print_heard("Barge", barge_heard);

        // 12C: Melee standing at the door is the kick (MeleeKickObject): the pawn stays put and the
        // animation's BargeHitNotify (0.3272 s) opens the door; the move ends with the animation.
        reset_door();
        controller.reset(Vec3(door.center_pos.x + 70.0f, door.center_pos.y, 4224.0f), 180.0f);
        float kick_yaw_off = 0.0f, kick_pitch_off = 0.0f;
        Vec3 kick_eye;
        for (int step = 0; step < 10; ++step) {
            controller.step(InputFrame{}, kDt, sp00_scene);
            camera_offsets(kick_yaw_off, kick_pitch_off, kick_eye);
        }
        kick_eye_rest = kick_eye.x;
        std::set<std::string> kick_heard;
        bool saw_kick = false;
        float t = 0.0f;
        for (int step = 0; step < 60; ++step) {
            InputFrame kick_in{};
            kick_in.melee = (step == 0);
            controller.step(kick_in, kDt, sp00_scene);
            t += kDt;
            collect_sounds(kick_heard);
            camera_offsets(kick_yaw_off, kick_pitch_off, kick_eye);
            const PlayerTelemetry& kt = controller.get_telemetry();
            if (kt.move_state == EMovement::MOVE_Barge && kt.barge_anim == 3) {
                saw_kick = true;
                // MeleeKickObject (FullBody) carries the eye forward with the kick, 8.1 -> 27.9 uu ahead
                // of the pawn at 0.36 s.
                kick_eye_lunge = std::max(kick_eye_lunge, kick_eye.x - kick_eye_rest);
            }
            if (kick_open_t < 0.0f && door.state != DoorState::Closed) kick_open_t = t;
            if (saw_kick && kick_end_t < 0.0f && kt.move_state == EMovement::MOVE_Walking) kick_end_t = t;
            kick_max_speed = std::max(kick_max_speed, kt.speed_2d);
        }
        kick_ok = saw_kick && kick_open_t > 0.30f && kick_open_t < 0.37f &&
                  kick_end_t > 0.84f && kick_end_t < 0.90f && kick_max_speed < 5.0f;
        kick_camera_ok = kick_eye_lunge > 15.0f && kick_eye_lunge < 25.0f;
        kick_sounds_ok = heard_all(kick_heard, {"Oral_Strain.Hard", "Foot_Swoosh", "Doors.Door_Barge",
                                                "Wood._11_Female_FootStepAttack"});
        print_heard("Kick", kick_heard);

        // 12D: The door's Kismet starts the close matinee 3 s after the open one (0.6 s) completes;
        // its sound track plays hatch.Squek, then Door_Hit as the leaf swings past shut.
        std::set<std::string> close_heard;
        for (int step = 0; step < 300 && door.state != DoorState::Closed; ++step) {
            controller.step(InputFrame{}, kDt, sp00_scene);
            t += kDt;
            collect_sounds(close_heard);
            if (door.state == DoorState::Closing) {
                if (close_start_t < 0.0f) close_start_t = t;
                close_overshoot_deg = std::max(close_overshoot_deg,
                                               -door.swing_sign * door.open_angle_rad * (180.0f / 3.14159265f));
            }
        }
        const float close_delay = close_start_t - kick_open_t;
        door_cycle_ok = door.state == DoorState::Closed && door.open_angle_rad == 0.0f &&
                        heard_all(close_heard, {"hatch.Squek", "Doors.Door_Hit"}) &&
                        close_delay > 3.5f && close_delay < 3.7f;
        print_heard("Close", close_heard);

        // 12E: Slide-kicking a closed barge door (MOVE_Slide -> MOVE_MeleeSlide via TdMove_MeleeSlide):
        // sprinting into a slide and pressing melee barges the door open and carries Faith through.
        reset_door();
        controller.reset(Vec3(-3960.0f, -6360.0f, 4224.0f), 180.0f);
        std::set<std::string> slide_kick_heard;
        bool saw_melee_slide = false;
        for (int step = 0; step < 120; ++step) {
            InputFrame sk_in{};
            sk_in.forward = 1.0f;
            sk_in.sprint = true;
            const float dx = std::abs(controller.get_position().x - door.center_pos.x);
            if (dx <= 160.0f) {
                sk_in.crouch = true;
            }
            if (dx <= 140.0f && door.state == DoorState::Closed && !saw_melee_slide) {
                sk_in.melee = true;
            }
            controller.step(sk_in, kDt, sp00_scene);
            collect_sounds(slide_kick_heard);
            if (controller.get_telemetry().move_state == EMovement::MOVE_MeleeSlide) {
                saw_melee_slide = true;
            }
        }
        slide_kick_door_deg = door.open_angle_rad * (180.0f / 3.14159265f);
        slide_kick_end_x = controller.get_telemetry().position.x;
        slide_kick_ok = saw_melee_slide && std::abs(std::abs(slide_kick_door_deg) - 103.755f) < 1.0f &&
                        (slide_kick_end_x < -4300.0f);
        slide_kick_sounds_ok = heard_all(slide_kick_heard,
                                         {"Cloth.Run", "Oral_Strain.Medium", "Foot_Swoosh",
                                          "Wood._11_Female_FootStepAttack", "Doors.Door_Barge", "Doors.Door_Hit"});
        print_heard("SlideKick", slide_kick_heard);

        s12_pass = no_auto_barge && saw_move_barge && barge_anims_ok && barge_speed_ok && barge_swing_ok &&
                   (final_door_x < -4300.0f) && barge_sounds_ok && kick_ok && kick_sounds_ok && door_cycle_ok &&
                   slide_kick_ok && slide_kick_sounds_ok && cues_ok;
    }
    // Both door cues are SoundNodeMixers of two layers; Door_Hit's waves are imports from A_CXP_Plaza.
    const size_t barge_layers = audio.count_sound_layers("Doors.Door_Barge");
    const size_t hit_layers = audio.count_sound_layers("Doors.Door_Hit");
    s12_pass = s12_pass && barge_layers == 2 && hit_layers == 2 && barge_camera_ok && kick_camera_ok;
    std::cout << "  -> Stage 12 Result: " << (s12_pass ? "PASS" : "FAIL")
              << " (Doors=" << sp00_scene.barge_doors.size()
              << ", NoAutoBarge=" << (no_auto_barge ? "OK" : "FAIL")
              << ", MoveBarge=" << (saw_move_barge ? "OK" : "NO")
              << ", BargeInOut=" << (barge_anims_ok ? "OK" : "NO")
              << ", BargeSpeed=" << barge_entry_speed << "/" << barge_expect_speed
              << ", Swing=" << final_door_deg << " deg" << (barge_swing_ok ? "" : " (BAD)")
              << ", EndX=" << final_door_x
              << ", BargeSounds=" << (barge_sounds_ok ? "OK" : "MISSING")
              << ", Kick=" << (kick_ok ? "OK" : "FAIL") << " (open " << kick_open_t << " s, end " << kick_end_t
              << " s, max speed " << kick_max_speed << ")"
              << ", KickSounds=" << (kick_sounds_ok ? "OK" : "MISSING")
              << ", DoorClose=" << (door_cycle_ok ? "OK" : "FAIL") << " (at " << close_start_t
              << " s, overshoot " << close_overshoot_deg << " deg)"
              << ", SlideKick=" << (slide_kick_ok ? "OK" : "FAIL") << " (swing " << slide_kick_door_deg
              << " deg, endX " << slide_kick_end_x << ")"
              << ", SlideKickSounds=" << (slide_kick_sounds_ok ? "OK" : "MISSING")
              << ", Cues=" << (cues_ok ? std::string("OK") : ("UNRESOLVED " + missing_cues))
              << ", DoorCueLayers=" << barge_layers << "/" << hit_layers
              << ", BargeCamera=" << (barge_camera_ok ? "OK" : "FAIL") << " [in yaw " << barge_in_yaw_max
              << ", out yaw " << barge_out_yaw_max << ", pitch min " << barge_cam_pitch_min << ", end yaw "
              << barge_cam_yaw_end << "]"
              << ", KickEye=" << (kick_camera_ok ? "OK" : "FAIL") << " [" << kick_eye_rest << " uu ahead, lunge +"
              << kick_eye_lunge << "]" << ")" << std::endl;

    // Write complete telemetry log
    const std::string tel_path = tmp_dir + "/me_oracle_telemetry.json";
    std::ofstream tel_file(tel_path);
    if (tel_file.is_open()) {
        tel_file << "[\n";
        for (size_t i = 0; i < telemetry_log.size(); ++i) {
            tel_file << "  " << telemetry_log[i] << (i + 1 < telemetry_log.size() ? ",\n" : "\n");
        }
        tel_file << "]\n";
        tel_file.close();
        std::cout << "[Oracle] Telemetry written to " << tel_path << std::endl;
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

        // 13B. Climb Tutorial Stage 11 Pipe 1 (-7890, -3106, 4223..4936, bCanExitAtTop=False) to its top stop,
        //      verify it does NOT exit at the top or glitch through the roof fence (Y=-3104),
        //      jump East to Pipe 2 (-7663, -3106, 4596..4914, bCanExitAtTop=False), verify Pipe 2 also stops
        //      cleanly below its top cap without glitching through the fence, and jump East onto the Stage 12
        //      catwalk platform (X=-7472..-7280, Y=-3296..-3104, Z=4704).
        //      Retail stop (TdLadderVolume.GetLastStep = Num - 4 on a pipe, GetLadderLocation ZOffsetPipe -5):
        //      cylinder centre End.Z - 197, so feet End.Z - 287 (Pipe 1: 4649, Pipe 2: 4627).
        constexpr float kPipe1StopZ = 4936.0f - 287.0f;
        constexpr float kPipe2StopZ = 4914.0f - 287.0f;
        controller.reset(Vec3(-7890.0f, -3165.0f, 4225.0f), 90.0f);
        bool grabbed_pipe1 = false;
        float pipe1_max_z = -1e9f;
        for (int i = 0; i < 300; ++i) {
            controller.step(in_walk, kDt, sim_scene);
            if (controller.get_move_state() == EMovement::MOVE_Climb) {
                grabbed_pipe1 = true;
                pipe1_max_z = std::max(pipe1_max_z, controller.get_position().z);
            }
        }
        const Vec3 pipe1_top_pos = controller.get_position();
        const bool pipe1_capped_ok = grabbed_pipe1 &&
            (controller.get_move_state() == EMovement::MOVE_Climb) &&
            (std::abs(pipe1_top_pos.z - kPipe1StopZ) < 1.0f) && (pipe1_max_z <= kPipe1StopZ + 0.5f) &&
            (pipe1_top_pos.y < -3106.0f);

        // Aim toward Pipe 2 (+X / East along the North wall) and jump across
        controller.set_rotation(25.0f, 0.0f, 0.0f);
        InputFrame in_pipe_jump{};
        in_pipe_jump.jump = true;
        in_pipe_jump.forward = 1.0f;
        controller.step(in_pipe_jump, kDt, sim_scene);

        bool caught_pipe2 = false;
        float pipe2_catch_z = 0.0f;
        float pipe2_max_step_down = 0.0f;   // largest single-frame drop while on the pipe (no snap)
        float prev_z = controller.get_position().z;
        for (int i = 0; i < 180; ++i) {
            controller.step(in_walk, kDt, sim_scene);
            const float z = controller.get_position().z;
            if (controller.get_move_state() == EMovement::MOVE_Climb &&
                std::abs(controller.get_position().x - (-7663.0f)) < 60.0f) {
                if (!caught_pipe2) pipe2_catch_z = z;
                else pipe2_max_step_down = std::max(pipe2_max_step_down, prev_z - z);
                caught_pipe2 = true;
            }
            prev_z = z;
        }
        const Vec3 pipe2_top_pos = controller.get_position();
        const bool pipe2_capped_ok = caught_pipe2 &&
            (controller.get_move_state() == EMovement::MOVE_Climb) &&
            (std::abs(pipe2_top_pos.z - kPipe2StopZ) < 1.0f) &&
            (pipe2_catch_z <= 4914.0f - 191.0f + 0.5f) && (pipe2_max_step_down < 16.0f) &&
            (pipe2_top_pos.y < -3106.0f);

        // Jump East off Pipe 2 onto the Stage 12 catwalk platform (Z=4704)
        controller.set_rotation(15.0f, 0.0f, 0.0f);
        controller.step(in_pipe_jump, kDt, sim_scene);
        std::string catwalk_route;
        for (int i = 0; i < 240; ++i) {
            controller.step(in_walk, kDt, sim_scene);
            const std::string st = move_state_name(controller.get_move_state());
            if (catwalk_route.empty() || catwalk_route.rfind(st) != catwalk_route.size() - st.size()) {
                catwalk_route += (catwalk_route.empty() ? "" : ">") + st;
            }
            if (controller.is_grounded() && controller.get_position().x >= -7472.0f &&
                controller.get_position().z >= 4700.0f) {
                break;
            }
        }
        const Vec3 catwalk_pos = controller.get_position();
        const bool climb_ok = pipe1_capped_ok && pipe2_capped_ok &&
            controller.is_grounded() && (catwalk_pos.x >= -7472.0f) &&
            (catwalk_pos.z >= 4700.0f) && (catwalk_pos.y < -3104.0f);

        // 13C. Climb both Tutorial_p TdLadderVolume ladders to the top and verify clean dismount onto top platform:
        //      - Ladder 1 (Billboard catwalk ladder at (-4202, -1369, 3183..4177), WallNormal=(-1,0,0))
        //      - Ladder 2 (Rooftop ladder at (1393, -3907, 4191..4437), WallNormal=(-1,0,0))
        controller.reset(Vec3(-4266.0f, -1369.0f, 3183.0f), 0.0f);
        bool grabbed_ladder1 = false;
        bool exited_ladder1 = false;
        for (int i = 0; i < 420; ++i) {
            controller.step(in_walk, kDt, sim_scene);
            if (controller.get_move_state() == EMovement::MOVE_Climb) grabbed_ladder1 = true;
            if (grabbed_ladder1 && controller.get_move_state() != EMovement::MOVE_Climb &&
                controller.is_grounded() && controller.get_position().x > -4202.0f &&
                controller.get_position().z >= 4080.0f) {
                exited_ladder1 = true;
                break;
            }
        }
        controller.reset(Vec3(1329.0f, -3907.0f, 4192.0f), 0.0f);
        bool grabbed_ladder2 = false;
        bool exited_ladder2 = false;
        for (int i = 0; i < 180; ++i) {
            controller.step(in_walk, kDt, sim_scene);
            if (controller.get_move_state() == EMovement::MOVE_Climb) grabbed_ladder2 = true;
            if (grabbed_ladder2 && controller.get_move_state() != EMovement::MOVE_Climb &&
                controller.is_grounded() && controller.get_position().x > 1393.0f &&
                controller.get_position().z >= 4340.0f) {
                exited_ladder2 = true;
                break;
            }
        }
        const bool ladders_ok = grabbed_ladder1 && exited_ladder1 && grabbed_ladder2 && exited_ladder2;

        s13_pass = bal_ok && climb_ok && ladders_ok;
        std::cout << "  -> Stage 13 Result: " << (s13_pass ? "PASS" : "FAIL")
                  << " (Balance Entered=" << (entered_balance ? "YES" : "NO")
                  << ", Balance End X=" << bal_end_pos.x
                  << ", Pipe1 Capped=" << (pipe1_capped_ok ? "YES" : "NO")
                  << " [Z=" << pipe1_top_pos.z << " max " << pipe1_max_z << "]"
                  << ", Pipe2 Capped=" << (pipe2_capped_ok ? "YES" : "NO")
                  << " [caught Z=" << pipe2_catch_z << ", settled Z=" << pipe2_top_pos.z
                  << ", max drop/frame " << pipe2_max_step_down << "]"
                  << ", Catwalk Land=(" << catwalk_pos.x << "," << catwalk_pos.y << "," << catwalk_pos.z << ")"
                  << " via " << catwalk_route
                  << ", Ladder1 TopExit=" << (exited_ladder1 ? "YES" : "NO")
                  << ", Ladder2 TopExit=" << (exited_ladder2 ? "YES" : "NO") << ")" << std::endl;
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

    // Stage 15: Zipline Shift Drop, Horizontal Swing Bar & Narrow Ledge Walk
    bool s15_pass = false;
    {
        std::cout << "[Oracle Stage 15] Testing Zipline Shift Drop, Swing Bar & Ledge Walk..." << std::endl;
        // 15A. Zipline Shift Drop: attach to zipline in sim_scene, press Shift (crouch=true),
        // verify immediate drop to MOVE_Falling AND survival + MOVE_Landing (FallingLandSoftLanding)
        // when dropping onto the Tutorial soft-landing cushion (S_ConstructionTent_01 at (-4500, -3700, 4251), >1250 uu drop).
        bool zip_drop_ok = false;
        for (const auto& act : sim_scene.actors) {
            if (!act.is_zipline) continue;
            const Vec3 zs = act.location;
            const Vec3 ze = act.end_point;
            const Vec3 high_pt = (zs.z >= ze.z) ? zs : ze;
            const Vec3 low_pt  = (zs.z >= ze.z) ? ze : zs;
            const Vec3 seg = low_pt - high_pt;
            // Position along the zipline directly above the Tutorial soft-landing tent (~t=0.48)
            const Vec3 start_pos = high_pt + seg * 0.48f - Vec3(0.0f, 0.0f, 110.0f);
            controller.reset(start_pos, 20.0f);
            InputFrame in_idle{};
            bool attached = false;
            for (int i = 0; i < 15; ++i) {
                controller.step(in_idle, kDt, sim_scene);
                if (controller.get_move_state() == EMovement::MOVE_ZipLine) {
                    attached = true;
                    break;
                }
            }
            if (attached) {
                InputFrame in_drop{};
                in_drop.crouch = true; // Shift / Crouch to detach from zip line
                controller.step(in_drop, kDt, sim_scene);
                const bool detached = (controller.get_move_state() == EMovement::MOVE_Falling);
                bool any_death_state = false;
                bool landed_soft = false;
                for (int i = 0; i < 180; ++i) {
                    controller.step(in_idle, kDt, sim_scene);
                    if (controller.get_telemetry().falling_to_death ||
                        controller.get_telemetry().fall_death_impact) {
                        any_death_state = true;
                    }
                    if (controller.is_grounded()) {
                        landed_soft = (controller.get_move_state() == EMovement::MOVE_Landing ||
                                       controller.get_move_state() == EMovement::MOVE_SoftLanding ||
                                       controller.get_move_state() == EMovement::MOVE_Walking) &&
                                      controller.get_telemetry().health >= 99.0f;
                        break;
                    }
                }
                zip_drop_ok = detached && !any_death_state && landed_soft;
            }
            break;
        }

        // 15B. Horizontal Swing Bar: jump into TdSwingVolume in sim_scene and verify MOVE_Swing pendulum + jump release
        bool swing_ok = false;
        for (const auto& act : sim_scene.actors) {
            if (!act.is_swing_bar) continue;
            const Vec3 mid = (act.location + act.end_point) * 0.5f;
            const Vec3 bar_axis = (act.end_point - act.location).normalized();
            const Vec3 approach(-bar_axis.y, bar_axis.x, 0.0f);
            controller.reset(mid - approach * 65.0f - Vec3(0.0f, 0.0f, 155.0f),
                             std::atan2(approach.y, approach.x) * (180.0f / 3.14159265f));
            controller.set_velocity(approach * 320.0f);
            bool caught_bar = false;
            for (int i = 0; i < 24; ++i) {
                InputFrame in_grab{};
                in_grab.forward = 1.0f;
                in_grab.jump = (i == 0);
                controller.step(in_grab, kDt, sim_scene);
                if (controller.get_move_state() == EMovement::MOVE_Swing) {
                    caught_bar = true;
                    break;
                }
            }
            if (caught_bar) {
                InputFrame in_fwd{};
                in_fwd.forward = 1.0f;
                for (int i = 0; i < 12; ++i) controller.step(in_fwd, kDt, sim_scene);
                InputFrame in_jump{};
                in_jump.jump = true;
                controller.step(in_jump, kDt, sim_scene);
                swing_ok = ((controller.get_move_state() == EMovement::MOVE_Jump ||
                             controller.get_move_state() == EMovement::MOVE_Falling) &&
                            controller.get_telemetry().speed_2d > 400.0f);
            }
            break;
        }

        // 15C. Narrow Ledge Walk (TdLedgeWalkVolume): step onto ledge walk volume and shimmy across
        bool ledge_walk_ok = false;
        LevelActor synthetic_ledge{};
        synthetic_ledge.is_ledge = true;
        synthetic_ledge.location    = Vec3(-4000.0f, -6000.0f, 4224.0f);
        synthetic_ledge.end_point   = Vec3(-4300.0f, -6000.0f, 4224.0f);
        synthetic_ledge.wall_normal   = Vec3(0.0f, -1.0f, 0.0f);
        sim_scene.actors.push_back(synthetic_ledge);
        controller.reset(Vec3(-4020.0f, -6000.0f, 4224.0f), 90.0f);
        InputFrame in_shimmy{};
        in_shimmy.strafe = -1.0f;
        for (int i = 0; i < 30; ++i) {
            controller.step(in_shimmy, kDt, sim_scene);
            if (controller.get_move_state() == EMovement::MOVE_LedgeWalk) {
                ledge_walk_ok = true;
            }
        }
        sim_scene.actors.pop_back();

        s15_pass = zip_drop_ok && swing_ok && ledge_walk_ok;
        std::cout << "  -> Stage 15 Result: " << (s15_pass ? "PASS" : "FAIL")
                  << " (ZiplineDrop=" << (zip_drop_ok ? "OK" : "FAIL")
                  << ", SwingBar=" << (swing_ok ? "OK" : "FAIL")
                  << ", LedgeWalk=" << (ledge_walk_ok ? "OK" : "FAIL") << ")" << std::endl;
    }

    // Stage 16: Per-move camera rules (TdMove bConstrainLook / DisableLookTime / ResetCameraLook):
    // the slide keeps the view within 55 deg of the body, the wallrun keeps it between the run
    // direction and the wall normal, the skill roll ignores look input, and walking looks freely.
    std::cout << "[Oracle Stage 16] Testing Per-Move Camera Constraints..." << std::endl;
    auto wrap180 = [](float a) {
        a = std::fmod(a + 180.0f, 360.0f);
        if (a < 0.0f) a += 360.0f;
        return a - 180.0f;
    };
    // A. Free look while walking: 10 frames of +5 deg turn 50 deg, and the body follows.
    controller.reset(Vec3(208.0f, -7790.0f, 5760.0f), 0.0f);
    InputFrame in_look_walk{};
    in_look_walk.look_yaw_delta = 5.0f;
    for (int i = 0; i < 10; ++i) controller.step(in_look_walk, kDt, sim_scene);
    const bool free_look_ok = std::abs(wrap180(controller.get_yaw() - 50.0f)) < 0.01f &&
                              std::abs(wrap180(controller.get_body_yaw() - controller.get_yaw())) < 0.01f;

    // B. Slide (TdMove_Slide: yaw +-10000 uu = +-54.9 deg of the body): turning hard during the slide
    //    moves the view, but never more than 55 deg from the body.
    controller.reset(Vec3(208.0f, -7790.0f, 5760.0f), 0.0f);
    for (int i = 0; i < 100; ++i) controller.step(in_run, kDt, sim_scene);
    InputFrame in16_slide{};
    in16_slide.forward = 1.0f;
    in16_slide.crouch = true;
    controller.step(in16_slide, kDt, sim_scene);
    const bool s16_slide = (controller.get_move_state() == EMovement::MOVE_Slide);
    const float slide_yaw0 = controller.get_yaw();
    in16_slide.look_yaw_delta = 6.0f;
    float slide_max_rel = 0.0f;
    int slide_frames = 0;
    for (int i = 0; i < 25 && controller.get_move_state() == EMovement::MOVE_Slide; ++i) {
        controller.step(in16_slide, kDt, sim_scene);
        slide_max_rel = std::max(slide_max_rel, std::abs(wrap180(controller.get_yaw() - controller.get_body_yaw())));
        ++slide_frames;
    }
    const float slide_turned = std::abs(wrap180(controller.get_yaw() - slide_yaw0));
    const bool slide_cam_ok = s16_slide && slide_frames >= 10 && slide_max_rel <= 55.0f && slide_turned >= 20.0f;

    // C. Wallrun (TdMove_WallRun: absolute yaw window from the wall normal to the run direction):
    //    looking into the wall never takes the view further outside the window.
    controller.reset(Vec3(-100.0f, -6180.0f, 4224.0f), 160.0f);
    controller.set_velocity(Vec3(-517.0f, 188.0f, 0.0f));
    step_until(in_run, 60, sim_scene, [&] { return controller.get_position().x <= -275.0f; });
    controller.step(in_run_jump, kDt, sim_scene);
    const EMovement s16_wr = controller.get_move_state();
    const bool s16_wallrun = (s16_wr == EMovement::MOVE_WallRunningLeft || s16_wr == EMovement::MOVE_WallRunningRight);
    auto window_violation = [&]() {
        const Vec3 n = controller.get_telemetry().wall_normal;
        const float ny = std::atan2(n.y, n.x) * RAD2DEG;
        const bool right = (controller.get_move_state() == EMovement::MOVE_WallRunningRight);
        const float lo = right ? ny : ny - 90.0f;  // MinContraintWorld
        const float hi = right ? ny + 90.0f : ny;  // MaxContraintWorld
        const float v = controller.get_yaw();
        return std::max({0.0f, -wrap180(v - lo), wrap180(v - hi)});
    };
    InputFrame in16_wall = in_run;
    // Into the wall: a left wallrun has the wall on the left (negative yaw), a right one on the right.
    in16_wall.look_yaw_delta = (s16_wr == EMovement::MOVE_WallRunningLeft) ? -4.0f : 4.0f;
    float wr_prev = window_violation(), wr_worst_rise = 0.0f;
    int wr_frames = 0;
    for (int i = 0; i < 40; ++i) {
        controller.step(in16_wall, kDt, sim_scene);
        const EMovement st = controller.get_move_state();
        if (st != EMovement::MOVE_WallRunningLeft && st != EMovement::MOVE_WallRunningRight) break;
        const float v = window_violation();
        wr_worst_rise = std::max(wr_worst_rise, v - wr_prev);
        wr_prev = v;
        ++wr_frames;
    }
    const bool wallrun_cam_ok = s16_wallrun && wr_frames >= 10 && wr_worst_rise <= 0.05f && wr_prev <= 15.0f;

    // D. Skill roll (TdMove_SkillRoll: SetIgnoreLookInput(-1) until the roll ends): mouse look does
    //    not turn the view mid-roll.
    controller.reset(Vec3(-8900.0f, -5280.0f, 6144.0f), 90.0f);
    for (int i = 0; i < 35; ++i) controller.step(in_run, kDt, sim_scene);
    controller.step(in_run_jump, kDt, sim_scene);
    InputFrame in16_coil{};
    in16_coil.crouch = true;
    in16_coil.forward = 1.0f;
    controller.step(in16_coil, kDt, sim_scene);
    step_until(in_run, 120, sim_scene, [&] { return controller.get_position().z <= 5860.0f; });
    step_until(in16_coil, 60, sim_scene, [&] { return controller.is_grounded(); });
    const bool s16_roll = (controller.get_move_state() == EMovement::MOVE_SkillRoll);
    const float roll_yaw0 = controller.get_yaw();
    InputFrame in16_look{};
    in16_look.look_yaw_delta = 5.0f;
    in16_look.look_pitch_delta = 3.0f;
    int roll_frames = 0;
    for (int i = 0; i < 10 && controller.get_move_state() == EMovement::MOVE_SkillRoll; ++i) {
        controller.step(in16_look, kDt, sim_scene);
        ++roll_frames;
    }
    const float roll_turn = std::abs(wrap180(controller.get_yaw() - roll_yaw0));
    const bool roll_lock_ok = s16_roll && roll_frames >= 5 && roll_turn < 0.01f;

    const bool s16_pass = free_look_ok && slide_cam_ok && wallrun_cam_ok && roll_lock_ok;
    std::cout << "  -> Stage 16 Result: " << (s16_pass ? "PASS" : "FAIL")
              << " (FreeLook=" << (free_look_ok ? "OK" : "FAIL")
              << ", Slide=" << (slide_cam_ok ? "OK" : "FAIL") << " [max " << slide_max_rel << "° from body, turned "
              << slide_turned << "° in " << slide_frames << " frames]"
              << ", Wallrun=" << (wallrun_cam_ok ? "OK" : "FAIL") << " [" << move_state_name(s16_wr)
              << ", outside window " << wr_prev << "°, worst rise " << wr_worst_rise << "° in " << wr_frames << " frames]"
              << ", SkillRollLookLock=" << (roll_lock_ok ? "OK" : "FAIL") << " [turned " << roll_turn << "° in "
              << roll_frames << " frames])" << std::endl;

    // -------------------------------------------------------------------------
    // Stage 17: UTdHudEffectManager + PlayHitCameraShake Damage Screen Effects
    // -------------------------------------------------------------------------
    std::cout << "\n[Oracle Stage 17] Verifying Player Damage Screen Effects & Directional Hit Camera Shake..." << std::endl;
    LevelScene s17_scene = sim_scene;
    s17_scene.enemies.clear();
    s17_scene.helicopters.clear();
    s17_scene.dummy_fire_barrages.clear();
    controller.reset(Vec3(208.0f, -7790.0f, 5760.0f), 0.0f);
    controller.get_telemetry().intro_active = false;
    controller.get_telemetry().sound_events.clear();
    controller.apply_damage(10.0f, 0, Vec3(100.0f, 0.0f, 0.0f));  // Front bullet hit
    const bool hit_front_ok = controller.get_telemetry().camera_anim == "gethitfront" &&
                              controller.get_telemetry().bullet_hit_timers[0] > 0.20f &&
                              controller.get_telemetry().bullet_hit_angles[0] < 0.01f &&
                              controller.get_telemetry().health_desat > 0.30f &&
                              controller.get_telemetry().hit_blur > 0.30f &&
                              controller.get_telemetry().hit_focus_distance == -500.0f &&
                              !controller.get_telemetry().sound_events.empty();
    for (int i = 0; i < 15; ++i) controller.step(InputFrame{}, kDt, s17_scene);  // clear 0.2s SpazzThrottle
    controller.apply_damage(10.0f, 0, Vec3(0.0f, 100.0f, 0.0f));  // Right bullet hit
    const bool hit_right_ok = controller.get_telemetry().camera_anim == "gethitright" &&
                              std::abs(controller.get_telemetry().bullet_hit_angles[1] - 0.25f) < 0.01f;
    for (int i = 0; i < 15; ++i) controller.step(InputFrame{}, kDt, s17_scene);
    controller.apply_damage(10.0f, 0, Vec3(-100.0f, 0.0f, 0.0f));  // Back bullet hit
    const bool hit_back_ok = controller.get_telemetry().camera_anim == "gethitback" &&
                             std::abs(controller.get_telemetry().bullet_hit_angles[2] - 0.50f) < 0.01f;
    for (int i = 0; i < 15; ++i) controller.step(InputFrame{}, kDt, s17_scene);
    controller.apply_damage(22.0f, 1, Vec3(0.0f, -100.0f, 0.0f));  // Left melee strike
    const bool hit_left_melee_ok = controller.get_telemetry().camera_anim == "gethitleft" &&
                                   controller.get_telemetry().melee_damage_strength > 0.30f &&
                                   std::abs(controller.get_telemetry().melee_hit_dir - 0.75f) < 0.01f;
    controller.apply_damage(15.0f, 2, Vec3(0.0f, 0.0f, 0.0f));  // Fall damage
    ScreenEffects s17_fx;
    s17_fx.update(controller.get_telemetry(), kDt, controller.get_telemetry().screen_effects);
    const bool hit_fall_ok = controller.get_telemetry().fall_damage_strength > 0.30f &&
                             controller.get_telemetry().screen_effects.size() >= 3;
    const bool s17_pass = hit_front_ok && hit_right_ok && hit_back_ok && hit_left_melee_ok && hit_fall_ok;
    std::cout << "  -> Stage 17 Result: " << (s17_pass ? "PASS" : "FAIL")
              << " (FrontBullet=" << (hit_front_ok ? "OK" : "FAIL")
              << ", RightBullet=" << (hit_right_ok ? "OK" : "FAIL")
              << ", BackBullet=" << (hit_back_ok ? "OK" : "FAIL")
              << ", LeftMelee=" << (hit_left_melee_ok ? "OK" : "FAIL")
              << ", FallDamage=" << (hit_fall_ok ? "OK" : "FAIL") << ")" << std::endl;

    // -------------------------------------------------------------------------
    // Stage 18: Retail In-Game Pause Menu (TdSPPause, TdTutorialPause, TdPauseOptions)
    // -------------------------------------------------------------------------
    std::cout << "[Oracle Stage 18] Testing Retail In-Game Pause Menu (TdSPPause / TdTutorialPause / TdPauseOptions)..." << std::endl;
    bool s18_sp_ok = false, s18_opt_ok = false, s18_tut_ok = false, s18_overlay_ok = false;
    {
        fe::Frontend fe;
        std::string fe_err;
        if (fe.init(game_root, 1280, 720, fe_err, /*load_menu_level=*/false)) {
            fe::SoftRenderer sr(fe.assets());
            std::vector<uint8_t> rgba;
            auto step_fe = [&](int ticks) {
                for (int i = 0; i < ticks; ++i) fe.update(0.1f);
            };

            fe.open_pause_menu("Edge_p");
            step_fe(6);  // finish 0.3s StickOpenAnim and 0.5s saturation ramp
            s18_sp_ok = fe.is_pause_open() && fe.scene_name() == "TdSPPause" &&
                        std::abs(fe.pause_saturation() - 1.0f) < 0.01f;

            sr.render_overlay(fe.frame(), rgba);
            size_t transparent_px = 0, opaque_px = 0;
            for (size_t p = 3; p < rgba.size(); p += 4) {
                if (rgba[p] == 0) ++transparent_px;
                else if (rgba[p] >= 200) ++opaque_px;
            }
            // The pause overlay leaves most of the 1280x720 frame transparent for the 3D scene while drawing the red stick & buttons.
            s18_overlay_ok = (rgba.size() == 1280u * 720u * 4u) &&
                             (transparent_px > 1280u * 720u / 2u) &&
                             (opaque_px > 20000u);

            // Navigate Resume -> Achievements (skipped, disabled) -> Options and open TdPauseOptions -> TdVideoSettingsPC.
            fe.key_down(fe::Key::Down);
            fe.key_up(fe::Key::Down);
            fe.key_down(fe::Key::Accept);
            fe.key_up(fe::Key::Accept);
            step_fe(4);  // finish TdPauseOptions StickOpenAnim
            const bool in_pause_opt = (fe.scene_name() == "TdPauseOptions");
            fe.key_down(fe::Key::Accept);
            fe.key_up(fe::Key::Accept);
            const bool in_video_unsat = (fe.scene_name() == "TdVideoSettingsPC") && (fe.pause_saturation() == 0.0f);
            fe.key_down(fe::Key::Escape);
            fe.key_up(fe::Key::Escape);
            fe.key_down(fe::Key::Escape);
            fe.key_up(fe::Key::Escape);
            step_fe(4);  // finish TdPauseOptions StickCloseAnim -> back to TdSPPause
            const bool back_to_sp = (fe.scene_name() == "TdSPPause");
            fe.key_down(fe::Key::Escape);
            fe.key_up(fe::Key::Escape);
            step_fe(4);  // finish TdSPPause StickCloseAnim -> Resume
            s18_opt_ok = in_pause_opt && in_video_unsat && back_to_sp &&
                         !fe.is_pause_open() && (fe.take_action() == "Resume");

            fe.open_pause_menu("Tutorial_p");
            step_fe(4);
            s18_tut_ok = fe.is_pause_open() && fe.scene_name() == "TdTutorialPause";
        }
    }
    const bool s18_pass = s18_sp_ok && s18_opt_ok && s18_tut_ok && s18_overlay_ok;
    std::cout << "  -> Stage 18 Result: " << (s18_pass ? "PASS" : "FAIL")
              << " (TdSPPause=" << (s18_sp_ok ? "OK" : "FAIL")
              << ", OverlayAlpha=" << (s18_overlay_ok ? "OK" : "FAIL")
              << ", TdPauseOptions=" << (s18_opt_ok ? "OK" : "FAIL")
              << ", TdTutorialPause=" << (s18_tut_ok ? "OK" : "FAIL") << ")" << std::endl;

    // Stage 19: Electric Fence & Movement Exclusion Volume (TdDmgType_ElectricShock / TdBarbedWireVolume /
    // TdMovementExclusionVolume / SeqAct_ChangeCollision): an energized electric fence blocks vaulting and
    // ledge-grabbing and shocks Faith back into MOVE_Falling with DamageImpulse=300, DamageZDirection=0.2;
    // once de-energized (e.g. SeqAct_ChangeCollision), the same obstacle can be vaulted cleanly.
    std::cout << "[Oracle Stage 19] Testing Electric Fence Vault Prevention & Shock Knockback..." << std::endl;
    bool s19_pass = false;
    {
        // Place an electric fence volume over the Stage 2 airduct obstacle (east face x ~ -2621, z = 4224..4380)
        LevelActor elec_vol{};
        elec_vol.class_name = "PhysicsVolume";
        elec_vol.object_name = "PhysicsVolume_ElectricTest";
        elec_vol.unique_name = "PhysicsVolume_ElectricTest";
        elec_vol.is_electric_volume = true;
        elec_vol.exclude_hand_moves = true;
        elec_vol.exclude_foot_moves = true;
        elec_vol.damage_per_sec = 90.0f;
        elec_vol.world_bounds.min_pt = Vec3(-2700.0f, -6600.0f, 4220.0f);
        elec_vol.world_bounds.max_pt = Vec3(-2500.0f, -6400.0f, 4420.0f);
        elec_vol.location = (elec_vol.world_bounds.min_pt + elec_vol.world_bounds.max_pt) * 0.5f;
        sim_scene.actors.push_back(elec_vol);

        // 19A: Attempt the exact Stage 2 speed-vault approach while the electric fence volume is active.
        // Vaulting / grabbing must be rejected, and running into the volume must shock Faith back (+X velocity,
        // MOVE_Falling, health reduced).
        controller.reset(Vec3(-1655.2f, -6505.2f, 4224.0f), 180.0f);
        bool saw_illegal_vault = false;
        bool saw_shock_knockback = false;
        float min_health = 100.0f;
        for (int i = 0; i < 180; ++i) {
            const bool jump_frame = (std::abs(controller.get_position().x - (-2513.0f)) < 25.0f);
            controller.step(jump_frame ? in_run_jump : in_run, kDt, sim_scene);
            const EMovement st = controller.get_move_state();
            if (st == EMovement::MOVE_VaultOver || st == EMovement::MOVE_SpeedVaulting ||
                st == EMovement::MOVE_Grabbing || st == EMovement::MOVE_IntoGrab ||
                st == EMovement::MOVE_GrabPullUp || st == EMovement::MOVE_WallClimbing) {
                saw_illegal_vault = true;
            }
            min_health = std::min(min_health, controller.get_telemetry().health);
            if (controller.get_telemetry().health < 100.0f && controller.get_velocity().x > 100.0f &&
                st == EMovement::MOVE_Falling) {
                saw_shock_knockback = true;
                break;
            }
        }

        // 19B: De-energize the electric fence volume (as SeqAct_ChangeCollision does when flipping the switch)
        // and verify the obstacle can now be vaulted normally.
        sim_scene.actors.back().is_electric_volume = false;
        controller.reset(Vec3(-1655.2f, -6505.2f, 4224.0f), 180.0f);
        step_until(in_run, 240, sim_scene, [&] { return controller.get_position().x <= -2513.0f; });
        controller.step(in_run_jump, kDt, sim_scene);
        const bool deenergized_vault_ok = (controller.get_move_state() == EMovement::MOVE_VaultOver);
        sim_scene.actors.pop_back();

        // 19C: Verify real UPK extraction of electric-shock PhysicsVolumes (Subway_MoPu_Spt.me1),
        // TdBarbedWireVolumes (Tutorial_p.me1 / Edge_p.me1), and TdMovementExclusionVolumes
        // (Escape_Plaza_Spt.me1) with valid world-space BrushAggGeom bounds.
        int real_elec_vols = 0;
        int real_wire_vols = 0;
        int real_excl_vols = 0;
        auto count_vols = [&](const std::vector<LevelActor>& acts) {
            for (const auto& a : acts) {
                const bool valid_bounds = (a.world_bounds.max_pt.x > a.world_bounds.min_pt.x &&
                                           a.world_bounds.max_pt.z > a.world_bounds.min_pt.z);
                if (a.is_electric_volume && valid_bounds) ++real_elec_vols;
                if (a.is_barbed_wire_volume && valid_bounds) ++real_wire_vols;
                if (a.is_movement_exclusion_volume && valid_bounds) ++real_excl_vols;
            }
        };
        count_vols(sp00_scene.actors);
        count_vols(sp01_scene.actors);
        for (const char* rel : {"/TdGame/CookedPC/Maps/SP04/Subway_MoPu_Spt.me1",
                                "/TdGame/CookedPC/Maps/SP01/Escape_Plaza_Spt.me1"}) {
            UPKPackage pkg(game_root + rel);
            if (pkg.is_valid()) {
                count_vols(pkg.extract_actors());
            }
        }

        s19_pass = !saw_illegal_vault && saw_shock_knockback && (min_health < 100.0f) &&
                   deenergized_vault_ok && (real_elec_vols >= 6) && (real_wire_vols > 0) && (real_excl_vols > 0);
        std::cout << "  -> Stage 19 Result: " << (s19_pass ? "PASS" : "FAIL")
                  << " (BlockedVaultWhenEnergized=" << (!saw_illegal_vault ? "OK" : "FAIL")
                  << ", ShockKnockback=" << (saw_shock_knockback ? "OK" : "FAIL")
                  << ", HealthAfterShock=" << min_health
                  << ", VaultWhenDeenergized=" << (deenergized_vault_ok ? "OK" : "FAIL")
                  << ", RealVols[Electric=" << real_elec_vols
                  << ", BarbedWire=" << real_wire_vols
                  << ", Exclusion=" << real_excl_vols << "])" << std::endl;
    }

    // Stage 20: Ledge pull-up after a jump (TdMove_GrabPullUp). The heave's root motion carries the
    // pawn, and the first-person camera on the EyeJoint rides it, as in retail. At the Escape_p ledge
    // of the 2026-09-20 18:59 recording (hang capsule (17584, -6869, 12139.2), lip 12232): fall onto
    // it, hang, pull up holding W. Retail's numbers are the mean of its five recorded HangHeaveUp
    // pull-ups (spread under 2 uu), capsule rise and eye over the hanging capsule's centre against
    // the time in the heave; the recorded camera is 10 uu ahead along the view and a frame older than
    // the pawn (tools/retail/README.md), both taken out. Before root motion the eye was 22 uu low.
    std::cout << "[Oracle Stage 20] Testing Ledge Pull-Up Root Motion & Camera (TdMove_GrabPullUp)..." << std::endl;
    bool s20_pass = false;
    {
        LevelScene esc_scene;
        const bool esc_ok = load_level_scene(game_root, "Maps/SP01/Escape_p.me1", esc_scene);
        struct Ref { float t, cap, eye; };
        constexpr Ref kRetailHeave[] = {{0.2f, 34.2f, 114.5f}, {0.4f, 68.6f, 152.3f}, {0.6f, 103.8f, 175.4f},
                                        {0.8f, 142.0f, 189.5f}, {1.0f, 177.2f, 202.8f}, {1.2f, 188.4f, 227.6f}};
        constexpr float kHangHeaveUpLength = 1.533333f;
        struct Sample { float t, cap, eye; };
        std::vector<Sample> heave_samples;
        bool grabbed = false, pulled = false, walked = false, shot = false;
        std::string heave_anim;
        float worst_cap = 0.0f, worst_eye = 0.0f, max_cam_step = 0.0f, last_heave_t = 0.0f, end_rise = 0.0f;
        std::ostringstream eye_report;
        if (esc_ok) {
            controller.anchor(Vec3(17584.0f, -6880.0f, 12080.0f), Vec3(0.0f, 0.0f, -150.0f), 90.0f, 25.0f, false,
                              ParkourController::AnchorState{});
            Vec3 cam_pos;
            Rotator cam_rot;
            InputFrame in20_idle{};
            for (int i = 0; i < 120 && !grabbed; ++i) {
                controller.step(in20_idle, kDt, esc_scene);
                renderer.player_camera(controller.get_telemetry(), cam_pos, cam_rot);
                grabbed = controller.get_move_state() == EMovement::MOVE_Grabbing;
            }
            for (int i = 0; grabbed && i < 72; ++i) {  // hang 1.2 s
                controller.step(in20_idle, kDt, esc_scene);
                renderer.player_camera(controller.get_telemetry(), cam_pos, cam_rot);
            }
            const Vec3 hang = controller.get_position();
            const float hang_centre_z = hang.z + 90.0f;
            InputFrame in20_up{};
            in20_up.forward = 1.0f;
            Vec3 prev_cam(0.0f, 0.0f, 0.0f);
            int frames_after = 0;
            for (int i = 0; grabbed && i < 180 && frames_after < 24; ++i) {
                controller.step(in20_up, kDt, esc_scene);
                renderer.player_camera(controller.get_telemetry(), cam_pos, cam_rot);
                const EMovement st = controller.get_move_state();
                if (st == EMovement::MOVE_GrabPullUp) {
                    if (!pulled) heave_anim = controller.get_telemetry().move_anim;
                    pulled = true;
                    last_heave_t = controller.get_telemetry().combat_anim_time;
                    heave_samples.push_back({last_heave_t, controller.get_position().z + 90.0f - hang_centre_z, cam_pos.z - hang_centre_z});
                    if (!shot && last_heave_t >= 0.5f) {
                        // Half a second in: up past the lip, looking down onto the roof.
                        shot = true;
                        renderer.render_frame(esc_scene, controller.get_telemetry());
                        save_and_publish_png("oracle_20_ledge_pullup.png");
                    }
                } else if (pulled) {
                    walked = walked || st == EMovement::MOVE_Walking;
                    ++frames_after;
                }
                if (pulled && i > 0) max_cam_step = std::max(max_cam_step, cam_pos.distance(prev_cam));
                prev_cam = cam_pos;
            }
            end_rise = controller.get_position().z - hang.z;
            // The heave's samples at retail's times (frames interpolated on the move's own clock).
            for (const Ref& ref : kRetailHeave) {
                for (size_t k = 1; k < heave_samples.size(); ++k) {
                    const Sample& a = heave_samples[k - 1];
                    const Sample& b = heave_samples[k];
                    if (b.t < ref.t || a.t > ref.t) continue;
                    const float w = b.t > a.t ? (ref.t - a.t) / (b.t - a.t) : 0.0f;
                    const float cap = a.cap + (b.cap - a.cap) * w;
                    const float eye = a.eye + (b.eye - a.eye) * w;
                    if (std::abs(cap - ref.cap) > std::abs(worst_cap)) worst_cap = cap - ref.cap;
                    if (std::abs(eye - ref.eye) > std::abs(worst_eye)) worst_eye = eye - ref.eye;
                    eye_report << " " << ref.t << "s:" << static_cast<int>(std::lround(eye)) << "/" << static_cast<int>(std::lround(ref.eye));
                    break;
                }
            }
        }
        const bool s20_heave = pulled && heave_anim == "HangHeaveUp" && heave_samples.size() >= 80;
        // The camera follows retail's to 5 uu, the pawn the root motion to 1.5 uu.
        const bool s20_track = s20_heave && eye_report.str().size() > 0 && std::abs(worst_cap) <= 1.5f && std::abs(worst_eye) <= 5.0f;
        // The move lasts the animation (OnCustomAnimEnd) and ends on the roof (the lip is 182.8 over the
        // hanging feet, and she hovers over it), walking, with no jump of
        // the camera on the way (the old end shoved her 76 uu forward in one frame).
        const bool s20_end = walked && last_heave_t > kHangHeaveUpLength - kDt - 1e-3f && last_heave_t < kHangHeaveUpLength &&
                             std::abs(end_rise - (182.8f + kPawnFloorHover)) <= 1.5f && max_cam_step < 12.0f;
        s20_pass = esc_ok && grabbed && s20_heave && s20_track && s20_end;
        std::cout << "  -> Stage 20 Result: " << (s20_pass ? "PASS" : "FAIL")
                  << " (Escape_p=" << (esc_ok ? "OK" : "NO") << ", Grab=" << (grabbed ? "OK" : "NO")
                  << ", Heave=" << (heave_anim.empty() ? "none" : heave_anim)
                  << ", RootMotion=" << (std::abs(worst_cap) <= 1.5f ? "OK" : "FAIL") << " [worst " << worst_cap << " uu]"
                  << ", Camera=" << (std::abs(worst_eye) <= 5.0f && !eye_report.str().empty() ? "OK" : "FAIL")
                  << " [eye over hang centre port/retail" << eye_report.str() << "; worst " << worst_eye << " uu]"
                  << ", End=" << (s20_end ? "OK" : "FAIL") << " [" << last_heave_t << " s, rise " << end_rise
                  << " uu, largest camera step " << max_cam_step << " uu])" << std::endl;
    }

    // Stages with pass/fail assertions: parkour stages 1-8, cutscene stage 11, door barging stage 12, pipe climb/balance stage 13, SP02 sprint stage 14, zipline/swing/ledge stage 15, camera stage 16, damage screen effects stage 17, pause menu stage 18, electric fence stage 19, and ledge pull-up stage 20
    // Stage 21: the level's script acting on the scene (oracle_script_effects above).
    std::cout << "[Oracle Stage 21] Testing the Level Script's Effects (breakable glass, emitter factories, toggles)..." << std::endl;
    const bool s21_pass = oracle_script_effects(game_root);

    // Stage 22: the hand-over at the end of a level intro (measure_intro_handover above), against what
    // retail's intro traces show (docs/LEVEL_INTROS.md section 5): as the pawn's own animation comes back
    // the eye is 6.5 uu over its standing place and comes down in five frames, and it rests where the
    // intro's stand-in left it less the pawn's settling onto its hover over the floor. The Boat is the
    // chapter that ends facing backwards along the world's axis (yaw 178): a body yaw that lags a frame
    // would throw the eye 16 uu.
    std::cout << "[Oracle Stage 22] Testing the Camera at a Level Intro's Hand-Over (retail's lift and rest)..." << std::endl;
    bool s22_pass = true;
    {
        struct Case {
            const char* map;
            float retail_up;    // retail's eye in the first frame, over the intro's last
            float retail_rest;  // and where it rests
        };
        // tools/retail/intro_capture traces, 2026-10-08 (build/re/cam/handover_table.py).
        const Case cases[] = {{"Maps/SP07/Boat_p.me1", 3.79f, -2.63f}, {"Maps/SP05/Mall_p.me1", 5.85f, -0.70f}};
        for (const Case& c : cases) {
            const HandoverResult r = measure_intro_handover(renderer, game_root, c.map, false);
            const bool ok = r.valid && r.first_side < 0.5f && std::abs(r.first_up - c.retail_up) < 0.6f &&
                            std::abs(r.rest_up - c.retail_rest) < 0.6f && r.settle < 1.6f && r.glide_frames >= 4 && r.glide_frames <= 6 &&
                            r.turn_deg < 1.5f;
            s22_pass = s22_pass && ok;
            std::cout << std::fixed << std::setprecision(2) << "  [" << c.map << "] " << (ok ? "OK" : "FAIL") << ": first frame up " << r.first_up
                      << " (retail " << c.retail_up << "), sideways " << r.first_side << ", rests " << r.rest_up << " (retail " << c.retail_rest
                      << "), down in " << r.glide_frames << " frames, largest step " << r.settle << ", turn " << r.turn_deg << " deg"
                      << std::defaultfloat << std::endl;
        }
        std::cout << "  -> Stage 22 Result: " << (s22_pass ? "PASS" : "FAIL") << std::endl;
    }

    // Stages with pass/fail assertions: parkour stages 1-8, cutscene stage 11, door barging stage 12, pipe climb/balance stage 13, SP02 sprint stage 14, zipline/swing/ledge stage 15, camera stage 16, damage screen effects stage 17, pause menu stage 18, and electric fence stage 19
    // (stages 9 and 10 only render screenshots).
    const bool stage_results[] = {s1_pass, s2_pass, s3_pass, s4_pass, s5_pass, s6_pass, s7_pass, s8_pass, s11_pass, s12_pass, s13_pass, s14_pass, s15_pass, s16_pass, s17_pass, s18_pass, s19_pass, s20_pass, s21_pass, s22_pass};
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

// A key by the name the engine gives it, which is what the front end's CONTROLS screen binds and
// TdGameUI.int names ("GMS_SpaceBar=SPACE"). "" for a key the game has no name for.
static std::string frontend_key_name(SDL_Keycode sym) {
    if (sym >= SDLK_a && sym <= SDLK_z) return std::string(1, static_cast<char>('A' + (sym - SDLK_a)));
    static const char* const kDigits[10] = {"Zero", "One", "Two", "Three", "Four", "Five", "Six", "Seven", "Eight", "Nine"};
    if (sym >= SDLK_0 && sym <= SDLK_9) return kDigits[sym - SDLK_0];
    if (sym >= SDLK_F1 && sym <= SDLK_F12) return "F" + std::to_string(sym - SDLK_F1 + 1);
    if (sym >= SDLK_KP_1 && sym <= SDLK_KP_9) return std::string("NumPad") + kDigits[sym - SDLK_KP_1 + 1];
    switch (sym) {
        case SDLK_KP_0:         return "NumPadZero";
        case SDLK_SPACE:        return "SpaceBar";
        case SDLK_RETURN:
        case SDLK_KP_ENTER:     return "Enter";
        case SDLK_ESCAPE:       return "Escape";
        case SDLK_TAB:          return "Tab";
        case SDLK_BACKSPACE:    return "BackSpace";
        case SDLK_CAPSLOCK:     return "CapsLock";
        case SDLK_LSHIFT:       return "LeftShift";
        case SDLK_RSHIFT:       return "RightShift";
        case SDLK_LCTRL:        return "LeftControl";
        case SDLK_RCTRL:        return "RightControl";
        case SDLK_LALT:         return "LeftAlt";
        case SDLK_RALT:         return "RightAlt";
        case SDLK_LEFT:         return "Left";
        case SDLK_RIGHT:        return "Right";
        case SDLK_UP:           return "Up";
        case SDLK_DOWN:         return "Down";
        case SDLK_INSERT:       return "Insert";
        case SDLK_DELETE:       return "Delete";
        case SDLK_HOME:         return "Home";
        case SDLK_END:          return "End";
        case SDLK_PAGEUP:       return "PageUp";
        case SDLK_PAGEDOWN:     return "PageDown";
        case SDLK_PAUSE:        return "Pause";
        case SDLK_NUMLOCKCLEAR: return "NumLock";
        case SDLK_SCROLLLOCK:   return "ScrollLock";
        case SDLK_KP_MULTIPLY:  return "Multiply";
        case SDLK_KP_PLUS:      return "Add";
        case SDLK_KP_MINUS:     return "Subtract";
        case SDLK_KP_PERIOD:    return "Decimal";
        case SDLK_KP_DIVIDE:    return "Divide";
        case SDLK_SEMICOLON:    return "Semicolon";
        case SDLK_EQUALS:       return "Equals";
        case SDLK_COMMA:        return "Comma";
        case SDLK_MINUS:        return "Underscore";
        case SDLK_PERIOD:       return "Period";
        case SDLK_SLASH:        return "Slash";
        case SDLK_BACKQUOTE:    return "Tilde";
        case SDLK_LEFTBRACKET:  return "LeftBracket";
        case SDLK_BACKSLASH:    return "Backslash";
        case SDLK_RIGHTBRACKET: return "RightBracket";
        case SDLK_QUOTE:        return "Quote";
        default:                return {};
    }
}

// -----------------------------------------------------------------------------
// Interactive SDL2 Window Gameplay Loop (Metal on macOS, Direct3D 11 on Windows)
// -----------------------------------------------------------------------------
// --intro-shots: the level's intro posed at given Matinee times and rendered headless, one PNG a
// time. The intro's camera is retail's to within a unit (docs/LEVEL_INTROS.md), so each picture
// has a retail frame taken from the same place to be held against (tools/retail/render_check.py).
// -----------------------------------------------------------------------------
static int run_intro_shots(const std::string& game_root, const std::string& map_rel, const std::string& times_csv,
                           const std::string& out_dir) {
    using namespace me;
    Renderer renderer;
    renderer.set_game_root(game_root);
    if (!renderer.init_headless(1280, 720)) {
        std::cerr << "[Shots] Renderer::init_headless failed" << std::endl;
        return 1;
    }
    LevelScene scene;
    if (!load_level_scene(game_root, map_rel, scene) || !scene.level_intro.valid) {
        std::cerr << "[Shots] " << map_rel << ": not loaded, or it has no intro" << std::endl;
        return 1;
    }
    // ME_SHOT_CHECKPOINT=<n>: the sublevels of that checkpoint, to look at a place the level's opening does not load.
    const char* checkpoint_spec = std::getenv("ME_SHOT_CHECKPOINT");
    stream_level_to_checkpoint(game_root, scene, checkpoint_spec ? std::atoi(checkpoint_spec) : 0);
    ensure_dir(out_dir);

    MovementConfig move_cfg;
    load_movement_config_from_ini(get_config_path(game_root, "DefaultPawnMovement.ini"), move_cfg);
    ParkourController controller(move_cfg);
    controller.reset(scene.player_spawn_pos, scene.player_spawn_yaw);

    std::string stem = map_rel.substr(map_rel.find_last_of("/\\") + 1);
    stem = stem.substr(0, stem.find('.'));
    std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    std::vector<float> wanted;
    {
        std::stringstream times(times_csv);
        std::string item;
        while (std::getline(times, item, ',')) wanted.push_back(static_cast<float>(std::atof(item.c_str())));
        std::sort(wanted.begin(), wanted.end());
    }

    // The intro played through from its start in steps of a thirtieth of a second at most, so that
    // what adapts over time (the exposure, which a level opens at its brightest) is where the game
    // would have it at each picture.
    CutscenePlayer cutscene;
    PlayerTelemetry tel = controller.get_telemetry();
    cutscene.play_in_engine_intro(scene, tel);
    ScreenFade fade;
    fade.restart();
    float now = 0.0f;
    int written = 0;
    // ME_SHOT_LOOK="pitch,yaw" (degrees): the view turned by hand, to look at something the intro does not.
    float look_pitch = 0.0f, look_yaw = 0.0f;
    const char* look_spec = std::getenv("ME_SHOT_LOOK");
    const bool look = look_spec && std::sscanf(look_spec, "%f,%f", &look_pitch, &look_yaw) == 2;
    // ME_SHOT_FIRE="ammo" (0 a light weapon, 1 a heavy one, 2 a helicopter's gun, 3 a shotgun): a fifth
    // of a second before each picture five shots are fired from the view along it, to see what
    // they leave where they land (game/impact_effects.hpp).
    const char* fire_spec = std::getenv("ME_SHOT_FIRE");
    const int fire_ammo = fire_spec ? std::clamp(std::atoi(fire_spec), 0, 3) : -1;
    float fire_at = -1.0f;
    // ME_SHOT_STAND="x,y,z" (feet): the pictures are the player's, standing there, not the intro's.
    Vec3 stand_at(0.0f, 0.0f, 0.0f);
    const char* stand_spec = std::getenv("ME_SHOT_STAND");
    const bool standing = stand_spec && std::sscanf(stand_spec, "%f,%f,%f", &stand_at.x, &stand_at.y, &stand_at.z) == 3;
    // ME_SHOT_MOVERS="degrees,units[,1]": the level's doors and lifts are listed; the shots are
    // fired before the first picture only, and after it every door is swung by so many degrees and
    // every part of a lift raised by so many units, to see the bullet holes go with them. With the
    // third number the shots are fired before the later pictures too, at the movers where they are then.
    float swing_deg = 0.0f, raise = 0.0f;
    int fire_again = 0;
    const char* movers_spec = std::getenv("ME_SHOT_MOVERS");
    const bool movers = movers_spec && std::sscanf(movers_spec, "%f,%f,%d", &swing_deg, &raise, &fire_again) >= 2;
    bool movers_moved = false;
    // ME_SHOT_BODY="units": the shots stop in a person that far ahead, not in the level: the
    // effect and the sound of a body.
    const char* body_spec = std::getenv("ME_SHOT_BODY");
    const float body_at = body_spec ? static_cast<float>(std::atof(body_spec)) : 0.0f;
    if (movers) {
        for (size_t d = 0; d < scene.barge_doors.size(); ++d) {
            const BargeDoorInstance& door = scene.barge_doors[d];
            std::cout << "[Shots] door " << d << " " << door.name << ": hinge (" << door.hinge_pos.x << ", " << door.hinge_pos.y << ", "
                      << door.hinge_pos.z << "), middle (" << door.center_pos.x << ", " << door.center_pos.y << ", " << door.center_pos.z
                      << "), " << door.parts.size() << " parts" << std::endl;
        }
        for (size_t e = 0; e < scene.elevators.size(); ++e) {
            const ElevatorInstance& lift = scene.elevators[e];
            std::cout << "[Shots] lift " << e << " " << lift.name << ": cab at (" << lift.start_pos.x << ", " << lift.start_pos.y << ", "
                      << lift.start_pos.z << "), half (" << lift.cab_half_extents.x << ", " << lift.cab_half_extents.y << ", "
                      << lift.cab_half_extents.z << "), offset (" << lift.cab_local_offset.x << ", " << lift.cab_local_offset.y << ", "
                      << lift.cab_local_offset.z << "), " << lift.parts.size() << " parts" << std::endl;
        }
    }
    auto frame = [&](float dt) {
        cutscene.update(dt, scene, tel);
        if (standing) {
            tel.position = stand_at;
            tel.intro_active = false;
            tel.intro_camera_only = false;
        }
        if (look) {
            tel.pitch_deg = look_pitch;
            tel.yaw_deg = look_yaw;
            tel.camera_roll_deg = 0.0f;
        }
        if (fire_ammo >= 0 && fire_at >= 0.0f && now + dt >= fire_at && scene.collision) {
            fire_at = -1.0f;
            Vec3 eye;
            Rotator rot;
            renderer.player_camera(tel, eye, rot);
            static const float kSpread[5][2] = {{0.0f, 0.0f}, {-0.09f, 0.05f}, {0.09f, 0.05f}, {-0.05f, -0.07f}, {0.06f, -0.06f}};
            for (const auto& s : kSpread) {
                const Vec3 dir = (rot.forward() + rot.right() * s[0] + rot.up() * s[1]).normalized();
                const ImpactSurface hit = find_impact_surface(scene, eye, eye + dir * 8000.0f);  // the level, a lift or a door
                if (!hit.hit && body_at <= 0.0f) continue;
                BulletTracer tr;
                tr.start_pos = eye + dir * 30.0f;
                tr.end_pos = hit.location;
                tr.ammo = static_cast<uint8_t>(fire_ammo);
                if (body_at > 0.0f) {
                    tr.end_pos = eye + dir * body_at;
                    tr.from_player = true;
                    tr.hit_enemy = true;
                    tr.pawn_hit = 1;
                    tr.pawn_normal = dir * -1.0f;
                }
                scene.active_tracers.push_back(tr);
            }
            const size_t effects = scene.spawned_effects.size(), holes = scene.dynamic_decals.size();
            std::vector<SimSoundEvent> heard;
            update_impact_effects(scene, 0.0f, eye, &heard);
            std::cout << "[Shots] fired " << scene.active_tracers.size() << " shots: " << scene.spawned_effects.size() - effects
                      << " impact effects, " << scene.dynamic_decals.size() - holes << " bullet holes, " << heard.size() << " impact sounds"
                      << std::endl;
            scene.active_tracers.clear();
        }
        update_impact_effects(scene, dt, tel.position);
        now += dt;
        (void)cutscene.take_intro_sounds();
        for (const IntroFadeEvent& ev : cutscene.take_intro_fades()) fade.apply(ev);
        fade.update(dt);
        CutscenePlayer::pose_intro_doors(scene, now);
        if (movers_moved) {
            for (BargeDoorInstance& door : scene.barge_doors) {
                door.open_angle_rad = swing_deg * 0.01745329252f;
                const float c = std::cos(door.open_angle_rad), s = std::sin(door.open_angle_rad);
                Mat4 m = Mat4::identity();
                m.m[0] = c;
                m.m[1] = s;
                m.m[4] = -s;
                m.m[5] = c;
                m.m[12] = door.hinge_pos.x - c * door.hinge_pos.x + s * door.hinge_pos.y;
                m.m[13] = door.hinge_pos.y - s * door.hinge_pos.x - c * door.hinge_pos.y;
                door.model_matrix = m;
            }
            for (ElevatorInstance& lift : scene.elevators) {
                for (ElevatorPart& part : lift.parts) part.offset = Vec3(0.0f, 0.0f, raise);
            }
        }
        tel.speed_2d = 0.0f;  // no speed effects
        tel.sim_time = now;
        tel.fade_amount = standing ? 1.0f : fade.shown();
        tel.fade_color = fade.color;
        tel.exposure_reset = dt <= 0.0f;  // the level opens
        tel.screen_effects = screen_effects_from_environment();
        renderer.render_frame(scene, tel);
    };
    frame(0.0f);
    for (float t : wanted) {
        fire_at = movers && written > 0 && fire_again == 0 ? -1.0f : std::max(t - 0.2f, 0.0f);
        movers_moved = movers && written > 0;
        const int steps = std::max(1, static_cast<int>(std::ceil((t - now) * 30.0f)));
        const float dt = (t - now) / static_cast<float>(steps);
        for (int i = 0; i < steps && dt > 0.0f; ++i) frame(dt);
        char name[64];
        std::snprintf(name, sizeof(name), "%s_%06.2f.png", stem.c_str(), t);
        if (renderer.save_screenshot_png(out_dir + "/" + name)) ++written;
    }
    std::cout << "[Shots] " << stem << ": " << written << " frame(s) in " << out_dir << std::endl;
    return written > 0 ? 0 : 1;
}

// -----------------------------------------------------------------------------
static int run_interactive_app(const std::string& game_root, int initial_chapter,
                              const std::string& custom_level, int max_frames,
                              bool start_in_main_menu, const std::string& trace_path) {
    using namespace me;
    std::cout << "\n============================================================" << std::endl;
    std::cout << "  MIRROR'S EDGE NATIVE " << platform_upper() << " - INTERACTIVE LAUNCH" << std::endl;
    std::cout << "============================================================" << std::endl;

    SDL_SetMainReady();
#if defined(_WIN32)
    // Window sizes in points and the drawable in pixels, as on a Retina display.
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_SCALING, "1");
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        std::cerr << "[SDL ERROR] Initialization failed: " << SDL_GetError() << std::endl;
        return 1;
    }

    int win_w = 1280;
    int win_h = 720;
#if defined(ME_RENDERER_OPENGL)
    // The OpenGL backend (Linux, or macOS when built to check it): the window carries the
    // GL context the renderer creates in init_with_window().
    const char* window_title = "Mirror's Edge (Native " ME_PLATFORM_TITLE " OpenGL)";
    const Uint32 window_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    Renderer::prepare_window_attributes();
#elif defined(__APPLE__)
    const char* window_title = "Mirror's Edge (Native macOS Apple Silicon)";
    const Uint32 window_flags = SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
#else
    const char* window_title = "Mirror's Edge (Native Windows Direct3D 11)";
    const Uint32 window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
#endif
    SDL_Window* window = SDL_CreateWindow(
        window_title,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        win_w, win_h,
        window_flags
    );

    if (!window) {
        std::cerr << "[SDL ERROR] Failed to create window: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return 1;
    }

    // What the renderer draws into: a CAMetalLayer on macOS, the window itself (HWND) on Windows,
    // the SDL window's GL context on Linux.
#if defined(ME_RENDERER_OPENGL)
    auto get_drawable_size = [&](int* w, int* h) { SDL_GL_GetDrawableSize(window, w, h); };
    auto destroy_surface = []() {};
#elif defined(__APPLE__)
    SDL_MetalView metal_view = SDL_Metal_CreateView(window);
    if (!metal_view) {
        std::cerr << "[SDL ERROR] Failed to create Metal view: " << SDL_GetError() << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    auto get_drawable_size = [&](int* w, int* h) { SDL_Metal_GetDrawableSize(window, w, h); };
    auto destroy_surface = [&]() { SDL_Metal_DestroyView(metal_view); };
#else
    auto get_drawable_size = [&](int* w, int* h) { SDL_GetWindowSizeInPixels(window, w, h); };
    auto destroy_surface = []() {};
#endif

    int drawable_w = win_w;
    int drawable_h = win_h;
    get_drawable_size(&drawable_w, &drawable_h);

    Renderer renderer;
    renderer.set_game_root(game_root);
#if defined(ME_RENDERER_OPENGL)
    const bool renderer_ok = renderer.init_with_window(window, drawable_w, drawable_h);
#elif defined(__APPLE__)
    const bool renderer_ok = renderer.init_with_metal_layer(SDL_Metal_GetLayer(metal_view), drawable_w, drawable_h);
#else
    SDL_SysWMinfo wm_info;
    SDL_VERSION(&wm_info.version);
    const bool renderer_ok = SDL_GetWindowWMInfo(window, &wm_info) == SDL_TRUE &&
                             renderer.init_with_window(wm_info.info.win.window, drawable_w, drawable_h);
#endif
    if (!renderer_ok) {
        std::cerr << "[Renderer ERROR] Could not initialize the renderer for the window!" << std::endl;
        destroy_surface();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    AudioEngine audio;
    audio.init(false);
    audio.load_stock_audio(game_root);

    // --trace: one JSON line per frame (the camera, the pawn, what is playing) and one per sound asked
    // to play, in the shape of a retail recording (tools/retail/tracefile.py), so the two can be laid
    // over each other.
    std::ofstream trace_file;
    if (!trace_path.empty()) {
        trace_file.open(trace_path, std::ios::out | std::ios::trunc);
        if (!trace_file.is_open()) std::cerr << "[Trace] Cannot write " << trace_path << std::endl;
        audio.set_play_log(trace_file.is_open());
    }

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
    // The first-person mesh's foot placement asks the level where the floor is under each foot.
    renderer.set_world_trace([&controller, &active_scene](const Vec3& from, const Vec3& to, Vec3& hit, Vec3& normal) {
        return controller.leg_line_check(from, to, active_scene, hit, normal);
    });

    CutscenePlayer cutscene_player;
    cutscene_player.init(game_root, /*headless=*/(max_frames > 0));
    renderer.set_cutscene_player(&cutscene_player);

    bool pending_level_loaded_audio = false;
    bool was_vo_playing = false;

    // The level's Kismet, run against the player (src/game/level_script.hpp), and the localized
    // text it puts on the screen.
    LocalizedStrings strings;
    strings.load(game_root);
    LevelScript script;
    bool splash_hint_open = false;      // SeqAct_TdTriggerSplashHint pauses the game under its card
    bool into_cutscene_pending = false; // SeqAct_TdIntoCutscene: the pawn is being moved onto the mark
    Vec3 into_cutscene_target(0.0f, 0.0f, 0.0f);
    float into_cutscene_yaw = 0.0f;
    float into_cutscene_timer = 0.0f;
    struct PendingTransition {
        bool pending = false;
        std::string level;
        std::string checkpoint;
        float delay = 0.0f;
    } pending_transition;
    std::string start_checkpoint_name;  // the checkpoint the level was entered at

    // "Escape_p" -> "Maps/SP01/Escape_p.me1": the chapter maps by file name, any case.
    auto find_map_file = [&](const std::string& level_name) -> std::string {
        namespace fs = std::filesystem;
        const std::string want = lower(level_name) + ".me1";
        const fs::path maps = fs::path(game_root) / "TdGame" / "CookedPC" / "Maps";
        std::error_code ec;
        for (const auto& dir : fs::directory_iterator(maps, ec)) {
            if (!dir.is_directory()) continue;
            for (const auto& f : fs::directory_iterator(dir.path(), ec)) {
                if (f.is_regular_file() && lower(f.path().filename().string()) == want) {
                    return "Maps/" + dir.path().filename().string() + "/" + f.path().filename().string();
                }
            }
        }
        return std::string();
    };

    // Config/DefaultEngine.ini [LoadMovies]: the movie that plays while a chapter loads (Edge has
    // none; the training area and every other chapter their scene_NN).
    std::unordered_map<std::string, std::string> load_movies;
    {
        IniConfig engine_ini;
        if (engine_ini.load_file(get_config_path(game_root, "DefaultEngine.ini"))) {
            for (const auto& [key, values] : engine_ini.get_section_keys("LoadMovies")) {
                if (!values.empty()) load_movies[lower(key)] = values.back();
            }
        }
    }
    auto load_movie_for = [&](const std::string& map_file) -> std::string {
        const std::string stem = lower(std::filesystem::path(map_file).stem().string());
        auto it = load_movies.find(stem);
        if (it != load_movies.end()) return it->second;
        return load_movies.empty() ? CutscenePlayer::get_chapter_intro_movie(map_file) : std::string();
    };

    // The screen fade (TdHUD's FadeInEffect), declared here for the script host below.
    ScreenFade screen_fade;
    ScreenEffects screen_effects;
    bool level_play_pending = false;  // the loading movie is on; the level begins play after it

    // Where a cutscene leaves the pawn: at its animation's root unless its Kismet teleported the
    // pawn elsewhere on the way (Edge's skip puts her at the After_Intro checkpoint).
    Vec3 handover_pos(0.0f, 0.0f, 0.0f);
    float handover_yaw = 0.0f;
    bool handover_set = false;
    auto hand_over = [&](const LevelIntroSequence& seq) {
        if (handover_set) controller.hand_over(handover_pos - Vec3(0.0f, 0.0f, 2.0f), handover_yaw, active_scene);
        else controller.hand_over(seq.end_feet_pos, seq.end_yaw_deg, active_scene);
        handover_set = false;
    };
    // A cutscene a key stops in a level without a script to stop it: the player stands at the
    // level's start, and a fade that had taken the picture out (the training area's pan fades to
    // white at its end) is undone, as the Kismet it cut short would have done.
    auto abandon_cutscene = [&]() {
        controller.reset(active_scene.player_spawn_pos, active_scene.player_spawn_yaw);
        controller.get_telemetry().intro_active = false;
        controller.get_telemetry().intro_camera_only = false;
        cutscene_player.stop();
        if (screen_fade.amount < 1.0f) screen_fade.fade_in(0.5f, screen_fade.color);
    };

    // What the level's Kismet does to the game. docs/GAMEPLAY_SCRIPTING_RE.md.
    auto make_script_host = [&]() {
        ScriptHost host;
        host.log = [&](const std::string& line) {
            std::cout << std::fixed << std::setprecision(2) << "[" << script.time() << "s] " << line << std::defaultfloat << std::endl;
        };
        host.set_checkpoint = [&](const ScriptActor& cp, bool teleport, bool /*save*/) {
            // The index the HUD counts is the checkpoint's place in the chapter's weighted list.
            int index = controller.get_telemetry().active_checkpoint;
            for (size_t c = 0; c < active_scene.checkpoint_infos.size(); ++c) {
                if (lower(active_scene.checkpoint_infos[c].checkpoint_name) == lower(cp.checkpoint_name)) {
                    index = static_cast<int>(c);
                    if (!active_scene.checkpoint_infos[c].streaming_levels.empty()) {
                        active_scene.loaded_sublevel_packages = active_scene.checkpoint_infos[c].streaming_levels;
                        script.set_loaded_packages(active_scene.loaded_sublevel_packages);
                    }
                }
            }
            controller.set_checkpoint(cp.location + Vec3(0.0f, 0.0f, 35.0f), cp.yaw_deg, index, cp.checkpoint_name);
            if (teleport) controller.reset(cp.location + Vec3(0.0f, 0.0f, 35.0f), cp.yaw_deg);
        };
        host.level_completed = [&](const std::string& next_level, const std::string& next_checkpoint) {
            if (pending_transition.pending) return;
            pending_transition.pending = true;
            pending_transition.level = next_level;
            pending_transition.checkpoint = next_checkpoint;
            pending_transition.delay = 0.0f;
        };
        host.show_tutorial = [&](const std::string& key, float /*duration*/, bool /*replace*/) {
            controller.get_telemetry().tutorial_text = strings.tutorial_message(key);
        };
        host.hide_tutorial = [&]() { controller.get_telemetry().tutorial_text.clear(); };
        host.show_supers = [&](const std::string& text, float duration) {
            controller.get_telemetry().supers_text = strings.resolve(text);
            controller.get_telemetry().supers_time_left = duration;
        };
        host.show_subtitle = [&](const std::string& text, float duration) {
            controller.get_telemetry().sign_text = strings.resolve(text);
            controller.get_telemetry().sign_time_left = duration;
        };
        host.splash_hint = [&](int number) {
            const std::string text = strings.splash_hint(number);
            if (text.empty()) return;
            controller.get_telemetry().splash_hint_title = strings.lookup("TdGameUI", "TdSplashHints", "TitleLabelText");
            if (controller.get_telemetry().splash_hint_title.empty()) controller.get_telemetry().splash_hint_title = "HINT";
            controller.get_telemetry().splash_hint_text = text;
            splash_hint_open = true;
        };
        host.fade = [&](bool fade_out, float time, const Vec3& color) {
            IntroFadeEvent ev;
            ev.time = 0.0f;
            ev.fade_out = fade_out;
            ev.duration = time;
            ev.color = color;
            screen_fade.apply(ev);
        };
        host.play_cutscene = [&](int index, float play_rate) {
            handover_set = false;
            cutscene_player.play_level_cutscene(active_scene, index, play_rate);
        };
        host.stop_cutscene = [&]() {
            if (cutscene_player.is_level_intro()) {
                if (const LevelIntroSequence* seq = cutscene_player.active_sequence(active_scene)) {
                    CutscenePlayer::pose_sequence_doors(active_scene, *seq, 1.0e9f);
                    hand_over(*seq);
                }
                controller.get_telemetry().intro_active = false;
                cutscene_player.stop();
            }
        };
        host.into_cutscene = [&](const Vec3& location, float yaw_deg) {
            // TdMove_IntoCutscene: the pawn is carried onto the mark over a short blend.
            into_cutscene_pending = true;
            into_cutscene_target = location;
            into_cutscene_yaw = yaw_deg;
            into_cutscene_timer = 0.0f;
        };
        host.play_sound = [&](const std::string& cue, const std::string& bank, bool voice, float volume, const Vec3* at) -> float {
            if (!audio.has_cue(cue)) audio.load_cue_bank(game_root, bank);
            if (!audio.has_cue(cue)) {
                std::cout << "[Script] sound not loaded: " << cue << " (" << bank << ")" << std::endl;
                return 0.0f;
            }
            if (at && !voice) {
                audio.play_sound_3d(cue, *at, std::clamp(volume, 0.0f, 1.5f));
            } else if (!audio.play_cue(cue, voice, std::clamp(volume, 0.0f, 1.5f))) {
                return 0.0f;
            }
            return std::max(0.05f, audio.cue_duration(cue));
        };
        host.stop_sound = [&](const std::string& cue) { audio.stop_cue(cue); };
        host.teleport_player = [&](const Vec3& location, float yaw_deg) {
            // During a cutscene the camera is the animation's; a teleport onto the cutscene's
            // stand-in is the attach that starts it (the root motion carries the pawn from there),
            // any other is where the pawn stands when the Matinee hands over.
            if (const LevelIntroSequence* seq = cutscene_player.active_sequence(active_scene)) {
                if ((location - seq->actor_location).length() < 1.0f) return;
                handover_pos = location + Vec3(0.0f, 0.0f, 2.0f);
                handover_yaw = yaw_deg;
                handover_set = true;
                return;
            }
            controller.reset(location + Vec3(0.0f, 0.0f, 2.0f), yaw_deg);
        };
        host.player_fail = [&]() {
            if (cutscene_player.is_playing() || into_cutscene_pending || script.cinematic_mode()) return;
            controller.get_telemetry().health = 0.0f;
        };
        host.damage_player = [&](float amount) {
            if (cutscene_player.is_playing() || into_cutscene_pending || script.cinematic_mode()) return;
            controller.apply_damage(amount, 0, Vec3(0.0f, 0.0f, 0.0f));
        };
        host.stream_levels = [&](const std::vector<std::string>& levels, bool load) {
            auto& loaded = active_scene.loaded_sublevel_packages;
            for (const std::string& l : levels) {
                auto it = std::find_if(loaded.begin(), loaded.end(), [&](const std::string& s) { return lower(s) == lower(l); });
                if (load && it == loaded.end()) loaded.push_back(l);
                if (!load && it != loaded.end()) loaded.erase(it);
            }
            script.set_loaded_packages(loaded);
        };
        host.line_clear = [&](const Vec3& from, const Vec3& to) {
            Vec3 hit, normal;
            return !controller.leg_line_check(from, to, active_scene, hit, normal);
        };
        // What it does to the level's actors and effects (game/script_effects.hpp).
        bind_scene_effects(host, active_scene);
        return host;
    };

    // The level begins play (after its loading movie, or at once): retail's
    // TdSPStoryGame.TriggerEventsOnLevelReload, which fires every SeqEvent_LevelLoaded and the
    // active checkpoint's SeqEvt_TdCheckpointLoaded / Activated. The chapter's intro Matinee is
    // what those chains reach; a level whose script does not reach one plays it directly, as
    // before, and says so.
    auto begin_level_play = [&]() {
        level_play_pending = false;
        if (active_scene.script && active_scene.script->valid()) {
            script.init(active_scene.script, make_script_host());
            script.set_loaded_packages(active_scene.loaded_sublevel_packages);
            const ScriptActor* cp = script.begin_play(start_checkpoint_name);
            const bool at_default = start_checkpoint_name.empty() || (cp && cp->default_checkpoint);
            if (cp) {
                int index = controller.get_telemetry().active_checkpoint;
                for (size_t c = 0; c < active_scene.checkpoint_infos.size(); ++c) {
                    if (lower(active_scene.checkpoint_infos[c].checkpoint_name) == lower(cp->checkpoint_name)) index = static_cast<int>(c);
                }
                // The pawn spawns at the checkpoint (TdSPStoryGame.FindPlayerStart), as retail's does;
                // the intro's Kismet teleports it from there onto its mark. (Without a script the
                // level's spawn is the intro's end, for the hand-over.)
                if (cp->location.length_xy() > 1.0f) controller.reset(cp->location + Vec3(0.0f, 0.0f, 35.0f), cp->yaw_deg);
                controller.set_checkpoint(cp->location + Vec3(0.0f, 0.0f, 35.0f), cp->yaw_deg, index, cp->checkpoint_name);
            }
            ScriptPlayerState ps;
            ps.position = controller.get_telemetry().position;
            ps.eye = ps.position + Vec3(0.0f, 0.0f, controller.get_telemetry().eye_height);
            script.update(0.0f, ps);
            if (script.playing_cutscene() < 0 && active_scene.level_intro.valid && at_default) {
                std::cout << "[Script] the chapter's start did not reach its intro Matinee; playing it directly" << std::endl;
                cutscene_player.play_in_engine_intro(active_scene, controller.get_telemetry(), 4.5f);
            }
        } else if (active_scene.level_intro.valid && start_checkpoint_name.empty()) {
            cutscene_player.play_in_engine_intro(active_scene, controller.get_telemetry(), 4.5f);
        }
    };

    auto load_chapter_or_level = [&](int ch_idx, const std::string& custom_path, bool play_intro = true,
                                     const std::string& checkpoint_name = std::string(),
                                     bool defer_begin_play = false) {
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
        cutscene_player.stop();
        script.init(nullptr, ScriptHost{});
        active_scene = std::move(loaded_scene);
        controller.reset(active_scene.player_spawn_pos, active_scene.player_spawn_yaw);
        controller.get_telemetry().active_checkpoint = 0;
        controller.get_telemetry().tutorial_text.clear();
        controller.get_telemetry().supers_text.clear();
        controller.get_telemetry().sign_text.clear();
        controller.get_telemetry().splash_hint_text.clear();
        splash_hint_open = false;
        into_cutscene_pending = false;
        pending_transition = PendingTransition{};
        start_checkpoint_name = checkpoint_name;
        if (!active_scene.subtitles.empty() && !active_scene.subtitles[0].empty()) {
            controller.get_telemetry().active_subtitle = active_scene.subtitles[0];
        }
        audio.load_level_audio(game_root, map_file);
        // The cutscenes' cues come from packages of their own (door hits, cutscene foley, the
        // opening's long tracks, the voice lines): whatever is not loaded yet is fetched from the
        // one it names.
        for (const IntroSoundEvent& ev : active_scene.level_intro.sounds) {
            if (!ev.cue.empty() && !audio.has_cue(ev.cue)) audio.load_cue_bank(game_root, ev.bank);
        }
        for (const LevelIntroSequence& cs : active_scene.cutscenes) {
            for (const IntroSoundEvent& ev : cs.sounds) {
                if (!ev.cue.empty() && !audio.has_cue(ev.cue)) audio.load_cue_bank(game_root, ev.bank);
            }
        }
        if (active_scene.script) {
            for (const ScriptGraph::Node& n : active_scene.script->nodes) {
                if (!n.cue.empty() && !audio.has_cue(n.cue)) audio.load_cue_bank(game_root, n.cue_bank);
            }
            for (const ScriptMatinee& m : active_scene.script->matinees) {
                for (const ScriptSound& s : m.sounds) {
                    if (!s.cue.empty() && !audio.has_cue(s.cue)) audio.load_cue_bank(game_root, s.bank);
                }
            }
        }
        // The surfaces' bullet impact sounds (TdPhysicalMaterialImpactSounds: A_Effects_Bullet_Impacts).
        for (const PhysicalMaterialInfo& pm : active_scene.physical_materials) {
            if (!pm.impact_sound.empty() && !audio.has_cue(pm.impact_sound)) audio.load_cue_bank(game_root, pm.impact_sound_package);
        }
        if (std::getenv("ME_IMPACT_DEBUG")) {
            size_t named = 0, found = 0;
            for (const PhysicalMaterialInfo& pm : active_scene.physical_materials) {
                if (pm.impact_sound.empty()) continue;
                ++named;
                if (audio.has_cue(pm.impact_sound)) {
                    ++found;
                } else {
                    std::cout << "[Impact] no cue " << pm.impact_sound_package << "." << pm.impact_sound << " for " << pm.path << std::endl;
                }
            }
            std::cout << "[Impact] impact sounds: the cues of " << found << " of the " << named << " physical materials that name one are loaded"
                      << std::endl;
        }
        pending_level_loaded_audio = true;
        was_vo_playing = false;

        // A checkpoint other than the chapter's first: the player stands there, with its sublevels.
        if (!checkpoint_name.empty()) {
            for (size_t c = 0; c < active_scene.checkpoint_infos.size(); ++c) {
                const LevelCheckpointInfo& cp = active_scene.checkpoint_infos[c];
                if (lower(cp.checkpoint_name) != lower(checkpoint_name)) continue;
                stream_level_to_checkpoint(game_root, active_scene, static_cast<int>(c));
                controller.reset(cp.location + Vec3(0.0f, 0.0f, 35.0f), cp.rotation.to_degrees().y);
                controller.get_telemetry().active_checkpoint = static_cast<int>(c);
                controller.get_telemetry().active_checkpoint_name = cp.checkpoint_name;
                break;
            }
        }

        if (defer_begin_play) {
            level_play_pending = true;
            return true;
        }

        // The chapter's loading movie (DefaultEngine.ini [LoadMovies]); the level begins play when
        // it ends. Without one the level begins at once.
        if (max_frames == 0 && play_intro) {
            const std::string intro_movie = load_movie_for(map_file);
            if (!intro_movie.empty() && cutscene_player.play_bink_movie(intro_movie, /*chain_in_engine=*/false)) {
                level_play_pending = true;
                return true;  // begin_level_play() follows the movie
            }
        }
        begin_level_play();
        return true;
    };

    // ME_START_CHECKPOINT="Name": play starts at that checkpoint of the level instead of its first (a
    // test aid, with --level: the sublevels the checkpoint streams in are the ones whose script runs).
    const char* start_checkpoint_env = std::getenv("ME_START_CHECKPOINT");
    if (!load_chapter_or_level(current_chapter_idx, custom_level, /*play_intro=*/!start_in_main_menu,
                               start_checkpoint_env ? std::string(start_checkpoint_env) : std::string(),
                               /*defer_begin_play=*/start_in_main_menu)) {
        std::cerr << "[Game ERROR] No playable level (check --game-root / --level)." << std::endl;
        if (game_controller) SDL_GameControllerClose(game_controller);
        destroy_surface();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    auto set_menu_active = [&](bool open) {
        renderer.set_menu_open(open);
        audio.set_menu_music(open);
        SDL_SetRelativeMouseMode(open ? SDL_FALSE : SDL_TRUE);
        SDL_ShowCursor(open ? SDL_ENABLE : SDL_DISABLE);
        if (!open && level_play_pending && !cutscene_player.is_playing()) {
            begin_level_play();
        }
    };

    auto to_menu_coords = [&](int mx, int my, float& ux, float& uy) {
        SDL_GetWindowSize(window, &win_w, &win_h);
        ux = (static_cast<float>(mx) / static_cast<float>(std::max(1, win_w))) * 1280.0f;
        uy = (static_cast<float>(my) / static_cast<float>(std::max(1, win_h))) * 720.0f;
    };

    set_menu_active(start_in_main_menu);
    ensure_dir("screenshots");

    // The front end: "Press Any Key", then the main menu, as retail has them
    // (docs/MAIN_MENU_SYSTEM_RE.md). me::fe::Frontend is the state machine and
    // me::fe::SoftRenderer draws its frames on the CPU, at the 1280x720 the scenes were authored
    // for; the Metal renderer shows the result full screen. The screens behind the sub-buttons
    // (docs/SUB_MENUS_RE.md) are part of it; it reports what to do through take_action().
    constexpr int kFrontendW = 1280;
    constexpr int kFrontendH = 720;
    std::unique_ptr<fe::Frontend> frontend;
    std::unique_ptr<fe::SoftRenderer> frontend_renderer;
    std::vector<uint8_t> frontend_rgba;
    bool frontend_active = false;
    {
        frontend = std::make_unique<fe::Frontend>();
        std::string frontend_error;
        if (frontend->init(game_root, kFrontendW, kFrontendH, frontend_error, start_in_main_menu)) {
            frontend_renderer = std::make_unique<fe::SoftRenderer>(frontend->assets());
            if (start_in_main_menu) {
                frontend_active = true;
                renderer.set_menu_open(false);
                audio.set_menu_music(true);
                SDL_SetRelativeMouseMode(SDL_FALSE);
                SDL_ShowCursor(SDL_ENABLE);
            }
        } else {
            std::cerr << "[Frontend] " << frontend_error << " - falling back to the chapter-select overlay." << std::endl;
            frontend.reset();
        }
    }
    auto leave_frontend = [&]() {
        frontend_active = false;
        renderer.set_frontend_frame(nullptr, 0, 0);
    };

    // Interactive Loop
    bool running = true;
    bool fade_intro_before = false;
    float fade_last_sim_time = 1.0e30f;
    std::string fade_map;
    int frame_counter = 0;
    auto last_time = std::chrono::high_resolution_clock::now();

    const auto trace_start = std::chrono::steady_clock::now();
    auto write_trace = [&](const PlayerTelemetry& t, bool in_frontend) {
        if (!trace_file.is_open()) return;
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - trace_start).count();
        const char* cutscene = cutscene_player.get_mode() == ECutsceneMode::BinkVideo ? "movie"
                               : cutscene_player.get_mode() == ECutsceneMode::InEngineMatinee ? "intro" : "none";
        trace_file << std::fixed << std::setprecision(6) << "{\"type\":\"sample\",\"frame\":" << frame_counter
                   << ",\"t\":" << now << std::setprecision(3)
                   << ",\"x\":" << t.position.x << ",\"y\":" << t.position.y << ",\"z\":" << t.position.z + t.eye_height
                   << ",\"yaw\":" << t.yaw_deg << ",\"pitch\":" << t.pitch_deg << ",\"roll\":" << t.camera_roll_deg
                   << ",\"fx\":" << t.position.x << ",\"fy\":" << t.position.y << ",\"fz\":" << t.position.z
                   << ",\"move_name\":\"" << move_state_name(t.move_state) << "\""
                   << ",\"cutscene\":\"" << cutscene << "\""
                   << ",\"fade\":" << t.fade_amount;
        if (t.intro_active) {
            trace_file << ",\"intro\":\"" << t.intro_anim_name << "\",\"intro_t\":" << t.intro_anim_time;
            if (cutscene_player.is_level_intro()) {
                const Vec3 root = cutscene_player.intro_root_pos();
                trace_file << ",\"rx\":" << root.x << ",\"ry\":" << root.y << ",\"rz\":" << root.z;
            }
        }
        {
            // The camera the frame is drawn from (the first-person tree's eye in play).
            Vec3 cam;
            Rotator cam_rot;
            renderer.player_camera(t, cam, cam_rot);
            trace_file << ",\"cx\":" << cam.x << ",\"cy\":" << cam.y << ",\"cz\":" << cam.z;
        }
        if (in_frontend) trace_file << ",\"frontend\":true";
        if (renderer.is_menu_open() || (frontend && frontend->is_pause_open())) trace_file << ",\"menu\":true";
        trace_file << "}\n";
        for (const AudioEngine::PlayEvent& e : audio.take_play_log()) {
            trace_file << std::setprecision(6) << "{\"type\":\"sound\",\"frame\":" << frame_counter << ",\"t\":" << now
                       << ",\"cue\":\"" << e.name << "\",\"kind\":\"" << e.kind << "\",\"start\":true"
                       << std::setprecision(3) << ",\"dur\":" << e.duration << "}\n";
        }
        trace_file << std::defaultfloat << std::setprecision(6);
    };

    EMovement prev_state = EMovement::MOVE_Walking;
    int prev_checkpoint = 0;
    int intro_handover_frames = 0;  // frames since a cutscene handed the player over
    bool prev_use_held = false;     // the Use key last frame, for its press edge (SeqEvent_TdUsed)
    bool level_intro_running = false;
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
    std::cout << "  ESC: Pause Menu (TdSPPause / TdTutorialPause)\n" << std::endl;

    while (running) {
        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - last_time).count();
        last_time = now;
        if (dt > 0.1f) dt = 0.1f; // clamp hitch spikes

        // While the front end (main menu or in-game pause menu) is up it owns the frame:
        // its own events, update and picture.
        if (frontend_active || (frontend && frontend->is_pause_open())) {
            SDL_Event fev;
            while (SDL_PollEvent(&fev)) {
                if (fev.type == SDL_QUIT) {
                    running = false;
                } else if (fev.type == SDL_WINDOWEVENT) {
                    if (fev.window.event == SDL_WINDOWEVENT_RESIZED || fev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                        get_drawable_size(&drawable_w, &drawable_h);
                        renderer.resize(drawable_w, drawable_h);
                    }
                } else if ((fev.type == SDL_KEYDOWN && fev.key.repeat == 0) || fev.type == SDL_KEYUP) {
                    if (fev.type == SDL_KEYDOWN && fev.key.keysym.sym == SDLK_F12 &&
                        frontend->scene_name() != "TdKeyMappings") {
                        auto t = std::time(nullptr);
                        std::ostringstream ss;
                        ss << "screenshots/screenshot_" << std::put_time(std::localtime(&t), "%Y%m%d_%H%M%S") << ".png";
                        renderer.save_screenshot_png(ss.str());
                        continue;
                    }
                    fe::Key key = fe::Key::Other;
                    switch (fev.key.keysym.sym) {
                        case SDLK_LEFT:     key = fe::Key::Left; break;
                        case SDLK_RIGHT:    key = fe::Key::Right; break;
                        case SDLK_UP:       key = fe::Key::Up; break;
                        case SDLK_DOWN:     key = fe::Key::Down; break;
                        case SDLK_RETURN:
                        case SDLK_KP_ENTER:
                        case SDLK_SPACE:    key = fe::Key::Accept; break;
                        case SDLK_ESCAPE:   key = fe::Key::Escape; break;
                        default: break;
                    }
                    // The key by the engine's name as well: CONTROLS binds keys by it, and tells the
                    // space bar from Enter.
                    const std::string key_name = frontend_key_name(fev.key.keysym.sym);
                    if (fev.type == SDL_KEYDOWN) frontend->key_down(key, key_name);
                    else frontend->key_up(key, key_name);
                } else if (fev.type == SDL_CONTROLLERBUTTONDOWN || fev.type == SDL_CONTROLLERBUTTONUP) {
                    fe::Key key = fe::Key::Other;
                    switch (fev.cbutton.button) {
                        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  key = fe::Key::Left; break;
                        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: key = fe::Key::Right; break;
                        case SDL_CONTROLLER_BUTTON_DPAD_UP:    key = fe::Key::Up; break;
                        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  key = fe::Key::Down; break;
                        case SDL_CONTROLLER_BUTTON_A:
                        case SDL_CONTROLLER_BUTTON_START:      key = fe::Key::Accept; break;
                        case SDL_CONTROLLER_BUTTON_B:          key = fe::Key::Escape; break;
                        case SDL_CONTROLLER_BUTTON_X:          key = fe::Key::Reset; break;
                        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  key = fe::Key::PrevPage; break;
                        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: key = fe::Key::NextPage; break;
                        default: break;
                    }
                    if (fev.type == SDL_CONTROLLERBUTTONDOWN) frontend->key_down(key);
                    else frontend->key_up(key);
                } else if (fev.type == SDL_MOUSEMOTION ||
                           (fev.type == SDL_MOUSEBUTTONDOWN && fev.button.button == SDL_BUTTON_LEFT)) {
                    // Window points to the front end's frame, which is aspect-fitted in the window.
                    SDL_GetWindowSize(window, &win_w, &win_h);
                    const float fit = std::min(static_cast<float>(std::max(1, win_w)) / static_cast<float>(kFrontendW),
                                               static_cast<float>(std::max(1, win_h)) / static_cast<float>(kFrontendH));
                    const float off_x = (static_cast<float>(win_w) - static_cast<float>(kFrontendW) * fit) * 0.5f;
                    const float off_y = (static_cast<float>(win_h) - static_cast<float>(kFrontendH) * fit) * 0.5f;
                    const bool motion = fev.type == SDL_MOUSEMOTION;
                    const float fx = (static_cast<float>(motion ? fev.motion.x : fev.button.x) - off_x) / fit;
                    const float fy = (static_cast<float>(motion ? fev.motion.y : fev.button.y) - off_y) / fit;
                    if (motion) frontend->mouse_move(fx, fy);
                    else frontend->mouse_click(fx, fy);
                } else if (fev.type == SDL_MOUSEBUTTONUP && fev.button.button != SDL_BUTTON_LEFT) {
                    // The other mouse buttons are keys to CONTROLS: MOUSE 2, MOUSE 3, MOUSE 4.
                    const char* button = fev.button.button == SDL_BUTTON_RIGHT ? "RightMouseButton"
                                       : fev.button.button == SDL_BUTTON_MIDDLE ? "MiddleMouseButton" : "ThumbMouseButton";
                    frontend->key_up(fe::Key::Other, button);
                } else if (fev.type == SDL_MOUSEWHEEL && fev.wheel.y != 0) {
                    frontend->key_up(fe::Key::Other, fev.wheel.y > 0 ? "MouseScrollUp" : "MouseScrollDown");
                }
            }

            frontend->update(dt);
            for (const std::string& cue : frontend->take_sounds()) {
                // The skin's UI sound cues, all in Audio/A_HUD.upk.
                if (cue == "Music") audio.set_menu_music(true);
                else if (cue == "TabChangeRight" || cue == "TabChangeLeft") audio.play_sound("Tab_Change");
                else if (cue == "NavigateUp" || cue == "NavigateDown" || cue == "SliderIncrement" || cue == "SliderDecrement" ||
                         cue == "ListUp" || cue == "ListDown") audio.play_sound("D-Pad");
                else if (cue == "Accept") audio.play_sound("A_Pos");
                else if (cue == "Cancel") audio.play_sound("B_Neg");
            }

            std::string action = frontend->take_action();
            // SPEED RUN starts a chapter at its first checkpoint ("SpeedRun edge_p?LoadCheckpoint=Edge_Start"),
            // TIME TRIAL a course map of its own ("TimeTrial tt_TutorialA01_p"). The race itself (the
            // clock, the checkpoints, the ghost) is not in the port: the map is loaded to be run.
            if (action.rfind("SpeedRun ", 0) == 0 || action.rfind("TimeTrial ", 0) == 0) {
                std::string url = action.substr(action.find(' ') + 1);
                std::string checkpoint;
                const size_t query = url.find('?');
                if (query != std::string::npos) {
                    const size_t at = url.find("LoadCheckpoint=", query);
                    if (at != std::string::npos) checkpoint = url.substr(at + 15, url.find('?', at) == std::string::npos ? std::string::npos : url.find('?', at) - at - 15);
                    url.erase(query);
                }
                action = "StartLevel " + url + (checkpoint.empty() ? std::string() : " " + checkpoint);
            }
            if (action == "Quit") {
                running = false;
            } else if (action == "Continue" || action == "Resume") {
                // CONTINUE GAME or RESUME from Pause Menu: back into the active chapter.
                leave_frontend();
                set_menu_active(false);
            } else if (action == "QuitToMainMenu") {
                // EXIT TO MAIN MENU from Pause Menu (TdUIScene_Pause.OnLeaveGameMessageBoxAction).
                cutscene_player.stop();
                controller.get_telemetry().intro_active = false;
                frontend->open_main_menu();
                frontend_active = true;
                renderer.set_menu_open(false);
                audio.set_menu_music(true);
                SDL_SetRelativeMouseMode(SDL_FALSE);
                SDL_ShowCursor(SDL_ENABLE);
            } else if (action == "SkipTutorial") {
                // SKIP TUTORIAL from TdTutorialPause (TdSPTutorialGame.OnLevelCompleted -> edge_p?Edge_Start).
                leave_frontend();
                set_menu_active(false);
                current_chapter_idx = 1;
                renderer.set_selected_chapter(current_chapter_idx);
                load_chapter_or_level(current_chapter_idx, "", /*play_intro=*/true, "Edge_Start");
            } else if (action == "NewGame") {
                // NEW GAME: the Prologue, with its opening.
                current_chapter_idx = 1;
                renderer.set_selected_chapter(current_chapter_idx);
                load_chapter_or_level(current_chapter_idx, "", /*play_intro=*/true);
                leave_frontend();
                set_menu_active(false);
            } else if (action.rfind("StartLevel ", 0) == 0) {
                // PLAY CHAPTER: "StartLevel <map> [checkpoint]", the map by its file name ("edge_p").
                std::string map_name = action.substr(11);
                std::string checkpoint_name;
                const size_t space = map_name.find(' ');
                if (space != std::string::npos) {
                    checkpoint_name = map_name.substr(space + 1);
                    map_name.erase(space);
                }
                auto lower = [](std::string v) {
                    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    return v;
                };
                static const char* const kChapterFiles[10] = {"tutorial_p", "edge_p", "stormdrain_p", "cranes_p", "subway_p",
                                                              "mall_p", "factory_p", "boat_p", "convoy_p", "scraper_p"};
                int chapter = -1;
                for (int c = 0; c < 10; ++c) {
                    if (lower(map_name) == kChapterFiles[c]) chapter = c;
                }
                // A chapter of the list starts with its opening; any other map (Flight is the second
                // half of the Prologue's folder) is loaded by path. A checkpoint other than the first
                // is where the level begins play (its sublevels in, the player standing there).
                const std::string map_path = chapter >= 0 ? std::string() : frontend->map_path(map_name);
                if ((chapter >= 0 || !map_path.empty()) &&
                    load_chapter_or_level(chapter >= 0 ? chapter : current_chapter_idx, map_path, /*play_intro=*/true, checkpoint_name)) {
                    if (chapter >= 0) {
                        current_chapter_idx = chapter;
                        renderer.set_selected_chapter(current_chapter_idx);
                    }
                    leave_frontend();
                    set_menu_active(false);
                }
            } else if (!frontend_active && !(frontend && frontend->is_pause_open())) {
                leave_frontend();
                set_menu_active(false);
            }

            if (frontend_active) {
                frontend_renderer->render(frontend->frame(), frontend_rgba);
                renderer.set_frontend_frame(frontend_rgba.data(), kFrontendW, kFrontendH, /*overlay=*/false, 0.0f);
            } else if (frontend && frontend->is_pause_open()) {
                frontend_renderer->render_overlay(frontend->frame(), frontend_rgba);
                renderer.set_frontend_frame(frontend_rgba.data(), kFrontendW, kFrontendH, /*overlay=*/true, frontend->pause_saturation());
            }
            audio.update(dt, Vec3(0.0f, 0.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, 1.0f), 0.0f, false);
            renderer.render_frame(active_scene, controller.get_telemetry());
            write_trace(controller.get_telemetry(), /*in_frontend=*/frontend_active);

            ++frame_counter;
            if (max_frames > 0 && frame_counter >= max_frames) {
                std::cout << "[Game] Reached max-frames limit (" << max_frames << "). Exiting cleanly." << std::endl;
                running = false;
            }
            continue;
        }

        InputFrame input{};
        // By default sprint is active for momentum acceleration matching retail Mirror's Edge
        input.sprint = true;

        auto activate_menu_selection = [&]() {
            const int tab = renderer.selected_menu_tab();
            if (tab == 0) {
                // STORY: Launch selected chapter with Bink / 3D opening cutscene
                load_chapter_or_level(current_chapter_idx, "", /*play_intro=*/true);
                set_menu_active(false);
            } else if (tab == 1) {
                // RACE (SPEED RUN): Launch selected course directly into timed run (no cutscene)
                load_chapter_or_level(current_chapter_idx, "", /*play_intro=*/false);
                set_menu_active(false);
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
                    get_drawable_size(&drawable_w, &drawable_h);
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

        auto open_ingame_pause_menu = [&]() {
            if (frontend) {
                static const char* const kChapterMaps[10] = {
                    "Maps/SP00/Tutorial_p.me1", "Maps/SP01/Edge_p.me1", "Maps/SP02/Stormdrain_p.me1",
                    "Maps/SP03/Cranes_p.me1",   "Maps/SP04/Subway_p.me1", "Maps/SP05/Mall_p.me1",
                    "Maps/SP06/Factory_p.me1",  "Maps/SP07/Boat_p.me1",   "Maps/SP08/Convoy_p.me1",
                    "Maps/SP09/Scraper_p.me1"
                };
                const std::string map_for_pause = active_scene.map_name.empty()
                    ? std::string(kChapterMaps[std::clamp(current_chapter_idx, 0, 9)])
                    : active_scene.map_name;
                frontend->open_pause_menu(map_for_pause);
                SDL_SetRelativeMouseMode(SDL_FALSE);
                SDL_ShowCursor(SDL_ENABLE);
            } else {
                set_menu_active(true);
            }
        };

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                running = false;
            } else if (ev.type == SDL_WINDOWEVENT) {
                if (ev.window.event == SDL_WINDOWEVENT_RESIZED || ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    get_drawable_size(&drawable_w, &drawable_h);
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
            } else if (ev.type == SDL_CONTROLLERBUTTONDOWN && ev.cbutton.button == SDL_CONTROLLER_BUTTON_START) {
                if (renderer.is_menu_open()) {
                    set_menu_active(false);
                } else if (!splash_hint_open && !cutscene_player.is_playing()) {
                    open_ingame_pause_menu();
                }
            } else if (ev.type == SDL_KEYDOWN && !ev.key.repeat) {
                SDL_Keycode key = ev.key.keysym.sym;
                if (key == SDLK_ESCAPE) {
                    if (renderer.is_menu_open()) {
                        set_menu_active(false);
                    } else if (splash_hint_open) {
                        splash_hint_open = false;
                        controller.get_telemetry().splash_hint_text.clear();
                        script.accept_message();
                    } else if (cutscene_player.is_playing()) {
                        if (cutscene_player.get_mode() == ECutsceneMode::BinkVideo && level_play_pending) {
                            cutscene_player.stop();
                            begin_level_play();
                        } else if (script.playing_cutscene() >= 0) {
                            std::cout << "[Script] skip asked for (Escape)" << std::endl;
                            script.skip_cutscene();  // SkipCutscene: only if the Matinee allows it
                        } else {
                            abandon_cutscene();
                        }
                    } else {
                        open_ingame_pause_menu();
                    }
                } else if (splash_hint_open && (key == SDLK_SPACE || key == SDLK_RETURN)) {
                    suppress_space_until_release = true;
                    splash_hint_open = false;
                    controller.get_telemetry().splash_hint_text.clear();
                    script.accept_message();
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
                    if (cutscene_player.get_mode() == ECutsceneMode::BinkVideo && level_play_pending) {
                        // Skipping the chapter's loading movie: the level begins play, as it would
                        // when the movie ends.
                        cutscene_player.stop();
                        begin_level_play();
                    } else if (script.playing_cutscene() >= 0) {
                        // SpaceBar is "GBA_Jump | SkipCutscene" (DefaultInput.ini): the Matinee stops
                        // if it is skippable and no SeqAct_TdDisablePlayerInput forbade it, and its
                        // Aborted output runs.
                        std::cout << "[Script] skip asked for (" << SDL_GetKeyName(key) << ")" << std::endl;
                        script.skip_cutscene();
                    } else if (cutscene_player.get_mode() == ECutsceneMode::BinkVideo && active_scene.level_intro.valid) {
                        controller.reset(active_scene.player_spawn_pos, active_scene.player_spawn_yaw);
                        cutscene_player.play_in_engine_intro(active_scene, controller.get_telemetry(), 3.2f);
                    } else if (const LevelIntroSequence* seq = cutscene_player.active_sequence(active_scene); seq && !seq->skippable) {
                        // A level intro with bIsSkippable off (the training area's pan): SkipCutscene
                        // does nothing to it.
                        std::cout << "[Cutscene] '" << seq->seq_name << "' is not skippable" << std::endl;
                    } else {
                        abandon_cutscene();
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
                } else if (key == SDLK_r && script.valid()) {
                    // Load last checkpoint (TdPlayerController.CanLoadFromLastCheckpoint): the
                    // level starts over at the active checkpoint, its script with it.
                    if (!script.load_from_checkpoint_disabled()) {
                        cutscene_player.stop();
                        controller.get_telemetry().intro_active = false;
                        if (const ScriptActor* cp = script.active_checkpoint()) {
                            controller.reset(cp->location + Vec3(0.0f, 0.0f, 35.0f), cp->yaw_deg);
                        }
                        controller.get_telemetry().tutorial_text.clear();
                        controller.get_telemetry().sign_text.clear();
                        restore_script_actors(active_scene);
                        script.reload_checkpoint();
                    }
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
                    load_chapter_or_level(sel, "", /*play_intro=*/true);
                    set_menu_active(false);
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
        if (state[SDL_SCANCODE_C] || state[SDL_SCANCODE_LCTRL] || state[SDL_SCANCODE_RCTRL] ||
            state[SDL_SCANCODE_LSHIFT] || state[SDL_SCANCODE_RSHIFT]) input.crouch = true;
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

        // SeqAct_TdDisablePlayerInput: the script holds the player's movement and look (a cutscene
        // about to start, an elevator ride) until SeqAct_TdEnablePlayerInput.
        const bool use_edge = input.use && !prev_use_held;
        prev_use_held = input.use;
        if (script.valid() && script.input_move_disabled()) {
            input.forward = 0.0f;
            input.strafe = 0.0f;
            input.jump = false;
            input.crouch = false;
            input.turn_180 = false;
            input.melee = false;
            input.fire = false;
            input.disarm = false;
            input.use = false;
        }
        if (script.valid() && script.input_look_disabled()) {
            input.look_yaw_delta = 0.0f;
            input.look_pitch_delta = 0.0f;
        }

        // Advance simulation or active cutscene step (paused while Main Menu is open or a hint card is up)
        Vec3 pre_vel = controller.get_velocity();
        if (!renderer.is_menu_open() && !splash_hint_open) {
            if (cutscene_player.get_mode() != ECutsceneMode::BinkVideo && pending_level_loaded_audio) {
                pending_level_loaded_audio = false;
                audio.play_level_loaded_cues();
            }
            if (cutscene_player.is_playing()) {
                const bool was_level_cutscene = cutscene_player.is_level_intro();
                cutscene_player.update(dt, active_scene, controller.get_telemetry());
                // The doors the cutscene goes through swing as its Matinee has them.
                if (const LevelIntroSequence* seq = cutscene_player.active_sequence(active_scene)) {
                    CutscenePlayer::pose_sequence_doors(active_scene, *seq, cutscene_player.get_current_time());
                }
                // The cutscene's own sounds: its animation's footsteps, clothing and foley, and the
                // voice lines and effects behind its Matinee's event keys.
                // With the level's script running, the Kismet-side sounds (the voice lines and
                // effects behind the event keys, the sound tracks) are the script's to play; the
                // baked list supplies the animation's notifies alone.
                const bool script_owns_sounds = script.valid() && script.playing_cutscene() >= 0;
                for (const IntroSoundEvent& ev : cutscene_player.take_intro_sounds()) {
                    if (script_owns_sounds && !ev.from_notify) continue;
                    if (ev.footstep > 0) {
                        audio.play_footstep_number(ESurfaceMaterial::Concrete, ev.footstep);
                    } else if (!audio.play_cue(ev.cue, ev.voice)) {
                        std::cout << "[Intro] sound not loaded: " << ev.cue << std::endl;
                    }
                }
                if (!cutscene_player.is_playing()) {
                    if (level_play_pending) {
                        // The loading movie ended: the level begins play.
                        begin_level_play();
                    } else if (was_level_cutscene) {
                        // The Matinee ran out: the player stands where its animation left her.
                        const LevelIntroSequence* seq = nullptr;
                        if (script.valid() && script.playing_cutscene() >= 0 &&
                            static_cast<size_t>(script.playing_cutscene()) < active_scene.cutscenes.size()) {
                            seq = &active_scene.cutscenes[static_cast<size_t>(script.playing_cutscene())];
                        }
                        if (seq) {
                            hand_over(*seq);
                        } else {
                            controller.reset(active_scene.player_spawn_pos, active_scene.player_spawn_yaw);
                        }
                        if (script.valid()) script.cutscene_finished();
                        intro_handover_frames = 30;
                    } else {
                        controller.reset(active_scene.player_spawn_pos, active_scene.player_spawn_yaw);
                        intro_handover_frames = 30;
                    }
                }
            } else {
                // ME_WARP="x,y,z,yaw" puts the player there (feet) as play starts: a test aid, for
                // looking at one place in a level without playing to it.
                static bool warp_done = false;
                if (!warp_done) {
                    warp_done = true;
                    float wx = 0.0f, wy = 0.0f, wz = 0.0f, wyaw = 0.0f;
                    const char* warp = std::getenv("ME_WARP");
                    if (warp && std::sscanf(warp, "%f,%f,%f,%f", &wx, &wy, &wz, &wyaw) == 4) {
                        controller.reset(Vec3(wx, wy, wz), wyaw);
                    }
                }
                const bool cinematic_lock = cutscene_player.is_playing() || into_cutscene_pending ||
                                            script.cinematic_mode() || script.input_move_disabled();
                controller.get_telemetry().intro_active = cinematic_lock;
                controller.get_telemetry().intro_camera_only = false;  // only a playing cutscene is seen through a camera
                if (into_cutscene_pending) {
                    // TdMove_IntoCutscene: the pawn glides onto its mark over 0.4 s and faces it.
                    into_cutscene_timer += dt;
                    const float u = std::clamp(into_cutscene_timer / 0.4f, 0.0f, 1.0f);
                    const Vec3 from = controller.get_telemetry().position;
                    const Vec3 to = into_cutscene_target + Vec3(0.0f, 0.0f, 2.0f);
                    controller.set_position(from + (to - from) * u);
                    controller.set_velocity(Vec3(0.0f, 0.0f, 0.0f));
                    if (u >= 1.0f) {
                        controller.reset(to, into_cutscene_yaw);
                        into_cutscene_pending = false;
                        script.into_cutscene_finished();
                    }
                } else {
                    controller.step(input, dt, active_scene);
                }
                // TdPlayerPawn.CalcCamera: the move checks the camera against the walls with the
                // eye the first-person tree has for this frame.
                Vec3 eye;
                Rotator eye_rot;
                renderer.player_camera(controller.get_telemetry(), eye, eye_rot);
                const Vec3 off = controller.get_telemetry().camera_mesh_offset;
                controller.update_camera_collision(eye - Vec3(off.x, off.y, 0.0f), dt, active_scene);
            }

            // The level's Kismet, against the player as she is after this frame's move. It runs
            // through its own cutscenes (it owns their Matinees) but not under the loading movie.
            if (script.valid() && !level_play_pending) {
                PlayerTelemetry& t = controller.get_telemetry();
                if (t.respawned) {
                    // A death reloads the level at the checkpoint (TdSPStoryGame.ResetLevel).
                    t.respawned = false;
                    t.tutorial_text.clear();
                    t.sign_text.clear();
                    cutscene_player.stop();
                    t.intro_active = false;
                    restore_script_actors(active_scene);  // the panes are whole again
                    script.reload_checkpoint();
                }
                ScriptPlayerState ps;
                ps.position = cutscene_player.is_level_intro() ? cutscene_player.intro_root_pos() : t.position;
                ps.eye = t.position + Vec3(0.0f, 0.0f, t.eye_height);
                ps.view_dir = Rotator::from_degrees(t.pitch_deg, t.yaw_deg, 0.0f).forward();
                ps.fov_deg = t.fov_deg;
                ps.speed = t.speed_2d;
                ps.use_pressed = use_edge && !cutscene_player.is_playing();
                ps.dead = t.falling_to_death || t.fall_death_impact || t.health <= 0.0f;
                ps.in_cutscene = cutscene_player.is_playing();
                // ME_SCRIPT_EVENT="Name@seconds[,Name@seconds..]" fires remote events at those
                // script times and ME_SCRIPT_SKIP="seconds" skips the cutscene then: test aids,
                // for driving a chapter's chains (its end, its skips) without playing to them.
                {
                    static const char* ev_env = std::getenv("ME_SCRIPT_EVENT");
                    static const char* skip_env = std::getenv("ME_SCRIPT_SKIP");
                    static std::vector<std::pair<std::string, float>> test_events;
                    static bool parsed = false, skip_done = false;
                    if (ev_env && !parsed) {
                        parsed = true;
                        std::string all = ev_env;
                        size_t pos = 0;
                        while (pos <= all.size()) {
                            const size_t comma = all.find(',', pos);
                            const std::string spec = all.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                            const size_t at = spec.find('@');
                            if (!spec.empty()) {
                                test_events.emplace_back(spec.substr(0, at), at == std::string::npos ? 0.0f : std::strtof(spec.c_str() + at + 1, nullptr));
                            }
                            if (comma == std::string::npos) break;
                            pos = comma + 1;
                        }
                    }
                    for (size_t k = 0; k < test_events.size();) {
                        if (script.time() >= test_events[k].second) {
                            std::cout << "[Script] test: firing remote event '" << test_events[k].first << "'" << std::endl;
                            script.fire_remote_event(test_events[k].first);
                            test_events.erase(test_events.begin() + static_cast<std::ptrdiff_t>(k));
                        } else {
                            ++k;
                        }
                    }
                    if (skip_env && !skip_done && script.time() >= std::strtof(skip_env, nullptr) && script.playing_cutscene() >= 0) {
                        skip_done = true;
                        std::cout << "[Script] test: skipping the cutscene" << std::endl;
                        script.skip_cutscene();
                    }
                }
                deliver_actor_damage(script, active_scene);  // the bullets' and the barges', for its damage events
                script.update(dt, ps);
                if (t.supers_time_left > 0.0f) t.supers_time_left -= dt;
                if (t.sign_time_left > 0.0f) t.sign_time_left -= dt;
            } else {
                controller.get_telemetry().respawned = false;
            }
            {
                const LevelIntroSequence* playing = cutscene_player.active_sequence(active_scene);
                controller.get_telemetry().skip_prompt = playing != nullptr && playing->skippable &&
                                                         !(script.valid() && script.skip_disabled());
                const float cs_time = controller.get_telemetry().intro_anim_time;
                for (auto& bot : active_scene.enemies) {
                    if (!bot.is_story_npc) continue;
                    if (bot.cutscene_only) {
                        bot.alive = (playing != nullptr && !bot.cutscene_pkg_path.empty() &&
                                     playing->package_path == bot.cutscene_pkg_path &&
                                     cs_time >= bot.cutscene_start_sec);
                        if (bot.alive) {
                            bot.anim_timer = std::max(0.0f, cs_time - bot.cutscene_start_sec);
                        }
                    } else {
                        bool replaced_by_cs = false;
                        for (const auto& cs_bot : active_scene.enemies) {
                            if (cs_bot.is_story_npc && cs_bot.cutscene_only && cs_bot.archetype == bot.archetype &&
                                playing != nullptr && cs_bot.cutscene_pkg_path == playing->package_path) {
                                replaced_by_cs = true;
                                break;
                            }
                        }
                        bot.alive = !replaced_by_cs;
                    }
                }
            }

            // The subtitle slot: with a script, retail's text only (voice-over subtitles); the
            // controller's own notes otherwise.
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
                    (!active_scene.subtitles.empty() && !active_scene.subtitles[cp].empty() && !active_scene.script)
                        ? active_scene.subtitles[cp]
                        : "";
            } else if (active_scene.script) {
                controller.get_telemetry().active_subtitle.clear();
            }

            // SeqAct_TdLevelCompleted: the next chapter, at its first checkpoint, behind its loading
            // movie. A frame later than the action, so the frame it fires in still draws.
            if (pending_transition.pending) {
                pending_transition.delay += dt;
                if (pending_transition.delay > 0.05f) {
                    const PendingTransition tr = pending_transition;
                    pending_transition = PendingTransition{};
                    if (lower(tr.level) == "tdmainmenu") {
                        // The Shard's end: back to the main menu (after the credits in retail).
                        std::cout << "[Script] chapter complete: to the main menu" << std::endl;
                        if (frontend) {
                            frontend->open_main_menu();
                            frontend_active = true;
                            renderer.set_menu_open(false);
                            audio.set_menu_music(true);
                            SDL_SetRelativeMouseMode(SDL_FALSE);
                            SDL_ShowCursor(SDL_ENABLE);
                        } else {
                            set_menu_active(true);
                        }
                    } else {
                        const std::string map_file = find_map_file(tr.level);
                        if (map_file.empty()) {
                            std::cout << "[Script] no map file for '" << tr.level << "'" << std::endl;
                        } else {
                            static const char* const kChapterFiles[10] = {"tutorial_p", "edge_p", "stormdrain_p", "cranes_p", "subway_p",
                                                                          "mall_p", "factory_p", "boat_p", "convoy_p", "scraper_p"};
                            for (int c = 0; c < 10; ++c) {
                                if (lower(tr.level) == kChapterFiles[c]) {
                                    current_chapter_idx = c;
                                    renderer.set_selected_chapter(c);
                                }
                            }
                            // The next chapter's own loading movie plays over the load; the
                            // checkpoint it names is where play begins (retail's "Start" is the
                            // chapter's first, which also runs its intro).
                            load_chapter_or_level(current_chapter_idx, map_file, /*play_intro=*/true, tr.checkpoint);
                        }
                    }
                }
            }
        }
        const auto& tel = controller.get_telemetry();

        // Live movement telemetry logging for run diagnostics
        static std::ofstream live_trace(temp_dir() + "/me_live_run_telemetry.jsonl", std::ios::out | std::ios::trunc);
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
                case EMovement::MOVE_MeleeWallrun: audio.play_effect(EAudioEffect::Vault); break;
                case EMovement::MOVE_Slide:
                case EMovement::MOVE_MeleeSlide: audio.play_effect(EAudioEffect::Slide); break;
                case EMovement::MOVE_SkillRoll: audio.play_effect(EAudioEffect::SkillRoll); break;
                case EMovement::MOVE_ZipLine: audio.play_effect(EAudioEffect::Zipline); break;
                case EMovement::MOVE_Snatch: audio.play_effect(EAudioEffect::Disarm); break;
                default: break;
            }
            prev_state = tel.move_state;
        }

        // Sounds the simulation started this step (animation notifies such as TdMove_Barge's, and the
        // barge doors' Kismet / matinee sound tracks): on the player in 2D, or 3D at the actor.
        for (const SimSoundEvent& ev : tel.sound_events) {
            const float vol = (ev.cue.find("FootStep") != std::string::npos) ? 0.75f : 1.0f;
            if (ev.at_pawn) {
                audio.play_sound(ev.cue, vol);
            } else {
                audio.play_sound_3d(ev.cue, ev.location, vol);
            }
        }
        controller.get_telemetry().sound_events.clear();

        if (tel.falling_to_death && !prev_falling_to_death) {
            if (tel.fall_death_impact) {
                // Direct lethal ground impact without prior freefall wind entry
                audio.play_effect(EAudioEffect::FallDeathImpact);
            } else if (tel.grounded) {
                // Combat / bullet death on ground: SetSoundMode(9) + Oral_Death.Death (NO freefall wind or Bodyfall splat)
                audio.set_sound_group_mode(ESoundGroupEffectMode::DeathGeneric);
                audio.play_sound("Death", 1.15f, 1.0f);
            } else {
                // Entering lethal freefall: SetSoundMode(6) + Death_Fall (Freefall_Loop + LOD wind)
                audio.play_effect(EAudioEffect::FallDeathScream);
            }
            if (script.valid()) script.player_died();  // SeqEvt_TdPlayerDeath
        } else if (tel.fall_death_impact && !prev_fall_death_impact) {
            // Hitting ground after freefall: cuts freefall wind + background audio & plays pure Death_Impact thud
            audio.play_effect(EAudioEffect::FallDeathImpact);
        }
        if (!tel.falling_to_death && !tel.fall_death_impact &&
            (prev_falling_to_death || prev_fall_death_impact)) {
            audio.stop_cue("Death_Fall");
            audio.stop_cue("Freefall_Loop");
            audio.stop_cue("LOD");
            audio.set_sound_group_mode(ESoundGroupEffectMode::Normal);
        }
        prev_falling_to_death = tel.falling_to_death;
        prev_fall_death_impact = tel.fall_death_impact;

        // What the intro's Matinee stops when it ends, whether it ran out or was skipped.
        const bool level_intro_now = cutscene_player.is_level_intro();
        if (level_intro_running && !level_intro_now && !script.valid()) {
            for (const std::string& cue : active_scene.level_intro.stop_cues) audio.stop_cue(cue);
            CutscenePlayer::pose_intro_doors(active_scene, 1.0e9f);
        }
        level_intro_running = level_intro_now;

        if (intro_handover_frames > 0 || cutscene_player.is_playing() || into_cutscene_pending) {
            // Standing where an intro/cutscene sets or hands over a checkpoint does not chime BagFound.
            if (intro_handover_frames > 0) --intro_handover_frames;
            prev_checkpoint = tel.active_checkpoint;
        }
        if (tel.active_checkpoint != prev_checkpoint) {
            audio.play_effect(EAudioEffect::CheckpointChime);
            prev_checkpoint = tel.active_checkpoint;
        }

        if (tel.weapon.fired_this_tick && !cutscene_player.is_playing()) {
            audio.play_effect(EAudioEffect::Gunshot);
        }

        // Footstep cadence (TdPhysicalMaterialFootSteps: Sneak / Walk / Run / Sprint)
        // (not during a cutscene: a level intro's footsteps are its animation's own notifies). Under the
        // barge the locomotion only steps while the move's animation slot is mostly blended out.
        const bool locomotion_feet = tel.move_state == EMovement::MOVE_Walking ||
                                     (tel.move_state == EMovement::MOVE_Barge && tel.barge_anim_weight < 0.5f);
        if (!renderer.is_menu_open() && !cutscene_player.is_playing() && tel.grounded && locomotion_feet &&
            tel.speed_2d > 40.0f) {
            footstep_timer += dt;
            float stride_time = std::clamp(150.0f / tel.speed_2d, 0.22f, 0.45f);
            if (footstep_timer >= stride_time) {
                audio.play_footstep(ESurfaceMaterial::Concrete, tel.speed_2d, input.crouch > 0.5f, 0.75f);
                footstep_timer = 0.0f;
            }
        } else {
            footstep_timer = 0.0f;
        }

        // Trigger helicopter rotor & FNMinimi gunfire audio cues (and always clear transient flags so a
        // helicopter firing right before a cutscene never loops FNMinimi_Fire during the cutscene)
        for (auto& heli : active_scene.helicopters) {
            if (heli.just_spawned) {
                heli.just_spawned = false;
                if (!cutscene_player.is_playing() && !into_cutscene_pending) {
                    audio.load_sound_bank(game_root, "A_Vehicle_Helicopter");
                    audio.play_cue("Helicopter.Helicopter", false, 0.85f);
                }
            }
            if (heli.just_fired) {
                heli.just_fired = false;
                if (!cutscene_player.is_playing() && !into_cutscene_pending) {
                    audio.play_sound_3d("FNMinimi_Fire", heli.position, 0.55f);
                }
            }
        }

        // Update 3D listener and dynamic audio stems
        audio.set_menu_music(renderer.is_menu_open());
        Vec3 ear = tel.position + Vec3(0, 0, tel.eye_height);
        Rotator ear_rot = Rotator::from_degrees(tel.pitch_deg, tel.yaw_deg, tel.camera_roll_deg);
        // A cutscene's motion is not the player's running: no speed-driven wind or breathing for it.
        audio.update(dt, ear, ear_rot.forward(), ear_rot.up(),
                     (cutscene_player.is_playing() || into_cutscene_pending) ? 0.0f : tel.speed_2d,
                     tel.reaction_active && !cutscene_player.is_playing());

        // The screen fade (TdHUD): in from white when the player starts or restarts, and whatever
        // the level intro's Kismet asks for on the way.
        {
            const LevelIntroSequence* now_seq = cutscene_player.active_sequence(active_scene);
            const bool intro_now = now_seq != nullptr &&
                                   (now_seq == &active_scene.level_intro ||
                                    (now_seq->interp_export_index_1 == active_scene.level_intro.interp_export_index_1 &&
                                     now_seq->interp_package == active_scene.level_intro.interp_package));
            const bool restarted = tel.sim_time < fade_last_sim_time && !fade_intro_before;
            const bool level_opened = active_scene.map_name != fade_map;
            if ((intro_now && !fade_intro_before) || restarted || level_opened) screen_fade.restart();
            controller.get_telemetry().exposure_reset = level_opened;
            const bool script_owns_fades = script.valid() && script.playing_cutscene() >= 0;
            for (const IntroFadeEvent& ev : cutscene_player.take_intro_fades()) {
                if (!script_owns_fades) screen_fade.apply(ev);
            }
            if (!renderer.is_menu_open() && !splash_hint_open) screen_fade.update(dt);
            fade_intro_before = intro_now;
            fade_last_sim_time = tel.sim_time;
            fade_map = active_scene.map_name;
            controller.get_telemetry().fade_amount = screen_fade.shown();
            // The chain's material effects (falling, health, reaction time, hits), from the pawn's state.
            if (restarted || level_opened) screen_effects.restart();
            if (!renderer.is_menu_open()) screen_effects.update(tel, dt, controller.get_telemetry().screen_effects);
            for (ScreenEffect& forced : screen_effects_from_environment()) {
                controller.get_telemetry().screen_effects.push_back(std::move(forced));
            }
            controller.get_telemetry().fade_color = screen_fade.color;
        }

        // Render frame
        renderer.render_frame(active_scene, tel);
        write_trace(tel, /*in_frontend=*/false);

        ++frame_counter;
        if (max_frames > 0 && frame_counter >= max_frames) {
            std::cout << "[Game] Reached max-frames limit (" << max_frames << "). Exiting cleanly." << std::endl;
            running = false;
        }
    }

    if (game_controller) SDL_GameControllerClose(game_controller);
    audio.shutdown();
    destroy_surface();
    SDL_DestroyWindow(window);
    SDL_Quit();

    std::cout << "[Game] Shutdown cleanly." << std::endl;
    return 0;
}

// -----------------------------------------------------------------------------
// Main Entrypoint & CLI Parsing
// -----------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    std::string game_root = me::default_game_root();
    bool verify_all = false;
    bool verify_script = false;  // the level script's stage of the oracle, alone
    std::string shots_map, shots_times, shots_dir;
    std::string handover_map;  // --handover-check <map>
    std::string script_json = "";
    int initial_chapter = 0;
    std::string custom_level = "";
    int max_frames = 0;
    bool start_in_main_menu = true;
    std::string trace_path;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--handover-check") {
            if (i + 1 < argc) handover_map = argv[++i];
        } else if (arg == "--intro-shots") {
            if (i + 3 < argc) {
                shots_map = argv[++i];
                shots_times = argv[++i];
                shots_dir = argv[++i];
            }
        } else if (arg == "--dump-shaders") {
            // The Metal Shading Language sources as the renderer compiles them, for checking them
            // with a compiler on a machine that has no game data (xcrun metal -c).
            const std::string dir = (i + 1 < argc) ? argv[++i] : ".";
            me::ensure_dir(dir);
            std::ofstream(dir + "/builtin.metal", std::ios::binary) << me::sun_shadow_msl() << me::kBuiltinShadersMSL;
            std::ofstream(dir + "/material_check.metal", std::ios::binary) << me::material_check_msl();
            std::cout << "Wrote builtin.metal and material_check.metal to " << dir << std::endl;
            return 0;
        } else if (arg == "--verify-script") {
            verify_script = true;
        } else if (arg == "--verify-all") {
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
        } else if (arg == "--trace") {
            if (i + 1 < argc) trace_path = argv[++i];
        } else if (arg == "--game-root") {
            if (i + 1 < argc) game_root = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Mirror's Edge Native " << me::platform_name() << " Engine\n\n"
                      << "Usage:\n"
                      << "  " << fs::path(argv[0]).filename().string() << " [options]\n\n"
                      << "Options:\n"
                      << "  --main-menu              Boot into the 3D City of Glass Main Menu (default)\n"
                      << "  --verify-all             Run deterministic headless oracle verification suite\n"
                      << "  --verify-script          Run only its level-script stage (glass, emitter factories, toggles)\n"
                      << "  --handover-check <map>   Print the camera around the end of the level's intro, headless\n"
                      << "  --intro-shots <map> <t,t,..> <dir>  Render the level's intro at those Matinee times, headless\n"
                      << "  --dump-shaders <dir>     Write the Metal shader sources, to check them with a compiler\n"
                      << "  --headless-oracle <file> Run script-based headless oracle\n"
                      << "  --test-replay <trace>    Replay physics trace headless\n"
                      << "  --chapter <0..9>         Start at specified campaign chapter\n"
                      << "  --level <path>           Load custom level package\n"
                      << "  --max-frames <N>         Exit after rendering N frames\n"
                      << "  --trace <file>           Write the camera and every sound played, one JSON line per frame\n"
                      << "  --game-root <dir>        Set retail game assets directory (default: " << me::default_game_root() << ")\n"
                      << "  --help, -h               Show this help message\n";
            return 0;
        }
    }

    if (!me::is_game_root(game_root)) {
        std::cerr << "[Game ERROR] No Mirror's Edge install at '" << game_root << "' (expected TdGame/CookedPC inside it).\n"
                  << "             Pass --game-root <dir> or set MEDGE_ME_INSTALL." << std::endl;
        return 1;
    }

    if (!shots_map.empty()) {
        return run_intro_shots(game_root, shots_map, shots_times, shots_dir);
    }
    if (!handover_map.empty()) {
        me::Renderer renderer;
        renderer.set_game_root(game_root);
        if (!renderer.init_headless(1280, 720)) return 1;
        return measure_intro_handover(renderer, game_root, handover_map, true).valid ? 0 : 1;
    }
    if (verify_script) {
        // ME_SCRIPT_SHOTS=<dir>: with pictures of the pane it breaks.
        const char* shots = std::getenv("ME_SCRIPT_SHOTS");
        if (!shots) return oracle_script_effects(game_root) ? 0 : 1;
        me::Renderer renderer;
        renderer.set_game_root(game_root);
        if (!renderer.init_headless(1280, 720)) return 1;
        me::ensure_dir(shots);
        return oracle_script_effects(game_root, &renderer, shots) ? 0 : 1;
    }
    if (verify_all) {
        return run_oracle_verification(game_root, script_json);
    }

    return run_interactive_app(game_root, initial_chapter, custom_level, max_frames, start_in_main_menu, trace_path);
}
