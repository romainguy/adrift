/* SPDX-License-Identifier: MPL-2.0
 * Freestanding WebAssembly platform backend for the Adrift browser emulator.
 * Compiles CORE_MODULES directly to wasm32 (-nostdlib) for AudioWorklet execution. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app.h"

#define WASM_EXPORT __attribute__((visibility("default")))

#define ADRIFT_WEB_MAX_FRAMES    512u
#define ADRIFT_WEB_MIDI_CAP      256u
#define ADRIFT_WEB_QWERTY_SLOTS  64u
#define ADRIFT_WEB_FLASH_BYTES   (ADRIFT_STORAGE_TOTAL_SLOTS * ADRIFT_STORAGE_SLOT_BYTES)

/* ---- Minimal freestanding C99 memory primitives (-nostdlib -fno-builtin) ---- */

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0u; i < n; i++) {
        d[i] = s[i];
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s || d >= s + n) {
        for (size_t i = 0u; i < n; i++) {
            d[i] = s[i];
        }
    } else if (n > 0u) {
        for (size_t i = n; i > 0u; i--) {
            d[i - 1u] = s[i - 1u];
        }
    }
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    uint8_t  v = (uint8_t)c;
    for (size_t i = 0u; i < n; i++) {
        d[i] = v;
    }
    return dst;
}

/* ---- Platform HAL: Single-core DSP fallback & 25-slot User Preset storage ---- */

bool adrift_platform_run_worker(void (*fn)(uint32_t arg), uint32_t arg) {
    (void)fn;
    (void)arg;
    return false;
}

bool adrift_platform_wait_worker(void) {
    return false;
}

static uint8_t g_web_flash[ADRIFT_STORAGE_TOTAL_SLOTS][ADRIFT_STORAGE_SLOT_BYTES];
static bool    g_web_flash_inited = false;
static uint8_t g_storage_dirty    = 0u;

static void ensure_web_flash(void) {
    if (!g_web_flash_inited) {
        memset(g_web_flash, 0xFF, sizeof(g_web_flash));
        g_web_flash_inited = true;
    }
}

bool adrift_platform_storage_available(void) {
    ensure_web_flash();
    return true;
}

bool adrift_platform_storage_read(uint8_t slot, uint8_t *dst, uint32_t len) {
    ensure_web_flash();
    if (!dst || slot >= ADRIFT_STORAGE_TOTAL_SLOTS || len > ADRIFT_STORAGE_SLOT_BYTES) {
        return false;
    }
    memcpy(dst, g_web_flash[slot], len);
    return true;
}

bool adrift_platform_storage_write(uint8_t slot, const uint8_t *src, uint32_t len) {
    ensure_web_flash();
    if (!src || slot >= ADRIFT_STORAGE_TOTAL_SLOTS || len > ADRIFT_STORAGE_SLOT_BYTES) {
        return false;
    }
    memset(g_web_flash[slot], 0xFF, ADRIFT_STORAGE_SLOT_BYTES);
    memcpy(g_web_flash[slot], src, len);
    g_storage_dirty = 1u;
    return true;
}

bool adrift_platform_storage_erase(uint8_t slot) {
    ensure_web_flash();
    if (slot >= ADRIFT_STORAGE_TOTAL_SLOTS) {
        return false;
    }
    memset(g_web_flash[slot], 0xFF, ADRIFT_STORAGE_SLOT_BYTES);
    g_storage_dirty = 1u;
    return true;
}

/* ---- Web Emulator Audio, MIDI, Input & Framebuffer State ---- */

static adrift_midi_msg_t g_midi_q[ADRIFT_WEB_MIDI_CAP];
static uint32_t          g_midi_len = 0u;

static int16_t  g_pcm_i16[ADRIFT_WEB_MAX_FRAMES * 2u];
static float    g_audio_l[ADRIFT_WEB_MAX_FRAMES];
static float    g_audio_r[ADRIFT_WEB_MAX_FRAMES];

static uint16_t g_rgb565_fb[ADRIFT_SCREEN_W * ADRIFT_SCREEN_H];
static uint8_t  g_rgba_fb[ADRIFT_SCREEN_W * ADRIFT_SCREEN_H * 4u];

