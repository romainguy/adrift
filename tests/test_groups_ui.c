/* SPDX-License-Identifier: MPL-2.0
 * Factory Presets, 240x240 UI Screens, Voice Groups G1..G6 & Preset Browser Tests */

#include "test_common.h"

/* ---- 7. Render Factory Preset Reference WAVs & UI Dirty-Tracking Smoke Test ---- */
void test_presets_and_ui(const char *wav_dir) {
    printf("[Test 7] Rendering %u Factory Presets (.wav) & Verifying 240x240 UI...\n", ADRIFT_NUM_PRESETS);
    const uint32_t N_NOTE = 33088u; /* 0.75 s held */
    const uint32_t N_TAIL = 11030u; /* 0.25 s release */
    const uint32_t N_TOT  = N_NOTE + N_TAIL;
    int16_t *pcm = calloc(N_TOT * 2u, sizeof(int16_t));
    uint16_t *fb = calloc(ADRIFT_SCREEN_W * ADRIFT_SCREEN_H, sizeof(uint16_t));

    for (uint8_t p_idx = 0; p_idx < ADRIFT_NUM_PRESETS; p_idx++) {
        adrift_app_init();
        adrift_synth_t *s = adrift_app_get_synth();
        adrift_synth_load_preset(s, p_idx);
        CHECK(strlen(s->preset_name) <= 12u,
              "Preset %u (%s) exceeds 12 characters (%zu)", p_idx, s->preset_name, strlen(s->preset_name));

        adrift_midi_msg_t on_ev = { 0x90u, 57u, 110u, 0u }; /* A3 = 220 Hz */
        adrift_headless_render(pcm, N_NOTE, &on_ev, 1u);

        adrift_midi_msg_t off_ev = { 0x80u, 57u, 0u, 0u };
        adrift_headless_render(pcm + 2u * N_NOTE, N_TAIL, &off_ev, 1u);

        double rms = compute_rms_dbfs(pcm, 0u, N_TOT);
        CHECK(rms > -38.0, "Preset %u (%s) produced silence (rms=%.1f dBFS)",
              p_idx, s->preset_name, rms);

        if (wav_dir) {
            char path[512];
            snprintf(path, sizeof(path), "%s/preset_%u.wav", wav_dir, p_idx);
            CHECK(adrift_headless_write_wav(path, pcm, N_TOT, 2u, ADRIFT_SAMPLE_RATE),
                  "Failed to write WAV: %s", path);
        }

        /* Exercise UI screens & both subpages (1/2 and 2/2) across all 5 parameter screens + HOME scope */
        for (uint8_t pg = 0; pg < 5u; pg++) {
            for (uint8_t sp = 0; sp < 2u; sp++) {
                adrift_input_state_t in_st;
                memset(&in_st, 0, sizeof(in_st));
                in_st.buttons_pressed = (1u << pg);
                in_st.enc_delta[0]    = 2;
                in_st.enc_delta[2]    = -1;
                in_st.master_pot      = -1;
                in_st.cpu_load_pct    = 6u;
                adrift_dirty_t dirty;
                adrift_dirty_clear(&dirty);
                adrift_app_tick_ui(&in_st, fb, &dirty);
            }
        }
    }

    free(fb);
    free(pcm);
    printf("  -> PASS (All %u presets rendered & UI pages 1-5 verified)\n", ADRIFT_NUM_PRESETS);
}

