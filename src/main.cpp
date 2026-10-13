#if !defined(__ANDROID__)
// The desktop builds run main() themselves. On Android SDL's Java side (SDLActivity) calls
// SDL_main, which <SDL2/SDL_main.h> renames main to.
#define SDL_MAIN_HANDLED
#endif
#include <cstdlib>
#include <SDL2/SDL.h>
#if defined(__ANDROID__)
#include <SDL2/SDL_main.h>
#include <unistd.h>
#include "platform/android_log.hpp"
#define ME_PLATFORM_TITLE "Android"
#elif defined(__APPLE__)
#include <SDL2/SDL_metal.h>
#define ME_PLATFORM_TITLE "macOS"
#elif defined(__EMSCRIPTEN__)
#include <emscripten.h>
#define ME_PLATFORM_TITLE "WebAssembly"
#elif defined(_WIN32)
#define ME_PLATFORM_TITLE "Windows"
#else
#define ME_PLATFORM_TITLE "Linux"
#endif
#if defined(ME_RENDERER_VULKAN)
#include <SDL2/SDL_vulkan.h>
#endif
#include "platform/touch_controls.hpp"
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
#include <thread>
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
            update_script_movers(scene, 1.0f / 30.0f);
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

// -----------------------------------------------------------------------------
// Oracle stage 23: how loud a sound at a place is (docs/AUDIO_SYSTEM_RE.md section 3.2).
// The curve itself against values worked by hand from the executable's formulas, then the audio
// engine, without a device, playing cues at measured distances from the listener: what each wave
// would be set to must be its attenuation nodes' curve, follow the listener, be the product of two
// nodes on one branch, leave a wave with no node above it alone, and not start at all where retail
// refuses to; the high frequencies its source's low-pass filter would be given must be the same
// nodes' low pass; and of a level's emitters none may hold a source it is not heard on, and every
// layer of one's cue must be a voice on its own node's curve.
// --verify-sound runs it, and stage 26 after it; ME_AUDIO_DEBUG=1 prints every gain as it is set.
// -----------------------------------------------------------------------------
static bool oracle_sound_attenuation(const std::string& game_root) {
    using namespace me;
    const auto near = [](float a, float b, float tolerance = 1.0e-3f) { return std::abs(a - b) <= tolerance; };

    // 1. USoundNodeAttenuation::ParseNodes' five models (0x00B7DD60). The wanted values are the
    // formulas evaluated by hand to four places; the research's emulation of the function's own
    // bytes gave the same ones.
    struct Worked {
        uint8_t model;
        float distance, min_radius, max_radius, db, want;
    };
    static const Worked kWorked[] = {
        {0, 480.0f, 10.0f, 2000.0f, -60.0f, 0.7638f},    // a concrete bullet impact
        {0, 1000.0f, 10.0f, 2000.0f, -60.0f, 0.5025f},
        {0, 1900.0f, 10.0f, 2000.0f, -60.0f, 0.0503f},
        {0, 2000.0f, 10.0f, 2000.0f, -60.0f, 0.0f},      // at MaxRadius: silent
        {0, 480.0f, 400.0f, 600.0f, -60.0f, 0.6f},       // Flesh.9mm_Flesh_Impact
        {0, 299.0f, 300.0f, 300.0f, -60.0f, 1.0f},       // Faith.9mm_Faith_Impact: equal radii
        {0, 300.0f, 300.0f, 300.0f, -60.0f, 0.0f},
        {1, 1000.0f, 500.0f, 2000.0f, -60.0f, 0.5f},     // Glass_BP.9mm_Glass_BP_Impact
        {1, 480.0f, 10.0f, 2000.0f, -60.0f, 0.2694f},    // Glass_Prop, Taser_NotBody
        {1, 1000.0f, 10.0f, 2000.0f, -60.0f, 0.1308f},
        {2, 1000.0f, 100.0f, 2000.0f, -60.0f, 0.8f},
        {2, 1999.0f, 100.0f, 2000.0f, -60.0f, 0.4002f},  // still 0.02 * Max / Min just inside MaxRadius
        {2, 2000.0f, 100.0f, 2000.0f, -60.0f, 0.0f},
        {3, 2700.0f, 400.0f, 5000.0f, -60.0f, 0.6926f},
        {3, 400.01f, 400.0f, 5000.0f, -60.0f, 0.9670f},  // the step down just outside MinRadius
        {4, 2700.0f, 400.0f, 5000.0f, -60.0f, 0.0316f},  // half way: -30 dB
        {5, 2700.0f, 400.0f, 5000.0f, -60.0f, 1.0f},     // no such model: untouched
        {3, 4000.0f, 1.0f, 18000.0f, -60.0f, 0.9744f},   // the two nodes of a gun's Fire3P,
        {3, 4000.0f, 10.0f, 8000.0f, -60.0f, 0.8963f},   // see 5 below
    };
    bool curve_ok = true;
    float curve_worst = 0.0f;
    for (const Worked& w : kWorked) {
        const float got = attenuation_gain(w.model, w.distance, w.min_radius, w.max_radius, w.db);
        curve_worst = std::max(curve_worst, std::abs(got - w.want));
        if (!near(got, w.want, 1.0e-4f)) {
            curve_ok = false;
            std::cout << "  [curve] model " << static_cast<int>(w.model) << " " << w.min_radius << "/" << w.max_radius << " at " << w.distance
                      << ": " << got << ", wanted " << w.want << std::endl;
        }
    }
    // A radius is drawn between its pair whichever way round the pair is stored.
    const float backwards[2] = {5000.0f, 800.0f};
    curve_ok = curve_ok && near(draw_radius(backwards, 0.0f), 5000.0f) && near(draw_radius(backwards, 1.0f), 800.0f) &&
               near(draw_radius(backwards, 0.25f), 3950.0f);
    std::cout << "  [curve] " << std::size(kWorked) << " worked values: " << (curve_ok ? "OK" : "FAIL") << ", worst difference "
              << curve_worst << std::endl;

    // 1b. The same function's low pass (0x00B7DEDA), for a node with bAttenuateWithLowPassFilter:
    // all of the high frequencies at or inside LPFMinRadius, none at or beyond LPFMaxRadius, a
    // straight line between, whatever the node's model.
    struct WorkedLowPass {
        float distance, lpf_min, lpf_max, want;
    };
    static const WorkedLowPass kLowPass[] = {
        {1000.0f, 1500.0f, 5000.0f, 1.0f},     // the class's radii: untouched inside 1500
        {1500.0f, 1500.0f, 5000.0f, 1.0f},
        {1900.0f, 1500.0f, 5000.0f, 0.8857f},
        {3250.0f, 1500.0f, 5000.0f, 0.5f},     // half way
        {4999.0f, 1500.0f, 5000.0f, 0.0003f},
        {5000.0f, 1500.0f, 5000.0f, 0.0f},     // at LPFMaxRadius and beyond: none
        {9000.0f, 1500.0f, 5000.0f, 0.0f},
        {2000.0f, 1500.0f, 2500.0f, 0.5f},     // Default__SoundNodeAmbient's radii
        {4000.0f, 300.0f, 12000.0f, 0.6838f},  // a pistol's Fire3P, see 5 below
        {299.0f, 300.0f, 300.0f, 1.0f},        // equal radii: all of them inside, none outside
        {300.0f, 300.0f, 300.0f, 0.0f},
    };
    bool lowpass_ok = true;
    float lowpass_worst = 0.0f;
    for (const WorkedLowPass& w : kLowPass) {
        const float got = low_pass_gain(w.distance, w.lpf_min, w.lpf_max);
        lowpass_worst = std::max(lowpass_worst, std::abs(got - w.want));
        if (!near(got, w.want, 1.0e-4f)) {
            lowpass_ok = false;
            std::cout << "  [low pass] " << w.lpf_min << "/" << w.lpf_max << " at " << w.distance << ": " << got << ", wanted " << w.want << std::endl;
        }
    }
    {
        // The nodes above one wave: a node with the flag off leaves the high frequencies alone, two
        // nodes with it multiply, and a node that does not attenuate still filters.
        DrawnAttenuation plain;  // Linear 400 / 5000, no low pass
        DrawnAttenuation first = plain, second = plain, filter_only = plain;
        first.low_pass = true;
        first.lpf_min_radius = 1500.0f;
        first.lpf_max_radius = 5000.0f;
        second.low_pass = true;
        second.lpf_min_radius = 1000.0f;
        second.lpf_max_radius = 5500.0f;
        filter_only.attenuate = false;
        filter_only.low_pass = true;
        filter_only.lpf_min_radius = 1500.0f;
        filter_only.lpf_max_radius = 5000.0f;
        VoiceAttenuation none, one, two, unattenuated;
        none.nodes = {plain};
        one.nodes = {first};
        two.nodes = {first, second};
        unattenuated.nodes = {filter_only};
        lowpass_ok = lowpass_ok && near(none.high_frequency_gain(3250.0f), 1.0f) && near(one.high_frequency_gain(3250.0f), 0.5f) &&
                     near(two.high_frequency_gain(3250.0f), 0.25f) && near(two.high_frequency_gain(5200.0f), 0.0f) &&
                     near(unattenuated.gain(3250.0f), 1.0f) && near(unattenuated.high_frequency_gain(3250.0f), 0.5f);
    }
    std::cout << "  [low pass] " << std::size(kLowPass) << " worked values: " << (lowpass_ok ? "OK" : "FAIL") << ", worst difference "
              << lowpass_worst << std::endl;

    // 2. The engine, with no device: the stock banks and the training area's audio sublevel.
    AudioEngine audio;
    audio.init(true);
    audio.load_stock_audio(game_root);
    audio.load_level_audio(game_root, "Maps/SP00/Tutorial_p.me1");
    audio.load_cue_bank(game_root, "A_WP_SLAPBACK");
    const Vec3 ahead(1.0f, 0.0f, 0.0f), up(0.0f, 0.0f, 1.0f);
    // Far from the level, so that none of its emitters is in the way of the counts below.
    const Vec3 home(400000.0f, 400000.0f, 0.0f);
    const auto tick = [&](const Vec3& listener, float dt = 0.0f) { audio.update(dt, listener, ahead, up, 0.0f, false); };
    const auto voices_of = [&](const std::string& name) {
        std::vector<AudioEngine::PositionalVoice> found;
        for (const AudioEngine::PositionalVoice& v : audio.positional_voices()) {
            if (!v.emitter && v.name == name) found.push_back(v);
        }
        return found;
    };
    const auto one_gain = [&](const std::string& name) {
        const auto found = voices_of(name);
        return found.size() == 1 ? found[0].distance_gain : -1.0f;
    };

    // 3. A bullet impact on concrete (Linear 10 / 2000, 1.35 s) at 480, 1000 and 2100 uu: the curve's
    // gain at each, the last one playing at 0 (it lasts more than a second, so it is started).
    const char* const kConcrete = "Concrete.9mm_Concrete_Impact";
    bool impact_ok = audio.find_cue(kConcrete) != nullptr;
    float impact_gain[3] = {-1.0f, -1.0f, -1.0f};
    {
        const float distances[3] = {480.0f, 1000.0f, 2100.0f};
        const float wanted[3] = {0.7638f, 0.5025f, 0.0f};
        tick(home);
        for (int i = 0; i < 3; ++i) {
            audio.stop_all();
            audio.play_sound_3d(kConcrete, home + Vec3(distances[i], 0.0f, 0.0f));
            impact_ok = impact_ok && voices_of(kConcrete).empty();  // not before the update that knows the listener
            tick(home);
            const auto found = voices_of(kConcrete);
            if (found.size() == 1) impact_gain[i] = found[0].distance_gain;
            // (The cue's VolumeMultiplier is 1: the source's gain is the distance's.)
            impact_ok = impact_ok && found.size() == 1 && near(found[0].distance_gain, wanted[i], 1.0e-4f) &&
                        near(found[0].gain, wanted[i], 1.0e-4f) && near(found[0].distance, distances[i], 0.01f) && found[0].spatialized &&
                        found[0].attenuation_nodes == 1;
        }
    }
    // The gain follows the listener while the sound plays, down to nothing and back, and so does
    // what its source's low-pass filter would be given (the cue's node has the class's 1500 / 5000).
    bool follow_ok = true;
    float follow_gain[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
    float follow_hf[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
    {
        audio.stop_all();
        const Vec3 at = home + Vec3(0.0f, 480.0f, 0.0f);
        audio.play_sound_3d(kConcrete, at);
        const float distances[4] = {480.0f, 1900.0f, 2500.0f, 1000.0f};
        const float wanted[4] = {0.7638f, 0.0503f, 0.0f, 0.5025f};
        const float wanted_hf[4] = {1.0f, 0.8857f, 0.7143f, 1.0f};
        for (int i = 0; i < 4; ++i) {
            tick(at - Vec3(0.0f, distances[i], 0.0f));
            follow_gain[i] = one_gain(kConcrete);
            const auto found = voices_of(kConcrete);
            if (found.size() == 1) follow_hf[i] = found[0].high_frequency_gain;
            follow_ok = follow_ok && near(follow_gain[i], wanted[i], 1.0e-4f) && near(follow_hf[i], wanted_hf[i], 1.0e-4f);
        }
    }
    // A sound played on the player (2D) is not one of these: nothing gives it a distance.
    bool flat_ok = true;
    {
        audio.stop_all();
        tick(home);
        const size_t before = audio.positional_voices().size();
        audio.play_sound(kConcrete);
        audio.play_cue("Doors.Door_Barge");
        tick(home, 0.5f);
        tick(home + Vec3(5000.0f, 0.0f, 0.0f));
        flat_ok = audio.positional_voices().size() == before;
    }

    // 4. The test for starting at all (UAudioDevice::CreateComponent): a cue of at most a second is
    // refused where the listener is not within its MaxAudibleDistance. Fabric's impact lasts 0.72 s.
    const char* const kFabric = "Fabric.9mm_Fabric_Impact";
    bool gate_ok = audio.find_cue(kFabric) != nullptr && near(audio.cue_max_audible_distance(kFabric), 2000.0f) &&
                   near(audio.cue_max_audible_distance(kConcrete), 2000.0f) &&
                   audio.cue_max_audible_distance("Fire.Fire1P") >= kSoundWorldMax;  // no attenuation node: anywhere
    size_t gate_far = 99, gate_near = 99;
    {
        audio.stop_all();
        audio.play_sound_3d(kFabric, home + Vec3(2100.0f, 0.0f, 0.0f));
        tick(home);
        gate_far = voices_of(kFabric).size();
        audio.play_sound_3d(kFabric, home + Vec3(1900.0f, 0.0f, 0.0f));
        tick(home);
        gate_near = voices_of(kFabric).size();
        gate_ok = gate_ok && gate_far == 0 && gate_near == 1 && near(one_gain(kFabric), 0.0503f, 1.0e-4f);
    }

    // 5. Two attenuation nodes on one branch multiply. A pistol's Fire3P is a mixer of two layers
    // under a LogReverse 1 / 18000 node, one of them under a second node, LogReverse 10 / 8000.
    // Their low passes are 300 / 12000 and 10000 / 20000: at 4000 uu the first takes the high
    // frequencies of both layers to 0.6838 and the second leaves them. At 13000 uu the first layer is
    // still at 0.8693 of its volume and past its LPFMaxRadius: the source's floor, 0.01, dull and
    // not silent.
    bool chain_ok = false;
    float chain_one = -1.0f, chain_two = -1.0f, chain_hf = -1.0f, chain_far = -1.0f, chain_far_hf = -1.0f;
    {
        audio.stop_all();
        audio.play_sound_3d("Fire.Fire3P", home + Vec3(0.0f, 0.0f, 4000.0f));
        tick(home);
        bool hf_same = true;
        for (const AudioEngine::PositionalVoice& v : voices_of("Fire.Fire3P")) {
            if (v.attenuation_nodes == 1) chain_one = v.distance_gain;
            if (v.attenuation_nodes == 2) chain_two = v.distance_gain;
            hf_same = hf_same && (chain_hf < 0.0f || near(chain_hf, v.high_frequency_gain, 1.0e-5f));
            chain_hf = v.high_frequency_gain;
        }
        audio.stop_all();
        audio.play_sound_3d("Fire.Fire3P", home + Vec3(0.0f, 0.0f, 13000.0f));
        tick(home);
        for (const AudioEngine::PositionalVoice& v : voices_of("Fire.Fire3P")) {
            if (v.attenuation_nodes != 1) continue;
            chain_far = v.distance_gain;
            chain_far_hf = v.high_frequency_gain;
        }
        chain_ok = near(chain_one, 0.9744f, 2.0e-4f) && near(chain_two, 0.9744f * 0.8963f, 2.0e-4f) && hf_same &&
                   near(chain_hf, 0.6838f, 1.0e-4f) && near(chain_far, 0.8693f, 2.0e-4f) && near(chain_far_hf, 0.01f, 1.0e-6f);
    }

    // 6. A wave with no attenuation node above it plays as it is, at the listener, beside its
    // attenuated sisters: the soft hand step on metal has such a layer under its mixer (Linear
    // 100 / 800 on the other). Past 800 uu the cue (0.38 s) does not start.
    const char* const kHandStep = "Metal._21_Female_HandStepSoft";
    bool bare_ok = false;
    float bare_layer = -1.0f, placed_layer = -1.0f;
    {
        audio.stop_all();
        audio.play_sound_3d(kHandStep, home + Vec3(400.0f, 0.0f, 0.0f));
        tick(home);
        bool flags_ok = true;
        for (const AudioEngine::PositionalVoice& v : voices_of(kHandStep)) {
            if (v.attenuation_nodes == 0) {
                bare_layer = v.distance_gain;
                flags_ok = flags_ok && !v.spatialized;
            } else {
                placed_layer = v.distance_gain;
                flags_ok = flags_ok && v.spatialized;
            }
        }
        audio.stop_all();
        audio.play_sound_3d(kHandStep, home + Vec3(900.0f, 0.0f, 0.0f));
        tick(home);
        bare_ok = flags_ok && near(bare_layer, 1.0f) && near(placed_layer, 1.0f - 300.0f / 700.0f) && voices_of(kHandStep).empty();
    }

    // 7. bSpatialize off, bAttenuate on (the 300 radio lines): the distance's gain, but at the
    // listener, not placed. The training area's opening line is Linear 400 / 5000.
    const char* const kRadio = "A_VO_SP00_Opening_1_1_Merc_Cue";
    bool radio_ok = false;
    float radio_gain = -1.0f;
    {
        audio.stop_all();
        audio.play_sound_3d(kRadio, home + Vec3(2700.0f, 0.0f, 0.0f));
        tick(home);
        const auto found = voices_of(kRadio);
        if (found.size() == 1) radio_gain = found[0].distance_gain;
        radio_ok = found.size() == 1 && near(radio_gain, 0.5f, 1.0e-4f) && !found[0].spatialized && found[0].attenuation_nodes == 1;
    }

    // 8. DICE's TdSoundNodeAttenuation (the guns' slap-back echo): held back by the first distance
    // over the speed of sound, and attenuated twice, once on that first distance for good. The cue's
    // first node is Linear 100 / 11000 with bDelay at 33100 uu/s over a mixer of a click (a 40 ms
    // wave) and a reverb tail that has a second such node above it, Linear 800 / 15000 at 53100 uu/s.
    // At 5000 uu the click comes 0.151 s late at 0.5505 squared, the tail 0.094 s after it at that
    // times 0.7042 squared.
    const char* const kSlap = "Slap_Back.Slap_Back";
    // Both nodes have the low pass on, and a TdSoundNodeAttenuation's parent takes the first
    // distance and the delay for its LPF radii: the echo has the floor's high frequencies, 0.01.
    bool td_ok = false;
    size_t td_early = 99;
    float td_click = -1.0f, td_tail = -1.0f, td_moved = -1.0f, td_hf = -1.0f;
    if (audio.find_cue(kSlap)) {
        audio.stop_all();
        const Vec3 at = home + Vec3(5000.0f, 0.0f, 0.0f);
        audio.play_sound_3d(kSlap, at);
        tick(home);
        tick(home, 0.14f);
        td_early = voices_of(kSlap).size();
        tick(home, 0.012f);
        for (const AudioEngine::PositionalVoice& v : voices_of(kSlap)) {
            if (v.attenuation_nodes == 1) td_click = v.distance_gain;
        }
        tick(home, 0.1f);
        for (const AudioEngine::PositionalVoice& v : voices_of(kSlap)) {
            if (v.attenuation_nodes == 2) td_tail = v.distance_gain;
        }
        tick(home + Vec3(2500.0f, 0.0f, 0.0f));  // half way there: the first factors stay, the second ones follow
        for (const AudioEngine::PositionalVoice& v : voices_of(kSlap)) {
            if (v.attenuation_nodes == 2) td_moved = v.distance_gain;
            td_hf = std::max(td_hf, v.high_frequency_gain);
        }
        const float outer = 1.0f - 4900.0f / 10900.0f, inner = 1.0f - 4200.0f / 14200.0f;
        td_ok = td_early == 0 && near(td_click, outer * outer) && near(td_tail, outer * outer * inner * inner) &&
                near(td_moved, outer * (1.0f - 2400.0f / 10900.0f) * inner * (1.0f - 1700.0f / 14200.0f)) && near(td_hf, 0.01f, 1.0e-6f);
    }

    // 9. The level's emitters, heard from a thousand units above each in turn: whichever of them
    // have a source play at their cue's curve for the distance (the radii drawn once per emitter,
    // so a ranged MaxRadius gives a gain between the two ends), and none is past its reach.
    bool emitter_ok = true;
    int emitters_heard = 0, emitters_checked = 0;
    float emitter_worst = 0.0f, emitter_sample_distance = 0.0f, emitter_sample_gain = -1.0f, emitter_sample_hf = -1.0f;
    std::string emitter_sample;
    {
        audio.stop_all();
        const std::vector<AmbientEmitterInfo> emitters = audio.get_ambient_emitters();
        for (size_t e = 0; e < emitters.size(); ++e) {
            const Vec3 listener = emitters[e].location + Vec3(0.0f, 0.0f, 1000.0f);
            // (The vehicle packs wait 7 to 15 s before each pass.)
            for (int step = 0; step < (e == 0 ? 20 : 1); ++step) tick(listener, e == 0 ? 1.0f : 0.0f);
            for (const AudioEngine::PositionalVoice& v : audio.positional_voices()) {
                if (!v.emitter) continue;
                ++emitters_heard;
                const SoundCueDef* cue = audio.find_cue(v.name);
                if (!cue || cue->attenuations.size() != 1 || v.attenuation_nodes != 1) continue;
                const SoundAttenuation& a = cue->attenuations[0];
                const float lo = attenuation_gain(a.model, v.distance, std::max(a.min_radius[0], a.min_radius[1]),
                                                  std::min(a.max_radius[0], a.max_radius[1]), a.db_at_max);
                const float hi = attenuation_gain(a.model, v.distance, std::min(a.min_radius[0], a.min_radius[1]),
                                                  std::max(a.max_radius[0], a.max_radius[1]), a.db_at_max);
                ++emitters_checked;
                float off = std::max(0.0f, std::max(lo - v.distance_gain, v.distance_gain - hi));
                // And its low pass, where the node has one with single radii (WindHard: 10 / 5000).
                if (a.lowpass && a.lpf_min_radius[0] == a.lpf_min_radius[1] && a.lpf_max_radius[0] == a.lpf_max_radius[1]) {
                    const float hf = std::clamp(low_pass_gain(v.distance, a.lpf_min_radius[0], a.lpf_max_radius[0]), 0.01f, 1.0f);
                    off = std::max(off, std::abs(hf - v.high_frequency_gain));
                }
                emitter_worst = std::max(emitter_worst, off);
                emitter_ok = emitter_ok && off < 1.0e-4f && v.distance < std::max(a.max_radius[0], a.max_radius[1]);
                if (emitter_sample.empty() && lo == hi && v.distance > 900.0f && v.distance_gain > 0.0f && v.distance_gain < 1.0f) {
                    emitter_sample = v.name;
                    emitter_sample_distance = v.distance;
                    emitter_sample_gain = v.distance_gain;
                    emitter_sample_hf = v.high_frequency_gain;
                }
            }
        }
        emitter_ok = emitter_ok && emitters_checked >= 20;
        // And from far away from all of them, none has a source.
        tick(home);
        for (const AudioEngine::PositionalVoice& v : audio.positional_voices()) emitter_ok = emitter_ok && !v.emitter;
    }

    // 10. Every layer of an emitter's cue is a voice of its own, on the curve of the node above it
    // (114 of the factory's 153 emitters have a cue that mixes two layers with different radii: an
    // air vent's hiss, Logarithmic 2 / 1500, with its hum, Logarithmic 150 / 3000). From 150 and from
    // 1900 uu above each of the factory's emitters in turn: no source is at volume 0, every voice of
    // a cue whose radii are all single values is on the curve of one of its cue's nodes, and of the
    // vent straight below both layers ask for a source from 150 uu and the hum alone from 1900.
    bool layered_ok = true;
    int layered_heard = 0, layered_silent = 0, layered_checked = 0, vents = 0, vents_right = 0;
    float layered_worst = 0.0f, vent_near[2] = {-1.0f, -1.0f}, vent_far = -1.0f;
    {
        audio.stop_all();
        audio.load_level_audio(game_root, "Maps/SP06/Factory_p.me1");
        const std::vector<AmbientEmitterInfo> emitters = audio.get_ambient_emitters();
        for (const float height : {150.0f, 1900.0f}) {
            for (size_t e = 0; e < emitters.size(); ++e) {
                tick(emitters[e].location + Vec3(0.0f, 0.0f, height));
                for (const AudioEngine::PositionalVoice& v : audio.positional_voices()) {
                    if (!v.emitter) continue;
                    ++layered_heard;
                    if (v.distance_gain <= 0.0f) ++layered_silent;
                    const SoundCueDef* cue = audio.find_cue(v.name);
                    if (!cue || cue->attenuations.size() < 2 || v.attenuation_nodes != 1) continue;
                    bool single_values = true;
                    for (const SoundAttenuation& a : cue->attenuations) {
                        single_values = single_values && a.attenuate && a.min_radius[0] == a.min_radius[1] && a.max_radius[0] == a.max_radius[1];
                    }
                    if (!single_values) continue;
                    float off = 1.0f;
                    for (const SoundAttenuation& a : cue->attenuations) {
                        off = std::min(off, std::abs(v.distance_gain - attenuation_gain(a.model, v.distance, a.min_radius[0], a.max_radius[0], a.db_at_max)));
                    }
                    ++layered_checked;
                    layered_worst = std::max(layered_worst, off);
                }
                if (emitters[e].cue_name != "A_Prop_AirVent_Indoor") continue;
                // The vent straight below: its hiss and its hum, each by its own curve.
                std::vector<float> gains;
                for (const AudioEngine::PositionalVoice& v : audio.emitter_candidates()) {
                    if (v.emitter_index == static_cast<int>(e)) gains.push_back(v.distance_gain);
                }
                std::sort(gains.begin(), gains.end());
                ++vents;
                const float hiss = attenuation_gain(1, height, 2.0f, 1500.0f, -60.0f), hum = attenuation_gain(1, height, 150.0f, 3000.0f, -60.0f);
                if (height == 150.0f) {
                    if (gains.size() == 2 && near(gains[0], hiss, 1.0e-4f) && near(gains[1], hum, 1.0e-4f)) ++vents_right;
                    if (gains.size() == 2 && vent_near[0] < 0.0f) {
                        vent_near[0] = gains[0];
                        vent_near[1] = gains[1];
                    }
                } else {
                    if (gains.size() == 1 && near(gains[0], hum, 1.0e-4f)) ++vents_right;
                    if (gains.size() == 1 && vent_far < 0.0f) vent_far = gains[0];
                }
            }
        }
        layered_ok = layered_silent == 0 && layered_checked >= 20 && layered_worst < 1.0e-4f && vents >= 2 && vents_right == vents;
    }

    const bool pass = curve_ok && lowpass_ok && impact_ok && follow_ok && flat_ok && gate_ok && chain_ok && bare_ok && radio_ok && td_ok &&
                      emitter_ok && layered_ok;
    std::cout << std::fixed << std::setprecision(4) << "  -> Stage 23 Result: " << (pass ? "PASS" : "FAIL") << " (Curve=" << (curve_ok ? "OK" : "FAIL")
              << ", LowPass=" << (lowpass_ok ? "OK" : "FAIL") << ", Impact=" << (impact_ok ? "OK" : "FAIL") << " [concrete at 480 / 1000 / 2100 uu: "
              << impact_gain[0] << " / " << impact_gain[1] << " / " << impact_gain[2] << "], Follow=" << (follow_ok ? "OK" : "FAIL")
              << " [listener at 480 / 1900 / 2500 / 1000 uu: " << follow_gain[0] << " / " << follow_gain[1] << " / " << follow_gain[2] << " / "
              << follow_gain[3] << ", high frequencies " << follow_hf[0] << " / " << follow_hf[1] << " / " << follow_hf[2] << " / " << follow_hf[3]
              << "], 2D=" << (flat_ok ? "OK" : "FAIL") << ", Gate=" << (gate_ok ? "OK" : "FAIL") << " [0.72 s cue at 2100 uu: " << gate_far
              << " started, at 1900: " << gate_near << "], Chain=" << (chain_ok ? "OK" : "FAIL") << " [Fire3P at 4000 uu: one node "
              << chain_one << ", two " << chain_two << ", high frequencies " << chain_hf << "; at 13000 uu: " << chain_far << " with high frequencies "
              << chain_far_hf << "], BareLayer=" << (bare_ok ? "OK" : "FAIL") << " [at 400 uu: " << bare_layer
              << " beside " << placed_layer << "], Radio=" << (radio_ok ? "OK" : "FAIL") << " [not spatialised, at 2700 uu: " << radio_gain
              << "], TdAttenuation=" << (td_ok ? "OK" : "FAIL") << " [at 5000 uu: " << td_early << " waves before 0.15 s, then the click at "
              << td_click << " and the tail at " << td_tail << ", " << td_moved << " heard from 2500 uu, high frequencies " << td_hf
              << "], Emitters=" << (emitter_ok ? "OK" : "FAIL") << " [" << emitters_heard << " heard, "
              << emitters_checked << " held against their curve and low pass, worst " << emitter_worst << "; " << emitter_sample << " at "
              << std::setprecision(1) << emitter_sample_distance << std::setprecision(4) << " uu: " << emitter_sample_gain
              << ", high frequencies " << emitter_sample_hf << "], LayeredEmitters=" << (layered_ok ? "OK" : "FAIL") << " [the factory: "
              << layered_heard << " heard, " << layered_silent << " at volume 0, " << layered_checked
              << " held against their cue's curves, worst " << layered_worst << "; " << vents_right << " of " << vents
              << " looks at an A_Prop_AirVent_Indoor right, from 150 uu its layers at " << vent_near[0] << " and " << vent_near[1]
              << ", from 1900 uu one at " << vent_far << "])" << std::defaultfloat << std::setprecision(6) << std::endl;
    return pass;
}

// -----------------------------------------------------------------------------
// Oracle stage 26: a paused game, and the level emitters as retail's audio device has them
// (docs/AUDIO_SYSTEM_RE.md section 3.3). The audio engine with no device, as in stage 23:
//   Pause      under set_game_paused the sounds at a place and the emitters' voices stay as they
//              are for as long as it lasts (none starts, none ends, none moves on, none follows the
//              listener), a delay does not run, a sound asked for meanwhile waits and starts with
//              the first update after it, a sound of a UI sound group plays through, and the level's
//              music group is at 0.25, then 1.0 (the ini's 1.2 before the first call)
//   FrontEnd   behind the front end nothing of the level sounds and nothing is paused
//   Layers     an indoor air vent is two voices that loop, each on its own node's curve and ranked
//              by its volume in the graph (its mix group's 0.9 in it); nothing of it is parsed past
//              its sound group's MaxUpdateDistance
//   Budget     where more waves ask for a source than there are, the ones with one are the
//              loudest, and a sound from the pool takes one from them
//   NoRestart  a voice that keeps its source while the listener moves and the ranks change plays on
//   Rounds     a vehicle pack plays one pass a round, 7 to 15 s after the one before has ended,
//              and not the same one every time; an unheard round waits for its listener; a cue
//              with its delay above its looping node waits once; a wind's bed loops while its gusts
//              are drawn again every round
// --verify-sound runs it after stage 23; ME_AUDIO_DEBUG=1 prints every gain as it is set.
// -----------------------------------------------------------------------------
static bool oracle_sound_pause_and_emitters(const std::string& game_root) {
    using namespace me;
    const auto near = [](float a, float b, float tolerance = 1.0e-3f) { return std::abs(a - b) <= tolerance; };
    const Vec3 ahead(1.0f, 0.0f, 0.0f), up(0.0f, 0.0f, 1.0f);
    const Vec3 home(400000.0f, 400000.0f, 0.0f);  // far from every level
    using Voices = std::vector<AudioEngine::PositionalVoice>;
    const auto emitter_voices = [](const Voices& all, int emitter = -1) {
        Voices found;
        for (const AudioEngine::PositionalVoice& v : all) {
            if (v.emitter && (emitter < 0 || v.emitter_index == emitter)) found.push_back(v);
        }
        return found;
    };
    const auto first_emitter_of = [](const AudioEngine& audio, const std::string& cue) {
        const std::vector<AmbientEmitterInfo>& emitters = audio.get_ambient_emitters();
        for (size_t e = 0; e < emitters.size(); ++e) {
            if (emitters[e].cue_name == cue) return static_cast<int>(e);
        }
        return -1;
    };

    AudioEngine audio;
    audio.init(true);
    const float music_at_start = audio.ingame_music_volume();
    audio.load_stock_audio(game_root);
    audio.load_level_audio(game_root, "Maps/SP00/Tutorial_p.me1");
    audio.load_cue_bank(game_root, "A_WP_SLAPBACK");
    const auto tick = [&](const Vec3& listener, float dt = 0.0f) { audio.update(dt, listener, ahead, up, 0.0f, false); };

    // 1. Pause. Beside one of the training area's winds, with a bullet impact playing 300 uu off
    // (1.35 s long) and a slap-back echo 5000 uu off still held back by its distance (0.151 s).
    const char* const kConcrete = "Concrete.9mm_Concrete_Impact";
    const char* const kFabric = "Fabric.9mm_Fabric_Impact";
    const char* const kSlap = "Slap_Back.Slap_Back";
    const char* const kFootstep = "Concrete._03_Female_FootStepRun";
    const char* const kClick = "A_Pos";
    bool pause_ok = false;
    size_t paused_voices = 0, changed = 0, started_in_pause = 0, started_after = 0;
    float music_paused = -1.0f, music_after = -1.0f, age_before = -1.0f, age_in_pause = -1.0f, age_after = -1.0f;
    bool click_played = false, others_waited = false, others_started = false, delay_held = false, impact_ended = false;
    const int wind = first_emitter_of(audio, "WindHard");
    if (wind >= 0 && audio.find_cue(kSlap) && audio.has_sound(kFootstep) && audio.has_sound(kClick)) {
        const Vec3 ear = audio.get_ambient_emitters()[static_cast<size_t>(wind)].location + Vec3(0.0f, 0.0f, 150.0f);
        for (int i = 0; i < 5; ++i) tick(ear, 0.1f);
        audio.play_sound_3d(kConcrete, ear + Vec3(300.0f, 0.0f, 0.0f));
        audio.play_sound_3d(kSlap, ear + Vec3(5000.0f, 0.0f, 0.0f));
        tick(ear);
        tick(ear, 0.1f);
        const Voices before = audio.positional_voices();
        for (const AudioEngine::PositionalVoice& v : before) {
            if (v.name == kConcrete) age_before = v.age;
        }
        audio.set_play_log(true);
        (void)audio.take_play_log();
        audio.set_game_paused(true);
        music_paused = audio.ingame_music_volume();
        audio.play_sound_3d(kFabric, ear + Vec3(200.0f, 0.0f, 0.0f));  // at a place
        audio.play_sound(kFootstep);                                  // on the player
        audio.play_sound(kClick);                                     // the menu's click: HudSFX, a UI sound group
        // A minute of updates, the listener walking off the while: the vehicle packs would have
        // sounded, the impact ended, the echo come, and every gain fallen.
        for (int i = 0; i < 600; ++i) tick(ear + Vec3(0.0f, 2.0f * static_cast<float>(i), 0.0f), 0.1f);
        const Voices during = audio.positional_voices();
        paused_voices = during.size();
        changed = before.size() == during.size() ? 0 : 1;
        for (size_t i = 0; i < before.size() && i < during.size(); ++i) {
            const AudioEngine::PositionalVoice& a = before[i];
            const AudioEngine::PositionalVoice& b = during[i];
            if (a.name != b.name || a.clip != b.clip || a.age != b.age || a.gain != b.gain || a.distance != b.distance || !b.paused) ++changed;
            if (b.name == kConcrete) age_in_pause = b.age;
        }
        delay_held = true;
        for (const AudioEngine::PositionalVoice& v : during) delay_held = delay_held && v.name != kSlap;
        const std::vector<AudioEngine::PlayEvent> log_in_pause = audio.take_play_log();
        started_in_pause = log_in_pause.size();
        others_waited = true;
        for (const AudioEngine::PlayEvent& e : log_in_pause) {
            click_played = click_played || e.name == kClick;
            others_waited = others_waited && e.name == kClick;
        }
        audio.set_game_paused(false);
        music_after = audio.ingame_music_volume();
        tick(ear, 0.04f);
        const std::vector<AudioEngine::PlayEvent> log_after = audio.take_play_log();
        started_after = log_after.size();
        bool step_started = false, fabric_started = false;
        for (const AudioEngine::PlayEvent& e : log_after) {
            step_started = step_started || e.name == kFootstep;
            fabric_started = fabric_started || e.name == kFabric;
        }
        bool fabric_sounds = false, echo_early = false, echo_came = false;
        for (const AudioEngine::PositionalVoice& v : audio.positional_voices()) {
            if (v.name == kConcrete) age_after = v.age;
            fabric_sounds = fabric_sounds || v.name == kFabric;
            echo_early = echo_early || v.name == kSlap;  // 0.14 of its 0.151 s
        }
        others_started = step_started && fabric_started && fabric_sounds;
        tick(ear, 0.02f);
        for (const AudioEngine::PositionalVoice& v : audio.positional_voices()) echo_came = echo_came || v.name == kSlap;
        delay_held = delay_held && !echo_early && echo_came;
        // And the impact plays out the 1.2 s it had left, no more.
        tick(ear, 1.0f);
        bool still = false, gone = true;
        for (const AudioEngine::PositionalVoice& v : audio.positional_voices()) still = still || v.name == kConcrete;
        tick(ear, 0.3f);
        tick(ear, 0.0f);
        for (const AudioEngine::PositionalVoice& v : audio.positional_voices()) gone = gone && v.name != kConcrete;
        impact_ended = still && gone;
        audio.set_play_log(false);
        // A spawn's SetPause(false), with no pause before it, on an engine of its own.
        AudioEngine fresh;
        fresh.init(true);
        const float fresh_start = fresh.ingame_music_volume();
        fresh.set_game_paused(false);
        pause_ok = near(music_at_start, 1.2f, 1.0e-6f) && near(fresh_start, 1.2f, 1.0e-6f) && near(fresh.ingame_music_volume(), 1.0f, 1.0e-6f) &&
                   near(music_paused, 0.25f, 1.0e-6f) && near(music_after, 1.0f, 1.0e-6f) && before.size() >= 3 && changed == 0 &&
                   near(age_before, 0.1f, 1.0e-4f) && age_in_pause == age_before && near(age_after, age_before + 0.04f, 1.0e-4f) &&
                   click_played && others_waited && others_started && delay_held && impact_ended;
    }

    // 2. The front end is not a pause: nothing of the level sounds behind it, and nothing waits.
    bool frontend_ok = false;
    size_t behind_menu = 99, back_in_play = 0;
    if (wind >= 0) {
        const Vec3 ear = audio.get_ambient_emitters()[static_cast<size_t>(wind)].location + Vec3(0.0f, 0.0f, 150.0f);
        audio.play_sound_3d(kConcrete, ear + Vec3(300.0f, 0.0f, 0.0f));
        tick(ear, 0.1f);
        const size_t in_play = audio.positional_voices().size();
        audio.set_menu_music(true);
        tick(ear, 0.1f);
        behind_menu = audio.positional_voices().size();
        const bool paused = audio.is_game_paused();
        audio.set_menu_music(false);
        tick(ear, 0.1f);
        const Voices back = audio.positional_voices();
        back_in_play = back.size();
        bool from_their_start = !back.empty();
        for (const AudioEngine::PositionalVoice& v : back) from_their_start = from_their_start && v.emitter && v.age == 0.0f;
        frontend_ok = in_play >= 3 && behind_menu == 0 && !paused && from_their_start;
    }

    // 3. Rounds, still in the training area. A vehicle pack (VehiclePack_02) is a looping node over a delay of 7 to 15 s, a
    // modulator and a random node over 38 passes: a random node and a delay between the looping
    // node and the wave, so the wave does not loop in its source; it plays once, and when it has
    // ended the round is over and the next draws its delay, its modulation and its pass again.
    bool rounds_ok = false;
    int pack_rounds = 0, pack_clips = 0, pack_looped = 0;
    float pack_gap_min = 1.0e9f, pack_gap_max = 0.0f, pack_wait_after_return = -1.0f;
    bool pack_lengths_ok = true;
    audio.stop_all();
    const int pack = first_emitter_of(audio, "VehiclePack_02");
    constexpr float kStep = 0.05f;
    if (pack >= 0) {
        const Vec3 ear = audio.get_ambient_emitters()[static_cast<size_t>(pack)].location + Vec3(0.0f, 0.0f, 500.0f);
        std::vector<std::string> clips;
        float now = 0.0f, started_at = -1.0f, ended_at = -1.0f, length = 0.0f;
        bool sounding = false;
        std::string clip;
        const auto listen = [&](const Vec3& from) {
            tick(from, kStep);
            now += kStep;
            const Voices heard = emitter_voices(audio.positional_voices(), pack);
            if (!heard.empty() && !sounding) {
                if (ended_at >= 0.0f) {
                    pack_gap_min = std::min(pack_gap_min, now - ended_at);
                    pack_gap_max = std::max(pack_gap_max, now - ended_at);
                }
                started_at = now;
                clip = heard[0].clip;
                if (std::find(clips.begin(), clips.end(), clip) == clips.end()) clips.push_back(clip);
                if (heard[0].looping) ++pack_looped;
                const SoundClip* wave = audio.get_clip(clip);
                length = wave ? wave->duration : 0.0f;
            } else if (heard.empty() && sounding) {
                ended_at = now;
                ++pack_rounds;
                // (Its pitch is the modulator's draw, held by the source between 0.4 and 2.)
                const float played = now - started_at;
                pack_lengths_ok = pack_lengths_ok && played >= length / 2.0f - 2.0f * kStep && played <= length / 0.4f + 2.0f * kStep;
            }
            sounding = !heard.empty();
        };
        while (now < 150.0f || sounding) listen(ear);
        // An unheard round: the listener leaves for 40 s once the pass has ended. The delay runs
        // out meanwhile, and the pass, which has no source out there, has not moved on: it starts
        // when the listener is back, not 7 s or more later.
        const int rounds_heard = pack_rounds;
        const float gap_min = pack_gap_min, gap_max = pack_gap_max;
        for (int i = 0; i < 800; ++i) listen(home);
        const bool silent_away = !sounding && pack_rounds == rounds_heard;
        const float back_at = now;
        while (!sounding && now < back_at + 20.0f) listen(ear);
        pack_wait_after_return = now - back_at;
        pack_gap_min = gap_min;
        pack_gap_max = gap_max;
        pack_clips = static_cast<int>(clips.size());
        rounds_ok = pack_rounds >= 5 && pack_looped == 0 && pack_clips >= 2 && pack_lengths_ok && pack_gap_min >= 7.0f - kStep &&
                    pack_gap_max <= 15.0f + 2.0f * kStep && silent_away && pack_wait_after_return <= 2.0f * kStep;
    }
    // A wind (WindHard): under one attenuation node a mixer of a bed, a looping node straight over
    // its wave, which loops in its source, and three layers of gusts, each a looping node over a
    // random node: one gust a round, drawn again when it has ended.
    bool wind_ok = false;
    int wind_beds = 0;
    size_t wind_gust_clips = 0;
    const int gusty = first_emitter_of(audio, "WindHard");
    if (gusty >= 0) {
        const Vec3 ear = audio.get_ambient_emitters()[static_cast<size_t>(gusty)].location + Vec3(0.0f, 0.0f, 100.0f);
        std::vector<std::string> beds, gusts;
        for (int i = 0; i < 1200; ++i) {
            tick(ear, kStep);
            for (const AudioEngine::PositionalVoice& v : emitter_voices(audio.positional_voices(), gusty)) {
                std::vector<std::string>& list = v.looping ? beds : gusts;
                if (std::find(list.begin(), list.end(), v.clip) == list.end()) list.push_back(v.clip);
            }
        }
        wind_beds = static_cast<int>(beds.size());
        wind_gust_clips = gusts.size();
        wind_ok = wind_beds == 1 && wind_gust_clips >= 4;
    }
    // 4. An indoor air vent (A_Prop_AirVent_Indoor, sound group IndoorAmbientProps): a mix group
    // node (AmbientPropSounds, 0.9) over a mixer of two layers, each an attenuation node, a
    // modulator, a random node and a looping node over each of its waves. The hiss is Logarithmic
    // 2 / 1500 and the hum Logarithmic 150 / 3000, both with the class's low pass, 1500 / 5000.
    bool layers_ok = false;
    size_t vent_near = 0, vent_far = 99, vent_past = 99;
    float vent_gain[2] = {-1.0f, -1.0f}, vent_rank[2] = {-1.0f, -1.0f}, vent_far_gain = -1.0f, vent_far_hf = -1.0f, vent_mix = -1.0f;
    audio.stop_all();
    audio.load_level_audio(game_root, "Maps/SP06/Factory_p.me1");
    const int vent = first_emitter_of(audio, "A_Prop_AirVent_Indoor");
    if (const SoundCueDef* cue = audio.find_cue("A_Prop_AirVent_Indoor"); cue && vent >= 0 && !cue->nodes.empty()) {
        // What the graph can leave a wave's volume at before the distance: the mix group's factor,
        // the mixer's inputs, the modulators' ranges.
        float lowest = 1.0e9f, highest = 0.0f;
        const auto bounds = [&](const auto& self, int node, float lo, float hi) -> void {
            if (node < 0 || static_cast<size_t>(node) >= cue->nodes.size()) return;
            const SoundCueNode& n = cue->nodes[static_cast<size_t>(node)];
            if (n.kind == SoundCueNode::Kind::Wave) {
                lowest = std::min(lowest, lo);
                highest = std::max(highest, hi);
                return;
            }
            for (size_t i = 0; i < n.children.size(); ++i) {
                float a = lo, b = hi;
                if (n.kind == SoundCueNode::Kind::MixGroup) {
                    a *= n.min_value;
                    b *= n.min_value;
                } else if (n.kind == SoundCueNode::Kind::Mixer) {
                    const float input = i < n.weights.size() ? n.weights[i] : 1.0f;
                    a *= input;
                    b *= input;
                } else if (n.kind == SoundCueNode::Kind::Modulator) {
                    a *= std::min(n.min_value, n.max_value);
                    b *= std::max(n.min_value, n.max_value);
                }
                self(self, n.children[i], a, b);
            }
        };
        bounds(bounds, 0, 1.0f, 1.0f);
        vent_mix = cue->nodes[0].kind == SoundCueNode::Kind::MixGroup ? cue->nodes[0].min_value : -1.0f;
        const Vec3 at = audio.get_ambient_emitters()[static_cast<size_t>(vent)].location;
        tick(at + Vec3(0.0f, 0.0f, 100.0f), 0.1f);
        Voices heard = emitter_voices(audio.emitter_candidates(), vent);
        std::sort(heard.begin(), heard.end(), [](const auto& a, const auto& b) { return a.distance_gain < b.distance_gain; });
        vent_near = heard.size();
        bool near_ok = heard.size() == 2;
        for (size_t i = 0; i < heard.size() && i < 2; ++i) {
            vent_gain[i] = heard[i].distance_gain;
            vent_rank[i] = heard[i].priority;
            near_ok = near_ok && heard[i].looping && heard[i].sourced && heard[i].attenuation_nodes == 1 && heard[i].spatialized &&
                      near(heard[i].high_frequency_gain, 1.0f, 1.0e-6f) && heard[i].priority >= lowest * heard[i].distance_gain - 1.0e-5f &&
                      heard[i].priority <= highest * heard[i].distance_gain + 1.0e-5f;
        }
        near_ok = near_ok && near(vent_gain[0], attenuation_gain(1, 100.0f, 2.0f, 1500.0f, -60.0f), 1.0e-4f) && near(vent_gain[1], 1.0f, 1.0e-6f);
        tick(at + Vec3(0.0f, 0.0f, 1900.0f), 0.1f);
        heard = emitter_voices(audio.emitter_candidates(), vent);
        vent_far = heard.size();
        if (heard.size() == 1) {
            vent_far_gain = heard[0].distance_gain;
            vent_far_hf = heard[0].high_frequency_gain;
        }
        tick(at + Vec3(0.0f, 0.0f, 2100.0f), 0.1f);
        vent_past = emitter_voices(audio.emitter_candidates(), vent).size();
        layers_ok = near_ok && vent_far == 1 && near(vent_far_gain, attenuation_gain(1, 1900.0f, 150.0f, 3000.0f, -60.0f), 1.0e-4f) &&
                    near(vent_far_hf, low_pass_gain(1900.0f, 1500.0f, 5000.0f), 1.0e-4f) && vent_past == 0 && near(vent_mix, 0.9f, 1.0e-6f) &&
                    highest < 1.0e8f && lowest > 0.0f;
    }

    // 5. More waves than sources. On The Boat about 80 waves ask for one at the busiest place. Above each of its
    // emitters in turn: the waves that have a source are as many as there are sources, and none of
    // them ranks under one that has none. With three bullet impacts playing beside the listener
    // there are three sources fewer for the emitters.
    bool budget_ok = false;
    size_t budget = 0, most_asking = 0, places_over = 0, misranked = 0, wrong_count = 0, budget_with_impacts = 0, sourced_with_impacts = 0;
    Vec3 busiest = home;
    audio.stop_all();
    const size_t before_load = emitter_voices(audio.positional_voices()).size();
    audio.load_level_audio(game_root, "Maps/SP07/Boat_p.me1");
    // (The level before's emitters go with it: none of the factory's is left sounding.)
    const size_t after_load = emitter_voices(audio.positional_voices()).size();
    {
        const std::vector<AmbientEmitterInfo> emitters = audio.get_ambient_emitters();
        for (const AmbientEmitterInfo& em : emitters) {
            const Vec3 ear = em.location + Vec3(0.0f, 0.0f, 150.0f);
            tick(ear, 0.1f);
            const Voices asking = audio.emitter_candidates();
            size_t sourced = 0;
            float lowest_with = 1.0e9f, highest_without = -1.0f;
            for (const AudioEngine::PositionalVoice& v : asking) {
                if (v.sourced) {
                    ++sourced;
                    lowest_with = std::min(lowest_with, v.priority);
                } else {
                    highest_without = std::max(highest_without, v.priority);
                }
            }
            budget = audio.emitter_source_budget();
            if (sourced != std::min(asking.size(), budget)) ++wrong_count;
            if (asking.size() > budget) ++places_over;
            if (lowest_with < highest_without) ++misranked;
            if (asking.size() > most_asking) {
                most_asking = asking.size();
                busiest = ear;
            }
        }
        tick(busiest, 0.1f);
        for (int i = 0; i < 3; ++i) audio.play_sound_3d("Concrete.9mm_Concrete_Impact", busiest + Vec3(100.0f * static_cast<float>(i + 1), 0.0f, 0.0f));
        tick(busiest, 0.0f);  // starts them; the emitters were dealt before
        tick(busiest, 0.1f);
        budget_with_impacts = audio.emitter_source_budget();
        for (const AudioEngine::PositionalVoice& v : audio.emitter_candidates()) sourced_with_impacts += v.sourced ? 1 : 0;
        budget_ok = budget == 32 && most_asking > budget && places_over > 0 && misranked == 0 && wrong_count == 0 && budget_with_impacts == 29 &&
                    sourced_with_impacts == 29 && before_load > 0 && after_load == 0;
        audio.stop_all();
    }

    // 6. No restart for a voice that keeps its source. From that busiest place the listener moves
    // off in steps of 150 uu, a tenth of a second each: the ranks of the waves change and some lose
    // their source to others, but every wave that has one before and after a step has played on.
    bool restart_ok = false;
    size_t kept = 0, restarted = 0, rank_changes = 0, newcomers = 0;
    {
        tick(busiest, 0.1f);
        tick(busiest, 0.1f);
        const auto key = [](const AudioEngine::PositionalVoice& v) { return v.emitter_index * 1000 + v.emitter_wave; };
        for (int step = 1; step <= 20; ++step) {
            Voices before = audio.positional_voices();
            tick(busiest + Vec3(150.0f * static_cast<float>(step), 0.0f, 0.0f), 0.1f);
            Voices after = audio.positional_voices();
            std::vector<int> order_before, order_after;
            const auto by_rank = [](const auto& a, const auto& b) { return a.priority > b.priority; };
            std::stable_sort(before.begin(), before.end(), by_rank);
            std::stable_sort(after.begin(), after.end(), by_rank);
            for (const AudioEngine::PositionalVoice& b : after) {
                bool had = false;
                for (const AudioEngine::PositionalVoice& a : before) {
                    if (key(a) != key(b)) continue;
                    had = true;
                    ++kept;
                    // (A wave that plays once a round may have ended and begun its next round.)
                    const bool played_on = near(b.age, a.age + 0.1f, 1.0e-4f);
                    if (!played_on && !(!b.looping && b.age == 0.0f)) ++restarted;
                    order_after.push_back(key(b));
                }
                if (!had) {
                    ++newcomers;
                    if (b.age != 0.0f) ++restarted;  // one that gets a source starts from its beginning
                }
            }
            for (const AudioEngine::PositionalVoice& a : before) {
                for (const AudioEngine::PositionalVoice& b : after) {
                    if (key(a) == key(b)) order_before.push_back(key(a));
                }
            }
            for (size_t i = 0; i < order_before.size() && i < order_after.size(); ++i) rank_changes += order_before[i] != order_after[i] ? 1 : 0;
        }
        restart_ok = kept >= 100 && restarted == 0 && rank_changes > 0 && newcomers > 0;
    }

    // 7. A cue with its delay above its looping node (DullHammering, in The Shard's ducts: a delay, then
    // a looping node over a modulator and a random node over six hammerings) waits once, when the
    // level starts it, and then plays one hammering after another.
    bool once_ok = false;
    int hammer_rounds = 0;
    float hammer_first = -1.0f, hammer_gap_max = 0.0f, hammer_delay[2] = {0.0f, 0.0f};
    audio.stop_all();
    audio.load_level_audio(game_root, "Maps/SP09/Scraper_p.me1");
    const int hammer = first_emitter_of(audio, "DullHammering");
    if (const SoundCueDef* cue = audio.find_cue("DullHammering"); cue && hammer >= 0) {
        // The delay above the looping node, as the cue has it.
        bool above_loop = false;
        const auto find_delay = [&](const auto& self, int node) -> void {
            if (node < 0 || static_cast<size_t>(node) >= cue->nodes.size()) return;
            const SoundCueNode& n = cue->nodes[static_cast<size_t>(node)];
            if (n.kind == SoundCueNode::Kind::Looping) return;
            if (n.kind == SoundCueNode::Kind::Delay) {
                above_loop = true;
                hammer_delay[0] = std::min(n.min_value, n.max_value);
                hammer_delay[1] = std::max(n.min_value, n.max_value);
            }
            for (int child : n.children) self(self, child);
        };
        find_delay(find_delay, 0);
        const Vec3 ear = audio.get_ambient_emitters()[static_cast<size_t>(hammer)].location + Vec3(0.0f, 0.0f, 150.0f);
        float now = 0.0f, silent_since = -1.0f, last_age = -1.0f;
        while (now < hammer_delay[1] + 40.0f) {
            tick(ear, kStep);
            now += kStep;
            const Voices heard = emitter_voices(audio.positional_voices(), hammer);
            if (heard.empty()) {
                if (hammer_first >= 0.0f && silent_since < 0.0f) silent_since = now;
                last_age = -1.0f;
                continue;
            }
            if (hammer_first < 0.0f) hammer_first = now;
            if (silent_since >= 0.0f) hammer_gap_max = std::max(hammer_gap_max, now - silent_since);
            silent_since = -1.0f;
            // One hammering ends and the next begins in the same update: its age starts over.
            if (heard[0].age < last_age || last_age < 0.0f) ++hammer_rounds;
            last_age = heard[0].age;
        }
        once_ok = above_loop && hammer_first >= hammer_delay[0] - kStep && hammer_first <= hammer_delay[1] + 2.0f * kStep && hammer_rounds >= 3 &&
                  hammer_gap_max <= 2.0f * kStep;
    }
    rounds_ok = rounds_ok && wind_ok && once_ok;

    const bool pass = pause_ok && frontend_ok && layers_ok && budget_ok && restart_ok && rounds_ok;
    std::cout << std::fixed << std::setprecision(4) << "  -> Stage 26 Result: " << (pass ? "PASS" : "FAIL") << " (Pause=" << (pause_ok ? "OK" : "FAIL")
              << " [" << paused_voices << " voices held for 60 s, " << changed << " changed; an impact's age " << age_before << " s before, "
              << age_in_pause << " in it, " << age_after << " 0.04 s after; " << started_in_pause << " sound started in it (the menu's click: "
              << (click_played ? "yes" : "no") << "), " << started_after << " after; the echo's delay held: " << (delay_held ? "yes" : "no")
              << "; InGameMusic " << music_at_start << " at first, " << music_paused << " paused, " << music_after << " after], FrontEnd="
              << (frontend_ok ? "OK" : "FAIL") << " [" << behind_menu << " voices behind it, " << back_in_play << " back in play], Layers="
              << (layers_ok ? "OK" : "FAIL") << " [A_Prop_AirVent_Indoor from 100 uu: " << vent_near << " voices, distance gains " << vent_gain[0]
              << " and " << vent_gain[1] << ", ranked at " << vent_rank[0] << " and " << vent_rank[1] << " (mix group " << vent_mix
              << "); from 1900 uu: " << vent_far << " at " << vent_far_gain << ", high frequencies " << vent_far_hf << "; from 2100 uu: "
              << vent_past << "], Budget=" << (budget_ok ? "OK" : "FAIL") << " [The Boat: " << budget << " sources, up to " << most_asking
              << " waves asking, " << places_over << " places with more asking than sources, " << misranked << " misranked, " << wrong_count
              << " miscounted; with three impacts playing " << sourced_with_impacts << " of " << budget_with_impacts << "; "
              << before_load << " voices of the level before, " << after_load << " once this one was loaded], NoRestart="
              << (restart_ok ? "OK" : "FAIL") << " [" << kept << " voices kept their source over 20 steps, " << restarted << " restarted, "
              << rank_changes << " places in the ranking changed, " << newcomers << " new voices], Rounds=" << (rounds_ok ? "OK" : "FAIL")
              << " [VehiclePack_02: " << pack_rounds << " rounds, waits " << pack_gap_min << " to "
              << pack_gap_max << " s, " << pack_clips << " different passes, " << pack_looped << " looped, " << pack_wait_after_return
              << " s to start after 40 s unheard; WindHard: " << wind_beds << " looping bed, " << wind_gust_clips
              << " different gusts in 60 s; DullHammering: first after " << hammer_first << " s (its delay " << hammer_delay[0] << " to "
              << hammer_delay[1] << "), " << hammer_rounds << " rounds, longest wait between them " << hammer_gap_max << " s])"
              << std::defaultfloat << std::setprecision(6) << std::endl;
    return pass;
}

// -----------------------------------------------------------------------------
// --audio-device-check: the audio engine on a real audio device, with no window and its master
// volume at nothing, so that everything plays and nothing is heard. It says what the device gives
// (the emitters' sources, the EFX low-pass filters) and asks OpenAL itself what a paused game does
// to its sources: the emitters', a bullet impact's and the run wind's paused, the music's playing
// on, the menu's click starting under the pause, all of them playing again after it.
// -----------------------------------------------------------------------------
static int run_audio_device_check(const std::string& game_root) {
    using namespace me;
    AudioEngine audio;
    audio.init(false);
    if (!audio.device_report().device) {
        std::cout << "[AudioDevice] no audio device (or a build without OpenAL): nothing checked" << std::endl;
        return 1;
    }
    audio.set_master_volume(0.0f);
    audio.load_stock_audio(game_root);
    audio.load_level_audio(game_root, "Maps/SP00/Tutorial_p.me1");
    Vec3 ear(0.0f, 0.0f, 0.0f);
    for (const AmbientEmitterInfo& em : audio.get_ambient_emitters()) {
        if (em.cue_name != "WindHard") continue;
        ear = em.location + Vec3(0.0f, 0.0f, 150.0f);
        break;
    }
    const Vec3 ahead(1.0f, 0.0f, 0.0f), up(0.0f, 0.0f, 1.0f);
    // Frames of a sixtieth of a second, in real time: the device plays what it is given.
    const auto run = [&](int frames) {
        for (int i = 0; i < frames; ++i) {
            audio.update(1.0f / 60.0f, ear, ahead, up, 0.0f, false);
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    };
    const auto say = [](const char* when, const AudioEngine::DeviceReport& r) {
        std::cout << "[AudioDevice] " << when << ": emitter sources " << r.emitters_playing << " playing, " << r.emitters_paused << " paused; pool "
                  << r.pool_playing << " playing, " << r.pool_paused << " paused; music stems " << r.music_playing << " playing; run wind "
                  << (r.wind_paused ? "paused" : "not paused") << "; OpenAL error: " << (r.error ? "yes" : "no") << std::endl;
    };
    audio.set_game_paused(false);
    run(30);
    audio.play_sound_3d("Concrete.9mm_Concrete_Impact", ear + Vec3(300.0f, 0.0f, 0.0f));  // 1.35 s
    run(6);
    const AudioEngine::DeviceReport play = audio.device_report();
    std::cout << "[AudioDevice] device '" << play.name << "': " << play.emitter_sources << " emitter sources, low-pass filters (EFX): "
              << (play.low_pass ? "yes" : "no") << std::endl;
    float lowest_hf = 1.0f;
    for (const AudioEngine::PositionalVoice& v : audio.positional_voices()) lowest_hf = std::min(lowest_hf, v.high_frequency_gain);
    std::cout << "[AudioDevice] " << audio.positional_voices().size() << " sounds at a place, the dullest with its filter's high frequencies at "
              << lowest_hf << std::endl;
    say("in play", play);
    audio.set_game_paused(true);
    run(30);
    const AudioEngine::DeviceReport paused = audio.device_report();
    say("paused for half a second", paused);
    audio.play_sound("A_Pos");                          // the menu's click, a UI sound
    audio.play_sound("Concrete._03_Female_FootStepRun");  // and a footstep, which waits
    run(2);
    const AudioEngine::DeviceReport click = audio.device_report();
    say("a click and a footstep asked for", click);
    audio.set_game_paused(false);
    run(3);
    const AudioEngine::DeviceReport after = audio.device_report();
    say("in play again", after);
    const bool pass = play.emitters_playing > 0 && play.emitters_paused == 0 && play.pool_playing >= 1 && play.music_playing >= 1 && !play.error &&
                      paused.emitters_playing == 0 && paused.emitters_paused > 0 && paused.pool_playing == 0 && paused.pool_paused >= 1 &&
                      paused.music_playing == play.music_playing && paused.wind_paused && !paused.error && click.pool_playing >= 1 &&
                      click.pool_paused == paused.pool_paused && !click.error && after.emitters_playing > 0 && after.emitters_paused == 0 &&
                      after.pool_paused == 0 && after.pool_playing >= 2 && !after.wind_paused && !after.error;
    std::cout << "[AudioDevice] " << (pass ? "PASS" : "FAIL") << std::endl;
    audio.shutdown();
    return pass ? 0 : 1;
}

// -----------------------------------------------------------------------------
// --emitter-walk: the level emitters of the eleven maps, heard with no audio device from 100, 150,
// 1000 and 2000 uu above each of them in turn, a quarter of a second at every place: how many
// emitter voices sound there, how many of those are at a distance gain of nothing or next to it,
// how many emitters they belong to, and how many waves asked for a source (the ones with one and
// the ones ranked out). The walk of MODLOG.md sections 38 and 41.
// -----------------------------------------------------------------------------
static int run_emitter_walk(const std::string& game_root) {
    using namespace me;
    static const char* const kMaps[] = {
        "Maps/SP00/Tutorial_p.me1",   "Maps/SP01/Edge_p.me1",    "Maps/SP01/Escape_p.me1", "Maps/SP02/Stormdrain_p.me1",
        "Maps/SP03/Cranes_p.me1",     "Maps/SP04/Subway_p.me1",  "Maps/SP05/Mall_p.me1",   "Maps/SP06/Factory_p.me1",
        "Maps/SP07/Boat_p.me1",       "Maps/SP08/Convoy_p.me1",  "Maps/SP09/Scraper_p.me1"};
    constexpr size_t kMost = 40;  // the histogram's last column: that many voices or more
    struct Count {
        size_t emitters = 0, places = 0, voices = 0, silent = 0, quiet = 0, heard_emitters = 0, most = 0;
        size_t by_voices[kMost + 1] = {};
        size_t asked = 0, most_asked = 0, over_budget = 0, misranked = 0, stopped = 0;
        size_t by_asked[7] = {};  // 0, 1-4, 5-8, 9-16, 17-24, 25-32, more
    };
    AudioEngine audio;
    audio.init(true);
    audio.load_stock_audio(game_root);
    const Vec3 ahead(1.0f, 0.0f, 0.0f), up(0.0f, 0.0f, 1.0f);
    Count total;
    const auto print = [](const char* name, const Count& c) {
        std::cout << "[Walk] " << name << ": " << c.emitters << " emitters, " << c.places << " places, " << c.voices << " emitter voices sound ("
                  << c.silent << " at distance gain 0, " << c.quiet << " at 0.002 or less), of " << c.heard_emitters
                  << " emitters; most at one place " << c.most << "; places by voices";
        for (size_t n = 0; n <= kMost; ++n) {
            if (c.by_voices[n]) std::cout << " " << n << (n == kMost ? "+" : "") << ":" << c.by_voices[n];
        }
        static const char* const kAsked[] = {"0", "1-4", "5-8", "9-16", "17-24", "25-32", "33+"};
        std::cout << "; " << c.asked << " waves asked for a source, most at one place " << c.most_asked << ", places by waves asking";
        for (size_t n = 0; n < std::size(kAsked); ++n) {
            if (c.by_asked[n]) std::cout << " " << kAsked[n] << ":" << c.by_asked[n];
        }
        std::cout << "; " << c.over_budget << " places with more asking than there are sources, " << c.misranked
                  << " waves with a source ranked under one without; " << c.stopped << " emitters stopped for good" << std::endl;
    };
    for (const char* map : kMaps) {
        audio.stop_all();
        audio.load_level_audio(game_root, map);
        const std::vector<AmbientEmitterInfo> emitters = audio.get_ambient_emitters();
        Count c;
        c.emitters = emitters.size();
        for (const float height : {100.0f, 150.0f, 1000.0f, 2000.0f}) {
            for (const AmbientEmitterInfo& em : emitters) {
                audio.update(0.25f, em.location + Vec3(0.0f, 0.0f, height), ahead, up, 0.0f, false);
                size_t voices = 0;
                std::vector<Vec3> from;
                for (const AudioEngine::PositionalVoice& v : audio.positional_voices()) {
                    if (!v.emitter) continue;
                    ++voices;
                    if (v.distance_gain <= 0.0f) ++c.silent;
                    if (v.distance_gain <= 0.002f) ++c.quiet;
                    bool known = false;
                    for (const Vec3& p : from) known = known || (p - v.position).length() < 0.01f;
                    if (!known) from.push_back(v.position);
                }
                ++c.places;
                c.voices += voices;
                c.heard_emitters += from.size();
                c.most = std::max(c.most, voices);
                ++c.by_voices[std::min(voices, kMost)];
                // The waves that asked for a source, and whether the ones that got one outrank the rest.
                const std::vector<AudioEngine::PositionalVoice> asking = audio.emitter_candidates();
                float lowest_with = 1.0e9f, highest_without = -1.0f;
                for (const AudioEngine::PositionalVoice& v : asking) {
                    if (v.sourced) lowest_with = std::min(lowest_with, v.priority);
                    else highest_without = std::max(highest_without, v.priority);
                }
                for (const AudioEngine::PositionalVoice& v : asking) {
                    if (v.sourced && v.priority < highest_without) ++c.misranked;
                }
                c.asked += asking.size();
                c.most_asked = std::max(c.most_asked, asking.size());
                if (asking.size() > voices) ++c.over_budget;
                ++c.by_asked[asking.empty() ? 0 : asking.size() <= 4 ? 1 : asking.size() <= 8 ? 2 : asking.size() <= 16 ? 3
                                             : asking.size() <= 24 ? 4 : asking.size() <= 32 ? 5 : 6];
            }
        }
        c.stopped = audio.stopped_emitter_count();
        print(map, c);
        total.emitters += c.emitters;
        total.places += c.places;
        total.voices += c.voices;
        total.silent += c.silent;
        total.quiet += c.quiet;
        total.heard_emitters += c.heard_emitters;
        total.most = std::max(total.most, c.most);
        for (size_t n = 0; n <= kMost; ++n) total.by_voices[n] += c.by_voices[n];
        total.asked += c.asked;
        total.most_asked = std::max(total.most_asked, c.most_asked);
        total.over_budget += c.over_budget;
        total.misranked += c.misranked;
        total.stopped += c.stopped;
        for (size_t n = 0; n < std::size(c.by_asked); ++n) total.by_asked[n] += c.by_asked[n];
    }
    print("all eleven maps", total);
    return 0;
}

// -----------------------------------------------------------------------------
// --dump-sound-cues <file>: every SoundCue of every package under CookedPC as the loader reads it,
// one JSON object a line: the cue's attenuation nodes (class, model, the stored radius pairs, flags,
// where each sits in the graph) and how many of its waves have none, one or two of them above. For
// holding the loader against a scan of the cooked data (tools/retail/attenuation_check.py).
// -----------------------------------------------------------------------------
static int run_dump_sound_cues(const std::string& game_root, const std::string& out_path) {
    using namespace me;
    const fs::path cooked = fs::path(game_root) / "TdGame" / "CookedPC";
    std::vector<fs::path> files;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(cooked, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file()) continue;
        const std::string ext = lower(it->path().extension().string());
        if (ext == ".upk" || ext == ".me1") files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    std::ofstream out(out_path, std::ios::binary);
    if (!out.is_open()) {
        std::cerr << "[Cues] cannot write " << out_path << std::endl;
        return 1;
    }
    out << std::setprecision(9);
    const auto text = [](const std::string& s) {
        std::string quoted = "\"";
        for (char c : s) {
            if (c == '"' || c == '\\') quoted += '\\';
            quoted += c;
        }
        return quoted + "\"";
    };
    size_t packages = 0, cue_count = 0, node_count = 0, emitter_count = 0;
    for (const fs::path& file : files) {
        const UPKPackage pkg(file.string());
        if (!pkg.is_valid()) {
            std::cerr << "[Cues] not read: " << file.string() << std::endl;
            continue;
        }
        ++packages;
        std::vector<SoundCueDef> cues;
        std::vector<AmbientEmitterInfo> emitters;
        pkg.extract_sound_cues_and_ambients(cues, emitters);
        const std::string rel = fs::relative(file, cooked, ec).generic_string();
        for (const SoundCueDef& cue : cues) {
            // Where each attenuation node is first met going down the graph (its depth and the
            // attenuation nodes above it there), and the waves by how many are above them.
            std::vector<int> depth(cue.attenuations.size(), -1);
            std::vector<std::vector<int>> above(cue.attenuations.size());
            int waves[3] = {0, 0, 0};
            std::vector<int> path;
            const auto walk = [&](const auto& self, int node, int level) -> void {
                if (node < 0 || static_cast<size_t>(node) >= cue.nodes.size()) return;
                const SoundCueNode& n = cue.nodes[static_cast<size_t>(node)];
                if (n.kind == SoundCueNode::Kind::Wave) {
                    ++waves[std::min<size_t>(path.size(), 2)];
                    return;
                }
                const bool attenuates = n.attenuation >= 0 && static_cast<size_t>(n.attenuation) < cue.attenuations.size();
                if (attenuates && depth[static_cast<size_t>(n.attenuation)] < 0) {
                    depth[static_cast<size_t>(n.attenuation)] = level;
                    above[static_cast<size_t>(n.attenuation)] = path;
                }
                if (attenuates) path.push_back(n.attenuation);
                for (int child : n.children) self(self, child, level + 1);
                if (attenuates) path.pop_back();
            };
            if (!cue.nodes.empty()) walk(walk, 0, 0);
            out << "{\"pkg\":" << text(rel) << ",\"path\":" << text(cue.full_path) << ",\"group\":" << text(cue.sound_group)
                << ",\"vol\":" << cue.volume_multiplier << ",\"dur\":" << cue.duration << ",\"looping\":" << (cue.looping ? "true" : "false")
                << ",\"mixer\":" << (cue.has_mixer ? "true" : "false") << ",\"waves\":[" << waves[0] << "," << waves[1] << "," << waves[2]
                << "],\"att\":[";
            for (size_t k = 0; k < cue.attenuations.size(); ++k) {
                const SoundAttenuation& a = cue.attenuations[k];
                const auto pair = [&](const char* name, const float* v) { out << ",\"" << name << "\":[" << v[0] << "," << v[1] << "]"; };
                out << (k ? "," : "") << "{\"n\":" << a.export_index << ",\"c\":\""
                    << (a.td ? "TdSoundNodeAttenuation" : a.linear_only ? "SoundNodeAmbient" : "SoundNodeAttenuation")
                    << "\",\"model\":" << static_cast<int>(a.model);
                pair("min", a.min_radius);
                pair("max", a.max_radius);
                pair("lpfmin", a.lpf_min_radius);
                pair("lpfmax", a.lpf_max_radius);
                pair("speed", a.speed_of_sound);
                out << ",\"db\":" << a.db_at_max << ",\"attenuate\":" << (a.attenuate ? "true" : "false") << ",\"spatialize\":"
                    << (a.spatialize ? "true" : "false") << ",\"lowpass\":" << (a.lowpass ? "true" : "false") << ",\"delay\":"
                    << (a.delay ? "true" : "false") << ",\"depth\":" << depth[k] << ",\"above\":[";
                for (size_t j = 0; j < above[k].size(); ++j) {
                    out << (j ? "," : "") << cue.attenuations[static_cast<size_t>(above[k][j])].export_index;
                }
                out << "]}";
                ++node_count;
            }
            out << "]}\n";
            ++cue_count;
        }
        for (const AmbientEmitterInfo& em : emitters) {
            out << "{\"pkg\":" << text(rel) << ",\"emitter\":" << text(em.cue_name) << ",\"wave\":" << text(em.wave_name) << ",\"at\":["
                << em.location.x << "," << em.location.y << "," << em.location.z << "],\"vol\":" << em.volume << "}\n";
            ++emitter_count;
        }
    }
    std::cout << "[Cues] " << packages << " packages of " << files.size() << ": " << cue_count << " sound cues, " << node_count
              << " attenuation nodes, " << emitter_count << " ambient sound actors -> " << out_path << std::endl;
    return 0;
}


// The player's bullets against an enemy's posed bodies (physics/body_shapes.hpp,
// ParkourController::pose_enemies_for_shot and trace_enemies), on the Training Area's combat terrace
// with the renderer's own skeletons. Oracle stage 24; --verify-bodies runs it alone.
//   Asset   the bodies a posed bot carries against the sizes read out of the package
//           (CH_TKY_Cop_SWAT.Male3p_Physics: 16 bodies, 7 boxes, 10 capsules), on every archetype,
//           and how far from where he stands they reach in any state and sequence the controller
//           gives a bot, a dead one's included
//   Lines   a few thousand lines through a posed bot from three sides, each against the same
//           shapes walked in steps of a twentieth of a unit, the two compared across the shapes'
//           surfaces
//   Rays    lines with known answers, on his bodies and on the cylinder that stands in for them
//   Pulls   the controller's own trigger pulls: the damage, what the tracer carries, how many bots
//           a pull poses, a shotgun's pellets after the one that kills, a dead bot, the cylinder
//           when nothing poses them, the tutorial's bot
// -----------------------------------------------------------------------------
static bool oracle_enemy_bodies(me::Renderer& renderer, me::LevelScene& scene, const me::MovementConfig& move_cfg) {
    using namespace me;
    constexpr float kDt = 1.0f / 60.0f;
    const std::vector<EnemyBot> enemies_before = scene.enemies;
    const std::vector<BulletTracer> tracers_before = scene.active_tracers;
    const std::vector<DroppedWeapon> dropped_before = scene.dropped_weapons;

    int posed_calls = 0;
    const EnemyBodyPoser poser = [&](const EnemyBot& bot, float sim_time, bool reaction_disarm, EnemyBodySet& out) {
        ++posed_calls;
        return renderer.pose_enemy_bodies(bot, sim_time, reaction_disarm, out);
    };
    ParkourController controller(move_cfg);
    controller.set_enemy_body_poser(poser);
    // What the bots fire back is stage 27's: here they hold their fire (it would kill her in a
    // few seconds, at 272 uu).
    controller.set_ai_hold_fire(true);

    // She stands at the terrace's checkpoint and the bot where the tutorial's partner does, facing
    // her (stage 7's ground): his forward is -Y, his right +X.
    const Vec3 stand(751.8f, -1591.7f, 4992.0f), bot_at(751.8f, -1320.0f, 4992.0f);
    const Vec3 fwd(0.0f, -1.0f, 0.0f), right(1.0f, 0.0f, 0.0f), up(0.0f, 0.0f, 1.0f);
    const auto make_bot = [&](const char* archetype, const char* weapon) {
        EnemyBot bot;
        bot.archetype = archetype;
        bot.weapon_name = weapon;
        bot.position = bot_at;
        bot.home_position = bot_at;
        bot.yaw_deg = -90.0f;
        bot.anim_state = EEnemyAnimState::AimFire;
        bot.has_shield = template_carries_shield(find_aim_template(archetype));  // the riot cop's
        return bot;
    };
    bool pass = true;
    std::cout << std::fixed << std::setprecision(2);

    // --- Asset -----------------------------------------------------------------------------------
    // A box's X, Y, Z and a capsule's Radius and Length as the package has them
    // (build/re/impacts/research/bodies/physasset.py Characters/CH_TKY_Cop_SWAT.upk Male3p_Physics).
    struct Stored {
        uint8_t body;
        const char* bone;
        EBodyShape kind;
        float a, b, c;
        bool head;  // PM_Character_Head
    };
    static const Stored kStored[] = {
        {0, "Hips", EBodyShape::Box, 13.000f, 39.941f, 20.976f, false},
        {0, "Hips", EBodyShape::Capsule, 9.001f, 28.941f, 0.0f, false},
        {1, "Spine1", EBodyShape::Box, 21.008f, 35.325f, 25.197f, false},
        {2, "Spine2", EBodyShape::Capsule, 12.029f, 21.499f, 0.0f, false},
        {3, "Neck", EBodyShape::Capsule, 12.745f, 6.573f, 0.0f, true},
        {4, "LeftArm", EBodyShape::Capsule, 9.945f, 15.000f, 0.0f, false},
        {5, "LeftForeArm", EBodyShape::Box, 17.995f, 12.976f, 13.555f, false},
        {6, "LeftHand", EBodyShape::Capsule, 4.402f, 15.000f, 0.0f, true},
        {7, "RightArm", EBodyShape::Capsule, 9.871f, 15.000f, 0.0f, false},
        {8, "RightForeArm", EBodyShape::Box, 17.415f, 12.647f, 13.597f, false},
        {9, "RightHand", EBodyShape::Capsule, 3.382f, 15.000f, 0.0f, true},
        {10, "LeftUpLeg", EBodyShape::Capsule, 11.880f, 25.933f, 0.0f, false},
        {11, "LeftLeg", EBodyShape::Capsule, 10.265f, 28.940f, 0.0f, false},
        {12, "LeftFoot", EBodyShape::Box, 15.007f, 29.344f, 7.500f, false},
        {13, "RightUpLeg", EBodyShape::Box, 19.406f, 24.178f, 41.414f, false},
        {14, "RightLeg", EBodyShape::Capsule, 9.803f, 20.993f, 0.0f, false},
        {15, "RightFoot", EBodyShape::Box, 14.472f, 30.596f, 8.512f, false},
    };
    scene.enemies = {make_bot("Assault_SWAT", "G36C")};
    EnemyBodySet set;
    const bool posed_ok = renderer.pose_enemy_bodies(scene.enemies[0], 0.0f, false, set) && set.valid;
    bool asset_ok = posed_ok && set.bodies.size() == 16 && set.shapes.size() == std::size(kStored);
    float worst_size = 0.0f;
    for (size_t i = 0; asset_ok && i < std::size(kStored); ++i) {
        const Stored& want = kStored[i];
        const BodyShape& s = set.shapes[i];
        asset_ok = s.kind == want.kind && s.body == want.body && set.bodies[s.body].bone == want.bone &&
                   (set.bodies[s.body].surface == ECharacterSurface::Head) == want.head;
        if (s.kind == EBodyShape::Box) {
            worst_size = std::max({worst_size, std::abs(s.half.x * 2.0f - want.a), std::abs(s.half.y * 2.0f - want.b), std::abs(s.half.z * 2.0f - want.c)});
        } else {
            worst_size = std::max({worst_size, std::abs(s.half.x - want.a), std::abs(s.half.z * 2.0f - want.b)});
        }
    }
    asset_ok = asset_ok && worst_size < 0.001f;
    // Every archetype the port draws, in every state the controller puts a bot in and with every
    // sequence it names for him there (ParkourController writes EnemyBot::active_anim_seq: a blow's
    // HitMelee*, a bullet's stumble, the disarm's Snatch*; none is the state's own), with a light weapon's
    // set of sequences and a heavy one's, at forty moments: the bodies found, and their reach. A
    // dead bot keeps the sequence he died with and is drawn, and met, at its end.
    static const char* const kArchetypes[] = {"Assault_SWAT", "PatrolCop", "Support", "RiotCop", "PursuitCop", "Celeste",
                                              "Kate",         "Jacknife",  "Ropeburn", "Miller",  "Kreeg"};
    static const char* const kOwn[] = {""};
    static const char* const kBlows[] = {"",                    "HitMeleeRight",      "HitMeleeLeft",  "HitMeleeSoccerKick",
                                         "HitMeleeCrouchSweep", "HitMeleeInAir_High", "HitMeleeSlide", "HitMeleeWallrunRight"};
    static const char* const kSnatches[] = {"", "SnatchFwd", "SnatchBack"};
    // A bullet's stumbles (TdMove_StumbleBot; physics/bot_fire.hpp), each played from its start.
    static const char* const kStumbles[] = {"WalkBwdStop", "WalkFwdStop", "HitHeavyStomach", "HitHeavyHead"};
    static const uint8_t kStumbleStates[] = {14, 16, 17, 18};
    static const char* const kDeaths[] = {"",                   "HitMeleeRight", "HitMeleeLeft",         "HitMeleeSoccerKick", "HitMeleeCrouchSweep",
                                          "HitMeleeInAir_High", "HitMeleeSlide", "HitMeleeWallrunRight", "SnatchFwd",          "SnatchBack"};
    struct Played {
        const char* name;
        EEnemyAnimState state;
        bool stunned, alive;
        const char* const* sequences;
        size_t sequence_count;
    };
    static const Played kPlayed[] = {
        {"idle", EEnemyAnimState::Idle, false, true, kOwn, std::size(kOwn)},
        {"walk", EEnemyAnimState::Patrol, false, true, kOwn, std::size(kOwn)},
        {"run", EEnemyAnimState::Chase, false, true, kOwn, std::size(kOwn)},
        {"aim", EEnemyAnimState::AimFire, false, true, kOwn, std::size(kOwn)},
        {"windup", EEnemyAnimState::MeleeWindup, false, true, kOwn, std::size(kOwn)},
        {"strike", EEnemyAnimState::MeleeStrike, false, true, kOwn, std::size(kOwn)},
        {"stagger", EEnemyAnimState::HitStagger, true, true, kBlows, std::size(kBlows)},
        {"stumble", EEnemyAnimState::HitStagger, true, true, kStumbles, std::size(kStumbles)},
        {"disarm", EEnemyAnimState::BeingDisarmed, true, true, kSnatches, std::size(kSnatches)},
        {"dead", EEnemyAnimState::KnockedOut, true, false, kDeaths, std::size(kDeaths)},
    };
    std::string archetype_note, reach_note, state_note[std::size(kPlayed)];
    float reach = 0.0f, state_reach[std::size(kPlayed)] = {}, sequence_reach[std::size(kPlayed)][std::size(kDeaths)] = {};
    int poses = 0;
    for (const char* archetype : kArchetypes) {
        size_t shapes = 0;
        for (size_t state = 0; state < std::size(kPlayed); ++state) {
            const Played& played = kPlayed[state];
            for (size_t q = 0; q < played.sequence_count; ++q) {
                for (int k = 0; k < 80; ++k) {
                    EnemyBot bot = make_bot(archetype, k % 2 ? "G36C" : "Colt1911");
                    bot.anim_state = played.state;
                    bot.alive = played.alive;
                    bot.stunned = played.stunned;
                    bot.active_anim_seq = played.sequences[q];
                    bot.anim_timer = static_cast<float>(k / 2) * 0.025f;
                    if (played.sequences == kStumbles) {
                        bot.combat.stumble = kStumbleStates[q];
                        bot.anim_duration = bullet_stumble_anim(kStumbleStates[q]).length;
                    }
                    EnemyBodySet s;
                    if (!renderer.pose_enemy_bodies(bot, static_cast<float>(k / 2) * 0.0437f, false, s)) continue;
                    ++poses;
                    shapes = s.shapes.size();
                    float far = 0.0f;
                    for (const BodyShape& b : s.shapes) {
                        far = std::max(far, (b.centre - bot.position).length() + (b.kind == EBodyShape::Box ? b.half.length() : b.half.x + b.half.z));
                    }
                    sequence_reach[state][q] = std::max(sequence_reach[state][q], far);
                    if (far > state_reach[state]) {
                        state_reach[state] = far;
                        state_note[state] = played.sequences[q];
                    }
                    if (far > reach) {
                        reach = far;
                        reach_note = std::string(archetype) + ", " + played.name + (played.sequences[q][0] ? std::string(" with ") + played.sequences[q] : std::string());
                    }
                }
            }
        }
        archetype_note += std::string(archetype_note.empty() ? "" : ", ") + archetype + " " + std::to_string(shapes);
        asset_ok = asset_ok && shapes == std::size(kStored);
    }
    asset_ok = asset_ok && reach < kEnemyBodyReach;
    double pose_us = 0.0;
    {
        const auto t0 = std::chrono::steady_clock::now();
        for (int k = 0; k < 2000; ++k) renderer.pose_enemy_bodies(scene.enemies[0], static_cast<float>(k) * 0.01f, false, set);
        pose_us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 2000.0;
    }
    std::cout << "  [Asset] " << (asset_ok ? "OK" : "FAIL") << ": " << set.bodies.size() << " bodies, " << set.shapes.size()
              << " shapes on a posed bot, sizes within " << std::setprecision(5) << worst_size << std::setprecision(2)
              << " of the package's; shapes by archetype: " << archetype_note << "; furthest reach from where he stands over " << poses
              << " poses " << reach << " uu (" << reach_note << "; the first reject allows " << kEnemyBodyReach << "), by state:";
    for (size_t k = 0; k < std::size(kPlayed); ++k) {
        std::cout << " " << kPlayed[k].name << " " << state_reach[k];
        if (!state_note[k].empty()) std::cout << " (" << state_note[k] << ")";
    }
    std::cout << std::endl;
    if (std::getenv("ME_BODIES_DEBUG")) {
        // The reach by sequence, in the states that take one from the controller.
        for (size_t k = 0; k < std::size(kPlayed); ++k) {
            if (kPlayed[k].sequence_count < 2) continue;
            std::cout << "  reach, " << kPlayed[k].name << ":";
            for (size_t q = 0; q < kPlayed[k].sequence_count; ++q) {
                std::cout << " " << (kPlayed[k].sequences[q][0] ? kPlayed[k].sequences[q] : "(its own)") << " " << sequence_reach[k][q];
            }
            std::cout << std::endl;
        }
    }
    pass = pass && asset_ok;
    if (!posed_ok) {
        std::cout << "  -> Stage 24 Result: FAIL (no bot could be posed: the character assets are missing)" << std::endl;
        scene.enemies = enemies_before;
        return false;
    }

    // --- Lines -----------------------------------------------------------------------------------
    // The bot as the controller poses him for a pull, and what a pull's lines meet, against the
    // shapes walked point by point.
    controller.reset(stand, 90.0f);
    std::vector<EnemyBodySet> posed;
    controller.pose_enemies_for_shot(scene, stand + up * 150.0f, fwd * -1.0f, 4000.0f, 0.0f, posed);
    set = posed[0];
    // How far a point is from a shape's surface, less than nothing inside it. In doubles, so that
    // the walk has no rounding of its own to answer for.
    const auto off_surface = [](const BodyShape& s, double px, double py, double pz) {
        const double rx = px - s.centre.x, ry = py - s.centre.y, rz = pz - s.centre.z;
        const auto along = [&](const Vec3& axis) { return rx * axis.x + ry * axis.y + rz * axis.z; };
        const double x = along(s.axis[0]), y = along(s.axis[1]), z = along(s.axis[2]);
        if (s.kind == EBodyShape::Box) {
            const double ex = std::abs(x) - s.half.x, ey = std::abs(y) - s.half.y, ez = std::abs(z) - s.half.z;
            const double ox = std::max(ex, 0.0), oy = std::max(ey, 0.0), oz = std::max(ez, 0.0);
            return std::sqrt(ox * ox + oy * oy + oz * oz) + std::min(std::max({ex, ey, ez}), 0.0);
        }
        const double past = z - std::clamp(z, -static_cast<double>(s.half.z), static_cast<double>(s.half.z));
        return std::sqrt(x * x + y * y + past * past) - s.half.x;
    };
    constexpr double kStep = 0.05;
    // The trace and the walk are held against each other across a shape's surface, not along the
    // line: where a line that grazes a shape goes into it moves along the line by many times a
    // float's last bit (half a thousandth of a unit at these coordinates), across the surface by
    // no more than that bit. A hundredth of a unit is the skin within which the two may differ.
    constexpr double kSkin = 0.01;
    // The walk: how deep in a shape of `in` the points of a line get, on all of it, and on the part
    // of it short of `before`.
    const auto walk = [&](const EnemyBodySet& in, const Vec3& from, const Vec3& dir, float length, float before, double& deepest_before) {
        double deepest = 0.0;
        deepest_before = 0.0;
        for (const BodyShape& s : in.shapes) {
            const double round = (s.kind == EBodyShape::Box ? s.half.length() : s.half.x + s.half.z) + 1.0;
            const double mid = (s.centre - from).dot(dir);
            const int first = std::max(0, static_cast<int>(std::ceil((mid - round) / kStep)));
            const int last = static_cast<int>(std::floor(std::min(static_cast<double>(length), mid + round) / kStep));
            for (int k = first; k <= last; ++k) {
                const double t = static_cast<double>(k) * kStep;
                const double deep = -off_surface(s, from.x + dir.x * t, from.y + dir.y * t, from.z + dir.z * t);
                deepest = std::max(deepest, deep);
                if (t < before) deepest_before = std::max(deepest_before, deep);
            }
        }
        return deepest;
    };
    int lines = 0, same_hit = 0, same_miss = 0, grazes = 0, wrong = 0, bad_normals = 0;
    double worst_entry = 0.0, worst_before = 0.0;
    const auto check_line = [&](const Vec3& from, const Vec3& dir, float length) {
        ++lines;
        EnemyBodyHit hit;
        const bool met = trace_enemy_bodies(set, from, dir, length, hit);
        double deepest_before = 0.0;
        const double deepest = walk(set, from, dir, length, met ? hit.distance : 0.0f, deepest_before);
        if (!met) {
            // Nothing met: right unless the walk gets under a shape's skin.
            if (deepest > kSkin) {
                ++wrong;
            } else {
                deepest > 0.0 ? ++grazes : ++same_miss;
            }
            return;
        }
        // Met: right when the place is on the surface of the shape named and the walk is under no
        // shape's skin before it.
        const BodyShape& s = set.shapes[static_cast<size_t>(hit.shape)];
        const double at_entry = off_surface(s, hit.location.x, hit.location.y, hit.location.z);
        if (std::abs(at_entry) > kSkin || deepest_before > kSkin) {
            ++wrong;
            return;
        }
        worst_entry = std::max(worst_entry, std::abs(at_entry));
        worst_before = std::max(worst_before, deepest_before);
        // The normal: of length one, against the line, and straight out of the shape: ten units
        // along it from the place are ten units further from the surface.
        const double out = off_surface(s, hit.location.x + 10.0 * hit.normal.x, hit.location.y + 10.0 * hit.normal.y, hit.location.z + 10.0 * hit.normal.z);
        if (std::abs(hit.normal.length() - 1.0f) >= 1.0e-3f || hit.normal.dot(dir) > 1.0e-3f || out - at_entry < 10.0 - kSkin) ++bad_normals;
        // (A line that is under the skin for less than a step, or never, the walk cannot vouch for.)
        deepest > kSkin ? ++same_hit : ++grazes;
    };
    for (int iy = -15; iy <= 15; ++iy) {
        for (int iz = -1; iz <= 50; ++iz) {
            const float y = static_cast<float>(iy) * 4.0f, z = static_cast<float>(iz) * 4.0f;
            const Vec3 through = bot_at + right * y + up * z;
            check_line(through + fwd * 300.0f, fwd * -1.0f, 600.0f);                               // front on
            check_line(bot_at + fwd * y + up * z + right * 300.0f, right * -1.0f, 600.0f);          // side on
            const Vec3 slant = (fwd * -1.0f + right * 0.45f - up * 0.6f).normalized();              // from above and aside
            check_line(through - slant * 300.0f, slant, 600.0f);
        }
    }
    const bool lines_ok = wrong == 0 && bad_normals == 0 && same_hit > 500 && same_miss > 500;
    std::cout << "  [Lines] " << (lines_ok ? "OK" : "FAIL") << ": " << lines << " lines: " << same_hit << " go into a shape as the walk does (the place within "
              << std::setprecision(4) << worst_entry << " uu of that shape's surface, the walk no deeper than " << worst_before << std::setprecision(2)
              << " in any shape before it), " << same_miss << " meet nothing either way, " << grazes << " graze a shape (in it for less than a step, or no deeper than "
              << kSkin << "), " << wrong << " differ; " << bad_normals << " wrong normals" << std::endl;
    pass = pass && lines_ok;
    if (std::getenv("ME_BODIES_DEBUG")) {
        // What lines straight at him meet, front on and from his right, every 2 uu across and 4 up:
        // the body's number in the asset as a hexadecimal digit.
        for (int view = 0; view < 2; ++view) {
            std::cout << (view == 0 ? "  from in front (his right to the left of the page):" : "  from his right (his front to the right of the page):") << std::endl;
            for (int iz = 48; iz >= 0; --iz) {
                std::string row;
                for (int iy = -30; iy <= 30; ++iy) {
                    const float y = static_cast<float>(iy) * 2.0f, z = static_cast<float>(iz) * 4.0f;
                    EnemyBodyHit h;
                    const bool met = view == 0 ? trace_enemy_bodies(set, bot_at - right * y + up * z + fwd * 300.0f, fwd * -1.0f, 600.0f, h)
                                               : trace_enemy_bodies(set, bot_at + fwd * y + up * z + right * 300.0f, right * -1.0f, 600.0f, h);
                    row += met ? "0123456789ABCDEF"[h.body & 15] : '.';
                }
                std::cout << "    " << std::setw(3) << iz * 4 << " " << row << std::endl;
            }
        }
    }

    // --- Rays ------------------------------------------------------------------------------------
    // Where the rays with known answers are aimed, read off his bodies as they stand: the middle of
    // the head's capsule (the Neck's body), of Spine1's box, of the left hand's capsule, of the right
    // thigh's box; midway between the two shins' capsules; and at the left upper arm's height, 5 uu
    // past its capsule on his left. What each must meet is known from the pictures of the pose
    // (ME_SHOW_BODIES, ME_BODIES_DEBUG).
    enum { kAimHead = 0, kAimBelly, kAimHand, kAimThigh, kAimLegs, kAimShoulder, kAims };
    const auto aim_at = [&](const EnemyBodySet& s, int which) {
        const auto shape = [&](uint8_t body, EBodyShape kind) -> const BodyShape& {
            for (const BodyShape& b : s.shapes) {
                if (b.body == body && b.kind == kind) return b;
            }
            return s.shapes.front();
        };
        if (which == kAimHead) return shape(3, EBodyShape::Capsule).centre;
        if (which == kAimBelly) return shape(1, EBodyShape::Box).centre;
        if (which == kAimHand) return shape(6, EBodyShape::Capsule).centre;
        if (which == kAimThigh) return shape(13, EBodyShape::Box).centre;
        if (which == kAimLegs) return (shape(11, EBodyShape::Capsule).centre + shape(14, EBodyShape::Capsule).centre) * 0.5f;
        const BodyShape& arm = shape(4, EBodyShape::Capsule);
        return arm.centre - right * (arm.half.x + std::abs(arm.axis[2].dot(right)) * arm.half.z + 5.0f);
    };
    struct Known {
        const char* name;
        const char* bone;  // the bone of the body the ray must meet; "" for nothing
        bool head;         // ...and whether that body is PM_Character_Head
        bool pulled;       // also fired at from her eyes, below
    };
    // (From her eyes, 166 over the floor and 270 off, the line to his belly goes through the forearm
    // he holds in front of it and the one between his shins meets the knee in front: those two are
    // level rays only.)
    static const Known kKnown[kAims] = {
        {"the head", "Neck", true, true},
        {"the belly", "Spine1", false, false},
        {"a hand", "LeftHand", true, true},
        {"a thigh", "RightUpLeg", false, true},
        {"between the legs", "", false, false},
        {"just outside a shoulder", "", false, true},
    };
    // From 300 uu in front of him, straight at him: on his bodies, and on the cylinder that stands in
    // for them (48 round his axis, 180 high, the head from 150 up), which every one of these meets.
    bool rays_ok = true;
    for (int a = 0; a < kAims; ++a) {
        const Vec3 at = aim_at(set, a);
        const Vec3 from = at + fwd * 300.0f, dir = fwd * -1.0f;
        const Vec3 rel = at - bot_at;
        ParkourController::EnemyShotHit on_bodies, on_cylinder;
        const bool met = controller.trace_enemies(scene, posed, from, dir, 600.0f, on_bodies);
        const bool met_cylinder = controller.trace_enemies(scene, {}, from, dir, 600.0f, on_cylinder);
        const bool ok = met == (kKnown[a].bone[0] != '\0') &&
                        (!met || (on_bodies.posed && on_bodies.bone == kKnown[a].bone && on_bodies.head == kKnown[a].head)) && met_cylinder &&
                        !on_cylinder.posed && on_cylinder.head == (rel.z >= 150.0f);
        rays_ok = rays_ok && ok;
        std::cout << "  [Rays] " << (ok ? "OK" : "FAIL") << ": at " << kKnown[a].name << " (" << rel.dot(fwd) << " ahead of his feet, " << rel.dot(right)
                  << " to his right, " << rel.z << " up): ";
        if (met) {
            std::cout << "body " << static_cast<int>(on_bodies.body) << " " << on_bodies.bone << (on_bodies.head ? ", PM_Character_Head" : ", PM_Character_Body")
                      << ", " << on_bodies.distance << " uu off, normal (" << on_bodies.normal.x << ", " << on_bodies.normal.y << ", " << on_bodies.normal.z << ")";
        } else {
            std::cout << "nothing";
        }
        std::cout << "; the cylinder: " << (met_cylinder ? (on_cylinder.head ? "met, as the head" : "met, as the body") : "nothing") << std::endl;
    }
    pass = pass && rays_ok;

    // --- Pulls -----------------------------------------------------------------------------------
    // The controller's own fire: she stands on the terrace with a pistol that does not scatter, the
    // view is turned onto a point of him as he stands at that moment and the trigger is pulled for
    // one step.
    controller.equip_weapon("Colt1911");
    InputFrame idle{}, fire{};
    fire.fire = true;
    for (int i = 0; i < 30; ++i) controller.step(idle, kDt, scene);
    struct Pull {
        bool fired = false;
        float damage = 0.0f;
        int tracers = 0, bots_met = 0, posed = 0;
        int unshown = 0;       // of bots_met: those that left nothing to see (pawn_hit 3)
        int through_pose = 0;  // the tracers whose line from her eyes goes into the bodies the first bot had at the pull
        int astray = 0;        // the tracers at odds with those bodies: through one and on, or ended on none
        bool alive = false;    // the first bot, after it
        BulletTracer tracer;   // the last one of the pull
        ParkourController::EnemyShotHit expected;  // what the same line meets, asked just before
        bool expected_met = false;
    };
    // One pull at the aim `which` on the first of `bots`, which are the enemies for it.
    const auto pull = [&](const std::vector<EnemyBot>& bots, int which, bool scatter) {
        // The weapon's interval (a pump-action's is the longest), and the trigger let go.
        for (int i = 0; i < 16 || (i < 240 && controller.get_telemetry().weapon.cooldown > 0.0f); ++i) controller.step(idle, kDt, scene);
        scene.enemies = bots;
        scene.active_tracers.clear();
        if (!scatter) controller.get_telemetry().weapon.spread_rad = 0.0f;
        Pull p;
        const int calls = posed_calls;
        const Vec3 eye = controller.get_position() + Vec3(0.0f, 0.0f, controller.get_telemetry().eye_height);
        EnemyBodySet at_pull;
        renderer.pose_enemy_bodies(scene.enemies[0], controller.get_telemetry().sim_time, controller.get_telemetry().reaction_active, at_pull);
        const Vec3 dir = (aim_at(at_pull, which) - eye).normalized();
        std::vector<EnemyBodySet> now;
        controller.pose_enemies_for_shot(scene, eye, dir, 4000.0f, 0.0f, now);
        p.expected_met = controller.trace_enemies(scene, now, eye, dir, 4000.0f, p.expected);
        posed_calls = calls;
        controller.set_rotation(std::atan2(dir.y, dir.x) * RAD2DEG, std::asin(std::clamp(dir.z, -1.0f, 1.0f)) * RAD2DEG, 0.0f);
        const float health = scene.enemies[0].health;
        controller.step(fire, kDt, scene);
        p.fired = controller.get_telemetry().weapon.fired_this_tick;
        p.posed = posed_calls - calls;
        p.damage = health - scene.enemies[0].health;
        p.alive = scene.enemies[0].alive;
        for (const BulletTracer& tr : scene.active_tracers) {
            if (!tr.from_player) continue;
            ++p.tracers;
            p.bots_met += tr.pawn_hit != 0 ? 1 : 0;
            p.unshown += tr.pawn_hit == 3 ? 1 : 0;
            // The tracer's line against the bodies the first bot had as the trigger was pulled, across
            // their surfaces as for the lines above: one that stopped in a bot ends on a shape's
            // surface with no shape gone through before it, one that did not has gone through none.
            const Vec3 line = tr.end_pos - eye, along = line.normalized();
            EnemyBodyHit in_pose;
            p.through_pose += trace_enemy_bodies(now[0], eye, along, line.length() + 0.05f, in_pose) ? 1 : 0;
            if (now[0].valid) {
                double deepest_before = 0.0, off = 1.0e9;
                const double deepest = walk(now[0], eye, along, line.length(), line.length(), deepest_before);
                for (const BodyShape& s : now[0].shapes) off = std::min(off, std::abs(off_surface(s, tr.end_pos.x, tr.end_pos.y, tr.end_pos.z)));
                p.astray += (tr.pawn_hit != 0 ? off > kSkin || deepest_before > kSkin : deepest > kSkin) ? 1 : 0;
            }
            p.tracer = tr;
        }
        return p;
    };
    const auto say = [&](const std::string& what, bool ok, const Pull& p, const std::string& more) {
        std::cout << "  [Pulls] " << (ok ? "OK" : "FAIL") << ": " << what << ": " << (p.fired ? "" : "NOT FIRED, ") << "damage " << p.damage << ", ";
        if (p.expected_met) {
            std::cout << "the line meets " << (p.expected.posed ? "body " + std::to_string(p.expected.body) + " " + p.expected.bone : std::string("his cylinder"))
                      << (p.expected.head ? " (head)" : " (body)") << " at (" << p.expected.location.x << ", " << p.expected.location.y << ", "
                      << p.expected.location.z << "), ";
        } else {
            std::cout << "the line meets nobody, ";
        }
        std::cout << "tracer pawn_hit " << static_cast<int>(p.tracer.pawn_hit) << (p.tracer.pawn_surface == ECharacterSurface::Head ? " head" : " body")
                  << " ends (" << p.tracer.end_pos.x << ", " << p.tracer.end_pos.y << ", " << p.tracer.end_pos.z << "), " << p.posed << " bot(s) posed"
                  << more << std::endl;
        pass = pass && ok;
    };
    // The tracer against what the same line met when asked beforehand: the place, the normal, the material.
    const auto as_asked = [](const Pull& p) {
        return p.expected_met && (p.tracer.end_pos - p.expected.location).length() < 0.05f && (p.tracer.pawn_normal - p.expected.normal).length() < 0.01f &&
               (p.tracer.pawn_surface == ECharacterSurface::Head) == p.expected.head;
    };
    const std::vector<EnemyBot> swat = {make_bot("Assault_SWAT", "G36C")};
    // What the Colt's bullet takes from a bot (physics/bot_fire.hpp): the class's 42 (he is nearer
    // than where its fall-off starts), twice that on the head's material, less the armour his pawn
    // class has for the bone at this difficulty (TdBotPawn_Assault at Medium 0.3: 58 and 29), none
    // where the hit has no bone. Stage 27 holds the rule against the numbers; here it says what
    // each pull must have taken.
    const BotWeapon& colt_row = *find_bot_weapon("Colt1911");
    const float colt = colt_row.damage;
    const auto colt_takes = [&](const EnemyBot& bot, const ParkourController::EnemyShotHit& hit) {
        return static_cast<float>(bot_bullet_damage(find_aim_template(bot.archetype), weapon_damage_at(colt_row, hit.distance, false), colt_row.kind, hit.head,
                                                    hit.bone, hit.posed, controller.get_difficulty()));
    };
    for (int a = 0; a < kAims; ++a) {
        if (!kKnown[a].pulled) continue;
        const Pull p = pull(swat, a, false);
        const bool meets = kKnown[a].bone[0] != '\0';
        const bool ok = p.fired && p.posed == 1 &&
                        (meets ? p.damage == colt_takes(swat[0], p.expected) && p.damage == (kKnown[a].head ? 58.0f : 29.0f) && p.tracer.pawn_hit == 1 &&
                                     as_asked(p) && p.expected.posed &&
                                     p.expected.bone == kKnown[a].bone && p.expected.head == kKnown[a].head
                               : p.damage == 0.0f && p.tracer.pawn_hit == 0 && !p.expected_met);
        say(std::string("at ") + kKnown[a].name, ok, p, a == 0 ? " (the weapon's damage " + std::to_string(static_cast<int>(colt)) + ", his armour 0.3)" : std::string());
    }

    // A shotgun: every pellet of the pull meets the pose he had as the trigger was pulled, posed once,
    // whatever the pellets before it made of him. The pull kills a bot with 60 left (a pellet's 16
    // is 11 through his armour, 22 on a hand): the pellets that come after the one that does end in
    // him as well, and show nothing. Each tracer's line is held against the bodies he had at the
    // pull; and the same pull at a bot who cannot die.
    controller.equip_weapon("Remington870");
    const auto pellets = [](const Pull& q) {
        return ", " + std::to_string(q.tracers) + " pellets: " + std::to_string(q.bots_met) + " end in him, " + std::to_string(q.unshown) +
               " of them showing nothing; " + std::to_string(q.through_pose) + " lines go into the bodies he had at the pull, " +
               std::to_string(q.astray) + " at odds with them";
    };
    std::vector<EnemyBot> hurt = swat;
    hurt[0].health = 60.0f;
    Pull p = pull(hurt, kAimBelly, true);
    say("a shotgun at the belly of one with 60 left", p.fired && p.tracers > 1 && p.posed == 1 && !p.alive && p.astray == 0 && p.unshown >= 2, p, pellets(p));
    std::vector<EnemyBot> tough = swat;
    tough[0].health = tough[0].max_health = 100000.0f;
    p = pull(tough, kAimBelly, true);
    say("a shotgun at the belly of one who cannot die",
        p.fired && p.tracers > 1 && p.posed == 1 && p.alive && p.astray == 0 && p.bots_met > 1 && p.unshown == 0 && p.damage >= 11.0f * static_cast<float>(p.bots_met), p, pellets(p));
    controller.equip_weapon("Colt1911");

    // A bot who lies dead is still met, in the bodies of the pose he is drawn in, and nothing comes
    // of it (TdBotPawn.TakeDamage, TdBotPawn.PreventWeaponImpactEffect).
    EnemyBot dead = make_bot("Assault_SWAT", "None");
    dead.alive = false;
    dead.health = 0.0f;
    dead.stunned = true;
    dead.anim_state = EEnemyAnimState::KnockedOut;
    dead.active_anim_seq = "HitMeleeRight";
    dead.anim_timer = 2.0f;
    p = pull({dead}, kAimBelly, false);
    say("at a dead bot", p.fired && p.posed == 1 && p.damage == 0.0f && p.tracer.pawn_hit == 3 && as_asked(p) && p.expected.posed && p.astray == 0, p, "");

    // A story character who is off stage (her `alive`) is neither posed nor met.
    EnemyBot off_stage = make_bot("Kate", "None");
    off_stage.is_story_npc = true;
    off_stage.alive = false;
    p = pull({off_stage}, kAimBelly, false);
    say("at a story character off stage", p.fired && p.posed == 0 && !p.expected_met && p.damage == 0.0f && p.tracer.pawn_hit == 0, p, "");

    // Three bots: the one aimed at, one 400 uu to his side and one 500 behind her. Only the first is posed.
    std::vector<EnemyBot> three = swat;
    three.push_back(make_bot("PatrolCop", "Colt1911"));
    three.back().position = bot_at + right * 400.0f;
    three.push_back(make_bot("Support", "FNMinimi"));
    three.back().position = stand + fwd * 500.0f;
    p = pull(three, kAimBelly, false);
    say("with a bot 400 uu aside and one 500 behind her", p.fired && p.damage == colt_takes(three[0], p.expected) && p.damage > 0.0f && p.posed == 1 && as_asked(p), p, "");

    // The tutorial's partner (TdBotPawn_Tutorial.PreventWeaponImpactEffect): hurt, and nothing shown.
    p = pull({make_bot("TutorialTrainer_Celeste", "Colt1911")}, kAimBelly, false);
    say("at the tutorial's bot (TdBotPawn_Tutorial: no armour)",
        p.fired && (p.damage == colt || p.damage == colt * 2.0f) && p.damage == (p.expected.head ? 2.0f : 1.0f) * colt && p.tracer.pawn_hit == 3 && p.expected_met &&
            p.expected.posed,
        p, "");

    // Nothing to pose them with (a tool with no renderer): his cylinder, and its rule of heights.
    controller.set_enemy_body_poser(EnemyBodyPoser());
    p = pull(swat, kAimShoulder, false);
    say("with no poser, just outside a shoulder",
        p.fired && p.posed == 0 && p.expected_met && !p.expected.posed && !p.expected.head && p.damage == colt && p.tracer.pawn_hit == 1 && as_asked(p), p,
        " (no bone, no armour)");
    p = pull(swat, kAimHead, false);
    say("with no poser, at the head",
        p.fired && p.posed == 0 && p.expected_met && !p.expected.posed && p.expected.head && p.damage == colt * 2.0f && p.tracer.pawn_hit == 1 && as_asked(p), p, "");
    // (The cylinder is a living bot's alone, as it was before there were bodies.)
    p = pull({dead}, kAimBelly, false);
    say("with no poser, at a dead bot", p.fired && p.posed == 0 && !p.expected_met && p.damage == 0.0f && p.tracer.pawn_hit == 0, p, "");
    // ...and with one that cannot pose him, as the animation system answers without the character assets.
    controller.set_enemy_body_poser([](const EnemyBot&, float, bool, EnemyBodySet& out) {
        out.valid = false;
        return false;
    });
    p = pull(swat, kAimShoulder, false);
    say("with a poser that cannot pose him, just outside a shoulder",
        p.fired && p.posed == 0 && p.expected_met && !p.expected.posed && !p.expected.head && p.damage == colt && p.tracer.pawn_hit == 1 && as_asked(p), p, "");
    controller.set_enemy_body_poser(poser);

    std::cout << std::defaultfloat << std::setprecision(6);
    std::cout << "  -> Stage 24 Result: " << (pass ? "PASS" : "FAIL") << " (Asset=" << (asset_ok ? "OK" : "FAIL") << ", Lines=" << (lines_ok ? "OK" : "FAIL")
              << ", Rays=" << (rays_ok ? "OK" : "FAIL") << ", a pose " << pose_us << " us)" << std::endl;
    scene.enemies = enemies_before;
    scene.active_tracers = tracers_before;
    scene.dropped_weapons = dropped_before;
    return pass;
}

// -----------------------------------------------------------------------------
// A bot's fire at the player, a bot's armour, the helicopter's box and the stumble a bullet puts a
// bot in (physics/bot_fire.hpp, ParkourController::update_ai_bots), on the Training Area with the
// renderer's own skeletons. Oracle stage 27; --verify-botfire runs it alone.
//   Aim         the aim bot by itself: how long after its first shot a hit begins to count at each
//               range band and difficulty against (HorizontalOffset_Max - OffsetThreshold) /
//               (rate x multiplier), the reset after a lost line of fire, what her running and her
//               moves do to it
//   Fire        the controller's bots: one cop in the open, his first shot and the time to his
//               first hit that counts; the same cop moved behind something and back
//   Gate        shots that land on her with his offset over the threshold and under it, and what
//               the open ones take from her at each difficulty
//   Armour      her own pulls at three templates' bots, head, hand and thigh, at each difficulty
//   Helicopter  a pull at the box and one past it; the effect, the sound and no hole
//   Stumble     the sequence a pistol's and a shotgun's hit starts, from in front and from behind
//   Survival    how long she lasts standing in front of one patrol cop 600 uu off
// -----------------------------------------------------------------------------
static bool oracle_bot_fire(me::Renderer& renderer, me::LevelScene& scene, const me::MovementConfig& move_cfg) {
    using namespace me;
    constexpr float kDt = 1.0f / 60.0f;
    const std::vector<EnemyBot> enemies_before = scene.enemies;
    const std::vector<HelicopterInstance> helicopters_before = scene.helicopters;
    const std::vector<BulletTracer> tracers_before = scene.active_tracers;
    const std::vector<DroppedWeapon> dropped_before = scene.dropped_weapons;
    const std::vector<SpawnedEffect> effects_before = scene.spawned_effects;
    const std::vector<DynamicDecal> decals_before = scene.dynamic_decals;
    static const char* const kLevels[3] = {"Easy", "Medium", "Hard"};
    bool pass = true;
    std::cout << std::fixed << std::setprecision(2);
    const auto report = [&](const char* section, bool ok, const std::string& text) {
        std::cout << "  [" << section << "] " << (ok ? "OK" : "FAIL") << ": " << text << std::endl;
        pass = pass && ok;
    };
    const auto num = [](float v, int digits = 2) {
        std::ostringstream o;
        o << std::fixed << std::setprecision(digits) << v;
        return o.str();
    };

    // --- Aim -------------------------------------------------------------------------------------
    // The numbers of DefaultAITemplates.ini's AITemplate_Default, typed here apart from the table
    // the controller reads: HorizontalOffset_Max, OffsetThreshold and ImprovementRates by band and
    // difficulty, MoveTowardMultiplier 1.3, MoveSidewaysMultiplier 0.8, MoveAwayMultiplier 0.15.
    static const float kOffsetMax[3] = {50.0f, 90.0f, 200.0f}, kThreshold[3] = {40.0f, 70.0f, 130.0f}, kRate[3] = {20.0f, 35.0f, 40.0f};
    // An Assault cop's FN SCAR-L: CombatRange_Max 5000, so 300 uu is near, 1500 medium, 6000 far.
    static const float kBandDistance[3] = {300.0f, 1500.0f, 6000.0f};
    static const char* const kBandName[3] = {"near", "medium", "far"};
    const AimTemplate& assault = find_aim_template("AITemplate_Assault");
    const BotWeapon& scar = *find_bot_weapon("FNSCARL");
    const Vec3 aim_bot(0.0f, 0.0f, 94.0f);
    const auto target_at = [](float distance) {
        AimTarget t;
        t.location = Vec3(distance, 0.0f, 90.0f);
        t.viewpoint = Vec3(distance, 0.0f, 166.0f);
        return t;
    };
    // The time after the first shot at which a hit begins to count, the bot seeing her all along.
    const auto gate_time = [&](const AimTemplate& tpl, const BotWeapon& weapon, int difficulty, const AimTarget& target, float distance, float limit,
                               float* start_offset = nullptr) {
        BotCombatState s;
        s.random = 20081111u;
        aim_tick(s, tpl, weapon, difficulty, aim_bot, target, true, distance, 0.0f, kDt);
        aim_location(s, tpl, aim_bot, target, 0.0f);
        if (start_offset) *start_offset = s.base_offset;
        if (aim_hit_relevant(s, tpl, weapon.kind)) return 0.0f;
        for (int f = 1; static_cast<float>(f) * kDt <= limit; ++f) {
            const float now = static_cast<float>(f) * kDt;
            aim_tick(s, tpl, weapon, difficulty, aim_bot, target, true, distance, now, kDt);
            if (aim_hit_relevant(s, tpl, weapon.kind)) return now;
        }
        return -1.0f;
    };
    // (A tick is a sixtieth: the gate opens on the first tick at or past the time the rule gives.)
    const auto on_time = [&](float measured, float expected) { return measured >= expected - 1.0e-3f && measured <= expected + kDt + 1.0e-3f; };
    {
        bool ok = true;
        std::string text = "seconds from the first shot until a hit counts, she standing in his sight (measured / by the rule):";
        for (int b = 0; b < 3; ++b) {
            text += std::string(" ") + kBandName[b] + " (" + num(kOffsetMax[b], 0) + " -> " + num(kThreshold[b], 0) + " uu)";
            for (int d = 0; d < 3; ++d) {
                float start = 0.0f;
                const float measured = gate_time(assault, scar, d, target_at(kBandDistance[b]), kBandDistance[b], 30.0f, &start);
                const float expected = (kOffsetMax[b] - kThreshold[b]) / (kRate[d] * 1.3f);
                ok = ok && start == kOffsetMax[b] && on_time(measured, expected);
                text += std::string(" ") + kLevels[d] + " " + num(measured, 3) + " / " + num(expected, 3);
            }
            text += b < 2 ? ";" : "";
        }
        report("Aim", ok, text);
    }
    {
        // The reset. Medium difficulty, medium range: his offset comes down to HorizontalOffset_Min
        // (5) in (90 - 5) / 45.5 = 1.87 s. Out of his sight for 0.15 s (under the least
        // KeepFiringTime, 0.2) he keeps his line of fire and his aim; out of it for 1 s (over the
        // most, 0.8) his next shot starts at 90 again.
        const float distance = kBandDistance[1];
        const AimTarget target = target_at(distance);
        BotCombatState s;
        s.random = 20081111u;
        float now = 0.0f;
        const auto run = [&](float seconds, bool visible) {
            for (int f = 0; f < static_cast<int>(std::lround(seconds / kDt)); ++f) {
                now += kDt;
                aim_tick(s, assault, scar, kDifficultyMedium, aim_bot, target, visible, distance, now, kDt);
            }
        };
        aim_tick(s, assault, scar, kDifficultyMedium, aim_bot, target, true, distance, now, kDt);
        const float before_any = s.base_offset;
        aim_location(s, assault, aim_bot, target, now);
        const float first = s.base_offset;
        run(3.0f, true);
        const float settled = s.base_offset;
        run(0.15f, false);
        const bool kept = !s.first_shot;
        run(0.15f, true);
        aim_location(s, assault, aim_bot, target, now);
        const float after_short = s.base_offset;
        run(1.0f, false);
        const bool lost = s.first_shot;
        const float while_lost = s.base_offset;
        run(kDt, true);
        aim_location(s, assault, aim_bot, target, now);
        const float after_long = s.base_offset;
        const bool closed_again = !aim_hit_relevant(s, assault, scar.kind);
        const bool ok = before_any == 5.0f && first == 90.0f && settled == 5.0f && kept && after_short == 5.0f && lost && while_lost == 5.0f &&
                        after_long == 90.0f && closed_again;
        report("Aim", ok,
               "the reset (Medium, medium range): offset " + num(before_any, 1) + " before any shot, " + num(first, 1) + " at the first, " + num(settled, 1) +
                   " after 3 s in his sight; out of it 0.15 s: line of fire " + (kept ? "kept" : "LOST") + ", the next shot at " + num(after_short, 1) +
                   "; out of it 1 s: bFirstShot " + (lost ? "set" : "NOT SET") + ", offset still " + num(while_lost, 1) + ", the next shot at " +
                   num(after_long, 1) + (closed_again ? " and no hit counts" : " and a hit COUNTS"));
    }
    {
        // Her running and her moves, Medium difficulty and medium range unless said: 20 uu to lose
        // at 35 x multiplier x the move's AiAimPenalty.
        const float distance = kBandDistance[1];
        const auto moving = [&](const Vec3& velocity, EMovement move) {
            AimTarget t = target_at(distance);
            t.velocity = velocity;
            t.move = move;
            return t;
        };
        struct Case {
            const char* name;
            Vec3 velocity;
            EMovement move;
            int difficulty;
            float expected;  // 20 / the rate
        };
        const Case cases[] = {
            {"sprinting across his view", Vec3(0.0f, 600.0f, 0.0f), EMovement::MOVE_Walking, 1, 20.0f / (35.0f * 0.8f)},
            {"sprinting at him", Vec3(-600.0f, 0.0f, 0.0f), EMovement::MOVE_Walking, 1, 20.0f / (35.0f * 1.3f)},
            {"sprinting away", Vec3(600.0f, 0.0f, 0.0f), EMovement::MOVE_Walking, 1, 20.0f / (35.0f * 0.15f)},
            {"in a skill roll at 400 uu/s across", Vec3(0.0f, 400.0f, 0.0f), EMovement::MOVE_SkillRoll, 1, 20.0f / (35.0f * 0.8f)},
            {"sliding at him", Vec3(-500.0f, 0.0f, 0.0f), EMovement::MOVE_Slide, 1, 20.0f / (35.0f * 1.3f * 0.8f)},
            {"wall running across, Easy", Vec3(0.0f, 600.0f, 0.0f), EMovement::MOVE_WallRunningLeft, 0, 20.0f / (20.0f * 0.8f * 0.4f)},
            {"wall running across, Medium", Vec3(0.0f, 600.0f, 0.0f), EMovement::MOVE_WallRunningLeft, 1, 20.0f / (35.0f * 0.8f * 0.5f)},
            {"wall running across, Hard", Vec3(0.0f, 600.0f, 0.0f), EMovement::MOVE_WallRunningLeft, 2, 20.0f / (40.0f * 0.8f * 0.7f)},
            {"on a zip line across, Medium", Vec3(0.0f, 600.0f, 0.0f), EMovement::MOVE_ZipLine, 1, 20.0f / (35.0f * 0.8f * 0.1f)},
        };
        bool ok = true;
        std::string text = "seconds until a hit counts, she moving (measured / by the rule):";
        for (const Case& c : cases) {
            const float measured = gate_time(assault, scar, c.difficulty, moving(c.velocity, c.move), distance, 60.0f);
            ok = ok && on_time(measured, c.expected);
            text += std::string(" ") + c.name + " " + num(measured, 3) + " / " + num(c.expected, 3) + ";";
        }
        // A Support cop (MoveAwayMultiplier -0.2): while she runs from him his offset grows.
        const AimTemplate& support = find_aim_template("AITemplate_Support");
        const BotWeapon& minimi = *find_bot_weapon("FNMinimi");
        const float never = gate_time(support, minimi, kDifficultyMedium, moving(Vec3(600.0f, 0.0f, 0.0f), EMovement::MOVE_Walking), distance, 60.0f);
        ok = ok && never < 0.0f;
        text += std::string(" from a Support cop, sprinting away: ") + (never < 0.0f ? "never in 60 s" : num(never, 3));
        // The door gunner's own rates (Medium difficulty): 30, 25 and 15 uu/s by band over 200 - 80.
        const AimTemplate& gunner = find_aim_template("AITemplate_Gunner");
        static const float kGunnerDistance[3] = {300.0f, 1500.0f, 16000.0f}, kGunnerRate[3] = {30.0f, 25.0f, 15.0f};
        text += "; the door gunner, she standing:";
        for (int b = 0; b < 3; ++b) {
            const float measured = gate_time(gunner, minimi, kDifficultyMedium, target_at(kGunnerDistance[b]), kGunnerDistance[b], 60.0f);
            ok = ok && on_time(measured, 120.0f / kGunnerRate[b]);
            text += std::string(" ") + kBandName[b] + " " + num(measured, 3) + " / " + num(120.0f / kGunnerRate[b], 3);
        }
        report("Aim", ok, text);

        // The one-shot penalties, on a bot whose aim has settled at 5: a vault adds 50, less the
        // tick's own improvement (45.5 / 60), and hits go on counting (under 70); a swing adds 200,
        // which the band's most (90) cuts off, and they stop.
        BotCombatState s;
        s.random = 20081111u;
        AimTarget t = target_at(distance);
        float now = 0.0f;
        aim_tick(s, assault, scar, kDifficultyMedium, aim_bot, t, true, distance, now, kDt);
        aim_location(s, assault, aim_bot, t, now);
        for (int f = 0; f < 180; ++f) aim_tick(s, assault, scar, kDifficultyMedium, aim_bot, t, true, distance, now += kDt, kDt);
        BotCombatState vault = s, swing = s;
        t.move = EMovement::MOVE_SpeedVaulting;
        t.move_changed = true;
        aim_tick(vault, assault, scar, kDifficultyMedium, aim_bot, t, true, distance, now + kDt, kDt);
        t.move = EMovement::MOVE_Swing;
        aim_tick(swing, assault, scar, kDifficultyMedium, aim_bot, t, true, distance, now + kDt, kDt);
        const float vault_expected = 5.0f + 50.0f - 35.0f * 1.3f * kDt;
        const bool shots_ok = s.base_offset == 5.0f && std::abs(vault.base_offset - vault_expected) < 1.0e-3f && aim_hit_relevant(vault, assault, scar.kind) &&
                              swing.base_offset == 90.0f && !aim_hit_relevant(swing, assault, scar.kind);
        report("Aim", shots_ok,
               "one-shot penalties on a settled aim (offset " + num(s.base_offset, 1) + "): entering a vault " + num(vault.base_offset, 3) + " (by the rule " +
                   num(vault_expected, 3) + ", hits still count), entering a swing " + num(swing.base_offset, 1) + " (the band's most, no hit counts)");
    }
    {
        // The weapons' damage with distance (TdWeapon.GetInstantHitDamage), fired by a bot, and what
        // is left of it on her (TdPawn.AdjustDamage: 0.7, 0.5, 0.3 off by difficulty).
        struct Case {
            const char* weapon;
            float distance, damage;  // the damage before her armour, worked by hand from the class's values
            int on_her[3];
        };
        const Case cases[] = {
            {"Colt1911", 300.0f, 42.0f, {12, 21, 29}},        // under FallOffDistance 600
            {"Colt1911", 2550.0f, 21.0f, {6, 10, 14}},        // linear: half way from 600 to 4500
            {"Colt1911", 4500.0f, 0.0f, {0, 0, 0}},           // WeaponRange
            {"FNSCARL", 1500.0f, 35.0f, {10, 17, 24}},        // under 2000
            {"FNSCARL", 5500.0f, 14.0f, {4, 7, 9}},           // inverse: 35 / (3 x 0.5 + 1)
            {"SteyrTMP", 500.0f, 15.5f, {4, 7, 10}},          // int(15.5) = 15 goes to TakeDamage
            {"Remington870", 1000.0f, 16.0f, {4, 8, 11}},     // a pellet
            {"FNMinimi", 3000.0f, 36.0f, {10, 18, 25}},
            {"M95", 15000.0f, 80.0f, {24, 40, 56}},           // AIDamageMultiplier 0.8, no fall-off before its range
        };
        bool ok = true;
        std::string text = "a bot's bullet on her, before her armour and after it at Easy / Medium / Hard:";
        for (const Case& c : cases) {
            const BotWeapon& w = *find_bot_weapon(c.weapon);
            const float damage = weapon_damage_at(w, c.distance, true);
            ok = ok && std::abs(damage - c.damage) < 1.0e-3f;
            text += std::string(" ") + c.weapon + " at " + num(c.distance, 0) + " uu " + num(damage, 1) + " ->";
            for (int d = 0; d < 3; ++d) {
                const int on_her = player_bullet_damage(w, c.distance, d);
                ok = ok && on_her == c.on_her[d];
                text += std::string(d ? " / " : " ") + std::to_string(on_her);
            }
            text += ";";
        }
        report("Aim", ok, text);
    }

    // --- The controller's bots ---------------------------------------------------------------------
    const EnemyBodyPoser poser = [&](const EnemyBot& bot, float sim_time, bool reaction_disarm, EnemyBodySet& out) {
        return renderer.pose_enemy_bodies(bot, sim_time, reaction_disarm, out);
    };
    // She stands at the terrace's checkpoint (stages 7 and 24's ground), looking along +Y.
    const Vec3 stand(751.8f, -1591.7f, 4992.0f), bot_at(751.8f, -1320.0f, 4992.0f);
    const Vec3 fwd(0.0f, -1.0f, 0.0f), right(1.0f, 0.0f, 0.0f), up(0.0f, 0.0f, 1.0f);
    const auto make_bot = [&](const char* archetype, const char* weapon, const Vec3& at) {
        EnemyBot bot;
        bot.archetype = archetype;
        bot.weapon_name = weapon;
        bot.position = at;
        bot.home_position = at;
        const Vec3 to_her = (stand - at).normalized_xy();
        bot.yaw_deg = std::atan2(to_her.y, to_her.x) * RAD2DEG;
        bot.anim_state = EEnemyAnimState::AimFire;
        return bot;
    };
    InputFrame idle{}, fire{};
    fire.fire = true;
    scene.enemies.clear();
    scene.helicopters.clear();
    scene.active_tracers.clear();

    // Where she really stands there, and two places for a cop 600 uu from her on the same floor:
    // one the level leaves in sight of her eyes, one it hides. Looked for round her, every 5 degrees.
    Vec3 feet = stand;
    {
        ParkourController settle(move_cfg);
        settle.reset(stand, 90.0f);
        for (int i = 0; i < 60; ++i) settle.step(idle, kDt, scene);
        feet = settle.get_position();
    }
    ParkourController probe(move_cfg);
    const Vec3 her_eye = feet + Vec3(0.0f, 0.0f, 166.0f);
    bool have_open = false, have_hidden = false;
    Vec3 open_spot = bot_at, hidden_spot = bot_at;
    for (int a = 0; a < 72 && !(have_open && have_hidden); ++a) {
        const float angle = static_cast<float>(a) * 5.0f * DEG2RAD;
        const Vec3 at = feet + Vec3(std::cos(angle), std::sin(angle), 0.0f) * 600.0f;
        Vec3 hit, normal;
        // A floor under the place within 40 of her own, and room over it.
        if (!probe.leg_line_check(at + up * 150.0f, at - up * 60.0f, scene, hit, normal) || std::abs(hit.z - feet.z) > 40.0f || normal.z < 0.9f) continue;
        const Vec3 spot(at.x, at.y, hit.z);
        Vec3 h2, n2;
        const bool sight = !probe.leg_line_check(spot + up * 166.0f, her_eye, scene, h2, n2);
        if (sight && !have_open) {
            // The whole of him in the open: his chest's and his hands' height too.
            if (!probe.leg_line_check(spot + up * 120.0f, feet + up * 120.0f, scene, h2, n2) && !probe.leg_line_check(spot + up * 60.0f, feet + up * 60.0f, scene, h2, n2)) {
                have_open = true;
                open_spot = spot;
            }
        } else if (!sight && !have_hidden) {
            have_hidden = true;
            hidden_spot = spot;
        }
    }
    report("Fire", have_open && have_hidden,
           "she stands at (" + num(feet.x, 1) + ", " + num(feet.y, 1) + ", " + num(feet.z, 1) + "); a cop 600 uu off in her sight at (" + num(open_spot.x, 1) + ", " +
               num(open_spot.y, 1) + ", " + num(open_spot.z, 1) + ")" + (have_open ? "" : " NOT FOUND") + ", one the level hides at (" + num(hidden_spot.x, 1) + ", " +
               num(hidden_spot.y, 1) + ", " + num(hidden_spot.z, 1) + ")" + (have_hidden ? "" : " NOT FOUND"));

    // One run of her standing still with one bot: what he fires and what comes of it.
    struct Run {
        float first_shot = -1.0f, first_counted = -1.0f, death = -1.0f;
        float offset_at_first = 0.0f;
        uint8_t range = 0;
        int shots = 0, landed = 0, counted = 0;
        float health = 100.0f;
        std::vector<float> hits;  // what each counted hit took
    };
    // `seconds` of it, or until she dies; `between` is called before every step with the time.
    const auto stand_under_fire = [&](const EnemyBot& bot, int difficulty, float seconds, const std::function<void(float, LevelScene&)>& between) {
        ParkourController controller(move_cfg);
        controller.set_enemy_body_poser(poser);
        controller.set_difficulty(difficulty);
        controller.reset(feet, 90.0f);
        scene.enemies = {bot};
        scene.active_tracers.clear();
        Run r;
        for (int f = 0; static_cast<float>(f) * kDt < seconds; ++f) {
            const float now = static_cast<float>(f) * kDt;
            if (between) between(now, scene);
            const float health = controller.get_telemetry().health;
            // (A tracer lives 0.085 s: those of this step are the ones not marked before it.)
            for (BulletTracer& tr : scene.active_tracers) tr.max_time = 0.0f;
            controller.step(idle, kDt, scene);
            int fired = 0;
            for (const BulletTracer& tr : scene.active_tracers) {
                if (tr.from_player || tr.max_time == 0.0f) continue;
                ++fired;
                r.landed += tr.pawn_hit == 2 || tr.pawn_hit == 3 ? 1 : 0;
                r.counted += tr.pawn_hit == 2 ? 1 : 0;
            }
            if (fired > 0 && r.first_shot < 0.0f) {
                r.first_shot = now;
                r.offset_at_first = scene.enemies[0].combat.base_offset;
                r.range = scene.enemies[0].combat.firing_range;
            }
            r.shots += fired;
            const float lost = health - controller.get_telemetry().health;
            if (lost > 0.0f) {
                if (r.first_counted < 0.0f) r.first_counted = now;
                r.hits.push_back(lost);
            }
            r.health = controller.get_telemetry().health;
            if (r.health <= 0.0f) {
                r.death = now;
                break;
            }
        }
        return r;
    };
    const EnemyBot cop = make_bot("AITemplate_PatrolCop", "Colt1911", open_spot);

    if (have_open) {
        // A patrol cop's Colt (CombatRange_Max 2000): 600 uu is the medium band. His first shot
        // throws his offset to 90 (less the improvement of the ticks since, at most two); no hit
        // can count before (90 - 70) / (rate x 1.3) later, whatever lands.
        bool ok = true;
        std::string text = "a patrol cop 600 uu off, she standing: ";
        for (int d = 0; d < 3; ++d) {
            const Run r = stand_under_fire(cop, d, 12.0f, nullptr);
            const float gate = 20.0f / (kRate[d] * 1.3f);
            ok = ok && r.first_shot >= 0.0f && r.range == kFiringRangeMedium && r.offset_at_first <= 90.0f && r.offset_at_first > 90.0f - 3.0f * kRate[d] * 1.3f * kDt &&
                 r.first_counted >= r.first_shot + gate - kDt && r.counted > 0;
            // (The hit that kills her takes what she had left.)
            for (size_t i = 0; i < r.hits.size(); ++i) {
                if (r.death >= 0.0f && i + 1 == r.hits.size()) continue;
                ok = ok && r.hits[i] == static_cast<float>(player_bullet_damage(*find_bot_weapon("Colt1911"), 500.0f, d));
            }
            text += std::string(d ? "; " : "") + kLevels[d] + ": first shot at " + num(r.first_shot, 2) + " s with his offset at " + num(r.offset_at_first, 1) +
                    ", the first hit that counts at " + num(r.first_counted, 2) + " s (not before " + num(r.first_shot + gate, 2) + "), " + std::to_string(r.shots) +
                    " shots in " + num(r.death >= 0.0f ? r.death : 12.0f, 1) + " s, " + std::to_string(r.landed) + " on her, " + std::to_string(r.counted) + " counted" +
                    (r.hits.empty() ? "" : " for " + num(r.hits.front(), 0) + " each");
        }
        report("Fire", ok, text);

        // A restart (the load of the last checkpoint) starts his fire over with the clock: his turn
        // to burst, his timers and his aim (the first shot from 90 again, as soon as in a fresh run).
        ParkourController controller(move_cfg);
        controller.set_enemy_body_poser(poser);
        controller.set_difficulty(kDifficultyMedium);
        controller.reset(feet, 90.0f);
        scene.enemies = {cop};
        scene.active_tracers.clear();
        const auto first_shot_within = [&](float seconds, float& offset_at_first) {
            for (int f = 0; static_cast<float>(f) * kDt < seconds; ++f) {
                for (BulletTracer& tr : scene.active_tracers) tr.max_time = 0.0f;
                controller.step(idle, kDt, scene);
                for (const BulletTracer& tr : scene.active_tracers) {
                    if (tr.from_player || tr.max_time == 0.0f) continue;
                    offset_at_first = scene.enemies[0].combat.base_offset;
                    return static_cast<float>(f) * kDt;
                }
            }
            return -1.0f;
        };
        float offset_before = 0.0f, offset_after = 0.0f, ignored = 0.0f;
        const float shot_before = first_shot_within(1.0f, offset_before);
        first_shot_within(2.5f, ignored);  // into his fire, his aim settling
        const float settled = scene.enemies[0].combat.base_offset;
        controller.restart_level_at(ParkourController::start_spot_over_feet(feet), 90.0f, scene);
        const float shot_after = first_shot_within(1.0f, offset_after);
        report("Restart",
               shot_before >= 0.0f && settled < 60.0f && shot_after >= 0.0f && std::abs(shot_after - shot_before) <= kDt && std::abs(offset_after - offset_before) < 0.5f &&
                   (controller.get_telemetry().position - feet).length() < 2.0f,
               "a patrol cop at Medium: first shot at " + num(shot_before, 2) + " s with his offset at " + num(offset_before, 1) + "; at 3.5 s his offset " +
                   num(settled, 1) + "; after a restart where she stood, first shot at " + num(shot_after, 2) + " s with his offset at " + num(offset_after, 1));
    }
    if (have_open && have_hidden) {
        // The same cop, Medium: three seconds in her sight (his aim settles), then moved behind what
        // the level has there for 1.5 s, then back. He sees nobody there and fires nothing once his
        // KeepFiringTime (0.8 at most) is over; his first shot on coming back is from 90 again.
        float settled = -1.0f, hidden_offset = -1.0f, back_offset = -1.0f, back_shot = -1.0f, back_counted = -1.0f;
        bool unseen = false, first_shot_set = false;
        int shots_hidden_late = 0, tracers_seen = 0;
        ParkourController controller(move_cfg);
        controller.set_enemy_body_poser(poser);
        controller.set_difficulty(kDifficultyMedium);
        controller.reset(feet, 90.0f);
        controller.get_telemetry().health = 1.0e6f;  // what he does to her is not this case's to judge
        scene.enemies = {cop};
        scene.active_tracers.clear();
        for (int f = 0; f < 60 * 14; ++f) {
            const float now = static_cast<float>(f) * kDt;
            EnemyBot& bot = scene.enemies[0];
            const bool hide = now >= 3.0f && now < 4.5f;
            bot.position = hide ? hidden_spot : open_spot;
            bot.home_position = bot.position;
            for (BulletTracer& tr : scene.active_tracers) tr.max_time = 0.0f;
            const float health = controller.get_telemetry().health;
            controller.step(idle, kDt, scene);
            int fired = 0;
            for (const BulletTracer& tr : scene.active_tracers) fired += !tr.from_player && tr.max_time != 0.0f ? 1 : 0;
            tracers_seen += fired;
            if (std::abs(now - 2.9f) < kDt * 0.5f) settled = bot.combat.base_offset;
            if (hide && now >= 3.9f) {
                shots_hidden_late += fired;
                unseen = !bot.combat.enemy_visible;
                first_shot_set = bot.combat.first_shot;
                hidden_offset = bot.combat.base_offset;
            }
            if (now >= 4.5f && fired > 0 && back_shot < 0.0f) {
                back_shot = now;
                back_offset = bot.combat.base_offset;
            }
            if (now >= 4.5f && back_counted < 0.0f && controller.get_telemetry().health < health) back_counted = now;
        }
        const float gate = 20.0f / (35.0f * 1.3f);
        const bool ok = settled == 5.0f && unseen && first_shot_set && shots_hidden_late == 0 && hidden_offset == 5.0f && back_shot >= 4.5f && back_offset <= 90.0f &&
                        back_offset > 90.0f - 3.0f * 35.0f * 1.3f * kDt && back_counted >= back_shot + gate - kDt;
        report("Fire", ok,
               "the same cop, Medium: offset " + num(settled, 1) + " after 2.9 s in her sight; behind the level from 3.0 to 4.5 s: " +
                   (unseen ? "he does not see her" : "HE SEES HER") + ", " + std::to_string(shots_hidden_late) + " shots after 3.9 s, bFirstShot " +
                   (first_shot_set ? "set" : "NOT SET") + ", offset " + num(hidden_offset, 1) + "; back in her sight his first shot at " + num(back_shot, 2) +
                   " s with his offset at " + num(back_offset, 1) + ", the first hit that counts at " + num(back_counted, 2) + " s (not before " +
                   num(back_shot + gate, 2) + "); " + std::to_string(tracers_seen) + " shots in all");
    }
    {
        // The bursts, 272 uu off (the near band: the weapon's AimedBurst_Near, ATdBotPawn::SetAIFiringState),
        // with her health out of the question. An Assault cop's MP5K: six shots a tenth of a second
        // apart, then PauseFiring's 0.03 s and a pause of 0.3 to 0.5 s. One with a Neostead: a shot a
        // burst, and the pause (0.6 to 0.8 s) is shorter than the weapon's FireInterval, which is then
        // what parts his shots: 1.1 s. Three MP5K cops side by side: no more than two hold their
        // triggers at once (TdAIManager.MaxSimultaneousBursts 2), and each gets his turn.
        const auto shot_times = [&](std::vector<EnemyBot> bots, float seconds, int& most_at_once) {
            ParkourController controller(move_cfg);
            controller.set_enemy_body_poser(poser);
            controller.set_difficulty(kDifficultyMedium);
            controller.reset(feet, 90.0f);
            controller.get_telemetry().health = 1.0e6f;
            scene.enemies = std::move(bots);
            scene.active_tracers.clear();
            std::vector<std::vector<float>> times(scene.enemies.size());
            most_at_once = 0;
            for (int f = 0; static_cast<float>(f) * kDt < seconds; ++f) {
                controller.step(idle, kDt, scene);
                scene.active_tracers.clear();
                int held = 0;
                for (size_t i = 0; i < scene.enemies.size(); ++i) {
                    // (The flash is lit for 0.08 s by the step he fires in, and burns down from the next.)
                    if (scene.enemies[i].muzzle_flash_timer == 0.08f) times[i].push_back(static_cast<float>(f) * kDt);
                    held += scene.enemies[i].combat.trigger ? 1 : 0;
                }
                most_at_once = std::max(most_at_once, held);
            }
            return times;
        };
        // The shots of one bot as bursts: a gap of more than a quarter of a second begins a new one.
        struct Bursts {
            int count = 0, least = 1 << 30, most = 0;
            float gap_min = 1.0e9f, gap_max = 0.0f;      // between the shots of a burst
            float pause_min = 1.0e9f, pause_max = 0.0f;  // from a burst's last shot to the next one's first
        };
        const auto bursts_of = [](const std::vector<float>& times) {
            Bursts b;
            int in_burst = 0;
            for (size_t i = 0; i < times.size(); ++i) {
                const float gap = i ? times[i] - times[i - 1] : 0.0f;
                if (i && gap <= 0.25f) {
                    b.gap_min = std::min(b.gap_min, gap);
                    b.gap_max = std::max(b.gap_max, gap);
                    ++in_burst;
                    continue;
                }
                if (i) {
                    b.pause_min = std::min(b.pause_min, gap);
                    b.pause_max = std::max(b.pause_max, gap);
                    b.least = std::min(b.least, in_burst);
                    b.most = std::max(b.most, in_burst);
                    ++b.count;
                }
                in_burst = 1;
            }
            return b;  // (the last burst may have been cut short by the end of the run: not counted)
        };
        int at_once = 0;
        const Bursts mp5k = bursts_of(shot_times({make_bot("AITemplate_Assault_MP5K", "MP5K", bot_at)}, 12.0f, at_once)[0]);
        const Bursts neostead = bursts_of(shot_times({make_bot("AITemplate_Assault_Neostead", "Neostead", bot_at)}, 12.0f, at_once)[0]);
        const std::vector<std::vector<float>> three = shot_times({make_bot("AITemplate_Assault_MP5K", "MP5K", bot_at - right * 130.0f),
                                                                  make_bot("AITemplate_Assault_MP5K", "MP5K", bot_at),
                                                                  make_bot("AITemplate_Assault_MP5K", "MP5K", bot_at + right * 130.0f)},
                                                                 12.0f, at_once);
        const bool mp5k_ok = mp5k.count >= 8 && mp5k.least == 6 && mp5k.most == 6 && mp5k.gap_min > 0.1f - 0.5f * kDt && mp5k.gap_max < 0.1f + 1.5f * kDt &&
                             mp5k.pause_min >= 0.03f + 0.3f && mp5k.pause_max <= 0.03f + 0.5f + 5.0f * kDt;
        const bool neostead_ok = neostead.count >= 8 && neostead.least == 1 && neostead.most == 1 && neostead.pause_min > 1.1f - 0.5f * kDt &&
                                 neostead.pause_max < 1.1f + 1.5f * kDt;
        const bool three_ok = at_once == 2 && three.size() == 3 && three[0].size() >= 6 && three[1].size() >= 6 && three[2].size() >= 6;
        // A helicopter's rider, whom the level's factory leaves standing where it spawns him: his
        // fire is the helicopter's gun's, and he has none of his own.
        int rider_held = 0;
        const size_t rider_shots = shot_times({make_bot("AITemplate_Gunner", "FNMinimi", bot_at)}, 3.0f, rider_held)[0].size();
        report("Fire", mp5k_ok && neostead_ok && three_ok && rider_shots == 0 && rider_held == 0,
               "bursts 272 uu off, 12 s each: an MP5K " + std::to_string(mp5k.count) + " bursts of " + std::to_string(mp5k.least) + " to " + std::to_string(mp5k.most) +
                   " shots (6), " + num(mp5k.gap_min, 3) + " to " + num(mp5k.gap_max, 3) + " s apart (FireInterval 0.1), " + num(mp5k.pause_min, 2) + " to " +
                   num(mp5k.pause_max, 2) + " s between bursts (0.03 and a pause of 0.3 to 0.5); a Neostead " + std::to_string(neostead.count + 1) + " shots, " +
                   num(neostead.pause_min, 3) + " to " + num(neostead.pause_max, 3) + " s apart (its FireInterval 1.1, over its pause of 0.6 to 0.8); three MP5K cops: " +
                   std::to_string(at_once) + " triggers held at most (2), their shots " + std::to_string(three.size() > 0 ? three[0].size() : 0) + ", " +
                   std::to_string(three.size() > 1 ? three[1].size() : 0) + " and " + std::to_string(three.size() > 2 ? three[2].size() : 0) +
                   "; a helicopter's rider left standing with his Minimi: " + std::to_string(rider_shots) + " shots in 3 s (0)");
    }

    // --- Gate --------------------------------------------------------------------------------------
    // A cop on the terrace 272 uu in front of her (the near band: a hit counts at 40 or under), his
    // aim set by hand on her middle (CurrentX 0.4 going to -0.4, which GetNextLocation leaves where
    // it is; 45 over her Location, her chest) and held there by an overridden improvement rate of
    // next to nothing (OverriddenImprovementRate, the level script's own way in). With his offset
    // at 45 the bullet lands and nothing comes of it; at 30 it takes the weapon's damage less her
    // armour.
    const auto rigged_shot = [&](const char* archetype, const char* weapon, float offset, int difficulty, float& lost, uint8_t& pawn_hit, Vec3& end) {
        ParkourController controller(move_cfg);
        controller.set_enemy_body_poser(poser);
        controller.set_difficulty(difficulty);
        controller.reset(feet, 90.0f);
        EnemyBot bot = make_bot(archetype, weapon, bot_at);
        BotCombatState& s = bot.combat;
        s.random = 20081111u;
        s.first_shot = false;
        s.has_enemy = true;
        s.enemy_visible = true;
        s.improvement_override = 1.0e-6f;
        s.base_offset = offset;
        s.base_dispersion = 10.0f;
        s.base_y_offset = 10.0f;
        s.current_x = 0.4f;
        s.target_x = -0.4f;
        s.diff_x = 0.8f;
        s.current_y = 45.0f;
        s.current_location = feet + Vec3(0.0f, 0.0f, 90.0f + 45.0f);
        s.time_of_last_shot = 0.0f;
        scene.enemies = {bot};
        scene.active_tracers.clear();
        lost = 0.0f;
        pawn_hit = 255;
        for (int f = 0; f < 240; ++f) {
            const float health = controller.get_telemetry().health;
            controller.step(idle, kDt, scene);
            for (const BulletTracer& tr : scene.active_tracers) {
                if (tr.from_player) continue;
                pawn_hit = tr.pawn_hit;
                end = tr.end_pos;
                lost = health - controller.get_telemetry().health;
                return scene.enemies[0].combat.base_offset;
            }
        }
        return -1.0f;
    };
    {
        bool ok = true;
        std::string text;
        float lost = 0.0f;
        uint8_t pawn_hit = 0;
        Vec3 end;
        float offset = rigged_shot("AITemplate_PatrolCop", "Colt1911", 45.0f, kDifficultyMedium, lost, pawn_hit, end);
        EnemyBodySet her;
        pose_player_stand_in(feet, 90.0f, 180.0f, her);
        const auto on_her = [&](const Vec3& p) {
            // Within a unit of one of her stand-in's shapes' middles' reach: the tracer ends in her.
            return (p - her.bound_centre).length() <= her.bound_radius + 0.5f && std::abs(p.x - feet.x) < 30.0f && p.z > feet.z + 100.0f && p.z < feet.z + 185.0f;
        };
        ok = ok && offset > 40.0f && pawn_hit == 3 && lost == 0.0f && on_her(end);
        text += "a patrol cop's Colt at 272 uu, his offset " + num(offset, 1) + " (over 40): the bullet ends in her at (" + num(end.x, 1) + ", " + num(end.y, 1) + ", " +
                num(end.z, 1) + "), pawn_hit " + std::to_string(pawn_hit) + ", health lost " + num(lost, 0) + "; his offset 30:";
        static const int kColtOnHer[3] = {12, 21, 29};  // int(42 x 0.3), int(42 x 0.5), int(42 x 0.7)
        for (int d = 0; d < 3; ++d) {
            offset = rigged_shot("AITemplate_PatrolCop", "Colt1911", 30.0f, d, lost, pawn_hit, end);
            ok = ok && offset <= 40.0f && pawn_hit == 2 && lost == static_cast<float>(kColtOnHer[d]) && on_her(end);
            text += std::string(" ") + kLevels[d] + " " + num(lost, 0) + " (" + std::to_string(kColtOnHer[d]) + ")";
        }
        // An Assault cop's G36C (30) and a Support cop's Minimi (36), Medium: 15 and 18.
        offset = rigged_shot("AITemplate_Assault_HKG36C", "G36C", 30.0f, kDifficultyMedium, lost, pawn_hit, end);
        ok = ok && pawn_hit == 2 && lost == 15.0f;
        text += "; a G36C at Medium " + num(lost, 0) + " (15)";
        offset = rigged_shot("AITemplate_Support", "FNMinimi", 30.0f, kDifficultyMedium, lost, pawn_hit, end);
        ok = ok && pawn_hit == 2 && lost == 18.0f;
        text += ", a Minimi " + num(lost, 0) + " (18)";
        // A sniper's bullet is no "bullet damage" to IsHitRelevant: it counts whatever his offset
        // (100 is the most a sniper's template lets it be, over its threshold of 75), and is not
        // heard while the offset is over it (PlayImpactEffects asks of TdDmgType_LowCaliber_Bullet).
        offset = rigged_shot("AITemplate_SniperCop", "M95", 100.0f, kDifficultyMedium, lost, pawn_hit, end);
        ok = ok && offset > 75.0f && pawn_hit == 3 && lost == 40.0f;
        text += "; a sniper's M95 with his offset " + num(offset, 1) + " (over 75) " + num(lost, 0) + " (40: int(100 x 0.8) x 0.5)";
        // The Training Area's partner's hits are never held back (TdAI_Tutorial.IsHitRelevant).
        BotCombatState tutorial;
        tutorial.base_offset = 50.0f;
        tutorial.firing_range = kFiringRangeNear;
        ok = ok && aim_hit_relevant(tutorial, find_aim_template("TutorialTrainer_Celeste"), EBulletKind::LowCaliber) &&
             !aim_hit_relevant(tutorial, find_aim_template("AITemplate_PatrolCop"), EBulletKind::LowCaliber);
        report("Gate", ok, text);
    }

    // --- Armour ------------------------------------------------------------------------------------
    // Her own pulls (stage 24's way of pulling): a Colt that does not scatter, the view turned onto
    // a point of the bot as he stands, one step with the trigger held. The bots hold their fire.
    enum { kAimHead = 0, kAimHand, kAimThigh, kAimBelly, kAims };
    const auto aim_at = [&](const EnemyBodySet& s, int which) {
        const auto shape = [&](uint8_t body, EBodyShape kind) -> const BodyShape& {
            for (const BodyShape& b : s.shapes) {
                if (b.body == body && b.kind == kind) return b;
            }
            return s.shapes.front();
        };
        if (which == kAimHead) return shape(3, EBodyShape::Capsule).centre;
        if (which == kAimHand) return shape(6, EBodyShape::Capsule).centre;
        if (which == kAimThigh) return shape(13, EBodyShape::Box).centre;
        return shape(1, EBodyShape::Box).centre;
    };
    ParkourController shooter(move_cfg);
    shooter.set_enemy_body_poser(poser);
    shooter.set_ai_hold_fire(true);
    shooter.reset(feet, 90.0f);
    for (int i = 0; i < 30; ++i) shooter.step(idle, kDt, scene);
    struct Pull {
        bool fired = false;
        float damage = 0.0f;
        BulletTracer tracer;
        bool met = false;
        ParkourController::EnemyShotHit expected;
        EnemyBot after;
    };
    // One pull at the point `target` gives once the weapon is ready, the first of `bots` being the
    // one looked at afterwards.
    const auto pull_at = [&](const std::vector<EnemyBot>& bots, const std::function<Vec3()>& target, bool wait = true) {
        for (int i = 0; wait && (i < 16 || (i < 240 && shooter.get_telemetry().weapon.cooldown > 0.0f)); ++i) shooter.step(idle, kDt, scene);
        scene.enemies = bots;
        scene.active_tracers.clear();
        shooter.get_telemetry().weapon.spread_rad = 0.0f;
        Pull p;
        const Vec3 eye = shooter.get_position() + Vec3(0.0f, 0.0f, shooter.get_telemetry().eye_height);
        const Vec3 dir = (target() - eye).normalized();
        std::vector<EnemyBodySet> now;
        shooter.pose_enemies_for_shot(scene, eye, dir, 4000.0f, 0.0f, now);
        p.met = shooter.trace_enemies(scene, now, eye, dir, 4000.0f, p.expected);
        shooter.set_rotation(std::atan2(dir.y, dir.x) * RAD2DEG, std::asin(std::clamp(dir.z, -1.0f, 1.0f)) * RAD2DEG, 0.0f);
        const float health = scene.enemies.empty() ? 0.0f : scene.enemies[0].health;
        shooter.step(fire, kDt, scene);
        p.fired = shooter.get_telemetry().weapon.fired_this_tick;
        if (!scene.enemies.empty()) {
            p.damage = health - scene.enemies[0].health;
            p.after = scene.enemies[0];
        }
        for (const BulletTracer& tr : scene.active_tracers) {
            if (tr.from_player) p.tracer = tr;
        }
        return p;
    };
    const auto bot_point = [&](const EnemyBot& bot, int which) {
        EnemyBodySet set;
        renderer.pose_enemy_bodies(bot, shooter.get_telemetry().sim_time, false, set);
        return set.valid ? aim_at(set, which) : bot.position + up * 120.0f;
    };
    // The same, as a point to be read when the trigger is pulled.
    const auto at_bot = [&](const EnemyBot& bot, int which) { return std::function<Vec3()>([&bot_point, bot, which]() { return bot_point(bot, which); }); };
    const auto at_point = [](const Vec3& point) { return std::function<Vec3()>([point]() { return point; }); };
    {
        // ArmorBullets*Settings of the three pawn classes at Easy / Medium / Hard, typed here apart
        // from the table: TdBotPawn_Assault 0.2 / 0.3 / 0.5, TdBotPawn_PatrolCop 0 / 0 / 0.2,
        // TdBotPawn_Support 0.4 / 0.5 / 0.7, the same on head, body and legs. The Colt's 42 at
        // 272 uu: the head's body (bone Neck, PM_Character_Head) int(84 x (1 - A)), a hand's (the
        // same material, the body's armour) the same, a thigh's int(42 x (1 - A)).
        // (Each stands as stage 24's bot does, a rifle at his shoulder: the pose the points are read off.)
        struct Row {
            const char* archetype;
            int head[3], thigh[3];
        };
        static const Row kRows[] = {
            {"AITemplate_Assault", {67, 58, 42}, {33, 29, 21}},
            {"AITemplate_PatrolCop", {84, 84, 67}, {42, 42, 33}},
            {"AITemplate_Support", {50, 42, 25}, {25, 21, 12}},
        };
        shooter.equip_weapon("Colt1911");
        bool ok = true;
        std::string text = "a Colt's bullet (42) at head / hand / thigh, Easy, Medium, Hard:";
        for (const Row& row : kRows) {
            text += std::string(" ") + row.archetype + " (" + find_aim_template(row.archetype).name + ")";
            for (int d = 0; d < 3; ++d) {
                shooter.set_difficulty(d);
                EnemyBot bot = make_bot(row.archetype, "G36C", bot_at);
                bot.health = bot.max_health = 100000.0f;
                shooter.equip_weapon("Colt1911");  // eight rounds
                const Pull head = pull_at({bot}, at_bot(bot, kAimHead));
                const Pull hand = pull_at({bot}, at_bot(bot, kAimHand));
                const Pull thigh = pull_at({bot}, at_bot(bot, kAimThigh));
                ok = ok && head.fired && head.met && head.expected.bone == "Neck" && head.expected.head && head.damage == static_cast<float>(row.head[d]) &&
                     hand.met && hand.expected.bone == "LeftHand" && hand.expected.head && hand.damage == static_cast<float>(row.head[d]) && thigh.met &&
                     thigh.expected.bone == "RightUpLeg" && !thigh.expected.head && thigh.damage == static_cast<float>(row.thigh[d]);
                text += std::string(d ? ", " : " ") + num(head.damage, 0) + " / " + num(hand.damage, 0) + " / " + num(thigh.damage, 0);
            }
            text += ";";
        }
        shooter.set_difficulty(kDifficultyMedium);
        // The rest of the table, by the rule alone: Celeste's parts, a sniper's bullet, the classes with none.
        const AimTemplate& celeste = find_aim_template("AITemplate_Celeste");
        const int c_head = bot_bullet_damage(celeste, 42.0f, EBulletKind::LowCaliber, true, "Neck", true, 1);
        const int c_hand = bot_bullet_damage(celeste, 42.0f, EBulletKind::LowCaliber, true, "LeftHand", true, 1);
        const int c_body = bot_bullet_damage(celeste, 42.0f, EBulletKind::LowCaliber, false, "Spine1", true, 1);
        const int c_legs = bot_bullet_damage(celeste, 42.0f, EBulletKind::LowCaliber, false, "LeftFoot", true, 1);
        const int riot = bot_bullet_damage(find_aim_template("AITemplate_RiotCop"), 42.0f, EBulletKind::LowCaliber, false, "RightLeg", true, 2);
        const int steyr = bot_bullet_damage(find_aim_template("AITemplate_PatrolCop_MiniUzi"), 42.0f, EBulletKind::LowCaliber, false, "Hips", true, 1);
        const int sniper = bot_bullet_damage(find_aim_template("AITemplate_SniperCop"), 42.0f, EBulletKind::LowCaliber, false, "Hips", true, 2);
        const int by_sniper = bot_bullet_damage(find_aim_template("AITemplate_RiotCop"), 100.0f, EBulletKind::Sniper, false, "RightFoot", true, 2);
        const int gunner = bot_bullet_damage(find_aim_template("AITemplate_Gunner"), 42.0f, EBulletKind::LowCaliber, false, "Hips", true, 2);
        const int no_bone = bot_bullet_damage(find_aim_template("AITemplate_Support"), 42.0f, EBulletKind::LowCaliber, false, "", false, 2);
        ok = ok && c_head == 67 && c_hand == 33 && c_body == 16 && c_legs == 21 && riot == 8 && steyr == 37 && sniper == 25 && by_sniper == 99999 && gunner == 42 && no_bone == 42;
        text += " by the rule: Celeste at Medium head " + std::to_string(c_head) + ", a hand (the head's material, the body's armour) " + std::to_string(c_hand) + ", body " + std::to_string(c_body) + ", foot " + std::to_string(c_legs) +
                "; a riot cop's shin at Hard " + std::to_string(riot) + "; PatrolCop_MiniUzi (a Steyr cop) at Medium " + std::to_string(steyr) +
                "; a sniper cop at Hard " + std::to_string(sniper) + "; a sniper's bullet " + std::to_string(by_sniper) + "; the door gunner " +
                std::to_string(gunner) + "; with no bone (his cylinder) " + std::to_string(no_bone);
        report("Armour", ok, text);
    }

    // --- Helicopter --------------------------------------------------------------------------------
    {
        // A helicopter hung where the level leaves her a clear line to its box, broadside to her
        // (its box is then 487 across and 137.5 high), not yet firing. A pull at the box's middle
        // stops on its near face; one 150 uu over the box goes on. A step later the hit has its
        // effect and its sound, the default material's, and no hole.
        HelicopterInstance heli;
        heli.state = EHeliState::Arriving;
        heli.hold_fire_delay = 1.0e9f;
        heli.perfect_aim_delay = 1.0e9f;
        heli.yaw_deg = 0.0f;
        bool placed = false;
        const Vec3 eye = shooter.get_position() + Vec3(0.0f, 0.0f, shooter.get_telemetry().eye_height);
        for (int a = 0; a < 36 && !placed; ++a) {
            const float angle = (90.0f + static_cast<float>(a) * 10.0f) * DEG2RAD;
            heli.position = feet + Vec3(std::cos(angle) * 900.0f, std::sin(angle) * 900.0f, 100.0f);
            heli.yaw_deg = static_cast<float>(a) * 10.0f;  // broadside to her
            const BodyShape box = helicopter_box(heli);
            Vec3 h, n;
            placed = !probe.leg_line_check(eye, box.centre, scene, h, n) && !probe.leg_line_check(eye, box.centre + up * 220.0f + (box.centre - eye).normalized() * 400.0f, scene, h, n);
        }
        heli.spawn_pos = heli.position;
        const std::vector<EnemyBot> nobody;
        scene.enemies.clear();
        shooter.equip_weapon("Colt1911");
        // (It is put there only once her weapon is ready, and so has not flown anywhere yet.)
        for (int i = 0; i < 20; ++i) shooter.step(idle, kDt, scene);
        scene.spawned_effects.clear();
        scene.dynamic_decals.clear();
        scene.helicopters = {heli};
        const size_t effects = scene.spawned_effects.size(), decals = scene.dynamic_decals.size();
        BodyShape box = helicopter_box(scene.helicopters[0]);
        const Pull on = pull_at(nobody, at_point(box.centre), false);
        // Where the line from her eye to the box's middle goes into the box, worked here.
        float expected_distance = 0.0f;
        Vec3 expected_normal;
        const Vec3 line = (box.centre - eye).normalized();
        const bool crosses = trace_body_shape(box, eye, line, 4000.0f, expected_distance, expected_normal);
        const Vec3 expected_at = eye + line * expected_distance;
        // In the box's own frame the place is on its near side face (y: 101.44 from the middle).
        const Vec3 rel = on.tracer.end_pos - box.centre;
        const float side = std::abs(rel.dot(box.axis[1]));
        shooter.step(idle, kDt, scene);
        const size_t effects_after = scene.spawned_effects.size(), decals_after = scene.dynamic_decals.size();
        std::string heard;
        const std::string want = scene.default_physical >= 0 && static_cast<size_t>(scene.default_physical) < scene.physical_materials.size()
                                     ? scene.physical_materials[static_cast<size_t>(scene.default_physical)].impact_sound
                                     : std::string();
        for (const SimSoundEvent& ev : shooter.get_telemetry().sound_events) {
            if (!ev.at_pawn && (ev.location - on.tracer.end_pos).length() < 1.0f) heard = ev.cue;
        }
        std::string effect_path;
        if (effects_after > effects) {
            const SpawnedEffect& e = scene.spawned_effects.back();
            if (e.at.template_index >= 0 && static_cast<size_t>(e.at.template_index) < scene.particle_templates.size()) {
                effect_path = scene.particle_templates[static_cast<size_t>(e.at.template_index)].path;
            }
        }
        // The second pull, with the helicopter put back where it hung.
        scene.helicopters.clear();
        for (int i = 0; i < 20; ++i) shooter.step(idle, kDt, scene);
        scene.helicopters = {heli};
        box = helicopter_box(scene.helicopters[0]);
        const Vec3 eye2 = shooter.get_position() + Vec3(0.0f, 0.0f, shooter.get_telemetry().eye_height);
        const Pull over = pull_at(nobody, at_point(box.centre + up * 220.0f + (box.centre - eye2).normalized() * 400.0f), false);
        const bool ok = placed && crosses && on.fired && on.tracer.pawn_hit == 4 && (on.tracer.end_pos - expected_at).length() < 0.05f && std::abs(side - 101.441f) < 0.05f &&
                        (on.tracer.pawn_normal - expected_normal).length() < 0.01f && effects_after == effects + 1 && decals_after == decals && !want.empty() &&
                        heard == want && over.fired && over.tracer.pawn_hit == 0 && scene.helicopters[0].state == EHeliState::Arriving;
        report("Helicopter", ok,
               std::string(placed ? "" : "NO CLEAR PLACE FOUND; ") + "a helicopter 900 uu off, its box's middle at (" + num(box.centre.x, 1) + ", " + num(box.centre.y, 1) + ", " +
                   num(box.centre.z, 1) + "): a Colt's bullet at it ends " + num((on.tracer.end_pos - eye).length(), 2) + " uu from her eye (the box's face is at " +
                   num(expected_distance, 2) + "), " + num(side, 2) + " from the box's middle across it (101.44), pawn_hit " + std::to_string(on.tracer.pawn_hit) +
                   ", normal (" + num(on.tracer.pawn_normal.x, 2) + ", " + num(on.tracer.pawn_normal.y, 2) + ", " + num(on.tracer.pawn_normal.z, 2) + "); a step later " +
                   std::to_string(effects_after - effects) + " effect (" + (effect_path.empty() ? "none" : effect_path) + "), sound " + (heard.empty() ? "none" : heard) +
                   " (the default material's is " + want + "), " + std::to_string(decals_after - decals) + " bullet holes; a bullet 220 uu over the box: pawn_hit " +
                   std::to_string(over.tracer.pawn_hit) + ", it ends " + num((over.tracer.end_pos - eye2).length(), 0) + " uu off");
        scene.helicopters.clear();
    }

    // --- Stumble -----------------------------------------------------------------------------------
    {
        // The sequences are in the sets the bots are drawn with: a staggered bot posed with each
        // stands differently from one posed with a name no set has (which falls back to HitHeavyHead).
        static const char* const kSequences[] = {"WalkBwdStop", "WalkFwdStop", "HitHeavyStomach", "HitHeavyHead"};
        bool have_all = true;
        std::string have_text;
        for (const char* weapon : {"Colt1911", "G36C"}) {
            const auto posed_with = [&](const char* sequence) {
                EnemyBot bot = make_bot("AITemplate_PatrolCop", weapon, bot_at);
                bot.stunned = true;
                bot.anim_state = EEnemyAnimState::HitStagger;
                bot.active_anim_seq = sequence;
                bot.anim_timer = 0.35f;
                bot.anim_duration = 0.7f;
                EnemyBodySet set;
                renderer.pose_enemy_bodies(bot, 0.0f, false, set);
                return set;
            };
            const EnemyBodySet fallback = posed_with("NoSuchSequence");
            for (const char* sequence : kSequences) {
                const EnemyBodySet set = posed_with(sequence);
                float apart = 0.0f;
                for (size_t i = 0; set.valid && fallback.valid && i < set.shapes.size() && i < fallback.shapes.size(); ++i) {
                    apart = std::max(apart, (set.shapes[i].centre - fallback.shapes[i].centre).length());
                }
                const bool there = std::string(sequence) == "HitHeavyHead" ? set.valid : apart > 1.0f;
                have_all = have_all && there;
                have_text += std::string(" ") + sequence + (there ? "" : " MISSING");
            }
            have_text += std::string(" with a ") + weapon + ";";
        }
        report("Stumble", have_all, "the four sequences in the bots' sets:" + have_text);

        struct Case {
            const char* name;
            const char* weapon;  // hers
            float bot_turn;      // his yaw off facing her: 0 in front of her, 180 with his back to her
            int aim;
            uint8_t stumble;
            const char* sequence;
        };
        static const Case kCases[] = {
            {"a pistol from in front", "Colt1911", 0.0f, kAimBelly, 14, "WalkBwdStop"},
            {"a pistol from behind", "Colt1911", 150.0f, kAimBelly, 16, "WalkFwdStop"},
            {"a rifle from in front", "G36C", 0.0f, kAimBelly, 14, "WalkBwdStop"},
            {"a shotgun from in front at a thigh", "Remington870", 0.0f, kAimThigh, 17, "HitHeavyStomach"},
            {"a shotgun from in front at the head", "Remington870", 0.0f, kAimHead, 18, "HitHeavyHead"},
            {"a shotgun from behind", "Remington870", 180.0f, kAimBelly, 0, ""},
        };
        bool ok = true;
        std::string text;
        for (const Case& c : kCases) {
            shooter.equip_weapon(c.weapon);
            EnemyBot bot = make_bot("AITemplate_Support", "FNMinimi", bot_at);
            bot.health = bot.max_health = 100000.0f;
            bot.yaw_deg += c.bot_turn;
            const Pull p = pull_at({bot}, at_bot(bot, c.aim));
            const float her_yaw = shooter.get_body_yaw();
            const bool turn_ok = c.stumble == 14   ? std::abs(std::fmod(p.after.combat.turn_to_yaw - (her_yaw + 180.0f) + 540.0f, 360.0f) - 180.0f) < 0.01f
                                 : c.stumble == 16 ? std::abs(std::fmod(p.after.combat.turn_to_yaw - her_yaw + 540.0f, 360.0f) - 180.0f) < 0.01f
                                                   : p.after.combat.turn_left == 0.0f;
            const bool case_ok = p.fired && p.damage > 0.0f && p.after.combat.stumble == c.stumble && p.after.active_anim_seq == c.sequence &&
                                 p.after.stunned == (c.stumble != 0) && turn_ok;
            ok = ok && case_ok;
            text += std::string(text.empty() ? "" : "; ") + c.name + ": " + (p.after.active_anim_seq.empty() ? "no sequence" : p.after.active_anim_seq) + " (state " +
                    std::to_string(p.after.combat.stumble) + (case_ok ? ")" : ") WRONG");
        }
        report("Stumble", ok, text);

        // The stumble as it runs: a second bullet does not begin it again; he has turned to face
        // the way opposite her facing after 0.3 s; it is over when the sequence is (0.7 s).
        shooter.equip_weapon("Colt1911");
        EnemyBot bot = make_bot("AITemplate_Support", "FNMinimi", bot_at);
        bot.health = bot.max_health = 100000.0f;
        bot.yaw_deg += 40.0f;  // not quite facing her
        Pull p = pull_at({bot}, at_bot(bot, kAimBelly));
        const float her_yaw = shooter.get_body_yaw();
        float second_timer = -1.0f, yaw_error = 999.0f, ended_at = -1.0f;
        bool second_kept = false;
        float t = kDt;
        for (int f = 0; f < 80; ++f, t += kDt) {
            if (f == 20) {
                // 0.35 s in: the same line again, with no wait (the Colt's interval is long over).
                const float timer = scene.enemies[0].anim_timer;
                const float health = scene.enemies[0].health;
                const Vec3 eye = shooter.get_position() + Vec3(0.0f, 0.0f, shooter.get_telemetry().eye_height);
                const Vec3 dir = (bot_point(scene.enemies[0], kAimBelly) - eye).normalized();
                shooter.set_rotation(std::atan2(dir.y, dir.x) * RAD2DEG, std::asin(std::clamp(dir.z, -1.0f, 1.0f)) * RAD2DEG, 0.0f);
                shooter.step(fire, kDt, scene);
                second_timer = scene.enemies[0].anim_timer;
                second_kept = scene.enemies[0].health < health && second_timer > timer && scene.enemies[0].combat.stumble == 14 &&
                              scene.enemies[0].active_anim_seq == "WalkBwdStop";
            } else {
                shooter.step(idle, kDt, scene);
            }
            if (f == 19) yaw_error = std::abs(std::fmod(scene.enemies[0].yaw_deg - (her_yaw + 180.0f) + 540.0f, 360.0f) - 180.0f);
            if (ended_at < 0.0f && !scene.enemies[0].stunned) ended_at = t;
        }
        const bool run_ok = p.after.combat.stumble == 14 && second_kept && yaw_error < 0.5f && ended_at > 0.7f - 2.0f * kDt && ended_at < 0.7f + 3.0f * kDt &&
                            scene.enemies[0].combat.stumble == 0;
        report("Stumble", run_ok,
               "a pistol's stumble as it runs: a second bullet 0.35 s in " + std::string(second_kept ? "hurts him and leaves it running" : "BEGINS IT AGAIN") +
                   " (its time " + num(second_timer, 2) + " s); 0.33 s in he is " + num(yaw_error, 2) + " degrees from the way opposite her facing; over at " +
                   num(ended_at, 2) + " s (the sequence is 0.7)");
    }

    // --- Survival ----------------------------------------------------------------------------------
    if (have_open) {
        // She stands still in the open, one patrol cop 600 uu off: until she is dead.
        std::string text = "she stands still, one patrol cop (Colt 1911) 600 uu off:";
        bool ok = true;
        float deaths[3] = {-1.0f, -1.0f, -1.0f};
        for (int d = 0; d < 3; ++d) {
            const Run r = stand_under_fire(cop, d, 120.0f, nullptr);
            deaths[d] = r.death;
            ok = ok && r.death > 0.0f;
            text += std::string(d ? "; " : " ") + kLevels[d] + ": dead at " + num(r.death, 2) + " s after " + std::to_string(r.shots) + " shots, " + std::to_string(r.landed) +
                    " on her, " + std::to_string(r.counted) + " counted (" + (r.hits.empty() ? std::string("none") : num(r.hits.front(), 0)) + " each), the first at " +
                    num(r.first_counted, 2) + " s";
        }
        // The harder, the sooner.
        ok = ok && deaths[0] > deaths[1] && deaths[1] >= deaths[2];
        report("Survival", ok, text);
    }

    std::cout << std::defaultfloat << std::setprecision(6);
    std::cout << "  -> Stage 27 Result: " << (pass ? "PASS" : "FAIL") << std::endl;
    scene.enemies = enemies_before;
    scene.helicopters = helicopters_before;
    scene.active_tracers = tracers_before;
    scene.dropped_weapons = dropped_before;
    scene.spawned_effects = effects_before;
    scene.dynamic_decals = decals_before;
    return pass;
}

// -----------------------------------------------------------------------------
// Where a restart puts the player (TdSPStoryGame.RestartPlayer): at every place a level can restart
// her (the TdCheckpoints of a level with a script, the listed stages of one without) she is put as
// a restart puts her and left alone until she rests. --verify-respawn runs it over every chapter;
// the oracle over two levels it has loaded. ME_RESPAWN_DEBUG=1 lists every place and every lift,
// ME_RESPAWN_AROUND=1 what is round a failing one; --verify-respawn takes ME_RESPAWN_RULE=<units>,
// her feet that far over the checkpoint's place with no search for a floor (35 was the rule once).
// -----------------------------------------------------------------------------
struct RespawnCheck {
    int checkpoints = 0;
    int stood = 0;      // put on the floor under the spot, and still there
    int in_cab = 0;     // put into a lift's cab (it takes her off, or stands at the top of its run)
    int on_mover = 0;   // on an actor the script's Matinees move, which the checkpoint's events put there or which carries her
    int fell = 0;       // nothing under the spot within reach: she fell, and lived
    int fell_dead = 0;  // ... and died of it
    int known = 0;      // the places of kRestartsOnMovers
    int failed = 0;     // put standing, and not left standing there
    float worst_drift = 0.0f;
};

// Checkpoints whose place the port cannot yet make sense of (map, name, why). Reported, not counted.
// (Jacknife's `construction` and Ropeburn's `Train_Ride_End` were here until the harness ran the
// level's script as a restart does: the first stands on a girder the checkpoint's events raise
// under her, the second on a train they give its collision.)
static const char* const kRestartsOnMovers[][3] = {
    {"Scraper_p", "Scraper_Start", "its spot is 140 under the plaza's floor; she is pushed up onto it"},
};

static RespawnCheck check_checkpoint_respawns(const me::LevelScene& scene, const me::MovementConfig& move_cfg, const std::string& label,
                                              const char* rule_spec = nullptr) {
    using namespace me;
    static const bool debug = std::getenv("ME_RESPAWN_DEBUG") != nullptr;
    RespawnCheck out;
    LevelScene world = scene;  // the lifts and doors move as she is stepped
    // Where she is put is what is looked at, not who is waiting there for her.
    world.enemies.clear();
    world.helicopters.clear();

    // The places a death restarts her in this level, each a start spot's Location.
    struct Spot {
        std::string name;
        Vec3 location;
        float yaw = 0.0f;
        std::vector<std::string> levels;  // TdCheckpoint.StreamingLevels: the sublevels in play there
    };
    std::vector<Spot> spots;
    if (scene.script_checkpoints) {
        // (The chapter's list also holds the time trials' and the tutorial's gates, which nobody restarts at.)
        for (const LevelCheckpointInfo& cp : scene.checkpoint_infos) {
            if (cp.object_name.rfind("TdCheckpoint", 0) == 0) spots.push_back({cp.checkpoint_name, cp.location, cp.rotation.to_degrees().y, cp.streaming_levels});
        }
    } else {
        for (size_t i = 0; i < scene.checkpoints.size(); ++i) {
            spots.push_back({"stage " + std::to_string(i + 1),
                             scene.checkpoints_are_feet ? ParkourController::start_spot_over_feet(scene.checkpoints[i]) : scene.checkpoints[i],
                             scene.player_spawn_yaw, {}});
        }
    }

    // In a level with a script a restart is the level's own (TdSPStoryGame.ResetLevel: the script levels
    // are loaded again, then TriggerEventsOnLevelReload): every actor the script moved, hid or changed is
    // as the level places it, and the level-loaded events and the checkpoint's own run its Kismet from
    // the start, with the sublevels of that checkpoint in play. As the game loop does it on a death.
    LevelScript script;
    const bool scripted = !rule_spec && scene.script_checkpoints && world.script && world.script->valid();
    const std::vector<std::string> default_levels = world.loaded_sublevel_packages;
    if (scripted) {
        ScriptHost host;
        bind_scene_effects(host, world);
        host.stream_levels = [&world, &script](const std::vector<std::string>& levels, bool load) {
            auto& streamed = world.loaded_sublevel_packages;
            for (const std::string& l : levels) {
                auto it = std::find_if(streamed.begin(), streamed.end(), [&](const std::string& s) { return lower(s) == lower(l); });
                if (load && it == streamed.end()) streamed.push_back(l);
                if (!load && it != streamed.end()) streamed.erase(it);
            }
            script.set_loaded_packages(streamed);
        };
        script.init(world.script, host);
    }

    for (const Spot& cp : spots) {
        ParkourController controller(move_cfg);
        if (rule_spec) {
            controller.reset(cp.location + Vec3(0.0f, 0.0f, static_cast<float>(std::atof(rule_spec))), cp.yaw);
        } else {
            if (scripted) {
                restore_script_actors(world);
                world.loaded_sublevel_packages = cp.levels.empty() ? default_levels : cp.levels;
                script.set_loaded_packages(world.loaded_sublevel_packages);
            }
            controller.restart_level_at(cp.location, cp.yaw, world);
            controller.set_checkpoint(cp.location, cp.yaw, 0, cp.name);
            if (scripted) script.begin_play(cp.name);
        }
        const Vec3 feet = controller.get_telemetry().position;
        const bool placed = controller.get_telemetry().grounded;
        // Room for her capsule where she is put: nothing between her knees and the top of her head.
        bool roofed = false;
        if (world.collision) {
            const CollisionHit above = world.collision->line_check(feet + Vec3(0.0f, 0.0f, 40.0f), feet + Vec3(0.0f, 0.0f, 2.0f * kPawnCollisionHeight), COLL_BlockNonZeroExtent);
            roofed = above.hit;
        }
        // Three seconds, and on for as long again three times while she is still on her way down (a
        // restart at the top of a shaft she is meant to drop through).
        bool died = false;
        for (int f = 0; f < 720; ++f) {
            controller.step(InputFrame{}, 1.0f / 60.0f, world);
            died = died || controller.get_telemetry().falling_to_death || controller.get_telemetry().health <= 0.0f;
            if (scripted) {
                ScriptPlayerState ps;
                ps.position = controller.get_telemetry().position;
                ps.eye = ps.position + Vec3(0.0f, 0.0f, controller.get_telemetry().eye_height);
                ps.view_dir = Rotator::from_degrees(0.0f, cp.yaw, 0.0f).forward();
                ps.dead = died;
                script.update(1.0f / 60.0f, ps);
                update_script_movers(world, 1.0f / 60.0f, &controller);
            }
            if (f >= 179 && (controller.get_telemetry().grounded || died)) break;
        }
        const PlayerTelemetry& t = controller.get_telemetry();
        const float drop = feet.z - t.position.z;
        const float drift = (t.position - feet).length_xy();
        // Standing on something the script's Matinees move (which may have carried her since).
        const int32_t base = controller.base_actor();
        const bool on_mover = base >= 0 && static_cast<size_t>(base) < world.actors.size() && world.actors[static_cast<size_t>(base)].mover >= 0;
        const char* known = nullptr;
        for (const auto& k : kRestartsOnMovers) {
            if (label.find(k[0]) != std::string::npos && cp.name == k[1]) known = k[2];
        }
        // She is put standing and stays (on the floor under the spot, at her hover over it); or into a
        // lift's cab; or, with nothing under the spot, she falls from it (the Prologue's Cops
        // checkpoint is the top of the shaft she drops down).
        bool ok = true;
        ++out.checkpoints;
        if (known) {
            ++out.known;
        } else if (roofed) {
            ok = false;
        } else if (t.in_elevator) {
            ok = !died && drift < 8.0f;
            out.in_cab += ok ? 1 : 0;
        } else if (on_mover && (!placed || std::abs(drop) >= 1.0f || drift >= 8.0f)) {
            // On one of the script's movers that the checkpoint's events put under her, or that carries her:
            // not far down onto one, nor carried far from the spot.
            ok = !died && drop < 200.0f && drift < 50.0f;
            out.on_mover += ok ? 1 : 0;
        } else if (placed) {
            ok = !died && t.grounded && std::abs(drop) < 1.0f && drift < 8.0f;
            out.stood += ok ? 1 : 0;
        } else {
            (died ? out.fell_dead : out.fell) += 1;
        }
        out.failed += ok ? 0 : 1;
        if (ok && !known && (placed || t.in_elevator)) out.worst_drift = std::max(out.worst_drift, drift);

        if (debug || !ok || known || (!placed && died)) {
            if (!ok && world.collision) {
                // What is under the spot: the level's floor, and what the port keeps apart or never moves.
                const CollisionHit under = world.collision->line_check(cp.location, cp.location - Vec3(0.0f, 0.0f, 4000.0f), COLL_BlockNonZeroExtent);
                std::cout << "  [respawn]   under the spot: " << (under.hit ? "the level " + std::to_string(cp.location.z - under.location.z) + " below (actor " + std::to_string(under.actor) + ")" : std::string("no level floor within 4000"));
                for (size_t ai = 0; ai < world.actors.size(); ++ai) {
                    const LevelActor& a = world.actors[ai];
                    if (!(a.dynamic_class || a.elevator >= 0 || a.barge_door >= 0)) continue;
                    const AABB& b = a.world_bounds;
                    if (b.min_pt.x > b.max_pt.x || cp.location.x < b.min_pt.x - 30.0f || cp.location.x > b.max_pt.x + 30.0f || cp.location.y < b.min_pt.y - 30.0f ||
                        cp.location.y > b.max_pt.y + 30.0f || b.max_pt.z > cp.location.z + 100.0f || b.max_pt.z < cp.location.z - 600.0f) {
                        continue;
                    }
                    std::cout << "; " << a.class_name << " " << a.source_package << ":" << a.unique_name << " (" << a.mesh_name << ") top " << cp.location.z - b.max_pt.z
                              << " below" << (a.elevator >= 0 ? ", a lift's part" : a.barge_door >= 0 ? ", a door" : "") << (a.is_hidden ? ", hidden" : "");
                }
                std::cout << std::endl;
                if (std::getenv("ME_RESPAWN_AROUND")) {
                    // The floor round the spot, every 30 units: what a capsule stands on (C) and what only a line meets (L).
                    for (int gy = 2; gy >= -2; --gy) {
                        std::cout << "  [respawn]     y " << (gy >= 0 ? "+" : "") << gy * 30 << ":";
                        for (int gx = -3; gx <= 3; ++gx) {
                            const Vec3 from = cp.location + Vec3(gx * 30.0f, gy * 30.0f, 0.0f);
                            const CollisionHit c = world.collision->line_check(from, from - Vec3(0.0f, 0.0f, 400.0f), COLL_BlockNonZeroExtent);
                            const CollisionHit l = world.collision->line_check(from, from - Vec3(0.0f, 0.0f, 400.0f), COLL_BlockZeroExtent | COLL_ShadowCast);
                            std::cout << "  C" << (c.hit ? std::to_string(static_cast<int>(cp.location.z - c.location.z)) : std::string("-")) << "/L"
                                      << (l.hit ? std::to_string(static_cast<int>(cp.location.z - l.location.z)) : std::string("-"));
                        }
                        std::cout << std::endl;
                    }
                }
            }
            std::cout << "  [respawn] " << label << " '" << cp.name << "' at (" << cp.location.x << ", " << cp.location.y << ", " << cp.location.z
                      << "): put at feet z " << feet.z << (placed ? " on a floor" : " in the air") << ", then at (" << t.position.x << ", " << t.position.y
                      << ", " << t.position.z << "), drop " << drop << ", drift " << drift << (t.grounded ? "" : ", in the air") << (t.in_elevator ? ", in a cab" : "")
                      << (on_mover ? ", on " + world.actors[static_cast<size_t>(base)].source_package + ":" + world.actors[static_cast<size_t>(base)].unique_name + ", which the script moves" : std::string())
                      << ", " << move_state_name(t.move_state) << (roofed ? ", something in her capsule where she is put" : "") << (died ? ", DEAD" : "")
                      << (known ? std::string("  (known: ") + known + ")" : ok ? std::string() : std::string("  <-- FAIL")) << std::endl;
        }
    }
    // The copy shares its collision worlds with `scene`: what the scripts switched there is put back.
    if (scripted) restore_script_actors(world);
    return out;
}

// A death goes back to where a restart goes, whatever she has stood on since. She is restarted at
// `home` (which makes it the active checkpoint: by set_checkpoint in a level with a script, by
// walking onto it in one without), moved to `elsewhere` as a teleport moves her, left to stand
// there a second (the void baseline follows her down), and her health is taken. Returns how far
// from where `home` put her the restart leaves her, or -1 when she does not come back.
static float death_returns_to(const me::LevelScene& scene, const me::MovementConfig& move_cfg, const me::Vec3& home, const me::Vec3& elsewhere) {
    using namespace me;
    LevelScene world = scene;
    world.enemies.clear();
    world.helicopters.clear();
    ParkourController controller(move_cfg);
    controller.restart_level_at(home, 0.0f, world);
    if (scene.script_checkpoints) controller.set_checkpoint(home, 0.0f, 0, "home");
    const Vec3 put = controller.get_telemetry().position;
    for (int f = 0; f < 30; ++f) controller.step(InputFrame{}, 1.0f / 60.0f, world);
    controller.restart_at(elsewhere, 0.0f, world);
    for (int f = 0; f < 60; ++f) controller.step(InputFrame{}, 1.0f / 60.0f, world);
    if ((controller.get_telemetry().position - put).length() < 100.0f) return -1.0f;  // she never left
    controller.get_telemetry().health = 0.0f;
    for (int f = 0; f < 300; ++f) {
        controller.step(InputFrame{}, 1.0f / 60.0f, world);
        if (controller.get_telemetry().respawned) return (controller.get_telemetry().position - put).length();
    }
    return -1.0f;
}

static int run_verify_respawn(const std::string& game_root) {
    using namespace me;
    MovementConfig move_cfg;
    load_movement_config_from_ini(get_config_path(game_root, "DefaultPawnMovement.ini"), move_cfg);
    static const char* const kMaps[] = {"Maps/SP00/Tutorial_p.me1", "Maps/SP01/Edge_p.me1",   "Maps/SP01/Escape_p.me1", "Maps/SP02/Stormdrain_p.me1",
                                        "Maps/SP03/Cranes_p.me1",   "Maps/SP04/Subway_p.me1", "Maps/SP05/Mall_p.me1",   "Maps/SP06/Factory_p.me1",
                                        "Maps/SP07/Boat_p.me1",     "Maps/SP08/Convoy_p.me1", "Maps/SP09/Scraper_p.me1"};
    RespawnCheck all;
    bool loaded = true;
    for (const char* map : kMaps) {
        LevelScene scene;
        if (!load_level_scene(game_root, map, scene)) {
            std::cout << "[Respawn] " << map << ": not loaded" << std::endl;
            loaded = false;
            continue;
        }
        if (std::getenv("ME_RESPAWN_DEBUG")) {
            for (const ElevatorInstance& e : scene.elevators) {
                std::cout << "  [respawn] lift " << e.name << ": (" << e.start_pos.x << ", " << e.start_pos.y << ", " << e.start_pos.z << ") -> ("
                          << e.end_pos.x << ", " << e.end_pos.y << ", " << e.end_pos.z << "), " << (e.move_frame_world ? "world keys" : "relative keys")
                          << ", " << e.parts.size() << " parts" << std::endl;
                for (const ElevatorPart& part : e.parts) {
                    std::cout << "  [respawn]   part " << part.actor_name << " role " << static_cast<int>(part.role) << " opens by (" << part.door_open_offset.x
                              << ", " << part.door_open_offset.y << ", " << part.door_open_offset.z << ")" << std::endl;
                }
            }
        }
        const RespawnCheck r = check_checkpoint_respawns(scene, move_cfg, map, std::getenv("ME_RESPAWN_RULE"));
        std::cout << "[Respawn] " << map << (scene.script_checkpoints ? "" : " (no script: its listed stages)") << ": " << r.checkpoints << " places: "
                  << r.stood << " stand where they are put, " << r.in_cab << " in a lift's cab, " << r.on_mover << " on something the script moves, " << r.fell
                  << " fall and live, " << r.fell_dead << " fall and die, " << r.known << " known, " << r.failed << " failed" << std::endl;
        all.checkpoints += r.checkpoints;
        all.stood += r.stood;
        all.in_cab += r.in_cab;
        all.on_mover += r.on_mover;
        all.fell += r.fell;
        all.fell_dead += r.fell_dead;
        all.known += r.known;
        all.failed += r.failed;
        all.worst_drift = std::max(all.worst_drift, r.worst_drift);
    }
    std::cout << "[Respawn] " << all.checkpoints << " places in " << (sizeof(kMaps) / sizeof(kMaps[0])) << " maps: " << all.stood
              << " stand where they are put, " << all.in_cab << " in a lift's cab, " << all.on_mover << " on something the script moves, " << all.fell
              << " fall and live, " << all.fell_dead << " fall and die, " << all.known << " known, " << all.failed << " failed; largest drift " << all.worst_drift
              << " uu" << std::endl;
    return loaded && all.failed == 0 && all.fell_dead == 0 ? 0 : 1;
}

// -----------------------------------------------------------------------------
// Oracle stage 29: the level script's Matinees on the actors they move (docs/GAMEPLAY_SCRIPTING_RE.md
// section 9; game/matinee_move.hpp, game/script_movers.hpp).
//   Eval:     the track evaluation against cooked keys worked out from the data: Flight's office door
//             (a world-framed turn), New Eden's lift hull (a relative track whose first key is far
//             from zero), Ropeburn's first train, Pirandello Kruger's construction lift (a world
//             track that ends on its first key), and the curve's rules on keys made here.
//   Playback: the executable's rules on those Matinees: Completed a tick after the end, Play again
//             without a rewind, Reverse, a Stop part way, a Stop on an idle one, Pause twice, a
//             loop, ForceStartPosition, a rewind, a linked PlayRate.
//   Door:     Flight's office door by its remote events: open, a line and a walk through the doorway,
//             shut again, and shut on her standing in its swing.
//   Ride:     Heat's gondola carrying her its whole way; Jacknife's girder raising her.
// --verify-movers runs it alone and then counts, chapter by chapter, what the Matinees move.
// -----------------------------------------------------------------------------
static bool oracle_matinee_movers(const std::string& game_root, const me::MovementConfig& move_cfg) {
    using namespace me;
    namespace fs = std::filesystem;
    const auto lower_of = [](std::string v) {
        std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return v;
    };
    // One package's Kismet alone: its Matinees and their keys, with no level round them.
    const auto lone_graph = [&](const char* rel) -> std::shared_ptr<ScriptGraph> {
        fs::path path = fs::path(game_root) / "TdGame" / "CookedPC" / rel;
        if (!fs::exists(path)) path = fs::path(game_root) / "CookedPC" / rel;
        if (!fs::exists(path)) path = fs::path(game_root) / rel;
        auto pkg = std::make_shared<UPKPackage>(path.string());
        if (!pkg->is_valid()) return nullptr;
        auto graph = std::make_shared<ScriptGraph>();
        std::vector<std::string> warnings;
        graph->load({pkg}, {}, warnings);
        return graph;
    };
    const auto find_op = [&](const ScriptGraph& g, const std::string& package, const std::string& name) {
        for (size_t i = 0; i < g.nodes.size(); ++i) {
            const ScriptGraph::Node& n = g.nodes[i];
            if (n.cls == "SeqAct_Interp" && n.name == name && g.packages[static_cast<size_t>(n.package)] == package) return static_cast<int>(i);
        }
        return -1;
    };
    // The movement track of the op's group that the named actor is linked to.
    const auto track_of = [&](const ScriptGraph& g, int op, const std::string& actor) -> const MoveTrack* {
        if (op < 0 || g.nodes[static_cast<size_t>(op)].matinee < 0) return nullptr;
        const ScriptGraph::Node& n = g.nodes[static_cast<size_t>(op)];
        for (const ScriptMatinee::Move& move : g.matinees[static_cast<size_t>(n.matinee)].moves) {
            for (const ScriptGraph::VarLink& link : n.vars) {
                if (lower_of(link.desc) != lower_of(move.group)) continue;
                for (int v : link.vars) {
                    if (v < 0 || g.nodes[static_cast<size_t>(v)].actor < 0) continue;
                    if (g.actors[static_cast<size_t>(g.nodes[static_cast<size_t>(v)].actor)].name == actor) return &move.track;
                }
            }
        }
        return nullptr;
    };
    const auto origin = [](const Mat4& m) { return Vec3(m.m[12], m.m[13], m.m[14]); };
    const auto yaw_of = [](const Mat4& m) { return std::atan2(m.m[1], m.m[0]) * RAD2DEG; };
    const auto turn_between = [](float a, float b) { return std::abs(std::remainder(a - b, 360.0f)); };
    const Mat4 unit = Mat4::identity();
    std::ostringstream note;
    note << std::fixed << std::setprecision(3);

    // --- Eval ---
    bool eval_ok = true;
    const std::shared_ptr<ScriptGraph> office = lone_graph("Maps/SP01/Escape_Off.me1");
    const std::shared_ptr<ScriptGraph> trains = lone_graph("Maps/SP04/Subway_Train_Spt.me1");
    {
        // Flight's office door: IMF_World, the place constant, yaw 270 at 0 and 140.63049 from 0.1 s
        // (49152 and 25601 rotation units as MakeFromEuler truncates them).
        const MoveTrack* t = office ? track_of(*office, find_op(*office, "escape_off", "SeqAct_Interp_0"), "InterpActor_13") : nullptr;
        Mat4 w0, w1;
        const bool ok = t && !t->relative && move_track_transform(*t, unit, unit, 0.0f, w0) && move_track_transform(*t, unit, unit, 0.15f, w1) &&
                        turn_between(yaw_of(w0), 270.0f) < 0.01f && turn_between(yaw_of(w1), 140.63049f) < 0.01f &&
                        (origin(w0) - Vec3(4214.003f, 7543.999f, 12672.0f)).length() < 0.02f && (origin(w1) - origin(w0)).length() < 0.02f;
        eval_ok = eval_ok && ok;
        if (t) note << "office door yaw " << (yaw_of(w0) < 0.0f ? yaw_of(w0) + 360.0f : yaw_of(w0)) << " -> " << yaw_of(w1) << ", moved " << (origin(w1) - origin(w0)).length();
        else note << "office door: no track";
    }
    {
        // New Eden's lift hull: relative, keys (-41823.95, 7719.99, -1151.99) -> (-41823.96, 7719.99, 127.99): it
        // rises 1280 from where it stands, and stands where it is at the start.
        const std::shared_ptr<ScriptGraph> mall = lone_graph("Maps/SP05/Mall_Mall_Spt.me1");
        const MoveTrack* t = mall ? track_of(*mall, find_op(*mall, "mall_mall_spt", "SeqAct_Interp_26"), "InterpActor_1") : nullptr;
        Mat4 w0, w1;
        const Mat4 initial = t ? move_initial_transform(*t, unit, 0.0f) : unit;
        const bool ok = t && t->relative && move_track_transform(*t, initial, unit, 0.0f, w0) && move_track_transform(*t, initial, unit, 10.0f, w1) &&
                        origin(w0).length() < 0.02f && (origin(w1) - Vec3(0.0f, 0.0f, 1280.0f)).length() < 0.02f;
        eval_ok = eval_ok && ok;
        if (t) note << "; lift hull at 0 s off by " << origin(w0).length() << ", at 10 s up " << origin(w1).z;
        else note << "; lift hull: no track";
    }
    {
        // Ropeburn's first train: its last key 34095.97 along -Y of the frame it is placed in.
        const MoveTrack* t = trains ? track_of(*trains, find_op(*trains, "subway_train_spt", "SeqAct_Interp_10"), "InterpActor_27") : nullptr;
        Mat4 w;
        const Mat4 initial = t ? move_initial_transform(*t, unit, 0.0f) : unit;
        const bool ok = t && t->relative && move_track_transform(*t, initial, unit, 18.0f, w) && std::abs(origin(w).y + 34095.97f) < 0.05f &&
                        std::abs(origin(w).x) < 0.05f && std::abs(origin(w).z) < 0.05f;
        eval_ok = eval_ok && ok;
        if (t) note << "; train at its end y " << origin(w).y;
        else note << "; train: no track";
    }
    {
        // Pirandello Kruger's construction lift: world-framed, four keys, back on its first at the end.
        const std::shared_ptr<ScriptGraph> arena = lone_graph("Maps/SP06/Factory_Arena_Spt.me1");
        const int op = arena ? find_op(*arena, "factory_arena_spt", "SeqAct_Interp_6") : -1;
        const MoveTrack* t = arena ? track_of(*arena, op, "InterpActor_7") : nullptr;
        const float length = t ? arena->matinees[static_cast<size_t>(arena->nodes[static_cast<size_t>(op)].matinee)].length : 0.0f;
        Mat4 w0, w1, at;
        float furthest = 0.0f;
        for (int k = 0; t && k <= 400; ++k) {
            if (move_track_transform(*t, unit, unit, length * static_cast<float>(k) / 400.0f, at)) furthest = std::max(furthest, (origin(at) - t->pos.front().value).length());
        }
        const bool ok = t && !t->relative && t->pos.size() == 4 && move_track_transform(*t, unit, unit, 0.0f, w0) &&
                        move_track_transform(*t, unit, unit, length, w1) && (origin(w0) - t->pos.front().value).length() < 0.01f &&
                        (origin(w1) - origin(w0)).length() < 0.01f && furthest > 1640.0f && furthest < 1660.0f;
        eval_ok = eval_ok && ok;
        if (t) note << "; construction lift ends " << (origin(w1) - origin(w0)).length() << " from its start, " << furthest << " away at its furthest";
        else note << "; construction lift: no track";
    }
    {
        // The curve itself: a held segment ends at the next key's own time, the last key is reached at
        // its time; a line; a cubic with level tangents; and the two ways of turning from yaw 0 to 270.
        const auto key = [](float time, float x, uint8_t mode) {
            MoveCurveKey k;
            k.time = time;
            k.value = Vec3(x, 0.0f, 0.0f);
            k.mode = mode;
            return k;
        };
        const std::vector<MoveCurveKey> held = {key(0.0f, 0.0f, 0), key(1.0f, 5.0f, 2), key(2.0f, 9.0f, 0)};
        const std::vector<MoveCurveKey> cubic = {key(0.0f, 0.0f, 1), key(1.0f, 10.0f, 1)};
        MoveTrack turn;
        turn.euler = {key(0.0f, 0.0f, 0), key(1.0f, 0.0f, 0)};
        turn.euler[1].value = Vec3(0.0f, 0.0f, 270.0f);
        const float by_euler = yaw_of(move_key_transform(turn, 0.5f));
        turn.quat = true;
        const float by_quat = yaw_of(move_key_transform(turn, 0.5f));
        const bool ok = eval_move_curve(held, 0.5f).x == 2.5f && eval_move_curve(held, 1.0f).x == 5.0f && eval_move_curve(held, 1.999f).x == 5.0f &&
                        eval_move_curve(held, 2.0f).x == 9.0f && eval_move_curve(held, 3.0f).x == 9.0f && eval_move_curve(held, -1.0f).x == 0.0f &&
                        std::abs(eval_move_curve(cubic, 0.5f).x - 5.0f) < 1.0e-4f && std::abs(eval_move_curve(cubic, 0.25f).x - 1.5625f) < 1.0e-4f &&
                        turn_between(by_euler, 135.0f) < 0.01f && turn_between(by_quat, -45.0f) < 0.01f;
        eval_ok = eval_ok && ok;
        note << "; yaw 0 to 270 half way: " << by_euler << " by its angles, " << by_quat << " by quaternion";
    }
    const std::string eval_note = note.str();
    note.str(std::string());

    // --- Playback ---
    bool play_ok = office != nullptr && trains != nullptr;
    if (play_ok) {
        ScriptPlayerState ps;
        LevelScript s;
        s.init(office, ScriptHost{});
        const int op = find_op(*office, "escape_off", "SeqAct_Interp_0");
        const float length = op >= 0 ? office->matinees[static_cast<size_t>(office->nodes[static_cast<size_t>(op)].matinee)].length : 0.0f;
        const float dt = 0.01f;
        const auto until_idle = [&](int limit) {
            int ticks = 0;
            while (ticks < limit && s.op_active(op)) {
                s.update(dt, ps);
                ++ticks;
            }
            return ticks;
        };
        play_ok = op >= 0 && std::abs(length - 0.15f) < 1.0e-4f;
        if (play_ok) {
            // Play to the end: the tick that reaches it fires nothing yet; Completed comes the tick after.
            s.activate_input(op, 0);
            int ticks = 0;
            do {
                s.update(dt, ps);
                ++ticks;
            } while (s.interp_playing(op) && ticks < 100);
            const bool at_end = std::abs(s.interp_position(op) - length) < 1.0e-5f && s.op_active(op) && s.interp_fired(op, 0) == 0;
            s.update(dt, ps);
            const bool completed = !s.op_active(op) && s.interp_fired(op, 0) == 1 && s.interp_fired(op, 1) == 0;
            // Play again, no rewind: it ends at once where it is and Completed fires again.
            s.activate_input(op, 0);
            s.update(dt, ps);
            until_idle(10);
            const bool again = s.interp_fired(op, 0) == 2 && std::abs(s.interp_position(op) - length) < 1.0e-5f;
            // Reverse: back to the start, output 1; and Reverse again at the start: output 1 once more.
            s.activate_input(op, 1);
            s.update(dt, ps);
            const bool backwards = s.interp_reversed(op) && s.interp_position(op) < length;
            until_idle(100);
            const bool reversed = backwards && s.interp_position(op) == 0.0f && s.interp_fired(op, 1) == 1 && s.interp_fired(op, 0) == 2;
            s.activate_input(op, 1);
            s.update(dt, ps);
            until_idle(10);
            const bool reversed_again = s.interp_position(op) == 0.0f && s.interp_fired(op, 1) == 2;
            // Stop part way: neither output; the Position stays.
            s.activate_input(op, 0);
            for (int i = 0; i < 5; ++i) s.update(dt, ps);
            const float part = s.interp_position(op);
            s.activate_input(op, 2);
            s.update(dt, ps);
            until_idle(10);
            const bool stopped = part > 0.03f && part < 0.07f && s.interp_position(op) == part && s.interp_fired(op, 0) == 2 && s.interp_fired(op, 1) == 2;
            // Stop on an idle Matinee: the op ends a tick later with no output.
            s.activate_input(op, 2);
            s.update(dt, ps);
            const bool idle_active = s.op_active(op);
            until_idle(10);
            const bool idle_stop = idle_active && s.interp_fired(op, 0) == 2 && s.interp_fired(op, 1) == 2 && s.interp_position(op) == part;
            // Pause twice: it stands, then goes on to its end.
            s.activate_input(op, 0);
            s.update(dt, ps);
            s.update(dt, ps);
            s.activate_input(op, 3);
            s.update(dt, ps);
            const float held = s.interp_position(op);
            for (int i = 0; i < 5; ++i) s.update(dt, ps);
            const bool paused = s.op_active(op) && s.interp_playing(op) && s.interp_position(op) == held && held > part && held < length;
            s.activate_input(op, 3);
            s.update(dt, ps);
            until_idle(100);
            const bool resumed = std::abs(s.interp_position(op) - length) < 1.0e-5f && s.interp_fired(op, 0) == 3;
            play_ok = at_end && completed && again && reversed && reversed_again && stopped && idle_stop && paused && resumed;
            note << "end reached after " << ticks << " ticks of 0.01 s, Completed the tick after " << (completed ? "yes" : "NO") << ", again without a rewind "
                 << (again ? "yes" : "NO") << ", reversed to 0 " << (reversed ? "yes" : "NO") << ", reversed at 0 " << (reversed_again ? "yes" : "NO")
                 << ", stopped at " << part << " with no output " << (stopped ? "yes" : "NO") << ", stop when idle silent " << (idle_stop ? "yes" : "NO")
                 << ", paused at " << held << " " << (paused ? "yes" : "NO") << ", resumed " << (resumed ? "yes" : "NO");
        }
        // A loop: the Training Area's turning sign (bLooping, PlayRate 0.05, 1 s long) goes round and never ends.
        const std::shared_ptr<ScriptGraph> sign = lone_graph("Maps/SP00/Tutorial_Art.me1");
        const int loop = sign ? find_op(*sign, "tutorial_art", "SeqAct_Interp_2") : -1;
        bool loop_ok = loop >= 0;
        if (loop_ok) {
            LevelScript l;
            l.init(sign, ScriptHost{});
            l.activate_input(loop, 0);
            bool always = true;
            float wrapped_from = 0.0f;
            int wraps = 0;
            for (int i = 0; i < 1500; ++i) {  // 50 s: two and a half times round
                const float before = l.interp_position(loop);
                l.update(1.0f / 30.0f, ps);
                always = always && l.op_active(loop) && l.interp_playing(loop) && l.interp_position(loop) <= 1.0f;
                if (l.interp_position(loop) < before) {
                    wrapped_from = before;
                    ++wraps;
                }
            }
            loop_ok = always && wraps == 2 && wrapped_from > 0.99f && l.interp_fired(loop, 0) == 0 && l.interp_fired(loop, 1) == 0;
            note << "; a loop went round " << wraps << " times in 50 s (last from " << wrapped_from << ") and is still playing " << (always ? "yes" : "NO");
        }
        // The trains' Kismet: a forced start (ForceStartPosition 3; its rate comes from the ride's own
        // variable), and the first train's loop, which its own Completed plays again: 18 s at twice
        // the rate, rewound to 0 each time (bRewindOnPlay).
        LevelScript t;
        t.init(trains, ScriptHost{});
        const int forced = find_op(*trains, "subway_train_spt", "SeqAct_Interp_0");
        const int rewind = find_op(*trains, "subway_train_spt", "SeqAct_Interp_10");
        bool train_ok = forced >= 0 && rewind >= 0;
        if (train_ok) {
            t.activate_input(forced, 0);
            t.activate_input(rewind, 0);
            t.update(1.0f / 60.0f, ps);
            const float forced_at = t.interp_position(forced);
            const float rate = t.interp_position(rewind) * 60.0f;  // PlayRate 2
            for (int i = 0; i < 600; ++i) t.update(1.0f / 60.0f, ps);  // 10 s: it ended at 9 s
            const bool rewound = t.interp_fired(rewind, 0) == 1 && t.interp_playing(rewind) && t.interp_position(rewind) > 0.5f && t.interp_position(rewind) < 3.0f;
            train_ok = forced_at >= 3.0f && forced_at < 3.1f && std::abs(rate - 2.0f) < 1.0e-3f && rewound;
            note << "; forced start at " << forced_at << ", rate " << rate << ", played again by its own Completed and rewound (at " << t.interp_position(rewind) << " a second later) "
                 << (rewound ? "yes" : "NO");
        }
        play_ok = play_ok && loop_ok && train_ok;
    }
    std::string play_note = note.str();
    note.str(std::string());

    // What a level needs to run its script against its movers and a pawn.
    struct Level {
        LevelScene scene;
        LevelScript script;
        std::vector<std::string> log;
        bool loaded = false;
    };
    const auto begin = [&](Level& level, const char* map) {
        level.loaded = load_level_scene(game_root, map, level.scene) && level.scene.script && level.scene.script->valid() && level.scene.collision;
        if (!level.loaded) return;
        level.scene.enemies.clear();
        level.scene.helicopters.clear();
        ScriptHost host;
        bind_scene_effects(host, level.scene);
        const bool debug = std::getenv("ME_SCRIPT_DEBUG") != nullptr;
        std::vector<std::string>* log = &level.log;
        host.log = [log, debug](const std::string& line) {
            log->push_back(line);
            if (debug) std::cout << "    " << line << std::endl;
        };
        level.script.init(level.scene.script, host);  // every package's sequence runs; no level-loaded events
    };
    const auto actor_named = [&](const LevelScene& scene, const std::string& package, const std::string& name) {
        for (size_t i = 0; i < scene.actors.size(); ++i) {
            if (scene.actors[i].unique_name == name && lower_of(scene.actors[i].source_package) == package) return static_cast<int>(i);
        }
        return -1;
    };
    const float dt = 1.0f / 60.0f;

    // --- Door ---
    bool door_ok = false;
    {
        auto level = std::make_unique<Level>();
        begin(*level, "Maps/SP01/Escape_p.me1");
        LevelScene& scene = level->scene;
        LevelScript& script = level->script;
        const int ai = level->loaded ? actor_named(scene, "escape_off", "InterpActor_13") : -1;
        const int mi = ai >= 0 ? scene.actors[static_cast<size_t>(ai)].mover : -1;
        if (mi < 0) {
            note << "the office door is not one of the script's movers" << (ai >= 0 && scene.actors[static_cast<size_t>(ai)].barge_door >= 0 ? " (still a door of the port's own)" : "");
        } else {
            const LevelActor& door = scene.actors[static_cast<size_t>(ai)];
            const ScriptMover& mover = scene.movers[static_cast<size_t>(mi)];
            ScriptPlayerState ps;
            ps.position = Vec3(0.0f, 0.0f, -100000.0f);
            const auto tick = [&](int frames, ParkourController* pawn = nullptr) {
                for (int i = 0; i < frames; ++i) {
                    if (pawn) pawn->step(InputFrame{}, dt, scene);
                    script.update(dt, ps);
                    update_script_movers(scene, dt, pawn);
                }
            };
            const Vec3 hinge = door.location;
            const Vec3 centre = (door.world_bounds.min_pt + door.world_bounds.max_pt) * 0.5f;
            const Vec3 size = door.world_bounds.max_pt - door.world_bounds.min_pt;
            const Vec3 n = size.x <= size.y ? Vec3(1.0f, 0.0f, 0.0f) : Vec3(0.0f, 1.0f, 0.0f);  // the way through the doorway
            const auto on_line = [&]() { return find_impact_surface(scene, centre + n * 80.0f, centre - n * 80.0f).actor == ai; };
            // A walk at the doorway from 150 uu out on either side: how far past its middle she gets (negative: not past).
            const auto walk_through = [&](float side) {
                const ImpactSurface floor = find_impact_surface(scene, centre + n * (side * 150.0f), centre + n * (side * 150.0f) - Vec3(0.0f, 0.0f, 400.0f));
                if (!floor.hit) return -1.0e9f;
                ParkourController walker(move_cfg);
                walker.reset(floor.location + Vec3(0.0f, 0.0f, 4.0f), std::atan2(-n.y * side, -n.x * side) * RAD2DEG);
                float past = -1.0e9f;
                for (int f = 0; f < 150; ++f) {
                    InputFrame in{};
                    in.forward = 1.0f;
                    walker.step(in, dt, scene);
                    past = std::max(past, -(walker.get_position() - centre).dot(n * side));
                }
                return past;
            };
            const bool shut_line = on_line();
            const float shut_walk = std::max(walk_through(1.0f), walk_through(-1.0f));
            script.fire_remote_event("Open_Pope_Door");
            tick(18);  // 0.3 s: the Matinee is 0.15 s long
            const float opened = yaw_of(mover.delta);
            const float hinge_off = (mover.delta.transform_point(hinge) - hinge).length();
            const bool open_line = on_line();
            const float open_walk = std::max(walk_through(1.0f), walk_through(-1.0f));
            script.fire_remote_event("Close_Pope_Door");
            tick(18);
            const bool shut_again = !mover.moved && on_line();
            // Open once more (its events have had their one firing: by the op's own inputs now), and shut
            // with her standing where the leaf swings: 40 degrees short of shut, at the leaf's middle.
            // She is pushed along by it; it stops for her only if she cannot get out.
            const int door_op = find_op(*scene.script, "escape_off", "SeqAct_Interp_0");
            script.activate_input(door_op, 0);
            tick(18);
            const float sign = opened < 0.0f ? -1.0f : 1.0f;
            const float at = sign * 40.0f * DEG2RAD;
            const Vec3 arm = centre - hinge;
            const Vec3 spot(hinge.x + std::cos(at) * arm.x - std::sin(at) * arm.y, hinge.y + std::sin(at) * arm.x + std::cos(at) * arm.y, centre.z);
            const ImpactSurface under = find_impact_surface(scene, spot, spot - Vec3(0.0f, 0.0f, 400.0f));
            ParkourController stander(move_cfg);
            stander.reset((under.hit ? under.location : spot) + Vec3(0.0f, 0.0f, 4.0f), 0.0f);
            tick(20, &stander);
            const Vec3 stood = stander.get_position();
            const float open_before = yaw_of(mover.delta);
            script.activate_input(door_op, 1);
            if (std::getenv("ME_MOVER_DEBUG")) {
                std::cout << "    door hinge (" << hinge.x << ", " << hinge.y << ", " << hinge.z << ") middle (" << centre.x << ", " << centre.y << ", " << centre.z
                          << ") spot (" << spot.x << ", " << spot.y << ", " << spot.z << ") floor " << (under.hit ? under.location.z : -1.0f) << " stood ("
                          << stood.x << ", " << stood.y << ", " << stood.z << ") grounded " << stander.is_grounded() << std::endl;
                for (int f = 0; f < 12; ++f) {
                    tick(1, &stander);
                    const Vec3 p = stander.get_position();
                    std::cout << "    tick " << f << ": door " << yaw_of(mover.delta) << " deg, she (" << p.x << ", " << p.y << ", " << p.z << ")" << std::endl;
                }
            }
            tick(40, &stander);
            const float pushed = (stander.get_position() - stood).length_xy();
            const float left_open = yaw_of(mover.delta);
            // Either it shut and she was moved out of its swing, or it stands part open against her.
            const bool in_way_ok = turn_between(open_before, opened) < 0.02f && std::isfinite(pushed) &&
                                   ((!mover.moved && pushed > 5.0f) || (mover.moved && mover.stop_on_encroach));
            door_ok = shut_line && shut_walk < 0.0f && turn_between(opened, -129.37f) < 0.02f && hinge_off < 0.05f && !open_line && open_walk > 40.0f &&
                      shut_again && in_way_ok;
            note << "shut: on the line " << (shut_line ? "yes" : "NO") << ", a walk gets " << shut_walk << " past; open: turned " << opened
                 << " deg about its hinge (off by " << hinge_off << "), on the line " << (open_line ? "YES" : "no") << ", a walk gets " << open_walk
                 << " past; shut again " << (shut_again ? "yes" : "NO") << "; shut on her: pushed " << pushed << " uu, the door left at " << left_open
                 << " deg (bStopOnEncroach " << (mover.stop_on_encroach ? "on" : "off") << ")";
        }
        // A Matinee whose PlayRate comes from a linked SeqVar_RandomFloat plays at a rate between its Min and Max.
        if (level->loaded) {
            const ScriptGraph& g = *scene.script;
            bool found = false, in_range = false;
            float rate = 0.0f, lo = 0.0f, hi = 0.0f;
            for (size_t i = 0; i < g.nodes.size() && !found; ++i) {
                const ScriptGraph::Node& node = g.nodes[i];
                if (node.cls != "SeqAct_Interp" || node.matinee < 0 || node.b2 || node.b6 || g.matinees[static_cast<size_t>(node.matinee)].length < 1.0f) continue;
                for (const ScriptGraph::VarLink& link : node.vars) {
                    if (lower_of(link.desc) != "playrate" || link.vars.empty() || link.vars.front() < 0) continue;
                    const ScriptGraph::Node& var = g.nodes[static_cast<size_t>(link.vars.front())];
                    if (var.cls != "SeqVar_RandomFloat" || var.f2 <= var.f) continue;
                    ScriptPlayerState ps;
                    script.activate_input(static_cast<int>(i), 0);
                    script.update(dt, ps);
                    rate = script.interp_position(static_cast<int>(i)) / dt;
                    lo = var.f;
                    hi = var.f2;
                    in_range = rate >= lo - 1.0e-3f && rate <= hi + 1.0e-3f;
                    found = true;
                }
            }
            play_ok = play_ok && found && in_range;
            std::ostringstream linked;
            linked << std::fixed << std::setprecision(3) << "; a linked PlayRate drew " << rate << " from " << lo << ".." << hi << (found && in_range ? "" : " (FAILED)");
            play_note += linked.str();
        }
    }
    const std::string door_note = note.str();
    note.str(std::string());

    // --- Ride ---
    bool ride_ok = false, rise_ok = false;
    // She is set on a mover's top and left to settle; then the script and the movers run with her.
    const auto stand_on = [&](LevelScene& scene, int actor, ParkourController& pawn, const Vec3* over = nullptr) {
        const ScriptMover& mover = scene.movers[static_cast<size_t>(scene.actors[static_cast<size_t>(actor)].mover)];
        const Vec3 middle = over ? *over : mover.delta.transform_point((scene.actors[static_cast<size_t>(actor)].world_bounds.min_pt + scene.actors[static_cast<size_t>(actor)].world_bounds.max_pt) * 0.5f);
        // Her feet at that point, and down onto whatever of it is under her there.
        pawn.reset(middle, 0.0f);
        for (int f = 0; f < 150 && (f < 30 || !pawn.is_grounded()); ++f) {
            pawn.step(InputFrame{}, dt, scene);
            update_script_movers(scene, dt, &pawn);
        }
        return pawn.base_actor() == actor;
    };
    {
        // Heat's window-cleaner gondola: 1352 uu along its own X in 10 s at PlayRate 2.5.
        auto level = std::make_unique<Level>();
        begin(*level, "Maps/SP03/Cranes_p.me1");
        LevelScene& scene = level->scene;
        const int ai = level->loaded ? actor_named(scene, "cranes_puzz_spt", "InterpActor_2") : -1;
        const int op = level->loaded ? find_op(*scene.script, "cranes_puzz_spt", "SeqAct_Interp_0") : -1;
        if (ai < 0 || op < 0 || scene.actors[static_cast<size_t>(ai)].mover < 0) {
            note << "the gondola is not one of the script's movers";
        } else {
            const ScriptMover& mover = scene.movers[static_cast<size_t>(scene.actors[static_cast<size_t>(ai)].mover)];
            ParkourController rider(move_cfg);
            const bool on_it = stand_on(scene, ai, rider);
            const Vec3 her_start = rider.get_position(), its_start = origin(mover.actor_tm);
            ScriptPlayerState ps;
            ps.position = Vec3(0.0f, 0.0f, -100000.0f);
            level->script.activate_input(op, 0);
            bool rode = on_it;
            if (std::getenv("ME_MOVER_DEBUG")) {
                const LevelActor& a = scene.actors[static_cast<size_t>(ai)];
                std::cout << "    gondola " << a.mesh_name << " at (" << its_start.x << ", " << its_start.y << ", " << its_start.z << ") yaw " << a.rotation.to_degrees().y << " box ("
                          << a.world_bounds.min_pt.x << ", " << a.world_bounds.min_pt.y << ", " << a.world_bounds.min_pt.z << ") .. (" << a.world_bounds.max_pt.x << ", "
                          << a.world_bounds.max_pt.y << ", " << a.world_bounds.max_pt.z << "), she at (" << her_start.x << ", " << her_start.y << ", " << her_start.z << ")" << std::endl;
                for (const LevelActor& o : scene.actors) {
                    if (o.unique_name != "StaticMeshActor_5" && o.unique_name != "StaticMeshActor_116") continue;
                    if ((o.location - a.location).length() > 3000.0f) continue;
                    std::cout << "      " << o.source_package << ":" << o.unique_name << " " << o.mesh_name << " box (" << o.world_bounds.min_pt.x << ", " << o.world_bounds.min_pt.y << ", "
                              << o.world_bounds.min_pt.z << ") .. (" << o.world_bounds.max_pt.x << ", " << o.world_bounds.max_pt.y << ", " << o.world_bounds.max_pt.z << ") base '" << o.base_name << "'" << std::endl;
                }
            }
            for (int f = 0; f < 300; ++f) {
                rider.step(InputFrame{}, dt, scene);
                level->script.update(dt, ps);
                const int base = rider.base_actor();
                update_script_movers(scene, dt, &rider);
                if (std::getenv("ME_MOVER_DEBUG") && (base != ai || f < 3)) {
                    const Vec3 p = rider.get_position(), o = origin(mover.actor_tm);
                    std::cout << "    frame " << f << ": base " << base << " (" << (base >= 0 ? scene.actors[static_cast<size_t>(base)].unique_name : std::string("none")) << "), she (" << p.x - her_start.x << ", " << p.y - her_start.y << ", " << p.z - her_start.z
                              << ") it (" << o.x - its_start.x << ", " << o.y - its_start.y << ", " << o.z - its_start.z << ") " << move_state_name(rider.get_move_state()) << std::endl;
                }
                rode = rode && base == ai;
            }
            const float its_way = (origin(mover.actor_tm) - its_start).length(), her_way = (rider.get_position() - her_start).length();
            const float apart = ((rider.get_position() - her_start) - (origin(mover.actor_tm) - its_start)).length();
            ride_ok = on_it && rode && std::abs(its_way - 1352.0f) < 1.0f && apart < 2.0f;
            // And back by the Reverse input, without her.
            level->script.activate_input(op, 1);
            for (int f = 0; f < 300; ++f) {
                level->script.update(dt, ps);
                update_script_movers(scene, dt);
            }
            const float back = (origin(mover.actor_tm) - its_start).length();
            ride_ok = ride_ok && back < 0.05f;
            note << "the gondola went " << its_way << " uu and she " << her_way << " with it (" << apart << " apart, standing on it " << (rode ? "throughout" : "NOT throughout")
                 << "); reversed it is " << back << " from its start";
        }
    }
    {
        // Jacknife's girder: Rising_Girder_Event gives the hidden twin its collision and plays its Matinee
        // from ForceStartPosition 3.1 of 5 at a tenth of the rate; she stands on it and is raised.
        auto level = std::make_unique<Level>();
        begin(*level, "Maps/SP02/Stormdrain_p.me1");
        LevelScene& scene = level->scene;
        const int ai = level->loaded ? actor_named(scene, "stormdrain_stde_spt", "InterpActor_10") : -1;
        if (ai < 0 || scene.actors[static_cast<size_t>(ai)].mover < 0) {
            note << "; the girder is not one of the script's movers";
        } else {
            const ScriptMover& mover = scene.movers[static_cast<size_t>(scene.actors[static_cast<size_t>(ai)].mover)];
            ScriptPlayerState ps;
            ps.position = Vec3(0.0f, 0.0f, -100000.0f);
            const float placed_z = origin(mover.actor_tm).z;
            level->script.fire_remote_event("Rising_Girder_Event");
            for (int f = 0; f < 3; ++f) {
                level->script.update(dt, ps);
                update_script_movers(scene, dt);
            }
            const float jumped = origin(mover.actor_tm).z - placed_z;
            ParkourController rider(move_cfg);
            // (Put as a restart at the chapter's `construction` checkpoint puts her: her centre at its place.)
            const Vec3 over(2592.46f, -8019.43f, -2344.0f - kPawnCollisionHeight);
            const bool on_it = stand_on(scene, ai, rider, &over);
            const float her_z = rider.get_position().z, its_z = origin(mover.actor_tm).z;
            if (std::getenv("ME_MOVER_DEBUG")) {
                const Vec3 p = rider.get_position(), o = origin(mover.actor_tm);
                std::cout << "    girder at (" << o.x << ", " << o.y << ", " << o.z << "), collidable " << scene.actors[static_cast<size_t>(ai)].is_collidable << ", collision "
                          << (mover.collision ? mover.collision->triangle_count() : 0) << " triangles, she at (" << p.x << ", " << p.y << ", " << p.z << ") on it " << on_it
                          << " base " << rider.base_actor() << std::endl;
            }
            bool rode = on_it;
            for (int f = 0; f < 300; ++f) {
                rider.step(InputFrame{}, dt, scene);
                level->script.update(dt, ps);
                update_script_movers(scene, dt, &rider);
                rode = rode && rider.base_actor() == ai;
            }
            const float its_rise = origin(mover.actor_tm).z - its_z, her_rise = rider.get_position().z - her_z;
            rise_ok = on_it && rode && scene.actors[static_cast<size_t>(ai)].is_collidable && jumped > 4000.0f && its_rise > 100.0f && std::abs(her_rise - its_rise) < 2.0f;
            note << "; the girder starts " << jumped << " up (ForceStartPosition), then rises " << its_rise << " in 5 s and she " << her_rise << " on it"
                 << (rode ? "" : " (NOT standing on it throughout)");
        }
    }
    const std::string ride_note = note.str();

    const bool pass = eval_ok && play_ok && door_ok && ride_ok && rise_ok;
    std::cout << "  -> Stage 29 Result: " << (pass ? "PASS" : "FAIL") << " (Eval=" << (eval_ok ? "OK" : "FAIL") << " [" << eval_note << "], Playback="
              << (play_ok ? "OK" : "FAIL") << " [" << play_note << "], Door=" << (door_ok ? "OK" : "FAIL") << " [" << door_note << "], Ride="
              << (ride_ok && rise_ok ? "OK" : "FAIL") << " [" << ride_note << "])" << std::endl;
    return pass;
}

// --verify-movers: stage 29, then what the Matinees move in every chapter (ME_MOVER_DEBUG=1 names
// each blocking one that nothing moves).
static int run_verify_movers(const std::string& game_root) {
    using namespace me;
    MovementConfig move_cfg;
    load_movement_config_from_ini(get_config_path(game_root, "DefaultPawnMovement.ini"), move_cfg);
    std::cout << "[Oracle Stage 29] Testing the Level Script's Matinees on the Actors They Move..." << std::endl;
    const bool pass = oracle_matinee_movers(game_root, move_cfg);
    static const char* const kMaps[] = {"Maps/SP00/Tutorial_p.me1", "Maps/SP01/Edge_p.me1",   "Maps/SP01/Escape_p.me1", "Maps/SP02/Stormdrain_p.me1",
                                        "Maps/SP03/Cranes_p.me1",   "Maps/SP04/Subway_p.me1", "Maps/SP05/Mall_p.me1",   "Maps/SP06/Factory_p.me1",
                                        "Maps/SP07/Boat_p.me1",     "Maps/SP08/Convoy_p.me1", "Maps/SP09/Scraper_p.me1"};
    ScriptMoverCensus all;
    size_t movers = 0, colliding = 0;
    for (const char* map : kMaps) {
        LevelScene scene;
        if (!load_level_scene(game_root, map, scene)) {
            std::cout << "[Movers] " << map << ": not loaded" << std::endl;
            continue;
        }
        const ScriptMoverCensus c = script_mover_census(scene);
        size_t with_collision = 0;
        for (const ScriptMover& m : scene.movers) with_collision += m.collision ? 1 : 0;
        std::cout << "[Movers] " << map << (scene.script ? "" : " (no script is run)") << ": " << c.matinees << " Matinees with a movement track on a placed actor: " << c.moved
                  << " move the script's movers, " << c.machines << " only what the port's own machines move, " << c.other << " only actors that are not moved; "
                  << c.blocking << " have something that blocks: " << c.blocking_moved << " moved, " << c.blocking_machines << " the machines'; " << scene.movers.size()
                  << " movers, " << with_collision << " with collision";
        if (!c.other_classes.empty()) {
            std::cout << "; not moved:";
            for (const auto& [cls, count] : c.other_classes) std::cout << " " << cls << " " << count;
        }
        std::cout << std::endl;
        if (std::getenv("ME_MOVER_DEBUG")) {
            for (const std::string& name : c.blocking_unmoved) std::cout << "[Movers]   blocking, not moved: " << name << std::endl;
            for (const auto& [package, count] : c.by_package) std::cout << "[Movers]   package " << package << " " << count << std::endl;
            for (const std::string& name : c.counted) std::cout << "[Movers]   op " << name << std::endl;
            if (scene.script) {
                std::cout << "[Movers]   script packages:";
                for (const std::string& p : scene.script->packages) std::cout << " " << p;
                std::cout << std::endl;
            }
        }
        all.matinees += c.matinees;
        all.moved += c.moved;
        all.machines += c.machines;
        all.other += c.other;
        all.blocking += c.blocking;
        all.blocking_moved += c.blocking_moved;
        all.blocking_machines += c.blocking_machines;
        movers += scene.movers.size();
        colliding += with_collision;
    }
    std::cout << "[Movers] all chapters: " << all.matinees << " Matinees with a movement track on a placed actor in the packages the port loads: " << all.moved
              << " move the script's movers, " << all.machines << " only what the port's own machines move, " << all.other << " only actors that are not moved; "
              << all.blocking << " have something that blocks: " << all.blocking_moved << " moved, " << all.blocking_machines << " the machines', "
              << all.blocking - all.blocking_moved - all.blocking_machines << " not moved; " << movers << " movers, " << colliding << " with collision" << std::endl;
    return pass ? 0 : 1;
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
    // As in the game: the bullets of stage 7 meet the enemies' posed bodies.
    controller.set_enemy_body_poser([&renderer](const EnemyBot& bot, float sim_time, bool reaction_disarm, EnemyBodySet& out) {
        return renderer.pose_enemy_bodies(bot, sim_time, reaction_disarm, out);
    });
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

    // Stage 23: the loudness of sounds at a place (oracle_sound_attenuation above).
    std::cout << "[Oracle Stage 23] Testing Sound Attenuation with Distance (the cues' own curves, per wave)..." << std::endl;
    const bool s23_pass = oracle_sound_attenuation(game_root);
    // Stage 24: the player's bullets against an enemy's posed bodies (oracle_enemy_bodies above).
    std::cout << "[Oracle Stage 24] Testing the Player's Bullets against an Enemy's Posed Bodies (Male3p_Physics)..." << std::endl;
    const bool s24_pass = oracle_enemy_bodies(renderer, sim_scene, move_cfg);
    // Stage 25: where a restart at a checkpoint puts her (check_checkpoint_respawns above), in the
    // two levels loaded here; --verify-respawn does every chapter.
    std::cout << "[Oracle Stage 25] Testing Restarts at the Checkpoints (TdSPStoryGame.RestartPlayer)..." << std::endl;
    bool s25_pass = false;
    {
        const RespawnCheck tutorial = check_checkpoint_respawns(sp00_scene, move_cfg, "Tutorial_p");
        const RespawnCheck edge = check_checkpoint_respawns(sp01_scene, move_cfg, "Edge_p");
        // Every stage of the Training Area and nine of the Prologue's ten checkpoints stand her on
        // their floor; the tenth, Cops, is the top of the shaft she drops down (how that fall ends
        // is not this stage's to judge).
        // A death: in the Training Area back at the last stage she walked onto, from a stage's start
        // that lies lower and comes earlier in the list (so that it does not become the active one);
        // in the Prologue back at After_Intro from Rooftop_Action's place, 1896 lower.
        float tutorial_back = -1.0f, tutorial_lower = 0.0f;
        for (size_t i = sp00_scene.checkpoints.size(); i-- > 1 && tutorial_back < 0.0f;) {
            for (size_t j = 0; j < i && tutorial_back < 0.0f; ++j) {
                if (sp00_scene.checkpoints[j].z > sp00_scene.checkpoints[i].z - 200.0f) continue;
                tutorial_lower = sp00_scene.checkpoints[i].z - sp00_scene.checkpoints[j].z;
                tutorial_back = death_returns_to(sp00_scene, move_cfg, ParkourController::start_spot_over_feet(sp00_scene.checkpoints[i]),
                                                 ParkourController::start_spot_over_feet(sp00_scene.checkpoints[j]));
            }
        }
        float edge_back = -1.0f;
        {
            const LevelCheckpointInfo* home = nullptr;
            const LevelCheckpointInfo* away = nullptr;
            for (const LevelCheckpointInfo& cp : sp01_scene.checkpoint_infos) {
                if (cp.checkpoint_name == "After_Intro") home = &cp;
                if (cp.checkpoint_name == "Rooftop_Action") away = &cp;
            }
            if (home && away) edge_back = death_returns_to(sp01_scene, move_cfg, home->location, away->location);
        }
        const bool deaths_ok = tutorial_back >= 0.0f && tutorial_back < 2.0f && edge_back >= 0.0f && edge_back < 2.0f;
        s25_pass = tutorial.failed == 0 && edge.failed == 0 && tutorial.checkpoints >= 19 && tutorial.stood == tutorial.checkpoints &&
                   edge.checkpoints == 10 && edge.stood == 9 && deaths_ok;
        std::cout << "  -> Stage 25 Result: " << (s25_pass ? "PASS" : "FAIL") << " (Tutorial_p: " << tutorial.stood << " of " << tutorial.checkpoints
                  << " stages stand her where they put her; Edge_p: " << edge.stood << " of " << edge.checkpoints << " checkpoints stand, "
                  << edge.fell + edge.fell_dead << " falls, " << tutorial.failed + edge.failed << " failed, largest drift "
                  << std::max(tutorial.worst_drift, edge.worst_drift) << " uu; Death=" << (deaths_ok ? "OK" : "FAIL") << " [Tutorial_p: after standing "
                  << tutorial_lower << " lower she is back " << tutorial_back << " uu from her stage's start; Edge_p: back " << edge_back
                  << " uu from After_Intro's])" << std::endl;
    }

    // Stage 26: a paused game and the level emitters (oracle_sound_pause_and_emitters above).
    std::cout << "[Oracle Stage 26] Testing the Paused Game's Sounds and the Level Emitters (layers, sources, rounds)..." << std::endl;
    const bool s26_pass = oracle_sound_pause_and_emitters(game_root);

    // Stage 27: a bot's fire at her, a bot's armour, the helicopter's box, a bullet's stumble
    // (oracle_bot_fire above).
    std::cout << "[Oracle Stage 27] Testing the Bots' Fire at Her, their Armour, the Helicopter's Box and a Bullet's Stumble..." << std::endl;
    const bool s27_pass = oracle_bot_fire(renderer, sim_scene, move_cfg);

    // Stage 29: the level script's Matinees on the actors they move (oracle_matinee_movers above).
    std::cout << "[Oracle Stage 29] Testing the Level Script's Matinees on the Actors They Move..." << std::endl;
    const bool s29_pass = oracle_matinee_movers(game_root, move_cfg);

    // Stages with pass/fail assertions: parkour stages 1-8, cutscene stage 11, door barging stage 12, pipe climb/balance stage 13, SP02 sprint stage 14, zipline/swing/ledge stage 15, camera stage 16, damage screen effects stage 17, pause menu stage 18, and electric fence stage 19
    // (stages 9 and 10 only render screenshots).
    const bool stage_results[] = {s1_pass, s2_pass, s3_pass, s4_pass, s5_pass, s6_pass, s7_pass, s8_pass, s11_pass, s12_pass, s13_pass, s14_pass, s15_pass, s16_pass, s17_pass, s18_pass, s19_pass, s20_pass, s21_pass, s22_pass, s23_pass, s24_pass, s25_pass, s26_pass, s27_pass, s29_pass};
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
    stream_level_to_checkpoint(game_root, scene, 0);
    ensure_dir(out_dir);

    MovementConfig move_cfg;
    load_movement_config_from_ini(get_config_path(game_root, "DefaultPawnMovement.ini"), move_cfg);
    ParkourController controller(move_cfg);
    controller.reset(scene.player_spawn_pos, scene.player_spawn_yaw);
    controller.set_enemy_body_poser([&renderer](const EnemyBot& bot, float sim_time, bool reaction_disarm, EnemyBodySet& out) {
        return renderer.pose_enemy_bodies(bot, sim_time, reaction_disarm, out);
    });

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
    // ME_AUDIO_DEBUG=1: the shots' impact sounds and the level's emitters go through the audio
    // engine (with no device), heard from the view, and it prints the gain each of them gets.
    std::unique_ptr<AudioEngine> audio;
    if (std::getenv("ME_AUDIO_DEBUG")) {
        audio = std::make_unique<AudioEngine>();
        audio->init(true);
        audio->load_stock_audio(game_root);
        audio->load_level_audio(game_root, map_rel);
        for (const PhysicalMaterialInfo& pm : scene.physical_materials) {
            if (!pm.impact_sound.empty() && !audio->has_cue(pm.impact_sound)) audio->load_cue_bank(game_root, pm.impact_sound_package);
        }
    }
    // ME_SHOT_BOT="archetype[,state[,distance[,weapon[,turn[,seconds[,sequence]]]]]]": the level's own
    // enemies are put away and one bot stands on the floor `distance` units ahead of the view (220
    // unless given), in the state named (idle, walk, run, aim, windup, strike, stagger, disarm,
    // dead), with that weapon, turned by `turn` degrees from facing the view, `seconds` into a state
    // that is timed, playing the sequence named where the state takes one from the controller (a
    // blow's HitMelee*, the disarm's Snatch*). The shots of ME_SHOT_FIRE then meet his bodies as the
    // controller's do (ME_SHOW_BODIES=1 draws them).
    bool shot_bot = false;
    float bot_distance = 220.0f, bot_turn = 0.0f;
    if (const char* bot_spec = std::getenv("ME_SHOT_BOT")) {
        std::vector<std::string> field;
        std::stringstream fields(bot_spec);
        for (std::string item; std::getline(fields, item, ',');) field.push_back(item);
        const auto given = [&](size_t i) { return i < field.size() && !field[i].empty(); };
        EnemyBot bot;
        bot.archetype = given(0) ? field[0] : std::string("Assault_SWAT");
        static const std::pair<const char*, EEnemyAnimState> kStates[] = {
            {"idle", EEnemyAnimState::Idle},          {"walk", EEnemyAnimState::Patrol},         {"run", EEnemyAnimState::Chase},
            {"aim", EEnemyAnimState::AimFire},        {"windup", EEnemyAnimState::MeleeWindup},  {"strike", EEnemyAnimState::MeleeStrike},
            {"stagger", EEnemyAnimState::HitStagger}, {"disarm", EEnemyAnimState::BeingDisarmed}, {"dead", EEnemyAnimState::KnockedOut}};
        for (const auto& [name, state] : kStates) {
            if (given(1) && field[1] == name) bot.anim_state = state;
        }
        bot.alive = bot.anim_state != EEnemyAnimState::KnockedOut;
        if (given(2)) bot_distance = static_cast<float>(std::atof(field[2].c_str()));
        bot.weapon_name = given(3) ? field[3] : std::string("Colt1911");
        if (given(4)) bot_turn = static_cast<float>(std::atof(field[4].c_str()));
        if (given(5)) bot.anim_timer = static_cast<float>(std::atof(field[5].c_str()));
        if (given(6)) bot.active_anim_seq = field[6];
        // What the template gives him beside the weapon: the riot cop's shield.
        bot.has_shield = template_carries_shield(find_aim_template(bot.archetype));
        scene.enemies.clear();
        scene.enemies.push_back(bot);
        shot_bot = true;
        std::cout << "[Shots] bot " << bot.archetype << ", state " << static_cast<int>(bot.anim_state) << ", " << bot_distance
                  << " ahead, weapon " << bot.weapon_name << ", turned " << bot_turn << std::endl;
    }
    // ME_SHOT_HELI="distance[,height[,turn]]": the level's own helicopters are put away and one hangs
    // `distance` units ahead of the view (1500 unless given), its Location `height` units over the
    // eye (-434, which puts its box level with the eye, unless given), turned by `turn` degrees from
    // broadside. The shots of ME_SHOT_FIRE then meet its box as the controller's do, and
    // ME_SHOW_BODIES=1 draws the box over the hull.
    bool shot_heli = false;
    float heli_distance = 1500.0f, heli_height = -434.0f, heli_turn = 0.0f;
    if (const char* heli_spec = std::getenv("ME_SHOT_HELI")) {
        std::vector<std::string> field;
        std::stringstream fields(heli_spec);
        for (std::string item; std::getline(fields, item, ',');) field.push_back(item);
        if (field.size() > 0 && !field[0].empty()) heli_distance = static_cast<float>(std::atof(field[0].c_str()));
        if (field.size() > 1 && !field[1].empty()) heli_height = static_cast<float>(std::atof(field[1].c_str()));
        if (field.size() > 2 && !field[2].empty()) heli_turn = static_cast<float>(std::atof(field[2].c_str()));
        HelicopterInstance heli;
        heli.state = EHeliState::Arriving;
        scene.helicopters.clear();
        scene.helicopters.push_back(heli);
        shot_heli = true;
        std::cout << "[Shots] a helicopter " << heli_distance << " ahead, its Location " << heli_height << " over the eye, turned " << heli_turn << std::endl;
    }
    // The level's script and the actors its Matinees move, for pictures of them (game/script_movers.hpp):
    //   ME_SHOT_SCRIPT=<checkpoint>   the level begins play at that checkpoint ("1": its default one) with every
    //                                 sublevel's sequence running, so the Matinees its level-loaded events start play;
    //   ME_SHOT_EVENT="Name[@seconds],.."                       remote events fired at those times of the run;
    //   ME_SHOT_MATINEE="package:SeqAct_Interp_N[:reverse][@seconds],.."  ops given their Play (or Reverse) input then.
    // With any of them the script runs each frame and the movers follow; with ME_SHOT_MOVERS they are listed too.
    LevelScript shot_script;
    struct ShotCue {
        float time = 0.0f;
        std::string name;   // a remote event, or "package:op"
        int input = -1;     // -1 a remote event, else the op's input
    };
    std::vector<ShotCue> shot_cues;
    const char* shot_checkpoint = std::getenv("ME_SHOT_SCRIPT");
    {
        const auto parse = [&](const char* spec, bool matinee) {
            if (!spec) return;
            std::stringstream list(spec);
            for (std::string item; std::getline(list, item, ',');) {
                ShotCue cue;
                const size_t at = item.find('@');
                if (at != std::string::npos) cue.time = static_cast<float>(std::atof(item.c_str() + at + 1));
                cue.name = item.substr(0, at);
                if (matinee) {
                    cue.input = 0;
                    const size_t rev = cue.name.rfind(":reverse");
                    if (rev != std::string::npos && rev + 8 == cue.name.size()) {
                        cue.input = 1;
                        cue.name = cue.name.substr(0, rev);
                    }
                }
                if (!cue.name.empty()) shot_cues.push_back(cue);
            }
        };
        parse(std::getenv("ME_SHOT_EVENT"), false);
        parse(std::getenv("ME_SHOT_MATINEE"), true);
    }
    const bool shot_scripted = (shot_checkpoint || !shot_cues.empty()) && scene.script && scene.script->valid();
    if (shot_scripted) {
        ScriptHost host;
        bind_scene_effects(host, scene);
        if (std::getenv("ME_SCRIPT_DEBUG")) host.log = [](const std::string& line) { std::cout << "[Shots] " << line << std::endl; };
        shot_script.init(scene.script, host);
        if (shot_checkpoint) shot_script.begin_play(std::string(shot_checkpoint) == "1" ? std::string() : std::string(shot_checkpoint));
        std::cout << "[Shots] the level's script runs: " << scene.movers.size() << " actors its Matinees move, " << shot_cues.size() << " cue(s)" << std::endl;
    }
    if (movers) {
        for (size_t k = 0; k < scene.movers.size(); ++k) {
            const ScriptMover& m = scene.movers[k];
            const LevelActor& a = scene.actors[static_cast<size_t>(m.actor_index)];
            std::cout << "[Shots] mover " << k << " " << a.source_package << ":" << a.unique_name << " (" << a.mesh_name << ") at (" << a.location.x << ", "
                      << a.location.y << ", " << a.location.z << ")" << (m.collision ? ", collides" : "") << (m.base >= 0 ? ", attached" : "") << std::endl;
        }
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
        if (shot_bot && !scene.enemies.empty()) {
            // The bot, ahead of the view on the floor there.
            Vec3 eye;
            Rotator rot;
            renderer.player_camera(tel, eye, rot);
            const Vec3 ahead = Vec3(rot.forward().x, rot.forward().y, 0.0f).normalized_xy();
            EnemyBot& bot = scene.enemies.front();
            bot.position = Vec3(eye.x + ahead.x * bot_distance, eye.y + ahead.y * bot_distance, standing ? stand_at.z : tel.position.z);
            const ImpactSurface floor = find_impact_surface(scene, Vec3(bot.position.x, bot.position.y, eye.z),
                                                            Vec3(bot.position.x, bot.position.y, eye.z - 600.0f));
            if (floor.hit) bot.position.z = floor.location.z;
            bot.home_position = bot.position;
            bot.yaw_deg = std::atan2(-ahead.y, -ahead.x) * RAD2DEG + bot_turn;
        }
        if (shot_heli && !scene.helicopters.empty()) {
            // The helicopter, ahead of the view and broadside to it.
            Vec3 eye;
            Rotator rot;
            renderer.player_camera(tel, eye, rot);
            const Vec3 ahead = Vec3(rot.forward().x, rot.forward().y, 0.0f).normalized_xy();
            HelicopterInstance& heli = scene.helicopters.front();
            heli.position = Vec3(eye.x + ahead.x * heli_distance, eye.y + ahead.y * heli_distance, eye.z + heli_height);
            heli.yaw_deg = std::atan2(ahead.y, ahead.x) * RAD2DEG + 90.0f + heli_turn;
        }
        if (fire_ammo >= 0 && fire_at >= 0.0f && now + dt >= fire_at && scene.collision) {
            fire_at = -1.0f;
            Vec3 eye;
            Rotator rot;
            renderer.player_camera(tel, eye, rot);
            static const float kSpread[5][2] = {{0.0f, 0.0f}, {-0.09f, 0.05f}, {0.09f, 0.05f}, {-0.05f, -0.07f}, {0.06f, -0.06f}};
            // With a bot (ME_SHOT_BOT): his bodies as the last picture drew them, posed once for the
            // five shots as the controller poses them for a trigger pull.
            std::vector<EnemyBodySet> posed;
            if (shot_bot) {
                controller.get_telemetry().sim_time = tel.sim_time;
                controller.get_telemetry().reaction_active = tel.reaction_active;
                controller.pose_enemies_for_shot(scene, eye, rot.forward(), 8000.0f, 0.12f, posed);
            }
            for (const auto& s : kSpread) {
                const Vec3 dir = (rot.forward() + rot.right() * s[0] + rot.up() * s[1]).normalized();
                const ImpactSurface hit = find_impact_surface(scene, eye, eye + dir * 8000.0f);  // the level, a lift or a door
                ParkourController::EnemyShotHit shot;
                const bool met = shot_bot && controller.trace_enemies(scene, posed, eye, dir, hit.hit ? (hit.location - eye).length() : 8000.0f, shot);
                if (shot_bot) {
                    std::cout << "[Shots] a shot " << s[0] << " across, " << s[1] << " up: ";
                    if (met) {
                        std::cout << (shot.shield  ? "the bot's riot shield (" + shot.bone + ", no physical material)"
                                      : shot.posed ? "the bot's " + shot.bone + " (body " + std::to_string(shot.body) + ")" + (shot.head ? ", PM_Character_Head" : ", PM_Character_Body")
                                                   : std::string("the bot's cylinder") + (shot.head ? ", PM_Character_Head" : ", PM_Character_Body"))
                                  << ", " << shot.distance << " uu off at ("
                                  << shot.location.x << ", " << shot.location.y << ", " << shot.location.z << "), normal (" << shot.normal.x << ", "
                                  << shot.normal.y << ", " << shot.normal.z << ")" << std::endl;
                    } else {
                        std::cout << "past the bot" << std::endl;
                    }
                }
                // With a helicopter (ME_SHOT_HELI): its box, short of the level and of a bot.
                ParkourController::HelicopterShotHit on_heli;
                const bool heli_met = shot_heli && controller.trace_helicopters(scene, eye, dir, met ? shot.distance : hit.hit ? (hit.location - eye).length() : 8000.0f, on_heli);
                if (shot_heli) {
                    std::cout << "[Shots] a shot " << s[0] << " across, " << s[1] << " up: ";
                    if (heli_met) {
                        std::cout << "the helicopter's box, " << on_heli.distance << " uu off at (" << on_heli.location.x << ", " << on_heli.location.y << ", "
                                  << on_heli.location.z << "), normal (" << on_heli.normal.x << ", " << on_heli.normal.y << ", " << on_heli.normal.z << ")" << std::endl;
                    } else {
                        std::cout << "past the helicopter's box" << std::endl;
                    }
                }
                if (!hit.hit && body_at <= 0.0f && !met && !heli_met) continue;
                BulletTracer tr;
                tr.start_pos = eye + dir * 30.0f;
                tr.end_pos = hit.location;
                tr.ammo = static_cast<uint8_t>(fire_ammo);
                if (heli_met) {
                    tr.end_pos = on_heli.location;
                    tr.from_player = true;
                    tr.pawn_hit = 4;  // the weapon's default impact effect and sound, no hole
                    tr.pawn_normal = on_heli.normal;
                } else if (met && shot.shield) {
                    // His riot shield: the weapon's default impact effect and sound, no hole; nothing
                    // on one who is dead.
                    tr.end_pos = shot.location;
                    tr.from_player = true;
                    tr.pawn_hit = scene.enemies[static_cast<size_t>(shot.enemy)].alive ? 4 : 3;
                    tr.pawn_normal = shot.normal;
                } else if (met) {
                    tr.end_pos = shot.location;
                    tr.from_player = true;
                    tr.hit_enemy = true;
                    tr.pawn_hit = scene.enemies[static_cast<size_t>(shot.enemy)].alive ? 1 : 3;  // a dead bot shows nothing
                    tr.pawn_normal = shot.normal;
                    tr.pawn_surface = shot.head ? ECharacterSurface::Head : ECharacterSurface::Body;
                } else if (body_at > 0.0f) {
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
            if (audio) {
                for (const SimSoundEvent& ev : heard) audio->play_sound_3d(ev.cue, ev.location);
            }
        }
        if (audio) {
            Vec3 ear;
            Rotator ear_rot;
            renderer.player_camera(tel, ear, ear_rot);
            audio->update(dt, ear, ear_rot.forward(), ear_rot.up(), 0.0f, false);
        }
        update_impact_effects(scene, dt, tel.position);
        if (shot_scripted) {
            for (size_t k = 0; k < shot_cues.size();) {
                const ShotCue& cue = shot_cues[k];
                if (now + dt < cue.time) {
                    ++k;
                    continue;
                }
                if (cue.input < 0) {
                    shot_script.fire_remote_event(cue.name);
                } else {
                    const size_t colon = cue.name.find(':');
                    const std::string package = cue.name.substr(0, colon), op = colon == std::string::npos ? std::string() : cue.name.substr(colon + 1);
                    int node = -1;
                    for (size_t i = 0; i < scene.script->nodes.size() && node < 0; ++i) {
                        const ScriptGraph::Node& n = scene.script->nodes[i];
                        if (n.cls == "SeqAct_Interp" && n.name == op && scene.script->packages[static_cast<size_t>(n.package)] == lower(package)) node = static_cast<int>(i);
                    }
                    if (node >= 0) shot_script.activate_input(node, cue.input);
                    else std::cout << "[Shots] no such Matinee: " << cue.name << std::endl;
                }
                std::cout << "[Shots] at " << now + dt << " s: " << (cue.input < 0 ? "remote event " : cue.input == 1 ? "Reverse on " : "Play on ") << cue.name << std::endl;
                shot_cues.erase(shot_cues.begin() + static_cast<std::ptrdiff_t>(k));
            }
            ScriptPlayerState ps;
            ps.position = standing ? stand_at : tel.position;
            ps.eye = ps.position + Vec3(0.0f, 0.0f, tel.eye_height);
            deliver_actor_damage(shot_script, scene);
            shot_script.update(dt, ps);
            update_script_movers(scene, dt);
        }
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
                              bool start_in_main_menu, const std::string& trace_path,
                              const std::string& exit_screenshot, bool mute_requested = false) {
    using namespace me;
    std::cout << "\n============================================================" << std::endl;
    std::cout << "  MIRROR'S EDGE NATIVE " << platform_upper() << " - INTERACTIVE LAUNCH" << std::endl;
    std::cout << "============================================================" << std::endl;

    const char* env_mute = std::getenv("ME_MUTE");
    const bool mute_audio = mute_requested || (env_mute && env_mute[0] != '\0' && env_mute[0] != '0') ||
                            (max_frames > 0) || !exit_screenshot.empty();
    if (mute_audio) {
        SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    }

    SDL_SetMainReady();
#if defined(_WIN32)
    // Window sizes in points and the drawable in pixels, as on a Retina display.
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_SCALING, "1");
#endif
#if defined(__ANDROID__)
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    // A tap is a finger to the touch controls, and a synthesized mouse click to the menus; the
    // other way round (mouse -> touch) is not wanted.
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        std::cerr << "[SDL ERROR] Initialization failed: " << SDL_GetError() << std::endl;
        return 1;
    }

    int win_w = 1280;
    int win_h = 720;
#if defined(ME_RENDERER_VULKAN)
    Renderer::prepare_window_attributes();
#if defined(__ANDROID__)
    const char* window_title = "Mirror's Edge (Native Android Vulkan)";
    const Uint32 window_flags = SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_ALLOW_HIGHDPI;
    {
        SDL_DisplayMode mode{};
        if (SDL_GetCurrentDisplayMode(0, &mode) == 0 && mode.w > 0 && mode.h > 0) {
            win_w = std::max(mode.w, mode.h);
            win_h = std::min(mode.w, mode.h);
        }
    }
#else
    const char* window_title = "Mirror's Edge (Native " ME_PLATFORM_TITLE " Vulkan)";
    const Uint32 window_flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
#endif
#elif defined(__ANDROID__)
    // The whole display, in landscape; the drawable's pixel size comes from SDL_GL_GetDrawableSize.
    const char* window_title = "Mirror's Edge (Native Android OpenGL ES)";
    const Uint32 window_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_ALLOW_HIGHDPI;
    {
        SDL_DisplayMode mode{};
        if (SDL_GetCurrentDisplayMode(0, &mode) == 0 && mode.w > 0 && mode.h > 0) {
            win_w = std::max(mode.w, mode.h);
            win_h = std::min(mode.w, mode.h);
        }
    }
    Renderer::prepare_window_attributes();
#elif defined(ME_RENDERER_OPENGL)
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

    // What the renderer draws into: Vulkan surface, CAMetalLayer on macOS, HWND on Windows,
    // or SDL window GL context on Linux / Android GLES.
#if defined(ME_RENDERER_VULKAN)
    auto get_drawable_size = [&](int* w, int* h) { SDL_Vulkan_GetDrawableSize(window, w, h); };
    auto destroy_surface = []() {};
#elif defined(ME_RENDERER_OPENGL)
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
#if defined(ME_RENDERER_VULKAN) || defined(ME_RENDERER_OPENGL)
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
    audio.init(mute_audio);
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

    // The touch controls (Android, or ME_TOUCH=1): a stick, a look area and buttons over the HUD.
    TouchControls touch;
    touch.set_active(TouchControls::should_activate());
    if (touch.active()) std::cout << "[Touch] On-screen controls active" << std::endl;

    // Load active level & Cutscene Player
    LevelScene active_scene;
    int current_chapter_idx = std::clamp(initial_chapter, 0, 9);
    renderer.set_selected_chapter(current_chapter_idx);

    ParkourController controller(move_cfg);
    // The first-person mesh's foot placement asks the level where the floor is under each foot.
    renderer.set_world_trace([&controller, &active_scene](const Vec3& from, const Vec3& to, Vec3& hit, Vec3& normal) {
        return controller.leg_line_check(from, to, active_scene, hit, normal);
    });
    // The player's bullets meet an enemy in the bodies of his physics asset, posed as he is drawn.
    controller.set_enemy_body_poser([&renderer](const EnemyBot& bot, float sim_time, bool reaction_disarm, EnemyBodySet& out) {
        return renderer.pose_enemy_bodies(bot, sim_time, reaction_disarm, out);
    });

    CutscenePlayer cutscene_player;
    cutscene_player.init(game_root, /*headless=*/mute_audio);
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
        if (handover_set) controller.hand_over_at_spot(handover_pos, handover_yaw, active_scene);
        else controller.hand_over(seq.end_feet_pos, seq.end_yaw_deg, active_scene);
        handover_set = false;
    };
    // Load last checkpoint (TdPlayerController.CanLoadFromLastCheckpoint): the level starts over at
    // the active checkpoint, its script with it; the script's own actors are put back before she
    // is set down among them.
    auto load_last_checkpoint = [&]() {
        if (script.load_from_checkpoint_disabled()) return;
        cutscene_player.stop();
        controller.get_telemetry().intro_active = false;
        controller.get_telemetry().tutorial_text.clear();
        controller.get_telemetry().sign_text.clear();
        restore_script_actors(active_scene);
        if (const ScriptActor* cp = script.active_checkpoint()) controller.restart_level_at(cp->location, cp->yaw_deg, active_scene);
        script.reload_checkpoint();
    };
    // In a level without a script: the level begun again at one of its listed checkpoints.
    auto restart_at_listed_checkpoint = [&](int cp, float yaw) {
        const Vec3 spot = active_scene.checkpoints.empty()     ? ParkourController::start_spot_over_feet(active_scene.player_spawn_pos)
                          : active_scene.checkpoints_are_feet ? ParkourController::start_spot_over_feet(active_scene.checkpoints[static_cast<size_t>(cp)])
                                                              : active_scene.checkpoints[static_cast<size_t>(cp)];
        controller.restart_level_at(spot, yaw, active_scene);
        controller.set_checkpoint(spot, yaw, cp, std::string());
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
            // (SeqAct_TdCheckpoint with a teleport: TdCheckpoint.HandlePawnTeleport.)
            if (teleport) controller.restart_at(cp.location, cp.yaw_deg, active_scene);
            controller.set_checkpoint(cp.location, cp.yaw_deg, index, cp.checkpoint_name);
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
                handover_pos = location;
                handover_yaw = yaw_deg;
                handover_set = true;
                return;
            }
            // Actor.OnTeleport: SetLocation(the destination's Location), the capsule's centre.
            controller.restart_at(location, yaw_deg, active_scene);
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
                if (cp->location.length_xy() > 1.0f) controller.restart_level_at(cp->location, cp->yaw_deg, active_scene);
                controller.set_checkpoint(cp->location, cp->yaw_deg, index, cp->checkpoint_name);
            }
            ScriptPlayerState ps;
            ps.position = controller.get_telemetry().position;
            ps.eye = ps.position + Vec3(0.0f, 0.0f, controller.get_telemetry().eye_height);
            script.update(0.0f, ps);
            update_script_movers(active_scene, 0.0f, &controller);
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
                controller.restart_level_at(cp.location, cp.rotation.to_degrees().y, active_scene);
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

    // The port's own overlay menu stands for two of retail's things. Opened from play (Tab, or
    // Escape when no front end is loaded) it is a paused game: the level's sounds are paused under
    // it and its music turned down (AudioEngine::set_game_paused). Opened as the front end, where the
    // game starts in a menu or The Shard ends and no front end is loaded, it is not a pause: it has
    // the menu's theme and nothing of the level, as retail's front end, a map of its own, has.
    bool overlay_is_front_end = false;
    auto set_menu_active = [&](bool open, bool as_front_end = false) {
        renderer.set_menu_open(open);
        overlay_is_front_end = open && as_front_end;
        audio.set_menu_music(overlay_is_front_end);
        audio.set_game_paused(open && !as_front_end);
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

    set_menu_active(start_in_main_menu, /*as_front_end=*/true);
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
    // The difficulty the game is played at (TdPlayerController.GetDifficultyLevel): the profile's
    // GameDifficulty, which the NEW GAME and the options screens write (0 Easy, 1 Medium, 2 Hard);
    // Medium with no profile, and always Medium in a race (TdSPLevelRace: a speed run, a time
    // trial). Given to the controller whenever the front end reports an action, so a change on the
    // options screen holds at once for every bot there is (TdPlayerController.SetNewDifficulty).
    bool race_game = false;
    auto apply_difficulty = [&]() {
        controller.set_difficulty(race_game || !frontend ? me::kDifficultyMedium : frontend->settings().get("GameDifficulty", me::kDifficultyMedium));
    };
    apply_difficulty();

    // Interactive Loop
    bool running = true;
    bool fade_intro_before = false;
    float fade_last_sim_time = 1.0e30f;
    std::string fade_map;
    int frame_counter = 0;
    auto last_time = std::chrono::high_resolution_clock::now();

    const auto trace_start = std::chrono::steady_clock::now();
    // --exit-screenshot <path>: the frame the run ends on (--max-frames), as a PNG, for a look at
    // a run on a machine without a screen (the Android emulator, a CI box).
    auto save_exit_screenshot = [&]() {
        if (exit_screenshot.empty()) return;
        const fs::path parent = fs::path(exit_screenshot).parent_path();
        if (!parent.empty()) ensure_dir(parent.string());
        const bool ok = renderer.save_screenshot_png(exit_screenshot);
        std::cout << "[Game] exit screenshot " << (ok ? "written: " : "FAILED: ") << exit_screenshot << std::endl;
    };
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

    // Whether the game is paused as retail's is while WorldInfo.Pauser is set: under the pause menu
    // (TdUIScene_Pause.PostInitialize calls SetPause(true)), under the port's overlay menu when it
    // was opened from play, and under a hint card (TdUIScene_SplashHint, the scene that says "game
    // paused"). The front end is not a paused game: TdHUD.PauseGame returns at once in the main menu.
    auto game_is_paused = [&]() {
        return (frontend && !frontend_active && frontend->is_pause_open()) || (renderer.is_menu_open() && !overlay_is_front_end) ||
               splash_hint_open;
    };

    // The audio engine's frame. The listener is the pawn's location plus her eye height, facing as
    // the view does, in play and behind a menu alike: the update starts the sounds the frame asked
    // for at a place and sets the ones already playing by their distance from it. It is told first
    // whether the game is paused, which pauses every sound of the level but its music.
    auto update_audio = [&](float dt, float player_speed, bool reaction_active) {
        audio.set_game_paused(game_is_paused());
        const PlayerTelemetry& t = controller.get_telemetry();
        const Rotator ear_rot = Rotator::from_degrees(t.pitch_deg, t.yaw_deg, t.camera_roll_deg);
        audio.update(dt, t.position + Vec3(0.0f, 0.0f, t.eye_height), ear_rot.forward(), ear_rot.up(), player_speed, reaction_active);
    };

    EMovement prev_state = EMovement::MOVE_Walking;
    int prev_checkpoint = 0;
    int intro_handover_frames = 0;  // frames since a cutscene handed the player over
    bool prev_use_held = false;     // the Use key last frame, for its press edge (SeqEvent_TdUsed)
    bool prev_touch_jump = false;   // the touch JUMP / RT buttons last frame, for their edges
    bool prev_touch_reaction = false;
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

    auto frame_tick = [&]() {
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
                } else if (fev.type == SDL_APP_WILLENTERBACKGROUND || fev.type == SDL_APP_DIDENTERFOREGROUND) {
                    // Android: the app leaves / returns to the screen; no finger is still down.
                    touch.reset();
                } else if (fev.type == SDL_FINGERDOWN || fev.type == SDL_FINGERUP || fev.type == SDL_FINGERMOTION) {
                    // The menus take the synthesized mouse events; the fingers are tracked so none
                    // is left "down" when play resumes.
                    touch.handle_event(fev, drawable_w, drawable_h);
                } else if (fev.type == SDL_WINDOWEVENT) {
                    if (fev.window.event == SDL_WINDOWEVENT_RESIZED || fev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                        get_drawable_size(&drawable_w, &drawable_h);
                        renderer.resize(drawable_w, drawable_h);
                    }
                } else if ((fev.type == SDL_KEYDOWN && fev.key.repeat == 0) || fev.type == SDL_KEYUP) {
                    // Android's Back button is Escape to the menus.
                    if (fev.key.keysym.sym == SDLK_AC_BACK) fev.key.keysym.sym = SDLK_ESCAPE;
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
            if (!action.empty()) {
                // A level begun any other way than as a race is the story's again.
                if (action.rfind("SpeedRun ", 0) == 0 || action.rfind("TimeTrial ", 0) == 0) {
                    race_game = true;
                } else if (action == "NewGame" || action == "SkipTutorial" || action == "QuitToMainMenu" || action.rfind("StartLevel ", 0) == 0) {
                    race_game = false;
                }
                apply_difficulty();
            }
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
            // (The level may have begun play above, from the menu, with its script asking for sounds at a
            // place: they start here, heard from where she stands. A menu has no running speed.)
            update_audio(dt, 0.0f, false);
            (void)touch.consume(dt);  // the menu took the taps as clicks; no edge is kept for play
            touch.publish_overlay(drawable_w, drawable_h, /*visible=*/false);
            renderer.render_frame(active_scene, controller.get_telemetry());
            write_trace(controller.get_telemetry(), /*in_frontend=*/frontend_active);

            ++frame_counter;
            if (max_frames > 0 && frame_counter >= max_frames) {
                std::cout << "[Game] Reached max-frames limit (" << max_frames << "). Exiting cleanly." << std::endl;
                save_exit_screenshot();
                running = false;
            }
            return;
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
                    if (script.valid()) {
                        load_last_checkpoint();
                    } else {
                        controller.get_telemetry().intro_active = false;
                        cutscene_player.stop();
                        int cp = std::clamp(controller.get_telemetry().active_checkpoint, 0,
                                            std::max(0, static_cast<int>(active_scene.checkpoints.size()) - 1));
                        restart_at_listed_checkpoint(cp, active_scene.player_spawn_yaw);
                    }
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
            } else if (ev.type == SDL_APP_WILLENTERBACKGROUND || ev.type == SDL_APP_DIDENTERFOREGROUND) {
                // Android: the app leaves / returns to the screen. No finger is still down; the
                // audio device is SDL's / OpenAL's to suspend.
                touch.reset();
            } else if (ev.type == SDL_FINGERDOWN || ev.type == SDL_FINGERUP || ev.type == SDL_FINGERMOTION) {
                touch.handle_event(ev, drawable_w, drawable_h);
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
                } else if (ev.motion.which == SDL_TOUCH_MOUSEID) {
                    // A finger's synthesized mouse motion: the touch controls' look, not the mouse's.
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
                } else if (ev.button.which == SDL_TOUCH_MOUSEID) {
                    // A tap (synthesized from a finger): the touch controls' business in play; only
                    // the menus above take it as a click.
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
                if (key == SDLK_AC_BACK) key = SDLK_ESCAPE;  // Android's Back button
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
                    load_last_checkpoint();
                } else if (key == SDLK_r) {
                    controller.get_telemetry().intro_active = false;
                    cutscene_player.stop();
                    int cp = std::clamp(controller.get_telemetry().active_checkpoint, 0,
                                        std::max(0, static_cast<int>(active_scene.checkpoints.size()) - 1));
                    float yaw = active_scene.player_spawn_yaw;
                    if (cp + 1 < static_cast<int>(active_scene.checkpoints.size())) {
                        Vec3 d = active_scene.checkpoints[cp + 1] - active_scene.checkpoints[cp];
                        yaw = std::atan2(d.y, d.x) * RAD2DEG;
                    }
                    restart_at_listed_checkpoint(cp, yaw);
                    if (cp < static_cast<int>(active_scene.subtitles.size()) && !active_scene.subtitles[cp].empty()) {
                        controller.get_telemetry().active_subtitle = active_scene.subtitles[cp];
                    }
                } else if (key == SDLK_RIGHTBRACKET || key == SDLK_n || key == SDLK_LEFTBRACKET || key == SDLK_b) {
                    cutscene_player.stop();
                    if (!active_scene.checkpoints.empty()) {
                        int delta_cp = (key == SDLK_RIGHTBRACKET || key == SDLK_n) ? 1 : -1;
                        int next_cp = std::clamp(controller.get_telemetry().active_checkpoint + delta_cp,
                                                 0, static_cast<int>(active_scene.checkpoints.size()) - 1);
                        float yaw = active_scene.player_spawn_yaw;
                        if (next_cp + 1 < static_cast<int>(active_scene.checkpoints.size())) {
                            Vec3 d = active_scene.checkpoints[next_cp + 1] - active_scene.checkpoints[next_cp];
                            yaw = std::atan2(d.y, d.x) * RAD2DEG;
                        }
                        restart_at_listed_checkpoint(next_cp, yaw);
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
            SDL_GetRelativeMouseMode() == SDL_TRUE && !touch.active() &&  // (a finger is a left button to SDL)
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

        // Touch controls (src/platform/touch_controls.hpp): the stick overrides the keys while a
        // finger is on it, the look drag adds to the mouse's, the buttons are held like keys. MENU
        // and a JUMP tap while a cutscene or hint card is up go through the keyboard paths above
        // as Escape / Space presses, so the menus and skips behave as with a keyboard.
        if (touch.active()) {
            const TouchInput ti = touch.consume(dt);
            auto push_key = [](SDL_Keycode sym, SDL_Scancode scancode) {
                SDL_Event e{};
                e.type = SDL_KEYDOWN;
                e.key.state = SDL_PRESSED;
                e.key.keysym.sym = sym;
                e.key.keysym.scancode = scancode;
                SDL_PushEvent(&e);
                e.type = SDL_KEYUP;
                e.key.state = SDL_RELEASED;
                SDL_PushEvent(&e);
            };
            if (ti.stick_active) {
                input.forward = ti.forward;
                input.strafe = ti.strafe;
            }
            if (!renderer.is_menu_open() && !cutscene_player.is_playing()) {
                const float sens = static_cast<float>(kSensPresets[sens_preset_idx]) / 100.0f;
                input.look_yaw_delta += ti.look_yaw_delta_deg * sens;
                input.look_pitch_delta += ti.look_pitch_delta_deg * sens;
            }
            const bool jump_edge = ti.jump && !prev_touch_jump;
            prev_touch_jump = ti.jump;
            if (jump_edge && (cutscene_player.is_playing() || splash_hint_open || renderer.is_menu_open())) {
                push_key(SDLK_SPACE, SDL_SCANCODE_SPACE);
            } else if (ti.jump && !suppress_space_until_release) {
                input.jump = true;
            }
            if (ti.crouch) input.crouch = true;
            if (ti.turn_180) input.turn_180 = true;
            if (ti.attack) {
                if (controller.get_weapon().equipped) input.fire = true;
                else input.melee = true;
            }
            if (ti.action) {
                input.disarm = true;
                input.use = true;
            }
            if (ti.reaction && !prev_touch_reaction) {
                reaction_toggled = !reaction_toggled;
                input.reaction_time = reaction_toggled;
            }
            prev_touch_reaction = ti.reaction;
            if (ti.menu_pressed_edge) push_key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
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
        const bool sim_running = !renderer.is_menu_open() && !splash_hint_open;
        if (sim_running) {
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
                        controller.relocate(to, into_cutscene_yaw);
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
                    controller.restart_at_checkpoint(active_scene);  // onto the level as it is now
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
                // The actors its Matinees move go where their Positions now have them, she with the
                // one she stands on, and out of the way of one that comes at her.
                update_script_movers(active_scene, dt, &controller);
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
                            set_menu_active(true, /*as_front_end=*/true);
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

        // (Only for a step that ran: the flag stays up for as long as the simulation is not stepping.)
        if (sim_running && tel.weapon.fired_this_tick && !cutscene_player.is_playing()) {
            audio.play_effect(EAudioEffect::Gunshot);
        }

        // Footstep cadence (TdPhysicalMaterialFootSteps: Sneak / Walk / Run / Sprint)
        // (not during a cutscene: a level intro's footsteps are its animation's own notifies). Under the
        // barge the locomotion only steps while the move's animation slot is mostly blended out.
        const bool locomotion_feet = tel.move_state == EMovement::MOVE_Walking ||
                                     (tel.move_state == EMovement::MOVE_Barge && tel.barge_anim_weight < 0.5f);
        if (sim_running && !cutscene_player.is_playing() && tel.grounded && locomotion_feet &&
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
        audio.set_menu_music(renderer.is_menu_open() && overlay_is_front_end);
        // A cutscene's motion is not the player's running: no speed-driven wind or breathing for it.
        update_audio(dt, (cutscene_player.is_playing() || into_cutscene_pending) ? 0.0f : tel.speed_2d,
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
        touch.publish_overlay(drawable_w, drawable_h, /*visible=*/!renderer.is_menu_open());
        renderer.render_frame(active_scene, tel);
        write_trace(tel, /*in_frontend=*/false);

        ++frame_counter;
        if (max_frames > 0 && frame_counter >= max_frames) {
            std::cout << "[Game] Reached max-frames limit (" << max_frames << "). Exiting cleanly." << std::endl;
            save_exit_screenshot();
            running = false;
        }
    };

    while (running) {
        frame_tick();
#if defined(__EMSCRIPTEN__)
        if (running) {
            emscripten_sleep(16);
        }
#endif
    }

#if !defined(__EMSCRIPTEN__)
    if (game_controller) SDL_GameControllerClose(game_controller);
    audio.shutdown();
    destroy_surface();
    SDL_DestroyWindow(window);
    SDL_Quit();
#endif

    std::cout << "[Game] Shutdown cleanly." << std::endl;
    return 0;
}

// -----------------------------------------------------------------------------
// Main Entrypoint & CLI Parsing
// -----------------------------------------------------------------------------
int main(int argc, char* argv[]) {
#if defined(__ANDROID__)
    // Everything printed goes to logcat (tag "mirrorsedge"), and the working directory is the
    // app's external files directory: `screenshots/` and the oracle's files land there, where
    // `adb pull` reaches them (android/README.md).
    me::install_android_log_redirect();
    if (const char* external = SDL_AndroidGetExternalStoragePath()) {
        std::error_code ec;
        fs::create_directories(external, ec);
        if (chdir(external) != 0) std::cerr << "[Android] chdir(" << external << ") failed" << std::endl;
        else std::cout << "[Android] working directory: " << external << std::endl;
    } else if (const char* internal = SDL_AndroidGetInternalStoragePath()) {
        if (chdir(internal) == 0) std::cout << "[Android] working directory: " << internal << std::endl;
    }
    std::cout << "[Android] args:";
    for (int i = 1; i < argc; ++i) std::cout << " " << argv[i];
    std::cout << std::endl;
#endif
    std::string game_root = me::default_game_root();
    bool verify_all = false;
    bool verify_script = false;  // the level script's stage of the oracle, alone
    bool verify_sound = false;   // and the two sound stages
    bool verify_bodies = false;  // the enemies' bodies stage of the oracle, alone
    bool verify_botfire = false; // the bots' fire, armour, helicopter and stumble stage of the oracle, alone
    bool verify_respawn = false; // a restart at every checkpoint of every chapter
    bool verify_movers = false;  // the Matinee movers' stage of the oracle, and a count over every chapter
    std::string shots_map, shots_times, shots_dir;
    std::string handover_map;  // --handover-check <map>
    std::string cue_dump;      // --dump-sound-cues <file>
    bool emitter_walk = false; // --emitter-walk
    bool audio_device_check = false;  // --audio-device-check
    std::string script_json = "";
    int initial_chapter = 0;
    std::string custom_level = "";
    int max_frames = 0;
    bool start_in_main_menu = true;
    bool mute_requested = false;
    std::string trace_path;
    std::string exit_screenshot;  // --exit-screenshot <png>: the last frame when --max-frames ends the run

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
        } else if (arg == "--dump-sound-cues") {
            if (i + 1 < argc) cue_dump = argv[++i];
        } else if (arg == "--emitter-walk") {
            emitter_walk = true;
        } else if (arg == "--audio-device-check") {
            audio_device_check = true;
        } else if (arg == "--verify-script") {
            verify_script = true;
        } else if (arg == "--verify-sound") {
            verify_sound = true;
        } else if (arg == "--verify-bodies") {
            verify_bodies = true;
        } else if (arg == "--verify-botfire") {
            verify_botfire = true;
        } else if (arg == "--verify-respawn") {
            verify_respawn = true;
        } else if (arg == "--verify-movers") {
            verify_movers = true;
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
        } else if (arg == "--mute") {
            mute_requested = true;
        } else if (arg == "--trace") {
            if (i + 1 < argc) trace_path = argv[++i];
        } else if (arg == "--exit-screenshot") {
            if (i + 1 < argc) exit_screenshot = argv[++i];
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
                      << "  --verify-sound           Run only its sound stages (gain and low pass with distance, pause, emitters)\n"
                      << "  --verify-bodies          Run only its stage of bullets against the enemies' posed bodies\n"
                      << "  --verify-botfire         Run only its stage of the bots' fire at her, their armour, the helicopter, a bullet's stumble\n"
                      << "  --verify-respawn         Restart at every checkpoint of every chapter and see where she ends up\n"
                      << "  --verify-movers          Run only its stage of the script's Matinees moving actors, then count them by chapter\n"
                      << "  --handover-check <map>   Print the camera around the end of the level's intro, headless\n"
                      << "  --intro-shots <map> <t,t,..> <dir>  Render the level's intro at those Matinee times, headless\n"
                      << "  --dump-shaders <dir>     Write the Metal shader sources, to check them with a compiler\n"
                      << "  --dump-sound-cues <file> Write every sound cue's attenuation nodes as the loader reads them, headless\n"
                      << "  --emitter-walk           Count the emitter voices heard above every level emitter of the eleven maps, headless\n"
                      << "  --audio-device-check     Open the audio device with no window and no sound and see what a pause does to its sources\n"
                      << "  --headless-oracle <file> Run script-based headless oracle\n"
                      << "  --test-replay <trace>    Replay physics trace headless\n"
                      << "  --chapter <0..9>         Start at specified campaign chapter\n"
                      << "  --level <path>           Load custom level package\n"
                      << "  --max-frames <N>         Exit after rendering N frames\n"
                      << "  --mute                   Start with all audio muted (SDL dummy audio driver)\n"
                      << "  --trace <file>           Write the camera and every sound played, one JSON line per frame\n"
                      << "  --exit-screenshot <png>  With --max-frames: save the last frame there before exiting\n"
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
    if (!cue_dump.empty()) {
        return run_dump_sound_cues(game_root, cue_dump);
    }
    if (emitter_walk) return run_emitter_walk(game_root);
    if (audio_device_check) return run_audio_device_check(game_root);
    if (verify_sound) {
        std::cout << "[Oracle Stage 23] Testing Sound Attenuation with Distance (the cues' own curves, per wave)..." << std::endl;
        const bool attenuation_ok = oracle_sound_attenuation(game_root);
        std::cout << "[Oracle Stage 26] Testing the Paused Game's Sounds and the Level Emitters (layers, sources, rounds)..." << std::endl;
        const bool emitters_ok = oracle_sound_pause_and_emitters(game_root);
        return attenuation_ok && emitters_ok ? 0 : 1;
    }
    if (!handover_map.empty()) {
        me::Renderer renderer;
        renderer.set_game_root(game_root);
        if (!renderer.init_headless(1280, 720)) return 1;
        return measure_intro_handover(renderer, game_root, handover_map, true).valid ? 0 : 1;
    }
    if (verify_respawn) return run_verify_respawn(game_root);
    if (verify_movers) return run_verify_movers(game_root);
    if (verify_bodies) {
        me::Renderer renderer;
        renderer.set_game_root(game_root);
        if (!renderer.init_headless(1280, 720)) return 1;
        me::MovementConfig move_cfg;
        me::load_movement_config_from_ini(me::get_config_path(game_root, "DefaultPawnMovement.ini"), move_cfg);
        me::LevelScene scene;
        if (!me::load_level_scene(game_root, "Maps/SP00/Tutorial_p.me1", scene)) return 1;
        std::cout << "[Oracle Stage 24] Testing the Player's Bullets against an Enemy's Posed Bodies (Male3p_Physics)..." << std::endl;
        return oracle_enemy_bodies(renderer, scene, move_cfg) ? 0 : 1;
    }
    if (verify_botfire) {
        me::Renderer renderer;
        renderer.set_game_root(game_root);
        if (!renderer.init_headless(1280, 720)) return 1;
        me::MovementConfig move_cfg;
        me::load_movement_config_from_ini(me::get_config_path(game_root, "DefaultPawnMovement.ini"), move_cfg);
        me::LevelScene scene;
        if (!me::load_level_scene(game_root, "Maps/SP00/Tutorial_p.me1", scene)) return 1;
        std::cout << "[Oracle Stage 27] Testing the Bots' Fire at Her, their Armour, the Helicopter's Box and a Bullet's Stumble..." << std::endl;
        return oracle_bot_fire(renderer, scene, move_cfg) ? 0 : 1;
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

    return run_interactive_app(game_root, initial_chapter, custom_level, max_frames, start_in_main_menu, trace_path,
                               exit_screenshot, mute_requested);
}
