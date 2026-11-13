/* SPDX-License-Identifier: MPL-2.0
 * Top Visualizer Panes (Group Matrix, Envelope Graph, EXC, BBD, MOD, ARP, FX) */

#include "ui_internal.h"

/* 6-Column Voice-Group Allocation & Routing Matrix (shown when SAVE Group Settings screen is open) */
void draw_group_matrix(uint16_t *fb, const adrift_synth_t *s, uint8_t subpage) {
    uint8_t eg = s->edit_group % ADRIFT_MAX_GROUPS;
    const adrift_group_theme_t *eth = get_group_theme(eg);
    const uint32_t sx = 5u, sy = 30u, sw = 230u, sh = 105u;
    uint8_t used_v = adrift_synth_used_voices(s);
    uint8_t free_v = adrift_synth_free_voices(s);

    fb_fill_rect(fb, sx, sy, sw, sh, COL_SCOPE_BG);
    fb_rect_outline(fb, sx, sy, sw, sh, eth->border);

    draw_screen_page_badge(fb, sx, sy, "GRP", subpage, eth);

    char ubuf[16];
    ubuf[0] = 'U'; ubuf[1] = 'S'; ubuf[2] = 'E'; ubuf[3] = 'D'; ubuf[4] = ' ';
    fmt_int(&ubuf[5], used_v, "/16");
    fb_draw_str_spec(fb, sx + 74u, sy + 5u, ubuf, COL_MUTED, &k_font_sm_bold);
    draw_voice_pips(fb, s, sx, sy + 1u, sw);

    for (uint8_t g = 0; g < ADRIFT_MAX_GROUPS; g++) {
        const adrift_group_t       *gp = &s->groups[g];
        const adrift_group_theme_t *gt = get_group_theme(g);
        bool selected = (g == eg);
        bool enabled  = (gp->num_voices > 0u);

        uint32_t cx = sx + 4u + (uint32_t)g * 37u;
        uint32_t cy = sy + 19u;
        uint32_t cw = 36u;
        uint32_t ch = 82u;

        fb_fill_rect(fb, cx, cy, cw, ch, enabled ? COL_CARD : COL_SCOPE_BG);
        if (selected) {
            fb_rect_outline(fb, cx, cy, cw, ch, COL_WHITE);
            fb_hline(fb, cx + 1u, cy + 1u, cw - 2u, gt->accent);
        } else {
            fb_rect_outline(fb, cx, cy, cw, ch, enabled ? gt->border : COL_BAR_TRACK);
        }

        /* Group header: G1..G6 + assignment mode ('R' = Round-Robin, 'U' = Unison, 'S' = Super) */
        char gname[3] = { 'G', (char)('1' + g), '\0' };
        fb_draw_str_spec(fb, cx + 3u, cy + 4u, gname, (enabled || selected) ? gt->accent : COL_MUTED, &k_font_sm_bold);
        char mch = (gp->assign_mode == ADRIFT_GROUP_SUPER)  ? 'S'
                 : (gp->assign_mode == ADRIFT_GROUP_UNISON) ? 'U' : 'R';
        fb_draw_char_spec(fb, cx + 26u, cy + 4u, mch,
                          enabled ? (adrift_group_is_stacked(gp->assign_mode) ? COL_WHITE : COL_MUTED)
                                  : COL_BAR_TRACK,
                          &k_font_sm_bold);

        /* 16 voice-slot blocks arranged in a 2x8 bottom-up grid */
        for (uint8_t slot = 0; slot < ADRIFT_MAX_VOICES; slot++) {
            uint32_t row = (uint32_t)(slot >> 1);
            uint32_t col = (uint32_t)(slot & 1u);
            uint32_t bx  = cx + 4u + col * 15u;
            uint32_t by  = cy + 56u - row * 5u;
            uint16_t bcol = COL_BAR_TRACK;
            if (slot < gp->num_voices) {
                bcol = gt->accent;
            } else if (selected && slot < (uint8_t)(gp->num_voices + free_v)) {
                bcol = COL_SCOPE_GRID;
            }
            fb_fill_rect(fb, bx, by, 13u, 3u, bcol);
        }

        /* Bottom MIDI channel or OFF tag */
        if (!enabled) {
            fb_draw_str_centered_spec(fb, cx, cw, cy + 68u, "OFF", selected ? COL_WHITE : COL_MUTED, &k_font_sm_bold);
        } else {
            char chbuf[8];
            chbuf[0] = 'C';
            chbuf[1] = 'H';
            fmt_int(&chbuf[2], gp->midi_channel, "");
            fb_draw_str_centered_spec(fb, cx, cw, cy + 68u, chbuf, COL_WHITE, &k_font_sm_bold);
        }
    }
}

/* Analog RC attack (1.20x overshoot target -> 1.0) & exponential decay/release (tau = T/3) LUTs in Q15 */
static const uint16_t k_rc_atk_q15[17] = {
    0, 4157, 7875, 11201, 14175, 16835, 19214, 21342, 23245,
    24947, 26470, 27831, 29049, 30138, 31112, 31983, 32768
};

static const uint16_t k_rc_dec_q15[17] = {
    32768, 26888, 22014, 17976, 14630, 11858, 9561, 7658, 6081,
    4775, 3692, 2796, 2053, 1438, 928, 506, 0
};

static uint32_t interp_lut16_q15(const uint16_t lut[17], uint32_t pos, uint32_t width) {
    if (width == 0u || pos >= width) return lut[16];
    uint32_t idx_q8 = (pos << 12) / width;
    uint32_t idx    = idx_q8 >> 8;
    int32_t  frac   = (int32_t)(idx_q8 & 0xFFu);
    int32_t  a      = (int32_t)lut[idx];
    int32_t  b      = (int32_t)lut[idx + 1u];
    return (uint32_t)(a + (((b - a) * frac) >> 8));
}

static void draw_env_node(uint16_t *fb, uint32_t x, uint32_t y, uint16_t col) {
    fb_fill_rect(fb, x - 1u, y - 1u, 3u, 3u, col);
    fb[y * ADRIFT_SCREEN_W + x] = COL_WHITE;
}

static void draw_env_vline_dotted(uint16_t *fb, uint32_t x, uint32_t y0, uint32_t y1) {
    for (uint32_t y = y0; y <= y1; y += 3u) {
        fb[y * ADRIFT_SCREEN_W + x] = COL_SCOPE_GRID;
    }
}

void draw_env_graph(uint16_t *fb, const adrift_synth_t *s, bool is_env1, uint8_t subpage) {
    const adrift_group_t       *gp = adrift_synth_active_group(s);
    const adrift_patch_t       *vp = &gp->patch;
    const adrift_env_params_t  *ep = is_env1 ? &vp->env1 : &vp->env2;
    const adrift_group_theme_t *th = get_group_theme(s->edit_group);
    const uint32_t sx = 5u, sy = 30u, sw = 230u, sh = 105u;

    fb_fill_rect(fb, sx, sy, sw, sh, COL_SCOPE_BG);
    fb_rect_outline(fb, sx, sy, sw, sh, th->border);

    /* Top-left envelope screen & page badge + mode badge in 7x10 font */
    draw_screen_page_badge(fb, sx, sy, is_env1 ? "ENV1" : "ENV2", subpage, th);

    bool has_right_tag = (gp->num_voices == 0u) || gp->sustain_pedal ||
                         (is_env1 && ep->range != ADRIFT_ENV_RANGE_NORM);
    if (is_env1) {
        if (ep->loop_mode == ADRIFT_ENV_LOOP_AD) {
            fb_draw_str_spec(fb, sx + 73u, sy + 4u, has_right_tag ? "LP A-D" : "LOOP A-D", COL_WHITE, &k_font_md);
        } else if (ep->loop_mode == ADRIFT_ENV_LOOP_DAD) {
            fb_draw_str_spec(fb, sx + 73u, sy + 4u, has_right_tag ? "LP DAD" : "LOOP DAD", COL_WHITE, &k_font_md);
        } else if (ep->onset_delay > 0) {
            fb_draw_str_spec(fb, sx + 73u, sy + 4u, "DADSR", COL_MUTED, &k_font_md);
        } else {
            fb_draw_str_spec(fb, sx + 73u, sy + 4u, "ADSR", COL_MUTED, &k_font_md);
        }
    } else {
        if (ep->drone_hold) {
            fb_draw_str_spec(fb, sx + 73u, sy + 4u, "DRONE", COL_WHITE, &k_font_md);
        } else if (ep->onset_delay > 0) {
            fb_draw_str_spec(fb, sx + 73u, sy + 4u, "DADSR", COL_MUTED, &k_font_md);
        } else {
            fb_draw_str_spec(fb, sx + 73u, sy + 4u, "ADSR", COL_MUTED, &k_font_md);
        }
    }
    if (gp->num_voices == 0u) {
        fb_draw_str_spec(fb, sx + 140u, sy + 4u, "OFF", COL_MUTED, &k_font_md);
    } else if (gp->sustain_pedal) {
        fb_draw_str_spec(fb, sx + 140u, sy + 4u, "SUS", th->accent, &k_font_md);
    } else if (is_env1 && ep->range != ADRIFT_ENV_RANGE_NORM) {
        fb_draw_str_spec(fb, sx + 132u, sy + 5u,
                         (ep->range == ADRIFT_ENV_RANGE_SHORT) ? "250MS" : "25SEC",
                         th->accent, &k_font_sm_bold);
    }

    draw_voice_pips(fb, s, sx, sy + 1u, sw);

    /* Plot bounds: 212 columns (dx = 0..211), height 82 px (gy_top = 47 .. gy_bot = 129) */
    const uint32_t gx0    = sx + 9u;
    const uint32_t gw     = 211u;
    const uint32_t gy_top = sy + 17u;
    const uint32_t gy_bot = sy + sh - 6u;
    const uint32_t gh     = gy_bot - gy_top;

    uint32_t dly_k = (uint32_t)dsp_clamp_i32(ep->onset_delay, 0, 1023);
    uint32_t atk_k = (uint32_t)dsp_clamp_i32(ep->attack,      0, 1023);
    uint32_t dec_k = (uint32_t)dsp_clamp_i32(ep->decay,       0, 1023);
    uint32_t sus_k = (uint32_t)dsp_clamp_i32(ep->sustain,     0, 1023);
    uint32_t rel_k = (uint32_t)dsp_clamp_i32(ep->release,     0, 1023);
    uint32_t sus_q15 = (sus_k * 32768u) / 1023u;

    uint8_t  y_pts[212];
    uint16_t fill_col = make_env_fill_col(th->accent);

    if (is_env1 && ep->loop_mode != ADRIFT_ENV_LOOP_OFF) {
        uint32_t loop_floor_q15 = (sus_k > 1000u) ? 0u : sus_q15;
        uint32_t w_rel          = 6u + (rel_k * 42u) / 1023u;
        uint32_t c_dly          = (dly_k > 0u) ? (4u + (dly_k * 28u) / 1023u) : 0u;
        uint32_t c_atk          = 2u + (atk_k * 30u) / 1023u;
        uint32_t c_dec          = 5u + (dec_k * 34u) / 1023u;
        if (ep->range == ADRIFT_ENV_RANGE_SHORT) {
            w_rel = 4u + (rel_k * 20u) / 1023u;
            c_atk = 2u + (atk_k * 14u) / 1023u;
            c_dec = 3u + (dec_k * 16u) / 1023u;
        } else if (ep->range == ADRIFT_ENV_RANGE_LONG) {
            w_rel = 8u + (rel_k * 52u) / 1023u;
            c_atk = 4u + (atk_k * 42u) / 1023u;
            c_dec = 7u + (dec_k * 46u) / 1023u;
        }
        uint32_t x_gate_off = gw - w_rel;
        uint32_t c_sum = c_dly + c_atk + c_dec;
        if (c_sum > 54u) {
            if (c_dly > 0u) {
                c_dly = (c_dly * 54u) / c_sum;
                if (c_dly < 2u) c_dly = 2u;
            }
            c_atk = (c_atk * 54u) / c_sum;
            if (c_atk < 2u) c_atk = 2u;
            c_dec = (c_dec * 54u) / c_sum;
            if (c_dec < 3u) c_dec = 3u;
        }

        uint32_t lvl_at_off = 0u;
        for (uint32_t dx = 0; dx <= gw; dx++) {
            uint32_t lvl_q15 = 0u;
            if (dx <= x_gate_off) {
                if (ep->loop_mode == ADRIFT_ENV_LOOP_AD) {
                    if (dx >= c_dly) {
                        uint32_t cyc      = c_atk + c_dec;
                        uint32_t rel_dx   = dx - c_dly;
                        uint32_t u        = rel_dx % cyc;
                        uint32_t base_q15 = (rel_dx < cyc) ? 0u : loop_floor_q15;
                        if (u <= c_atk) {
                            lvl_q15 = base_q15 + (((32768u - base_q15) * interp_lut16_q15(k_rc_atk_q15, u, c_atk)) >> 15);
                        } else {
                            lvl_q15 = loop_floor_q15 + (((32768u - loop_floor_q15) * interp_lut16_q15(k_rc_dec_q15, u - c_atk, c_dec)) >> 15);
                        }
                    }
                } else {
                    uint32_t cyc = c_dly + c_atk + c_dec;
                    uint32_t u   = dx % cyc;
                    if (u >= c_dly) {
                        if (u <= c_dly + c_atk) {
                            uint32_t base_q15 = (c_dly > 0u || dx < cyc) ? 0u : loop_floor_q15;
                            lvl_q15 = base_q15 + (((32768u - base_q15) * interp_lut16_q15(k_rc_atk_q15, u - c_dly, c_atk)) >> 15);
                        } else {
                            lvl_q15 = loop_floor_q15 + (((32768u - loop_floor_q15) * interp_lut16_q15(k_rc_dec_q15, u - c_dly - c_atk, c_dec)) >> 15);
                        }
                    }
                }
                if (dx == x_gate_off) {
                    lvl_at_off = lvl_q15;
                }
            } else {
                uint32_t rpos = dx - x_gate_off;
                if (rpos <= w_rel) {
                    lvl_q15 = (lvl_at_off * interp_lut16_q15(k_rc_dec_q15, rpos, w_rel)) >> 15;
                }
            }
            y_pts[dx] = (uint8_t)(gy_bot - ((lvl_q15 * gh + 16384u) >> 15));
        }

        for (uint32_t dx = 0; dx <= gw; dx++) {
            uint32_t py = y_pts[dx];
            if (py + 1u <= gy_bot) {
                fb_fill_rect(fb, gx0 + dx, py + 1u, 1u, gy_bot - py, fill_col);
            }
        }
        for (uint32_t dx = 0; dx <= gw; dx += 4u) {
            fb[gy_top * ADRIFT_SCREEN_W + gx0 + dx] = COL_SCOPE_GRID;
            fb[gy_bot * ADRIFT_SCREEN_W + gx0 + dx] = COL_SCOPE_GRID;
        }
        draw_env_vline_dotted(fb, gx0 + x_gate_off, gy_top, gy_bot);

        for (uint32_t dx = 0; dx <= gw; dx++) {
            uint32_t py = y_pts[dx];
            if (dx > 0u) {
                uint32_t prev = y_pts[dx - 1u];
                uint32_t y0   = prev < py ? prev : py;
                uint32_t y1   = prev < py ? py : prev;
                fb_fill_rect(fb, gx0 + dx, y0, 1u, y1 - y0 + 1u, th->accent);
            } else {
                fb[py * ADRIFT_SCREEN_W + gx0] = th->accent;
            }
        }
        draw_env_node(fb, gx0 + x_gate_off, y_pts[x_gate_off], th->accent);
        draw_env_node(fb, gx0 + gw,         y_pts[gw],         th->accent);
        return;
    }

    /* Standard DADSR / ADSR geometry */
    const uint32_t w_sus = 36u;
    const uint32_t avail = gw - w_sus;
    uint32_t w_dly = (dly_k > 0u) ? (6u + (dly_k * 64u) / 1023u) : 0u;
    uint32_t w_atk = 3u + (atk_k * 84u) / 1023u;
    uint32_t w_dec = 8u + (dec_k * 88u) / 1023u;
    uint32_t w_rel = 8u + (rel_k * 88u) / 1023u;
    if (is_env1 && ep->range == ADRIFT_ENV_RANGE_SHORT) {
        w_atk = 2u + (atk_k * 36u) / 1023u;
        w_dec = 4u + (dec_k * 40u) / 1023u;
        w_rel = 4u + (rel_k * 40u) / 1023u;
    } else if (is_env1 && ep->range == ADRIFT_ENV_RANGE_LONG) {
        w_atk = 5u + (atk_k * 108u) / 1023u;
        w_dec = 12u + (dec_k * 112u) / 1023u;
        w_rel = 12u + (rel_k * 112u) / 1023u;
    }

    uint32_t raw_sum = w_dly + w_atk + w_dec + w_rel;
    if (raw_sum > avail) {
        if (w_dly > 0u) {
            w_dly = (w_dly * avail) / raw_sum;
            if (w_dly < 2u) w_dly = 2u;
        }
        w_atk = (w_atk * avail) / raw_sum;
        if (w_atk < 2u) w_atk = 2u;
        w_dec = (w_dec * avail) / raw_sum;
        if (w_dec < 3u) w_dec = 3u;
        w_rel = avail - (w_dly + w_atk + w_dec);
        if (w_rel < 3u) w_rel = 3u;
    }

    uint32_t x_dly = w_dly;
    uint32_t x_atk = x_dly + w_atk;
    uint32_t x_dec = x_atk + w_dec;
    uint32_t x_sus = x_dec + w_sus;
    uint32_t x_rel = x_sus + w_rel;
    if (x_rel > gw) x_rel = gw;

    for (uint32_t dx = 0; dx <= gw; dx++) {
        uint32_t lvl_q15 = 0u;
        if (dx < x_dly) {
            lvl_q15 = 0u;
        } else if (dx <= x_atk) {
            lvl_q15 = interp_lut16_q15(k_rc_atk_q15, dx - x_dly, w_atk);
        } else if (dx <= x_dec) {
            lvl_q15 = sus_q15 + (((32768u - sus_q15) * interp_lut16_q15(k_rc_dec_q15, dx - x_atk, w_dec)) >> 15);
        } else if (dx <= x_sus) {
            lvl_q15 = sus_q15;
        } else if (dx <= x_rel) {
            lvl_q15 = (sus_q15 * interp_lut16_q15(k_rc_dec_q15, dx - x_sus, w_rel)) >> 15;
        }
        y_pts[dx] = (uint8_t)(gy_bot - ((lvl_q15 * gh + 16384u) >> 15));
    }

    if (!is_env1 && ep->drone_hold) {
        fb_fill_rect(fb, gx0, gy_top + 1u, gw + 1u, gh, fill_col);
        for (uint32_t dx = 0; dx <= gw; dx += 4u) {
            fb[gy_bot * ADRIFT_SCREEN_W + gx0 + dx] = COL_SCOPE_GRID;
        }
        for (uint32_t dx = 0; dx <= gw; dx++) {
            uint32_t py = y_pts[dx];
            if (dx > 0u) {
                uint32_t prev = y_pts[dx - 1u];
                uint32_t y0   = prev < py ? prev : py;
                uint32_t y1   = prev < py ? py : prev;
                fb_fill_rect(fb, gx0 + dx, y0, 1u, y1 - y0 + 1u, th->dim);
            } else {
                fb[py * ADRIFT_SCREEN_W + gx0] = th->dim;
            }
        }
        fb_fill_rect(fb, gx0, gy_top, gw + 1u, 2u, th->accent);
        return;
    }

    for (uint32_t dx = 0; dx <= gw; dx++) {
        uint32_t py = y_pts[dx];
        if (py + 1u <= gy_bot) {
            fb_fill_rect(fb, gx0 + dx, py + 1u, 1u, gy_bot - py, fill_col);
        }
    }

    for (uint32_t dx = 0; dx <= gw; dx += 4u) {
        fb[gy_top * ADRIFT_SCREEN_W + gx0 + dx] = COL_SCOPE_GRID;
        fb[gy_bot * ADRIFT_SCREEN_W + gx0 + dx] = COL_SCOPE_GRID;
    }
    if (w_dly > 0u) {
        draw_env_vline_dotted(fb, gx0 + x_dly, gy_top, gy_bot);
    }
    draw_env_vline_dotted(fb, gx0 + x_atk, gy_top, gy_bot);
    draw_env_vline_dotted(fb, gx0 + x_dec, gy_top, gy_bot);
    draw_env_vline_dotted(fb, gx0 + x_sus, gy_top, gy_bot);

    for (uint32_t dx = 0; dx <= gw; dx++) {
        uint32_t py = y_pts[dx];
        if (dx > 0u) {
            uint32_t prev = y_pts[dx - 1u];
            uint32_t y0   = prev < py ? prev : py;
            uint32_t y1   = prev < py ? py : prev;
            fb_fill_rect(fb, gx0 + dx, y0, 1u, y1 - y0 + 1u, th->accent);
        } else {
            fb[py * ADRIFT_SCREEN_W + gx0] = th->accent;
        }
    }

    if (w_dly > 0u) {
        draw_env_node(fb, gx0 + x_dly, y_pts[x_dly], th->accent);
    }
    draw_env_node(fb, gx0 + x_atk, y_pts[x_atk], th->accent);
    draw_env_node(fb, gx0 + x_dec, y_pts[x_dec], th->accent);
    draw_env_node(fb, gx0 + x_sus, y_pts[x_sus], th->accent);
    draw_env_node(fb, gx0 + x_rel, y_pts[x_rel], th->accent);
}