/* ---- 8. Voice Groups (G1..G6), SAVE Sub-Menu, Round-Robin vs Unison & 16-Voice Multi-Channel MIDI ---- */
void test_voice_groups_and_save_menu(const char *wav_dir) {
    printf("[Test 8] Voice Groups (16 Voices, G1=3 Default, SAVE Menu, Free-Pool Cap, Unison & MIDI Ch)...\n");
    CHECK(ADRIFT_MAX_VOICES == 16 && ADRIFT_MAX_GROUPS == 6,
          "Expected 16 voices and 6 groups");

    const uint32_t N = 22059u; /* 0.5 s */
    int16_t  *pcm = calloc(N * 2u, sizeof(int16_t));
    uint16_t *fb  = calloc(ADRIFT_SCREEN_W * ADRIFT_SCREEN_H, sizeof(uint16_t));

    adrift_app_init();
    adrift_synth_t *s  = adrift_app_get_synth();
    adrift_ui_t    *ui = adrift_app_get_ui();
    adrift_input_state_t in_st;
    adrift_dirty_t dirty;

    /* 8a. Default allocation: G1 = 3 voices (Ch 1, Round-Robin), G2..G6 = 0 voices (Ch 2..6) */
    CHECK(s->edit_group == 0u, "Expected initial edit_group == 0 (G1), got %u", s->edit_group);
    CHECK(s->groups[0].num_voices == 3u &&
          s->groups[0].assign_mode == ADRIFT_GROUP_ROUND_ROBIN &&
          s->groups[0].midi_channel == 1u,
          "G1 default config mismatch (voices=%u, mode=%u, ch=%u)",
          s->groups[0].num_voices, s->groups[0].assign_mode, s->groups[0].midi_channel);
    for (uint8_t g = 1u; g < ADRIFT_MAX_GROUPS; g++) {
        CHECK(s->groups[g].num_voices == 0u && s->groups[g].midi_channel == (uint8_t)(g + 1u),
              "G%u default config mismatch (voices=%u, ch=%u)",
              g + 1u, s->groups[g].num_voices, s->groups[g].midi_channel);
    }
    CHECK(adrift_synth_used_voices(s) == 3u && adrift_synth_free_voices(s) == 13u,
          "Expected used=3, free=13 (got used=%u, free=%u)",
          adrift_synth_used_voices(s), adrift_synth_free_voices(s));

    /* Disabled group (G2 on MIDI Ch 2 = 0x91) must ignore Note On and remain silent */
    adrift_midi_msg_t ch2_disabled = { 0x91u, 64u, 110u, 0u };
    adrift_headless_render(pcm, 2048u, &ch2_disabled, 1u);
    CHECK(compute_rms_dbfs(pcm, 0u, 2048u) < -120.0, "Disabled G2 produced audio on MIDI Ch 2");

    /* 8b. G1 Round-Robin allocates strictly within voices[0..2] and steals within G1 on 4th note */
    static const uint8_t k_triad[3] = { 48u, 52u, 55u }; /* C3, E3, G3 */
    for (int i = 0; i < 3; i++) {
        adrift_synth_handle_midi(s, 0x90u, k_triad[i], 112u);
    }
    for (uint8_t v = 0u; v < 3u; v++) {
        CHECK(s->voices[v].owner_group == 0 && s->voices[v].active && s->voices[v].note == k_triad[v],
              "G1 voice %u failed to allocate triad note", v);
    }
    for (uint8_t v = 3u; v < ADRIFT_MAX_VOICES; v++) {
        CHECK(s->voices[v].owner_group == -1 && !s->voices[v].active,
              "Free-pool voice %u should remain unassigned", v);
    }
    /* 4th note on Ch 1 steals oldest G1 voice (voice 0 -> B3=59), leaving free voices 3..15 untouched */
    adrift_synth_handle_midi(s, 0x90u, 59u, 115u);
    CHECK(s->voices[0].note == 59u && s->voices[3].owner_group == -1,
          "4th note on G1 did not steal within G1 (v0.note=%u, v3.owner=%d)",
          s->voices[0].note, s->voices[3].owner_group);
    adrift_synth_all_notes_off(s, true);

    /* 8c. SAVE (ADRIFT_BTN_GROUP_CFG) opens Group Settings (Page 1/2), pressing again cycles to Page 2/2 */
    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed = ADRIFT_BTN_GROUP_CFG;
    in_st.master_pot      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->in_group_menu && ui->subpage == 0u, "SAVE button did not enter Group Settings Page 1/2");

    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed = ADRIFT_BTN_GROUP_CFG;
    in_st.master_pot      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->in_group_menu && ui->subpage == 1u, "Second SAVE press did not advance Group Settings to Page 2/2");

    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed = ADRIFT_BTN_GROUP_CFG;
    in_st.master_pot      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->in_group_menu && ui->subpage == 0u, "Third SAVE press did not wrap Group Settings back to Page 1/2");

    /* Turn SELECT encoder (+1) -> G2 (edit_group == 1) and spin KNOB1 (VOICES) +20 clicks -> caps at 13 free voices! */
    memset(&in_st, 0, sizeof(in_st));
    in_st.enc_delta[ADRIFT_ENC_SELECT] = 1;
    in_st.enc_delta[ADRIFT_ENC_KNOB1]  = 20;
    in_st.master_pot                   = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(s->edit_group == 1u && s->groups[1].num_voices == 13u && s->groups[0].num_voices == 3u,
          "G2 voice allocation failed free-pool cap (G1=%u, G2=%u)",
          s->groups[0].num_voices, s->groups[1].num_voices);
    CHECK(adrift_synth_free_voices(s) == 0u, "Expected 0 free voices when G1=3, G2=13");

    /* Turn SELECT encoder (+1) -> G3 (edit_group == 2): attempting to add voices when free==0 must stay at 0! */
    memset(&in_st, 0, sizeof(in_st));
    in_st.enc_delta[ADRIFT_ENC_SELECT] = 1;
    in_st.enc_delta[ADRIFT_ENC_KNOB1]  = 4;
    in_st.master_pot                   = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(s->edit_group == 2u && s->groups[2].num_voices == 0u,
          "G3 stole voices when free pool was 0 (G3=%u)", s->groups[2].num_voices);

    /* Partition 16 voices across G1=6, G2=6, G3=4 */
    adrift_synth_set_group_voices(s, 1u, 6u);
    adrift_synth_set_group_voices(s, 0u, 6u);
    CHECK(adrift_synth_free_voices(s) == 4u, "Expected 4 free voices when G1=6, G2=6");
    memset(&in_st, 0, sizeof(in_st));
    in_st.enc_delta[ADRIFT_ENC_KNOB1] = 8; /* G3 claims the remaining 4 free voices and caps at 4 */
    in_st.master_pot                  = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(s->groups[0].num_voices == 6u &&
          s->groups[1].num_voices == 6u &&
          s->groups[2].num_voices == 4u &&
          adrift_synth_free_voices(s) == 0u,
          "Expected G1=6, G2=6, G3=4, free=0");

    /* 8d. Exit SAVE menu via SEL (EXC Page 1/2), press SEL again for EXC Page 2/2, edit G3 FINE via KNOB1 */
    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed = ADRIFT_BTN_PAGE_1;
    in_st.master_pot      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(!ui->in_group_menu && ui->page == 0u && ui->subpage == 0u, "SEL button did not open EXC Page 1/2");

    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed             = ADRIFT_BTN_PAGE_1; /* Advance EXC to Page 2/2 (FINE, MIX, SH CLK, PLUCK) */
    in_st.enc_delta[ADRIFT_ENC_KNOB1] = 15;                /* G3 EXC Page 2/2 KNOB1 (FINE) = +30 cents */
    in_st.enc_delta[ADRIFT_ENC_KNOB4] = 20;                /* G3 EXC Page 2/2 KNOB4 (PLUCK) = +240 */
    in_st.master_pot                  = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->page == 0u && ui->subpage == 1u, "Second SEL press did not advance EXC to Page 2/2");
    CHECK(s->groups[2].patch.exc.vco_fine == 30 &&
          s->groups[2].patch.exc.pluck == 240 &&
          s->groups[0].patch.exc.vco_fine == 0 &&
          s->groups[0].patch.exc.pluck == 0,
          "Per-group parameter edit on Page 2/2 failed or leaked between groups");
    s->groups[2].patch.exc.pluck = 0;

    /* Verify the 4th cell on EXC Page 2/3 (PLUCK) has a header band */
    uint16_t exc_card4_hdr_px = fb[145u * ADRIFT_SCREEN_W + 190u];
    uint16_t exc_card4_int_px = fb[185u * ADRIFT_SCREEN_W + 190u];
    CHECK(exc_card4_hdr_px != exc_card4_int_px,
          "Expected 4th cell (PLUCK) on EXC Page 2/3 to have a header band");

    /* Advance EXC to Page 3/3 (DRIFT, KEYTRK, empty, empty) and verify edits + wrap to Page 1/3 */
    int16_t g3_drift0 = s->groups[2].patch.exc.analog_drift;
    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed             = ADRIFT_BTN_PAGE_1; /* Advance EXC to Page 3/3 (DRIFT, KEYTRK) */
    in_st.enc_delta[ADRIFT_ENC_KNOB1] = 10;                /* KNOB1 (DRIFT) = +120 */
    in_st.enc_delta[ADRIFT_ENC_KNOB2] = -1;                /* KNOB2 (KEYTRK) = FIXED (false) */
    in_st.enc_delta[ADRIFT_ENC_KNOB3] = 20;                /* Unmapped slot 2 on EXC Page 3/3 -> ignored */
    in_st.master_pot                  = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->page == 0u && ui->subpage == 2u &&
          s->groups[2].patch.exc.analog_drift == g3_drift0 + 120 &&
          !s->groups[2].patch.exc.vco_keytrack,
          "EXC Page 3/3 KNOB1/KNOB2 did not edit DRIFT/KEYTRK");
    uint16_t exc3_c2_hdr = fb[145u * ADRIFT_SCREEN_W + 78u];
    uint16_t exc3_c2_int = fb[185u * ADRIFT_SCREEN_W + 78u];
    uint16_t exc3_c3_hdr = fb[145u * ADRIFT_SCREEN_W + 136u];
    uint16_t exc3_c3_int = fb[185u * ADRIFT_SCREEN_W + 136u];
    CHECK(exc3_c2_hdr != exc3_c2_int && exc3_c3_hdr == exc3_c3_int,
          "Expected EXC Page 3/3 card 2 (KEYTRK) to have header band and card 3 to be empty");
    s->groups[2].patch.exc.analog_drift = g3_drift0;
    s->groups[2].patch.exc.vco_keytrack = true;

    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed = ADRIFT_BTN_PAGE_1; /* 4th SEL press wraps EXC back to Page 1/3 */
    in_st.master_pot      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->page == 0u && ui->subpage == 0u, "4th SEL press did not wrap EXC to Page 1/3");

    /* Verify ENV1 (ADRIFT_BTN_PAGE_2) and ENV2 (ADRIFT_BTN_PAGE_4) have RELEAS on Page 1/2 KNOB4 and DELAY on Page 2/2 KNOB1 */
    int16_t e1_rel0 = s->groups[2].patch.env1.release;
    int16_t e1_dly0 = s->groups[2].patch.env1.onset_delay;
    uint8_t e1_rng0 = s->groups[2].patch.env1.range;
    s->groups[2].patch.env1.range = ADRIFT_ENV_RANGE_NORM;
    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed             = ADRIFT_BTN_PAGE_2; /* Open ENV1 Page 1/2 */
    in_st.enc_delta[ADRIFT_ENC_KNOB4] = 5;                 /* Page 1/2 KNOB4 = RELEAS (+60) */
    in_st.master_pot                  = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->page == 1u && ui->subpage == 0u && s->groups[2].patch.env1.release == e1_rel0 + 60,
          "ENV1 Page 1/2 KNOB4 did not edit RELEAS");

    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed             = ADRIFT_BTN_PAGE_2; /* Cycle to ENV1 Page 2/2 */
    in_st.enc_delta[ADRIFT_ENC_KNOB1] = 4;                 /* Page 2/2 KNOB1 = DELAY (+48) */
    in_st.enc_delta[ADRIFT_ENC_KNOB4] = 1;                 /* Page 2/2 KNOB4 = RANGE (NORM -> LONG) */
    in_st.master_pot                  = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->page == 1u && ui->subpage == 1u &&
          s->groups[2].patch.env1.onset_delay == e1_dly0 + 48 &&
          s->groups[2].patch.env1.range == ADRIFT_ENV_RANGE_LONG,
          "ENV1 Page 2/2 KNOB1/KNOB4 did not edit DELAY/RANGE");

    /* Verify the 4th cell on ENV1 Page 2/2 (8:RANGE) renders a header band */
    uint16_t card1_hdr_px = fb[145u * ADRIFT_SCREEN_W + 190u];
    uint16_t card1_int_px = fb[185u * ADRIFT_SCREEN_W + 190u];
    CHECK(card1_hdr_px != card1_int_px,
          "Expected 4th cell (RANGE) on ENV1 Page 2/2 to have a header band");
    s->groups[2].patch.env1.release     = e1_rel0;
    s->groups[2].patch.env1.onset_delay = e1_dly0;
    s->groups[2].patch.env1.range       = e1_rng0;

    int16_t e2_rel0 = s->groups[2].patch.env2.release;
    int16_t e2_dly0 = s->groups[2].patch.env2.onset_delay;
    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed             = ADRIFT_BTN_PAGE_4; /* Open ENV2 Page 1/2 */
    in_st.enc_delta[ADRIFT_ENC_KNOB4] = 5;                 /* Page 1/2 KNOB4 = RELEAS (+60) */
    in_st.master_pot                  = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->page == 3u && ui->subpage == 0u && s->groups[2].patch.env2.release == e2_rel0 + 60,
          "ENV2 Page 1/2 KNOB4 did not edit RELEAS");

    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed             = ADRIFT_BTN_PAGE_4; /* Cycle to ENV2 Page 2/2 */
    in_st.enc_delta[ADRIFT_ENC_KNOB1] = 4;                 /* Page 2/2 KNOB1 = DELAY (+48) */
    in_st.enc_delta[ADRIFT_ENC_KNOB4] = 25;                /* Page 2/2 KNOB4 (Empty 4th cell on ENV2) = ignored */
    in_st.master_pot                  = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->page == 3u && ui->subpage == 1u && s->groups[2].patch.env2.onset_delay == e2_dly0 + 48,
          "ENV2 Page 2/2 KNOB1 did not edit DELAY");

    /* Verify the 4th cell on ENV2 Page 2/2 (x=179..234, y=139..235) is rendered empty (no header band) */
    uint16_t card3_hdr_px = fb[145u * ADRIFT_SCREEN_W + 190u];
    uint16_t card3_int_px = fb[185u * ADRIFT_SCREEN_W + 190u];
    CHECK(card3_hdr_px == card3_int_px,
          "Expected 4th cell on ENV2 Page 2/2 to be empty without header band (hdr=0x%04x, int=0x%04x)",
          card3_hdr_px, card3_int_px);
    s->groups[2].patch.env2.release     = e2_rel0;
    s->groups[2].patch.env2.onset_delay = e2_dly0;

    /* Multi-channel 16-voice trigger: Ch 1 -> G1 (6 notes), Ch 2 -> G2 (6 notes), Ch 3 -> G3 (4 notes) */
    adrift_midi_msg_t multi_evs[16] = {
        { 0x90u, 48u, 110u, 0u  }, { 0x90u, 52u, 110u, 2u  }, { 0x90u, 55u, 110u, 4u  },
        { 0x90u, 59u, 110u, 6u  }, { 0x90u, 60u, 110u, 8u  }, { 0x90u, 62u, 110u, 10u },
        { 0x91u, 64u, 108u, 12u }, { 0x91u, 67u, 108u, 14u }, { 0x91u, 69u, 108u, 16u },
        { 0x91u, 71u, 108u, 18u }, { 0x91u, 72u, 108u, 20u }, { 0x91u, 74u, 108u, 22u },
        { 0x92u, 76u, 112u, 24u }, { 0x92u, 79u, 112u, 26u }, { 0x92u, 81u, 112u, 28u },
        { 0x92u, 84u, 112u, 30u }
    };
    adrift_headless_render(pcm, N, multi_evs, 16u);

    uint8_t active_cnt = 0u;
    for (uint8_t v = 0u; v < ADRIFT_MAX_VOICES; v++) {
        if (s->voices[v].active && s->voices[v].key_down) active_cnt++;
    }
    CHECK(active_cnt == 16u, "Expected all 16 voices active across G1+G2+G3, got %u", active_cnt);

    double chord_rms = compute_rms_dbfs(pcm, 2048u, 16384u);
    int16_t max_abs = 0;
    for (uint32_t i = 0; i < N * 2u; i++) {
        int32_t a = pcm[i] < 0 ? -(int32_t)pcm[i] : (int32_t)pcm[i];
        if (a > max_abs) max_abs = (int16_t)a;
    }
    CHECK(chord_rms > -24.0 && chord_rms < -1.0,
          "Multi-group 16-voice chord RMS out of expected range: %.2f dBFS", chord_rms);
    CHECK(max_abs > 4000 && max_abs <= 32766,
          "16-voice soft-knee limiter out of bounds (max_abs=%d)", max_abs);

    if (wav_dir) {
        char path[512];
        snprintf(path, sizeof(path), "%s/preset_poly16_chord.wav", wav_dir);
        CHECK(adrift_headless_write_wav(path, pcm, N, 2u, ADRIFT_SAMPLE_RATE),
              "Failed to write 16-voice multi-group chord WAV: %s", path);
    }

    /* Reassign G3 to MIDI Ch 10 via SAVE menu Page 2/2 Encoder 1 (5:MIDI) */
    adrift_synth_all_notes_off(s, true);
    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed = ADRIFT_BTN_GROUP_CFG; /* Open SAVE menu Page 1/2 on G3 */
    in_st.master_pot      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);

    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed = ADRIFT_BTN_GROUP_CFG; /* Advance SAVE menu to Page 2/2 */
    in_st.enc_delta[0]    = 7;                    /* Page 2/2 Encoder 1 (MIDI): Ch 3 + 7 -> Ch 10 */
    in_st.master_pot      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(s->groups[2].midi_channel == 10u, "Failed to set G3 MIDI channel to 10 (got %u)", s->groups[2].midi_channel);

    adrift_synth_handle_midi(s, 0x92u, 72u, 100u); /* Ch 3 -> ignored now */
    for (uint8_t v = 0u; v < ADRIFT_MAX_VOICES; v++) {
        if (s->voices[v].owner_group == 2) {
            CHECK(!s->voices[v].active, "G3 responded to old MIDI Ch 3 after reassignment");
        }
    }
    adrift_synth_handle_midi(s, 0x99u, 72u, 100u); /* Ch 10 -> triggers G3! */
    bool g3_hit = false;
    for (uint8_t v = 0u; v < ADRIFT_MAX_VOICES; v++) {
        if (s->voices[v].owner_group == 2 && s->voices[v].active && s->voices[v].note == 72u) {
            g3_hit = true;
        }
    }
    CHECK(g3_hit, "G3 did not respond to reassigned MIDI Ch 10");
    adrift_synth_all_notes_off(s, true);

    /* 8e. Unison Mode + Unison Detune on G1 (6 stacked voices) on SAVE Page 1/2 */
    s->edit_group = 0u; /* Select G1 while in SAVE menu */
    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed = ADRIFT_BTN_GROUP_CFG; /* Toggle SAVE Page 2/2 -> Page 1/2 */
    in_st.enc_delta[1]    = 1;                    /* Encoder 2 (ASSIGN) -> UNISON */
    in_st.enc_delta[3]    = 25;                   /* Encoder 4 (DETUNE) -> ~24 cents spread */
    in_st.master_pot      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(s->groups[0].assign_mode == ADRIFT_GROUP_UNISON && s->groups[0].unison_detune == 500,
          "Failed to enable Unison + Detune on G1");

    adrift_midi_msg_t uni_on = { 0x90u, 60u, 118u, 0u }; /* Single C4 note on Ch 1 */
    adrift_headless_render(pcm, 4096u, &uni_on, 1u);

    uint8_t g1_uni_cnt = 0u;
    int32_t dly_slot0 = 0, dly_slot5 = 0;
    for (uint8_t v = 0u; v < ADRIFT_MAX_VOICES; v++) {
        if (s->voices[v].owner_group == 0) {
            CHECK(s->voices[v].active && s->voices[v].key_down && s->voices[v].note == 60u,
                  "Unison voice %u did not stack on C4", v);
            g1_uni_cnt++;
            if (s->voices[v].group_slot == 0u) dly_slot0 = s->voices[v].bbd.delay_q16;
            if (s->voices[v].group_slot == 5u) dly_slot5 = s->voices[v].bbd.delay_q16;
        } else {
            CHECK(!s->voices[v].active, "Unison note leaked into another group's voice %u", v);
        }
    }
    CHECK(g1_uni_cnt == 6u && dly_slot0 != dly_slot5,
          "Unison 6-voice stack or symmetric detune failed (cnt=%u, d0=%d, d5=%d)",
          g1_uni_cnt, dly_slot0, dly_slot5);

    /* Unison last-note priority legato return: hold C4 (60), press G4 (67), release G4 -> returns to C4 (60) */
    adrift_synth_handle_midi(s, 0x90u, 67u, 110u);
    CHECK(s->voices[0].note == 67u && s->voices[1].note == 67u && s->voices[2].note == 67u,
          "Unison stack did not move to new top note 67");
    adrift_synth_handle_midi(s, 0x80u, 67u, 0u);
    CHECK(s->voices[0].key_down && s->voices[0].note == 60u,
          "Unison stack did not return to still-held note 60 (note=%u)", s->voices[0].note);

    /* Step Encoder 2 (ASSIGN) +1 -> SUPER mode (6 detuned VCOs summed into stereo BBD pair on slots 0 & 1) */
    memset(&in_st, 0, sizeof(in_st));
    in_st.enc_delta[1] = 1; /* UNISON -> SUPER */
    in_st.master_pot   = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(s->groups[0].assign_mode == ADRIFT_GROUP_SUPER,
          "Expected ASSIGN to step to ADRIFT_GROUP_SUPER (2), got %u", s->groups[0].assign_mode);
    adrift_headless_render(pcm, 4096u, &uni_on, 1u);
    CHECK(s->voices[0].exc.vco_inc != s->voices[5].exc.vco_inc &&
          compute_rms_dbfs(pcm, 0u, 4096u) > -26.0,
          "SUPER mode failed to detune companion VCOs or render through shared BBD");

    /* Verify non-linear 120c detune curve, Demon Core octave stacking (-12 ST slot 2, +12 ST slot 5),
     * stereo BBD pair in SUPER mode (slots 0 & 1), and per-group SPREAD (SAVE Page 2/2 + MIDI CC 10) */
    CHECK(adrift_detune_to_cents(0) == 0 &&
          adrift_detune_to_cents(512) >= 30 && adrift_detune_to_cents(512) <= 42 &&
          adrift_detune_to_cents(1023) == 120 && adrift_detune_to_q8(1023) == 307,
          "Non-linear 120c detune curve mismatch (512->%dc, 1023->%dc)",
          (int)adrift_detune_to_cents(512), (int)adrift_detune_to_cents(1023));

    double inc0 = (double)s->voices[0].exc.vco_inc;
    double inc2 = (double)s->voices[2].exc.vco_inc; /* Slot 2: -12 ST sub-octave (~0.5x inc0) */
    double inc5 = (double)s->voices[5].exc.vco_inc; /* Slot 5: +12 ST upper octave (~2.0x inc0) */
    CHECK(inc2 > inc0 * 0.46 && inc2 < inc0 * 0.54 &&
          inc5 > inc0 * 1.85 && inc5 < inc0 * 2.15,
          "Demon Core octave stacking failed (inc0=%.0f, inc2=%.0f, inc5=%.0f)", inc0, inc2, inc5);

    CHECK(s->voices[0].bbd.delay_q16 != s->voices[1].bbd.delay_q16 &&
          s->voices[0].bbd.peak_wet_q15 > 0 && s->voices[1].bbd.peak_wet_q15 > 0,
          "SUPER stereo BBD pair (slots 0 & 1) did not both run with stereo micro-detune");

    bool stereo_diff = false;
    for (uint32_t i = 512u; i < 4096u; i++) {
        if (pcm[2u * i] != pcm[2u * i + 1u]) {
            stereo_diff = true;
            break;
        }
    }
    CHECK(stereo_diff, "Expected stereo L != R separation when stereo_spread > 0");

    /* Adjust SAVE Page 1/2 Encoder 3 (3:SPREAD) to 0 -> verify L == R mono collapse */
    memset(&in_st, 0, sizeof(in_st));
    in_st.enc_delta[2] = -50; /* Encoder 3 on Page 1/2 (SPREAD) -> 0% */
    in_st.master_pot   = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->in_group_menu && ui->subpage == 0u && s->groups[0].stereo_spread == 0,
          "SAVE Page 1/2 SPREAD encoder failed (subpage=%u, spread=%d)",
          ui->subpage, s->groups[0].stereo_spread);

    adrift_headless_render(pcm, 1024u, NULL, 0u);
    bool mono_locked = true;
    for (uint32_t i = 256u; i < 1024u; i++) {
        if (pcm[2u * i] != pcm[2u * i + 1u]) {
            mono_locked = false;
            break;
        }
    }
    CHECK(mono_locked, "Expected L == R when stereo_spread == 0");

    adrift_synth_handle_midi(s, 0xB0u, 10u, 127u); /* MIDI CC 10 -> stereo_spread = 1023 */
    CHECK(s->groups[0].stereo_spread == 1023, "MIDI CC 10 failed to set stereo_spread to 1023");

    /* Verify SAVE Page 2/2 Encoder 2 (6:PAN, replacing PB RNG) controls per-group stereo panning
     * and works properly with voice stereo spread across SUPER, UNISON, and ROUND-ROBIN modes */
    s->groups[0].stereo_spread = 0;
    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed = ADRIFT_BTN_GROUP_CFG; /* Advance SAVE Page 1/2 -> Page 2/2 */
    in_st.enc_delta[1]    = -32;                  /* Page 2/2 Encoder 2 (6:PAN): -512 -> L100 (hard Left) */
    in_st.master_pot      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->in_group_menu && ui->subpage == 1u && s->groups[0].pan == -512,
          "SAVE Page 2/2 Encoder 2 (PAN) failed to set hard Left -512 (subpage=%u, pan=%d)",
          ui->subpage, s->groups[0].pan);

    adrift_headless_render(pcm, 1024u, &uni_on, 1u);
    double hard_l_rms_l = compute_rms_dbfs(pcm, 256u, 768u);
    double hard_l_rms_r = compute_rms_dbfs(pcm + 1, 256u, 768u);
    CHECK(hard_l_rms_l > -24.0 && hard_l_rms_r < -110.0,
          "Hard-Left group PAN (L100, spread=0) failed (L=%.1f dBFS, R=%.1f dBFS)",
          hard_l_rms_l, hard_l_rms_r);

    /* Pan hard Right (+64 clicks from -512 -> +512 = R100) via SAVE Page 2/2 Encoder 2 */
    memset(&in_st, 0, sizeof(in_st));
    in_st.enc_delta[1] = 64;
    in_st.master_pot   = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(s->groups[0].pan == 512, "Expected group PAN=+512 (R100), got %d", s->groups[0].pan);

    adrift_headless_render(pcm, 1024u, &uni_on, 1u);
    double hard_r_rms_l = compute_rms_dbfs(pcm, 256u, 768u);
    double hard_r_rms_r = compute_rms_dbfs(pcm + 1, 256u, 768u);
    CHECK(hard_r_rms_r > -24.0 && hard_r_rms_l < -110.0,
          "Hard-Right group PAN (R100, spread=0) failed (L=%.1f dBFS, R=%.1f dBFS)",
          hard_r_rms_l, hard_r_rms_r);

    /* Combine group PAN with voice stereo_spread (820) in SUPER, UNISON, and ROUND-ROBIN modes */
    s->groups[0].stereo_spread = 820;
    memset(&in_st, 0, sizeof(in_st));
    in_st.enc_delta[1] = -48; /* +512 - 48*16 = -256 (L50) */
    in_st.master_pot   = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(s->groups[0].pan == -256, "Expected group PAN=-256 (L50), got %d", s->groups[0].pan);

    adrift_headless_render(pcm, 1024u, &uni_on, 1u);
    double sup_l50_l = compute_rms_dbfs(pcm, 256u, 768u);
    double sup_l50_r = compute_rms_dbfs(pcm + 1, 256u, 768u);
    bool sup_l50_diff = false;
    for (uint32_t i = 256u; i < 1024u; i++) {
        if (pcm[2u * i] != pcm[2u * i + 1u]) {
            sup_l50_diff = true;
            break;
        }
    }
    CHECK(sup_l50_diff && sup_l50_l > sup_l50_r + 2.5 && sup_l50_r > -45.0,
          "SUPER mode L50 pan + stereo_spread failed (L=%.1f dBFS, R=%.1f dBFS, diff=%d)",
          sup_l50_l, sup_l50_r, sup_l50_diff);

    /* Test UNISON mode with stereo_spread=820 and R50 pan (+256) */
    s->groups[0].assign_mode = ADRIFT_GROUP_UNISON;
    memset(&in_st, 0, sizeof(in_st));
    in_st.enc_delta[1] = 32; /* -256 + 32*16 = +256 (R50) */
    in_st.master_pot   = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(s->groups[0].pan == 256, "Expected group PAN=+256 (R50), got %d", s->groups[0].pan);

    adrift_headless_render(pcm, 1024u, &uni_on, 1u);
    double uni_r50_l = compute_rms_dbfs(pcm, 256u, 768u);
    double uni_r50_r = compute_rms_dbfs(pcm + 1, 256u, 768u);
    bool uni_r50_diff = false;
    for (uint32_t i = 256u; i < 1024u; i++) {
        if (pcm[2u * i] != pcm[2u * i + 1u]) {
            uni_r50_diff = true;
            break;
        }
    }
    CHECK(uni_r50_diff && uni_r50_r > uni_r50_l + 2.5 && uni_r50_l > -45.0,
          "UNISON mode R50 pan + stereo_spread failed (L=%.1f dBFS, R=%.1f dBFS, diff=%d)",
          uni_r50_l, uni_r50_r, uni_r50_diff);

    /* Test ROUND-ROBIN mode with stereo_spread=820 and group PAN (-256 vs +256) */
    adrift_synth_group_all_notes_off(s, 0u, true);
    s->groups[0].assign_mode   = ADRIFT_GROUP_ROUND_ROBIN;
    s->groups[0].stereo_spread = 820;
    s->groups[0].pan           = -256;
    adrift_synth_group_note_on(s, 0u, 60u, 110u);
    adrift_synth_group_note_on(s, 0u, 64u, 110u);
    adrift_headless_render(pcm, 1024u, NULL, 0u);
    double rr_l50_l = compute_rms_dbfs(pcm, 256u, 768u);
    double rr_l50_r = compute_rms_dbfs(pcm + 1, 256u, 768u);
    bool rr_l50_diff = false;
    for (uint32_t i = 256u; i < 1024u; i++) {
        if (pcm[2u * i] != pcm[2u * i + 1u]) {
            rr_l50_diff = true;
            break;
        }
    }
    CHECK(rr_l50_diff && rr_l50_l > rr_l50_r + 2.0 && rr_l50_r > -45.0,
          "ROUND-ROBIN L50 pan + stereo_spread failed (L=%.1f dBFS, R=%.1f dBFS, diff=%d)",
          rr_l50_l, rr_l50_r, rr_l50_diff);

    /* Return G1 to SUPER mode and center PAN (0) via SAVE Page 2/2 Encoder 2 */
    s->groups[0].assign_mode = ADRIFT_GROUP_SUPER;
    memset(&in_st, 0, sizeof(in_st));
    in_st.enc_delta[1] = 16; /* -256 + 16*16 = 0 (Center) */
    in_st.master_pot   = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(s->groups[0].pan == 0, "Expected group PAN=0 (C), got %d", s->groups[0].pan);

    /* 8f. Presets Browser UI (PRESETS encoder open/scroll/wrap, OCT- cancel, OCT+ load P1..P24 / INIT GROUP / INIT SYNTH) */
    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed = ADRIFT_BTN_PAGE_1; /* Switch from SAVE to EXC Page 1/2 */
    in_st.master_pot      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(!ui->in_group_menu && !ui->in_preset_menu && ui->page == 0u,
          "Expected EXC Page 1/2 before opening Presets Browser");

    /* Customize patch, group, and FX settings, then verify first PRESETS turn opens browser without overriding edits */
    s->groups[0].patch.exc.wave_morph = 123;
    s->groups[0].stereo_spread        = 777;
    s->groups[0].pan                  = -192;
    s->fx_params.chorus_mix           = 444;
    s->kbd_octave = 0;
    memset(&in_st, 0, sizeof(in_st));
    in_st.enc_delta[ADRIFT_ENC_PRESETS] = 1; /* First turn opens Presets Browser at current preset (P1 -> cursor 2) */
    in_st.master_pot                    = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->in_preset_menu && ui->preset_cursor == 2u &&
          s->preset_index == 0u && s->groups[0].patch.exc.wave_morph == 123,
          "First PRESETS turn should open browser at cursor 2 (P1) without overriding patch (in_menu=%d, cur=%u, morph=%d)",
          ui->in_preset_menu, ui->preset_cursor, s->groups[0].patch.exc.wave_morph);

    /* Scroll up to INIT SYNTH (cursor 0), wrap around to P24 (cursor 25), wrap back to 0, then step to P2 (cursor 3) */
    memset(&in_st, 0, sizeof(in_st));
    in_st.enc_delta[ADRIFT_ENC_PRESETS] = -2;
    in_st.master_pot                    = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->preset_cursor == 0u && s->preset_index == ADRIFT_PRESET_INIT &&
          s->groups[0].patch.exc.wave_morph == 682 && s->fx_params.chorus_mix == 0,
          "Expected live preview of INIT SYNTH at cursor 0 (got cur=%u, pst=0x%02x, morph=%d)",
          ui->preset_cursor, s->preset_index, s->groups[0].patch.exc.wave_morph);

    in_st.enc_delta[ADRIFT_ENC_PRESETS] = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->preset_cursor == 49u && s->preset_index == 0u &&
          s->groups[0].patch.exc.wave_morph == 123,
          "Expected ALL-bank wrap to 49 (U24 <EMPTY> restoring snapshot), got cur=%u pst=%u morph=%d",
          ui->preset_cursor, s->preset_index, s->groups[0].patch.exc.wave_morph);

    in_st.enc_delta[ADRIFT_ENC_PRESETS] = 1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->preset_cursor == 0u && s->preset_index == ADRIFT_PRESET_INIT,
          "Expected preset_cursor wrap to 0 (INIT SYNTH), got %u", ui->preset_cursor);

    /* Switch KNOB1 (1:BK) to FACT bank (ALL->USER->FACT) and verify FACT-bank wrap 0 -> 25 (P24) -> 0 */
    memset(&in_st, 0, sizeof(in_st));
    in_st.enc_delta[ADRIFT_ENC_KNOB1] = 2;
    in_st.master_pot                  = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->preset_bank == ADRIFT_PRESET_BANK_FACT && ui->preset_cursor == 0u,
          "Expected FACT bank at cursor 0, got bank=%u cur=%u", ui->preset_bank, ui->preset_cursor);

    memset(&in_st, 0, sizeof(in_st));
    in_st.enc_delta[ADRIFT_ENC_PRESETS] = -1;
    in_st.master_pot                    = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->preset_cursor == 25u && s->preset_index == 23u,
          "Expected FACT-bank preset_cursor wrap to 25 (P24 live preview), got cur=%u pst=%u",
          ui->preset_cursor, s->preset_index);

    in_st.enc_delta[ADRIFT_ENC_PRESETS] = 1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->preset_cursor == 0u && s->preset_index == ADRIFT_PRESET_INIT,
          "Expected FACT-bank preset_cursor wrap to 0 (INIT SYNTH), got %u", ui->preset_cursor);

    /* Return KNOB1 to ALL bank */
    memset(&in_st, 0, sizeof(in_st));
    in_st.enc_delta[ADRIFT_ENC_KNOB1] = 1;
    in_st.master_pot                  = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->preset_bank == ADRIFT_PRESET_BANK_ALL, "Expected return to ALL bank");

    memset(&in_st, 0, sizeof(in_st));
    in_st.master_pot = -1;

    in_st.enc_delta[ADRIFT_ENC_PRESETS] = 3;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->preset_cursor == 3u && s->preset_index == 1u &&
          s->groups[0].num_voices == 8u && s->groups[1].num_voices == 6u,
          "Scrolling to P2 (cursor 3) must immediately preview P2 (G1=%u, G2=%u)",
          s->groups[0].num_voices, s->groups[1].num_voices);

    /* Play a note while P2 is highlighted to verify live audio preview in the Presets Browser */
    adrift_synth_handle_midi(s, 0x90u, 60u, 110u);
    adrift_synth_render_block(s, pcm, 512u, NULL, 0u);
    CHECK(s->any_gate && compute_rms_dbfs(pcm, 0u, 512u) > -45.0,
          "Playing notes while P2 is highlighted in Presets Browser should preview P2");

    /* Pressing OCT- cancels Presets Browser and restores ALL user settings from before opening the browser */
    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed = ADRIFT_BTN_OCT_DOWN;
    in_st.master_pot      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(!ui->in_preset_menu && ui->page == 0u && s->preset_index == 0u &&
          s->groups[0].num_voices == 6u && s->groups[1].num_voices == 6u &&
          s->groups[0].assign_mode == ADRIFT_GROUP_SUPER && s->groups[0].unison_detune == 500 &&
          s->groups[0].patch.exc.wave_morph == 123 && s->groups[0].stereo_spread == 777 &&
          s->groups[0].pan == -192 &&
          s->fx_params.chorus_mix == 444 && s->kbd_octave == 0,
          "OCT- cancel failed to restore user snapshot (in_menu=%d, pst=%u, G1=%u, G2=%u, morph=%d, spread=%d, pan=%d, chr=%d, oct=%d)",
          ui->in_preset_menu, s->preset_index, s->groups[0].num_voices, s->groups[1].num_voices,
          s->groups[0].patch.exc.wave_morph, s->groups[0].stereo_spread, s->groups[0].pan,
          s->fx_params.chorus_mix, s->kbd_octave);

    /* Open Presets Browser again, scroll +1 to P2 (cursor 3), and press OCT+ to confirm loading P2 */
    memset(&in_st, 0, sizeof(in_st));
    in_st.enc_delta[ADRIFT_ENC_PRESETS] = 1;
    in_st.master_pot                    = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->in_preset_menu && ui->preset_cursor == 2u, "Expected browser to reopen at P1 (cursor 2)");

    in_st.enc_delta[ADRIFT_ENC_PRESETS] = 1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->preset_cursor == 3u, "Expected cursor at P2 (3)");

    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed = ADRIFT_BTN_OCT_UP;
    in_st.master_pot      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(!ui->in_preset_menu && s->preset_index == 1u && s->kbd_octave == 0 &&
          s->groups[0].num_voices == 8u && s->groups[1].num_voices == 6u,
          "OCT+ on P2 did not load multi-group preset allocation (G1=%u, G2=%u)",
          s->groups[0].num_voices, s->groups[1].num_voices);

    /* Test INIT GROUP (cursor 1) on G2: preserves G2's 6 voices & Ch 1, resets G2 patch & transpose, keeps G1 intact */
    s->edit_group = 1u;
    CHECK(s->groups[1].transpose == 12, "Expected P2 G2 transpose=+12 before INIT GROUP");
    memset(&in_st, 0, sizeof(in_st));
    in_st.enc_delta[ADRIFT_ENC_PRESETS] = 1; /* Opens at P2 (cursor 3) */
    in_st.master_pot                    = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    in_st.enc_delta[ADRIFT_ENC_PRESETS] = -2; /* Scroll 3 -> 1 (INIT GROUP) */
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->preset_cursor == 1u, "Expected cursor at INIT GROUP (1)");

    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed = ADRIFT_BTN_OCT_UP;
    in_st.master_pot      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(!ui->in_preset_menu && s->preset_index == 1u &&
          s->groups[1].num_voices == 6u && s->groups[1].midi_channel == 1u &&
          s->groups[1].transpose == 0 && s->groups[1].patch.exc.wave_morph == 682 &&
          s->groups[1].patch.dry_wet == 0 && s->groups[0].num_voices == 8u,
          "INIT GROUP on G2 failed (G2_v=%u, tr=%d, morph=%d, dry_wet=%d)",
          s->groups[1].num_voices, s->groups[1].transpose,
          s->groups[1].patch.exc.wave_morph, s->groups[1].patch.dry_wet);

    /* Test INIT SYNTH (cursor 0): resets whole synth to G1=3v, G2..G6=0v, preset_index=ADRIFT_PRESET_INIT */
    memset(&in_st, 0, sizeof(in_st));
    in_st.enc_delta[ADRIFT_ENC_PRESETS] = 1; /* Opens at P2 (cursor 3) */
    in_st.master_pot                    = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    in_st.enc_delta[ADRIFT_ENC_PRESETS] = -3; /* Scroll 3 -> 0 (INIT SYNTH) */
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->preset_cursor == 0u, "Expected cursor at INIT SYNTH (0)");

    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed = ADRIFT_BTN_OCT_UP;
    in_st.master_pot      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(!ui->in_preset_menu && s->preset_index == ADRIFT_PRESET_INIT &&
          s->edit_group == 0u && s->groups[0].num_voices == 3u && s->groups[1].num_voices == 0u &&
          s->groups[0].patch.exc.wave_morph == 682 && s->groups[0].patch.dry_wet == 0 &&
          s->fx_params.chorus_mix == 0 && s->fx_params.delay_mix == 0 && s->fx_params.reverb_mix == 0,
          "INIT SYNTH failed (pst=0x%02x, G1=%u, G2=%u, morph=%d)",
          s->preset_index, s->groups[0].num_voices, s->groups[1].num_voices,
          s->groups[0].patch.exc.wave_morph);

    /* Reopen from INIT SYNTH (starts at cursor 0), scroll +2 to P1 (cursor 2), and confirm with OCT+ */
    memset(&in_st, 0, sizeof(in_st));
    in_st.enc_delta[ADRIFT_ENC_PRESETS] = 1;
    in_st.master_pot                    = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->in_preset_menu && ui->preset_cursor == 0u,
          "Opening Presets Browser from INIT SYNTH should start at cursor 0 (got %u)", ui->preset_cursor);
    in_st.enc_delta[ADRIFT_ENC_PRESETS] = 2;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed = ADRIFT_BTN_OCT_UP;
    in_st.master_pot      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(!ui->in_preset_menu && s->preset_index == 0u &&
          s->groups[0].num_voices == 3u && s->groups[1].num_voices == 0u,
          "OCT+ on P1 (cursor 2) did not restore G1=3, G2=0 default");

    /* 8g. Scale Browser UI (ALGORITHM encoder open/scroll/wrap, KNOB1 root, OCT- cancel, OCT+ apply, white-key mapping) */
    CHECK(!ui->in_scale_menu && s->kbd_scale == ADRIFT_SCALE_CHROMATIC && s->kbd_scale_root == 0u,
          "Expected default Chromatic scale (0) and C root (0) at boot (got scale=%u, root=%u)",
          s->kbd_scale, s->kbd_scale_root);
    for (uint32_t k = 0u; k < 27u; k++) {
        int32_t mapped = adrift_synth_map_kbd_key(s, (int32_t)k - 7);
        CHECK(mapped == 53 + (int32_t)k,
              "Chromatic mapping mismatch on FM-1 key %u: got %d, want %d", k, mapped, 53 + (int32_t)k);
    }

    /* First ALGORITHM turn opens Scale Browser on active item (cursor 0 = CHROMATIC) without changing scale */
    int16_t saved_pw = s->groups[0].patch.exc.pulse_width;
    memset(&in_st, 0, sizeof(in_st));
    in_st.enc_delta[ADRIFT_ENC_ALGORITHM] = 1;
    in_st.master_pot                      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->in_scale_menu && ui->prev_layout_mode == 4u &&
          ui->scale_cursor == ADRIFT_SCALE_CHROMATIC && s->kbd_scale == ADRIFT_SCALE_CHROMATIC,
          "First ALGORITHM turn should open Scale Browser at cursor 0 without changing scale (in=%d, mode=%u, cur=%u, sc=%u)",
          ui->in_scale_menu, ui->prev_layout_mode, ui->scale_cursor, s->kbd_scale);
    /* Verify row 0 is highlighted in active group accent color (ry = 31 + 22 = 53, x = 200) */
    CHECK(fb[56u * ADRIFT_SCREEN_W + 160u] == adrift_ui_group_accent_rgb565(s->edit_group),
          "Expected selected Scale Browser row 0 to be filled with group accent color");

    /* Scroll CCW -1 -> wraps 0 to 15 (WHOLE TONE), then +1 -> wraps 15 to 0, then +1 -> 1 (MAJOR) */
    in_st.enc_delta[ADRIFT_ENC_ALGORITHM] = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->scale_cursor == ADRIFT_SCALE_WHOLE_TONE && s->kbd_scale == ADRIFT_SCALE_WHOLE_TONE &&
          strcmp(adrift_synth_get_scale_name(s->kbd_scale), "WHOLE TONE") == 0,
          "Expected Scale Browser wrap to WHOLE TONE (15), got cur=%u sc=%u", ui->scale_cursor, s->kbd_scale);

    in_st.enc_delta[ADRIFT_ENC_ALGORITHM] = 2; /* 15 -> 0 -> 1 (MAJOR) */
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->scale_cursor == ADRIFT_SCALE_MAJOR && s->kbd_scale == ADRIFT_SCALE_MAJOR && s->kbd_scale_root == 0u,
          "Expected live preview of C MAJOR (cur=%u, sc=%u, root=%u)",
          ui->scale_cursor, s->kbd_scale, s->kbd_scale_root);

    /* Verify C Major maps all 16 FM-1 white keys to natural piano notes F3..G5 and mutes all 11 black keys */
    static const uint8_t k_fm1_whites[16] = { 0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26 };
    static const uint8_t k_fm1_blacks[11] = { 1, 3, 5, 8, 10, 13, 15, 17, 20, 22, 25 };
    for (uint32_t i = 0u; i < 16u; i++) {
        uint8_t k = k_fm1_whites[i];
        int32_t mapped = adrift_synth_map_kbd_key(s, (int32_t)k - 7);
        CHECK(mapped == 53 + (int32_t)k,
              "C Major white key %u mismatch: got %d, want %d", k, mapped, 53 + (int32_t)k);
    }
    for (uint32_t i = 0u; i < 11u; i++) {
        uint8_t k = k_fm1_blacks[i];
        int32_t mapped = adrift_synth_map_kbd_key(s, (int32_t)k - 7);
        CHECK(mapped == -1, "Expected black key %u to be muted (-1) in C Major, got %d", k, mapped);
    }

    /* Turn KNOB1 -3 inside Scale Browser -> wraps root C (0) to A (9), while KNOB2 is ignored; scroll +2 to MIN PENTA (3) */
    memset(&in_st, 0, sizeof(in_st));
    in_st.enc_delta[ADRIFT_ENC_KNOB1]     = -3; /* 0 - 3 -> 9 ("A") */
    in_st.enc_delta[ADRIFT_ENC_KNOB2]     = 5;  /* Must not edit EXC PWM while in Scale Browser */
    in_st.enc_delta[ADRIFT_ENC_ALGORITHM] = 2;  /* MAJOR (1) -> MIN PENTA (3) */
    in_st.master_pot                      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(s->kbd_scale == ADRIFT_SCALE_MIN_PENTA && s->kbd_scale_root == 9u &&
          strcmp(adrift_synth_get_root_name(s->kbd_scale_root), "A") == 0 &&
          s->groups[0].patch.exc.pulse_width == saved_pw,
          "Expected A MIN PENTA live preview without KNOB2 editing patch (sc=%u, root=%u, pw=%d vs %d)",
          s->kbd_scale, s->kbd_scale_root, s->groups[0].patch.exc.pulse_width, saved_pw);

    /* Verify sequential 5-note A Minor Pentatonic degrees across FM-1 white keys (white_step = -4..+11) */
    static const int32_t k_a_min_penta_fm1[16] = {
        60, 62, 64, 67, /* F3..B3 white keys -> C4, D4, E4, G4 */
        69, 72, 74, 76, 79, /* C4..G4 white keys -> A4, C5, D5, E5, G5 */
        81, 84, 86, 88, 91, /* A4..E5 white keys -> A5, C6, D6, E6, G6 */
        93, 96          /* F5, G5 white keys -> A6, C7 */
    };
    for (uint32_t i = 0u; i < 16u; i++) {
        int32_t mapped = adrift_synth_map_kbd_key(s, (int32_t)k_fm1_whites[i] - 7);
        CHECK(mapped == k_a_min_penta_fm1[i],
              "A Min Penta white key #%u (k=%u) mismatch: got %d, want %d",
              i, k_fm1_whites[i], mapped, k_a_min_penta_fm1[i]);
    }
    for (uint32_t i = 0u; i < 11u; i++) {
        CHECK(adrift_synth_map_kbd_key(s, (int32_t)k_fm1_blacks[i] - 7) == -1,
              "Expected black key %u muted in A Min Penta", k_fm1_blacks[i]);
    }

    /* Pressing OCT- cancels Scale Browser and restores Chromatic / C without changing kbd_octave */
    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed = ADRIFT_BTN_OCT_DOWN;
    in_st.master_pot      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(!ui->in_scale_menu && s->kbd_scale == ADRIFT_SCALE_CHROMATIC &&
          s->kbd_scale_root == 0u && s->kbd_octave == 0,
          "OCT- cancel failed to restore Chromatic/C (in=%d, sc=%u, root=%u, oct=%d)",
          ui->in_scale_menu, s->kbd_scale, s->kbd_scale_root, s->kbd_octave);

    /* Open Scale Browser again, select D BLUES (scale=5, root=2), and press OCT+ to confirm */
    memset(&in_st, 0, sizeof(in_st));
    in_st.enc_delta[ADRIFT_ENC_ALGORITHM] = 1; /* Opens at 0 */
    in_st.master_pot                      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    in_st.enc_delta[ADRIFT_ENC_ALGORITHM] = 5; /* Scroll 0 -> 5 (BLUES) */
    in_st.enc_delta[ADRIFT_ENC_KNOB1]     = 2; /* Root C (0) -> D (2) */
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed = ADRIFT_BTN_OCT_UP;
    in_st.master_pot      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(!ui->in_scale_menu && s->kbd_scale == ADRIFT_SCALE_BLUES &&
          s->kbd_scale_root == 2u && s->kbd_octave == 0,
          "OCT+ did not confirm D BLUES without changing octave (in=%d, sc=%u, root=%u, oct=%d)",
          ui->in_scale_menu, s->kbd_scale, s->kbd_scale_root, s->kbd_octave);

    /* Reopen Scale Browser: must start on active scale BLUES (cursor 5); preview HIRAJOSHI/G then cancel via PAGE_1 */
    memset(&in_st, 0, sizeof(in_st));
    in_st.enc_delta[ADRIFT_ENC_ALGORITHM] = 1;
    in_st.master_pot                      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->in_scale_menu && ui->scale_cursor == ADRIFT_SCALE_BLUES,
          "Expected Scale Browser to open on active scale BLUES (5), got %u", ui->scale_cursor);
    in_st.enc_delta[ADRIFT_ENC_ALGORITHM] = 9; /* 5 -> 14 (HIRAJOSHI) */
    in_st.enc_delta[ADRIFT_ENC_KNOB1]     = 5; /* 2 -> 7 (G) */
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(s->kbd_scale == ADRIFT_SCALE_HIRAJOSHI && s->kbd_scale_root == 7u, "Expected G HIRAJOSHI preview");

    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed = ADRIFT_BTN_PAGE_1;
    in_st.master_pot      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(!ui->in_scale_menu && ui->page == 0u &&
          s->kbd_scale == ADRIFT_SCALE_BLUES && s->kbd_scale_root == 2u,
          "Switching screen with PAGE_1 should cancel Scale Browser and restore D BLUES (sc=%u, root=%u)",
          s->kbd_scale, s->kbd_scale_root);

    /* Mutual exclusion: opening Scale Browser, scrolling, then turning PRESETS cancels Scale Browser & opens Presets */
    memset(&in_st, 0, sizeof(in_st));
    in_st.enc_delta[ADRIFT_ENC_ALGORITHM] = 1;
    in_st.master_pot                      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    in_st.enc_delta[ADRIFT_ENC_ALGORITHM] = 3; /* 5 -> 8 (LYDIAN) */
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    memset(&in_st, 0, sizeof(in_st));
    in_st.enc_delta[ADRIFT_ENC_PRESETS] = 1;
    in_st.master_pot                    = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->in_preset_menu && !ui->in_scale_menu &&
          s->kbd_scale == ADRIFT_SCALE_BLUES && s->kbd_scale_root == 2u,
          "Opening Presets Browser from Scale Browser must cancel previewed scale (sc=%u, root=%u)",
          s->kbd_scale, s->kbd_scale_root);
    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed = ADRIFT_BTN_OCT_DOWN;
    in_st.master_pot      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(!ui->in_preset_menu && s->kbd_scale == ADRIFT_SCALE_BLUES && s->kbd_scale_root == 2u,
          "Confirmed keyboard scale must persist across Presets Browser cancel");

    /* Reset keyboard scale back to default Chromatic / C for subsequent tests */
    s->kbd_scale      = ADRIFT_SCALE_CHROMATIC;
    s->kbd_scale_root = 0u;

    uint16_t seen_cols[ADRIFT_MAX_GROUPS];
    for (uint8_t step = 0u; step < ADRIFT_MAX_GROUPS; step++) {
        uint8_t g = s->edit_group;
        /* Active [1/2] page pill in top visualizer pane (x=39..63, y=33..44) */
        uint16_t tab_px  = fb[34u * ADRIFT_SCREEN_W + 40u];
        uint16_t want_ac = adrift_ui_group_accent_rgb565(g);
        CHECK(tab_px == want_ac,
              "Active page pill color mismatch on G%u (got 0x%04x, want 0x%04x)", g + 1u, tab_px, want_ac);
        seen_cols[g] = tab_px;

        memset(&in_st, 0, sizeof(in_st));
        in_st.enc_delta[ADRIFT_ENC_SELECT] = 1;
        in_st.master_pot                   = -1;
        adrift_dirty_clear(&dirty);
        adrift_app_tick_ui(&in_st, fb, &dirty);
    }
    CHECK(s->edit_group == 0u, "SELECT encoder did not wrap back to G1 (0)");
    for (uint8_t a = 0u; a < ADRIFT_MAX_GROUPS; a++) {
        for (uint8_t b = a + 1u; b < ADRIFT_MAX_GROUPS; b++) {
            CHECK(seen_cols[a] != seen_cols[b],
                  "Groups G%u and G%u share identical highlight color", a + 1u, b + 1u);
        }
    }

    /* Verify parameter screens stay visible when notes play, and HOME button opens the oscilloscope */
    CHECK(!ui->in_scope && ui->prev_layout_mode == 0u, "Expected Parameter Screen layout (mode 0) on EXC");
    adrift_synth_handle_midi(s, 0x90u, 60u, 110u);
    adrift_synth_render_block(s, pcm, 256u, NULL, 0u);
    memset(&in_st, 0, sizeof(in_st));
    in_st.master_pot = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(!ui->in_scope && ui->prev_layout_mode == 0u,
          "Playing a note on EXC should not auto-switch away from the parameter screen");

    /* Pressing HOME opens the dedicated full-screen oscilloscope (Page 1/6: SCOPE) */
    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed = ADRIFT_BTN_HOME;
    in_st.master_pot      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->in_scope && ui->scope_page == ADRIFT_HOME_SCOPE && ui->prev_layout_mode == 2u,
          "HOME button did not open Full-Screen Oscilloscope (mode 2, page 0)");

    /* Subsequent scope frame while playing should only mark the tight wave bounding band (< 120 rows), not all 240 */
    memset(&in_st, 0, sizeof(in_st));
    in_st.master_pot = -1;
    adrift_synth_render_block(s, pcm, 256u, NULL, 0u);
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    uint32_t scope_dirty_rows = 0u;
    for (uint32_t y = 0u; y < ADRIFT_SCREEN_H; y++) {
        if (dirty.row_mask[y >> 5] & (1u << (y & 31u))) scope_dirty_rows++;
    }
    CHECK(scope_dirty_rows > 0u && scope_dirty_rows < 120u,
          "Expected incremental scope dirty band (<120 rows), got %u rows", scope_dirty_rows);

    /* Test HOME Knobs 1..4 (ZOOM, GAIN, FREEZE, MODE) and cycling all 6 HOME pages (1/6..6/6 -> 1/6) */
    memset(&in_st, 0, sizeof(in_st));
    in_st.enc_delta[ADRIFT_ENC_KNOB1] = 2; /* ZOOM: 1X -> 2X -> 4X */
    in_st.enc_delta[ADRIFT_ENC_KNOB2] = 1; /* GAIN: 100% -> 125% */
    in_st.enc_delta[ADRIFT_ENC_KNOB3] = 1; /* FREEZE: RUN -> HOLD */
    in_st.enc_delta[ADRIFT_ENC_KNOB4] = 1; /* MODE: MONO -> DUAL */
    in_st.master_pot = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->scope_zoom == 2u && ui->scope_gain == 125u && ui->scope_freeze &&
          s->scope_frozen && ui->scope_mode[ADRIFT_HOME_SCOPE] == 1u,
          "HOME knobs did not update ZOOM/GAIN/FREEZE/MODE as expected");

    /* Unfreeze and cycle through VECT (2/6), SPEC (3/6), STAGE (4/6), POLY (5/6), PERF (6/6), and back to SCOPE (1/6) */
    memset(&in_st, 0, sizeof(in_st));
    in_st.enc_delta[ADRIFT_ENC_KNOB3] = -1; /* FREEZE: HOLD -> RUN */
    in_st.master_pot = -1;
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(!ui->scope_freeze && !s->scope_frozen, "KNOB3 CCW did not unfreeze scope");

    for (uint8_t p = 1u; p <= ADRIFT_UI_NUM_HOME_PAGES; p++) {
        memset(&in_st, 0, sizeof(in_st));
        in_st.buttons_pressed = ADRIFT_BTN_HOME;
        in_st.master_pot      = -1;
        adrift_synth_render_block(s, pcm, 256u, NULL, 0u);
        adrift_dirty_clear(&dirty);
        adrift_app_tick_ui(&in_st, fb, &dirty);
        uint8_t expected_page = (uint8_t)(p % ADRIFT_UI_NUM_HOME_PAGES);
        CHECK(ui->in_scope && ui->scope_page == expected_page,
              "Pressing HOME did not advance to expected page %u (got %u)", expected_page, ui->scope_page);

        /* Cycle all 3 modes on each visualization page */
        for (uint8_t m = 0u; m < ADRIFT_UI_NUM_HOME_MODES; m++) {
            memset(&in_st, 0, sizeof(in_st));
            in_st.enc_delta[ADRIFT_ENC_KNOB4] = 1;
            in_st.master_pot = -1;
            adrift_dirty_clear(&dirty);
            adrift_app_tick_ui(&in_st, fb, &dirty);
        }
        CHECK(ui->scope_mode[expected_page] == ((expected_page == 0u) ? 1u : 0u),
              "KNOB4 mode wrap failed on HOME page %u", expected_page);
    }
    /* Restore default SCOPE zoom/gain/mode */
    ui->scope_zoom = 0u;
    ui->scope_gain = 100u;
    ui->scope_mode[ADRIFT_HOME_SCOPE] = 0u;
    adrift_synth_all_notes_off(s, true);

    /* Verify 1:EXC MORPH cell (Card 1 at y=139..235) draws the continuous waveform shape above the horizontal bar */
    memset(s->scope_ring, 0, sizeof(s->scope_ring));
    uint16_t acc_col = adrift_ui_group_accent_rgb565(s->edit_group);
    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed = ADRIFT_BTN_PAGE_1;
    in_st.master_pot      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(!ui->in_scope && ui->page == 0u && ui->subpage == 0u, "SEL did not return from HOME scope to EXC Page 1/2");
    uint32_t morph_wave_hits = 0u;
    for (uint32_t y = 170u; y <= 210u; y++) {
        for (uint32_t x = 10u; x <= 55u; x++) {
            uint16_t px = fb[y * ADRIFT_SCREEN_W + x];
            if (px == 0xFFFFu || px == acc_col) morph_wave_hits++;
        }
    }
    CHECK(morph_wave_hits >= 46u, "Expected 1:EXC MORPH cell to render waveform trace (hits=%u)", morph_wave_hits);

    /* Verify 2:ENV1 and 4:ENV2 envelope curve visualizers (gy_top = 47) */
    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed = ADRIFT_BTN_PAGE_2;
    in_st.master_pot      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    uint32_t env1_peak_hits = 0u;
    for (uint32_t x = 14u; x <= 225u; x++) {
        if (fb[47u * ADRIFT_SCREEN_W + x] == acc_col || fb[48u * ADRIFT_SCREEN_W + x] == acc_col) {
            env1_peak_hits++;
        }
    }
    CHECK(env1_peak_hits > 0u, "Expected 2:ENV1 page to render envelope peak curve");

    in_st.buttons_pressed = ADRIFT_BTN_PAGE_4;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    in_st.buttons_pressed = ADRIFT_BTN_HOLD;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    uint32_t drone_rail_hits = 0u;
    for (uint32_t x = 14u; x <= 225u; x++) {
        if (fb[47u * ADRIFT_SCREEN_W + x] == acc_col) drone_rail_hits++;
    }
    CHECK(drone_rail_hits > 200u, "Expected 4:ENV2 Drone Hold to draw full-width top rail");

    /* 8f. Verify incremental rendering pixel-equivalence & dirty-row bounds across all 6 HOME pages x 3 modes */
    {
        uint16_t *fb_prev = (uint16_t *)calloc(ADRIFT_SCREEN_W * ADRIFT_SCREEN_H, sizeof(uint16_t));
        uint16_t *fb_ref  = (uint16_t *)calloc(ADRIFT_SCREEN_W * ADRIFT_SCREEN_H, sizeof(uint16_t));
        CHECK(fb_prev != NULL && fb_ref != NULL, "Out of memory allocating UI equivalence buffers");

        /* Load P20 (16-voice Unison + Master Stereo FX) and trigger active unison chord */
        adrift_synth_load_preset(s, 19u);
        adrift_midi_msg_t p20_evs[2] = {
            { 0x90u, 48u, 118u, 0u },
            { 0x90u, 55u, 112u, 16u }
        };
        adrift_headless_render(pcm, 1024u, p20_evs, 2u);

        memset(&in_st, 0, sizeof(in_st));
        in_st.buttons_pressed = ADRIFT_BTN_HOME;
        in_st.master_pot      = -1;
        in_st.cpu_load_pct    = 62u;
        adrift_dirty_clear(&dirty);
        adrift_app_tick_ui(&in_st, fb, &dirty);
        CHECK(ui->in_scope && ui->scope_page == ADRIFT_HOME_SCOPE, "Expected HOME page 0");

        for (uint8_t page = 0u; page < ADRIFT_UI_NUM_HOME_PAGES; page++) {
            if (page > 0u) {
                memset(&in_st, 0, sizeof(in_st));
                in_st.buttons_pressed = ADRIFT_BTN_HOME;
                in_st.master_pot      = -1;
                in_st.cpu_load_pct    = 62u;
                adrift_dirty_clear(&dirty);
                adrift_app_tick_ui(&in_st, fb, &dirty);
                CHECK(ui->scope_page == page, "Expected HOME page %u, got %u", page, ui->scope_page);
            }

            for (uint8_t mode_idx = 0u; mode_idx < ADRIFT_UI_NUM_HOME_MODES; mode_idx++) {
                if (mode_idx > 0u) {
                    memset(&in_st, 0, sizeof(in_st));
                    in_st.enc_delta[ADRIFT_ENC_KNOB4] = 1;
                    in_st.master_pot                  = -1;
                    in_st.cpu_load_pct                = 62u;
                    adrift_dirty_clear(&dirty);
                    adrift_app_tick_ui(&in_st, fb, &dirty);
                }

                uint32_t min_pane_dirty = 212u;
                for (uint32_t f = 0u; f < 5u; f++) {
                    adrift_synth_render_block(s, pcm, 128u, NULL, 0u);
                    memcpy(fb_prev, fb, ADRIFT_SCREEN_W * ADRIFT_SCREEN_H * sizeof(uint16_t));
                    adrift_ui_t ui_snap = *ui;

                    memset(&in_st, 0, sizeof(in_st));
                    in_st.master_pot   = -1;
                    in_st.cpu_load_pct = (uint8_t)(62u + (f & 1u) * 3u);
                    adrift_dirty_clear(&dirty);
                    adrift_app_tick_ui(&in_st, fb, &dirty);

                    CHECK(!dirty.full_redraw, "Unexpected full_redraw on incremental frame (page=%u mode=%u)", page, mode_idx);

                    uint32_t pane_dirty = 0u;
                    for (uint32_t y = 0u; y < ADRIFT_SCREEN_H; y++) {
                        bool row_marked  = (dirty.row_mask[y >> 5] & (1u << (y & 31u))) != 0u;
                        bool row_changed = memcmp(&fb[y * ADRIFT_SCREEN_W],
                                                  &fb_prev[y * ADRIFT_SCREEN_W],
                                                  ADRIFT_SCREEN_W * sizeof(uint16_t)) != 0;
                        CHECK(!row_changed || row_marked,
                              "Unmarked modified row y=%u on HOME page=%u mode=%u frame=%u",
                              y, page, mode_idx, f);
                        if (y >= 28u && row_marked) pane_dirty++;
                    }
                    if (pane_dirty < min_pane_dirty) min_pane_dirty = pane_dirty;

                    /* Compare incremental framebuffer against a fresh full-chrome render from identical pre-step state */
                    adrift_ui_t    ui_ref = ui_snap;
                    adrift_dirty_t d_ref;
                    adrift_dirty_clear(&d_ref);
                    d_ref.full_redraw      = true;
                    ui_ref.prev_preset     = 0xFFu;
                    ui_ref.prev_scope_page = 0xFFu;
                    for (uint32_t px = 0u; px < ADRIFT_SCREEN_W * ADRIFT_SCREEN_H; px++) {
                        fb_ref[px] = 0xA55Au;
                    }
                    adrift_ui_step(&ui_ref, s, &in_st, fb_ref, &d_ref);
                    for (uint32_t y = 0u; y < ADRIFT_SCREEN_H; y++) {
                        for (uint32_t x = 0u; x < ADRIFT_SCREEN_W; x++) {
                            uint16_t got  = fb[y * ADRIFT_SCREEN_W + x];
                            uint16_t want = fb_ref[y * ADRIFT_SCREEN_W + x];
                            CHECK(got == want,
                                  "Pixel mismatch at (%u,%u) on HOME page=%u mode=%u frame=%u: got=0x%04x want=0x%04x",
                                  x, y, page, mode_idx, f, got, want);
                        }
                    }
                }
                CHECK(min_pane_dirty < 200u,
                      "Expected incremental HOME page=%u mode=%u to dirty <200 pane rows (min=%u)",
                      page, mode_idx, min_pane_dirty);
            }
        }
        free(fb_prev);
        free(fb_ref);
    }

    free(fb);
    free(pcm);
    printf("  -> PASS (G1=3 Default, SAVE Menu, Free-Pool Cap, Multi-Ch 16V RMS=%.1f dBFS, Unison+Detune OK)\n",
           chord_rms);
}

