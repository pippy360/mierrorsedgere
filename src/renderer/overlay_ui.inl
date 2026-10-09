// -----------------------------------------------------------------------------
// The 2D overlays every renderer backend draws the same way: the debug HUD, the
// cutscene letterbox and subtitles, and the chapter-select screen. These are the
// member functions that build their draw lists, written once and included inside
// each backend's Impl struct (metal_renderer.mm, d3d11_renderer.cpp).
//
// The including struct provides:
//   types     HUDVertex {position{x, y}, color}, UITexVertex {position, uv, color},
//             UITextureBatch {tex, verts}
//   UIColor / ui_color(r, g, b, a)                        an RGBA value with x, y, z, w
//   UITexture / ui_texture_width(t) / ui_texture_height(t) a nullable GPU texture handle
//   members   width, height, main_menu, selected_chapter, selected_menu_tab,
//             selected_menu_row, opt_sens_pct, opt_fov_deg, opt_fullscreen and the
//             ui_*_tex textures uploaded by ensure_main_menu_loaded()
// -----------------------------------------------------------------------------

    void draw_ui_quad(std::vector<HUDVertex>& verts, float x, float y, float w, float h, UIColor color) {
        HUDVertex v0 = {{x, y}, color};
        HUDVertex v1 = {{x + w, y}, color};
        HUDVertex v2 = {{x + w, y + h}, color};
        HUDVertex v3 = {{x, y + h}, color};
        verts.push_back(v0); verts.push_back(v1); verts.push_back(v2);
        verts.push_back(v0); verts.push_back(v2); verts.push_back(v3);
    }

    // Forward-slanted parallelogram matching Mirror's Edge TdUIScene / StickSlant UI bars
    void draw_ui_skew_quad(std::vector<HUDVertex>& verts, float x, float y, float w, float h,
                           float slant_dx, UIColor color) {
        HUDVertex v0 = {{x + slant_dx, y}, color};
        HUDVertex v1 = {{x + w + slant_dx, y}, color};
        HUDVertex v2 = {{x + w, y + h}, color};
        HUDVertex v3 = {{x, y + h}, color};
        verts.push_back(v0); verts.push_back(v1); verts.push_back(v2);
        verts.push_back(v0); verts.push_back(v2); verts.push_back(v3);
    }

    void add_ui_tex_quad(std::vector<UITextureBatch>& batches, UITexture tex,
                         float x, float y, float w, float h,
                         float u0 = 0.0f, float v0 = 0.0f, float u1 = 1.0f, float v1 = 1.0f,
                         UIColor tint = ui_color(1.0f, 1.0f, 1.0f, 1.0f)) {
        if (!tex) return;
        UITextureBatch* batch = nullptr;
        if (!batches.empty() && batches.back().tex == tex) {
            batch = &batches.back();
        } else {
            batches.push_back(UITextureBatch{tex, {}});
            batch = &batches.back();
        }
        UITexVertex p0 = {{x,     y},     {u0, v0}, tint};
        UITexVertex p1 = {{x + w, y},     {u1, v0}, tint};
        UITexVertex p2 = {{x + w, y + h}, {u1, v1}, tint};
        UITexVertex p3 = {{x,     y + h}, {u0, v1}, tint};
        batch->verts.push_back(p0); batch->verts.push_back(p1); batch->verts.push_back(p2);
        batch->verts.push_back(p0); batch->verts.push_back(p2); batch->verts.push_back(p3);
    }

    // Renders authentic UE3 MultiFont glyph atlases (UI_Fonts_Final.upk: Helvetica_Headline_Thick_Italic,
    // Helvetica_Headline_Light_Italic, Helvetica_Medium_Italic, Helvetica_Small_Bold_Italic).
    float draw_multifont_text(std::vector<UITextureBatch>& tex_batches,
                              std::vector<HUDVertex>& fallback_verts,
                              const UIMultiFont& font,
                              const std::vector<UITexture>& pages,
                              const std::string& text,
                              float start_x,
                              float start_y,
                              float target_px_height,
                              UIColor color,
                              bool drop_shadow = true) {
        if (!font.valid() || pages.empty()) {
            draw_ui_text_italic(fallback_verts, text, start_x, start_y,
                                std::max(1.0f, target_px_height / 9.0f), color, drop_shadow);
            return static_cast<float>(text.size()) * target_px_height * 0.6f;
        }

        const float scale = target_px_height / std::max(12.0f, font.base_line_height);
        const float space_advance = target_px_height * 0.28f;
        const float tracking = target_px_height * 0.025f;

        auto emit_pass = [&](float ox, float oy, UIColor pass_col) -> float {
            float pen_x = start_x + ox;
            float pen_y = start_y + oy;
            for (unsigned char ch : text) {
                if (ch == ' ') {
                    pen_x += space_advance;
                    continue;
                }
                if (ch < 32) continue;
                const UIFontGlyph& g = font.glyphs[ch];
                if (g.w <= 0 || g.h <= 0 || g.page >= pages.size() || !pages[g.page]) {
                    pen_x += space_advance * 0.8f;
                    continue;
                }
                UITexture tex = pages[g.page];
                float tw = ui_texture_width(tex);
                float th = ui_texture_height(tex);
                float u0 = static_cast<float>(g.u) / tw;
                float v0 = static_cast<float>(g.v) / th;
                float u1 = static_cast<float>(g.u + g.w) / tw;
                float v1 = static_cast<float>(g.v + g.h) / th;

                float gw = static_cast<float>(g.w) * scale;
                float gh = static_cast<float>(g.h) * scale;
                float gy = pen_y + static_cast<float>(g.v_offset) * scale;

                // Negative alpha triggers UI alpha-mask tinting (s.a * -tint.w) in ui_tex_fragment
                UIColor mask_tint = ui_color(pass_col.x, pass_col.y, pass_col.z, -pass_col.w);
                add_ui_tex_quad(tex_batches, tex, pen_x, gy, gw, gh, u0, v0, u1, v1, mask_tint);
                pen_x += gw + tracking;
            }
            return pen_x - (start_x + ox);
        };

        if (drop_shadow) {
            float sh_off = std::max(1.2f, target_px_height * 0.055f);
            UIColor sh_col = ui_color(0.03f, 0.05f, 0.09f, color.w * 0.65f);
            emit_pass(sh_off, sh_off, sh_col);
        }
        return emit_pass(0.0f, 0.0f, color);
    }

    void draw_ui_text_raw(std::vector<HUDVertex>& verts, const std::string& text, float start_x, float start_y,
                          float scale, UIColor color, float italic_shear = 0.0f) {
        float cur_x = start_x;
        float cur_y = start_y;
        float char_w = 5.0f * scale;
        float char_h = 7.0f * scale;
        float spacing = 1.15f * scale;

        for (char ch : text) {
            if (ch == '\n') {
                cur_x = start_x;
                cur_y += char_h + 3.0f * scale;
                continue;
            }
            if (ch < 32 || ch > 126) ch = '?';
            int idx = ch - 32;

            for (int col = 0; col < 5; ++col) {
                uint8_t line = FONT_5X7[idx][col];
                for (int row = 0; row < 7; ++row) {
                    if ((line >> row) & 1) {
                        float shear_x = (6.0f - float(row)) * scale * italic_shear;
                        float px = cur_x + col * scale + shear_x;
                        float py = cur_y + row * scale;
                        draw_ui_quad(verts, px, py, scale, scale, color);
                    }
                }
            }
            cur_x += char_w + spacing;
        }
    }

    void draw_ui_text(std::vector<HUDVertex>& verts, const std::string& text, float start_x, float start_y,
                      float scale, UIColor color) {
        // High-contrast dark drop shadow for 100% legibility over bright sky & white rooftops
        UIColor shadow_col = ui_color(0.02f, 0.03f, 0.05f, color.w * 0.85f);
        draw_ui_text_raw(verts, text, start_x + 1.5f, start_y + 1.5f, scale, shadow_col, 0.0f);
        draw_ui_text_raw(verts, text, start_x, start_y, scale, color, 0.0f);
    }

    // Forward-slanted italic sans-serif typography matching UI_Fonts_Final.Menus.Fonts_Positec
    void draw_ui_text_italic(std::vector<HUDVertex>& verts, const std::string& text, float start_x, float start_y,
                             float scale, UIColor color, bool dark_shadow = true, float shear = 0.22f) {
        if (dark_shadow) {
            UIColor shadow_col = ui_color(0.02f, 0.03f, 0.05f, color.w * 0.82f);
            draw_ui_text_raw(verts, text, start_x + 1.4f, start_y + 1.4f, scale, shadow_col, shear);
        }
        draw_ui_text_raw(verts, text, start_x, start_y, scale, color, shear);
    }

    void draw_ui_reticle(std::vector<HUDVertex>& verts, float cx, float cy, bool is_target, float pulse) {
        UIColor shadow = ui_color(0.05f, 0.07f, 0.10f, 0.75f);
        UIColor color = is_target ? ui_color(0.902f, 0.078f, 0.078f, 1.0f)
                                      : ui_color(1.0f, 1.0f, 1.0f, 0.92f);

        float r = 2.5f * (is_target ? (1.0f + 0.3f * pulse) : 1.0f);
        draw_ui_quad(verts, cx - r - 1.0f, cy - r - 1.0f, (r + 1.0f) * 2.0f, (r + 1.0f) * 2.0f, shadow);
        draw_ui_quad(verts, cx - r, cy - r, r * 2.0f, r * 2.0f, color);

        float d = 10.0f;
        float len = 5.0f;
        draw_ui_quad(verts, cx - d - len, cy - 1.0f, len, 2.0f, color);
        draw_ui_quad(verts, cx + d, cy - 1.0f, len, 2.0f, color);
        draw_ui_quad(verts, cx - 1.0f, cy - d - len, 2.0f, len, color);
        draw_ui_quad(verts, cx - 1.0f, cy + d, 2.0f, len, color);
    }

    // -------------------------------------------------------------------------
    // Authentic Mirror's Edge Frontend UI (TdMainMenu + TdLoadLevel)
    // Layout & Typography faithful to TdUI_FrontEnd.upk & UI_Fonts_Final.upk
    // -------------------------------------------------------------------------
    void draw_main_menu_ui(std::vector<HUDVertex>& bg_verts,
                           std::vector<UITextureBatch>& tex_batches,
                           std::vector<HUDVertex>& fg_verts,
                           const PlayerTelemetry& telemetry) {
        bg_verts.clear();
        tex_batches.clear();
        fg_verts.clear();

        const float w = float(width);
        const float h = float(height);
        const float sx = w / 1280.0f;
        const float sy = h / 720.0f;

        const UIColor runner_red   = ui_color(0.890f, 0.078f, 0.078f, 0.96f); // #E31414
        const UIColor dark_ink     = ui_color(0.110f, 0.135f, 0.175f, 0.96f);
        const UIColor muted_ink    = ui_color(0.240f, 0.285f, 0.350f, 0.92f);
        const UIColor pure_white   = ui_color(1.000f, 1.000f, 1.000f, 1.00f);
        const UIColor row_strip    = ui_color(0.960f, 0.975f, 0.992f, 0.62f);
        const UIColor col_veil     = ui_color(0.955f, 0.970f, 0.988f, 0.42f);
        const UIColor dark_bar     = ui_color(0.085f, 0.105f, 0.140f, 0.86f);

        const UIMultiFont& f_head_thick = main_menu.headline_thick_font();
        const UIMultiFont& f_head_light = main_menu.headline_light_font();
        const UIMultiFont& f_med_italic = main_menu.medium_italic_font();
        const UIMultiFont& f_sml_italic = main_menu.small_italic_font();

        const int sel = std::clamp(selected_chapter, 0, 9);
        const MenuChapterEntry& cur_ch = main_menu.get_chapter(sel);

        // =====================================================================
        // 1. TOP-LEFT OFFICIAL MIRROR'S EDGE LOGO (StartTitleImage from TdMainMenu.me1)
        // =====================================================================
        const float logo_x = 92.0f * sx;
        const float logo_y = 36.0f * sy;
        const float logo_h = 54.0f * sy;
        const float logo_w = logo_h * 4.0f; // Native 4:1 aspect ratio (256x64 / 512x128)
        if (ui_logo_tex) {
            // Left 24% of StartTitleImage is the iconic Runner Star (tinted Scarlet Red)
            add_ui_tex_quad(tex_batches, ui_logo_tex,
                            logo_x, logo_y, logo_w * 0.24f, logo_h,
                            0.0f, 0.0f, 0.24f, 1.0f,
                            ui_color(0.89f, 0.08f, 0.08f, -1.0f));
            // Right 76% of StartTitleImage is the official MIRROR'S EDGE wordmark
            add_ui_tex_quad(tex_batches, ui_logo_tex,
                            logo_x + logo_w * 0.24f + 1.5f * sx, logo_y + 1.5f * sy, logo_w * 0.76f, logo_h,
                            0.24f, 0.0f, 1.0f, 1.0f,
                            ui_color(1.0f, 1.0f, 1.0f, -0.75f));
            add_ui_tex_quad(tex_batches, ui_logo_tex,
                            logo_x + logo_w * 0.24f, logo_y, logo_w * 0.76f, logo_h,
                            0.24f, 0.0f, 1.0f, 1.0f,
                            ui_color(0.11f, 0.13f, 0.17f, -0.98f));
        }

        // =====================================================================
        // 2. LEFT SAFE-REGION: TAB-SPECIFIC MENU ROWS (STORY / RACE / OPTIONS / EXTRAS)
        // =====================================================================
        const float lx = 96.0f * sx;
        const float ly = 104.0f * sy;
        const float lw = 380.0f * sx;

        const std::string left_heading =
            (selected_menu_tab == 1) ? "SPEED RUN COURSES" :
            (selected_menu_tab == 2) ? "GAME & VIDEO OPTIONS" :
            (selected_menu_tab == 3) ? "EXTRAS & ARCHIVE" :
            config_title_or("LOAD CHAPTER");

        draw_multifont_text(tex_batches, fg_verts, f_head_thick, ui_font_headline_thick_tex,
                            left_heading, lx, ly, 23.0f * sy, dark_ink, false);
        draw_ui_skew_quad(fg_verts, lx - 4.0f * sx, ly + 28.0f * sy, lw, 2.5f * sy, 3.0f * sx, runner_red);

        const float list_top = ly + 38.0f * sy;
        const float row_step = 35.5f * sy;
        const float row_h    = 31.0f * sy;

        auto draw_menu_row = [&](int idx, const std::string& label, bool is_sel) {
            const float ry = list_top + float(idx) * row_step;
            if (is_sel) {
                draw_ui_skew_quad(bg_verts, lx - 8.0f * sx, ry, lw + 18.0f * sx, row_h, 9.0f * sx, runner_red);
                draw_multifont_text(tex_batches, fg_verts, f_head_thick, ui_font_headline_thick_tex,
                                    label, lx + 8.0f * sx, ry + 5.5f * sy, 18.0f * sy, pure_white, true);
            } else {
                draw_ui_skew_quad(bg_verts, lx, ry + 1.5f * sy, lw, row_h - 3.0f * sy, 7.5f * sx, row_strip);
                draw_multifont_text(tex_batches, fg_verts, f_med_italic, ui_font_medium_italic_tex,
                                    label, lx + 8.0f * sx, ry + 6.5f * sy, 15.8f * sy, dark_ink, false);
            }
        };

        if (selected_menu_tab == 0 || selected_menu_tab == 1) {
            // STORY & RACE: 10 Campaign / Speed Run Chapters
            for (int i = 0; i < 10; ++i) {
                const MenuChapterEntry& ch = main_menu.get_chapter(i);
                std::string ch_upper = ch.map_name;
                for (char& c : ch_upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                if (selected_menu_tab == 1) {
                    ch_upper += "   [" + ch.speedrun_target_time + "]";
                }
                draw_menu_row(i, ch_upper, i == sel);
            }
        } else if (selected_menu_tab == 2) {
            // OPTIONS: 6 Interactive Game, Input & Display Settings
            const int rsel = std::clamp(selected_menu_row, 0, 5);
            const std::string opt_rows[6] = {
                "MOUSE SENSITIVITY:   " + std::to_string(opt_sens_pct) + "%",
                "FIELD OF VIEW (FOV): " + std::to_string(opt_fov_deg) + " DEG",
                std::string("DISPLAY MODE:        ") + (opt_fullscreen ? "FULLSCREEN" : "WINDOWED"),
                std::string("REACTION TIME:       ") + (telemetry.reaction_active ? "ACTIVE" : "READY"),
                "RESET TO ACTIVE CHECKPOINT",
                "QUIT TO DESKTOP"
            };
            for (int i = 0; i < 6; ++i) {
                draw_menu_row(i, opt_rows[i], i == rsel);
            }
        } else {
            // EXTRAS: 6 Interactive Cutscene, Weapon & Sandbox Actions
            const int rsel = std::clamp(selected_menu_row, 0, 5);
            static const char* kExtraRows[6] = {
                "PLAY CHAPTER OPENING MOVIE",
                "CYCLE ALL 17 BINK CUTSCENES",
                "PLAY 3D ROOFTOP INTRO FLY-IN",
                "EQUIP RUNNER SIDEARM (M1911)",
                "DEPLOY KRUGERSEC SQUAD AHEAD",
                "ALL 10 CHAPTERS: UNLOCKED"
            };
            for (int i = 0; i < 6; ++i) {
                draw_menu_row(i, kExtraRows[i], i == rsel);
            }
        }

        // =====================================================================
        // 3. RIGHT SAFE-REGION: CHAPTER PREVIEW HALFTONE PHOTO & STATS (TdLoadLevel)
        // =====================================================================
        const float rw = 368.0f * sx;
        const float rx = w - 96.0f * sx - rw;
        const float ry = 104.0f * sy;

        std::string cur_upper = cur_ch.map_name;
        for (char& c : cur_upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        const std::string right_heading =
            (selected_menu_tab == 1) ? ("COURSE: " + cur_upper) :
            (selected_menu_tab == 2) ? "SYSTEM & GRAPHICS" :
            (selected_menu_tab == 3) ? "RUNNER ARCHIVE" :
            cur_upper;

        draw_multifont_text(tex_batches, fg_verts, f_head_thick, ui_font_headline_thick_tex,
                            right_heading, rx, ry, 23.0f * sy, runner_red, false);
        draw_ui_skew_quad(fg_verts, rx - 4.0f * sx, ry + 28.0f * sy, rw, 2.5f * sy, 3.0f * sx, dark_ink);

        std::string supers_flat = cur_ch.district_timestamp;
        for (char& c : supers_flat) {
            if (c == '\n') c = ' ';
        }
        const std::string right_sub =
            (selected_menu_tab == 1) ? "INSTANT START (NO CUTSCENES)" :
            (selected_menu_tab == 2) ? ("APPLE METAL 3.0  •  " + std::to_string(width) + "x" + std::to_string(height)) :
            (selected_menu_tab == 3) ? "17 BINK MOVIES  •  199 SUBTITLES" :
            supers_flat;

        draw_multifont_text(tex_batches, fg_verts, f_head_light, ui_font_headline_light_tex,
                            right_sub, rx + 2.0f * sx, ry + 35.0f * sy, 18.0f * sy, dark_ink, false);

        // Halftone Chapter Preview Photograph (UI/TdUIResources_CheckpointImages.upk)
        const float img_x = rx;
        const float img_y = ry + 64.0f * sy;
        const float img_w = rw;
        const float img_h = 184.0f * sy;
        draw_ui_quad(bg_verts, img_x - 3.0f * sx, img_y - 3.0f * sy, img_w + 6.0f * sx, img_h + 6.0f * sy,
                     ui_color(1.0f, 1.0f, 1.0f, 0.78f));
        if (ui_chapter_tex[sel]) {
            add_ui_tex_quad(tex_batches, ui_chapter_tex[sel],
                            img_x, img_y, img_w, img_h,
                            0.0f, 0.04f, 1.0f, 0.98f, pure_white);
        }

        // Compact Speed Run Time & Runner Bags Stats Strip (TdLoadLevel.LevelStatsPanel)
        const float st_y = img_y + img_h + 12.0f * sy;
        const float st_h = 56.0f * sy;
        draw_ui_skew_quad(bg_verts, rx, st_y, rw, st_h, 12.0f * sx, dark_bar);
        draw_ui_skew_quad(fg_verts, rx, st_y, 4.0f * sx, st_h, 12.0f * sx, runner_red);

        if (ui_time_tex) {
            add_ui_tex_quad(tex_batches, ui_time_tex,
                            rx + 18.0f * sx, st_y + 10.0f * sy, 34.0f * sy, 34.0f * sy,
                            0.0f, 0.0f, 1.0f, 1.0f,
                            ui_color(1.0f, 1.0f, 1.0f, -0.95f));
        }
        draw_multifont_text(tex_batches, fg_verts, f_sml_italic, ui_font_small_italic_tex,
                            main_menu.config().speed_run_time_label,
                            rx + 58.0f * sx, st_y + 9.0f * sy, 12.5f * sy,
                            ui_color(0.76f, 0.81f, 0.88f, 0.95f), true);
        draw_multifont_text(tex_batches, fg_verts, f_head_thick, ui_font_headline_thick_tex,
                            cur_ch.speedrun_target_time,
                            rx + 58.0f * sx, st_y + 24.0f * sy, 20.0f * sy, pure_white, true);

        const float bag_x = rx + rw * 0.56f;
        if (ui_bag_tex) {
            add_ui_tex_quad(tex_batches, ui_bag_tex,
                            bag_x, st_y + 10.0f * sy, 34.0f * sy, 34.0f * sy,
                            0.0f, 0.0f, 1.0f, 1.0f,
                            ui_color(1.0f, 0.84f, 0.16f, -0.98f));
        }
        const int bags_found = std::clamp(telemetry.bags_collected, 1, 3);
        draw_multifont_text(tex_batches, fg_verts, f_sml_italic, ui_font_small_italic_tex,
                            main_menu.config().bags_found_label,
                            bag_x + 42.0f * sx, st_y + 9.0f * sy, 12.5f * sy,
                            ui_color(0.76f, 0.81f, 0.88f, 0.95f), true);
        std::string bag_str = std::to_string(bags_found) + " / 3";
        draw_multifont_text(tex_batches, fg_verts, f_head_thick, ui_font_headline_thick_tex,
                            bag_str, bag_x + 42.0f * sx, st_y + 24.0f * sy, 20.0f * sy,
                            ui_color(1.0f, 0.85f, 0.18f, 1.0f), true);

        // =====================================================================
        // 4. LOWER-THIRD 4 ICONIC MIRROR'S EDGE CATEGORY COLUMNS (TdMainMenu)
        //    Exact X coordinates from TdUI_FrontEnd.upk: 96, 368, 640, 912 .. 1184
        // =====================================================================
        const float nav_y = 528.0f * sy;
        const float nav_h = 62.0f * sy;
        const float col_w = 272.0f * sx;
        static const char* kNavCaptions[4] = {"STORY", "RACE", "OPTIONS", "EXTRAS"};

        for (int c = 0; c < 4; ++c) {
            const float cx = (96.0f + float(c) * 272.0f) * sx;
            const bool active = (c == selected_menu_tab);
            draw_ui_skew_quad(bg_verts, cx, nav_y, col_w - 6.0f * sx, nav_h, 14.0f * sx,
                              active ? runner_red : col_veil);
            draw_ui_skew_quad(fg_verts, cx, nav_y, 3.0f * sx, nav_h, 14.0f * sx,
                              active ? pure_white : dark_ink);

            const std::string& cap = (c < static_cast<int>(main_menu.tabs().size()))
                ? main_menu.tabs()[c].caption
                : std::string(kNavCaptions[c]);
            draw_multifont_text(tex_batches, fg_verts,
                                active ? f_head_thick : f_head_light,
                                active ? ui_font_headline_thick_tex : ui_font_headline_light_tex,
                                cap,
                                cx + 22.0f * sx,
                                nav_y + (active ? 16.0f : 18.0f) * sy,
                                (active ? 28.0f : 25.0f) * sy,
                                active ? pure_white : dark_ink,
                                active);
        }

        // =====================================================================
        // 5. BOTTOM SAFE-REGION BUTTON BAR (TdUIButtonBar at y = 635.4..666)
        // =====================================================================
        const float btn_y = 636.0f * sy;
        const float btn_h = 30.0f * sy;

        draw_ui_skew_quad(bg_verts, 628.0f * sx, btn_y, 556.0f * sx, btn_h, 7.0f * sx, dark_bar);
        draw_ui_skew_quad(fg_verts, 628.0f * sx, btn_y, 4.0f * sx, btn_h, 7.0f * sx, runner_red);

        const char* bar_text =
            (selected_menu_tab == 1) ? "[CLICK / ENTER] START SPEED RUN      [ESC] RESUME" :
            (selected_menu_tab == 2) ? "[CLICK / ENTER] CHANGE OPTION        [ESC] RESUME" :
            (selected_menu_tab == 3) ? "[CLICK / ENTER] ACTIVATE EXTRA       [ESC] RESUME" :
                                       "[CLICK / ENTER] PLAY CHAPTER         [ESC] RESUME";

        draw_multifont_text(tex_batches, fg_verts, f_med_italic, ui_font_medium_italic_tex,
                            bar_text,
                            646.0f * sx, btn_y + 6.5f * sy, 15.0f * sy, pure_white, true);
    }

    std::string config_title_or(const char* fallback) const {
        const std::string& t = main_menu.config().load_chapter_title;
        return t.empty() ? std::string(fallback) : t;
    }

    void draw_hud(std::vector<HUDVertex>& verts, const LevelScene& scene, const PlayerTelemetry& telemetry) {
        verts.clear();
        // ME_NO_HUD: the picture alone, for holding against a retail frame (tools/retail/render_check.py).
        static const bool hidden = std::getenv("ME_NO_HUD") != nullptr;
        if (hidden) return;
        float w = float(width);
        float h = float(height);
        float sim_time = telemetry.sim_time;

        // 1. Center Runner Reticle Dot + Dynamic Weapon Crosshair & Hit Marker
        float cx = w * 0.5f;
        float cy = h * 0.5f;
        bool is_interactive_target = telemetry.weapon.equipped ||
                                     telemetry.disarm_prompt_visible ||
                                     (telemetry.speed_2d > 450.0f) ||
                                     (telemetry.move_state == EMovement::MOVE_Snatch);
        draw_ui_reticle(verts, cx, cy, is_interactive_target, std::sin(sim_time * 8.0f));

        if (telemetry.weapon.equipped) {
            float gap = 10.0f + telemetry.weapon.spread_rad * 180.0f + telemetry.weapon.fire_anim_timer * 28.0f;
            float tick_len = (telemetry.weapon.pellet_count > 1) ? 10.0f : 7.0f;
            UIColor xhair_col = (telemetry.weapon.ammo > 0)
                ? ui_color(0.95f, 0.97f, 1.0f, 0.88f)
                : ui_color(0.95f, 0.18f, 0.18f, 0.92f);
            draw_ui_quad(verts, cx - gap - tick_len, cy - 1.0f, tick_len, 2.0f, xhair_col);
            draw_ui_quad(verts, cx + gap,            cy - 1.0f, tick_len, 2.0f, xhair_col);
            draw_ui_quad(verts, cx - 1.0f, cy - gap - tick_len, 2.0f, tick_len, xhair_col);
            draw_ui_quad(verts, cx - 1.0f, cy + gap,            2.0f, tick_len, xhair_col);
        }

        // Uncontrolled falling wind edge vignette & lethal fall impact crimson-to-black screen fade
        if (telemetry.fall_death_impact) {
            float p = std::clamp(telemetry.death_anim_progress, 0.0f, 1.0f);
            float red_flash = (p < 0.25f) ? (1.0f - p / 0.25f) * 0.55f : 0.0f;
            float blackout  = std::clamp((p - 0.12f) / 0.72f, 0.0f, 1.0f);
            if (red_flash > 0.0f) {
                draw_ui_quad(verts, 0.0f, 0.0f, w, h, ui_color(0.82f, 0.04f, 0.04f, red_flash));
            }
            if (blackout > 0.0f) {
                draw_ui_quad(verts, 0.0f, 0.0f, w, h, ui_color(0.0f, 0.0f, 0.0f, blackout));
            }
        } else if (telemetry.falling_to_death) {
            float rush = std::clamp((-telemetry.velocity.z - 1200.0f) / 1400.0f, 0.25f, 0.85f);
            float edge_w = w * 0.14f;
            float edge_h = h * 0.16f;
            UIColor vig = ui_color(0.02f, 0.02f, 0.04f, rush * 0.55f);
            draw_ui_quad(verts, 0.0f, 0.0f, w, edge_h, vig);
            draw_ui_quad(verts, 0.0f, h - edge_h, w, edge_h, vig);
            draw_ui_quad(verts, 0.0f, edge_h, edge_w, h - 2.0f * edge_h, vig);
            draw_ui_quad(verts, w - edge_w, edge_h, edge_w, h - 2.0f * edge_h, vig);
        }

        if (telemetry.hit_marker_timer > 0.0f) {
            float alpha = std::clamp(telemetry.hit_marker_timer / 0.22f, 0.0f, 1.0f);
            UIColor hm_col = ui_color(0.96f, 0.14f, 0.14f, alpha);
            for (int d = 5; d <= 12; d += 2) {
                float fd = static_cast<float>(d);
                draw_ui_quad(verts, cx - fd - 1.5f, cy - fd - 1.5f, 3.0f, 3.0f, hm_col);
                draw_ui_quad(verts, cx + fd - 1.5f, cy - fd - 1.5f, 3.0f, 3.0f, hm_col);
                draw_ui_quad(verts, cx - fd - 1.5f, cy + fd - 1.5f, 3.0f, 3.0f, hm_col);
                draw_ui_quad(verts, cx + fd - 1.5f, cy + fd - 1.5f, 3.0f, 3.0f, hm_col);
            }
        }

        if (telemetry.disarm_prompt_visible) {
            std::string dprompt = "[RIGHT CLICK / E] DISARM WEAPON";
            float dw = float(dprompt.length()) * 11.5f + 28.0f;
            float dx = (w - dw) * 0.5f;
            float dy = cy + 46.0f;
            draw_ui_quad(verts, dx, dy, dw, 26.0f, ui_color(0.88f, 0.06f, 0.06f, 0.90f));
            draw_ui_text(verts, dprompt, dx + 14.0f, dy + 6.0f, 1.8f, ui_color(1.0f, 1.0f, 1.0f, 1.0f));
        }

        // 2. Bottom-Left Telemetry Panel Backdrop (Sleek Translucent Dark Glass + Red Runner Accent)
        draw_ui_quad(verts, 22.0f, h - 164.0f, 340.0f, 112.0f, ui_color(0.04f, 0.06f, 0.09f, 0.72f));
        draw_ui_quad(verts, 22.0f, h - 164.0f, 4.0f, 112.0f, ui_color(0.902f, 0.078f, 0.078f, 0.95f));

        float speed = telemetry.speed_2d;
        float kmh = speed * 0.06f;
        bool at_max_speed = (speed >= 695.0f);
        std::ostringstream ss_spd;
        ss_spd << "SPEED: " << std::fixed << std::setprecision(0) << speed << " u/s ("
               << std::setprecision(1) << kmh << " km/h)";
        if (at_max_speed) ss_spd << " MAX";
        draw_ui_text(verts, ss_spd.str(), 35.0f, h - 88.0f, 2.0f, ui_color(1.0f, 1.0f, 1.0f, 0.98f));

        // Flow momentum bar frame (scaled to 720 u/s top ground speed)
        draw_ui_quad(verts, 35.0f, h - 68.0f, 240.0f, 10.0f, ui_color(0.12f, 0.15f, 0.20f, 0.85f));
        float bar_fill = std::clamp(speed / 720.0f, 0.0f, 1.0f);
        UIColor bar_col = at_max_speed ? ui_color(0.92f, 0.98f, 1.0f, 1.0f)
                            : (speed > 400.0f) ? ui_color(0.902f, 0.078f, 0.078f, 1.0f)
                                               : ui_color(0.30f, 0.78f, 0.98f, 0.95f);
        draw_ui_quad(verts, 37.0f, h - 66.0f, 236.0f * bar_fill, 6.0f, bar_col);

        // 3. Current Parkour Move State
        std::string move_name = move_state_name(telemetry.move_state);
        std::string move_label = "MOVE: " + move_name;
        draw_ui_text(verts, move_label, 35.0f, h - 114.0f, 2.0f, ui_color(0.95f, 0.97f, 1.0f, 0.95f));

        // 4. Health & Reaction Time Meters
        float hp_pct = std::clamp(telemetry.health / 100.0f, 0.0f, 1.0f);
        float rt_pct = std::clamp(telemetry.reaction_energy / 100.0f, 0.0f, 1.0f);

        draw_ui_text(verts, "HEALTH", 35.0f, h - 154.0f, 1.6f, ui_color(0.92f, 0.94f, 0.96f, 0.95f));
        draw_ui_quad(verts, 118.0f, h - 153.0f, 158.0f, 8.0f, ui_color(0.12f, 0.15f, 0.20f, 0.85f));
        draw_ui_quad(verts, 120.0f, h - 151.0f, 154.0f * hp_pct, 4.0f,
                     (hp_pct < 0.35f) ? ui_color(0.902f, 0.078f, 0.078f, 1.0f)
                                      : ui_color(0.96f, 0.96f, 0.98f, 0.95f));

        draw_ui_text(verts, "REACTION", 35.0f, h - 138.0f, 1.6f, ui_color(0.92f, 0.94f, 0.96f, 0.95f));
        draw_ui_quad(verts, 118.0f, h - 137.0f, 158.0f, 8.0f, ui_color(0.12f, 0.15f, 0.20f, 0.85f));
        draw_ui_quad(verts, 120.0f, h - 135.0f, 154.0f * rt_pct, 4.0f, ui_color(0.22f, 0.82f, 1.0f, 0.98f));

        // 5. Top-Right Chapter / Checkpoint / Streaming / Bags / Weapon Panel Backdrop
        float rx = w - 335.0f;
        draw_ui_quad(verts, rx - 14.0f, 18.0f, 332.0f, 122.0f, ui_color(0.04f, 0.06f, 0.09f, 0.72f));
        draw_ui_quad(verts, rx - 14.0f, 18.0f, 332.0f, 3.0f, ui_color(0.902f, 0.078f, 0.078f, 0.95f));

        std::string ch_title = scene.chapter_title.empty() ? "PROLOGUE: THE EDGE" : scene.chapter_title;
        draw_ui_text(verts, ch_title, rx, 30.0f, 2.1f, ui_color(0.95f, 0.18f, 0.18f, 1.0f));

        std::ostringstream ss_cp;
        ss_cp << "CHECKPOINT " << (telemetry.active_checkpoint + 1) << " / "
              << std::max(1, (int)scene.checkpoints.size());
        draw_ui_text(verts, ss_cp.str(), rx, 55.0f, 1.8f, ui_color(0.96f, 0.97f, 0.99f, 0.95f));

        std::ostringstream ss_bags;
        ss_bags << "COURIER BAGS: " << telemetry.bags_collected << " / 3";
        draw_ui_text(verts, ss_bags.str(), rx, 75.0f, 1.8f, ui_color(0.96f, 0.86f, 0.25f, 0.95f));

        std::string wep_str = telemetry.weapon.equipped ? (telemetry.weapon.name + " [" +
                              std::to_string(telemetry.weapon.ammo) + "/" +
                              std::to_string(telemetry.weapon.max_ammo) + "]") : "UNARMED (T/Y GUNS)";
        draw_ui_text(verts, "WEAPON: " + wep_str, rx, 95.0f, 1.8f,
                     telemetry.weapon.equipped ? ui_color(0.95f, 0.22f, 0.22f, 1.0f)
                                               : ui_color(0.82f, 0.86f, 0.92f, 0.92f));

        std::ostringstream ss_str;
        ss_str << "STREAMED SUBLEVELS: " << std::max<int>(telemetry.streamed_sublevel_count, (int)scene.loaded_sublevel_packages.size());
        draw_ui_text(verts, ss_str.str(), rx, 115.0f, 1.7f, ui_color(0.55f, 0.85f, 1.0f, 0.95f));

        // Elevator Transit Indicator when Faith is riding an interactive elevator
        if (telemetry.in_elevator) {
            float ex = (w - 300.0f) * 0.5f;
            float ey = 28.0f;
            draw_ui_quad(verts, ex, ey, 300.0f, 38.0f, ui_color(0.04f, 0.06f, 0.09f, 0.82f));
            draw_ui_quad(verts, ex, ey, 300.0f, 3.0f, ui_color(0.902f, 0.078f, 0.078f, 0.95f));
            draw_ui_text(verts, "ELEVATOR TRANSIT / STREAMING", ex + 18.0f, ey + 8.0f, 1.7f,
                         ui_color(0.96f, 0.97f, 0.99f, 0.98f));
            draw_ui_quad(verts, ex + 18.0f, ey + 24.0f, 264.0f, 6.0f, ui_color(0.16f, 0.20f, 0.26f, 0.9f));
            draw_ui_quad(verts, ex + 18.0f, ey + 24.0f, 264.0f * std::clamp(telemetry.elevator_progress, 0.0f, 1.0f), 6.0f,
                         ui_color(0.902f, 0.078f, 0.078f, 1.0f));
        }

        // 6. Active Subtitle / Tutorial Prompt Banner (Bottom Center). With a level script the slot
        // carries retail's text only (voice-over subtitles); the key legend is the port's own.
        std::string prompt = telemetry.active_subtitle;
        if (prompt.empty() && !scene.script) {
            prompt = "[LMB/F] MELEE/FIRE | [RMB/E] DISARM | [T/Y] CYCLE 11 GUNS | [G] DROP | [H] SPAWN SQUAD";
        }
        if (!prompt.empty()) {
            float banner_w = float(prompt.length()) * 11.0f + 40.0f;
            float banner_x = (w - banner_w) * 0.5f;
            draw_ui_quad(verts, banner_x, h - 42.0f, banner_w, 28.0f, ui_color(0.04f, 0.06f, 0.09f, 0.80f));
            draw_ui_text(verts, prompt, banner_x + 20.0f, h - 35.0f, 1.8f, ui_color(0.98f, 0.98f, 0.98f, 1.0f));
        }
        draw_script_text(verts, telemetry);
    }

    // Wraps `text` into lines of at most `max_chars` at the spaces.
    static std::vector<std::string> wrap_ui_text(const std::string& text, size_t max_chars) {
        std::vector<std::string> lines;
        std::string cur;
        size_t pos = 0;
        while (pos <= text.size()) {
            size_t nl = text.find('\n', pos);
            const std::string para = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            size_t p = 0;
            cur.clear();
            while (p < para.size()) {
                size_t sp = para.find(' ', p);
                const std::string word = para.substr(p, sp == std::string::npos ? std::string::npos : sp - p);
                if (!cur.empty() && cur.size() + 1 + word.size() > max_chars) {
                    lines.push_back(cur);
                    cur.clear();
                }
                cur += (cur.empty() ? "" : " ") + word;
                if (sp == std::string::npos) break;
                p = sp + 1;
            }
            if (!cur.empty()) lines.push_back(cur);
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
        return lines;
    }

    // The text the level's Kismet asks for (docs/GAMEPLAY_SCRIPTING_RE.md, section 4), over the
    // game and over its cutscenes alike:
    //   - the supers (SeqAct_TdSupersMessage): the district and the time of day, lower left, for
    //     the action's Duration (6 s), as the chapter opens;
    //   - a tutorial card (SeqAct_TdTutorialMessage, TdUIScene_TutorialHUDMessage): the training
    //     area's instructions, top centre, until the move is done or the next card replaces it;
    //   - a sign's text (SeqAct_TdTriggerSubtitle, from a Trigger_LOS): bottom centre, 5 s;
    //   - a hint card (SeqAct_TdTriggerSplashHint): "HINT" and the text, the game paused under it;
    //   - the skip prompt of a skippable cutscene (TdPopUps.PopUp4).
    void draw_script_text(std::vector<HUDVertex>& verts, const PlayerTelemetry& telemetry) {
        const float w = float(width);
        const float h = float(height);
        if (!telemetry.supers_text.empty() && telemetry.supers_time_left > 0.0f) {
            const float a = std::clamp(telemetry.supers_time_left / 0.6f, 0.0f, 1.0f);
            std::string upper = telemetry.supers_text;
            for (char& ch : upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            draw_ui_text(verts, upper, w * 0.074f, h * 0.80f, 3.2f, ui_color(1.0f, 1.0f, 1.0f, 0.96f * a));
        }
        if (!telemetry.tutorial_text.empty()) {
            const std::vector<std::string> lines = wrap_ui_text(telemetry.tutorial_text, 70);
            float max_chars = 0.0f;
            for (const std::string& l : lines) max_chars = std::max(max_chars, float(l.size()));
            const float box_w = std::min(w - 80.0f, max_chars * 11.0f + 48.0f);
            const float box_h = 22.0f * float(lines.size()) + 26.0f;
            const float box_x = (w - box_w) * 0.5f;
            const float box_y = h * 0.14f;
            draw_ui_quad(verts, box_x, box_y, box_w, box_h, ui_color(1.0f, 1.0f, 1.0f, 0.86f));
            draw_ui_quad(verts, box_x, box_y, 4.0f, box_h, ui_color(0.902f, 0.078f, 0.078f, 1.0f));
            for (size_t i = 0; i < lines.size(); ++i) {
                draw_ui_text(verts, lines[i], box_x + 22.0f, box_y + 13.0f + 22.0f * float(i), 1.8f, ui_color(0.08f, 0.09f, 0.11f, 1.0f));
            }
        }
        if (!telemetry.sign_text.empty() && telemetry.sign_time_left > 0.0f) {
            const std::vector<std::string> lines = wrap_ui_text(telemetry.sign_text, 80);
            float max_chars = 0.0f;
            for (const std::string& l : lines) max_chars = std::max(max_chars, float(l.size()));
            const float box_w = std::min(w - 80.0f, max_chars * 10.6f + 40.0f);
            const float box_h = 21.0f * float(lines.size()) + 12.0f;
            const float box_x = (w - box_w) * 0.5f;
            const float box_y = h - 100.0f - box_h;
            draw_ui_quad(verts, box_x, box_y, box_w, box_h, ui_color(0.03f, 0.05f, 0.08f, 0.82f));
            for (size_t i = 0; i < lines.size(); ++i) {
                draw_ui_text(verts, lines[i], box_x + 20.0f, box_y + 7.0f + 21.0f * float(i), 1.75f, ui_color(0.99f, 0.99f, 1.0f, 1.0f));
            }
        }
        if (!telemetry.splash_hint_text.empty()) {
            draw_ui_quad(verts, 0.0f, 0.0f, w, h, ui_color(0.0f, 0.0f, 0.0f, 0.55f));
            const std::vector<std::string> lines = wrap_ui_text(telemetry.splash_hint_text, 64);
            const float box_w = std::min(w - 120.0f, 760.0f);
            const float box_h = 22.0f * float(lines.size()) + 96.0f;
            const float box_x = (w - box_w) * 0.5f;
            const float box_y = (h - box_h) * 0.5f;
            draw_ui_quad(verts, box_x, box_y, box_w, box_h, ui_color(1.0f, 1.0f, 1.0f, 0.94f));
            draw_ui_quad(verts, box_x, box_y, box_w, 6.0f, ui_color(0.902f, 0.078f, 0.078f, 1.0f));
            draw_ui_text(verts, telemetry.splash_hint_title, box_x + 24.0f, box_y + 20.0f, 2.6f, ui_color(0.902f, 0.078f, 0.078f, 1.0f));
            for (size_t i = 0; i < lines.size(); ++i) {
                draw_ui_text(verts, lines[i], box_x + 24.0f, box_y + 58.0f + 22.0f * float(i), 1.8f, ui_color(0.08f, 0.09f, 0.11f, 1.0f));
            }
            draw_ui_text(verts, "GAME PAUSED  -  PRESS SPACE TO CONTINUE", box_x + 24.0f, box_y + box_h - 26.0f, 1.5f,
                         ui_color(0.35f, 0.38f, 0.42f, 1.0f));
        }
        if (telemetry.skip_prompt) {
            draw_ui_text(verts, "Press SPACE to skip", w * 0.074f, h * 0.105f, 1.9f, ui_color(0.86f, 0.88f, 0.90f, 0.85f));
        }
    }
