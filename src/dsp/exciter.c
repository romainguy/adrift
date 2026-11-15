#include "exciter.h"

/* Deterministic per-voice analog component trims across all 16 hardware voices */
static const int8_t  k_voice_trim_pitch[16] = {
    -2, +3, -1, +4, -3, +1, +2, -4, +2, -3, +4, -1, +3, -2, -4, +1
};
static const int16_t k_voice_trim_sag[16] = {
    2100, 2450, 1950, 2600, 2250, 2050, 2500, 2150,
    2350, 2000, 2550, 2120, 2400, 1980, 2480, 2200
};
static const int16_t k_voice_trim_sym[16] = {
     950, -1100,  820,  -900, 1150,  -780, 1020,  -980,
    -860,  1080, -940,  1120, -810,   990, -1050,  890
};
static const int16_t k_voice_trim_pw[16] = {
     380, -420,  260, -510,  440, -310,  290, -360,
    -290,  460, -380,  340, -470,  240, -330,  410
};

void adrift_exciter_init(adrift_exciter_t *exc, uint8_t voice_idx) {
    uint8_t v = voice_idx & 15u;
    exc->voice_idx       = voice_idx;
    exc->vco_phase       = (uint32_t)voice_idx * 0x1F3D5B79u;
    exc->vco_inc         = dsp_pitch_to_phase_inc(60 << 8);
    exc->inv_dt16        = 1u << 16;
    exc->pw_thresh       = 0x80000000u;
    exc->sub_phase       = (uint32_t)voice_idx * 0x0F9EADB8u;
    exc->impulse_phase   = 0u;
    exc->impulse_amp_q15 = 0;

    exc->trim_pitch_q8   = k_voice_trim_pitch[v];
    exc->trim_sag_q15    = k_voice_trim_sag[v];
    exc->trim_sym_q15    = k_voice_trim_sym[v];
    exc->trim_pw_q15     = k_voice_trim_pw[v];

    exc->drift_pitch_q16 = 0;
    exc->drift_pw_q15    = 0;
    exc->ac_cap_q23      = 0;
    exc->hf_slew_q15     = 0;

    exc->rng_state       = 0x9E3779B9u ^ ((uint32_t)(voice_idx + 1u) * 0x85EBCA6Bu);
    exc->sh_phase        = 0x40000000u;
    exc->sh_inc          = 0u;
    exc->sh_held_q15     = dsp_white_noise_q15(&exc->rng_state);

    exc->vco_gain_q15       = 0;
    exc->noise_gain_q15     = 0;
    exc->last_pitch_q16     = -0x7FFFFFFF;
    exc->last_sh_rate       = -1;
    exc->pending_phase_lock = 0u;
    exc->pending_uni_slot   = 0u;
}

void adrift_exciter_trigger_ex(adrift_exciter_t *exc,
                               const adrift_exciter_params_t *p,
                               uint8_t velocity,
                               bool legato,
                               uint8_t unison_slot,
                               bool exc_audible,
                               bool delayed_onset) {
    if (legato || p->pluck <= 0) {
        return;
    }
    int32_t pluck = dsp_clamp_i32(p->pluck, 0, 1023);
    if (!exc_audible) {
        uint32_t phase_offset   = (uint32_t)unison_slot * 0x24924924u;
        exc->vco_phase          = phase_offset;
        exc->sub_phase          = phase_offset >> 1;
        exc->hf_slew_q15        = 0;
        exc->ac_cap_q23         = 0;
        exc->pending_phase_lock = 0u;
        int32_t exc_lvl_q15     = (int32_t)dsp_udiv_1023((uint32_t)dsp_clamp_i32(p->vco_level, 0, 1023) * 32767u);
        int32_t mix_1023        = dsp_clamp_i32(p->noise_level, 0, 1023);
        exc->vco_gain_q15       = (int32_t)dsp_udiv_1023((uint32_t)exc_lvl_q15 * (uint32_t)(1023 - mix_1023));
        exc->noise_gain_q15     = (int32_t)dsp_udiv_1023((uint32_t)exc_lvl_q15 * (uint32_t)mix_1023);
    } else if (delayed_onset) {
        exc->pending_phase_lock = 1u;
        exc->pending_uni_slot   = unison_slot;
    } else {
        exc->pending_phase_lock = 0u;
    }
    exc->drift_pitch_q16 = (exc->drift_pitch_q16 * (1023 - pluck)) >> 10;
    exc->drift_pw_q15    = (exc->drift_pw_q15 * (1023 - pluck)) >> 10;

    /* Velocity-scaled single-cycle mechanical pick/mallet displacement impulse */
    int32_t vel_scale    = 96 + (((int32_t)(velocity & 0x7Fu) * 160) >> 7); /* 96..256 */
    exc->impulse_amp_q15 = (pluck * vel_scale) >> 3;                        /* 0..32736 in Q15 */
    exc->impulse_phase   = 1u;
}

