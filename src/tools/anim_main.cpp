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
//   t  move  px py pz  vx vy vz  pawn_yaw  view_yaw  view_pitch  [name=value ...]
// and a line "reset" where the recording was cut (a pause, a reload). The name=value hints are
// what the moves read off the level, which a recording does not carry: g (the height of the feet
// above the ground), gap (a long jump over a gap), left (a sideways move going left), free (hanging
// free), acc (the player pushing a direction), anim (the animation the move picked, "@reached" for
// getting to the place the move steers for), swing (the swing's angle), lean, pipe, wall and
// wallat (the against-wall state and how far ahead the wall is), ledge (the ledge of a hang or
// vault), heavy, fired, floor (a floor that is not level).

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
    bool armed_sets = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--game-root") game_root = next();
        else if (a == "--frames") frames_path = next();
        else if (a == "--out") out_path = next();
        else if (a == "--eye") eye_path = next();
        else if (a == "--armed") armed_sets = true;
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
    // --armed: the one-handed weapons' common set in front of the unarmed one, as with a pistol in
    // hand (TdPawn.UpdateAnimSets), for the armed stances and the disarm.
    me::AnimSetAsset common;
    if (armed_sets) {
        me::UPKPackage pkg(game_root + "/TdGame/CookedPC/Animations/AS_C1P_OneHanded_Common.upk");
        if (!pkg.is_valid() || !me::AnimSystem::parse_anim_set_package(pkg, common)) {
            std::cerr << "me_anim: cannot read Animations/AS_C1P_OneHanded_Common.upk under " << game_root << "\n";
            return 1;
        }
    }
    me::fp::Director director;
    std::string error;
    auto lookup = [&](const std::string& name, const me::AnimSetAsset** set) {
        if (armed_sets) {
            if (const me::AnimSequenceAsset* seq = common.find_sequence(name)) {
                if (set) *set = &common;
                return seq;
            }
        }
        if (set) *set = &unarmed;
        return unarmed.find_sequence(name);
    };
    if (!director.init(game_root, lookup, error)) {
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
        director.controls().init(anim.faith_upper_mesh(), poser.forward_axis(), poser.right_axis(), poser.up_axis());
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
        // What the moves read off the level, as name=value: g (ground distance), gap, left, free,
        // acc (pushing a direction), anim (the animation the move picked, or @reached), swing
        // (radians), lean, pipe.
        f.ground_distance = -1.0f;
        float wall_ahead = -1.0f;
        for (std::string token; ls >> token;) {
            const size_t eq = token.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = token.substr(0, eq), value = token.substr(eq + 1);
            if (key == "g") f.ground_distance = std::stof(value);
            else if (key == "gap") f.long_jump_over_gap = value != "0";
            else if (key == "left") f.move_left = value != "0";
            else if (key == "free") f.hanging_free = value != "0";
            else if (key == "slope") {
                f.ledge_slope_deg = std::stof(value);
                f.ledge_sloped = f.ledge_slope_deg != 0.0f;
            }
            else if (key == "acc") f.accelerating = value != "0";
            else if (key == "anim") f.move_anim = value;
            else if (key == "swing") f.swing_angle = std::stof(value);
            else if (key == "lean") f.balance_lean = std::stof(value);
            else if (key == "armed") f.armed = value != "0";
            else if (key == "danger") f.balance_danger = std::stoi(value);
            else if (key == "wall") f.against_wall = std::stoi(value);
            else if (key == "wallat") wall_ahead = std::stof(value);  // how far ahead of her centre the wall is
            else if (key == "heavy") f.heavy_weapon = value != "0";
            else if (key == "fired") f.fired = value != "0";
            else if (key == "floor") f.floor_sloped = value != "0";
            else if (key == "ledge") {
                // ledge=x,y,z,nx,ny[,tx,ty,tz]: where the ledge is, the wall's normal, the top's normal.
                float v[8] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
                std::stringstream parts(value);
                int n = 0;
                for (std::string part; n < 8 && std::getline(parts, part, ',');) v[n++] = std::stof(part);
                f.ledge_known = true;
                f.ledge_point = me::Vec3(v[0], v[1], v[2]);
                f.ledge_wall_normal = me::Vec3(v[3], v[4], 0.0f);
                f.ledge_top_normal = me::Vec3(v[5], v[6], v[7]);
            }
            else if (key == "pipe") f.climbing_pipe = value != "0";
            else if (key == "top") f.climb_top = std::stof(value);
        }
        f.movement = static_cast<me::EMovement>(move);
        f.dt = last_t < 0.0 ? 0.0f : static_cast<float>(t - last_t);
        last_t = t;
        // The recordings' pawn location is the capsule's centre, 94 above the mesh's origin.
        me::fp::MeshPlace place;
        {
            const me::Rotator body = me::Rotator::from_degrees(0.0f, f.yaw_deg, 0.0f);
            place.origin = f.position - me::Vec3(0.0f, 0.0f, 94.0f);
            place.forward = body.forward();
            place.right = body.right();
            // TdPawn.CheckAgainstWall's traces: 14 to either side, 68 up, a 2 uu box that stops on the wall.
            if (wall_ahead >= 0.0f) {
                for (int side = 0; side < 2; ++side) {
                    f.wall_hand[side] = f.position + place.forward * (wall_ahead - 2.0f) + place.right * (side == 0 ? -14.0f : 14.0f) +
                                        me::Vec3(0.0f, 0.0f, 68.0f);
                }
            }
        }
        director.tick(f);
        if (eye_file.is_open()) {
            me::fp::PoseEvaluator::Aim aim;
            aim.hips = director.hips_offset();
            aim.swan_forward = director.swan_forward();
            aim.swan_down = director.swan_down();
            aim.look_yaw_deg = f.view_yaw_deg - f.yaw_deg;
            poser.evaluate(director.tree(), pose, aim, &director.controls(), &place);
            poser.component_space(pose, comp_pos, comp_rot);
            me::fp::ViewFrame v = poser.view(comp_pos, comp_rot, f.view_pitch_deg, f.view_yaw_deg - f.yaw_deg, director.swan_forward(),
                                             director.swan_down());
            director.apply_mesh_transform(v);
            char line[160];
            std::snprintf(line, sizeof line, "%.6f %.3f %.3f %.3f %.3f %.3f %.3f", t, v.eye_pawn.x, v.eye_pawn.y, v.eye_pawn.z, v.anim_pitch,
                          v.anim_yaw, v.anim_roll);
            eye_file << line;
            // Where the hands are in that view: to the left, ahead, up.
            if (watch.size() == 1 && watch[0] == "?") {
                // --bones ? lists the skeleton.
                // --bones ? lists the skeleton: each bone, its parent, and where it is in the mesh's
                // space in the reference pose.
                for (const auto& bone : upper_mesh.bones) {
                    const int up = bone.parent_index;
                    std::cout << bone.name << " (" << (up >= 0 && &bone != &upper_mesh.bones[0] ? upper_mesh.bones[static_cast<size_t>(up)].name : std::string("-"))
                              << ") " << bone.comp_ref_pos.x << " " << bone.comp_ref_pos.y << " " << bone.comp_ref_pos.z << std::endl;
                }
                return 0;
            }
            for (const std::string& want : watch) {
                // "w:<bone>" writes the bone's place in the world instead (the mesh's origin 94
                // under the recorded pawn location).
                const bool world = want.compare(0, 2, "w:") == 0;
                const std::string bone_name = world ? want.substr(2) : want;
                me::Vec3 d(0.0f, 0.0f, 0.0f);
                for (size_t b = 0; b < upper_mesh.bones.size(); ++b) {
                    std::string name = upper_mesh.bones[b].name;
                    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (name != bone_name) continue;
                    if (world) {
                        const me::Vec3& c = comp_pos[b];
                        d = place.origin + place.forward * c.dot(poser.forward_axis()) + place.right * c.dot(poser.right_axis()) +
                            me::Vec3(0.0f, 0.0f, c.dot(poser.up_axis()));
                    } else {
                        d = comp_pos[b] - v.eye;
                    }
                }
                if (world) std::snprintf(line, sizeof line, " %.2f %.2f %.2f", d.x, d.y, d.z);
                else std::snprintf(line, sizeof line, " %.2f %.2f %.2f", d.dot(v.left), d.dot(v.forward), d.dot(v.up));
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
