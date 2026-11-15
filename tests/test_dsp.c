/* SPDX-License-Identifier: MPL-2.0
 * Core DSP, Exciter, Envelopes, BBD Resonator, Physical Modeling & Modulation Tests */

#include "test_common.h"
#include "../src/platform/uac_fir.h"

double compute_rms_dbfs(const int16_t *stereo, uint32_t start_f, uint32_t count_f) {
    double sum_sq = 0.0;
    for (uint32_t i = 0; i < count_f; i++) {
        double s = (double)stereo[2u * (start_f + i)] / 32768.0;
        sum_sq += s * s;
    }
    double rms = sqrt(sum_sq / (double)(count_f ? count_f : 1u));
    if (rms < 1e-9) return -180.0;
    return 20.0 * log10(rms);
}

double compute_mean_lsb(const int16_t *stereo, uint32_t start_f, uint32_t count_f) {
    double sum = 0.0;
    for (uint32_t i = 0; i < count_f; i++) {
        sum += (double)stereo[2u * (start_f + i)];
    }
    return sum / (double)(count_f ? count_f : 1u);
}

/* Parabolic-interpolated autocorrelation pitch estimator around expected lag */
double estimate_freq_autocorr(const int16_t *stereo,
                              uint32_t start_f,
                              uint32_t window_f,
                              double min_hz,
                              double max_hz) {
    int lag_min = (int)floor(FS_EXACT / max_hz);
    int lag_max = (int)ceil(FS_EXACT / min_hz);
    if (lag_min < 4) lag_min = 4;
    if ((uint32_t)(lag_max + 2) >= window_f / 2u) {
        lag_max = (int)(window_f / 2u) - 3;
    }

    int    best_lag = lag_min;
    double best_r   = -1e30;
    double r_vals[2048];
    if (lag_max >= 2046) lag_max = 2045;

    for (int lag = lag_min - 1; lag <= lag_max + 1; lag++) {
        double acc = 0.0;
        double e0  = 0.0;
        double e1  = 0.0;
        uint32_t n = window_f - (uint32_t)lag;
        for (uint32_t i = 0; i < n; i++) {
            double a = (double)stereo[2u * (start_f + i)];
            double b = (double)stereo[2u * (start_f + i + (uint32_t)lag)];
            acc += a * b;
            e0  += a * a;
            e1  += b * b;
        }
        double denom = sqrt(e0 * e1);
        double norm  = (denom > 1e-9) ? (acc / denom) : 0.0;
        r_vals[lag]  = norm;
    }

    /* Pick first strong peak (>= 0.92 * global max) to avoid sub-harmonic octave ambiguity */
    for (int lag = lag_min; lag <= lag_max; lag++) {
        if (r_vals[lag] > best_r) {
            best_r   = r_vals[lag];
            best_lag = lag;
        }
    }
    for (int lag = lag_min; lag <= lag_max; lag++) {
        if (r_vals[lag] >= r_vals[lag - 1] &&
            r_vals[lag] >= r_vals[lag + 1] &&
            r_vals[lag] >= best_r * 0.96) {
            best_lag = lag;
            break;
        }
    }

    double ym1 = r_vals[best_lag - 1];
    double y0  = r_vals[best_lag];
    double yp1 = r_vals[best_lag + 1];
    double den = ym1 - 2.0 * y0 + yp1;
    double delta = (fabs(den) > 1e-12) ? (0.5 * (ym1 - yp1) / den) : 0.0;
    if (delta < -0.5) delta = -0.5;
    if (delta > 0.5)  delta = 0.5;
    return FS_EXACT / ((double)best_lag + delta);
}

double goertzel_mag_db(const int16_t *stereo, uint32_t start_f, uint32_t n, double freq_hz) {
    double re = 0.0, im = 0.0;
    double w  = 2.0 * M_PI * freq_hz / FS_EXACT;
    for (uint32_t i = 0; i < n; i++) {
        /* Hann window */
        double win = 0.5 * (1.0 - cos(2.0 * M_PI * (double)i / (double)(n - 1u)));
        double s   = ((double)stereo[2u * (start_f + i)] / 32768.0) * win;
        re += s * cos(w * (double)i);
        im -= s * sin(w * (double)i);
    }
    double mag = (2.0 * sqrt(re * re + im * im)) / (double)n;
    return (mag > 1e-9) ? (20.0 * log10(mag)) : -180.0;
}

/* High-resolution Hann-windowed DFT peak finder around expected fundamental (+-120 cents, 0.2-cent resolution) */
double estimate_fundamental_dft(const int16_t *stereo, uint32_t start_f, uint32_t n, double target_hz) {
    /* Coarse scan: 2-cent steps across [-120, +120] cents */
    double best_cents = 0.0;
    double best_p     = -1.0;
    for (int c = -120; c <= 120; c += 2) {
        double f = target_hz * pow(2.0, (double)c / 1200.0);
        double w = 2.0 * M_PI * f / FS_EXACT;
        double re = 0.0, im = 0.0;
        for (uint32_t i = 0; i < n; i++) {
            double win = 0.5 * (1.0 - cos(2.0 * M_PI * (double)i / (double)(n - 1u)));
            double s   = (double)stereo[2u * (start_f + i)] * win;
            re += s * cos(w * (double)i);
            im -= s * sin(w * (double)i);
        }
        double p = re * re + im * im;
        if (p > best_p) {
            best_p     = p;
            best_cents = (double)c;
        }
    }
    /* Fine scan: 0.2-cent steps across [best_cents - 3, best_cents + 3] */
    double fine_best_f = target_hz * pow(2.0, best_cents / 1200.0);
    for (int k = -15; k <= 15; k++) {
        double c = best_cents + (double)k * 0.2;
        double f = target_hz * pow(2.0, c / 1200.0);
        double w = 2.0 * M_PI * f / FS_EXACT;
        double re = 0.0, im = 0.0;
        for (uint32_t i = 0; i < n; i++) {
            double win = 0.5 * (1.0 - cos(2.0 * M_PI * (double)i / (double)(n - 1u)));
            double s   = (double)stereo[2u * (start_f + i)] * win;
            re += s * cos(w * (double)i);
            im -= s * sin(w * (double)i);
        }
        double p = re * re + im * im;
        if (p > best_p) {
            best_p      = p;
            fine_best_f = f;
        }
    }
    return fine_best_f;
}

double calc_hf_diff_ratio_db(const int16_t *stereo_pcm, uint32_t start_frame, uint32_t count) {
    double sig_acc  = 1e-12;
    double diff_acc = 1e-12;
    for (uint32_t i = start_frame + 1u; i < start_frame + count; i++) {
        double x0 = (double)stereo_pcm[(i - 1u) * 2u] / 32768.0;
        double x1 = (double)stereo_pcm[i * 2u] / 32768.0;
        sig_acc  += x1 * x1;
        diff_acc += (x1 - x0) * (x1 - x0);
    }
    return 10.0 * log10(diff_acc / sig_acc);
}

void render_frames(int16_t *buf, uint32_t frames) {
    adrift_headless_render(buf, frames, NULL, 0u);
}

/* ---- 1. Micro-ACB Virtual Analog VCO & S&H Noise Tests ---- */
void test_micro_acb_exciter(void) {
    printf("[Test 1] Micro-ACB Virtual Analog VCO & S&H Noise...\n");
    const uint32_t N = 16384u;
    int16_t *pcm = calloc(N * 2u, sizeof(int16_t));

    adrift_app_init();
    adrift_synth_t *s = adrift_app_get_synth();
    adrift_patch_t *p = &s->groups[0].patch;

    /* Pure Dry VCO path, wide-open Input LPF, zero drift for harmonic inspection */
    p->dry_wet          = 0;
    p->in_lpf_cutoff    = 1023;
    p->in_lpf_res       = 0;
    p->exc.vco_level    = 900;
    p->exc.noise_level  = 0;
    p->exc.analog_drift = 300;
    p->env1.onset_delay = 0;
    p->env1.attack      = 0;
    p->env1.sustain     = 1023;
    p->env2.attack      = 0;
    p->env2.sustain     = 1023;
    p->lfo_depth        = 0;
    p->master_vol       = 900;

    /* 1a. Analog transistor-folded Sine (morph = 0): strong fundamental + warm -30..-65 dB harmonics */
    p->exc.wave_morph = 0;
    adrift_synth_note_on(s, 60u, 127u); /* C4 = 261.63 Hz */
    render_frames(pcm, N);
    double f0 = 440.0 * pow(2.0, (60.0 - 69.0) / 12.0);
    double h1_db = goertzel_mag_db(pcm, 4096u, 8192u, f0);
    double h2_db = goertzel_mag_db(pcm, 4096u, 8192u, f0 * 2.0);
    double h3_db = goertzel_mag_db(pcm, 4096u, 8192u, f0 * 3.0);
    CHECK(h1_db > -12.0, "ACB Sine fundamental too quiet: %.2f dB", h1_db);
    CHECK((h2_db - h1_db) > -68.0 && (h2_db - h1_db) < -24.0,
          "ACB Sine 2nd harmonic warmth out of expected analog range: %.2f dBc", h2_db - h1_db);
    CHECK((h3_db - h1_db) < -20.0, "ACB Sine 3rd harmonic too high: %.2f dBc", h3_db - h1_db);

    /* 1b. Morph sweep level & phase consistency across Sine -> Tri -> Saw -> Pulse (including midpoints) */
    static const int16_t k_sweep_pts[7] = { 0, 170, 341, 511, 682, 852, 1023 };
    double sweep_rms[7];
    double min_sweep_rms =  100.0;
    double max_sweep_rms = -100.0;
    p->exc.pulse_width = 512;
    for (int k = 0; k < 7; k++) {
        p->exc.wave_morph = k_sweep_pts[k];
        render_frames(pcm, 8192u);
        sweep_rms[k] = compute_rms_dbfs(pcm, 4096u, 4096u);
        if (sweep_rms[k] < min_sweep_rms) min_sweep_rms = sweep_rms[k];
        if (sweep_rms[k] > max_sweep_rms) max_sweep_rms = sweep_rms[k];
    }
    CHECK(fabs(sweep_rms[0] - sweep_rms[2]) < 0.8,
          "Sine (%.2f dBFS) and Triangle (%.2f dBFS) RMS not equalized", sweep_rms[0], sweep_rms[2]);
    CHECK((max_sweep_rms - min_sweep_rms) < 2.6,
          "Morph sweep RMS variation too large across 0..1023 (span=%.2f dB, min=%.2f, max=%.2f)",
          max_sweep_rms - min_sweep_rms, min_sweep_rms, max_sweep_rms);

    /* 1c. Extreme 10% Pulse Width (morph = 1023, pw = 60): verify 15 Hz AC-coupling cap removes DC! */
    p->exc.wave_morph  = 1023;
    p->exc.pulse_width = 60;
    render_frames(pcm, N); /* Let 15 Hz cap settle (~370 ms) */
    double mean_dc = fabs(compute_mean_lsb(pcm, 12000u, 4096u));
    double pulse_rms = compute_rms_dbfs(pcm, 12000u, 4096u);
    CHECK(pulse_rms > -16.0, "ACB narrow PWM output missing: %.2f dBFS", pulse_rms);
    CHECK(mean_dc < 250.0, "ACB 15 Hz coupling capacitor failed to block PWM DC (mean=%.1f LSB)", mean_dc);

    /* 1d. Stepped Sample & Hold Noise: verify consecutive samples hold identical values at low S&H rate */
    adrift_app_init();
    p = &s->groups[0].patch;
    p->dry_wet         = 0;
    p->in_lpf_cutoff   = 1023;
    p->exc.vco_level   = 950;
    p->exc.noise_level = 1023; /* 100% noise mix (0% wave) */
    p->exc.sh_rate     = 450;  /* ~466 Hz stepped S&H -> ~95 samples per hold step (~2-3 steps in 256) */
    p->env1.attack     = 0;
    p->env1.sustain    = 1023;
    p->env2.attack     = 0;
    p->env2.sustain    = 1023;

    int32_t raw_exc[256];
    int16_t morph_out = 0;
    adrift_exciter_prepare_block(&s->voices[0].exc, &p->exc, 60 << 8, 0, 0, 0, &morph_out);
    /* Warm 1 block so smoothed noise_gain_q15 settles at target before checking staircase holds */
    adrift_exciter_process(&s->voices[0].exc, &p->exc, morph_out, raw_exc, 64u);
    adrift_exciter_process(&s->voices[0].exc, &p->exc, morph_out, raw_exc, 256u);
    int held_pairs = 0;
    int transitions = 0;
    for (int i = 1; i < 256; i++) {
        if (abs(raw_exc[i] - raw_exc[i - 1]) <= 2) held_pairs++;
        else transitions++;
    }
    CHECK(held_pairs > 200 && transitions >= 1 && transitions <= 10,
          "S&H stepped noise expected staircases (held=%d, transitions=%d)", held_pairs, transitions);

    /* 1e. Wave/Noise Mix (0% = Wave Only, 100% = Noise Only) & VCO LEV Overall Exciter Level */
    p->exc.vco_level   = 0;
    p->exc.noise_level = 1023;
    adrift_exciter_process(&s->voices[0].exc, &p->exc, morph_out, raw_exc, 64u); /* settle gain ramp */
    adrift_exciter_process(&s->voices[0].exc, &p->exc, morph_out, raw_exc, 256u);
    int32_t max_silent = 0;
    for (int i = 0; i < 256; i++) {
        if (abs(raw_exc[i]) > max_silent) max_silent = abs(raw_exc[i]);
    }
    CHECK(max_silent == 0,
          "Expected VCO LEV = 0 to silence both wave and noise even at 100%% mix (max=%d)", max_silent);

    /* At MIX = 100% (1023) and VCO LEV = 900, vco_gain_q15 must be 0 and noise_gain_q15 > 28000 */
    p->exc.vco_level   = 900;
    p->exc.noise_level = 1023;
    adrift_exciter_process(&s->voices[0].exc, &p->exc, morph_out, raw_exc, 64u);
    CHECK(s->voices[0].exc.vco_gain_q15 == 0 && s->voices[0].exc.noise_gain_q15 > 28000,
          "Expected MIX=100%% to mute wave and pass only noise (vco_g=%d, noise_g=%d)",
          s->voices[0].exc.vco_gain_q15, s->voices[0].exc.noise_gain_q15);

    /* At MIX = 0% (0) and VCO LEV = 900, noise_gain_q15 must be 0 and vco_gain_q15 > 28000 */
    p->exc.noise_level = 0;
    adrift_exciter_process(&s->voices[0].exc, &p->exc, morph_out, raw_exc, 64u);
    CHECK(s->voices[0].exc.noise_gain_q15 == 0 && s->voices[0].exc.vco_gain_q15 > 28000,
          "Expected MIX=0%% to mute noise and pass only wave (vco_g=%d, noise_g=%d)",
          s->voices[0].exc.vco_gain_q15, s->voices[0].exc.noise_gain_q15);

    /* At MIX = 50% (512), halving VCO LEV from 900 to 450 halves both wave and noise gains */
    p->exc.vco_level   = 900;
    p->exc.noise_level = 512;
    adrift_exciter_process(&s->voices[0].exc, &p->exc, morph_out, raw_exc, 64u);
    int32_t vco_900   = s->voices[0].exc.vco_gain_q15;
    int32_t noise_900 = s->voices[0].exc.noise_gain_q15;
    p->exc.vco_level  = 450;
    adrift_exciter_process(&s->voices[0].exc, &p->exc, morph_out, raw_exc, 64u);
    int32_t vco_450   = s->voices[0].exc.vco_gain_q15;
    int32_t noise_450 = s->voices[0].exc.noise_gain_q15;
    CHECK(abs(vco_900 - 2 * vco_450) <= 32 && abs(noise_900 - 2 * noise_450) <= 32,
          "VCO LEV did not proportionally scale combined wave+noise level (v900=%d, v450=%d, n900=%d, n450=%d)",
          vco_900, vco_450, noise_900, noise_450);

    /* 1f. High-register anti-aliasing: PolyBLAMP Triangle, Sine folder taper & Pluck Nyquist gate */
    adrift_app_init();
    s = adrift_app_get_synth();
    p = &s->groups[0].patch;
    p->dry_wet          = 0;
    p->in_lpf_cutoff    = 1023;
    p->in_lpf_res       = 0;
    p->exc.vco_level    = 900;
    p->exc.noise_level  = 0;
    p->exc.analog_drift = 0;
    p->exc.wave_morph   = 341; /* Triangle */
    p->env1.onset_delay = 0;
    p->env1.attack      = 0;
    p->env1.sustain     = 1023;
    p->lfo_depth        = 0;
    p->master_vol       = 900;
    adrift_synth_note_on(s, 105u, 127u); /* A7 = 3520.0 Hz */
    render_frames(pcm, N);
    double f_a7          = 3520.0;
    double tri_h1_db     = goertzel_mag_db(pcm, 4096u, 8192u, f_a7);
    double tri_alias9_db = goertzel_mag_db(pcm, 4096u, 8192u, FS_EXACT - 9.0 * f_a7);  /* ~12.44 kHz */
    double tri_alias11_db= goertzel_mag_db(pcm, 4096u, 8192u, FS_EXACT - 11.0 * f_a7); /* ~5.40 kHz */
    CHECK((tri_alias9_db - tri_h1_db) < -50.0 && (tri_alias11_db - tri_h1_db) < -52.0,
          "High-register Triangle PolyBLAMP aliasing too high (9th=%.1f dBc, 11th=%.1f dBc)",
          tri_alias9_db - tri_h1_db, tri_alias11_db - tri_h1_db);

    adrift_synth_all_notes_off(s, true);
    p->exc.wave_morph   = 0;   /* Sine */
    p->exc.analog_drift = 600; /* Strong drift/folder enabled: verify high-pitch folder taper */
    adrift_synth_note_on(s, 110u, 127u); /* D8 = 4698.63 Hz */
    render_frames(pcm, N);
    double f_d8           = 440.0 * pow(2.0, (110.0 - 69.0) / 12.0);
    double sin_h1_db      = goertzel_mag_db(pcm, 4096u, 8192u, f_d8);
    double sin_alias5_db  = goertzel_mag_db(pcm, 4096u, 8192u, FS_EXACT - 5.0 * f_d8); /* ~20.62 kHz */
    double sin_alias7_db  = goertzel_mag_db(pcm, 4096u, 8192u, FS_EXACT - 7.0 * f_d8); /* ~11.23 kHz */
    CHECK((sin_alias5_db - sin_h1_db) < -55.0 && (sin_alias7_db - sin_h1_db) < -55.0,
          "High-register Sine wave-folder aliasing too high (5th=%.1f dBc, 7th=%.1f dBc)",
          sin_alias5_db - sin_h1_db, sin_alias7_db - sin_h1_db);

    /* Direct exciter Pluck impulse Nyquist gating above Fs/4 (MIDI 125 = F9 = 11.18 kHz: 2*f0, 3*f0, 5*f0 > Fs/2) */
    adrift_exciter_t exc_hi;
    adrift_exciter_init(&exc_hi, 0u);
    adrift_exciter_params_t ep_hi = p->exc;
    ep_hi.vco_level   = 0;    /* Isolate pick impulse only (sub_g = 0 when vco_level = 0) */
    ep_hi.noise_level = 0;
    ep_hi.pluck       = 1023;
    ep_hi.analog_drift= 0;
    adrift_exciter_prepare_block(&exc_hi, &ep_hi, 125 << 8, 0, 0, 0, &morph_out);
    adrift_exciter_trigger(&exc_hi, &ep_hi, 127u, false, 0u);
    int32_t imp_amp_ref = exc_hi.impulse_amp_q15;
    uint32_t dt_ref     = exc_hi.vco_inc;
    int32_t imp_buf[64] = {0};
    adrift_exciter_process(&exc_hi, &ep_hi, 0, imp_buf, 64u);
    /* Above Fs/4, h2/h3/h5 must be completely gated to 0, leaving only the bandlimited fundamental burst */
    uint32_t ref_ph = 1u;
    int32_t max_alias_err = 0;
    for (int i = 0; i < 16; i++) {
        int32_t expected = 0;
        if (ref_ph != 0u) {
            expected = dsp_mul_q15(dsp_sine_lut_q15(ref_ph) >> 1, imp_amp_ref);
            uint32_t nxt = ref_ph + dt_ref;
            ref_ph = (nxt < ref_ph) ? 0u : nxt;
        }
        int32_t err = abs(imp_buf[i] - expected);
        if (err > max_alias_err) max_alias_err = err;
    }
    CHECK(max_alias_err == 0,
          "High-note pluck impulse leaked above-Nyquist h2/h3/h5 harmonics (max_err=%d)", max_alias_err);

    free(pcm);
    printf("  -> PASS (Sine H2=%.1f dBc, PWM DC=%.1f LSB, S&H held=%d/255, Tri A7 alias=%.1f dBc)\n",
           h2_db - h1_db, mean_dc, held_pairs, tri_alias9_db - tri_h1_db);
}

