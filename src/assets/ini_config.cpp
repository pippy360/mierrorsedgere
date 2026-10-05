#include "ini_config.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>

namespace me {

namespace {

// Trim from both ends
std::string trim(const std::string& str) {
    auto start = str.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = str.find_last_not_of(" \t\r\n");
    return str.substr(start, end - start + 1);
}

// Convert string to lower case
std::string to_lower(std::string str) {
    std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return str;
}

// Strip quotes if present
std::string unquote(const std::string& str) {
    std::string s = trim(str);
    if (s.size() >= 2) {
        if ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\'')) {
            return s.substr(1, s.size() - 2);
        }
    }
    return s;
}

// Decode raw binary buffer into UTF-8 string with BOM handling
std::string decode_to_utf8(const std::vector<uint8_t>& raw) {
    if (raw.empty()) return "";

    // UTF-16 LE BOM: 0xFF, 0xFE
    if (raw.size() >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
        std::string utf8;
        size_t num_words = (raw.size() - 2) / 2;
        utf8.reserve(num_words);
        for (size_t i = 0; i < num_words; ++i) {
            uint16_t w = static_cast<uint16_t>(raw[2 + i * 2]) | (static_cast<uint16_t>(raw[2 + i * 2 + 1]) << 8);
            if (w < 0x80) {
                utf8.push_back(static_cast<char>(w));
            } else if (w < 0x800) {
                utf8.push_back(static_cast<char>(0xC0 | (w >> 6)));
                utf8.push_back(static_cast<char>(0x80 | (w & 0x3F)));
            } else {
                utf8.push_back(static_cast<char>(0xE0 | (w >> 12)));
                utf8.push_back(static_cast<char>(0x80 | ((w >> 6) & 0x3F)));
                utf8.push_back(static_cast<char>(0x80 | (w & 0x3F)));
            }
        }
        return utf8;
    }

    // UTF-16 BE BOM: 0xFE, 0xFF
    if (raw.size() >= 2 && raw[0] == 0xFE && raw[1] == 0xFF) {
        std::string utf8;
        size_t num_words = (raw.size() - 2) / 2;
        utf8.reserve(num_words);
        for (size_t i = 0; i < num_words; ++i) {
            uint16_t w = (static_cast<uint16_t>(raw[2 + i * 2]) << 8) | static_cast<uint16_t>(raw[2 + i * 2 + 1]);
            if (w < 0x80) {
                utf8.push_back(static_cast<char>(w));
            } else if (w < 0x800) {
                utf8.push_back(static_cast<char>(0xC0 | (w >> 6)));
                utf8.push_back(static_cast<char>(0x80 | (w & 0x3F)));
            } else {
                utf8.push_back(static_cast<char>(0xE0 | (w >> 12)));
                utf8.push_back(static_cast<char>(0x80 | ((w >> 6) & 0x3F)));
                utf8.push_back(static_cast<char>(0x80 | (w & 0x3F)));
            }
        }
        return utf8;
    }

    // UTF-8 BOM: 0xEF, 0xBB, 0xBF
    size_t start = 0;
    if (raw.size() >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) {
        start = 3;
    }

    return std::string(reinterpret_cast<const char*>(raw.data() + start), raw.size() - start);
}

} // namespace

const std::unordered_map<std::string, std::vector<std::string>> IniConfig::empty_section_;

bool IniConfig::load_file(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(size);
    if (size > 0) {
        file.read(reinterpret_cast<char*>(buffer.data()), size);
    }
    file.close();

    std::string text = decode_to_utf8(buffer);
    return load_string(text);
}