static int32_t  g_enc_acc[ADRIFT_NUM_ENC];
static uint32_t g_hw_btn_down   = 0u;
static uint32_t g_hw_btn_pulse  = 0u;
static uint32_t g_log_btn_pulse = 0u;
static uint32_t g_prev_log_btn  = 0u;
static int16_t  g_master_pot    = 900; /* ~88% default master volume */
static char     g_text_char     = 0;
static uint8_t  g_text_cmd      = ADRIFT_TEXT_CMD_NONE;

/* 27-key panel mini-keyboard state (matches platform_fm1.c) */
static uint32_t g_sounding_keys = 0u;
static uint8_t  g_key_note[27];
static uint8_t  g_key_chan[27];

/* Host QWERTY keyboard note tracking slots */
static uint8_t  g_qwerty_note[ADRIFT_WEB_QWERTY_SLOTS];
static uint8_t  g_qwerty_chan[ADRIFT_WEB_QWERTY_SLOTS];

static uint32_t g_leds_buttons  = 0u;
static uint8_t  g_web_boot_frame = 0u;

static void queue_midi(uint8_t status, uint8_t d1, uint8_t d2) {
    if (g_midi_len < ADRIFT_WEB_MIDI_CAP) {
        adrift_midi_msg_t *m = &g_midi_q[g_midi_len++];
        m->status        = status;
        m->data1         = d1;
        m->data2         = d2;
        m->sample_offset = 0u;
    }
}

static uint32_t map_hw_buttons(uint32_t hw_b) {
    uint32_t out = 0u;
    if (hw_b & (1u << 0))  out |= ADRIFT_BTN_OCT_DOWN;      /* OCT- -> Octave Down / Modal Cancel */
    if (hw_b & (1u << 1))  out |= ADRIFT_BTN_OCT_UP;        /* OCT+ -> Octave Up / Modal Confirm */
    if (hw_b & (1u << 2))  out |= ADRIFT_BTN_FX;            /* FX   -> Master FX Settings */
    if (hw_b & (1u << 3))  out |= ADRIFT_BTN_PAGE_1;        /* SCL  -> EXC */
    if (hw_b & (1u << 4))  out |= ADRIFT_BTN_PAGE_2;        /* ENV  -> ENV1 */
    if (hw_b & (1u << 5))  out |= ADRIFT_BTN_PAGE_3;        /* LFO  -> BBD */
    if (hw_b & (1u << 6))  out |= ADRIFT_BTN_PAGE_4;        /* EDIT -> ENV2 */
    if (hw_b & (1u << 7))  out |= ADRIFT_BTN_PAGE_5;        /* GLO  -> MOD */
    if (hw_b & (1u << 8))  out |= ADRIFT_BTN_HOME;          /* HOME -> Visualizations (1/6..6/6) */
    if (hw_b & (1u << 9))  out |= ADRIFT_BTN_SAVE_PRESET;   /* SAVE -> Save User Preset modal */
    if (hw_b & (1u << 10)) out |= ADRIFT_BTN_ARP;           /* ARP  -> Arpeggiator Settings */
    if (hw_b & (1u << 11)) out |= ADRIFT_BTN_GROUP_CFG;     /* SEQ  -> Group Settings */
    if (hw_b & (1u << 12)) out |= ADRIFT_BTN_PANIC;         /* PLAY -> Panic / All Notes Off */
    /* REC (bit 13): intentionally unmapped */
    return out;
}

static uint32_t compute_button_leds(uint32_t hw_down) {
    static const uint8_t k_page_led[5] = { 3u, 4u, 5u, 6u, 7u };
    const adrift_ui_t    *ui = adrift_app_get_ui();
    const adrift_synth_t *s  = adrift_app_get_synth();
    bool arp_active = (adrift_synth_active_group(s)->patch.arp.mode != ADRIFT_ARP_OFF);
    bool fx_active  = adrift_fx_is_active(&s->fx_params);
    bool in_modal   = ui->in_preset_menu || ui->in_scale_menu || ui->in_save_menu;

    uint8_t  active_btn = k_page_led[ui->page % 5u];
    uint32_t mask       = 0u;
    for (uint32_t b = 0u; b < 14u; b++) {
        if (b == 13u) {
            continue; /* REC is inactive */
        }
        bool lit = (!in_modal && !ui->in_scope && !ui->in_group_menu && !ui->in_arp_menu && !ui->in_fx_menu && b == active_btn) ||
                   (in_modal && (b == 0u || b == 1u)) ||
                   (b == 2u && ((!in_modal && !ui->in_scope && ui->in_fx_menu) || fx_active)) ||
                   (b == 9u && (ui->in_save_menu || ui->save_toast_frames > 0u)) ||
                   (b == 10u && ((!in_modal && !ui->in_scope && ui->in_arp_menu) || arp_active)) ||
                   (b == 11u && !in_modal && !ui->in_scope && ui->in_group_menu) ||
                   (b == 8u && !in_modal && ui->in_scope) ||
                   (((hw_down >> b) & 1u) != 0u);
        if (lit) {
            mask |= (1u << b);
        }
    }
    return mask;
}

