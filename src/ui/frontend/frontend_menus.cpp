#include "frontend_menus.hpp"

#include "frontend.hpp"

#include <algorithm>
#include <cctype>
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
std::vector<KeyBinding>& SubMenu::bindings() { return fe_.bindings_; }

void SubMenu::close() { fe_.close_scene(this, nullptr); }
void SubMenu::close_then(const std::function<void()>& then) { fe_.close_scene(this, then); }

void SubMenu::play(const char* cue) { fe_.sound(cue); }
void SubMenu::level_event(const std::string& name) { fe_.level_event(name); }
void SubMenu::open(std::unique_ptr<SubMenu> menu) { fe_.open_scene(std::move(menu)); }
void SubMenu::host_action(const std::string& action) { fe_.action_ = action; }

// What a scene that is not the top one is drawn with: measured on retail's CONTROLS screen under
// its "(Press Key To Bind)" box.
constexpr float kOverlaidSceneOpacity = 0.25f;

void SubMenu::draw(Frame& f, float scale, float origin_x, float gamma, bool top) {
    if (!scene_) return;
    // TdUIScene.TopSceneChanged: ButtonBar.ToggleAllButtons(false), until SceneActivated shows them again.
    for (auto& [widget, buttons] : scene_->button_bars) {
        for (UiScene::BarButton& b : buttons) b.hidden = !top || bar_hidden_;
    }
    scene_->draw(f, scale, origin_x, gamma, top ? 1.0f : kOverlaidSceneOpacity);
}

bool SubMenu::raw_key(const std::string& name, bool released) {
    (void)released;
    return name == "SpaceBar";
}

int SubMenu::bar_append(const std::string& markup, Key key, const std::string& bar) {
    std::vector<UiScene::BarButton>& buttons = scene_->bar(bar);
    UiScene::BarButton b;
    b.key = static_cast<uint8_t>(key);
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
        // The arrows of an option row are drawn in their owner's state. Its focus label is the
        // child the focus is passed down to, which takes the navigation links the scene builds:
        // CONTROLS builds none, and its two labels stay as they are.
        for (int child : w.children) {
            UiWidget& c = scene_->widgets[static_cast<size_t>(child)];
            if (c.cls == "UIButton" || (c.cls == "TdUIFocusLabel" && !forced_navigation_only_)) c.focused = on;
        }
    };
    if (widget == focus_) return;
    mark(focus_, false);
    focus_ = widget;
    mark(focus_, true);
    if (focus_ >= 0) focus_changed();
}

void SubMenu::navigate(int face) {
    if (focus_ < 0) {
        focus_first();
        return;
    }
    const UiWidget& from = scene_->widgets[static_cast<size_t>(focus_)];
    int target = from.forced_nav[face];
    if (target >= 0 && !focusable(target)) target = -1;
    if (target < 0 && !forced_navigation_only_) {
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
            set_slider(focus_, w.slider[2] + w.slider[3] * static_cast<float>(by));
        } else {
            navigate(key == Key::Left ? kFaceLeft : kFaceRight);
        }
    } else if (key == Key::PrevPage || key == Key::NextPage) {
        // The gamepad's shoulders: the tab control's previous or next page, wrapping.
        for (size_t i = 1; i < scene_->widgets.size(); ++i) {
            const int n = static_cast<int>(scene_->widgets[i].pages.size());
            if (n < 2 || !scene_->visible(static_cast<int>(i))) continue;
            const int now = std::max(active_page(static_cast<int>(i)), 0);
            play(key == Key::NextPage ? "TabChangeRight" : "TabChangeLeft");
            activate_page(static_cast<int>(i), (now + (key == Key::NextPage ? 1 : n - 1)) % n);
            break;
        }
    }
}

void SubMenu::set_slider(int widget, float value) {
    UiWidget& w = scene_->widgets[static_cast<size_t>(widget)];
    const float v = std::clamp(value, w.slider[0], w.slider[1]);
    if (v == w.slider[2]) return;
    const bool up = v > w.slider[2];
    w.slider[2] = v;
    std::string store, field;
    if (split_markup(w.markup, store, field) && field.rfind("ProfileData.", 0) == 0) {
        if (ProfileSetting* s = fe_.settings_.find(field.substr(12))) s->value = static_cast<int>(std::lround(v));
    }
    play(up ? "SliderIncrement" : "SliderDecrement");
    value_changed(widget);
}