bool IniConfig::load_string(const std::string& content) {
    std::istringstream stream(content);
    std::string line;
    std::string current_section;

    while (std::getline(stream, line)) {
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed.front() == ';' || trimmed.front() == '#' || trimmed.rfind("//", 0) == 0) {
            continue;
        }

        // Section header [SectionName]
        if (trimmed.front() == '[' && trimmed.back() == ']') {
            current_section = trim(trimmed.substr(1, trimmed.size() - 2));
            continue;
        }

        if (current_section.empty()) {
            continue;
        }

        // Strip inline comments outside of quotes
        bool in_quote = false;
        char quote_char = 0;
        size_t comment_pos = std::string::npos;
        for (size_t i = 0; i < trimmed.size(); ++i) {
            char c = trimmed[i];
            if (in_quote) {
                if (c == quote_char) in_quote = false;
            } else {
                if (c == '"' || c == '\'') {
                    in_quote = true;
                    quote_char = c;
                } else if (c == ';' || c == '#') {
                    comment_pos = i;
                    break;
                }
            }
        }
        if (comment_pos != std::string::npos) {
            trimmed = trim(trimmed.substr(0, comment_pos));
            if (trimmed.empty()) continue;
        }

        // Parse Key=Value
        size_t eq_pos = trimmed.find('=');
        if (eq_pos == std::string::npos) {
            continue;
        }

        char prefix = 0;
        size_t key_start = 0;
        if (trimmed.front() == '+' || trimmed.front() == '-' || trimmed.front() == '.') {
            prefix = trimmed.front();
            key_start = 1;
        }

        std::string key = trim(trimmed.substr(key_start, eq_pos - key_start));
        std::string val = trim(trimmed.substr(eq_pos + 1));
        if (!val.empty() && val.back() == ';') {
            val.pop_back();
            val = trim(val);
        }

        std::string clean_val = unquote(val);

        if (prefix == '-') {
            auto& key_vals = sections_[current_section][key];
            auto it = std::find(key_vals.begin(), key_vals.end(), clean_val);
            if (it != key_vals.end()) {
                key_vals.erase(it);
            }
        } else if (prefix == '+' || prefix == '.') {
            sections_[current_section][key].push_back(clean_val);
        } else {
            // Standard assignment (if already present, append to preserve array directives)
            sections_[current_section][key].push_back(clean_val);
        }
    }

    return true;
}

bool IniConfig::has_section(const std::string& section) const {
    return sections_.find(section) != sections_.end();
}

std::string IniConfig::get_string(const std::string& section, const std::string& key, const std::string& default_val) const {
    auto sec_it = sections_.find(section);
    if (sec_it != sections_.end()) {
        auto key_it = sec_it->second.find(key);
        if (key_it != sec_it->second.end() && !key_it->second.empty()) {
            return key_it->second.back();
        }
    }
    return default_val;
}

float IniConfig::get_float(const std::string& section, const std::string& key, float default_val) const {
    std::string s = get_string(section, key, "");
    if (s.empty()) return default_val;

    // Strip trailing 'f' or 'F'
    if (!s.empty() && (s.back() == 'f' || s.back() == 'F')) {
        s.pop_back();
    }
    try {
        return std::stof(s);
    } catch (...) {
        return default_val;
    }
}

int IniConfig::get_int(const std::string& section, const std::string& key, int default_val) const {
    std::string s = get_string(section, key, "");
    if (s.empty()) return default_val;
    try {
        return std::stoi(s, nullptr, 0);
    } catch (...) {
        return default_val;
    }
}

bool IniConfig::get_bool(const std::string& section, const std::string& key, bool default_val) const {
    std::string s = to_lower(get_string(section, key, ""));
    if (s == "true" || s == "1" || s == "yes" || s == "on") return true;
    if (s == "false" || s == "0" || s == "no" || s == "off") return false;
    return default_val;
}

std::vector<std::string> IniConfig::get_array(const std::string& section, const std::string& key) const {
    auto sec_it = sections_.find(section);
    if (sec_it != sections_.end()) {
        auto key_it = sec_it->second.find(key);
        if (key_it != sec_it->second.end()) {
            return key_it->second;
        }
    }
    return {};
}

std::vector<std::string> IniConfig::get_section_names() const {
    std::vector<std::string> names;
    names.reserve(sections_.size());
    for (const auto& [sec, _] : sections_) {
        names.push_back(sec);
    }
    return names;
}

