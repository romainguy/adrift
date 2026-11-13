#include "ui_internal.h"
#include "fonts.h"

static const adrift_group_theme_t k_group_themes[ADRIFT_MAX_GROUPS] = {
    /* 0: G1 — Warm Gold / Amber */
    { RGB565(248, 190, 48),  RGB565(116, 88,  20),  RGB565(90, 70, 28)  },
    /* 1: G2 — Lush Emerald */
    { RGB565(64,  232, 140), RGB565(28,  104, 64),  RGB565(28, 82, 58)  },
    /* 2: G3 — Electric Cyan */
    { RGB565(56,  216, 248), RGB565(24,  96,  116), RGB565(28, 78, 98)  },
    /* 3: G4 — Sunset Coral */
    { RGB565(252, 108, 86),  RGB565(120, 48,  38),  RGB565(94, 46, 42)  },
    /* 4: G5 — Synthwave Magenta */
    { RGB565(244, 100, 198), RGB565(116, 44,  94),  RGB565(90, 42, 78)  },
    /* 5: G6 — Luminous Violet */
    { RGB565(168, 132, 255), RGB565(78,  60,  124), RGB565(66, 52, 104) },
};

const adrift_group_theme_t *get_group_theme(uint8_t group_idx) {
    return &k_group_themes[group_idx % ADRIFT_MAX_GROUPS];
}

uint16_t adrift_ui_group_accent_rgb565(uint8_t group_idx) {
    return get_group_theme(group_idx)->accent;
}

void fb_fill_rect(uint16_t *fb, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t col) {
    if (w == 0u || h == 0u || x >= ADRIFT_SCREEN_W || y >= ADRIFT_SCREEN_H) return;
    if (x + w > ADRIFT_SCREEN_W) w = ADRIFT_SCREEN_W - x;
    if (y + h > ADRIFT_SCREEN_H) h = ADRIFT_SCREEN_H - y;

    uint32_t col32 = (uint32_t)col | ((uint32_t)col << 16);
    for (uint32_t r = y; r < y + h; r++) {
        uint16_t *p   = &fb[r * ADRIFT_SCREEN_W + x];
        uint32_t  rem = w;
        if (((uintptr_t)p & 2u) != 0u) {
            *p++ = col;
            rem--;
        }
        uint32_t *p32 = (uint32_t *)(void *)p;
        uint32_t  n32 = rem >> 1;
        while (n32 >= 4u) {
            p32[0] = col32;
            p32[1] = col32;
            p32[2] = col32;
            p32[3] = col32;
            p32 += 4;
            n32 -= 4u;
        }
        while (n32 > 0u) {
            *p32++ = col32;
            n32--;
        }
        if ((rem & 1u) != 0u) {
            *(uint16_t *)(void *)p32 = col;
        }
    }
}

void fb_hline(uint16_t *fb, uint32_t x, uint32_t y, uint32_t w, uint16_t col) {
    fb_fill_rect(fb, x, y, w, 1u, col);
}

void fb_rect_outline(uint16_t *fb, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t col) {
    if (w < 2u || h < 2u) return;
    fb_hline(fb, x, y, w, col);
    fb_hline(fb, x, y + h - 1u, w, col);
    fb_fill_rect(fb, x, y, 1u, h, col);
    fb_fill_rect(fb, x + w - 1u, y, 1u, h, col);
}

/* 1x compact (5x7, advance 6) */
const ui_font_spec_t k_font_sm = {
    &g_font5x7[0][0], 5u, 5u, 7u, 6u, 0u
};

/* Compact wide-tracked (6x8 metric box, advance 7) — for split-mode vertical page selector pills */
const ui_font_spec_t k_font_sm_bold = {
    &g_font5x7[0][0], 5u, 6u, 8u, 7u, 0u
};

/* Medium bold (7x10, advance 8) — for card labels, header text, and screen badges */
const ui_font_spec_t k_font_md = {
    &g_font_7x10_aa4[0][0], 35u, 7u, 10u, 8u, 1u
};

/* Extra-large tight (7x18, advance 8) — for 6-char values in 1x4 parameter row (47 px wide) */
const ui_font_spec_t k_font_xl_tight = {
    &g_font_7x18_aa4[0][0], 63u, 7u, 18u, 8u, 1u
};