/* ---- Exported WASM API ---- */

WASM_EXPORT uint8_t *adrift_web_storage_ptr(void) {
    ensure_web_flash();
    return &g_web_flash[0][0];
}

WASM_EXPORT uint32_t adrift_web_storage_len(void) {
    return ADRIFT_WEB_FLASH_BYTES;
}

WASM_EXPORT void adrift_web_storage_reset_ram(void) {
    memset(g_web_flash, 0xFF, sizeof(g_web_flash));
    g_web_flash_inited = true;
    g_storage_dirty    = 0u;
}

WASM_EXPORT void adrift_web_storage_reload(void) {
    ensure_web_flash();
    adrift_preset_store_init();
    adrift_app_get_ui()->initialized = false;
    g_storage_dirty = 0u;
}

WASM_EXPORT uint32_t adrift_web_take_storage_dirty(void) {
    uint32_t d = g_storage_dirty;
    g_storage_dirty = 0u;
    return d;
}

WASM_EXPORT void adrift_web_init(void) {
    ensure_web_flash();
    g_midi_len      = 0u;
    g_hw_btn_down   = 0u;
    g_hw_btn_pulse  = 0u;
    g_log_btn_pulse = 0u;
    g_prev_log_btn  = 0u;
    g_master_pot    = 900;
    g_text_char     = 0;
    g_text_cmd      = ADRIFT_TEXT_CMD_NONE;
    g_sounding_keys  = 0u;
    g_storage_dirty  = 0u;
    g_web_boot_frame = 0u;

    for (uint32_t e = 0u; e < ADRIFT_NUM_ENC; e++) {
        g_enc_acc[e] = 0;
    }
    for (uint32_t k = 0u; k < 27u; k++) {
        g_key_note[k] = 0xFFu;
        g_key_chan[k] = 0u;
    }
    for (uint32_t s = 0u; s < ADRIFT_WEB_QWERTY_SLOTS; s++) {
        g_qwerty_note[s] = 0xFFu;
        g_qwerty_chan[s] = 0u;
    }
    memset(g_rgb565_fb, 0, sizeof(g_rgb565_fb));
    memset(g_rgba_fb, 0, sizeof(g_rgba_fb));

    adrift_app_init();
    g_storage_dirty = 0u; /* Clear any boot-recovery write flag */
}

WASM_EXPORT void adrift_web_push_midi(uint8_t status, uint8_t d1, uint8_t d2) {
    if (status < 0xF8u) {
        adrift_ui_dismiss_boot(adrift_app_get_ui());
    }
    queue_midi(status, d1, d2);
}

WASM_EXPORT void adrift_web_panel_key(uint8_t key_idx, uint8_t down, uint8_t velocity) {
    if (key_idx >= 27u) {
        return;
    }
    const adrift_synth_t *s = adrift_app_get_synth();
    if (down) {
        adrift_ui_dismiss_boot(adrift_app_get_ui());
        if (g_key_note[key_idx] <= 127u) {
            queue_midi((uint8_t)(0x80u | g_key_chan[key_idx]), g_key_note[key_idx], 0u);
            g_key_note[key_idx] = 0xFFu;
        }
        int32_t n = adrift_synth_map_kbd_key(s, (int32_t)key_idx - 7);
        if (n >= 0) {
            uint8_t ch_nib = (uint8_t)((adrift_synth_active_group(s)->midi_channel - 1u) & 0x0Fu);
            uint8_t vel    = (velocity >= 1u && velocity <= 127u) ? velocity : 100u;
            g_key_note[key_idx] = (uint8_t)n;
            g_key_chan[key_idx] = ch_nib;
            g_sounding_keys |= (1u << key_idx);
            queue_midi((uint8_t)(0x90u | ch_nib), (uint8_t)n, vel);
        } else {
            g_sounding_keys &= ~(1u << key_idx);
        }
    } else {
        g_sounding_keys &= ~(1u << key_idx);
        if (g_key_note[key_idx] <= 127u) {
            queue_midi((uint8_t)(0x80u | g_key_chan[key_idx]), g_key_note[key_idx], 0u);
            g_key_note[key_idx] = 0xFFu;
        }
    }
}