void adrift_exciter_trigger(adrift_exciter_t *exc,
                            const adrift_exciter_params_t *p,
                            uint8_t velocity,
                            bool legato,
                            uint8_t unison_slot) {
    adrift_exciter_trigger_ex(exc, p, velocity, legato, unison_slot, false, false);
}

DSP_ALWAYS_INLINE int32_t polyblep_edge_q15(uint32_t phase_diff, uint32_t dt, uint32_t inv_dt16) {
    /*
     * Division-free fixed-point PolyBLEP for a unit rising edge (-32768 -> +32767, step = 65536).
     * phase_diff = phase - edge_phase (wrapping unsigned modulo 2^32).
     * Fast single-branch unsigned window test rejects >97% of samples outside [-dt, +dt).
     */
    if ((uint32_t)(phase_diff + dt) <= (dt << 1)) {
        if (phase_diff < dt) {
            uint32_t t16  = phase_diff >> 16;
            int32_t  u    = (int32_t)((t16 * inv_dt16) >> 15); /* 0..32767 */
            if (u > 32767) u = 32767;
            int32_t  om_u = 32768 - u;
            return -((om_u * om_u) >> 15);
        }
        uint32_t rem_full = 0u - phase_diff;
        if (rem_full < dt) {
            uint32_t rem16  = rem_full >> 16;
            int32_t  eps    = (int32_t)((rem16 * inv_dt16) >> 15); /* 0..32767 */
            if (eps > 32767) eps = 32767;
            int32_t  om_eps = 32768 - eps;
            return (om_eps * om_eps) >> 15;
        }
    }
    return 0;
}

