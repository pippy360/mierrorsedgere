#include "frontend_assets.hpp"

#include "../../assets/ini_config.hpp"
#include "../../assets/package_manager.hpp"
#include "../../assets/texture_loader.hpp"
#include "../../assets/ue3_props.hpp"
#include "../../assets/upk_loader.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace me::fe {

namespace {

namespace fs = std::filesystem;

// --- textures -----------------------------------------------------------------------------

void rgb565(uint16_t c, uint8_t out[3]) {
    out[0] = static_cast<uint8_t>(((c >> 11) & 31) * 255 / 31);
    out[1] = static_cast<uint8_t>(((c >> 5) & 63) * 255 / 63);
    out[2] = static_cast<uint8_t>((c & 31) * 255 / 31);
}

// The 4x4 colour block shared by DXT1/3/5. `dxt1` enables the 3-colour + transparent mode.
void dxt_color_block(const uint8_t* b, bool dxt1, uint8_t out[16][4]) {
    uint16_t c0 = static_cast<uint16_t>(b[0] | (b[1] << 8));
    uint16_t c1 = static_cast<uint16_t>(b[2] | (b[3] << 8));
    uint8_t pal[4][4];
    rgb565(c0, pal[0]);
    rgb565(c1, pal[1]);
    pal[0][3] = pal[1][3] = pal[2][3] = pal[3][3] = 255;
    if (!dxt1 || c0 > c1) {
        for (int k = 0; k < 3; ++k) {
            pal[2][k] = static_cast<uint8_t>((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = static_cast<uint8_t>((pal[0][k] + 2 * pal[1][k]) / 3);
        }
    } else {
        for (int k = 0; k < 3; ++k) {
            pal[2][k] = static_cast<uint8_t>((pal[0][k] + pal[1][k]) / 2);
            pal[3][k] = 0;
        }
        pal[3][3] = 0;
    }
    uint32_t bits = static_cast<uint32_t>(b[4]) | (static_cast<uint32_t>(b[5]) << 8) |
                    (static_cast<uint32_t>(b[6]) << 16) | (static_cast<uint32_t>(b[7]) << 24);
    for (int i = 0; i < 16; ++i) std::memcpy(out[i], pal[(bits >> (2 * i)) & 3], 4);
}

void dxt5_alpha_block(const uint8_t* b, uint8_t out[16]) {
    uint8_t a[8];
    a[0] = b[0];
    a[1] = b[1];
    if (a[0] > a[1]) {
        for (int k = 1; k < 7; ++k) a[k + 1] = static_cast<uint8_t>(((7 - k) * a[0] + k * a[1]) / 7);
    } else {
        for (int k = 1; k < 5; ++k) a[k + 1] = static_cast<uint8_t>(((5 - k) * a[0] + k * a[1]) / 5);
        a[6] = 0;
        a[7] = 255;
    }
    uint64_t bits = 0;
    for (int k = 0; k < 6; ++k) bits |= static_cast<uint64_t>(b[2 + k]) << (8 * k);
    for (int i = 0; i < 16; ++i) out[i] = a[(bits >> (3 * i)) & 7];
}

bool decode_mip(const TextureMip& mip, TexFormat fmt, Image& out) {
    const int w = mip.width, h = mip.height;
    if (w <= 0 || h <= 0 || mip.data.size() < texture_mip_bytes(fmt, w, h)) return false;
    out.w = w;
    out.h = h;
    out.px.assign(static_cast<size_t>(w) * h * 4, 255);
    const uint8_t* src = mip.data.data();
    if (fmt == TexFormat::BGRA8) {
        for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
            out.px[i * 4 + 0] = src[i * 4 + 2];
            out.px[i * 4 + 1] = src[i * 4 + 1];
            out.px[i * 4 + 2] = src[i * 4 + 0];
            out.px[i * 4 + 3] = src[i * 4 + 3];
        }
        return true;
    }
    if (fmt == TexFormat::G8) {
        for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
            out.px[i * 4 + 0] = out.px[i * 4 + 1] = out.px[i * 4 + 2] = src[i];
        }
        return true;
    }
    if (fmt != TexFormat::DXT1 && fmt != TexFormat::DXT3 && fmt != TexFormat::DXT5) return false;
    const size_t block = fmt == TexFormat::DXT1 ? 8 : 16;
    const int bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
            const uint8_t* b = src + (static_cast<size_t>(by) * bw + bx) * block;
            uint8_t rgba[16][4];
            uint8_t alpha[16];
            if (fmt == TexFormat::DXT1) {
                dxt_color_block(b, true, rgba);
                for (int i = 0; i < 16; ++i) alpha[i] = rgba[i][3];
            } else {
                dxt_color_block(b + 8, false, rgba);
                if (fmt == TexFormat::DXT5) {
                    dxt5_alpha_block(b, alpha);
                } else {
                    for (int i = 0; i < 16; ++i) alpha[i] = static_cast<uint8_t>(((b[i / 2] >> ((i & 1) * 4)) & 15) * 17);
                }
            }
            for (int i = 0; i < 16; ++i) {
                const int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x >= w || y >= h) continue;
                uint8_t* d = &out.px[(static_cast<size_t>(y) * w + x) * 4];
                d[0] = rgba[i][0];
                d[1] = rgba[i][1];
                d[2] = rgba[i][2];
                d[3] = alpha[i];
            }
        }
    }
    return true;
}

