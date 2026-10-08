#include "ui_scene.hpp"

#include "../../assets/package_manager.hpp"
#include "../../assets/ue3_props.hpp"
#include "../../assets/upk_loader.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <unordered_map>

namespace me::fe {

namespace {

constexpr float kSceneWidth = 1280.0f;
constexpr float kSceneHeight = 720.0f;

// EPositionEvalType.
enum : uint8_t { kPosNone, kPosPixelViewport, kPosPixelScene, kPosPixelOwner, kPosPercentViewport, kPosPercentOwner, kPosPercentScene };
// EUIDockPaddingEvalType.
enum : uint8_t { kPadPixels, kPadPercentTarget, kPadPercentOwner, kPadPercentScene, kPadPercentViewport };

uint8_t pos_type(const std::string& s) {
    if (s == "EVALPOS_PixelViewport") return kPosPixelViewport;
    if (s == "EVALPOS_PixelScene") return kPosPixelScene;
    if (s == "EVALPOS_PixelOwner") return kPosPixelOwner;
    if (s == "EVALPOS_PercentageViewport") return kPosPercentViewport;
    if (s == "EVALPOS_PercentageScene") return kPosPercentScene;
    if (s == "EVALPOS_None") return kPosNone;
    return kPosPercentOwner;
}

uint8_t pad_type(const std::string& s) {
    if (s == "UIPADDINGEVAL_PercentTarget") return kPadPercentTarget;
    if (s == "UIPADDINGEVAL_PercentOwner") return kPadPercentOwner;
    if (s == "UIPADDINGEVAL_PercentScene") return kPadPercentScene;
    if (s == "UIPADDINGEVAL_PercentViewport") return kPadPercentViewport;
    return kPadPixels;
}

uint8_t face_of(const std::string& s) {
    if (s == "UIFACE_Left") return 0;
    if (s == "UIFACE_Top") return 1;
    if (s == "UIFACE_Right") return 2;
    if (s == "UIFACE_Bottom") return 3;
    return 4;
}

uint8_t align_of(const std::string& s, uint8_t fallback) {
    if (s == "UIALIGN_Left") return kAlignLeft;
    if (s == "UIALIGN_Center") return kAlignCenter;
    if (s == "UIALIGN_Right") return kAlignRight;
    if (s == "UIALIGN_Default") return kAlignDefault;
    return fallback;
}

uint8_t adjust_of(const std::string& s) {
    if (s == "ADJUST_None") return kAdjustNone;
    if (s == "ADJUST_Justified") return kAdjustJustified;
    if (s == "ADJUST_Bound") return kAdjustBound;
    if (s == "ADJUST_Stretch") return kAdjustStretch;
    return kAdjustNormal;
}

int state_of(const std::string& default_object_name) {
    static const char* const kNames[kUiStates] = {"UIState_Enabled", "UIState_Disabled", "UIState_Focused", "UIState_Active",
                                                  "UIState_Pressed", "TdUIState_FakeActive", "UIState_TargetedTab"};
    for (size_t i = 0; i < kUiStates; ++i) {
        if (default_object_name == std::string("Default__") + kNames[i] || default_object_name == kNames[i]) return static_cast<int>(i);
    }
    return -1;
}

float face(const Rect& r, int f) { return f == 0 ? r.l : (f == 1 ? r.t : (f == 2 ? r.r : r.b)); }

struct ObjRef {
    std::shared_ptr<UPKPackage> pkg;
    int32_t index = 0;
    [[nodiscard]] bool ok() const { return pkg && index > 0; }
};

// An object's tagged properties with its archetype chain behind them: what the object's
// properties are once the engine has filled in everything the package left out.
struct View {
    std::vector<const UPropertyList*> lists;  // the object's own first
    std::vector<ObjRef> owners;               // whose list it is: object references resolve against it

    const UProperty* find(const std::string& name, int index = 0, ObjRef* owner = nullptr) const {
        for (size_t i = 0; i < lists.size(); ++i) {
            if (const UProperty* p = find_prop(*lists[i], name, index)) {
                if (owner) *owner = owners[i];
                return p;
            }
        }
        return nullptr;
    }
    // A struct's member. A struct is saved as its difference from the archetype's, so a member
    // the object's struct lacks is looked for further down the chain.
    const UProperty* field(const std::string& prop, const std::string& member, int member_index = 0, ObjRef* owner = nullptr,
                           int prop_index = 0) const {
        for (size_t i = 0; i < lists.size(); ++i) {
            const UProperty* p = find_prop(*lists[i], prop, prop_index);
            if (!p) continue;
            if (const UProperty* m = find_prop(p->fields, member, member_index)) {
                if (owner) *owner = owners[i];
                return m;
            }
        }
        return nullptr;
    }
    // A member of a struct inside a struct (DockTargets.DockPadding.PaddingValue[k]).
    const UProperty* field2(const std::string& prop, const std::string& inner, const std::string& member, int member_index = 0) const {
        for (const UPropertyList* list : lists) {
            const UProperty* p = find_prop(*list, prop);
            if (!p) continue;
            const UProperty* q = find_prop(p->fields, inner);
            if (!q) continue;
            if (const UProperty* m = find_prop(q->fields, member, member_index)) return m;
        }
        return nullptr;
    }
    [[nodiscard]] bool empty() const { return lists.empty(); }
};

bool is_true(const UProperty* p, bool fallback) { return p ? p->b : fallback; }

}  // namespace

// --- objects and styles ---------------------------------------------------------------------

struct UiSystem::Impl {
    Assets* assets = nullptr;
    PackageManager* pm = nullptr;

    struct Parsed {
        UPropertyList props;
        size_t tail = 0;
    };
    std::unordered_map<const UPKPackage*, std::unordered_map<int32_t, std::unique_ptr<Parsed>>> parsed;

    struct StyleSource {
        ObjRef object;
        std::string tag;
        std::unique_ptr<UiStyle> resolved;
        bool resolving = false;
    };
    std::vector<StyleSource> styles;
    std::unordered_map<std::string, int> style_by_tag;
    std::map<std::array<int32_t, 4>, int> style_by_id;
    bool skin_loaded = false;

    const Parsed& parse(const ObjRef& o) {
        auto& per_package = parsed[o.pkg.get()];
        auto it = per_package.find(o.index);
        if (it != per_package.end()) return *it->second;
        auto p = std::make_unique<Parsed>();
        p->tail = parse_export_properties(*o.pkg, o.index, p->props);
        return *per_package.emplace(o.index, std::move(p)).first->second;
    }