void adrift_exciter_prepare_block_q16(adrift_exciter_t *exc,
                                      const adrift_exciter_params_t *p,
                                      int32_t base_note_pitch_q16,
                                      int32_t mod_pitch_q8,
                                      int32_t mod_wave_1023,
                                      int32_t mod_pw_1023,
                                      int16_t *out_morph_1023) {
    /* 1. Block-rate Ornstein-Uhlenbeck thermal drift (~689 Hz update rate) */
    int32_t drift_amt = dsp_clamp_i32(p->analog_drift, 0, 1023);
    if (drift_amt == 0) {
        exc->drift_pitch_q16 = 0;
        exc->drift_pw_q15    = 0;
    } else {
        int32_t wn1 = dsp_white_noise_q15(&exc->rng_state);
        int32_t wn2 = dsp_white_noise_q15(&exc->rng_state);
        exc->drift_pitch_q16 -= (exc->drift_pitch_q16 >> 9); /* ~0.75 s mean-reversion tau */
        exc->drift_pitch_q16 += (wn1 * drift_amt) >> 11;
        exc->drift_pw_q15    -= (exc->drift_pw_q15 >> 8);
        exc->drift_pw_q15    += (wn2 * drift_amt) >> 16;
    }

    /* 2. Compute VCO pitch in Q16 semitones (0.0015 cent resolution for smooth portamento & drift) */
    int32_t pitch_q16 = p->vco_keytrack ? base_note_pitch_q16 : (60 << 16);
    pitch_q16 += ((int32_t)p->vco_octave * 12 + (int32_t)p->vco_semi) * 65536;
    int32_t fine = (int32_t)p->vco_fine;
    if (fine >= -100 && fine <= 100) {
        pitch_q16 += (fine >= 0) ? ((fine * 41943 + 32) >> 6) : -(((-fine) * 41943 + 32) >> 6);
    } else {
        pitch_q16 += (fine * 65536) / 100;
    }
    pitch_q16 += mod_pitch_q8 * 256;
    pitch_q16 += ((int32_t)exc->trim_pitch_q8 * drift_amt) >> 1;
    pitch_q16 += exc->drift_pitch_q16 >> 7; /* up to ~+-6 cents at max analog_drift */

    if (pitch_q16 != exc->last_pitch_q16) {
        exc->last_pitch_q16 = pitch_q16;
        uint32_t inc = dsp_pitch_q16_to_phase_inc(pitch_q16);
        exc->vco_inc = inc;

        uint32_t dt16 = inc >> 16;
        if (dt16 < 1u) dt16 = 1u;
        exc->inv_dt16 = (1u << 30) / dt16;
    }

    /* 3. Pulse width threshold (clamped 5% .. 95% to prevent comparator dropout) */
    int32_t pw_1023 = dsp_clamp_i32((int32_t)p->pulse_width + mod_pw_1023, 0, 1023);
    int32_t pw_q15  = 1638 + ((pw_1023 * 29491) >> 10); /* 1638..31129 (5%..95%) */
    pw_q15 += ((int32_t)exc->trim_pw_q15 * (256 + drift_amt)) >> 10;
    pw_q15 += exc->drift_pw_q15;
    pw_q15 = dsp_clamp_i32(pw_q15, 1310, 31457);
    exc->pw_thresh = (uint32_t)pw_q15 << 17;

    /* 4. Waveform morph */
    *out_morph_1023 = (int16_t)dsp_clamp_i32((int32_t)p->wave_morph + mod_wave_1023, 0, 1023);

    /* 5. Sample & Hold clock rate: 0..1000 -> ~20 Hz .. 22 kHz, >=1000 -> pure white noise */
    int32_t shr = dsp_clamp_i32(p->sh_rate, 0, 1023);
    if (shr != (int32_t)exc->last_sh_rate) {
        exc->last_sh_rate = (int16_t)shr;
        if (shr >= 1000) {
            exc->sh_inc = 0u; /* pure continuous white noise flag */
        } else {
            /* MIDI 16 (~20.6 Hz) .. MIDI 136 (~21.1 kHz): 120 semitones over 0..1000 */
            int32_t sh_pitch_q8 = (16 << 8) + ((shr * (120 << 8)) / 1000);
            exc->sh_inc = dsp_pitch_to_phase_inc(sh_pitch_q8);
        }
    }
}

void adrift_exciter_prepare_block(adrift_exciter_t *exc,
                                  const adrift_exciter_params_t *p,
                                  int32_t base_note_pitch_q8,
                                  int32_t mod_pitch_q8,
                                  int32_t mod_wave_1023,
                                  int32_t mod_pw_1023,
                                  int16_t *out_morph_1023) {
    adrift_exciter_prepare_block_q16(exc, p, base_note_pitch_q8 * 256, mod_pitch_q8,
                                     mod_wave_1023, mod_pw_1023, out_morph_1023);
}

DSP_ALWAYS_INLINE int32_t exciter_vco_sample(uint32_t phase,
                                             uint32_t dt,
                                             uint32_t inv_dt16,
                                             uint32_t pw_thr,
                                             int32_t k_sag,
                                             int32_t k_sym,
                                             int32_t w0_q15,
                                             int32_t w1_q15,
                                             int32_t zone) {
    if (zone == 0) {
        uint32_t p16 = phase >> 16;
        int32_t tri_lin = (p16 < 32768u)
                              ? ((int32_t)(p16 << 1) - 32768)
                              : (32767 - (int32_t)((p16 - 32768u) << 1));
        int32_t tri_2 = ((tri_lin * tri_lin) >> 15) - 10923;
        int32_t tri   = dsp_sat_q15(tri_lin - ((k_sag * tri_2) >> 16));
        int32_t z     = dsp_sat_q15(tri + ((k_sym * tri_2) >> 15));
        int32_t z2    = (z * z) >> 15;
        int32_t sine  = (z * (98304 - z2)) >> 16;
        return dsp_sat_q15(((sine * w0_q15) + (tri * w1_q15)) >> 15);
    }

    uint32_t r_phase   = phase + 0x40000000u;
    int32_t  r_lin     = (int32_t)(r_phase >> 16) - 32768; /* [-32768, 32767] */
    int32_t  blep_wrap = polyblep_edge_q15(r_phase, dt, inv_dt16);
    int32_t  r_blep    = r_lin - blep_wrap;
    int32_t  r_clamped = dsp_sat_q15(r_blep);
    int32_t  r2_zm     = ((r_clamped * r_clamped) >> 15) - 10923;
    int32_t  ramp      = dsp_sat_q15(r_blep - ((k_sag * r2_zm) >> 15));

    if (zone == 1) {
        uint32_t p16 = phase >> 16;
        int32_t tri_lin = (p16 < 32768u)
                              ? ((int32_t)(p16 << 1) - 32768)
                              : (32767 - (int32_t)((p16 - 32768u) << 1));
        int32_t tri_2 = ((tri_lin * tri_lin) >> 15) - 10923;
        int32_t tri   = dsp_sat_q15(tri_lin - ((k_sag * tri_2) >> 16));
        return dsp_sat_q15(((tri * w0_q15) + (ramp * w1_q15)) >> 15);
    } else {
        int32_t pulse     = (r_phase < pw_thr) ? -18432 : 18432;
        int32_t blep_rise = polyblep_edge_q15(r_phase - pw_thr, dt, inv_dt16);
        pulse += ((blep_rise - blep_wrap) * 9) >> 4;
        return dsp_sat_q15(((ramp * w0_q15) + (pulse * w1_q15)) >> 15);
    }
}

