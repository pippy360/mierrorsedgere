#include "platform.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace me {

const char* platform_name() {
#if defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#else
    return "Linux";
#endif
}

bool is_game_root(const std::string& game_root) {
    std::error_code ec;
    return !game_root.empty() && fs::is_directory(fs::path(game_root) / "TdGame" / "CookedPC", ec);
}

#ifdef _WIN32
namespace {

std::string registry_string(HKEY root, const char* key, const char* value) {
    char buf[1024];
    DWORD size = sizeof(buf);
    if (RegGetValueA(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
    std::string s(buf);
    for (char& c : s) {
        if (c == '\\') c = '/';
    }
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

// Every Steam library: the Steam install itself plus the "path" entries of its libraryfolders.vdf.
std::vector<std::string> steam_libraries() {
    std::vector<std::string> roots;
    auto add = [&roots](const std::string& p) {
        if (!p.empty() && std::find(roots.begin(), roots.end(), p) == roots.end()) roots.push_back(p);
    };
    add(registry_string(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath"));
    add(registry_string(HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Valve\\Steam", "InstallPath"));
    add("C:/Program Files (x86)/Steam");
    add("C:/Program Files/Steam");
    for (const std::string& root : std::vector<std::string>(roots)) {
        std::ifstream vdf(root + "/steamapps/libraryfolders.vdf");
        std::string line;
        while (std::getline(vdf, line)) {
            const size_t key = line.find("\"path\"");
            if (key == std::string::npos) continue;
            const size_t open = line.find('"', key + 6);
            const size_t close = open == std::string::npos ? open : line.find('"', open + 1);
            if (close == std::string::npos) continue;
            std::string path;
            for (size_t i = open + 1; i < close; ++i) {
                if (line[i] == '\\' && i + 1 < close && line[i + 1] == '\\') ++i;  // "D:\\Games"
                path += (line[i] == '\\') ? '/' : line[i];
            }
            add(path);
        }
    }
    return roots;
}

}  // namespace
#endif

std::string default_game_root() {
    if (const char* env = std::getenv("MEDGE_ME_INSTALL")) {
        if (env[0] != '\0') return env;
    }
#ifdef _WIN32
    std::vector<std::string> candidates;
    for (const std::string& lib : steam_libraries()) candidates.push_back(lib + "/steamapps/common/mirrors edge");
    candidates.push_back(registry_string(HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\EA Games\\Mirror's Edge", "Install Dir"));
    candidates.push_back("C:/Program Files (x86)/Origin Games/Mirror's Edge");
    candidates.push_back("C:/Program Files (x86)/EA Games/Mirror's Edge");
    candidates.push_back("C:/Program Files/EA Games/Mirror's Edge");
    candidates.push_back("C:/GOG Games/Mirror's Edge");
    for (const std::string& c : candidates) {
        if (is_game_root(c)) return c;
    }
    return "C:/Program Files (x86)/Steam/steamapps/common/mirrors edge";
#else
    return "/Users/tomnom/mirrorsedge";
#endif
}

std::string temp_dir() {
#ifdef _WIN32
    std::error_code ec;
    std::string dir = fs::temp_directory_path(ec).generic_string();
    while (!dir.empty() && dir.back() == '/') dir.pop_back();
    return dir.empty() ? "." : dir;
#else
    return "/tmp";
#endif
}

std::string cache_dir() {
    std::string base;
#if defined(_WIN32)
    if (const char* local = std::getenv("LOCALAPPDATA")) base = local;
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME")) base = std::string(home) + "/Library/Caches";
#else
    if (const char* xdg = std::getenv("XDG_CACHE_HOME")) {
        base = xdg;
    } else if (const char* home = std::getenv("HOME")) {
        base = std::string(home) + "/.cache";
    }
#endif
    if (base.empty()) return {};
    const fs::path dir = fs::path(base) / "mierrorsedgere";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return ec ? std::string() : dir.generic_string();
}

}  // namespace me
