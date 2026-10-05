#pragma once

#include "../math/types.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <memory>
#include <utility>

namespace me {

// Structured INI/INT Parser capable of handling UE3 configuration files:
// - Directives: +Key=Val, -Key=Val, .Key=Val
// - Struct literals: (Field1=Val1, Field2=Val2)
// - UTF-16LE BOM, UTF-16BE BOM, UTF-8 BOM, and Latin-1 / ASCII encodings
class IniConfig {
public:
    IniConfig() = default;

    // Load and parse a file from disk
    bool load_file(const std::string& file_path);

    // Load and parse from in-memory string content
    bool load_string(const std::string& content);

    // Check if section exists
    [[nodiscard]] bool has_section(const std::string& section) const;

    // Query scalar values
    [[nodiscard]] std::string get_string(const std::string& section, const std::string& key, const std::string& default_val = "") const;
    [[nodiscard]] float get_float(const std::string& section, const std::string& key, float default_val = 0.0f) const;
    [[nodiscard]] int get_int(const std::string& section, const std::string& key, int default_val = 0) const;
    [[nodiscard]] bool get_bool(const std::string& section, const std::string& key, bool default_val = false) const;

    // Query array / multi-value directives
    [[nodiscard]] std::vector<std::string> get_array(const std::string& section, const std::string& key) const;

    // Query all sections
    [[nodiscard]] std::vector<std::string> get_section_names() const;

    // Direct access to section dictionary
    [[nodiscard]] const std::unordered_map<std::string, std::vector<std::string>>& get_section_keys(const std::string& section) const;

private:
    // section_name -> (key_name -> list of string values)
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::string>>> sections_;
    static const std::unordered_map<std::string, std::vector<std::string>> empty_section_;
};

// -----------------------------------------------------------------------------
// High-Level Domain Parsers
// -----------------------------------------------------------------------------

// Populate MovementConfig with exact physics numbers from DefaultPawnMovement.ini
bool load_movement_config_from_ini(const std::string& ini_path, MovementConfig& out_config);

// Load campaign chapter progression (SP00 through SP09, Menu, Entry)
bool load_campaign_chapters(const std::string& game_root, std::vector<ChapterInfo>& out_chapters);

// Load localized subtitles from Subtitles.INT
bool load_subtitles_from_int(const std::string& int_path, std::vector<std::pair<std::string, std::string>>& out_subtitles);

// Helper to resolve stock asset paths inside game installation root
std::string get_config_path(const std::string& game_root, const std::string& ini_filename);
std::string get_localization_path(const std::string& game_root, const std::string& loc_filename, const std::string& lang = "INT");

} // namespace me