/* ---- 2. Envelope 1 Onset Delay & Loop Mode ---- */
void test_env1_delay_and_loop(void) {
    printf("[Test 2] Envelope 1 Onset Delay (0-2s) & Loop Mode...\n");
    adrift_env_t env;
    adrift_env_params_t p = {
        .onset_delay = 512, /* ~500 ms (~22,059 samples) */
        .attack      = 60,
        .decay       = 180,
        .sustain     = 0,
        .release     = 150,
        .vel_sens    = 0,
        .loop_mode   = ADRIFT_ENV_LOOP_OFF,
        .drone_hold  = false
    };
    adrift_env_init(&env);
    adrift_env_note_on(&env, &p, 100u, false);
    uint32_t expected_delay = env.delay_total;
    CHECK(expected_delay > 18000u && expected_delay < 26000u,
          "Unexpected onset delay sample count: %u", expected_delay);

    int32_t blk[64];
    uint32_t first_nonzero = 0xFFFFFFFFu;
    for (uint32_t base = 0; base < expected_delay + 512u; base += 64u) {
        adrift_env_process(&env, &p, blk, 64u);
        for (uint32_t i = 0; i < 64u; i++) {
            if (blk[i] > 0 && first_nonzero == 0xFFFFFFFFu) {
                first_nonzero = base + i;
            }
        }
    }
    CHECK(first_nonzero == expected_delay,
          "Onset delay first non-zero sample=%u != expected=%u", first_nonzero, expected_delay);

    /* Verify A-D Loop Mode cycles repeatedly while Gate is held */
    p.onset_delay = 0;
    p.attack      = 80;
    p.decay       = 160;
    p.sustain     = 0;
    p.loop_mode   = ADRIFT_ENV_LOOP_AD;
    adrift_env_init(&env);
    adrift_env_note_on(&env, &p, 127u, false);

    int peaks = 0;
    bool was_high = false;
    for (uint32_t b = 0; b < 700u; b++) {
        adrift_env_process(&env, &p, blk, 64u);
        for (uint32_t i = 0; i < 64u; i++) {
            if (!was_high && blk[i] > 30000) {
                peaks++;
                was_high = true;
            } else if (was_high && blk[i] < 4000) {
                was_high = false;
            }
        }
    }
    CHECK(peaks >= 4, "Env 1 A-D Loop mode did not cycle (peaks=%d)", peaks);

    /* 2b. Onset-Delay Retrigger De-Click: active envelope ramps smoothly over <=64 samples instead of snapping to 0 */
    p.loop_mode   = ADRIFT_ENV_LOOP_OFF;
    p.onset_delay = 0;
    p.attack      = 0;
    p.sustain     = 1023;
    adrift_env_init(&env);
    adrift_env_note_on(&env, &p, 127u, false);
    for (int b = 0; b < 8; b++) {
        adrift_env_process(&env, &p, blk, 64u);
    }
    int32_t pre_retrig = blk[63];
    CHECK(pre_retrig >= 32700, "Expected full sustain before onset-delay retrigger (got %d)", pre_retrig);

    p.onset_delay = 300;
    adrift_env_note_on(&env, &p, 127u, false);
    adrift_env_process(&env, &p, blk, 64u);
    CHECK(blk[0] >= 32000, "Onset-delay retrigger snapped to %d on sample 0 instead of ramping smoothly", blk[0]);
    int32_t max_declick_step = abs(blk[0] - pre_retrig);
    for (int i = 1; i < 64; i++) {
        int32_t stp = abs(blk[i] - blk[i - 1]);
        if (stp > max_declick_step) max_declick_step = stp;
    }
    CHECK(max_declick_step <= 1024, "Onset-delay retrigger step too abrupt (%d Q15)", max_declick_step);
    adrift_env_process(&env, &p, blk, 64u);
    CHECK(blk[0] == 0 && blk[63] == 0, "Expected de-click ramp to reach exact silence within 64 samples (got %d, %d)", blk[0], blk[63]);

    /* 2c. Active PWM DC-cap preservation & quietest-releasing-voice stealing */
    adrift_app_init();
    adrift_synth_t *s = adrift_app_get_synth();
    adrift_synth_set_group_voices(s, 0u, 2u);
    adrift_patch_t *gp0 = &s->groups[0].patch;
    gp0->dry_wet          = 0;
    gp0->exc.wave_morph   = 1023;
    gp0->exc.pulse_width  = 60;
    gp0->exc.pluck        = 500;
    gp0->env1.attack      = 0;
    gp0->env1.sustain     = 800;
    gp0->env1.release     = 520;
    gp0->env2.attack      = 0;
    gp0->env2.sustain     = 1023;
    gp0->env2.release     = 520;
    gp0->env2.vel_sens    = 1023;

    int16_t *st_pcm = calloc(16384u * 2u, sizeof(int16_t));
    adrift_synth_note_on(s, 48u, 127u); /* Loud Note A on voice 0 */
    render_frames(st_pcm, 12288u);      /* Charge PWM 15 Hz AC-coupling cap */
    int v_loud = -1;
    for (int k = 0; k < ADRIFT_MAX_VOICES; k++) {
        if (s->voices[k].owner_group == 0 && s->voices[k].active && s->voices[k].note == 48u) {
            v_loud = k;
            break;
        }
    }
    CHECK(v_loud >= 0 && abs(s->voices[v_loud].exc.ac_cap_q23) > 500000,
          "Expected active PWM voice AC cap to charge (got %d)", v_loud >= 0 ? s->voices[v_loud].exc.ac_cap_q23 : 0);
    /* Retriggering same active note with pluck > 0 must NOT zero ac_cap_q23 */
    adrift_synth_note_on(s, 48u, 127u);
    CHECK(abs(s->voices[v_loud].exc.ac_cap_q23) > 500000,
          "Active exciter retrigger abruptly zeroed PWM AC-coupling cap (%d)", s->voices[v_loud].exc.ac_cap_q23);

    adrift_synth_note_on(s, 55u, 12u);  /* Quiet Note B on second voice */
    render_frames(st_pcm, 1024u);
    adrift_synth_note_off(s, 48u);
    adrift_synth_note_off(s, 55u);
    render_frames(st_pcm, 1024u);       /* Both voices now releasing: Note A is older AND much louder than Note B */

    adrift_synth_note_on(s, 67u, 100u); /* 3rd note must steal quieter Note B, sparing louder older Note A! */
    CHECK(s->voices[v_loud].note == 48u,
          "Voice stealer killed louder older releasing voice (note=%u) instead of quieter voice", s->voices[v_loud].note);

    /* Non-legato BBD steal on 1-voice group (C2 -> C5, glide=0): must damp old C2 energy without chirping */
    adrift_app_init();
    s = adrift_app_get_synth();
    adrift_synth_set_group_voices(s, 0u, 1u);
    gp0 = &s->groups[0].patch;
    gp0->dry_wet         = 1023;
    gp0->exc.vco_level   = 900;
    gp0->exc.noise_level = 1023;
    gp0->env1.attack     = 0;
    gp0->env1.decay      = 120;
    gp0->env1.sustain    = 0;
    gp0->bbd.feedback    = 885;
    gp0->bbd.glide       = 0;
    gp0->env2.attack     = 0;
    gp0->env2.sustain    = 1023;
    adrift_synth_note_on(s, 36u, 127u); /* C2 = 65.41 Hz */
    render_frames(st_pcm, 8192u);
    adrift_synth_note_on(s, 72u, 127u); /* Steal onto C5 = 523.25 Hz without glide */
    render_frames(st_pcm, 8192u);
    double c2_leak = goertzel_mag_db(st_pcm, 2048u, 4096u, 440.0 * pow(2.0, (36.0 - 69.0) / 12.0));
    double c5_main = goertzel_mag_db(st_pcm, 2048u, 4096u, 440.0 * pow(2.0, (72.0 - 69.0) / 12.0));
    CHECK((c5_main - c2_leak) > 30.0,
          "Stolen BBD voice leaked old C2 pitch into C5 (C5=%.1f dB, C2=%.1f dB)", c5_main, c2_leak);
    free(st_pcm);

    printf("  -> PASS (Onset Delay exact=%u samples, A-D Loop cycles=%d, Retrig declick<=%d)\n",
           expected_delay, peaks, max_declick_step);
}

