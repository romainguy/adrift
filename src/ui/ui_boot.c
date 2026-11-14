#include "ui_internal.h"
#include "../dsp/dsp_math.h"

/*
 * Power-On Boot Animation — "Plucked Filament -> 6-Group Harmonic Bloom"
 *
 * 68 frames (~1.13 s at 60 Hz) on 240x240 RGB565:
 *   Act I   (f =  0..12): Taut horizon filament plucked left-to-right by an Exciter bead.
 *   Act II  (f =  1..50): Waveguide disperses into 6 harmonic ribbons (G1..G6 colors)
 *                         with additive nodal crossings, then damps into a unified filament.
 *   Act III (f =  8..50): Custom geometric "A D R I F T" wordmark, "16V BBD SYNTH" subtitle,
 *                         and 6 group pips illuminate and dissolve cleanly before upward glide.
 *   Act IV  (f = 51..67): Unified filament widens (216 -> 230 px), thickens (2 -> 3 px),
 *                         and glides smoothly to dock at the Header BBD bar (x=5, y=21, w=230, h=3).
 *
 * Strictly 32-bit integer arithmetic (zero int64_t, zero float) and row-exact dirty tracking.
 */

#define BOOT_STR_XL   12u
#define BOOT_STR_XR   227u
#define BOOT_STR_LEN  215u /* BOOT_STR_XR - BOOT_STR_XL */
#define BOOT_STR_Y    122

#define BOOT_WM_W     154u
#define BOOT_WM_H     22u
#define BOOT_WM_X     43u
#define BOOT_WM_Y     82u

typedef struct {
    uint16_t *fb;
    uint32_t  y_min;
    uint32_t  y_max;
} boot_ctx_t;

typedef struct {
    uint8_t x_off;
    uint8_t width;
} wm_glyph_box_t;

static const wm_glyph_box_t k_wm_boxes[ADRIFT_MAX_GROUPS] = {
    {   0u, 20u }, /* A */
    {  32u, 18u }, /* D */
    {  62u, 18u }, /* R */
    {  92u,  6u }, /* I */
    { 110u, 16u }, /* F */
    { 135u, 18u }  /* T (optically kerned for F+T open counters) */
};

/* Lazy-seeded packed 4-bit AA wordmark bitmap in .bss (1,716 bytes SRAM, 0 bytes .rodata) */
static uint8_t g_boot_wm_aa4[(BOOT_WM_W * BOOT_WM_H) / 2u];
static uint8_t g_boot_wm_ready;

