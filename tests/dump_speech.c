/* SPDX-License-Identifier: MPL-2.0
 * Exhaustive Screen-Reader Utterance Dumper for Adrift Accessibility Audit (tools/audit.mjs) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/app.h"
#include "../src/platform/platform.h"
#include "../src/platform/platform_headless.h"

static const char *cat_name(adrift_sr_cat_t cat) {
    switch (cat) {
        case ADRIFT_SR_CAT_ACTION:   return "ACTION";
        case ADRIFT_SR_CAT_SCREEN:   return "SCREEN";
        case ADRIFT_SR_CAT_READOUT:  return "READOUT";
        case ADRIFT_SR_CAT_LIST:     return "LIST";
        case ADRIFT_SR_CAT_QUESTION: return "QUESTION";
        case ADRIFT_SR_CAT_WARNING:  return "WARNING";
        case ADRIFT_SR_CAT_WHERE:    return "WHERE";
        case ADRIFT_SR_CAT_EVENT:    return "EVENT";
        default:                     return "OTHER";
    }
}

static const char *prio_name(adrift_sr_prio_t prio) {
    switch (prio) {
        case ADRIFT_SR_PRIO_QUEUE: return "QUEUE";
        case ADRIFT_SR_PRIO_NOW:   return "NOW";
        case ADRIFT_SR_PRIO_KEEP:  return "KEEP";
        default:                   return "NOW";
    }
}

typedef struct {
    FILE    *fp;
    uint32_t rows;
} dump_ctx_t;

static void dump_logger(const char *ctx,
                        adrift_sr_cat_t cat,
                        adrift_sr_prio_t prio,
                        const char *text,
                        void *user) {
    dump_ctx_t *dc = (dump_ctx_t *)user;
    if (!dc || !dc->fp || !text) return;
    fprintf(dc->fp, "%s\t%s\t%s\t%s\n",
            ctx ? ctx : "ui", cat_name(cat), prio_name(prio), text);
    dc->rows++;
}

static uint16_t g_fb[ADRIFT_SCREEN_W * ADRIFT_SCREEN_H];

static void drain_sr(void) {
    adrift_screen_reader_t *sr = adrift_app_get_screen_reader();
    adrift_dirty_t dirty;
    adrift_input_state_t idle;
    memset(&idle, 0, sizeof(idle));
    idle.master_pot = -1;

    /* Advance settle timers (>= 220 ms) so pending knob/page announcements fire */
    for (int i = 0; i < 15; i++) {
        adrift_dirty_clear(&dirty);
        adrift_app_tick_ui(&idle, g_fb, &dirty);
    }

    /* Render audio out until TTS + Klatt + queue finish completely */
    int16_t pcm[512];
    for (int i = 0; i < 800; i++) {
        if (!sr->tts.active && !adrift_klatt_is_speaking(&sr->klatt) && sr->q_count == 0u) {
            break;
        }
        memset(pcm, 0, sizeof(pcm));
        adrift_app_mix_line_out(pcm, 256u);
    }
}

static void step_btn(uint32_t btn, const char *ctx_tag) {
    adrift_sr_set_log_context(adrift_app_get_screen_reader(), ctx_tag);
    adrift_dirty_t dirty;
    adrift_input_state_t in;
    memset(&in, 0, sizeof(in));
    in.master_pot      = -1;
    in.buttons_down    = btn;
    in.buttons_pressed = btn;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in, g_fb, &dirty);

    memset(&in, 0, sizeof(in));
    in.master_pot       = -1;
    in.buttons_released = btn;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in, g_fb, &dirty);
    drain_sr();
}

static void step_enc(uint8_t enc_idx, int8_t delta, const char *ctx_tag) {
    adrift_sr_set_log_context(adrift_app_get_screen_reader(), ctx_tag);
    adrift_dirty_t dirty;
    adrift_input_state_t in;
    memset(&in, 0, sizeof(in));
    in.master_pot = -1;
    in.enc_delta[enc_idx] = delta;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in, g_fb, &dirty);
    drain_sr();
}

