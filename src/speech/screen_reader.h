/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "speech/klatt.h"
#include "speech/tts.h"
#include "synth/synth.h"
#include "ui/ui.h"

/* 27-key panel indices for screen-reader black-key chords (F3=0 .. G5=26):
 *   Hold Black Key 11 (F#5, idx 25) + press:
 *     Black Key 6 (F#4, idx 13) -> Toggle screen reader on/off
 *     Black Key 7 (G#4, idx 15) -> WHERE (speak full current screen & controls)
 *     Black Key 4 (C#4, idx 8)  -> Speech speed down (1..5)
 *     Black Key 5 (D#4, idx 10) -> Speech speed up   (1..5)
 *   NOTE: REC button is intentionally NEVER mapped to screen-reader functions. */
#define ADRIFT_SR_KEY_MOD_BK11    25u
#define ADRIFT_SR_KEY_TOGGLE_BK6  13u
#define ADRIFT_SR_KEY_WHERE_BK7   15u
#define ADRIFT_SR_KEY_SLOWER_BK4   8u
#define ADRIFT_SR_KEY_FASTER_BK5  10u

/* Settle delays (Rule 18: ~120 ms for knobs, ~200 ms for pages) */
#define ADRIFT_SR_KNOB_SETTLE_MS  120u
#define ADRIFT_SR_PAGE_SETTLE_MS  200u

#define ADRIFT_SR_QUEUE_CAP       6u

typedef enum {
    ADRIFT_SR_PRIO_QUEUE = 0, /* Queued background event (Rule 17: no interrupt) */
    ADRIFT_SR_PRIO_NOW   = 1, /* Direct user action answer (interrupts normal speech) */
    ADRIFT_SR_PRIO_KEEP  = 2  /* ANN_KEEP warning (heard whole; never cut off by NOW/QUEUE) */
} adrift_sr_prio_t;

typedef enum {
    ADRIFT_SR_CAT_ACTION = 0,
    ADRIFT_SR_CAT_SCREEN,
    ADRIFT_SR_CAT_READOUT,
    ADRIFT_SR_CAT_LIST,
    ADRIFT_SR_CAT_QUESTION,
    ADRIFT_SR_CAT_WARNING,
    ADRIFT_SR_CAT_WHERE,
    ADRIFT_SR_CAT_EVENT
} adrift_sr_cat_t;

/* Optional observer hook used by tests/dump_speech.c & unit tests to capture utterances */
typedef void (*adrift_sr_log_fn)(const char *context_tag,
                                 adrift_sr_cat_t category,
                                 adrift_sr_prio_t prio,
                                 const char *text,
                                 void *user_data);

typedef struct {
    char             text[ADRIFT_TTS_MAX_TEXT + 4u];
    adrift_sr_prio_t prio;
    adrift_sr_cat_t  cat;
} adrift_sr_item_t;

typedef struct {
    bool             enabled;
    bool             current_keep;
    volatile uint8_t producer_busy;
    uint16_t         duck_gain_q14; /* 16384 = 0 dB synth, 8192 = -6 dB ducked during speech */

    /* Panel 27-key chord state */
    uint32_t         held_keys_mask;
    bool             bk11_used_as_mod;

    /* Pending settle timers (Rule 18: ann_later) */
    bool             knob_pending;
    uint16_t         knob_timer_ms;
    char             knob_text[ADRIFT_TTS_MAX_TEXT + 4u];

    bool             page_pending;
    uint16_t         page_timer_ms;
    char             page_text[ADRIFT_TTS_MAX_TEXT + 4u];

    /* Tracked UI / background state for semantic transition announcements */
    bool             prev_midi_clk_active;
    uint8_t          prev_preset_section; /* 0=none, 1=actions, 2=factory, 3=user */

    /* Queued utterances */
    adrift_sr_item_t queue[ADRIFT_SR_QUEUE_CAP];
    uint8_t          q_rd;
    uint8_t          q_wr;
    uint8_t          q_count;

    /* Core TTS & Klatt formant synthesizer */
    adrift_tts_t     tts;
    adrift_klatt_t   klatt;

    /* Optional test/audit logging callback */
    adrift_sr_log_fn log_fn;
    void            *log_user;
    const char      *log_ctx;
} adrift_screen_reader_t;

void adrift_sr_init(adrift_screen_reader_t *sr);
void adrift_sr_set_logger(adrift_screen_reader_t *sr,
                          adrift_sr_log_fn fn,
                          void *user_data,
                          const char *default_ctx);
void adrift_sr_set_log_context(adrift_screen_reader_t *sr, const char *ctx);

void adrift_sr_set_enabled(adrift_screen_reader_t *sr,
                           bool en,
                           bool announce,
                           const adrift_ui_t *ui,
                           const adrift_synth_t *s);
void adrift_sr_toggle(adrift_screen_reader_t *sr,
                      const adrift_ui_t *ui,
                      const adrift_synth_t *s);
void adrift_sr_adjust_speed(adrift_screen_reader_t *sr, int delta);
void adrift_sr_speak_where(adrift_screen_reader_t *sr,
                           const adrift_ui_t *ui,
                           const adrift_synth_t *s);

/* Enqueue or speak an immediate utterance */
void adrift_sr_say(adrift_screen_reader_t *sr,
                   adrift_sr_prio_t prio,
                   adrift_sr_cat_t cat,
                   const char *text);

/* Schedule settled knob readout (~120 ms) or page announcement (~200 ms) */
void adrift_sr_later_knob(adrift_screen_reader_t *sr, const char *text);
void adrift_sr_later_page(adrift_screen_reader_t *sr, const char *text);