DSP_ALWAYS_INLINE void exciter_loop_zone(adrift_exciter_t *exc,
                                         int32_t k_sag,
                                         int32_t k_sym,
                                         int32_t w0_q15,
                                         int32_t w1_q15,
                                         int32_t vco_g_step,
                                         int32_t noise_g_step,
                                         int32_t pluck,
                                         int32_t sub_g,
                                         int32_t *out_q15,
                                         uint32_t frames,
                                         int32_t zone) {
    uint32_t phase    = exc->vco_phase;
    uint32_t dt       = exc->vco_inc;
    uint32_t inv_dt16 = exc->inv_dt16;
    uint32_t pw_thr   = exc->pw_thresh;
    int32_t  vco_g    = exc->vco_gain_q15;
    int32_t  noise_g  = exc->noise_gain_q15;
    int32_t  ac_cap   = exc->ac_cap_q23;
    int32_t  hf_slew  = exc->hf_slew_q15;
    uint32_t rng      = exc->rng_state;
    uint32_t sh_inc   = exc->sh_inc;

    if (pluck <= 0 && sh_inc == 0u) {
        for (uint32_t i = 0; i < frames; i++) {
            int32_t vco_raw = exciter_vco_sample(phase, dt, inv_dt16, pw_thr,
                                                 k_sag, k_sym, w0_q15, w1_q15, zone);
            hf_slew += ((vco_raw - hf_slew) * 29000) >> 15;
            int32_t vco_ac = hf_slew - (ac_cap >> 8);
            ac_cap += (vco_ac * 17920) >> 15;
            phase  += dt;

            int32_t wn = dsp_white_noise_q15(&rng);
            vco_g   += vco_g_step;
            noise_g += noise_g_step;
            int32_t mixed = dsp_mul_q15(vco_ac, vco_g) + dsp_mul_q15(wn, noise_g);
            out_q15[i] = dsp_sat_q15(mixed);
        }
    } else {
        uint32_t sh_phase = exc->sh_phase;
        int32_t  sh_held  = exc->sh_held_q15;
        uint32_t sub_ph   = exc->sub_phase;
        uint32_t sub_dt   = dt >> 1;
        uint32_t imp_ph   = exc->impulse_phase;
        int32_t  imp_amp  = exc->impulse_amp_q15;

        for (uint32_t i = 0; i < frames; i++) {
            int32_t vco_raw = exciter_vco_sample(phase, dt, inv_dt16, pw_thr,
                                                 k_sag, k_sym, w0_q15, w1_q15, zone);
            hf_slew += ((vco_raw - hf_slew) * 29000) >> 15;
            int32_t vco_ac = hf_slew - (ac_cap >> 8);
            ac_cap += (vco_ac * 17920) >> 15;
            phase  += dt;

            int32_t pluck_sig = 0;
            if (pluck > 0) {
                if (sub_g > 0) {
                    int32_t sub_wave = dsp_sine_lut_q15(sub_ph);
                    pluck_sig += dsp_mul_q15(sub_wave, sub_g);
                }
                sub_ph += sub_dt;
                if (imp_ph != 0u) {
                    int32_t imp_s  = dsp_sine_lut_q15(imp_ph);
                    int32_t imp_h2 = dsp_sine_lut_q15(imp_ph << 1) >> 1;
                    int32_t imp_h3 = dsp_sine_lut_q15(imp_ph * 3u) >> 2;
                    int32_t imp_h5 = dsp_sine_lut_q15(imp_ph * 5u) >> 2;
                    int32_t imp_w  = dsp_sat_q15((imp_s >> 1) + imp_h2 + imp_h3 + imp_h5);
                    pluck_sig += dsp_mul_q15(imp_w, imp_amp);
                    uint32_t next_imp = imp_ph + dt;
                    imp_ph = (next_imp < imp_ph) ? 0u : next_imp;
                }
            }

            int32_t wn = dsp_white_noise_q15(&rng);
            int32_t noise_sample;
            if (sh_inc == 0u) {
                noise_sample = wn;
            } else {
                uint32_t next_sh = sh_phase + sh_inc;
                if (next_sh < sh_phase) {
                    sh_held = wn;
                }
                sh_phase = next_sh;
                noise_sample = sh_held;
            }

            vco_g   += vco_g_step;
            noise_g += noise_g_step;
            int32_t mixed = dsp_mul_q15(vco_ac, vco_g) + dsp_mul_q15(noise_sample, noise_g) + pluck_sig;
            out_q15[i] = dsp_sat_q15(mixed);
        }

        exc->sub_phase     = sub_ph;
        exc->impulse_phase = imp_ph;
        exc->sh_phase      = sh_phase;
        exc->sh_held_q15   = sh_held;
    }

    exc->vco_phase   = phase;
    exc->ac_cap_q23  = ac_cap;
    exc->hf_slew_q15 = hf_slew;
    exc->rng_state   = rng;
}

