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
    const UProperty* field2(const std::string& prop, const std::string& inner, const std::string& member, int member_index = 0,
                            int prop_index = 0) const {
        for (const UPropertyList* list : lists) {
            const UProperty* p = find_prop(*list, prop, prop_index);
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
    const UiStyle* style_ref(const View& v, const char* prop, int prop_index = 0) {
        load_skin();
        std::array<int32_t, 4> id{0, 0, 0, 0};
        static const char* const kParts[4] = {"A", "B", "C", "D"};
        for (int k = 0; k < 4; ++k) {
            if (const UProperty* part = v.field2(prop, "AssignedStyleID", kParts[k], 0, prop_index)) id[static_cast<size_t>(k)] = part->i;
        }
        if (id[0] || id[1] || id[2] || id[3]) {
            auto it = style_by_id.find(id);
            if (it != style_by_id.end()) return resolve_style(it->second);
        }
        if (const UProperty* tag = v.field(prop, "DefaultStyleTag", 0, nullptr, prop_index)) {
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
        if (is_true(v.field("StyleCustomization", "bOverrideFormatting"), false)) {
            for (int k = 0; k < 2; ++k) {
                for (const UPropertyList* list : v.lists) {
                    const UProperty* custom = find_prop(*list, "StyleCustomization");
                    const UProperty* fmt = custom ? find_prop(custom->fields, "Formatting", k) : nullptr;
                    if (!fmt) continue;
                    if (const UProperty* a = find_prop(fmt->fields, "AdjustmentType")) out.adjust[k] = static_cast<int8_t>(adjust_of(a->s));
                    if (const UProperty* a = find_prop(fmt->fields, "Alignment")) out.align[k] = static_cast<int8_t>(align_of(a->s, kAlignLeft));
                    break;
                }
            }
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
                    // A localized string may itself carry tags (a gamepad glyph alias in front of
                    // "QUIT GAME") and line breaks.
                    out += resolve_markup(assets_->localized(tag.substr(colon + 1)));
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

std::vector<UiRun> UiSystem::parse_runs(const std::string& markup) {
    std::vector<UiRun> runs;
    UiRun state;  // the font and colour in force
    std::vector<UiRun> saved;
    auto emit = [&](const std::string& text) {
        if (text.empty()) return;
        if (!runs.empty() && runs.back().font == state.font && runs.back().colored == state.colored &&
            std::equal(state.color, state.color + 4, runs.back().color)) {
            runs.back().text += text;
            return;
        }
        UiRun r = state;
        r.text = text;
        runs.push_back(std::move(r));
    };
    std::string text;
    for (size_t i = 0; i < markup.size();) {
        const size_t close = markup[i] == '<' ? markup.find('>', i) : std::string::npos;
        if (close == std::string::npos) {
            text.push_back(markup[i++]);
            continue;
        }
        emit(text);
        text.clear();
        const std::string tag = markup.substr(i + 1, close - i - 1);
        i = close + 1;
        const size_t colon = tag.find(':');
        if (colon == std::string::npos) continue;
        const std::string kind = tag.substr(0, colon), value = tag.substr(colon + 1);
        if (kind == "Strings") {
            if (!assets_) continue;
            // The string's own tags apply inside it only.
            const UiRun outer = state;
            for (UiRun& r : parse_runs(assets_->localized(value))) {
                if (!r.font) r.font = outer.font;
                if (!r.colored && outer.colored) {
                    r.colored = true;
                    std::copy(outer.color, outer.color + 4, r.color);
                }
                state = r;
                emit(r.text);
            }
            state = outer;
        } else if (value == "/") {
            if (!saved.empty()) {
                state = saved.back();
                saved.pop_back();
            }
        } else if (kind == "Fonts") {
            saved.push_back(state);
            const size_t dot = value.rfind('.');
            if (assets_) state.font = assets_->font(dot == std::string::npos ? value : value.substr(dot + 1));
        } else if (kind == "Styles") {
            saved.push_back(state);
            if (const UiStyle* style = style_by_tag(value)) {
                const UiTextStyle& ts = style->text_for(UiState::Enabled);
                if (ts.font) state.font = ts.font;
            }
        } else if (kind == "Color") {
            saved.push_back(state);
            state.colored = true;
            for (int k = 0; k < 4; ++k) {
                const size_t at = value.find(std::string(1, "RGBA"[k]) + "=");
                if (at != std::string::npos) state.color[k] = static_cast<float>(std::atof(value.c_str() + at + 2));
            }
        }
    }
    emit(text);
    return runs;
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
        if (w.markup.empty()) {
            if (const UProperty* p = v.field("CaptionDataSource", "MarkupString")) w.markup = p->s;  // UILabelButton
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
        if (w.cls == "UITdOptionButton" || w.cls == "UIScrollbar") {
            w.increment = impl_->style_ref(v, "IncrementStyle");
            w.decrement = impl_->style_ref(v, "DecrementStyle");
            if (w.cls == "UIScrollbar") w.marker_style = impl_->style_ref(v, "MarkerStyle");
        }
        if (w.cls == "UIList") {
            auto list = std::make_shared<UiList>();
            if (const UProperty* p = v.field("RowHeight", "Value")) list->row_height = p->f;
            if (const UProperty* p = v.field("RowHeight", "ScaleType")) list->row_percent = p->s == "UIEXTENTEVAL_PercentSelf";
            if (const UProperty* p = v.field("CellPadding", "Value")) list->cell_padding = p->f;
            for (int k = 0; k < 4; ++k) {
                list->cell[k] = impl_->style_ref(v, "GlobalCellStyle", k);
                list->overlay[k] = impl_->style_ref(v, "ItemOverlayStyle", k);
            }
            // RowAutoSizeMode other than CELLAUTOSIZE_None: a row is as tall as its text.
            const UProperty* auto_size = v.find("RowAutoSizeMode");
            if ((!auto_size || auto_size->s != "CELLAUTOSIZE_None") && list->cell[0]) {
                const UiTextStyle& ts = list->cell[0]->text_for(UiState::Enabled);
                if (ts.font && ts.font->valid()) {
                    list->row_height = static_cast<float>(ts.font->line_height) * ts.font->scale / view_scale_;
                    list->row_percent = false;
                }
            }
            if (const UProperty* p = v.find("VerticalScrollbar")) {
                auto it = widget_of.find(p->i);
                if (it != widget_of.end()) list->scrollbar = it->second;
            }
            if (const UProperty* p = v.find("CellDataComponent", 0, &owner)) {
                const ObjRef presenter = impl_->resolve(owner, p->i);
                if (presenter.ok()) {
                    const View pv = impl_->view(presenter);
                    list->every_other = is_true(pv.find("bOnlyDrawEveryOtherElementOverlay"), false);
                    if (const UProperty* cells = pv.field("ElementSchema", "Cells")) {
                        for (const auto& cell : cells->elements) {
                            UiListColumn column;
                            column.field = prop_name(cell, "CellDataField");
                            if (const UProperty* size = find_prop(cell, "CellSize")) {
                                if (const UProperty* value = find_prop(size->fields, "Value")) column.width = value->f;
                                if (const UProperty* type = find_prop(size->fields, "ScaleType")) column.percent = type->s == "UIEXTENTEVAL_PercentSelf";
                            }
                            list->columns.push_back(std::move(column));
                        }
                    }
                }
            }
            w.list = std::move(list);
        }
        if (w.cls.find("TabControl") != std::string::npos) {
            if (const UProperty* pages = v.find("Pages")) {
                for (int32_t page : pages->ints) {
                    auto it = widget_of.find(page);
                    if (it != widget_of.end()) w.pages.push_back(it->second);
                }
            }
        }
        if (w.cls.find("TabPage") != std::string::npos) {
            if (const UProperty* p = v.find("TabButton")) {
                auto it = widget_of.find(p->i);
                if (it != widget_of.end()) w.tab_button = it->second;
            }
            // UITabPage.ButtonCaption is what the page's button says.
            if (const UProperty* caption = v.field("ButtonCaption", "MarkupString")) {
                if (w.tab_button >= 0 && !caption->s.empty()) {
                    UiWidget& button = scene->widgets[static_cast<size_t>(w.tab_button)];
                    button.markup = caption->s;
                    button.text = resolve_markup(caption->s);
                }
            }
        }
        // A tab button is drawn in its tab control's styles.
        if (w.cls.find("TabButton") != std::string::npos && w.parent >= 0 &&
            scene->widgets[static_cast<size_t>(w.parent)].cls.find("TabControl") != std::string::npos) {
            const View control = impl_->view(ObjRef{pkg, export_of[static_cast<size_t>(w.parent)]});
            if (const UiStyle* caption = impl_->style_ref(control, "TabButtonCaptionStyle")) w.string.style = caption;
            if (const UiStyle* background = impl_->style_ref(control, "TabButtonBackgroundStyle")) w.image.style = background;
        }
    }

    // What every TdUIButtonBar button looks like, from the first bar's first template.
    for (const UiWidget& w : scene->widgets) {
        if (w.cls != "TdUIButtonBarButton") continue;
        if (w.string.style) {
            scene->bar_font = w.string.style->text_for(UiState::Enabled).font;
            scene->bar_text = w.string.style;
        }
        scene->bar_shadow = w.string.shadow;
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
            // UIComp_TdDropShadowString: the same glyphs again, moved by fractions of the line
            // height and not put back on whole pixels, so the filter softens them: 1.44 pixels
            // right for the 24 pixel font. Down it is that less one pixel (measured: the button
            // bar's and the labels' shadows start 0.44 below their text, the big title's 1.2).
            draw_line(f, font, lines[i], px + shadow_h * line, py + shadow_v * line - 1.0f, shadow, clip);
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
    const float ul = uv[2] != 0.0f ? uv[2] : w, vl = uv[3] != 0.0f ? uv[3] : h;
    // A negative extent runs backwards from the origin, which wraps: UL = -1024 at U = 0 is the
    // whole 1024 wide texture mirrored.
    float u0 = uv[0], u1 = uv[0] + ul, v0 = uv[1], v1 = uv[1] + vl;
    if (ul < 0.0f) {
        u0 += w;
        u1 += w;
    }
    if (vl < 0.0f) {
        v0 += h;
        v1 += h;
    }
    op.quads.push_back(Quad{box.l, box.t, box.r, box.b, u0 / w, v0 / h, u1 / w, v1 / h});
    f.ui.push_back(std::move(op));
}

void ui_draw_image_stretched(Frame& f, const Image& image, const Rect& box, const float uv[4], const float color[4], float gamma,
                             const Rect* clip, bool stretch_h, bool stretch_v) {
    if (!image.valid() || color[3] <= 0.0f || box.w() <= 0.0f || box.h() <= 0.0f) return;
    if (uv[2] < 0.0f || uv[3] < 0.0f || (!stretch_h && !stretch_v)) {
        ui_draw_image(f, image, box, uv, color, gamma, clip);
        return;
    }
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
    const float width = box.w(), height = box.h();
    const float mid_u = std::floor(ul * 0.5f), mid_v = std::floor(vl * 0.5f);
    // A corner is half the image. Where the box is larger than the image the halves keep their
    // size and the gap is filled from the middle; where it is smaller they are scaled down to
    // meet (a 1024x256 panel behind a short message box is drawn at 0.7 of its size, frame and all).
    // An axis that is not stretched has no middle: its halves meet in the middle of the box.
    const float fx = stretch_h ? std::min(mid_u, width * 0.5f) : width * 0.5f;
    const float fy = stretch_v ? std::min(mid_v, height * 0.5f) : height * 0.5f;
    // Nothing is put on whole pixels: the quads sit where the layout left the widget, and a box
    // that starts between two pixels has its image filtered across them. (Fitted on retail's
    // frames: the tab frame of CONTROLS and the button bar's boxes both come out wrong, by up to
    // a pixel, with the near edges floored.)
    const float xs[4] = {box.l, box.l + fx, box.r - fx, box.r};
    const float ys[4] = {box.t, box.t + fy, box.b - fy, box.b};
    const float us[4] = {uv[0], uv[0] + mid_u, uv[0] + ul - mid_u, uv[0] + ul};
    const float vs[4] = {uv[1], uv[1] + mid_v, uv[1] + vl - mid_v, uv[1] + vl};
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

void UiScene::list_select(int widget, int index) {
    if (widget < 0 || !widgets[static_cast<size_t>(widget)].list) return;
    UiWidget& w = widgets[static_cast<size_t>(widget)];
    UiList& list = *w.list;
    const int count = static_cast<int>(list.rows.size());
    const int shown = std::max(list.visible(w.rect.h()), 1);
    list.index = count > 0 ? std::clamp(index, 0, count - 1) : 0;
    // The selection is kept in view.
    if (list.index < list.top) list.top = list.index;
    if (list.index >= list.top + shown) list.top = list.index - shown + 1;
    list.top = std::clamp(list.top, 0, std::max(count - shown, 0));
    if (list.scrollbar < 0) return;
    // UIScrollbar: the marker's place and size along the track between the two buttons are the
    // part of the list that is shown.
    const UiWidget& bar = widgets[static_cast<size_t>(list.scrollbar)];
    for (int child : bar.children) {
        UiWidget& c = widgets[static_cast<size_t>(child)];
        if (c.cls != "UIScrollbarMarkerButton") continue;
        const float button = bar.rect.w();
        const float track = std::max(bar.rect.h() - 2.0f * button, 0.0f);
        const float total = static_cast<float>(std::max(count, 1));
        c.pos[1] = button + track * static_cast<float>(list.top) / total;
        c.pos[3] = track * std::min(static_cast<float>(shown) / total, 1.0f);
        // Nothing hangs off the marker: it is put in place here rather than by a new layout.
        c.rect = Rect{bar.rect.l, bar.rect.t + c.pos[1], bar.rect.r, bar.rect.t + c.pos[1] + c.pos[3]};
        if (face_value_.size() == widgets.size() * 4) {
            const float faces[4] = {c.rect.l, c.rect.t, c.rect.r, c.rect.b};
            std::copy(faces, faces + 4, face_value_.begin() + static_cast<std::ptrdiff_t>(child) * 4);
        }
    }
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

// UUIScene::ResolveScenePositions.
//
// The first pass resolves every face in the docking stack's order: widgets in tree order, each
// face after the face it is docked to, strings not yet sizing anything. What a percentage padding
// measures is read as it stands at that moment, and a face that has not been resolved yet reads
// as the face it is docked to, without its padding. (That is why the same docking gives
// SettingsPanel a different top in TdAudioSettings, where it comes before the label it hangs
// from, than in TdGameSettings, where it comes after.)
//
// Then the auto-sized strings set their widgets' sizes, and only the faces that depend on a face
// that moved are resolved again. A face that does not (the top of the message box's panel, which
// hangs from the top of the message) keeps what the first pass gave it.
void UiScene::layout() {
    if (widgets.empty()) return;
    const Rect scene{0.0f, 0.0f, kSceneWidth, kSceneHeight};
    const size_t n = widgets.size();
    const bool first = face_value_.size() != n * 4;
    if (first) {
        face_value_.assign(n * 4, 0.0f);
        face_value_[2] = kSceneWidth;
        face_value_[3] = kSceneHeight;
    }
    std::vector<float>& value = face_value_;
    std::vector<uint8_t> state(n * 4, first ? 0 : 2);  // 0 not resolved, 1 being resolved, 2 resolved
    for (int f = 0; f < 4; ++f) state[static_cast<size_t>(f)] = 2;
    bool autosize = false;

    std::function<float(int, int)> resolve;
    auto target_of = [&](const UiWidget& w, int f) { return w.dock_widget[f] >= 0 ? w.dock_widget[f] : 0; };

    // The extent the string wants, in scene pixels; negative if it does not size the widget that way.
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
    // Auto-sizing moves the far face, or the near one when only the far one is docked.
    auto sized_face = [&](int wi, int orientation) {
        const UiWidget& w = widgets[static_cast<size_t>(wi)];
        return (w.dock_face[orientation + 2] < 4 && w.dock_face[orientation] >= 4) ? orientation : orientation + 2;
    };

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
    auto read = [&](int wi, int f) -> float {
        const size_t k = static_cast<size_t>(wi) * 4 + static_cast<size_t>(f);
        if (state[k] == 2) return value[k];
        const UiWidget& w = widgets[static_cast<size_t>(wi)];
        if (w.dock_face[f] < 4) return resolve(target_of(w, f), w.dock_face[f]);
        return resolve(wi, f);
    };
    resolve = [&](int wi, int f) -> float {
        const size_t k = static_cast<size_t>(wi) * 4 + static_cast<size_t>(f);
        if (state[k] != 0) return value[k];
        state[k] = 1;
        const UiWidget& w = widgets[static_cast<size_t>(wi)];
        const int orientation = f & 1;
        const float wanted = autosize && sized_face(wi, orientation) == f ? string_extent(wi, orientation) : -1.0f;
        float out;
        if (wanted >= 0.0f) {
            out = f < 2 ? resolve(wi, f + 2) - wanted : resolve(wi, f - 2) + wanted;
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
    if (first) {
        for (int wi : order) {
            for (int f = 0; f < 4; ++f) resolve(wi, f);
        }
    }

    // What each face is resolved from, turned round: the faces to resolve again when one moves.
    std::vector<std::vector<int>> dependents(n * 4);
    for (size_t wi = 1; wi < n; ++wi) {
        const UiWidget& w = widgets[wi];
        const int owner = w.parent < 0 ? 0 : w.parent;
        for (int f = 0; f < 4; ++f) {
            const int self = static_cast<int>(wi) * 4 + f;
            auto needs = [&](int widget, int face) { dependents[static_cast<size_t>(widget) * 4 + static_cast<size_t>(face)].push_back(self); };
            if (w.dock_face[f] < 4) {
                needs(target_of(w, f), w.dock_face[f]);
            } else if (w.pos_type[f] != kPosPixelViewport) {
                if (f >= 2) needs(static_cast<int>(wi), f - 2);
                if (w.pos_type[f] == kPosPixelOwner || w.pos_type[f] == kPosPercentOwner) {
                    needs(owner, f & 1);
                    if (w.pos_type[f] == kPosPercentOwner) needs(owner, (f & 1) + 2);
                }
            }
        }
        // A wrapped string's height follows its width.
        if (w.string.present && w.string.autosize[1]) {
            dependents[wi * 4 + 0].push_back(static_cast<int>(wi) * 4 + sized_face(static_cast<int>(wi), 1));
            dependents[wi * 4 + 2].push_back(static_cast<int>(wi) * 4 + sized_face(static_cast<int>(wi), 1));
        }
    }

    autosize = true;
    for (int pass = 0; pass < 4; ++pass) {
        // The faces the strings size, and everything hanging off them, are open again.
        std::vector<int> open;
        for (size_t wi = 1; wi < n; ++wi) {
            for (int orientation = 0; orientation < 2; ++orientation) {
                if (string_extent(static_cast<int>(wi), orientation) >= 0.0f) open.push_back(static_cast<int>(wi) * 4 + sized_face(static_cast<int>(wi), orientation));
            }
        }
        if (open.empty()) break;
        const std::vector<float> before = value;
        while (!open.empty()) {
            const int k = open.back();
            open.pop_back();
            if (state[static_cast<size_t>(k)] == 0) continue;
            state[static_cast<size_t>(k)] = 0;
            for (int d : dependents[static_cast<size_t>(k)]) open.push_back(d);
        }
        for (int wi : order) {
            for (int f = 0; f < 4; ++f) resolve(wi, f);
        }
        float moved = 0.0f;
        for (size_t k = 0; k < value.size(); ++k) moved = std::max(moved, std::fabs(value[k] - before[k]));
        if (moved < 0.01f) break;
    }
    for (size_t i = 0; i < n; ++i) widgets[i].rect = Rect{value[i * 4], value[i * 4 + 1], value[i * 4 + 2], value[i * 4 + 3]};
}

namespace {

Rect to_view(const Rect& s, float scale, float origin_x) {
    return Rect{origin_x + s.l * scale, s.t * scale, origin_x + s.r * scale, s.b * scale};
}

}  // namespace

void UiScene::draw_widget(Frame& f, int index, float scale, float origin_x, float gamma, float opacity) {
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
            if (w.cls == "UIScrollbarMarkerButton" && parent.marker_style) style = parent.marker_style;
        }
        if (!style) return;
        UiImageStyle is = style->image_for(st);
        for (int k = 0; k < 2; ++k) {
            if (comp.adjust[k] >= 0) is.adjust[k] = static_cast<uint8_t>(comp.adjust[k]);
            if (comp.align[k] >= 0) is.align[k] = static_cast<uint8_t>(comp.align[k]);
        }
        const Image* image = comp.texture ? comp.texture : is.image;
        if (!image || !image->valid()) return;
        float color[4] = {is.color[0], is.color[1], is.color[2], is.color[3] * comp.opacity * opacity};
        // StylePadding is taken off each side of the widget (it is negative on the panel styles).
        Rect r{where.l + is.padding[0], where.t + is.padding[1], where.r - is.padding[0], where.b - is.padding[1]};
        if (is.adjust[0] == kAdjustStretch || is.adjust[1] == kAdjustStretch) {
            ui_draw_image_stretched(f, *image, r, is.uv, color, gamma, nullptr, is.adjust[0] == kAdjustStretch, is.adjust[1] == kAdjustStretch);
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
        for (auto& [bar_index, buttons] : button_bars) {
            if (bar_index != index || !bar_font || !bar_font->valid()) continue;
            float right = box.r;
            for (BarButton& b : buttons) {
                b.rect = Rect{};
                if (b.label.empty() || b.hidden) continue;
                const float width = bar_font->width(b.label);
                const Rect text{right - width, box.t, right, box.b};
                b.rect = Rect{text.l - bar_padding[0], text.t - bar_padding[1], text.r + bar_padding[0], text.b + bar_padding[1]};
                if (bar_image && bar_image->valid()) {
                    // TdImageButtonBarBackground around the auto-sized label: StylePadding (-20, -3).
                    const float white[4] = {1.0f, 1.0f, 1.0f, opacity};
                    const float full[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                    ui_draw_image_stretched(f, *bar_image, b.rect, full, white, gamma);
                }
                // A disabled button is its label in the Disabled state: paler, and without the shadow.
                const UiState st = b.disabled ? UiState::Disabled : UiState::Enabled;
                float color[4] = {1.0f, 1.0f, 1.0f, opacity};
                float shadow[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                if (bar_text) {
                    const UiTextStyle& ts = bar_text->text_for(st);
                    std::copy(ts.color, ts.color + 3, color);
                    color[3] = ts.color[3] * opacity;
                }
                if (bar_shadow) {
                    const UiTextStyle& ss = bar_shadow->text_for(st);
                    std::copy(ss.color, ss.color + 3, shadow);
                    shadow[3] = ss.color[3] * opacity;
                }
                ui_draw_text(f, *bar_font, b.label, text, 0, 1, false, color, shadow[3] > 0.0f ? shadow : nullptr, 0.06f, 0.06f, gamma);
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

    if (w.list) {
        // UIComp_ListPresenter: each element that fits, top down. An element is its overlay (every
        // other one has the normal overlay, the selected one its bar) and its cells left to right.
        const UiList& list = *w.list;
        const float pitch = list.pitch(w.rect.h());
        const int shown = list.visible(w.rect.h());
        // The elements stop at the scrollbar, where one is shown.
        const bool scrollbar = list.scrollbar >= 0 && !widgets[static_cast<size_t>(list.scrollbar)].hidden;
        const float right = scrollbar ? widgets[static_cast<size_t>(list.scrollbar)].rect.l : w.rect.r;
        for (int k = 0; k < shown && list.top + k < static_cast<int>(list.rows.size()); ++k) {
            const int element = list.top + k;
            const UiListRow& row = list.rows[static_cast<size_t>(element)];
            const bool selected = element == list.index;
            const Rect cell_rect{w.rect.l, w.rect.t + pitch * static_cast<float>(k), right, w.rect.t + pitch * static_cast<float>(k + 1)};
            const UiStyle* overlay = selected ? list.overlay[2] : ((!list.every_other || (element & 1)) ? list.overlay[0] : nullptr);
            if (overlay) {
                const UiImageStyle& is = overlay->image_for(UiState::Enabled);
                if (is.image && is.image->valid()) {
                    const float color[4] = {is.color[0], is.color[1], is.color[2], is.color[3] * opacity};
                    ui_draw_image(f, *is.image, to_view(cell_rect, scale, origin_x), is.uv, color, gamma);
                }
            }
            const UiStyle* style = list.cell[selected ? 2 : 0];
            if (!style) continue;
            const UiTextStyle& ts = style->text_for(row.enabled ? UiState::Enabled : UiState::Disabled);
            if (!ts.font || !ts.font->valid()) continue;
            float x = w.rect.l;
            for (size_t c = 0; c < list.columns.size() && c < row.cells.size(); ++c) {
                const float width = list.columns[c].percent ? list.columns[c].width * w.rect.w() : list.columns[c].width;
                // A cell's text starts half the padding in from the cell's left and top (measured:
                // five pixels each way with CellPadding 10).
                const float inset = list.cell_padding * 0.5f;
                const Rect text = to_view(Rect{x + inset, cell_rect.t + inset, x + width, cell_rect.b}, scale, origin_x);
                float pen = text.l;
                // Pieces in a smaller font sit in the middle of the line's height.
                float line = 0.0f;
                for (const UiRun& run : row.cells[c]) {
                    const Font& font = run.font && run.font->valid() ? *run.font : *ts.font;
                    line = std::max(line, static_cast<float>(font.line_height) * font.scale);
                }
                for (const UiRun& run : row.cells[c]) {
                    const Font& font = run.font && run.font->valid() ? *run.font : *ts.font;
                    const float drop = (line - static_cast<float>(font.line_height) * font.scale) * 0.5f;
                    float color[4] = {ts.color[0], ts.color[1], ts.color[2], ts.color[3] * opacity};
                    if (run.colored) {
                        std::copy(run.color, run.color + 3, color);
                        color[3] = run.color[3] * ts.color[3] * opacity;
                    }
                    const float run_width = font.width(run.text);
                    ui_draw_text(f, font, run.text, Rect{pen, text.t + drop, pen + run_width, text.b}, 0, 0, false, color, nullptr, 0.0f, 0.0f, gamma);
                    pen += run_width;
                }
                x += width;
            }
        }
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

void UiScene::draw(Frame& f, float scale, float origin_x, float gamma, float opacity) {
    if (!widgets.empty()) draw_widget(f, 0, scale, origin_x, gamma, opacity);
}

}  // namespace me::fe
