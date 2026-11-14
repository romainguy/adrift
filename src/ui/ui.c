/* SPDX-License-Identifier: MPL-2.0
 * 240x240 RGB565 UI State Machine, Header, Full-Screen Scope & Preset Browser */

#include "../app.h"
#include "ui_internal.h"

static bool compute_group_gate(const adrift_synth_t *s, uint8_t g) {
    for (uint8_t v = 0; v < ADRIFT_MAX_VOICES; v++) {
        if (s->voices[v].owner_group == (int8_t)g &&
            (s->voices[v].key_down || s->voices[v].sustained)) {
            return true;
        }
    }
    return false;
}

static uint32_t compute_header_bbd_width(const adrift_synth_t *s) {
    int32_t pk = 0;
    int8_t  eg = (int8_t)(s->edit_group % ADRIFT_MAX_GROUPS);
    for (uint8_t v = 0; v < ADRIFT_MAX_VOICES; v++) {
        if (s->voices[v].owner_group == eg && s->voices[v].bbd.peak_wet_q15 > pk) {
            pk = s->voices[v].bbd.peak_wet_q15;
        }
    }
    uint32_t w = (uint32_t)((pk * 230) >> 15);
    return (w > 230u) ? 230u : w;
}

static uint32_t compute_voice_pip_sig(const adrift_synth_t *s) {
    uint32_t sig = 0u;
    for (uint8_t v = 0; v < ADRIFT_MAX_VOICES; v++) {
        const adrift_voice_t *vc = &s->voices[v];
        uint32_t st = 0u;
        if (vc->owner_group >= 0 && vc->owner_group < ADRIFT_MAX_GROUPS) {
            const adrift_patch_t *vvp = &s->groups[(uint8_t)vc->owner_group].patch;
            if ((vc->active && vvp->env2.drone_hold) || vc->key_down || vc->sustained) {
                st = 3u;
            } else if (vc->active && vc->env2.stage != ADRIFT_ENV_IDLE) {
                st = 2u;
            } else {
                st = 1u;
            }
        }
        sig |= (st << (v * 2u));
    }
    return sig;
}

static void draw_header_bbd_bar(uint16_t *fb, uint32_t bbd_w, uint16_t accent) {
    fb_fill_rect(fb, 5, 21, 230, 3, COL_BAR_TRACK);
    if (bbd_w > 0u) {
        fb_fill_rect(fb, 5, 21, bbd_w, 3, accent);
    }
}

static void draw_header_cpu_pct(uint16_t *fb, uint8_t cpu_pct) {
    char cbuf[10];
    fb_fill_rect(fb, 171u, 8u, 22u, 7u, COL_HEADER);
    fmt_int(cbuf, cpu_pct, "%");
    fb_draw_str(fb, 171u, 8u, cbuf, COL_MUTED);
}

static uint32_t hash_str12(const char *s) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0u; i < 12u && s[i] != '\0'; i++) {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}

static void draw_header(uint16_t *fb,
                        const adrift_synth_t *s,
                        uint8_t cpu_pct,
                        uint8_t bbd_w,
                        bool in_group_menu,
                        uint8_t toast_slot_plus1) {
    uint8_t g = s->edit_group % ADRIFT_MAX_GROUPS;
    const adrift_group_t       *gp = &s->groups[g];
    const adrift_group_theme_t *th = get_group_theme(g);

    fb_fill_rect(fb, 0, 0, ADRIFT_SCREEN_W, 27, COL_HEADER);
    fb_hline(fb, 0, 27, ADRIFT_SCREEN_W, th->border);

    /* Preset badge [P1..P24], [U1..U24], [INIT], or post-save [SAVE] toast */
    if (toast_slot_plus1 > 0u) {
        char tmsg[14];
        copy_str(tmsg, "SAVED U");
        fmt_int(&tmsg[7], (int32_t)toast_slot_plus1, "");
        fb_fill_rect(fb, 5, 4, 34, 14, COL_WHITE);
        fb_draw_str_centered_spec(fb, 5, 34, 6, "SAVE", COL_BG, &k_font_md);
        fb_draw_str_spec(fb, 43, 6, tmsg, th->accent, &k_font_md);
    } else {
        char pst[6];
        uint32_t pb_w;
        if (s->preset_index == ADRIFT_PRESET_INIT) {
            copy_str(pst, "INIT");
            pb_w = 34u;
        } else if (s->preset_index >= ADRIFT_USER_PRESET_BASE &&
                   s->preset_index < ADRIFT_USER_PRESET_BASE + ADRIFT_NUM_USER_PRESETS) {
            uint8_t u = (uint8_t)(s->preset_index - ADRIFT_USER_PRESET_BASE);
            pst[0] = 'U';
            fmt_int(&pst[1], (int32_t)u + 1, "");
            pb_w = (u >= 9u) ? 28u : 21u;
        } else {
            uint8_t p = (uint8_t)(s->preset_index % ADRIFT_NUM_PRESETS);
            pst[0] = 'P';
            fmt_int(&pst[1], (int32_t)p + 1, "");
            pb_w = (p >= 9u) ? 28u : 21u;
        }
        fb_fill_rect(fb, 5, 4, pb_w, 14, th->accent);
        fb_draw_str_centered_spec(fb, 5, pb_w, 6, pst, COL_BG, &k_font_md);

        const char *pname = s->preset_name;
        /* Skip numeric prefix "01 " since badge already displays P1..P24 */
        if (pname[0] >= '0' && pname[0] <= '9' && pname[1] >= '0' && pname[1] <= '9' && pname[2] == ' ') {
            pname += 3;
        }
        char ptitle[13];
        uint32_t pi = 0u;
        while (pname[pi] && pi < 12u) {
            ptitle[pi] = pname[pi];
            pi++;
        }
        ptitle[pi] = '\0';
        fb_draw_str_spec(fb, 5u + pb_w + 4u, 6, ptitle, COL_WHITE, &k_font_md);
    }

    /* Active Group Note + Gate indicator */
    char nbuf[8];
    fmt_note(nbuf, gp->last_note ? gp->last_note : 60u);
    bool grp_gate = compute_group_gate(s, g);
    uint16_t gate_col = grp_gate ? th->accent : COL_MUTED;
    fb_fill_rect(fb, 134, 8, 5, 6, gate_col);
    fb_draw_str_spec(fb, 142, 6, nbuf, gate_col, &k_font_md);

    /* Octave shift or CPU % */
    if (s->kbd_octave != 0) {
        char obuf[6];
        if (s->kbd_octave > 0) {
            obuf[0] = '+';
            fmt_int(&obuf[1], s->kbd_octave, "");
        } else {
            fmt_int(obuf, s->kbd_octave, "");
        }
        fb_draw_str_spec(fb, 170, 6, obuf, th->accent, &k_font_md);
    } else {
        char cbuf[10];
        fmt_int(cbuf, cpu_pct, "%");
        fb_draw_str(fb, 171, 8, cbuf, COL_MUTED);
    }

    /* Voice Group Badge [G1:3 .. G1:16] (HOME cycles G1..G6; SAVE enters Group Settings) */
    char gtag[8];
    gtag[0] = 'G';
    gtag[1] = (char)('1' + g);
    gtag[2] = ':';
    fmt_int(&gtag[3], gp->num_voices, "");
    if (in_group_menu) {
        fb_fill_rect(fb, 193, 4, 42, 14, COL_WHITE);
        fb_draw_str_centered_spec(fb, 193, 42, 6, gtag, COL_BG, &k_font_md);
    } else if (gp->num_voices > 0u) {
        fb_fill_rect(fb, 193, 4, 42, 14, th->accent);
        fb_draw_str_centered_spec(fb, 193, 42, 6, gtag, COL_BG, &k_font_md);
    } else {
        fb_fill_rect(fb, 193, 4, 42, 14, COL_TAB_OFF);
        fb_rect_outline(fb, 193, 4, 42, 14, th->accent);
        fb_draw_str_centered_spec(fb, 193, 42, 6, gtag, th->accent, &k_font_md);
    }

    /* BBD Resonator energy bar along y = 21..24 */
    fb_fill_rect(fb, 5, 21, 230, 3, COL_BAR_TRACK);
    if (bbd_w > 0u) {
        fb_fill_rect(fb, 5, 21, bbd_w, 3, th->accent);
    }
}

/* Large Full-Screen Visualizations on HOME Screen (Pages 1/6 .. 6/6) */
static void draw_full_scope(uint16_t *fb,
                            adrift_ui_t *ui,
                            const adrift_synth_t *s,
                            bool full_chrome,
                            bool pips_dirty,
                            bool note_dirty,
                            adrift_dirty_t *dirty) {
    (void)note_dirty;
    if ((ui->scope_page % ADRIFT_UI_NUM_HOME_PAGES) != ADRIFT_HOME_SCOPE) {
        draw_home_page_extra(fb, ui, s, full_chrome, pips_dirty, dirty);
        return;
    }

    static const char *const k_scope_modes[3] = { "MONO", "DUAL", "FILL" };
    const adrift_group_t       *gp = adrift_synth_active_group(s);
    const adrift_patch_t       *vp = &gp->patch;
    const adrift_group_theme_t *th = get_group_theme(s->edit_group);
    const uint32_t sx = 5u, sy = 31u, sw = 230u, sh = 204u;
    const uint32_t mid_y   = sy + (sh / 2u);
    const uint32_t qtr_h   = 48u;
    const uint32_t wave_y0 = sy + 20u;
    const uint32_t wave_y1 = sy + sh - 17u;
    uint8_t s_mode = ui->scope_mode[ADRIFT_HOME_SCOPE] % ADRIFT_UI_NUM_HOME_MODES;
    uint8_t zoom   = ui->scope_zoom & 3u;
    int32_t gain   = (int32_t)ui->scope_gain;

    if (full_chrome) {
        fb_fill_rect(fb, 0, 28, ADRIFT_SCREEN_W, 212, COL_BG);
        fb_fill_rect(fb, sx, sy, sw, sh, COL_SCOPE_BG);
        fb_rect_outline(fb, sx, sy, sw, sh, th->border);

        /* Top status strip inside scope ([SCOPE] [1/6] badge + mode tags + voice pips) */
        draw_screen_page_badge(fb, sx, sy + 1u, "SCOPE", 0u, th);
        uint32_t bx = sx + 80u;
        if (gp->assign_mode == ADRIFT_GROUP_SUPER) {
            fb_draw_str_spec(fb, bx, sy + 5u, "SUP", COL_WHITE, &k_font_md);
            bx += 28u;
        } else if (gp->assign_mode == ADRIFT_GROUP_UNISON) {
            fb_draw_str_spec(fb, bx, sy + 5u, "UNI", COL_WHITE, &k_font_md);
            bx += 28u;
        } else if (vp->bbd.invert) {
            fb_draw_str_spec(fb, bx, sy + 5u, "INV", COL_WHITE, &k_font_md);
            bx += 28u;
        }
        if (vp->bbd.feedback > 890) {
            fb_draw_str_spec(fb, bx, sy + 5u, "SELF", th->accent, &k_font_md);
        } else if (vp->env2.drone_hold) {
            fb_draw_str_spec(fb, bx, sy + 5u, "DRONE", COL_WHITE, &k_font_md);
        } else if (vp->env1.loop_mode != ADRIFT_ENV_LOOP_OFF) {
            fb_draw_str_spec(fb, bx, sy + 5u, "LOOP", th->accent, &k_font_md);
        }
        draw_voice_pips(fb, s, sx, sy + 1u, sw);
        fb_hline(fb, sx + 1u, sy + 19u, sw - 2u, COL_SCOPE_GRID);

        /* Bottom 4-knob view control strip (1:ZOOM, 2:GAIN, 3:FREEZE, 4:MODE) */
        draw_home_knob_bar(fb, ui, th, k_scope_modes[s_mode]);

        ui->scope_prev_y0 = (uint8_t)wave_y0;
        ui->scope_prev_y1 = (uint8_t)wave_y1;
    } else if (pips_dirty) {
        fb_fill_rect(fb, sx + sw - 60u, sy + 3u, 58u, 13u, COL_SCOPE_BG);
        draw_voice_pips(fb, s, sx, sy + 1u, sw);
        adrift_dirty_mark_rows(dirty, sy + 3u, 13u);
    }

    /* Compute new waveform y-coordinates & tight vertical bounding box [cur_y0 .. cur_y1] */
    uint8_t  base_idx  = s->scope_wr;
    uint16_t trace_col = th->accent;
    uint32_t trace_w   = sw - 8u;
    uint32_t win_len   = ADRIFT_SCOPE_LEN >> zoom;
    uint32_t start_idx = (base_idx + ADRIFT_SCOPE_LEN - win_len) & (ADRIFT_SCOPE_LEN - 1u);
    uint8_t  py_lut[222];
    uint8_t  pyr_lut[222];
    uint32_t cur_y0 = mid_y;
    uint32_t cur_y1 = mid_y + 1u;

    bool has_lr = false;
    if (s_mode == 1u) {
        uint32_t last = (base_idx + ADRIFT_SCOPE_LEN - 1u) & (ADRIFT_SCOPE_LEN - 1u);
        if (s->scope_ring_l[last] != 0 || s->scope_ring_r[last] != 0) {
            has_lr = true;
        } else {
            for (uint32_t i = 0; i < ADRIFT_SCOPE_LEN; i++) {
                if (s->scope_ring_l[i] != 0 || s->scope_ring_r[i] != 0) {
                    has_lr = true;
                    break;
                }
            }
        }
    }

    for (uint32_t px = 0; px < trace_w; px++) {
        uint32_t pos_q8 = (px * (win_len << 8)) / trace_w;
        uint32_t idx0   = (start_idx + (pos_q8 >> 8)) & (ADRIFT_SCOPE_LEN - 1u);
        uint32_t idx1   = (idx0 + 1u) & (ADRIFT_SCOPE_LEN - 1u);
        int32_t  frac   = (int32_t)(pos_q8 & 0xFFu);

        int32_t  s0 = (s_mode == 1u && has_lr) ? s->scope_ring_l[idx0] : s->scope_ring[idx0];
        int32_t  s1 = (s_mode == 1u && has_lr) ? s->scope_ring_l[idx1] : s->scope_ring[idx1];
        int32_t  samp = (zoom == 0u) ? s0 : (s0 + (((s1 - s0) * frac) >> 8));
        int32_t  dy   = (((samp * 136) >> 15) * gain) / 100;
        uint32_t py   = (uint32_t)dsp_clamp_i32((int32_t)mid_y - dy, (int32_t)wave_y0, (int32_t)(wave_y1 - 1u));
        py_lut[px] = (uint8_t)py;
        if (py < cur_y0) cur_y0 = py;
        if (py + 1u > cur_y1) cur_y1 = py + 1u;

        if (s_mode == 1u) {
            int32_t r0 = has_lr ? s->scope_ring_r[idx0] : s->scope_ring[idx0];
            int32_t r1 = has_lr ? s->scope_ring_r[idx1] : s->scope_ring[idx1];
            int32_t sr = (zoom == 0u) ? r0 : (r0 + (((r1 - r0) * frac) >> 8));
            int32_t dyr = (((sr * 136) >> 15) * gain) / 100;
            uint32_t pyr = (uint32_t)dsp_clamp_i32((int32_t)mid_y - dyr, (int32_t)wave_y0, (int32_t)(wave_y1 - 1u));
            pyr_lut[px] = (uint8_t)pyr;
            if (pyr < cur_y0) cur_y0 = pyr;
            if (pyr + 1u > cur_y1) cur_y1 = pyr + 1u;
        }
    }

    /* Only clear the union of previous frame's wave band and current frame's wave band */
    uint32_t clr_y0 = (ui->scope_prev_y0 < cur_y0) ? ui->scope_prev_y0 : cur_y0;
    uint32_t clr_y1 = (ui->scope_prev_y1 > cur_y1) ? ui->scope_prev_y1 : cur_y1;
    if (clr_y0 < wave_y0) clr_y0 = wave_y0;
    if (clr_y1 > wave_y1) clr_y1 = wave_y1;

    if (!full_chrome) {
        fb_fill_rect(fb, sx + 1u, clr_y0, sw - 2u, clr_y1 - clr_y0 + 1u, COL_SCOPE_BG);
        if (clr_y1 >= wave_y1) {
            fb_hline(fb, sx + 1u, wave_y1, sw - 2u, COL_SCOPE_GRID);
        }
    }

    /* Restore graticule dots inside the cleared band (or all 3 graticules on full_chrome) */
    uint32_t g_top = mid_y - qtr_h;
    uint32_t g_bot = mid_y + qtr_h;
    bool draw_mid = full_chrome || (mid_y >= clr_y0 && mid_y <= clr_y1);
    bool draw_top = full_chrome || (g_top >= clr_y0 && g_top <= clr_y1);
    bool draw_bot = full_chrome || (g_bot >= clr_y0 && g_bot <= clr_y1);
    if (draw_mid || draw_top || draw_bot) {
        for (uint32_t x = sx + 4u; x < sx + sw - 4u; x += 4u) {
            if (draw_mid) fb[mid_y * ADRIFT_SCREEN_W + x] = COL_SCOPE_GRID;
            if ((x & 7u) == 0u) {
                if (draw_top) fb[g_top * ADRIFT_SCREEN_W + x] = COL_SCOPE_GRID;
                if (draw_bot) fb[g_bot * ADRIFT_SCREEN_W + x] = COL_SCOPE_GRID;
            }
        }
    }

    if (s_mode == 2u) {
        /* FILL mode: vertical shaded area to centerline */
        uint16_t fill_col = make_env_fill_col(trace_col);
        for (uint32_t px = 0; px < trace_w; px++) {
            uint32_t py    = py_lut[px];
            uint32_t x_col = sx + 4u + px;
            uint32_t fy0   = (py < mid_y) ? py : mid_y;
            uint32_t fy1   = (py < mid_y) ? mid_y : py;
            for (uint32_t y = fy0; y <= fy1; y++) {
                fb[y * ADRIFT_SCREEN_W + x_col] = fill_col;
            }
        }
    } else if (s_mode == 1u) {
        /* DUAL mode: Right channel trace in COL_WHITE behind Left channel trace */
        uint32_t prev_r = pyr_lut[0];
        for (uint32_t px = 0; px < trace_w; px++) {
            uint32_t pyr   = pyr_lut[px];
            uint32_t x_col = sx + 4u + px;
            uint32_t y0    = (px > 0u && prev_r < pyr) ? prev_r : pyr;
            uint32_t y1    = (px > 0u && prev_r > pyr) ? prev_r : pyr;
            for (uint32_t y = y0; y <= y1; y++) {
                fb[y * ADRIFT_SCREEN_W + x_col] = COL_WHITE;
            }
            prev_r = pyr;
        }
    }

    /* Rasterize thick 2px connected oscilloscope waveform */
    uint32_t prev_py = py_lut[0];
    for (uint32_t px = 0; px < trace_w; px++) {
        uint32_t py    = py_lut[px];
        uint32_t x_col = sx + 4u + px;
        if (px > 0u) {
            uint32_t y0 = prev_py < py ? prev_py : py;
            uint32_t y1 = prev_py < py ? py : prev_py;
            if (y1 + 1u <= wave_y1) y1++;
            for (uint32_t y = y0; y <= y1; y++) {
                fb[y * ADRIFT_SCREEN_W + x_col] = trace_col;
            }
        } else {
            fb[py * ADRIFT_SCREEN_W + x_col] = trace_col;
            if (py + 1u <= wave_y1) {
                fb[(py + 1u) * ADRIFT_SCREEN_W + x_col] = trace_col;
            }
        }
        prev_py = py;
    }

    ui->scope_prev_y0 = (uint8_t)cur_y0;
    ui->scope_prev_y1 = (uint8_t)cur_y1;

    if (full_chrome) {
        adrift_dirty_mark_rows(dirty, 28u, 212u);
    } else {
        adrift_dirty_mark_rows(dirty, clr_y0, clr_y1 - clr_y0 + 1u);
    }
}

