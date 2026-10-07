#pragma once

// The few places where the app needs to know which operating system it runs on.

#include <string>

namespace me {

// "macOS" / "Windows" / "Linux", for window titles and log banners.
const char* platform_name();

// The retail Mirror's Edge install used when --game-root is not given:
// $MEDGE_ME_INSTALL if set; on Windows the Steam (or EA / GOG) install if one is found;
// otherwise the development machines' defaults.
std::string default_game_root();

// True if `game_root` looks like a retail install (it has TdGame/CookedPC).
bool is_game_root(const std::string& game_root);

// Where scratch output goes (oracle telemetry, live run traces): /tmp, or %TEMP% on Windows.
std::string temp_dir();

// A per-user directory for derived data that is safe to delete (compiled shaders).
// Created on demand; empty if there is none.
std::string cache_dir();

}  // namespace me
