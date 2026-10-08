#include "frontend_menus.hpp"

#include "frontend.hpp"

#include <algorithm>
#include <cmath>

namespace me::fe {

namespace {

constexpr int kFaceLeft = 0, kFaceTop = 1, kFaceRight = 2, kFaceBottom = 3;

// "<OnlinePlayerData:ProfileData.GameDifficulty>" -> ("OnlinePlayerData", "ProfileData.GameDifficulty").
bool split_markup(const std::string& markup, std::string& store, std::string& field) {
    if (markup.size() < 4 || markup.front() != '<' || markup.back() != '>') return false;
    const size_t colon = markup.find(':');
    if (colon == std::string::npos) return false;
    store = markup.substr(1, colon - 1);
    field = markup.substr(colon + 1, markup.size() - colon - 2);
    return true;
}

}  // namespace

// --- SubMenu ---------------------------------------------------------------------------------

SubMenu::SubMenu(Frontend& fe, const std::string& package, const std::string& scene) : fe_(fe) {
    scene_ = fe.ui_.load_scene(package, scene);
}

SubMenu::~SubMenu() = default;

std::string SubMenu::focused_name() const {
    return (scene_ && focus_ >= 0) ? scene_->widgets[static_cast<size_t>(focus_)].name : std::string();
}

void SubMenu::opened() {
    bind_profile_widgets();
    focus_first();
    scene_->layout();
}

UiSystem& SubMenu::ui() { return fe_.ui_; }
Assets& SubMenu::assets() { return fe_.assets_; }
const Profile& SubMenu::profile() const { return fe_.profile_; }
ProfileSettings& SubMenu::settings() { return fe_.settings_; }
StringList& SubMenu::string_list(const std::string& tag) { return fe_.string_lists_[tag]; }

void SubMenu::close() { fe_.close_scene(this, nullptr); }
void SubMenu::close_then(const std::function<void()>& then) { fe_.close_scene(this, then); }

void SubMenu::play(const char* cue) { fe_.sound(cue); }
void SubMenu::level_event(const std::string& name) { fe_.level_event(name); }
void SubMenu::open(std::unique_ptr<SubMenu> menu) { fe_.open_scene(std::move(menu)); }
void SubMenu::host_action(const std::string& action) { fe_.action_ = action; }

void SubMenu::draw(Frame& f, float scale, float origin_x, float gamma) {
    if (scene_) scene_->draw(f, scale, origin_x, gamma);
}

int SubMenu::bar_append(const std::string& markup, const std::string& bar) {
    std::vector<UiScene::BarButton>& buttons = scene_->bar(bar);
    UiScene::BarButton b;
    // TdUIButtonBar.CheckMarkup: a bare word names a TdButtonCallouts string.
    const bool is_markup = markup.find('<') != std::string::npos && markup.find('>') != std::string::npos;
    b.label = fe_.ui_.resolve_markup(is_markup ? markup : "<Strings:TdGameUI.TdButtonCallouts." + markup + ">");
    buttons.push_back(std::move(b));
    return static_cast<int>(buttons.size()) - 1;
}

void SubMenu::bar_disable(int button, bool disable, const std::string& bar) {
    std::vector<UiScene::BarButton>& buttons = scene_->bar(bar);
    if (button >= 0 && static_cast<size_t>(button) < buttons.size()) buttons[static_cast<size_t>(button)].disabled = disable;
}

bool SubMenu::focusable(int widget) const {
    if (widget < 0 || !scene_->visible(widget)) return false;
    const UiWidget& w = scene_->widgets[static_cast<size_t>(widget)];
    if (w.disabled) return false;
    return w.cls == "UITdOptionButton" || w.cls == "UISlider" || w.cls == "UILabelButton" || w.cls == "UIList";
}

// SetFocus on a container goes to its first child in TabIndex order.
void SubMenu::focus_first(int under) {
    std::vector<int> stack{under};
    while (!stack.empty()) {
        const int i = stack.back();
        stack.pop_back();
        if (i != under && focusable(i)) {
            set_focus(i);
            return;
        }
        const UiWidget& w = scene_->widgets[static_cast<size_t>(i)];
        if (w.hidden) continue;
        std::vector<int> order = w.children;
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            return scene_->widgets[static_cast<size_t>(a)].tab_index < scene_->widgets[static_cast<size_t>(b)].tab_index;
        });
        for (size_t k = order.size(); k-- > 0;) stack.push_back(order[k]);
    }
}