void SubMenu::activate_page(int control, int page) {
    if (control < 0) return;
    const std::vector<int> pages = scene_->widgets[static_cast<size_t>(control)].pages;
    if (page < 0 || static_cast<size_t>(page) >= pages.size()) return;
    for (size_t k = 0; k < pages.size(); ++k) scene_->widgets[static_cast<size_t>(pages[k])].hidden = static_cast<int>(k) != page;
    if (focus_ >= 0 && !scene_->visible(focus_)) set_focus(-1);
    page_activated(control, page);
    scene_->layout();
}

int SubMenu::active_page(int control) const {
    if (control < 0) return -1;
    const std::vector<int>& pages = scene_->widgets[static_cast<size_t>(control)].pages;
    for (size_t k = 0; k < pages.size(); ++k) {
        if (!scene_->widgets[static_cast<size_t>(pages[k])].hidden) return static_cast<int>(k);
    }
    return -1;
}

// The left mouse button, as the widgets take it: a button of the bar, an option's arrow, a
// slider, a tab, anything that can have the focus.
void SubMenu::mouse_click(float x, float y) {
    if (!scene_) return;
    for (auto& [widget, buttons] : scene_->button_bars) {
        if (!scene_->visible(widget)) continue;
        for (const UiScene::BarButton& b : buttons) {
            if (b.hidden || b.disabled || b.key == 0 || x < b.rect.l || x >= b.rect.r || y < b.rect.t || y >= b.rect.b) continue;
            key_released(static_cast<Key>(b.key));
            return;
        }
    }
    const float sx = (x - fe_.origin_x_) / fe_.scale_, sy = y / fe_.scale_;
    int hit = -1;
    for (size_t i = 1; i < scene_->widgets.size(); ++i) {
        const UiWidget& w = scene_->widgets[i];
        if (sx < w.rect.l || sx >= w.rect.r || sy < w.rect.t || sy >= w.rect.b || !scene_->visible(static_cast<int>(i)) || w.disabled) continue;
        const bool arrow = w.cls == "UIButton" && w.parent >= 0 && option_of(w.parent);
        if (arrow || focusable(static_cast<int>(i)) || w.cls.find("TabButton") != std::string::npos) hit = static_cast<int>(i);
    }
    if (hit < 0) return;
    const UiWidget& w = scene_->widgets[static_cast<size_t>(hit)];
    if (w.cls == "UIButton") {
        set_focus(w.parent);
        step_option(*option_of(w.parent), w.name == "DecrementButton" ? -1 : 1);
    } else if (w.cls.find("TabButton") != std::string::npos) {
        // UITabControl.TabButtonClicked.
        const std::vector<int>& pages = scene_->widgets[static_cast<size_t>(w.parent)].pages;
        for (size_t k = 0; k < pages.size(); ++k) {
            if (scene_->widgets[static_cast<size_t>(pages[k])].tab_button != hit || active_page(w.parent) == static_cast<int>(k)) continue;
            play("TabChangeRight");
            activate_page(w.parent, static_cast<int>(k));
            break;
        }
    } else if (w.cls == "UISlider") {
        set_focus(hit);
        // The marker goes to the step nearest the cursor.
        const float range = w.slider[1] - w.slider[0];
        const float step = w.slider[3] > 0.0f ? w.slider[3] : 1.0f;
        const float t = std::clamp((sx - w.rect.l) / std::max(w.rect.w(), 1.0f), 0.0f, 1.0f);
        set_slider(hit, w.slider[0] + std::round(t * range / step) * step);
    } else {
        set_focus(hit);
        if (w.cls == "UILabelButton") clicked(hit);
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
        for (const MenuChoice& c : choices_) bar_append(c.markup, c.key);
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
        bar_append("Back", Key::Escape);
        bar_append("<Strings:TdGameUI.TdButtonCallouts.PlayOffline>", Key::Accept);
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

// TdTinyMessageBox shown with DisplayModalBox: no title, no buttons, the scene below still drawn.
class TinyMessageBoxMenu : public SubMenu {
public:
    TinyMessageBoxMenu(Frontend& fe, std::string message, std::function<bool(const std::string&)> key, std::function<void()> closed)
        : SubMenu(fe, "TdUI", "TdTinyMessageBox"), message_(std::move(message)), key_(std::move(key)), closed_(std::move(closed)) {}
    void opened() override {
        scene_->set_text(ui(), "MessageLabel", message_);
        scene_->set_visible("ButtonBar", false);
        scene_->layout();
    }
    [[nodiscard]] bool draws_parent() const override { return true; }
    void key_pressed(Key) override {}
    // A host that does not name its keys can still back out.
    void key_released(Key key) override {
        if (key == Key::Escape) raw_key("Escape", true);
    }
    bool raw_key(const std::string& name, bool released) override {
        if (released && key_ && key_(name)) close_then(closed_);
        return true;
    }
    void mouse_click(float, float) override {}

private:
    std::string message_;
    std::function<bool(const std::string&)> key_;
    std::function<void()> closed_;
};

}  // namespace

std::unique_ptr<SubMenu> make_tiny_message_box(Frontend& fe, const std::string& message, std::function<bool(const std::string&)> key,
                                               std::function<void()> closed) {
    return std::make_unique<TinyMessageBoxMenu>(fe, message, std::move(key), std::move(closed));
}

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
        bar_append("<Strings:TdGameUI.TdbuttonCallouts.Cancel>", Key::Escape);
        bar_append("<Strings:TdGameUI.TdbuttonCallouts.Accept>", Key::Accept);
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
        bar_append("Back", Key::Escape);
        bar_append("Accept", Key::Accept);
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
        bar_append("Back", Key::Escape);
        bar_append("Accept", Key::Accept);
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
        bar_append("<Strings:TdGameUI.TdbuttonCallouts.Cancel>", Key::Escape);
        bar_append("<Strings:TdGameUI.TdbuttonCallouts.SaveSettings>", Key::Accept);
        bar_append("<Strings:TdGameUI.TdbuttonCallouts.ResetToDefaults>", Key::Reset);
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
            if (!changed_ || !can_accept()) return;
            // OnAccept: the values are written to the profile and the scene closes.
            play("Accept");
            commit();
            host_action("ApplySettings");
            close();
        } else if (key == Key::Reset) {
            // OnReset: "Do you want to reset the settings to their defaults?"
            play("Accept");
            accept_cancel_box("<Strings:TdGameUI.TdMessageBox.ResetWarning_Title>", "<Strings:TdGameUI.TdMessageBox.ResetWarning_Message>",
                              [this](int option) {
                                  if (option == 1) reset_to_defaults();
                              });
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
    virtual bool can_accept() { return true; }
    virtual void commit();
    virtual void reset_to_defaults();  // DoReset
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

void OptionMenu::reset_to_defaults() {
    for (Option& o : options_) {
        if (o.widget < 0) continue;
        if (o.setting) {
            for (size_t k = 0; k < o.ids.size(); ++k) {
                if (o.ids[k] == o.setting->default_value) o.index = static_cast<int>(k);
            }
            o.setting->value = o.ids.empty() ? o.setting->default_value : o.ids[static_cast<size_t>(o.index)];
        } else {
            std::string store, field;
            if (!split_markup(scene_->widgets[static_cast<size_t>(o.widget)].markup, store, field) || store != "TdStringList") continue;
            for (const StringListData& list : assets().string_lists) {
                if (list.tag == field && !o.labels.empty()) o.index = std::clamp(list.default_index, 0, static_cast<int>(o.labels.size()) - 1);
            }
        }
        show_option(o);
    }
    for (UiWidget& w : scene_->widgets) {
        std::string store, field;
        if (w.cls != "UISlider" || !split_markup(w.markup, store, field) || field.rfind("ProfileData.", 0) != 0) continue;
        if (ProfileSetting* s = settings().find(field.substr(12))) {
            s->value = s->default_value;
            w.slider[2] = static_cast<float>(s->value);
        }
    }
    value_changed(-1);
    scene_->layout();
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

// --- TdUIScene_KeyMappings with its TdKeyBindingHandler (CONTROLS) ---------------------------

// Names compare as FNames do.
bool same_name(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

// IsControllerInput: the gamepad's keys are bound on GAMEPAD SETUP, not here.
bool controller_key(const std::string& key) { return key.size() > 10 && same_name(key.substr(0, 10), "XboxTypeS_"); }

// The alias a binding runs: "GBA_Jump | SkipCutscene" is a key for GBA_Jump.
std::string bound_alias(const std::string& command) {
    std::string alias = command.substr(0, command.find('|'));
    while (!alias.empty() && alias.back() == ' ') alias.pop_back();
    return alias;
}

class KeyMappingsMenu : public OptionMenu {
public:
    explicit KeyMappingsMenu(Frontend& fe) : OptionMenu(fe, "TdKeyMappings") { forced_navigation_only_ = true; }

    void opened() override {
        // InitWidgets: the handler works on its own copy of PlayerInput.Bindings.
        map_ = bindings();
        for (const KeyAction& a : assets().key_actions) {
            Row r;
            r.action = &a;
            r.label = scene_->find("KeyBindLabel_" + a.id);
            r.button[0] = scene_->find("KeyBindButton_" + a.id + "_0");
            r.button[1] = scene_->find("KeyBindButton_" + a.id + "_1");
            if (r.label < 0 || r.button[0] < 0) continue;
            scene_->widgets[static_cast<size_t>(r.label)].text = a.friendly;
            rows_.push_back(r);
        }
        refresh();
        OptionMenu::opened();
        // PostInitialize: OnTabPageActivated for the page the control starts on.
        control_ = scene_->find("KeyBindsTabControl");
        activate_page(control_, 0);
    }

    // OnTabPageActivated: the page's own frame, and the focus on its first key.
    void page_activated(int, int page) override {
        for (int k = 0; k < 2; ++k) {
            const std::string tab = "KeyBindsTab" + std::to_string(k);
            scene_->set_visible(tab + "BgImage", k == page);
            scene_->set_visible(tab + "BgTopImage", k == page);
        }
        const int first = scene_->find(page == 0 ? "KeyBindButton_MoveForward_0" : "KeyBindButton_Fire_0");
        if (first >= 0) set_focus(first);
    }

    void focus_changed() override {
        OptionMenu::focus_changed();
        // SetBindLabelState: an action's name is focused while either of its two buttons is.
        for (const Row& r : rows_) {
            scene_->widgets[static_cast<size_t>(r.label)].focused = focus_ == r.button[0] || (r.button[1] >= 0 && focus_ == r.button[1]);
        }
    }

    // OnBindButton_InputKey: the space bar or the left mouse button released on a key's button.
    bool raw_key(const std::string& name, bool released) override {
        if (released && name == "SpaceBar") begin_bind();
        return OptionMenu::raw_key(name, released);
    }
    void clicked(int) override { begin_bind(); }

protected:
    // OnAccept: every action needs a key before the bindings can be saved.
    bool can_accept() override {
        for (const Row& r : rows_) {
            if (!r.key[0].empty() || !r.key[1].empty()) continue;
            message_box("", "<Strings:TdGameUI.TdControllerSettings.UnboundActionText>", {{"<Strings:TdGameUI.TdButtonCallouts.OK>", Key::Accept}},
                        nullptr);
            return false;
        }
        return true;
    }
    void commit() override {
        bindings() = map_;  // StoreKeyBindings
        OptionMenu::commit();
    }
    void reset_to_defaults() override {
        map_ = assets().default_bindings;  // KeyBindingHandler.ResetToDefaults
        refresh();
        OptionMenu::reset_to_defaults();
    }

private:
    struct Row {
        const KeyAction* action = nullptr;
        int label = -1;
        int button[2] = {-1, -1};
        std::string key[2];  // BoundButtonKeys: the primary key and the second one
    };

    // RefreshBindingButtons: an action's primary key is the last binding of its alias in the
    // list, its second key the one before that.
    void refresh() {
        for (Row& r : rows_) {
            r.key[0].clear();
            r.key[1].clear();
            int found = 0;
            for (size_t i = map_.size(); i-- > 0 && found < 2;) {
                if (controller_key(map_[i].key) || !same_name(bound_alias(map_[i].command), r.action->command)) continue;
                r.key[found++] = map_[i].key;
            }
            for (int k = 0; k < 2; ++k) {
                if (r.button[k] >= 0) scene_->widgets[static_cast<size_t>(r.button[k])].text = assets().key_label(r.key[k]);
            }
        }
    }

    void begin_bind() {
        for (size_t r = 0; r < rows_.size(); ++r) {
            for (int k = 0; k < 2; ++k) {
                if (focus_ < 0 || rows_[r].button[k] != focus_) continue;
                bind_row_ = r;
                bind_slot_ = k;
                previous_ = rows_[r].key[k];
                pending_.clear();
                stomp_ = false;
                open(make_tiny_message_box(
                    fe_, "<Strings:TdGameUI.TdSettings.PressKeyToBind>", [this](const std::string& key) { return key_for_bind(key); },
                    [this] { prompt_closed(); }));
                return;
            }
        }
    }

    // TdKeyBindingHandler.HandleInputKey, for a key released while the prompt is up. True: the prompt closes.
    bool key_for_bind(const std::string& key) {
        if (same_name(key, "Escape")) return true;
        // A gamepad key is not for this screen, and IsAllowedBindingKey wants a key the profile knows.
        if (controller_key(key) || assets().key_label(key).empty()) return false;
        // AttemptKeyBind.
        if (same_name(key, previous_)) return true;
        pending_ = key;
        for (const Row& r : rows_) {
            if (same_name(r.key[0], key) || same_name(r.key[1], key)) stomp_ = true;  // IsAlreadyBound
        }
        if (!stomp_) bind();
        return true;
    }

    // OnFinisKeyBinding_MsgBoxClosed.
    void prompt_closed() {
        if (!stomp_) {
            refresh();
            return;
        }
        stomp_ = false;
        accept_cancel_box("<Strings:TdGameUI.TdMessageBox.StompBindKey_Title>", "<Strings:TdGameUI.TdMessageBox.StompBindKey_Message>",
                          [this](int option) {
                              if (option == 1) bind();
                              refresh();
                          });
    }

    // BindKey: the button's old key and the new key's old use go; the new binding becomes the
    // action's last (primary) or the one before its last (second key).
    void bind() {
        const std::string& command = rows_[bind_row_].action->command;
        unbind(previous_);
        unbind(pending_);
        size_t at = map_.size();
        if (bind_slot_ != 0) {
            for (size_t i = map_.size(); i-- > 0;) {
                if (controller_key(map_[i].key) || !same_name(bound_alias(map_[i].command), command)) continue;
                at = i;
                break;
            }
        }
        map_.insert(map_.begin() + static_cast<std::ptrdiff_t>(at), KeyBinding{pending_, command});
        pending_.clear();
        refresh();
        value_changed(-1);  // OnSettingsChangedDelegate: OnChange
    }

    void unbind(const std::string& key) {
        if (key.empty()) return;
        for (size_t i = 0; i < map_.size(); ++i) {
            if (!same_name(map_[i].key, key)) continue;
            map_.erase(map_.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }

    std::vector<KeyBinding> map_;  // CurrentBindingsMap
    std::vector<Row> rows_;
    int control_ = -1;
    size_t bind_row_ = 0;
    int bind_slot_ = 0;
    std::string previous_;  // CurrKeyBindData.PreviousBinding
    std::string pending_;   // CurrKeyBindData.KeyName
    bool stomp_ = false;    // bPromptForBindStomp
};

}  // namespace

std::unique_ptr<SubMenu> make_sub_menu(Frontend& fe, const std::string& scene) {
    std::unique_ptr<SubMenu> menu;
    if (scene == "TdDifficultySettings") menu = std::make_unique<DifficultyMenu>(fe);
    else if (scene == "TdLoadLevel") menu = std::make_unique<LoadLevelMenu>(fe);
    else if (scene == "TdGameSettings" || scene == "TdAudioSettings" || scene == "TdVideoSettingsPC") menu = std::make_unique<OptionMenu>(fe, scene);
    else if (scene == "TdKeyMappings") menu = std::make_unique<KeyMappingsMenu>(fe);
    if (menu && !menu->valid()) menu.reset();
    return menu;
}

}  // namespace me::fe