int32_t find_export(const UPKPackage& pkg, const std::string& name, const char* cls_a, const char* cls_b = nullptr) {
    const std::string want = to_lower(name);
    const auto& exports = pkg.get_exports();
    for (size_t i = 0; i < exports.size(); ++i) {
        if (to_lower(export_object_name(pkg, static_cast<int32_t>(i + 1))) != want) continue;
        const std::string cls = pkg.get_export_class(exports[i]);
        if (cls == cls_a || (cls_b && cls == cls_b)) return static_cast<int32_t>(i + 1);
    }
    return 0;
}

bool load_image_export(PackageManager& pm, const UPKPackage& pkg, int32_t index, Image& out) {
    if (index <= 0) return false;
    SceneTexture tex;
    if (!load_texture2d(pm, pkg, index, 4096, tex, nullptr) || tex.mips.empty()) return false;
    if (!decode_mip(tex.mips.front(), tex.format, out)) return false;
    out.wrap_x = tex.address_x != TexAddress::Clamp;
    out.wrap_y = tex.address_y != TexAddress::Clamp;
    out.srgb = tex.srgb;
    return true;
}

bool load_image(PackageManager& pm, const UPKPackage& pkg, const std::string& name, Image& out) {
    return load_image_export(pm, pkg, find_export(pkg, name, "Texture2D"), out);
}

// --- fonts --------------------------------------------------------------------------------

