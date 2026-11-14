#ifndef ADRIFT_UI_INTERNAL_H
#define ADRIFT_UI_INTERNAL_H

#include "ui.h"

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8u) << 8) | (((g) & 0xFCu) << 3) | (((b) & 0xF8u) >> 3)))

/* Dark neutral surfaces + per-group signature highlight themes (G1..G6) */
#define COL_BG         RGB565(12, 14, 20)
#define COL_HEADER     RGB565(20, 24, 34)
#define COL_CARD       RGB565(18, 22, 32)
#define COL_CARD_HDR   RGB565(25, 30, 44)
#define COL_TAB_OFF    RGB565(26, 30, 42)
#define COL_WHITE      RGB565(238, 242, 248)
#define COL_MUTED      RGB565(136, 148, 172)
#define COL_SCOPE_BG   RGB565(8, 12, 18)
#define COL_SCOPE_GRID RGB565(26, 36, 48)
#define COL_BAR_TRACK  RGB565(34, 40, 54)

typedef enum {
    UI_LAYOUT_PARAM_SCREEN   = 0, /* Top Visualizer/Status Pane + 1x4 Parameter Card Row */
    UI_LAYOUT_FULL_SCOPE     = 2, /* Full-screen oscilloscope (HOME screen only) */
    UI_LAYOUT_PRESET_BROWSER = 3, /* Full-pane Presets & Init Patch Browser */
    UI_LAYOUT_SCALE_BROWSER  = 4, /* Full-pane Keyboard Scale & Root Browser */
    UI_LAYOUT_PRESET_SAVE    = 5  /* Full-pane Save User Preset & Name Editor Modal */
} ui_layout_mode_t;

typedef struct {
    uint16_t accent; /* Primary highlight: active tab, bars, badges, scope/envelope trace */
    uint16_t dim;    /* Dimmed group color: releasing voice pip / drone underlie */
    uint16_t border; /* Tinted structural outline: header rule, scope frame, card border */
} adrift_group_theme_t;

/* Dedicated multi-size UI typography (crisp 1-bit for small, 4-bit AA for medium/XL) */
typedef struct {
    const uint8_t *data;    /* Base pointer to 64-glyph table */
    uint8_t        stride;  /* Bytes per glyph in data table */
    uint8_t        glyph_w; /* Glyph bounding width in px */
    uint8_t        glyph_h; /* Glyph bounding height in px */
    uint8_t        advance; /* glyph_w + inter-character spacing */
    uint8_t        is_aa4;  /* 0 = 5x7 1-bit column bitmap, 1 = packed 4-bit grayscale AA */
} ui_font_spec_t;

extern const ui_font_spec_t k_font_sm;
extern const ui_font_spec_t k_font_sm_bold;
extern const ui_font_spec_t k_font_md;
extern const ui_font_spec_t k_font_xl_tight;
extern const ui_font_spec_t k_font_xl_cond;
extern const ui_font_spec_t k_font_xl;

extern const char *const k_arp_mode_names[ADRIFT_ARP_MODE_COUNT];
extern const char *const k_arp_ratchet_names[ADRIFT_RATCHET_COUNT];
extern const char *const k_mod_dest_names[ADRIFT_MOD_DEST_COUNT];

/* Theme, drawing, font, and formatting primitives (ui_draw.c) */
const adrift_group_theme_t *get_group_theme(uint8_t group_idx);
void     fb_fill_rect(uint16_t *fb, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t col);
void     fb_hline(uint16_t *fb, uint32_t x, uint32_t y, uint32_t w, uint16_t col);
void     fb_rect_outline(uint16_t *fb, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t col);
uint32_t str_len(const char *s);
uint32_t fb_str_width_spec(const char *s, const ui_font_spec_t *f);
void     fb_draw_char_spec(uint16_t *fb, uint32_t x, uint32_t y, char ch, uint16_t col, const ui_font_spec_t *f);
void     fb_draw_str_spec(uint16_t *fb, uint32_t x, uint32_t y, const char *s, uint16_t col, const ui_font_spec_t *f);
void     fb_draw_str_centered_spec(uint16_t *fb, uint32_t box_x, uint32_t box_w, uint32_t y, const char *s, uint16_t col, const ui_font_spec_t *f);
void     fb_draw_str(uint16_t *fb, uint32_t x, uint32_t y, const char *s, uint16_t col);
void     copy_str(char *dst, const char *src);
void     fmt_int(char *buf, int32_t val, const char *suffix);
void     fmt_hz(char *buf, int32_t hz);
void     fmt_ms(char *buf, int32_t ms);
void     fmt_note(char *buf, uint8_t note);
uint16_t make_env_fill_col(uint16_t accent);
int32_t  eval_morph_wave_q15(uint32_t p16, const adrift_patch_t *p);
void     draw_voice_pips(uint16_t *fb, const adrift_synth_t *s, uint32_t sx, uint32_t sy, uint32_t sw);
void     draw_screen_page_badge(uint16_t *fb, uint32_t sx, uint32_t sy, const char *screen_name, uint8_t subpage, const adrift_group_theme_t *th);

/* Parameter editing, formatting & 1x4 card row (ui_params.c) */
uint8_t page_num_subpages(uint8_t page, bool in_group_menu, bool in_arp_menu, bool in_fx_menu);
bool    is_unmapped_param_slot(uint8_t page, uint8_t param_idx);
void    apply_encoder_for_synth(adrift_synth_t *synth,
                                bool in_group_menu,
                                bool in_arp_menu,
                                bool in_fx_menu,
                                uint8_t page,
                                uint8_t enc,
                                int32_t delta);
void    draw_cards_1x4(uint16_t *fb,
                       const adrift_synth_t *s,
                       uint8_t page,
                       uint8_t subpage,
                       bool in_group_menu,
                       bool in_arp_menu,
                       bool in_fx_menu);

/* Top visualizer panes & HOME full-screen visualizations (ui_panes.c) */
void draw_group_matrix(uint16_t *fb, const adrift_synth_t *s, uint8_t subpage);
void draw_env_graph(uint16_t *fb, const adrift_synth_t *s, bool is_env1, uint8_t subpage);
void draw_exc_pane(uint16_t *fb, const adrift_synth_t *s, uint8_t subpage);
void draw_bbd_pane(uint16_t *fb, const adrift_synth_t *s, uint8_t subpage);
void draw_mod_pane(uint16_t *fb, const adrift_synth_t *s, uint8_t subpage);
void draw_arp_pane(uint16_t *fb, const adrift_synth_t *s, uint8_t subpage);
void draw_fx_pane(uint16_t *fb, const adrift_synth_t *s, uint8_t subpage);
void draw_home_knob_bar(uint16_t *fb, const adrift_ui_t *ui, const adrift_group_theme_t *th, const char *mode_name);
void draw_home_page_extra(uint16_t *fb,
                          adrift_ui_t *ui,
                          const adrift_synth_t *s,
                          bool full_chrome,
                          bool pips_dirty,
                          adrift_dirty_t *dirty);

/* Power-on boot splash animation (ui_boot.c) */
void draw_boot_frame(adrift_ui_t *ui,
                     const adrift_synth_t *s,
                     uint16_t *fb,
                     adrift_dirty_t *dirty);

#endif /* ADRIFT_UI_INTERNAL_H */