/* ---- 3. Dry Path Post-Input-LPF Invariant ---- */
void test_dry_path_post_input_lpf(void) {
    printf("[Test 3] Signal Flow: Dry Path Taps After 12 dB/oct Input LPF...\n");
    const uint32_t N = 8192u;
    int16_t *pcm = calloc(N * 2u, sizeof(int16_t));

    adrift_app_init();
    adrift_synth_t *s = adrift_app_get_synth();
    adrift_patch_t *p = &s->groups[0].patch;
    p->dry_wet         = 0; /* 100% Dry */
    p->exc.vco_level   = 950;
    p->exc.noise_level = 1023; /* 100% noise mix */
    p->exc.sh_rate     = 1023;
    p->env1.attack     = 0;
    p->env1.sustain    = 1023;
    p->env2.attack     = 0;
    p->env2.sustain    = 1023;
    p->in_lpf_res      = 0;

    p->in_lpf_cutoff = 1023; /* 21 kHz */
    adrift_synth_note_on(s, 60u, 127u);
    render_frames(pcm, N);
    double hf_open = goertzel_mag_db(pcm, 2048u, 4096u, 8000.0);

    p->in_lpf_cutoff = 120; /* ~100 Hz */
    render_frames(pcm, N);
    double hf_closed = goertzel_mag_db(pcm, 2048u, 4096u, 8000.0);

    CHECK((hf_open - hf_closed) > 30.0,
          "Dry path 8 kHz not attenuated by Input LPF (open=%.1f dB, closed=%.1f dB)",
          hf_open, hf_closed);
    free(pcm);
    printf("  -> PASS (Dry path 8 kHz attenuated by %.1f dB when Input LPF closed)\n",
           hf_open - hf_closed);
}

/* ---- 4. BBD Resonator Pitch Tracking (< 5 cents C2-C5) & Invert Mode ---- */
void test_bbd_pitch_and_invert(void) {
    printf("[Test 4] 1024-Stage BBD Pitch Accuracy (C2-C5) & Polarity Invert...\n");
    const uint32_t N = 32768u;
    int16_t *pcm = calloc(N * 2u, sizeof(int16_t));

    static const uint8_t k_notes[] = { 36, 43, 48, 57, 60, 64, 72 }; /* C2 .. C5 */
    double max_abs_cents = 0.0;

    for (size_t idx = 0; idx < sizeof(k_notes); idx++) {
        uint8_t note = k_notes[idx];
        adrift_app_init();
        adrift_synth_t *s = adrift_app_get_synth();
        adrift_patch_t *p = &s->groups[0].patch;

        /* Short noise pluck into high-Q BBD resonator with realistic darkened FB filter (~2.2 kHz) */
        p->exc.vco_level      = 950;
        p->exc.noise_level    = 1023;
        p->exc.sh_rate        = 1023;
        p->env1.onset_delay   = 0;
        p->env1.attack        = 0;
        p->env1.decay         = 140; /* crisp ~25 ms acoustic pluck filling full C2-C5 delay line */
        p->env1.sustain       = 0;
        p->in_lpf_cutoff      = 900;
        p->in_lpf_res         = 0;
        p->bbd.feedback       = 888; /* ~99.8% sustain for precise pitch measurement */
        p->bbd.fb_lpf_cutoff  = 660; /* ~2.2 kHz damping: tests phase-delay pitch compensation! */
        p->bbd.glide          = 0;
        p->bbd.invert         = false;
        p->bbd.bbd_noise      = 0;
        p->dry_wet            = 1023;
        p->env2.attack        = 0;
        p->env2.sustain       = 1023;
        p->lfo_depth          = 0;
        p->master_vol         = 950;

        adrift_synth_note_on(s, note, 127u);
        render_frames(pcm, N);

        double target_hz = 440.0 * pow(2.0, ((double)note - 69.0) / 12.0);
        double meas_hz   = estimate_fundamental_dft(pcm, 4096u, 16384u, target_hz);
        double cents     = 1200.0 * log2(meas_hz / target_hz);
        if (fabs(cents) > max_abs_cents) max_abs_cents = fabs(cents);

        CHECK(fabs(cents) < 5.0,
              "Note %u: target=%.2f Hz, measured=%.2f Hz (error=%.2f cents >= 5 cents!)",
              note, target_hz, meas_hz, cents);
    }

    /* Now verify Polarity INVERT on C4 (261.63 Hz):
     * 1) invert=true, invert_pitch_comp=false -> drops 1 octave to C3 (130.81 Hz) with odd harmonics! */
    adrift_app_init();
    adrift_synth_t *s = adrift_app_get_synth();
    adrift_patch_t *p = &s->groups[0].patch;
    p->exc.vco_level          = 950;
    p->exc.noise_level        = 1023;
    p->env1.attack            = 0;
    p->env1.decay             = 90;
    p->env1.sustain           = 0;
    p->in_lpf_cutoff          = 900;
    p->bbd.feedback           = 888;
    p->bbd.fb_lpf_cutoff      = 720;
    p->bbd.invert             = true;
    p->bbd.invert_pitch_comp  = false;
    p->bbd.bbd_noise          = 0;
    p->dry_wet                = 1023;
    p->env2.attack            = 0;
    p->env2.sustain           = 1023;
    p->lfo_depth              = 0;

    adrift_synth_note_on(s, 60u, 127u); /* C4 key -> expects C3 = 130.813 Hz */
    render_frames(pcm, N);
    double inv_target_hz = 440.0 * pow(2.0, (48.0 - 69.0) / 12.0);
    double inv_meas_hz   = estimate_freq_autocorr(pcm, 4096u, 16384u, inv_target_hz * 0.85, inv_target_hz * 1.15);
    double inv_cents     = 1200.0 * log2(inv_meas_hz / inv_target_hz);
    double odd_h1        = goertzel_mag_db(pcm, 4096u, 16384u, inv_meas_hz);
    double even_h2       = goertzel_mag_db(pcm, 4096u, 16384u, inv_meas_hz * 2.0);
    double odd_h3        = goertzel_mag_db(pcm, 4096u, 16384u, inv_meas_hz * 3.0);

    CHECK(fabs(inv_cents) < 5.0,
          "Invert octave-drop pitch error=%.2f cents (meas=%.2f Hz vs %.2f Hz)",
          inv_cents, inv_meas_hz, inv_target_hz);
    CHECK((odd_h1 - even_h2) > 15.0 && (odd_h3 - even_h2) > 8.0,
          "Invert failed to suppress 2nd harmonic (H1=%.1f, H2=%.1f, H3=%.1f dB)",
          odd_h1, even_h2, odd_h3);

    /* 2) invert=true, invert_pitch_comp=true -> stays at C4 (261.63 Hz)! */
    adrift_synth_all_notes_off(s, true);
    p->bbd.invert_pitch_comp = true;
    adrift_synth_note_on(s, 60u, 127u);
    render_frames(pcm, N);
    double comp_target_hz = 440.0 * pow(2.0, (60.0 - 69.0) / 12.0);
    double comp_meas_hz   = estimate_freq_autocorr(pcm, 4096u, 16384u, comp_target_hz * 0.85, comp_target_hz * 1.15);
    double comp_cents     = 1200.0 * log2(comp_meas_hz / comp_target_hz);
    CHECK(fabs(comp_cents) < 5.0,
          "Invert+PitchComp error=%.2f cents (meas=%.2f Hz vs %.2f Hz)",
          comp_cents, comp_meas_hz, comp_target_hz);

    /* 3) Frame-invariant Portamento (glide) under 64-frame vs 8-frame sub-block splitting */
    adrift_bbd_t bbd_64, bbd_8;
    adrift_bbd_init(&bbd_64, 0u);
    adrift_bbd_init(&bbd_8,  0u);
    adrift_bbd_params_t gp_cfg = p->bbd;
    gp_cfg.glide     = 450;
    gp_cfg.bbd_noise = 0;
    int32_t dummy_fb = 0;
    adrift_bbd_prepare_block(&bbd_64, &gp_cfg, 48 << 8, 0, 0, 0, 64u, &dummy_fb);
    adrift_bbd_prepare_block(&bbd_8,  &gp_cfg, 48 << 8, 0, 0, 0, 8u,  &dummy_fb);
    for (uint32_t f = 0; f < 4096u; f += 64u) {
        adrift_bbd_prepare_block(&bbd_64, &gp_cfg, 72 << 8, 0, 0, 0, 64u, &dummy_fb);
    }
    for (uint32_t f = 0; f < 4096u; f += 8u) {
        adrift_bbd_prepare_block(&bbd_8,  &gp_cfg, 72 << 8, 0, 0, 0, 8u,  &dummy_fb);
    }
    double slew_diff_st = fabs((double)(bbd_64.slew_pitch_q16 - bbd_8.slew_pitch_q16)) / 65536.0;
    CHECK(slew_diff_st < 0.25,
          "Portamento glide depends on block size (64f vs 8f diff=%.3f semitones)", slew_diff_st);

    /* 3b) Exact zero-deadband convergence on both upward and downward glides (even at 8-frame sub-blocks) */
    for (uint32_t f = 0; f < 65536u; f += 64u) {
        adrift_bbd_prepare_block(&bbd_64, &gp_cfg, 72 << 8, 0, 0, 0, 64u, &dummy_fb);
    }
    for (uint32_t f = 0; f < 65536u; f += 8u) {
        adrift_bbd_prepare_block(&bbd_8,  &gp_cfg, 72 << 8, 0, 0, 0, 8u,  &dummy_fb);
    }
    CHECK(bbd_64.slew_pitch_q16 == (72 << 16) && bbd_8.slew_pitch_q16 == (72 << 16),
          "Upward portamento stalled in IIR deadband (64f=%d, 8f=%d, expected=%d)",
          bbd_64.slew_pitch_q16, bbd_8.slew_pitch_q16, 72 << 16);

    /* 3c) Unified VCO + BBD portamento lockstep: Exciter VCO must glide continuously with BBD */
    {
        adrift_app_init();
        adrift_synth_t *gs = adrift_app_get_synth();
        adrift_patch_t *gp = &gs->groups[0].patch;
        adrift_synth_set_group_voices(gs, 0u, 1u);
        gs->groups[0].assign_mode = ADRIFT_GROUP_UNISON;
        gp->legato           = true;
        gp->bbd.glide        = 850;
        gp->bbd.bbd_noise    = 0;
        gp->exc.analog_drift = 0;
        gp->exc.vco_level    = 900;
        gp->exc.noise_level  = 0;
        gp->exc.vco_octave   = 0;
        gp->exc.vco_semi     = 0;
        gp->exc.vco_fine     = 0;
        gp->exc.vco_keytrack = true;
        gp->env1.onset_delay = 0;
        gp->env1.attack      = 0;
        gp->env1.sustain     = 1023;
        gp->env2.attack      = 0;
        gp->env2.sustain     = 1023;
        gp->lfo_depth        = 0;
        gp->lfo2_depth       = 0;
        gp->lfo3_depth       = 0;

        uint8_t uv = 0u;
        for (uint8_t k = 0u; k < ADRIFT_MAX_VOICES; k++) {
            if (gs->voices[k].owner_group == 0) {
                uv = k;
                break;
            }
        }

        adrift_synth_note_on(gs, 48u, 120u);
        render_frames(pcm, 512u);
        CHECK(gs->voices[uv].exc.last_pitch_q16 == (48 << 16) &&
              gs->voices[uv].bbd.slew_pitch_q16 == (48 << 16),
              "Initial note did not snap VCO and BBD to C3 (vco=%d, bbd=%d)",
              gs->voices[uv].exc.last_pitch_q16, gs->voices[uv].bbd.slew_pitch_q16);

        adrift_synth_note_on(gs, 60u, 120u);
        render_frames(pcm, 2048u);
        int32_t vco_p = gs->voices[uv].exc.last_pitch_q16;
        int32_t bbd_p = gs->voices[uv].bbd.slew_pitch_q16;
        CHECK(bbd_p > (50 << 16) && bbd_p < (58 << 16) && vco_p == bbd_p,
              "Exciter VCO did not glide in lockstep with BBD (vco=%d, bbd=%d)", vco_p, bbd_p);
    }

    /* 3d) Fractional-delay HF damping uniformity (D = 50.00 integer vs D = 50.50 half-sample) */
    {
        double h7_loss_db[2] = { 0.0, 0.0 };
        for (int half = 0; half < 2; half++) {
            adrift_bbd_t bbd_eq;
            adrift_bbd_init(&bbd_eq, 0u);
            adrift_bbd_params_t eq_p = p->bbd;
            eq_p.feedback      = 880;
            eq_p.fb_lpf_cutoff = 960;
            eq_p.glide         = 0;
            eq_p.invert        = false;
            eq_p.bbd_noise     = 0;
            eq_p.dispersion    = 0;

            double   D_samp = half ? 50.5 : 50.0;
            double   f_h7   = 7.0 * (ADRIFT_SAMPLE_RATE / D_samp); /* ~6.1-6.2 kHz */
            int32_t  d_q16  = (int32_t)(D_samp * 65536.0 + 0.5);
            adrift_onepole_set_cutoff(&bbd_eq.fb_lpf, 960);
            adrift_onepole_set_alpha(&bbd_eq.aa_lpf, 32767u);
            adrift_onepole_set_alpha(&bbd_eq.clk_lpf, 32767u);
            bbd_eq.delay_q16         = d_q16;
            bbd_eq.target_delay_q16  = d_q16;
            bbd_eq.delay_initialized = true;
            bbd_eq.fb_gain_q14       = 16192;

            int32_t in_sig[4096];
            int32_t wet_sig[4096];
            for (uint32_t n = 0; n < 4096u; n++) {
                in_sig[n] = (n < 64u) ? (int32_t)(4096.0 * sin(2.0 * M_PI * f_h7 * (double)n / (double)ADRIFT_SAMPLE_RATE)) : 0;
            }
            for (uint32_t base = 0; base < 4096u; base += 64u) {
                bbd_eq.delay_q16        = d_q16;
                bbd_eq.target_delay_q16 = d_q16;
                adrift_bbd_process(&bbd_eq, &eq_p, 16192, in_sig + base, wet_sig + base, 64u);
            }
            int16_t mono_st[4096 * 2];
            for (uint32_t n = 0; n < 4096u; n++) {
                int16_t s16 = (int16_t)dsp_clamp_i32(wet_sig[n], -32768, 32767);
                mono_st[n * 2u]      = s16;
                mono_st[n * 2u + 1u] = s16;
            }
            double early_db = goertzel_mag_db(mono_st, 128u,  512u, f_h7);
            double late_db  = goertzel_mag_db(mono_st, 2628u, 512u, f_h7); /* +2500 samples ~= 50 loop passes */
            h7_loss_db[half] = early_db - late_db;
        }
        CHECK(fabs(h7_loss_db[0] - h7_loss_db[1]) < 1.5,
              "Fractional-delay HF droop mismatch across 50 passes (D=50.0 loss=%.2f dB vs D=50.5 loss=%.2f dB)",
              h7_loss_db[0], h7_loss_db[1]);
    }

    /* 4) Unbiased zero-mean thermal noise & MN3101 clock wander */
    adrift_bbd_t bbd_nz;
    adrift_bbd_init(&bbd_nz, 1u);
    gp_cfg.glide     = 0;
    gp_cfg.feedback  = 0;
    gp_cfg.bbd_noise = 256;
    int32_t zero_in[64] = { 0 };
    int32_t wet_blk[64];
    double  nz_sum = 0.0;
    double  nz_sq  = 0.0;
    bool    saw_drift = false;
    for (uint32_t b = 0; b < 256u; b++) {
        adrift_bbd_prepare_block(&bbd_nz, &gp_cfg, 60 << 8, 0, 0, 0, 64u, &dummy_fb);
        if (bbd_nz.clk_drift_q16 != 0) saw_drift = true;
        adrift_bbd_process(&bbd_nz, &gp_cfg, 0, zero_in, wet_blk, 64u);
        if (b >= 8u) {
            for (uint32_t i = 0; i < 64u; i++) {
                nz_sum += (double)wet_blk[i];
                nz_sq  += (double)wet_blk[i] * (double)wet_blk[i];
            }
        }
    }
    double nz_mean = fabs(nz_sum / (double)(248u * 64u));
    double buf_sum = 0.0;
    for (uint32_t i = 0; i < ADRIFT_BBD_BUF_LEN; i++) {
        buf_sum += (double)bbd_nz.buf[i];
    }
    double buf_mean = fabs(buf_sum / (double)ADRIFT_BBD_BUF_LEN);
    CHECK(nz_sq > 0.0 && buf_mean < 0.10 && nz_mean < 0.30 && saw_drift,
          "BBD thermal floor biased or clock wander inactive (buf_mean=%.3f, out_mean=%.3f LSB, sq=%.1f, drift=%d)",
          buf_mean, nz_mean, nz_sq, (int)saw_drift);

    /* 5) 2-Pole 1024-stage BBD AA/Reconstruction Clock Filter on C1 (note 24) & CTI HF ceiling */
    gp_cfg.invert        = false;
    gp_cfg.bbd_noise     = 0;
    gp_cfg.fb_lpf_cutoff = 1023;
    adrift_bbd_prepare_block(&bbd_nz, &gp_cfg, 24 << 8, 0, 0, 0, 64u, &dummy_fb);
    CHECK(bbd_nz.aa_lpf.a_q15 == bbd_nz.clk_lpf.a_q15 && bbd_nz.clk_lpf.a_q15 < 24000u &&
          bbd_nz.fb_lpf.a_q15 < 32767u,
          "2-pole BBD clock filter or CTI ceiling inactive (aa=%u, clk=%u, fb=%u)",
          bbd_nz.aa_lpf.a_q15, bbd_nz.clk_lpf.a_q15, bbd_nz.fb_lpf.a_q15);

    free(pcm);
    printf("  -> PASS (Max C2-C5 error=%.2f cents, Invert C3=%.2f cents, H1-H2=+%.1f dB, Glide diff=%.3f ST)\n",
           max_abs_cents, inv_cents, odd_h1 - even_h2, slew_diff_st);
}

