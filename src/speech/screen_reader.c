/* SPDX-License-Identifier: MPL-2.0 */
#include "speech/screen_reader.h"

/* ---- Freestanding String & Formatting Helpers ---- */

static size_t sr_strlen(const char *s) {
    size_t n = 0u;
    if (!s) return 0u;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static void sr_copy(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0u) return;
    size_t i = 0u;
    if (src) {
        while (src[i] != '\0' && i + 1u < cap) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

static void sr_append(char *dst, size_t *pos, size_t cap, const char *s) {
    if (!dst || !pos || !s || cap == 0u) return;
    while (*s != '\0' && *pos + 1u < cap) {
        dst[*pos] = *s++;
        (*pos)++;
    }
    dst[*pos] = '\0';
}

static void sr_append_ch(char *dst, size_t *pos, size_t cap, char c) {
    if (!dst || !pos || cap == 0u) return;
    if (*pos + 1u < cap) {
        dst[*pos] = c;
        (*pos)++;
        dst[*pos] = '\0';
    }
}

static void sr_append_u32(char *dst, size_t *pos, size_t cap, uint32_t val) {
    char tmp[12];
    uint32_t n = 0u;
    do {
        tmp[n++] = (char)('0' + (val % 10u));
        val /= 10u;
    } while (val > 0u && n < 11u);
    while (n > 0u) {
        sr_append_ch(dst, pos, cap, tmp[--n]);
    }
}

static void sr_append_signed(char *dst, size_t *pos, size_t cap, int32_t val, bool explicit_plus) {
    if (val < 0) {
        sr_append(dst, pos, cap, "minus ");
        sr_append_u32(dst, pos, cap, (uint32_t)(-val));
    } else if (val > 0 && explicit_plus) {
        sr_append(dst, pos, cap, "plus ");
        sr_append_u32(dst, pos, cap, (uint32_t)val);
    } else {
        sr_append_u32(dst, pos, cap, (uint32_t)val);
    }
}

/* Format frequency matching ui_draw.c fmt_hz ("20 hertz".."999 hertz", "1 point 2 kilohertz") */
static void sr_append_hz(char *dst, size_t *pos, size_t cap, int32_t hz) {
    if (hz < 0) hz = 0;
    if (hz < 1000) {
        sr_append_u32(dst, pos, cap, (uint32_t)hz);
        sr_append(dst, pos, cap, " hertz");
        return;
    }
    uint32_t khz_int  = (uint32_t)(hz / 1000);
    uint32_t khz_tent = (uint32_t)((hz % 1000) / 100);
    sr_append_u32(dst, pos, cap, khz_int);
    if (khz_tent > 0u) {
        sr_append(dst, pos, cap, " point ");
        sr_append_u32(dst, pos, cap, khz_tent);
    }
    sr_append(dst, pos, cap, " kilohertz");
}

/* Format duration matching ui_draw.c fmt_ms ("0 milliseconds".."999 milliseconds", "1 point 2 seconds") */
static void sr_append_ms(char *dst, size_t *pos, size_t cap, int32_t ms) {
    if (ms < 0) ms = 0;
    if (ms < 1000) {
        sr_append_u32(dst, pos, cap, (uint32_t)ms);
        sr_append(dst, pos, cap, (ms == 1) ? " millisecond" : " milliseconds");
        return;
    }
    uint32_t s_int  = (uint32_t)(ms / 1000);
    uint32_t s_tent = (uint32_t)((ms % 1000) / 100);
    sr_append_u32(dst, pos, cap, s_int);
    if (s_tent > 0u) {
        sr_append(dst, pos, cap, " point ");
        sr_append_u32(dst, pos, cap, s_tent);
    }
    sr_append(dst, pos, cap, (s_int == 1u && s_tent == 0u) ? " second" : " seconds");
}

/* Strip leading "01 " numeric prefix if present, trim trailing spaces, and expand via words.tsv */
static void sr_append_preset_title(char *dst, size_t *pos, size_t cap, const char *raw_name) {
    if (!raw_name) return;
    if (raw_name[0] >= '0' && raw_name[0] <= '9' &&
        raw_name[1] >= '0' && raw_name[1] <= '9' &&
        raw_name[2] == ' ') {
        raw_name += 3;
    }
    char trimmed[16];
    size_t n = 0u;
    while (raw_name[n] != '\0' && n < 12u) {
        char c = raw_name[n];
        /* Replace punctuation separators in custom user names with spaces so words speak cleanly */
        if (c == '-' || c == '_' || c == '/' || c == '.') {
            c = ' ';
        }
        trimmed[n] = c;
        n++;
    }
    while (n > 0u && trimmed[n - 1u] == ' ') {
        n--;
    }
    trimmed[n] = '\0';
    if (n == 0u) {
        sr_append(dst, pos, cap, "blank");
        return;
    }
    char expanded[96];
    if (adrift_tts_expand_label(trimmed, expanded, sizeof(expanded)) > 0u) {
        sr_append(dst, pos, cap, expanded);
    } else {
        sr_append(dst, pos, cap, "patch");
    }
}

/* Spoken musical root note ("C", "C sharp", ..., "A", "A sharp", "B") preserving capital A for Rule 3 */
static const char *sr_root_spoken(uint8_t root_idx) {
    static const char *const k_roots[12] = {
        "C", "C sharp", "D", "D sharp", "E", "F",
        "F sharp", "G", "G sharp", "A", "A sharp", "B"
    };
    return k_roots[root_idx % 12u];
}

static const char *sr_scale_spoken(uint8_t scale_idx) {
    static const char *const k_scales[ADRIFT_NUM_SCALES] = {
        "chromatic",
        "major",
        "natural minor",
        "minor pentatonic",
        "major pentatonic",
        "blues",
        "dorian",
        "phrygian",
        "lydian",
        "mixolydian",
        "locrian",
        "harmonic minor",
        "melodic minor",
        "phrygian dominant",
        "hirajoshi",
        "whole tone"
    };
    return k_scales[scale_idx % ADRIFT_NUM_SCALES];
}

static void sr_append_char_spoken(char *dst, size_t *pos, size_t cap, char ch) {
    if (ch == ' ' || ch == '\0') {
        sr_append(dst, pos, cap, "space");
    } else if (ch == '-') {
        sr_append(dst, pos, cap, "minus");
    } else if (ch == '+') {
        sr_append(dst, pos, cap, "plus");
    } else if (ch == '/') {
        sr_append(dst, pos, cap, "slash");
    } else if (ch == '.') {
        sr_append(dst, pos, cap, "dot");
    } else if (ch == '_') {
        sr_append(dst, pos, cap, "underscore");
    } else if (ch >= '0' && ch <= '9') {
        sr_append_ch(dst, pos, cap, ch);
    } else if (ch >= 'a' && ch <= 'z') {
        /* Uppercase single letter so Rule 3 keeps 'A' as letter/note EY1 ("A") */
        sr_append_ch(dst, pos, cap, (char)(ch - 'a' + 'A'));
    } else if (ch >= 'A' && ch <= 'Z') {
        sr_append_ch(dst, pos, cap, ch);
    } else {
        sr_append(dst, pos, cap, "space");
    }
}

/* ---- Queue & Scheduler Primitives ---- */

static bool sr_is_busy(const adrift_screen_reader_t *sr) {
    return sr->tts.active || adrift_klatt_is_speaking(&sr->klatt);
}

static void sr_start_utterance(adrift_screen_reader_t *sr,
                               adrift_sr_prio_t prio,
                               adrift_sr_cat_t cat,
                               const char *cleaned_text) {
    sr->current_keep = (prio == ADRIFT_SR_PRIO_KEEP);
    if (sr->log_fn) {
        const char *ctx = sr->log_ctx ? sr->log_ctx : "ui";
        sr->log_fn(ctx, cat, prio, cleaned_text, sr->log_user);
    }
    adrift_tts_start(&sr->tts, &sr->klatt, cleaned_text);
}

static void sr_enqueue_item(adrift_screen_reader_t *sr,
                            adrift_sr_prio_t prio,
                            adrift_sr_cat_t cat,
                            const char *cleaned_text) {
    /* If queue is full, never evict a KEEP warning; drop oldest non-KEEP or coalesce tail */
    if (sr->q_count >= ADRIFT_SR_QUEUE_CAP) {
        uint8_t tail = (uint8_t)((sr->q_wr + ADRIFT_SR_QUEUE_CAP - 1u) % ADRIFT_SR_QUEUE_CAP);
        if (sr->queue[tail].prio != ADRIFT_SR_PRIO_KEEP) {
            sr_copy(sr->queue[tail].text, sizeof(sr->queue[tail].text), cleaned_text);
            sr->queue[tail].prio = prio;
            sr->queue[tail].cat  = cat;
            return;
        }
        return;
    }
    adrift_sr_item_t *it = &sr->queue[sr->q_wr];
    sr_copy(it->text, sizeof(it->text), cleaned_text);
    it->prio = prio;
    it->cat  = cat;
    sr->q_wr = (uint8_t)((sr->q_wr + 1u) % ADRIFT_SR_QUEUE_CAP);
    sr->q_count++;
}

static void sr_clear_non_keep_queue(adrift_screen_reader_t *sr) {
    if (sr->q_count == 0u) return;
    adrift_sr_item_t kept[ADRIFT_SR_QUEUE_CAP];
    uint8_t n_kept = 0u;
    for (uint8_t i = 0u; i < sr->q_count; i++) {
        uint8_t idx = (uint8_t)((sr->q_rd + i) % ADRIFT_SR_QUEUE_CAP);
        if (sr->queue[idx].prio == ADRIFT_SR_PRIO_KEEP) {
            kept[n_kept++] = sr->queue[idx];
        }
    }
    sr->q_rd    = 0u;
    sr->q_wr    = n_kept % ADRIFT_SR_QUEUE_CAP;
    sr->q_count = n_kept;
    for (uint8_t i = 0u; i < n_kept; i++) {
        sr->queue[i] = kept[i];
    }
}

static void sr_pump_internal(adrift_screen_reader_t *sr) {
    if (sr->tts.active) {
        (void)adrift_tts_pump(&sr->tts, &sr->klatt);
    }
    if (!sr_is_busy(sr)) {
        sr->current_keep = false;
        if (sr->q_count > 0u) {
            adrift_sr_item_t next = sr->queue[sr->q_rd];
            sr->q_rd = (uint8_t)((sr->q_rd + 1u) % ADRIFT_SR_QUEUE_CAP);
            sr->q_count--;
            sr_start_utterance(sr, next.prio, next.cat, next.text);
        }
    }
}

void adrift_sr_init(adrift_screen_reader_t *sr) {
    if (!sr) return;
    uint8_t *raw = (uint8_t *)sr;
    for (size_t i = 0u; i < sizeof(*sr); i++) {
        raw[i] = 0u;
    }
    sr->enabled       = false;
    sr->duck_gain_q14 = 16384u;
    sr->log_ctx       = "ui";
    adrift_tts_init(&sr->tts);
    adrift_klatt_init(&sr->klatt);
}

void adrift_sr_set_logger(adrift_screen_reader_t *sr,
                          adrift_sr_log_fn fn,
                          void *user_data,
                          const char *default_ctx) {
    if (!sr) return;
    sr->log_fn   = fn;
    sr->log_user = user_data;
    sr->log_ctx  = default_ctx ? default_ctx : "ui";
}

void adrift_sr_set_log_context(adrift_screen_reader_t *sr, const char *ctx) {
    if (!sr) return;
    sr->log_ctx = ctx ? ctx : "ui";
}

void adrift_sr_say(adrift_screen_reader_t *sr,
                   adrift_sr_prio_t prio,
                   adrift_sr_cat_t cat,
                   const char *text) {
    if (!sr || !sr->enabled || !text || text[0] == '\0') {
        return;
    }

    char cleaned[ADRIFT_TTS_MAX_TEXT + 4u];
    if (adrift_tts_clean_say(text, cleaned, sizeof(cleaned)) == 0u) {
        return;
    }

    sr->producer_busy = 1u;
    __asm__ volatile("" ::: "memory");

    if (prio == ADRIFT_SR_PRIO_NOW || prio == ADRIFT_SR_PRIO_KEEP) {
        /* Direct user action cancels pending settle timers unless this is a knob settle itself */
        if (cat != ADRIFT_SR_CAT_READOUT) {
            sr->knob_pending  = false;
            sr->knob_timer_ms = 0u;
        }
        if (cat == ADRIFT_SR_CAT_SCREEN || cat == ADRIFT_SR_CAT_LIST ||
            cat == ADRIFT_SR_CAT_WHERE  || cat == ADRIFT_SR_CAT_QUESTION) {
            sr->page_pending  = false;
            sr->page_timer_ms = 0u;
        }

        if (!sr_is_busy(sr)) {
            sr_start_utterance(sr, prio, cat, cleaned);
        } else if (sr->current_keep) {
            /* Rule 15: ANN_KEEP warnings are heard whole and NEVER cut off */
            if (prio == ADRIFT_SR_PRIO_NOW) {
                sr_clear_non_keep_queue(sr);
            }
            sr_enqueue_item(sr, prio, cat, cleaned);
        } else {
            /* Interrupt normal speech cleanly */
            sr_clear_non_keep_queue(sr);
            if (sr->q_count > 0u) {
                /* Keep any queued warnings intact and play after them */
                adrift_tts_stop(&sr->tts);
                adrift_klatt_clear_queue(&sr->klatt);
                adrift_sr_item_t next = sr->queue[sr->q_rd];
                sr->q_rd = (uint8_t)((sr->q_rd + 1u) % ADRIFT_SR_QUEUE_CAP);
                sr->q_count--;
                sr_start_utterance(sr, next.prio, next.cat, next.text);
                sr_enqueue_item(sr, prio, cat, cleaned);
            } else {
                sr_start_utterance(sr, prio, cat, cleaned);
            }
        }
    } else {
        /* ADRIFT_SR_PRIO_QUEUE (Rule 17: background event does not interrupt) */
        if (!sr_is_busy(sr) && sr->q_count == 0u) {
            sr_start_utterance(sr, prio, cat, cleaned);
        } else {
            sr_enqueue_item(sr, prio, cat, cleaned);
        }
    }

    __asm__ volatile("" ::: "memory");
    sr->producer_busy = 0u;
}

void adrift_sr_later_knob(adrift_screen_reader_t *sr, const char *text) {
    if (!sr || !sr->enabled || !text || text[0] == '\0') {
        return;
    }
    sr_copy(sr->knob_text, sizeof(sr->knob_text), text);
    sr->knob_timer_ms = ADRIFT_SR_KNOB_SETTLE_MS;
    sr->knob_pending  = true;
}

void adrift_sr_later_page(adrift_screen_reader_t *sr, const char *text) {
    if (!sr || !sr->enabled || !text || text[0] == '\0') {
        return;
    }
    sr->knob_pending  = false;
    sr->knob_timer_ms = 0u;
    sr_copy(sr->page_text, sizeof(sr->page_text), text);
    sr->page_timer_ms = ADRIFT_SR_PAGE_SETTLE_MS;
    sr->page_pending  = true;
}

void adrift_sr_set_enabled(adrift_screen_reader_t *sr,
                           bool en,
                           bool announce,
                           const adrift_ui_t *ui,
                           const adrift_synth_t *s) {
    if (!sr) return;
    sr->knob_pending  = false;
    sr->knob_timer_ms = 0u;
    sr->page_pending  = false;
    sr->page_timer_ms = 0u;
    sr->q_rd          = 0u;
    sr->q_wr          = 0u;
    sr->q_count       = 0u;
    sr->current_keep  = false;

    if (en) {
        sr->enabled = true;
        if (s) {
            sr->prev_midi_clk_active = s->midi_clk_active;
        }
        if (announce) {
            if (ui && s) {
                char msg[ADRIFT_TTS_MAX_TEXT + 4u];
                size_t pos = 0u;
                sr_append(msg, &pos, sizeof(msg), "Screen reader on. ");
                /* Append concise current screen orientation */
                if (ui->in_save_menu) {
                    sr_append(msg, &pos, sizeof(msg), "Save preset.");
                } else if (ui->in_preset_menu) {
                    sr_append(msg, &pos, sizeof(msg), "Presets.");
                } else if (ui->in_scale_menu) {
                    sr_append(msg, &pos, sizeof(msg), "Scales.");
                } else if (ui->in_scope) {
                    static const char *const k_home[6] = {
                        "Oscilloscope.", "Vectorscope.", "Spectrum.",
                        "Soundstage.", "Polyphony.", "Performance."
                    };
                    sr_append(msg, &pos, sizeof(msg), k_home[ui->scope_page % 6u]);
                } else if (ui->in_group_menu) {
                    sr_append(msg, &pos, sizeof(msg), "Group settings.");
                } else if (ui->in_arp_menu) {
                    sr_append(msg, &pos, sizeof(msg), "Arpeggiator.");
                } else if (ui->in_fx_menu) {
                    sr_append(msg, &pos, sizeof(msg), "Effects.");
                } else {
                    static const char *const k_pg[5] = {
                        "Exciter.", "Envelope 1.", "Resonator.", "Envelope 2.", "Modulation."
                    };
                    sr_append(msg, &pos, sizeof(msg), k_pg[ui->page % 5u]);
                }
                adrift_sr_say(sr, ADRIFT_SR_PRIO_NOW, ADRIFT_SR_CAT_ACTION, msg);
            } else {
                adrift_sr_say(sr, ADRIFT_SR_PRIO_NOW, ADRIFT_SR_CAT_ACTION, "Screen reader on.");
            }
        }
    } else {
        if (announce) {
            /* Speak final confirmation before disabling subsequent UI announcements (Rule 14) */
            sr->enabled = true;
            adrift_sr_say(sr, ADRIFT_SR_PRIO_NOW, ADRIFT_SR_CAT_ACTION, "Screen reader off.");
            sr->enabled = false;
        } else {
            sr->enabled = false;
            adrift_tts_stop(&sr->tts);
            adrift_klatt_clear_queue(&sr->klatt);
        }
    }
}

void adrift_sr_toggle(adrift_screen_reader_t *sr,
                      const adrift_ui_t *ui,
                      const adrift_synth_t *s) {
    if (!sr) return;
    adrift_sr_set_enabled(sr, !sr->enabled, true, ui, s);
}

void adrift_sr_adjust_speed(adrift_screen_reader_t *sr, int delta) {
    if (!sr || !sr->enabled) return;
    int cur  = (int)adrift_klatt_get_speed(&sr->klatt);
    int next = cur + delta;
    if (next < 1) {
        adrift_sr_say(sr, ADRIFT_SR_PRIO_KEEP, ADRIFT_SR_CAT_WARNING,
                      "Cannot lower speech speed, already at 1.");
        return;
    }
    if (next > 5) {
        adrift_sr_say(sr, ADRIFT_SR_PRIO_KEEP, ADRIFT_SR_CAT_WARNING,
                      "Cannot raise speech speed, already at 5.");
        return;
    }
    adrift_klatt_set_speed(&sr->klatt, (uint8_t)next);
    char buf[32];
    size_t pos = 0u;
    sr_append(buf, &pos, sizeof(buf), "Speed ");
    sr_append_u32(buf, &pos, sizeof(buf), (uint32_t)next);
    sr_append_ch(buf, &pos, sizeof(buf), '.');
    adrift_sr_say(sr, ADRIFT_SR_PRIO_NOW, ADRIFT_SR_CAT_ACTION, buf);
}

bool adrift_sr_handle_panel_key(adrift_screen_reader_t *sr,
                                const adrift_ui_t *ui,
                                const adrift_synth_t *s,
                                uint8_t key_idx,
                                bool down,
                                bool *out_release_bk11) {
    if (out_release_bk11) {
        *out_release_bk11 = false;
    }
    if (!sr || key_idx >= 27u) {
        return false;
    }

    const uint32_t bit = (1u << key_idx);
    if (!down) {
        sr->held_keys_mask &= ~bit;
        if (key_idx == ADRIFT_SR_KEY_MOD_BK11) {
            bool was_mod = sr->bk11_used_as_mod;
            sr->bk11_used_as_mod = false;
            return was_mod;
        }
        return false;
    }

    sr->held_keys_mask |= bit;
    if (key_idx == ADRIFT_SR_KEY_MOD_BK11) {
        sr->bk11_used_as_mod = false;
        return false; /* Allow F#5 to play normally unless combined with a chord key */
    }

    /* Check if Black Key 11 (idx 25) is currently held */
    if ((sr->held_keys_mask & (1u << ADRIFT_SR_KEY_MOD_BK11)) == 0u) {
        return false;
    }

    if (key_idx == ADRIFT_SR_KEY_TOGGLE_BK6) {
        if (out_release_bk11) *out_release_bk11 = !sr->bk11_used_as_mod;
        sr->bk11_used_as_mod = true;
        adrift_sr_toggle(sr, ui, s);
        return true;
    }

    if (!sr->enabled) {
        return false;
    }

    if (key_idx == ADRIFT_SR_KEY_WHERE_BK7) {
        if (out_release_bk11) *out_release_bk11 = !sr->bk11_used_as_mod;
        sr->bk11_used_as_mod = true;
        adrift_sr_speak_where(sr, ui, s);
        return true;
    }
    if (key_idx == ADRIFT_SR_KEY_SLOWER_BK4) {
        if (out_release_bk11) *out_release_bk11 = !sr->bk11_used_as_mod;
        sr->bk11_used_as_mod = true;
        adrift_sr_adjust_speed(sr, -1);
        return true;
    }
    if (key_idx == ADRIFT_SR_KEY_FASTER_BK5) {
        if (out_release_bk11) *out_release_bk11 = !sr->bk11_used_as_mod;
        sr->bk11_used_as_mod = true;
        adrift_sr_adjust_speed(sr, 1);
        return true;
    }

    return false;
}

void adrift_sr_tick(adrift_screen_reader_t *sr,
                    const adrift_ui_t *ui,
                    const adrift_synth_t *s,
                    uint32_t dt_ms) {
    (void)ui;
    if (!sr) return;

    sr->producer_busy = 1u;
    __asm__ volatile("" ::: "memory");

    /* Stream remaining TTS words even if screen reader was just toggled off mid-confirmation */
    sr_pump_internal(sr);

    __asm__ volatile("" ::: "memory");
    sr->producer_busy = 0u;

    if (!sr->enabled) {
        if (s) sr->prev_midi_clk_active = s->midi_clk_active;
        return;
    }

    /* Rule 18: Page settle timer (~200 ms) */
    if (sr->page_pending) {
        if (dt_ms >= sr->page_timer_ms) {
            char msg[ADRIFT_TTS_MAX_TEXT + 4u];
            sr_copy(msg, sizeof(msg), sr->page_text);
            sr->page_pending  = false;
            sr->page_timer_ms = 0u;
            adrift_sr_say(sr, ADRIFT_SR_PRIO_NOW, ADRIFT_SR_CAT_SCREEN, msg);
        } else {
            sr->page_timer_ms = (uint16_t)(sr->page_timer_ms - dt_ms);
        }
    }

    /* Rule 18: Knob settle timer (~120 ms) */
    if (sr->knob_pending) {
        if (dt_ms >= sr->knob_timer_ms) {
            char msg[ADRIFT_TTS_MAX_TEXT + 4u];
            sr_copy(msg, sizeof(msg), sr->knob_text);
            sr->knob_pending  = false;
            sr->knob_timer_ms = 0u;
            adrift_sr_say(sr, ADRIFT_SR_PRIO_NOW, ADRIFT_SR_CAT_READOUT, msg);
        } else {
            sr->knob_timer_ms = (uint16_t)(sr->knob_timer_ms - dt_ms);
        }
    }

    /* Rule 17: Queued non-interrupting background MIDI clock lock/loss notification */
    if (s && s->midi_clk_active != sr->prev_midi_clk_active) {
        sr->prev_midi_clk_active = s->midi_clk_active;
        if (s->midi_clk_active) {
            char msg[64];
            size_t pos = 0u;
            sr_append(msg, &pos, sizeof(msg), "External clock locked, ");
            sr_append_u32(msg, &pos, sizeof(msg), (uint32_t)s->midi_clk_bpm);
            sr_append(msg, &pos, sizeof(msg), " beats per minute.");
            adrift_sr_say(sr, ADRIFT_SR_PRIO_QUEUE, ADRIFT_SR_CAT_EVENT, msg);
        } else {
            adrift_sr_say(sr, ADRIFT_SR_PRIO_QUEUE, ADRIFT_SR_CAT_EVENT,
                          "External clock stopped.");
        }
    }
}

void adrift_sr_mix_line_out_scaled(adrift_screen_reader_t *sr,
                                   int16_t *stereo_inout,
                                   uint32_t frames,
                                   int32_t master_gain_q15) {
    if (!sr || !stereo_inout || frames == 0u) {
        return;
    }

    /* Allow headless/single-threaded audio loops to stream multi-word TTS when UI tick is idle */
    if (sr->producer_busy == 0u && (sr->tts.active || sr->q_count > 0u)) {
        if (adrift_klatt_free_frames(&sr->klatt) >= 128u) {
            sr_pump_internal(sr);
        }
    }

    /* Fast path when completely silent and ducking envelope has returned to unity (16384) */
    if (!adrift_klatt_is_speaking(&sr->klatt) &&
        sr->klatt.tail_rem == 0u &&
        sr->duck_gain_q14 >= 16384u) {
        return;
    }

    int32_t mg_clamped = dsp_clamp_i32(master_gain_q15, 0, 32767);
    int32_t mg_q15     = (mg_clamped >= 32767) ? 32768 : mg_clamped; /* 0..32768 (unity at 32767) */

    uint32_t done = 0u;
    int16_t  sp_mono[64];
    while (done < frames) {
        uint32_t chunk = frames - done;
        if (chunk > 64u) chunk = 64u;

        bool speaking = adrift_klatt_render(&sr->klatt, sp_mono, chunk);
        uint16_t dg   = sr->duck_gain_q14;

        for (uint32_t i = 0u; i < chunk; i++) {
            /* Smoothly ramp synth ducking between 16384 (0 dB) and 8192 (-6 dB) */
            if (speaking) {
                if (dg > 8192u) {
                    dg = (dg > 8192u + 32u) ? (uint16_t)(dg - 32u) : 8192u;
                }
            } else {
                if (dg < 16384u) {
                    dg = (dg + 8u < 16384u) ? (uint16_t)(dg + 8u) : 16384u;
                }
            }

            uint32_t idx = (done + i) * 2u;
            int32_t  l   = ((int32_t)stereo_inout[idx]      * (int32_t)dg) >> 14;
            int32_t  r   = ((int32_t)stereo_inout[idx + 1u] * (int32_t)dg) >> 14;
            int32_t  sp  = ((int32_t)sp_mono[i] * mg_q15) >> 15;

            l += sp;
            r += sp;
            if (l > 32767)  l = 32767;
            if (l < -32768) l = -32768;
            if (r > 32767)  r = 32767;
            if (r < -32768) r = -32768;

            stereo_inout[idx]      = (int16_t)l;
            stereo_inout[idx + 1u] = (int16_t)r;
        }

        sr->duck_gain_q14 = dg;
        done += chunk;
    }
}

void adrift_sr_mix_line_out(adrift_screen_reader_t *sr,
                            int16_t *stereo_inout,
                            uint32_t frames) {
    adrift_sr_mix_line_out_scaled(sr, stereo_inout, frames, 32767);
}

/* ---- Parameter & Screen Semantic Speech Builders ---- */

static const char *sr_mod_dest_spoken(uint8_t dest) {
    static const char *const k_dests[ADRIFT_MOD_DEST_COUNT] = {
        "oscillator pitch",
        "morph",
        "pulse width",
        "input low pass",
        "bucket brigade pitch",
        "feedback low pass",
        "feedback amount",
        "all pitch",
        "pan",
        "stereo spread",
        "dry wet mix",
        "amplifier",
        "exciter mix",
        "exciter level",
        "sample and hold clock",
        "input resonance",
        "dispersion",
        "envelope 1 attack",
        "envelope 1 decay",
        "envelope 1 sustain",
        "envelope 1 release",
        "envelope 1 onset delay",
        "L F O 1 rate"
    };
    return k_dests[dest % ADRIFT_MOD_DEST_COUNT];
}

static const char *sr_lfo_wave_spoken(uint8_t w) {
    static const char *const k_waves[5] = {
        "sine", "triangle", "saw", "square", "sample and hold"
    };
    return k_waves[w % 5u];
}

static void sr_append_main_param_readout(char *dst,
                                         size_t *pos,
                                         size_t cap,
                                         const adrift_patch_t *p,
                                         uint8_t page,
                                         uint8_t enc) {
    switch (page % ADRIFT_UI_NUM_PAGES) {
        case 0u: /* Exciter */
            switch (enc) {
                case 0u:
                    sr_append(dst, pos, cap, "morph ");
                    sr_append_u32(dst, pos, cap, (uint32_t)((p->exc.wave_morph * 100) / 1023));
                    sr_append(dst, pos, cap, " percent");
                    break;
                case 1u:
                    sr_append(dst, pos, cap, "pulse width ");
                    sr_append_u32(dst, pos, cap, (uint32_t)(5 + (p->exc.pulse_width * 90) / 1023));
                    sr_append(dst, pos, cap, " percent");
                    break;
                case 2u:
                    sr_append(dst, pos, cap, "oscillator level ");
                    sr_append_u32(dst, pos, cap, (uint32_t)((p->exc.vco_level * 100) / 1023));
                    sr_append(dst, pos, cap, " percent");
                    break;
                case 3u:
                    sr_append(dst, pos, cap, "semitone ");
                    sr_append_signed(dst, pos, cap, p->exc.vco_semi, true);
                    break;
                case 4u:
                    sr_append(dst, pos, cap, "fine tune ");
                    sr_append_signed(dst, pos, cap, p->exc.vco_fine, true);
                    sr_append(dst, pos, cap,
                              (p->exc.vco_fine == 1 || p->exc.vco_fine == -1) ? " cent" : " cents");
                    break;
                case 5u:
                    sr_append(dst, pos, cap, "noise mix ");
                    sr_append_u32(dst, pos, cap, (uint32_t)((p->exc.noise_level * 100) / 1023));
                    sr_append(dst, pos, cap, " percent");
                    break;
                case 6u:
                    sr_append(dst, pos, cap, "sample and hold clock ");
                    if (p->exc.sh_rate >= 1000) {
                        sr_append(dst, pos, cap, "white noise");
                    } else {
                        sr_append_u32(dst, pos, cap, (uint32_t)((p->exc.sh_rate * 100) / 1000));
                        sr_append(dst, pos, cap, " percent");
                    }
                    break;
                case 7u:
                    sr_append(dst, pos, cap, "pluck ");
                    sr_append_u32(dst, pos, cap, (uint32_t)((p->exc.pluck * 100) / 1023));
                    sr_append(dst, pos, cap, " percent");
                    break;
                case 8u:
                    sr_append(dst, pos, cap, "analog drift ");
                    sr_append_u32(dst, pos, cap, (uint32_t)((p->exc.analog_drift * 100) / 1023));
                    sr_append(dst, pos, cap, " percent");
                    break;
                case 9u:
                default:
                    sr_append(dst, pos, cap, p->exc.vco_keytrack ? "keytrack on" : "keytrack fixed");
                    break;
            }
            break;

        case 1u: /* Envelope 1 */
            switch (enc) {
                case 0u:
                    sr_append(dst, pos, cap, "attack ");
                    sr_append_u32(dst, pos, cap, (uint32_t)((p->env1.attack * 100) / 1023));
                    sr_append(dst, pos, cap, " percent");
                    break;
                case 1u:
                    sr_append(dst, pos, cap, "decay ");
                    sr_append_u32(dst, pos, cap, (uint32_t)((p->env1.decay * 100) / 1023));
                    sr_append(dst, pos, cap, " percent");
                    break;
                case 2u:
                    sr_append(dst, pos, cap, "sustain ");
                    sr_append_u32(dst, pos, cap, (uint32_t)((p->env1.sustain * 100) / 1023));
                    sr_append(dst, pos, cap, " percent");
                    break;
                case 3u:
                    sr_append(dst, pos, cap, "release ");
                    sr_append_u32(dst, pos, cap, (uint32_t)((p->env1.release * 100) / 1023));
                    sr_append(dst, pos, cap, " percent");
                    break;
                case 4u: {
                    int32_t ms = ((int32_t)p->env1.onset_delay * (int32_t)p->env1.onset_delay * 2000) >> 20;
                    sr_append(dst, pos, cap, "onset delay ");
                    sr_append_ms(dst, pos, cap, ms);
                    break;
                }
                case 5u:
                    sr_append(dst, pos, cap, "velocity sensitivity ");
                    sr_append_u32(dst, pos, cap, (uint32_t)((p->env1.vel_sens * 100) / 1023));
                    sr_append(dst, pos, cap, " percent");
                    break;
                case 6u: {
                    static const char *const k_lm[3] = {
                        "loop off", "loop attack decay", "loop delay attack decay"
                    };
                    sr_append(dst, pos, cap, k_lm[p->env1.loop_mode % 3u]);
                    break;
                }
                case 7u:
                default:
                    if (p->env1.range == ADRIFT_ENV_RANGE_SHORT) {
                        sr_append(dst, pos, cap, "range short");
                    } else if (p->env1.range == ADRIFT_ENV_RANGE_LONG) {
                        sr_append(dst, pos, cap, "range long");
                    } else {
                        sr_append(dst, pos, cap, "range normal");
                    }
                    break;
            }
            break;

        case 2u: /* Resonator */
            switch (enc) {
                case 0u:
                    sr_append(dst, pos, cap, "input low pass ");
                    sr_append_hz(dst, pos, cap, dsp_cutoff_to_fc_q8(p->in_lpf_cutoff) >> 8);
                    break;
                case 1u:
                    sr_append(dst, pos, cap, "input resonance ");
                    sr_append_u32(dst, pos, cap, (uint32_t)((p->in_lpf_res * 100) / 1023));
                    sr_append(dst, pos, cap, " percent");
                    break;
                case 2u: {
                    int32_t pct = (p->bbd.feedback <= 890)
                                      ? ((p->bbd.feedback * 100) / 890)
                                      : (100 + ((p->bbd.feedback - 890) * 15) / 133);
                    sr_append(dst, pos, cap, "feedback amount ");
                    sr_append_u32(dst, pos, cap, (uint32_t)pct);
                    sr_append(dst, pos, cap, " percent");
                    break;
                }
                case 3u:
                    sr_append(dst, pos, cap, "feedback low pass ");
                    sr_append_hz(dst, pos, cap, dsp_cutoff_to_fc_q8(p->bbd.fb_lpf_cutoff) >> 8);
                    break;
                case 4u:
                    if (!p->bbd.invert) {
                        sr_append(dst, pos, cap, "invert off");
                    } else if (!p->bbd.invert_pitch_comp) {
                        sr_append(dst, pos, cap, "invert minus 1 octave");
                    } else {
                        sr_append(dst, pos, cap, "invert key pitch");
                    }
                    break;
                case 5u:
                    sr_append(dst, pos, cap, "glide ");
                    sr_append_u32(dst, pos, cap, (uint32_t)((p->bbd.glide * 100) / 1023));
                    sr_append(dst, pos, cap, " percent");
                    break;
                case 6u:
                    sr_append(dst, pos, cap, "dry wet mix ");
                    sr_append_u32(dst, pos, cap, (uint32_t)((p->dry_wet * 100) / 1023));
                    sr_append(dst, pos, cap, " percent");
                    break;
                case 7u:
                default:
                    sr_append(dst, pos, cap, "dispersion ");
                    sr_append_u32(dst, pos, cap, (uint32_t)((p->bbd.dispersion * 100) / 1023));
                    sr_append(dst, pos, cap, " percent");
                    break;
            }
            break;

        case 3u: /* Envelope 2 */
            switch (enc) {
                case 0u:
                    sr_append(dst, pos, cap, "attack ");
                    sr_append_u32(dst, pos, cap, (uint32_t)((p->env2.attack * 100) / 1023));
                    sr_append(dst, pos, cap, " percent");
                    break;
                case 1u:
                    sr_append(dst, pos, cap, "decay ");
                    sr_append_u32(dst, pos, cap, (uint32_t)((p->env2.decay * 100) / 1023));
                    sr_append(dst, pos, cap, " percent");
                    break;
                case 2u:
                    sr_append(dst, pos, cap, "sustain ");
                    sr_append_u32(dst, pos, cap, (uint32_t)((p->env2.sustain * 100) / 1023));
                    sr_append(dst, pos, cap, " percent");
                    break;
                case 3u:
                    sr_append(dst, pos, cap, "release ");
                    sr_append_u32(dst, pos, cap, (uint32_t)((p->env2.release * 100) / 1023));
                    sr_append(dst, pos, cap, " percent");
                    break;
                case 4u: {
                    int32_t ms = ((int32_t)p->env2.onset_delay * (int32_t)p->env2.onset_delay * 2000) >> 20;
                    sr_append(dst, pos, cap, "onset delay ");
                    sr_append_ms(dst, pos, cap, ms);
                    break;
                }
                case 5u:
                    sr_append(dst, pos, cap, "velocity sensitivity ");
                    sr_append_u32(dst, pos, cap, (uint32_t)((p->env2.vel_sens * 100) / 1023));
                    sr_append(dst, pos, cap, " percent");
                    break;
                case 6u:
                default:
                    sr_append(dst, pos, cap, p->env2.drone_hold ? "drone hold" : "drone gated");
                    break;
            }
            break;

        case 4u: /* Modulation */
        default:
            switch (enc) {
                case 0u:
                    sr_append(dst, pos, cap, "L F O 1 wave ");
                    sr_append(dst, pos, cap, sr_lfo_wave_spoken(p->lfo_wave));
                    break;
                case 1u:
                    sr_append(dst, pos, cap, "L F O 1 rate ");
                    sr_append_u32(dst, pos, cap, (uint32_t)((p->lfo_rate * 100) / 1023));
                    sr_append(dst, pos, cap, " percent");
                    break;
                case 2u:
                    sr_append(dst, pos, cap, "L F O 1 depth ");
                    sr_append_signed(dst, pos, cap, (p->lfo_depth * 100) / 512, true);
                    sr_append(dst, pos, cap, " percent");
                    break;
                case 3u: {
                    uint8_t d1 = (p->lfo_dest < ADRIFT_LFO1_DEST_COUNT)
                                     ? p->lfo_dest
                                     : (uint8_t)(ADRIFT_LFO1_DEST_COUNT - 1u);
                    sr_append(dst, pos, cap, "L F O 1 destination ");
                    sr_append(dst, pos, cap, sr_mod_dest_spoken(d1));
                    break;
                }
                case 4u:
                    sr_append(dst, pos, cap, "L F O 2 wave ");
                    sr_append(dst, pos, cap, sr_lfo_wave_spoken(p->lfo2_wave));
                    break;
                case 5u:
                    sr_append(dst, pos, cap, "L F O 2 rate ");
                    sr_append_u32(dst, pos, cap, (uint32_t)((p->lfo2_rate * 100) / 1023));
                    sr_append(dst, pos, cap, " percent");
                    break;
                case 6u:
                    sr_append(dst, pos, cap, "L F O 2 depth ");
                    sr_append_signed(dst, pos, cap, (p->lfo2_depth * 100) / 512, true);
                    sr_append(dst, pos, cap, " percent");
                    break;
                case 7u:
                    sr_append(dst, pos, cap, "L F O 2 destination ");
                    sr_append(dst, pos, cap, sr_mod_dest_spoken(p->lfo2_dest));
                    break;
                case 8u:
                    sr_append(dst, pos, cap, "L F O 3 wave ");
                    sr_append(dst, pos, cap, sr_lfo_wave_spoken(p->lfo3_wave));
                    break;
                case 9u:
                    sr_append(dst, pos, cap, "L F O 3 rate ");
                    sr_append_u32(dst, pos, cap, (uint32_t)((p->lfo3_rate * 100) / 1023));
                    sr_append(dst, pos, cap, " percent");
                    break;
                case 10u:
                    sr_append(dst, pos, cap, "L F O 3 depth ");
                    sr_append_signed(dst, pos, cap, (p->lfo3_depth * 100) / 512, true);
                    sr_append(dst, pos, cap, " percent");
                    break;
                case 11u:
                    sr_append(dst, pos, cap, "L F O 3 destination ");
                    sr_append(dst, pos, cap, sr_mod_dest_spoken(p->lfo3_dest));
                    break;
                case 12u:
                default:
                    sr_append(dst, pos, cap, "mod wheel depth ");
                    sr_append_u32(dst, pos, cap, (uint32_t)((p->mw_depth * 100) / 1023));
                    sr_append(dst, pos, cap, " percent");
                    break;
            }
            break;
    }
}

static void sr_append_group_param_readout(char *dst,
                                          size_t *pos,
                                          size_t cap,
                                          const adrift_synth_t *s,
                                          uint8_t enc) {
    const adrift_group_t *gp = adrift_synth_active_group(s);
    switch (enc) {
        case 0u:
            if (gp->num_voices == 0u) {
                sr_append(dst, pos, cap, "voices off");
            } else {
                sr_append(dst, pos, cap, "voices ");
                sr_append_u32(dst, pos, cap, gp->num_voices);
            }
            break;
        case 1u: {
            static const char *const k_assign[3] = {
                "assign polyphonic", "assign unison", "assign super"
            };
            sr_append(dst, pos, cap, k_assign[gp->assign_mode % 3u]);
            break;
        }
        case 2u:
            sr_append(dst, pos, cap, "stereo spread ");
            sr_append_u32(dst, pos, cap, (uint32_t)((gp->stereo_spread * 100) / 1023));
            sr_append(dst, pos, cap, " percent");
            break;
        case 3u: {
            int32_t c = adrift_detune_to_cents(gp->unison_detune);
            sr_append(dst, pos, cap, "detune ");
            sr_append_u32(dst, pos, cap, (uint32_t)c);
            sr_append(dst, pos, cap, (c == 1) ? " cent" : " cents");
            break;
        }
        case 4u:
            sr_append(dst, pos, cap, "midi channel ");
            sr_append_u32(dst, pos, cap, gp->midi_channel);
            break;
        case 5u: {
            int32_t pan_clamped = dsp_clamp_i32(gp->pan, -512, 512);
            int32_t pct         = (pan_clamped * 100) / 512;
            if (pct < 0) {
                sr_append(dst, pos, cap, "pan left ");
                sr_append_u32(dst, pos, cap, (uint32_t)(-pct));
                sr_append(dst, pos, cap, " percent");
            } else if (pct > 0) {
                sr_append(dst, pos, cap, "pan right ");
                sr_append_u32(dst, pos, cap, (uint32_t)pct);
                sr_append(dst, pos, cap, " percent");
            } else {
                sr_append(dst, pos, cap, "pan center");
            }
            break;
        }
        case 6u:
            sr_append(dst, pos, cap, gp->patch.legato ? "legato on" : "legato retrigger");
            break;
        case 7u:
        default:
            sr_append(dst, pos, cap, "group level ");
            sr_append_u32(dst, pos, cap, (uint32_t)((gp->patch.master_vol * 100) / 1023));
            sr_append(dst, pos, cap, " percent");
            break;
    }
}

static void sr_append_arp_param_readout(char *dst,
                                        size_t *pos,
                                        size_t cap,
                                        const adrift_synth_t *s,
                                        uint8_t enc) {
    const adrift_arp_params_t *ap = &adrift_synth_active_group(s)->patch.arp;
    switch (enc) {
        case 0u: {
            static const char *const k_modes[ADRIFT_ARP_MODE_COUNT] = {
                "algorithm off",
                "algorithm up",
                "algorithm down",
                "algorithm up down",
                "algorithm down up",
                "algorithm up and down",
                "algorithm converge",
                "algorithm diverge",
                "algorithm random",
                "algorithm order",
                "algorithm chord"
            };
            sr_append(dst, pos, cap, k_modes[ap->mode % ADRIFT_ARP_MODE_COUNT]);
            break;
        }
        case 1u: {
            int32_t rng = dsp_clamp_i32(ap->range, 1, 4);
            sr_append(dst, pos, cap, "range ");
            sr_append_u32(dst, pos, cap, (uint32_t)rng);
            sr_append(dst, pos, cap, (rng == 1) ? " octave" : " octaves");
            break;
        }
        case 2u:
            if (s->midi_clk_active) {
                int32_t bpm = dsp_clamp_i32((int32_t)s->midi_clk_bpm, 30, 300);
                sr_append(dst, pos, cap, "tempo external ");
                sr_append_u32(dst, pos, cap, (uint32_t)bpm);
                sr_append(dst, pos, cap, " beats per minute");
            } else {
                int32_t bpm = dsp_clamp_i32(ap->rate_bpm, 40, 240);
                sr_append(dst, pos, cap, "tempo ");
                sr_append_u32(dst, pos, cap, (uint32_t)bpm);
                sr_append(dst, pos, cap, " beats per minute");
            }
            break;
        case 3u: {
            int32_t gt = dsp_clamp_i32(ap->gate, 5, 100);
            sr_append(dst, pos, cap, "gate ");
            sr_append_u32(dst, pos, cap, (uint32_t)gt);
            sr_append(dst, pos, cap, " percent");
            break;
        }
        case 4u:
            if (ap->velocity == 0u) {
                sr_append(dst, pos, cap, "velocity key");
            } else {
                sr_append(dst, pos, cap, "velocity ");
                sr_append_u32(dst, pos, cap, ap->velocity);
            }
            break;
        case 5u:
            sr_append(dst, pos, cap, ap->latch ? "latch on" : "latch off");
            break;
        case 6u: {
            int32_t sw = dsp_clamp_i32(ap->swing, 0, 100);
            sr_append(dst, pos, cap, "swing ");
            sr_append_u32(dst, pos, cap, (uint32_t)sw);
            sr_append(dst, pos, cap, " percent");
            break;
        }
        case 7u:
        default: {
            static const char *const k_ratchets[ADRIFT_RATCHET_COUNT] = {
                "ratchet 1 time",
                "ratchet 2 times",
                "ratchet 3 times",
                "ratchet 4 times",
                "ratchet 1 to 2 times",
                "ratchet 1 to 3 times",
                "ratchet 1 to 4 times",
                "ratchet random"
            };
            sr_append(dst, pos, cap, k_ratchets[ap->ratchet % ADRIFT_RATCHET_COUNT]);
            break;
        }
    }
}

static void sr_append_fx_param_readout(char *dst,
                                       size_t *pos,
                                       size_t cap,
                                       const adrift_synth_t *s,
                                       uint8_t enc) {
    const adrift_fx_params_t *fp = &s->fx_params;
    switch (enc) {
        case 0u:
            sr_append(dst, pos, cap, "chorus ");
            sr_append_u32(dst, pos, cap, (uint32_t)((fp->chorus_mix * 100) / 1023));
            sr_append(dst, pos, cap, " percent");
            break;
        case 1u:
            sr_append(dst, pos, cap, "delay time ");
            if (s->midi_clk_active) {
                static const char *const k_subdiv_spoken[8] = {
                    "thirty second",
                    "sixteenth triplet",
                    "sixteenth",
                    "eighth triplet",
                    "dotted sixteenth",
                    "eighth",
                    "dotted eighth",
                    "quarter"
                };
                int32_t dt = dsp_clamp_i32(fp->delay_time, 0, 1023);
                sr_append(dst, pos, cap, k_subdiv_spoken[(dt * 8) >> 10]);
            } else {
                sr_append_ms(dst, pos, cap, (int32_t)adrift_fx_delay_time_ms(fp->delay_time));
            }
            break;
        case 2u:
            sr_append(dst, pos, cap, "delay feedback ");
            sr_append_u32(dst, pos, cap, (uint32_t)((fp->delay_fb * 92) / 1023));
            sr_append(dst, pos, cap, " percent");
            break;
        case 3u:
            sr_append(dst, pos, cap, "delay mix ");
            sr_append_u32(dst, pos, cap, (uint32_t)((fp->delay_mix * 100) / 1023));
            sr_append(dst, pos, cap, " percent");
            break;
        case 4u:
            sr_append(dst, pos, cap, "reverb size ");
            sr_append_u32(dst, pos, cap, (uint32_t)((fp->reverb_size * 100) / 1023));
            sr_append(dst, pos, cap, " percent");
            break;
        case 5u:
            sr_append(dst, pos, cap, "reverb damping ");
            sr_append_u32(dst, pos, cap, (uint32_t)((fp->reverb_damp * 100) / 1023));
            sr_append(dst, pos, cap, " percent");
            break;
        case 6u:
            sr_append(dst, pos, cap, "shimmer ");
            sr_append_u32(dst, pos, cap, (uint32_t)((fp->reverb_shimmer * 100) / 1023));
            sr_append(dst, pos, cap, " percent");
            break;
        case 7u:
            sr_append(dst, pos, cap, "reverb mix ");
            sr_append_u32(dst, pos, cap, (uint32_t)((fp->reverb_mix * 100) / 1023));
            sr_append(dst, pos, cap, " percent");
            break;
        case 8u:
            sr_append(dst, pos, cap, "compressor mix ");
            sr_append_u32(dst, pos, cap, (uint32_t)((fp->comp_mix * 100) / 1023));
            sr_append(dst, pos, cap, " percent");
            break;
        case 9u: {
            int32_t db = adrift_fx_comp_thresh_db(fp->comp_thresh);
            sr_append(dst, pos, cap, "threshold ");
            sr_append_signed(dst, pos, cap, db, false);
            sr_append(dst, pos, cap, (db == -1 || db == 1) ? " decibel" : " decibels");
            break;
        }
        case 10u:
            sr_append(dst, pos, cap, "compressor attack ");
            sr_append_ms(dst, pos, cap, (int32_t)adrift_fx_comp_attack_ms(fp->comp_attack));
            break;
        case 11u:
        default: {
            int32_t db = (fp->comp_makeup * 12 + 511) >> 10;
            sr_append(dst, pos, cap, "makeup gain ");
            sr_append_signed(dst, pos, cap, db, true);
            sr_append(dst, pos, cap, (db == 1) ? " decibel" : " decibels");
            break;
        }
    }
}

static const char *sr_home_mode_spoken(uint8_t scope_page, uint8_t mode) {
    static const char *const k_modes[6][3] = {
        { "mono", "dual", "fill" },
        { "goniometer", "X Y", "dots" },
        { "split", "bars", "waterfall" },
        { "field", "vectors", "harmonic" },
        { "triple", "bucket brigade", "envelopes" },
        { "dashboard", "arpeggiator", "mixer" }
    };
    return k_modes[scope_page % 6u][mode % 3u];
}

static void sr_append_home_param_readout(char *dst,
                                         size_t *pos,
                                         size_t cap,
                                         const adrift_ui_t *ui,
                                         uint8_t knob_0_to_3) {
    uint8_t sp = ui->scope_page % ADRIFT_UI_NUM_HOME_PAGES;
    switch (knob_0_to_3 & 3u) {
        case 0u: {
            uint32_t z = 1u << (ui->scope_zoom & 3u);
            sr_append(dst, pos, cap, "zoom ");
            sr_append_u32(dst, pos, cap, z);
            sr_append(dst, pos, cap, (z == 1u) ? " time" : " times");
            break;
        }
        case 1u:
            sr_append(dst, pos, cap, "gain ");
            sr_append_u32(dst, pos, cap, ui->scope_gain);
            sr_append(dst, pos, cap, " percent");
            break;
        case 2u:
            sr_append(dst, pos, cap, ui->scope_freeze ? "freeze on" : "freeze off");
            break;
        case 3u:
        default:
            sr_append(dst, pos, cap, "display ");
            sr_append(dst, pos, cap, sr_home_mode_spoken(sp, ui->scope_mode[sp]));
            break;
    }
}

/* ---- Screen & Page Announcements (Rules 8, 11, 12, 13, 18) ---- */

void adrift_sr_announce_screen(adrift_screen_reader_t *sr,
                               const adrift_ui_t *ui,
                               const adrift_synth_t *s,
                               bool immediate) {
    (void)s;
    if (!sr || !sr->enabled || !ui) return;

    char msg[ADRIFT_TTS_MAX_TEXT + 4u];
    size_t pos = 0u;

    if (ui->in_scope) {
        static const char *const k_home_names[6] = {
            "Oscilloscope", "Vectorscope", "Spectrum",
            "Soundstage", "Polyphony", "Performance"
        };
        uint8_t sp = ui->scope_page % 6u;
        sr_append(msg, &pos, sizeof(msg), k_home_names[sp]);
        sr_append(msg, &pos, sizeof(msg), ", page ");
        sr_append_u32(msg, &pos, sizeof(msg), (uint32_t)sp + 1u);
        sr_append(msg, &pos, sizeof(msg), " of 6. Zoom, gain, freeze, display.");
    } else if (ui->in_group_menu) {
        uint8_t sub = ui->subpage & 1u;
        if (sub == 0u) {
            sr_append(msg, &pos, sizeof(msg),
                      "Group settings, page 1 of 2. Voices, assign, stereo spread, detune.");
        } else {
            sr_append(msg, &pos, sizeof(msg),
                      "Group settings, page 2 of 2. Midi channel, pan, legato, group level.");
        }
    } else if (ui->in_arp_menu) {
        uint8_t sub = ui->subpage & 1u;
        if (sub == 0u) {
            sr_append(msg, &pos, sizeof(msg),
                      "Arpeggiator, page 1 of 2. Algorithm, range, tempo, gate.");
        } else {
            sr_append(msg, &pos, sizeof(msg),
                      "Arpeggiator, page 2 of 2. Velocity, latch, swing, ratchet.");
        }
    } else if (ui->in_fx_menu) {
        uint8_t sub = ui->subpage % 3u;
        if (sub == 0u) {
            sr_append(msg, &pos, sizeof(msg),
                      "Effects, page 1 of 3. Chorus, delay time, delay feedback, delay mix.");
        } else if (sub == 1u) {
            sr_append(msg, &pos, sizeof(msg),
                      "Effects, page 2 of 3. Reverb size, reverb damping, shimmer, reverb mix.");
        } else {
            sr_append(msg, &pos, sizeof(msg),
                      "Effects, page 3 of 3. Compressor mix, threshold, compressor attack, makeup gain.");
        }
    } else {
        switch (ui->page % ADRIFT_UI_NUM_PAGES) {
            case 0u: {
                uint8_t sub = ui->subpage % 3u;
                if (sub == 0u) {
                    sr_append(msg, &pos, sizeof(msg),
                              "Exciter, page 1 of 3. Morph, pulse width, oscillator level, semitone.");
                } else if (sub == 1u) {
                    sr_append(msg, &pos, sizeof(msg),
                              "Exciter, page 2 of 3. Fine tune, noise mix, sample and hold clock, pluck.");
                } else {
                    sr_append(msg, &pos, sizeof(msg),
                              "Exciter, page 3 of 3. Analog drift, keytrack.");
                }
                break;
            }
            case 1u:
                if ((ui->subpage & 1u) == 0u) {
                    sr_append(msg, &pos, sizeof(msg),
                              "Envelope 1, page 1 of 2. Attack, decay, sustain, release.");
                } else {
                    sr_append(msg, &pos, sizeof(msg),
                              "Envelope 1, page 2 of 2. Onset delay, velocity sensitivity, loop, range.");
                }
                break;
            case 2u:
                if ((ui->subpage & 1u) == 0u) {
                    sr_append(msg, &pos, sizeof(msg),
                              "Resonator, page 1 of 2. Input low pass, input resonance, feedback amount, feedback low pass.");
                } else {
                    sr_append(msg, &pos, sizeof(msg),
                              "Resonator, page 2 of 2. Invert, glide, dry wet mix, dispersion.");
                }
                break;
            case 3u:
                if ((ui->subpage & 1u) == 0u) {
                    sr_append(msg, &pos, sizeof(msg),
                              "Envelope 2, page 1 of 2. Attack, decay, sustain, release.");
                } else {
                    sr_append(msg, &pos, sizeof(msg),
                              "Envelope 2, page 2 of 2. Onset delay, velocity sensitivity, drone.");
                }
                break;
            case 4u:
            default: {
                uint8_t sub = ui->subpage % 4u;
                if (sub == 0u) {
                    sr_append(msg, &pos, sizeof(msg),
                              "Modulation, page 1 of 4. L F O 1 wave, rate, depth, destination.");
                } else if (sub == 1u) {
                    sr_append(msg, &pos, sizeof(msg),
                              "Modulation, page 2 of 4. L F O 2 wave, rate, depth, destination.");
                } else if (sub == 2u) {
                    sr_append(msg, &pos, sizeof(msg),
                              "Modulation, page 3 of 4. L F O 3 wave, rate, depth, destination.");
                } else {
                    sr_append(msg, &pos, sizeof(msg),
                              "Modulation, page 4 of 4. Mod wheel depth.");
                }
                break;
            }
        }
    }

    if (immediate) {
        adrift_sr_say(sr, ADRIFT_SR_PRIO_NOW, ADRIFT_SR_CAT_SCREEN, msg);
    } else {
        adrift_sr_later_page(sr, msg);
    }
}

/* ---- Knob Readouts (Settled ~120 ms per Rule 18) ---- */

void adrift_sr_announce_main_knob(adrift_screen_reader_t *sr,
                                  const adrift_ui_t *ui,
                                  const adrift_synth_t *s,
                                  uint8_t knob_0_to_3) {
    if (!sr || !sr->enabled || !ui || !s) return;
    uint8_t pg  = ui->page % ADRIFT_UI_NUM_PAGES;
    uint8_t nsb = (pg == 0u) ? 3u : ((pg == 4u) ? 4u : 2u);
    uint8_t enc = (uint8_t)((ui->subpage % nsb) * 4u + (knob_0_to_3 & 3u));

    char msg[96];
    size_t pos = 0u;
    sr_append_main_param_readout(msg, &pos, sizeof(msg), adrift_synth_view_patch(s), pg, enc);
    sr_append_ch(msg, &pos, sizeof(msg), '.');
    adrift_sr_later_knob(sr, msg);
}

void adrift_sr_announce_group_knob(adrift_screen_reader_t *sr,
                                   const adrift_ui_t *ui,
                                   const adrift_synth_t *s,
                                   uint8_t knob_0_to_3) {
    if (!sr || !sr->enabled || !ui || !s) return;
    uint8_t enc = (uint8_t)((ui->subpage & 1u) * 4u + (knob_0_to_3 & 3u));
    char msg[96];
    size_t pos = 0u;
    sr_append_group_param_readout(msg, &pos, sizeof(msg), s, enc);
    sr_append_ch(msg, &pos, sizeof(msg), '.');
    adrift_sr_later_knob(sr, msg);
}

void adrift_sr_announce_arp_knob(adrift_screen_reader_t *sr,
                                 const adrift_ui_t *ui,
                                 const adrift_synth_t *s,
                                 uint8_t knob_0_to_3) {
    if (!sr || !sr->enabled || !ui || !s) return;
    uint8_t enc = (uint8_t)((ui->subpage & 1u) * 4u + (knob_0_to_3 & 3u));
    char msg[96];
    size_t pos = 0u;
    sr_append_arp_param_readout(msg, &pos, sizeof(msg), s, enc);
    sr_append_ch(msg, &pos, sizeof(msg), '.');
    adrift_sr_later_knob(sr, msg);
}

void adrift_sr_announce_fx_knob(adrift_screen_reader_t *sr,
                                const adrift_ui_t *ui,
                                const adrift_synth_t *s,
                                uint8_t knob_0_to_3) {
    if (!sr || !sr->enabled || !ui || !s) return;
    uint8_t enc = (uint8_t)((ui->subpage % 3u) * 4u + (knob_0_to_3 & 3u));
    char msg[96];
    size_t pos = 0u;
    sr_append_fx_param_readout(msg, &pos, sizeof(msg), s, enc);
    sr_append_ch(msg, &pos, sizeof(msg), '.');
    adrift_sr_later_knob(sr, msg);
}

void adrift_sr_announce_home_knob(adrift_screen_reader_t *sr,
                                  const adrift_ui_t *ui,
                                  uint8_t knob_0_to_3) {
    if (!sr || !sr->enabled || !ui) return;
    char msg[96];
    size_t pos = 0u;
    sr_append_home_param_readout(msg, &pos, sizeof(msg), ui, knob_0_to_3);
    sr_append_ch(msg, &pos, sizeof(msg), '.');
    adrift_sr_later_knob(sr, msg);
}

void adrift_sr_announce_unused_knob(adrift_screen_reader_t *sr,
                                    uint8_t knob_0_to_3,
                                    const char *hint) {
    if (!sr || !sr->enabled) return;
    char msg[ADRIFT_TTS_MAX_TEXT + 4u];
    size_t pos = 0u;
    sr_append(msg, &pos, sizeof(msg), "Knob ");
    sr_append_u32(msg, &pos, sizeof(msg), (uint32_t)(knob_0_to_3 & 3u) + 1u);
    sr_append(msg, &pos, sizeof(msg), " is not used on this page.");
    if (hint && hint[0] != '\0') {
        sr_append_ch(msg, &pos, sizeof(msg), ' ');
        sr_append(msg, &pos, sizeof(msg), hint);
    }
    adrift_sr_say(sr, ADRIFT_SR_PRIO_KEEP, ADRIFT_SR_CAT_WARNING, msg);
}

/* ---- Group Select & Octave Actions ---- */

void adrift_sr_announce_group_select(adrift_screen_reader_t *sr,
                                     const adrift_synth_t *s) {
    if (!sr || !sr->enabled || !s) return;
    uint8_t g = s->edit_group % ADRIFT_MAX_GROUPS;
    const adrift_group_t *gp = &s->groups[g];

    char msg[128];
    size_t pos = 0u;
    sr_append(msg, &pos, sizeof(msg), "Group ");
    sr_append_u32(msg, &pos, sizeof(msg), (uint32_t)g + 1u);
    sr_append(msg, &pos, sizeof(msg), ", ");
    if (gp->num_voices == 0u) {
        sr_append(msg, &pos, sizeof(msg), "off.");
    } else {
        sr_append_u32(msg, &pos, sizeof(msg), gp->num_voices);
        sr_append(msg, &pos, sizeof(msg), (gp->num_voices == 1u) ? " voice, " : " voices, ");
        sr_append_preset_title(msg, &pos, sizeof(msg), gp->patch.name);
        sr_append_ch(msg, &pos, sizeof(msg), '.');
    }
    adrift_sr_say(sr, ADRIFT_SR_PRIO_NOW, ADRIFT_SR_CAT_ACTION, msg);
}

void adrift_sr_announce_octave(adrift_screen_reader_t *sr,
                               int8_t octave,
                               int delta_blocked) {
    if (!sr || !sr->enabled) return;
    if (delta_blocked < 0) {
        adrift_sr_say(sr, ADRIFT_SR_PRIO_KEEP, ADRIFT_SR_CAT_WARNING,
                      "Cannot lower octave, already at minus 3.");
        return;
    }
    if (delta_blocked > 0) {
        adrift_sr_say(sr, ADRIFT_SR_PRIO_KEEP, ADRIFT_SR_CAT_WARNING,
                      "Cannot raise octave, already at plus 3.");
        return;
    }
    char msg[48];
    size_t pos = 0u;
    sr_append(msg, &pos, sizeof(msg), "Octave ");
    sr_append_signed(msg, &pos, sizeof(msg), octave, true);
    sr_append_ch(msg, &pos, sizeof(msg), '.');
    adrift_sr_say(sr, ADRIFT_SR_PRIO_NOW, ADRIFT_SR_CAT_ACTION, msg);
}

/* ---- Presets Browser Modal (Rules 12, 14, 15, 16) ---- */

static uint8_t sr_preset_bank_count(uint8_t bank) {
    switch (bank % ADRIFT_PRESET_BANK_COUNT) {
        case ADRIFT_PRESET_BANK_USER: return ADRIFT_UI_PRESET_USER_COUNT;
        case ADRIFT_PRESET_BANK_FACT: return ADRIFT_UI_PRESET_FACT_COUNT;
        default:                      return ADRIFT_UI_PRESET_ALL_COUNT;
    }
}

static uint8_t sr_preset_logical_to_vis(uint8_t bank, uint8_t logical_id) {
    logical_id %= ADRIFT_UI_PRESET_ALL_COUNT;
    switch (bank % ADRIFT_PRESET_BANK_COUNT) {
        case ADRIFT_PRESET_BANK_USER:
            return (logical_id >= ADRIFT_UI_PRESET_FACT_COUNT)
                       ? (uint8_t)(logical_id - ADRIFT_UI_PRESET_FACT_COUNT)
                       : 0u;
        case ADRIFT_PRESET_BANK_FACT:
            return (logical_id < ADRIFT_UI_PRESET_FACT_COUNT) ? logical_id : 2u;
        default:
            return logical_id;
    }
}

static uint8_t sr_preset_section_id(uint8_t logical_id) {
    logical_id %= ADRIFT_UI_PRESET_ALL_COUNT;
    if (logical_id < 2u) return 1u; /* Actions */
    if (logical_id < ADRIFT_UI_PRESET_FACT_COUNT) return 2u; /* Factory presets */
    return 3u; /* User presets */
}

static const char *sr_preset_section_name(uint8_t sec) {
    if (sec == 1u) return "Actions";
    if (sec == 2u) return "Factory bank";
    return "User bank";
}

static void sr_append_preset_row_core(char *dst,
                                      size_t *pos,
                                      size_t cap,
                                      const adrift_ui_t *ui,
                                      const adrift_synth_t *s,
                                      bool include_section,
                                      bool from_bank_switch,
                                      uint8_t sec) {
    uint8_t bank    = ui->preset_bank % ADRIFT_PRESET_BANK_COUNT;
    uint8_t vis_cnt = sr_preset_bank_count(bank);
    uint8_t cur_log = ui->preset_cursor % ADRIFT_UI_PRESET_ALL_COUNT;
    uint8_t vis_cur = sr_preset_logical_to_vis(bank, cur_log);
    uint8_t eg      = s->edit_group % ADRIFT_MAX_GROUPS;

    if (include_section) {
        sr_append(dst, pos, cap, sr_preset_section_name(sec));
        sr_append(dst, pos, cap, ", ");
    }

    if (cur_log == 0u) {
        sr_append(dst, pos, cap, "initialize synth, 3 voices, 1 group");
    } else if (cur_log == 1u) {
        sr_append(dst, pos, cap, "initialize group ");
        sr_append_u32(dst, pos, cap, (uint32_t)eg + 1u);
    } else if (cur_log < ADRIFT_UI_PRESET_FACT_COUNT) {
        uint8_t pid = (uint8_t)(cur_log - 2u);
        sr_append_preset_title(dst, pos, cap, adrift_synth_get_preset_name(pid));
        sr_append(dst, pos, cap, (include_section || from_bank_switch) ? ", slot " : ", factory ");
        sr_append_u32(dst, pos, cap, (uint32_t)pid + 1u);
        if (ui->preset_load_grp) {
            uint8_t sg  = ui->preset_src_grp % ADRIFT_MAX_GROUPS;
            uint8_t gnv = adrift_synth_get_preset_group_voices(pid, sg);
            sr_append(dst, pos, cap, ", source group ");
            sr_append_u32(dst, pos, cap, (uint32_t)sg + 1u);
            sr_append(dst, pos, cap, ", ");
            sr_append_u32(dst, pos, cap, gnv);
            sr_append(dst, pos, cap, (gnv == 1u) ? " voice" : " voices");
        } else {
            uint8_t tv = 0u, ag = 0u;
            adrift_synth_get_preset_summary(pid, &tv, &ag);
            sr_append(dst, pos, cap, ", ");
            sr_append_u32(dst, pos, cap, tv);
            sr_append(dst, pos, cap, (tv == 1u) ? " voice, " : " voices, ");
            sr_append_u32(dst, pos, cap, ag);
            sr_append(dst, pos, cap, (ag == 1u) ? " group" : " groups");
        }
    } else {
        uint8_t ux  = (uint8_t)(cur_log - ADRIFT_UI_PRESET_FACT_COUNT);
        uint8_t pid = (uint8_t)(ADRIFT_USER_PRESET_BASE + ux);
        if (!adrift_synth_user_preset_is_occupied(ux)) {
            sr_append(dst, pos, cap, "empty");
            sr_append(dst, pos, cap, (include_section || from_bank_switch) ? ", slot " : ", user ");
            sr_append_u32(dst, pos, cap, (uint32_t)ux + 1u);
        } else {
            sr_append_preset_title(dst, pos, cap, adrift_synth_get_preset_name(pid));
            sr_append(dst, pos, cap, (include_section || from_bank_switch) ? ", slot " : ", user ");
            sr_append_u32(dst, pos, cap, (uint32_t)ux + 1u);
            if (ui->preset_load_grp) {
                uint8_t sg  = ui->preset_src_grp % ADRIFT_MAX_GROUPS;
                uint8_t gnv = adrift_synth_get_preset_group_voices(pid, sg);
                sr_append(dst, pos, cap, ", source group ");
                sr_append_u32(dst, pos, cap, (uint32_t)sg + 1u);
                sr_append(dst, pos, cap, ", ");
                sr_append_u32(dst, pos, cap, gnv);
                sr_append(dst, pos, cap, (gnv == 1u) ? " voice" : " voices");
            } else {
                uint8_t tv = 0u, ag = 0u;
                adrift_synth_get_preset_summary(pid, &tv, &ag);
                sr_append(dst, pos, cap, ", ");
                sr_append_u32(dst, pos, cap, tv);
                sr_append(dst, pos, cap, (tv == 1u) ? " voice, " : " voices, ");
                sr_append_u32(dst, pos, cap, ag);
                sr_append(dst, pos, cap, (ag == 1u) ? " group" : " groups");
            }
        }
    }

    sr_append(dst, pos, cap, ", ");
    sr_append_u32(dst, pos, cap, (uint32_t)vis_cur + 1u);
    sr_append(dst, pos, cap, " of ");
    sr_append_u32(dst, pos, cap, vis_cnt);
    sr_append_ch(dst, pos, cap, '.');
}

void adrift_sr_announce_preset_row(adrift_screen_reader_t *sr,
                                   const adrift_ui_t *ui,
                                   const adrift_synth_t *s,
                                   bool entered_modal) {
    if (!sr || !sr->enabled || !ui || !s) return;
    uint8_t cur_log = ui->preset_cursor % ADRIFT_UI_PRESET_ALL_COUNT;
    uint8_t sec     = sr_preset_section_id(cur_log);
    bool    new_sec = entered_modal || (sec != sr->prev_preset_section);
    sr->prev_preset_section = sec;

    char msg[ADRIFT_TTS_MAX_TEXT + 4u];
    size_t pos = 0u;
    if (entered_modal) {
        sr_append(msg, &pos, sizeof(msg), "Presets. ");
    }
    sr_append_preset_row_core(msg, &pos, sizeof(msg), ui, s, new_sec, false, sec);
    adrift_sr_say(sr, ADRIFT_SR_PRIO_NOW, ADRIFT_SR_CAT_LIST, msg);
}

void adrift_sr_announce_preset_bank(adrift_screen_reader_t *sr,
                                    const adrift_ui_t *ui,
                                    const adrift_synth_t *s) {
    if (!sr || !sr->enabled || !ui || !s) return;
    static const char *const k_banks[3] = {
        "Bank all, ", "Bank user, ", "Bank factory, "
    };
    uint8_t bank    = ui->preset_bank % ADRIFT_PRESET_BANK_COUNT;
    uint8_t cur_log = ui->preset_cursor % ADRIFT_UI_PRESET_ALL_COUNT;
    uint8_t sec     = sr_preset_section_id(cur_log);
    sr->prev_preset_section = sec;

    char msg[ADRIFT_TTS_MAX_TEXT + 4u];
    size_t pos = 0u;
    sr_append(msg, &pos, sizeof(msg), k_banks[bank]);
    sr_append_preset_row_core(msg, &pos, sizeof(msg), ui, s, false, bank != ADRIFT_PRESET_BANK_ALL, sec);
    adrift_sr_say(sr, ADRIFT_SR_PRIO_NOW, ADRIFT_SR_CAT_LIST, msg);
}

void adrift_sr_announce_preset_load_mode(adrift_screen_reader_t *sr,
                                         const adrift_ui_t *ui,
                                         const adrift_synth_t *s) {
    if (!sr || !sr->enabled || !ui || !s) return;
    char msg[128];
    size_t pos = 0u;
    if (ui->preset_load_grp) {
        sr_append(msg, &pos, sizeof(msg), "Load mode single group, source ");
        sr_append_u32(msg, &pos, sizeof(msg), (uint32_t)(ui->preset_src_grp % ADRIFT_MAX_GROUPS) + 1u);
        sr_append(msg, &pos, sizeof(msg), " into target ");
        sr_append_u32(msg, &pos, sizeof(msg), (uint32_t)(s->edit_group % ADRIFT_MAX_GROUPS) + 1u);
        sr_append_ch(msg, &pos, sizeof(msg), '.');
    } else {
        sr_append(msg, &pos, sizeof(msg), "Load mode full preset.");
    }
    adrift_sr_say(sr, ADRIFT_SR_PRIO_NOW, ADRIFT_SR_CAT_ACTION, msg);
}

void adrift_sr_announce_preset_src_grp(adrift_screen_reader_t *sr,
                                       const adrift_ui_t *ui,
                                       bool blocked) {
    if (!sr || !sr->enabled || !ui) return;
    if (blocked) {
        adrift_sr_say(sr, ADRIFT_SR_PRIO_KEEP, ADRIFT_SR_CAT_WARNING,
                      "Cannot change source group in full preset mode. Turn knob 2 right for single group.");
        return;
    }
    uint8_t sg = ui->preset_src_grp % ADRIFT_MAX_GROUPS;
    char msg[48];
    size_t pos = 0u;
    sr_append(msg, &pos, sizeof(msg), "source group ");
    sr_append_u32(msg, &pos, sizeof(msg), (uint32_t)sg + 1u);
    sr_append_ch(msg, &pos, sizeof(msg), '.');
    adrift_sr_later_knob(sr, msg);
}

void adrift_sr_announce_preset_clear_arm(adrift_screen_reader_t *sr,
                                         const adrift_ui_t *ui,
                                         bool armed,
                                         bool blocked) {
    if (!sr || !sr->enabled || !ui) return;
    if (blocked) {
        adrift_sr_say(sr, ADRIFT_SR_PRIO_KEEP, ADRIFT_SR_CAT_WARNING,
                      "Cannot clear this item, only occupied user presets can be cleared.");
        return;
    }
    if (!armed) {
        adrift_sr_say(sr, ADRIFT_SR_PRIO_NOW, ADRIFT_SR_CAT_ACTION, "Clear canceled.");
        return;
    }
    uint8_t cur_log = ui->preset_cursor % ADRIFT_UI_PRESET_ALL_COUNT;
    uint8_t ux      = (cur_log >= ADRIFT_UI_PRESET_FACT_COUNT)
                          ? (uint8_t)(cur_log - ADRIFT_UI_PRESET_FACT_COUNT)
                          : 0u;
    char msg[ADRIFT_TTS_MAX_TEXT + 4u];
    size_t pos = 0u;
    sr_append(msg, &pos, sizeof(msg), "Clear user preset ");
    sr_append_u32(msg, &pos, sizeof(msg), (uint32_t)ux + 1u);
    sr_append(msg, &pos, sizeof(msg), ", ");
    sr_append_preset_title(msg, &pos, sizeof(msg),
                           adrift_synth_get_preset_name((uint8_t)(ADRIFT_USER_PRESET_BASE + ux)));
    sr_append(msg, &pos, sizeof(msg), "? Octave plus: yes. Knob 4 left: no.");
    adrift_sr_say(sr, ADRIFT_SR_PRIO_NOW, ADRIFT_SR_CAT_QUESTION, msg);
}

void adrift_sr_announce_preset_confirm(adrift_screen_reader_t *sr,
                                       uint8_t confirmed_logical_idx,
                                       bool was_group_load,
                                       uint8_t src_grp,
                                       uint8_t dst_grp,
                                       bool was_cleared,
                                       uint8_t cleared_u_slot) {
    if (!sr || !sr->enabled) return;
    char msg[ADRIFT_TTS_MAX_TEXT + 4u];
    size_t pos = 0u;

    if (was_cleared) {
        sr_append(msg, &pos, sizeof(msg), "Cleared user preset ");
        sr_append_u32(msg, &pos, sizeof(msg), (uint32_t)(cleared_u_slot % ADRIFT_NUM_USER_PRESETS) + 1u);
        sr_append_ch(msg, &pos, sizeof(msg), '.');
        adrift_sr_say(sr, ADRIFT_SR_PRIO_NOW, ADRIFT_SR_CAT_ACTION, msg);
        return;
    }

    uint8_t log_id = confirmed_logical_idx % ADRIFT_UI_PRESET_ALL_COUNT;
    if (log_id == 0u) {
        sr_append(msg, &pos, sizeof(msg), "Synth initialized.");
    } else if (log_id == 1u) {
        sr_append(msg, &pos, sizeof(msg), "Group ");
        sr_append_u32(msg, &pos, sizeof(msg), (uint32_t)(dst_grp % ADRIFT_MAX_GROUPS) + 1u);
        sr_append(msg, &pos, sizeof(msg), " initialized.");
    } else {
        uint8_t pid = (uint8_t)(log_id - 2u);
        sr_append(msg, &pos, sizeof(msg), "Loaded ");
        if (pid >= ADRIFT_USER_PRESET_BASE) {
            sr_append(msg, &pos, sizeof(msg), "user preset ");
            sr_append_u32(msg, &pos, sizeof(msg), (uint32_t)(pid - ADRIFT_USER_PRESET_BASE) + 1u);
        } else {
            sr_append(msg, &pos, sizeof(msg), "factory preset ");
            sr_append_u32(msg, &pos, sizeof(msg), (uint32_t)pid + 1u);
        }
        sr_append(msg, &pos, sizeof(msg), ", ");
        sr_append_preset_title(msg, &pos, sizeof(msg), adrift_synth_get_preset_name(pid));
        if (was_group_load) {
            sr_append(msg, &pos, sizeof(msg), ", source ");
            sr_append_u32(msg, &pos, sizeof(msg), (uint32_t)(src_grp % ADRIFT_MAX_GROUPS) + 1u);
            sr_append(msg, &pos, sizeof(msg), " into group ");
            sr_append_u32(msg, &pos, sizeof(msg), (uint32_t)(dst_grp % ADRIFT_MAX_GROUPS) + 1u);
        }
        sr_append_ch(msg, &pos, sizeof(msg), '.');
    }
    adrift_sr_say(sr, ADRIFT_SR_PRIO_NOW, ADRIFT_SR_CAT_ACTION, msg);
}

/* ---- Save User Preset Modal ---- */

static void sr_append_save_slot_desc(char *dst,
                                     size_t *pos,
                                     size_t cap,
                                     uint8_t slot) {
    slot %= ADRIFT_NUM_USER_PRESETS;
    sr_append(dst, pos, cap, "user slot ");
    sr_append_u32(dst, pos, cap, (uint32_t)slot + 1u);
    if (adrift_synth_user_preset_is_occupied(slot)) {
        sr_append(dst, pos, cap, ", overwrite ");
        sr_append_preset_title(dst, pos, cap,
                               adrift_synth_get_preset_name((uint8_t)(ADRIFT_USER_PRESET_BASE + slot)));
    } else {
        sr_append(dst, pos, cap, ", empty");
    }
    sr_append(dst, pos, cap, ", ");
    sr_append_u32(dst, pos, cap, (uint32_t)slot + 1u);
    sr_append(dst, pos, cap, " of 24.");
}

void adrift_sr_announce_save_open(adrift_screen_reader_t *sr,
                                  const adrift_ui_t *ui) {
    if (!sr || !sr->enabled || !ui) return;
    char msg[ADRIFT_TTS_MAX_TEXT + 4u];
    size_t pos = 0u;
    sr_append(msg, &pos, sizeof(msg), "Save preset, ");
    sr_append_save_slot_desc(msg, &pos, sizeof(msg), ui->save_slot);
    sr_append(msg, &pos, sizeof(msg), " Name ");
    sr_append_preset_title(msg, &pos, sizeof(msg), ui->save_name);
    sr_append(msg, &pos, sizeof(msg), ". Octave plus saves, minus cancels.");
    adrift_sr_say(sr, ADRIFT_SR_PRIO_NOW, ADRIFT_SR_CAT_SCREEN, msg);
}

void adrift_sr_announce_save_slot(adrift_screen_reader_t *sr,
                                  const adrift_ui_t *ui) {
    if (!sr || !sr->enabled || !ui) return;
    char msg[ADRIFT_TTS_MAX_TEXT + 4u];
    size_t pos = 0u;
    sr_append_save_slot_desc(msg, &pos, sizeof(msg), ui->save_slot);
    adrift_sr_say(sr, ADRIFT_SR_PRIO_NOW, ADRIFT_SR_CAT_LIST, msg);
}

void adrift_sr_announce_save_cursor(adrift_screen_reader_t *sr,
                                    const adrift_ui_t *ui,
                                    bool settled_char_only) {
    if (!sr || !sr->enabled || !ui) return;
    uint8_t cur = ui->save_cursor % 12u;
    char    ch  = ui->save_name[cur];
    char    msg[64];
    size_t  pos = 0u;
    if (settled_char_only) {
        sr_append(msg, &pos, sizeof(msg), "character ");
        sr_append_char_spoken(msg, &pos, sizeof(msg), ch);
        sr_append_ch(msg, &pos, sizeof(msg), '.');
        adrift_sr_later_knob(sr, msg);
    } else {
        sr_append(msg, &pos, sizeof(msg), "Position ");
        sr_append_u32(msg, &pos, sizeof(msg), (uint32_t)cur + 1u);
        sr_append(msg, &pos, sizeof(msg), ", ");
        sr_append_char_spoken(msg, &pos, sizeof(msg), ch);
        sr_append_ch(msg, &pos, sizeof(msg), '.');
        adrift_sr_say(sr, ADRIFT_SR_PRIO_NOW, ADRIFT_SR_CAT_ACTION, msg);
    }
}

void adrift_sr_announce_save_edit(adrift_screen_reader_t *sr,
                                  const adrift_ui_t *ui,
                                  const char *action_msg) {
    if (!sr || !sr->enabled || !ui || !action_msg) return;
    char msg[128];
    size_t pos = 0u;
    sr_append(msg, &pos, sizeof(msg), action_msg);
    sr_append(msg, &pos, sizeof(msg), " Name ");
    sr_append_preset_title(msg, &pos, sizeof(msg), ui->save_name);
    sr_append_ch(msg, &pos, sizeof(msg), '.');
    adrift_sr_say(sr, ADRIFT_SR_PRIO_NOW, ADRIFT_SR_CAT_ACTION, msg);
}

void adrift_sr_announce_save_result(adrift_screen_reader_t *sr,
                                    uint8_t u_slot,
                                    const char *saved_name,
                                    bool ok) {
    if (!sr || !sr->enabled) return;
    if (!ok) {
        adrift_sr_say(sr, ADRIFT_SR_PRIO_KEEP, ADRIFT_SR_CAT_WARNING,
                      "Save failed, flash memory is not ready.");
        return;
    }
    char msg[128];
    size_t pos = 0u;
    sr_append(msg, &pos, sizeof(msg), "Saved user preset ");
    sr_append_u32(msg, &pos, sizeof(msg), (uint32_t)(u_slot % ADRIFT_NUM_USER_PRESETS) + 1u);
    sr_append(msg, &pos, sizeof(msg), ", ");
    sr_append_preset_title(msg, &pos, sizeof(msg), saved_name);
    sr_append_ch(msg, &pos, sizeof(msg), '.');
    adrift_sr_say(sr, ADRIFT_SR_PRIO_NOW, ADRIFT_SR_CAT_ACTION, msg);
}

/* ---- Scale Browser Modal ---- */

static void sr_append_scale_row_desc(char *dst,
                                     size_t *pos,
                                     size_t cap,
                                     uint8_t scale_idx,
                                     uint8_t root_idx) {
    scale_idx %= ADRIFT_NUM_SCALES;
    root_idx  %= 12u;
    if (scale_idx == 0u) {
        sr_append(dst, pos, cap, "chromatic");
    } else {
        sr_append(dst, pos, cap, sr_root_spoken(root_idx));
        sr_append_ch(dst, pos, cap, ' ');
        sr_append(dst, pos, cap, sr_scale_spoken(scale_idx));
    }
    uint8_t nn = adrift_synth_get_scale_num_notes(scale_idx);
    sr_append(dst, pos, cap, ", ");
    sr_append_u32(dst, pos, cap, nn);
    sr_append(dst, pos, cap, " notes, ");
    sr_append_u32(dst, pos, cap, (uint32_t)scale_idx + 1u);
    sr_append(dst, pos, cap, " of 16.");
}

void adrift_sr_announce_scale_row(adrift_screen_reader_t *sr,
                                  const adrift_synth_t *s,
                                  bool entered_modal) {
    if (!sr || !sr->enabled || !s) return;
    char msg[128];
    size_t pos = 0u;
    if (entered_modal) {
        sr_append(msg, &pos, sizeof(msg), "Scales, ");
    }
    sr_append_scale_row_desc(msg, &pos, sizeof(msg), s->kbd_scale, s->kbd_scale_root);
    adrift_sr_say(sr, ADRIFT_SR_PRIO_NOW, ADRIFT_SR_CAT_LIST, msg);
}

void adrift_sr_announce_scale_root(adrift_screen_reader_t *sr,
                                   const adrift_synth_t *s) {
    if (!sr || !sr->enabled || !s) return;
    char msg[64];
    size_t pos = 0u;
    sr_append(msg, &pos, sizeof(msg), "root ");
    sr_append(msg, &pos, sizeof(msg), sr_root_spoken(s->kbd_scale_root));
    sr_append_ch(msg, &pos, sizeof(msg), '.');
    adrift_sr_later_knob(sr, msg);
}

void adrift_sr_announce_scale_confirm(adrift_screen_reader_t *sr,
                                      const adrift_synth_t *s) {
    if (!sr || !sr->enabled || !s) return;
    uint8_t sc = s->kbd_scale % ADRIFT_NUM_SCALES;
    char msg[96];
    size_t pos = 0u;
    sr_append(msg, &pos, sizeof(msg), "Scale applied, ");
    if (sc == 0u) {
        sr_append(msg, &pos, sizeof(msg), "chromatic.");
    } else {
        sr_append(msg, &pos, sizeof(msg), sr_root_spoken(s->kbd_scale_root));
        sr_append_ch(msg, &pos, sizeof(msg), ' ');
        sr_append(msg, &pos, sizeof(msg), sr_scale_spoken(sc));
        sr_append_ch(msg, &pos, sizeof(msg), '.');
    }
    adrift_sr_say(sr, ADRIFT_SR_PRIO_NOW, ADRIFT_SR_CAT_ACTION, msg);
}

/* ---- Full Context WHERE Utterance (BK11 + BK7, Rules 11, 12, 13) ---- */

static void sr_append_active_group_summary(char *dst,
                                           size_t *pos,
                                           size_t cap,
                                           const adrift_synth_t *s) {
    uint8_t g = s->edit_group % ADRIFT_MAX_GROUPS;
    const adrift_group_t *gp = &s->groups[g];
    sr_append(dst, pos, cap, "Group ");
    sr_append_u32(dst, pos, cap, (uint32_t)g + 1u);
    sr_append(dst, pos, cap, ", ");
    if (gp->num_voices == 0u) {
        sr_append(dst, pos, cap, "off, ");
    } else {
        sr_append_u32(dst, pos, cap, gp->num_voices);
        sr_append(dst, pos, cap, (gp->num_voices == 1u) ? " voice, " : " voices, ");
    }
    sr_append_preset_title(dst, pos, cap, s->preset_name);
    sr_append(dst, pos, cap, ". ");
}

void adrift_sr_speak_where(adrift_screen_reader_t *sr,
                           const adrift_ui_t *ui,
                           const adrift_synth_t *s) {
    if (!sr || !sr->enabled || !ui || !s) return;

    char msg[ADRIFT_TTS_MAX_TEXT + 4u];
    size_t pos = 0u;

    if (ui->in_save_menu) {
        uint8_t cur = ui->save_cursor % 12u;
        sr_append(msg, &pos, sizeof(msg), "Save preset, ");
        sr_append_save_slot_desc(msg, &pos, sizeof(msg), ui->save_slot);
        sr_append(msg, &pos, sizeof(msg), " Name ");
        sr_append_preset_title(msg, &pos, sizeof(msg), ui->save_name);
        sr_append(msg, &pos, sizeof(msg), ", position ");
        sr_append_u32(msg, &pos, sizeof(msg), (uint32_t)cur + 1u);
        sr_append(msg, &pos, sizeof(msg), " of 12, ");
        sr_append_char_spoken(msg, &pos, sizeof(msg), ui->save_name[cur]);
        sr_append(msg, &pos, sizeof(msg), ". Octave plus saves, minus cancels.");
        adrift_sr_say(sr, ADRIFT_SR_PRIO_NOW, ADRIFT_SR_CAT_WHERE, msg);
        return;
    }

    if (ui->in_preset_menu) {
        static const char *const k_bnk[3] = { "all", "user", "factory" };
        uint8_t cur_log = ui->preset_cursor % ADRIFT_UI_PRESET_ALL_COUNT;
        uint8_t sec     = sr_preset_section_id(cur_log);
        sr_append(msg, &pos, sizeof(msg), "Presets, ");
        sr_append_preset_row_core(msg, &pos, sizeof(msg), ui, s, false, false, sec);
        sr_append(msg, &pos, sizeof(msg), " Bank ");
        sr_append(msg, &pos, sizeof(msg), k_bnk[ui->preset_bank % ADRIFT_PRESET_BANK_COUNT]);
        if (ui->preset_load_grp) {
            sr_append(msg, &pos, sizeof(msg), ", load mode single group, target ");
            sr_append_u32(msg, &pos, sizeof(msg), (uint32_t)(s->edit_group % ADRIFT_MAX_GROUPS) + 1u);
            sr_append_ch(msg, &pos, sizeof(msg), '.');
        } else {
            sr_append(msg, &pos, sizeof(msg), ", load mode full.");
        }
        adrift_sr_say(sr, ADRIFT_SR_PRIO_NOW, ADRIFT_SR_CAT_WHERE, msg);
        return;
    }

    if (ui->in_scale_menu) {
        sr_append(msg, &pos, sizeof(msg), "Scales, ");
        sr_append_scale_row_desc(msg, &pos, sizeof(msg), ui->scale_cursor, s->kbd_scale_root);
        sr_append(msg, &pos, sizeof(msg), " Root ");
        sr_append(msg, &pos, sizeof(msg), sr_root_spoken(s->kbd_scale_root));
        sr_append(msg, &pos, sizeof(msg), ". Octave plus applies, minus cancels.");
        adrift_sr_say(sr, ADRIFT_SR_PRIO_NOW, ADRIFT_SR_CAT_WHERE, msg);
        return;
    }

    if (ui->in_scope) {
        static const char *const k_home_names[6] = {
            "Oscilloscope", "Vectorscope", "Spectrum",
            "Soundstage", "Polyphony", "Performance"
        };
        uint8_t sp = ui->scope_page % 6u;
        sr_append(msg, &pos, sizeof(msg), k_home_names[sp]);
        sr_append(msg, &pos, sizeof(msg), ", page ");
        sr_append_u32(msg, &pos, sizeof(msg), (uint32_t)sp + 1u);
        sr_append(msg, &pos, sizeof(msg), " of 6. ");
        sr_append_active_group_summary(msg, &pos, sizeof(msg), s);
        for (uint8_t k = 0u; k < 4u; k++) {
            if (k > 0u) sr_append(msg, &pos, sizeof(msg), ", ");
            sr_append_home_param_readout(msg, &pos, sizeof(msg), ui, k);
        }
        sr_append_ch(msg, &pos, sizeof(msg), '.');
        adrift_sr_say(sr, ADRIFT_SR_PRIO_NOW, ADRIFT_SR_CAT_WHERE, msg);
        return;
    }

    if (ui->in_group_menu) {
        uint8_t sub = ui->subpage & 1u;
        sr_append(msg, &pos, sizeof(msg), "Group settings, page ");
        sr_append_u32(msg, &pos, sizeof(msg), (uint32_t)sub + 1u);
        sr_append(msg, &pos, sizeof(msg), " of 2. ");
        sr_append_active_group_summary(msg, &pos, sizeof(msg), s);
        for (uint8_t k = 0u; k < 4u; k++) {
            if (k > 0u) sr_append(msg, &pos, sizeof(msg), ", ");
            sr_append_group_param_readout(msg, &pos, sizeof(msg), s, (uint8_t)(sub * 4u + k));
        }
        sr_append_ch(msg, &pos, sizeof(msg), '.');
        adrift_sr_say(sr, ADRIFT_SR_PRIO_NOW, ADRIFT_SR_CAT_WHERE, msg);
        return;
    }

    if (ui->in_arp_menu) {
        uint8_t sub = ui->subpage & 1u;
        sr_append(msg, &pos, sizeof(msg), "Arpeggiator, page ");
        sr_append_u32(msg, &pos, sizeof(msg), (uint32_t)sub + 1u);
        sr_append(msg, &pos, sizeof(msg), " of 2. ");
        sr_append_active_group_summary(msg, &pos, sizeof(msg), s);
        for (uint8_t k = 0u; k < 4u; k++) {
            if (k > 0u) sr_append(msg, &pos, sizeof(msg), ", ");
            sr_append_arp_param_readout(msg, &pos, sizeof(msg), s, (uint8_t)(sub * 4u + k));
        }
        sr_append_ch(msg, &pos, sizeof(msg), '.');
        adrift_sr_say(sr, ADRIFT_SR_PRIO_NOW, ADRIFT_SR_CAT_WHERE, msg);
        return;
    }

    if (ui->in_fx_menu) {
        uint8_t sub = ui->subpage % 3u;
        sr_append(msg, &pos, sizeof(msg), "Effects, page ");
        sr_append_u32(msg, &pos, sizeof(msg), (uint32_t)sub + 1u);
        sr_append(msg, &pos, sizeof(msg), " of 3. ");
        sr_append_active_group_summary(msg, &pos, sizeof(msg), s);
        for (uint8_t k = 0u; k < 4u; k++) {
            if (k > 0u) sr_append(msg, &pos, sizeof(msg), ", ");
            sr_append_fx_param_readout(msg, &pos, sizeof(msg), s, (uint8_t)(sub * 4u + k));
        }
        sr_append_ch(msg, &pos, sizeof(msg), '.');
        adrift_sr_say(sr, ADRIFT_SR_PRIO_NOW, ADRIFT_SR_CAT_WHERE, msg);
        return;
    }

    /* Main parameter screens (Exciter, Envelope 1, Resonator, Envelope 2, Modulation) */
    static const char *const k_page_names[5] = {
        "Exciter", "Envelope 1", "Resonator", "Envelope 2", "Modulation"
    };
    uint8_t pg  = ui->page % ADRIFT_UI_NUM_PAGES;
    uint8_t nsb = (pg == 0u) ? 3u : ((pg == 4u) ? 4u : 2u);
    uint8_t sub = ui->subpage % nsb;
    const adrift_patch_t *p = adrift_synth_view_patch(s);

    sr_append(msg, &pos, sizeof(msg), k_page_names[pg]);
    sr_append(msg, &pos, sizeof(msg), ", page ");
    sr_append_u32(msg, &pos, sizeof(msg), (uint32_t)sub + 1u);
    sr_append(msg, &pos, sizeof(msg), " of ");
    sr_append_u32(msg, &pos, sizeof(msg), nsb);
    sr_append(msg, &pos, sizeof(msg), ". ");
    sr_append_active_group_summary(msg, &pos, sizeof(msg), s);

    bool first = true;
    for (uint8_t k = 0u; k < 4u; k++) {
        uint8_t enc = (uint8_t)(sub * 4u + k);
        bool unmapped = (pg == 0u && enc >= 10u) ||
                        (pg == 3u && enc >= 7u)  ||
                        (pg == 4u && enc >= 13u);
        if (unmapped) continue;
        if (!first) sr_append(msg, &pos, sizeof(msg), ", ");
        first = false;
        sr_append_main_param_readout(msg, &pos, sizeof(msg), p, pg, enc);
    }
    sr_append_ch(msg, &pos, sizeof(msg), '.');
    adrift_sr_say(sr, ADRIFT_SR_PRIO_NOW, ADRIFT_SR_CAT_WHERE, msg);
    (void)sr_strlen;
}