/* Extra-large condensed (8x18, advance 9) — for 5-char values in 1x4 parameter row (44 px wide) */
const ui_font_spec_t k_font_xl_cond = {
    &g_font_8x18_aa4[0][0], 72u, 8u, 18u, 9u, 1u
};

/* Extra-large full (10x18, advance 11) — for 1..4 char values in 1x4 parameter row */
const ui_font_spec_t k_font_xl = {
    &g_font_10x18_aa4[0][0], 90u, 10u, 18u, 11u, 1u
};

uint32_t str_len(const char *s) {
    uint32_t n = 0u;
    while (s[n]) n++;
    return n;
}

uint32_t fb_str_width_spec(const char *s, const ui_font_spec_t *f) {
    uint32_t n = str_len(s);
    if (n == 0u) return 0u;
    return (n - 1u) * (uint32_t)f->advance + (uint32_t)f->glyph_w;
}

void fb_draw_char_spec(uint16_t *fb,
                       uint32_t x,
                       uint32_t y,
                       char ch,
                       uint16_t col,
                       const ui_font_spec_t *f) {
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    if (ch < 0x20 || ch > 0x5F) ch = '?';
    uint32_t g_idx = (uint32_t)((uint8_t)ch - 0x20u);

    if (!f->is_aa4) {
        const uint8_t *glyph = f->data + g_idx * (uint32_t)f->stride;
        for (uint32_t cx = 0; cx < 5u; cx++) {
            uint32_t px = x + cx;
            if (px >= ADRIFT_SCREEN_W) continue;
            uint8_t bits = glyph[cx];
            for (uint32_t cy = 0; cy < 7u; cy++) {
                if (bits & (1u << cy)) {
                    uint32_t py = y + cy;
                    if (py < ADRIFT_SCREEN_H) {
                        fb[py * ADRIFT_SCREEN_W + px] = col;
                    }
                }
            }
        }
        return;
    }

    const uint8_t *glyph = f->data + g_idx * (uint32_t)f->stride;
    uint32_t fg_r = (col >> 11) & 0x1Fu;
    uint32_t fg_g = (col >> 5)  & 0x3Fu;
    uint32_t fg_b =  col        & 0x1Fu;
    uint32_t gw   = f->glyph_w;
    uint32_t gh   = f->glyph_h;

    for (uint32_t cy = 0; cy < gh; cy++) {
        uint32_t py = y + cy;
        if (py >= ADRIFT_SCREEN_H) continue;
        uint16_t *row_ptr = &fb[py * ADRIFT_SCREEN_W];
        uint32_t row_off  = cy * gw;
        for (uint32_t cx = 0; cx < gw; cx++) {
            uint32_t pidx = row_off + cx;
            uint8_t  b    = glyph[pidx >> 1];
            uint8_t  a4   = (pidx & 1u) ? (b >> 4) : (b & 0x0Fu);
            if (a4 == 0u) continue;
            uint32_t px = x + cx;
            if (px >= ADRIFT_SCREEN_W) continue;
            if (a4 >= 15u) {
                row_ptr[px] = col;
            } else {
                uint32_t w     = (uint32_t)a4 + ((uint32_t)a4 >> 3);
                uint32_t inv_w = 16u - w;
                uint16_t bg    = row_ptr[px];
                uint32_t bg_r  = (bg >> 11) & 0x1Fu;
                uint32_t bg_g  = (bg >> 5)  & 0x3Fu;
                uint32_t bg_b  =  bg        & 0x1Fu;
                uint32_t r     = (bg_r * inv_w + fg_r * w + 8u) >> 4;
                uint32_t g     = (bg_g * inv_w + fg_g * w + 8u) >> 4;
                uint32_t bl    = (bg_b * inv_w + fg_b * w + 8u) >> 4;
                row_ptr[px]    = (uint16_t)((r << 11) | (g << 5) | bl);
            }
        }
    }
}

void fb_draw_str_spec(uint16_t *fb,
                      uint32_t x,
                      uint32_t y,
                      const char *s,
                      uint16_t col,
                      const ui_font_spec_t *f) {
    while (*s && x + f->glyph_w <= ADRIFT_SCREEN_W) {
        fb_draw_char_spec(fb, x, y, *s++, col, f);
        x += f->advance;
    }
}

