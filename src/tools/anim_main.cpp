// me_anim: runs Faith's first-person animation tree over a recorded retail run, headless, and
// prints what it plays each frame in the form the retail recorder logs it (the three heaviest
// sequences: name, time, weight). tools/retail/anim_check.py lays the two side by side.
//
//   me_anim --game-root <install> --frames <frames.txt> --out <leaves.txt> [--eye <eye.txt>]
//
// --eye also poses the skeleton and writes where the camera is each frame: the EyeJoint against
// the pawn (forward, right, up from the mesh's origin) and what the animation turns the view by
// (pitch, yaw, roll in degrees), the two things TdPlayerPawn.CalcCamera takes from the mesh.
//
// A frames file has one retail frame per line:
//   t  move  px py pz  vx vy vz  pawn_yaw  view_yaw  view_pitch  [ground  gap  left  free  accel  anim]
// and a line "reset" where the recording was cut (a pause, a reload). The optional columns are
// what the moves read off the level, which a recording does not carry: the height of the feet
// above the ground (-1 unknown), whether a long jump is over a gap, whether a sideways move goes
// left, whether she hangs free, whether the player is pushing a direction, and the animation the
// move picked ("-" for none, "@reached" for getting to the place the move steers for).

#include "../anim/anim_system.hpp"
#include "../anim/fp_director.hpp"
#include "../anim/fp_pose.hpp"
#include "../assets/upk_loader.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string default_game_root() {
    if (const char* env = std::getenv("MEDGE_ME_INSTALL")) return env;
#ifdef _WIN32
    return "C:/Program Files (x86)/Steam/steamapps/common/mirrors edge";
#else
    return "/Users/tomnom/mirrorsedge";
#endif
}

}  // namespace

int main(int argc, char** argv) {
    std::string game_root = default_game_root();
    std::string frames_path, out_path, eye_path;
    std::vector<std::string> watch{"lefthand", "righthand"};
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--game-root") game_root = next();
        else if (a == "--frames") frames_path = next();
        else if (a == "--out") out_path = next();
        else if (a == "--eye") eye_path = next();
        else if (a == "--bones") {
            // Bones whose place in the view --eye also writes (to the left, ahead, up), comma separated.
            watch.clear();
            std::stringstream list(next());
            for (std::string name; std::getline(list, name, ',');) {
                for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                watch.push_back(name);
            }
        }
        else {
            std::cerr << "me_anim: unknown option " << a << "\n";
            return 2;
        }
    }
    if (frames_path.empty()) {
        std::cerr << "usage: me_anim --frames <frames.txt> [--out <leaves.txt>] [--game-root <install>]\n";
        return 2;
    }

    // Mesh1p's AnimSets with no weapon in hand: AS_C1P_Unarmed.
    me::AnimSetAsset unarmed;
    {
        me::UPKPackage pkg(game_root + "/TdGame/CookedPC/Animations/AS_C1P_Unarmed.upk");
        if (!pkg.is_valid() || !me::AnimSystem::parse_anim_set_package(pkg, unarmed)) {
            std::cerr << "me_anim: cannot read Animations/AS_C1P_Unarmed.upk under " << game_root << "\n";
            return 1;
        }
    }
    me::fp::Director director;
    std::string error;
    if (!director.init(game_root, [&](const std::string& name) { return unarmed.find_sequence(name); }, error)) {
        std::cerr << "me_anim: " << error << "\n";
        return 1;
    }

    std::ifstream in(frames_path);
    if (!in) {
        std::cerr << "me_anim: cannot open " << frames_path << "\n";
        return 1;
    }
    // The skeleton the poses are for: SK_UpperBody, as the game loads it.
    me::AnimSystem anim;
    me::fp::PoseEvaluator poser;
    std::ofstream eye_file;
    if (!eye_path.empty()) {
        if (!anim.init_from_game_root(game_root) || !anim.faith_upper_mesh().is_valid()) {
            std::cerr << "me_anim: cannot load Faith's first-person mesh under " << game_root << "\n";
            return 1;
        }
        poser.init(anim.faith_upper_mesh(), unarmed, director.tree());
        eye_file.open(eye_path);
    }
    me::fp::Pose pose;
    const me::SkeletalMeshAsset& upper_mesh = anim.faith_upper_mesh();
    std::vector<me::Vec3> comp_pos;
    std::vector<me::Quat4> comp_rot;

    std::ofstream out_file;
    if (!out_path.empty()) out_file.open(out_path);
    std::ostream& out = out_path.empty() ? std::cout : out_file;

    std::string line;
    double last_t = -1.0;
    std::vector<me::fp::AnimTree::Leaf> leaves;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (line.compare(0, 5, "reset") == 0) {
            director.reset();
            last_t = -1.0;
            out << "reset\n";
            if (eye_file.is_open()) eye_file << "reset\n";
            continue;
        }
        std::istringstream ls(line);
        double t = 0.0;
        int move = 0;
        me::fp::PawnFrame f;
        ls >> t >> move >> f.position.x >> f.position.y >> f.position.z >> f.velocity.x >> f.velocity.y >> f.velocity.z >> f.yaw_deg >>
            f.view_yaw_deg >> f.view_pitch_deg;
        int gap = 0, left = 0, hang_free = 0, accel = 1;
        std::string anim;
        if (ls >> f.ground_distance >> gap >> left >> hang_free >> accel >> anim) {
            f.long_jump_over_gap = gap != 0;
            f.move_left = left != 0;
            f.hanging_free = hang_free != 0;
            f.accelerating = accel != 0;
            if (anim != "-") f.move_anim = anim;
        } else {
            f.ground_distance = -1.0f;
        }
        f.movement = static_cast<me::EMovement>(move);
        f.dt = last_t < 0.0 ? 0.0f : static_cast<float>(t - last_t);
        last_t = t;
        director.tick(f);
        if (eye_file.is_open()) {
            poser.evaluate(director.tree(), pose);
            poser.component_space(pose, comp_pos, comp_rot);
            const me::fp::ViewFrame v = poser.view(comp_pos, comp_rot, f.view_pitch_deg, f.view_yaw_deg - f.yaw_deg);
            char line[160];
            std::snprintf(line, sizeof line, "%.6f %.3f %.3f %.3f %.3f %.3f %.3f", t, v.eye_pawn.x, v.eye_pawn.y, v.eye_pawn.z, v.anim_pitch,
                          v.anim_yaw, v.anim_roll);
            eye_file << line;
            // Where the hands are in that view: to the left, ahead, up.
            for (const std::string& want : watch) {
                me::Vec3 d(0.0f, 0.0f, 0.0f);
                for (size_t b = 0; b < upper_mesh.bones.size(); ++b) {
                    std::string name = upper_mesh.bones[b].name;
                    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (name == want) d = comp_pos[b] - v.eye;
                }
                std::snprintf(line, sizeof line, " %.2f %.2f %.2f", d.dot(v.left), d.dot(v.forward), d.dot(v.up));
                eye_file << line;
            }
            eye_file << "\n";
        }
        director.tree().leaves(leaves, 3);
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.6f", t);
        out << buf;
        for (const auto& leaf : leaves) {
            std::snprintf(buf, sizeof buf, "\t%s:%.4f:%.4f", leaf.name.c_str(), leaf.time, leaf.weight);
            out << buf;
        }
        out << "\n";
    }
    return 0;
}