void SubMenu::set_focus(int widget) {
    auto mark = [&](int index, bool on) {
        if (index < 0) return;
        UiWidget& w = scene_->widgets[static_cast<size_t>(index)];
        w.focused = on;
        // The focus label and the arrows of an option row take their owner's state.
        for (int child : w.children) {
            UiWidget& c = scene_->widgets[static_cast<size_t>(child)];
            if (c.cls == "TdUIFocusLabel" || c.cls == "UIButton") c.focused = on;
        }
    };
    if (widget == focus_) return;
    mark(focus_, false);
    focus_ = widget;
    mark(focus_, true);
    focus_changed();
}

void SubMenu::navigate(int face) {
    if (focus_ < 0) {
        focus_first();
        return;
    }
    const UiWidget& from = scene_->widgets[static_cast<size_t>(focus_)];
    int target = from.forced_nav[face];
    if (target >= 0 && !focusable(target)) target = -1;
    if (target < 0) {
        // The nearest focusable widget that way.
        float best = 1.0e9f;
        const float cx = (from.rect.l + from.rect.r) * 0.5f, cy = (from.rect.t + from.rect.b) * 0.5f;
        for (size_t i = 1; i < scene_->widgets.size(); ++i) {
            if (static_cast<int>(i) == focus_ || !focusable(static_cast<int>(i))) continue;
            const Rect& r = scene_->widgets[i].rect;
            const float dx = (r.l + r.r) * 0.5f - cx, dy = (r.t + r.b) * 0.5f - cy;
            const float along = face == kFaceTop ? -dy : (face == kFaceBottom ? dy : (face == kFaceLeft ? -dx : dx));
            const float across = (face == kFaceTop || face == kFaceBottom) ? std::fabs(dx) : std::fabs(dy);
            if (along <= 1.0f) continue;
            const float score = along + across * 2.0f;
            if (score < best) {
                best = score;
                target = static_cast<int>(i);
            }
        }
    }
    if (target < 0) return;
    set_focus(target);
    play(face == kFaceTop ? "NavigateUp" : "NavigateDown");
}

SubMenu::Option* SubMenu::option_of(int widget) {
    for (Option& o : options_) {
        if (o.widget == widget) return &o;
    }
    return nullptr;
}

SubMenu::Option* SubMenu::option_named(const std::string& widget) { return option_of(scene_->find(widget)); }

SubMenu::Option& SubMenu::add_option(const std::string& widget, std::vector<std::string> labels, int index) {
    Option o;
    o.widget = scene_->find(widget);
    o.labels = std::move(labels);
    for (size_t i = 0; i < o.labels.size(); ++i) o.ids.push_back(static_cast<int>(i));
    o.index = o.labels.empty() ? 0 : std::clamp(index, 0, static_cast<int>(o.labels.size()) - 1);
    options_.push_back(std::move(o));
    show_option(options_.back());
    return options_.back();
}

void SubMenu::show_option(Option& o) {
    if (o.widget < 0) return;
    UiWidget& w = scene_->widgets[static_cast<size_t>(o.widget)];
    w.text = (o.index >= 0 && static_cast<size_t>(o.index) < o.labels.size()) ? o.labels[static_cast<size_t>(o.index)] : std::string();
    // UpdateArrowStates: with nothing to step to, the arrows are disabled.
    for (int child : w.children) {
        UiWidget& c = scene_->widgets[static_cast<size_t>(child)];
        if (c.cls == "UIButton") c.disabled = o.labels.size() < 2;
    }
}

void SubMenu::step_option(Option& o, int by) {
    const int n = static_cast<int>(o.labels.size());
    if (n < 2) return;
    o.index = ((o.index + by) % n + n) % n;
    if (o.setting) o.setting->value = o.ids[static_cast<size_t>(o.index)];
    show_option(o);
    play(by > 0 ? "SliderIncrement" : "SliderDecrement");
    value_changed(o.widget);
}