    ObjRef resolve(const ObjRef& from, int32_t ref) {
        if (!from.pkg || ref == 0) return {};
        if (ref > 0) return ObjRef{from.pkg, ref};
        const std::string path = to_lower(object_canonical_path(*from.pkg, ref));
        const size_t dot = path.find('.');
        if (dot == std::string::npos) return {};
        std::shared_ptr<UPKPackage> pkg = pm->load(path.substr(0, dot));
        if (!pkg) return {};
        const int32_t index = pm->find_export(*pkg, path);
        return index > 0 ? ObjRef{pkg, index} : ObjRef{};
    }

    std::string name_of(const ObjRef& from, int32_t ref) const {
        if (!from.pkg || ref == 0) return {};
        if (ref > 0) return export_object_name(*from.pkg, ref);
        const auto& imports = from.pkg->get_imports();
        const size_t ii = static_cast<size_t>(-ref - 1);
        return ii < imports.size() ? imports[ii].object_name : std::string();
    }

    // The archetype, or for an object without one its class default object.
    ObjRef archetype_of(const ObjRef& o) {
        const FObjectExport& e = o.pkg->get_exports()[static_cast<size_t>(o.index - 1)];
        if (e.archetype != 0) return resolve(o, e.archetype);
        if (e.class_index == 0) return {};
        const std::string cls = to_lower(object_canonical_path(*o.pkg, e.class_index));
        const size_t dot = cls.rfind('.');
        if (dot == std::string::npos) return {};
        std::shared_ptr<UPKPackage> pkg = pm->load(cls.substr(0, cls.find('.')));
        if (!pkg) return {};
        const int32_t index = pm->find_export(*pkg, cls.substr(0, dot + 1) + "default__" + cls.substr(dot + 1));
        if (index <= 0 || (pkg == o.pkg && index == o.index)) return {};
        return ObjRef{pkg, index};
    }

    View view(ObjRef o) {
        View v;
        for (int guard = 0; guard < 16 && o.ok(); ++guard) {
            v.lists.push_back(&parse(o).props);
            v.owners.push_back(o);
            o = archetype_of(o);
        }
        return v;
    }

    // --- the skin ---

    void load_skin() {
        if (skin_loaded) return;
        skin_loaded = true;
        // UI_Skins.UI_Skins_TDUISkins2 over the engine's DefaultUISkin: a style of the first replaces
        // the one with the same ID or tag in the second.
        for (const char* package : {"DefaultUISkin", "UI_Skins"}) {
            std::shared_ptr<UPKPackage> pkg = pm->load(package);
            if (!pkg) {
                assets->warnings.push_back(std::string("UI skin package ") + package + " not found");
                continue;
            }
            const auto& exports = pkg->get_exports();
            for (size_t i = 0; i < exports.size(); ++i) {
                if (pkg->get_export_class(exports[i]) != "UIStyle") continue;
                const ObjRef o{pkg, static_cast<int32_t>(i + 1)};
                const View v = view(o);
                StyleSource src;
                src.object = o;
                const UProperty* tag = v.find("StyleTag");
                src.tag = tag ? tag->s : export_object_name(*pkg, o.index);
                const int index = static_cast<int>(styles.size());
                std::array<int32_t, 4> id{0, 0, 0, 0};
                static const char* const kParts[4] = {"A", "B", "C", "D"};
                for (int k = 0; k < 4; ++k) {
                    if (const UProperty* part = v.field("StyleID", kParts[k])) id[static_cast<size_t>(k)] = part->i;
                }
                styles.push_back(std::move(src));
                style_by_tag[to_lower(styles.back().tag)] = index;
                if (id[0] || id[1] || id[2] || id[3]) style_by_id[id] = index;
            }
        }
    }

    // UUIStyle's native tail: TMap<UIState class default, UIStyle_Data>.
    void state_map(const ObjRef& style, std::array<ObjRef, kUiStates>& out) {
        const Parsed& p = parse(style);
        const auto& data = style.pkg->get_data();
        const FObjectExport& e = style.pkg->get_exports()[static_cast<size_t>(style.index - 1)];
        const size_t end = static_cast<size_t>(e.serial_offset) + static_cast<size_t>(e.serial_size);
        if (p.tail + 4 > end) return;
        int32_t count = 0;
        std::memcpy(&count, data.data() + p.tail, 4);
        if (count < 0 || count > 64 || p.tail + 4 + static_cast<size_t>(count) * 8 > end) return;
        for (int32_t k = 0; k < count; ++k) {
            int32_t key = 0, value = 0;
            std::memcpy(&key, data.data() + p.tail + 4 + static_cast<size_t>(k) * 8, 4);
            std::memcpy(&value, data.data() + p.tail + 8 + static_cast<size_t>(k) * 8, 4);
            const int s = state_of(name_of(style, key));
            if (s < 0) continue;
            const ObjRef d = resolve(style, value);
            if (d.ok()) out[static_cast<size_t>(s)] = d;
        }
    }

    void fill_text(const View& v, UiTextStyle& out) {
        ObjRef owner;
        if (const UProperty* font = v.find("StyleFont", 0, &owner)) {
            const std::string name = name_of(owner, font->i);
            if (!name.empty()) out.font = assets->font(name);
        }
        if (const UProperty* color = v.find("StyleColor")) std::copy(color->v, color->v + 4, out.color);
        for (int k = 0; k < 2; ++k) {
            if (const UProperty* a = v.find("Alignment", k)) out.align[k] = align_of(a->s, out.align[k]);
        }
        if (const UProperty* clip = v.find("ClipMode")) out.wrap = clip->s == "CLIP_Wrap";
        out.valid = true;
    }

    void fill_image(const View& v, UiImageStyle& out) {
        ObjRef owner;
        if (const UProperty* image = v.find("DefaultImage", 0, &owner)) {
            if (image->i != 0) {
                const std::string path = object_canonical_path(*owner.pkg, image->i);
                if (!path.empty() && to_lower(path) != "engineresources.defaulttexture") out.image = assets->image(path);
            }
        }
        if (const UProperty* color = v.find("StyleColor")) std::copy(color->v, color->v + 4, out.color);
        for (int k = 0; k < 2; ++k) {
            if (const UProperty* a = v.field("AdjustmentType", "AdjustmentType", 0, nullptr, k)) out.adjust[k] = adjust_of(a->s);
            if (const UProperty* a = v.field("AdjustmentType", "Alignment", 0, nullptr, k)) out.align[k] = align_of(a->s, out.align[k]);
        }
        static const char* const kCoord[4] = {"U", "V", "UL", "VL"};
        for (int k = 0; k < 4; ++k) {
            if (const UProperty* c = v.field("Coordinates", kCoord[k])) out.uv[k] = c->f;
        }
        for (int k = 0; k < 2; ++k) {
            if (const UProperty* pad = v.find("StylePadding", k)) out.padding[k] = pad->f;
        }
        out.valid = true;
    }

