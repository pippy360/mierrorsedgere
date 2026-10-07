// me_menu: runs the front end ("Press Any Key" and the main menu) headless and writes frames as
// PNG, drawn by the CPU reference renderer. Builds on any platform, like me_replay, so the menu
// can be put next to retail on the machine that has retail installed.
//
//   me_menu --game-root <install> --out <dir> --script "wait 5; shot start.png; key any; wait 3; shot story.png"
//
// Script commands, separated by ';' (time advances in 1/60 s steps):
//   wait <seconds>                  run the front end
//   key <name>                      press and release: any, left, right, up, down, enter, escape
//   move <x> <y> | click <x> <y>    the mouse, in viewport pixels
//   shot <file.png>                 render the current frame into --out
//   state                           print the screen, column and focused button
//   menu                            go straight to the main menu

#include "../ui/frontend/frontend.hpp"
#include "../ui/frontend/soft_render.hpp"

#include <cstdlib>
#include <filesystem>
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
    if (s == "enter") return Key::Accept;
    if (s == "escape") return Key::Escape;
    return Key::Other;
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
    for (const me::fe::Matinee* m : {&a.opening, &a.intro[0], &a.loop[0], &a.intro[1], &a.loop[1], &a.intro[2], &a.loop[2],
                                     &a.intro[3], &a.loop[3]}) {
        std::cout << m->name << ": " << m->length << " s, camera keys " << m->camera.keys.size() << ", target keys "
                  << m->target.keys.size() << ", fov keys " << m->fov.keys.size() << ", events " << m->events.size() << "\n";
    }
    size_t tris = 0;
    for (const auto& b : a.city.batches) tris += b.tris.size() / 3;
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

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--game-root") {
            game_root = next();
        } else if (a == "--out") {
            out_dir = next();
        } else if (a == "--script") {
            script = next();
        } else if (a == "--dump-assets") {
            dump_dir = next();
        } else if (a == "--no-background") {
            background = false;
        } else if (a == "--no-ui") {
            ui = false;
        } else if (a == "--size") {
            const std::string s = next();
            const size_t x = s.find('x');
            if (x != std::string::npos) {
                width = std::atoi(s.substr(0, x).c_str());
                height = std::atoi(s.substr(x + 1).c_str());
            }
        } else if (a == "--help" || a == "-h") {
            std::cout << "me_menu --game-root <install> --out <dir> [--size 1280x720] [--no-background] [--no-ui]\n"
                         "        [--dump-assets <dir>] --script \"wait 5; shot start.png; key any; wait 4; shot menu.png\"\n";
            return 0;
        } else {
            std::cerr << "me_menu: unknown option " << a << "\n";
            return 2;
        }
    }

    me::fe::Frontend fe;
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
            fe.key_down(k);
            fe.update(dt);
            fe.key_up(k);
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
                      << fe.focused_button() << (fe.animating() ? " (animating)" : "") << "\n";
        } else if (verb == "shot") {
            std::string name;
            cs >> name;
            const me::fe::Frame& frame = fe.frame();
            renderer.render(frame, rgba);
            const std::string path = out_dir + "/" + name;
            if (!me::fe::write_png(path, frame.width, frame.height, rgba.data())) {
                std::cerr << "me_menu: could not write " << path << "\n";
                return 1;
            }
            std::cout << "wrote " << path << "\n";
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
