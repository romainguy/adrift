/* SPDX-License-Identifier: MPL-2.0
 * Automated Verification for Klatt (1980) Formant Speech Synthesis & Screen Reader */

#include "test_common.h"

typedef struct {
    uint32_t        count;
    char            last_text[256];
    char            texts[16][256];
    adrift_sr_cat_t cats[16];
    adrift_sr_prio_t prios[16];
} sr_test_log_t;

static void sr_test_logger(const char *ctx,
                           adrift_sr_cat_t cat,
                           adrift_sr_prio_t prio,
                           const char *text,
                           void *user) {
    (void)ctx;
    sr_test_log_t *lg = (sr_test_log_t *)user;
    if (!lg || !text) return;
    snprintf(lg->last_text, sizeof(lg->last_text), "%s", text);
    if (lg->count < 16u) {
        snprintf(lg->texts[lg->count], sizeof(lg->texts[0]), "%s", text);
        lg->cats[lg->count]  = cat;
        lg->prios[lg->count] = prio;
    }
    lg->count++;
}

static uint32_t drain_klatt_speech(adrift_tts_t *tts,
                                   adrift_klatt_t *kl,
                                   int16_t *mono_out,
                                   uint32_t max_samples) {
    uint32_t total = 0u;
    uint32_t guard = 0u;
    while ((tts->active || adrift_klatt_is_speaking(kl)) && total < max_samples && guard < 4000u) {
        (void)adrift_tts_pump(tts, kl);
        uint32_t chunk = max_samples - total;
        if (chunk > 256u) chunk = 256u;
        bool active = adrift_klatt_render(kl, mono_out + total, chunk);
        if (!active && !tts->active) break;
        total += chunk;
        guard++;
    }
    return total;
}

static void mono_to_stereo(const int16_t *mono, int16_t *stereo, uint32_t frames) {
    for (uint32_t i = 0u; i < frames; i++) {
        stereo[i * 2u + 0u] = mono[i];
        stereo[i * 2u + 1u] = mono[i];
    }
}