float bits_to_float(int32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

bool load_font(PackageManager& pm, const UPKPackage& pkg, const std::string& name, int viewport_height, Font& out) {
    const int32_t index = find_export(pkg, name, "MultiFont", "Font");
    if (index <= 0) return false;
    UPropertyList props;
    const size_t tail = parse_export_properties(pkg, index, props);
    const UProperty* chars = find_prop(props, "Characters");
    const UProperty* textures = find_prop(props, "Textures");
    if (!chars || !textures || chars->array_count < 256) return false;

    const auto& data = pkg.get_data();
    const int tiers = chars->array_count / 256;

    // UMultiFont::GetResolutionPageIndex: the tier whose height is nearest the viewport's.
    std::vector<float> heights;
    if (const UProperty* table = find_prop(props, "ResolutionTestTable")) {
        for (int32_t v : table->ints) heights.push_back(bits_to_float(v));
    }
    int tier = 0;
    float tier_height = static_cast<float>(viewport_height);
    if (static_cast<int>(heights.size()) >= tiers && tiers > 1) {
        float best = 1.0e9f;
        for (int k = 0; k < tiers; ++k) {
            const float d = std::fabs(heights[k] - static_cast<float>(viewport_height));
            if (d < best) {
                best = d;
                tier = k;
            }
        }
        tier_height = heights[tier];
    }

    out = Font{};
    out.name = name;
    out.scale = static_cast<float>(viewport_height) / tier_height;
    out.spacing = prop_int(props, "Kerning", 0);

    const size_t base = chars->value_offset + 4 + static_cast<size_t>(tier) * 256 * 21;
    if (base + 256 * 21 > data.size()) return false;
    std::vector<int> used_pages;
    for (int c = 0; c < 256; ++c) {
        const uint8_t* p = data.data() + base + static_cast<size_t>(c) * 21;
        Glyph g;
        std::memcpy(&g.u, p + 0, 4);
        std::memcpy(&g.v, p + 4, 4);
        std::memcpy(&g.w, p + 8, 4);
        std::memcpy(&g.h, p + 12, 4);
        g.page = p[16];
        std::memcpy(&g.voff, p + 17, 4);
        out.glyphs[static_cast<size_t>(c)] = g;
        // UFont's MaxCharHeight is the tallest VSize; the space glyph carries the full line.
        out.line_height = std::max(out.line_height, g.h);
        if (g.w > 0 && g.h > 0) used_pages.push_back(g.page);
    }

    out.pages.resize(textures->ints.size());
    std::sort(used_pages.begin(), used_pages.end());
    used_pages.erase(std::unique(used_pages.begin(), used_pages.end()), used_pages.end());
    for (int page : used_pages) {
        if (page < 0 || static_cast<size_t>(page) >= textures->ints.size()) return false;
        if (!load_image_export(pm, pkg, textures->ints[static_cast<size_t>(page)], out.pages[static_cast<size_t>(page)])) {
            return false;
        }
    }

    // Native tail: CharRemap count, then N kerning pairs {u16 first, u16 second, float}, then an
    // int32 array of offsets that splits those pairs between the tiers.
    const auto& exp = pkg.get_exports()[static_cast<size_t>(index - 1)];
    const size_t end = static_cast<size_t>(exp.serial_offset) + static_cast<size_t>(exp.serial_size);
    auto rd_i32 = [&](size_t off) {
        int32_t v = 0;
        if (off + 4 <= end) std::memcpy(&v, data.data() + off, 4);
        return v;
    };
    if (tail + 8 <= end && rd_i32(tail) == 0) {
        const int32_t n = rd_i32(tail + 4);
        const size_t pairs = tail + 8;
        const size_t after = pairs + static_cast<size_t>(std::max(n, 0)) * 8;
        if (n > 0 && after + 4 <= end) {
            const int32_t splits = rd_i32(after);
            int32_t first = 0, last = n;
            if (splits == tiers + 1 && after + 4 + static_cast<size_t>(splits) * 4 <= end) {
                first = rd_i32(after + 4 + static_cast<size_t>(tier) * 4);
                last = rd_i32(after + 4 + static_cast<size_t>(tier + 1) * 4);
            }
            for (int32_t k = std::max(first, 0); k < std::min(last, n); ++k) {
                const uint8_t* p = data.data() + pairs + static_cast<size_t>(k) * 8;
                uint16_t a, b;
                float amount;
                std::memcpy(&a, p, 2);
                std::memcpy(&b, p + 2, 2);
                std::memcpy(&amount, p + 4, 4);
                if (a < 256 && b < 256) out.pairs[(static_cast<uint32_t>(a) << 16) | b] = amount;
            }
        }
    }
    return out.valid();
}

// --- scenes -------------------------------------------------------------------------------

// Every widget under the scene `scene_name` that carries a Position, keyed by its object name.
void load_scene_rects(const UPKPackage& pkg, const std::string& scene_name, const char* scene_class,
                      std::unordered_map<std::string, Rect>& out) {
    const int32_t scene = find_export(pkg, scene_name, scene_class);
    if (scene <= 0) return;
    const auto& exports = pkg.get_exports();
    for (size_t i = 0; i < exports.size(); ++i) {
        int32_t outer = exports[i].outer_index;
        int guard = 0;
        while (outer > 0 && outer != scene && guard++ < 32) outer = exports[static_cast<size_t>(outer - 1)].outer_index;
        if (outer != scene) continue;
        const std::string cls = pkg.get_export_class(exports[i]);
        if (cls.rfind("UI", 0) != 0 && cls.rfind("TdUI", 0) != 0) continue;
        if (cls.rfind("UIState", 0) == 0 || cls.rfind("UIEvent", 0) == 0 || cls.rfind("UIAction", 0) == 0 ||
            cls.rfind("UIComp", 0) == 0 || cls == "UISequence" || cls == "UITexture") {
            continue;
        }
        UPropertyList props;
        parse_export_properties(pkg, static_cast<int32_t>(i + 1), props);
        const UProperty* pos = find_prop(props, "Position");
        if (!pos) continue;
        float v[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        bool pixels = true;
        for (const UProperty& f : pos->fields) {
            if (f.array_index < 0 || f.array_index > 3) continue;
            if (f.name == "Value") v[f.array_index] = f.f;
            // The scenes were saved at 1280x720 with every docked face resolved to viewport
            // pixels. A face still in its default (percentage) form is not one we can place.
            if (f.name == "ScaleType" && f.s != "EVALPOS_PixelViewport") pixels = false;
        }
        int scale_types = 0;
        for (const UProperty& f : pos->fields) scale_types += f.name == "ScaleType" ? 1 : 0;
        if (!pixels || scale_types < 4) continue;
        out[export_object_name(pkg, static_cast<int32_t>(i + 1))] = Rect{v[0], v[1], v[2], v[3]};
    }
}

// --- Matinee ------------------------------------------------------------------------------

CurveMode curve_mode(const std::string& s) {
    if (s == "CIM_CurveAuto" || s == "CIM_CurveAutoClamped") return CurveMode::CurveAuto;
    if (s == "CIM_Constant") return CurveMode::Constant;
    if (s == "CIM_CurveUser") return CurveMode::CurveUser;
    if (s == "CIM_CurveBreak") return CurveMode::CurveBreak;
    return CurveMode::Linear;
}

Vec3 curve_value(const UProperty& p) {
    if (p.type == "FloatProperty") return Vec3{p.f, 0.0f, 0.0f};
    return Vec3{p.v[0], p.v[1], p.v[2]};
}

void read_curve(const UPropertyList& track, const std::string& name, Curve& out) {
    const UProperty* curve = find_prop(track, name);
    if (!curve) return;
    const UProperty* points = find_prop(curve->fields, "Points");
    if (!points) return;
    for (const auto& el : points->elements) {
        CurveKey k;
        for (const UProperty& f : el) {
            if (f.name == "InVal") k.t = f.f;
            else if (f.name == "OutVal") k.v = curve_value(f);
            else if (f.name == "ArriveTangent") k.arrive = curve_value(f);
            else if (f.name == "LeaveTangent") k.leave = curve_value(f);
            else if (f.name == "InterpMode") k.mode = curve_mode(f.s);
        }
        out.keys.push_back(k);
    }
}

bool load_matinee(const UPKPackage& pkg, const std::string& interp_data, Matinee& out) {
    const int32_t index = find_export(pkg, interp_data, "InterpData");
    if (index <= 0) return false;
    UPropertyList props;
    parse_export_properties(pkg, index, props);
    out = Matinee{};
    out.name = interp_data;
    out.length = prop_float(props, "InterpLength", 0.0f);
    const UProperty* groups = find_prop(props, "InterpGroups");
    if (!groups) return false;
    for (int32_t g : groups->ints) {
        if (g <= 0) continue;
        UPropertyList gp;
        parse_export_properties(pkg, g, gp);
        const std::string group_name = prop_name(gp, "GroupName");
        const UProperty* tracks = find_prop(gp, "InterpTracks");
        if (!tracks) continue;
        for (int32_t t : tracks->ints) {
            if (t <= 0) continue;
            const std::string cls = object_class_name(pkg, t);
            UPropertyList tp;
            parse_export_properties(pkg, t, tp);
            if (cls == "InterpTrackMove") {
                read_curve(tp, "PosTrack", group_name == "Camera" ? out.camera : out.target);
            } else if (cls == "InterpTrackFloatProp" && group_name == "Camera" && prop_name(tp, "PropertyName") == "FOVAngle") {
                read_curve(tp, "FloatTrack", out.fov);
            } else if (cls == "InterpTrackEvent") {
                if (const UProperty* ev = find_prop(tp, "EventTrack")) {
                    for (const auto& el : ev->elements) {
                        out.events.emplace_back(prop_float(el, "Time", 0.0f), prop_name(el, "EventName"));
                    }
                }
            }
        }
    }
    return out.valid();
}

// IniConfig hands back the file's bytes as UTF-8; the fonts are indexed by Latin-1 code.
std::string to_latin1(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if ((c == 0xC2 || c == 0xC3) && i + 1 < s.size() && (static_cast<unsigned char>(s[i + 1]) & 0xC0) == 0x80) {
            out.push_back(static_cast<char>(((c & 0x03) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F)));
            ++i;
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '\r' || out.back() == '\t' || out.back() == '"')) out.pop_back();
    size_t start = 0;
    while (start < out.size() && (out[start] == ' ' || out[start] == '"')) ++start;
    return out.substr(start);
}

}  // namespace