    const UiStyle* resolve_style(int index) {
        if (index < 0 || static_cast<size_t>(index) >= styles.size()) return nullptr;
        StyleSource& src = styles[static_cast<size_t>(index)];
        if (src.resolved || src.resolving) return src.resolved.get();
        src.resolving = true;
        auto style = std::make_unique<UiStyle>();
        style->tag = src.tag;

        // The style's own states over its archetype style's.
        std::array<ObjRef, kUiStates> states{};
        std::vector<ObjRef> chain;
        for (ObjRef o = src.object; o.ok() && chain.size() < 8; o = resolve(o, o.pkg->get_exports()[static_cast<size_t>(o.index - 1)].archetype)) {
            chain.push_back(o);
        }
        for (size_t k = chain.size(); k-- > 0;) state_map(chain[k], states);

        for (size_t s = 0; s < kUiStates; ++s) {
            if (!states[s].ok()) continue;
            const std::string cls = object_class_name(*states[s].pkg, states[s].index);
            const View v = view(states[s]);
            if (cls == "UIStyle_Text") {
                fill_text(v, style->text[s]);
            } else if (cls == "UIStyle_Image") {
                fill_image(v, style->image[s]);
            } else if (cls == "UIStyle_Combo") {
                combo_part(v, "TextStyle", true, static_cast<int>(s), *style);
                combo_part(v, "ImageStyle", false, static_cast<int>(s), *style);
            }
        }
        styles[static_cast<size_t>(index)].resolved = std::move(style);
        styles[static_cast<size_t>(index)].resolving = false;
        return styles[static_cast<size_t>(index)].resolved.get();
    }

    // FUIStyleDataReference::GetStyleData: the custom data if it is enabled, else the named
    // state of the source style.
    void combo_part(const View& v, const char* prop, bool text, int state, UiStyle& out) {
        ObjRef owner;
        if (const UProperty* custom = v.field(prop, "CustomStyleData", 0, &owner)) {
            const ObjRef c = resolve(owner, custom->i);
            if (c.ok()) {
                const View cv = view(c);
                if (is_true(cv.find("bEnabled"), true)) {
                    if (text) fill_text(cv, out.text[static_cast<size_t>(state)]);
                    else fill_image(cv, out.image[static_cast<size_t>(state)]);
                    return;
                }
            }
        }
        std::array<int32_t, 4> id{0, 0, 0, 0};
        static const char* const kParts[4] = {"A", "B", "C", "D"};
        for (int k = 0; k < 4; ++k) {
            for (const UPropertyList* list : v.lists) {
                const UProperty* p = find_prop(*list, prop);
                const UProperty* sid = p ? find_prop(p->fields, "SourceStyleID") : nullptr;
                const UProperty* part = sid ? find_prop(sid->fields, kParts[k]) : nullptr;
                if (part) {
                    id[static_cast<size_t>(k)] = part->i;
                    break;
                }
            }
        }
        auto it = style_by_id.find(id);
        if (it == style_by_id.end()) return;
        int source_state = 0;
        if (const UProperty* st = v.field(prop, "SourceState", 0, &owner)) {
            const int s = state_of(name_of(owner, st->i));
            if (s >= 0) source_state = s;
        }
        const UiStyle* source = resolve_style(it->second);
        if (!source) return;
        if (text) out.text[static_cast<size_t>(state)] = source->text_for(static_cast<UiState>(source_state));
        else out.image[static_cast<size_t>(state)] = source->image_for(static_cast<UiState>(source_state));
    }

    // A UIStyleReference property of an object.
    const UiStyle* style_ref(const View& v, const char* prop) {
        load_skin();
        std::array<int32_t, 4> id{0, 0, 0, 0};
        static const char* const kParts[4] = {"A", "B", "C", "D"};
        for (int k = 0; k < 4; ++k) {
            if (const UProperty* part = v.field2(prop, "AssignedStyleID", kParts[k])) id[static_cast<size_t>(k)] = part->i;
        }
        if (id[0] || id[1] || id[2] || id[3]) {
            auto it = style_by_id.find(id);
            if (it != style_by_id.end()) return resolve_style(it->second);
        }
        if (const UProperty* tag = v.field(prop, "DefaultStyleTag")) {
            auto it = style_by_tag.find(to_lower(tag->s));
            if (it != style_by_tag.end()) return resolve_style(it->second);
        }
        return nullptr;
    }

    void read_string_comp(const ObjRef& comp, UiStringComp& out) {
        if (!comp.ok()) return;
        const View v = view(comp);
        out.present = true;
        out.style = style_ref(v, "StringStyle");
        if (object_class_name(*comp.pkg, comp.index) == "UIComp_TdDropShadowString") {
            out.shadow = style_ref(v, "DropShadowStyle");
            if (const UProperty* p = v.find("HorizontalPctOffset")) out.shadow_h = p->f;
            if (const UProperty* p = v.find("VerticalPctOffset")) out.shadow_v = p->f;
        }
        if (is_true(v.field("TextStyleCustomization", "bOverrideAlignment"), false)) {
            for (int k = 0; k < 2; ++k) {
                if (const UProperty* a = v.field("TextStyleCustomization", "TextAlignment", k)) out.align[k] = static_cast<int8_t>(align_of(a->s, kAlignLeft));
            }
        }
        for (int k = 0; k < 2; ++k) out.autosize[k] = is_true(v.field("AutoSizeParameters", "bAutoSizeEnabled", 0, nullptr, k), false);
    }