static const char k_save_charset[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-+/._";
#define SAVE_CHARSET_COUNT ((int32_t)(sizeof(k_save_charset) - 1u))

static uint8_t preset_bank_count(uint8_t bank) {
    switch (bank % ADRIFT_PRESET_BANK_COUNT) {
        case ADRIFT_PRESET_BANK_USER: return ADRIFT_UI_PRESET_USER_COUNT;
        case ADRIFT_PRESET_BANK_FACT: return ADRIFT_UI_PRESET_FACT_COUNT;
        default:                      return ADRIFT_UI_PRESET_ALL_COUNT;
    }
}

static uint8_t preset_vis_to_logical(uint8_t bank, uint8_t vis_idx) {
    switch (bank % ADRIFT_PRESET_BANK_COUNT) {
        case ADRIFT_PRESET_BANK_USER:
            return (uint8_t)(ADRIFT_UI_PRESET_FACT_COUNT + (vis_idx % ADRIFT_UI_PRESET_USER_COUNT));
        case ADRIFT_PRESET_BANK_FACT:
            return (uint8_t)(vis_idx % ADRIFT_UI_PRESET_FACT_COUNT);
        default:
            return (uint8_t)(vis_idx % ADRIFT_UI_PRESET_ALL_COUNT);
    }
}

static uint8_t preset_logical_to_vis(uint8_t bank, uint8_t logical_id) {
    logical_id %= ADRIFT_UI_PRESET_ALL_COUNT;
    switch (bank % ADRIFT_PRESET_BANK_COUNT) {
        case ADRIFT_PRESET_BANK_USER:
            if (logical_id >= ADRIFT_UI_PRESET_FACT_COUNT) {
                return (uint8_t)(logical_id - ADRIFT_UI_PRESET_FACT_COUNT);
            }
            return 0u;
        case ADRIFT_PRESET_BANK_FACT:
            if (logical_id < ADRIFT_UI_PRESET_FACT_COUNT) {
                return logical_id;
            }
            return 2u;
        default:
            return logical_id;
    }
}

static bool preset_logical_is_empty_user(uint8_t logical_id) {
    logical_id %= ADRIFT_UI_PRESET_ALL_COUNT;
    if (logical_id < ADRIFT_UI_PRESET_FACT_COUNT) {
        return false;
    }
    uint8_t ux = (uint8_t)(logical_id - ADRIFT_UI_PRESET_FACT_COUNT);
    return !adrift_synth_user_preset_is_occupied(ux);
}

static bool preset_logical_is_occ_user(uint8_t logical_id) {
    logical_id %= ADRIFT_UI_PRESET_ALL_COUNT;
    if (logical_id < ADRIFT_UI_PRESET_FACT_COUNT) {
        return false;
    }
    uint8_t ux = (uint8_t)(logical_id - ADRIFT_UI_PRESET_FACT_COUNT);
    return adrift_synth_user_preset_is_occupied(ux);
}

static uint8_t preset_auto_pick_src_grp(uint8_t preset_id, uint8_t preferred_grp) {
    uint8_t pref = preferred_grp % ADRIFT_MAX_GROUPS;
    if (adrift_synth_get_preset_group_voices(preset_id, pref) > 0u) {
        return pref;
    }
    for (uint8_t g = 0u; g < ADRIFT_MAX_GROUPS; g++) {
        if (adrift_synth_get_preset_group_voices(preset_id, g) > 0u) {
            return g;
        }
    }
    return 0u;
}

static void draw_preset_browser(uint16_t *fb, const adrift_ui_t *ui, const adrift_synth_t *s) {
    static const char *const k_bank_names[ADRIFT_PRESET_BANK_COUNT] = { "ALL", "USER", "FACT" };
    uint8_t eg = s->edit_group % ADRIFT_MAX_GROUPS;
    const adrift_group_theme_t *th = get_group_theme(eg);
    const uint32_t sx = 5u, sy = 31u, sw = 230u, sh = 204u;
    uint8_t bank    = ui->preset_bank % ADRIFT_PRESET_BANK_COUNT;
    uint8_t vis_cnt = preset_bank_count(bank);
    uint8_t vis_cur = preset_logical_to_vis(bank, ui->preset_cursor);
    uint8_t cur_log = preset_vis_to_logical(bank, vis_cur);

    fb_fill_rect(fb, 0, 28, ADRIFT_SCREEN_W, 212, COL_BG);
    fb_fill_rect(fb, sx, sy, sw, sh, COL_SCOPE_BG);
    fb_rect_outline(fb, sx, sy, sw, sh, th->border);

    /* Top title strip: PRESETS [01/50] [ALL|USER|FACT] (+ GRP tag) + voice pips */
    fb_draw_str_spec(fb, sx + 6u, sy + 6u, "PRESETS", th->accent, &k_font_md);
    uint8_t disp_idx = (uint8_t)(vis_cur + 1u);
    char idx_buf[8];
    idx_buf[0] = (char)('0' + (disp_idx / 10u));
    idx_buf[1] = (char)('0' + (disp_idx % 10u));
    idx_buf[2] = '/';
    idx_buf[3] = (char)('0' + (vis_cnt / 10u));
    idx_buf[4] = (char)('0' + (vis_cnt % 10u));
    idx_buf[5] = '\0';
    fb_fill_rect(fb, sx + 66u, sy + 5u, 39u, 12u, th->accent);
    fb_draw_str_centered_spec(fb, sx + 66u, 39u, sy + 7u, idx_buf, COL_BG, &k_font_sm_bold);

    fb_fill_rect(fb, sx + 109u, sy + 5u, 34u, 12u, COL_TAB_OFF);
    fb_rect_outline(fb, sx + 109u, sy + 5u, 34u, 12u, bank != 0u ? th->accent : th->border);
    fb_draw_str_centered_spec(fb, sx + 109u, 34u, sy + 7u, k_bank_names[bank],
                              bank != 0u ? th->accent : COL_WHITE, &k_font_sm_bold);
    if (ui->preset_load_grp) {
        fb_draw_str_spec(fb, sx + 147u, sy + 7u, "GRP", th->accent, &k_font_sm_bold);
    }
    draw_voice_pips(fb, s, sx, sy + 1u, sw);
    fb_hline(fb, sx + 1u, sy + 20u, sw - 2u, th->border);

    /* 6-row scrollable list (ALL: INIT SYNTH, INIT GROUP, P1..P24, U1..U24) */
    int32_t top_vis = (int32_t)vis_cur - 2;
    if (top_vis < 0) top_vis = 0;
    if (top_vis > (int32_t)vis_cnt - 6) {
        top_vis = (int32_t)vis_cnt - 6;
    }

    const uint32_t rx = sx + 5u;
    const uint32_t rw = sw - 16u;
    uint8_t orig_pst = ui->preset_snapshot.preset_index;
    uint8_t src_g    = ui->preset_src_grp % ADRIFT_MAX_GROUPS;
    for (uint32_t r = 0u; r < 6u; r++) {
        uint8_t  vis_i    = (uint8_t)(top_vis + (int32_t)r);
        uint8_t  item_idx = preset_vis_to_logical(bank, vis_i);
        uint32_t ry       = sy + 22u + r * 24u;
        bool     is_sel   = (vis_i == vis_cur);
        bool     is_user  = (item_idx >= ADRIFT_UI_PRESET_FACT_COUNT);
        uint8_t  pid      = (item_idx >= 2u) ? (uint8_t)(item_idx - 2u) : 0xFFu;
        bool     is_empty = is_user && !adrift_synth_user_preset_is_occupied((uint8_t)(pid - ADRIFT_USER_PRESET_BASE));
        bool     is_active = (item_idx == 0u && orig_pst == ADRIFT_PRESET_INIT) ||
                             (item_idx >= 2u && !is_empty && orig_pst == pid);

        if (is_sel) {
            fb_fill_rect(fb, rx, ry, rw, 22u, th->accent);
            fb_rect_outline(fb, rx, ry, rw, 22u, COL_WHITE);
        } else if (is_active) {
            fb_fill_rect(fb, rx, ry, rw, 22u, COL_CARD_HDR);
            fb_rect_outline(fb, rx, ry, rw, 22u, th->accent);
        } else {
            fb_fill_rect(fb, rx, ry, rw, 22u, COL_CARD);
            fb_rect_outline(fb, rx, ry, rw, 22u, COL_BAR_TRACK);
        }

        /* Left badge [INIT], [G1..G6], [P1..P24], or [U1..U24] */
        char btxt[6];
        if (item_idx == 0u) {
            copy_str(btxt, "INIT");
        } else if (item_idx == 1u) {
            btxt[0] = 'G';
            btxt[1] = (char)('1' + eg);
            btxt[2] = '\0';
        } else if (is_user) {
            btxt[0] = 'U';
            fmt_int(&btxt[1], (int32_t)(pid - ADRIFT_USER_PRESET_BASE) + 1, "");
        } else {
            btxt[0] = 'P';
            fmt_int(&btxt[1], (int32_t)pid + 1, "");
        }

        uint32_t bx = rx + 4u;
        uint32_t by = ry + 4u;
        uint32_t bw = 32u;
        uint32_t bh = 14u;
        uint16_t badge_fg;
        if (is_sel) {
            fb_fill_rect(fb, bx, by, bw, bh, COL_BG);
            badge_fg = COL_WHITE;
        } else if (item_idx < 2u) {
            fb_fill_rect(fb, bx, by, bw, bh, COL_TAB_OFF);
            fb_rect_outline(fb, bx, by, bw, bh, th->accent);
            badge_fg = th->accent;
        } else if (is_active) {
            fb_fill_rect(fb, bx, by, bw, bh, th->accent);
            badge_fg = COL_BG;
        } else if (is_user && !is_empty) {
            fb_fill_rect(fb, bx, by, bw, bh, COL_TAB_OFF);
            fb_rect_outline(fb, bx, by, bw, bh, th->border);
            badge_fg = COL_WHITE;
        } else {
            fb_fill_rect(fb, bx, by, bw, bh, COL_TAB_OFF);
            badge_fg = COL_MUTED;
        }
        fb_draw_str_centered_spec(fb, bx, bw, by + 3u, btxt, badge_fg, &k_font_sm_bold);

        /* Item title */
        const char *title = (item_idx == 0u) ? "INIT SYNTH"
                          : (item_idx == 1u) ? "INIT GROUP"
                          : adrift_synth_get_preset_name(pid);
        uint16_t title_col = is_sel ? COL_BG
                           : (is_active ? th->accent : (is_empty ? COL_MUTED : COL_WHITE));
        fb_draw_str_spec(fb, bx + bw + 6u, ry + 6u, title, title_col, &k_font_md);

        /* Right-aligned metadata tag (ACTIVE, EMPTY, Gx:yV in GRP mode, or total V/G) */
        char meta[12];
        if (is_empty) {
            copy_str(meta, "EMPTY");
        } else if (ui->preset_load_grp && item_idx >= 2u) {
            uint8_t gnv = adrift_synth_get_preset_group_voices(pid, src_g);
            meta[0] = 'G';
            meta[1] = (char)('1' + src_g);
            meta[2] = ':';
            fmt_int(&meta[3], gnv, "V");
        } else if (is_active) {
            copy_str(meta, "ACTIVE");
        } else if (item_idx == 0u) {
            copy_str(meta, "3V 1G");
        } else if (item_idx == 1u) {
            meta[0] = 'G';
            meta[1] = (char)('1' + eg);
            meta[2] = ' ';
            fmt_int(&meta[3], ui->preset_snapshot.groups[eg].num_voices, "V");
        } else {
            uint8_t tot_v = 0u, act_g = 0u;
            adrift_synth_get_preset_summary(pid, &tot_v, &act_g);
            fmt_int(meta, tot_v, "V ");
            uint32_t mlen = str_len(meta);
            fmt_int(&meta[mlen], act_g, "G");
        }
        uint32_t mw       = fb_str_width_spec(meta, &k_font_sm_bold);
        uint16_t meta_col = is_sel ? COL_BG : (is_active ? th->accent : COL_MUTED);
        fb_draw_str_spec(fb, rx + rw - mw - 5u, ry + 7u, meta, meta_col, &k_font_sm_bold);
    }

    /* Proportional vertical scrollbar track & thumb */
    uint32_t sb_x = sx + sw - 8u;
    uint32_t sb_y = sy + 22u;
    uint32_t sb_w = 4u;
    uint32_t sb_h = 142u;
    uint32_t th_h = (vis_cnt <= 26u) ? 32u : 22u;
    uint32_t th_y = sb_y + ((uint32_t)vis_cur * (sb_h - th_h)) / (uint32_t)(vis_cnt - 1u);
    fb_fill_rect(fb, sb_x, sb_y, sb_w, sb_h, COL_BAR_TRACK);
    fb_fill_rect(fb, sb_x, th_y, sb_w, th_h, th->accent);

    /* 4-Knob Control Bar: 1:BANK  2:MODE  3:SRC  4:CLR */
    bool cur_empty_user = preset_logical_is_empty_user(cur_log);
    bool cur_occ_user   = preset_logical_is_occ_user(cur_log);
    uint32_t ky = sy + 169u;
    uint32_t ty = ky + 4u;
    fb_hline(fb, sx + 1u, ky - 1u, sw - 2u, COL_SCOPE_GRID);
    fb_fill_rect(fb, sx + 1u, ky, sw - 2u, 15u, COL_SCOPE_BG);

    fb_draw_str_spec(fb, sx + 6u, ty, "1:BK", COL_MUTED, &k_font_sm);
    fb_draw_str_spec(fb, sx + 32u, ty, k_bank_names[bank],
                     bank != 0u ? th->accent : COL_WHITE, &k_font_sm_bold);

    fb_draw_str_spec(fb, sx + 66u, ty, "2:MD", COL_MUTED, &k_font_sm);
    fb_draw_str_spec(fb, sx + 92u, ty, ui->preset_load_grp ? "GRP" : "FULL",
                     ui->preset_load_grp ? th->accent : COL_WHITE, &k_font_sm_bold);

    fb_draw_str_spec(fb, sx + 125u, ty, "3:SR", COL_MUTED, &k_font_sm);
    if (ui->preset_load_grp && cur_log >= 2u && !cur_empty_user) {
        char sbuf[3] = { 'G', (char)('1' + src_g), '\0' };
        fb_draw_str_spec(fb, sx + 151u, ty, sbuf, th->accent, &k_font_sm_bold);
    } else {
        fb_draw_str_spec(fb, sx + 151u, ty, "--", COL_MUTED, &k_font_sm_bold);
    }

    fb_draw_str_spec(fb, sx + 174u, ty, "4:CL", COL_MUTED, &k_font_sm);
    if (cur_occ_user) {
        fb_draw_str_spec(fb, sx + 200u, ty, ui->preset_clear_armed ? "ARM" : "CLR",
                         ui->preset_clear_armed ? th->accent : COL_WHITE, &k_font_sm_bold);
    } else {
        fb_draw_str_spec(fb, sx + 200u, ty, "--", COL_MUTED, &k_font_sm_bold);
    }

    /* Bottom action hint bar: [OCT+] LOAD / LOAD >Gx / SAVE Uxx / CLEAR Uxx?   [OCT-] CANCEL */
    fb_hline(fb, sx + 1u, sy + sh - 19u, sw - 2u, th->border);
    uint32_t hy = sy + sh - 15u;
    char act_lbl[14];
    uint16_t act_col = COL_WHITE;
    if (ui->preset_clear_armed && cur_occ_user) {
        uint8_t ux = (uint8_t)(cur_log - ADRIFT_UI_PRESET_FACT_COUNT);
        copy_str(act_lbl, "CLEAR U");
        fmt_int(&act_lbl[7], (int32_t)ux + 1, "?");
        act_col = th->accent;
    } else if (cur_empty_user) {
        uint8_t ux = (uint8_t)(cur_log - ADRIFT_UI_PRESET_FACT_COUNT);
        copy_str(act_lbl, "SAVE U");
        fmt_int(&act_lbl[6], (int32_t)ux + 1, "");
    } else if (ui->preset_load_grp && cur_log >= 2u) {
        copy_str(act_lbl, "LOAD >G");
        act_lbl[7] = (char)('1' + eg);
        act_lbl[8] = '\0';
    } else {
        copy_str(act_lbl, "LOAD");
    }
    fb_fill_rect(fb, sx + 6u, hy, 32u, 12u, th->accent);
    fb_draw_str_centered_spec(fb, sx + 6u, 32u, hy + 2u, "OCT+", COL_BG, &k_font_sm_bold);
    fb_draw_str_spec(fb, sx + 42u, hy + 1u, act_lbl, act_col, &k_font_md);

    fb_fill_rect(fb, sx + 130u, hy, 32u, 12u, COL_TAB_OFF);
    fb_rect_outline(fb, sx + 130u, hy, 32u, 12u, th->border);
    fb_draw_str_centered_spec(fb, sx + 130u, 32u, hy + 2u, "OCT-", COL_WHITE, &k_font_sm_bold);
    fb_draw_str_spec(fb, sx + 166u, hy + 1u, "CANCEL", COL_MUTED, &k_font_md);
}

static void draw_preset_save_modal(uint16_t *fb, const adrift_ui_t *ui, const adrift_synth_t *s) {
    uint8_t eg   = s->edit_group % ADRIFT_MAX_GROUPS;
    uint8_t slot = ui->save_slot % ADRIFT_NUM_USER_PRESETS;
    uint8_t cur  = ui->save_cursor % 12u;
    bool    is_occ = adrift_synth_user_preset_is_occupied(slot);
    const adrift_group_theme_t *th = get_group_theme(eg);
    const uint32_t sx = 5u, sy = 31u, sw = 230u, sh = 204u;

    fb_fill_rect(fb, 0, 28, ADRIFT_SCREEN_W, 212, COL_BG);
    fb_fill_rect(fb, sx, sy, sw, sh, COL_SCOPE_BG);
    fb_rect_outline(fb, sx, sy, sw, sh, th->border);

    /* 1. Top title strip: SAVE PRESET [U01/24] + voice pips */
    fb_draw_str_spec(fb, sx + 8u, sy + 6u, "SAVE PRESET", th->accent, &k_font_md);
    uint8_t disp_u = (uint8_t)(slot + 1u);
    char u_idx[8];
    u_idx[0] = 'U';
    u_idx[1] = (char)('0' + (disp_u / 10u));
    u_idx[2] = (char)('0' + (disp_u % 10u));
    u_idx[3] = '/';
    u_idx[4] = '2';
    u_idx[5] = '4';
    u_idx[6] = '\0';
    fb_fill_rect(fb, sx + 100u, sy + 5u, 46u, 12u, th->accent);
    fb_draw_str_centered_spec(fb, sx + 100u, 46u, sy + 7u, u_idx, COL_BG, &k_font_sm_bold);
    draw_voice_pips(fb, s, sx, sy + 1u, sw);
    fb_hline(fb, sx + 1u, sy + 20u, sw - 2u, th->border);

    /* 2. Target Slot Card (U1..U24 + empty/overwrite preview) */
    uint32_t cx = sx + 6u, cy = sy + 24u, cw = sw - 12u, ch = 40u;
    fb_fill_rect(fb, cx, cy, cw, ch, COL_CARD);
    fb_rect_outline(fb, cx, cy, cw, ch, is_occ ? th->accent : th->border);

    char utag[6];
    utag[0] = 'U';
    fmt_int(&utag[1], (int32_t)slot + 1, "");
    fb_fill_rect(fb, cx + 5u, cy + 5u, 42u, 30u, th->accent);
    fb_draw_str_centered_spec(fb, cx + 5u, 42u, cy + 11u, utag, COL_BG, &k_font_xl);

    fb_draw_str_spec(fb, cx + 53u, cy + 6u, "TARGET SLOT", COL_MUTED, &k_font_sm);
    fb_draw_str_spec(fb, cx + cw - 72u, cy + 6u, "ENC:PRESETS", th->accent, &k_font_sm);
    if (is_occ) {
        char ovr_buf[20];
        copy_str(ovr_buf, "OVR: ");
        copy_str(&ovr_buf[5], adrift_synth_get_preset_name((uint8_t)(ADRIFT_USER_PRESET_BASE + slot)));
        fb_draw_str_spec(fb, cx + 53u, cy + 20u, ovr_buf, th->accent, &k_font_md);
    } else {
        fb_draw_str_spec(fb, cx + 53u, cy + 20u, "EMPTY SLOT (NEW)", COL_WHITE, &k_font_md);
    }

    /* 3. 12-Character Interactive Name Editor Card */
    uint32_t nx = sx + 6u, ny = sy + 68u, nw = sw - 12u, nh = 58u;
    fb_fill_rect(fb, nx, ny, nw, nh, COL_CARD_HDR);
    fb_rect_outline(fb, nx, ny, nw, nh, th->accent);

    fb_draw_str_spec(fb, nx + 7u, ny + 5u, "PATCH NAME", COL_WHITE, &k_font_sm_bold);
    char pos_buf[12];
    copy_str(pos_buf, "POS ");
    pos_buf[4] = (char)('0' + ((cur + 1u) / 10u));
    pos_buf[5] = (char)('0' + ((cur + 1u) % 10u));
    pos_buf[6] = '/';
    pos_buf[7] = '1';
    pos_buf[8] = '2';
    pos_buf[9] = '\0';
    uint32_t pw_lbl = fb_str_width_spec(pos_buf, &k_font_sm_bold);
    fb_draw_str_spec(fb, nx + nw - pw_lbl - 7u, ny + 5u, pos_buf, th->accent, &k_font_sm_bold);

    uint32_t cell_x0 = nx + 8u;
    uint32_t cell_y  = ny + 19u;
    for (uint32_t c = 0u; c < 12u; c++) {
        uint32_t ux   = cell_x0 + c * 17u;
        char     ch_c = ui->save_name[c];
        if (!ch_c) ch_c = ' ';
        if (c == cur) {
            fb_fill_rect(fb, ux + 7u, cell_y - 4u, 1u, 1u, th->accent);
            fb_fill_rect(fb, ux + 6u, cell_y - 3u, 3u, 1u, th->accent);
            fb_fill_rect(fb, ux, cell_y, 15u, 26u, th->accent);
            fb_rect_outline(fb, ux, cell_y, 15u, 26u, COL_WHITE);
            if (ch_c != ' ') {
                fb_draw_char_spec(fb, ux + 3u, cell_y + 3u, ch_c, COL_BG, &k_font_xl_cond);
            }
            fb_fill_rect(fb, ux + 3u, cell_y + 22u, 9u, 2u, COL_BG);
            fb_fill_rect(fb, ux + 6u, cell_y + 28u, 3u, 1u, th->accent);
            fb_fill_rect(fb, ux + 7u, cell_y + 29u, 1u, 1u, th->accent);
        } else {
            fb_fill_rect(fb, ux, cell_y, 15u, 26u, COL_BG);
            fb_rect_outline(fb, ux, cell_y, 15u, 26u, COL_BAR_TRACK);
            if (ch_c != ' ') {
                fb_draw_char_spec(fb, ux + 3u, cell_y + 3u, ch_c, COL_WHITE, &k_font_xl_cond);
            } else {
                fb_fill_rect(fb, ux + 5u, cell_y + 21u, 5u, 1u, COL_SCOPE_GRID);
            }
        }
    }

    /* 4. Multi-Group + Master FX Content Summary Card */
    uint32_t px = sx + 6u, py = sy + 130u, pw = sw - 12u, ph = 34u;
    fb_fill_rect(fb, px, py, pw, ph, COL_CARD);
    fb_rect_outline(fb, px, py, pw, ph, COL_BAR_TRACK);

    uint8_t tot_v = 0u, act_g = 0u;
    for (uint8_t g = 0u; g < ADRIFT_MAX_GROUPS; g++) {
        uint8_t nv = s->groups[g].num_voices;
        tot_v = (uint8_t)(tot_v + nv);
        if (nv > 0u) act_g++;
    }
    char sum_buf[12];
    fmt_int(sum_buf, tot_v, "V ");
    fmt_int(&sum_buf[str_len(sum_buf)], act_g, "G");
    fb_draw_str_spec(fb, px + 6u, py + 4u, "FULL 6-GROUP + FX", COL_MUTED, &k_font_sm);
    uint32_t sw_lbl = fb_str_width_spec(sum_buf, &k_font_sm_bold);
    fb_draw_str_spec(fb, px + pw - sw_lbl - 6u, py + 4u, sum_buf, COL_WHITE, &k_font_sm_bold);

    for (uint8_t g = 0u; g < ADRIFT_MAX_GROUPS; g++) {
        uint32_t gx = px + 6u + (uint32_t)g * 29u;
        uint32_t gy = py + 16u;
        uint8_t  nv = s->groups[g].num_voices;
        char gpill[6];
        gpill[0] = 'G';
        gpill[1] = (char)('1' + g);
        gpill[2] = ':';
        if (nv > 0u) {
            fmt_int(&gpill[3], nv, "");
            fb_fill_rect(fb, gx, gy, 26u, 13u, get_group_theme(g)->accent);
            fb_draw_str_centered_spec(fb, gx, 26u, gy + 3u, gpill, COL_BG, &k_font_sm);
        } else {
            gpill[3] = '-';
            gpill[4] = '\0';
            fb_fill_rect(fb, gx, gy, 26u, 13u, COL_TAB_OFF);
            fb_draw_str_centered_spec(fb, gx, 26u, gy + 3u, gpill, COL_MUTED, &k_font_sm);
        }
    }
    bool fx_on = adrift_fx_is_active(&s->fx_params);
    fb_fill_rect(fb, px + 182u, py + 16u, 30u, 13u, COL_TAB_OFF);
    if (fx_on) {
        fb_rect_outline(fb, px + 182u, py + 16u, 30u, 13u, th->accent);
        fb_draw_str_centered_spec(fb, px + 182u, 30u, py + 19u, "FX+", th->accent, &k_font_sm_bold);
    } else {
        fb_draw_str_centered_spec(fb, px + 182u, 30u, py + 19u, "FX-", COL_MUTED, &k_font_sm);
    }

    /* 5. 4-Knob Control Bar: 1:SL Uxx  2:PS xx  3:CH x  4:CL */
    uint32_t ky = sy + 169u;
    uint32_t ty = ky + 4u;
    fb_hline(fb, sx + 1u, ky - 1u, sw - 2u, COL_SCOPE_GRID);
    fb_fill_rect(fb, sx + 1u, ky, sw - 2u, 15u, COL_SCOPE_BG);

    char s_slot[5] = { 'U', (char)('0' + ((slot + 1u) / 10u)), (char)('0' + ((slot + 1u) % 10u)), '\0', '\0' };
    fb_draw_str_spec(fb, sx + 6u, ty, "1:SL", COL_MUTED, &k_font_sm);
    fb_draw_str_spec(fb, sx + 31u, ty, s_slot, th->accent, &k_font_sm_bold);

    char pnum[4] = { (char)('0' + ((cur + 1u) / 10u)), (char)('0' + ((cur + 1u) % 10u)), '\0', '\0' };
    fb_draw_str_spec(fb, sx + 62u, ty, "2:PS", COL_MUTED, &k_font_sm);
    fb_draw_str_spec(fb, sx + 88u, ty, pnum, COL_WHITE, &k_font_sm_bold);

    char cur_ch = ui->save_name[cur] ? ui->save_name[cur] : ' ';
    char cdisp[4] = { cur_ch, '\0', '\0', '\0' };
    fb_draw_str_spec(fb, sx + 114u, ty, "3:CH", COL_MUTED, &k_font_sm);
    fb_draw_str_spec(fb, sx + 140u, ty, cur_ch == ' ' ? "SPC" : cdisp, th->accent, &k_font_sm_bold);

    fb_draw_str_spec(fb, sx + 170u, ty, "4:CL", COL_MUTED, &k_font_sm);
    fb_draw_str_spec(fb, sx + 196u, ty, "C/RS", COL_WHITE, &k_font_sm_bold);

    /* 6. Bottom Action Bar: [OCT+] SAVE Uxx / OVR Uxx   [OCT-] CANCEL */
    fb_hline(fb, sx + 1u, sy + sh - 19u, sw - 2u, th->border);
    uint32_t hy = sy + sh - 15u;
    char save_lbl[12];
    if (is_occ) {
        copy_str(save_lbl, "OVR U");
        fmt_int(&save_lbl[5], (int32_t)slot + 1, "");
    } else {
        copy_str(save_lbl, "SAVE U");
        fmt_int(&save_lbl[6], (int32_t)slot + 1, "");
    }
    fb_fill_rect(fb, sx + 6u, hy, 32u, 12u, th->accent);
    fb_draw_str_centered_spec(fb, sx + 6u, 32u, hy + 2u, "OCT+", COL_BG, &k_font_sm_bold);
    fb_draw_str_spec(fb, sx + 42u, hy + 1u, save_lbl, is_occ ? th->accent : COL_WHITE, &k_font_md);

    fb_fill_rect(fb, sx + 130u, hy, 32u, 12u, COL_TAB_OFF);
    fb_rect_outline(fb, sx + 130u, hy, 32u, 12u, th->border);
    fb_draw_str_centered_spec(fb, sx + 130u, 32u, hy + 2u, "OCT-", COL_WHITE, &k_font_sm_bold);
    fb_draw_str_spec(fb, sx + 166u, hy + 1u, "CANCEL", COL_MUTED, &k_font_md);
}

static void draw_scale_browser(uint16_t *fb, const adrift_ui_t *ui, const adrift_synth_t *s) {
    uint8_t eg = s->edit_group % ADRIFT_MAX_GROUPS;
    const adrift_group_theme_t *th = get_group_theme(eg);
    const uint32_t sx = 5u, sy = 31u, sw = 230u, sh = 204u;
    uint8_t cur  = ui->scale_cursor % ADRIFT_NUM_SCALES;
    uint8_t root = s->kbd_scale_root % 12u;
    const char *root_name = adrift_synth_get_root_name(root);

    fb_fill_rect(fb, 0, 28, ADRIFT_SCREEN_W, 212, COL_BG);
    fb_fill_rect(fb, sx, sy, sw, sh, COL_SCOPE_BG);
    fb_rect_outline(fb, sx, sy, sw, sh, th->border);

    /* Top title strip inside scale browser: SCALES [01/16] + 1:ROOT [C] + voice pips */
    fb_draw_str_spec(fb, sx + 8u, sy + 6u, "SCALES", th->accent, &k_font_md);
    uint8_t disp_idx = (uint8_t)(cur + 1u);
    char idx_buf[8];
    idx_buf[0] = (char)('0' + (disp_idx / 10u));
    idx_buf[1] = (char)('0' + (disp_idx % 10u));
    idx_buf[2] = '/';
    idx_buf[3] = '1';
    idx_buf[4] = '6';
    idx_buf[5] = '\0';
    fb_fill_rect(fb, sx + 59u, sy + 5u, 39u, 12u, th->accent);
    fb_draw_str_centered_spec(fb, sx + 59u, 39u, sy + 7u, idx_buf, COL_BG, &k_font_sm_bold);

    fb_draw_str_spec(fb, sx + 103u, sy + 7u, "1:ROOT", COL_MUTED, &k_font_sm_bold);
    fb_fill_rect(fb, sx + 147u, sy + 5u, 21u, 12u, COL_TAB_OFF);
    fb_rect_outline(fb, sx + 147u, sy + 5u, 21u, 12u, th->accent);
    fb_draw_str_centered_spec(fb, sx + 147u, 21u, sy + 7u, root_name, COL_WHITE, &k_font_sm_bold);

    draw_voice_pips(fb, s, sx, sy + 1u, sw);
    fb_hline(fb, sx + 1u, sy + 20u, sw - 2u, th->border);

    /* 7-row scrollable list (0 = CHROMATIC, 1..15 = MAJOR .. WHOLE TONE) */
    int32_t top_idx = (int32_t)cur - 3;
    if (top_idx < 0) top_idx = 0;
    if (top_idx > (int32_t)ADRIFT_NUM_SCALES - 7) {
        top_idx = (int32_t)ADRIFT_NUM_SCALES - 7;
    }

    const uint32_t rx = sx + 5u;
    const uint32_t rw = sw - 16u;
    uint8_t orig_sc   = ui->scale_orig_type % ADRIFT_NUM_SCALES;
    uint8_t orig_root = ui->scale_orig_root % 12u;
    for (uint32_t r = 0; r < 7u; r++) {
        uint8_t  item_idx      = (uint8_t)(top_idx + (int32_t)r);
        uint32_t ry            = sy + 22u + r * 23u;
        bool     is_sel        = (item_idx == cur);
        bool     is_orig_scale = (item_idx == orig_sc);
        bool     is_active     = is_orig_scale && (item_idx == 0u || root == orig_root);

        if (is_sel) {
            fb_fill_rect(fb, rx, ry, rw, 21u, th->accent);
            fb_rect_outline(fb, rx, ry, rw, 21u, COL_WHITE);
        } else if (is_orig_scale) {
            fb_fill_rect(fb, rx, ry, rw, 21u, COL_CARD_HDR);
            fb_rect_outline(fb, rx, ry, rw, 21u, th->accent);
        } else {
            fb_fill_rect(fb, rx, ry, rw, 21u, COL_CARD);
            fb_rect_outline(fb, rx, ry, rw, 21u, COL_BAR_TRACK);
        }

        /* Left badge: [CHR] for Chromatic, or current root [C]..[B] for musical scales */
        const char *btxt = (item_idx == 0u) ? "CHR" : root_name;
        uint32_t bx = rx + 4u;
        uint32_t by = ry + 4u;
        uint32_t bw = 32u;
        uint32_t bh = 13u;
        uint16_t badge_fg;
        if (is_sel) {
            fb_fill_rect(fb, bx, by, bw, bh, COL_BG);
            badge_fg = COL_WHITE;
        } else if (item_idx == 0u && !is_orig_scale) {
            fb_fill_rect(fb, bx, by, bw, bh, COL_TAB_OFF);
            fb_rect_outline(fb, bx, by, bw, bh, th->accent);
            badge_fg = th->accent;
        } else if (is_orig_scale) {
            fb_fill_rect(fb, bx, by, bw, bh, th->accent);
            badge_fg = COL_BG;
        } else {
            fb_fill_rect(fb, bx, by, bw, bh, COL_TAB_OFF);
            badge_fg = COL_MUTED;
        }
        fb_draw_str_centered_spec(fb, bx, bw, by + 3u, btxt, badge_fg, &k_font_sm_bold);

        /* Scale title */
        const char *title = adrift_synth_get_scale_name(item_idx);
        uint16_t title_col = is_sel ? COL_BG : (is_orig_scale ? th->accent : COL_WHITE);
        fb_draw_str_spec(fb, bx + bw + 6u, ry + 6u, title, title_col, &k_font_md);

        /* Right-aligned metadata tag (ACTIVE, ACT:<root>, or note count 12N/7N/6N/5N) */
        char meta[12];
        if (is_active) {
            copy_str(meta, "ACTIVE");
        } else if (is_orig_scale) {
            copy_str(meta, "ACT:");
            copy_str(&meta[4], adrift_synth_get_root_name(orig_root));
        } else {
            fmt_int(meta, adrift_synth_get_scale_num_notes(item_idx), "N");
        }
        uint32_t mw       = fb_str_width_spec(meta, &k_font_sm_bold);
        uint16_t meta_col = is_sel ? COL_BG : (is_orig_scale ? th->accent : COL_MUTED);
        fb_draw_str_spec(fb, rx + rw - mw - 5u, ry + 7u, meta, meta_col, &k_font_sm_bold);
    }

    /* Proportional vertical scrollbar track & thumb */
    uint32_t sb_x = sx + sw - 8u;
    uint32_t sb_y = sy + 22u;
    uint32_t sb_w = 4u;
    uint32_t sb_h = 159u;
    uint32_t th_h = 56u;
    uint32_t th_y = sb_y + ((uint32_t)cur * (sb_h - th_h)) / (ADRIFT_NUM_SCALES - 1u);
    fb_fill_rect(fb, sb_x, sb_y, sb_w, sb_h, COL_BAR_TRACK);
    fb_fill_rect(fb, sb_x, th_y, sb_w, th_h, th->accent);

    /* Bottom action hint bar: [OCT+] APPLY   [OCT-] CANCEL */
    fb_hline(fb, sx + 1u, sy + sh - 19u, sw - 2u, th->border);
    uint32_t hy = sy + sh - 15u;
    fb_fill_rect(fb, sx + 8u, hy, 32u, 12u, th->accent);
    fb_draw_str_centered_spec(fb, sx + 8u, 32u, hy + 2u, "OCT+", COL_BG, &k_font_sm_bold);
    fb_draw_str_spec(fb, sx + 44u, hy + 1u, "APPLY", COL_WHITE, &k_font_md);

    fb_fill_rect(fb, sx + 118u, hy, 32u, 12u, COL_TAB_OFF);
    fb_rect_outline(fb, sx + 118u, hy, 32u, 12u, th->border);
    fb_draw_str_centered_spec(fb, sx + 118u, 32u, hy + 2u, "OCT-", COL_WHITE, &k_font_sm_bold);
    fb_draw_str_spec(fb, sx + 154u, hy + 1u, "CANCEL", COL_MUTED, &k_font_md);
}

static uint8_t default_preset_cursor_for_synth(const adrift_synth_t *s) {
    if (s->preset_index == ADRIFT_PRESET_INIT) {
        return 0u;
    }
    if (s->preset_index >= ADRIFT_USER_PRESET_BASE &&
        s->preset_index < ADRIFT_USER_PRESET_BASE + ADRIFT_NUM_USER_PRESETS) {
        return (uint8_t)(2u + s->preset_index);
    }
    return (uint8_t)(2u + (s->preset_index % ADRIFT_NUM_PRESETS));
}

static void apply_preset_cursor_selection(adrift_ui_t *ui, adrift_synth_t *s) {
    uint8_t sel = ui->preset_cursor % ADRIFT_UI_PRESET_ALL_COUNT;
    if (sel == 0u) {
        adrift_synth_all_notes_off(s, true);
        adrift_synth_init_patch(s);
        ui->preset_previewed = true;
    } else if (sel == 1u) {
        uint8_t target_g = s->edit_group % ADRIFT_MAX_GROUPS;
        if (ui->preset_previewed) {
            adrift_synth_restore_snapshot(s, &ui->preset_snapshot);
            s->edit_group = target_g;
        }
        adrift_synth_init_group_patch(s, target_g);
        ui->preset_previewed = true;
    } else {
        uint8_t pid = (uint8_t)(sel - 2u);
        if (pid >= ADRIFT_USER_PRESET_BASE &&
            !adrift_synth_user_preset_is_occupied((uint8_t)(pid - ADRIFT_USER_PRESET_BASE))) {
            if (ui->preset_previewed) {
                uint8_t target_g = s->edit_group % ADRIFT_MAX_GROUPS;
                adrift_synth_restore_snapshot(s, &ui->preset_snapshot);
                if (ui->preset_load_grp) {
                    s->edit_group = target_g;
                }
                ui->preset_previewed = false;
            }
            return;
        }
        if (ui->preset_load_grp) {
            uint8_t target_g = s->edit_group % ADRIFT_MAX_GROUPS;
            if (ui->preset_previewed) {
                adrift_synth_restore_snapshot(s, &ui->preset_snapshot);
                s->edit_group = target_g;
            }
            adrift_synth_load_preset_group(s, pid, ui->preset_src_grp % ADRIFT_MAX_GROUPS, target_g);
        } else {
            adrift_synth_all_notes_off(s, true);
            adrift_synth_load_preset(s, pid);
        }
        ui->preset_previewed = true;
    }
}

static void cancel_preset_menu_if_open(adrift_ui_t *ui, adrift_synth_t *s) {
    if (!ui->in_preset_menu) return;
    if (ui->preset_previewed) {
        adrift_synth_restore_snapshot(s, &ui->preset_snapshot);
    } else {
        s->edit_group = (uint8_t)(ui->preset_snapshot.edit_group % ADRIFT_MAX_GROUPS);
    }
    ui->in_preset_menu     = false;
    ui->preset_previewed   = false;
    ui->preset_clear_armed = false;
}

static void cancel_scale_menu_if_open(adrift_ui_t *ui, adrift_synth_t *s) {
    if (!ui->in_scale_menu) return;
    if (ui->scale_previewed) {
        s->kbd_scale      = (uint8_t)(ui->scale_orig_type % ADRIFT_NUM_SCALES);
        s->kbd_scale_root = (uint8_t)(ui->scale_orig_root % 12u);
    }
    ui->in_scale_menu   = false;
    ui->scale_previewed = false;
}

static void cancel_save_menu_if_open(adrift_ui_t *ui) {
    ui->in_save_menu = false;
}

static int32_t save_charset_index(char ch) {
    if (ch >= 'a' && ch <= 'z') {
        ch = (char)(ch - 'a' + 'A');
    }
    for (int32_t i = 0; i < SAVE_CHARSET_COUNT; i++) {
        if (k_save_charset[i] == ch) {
            return i;
        }
    }
    return -1;
}

static void open_save_preset_menu(adrift_ui_t *ui, adrift_synth_t *s, int32_t forced_slot) {
    cancel_preset_menu_if_open(ui, s);
    cancel_scale_menu_if_open(ui, s);

    if (forced_slot >= 0 && forced_slot < (int32_t)ADRIFT_NUM_USER_PRESETS) {
        ui->save_slot = (uint8_t)forced_slot;
    } else if (s->preset_index >= ADRIFT_USER_PRESET_BASE &&
               s->preset_index < ADRIFT_USER_PRESET_BASE + ADRIFT_NUM_USER_PRESETS) {
        ui->save_slot = (uint8_t)(s->preset_index - ADRIFT_USER_PRESET_BASE);
    } else {
        ui->save_slot = adrift_synth_first_empty_user_preset();
    }

    const char *src = s->preset_name;
    if (src[0] >= '0' && src[0] <= '9' && src[1] >= '0' && src[1] <= '9' && src[2] == ' ') {
        src += 3;
    }
    uint32_t i = 0u;
    while (i < 12u && src[i] != '\0') {
        char c = src[i];
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        ui->save_name[i] = (save_charset_index(c) >= 0) ? c : ' ';
        i++;
    }
    while (i < 12u) {
        ui->save_name[i++] = ' ';
    }
    ui->save_name[12] = '\0';
    for (uint32_t k = 0u; k <= 12u; k++) {
        ui->save_orig_name[k] = ui->save_name[k];
    }
    ui->save_cursor  = 0u;
    ui->in_save_menu = true;
}

static void save_modal_backspace(adrift_ui_t *ui) {
    uint8_t c = ui->save_cursor % 12u;
    if (c == 11u && ui->save_name[11] != ' ') {
        ui->save_name[11] = ' ';
        return;
    }
    if (c > 0u) {
        for (uint8_t i = (uint8_t)(c - 1u); i < 11u; i++) {
            ui->save_name[i] = ui->save_name[i + 1u];
        }
        ui->save_name[11] = ' ';
        ui->save_cursor   = (uint8_t)(c - 1u);
    } else {
        for (uint8_t i = 0u; i < 11u; i++) {
            ui->save_name[i] = ui->save_name[i + 1u];
        }
        ui->save_name[11] = ' ';
    }
}

static void save_modal_delete_at(adrift_ui_t *ui) {
    uint8_t c = ui->save_cursor % 12u;
    for (uint8_t i = c; i < 11u; i++) {
        ui->save_name[i] = ui->save_name[i + 1u];
    }
    ui->save_name[11] = ' ';
}

static void open_or_scroll_preset_menu(adrift_ui_t *ui, adrift_synth_t *s, int32_t delta) {
    if (ui->in_save_menu) {
        if (delta != 0) {
            int32_t cnt = (int32_t)ADRIFT_NUM_USER_PRESETS;
            int32_t d   = delta % cnt;
            ui->save_slot = (uint8_t)(((int32_t)ui->save_slot + d + cnt) % cnt);
        }
        return;
    }
    if (!ui->in_preset_menu) {
        cancel_scale_menu_if_open(ui, s);
        adrift_synth_save_snapshot(s, &ui->preset_snapshot);
        ui->in_preset_menu     = true;
        ui->preset_previewed   = false;
        ui->preset_load_grp    = false;
        ui->preset_src_grp     = (uint8_t)(s->edit_group % ADRIFT_MAX_GROUPS);
        ui->preset_clear_armed = false;
        ui->preset_cursor      = default_preset_cursor_for_synth(s);
        if ((ui->preset_bank == ADRIFT_PRESET_BANK_USER && ui->preset_cursor < ADRIFT_UI_PRESET_FACT_COUNT) ||
            (ui->preset_bank == ADRIFT_PRESET_BANK_FACT && ui->preset_cursor >= ADRIFT_UI_PRESET_FACT_COUNT)) {
            ui->preset_bank = ADRIFT_PRESET_BANK_ALL;
        }
    } else if (delta != 0) {
        uint8_t bank     = ui->preset_bank % ADRIFT_PRESET_BANK_COUNT;
        int32_t cnt      = (int32_t)preset_bank_count(bank);
        int32_t vis      = (int32_t)preset_logical_to_vis(bank, ui->preset_cursor);
        int32_t d        = delta % cnt;
        uint8_t next_vis = (uint8_t)((vis + d + cnt) % cnt);
        ui->preset_cursor      = preset_vis_to_logical(bank, next_vis);
        ui->preset_clear_armed = false;
        if (ui->preset_load_grp && ui->preset_cursor >= 2u && !preset_logical_is_empty_user(ui->preset_cursor)) {
            ui->preset_src_grp = preset_auto_pick_src_grp((uint8_t)(ui->preset_cursor - 2u), ui->preset_src_grp);
        }
        apply_preset_cursor_selection(ui, s);
    }
}

static void open_or_scroll_scale_menu(adrift_ui_t *ui, adrift_synth_t *s, int32_t delta) {
    if (!ui->in_scale_menu) {
        cancel_preset_menu_if_open(ui, s);
        cancel_save_menu_if_open(ui);
        ui->scale_orig_type = (uint8_t)(s->kbd_scale % ADRIFT_NUM_SCALES);
        ui->scale_orig_root = (uint8_t)(s->kbd_scale_root % 12u);
        ui->scale_cursor    = ui->scale_orig_type;
        ui->in_scale_menu   = true;
        ui->scale_previewed = false;
    } else if (delta != 0) {
        int32_t cnt = (int32_t)ADRIFT_NUM_SCALES;
        int32_t d   = delta % cnt;
        ui->scale_cursor    = (uint8_t)(((int32_t)ui->scale_cursor + d + cnt) % cnt);
        s->kbd_scale        = ui->scale_cursor;
        ui->scale_previewed = true;
    }
}

static void select_or_cycle_param_page(adrift_ui_t *ui, adrift_synth_t *s, uint8_t target_page) {
    if (!ui->in_preset_menu && !ui->in_scale_menu && !ui->in_save_menu && !ui->in_scope &&
        !ui->in_group_menu && !ui->in_arp_menu && !ui->in_fx_menu &&
        ui->page == target_page) {
        uint8_t num_sub = page_num_subpages(target_page, false, false, false);
        ui->subpage     = (uint8_t)((ui->subpage + 1u) % num_sub);
    } else {
        cancel_preset_menu_if_open(ui, s);
        cancel_scale_menu_if_open(ui, s);
        cancel_save_menu_if_open(ui);
        ui->in_scope       = false;
        ui->in_group_menu  = false;
        ui->in_arp_menu    = false;
        ui->in_fx_menu     = false;
        ui->page           = target_page;
        ui->subpage        = 0u;
    }
}

void adrift_ui_init(adrift_ui_t *ui) {
    uint8_t *raw = (uint8_t *)ui;
    for (uint32_t i = 0; i < (uint32_t)sizeof(*ui); i++) {
        raw[i] = 0u;
    }
    ui->page                = 0u;
    ui->subpage             = 0u;
    ui->in_group_menu       = false;
    ui->in_arp_menu         = false;
    ui->in_fx_menu          = false;
    ui->in_scope            = false;
    ui->scope_page          = 0u;
    ui->scope_zoom          = 0u;
    ui->scope_gain          = 100u;
    ui->scope_freeze        = false;
    ui->in_preset_menu      = false;
    ui->preset_previewed    = false;
    ui->preset_cursor       = 2u;
    ui->preset_bank         = ADRIFT_PRESET_BANK_ALL;
    ui->preset_load_grp     = false;
    ui->preset_src_grp      = 0u;
    ui->preset_clear_armed  = false;
    ui->in_save_menu        = false;
    ui->save_slot           = 0u;
    ui->save_cursor         = 0u;
    for (uint32_t i = 0u; i < 12u; i++) {
        ui->save_name[i]      = ' ';
        ui->save_orig_name[i] = ' ';
    }
    ui->save_name[12]       = '\0';
    ui->save_orig_name[12]  = '\0';
    ui->in_scale_menu       = false;
    ui->scale_previewed     = false;
    ui->scale_cursor        = 0u;
    ui->scale_orig_type     = 0u;
    ui->scale_orig_root     = 0u;
    ui->initialized         = false;
    ui->boot_active         = true;
    ui->frame_count         = 0u;
    ui->edit_hold_frames    = 0u;
    ui->sound_hold_frames   = 0u;
    ui->prev_layout_mode    = 0xFFu;
    ui->prev_preset         = 0xFFu;
    ui->prev_edit_group     = 0xFFu;
    ui->prev_group_voices   = 0xFFu;
    ui->prev_kbd_octave     = 127;
    ui->prev_in_group_menu  = false;
    ui->prev_in_arp_menu    = false;
    ui->prev_in_fx_menu     = false;
    ui->prev_in_scope       = false;
    ui->prev_scope_page     = 0xFFu;
    ui->prev_scope_zoom     = 0xFFu;
    ui->prev_scope_gain     = 0xFFFFu;
    ui->prev_scope_freeze   = false;
    ui->prev_scope_mode     = 0xFFu;
    ui->prev_in_preset_menu     = false;
    ui->prev_preset_cursor      = 0xFFu;
    ui->prev_preset_bank        = 0xFFu;
    ui->prev_preset_load_grp    = false;
    ui->prev_preset_src_grp     = 0xFFu;
    ui->prev_preset_clear_armed = false;
    ui->prev_in_save_menu       = false;
    ui->prev_save_sig           = 0xFFFFFFFFu;
    ui->prev_save_toast         = false;
    ui->prev_preset_name_sig    = 0xFFFFFFFFu;
    ui->prev_in_scale_menu      = false;
    ui->prev_scale_cursor   = 0xFFu;
    ui->prev_scale_root     = 0xFFu;
    ui->prev_page           = 0xFFu;
    ui->prev_subpage        = 0xFFu;
    ui->prev_note           = 0xFFu;
    ui->prev_gate           = false;
    ui->prev_meter_w        = 0xFFu;
    ui->prev_cpu_pct        = 0xFFu;
    ui->prev_pip_sig         = 0xFFFFFFFFu;
    ui->prev_arp_step        = 0xFFu;
    ui->prev_fx_gr           = 0xFFu;
    ui->prev_fx_sig          = 0xFFFFFFFFu;
    ui->prev_midi_clk_active = false;
    ui->prev_midi_clk_bpm    = 0u;
    ui->scope_prev_y0        = 51u;
    ui->scope_prev_y1        = 218u;
}

void adrift_ui_dismiss_boot(adrift_ui_t *ui) {
    if (!ui->boot_active) return;
    ui->boot_active    = false;
    ui->boot_bar_decay = 0u;
    ui->initialized    = false;
}

static bool boot_should_dismiss(const adrift_synth_t *s, const adrift_input_state_t *in) {
    if ((in->buttons_pressed | in->buttons_down) != 0u) return true;
    for (uint32_t i = 0u; i < ADRIFT_NUM_ENC; i++) {
        if (in->enc_delta[i] != 0) return true;
    }
    if (in->text_char != 0 || in->text_cmd != ADRIFT_TEXT_CMD_NONE) return true;
    if (s->voice_age_counter != 0u || s->any_gate || s->preset_index != 0u) return true;
    for (uint32_t g = 0u; g < ADRIFT_MAX_GROUPS; g++) {
        const adrift_group_t *gp = &s->groups[g];
        if (gp->arp_state.num_notes != 0u ||
            gp->mono_len != 0u ||
            gp->sustain_pedal ||
            gp->mod_wheel != 0u ||
            gp->aftertouch != 0u ||
            gp->pitch_bend_q13 != 0) {
            return true;
        }
    }
    return false;
}

void adrift_ui_step(adrift_ui_t *ui,
                    adrift_synth_t *synth,
                    const adrift_input_state_t *input,
                    uint16_t *fb,
                    adrift_dirty_t *dirty) {
    if (ui->boot_active) {
        if (boot_should_dismiss(synth, input)) {
            adrift_ui_dismiss_boot(ui);
        } else if (ui->boot_frame >= ADRIFT_UI_BOOT_FRAMES) {
            ui->boot_active    = false;
            ui->boot_bar_decay = 12u;
            ui->initialized    = false;
        } else {
            if (input->master_pot >= 0) {
                synth->hw_master_pot = input->master_pot;
            }
            draw_boot_frame(ui, synth, fb, dirty);
            ui->boot_frame++;
            ui->frame_count++;
            return;
        }
    }

    bool cards_dirty  = false;
    bool header_dirty = false;
    bool top_dirty    = false;

    if (!ui->initialized) {
        fb_fill_rect(fb, 0, 0, ADRIFT_SCREEN_W, ADRIFT_SCREEN_H, COL_BG);
        ui->initialized    = true;
        dirty->full_redraw = true;
        cards_dirty        = true;
        header_dirty       = true;
        top_dirty          = true;
    }

    if (ui->save_toast_frames > 0u) {
        ui->save_toast_frames--;
    }

    /* 1. Handle screen & action button edges */
    adrift_screen_reader_t *sr = adrift_app_get_screen_reader();
    uint32_t pressed = input->buttons_pressed;
    if (pressed & ADRIFT_BTN_SAVE_PRESET) {
        if (ui->in_save_menu) {
            cancel_save_menu_if_open(ui);
            adrift_sr_say(sr, ADRIFT_SR_PRIO_NOW, ADRIFT_SR_CAT_ACTION, "Save canceled.");
        } else if (ui->in_preset_menu && ui->preset_cursor >= ADRIFT_UI_PRESET_FACT_COUNT) {
            open_save_preset_menu(ui, synth, (int32_t)(ui->preset_cursor - ADRIFT_UI_PRESET_FACT_COUNT));
            adrift_sr_announce_save_open(sr, ui);
        } else {
            open_save_preset_menu(ui, synth, -1);
            adrift_sr_announce_save_open(sr, ui);
        }
        header_dirty = top_dirty = cards_dirty = true;
    }
    if (pressed & ADRIFT_BTN_HOME) {
        if (!ui->in_preset_menu && !ui->in_scale_menu && !ui->in_save_menu && ui->in_scope) {
            ui->scope_page = (uint8_t)((ui->scope_page + 1u) % ADRIFT_UI_NUM_HOME_PAGES);
        } else {
            cancel_preset_menu_if_open(ui, synth);
            cancel_scale_menu_if_open(ui, synth);
            cancel_save_menu_if_open(ui);
            ui->in_scope       = true;
            ui->in_group_menu  = false;
            ui->in_arp_menu    = false;
            ui->in_fx_menu     = false;
            ui->scope_page     = 0u;
        }
        adrift_sr_announce_screen(sr, ui, synth, false);
        header_dirty = top_dirty = cards_dirty = true;
    }
    if (pressed & ADRIFT_BTN_GROUP_CFG) {
        if (!ui->in_preset_menu && !ui->in_scale_menu && !ui->in_save_menu && !ui->in_scope && ui->in_group_menu) {
            ui->subpage ^= 1u;
        } else {
            cancel_preset_menu_if_open(ui, synth);
            cancel_scale_menu_if_open(ui, synth);
            cancel_save_menu_if_open(ui);
            ui->in_scope       = false;
            ui->in_group_menu  = true;
            ui->in_arp_menu    = false;
            ui->in_fx_menu     = false;
            ui->subpage        = 0u;
        }
        adrift_sr_announce_screen(sr, ui, synth, false);
        header_dirty = top_dirty = cards_dirty = true;
    }
    if (pressed & ADRIFT_BTN_ARP) {
        if (!ui->in_preset_menu && !ui->in_scale_menu && !ui->in_save_menu && !ui->in_scope && ui->in_arp_menu) {
            ui->subpage ^= 1u;
        } else {
            cancel_preset_menu_if_open(ui, synth);
            cancel_scale_menu_if_open(ui, synth);
            cancel_save_menu_if_open(ui);
            ui->in_scope       = false;
            ui->in_group_menu  = false;
            ui->in_arp_menu    = true;
            ui->in_fx_menu     = false;
            ui->subpage        = 0u;
        }
        adrift_sr_announce_screen(sr, ui, synth, false);
        header_dirty = top_dirty = cards_dirty = true;
    }
    if (pressed & ADRIFT_BTN_FX) {
        if (!ui->in_preset_menu && !ui->in_scale_menu && !ui->in_save_menu && !ui->in_scope && ui->in_fx_menu) {
            ui->subpage = (uint8_t)((ui->subpage + 1u) % 3u);
        } else {
            cancel_preset_menu_if_open(ui, synth);
            cancel_scale_menu_if_open(ui, synth);
            cancel_save_menu_if_open(ui);
            ui->in_scope       = false;
            ui->in_group_menu  = false;
            ui->in_arp_menu    = false;
            ui->in_fx_menu     = true;
            ui->subpage        = 0u;
        }
        adrift_sr_announce_screen(sr, ui, synth, false);
        header_dirty = top_dirty = cards_dirty = true;
    }
    if (pressed & ADRIFT_BTN_PAGE_1) { select_or_cycle_param_page(ui, synth, 0u); adrift_sr_announce_screen(sr, ui, synth, false); header_dirty = top_dirty = cards_dirty = true; }
    if (pressed & ADRIFT_BTN_PAGE_2) { select_or_cycle_param_page(ui, synth, 1u); adrift_sr_announce_screen(sr, ui, synth, false); header_dirty = top_dirty = cards_dirty = true; }
    if (pressed & ADRIFT_BTN_PAGE_3) { select_or_cycle_param_page(ui, synth, 2u); adrift_sr_announce_screen(sr, ui, synth, false); header_dirty = top_dirty = cards_dirty = true; }
    if (pressed & ADRIFT_BTN_PAGE_4) { select_or_cycle_param_page(ui, synth, 3u); adrift_sr_announce_screen(sr, ui, synth, false); header_dirty = top_dirty = cards_dirty = true; }
    if (pressed & ADRIFT_BTN_PAGE_5) { select_or_cycle_param_page(ui, synth, 4u); adrift_sr_announce_screen(sr, ui, synth, false); header_dirty = top_dirty = cards_dirty = true; }

    if (pressed & ADRIFT_BTN_NEXT_GROUP) {
        adrift_synth_cycle_edit_group(synth);
        if (ui->in_preset_menu &&
            (ui->preset_cursor == 1u || (ui->preset_load_grp && ui->preset_previewed))) {
            apply_preset_cursor_selection(ui, synth);
        }
        adrift_sr_announce_group_select(sr, synth);
        header_dirty = top_dirty = cards_dirty = true;
    }
    if (pressed & ADRIFT_BTN_PREV_PST) {
        bool was_save   = ui->in_save_menu;
        bool was_preset = ui->in_preset_menu;
        open_or_scroll_preset_menu(ui, synth, -1);
        if (was_save) {
            adrift_sr_announce_save_slot(sr, ui);
        } else {
            adrift_sr_announce_preset_row(sr, ui, synth, !was_preset);
        }
        header_dirty = top_dirty = cards_dirty = true;
    }
    if (pressed & ADRIFT_BTN_NEXT_PST) {
        bool was_save   = ui->in_save_menu;
        bool was_preset = ui->in_preset_menu;
        open_or_scroll_preset_menu(ui, synth, 1);
        if (was_save) {
            adrift_sr_announce_save_slot(sr, ui);
        } else {
            adrift_sr_announce_preset_row(sr, ui, synth, !was_preset);
        }
        header_dirty = top_dirty = cards_dirty = true;
    }
    if (pressed & ADRIFT_BTN_OCT_DOWN) {
        if (ui->in_save_menu) {
            cancel_save_menu_if_open(ui);
            adrift_sr_say(sr, ADRIFT_SR_PRIO_NOW, ADRIFT_SR_CAT_ACTION, "Save canceled.");
            header_dirty = top_dirty = cards_dirty = true;
        } else if (ui->in_preset_menu) {
            cancel_preset_menu_if_open(ui, synth);
            adrift_sr_say(sr, ADRIFT_SR_PRIO_NOW, ADRIFT_SR_CAT_ACTION, "Presets canceled.");
            header_dirty = top_dirty = cards_dirty = true;
        } else if (ui->in_scale_menu) {
            cancel_scale_menu_if_open(ui, synth);
            adrift_sr_say(sr, ADRIFT_SR_PRIO_NOW, ADRIFT_SR_CAT_ACTION, "Scales canceled.");
            header_dirty = top_dirty = cards_dirty = true;
        } else {
            if (synth->kbd_octave > -3) {
                synth->kbd_octave--;
                adrift_sr_announce_octave(sr, synth->kbd_octave, 0);
                header_dirty = true;
            } else {
                adrift_sr_announce_octave(sr, synth->kbd_octave, -1);
            }
        }
    }
    if (pressed & ADRIFT_BTN_OCT_UP) {
        if (ui->in_save_menu) {
            uint8_t slot = ui->save_slot % ADRIFT_NUM_USER_PRESETS;
            bool ok = adrift_synth_save_user_preset(synth, slot, ui->save_name);
            if (ok) {
                ui->save_toast_slot   = slot;
                ui->save_toast_frames = 45u;
            }
            ui->in_save_menu = false;
            adrift_sr_announce_save_result(sr, slot, ui->save_name, ok);
            header_dirty = top_dirty = cards_dirty = true;
        } else if (ui->in_preset_menu) {
            uint8_t cur_log = ui->preset_cursor % ADRIFT_UI_PRESET_ALL_COUNT;
            if (ui->preset_clear_armed && preset_logical_is_occ_user(cur_log)) {
                uint8_t ux = (uint8_t)(cur_log - ADRIFT_UI_PRESET_FACT_COUNT);
                (void)adrift_synth_clear_user_preset(synth, ux);
                if (ui->preset_snapshot.preset_index == (uint8_t)(ADRIFT_USER_PRESET_BASE + ux)) {
                    ui->preset_snapshot.preset_index = ADRIFT_PRESET_INIT;
                }
                ui->preset_clear_armed = false;
                apply_preset_cursor_selection(ui, synth);
                adrift_sr_announce_preset_confirm(sr, cur_log, false, 0u, 0u, true, ux);
            } else if (preset_logical_is_empty_user(cur_log)) {
                uint8_t ux = (uint8_t)(cur_log - ADRIFT_UI_PRESET_FACT_COUNT);
                open_save_preset_menu(ui, synth, (int32_t)ux);
                adrift_sr_announce_save_open(sr, ui);
            } else {
                bool    grp_load = ui->preset_load_grp;
                uint8_t src_g    = ui->preset_src_grp;
                uint8_t dst_g    = synth->edit_group;
                if (!ui->preset_previewed) {
                    apply_preset_cursor_selection(ui, synth);
                }
                ui->in_preset_menu     = false;
                ui->preset_previewed   = false;
                ui->preset_clear_armed = false;
                adrift_sr_announce_preset_confirm(sr, cur_log, grp_load, src_g, dst_g, false, 0u);
            }
            header_dirty = top_dirty = cards_dirty = true;
        } else if (ui->in_scale_menu) {
            synth->kbd_scale    = (uint8_t)(ui->scale_cursor % ADRIFT_NUM_SCALES);
            ui->in_scale_menu   = false;
            ui->scale_previewed = false;
            adrift_sr_announce_scale_confirm(sr, synth);
            header_dirty = top_dirty = cards_dirty = true;
        } else {
            if (synth->kbd_octave < 3) {
                synth->kbd_octave++;
                adrift_sr_announce_octave(sr, synth->kbd_octave, 0);
                header_dirty = true;
            } else {
                adrift_sr_announce_octave(sr, synth->kbd_octave, 1);
            }
        }
    }
    if (pressed & ADRIFT_BTN_PANIC) {
        adrift_synth_all_notes_off(synth, true);
        ui->sound_hold_frames = 0u;
        adrift_sr_say(sr, ADRIFT_SR_PRIO_NOW, ADRIFT_SR_CAT_ACTION, "All notes off.");
        header_dirty = top_dirty = cards_dirty = true;
    }
    if ((pressed & ADRIFT_BTN_HOLD) && !ui->in_preset_menu && !ui->in_scale_menu && !ui->in_save_menu) {
        bool cur_hold = adrift_synth_view_patch(synth)->env2.drone_hold;
        apply_encoder_for_synth(synth, false, false, false, 3u, 6u, cur_hold ? -1 : 1);
        adrift_sr_say(sr, ADRIFT_SR_PRIO_NOW, ADRIFT_SR_CAT_ACTION,
                      adrift_synth_view_patch(synth)->env2.drone_hold ? "Drone hold." : "Drone gated.");
        header_dirty = top_dirty = cards_dirty = true;
    }

    /* 2. Hardware master pot */
    if (input->master_pot >= 0) {
        synth->hw_master_pot = input->master_pot;
    }

    /* 3. Handle navigation encoders: SELECT -> Voice Group, PRESETS -> Presets Browser/Save Slot, ALGORITHM -> Scale Browser */
    if (input->enc_delta[ADRIFT_ENC_SELECT] != 0) {
        int32_t d = input->enc_delta[ADRIFT_ENC_SELECT] % (int32_t)ADRIFT_MAX_GROUPS;
        synth->edit_group = (uint8_t)(((int32_t)synth->edit_group + d + (int32_t)ADRIFT_MAX_GROUPS) % (int32_t)ADRIFT_MAX_GROUPS);
        if (ui->in_preset_menu &&
            (ui->preset_cursor == 1u || (ui->preset_load_grp && ui->preset_previewed))) {
            apply_preset_cursor_selection(ui, synth);
        }
        adrift_sr_announce_group_select(sr, synth);
        header_dirty = top_dirty = cards_dirty = true;
    }
    if (input->enc_delta[ADRIFT_ENC_PRESETS] != 0) {
        bool was_save   = ui->in_save_menu;
        bool was_preset = ui->in_preset_menu;
        open_or_scroll_preset_menu(ui, synth, input->enc_delta[ADRIFT_ENC_PRESETS]);
        if (was_save) {
            adrift_sr_announce_save_slot(sr, ui);
        } else {
            adrift_sr_announce_preset_row(sr, ui, synth, !was_preset);
        }
        header_dirty = top_dirty = cards_dirty = true;
    }
    if (input->enc_delta[ADRIFT_ENC_ALGORITHM] != 0) {
        bool was_scale = ui->in_scale_menu;
        open_or_scroll_scale_menu(ui, synth, input->enc_delta[ADRIFT_ENC_ALGORITHM]);
        adrift_sr_announce_scale_row(sr, synth, !was_scale);
        header_dirty = top_dirty = cards_dirty = true;
    }

    /* 4. Handle 4 parameter encoders & text input across Save Modal, Presets Browser, Scale Browser, HOME, or Param Pages */
    if (ui->in_save_menu) {
        int8_t d0 = input->enc_delta[ADRIFT_ENC_KNOB1];
        int8_t d1 = input->enc_delta[ADRIFT_ENC_KNOB2];
        int8_t d2 = input->enc_delta[ADRIFT_ENC_KNOB3];
        int8_t d3 = input->enc_delta[ADRIFT_ENC_KNOB4];

        if (d0 != 0) {
            int32_t cnt  = (int32_t)ADRIFT_NUM_USER_PRESETS;
            int32_t step = (int32_t)d0 % cnt;
            ui->save_slot = (uint8_t)(((int32_t)(ui->save_slot % ADRIFT_NUM_USER_PRESETS) + step + cnt) % cnt);
            adrift_sr_announce_save_slot(sr, ui);
            top_dirty = cards_dirty = true;
        }
        if (d1 != 0) {
            ui->save_cursor = (uint8_t)dsp_clamp_i32((int32_t)ui->save_cursor + (int32_t)d1, 0, 11);
            adrift_sr_announce_save_cursor(sr, ui, false);
            top_dirty = cards_dirty = true;
        }
        if (d2 != 0) {
            uint8_t c   = ui->save_cursor % 12u;
            int32_t idx = save_charset_index(ui->save_name[c]);
            if (idx < 0) idx = 0;
            int32_t step = (int32_t)d2 % SAVE_CHARSET_COUNT;
            int32_t next = (idx + step + SAVE_CHARSET_COUNT) % SAVE_CHARSET_COUNT;
            ui->save_name[c] = k_save_charset[next];
            adrift_sr_announce_save_cursor(sr, ui, true);
            top_dirty = cards_dirty = true;
        }
        if (d3 > 0) {
            uint8_t c = ui->save_cursor % 12u;
            for (uint32_t i = c; i < 12u; i++) {
                ui->save_name[i] = ' ';
            }
            ui->save_name[12] = '\0';
            adrift_sr_announce_save_edit(sr, ui, "Cleared to end.");
            top_dirty = cards_dirty = true;
        } else if (d3 < 0) {
            for (uint32_t i = 0u; i <= 12u; i++) {
                ui->save_name[i] = ui->save_orig_name[i];
            }
            ui->save_cursor = 0u;
            adrift_sr_announce_save_edit(sr, ui, "Name restored.");
            top_dirty = cards_dirty = true;
        }

        if (input->text_cmd != ADRIFT_TEXT_CMD_NONE) {
            switch (input->text_cmd) {
                case ADRIFT_TEXT_CMD_LEFT:
                    if (ui->save_cursor > 0u) ui->save_cursor--;
                    adrift_sr_announce_save_cursor(sr, ui, false);
                    break;
                case ADRIFT_TEXT_CMD_RIGHT:
                    if (ui->save_cursor < 11u) ui->save_cursor++;
                    adrift_sr_announce_save_cursor(sr, ui, false);
                    break;
                case ADRIFT_TEXT_CMD_BACKSPACE:
                    save_modal_backspace(ui);
                    adrift_sr_announce_save_edit(sr, ui, "Character deleted.");
                    break;
                case ADRIFT_TEXT_CMD_DELETE:
                    save_modal_delete_at(ui);
                    adrift_sr_announce_save_edit(sr, ui, "Character deleted.");
                    break;
                case ADRIFT_TEXT_CMD_CLEAR:
                    for (uint32_t i = 0u; i < 12u; i++) ui->save_name[i] = ' ';
                    ui->save_name[12] = '\0';
                    ui->save_cursor   = 0u;
                    adrift_sr_announce_save_edit(sr, ui, "Name cleared.");
                    break;
                default:
                    break;
            }
            top_dirty = cards_dirty = true;
        }
        if (input->text_char != 0) {
            int32_t ci = save_charset_index(input->text_char);
            if (ci >= 0) {
                uint8_t c = ui->save_cursor % 12u;
                ui->save_name[c] = k_save_charset[ci];
                adrift_sr_announce_save_cursor(sr, ui, false);
                if (c < 11u) ui->save_cursor = (uint8_t)(c + 1u);
                top_dirty = cards_dirty = true;
            }
        }
    } else if (ui->in_preset_menu) {
        int8_t d0 = input->enc_delta[ADRIFT_ENC_KNOB1];
        int8_t d1 = input->enc_delta[ADRIFT_ENC_KNOB2];
        int8_t d2 = input->enc_delta[ADRIFT_ENC_KNOB3];
        int8_t d3 = input->enc_delta[ADRIFT_ENC_KNOB4];

        if (d0 != 0) {
            int32_t cnt = (int32_t)ADRIFT_PRESET_BANK_COUNT;
            int32_t step = (int32_t)d0 % cnt;
            ui->preset_bank = (uint8_t)(((int32_t)(ui->preset_bank % ADRIFT_PRESET_BANK_COUNT) + step + cnt) % cnt);
            ui->preset_clear_armed = false;
            uint8_t orig = ui->preset_snapshot.preset_index;
            if (ui->preset_bank == ADRIFT_PRESET_BANK_USER && ui->preset_cursor < ADRIFT_UI_PRESET_FACT_COUNT) {
                ui->preset_cursor = (orig >= ADRIFT_USER_PRESET_BASE &&
                                     orig < ADRIFT_USER_PRESET_BASE + ADRIFT_NUM_USER_PRESETS)
                                        ? (uint8_t)(2u + orig)
                                        : ADRIFT_UI_PRESET_FACT_COUNT;
                if (ui->preset_load_grp && !preset_logical_is_empty_user(ui->preset_cursor)) {
                    ui->preset_src_grp = preset_auto_pick_src_grp((uint8_t)(ui->preset_cursor - 2u), ui->preset_src_grp);
                }
                apply_preset_cursor_selection(ui, synth);
            } else if (ui->preset_bank == ADRIFT_PRESET_BANK_FACT && ui->preset_cursor >= ADRIFT_UI_PRESET_FACT_COUNT) {
                ui->preset_cursor = (orig == ADRIFT_PRESET_INIT) ? 0u
                                  : ((orig < ADRIFT_NUM_PRESETS) ? (uint8_t)(2u + orig) : 2u);
                if (ui->preset_load_grp && ui->preset_cursor >= 2u) {
                    ui->preset_src_grp = preset_auto_pick_src_grp((uint8_t)(ui->preset_cursor - 2u), ui->preset_src_grp);
                }
                apply_preset_cursor_selection(ui, synth);
            }
            adrift_sr_announce_preset_bank(sr, ui, synth);
            header_dirty = top_dirty = cards_dirty = true;
        }
        if (d1 != 0) {
            bool next_grp = (d1 > 0);
            if (next_grp != ui->preset_load_grp) {
                ui->preset_load_grp    = next_grp;
                ui->preset_clear_armed = false;
                if (ui->preset_load_grp) {
                    if (ui->preset_previewed) {
                        adrift_synth_restore_snapshot(synth, &ui->preset_snapshot);
                        ui->preset_previewed = false;
                    }
                    ui->preset_src_grp = (uint8_t)(synth->edit_group % ADRIFT_MAX_GROUPS);
                    if (ui->preset_cursor >= 2u && !preset_logical_is_empty_user(ui->preset_cursor)) {
                        ui->preset_src_grp = preset_auto_pick_src_grp((uint8_t)(ui->preset_cursor - 2u),
                                                                      ui->preset_src_grp);
                    }
                }
                if (ui->preset_previewed || ui->preset_load_grp) {
                    apply_preset_cursor_selection(ui, synth);
                }
                header_dirty = top_dirty = cards_dirty = true;
            }
            adrift_sr_announce_preset_load_mode(sr, ui, synth);
        }
        if (d2 != 0) {
            if (!ui->preset_load_grp) {
                adrift_sr_announce_preset_src_grp(sr, ui, true);
            } else {
                uint8_t next_src = (uint8_t)dsp_clamp_i32((int32_t)ui->preset_src_grp + (int32_t)d2,
                                                          0, (int32_t)ADRIFT_MAX_GROUPS - 1);
                if (next_src != ui->preset_src_grp) {
                    ui->preset_src_grp     = next_src;
                    ui->preset_clear_armed = false;
                    if (ui->preset_cursor >= 2u && !preset_logical_is_empty_user(ui->preset_cursor)) {
                        apply_preset_cursor_selection(ui, synth);
                    }
                    header_dirty = top_dirty = cards_dirty = true;
                }
                adrift_sr_announce_preset_src_grp(sr, ui, false);
            }
        }
        if (d3 != 0) {
            if (preset_logical_is_occ_user(ui->preset_cursor)) {
                ui->preset_clear_armed = (d3 > 0);
                adrift_sr_announce_preset_clear_arm(sr, ui, ui->preset_clear_armed, false);
                top_dirty = cards_dirty = true;
            } else {
                adrift_sr_announce_preset_clear_arm(sr, ui, false, true);
            }
        }
    } else if (ui->in_scale_menu) {
        int8_t root_delta = input->enc_delta[ADRIFT_ENC_KNOB1];
        if (root_delta != 0) {
            int32_t d = (int32_t)root_delta % 12;
            synth->kbd_scale_root = (uint8_t)(((int32_t)(synth->kbd_scale_root % 12u) + d + 12) % 12);
            ui->scale_previewed   = true;
            top_dirty             = true;
            cards_dirty           = true;
            adrift_sr_announce_scale_root(sr, synth);
        }
        for (uint8_t k = 1u; k < ADRIFT_NUM_PARAM_KNOBS; k++) {
            if (input->enc_delta[k] != 0) {
                adrift_sr_announce_unused_knob(sr, k, "Turn algorithm knob for scale or knob 1 for root.");
            }
        }
    } else if (ui->in_scope && !ui->in_preset_menu) {
        for (uint8_t k = 0; k < ADRIFT_NUM_PARAM_KNOBS; k++) {
            int8_t delta = input->enc_delta[k];
            if (delta == 0) continue;

            uint8_t sp = ui->scope_page % ADRIFT_UI_NUM_HOME_PAGES;
            switch (k) {
                case 0: /* 1:TIME/ZOOM (1X, 2X, 4X, 8X) */
                    ui->scope_zoom = (uint8_t)dsp_clamp_i32((int32_t)ui->scope_zoom + (int32_t)delta, 0, 3);
                    break;
                case 1: /* 2:GAIN (25% .. 400%) */
                    ui->scope_gain = (uint16_t)dsp_clamp_i32((int32_t)ui->scope_gain + (int32_t)delta * 25, 25, 400);
                    break;
                case 2: /* 3:FREEZE (RUN / HOLD) */
                    ui->scope_freeze = (delta > 0);
                    break;
                case 3: /* 4:MODE (0..2 per HOME page, wraps cyclically) */
                default: {
                    int32_t cnt = (int32_t)ADRIFT_UI_NUM_HOME_MODES;
                    int32_t d   = (int32_t)delta % cnt;
                    ui->scope_mode[sp] = (uint8_t)(((int32_t)ui->scope_mode[sp] + d + cnt) % cnt);
                    break;
                }
            }
            adrift_sr_announce_home_knob(sr, ui, k);
            top_dirty = true;
        }
    } else if (!ui->in_scope && !ui->in_preset_menu) {
        for (uint8_t k = 0; k < ADRIFT_NUM_PARAM_KNOBS; k++) {
            int8_t delta = input->enc_delta[k];
            if (delta == 0) continue;

            uint8_t num_sub   = page_num_subpages(ui->page, ui->in_group_menu, ui->in_arp_menu, ui->in_fx_menu);
            uint8_t param_idx = (uint8_t)((ui->subpage % num_sub) * 4u + k);
            if (!ui->in_group_menu && !ui->in_arp_menu && !ui->in_fx_menu &&
                is_unmapped_param_slot(ui->page, param_idx)) {
                const char *hint = (ui->page == 0u) ? "Press S E L for other exciter controls."
                                 : (ui->page == 3u) ? "Press edit for other envelope 2 controls."
                                                    : "Press global for other modulation controls.";
                adrift_sr_announce_unused_knob(sr, k, hint);
                continue;
            }

            uint8_t prev_nv   = adrift_synth_active_group(synth)->num_voices;
            uint8_t prev_mode = adrift_synth_active_group(synth)->assign_mode;
            apply_encoder_for_synth(synth, ui->in_group_menu, ui->in_arp_menu, ui->in_fx_menu, ui->page, param_idx, delta);
            if (ui->in_group_menu && param_idx == 1u) {
                adrift_group_t *ag = &synth->groups[synth->edit_group % ADRIFT_MAX_GROUPS];
                if (!adrift_group_is_stacked(prev_mode) &&
                    adrift_group_is_stacked(ag->assign_mode)) {
                    /* Auto-seed 16C detune and 80% stereo spread when switching from R-ROBN to UNISON/SUPER */
                    if (ag->unison_detune == 0 && input->enc_delta[3] == 0) ag->unison_detune = 370;
                    if (ag->stereo_spread == 0 && input->enc_delta[2] == 0) ag->stereo_spread = 820;
                }
            }
            if (ui->in_group_menu) {
                uint8_t cur_nv = adrift_synth_active_group(synth)->num_voices;
                if (param_idx == 0u && delta > 0 && cur_nv == prev_nv && cur_nv < ADRIFT_MAX_VOICES) {
                    adrift_sr_say(sr, ADRIFT_SR_PRIO_KEEP, ADRIFT_SR_CAT_WARNING,
                                  "Cannot add voices, all 16 are in use. Lower voices in another group first.");
                } else {
                    adrift_sr_announce_group_knob(sr, ui, synth, k);
                }
            } else if (ui->in_arp_menu) {
                adrift_sr_announce_arp_knob(sr, ui, synth, k);
            } else if (ui->in_fx_menu) {
                adrift_sr_announce_fx_knob(sr, ui, synth, k);
            } else {
                adrift_sr_announce_main_knob(sr, ui, synth, k);
            }
            cards_dirty = true;
            top_dirty   = true;
            if (ui->in_group_menu && adrift_synth_active_group(synth)->num_voices != prev_nv) {
                header_dirty = true;
            }
        }
    }

    /* 5. Track voice activity & determine active screen layout mode */
    uint8_t  g          = synth->edit_group % ADRIFT_MAX_GROUPS;
    const adrift_group_t *gp = &synth->groups[g];
    uint8_t  meter_w    = (uint8_t)compute_header_bbd_width(synth);
    if (ui->boot_bar_decay > 0u) {
        uint8_t decay_w = (uint8_t)(((uint32_t)ui->boot_bar_decay * 230u) / 12u);
        ui->boot_bar_decay--;
        if (decay_w > meter_w) {
            meter_w = decay_w;
        }
    }
    uint8_t  disp_note  = gp->last_note ? gp->last_note : 60u;
    bool     grp_gate   = compute_group_gate(synth, g);
    uint32_t pip_sig    = compute_voice_pip_sig(synth);
    uint8_t  arp_step   = (uint8_t)(gp->arp_state.step_counter & 7u);
    uint8_t  fx_gr_px   = (synth->fx_params.comp_mix > 0)
                              ? (uint8_t)((synth->fx.meter_comp_gr_q15 * 60) >> 15)
                              : 0u;
    bool     pips_dirty = (pip_sig != ui->prev_pip_sig) ||
                          (ui->in_arp_menu && arp_step != ui->prev_arp_step) ||
                          (ui->in_fx_menu && fx_gr_px != ui->prev_fx_gr);
    bool     note_dirty = (disp_note != ui->prev_note);
    ui->prev_pip_sig    = pip_sig;
    ui->prev_arp_step   = arp_step;
    ui->prev_fx_gr      = fx_gr_px;
    if (ui->in_fx_menu) {
        const adrift_fx_params_t *fp = &synth->fx_params;
        uint32_t fx_sig = ((uint32_t)fp->chorus_mix) ^
                          ((uint32_t)fp->delay_time << 3) ^
                          ((uint32_t)fp->delay_fb << 6) ^
                          ((uint32_t)fp->delay_mix << 9) ^
                          ((uint32_t)fp->reverb_size << 12) ^
                          ((uint32_t)fp->reverb_damp << 15) ^
                          ((uint32_t)fp->reverb_shimmer << 18) ^
                          ((uint32_t)fp->reverb_mix << 21) ^
                          ((uint32_t)fp->comp_mix << 1) ^
                          ((uint32_t)fp->comp_thresh << 7) ^
                          ((uint32_t)fp->comp_attack << 13) ^
                          ((uint32_t)fp->comp_makeup << 19);
        if (fx_sig != ui->prev_fx_sig) {
            ui->prev_fx_sig = fx_sig;
            top_dirty       = true;
            cards_dirty     = true;
        }
    }
    if (synth->midi_clk_active != ui->prev_midi_clk_active ||
        (synth->midi_clk_active && synth->midi_clk_bpm != ui->prev_midi_clk_bpm)) {
        ui->prev_midi_clk_active = synth->midi_clk_active;
        ui->prev_midi_clk_bpm    = synth->midi_clk_bpm;
        if (ui->in_arp_menu || ui->in_fx_menu) {
            top_dirty   = true;
            cards_dirty = true;
        }
    }

    synth->scope_frozen = ui->in_scope && ui->scope_freeze && !ui->in_save_menu && !ui->in_preset_menu && !ui->in_scale_menu;
    uint8_t  cur_scope_p = ui->scope_page % ADRIFT_UI_NUM_HOME_PAGES;
    uint8_t  cur_scope_m = ui->scope_mode[cur_scope_p] % ADRIFT_UI_NUM_HOME_MODES;
    uint32_t save_sig    = ui->in_save_menu
                               ? (hash_str12(ui->save_name) ^ ((uint32_t)ui->save_slot << 16) ^ ((uint32_t)ui->save_cursor << 24))
                               : 0u;
    uint32_t pname_sig   = hash_str12(synth->preset_name);
    bool     toast_on    = (ui->save_toast_frames > 0u);

    ui_layout_mode_t mode = ui->in_save_menu   ? UI_LAYOUT_PRESET_SAVE
                          : ui->in_preset_menu ? UI_LAYOUT_PRESET_BROWSER
                          : ui->in_scale_menu  ? UI_LAYOUT_SCALE_BROWSER
                          : (ui->in_scope ? UI_LAYOUT_FULL_SCOPE : UI_LAYOUT_PARAM_SCREEN);
    if ((uint8_t)mode != ui->prev_layout_mode ||
        ui->in_save_menu != ui->prev_in_save_menu ||
        (ui->in_save_menu && save_sig != ui->prev_save_sig) ||
        ui->in_preset_menu != ui->prev_in_preset_menu ||
        (ui->in_preset_menu && (ui->preset_cursor != ui->prev_preset_cursor ||
                                ui->preset_bank != ui->prev_preset_bank ||
                                ui->preset_load_grp != ui->prev_preset_load_grp ||
                                ui->preset_src_grp != ui->prev_preset_src_grp ||
                                ui->preset_clear_armed != ui->prev_preset_clear_armed)) ||
        ui->in_scale_menu != ui->prev_in_scale_menu ||
        (ui->in_scale_menu && (ui->scale_cursor != ui->prev_scale_cursor ||
                               synth->kbd_scale_root != ui->prev_scale_root)) ||
        ui->in_scope != ui->prev_in_scope ||
        (ui->in_scope && (cur_scope_p != ui->prev_scope_page ||
                          ui->scope_zoom != ui->prev_scope_zoom ||
                          ui->scope_gain != ui->prev_scope_gain ||
                          ui->scope_freeze != ui->prev_scope_freeze ||
                          cur_scope_m != ui->prev_scope_mode)) ||
        ui->in_arp_menu != ui->prev_in_arp_menu ||
        ui->in_fx_menu != ui->prev_in_fx_menu ||
        ui->page != ui->prev_page ||
        ui->subpage != ui->prev_subpage) {
        ui->prev_layout_mode        = (uint8_t)mode;
        ui->prev_in_save_menu       = ui->in_save_menu;
        ui->prev_save_sig           = save_sig;
        ui->prev_in_preset_menu     = ui->in_preset_menu;
        ui->prev_preset_cursor      = ui->preset_cursor;
        ui->prev_preset_bank        = ui->preset_bank;
        ui->prev_preset_load_grp    = ui->preset_load_grp;
        ui->prev_preset_src_grp     = ui->preset_src_grp;
        ui->prev_preset_clear_armed = ui->preset_clear_armed;
        ui->prev_in_scale_menu      = ui->in_scale_menu;
        ui->prev_scale_cursor       = ui->scale_cursor;
        ui->prev_scale_root         = synth->kbd_scale_root;
        ui->prev_in_scope           = ui->in_scope;
        ui->prev_scope_page         = cur_scope_p;
        ui->prev_scope_zoom         = ui->scope_zoom;
        ui->prev_scope_gain         = ui->scope_gain;
        ui->prev_scope_freeze       = ui->scope_freeze;
        ui->prev_scope_mode         = cur_scope_m;
        ui->prev_in_arp_menu        = ui->in_arp_menu;
        ui->prev_in_fx_menu         = ui->in_fx_menu;
        ui->prev_page               = ui->page;
        ui->prev_subpage            = ui->subpage;
        top_dirty   = true;
        cards_dirty = true;
    }

    if (synth->preset_index != ui->prev_preset ||
        pname_sig != ui->prev_preset_name_sig ||
        gp->num_voices != ui->prev_group_voices ||
        g != ui->prev_edit_group ||
        ui->in_group_menu != ui->prev_in_group_menu ||
        ui->in_arp_menu != ui->prev_in_arp_menu ||
        ui->in_fx_menu != ui->prev_in_fx_menu) {
        header_dirty = top_dirty = cards_dirty = true;
    }
    bool meter_only_dirty = false;
    bool cpu_only_dirty   = false;
    uint8_t cpu_diff = (input->cpu_load_pct > ui->prev_cpu_pct)
                           ? (uint8_t)(input->cpu_load_pct - ui->prev_cpu_pct)
                           : (uint8_t)(ui->prev_cpu_pct - input->cpu_load_pct);
    bool cpu_needs_full = (cpu_diff >= 2u) &&
                          (synth->kbd_octave != 0 || disp_note < 12u ||
                           input->cpu_load_pct >= 100u || ui->prev_cpu_pct >= 100u);
    if (header_dirty ||
        synth->preset_index != ui->prev_preset ||
        pname_sig != ui->prev_preset_name_sig ||
        toast_on != ui->prev_save_toast ||
        g != ui->prev_edit_group ||
        gp->num_voices != ui->prev_group_voices ||
        synth->kbd_octave != ui->prev_kbd_octave ||
        ui->in_group_menu != ui->prev_in_group_menu ||
        disp_note != ui->prev_note ||
        grp_gate != ui->prev_gate ||
        cpu_needs_full) {
        header_dirty              = true;
        ui->prev_preset           = synth->preset_index;
        ui->prev_preset_name_sig  = pname_sig;
        ui->prev_save_toast       = toast_on;
        ui->prev_edit_group       = g;
        ui->prev_group_voices     = gp->num_voices;
        ui->prev_kbd_octave       = synth->kbd_octave;
        ui->prev_in_group_menu    = ui->in_group_menu;
        ui->prev_note             = disp_note;
        ui->prev_gate             = grp_gate;
        ui->prev_meter_w          = meter_w;
        ui->prev_cpu_pct          = input->cpu_load_pct;
    } else {
        if (cpu_diff >= 2u && synth->kbd_octave == 0) {
            cpu_only_dirty   = true;
            ui->prev_cpu_pct = input->cpu_load_pct;
        }
        if (meter_w != ui->prev_meter_w) {
            meter_only_dirty = true;
            ui->prev_meter_w = meter_w;
        }
    }

    /* 6. Render Header (fast micro-updates when only CPU% or BBD energy meter changed) */
    if (header_dirty) {
        uint8_t toast_u1 = toast_on ? (uint8_t)((ui->save_toast_slot % ADRIFT_NUM_USER_PRESETS) + 1u) : 0u;
        draw_header(fb, synth, input->cpu_load_pct, meter_w, ui->in_group_menu, toast_u1);
        adrift_dirty_mark_rows(dirty, 0u, 28u);
    } else {
        if (cpu_only_dirty) {
            draw_header_cpu_pct(fb, input->cpu_load_pct);
            adrift_dirty_mark_rows(dirty, 8u, 7u);
        }
        if (meter_only_dirty) {
            draw_header_bbd_bar(fb, meter_w, get_group_theme(g)->accent);
            adrift_dirty_mark_rows(dirty, 21u, 3u);
        }
    }

    /* 7. Render Main Area according to Layout Mode */
    ui->frame_count++;
    if (mode == UI_LAYOUT_PRESET_SAVE) {
        if (top_dirty || cards_dirty || dirty->full_redraw || pips_dirty) {
            draw_preset_save_modal(fb, ui, synth);
            adrift_dirty_mark_rows(dirty, 28u, 212u);
        }
    } else if (mode == UI_LAYOUT_PRESET_BROWSER) {
        if (top_dirty || cards_dirty || dirty->full_redraw || pips_dirty) {
            draw_preset_browser(fb, ui, synth);
            adrift_dirty_mark_rows(dirty, 28u, 212u);
        }
    } else if (mode == UI_LAYOUT_SCALE_BROWSER) {
        if (top_dirty || cards_dirty || dirty->full_redraw || pips_dirty) {
            draw_scale_browser(fb, ui, synth);
            adrift_dirty_mark_rows(dirty, 28u, 212u);
        }
    } else if (mode == UI_LAYOUT_FULL_SCOPE) {
        draw_full_scope(fb, ui, synth,
                        top_dirty || dirty->full_redraw,
                        pips_dirty, note_dirty, dirty);
    } else {
        /* UI_LAYOUT_PARAM_SCREEN: Top Visualizer Pane (y=28..135) + 1x4 Parameter Card Row (y=136..239) */
        if (top_dirty || dirty->full_redraw || pips_dirty) {
            fb_fill_rect(fb, 0, 28, ADRIFT_SCREEN_W, 108, COL_BG);
            if (ui->in_group_menu) {
                draw_group_matrix(fb, synth, ui->subpage);
            } else if (ui->in_arp_menu) {
                draw_arp_pane(fb, synth, ui->subpage);
            } else if (ui->in_fx_menu) {
                draw_fx_pane(fb, synth, ui->subpage);
            } else {
                switch (ui->page % ADRIFT_UI_NUM_PAGES) {
                    case 0: draw_exc_pane(fb, synth, ui->subpage); break;
                    case 1: draw_env_graph(fb, synth, true, ui->subpage); break;
                    case 2: draw_bbd_pane(fb, synth, ui->subpage); break;
                    case 3: draw_env_graph(fb, synth, false, ui->subpage); break;
                    case 4:
                    default: draw_mod_pane(fb, synth, ui->subpage); break;
                }
            }
            adrift_dirty_mark_rows(dirty, 28u, 108u);
        }
        if (cards_dirty || dirty->full_redraw) {
            draw_cards_1x4(fb, synth, ui->page, ui->subpage, ui->in_group_menu, ui->in_arp_menu, ui->in_fx_menu);
            adrift_dirty_mark_rows(dirty, 136u, 104u);
        }
    }
}