// Defined in frontend_city.cpp.
bool load_city(PackageManager& pm, const std::shared_ptr<UPKPackage>& menu_map, City& out, std::vector<std::string>& warnings);

float Font::advance(unsigned char c, unsigned char next) const {
    float a = static_cast<float>(glyphs[c].w + spacing);
    if (next != 0) {
        auto it = pairs.find((static_cast<uint32_t>(c) << 16) | next);
        if (it != pairs.end()) a += it->second;
    }
    return a * scale;
}

float Font::width(const std::string& s) const {
    float w = 0.0f;
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        // The last glyph contributes its own width, not its spacing.
        if (i + 1 < s.size()) w += advance(c, static_cast<unsigned char>(s[i + 1]));
        else w += static_cast<float>(glyphs[c].w) * scale;
    }
    return w;
}

// FInterpCurve<T>::Eval.
Vec3 Curve::eval(float t, const Vec3& fallback) const {
    if (keys.empty()) return fallback;
    if (keys.size() == 1 || t <= keys.front().t) return keys.front().v;
    if (t >= keys.back().t) return keys.back().v;
    for (size_t i = 1; i < keys.size(); ++i) {
        if (t >= keys[i].t) continue;
        const CurveKey& a = keys[i - 1];
        const CurveKey& b = keys[i];
        const float diff = b.t - a.t;
        if (diff <= 0.0f || a.mode == CurveMode::Constant) return a.v;
        const float s = (t - a.t) / diff;
        if (a.mode == CurveMode::Linear) return a.v + (b.v - a.v) * s;
        // CubicInterp(P0, T0 * Diff, P1, T1 * Diff, Alpha)
        const float s2 = s * s, s3 = s2 * s;
        return a.v * (2.0f * s3 - 3.0f * s2 + 1.0f) + a.leave * (diff * (s3 - 2.0f * s2 + s)) +
               b.arrive * (diff * (s3 - s2)) + b.v * (-2.0f * s3 + 3.0f * s2);
    }
    return keys.back().v;
}