/* Top Visualizer Pane for EXC Screen (Micro-ACB Waveform + S&H Noise + Tuning overview) */
void draw_exc_pane(uint16_t *fb, const adrift_synth_t *s, uint8_t subpage) {
    const adrift_group_t       *gp = adrift_synth_active_group(s);
    const adrift_patch_t       *p  = &gp->patch;
    const adrift_group_theme_t *th = get_group_theme(s->edit_group);
    const uint32_t sx = 5u, sy = 30u, sw = 230u, sh = 105u;

    fb_fill_rect(fb, sx, sy, sw, sh, COL_SCOPE_BG);
    fb_rect_outline(fb, sx, sy, sw, sh, th->border);

    draw_screen_page_badge(fb, sx, sy, "EXC", subpage, th);

    const char *wname = (p->exc.wave_morph < 120) ? "SINE"
                      : (p->exc.wave_morph < 260) ? "SIN-TRI"
                      : (p->exc.wave_morph < 420) ? "TRI"
                      : (p->exc.wave_morph < 580) ? "TRI-SAW"
                      : (p->exc.wave_morph < 760) ? "SAW"
                      : (p->exc.wave_morph < 920) ? "SAW-PLS" : "PULSE";
    fb_draw_str_spec(fb, sx + 68u, sy + 4u, wname, COL_WHITE, &k_font_md);
    if (!p->exc.vco_keytrack) {
        fb_draw_str_spec(fb, sx + 132u, sy + 5u, "FIXED", th->accent, &k_font_sm_bold);
    }
    draw_voice_pips(fb, s, sx, sy + 1u, sw);

    /* Plot 2 cycles of the continuous Micro-ACB VCO + S&H Noise overlay on left (142 px wide) */
    const uint32_t wx0   = sx + 8u;
    const uint32_t ww    = 138u;
    const int32_t  mid_y = (int32_t)(sy + 58u);
    const int32_t  amp_y = 32;
    uint16_t fill_col    = make_env_fill_col(th->accent);

    for (uint32_t dx = 0; dx < ww; dx += 4u) {
        fb[(uint32_t)(mid_y - amp_y) * ADRIFT_SCREEN_W + (wx0 + dx)] = COL_SCOPE_GRID;
        fb[(uint32_t)mid_y           * ADRIFT_SCREEN_W + (wx0 + dx)] = COL_SCOPE_GRID;
        fb[(uint32_t)(mid_y + amp_y) * ADRIFT_SCREEN_W + (wx0 + dx)] = COL_SCOPE_GRID;
    }

    uint32_t prev_py = (uint32_t)mid_y;
    int32_t  plk_k   = dsp_clamp_i32(p->exc.pluck, 0, 1023);
    for (uint32_t dx = 0; dx < ww; dx++) {
        uint32_t p16   = ((dx * 2u * 65535u) / (ww - 1u)) & 0xFFFFu;
        int32_t  y_q15 = eval_morph_wave_q15(p16, p);
        if (plk_k > 0) {
            uint32_t sub_p32 = ((dx * 65535u) / (ww - 1u)) << 16;
            int32_t  sub_q15 = (dsp_sine_lut_q15(sub_p32) * plk_k) >> 11;
            int32_t  imp_q15 = (dx < 34u)
                                   ? ((dsp_sine_lut_q15((dx * 65535u / 34u) << 16) * plk_k) >> 10)
                                   : 0;
            y_q15 = dsp_sat_q15(y_q15 + sub_q15 + imp_q15);
        }
        int32_t  dy    = (y_q15 * amp_y) >> 15;
        uint32_t py    = (uint32_t)dsp_clamp_i32(mid_y - dy, mid_y - amp_y, mid_y + amp_y);
        uint32_t x     = wx0 + dx;

        uint32_t fy0 = (py < (uint32_t)mid_y) ? py : (uint32_t)mid_y;
        uint32_t fy1 = (py < (uint32_t)mid_y) ? (uint32_t)mid_y : py;
        for (uint32_t fy = fy0; fy <= fy1; fy++) {
            fb[fy * ADRIFT_SCREEN_W + x] = fill_col;
        }
        if (dx > 0u) {
            uint32_t y0 = (prev_py < py) ? prev_py : py;
            uint32_t y1 = (prev_py < py) ? py : prev_py;
            for (uint32_t y = y0; y <= y1; y++) {
                fb[y * ADRIFT_SCREEN_W + x] = th->accent;
            }
        } else {
            fb[py * ADRIFT_SCREEN_W + x] = th->accent;
        }
        prev_py = py;
    }

    /* Right-side compact summary panel so both pages' key values are always visible */
    uint32_t rx = sx + 152u;
    fb_fill_rect(fb, rx - 4u, sy + 20u, 1u, 78u, COL_SCOPE_GRID);
    char buf[16];

    fb_draw_str_spec(fb, rx, sy + 22u, "VCO", COL_MUTED, &k_font_sm_bold);
    fmt_int(buf, (p->exc.vco_level * 100) / 1023, "%");
    fb_draw_str_spec(fb, rx + 34u, sy + 22u, buf, COL_WHITE, &k_font_sm_bold);

    fb_draw_str_spec(fb, rx, sy + 36u, "MIX", COL_MUTED, &k_font_sm_bold);
    fmt_int(buf, (p->exc.noise_level * 100) / 1023, "%");
    fb_draw_str_spec(fb, rx + 34u, sy + 36u, buf, COL_WHITE, &k_font_sm_bold);

    fb_draw_str_spec(fb, rx, sy + 50u, "S&H", COL_MUTED, &k_font_sm_bold);
    if (p->exc.sh_rate >= 1000) copy_str(buf, "WHT");
    else fmt_int(buf, (p->exc.sh_rate * 100) / 1000, "%");
    fb_draw_str_spec(fb, rx + 34u, sy + 50u, buf, COL_WHITE, &k_font_sm_bold);

    fb_draw_str_spec(fb, rx, sy + 64u, "SEMI", COL_MUTED, &k_font_sm_bold);
    fmt_int(buf, p->exc.vco_semi, "ST");
    fb_draw_str_spec(fb, rx + 34u, sy + 64u, buf, COL_WHITE, &k_font_sm_bold);

    if (subpage == 2u) {
        fb_draw_str_spec(fb, rx, sy + 78u, "DRFT", COL_MUTED, &k_font_sm_bold);
        fmt_int(buf, (p->exc.analog_drift * 100) / 1023, "%");
        fb_draw_str_spec(fb, rx + 34u, sy + 78u, buf, COL_WHITE, &k_font_sm_bold);
    } else if (p->exc.pluck > 0 || subpage == 1u) {
        fb_draw_str_spec(fb, rx, sy + 78u, "PLK", COL_MUTED, &k_font_sm_bold);
        fmt_int(buf, (p->exc.pluck * 100) / 1023, "%");
        fb_draw_str_spec(fb, rx + 34u, sy + 78u, buf, p->exc.pluck > 0 ? th->accent : COL_WHITE, &k_font_sm_bold);
    } else {
        fb_draw_str_spec(fb, rx, sy + 78u, "FINE", COL_MUTED, &k_font_sm_bold);
        fmt_int(buf, p->exc.vco_fine, "C");
        fb_draw_str_spec(fb, rx + 34u, sy + 78u, buf, COL_WHITE, &k_font_sm_bold);
    }
}

/* Top Visualizer Pane for BBD Resonator Screen (Input LPF + 1024-stage BBD Comb + FB LPF) */
void draw_bbd_pane(uint16_t *fb, const adrift_synth_t *s, uint8_t subpage) {
    const adrift_group_t       *gp = adrift_synth_active_group(s);
    const adrift_patch_t       *p  = &gp->patch;
    const adrift_group_theme_t *th = get_group_theme(s->edit_group);
    const uint32_t sx = 5u, sy = 30u, sw = 230u, sh = 105u;

    fb_fill_rect(fb, sx, sy, sw, sh, COL_SCOPE_BG);
    fb_rect_outline(fb, sx, sy, sw, sh, th->border);

    draw_screen_page_badge(fb, sx, sy, "BBD", subpage, th);

    if (p->bbd.feedback > 890) {
        fb_draw_str_spec(fb, sx + 68u, sy + 4u, "SELF-OSC", th->accent, &k_font_md);
    } else if (p->bbd.invert) {
        fb_draw_str_spec(fb, sx + 68u, sy + 4u, p->bbd.invert_pitch_comp ? "INV+0O" : "INV-1O", COL_WHITE, &k_font_md);
    } else if (p->bbd.dispersion > 0) {
        fb_draw_str_spec(fb, sx + 68u, sy + 4u, "DISPERSV", th->accent, &k_font_md);
    } else {
        fb_draw_str_spec(fb, sx + 68u, sy + 4u, "1024-STG", COL_MUTED, &k_font_md);
    }
    draw_voice_pips(fb, s, sx, sy + 1u, sw);

    /* Plot Input LPF + BBD Comb Harmonic Peaks across wx0..wx0+138 */
    const uint32_t gx0    = sx + 8u;
    const uint32_t gw     = 138u;
    const uint32_t gy_top = sy + 20u;
    const uint32_t gy_bot = sy + sh - 8u;
    const uint32_t gh     = gy_bot - gy_top;
    uint16_t fill_col     = make_env_fill_col(th->accent);

    uint32_t cut_x = 12u + ((uint32_t)dsp_clamp_i32(p->in_lpf_cutoff, 0, 1023) * (gw - 20u)) / 1023u;
    uint32_t fb_x  = 12u + ((uint32_t)dsp_clamp_i32(p->bbd.fb_lpf_cutoff, 0, 1023) * (gw - 20u)) / 1023u;
    uint32_t res_h = ((uint32_t)dsp_clamp_i32(p->in_lpf_res, 0, 1023) * 22u) / 1023u;
    uint32_t fb_h  = ((uint32_t)dsp_clamp_i32(p->bbd.feedback, 0, 1023) * 38u) / 1023u;
    uint32_t disp_k = (uint32_t)dsp_clamp_i32(p->bbd.dispersion, 0, 1023);

    uint32_t prev_py = gy_bot;
    for (uint32_t dx = 0; dx < gw; dx++) {
        /* Base Input 12dB LPF envelope */
        int32_t h_px = (int32_t)(gh * 52u / 100u);
        if (dx > cut_x) {
            uint32_t over = dx - cut_x;
            h_px -= (int32_t)(over * 2u);
        } else {
            uint32_t dist = cut_x - dx;
            if (dist < 14u) {
                h_px += (int32_t)((res_h * (14u - dist)) / 14u);
            }
        }
        /* Superimpose BBD resonator harmonic comb spikes (shifted on Invert, stretched on Dispersion) */
        uint32_t comb_period = p->bbd.invert ? 22u : 18u;
        uint32_t stretch     = (disp_k * dx * dx) >> 18;
        uint32_t eff_dx      = (dx > stretch) ? (dx - stretch) : 0u;
        uint32_t comb_phase  = (eff_dx + (p->bbd.invert ? 11u : 4u)) % comb_period;
        if (comb_phase <= 4u && dx <= fb_x + 24u) {
            uint32_t spike = fb_h * (comb_phase == 2u ? 4u : (comb_phase == 1u || comb_phase == 3u ? 2u : 1u)) / 4u;
            if (dx > fb_x) {
                spike = (spike * 12u) / (12u + (dx - fb_x));
            }
            h_px += (int32_t)spike;
        }
        h_px = dsp_clamp_i32(h_px, 2, (int32_t)gh - 1);
        uint32_t py = gy_bot - (uint32_t)h_px;
        uint32_t x  = gx0 + dx;

        if (py + 1u <= gy_bot) {
            fb_fill_rect(fb, x, py + 1u, 1u, gy_bot - py, fill_col);
        }
        if (dx > 0u) {
            uint32_t y0 = (prev_py < py) ? prev_py : py;
            uint32_t y1 = (prev_py < py) ? py : prev_py;
            fb_fill_rect(fb, x, y0, 1u, y1 - y0 + 1u, th->accent);
        } else {
            fb[py * ADRIFT_SCREEN_W + x] = th->accent;
        }
        prev_py = py;
    }

    /* Right-side compact summary panel */
    uint32_t rx = sx + 152u;
    fb_fill_rect(fb, rx - 4u, sy + 20u, 1u, 78u, COL_SCOPE_GRID);
    char buf[16];

    fb_draw_str_spec(fb, rx, sy + 22u, "LPF", COL_MUTED, &k_font_sm_bold);
    fmt_hz(buf, dsp_cutoff_to_fc_q8(p->in_lpf_cutoff) >> 8);
    fb_draw_str_spec(fb, rx + 30u, sy + 22u, buf, COL_WHITE, &k_font_sm_bold);

    fb_draw_str_spec(fb, rx, sy + 36u, "FBLP", COL_MUTED, &k_font_sm_bold);
    fmt_hz(buf, dsp_cutoff_to_fc_q8(p->bbd.fb_lpf_cutoff) >> 8);
    fb_draw_str_spec(fb, rx + 30u, sy + 36u, buf, COL_WHITE, &k_font_sm_bold);

    fb_draw_str_spec(fb, rx, sy + 50u, "INV", COL_MUTED, &k_font_sm_bold);
    copy_str(buf, !p->bbd.invert ? "OFF" : (p->bbd.invert_pitch_comp ? "+0O" : "-1O"));
    fb_draw_str_spec(fb, rx + 30u, sy + 50u, buf, p->bbd.invert ? th->accent : COL_WHITE, &k_font_sm_bold);

    if (p->bbd.dispersion > 0 || subpage == 1u) {
        fb_draw_str_spec(fb, rx, sy + 64u, "DISP", COL_MUTED, &k_font_sm_bold);
        fmt_int(buf, (p->bbd.dispersion * 100) / 1023, "%");
        fb_draw_str_spec(fb, rx + 34u, sy + 64u, buf, p->bbd.dispersion > 0 ? th->accent : COL_WHITE, &k_font_sm_bold);
    } else {
        fb_draw_str_spec(fb, rx, sy + 64u, "GLID", COL_MUTED, &k_font_sm_bold);
        fmt_int(buf, (p->bbd.glide * 100) / 1023, "%");
        fb_draw_str_spec(fb, rx + 34u, sy + 64u, buf, COL_WHITE, &k_font_sm_bold);
    }

    fb_draw_str_spec(fb, rx, sy + 78u, "MIX", COL_MUTED, &k_font_sm_bold);
    fmt_int(buf, (p->dry_wet * 100) / 1023, "%");
    fb_draw_str_spec(fb, rx + 34u, sy + 78u, buf, COL_WHITE, &k_font_sm_bold);
}

/* Top Visualizer Pane for MOD Screen (Multi-LFO 1..3 Wave Overlay + Bipolar Depth + Mod Matrix Summary) */
static void draw_lfo_trace_curve(uint16_t *fb,
                                 uint32_t wx0,
                                 uint32_t ww,
                                 int32_t mid_y,
                                 int32_t max_a,
                                 uint8_t wave,
                                 int16_t rate,
                                 int16_t depth,
                                 uint16_t line_col,
                                 uint16_t fill_col,
                                 bool draw_fill) {
    int32_t  dep_amp = (depth * max_a) / 512;
    uint32_t cycles  = 1u + ((uint32_t)dsp_clamp_i32(rate, 0, 1023) * 3u) / 1023u;
    uint32_t prev_py = (uint32_t)mid_y;

    for (uint32_t dx = 0; dx < ww; dx++) {
        uint32_t p16 = ((dx * cycles * 65535u) / (ww - 1u)) & 0xFFFFu;
        int32_t  w_q15 = 0;
        switch (wave % 5u) {
            case ADRIFT_LFO_SINE:
                w_q15 = dsp_sine_lut_q15(p16 << 16);
                break;
            case ADRIFT_LFO_TRI: {
                uint32_t t = (p16 + 16384u) & 0xFFFFu;
                w_q15 = (t < 32768u) ? ((int32_t)(t << 1) - 32768) : (32767 - (int32_t)((t - 32768u) << 1));
                break;
            }
            case ADRIFT_LFO_SAW:
                w_q15 = (int32_t)p16 - 32768;
                break;
            case ADRIFT_LFO_SQR:
                w_q15 = (p16 < 32768u) ? 32767 : -32767;
                break;
            default: {
                uint32_t step_idx = (dx * cycles * 4u) / ww;
                int32_t  sh_lut[8] = { 24000, -18000, 30000, -28000, 11000, -22000, 27000, -12000 };
                w_q15 = sh_lut[step_idx & 7u];
                break;
            }
        }
        int32_t  dy = (w_q15 * dep_amp) >> 15;
        uint32_t py = (uint32_t)dsp_clamp_i32(mid_y - dy, mid_y - max_a, mid_y + max_a);
        uint32_t x  = wx0 + dx;

        if (draw_fill) {
            uint32_t fy0 = (py < (uint32_t)mid_y) ? py : (uint32_t)mid_y;
            uint32_t fy1 = (py < (uint32_t)mid_y) ? (uint32_t)mid_y : py;
            for (uint32_t fy = fy0; fy <= fy1; fy++) {
                fb[fy * ADRIFT_SCREEN_W + x] = fill_col;
            }
        }
        if (dx > 0u) {
            uint32_t y0 = (prev_py < py) ? prev_py : py;
            uint32_t y1 = (prev_py < py) ? py : prev_py;
            fb_fill_rect(fb, x, y0, 1u, y1 - y0 + 1u, line_col);
        } else {
            fb[py * ADRIFT_SCREEN_W + x] = line_col;
        }
        prev_py = py;
    }
}

void draw_mod_pane(uint16_t *fb, const adrift_synth_t *s, uint8_t subpage) {
    const adrift_group_t       *gp = adrift_synth_active_group(s);
    const adrift_patch_t       *p  = &gp->patch;
    const adrift_group_theme_t *th = get_group_theme(s->edit_group);
    const uint32_t sx = 5u, sy = 30u, sw = 230u, sh = 105u;

    uint8_t l1_dst    = (p->lfo_dest < ADRIFT_LFO1_DEST_COUNT) ? p->lfo_dest : (uint8_t)(ADRIFT_LFO1_DEST_COUNT - 1u);
    uint8_t waves[3]  = { p->lfo_wave,  p->lfo2_wave,  p->lfo3_wave  };
    int16_t rates[3]  = { p->lfo_rate,  p->lfo2_rate,  p->lfo3_rate  };
    int16_t depths[3] = { p->lfo_depth, p->lfo2_depth, p->lfo3_depth };
    uint8_t dests[3]  = { l1_dst,       p->lfo2_dest,  p->lfo3_dest  };
    uint8_t focus_idx = ((subpage & 3u) < 3u) ? (subpage & 3u) : 0u;

    fb_fill_rect(fb, sx, sy, sw, sh, COL_SCOPE_BG);
    fb_rect_outline(fb, sx, sy, sw, sh, th->border);

    draw_screen_page_badge(fb, sx, sy, "MOD", subpage, th);
    fb_draw_str_spec(fb, sx + 64u, sy + 4u,
                     k_mod_dest_names[dests[focus_idx] % (uint8_t)ADRIFT_MOD_DEST_COUNT],
                     COL_WHITE, &k_font_md);
    draw_voice_pips(fb, s, sx, sy + 1u, sw);

    const uint32_t wx0   = sx + 8u;
    const uint32_t ww    = 138u;
    const int32_t  mid_y = (int32_t)(sy + 58u);
    const int32_t  max_a = 32;
    uint16_t fill_col    = make_env_fill_col(th->accent);

    for (uint32_t dx = 0; dx < ww; dx += 4u) {
        fb[(uint32_t)(mid_y - max_a) * ADRIFT_SCREEN_W + (wx0 + dx)] = COL_SCOPE_GRID;
        fb[(uint32_t)mid_y           * ADRIFT_SCREEN_W + (wx0 + dx)] = COL_SCOPE_GRID;
        fb[(uint32_t)(mid_y + max_a) * ADRIFT_SCREEN_W + (wx0 + dx)] = COL_SCOPE_GRID;
    }

    /* Draw non-focused active LFOs behind in dimmed group accent, then focused LFO on top */
    for (uint8_t li = 0; li < 3u; li++) {
        if (li != focus_idx && depths[li] != 0) {
            draw_lfo_trace_curve(fb, wx0, ww, mid_y, max_a,
                                 waves[li], rates[li], depths[li],
                                 th->dim, fill_col, false);
        }
    }
    draw_lfo_trace_curve(fb, wx0, ww, mid_y, max_a,
                         waves[focus_idx], rates[focus_idx], depths[focus_idx],
                         th->accent, fill_col, true);

    /* Right-side compact Modulation Matrix summary panel (LFO 1..3 + MW) */
    uint32_t rx = sx + 152u;
    fb_fill_rect(fb, rx - 4u, sy + 20u, 1u, 78u, COL_SCOPE_GRID);
    char buf[16];

    for (uint8_t li = 0; li < 3u; li++) {
        uint32_t ry = sy + 24u + (uint32_t)li * 18u;
        char ltag[4] = { 'L', 'F', (char)('1' + li), '\0' };
        bool is_sel  = ((subpage & 3u) == li);
        bool is_act  = (depths[li] != 0);
        fb_draw_str_spec(fb, rx, ry, ltag, is_sel ? th->accent : COL_MUTED, &k_font_sm_bold);
        fmt_int(buf, (depths[li] * 100) / 512, "%");
        fb_draw_str_spec(fb, rx + 34u, ry, buf, is_act ? th->accent : COL_WHITE, &k_font_sm_bold);
    }

    fb_draw_str_spec(fb, rx, sy + 78u, "MW", ((subpage & 3u) == 3u) ? th->accent : COL_MUTED, &k_font_sm_bold);
    fmt_int(buf, (p->mw_depth * 100) / 1023, "%");
    fb_draw_str_spec(fb, rx + 34u, sy + 78u, buf, COL_WHITE, &k_font_sm_bold);
}

