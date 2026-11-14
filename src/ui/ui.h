#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "../platform/platform.h"
#include "../synth/synth.h"

#define ADRIFT_UI_NUM_PAGES         5u
#define ADRIFT_UI_NUM_SUBPAGES      2u
#define ADRIFT_UI_NUM_HOME_PAGES    6u
#define ADRIFT_UI_NUM_HOME_MODES    3u
#define ADRIFT_UI_SPEC_BINS         32u
#define ADRIFT_UI_WATERFALL_ROWS    24u
#define ADRIFT_UI_BOOT_FRAMES       68u
#define ADRIFT_UI_PRESET_FACT_COUNT (2u + ADRIFT_NUM_PRESETS) /* 0=INIT SYNTH, 1=INIT GROUP, 2..25=P1..P24 */
#define ADRIFT_UI_PRESET_USER_COUNT (ADRIFT_NUM_USER_PRESETS) /* 24 = U1..U24 */
#define ADRIFT_UI_PRESET_ALL_COUNT  (ADRIFT_UI_PRESET_FACT_COUNT + ADRIFT_UI_PRESET_USER_COUNT) /* 50 */
#define ADRIFT_UI_PRESET_LIST_COUNT ADRIFT_UI_PRESET_ALL_COUNT

typedef enum {
    ADRIFT_PRESET_BANK_ALL   = 0,
    ADRIFT_PRESET_BANK_USER  = 1,
    ADRIFT_PRESET_BANK_FACT  = 2,
    ADRIFT_PRESET_BANK_COUNT = 3
} adrift_preset_bank_t;

typedef enum {
    ADRIFT_HOME_SCOPE  = 0, /* 1/6: Full-screen Waveform Oscilloscope */
    ADRIFT_HOME_VECT   = 1, /* 2/6: Stereo Vectorscope / Lissajous Goniometer */
    ADRIFT_HOME_SPEC   = 2, /* 3/6: Real-Time Spectrum Analyzer & Waterfall */
    ADRIFT_HOME_STAGE  = 3, /* 4/6: 2D Stereo Soundstage & 16-Voice Constellation */
    ADRIFT_HOME_POLY16 = 4, /* 5/6: 16-Voice Telemetry Bridge */
    ADRIFT_HOME_PERF   = 5  /* 6/6: 6-Group Performance & Arpeggiator Dashboard */
} adrift_home_page_t;

typedef struct {
    int16_t  px;
    int16_t  py;
    int16_t  g_pan_x;
    int16_t  g_py;
    uint8_t  wing_w;
    uint8_t  rad_px;
    uint8_t  flags; /* bit0: assigned, bit1: active, bit2: gate, bits3..5: owner_group */
    uint8_t  note_disp;
} adrift_stage_node_t;