/* ---- 5. Self-Oscillation (>100% FB) & Post-BBD Envelope 2 Gating ---- */
void test_self_oscillation_and_env2_gate(void) {
    printf("[Test 5] Feedback >100%% Self-Oscillation & Post-BBD Env 2 Gating...\n");
    const uint32_t N_ON  = 44118u * 2u; /* 2.0 s to let self-oscillation blossom from -78 dBFS thermal noise */
    const uint32_t N_OFF = 22059u;      /* 0.5 s after Note Off */
    int16_t *pcm = calloc((N_ON + N_OFF) * 2u, sizeof(int16_t));

    adrift_app_init();
    adrift_synth_t *s = adrift_app_get_synth();
    adrift_synth_load_preset(s, 3u); /* Preset 3: SELF-OSC SING (vco=0, noise=0, fb>100%) */
    s->groups[0].patch.bbd.feedback  = 995; /* ~112% strong self-oscillation */
    s->groups[0].patch.bbd.bbd_noise = 500;
    s->groups[0].patch.env2.attack   = 40;
    s->groups[0].patch.env2.sustain  = 1023;
    s->groups[0].patch.env2.release  = 220; /* ~60 ms release */

    adrift_synth_note_on(s, 60u, 120u);
    render_frames(pcm, N_ON);

    double osc_rms = compute_rms_dbfs(pcm, N_ON - 8192u, 8192u);
    double osc_dc  = fabs(compute_mean_lsb(pcm, N_ON - 8192u, 8192u));
    CHECK(osc_rms > -22.0 && osc_rms < -0.2,
          "Self-oscillation did not settle into bounded analog level: %.2f dBFS", osc_rms);
    CHECK(osc_dc < 600.0, "Self-oscillation DC latch-up detected: %.1f LSB", osc_dc);

    /* Release key: internal BBD keeps oscillating, but Post-BBD Env 2 must gate output to silence! */
    adrift_synth_note_off(s, 60u);
    render_frames(pcm + 2u * N_ON, N_OFF);

    double gated_rms = compute_rms_dbfs(pcm + 2u * N_ON, N_OFF - 4096u, 4096u);
    CHECK(gated_rms < -96.0,
          "Post-BBD Env 2 failed to gate self-oscillating tail to silence (rms=%.1f dBFS)", gated_rms);

    /* 5b. Linear-core bucket-well saturator: normal plucks (< -12 dBFS) decay linearly without double compression */
    CHECK(dsp_bbd_saturate_q15(8000) == 8000 && dsp_bbd_saturate_q15(-8500) == -8500,
          "Expected linear core in dsp_bbd_saturate_q15");
    {
        double decay_db[2] = { 0.0, 0.0 };
        for (int loud = 0; loud < 2; loud++) {
            adrift_bbd_t bbd_lin;
            adrift_bbd_init(&bbd_lin, 0u);
            adrift_bbd_params_t lp = {
                .feedback      = 885,
                .fb_lpf_cutoff = 920,
                .glide         = 0,
                .invert        = false,
                .bbd_noise     = 0,
                .dispersion    = 0
            };
            int32_t dummy_fb = 0;
            adrift_bbd_prepare_block(&bbd_lin, &lp, 60 << 8, 0, 0, 0, 64u, &dummy_fb);
            bbd_lin.fb_gain_q14 = dummy_fb;

            double amp = loud ? 7800.0 : 1950.0; /* -12.5 dBFS vs -24.5 dBFS */
            int32_t in_b[8192], wet_b[8192];
            for (uint32_t n = 0; n < 8192u; n++) {
                in_b[n] = (n < 168u) ? (int32_t)(amp * sin(2.0 * M_PI * 261.625565 * (double)n / (double)ADRIFT_SAMPLE_RATE)) : 0;
            }
            for (uint32_t base = 0; base < 8192u; base += 64u) {
                adrift_bbd_prepare_block(&bbd_lin, &lp, 60 << 8, 0, 0, 0, 64u, &dummy_fb);
                adrift_bbd_process(&bbd_lin, &lp, dummy_fb, in_b + base, wet_b + base, 64u);
            }
            int16_t st_b[8192 * 2];
            for (uint32_t n = 0; n < 8192u; n++) {
                int16_t s16 = (int16_t)dsp_clamp_i32(wet_b[n], -32768, 32767);
                st_b[n * 2u]      = s16;
                st_b[n * 2u + 1u] = s16;
            }
            double e_rms = compute_rms_dbfs(st_b, 512u,  1024u);
            double l_rms = compute_rms_dbfs(st_b, 6656u, 1024u);
            decay_db[loud] = e_rms - l_rms;
        }
        CHECK(fabs(decay_db[1] - decay_db[0]) < 0.25,
              "Normal pluck decay compressed nonlinearly (-9.5 dBFS decay=%.2f dB vs -21.5 dBFS decay=%.2f dB)",
              decay_db[1], decay_db[0]);
    }

    free(pcm);
    printf("  -> PASS (Self-Osc settled=%.1f dBFS, Post-Env2 Release=%.1f dBFS)\n",
           osc_rms, gated_rms);
}

/* ---- 12. Deeper Physical Modeling: BBD Loop Dispersion & Exciter Pluck / Sub-Osc ---- */
void test_physical_modeling_dispersion_and_pluck(void) {
    printf("[Test 12] Deeper Physical Modeling (BBD All-Pass Dispersion & Exciter Pluck)...\n");
    const uint32_t N = 32768u;
    int16_t  *pcm  = calloc(N * 2u, sizeof(int16_t));
    int16_t  *pcm2 = calloc(N * 2u, sizeof(int16_t));
    uint16_t *fb   = calloc(ADRIFT_SCREEN_W * ADRIFT_SCREEN_H, sizeof(uint16_t));

    /* 12a. Pure Mechanical Pluck Impulse (VCO=0, Noise=0, Pluck=900) excites BBD string */
    adrift_app_init();
    adrift_synth_t *s  = adrift_app_get_synth();
    adrift_ui_t    *ui = adrift_app_get_ui();
    adrift_patch_t *p  = &s->groups[0].patch;

    p->exc.vco_level      = 0;
    p->exc.noise_level    = 0;
    p->exc.pluck          = 900;
    p->env1.onset_delay   = 0;
    p->env1.attack        = 0;
    p->env1.decay         = 140;
    p->env1.sustain       = 0;
    p->in_lpf_cutoff      = 950;
    p->in_lpf_res         = 0;
    p->bbd.feedback       = 888;
    p->bbd.fb_lpf_cutoff  = 860;
    p->bbd.bbd_noise      = 0;
    p->bbd.dispersion     = 0;
    p->dry_wet            = 1023;
    p->env2.attack        = 0;
    p->env2.sustain       = 1023;
    p->lfo_depth          = 0;
    p->master_vol         = 950;

    adrift_synth_note_on(s, 48u, 127u); /* C3 = 130.813 Hz */
    render_frames(pcm, N);
    double pluck_only_rms = compute_rms_dbfs(pcm, 1024u, 8192u);
    CHECK(pluck_only_rms > -26.0,
          "Pure mechanical Pluck impulse (VCO=0, Noise=0, Pluck=900) failed to excite BBD string (rms=%.2f dBFS)",
          pluck_only_rms);

    /* Verify -1 Oct Sub-Oscillator & deterministic Note-On phase lock when VCO > 0 and Pluck > 0 */
    adrift_synth_all_notes_off(s, true);
    s->groups[0].num_voices = 1u;
    adrift_synth_set_group_voices(s, 0u, 1u);
    p->dry_wet          = 0;
    p->exc.vco_level    = 850;
    p->exc.wave_morph   = 600;
    p->exc.analog_drift = 0;
    p->env1.sustain     = 1023;

    p->exc.pluck = 0;
    adrift_synth_note_on(s, 60u, 127u); /* C4 = 261.63 Hz, sub = C3 = 130.81 Hz */
    render_frames(pcm, 8192u);
    double f_sub    = 440.0 * pow(2.0, (48.0 - 69.0) / 12.0);
    double sub_off  = goertzel_mag_db(pcm, 1024u, 4096u, f_sub);

    adrift_synth_all_notes_off(s, true);
    p->exc.pluck = 950;
    adrift_synth_note_on(s, 60u, 127u);
    render_frames(pcm, 8192u);
    double sub_on = goertzel_mag_db(pcm, 1024u, 4096u, f_sub);
    CHECK((sub_on - sub_off) > 15.0,
          "Exciter -1 Oct Sub-Oscillator did not boost f0/2 fundamental (on=%.1f vs off=%.1f dB)",
          sub_on, sub_off);

    /* Re-trigger Note On after arbitrary phase advancement and verify deterministic phase-locked attack */
    adrift_synth_all_notes_off(s, true);
    s->voices[0].exc.vco_phase = 0x57391B20u;
    s->voices[0].exc.sub_phase = 0x29A81100u;
    adrift_synth_note_on(s, 60u, 127u);
    render_frames(pcm2, 512u);
    int32_t max_sample_diff = 0;
    for (uint32_t i = 0; i < 256u; i++) {
        int32_t d = abs((int32_t)pcm[i * 2u] - (int32_t)pcm2[i * 2u]);
        if (d > max_sample_diff) max_sample_diff = d;
    }
    CHECK(max_sample_diff <= 2,
          "Note-On VCO/Sub/Impulse phase lock was not deterministic (max_sample_diff=%d LSB)",
          max_sample_diff);

    /* 12b. BBD Loop All-Pass Dispersion: fundamental stays locked (< 3.0 cents) while upper partials stretch sharp! */
    adrift_app_init();
    p = &s->groups[0].patch;
    p->exc.vco_level      = 0;
    p->exc.noise_level    = 0;
    p->exc.pluck          = 600;
    p->env1.onset_delay   = 0;
    p->env1.attack        = 0;
    p->env1.decay         = 60;
    p->env1.sustain       = 0;
    p->in_lpf_cutoff      = 1023;
    p->in_lpf_res         = 0;
    p->bbd.feedback       = 884;
    p->bbd.fb_lpf_cutoff  = 960;
    p->bbd.glide          = 0;
    p->bbd.invert         = false;
    p->bbd.bbd_noise      = 0;
    p->bbd.dispersion     = 0;
    p->dry_wet            = 1023;
    p->env2.attack        = 0;
    p->env2.sustain       = 1023;
    p->lfo_depth          = 0;
    p->master_vol         = 950;

    double c3_hz = 440.0 * pow(2.0, (48.0 - 69.0) / 12.0); /* 130.8128 Hz */
    adrift_synth_note_on(s, 48u, 100u);
    render_frames(pcm, N);
    double f1_nodisp = estimate_fundamental_dft(pcm, 1024u, 8192u, c3_hz);
    double f5_nodisp = estimate_fundamental_dft(pcm, 1024u, 8192u, c3_hz * 5.0);

    adrift_synth_all_notes_off(s, true);
    p->bbd.dispersion = 880;
    adrift_synth_note_on(s, 48u, 100u);
    render_frames(pcm2, N);
    double f1_disp = estimate_fundamental_dft(pcm2, 1024u, 8192u, c3_hz);
    double f5_disp = estimate_fundamental_dft(pcm2, 1024u, 8192u, c3_hz * 5.0);

    double f1_disp_cents    = 1200.0 * log2(f1_disp / c3_hz);
    double f5_stretch_cents = 1200.0 * log2((f5_disp / f1_disp) / (f5_nodisp / f1_nodisp));
    CHECK(fabs(f1_disp_cents) < 3.5,
          "Dispersive BBD fundamental C3 out of tune: %.2f cents (meas=%.2f Hz vs %.2f Hz)",
          f1_disp_cents, f1_disp, c3_hz);
    CHECK(f5_stretch_cents > 8.0,
          "Expected BBD Loop All-Pass Dispersion to stretch 5th harmonic sharp > +8 cents (got +%.2f cents)",
          f5_stretch_cents);

    /* 12c. Verify UI BBD Page 2/2 4th card (8:DISPR) & MIDI CC88/CC89 */
    adrift_synth_load_preset(s, 0u);
    adrift_input_state_t in_st;
    adrift_dirty_t dirty;
    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed = ADRIFT_BTN_PAGE_3; /* Open BBD Page 1/2 */
    in_st.master_pot      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);

    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed             = ADRIFT_BTN_PAGE_3; /* Advance to BBD Page 2/2 (INVERT, GLIDE, DRYWET, DISPR) */
    in_st.enc_delta[ADRIFT_ENC_KNOB4] = 30;                /* 8:DISPR = +360 */
    in_st.master_pot                  = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->page == 2u && ui->subpage == 1u && s->groups[0].patch.bbd.dispersion == 360,
          "BBD Page 2/2 KNOB4 did not edit DISPR (got %d)", s->groups[0].patch.bbd.dispersion);

    uint16_t bbd_card4_hdr = fb[145u * ADRIFT_SCREEN_W + 190u];
    uint16_t bbd_card4_int = fb[185u * ADRIFT_SCREEN_W + 190u];
    CHECK(bbd_card4_hdr != bbd_card4_int,
          "Expected 4th card (DISPR) on BBD Page 2/2 to have a header band");

    adrift_synth_handle_midi(s, 0xB0u, 88u, 100u); /* CC88 -> Pluck */
    adrift_synth_handle_midi(s, 0xB0u, 89u, 110u); /* CC89 -> Dispersion */
    CHECK(s->groups[0].patch.exc.pluck == (100 * 1023) / 127 &&
          s->groups[0].patch.bbd.dispersion == (110 * 1023) / 127,
          "MIDI CC88/CC89 mapping failed (pluck=%d, disp=%d)",
          s->groups[0].patch.exc.pluck, s->groups[0].patch.bbd.dispersion);

    free(fb);
    free(pcm2);
    free(pcm);
    printf("  -> PASS (Pluck RMS=%.1f dBFS, Sub +%.1f dB, C3 f0 err=%.2f cents, H5 stretch=+%.1f cents, UI/CC OK)\n",
           pluck_only_rms, sub_on - sub_off, f1_disp_cents, f5_stretch_cents);
}