const std::unordered_map<std::string, std::vector<std::string>>& IniConfig::get_section_keys(const std::string& section) const {
    auto it = sections_.find(section);
    if (it != sections_.end()) {
        return it->second;
    }
    return empty_section_;
}

// -----------------------------------------------------------------------------
// High-Level Domain Parsers
// -----------------------------------------------------------------------------

bool load_movement_config_from_ini(const std::string& ini_path, MovementConfig& out_config) {
    IniConfig ini;
    if (!ini.load_file(ini_path)) {
        return false;
    }

    out_config.gravity = 980.0f;
    out_config.walk_speed = 50.0f;
    out_config.jog_speed = 260.0f;
    out_config.run_speed = 400.0f;
    out_config.sprint_speed = 630.0f;

    // [TdGame.TdMove_Jump]
    out_config.base_jump_z = ini.get_float("TdGame.TdMove_Jump", "BaseJumpZ", 630.0f);
    out_config.base_jump_z_heavy = ini.get_float("TdGame.TdMove_Jump", "BaseJumpZHeavy", 430.0f);
    out_config.jump_add_xy = ini.get_float("TdGame.TdMove_Jump", "JumpAddXY", 100.0f);

    // [TdGame.TdMove_WallRun]
    out_config.wallrun_min_speed = ini.get_float("TdGame.TdMove_WallRun", "WallRunningMinSpeed", 200.0f);
    out_config.wallrun_initial_z = ini.get_float("TdGame.TdMove_WallRun", "WallRunningHorisontalInitialZHeight", 170.0f);
    out_config.wallrun_accel = ini.get_float("TdGame.TdMove_WallRun", "WallRunningHorisontalAcceleration", 820.0f);
    out_config.wallrun_decel = ini.get_float("TdGame.TdMove_WallRun", "WallRunningHorisontalDeceleration", 500.0f);
    out_config.wallrun_max_angle_deg = ini.get_float("TdGame.TdMove_WallRun", "WallRunningForwardMaxStartAngle", 57.0f);
    out_config.wallrun_duration = 1.6f;

    // [TdGame.TdMove_WallClimb]
    out_config.wallclimb_max_angle_deg = ini.get_float("TdGame.TdMove_WallClimb", "WallClimbingVerticalStartAngle", 33.0f);
    out_config.wallclimb_gravity = ini.get_float("TdGame.TdMove_WallClimb", "WallClimbingGravity", 800.0f);
    out_config.wallclimb_boost_z = ini.get_float("TdGame.TdMove_WallClimb", "AddOnSpeedZMaxLimit", 320.0f);

    // [TdGame.TdMove_SpringBoard]
    out_config.springboard_jump_z = ini.get_float("TdGame.TdMove_SpringBoard", "SpringBoardJumpZ", 950.0f);

    // [TdGame.TdMove_Slide]
    out_config.slide_min_speed = ini.get_float("TdGame.TdMove_Slide", "SlideAbortSpeed", 250.0f);
    out_config.slide_friction = ini.get_float("TdGame.TdMove_Slide", "FrictionModifier", 0.1f);
    out_config.slide_max_duration = ini.get_float("TdGame.TdMove_Slide", "SlideAbortTime", 2.0f);

    // [TdGame.TdMove_Coil]
    out_config.coil_height_boost = ini.get_float("TdGame.TdMove_Coil", "TotalHeightBoost", 60.0f);
    out_config.coil_duration = ini.get_float("TdGame.TdMove_Coil", "CoilTime", 0.5f);

    // [TdGame.TdMove_Landing]
    out_config.skill_roll_min_fall = ini.get_float("TdGame.TdMove_Landing", "SkillRollLandingHeight", 200.0f);
    out_config.hard_landing_min_fall = ini.get_float("TdGame.TdMove_Landing", "HardLandingHeight", 530.0f);
    out_config.uncontrolled_fall = 1000.0f;

    // [TdGame.TdMove_180Turn]
    out_config.turn_180_time = ini.get_float("TdGame.TdMove_180Turn", "TurnTime", 0.25f);

    out_config.reaction_time_dilation = 0.25f;
    out_config.reaction_time_drain = 8.0f;
    out_config.health_regen_delay = 5.0f;
    out_config.health_regen_rate = 25.0f;

    return true;
}