void SubMenu::bind_profile_widgets() {
    for (size_t i = 1; i < scene_->widgets.size(); ++i) {
        UiWidget& w = scene_->widgets[i];
        std::string store, field;
        if (!split_markup(w.markup, store, field)) continue;
        if (store == "OnlinePlayerData" && field.rfind("ProfileData.", 0) == 0) {
            ProfileSetting* setting = fe_.settings_.find(field.substr(12));
            if (!setting) continue;
            if (w.cls == "UISlider") {
                w.slider[2] = static_cast<float>(setting->value);
            } else if (w.cls == "UITdOptionButton" && !option_of(static_cast<int>(i))) {
                Option o;
                o.widget = static_cast<int>(i);
                o.setting = setting;
                for (size_t k = 0; k < setting->value_ids.size(); ++k) {
                    // HARD is offered once the story has been finished.
                    if (setting->name == "GameDifficulty" && setting->value_ids[k] == 2 && !fe_.profile_.hard_unlocked) continue;
                    o.ids.push_back(setting->value_ids[k]);
                    o.labels.push_back(fe_.ui_.resolve_markup(setting->value_names[k]));
                    if (setting->value_ids[k] == setting->value) o.index = static_cast<int>(o.ids.size()) - 1;
                }
                options_.push_back(std::move(o));
                show_option(options_.back());
            }
        } else if (store == "TdStringList" && w.cls == "UITdOptionButton" && !option_of(static_cast<int>(i))) {
            const StringList& list = fe_.string_lists_[field];
            Option o;
            o.widget = static_cast<int>(i);
            o.labels = list.values;
            for (size_t k = 0; k < o.labels.size(); ++k) o.ids.push_back(static_cast<int>(k));
            o.index = o.labels.empty() ? 0 : std::clamp(list.index, 0, static_cast<int>(o.labels.size()) - 1);
            options_.push_back(std::move(o));
            show_option(options_.back());
        }
    }
}

void SubMenu::key_pressed(Key key) {
    if (!scene_) return;
    if (key == Key::Up) navigate(kFaceTop);
    else if (key == Key::Down) navigate(kFaceBottom);
    else if ((key == Key::Left || key == Key::Right) && focus_ >= 0) {
        UiWidget& w = scene_->widgets[static_cast<size_t>(focus_)];
        const int by = key == Key::Right ? 1 : -1;
        if (Option* o = option_of(focus_)) {
            step_option(*o, by);
        } else if (w.cls == "UISlider") {
            const float v = std::clamp(w.slider[2] + w.slider[3] * static_cast<float>(by), w.slider[0], w.slider[1]);
            if (v != w.slider[2]) {
                w.slider[2] = v;
                std::string store, field;
                if (split_markup(w.markup, store, field) && field.rfind("ProfileData.", 0) == 0) {
                    if (ProfileSetting* s = fe_.settings_.find(field.substr(12))) s->value = static_cast<int>(std::lround(v));
                }
                play(by > 0 ? "SliderIncrement" : "SliderDecrement");
                value_changed(focus_);
            }
        }
    }
}

void SubMenu::key_released(Key key) {
    if (key == Key::Escape) {
        play("Cancel");
        close();
    }
}

// --- TdUIScene_MessageBox --------------------------------------------------------------------

namespace {

class MessageBoxMenu : public SubMenu {
public:
    MessageBoxMenu(Frontend& fe, std::string title, std::string message, std::vector<MenuChoice> choices, std::function<void(int)> selected,
                   float seconds)
        : SubMenu(fe, "TdUI", "TdMessageBox"), title_(std::move(title)), message_(std::move(message)), choices_(std::move(choices)),
          selected_(std::move(selected)), seconds_(seconds) {}

