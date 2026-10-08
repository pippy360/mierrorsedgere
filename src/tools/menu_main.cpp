// me_menu: runs the front end ("Press Any Key" and the main menu) headless and writes frames as
// PNG, drawn by the CPU reference renderer. Builds on any platform, like me_replay, so the menu
// can be put next to retail on the machine that has retail installed.
//
//   me_menu --game-root <install> --out <dir> --script "wait 5; shot start.png; key any; wait 3; shot story.png"
//
// Script commands, separated by ';' (time advances in 1/60 s steps):
//   wait <seconds>                  run the front end
//   key <name>                      press and release: any, left, right, up, down, enter, escape,
//                                   prevpage, nextpage (the gamepad's shoulders), reset (its X);
//                                   or a key by its engine name, as the game sends it: SpaceBar, W, LeftShift
//   move <x> <y> | click <x> <y>    the mouse, in viewport pixels
//   shot <file.png>                 render the current frame into --out
//   state                           print the screen, column and focused button
//   linear <file.f32>               the 3D scene before tone mapping, raw float32 RGB
//   camera <eye xyz> <target xyz> <fov>   override the camera for the shots that follow
//   bench <frames>                  time the reference renderer
//   menu                            go straight to the main menu

#include "../ui/frontend/frontend.hpp"
#include "../ui/frontend/soft_render.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
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

me::fe::Key parse_key(const std::string& s) {
    using me::fe::Key;
    if (s == "left") return Key::Left;
    if (s == "right") return Key::Right;
    if (s == "up") return Key::Up;
    if (s == "down") return Key::Down;
    if (s == "enter" || s == "Enter" || s == "SpaceBar") return Key::Accept;
    if (s == "escape" || s == "Escape") return Key::Escape;
    if (s == "prevpage") return Key::PrevPage;
    if (s == "nextpage") return Key::NextPage;
    if (s == "reset") return Key::Reset;
    return Key::Other;
}

// The keys the script names the way the engine does are sent with that name, as the game sends them.
std::string engine_key_name(const std::string& s) {
    if (s == "enter") return "Enter";
    if (s == "escape") return "Escape";
    if (s == "left") return "Left";
    if (s == "right") return "Right";
    if (s == "up") return "Up";
    if (s == "down") return "Down";
    return (!s.empty() && s[0] >= 'A' && s[0] <= 'Z') ? s : std::string();
}

std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    const size_t a = s.find_first_not_of(ws);
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(ws) - a + 1);
}

void dump_image(const std::string& dir, const std::string& name, const me::fe::Image& img) {
    if (!img.valid()) return;
    // The alpha channel as its own picture: the glyphs live there.
    std::vector<uint8_t> alpha(img.px.size());
    for (size_t k = 0; k < img.px.size(); k += 4) {
        alpha[k] = alpha[k + 1] = alpha[k + 2] = img.px[k + 3];
        alpha[k + 3] = 255;
    }
    me::fe::write_png(dir + "/" + name + ".png", img.w, img.h, img.px.data());
    me::fe::write_png(dir + "/" + name + "_alpha.png", img.w, img.h, alpha.data());
    std::cout << name << ": " << img.w << "x" << img.h << (img.wrap_x ? " wrapX" : " clampX") << (img.wrap_y ? " wrapY" : " clampY")
              << (img.srgb ? " sRGB" : " linear") << "\n";
}