bool load_campaign_chapters(const std::string& game_root, std::vector<ChapterInfo>& out_chapters) {
    out_chapters.clear();

    // Default canonical campaign mapping
    struct ChapterDef {
        const char* code;
        const char* title;
        const char* primary_map;
    };

    static const ChapterDef kChapters[] = {
        {"Entry", "Bootstrap & Splash", "Maps/Entry.upk"},
        {"Menu",  "City Skyline (Main Menu)", "Maps/Menu/TdMainMenu.me1"},
        {"SP00",  "Prologue: Training Area", "Maps/SP00/Tutorial_p.me1"},
        {"SP01a", "Prologue: The Edge",      "Maps/SP01/Edge_p.me1"},
        {"SP01b", "Chapter 1: Flight",       "Maps/SP01/Escape_p.me1"},
        {"SP02",  "Chapter 2: Jacknife",     "Maps/SP02/Stormdrain_p.me1"},
        {"SP03",  "Chapter 3: Heat",         "Maps/SP03/Cranes_p.me1"},
        {"SP04",  "Chapter 4: Ropeburn",     "Maps/SP04/Subway_p.me1"},
        {"SP05",  "Chapter 5: New Eden",     "Maps/SP05/Mall_p.me1"},
        {"SP06",  "Chapter 6: Pirandello Kruger", "Maps/SP06/Factory_p.me1"},
        {"SP07",  "Chapter 7: The Boat",     "Maps/SP07/Boat_p.me1"},
        {"SP08",  "Chapter 8: Kate",         "Maps/SP08/Convoy_p.me1"},
        {"SP09",  "Chapter 9: The Shard",    "Maps/SP09/Scraper_p.me1"}
    };

    for (const auto& ch : kChapters) {
        out_chapters.push_back(ChapterInfo{
            .code = ch.code,
            .title = ch.title,
            .primary_map = ch.primary_map
        });
    }

    // Try refining titles from DefaultGame.ini / TdGameUI.int if present
    std::string game_ini = get_config_path(game_root, "DefaultGame.ini");
    IniConfig ini;
    if (ini.load_file(game_ini)) {
        // Can read dynamic chapter overrides
    }

    return true;
}

bool load_subtitles_from_int(const std::string& int_path, std::vector<std::pair<std::string, std::string>>& out_subtitles) {
    out_subtitles.clear();
    IniConfig int_file;
    if (!int_file.load_file(int_path)) {
        return false;
    }

    const auto& keys = int_file.get_section_keys("Subtitles");
    for (const auto& [k, vals] : keys) {
        for (const auto& v : vals) {
            if (!v.empty() && v != " ") {
                out_subtitles.emplace_back(k, v);
            }
        }
    }
    return !out_subtitles.empty();
}

std::string get_config_path(const std::string& game_root, const std::string& ini_filename) {
    namespace fs = std::filesystem;
    fs::path base(game_root);
    fs::path candidate = base / "TdGame" / "Config" / ini_filename;
    if (fs::exists(candidate)) return candidate.string();
    candidate = base / "Config" / ini_filename;
    if (fs::exists(candidate)) return candidate.string();
    return (base / ini_filename).string();
}

std::string get_localization_path(const std::string& game_root, const std::string& loc_filename, const std::string& lang) {
    namespace fs = std::filesystem;
    fs::path base(game_root);
    fs::path candidate = base / "TdGame" / "Localization" / lang / loc_filename;
    if (fs::exists(candidate)) return candidate.string();
    candidate = base / "Localization" / lang / loc_filename;
    if (fs::exists(candidate)) return candidate.string();
    return (base / loc_filename).string();
}

} // namespace me