    void read_image_comp(const ObjRef& comp, UiImageComp& out) {
        if (!comp.ok()) return;
        const View v = view(comp);
        out.present = true;
        out.style = style_ref(v, "ImageStyle");
        if (const UProperty* tag = v.find("StyleResolverTag")) out.resolver = tag->s;
        ObjRef owner;
        if (const UProperty* ref = v.find("ImageRef", 0, &owner)) {
            const ObjRef texture = resolve(owner, ref->i);
            if (texture.ok()) {
                const View tv = view(texture);
                ObjRef towner;
                if (const UProperty* image = tv.find("ImageTexture", 0, &towner)) {
                    if (image->i != 0) out.texture = assets->image(object_canonical_path(*towner.pkg, image->i));
                }
            }
        }
        if (is_true(v.field("StyleCustomization", "bOverrideOpacity"), false)) {
            if (const UProperty* p = v.field("StyleCustomization", "Opacity")) out.opacity = p->f;
        }
    }
};

UiSystem::UiSystem() : impl_(std::make_unique<Impl>()) {}
UiSystem::~UiSystem() = default;

void UiSystem::init(Assets* assets, float view_scale) {
    assets_ = assets;
    view_scale_ = view_scale;
    impl_->assets = assets;
    impl_->pm = assets ? assets->packages() : nullptr;
}

const UiStyle* UiSystem::style_by_tag(const std::string& tag) {
    if (!impl_->pm) return nullptr;
    impl_->load_skin();
    auto it = impl_->style_by_tag.find(to_lower(tag));
    return it == impl_->style_by_tag.end() ? nullptr : impl_->resolve_style(it->second);
}

bool UiSystem::load_profile_settings(ProfileSettings& out) {
    out.settings.clear();
    if (!impl_->pm) return false;
    std::shared_ptr<UPKPackage> pkg = impl_->pm->load("TdGame");
    if (!pkg) return false;
    const int32_t index = impl_->pm->find_export(*pkg, "tdgame.default__tdprofilesettings");
    if (index <= 0) return false;
    const UPropertyList& props = impl_->parse(ObjRef{pkg, index}).props;
    if (const UProperty* mappings = find_prop(props, "ProfileMappings")) {
        for (const auto& el : mappings->elements) {
            ProfileSetting s;
            s.id = prop_int(el, "Id", 0);
            s.name = prop_name(el, "Name");
            s.id_mapped = prop_name(el, "MappingType") == "PVMT_IdMapped";
            if (const UProperty* values = find_prop(el, "ValueMappings")) {
                for (const auto& v : values->elements) {
                    s.value_ids.push_back(prop_int(v, "Id", 0));
                    s.value_names.push_back(prop_name(v, "Name"));
                }
            }
            out.settings.push_back(std::move(s));
        }
    }
    if (const UProperty* defaults = find_prop(props, "DefaultSettings")) {
        for (const auto& el : defaults->elements) {
            const UProperty* setting = find_prop(el, "ProfileSetting");
            if (!setting) continue;
            const int id = prop_int(setting->fields, "PropertyId", 0);
            const UProperty* data = find_prop(setting->fields, "Data");
            const int value = data ? prop_int(data->fields, "Value1", 0) : 0;
            for (ProfileSetting& s : out.settings) {
                if (s.id == id) s.value = s.default_value = value;
            }
        }
    }
    return !out.settings.empty();
}

std::string UiSystem::resolve_markup(const std::string& markup) const {
    std::string out;
    for (size_t i = 0; i < markup.size();) {
        if (markup[i] == '<') {
            const size_t close = markup.find('>', i);
            if (close != std::string::npos) {
                const std::string tag = markup.substr(i + 1, close - i - 1);
                const size_t colon = tag.find(':');
                if (colon != std::string::npos && tag.compare(0, colon, "Strings") == 0 && assets_) {
                    // A localized string may itself carry tags (a gamepad glyph alias in front of "QUIT GAME").
                    const std::string value = assets_->localized(tag.substr(colon + 1));
                    out += value.find('<') != std::string::npos ? resolve_markup(value) : value;
                }
                i = close + 1;
                continue;
            }
        }
        if (markup[i] == '\\' && i + 1 < markup.size() && markup[i + 1] == 'n') {
            out.push_back('\n');
            i += 2;
            continue;
        }
        out.push_back(markup[i++]);
    }
    return out;
}

std::unique_ptr<UiScene> UiSystem::load_scene(const std::string& package, const std::string& scene_name) {
    if (!impl_->pm) return nullptr;
    std::shared_ptr<UPKPackage> pkg = impl_->pm->load(package);
    if (!pkg) return nullptr;
    const auto& exports = pkg->get_exports();
    int32_t root = 0;
    const std::string want = to_lower(scene_name);
    for (size_t i = 0; i < exports.size(); ++i) {
        if (to_lower(export_object_name(*pkg, static_cast<int32_t>(i + 1))) != want) continue;
        if (pkg->get_export_class(exports[i]).find("UIScene") == std::string::npos) continue;
        root = static_cast<int32_t>(i + 1);
        break;
    }
    if (root <= 0) return nullptr;

    auto scene = std::make_unique<UiScene>();
    scene->name = scene_name;
    scene->cls = pkg->get_export_class(exports[static_cast<size_t>(root - 1)]);
    scene->view_scale = view_scale_;

    std::unordered_map<int32_t, int> widget_of;
    std::vector<int32_t> export_of;
    // Depth first, in Children order.
    std::vector<std::pair<int32_t, int>> stack{{root, -1}};
    while (!stack.empty()) {
        const auto [e, parent] = stack.back();
        stack.pop_back();
        if (widget_of.count(e)) continue;
        UiWidget w;
        w.name = export_object_name(*pkg, e);
        w.cls = pkg->get_export_class(exports[static_cast<size_t>(e - 1)]);
        w.parent = parent;
        const int index = static_cast<int>(scene->widgets.size());
        widget_of[e] = index;
        export_of.push_back(e);
        scene->widgets.push_back(std::move(w));
        if (parent >= 0) scene->widgets[static_cast<size_t>(parent)].children.push_back(index);
        const View v = impl_->view(ObjRef{pkg, e});
        if (const UProperty* children = v.find("Children")) {
            // Pushed in reverse so the first child is loaded first and the indices follow tree order.
            for (size_t k = children->ints.size(); k-- > 0;) {
                if (children->ints[k] > 0) stack.push_back({children->ints[k], index});
            }
        }
    }
    // A parent's children were appended as they were popped, which is Children order.

    for (size_t wi = 0; wi < scene->widgets.size(); ++wi) {
        UiWidget& w = scene->widgets[wi];
        const ObjRef self{pkg, export_of[wi]};
        const View v = impl_->view(self);

        for (int k = 0; k < 4; ++k) {
            if (const UProperty* p = v.field("Position", "Value", k)) w.pos[k] = p->f;
            const UProperty* t = v.field("Position", "ScaleType", k);
            w.pos_type[k] = t ? pos_type(t->s) : kPosPercentOwner;
            const UProperty* tf = v.field("DockTargets", "TargetFace", k);
            w.dock_face[k] = tf ? face_of(tf->s) : 4;
            if (w.dock_face[k] < 4) {
                const UProperty* tw = v.field("DockTargets", "TargetWidget", k);
                auto it = tw ? widget_of.find(tw->i) : widget_of.end();
                w.dock_widget[k] = it == widget_of.end() ? -2 : it->second;
                if (const UProperty* pv = v.field2("DockTargets", "DockPadding", "PaddingValue", k)) w.dock_pad[k] = pv->f;
                const UProperty* pt = v.field2("DockTargets", "DockPadding", "PaddingScaleType", k);
                w.dock_pad_type[k] = pt ? pad_type(pt->s) : kPadPixels;
            }
            if (const UProperty* nav = v.field("NavigationTargets", "ForcedNavigationTarget", k)) {
                auto it = widget_of.find(nav->i);
                if (it != widget_of.end()) w.forced_nav[k] = it->second;
            }
        }
        w.hidden = is_true(v.find("bHidden"), false);
        if (const UProperty* p = v.find("Opacity")) w.opacity = p->f;
        if (const UProperty* p = v.find("ZDepth")) w.zdepth = p->f;
        if (const UProperty* p = v.find("TabIndex")) w.tab_index = p->i;
        if (const UProperty* p = v.field("DataSource", "MarkupString")) w.markup = p->s;
        if (w.markup.empty()) {
            if (const UProperty* p = v.field("ImageDataSource", "MarkupString")) w.markup = p->s;
        }
        w.text = resolve_markup(w.markup);

        ObjRef owner;
        if (const UProperty* p = v.find("StringRenderComponent", 0, &owner)) impl_->read_string_comp(impl_->resolve(owner, p->i), w.string);
        if (const UProperty* p = v.find("BackgroundImageComponent", 0, &owner)) impl_->read_image_comp(impl_->resolve(owner, p->i), w.image);
        if (!w.image.present) {
            if (const UProperty* p = v.find("ImageComponent", 0, &owner)) impl_->read_image_comp(impl_->resolve(owner, p->i), w.image);
        }
        if (w.cls == "UISlider") {
            if (const UProperty* p = v.find("SliderBarImageComponent", 0, &owner)) impl_->read_image_comp(impl_->resolve(owner, p->i), w.bar);
            if (const UProperty* p = v.find("MarkerImageComponent", 0, &owner)) impl_->read_image_comp(impl_->resolve(owner, p->i), w.marker);
            if (const UProperty* p = v.field("SliderValue", "MinValue")) w.slider[0] = p->f;
            if (const UProperty* p = v.field("SliderValue", "MaxValue")) w.slider[1] = p->f;
            if (const UProperty* p = v.field("SliderValue", "CurrentValue")) w.slider[2] = p->f;
            if (const UProperty* p = v.field("SliderValue", "NudgeValue")) w.slider[3] = p->f;
            if (const UProperty* p = v.field("BarSize", "Value")) w.bar_size = p->f;
            if (const UProperty* p = v.field("MarkerWidth", "Value")) w.marker_width = p->f;
            if (const UProperty* p = v.field("MarkerHeight", "Value")) w.marker_height = p->f;
        }
        if (w.cls == "UITdOptionButton") {
            w.increment = impl_->style_ref(v, "IncrementStyle");
            w.decrement = impl_->style_ref(v, "DecrementStyle");
        }
    }

    // What every TdUIButtonBar button looks like, from the first bar's first template.
    for (const UiWidget& w : scene->widgets) {
        if (w.cls != "TdUIButtonBarButton") continue;
        if (w.string.style) {
            const UiTextStyle& t = w.string.style->text_for(UiState::Enabled);
            scene->bar_font = t.font;
            std::copy(t.color, t.color + 4, scene->bar_text);
        }
        if (w.string.shadow) {
            const UiTextStyle& t = w.string.shadow->text_for(UiState::Enabled);
            std::copy(t.color, t.color + 4, scene->bar_shadow);
        }
        if (w.image.style) scene->bar_image = w.image.style->image_for(UiState::Enabled).image;
        break;
    }
    // Every bar draws TdImageButtonBarBackground, whatever style its templates were saved with.
    if (const UiStyle* style = style_by_tag("TdImageButtonBarBackground")) {
        const UiImageStyle& is = style->image_for(UiState::Enabled);
        scene->bar_image = is.image;
        scene->bar_padding[0] = -is.padding[0];
        scene->bar_padding[1] = -is.padding[1];
    }
    for (size_t wi = 0; wi < scene->widgets.size(); ++wi) {
        if (scene->widgets[wi].cls == "TdUIButtonBar") scene->button_bars.emplace_back(static_cast<int>(wi), std::vector<UiScene::BarButton>{});
    }
    scene->layout();
    return scene;
}

// --- text ------------------------------------------------------------------------------------

std::vector<std::string> ui_wrap(const Font& font, const std::string& text, float width, bool wrap) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t nl = text.find('\n', start);
        const std::string para = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (!wrap || width <= 0.0f) {
            lines.push_back(para);
        } else {
            std::string line;
            size_t pos = 0;
            while (pos <= para.size()) {
                const size_t sp = para.find(' ', pos);
                const std::string word = para.substr(pos, sp == std::string::npos ? std::string::npos : sp - pos);
                const std::string trial = line.empty() ? word : line + " " + word;
                if (!line.empty() && font.width(trial) > width) {
                    // The space the line was broken at stays on it: a right-aligned line ends one
                    // space short of the edge (measured on retail frames).
                    lines.push_back(line + " ");
                    line = word;
                } else {
                    line = trial;
                }
                if (sp == std::string::npos) break;
                pos = sp + 1;
            }
            lines.push_back(line);
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return lines;
}

namespace {

void encode_color(const float linear[4], float gamma, float out[4]) {
    for (int k = 0; k < 3; ++k) out[k] = canvas_encode(linear[k], gamma);
    out[3] = linear[3];
}

void draw_line(Frame& f, const Font& font, const std::string& text, float x, float y, const float color[4], const Rect* clip) {
    if (text.empty() || color[3] <= 0.0f) return;
    const size_t first_op = f.ui.size();
    float pen = x;
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        const Glyph& g = font.glyphs[c];
        if (g.w > 0 && g.h > 0 && g.page >= 0 && static_cast<size_t>(g.page) < font.pages.size() &&
            font.pages[static_cast<size_t>(g.page)].valid()) {
            const Image* page = &font.pages[static_cast<size_t>(g.page)];
            DrawOp* op = nullptr;
            for (size_t k = first_op; k < f.ui.size(); ++k) {
                if (f.ui[k].image == page) op = &f.ui[k];
            }
            if (!op) {
                f.ui.emplace_back();
                op = &f.ui.back();
                op->kind = DrawOp::Kind::Glyphs;
                op->image = page;
                std::copy(color, color + 4, op->color);
                if (clip) {
                    op->clipped = true;
                    op->clip = *clip;
                }
            }
            Quad q;
            q.x0 = pen;
            q.y0 = y + static_cast<float>(g.voff) * font.scale;
            q.x1 = q.x0 + static_cast<float>(g.w) * font.scale;
            q.y1 = q.y0 + static_cast<float>(g.h) * font.scale;
            q.u0 = static_cast<float>(g.u) / static_cast<float>(page->w);
            q.v0 = static_cast<float>(g.v) / static_cast<float>(page->h);
            q.u1 = static_cast<float>(g.u + g.w) / static_cast<float>(page->w);
            q.v1 = static_cast<float>(g.v + g.h) / static_cast<float>(page->h);
            op->quads.push_back(q);
        }
        pen += font.advance(c, i + 1 < text.size() ? static_cast<unsigned char>(text[i + 1]) : 0);
    }
}

}  // namespace

