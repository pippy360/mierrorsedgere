// me_replay: replay a recorded retail Mirror's Edge run through the ParkourController, headless.
//
// The retail side is tools/retail/: a d3d9 proxy in the shipping game records the pawn's own
// Location, Velocity and move every frame plus every key the game received
// (tools/retail/record_session.py). tools/retail/replay.py turns that recording into a script of
// windows, each anchored at a recorded retail frame and then driven open loop for its length by
// the keys and the view retail had, one step per recorded retail frame with that frame's own dt.
// This program runs the script against the real level collision and writes where the controller
// went, frame by frame, for replay.py to score against the recording.
//
// It needs no window, GPU or audio, so it builds wherever the asset loader and the controller do
// (macOS, and Windows - where the retail game and the recorder run).
//
//   me_replay --game-root <dir> --level Maps/SP01/Escape_p.me1 --script run.txt --out run_port.txt
//
// Script (one record per line; '#' comments):
//   seg <id> <fx> <fy> <fz> <vx> <vy> <vz> <yaw> <pitch> <grounded> <jump> <energy> <acctime>
//       <stop_left> <prejump> <held>
//       a window's anchor: the feet (uu), velocity (uu/s), view (deg), PHYS_Walking, the jump chain
//       and the frame state of ParkourController::AnchorState, and the keys held into it
//   f <dt> <keys> <yaw> <pitch>
//       one retail frame: its length (s), the keys it took and the view it turned to (deg)
// Keys are bits: 1 W, 2 S, 4 A, 8 D, 16 jump, 32 crouch, 64 Q (a press), 128 left button (a press).
//
// Output:
//   S <id> <fx> <fy> <fz> <grounded>               the anchor as placed
//   T <id> <frame> <t> <x> <y> <z> <vx> <vy> <vz> <yaw> <pitch> <move> <grounded> <health>
//       after each frame: the feet, velocity, view, EMovement number, grounded and health

#include "assets/ini_config.hpp"
#include "assets/upk_loader.hpp"
#include "physics/parkour_controller.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace me;

namespace {

enum KeyBit : unsigned {
    KEY_W = 1, KEY_S = 2, KEY_A = 4, KEY_D = 8, KEY_JUMP = 16, KEY_CROUCH = 32, KEY_Q = 64, KEY_LMB = 128,
};

struct Frame {
    float dt = 0.0f;
    unsigned keys = 0;
    float yaw = 0.0f;
    float pitch = 0.0f;
};

struct Segment {
    int id = 0;
    Vec3 feet;
    Vec3 velocity;
    float yaw = 0.0f;
    float pitch = 0.0f;
    bool grounded = true;
    ParkourController::AnchorState state;
    std::vector<Frame> frames;
};

float wrap180(float a) {
    a = std::fmod(a + 180.0f, 360.0f);
    if (a < 0.0f) a += 360.0f;
    return a - 180.0f;
}

bool read_script(const std::string& path, std::vector<Segment>& out) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string tag;
        ss >> tag;
        if (tag == "seg") {
            Segment s;
            int grounded = 1, held = 0;
            ss >> s.id >> s.feet.x >> s.feet.y >> s.feet.z >> s.velocity.x >> s.velocity.y >> s.velocity.z
               >> s.yaw >> s.pitch >> grounded >> s.state.jump >> s.state.sprint_energy >> s.state.accel_time
               >> s.state.stop_left >> s.state.pre_jump_momentum >> held;
            if (!ss) {
                std::cerr << path << ":" << lineno << ": bad seg line" << std::endl;
                return false;
            }
            s.grounded = grounded != 0;
            s.state.held_jump = (held & KEY_JUMP) != 0;
            s.state.held_crouch = (held & KEY_CROUCH) != 0;
            out.push_back(std::move(s));
        } else if (tag == "f") {
            Frame f;
            ss >> f.dt >> f.keys >> f.yaw >> f.pitch;
            if (!ss || out.empty()) {
                std::cerr << path << ":" << lineno << ": bad frame line" << std::endl;
                return false;
            }
            out.back().frames.push_back(f);
        } else {
            std::cerr << path << ":" << lineno << ": unknown record '" << tag << "'" << std::endl;
            return false;
        }
    }
    return true;
}

