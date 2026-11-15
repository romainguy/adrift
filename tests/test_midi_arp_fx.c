/* SPDX-License-Identifier: MPL-2.0
 * MIDI Protocol, Arpeggiator, Master Stereo FX & External 24 PPQN Clock Sync Tests */

#include "test_common.h"

/* ---- 6. Full MIDI Protocol Reactivity ---- */
void test_midi_protocol(void) {
    printf("[Test 6] MIDI Protocol (Note, Vel, PB, MW, Sustain CC64, CCs, AllNotesOff)...\n");
    const uint32_t N = 16384u;
    int16_t *pcm = calloc(N * 2u, sizeof(int16_t));

    adrift_app_init();
    adrift_synth_t *s = adrift_app_get_synth();

    /* Velocity scaling test */
    adrift_midi_msg_t ev_soft = { 0x90u, 60u, 20u, 0u };
    adrift_headless_render(pcm, 4096u, &ev_soft, 1u);
    double rms_soft = compute_rms_dbfs(pcm, 0u, 4096u);

    adrift_app_init();
    adrift_midi_msg_t ev_loud = { 0x90u, 60u, 127u, 0u };
    adrift_headless_render(pcm, 4096u, &ev_loud, 1u);
    double rms_loud = compute_rms_dbfs(pcm, 0u, 4096u);
    CHECK((rms_loud - rms_soft) > 6.0,
          "Velocity did not scale output level (loud=%.1f vs soft=%.1f dBFS)", rms_loud, rms_soft);

    /* 14-bit Pitch Bend (+8191 -> +2 semitones from C4=60 to D4=62) */
    adrift_app_init();
    s->groups[0].patch.lfo_depth    = 0;
    s->groups[0].patch.bbd.feedback = 886;
    adrift_midi_msg_t pb_evs[2] = {
        { 0xE0u, 0x7Fu, 0x7Fu, 0u }, /* Full +2st bend */
        { 0x90u, 60u,   120u,  0u }
    };
    adrift_headless_render(pcm, N, pb_evs, 2u);
    double d4_hz   = 440.0 * pow(2.0, (62.0 - 69.0) / 12.0);
    double bend_hz = estimate_freq_autocorr(pcm, 4096u, 8192u, d4_hz * 0.9, d4_hz * 1.1);
    double bend_cents = 1200.0 * log2(bend_hz / d4_hz);
    CHECK(fabs(bend_cents) < 8.0, "Pitch Bend +2st inaccurate: %.2f cents", bend_cents);

    /* Sustain Pedal (CC64) holds gate after Note Off until pedal up */
    adrift_app_init();
    adrift_midi_msg_t sus_on[3] = {
        { 0xB0u, 64u, 127u, 0u },
        { 0x90u, 60u, 100u, 4u },
        { 0x80u, 60u, 0u,   32u }
    };
    adrift_headless_render(pcm, 2048u, sus_on, 3u);
    CHECK(s->voices[0].sustained && s->voices[0].env2.stage != ADRIFT_ENV_RELEASE,
          "CC64 Sustain Pedal did not hold voice after Note Off");

    adrift_midi_msg_t sus_off = { 0xB0u, 64u, 0u, 0u };
    adrift_headless_render(pcm, 256u, &sus_off, 1u);
    CHECK(!s->voices[0].sustained && s->voices[0].env2.stage == ADRIFT_ENV_RELEASE,
          "CC64 release did not transition voice into Release");

    /* Parameter CCs + All Sound Off (CC120) */
    adrift_midi_msg_t cc_evs[3] = {
        { 0xB0u, 1u,   100u, 0u },  /* Mod Wheel */
        { 0xB0u, 74u,  64u,  0u },  /* Input LPF cutoff */
        { 0xB0u, 120u, 0u,   16u }  /* All Sound Off */
    };
    adrift_headless_render(pcm, 256u, cc_evs, 3u);
    CHECK(s->groups[0].mod_wheel == 100u &&
          s->groups[0].patch.in_lpf_cutoff > 500 &&
          s->groups[0].patch.in_lpf_cutoff < 530,
          "MIDI CC1 / CC74 mapping failed");
    CHECK(!s->voices[0].active && s->voices[0].env2.stage == ADRIFT_ENV_IDLE,
          "CC120 All Sound Off did not immediately silence voice");

    free(pcm);
    printf("  -> PASS (Vel diff=+%.1f dB, PB +2st err=%.2f cents, CC64/CC74/CC120 OK)\n",
           rms_loud - rms_soft, bend_cents);
}

/* ============================================================================
 * 9. Arpeggiator Engine, Algorithms, Range/Rate/Gate/Vel/Latch/Swing/Ratchet & ARP UI
 * ============================================================================ */
static void advance_arp_steps(adrift_synth_t *s,
                              uint8_t g,
                              int16_t *pcm_scratch,
                              uint8_t *out_notes,
                              uint32_t num_steps) {
    const adrift_group_t *gp = &s->groups[g];
    for (uint32_t i = 0; i < num_steps; i++) {
        out_notes[i] = gp->arp_state.cur_step_note;
        uint32_t step_len = gp->arp_state.substep_len_samples;
        if (step_len == 0u) step_len = 5514u;
        uint32_t rem = step_len;
        while (rem > 0u) {
            uint32_t chunk = (rem > 256u) ? 256u : rem;
            adrift_synth_render_block(s, pcm_scratch, chunk, NULL, 0u);
            rem -= chunk;
        }
    }
}

