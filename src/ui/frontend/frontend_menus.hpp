#pragma once

// -----------------------------------------------------------------------------
// The scenes the main menu opens, as their UnrealScript runs them: TdUIScene_SubMenu,
// TdUIScene_OptionMenu and their subclasses, and TdUIScene_MessageBox. A SubMenu owns
// one loaded UiScene and handles its input; Frontend keeps the stack of open ones.
//
// docs/SUB_MENUS_RE.md describes each scene.
// -----------------------------------------------------------------------------

#include "ui_scene.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace me::fe {

class Frontend;
struct KeyBinding;
enum class Key : uint8_t;

// One option of a message box: its button, and the key that picks it.
struct MenuChoice {
    std::string markup;  // "<Strings:TdGameUI.TdButtonCallouts.Cancel>"
    Key key;
};

class SubMenu {
public:
    SubMenu(Frontend& fe, const std::string& package, const std::string& scene);
    virtual ~SubMenu();

    [[nodiscard]] bool valid() const { return scene_ != nullptr; }
    [[nodiscard]] UiScene* scene() const { return scene_.get(); }
    [[nodiscard]] std::string focused_name() const;

    virtual void opened();                // PostInitialize, SetupButtonBar and the first SceneActivated
    virtual void reactivated() {}         // the scene opened above this one has closed
    virtual void tick(float dt) { (void)dt; }
    virtual void key_pressed(Key key);    // IE_Pressed: navigation
    virtual void key_released(Key key);   // IE_Released: HandleInputKey
    // A key by its engine name ("SpaceBar", "LeftMouseButton"), before it is offered as a Key. True
    // if the scene took it. The space bar is no Enter on these scenes: it is taken and dropped.
    virtual bool raw_key(const std::string& name, bool released);
    virtual void mouse_click(float x, float y);  // viewport pixels
    // bRenderParentScenes: the scene under this one stays on screen, dimmed.
    [[nodiscard]] virtual bool draws_parent() const { return false; }
    // `top` is false for a scene drawn under another: dimmed, its button bar gone.
    virtual void draw(Frame& f, float scale, float origin_x, float gamma, bool top = true);

protected:
    // An option button's values.
    struct Option {
        int widget = -1;
        ProfileSetting* setting = nullptr;  // null: the scene supplies and keeps the value
        std::vector<int> ids;
        std::vector<std::string> labels;
        int index = 0;
    };

    // The front end's services.
    UiSystem& ui();
    Assets& assets();
    const struct Profile& profile() const;
    ProfileSettings& settings();
    struct StringList& string_list(const std::string& tag);
    std::vector<KeyBinding>& bindings();  // PlayerInput.Bindings

    // What the scripts call.
    void close();
    void close_then(const std::function<void()>& then);  // CloseScene, then a delegate
    void play(const char* cue);
    void level_event(const std::string& name);
    void open(std::unique_ptr<SubMenu> menu);
    void host_action(const std::string& action);
    void message_box(const std::string& title, const std::string& message, std::vector<MenuChoice> choices, std::function<void(int)> selected);
    // DisplayAcceptCancelBox: option 0 is Cancel (Escape), option 1 is OK (Enter).
    void accept_cancel_box(const std::string& title, const std::string& message, std::function<void(int)> selected);

    // TdUIButtonBar. `key` is the key that does what a click on the button does.
    int bar_append(const std::string& markup, Key key = Key{}, const std::string& bar = "ButtonBar");
    void bar_disable(int button, bool disable, const std::string& bar = "ButtonBar");

    // Focus.
    [[nodiscard]] bool focusable(int widget) const;
    void focus_first(int under = 0);
    void set_focus(int widget);
    void navigate(int face);  // UIFACE_Left = 0, UIFACE_Top = 1, UIFACE_Right = 2, UIFACE_Bottom = 3
    virtual void focus_changed() {}
    virtual void clicked(int widget) { (void)widget; }  // the mouse button came up on a UILabelButton

    // UITabControl.ActivatePage: that page shown, the others hidden.
    void activate_page(int control, int page);
    [[nodiscard]] int active_page(int control) const;
    virtual void page_activated(int control, int page) {
        (void)control;
        (void)page;
    }

    // Options and sliders.
    Option* option_of(int widget);
    Option* option_named(const std::string& widget);
    Option& add_option(const std::string& widget, std::vector<std::string> labels, int index);
    void bind_profile_widgets();          // every "<OnlinePlayerData:ProfileData.X>" option button and slider
    void show_option(Option& o);          // the value's text and the arrows' state
    void step_option(Option& o, int by);  // OnMoveSelectionLeft / Right
    void set_slider(int widget, float value);
    virtual void value_changed(int widget) { (void)widget; }

    Frontend& fe_;
    std::unique_ptr<UiScene> scene_;
    std::vector<Option> options_;
    int focus_ = -1;
    // The scene overrides RebuildNavigationLinks with nothing: focus only moves where a
    // ForcedNavigationTarget sends it.
    bool forced_navigation_only_ = false;
    bool bar_hidden_ = false;  // ButtonBar.SetVisibility(false)
};

// OpenScene(TdHUDContent.GetUISceneByName(name)): the SubMenu subclass for a scene.
std::unique_ptr<SubMenu> make_sub_menu(Frontend& fe, const std::string& scene);
// TdUIScene_MessageBox.Display. `selected` gets the index of the option picked, after the box has
// closed. With `seconds` > 0 the box closes by itself after that long and reports -1
// (DisplayModalBox with a pending Close).
std::unique_ptr<SubMenu> make_message_box(Frontend& fe, const std::string& title, const std::string& message, std::vector<MenuChoice> choices,
                                          std::function<void(int)> selected, float seconds = 0.0f);
// TdGameUISceneClient.OpenTinyMessageBox with DisplayModalBox: a line of text over the dimmed scene
// below, no buttons. Every key released goes to `key` by its engine name; when that returns true
// the box closes and `closed` runs.
std::unique_ptr<SubMenu> make_tiny_message_box(Frontend& fe, const std::string& message, std::function<bool(const std::string&)> key,
                                               std::function<void()> closed);
// TdUIScene_OnlineCheck: "NOT ONLINE", PLAY OFFLINE or BACK. `play_offline` runs after it has closed.
std::unique_ptr<SubMenu> make_online_check(Frontend& fe, std::function<void()> play_offline);

}  // namespace me::fe