void dump_assets(const std::string& dir, const me::fe::Assets& a) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    dump_image(dir, "title", a.title);
    dump_image(dir, "button", a.button);
    dump_image(dir, "stick_left", a.stick_left);
    dump_image(dir, "stick_right", a.stick_right);
    dump_image(dir, "stick_shadow", a.stick_shadow);
    dump_image(dir, "stick_timeline", a.stick_timeline);
    dump_image(dir, "city_fade", a.city.fade);
    dump_image(dir, "city_sky", a.city.sky);
    dump_image(dir, "city_waves", a.city.waves);
    for (const me::fe::Font* f : {&a.small_bold, &a.small_normal, &a.medium_italic, &a.headline}) {
        std::cout << f->name << ": line " << f->line_height << " scale " << f->scale << " spacing " << f->spacing << " pairs "
                  << f->pairs.size() << "\n";
        for (size_t p = 0; p < f->pages.size(); ++p) dump_image(dir, f->name + "_page" + std::to_string(p), f->pages[p]);
    }
    std::cout << a.kismet.nodes.size() << " Kismet objects, " << a.kismet.matinees.size() << " Matinees, " << a.kismet.actors.size()
              << " camera and target actors\n";
    for (const me::fe::MatineeData& m : a.kismet.matinees) {
        std::cout << "  " << m.name << ": " << m.length << " s, groups";
        for (const me::fe::MatineeGroup& g : m.groups) std::cout << " " << (g.name.empty() ? "(director)" : g.name);
        std::cout << ", events " << m.events.size() << "\n";
    }
    size_t tris = 0;
    static const char* const kMaterial[] = {"buildings", "base", "water", "waves", "sky"};
    for (const auto& b : a.city.batches) {
        tris += b.tris.size() / 3;
        float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
        for (const auto& v : b.tris) {
            const float p[3] = {v.pos.x, v.pos.y, v.pos.z};
            for (int k = 0; k < 3; ++k) {
                lo[k] = std::min(lo[k], p[k]);
                hi[k] = std::max(hi[k], p[k]);
            }
        }
        std::cout << "  " << b.mesh << " (" << kMaterial[static_cast<int>(b.material)] << "): " << b.tris.size() / 3 << " triangles, x "
                  << lo[0] << ".." << hi[0] << ", y " << lo[1] << ".." << hi[1] << ", z " << lo[2] << ".." << hi[2]
                  << (b.lightmap >= 0 ? ", light map" : "") << "\n";
    }
    std::cout << "city: " << a.city.batches.size() << " batches, " << tris << " triangles\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string game_root = default_game_root();
    std::string out_dir = ".";
    std::string script = "wait 5; shot start.png; key any; wait 4; shot menu.png";
    std::string dump_dir;
    int width = 1280, height = 720;
    bool background = true, ui = true;
    me::fe::Profile profile;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--game-root") {
            game_root = next();
        } else if (a == "--out") {
            out_dir = next();
        } else if (a == "--script") {
            script = next();
        } else if (a == "--script-file") {
            // A long script: the command line has a length limit.
            std::ifstream in(next());
            std::stringstream ss;
            ss << in.rdbuf();
            script = ss.str();
        } else if (a == "--dump-assets") {
            dump_dir = next();
        } else if (a == "--no-background") {
            background = false;
        } else if (a == "--no-ui") {
            ui = false;
        } else if (a == "--no-save") {
            profile.can_continue = false;
            profile.chapters_unlocked = false;
        } else if (a == "--all-levels") {
            profile.all_levels_unlocked = true;
        } else if (a == "--controller") {
            profile.controller = true;
        } else if (a == "--chapters") {
            // --chapters <n>: the first n entries of the PLAY CHAPTER list are unlocked (1 = the Training Area only)
            const int n = std::atoi(next().c_str());
            profile.unlocked_levels = n >= 32 ? 0xFFFFFFFFu : ((1u << (n < 0 ? 0 : n)) - 1u);
        } else if (a == "--courses") {
            // --courses <n>: the first n TIME TRIAL courses and SPEED RUN chapters are unlocked (1 = a profile that has only trained)
            const int n = std::atoi(next().c_str());
            profile.time_trials = profile.level_races = n >= 32 ? 0xFFFFFFFFu : ((1u << (n < 0 ? 0 : n)) - 1u);
        } else if (a == "--completed") {
            // --completed <n>: chapters finished, for UNLOCKABLES (0 = a profile that has only trained)
            profile.levels_completed = std::atoi(next().c_str());
        } else if (a == "--hard") {
            profile.hard_unlocked = true;
        } else if (a == "--player") {
            profile.player_name = next();
        } else if (a == "--size") {
            const std::string s = next();
            const size_t x = s.find('x');
            if (x != std::string::npos) {
                width = std::atoi(s.substr(0, x).c_str());
                height = std::atoi(s.substr(x + 1).c_str());
            }
        } else if (a == "--help" || a == "-h") {
            std::cout << "me_menu --game-root <install> --out <dir> [--size 1280x720] [--no-background] [--no-ui]\n"
                         "        [--no-save] [--all-levels] [--controller] [--chapters <n>] [--hard] [--player <name>]\n"
                         "                                              what the profile and the save file would unlock\n"
                         "        [--dump-assets <dir>] [--script-file <file>] --script \"wait 5; shot start.png; key any; wait 4; shot menu.png\"\n";
            return 0;
        } else {
            std::cerr << "me_menu: unknown option " << a << "\n";
            return 2;
        }
    }

    me::fe::Frontend fe;
    fe.set_profile(profile);
    std::string error;
    if (!fe.init(game_root, width, height, error)) {
        std::cerr << "me_menu: " << error << "\n";
        return 1;
    }
    for (const std::string& w : fe.assets().warnings) std::cerr << "me_menu: warning: " << w << "\n";

    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    if (!dump_dir.empty()) dump_assets(dump_dir, fe.assets());

    me::fe::SoftRenderer renderer(fe.assets());
    renderer.set_background(background);
    renderer.set_ui(ui);

    const float dt = 1.0f / 60.0f;
    std::vector<uint8_t> rgba;
    bool camera_override = false;
    float cam[7] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 90.0f};
    std::stringstream commands(script);
    std::string command;
    while (std::getline(commands, command, ';')) {
        std::stringstream cs(trim(command));
        std::string verb;
        cs >> verb;
        if (verb.empty()) continue;
        if (verb == "wait") {
            float seconds = 0.0f;
            cs >> seconds;
            for (int n = static_cast<int>(seconds / dt + 0.5f); n > 0; --n) fe.update(dt);
        } else if (verb == "key") {
            std::string name;
            cs >> name;
            const me::fe::Key k = parse_key(name);
            const std::string engine_name = engine_key_name(name);
            fe.key_down(k, engine_name);
            fe.update(dt);
            fe.key_up(k, engine_name);
            fe.update(dt);
        } else if (verb == "move" || verb == "click") {
            float x = 0.0f, y = 0.0f;
            cs >> x >> y;
            if (verb == "move") fe.mouse_move(x, y);
            else fe.mouse_click(x, y);
            fe.update(dt);
        } else if (verb == "menu") {
            fe.open_main_menu();
            fe.update(dt);
        } else if (verb == "state") {
            std::cout << (fe.screen() == me::fe::Screen::Start ? "start" : "menu") << " column " << fe.panel() << " focus "
                      << fe.focused_button() << (fe.animating() ? " (animating)" : "");
            if (!fe.scene_name().empty()) std::cout << " scene " << fe.scene_name() << " focus " << fe.scene_focus();
            std::cout << "\n";
        } else if (verb == "set") {
            // set <setting> <value>: a profile setting, before its screen is opened ("set Brightness 10")
            std::string name;
            int value = 0;
            cs >> name >> value;
            if (me::fe::ProfileSetting* s = fe.settings().find(name)) s->value = value;
            else std::cout << "set: no profile setting " << name << "\n";
        } else if (verb == "list") {
            // list <tag> <a,b,c> <index>: a PC string list, as the host would report it ("list Antialiasing OFF,2X,4X 1")
            std::string tag, values;
            int index = 0;
            cs >> tag >> values >> index;
            me::fe::StringList& list = fe.string_list(tag);
            list.values.clear();
            std::stringstream vs(values);
            std::string v;
            while (std::getline(vs, v, ',')) list.values.push_back(v);
            list.index = index;
        } else if (verb == "texture") {
            // texture <object path> <name>: a Texture2D of the retail packages as <name>.png and its alpha as <name>_a.png
            std::string path, name;
            cs >> path >> name;
            const me::fe::Image* img = fe.image(path);
            if (!img || !img->valid()) {
                std::cout << "texture " << path << ": not found\n";
            } else {
                std::vector<uint8_t> alpha(img->px.size());
                for (size_t i = 0; i < img->px.size(); i += 4) {
                    alpha[i] = alpha[i + 1] = alpha[i + 2] = img->px[i + 3];
                    alpha[i + 3] = 255;
                }
                me::fe::write_png(out_dir + "/" + name + ".png", img->w, img->h, img->px.data());
                me::fe::write_png(out_dir + "/" + name + "_a.png", img->w, img->h, alpha.data());
                std::cout << "texture " << path << ": " << img->w << "x" << img->h << "\n";
            }
        } else if (verb == "bindings") {
            // The key bindings as CONTROLS last saved them (the gamepad's left out).
            for (const me::fe::KeyBinding& b : fe.bindings()) {
                if (b.key.compare(0, 4, "Xbox") != 0) std::cout << "  " << b.key << " = " << b.command << "\n";
            }
        } else if (verb == "kern") {
            // kern <font> <text>: every glyph's width and what follows it ("kern Helvetica_Small_Normal DEFAULTS")
            std::string font_name, text;
            cs >> font_name;
            std::getline(cs, text);
            text = trim(text);
            const me::fe::Font* font = fe.font(font_name);
            if (!font || !font->valid()) {
                std::cout << "kern: no font " << font_name << "\n";
            } else {
                std::cout << font_name << " scale " << font->scale << " spacing " << font->spacing << " line " << font->line_height << " width "
                          << font->width(text) << "\n";
                for (size_t i = 0; i < text.size(); ++i) {
                    const unsigned char c = static_cast<unsigned char>(text[i]);
                    const unsigned char next = i + 1 < text.size() ? static_cast<unsigned char>(text[i + 1]) : 0;
                    const auto pair = font->pairs.find((static_cast<uint32_t>(c) << 16) | next);
                    std::cout << "  '" << text[i] << "' w " << font->glyphs[c].w << " advance " << font->advance(c, next) << " pair "
                              << (pair == font->pairs.end() ? 0.0f : pair->second) << "\n";
                }
            }
        } else if (verb == "rects") {
            // The open scene's widgets with their resolved rectangles (scene pixels) and text.
            if (const me::fe::UiScene* scene = fe.scene()) {
                for (const me::fe::UiWidget& w : scene->widgets) {
                    if (w.cls == "TdUIButtonBarButton") continue;
                    std::printf("%-28s %-18s %7.1f %7.1f %7.1f %7.1f%s %s\n", w.name.c_str(), w.cls.c_str(), w.rect.l, w.rect.t, w.rect.r, w.rect.b,
                                w.hidden ? " hidden" : "", w.text.substr(0, 40).c_str());
                }
            }
        } else if (verb == "event") {
            // event <name>: fire a level event as a UI scene would ("VideoButton_Clicked", "LoadLevel_Edge")
            std::string name;
            cs >> name;
            fe.level_event(name);
            fe.update(dt);
        } else if (verb == "kismet") {
            // The Matinees playing now, in the order they are updated (the last one moves the camera last).
            std::cout << "kismet: " << fe.kismet().playing() << "\n";
        } else if (verb == "bench") {
            // bench <frames>: run and render that many frames, print the average time of one
            int frames = 60;
            cs >> frames;
            const auto t0 = std::chrono::steady_clock::now();
            for (int n = 0; n < frames; ++n) {
                fe.update(dt);
                renderer.render(fe.frame(), rgba);
            }
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            std::cout << "bench: " << (ms / std::max(frames, 1)) << " ms a frame at " << width << "x" << height << "\n";
        } else if (verb == "camera") {
            // camera <x y z> <target x y z> <fov>: look from here in the shots that follow ("camera" alone: back to the menu's own)
            camera_override = static_cast<bool>(cs >> cam[0] >> cam[1] >> cam[2] >> cam[3] >> cam[4] >> cam[5] >> cam[6]);
        } else if (verb == "shot") {
            std::string name;
            cs >> name;
            me::fe::Frame frame = fe.frame();
            if (camera_override) {
                frame.camera = me::Vec3{cam[0], cam[1], cam[2]};
                frame.target = me::Vec3{cam[3], cam[4], cam[5]};
                frame.fov = cam[6];
                frame.white = 0.0f;
            }
            renderer.render(frame, rgba);
            const std::string path = out_dir + "/" + name;
            if (!me::fe::write_png(path, frame.width, frame.height, rgba.data())) {
                std::cerr << "me_menu: could not write " << path << "\n";
                return 1;
            }
            std::cout << "wrote " << path << "\n";
        } else if (verb == "linear") {
            // The 3D scene before tone mapping, as raw float32 RGB: for fitting the tone curve.
            std::string name;
            cs >> name;
            renderer.render(fe.frame(), rgba);
            const std::vector<float>& lin = renderer.linear_scene();
            if (std::FILE* f = std::fopen((out_dir + "/" + name).c_str(), "wb")) {
                std::fwrite(lin.data(), sizeof(float), lin.size(), f);
                std::fclose(f);
            }
        } else {
            std::cerr << "me_menu: unknown script command '" << verb << "'\n";
            return 2;
        }
        for (const std::string& s : fe.take_sounds()) std::cout << "sound " << s << "\n";
        const std::string action = fe.take_action();
        if (!action.empty()) std::cout << "action " << action << "\n";
    }
    return 0;
}