static void test_klatt_formant_dsp(void) {
    printf("[Speech 1] Klatt (1980) cascade/parallel formant DSP, spectral peaks & speed...\n");

    adrift_klatt_t kl;
    adrift_tts_t   tts;
    adrift_klatt_init(&kl);
    adrift_tts_init(&tts);

    int16_t *mono   = (int16_t *)calloc(ADRIFT_SAMPLE_RATE * 4u, sizeof(int16_t));
    int16_t *stereo = (int16_t *)calloc(ADRIFT_SAMPLE_RATE * 4u * 2u, sizeof(int16_t));
    CHECK(mono != NULL && stereo != NULL, "Speech test buffer allocation");
    if (!mono || !stereo) {
        free(mono);
        free(stereo);
        return;
    }

    adrift_tts_start(&tts, &kl, "Screen reader on.");
    uint32_t n_samp = drain_klatt_speech(&tts, &kl, mono, ADRIFT_SAMPLE_RATE * 4u);
    CHECK(n_samp > ADRIFT_SAMPLE_RATE / 3u && n_samp < ADRIFT_SAMPLE_RATE * 3u,
          "Expected ~0.4..3.0s speech duration, got %u samples", n_samp);

    mono_to_stereo(mono, stereo, n_samp);
    double rms_db = compute_rms_dbfs(stereo, 0u, n_samp);
    double dc_lsb = fabs(compute_mean_lsb(stereo, 0u, n_samp));
    CHECK(rms_db > -32.0 && rms_db < -1.0,
          "Expected audible speech RMS (-32..-1 dBFS), got %.2f dBFS", rms_db);
    CHECK(dc_lsb < 250.0,
          "Expected low DC offset (< 250 LSB), got %.2f LSB", dc_lsb);

    /* Direct formant verification: open back /AA/ (F1=730, F2=1090) vs close front /IY/ (F1=280, F2=2290) */
    const uint32_t k_test_frames = 40u * 220u;
    adrift_klatt_reset(&kl);
    adrift_klatt_frame_t f_aa;
    memset(&f_aa, 0, sizeof(f_aa));
    f_aa.f0_hz10 = 1200u;
    f_aa.f1      = 730u;  f_aa.b1 = 75u;
    f_aa.f2      = 1090u; f_aa.b2 = 100u;
    f_aa.f3      = 2440u; f_aa.b3 = 140u;
    f_aa.fnz     = 270u;
    f_aa.av      = 60u;
    f_aa.dur_mul = 16u;
    for (int i = 0; i < 45; i++) (void)adrift_klatt_push_frame(&kl, &f_aa);
    (void)adrift_klatt_render(&kl, mono, k_test_frames);
    mono_to_stereo(mono, stereo, k_test_frames);
    /* At Fs=11030 Hz and f0=120 Hz (T0=91 -> 121.2 Hz), H2=242.4 Hz, H6=727.3 Hz, H19=2303.0 Hz */
    double aa_f1 = goertzel_mag_db(stereo, 1024u, 4096u, 727.3);
    double aa_f2 = goertzel_mag_db(stereo, 1024u, 4096u, 2303.0);
    CHECK(aa_f1 > aa_f2 + 6.0,
          "Expected /AA/ F1 harmonic (727 Hz: %.1f dB) > 2303 Hz (%.1f dB) by > 6 dB", aa_f1, aa_f2);

    adrift_klatt_reset(&kl);
    adrift_klatt_frame_t f_iy = f_aa;
    f_iy.f1 = 280u;  f_iy.b1 = 55u;
    f_iy.f2 = 2290u; f_iy.b2 = 90u;
    f_iy.f3 = 3010u; f_iy.b3 = 150u;
    for (int i = 0; i < 45; i++) (void)adrift_klatt_push_frame(&kl, &f_iy);
    (void)adrift_klatt_render(&kl, mono, k_test_frames);
    mono_to_stereo(mono, stereo, k_test_frames);
    double iy_f1_lo = goertzel_mag_db(stereo, 1024u, 4096u, 242.4);
    double iy_f1_hi = goertzel_mag_db(stereo, 1024u, 4096u, 727.3);
    double iy_f2    = goertzel_mag_db(stereo, 1024u, 4096u, 2303.0);
    CHECK(iy_f1_lo > iy_f1_hi + 8.0 && iy_f2 > aa_f2 + 6.0,
          "Expected /IY/ F1(242)=%.1f > 727=%.1f and F2(2303)=%.1f > /AA/(2303)=%.1f",
          iy_f1_lo, iy_f1_hi, iy_f2, aa_f2);
    double hf_vowel = calc_hf_diff_ratio_db(stereo, 1024u, 4096u);

    /* Parallel fricative /S/: unvoiced high-frequency noise */
    adrift_klatt_reset(&kl);
    adrift_klatt_frame_t f_ss;
    memset(&f_ss, 0, sizeof(f_ss));
    f_ss.f0_hz10 = 0u;
    f_ss.f1      = 350u;  f_ss.b1 = 150u;
    f_ss.f2      = 1750u; f_ss.b2 = 180u;
    f_ss.f3      = 2600u; f_ss.b3 = 220u;
    f_ss.fnz     = 270u;
    f_ss.af      = 60u;
    f_ss.a5      = 50u;
    f_ss.a6      = 56u;
    f_ss.sw_par  = 1u;
    f_ss.dur_mul = 16u;
    for (int i = 0; i < 45; i++) (void)adrift_klatt_push_frame(&kl, &f_ss);
    (void)adrift_klatt_render(&kl, mono, k_test_frames);
    mono_to_stereo(mono, stereo, k_test_frames);
    double hf_fric = calc_hf_diff_ratio_db(stereo, 1024u, 4096u);
    CHECK(hf_fric > hf_vowel + 6.0,
          "Expected /S/ fricative HF ratio (%.1f dB) > voiced vowel (%.1f dB)", hf_fric, hf_vowel);

    /* Speed 1..5 duration monotonicity */
    uint32_t dur[6] = {0};
    for (uint8_t sp = 1u; sp <= 5u; sp++) {
        adrift_klatt_reset(&kl);
        adrift_klatt_set_speed(&kl, sp);
        adrift_tts_start(&tts, &kl, "Envelope 1, attack, decay, sustain, release.");
        dur[sp] = drain_klatt_speech(&tts, &kl, mono, ADRIFT_SAMPLE_RATE * 4u);
    }
    CHECK(dur[1] > dur[2] && dur[2] > dur[3] && dur[3] > dur[4] && dur[4] > dur[5] && dur[5] > 2000u,
          "Expected strictly decreasing duration across speeds 1..5: %u > %u > %u > %u > %u",
          dur[1], dur[2], dur[3], dur[4], dur[5]);

    free(mono);
    free(stereo);
}