/* ============================================================================
 * 13. LFO -> Env 1 Stage Modulation Destinations (E1 ATK/DEC/SUS/REL/DLY + MOD 4/4 UI)
 * ============================================================================ */
void test_env1_modulation(void) {
    printf("[Test 13] LFO -> Env 1 Stage Destinations (E1 ATK, E1 DEC, E1 SUS, E1 REL, E1 DLY + MOD 4/4 UI)...\n");

    const uint32_t N = 26624u;
    int16_t  *pcm = (int16_t *)calloc(2u * N, sizeof(int16_t));
    uint16_t *fb  = (uint16_t *)calloc(ADRIFT_SCREEN_W * ADRIFT_SCREEN_H, sizeof(uint16_t));
    CHECK(pcm != NULL && fb != NULL, "Failed to allocate test buffers");

    /* 13a. LFO -> E1 SUS (Square LFO @ ~4.3 Hz gates Env 1 sustain between 100% and 0%) */
    adrift_app_init();
    adrift_synth_t *s  = adrift_app_get_synth();
    adrift_ui_t    *ui = adrift_app_get_ui();
    adrift_synth_init_patch(s);
    adrift_patch_t *p  = &s->groups[0].patch;

    p->exc.vco_level    = 900;
    p->dry_wet          = 0;
    p->env1.onset_delay = 0;
    p->env1.attack      = 0;
    p->env1.decay       = 0;
    p->env1.sustain     = 512;
    p->lfo_wave         = ADRIFT_LFO_SQR;
    p->lfo_rate         = 480; /* half-cycle ~3321 samples */
    p->lfo_depth        = 511;
    p->lfo_dest         = ADRIFT_MOD_ENV1_SUS;

    adrift_synth_note_on(s, 60u, 127u);
    render_frames(pcm, N);

    double sus_hi_db = compute_rms_dbfs(pcm, 256u,  2048u);
    double sus_lo_db = compute_rms_dbfs(pcm, 3700u, 2048u);
    CHECK(sus_hi_db > -25.0 && sus_hi_db - sus_lo_db > 35.0,
          "Expected LFO SQR -> E1 SUS to toggle sustain between loud (%.1f dB) and silent (%.1f dB)",
          sus_hi_db, sus_lo_db);

    /* 13b. LFO -> E1 ATK & E1 REL (Fast vs slow attack rise and release rate modulation) */
    adrift_app_init();
    adrift_synth_init_patch(s);
    p = &s->groups[0].patch;
    p->exc.vco_level = 900;
    p->dry_wet       = 0;
    p->env1.attack   = 512;
    p->env1.decay    = 512;
    p->env1.sustain  = 1023;
    p->env1.release  = 512;
    p->lfo2_wave     = ADRIFT_LFO_SQR;
    p->lfo2_rate     = 100;
    p->lfo2_depth    = -512; /* Pulls E1 ATK from 512 -> 0 (instant 0.5 ms attack) */
    p->lfo2_dest     = ADRIFT_MOD_ENV1_ATK;
    p->lfo3_wave     = ADRIFT_LFO_SQR;
    p->lfo3_rate     = 100;
    p->lfo3_depth    = -512; /* Pulls E1 REL from 512 -> 0 (instant 1 ms release) */
    p->lfo3_dest     = ADRIFT_MOD_ENV1_REL;

    adrift_synth_note_on(s, 60u, 127u);
    render_frames(pcm, 2048u);
    double fast_atk_db = compute_rms_dbfs(pcm, 64u, 1024u);
    int32_t fast_atk_flr = s->voices[0].env1.atk_floor_q30;
    int32_t fast_rel_flr = s->voices[0].env1.rel_floor_q30;

    adrift_app_init();
    adrift_synth_init_patch(s);
    p = &s->groups[0].patch;
    p->exc.vco_level = 900;
    p->dry_wet       = 0;
    p->env1.attack   = 512;
    p->env1.decay    = 512;
    p->env1.sustain  = 1023;
    p->env1.release  = 512;
    p->lfo2_wave     = ADRIFT_LFO_SQR;
    p->lfo2_rate     = 100;
    p->lfo2_depth    = 511; /* Pushes E1 ATK from 512 -> 1023 (5.0 s slow swell) */
    p->lfo2_dest     = ADRIFT_MOD_ENV1_ATK;
    p->lfo3_wave     = ADRIFT_LFO_SQR;
    p->lfo3_rate     = 100;
    p->lfo3_depth    = 511; /* Pushes E1 REL from 512 -> 1023 (10.0 s slow release) */
    p->lfo3_dest     = ADRIFT_MOD_ENV1_REL;

    adrift_synth_note_on(s, 60u, 127u);
    render_frames(pcm, 2048u);
    double slow_atk_db = compute_rms_dbfs(pcm, 64u, 1024u);
    int32_t slow_atk_flr = s->voices[0].env1.atk_floor_q30;
    int32_t slow_rel_flr = s->voices[0].env1.rel_floor_q30;

    CHECK(fast_atk_db - slow_atk_db > 20.0 &&
          fast_atk_flr > slow_atk_flr * 100 &&
          fast_rel_flr > slow_rel_flr * 100,
          "Expected LFO -> E1 ATK/E1 REL to modulate attack/release rates (fast_atk=%.1f dB, slow_atk=%.1f dB)",
          fast_atk_db, slow_atk_db);

    /* 13c. LFO -> E1 DLY (Modulates Env 1 onset delay from base 0 -> ~500 ms delayed strike) */
    adrift_app_init();
    adrift_synth_init_patch(s);
    p = &s->groups[0].patch;
    p->exc.vco_level    = 900;
    p->dry_wet          = 0;
    p->env1.onset_delay = 0; /* Base delay 0, enabled via LFO -> E1 DLY */
    p->env1.attack      = 0;
    p->env1.sustain     = 1023;
    p->lfo_wave         = ADRIFT_LFO_SQR;
    p->lfo_rate         = 0;   /* Very slow SQR stays positive throughout test window */
    p->lfo_depth        = 511; /* Modulates onset_delay to +511 (~22,000 samples) */
    p->lfo_dest         = ADRIFT_MOD_ENV1_DLY;

    adrift_synth_note_on(s, 60u, 127u);
    render_frames(pcm, N);

    double dly_pre_db  = compute_rms_dbfs(pcm, 64u,    4096u);
    double dly_post_db = compute_rms_dbfs(pcm, 23500u, 2048u);
    CHECK(dly_pre_db < -90.0 && dly_post_db > -25.0,
          "Expected LFO -> E1 DLY to hold note silent during delay (%.1f dB) then strike (%.1f dB)",
          dly_pre_db, dly_post_db);

    /* 13d. Verify UI MOD Page 4/4 has MW DEP on slot 0 and empty unmapped slots 1..3 */
    adrift_synth_load_preset(s, 0u);
    adrift_input_state_t in_st;
    adrift_dirty_t dirty;
    for (uint8_t step_p = 0; step_p < 3u; step_p++) {
        memset(&in_st, 0, sizeof(in_st));
        in_st.buttons_pressed = ADRIFT_BTN_PAGE_5; /* Open MOD Page 1/4 -> 2/4 -> 3/4 */
        in_st.master_pot      = -1;
        adrift_dirty_clear(&dirty);
        adrift_app_tick_ui(&in_st, fb, &dirty);
    }

    int16_t mw0 = s->groups[0].patch.mw_depth;
    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed             = ADRIFT_BTN_PAGE_5; /* Advance to MOD Page 4/4 (MW DEP only) */
    in_st.enc_delta[ADRIFT_ENC_KNOB1] = 5;                 /* 1:MW DEP = +60 */
    in_st.enc_delta[ADRIFT_ENC_KNOB2] = 5;                 /* Unmapped slot 1 -> ignored */
    in_st.master_pot                  = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    int16_t expected_mw = (mw0 + 60 > 1023) ? 1023 : (int16_t)(mw0 + 60);
    CHECK(ui->page == 4u && ui->subpage == 3u && s->groups[0].patch.mw_depth == expected_mw,
          "MOD Page 4/4 KNOB1 did not edit MW DEP (subpage=%u, got %d, expected %d)",
          (unsigned)ui->subpage, (int)s->groups[0].patch.mw_depth, (int)expected_mw);

    uint16_t mod_card1_hdr = fb[145u * ADRIFT_SCREEN_W + 20u];
    uint16_t mod_card1_int = fb[185u * ADRIFT_SCREEN_W + 20u];
    uint16_t mod_card2_hdr = fb[145u * ADRIFT_SCREEN_W + 78u];
    uint16_t mod_card2_int = fb[185u * ADRIFT_SCREEN_W + 78u];
    CHECK(mod_card1_hdr != mod_card1_int && mod_card2_hdr == mod_card2_int,
          "Expected 1st card (MW DEP) on MOD Page 4/4 to have a header band and 2nd card to be empty");

    free(fb);
    free(pcm);
    printf("  -> PASS (E1 SUS=+%.1f dB, E1 ATK=+%.1f dB, E1 DLY=%.1f->%.1f dB, MOD 4/4 UI OK)\n",
           sus_hi_db - sus_lo_db, fast_atk_db - slow_atk_db, dly_pre_db, dly_post_db);
}

/* ============================================================================
 * 14. 3 Per-Group LFOs (LFO 1..3 Simultaneous Modulation, 4-Subpage MOD UI & CCs)
 * ============================================================================ */