    void opened() override {
        // Display(): the title, the message, one button per option.
        scene_->set_text(ui(), "TitleLabel", title_);
        scene_->set_text(ui(), "MessageLabel", message_);
        for (const MenuChoice& c : choices_) bar_append(c.markup);
        scene_->layout();
    }
    void tick(float dt) override {
        if (seconds_ <= 0.0f) return;
        seconds_ -= dt;
        if (seconds_ <= 0.0f) select(-1);
    }
    void key_pressed(Key) override {}
    void key_released(Key key) override {
        for (size_t i = 0; i < choices_.size(); ++i) {
            if (choices_[i].key != key) continue;
            play(key == Key::Escape ? "Cancel" : "Accept");
            select(static_cast<int>(i));
            return;
        }
    }

private:
    // OptionSelected: the scene closes, then the selection is reported.
    void select(int option) {
        const std::function<void(int)> selected = selected_;
        close_then([selected, option] {
            if (selected) selected(option);
        });
    }
    std::string title_, message_;
    std::vector<MenuChoice> choices_;
    std::function<void(int)> selected_;
    float seconds_ = 0.0f;
};

// TdUIScene_OnlineCheck.
class OnlineCheckMenu : public SubMenu {
public:
    OnlineCheckMenu(Frontend& fe, std::function<void()> play_offline)
        : SubMenu(fe, "TdUI_FrontEnd_Online", "TdOnlineCheck"), play_offline_(std::move(play_offline)) {}
    void opened() override {
        bar_append("Back");
        bar_append("<Strings:TdGameUI.TdButtonCallouts.PlayOffline>");
        scene_->layout();
    }
    void key_pressed(Key) override {}
    void key_released(Key key) override {
        if (key == Key::Accept) {
            play("Accept");
            const std::function<void()> go = play_offline_;
            close_then([go] {
                if (go) go();
            });
        } else if (key == Key::Escape) {
            play("Cancel");
            close();
        }
    }

private:
    std::function<void()> play_offline_;
};

}  // namespace

std::unique_ptr<SubMenu> make_message_box(Frontend& fe, const std::string& title, const std::string& message, std::vector<MenuChoice> choices,
                                          std::function<void(int)> selected, float seconds) {
    return std::make_unique<MessageBoxMenu>(fe, title, message, std::move(choices), std::move(selected), seconds);
}

std::unique_ptr<SubMenu> make_online_check(Frontend& fe, std::function<void()> play_offline) {
    return std::make_unique<OnlineCheckMenu>(fe, std::move(play_offline));
}

void SubMenu::message_box(const std::string& title, const std::string& message, std::vector<MenuChoice> choices,
                          std::function<void(int)> selected) {
    fe_.open_scene(make_message_box(fe_, title, message, std::move(choices), std::move(selected)));
}

void SubMenu::accept_cancel_box(const std::string& title, const std::string& message, std::function<void(int)> selected) {
    message_box(title, message,
                {{"<Strings:TdGameUI.TdButtonCallouts.Cancel>", Key::Escape}, {"<Strings:TdGameUI.TdButtonCallouts.OK>", Key::Accept}},
                std::move(selected));
}