void test_arpeggiator(void) {
    printf("[Test 9] Arpeggiator (Algorithms, Range, Rate, Gate, Vel, Latch, Swing, Ratchet, ARP Screen)...\n");

    adrift_app_init();
    adrift_synth_t *s  = adrift_app_get_synth();
    adrift_ui_t    *ui = adrift_app_get_ui();
    int16_t  *pcm = (int16_t *)calloc(8192u * 2u, sizeof(int16_t));
    uint16_t *fb  = (uint16_t *)calloc(ADRIFT_SCREEN_W * ADRIFT_SCREEN_H, sizeof(uint16_t));

    /* 9a. Verify UP, DOWN, UP/DN (excl), DN/UP (excl), UP&DN (incl), CONVRG, DIVERG, ORDER on 3 notes */
    uint8_t seq[12];
    const uint8_t chord3[3] = { 60u, 64u, 67u };

    /* UP: 60, 64, 67, 60, 64, 67 */
    adrift_synth_group_all_notes_off(s, 0u, true);
    adrift_synth_set_group_arp_mode(s, 0u, ADRIFT_ARP_UP);
    s->groups[0].patch.arp.range    = 1u;
    s->groups[0].patch.arp.rate_bpm = 120u;
    s->groups[0].patch.arp.gate     = 80u;
    s->groups[0].patch.arp.swing    = 0u;
    s->groups[0].patch.arp.ratchet  = ADRIFT_RATCHET_1X;
    for (int i = 0; i < 3; i++) adrift_synth_group_note_on(s, 0u, chord3[i], 100u);
    advance_arp_steps(s, 0u, pcm, seq, 6u);
    CHECK(seq[0] == 60u && seq[1] == 64u && seq[2] == 67u &&
          seq[3] == 60u && seq[4] == 64u && seq[5] == 67u,
          "ARP UP sequence mismatch: %u,%u,%u,%u,%u,%u",
          seq[0], seq[1], seq[2], seq[3], seq[4], seq[5]);

    /* DOWN: 67, 64, 60, 67, 64, 60 */
    adrift_synth_group_all_notes_off(s, 0u, true);
    adrift_synth_set_group_arp_mode(s, 0u, ADRIFT_ARP_DOWN);
    for (int i = 0; i < 3; i++) adrift_synth_group_note_on(s, 0u, chord3[i], 100u);
    advance_arp_steps(s, 0u, pcm, seq, 6u);
    CHECK(seq[0] == 67u && seq[1] == 64u && seq[2] == 60u &&
          seq[3] == 67u && seq[4] == 64u && seq[5] == 60u,
          "ARP DOWN sequence mismatch: %u,%u,%u,%u,%u,%u",
          seq[0], seq[1], seq[2], seq[3], seq[4], seq[5]);

    /* UP/DN (exclusive): 60, 64, 67, 64, 60, 64 */
    adrift_synth_group_all_notes_off(s, 0u, true);
    adrift_synth_set_group_arp_mode(s, 0u, ADRIFT_ARP_UP_DOWN);
    for (int i = 0; i < 3; i++) adrift_synth_group_note_on(s, 0u, chord3[i], 100u);
    advance_arp_steps(s, 0u, pcm, seq, 6u);
    CHECK(seq[0] == 60u && seq[1] == 64u && seq[2] == 67u &&
          seq[3] == 64u && seq[4] == 60u && seq[5] == 64u,
          "ARP UP/DN sequence mismatch: %u,%u,%u,%u,%u,%u",
          seq[0], seq[1], seq[2], seq[3], seq[4], seq[5]);

    /* DN/UP (exclusive): 67, 64, 60, 64, 67, 64 */
    adrift_synth_group_all_notes_off(s, 0u, true);
    adrift_synth_set_group_arp_mode(s, 0u, ADRIFT_ARP_DOWN_UP);
    for (int i = 0; i < 3; i++) adrift_synth_group_note_on(s, 0u, chord3[i], 100u);
    advance_arp_steps(s, 0u, pcm, seq, 6u);
    CHECK(seq[0] == 67u && seq[1] == 64u && seq[2] == 60u &&
          seq[3] == 64u && seq[4] == 67u && seq[5] == 64u,
          "ARP DN/UP sequence mismatch: %u,%u,%u,%u,%u,%u",
          seq[0], seq[1], seq[2], seq[3], seq[4], seq[5]);

    /* UP&DN (inclusive): 60, 64, 67, 67, 64, 60 */
    adrift_synth_group_all_notes_off(s, 0u, true);
    adrift_synth_set_group_arp_mode(s, 0u, ADRIFT_ARP_UP_AND_DOWN);
    for (int i = 0; i < 3; i++) adrift_synth_group_note_on(s, 0u, chord3[i], 100u);
    advance_arp_steps(s, 0u, pcm, seq, 6u);
    CHECK(seq[0] == 60u && seq[1] == 64u && seq[2] == 67u &&
          seq[3] == 67u && seq[4] == 64u && seq[5] == 60u,
          "ARP UP&DN sequence mismatch: %u,%u,%u,%u,%u,%u",
          seq[0], seq[1], seq[2], seq[3], seq[4], seq[5]);

    /* CONVRG on 4 notes (60, 64, 67, 71): 60, 71, 64, 67 */
    adrift_synth_group_all_notes_off(s, 0u, true);
    adrift_synth_set_group_arp_mode(s, 0u, ADRIFT_ARP_CONVERGE);
    adrift_synth_group_note_on(s, 0u, 60u, 100u);
    adrift_synth_group_note_on(s, 0u, 64u, 100u);
    adrift_synth_group_note_on(s, 0u, 67u, 100u);
    adrift_synth_group_note_on(s, 0u, 71u, 100u);
    advance_arp_steps(s, 0u, pcm, seq, 4u);
    CHECK(seq[0] == 60u && seq[1] == 71u && seq[2] == 64u && seq[3] == 67u,
          "ARP CONVRG sequence mismatch: %u,%u,%u,%u", seq[0], seq[1], seq[2], seq[3]);

    /* DIVERG on 4 notes (60, 64, 67, 71): 64, 67, 60, 71 */
    adrift_synth_group_all_notes_off(s, 0u, true);
    adrift_synth_set_group_arp_mode(s, 0u, ADRIFT_ARP_DIVERGE);
    adrift_synth_group_note_on(s, 0u, 60u, 100u);
    adrift_synth_group_note_on(s, 0u, 64u, 100u);
    adrift_synth_group_note_on(s, 0u, 67u, 100u);
    adrift_synth_group_note_on(s, 0u, 71u, 100u);
    advance_arp_steps(s, 0u, pcm, seq, 4u);
    CHECK(seq[0] == 64u && seq[1] == 67u && seq[2] == 60u && seq[3] == 71u,
          "ARP DIVERG sequence mismatch: %u,%u,%u,%u", seq[0], seq[1], seq[2], seq[3]);

    /* ORDER (as played: 67, 60, 64): 67, 60, 64, 67, 60, 64 */
    adrift_synth_group_all_notes_off(s, 0u, true);
    adrift_synth_set_group_arp_mode(s, 0u, ADRIFT_ARP_ORDER);
    adrift_synth_group_note_on(s, 0u, 67u, 100u);
    adrift_synth_group_note_on(s, 0u, 60u, 100u);
    adrift_synth_group_note_on(s, 0u, 64u, 100u);
    advance_arp_steps(s, 0u, pcm, seq, 6u);
    CHECK(seq[0] == 67u && seq[1] == 60u && seq[2] == 64u &&
          seq[3] == 67u && seq[4] == 60u && seq[5] == 64u,
          "ARP ORDER sequence mismatch: %u,%u,%u,%u,%u,%u",
          seq[0], seq[1], seq[2], seq[3], seq[4], seq[5]);

    /* CHORD mode: triggers all 3 chord voices simultaneously */
    adrift_synth_group_all_notes_off(s, 0u, true);
    adrift_synth_set_group_arp_mode(s, 0u, ADRIFT_ARP_CHORD);
    for (int i = 0; i < 3; i++) adrift_synth_group_note_on(s, 0u, chord3[i], 100u);
    CHECK(s->groups[0].arp_state.sounding_count == 3u,
          "ARP CHORD should trigger all 3 held notes simultaneously (got %u)",
          s->groups[0].arp_state.sounding_count);

    /* 9b. Octave Range = 3 octaves on 2 notes (60, 67) -> 60, 67, 72, 79, 84, 91 */
    adrift_synth_group_all_notes_off(s, 0u, true);
    adrift_synth_set_group_arp_mode(s, 0u, ADRIFT_ARP_UP);
    s->groups[0].patch.arp.range = 3u;
    adrift_synth_group_note_on(s, 0u, 60u, 100u);
    adrift_synth_group_note_on(s, 0u, 67u, 100u);
    advance_arp_steps(s, 0u, pcm, seq, 6u);
    CHECK(seq[0] == 60u && seq[1] == 67u &&
          seq[2] == 72u && seq[3] == 79u &&
          seq[4] == 84u && seq[5] == 91u,
          "ARP 3-octave RANGE expansion failed: %u,%u,%u,%u,%u,%u",
          seq[0], seq[1], seq[2], seq[3], seq[4], seq[5]);
    s->groups[0].patch.arp.range = 1u;

    /* 9c. Gate & Velocity (KEY vs Fixed) */
    adrift_synth_group_all_notes_off(s, 0u, true);
    adrift_synth_set_group_arp_mode(s, 0u, ADRIFT_ARP_UP);
    s->groups[0].patch.arp.rate_bpm = 120u; /* step = 661770 / 120 = 5514 samples */
    s->groups[0].patch.arp.gate     = 50u;  /* gate = 2757 samples */
    s->groups[0].patch.arp.velocity = 0u;   /* KEY velocity */
    adrift_synth_group_note_on(s, 0u, 60u, 42u);
    CHECK(s->groups[0].arp_state.cur_step_vel == 42u, "Expected KEY velocity 42, got %u", s->groups[0].arp_state.cur_step_vel);
    CHECK(s->groups[0].arp_state.gate_open, "Expected gate to be open at step start");
    /* Advance 2800 samples (> 50% gate = 2757 samples, < 5514 step) -> gate must close */
    for (uint32_t rem = 2800u; rem > 0u; ) {
        uint32_t c = (rem > 256u) ? 256u : rem;
        adrift_synth_render_block(s, pcm, c, NULL, 0u);
        rem -= c;
    }
    CHECK(!s->groups[0].arp_state.gate_open, "Expected gate to close after 50%% of step");

    /* Fixed Velocity = 115 */
    s->groups[0].patch.arp.velocity = 115u;
    for (uint32_t rem = 2800u; rem > 0u; ) {
        uint32_t c = (rem > 256u) ? 256u : rem;
        adrift_synth_render_block(s, pcm, c, NULL, 0u);
        rem -= c;
    }
    CHECK(s->groups[0].arp_state.gate_open && s->groups[0].arp_state.cur_step_vel == 115u,
          "Expected fixed velocity 115 on next step, got %u", s->groups[0].arp_state.cur_step_vel);
    s->groups[0].patch.arp.velocity = 0u;
    s->groups[0].patch.arp.gate     = 80u;

    /* 9d. Latch: hold after Note-Off, replace chord on next Note-On when phys_count == 0, stop on Latch Off */
    adrift_synth_group_all_notes_off(s, 0u, true);
    adrift_synth_set_group_arp_mode(s, 0u, ADRIFT_ARP_UP);
    s->groups[0].patch.arp.latch = true;
    adrift_synth_group_note_on(s, 0u, 60u, 100u);
    adrift_synth_group_note_on(s, 0u, 64u, 100u);
    adrift_synth_group_note_off(s, 0u, 60u);
    adrift_synth_group_note_off(s, 0u, 64u);
    CHECK(s->groups[0].arp_state.phys_count == 0u && s->groups[0].arp_state.num_notes == 2u,
          "Latch should retain 2 notes after physical keys are released");
    /* Pressing a new note when phys_count == 0 replaces the latched chord */
    adrift_synth_group_note_on(s, 0u, 72u, 100u);
    CHECK(s->groups[0].arp_state.num_notes == 1u && s->groups[0].arp_state.notes[0] == 72u,
          "Pressing new key while latched (phys_count=0) should replace chord with 72");
    adrift_synth_group_note_off(s, 0u, 72u);
    CHECK(s->groups[0].arp_state.num_notes == 1u, "Latched note 72 should remain after release");
    /* Turning Latch OFF with no physical keys held stops the arpeggio */
    s->groups[0].patch.arp.latch = false;
    adrift_synth_sync_group_arp(s, 0u);
    CHECK(s->groups[0].arp_state.num_notes == 0u && !s->groups[0].arp_state.gate_open,
          "Disabling Latch with no keys held should stop arpeggio");

    /* 9e. Swing: at 50% swing and 120 BPM (base = 5514), even step = 6892, odd step = 4136, sum = 11028 = 2*base */
    adrift_synth_group_all_notes_off(s, 0u, true);
    adrift_synth_set_group_arp_mode(s, 0u, ADRIFT_ARP_UP);
    s->groups[0].patch.arp.rate_bpm = 120u;
    s->groups[0].patch.arp.swing    = 50u;
    s->groups[0].patch.arp.ratchet  = ADRIFT_RATCHET_1X;
    adrift_synth_group_note_on(s, 0u, 60u, 100u);
    adrift_synth_group_note_on(s, 0u, 67u, 100u);
    uint32_t even_step_len = s->groups[0].arp_state.substep_len_samples;
    advance_arp_steps(s, 0u, pcm, seq, 1u);
    uint32_t odd_step_len  = s->groups[0].arp_state.substep_len_samples;
    CHECK(even_step_len > odd_step_len && (even_step_len + odd_step_len) == 5514u * 2u,
          "Swing 50%% step lengths invalid: even=%u, odd=%u", even_step_len, odd_step_len);
    s->groups[0].patch.arp.swing = 0u;

    /* 9f. Ratchet: 3X subdivides 1 step (5514 samples) into 3 sub-steps of 1838 samples on the same note before advancing */
    adrift_synth_group_all_notes_off(s, 0u, true);
    adrift_synth_set_group_arp_mode(s, 0u, ADRIFT_ARP_UP);
    s->groups[0].patch.arp.ratchet = ADRIFT_RATCHET_3X;
    adrift_synth_group_note_on(s, 0u, 60u, 100u);
    adrift_synth_group_note_on(s, 0u, 67u, 100u);
    CHECK(s->groups[0].arp_state.substep_total == 3u &&
          s->groups[0].arp_state.substep_len_samples == 5514u / 3u,
          "Expected 3X ratchet substep_total=3, len=%u", s->groups[0].arp_state.substep_len_samples);
    /* Advance 3 ratcheted substeps (all should play note 60), 4th substep advances to note 67 */
    advance_arp_steps(s, 0u, pcm, seq, 4u);
    CHECK(seq[0] == 60u && seq[1] == 60u && seq[2] == 60u && seq[3] == 67u,
          "3X Ratchet sub-step sequence mismatch: %u,%u,%u,%u", seq[0], seq[1], seq[2], seq[3]);
    s->groups[0].patch.arp.ratchet = ADRIFT_RATCHET_1X;

    /* 9g. ARP Hardware Button UI Screen (Pages 1/2 & 2/2) + MIDI CC102..109 */
    adrift_input_state_t in_st;
    adrift_dirty_t dirty;
    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed = ADRIFT_BTN_ARP; /* Open ARP Page 1/2 (ALGO, RANGE, RATE, GATE) */
    in_st.master_pot      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->in_arp_menu && !ui->in_group_menu && !ui->in_scope && ui->subpage == 0u,
          "ARP button did not open ARP Page 1/2");

    /* Edit Page 1/2 via KNOB1..KNOB4: ALGO -> DOWN, RANGE -> 2 OCT, RATE -> 140 BPM, GATE -> 65% */
    memset(&in_st, 0, sizeof(in_st));
    in_st.enc_delta[ADRIFT_ENC_KNOB1] = 1;   /* UP (1) -> DOWN (2) */
    in_st.enc_delta[ADRIFT_ENC_KNOB2] = 1;   /* RANGE: 1 -> 2 */
    in_st.enc_delta[ADRIFT_ENC_KNOB3] = 10;  /* RATE: 120 + 20 = 140 BPM */
    in_st.enc_delta[ADRIFT_ENC_KNOB4] = -3;  /* GATE: 80 - 15 = 65% */
    in_st.master_pot                  = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(s->groups[0].patch.arp.mode == ADRIFT_ARP_DOWN &&
          s->groups[0].patch.arp.range == 2u &&
          s->groups[0].patch.arp.rate_bpm == 140u &&
          s->groups[0].patch.arp.gate == 65u,
          "ARP Page 1/2 encoder edits failed (mode=%u, range=%u, bpm=%u, gate=%u)",
          s->groups[0].patch.arp.mode, s->groups[0].patch.arp.range,
          s->groups[0].patch.arp.rate_bpm, s->groups[0].patch.arp.gate);

    /* Press ARP again to cycle to ARP Page 2/2 (VEL, LATCH, SWING, RATCH) and edit all 4 knobs */
    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed             = ADRIFT_BTN_ARP;
    in_st.enc_delta[ADRIFT_ENC_KNOB1] = 50; /* VEL: 0 + 100 = 100 */
    in_st.enc_delta[ADRIFT_ENC_KNOB2] = 1;  /* LATCH: ON */
    in_st.enc_delta[ADRIFT_ENC_KNOB3] = 15; /* SWING: 0 + 30 = 30% */
    in_st.enc_delta[ADRIFT_ENC_KNOB4] = 2;  /* RATCH: 1X (0) -> 3X (2) */
    in_st.master_pot                  = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->in_arp_menu && ui->subpage == 1u, "Second ARP press did not advance to ARP Page 2/2");
    CHECK(s->groups[0].patch.arp.velocity == 100u &&
          s->groups[0].patch.arp.latch == true &&
          s->groups[0].patch.arp.swing == 30u &&
          s->groups[0].patch.arp.ratchet == ADRIFT_RATCHET_3X,
          "ARP Page 2/2 encoder edits failed (vel=%u, latch=%d, swing=%u, ratch=%u)",
          s->groups[0].patch.arp.velocity, s->groups[0].patch.arp.latch,
          s->groups[0].patch.arp.swing, s->groups[0].patch.arp.ratchet);

    /* Verify the 4th card on ARP Page 2/2 (RATCH) is populated with a header band (unlike 7-param screens) */
    uint16_t arp_card4_hdr = fb[145u * ADRIFT_SCREEN_W + 190u];
    uint16_t arp_card4_int = fb[185u * ADRIFT_SCREEN_W + 190u];
    CHECK(arp_card4_hdr != arp_card4_int,
          "Expected 4th card (RATCH) on ARP Page 2/2 to have a header band");

    /* Verify MIDI CC102 (Mode) and CC107 (Latch) */
    adrift_synth_handle_midi(s, 0xB0u, 102u, (uint8_t)ADRIFT_ARP_OFF);
    adrift_synth_handle_midi(s, 0xB0u, 107u, 0u);
    CHECK(s->groups[0].patch.arp.mode == ADRIFT_ARP_OFF && !s->groups[0].patch.arp.latch,
          "MIDI CC102/107 failed to reset arp mode and latch");

    free(fb);
    free(pcm);
    printf("  -> PASS (All 10 Arp Modes, 1..4 Oct Range, Gate, Vel, Latch, Swing, Ratchet & ARP UI OK)\n");
}