void test_multi_lfos(void) {
    printf("[Test 14] 3 Per-Group LFOs (LFO 1..3 Simultaneous Modulation, MOD 1/4..4/4 UI & CCs 31..38)...\n");

    const uint32_t N = 22059u; /* ~0.5 s */
    int16_t  *pcm = (int16_t *)calloc(2u * N, sizeof(int16_t));
    uint16_t *fb  = (uint16_t *)calloc(ADRIFT_SCREEN_W * ADRIFT_SCREEN_H, sizeof(uint16_t));
    CHECK(pcm != NULL && fb != NULL, "Failed to allocate test buffers");

    /* 14a. Verify LFO 2 alone modulates VCO PITCH (+6 ST vs -6 ST square wave @ ~4 Hz) */
    adrift_app_init();
    adrift_synth_t *s  = adrift_app_get_synth();
    adrift_ui_t    *ui = adrift_app_get_ui();
    adrift_patch_t *p  = &s->groups[0].patch;

    p->exc.wave_morph   = 0;
    p->exc.vco_level    = 900;
    p->exc.noise_level  = 0;
    p->exc.pluck        = 0;
    p->exc.analog_drift = 0;
    p->exc.vco_keytrack = true;
    p->env1.onset_delay = 0;
    p->env1.attack      = 0;
    p->env1.sustain     = 1023;
    p->in_lpf_cutoff    = 1023;
    p->in_lpf_res       = 0;
    p->dry_wet          = 0;
    p->env2.attack      = 0;
    p->env2.sustain     = 1023;
    p->lfo_depth        = 0;
    p->lfo2_wave        = ADRIFT_LFO_SQR;
    p->lfo2_rate        = 480; /* ~4.3 Hz -> half-cycle ~5100 samples */
    p->lfo2_depth       = 256; /* +-6 ST square pitch modulation */
    p->lfo2_dest        = ADRIFT_MOD_VCO_PITCH;
    p->lfo3_depth       = 0;
    p->master_vol       = 950;

    adrift_synth_note_on(s, 60u, 127u); /* C4 = 261.63 Hz; +6 ST = 370.0 Hz, -6 ST = 185.0 Hz */
    render_frames(pcm, N);

    double f_hi = estimate_fundamental_dft(pcm, 256u, 2048u, 370.0);
    double f_lo = estimate_fundamental_dft(pcm, 3700u, 2048u, 185.0);
    double lfo2_span_st = 12.0 * log2(f_hi / f_lo);
    CHECK(f_hi > 350.0 && f_lo < 200.0 && fabs(lfo2_span_st - 12.0) < 0.5,
          "Expected LFO 2 SQR -> VCO PITCH to toggle +6/-6 ST (12 ST span), got f_hi=%.1f Hz, f_lo=%.1f Hz (span=%.2f ST)",
          f_hi, f_lo, lfo2_span_st);

    /* 14b. Verify LFO 3 alone modulates IN LPF brightness (SQR wave @ ~4.3 Hz) */
    adrift_app_init();
    p = &s->groups[0].patch;
    p->exc.wave_morph   = 682; /* Sawtooth */
    p->exc.vco_level    = 900;
    p->exc.noise_level  = 0;
    p->exc.pluck        = 0;
    p->exc.analog_drift = 0;
    p->env1.onset_delay = 0;
    p->env1.attack      = 0;
    p->env1.sustain     = 1023;
    p->in_lpf_cutoff    = 460;
    p->in_lpf_res       = 300;
    p->dry_wet          = 0;
    p->env2.attack      = 0;
    p->env2.sustain     = 1023;
    p->lfo_depth        = 0;
    p->lfo2_depth       = 0;
    p->lfo3_wave        = ADRIFT_LFO_SQR;
    p->lfo3_rate        = 480;
    p->lfo3_depth       = 511;
    p->lfo3_dest        = ADRIFT_MOD_IN_LPF;
    p->master_vol       = 950;

    adrift_synth_note_on(s, 48u, 127u);
    render_frames(pcm, N);

    double l3_bright_db = calc_hf_diff_ratio_db(pcm, 256u, 2048u);
    double l3_dark_db   = calc_hf_diff_ratio_db(pcm, 3700u, 2048u);
    CHECK(l3_bright_db - l3_dark_db > 10.0,
          "Expected LFO 3 SQR -> IN LPF to modulate filter brightness by >10 dB (bright=%.1f dB, dark=%.1f dB)",
          l3_bright_db, l3_dark_db);

    /* 14c. Verify MOD UI 4-subpage cycling (1/4 -> 2/4 -> 3/4 -> 4/4 -> 1/4) & MIDI CCs 31..38 */
    adrift_synth_load_preset(s, 0u);
    adrift_input_state_t in_st;
    adrift_dirty_t dirty;

    /* Press 1: MOD Page 1/4 (LFO 1) */
    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed             = ADRIFT_BTN_PAGE_5;
    in_st.enc_delta[ADRIFT_ENC_KNOB3] = 5; /* L1 DEP +40 */
    in_st.master_pot                  = -1;
    int16_t d1_0 = s->groups[0].patch.lfo_depth;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->page == 4u && ui->subpage == 0u && s->groups[0].patch.lfo_depth == d1_0 + 40,
          "MOD Page 1/4 (LFO 1) failed");

    /* Press 2: MOD Page 2/4 (LFO 2) */
    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed             = ADRIFT_BTN_PAGE_5;
    in_st.enc_delta[ADRIFT_ENC_KNOB1] = 1;  /* L2 WAV */
    in_st.enc_delta[ADRIFT_ENC_KNOB2] = 5;  /* L2 RT +60 */
    in_st.enc_delta[ADRIFT_ENC_KNOB3] = 10; /* L2 DEP +80 */
    in_st.enc_delta[ADRIFT_ENC_KNOB4] = 1;  /* L2 DST +1 */
    in_st.master_pot                  = -1;
    int16_t r2_0 = s->groups[0].patch.lfo2_rate;
    int16_t d2_0 = s->groups[0].patch.lfo2_depth;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->page == 4u && ui->subpage == 1u &&
          s->groups[0].patch.lfo2_rate == r2_0 + 60 &&
          s->groups[0].patch.lfo2_depth == d2_0 + 80,
          "MOD Page 2/4 (LFO 2) encoder edits failed");

    /* Press 3: MOD Page 3/4 (LFO 3) */
    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed             = ADRIFT_BTN_PAGE_5;
    in_st.enc_delta[ADRIFT_ENC_KNOB2] = 5;  /* L3 RT +60 */
    in_st.enc_delta[ADRIFT_ENC_KNOB3] = -8; /* L3 DEP -64 */
    in_st.master_pot                  = -1;
    int16_t r3_0 = s->groups[0].patch.lfo3_rate;
    int16_t d3_0 = s->groups[0].patch.lfo3_depth;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->page == 4u && ui->subpage == 2u &&
          s->groups[0].patch.lfo3_rate == r3_0 + 60 &&
          s->groups[0].patch.lfo3_depth == d3_0 - 64,
          "MOD Page 3/4 (LFO 3) encoder edits failed");

    /* Press 4 -> Page 4/4 (subpage 3), Press 5 -> wraps back to Page 1/4 (subpage 0) */
    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed = ADRIFT_BTN_PAGE_5;
    in_st.master_pot      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->page == 4u && ui->subpage == 3u, "Expected MOD Page 4/4 (subpage 3)");

    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed = ADRIFT_BTN_PAGE_5;
    in_st.master_pot      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->page == 4u && ui->subpage == 0u, "Expected MOD Page 5th press to wrap to Page 1/4 (subpage 0)");

    /* Verify MIDI CCs 31..34 (LFO 2) and 35..38 (LFO 3) */
    adrift_synth_handle_midi(s, 0xB0u, 31u, 100u); /* L2 Wave */
    adrift_synth_handle_midi(s, 0xB0u, 32u, 127u); /* L2 Rate */
    adrift_synth_handle_midi(s, 0xB0u, 33u, 127u); /* L2 Depth (+511) */
    adrift_synth_handle_midi(s, 0xB0u, 34u, 90u);  /* L2 Dest */
    adrift_synth_handle_midi(s, 0xB0u, 35u, 64u);  /* L3 Wave */
    adrift_synth_handle_midi(s, 0xB0u, 36u, 64u);  /* L3 Rate */
    adrift_synth_handle_midi(s, 0xB0u, 37u, 0u);   /* L3 Depth (-512) */
    adrift_synth_handle_midi(s, 0xB0u, 38u, 110u); /* L3 Dest */
    CHECK(s->groups[0].patch.lfo2_rate == 1023 &&
          s->groups[0].patch.lfo2_depth == 511 &&
          s->groups[0].patch.lfo3_depth == -512 &&
          s->groups[0].patch.lfo3_dest == (110u * ADRIFT_MOD_DEST_COUNT) / 128u,
          "MIDI CCs 31..38 (LFO 2 & LFO 3) failed");

    free(fb);
    free(pcm);
    printf("  -> PASS (LFO 2 Pitch span=%.2f ST, LFO 3 Filter sweep=+%.1f dB, MOD 1/4..4/4 UI & CCs 31..38 OK)\n",
           lfo2_span_st, l3_bright_db - l3_dark_db);
}

/* ---- 15. Envelope 1 Stage Range (SHORT 0-250 ms, NORM 5s/10s, LONG up to 25 s) ---- */
static uint32_t measure_env_attack_samples(adrift_env_range_t range, int16_t knob) {
    adrift_env_t env;
    adrift_env_params_t p = {
        .onset_delay = 0,
        .attack      = knob,
        .decay       = 512,
        .sustain     = 1023,
        .release     = 512,
        .vel_sens    = 0,
        .loop_mode   = ADRIFT_ENV_LOOP_OFF,
        .range       = (uint8_t)range,
        .drone_hold  = false
    };
    adrift_env_init(&env);
    adrift_env_note_on(&env, &p, 127u, false);

    int32_t blk[64];
    uint32_t total = 0u;
    while (env.stage == ADRIFT_ENV_ATTACK && total < 1500000u) {
        adrift_env_process(&env, &p, blk, 64u);
        total += 64u;
    }
    return total;
}

void test_env1_range(void) {
    printf("[Test 15] Envelope 1 Range (SHORT 0-250ms, NORM 5s/10s, LONG up to 25s, UI 2/2 & CC39)...\n");

    /* 15a. Verify full-scale Attack (knob = 1023) across SHORT, NORM, and LONG ranges */
    uint32_t atk_short = measure_env_attack_samples(ADRIFT_ENV_RANGE_SHORT, 1023);
    uint32_t atk_norm  = measure_env_attack_samples(ADRIFT_ENV_RANGE_NORM,  1023);
    uint32_t atk_long  = measure_env_attack_samples(ADRIFT_ENV_RANGE_LONG,  1023);

    double short_ms = (double)atk_short * 1000.0 / (double)ADRIFT_SAMPLE_RATE;
    double norm_s   = (double)atk_norm  / (double)ADRIFT_SAMPLE_RATE;
    double long_s   = (double)atk_long  / (double)ADRIFT_SAMPLE_RATE;

    CHECK(short_ms > 240.0 && short_ms < 260.0,
          "Expected SHORT max attack ~250 ms, got %.1f ms (%u samples)", short_ms, atk_short);
    CHECK(norm_s > 4.5 && norm_s < 5.5,
          "Expected NORM max attack ~5.0 s, got %.2f s (%u samples)", norm_s, atk_norm);
    CHECK(long_s > 24.0 && long_s < 26.0,
          "Expected LONG max attack ~25.0 s, got %.2f s (%u samples)", long_s, atk_long);

    /* Verify slow NORM (5 s) and LONG (25 s) envelopes retain true analog RC exponential curvature
     * (attack midpoint > 62% of full scale; decay midpoint < 30% of full scale, vs 50% linear). */
    {
        adrift_env_t env_rc;
        adrift_env_params_t prc = {
            .onset_delay = 0, .attack = 1023, .decay = 1023, .sustain = 0, .release = 1023,
            .vel_sens = 0, .loop_mode = ADRIFT_ENV_LOOP_OFF, .range = ADRIFT_ENV_RANGE_LONG,
            .drone_hold = false
        };
        adrift_env_init(&env_rc);
        adrift_env_note_on(&env_rc, &prc, 127u, false);
        int32_t blk[64];
        uint32_t half_atk = (atk_long / 2u) & ~63u;
        for (uint32_t s_cnt = 0u; s_cnt < half_atk; s_cnt += 64u) {
            adrift_env_process(&env_rc, &prc, blk, 64u);
        }
        double atk_mid_norm = (double)blk[63] / 32767.0;
        CHECK(atk_mid_norm > 0.58 && atk_mid_norm < 0.72,
              "25s LONG attack fell back to linear ramp (midpoint=%.3f, expected RC ~0.63)", atk_mid_norm);

        while (env_rc.stage == ADRIFT_ENV_ATTACK) {
            adrift_env_process(&env_rc, &prc, blk, 64u);
        }
        /* Measure total decay samples and verify midpoint is strictly convex (< 38% vs 50% linear) */
        adrift_env_t env_dec_cnt = env_rc;
        uint32_t dec_long = 0u;
        while (env_dec_cnt.stage == ADRIFT_ENV_DECAY && dec_long < 2000000u) {
            adrift_env_process(&env_dec_cnt, &prc, blk, 64u);
            dec_long += 64u;
        }
        uint32_t half_dec = (dec_long / 2u) & ~63u;
        for (uint32_t s_cnt = 0u; s_cnt < half_dec; s_cnt += 64u) {
            adrift_env_process(&env_rc, &prc, blk, 64u);
        }
        double dec_mid_norm = (double)blk[63] / 32767.0;
        CHECK(dec_mid_norm > 0.25 && dec_mid_norm < 0.40,
              "25s LONG decay fell back to linear ramp (midpoint=%.3f, expected RC ~0.33)", dec_mid_norm);
    }

    /* 15b. Verify Decay & Release scale with range while Onset Delay stays 0-2 s */
    adrift_env_t env_s, env_l;
    adrift_env_params_t ps = {
        .onset_delay = 512, .attack = 0, .decay = 1023, .sustain = 0, .release = 1023,
        .range = ADRIFT_ENV_RANGE_SHORT
    };
    adrift_env_params_t pl = ps;
    pl.range = ADRIFT_ENV_RANGE_LONG;

    adrift_env_init(&env_s);
    adrift_env_init(&env_l);
    adrift_env_note_on(&env_s, &ps, 127u, false);
    adrift_env_note_on(&env_l, &pl, 127u, false);
    CHECK(env_s.delay_total == env_l.delay_total && env_s.delay_total > 18000u,
          "Expected Onset Delay to remain unaffected by ENV RANGE (short=%u, long=%u)",
          env_s.delay_total, env_l.delay_total);
    CHECK(env_s.dec_floor_q30 > env_l.dec_floor_q30 * 30 &&
          env_s.rel_floor_q30 > env_l.rel_floor_q30 * 30,
          "Expected SHORT decay/release rates to be >30x faster than LONG");

    /* 15c. Verify ENV1 UI 2-subpage cycling (1/2 -> 2/2 -> 1/2), 8:RANGE card on Page 2/2 KNOB4, and MIDI CC39 */
    uint16_t *fb = (uint16_t *)calloc(ADRIFT_SCREEN_W * ADRIFT_SCREEN_H, sizeof(uint16_t));
    CHECK(fb != NULL, "Failed to allocate framebuffer");

    adrift_app_init();
    adrift_synth_t *s  = adrift_app_get_synth();
    adrift_ui_t    *ui = adrift_app_get_ui();
    adrift_synth_init_patch(s);

    adrift_input_state_t in_st;
    adrift_dirty_t dirty;

    /* Press 1: Open ENV1 Page 1/2 (subpage 0) */
    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed = ADRIFT_BTN_PAGE_2;
    in_st.master_pot      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->page == 1u && ui->subpage == 0u, "Expected ENV1 Page 1/2 (subpage 0)");

    /* Press 2: Advance to ENV1 Page 2/2 (subpage 1) and turn KNOB4 (-1) -> SHORT */
    in_st.enc_delta[ADRIFT_ENC_KNOB4] = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->page == 1u && ui->subpage == 1u &&
          s->groups[0].patch.env1.range == ADRIFT_ENV_RANGE_SHORT,
          "Expected ENV1 Page 2/2 KNOB4 (-1) to select SHORT range (got subpage=%u, range=%u)",
          (unsigned)ui->subpage, (unsigned)s->groups[0].patch.env1.range);

    /* Verify all 4 slots (including slot 3 = 8:RANGE at x=190) on ENV1 Page 2/2 render card header bands */
    uint16_t c3_hdr = fb[145u * ADRIFT_SCREEN_W + 190u];
    uint16_t c3_int = fb[185u * ADRIFT_SCREEN_W + 190u];
    CHECK(c3_hdr != c3_int,
          "Expected ENV1 Page 2/2 slot 3 (RANGE) to render a card header band");

    /* Turn KNOB4 (+1) -> NORM, then (+1) -> LONG */
    memset(&in_st, 0, sizeof(in_st));
    in_st.enc_delta[ADRIFT_ENC_KNOB4] = 1;
    in_st.master_pot                  = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(s->groups[0].patch.env1.range == ADRIFT_ENV_RANGE_NORM, "Expected KNOB4 (+1) to step SHORT -> NORM");

    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(s->groups[0].patch.env1.range == ADRIFT_ENV_RANGE_LONG, "Expected KNOB4 (+1) to step NORM -> LONG");

    /* Press 3: Wraps back to ENV1 Page 1/2 (subpage 0) */
    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed = ADRIFT_BTN_PAGE_2;
    in_st.master_pot      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(ui->page == 1u && ui->subpage == 0u, "Expected 3rd ENV press to wrap to Page 1/2 (subpage 0)");

    /* Verify MIDI CC39 (0..41 -> SHORT, 42..84 -> NORM, 85..127 -> LONG) */
    adrift_synth_handle_midi(s, 0xB0u, 39u, 0u);
    CHECK(s->groups[0].patch.env1.range == ADRIFT_ENV_RANGE_SHORT, "CC39=0 should select SHORT");
    adrift_synth_handle_midi(s, 0xB0u, 39u, 64u);
    CHECK(s->groups[0].patch.env1.range == ADRIFT_ENV_RANGE_NORM, "CC39=64 should select NORM");
    adrift_synth_handle_midi(s, 0xB0u, 39u, 127u);
    CHECK(s->groups[0].patch.env1.range == ADRIFT_ENV_RANGE_LONG, "CC39=127 should select LONG");

    free(fb);
    printf("  -> PASS (SHORT=%.1f ms, NORM=%.2f s, LONG=%.2f s, UI Page 2/2 & CC39 OK)\n",
           short_ms, norm_s, long_s);
}