/* Top Visualizer Pane for ARP Screen (8-step Arpeggio Pattern + Swing/Gate/Ratchet + Summary) */
void draw_arp_pane(uint16_t *fb, const adrift_synth_t *s, uint8_t subpage) {
    const adrift_group_t       *gp = adrift_synth_active_group(s);
    const adrift_arp_params_t  *ap = &gp->patch.arp;
    const adrift_arp_state_t   *st = &gp->arp_state;
    const adrift_group_theme_t *th = get_group_theme(s->edit_group);
    const uint32_t sx = 5u, sy = 30u, sw = 230u, sh = 105u;

    fb_fill_rect(fb, sx, sy, sw, sh, COL_SCOPE_BG);
    fb_rect_outline(fb, sx, sy, sw, sh, th->border);

    draw_screen_page_badge(fb, sx, sy, "ARP", subpage, th);

    uint8_t m = (uint8_t)ap->mode % (uint8_t)ADRIFT_ARP_MODE_COUNT;
    fb_draw_str_spec(fb, sx + 64u, sy + 4u, k_arp_mode_names[m],
                     (m != 0u) ? th->accent : COL_MUTED, &k_font_md);
    if (s->midi_clk_active) {
        fb_draw_str_spec(fb, sx + 124u, sy + 5u, ap->latch ? "EXT+L" : "EXT", th->accent, &k_font_sm_bold);
    } else if (ap->latch) {
        fb_draw_str_spec(fb, sx + 124u, sy + 5u, "LTCH", COL_WHITE, &k_font_sm_bold);
    }
    draw_voice_pips(fb, s, sx, sy + 1u, sw);

    /* Plot 8-step arpeggio sequence visualizer on left (138 px wide) */
    const uint32_t wx0   = sx + 8u;
    const uint32_t ww    = 138u;
    const uint32_t wy0   = sy + 20u;
    const uint32_t wy1   = sy + sh - 8u;
    const uint32_t wh    = wy1 - wy0;
    uint16_t fill_col    = make_env_fill_col(th->accent);

    /* Horizontal octave grid lines */
    uint32_t rng = (uint32_t)dsp_clamp_i32(ap->range, 1, 4);
    for (uint32_t r = 0; r <= rng; r++) {
        uint32_t gy = wy1 - (r * wh) / rng;
        for (uint32_t dx = 0; dx < ww; dx += 4u) {
            fb[gy * ADRIFT_SCREEN_W + (wx0 + dx)] = COL_SCOPE_GRID;
        }
    }

    /* Normalized 0..7 pitch contour for each algorithm across 8 preview steps */
    static const uint8_t k_algo_pat[ADRIFT_ARP_MODE_COUNT][8] = {
        { 3, 3, 3, 3, 3, 3, 3, 3 }, /* OFF */
        { 0, 1, 2, 3, 4, 5, 6, 7 }, /* UP */
        { 7, 6, 5, 4, 3, 2, 1, 0 }, /* DOWN */
        { 0, 2, 4, 6, 7, 5, 3, 1 }, /* UP/DN (excl) */
        { 7, 5, 3, 1, 0, 2, 4, 6 }, /* DN/UP (excl) */
        { 0, 2, 5, 7, 7, 5, 2, 0 }, /* UP&DN (incl) */
        { 0, 7, 1, 6, 2, 5, 3, 4 }, /* CONVRG */
        { 3, 4, 2, 5, 1, 6, 0, 7 }, /* DIVERG */
        { 2, 6, 1, 7, 4, 0, 5, 3 }, /* RANDOM */
        { 0, 4, 2, 6, 1, 5, 3, 7 }, /* ORDER */
        { 5, 5, 5, 5, 5, 5, 5, 5 }  /* CHORD */
    };

    uint32_t active_step = (st->num_notes > 0u && st->step_counter > 0u)
                               ? ((st->step_counter - 1u) & 7u)
                               : 0xFFu;
    uint32_t swing_px = ((uint32_t)dsp_clamp_i32(ap->swing, 0, 100) * 5u) / 100u;
    uint32_t gate_pct = (uint32_t)dsp_clamp_i32(ap->gate, 5, 100);

    for (uint32_t step = 0; step < 8u; step++) {
        uint32_t base_x = wx0 + step * 17u;
        uint32_t sx0    = base_x + (((step & 1u) != 0u) ? swing_px : 0u);
        uint32_t max_w  = 15u;
        if (sx0 + max_w > wx0 + ww) {
            max_w = (wx0 + ww > sx0) ? (wx0 + ww - sx0) : 2u;
        }
        uint32_t bar_w = (max_w * gate_pct) / 100u;
        if (bar_w < 2u) bar_w = 2u;

        uint32_t p_idx = k_algo_pat[m][step];
        uint32_t bar_h = 10u + (p_idx * (wh - 12u)) / 7u;
        if (ap->mode == ADRIFT_ARP_OFF) {
            bar_h = 8u;
        }
        uint32_t by0 = wy1 - bar_h;

        /* Determine ratchet sub-slices for this step */
        uint32_t rdiv = 1u;
        switch (ap->ratchet) {
            case ADRIFT_RATCHET_2X:    rdiv = 2u; break;
            case ADRIFT_RATCHET_3X:    rdiv = 3u; break;
            case ADRIFT_RATCHET_4X:    rdiv = 4u; break;
            case ADRIFT_RATCHET_ALT_2: rdiv = ((step & 1u) != 0u) ? 2u : 1u; break;
            case ADRIFT_RATCHET_ALT_3: rdiv = ((step % 3u) == 2u) ? 3u : (((step % 3u) == 1u) ? 2u : 1u); break;
            case ADRIFT_RATCHET_ALT_4: rdiv = ((step & 3u) == 3u) ? 4u : (((step & 1u) != 0u) ? 2u : 1u); break;
            case ADRIFT_RATCHET_RANDOM: rdiv = ((step * 3u + 1u) & 3u) + 1u; break;
            default: rdiv = 1u; break;
        }

        bool is_cur = (step == active_step && ap->mode != ADRIFT_ARP_OFF);
        uint16_t top_col  = (ap->mode == ADRIFT_ARP_OFF) ? COL_MUTED : (is_cur ? COL_WHITE : th->accent);
        uint16_t body_col = (ap->mode == ADRIFT_ARP_OFF) ? COL_BAR_TRACK : (is_cur ? th->accent : fill_col);

        for (uint32_t r = 0; r < rdiv; r++) {
            uint32_t rx0 = sx0 + (r * bar_w) / rdiv;
            uint32_t rx1 = sx0 + ((r + 1u) * bar_w) / rdiv;
            if (r + 1u < rdiv && rx1 > rx0 + 1u) {
                rx1--; /* 1px gap between ratchet retriggers */
            }
            uint32_t rw = (rx1 > rx0) ? (rx1 - rx0) : 1u;
            fb_fill_rect(fb, rx0, by0, rw, bar_h, body_col);
            fb_fill_rect(fb, rx0, by0, rw, 2u, top_col);
            if (ap->mode == ADRIFT_ARP_CHORD && bar_h >= 18u) {
                /* Draw stacked chord bars inside each step */
                fb_fill_rect(fb, rx0, by0 + bar_h / 3u, rw, 2u, top_col);
                fb_fill_rect(fb, rx0, by0 + (bar_h * 2u) / 3u, rw, 2u, top_col);
            }
        }
    }

    /* Right-side compact summary panel so both pages' key values are always visible */
    uint32_t rx = sx + 152u;
    fb_fill_rect(fb, rx - 4u, sy + 20u, 1u, 78u, COL_SCOPE_GRID);
    char buf[16];

    fb_draw_str_spec(fb, rx, sy + 22u, s->midi_clk_active ? "EXT" : "BPM",
                     s->midi_clk_active ? th->accent : COL_MUTED, &k_font_sm_bold);
    int32_t disp_bpm = s->midi_clk_active
                           ? dsp_clamp_i32((int32_t)s->midi_clk_bpm, 30, 300)
                           : dsp_clamp_i32(ap->rate_bpm, 40, 240);
    fmt_int(buf, disp_bpm, "");
    fb_draw_str_spec(fb, rx + 34u, sy + 22u, buf,
                     s->midi_clk_active ? th->accent : COL_WHITE, &k_font_sm_bold);

    fb_draw_str_spec(fb, rx, sy + 36u, "RNG", COL_MUTED, &k_font_sm_bold);
    fmt_int(buf, dsp_clamp_i32(ap->range, 1, 4), "OCT");
    fb_draw_str_spec(fb, rx + 34u, sy + 36u, buf, COL_WHITE, &k_font_sm_bold);

    fb_draw_str_spec(fb, rx, sy + 50u, "GATE", COL_MUTED, &k_font_sm_bold);
    fmt_int(buf, dsp_clamp_i32(ap->gate, 5, 100), "%");
    fb_draw_str_spec(fb, rx + 34u, sy + 50u, buf, COL_WHITE, &k_font_sm_bold);

    fb_draw_str_spec(fb, rx, sy + 64u, "SWNG", COL_MUTED, &k_font_sm_bold);
    fmt_int(buf, dsp_clamp_i32(ap->swing, 0, 100), "%");
    fb_draw_str_spec(fb, rx + 34u, sy + 64u, buf, ap->swing > 0u ? th->accent : COL_WHITE, &k_font_sm_bold);

    fb_draw_str_spec(fb, rx, sy + 78u, "RTCH", COL_MUTED, &k_font_sm_bold);
    copy_str(buf, k_arp_ratchet_names[(uint8_t)ap->ratchet % (uint8_t)ADRIFT_RATCHET_COUNT]);
    fb_draw_str_spec(fb, rx + 34u, sy + 78u, buf,
                     ap->ratchet != ADRIFT_RATCHET_1X ? th->accent : COL_WHITE, &k_font_sm_bold);
}

/* Top Visualizer Pane for FX Screen (Chorus/Delay/Shimmer Reverb + Page 3/3 Bus Compressor Knee & GR) */
void draw_fx_pane(uint16_t *fb, const adrift_synth_t *s, uint8_t subpage) {
    const adrift_fx_params_t   *fp = &s->fx_params;
    const adrift_group_theme_t *th = get_group_theme(s->edit_group);
    const uint32_t sx = 5u, sy = 30u, sw = 230u, sh = 105u;
    uint8_t sp = subpage % 3u;

    fb_fill_rect(fb, sx, sy, sw, sh, COL_SCOPE_BG);
    fb_rect_outline(fb, sx, sy, sw, sh, th->border);

    draw_screen_page_badge(fb, sx, sy, "FX", sp, th);

    bool fx_on = adrift_fx_is_active(fp);
    if (sp == 2u) {
        fb_draw_str_spec(fb, sx + 64u, sy + 4u, "BUS COMP",
                         fp->comp_mix > 0 ? th->accent : COL_MUTED, &k_font_md);
    } else if (fp->reverb_mix > 0 && fp->reverb_shimmer > 0) {
        fb_draw_str_spec(fb, sx + 64u, sy + 4u, "SHIMMER", COL_WHITE, &k_font_md);
    } else if (fp->comp_mix > 0 && fp->chorus_mix == 0 && fp->delay_mix == 0 && fp->reverb_mix == 0) {
        fb_draw_str_spec(fb, sx + 64u, sy + 4u, "BUS GLUE", th->accent, &k_font_md);
    } else if (fx_on) {
        fb_draw_str_spec(fb, sx + 64u, sy + 4u, "STEREO", th->accent, &k_font_md);
    } else {
        fb_draw_str_spec(fb, sx + 64u, sy + 4u, "BYPASS", COL_MUTED, &k_font_md);
    }
    if (s->midi_clk_active && sp != 2u) {
        fb_draw_str_spec(fb, sx + 132u, sy + 5u, "SYNC", th->accent, &k_font_sm_bold);
    }
    draw_voice_pips(fb, s, sx, sy + 1u, sw);

    /* Plot bounds on left (138 px wide, 77 px tall) */
    const uint32_t wx0 = sx + 8u;
    const uint32_t ww  = 138u;
    const uint32_t wy0 = sy + 20u;
    const uint32_t wy1 = sy + sh - 8u;
    const uint32_t wh  = wy1 - wy0;
    uint16_t fill_col  = make_env_fill_col(th->accent);

    if (sp == 2u) {
        /* FX 3/3: Soft-Knee 4:1 Bus Compression Transfer Curve + Threshold Crosshair + Top-Down GR Meter */
        const uint32_t cx0 = wx0;
        const uint32_t cw  = 114u;
        int32_t thr_db     = adrift_fx_comp_thresh_db(fp->comp_thresh); /* -24 .. -2 dB */
        int32_t mix_k      = dsp_clamp_i32(fp->comp_mix, 0, 1023);
        int32_t mkp_q8     = (dsp_clamp_i32(fp->comp_makeup, 0, 1023) * 12 * 256) / 1023;
        uint32_t thr_dx    = (uint32_t)(((36 + thr_db) * (int32_t)(cw - 1u)) / 36);
        uint32_t thr_y     = wy1 - ((thr_dx * (wh - 1u)) / (cw - 1u));

        /* Grid baseline, 1:1 unity diagonal, and threshold crosshair */
        for (uint32_t dx = 0u; dx < cw; dx += 3u) {
            fb[wy1 * ADRIFT_SCREEN_W + (cx0 + dx)] = COL_SCOPE_GRID;
            fb[thr_y * ADRIFT_SCREEN_W + (cx0 + dx)] = COL_BAR_TRACK;
            uint32_t uy = wy1 - ((dx * (wh - 1u)) / (cw - 1u));
            fb[uy * ADRIFT_SCREEN_W + (cx0 + dx)] = COL_SCOPE_GRID;
        }
        for (uint32_t dy = 0u; dy < wh; dy += 3u) {
            fb[(wy0 + dy) * ADRIFT_SCREEN_W + (cx0 + thr_dx)] = COL_BAR_TRACK;
        }

        uint32_t prev_py = wy1;
        uint16_t trace_c = (mix_k > 0) ? th->accent : COL_MUTED;
        for (uint32_t dx = 0u; dx < cw; dx++) {
            int32_t in_q8   = -36 * 256 + ((int32_t)dx * 36 * 256) / (int32_t)(cw - 1u);
            int32_t over_q8 = in_q8 - (thr_db * 256);
            int32_t gr_q8   = 0;
            if (over_q8 > -768) {
                if (over_q8 >= 768) {
                    gr_q8 = (over_q8 * 3) >> 2;
                } else {
                    int32_t u = over_q8 + 768;
                    gr_q8 = (u * u) >> 12;
                }
            }
            int32_t wet_q8  = in_q8 + mkp_q8 - gr_q8;
            int32_t out_q8  = in_q8 + (((wet_q8 - in_q8) * mix_k) >> 10);
            int32_t norm_y  = ((out_q8 + 36 * 256) * (int32_t)(wh - 1u)) / (36 * 256);
            uint32_t py     = (uint32_t)dsp_clamp_i32((int32_t)wy1 - norm_y, (int32_t)wy0, (int32_t)wy1);
            uint32_t x      = cx0 + dx;

            if (mix_k > 0 && py < wy1) {
                fb_fill_rect(fb, x, py, 1u, wy1 - py, fill_col);
            }
            uint16_t lcol = (mix_k > 0 && over_q8 > -384) ? COL_WHITE : trace_c;
            if (dx > 0u) {
                uint32_t y0 = (prev_py < py) ? prev_py : py;
                uint32_t y1 = (prev_py < py) ? py : prev_py;
                fb_fill_rect(fb, x, y0, 1u, y1 - y0 + 1u, lcol);
            } else {
                fb[py * ADRIFT_SCREEN_W + x] = lcol;
            }
            prev_py = py;
        }

        /* Studio top-down Gain Reduction (GR) meter on right of transfer plot */
        uint32_t gx     = wx0 + 120u;
        uint32_t m_top  = wy0 + 10u;
        uint32_t m_max  = wh - 10u;
        uint32_t gr_h   = (mix_k > 0)
                              ? (uint32_t)dsp_clamp_i32((s->fx.meter_comp_gr_q15 * (int32_t)(m_max - 2u)) >> 15,
                                                        0, (int32_t)(m_max - 2u))
                              : 0u;
        fb_draw_str_spec(fb, gx + 2u, wy0, "GR", gr_h > 0u ? th->accent : COL_MUTED, &k_font_sm_bold);
        fb_fill_rect(fb, gx + 2u, m_top, 12u, m_max, COL_BAR_TRACK);
        fb_rect_outline(fb, gx + 2u, m_top, 12u, m_max, COL_SCOPE_GRID);
        if (gr_h > 0u) {
            fb_fill_rect(fb, gx + 3u, m_top + 1u, 10u, gr_h, th->accent);
            uint32_t cap_y = m_top + gr_h - (gr_h >= 2u ? 1u : 0u);
            fb_fill_rect(fb, gx + 3u, cap_y, 10u, gr_h >= 2u ? 2u : 1u, COL_WHITE);
        }
    } else {
        /* 1. Reverb Diffusion & Shimmer Decay Cloud */
        int32_t rvb_mix  = dsp_clamp_i32(fp->reverb_mix, 0, 1023);
        int32_t rvb_size = dsp_clamp_i32(fp->reverb_size, 0, 1023);
        int32_t rvb_shim = dsp_clamp_i32(fp->reverb_shimmer, 0, 1023);
        uint32_t tail_w  = 24u + ((uint32_t)rvb_size * (ww - 26u)) / 1023u;
        uint32_t peak_h  = ((uint32_t)rvb_mix * (wh - 12u)) / 1023u;

        for (uint32_t dx = 0; dx < ww; dx += 4u) {
            fb[wy1 * ADRIFT_SCREEN_W + (wx0 + dx)] = COL_SCOPE_GRID;
            fb[(wy0 + wh / 2u) * ADRIFT_SCREEN_W + (wx0 + dx)] = COL_SCOPE_GRID;
        }

        if (peak_h > 2u) {
            uint32_t prev_ry = wy1;
            for (uint32_t dx = 0; dx < ww; dx++) {
                uint32_t env_q15 = interp_lut16_q15(k_rc_dec_q15, dx < tail_w ? dx : tail_w, tail_w);
                uint32_t h       = (peak_h * env_q15) >> 15;
                if (rvb_shim > 0 && h > 2u) {
                    int32_t rip = dsp_sine_lut_q15(dx * 0x0A000000u);
                    int32_t dh  = ((rip * rvb_shim) >> 10) * (int32_t)h;
                    int32_t hh  = (int32_t)h + (dh >> 16);
                    if (hh < 0) hh = 0;
                    if (hh > (int32_t)(wh - 4u)) hh = (int32_t)(wh - 4u);
                    h = (uint32_t)hh;
                }
                uint32_t ry = wy1 - h;
                if (h > 0u) {
                    fb_fill_rect(fb, wx0 + dx, ry, 1u, h, fill_col);
                }
                if (dx > 0u) {
                    uint32_t y0 = prev_ry < ry ? prev_ry : ry;
                    uint32_t y1 = prev_ry < ry ? ry : prev_ry;
                    fb_fill_rect(fb, wx0 + dx, y0, 1u, y1 - y0 + 1u,
                                 (rvb_shim > 200 && (dx & 3u) == 0u) ? COL_WHITE : th->dim);
                }
                prev_ry = ry;
            }
        }

        /* 2. Stereo Ping-Pong Tape Delay Echo Taps */
        int32_t dly_mix  = dsp_clamp_i32(fp->delay_mix, 0, 1023);
        int32_t dly_time = dsp_clamp_i32(fp->delay_time, 0, 1023);
        int32_t dly_fb   = dsp_clamp_i32(fp->delay_fb, 0, 1023);
        if (dly_mix > 0) {
            uint32_t step_px = 10u + ((uint32_t)dly_time * 22u) / 1023u;
            int32_t  tap_amp = (dly_mix * (int32_t)(wh - 10u)) / 1023;
            int32_t  fb_q10  = 220 + (dly_fb * 720) / 1023;
            uint32_t tap_idx = 0u;
            for (uint32_t tx = 6u; tx + 3u < ww && tap_amp >= 4; tx += step_px) {
                uint32_t th_px = (uint32_t)tap_amp;
                uint32_t ty0   = wy1 - th_px;
                uint16_t tcol  = ((tap_idx & 1u) == 0u) ? th->accent : COL_WHITE;
                fb_fill_rect(fb, wx0 + tx, ty0, 3u, th_px, tcol);
                fb_fill_rect(fb, wx0 + tx, ty0, 3u, 2u, COL_WHITE);
                tap_amp = (tap_amp * fb_q10) >> 10;
                tap_idx++;
            }
        }

        /* 3. Stereo BBD Dimension Chorus Quadrature Wave */
        int32_t chr_mix = dsp_clamp_i32(fp->chorus_mix, 0, 1023);
        if (chr_mix > 0) {
            int32_t  mid_y = (int32_t)(wy0 + wh / 2u);
            int32_t  amp   = 2 + (chr_mix * 18) / 1023;
            uint32_t prev_l = (uint32_t)mid_y;
            uint32_t prev_r = (uint32_t)mid_y;
            for (uint32_t dx = 0; dx < ww; dx++) {
                uint32_t ph = dx * (0xFFFFFFFFu / ww) * 2u;
                int32_t  sl = (dsp_sine_lut_q15(ph) * amp) >> 15;
                int32_t  sr = (dsp_sine_lut_q15(ph + 0x80000000u) * amp) >> 15;
                uint32_t yl = (uint32_t)dsp_clamp_i32(mid_y - sl, (int32_t)wy0, (int32_t)wy1);
                uint32_t yr = (uint32_t)dsp_clamp_i32(mid_y - sr, (int32_t)wy0, (int32_t)wy1);
                if (dx > 0u) {
                    uint32_t l0 = prev_l < yl ? prev_l : yl;
                    uint32_t l1 = prev_l < yl ? yl : prev_l;
                    fb_fill_rect(fb, wx0 + dx, l0, 1u, l1 - l0 + 1u, th->accent);
                    uint32_t r0 = prev_r < yr ? prev_r : yr;
                    uint32_t r1 = prev_r < yr ? yr : prev_r;
                    fb_fill_rect(fb, wx0 + dx, r0, 1u, r1 - r0 + 1u, COL_WHITE);
                }
                prev_l = yl;
                prev_r = yr;
            }
        }

        /* 4. Slim live Bus Compressor GR strip on right edge when active */
        if (fp->comp_mix > 0) {
            uint32_t gr_h = (uint32_t)dsp_clamp_i32((s->fx.meter_comp_gr_q15 * (int32_t)wh) >> 15, 0, (int32_t)wh);
            fb_fill_rect(fb, wx0 + ww - 4u, wy0, 4u, wh, COL_BAR_TRACK);
            if (gr_h > 0u) {
                fb_fill_rect(fb, wx0 + ww - 4u, wy0, 4u, gr_h, th->accent);
            }
        }
    }

    /* Right-side compact summary panel */
    uint32_t rx = sx + 152u;
    fb_fill_rect(fb, rx - 4u, sy + 20u, 1u, 78u, COL_SCOPE_GRID);
    char buf[16];

    fb_draw_str_spec(fb, rx, sy + 22u, "CHR", COL_MUTED, &k_font_sm_bold);
    fmt_int(buf, (fp->chorus_mix * 100) / 1023, "%");
    fb_draw_str_spec(fb, rx + 34u, sy + 22u, buf, fp->chorus_mix > 0 ? th->accent : COL_WHITE, &k_font_sm_bold);

    fb_draw_str_spec(fb, rx, sy + 36u, "DLY", COL_MUTED, &k_font_sm_bold);
    fmt_int(buf, (fp->delay_mix * 100) / 1023, "%");
    fb_draw_str_spec(fb, rx + 34u, sy + 36u, buf, fp->delay_mix > 0 ? th->accent : COL_WHITE, &k_font_sm_bold);

    fb_draw_str_spec(fb, rx, sy + 50u, "RVB", COL_MUTED, &k_font_sm_bold);
    fmt_int(buf, (fp->reverb_mix * 100) / 1023, "%");
    fb_draw_str_spec(fb, rx + 34u, sy + 50u, buf, fp->reverb_mix > 0 ? th->accent : COL_WHITE, &k_font_sm_bold);

    if (sp == 2u) {
        fb_draw_str_spec(fb, rx, sy + 64u, "THR", COL_MUTED, &k_font_sm_bold);
        fmt_int(buf, adrift_fx_comp_thresh_db(fp->comp_thresh), "DB");
        fb_draw_str_spec(fb, rx + 34u, sy + 64u, buf, COL_WHITE, &k_font_sm_bold);
    } else if (sp == 1u || (fp->reverb_shimmer > 0 && fp->reverb_mix > 0)) {
        fb_draw_str_spec(fb, rx, sy + 64u, "SHIM", COL_MUTED, &k_font_sm_bold);
        fmt_int(buf, (fp->reverb_shimmer * 100) / 1023, "%");
        fb_draw_str_spec(fb, rx + 34u, sy + 64u, buf,
                         (fp->reverb_shimmer > 0 && fp->reverb_mix > 0) ? th->accent : COL_WHITE, &k_font_sm_bold);
    } else {
        fb_draw_str_spec(fb, rx, sy + 64u, s->midi_clk_active ? "SYNC" : "TIME",
                         s->midi_clk_active ? th->accent : COL_MUTED, &k_font_sm_bold);
        if (s->midi_clk_active) {
            copy_str(buf, adrift_fx_delay_subdiv_name(fp->delay_time));
        } else {
            fmt_ms(buf, (int32_t)adrift_fx_delay_time_ms(fp->delay_time));
        }
        fb_draw_str_spec(fb, rx + 34u, sy + 64u, buf,
                         s->midi_clk_active ? th->accent : COL_WHITE, &k_font_sm_bold);
    }

    int32_t gr_db = (fp->comp_mix > 0) ? ((s->fx.meter_comp_gr_q15 * 18 + 16384) >> 15) : 0;
    fb_draw_str_spec(fb, rx, sy + 78u, "CMP", sp == 2u ? th->accent : COL_MUTED, &k_font_sm_bold);
    if (sp == 2u && gr_db > 0) {
        fmt_int(buf, -gr_db, "DB");
        fb_draw_str_spec(fb, rx + 34u, sy + 78u, buf, th->accent, &k_font_sm_bold);
    } else {
        fmt_int(buf, (fp->comp_mix * 100) / 1023, "%");
        fb_draw_str_spec(fb, rx + 34u, sy + 78u, buf, fp->comp_mix > 0 ? th->accent : COL_WHITE, &k_font_sm_bold);
    }
}