WASM_EXPORT void adrift_web_qwerty_note(uint8_t slot_id, int32_t semi_from_c4, uint8_t down, uint8_t velocity) {
    if (slot_id >= ADRIFT_WEB_QWERTY_SLOTS) {
        return;
    }
    const adrift_synth_t *s = adrift_app_get_synth();
    if (down) {
        adrift_ui_dismiss_boot(adrift_app_get_ui());
        if (g_qwerty_note[slot_id] <= 127u) {
            queue_midi((uint8_t)(0x80u | g_qwerty_chan[slot_id]), g_qwerty_note[slot_id], 0u);
            g_qwerty_note[slot_id] = 0xFFu;
        }
        int32_t n = adrift_synth_map_kbd_key(s, semi_from_c4);
        if (n >= 0) {
            uint8_t ch_nib = (uint8_t)((adrift_synth_active_group(s)->midi_channel - 1u) & 0x0Fu);
            uint8_t vel    = (velocity >= 1u && velocity <= 127u) ? velocity : 104u;
            g_qwerty_note[slot_id] = (uint8_t)n;
            g_qwerty_chan[slot_id] = ch_nib;
            queue_midi((uint8_t)(0x90u | ch_nib), (uint8_t)n, vel);
        }
    } else {
        if (g_qwerty_note[slot_id] <= 127u) {
            queue_midi((uint8_t)(0x80u | g_qwerty_chan[slot_id]), g_qwerty_note[slot_id], 0u);
            g_qwerty_note[slot_id] = 0xFFu;
        }
    }
}

WASM_EXPORT void adrift_web_enc_delta(uint8_t enc_idx, int32_t delta) {
    if (enc_idx < ADRIFT_NUM_ENC) {
        g_enc_acc[enc_idx] += delta;
    }
}

WASM_EXPORT void adrift_web_hw_button(uint8_t hw_idx, uint8_t down) {
    if (hw_idx >= 14u) {
        return;
    }
    uint32_t bit = (1u << hw_idx);
    if (down) {
        g_hw_btn_down  |= bit;
        g_hw_btn_pulse |= bit;
    } else {
        g_hw_btn_down &= ~bit;
    }
}

WASM_EXPORT void adrift_web_hw_button_pulse(uint8_t hw_idx) {
    if (hw_idx < 14u) {
        g_hw_btn_pulse |= (1u << hw_idx);
    }
}

WASM_EXPORT void adrift_web_set_master_pot(int32_t val_0_1023) {
    if (val_0_1023 < 0) val_0_1023 = 0;
    if (val_0_1023 > 1023) val_0_1023 = 1023;
    g_master_pot = (int16_t)val_0_1023;
}

WASM_EXPORT void adrift_web_text_input(uint8_t ascii_char, uint8_t text_cmd) {
    if (ascii_char != 0u) {
        g_text_char = (char)ascii_char;
    }
    if (text_cmd != ADRIFT_TEXT_CMD_NONE) {
        g_text_cmd = text_cmd;
    }
}

