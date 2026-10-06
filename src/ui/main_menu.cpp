#include "main_menu.hpp"
#include "../assets/ini_config.hpp"
#include "../assets/upk_loader.hpp"
#include "../assets/ue3_props.hpp"
#include "../assets/package_manager.hpp"
#include "../assets/texture_loader.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <iostream>

namespace me {

namespace {

std::string to_lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string strip_quotes(std::string s) {
    while (!s.empty() && (s.front() == '"' || s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.erase(s.begin());
    }
    while (!s.empty() && (s.back() == '"' || s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.pop_back();
    }
    return s;
}

std::string extract_kv_field(const std::string& struct_str, const std::string& key) {
    std::string pattern = key + "=";
    size_t pos = struct_str.find(pattern);
    if (pos == std::string::npos) return "";
    pos += pattern.size();
    if (pos < struct_str.size() && struct_str[pos] == '"') {
        pos++;
        size_t end = struct_str.find('"', pos);
        if (end != std::string::npos) {
            return struct_str.substr(pos, end - pos);
        }
    }
    size_t end = struct_str.find_first_of(",)", pos);
    if (end == std::string::npos) end = struct_str.size();
    return strip_quotes(struct_str.substr(pos, end - pos));
}

bool load_ui_texture_by_name(PackageManager& pm, const UPKPackage& pkg, const std::string& tex_name,
                             int max_size, SceneTexture& out_tex) {
    std::string target_low = to_lower_copy(tex_name);
    const auto& exports = pkg.get_exports();
    for (size_t i = 0; i < exports.size(); ++i) {
        if (pkg.get_export_class(exports[i]) != "Texture2D") continue;
        if (to_lower_copy(exports[i].object_name) == target_low) {
            if (load_texture2d(pm, pkg, static_cast<int32_t>(i + 1), max_size, out_tex, nullptr)) {
                // UI textures are rendered in Pass 3 directly to display-space RGBA8Unorm
                out_tex.srgb = false;
                out_tex.address_x = TexAddress::Clamp;
                out_tex.address_y = TexAddress::Clamp;
                return true;
            }
        }
    }
    return false;
}

bool load_ui_multifont(PackageManager& pm, const UPKPackage& pkg, const std::string& font_name,
                       UIMultiFont& out_font) {
    std::string target_low = to_lower_copy(font_name);
    const auto& exports = pkg.get_exports();
    const uint8_t* raw = pkg.get_data().data();
    size_t raw_size = pkg.get_data().size();

    for (size_t i = 0; i < exports.size(); ++i) {
        std::string cls = pkg.get_export_class(exports[i]);
        if (cls != "MultiFont" && cls != "Font") continue;
        if (to_lower_copy(exports[i].object_name) != target_low) continue;

        UPropertyList props;
        parse_export_properties(pkg, static_cast<int32_t>(i + 1), props);

        const UProperty* p_chars = find_prop(props, "Characters");
        const UProperty* p_texs = find_prop(props, "Textures");
        if (!p_chars || !p_texs || p_chars->array_count < 256) return false;

        out_font = UIMultiFont{};
        out_font.name = exports[i].object_name;

        // Pick highest-resolution page tier available in MultiFont (tier 2 = 1080p)
        int num_tiers = std::max(1, p_chars->array_count / 256);
        int tier = num_tiers - 1;
        size_t base_off = p_chars->value_offset + 4 + static_cast<size_t>(tier * 256) * 21;
        if (base_off + 256 * 21 > raw_size) return false;

        float max_glyph_h = 16.0f;
        for (int c = 0; c < 256; ++c) {
            size_t off = base_off + static_cast<size_t>(c) * 21;
            UIFontGlyph g{};
            std::memcpy(&g.u, raw + off + 0, 4);
            std::memcpy(&g.v, raw + off + 4, 4);
            std::memcpy(&g.w, raw + off + 8, 4);
            std::memcpy(&g.h, raw + off + 12, 4);
            std::memcpy(&g.page, raw + off + 16, 1);
            std::memcpy(&g.v_offset, raw + off + 17, 4);
            out_font.glyphs[static_cast<size_t>(c)] = g;
            if (c >= 'A' && c <= 'Z') {
                max_glyph_h = std::max(max_glyph_h, static_cast<float>(g.h + g.v_offset));
            }
        }
        out_font.base_line_height = max_glyph_h;

        out_font.pages.resize(p_texs->ints.size());
        for (size_t ti = 0; ti < p_texs->ints.size(); ++ti) {
            int32_t ref = p_texs->ints[ti];
            if (ref > 0 && load_texture2d(pm, pkg, ref, 1024, out_font.pages[ti], nullptr)) {
                out_font.pages[ti].srgb = false;
                out_font.pages[ti].address_x = TexAddress::Clamp;
                out_font.pages[ti].address_y = TexAddress::Clamp;
            }
        }
        return out_font.valid();
    }
    return false;
}

} // namespace

bool MainMenuSystem::init(const std::string& game_root) {
    namespace fs = std::filesystem;

    // 1. Parse DefaultUI.ini
    IniConfig ui_ini;
    if (ui_ini.load_file(get_config_path(game_root, "DefaultUI.ini"))) {
        config_.ui_skin_name = strip_quotes(ui_ini.get_string("Engine.UIInteraction", "UISkinName", config_.ui_skin_name));
        config_.scene_anim_duration = ui_ini.get_float("TdGame.TdUIScene", "SceneAnimDuration", config_.scene_anim_duration);
        config_.attract_movie = strip_quotes(ui_ini.get_string("TdGame.TdUIScene_Start", "MovieName", config_.attract_movie));
        config_.time_till_attract_movie = ui_ini.get_float("TdGame.TdUIScene_Start", "TimeTillAttractMovie", config_.time_till_attract_movie);
        config_.debug_unlock_all_levels = ui_ini.get_bool("TdGame.TdUIScene_MainMenu", "bDebugUnlockAllLevels", true);
        config_.show_downloads_button = ui_ini.get_bool("TdGame.TdUIScene_MainMenu", "bShowDownloadsButton", true);
    }

    // 2. Parse TdGameUI.int for localized Main Menu, LoadChapter, and SupersMessage strings
    IniConfig ui_int;
    bool has_ui_int = ui_int.load_file(get_localization_path(game_root, "TdGameUI.int", "INT"));

    std::string story_caption = "STORY";
    std::string race_caption = "RACE";
    std::string options_caption = "OPTIONS";
    std::string extras_caption = "EXTRAS";

    if (has_ui_int) {
        story_caption = strip_quotes(ui_int.get_string("TdMainMenu", "StoryCaptionText", story_caption));
        race_caption = strip_quotes(ui_int.get_string("TdMainMenu", "TimeTrialCaptionText", race_caption));
        options_caption = strip_quotes(ui_int.get_string("TdMainMenu", "OptionsCaptionText", options_caption));
        extras_caption = strip_quotes(ui_int.get_string("TdMainMenu", "ExtrasCaptionText", extras_caption));

        config_.load_chapter_title = strip_quotes(ui_int.get_string("TdLoadLevel", "Title_Text", config_.load_chapter_title));
        config_.select_checkpoint_title = strip_quotes(ui_int.get_string("TdLoadCheckpoint", "Title_Text", config_.select_checkpoint_title));
        config_.speed_run_time_label = strip_quotes(ui_int.get_string("TdLoadLevel", "TimeStaticLabel_Text", config_.speed_run_time_label));
        config_.bags_found_label = strip_quotes(ui_int.get_string("TdLoadLevel", "BagsFoundLabel_Text", config_.bags_found_label));
    }

    tabs_.clear();
    tabs_.push_back(MenuCategoryTab{
        .caption = story_caption,
        .remote_event = "panel1",
        .items = {"CONTINUE GAME", "NEW GAME", "PLAY CHAPTER", "TRAINING AREA"}
    });
    tabs_.push_back(MenuCategoryTab{
        .caption = race_caption,
        .remote_event = "panel2",
        .items = {"SPEED RUN", "TIME TRIAL", "LEADERBOARDS"}
    });
    tabs_.push_back(MenuCategoryTab{
        .caption = options_caption,
        .remote_event = "panel3",
        .items = {"GAMEPAD SETUP", "VIDEO", "AUDIO", "MOUSE & KEYBOARD", "GAME SETTINGS"}
    });
    tabs_.push_back(MenuCategoryTab{
        .caption = extras_caption,
        .remote_event = "panel4",
        .items = {"DOWNLOADABLE CONTENT", "ACHIEVEMENTS", "UNLOCKABLES", "CREDITS"}
    });

    // 3. Parse DefaultGame.ini & TdGame.int for the 10 campaign chapters (0..9)
    IniConfig game_ini;
    game_ini.load_file(get_config_path(game_root, "DefaultGame.ini"));
    IniConfig game_int;
    game_int.load_file(get_localization_path(game_root, "TdGame.int", "INT"));

    struct CanonicalChapterSpec {
        int idx;
        const char* provider_sec;
        const char* default_name;
        const char* map_path;
        const char* level_event;
        const char* supers_key;
        const char* default_supers;
        const char* mi_tag;
        const char* preview_tex;
        const char* speedrun_time;
        int cam_export_0based; // Verified from TdMainMenu.me1 Kismet SequenceFrames
    };

    static const CanonicalChapterSpec kSpecs[10] = {
        {0, "SP01a UIDataProvider_TdMaps", "PROLOGUE - THE EDGE",          "Maps/SP01/Edge_p.me1",       "LoadLevel_Edge",        "SP01A", "Financial District 1.58pm",     "mi_sp01a_01", "Level1a_CP1", "03:00:00", 14},
        {1, "SP01b UIDataProvider_TdMaps", "CHAPTER 1 - FLIGHT",           "Maps/SP01/Escape_p.me1",     "LoadLevel_Escape",      "SP01B", "West Arlington 5.21am",         "mi_sp01b_01", "Level1b_CP1", "06:00:00", 23},
        {2, "SP02 UIDataProvider_TdMaps",  "CHAPTER 2 - JACKNIFE",         "Maps/SP02/Stormdrain_p.me1", "LoadLevel_Stormdrains", "SP02",  "Lower East Side 11.01am",       "mi_sp02_01",  "Level2_CP1",  "11:00:00", 22},
        {3, "SP03 UIDataProvider_TdMaps",  "CHAPTER 3 - HEAT",             "Maps/SP03/Cranes_p.me1",     "LoadLevel_Cranes",      "SP03",  "Ryding Park 12.08pm",           "mi_sp03_01",  "Level3_CP1",  "08:00:00", 21},
        {4, "SP04 UIDataProvider_TdMaps",  "CHAPTER 4 - ROPEBURN",         "Maps/SP04/Subway_p.me1",     "LoadLevel_Subway",      "SP04",  "Renold's Street 7.17pm",        "mi_sp04_01",  "Level4_CP1",  "09:00:00", 20},
        {5, "SP05 UIDataProvider_TdMaps",  "CHAPTER 5 - NEW EDEN",         "Maps/SP05/Mall_p.me1",       "LoadLevel_Mall",        "SP05",  "Downtown 9.07am",               "mi_sp05_01",  "Level5_CP1",  "07:30:00", 15},
        {6, "SP06 UIDataProvider_TdMaps",  "CHAPTER 6 - PIRANDELLO KRUGER","Maps/SP06/Factory_p.me1",    "LoadLevel_Factory",     "SP06",  "The Docks 5.00pm",              "mi_sp06_01",  "Level6_CP1",  "07:30:00", 19},
        {7, "SP07 UIDataProvider_TdMaps",  "CHAPTER 7 - THE BOAT",         "Maps/SP07/Boat_p.me1",       "LoadLevel_Boat",        "SP07",  "The Harbour 11.45pm",           "mi_sp07_01",  "Level7_CP1",  "07:30:00", 18},
        {8, "SP08 UIDataProvider_TdMaps",  "CHAPTER 8 - KATE",             "Maps/SP08/Convoy_p.me1",     "LoadLevel_Convoy",      "SP08",  "Looking Glass Gardens 1.12pm",  "mi_sp08_01",  "Level8_CP1",  "08:00:00", 17},
        {9, "SP09 UIDataProvider_TdMaps",  "CHAPTER 9 - THE SHARD",        "Maps/SP09/Scraper_p.me1",    "LoadLevel_Scraper",     "SP09",  "The Shard 9.55pm",              "mi_sp09_01",  "Level9_CP1",  "09:00:00", 13}
    };

    chapters_.clear();
    chapters_.reserve(10);
    for (const auto& spec : kSpecs) {
        MenuChapterEntry ch;
        ch.chapter_index = spec.idx;
        ch.provider_section = spec.provider_sec;
        ch.map_name = strip_quotes(game_int.get_string(spec.provider_sec, "MapName", spec.default_name));
        ch.map_filename = spec.map_path;
        ch.level_event = strip_quotes(game_ini.get_string(spec.provider_sec, "LevelEvent", spec.level_event));
        ch.district_timestamp = has_ui_int
            ? strip_quotes(ui_int.get_string("TdSupersMessage", spec.supers_key, spec.default_supers))
            : spec.default_supers;
        ch.material_instance_tag = spec.mi_tag;
        ch.preview_tex_name = spec.preview_tex;
        ch.speedrun_target_time = spec.speedrun_time;
        ch.bags_found = (spec.idx == 1) ? 2 : 1;
        ch.bags_total = 3;

        // Parse checkpoints from DefaultGame.ini and localized descriptions from TdGame.int
        const auto& ini_sec = game_ini.get_section_keys(spec.provider_sec);
        const auto& int_sec = game_int.get_section_keys(spec.provider_sec);

        auto it_cp_ini = ini_sec.find("Checkpoints");
        if (it_cp_ini != ini_sec.end()) {
            for (const auto& raw_cp : it_cp_ini->second) {
                MenuCheckpointEntry cp;
                cp.id = extract_kv_field(raw_cp, "CheckpointName");
                std::string markup = extract_kv_field(raw_cp, "CheckpointImageMarkup");
                size_t dot = markup.rfind('.');
                if (dot != std::string::npos) {
                    std::string img = markup.substr(dot + 1);
                    if (!img.empty() && img.back() == '>') img.pop_back();
                    cp.image_name = img;
                }
                ch.checkpoints.push_back(cp);
            }
        }

        for (size_t c = 0; c < 8; ++c) {
            std::string key = "Checkpoints[" + std::to_string(c) + "]";
            auto it_cp_int = int_sec.find(key);
            if (it_cp_int != int_sec.end() && !it_cp_int->second.empty()) {
                const std::string& raw_loc = it_cp_int->second.front();
                if (c >= ch.checkpoints.size()) {
                    ch.checkpoints.push_back(MenuCheckpointEntry{});
                }
                ch.checkpoints[c].friendly_name = extract_kv_field(raw_loc, "CheckpointFriendlyName");
                ch.checkpoints[c].description = extract_kv_field(raw_loc, "CheckpointDescription");
            }
        }

        if (ch.checkpoints.empty()) {
            ch.checkpoints.push_back(MenuCheckpointEntry{
                .id = "Start",
                .friendly_name = "CHECKPOINT A",
                .description = "Start of level",
                .image_name = spec.preview_tex
            });
        }

        chapters_.push_back(std::move(ch));
    }

    // 4. Load 3D Main Menu City Panorama (Maps/Menu/TdMainMenu.me1) & Extract CameraActor viewpoints
    fs::path root_path(game_root);
    fs::path cooked_root = root_path / "TdGame" / "CookedPC";
    if (!fs::exists(cooked_root)) {
        cooked_root = root_path / "CookedPC";
    }

    if (fs::exists(cooked_root)) {
        PackageManager pm(cooked_root.string());

        // Load 3D City of Glass menu level (S_City_01..05, S_CityBase_01, S_CityBaseMountains_01, S_CityBaseWater_01)
        if (load_level_scene(game_root, "Maps/Menu/TdMainMenu.me1", city_scene_)) {
            city_scene_.sun_direction = Vec3(-0.22f, 0.76f, 0.61f).normalized();
            city_scene_.sun_color = Vec3(2.35f, 2.24f, 2.08f);
            if (city_scene_.materials) {
                mutable_city_materials_ = std::make_shared<SceneMaterialLibrary>(*city_scene_.materials);
                city_scene_.materials = mutable_city_materials_;
            }
        }

        // Extract exact CameraActor transforms & inline textures from Maps/Menu/TdMainMenu.me1
        fs::path mm_path = cooked_root / "Maps" / "Menu" / "TdMainMenu.me1";
        std::shared_ptr<UPKPackage> mm_pkg;
        if (fs::exists(mm_path)) {
            mm_pkg = std::make_shared<UPKPackage>(mm_path.string());
            if (mm_pkg->is_valid()) {
                pm.add_loaded("TdMainMenu", mm_pkg);
                const auto& exps = mm_pkg->get_exports();

                auto read_cam = [&](int exp_0based, Vec3& out_loc, Rotator& out_rot) {
                    if (exp_0based < 0 || static_cast<size_t>(exp_0based) >= exps.size()) return;
                    const auto& e = exps[exp_0based];
                    if (mm_pkg->get_export_class(e) != "CameraActor") return;
                    size_t ps = mm_pkg->find_property_start(e);
                    size_t end = static_cast<size_t>(e.serial_offset) + static_cast<size_t>(e.serial_size);
                    if (ps < end) {
                        auto props = mm_pkg->parse_properties(ps, end - ps);
                        if (props.count("Location")) out_loc = props["Location"].vec_val;
                        if (props.count("Rotation")) out_rot = props["Rotation"].rot_val;
                    }
                };

                read_cam(12, main_cam_loc_, main_cam_rot_);
                for (size_t i = 0; i < 10; ++i) {
                    read_cam(kSpecs[i].cam_export_0based, chapters_[i].camera_location, chapters_[i].camera_rotation);
                }

                // Aim each chapter camera across the skyline toward its MI_SP0*_01 skyscraper cluster
                // with DICE's panoramic horizon pitch so the glowing district and M_Skydome_Menu shine unobstructed.
                if (mutable_city_materials_ && !city_scene_.meshes.empty()) {
                    const auto& mesh = city_scene_.meshes[0];
                    for (size_t i = 0; i < 10; ++i) {
                        std::string tag = to_lower_copy(chapters_[i].material_instance_tag);
                        Vec3 sum{0.0f, 0.0f, 0.0f};
                        float max_z = -1e9f;
                        size_t count = 0;
                        for (const auto& s : mesh.sections) {
                            if (s.vertex_count <= 0 || s.material < 0 ||
                                static_cast<size_t>(s.material) >= mutable_city_materials_->materials.size()) continue;
                            std::string mname = to_lower_copy(mutable_city_materials_->materials[static_cast<size_t>(s.material)].name);
                            if (mname.find(tag) != std::string::npos) {
                                size_t end_v = std::min(mesh.vertices.size(),
                                                        static_cast<size_t>(s.first_vertex + s.vertex_count));
                                for (size_t v = static_cast<size_t>(s.first_vertex); v < end_v; ++v) {
                                    sum += mesh.vertices[v].position;
                                    max_z = std::max(max_z, mesh.vertices[v].position.z);
                                    count++;
                                }
                            }
                        }
                        if (count > 0) {
                            Vec3 center = sum / static_cast<float>(count);
                            Vec3 look_target(center.x, center.y, max_z + 28.0f);
                            Vec3 dir = (look_target - chapters_[i].camera_location).normalized();
                            float pitch_rad = std::clamp(std::asin(std::clamp(dir.z, -1.0f, 1.0f)), -0.11f, 0.04f);
                            float yaw_rad   = std::atan2(dir.y, dir.x);
                            chapters_[i].camera_rotation = Rotator::from_radians(pitch_rad, yaw_rad, 0.0f);
                        }
                    }
                }

                // Load StartTitleImage (1024x256 PF_DXT5) from TdMainMenu.me1
                load_ui_texture_by_name(pm, *mm_pkg, "StartTitleImage", 1024, logo_tex_);
            }
        }

        // 5. Load retail Mirror's Edge MultiFont atlases from UI/UI_Fonts_Final.upk
        fs::path font_path = cooked_root / "UI" / "UI_Fonts_Final.upk";
        if (fs::exists(font_path)) {
            auto font_pkg = std::make_shared<UPKPackage>(font_path.string());
            if (font_pkg->is_valid()) {
                pm.add_loaded("UI_Fonts_Final", font_pkg);
                load_ui_multifont(pm, *font_pkg, "Helvetica_Headline_Thick_Italic", font_headline_thick_);
                load_ui_multifont(pm, *font_pkg, "Helvetica_Headline_Light_Italic", font_headline_light_);
                load_ui_multifont(pm, *font_pkg, "Helvetica_Medium_Italic", font_medium_italic_);
                load_ui_multifont(pm, *font_pkg, "Helvetica_Small_Bold_Italic", font_small_italic_);
            }
        }

        // 6. Load UI skin & icon textures from UI/TdUIResources.upk
        fs::path res_path = cooked_root / "UI" / "TdUIResources.upk";
        if (fs::exists(res_path)) {
            auto res_pkg = std::make_shared<UPKPackage>(res_path.string());
            if (res_pkg->is_valid()) {
                pm.add_loaded("TdUIResources", res_pkg);
                if (!logo_tex_.valid()) {
                    load_ui_texture_by_name(pm, *res_pkg, "StartTitleImage", 1024, logo_tex_);
                }
                load_ui_texture_by_name(pm, *res_pkg, "Icon_Bag", 128, icon_bag_tex_);
                load_ui_texture_by_name(pm, *res_pkg, "Icon_Time", 128, icon_time_tex_);
                load_ui_texture_by_name(pm, *res_pkg, "LoadCheckpoint_BG", 1024, panel_bg_tex_);
            }
        }

        // 7. Load Faith & diagonal red stripe artwork from UI/TdUIResources_FrontEnd.upk
        fs::path fres_path = cooked_root / "UI" / "TdUIResources_FrontEnd.upk";
        if (fs::exists(fres_path)) {
            auto fres_pkg = std::make_shared<UPKPackage>(fres_path.string());
            if (fres_pkg->is_valid()) {
                pm.add_loaded("TdUIResources_FrontEnd", fres_pkg);
                load_ui_texture_by_name(pm, *fres_pkg, "T_Faith_03", 1024, faith_art_tex_);
            }
        }

        // 8. Load Chapter Preview Halftone Photographs (Level1a_CP1..Level9_CP1, 512x256 PF_DXT5/PF_DXT1)
        fs::path cp_path = cooked_root / "UI" / "TdUIResources_CheckpointImages.upk";
        std::shared_ptr<UPKPackage> cp_pkg;
        if (fs::exists(cp_path)) {
            cp_pkg = std::make_shared<UPKPackage>(cp_path.string());
            if (cp_pkg->is_valid()) {
                pm.add_loaded("TdUIResources_CheckpointImages", cp_pkg);
            }
        }

        for (size_t i = 0; i < 10; ++i) {
            bool ok = false;
            if (cp_pkg && cp_pkg->is_valid()) {
                ok = load_ui_texture_by_name(pm, *cp_pkg, chapters_[i].preview_tex_name, 512, chapter_preview_tex_[i]);
            }
            if (!ok && mm_pkg && mm_pkg->is_valid()) {
                load_ui_texture_by_name(pm, *mm_pkg, chapters_[i].preview_tex_name, 512, chapter_preview_tex_[i]);
            }
        }
    }

    update_selected_chapter_highlight(1);
    loaded_ = true;

    int loaded_previews = 0;
    for (const auto& t : chapter_preview_tex_) {
        if (t.valid()) loaded_previews++;
    }
    std::cout << "[MainMenu] Initialized Mirror's Edge Frontend UI ("
              << tabs_.size() << " tabs, " << chapters_.size() << " chapters, "
              << "Logo=" << (logo_tex_.valid() ? "OK" : "NO")
              << ", Fonts=" << (font_headline_thick_.valid() && font_medium_italic_.valid() ? "OK" : "NO")
              << ", Previews=" << loaded_previews << "/10"
              << ", CityMeshes=" << city_scene_.meshes.size() << ")" << std::endl;
    return true;
}

const MenuChapterEntry& MainMenuSystem::get_chapter(int idx) const {
    static const MenuChapterEntry kFallback{};
    if (chapters_.empty()) return kFallback;
    int clamped = std::clamp(idx, 0, static_cast<int>(chapters_.size()) - 1);
    return chapters_[clamped];
}

const SceneTexture& MainMenuSystem::chapter_preview_texture(int idx) const {
    int clamped = std::clamp(idx, 0, 9);
    return chapter_preview_tex_[clamped];
}

void MainMenuSystem::update_selected_chapter_highlight(int selected_chapter_idx) {
    if (!mutable_city_materials_ || chapters_.empty()) return;
    int clamped = std::clamp(selected_chapter_idx, 0, static_cast<int>(chapters_.size()) - 1);
    std::string active_tag = to_lower_copy(chapters_[clamped].material_instance_tag);

    for (auto& mat : mutable_city_materials_->materials) {
        std::string mlow = to_lower_copy(mat.name);
        if (mlow.find("mi_sp0") != std::string::npos) {
            bool is_active = (mlow.find(active_tag) != std::string::npos);
            if (!mat.uniforms.empty()) {
                mat.uniforms[0][0] = is_active ? 1.0f : 0.0f;
            }
        }
    }
}

} // namespace me