/* ============================================================================
 * 10. Master Stereo Bus Effects (Chorus, Ping-Pong Delay, Shimmer Reverb) & FX UI
 * ============================================================================ */
void test_master_fx(void) {
    printf("[Test 10] Master Stereo FX (Dimension Chorus, Ping-Pong Delay, Shimmer Reverb & FX UI)...\n");
    adrift_app_init();
    adrift_synth_t *s  = adrift_app_get_synth();
    adrift_ui_t    *ui = adrift_app_get_ui();

    const uint32_t N = 16384u;
    int16_t  *pcm = (int16_t *)calloc(N * 2u, sizeof(int16_t));
    uint16_t *fb  = (uint16_t *)calloc(ADRIFT_SCREEN_W * ADRIFT_SCREEN_H, sizeof(uint16_t));

    /* 10a. Stage 1: Stereo BBD Dimension Chorus creates wide stereo image from mono-centered voice */
    s->groups[0].stereo_spread = 0;
    s->fx_params.chorus_mix    = 750;
    s->fx_params.delay_mix     = 0;
    s->fx_params.reverb_mix    = 0;
    adrift_synth_group_note_on(s, 0u, 60u, 110u);
    adrift_synth_render_block(s, pcm, 4096u, NULL, 0u);

    bool chorus_stereo_diff = false;
    for (uint32_t i = 512u; i < 4096u; i++) {
        if (pcm[2u * i] != pcm[2u * i + 1u]) {
            chorus_stereo_diff = true;
            break;
        }
    }
    CHECK(chorus_stereo_diff && s->fx.meter_chorus_lfo_q15 != 0,
          "Expected Stereo Dimension Chorus to widen mono voice (diff=%d, lfo=%d)",
          chorus_stereo_diff, s->fx.meter_chorus_lfo_q15);

    /* Polyphonic Chorus headroom check: a 4-voice bus signal (+/-96000) must pass through
     * the Chorus delay line linearly without hard-clipping at int16_t (+/-32767). */
    {
        const uint32_t CN = 2048u;
        adrift_fx_state_t *fx_1x = (adrift_fx_state_t *)calloc(1u, sizeof(adrift_fx_state_t));
        adrift_fx_state_t *fx_4x = (adrift_fx_state_t *)calloc(1u, sizeof(adrift_fx_state_t));
        int32_t *al_1x = (int32_t *)calloc(CN, sizeof(int32_t));
        int32_t *ar_1x = (int32_t *)calloc(CN, sizeof(int32_t));
        int32_t *al_4x = (int32_t *)calloc(CN, sizeof(int32_t));
        int32_t *ar_4x = (int32_t *)calloc(CN, sizeof(int32_t));
        adrift_fx_init(fx_1x);
        adrift_fx_init(fx_4x);
        adrift_fx_params_t cp;
        memset(&cp, 0, sizeof(cp));
        cp.chorus_mix = 800;

        for (uint32_t i = 0u; i < CN; i++) {
            double s_val = sin(2.0 * M_PI * 440.0 * (double)i / (double)ADRIFT_SAMPLE_RATE);
            int32_t v1 = (int32_t)llround(24000.0 * s_val);
            al_1x[i] = v1;
            ar_1x[i] = v1;
            al_4x[i] = v1 * 4;
            ar_4x[i] = v1 * 4;
        }
        adrift_fx_process(fx_1x, &cp, al_1x, ar_1x, CN);
        adrift_fx_process(fx_4x, &cp, al_4x, ar_4x, CN);

        double err_sq = 0.0;
        double ref_sq = 0.0;
        for (uint32_t i = 768u; i < CN; i++) {
            double s_val = sin(2.0 * M_PI * 440.0 * (double)i / (double)ADRIFT_SAMPLE_RATE);
            int32_t v1 = (int32_t)llround(24000.0 * s_val);
            double wet_1x = (double)(al_1x[i] - v1);
            double wet_4x = (double)(al_4x[i] - v1 * 4);
            double diff   = wet_4x - 4.0 * wet_1x;
            err_sq += diff * diff;
            ref_sq += (4.0 * wet_1x) * (4.0 * wet_1x);
        }
        double nrmse = sqrt(err_sq / (ref_sq + 1.0));
        CHECK(nrmse < 0.01,
              "Chorus clipped polyphonic 4-voice bus signal (NRMSE=%.4f, expected < 0.01)", nrmse);

        free(ar_4x);
        free(al_4x);
        free(ar_1x);
        free(al_1x);
        free(fx_4x);
        free(fx_1x);

        /* Chorus Doppler pitch-excursion bound: at max depth (1023), wet tap pitch shift on a 1 kHz
         * tone must stay in the lush Dimension ensemble range (6..28 cents), avoiding seasick +-90c warble. */
        const uint32_t PN = 65536u;
        adrift_fx_state_t *fx_p = (adrift_fx_state_t *)calloc(1u, sizeof(adrift_fx_state_t));
        int32_t *pl = (int32_t *)calloc(PN, sizeof(int32_t));
        int32_t *pr = (int32_t *)calloc(PN, sizeof(int32_t));
        adrift_fx_init(fx_p);
        cp.chorus_mix = 1023;
        for (uint32_t i = 0u; i < PN; i++) {
            int32_t v = (int32_t)llround(20000.0 * sin(2.0 * M_PI * 1000.0 * (double)i / FS_EXACT));
            pl[i] = v;
            pr[i] = 0; /* Drive L only so wet_l = chorus_lp_l is pure single-channel modulated tap */
        }
        adrift_fx_process(fx_p, &cp, pl, pr, PN);
        double max_cents_dev = 0.0;
        for (uint32_t win0 = 2048u; win0 + 2048u <= PN; win0 += 1024u) {
            /* Measure wet tap instantaneous frequency via interpolated positive zero crossings */
            double first_zc = -1.0, last_zc = -1.0;
            int zc_cnt = 0;
            double prev_w = (double)pl[win0] - 20000.0 * sin(2.0 * M_PI * 1000.0 * (double)win0 / FS_EXACT);
            for (uint32_t k = 1u; k < 2048u; k++) {
                uint32_t idx = win0 + k;
                double cur_w = (double)pl[idx] - 20000.0 * sin(2.0 * M_PI * 1000.0 * (double)idx / FS_EXACT);
                if (prev_w <= 0.0 && cur_w > 0.0) {
                    double frac = -prev_w / (cur_w - prev_w + 1e-12);
                    double pos  = (double)(idx - 1u) + frac;
                    if (first_zc < 0.0) first_zc = pos;
                    last_zc = pos;
                    zc_cnt++;
                }
                prev_w = cur_w;
            }
            if (zc_cnt > 10) {
                double f_inst = FS_EXACT * (double)(zc_cnt - 1) / (last_zc - first_zc);
                double cents  = fabs(1200.0 * log2(f_inst / 1000.0));
                if (cents > max_cents_dev) max_cents_dev = cents;
            }
        }
        CHECK(max_cents_dev > 6.0 && max_cents_dev < 28.0,
              "Chorus max pitch excursion out of lush ensemble range (got %.2f cents, expected 6..28 cents)",
              max_cents_dev);
        free(pr);
        free(pl);
        free(fx_p);
    }

    /* 10b. Stage 2: Stereo Ping-Pong Tape Delay sustains echoes after dry voice stops */
    adrift_synth_all_notes_off(s, true);
    /* Short tight dry pluck so dry signal finishes quickly */
    s->groups[0].patch.dry_wet      = 0;
    s->groups[0].patch.env2.attack  = 0;
    s->groups[0].patch.env2.decay   = 80;
    s->groups[0].patch.env2.sustain = 0;
    s->groups[0].patch.env2.release = 40;
    s->fx_params.chorus_mix         = 0;
    s->fx_params.delay_time         = 220; /* ~95 ms */
    s->fx_params.delay_fb           = 780;
    s->fx_params.delay_mix          = 900;
    s->fx_params.reverb_mix         = 0;

    adrift_synth_group_note_on(s, 0u, 64u, 120u);
    adrift_synth_render_block(s, pcm, 2048u, NULL, 0u);
    adrift_synth_group_note_off(s, 0u, 64u);
    adrift_synth_render_block(s, pcm, 12288u, NULL, 0u);

    double dly_tail_rms = compute_rms_dbfs(pcm, 4096u, 8192u);
    CHECK(dly_tail_rms > -48.0 && s->fx.meter_delay_env_q15 > 0,
          "Expected Ping-Pong Tape Delay echo tail > -48 dBFS (got %.2f dBFS, env=%d)",
          dly_tail_rms, s->fx.meter_delay_env_q15);

    /* 10c. Stage 3: Stereo Diffusion Reverb + Octave Shimmer Tank & Panic tail flush */
    adrift_synth_all_notes_off(s, true);
    s->groups[0].patch.env1.sustain = 900;
    s->groups[0].patch.env2.sustain = 900;
    s->fx_params.delay_mix          = 0;
    s->fx_params.reverb_size        = 900;
    s->fx_params.reverb_damp        = 240;
    s->fx_params.reverb_shimmer     = 800;
    s->fx_params.reverb_mix         = 900;

    adrift_synth_group_note_on(s, 0u, 60u, 120u);
    adrift_synth_render_block(s, pcm, 2048u, NULL, 0u);
    adrift_synth_group_note_off(s, 0u, 60u);
    adrift_synth_render_block(s, pcm, 12288u, NULL, 0u);

    double rvb_tail_rms = compute_rms_dbfs(pcm, 4096u, 8192u);
    bool rvb_stereo_diff = false;
    for (uint32_t i = 4096u; i < 8192u; i++) {
        if (pcm[2u * i] != pcm[2u * i + 1u]) {
            rvb_stereo_diff = true;
            break;
        }
    }
    CHECK(rvb_tail_rms > -46.0 && rvb_stereo_diff && s->fx.meter_reverb_env_q15 > 0,
          "Expected Shimmer Diffusion Reverb stereo tail > -46 dBFS (got %.2f dBFS, diff=%d)",
          rvb_tail_rms, rvb_stereo_diff);

    /* Modulated Output Allpass smoothness check: feeding a Hann-windowed 220 Hz tone burst
     * must produce a smooth reverb tail free of integer delay-jump second-difference spikes. */
    {
        const uint32_t RN = 8192u;
        adrift_fx_state_t *fx_sm = (adrift_fx_state_t *)calloc(1u, sizeof(adrift_fx_state_t));
        int32_t *rl = (int32_t *)calloc(RN, sizeof(int32_t));
        int32_t *rr = (int32_t *)calloc(RN, sizeof(int32_t));
        adrift_fx_init(fx_sm);
        adrift_fx_params_t rp;
        memset(&rp, 0, sizeof(rp));
        rp.reverb_size = 900;
        rp.reverb_damp = 600;
        rp.reverb_mix  = 1023;

        for (uint32_t i = 0u; i < 4096u; i++) {
            double win = 0.5 - 0.5 * cos(2.0 * M_PI * (double)i / 4096.0);
            int32_t v  = (int32_t)llround(24000.0 * win *
                                          sin(2.0 * M_PI * 220.0 * (double)i / (double)ADRIFT_SAMPLE_RATE));
            rl[i] = v;
            rr[i] = v;
        }
        adrift_fx_process(fx_sm, &rp, rl, rr, RN);

        int32_t max_tail = 0;
        int32_t max_d2   = 0;
        for (uint32_t i = 4608u; i < RN; i++) {
            int32_t a = rl[i] < 0 ? -rl[i] : rl[i];
            if (a > max_tail) max_tail = a;
            int32_t d2 = rl[i] - 2 * rl[i - 1u] + rl[i - 2u];
            if (d2 < 0) d2 = -d2;
            if (d2 > max_d2) max_d2 = d2;
        }
        double d2_ratio = (double)max_d2 / (double)(max_tail > 0 ? max_tail : 1);
        CHECK(max_tail > 500 && d2_ratio < 0.015,
              "Reverb modulated allpass produced discontinuity ticks (max_d2=%d, tail=%d, ratio=%.4f)",
              max_d2, max_tail, d2_ratio);

        free(rr);
        free(rl);
        free(fx_sm);
    }

    /* Octave Shimmer +12 ST pitch & 2048-sample equal-power window check:
     * Feeding a pure 440 Hz tone must excite a clean 880 Hz (+12 ST) octave peak in the tank
     * dominating 1320 Hz (+19 ST) by > 20 dB without write-head collision buzz. */
    {
        const uint32_t SN = 16384u;
        adrift_fx_state_t *fx_sh = (adrift_fx_state_t *)calloc(1u, sizeof(adrift_fx_state_t));
        int32_t *sl = (int32_t *)calloc(SN, sizeof(int32_t));
        int32_t *sr = (int32_t *)calloc(SN, sizeof(int32_t));
        int16_t *spcm = (int16_t *)calloc(SN * 2u, sizeof(int16_t));
        adrift_fx_init(fx_sh);
        adrift_fx_params_t sp;
        memset(&sp, 0, sizeof(sp));
        sp.reverb_size    = 750;
        sp.reverb_damp    = 200;
        sp.reverb_shimmer = 1023;
        sp.reverb_mix     = 1023;

        for (uint32_t i = 0u; i < SN; i++) {
            int32_t v = (int32_t)llround(20000.0 * sin(2.0 * M_PI * 440.0 * (double)i / FS_EXACT));
            sl[i] = v;
            sr[i] = v;
        }
        adrift_fx_process(fx_sh, &sp, sl, sr, SN);
        for (uint32_t i = 0u; i < SN; i++) {
            int32_t v = (int32_t)llround(20000.0 * sin(2.0 * M_PI * 440.0 * (double)i / FS_EXACT));
            spcm[2u * i]      = dsp_sat16(sl[i] - v); /* Pure reverb+shimmer wet signal */
            spcm[2u * i + 1u] = dsp_sat16(sr[i] - v);
        }
        double oct_880_db  = goertzel_mag_db(spcm, 8192u, 8192u, 880.0);
        double fifth_1320_db = goertzel_mag_db(spcm, 8192u, 8192u, 1320.0);
        CHECK(oct_880_db > -36.0 && (oct_880_db - fifth_1320_db) > 20.0,
              "Shimmer failed +12 ST octave purity check (880 Hz=%.1f dB, 1320 Hz=%.1f dB)",
              oct_880_db, fifth_1320_db);

        free(spcm);
        free(sr);
        free(sl);
        free(fx_sh);
    }

    /* Anti-Larsen stability check: even at 100% Size + 100% Shimmer + 0% Damping,
     * the reverb tail must decay monotonically without runaway feedback oscillation. */
    s->fx_params.reverb_size    = 1023;
    s->fx_params.reverb_damp    = 0;
    s->fx_params.reverb_shimmer = 1023;
    s->fx_params.reverb_mix     = 1023;
    adrift_synth_group_note_on(s, 0u, 64u, 127u);
    adrift_synth_render_block(s, pcm, 4096u, NULL, 0u);
    adrift_synth_group_note_off(s, 0u, 64u);
    adrift_synth_render_block(s, pcm, 8192u, NULL, 0u);
    double early_tail_rms = compute_rms_dbfs(pcm, 2048u, 6144u);
    for (uint32_t b = 0u; b < 6u; b++) {
        adrift_synth_render_block(s, pcm, 12288u, NULL, 0u);
    }
    double late_tail_rms = compute_rms_dbfs(pcm, 0u, 12288u);
    CHECK(late_tail_rms < early_tail_rms - 12.0,
          "Reverb/Shimmer failed anti-Larsen decay check (early=%.2f dBFS, late=%.2f dBFS)",
          early_tail_rms, late_tail_rms);

    /* Panic / All Sound Off (clear_bbd = true) must flush FX delay & reverb buffers to silence */
    adrift_synth_all_notes_off(s, true);
    adrift_synth_render_block(s, pcm, 1024u, NULL, 0u);
    double flushed_rms = compute_rms_dbfs(pcm, 0u, 1024u);
    CHECK(flushed_rms < -110.0, "Expected Panic to flush FX tail to silence (got %.2f dBFS)", flushed_rms);

    /* 10d. Stereo Bus ("Glue" / Parallel) Soft-Knee Compressor: bypass, dynamic glue, transient attack & GR */
    adrift_synth_load_preset(s, 0u);
    s->fx_params.chorus_mix  = 0;
    s->fx_params.delay_mix   = 0;
    s->fx_params.reverb_mix  = 0;
    s->fx_params.comp_mix    = 0;
    s->fx_params.comp_thresh = 128;
    s->fx_params.comp_attack = 0;
    s->fx_params.comp_makeup = 256;

    /* Measure dry dynamic spread between soft solo note (vel=24) and loud 3-voice chord (vel=127) */
    adrift_synth_group_note_on(s, 0u, 60u, 24u);
    adrift_synth_render_block(s, pcm, 4096u, NULL, 0u);
    double soft_dry_rms = compute_rms_dbfs(pcm, 512u, 3584u);
    adrift_synth_all_notes_off(s, true);

    adrift_synth_load_preset(s, 0u);
    s->fx_params.chorus_mix = 0;
    s->fx_params.delay_mix  = 0;
    s->fx_params.reverb_mix = 0;
    s->fx_params.comp_mix   = 0;
    adrift_synth_group_note_on(s, 0u, 60u, 127u);
    adrift_synth_group_note_on(s, 0u, 64u, 127u);
    adrift_synth_group_note_on(s, 0u, 67u, 127u);
    adrift_synth_render_block(s, pcm, 4096u, NULL, 0u);
    double loud_dry_rms = compute_rms_dbfs(pcm, 512u, 3584u);
    adrift_synth_all_notes_off(s, true);

    /* Measure compressed dynamic spread at 100% COMP, -21 dB THRESH, 1 ms ATTACK, +3 dB MAKEUP */
    adrift_synth_load_preset(s, 0u);
    s->fx_params.chorus_mix  = 0;
    s->fx_params.delay_mix   = 0;
    s->fx_params.reverb_mix  = 0;
    s->fx_params.comp_mix    = 1023;
    s->fx_params.comp_thresh = 140;
    s->fx_params.comp_attack = 0;
    s->fx_params.comp_makeup = 256;
    adrift_synth_group_note_on(s, 0u, 60u, 24u);
    adrift_synth_render_block(s, pcm, 4096u, NULL, 0u);
    double soft_cmp_rms = compute_rms_dbfs(pcm, 512u, 3584u);
    adrift_synth_all_notes_off(s, true);

    adrift_synth_load_preset(s, 0u);
    s->fx_params.chorus_mix  = 0;
    s->fx_params.delay_mix   = 0;
    s->fx_params.reverb_mix  = 0;
    s->fx_params.comp_mix    = 1023;
    s->fx_params.comp_thresh = 140;
    s->fx_params.comp_attack = 0;
    s->fx_params.comp_makeup = 256;
    adrift_synth_group_note_on(s, 0u, 60u, 127u);
    adrift_synth_group_note_on(s, 0u, 64u, 127u);
    adrift_synth_group_note_on(s, 0u, 67u, 127u);
    adrift_synth_render_block(s, pcm, 4096u, NULL, 0u);
    double loud_cmp_rms = compute_rms_dbfs(pcm, 512u, 3584u);
    int32_t active_gr   = s->fx.meter_comp_gr_q15;

    double dry_spread_db = loud_dry_rms - soft_dry_rms;
    double cmp_spread_db = loud_cmp_rms - soft_cmp_rms;
    CHECK(active_gr > 4000, "Expected Bus Compressor GR meter > 4000 on loud chord (got %d)", active_gr);
    CHECK(soft_cmp_rms > soft_dry_rms + 1.0,
          "Expected Bus Compressor makeup to lift quiet solo note (dry=%.2f, cmp=%.2f dBFS)",
          soft_dry_rms, soft_cmp_rms);
    CHECK(cmp_spread_db < dry_spread_db - 4.0,
          "Expected Bus Compressor to reduce loud/soft dynamic spread by >4 dB (dry=%.2f dB, cmp=%.2f dB)",
          dry_spread_db, cmp_spread_db);

    /* Slow 50 ms attack must let initial pluck transient peak higher than fast 1 ms attack */
    adrift_synth_load_preset(s, 0u);
    s->groups[0].patch.env1.attack = 0;
    s->groups[0].patch.env2.attack = 0;
    s->fx_params.chorus_mix  = 0;
    s->fx_params.delay_mix   = 0;
    s->fx_params.reverb_mix  = 0;
    s->fx_params.comp_mix    = 1023;
    s->fx_params.comp_thresh = 80;
    s->fx_params.comp_makeup = 0;
    s->fx_params.comp_attack = 0; /* 1 ms */
    adrift_synth_group_note_on(s, 0u, 60u, 127u);
    adrift_synth_group_note_on(s, 0u, 67u, 127u);
    adrift_synth_render_block(s, pcm, 1024u, NULL, 0u);
    int32_t pk_fast = 0;
    for (uint32_t i = 64u; i < 640u * 2u; i++) {
        int32_t a = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (a > pk_fast) pk_fast = a;
    }

    adrift_synth_load_preset(s, 0u);
    s->groups[0].patch.env1.attack = 0;
    s->groups[0].patch.env2.attack = 0;
    s->fx_params.chorus_mix  = 0;
    s->fx_params.delay_mix   = 0;
    s->fx_params.reverb_mix  = 0;
    s->fx_params.comp_mix    = 1023;
    s->fx_params.comp_thresh = 80;
    s->fx_params.comp_makeup = 0;
    s->fx_params.comp_attack = 1023; /* 50 ms */
    adrift_synth_group_note_on(s, 0u, 60u, 127u);
    adrift_synth_group_note_on(s, 0u, 67u, 127u);
    adrift_synth_render_block(s, pcm, 1024u, NULL, 0u);
    int32_t pk_slow = 0;
    for (uint32_t i = 64u; i < 640u * 2u; i++) {
        int32_t a = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (a > pk_slow) pk_slow = a;
    }
    CHECK(pk_slow > pk_fast + 800,
          "Expected 50 ms punchy attack to preserve higher initial transient than 1 ms attack (slow=%d, fast=%d)",
          pk_slow, pk_fast);

    adrift_synth_all_notes_off(s, true);
    adrift_synth_render_block(s, pcm, 1024u, NULL, 0u);
    CHECK(compute_rms_dbfs(pcm, 0u, 1024u) < -110.0,
          "Expected silent output with active Bus Compressor after Panic");

    /* 10e. FX Hardware Button UI Screen (FX 1/3, FX 2/3 & FX 3/3) + MIDI CCs */
    adrift_synth_load_preset(s, 0u);
    adrift_input_state_t in_st;
    adrift_dirty_t dirty;
    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed = ADRIFT_BTN_FX; /* Open FX Page 1/3 (CHORUS, DLY TM, DLY FB, DLY MX) */
    in_st.master_pot      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->in_fx_menu && !ui->in_group_menu && !ui->in_arp_menu && !ui->in_scope && ui->subpage == 0u,
          "FX button did not open FX Page 1/3");

    int32_t dt0 = s->fx_params.delay_time;
    int32_t df0 = s->fx_params.delay_fb;
    memset(&in_st, 0, sizeof(in_st));
    in_st.enc_delta[ADRIFT_ENC_KNOB1] = 10; /* CHORUS: +160 */
    in_st.enc_delta[ADRIFT_ENC_KNOB2] = 5;  /* DLY TM: +80 */
    in_st.enc_delta[ADRIFT_ENC_KNOB3] = 5;  /* DLY FB: +80 */
    in_st.enc_delta[ADRIFT_ENC_KNOB4] = 12; /* DLY MX: +192 */
    in_st.master_pot                  = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(s->fx_params.chorus_mix == 160 &&
          s->fx_params.delay_time == dt0 + 80 &&
          s->fx_params.delay_fb == df0 + 80 &&
          s->fx_params.delay_mix == 192,
          "FX Page 1/3 encoder edits failed (chr=%d, dt=%d, df=%d, dm=%d)",
          s->fx_params.chorus_mix, s->fx_params.delay_time,
          s->fx_params.delay_fb, s->fx_params.delay_mix);

    /* Press FX again -> FX Page 2/3 (RVB SZ, RVB DM, SHIMMR, RVB MX) */
    int32_t rsz0 = s->fx_params.reverb_size;
    int32_t rdm0 = s->fx_params.reverb_damp;
    int32_t rsh0 = s->fx_params.reverb_shimmer;
    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed             = ADRIFT_BTN_FX;
    in_st.enc_delta[ADRIFT_ENC_KNOB1] = 5;  /* RVB SZ: +80 */
    in_st.enc_delta[ADRIFT_ENC_KNOB2] = -5; /* RVB DM: -80 */
    in_st.enc_delta[ADRIFT_ENC_KNOB3] = 10; /* SHIMMR: +160 */
    in_st.enc_delta[ADRIFT_ENC_KNOB4] = 15; /* RVB MX: +240 */
    in_st.master_pot                  = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->in_fx_menu && ui->subpage == 1u, "Second FX press did not advance to FX Page 2/3");
    CHECK(s->fx_params.reverb_size == rsz0 + 80 &&
          s->fx_params.reverb_damp == rdm0 - 80 &&
          s->fx_params.reverb_shimmer == rsh0 + 160 &&
          s->fx_params.reverb_mix == 240,
          "FX Page 2/3 encoder edits failed (sz=%d, dm=%d, sh=%d, mx=%d)",
          s->fx_params.reverb_size, s->fx_params.reverb_damp,
          s->fx_params.reverb_shimmer, s->fx_params.reverb_mix);

    /* Verify 4th card on FX Page 2/3 (RVB MX) renders a full header band */
    uint16_t fx_card4_hdr = fb[145u * ADRIFT_SCREEN_W + 190u];
    uint16_t fx_card4_int = fb[185u * ADRIFT_SCREEN_W + 190u];
    CHECK(fx_card4_hdr != fx_card4_int,
          "Expected 4th card (RVB MX) on FX Page 2/3 to have a header band");

    /* Press FX a third time -> FX Page 3/3 (COMP, THRESH, ATTACK, MAKEUP) */
    int32_t cth0 = s->fx_params.comp_thresh;
    int32_t cat0 = s->fx_params.comp_attack;
    int32_t cmk0 = s->fx_params.comp_makeup;
    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed             = ADRIFT_BTN_FX;
    in_st.enc_delta[ADRIFT_ENC_KNOB1] = 20; /* COMP: +320 */
    in_st.enc_delta[ADRIFT_ENC_KNOB2] = -5; /* THRESH: -80 */
    in_st.enc_delta[ADRIFT_ENC_KNOB3] = 5;  /* ATTACK: +80 */
    in_st.enc_delta[ADRIFT_ENC_KNOB4] = 4;  /* MAKEUP: +64 */
    in_st.master_pot                  = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->in_fx_menu && ui->subpage == 2u, "Third FX press did not advance to FX Page 3/3");
    CHECK(s->fx_params.comp_mix == 320 &&
          s->fx_params.comp_thresh == cth0 - 80 &&
          s->fx_params.comp_attack == cat0 + 80 &&
          s->fx_params.comp_makeup == cmk0 + 64,
          "FX Page 3/3 encoder edits failed (cmp=%d, thr=%d, atk=%d, mkp=%d)",
          s->fx_params.comp_mix, s->fx_params.comp_thresh,
          s->fx_params.comp_attack, s->fx_params.comp_makeup);

    /* Verify 4th card on FX Page 3/3 (MAKEUP) renders a full header band, and 4th press wraps to 1/3 */
    uint16_t fx3_card4_hdr = fb[145u * ADRIFT_SCREEN_W + 190u];
    uint16_t fx3_card4_int = fb[185u * ADRIFT_SCREEN_W + 190u];
    CHECK(fx3_card4_hdr != fx3_card4_int,
          "Expected 4th card (MAKEUP) on FX Page 3/3 to have a header band");

    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed = ADRIFT_BTN_FX;
    in_st.master_pot      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->in_fx_menu && ui->subpage == 0u, "Fourth FX press did not wrap back to FX Page 1/3");

    /* Verify MIDI CCs 91/92/95 + 110..119 + 29..30 */
    adrift_synth_handle_midi(s, 0xB0u, 95u, 127u);
    adrift_synth_handle_midi(s, 0xB0u, 92u, 64u);
    adrift_synth_handle_midi(s, 0xB0u, 91u, 100u);
    adrift_synth_handle_midi(s, 0xB0u, 118u, 96u);
    adrift_synth_handle_midi(s, 0xB0u, 119u, 40u);
    adrift_synth_handle_midi(s, 0xB0u, 29u, 80u);
    adrift_synth_handle_midi(s, 0xB0u, 30u, 64u);
    CHECK(s->fx_params.chorus_mix == 1023 &&
          s->fx_params.reverb_shimmer > 500 &&
          s->fx_params.reverb_mix > 790 &&
          s->fx_params.comp_mix > 750 &&
          s->fx_params.comp_thresh > 300 &&
          s->fx_params.comp_attack > 600 &&
          s->fx_params.comp_makeup > 500,
          "MIDI CCs 91/92/95/118/119/29/30 failed to update Master FX parameters");

    adrift_synth_handle_midi(s, 0xB0u, 110u, 0u);
    adrift_synth_handle_midi(s, 0xB0u, 113u, 0u);
    adrift_synth_handle_midi(s, 0xB0u, 117u, 0u);
    adrift_synth_handle_midi(s, 0xB0u, 118u, 0u);
    CHECK(!adrift_fx_is_active(&s->fx_params),
          "MIDI CCs 110/113/117/118 failed to return Master FX to bypass");

    free(fb);
    free(pcm);
    printf("  -> PASS (Chorus, Ping-Pong Delay=%.1f dBFS, Shimmer Reverb=%.1f dBFS, Bus Comp spread %.1f->%.1f dB, FX 1..3 UI & CCs OK)\n",
           dly_tail_rms, rvb_tail_rms, dry_spread_db, cmp_spread_db);
}