std::string Assets::text(const std::string& section, const std::string& key) const {
    auto it = strings_.find(section + "." + key);
    return it == strings_.end() ? std::string() : it->second;
}

bool Assets::start_rect(const std::string& widget, Rect& out) const {
    auto it = start_rects_.find(widget);
    if (it == start_rects_.end()) return false;
    out = it->second;
    return true;
}

bool Assets::menu_rect(const std::string& widget, Rect& out) const {
    auto it = menu_rects_.find(widget);
    if (it == menu_rects_.end()) return false;
    out = it->second;
    return true;
}

bool Assets::load(const std::string& game_root, int viewport_height, std::string& error) {
    fs::path cooked = fs::path(game_root) / "TdGame" / "CookedPC";
    if (!fs::exists(cooked)) cooked = fs::path(game_root) / "CookedPC";
    if (!fs::exists(cooked)) {
        error = "no TdGame/CookedPC under " + game_root;
        return false;
    }
    PackageManager pm(cooked.string());

    auto open = [&](const fs::path& rel) -> std::shared_ptr<UPKPackage> {
        const fs::path p = cooked / rel;
        if (!fs::exists(p)) return nullptr;
        auto pkg = std::make_shared<UPKPackage>(p.string());
        if (!pkg->is_valid()) return nullptr;
        pm.add_loaded(p.stem().string(), pkg);
        return pkg;
    };

    // Strings and timings.
    IniConfig ui_int;
    if (ui_int.load_file(get_localization_path(game_root, "TdGameUI.int", "INT"))) {
        for (const char* section : {"TdStart", "TdMainMenu", "TdButtonCallouts"}) {
            for (const auto& [key, values] : ui_int.get_section_keys(section)) {
                if (!values.empty()) strings_[std::string(section) + "." + key] = to_latin1(values.front());
            }
        }
    } else {
        warnings.push_back("Localization/INT/TdGameUI.int not found");
    }
    IniConfig ui_ini;
    if (ui_ini.load_file(get_config_path(game_root, "DefaultUI.ini"))) {
        time_till_start_button = ui_ini.get_float("TdGame.TdUIScene_Start", "TimeTillStartButton", time_till_start_button);
        time_till_attract_movie = ui_ini.get_float("TdGame.TdUIScene_Start", "TimeTillAttractMovie", time_till_attract_movie);
    }

    // Fonts.
    auto fonts = open(fs::path("UI") / "UI_Fonts_Final.upk");
    if (!fonts) {
        error = "UI/UI_Fonts_Final.upk is missing or unreadable";
        return false;
    }
    struct FontSlot {
        const char* name;
        Font* font;
    };
    for (const FontSlot& slot : {FontSlot{"Helvetica_Small_Bold", &small_bold}, FontSlot{"Helvetica_Small_Normal", &small_normal},
                                 FontSlot{"Helvetica_Medium_Italic", &medium_italic},
                                 FontSlot{"Helvetica_Headline_Light_Italic", &headline}}) {
        if (!load_font(pm, *fonts, slot.name, viewport_height, *slot.font)) {
            error = std::string("font ") + slot.name + " could not be read from UI_Fonts_Final.upk";
            return false;
        }
    }

    // Scenes.
    auto front_end = open(fs::path("UI") / "TdUI_FrontEnd.upk");
    if (!front_end) {
        error = "UI/TdUI_FrontEnd.upk is missing or unreadable";
        return false;
    }
    load_scene_rects(*front_end, "tdstart", "TdUIScene_Start", start_rects_);
    load_scene_rects(*front_end, "TdMainMenu", "TdUIScene_MainMenu", menu_rects_);
    if (start_rects_.empty() || menu_rects_.empty()) {
        error = "the tdstart / TdMainMenu scenes were not found in TdUI_FrontEnd.upk";
        return false;
    }

    // Textures.
    auto resources = open(fs::path("UI") / "TdUIResources.upk");
    auto menus = open(fs::path("UI") / "UI_Menus.upk");
    auto menu_map = open(fs::path("Maps") / "Menu" / "TdMainMenu.me1");
    if (!menus) {
        error = "UI/UI_Menus.upk is missing or unreadable";
        return false;
    }
    if (!(resources && load_image(pm, *resources, "StartTitleImage", title)) &&
        !(menu_map && load_image(pm, *menu_map, "StartTitleImage", title))) {
        warnings.push_back("TdUIResources.Scene.StartTitleImage not found");
    }
    if (!(resources && load_image(pm, *resources, "button_full", button))) {
        warnings.push_back("TdUIResources.button_full not found");
    }
    struct ImageSlot {
        const char* name;
        Image* image;
    };
    for (const ImageSlot& slot : {ImageSlot{"T_StickMaskLeft_01", &stick_left}, ImageSlot{"T_StickMaskRight_01", &stick_right},
                                  ImageSlot{"T_StickMaskRightShadow_01", &stick_shadow},
                                  ImageSlot{"T_StickMovementTimeline_01", &stick_timeline}}) {
        if (!load_image(pm, *menus, slot.name, *slot.image)) {
            error = std::string("texture ") + slot.name + " could not be read from UI_Menus.upk";
            return false;
        }
    }

    // Cameras and the city.
    if (!menu_map) {
        warnings.push_back("Maps/Menu/TdMainMenu.me1 not found: no background");
        return true;
    }
    // Which InterpData each Kismet branch plays (docs/MAIN_MENU_SYSTEM_RE.md, section 6).
    static const char* const kIntro[4] = {"InterpData_21", "InterpData_36", "InterpData_32", "InterpData_33"};
    static const char* const kLoop[4] = {"InterpData_18", "InterpData_23", "InterpData_27", "InterpData_57"};
    if (!load_matinee(*menu_map, "InterpData_17", opening)) warnings.push_back("opening Matinee InterpData_17 not found");
    for (int i = 0; i < 4; ++i) {
        if (!load_matinee(*menu_map, kIntro[i], intro[static_cast<size_t>(i)])) warnings.push_back(std::string(kIntro[i]) + " not found");
        if (!load_matinee(*menu_map, kLoop[i], loop[static_cast<size_t>(i)])) warnings.push_back(std::string(kLoop[i]) + " not found");
    }
    load_city(pm, menu_map, city, warnings);
    return true;
}

}  // namespace me::fe