void ui_draw_text(Frame& f, const Font& font, const std::string& text, const Rect& box, int halign, int valign, bool wrap,
                  const float color[4], const float* shadow_color, float shadow_h, float shadow_v, float gamma, const Rect* clip) {
    if (text.empty() || !font.valid() || color[3] <= 0.0f) return;
    const float line = static_cast<float>(font.line_height) * font.scale;
    const std::vector<std::string> lines = ui_wrap(font, text, box.w(), wrap);
    const float total = line * static_cast<float>(lines.size());
    float y = box.t;
    if (valign == 1) y = box.t + (box.h() - total) * 0.5f;
    if (valign == 2) y = box.b - total;
    float main_color[4], shadow[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    encode_color(color, gamma, main_color);
    if (shadow_color) encode_color(shadow_color, gamma, shadow);
    for (size_t i = 0; i < lines.size(); ++i) {
        const float width = font.width(lines[i]);
        float x = box.l;
        if (halign == 1) x = box.l + (box.w() - width) * 0.5f;
        if (halign == 2) x = box.r - width;
        // The canvas places a string on whole pixels.
        const float px = std::round(x), py = std::round(y + line * static_cast<float>(i));
        if (shadow_color && shadow[3] > 0.0f) {
            // UIComp_TdDropShadowString: the offsets are fractions of the line height.
            draw_line(f, font, lines[i], px + std::round(shadow_h * line), py + std::round(shadow_v * line), shadow, clip);
        }
        draw_line(f, font, lines[i], px, py, main_color, clip);
    }
}

void ui_draw_image(Frame& f, const Image& image, const Rect& box, const float uv[4], const float color[4], float gamma, const Rect* clip) {
    if (!image.valid() || color[3] <= 0.0f || box.w() <= 0.0f || box.h() <= 0.0f) return;
    DrawOp op;
    op.kind = DrawOp::Kind::Image;
    op.image = &image;
    encode_color(color, gamma, op.color);
    if (clip) {
        op.clipped = true;
        op.clip = *clip;
    }
    const float w = static_cast<float>(image.w), h = static_cast<float>(image.h);
    const float ul = uv[2] > 0.0f ? uv[2] : w, vl = uv[3] > 0.0f ? uv[3] : h;
    op.quads.push_back(Quad{box.l, box.t, box.r, box.b, uv[0] / w, uv[1] / h, (uv[0] + ul) / w, (uv[1] + vl) / h});
    f.ui.push_back(std::move(op));
}

void ui_draw_image_stretched(Frame& f, const Image& image, const Rect& box, const float uv[4], const float color[4], float gamma,
                             const Rect* clip) {
    if (!image.valid() || color[3] <= 0.0f || box.w() <= 0.0f || box.h() <= 0.0f) return;
    DrawOp op;
    op.kind = DrawOp::Kind::Image;
    op.image = &image;
    encode_color(color, gamma, op.color);
    if (clip) {
        op.clipped = true;
        op.clip = *clip;
    }
    const float tw = static_cast<float>(image.w), th = static_cast<float>(image.h);
    const float ul = uv[2] > 0.0f ? uv[2] : tw, vl = uv[3] > 0.0f ? uv[3] : th;
    // The canvas draws on whole pixels: texels land on screen pixels.
    const float left = std::floor(box.l), top = std::floor(box.t);
    const float width = box.w(), height = box.h();
    const float mid_u = std::floor(ul * 0.5f), mid_v = std::floor(vl * 0.5f);
    const float fx = std::min(mid_u, width * 0.5f);  // the width of a corner, on screen and in texels
    const float fy = std::min(mid_v, height * 0.5f);
    const float xs[4] = {left, left + fx, std::floor(left + width - fx), std::floor(left + width - fx) + fx};
    const float ys[4] = {top, top + fy, std::floor(top + height - fy), std::floor(top + height - fy) + fy};
    const float us[4] = {uv[0], uv[0] + fx, uv[0] + ul - fx, uv[0] + ul};
    const float vs[4] = {uv[1], uv[1] + fy, uv[1] + vl - fy, uv[1] + vl};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            if (xs[col + 1] <= xs[col] || ys[row + 1] <= ys[row]) continue;
            // The middle column and row are one texel from the centre of the image, stretched.
            const float u0 = col == 1 ? uv[0] + mid_u : us[col], u1 = col == 1 ? uv[0] + mid_u + 1.0f : us[col + 1];
            const float v0 = row == 1 ? uv[1] + mid_v : vs[row], v1 = row == 1 ? uv[1] + mid_v + 1.0f : vs[row + 1];
            op.quads.push_back(Quad{xs[col], ys[row], xs[col + 1], ys[row + 1], u0 / tw, v0 / th, u1 / tw, v1 / th});
        }
    }
    f.ui.push_back(std::move(op));
}