/* ============================================================================
 * HOME Screen Multi-Page Visualizations (Pages 1/6 .. 6/6) & 4-Knob View Bar
 *   1:TIME/ZOOM (1X, 2X, 4X, 8X)
 *   2:GAIN      (25% .. 400%)
 *   3:FREEZE    (RUN / HOLD)
 *   4:MODE      (3 display modes per page)
 * ============================================================================ */

void draw_home_knob_bar(uint16_t *fb,
                        const adrift_ui_t *ui,
                        const adrift_group_theme_t *th,
                        const char *mode_name) {
    static const char *const k_zoom_names[4] = { "1X", "2X", "4X", "8X" };
    const uint32_t sx = 5u, sy = 31u, sw = 230u, sh = 204u;
    const uint32_t by = sy + sh - 16u;
    const uint32_t ty = by + 4u;
    uint8_t sp   = ui->scope_page % ADRIFT_UI_NUM_HOME_PAGES;
    uint8_t zoom = ui->scope_zoom & 3u;
    uint8_t mode = ui->scope_mode[sp] % ADRIFT_UI_NUM_HOME_MODES;

    fb_hline(fb, sx + 1u, by - 1u, sw - 2u, COL_SCOPE_GRID);
    fb_fill_rect(fb, sx + 1u, by, sw - 2u, 15u, COL_SCOPE_BG);

    /* 1:ZOOM */
    fb_draw_str_spec(fb, sx + 6u, ty, "1:ZM", COL_MUTED, &k_font_sm);
    fb_draw_str_spec(fb, sx + 32u, ty, k_zoom_names[zoom],
                     zoom > 0u ? th->accent : COL_WHITE, &k_font_sm_bold);

    /* 2:GAIN */
    char gbuf[12];
    fmt_int(gbuf, (int32_t)ui->scope_gain, "%");
    fb_draw_str_spec(fb, sx + 56u, ty, "2:GN", COL_MUTED, &k_font_sm);
    fb_draw_str_spec(fb, sx + 82u, ty, gbuf,
                     ui->scope_gain != 100u ? th->accent : COL_WHITE, &k_font_sm_bold);

    /* 3:FREEZE */
    fb_draw_str_spec(fb, sx + 116u, ty, "3:", COL_MUTED, &k_font_sm);
    if (ui->scope_freeze) {
        fb_fill_rect(fb, sx + 129u, by + 2u, 32u, 11u, th->accent);
        fb_draw_str_centered_spec(fb, sx + 129u, 32u, ty, "HOLD", COL_BG, &k_font_sm_bold);
    } else {
        fb_draw_str_spec(fb, sx + 130u, ty, "RUN", COL_WHITE, &k_font_sm_bold);
    }

    /* 4:MODE */
    fb_draw_str_spec(fb, sx + 168u, ty, "4:", COL_MUTED, &k_font_sm);
    fb_draw_str_spec(fb, sx + 182u, ty, mode_name,
                     mode > 0u ? th->accent : COL_WHITE, &k_font_sm_bold);
}

static bool str_eq_short(const char *a, const char *b) {
    uint32_t i = 0u;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return false;
        i++;
    }
    return a[i] == b[i];
}