static void test_speech_audio_isolation(void) {
    printf("[Speech 2] Line-out/headphone mixing vs pure synth/USB audio isolation...\n");

    const uint32_t frames = 512u;
    int16_t ref_pcm[512u * 2u];
    int16_t usb_pcm[512u * 2u];
    int16_t line_pcm[512u * 2u];

    adrift_midi_msg_t ev = { 0x90u, 60u, 100u, 0u };

    /* Reference render with screen reader OFF */
    adrift_headless_storage_reset();
    adrift_app_init();
    CHECK(!adrift_app_get_screen_reader()->enabled, "Screen reader must default to OFF on boot");
    adrift_headless_render(ref_pcm, frames, &ev, 1u);

    /* Identical synth render with screen reader ON and actively speaking */
    adrift_headless_storage_reset();
    adrift_app_init();
    adrift_sr_set_enabled(adrift_app_get_screen_reader(), true, true,
                          adrift_app_get_ui(), adrift_app_get_synth());
    CHECK(adrift_klatt_is_speaking(&adrift_app_get_screen_reader()->klatt),
          "Screen reader must enqueue speech frames when enabled");

    adrift_headless_render(usb_pcm, frames, &ev, 1u);
    memcpy(line_pcm, usb_pcm, sizeof(usb_pcm));
    adrift_app_mix_line_out(line_pcm, frames);

    CHECK(memcmp(ref_pcm, usb_pcm, sizeof(ref_pcm)) == 0,
          "adrift_app_audio_block (USB/synth out) MUST remain 100%% bit-exact with speech active");
    CHECK(memcmp(usb_pcm, line_pcm, sizeof(usb_pcm)) != 0,
          "adrift_app_mix_line_out MUST mix synthesized speech into line-out/headphone buffer");
}