InputFrame input_for(const Frame& f, const ParkourController& pc) {
    InputFrame in{};
    in.sprint = true;  // as the interactive loop: sprint is the default, the ground model decides
    in.forward = float((f.keys & KEY_W) ? 1 : 0) - float((f.keys & KEY_S) ? 1 : 0);
    in.strafe = float((f.keys & KEY_D) ? 1 : 0) - float((f.keys & KEY_A) ? 1 : 0);
    in.jump = (f.keys & KEY_JUMP) != 0;
    in.crouch = (f.keys & KEY_CROUCH) != 0;
    in.turn_180 = (f.keys & KEY_Q) != 0;
    in.melee = (f.keys & KEY_LMB) != 0;
    // The view retail turned to this frame, as the mouse delta that gets there: the controller's
    // turn damping reads the delta (PlayerInput.aTurn), so the look must arrive as one.
    in.look_yaw_delta = wrap180(f.yaw - pc.get_yaw());
    in.look_pitch_delta = f.pitch - pc.get_pitch();
    // Retail's view already went through its per-move camera locks and constraints.
    in.view_recorded = true;
    return in;
}

int usage() {
    std::cerr << "usage: me_replay --game-root <dir> --level <Maps/...me1> --script <file> --out <file>"
              << std::endl;
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::string game_root, level, script, out_path;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](std::string& dst) {
            if (i + 1 < argc) dst = argv[++i];
        };
        if (a == "--game-root") next(game_root);
        else if (a == "--level") next(level);
        else if (a == "--script") next(script);
        else if (a == "--out") next(out_path);
        else return usage();
    }
    if (game_root.empty() || level.empty() || script.empty() || out_path.empty()) return usage();

    std::vector<Segment> segs;
    if (!read_script(script, segs)) {
        std::cerr << "[Replay ERROR] cannot read script " << script << std::endl;
        return 1;
    }

    MovementConfig cfg;
    if (!load_movement_config_from_ini(get_config_path(game_root, "DefaultPawnMovement.ini"), cfg)) {
        std::cerr << "[Replay ERROR] DefaultPawnMovement.ini not found under " << game_root << std::endl;
        return 1;
    }
    LevelScene scene;
    if (!load_level_scene(game_root, level, scene)) {
        std::cerr << "[Replay ERROR] failed to load " << level << " from " << game_root << std::endl;
        return 1;
    }

    std::FILE* out = std::fopen(out_path.c_str(), "w");
    if (!out) {
        std::cerr << "[Replay ERROR] cannot write " << out_path << std::endl;
        return 1;
    }
    ParkourController pc(cfg);
    size_t total = 0;
    for (const Segment& s : segs) {
        pc.anchor(s.feet, s.velocity, s.yaw, s.pitch, s.grounded, s.state);
        std::fprintf(out, "S %d %.3f %.3f %.3f %d\n", s.id, s.feet.x, s.feet.y, s.feet.z, s.grounded ? 1 : 0);
        float t = 0.0f;
        for (size_t i = 0; i < s.frames.size(); ++i) {
            const Frame& f = s.frames[i];
            pc.step(input_for(f, pc), f.dt, scene);
            t += f.dt;
            const PlayerTelemetry& tel = pc.get_telemetry();
            std::fprintf(out, "T %d %zu %.5f %.3f %.3f %.3f %.2f %.2f %.2f %.3f %.3f %d %d %.1f\n",
                         s.id, i + 1, t, tel.position.x, tel.position.y, tel.position.z,
                         tel.velocity.x, tel.velocity.y, tel.velocity.z, tel.yaw_deg, tel.pitch_deg,
                         int(tel.move_state), tel.grounded ? 1 : 0, tel.health);
        }
        total += s.frames.size();
    }
    std::fclose(out);
    std::cerr << "[Replay] " << segs.size() << " windows, " << total << " frames -> " << out_path << std::endl;
    return 0;
}