// --- the scene ------------------------------------------------------------------------------

int UiScene::find(const std::string& widget) const {
    for (size_t i = 0; i < widgets.size(); ++i) {
        if (widgets[i].name == widget) return static_cast<int>(i);
    }
    return -1;
}

void UiScene::set_text(UiSystem& ui, const std::string& widget, const std::string& markup) {
    if (UiWidget* w = get(widget)) {
        w->markup = markup;
        w->text = ui.resolve_markup(markup);
    }
}

void UiScene::set_visible(const std::string& widget, bool visible) {
    if (UiWidget* w = get(widget)) w->hidden = !visible;
}

bool UiScene::visible(int widget) const {
    for (int i = widget; i >= 0; i = widgets[static_cast<size_t>(i)].parent) {
        if (widgets[static_cast<size_t>(i)].hidden) return false;
    }
    return true;
}

std::vector<UiScene::BarButton>& UiScene::bar(const std::string& widget) {
    const int index = find(widget);
    for (auto& [w, buttons] : button_bars) {
        if (w == index) return buttons;
    }
    static std::vector<BarButton> none;
    none.clear();
    return none;
}

// UUIScene::ResolveScenePositions. Faces are resolved in the docking stack's order: widgets in
// tree order, each face after the face it is docked to. What a percentage padding measures is
// read as it stands at that moment: a face that has not been resolved yet reads as the face it
// is docked to, without its padding (which is why the same docking gives SettingsPanel a
// different top in TdAudioSettings, where it comes before the label it hangs from, than in
// TdGameSettings, where it comes after).
void UiScene::layout() {
    if (widgets.empty()) return;
    const Rect scene{0.0f, 0.0f, kSceneWidth, kSceneHeight};
    const size_t n = widgets.size();
    std::vector<float> value(n * 4, 0.0f);
    std::vector<uint8_t> state(n * 4, 0);  // 0 not resolved, 1 being resolved, 2 resolved
    value[2] = kSceneWidth;
    value[3] = kSceneHeight;
    for (int f = 0; f < 4; ++f) state[static_cast<size_t>(f)] = 2;

    std::function<float(int, int)> resolve;
    std::function<float(int, int)> read;

    // A Position value as viewport pixels. Right and Bottom are a width and a height from the
    // widget's own Left and Top unless they are in viewport pixels.
    auto evaluate = [&](int wi, int f) {
        const UiWidget& w = widgets[static_cast<size_t>(wi)];
        const float v = w.pos[f];
        const bool far_face = f >= 2;
        const int owner = w.parent < 0 ? 0 : w.parent;
        auto own_origin = [&] { return resolve(wi, f - 2); };
        auto owner_near = [&] { return resolve(owner, f & 1); };
        auto owner_extent = [&] { return resolve(owner, (f & 1) + 2) - resolve(owner, f & 1); };
        switch (w.pos_type[f]) {
            case kPosPixelViewport: return v;
            case kPosPixelScene: return far_face ? own_origin() + v : v;
            case kPosPixelOwner: return (far_face ? own_origin() : owner_near()) + v;
            case kPosPercentViewport:
            case kPosPercentScene: return (far_face ? own_origin() : 0.0f) + v * ((f & 1) ? scene.h() : scene.w());
            default: return (far_face ? own_origin() : owner_near()) + v * owner_extent();
        }
    };
    auto target_of = [&](const UiWidget& w, int f) { return w.dock_widget[f] >= 0 ? w.dock_widget[f] : 0; };

    read = [&](int wi, int f) -> float {
        const size_t k = static_cast<size_t>(wi) * 4 + static_cast<size_t>(f);
        if (state[k] == 2) return value[k];
        const UiWidget& w = widgets[static_cast<size_t>(wi)];
        if (w.dock_face[f] < 4) return resolve(target_of(w, f), w.dock_face[f]);
        return resolve(wi, f);
    };

    // The height or width the string wants, in scene pixels; negative if it does not size the widget.
    auto string_extent = [&](int wi, int orientation) -> float {
        const UiWidget& w = widgets[static_cast<size_t>(wi)];
        if (!w.string.present || !w.string.style || !w.string.autosize[orientation] || w.cls == "TdUIButtonBarButton") return -1.0f;
        if (w.dock_face[orientation] < 4 && w.dock_face[orientation + 2] < 4) return -1.0f;  // both faces docked
        const UiTextStyle& ts = w.string.style->text_for(UiState::Enabled);
        if (!ts.font || !ts.font->valid()) return -1.0f;
        if (orientation == 0) {
            float width = 0.0f;
            for (const std::string& l : ui_wrap(*ts.font, w.text, 0.0f, false)) width = std::max(width, ts.font->width(l) / view_scale);
            return width;
        }
        const float width = (resolve(wi, 2) - resolve(wi, 0)) * view_scale;
        const size_t lines = ui_wrap(*ts.font, w.text, width, ts.wrap).size();
        return static_cast<float>(ts.font->line_height) * ts.font->scale / view_scale * static_cast<float>(std::max<size_t>(lines, 1));
    };

    resolve = [&](int wi, int f) -> float {
        const size_t k = static_cast<size_t>(wi) * 4 + static_cast<size_t>(f);
        if (state[k] != 0) return value[k];
        state[k] = 1;
        const UiWidget& w = widgets[static_cast<size_t>(wi)];
        const int orientation = f & 1;
        const float wanted = string_extent(wi, orientation);
        // Auto-sizing moves the far face, or the near one when only the far one is docked.
        const bool sizes_near = wanted >= 0.0f && w.dock_face[orientation + 2] < 4 && w.dock_face[orientation] >= 4;
        float out;
        if (wanted >= 0.0f && f < 2 && sizes_near) {
            out = resolve(wi, f + 2) - wanted;
        } else if (wanted >= 0.0f && f >= 2 && !sizes_near) {
            out = resolve(wi, f - 2) + wanted;
        } else if (w.dock_face[f] < 4) {
            const int target = target_of(w, f);
            const float base = resolve(target, w.dock_face[f]);
            value[k] = base;  // what this face reads as while its padding is worked out
            float pad = w.dock_pad[f];
            switch (w.dock_pad_type[f]) {
                case kPadPercentTarget: pad *= read(target, orientation + 2) - read(target, orientation); break;
                case kPadPercentOwner: {
                    const float lo = f < 2 ? base : read(wi, orientation);
                    const float hi = f < 2 ? read(wi, orientation + 2) : base;
                    pad *= hi - lo;
                    break;
                }
                case kPadPercentScene:
                case kPadPercentViewport: pad *= orientation ? scene.h() : scene.w(); break;
                default: break;
            }
            out = base + pad;
        } else {
            out = evaluate(wi, f);
        }
        value[k] = out;
        state[k] = 2;
        return out;
    };

    // Tree order: the widgets were loaded depth first in Children order.
    std::vector<int> order;
    std::vector<int> stack{0};
    while (!stack.empty()) {
        const int i = stack.back();
        stack.pop_back();
        order.push_back(i);
        const std::vector<int>& children = widgets[static_cast<size_t>(i)].children;
        for (size_t c = children.size(); c-- > 0;) stack.push_back(children[c]);
    }
    for (int wi : order) {
        for (int f = 0; f < 4; ++f) resolve(wi, f);
    }
    for (size_t i = 0; i < n; ++i) widgets[i].rect = Rect{value[i * 4], value[i * 4 + 1], value[i * 4 + 2], value[i * 4 + 3]};
}

