#include "frontend_assets.hpp"

#include "frontend_internal.hpp"

#include "../../assets/ini_config.hpp"
#include "../../assets/package_manager.hpp"
#include "../../assets/texture_loader.hpp"
#include "../../assets/ue3_props.hpp"
#include "../../assets/upk_loader.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

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
    if (!decode_texture_mip(tex.mips.front(), tex.format, out)) return false;
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

// The value of `key` in an ini struct literal: (Key="value",Other="...").
std::string struct_field(const std::string& text, const std::string& key) {
    const size_t at = text.find(key + "=");
    if (at == std::string::npos) return {};
    size_t start = at + key.size() + 1;
    if (start < text.size() && text[start] == '"') {
        const size_t end = text.find('"', start + 1);
        return end == std::string::npos ? text.substr(start + 1) : text.substr(start + 1, end - start - 1);
    }
    const size_t end = text.find_first_of(",)", start);
    return text.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// "<Images:TdUIResources_CheckpointImages.Level1a_CP1>" -> "TdUIResources_CheckpointImages.Level1a_CP1".
std::string image_markup_path(const std::string& markup) {
    const size_t colon = markup.find(':');
    const size_t close = markup.rfind('>');
    if (colon == std::string::npos || close == std::string::npos || close < colon) return {};
    return markup.substr(colon + 1, close - colon - 1);
}

}  // namespace

bool decode_texture_mip(const TextureMip& mip, TexFormat fmt, Image& out) {
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

std::string Assets::localized(const std::string& path) const {
    auto it = localized_.find(to_lower(path));
    return it == localized_.end() ? std::string() : it->second;
}

const Font* Assets::font(const std::string& name) {
    auto it = fonts_.find(name);
    if (it != fonts_.end()) return it->second.get();
    std::unique_ptr<Font> f;
    if (pm_) {
        if (auto pkg = pm_->load("UI_Fonts_Final")) {
            auto loaded = std::make_unique<Font>();
            if (load_font(*pm_, *pkg, name, viewport_height_, *loaded)) f = std::move(loaded);
        }
    }
    if (!f) warnings.push_back("font " + name + " not found in UI_Fonts_Final.upk");
    return fonts_.emplace(name, std::move(f)).first->second.get();
}

const Image* Assets::image(const std::string& object_path) {
    const std::string key = to_lower(object_path);
    auto it = images_.find(key);
    if (it != images_.end()) return it->second.get();
    std::unique_ptr<Image> img;
    const size_t dot = key.find('.');
    if (pm_ && dot != std::string::npos) {
        if (auto pkg = pm_->load(object_path.substr(0, dot))) {
            int32_t index = pm_->find_export(*pkg, key);
            if (index <= 0) index = pm_->find_export(*pkg, key.substr(dot + 1));
            auto loaded = std::make_unique<Image>();
            if (index > 0 && load_image_export(*pm_, *pkg, index, *loaded)) img = std::move(loaded);
        }
    }
    if (!img) warnings.push_back("texture " + object_path + " could not be read");
    return images_.emplace(key, std::move(img)).first->second.get();
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
    pm_ = std::make_shared<PackageManager>(cooked.string());
    PackageManager& pm = *pm_;
    game_root_ = game_root;
    viewport_height_ = viewport_height;

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
    // Every string a "<Strings:File.Section.Key>" markup can name.
    for (const char* file : {"TdGameUI", "TdGame", "TdGameCredits"}) {
        IniConfig ini;
        if (!ini.load_file(get_localization_path(game_root, std::string(file) + ".int", "INT"))) continue;
        for (const std::string& section : ini.get_section_names()) {
            for (const auto& [key, values] : ini.get_section_keys(section)) {
                if (!values.empty()) localized_[to_lower(std::string(file) + "." + section + "." + key)] = to_latin1(values.front());
            }
        }
    }
    // The chapters: every "[<Id> UIDataProvider_TdMaps]" section of DefaultGame.ini, in file order.
    {
        std::ifstream game_ini(get_config_path(game_root, "DefaultGame.ini"));
        std::string line;
        MapProvider* current = nullptr;
        const std::string suffix = " UIDataProvider_TdMaps]";
        while (std::getline(game_ini, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (!line.empty() && line.front() == '[') {
                current = nullptr;
                if (line.size() > suffix.size() && line.compare(line.size() - suffix.size(), suffix.size(), suffix) == 0) {
                    maps.emplace_back();
                    current = &maps.back();
                    current->id = line.substr(1, line.size() - suffix.size() - 1);
                    const std::string section = "TdGame." + current->id + " UIDataProvider_TdMaps.";
                    current->name = localized(section + "MapName");
                }
                continue;
            }
            const size_t eq = line.find('=');
            if (!current || eq == std::string::npos) continue;
            std::string key = line.substr(0, eq);
            if (!key.empty() && key.front() == '+') key.erase(0, 1);
            const std::string value = line.substr(eq + 1);
            if (key == "FileName") current->file = value;
            else if (key == "LevelEvent") current->level_event = value;
            else if (key == "GameMode") current->game_mode = value;
            else if (key == "Checkpoints") {
                MapCheckpoint cp;
                cp.name = struct_field(value, "CheckpointName");
                cp.image = image_markup_path(struct_field(value, "CheckpointImageMarkup"));
                const std::string text = localized("TdGame." + current->id + " UIDataProvider_TdMaps.Checkpoints[" +
                                                   std::to_string(current->checkpoints.size()) + "]");
                cp.friendly = struct_field(text, "CheckpointFriendlyName");
                cp.description = struct_field(text, "CheckpointDescription");
                current->checkpoints.push_back(std::move(cp));
            }
        }
    }
    // The string lists: tags in DefaultGame.ini, strings at the same index in TdGame.int.
    {
        IniConfig game_ini;
        if (game_ini.load_file(get_config_path(game_root, "DefaultGame.ini"))) {
            const std::vector<std::string> entries = game_ini.get_array("TdGame.UIDataStore_TdStringList", "StringData");
            for (size_t k = 0; k < entries.size(); ++k) {
                StringListData list;
                list.tag = struct_field(entries[k], "Tag");
                list.default_index = std::atoi(struct_field(entries[k], "DefaultValueIndex").c_str());
                const std::string text = localized("TdGame.UIDataStore_TdStringList.StringData[" + std::to_string(k) + "]");
                size_t at = text.find("Strings=(");
                while (at != std::string::npos) {
                    const size_t open = text.find('"', at);
                    const size_t close = open == std::string::npos ? std::string::npos : text.find('"', open + 1);
                    if (close == std::string::npos) break;
                    list.strings.push_back(text.substr(open + 1, close - open - 1));
                    at = close + 1;
                }
                string_lists.push_back(std::move(list));
            }
        }
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
    kismet.load(*menu_map, warnings);
    load_city(pm, menu_map, city, warnings);
    return true;
}

}  // namespace me::fe