namespace {

// --- TdUIScene_DifficultySettings (NEW GAME) -------------------------------------------------

class DifficultyMenu : public SubMenu {
public:
    explicit DifficultyMenu(Frontend& fe) : SubMenu(fe, "TdUI_FrontEnd", "TdDifficultySettings") {}
    void opened() override {
        bar_append("<Strings:TdGameUI.TdbuttonCallouts.Cancel>");
        bar_append("<Strings:TdGameUI.TdbuttonCallouts.Accept>");
        SubMenu::opened();
    }
    void key_released(Key key) override {
        if (key == Key::Accept) {
            // OnAccept: the difficulty is saved, the old progress cleared, the game started.
            play("Accept");
            host_action("NewGame");
        } else {
            SubMenu::key_released(key);
        }
    }
};

// --- TdUIScene_LoadCheckpoint ----------------------------------------------------------------

class LoadCheckpointMenu : public SubMenu {
public:
    LoadCheckpointMenu(Frontend& fe, const MapProvider& map) : SubMenu(fe, "TdUI_FrontEnd", "TdLoadCheckpoint"), map_(map) {}
    void opened() override {
        bar_append("Back");
        bar_append("Accept");
        std::vector<std::string> labels;
        for (const MapCheckpoint& cp : map_.checkpoints) labels.push_back(cp.friendly.empty() ? cp.name : cp.friendly);
        add_option("CheckpointOptionButton", std::move(labels), 0);
        SubMenu::opened();
        value_changed(-1);
    }
    void value_changed(int) override {
        // OnCheckpointChanged: the checkpoint's picture.
        Option* o = option_named("CheckpointOptionButton");
        UiWidget* image = scene_->get("CheckpointImage");
        if (!o || !image || map_.checkpoints.empty()) return;
        const MapCheckpoint& cp = map_.checkpoints[static_cast<size_t>(o->index)];
        image->image.texture = cp.image.empty() ? nullptr : assets().image(cp.image);
        image->image.present = true;
    }
    void key_released(Key key) override {
        if (key == Key::Accept) {
            Option* o = option_named("CheckpointOptionButton");
            play("Accept");
            std::string action = "StartLevel " + map_.file;
            if (o && !map_.checkpoints.empty()) action += " " + map_.checkpoints[static_cast<size_t>(o->index)].name;
            host_action(action);
        } else {
            SubMenu::key_released(key);
        }
    }

private:
    const MapProvider& map_;
};

// --- TdUIScene_LoadLevel (PLAY CHAPTER) ------------------------------------------------------

class LoadLevelMenu : public SubMenu {
public:
    explicit LoadLevelMenu(Frontend& fe) : SubMenu(fe, "TdUI_FrontEnd", "TdLoadLevel") {}
    void opened() override {
        bar_append("Back");
        bar_append("Accept");
        // <TdGameData:TdMaps>: the chapters the profile has unlocked.
        std::vector<std::string> labels;
        const std::vector<MapProvider>& maps = map_list();
        for (size_t i = 0; i < maps.size(); ++i) {
            if (!(unlocked() & (1u << i))) continue;
            labels.push_back(maps[i].name);
            indices_.push_back(static_cast<int>(i));
        }
        add_option("LevelOptionButton", std::move(labels), 0);
        SubMenu::opened();
        update_level_data();
    }
    void value_changed(int) override { update_level_data(); }
    void key_released(Key key) override {
        if (key == Key::Accept) {
            // TryOpenScene: the checkpoints of a chapter that has them, else straight into the level.
            const MapProvider* map = current();
            if (!map) return;
            play("Accept");
            if (!map->checkpoints.empty()) open(std::make_unique<LoadCheckpointMenu>(fe_, *map));
            else host_action("StartLevel " + map->file);
        } else {
            SubMenu::key_released(key);
        }
    }

private:
    const std::vector<MapProvider>& map_list() { return assets().maps; }
    uint32_t unlocked() const { return profile().unlocked_levels; }
    const MapProvider* current() {
        Option* o = option_named("LevelOptionButton");
        if (!o || indices_.empty()) return nullptr;
        return &map_list()[static_cast<size_t>(indices_[static_cast<size_t>(o->index)])];
    }
    // UpdateLevelData: the chapter's camera and district, and its statistics (not for the Training Area).
    void update_level_data() {
        const MapProvider* map = current();
        if (!map) return;
        if (!map->level_event.empty()) level_event(map->level_event);
        const bool stats = map != &map_list().front();
        scene_->set_visible("LevelStatsPanel", stats);
        if (UiWidget* bags = scene_->get("BagsFoundDataLabel")) bags->text = "0/3";
        if (UiWidget* time = scene_->get("LevelRaceTimeLabel")) time->text = "--:--:--";
        scene_->layout();
    }
    std::vector<int> indices_;
};

// --- TdUIScene_OptionMenu: GAME SETTINGS, AUDIO, VIDEO ---------------------------------------

class OptionMenu : public SubMenu {
public:
    OptionMenu(Frontend& fe, const std::string& scene) : SubMenu(fe, "TdUI_Options", scene) {}
    void opened() override {
        // SetupButtonBar: Cancel, Save settings (disabled until something changes), Defaults.
        bar_append("<Strings:TdGameUI.TdbuttonCallouts.Cancel>");
        bar_append("<Strings:TdGameUI.TdbuttonCallouts.SaveSettings>");
        bar_append("<Strings:TdGameUI.TdbuttonCallouts.ResetToDefaults>");
        bar_disable(1, true);
        SubMenu::opened();
        backup();
        focus_changed();
    }
    void value_changed(int) override {
        // OnChange
        changed_ = true;
        bar_disable(1, false);
    }
    // The description of the focused row: "<Strings:...FooText>" on its label names "...FooDesc".
    void focus_changed() override {
        if (focus_ < 0) return;
        for (int child : scene_->widgets[static_cast<size_t>(focus_)].children) {
            const UiWidget& label = scene_->widgets[static_cast<size_t>(child)];
            if (label.cls != "TdUIFocusLabel") continue;
            std::string markup = label.markup;
            const size_t at = markup.rfind("Text>");
            if (at == std::string::npos) continue;
            markup.replace(at, 5, "Desc>");
            scene_->set_text(ui(), "DescriptionLabel", markup);
            scene_->layout();
        }
    }
    void key_released(Key key) override {
        if (key == Key::Accept) {
            if (!changed_) return;
            // OnAccept: the values are written to the profile and the scene closes.
            play("Accept");
            commit();
            host_action("ApplySettings");
            close();
        } else if (key == Key::Escape) {
            play("Cancel");
            if (!changed_) {
                close();
                return;
            }
            // OnCancel: "The changes you made will not be saved."
            accept_cancel_box("<Strings:TdGameUI.TdMessageBox.WillNotSave_Title>", "<Strings:TdGameUI.TdMessageBox.WillNotSave_Message>",
                              [this](int option) {
                                  if (option != 1) return;
                                  restore();
                                  close();
                              });
        }
    }

protected:
    void backup() {
        old_.clear();
        for (const Option& o : options_) old_.push_back(o.index);
        old_sliders_.clear();
        for (const UiWidget& w : scene_->widgets) {
            if (w.cls == "UISlider") old_sliders_.push_back(w.slider[2]);
        }
    }
    void restore();
    void commit();
    bool changed_ = false;
    std::vector<int> old_;
    std::vector<float> old_sliders_;
};

void OptionMenu::restore() {
    for (size_t i = 0; i < options_.size() && i < old_.size(); ++i) {
        options_[i].index = old_[i];
        if (options_[i].setting) options_[i].setting->value = options_[i].ids[static_cast<size_t>(old_[i])];
        show_option(options_[i]);
    }
    size_t k = 0;
    for (UiWidget& w : scene_->widgets) {
        if (w.cls != "UISlider" || k >= old_sliders_.size()) continue;
        w.slider[2] = old_sliders_[k++];
        std::string store, field;
        if (split_markup(w.markup, store, field) && field.rfind("ProfileData.", 0) == 0) {
            if (ProfileSetting* s = settings().find(field.substr(12))) s->value = static_cast<int>(std::lround(w.slider[2]));
        }
    }
}

void OptionMenu::commit() {
    // The string lists keep the index the option buttons were left at.
    for (const Option& o : options_) {
        if (o.setting || o.widget < 0) continue;
        std::string store, field;
        if (split_markup(scene_->widgets[static_cast<size_t>(o.widget)].markup, store, field) && store == "TdStringList") {
            string_list(field).index = o.index;
        }
    }
}

}  // namespace

std::unique_ptr<SubMenu> make_sub_menu(Frontend& fe, const std::string& scene) {
    std::unique_ptr<SubMenu> menu;
    if (scene == "TdDifficultySettings") menu = std::make_unique<DifficultyMenu>(fe);
    else if (scene == "TdLoadLevel") menu = std::make_unique<LoadLevelMenu>(fe);
    else if (scene == "TdGameSettings" || scene == "TdAudioSettings" || scene == "TdVideoSettingsPC") menu = std::make_unique<OptionMenu>(fe, scene);
    if (menu && !menu->valid()) menu.reset();
    return menu;
}

}  // namespace me::fe