void fb_draw_str_centered_spec(uint16_t *fb,
                               uint32_t box_x,
                               uint32_t box_w,
                               uint32_t y,
                               const char *s,
                               uint16_t col,
                               const ui_font_spec_t *f) {
    uint32_t tw = fb_str_width_spec(s, f);
    uint32_t tx = (tw < box_w) ? (box_x + (box_w - tw) / 2u) : (box_x + 1u);
    fb_draw_str_spec(fb, tx, y, s, col, f);
}

void fb_draw_str(uint16_t *fb, uint32_t x, uint32_t y, const char *s, uint16_t col) {
    fb_draw_str_spec(fb, x, y, s, col, &k_font_sm);
}

void copy_str(char *dst, const char *src) {
    while (*src) {
        *dst++ = *src++;
    }
    *dst = '\0';
}

void fmt_int(char *buf, int32_t val, const char *suffix) {
    char tmp[12];
    int  pos = 0;
    if (val < 0) {
        *buf++ = '-';
        val = -val;
    }
    do {
        tmp[pos++] = (char)('0' + (val % 10));
        val /= 10;
    } while (val > 0 && pos < 10);
    while (pos > 0) {
        *buf++ = tmp[--pos];
    }
    if (suffix) {
        while (*suffix) *buf++ = *suffix++;
    }
    *buf = '\0';
}

/* Format frequency concisely so it never exceeds 5 chars ("20HZ".."999HZ", "1.0K".."16.0K") */
void fmt_hz(char *buf, int32_t hz) {
    if (hz < 1000) {
        fmt_int(buf, hz, "HZ");
        return;
    }
    int32_t khz_int  = hz / 1000;
    int32_t khz_tent = (hz % 1000) / 100;
    fmt_int(buf, khz_int, "");
    while (*buf) buf++;
    *buf++ = '.';
    *buf++ = (char)('0' + khz_tent);
    *buf++ = 'K';
    *buf   = '\0';
}

/* Format milliseconds concisely so it never exceeds 5 chars ("0MS".."999MS", "1.0S".."2.0S") */
void fmt_ms(char *buf, int32_t ms) {
    if (ms < 1000) {
        fmt_int(buf, ms, "MS");
        return;
    }
    int32_t s_int  = ms / 1000;
    int32_t s_tent = (ms % 1000) / 100;
    fmt_int(buf, s_int, "");
    while (*buf) buf++;
    *buf++ = '.';
    *buf++ = (char)('0' + s_tent);
    *buf++ = 'S';
    *buf   = '\0';
}

void fmt_note(char *buf, uint8_t note) {
    static const char k_names[12][3] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    const char *nm = k_names[note % 12u];
    int32_t oct = (int32_t)(note / 12u) - 1;
    *buf++ = nm[0];
    if (nm[1]) *buf++ = nm[1];
    fmt_int(buf, oct, "");
}

uint16_t make_env_fill_col(uint16_t accent) {
    uint32_t r5 = (accent >> 11) & 0x1Fu;
    uint32_t g6 = (accent >> 5)  & 0x3Fu;
    uint32_t b5 =  accent        & 0x1Fu;
    uint32_t rf = 1u + ((r5 * 7u) >> 5);
    uint32_t gf = 3u + ((g6 * 7u) >> 5);
    uint32_t bf = 2u + ((b5 * 7u) >> 5);
    if (rf > 31u) rf = 31u;
    if (gf > 63u) gf = 63u;
    if (bf > 31u) bf = 31u;
    return (uint16_t)((rf << 11) | (gf << 5) | bf);
}