/* =========================================================================
 * Test: Persistent User Presets (U1..U24), Atomic Flash Recovery, Save Modal,
 *       Single-Group Patch Load & MIDI Bank Select (CC0 + Program Change)
 * ========================================================================= */
void test_user_presets_and_save_ui(void) {
    printf("[Test 17] User Presets (U1..U24), Flash CRC/Recovery, Save UI & Group Load...\n");
    uint16_t *fb  = (uint16_t *)calloc(ADRIFT_SCREEN_W * ADRIFT_SCREEN_H, sizeof(uint16_t));
    int16_t  *pcm = (int16_t *)calloc(2048u * 2u, sizeof(int16_t));

    /* 1. Clean erased NOR flash boot: all 24 user slots unoccupied */
    adrift_headless_storage_reset();
    adrift_app_init();
    adrift_synth_t *s  = adrift_app_get_synth();
    adrift_ui_t    *ui = adrift_app_get_ui();
    for (uint8_t u = 0u; u < ADRIFT_NUM_USER_PRESETS; u++) {
        CHECK(!adrift_synth_user_preset_is_occupied(u), "Expected U%u empty on erased flash", u + 1u);
    }

    /* 2. Customize all 6 groups + Master FX and save via Save Modal (knobs + direct text commands) */
    adrift_synth_load_preset(s, 0u);
    /* Reallocate 16 voices across G1..G6: 4 + 3 + 3 + 2 + 2 + 2 = 16 */
    adrift_synth_set_group_voices(s, 0u, 0u);
    const uint8_t target_nv[ADRIFT_MAX_GROUPS] = { 4u, 3u, 3u, 2u, 2u, 2u };
    for (uint8_t g = 0u; g < ADRIFT_MAX_GROUPS; g++) {
        CHECK(adrift_synth_set_group_voices(s, g, target_nv[g]), "Set G%u voices=%u", g + 1u, target_nv[g]);
        s->groups[g].assign_mode   = (uint8_t)(g % 3u);
        s->groups[g].midi_channel  = (uint8_t)(g + 1u);
        s->groups[g].unison_detune = (int16_t)(110 + g * 80);
        s->groups[g].stereo_spread = (int16_t)(200 + g * 100);
        s->groups[g].pan           = (int16_t)(-300 + (int)g * 120);
        s->groups[g].transpose     = (int8_t)(-12 + (int)g * 5);

        adrift_patch_t *p     = &s->groups[g].patch;
        p->exc.wave_morph     = (int16_t)(100 + g * 140);
        p->exc.pulse_width    = (int16_t)(250 + g * 90);
        p->exc.vco_level      = (int16_t)(700 + g * 40);
        p->exc.vco_octave     = (int8_t)(-2 + (int)(g % 5u));
        p->exc.vco_semi       = (int8_t)(-7 + (int)g * 3);
        p->exc.vco_fine       = (int16_t)(-25 + (int)g * 10);
        p->exc.noise_level    = (int16_t)(50 + g * 70);
        p->exc.sh_rate        = (int16_t)(300 + g * 80);
        p->exc.analog_drift   = (int16_t)(180 + g * 50);
        p->exc.pluck          = (int16_t)(120 + g * 95);
        p->exc.vco_keytrack   = (g & 1u) == 0u;

        p->env1.onset_delay   = (int16_t)(40 * g);
        p->env1.attack        = (int16_t)(120 + g * 60);
        p->env1.decay         = (int16_t)(310 + g * 50);
        p->env1.sustain       = (int16_t)(500 + g * 40);
        p->env1.release       = (int16_t)(420 + g * 55);
        p->env1.vel_sens      = (int16_t)(300 + g * 60);
        p->env1.loop_mode     = (g == 2u) ? ADRIFT_ENV_LOOP_DAD : ADRIFT_ENV_LOOP_OFF;
        p->env1.range         = (uint8_t)(g % 3u);

        p->in_lpf_cutoff      = (int16_t)(520 + g * 65);
        p->in_lpf_res         = (int16_t)(140 + g * 70);
        p->bbd.feedback       = (int16_t)(410 + g * 75);
        p->bbd.fb_lpf_cutoff  = (int16_t)(600 + g * 45);
        p->bbd.dispersion     = (int16_t)(90 + g * 85);
        p->bbd.invert         = (g & 1u) != 0u;
        p->bbd.invert_pitch_comp = (g & 2u) != 0u;
        p->bbd.glide          = (int16_t)(30 * g);
        p->dry_wet            = (int16_t)(550 + g * 60);

        p->env2.onset_delay   = (int16_t)(20 * g);
        p->env2.attack        = (int16_t)(80 + g * 50);
        p->env2.decay         = (int16_t)(360 + g * 40);
        p->env2.sustain       = (int16_t)(620 + g * 35);
        p->env2.release       = (int16_t)(480 + g * 50);
        p->env2.vel_sens      = (int16_t)(220 + g * 55);
        p->env2.drone_hold    = (g == 5u);

        p->master_vol         = (int16_t)(720 + g * 30);
        p->lfo_wave           = (uint8_t)(g % ADRIFT_LFO_COUNT);
        p->lfo_rate           = (int16_t)(210 + g * 60);
        p->lfo_depth          = (int16_t)(-240 + (int)g * 95);
        p->lfo_dest           = (uint8_t)(g % ADRIFT_LFO1_DEST_COUNT);
        p->lfo2_wave          = (uint8_t)((g + 1u) % ADRIFT_LFO_COUNT);
        p->lfo2_rate          = (int16_t)(150 + g * 55);
        p->lfo2_depth         = (int16_t)(180 - (int)g * 60);
        p->lfo2_dest          = ADRIFT_MOD_LFO1_RATE;
        p->lfo3_wave          = (uint8_t)((g + 2u) % ADRIFT_LFO_COUNT);
        p->lfo3_rate          = (int16_t)(330 + g * 40);
        p->lfo3_depth         = (int16_t)(-100 + (int)g * 45);
        p->lfo3_dest          = ADRIFT_MOD_DISPR;
        p->mw_depth           = (int16_t)(400 + g * 35);
        p->pb_range           = (uint8_t)(2u + g * 2u);
        p->legato             = (g & 1u) != 0u;

        p->arp.mode           = (uint8_t)((g + 1u) % ADRIFT_ARP_MODE_COUNT);
        p->arp.range          = (uint8_t)(1u + (g % 4u));
        p->arp.rate_bpm       = (uint16_t)(95u + g * 18u);
        p->arp.gate           = (uint8_t)(45u + g * 8u);
        p->arp.velocity       = (uint8_t)(g == 0u ? 0u : (80u + g * 6u));
        p->arp.latch          = (g == 3u);
        p->arp.swing          = (uint8_t)(12u * g);
        p->arp.ratchet        = (uint8_t)(g % ADRIFT_RATCHET_COUNT);
    }

    s->fx_params.chorus_mix     = 412;
    s->fx_params.delay_time     = 634;
    s->fx_params.delay_fb       = 518;
    s->fx_params.delay_mix      = 377;
    s->fx_params.reverb_size    = 789;
    s->fx_params.reverb_damp    = 345;
    s->fx_params.reverb_shimmer = 456;
    s->fx_params.reverb_mix     = 521;
    s->fx_params.comp_mix       = 688;
    s->fx_params.comp_thresh    = 312;
    s->fx_params.comp_attack    = 275;
    s->fx_params.comp_makeup    = 244;

    adrift_synth_snapshot_t expected_snap;
    adrift_synth_save_snapshot(s, &expected_snap);

    /* Open Save Preset modal via ADRIFT_BTN_SAVE_PRESET -> defaults to first empty slot U1 (slot 0) */
    adrift_input_state_t in_st;
    adrift_dirty_t       dirty;
    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed = ADRIFT_BTN_SAVE_PRESET;
    in_st.master_pot      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->in_save_menu && ui->save_slot == 0u && ui->save_cursor == 0u,
          "Expected Save Modal open at slot 0 (U1), got in_save=%d slot=%u cur=%u",
          ui->in_save_menu, ui->save_slot, ui->save_cursor);

    /* Test KNOB3 character stepping + KNOB2 cursor + KNOB4 reset, then type "DRIFT PAD 1" */
    memset(&in_st, 0, sizeof(in_st));
    in_st.enc_delta[ADRIFT_ENC_KNOB3] = 2; /* 'A' -> 'C' at cursor 0 */
    in_st.master_pot                  = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);

    memset(&in_st, 0, sizeof(in_st));
    in_st.enc_delta[ADRIFT_ENC_KNOB2] = 1; /* cursor 0 -> 1 */
    in_st.master_pot                  = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->save_name[0] == 'C' && ui->save_cursor == 1u,
          "Expected KNOB3/KNOB2 edit ('C', cur=1), got '%c' cur=%u", ui->save_name[0], ui->save_cursor);

    /* KNOB4 negative turn resets back to save_orig_name ("ACOUST GUIT ") and cursor 0 */
    memset(&in_st, 0, sizeof(in_st));
    in_st.enc_delta[ADRIFT_ENC_KNOB4] = -1;
    in_st.master_pot                  = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->save_name[0] == 'A' && ui->save_cursor == 0u,
          "Expected KNOB4 -1 to reset name to 'A...', got '%c' cur=%u", ui->save_name[0], ui->save_cursor);

    /* Clear remaining chars via KNOB4 +1 from cursor 0, then type "DRIFT PAD 1" via text_char */
    memset(&in_st, 0, sizeof(in_st));
    in_st.enc_delta[ADRIFT_ENC_KNOB4] = 1;
    in_st.master_pot                  = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(strncmp(ui->save_name, "            ", 12u) == 0, "Expected KNOB4 +1 clear-to-end");

    const char *custom_name = "DRIFT PAD 1";
    for (size_t i = 0u; custom_name[i] != '\0'; i++) {
        memset(&in_st, 0, sizeof(in_st));
        in_st.text_char  = custom_name[i];
        in_st.master_pot = -1;
        adrift_dirty_clear(&dirty);
        adrift_app_tick_ui(&in_st, fb, &dirty);
    }

    /* Confirm save with OCT+ and flush pending NOR write */
    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed = ADRIFT_BTN_OCT_UP;
    in_st.master_pot      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(adrift_preset_store_flush_pending(), "Expected pending flash commit for U1");
    CHECK(!ui->in_save_menu && s->preset_index == 24u &&
          strcmp(s->preset_name, "DRIFT PAD 1") == 0 &&
          ui->save_toast_frames == 45u,
          "Expected saved U1 ('DRIFT PAD 1', idx=24, toast=45), got idx=%u name='%s' toast=%u",
          s->preset_index, s->preset_name, ui->save_toast_frames);

    /* 3. Simulate full power-cycle reboot WITHOUT wiping flash -> verify bit-exact round-trip */
    adrift_synth_init_patch(s);
    CHECK(s->preset_index == ADRIFT_PRESET_INIT, "Expected INIT patch before simulated reboot");

    adrift_app_init();
    s  = adrift_app_get_synth();
    ui = adrift_app_get_ui();
    CHECK(adrift_synth_user_preset_is_occupied(0u) &&
          strcmp(adrift_synth_get_user_preset_name(0u), "DRIFT PAD 1") == 0,
          "Expected U1 to persist across reboot (got occ=%d name='%s')",
          adrift_synth_user_preset_is_occupied(0u), adrift_synth_get_user_preset_name(0u));

    adrift_synth_load_preset(s, 24u); /* Load U1 */
    CHECK(s->preset_index == 24u && strcmp(s->preset_name, "DRIFT PAD 1") == 0,
          "Expected loaded U1 preset_index=24 name='DRIFT PAD 1'");
    CHECK(memcmp(&s->fx_params, &expected_snap.fx_params, sizeof(s->fx_params)) == 0,
          "Master FX params mismatch after flash round-trip");

    for (uint8_t g = 0u; g < ADRIFT_MAX_GROUPS; g++) {
        const adrift_group_t          *got  = &s->groups[g];
        const adrift_group_snapshot_t *want = &expected_snap.groups[g];
        CHECK(got->num_voices    == want->num_voices &&
              got->assign_mode   == want->assign_mode &&
              got->midi_channel  == want->midi_channel &&
              got->unison_detune == want->unison_detune &&
              got->stereo_spread == want->stereo_spread &&
              got->pan           == want->pan &&
              got->transpose     == want->transpose,
              "Group G%u header mismatch after U1 reload", g + 1u);
        CHECK(got->patch.exc.wave_morph   == want->patch.exc.wave_morph &&
              got->patch.exc.pluck        == want->patch.exc.pluck &&
              got->patch.exc.vco_keytrack == want->patch.exc.vco_keytrack &&
              got->patch.env1.range       == want->patch.env1.range &&
              got->patch.env1.loop_mode   == want->patch.env1.loop_mode &&
              got->patch.bbd.feedback     == want->patch.bbd.feedback &&
              got->patch.bbd.dispersion   == want->patch.bbd.dispersion &&
              got->patch.bbd.invert       == want->patch.bbd.invert &&
              got->patch.bbd.invert_pitch_comp == want->patch.bbd.invert_pitch_comp &&
              got->patch.env2.drone_hold  == want->patch.env2.drone_hold &&
              got->patch.lfo2_dest        == want->patch.lfo2_dest &&
              got->patch.lfo3_dest        == want->patch.lfo3_dest &&
              got->patch.arp.mode         == want->patch.arp.mode &&
              got->patch.arp.rate_bpm     == want->patch.arp.rate_bpm &&
              got->patch.arp.latch        == want->patch.arp.latch &&
              got->patch.arp.ratchet      == want->patch.arp.ratchet,
              "Group G%u patch/arp/mod mismatch after U1 reload", g + 1u);
    }

    /* 4. CRC32 corruption rejection: save U3 (slot 2), flip payload byte, verify clean rejection */
    CHECK(adrift_synth_save_user_preset(s, 2u, "BAD CRC TST") && adrift_preset_store_flush_pending(),
          "Save U3 for CRC test");
    CHECK(adrift_synth_user_preset_is_occupied(2u), "U3 occupied before corruption");
    adrift_headless_storage_raw_slot(2u)[24] ^= 0x5Au;
    adrift_preset_store_init();
    CHECK(!adrift_synth_user_preset_is_occupied(2u) && adrift_synth_user_preset_is_occupied(0u),
          "Corrupted U3 must be rejected while U1 remains intact");

    /* 5. Atomic backup-sector recovery: simulate power loss right as target slot 1 (U2) is erased */
    adrift_synth_load_preset(s, 3u); /* P4 */
    adrift_headless_storage_abort_on_target_write(true);
    CHECK(adrift_synth_save_user_preset(s, 1u, "RECOVER ME"), "Queue U2 save");
    CHECK(!adrift_preset_store_flush_pending(), "Expected simulated power-cut failure during target slot erase");
    /* Verify target slot 1 is corrupt/unreadable while backup slot 24 holds valid image */
    CHECK(adrift_headless_storage_raw_slot(1u)[0] == 0x41u &&
          adrift_headless_storage_raw_slot(1u)[1] == 0xFFu &&
          memcmp(adrift_headless_storage_raw_slot(24u), "ADRP", 4u) == 0,
          "Expected partially erased/corrupt slot 1 + intact backup slot 24 after simulated power cut");

    adrift_headless_storage_abort_on_target_write(false);
    adrift_preset_store_init(); /* Boot recovery replays backup slot 24 -> slot 1 and erases slot 24 */
    CHECK(adrift_synth_user_preset_is_occupied(1u) &&
          strcmp(adrift_synth_get_user_preset_name(1u), "RECOVER ME") == 0 &&
          adrift_headless_storage_raw_slot(24u)[0] == 0xFFu,
          "Expected boot recovery to restore U2 ('RECOVER ME') and clear backup slot 24");

    /* 6. Single-group patch load (2:MD = GRP, 3:SR = G2) & two-step Clear User Slot (4:CL) */
    adrift_synth_load_preset(s, 24u); /* Load U1 ("DRIFT PAD 1") */
    s->edit_group = 2u;               /* Target destination = G3 */
    const int16_t orig_g1_morph  = s->groups[0].patch.exc.wave_morph;
    const int32_t orig_chorus    = s->fx_params.chorus_mix;
    const uint8_t orig_g3_voices = s->groups[2].num_voices;

    /* Open Presets Browser, switch 2:MD -> GRP (+1), 3:SR -> G2 (+1), scroll to P2 (cursor 3), confirm OCT+ */
    memset(&in_st, 0, sizeof(in_st));
    in_st.enc_delta[ADRIFT_ENC_PRESETS] = 1;
    in_st.master_pot                    = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->in_preset_menu && ui->preset_cursor == 26u,
          "Expected Presets Browser to open at U1 (cursor 26), got %u", ui->preset_cursor);

    /* Test two-step clear on U2 (cursor 27): scroll +1 to U2, arm 4:CL, confirm OCT+ */
    memset(&in_st, 0, sizeof(in_st));
    in_st.enc_delta[ADRIFT_ENC_PRESETS] = 1; /* Cursor 27 = U2 ("RECOVER ME") */
    in_st.master_pot                    = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->preset_cursor == 27u && adrift_synth_user_preset_is_occupied(1u), "At U2 before clear");

    memset(&in_st, 0, sizeof(in_st));
    in_st.enc_delta[ADRIFT_ENC_KNOB4] = 1; /* Arm clear */
    in_st.master_pot                  = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->preset_clear_armed, "Expected 4:CL to arm user slot clear");

    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed = ADRIFT_BTN_OCT_UP; /* Confirm clear U2 (stays in browser!) */
    in_st.master_pot      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->in_preset_menu && !ui->preset_clear_armed && !adrift_synth_user_preset_is_occupied(1u),
          "Expected OCT+ to clear U2 and stay in Presets Browser");

    /* Now scroll from 27 back to P2 (cursor 3: delta -24), switch 2:MD -> GRP (auto-picks P2's G1 since G3 has 0 voices in P2!),
     * then step 3:SR +1 to G2 (index 1) and confirm OCT+ */
    memset(&in_st, 0, sizeof(in_st));
    in_st.enc_delta[ADRIFT_ENC_PRESETS] = -24; /* 27 -> 3 (P2: BOWED CELLO) */
    in_st.enc_delta[ADRIFT_ENC_KNOB2]   = 1;   /* FULL -> GRP (restores snapshot edit_group=G3, auto-picks G1 in P2) */
    in_st.enc_delta[ADRIFT_ENC_KNOB3]   = 1;   /* G1 (0) -> G2 (1) */
    in_st.master_pot                    = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->preset_load_grp && ui->preset_src_grp == 1u && ui->preset_cursor == 3u && s->edit_group == 2u,
          "Expected GRP mode (dst=G3, src=G2, cur=3), got grp=%d dst=%u src=%u cur=%u",
          ui->preset_load_grp, s->edit_group, ui->preset_src_grp, ui->preset_cursor);

    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed = ADRIFT_BTN_OCT_UP;
    in_st.master_pot      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);

    /* Verify G3 received P2's G2 patch while G1, G3's voice allocation, and Master FX stayed untouched! */
    adrift_synth_t *ref_p2 = (adrift_synth_t *)calloc(1u, sizeof(adrift_synth_t));
    adrift_synth_init(ref_p2);
    adrift_synth_load_preset(ref_p2, 1u);
    CHECK(!ui->in_preset_menu &&
          s->groups[0].patch.exc.wave_morph == orig_g1_morph &&
          s->fx_params.chorus_mix == orig_chorus &&
          s->groups[2].num_voices == orig_g3_voices &&
          memcmp(&s->groups[2].patch, &ref_p2->groups[1].patch, sizeof(adrift_patch_t)) == 0,
          "Single-group load should copy P2:G2 into G3 while preserving G1, voice count, and Master FX");
    free(ref_p2);

    /* 7. MIDI Bank Select MSB (CC0: 0=Factory, 1=User; CC32 remains LFO 2 Rate!) + Program Change */
    const int16_t prev_lfo2_rate = s->groups[0].patch.lfo2_rate;
    adrift_synth_handle_midi(s, 0xB0u, 32u, 100u); /* CC32 must still control G1 LFO 2 Rate */
    CHECK(s->groups[0].patch.lfo2_rate != prev_lfo2_rate && s->midi_bank == 0u,
          "CC32 must remain mapped to LFO 2 Rate without altering midi_bank");

    /* Bank 0 Program Change 24 -> loads U1 (occupied); Program Change 25 -> U2 (empty, ignored!) */
    adrift_synth_handle_midi(s, 0xC0u, 24u, 0u);
    CHECK(s->preset_index == 24u, "Expected Bank 0 PC 24 to load U1, got %u", s->preset_index);
    adrift_synth_handle_midi(s, 0x90u, 60u, 110u);
    adrift_synth_render_block(s, pcm, 256u, NULL, 0u);
    CHECK(s->any_gate, "Expected active voice on U1 before ignored PC");
    adrift_synth_handle_midi(s, 0xC0u, 25u, 0u); /* U2 is empty -> must be ignored without cutting active voice */
    CHECK(s->preset_index == 24u && s->any_gate,
          "Program Change to empty user slot U2 must be ignored without cutting active voices");

    /* CC0 = 1 (User Bank) + PC 0 -> loads U1; CC0 = 0 (Factory Bank) + PC 1 -> loads P2 */
    adrift_synth_handle_midi(s, 0xB0u, 0u, 0u);
    adrift_synth_handle_midi(s, 0xC0u, 1u, 0u);
    CHECK(s->preset_index == 1u, "Expected CC0=0 + PC 1 to load P2, got %u", s->preset_index);
    adrift_synth_handle_midi(s, 0xB0u, 0u, 1u);
    adrift_synth_handle_midi(s, 0xC0u, 0u, 0u);
    CHECK(s->preset_index == 24u, "Expected CC0=1 + PC 0 to load U1 (24), got %u", s->preset_index);

    /* Leave headless storage clean for any subsequent tools/tests */
    adrift_headless_storage_reset();
    adrift_preset_store_init();

    free(fb);
    free(pcm);
    printf("  -> PASS (U1..U24 Round-Trip, IEEE CRC32 Reject, Backup Sector Recovery, GRP Load & MIDI CC0 OK)\n");
}