/* ============================================================================
 * 11. External MIDI Clock Sync (24 PPQN 0xF8, Start 0xFA, Continue 0xFB, Stop 0xFC)
 * ============================================================================ */
void test_midi_clock_sync(void) {
    printf("[Test 11] External MIDI Clock Sync (24 PPQN -> Arp & Master Delay + UI + Watchdog)...\n");

    const uint32_t BLK = 1024u;
    int16_t  *pcm = (int16_t *)calloc(2u * BLK, sizeof(int16_t));
    uint16_t *fb  = (uint16_t *)calloc(ADRIFT_SCREEN_W * ADRIFT_SCREEN_H, sizeof(uint16_t));
    CHECK(pcm != NULL && fb != NULL, "Failed to allocate test buffers");

    adrift_app_init();
    adrift_synth_t *s  = adrift_app_get_synth();
    adrift_ui_t    *ui = adrift_app_get_ui();
    CHECK(!s->midi_clk_active && !s->midi_clk_running,
          "Expected external MIDI clock to be inactive at boot");

    /* Configure G1 Arpeggiator with internal rate = 80 BPM so external 150 BPM is distinct */
    adrift_group_t *g1    = &s->groups[0];
    g1->patch.arp.mode     = ADRIFT_ARP_UP;
    g1->patch.arp.range    = 1;
    g1->patch.arp.rate_bpm = 80;
    g1->patch.arp.gate     = 80;
    g1->patch.arp.latch    = true;
    g1->patch.arp.swing    = 0;
    g1->patch.arp.ratchet  = ADRIFT_RATCHET_1X;

    adrift_synth_handle_midi(s, 0x90u, 60u, 110u);
    adrift_synth_handle_midi(s, 0x90u, 64u, 110u);
    adrift_synth_handle_midi(s, 0x90u, 67u, 110u);
    CHECK(g1->arp_state.num_notes == 3u, "Expected 3 held notes in G1 arp");

    /* 150 BPM at 44100 Hz, 24 PPQN -> 44100 * 60 / (150 * 24) = 735 samples per 0xF8 pulse.
     * 6 pulses = 1/16th note = 4410 samples (whereas 80 BPM internal = 8268 samples). */
    const uint32_t PULSE_SAMPLES = 735u;
    adrift_midi_msg_t start_and_tick[2] = {
        { 0xFAu, 0u, 0u, 0u }, /* MIDI Start */
        { 0xF8u, 0u, 0u, 0u }  /* Pulse 0 (downbeat) */
    };
    adrift_headless_render(pcm, PULSE_SAMPLES, start_and_tick, 2u);
    CHECK(s->midi_clk_running, "0xFA Start did not set midi_clk_running");
    CHECK(g1->arp_state.step_counter == 1u && g1->arp_state.sounding_notes[0] == 60u,
          "0xFA + 0xF8 downbeat did not reset arp to step 1 (note 60), got step=%u note=%u",
          (unsigned)g1->arp_state.step_counter, (unsigned)g1->arp_state.sounding_notes[0]);

    /* Feed pulses 1..5 (completes the first 6-pulse 1/16th step at 150 BPM) */
    adrift_midi_msg_t tick_msg = { 0xF8u, 0u, 0u, 0u };
    for (uint32_t p = 1u; p < 6u; p++) {
        adrift_headless_render(pcm, PULSE_SAMPLES, &tick_msg, 1u);
    }
    CHECK(s->midi_clk_active && s->midi_clk_bpm == 150u,
          "External MIDI clock failed to lock to 150 BPM (active=%d, bpm=%u)",
          (int)s->midi_clk_active, (unsigned)s->midi_clk_bpm);
    CHECK(s->midi_clk_period_q4 == (PULSE_SAMPLES << 4),
          "Expected midi_clk_period_q4=%u, got %u",
          (unsigned)(PULSE_SAMPLES << 4), (unsigned)s->midi_clk_period_q4);

    /* Pulse 6 triggers step 2 (E4 = 64) phase-locked to the 6th MIDI clock tick */
    adrift_headless_render(pcm, PULSE_SAMPLES, &tick_msg, 1u);
    CHECK(g1->arp_state.step_counter == 2u && g1->arp_state.sounding_notes[0] == 64u,
          "Pulse 6 did not advance arp to step 2 (E4=64), got step=%u note=%u",
          (unsigned)g1->arp_state.step_counter, (unsigned)g1->arp_state.sounding_notes[0]);

    /* Pulses 7..11 + Pulse 12 -> triggers step 3 (G4 = 67) */
    for (uint32_t p = 7u; p <= 12u; p++) {
        adrift_headless_render(pcm, PULSE_SAMPLES, &tick_msg, 1u);
    }
    CHECK(g1->arp_state.step_counter == 3u && g1->arp_state.sounding_notes[0] == 67u,
          "Pulse 12 did not advance arp to step 3 (G4=67), got step=%u note=%u",
          (unsigned)g1->arp_state.step_counter, (unsigned)g1->arp_state.sounding_notes[0]);

    /* Test 2X Ratchet subdivision under External MIDI Clock */
    g1->patch.arp.ratchet = ADRIFT_RATCHET_2X;
    for (uint32_t p = 13u; p <= 18u; p++) {
        adrift_headless_render(pcm, PULSE_SAMPLES, &tick_msg, 1u);
    }
    /* At Pulse 18 (start of step 4) + 735 samples, substep_idx is 0; after 2 more pulses (2205 samples total = half of 4410),
     * the 2X ratchet sub-step should have retriggered (substep_idx == 1) */
    CHECK(g1->arp_state.step_counter == 4u && g1->arp_state.substep_idx == 0u,
          "Expected substep_idx==0 at start of step 4");
    adrift_headless_render(pcm, PULSE_SAMPLES, &tick_msg, 1u);
    adrift_headless_render(pcm, PULSE_SAMPLES, &tick_msg, 1u);
    CHECK(g1->arp_state.substep_idx == 1u,
          "Expected 2X ratchet to trigger second substep halfway through external 1/16th step");

    /* Test Master Stereo Delay tempo subdivision sync (1/32 .. 1/4) */
    CHECK(adrift_fx_delay_subdiv_pulses(0) == 3u &&
          strcmp(adrift_fx_delay_subdiv_name(0), "1/32") == 0 &&
          adrift_fx_delay_subdiv_pulses(300) == 6u &&
          strcmp(adrift_fx_delay_subdiv_name(300), "1/16") == 0 &&
          adrift_fx_delay_subdiv_pulses(700) == 12u &&
          strcmp(adrift_fx_delay_subdiv_name(700), "1/8") == 0 &&
          adrift_fx_delay_subdiv_pulses(1023) == 24u &&
          strcmp(adrift_fx_delay_subdiv_name(1023), "1/4") == 0,
          "Delay tempo subdivision table mismatch");

    s->fx_params.delay_mix  = 600;
    s->fx_params.delay_time = 300; /* "1/16" -> 6 pulses = 4410 samples (1128960 in Q8) */
    for (uint32_t p = 0u; p < 12u; p++) {
        adrift_headless_render(pcm, PULSE_SAMPLES, &tick_msg, 1u);
    }
    int32_t target_16th_q8 = (int32_t)(6u * PULSE_SAMPLES * 256u);
    int32_t err_q8 = s->fx.delay_len_q8 - target_16th_q8;
    if (err_q8 < 0) err_q8 = -err_q8;
    CHECK(s->fx.clk_period_q4 == s->midi_clk_period_q4 && err_q8 < (64 << 8),
          "Master Delay did not tempo-sync to 1/16th subdivision (delay_len_q8=%d, target_q8=%d)",
          s->fx.delay_len_q8, target_16th_q8);

    /* Verify UI ARP and FX screens render External Clock indicators ("EXT150", "1/16", "SYNC") */
    adrift_input_state_t in_st;
    adrift_dirty_t dirty;
    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed = ADRIFT_BTN_ARP;
    in_st.master_pot      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->in_arp_menu && ui->prev_midi_clk_active && ui->prev_midi_clk_bpm == 150u,
          "UI ARP screen did not track external MIDI clock state");

    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed = ADRIFT_BTN_FX;
    in_st.master_pot      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->in_fx_menu && dirty.row_mask[1] != 0u,
          "UI FX screen did not render synced state");

    /* Verify 0xFC Stop unlocks external clock, and 0xFB Continue resumes */
    adrift_synth_handle_midi(s, 0xFCu, 0u, 0u);
    CHECK(!s->midi_clk_active && !s->midi_clk_running,
          "0xFC Stop did not unlock external MIDI clock");

    adrift_synth_handle_midi(s, 0xFBu, 0u, 0u);
    adrift_headless_render(pcm, PULSE_SAMPLES, &tick_msg, 1u);
    adrift_headless_render(pcm, PULSE_SAMPLES, &tick_msg, 1u);
    CHECK(s->midi_clk_active && s->midi_clk_running,
          "0xFB Continue + 0xF8 pulses did not re-lock external MIDI clock");

    /* Verify 2-second watchdog timeout (> 88200 samples without 0xF8) reverts to internal clock */
    for (uint32_t i = 0u; i < 90u; i++) {
        adrift_headless_render(pcm, 1000u, NULL, 0u);
    }
    CHECK(!s->midi_clk_active && !s->midi_clk_running,
          "External MIDI clock watchdog did not fall back to internal clock after >2s silence");

    free(fb);
    free(pcm);
    printf("  -> PASS (24 PPQN lock=150 BPM, phase-locked Arp & 2X Ratchet, Delay 1/16 sync, Stop/Watchdog OK)\n");
}
