#pragma once

#include "../math/types.hpp"
#include "../assets/scene_materials.hpp"
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace me {

struct MenuCheckpointEntry {
    std::string id;
    std::string friendly_name;
    std::string description;
    std::string image_name;
};

struct MenuChapterEntry {
    int chapter_index = 0;
    std::string provider_section;
    std::string map_name;
    std::string map_filename;
    std::string level_event;
    std::string district_timestamp;
    std::string material_instance_tag;
    std::string preview_tex_name;
    std::string speedrun_target_time;
    int bags_found = 0;
    int bags_total = 3;
    Vec3 camera_location{58.822f, 1600.015f, 231.719f};
    Rotator camera_rotation{-1441.0f, -12479.0f, 0.0f};
    std::vector<MenuCheckpointEntry> checkpoints;
};

struct MenuCategoryTab {
    std::string caption;
    std::string remote_event;
    std::vector<std::string> items;
};

struct MainMenuConfig {
    std::string ui_skin_name = "UI_Skins.UI_Skins_TDUISkins2";
    float scene_anim_duration = 0.25f;
    std::string attract_movie = "Attract_Movie";
    float time_till_attract_movie = 90.0f;
    bool debug_unlock_all_levels = true;
    bool show_downloads_button = true;

    // Localized headings from TdGameUI.int
    std::string load_chapter_title = "LOAD CHAPTER";
    std::string select_checkpoint_title = "SELECT CHECKPOINT";
    std::string speed_run_time_label = "SPEED RUN TIME";
    std::string bags_found_label = "BAGS FOUND:";
};

/**
 * MainMenuSystem: Reverse-engineered Mirror's Edge Frontend UI & 3D Menu City Subsystem.
 *
 * Loads authentic retail assets from /Users/tomnom/mirrorsedge/TdGame:
 *  - Config/DefaultUI.ini (TdUIScene, TdUIScene_MainMenu, TdUIScene_Start parameters)
 *  - Config/DefaultGame.ini ([... UIDataProvider_TdMaps] map & checkpoint bindings)
 *  - Localization/INT/TdGameUI.int ([TdMainMenu], [TdLoadLevel], [TdLoadCheckpoint], [TdSupersMessage])
 *  - Localization/INT/TdGame.int ([... UIDataProvider_TdMaps] localized chapter & checkpoint names)
 *  - CookedPC/Maps/Menu/TdMainMenu.me1 (S_City_01..05 3D City of Glass panorama, 12 CameraActors,
 *    MI_SP00_01..MI_SP09_01 district highlight MaterialInstanceConstants, and StartTitleImage)
 *  - CookedPC/UI/TdUIResources.upk (StartTitleImage, Icon_Bag, Icon_Time, LoadCheckpoint_BG)
 *  - CookedPC/UI/TdUIResources_FrontEnd.upk (T_Faith_03 character & diagonal stripe artwork)
 *  - CookedPC/UI/TdUIResources_CheckpointImages.upk (Level1a_CP1..Level9_CP7 halftone chapter previews)
 */
class MainMenuSystem {
public:
    MainMenuSystem() = default;
    ~MainMenuSystem() = default;

    bool init(const std::string& game_root);

    [[nodiscard]] bool is_loaded() const { return loaded_; }
    [[nodiscard]] const MainMenuConfig& config() const { return config_; }
    [[nodiscard]] const std::vector<MenuCategoryTab>& tabs() const { return tabs_; }
    [[nodiscard]] const std::vector<MenuChapterEntry>& chapters() const { return chapters_; }
    [[nodiscard]] const MenuChapterEntry& get_chapter(int idx) const;

    [[nodiscard]] bool has_city_scene() const { return !city_scene_.meshes.empty(); }
    [[nodiscard]] const LevelScene& city_scene() const { return city_scene_; }
    [[nodiscard]] Vec3 main_camera_location() const { return main_cam_loc_; }
    [[nodiscard]] Rotator main_camera_rotation() const { return main_cam_rot_; }

    // Updates the 'Selected' scalar parameter on MI_SP00_01..MI_SP09_01 in TdMainMenu.me1's material library
    // (reproducing Kismet Sequence 'Level_Selection_Fade' in TdMainMenu.me1).
    void update_selected_chapter_highlight(int selected_chapter_idx);

    [[nodiscard]] const SceneTexture& logo_texture() const { return logo_tex_; }
    [[nodiscard]] const SceneTexture& icon_bag_texture() const { return icon_bag_tex_; }
    [[nodiscard]] const SceneTexture& icon_time_texture() const { return icon_time_tex_; }
    [[nodiscard]] const SceneTexture& panel_bg_texture() const { return panel_bg_tex_; }
    [[nodiscard]] const SceneTexture& faith_art_texture() const { return faith_art_tex_; }
    [[nodiscard]] const SceneTexture& chapter_preview_texture(int idx) const;

private:
    bool loaded_ = false;
    MainMenuConfig config_;
    std::vector<MenuCategoryTab> tabs_;
    std::vector<MenuChapterEntry> chapters_;

    LevelScene city_scene_;
    std::shared_ptr<SceneMaterialLibrary> mutable_city_materials_;
    Vec3 main_cam_loc_{58.822f, 1600.015f, 231.719f};
    Rotator main_cam_rot_{-1441.0f, -12479.0f, 0.0f};

    SceneTexture logo_tex_;
    SceneTexture icon_bag_tex_;
    SceneTexture icon_time_tex_;
    SceneTexture panel_bg_tex_;
    SceneTexture faith_art_tex_;
    std::array<SceneTexture, 10> chapter_preview_tex_;
};

} // namespace me