WASM_EXPORT void adrift_web_lcd_click(int32_t x, int32_t y, uint8_t right_btn) {
    if (x < 0 || x >= (int32_t)ADRIFT_SCREEN_W || y < 0 || y >= (int32_t)ADRIFT_SCREEN_H) {
        return;
    }
    adrift_ui_t    *ui = adrift_app_get_ui();
    adrift_synth_t *s  = adrift_app_get_synth();

    if (ui->boot_active) {
        adrift_ui_dismiss_boot(ui);
        return;
    }

    if (right_btn) {
        if (x >= 5 && x < 130 && y >= 0 && y <= 21) {
            g_log_btn_pulse |= ADRIFT_BTN_SAVE_PRESET;
        } else if (x >= 192 && x < 240 && y >= 0 && y <= 21) {
            g_log_btn_pulse |= ADRIFT_BTN_GROUP_CFG;
        }
        return;
    }

    /* Header preset badge/title -> open Presets Browser */
    if (!ui->in_preset_menu && !ui->in_save_menu && x >= 5 && x < 130 && y >= 0 && y <= 21) {
        g_enc_acc[ADRIFT_ENC_PRESETS] += 1;
        return;
    }

    /* Save Preset modal: click any of the 12 character cells */
    if (ui->in_save_menu && x >= 19 && x < 223 && y >= 118 && y <= 144) {
        int32_t cell = (x - 19) / 17;
        if (cell >= 0 && cell < 12) {
            ui->save_cursor = (uint8_t)cell;
        }
        return;
    }

    /* Header [Gx:y] badge -> cycle active Voice Group G1..G6 */
    if (x >= 192 && x < 240 && y >= 0 && y <= 21) {
        g_enc_acc[ADRIFT_ENC_SELECT] += 1;
        return;
    }

    /* Top-left screen/subpage badge [EXC 1/3] or [SCOPE 1/6] -> cycle subpages */
    if (!ui->in_preset_menu && !ui->in_scale_menu && x >= 5 && x < 80 && y >= 30 && y <= 48) {
        if (ui->in_scope) {
            g_log_btn_pulse |= ADRIFT_BTN_HOME;
        } else if (ui->in_group_menu) {
            g_log_btn_pulse |= ADRIFT_BTN_GROUP_CFG;
        } else if (ui->in_arp_menu) {
            g_log_btn_pulse |= ADRIFT_BTN_ARP;
        } else if (ui->in_fx_menu) {
            g_log_btn_pulse |= ADRIFT_BTN_FX;
        } else {
            g_log_btn_pulse |= (1u << (ui->page % 5u));
        }
        return;
    }

    /* Group Settings menu: click G1..G6 column to focus that group */
    if (!ui->in_preset_menu && !ui->in_scale_menu && !ui->in_scope &&
        ui->in_group_menu && x >= 9 && x < 231 && y >= 48 && y <= 132) {
        int32_t col = (x - 9) / 37;
        if (col >= 0 && col < ADRIFT_MAX_GROUPS) {
            s->edit_group = (uint8_t)col;
        }
    }
}

WASM_EXPORT void adrift_web_load_preset(uint8_t preset_idx) {
    adrift_synth_t *s  = adrift_app_get_synth();
    adrift_ui_t    *ui = adrift_app_get_ui();

    adrift_ui_dismiss_boot(ui);
    ui->in_preset_menu   = false;
    ui->preset_previewed = false;
    ui->in_scale_menu    = false;
    ui->scale_previewed  = false;
    ui->in_save_menu     = false;

    for (uint32_t k = 0u; k < 27u; k++) {
        g_key_note[k] = 0xFFu;
    }
    for (uint32_t q = 0u; q < ADRIFT_WEB_QWERTY_SLOTS; q++) {
        g_qwerty_note[q] = 0xFFu;
    }
    g_sounding_keys = 0u;
    g_midi_len      = 0u;

    adrift_synth_all_notes_off(s, true);
    adrift_synth_load_preset(s, preset_idx);
    ui->initialized = false;
}

WASM_EXPORT void adrift_web_panic(void) {
    for (uint32_t k = 0u; k < 27u; k++) {
        g_key_note[k] = 0xFFu;
    }
    for (uint32_t q = 0u; q < ADRIFT_WEB_QWERTY_SLOTS; q++) {
        g_qwerty_note[q] = 0xFFu;
    }
    g_sounding_keys = 0u;
    g_midi_len      = 0u;
    adrift_synth_all_notes_off(adrift_app_get_synth(), true);
}

WASM_EXPORT void adrift_web_render_audio(uint32_t frames) {
    if (frames > ADRIFT_WEB_MAX_FRAMES) {
        frames = ADRIFT_WEB_MAX_FRAMES;
    }

    uint32_t done = 0u;
    while (done < frames) {
        uint32_t blk = frames - done;
        if (blk > ADRIFT_BLOCK_SIZE) {
            blk = ADRIFT_BLOCK_SIZE;
        }
        const adrift_midi_msg_t *evs = (done == 0u && g_midi_len > 0u) ? g_midi_q : (const adrift_midi_msg_t *)0;
        uint32_t                 nev = (done == 0u) ? g_midi_len : 0u;

        adrift_app_audio_block(g_pcm_i16 + (done * 2u), blk, evs, nev);
        done += blk;
    }
    g_midi_len = 0u;

    const float inv_32768 = 1.0f / 32768.0f;
    for (uint32_t i = 0u; i < frames; i++) {
        g_audio_l[i] = (float)g_pcm_i16[i * 2u + 0u] * inv_32768;
        g_audio_r[i] = (float)g_pcm_i16[i * 2u + 1u] * inv_32768;
    }
}