static void test_speech_black_key_chords_and_rules(void) {
    printf("[Speech 3] BK11 + BK6/7/4/5 chords, settle timers, ANN_KEEP & Rule 3 A-note...\n");

    adrift_headless_storage_reset();
    adrift_app_init();
    adrift_screen_reader_t *sr    = adrift_app_get_screen_reader();
    adrift_ui_t            *ui    = adrift_app_get_ui();
    adrift_synth_t         *synth = adrift_app_get_synth();
    adrift_ui_dismiss_boot(ui);

    sr_test_log_t log;
    memset(&log, 0, sizeof(log));
    adrift_sr_set_logger(sr, sr_test_logger, &log, "test");

    bool rel_bk11 = false;
    /* Pressing BK6 alone plays a normal note (not consumed) */
    CHECK(!adrift_sr_handle_panel_key(sr, ui, synth, ADRIFT_SR_KEY_TOGGLE_BK6, true, &rel_bk11),
          "BK6 without BK11 must not be consumed");
    (void)adrift_sr_handle_panel_key(sr, ui, synth, ADRIFT_SR_KEY_TOGGLE_BK6, false, &rel_bk11);
    CHECK(!sr->enabled, "Screen reader stays off when BK6 pressed alone");

    /* Hold BK11 (25), then press BK6 (13) -> toggles screen reader ON and releases BK11 note */
    CHECK(!adrift_sr_handle_panel_key(sr, ui, synth, ADRIFT_SR_KEY_MOD_BK11, true, &rel_bk11),
          "BK11 press alone allows note until chord partner is pressed");
    CHECK(adrift_sr_handle_panel_key(sr, ui, synth, ADRIFT_SR_KEY_TOGGLE_BK6, true, &rel_bk11),
          "BK11 + BK6 must be consumed as toggle chord");
    CHECK(rel_bk11, "BK11 + BK6 must signal Note-Off release for BK11");
    CHECK(sr->enabled, "BK11 + BK6 must enable screen reader");
    (void)adrift_sr_handle_panel_key(sr, ui, synth, ADRIFT_SR_KEY_TOGGLE_BK6, false, &rel_bk11);

    /* While BK11 is still held: BK5 (speed up), BK4 (speed down), BK7 (WHERE) */
    CHECK(adrift_sr_handle_panel_key(sr, ui, synth, ADRIFT_SR_KEY_FASTER_BK5, true, &rel_bk11),
          "BK11 + BK5 must be consumed as speed up");
    CHECK(!rel_bk11, "BK11 already marked as modifier should not re-trigger Note-Off");
    CHECK(sr->klatt.speed == 4u, "Expected speed 4 after BK11 + BK5, got %u", sr->klatt.speed);
    (void)adrift_sr_handle_panel_key(sr, ui, synth, ADRIFT_SR_KEY_FASTER_BK5, false, &rel_bk11);

    CHECK(adrift_sr_handle_panel_key(sr, ui, synth, ADRIFT_SR_KEY_SLOWER_BK4, true, &rel_bk11),
          "BK11 + BK4 must be consumed as speed down");
    CHECK(sr->klatt.speed == 3u, "Expected speed 3 after BK11 + BK4, got %u", sr->klatt.speed);
    (void)adrift_sr_handle_panel_key(sr, ui, synth, ADRIFT_SR_KEY_SLOWER_BK4, false, &rel_bk11);

    uint32_t prev_cnt = log.count;
    CHECK(adrift_sr_handle_panel_key(sr, ui, synth, ADRIFT_SR_KEY_WHERE_BK7, true, &rel_bk11),
          "BK11 + BK7 must be consumed as WHERE");
    CHECK(log.count == prev_cnt + 1u && log.cats[prev_cnt] == ADRIFT_SR_CAT_WHERE,
          "BK11 + BK7 must immediately emit WHERE utterance");
    (void)adrift_sr_handle_panel_key(sr, ui, synth, ADRIFT_SR_KEY_WHERE_BK7, false, &rel_bk11);
    (void)adrift_sr_handle_panel_key(sr, ui, synth, ADRIFT_SR_KEY_MOD_BK11, false, &rel_bk11);

    /* Drain any active speech before testing settle timers */
    int16_t sink[512];
    for (int i = 0; i < 400 && (sr->tts.active || adrift_klatt_is_speaking(&sr->klatt) || sr->q_count > 0u); i++) {
        adrift_sr_mix_line_out(sr, sink, 256u);
    }

    /* Rule 18: 120 ms knob settle coalesces rapid turns into one final readout */
    memset(&log, 0, sizeof(log));
    uint16_t fb[ADRIFT_SCREEN_W * ADRIFT_SCREEN_H];
    adrift_dirty_t dirty;
    adrift_input_state_t in;
    memset(&in, 0, sizeof(in));
    in.master_pot = -1;

    for (int step = 0; step < 5; step++) {
        in.enc_delta[ADRIFT_ENC_KNOB1] = 2;
        adrift_dirty_clear(&dirty);
        adrift_app_tick_ui(&in, fb, &dirty); /* +16 ms (< 120 ms) */
    }
    CHECK(log.count == 0u, "Knob readout must wait for 120 ms settle, got %u early utterances", log.count);
    in.enc_delta[ADRIFT_ENC_KNOB1] = 0;
    for (int i = 0; i < 8; i++) {
        adrift_dirty_clear(&dirty);
        adrift_app_tick_ui(&in, fb, &dirty);
    }
    CHECK(log.count == 1u && log.cats[0] == ADRIFT_SR_CAT_READOUT,
          "Expected exactly 1 settled knob readout after >= 120 ms, got %u", log.count);

    /* Drain speech */
    for (int i = 0; i < 400 && (sr->tts.active || adrift_klatt_is_speaking(&sr->klatt) || sr->q_count > 0u); i++) {
        adrift_sr_mix_line_out(sr, sink, 256u);
    }

    /* Rule 15: ANN_KEEP warning cannot be cut off by a subsequent NOW utterance */
    memset(&log, 0, sizeof(log));
    synth->kbd_octave = -3;
    in.buttons_pressed = ADRIFT_BTN_OCT_DOWN;
    in.buttons_down    = ADRIFT_BTN_OCT_DOWN;
    adrift_app_tick_ui(&in, fb, &dirty);
    CHECK(log.count == 1u && log.prios[0] == ADRIFT_SR_PRIO_KEEP && sr->current_keep,
          "Octave limit warning must start with ANN_KEEP");

    /* Trigger a NOW action while the warning is mid-utterance */
    in.buttons_pressed = ADRIFT_BTN_PANIC;
    in.buttons_down    = ADRIFT_BTN_PANIC;
    adrift_app_tick_ui(&in, fb, &dirty);
    CHECK(log.count == 1u && sr->q_count == 1u && sr->current_keep,
          "NOW utterance must queue behind active ANN_KEEP warning instead of cutting it off");

    for (int i = 0; i < 600 && (sr->tts.active || adrift_klatt_is_speaking(&sr->klatt) || sr->q_count > 0u); i++) {
        adrift_sr_mix_line_out(sr, sink, 256u);
    }
    CHECK(log.count == 2u && strcmp(log.texts[1], "All notes off.") == 0,
          "Queued NOW utterance ('%s') must play after ANN_KEEP completes", log.texts[1]);

    /* Rule 3: Capital 'A' before scale/chord word stays note EY1, before ordinary word becomes AX0 */
    uint8_t ph_note[32], ph_art[32], ph_solo[32];
    uint32_t n_ph_note = adrift_tts_phonemize_phrase("A minor.", ph_note, 32u);
    uint32_t n_ph_art  = adrift_tts_phonemize_phrase("A voice.", ph_art, 32u);
    uint32_t n_ph_solo = adrift_tts_phonemize_phrase("A.", ph_solo, 32u);
    CHECK(n_ph_note > 0u && ADRIFT_PH_ID(ph_note[0]) == ADRIFT_PH_EY,
          "Expected 'A minor.' to start with note phoneme EY, got %u", ADRIFT_PH_ID(ph_note[0]));
    CHECK(n_ph_art > 0u && ADRIFT_PH_ID(ph_art[0]) == ADRIFT_PH_AX,
          "Expected 'A voice.' to start with article schwa AX, got %u", ADRIFT_PH_ID(ph_art[0]));
    CHECK(n_ph_solo > 0u && ADRIFT_PH_ID(ph_solo[0]) == ADRIFT_PH_EY,
          "Expected standalone 'A.' to stay letter/note EY, got %u", ADRIFT_PH_ID(ph_solo[0]));

    /* BK11 + BK6 toggles screen reader OFF and announces 'screen reader off.' */
    (void)adrift_sr_handle_panel_key(sr, ui, synth, ADRIFT_SR_KEY_MOD_BK11, true, &rel_bk11);
    (void)adrift_sr_handle_panel_key(sr, ui, synth, ADRIFT_SR_KEY_TOGGLE_BK6, true, &rel_bk11);
    CHECK(!sr->enabled, "BK11 + BK6 second time must disable screen reader");
}