typedef struct {
    uint8_t  page;          /* 0..4 (EXC, ENV1, BBD, ENV2, MOD) */
    uint8_t  subpage;       /* 0..1 (Page 1/2: params 0..3, Page 2/2: params 4..6 + empty) */
    bool     in_group_menu; /* SAVE Group Settings screen */
    bool     in_arp_menu;   /* ARP Arpeggiator Parameters screen */
    bool     in_fx_menu;    /* FX Master Stereo Bus Effects screen */
    bool     in_scope;      /* HOME Visualizations screen (6 pages) */
    uint8_t  scope_page;    /* 0..5 (adrift_home_page_t: SCOPE, VECT, SPEC, STAGE, POLY16, PERF) */
    uint8_t  scope_zoom;    /* KNOB1 on HOME: 0=1X, 1=2X, 2=4X, 3=8X */
    uint16_t scope_gain;    /* KNOB2 on HOME: 25..400 % visual gain (default 100%) */
    bool     scope_freeze;  /* KNOB3 on HOME: false=RUN, true=HOLD/FREEZE */
    uint8_t  scope_mode[ADRIFT_UI_NUM_HOME_PAGES]; /* KNOB4 on HOME: 0..2 display mode per page */
    bool     in_preset_menu;/* PRESETS Browser screen */
    bool     preset_previewed; /* True once cursor has previewed an entry in Presets Browser */
    uint8_t  preset_cursor; /* Logical ID: 0=INIT SYNTH, 1=INIT GROUP, 2..25=P1..P24, 26..49=U1..U24 */
    uint8_t  preset_bank;   /* adrift_preset_bank_t: 0=ALL, 1=USER, 2=FACT */
    bool     preset_load_grp; /* false=FULL preset load, true=single-group patch load into edit_group */
    uint8_t  preset_src_grp;  /* 0..5 source group (G1..G6) when preset_load_grp is true */
    bool     preset_clear_armed; /* Two-step clear confirmation for occupied user slot */
    adrift_synth_snapshot_t preset_snapshot; /* User synth state saved when opening Presets Browser */
    bool     in_save_menu;  /* SAVE User Preset & Name Editor modal */
    uint8_t  save_slot;     /* 0..23 target User Preset slot (U1..U24) */
    uint8_t  save_cursor;   /* 0..11 active character cell in save_name */
    char     save_name[13]; /* Space-padded 12-char name buffer + NUL */
    char     save_orig_name[13]; /* Initial name when modal opened (restored by 4:RST) */
    uint8_t  save_toast_frames;  /* Countdown frames for post-save [SAVE] header banner */
    uint8_t  save_toast_slot;    /* 0..23 slot index shown in post-save banner */
    bool     in_scale_menu; /* ALGORITHM Scale Browser screen */
    bool     scale_previewed; /* True once cursor/root has previewed a scale in Scale Browser */
    uint8_t  scale_cursor;  /* 0..15 = adrift_scale_t */
    uint8_t  scale_orig_type; /* Saved kbd_scale when opening Scale Browser */
    uint8_t  scale_orig_root; /* Saved kbd_scale_root when opening Scale Browser */
    bool     initialized;
    bool     boot_active;
    uint8_t  boot_frame;
    uint8_t  boot_bar_decay;
    uint8_t  boot_prev_y0;
    uint8_t  boot_prev_y1;
    uint32_t frame_count;
    uint8_t  edit_hold_frames;
    uint8_t  sound_hold_frames;
    uint8_t  prev_layout_mode;
    uint8_t  prev_preset;
    uint8_t  prev_edit_group;
    uint8_t  prev_group_voices;
    int8_t   prev_kbd_octave;
    bool     prev_in_group_menu;
    bool     prev_in_arp_menu;
    bool     prev_in_fx_menu;
    bool     prev_in_scope;
    uint8_t  prev_scope_page;
    uint8_t  prev_scope_zoom;
    uint16_t prev_scope_gain;
    bool     prev_scope_freeze;
    uint8_t  prev_scope_mode;
    bool     prev_in_preset_menu;
    uint8_t  prev_preset_cursor;
    uint8_t  prev_preset_bank;
    bool     prev_preset_load_grp;
    uint8_t  prev_preset_src_grp;
    bool     prev_preset_clear_armed;
    bool     prev_in_save_menu;
    uint32_t prev_save_sig;
    bool     prev_save_toast;
    uint32_t prev_preset_name_sig;
    bool     prev_in_scale_menu;
    uint8_t  prev_scale_cursor;
    uint8_t  prev_scale_root;
    uint8_t  prev_page;
    uint8_t  prev_subpage;
    uint8_t  prev_note;
    bool     prev_gate;
    uint8_t  prev_meter_w;
    uint8_t  prev_cpu_pct;
    uint32_t prev_pip_sig;
    uint8_t  prev_arp_step;
    uint8_t  prev_fx_gr;
    uint32_t prev_fx_sig;
    bool     prev_midi_clk_active;
    uint16_t prev_midi_clk_bpm;
    uint8_t  scope_prev_y0;
    uint8_t  scope_prev_y1;
    char     home_sub_prev[16];
    uint16_t home_sub_col_prev;
    uint8_t  vect_prev_y0;
    uint8_t  vect_prev_y1;
    uint8_t  vect_prev_hl;
    uint8_t  vect_prev_hr;
    int8_t   vect_prev_corr;
    uint8_t  vect_prev_width;
    uint8_t  spec_bins[ADRIFT_UI_SPEC_BINS];
    uint8_t  spec_peaks[ADRIFT_UI_SPEC_BINS];
    uint8_t  spec_waterfall[ADRIFT_UI_WATERFALL_ROWS][ADRIFT_UI_SPEC_BINS];
    uint8_t  spec_wf_head;
    uint8_t  spec_prev_bh[ADRIFT_UI_SPEC_BINS];
    uint8_t  spec_prev_ph[ADRIFT_UI_SPEC_BINS];
    uint8_t  spec_prev_crest[ADRIFT_UI_SPEC_BINS];
    uint8_t  spec_prev_crest_y0;
    adrift_stage_node_t stage_prev[ADRIFT_MAX_VOICES];
    uint8_t  poly_peaks[ADRIFT_MAX_VOICES];
    uint8_t  poly_prev_e1[ADRIFT_MAX_VOICES];
    uint8_t  poly_prev_bbd[ADRIFT_MAX_VOICES];
    uint8_t  poly_prev_e2[ADRIFT_MAX_VOICES];
    uint8_t  poly_prev_pk[ADRIFT_MAX_VOICES];
    uint8_t  poly_prev_fby[ADRIFT_MAX_VOICES];
    uint8_t  poly_prev_hdr[ADRIFT_MAX_VOICES];
    uint16_t poly_prev_foot[ADRIFT_MAX_VOICES];
    uint8_t  grp_peaks[ADRIFT_MAX_GROUPS];
    uint8_t  perf_prev_mw[ADRIFT_MAX_GROUPS];
    uint8_t  perf_prev_pk[ADRIFT_MAX_GROUPS];
    uint8_t  perf_prev_wl[ADRIFT_MAX_GROUPS];
    uint8_t  perf_prev_wr[ADRIFT_MAX_GROUPS];
    uint8_t  perf_prev_vol[ADRIFT_MAX_GROUPS];
    uint16_t perf_prev_arp[ADRIFT_MAX_GROUPS];
    uint16_t perf_prev_hdr[ADRIFT_MAX_GROUPS];
    uint16_t perf_prev_note[ADRIFT_MAX_GROUPS];
    int16_t  perf_prev_fx_mix[4];
    uint8_t  perf_prev_fx_env[4];
} adrift_ui_t;

void     adrift_ui_init(adrift_ui_t *ui);
void     adrift_ui_dismiss_boot(adrift_ui_t *ui);
uint16_t adrift_ui_group_accent_rgb565(uint8_t group_idx);

static inline uint16_t adrift_ui_voice_accent_rgb565(uint8_t group_idx) {
    return adrift_ui_group_accent_rgb565(group_idx);
}

void adrift_ui_step(adrift_ui_t *ui,
                    adrift_synth_t *synth,
                    const adrift_input_state_t *input,
                    uint16_t *fb,
                    adrift_dirty_t *dirty);