/* =========================================================================
 * Test: Power-On Boot Animation, Dirty-Band Equivalence, Dock & Play-Through
 * ========================================================================= */
void test_boot_animation(void) {
    printf("[Test 18] Boot Animation (68-Frame Bloom, Dirty-Band Equivalence, Dock & Play-Through)...\n");
    const uint32_t npx = ADRIFT_SCREEN_W * ADRIFT_SCREEN_H;
    uint16_t *fb      = (uint16_t *)calloc(npx, sizeof(uint16_t));
    uint16_t *fb_prev = (uint16_t *)calloc(npx, sizeof(uint16_t));
    uint16_t *fb_ref  = (uint16_t *)calloc(npx, sizeof(uint16_t));
    int16_t  *pcm     = (int16_t *)calloc(1024u * 2u, sizeof(int16_t));

    adrift_app_init();
    adrift_synth_t *s  = adrift_app_get_synth();
    adrift_ui_t    *ui = adrift_app_get_ui();
    CHECK(ui->boot_active && ui->boot_frame == 0u,
          "Expected boot_active=true, boot_frame=0 after adrift_app_init");

    /* 1. Silent boot audio + passive 0xF8 MIDI Clock & MASTER pot tracking must not dismiss boot */
    adrift_synth_handle_midi_realtime(s, 0xF8u);
    adrift_headless_render(pcm, 512u, NULL, 0u);
    double idle_rms = compute_rms_dbfs(pcm, 0u, 512u);
    CHECK(idle_rms < -120.0, "Expected silent audio during boot (got %.1f dBFS)", idle_rms);

    uint32_t max_inc_dirty   = 0u;
    uint32_t max_glide_dirty = 0u;
    const uint16_t col_bg    = (uint16_t)((((12u) & 0xF8u) << 8) | (((14u) & 0xFCu) << 3) | (((20u) & 0xF8u) >> 3));
    const uint16_t g1_gold   = adrift_ui_group_accent_rgb565(0u);

    /* 2. Step all 68 boot frames and verify dirty-band accuracy + bit-exact full-redraw equivalence */
    for (uint32_t f = 0u; f < ADRIFT_UI_BOOT_FRAMES; f++) {
        memcpy(fb_prev, fb, npx * sizeof(uint16_t));
        adrift_ui_t ui_snap = *ui;

        adrift_input_state_t in_st;
        memset(&in_st, 0, sizeof(in_st));
        in_st.master_pot   = 900; /* Hardware/web master pot must update synth without dismissing boot */
        in_st.cpu_load_pct = 7u;

        adrift_dirty_t dirty;
        adrift_dirty_clear(&dirty);
        adrift_app_tick_ui(&in_st, fb, &dirty);

        CHECK(ui->boot_active && ui->boot_frame == (uint8_t)(f + 1u) && s->hw_master_pot == 900,
              "Expected boot frame %u->%u active with hw_master_pot=900", f, f + 1u);

        if (f == 0u) {
            CHECK(dirty.full_redraw, "Expected full_redraw on boot frame 0");
        } else {
            CHECK(!dirty.full_redraw, "Unexpected full_redraw on incremental boot frame %u", f);
            uint32_t dirty_rows = 0u;
            for (uint32_t y = 0u; y < ADRIFT_SCREEN_H; y++) {
                bool row_marked  = (dirty.row_mask[y >> 5] & (1u << (y & 31u))) != 0u;
                bool row_changed = memcmp(&fb[y * ADRIFT_SCREEN_W],
                                          &fb_prev[y * ADRIFT_SCREEN_W],
                                          ADRIFT_SCREEN_W * sizeof(uint16_t)) != 0;
                CHECK(!row_changed || row_marked,
                      "Unmarked modified row y=%u on boot frame %u", y, f);
                if (row_marked) dirty_rows++;
            }
            CHECK(dirty_rows > 0u && dirty_rows <= 96u,
                  "Boot frame %u dirty_rows=%u out of [1..96] SPI DMA fast budget", f, dirty_rows);
            if (dirty_rows > max_inc_dirty) max_inc_dirty = dirty_rows;
            if (f >= 52u && dirty_rows > max_glide_dirty) max_glide_dirty = dirty_rows;
        }

        /* Bit-exact comparison against a fresh full-screen clear render of frame f */
        adrift_ui_t ui_ref = ui_snap;
        ui_ref.initialized = false;
        for (uint32_t i = 0u; i < npx; i++) {
            fb_ref[i] = 0xA55Au;
        }
        adrift_dirty_t d_ref;
        adrift_dirty_clear(&d_ref);
        adrift_ui_step(&ui_ref, s, &in_st, fb_ref, &d_ref);
        CHECK(memcmp(fb, fb_ref, npx * sizeof(uint16_t)) == 0,
              "Incremental vs fresh render pixel mismatch on boot frame %u", f);

        if (f == 26u) {
            uint32_t lit_wm = 0u, lit_str = 0u, lit_sub = 0u;
            for (uint32_t y = 82u; y <= 103u; y++) {
                for (uint32_t x = 42u; x < 198u; x++) {
                    if (fb[y * ADRIFT_SCREEN_W + x] != col_bg) lit_wm++;
                }
            }
            for (uint32_t y = 105u; y <= 139u; y++) {
                for (uint32_t x = 12u; x <= 227u; x++) {
                    if (fb[y * ADRIFT_SCREEN_W + x] != col_bg) lit_str++;
                }
            }
            for (uint32_t y = 141u; y <= 156u; y++) {
                for (uint32_t x = 40u; x < 200u; x++) {
                    if (fb[y * ADRIFT_SCREEN_W + x] != col_bg) lit_sub++;
                }
            }
            CHECK(lit_wm > 400u && lit_str > 600u && lit_sub > 200u,
                  "Expected rich bloom on frame 26 (wm=%u, str=%u, sub=%u)", lit_wm, lit_str, lit_sub);
        }

        if (f == ADRIFT_UI_BOOT_FRAMES - 1u) {
            /* Final boot frame must dock cleanly onto (x=5..234, y=21..23) in G1 Gold */
            CHECK(fb[21u * ADRIFT_SCREEN_W + 4u] == col_bg &&
                  fb[21u * ADRIFT_SCREEN_W + 5u] == g1_gold &&
                  fb[22u * ADRIFT_SCREEN_W + 120u] == g1_gold &&
                  fb[23u * ADRIFT_SCREEN_W + 234u] == g1_gold &&
                  fb[21u * ADRIFT_SCREEN_W + 235u] == col_bg,
                  "Expected final boot frame %u to dock exactly at Header BBD bar (x=5..234, y=21..23)", f);
        }
    }
    CHECK(max_glide_dirty <= 32u,
          "Expected Act IV upward glide to dirty <=32 rows/frame for 60fps SPI DMA (got %u)", max_glide_dirty);

    /* 3. Frame 68 transitions seamlessly to main UI with full docked bar + 12-frame post-dock decay */
    adrift_input_state_t in_idle;
    memset(&in_idle, 0, sizeof(in_idle));
    in_idle.master_pot   = -1;
    in_idle.cpu_load_pct = 7u;
    adrift_dirty_t dirty;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_idle, fb, &dirty);
    CHECK(!ui->boot_active && dirty.full_redraw && ui->boot_bar_decay == 11u && ui->prev_meter_w == 230u &&
          fb[22u * ADRIFT_SCREEN_W + 234u] == g1_gold,
          "Expected frame 68 UI reveal to preserve full 230px docked Gold bar (decay=%u, w=%u)",
          ui->boot_bar_decay, ui->prev_meter_w);

    for (uint32_t d = 0u; d < 12u; d++) {
        adrift_dirty_clear(&dirty);
        adrift_app_tick_ui(&in_idle, fb, &dirty);
        uint32_t drows = 0u;
        for (uint32_t y = 0u; y < ADRIFT_SCREEN_H; y++) {
            if ((dirty.row_mask[y >> 5] & (1u << (y & 31u))) != 0u) drows++;
        }
        CHECK(!dirty.full_redraw && drows == 3u,
              "Expected 3-row micro-dirty decay on post-dock frame %u (got full=%d rows=%u)",
              d, dirty.full_redraw, drows);
    }
    CHECK(ui->boot_bar_decay == 0u && ui->prev_meter_w == 0u,
          "Expected post-dock BBD bar decay to settle to 0 (decay=%u, w=%u)",
          ui->boot_bar_decay, ui->prev_meter_w);

    /* 4. Zero-Delay Play-Through: Button press, Encoder turn, and MIDI Note On during boot */
    /* 4a. Button press mid-boot opens FX screen on that exact tick */
    adrift_app_init();
    ui = adrift_app_get_ui();
    s  = adrift_app_get_synth();
    for (uint32_t i = 0u; i < 8u; i++) {
        adrift_dirty_clear(&dirty);
        adrift_app_tick_ui(&in_idle, fb, &dirty);
    }
    CHECK(ui->boot_active && ui->boot_frame == 8u, "Mid-boot before FX button");
    adrift_input_state_t in_btn = in_idle;
    in_btn.buttons_pressed = ADRIFT_BTN_FX;
    in_btn.buttons_down    = ADRIFT_BTN_FX;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_btn, fb, &dirty);
    CHECK(!ui->boot_active && ui->in_fx_menu && dirty.full_redraw,
          "FX button during boot must dismiss splash AND open FX menu without swallowing press");

    /* 4b. Encoder turn mid-boot updates parameter on that exact tick */
    adrift_app_init();
    ui = adrift_app_get_ui();
    s  = adrift_app_get_synth();
    int16_t orig_g2_morph = s->groups[1].patch.exc.wave_morph;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_idle, fb, &dirty);
    adrift_input_state_t in_enc = in_idle;
    in_enc.enc_delta[ADRIFT_ENC_SELECT] = 1; /* G1 -> G2 */
    in_enc.enc_delta[ADRIFT_ENC_KNOB1]  = 5; /* Edit G2 Wave Morph */
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_enc, fb, &dirty);
    CHECK(!ui->boot_active && s->edit_group == 1u && s->groups[1].patch.exc.wave_morph != orig_g2_morph,
          "Encoder turn during boot must dismiss splash AND apply delta immediately");

    /* 4c. Incoming MIDI Note On mid-boot sounds immediately on audio block 0 AND dismisses boot UI */
    adrift_app_init();
    ui = adrift_app_get_ui();
    s  = adrift_app_get_synth();
    for (uint32_t i = 0u; i < 5u; i++) {
        adrift_dirty_clear(&dirty);
        adrift_app_tick_ui(&in_idle, fb, &dirty);
    }
    adrift_midi_msg_t note_on = { 0x90u, 60u, 112u, 0u };
    adrift_headless_render(pcm, 512u, &note_on, 1u);
    double note_rms = compute_rms_dbfs(pcm, 0u, 512u);
    CHECK(note_rms > -42.0 && s->any_gate,
          "MIDI Note On during boot must sound immediately without latency/swallowing (rms=%.1f dBFS)", note_rms);
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_idle, fb, &dirty);
    CHECK(!ui->boot_active && dirty.full_redraw && s->any_gate,
          "Active MIDI voice must dismiss boot splash on next UI tick");

    free(pcm);
    free(fb_ref);
    free(fb_prev);
    free(fb);
    printf("  -> PASS (68 Frames Bit-Exact, MaxDirty=%u, GlideDirty=%u, Seamless Dock & Zero-Delay Play-Through)\n",
           max_inc_dirty, max_glide_dirty);
}