static void test_speech_post_utterance_silence(void) {
    printf("[Speech 4] Post-utterance exact digital silence & zero limit-cycle regression...\n");

    static const char *const k_phrases[] = {
        "Screen reader on. Exciter.",
        "Presets.",
        "Speed 4.",
        "morph 60 percent.",
        "A phrygian, 7 notes, 8 of 16.",
        "Screen reader off."
    };

    int16_t blk[128];
    for (size_t p = 0u; p < sizeof(k_phrases) / sizeof(k_phrases[0]); p++) {
        adrift_headless_storage_reset();
        adrift_app_init();
        adrift_screen_reader_t *sr = adrift_app_get_screen_reader();
        sr->enabled = true;
        adrift_sr_say(sr, ADRIFT_SR_PRIO_NOW, ADRIFT_SR_CAT_ACTION, k_phrases[p]);

        /* Drain utterance + bounded ring-down + duck recovery (~3.5 s max) */
        for (uint32_t b = 0u; b < (ADRIFT_SAMPLE_RATE * 4u) / 64u; b++) {
            memset(blk, 0, sizeof(blk));
            adrift_app_mix_line_out(blk, 64u);
            if (!sr->tts.active &&
                !adrift_klatt_is_speaking(&sr->klatt) &&
                sr->klatt.tail_rem == 0u &&
                sr->duck_gain_q14 >= 16384u) {
                break;
            }
        }

        CHECK(sr->klatt.tail_rem == 0u && sr->duck_gain_q14 == 16384u,
              "Expected tail_rem == 0 and duck_gain == 16384 after '%s'", k_phrases[p]);

        int32_t max_abs = 0;
        for (int b = 0; b < 64; b++) {
            memset(blk, 0, sizeof(blk));
            adrift_app_mix_line_out(blk, 64u);
            for (int i = 0; i < 128; i++) {
                int32_t v = (blk[i] < 0) ? -(int32_t)blk[i] : (int32_t)blk[i];
                if (v > max_abs) max_abs = v;
            }
        }
        CHECK(max_abs == 0,
              "Expected exact 0 LSB post-utterance silence after '%s', got max_abs=%d",
              k_phrases[p], max_abs);
    }

    /* Mid-vowel abrupt interruption must fade smoothly without clicks and settle to exact 0 LSB */
    adrift_headless_storage_reset();
    adrift_app_init();
    adrift_screen_reader_t *sr = adrift_app_get_screen_reader();
    sr->enabled = true;
    adrift_sr_say(sr, ADRIFT_SR_PRIO_NOW, ADRIFT_SR_CAT_ACTION, "Screen reader on. Exciter.");

    int16_t prev_sample = 0;
    for (int b = 0; b < 80; b++) {
        memset(blk, 0, sizeof(blk));
        adrift_app_mix_line_out(blk, 64u);
        prev_sample = blk[126];
    }
    adrift_klatt_clear_queue(&sr->klatt);
    adrift_tts_stop(&sr->tts);

    int32_t max_step = 0;
    for (int b = 0; b < 8; b++) {
        memset(blk, 0, sizeof(blk));
        adrift_app_mix_line_out(blk, 64u);
        for (int i = 0; i < 64; i++) {
            int32_t diff = (int32_t)blk[i * 2] - (int32_t)prev_sample;
            if (diff < 0) diff = -diff;
            if (diff > max_step) max_step = diff;
            prev_sample = blk[i * 2];
        }
    }
    CHECK(max_step < 6000,
          "Expected smooth click-free fade on interruption (< 6000 LSB/sample), got %d", max_step);

    int32_t post_irq_max = 0;
    for (int b = 0; b < 32; b++) {
        memset(blk, 0, sizeof(blk));
        adrift_app_mix_line_out(blk, 64u);
        for (int i = 0; i < 128; i++) {
            int32_t v = (blk[i] < 0) ? -(int32_t)blk[i] : (int32_t)blk[i];
            if (v > post_irq_max) post_irq_max = v;
        }
    }
    CHECK(post_irq_max == 0,
          "Expected exact 0 LSB silence after mid-utterance interruption, got %d", post_irq_max);

    /* Bit-exact synth pass-through once duck recovery completes */
    for (int b = 0; b < 32; b++) {
        memset(blk, 0, sizeof(blk));
        adrift_app_mix_line_out(blk, 64u);
    }
    adrift_midi_msg_t ev = { 0x90u, 60u, 100u, 0u };
    int16_t synth_raw[128];
    int16_t synth_mixed[128];
    adrift_app_audio_block(synth_raw, 64u, &ev, 1u);
    memcpy(synth_mixed, synth_raw, sizeof(synth_raw));
    adrift_app_mix_line_out(synth_mixed, 64u);
    CHECK(memcmp(synth_raw, synth_mixed, sizeof(synth_raw)) == 0,
          "Line-out synth audio must be 100%% bit-exact uncolored after speech completes");

    /* Intra-utterance active silence frames (speaking == true) must also decay to exact 0 LSB */
    adrift_klatt_t kl;
    adrift_tts_t   tts;
    adrift_klatt_init(&kl);
    adrift_tts_init(&tts);
    adrift_tts_start(&tts, &kl, "Screen reader on.");
    int16_t mono[256];
    while (tts.active) {
        (void)adrift_tts_pump(&tts, &kl);
        (void)adrift_klatt_render(&kl, mono, 256u);
    }
    adrift_klatt_frame_t sil;
    memset(&sil, 0, sizeof(sil));
    sil.f1 = 500u; sil.f2 = 1500u; sil.f3 = 2500u;
    sil.b1 = 90u;  sil.b2 = 120u;  sil.b3 = 160u;
    sil.fnz = 270u; sil.dur_mul = 16u;
    for (int i = 0; i < 400; i++) (void)adrift_klatt_push_frame(&kl, &sil);
    for (int b = 0; b < (int)(ADRIFT_SAMPLE_RATE * 2u + ADRIFT_SAMPLE_RATE / 4u) / 256; b++) {
        (void)adrift_klatt_render(&kl, mono, 256u);
    }
    bool still_speaking = adrift_klatt_render(&kl, mono, 256u);
    int32_t intra_max = 0;
    for (int i = 0; i < 256; i++) {
        int32_t v = (mono[i] < 0) ? -(int32_t)mono[i] : (int32_t)mono[i];
        if (v > intra_max) intra_max = v;
    }
    CHECK(still_speaking && intra_max == 0,
          "Expected exact 0 LSB during active intra-utterance silence (speaking=%d, max=%d)",
          (int)still_speaking, intra_max);
}

void run_speech_tests(void) {
    test_klatt_formant_dsp();
    test_speech_audio_isolation();
    test_speech_black_key_chords_and_rules();
    test_speech_post_utterance_silence();
}