static DSP_HOT_RAM void exciter_loop_z0(adrift_exciter_t *exc,
                                        int32_t k_sag,
                                        int32_t k_sym,
                                        int32_t w0_q15,
                                        int32_t w1_q15,
                                        int32_t vco_g_step,
                                        int32_t noise_g_step,
                                        int32_t pluck,
                                        int32_t sub_g,
                                        int32_t *out_q15,
                                        uint32_t frames) {
    exciter_loop_zone(exc, k_sag, k_sym, w0_q15, w1_q15,
                      vco_g_step, noise_g_step, pluck, sub_g, out_q15, frames, 0);
}

static DSP_HOT_RAM void exciter_loop_z1(adrift_exciter_t *exc,
                                        int32_t k_sag,
                                        int32_t w0_q15,
                                        int32_t w1_q15,
                                        int32_t vco_g_step,
                                        int32_t noise_g_step,
                                        int32_t pluck,
                                        int32_t sub_g,
                                        int32_t *out_q15,
                                        uint32_t frames) {
    exciter_loop_zone(exc, k_sag, 0, w0_q15, w1_q15,
                      vco_g_step, noise_g_step, pluck, sub_g, out_q15, frames, 1);
}

static DSP_HOT_RAM void exciter_loop_z2(adrift_exciter_t *exc,
                                        int32_t k_sag,
                                        int32_t w0_q15,
                                        int32_t w1_q15,
                                        int32_t vco_g_step,
                                        int32_t noise_g_step,
                                        int32_t pluck,
                                        int32_t sub_g,
                                        int32_t *out_q15,
                                        uint32_t frames) {
    exciter_loop_zone(exc, k_sag, 0, w0_q15, w1_q15,
                      vco_g_step, noise_g_step, pluck, sub_g, out_q15, frames, 2);
}