static void trigger_where(const char *ctx_tag) {
    adrift_screen_reader_t *sr = adrift_app_get_screen_reader();
    adrift_sr_set_log_context(sr, ctx_tag);
    bool rel = false;
    (void)adrift_sr_handle_panel_key(sr, adrift_app_get_ui(), adrift_app_get_synth(),
                                     ADRIFT_SR_KEY_MOD_BK11, true, &rel);
    (void)adrift_sr_handle_panel_key(sr, adrift_app_get_ui(), adrift_app_get_synth(),
                                     ADRIFT_SR_KEY_WHERE_BK7, true, &rel);
    (void)adrift_sr_handle_panel_key(sr, adrift_app_get_ui(), adrift_app_get_synth(),
                                     ADRIFT_SR_KEY_WHERE_BK7, false, &rel);
    (void)adrift_sr_handle_panel_key(sr, adrift_app_get_ui(), adrift_app_get_synth(),
                                     ADRIFT_SR_KEY_MOD_BK11, false, &rel);
    drain_sr();
}

int main(int argc, char **argv) {
    const char *out_path = (argc > 1) ? argv[1] : "build/test/said.tsv";
    FILE *fp = fopen(out_path, "w");
    if (!fp) {
        fprintf(stderr, "dump_speech: cannot open %s for writing\n", out_path);
        return 1;
    }

    adrift_headless_storage_reset();
    adrift_app_init();
    adrift_ui_t            *ui    = adrift_app_get_ui();
    adrift_synth_t         *synth = adrift_app_get_synth();
    adrift_screen_reader_t *sr    = adrift_app_get_screen_reader();
    adrift_ui_dismiss_boot(ui);

    dump_ctx_t dctx = { fp, 0u };
    adrift_sr_set_logger(sr, dump_logger, &dctx, "boot");

    /* 1. Enable screen reader via BK11 + BK6, adjust speeds via BK11 + BK5/BK4 */
    bool rel = false;
    adrift_sr_set_log_context(sr, "chord_toggle_on");
    (void)adrift_sr_handle_panel_key(sr, ui, synth, ADRIFT_SR_KEY_MOD_BK11, true, &rel);
    (void)adrift_sr_handle_panel_key(sr, ui, synth, ADRIFT_SR_KEY_TOGGLE_BK6, true, &rel);
    (void)adrift_sr_handle_panel_key(sr, ui, synth, ADRIFT_SR_KEY_TOGGLE_BK6, false, &rel);
    drain_sr();

    adrift_sr_set_log_context(sr, "chord_speed");
    (void)adrift_sr_handle_panel_key(sr, ui, synth, ADRIFT_SR_KEY_FASTER_BK5, true, &rel);
    (void)adrift_sr_handle_panel_key(sr, ui, synth, ADRIFT_SR_KEY_FASTER_BK5, false, &rel);
    drain_sr();
    (void)adrift_sr_handle_panel_key(sr, ui, synth, ADRIFT_SR_KEY_SLOWER_BK4, true, &rel);
    (void)adrift_sr_handle_panel_key(sr, ui, synth, ADRIFT_SR_KEY_SLOWER_BK4, false, &rel);
    (void)adrift_sr_handle_panel_key(sr, ui, synth, ADRIFT_SR_KEY_MOD_BK11, false, &rel);
    drain_sr();

    /* 2. Exercise all 5 Main Parameter pages, subpages, mapped & unmapped knobs, and WHERE */
    const uint32_t page_btns[5] = {
        ADRIFT_BTN_PAGE_1, ADRIFT_BTN_PAGE_2, ADRIFT_BTN_PAGE_3, ADRIFT_BTN_PAGE_4, ADRIFT_BTN_PAGE_5
    };
    const uint8_t page_subs[5] = { 3u, 2u, 2u, 2u, 4u };
    for (uint8_t p = 0u; p < 5u; p++) {
        for (uint8_t sub = 0u; sub < page_subs[p]; sub++) {
            char tag[32];
            snprintf(tag, sizeof(tag), "page%u_sub%u", (unsigned)(p + 1u), (unsigned)(sub + 1u));
            step_btn(page_btns[p], tag);
            trigger_where(tag);
            for (uint8_t k = 0u; k < 4u; k++) {
                char ktag[40];
                snprintf(ktag, sizeof(ktag), "page%u_sub%u_knob%u",
                         (unsigned)(p + 1u), (unsigned)(sub + 1u), (unsigned)(k + 1u));
                step_enc(k, 1, ktag);
            }
        }
    }

    /* Walk all 23 LFO1 modulation destinations & LFO waves on MOD page 1/4 */
    step_btn(ADRIFT_BTN_PAGE_5, "mod_dest_sweep");
    for (int i = 0; i < 6; i++) step_enc(ADRIFT_ENC_KNOB1, 1, "mod_lfo_waves");
    for (int i = 0; i < 23; i++) step_enc(ADRIFT_ENC_KNOB4, 1, "mod_all_dests");

    /* Walk ENV1 loop modes & time ranges on ENV1 page 2/2 */
    step_btn(ADRIFT_BTN_PAGE_2, "env1_p1");
    step_btn(ADRIFT_BTN_PAGE_2, "env1_p2");
    for (int i = 0; i < 3; i++) step_enc(ADRIFT_ENC_KNOB3, 1, "env1_loop_modes");
    for (int i = 0; i < 3; i++) step_enc(ADRIFT_ENC_KNOB4, 1, "env1_ranges");

    /* 3. Group Settings (1/2, 2/2), voice limit warning, pan/assign/legato, group cycling */
    for (uint8_t sub = 0u; sub < 2u; sub++) {
        step_btn(ADRIFT_BTN_GROUP_CFG, "group_cfg");
        trigger_where("group_cfg_where");
        for (uint8_t k = 0u; k < 4u; k++) {
            step_enc(k, 1, "group_cfg_knob");
        }
    }
    /* Trigger all-16-voices-in-use ANN_KEEP warning */
    step_btn(ADRIFT_BTN_GROUP_CFG, "group_cfg_p1"); /* back to 1/2 */
    synth->groups[0].num_voices = 10u;
    synth->groups[1].num_voices = 6u;
    for (uint8_t g = 2u; g < ADRIFT_MAX_GROUPS; g++) synth->groups[g].num_voices = 0u;
    synth->edit_group = 0u;
    step_enc(ADRIFT_ENC_KNOB1, 1, "group_voice_limit_warn");

    /* Cycle active and off groups */
    step_enc(ADRIFT_ENC_SELECT, 1, "select_group_2");
    step_btn(ADRIFT_BTN_NEXT_GROUP, "next_group_3_off");
    synth->edit_group = 0u;

    /* 4. Arpeggiator (1/2, 2/2), all 11 modes, octave range, tempo, gate, vel, latch, swing & all 8 ratchets */
    step_btn(ADRIFT_BTN_ARP, "arp_p1");
    trigger_where("arp_p1_where");
    for (int i = 0; i < 11; i++) step_enc(ADRIFT_ENC_KNOB1, 1, "arp_modes");
    for (int i = 0; i < 3; i++) step_enc(ADRIFT_ENC_KNOB2, 1, "arp_octaves");
    step_enc(ADRIFT_ENC_KNOB3, 2, "arp_tempo");
    step_enc(ADRIFT_ENC_KNOB4, 1, "arp_gate");

    step_btn(ADRIFT_BTN_ARP, "arp_p2");
    trigger_where("arp_p2_where");
    step_enc(ADRIFT_ENC_KNOB1, 2, "arp_vel_fixed");
    step_enc(ADRIFT_ENC_KNOB1, -2, "arp_vel_key");
    step_enc(ADRIFT_ENC_KNOB2, 1, "arp_latch_on");
    step_enc(ADRIFT_ENC_KNOB2, -1, "arp_latch_off");
    step_enc(ADRIFT_ENC_KNOB3, 2, "arp_swing");
    for (int i = 0; i < 8; i++) step_enc(ADRIFT_ENC_KNOB4, 1, "arp_ratchets");

    /* 5. Master Effects (1/3, 2/3, 3/3) */
    for (uint8_t sub = 0u; sub < 3u; sub++) {
        step_btn(ADRIFT_BTN_FX, "fx_page");
        trigger_where("fx_where");
        for (uint8_t k = 0u; k < 4u; k++) {
            step_enc(k, 2, "fx_knob");
        }
    }

    /* 6. All 6 HOME visualization screens & all 18 display modes */
    for (uint8_t hp = 0u; hp < 6u; hp++) {
        step_btn(ADRIFT_BTN_HOME, "home_page");
        trigger_where("home_where");
        step_enc(ADRIFT_ENC_KNOB1, 1, "home_zoom");
        step_enc(ADRIFT_ENC_KNOB2, 1, "home_gain");
        step_enc(ADRIFT_ENC_KNOB3, 1, "home_freeze_on");
        step_enc(ADRIFT_ENC_KNOB3, -1, "home_freeze_off");
        for (int m = 0; m < 3; m++) {
            step_enc(ADRIFT_ENC_KNOB4, 1, "home_mode");
        }
    }

    /* 7. Octave shifts + limits, Panic, Hold */
    step_btn(ADRIFT_BTN_PAGE_1, "back_to_exc");
    for (int i = 0; i < 4; i++) step_btn(ADRIFT_BTN_OCT_DOWN, "octave_down");
    synth->kbd_octave = 2;
    step_btn(ADRIFT_BTN_OCT_UP, "octave_up_3");
    step_btn(ADRIFT_BTN_OCT_UP, "octave_up_limit");
    synth->kbd_octave = 0;
    step_btn(ADRIFT_BTN_PANIC, "panic");
    step_btn(ADRIFT_BTN_HOLD, "drone_toggle_1");
    step_btn(ADRIFT_BTN_HOLD, "drone_toggle_2");

    /* 8. Scale Browser: open, set root to 'A' (9) to exercise Rule 3 ('A minor', 'A dorian', etc.), scroll all 16 scales */
    step_enc(ADRIFT_ENC_ALGORITHM, 1, "scale_open");
    for (int r = 0; r < 9; r++) step_enc(ADRIFT_ENC_KNOB1, 1, "scale_root"); /* C -> A */
    for (int s = 0; s < 16; s++) step_enc(ADRIFT_ENC_ALGORITHM, 1, "scale_row");
    step_enc(ADRIFT_ENC_KNOB2, 1, "scale_unused_knob");
    trigger_where("scale_where");
    step_btn(ADRIFT_BTN_OCT_DOWN, "scale_cancel");
    step_enc(ADRIFT_ENC_ALGORITHM, 1, "scale_reopen");
    step_enc(ADRIFT_ENC_ALGORITHM, 2, "scale_pick_minor");
    step_btn(ADRIFT_BTN_OCT_UP, "scale_confirm");

    /* 9. Save Preset Modal: open, slot scroll, cursor, chars, clear/restore, save, save-fail */
    step_btn(ADRIFT_BTN_SAVE_PRESET, "save_open");
    trigger_where("save_where");
    step_enc(ADRIFT_ENC_KNOB1, 1, "save_slot_2");
    step_enc(ADRIFT_ENC_KNOB1, -1, "save_slot_1");
    step_enc(ADRIFT_ENC_KNOB2, 1, "save_cursor_move");
    step_enc(ADRIFT_ENC_KNOB3, 1, "save_char_turn");
    step_enc(ADRIFT_ENC_KNOB4, 1, "save_clear_end");
    {
        adrift_dirty_t dirty;
        adrift_input_state_t in;
        memset(&in, 0, sizeof(in));
        in.master_pot = -1;
        in.text_cmd   = ADRIFT_TEXT_CMD_CLEAR;
        adrift_sr_set_log_context(sr, "save_clear_all");
        adrift_dirty_clear(&dirty);
        adrift_app_tick_ui(&in, g_fb, &dirty);
        drain_sr();
    }
    step_enc(ADRIFT_ENC_KNOB4, -1, "save_restore_name");
    step_btn(ADRIFT_BTN_OCT_UP, "save_confirm_u1");

    /* Exercise save cancel & simulated flash write failure warning */
    step_btn(ADRIFT_BTN_SAVE_PRESET, "save_open_again");
    step_btn(ADRIFT_BTN_OCT_DOWN, "save_cancel");
    adrift_sr_set_log_context(sr, "save_flash_fail");
    adrift_sr_announce_save_result(sr, 0u, "ACOUSTIC GTR", false);
    drain_sr();

    /* 10. Presets Browser: all 50 rows (Actions + 24 Factory + 24 User), banks, load modes, clear question */
    step_enc(ADRIFT_ENC_PRESETS, 0, "presets_noop");
    step_btn(ADRIFT_BTN_NEXT_PST, "presets_open");
    for (int i = 0; i < 50; i++) {
        step_enc(ADRIFT_ENC_PRESETS, 1, "presets_scroll_all");
    }
    trigger_where("presets_where");

    /* Bank switches */
    step_enc(ADRIFT_ENC_KNOB1, 1, "presets_bank_user");
    step_enc(ADRIFT_ENC_KNOB1, 1, "presets_bank_fact");
    step_enc(ADRIFT_ENC_KNOB1, 1, "presets_bank_all");

    /* Blocked source group warning in FULL mode, then switch to GRP mode & change source group */
    step_enc(ADRIFT_ENC_KNOB3, 1, "presets_src_blocked");
    step_enc(ADRIFT_ENC_KNOB2, 1, "presets_mode_grp");
    step_enc(ADRIFT_ENC_KNOB3, 1, "presets_src_grp_change");
    step_enc(ADRIFT_ENC_KNOB2, -1, "presets_mode_full");

    /* Blocked clear warning on Factory preset, then scroll to occupied User 1 -> arm/cancel/confirm clear */
    ui->preset_cursor = 2u; /* Factory 1 */
    step_enc(ADRIFT_ENC_KNOB4, 1, "presets_clear_blocked");

    ui->preset_cursor = ADRIFT_UI_PRESET_FACT_COUNT; /* Occupied User 1 */
    step_enc(ADRIFT_ENC_KNOB4, 1, "presets_clear_arm_question");
    step_enc(ADRIFT_ENC_KNOB4, -1, "presets_clear_disarm");
    step_enc(ADRIFT_ENC_KNOB4, 1, "presets_clear_rearm");
    step_btn(ADRIFT_BTN_OCT_UP, "presets_clear_confirm");

    /* Confirm Factory 1 load, then reopen and cancel */
    step_btn(ADRIFT_BTN_NEXT_PST, "presets_reopen");
    ui->preset_cursor = 2u;
    step_btn(ADRIFT_BTN_OCT_UP, "presets_load_confirm");
    step_btn(ADRIFT_BTN_NEXT_PST, "presets_reopen_cancel");
    step_btn(ADRIFT_BTN_OCT_DOWN, "presets_cancel");

    /* 11. Queued background events: external MIDI clock lock, synced delay divisions & clock stop */
    adrift_sr_set_log_context(sr, "bg_midi_clock");
    synth->midi_clk_active = true;
    synth->midi_clk_bpm    = 124u;
    drain_sr();
    step_btn(ADRIFT_BTN_FX, "fx_sync_open");
    synth->fx_params.delay_time = 0;
    for (int i = 0; i < 8; i++) {
        step_enc(ADRIFT_ENC_KNOB2, 8, "fx_sync_subdiv");
    }
    adrift_sr_set_log_context(sr, "bg_midi_clock");
    synth->midi_clk_active = false;
    drain_sr();

    /* 12. Toggle screen reader OFF via BK11 + BK6 */
    adrift_sr_set_log_context(sr, "chord_toggle_off");
    (void)adrift_sr_handle_panel_key(sr, ui, synth, ADRIFT_SR_KEY_MOD_BK11, true, &rel);
    (void)adrift_sr_handle_panel_key(sr, ui, synth, ADRIFT_SR_KEY_TOGGLE_BK6, true, &rel);
    (void)adrift_sr_handle_panel_key(sr, ui, synth, ADRIFT_SR_KEY_TOGGLE_BK6, false, &rel);
    (void)adrift_sr_handle_panel_key(sr, ui, synth, ADRIFT_SR_KEY_MOD_BK11, false, &rel);
    drain_sr();

    fclose(fp);
    printf("  [dump]   Wrote %u utterances to %s\n", dctx.rows, out_path);
    return 0;
}