/* =========================================================================
 * Test: Power-On MASTER Volume Knob Sync (Zero-Transient Snap, 0..1023 & Speech)
 * ========================================================================= */
static void boot_volume_setup_instant_patch(adrift_synth_t *s) {
    s->groups[0].patch.env1.onset_delay = 0;
    s->groups[0].patch.env1.attack      = 0;
    s->groups[0].patch.env2.attack      = 0;
    s->groups[0].patch.exc.vco_level    = 900;
    s->groups[0].patch.dry_wet          = 0;
}

void test_boot_master_volume(void) {
    printf("[Test 20] Boot Master Volume (Zero-Transient Snap, 0..1023 Range & Line-Out Speech Scaling)...\n");
    const uint32_t nframes = 2048u;
    const uint32_t npx     = ADRIFT_SCREEN_W * ADRIFT_SCREEN_H;
    int16_t  *pcm_1023 = (int16_t *)calloc(nframes * 2u, sizeof(int16_t));
    int16_t  *pcm_512  = (int16_t *)calloc(nframes * 2u, sizeof(int16_t));
    int16_t  *pcm_256  = (int16_t *)calloc(nframes * 2u, sizeof(int16_t));
    int16_t  *pcm_0    = (int16_t *)calloc(nframes * 2u, sizeof(int16_t));
    uint16_t *fb       = (uint16_t *)calloc(npx, sizeof(uint16_t));

    const adrift_midi_msg_t note_on = { 0x90u, 60u, 120u, 0u };

    /* 1. Boot with MASTER knob at 0 -> exact 0 LSB digital silence from sample 0 of block 0 */
    adrift_app_init();
    adrift_synth_t *s = adrift_app_get_synth();
    boot_volume_setup_instant_patch(s);
    adrift_synth_set_master_pot(s, 0, true);
    CHECK(s->hw_master_pot == 0 && s->master_gain_q15 == 0,
          "Expected boot pot=0 to snap hw_master_pot=0 and master_gain_q15=0 (got pot=%d, gain=%d)",
          (int)s->hw_master_pot, (int)s->master_gain_q15);
    memset(pcm_0, 0x5A, nframes * 2u * sizeof(int16_t));
    adrift_headless_render(pcm_0, 512u, &note_on, 1u);
    int32_t peak_zero = 0;
    for (uint32_t i = 0u; i < 512u * 2u; i++) {
        int32_t v = (pcm_0[i] < 0) ? -(int32_t)pcm_0[i] : (int32_t)pcm_0[i];
        if (v > peak_zero) peak_zero = v;
    }
    CHECK(peak_zero == 0,
          "Boot with MASTER pot=0 must produce exact 0 LSB on block 0 with zero ramp transient (peak=%d)",
          (int)peak_zero);

    /* 2. Boot with MASTER knob at 1023, 512, 256 -> proportional gain from sample 0 of DMA block 0 */
    adrift_app_init();
    s = adrift_app_get_synth();
    boot_volume_setup_instant_patch(s);
    adrift_synth_set_master_pot(s, 1023, true);
    CHECK(s->master_gain_q15 == 32767, "Expected boot pot=1023 -> master_gain_q15=32767 (got %d)",
          (int)s->master_gain_q15);
    adrift_headless_render(pcm_1023, 512u, &note_on, 1u);

    adrift_app_init();
    s = adrift_app_get_synth();
    boot_volume_setup_instant_patch(s);
    adrift_synth_set_master_pot(s, 512, true);
    adrift_headless_render(pcm_512, 512u, &note_on, 1u);

    adrift_app_init();
    s = adrift_app_get_synth();
    boot_volume_setup_instant_patch(s);
    adrift_synth_set_master_pot(s, 256, true);
    adrift_headless_render(pcm_256, 512u, &note_on, 1u);

    /* Check the very first 64-frame hardware DMA block (frames 0..64) as well as full 512 frames */
    double b0_1023  = compute_rms_dbfs(pcm_1023, 0u, 64u);
    double b0_512   = compute_rms_dbfs(pcm_512,  0u, 64u);
    double b0_256   = compute_rms_dbfs(pcm_256,  0u, 64u);
    double all_1023 = compute_rms_dbfs(pcm_1023, 0u, 512u);
    double all_512  = compute_rms_dbfs(pcm_512,  0u, 512u);
    double all_256  = compute_rms_dbfs(pcm_256,  0u, 512u);

    CHECK(b0_1023 > -30.0, "Expected strong block-0 output at pot=1023 (got %.2f dBFS)", b0_1023);
    CHECK(fabs((b0_512 - b0_1023) - (-6.012)) < 0.25 && fabs((all_512 - all_1023) - (-6.012)) < 0.20,
          "Expected boot pot=512 to be -6.0 dB vs 1023 from block 0 (b0=%.2f dB, all=%.2f dB)",
          b0_512 - b0_1023, all_512 - all_1023);
    CHECK(fabs((b0_256 - b0_1023) - (-12.033)) < 0.30 && fabs((all_256 - all_1023) - (-12.033)) < 0.25,
          "Expected boot pot=256 to be -12.0 dB vs 1023 from block 0 (b0=%.2f dB, all=%.2f dB)",
          b0_256 - b0_1023, all_256 - all_1023);

    /* 3. First UI tick transition (-1 -> 0..4 low pot range) snaps immediately without ignoring <=4 */
    adrift_app_init();
    s = adrift_app_get_synth();
    adrift_ui_t *ui = adrift_app_get_ui();
    CHECK(s->hw_master_pot == -1 && s->master_gain_q15 == 32767, "Initial unset pot state");
    adrift_input_state_t in_low;
    memset(&in_low, 0, sizeof(in_low));
    in_low.master_pot = 0; /* Previously treated as -1 by (pot_smooth > 4) */
    adrift_dirty_t dirty;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_low, fb, &dirty);
    CHECK(ui->boot_active && s->hw_master_pot == 0 && s->master_gain_q15 == 0,
          "First UI tick with master_pot=0 during boot must snap gain to 0 without dismissing splash (pot=%d, gain=%d)",
          (int)s->hw_master_pot, (int)s->master_gain_q15);

    /* Subsequent live knob movement updates target while preserving smooth block interpolation */
    in_low.master_pot = 1023;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_low, fb, &dirty);
    CHECK(s->hw_master_pot == 1023 && s->master_gain_q15 == 0,
          "Subsequent UI tick knob change must ramp smoothly in audio block rather than zipper-snapping");

    /* Also verify post-boot UI path snaps on first valid low pot value (e.g., pot = 3 <= 4) */
    adrift_app_init();
    s  = adrift_app_get_synth();
    ui = adrift_app_get_ui();
    adrift_ui_dismiss_boot(ui);
    in_low.master_pot = 3;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_low, fb, &dirty);
    CHECK(s->hw_master_pot == 3 && s->master_gain_q15 > 0 && s->master_gain_q15 < 128,
          "Post-boot first UI tick with master_pot=3 must snap low gain immediately (pot=%d, gain=%d)",
          (int)s->hw_master_pot, (int)s->master_gain_q15);

    /* 4. Line-out / headphone Klatt speech obeys boot MASTER volume (0 = silent, 512 = -6 dB vs 1023) */
    adrift_app_init();
    s  = adrift_app_get_synth();
    ui = adrift_app_get_ui();
    adrift_screen_reader_t *sr = adrift_app_get_screen_reader();
    adrift_synth_set_master_pot(s, 0, true);
    adrift_sr_set_enabled(sr, true, true, ui, s);
    memset(pcm_0, 0, nframes * 2u * sizeof(int16_t));
    adrift_app_mix_line_out(pcm_0, nframes);
    int32_t sp_peak_0 = 0;
    for (uint32_t i = 0u; i < nframes * 2u; i++) {
        int32_t v = (pcm_0[i] < 0) ? -(int32_t)pcm_0[i] : (int32_t)pcm_0[i];
        if (v > sp_peak_0) sp_peak_0 = v;
    }
    CHECK(sp_peak_0 == 0, "Expected line-out speech at boot pot=0 to be exact 0 LSB (got peak=%d)",
          (int)sp_peak_0);

    adrift_app_init();
    s  = adrift_app_get_synth();
    ui = adrift_app_get_ui();
    sr = adrift_app_get_screen_reader();
    adrift_synth_set_master_pot(s, 1023, true);
    adrift_sr_set_enabled(sr, true, true, ui, s);
    memset(pcm_1023, 0, nframes * 2u * sizeof(int16_t));
    adrift_app_mix_line_out(pcm_1023, nframes);

    adrift_app_init();
    s  = adrift_app_get_synth();
    ui = adrift_app_get_ui();
    sr = adrift_app_get_screen_reader();
    adrift_synth_set_master_pot(s, 512, true);
    adrift_sr_set_enabled(sr, true, true, ui, s);
    memset(pcm_512, 0, nframes * 2u * sizeof(int16_t));
    adrift_app_mix_line_out(pcm_512, nframes);

    double sp_rms_1023 = compute_rms_dbfs(pcm_1023, 0u, nframes);
    double sp_rms_512  = compute_rms_dbfs(pcm_512,  0u, nframes);
    CHECK(sp_rms_1023 > -35.0 && fabs((sp_rms_512 - sp_rms_1023) - (-6.012)) < 0.25,
          "Expected line-out speech at pot=512 to be -6.0 dB vs pot=1023 (1023=%.2f dBFS, diff=%.2f dB)",
          sp_rms_1023, sp_rms_512 - sp_rms_1023);

    /* 5. 24-bit Q23 audio path sub-16-bit dynamic range, zero-gated TPDF dither & Q23 speech mix */
    int32_t *q23_ref = (int32_t *)calloc(nframes * 2u, sizeof(int32_t));
    int32_t *q23_low = (int32_t *)calloc(nframes * 2u, sizeof(int32_t));
    int32_t *q23_0   = (int32_t *)calloc(nframes * 2u, sizeof(int32_t));

    /* 5a. Exact 0 Q23 LSBs at pot=0 */
    adrift_app_init();
    s = adrift_app_get_synth();
    boot_volume_setup_instant_patch(s);
    adrift_synth_set_master_pot(s, 0, true);
    memset(q23_0, 0x5A, nframes * 2u * sizeof(int32_t));
    adrift_app_audio_block_q23(q23_0, 512u, &note_on, 1u);
    int32_t q23_peak_0 = 0;
    for (uint32_t i = 0u; i < 512u * 2u; i++) {
        int32_t v = (q23_0[i] < 0) ? -q23_0[i] : q23_0[i];
        if (v > q23_peak_0) q23_peak_0 = v;
    }
    CHECK(q23_peak_0 == 0, "Expected Q23 output at pot=0 to be exact 0 Q23 LSB (got %d)", (int)q23_peak_0);

    /* 5b. Compare quiet (~-72 dBFS) Q23 render vs scaled full-level Q23 reference & 16-bit truncation */
    for (int pass = 0; pass < 2; pass++) {
        adrift_app_init();
        s = adrift_app_get_synth();
        adrift_synth_set_group_voices(s, 0u, 1u);
        s->groups[0].assign_mode             = ADRIFT_GROUP_ROUND_ROBIN;
        s->groups[0].pan                     = 0;
        s->groups[0].stereo_spread           = 0;
        s->groups[0].patch.exc.wave_morph    = 0; /* Pure sine VCO */
        s->groups[0].patch.exc.vco_level     = 900;
        s->groups[0].patch.exc.noise_level   = 0;
        s->groups[0].patch.exc.pluck         = 0;
        s->groups[0].patch.in_lpf_cutoff     = 1023;
        s->groups[0].patch.in_lpf_res        = 0;
        s->groups[0].patch.dry_wet           = 0;
        s->groups[0].patch.lfo_depth         = 0;
        s->groups[0].patch.lfo2_depth        = 0;
        s->groups[0].patch.lfo3_depth        = 0;
        s->groups[0].patch.env1.onset_delay  = 0;
        s->groups[0].patch.env1.attack       = 0;
        s->groups[0].patch.env1.sustain      = 1023;
        s->groups[0].patch.env2.attack       = 0;
        s->groups[0].patch.env2.sustain      = 1023;
        s->groups[0].patch.master_vol        = (pass == 0) ? 1023 : 8;
        adrift_synth_set_master_pot(s, (pass == 0) ? 1023 : 32, true);
        adrift_app_audio_block_q23((pass == 0) ? q23_ref : q23_low, nframes, &note_on, 1u);
    }

    double num_pow = 0.0, den_pow = 0.0;
    for (uint32_t i = 256u; i < nframes; i++) {
        double r = (double)q23_ref[2u * i];
        double y = (double)q23_low[2u * i];
        num_pow += r * y;
        den_pow += r * r;
    }
    double k_scale = (den_pow > 0.0) ? (num_pow / den_pow) : 0.0;
    double sig_pow = 0.0, err_q23_pow = 0.0, err_s16_pow = 0.0;
    uint32_t sub_lsb_cnt = 0u;
    for (uint32_t i = 256u; i < nframes; i++) {
        int32_t ql    = q23_low[2u * i];
        int32_t absql = (ql < 0) ? -ql : ql;
        if (absql > 0 && absql < 128) {
            sub_lsb_cnt++;
        }
        double ideal = k_scale * (double)q23_ref[2u * i];
        double s16_q = (double)(((ql + 128) >> 8) * 256);
        sig_pow     += ideal * ideal;
        err_q23_pow += ((double)ql - ideal) * ((double)ql - ideal);
        err_s16_pow += (s16_q - ideal) * (s16_q - ideal);
    }
    double snr_q23_db = 10.0 * log10(sig_pow / (err_q23_pow + 1e-12));
    double snr_s16_db = 10.0 * log10(sig_pow / (err_s16_pow + 1e-12));
    CHECK(sub_lsb_cnt > 100u && (snr_q23_db - snr_s16_db) > 35.0,
          "Expected Q23 path to preserve sub-16-bit waveform steps and gain >35 dB SNR over 16-bit (sub_lsb=%u, Q23=%.1f dB, S16=%.1f dB)",
          sub_lsb_cnt, snr_q23_db, snr_s16_db);

    /* 5c. Sub-16-bit (~-102 dBFS, |q23| < 96) TPDF dither: resolves signal without dead-zone,
     * preserves exact L==R on centered mono, and keeps muted hard-left Right channel at exact 0 */
    adrift_app_init();
    s = adrift_app_get_synth();
    memset(&s->fx_params, 0, sizeof(s->fx_params));
    adrift_synth_set_group_voices(s, 0u, 1u);
    s->groups[0].pan                    = 0;
    s->groups[0].stereo_spread          = 0;
    s->groups[0].patch.exc.wave_morph   = 0;
    s->groups[0].patch.exc.analog_drift = 0;
    s->groups[0].patch.exc.vco_level    = 900;
    s->groups[0].patch.exc.noise_level  = 0;
    s->groups[0].patch.exc.pluck        = 0;
    s->groups[0].patch.in_lpf_cutoff    = 1023;
    s->groups[0].patch.in_lpf_res       = 0;
    s->groups[0].patch.dry_wet          = 0;
    s->groups[0].patch.lfo_depth        = 0;
    s->groups[0].patch.lfo2_depth       = 0;
    s->groups[0].patch.lfo3_depth       = 0;
    s->groups[0].patch.env1.onset_delay = 0;
    s->groups[0].patch.env1.attack      = 0;
    s->groups[0].patch.env1.sustain     = 1023;
    s->groups[0].patch.env2.attack      = 0;
    s->groups[0].patch.env2.sustain     = 1023;
    s->groups[0].patch.master_vol       = 1;
    adrift_synth_set_master_pot(s, 8, true);
    adrift_app_audio_block_q23(q23_low, 64u, &note_on, 1u); /* Settle initial block-0 gain ramp */
    adrift_app_audio_block_q23(q23_low, nframes, 0, 0u);
    int32_t sub_q23_peak = 0;
    double  sub_q23_sum  = 0.0;
    for (uint32_t i = 0u; i < nframes; i++) {
        int32_t ql = q23_low[2u * i];
        int32_t aq = (ql < 0) ? -ql : ql;
        if (aq > sub_q23_peak) sub_q23_peak = aq;
        sub_q23_sum += (double)ql / 256.0;
    }

    memset(pcm_256, 0, nframes * 2u * sizeof(int16_t));
    adrift_app_audio_block(pcm_256, nframes, 0, 0u);
    uint32_t dither_toggles = 0u;
    uint32_t mono_mismatch  = 0u;
    int64_t  dither_sum     = 0;
    for (uint32_t i = 0u; i < nframes; i++) {
        int16_t l = pcm_256[2u * i];
        int16_t r = pcm_256[2u * i + 1u];
        if (l != 0) dither_toggles++;
        if (l != r) mono_mismatch++;
        dither_sum += l;
    }
    double dither_dc = fabs(((double)dither_sum - sub_q23_sum) / (double)nframes);
    CHECK(sub_q23_peak > 0 && sub_q23_peak < 120 && dither_toggles > 50u &&
          mono_mismatch == 0u && dither_dc < 0.05,
          "Expected sub-LSB TPDF dither to resolve signal with L==R mono coherence and zero DC bias (q23_pk=%d, toggles=%u, mismatch=%u, dc=%.4f)",
          (int)sub_q23_peak, dither_toggles, mono_mismatch, dither_dc);

    s->groups[0].pan = -512; /* Hard Left -> Right Q23 channel is exact 0 -> zero-gated dither keeps R == 0 */
    adrift_app_audio_block(pcm_256, 256u, 0, 0u); /* Settle pan ramp */
    adrift_app_audio_block(pcm_256, 512u, 0, 0u);
    int32_t hard_l_right_peak = 0;
    for (uint32_t i = 0u; i < 512u; i++) {
        int32_t r = (pcm_256[2u * i + 1u] < 0) ? -(int32_t)pcm_256[2u * i + 1u] : (int32_t)pcm_256[2u * i + 1u];
        if (r > hard_l_right_peak) hard_l_right_peak = r;
    }
    CHECK(hard_l_right_peak == 0,
          "Expected per-channel zero-gated TPDF dither to keep muted hard-panned R channel at exact 0 LSB (peak=%d)",
          (int)hard_l_right_peak);

    /* 5d. Q23 headphone speech mixer: bit-exact passthrough when idle, scaled speech when active */
    adrift_app_init();
    for (uint32_t i = 0u; i < 128u; i++) {
        q23_low[i] = (i & 1u) ? -37 : 37;
        q23_ref[i] = q23_low[i];
    }
    adrift_app_mix_line_out_q23(q23_low, 64u);
    CHECK(memcmp(q23_low, q23_ref, 128u * sizeof(int32_t)) == 0,
          "Expected idle adrift_app_mix_line_out_q23 to preserve sub-16-bit Q23 synth audio bit-for-bit");

    adrift_app_init();
    s  = adrift_app_get_synth();
    ui = adrift_app_get_ui();
    sr = adrift_app_get_screen_reader();
    adrift_synth_set_master_pot(s, 1023, true);
    adrift_sr_set_enabled(sr, true, true, ui, s);
    memset(q23_ref, 0, nframes * 2u * sizeof(int32_t));
    adrift_app_mix_line_out_q23(q23_ref, nframes);

    adrift_app_init();
    s  = adrift_app_get_synth();
    ui = adrift_app_get_ui();
    sr = adrift_app_get_screen_reader();
    adrift_synth_set_master_pot(s, 512, true);
    adrift_sr_set_enabled(sr, true, true, ui, s);
    memset(q23_low, 0, nframes * 2u * sizeof(int32_t));
    adrift_app_mix_line_out_q23(q23_low, nframes);

    double q23_sp_1023 = 0.0, q23_sp_512 = 0.0;
    for (uint32_t i = 0u; i < nframes * 2u; i++) {
        double a = (double)q23_ref[i];
        double b = (double)q23_low[i];
        q23_sp_1023 += a * a;
        q23_sp_512  += b * b;
    }
    double q23_sp_diff_db = 10.0 * log10((q23_sp_512 + 1e-12) / (q23_sp_1023 + 1e-12));
    CHECK(q23_sp_1023 > 1e12 && fabs(q23_sp_diff_db - (-6.012)) < 0.25,
          "Expected Q23 speech mix at pot=512 to be -6.0 dB vs pot=1023 (diff=%.2f dB)",
          q23_sp_diff_db);

    free(q23_0);
    free(q23_low);
    free(q23_ref);
    free(fb);
    free(pcm_0);
    free(pcm_256);
    free(pcm_512);
    free(pcm_1023);
    printf("  -> PASS (pot=0 Exact 0 LSB, 512=-6.0dB, Q23 SNR=+%.1f dB, Zero-Gated TPDF & Speech Scaling)\n",
           snr_q23_db - snr_s16_db);
}