/* Advance settle timers & background event monitors (call once per UI tick with dt_ms ~ 16) */
void adrift_sr_tick(adrift_screen_reader_t *sr,
                    const adrift_ui_t *ui,
                    const adrift_synth_t *s,
                    uint32_t dt_ms);

/* Mix synthesized speech into stereo 44,118 Hz 16-bit or 24-bit Q23 line-out/headphone buffer ONLY. */
void adrift_sr_mix_line_out(adrift_screen_reader_t *sr,
                            int16_t *stereo_inout,
                            uint32_t frames);
void adrift_sr_mix_line_out_scaled(adrift_screen_reader_t *sr,
                                   int16_t *stereo_inout,
                                   uint32_t frames,
                                   int32_t master_gain_q15);
void adrift_sr_mix_line_out_q23(adrift_screen_reader_t *sr,
                                int32_t *stereo_q23_inout,
                                uint32_t frames);
void adrift_sr_mix_line_out_scaled_q23(adrift_screen_reader_t *sr,
                                       int32_t *stereo_q23_inout,
                                       uint32_t frames,
                                       int32_t master_gain_q15);

/* Process a 27-key panel event (key_idx 0..26) before note mapping.
 * Returns true if the event was consumed as a screen-reader chord (suppress Note-On).
 * Sets *out_release_bk11 = true when BK11 (25) was sounding and must send Note-Off. */
bool adrift_sr_handle_panel_key(adrift_screen_reader_t *sr,
                                const adrift_ui_t *ui,
                                const adrift_synth_t *s,
                                uint8_t key_idx,
                                bool down,
                                bool *out_release_bk11);

/* Semantic UI speech builders called from ui.c & ui_params.c */
void adrift_sr_announce_screen(adrift_screen_reader_t *sr,
                               const adrift_ui_t *ui,
                               const adrift_synth_t *s,
                               bool immediate);
void adrift_sr_announce_main_knob(adrift_screen_reader_t *sr,
                                  const adrift_ui_t *ui,
                                  const adrift_synth_t *s,
                                  uint8_t knob_0_to_3);
void adrift_sr_announce_group_knob(adrift_screen_reader_t *sr,
                                   const adrift_ui_t *ui,
                                   const adrift_synth_t *s,
                                   uint8_t knob_0_to_3);
void adrift_sr_announce_arp_knob(adrift_screen_reader_t *sr,
                                 const adrift_ui_t *ui,
                                 const adrift_synth_t *s,
                                 uint8_t knob_0_to_3);
void adrift_sr_announce_fx_knob(adrift_screen_reader_t *sr,
                                const adrift_ui_t *ui,
                                const adrift_synth_t *s,
                                uint8_t knob_0_to_3);
void adrift_sr_announce_home_knob(adrift_screen_reader_t *sr,
                                  const adrift_ui_t *ui,
                                  uint8_t knob_0_to_3);
void adrift_sr_announce_group_select(adrift_screen_reader_t *sr,
                                     const adrift_synth_t *s);
void adrift_sr_announce_octave(adrift_screen_reader_t *sr,
                               int8_t octave,
                               int delta_blocked);
void adrift_sr_announce_preset_row(adrift_screen_reader_t *sr,
                                   const adrift_ui_t *ui,
                                   const adrift_synth_t *s,
                                   bool entered_modal);
void adrift_sr_announce_preset_bank(adrift_screen_reader_t *sr,
                                    const adrift_ui_t *ui,
                                    const adrift_synth_t *s);
void adrift_sr_announce_preset_load_mode(adrift_screen_reader_t *sr,
                                         const adrift_ui_t *ui,
                                         const adrift_synth_t *s);
void adrift_sr_announce_preset_src_grp(adrift_screen_reader_t *sr,
                                       const adrift_ui_t *ui,
                                       bool blocked);
void adrift_sr_announce_preset_clear_arm(adrift_screen_reader_t *sr,
                                         const adrift_ui_t *ui,
                                         bool armed,
                                         bool blocked);
void adrift_sr_announce_preset_confirm(adrift_screen_reader_t *sr,
                                       uint8_t confirmed_logical_idx,
                                       bool was_group_load,
                                       uint8_t src_grp,
                                       uint8_t dst_grp,
                                       bool was_cleared,
                                       uint8_t cleared_u_slot);
void adrift_sr_announce_save_open(adrift_screen_reader_t *sr,
                                  const adrift_ui_t *ui);
void adrift_sr_announce_save_slot(adrift_screen_reader_t *sr,
                                  const adrift_ui_t *ui);
void adrift_sr_announce_save_cursor(adrift_screen_reader_t *sr,
                                    const adrift_ui_t *ui,
                                    bool settled_char_only);
void adrift_sr_announce_save_edit(adrift_screen_reader_t *sr,
                                  const adrift_ui_t *ui,
                                  const char *action_msg);
void adrift_sr_announce_save_result(adrift_screen_reader_t *sr,
                                    uint8_t u_slot,
                                    const char *saved_name,
                                    bool ok);
void adrift_sr_announce_scale_row(adrift_screen_reader_t *sr,
                                  const adrift_synth_t *s,
                                  bool entered_modal);
void adrift_sr_announce_scale_root(adrift_screen_reader_t *sr,
                                   const adrift_synth_t *s);
void adrift_sr_announce_scale_confirm(adrift_screen_reader_t *sr,
                                      const adrift_synth_t *s);
void adrift_sr_announce_unused_knob(adrift_screen_reader_t *sr,
                                    uint8_t knob_0_to_3,
                                    const char *hint);