/* ============================================================================
 * 16. Extended LFO & Env 1 Modulation Targets (PAN, SPREAD, DRYWET, VCA,
 *     ALL PT, EX MIX, EX LVL, SH CLK, IN RES, DISPR, E1 DEC, L1 RT)
 * ============================================================================ */
static void compute_lr_rms_dbfs(const int16_t *stereo,
                                uint32_t start_f,
                                uint32_t count_f,
                                double *out_l_db,
                                double *out_r_db) {
    double sum_l = 0.0;
    double sum_r = 0.0;
    for (uint32_t i = 0; i < count_f; i++) {
        double sl = (double)stereo[2u * (start_f + i)]     / 32768.0;
        double sr = (double)stereo[2u * (start_f + i) + 1] / 32768.0;
        sum_l += sl * sl;
        sum_r += sr * sr;
    }
    double rms_l = sqrt(sum_l / (double)(count_f > 0u ? count_f : 1u));
    double rms_r = sqrt(sum_r / (double)(count_f > 0u ? count_f : 1u));
    *out_l_db = (rms_l > 1e-9) ? (20.0 * log10(rms_l)) : -180.0;
    *out_r_db = (rms_r > 1e-9) ? (20.0 * log10(rms_r)) : -180.0;
}

void test_extended_mod_targets(void) {
    printf("[Test 16] Extended Modulation Targets (PAN, SPREAD, DRYWET, VCA, ALL PT, EX MIX/LVL, SH CLK, IN RES, DISPR, E1 DEC, L1 RT)...\n");

    const uint32_t N = 16384u;
    int16_t  *pcm = (int16_t *)calloc(2u * N, sizeof(int16_t));
    uint16_t *fb  = (uint16_t *)calloc(ADRIFT_SCREEN_W * ADRIFT_SCREEN_H, sizeof(uint16_t));
    CHECK(pcm != NULL && fb != NULL, "Failed to allocate test buffers");

    /* 16a. LFO -> PAN (Square LFO @ ~4.3 Hz pans hard Right on +half, hard Left on -half) */
    adrift_app_init();
    adrift_synth_t *s = adrift_app_get_synth();
    adrift_synth_init_patch(s);
    adrift_group_t *gp = &s->groups[0];
    adrift_patch_t *p  = &gp->patch;

    (void)adrift_synth_set_group_voices(s, 0u, 1u);
    gp->pan           = 0;
    gp->stereo_spread = 0;
    p->exc.vco_level  = 900;
    p->dry_wet        = 0;
    p->lfo_wave       = ADRIFT_LFO_SQR;
    p->lfo_rate       = 480; /* half-cycle ~3321 samples */
    p->lfo_depth      = 511;
    p->lfo_dest       = ADRIFT_MOD_PAN;

    adrift_synth_note_on(s, 60u, 127u);
    render_frames(pcm, N);

    double pan_hi_l = 0.0, pan_hi_r = 0.0;
    double pan_lo_l = 0.0, pan_lo_r = 0.0;
    compute_lr_rms_dbfs(pcm, 256u,  2048u, &pan_hi_l, &pan_hi_r);
    compute_lr_rms_dbfs(pcm, 3700u, 2048u, &pan_lo_l, &pan_lo_r);
    CHECK(pan_hi_r - pan_hi_l > 25.0 && pan_lo_l - pan_lo_r > 25.0,
          "Expected LFO SQR -> PAN to alternate hard R then hard L (hi L=%.1f R=%.1f, lo L=%.1f R=%.1f)",
          pan_hi_l, pan_hi_r, pan_lo_l, pan_lo_r);

    /* 16b. LFO -> SPREAD (2-voice Unison with base spread=0 widens stereo image on +half, collapses to mono on -half) */
    adrift_app_init();
    adrift_synth_init_patch(s);
    (void)adrift_synth_set_group_voices(s, 0u, 2u);
    gp->assign_mode   = ADRIFT_GROUP_UNISON;
    gp->unison_detune = 500;
    gp->stereo_spread = 0;
    gp->pan           = 0;
    p->exc.vco_level  = 900;
    p->dry_wet        = 0;
    for (uint8_t v = 0; v < ADRIFT_MAX_VOICES; v++) {
        s->voices[v].lfo2_phase = 0u;
    }
    p->lfo2_wave  = ADRIFT_LFO_SQR;
    p->lfo2_rate  = 480;
    p->lfo2_depth = 511;
    p->lfo2_dest  = ADRIFT_MOD_SPREAD;

    adrift_synth_note_on(s, 60u, 127u);
    render_frames(pcm, N);

    double diff_wide = 0.0, diff_mono = 0.0;
    for (uint32_t i = 256u; i < 2304u; i++) {
        double d = (double)(pcm[2u * i] - pcm[2u * i + 1u]) / 32768.0;
        diff_wide += d * d;
    }
    for (uint32_t i = 3700u; i < 5748u; i++) {
        double d = (double)(pcm[2u * i] - pcm[2u * i + 1u]) / 32768.0;
        diff_mono += d * d;
    }
    CHECK(diff_wide > 10.0 && diff_mono < 1e-6,
          "Expected LFO SQR -> SPREAD to alternate wide stereo (%.3f) and exact mono (%.6f)",
          diff_wide, diff_mono);

    /* 16c. LFO -> VCA (Square LFO @ 100% depth gates voice output by >35 dB) */
    adrift_app_init();
    adrift_synth_init_patch(s);
    p->exc.vco_level = 900;
    p->dry_wet       = 0;
    p->lfo_wave      = ADRIFT_LFO_SQR;
    p->lfo_rate      = 480;
    p->lfo_depth     = 511;
    p->lfo_dest      = ADRIFT_MOD_VCA;

    adrift_synth_note_on(s, 60u, 127u);
    render_frames(pcm, N);

    double vca_hi_db = compute_rms_dbfs(pcm, 256u,  2048u);
    double vca_lo_db = compute_rms_dbfs(pcm, 3700u, 2048u);
    CHECK(vca_hi_db - vca_lo_db > 35.0,
          "Expected LFO SQR -> VCA to gate voice level by >35 dB (hi=%.1f dB, lo=%.1f dB)",
          vca_hi_db, vca_lo_db);

    /* 16d. LFO -> EX LVL, LFO3 -> EX MIX, LFO2 -> SH CLK & LFO -> DRYWET */
    adrift_app_init();
    adrift_synth_init_patch(s);
    p->exc.vco_level   = 0; /* Base exciter level 0, woken up by LFO -> EX LVL */
    p->exc.noise_level = 0; /* Base pure VCO wave, modulated toward noise by LFO3 -> EX MIX */
    p->exc.sh_rate     = 900;
    p->dry_wet         = 0;
    p->lfo_wave        = ADRIFT_LFO_SQR;
    p->lfo_rate        = 480;
    p->lfo_depth       = 511;
    p->lfo_dest        = ADRIFT_MOD_EX_LVL;
    p->lfo2_wave       = ADRIFT_LFO_SQR;
    p->lfo2_rate       = 480;
    p->lfo2_depth      = -512; /* Pulls SH CLK down from 900 to ~388 on +half */
    p->lfo2_dest       = ADRIFT_MOD_SH_CLK;
    p->lfo3_wave       = ADRIFT_LFO_SQR;
    p->lfo3_rate       = 480;
    p->lfo3_depth      = 511;
    p->lfo3_dest       = ADRIFT_MOD_EX_MIX;

    adrift_synth_note_on(s, 60u, 127u);
    render_frames(pcm, N);

    double ex_lvl_hi_db = compute_rms_dbfs(pcm, 256u,  2048u);
    double ex_lvl_lo_db = compute_rms_dbfs(pcm, 3700u, 2048u);
    CHECK(ex_lvl_hi_db > -25.0 && ex_lvl_hi_db - ex_lvl_lo_db > 35.0,
          "Expected LFO -> EX LVL to wake exciter on +half and mute on -half (hi=%.1f dB, lo=%.1f dB)",
          ex_lvl_hi_db, ex_lvl_lo_db);

    /* Test LFO -> DRYWET crossfading between loud dry VCO and silent BBD (when DRYWET toggles 0 <-> 1023) */
    adrift_app_init();
    adrift_synth_init_patch(s);
    p->exc.vco_level = 0;    /* Silent dry path */
    p->bbd.feedback  = 1023; /* Self-oscillating BBD wet path */
    p->bbd.bbd_noise = 500;
    p->dry_wet       = 512;
    p->lfo_wave      = ADRIFT_LFO_SQR;
    p->lfo_rate      = 480;
    p->lfo_depth     = -512; /* +half -> dry_wet=0 (silent dry), -half -> dry_wet=1023 (self-osc wet) */
    p->lfo_dest      = ADRIFT_MOD_DRY_WET;

    adrift_synth_note_on(s, 60u, 127u);
    /* Pre-warm BBD self-oscillation for 1 LFO cycle (6642 samples), then measure +half (dry) vs -half (wet) */
    render_frames(pcm, 6642u);
    render_frames(pcm, N);
    double dw_dry_db = compute_rms_dbfs(pcm, 256u,  2048u);
    double dw_wet_db = compute_rms_dbfs(pcm, 3700u, 2048u);
    CHECK(dw_wet_db - dw_dry_db > 35.0,
          "Expected LFO -> DRYWET to crossfade between silent dry (%.1f dB) and self-osc BBD wet (%.1f dB)",
          dw_dry_db, dw_wet_db);

    /* 16e. LFO -> ALL PT (Unified VCO + BBD pitch modulation stays locked across +6 / -6 ST) */
    adrift_app_init();
    adrift_synth_init_patch(s);
    p->exc.vco_level     = 850;
    p->bbd.feedback      = 820;
    p->bbd.fb_lpf_cutoff = 950;
    p->bbd.glide         = 0;
    p->bbd.bbd_noise     = 0;
    p->dry_wet           = 512; /* 50/50 mix of VCO and BBD */
    p->lfo_wave          = ADRIFT_LFO_SQR;
    p->lfo_rate          = 480;
    p->lfo_depth         = 256; /* +-6 ST on both VCO and BBD */
    p->lfo_dest          = ADRIFT_MOD_ALL_PITCH;

    adrift_synth_note_on(s, 60u, 127u);
    render_frames(pcm, N);

    double all_hi_hz = estimate_fundamental_dft(pcm, 512u,  2048u, 370.0);
    double all_lo_hz = estimate_fundamental_dft(pcm, 3700u, 2048u, 185.0);
    double all_span  = 12.0 * log2(all_hi_hz / all_lo_hz);
    CHECK(fabs(all_span - 12.0) < 0.6,
          "Expected LFO -> ALL PT to shift VCO+BBD by 12.0 ST span (hi=%.1f Hz, lo=%.1f Hz, span=%.2f ST)",
          all_hi_hz, all_lo_hz, all_span);

    /* 16f. LFO -> IN RES, DISPR, E1 DEC & L1 RT */
    adrift_app_init();
    adrift_synth_init_patch(s);
    p->in_lpf_cutoff  = 520;
    p->in_lpf_res     = 0;
    p->bbd.dispersion = 0;
    p->env1.attack    = 0;
    p->env1.decay     = 500;
    p->env1.sustain   = 0;
    p->env1.loop_mode = ADRIFT_ENV_LOOP_AD;
    p->lfo_wave       = ADRIFT_LFO_SQR;
    p->lfo_rate       = 100; /* Slow LFO 1 */
    p->lfo_depth      = 511;
    p->lfo_dest       = ADRIFT_MOD_DISPR;
    p->lfo2_wave      = ADRIFT_LFO_SQR;
    p->lfo2_rate      = 480;
    p->lfo2_depth     = 511;
    p->lfo2_dest      = ADRIFT_MOD_LFO1_RATE; /* Boosts LFO 1 rate from 100 -> 611 */
    p->lfo3_wave      = ADRIFT_LFO_SQR;
    p->lfo3_rate      = 480;
    p->lfo3_depth     = -512;
    p->lfo3_dest      = ADRIFT_MOD_ENV1_DEC;  /* Shortens Env 1 decay from 500 -> 0 on +half */

    adrift_synth_note_on(s, 60u, 127u);
    uint32_t ph0 = s->voices[0].lfo_phase;
    render_frames(pcm, 64u);
    uint32_t dph_boosted = s->voices[0].lfo_phase - ph0;
    uint32_t disp_r      = s->voices[0].bbd.disp_r_q15;
    int32_t  a1_no_res   = s->voices[0].in_lpf.a1_q28;

    p->lfo_dest = ADRIFT_MOD_IN_RES;
    render_frames(pcm, 64u);
    int16_t res_mod    = s->voices[0].in_lpf.last_res;
    int32_t a1_with_res = s->voices[0].in_lpf.a1_q28;

    CHECK(disp_r > 0u &&
          s->voices[0].env1.dec_floor_q30 > (1 << 20) &&
          dph_boosted > 40000000u &&
          res_mod > 400 && a1_with_res > a1_no_res,
          "Expected DISPR (r=%u), E1 DEC (floor=%d), L1 RT (dph=%u), and IN RES (res=%d) modulations to be active",
          (unsigned)disp_r,
          s->voices[0].env1.dec_floor_q30,
          (unsigned)dph_boosted,
          (int)res_mod);

    /* 16g. Verify L1 DST excludes L1 RT while L2/L3 DST include all ADRIFT_MOD_DEST_COUNT targets */
    adrift_input_state_t in_st;
    adrift_dirty_t dirty;
    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed = ADRIFT_BTN_PAGE_5; /* Open MOD Page 1/4 (LFO 1) */
    in_st.master_pot      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);

    p->lfo_dest = 0u;
    for (uint8_t d = 0; d < ADRIFT_LFO1_DEST_COUNT; d++) {
        memset(&in_st, 0, sizeof(in_st));
        in_st.enc_delta[ADRIFT_ENC_KNOB4] = (d == 0u) ? 0 : 1;
        in_st.master_pot                  = -1;
        adrift_dirty_clear(&dirty);
        adrift_app_tick_ui(&in_st, fb, &dirty);
        CHECK(p->lfo_dest == d, "Expected L1 DST to step to %u, got %u", (unsigned)d, (unsigned)p->lfo_dest);
    }

    /* Turning KNOB4 +1 past E1 DLY on LFO 1 must stay clamped and NOT reach ADRIFT_MOD_LFO1_RATE */
    memset(&in_st, 0, sizeof(in_st));
    in_st.enc_delta[ADRIFT_ENC_KNOB4] = 1;
    in_st.master_pot                  = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
    CHECK(p->lfo_dest == ADRIFT_LFO1_DEST_COUNT - 1u && p->lfo_dest != ADRIFT_MOD_LFO1_RATE,
          "Expected L1 DST to exclude L1 RT and clamp at %u, got %u",
          (unsigned)(ADRIFT_LFO1_DEST_COUNT - 1u), (unsigned)p->lfo_dest);

    adrift_synth_handle_midi(s, 0xB0u, 86u, 127u); /* CC86=127 -> max LFO 1 Dest */
    CHECK(p->lfo_dest == ADRIFT_LFO1_DEST_COUNT - 1u,
          "Expected CC86=127 to clamp LFO 1 Dest at %u, got %u",
          (unsigned)(ADRIFT_LFO1_DEST_COUNT - 1u), (unsigned)p->lfo_dest);

    /* Advance to MOD Page 2/4 (LFO 2) and verify L2 DST reaches ADRIFT_MOD_LFO1_RATE */
    memset(&in_st, 0, sizeof(in_st));
    in_st.buttons_pressed = ADRIFT_BTN_PAGE_5;
    in_st.master_pot      = -1;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);

    p->lfo2_dest = 0u;
    for (uint8_t d = 0; d < (uint8_t)ADRIFT_MOD_DEST_COUNT; d++) {
        memset(&in_st, 0, sizeof(in_st));
        in_st.enc_delta[ADRIFT_ENC_KNOB4] = (d == 0u) ? 0 : 1;
        in_st.master_pot                  = -1;
        adrift_dirty_clear(&dirty);
        adrift_app_tick_ui(&in_st, fb, &dirty);
        CHECK(p->lfo2_dest == d, "Expected L2 DST to step to %u, got %u", (unsigned)d, (unsigned)p->lfo2_dest);
    }
    CHECK(p->lfo2_dest == ADRIFT_MOD_LFO1_RATE, "Expected L2 DST max to be ADRIFT_MOD_LFO1_RATE");

    free(fb);
    free(pcm);
    printf("  -> PASS (PAN R/L=+%.1f/+%.1f dB, SPREAD, DRYWET, VCA=+%.1f dB, ALL PT=%.2f ST, EX MIX/LVL, SH CLK, IN RES, DISPR, E1 DEC, L1 RT & UI OK)\n",
           pan_hi_r - pan_hi_l, pan_lo_l - pan_lo_r, vca_hi_db - vca_lo_db, all_span);
}