static inline uint16_t rgb565_lerp(uint16_t bg, uint16_t fg, uint32_t a256) {
    if (a256 == 0u) return bg;
    if (a256 >= 256u) return fg;
    uint32_t inv  = 256u - a256;
    uint32_t bg_r = (bg >> 11) & 0x1Fu;
    uint32_t bg_g = (bg >> 5)  & 0x3Fu;
    uint32_t bg_b =  bg        & 0x1Fu;
    uint32_t fg_r = (fg >> 11) & 0x1Fu;
    uint32_t fg_g = (fg >> 5)  & 0x3Fu;
    uint32_t fg_b =  fg        & 0x1Fu;
    uint32_t r    = (bg_r * inv + fg_r * a256 + 128u) >> 8;
    uint32_t g    = (bg_g * inv + fg_g * a256 + 128u) >> 8;
    uint32_t b    = (bg_b * inv + fg_b * a256 + 128u) >> 8;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static inline uint16_t rgb565_add_scaled(uint16_t dst, uint16_t col, uint32_t a256) {
    if (a256 == 0u) return dst;
    uint32_t dr = (dst >> 11) & 0x1Fu;
    uint32_t dg = (dst >> 5)  & 0x3Fu;
    uint32_t db =  dst        & 0x1Fu;
    uint32_t cr = (col >> 11) & 0x1Fu;
    uint32_t cg = (col >> 5)  & 0x3Fu;
    uint32_t cb =  col        & 0x1Fu;
    uint32_t r  = dr + ((cr * a256 + 128u) >> 8);
    uint32_t g  = dg + ((cg * a256 + 128u) >> 8);
    uint32_t b  = db + ((cb * a256 + 128u) >> 8);
    if (r > 31u) r = 31u;
    if (g > 63u) g = 63u;
    if (b > 31u) b = 31u;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static inline void boot_touch_y(boot_ctx_t *ctx, uint32_t y) {
    if (y < ctx->y_min) ctx->y_min = y;
    if (y > ctx->y_max) ctx->y_max = y;
}

static inline void boot_blend_px(boot_ctx_t *ctx, uint32_t x, uint32_t y, uint16_t col, uint32_t a256) {
    if (a256 == 0u || x >= ADRIFT_SCREEN_W || y >= ADRIFT_SCREEN_H) return;
    boot_touch_y(ctx, y);
    uint16_t *p = &ctx->fb[y * ADRIFT_SCREEN_W + x];
    *p = rgb565_lerp(*p, col, a256);
}

static inline void boot_add_px(boot_ctx_t *ctx, uint32_t x, uint32_t y, uint16_t col, uint32_t a256) {
    if (a256 == 0u || x >= ADRIFT_SCREEN_W || y >= ADRIFT_SCREEN_H) return;
    boot_touch_y(ctx, y);
    uint16_t *p = &ctx->fb[y * ADRIFT_SCREEN_W + x];
    *p = rgb565_add_scaled(*p, col, a256);
}

static void boot_fill_rect_alpha(boot_ctx_t *ctx,
                                 uint32_t x,
                                 uint32_t y,
                                 uint32_t w,
                                 uint32_t h,
                                 uint16_t col,
                                 uint32_t a256) {
    if (a256 == 0u || w == 0u || h == 0u || x >= ADRIFT_SCREEN_W || y >= ADRIFT_SCREEN_H) return;
    if (x + w > ADRIFT_SCREEN_W) w = ADRIFT_SCREEN_W - x;
    if (y + h > ADRIFT_SCREEN_H) h = ADRIFT_SCREEN_H - y;
    boot_touch_y(ctx, y);
    boot_touch_y(ctx, y + h - 1u);
    if (a256 >= 256u) {
        fb_fill_rect(ctx->fb, x, y, w, h, col);
        return;
    }
    for (uint32_t ry = y; ry < y + h; ry++) {
        uint16_t *row = &ctx->fb[ry * ADRIFT_SCREEN_W + x];
        for (uint32_t rx = 0u; rx < w; rx++) {
            row[rx] = rgb565_lerp(row[rx], col, a256);
        }
    }
}

/* Point-to-segment distance test in 1/8-pixel integer coordinates */
static bool wm_in_seg8(int32_t qx,
                       int32_t qy,
                       int32_t ax,
                       int32_t ay,
                       int32_t bx,
                       int32_t by,
                       int32_t r8) {
    int32_t vx   = bx - ax;
    int32_t vy   = by - ay;
    int32_t len2 = vx * vx + vy * vy;
    int32_t wx   = qx - ax;
    int32_t wy   = qy - ay;
    int32_t dot  = wx * vx + wy * vy;
    int32_t t_q10;
    if (dot <= 0) {
        t_q10 = 0;
    } else if (dot >= len2) {
        t_q10 = 1024;
    } else {
        t_q10 = (dot << 10) / len2;
    }
    int32_t cx = ax + ((vx * t_q10 + 512) >> 10);
    int32_t cy = ay + ((vy * t_q10 + 512) >> 10);
    int32_t dx = qx - cx;
    int32_t dy = qy - cy;
    return (dx * dx + dy * dy) <= (r8 * r8);
}

/* Geometric vector glyph evaluator in 1/8-pixel space (H = 22 px -> 176 units) */
static bool wm_sample_letter8(uint32_t idx, int32_t qx, int32_t qy) {
    switch (idx) {
        case 0u: { /* A (w = 20 -> 160 units) */
            if (wm_in_seg8(qx, qy, 84, -17, 10, 193, 13)) return true;
            if (wm_in_seg8(qx, qy, 76, -17, 150, 193, 13)) return true;
            if (qy >= 108 && qy <= 132) {
                int32_t xl = 78 - ((62 * qy) / 176);
                int32_t xr = 82 + ((62 * qy) / 176);
                if (qx >= xl && qx <= xr) return true;
            }
            return false;
        }
        case 1u: { /* D (w = 18 -> 144 units) */
            if (qx >= 0 && qx <= 26) return true;
            if (qx <= 56 && (qy <= 26 || qy >= 150)) return true;
            if (qx >= 56) {
                int32_t dx = qx - 56;
                int32_t dy = qy - 88;
                int32_t d2 = dx * dx + dy * dy;
                if (d2 <= (88 * 88) && d2 >= (62 * 62)) return true;
            }
            return false;
        }
        case 2u: { /* R (w = 18 -> 144 units) */
            if (qx >= 0 && qx <= 26) return true;
            if (qx <= 84 && (qy <= 25 || (qy >= 79 && qy <= 104))) return true;
            if (qx >= 84 && qy <= 104) {
                int32_t dx = qx - 84;
                int32_t dy = qy - 52;
                int32_t d2 = dx * dx + dy * dy;
                if (d2 <= (52 * 52) && d2 >= (27 * 27)) return true;
            }
            if (qy >= 92 && wm_in_seg8(qx, qy, 76, 92, 136, 193, 13)) return true;
            return false;
        }
        case 3u: /* I (w = 6 -> 48 units) */
            return (qx >= 11 && qx <= 37);

        case 4u: /* F (w = 16 -> 128 units) */
            if (qx >= 0 && qx <= 26) return true;
            if (qy <= 26 && qx <= 128) return true;
            if (qy >= 79 && qy <= 104 && qx <= 102) return true;
            return false;

        default: /* T (w = 18 -> 144 units) */
            if (qy <= 26 && qx <= 144) return true;
            if (qx >= 59 && qx <= 85) return true;
            return false;
    }
}

static void boot_ensure_wordmark(void) {
    if (g_boot_wm_ready) return;

    for (uint32_t i = 0u; i < sizeof(g_boot_wm_aa4); i++) {
        g_boot_wm_aa4[i] = 0u;
    }

    for (uint32_t li = 0u; li < ADRIFT_MAX_GROUPS; li++) {
        uint32_t x0 = k_wm_boxes[li].x_off;
        uint32_t lw = k_wm_boxes[li].width;
        for (uint32_t py = 0u; py < BOOT_WM_H; py++) {
            for (uint32_t px = 0u; px < lw; px++) {
                uint32_t hits = 0u;
                for (uint32_t sy = 0u; sy < 4u; sy++) {
                    int32_t qy = (int32_t)(py * 8u + sy * 2u + 1u);
                    for (uint32_t sx = 0u; sx < 4u; sx++) {
                        int32_t qx = (int32_t)(px * 8u + sx * 2u + 1u);
                        if (wm_sample_letter8(li, qx, qy)) {
                            hits++;
                        }
                    }
                }
                if (hits > 0u) {
                    uint32_t a4   = (hits >= 16u) ? 15u : hits;
                    uint32_t gx   = x0 + px;
                    uint32_t pidx = py * BOOT_WM_W + gx;
                    uint8_t  cur  = g_boot_wm_aa4[pidx >> 1];
                    if ((pidx & 1u) != 0u) {
                        g_boot_wm_aa4[pidx >> 1] = (uint8_t)((cur & 0x0Fu) | (a4 << 4));
                    } else {
                        g_boot_wm_aa4[pidx >> 1] = (uint8_t)((cur & 0xF0u) | a4);
                    }
                }
            }
        }
    }

    g_boot_wm_ready = 1u;
}

static void boot_draw_wordmark(boot_ctx_t *ctx, uint32_t f) {
    if (f < 7u || f >= 51u) return;

    /* Global dissolve out across f = 43..51 */
    uint32_t fade_out_256 = 256u;
    if (f >= 43u) {
        fade_out_256 = ((51u - f) * 256u) / 8u;
    }
    if (fade_out_256 == 0u) return;

    for (uint32_t li = 0u; li < ADRIFT_MAX_GROUPS; li++) {
        uint32_t t_start = 7u + li * 2u; /* Staggered left-to-right behind pluck wavefront */
        if (f < t_start) continue;

        uint32_t age = f - t_start;
        uint32_t a_in_256 = (age >= 7u) ? 256u : ((age + 1u) * 256u) / 8u;
        uint32_t a_let_256 = (a_in_256 * fade_out_256) >> 8;
        if (a_let_256 == 0u) continue;

        /* Each letter ignites with its corresponding voice-group accent and cools to COL_WHITE */
        uint32_t tint_256 = (age >= 12u) ? 0u : ((12u - age) * 220u) / 12u;
        uint16_t col = rgb565_lerp(COL_WHITE, get_group_theme((uint8_t)li)->accent, tint_256);

        /* Subtle 2 px upward settle on ignition */
        uint32_t y_lift = (age < 4u) ? ((4u - age) >> 1) : 0u;
        uint32_t dst_y0 = BOOT_WM_Y + y_lift;

        uint32_t x_off = k_wm_boxes[li].x_off;
        uint32_t lw    = k_wm_boxes[li].width;
        for (uint32_t py = 0u; py < BOOT_WM_H; py++) {
            uint32_t row_off = py * BOOT_WM_W + x_off;
            uint32_t dy      = dst_y0 + py;
            for (uint32_t px = 0u; px < lw; px++) {
                uint32_t pidx = row_off + px;
                uint8_t  b    = g_boot_wm_aa4[pidx >> 1];
                uint32_t a4   = (pidx & 1u) ? (uint32_t)(b >> 4) : (uint32_t)(b & 0x0Fu);
                if (a4 == 0u) continue;
                uint32_t cov256 = (a4 * 17u * a_let_256) >> 8;
                boot_blend_px(ctx, BOOT_WM_X + x_off + px, dy, col, cov256);
            }
        }
    }
}

static void boot_draw_str_md_alpha(boot_ctx_t *ctx,
                                   uint32_t box_x,
                                   uint32_t box_w,
                                   uint32_t y,
                                   const char *s,
                                   uint16_t col,
                                   uint32_t a256) {
    if (a256 == 0u) return;
    const ui_font_spec_t *font = &k_font_md;
    uint32_t tw = fb_str_width_spec(s, font);
    uint32_t x  = (tw < box_w) ? (box_x + (box_w - tw) / 2u) : box_x;
    uint32_t gw = font->glyph_w;
    uint32_t gh = font->glyph_h;

    while (*s && x + gw <= ADRIFT_SCREEN_W) {
        char ch = *s++;
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
        if (ch < 0x20 || ch > 0x5F) ch = '?';
        const uint8_t *glyph = font->data + (uint32_t)((uint8_t)ch - 0x20u) * (uint32_t)font->stride;
        for (uint32_t cy = 0u; cy < gh; cy++) {
            uint32_t py = y + cy;
            if (py >= ADRIFT_SCREEN_H) continue;
            uint32_t row_off = cy * gw;
            for (uint32_t cx = 0u; cx < gw; cx++) {
                uint32_t pidx = row_off + cx;
                uint8_t  b    = glyph[pidx >> 1];
                uint32_t a4   = (pidx & 1u) ? (uint32_t)(b >> 4) : (uint32_t)(b & 0x0Fu);
                if (a4 == 0u) continue;
                uint32_t cov256 = (a4 * 17u * a256) >> 8;
                boot_blend_px(ctx, x + cx, py, col, cov256);
            }
        }
        x += font->advance;
    }
}

/* Subpixel vertical anti-aliased ribbon stroke (y_q8 in 1/256 px, ~1.5 px visual core) */
static void boot_plot_ribbon_px(boot_ctx_t *ctx, uint32_t x, int32_t y_q8, uint16_t col, uint32_t a256) {
    if (a256 == 0u || x >= ADRIFT_SCREEN_W) return;
    int32_t y_i  = y_q8 >> 8;
    int32_t frac = y_q8 & 0xFF; /* 0..255 */

    /* Symmetric 3-tap tent kernel centered at y_q8 (sum = 384 -> peak = 256, wings up to 192) */
    uint32_t w_mid = (uint32_t)(256 - ((frac > 128 ? (frac - 128) : (128 - frac)) >> 1));
    uint32_t w_top = (uint32_t)((256 - frac) >> 1);
    uint32_t w_bot = (uint32_t)(frac >> 1);

    if (y_i - 1 >= 0 && y_i - 1 < (int32_t)ADRIFT_SCREEN_H) {
        boot_add_px(ctx, x, (uint32_t)(y_i - 1), col, (w_top * a256) >> 8);
    }
    if (y_i >= 0 && y_i < (int32_t)ADRIFT_SCREEN_H) {
        boot_add_px(ctx, x, (uint32_t)y_i, col, (w_mid * a256) >> 8);
    }
    if (y_i + 1 >= 0 && y_i + 1 < (int32_t)ADRIFT_SCREEN_H) {
        boot_add_px(ctx, x, (uint32_t)(y_i + 1), col, (w_bot * a256) >> 8);
    }
}

/* Soft radial glow disc (r_px <= 6) using additive RGB565 blending */
static void boot_draw_glow_disc(boot_ctx_t *ctx,
                                int32_t cx,
                                int32_t cy,
                                int32_t r_px,
                                uint16_t core_col,
                                uint16_t halo_col,
                                uint32_t a256) {
    if (a256 == 0u || r_px <= 0) return;
    int32_t r2 = r_px * r_px;
    for (int32_t dy = -r_px; dy <= r_px; dy++) {
        int32_t py = cy + dy;
        if (py < 0 || py >= (int32_t)ADRIFT_SCREEN_H) continue;
        for (int32_t dx = -r_px; dx <= r_px; dx++) {
            int32_t d2 = dx * dx + dy * dy;
            if (d2 > r2) continue;
            int32_t px = cx + dx;
            if (px < 0 || px >= (int32_t)ADRIFT_SCREEN_W) continue;
            uint32_t falloff = (uint32_t)(((r2 - d2) * 256) / r2);
            uint32_t fall2   = (falloff * falloff) >> 8;
            uint16_t col     = (d2 * 4 <= r2) ? core_col : halo_col;
            boot_add_px(ctx, (uint32_t)px, (uint32_t)py, col, (fall2 * a256) >> 8);
        }
    }
}

static void boot_draw_harmonic_string(boot_ctx_t *ctx, uint32_t f, uint16_t target_accent) {
    /* Exciter pluck wavefront sweeps x = 12..227 across frames 0..12 */
    uint32_t pluck_x = (f < 12u)
                           ? (BOOT_STR_XL + (f * BOOT_STR_LEN) / 12u)
                           : BOOT_STR_XR;

    /* Unplucked taut horizon ahead of wavefront during Act I */
    if (f < 12u && pluck_x < BOOT_STR_XR) {
        uint32_t base_a = (f < 4u) ? ((f + 1u) * 56u) : 224u;
        uint16_t rest_c = rgb565_lerp(COL_BAR_TRACK, target_accent, 48u);
        boot_fill_rect_alpha(ctx, pluck_x, (uint32_t)BOOT_STR_Y, BOOT_STR_XR - pluck_x + 1u, 1u, rest_c, base_a);
    }

    /* Vibration amplitude envelope (peaks ~16 px around f=12..18, damps cleanly to 0 at f=50) */
    int32_t amp_q8;
    if (f <= 12u) {
        amp_q8 = (int32_t)(900u + (f * 3200u) / 12u); /* 3.5 px -> 16.0 px */
    } else if (f <= 20u) {
        amp_q8 = (int32_t)(4100u - ((f - 12u) * 500u) / 8u); /* 16.0 px -> 14.0 px */
    } else if (f < 50u) {
        uint32_t rem = 50u - f; /* 29 .. 1 */
        amp_q8 = (int32_t)((rem * rem * 3600u) / (30u * 30u));
    } else {
        amp_q8 = 0;
    }

    /* Harmonic dispersion spread (0 -> 32768 across f=0..12, holds, converges -> 0 over f=30..50) */
    int32_t disp_q15;
    if (f <= 12u) {
        disp_q15 = (int32_t)((f * 32768u) / 12u);
    } else if (f <= 30u) {
        disp_q15 = 32768;
    } else if (f < 50u) {
        uint32_t rem = 50u - f;
        disp_q15 = (int32_t)((rem * rem * 32768u) / (20u * 20u));
    } else {
        disp_q15 = 0;
    }

    /* Color & brightness convergence into the unified Gold/accent filament (f = 35..50) */
    uint32_t conv_256 = 0u;
    if (f >= 35u) {
        conv_256 = (f >= 50u) ? 256u : ((f - 35u) * 256u) / 15u;
    }

    /* Per-ribbon additive alpha: 184 while braided, normalizing to ~52*6 when stacked together */
    uint32_t rib_a256 = 184u - ((conv_256 * 132u) >> 8);

    uint16_t rib_cols[ADRIFT_MAX_GROUPS];
    uint32_t base_ph1[ADRIFT_MAX_GROUPS];
    uint32_t base_ph2[ADRIFT_MAX_GROUPS];
    for (uint32_t g = 0u; g < ADRIFT_MAX_GROUPS; g++) {
        rib_cols[g] = rgb565_lerp(get_group_theme((uint8_t)g)->accent, target_accent, conv_256);
        /* Distinct temporal phase velocities + dispersion offsets per voice group */
        uint32_t vel1 = 0x12000000u + g * 0x03400000u;
        uint32_t vel2 = 0x1D000000u + g * 0x04A00000u;
        uint32_t off1 = ((g * 0x2AAAu) * (uint32_t)disp_q15) << 1;
        uint32_t off2 = ((g * 0x3D55u) * (uint32_t)disp_q15) << 1;
        base_ph1[g] = f * vel1 + off1;
        base_ph2[g] = f * vel2 + off2;
    }

    uint32_t max_x = pluck_x;
    for (uint32_t x = BOOT_STR_XL; x <= max_x; x++) {
        uint32_t i     = x - BOOT_STR_XL;
        uint32_t u_q15 = (i * 32768u) / BOOT_STR_LEN;

        /* Pinned boundary envelope: sin(pi * u) == 0 at x=12 and x=227 */
        int32_t win_q15 = dsp_sine_lut_q15(u_q15 << 16);

        /* Smooth pluck-front taper right behind the traveling Exciter bead */
        if (f < 12u) {
            uint32_t dist = pluck_x - x;
            if (dist < 24u) {
                win_q15 = (win_q15 * (int32_t)(dist * 1365u)) >> 15;
            }
        }

        int32_t env_amp_q8 = (amp_q8 * win_q15) >> 15;
        uint32_t sp1 = u_q15 << 16;          /* Mode 1: 1 half-wave */
        uint32_t sp2 = (u_q15 * 2u) << 16;   /* Mode 2: 2 half-waves */
        uint32_t sp3 = (u_q15 * 3u) << 16;   /* Mode 3: 3 half-waves */

        for (uint32_t g = 0u; g < ADRIFT_MAX_GROUPS; g++) {
            int32_t m1 = dsp_sine_lut_q15(sp1 + base_ph1[g]);
            int32_t m2 = dsp_sine_lut_q15(sp2 - base_ph2[g]);
            int32_t m3 = dsp_sine_lut_q15(sp3 + base_ph1[g] + base_ph2[g]);

            /* Higher overtones scale with disp_q15 so they damp faster than fundamental */
            int32_t overtones = (((m2 * 19000) >> 15) + ((m3 * 9500) >> 15));
            overtones = (overtones * disp_q15) >> 15;
            int32_t wave_q15 = ((m1 * 23500) >> 15) + overtones;

            int32_t dy_q8 = (env_amp_q8 * wave_q15) >> 15;
            int32_t y_q8  = (BOOT_STR_Y << 8) + dy_q8;
            boot_plot_ribbon_px(ctx, x, y_q8, rib_cols[g], rib_a256);
        }
    }

    /* Crisp converged core reinforcement as ribbons lock into unison (f = 42..50) */
    if (f >= 42u) {
        uint32_t lock_a = ((f - 41u) * 256u) / 9u;
        if (lock_a > 256u) lock_a = 256u;
        boot_fill_rect_alpha(ctx, BOOT_STR_XL, (uint32_t)(BOOT_STR_Y - 1), BOOT_STR_LEN + 1u, 2u, target_accent, lock_a);
    }

    /* Bridge anchor pins at (12, 122) and (227, 122) */
    uint32_t pin_a = (f < 44u) ? 220u : ((51u - f) * 220u) / 7u;
    boot_fill_rect_alpha(ctx, BOOT_STR_XL - 1u, (uint32_t)(BOOT_STR_Y - 1), 3u, 3u, COL_WHITE, pin_a);
    uint16_t rpin_col = (f >= 11u && f <= 16u) ? COL_WHITE : COL_MUTED;
    boot_fill_rect_alpha(ctx, BOOT_STR_XR - 1u, (uint32_t)(BOOT_STR_Y - 1), 3u, 3u, rpin_col, pin_a);

    /* Traveling Exciter pluck bead (f = 0..12) + right-bridge reflection glint (f = 12..16) */
    if (f <= 12u) {
        uint16_t cyan_halo = get_group_theme(2u)->accent; /* G3 Electric Cyan */
        boot_draw_glow_disc(ctx, (int32_t)pluck_x, BOOT_STR_Y, 5, COL_WHITE, cyan_halo, 248u);
    } else if (f <= 16u) {
        uint32_t refl_a = ((17u - f) * 210u) / 5u;
        boot_draw_glow_disc(ctx, (int32_t)BOOT_STR_XR, BOOT_STR_Y, 4, COL_WHITE, target_accent, refl_a);
    }
}

/* Act IV (f = 51..67): Smooth upward glide & morph into Header BBD bar (x=5, y=21, w=230, h=3) */
static void boot_draw_dock_glide(boot_ctx_t *ctx, uint32_t f, uint16_t target_accent) {
    uint32_t step  = f - 51u; /* 0 .. 16 */
    int32_t  t_q15 = (int32_t)((step * 32768u) / 16u);
    /* Cubic smoothstep s = t^2 * (3 - 2*t) in Q15 (exact 0 at step=0, exact 32768 at step=16) */
    int32_t  t2_q15 = (t_q15 * t_q15) >> 15;
    int32_t  s_q15  = (t2_q15 * (98304 - (t_q15 << 1))) >> 15;

    /* Interpolate x0: 12 -> 5, x1: 227 -> 234, y_top_q8: 121.0 -> 21.0, h_q8: 2.0 -> 3.0 */
    uint32_t x0 = 12u - (uint32_t)((7 * s_q15 + 16384) >> 15);
    uint32_t x1 = 227u + (uint32_t)((7 * s_q15 + 16384) >> 15);
    uint32_t w  = x1 - x0 + 1u;

    int32_t y0_q8 = (121 << 8) - (((100 << 8) * s_q15 + 16384) >> 15);
    int32_t h_q8  = (2 << 8) + (((1 << 8) * s_q15 + 16384) >> 15);
    int32_t y1_q8 = y0_q8 + h_q8;

    /* Soft BBD phosphor wake beneath the rising filament (peaks mid-glide, zero at dock) */
    int32_t vel_arch = (t_q15 * (32768 - t_q15)) >> 13; /* 0 .. 32768 */
    if (vel_arch > 0 && step < 16u) {
        int32_t  wy0   = (y1_q8 + 255) >> 8;
        uint16_t trail = rgb565_lerp(target_accent, get_group_theme(5u)->accent, 96u);
        for (int32_t k = 0; k < 5; k++) {
            int32_t ry = wy0 + k;
            if (ry >= 0 && ry < (int32_t)ADRIFT_SCREEN_H) {
                uint32_t wa = (uint32_t)((((5 - k) * 22) * vel_arch) >> 15);
                boot_fill_rect_alpha(ctx, x0 + 2u, (uint32_t)ry, w - 4u, 1u, trail, wa);
            }
        }
    }

    /* Subpixel vertical rasterization of [y0_q8, y1_q8] (exact integer [21, 24) on step == 16) */
    int32_t r_first = y0_q8 >> 8;
    int32_t r_last  = (y1_q8 - 1) >> 8;
    for (int32_t ry = r_first; ry <= r_last; ry++) {
        if (ry < 0 || ry >= (int32_t)ADRIFT_SCREEN_H) continue;
        int32_t row_top = ry << 8;
        int32_t row_bot = (ry + 1) << 8;
        int32_t ov_top  = (y0_q8 > row_top) ? y0_q8 : row_top;
        int32_t ov_bot  = (y1_q8 < row_bot) ? y1_q8 : row_bot;
        int32_t cov256  = ov_bot - ov_top;
        if (cov256 > 0) {
            boot_fill_rect_alpha(ctx, x0, (uint32_t)ry, w, 1u, target_accent, (uint32_t)cov256);
        }
    }
}

void draw_boot_frame(adrift_ui_t *ui,
                     const adrift_synth_t *s,
                     uint16_t *fb,
                     adrift_dirty_t *dirty) {
    boot_ensure_wordmark();

    uint32_t f = ui->boot_frame;
    if (f >= ADRIFT_UI_BOOT_FRAMES) {
        f = ADRIFT_UI_BOOT_FRAMES - 1u;
    }

    if (!ui->initialized || f == 0u) {
        fb_fill_rect(fb, 0u, 0u, ADRIFT_SCREEN_W, ADRIFT_SCREEN_H, COL_BG);
        ui->initialized    = true;
        dirty->full_redraw = true;
    } else if (ui->boot_prev_y1 >= ui->boot_prev_y0) {
        uint32_t prev_y0 = ui->boot_prev_y0;
        uint32_t prev_h  = (uint32_t)ui->boot_prev_y1 - prev_y0 + 1u;
        fb_fill_rect(fb, 0u, prev_y0, ADRIFT_SCREEN_W, prev_h, COL_BG);
        adrift_dirty_mark_rows(dirty, prev_y0, prev_h);
    }

    boot_ctx_t ctx;
    ctx.fb    = fb;
    ctx.y_min = ADRIFT_SCREEN_H;
    ctx.y_max = 0u;

    uint16_t target_accent = get_group_theme(s->edit_group % ADRIFT_MAX_GROUPS)->accent;

    if (f < 51u) {
        /* Acts I-III: Wordmark + Subtitle + 6 Group Pips + Plucked 6-Ribbon Waveguide */
        boot_draw_wordmark(&ctx, f);

        if (f >= 18u) {
            uint32_t sub_in  = (f < 30u) ? ((f - 17u) * 256u) / 13u : 256u;
            uint32_t sub_out = (f >= 43u) ? ((51u - f) * 256u) / 8u : 256u;
            uint32_t sub_a   = (sub_in * sub_out) >> 8;
            boot_draw_str_md_alpha(&ctx, 0u, ADRIFT_SCREEN_W, 141u, "16V BBD SYNTH", COL_MUTED, sub_a);
        }

        if (f >= 20u) {
            uint32_t pip_out = (f >= 43u) ? ((51u - f) * 256u) / 8u : 256u;
            /* 6 group pips: 6 * 8 px + 5 * 4 px = 68 px -> centered at x = 86..153, y = 155..156 */
            for (uint32_t g = 0u; g < ADRIFT_MAX_GROUPS; g++) {
                uint32_t p_start = 20u + g * 2u;
                if (f < p_start) continue;
                uint32_t p_age = f - p_start;
                uint32_t p_in  = (p_age >= 5u) ? 256u : ((p_age + 1u) * 256u) / 6u;
                uint32_t p_a   = (p_in * pip_out) >> 8;
                boot_fill_rect_alpha(&ctx, 86u + g * 12u, 155u, 8u, 2u,
                                     get_group_theme((uint8_t)g)->accent, p_a);
            }
        }

        boot_draw_harmonic_string(&ctx, f, target_accent);
    } else {
        /* Act IV: Converged filament glides upward to dock into Header BBD bar */
        boot_draw_dock_glide(&ctx, f, target_accent);
    }

    if (ctx.y_min > ctx.y_max) {
        ctx.y_min = 21u;
        ctx.y_max = 23u;
    }

    adrift_dirty_mark_rows(dirty, ctx.y_min, ctx.y_max - ctx.y_min + 1u);
    ui->boot_prev_y0 = (uint8_t)ctx.y_min;
    ui->boot_prev_y1 = (uint8_t)ctx.y_max;
}