namespace {

Rect to_view(const Rect& s, float scale, float origin_x) {
    return Rect{origin_x + s.l * scale, s.t * scale, origin_x + s.r * scale, s.b * scale};
}

}  // namespace

void UiScene::draw_widget(Frame& f, int index, float scale, float origin_x, float gamma, float opacity) const {
    const UiWidget& w = widgets[static_cast<size_t>(index)];
    if (w.hidden) return;
    opacity *= w.opacity;
    if (opacity <= 0.0f) return;
    const Rect box = to_view(w.rect, scale, origin_x);
    const UiState state = w.state();

    auto draw_comp = [&](const UiImageComp& comp, const Rect& where, UiState st) {
        if (!comp.present) return;
        const UiStyle* style = comp.style;
        if (w.parent >= 0) {
            const UiWidget& parent = widgets[static_cast<size_t>(w.parent)];
            if (comp.resolver == "IncrementStyle" && parent.increment) style = parent.increment;
            if (comp.resolver == "DecrementStyle" && parent.decrement) style = parent.decrement;
        }
        if (!style) return;
        const UiImageStyle& is = style->image_for(st);
        const Image* image = comp.texture ? comp.texture : is.image;
        if (!image || !image->valid()) return;
        float color[4] = {is.color[0], is.color[1], is.color[2], is.color[3] * comp.opacity * opacity};
        // StylePadding is taken off each side of the widget (it is negative on the panel styles).
        Rect r{where.l + is.padding[0], where.t + is.padding[1], where.r - is.padding[0], where.b - is.padding[1]};
        if (is.adjust[0] == kAdjustStretch && is.adjust[1] == kAdjustStretch) {
            ui_draw_image_stretched(f, *image, r, is.uv, color, gamma);
            return;
        }
        // EMaterialAdjustmentType: Normal scales the image to the widget, Justified keeps its
        // shape, None its size.
        float size[2] = {static_cast<float>(image->w) * scale, static_cast<float>(image->h) * scale};
        if (is.adjust[0] == kAdjustJustified || is.adjust[1] == kAdjustJustified) {
            const float fit = std::min(where.w() / size[0], where.h() / size[1]);
            size[0] *= fit;
            size[1] *= fit;
        }
        for (int k = 0; k < 2; ++k) {
            if (is.adjust[k] == kAdjustNormal || is.adjust[k] == kAdjustStretch) continue;
            float& lo = k == 0 ? r.l : r.t;
            float& hi = k == 0 ? r.r : r.b;
            const float room = hi - lo;
            if (is.align[k] == kAlignCenter) lo += (room - size[k]) * 0.5f;
            else if (is.align[k] == kAlignRight) lo = hi - size[k];
            hi = lo + size[k];
        }
        ui_draw_image(f, *image, r, is.uv, color, gamma);
    };

    if (w.cls == "TdUIButtonBar") {
        // The six button templates are placed by TdUIButtonBar.AppendButton, not by the scene.
        for (const auto& [bar_index, buttons] : button_bars) {
            if (bar_index != index || !bar_font || !bar_font->valid()) continue;
            float right = box.r;
            for (const BarButton& b : buttons) {
                if (b.label.empty()) continue;
                const float width = bar_font->width(b.label);
                const Rect text{right - width, box.t, right, box.b};
                if (bar_image && bar_image->valid()) {
                    // TdImageButtonBarBackground around the auto-sized label: StylePadding (-20, -3).
                    const float white[4] = {1.0f, 1.0f, 1.0f, opacity};
                    const float full[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                    ui_draw_image_stretched(f, *bar_image, Rect{text.l - bar_padding[0], text.t - bar_padding[1], text.r + bar_padding[0], text.b + bar_padding[1]},
                                            full, white, gamma);
                }
                float color[4] = {bar_text[0], bar_text[1], bar_text[2], bar_text[3] * opacity * (b.disabled ? 0.5f : 1.0f)};
                float shadow[4] = {bar_shadow[0], bar_shadow[1], bar_shadow[2], bar_shadow[3] * opacity};
                ui_draw_text(f, *bar_font, b.label, text, 0, 1, false, color, shadow, 0.06f, 0.06f, gamma);
                right = text.l - 50.0f * scale;
            }
        }
        return;
    }

    if (w.cls == "UISlider") {
        // The bar across the widget's width, BarSize tall, and the marker at the value.
        const float mid = (box.t + box.b) * 0.5f;
        const float bar_h = w.bar_size * scale;
        draw_comp(w.image, box, state);
        if (w.bar.present && w.bar.style) {
            const UiImageStyle& is = w.bar.style->image_for(state);
            if (is.image && is.image->valid()) {
                const float color[4] = {is.color[0], is.color[1], is.color[2], is.color[3] * opacity};
                ui_draw_image(f, *is.image, Rect{box.l, mid - bar_h * 0.5f, box.r, mid + bar_h * 0.5f}, is.uv, color, gamma);
            }
        }
        if (w.marker.present && w.marker.style) {
            const UiImageStyle& is = w.marker.style->image_for(state);
            if (is.image && is.image->valid()) {
                const float range = w.slider[1] - w.slider[0];
                const float t = range > 0.0f ? std::clamp((w.slider[2] - w.slider[0]) / range, 0.0f, 1.0f) : 0.0f;
                const float mw = w.marker_width * box.w();
                const float mh = w.marker_height * scale;
                const float x = box.l + (box.w() - mw) * t;
                const float color[4] = {is.color[0], is.color[1], is.color[2], is.color[3] * opacity};
                ui_draw_image(f, *is.image, Rect{x, mid - mh * 0.5f, x + mw, mid + mh * 0.5f}, is.uv, color, gamma);
            }
        }
    } else {
        draw_comp(w.image, box, state);
    }

    if (w.string.present && w.string.style && !w.text.empty()) {
        const UiTextStyle& ts = w.string.style->text_for(state);
        if (ts.font && ts.font->valid()) {
            const int halign = w.string.align[0] >= 0 ? w.string.align[0] : ts.align[0];
            const int valign = w.string.align[1] >= 0 ? w.string.align[1] : ts.align[1];
            float color[4] = {ts.color[0], ts.color[1], ts.color[2], ts.color[3] * opacity};
            float shadow[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            bool has_shadow = false;
            if (w.string.shadow) {
                const UiTextStyle& ss = w.string.shadow->text_for(state);
                shadow[0] = ss.color[0];
                shadow[1] = ss.color[1];
                shadow[2] = ss.color[2];
                shadow[3] = ss.color[3] * opacity;
                has_shadow = shadow[3] > 0.0f;
            }
            ui_draw_text(f, *ts.font, w.text, box, halign == kAlignDefault ? 0 : halign, valign == kAlignDefault ? 0 : valign, ts.wrap, color,
                         has_shadow ? shadow : nullptr, w.string.shadow_h, w.string.shadow_v, gamma);
        }
    }

    // Children, the deeper ZDepth first.
    std::vector<int> order = w.children;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return widgets[static_cast<size_t>(a)].zdepth > widgets[static_cast<size_t>(b)].zdepth;
    });
    for (int child : order) draw_widget(f, child, scale, origin_x, gamma, opacity);
}

void UiScene::draw(Frame& f, float scale, float origin_x, float gamma) const {
    if (!widgets.empty()) draw_widget(f, 0, scale, origin_x, gamma, 1.0f);
}

}  // namespace me::fe