static void copy_str_cap16(char *dst, const char *src) {
    uint32_t i = 0u;
    while (src && src[i] && i < 15u) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static inline uint32_t div255_u32(uint32_t x) {
    return (x + 1u + (x >> 8)) >> 8;
}

static inline int32_t mul_gain_pct(int32_t v, int32_t gain) {
    if (gain == 100) return v;
    if ((gain % 25) == 0) {
        int32_t p = v * (gain / 25);
        return (p >= 0) ? (p >> 2) : -((-p) >> 2);
    }
    return (v * gain) / 100;
}

static bool scope_has_stereo_lr(const adrift_synth_t *s) {
    uint32_t last = (s->scope_wr + ADRIFT_SCOPE_LEN - 1u) & (ADRIFT_SCOPE_LEN - 1u);
    if (s->scope_ring_l[last] != 0 || s->scope_ring_r[last] != 0) {
        return true;
    }
    for (uint32_t i = 0; i < ADRIFT_SCOPE_LEN; i++) {
        if (s->scope_ring_l[i] != 0 || s->scope_ring_r[i] != 0) {
            return true;
        }
    }
    return false;
}

static void update_home_chrome(uint16_t *fb,
                               adrift_ui_t *ui,
                               const adrift_synth_t *s,
                               const char *title,
                               const char *subtitle,
                               uint16_t sub_col,
                               const char *mode_name,
                               bool full_chrome,
                               bool pips_dirty,
                               adrift_dirty_t *dirty) {
    const adrift_group_theme_t *th = get_group_theme(s->edit_group);
    const uint32_t sx = 5u, sy = 31u, sw = 230u, sh = 204u;
    const char *sub_safe = subtitle ? subtitle : "";

    if (full_chrome) {
        fb_fill_rect(fb, 0, 28, ADRIFT_SCREEN_W, 212, COL_BG);
        fb_fill_rect(fb, sx, sy, sw, sh, COL_SCOPE_BG);
        fb_rect_outline(fb, sx, sy, sw, sh, th->border);

        draw_screen_page_badge(fb, sx, sy + 1u, title, ui->scope_page, th);
        if (sub_safe[0]) {
            fb_draw_str_spec(fb, sx + 76u, sy + 5u, sub_safe, sub_col, &k_font_md);
        }
        if (ui->scope_freeze) {
            fb_draw_str_spec(fb, sx + 138u, sy + 6u, "HOLD", th->accent, &k_font_sm_bold);
        }
        draw_voice_pips(fb, s, sx, sy + 1u, sw);
        fb_hline(fb, sx + 1u, sy + 19u, sw - 2u, COL_SCOPE_GRID);
        draw_home_knob_bar(fb, ui, th, mode_name);

        copy_str_cap16(ui->home_sub_prev, sub_safe);
        ui->home_sub_col_prev = sub_col;
        adrift_dirty_mark_rows(dirty, 28u, 212u);
        return;
    }

    if (!str_eq_short(sub_safe, ui->home_sub_prev) || sub_col != ui->home_sub_col_prev) {
        fb_fill_rect(fb, sx + 76u, sy + 5u, 60u, 10u, COL_SCOPE_BG);
        if (sub_safe[0]) {
            fb_draw_str_spec(fb, sx + 76u, sy + 5u, sub_safe, sub_col, &k_font_md);
        }
        copy_str_cap16(ui->home_sub_prev, sub_safe);
        ui->home_sub_col_prev = sub_col;
        adrift_dirty_mark_rows(dirty, sy + 5u, 10u);
    }
    if (pips_dirty) {
        fb_fill_rect(fb, sx + sw - 60u, sy + 3u, 58u, 13u, COL_SCOPE_BG);
        draw_voice_pips(fb, s, sx, sy + 1u, sw);
        adrift_dirty_mark_rows(dirty, sy + 3u, 13u);
    }
}

/* Bresenham 2D line primitive clipped to inner viewport [xmin..xmax, ymin..ymax] */
static void fb_draw_line_clipped(uint16_t *fb,
                                 int32_t x0,
                                 int32_t y0,
                                 int32_t x1,
                                 int32_t y1,
                                 int32_t xmin,
                                 int32_t ymin,
                                 int32_t xmax,
                                 int32_t ymax,
                                 uint16_t col) {
    int32_t dx  = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int32_t sx  = (x0 < x1) ? 1 : -1;
    int32_t dy  = (y1 > y0) ? -(y1 - y0) : -(y0 - y1);
    int32_t sy  = (y0 < y1) ? 1 : -1;
    int32_t err = dx + dy;

    for (uint32_t steps = 0; steps < 256u; steps++) {
        if (x0 >= xmin && x0 <= xmax && y0 >= ymin && y0 <= ymax) {
            fb[(uint32_t)y0 * ADRIFT_SCREEN_W + (uint32_t)x0] = col;
        }
        if (x0 == x1 && y0 == y1) break;
        int32_t e2 = err * 2;
        if (e2 >= dy) {
            err += dy;
            x0  += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0  += sy;
        }
    }
}

/* ----------------------------------------------------------------------------
 * Page 2/6: VECT — Stereo Vectorscope / Lissajous Goniometer
 * ---------------------------------------------------------------------------- */
static void draw_vect_reticle(uint16_t *fb, uint8_t mode, int32_t clip_y0, int32_t clip_y1) {
    const int32_t cx   = 84;
    const int32_t cy   = 134;
    const int32_t rad  = 64;
    const int32_t xmin = 14, xmax = 154;

    if (mode == 1u) {
        /* X-Y mode: square frame (20..147, 70..197) + crosshairs + 45-deg mono diagonal */
        int32_t top_y = cy - rad;
        int32_t bot_y = cy + rad - 1;
        if (top_y >= clip_y0 && top_y <= clip_y1) {
            fb_hline(fb, (uint32_t)(cx - rad), (uint32_t)top_y, (uint32_t)(rad * 2), COL_SCOPE_GRID);
        }
        if (bot_y >= clip_y0 && bot_y <= clip_y1) {
            fb_hline(fb, (uint32_t)(cx - rad), (uint32_t)bot_y, (uint32_t)(rad * 2), COL_SCOPE_GRID);
        }
        int32_t vy0 = (top_y > clip_y0) ? top_y : clip_y0;
        int32_t vy1 = (bot_y < clip_y1) ? bot_y : clip_y1;
        if (vy0 <= vy1) {
            uint32_t vh = (uint32_t)(vy1 - vy0 + 1);
            fb_fill_rect(fb, (uint32_t)(cx - rad),     (uint32_t)vy0, 1u, vh, COL_SCOPE_GRID);
            fb_fill_rect(fb, (uint32_t)(cx + rad - 1), (uint32_t)vy0, 1u, vh, COL_SCOPE_GRID);
        }
        for (int32_t d = -rad; d <= rad; d += 3) {
            if (cy >= clip_y0 && cy <= clip_y1) {
                fb[(uint32_t)cy * ADRIFT_SCREEN_W + (uint32_t)(cx + d)] = COL_SCOPE_GRID;
            }
            if (cy + d >= clip_y0 && cy + d <= clip_y1) {
                fb[(uint32_t)(cy + d) * ADRIFT_SCREEN_W + (uint32_t)cx] = COL_SCOPE_GRID;
            }
            if (cy - d >= clip_y0 && cy - d <= clip_y1) {
                fb[(uint32_t)(cy - d) * ADRIFT_SCREEN_W + (uint32_t)(cx + d)] = COL_SCOPE_GRID;
            }
        }
        if (cy + 9 >= clip_y0 && cy + 3 <= clip_y1) {
            fb_draw_str_spec(fb, (uint32_t)(cx + rad - 10), (uint32_t)(cy + 3), "L", COL_MUTED, &k_font_sm);
        }
        if (cy - rad + 9 >= clip_y0 && cy - rad + 3 <= clip_y1) {
            fb_draw_str_spec(fb, (uint32_t)(cx + 3), (uint32_t)(cy - rad + 3), "R", COL_MUTED, &k_font_sm);
        }
    } else {
        /* GONI / DOTS mode: rotated 45-deg diamond reticle (Mid vertical, Side horizontal) */
        fb_draw_line_clipped(fb, cx, cy - rad, cx + rad, cy, xmin, clip_y0, xmax, clip_y1, COL_SCOPE_GRID);
        fb_draw_line_clipped(fb, cx + rad, cy, cx, cy + rad, xmin, clip_y0, xmax, clip_y1, COL_SCOPE_GRID);
        fb_draw_line_clipped(fb, cx, cy + rad, cx - rad, cy, xmin, clip_y0, xmax, clip_y1, COL_SCOPE_GRID);
        fb_draw_line_clipped(fb, cx - rad, cy, cx, cy - rad, xmin, clip_y0, xmax, clip_y1, COL_SCOPE_GRID);
        for (int32_t d = -rad; d <= rad; d += 3) {
            if (cy >= clip_y0 && cy <= clip_y1) {
                fb[(uint32_t)cy * ADRIFT_SCREEN_W + (uint32_t)(cx + d)] = COL_SCOPE_GRID;
            }
            if (cy + d >= clip_y0 && cy + d <= clip_y1) {
                fb[(uint32_t)(cy + d) * ADRIFT_SCREEN_W + (uint32_t)cx] = COL_SCOPE_GRID;
            }
        }
        if (cy - rad + 8 >= clip_y0 && cy - rad + 2 <= clip_y1) {
            fb_draw_str_spec(fb, (uint32_t)(cx + 3), (uint32_t)(cy - rad + 2), "M", COL_MUTED, &k_font_sm);
        }
        if (cy - 2 >= clip_y0 && cy - 8 <= clip_y1) {
            fb_draw_str_spec(fb, (uint32_t)(cx - rad + 2),  (uint32_t)(cy - 8), "+S", COL_MUTED, &k_font_sm);
            fb_draw_str_spec(fb, (uint32_t)(cx + rad - 11), (uint32_t)(cy - 8), "-S", COL_MUTED, &k_font_sm);
        }
        if (cy - rad / 2 - 2 >= clip_y0 && cy - rad / 2 - 8 <= clip_y1) {
            fb_draw_str_spec(fb, (uint32_t)(cx - rad / 2 - 8), (uint32_t)(cy - rad / 2 - 8), "L", COL_MUTED, &k_font_sm);
            fb_draw_str_spec(fb, (uint32_t)(cx + rad / 2 + 3), (uint32_t)(cy - rad / 2 - 8), "R", COL_MUTED, &k_font_sm);
        }
    }
}

static void draw_home_vect_page(uint16_t *fb,
                                adrift_ui_t *ui,
                                const adrift_synth_t *s,
                                bool full_chrome,
                                bool pips_dirty,
                                adrift_dirty_t *dirty) {
    static const char *const k_vect_modes[3] = { "GONI", "X-Y", "DOTS" };
    const adrift_group_theme_t *th = get_group_theme(s->edit_group);
    uint8_t mode = ui->scope_mode[ADRIFT_HOME_VECT] % ADRIFT_UI_NUM_HOME_MODES;
    uint8_t zoom = ui->scope_zoom & 3u;
    int32_t gain = (int32_t)ui->scope_gain;

    if (!full_chrome && ui->scope_freeze) {
        update_home_chrome(fb, ui, s, "VECT", ui->home_sub_prev, ui->home_sub_col_prev,
                           k_vect_modes[mode], false, pips_dirty, dirty);
        return;
    }

    /* Compute stereo peak L/R, correlation (-100..+100), and stereo width % */
    int32_t peak_l   = 0;
    int32_t peak_r   = 0;
    int32_t corr_num = 0;
    int32_t corr_den = 0;
    int32_t side_sum = 0;
    int32_t tot_sum  = 0;
    uint8_t base_idx = s->scope_wr;
    bool    has_lr   = scope_has_stereo_lr(s);

    for (uint32_t i = 0; i < 64u; i++) {
        uint32_t idx = (base_idx + 64u + i) & (ADRIFT_SCOPE_LEN - 1u);
        int32_t  sl  = has_lr ? s->scope_ring_l[idx] : s->scope_ring[idx];
        int32_t  sr  = has_lr ? s->scope_ring_r[idx] : s->scope_ring[idx];
        int32_t  al  = sl < 0 ? -sl : sl;
        int32_t  ar  = sr < 0 ? -sr : sr;
        if (al > peak_l) peak_l = al;
        if (ar > peak_r) peak_r = ar;

        int32_t l8 = sl >> 4;
        int32_t r8 = sr >> 4;
        corr_num += l8 * r8;
        corr_den += ((l8 * l8) + (r8 * r8)) >> 1;

        int32_t sd = sl - sr;
        if (sd < 0) sd = -sd;
        side_sum += sd >> 4;
        tot_sum  += (al + ar) >> 4;
    }

    int32_t c_num = corr_num;
    int32_t c_den = corr_den;
    while (c_den > 16000000 || c_num > 16000000 || c_num < -16000000) {
        c_num >>= 2;
        c_den >>= 2;
    }
    int32_t corr_pct  = (corr_den > 64 && c_den > 0) ? dsp_clamp_i32((c_num * 100) / c_den, -100, 100) : 100;
    int32_t width_pct = (tot_sum > 64)  ? dsp_clamp_i32((side_sum * 100) / tot_sum, 0, 100)     : 0;

    const char *sub = (width_pct >= 25) ? "STEREO" : ((tot_sum > 64) ? "MONO" : "IDLE");
    update_home_chrome(fb, ui, s, "VECT", sub, (width_pct >= 25) ? th->accent : COL_MUTED,
                       k_vect_modes[mode], full_chrome, pips_dirty, dirty);

    /* Left Goniometer / Lissajous Field: center (84, 134), radius 64 */
    const int32_t cx   = 84;
    const int32_t cy   = 134;
    const int32_t xmin = 14, xmax = 154, ymin = 56, ymax = 212;

    uint32_t n_pts    = 32u + (uint32_t)zoom * 32u;
    uint32_t start_of = ADRIFT_SCOPE_LEN - n_pts;
    uint8_t  px_arr[128];
    uint8_t  py_arr[128];
    uint32_t cur_y0 = 213u;
    uint32_t cur_y1 = 56u;

    for (uint32_t i = 0; i < n_pts; i++) {
        uint32_t idx = (base_idx + start_of + i) & (ADRIFT_SCOPE_LEN - 1u);
        int32_t  sl  = has_lr ? s->scope_ring_l[idx] : s->scope_ring[idx];
        int32_t  sr  = has_lr ? s->scope_ring_r[idx] : s->scope_ring[idx];
        int32_t  vx, vy;

        if (mode == 1u) {
            vx = mul_gain_pct(sl >> 9, gain);
            vy = mul_gain_pct(sr >> 9, gain);
        } else {
            int32_t mid  = (sl + sr) >> 1;
            int32_t side = (sr - sl) >> 1;
            vx = mul_gain_pct(side >> 8, gain);
            vy = mul_gain_pct(mid  >> 8, gain);
        }

        int32_t px = dsp_clamp_i32(cx + vx, xmin, xmax);
        int32_t py = dsp_clamp_i32(cy - vy, ymin, ymax);
        px_arr[i] = (uint8_t)px;
        py_arr[i] = (uint8_t)py;

        uint32_t py0 = (uint32_t)py;
        uint32_t py1 = (mode == 2u) ? (py0 + 1u) : py0;
        if (py0 < cur_y0) cur_y0 = py0;
        if (py1 > cur_y1) cur_y1 = py1;
    }

    if (full_chrome) {
        draw_vect_reticle(fb, mode, 56, 213);
    } else {
        uint32_t clr_y0 = (ui->vect_prev_y0 < cur_y0) ? ui->vect_prev_y0 : cur_y0;
        uint32_t clr_y1 = (ui->vect_prev_y1 > cur_y1) ? ui->vect_prev_y1 : cur_y1;
        if (clr_y0 < 56u)  clr_y0 = 56u;
        if (clr_y1 > 213u) clr_y1 = 213u;
        fb_fill_rect(fb, 14u, clr_y0, 142u, clr_y1 - clr_y0 + 1u, COL_SCOPE_BG);
        draw_vect_reticle(fb, mode, (int32_t)clr_y0, (int32_t)clr_y1);
        adrift_dirty_mark_rows(dirty, clr_y0, clr_y1 - clr_y0 + 1u);
    }

    int32_t prev_x = (int32_t)px_arr[0];
    int32_t prev_y = (int32_t)py_arr[0];
    for (uint32_t i = 0; i < n_pts; i++) {
        int32_t  px   = (int32_t)px_arr[i];
        int32_t  py   = (int32_t)py_arr[i];
        uint16_t pcol = (i >= n_pts - 8u) ? COL_WHITE
                      : ((i >= (n_pts >> 1)) ? th->accent : th->dim);
        if (mode == 2u) {
            fb_fill_rect(fb, (uint32_t)px, (uint32_t)py, 2u, 2u, pcol);
        } else if (i > 0u) {
            fb_draw_line_clipped(fb, prev_x, prev_y, px, py, xmin, ymin, xmax, ymax, pcol);
        } else {
            fb[(uint32_t)py * ADRIFT_SCREEN_W + (uint32_t)px] = pcol;
        }
        prev_x = px;
        prev_y = py;
    }
    ui->vect_prev_y0 = (uint8_t)cur_y0;
    ui->vect_prev_y1 = (uint8_t)cur_y1;

    /* Right-side Stereo L/R Level & Phase Correlation Panel (x = 161..228) */
    const uint32_t rx  = 166u;
    const uint32_t my0 = 67u, mh = 76u;
    uint32_t hl = (uint32_t)dsp_clamp_i32(mul_gain_pct((peak_l * (int32_t)mh) >> 15, gain), 0, (int32_t)mh);
    uint32_t hr = (uint32_t)dsp_clamp_i32(mul_gain_pct((peak_r * (int32_t)mh) >> 15, gain), 0, (int32_t)mh);

    const uint32_t cbx = rx, cby = 161u, cbw = 60u, cbh = 8u;
    int8_t corr_px = (corr_pct >= 0)
                         ? (int8_t)(((uint32_t)corr_pct * (cbw / 2u)) / 100u)
                         : -(int8_t)(((uint32_t)(-corr_pct) * (cbw / 2u)) / 100u);

    if (full_chrome) {
        fb_fill_rect(fb, rx - 5u, 54u, 1u, 158u, COL_SCOPE_GRID);
        fb_draw_str_spec(fb, rx + 6u,  56u, "L",  th->accent, &k_font_sm_bold);
        fb_draw_str_spec(fb, rx + 26u, 56u, "R",  COL_WHITE,  &k_font_sm_bold);
        fb_draw_str_spec(fb, rx + 42u, 56u, "PK", COL_MUTED,  &k_font_sm);

        fb_fill_rect(fb, rx + 4u,  my0, 12u, mh, COL_BAR_TRACK);
        fb_fill_rect(fb, rx + 24u, my0, 12u, mh, COL_BAR_TRACK);
        if (hl > 0u) fb_fill_rect(fb, rx + 4u,  my0 + mh - hl, 12u, hl, th->accent);
        if (hr > 0u) fb_fill_rect(fb, rx + 24u, my0 + mh - hr, 12u, hr, COL_WHITE);

        for (uint32_t ty = my0; ty <= my0 + mh; ty += 19u) {
            fb_hline(fb, rx + 40u, ty, 6u, COL_SCOPE_GRID);
        }

        fb_draw_str_spec(fb, rx, 150u, "PHASE", COL_MUTED, &k_font_sm_bold);
        fb_fill_rect(fb, cbx, cby, cbw, cbh, COL_BAR_TRACK);
        fb_fill_rect(fb, cbx + cbw / 2u, cby - 1u, 1u, cbh + 2u, COL_WHITE);
        if (corr_px > 0) {
            fb_fill_rect(fb, cbx + cbw / 2u, cby + 1u, (uint32_t)corr_px, cbh - 2u, th->accent);
        } else if (corr_px < 0) {
            uint32_t cw = (uint32_t)(-corr_px);
            fb_fill_rect(fb, cbx + cbw / 2u - cw, cby + 1u, cw, cbh - 2u, COL_WHITE);
        }
        fb_draw_str_spec(fb, cbx,       172u, "-1", COL_MUTED, &k_font_sm);
        fb_draw_str_spec(fb, cbx + 27u, 172u, "0",  COL_MUTED, &k_font_sm);
        fb_draw_str_spec(fb, cbx + 48u, 172u, "+1", COL_MUTED, &k_font_sm);

        char wbuf[12];
        fb_draw_str_spec(fb, rx, 186u, "WIDTH", COL_MUTED, &k_font_sm_bold);
        fmt_int(wbuf, width_pct, "%");
        fb_draw_str_spec(fb, rx, 198u, wbuf, width_pct > 0 ? th->accent : COL_WHITE, &k_font_md);
    } else {
        uint32_t phl = ui->vect_prev_hl;
        if (hl > phl) {
            uint32_t y_up = my0 + mh - hl;
            uint32_t dh   = hl - phl;
            fb_fill_rect(fb, rx + 4u, y_up, 12u, dh, th->accent);
            adrift_dirty_mark_rows(dirty, y_up, dh);
        } else if (hl < phl) {
            uint32_t y_dn = my0 + mh - phl;
            uint32_t dh   = phl - hl;
            fb_fill_rect(fb, rx + 4u, y_dn, 12u, dh, COL_BAR_TRACK);
            adrift_dirty_mark_rows(dirty, y_dn, dh);
        }

        uint32_t phr = ui->vect_prev_hr;
        if (hr > phr) {
            uint32_t y_up = my0 + mh - hr;
            uint32_t dh   = hr - phr;
            fb_fill_rect(fb, rx + 24u, y_up, 12u, dh, COL_WHITE);
            adrift_dirty_mark_rows(dirty, y_up, dh);
        } else if (hr < phr) {
            uint32_t y_dn = my0 + mh - phr;
            uint32_t dh   = phr - hr;
            fb_fill_rect(fb, rx + 24u, y_dn, 12u, dh, COL_BAR_TRACK);
            adrift_dirty_mark_rows(dirty, y_dn, dh);
        }

        if (corr_px != ui->vect_prev_corr) {
            fb_fill_rect(fb, cbx, cby + 1u, cbw, cbh - 2u, COL_BAR_TRACK);
            fb_fill_rect(fb, cbx + cbw / 2u, cby + 1u, 1u, cbh - 2u, COL_WHITE);
            if (corr_px > 0) {
                fb_fill_rect(fb, cbx + cbw / 2u, cby + 1u, (uint32_t)corr_px, cbh - 2u, th->accent);
            } else if (corr_px < 0) {
                uint32_t cw = (uint32_t)(-corr_px);
                fb_fill_rect(fb, cbx + cbw / 2u - cw, cby + 1u, cw, cbh - 2u, COL_WHITE);
            }
            adrift_dirty_mark_rows(dirty, cby + 1u, cbh - 2u);
        }

        if ((uint8_t)width_pct != ui->vect_prev_width) {
            char wbuf[12];
            fb_fill_rect(fb, rx, 198u, 36u, 10u, COL_SCOPE_BG);
            fmt_int(wbuf, width_pct, "%");
            fb_draw_str_spec(fb, rx, 198u, wbuf, width_pct > 0 ? th->accent : COL_WHITE, &k_font_md);
            adrift_dirty_mark_rows(dirty, 198u, 10u);
        }
    }

    ui->vect_prev_hl    = (uint8_t)hl;
    ui->vect_prev_hr    = (uint8_t)hr;
    ui->vect_prev_corr  = corr_px;
    ui->vect_prev_width = (uint8_t)width_pct;
}

/* ----------------------------------------------------------------------------
 * Page 3/6: SPEC — Real-Time 64-Point Radix-2 FFT Spectrum Analyzer & Waterfall
 * ---------------------------------------------------------------------------- */
static const uint8_t k_bitrev64[64] = {
     0, 32, 16, 48,  8, 40, 24, 56,  4, 36, 20, 52, 12, 44, 28, 60,
     2, 34, 18, 50, 10, 42, 26, 58,  6, 38, 22, 54, 14, 46, 30, 62,
     1, 33, 17, 49,  9, 41, 25, 57,  5, 37, 21, 53, 13, 45, 29, 61,
     3, 35, 19, 51, 11, 43, 27, 59,  7, 39, 23, 55, 15, 47, 31, 63
};

static void update_home_spectrum(adrift_ui_t *ui, const adrift_synth_t *s) {
    if (ui->scope_freeze) return;

    int32_t re[64];
    int32_t im[64];
    uint8_t base_idx = s->scope_wr;
    bool    has_lr   = scope_has_stereo_lr(s);

    /* 1. Extract 64 samples into bit-reversed slots with Hann window + stereo side energy */
    for (uint32_t i = 0; i < 64u; i++) {
        uint32_t idx  = (base_idx + 64u + i) & (ADRIFT_SCOPE_LEN - 1u);
        int32_t  mono = s->scope_ring[idx];
        int32_t  side = has_lr ? ((s->scope_ring_l[idx] - s->scope_ring_r[idx]) >> 2) : 0;
        int32_t  samp = dsp_sat_q15(mono + side);
        /* Hann window: exact integer index into SRAM 1024-entry sine LUT */
        int32_t  cos_q15 = g_dsp_sine_q15[((i << 4) + 256u) & DSP_SINE_LUT_MASK];
        int32_t  win_q15 = (32768 - cos_q15) >> 1;
        uint32_t dst     = k_bitrev64[i];
        re[dst] = dsp_mul_q15(samp, win_q15) >> 1;
        im[dst] = 0;
    }

    /* 2. Stage 0 butterfly (wr = 1, wi = 0, im = 0): pure add/sub without multiplies */
    for (uint32_t i = 0; i < 64u; i += 2u) {
        int32_t r0 = re[i];
        int32_t r1 = re[i + 1u];
        re[i]      = (r0 + r1) >> 1;
        re[i + 1u] = (r0 - r1) >> 1;
    }

    /* 3. Stages 1..5 Radix-2 Decimation-in-Time FFT using SRAM sine LUT twiddles */
    for (uint32_t stage = 1u; stage < 6u; stage++) {
        uint32_t half     = 1u << stage;
        uint32_t step     = half << 1;
        uint32_t lut_step = 512u >> stage;
        for (uint32_t k = 0; k < half; k++) {
            uint32_t lut_idx = (0u - k * lut_step) & DSP_SINE_LUT_MASK;
            int32_t  wr      = g_dsp_sine_q15[(lut_idx + 256u) & DSP_SINE_LUT_MASK];
            int32_t  wi      = g_dsp_sine_q15[lut_idx];
            for (uint32_t i = k; i < 64u; i += step) {
                uint32_t j  = i + half;
                int32_t  tr = (wr * re[j] - wi * im[j]) >> 15;
                int32_t  ti = (wr * im[j] + wi * re[j]) >> 15;
                re[j] = (re[i] - tr) >> 1;
                im[j] = (im[i] - ti) >> 1;
                re[i] = (re[i] + tr) >> 1;
                im[i] = (im[i] + ti) >> 1;
            }
        }
    }

    /* 4. Compute 32 raw positive-frequency magnitudes (bins 1..32) with +3 dB/oct musical tilt */
    uint8_t raw_bins[ADRIFT_UI_SPEC_BINS];
    int32_t gain = (int32_t)ui->scope_gain;
    for (uint32_t b = 0; b < ADRIFT_UI_SPEC_BINS; b++) {
        uint32_t k   = b + 1u;
        int32_t  ar  = re[k] < 0 ? -re[k] : re[k];
        int32_t  ai  = im[k] < 0 ? -im[k] : im[k];
        int32_t  mag = (ar > ai) ? (ar + (ai >> 1)) : (ai + (ar >> 1));
        int32_t  tilted = mag + ((mag * (int32_t)k) >> 3);
        int32_t  scaled = mul_gain_pct(tilted, gain) >> 2;
        if (scaled > 255) scaled = 255;
        int32_t  comp = (scaled * (384 - scaled)) >> 8;
        raw_bins[b] = (uint8_t)dsp_clamp_i32(comp, 0, 255);
    }

    /* 5. Apply frequency zoom (1X=32 bins, 2X=16 bins, 4X=8 bins, 8X=4 bins interpolated) & peak decay */
    uint8_t zoom  = ui->scope_zoom & 3u;
    uint8_t decay = (uint8_t)(2u + zoom * 2u);
    uint8_t wf_r  = ui->spec_wf_head + 1u;
    if (wf_r >= ADRIFT_UI_WATERFALL_ROWS) wf_r = 0u;
    ui->spec_wf_head = wf_r;

    for (uint32_t b = 0; b < ADRIFT_UI_SPEC_BINS; b++) {
        uint32_t pos_q4 = (b << 4) >> zoom;
        uint32_t idx0   = pos_q4 >> 4;
        uint32_t idx1   = (idx0 + 1u < ADRIFT_UI_SPEC_BINS) ? (idx0 + 1u) : idx0;
        uint32_t frac   = pos_q4 & 0xFu;
        uint32_t val    = ((uint32_t)raw_bins[idx0] * (16u - frac) + (uint32_t)raw_bins[idx1] * frac) >> 4;

        uint32_t prev = ui->spec_bins[b];
        /* Exact /3u for x <= 32765: (x * 10923u) >> 15 */
        uint32_t sm   = (val > prev) ? ((val * 3u + prev) >> 2) : (((val + prev * 2u) * 10923u) >> 15);
        ui->spec_bins[b] = (uint8_t)sm;
        ui->spec_waterfall[wf_r][b] = (uint8_t)sm;

        if (sm >= ui->spec_peaks[b]) {
            ui->spec_peaks[b] = (uint8_t)sm;
        } else if (ui->spec_peaks[b] > decay) {
            ui->spec_peaks[b] = (uint8_t)(ui->spec_peaks[b] - decay);
        } else {
            ui->spec_peaks[b] = 0u;
        }
    }
}

static uint16_t waterfall_heat_col(uint8_t val, const adrift_group_theme_t *th, uint16_t fill_col) {
    if (val < 12u)  return COL_SCOPE_BG;
    if (val < 40u)  return fill_col;
    if (val < 88u)  return th->border;
    if (val < 148u) return th->dim;
    if (val < 210u) return th->accent;
    return COL_WHITE;
}

static void restore_bars_graticule(uint16_t *fb, uint32_t bx0, uint32_t x, uint32_t y0, uint32_t y1) {
    for (uint32_t gy = 84u; gy <= 174u; gy += 30u) {
        if (gy >= y0 && gy <= y1) {
            for (uint32_t px = x; px < x + 6u; px++) {
                if (((px - bx0) & 3u) == 0u) {
                    fb[gy * ADRIFT_SCREEN_W + px] = COL_SCOPE_GRID;
                }
            }
        }
    }
}

static void scroll_and_draw_waterfall(uint16_t *fb,
                                      const adrift_ui_t *ui,
                                      const adrift_group_theme_t *th,
                                      uint16_t fill_col,
                                      uint32_t wf_y0,
                                      uint32_t row_stride,
                                      uint32_t cell_h,
                                      adrift_dirty_t *dirty) {
    /* Scroll rows r = 22..0 down to r = 23..1 using aligned 32-bit word copies across x = 8..231 */
    for (int32_t r = (int32_t)ADRIFT_UI_WATERFALL_ROWS - 2; r >= 0; r--) {
        uint32_t ry_src = wf_y0 + (uint32_t)r * row_stride;
        uint32_t ry_dst = ry_src + row_stride;
        const uint32_t *src  = (const uint32_t *)(const void *)&fb[ry_src * ADRIFT_SCREEN_W + 8u];
        uint32_t       *dst0 = (uint32_t *)(void *)&fb[ry_dst * ADRIFT_SCREEN_W + 8u];
        uint32_t        diff = 0u;

        if (cell_h == 2u) {
            uint32_t *dst1 = (uint32_t *)(void *)&fb[(ry_dst + 1u) * ADRIFT_SCREEN_W + 8u];
            for (uint32_t w = 0; w < 112u; w++) {
                uint32_t v = src[w];
                diff |= (dst0[w] ^ v);
                dst0[w] = v;
                dst1[w] = v;
            }
        } else {
            uint32_t *dst1 = (uint32_t *)(void *)&fb[(ry_dst + 1u) * ADRIFT_SCREEN_W + 8u];
            uint32_t *dst2 = (uint32_t *)(void *)&fb[(ry_dst + 2u) * ADRIFT_SCREEN_W + 8u];
            uint32_t *dst3 = (uint32_t *)(void *)&fb[(ry_dst + 3u) * ADRIFT_SCREEN_W + 8u];
            for (uint32_t w = 0; w < 112u; w++) {
                uint32_t v = src[w];
                diff |= (dst0[w] ^ v);
                dst0[w] = v;
                dst1[w] = v;
                dst2[w] = v;
                dst3[w] = v;
            }
        }
        if (diff != 0u) {
            adrift_dirty_mark_rows(dirty, ry_dst, cell_h);
        }
    }

    /* Render newest waterfall row r = 0 into aligned row buffer and write to wf_y0 */
    uint16_t row_px[224] __attribute__((aligned(4)));
    for (uint32_t i = 0; i < 224u; i++) {
        row_px[i] = COL_SCOPE_BG;
    }
    uint8_t head = ui->spec_wf_head % ADRIFT_UI_WATERFALL_ROWS;
    for (uint32_t b = 0; b < ADRIFT_UI_SPEC_BINS; b++) {
        uint16_t col = waterfall_heat_col(ui->spec_waterfall[head][b], th, fill_col);
        if (col != COL_SCOPE_BG) {
            uint32_t off = 1u + b * 7u; /* x = 9 relative to x = 8 */
            row_px[off + 0u] = col;
            row_px[off + 1u] = col;
            row_px[off + 2u] = col;
            row_px[off + 3u] = col;
            row_px[off + 4u] = col;
            row_px[off + 5u] = col;
        }
    }

    const uint32_t *src0 = (const uint32_t *)(const void *)row_px;
    uint32_t       *top0 = (uint32_t *)(void *)&fb[wf_y0 * ADRIFT_SCREEN_W + 8u];
    uint32_t        diff0 = 0u;
    if (cell_h == 2u) {
        uint32_t *top1 = (uint32_t *)(void *)&fb[(wf_y0 + 1u) * ADRIFT_SCREEN_W + 8u];
        for (uint32_t w = 0; w < 112u; w++) {
            uint32_t v = src0[w];
            diff0 |= (top0[w] ^ v);
            top0[w] = v;
            top1[w] = v;
        }
    } else {
        uint32_t *top1 = (uint32_t *)(void *)&fb[(wf_y0 + 1u) * ADRIFT_SCREEN_W + 8u];
        uint32_t *top2 = (uint32_t *)(void *)&fb[(wf_y0 + 2u) * ADRIFT_SCREEN_W + 8u];
        uint32_t *top3 = (uint32_t *)(void *)&fb[(wf_y0 + 3u) * ADRIFT_SCREEN_W + 8u];
        for (uint32_t w = 0; w < 112u; w++) {
            uint32_t v = src0[w];
            diff0 |= (top0[w] ^ v);
            top0[w] = v;
            top1[w] = v;
            top2[w] = v;
            top3[w] = v;
        }
    }
    if (diff0 != 0u) {
        adrift_dirty_mark_rows(dirty, wf_y0, cell_h);
    }
}

static void draw_rta_bars_incremental(uint16_t *fb,
                                      adrift_ui_t *ui,
                                      const adrift_group_theme_t *th,
                                      uint16_t fill_col,
                                      uint32_t bx0,
                                      uint32_t by1,
                                      uint32_t bh_max,
                                      bool has_graticule,
                                      bool full_chrome,
                                      adrift_dirty_t *dirty) {
    for (uint32_t b = 0; b < ADRIFT_UI_SPEC_BINS; b++) {
        uint32_t x  = bx0 + b * 7u;
        uint32_t bh = div255_u32((uint32_t)ui->spec_bins[b]  * bh_max);
        uint32_t ph = div255_u32((uint32_t)ui->spec_peaks[b] * bh_max);

        if (full_chrome) {
            if (bh > 0u) {
                fb_fill_rect(fb, x, by1 - bh, 6u, bh, fill_col);
                fb_fill_rect(fb, x, by1 - bh, 6u, (bh >= 2u) ? 2u : 1u, th->accent);
            }
            if (ph > 1u) {
                fb_hline(fb, x, by1 - ph, 6u, COL_WHITE);
            }
            ui->spec_prev_bh[b] = (uint8_t)bh;
            ui->spec_prev_ph[b] = (uint8_t)ph;
            continue;
        }

        uint32_t prev_bh = ui->spec_prev_bh[b];
        uint32_t prev_ph = ui->spec_prev_ph[b];
        if (bh == prev_bh && ph == prev_ph) {
            continue;
        }

        if (ph != prev_ph && prev_ph > 1u) {
            uint32_t yp_old = by1 - prev_ph;
            if (bh > 0u && yp_old >= by1 - bh) {
                uint32_t cap_h = (bh >= 2u) ? 2u : 1u;
                uint16_t rcol  = (yp_old < by1 - bh + cap_h) ? th->accent : fill_col;
                fb_hline(fb, x, yp_old, 6u, rcol);
            } else {
                fb_hline(fb, x, yp_old, 6u, COL_SCOPE_BG);
                if (has_graticule) {
                    restore_bars_graticule(fb, bx0, x, yp_old, yp_old);
                }
            }
            adrift_dirty_mark_rows(dirty, yp_old, 1u);
        }

        if (bh != prev_bh) {
            uint32_t max_bh    = (bh > prev_bh) ? bh : prev_bh;
            uint32_t min_bh    = (bh < prev_bh) ? bh : prev_bh;
            uint32_t keep_body = (min_bh > 2u) ? (min_bh - 2u) : 0u;
            uint32_t y_top     = by1 - max_bh;
            uint32_t y_bot     = by1 - keep_body - 1u;

            if (bh < max_bh) {
                uint32_t clr_h = max_bh - bh;
                fb_fill_rect(fb, x, y_top, 6u, clr_h, COL_SCOPE_BG);
                if (has_graticule) {
                    restore_bars_graticule(fb, bx0, x, y_top, y_top + clr_h - 1u);
                }
            }
            if (bh > keep_body) {
                fb_fill_rect(fb, x, by1 - bh, 6u, bh - keep_body, fill_col);
                fb_fill_rect(fb, x, by1 - bh, 6u, (bh >= 2u) ? 2u : 1u, th->accent);
            }
            if (ph > 1u) {
                uint32_t yp_new = by1 - ph;
                if (yp_new >= y_top && yp_new <= y_bot) {
                    fb_hline(fb, x, yp_new, 6u, COL_WHITE);
                }
            }
            adrift_dirty_mark_rows(dirty, y_top, y_bot - y_top + 1u);
        }

        if (ph != prev_ph && ph > 1u) {
            uint32_t yp_new = by1 - ph;
            fb_hline(fb, x, yp_new, 6u, COL_WHITE);
            adrift_dirty_mark_rows(dirty, yp_new, 1u);
        }

        ui->spec_prev_bh[b] = (uint8_t)bh;
        ui->spec_prev_ph[b] = (uint8_t)ph;
    }
}

static void draw_home_spec_page(uint16_t *fb,
                                adrift_ui_t *ui,
                                const adrift_synth_t *s,
                                bool full_chrome,
                                bool pips_dirty,
                                adrift_dirty_t *dirty) {
    static const char *const k_spec_modes[3] = { "SPLT", "BARS", "WTRF" };
    const adrift_group_theme_t *th = get_group_theme(s->edit_group);
    const adrift_patch_t       *vp = adrift_synth_view_patch(s);
    uint8_t  mode     = ui->scope_mode[ADRIFT_HOME_SPEC] % ADRIFT_UI_NUM_HOME_MODES;
    uint16_t fill_col = make_env_fill_col(th->accent);

    update_home_spectrum(ui, s);

    char sub[16];
    fmt_hz(sub, dsp_cutoff_to_fc_q8(vp->in_lpf_cutoff) >> 8);
    update_home_chrome(fb, ui, s, "SPEC", sub, COL_WHITE, k_spec_modes[mode],
                       full_chrome, pips_dirty, dirty);

    if (!full_chrome && ui->scope_freeze) {
        return;
    }

    const uint32_t bx0 = 9u;
    if (mode == 0u) {
        /* MODE 0 (SPLT): Top 24x3px Waterfall (y=53..124) + Bottom 32-band RTA Bars (y=128..214) */
        const uint32_t wf_y0 = 53u;
        if (full_chrome) {
            for (uint32_t r = 0; r < ADRIFT_UI_WATERFALL_ROWS; r++) {
                uint8_t row_idx = (uint8_t)((ui->spec_wf_head + ADRIFT_UI_WATERFALL_ROWS - r) % ADRIFT_UI_WATERFALL_ROWS);
                uint32_t ry = wf_y0 + r * 3u;
                for (uint32_t b = 0; b < ADRIFT_UI_SPEC_BINS; b++) {
                    uint16_t col = waterfall_heat_col(ui->spec_waterfall[row_idx][b], th, fill_col);
                    if (col != COL_SCOPE_BG) {
                        fb_fill_rect(fb, bx0 + b * 7u, ry, 6u, 2u, col);
                    }
                }
            }
            fb_hline(fb, 6u, 126u, 228u, COL_SCOPE_GRID);
        } else {
            scroll_and_draw_waterfall(fb, ui, th, fill_col, wf_y0, 3u, 2u, dirty);
        }
        draw_rta_bars_incremental(fb, ui, th, fill_col, bx0, 214u, 84u, false, full_chrome, dirty);
    } else if (mode == 1u) {
        /* MODE 1 (BARS): Full-height 32-band RTA (y=54..204) + frequency axis labels */
        const uint32_t by0 = 54u, by1 = 204u, bh_max = by1 - by0;
        if (full_chrome) {
            for (uint32_t gy = by0 + 30u; gy < by1; gy += 30u) {
                for (uint32_t x = bx0; x < bx0 + 222u; x += 4u) {
                    fb[gy * ADRIFT_SCREEN_W + x] = COL_SCOPE_GRID;
                }
            }
            fb_draw_str_spec(fb, bx0,        207u, "170", COL_MUTED, &k_font_sm);
            fb_draw_str_spec(fb, bx0 + 56u,  207u, "1K",  COL_MUTED, &k_font_sm);
            fb_draw_str_spec(fb, bx0 + 112u, 207u, "2.5K",COL_MUTED, &k_font_sm);
            fb_draw_str_spec(fb, bx0 + 190u, 207u, "5.5K",COL_MUTED, &k_font_sm);
        }
        draw_rta_bars_incremental(fb, ui, th, fill_col, bx0, by1, bh_max, true, full_chrome, dirty);
    } else {
        /* MODE 2 (WTRF): Full 24x5px Waterfall (y=53..172) + Live Spectral Crest Curve (y=175..214) */
        const uint32_t wf_y0 = 53u;
        if (full_chrome) {
            for (uint32_t r = 0; r < ADRIFT_UI_WATERFALL_ROWS; r++) {
                uint8_t row_idx = (uint8_t)((ui->spec_wf_head + ADRIFT_UI_WATERFALL_ROWS - r) % ADRIFT_UI_WATERFALL_ROWS);
                uint32_t ry = wf_y0 + r * 5u;
                for (uint32_t b = 0; b < ADRIFT_UI_SPEC_BINS; b++) {
                    uint16_t col = waterfall_heat_col(ui->spec_waterfall[row_idx][b], th, fill_col);
                    if (col != COL_SCOPE_BG) {
                        fb_fill_rect(fb, bx0 + b * 7u, ry, 6u, 4u, col);
                    }
                }
            }
            fb_hline(fb, 6u, 174u, 228u, COL_SCOPE_GRID);
        } else {
            scroll_and_draw_waterfall(fb, ui, th, fill_col, wf_y0, 5u, 4u, dirty);
        }

        const uint32_t cy1 = 214u, ch_max = 38u;
        uint8_t  h_arr[ADRIFT_UI_SPEC_BINS];
        uint32_t cur_y0  = cy1;
        bool     changed = full_chrome;
        for (uint32_t b = 0; b < ADRIFT_UI_SPEC_BINS; b++) {
            uint8_t h = (uint8_t)div255_u32((uint32_t)ui->spec_bins[b] * ch_max);
            h_arr[b]  = h;
            uint32_t py = cy1 - h;
            if (py < cur_y0) cur_y0 = py;
            if (!changed && h != ui->spec_prev_crest[b]) changed = true;
        }

        if (changed) {
            if (!full_chrome) {
                uint32_t clr_y0 = (ui->spec_prev_crest_y0 < cur_y0) ? ui->spec_prev_crest_y0 : cur_y0;
                if (clr_y0 < 176u) clr_y0 = 176u;
                fb_fill_rect(fb, bx0, clr_y0, 223u, cy1 - clr_y0 + 1u, COL_SCOPE_BG);
                adrift_dirty_mark_rows(dirty, clr_y0, cy1 - clr_y0 + 1u);
            }

            uint32_t prev_y = cy1;
            for (uint32_t b = 0; b < ADRIFT_UI_SPEC_BINS; b++) {
                uint32_t py = cy1 - h_arr[b];
                uint32_t x0 = bx0 + b * 7u;
                if (x0 >= 232u) break;

                /* Column dx = 0: vertical step connector from prev_y to py */
                fb_fill_rect(fb, x0, py, 1u, cy1 - py + 1u, fill_col);
                uint32_t y_lo = (prev_y < py) ? prev_y : py;
                uint32_t y_hi = (prev_y < py) ? py : prev_y;
                fb_fill_rect(fb, x0, y_lo, 1u, y_hi - y_lo + 1u, th->accent);

                /* Columns dx = 1..6: flat horizontal plateau at py */
                uint32_t w_rem = (x0 + 7u <= 232u) ? 6u : (232u - x0 - 1u);
                if (w_rem > 0u) {
                    if (py < cy1) {
                        fb_fill_rect(fb, x0 + 1u, py + 1u, w_rem, cy1 - py, fill_col);
                    }
                    fb_hline(fb, x0 + 1u, py, w_rem, th->accent);
                }
                prev_y = py;
                ui->spec_prev_crest[b] = h_arr[b];
            }
            ui->spec_prev_crest_y0 = (uint8_t)cur_y0;
        }
    }
}

/* ----------------------------------------------------------------------------
 * Page 4/6: STAGE — 2D Stereo Soundstage & 16-Voice Constellation
 * ---------------------------------------------------------------------------- */
static void restore_stage_grid_band(uint16_t *fb,
                                    const adrift_group_theme_t *th,
                                    uint32_t y0,
                                    uint32_t y1) {
    const int32_t gx0 = 26, gy0 = 54, gw = 202, gh = 148;
    const int32_t gcx = gx0 + gw / 2;

    if (y0 > y1) return;
    fb_fill_rect(fb, 24u, y0, 206u, y1 - y0 + 1u, COL_SCOPE_BG);

    uint32_t by0 = (y0 > (uint32_t)gy0) ? y0 : (uint32_t)gy0;
    uint32_t by1 = (y1 < (uint32_t)(gy0 + gh - 1)) ? y1 : (uint32_t)(gy0 + gh - 1);
    if (by0 <= by1) {
        uint32_t bh = by1 - by0 + 1u;
        fb_fill_rect(fb, (uint32_t)gx0,            by0, 1u, bh, COL_SCOPE_GRID);
        fb_fill_rect(fb, (uint32_t)(gx0 + gw - 1), by0, 1u, bh, COL_SCOPE_GRID);
    }
    if (y0 <= (uint32_t)gy0 && y1 >= (uint32_t)gy0) {
        fb_hline(fb, (uint32_t)gx0, (uint32_t)gy0, (uint32_t)gw, COL_SCOPE_GRID);
    }
    if (y0 <= (uint32_t)(gy0 + gh - 1) && y1 >= (uint32_t)(gy0 + gh - 1)) {
        fb_hline(fb, (uint32_t)gx0, (uint32_t)(gy0 + gh - 1), (uint32_t)gw, COL_SCOPE_GRID);
    }

    for (int32_t y = gy0 + 2; y < gy0 + gh - 1; y += 3) {
        if ((uint32_t)y >= y0 && (uint32_t)y <= y1) {
            fb[(uint32_t)y * ADRIFT_SCREEN_W + (uint32_t)(gx0 + gw / 4)]       = COL_SCOPE_GRID;
            fb[(uint32_t)y * ADRIFT_SCREEN_W + (uint32_t)gcx]                  = th->border;
            fb[(uint32_t)y * ADRIFT_SCREEN_W + (uint32_t)(gx0 + (gw * 3) / 4)] = COL_SCOPE_GRID;
        }
    }
    for (uint32_t r = 1u; r <= 3u; r++) {
        uint32_t ry = (uint32_t)gy0 + ((uint32_t)gh * r) / 4u;
        if (ry >= y0 && ry <= y1) {
            for (int32_t x = gx0 + 2; x < gx0 + gw - 1; x += 4) {
                fb[ry * ADRIFT_SCREEN_W + (uint32_t)x] = COL_SCOPE_GRID;
            }
        }
    }
}

static bool stage_node_y_span(const adrift_stage_node_t *nd,
                              uint8_t mode,
                              uint32_t *out_y0,
                              uint32_t *out_y1) {
    if ((nd->flags & 1u) == 0u) return false;
    const int32_t gy0 = 54, gh = 148;
    int32_t y0, y1;
    if ((nd->flags & 2u) == 0u) {
        y0 = (int32_t)nd->py - 1;
        y1 = (int32_t)nd->py + 1;
    } else {
        int32_t rad = (int32_t)nd->rad_px;
        y0 = (int32_t)nd->py - rad - 1;
        y1 = (int32_t)nd->py + rad + 1;
        if (mode == 1u) {
            if ((int32_t)nd->g_py < y0) y0 = (int32_t)nd->g_py;
            if ((int32_t)nd->g_py > y1) y1 = (int32_t)nd->g_py;
        } else if (mode == 2u) {
            int32_t ly = dsp_clamp_i32((int32_t)nd->py - 3, gy0 + 2, gy0 + gh - 9);
            if (ly < y0) y0 = ly;
            if (ly + 6 > y1) y1 = ly + 6;
        }
    }
    if (y0 < 52)  y0 = 52;
    if (y1 > 203) y1 = 203;
    *out_y0 = (uint32_t)y0;
    *out_y1 = (uint32_t)y1;
    return true;
}

static bool stage_node_eq(const adrift_stage_node_t *a,
                          const adrift_stage_node_t *b,
                          uint8_t mode) {
    if ((a->flags & 1u) != (b->flags & 1u)) return false;
    if ((a->flags & 1u) == 0u) return true;
    if ((a->flags & 0x3Bu) != (b->flags & 0x3Bu)) return false;
    if ((a->flags & 2u) == 0u) {
        return a->px == b->px && a->py == b->py;
    }
    if (a->flags != b->flags || a->px != b->px || a->py != b->py ||
        a->wing_w != b->wing_w || a->rad_px != b->rad_px) {
        return false;
    }
    if (mode == 1u && (a->g_pan_x != b->g_pan_x || a->g_py != b->g_py)) return false;
    if (mode == 2u && a->note_disp != b->note_disp) return false;
    return true;
}

static void draw_stage_voice_node(uint16_t *fb,
                                  const adrift_stage_node_t *nd,
                                  uint8_t mode) {
    if ((nd->flags & 1u) == 0u) return;

    const int32_t gx0 = 26, gy0 = 54, gw = 202, gh = 148;
    uint8_t og = (uint8_t)((nd->flags >> 3) & 7u);
    const adrift_group_theme_t *vt = get_group_theme(og);
    int32_t px = (int32_t)nd->px;
    int32_t py = (int32_t)nd->py;

    if ((nd->flags & 2u) == 0u) {
        fb_fill_rect(fb, (uint32_t)(px - 1), (uint32_t)(py - 1), 3u, 3u, vt->border);
        return;
    }

    if (mode == 1u) {
        fb_draw_line_clipped(fb, (int32_t)nd->g_pan_x, (int32_t)nd->g_py, px, py,
                             gx0 + 1, gy0 + 1, gx0 + gw - 2, gy0 + gh - 2, vt->dim);
    }

    int32_t wing_w = (int32_t)nd->wing_w;
    int32_t rad_px = (int32_t)nd->rad_px;
    int32_t wx0    = dsp_clamp_i32(px - wing_w, gx0 + 2, gx0 + gw - 3);
    int32_t wx1    = dsp_clamp_i32(px + wing_w, gx0 + 2, gx0 + gw - 3);
    fb_fill_rect(fb, (uint32_t)wx0, (uint32_t)py, (uint32_t)(wx1 - wx0 + 1), 1u, vt->accent);
    fb_rect_outline(fb, (uint32_t)(px - rad_px - 1), (uint32_t)(py - rad_px - 1),
                    (uint32_t)(rad_px * 2 + 3), (uint32_t)(rad_px * 2 + 3), vt->dim);

    fb_fill_rect(fb, (uint32_t)(px - rad_px), (uint32_t)(py - rad_px),
                 (uint32_t)(rad_px * 2 + 1), (uint32_t)(rad_px * 2 + 1), vt->accent);
    if ((nd->flags & 4u) != 0u) {
        fb_fill_rect(fb, (uint32_t)(px - 1), (uint32_t)(py - 1), 3u, 3u, COL_WHITE);
    }

    if (mode == 2u) {
        char nbuf[8];
        fmt_note(nbuf, nd->note_disp);
        uint32_t lx = (px + rad_px + 22 < gx0 + gw) ? (uint32_t)(px + rad_px + 3) : (uint32_t)(px - rad_px - 19);
        uint32_t ly = (uint32_t)dsp_clamp_i32(py - 3, gy0 + 2, gy0 + gh - 9);
        fb_draw_str_spec(fb, lx, ly, nbuf, COL_WHITE, &k_font_sm);
    }
}

static void mask_mark_span(uint32_t mask[8], uint32_t y0, uint32_t y1) {
    for (uint32_t y = y0; y <= y1; y++) {
        mask[y >> 5] |= (1u << (y & 31u));
    }
}

static bool mask_hits_span(const uint32_t mask[8], uint32_t y0, uint32_t y1) {
    for (uint32_t y = y0; y <= y1; y++) {
        if ((mask[y >> 5] & (1u << (y & 31u))) != 0u) return true;
    }
    return false;
}

static void draw_home_stage_page(uint16_t *fb,
                                 adrift_ui_t *ui,
                                 const adrift_synth_t *s,
                                 bool full_chrome,
                                 bool pips_dirty,
                                 adrift_dirty_t *dirty) {
    static const char *const k_stage_modes[3] = { "FLD", "VEC", "HRM" };
    const adrift_group_theme_t *th = get_group_theme(s->edit_group);
    uint8_t mode = ui->scope_mode[ADRIFT_HOME_STAGE] % ADRIFT_UI_NUM_HOME_MODES;
    uint8_t zoom = ui->scope_zoom & 3u;
    int32_t gain = (int32_t)ui->scope_gain;

    /* Count active voices, precompute SUPER slot 0/1 energy & pitch centroid */
    uint8_t active_cnt = 0u;
    int32_t pitch_sum  = 0;
    int32_t super_env[ADRIFT_MAX_GROUPS][2];
    int32_t super_bbd[ADRIFT_MAX_GROUPS][2];
    int32_t spread_lut[ADRIFT_MAX_GROUPS];
    for (uint8_t g = 0; g < ADRIFT_MAX_GROUPS; g++) {
        super_env[g][0] = super_env[g][1] = 0;
        super_bbd[g][0] = super_bbd[g][1] = 0;
        spread_lut[g]   = adrift_detune_to_q8(s->groups[g].unison_detune);
    }
    for (uint8_t v = 0; v < ADRIFT_MAX_VOICES; v++) {
        const adrift_voice_t *vc = &s->voices[v];
        if (vc->owner_group >= 0 && vc->owner_group < ADRIFT_MAX_GROUPS) {
            uint8_t og = (uint8_t)vc->owner_group;
            if (vc->group_slot <= 1u) {
                super_env[og][vc->group_slot] = vc->env2.level_q30 >> 15;
                super_bbd[og][vc->group_slot] = vc->bbd.peak_wet_q15;
            }
            if (vc->active) {
                active_cnt++;
                int32_t pq8 = vc->bbd.pitch_initialized ? (vc->bbd.slew_pitch_q16 >> 8) : ((int32_t)vc->note << 8);
                pitch_sum += pq8;
            }
        }
    }

    char sub[12];
    fmt_int(sub, active_cnt, "/16V");
    update_home_chrome(fb, ui, s, "STAGE", sub, active_cnt > 0u ? th->accent : COL_MUTED,
                       k_stage_modes[mode], full_chrome, pips_dirty, dirty);

    const int32_t gx0 = 26, gy0 = 54, gw = 202, gh = 148;
    const int32_t gcx = gx0 + gw / 2;

    if (full_chrome) {
        restore_stage_grid_band(fb, th, 52u, 203u);
        fb_draw_str_spec(fb, 8u,  (uint32_t)(gy0 + 2),          "HI",  COL_MUTED,  &k_font_sm);
        fb_draw_str_spec(fb, 8u,  (uint32_t)(gy0 + gh / 2 - 3), "MD",  COL_MUTED,  &k_font_sm);
        fb_draw_str_spec(fb, 8u,  (uint32_t)(gy0 + gh - 8),     "LO",  COL_MUTED,  &k_font_sm);
        fb_draw_str_spec(fb, (uint32_t)gx0,                      205u, "L",   COL_MUTED,  &k_font_sm);
        fb_draw_str_spec(fb, (uint32_t)(gx0 + gw / 4 - 6),       205u, "L50", COL_MUTED,  &k_font_sm);
        fb_draw_str_spec(fb, (uint32_t)(gcx - 2),                205u, "C",   th->accent, &k_font_sm_bold);
        fb_draw_str_spec(fb, (uint32_t)(gx0 + (gw * 3) / 4 - 8), 205u, "R50", COL_MUTED,  &k_font_sm);
        fb_draw_str_spec(fb, (uint32_t)(gx0 + gw - 6),           205u, "R",   COL_MUTED,  &k_font_sm);
    }

    static const int32_t k_span_st[4] = { 72, 48, 24, 12 };
    int32_t span_q8   = k_span_st[zoom] << 8;
    int32_t center_q8 = (active_cnt > 0u) ? (pitch_sum / (int32_t)active_cnt) : (60 << 8);
    int32_t min_p_q8  = center_q8 - (span_q8 >> 1);

    adrift_stage_node_t cur_node[ADRIFT_MAX_VOICES];
    uint32_t cur_y0[ADRIFT_MAX_VOICES];
    uint32_t cur_y1[ADRIFT_MAX_VOICES];
    bool     cur_has[ADRIFT_MAX_VOICES];

    for (uint8_t v = 0; v < ADRIFT_MAX_VOICES; v++) {
        const adrift_voice_t *vc = &s->voices[v];
        adrift_stage_node_t nd = { 0, 0, 0, 0, 0u, 0u, 0u, 0u };

        if (vc->owner_group >= 0 && vc->owner_group < ADRIFT_MAX_GROUPS) {
            uint8_t og = (uint8_t)vc->owner_group;
            const adrift_group_t *gp = &s->groups[og];

            int32_t gl, gr;
            if (vc->active) {
                gl = vc->grp_gain_l_q15;
                gr = vc->grp_gain_r_q15;
            } else {
                int32_t pan_l = 32767, pan_r = 32767;
                adrift_synth_compute_voice_pan_gains_q15(gp, vc->group_slot, &pan_l, &pan_r);
                gl = pan_l;
                gr = pan_r;
            }
            int32_t gsum    = gl + gr;
            int32_t pan_q10 = (gsum > 16) ? (((gr - gl) * 1024) / gsum) : 0;
            int32_t px      = dsp_clamp_i32(gcx + ((pan_q10 * (gw - 16)) >> 11), gx0 + 5, gx0 + gw - 6);

            int32_t pq8;
            if (vc->active && vc->bbd.pitch_initialized &&
                (gp->assign_mode != ADRIFT_GROUP_SUPER || vc->group_slot <= 1u)) {
                pq8 = vc->bbd.slew_pitch_q16 >> 8;
            } else {
                int32_t semi_q8 = 0, det_q8 = 0, sgain = 32767;
                adrift_synth_compute_stack_slot_voicing(gp, vc->group_slot, spread_lut[og], &semi_q8, &det_q8, &sgain);
                int32_t nt = dsp_clamp_i32((int32_t)vc->note + (int32_t)gp->transpose, 0, 127);
                pq8 = (nt << 8) + semi_q8 + det_q8;
            }

            int32_t rel_p = pq8 - min_p_q8;
            int32_t py    = dsp_clamp_i32(gy0 + gh - 6 - ((rel_p * (gh - 12)) / span_q8), gy0 + 5, gy0 + gh - 6);

            uint8_t flags = (uint8_t)(1u | ((og & 7u) << 3));
            nd.px = (int16_t)px;
            nd.py = (int16_t)py;

            if (vc->active) {
                flags |= 2u;
                if (vc->key_down || vc->sustained || gp->patch.env2.drone_hold) {
                    flags |= 4u;
                }
                if (mode == 1u) {
                    int32_t g_pan_x = dsp_clamp_i32(gcx + ((gp->pan * (gw - 16)) >> 10), gx0 + 5, gx0 + gw - 6);
                    int32_t g_nt    = dsp_clamp_i32((int32_t)(gp->last_note ? gp->last_note : 60u) + gp->transpose, 0, 127);
                    int32_t g_py    = dsp_clamp_i32(gy0 + gh - 6 - ((((g_nt << 8) - min_p_q8) * (gh - 12)) / span_q8),
                                                    gy0 + 5, gy0 + gh - 6);
                    nd.g_pan_x = (int16_t)g_pan_x;
                    nd.g_py    = (int16_t)g_py;
                }
                int32_t env_q15 = vc->env2.level_q30 >> 15;
                int32_t bbd_q15 = vc->bbd.peak_wet_q15;
                if (gp->assign_mode == ADRIFT_GROUP_SUPER && vc->group_slot > 1u) {
                    env_q15 = super_env[og][vc->group_slot & 1u];
                    bbd_q15 = super_bbd[og][vc->group_slot & 1u];
                }
                nd.wing_w    = (uint8_t)dsp_clamp_i32(mul_gain_pct((bbd_q15 * 16) >> 15, gain), 3, 20);
                nd.rad_px    = (uint8_t)dsp_clamp_i32(2 + mul_gain_pct((env_q15 * 5) >> 15, gain), 2, 6);
                nd.note_disp = (uint8_t)dsp_clamp_i32((pq8 + 128) >> 8, 0, 127);
            }
            nd.flags = flags;
        }

        cur_node[v] = nd;
        cur_has[v]  = stage_node_y_span(&nd, mode, &cur_y0[v], &cur_y1[v]);
    }

    if (full_chrome) {
        for (uint8_t v = 0; v < ADRIFT_MAX_VOICES; v++) {
            draw_stage_voice_node(fb, &cur_node[v], mode);
            ui->stage_prev[v] = cur_node[v];
        }
        return;
    }

    uint32_t s_mask[8] = { 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
    bool     inc[ADRIFT_MAX_VOICES];
    bool     any_dirty = false;

    for (uint8_t v = 0; v < ADRIFT_MAX_VOICES; v++) {
        inc[v] = false;
        if (!stage_node_eq(&cur_node[v], &ui->stage_prev[v], mode)) {
            uint32_t oy0, oy1;
            if (stage_node_y_span(&ui->stage_prev[v], mode, &oy0, &oy1)) {
                mask_mark_span(s_mask, oy0, oy1);
                any_dirty = true;
            }
            if (cur_has[v]) {
                mask_mark_span(s_mask, cur_y0[v], cur_y1[v]);
                inc[v]    = true;
                any_dirty = true;
            }
        }
    }

    if (!any_dirty) {
        for (uint8_t v = 0; v < ADRIFT_MAX_VOICES; v++) {
            ui->stage_prev[v] = cur_node[v];
        }
        return;
    }

    /* Transitive Y-overlap closure so every redrawn voice lies 100% inside s_mask */
    bool expanded = true;
    while (expanded) {
        expanded = false;
        for (uint8_t v = 0; v < ADRIFT_MAX_VOICES; v++) {
            if (cur_has[v] && !inc[v] && mask_hits_span(s_mask, cur_y0[v], cur_y1[v])) {
                mask_mark_span(s_mask, cur_y0[v], cur_y1[v]);
                inc[v]   = true;
                expanded = true;
            }
        }
    }

    uint32_t y = 52u;
    while (y <= 203u) {
        if ((s_mask[y >> 5] & (1u << (y & 31u))) == 0u) {
            y++;
            continue;
        }
        uint32_t by0 = y;
        while (y <= 203u && (s_mask[y >> 5] & (1u << (y & 31u))) != 0u) {
            y++;
        }
        uint32_t by1 = y - 1u;
        restore_stage_grid_band(fb, th, by0, by1);
        adrift_dirty_mark_rows(dirty, by0, by1 - by0 + 1u);
    }

    for (uint8_t v = 0; v < ADRIFT_MAX_VOICES; v++) {
        if (inc[v]) {
            draw_stage_voice_node(fb, &cur_node[v], mode);
        }
        ui->stage_prev[v] = cur_node[v];
    }
}

/* ----------------------------------------------------------------------------
 * Page 5/6: POLY — 16-Voice Telemetry Bridge
 * ---------------------------------------------------------------------------- */
static void fill_clipped_vbar(uint16_t *fb,
                              uint32_t x,
                              uint32_t w,
                              uint32_t bar_y0,
                              uint32_t bar_y1,
                              uint32_t sy0,
                              uint32_t sy1,
                              uint16_t col) {
    uint32_t iy0 = (bar_y0 > sy0) ? bar_y0 : sy0;
    uint32_t iy1 = (bar_y1 < sy1) ? bar_y1 : sy1;
    if (iy0 <= iy1) {
        fb_fill_rect(fb, x, iy0, w, iy1 - iy0 + 1u, col);
    }
}

static void draw_poly_col_slice(uint16_t *fb,
                                uint32_t cx,
                                uint32_t cw,
                                uint8_t mode,
                                bool assigned,
                                uint16_t border_col,
                                const adrift_group_theme_t *vt,
                                uint32_t h_e1,
                                uint32_t h_bbd,
                                uint32_t h_e2,
                                uint32_t pk,
                                uint32_t fb_y,
                                uint32_t sy0,
                                uint32_t sy1) {
    if (sy0 > sy1) return;
    uint32_t card_y1 = (sy1 < 192u) ? sy1 : 192u;
    if (sy0 <= card_y1) {
        fb_fill_rect(fb, cx + 1u, sy0, cw - 2u, card_y1 - sy0 + 1u, COL_CARD);
    }
    if (sy1 >= 193u) {
        fb_hline(fb, cx + 1u, 193u, cw - 2u, border_col);
    }
    if (!assigned) return;

    if (mode == 0u) {
        if (h_e1 > 0u)  fill_clipped_vbar(fb, cx + 1u, 3u, 193u - h_e1,  192u, sy0, sy1, vt->dim);
        if (h_bbd > 0u) fill_clipped_vbar(fb, cx + 5u, 3u, 193u - h_bbd, 192u, sy0, sy1, vt->accent);
        if (h_e2 > 0u)  fill_clipped_vbar(fb, cx + 9u, 3u, 193u - h_e2,  192u, sy0, sy1, COL_WHITE);
        if (pk > 1u && (193u - pk) >= sy0 && (193u - pk) <= sy1) {
            fb_hline(fb, cx + 1u, 193u - pk, cw - 2u, COL_WHITE);
        }
    } else if (mode == 1u) {
        if (h_bbd > 0u) {
            uint16_t fcol  = make_env_fill_col(vt->accent);
            uint32_t cap_h = (h_bbd >= 2u) ? 2u : 1u;
            fill_clipped_vbar(fb, cx + 1u, cw - 2u, 193u - h_bbd, 192u, sy0, sy1, fcol);
            fill_clipped_vbar(fb, cx + 1u, cw - 2u, 193u - h_bbd, 193u - h_bbd + cap_h - 1u, sy0, sy1, vt->accent);
        }
        if (fb_y >= sy0 && fb_y <= sy1) {
            fb_hline(fb, cx + 1u, fb_y, cw - 2u, vt->dim);
        }
        if (pk > 1u && (193u - pk) >= sy0 && (193u - pk) <= sy1) {
            fb_hline(fb, cx + 1u, 193u - pk, cw - 2u, COL_WHITE);
        }
    } else {
        const uint32_t mid = 129u;
        if (mid >= sy0 && mid <= sy1) {
            fb_hline(fb, cx + 1u, mid, cw - 2u, COL_SCOPE_GRID);
        }
        uint32_t u_e1 = h_e1 >> 1;
        uint32_t d_e2 = h_e2 >> 1;
        if (u_e1 > 0u) fill_clipped_vbar(fb, cx + 2u, cw - 4u, mid - u_e1, mid - 1u,   sy0, sy1, vt->accent);
        if (d_e2 > 0u) fill_clipped_vbar(fb, cx + 2u, cw - 4u, mid + 1u,   mid + d_e2, sy0, sy1, COL_WHITE);
    }
}

static void span_union_inc(uint32_t *sy0, uint32_t *sy1, uint32_t a, uint32_t b) {
    if (a < *sy0) *sy0 = a;
    if (b > *sy1) *sy1 = b;
}

static void draw_home_poly16_page(uint16_t *fb,
                                  adrift_ui_t *ui,
                                  const adrift_synth_t *s,
                                  bool full_chrome,
                                  bool pips_dirty,
                                  adrift_dirty_t *dirty) {
    static const char *const k_poly_modes[3] = { "TRI", "BBD", "ENV" };
    uint8_t eg = s->edit_group % ADRIFT_MAX_GROUPS;
    const adrift_group_theme_t *th = get_group_theme(eg);
    uint8_t mode  = ui->scope_mode[ADRIFT_HOME_POLY16] % ADRIFT_UI_NUM_HOME_MODES;
    uint8_t zoom  = ui->scope_zoom & 3u;
    int32_t gain  = (int32_t)ui->scope_gain;
    uint8_t decay = (uint8_t)(1u + zoom * 2u);

    uint8_t active_cnt = 0u;
    int32_t super_e1[ADRIFT_MAX_GROUPS][2];
    int32_t super_bbd[ADRIFT_MAX_GROUPS][2];
    int32_t super_e2[ADRIFT_MAX_GROUPS][2];
    for (uint8_t g = 0; g < ADRIFT_MAX_GROUPS; g++) {
        super_e1[g][0]  = super_e1[g][1]  = 0;
        super_bbd[g][0] = super_bbd[g][1] = 0;
        super_e2[g][0]  = super_e2[g][1]  = 0;
    }
    for (uint8_t v = 0; v < ADRIFT_MAX_VOICES; v++) {
        const adrift_voice_t *vc = &s->voices[v];
        if (vc->owner_group >= 0 && vc->owner_group < ADRIFT_MAX_GROUPS) {
            uint8_t og = (uint8_t)vc->owner_group;
            if (vc->group_slot <= 1u) {
                super_e1[og][vc->group_slot]  = vc->env1.level_q30 >> 15;
                super_bbd[og][vc->group_slot] = vc->bbd.peak_wet_q15;
                super_e2[og][vc->group_slot]  = vc->env2.level_q30 >> 15;
            }
            if (vc->active) active_cnt++;
        }
    }

    char sub[12];
    fmt_int(sub, active_cnt, " ACTV");
    update_home_chrome(fb, ui, s, "POLY", sub, active_cnt > 0u ? th->accent : COL_MUTED,
                       k_poly_modes[mode], full_chrome, pips_dirty, dirty);

    const uint32_t x0  = 9u;
    const uint32_t cw  = 13u;
    const uint32_t my0 = 64u, my1 = 194u, mh = my1 - my0;

    for (uint8_t v = 0; v < ADRIFT_MAX_VOICES; v++) {
        const adrift_voice_t *vc = &s->voices[v];
        uint32_t cx       = x0 + (uint32_t)v * 14u;
        bool     assigned = (vc->owner_group >= 0 && vc->owner_group < ADRIFT_MAX_GROUPS);
        uint8_t  og       = assigned ? (uint8_t)vc->owner_group : 0u;
        const adrift_group_theme_t *vt = get_group_theme(og);
        const adrift_group_t       *gp = &s->groups[og];

        uint8_t hdr_sig = (uint8_t)((assigned ? 1u : 0u) |
                                    ((vc->active ? 1u : 0u) << 1) |
                                    ((og & 7u) << 2) |
                                    (((og == eg) ? 1u : 0u) << 5));
        uint16_t border_col = !assigned ? COL_BAR_TRACK : (vc->active ? vt->border : COL_SCOPE_GRID);

        int32_t e1_q15  = vc->active ? (vc->env1.level_q30 >> 15) : 0;
        int32_t bbd_q15 = vc->active ? vc->bbd.peak_wet_q15       : 0;
        int32_t e2_q15  = vc->active ? (vc->env2.level_q30 >> 15) : 0;
        if (assigned && gp->assign_mode == ADRIFT_GROUP_SUPER && vc->group_slot > 1u && vc->active) {
            uint8_t sl = vc->group_slot & 1u;
            e1_q15  = super_e1[og][sl];
            bbd_q15 = super_bbd[og][sl];
            e2_q15  = super_e2[og][sl];
        }

        uint32_t h_e1   = (uint32_t)dsp_clamp_i32(mul_gain_pct((e1_q15  * (int32_t)(mh - 2u)) >> 15, gain), 0, (int32_t)(mh - 2u));
        uint32_t h_bbd  = (uint32_t)dsp_clamp_i32(mul_gain_pct((bbd_q15 * (int32_t)(mh - 2u)) >> 15, gain), 0, (int32_t)(mh - 2u));
        uint32_t h_e2   = (uint32_t)dsp_clamp_i32(mul_gain_pct((e2_q15  * (int32_t)(mh - 2u)) >> 15, gain), 0, (int32_t)(mh - 2u));
        uint32_t pk_cur = (h_bbd > h_e2) ? h_bbd : h_e2;

        if (!ui->scope_freeze) {
            if (pk_cur >= ui->poly_peaks[v]) {
                ui->poly_peaks[v] = (uint8_t)pk_cur;
            } else if (ui->poly_peaks[v] > decay) {
                ui->poly_peaks[v] = (uint8_t)(ui->poly_peaks[v] - decay);
            } else {
                ui->poly_peaks[v] = 0u;
            }
        }
        uint32_t pk   = ui->poly_peaks[v];
        uint32_t fb_y = my1 - 1u - (((uint32_t)dsp_clamp_i32(gp->patch.bbd.feedback, 0, 1023) * (mh - 4u)) / 1023u);

        uint8_t gstate = 0u;
        if (assigned) {
            if (vc->key_down || (vc->active && gp->patch.env2.drone_hold)) gstate = 3u;
            else if (vc->sustained) gstate = 2u;
            else if (vc->active)    gstate = 1u;
        }
        bool     show_note = (assigned && vc->active);
        uint8_t  n12       = show_note ? (uint8_t)(vc->note % 12u) : 0u;
        uint16_t foot_sig  = (uint16_t)(gstate |
                                        ((show_note ? 1u : 0u) << 2) |
                                        ((uint16_t)(n12 & 0xFu) << 3) |
                                        (((show_note && vc->key_down) ? 1u : 0u) << 7) |
                                        ((uint16_t)(og & 7u) << 8));

        if (full_chrome || hdr_sig != ui->poly_prev_hdr[v]) {
            if (!full_chrome) {
                fb_fill_rect(fb, cx + 4u, 53u, 5u, 7u, COL_SCOPE_BG);
            }
            char vch = (v < 9u) ? (char)('1' + v) : (char)('A' + (v - 9u));
            fb_draw_char_spec(fb, cx + 4u, 53u, vch,
                              !assigned ? COL_BAR_TRACK : (vc->active ? vt->accent : COL_MUTED),
                              &k_font_sm);
            fb_fill_rect(fb, cx, 61u, cw, 2u,
                         !assigned ? COL_BAR_TRACK : ((og == eg) ? COL_WHITE : vt->accent));
            fb_rect_outline(fb, cx, my0, cw, mh, border_col);
            draw_poly_col_slice(fb, cx, cw, mode, assigned, border_col, vt,
                                h_e1, h_bbd, h_e2, pk, fb_y, 65u, 193u);
            if (!full_chrome) {
                adrift_dirty_mark_rows(dirty, 53u, 10u);
                adrift_dirty_mark_rows(dirty, my0, mh);
            }
        } else if (assigned) {
            uint32_t sy0   = 200u;
            uint32_t sy1   = 60u;
            uint32_t pe1   = ui->poly_prev_e1[v];
            uint32_t pbbd  = ui->poly_prev_bbd[v];
            uint32_t pe2   = ui->poly_prev_e2[v];
            uint32_t ppk   = ui->poly_prev_pk[v];
            uint32_t pfby  = ui->poly_prev_fby[v];

            if (mode == 0u) {
                if (h_e1 != pe1) {
                    uint32_t hi = (h_e1 > pe1) ? h_e1 : pe1;
                    uint32_t lo = (h_e1 < pe1) ? h_e1 : pe1;
                    span_union_inc(&sy0, &sy1, 193u - hi, 192u - lo);
                }
                if (h_bbd != pbbd) {
                    uint32_t hi = (h_bbd > pbbd) ? h_bbd : pbbd;
                    uint32_t lo = (h_bbd < pbbd) ? h_bbd : pbbd;
                    span_union_inc(&sy0, &sy1, 193u - hi, 192u - lo);
                }
                if (h_e2 != pe2) {
                    uint32_t hi = (h_e2 > pe2) ? h_e2 : pe2;
                    uint32_t lo = (h_e2 < pe2) ? h_e2 : pe2;
                    span_union_inc(&sy0, &sy1, 193u - hi, 192u - lo);
                }
                if (pk != ppk) {
                    if (ppk > 1u) span_union_inc(&sy0, &sy1, 193u - ppk, 193u - ppk);
                    if (pk  > 1u) span_union_inc(&sy0, &sy1, 193u - pk,  193u - pk);
                }
            } else if (mode == 1u) {
                if (h_bbd != pbbd) {
                    uint32_t hi   = (h_bbd > pbbd) ? h_bbd : pbbd;
                    uint32_t lo   = (h_bbd < pbbd) ? h_bbd : pbbd;
                    uint32_t keep = (lo > 2u) ? (lo - 2u) : 0u;
                    span_union_inc(&sy0, &sy1, 193u - hi, 192u - keep);
                }
                if (fb_y != pfby) {
                    span_union_inc(&sy0, &sy1, pfby, pfby);
                    span_union_inc(&sy0, &sy1, fb_y, fb_y);
                }
                if (pk != ppk) {
                    if (ppk > 1u) span_union_inc(&sy0, &sy1, 193u - ppk, 193u - ppk);
                    if (pk  > 1u) span_union_inc(&sy0, &sy1, 193u - pk,  193u - pk);
                }
            } else {
                uint32_t u_cur = h_e1 >> 1, u_prv = pe1 >> 1;
                if (u_cur != u_prv) {
                    uint32_t hi = (u_cur > u_prv) ? u_cur : u_prv;
                    uint32_t lo = (u_cur < u_prv) ? u_cur : u_prv;
                    span_union_inc(&sy0, &sy1, 129u - hi, 128u - lo);
                }
                uint32_t d_cur = h_e2 >> 1, d_prv = pe2 >> 1;
                if (d_cur != d_prv) {
                    uint32_t hi = (d_cur > d_prv) ? d_cur : d_prv;
                    uint32_t lo = (d_cur < d_prv) ? d_cur : d_prv;
                    span_union_inc(&sy0, &sy1, 130u + lo, 129u + hi);
                }
            }

            if (sy0 <= sy1) {
                if (sy0 < 65u)  sy0 = 65u;
                if (sy1 > 193u) sy1 = 193u;
                draw_poly_col_slice(fb, cx, cw, mode, assigned, border_col, vt,
                                    h_e1, h_bbd, h_e2, pk, fb_y, sy0, sy1);
                adrift_dirty_mark_rows(dirty, sy0, sy1 - sy0 + 1u);
            }
        }

        if (full_chrome || foot_sig != ui->poly_prev_foot[v]) {
            uint16_t gcol = (gstate == 3u) ? COL_WHITE
                          : ((gstate == 2u) ? vt->accent
                          : ((gstate == 1u) ? vt->dim : COL_BAR_TRACK));
            fb_fill_rect(fb, cx, 196u, cw, 3u, gcol);
            if (!full_chrome) {
                fb_fill_rect(fb, cx, 202u, cw, 7u, COL_SCOPE_BG);
            }
            if (show_note) {
                static const char    k_note_let[12] = { 'C','C','D','D','E','F','F','G','G','A','A','B' };
                static const uint8_t k_sharp[12]    = { 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0 };
                fb_draw_char_spec(fb, cx + (k_sharp[n12] ? 1u : 4u), 202u, k_note_let[n12],
                                  vc->key_down ? COL_WHITE : vt->accent, &k_font_sm);
                if (k_sharp[n12]) {
                    fb_draw_char_spec(fb, cx + 7u, 202u, '#', vt->accent, &k_font_sm);
                }
            }
            if (!full_chrome) {
                adrift_dirty_mark_rows(dirty, 196u, 3u);
                adrift_dirty_mark_rows(dirty, 202u, 7u);
            }
        }

        ui->poly_prev_e1[v]   = (uint8_t)h_e1;
        ui->poly_prev_bbd[v]  = (uint8_t)h_bbd;
        ui->poly_prev_e2[v]   = (uint8_t)h_e2;
        ui->poly_prev_pk[v]   = (uint8_t)pk;
        ui->poly_prev_fby[v]  = (uint8_t)fb_y;
        ui->poly_prev_hdr[v]  = hdr_sig;
        ui->poly_prev_foot[v] = foot_sig;
    }
}

/* ----------------------------------------------------------------------------
 * Page 6/6: PERF — 6-Group Performance, Mixer & Multi-Arpeggiator Dashboard
 * ---------------------------------------------------------------------------- */
static void draw_home_perf_page(uint16_t *fb,
                                adrift_ui_t *ui,
                                const adrift_synth_t *s,
                                bool full_chrome,
                                bool pips_dirty,
                                adrift_dirty_t *dirty) {
    static const char *const k_perf_modes[3] = { "DASH", "ARP", "MIX" };
    uint8_t eg = s->edit_group % ADRIFT_MAX_GROUPS;
    const adrift_group_theme_t *th = get_group_theme(eg);
    uint8_t mode  = ui->scope_mode[ADRIFT_HOME_PERF] % ADRIFT_UI_NUM_HOME_MODES;
    uint8_t zoom  = ui->scope_zoom & 3u;
    int32_t gain  = (int32_t)ui->scope_gain;
    uint8_t decay = (uint8_t)(1u + zoom * 2u);

    char sub[16];
    if (s->midi_clk_active) {
        sub[0] = 'E'; sub[1] = 'X'; sub[2] = 'T'; sub[3] = ' ';
        fmt_int(&sub[4], dsp_clamp_i32((int32_t)s->midi_clk_bpm, 30, 300), "");
    } else {
        fmt_int(sub, dsp_clamp_i32(adrift_synth_active_group(s)->patch.arp.rate_bpm, 40, 240), "BPM");
    }
    update_home_chrome(fb, ui, s, "PERF", sub, s->midi_clk_active ? th->accent : COL_WHITE,
                       k_perf_modes[mode], full_chrome, pips_dirty, dirty);

    /* Single pass over all 16 voices to aggregate per-group stereo & mono peak levels */
    int32_t grp_pk_l[ADRIFT_MAX_GROUPS] = { 0, 0, 0, 0, 0, 0 };
    int32_t grp_pk_r[ADRIFT_MAX_GROUPS] = { 0, 0, 0, 0, 0, 0 };
    int32_t grp_pk_m[ADRIFT_MAX_GROUPS] = { 0, 0, 0, 0, 0, 0 };
    bool    grp_gate[ADRIFT_MAX_GROUPS] = { false, false, false, false, false, false };

    for (uint8_t v = 0; v < ADRIFT_MAX_VOICES; v++) {
        const adrift_voice_t *vc = &s->voices[v];
        if (vc->active && vc->owner_group >= 0 && vc->owner_group < ADRIFT_MAX_GROUPS) {
            uint8_t g = (uint8_t)vc->owner_group;
            if (vc->key_down || vc->sustained || s->groups[g].patch.env2.drone_hold) {
                grp_gate[g] = true;
            }
            int32_t e2  = vc->env2.level_q30 >> 15;
            int32_t bbd = vc->bbd.peak_wet_q15;
            int32_t lvl = (bbd > e2) ? ((bbd + e2) >> 1) : e2;
            int32_t vl  = dsp_mul_q15(lvl, vc->grp_gain_l_q15);
            int32_t vr  = dsp_mul_q15(lvl, vc->grp_gain_r_q15);
            if (vl  > grp_pk_l[g]) grp_pk_l[g] = vl;
            if (vr  > grp_pk_r[g]) grp_pk_r[g] = vr;
            if (lvl > grp_pk_m[g]) grp_pk_m[g] = lvl;
        }
    }

    /* 6 Horizontal Voice-Group Lanes (y = 53 .. 184, 22 px per group) */
    for (uint8_t g = 0; g < ADRIFT_MAX_GROUPS; g++) {
        const adrift_group_t       *gp = &s->groups[g];
        const adrift_group_theme_t *gt = get_group_theme(g);
        uint32_t ly      = 53u + (uint32_t)g * 22u;
        bool     enabled = (gp->num_voices > 0u);
        bool     is_sel  = (g == eg);
        uint16_t lane_bg = enabled ? COL_CARD : COL_SCOPE_BG;

        uint32_t mw_max = 136u;
        uint32_t mw_cur = (uint32_t)dsp_clamp_i32(mul_gain_pct((grp_pk_m[g] * (int32_t)mw_max) >> 15, gain), 0, (int32_t)mw_max);
        if (!ui->scope_freeze) {
            if (mw_cur >= ui->grp_peaks[g]) {
                ui->grp_peaks[g] = (uint8_t)mw_cur;
            } else if (ui->grp_peaks[g] > decay) {
                ui->grp_peaks[g] = (uint8_t)(ui->grp_peaks[g] - decay);
            } else {
                ui->grp_peaks[g] = 0u;
            }
        }
        uint8_t pk_val = ui->grp_peaks[g];

        uint8_t  disp_note = gp->last_note ? gp->last_note : 60u;
        uint8_t  asg_m     = (uint8_t)(gp->assign_mode & 3u);
        uint16_t hdr_sig   = (uint16_t)((gp->num_voices & 0x1Fu) | ((is_sel ? 1u : 0u) << 5));
        uint16_t note_sig  = (uint16_t)((disp_note & 0x7Fu) |
                                        ((grp_gate[g] ? 1u : 0u) << 7) |
                                        ((uint16_t)asg_m << 8) |
                                        ((enabled ? 1u : 0u) << 10));

        uint8_t  arp_m   = gp->patch.arp.mode % ADRIFT_ARP_MODE_COUNT;
        bool     arp_on  = (arp_m != ADRIFT_ARP_OFF);
        uint32_t act_st  = (gp->arp_state.num_notes > 0u && gp->arp_state.step_counter > 0u)
                               ? ((gp->arp_state.step_counter - 1u) & 7u)
                               : 8u;
        uint16_t arp_sig = (uint16_t)((enabled ? 1u : 0u) |
                                      ((arp_on ? 1u : 0u) << 1) |
                                      ((uint16_t)(arp_on ? act_st : 8u) << 2));

        uint32_t wl      = (uint32_t)dsp_clamp_i32(mul_gain_pct((grp_pk_l[g] * 94) >> 15, gain), 0, 94);
        uint32_t wr      = (uint32_t)dsp_clamp_i32(mul_gain_pct((grp_pk_r[g] * 94) >> 15, gain), 0, 94);
        uint8_t  vol_pct = (uint8_t)((dsp_clamp_i32(gp->patch.master_vol, 0, 1023) * 100) / 1023);

        bool full_lane = full_chrome || (hdr_sig != ui->perf_prev_hdr[g]);
        if (full_lane) {
            fb_fill_rect(fb, 9u, ly, 222u, 20u, lane_bg);
            fb_rect_outline(fb, 9u, ly, 222u, 20u, is_sel ? COL_WHITE : (enabled ? gt->border : COL_BAR_TRACK));

            char gtxt[8];
            gtxt[0] = 'G';
            gtxt[1] = (char)('1' + g);
            gtxt[2] = ':';
            fmt_int(&gtxt[3], gp->num_voices, "");
            if (is_sel) {
                fb_fill_rect(fb, 11u, ly + 3u, 32u, 14u, gt->accent);
                fb_draw_str_centered_spec(fb, 11u, 32u, ly + 6u, gtxt, COL_BG, &k_font_sm_bold);
            } else {
                fb_fill_rect(fb, 11u, ly + 3u, 32u, 14u, COL_TAB_OFF);
                fb_draw_str_centered_spec(fb, 11u, 32u, ly + 6u, gtxt, enabled ? gt->accent : COL_MUTED, &k_font_sm_bold);
            }
            if (!full_chrome) {
                adrift_dirty_mark_rows(dirty, ly, 20u);
            }
        }

        if (full_lane || note_sig != ui->perf_prev_note[g]) {
            if (!full_lane) {
                fb_fill_rect(fb, 46u, ly + 6u, 36u, 7u, lane_bg);
                adrift_dirty_mark_rows(dirty, ly + 6u, 7u);
            }
            char nbuf[8];
            fmt_note(nbuf, disp_note);
            fb_draw_str_spec(fb, 46u, ly + 6u, nbuf,
                             grp_gate[g] ? gt->accent : (enabled ? COL_WHITE : COL_MUTED), &k_font_sm_bold);
            char mch = (gp->assign_mode == ADRIFT_GROUP_SUPER)  ? 'S'
                     : (gp->assign_mode == ADRIFT_GROUP_UNISON) ? 'U' : 'R';
            fb_draw_char_spec(fb, 76u, ly + 6u, mch, enabled ? COL_MUTED : COL_BAR_TRACK, &k_font_sm);
        }

        const uint32_t rx0 = 88u;
        if (mode == 0u) {
            if (full_lane || (uint8_t)mw_cur != ui->perf_prev_mw[g] || pk_val != ui->perf_prev_pk[g]) {
                fb_fill_rect(fb, rx0, ly + 3u, mw_max, 5u, COL_BAR_TRACK);
                if (mw_cur > 0u) {
                    fb_fill_rect(fb, rx0, ly + 3u, mw_cur, 5u, gt->accent);
                }
                if (pk_val > 1u) {
                    uint32_t pk_x = (pk_val + 2u > mw_max) ? (mw_max - 2u) : pk_val;
                    fb_fill_rect(fb, rx0 + pk_x, ly + 3u, 2u, 5u, COL_WHITE);
                }
                if (!full_lane) {
                    adrift_dirty_mark_rows(dirty, ly + 3u, 5u);
                }
            }
            if (full_lane || arp_sig != ui->perf_prev_arp[g]) {
                for (uint32_t st = 0; st < 8u; st++) {
                    uint32_t sx = rx0 + st * 17u;
                    bool is_head = (arp_on && st == act_st);
                    uint16_t scol = !enabled ? COL_BAR_TRACK
                                  : (is_head ? COL_WHITE : (arp_on ? gt->dim : COL_SCOPE_GRID));
                    fb_fill_rect(fb, sx, ly + 11u, 15u, 5u, scol);
                }
                if (!full_lane) {
                    adrift_dirty_mark_rows(dirty, ly + 11u, 5u);
                }
            }
        } else if (mode == 1u) {
            if (full_lane || arp_sig != ui->perf_prev_arp[g]) {
                if (!full_lane) {
                    fb_fill_rect(fb, rx0, ly + 3u, 134u, 14u, lane_bg);
                    adrift_dirty_mark_rows(dirty, ly + 3u, 14u);
                }
                for (uint32_t st = 0; st < 8u; st++) {
                    uint32_t sx = rx0 + st * 17u;
                    uint32_t bh = !arp_on ? 4u : (4u + ((st * 3u + g) & 7u) + 3u);
                    if (bh > 14u) bh = 14u;
                    bool is_head = (arp_on && st == act_st);
                    fb_fill_rect(fb, sx, ly + 17u - bh, 15u, bh,
                                 is_head ? COL_WHITE : (enabled && arp_on ? gt->accent : COL_BAR_TRACK));
                }
            }
        } else {
            if (full_lane || (uint8_t)wl != ui->perf_prev_wl[g]) {
                fb_fill_rect(fb, rx0, ly + 4u, 94u, 5u, COL_BAR_TRACK);
                if (wl > 0u) fb_fill_rect(fb, rx0, ly + 4u, wl, 5u, gt->accent);
                if (!full_lane) {
                    adrift_dirty_mark_rows(dirty, ly + 4u, 5u);
                }
            }
            if (full_lane || (uint8_t)wr != ui->perf_prev_wr[g]) {
                fb_fill_rect(fb, rx0, ly + 11u, 94u, 5u, COL_BAR_TRACK);
                if (wr > 0u) fb_fill_rect(fb, rx0, ly + 11u, wr, 5u, COL_WHITE);
                if (!full_lane) {
                    adrift_dirty_mark_rows(dirty, ly + 11u, 5u);
                }
            }
            if (full_lane || vol_pct != ui->perf_prev_vol[g]) {
                if (!full_lane) {
                    fb_fill_rect(fb, rx0 + 98u, ly + 6u, 30u, 7u, lane_bg);
                    adrift_dirty_mark_rows(dirty, ly + 6u, 7u);
                }
                char vbuf[8];
                fmt_int(vbuf, vol_pct, "%");
                fb_draw_str_spec(fb, rx0 + 98u, ly + 6u, vbuf, enabled ? COL_WHITE : COL_MUTED, &k_font_sm_bold);
            }
        }

        ui->perf_prev_hdr[g]  = hdr_sig;
        ui->perf_prev_note[g] = note_sig;
        ui->perf_prev_mw[g]   = (uint8_t)mw_cur;
        ui->perf_prev_pk[g]   = pk_val;
        ui->perf_prev_arp[g]  = arp_sig;
        ui->perf_prev_wl[g]   = (uint8_t)wl;
        ui->perf_prev_wr[g]   = (uint8_t)wr;
        ui->perf_prev_vol[g]  = vol_pct;
    }

    /* Bottom Master Stereo FX Telemetry Strip (CHR, DLY, RVB, CMP at y = 187..214) */
    const adrift_fx_params_t *fp = &s->fx_params;
    const uint32_t fy = 187u;
    static const char *const k_fx_lbl[4] = { "CHR", "DLY", "RVB", "CMP" };
    int32_t fx_mix[4] = { fp->chorus_mix, fp->delay_mix, fp->reverb_mix, fp->comp_mix };
    int32_t fx_env[4] = {
        (fp->chorus_mix > 0) ? ((s->fx.meter_chorus_lfo_q15 < 0 ? -s->fx.meter_chorus_lfo_q15 : s->fx.meter_chorus_lfo_q15) * fp->chorus_mix) >> 10 : 0,
        s->fx.meter_delay_env_q15,
        s->fx.meter_reverb_env_q15,
        (fp->comp_mix > 0) ? s->fx.meter_comp_gr_q15 : 0
    };

    for (uint32_t i = 0; i < 4u; i++) {
        uint32_t fx_x   = 9u + i * 56u;
        uint32_t fx_w   = 54u;
        uint32_t bw_max = fx_w - 8u;
        uint32_t bw_mix = ((uint32_t)dsp_clamp_i32(fx_mix[i], 0, 1023) * bw_max) / 1023u;
        uint32_t bw_env = (uint32_t)dsp_clamp_i32(mul_gain_pct((fx_env[i] * (int32_t)bw_max) >> 15, gain), 0, (int32_t)bw_max);

        if (full_chrome || (int16_t)fx_mix[i] != ui->perf_prev_fx_mix[i]) {
            fb_fill_rect(fb, fx_x, fy, fx_w, 27u, COL_CARD);
            fb_rect_outline(fb, fx_x, fy, fx_w, 27u, fx_mix[i] > 0 ? th->border : COL_BAR_TRACK);

            fb_draw_str_spec(fb, fx_x + 3u, fy + 4u, k_fx_lbl[i],
                             fx_mix[i] > 0 ? th->accent : COL_MUTED, &k_font_sm_bold);
            char mbuf[8];
            fmt_int(mbuf, (fx_mix[i] * 100) / 1023, "%");
            fb_draw_str_spec(fb, fx_x + 25u, fy + 4u, mbuf, COL_WHITE, &k_font_sm_bold);

            fb_fill_rect(fb, fx_x + 4u, fy + 16u, bw_max, 6u, COL_BAR_TRACK);
            if (bw_mix > 0u) fb_fill_rect(fb, fx_x + 4u, fy + 16u, bw_mix, 6u, th->dim);
            if (bw_env > 0u) fb_fill_rect(fb, fx_x + 4u, fy + 16u, bw_env, 6u, th->accent);
            if (!full_chrome) {
                adrift_dirty_mark_rows(dirty, fy, 27u);
            }
        } else if ((uint8_t)bw_env != ui->perf_prev_fx_env[i]) {
            fb_fill_rect(fb, fx_x + 4u, fy + 16u, bw_max, 6u, COL_BAR_TRACK);
            if (bw_mix > 0u) fb_fill_rect(fb, fx_x + 4u, fy + 16u, bw_mix, 6u, th->dim);
            if (bw_env > 0u) fb_fill_rect(fb, fx_x + 4u, fy + 16u, bw_env, 6u, th->accent);
            adrift_dirty_mark_rows(dirty, fy + 16u, 6u);
        }

        ui->perf_prev_fx_mix[i] = (int16_t)fx_mix[i];
        ui->perf_prev_fx_env[i] = (uint8_t)bw_env;
    }
}

void draw_home_page_extra(uint16_t *fb,
                          adrift_ui_t *ui,
                          const adrift_synth_t *s,
                          bool full_chrome,
                          bool pips_dirty,
                          adrift_dirty_t *dirty) {
    switch (ui->scope_page % ADRIFT_UI_NUM_HOME_PAGES) {
        case ADRIFT_HOME_VECT:
            draw_home_vect_page(fb, ui, s, full_chrome, pips_dirty, dirty);
            break;
        case ADRIFT_HOME_SPEC:
            draw_home_spec_page(fb, ui, s, full_chrome, pips_dirty, dirty);
            break;
        case ADRIFT_HOME_STAGE:
            draw_home_stage_page(fb, ui, s, full_chrome, pips_dirty, dirty);
            break;
        case ADRIFT_HOME_POLY16:
            draw_home_poly16_page(fb, ui, s, full_chrome, pips_dirty, dirty);
            break;
        case ADRIFT_HOME_PERF:
        default:
            draw_home_perf_page(fb, ui, s, full_chrome, pips_dirty, dirty);
            break;
    }
}