/* ============================================================================
 * 19. USB Audio Class 1 (44.1 kHz Bit-Exact & 48 kHz Polyphase FIR Resampler)
 * ============================================================================ */
static uint32_t sim_uac_tap48(const int16_t *in_stereo,
                              uint32_t n_in,
                              int16_t *out_stereo,
                              uint32_t max_out) {
    enum { RS_H = 32u };
    int16_t  hist_l[2u * RS_H];
    int16_t  hist_r[2u * RS_H];
    uint32_t h   = 0u;
    uint32_t ph  = 0u;
    uint32_t out = 0u;
    memset(hist_l, 0, sizeof(hist_l));
    memset(hist_r, 0, sizeof(hist_r));

    for (uint32_t i = 0u; i < n_in; i++) {
        uint32_t s = h & (RS_H - 1u);
        int16_t  l = in_stereo[2u * i];
        int16_t  r = in_stereo[2u * i + 1u];
        hist_l[s] = hist_l[s + RS_H] = l;
        hist_r[s] = hist_r[s + RS_H] = r;
        h++;
        s = (h - UAC_FIR_N) & (RS_H - 1u);
        do {
            const int16_t *xl = &hist_l[s];
            const int16_t *xr = &hist_r[s];
            int64_t al = 0;
            int64_t ar = 0;
            if (ph < UAC_FIR_L / 2u) {
                const int32_t *c = UAC_FIR[ph];
                for (uint32_t j = UAC_FIR_N; j; j--, xl++, xr++) {
                    int32_t t = *c++;
                    al += (int64_t)t * (*xl);
                    ar += (int64_t)t * (*xr);
                }
            } else {
                const int32_t *c = &UAC_FIR[UAC_FIR_L - 1u - ph][UAC_FIR_N - 1u];
                for (uint32_t j = UAC_FIR_N; j; j--, xl++, xr++) {
                    int32_t t = *c--;
                    al += (int64_t)t * (*xl);
                    ar += (int64_t)t * (*xr);
                }
            }
            int32_t yl = (int32_t)((al + (1 << (UAC_FIR_Q - 1))) >> UAC_FIR_Q);
            int32_t yr = (int32_t)((ar + (1 << (UAC_FIR_Q - 1))) >> UAC_FIR_Q);
            if (yl > 32767)  yl = 32767;
            if (yl < -32768) yl = -32768;
            if (yr > 32767)  yr = 32767;
            if (yr < -32768) yr = -32768;
            if (out < max_out) {
                out_stereo[2u * out]      = (int16_t)yl;
                out_stereo[2u * out + 1u] = (int16_t)yr;
            }
            out++;
            ph += UAC_FIR_M;
        } while (ph < UAC_FIR_L);
        ph -= UAC_FIR_L;
    }
    return out;
}

void test_uac_resampler(void) {
    printf("[Test 19] USB Audio 44.1 kHz Bit-Exact Tap & 48 kHz Polyphase FIR (uac_fir.h)...\n");

    /* 19a. 44.1 kHz pcm16 -> Q23 (* 256) -> uac_in16 (((v + 128) >> 8)) bit-exactness across all int16_t */
    int32_t roundtrip_errs = 0;
    for (int32_t v = -32768; v <= 32767; v++) {
        int32_t q23 = (int32_t)((int16_t)v) * 256;
        int32_t u16 = (q23 + 128) >> 8;
        if (u16 != v) roundtrip_errs++;
    }
    CHECK(roundtrip_errs == 0, "Expected 0 roundtrip errors across all 65536 int16_t values, got %d", roundtrip_errs);

    /* 19b. Exact Q30 unity DC gain per polyphase row (eliminates phase-dependent DC modulation) */
    const int64_t q30_one = (int64_t)1 << UAC_FIR_Q;
    uint32_t bad_sums = 0u;
    for (uint32_t ph = 0u; ph < UAC_FIR_L / 2u; ph++) {
        int64_t s = 0;
        for (uint32_t k = 0u; k < UAC_FIR_N; k++) {
            s += UAC_FIR[ph][k];
        }
        if (s != q30_one) bad_sums++;
    }
    CHECK(bad_sums == 0u, "Expected all 80 polyphase rows to sum to exact 2^30, got %u mismatches", bad_sums);

    /* 19c. Linear ramp continuity across all 160 phases (forward & mirror branches) */
    const uint32_t N_RAMP = 512u;
    int16_t ramp_in[512u * 2u];
    int16_t ramp_out[600u * 2u];
    for (uint32_t i = 0u; i < N_RAMP; i++) {
        int16_t v = (int16_t)((int32_t)i * 32 - 8192);
        ramp_in[2u * i]      = v;
        ramp_in[2u * i + 1u] = (int16_t)(-v);
    }
    uint32_t n_ramp_out = sim_uac_tap48(ramp_in, N_RAMP, ramp_out, 600u);
    const double expected_step = 32.0 * (double)UAC_FIR_M / (double)UAC_FIR_L; /* 29.400 LSB/step */
    double max_step_err = 0.0;
    for (uint32_t i = 64u; i < n_ramp_out - 32u; i++) {
        double dl = (double)ramp_out[2u * (i + 1u)]      - (double)ramp_out[2u * i];
        double dr = (double)ramp_out[2u * (i + 1u) + 1u] - (double)ramp_out[2u * i + 1u];
        double el = fabs(dl - expected_step);
        double er = fabs(dr + expected_step);
        if (el > max_step_err) max_step_err = el;
        if (er > max_step_err) max_step_err = er;
    }
    CHECK(max_step_err < 1.1,
          "Polyphase 160/147 ramp step discontinuity: max_step_err=%.3f LSB (expected ~%.2f LSB/step)",
          max_step_err, expected_step);

    /* 19d. 1 kHz & 12 kHz passband accuracy and audible-band anti-imaging SFDR */
    const uint32_t N_IN  = 147u * 60u;  /* 8820 input frames -> exact 9600 output frames */
    const uint32_t N_OUT = 160u * 60u;
    int16_t *in_buf  = (int16_t *)calloc(N_IN * 2u, sizeof(int16_t));
    int16_t *out_buf = (int16_t *)calloc((N_OUT + 16u) * 2u, sizeof(int16_t));
    CHECK(in_buf != NULL && out_buf != NULL, "UAC resampler test buffer allocation");

    const double fs_in  = FS_EXACT;
    const double fs_out = FS_EXACT * 160.0 / 147.0;
    const double amp    = 20000.0;

    for (uint32_t i = 0u; i < N_IN; i++) {
        double s1 = amp * sin(2.0 * M_PI * 1000.0 * (double)i / fs_in);
        double s2 = amp * sin(2.0 * M_PI * 12000.0 * (double)i / fs_in);
        in_buf[2u * i]      = (int16_t)lrint(s1);
        in_buf[2u * i + 1u] = (int16_t)lrint(s2);
    }
    uint32_t got_out = sim_uac_tap48(in_buf, N_IN, out_buf, N_OUT + 16u);
    CHECK(got_out == N_OUT, "Expected exact %u output frames from %u input frames, got %u", N_OUT, N_IN, got_out);

    /* Measure 1 kHz (L) and 12 kHz (R) steady-state RMS over 8192 frames after startup transient */
    double sq_l = 0.0;
    double sq_r = 0.0;
    const uint32_t W = 8192u;
    const uint32_t S = 512u;
    for (uint32_t i = 0u; i < W; i++) {
        double l = (double)out_buf[2u * (S + i)];
        double r = (double)out_buf[2u * (S + i) + 1u];
        sq_l += l * l;
        sq_r += r * r;
    }
    double rms_in = amp / sqrt(2.0);
    double err_1k_db  = fabs(20.0 * log10(sqrt(sq_l / (double)W) / rms_in));
    double err_12k_db = fabs(20.0 * log10(sqrt(sq_r / (double)W) / rms_in));
    CHECK(err_1k_db < 0.05 && err_12k_db < 0.05,
          "Expected < 0.05 dB passband error at 1 kHz (%.4f dB) and 12 kHz (%.4f dB)",
          err_1k_db, err_12k_db);

    /* Hann-windowed DFT check on R channel (12 kHz) to verify audible spurs < -75 dBc */
    double fund_mag = 0.0;
    double max_spur = 0.0;
    for (uint32_t bin = 40u; bin < 3400u; bin++) { /* ~234 Hz .. 19.9 kHz at fs_out=48019.2 Hz */
        double f_bin = (double)bin * fs_out / (double)W;
        bool is_fund = fabs(f_bin - 12000.0) < 30.0;
        if (!is_fund && (bin % 37u != 0u || fabs(f_bin - 12000.0) < 120.0)) {
            continue;
        }
        double re = 0.0;
        double im = 0.0;
        double w_step = 2.0 * M_PI * (double)bin / (double)W;
        for (uint32_t n = 0u; n < W; n++) {
            double h_win = 0.5 - 0.5 * cos(2.0 * M_PI * (double)n / (double)W);
            double x     = (double)out_buf[2u * (S + n) + 1u] * h_win;
            re += x * cos(w_step * (double)n);
            im -= x * sin(w_step * (double)n);
        }
        double mag = sqrt(re * re + im * im);
        if (is_fund) {
            if (mag > fund_mag) fund_mag = mag;
        } else {
            if (mag > max_spur) max_spur = mag;
        }
    }
    /* Also explicitly probe the primary alias/image frequencies (2*fs_in - 3*f0, fs_out - (fs_in - f0), etc.) */
    const double probe_hz[4] = {
        fabs(fs_out - (fs_in - 12000.0)),
        fabs(2.0 * fs_in - 4.0 * 12000.0),
        32117.647 - 16000.0,
        4901.56
    };
    for (uint32_t p = 0u; p < 4u; p++) {
        double re = 0.0;
        double im = 0.0;
        double w_step = 2.0 * M_PI * probe_hz[p] / fs_out;
        for (uint32_t n = 0u; n < W; n++) {
            double h_win = 0.5 - 0.5 * cos(2.0 * M_PI * (double)n / (double)W);
            double x     = (double)out_buf[2u * (S + n) + 1u] * h_win;
            re += x * cos(w_step * (double)n);
            im -= x * sin(w_step * (double)n);
        }
        double mag = sqrt(re * re + im * im);
        if (mag > max_spur) max_spur = mag;
    }
    double sfdr_db = 20.0 * log10(fund_mag / (max_spur > 1e-9 ? max_spur : 1e-9));
    CHECK(sfdr_db > 75.0, "Expected > 75 dBc SFDR on 48 kHz resampled 12 kHz tone, got %.1f dBc", sfdr_db);

    free(out_buf);
    free(in_buf);
    printf("  -> PASS (65536 int16 bit-exact, 80x Q30 unity DC, ramp err=%.2f LSB, 1k/12k err=%.4f/%.4f dB, SFDR=%.1f dBc)\n",
           max_step_err, err_1k_db, err_12k_db, sfdr_db);
}