void adrift_exciter_process(adrift_exciter_t *exc,
                            const adrift_exciter_params_t *p,
                            int16_t morph_1023,
                            int32_t *out_q15,
                            uint32_t frames) {
    int32_t drift_scale = 256 + dsp_clamp_i32(p->analog_drift, 0, 1023); /* 256..1279 */
    int32_t k_sag = ((int32_t)exc->trim_sag_q15 * drift_scale) >> 9;     /* ~1000..6500 in Q15 */
    int32_t k_sym = ((int32_t)exc->trim_sym_q15 * drift_scale) >> 9;

    int32_t m_clamped = dsp_clamp_i32(morph_1023, 0, 1023);
    int32_t zone = 0;
    int32_t mix_q15 = 0;
    int32_t g0_q15 = 28836;
    int32_t g1_q15 = 32768;
    int32_t arch_b = 1200;
    if (m_clamped < 341) {
        zone    = 0; /* Sine -> Triangle */
        mix_q15 = (int32_t)dsp_udiv_1023((uint32_t)m_clamped * 98304u);
        g0_q15  = 28836; /* 0.88x (-1.11 dB) */
        g1_q15  = 32768; /* 1.00x ( 0.00 dB) */
        arch_b  = 1200;
    } else if (m_clamped < 682) {
        zone    = 1; /* Triangle -> Sagged Ramp */
        mix_q15 = (int32_t)dsp_udiv_1023((uint32_t)(m_clamped - 341) * 98304u);
        g0_q15  = 32768; /* 1.00x ( 0.00 dB) */
        g1_q15  = 27525; /* 0.84x (-1.51 dB) */
        arch_b  = 4200;
    } else {
        zone    = 2; /* Sagged Ramp -> PWM Pulse */
        mix_q15 = (int32_t)dsp_udiv_1023((uint32_t)(m_clamped - 682) * 98304u);
        int32_t pw_dev      = dsp_clamp_i32(p->pulse_width, 0, 1023) - 512;
        int32_t pw_comp_q15 = 32768 + ((pw_dev * pw_dev) >> 14); /* Up to +50% on extreme 8%/92% PWM */
        g0_q15  = 27525; /* 0.84x (-1.51 dB) */
        g1_q15  = (28836 * pw_comp_q15) >> 15;
        arch_b  = 3400;
    }
    int32_t inv_mix_q15 = 32768 - mix_q15;
    int32_t parabola    = (mix_q15 * inv_mix_q15) >> 13; /* 4 * m * (1 - m) in Q15 (0..32768) */
    int32_t arch_q15    = 32768 + ((arch_b * parabola) >> 15);
    int32_t w0_q15      = dsp_mul_q15(dsp_mul_q15(inv_mix_q15, g0_q15), arch_q15);
    int32_t w1_q15      = dsp_mul_q15(dsp_mul_q15(mix_q15,     g1_q15), arch_q15);

    int32_t exc_lvl_q15    = (int32_t)dsp_udiv_1023((uint32_t)dsp_clamp_i32(p->vco_level, 0, 1023) * 32767u);
    int32_t mix_1023       = dsp_clamp_i32(p->noise_level, 0, 1023);
    int32_t target_vco_g   = (int32_t)dsp_udiv_1023((uint32_t)exc_lvl_q15 * (uint32_t)(1023 - mix_1023));
    int32_t target_noise_g = (int32_t)dsp_udiv_1023((uint32_t)exc_lvl_q15 * (uint32_t)mix_1023);
    if (frames == 0u) {
        exc->vco_gain_q15   = target_vco_g;
        exc->noise_gain_q15 = target_noise_g;
        return;
    }

    int32_t vco_g_step   = dsp_div_frames_i32(target_vco_g - exc->vco_gain_q15, frames);
    int32_t noise_g_step = dsp_div_frames_i32(target_noise_g - exc->noise_gain_q15, frames);
    int32_t pluck        = dsp_clamp_i32(p->pluck, 0, 1023);
    int32_t sub_g        = (target_vco_g * pluck) >> 11; /* Up to +50% -1 Oct Sub-Oscillator body */

    if (zone == 0) {
        DSP_CALL_RAM(exciter_loop_z0)(exc, k_sag, k_sym, w0_q15, w1_q15,
                                      vco_g_step, noise_g_step, pluck, sub_g, out_q15, frames);
    } else if (zone == 1) {
        DSP_CALL_RAM(exciter_loop_z1)(exc, k_sag, w0_q15, w1_q15,
                                      vco_g_step, noise_g_step, pluck, sub_g, out_q15, frames);
    } else {
        DSP_CALL_RAM(exciter_loop_z2)(exc, k_sag, w0_q15, w1_q15,
                                      vco_g_step, noise_g_step, pluck, sub_g, out_q15, frames);
    }

    exc->vco_gain_q15   = target_vco_g;
    exc->noise_gain_q15 = target_noise_g;
}