/* Sample Micro-ACB waveform shape at normalized phase p16 (0..65535) in Q15 */
int32_t eval_morph_wave_q15(uint32_t p16, const adrift_patch_t *p) {
    int32_t morph = dsp_clamp_i32(p->exc.wave_morph, 0, 1023);
    int32_t zone  = 0;
    int32_t mix_q15 = 0;
    if (morph < 341) {
        zone    = 0; /* Sine -> Triangle */
        mix_q15 = (morph * 32767) / 341;
    } else if (morph < 682) {
        zone    = 1; /* Triangle -> Sagged Saw */
        mix_q15 = ((morph - 341) * 32767) / 341;
    } else {
        zone    = 2; /* Sagged Saw -> PWM Pulse */
        mix_q15 = ((morph - 682) * 32767) / 341;
    }

    int32_t  pw_1023 = dsp_clamp_i32(p->exc.pulse_width, 0, 1023);
    uint32_t pw_16   = (uint32_t)(5243 + ((pw_1023 * 55050) >> 10));

    uint32_t t16 = (p16 - 16384u) & 0xFFFFu;
    int32_t  tri = (t16 < 32768u)
                       ? ((int32_t)(t16 << 1) - 32768)
                       : (32767 - (int32_t)((t16 - 32768u) << 1));

    int32_t r_lin = (int32_t)p16 - 32768;
    int32_t r2    = ((r_lin * r_lin) >> 15) - 10923;
    int32_t saw   = (p16 >= 64800u) ? -32767 : dsp_sat_q15(r_lin - ((3200 * r2) >> 15));

    if (zone == 0) {
        int32_t z2   = (tri * tri) >> 15;
        int32_t sine = (tri * (98304 - z2)) >> 16;
        return sine + (((tri - sine) * mix_q15) >> 15);
    } else if (zone == 1) {
        return tri + (((saw - tri) * mix_q15) >> 15);
    } else {
        int32_t pls = (p16 >= 64800u || p16 < pw_16) ? -32767 : 32767;
        return saw + (((pls - saw) * mix_q15) >> 15);
    }
}

void draw_voice_pips(uint16_t *fb, const adrift_synth_t *s, uint32_t sx, uint32_t sy, uint32_t sw) {
    uint8_t eg = s->edit_group % ADRIFT_MAX_GROUPS;
    const adrift_group_theme_t *eth = get_group_theme(eg);

    /* 2x8 compact pip grid for 16 hardware voices in top-right of scope/envelope pane */
    for (uint8_t v = 0; v < ADRIFT_MAX_VOICES; v++) {
        const adrift_voice_t *vc = &s->voices[v];
        uint32_t px = sx + sw - 58u + (uint32_t)(v & 7u) * 7u;
        uint32_t py = sy + 3u + (uint32_t)(v >> 3) * 6u;

        if (vc->owner_group < 0 || vc->owner_group >= ADRIFT_MAX_GROUPS) {
            fb_rect_outline(fb, px, py, 5u, 4u, COL_BAR_TRACK);
            continue;
        }

        uint8_t og = (uint8_t)vc->owner_group;
        const adrift_group_theme_t *vt  = get_group_theme(og);
        const adrift_patch_t       *vvp = &s->groups[og].patch;
        uint16_t pcol = vt->border;
        if ((vc->active && vvp->env2.drone_hold) || vc->key_down || vc->sustained) {
            pcol = vt->accent;
        } else if (vc->active && vc->env2.stage != ADRIFT_ENV_IDLE) {
            pcol = vt->dim;
        }
        fb_fill_rect(fb, px, py, 5u, 4u, pcol);
        if (og == eg) {
            fb_hline(fb, px, py + 4u, 5u, eth->accent);
        }
    }
}

void draw_screen_page_badge(uint16_t *fb,
                            uint32_t sx,
                            uint32_t sy,
                            const char *screen_name,
                            uint8_t subpage,
                            const adrift_group_theme_t *th) {
    /* Screen name in 7x10 bold accent font + [1/2]..[2/2] ([1/3]..[3/3] on EXC, [1/4]..[4/4] on MOD) page pill */
    fb_draw_str_spec(fb, sx + 6u, sy + 4u, screen_name, th->accent, &k_font_md);
    uint32_t name_w = fb_str_width_spec(screen_name, &k_font_md);
    uint32_t px     = sx + 6u + name_w + 5u;

    bool is_home    = (screen_name[0] == 'S' || screen_name[0] == 'V' || screen_name[0] == 'P');
    bool is_mod     = (screen_name[0] == 'M' && screen_name[1] == 'O');
    bool is_exc     = (screen_name[0] == 'E' && screen_name[1] == 'X');
    bool is_fx      = (screen_name[0] == 'F' && screen_name[1] == 'X');
    uint8_t num_sub = is_home ? ADRIFT_UI_NUM_HOME_PAGES
                              : (is_mod ? 4u : ((is_exc || is_fx) ? 3u : 2u));
    char tot_ch     = (char)('0' + num_sub);
    char pbuf[4]    = { (char)('1' + (subpage % num_sub)), '/', tot_ch, '\0' };
    uint32_t pw = 25u;
    fb_fill_rect(fb, px, sy + 3u, pw, 12u, th->accent);
    fb_draw_str_centered_spec(fb, px, pw, sy + 5u, pbuf, COL_BG, &k_font_sm_bold);
}