WASM_EXPORT float *adrift_web_audio_l_ptr(void) {
    return g_audio_l;
}

WASM_EXPORT float *adrift_web_audio_r_ptr(void) {
    return g_audio_r;
}

WASM_EXPORT void adrift_web_tick_ui(uint8_t cpu_load_pct) {
    adrift_input_state_t in_st;
    for (uint32_t e = 0u; e < ADRIFT_NUM_ENC; e++) {
        int32_t d = g_enc_acc[e];
        if (d > 64) d = 64;
        if (d < -64) d = -64;
        in_st.enc_delta[e] = (int8_t)d;
        g_enc_acc[e] = 0;
    }

    uint32_t hw_eff = g_hw_btn_down | g_hw_btn_pulse;
    uint32_t cur_b  = map_hw_buttons(hw_eff) | g_log_btn_pulse;
    uint32_t hold_b = map_hw_buttons(g_hw_btn_down);

    in_st.buttons_down     = cur_b;
    in_st.buttons_pressed  = cur_b & ~g_prev_log_btn;
    in_st.buttons_released = g_prev_log_btn & ~cur_b;
    g_prev_log_btn         = hold_b;
    g_hw_btn_pulse         = 0u;
    g_log_btn_pulse        = 0u;

    in_st.master_pot   = g_master_pot;
    in_st.cpu_load_pct = cpu_load_pct;
    in_st.text_char    = g_text_char;
    in_st.text_cmd     = g_text_cmd;
    g_text_char        = 0;
    g_text_cmd         = ADRIFT_TEXT_CMD_NONE;

    adrift_dirty_t dirty;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, g_rgb565_fb, &dirty);
    (void)adrift_preset_store_flush_pending();

    const adrift_ui_t *ui = adrift_app_get_ui();
    g_web_boot_frame = ui->boot_active ? ui->boot_frame : 0u;
    g_leds_buttons   = ui->boot_active ? 0u : compute_button_leds(hw_eff);

    /* Expand RGB565 framebuffer to RGBA8888 for HTML5 Canvas ImageData */
    const uint32_t total_px = ADRIFT_SCREEN_W * ADRIFT_SCREEN_H;
    for (uint32_t i = 0u; i < total_px; i++) {
        uint32_t c  = g_rgb565_fb[i];
        uint32_t r5 = (c >> 11) & 0x1Fu;
        uint32_t g6 = (c >> 5)  & 0x3Fu;
        uint32_t b5 =  c        & 0x1Fu;
        uint32_t o  = i * 4u;
        g_rgba_fb[o + 0u] = (uint8_t)((r5 << 3) | (r5 >> 2));
        g_rgba_fb[o + 1u] = (uint8_t)((g6 << 2) | (g6 >> 4));
        g_rgba_fb[o + 2u] = (uint8_t)((b5 << 3) | (b5 >> 2));
        g_rgba_fb[o + 3u] = 255u;
    }
}

WASM_EXPORT uint8_t *adrift_web_rgba_ptr(void) {
    return g_rgba_fb;
}

WASM_EXPORT uint32_t adrift_web_leds_buttons(void) {
    return g_leds_buttons;
}

WASM_EXPORT uint32_t adrift_web_leds_keys(void) {
    return g_sounding_keys;
}

WASM_EXPORT uint32_t adrift_web_active_group(void) {
    return (uint32_t)(adrift_app_get_synth()->edit_group % ADRIFT_MAX_GROUPS);
}

WASM_EXPORT uint32_t adrift_web_active_preset(void) {
    return (uint32_t)adrift_app_get_synth()->preset_index;
}

WASM_EXPORT uint32_t adrift_web_in_save_menu(void) {
    return adrift_app_get_ui()->in_save_menu ? 1u : 0u;
}

WASM_EXPORT uint32_t adrift_web_boot_frame(void) {
    return g_web_boot_frame;
}

WASM_EXPORT void adrift_web_set_boot_frame(uint8_t frame) {
    adrift_ui_t *ui = adrift_app_get_ui();
    if (frame >= ADRIFT_UI_BOOT_FRAMES) {
        adrift_ui_dismiss_boot(ui);
        g_web_boot_frame = 0u;
    } else {
        ui->boot_active  = true;
        ui->boot_frame   = frame;
        ui->initialized  = false;
        g_web_boot_frame = frame;
    }
}
