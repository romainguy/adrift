#include "bbd.h"

void adrift_bbd_init(adrift_bbd_t *bbd, uint8_t voice_idx) {
    for (uint32_t i = 0; i < ADRIFT_BBD_BUF_LEN; i++) {
        bbd->buf[i] = 0;
    }
    bbd->write_idx         = 0u;
    bbd->delay_q16         = 168 * 65536;
    bbd->target_delay_q16  = 168 * 65536;
    bbd->slew_pitch_q16    = (60 << 16);
    bbd->clk_drift_q16     = 0;
    bbd->prev_fb_q15        = 0;
    bbd->pitch_initialized  = false;
    bbd->delay_initialized  = false;
    bbd->steal_damp_pending = false;
    bbd->steal_tail_q15     = 0;
    bbd->steal_step_q15     = 0;
    adrift_onepole_init(&bbd->fb_lpf);
    adrift_onepole_init(&bbd->aa_lpf);
    adrift_onepole_init(&bbd->clk_lpf);
    bbd->disp_x1           = 0;
    bbd->disp_y1           = 0;
    bbd->disp_x2           = 0;
    bbd->disp_y2           = 0;
    bbd->disp_r_q15        = 0u;
    bbd->dc_cap_q23        = 0;
    bbd->fb_gain_q14       = 0;
    bbd->noise_rng         = 0xA5A5F00Du ^ ((uint32_t)(voice_idx + 1u) * 0x27D4EB2Du);
    bbd->peak_wet_q15      = 0;
    bbd->delay_cache_valid = false;
}

void adrift_bbd_reset(adrift_bbd_t *bbd) {
    for (uint32_t i = 0; i < ADRIFT_BBD_BUF_LEN; i++) {
        bbd->buf[i] = 0;
    }
    bbd->write_idx         = 0u;
    bbd->clk_drift_q16     = 0;
    bbd->prev_fb_q15       = 0;
    adrift_onepole_reset(&bbd->fb_lpf);
    adrift_onepole_reset(&bbd->aa_lpf);
    adrift_onepole_reset(&bbd->clk_lpf);
    bbd->disp_x1           = 0;
    bbd->disp_y1           = 0;
    bbd->disp_x2           = 0;
    bbd->disp_y2           = 0;
    bbd->dc_cap_q23        = 0;
    bbd->peak_wet_q15       = 0;
    bbd->pitch_initialized  = false;
    bbd->delay_initialized  = false;
    bbd->steal_damp_pending = false;
    bbd->steal_tail_q15     = 0;
    bbd->steal_step_q15     = 0;
    bbd->delay_cache_valid  = false;
}

int32_t adrift_bbd_step_glide(adrift_bbd_t *bbd,
                              int16_t glide,
                              int32_t target_note_pitch_q8,
                              uint32_t frames) {
    int32_t dest_q16 = target_note_pitch_q8 * 256;
    if (!bbd->pitch_initialized || glide <= 2) {
        bbd->slew_pitch_q16    = dest_q16;
        bbd->pitch_initialized = true;
    } else if (frames > 0u) {
        int32_t diff = dest_q16 - bbd->slew_pitch_q16;
        if (diff != 0) {
            /* Exponential pitch slew: glide 3..1023 -> ~5 ms .. ~2.5 s, scaled by sub-block frames.
             * Symmetric minimum 1-ULP step prevents IIR truncation deadband on upward/sub-block glides. */
            int32_t g         = dsp_clamp_i32(glide, 3, 1023);
            int32_t g_inv     = 1026 - g; /* 3..1023 */
            int32_t rate_q15  = 18 + (((g_inv * g_inv) >> 10) * 16000 >> 10);
            int32_t f_clamped = (frames > 256u) ? 256 : (int32_t)frames;
            int32_t step_q16  = (rate_q15 * f_clamped + 16) >> 5; /* == rate_q15 << 1 when frames == 64 */
            if (step_q16 < 1) step_q16 = 1;
            if (step_q16 > 65536) step_q16 = 65536;

            int32_t adiff = (diff < 0) ? -diff : diff;
            int32_t delta = dsp_mul_q16(adiff, step_q16);
            if (delta < 1) delta = 1;
            if (delta > adiff) delta = adiff;
            bbd->slew_pitch_q16 += (diff < 0) ? -delta : delta;
        }
    }
    return (bbd->slew_pitch_q16 >= 0)
               ? ((bbd->slew_pitch_q16 + 128) >> 8)
               : -(((-bbd->slew_pitch_q16) + 128) >> 8);
}

void adrift_bbd_prepare_block_ex(adrift_bbd_t *bbd,
                                 const adrift_bbd_params_t *p,
                                 int32_t target_note_pitch_q8,
                                 int32_t mod_pitch_q8,
                                 int32_t mod_fb_1023,
                                 int32_t mod_fb_cutoff_1023,
                                 uint32_t frames,
                                 int32_t *out_target_fb_q14,
                                 bool step_glide) {
    /* 1a. Frame-invariant Portamento / Glide on base note pitch */
    if (step_glide) {
        (void)adrift_bbd_step_glide(bbd, p->glide, target_note_pitch_q8, frames);
    }

    /* 1b. MN3101-style BBD analog clock oscillator Ornstein-Uhlenbeck thermal wander */
    int32_t noise_k = dsp_clamp_i32(p->bbd_noise, 0, 1023);
    if (noise_k == 0) {
        bbd->clk_drift_q16 = 0;
    } else if (frames > 0u) {
        int32_t wn = dsp_white_noise_q15(&bbd->noise_rng);
        bbd->clk_drift_q16 -= (bbd->clk_drift_q16 >> 8);
        bbd->clk_drift_q16 += (wn * noise_k) >> 12;
    }

    int32_t pitch_q16 = bbd->slew_pitch_q16 + (mod_pitch_q8 * 256) + (bbd->clk_drift_q16 >> 7);
    /* Optional +1 octave delay halving when Invert is active so hollow odd harmonics stay at key pitch */
    int32_t delay_pitch_q16 = (p->invert && p->invert_pitch_comp) ? (pitch_q16 + (12 << 16)) : pitch_q16;

    /* Sounding fundamental frequency at which loop phase delay must equal 2*pi (or pi when inverted):
     * - Normal mode: fundamental is at pitch_q16 (period = T_delay)
     * - Invert w/o pitch comp: fundamental drops 1 octave to pitch_q16 - 12 semitones (period = 2 * T_delay)
     * - Invert w/ +1 oct comp: fundamental stays at pitch_q16 (T_delay = T_0 / 2) */
    int32_t sounding_pitch_q16 = (p->invert && !p->invert_pitch_comp) ? (pitch_q16 - (12 << 16)) : pitch_q16;
    int32_t fb_cut             = dsp_clamp_i32((int32_t)p->fb_lpf_cutoff + mod_fb_cutoff_1023, 0, 960);
    int32_t disp_k             = dsp_clamp_i32(p->dispersion, 0, 1023);

    const int32_t min_delay_q16 = 4 * 65536;
    const int32_t max_delay_q16 = (int32_t)(ADRIFT_BBD_BUF_LEN - 5u) * 65536;

    if (!bbd->delay_cache_valid ||
        bbd->last_delay_pitch_q16    != delay_pitch_q16 ||
        bbd->last_sounding_pitch_q16 != sounding_pitch_q16 ||
        bbd->last_fb_cut             != (int16_t)fb_cut ||
        bbd->last_disp_k             != (int16_t)disp_k) {
        bbd->delay_cache_valid       = true;
        bbd->last_delay_pitch_q16    = delay_pitch_q16;
        bbd->last_sounding_pitch_q16 = sounding_pitch_q16;
        bbd->last_fb_cut             = (int16_t)fb_cut;
        bbd->last_disp_k             = (int16_t)disp_k;

        uint32_t raw_period_q16 = dsp_pitch_q16_to_period_q16(delay_pitch_q16);
        uint32_t f0_phase_inc   = dsp_pitch_q16_to_phase_inc(sounding_pitch_q16);
        int32_t  delay_pitch_q8 = (delay_pitch_q16 >= 0)
                                      ? ((delay_pitch_q16 + 128) >> 8)
                                      : -(((-delay_pitch_q16) + 128) >> 8);

        /* 2. 6 dB/oct Feedback Low-Pass Filter bounded by 1024-stage MOSFET CTI ceiling (~14 kHz):
         * Even when the external feedback LPF is wide open (1023), cumulative Charge-Transfer
         * Inefficiency (N*eps ~= 0.15) across 1024 bucket transfers limits per-pass loop bandwidth. */
        adrift_onepole_set_cutoff(&bbd->fb_lpf, fb_cut);

        /* 3. Authentic 1024-stage BBD 2-Pole (12 dB/oct) Anti-Aliasing + Reconstruction Filter:
         * A 1024-stage BBD has 512 sample buckets of delay: f_clk = 512 / T_delay, Nyquist = 256 / T_delay.
         * In semitones, 256 * f_delay = +8 octaves (+96 semitones) above delay_pitch_q8!
         * On low bass notes (e.g. C1 = 32.7 Hz, delay = 1349 samples -> BBD Nyquist = 8.37 kHz < 22 kHz),
         * both the input anti-aliasing filter (aa_lpf) and output reconstruction filter (clk_lpf) roll off
         * highs at 12 dB/oct proportionally to the 1024-stage clock rate. */
        int32_t clk_nyq_pitch_q8 = delay_pitch_q8 + (94 << 8); /* ~228x f_delay anti-imaging corner */
        if (clk_nyq_pitch_q8 >= (135 << 8)) {
            /* Clock Nyquist exceeds 20 kHz: transparent pass-through */
            adrift_onepole_set_alpha(&bbd->aa_lpf, 32767u);
            adrift_onepole_set_alpha(&bbd->clk_lpf, 32767u);
        } else {
            /* Map Q8 pitch above C1 (24<<8) directly to exponential cutoff table [0..1023] (1 - exp(-2*pi*fc/fs)) */
            int32_t clk_cut = dsp_clamp_i32(((clk_nyq_pitch_q8 - (24 << 8)) * 2342) >> 16, 0, 1022);
            adrift_onepole_set_cutoff(&bbd->aa_lpf, clk_cut);
            adrift_onepole_set_cutoff(&bbd->clk_lpf, clk_cut);
        }

        /* 4. Exact Group/Phase-Delay Pitch Compensation:
         * Subtract the exact discrete phase delay of fb_lpf and the 2-pole BBD clock/CTI filters
         * (aa_lpf + clk_lpf = 2 * clk_lag_q16), and add the exact phase lead T0^2 / (16384 * 4*pi^2)
         * of the 0.43 Hz loop DC-blocker at the sounding fundamental so BBD pitch stays within < 1.5 cents! */
        uint32_t fb_lag_q16    = adrift_onepole_phase_delay_q16(bbd->fb_lpf.a_q15, f0_phase_inc);
        uint32_t clk_lag_q16   = adrift_onepole_phase_delay_q16(bbd->clk_lpf.a_q15, f0_phase_inc);
        uint32_t t0_period_q16 = (sounding_pitch_q16 == delay_pitch_q16)
                                     ? raw_period_q16
                                     : dsp_pitch_q16_to_period_q16(sounding_pitch_q16);
        uint32_t t0_int        = t0_period_q16 >> 16;
        if (t0_int > 4096u) t0_int = 4096u;
        uint32_t hpf_lead_q16  = (t0_int * t0_int * 13u) >> 7;

        /* 4b. Loop All-Pass Dispersion (Stiffness / Inharmonicity):
         * 2-stage pitch-tracked 1st-order all-pass cascade H_ap(z) = ((-r + z^-1)/(1 - r*z^-1))^2 inside the BBD loop.
         * Exact phase delay at f0 is 2 + 4 * tau_lp(1 - r, w0) samples; subtracting it from net_period_q16
         * keeps the fundamental f0 locked in tune (< 1.5 cents) while stretching upper partials sharp! */
        uint32_t disp_lag_q16 = 0u;
        if (disp_k > 0) {
            uint32_t delay_inc  = (delay_pitch_q16 == sounding_pitch_q16)
                                      ? f0_phase_inc
                                      : dsp_pitch_q16_to_phase_inc(delay_pitch_q16);
            uint32_t omega0_q15 = ((delay_inc >> 16) * 402u) >> 7; /* 2*pi*f_delay/fs in Q15 */
            if (omega0_q15 < 64u) omega0_q15 = 64u;
            uint32_t inv_disp   = 1024u - (uint32_t)disp_k;
            uint32_t kappa_q4   = 38u + ((inv_disp * 220u) >> 10); /* fc/f0 in [2.375 .. 16.1] */
            uint32_t a_q15      = (omega0_q15 * kappa_q4) >> 4;
            if (a_q15 < 1024u)  a_q15 = 1024u;
            if (a_q15 > 28672u) a_q15 = 28672u;
            uint32_t r_q15      = 32768u - a_q15;
            bbd->disp_r_q15 = (uint16_t)r_q15;
            disp_lag_q16    = (2u << 16) + (adrift_onepole_phase_delay_q16((uint16_t)a_q15, f0_phase_inc) << 2);
        } else {
            bbd->disp_r_q15 = 0u;
        }

        int32_t net_period_q16 = (int32_t)raw_period_q16 + (int32_t)hpf_lead_q16 -
                                 (int32_t)(fb_lag_q16 + (clk_lag_q16 << 1) + disp_lag_q16);

        /* Clamp compensated delay to [4.0 .. 2043.0] samples (Hermite reads -1..+2 around D_int) */
        bbd->target_delay_q16 = dsp_clamp_i32(net_period_q16, min_delay_q16, max_delay_q16);
    }

    if (bbd->steal_damp_pending) {
        bbd->steal_damp_pending = false;
        bbd->delay_q16          = bbd->target_delay_q16;
        bbd->delay_initialized  = true;
        int32_t y_cur           = dsp_bbd_saturate_q15((bbd->clk_lpf.state_q29 + 8192) >> 14);
        bbd->steal_tail_q15     = y_cur;
        bbd->steal_step_q15     = (y_cur >= 0) ? (int16_t)((y_cur >> 5) + 1)
                                               : (int16_t)(-(((-y_cur) >> 5) + 1));
        for (uint32_t j = 0; j < ADRIFT_BBD_BUF_LEN; j++) {
            bbd->buf[j] = 0;
        }
        adrift_onepole_reset(&bbd->fb_lpf);
        adrift_onepole_reset(&bbd->aa_lpf);
        adrift_onepole_reset(&bbd->clk_lpf);
        bbd->prev_fb_q15 = 0;
        bbd->disp_x1     = 0;
        bbd->disp_y1     = 0;
        bbd->disp_x2     = 0;
        bbd->disp_y2     = 0;
        bbd->dc_cap_q23  = 0;
    } else if (!bbd->delay_initialized || bbd->delay_q16 < min_delay_q16 || bbd->delay_q16 > max_delay_q16) {
        bbd->delay_q16         = bbd->target_delay_q16;
        bbd->delay_initialized = true;
    }

    /* 5. Feedback VCA Gain:
     * Knob 0..1023 maps smoothly to 0.0 .. 1.152 in Q14 (16384 = 100.0% unity loop gain):
     * - 0..890   -> 0 .. 16384 (0% .. 100.0% resonator decay)
     * - 890..1023 -> 16384 .. 18874 (100.0% .. 115.2% self-oscillation region!) */
    int32_t fb_k = dsp_clamp_i32((int32_t)p->feedback + mod_fb_1023, 0, 1023);
    int32_t fb_q14;
    if (fb_k <= 890) {
        fb_q14 = (int32_t)(((uint32_t)fb_k * 2412903u) >> 17);
    } else {
        fb_q14 = 16384 + (int32_t)(((uint32_t)(fb_k - 890) * 306739u) >> 14);
    }
    *out_target_fb_q14 = fb_q14;
}

void adrift_bbd_prepare_block(adrift_bbd_t *bbd,
                              const adrift_bbd_params_t *p,
                              int32_t target_note_pitch_q8,
                              int32_t mod_pitch_q8,
                              int32_t mod_fb_1023,
                              int32_t mod_fb_cutoff_1023,
                              uint32_t frames,
                              int32_t *out_target_fb_q14) {
    adrift_bbd_prepare_block_ex(bbd, p, target_note_pitch_q8, mod_pitch_q8,
                                mod_fb_1023, mod_fb_cutoff_1023, frames, out_target_fb_q14, true);
}

DSP_ALWAYS_INLINE int32_t bbd_onepole_step(int32_t *state_q29, int32_t a_q15, int32_t x_q15) {
    int32_t x_q29   = (int32_t)dsp_sat16(x_q15) * 16384;
    int32_t diff    = x_q29 - *state_q29;
    int32_t step_hi = (diff >> 15) * a_q15;
    int32_t step_lo = ((diff & 0x7FFF) * a_q15) >> 15;
    *state_q29     += step_hi + step_lo;
    return (*state_q29 + 8192) >> 14;
}

DSP_ALWAYS_INLINE void bbd_loop_impl(adrift_bbd_t *bbd,
                                     bool invert,
                                     int32_t d_step,
                                     int32_t fb_step,
                                     int32_t disp_fm_k,
                                     int32_t noise_q6,
                                     const int32_t *in_q15,
                                     int32_t *wet_out_q15,
                                     uint32_t frames,
                                     bool use_disp) {
    int16_t *const buf = bbd->buf;
    uint32_t widx      = bbd->write_idx;
    int32_t  cur_d     = bbd->delay_q16;
    int32_t  fb_g      = bbd->fb_gain_q14;
    int32_t  dc_cap    = bbd->dc_cap_q23;
    int32_t  prev_fb   = bbd->prev_fb_q15;
    int32_t  clk_st    = bbd->clk_lpf.state_q29;
    int32_t  clk_a     = (int32_t)bbd->clk_lpf.a_q15;
    int32_t  fb_st     = bbd->fb_lpf.state_q29;
    int32_t  fb_a      = (int32_t)bbd->fb_lpf.a_q15;
    int32_t  aa_st     = bbd->aa_lpf.state_q29;
    int32_t  aa_a      = (int32_t)bbd->aa_lpf.a_q15;
    int32_t  disp_r    = (int32_t)bbd->disp_r_q15;
    int32_t  dx1       = bbd->disp_x1;
    int32_t  dy1       = bbd->disp_y1;
    int32_t  dx2       = bbd->disp_x2;
    int32_t  dy2       = bbd->disp_y2;
    uint32_t rng       = bbd->noise_rng;
    int32_t  peak      = bbd->peak_wet_q15 - (bbd->peak_wet_q15 >> 5);

    const int32_t min_delay_q16 = 4 * 65536;
    const int32_t max_delay_q16 = (int32_t)(ADRIFT_BBD_BUF_LEN - 5u) * 65536;

    for (uint32_t i = 0; i < frames; i++) {
        cur_d += d_step;
        fb_g  += fb_step;
        int32_t x_in = in_q15[i];

        /* 1. 1024-stage MOSFET bucket-capacitance C(V) dispersion + 4-point Catmull-Rom fractional read */
        int32_t disp_q16 = (prev_fb * (cur_d >> 16)) >> 9;
        if (use_disp) {
            disp_q16 += (x_in * disp_fm_k) >> 3;
        }
        int32_t  eff_d = dsp_clamp_i32(cur_d + disp_q16, min_delay_q16, max_delay_q16);
        uint32_t d_int = (uint32_t)(eff_d >> 16);
        int32_t  f_q11 = (eff_d >> 5) & 0x7FF; /* [0 .. 2047] */

        uint32_t i0  = (widx - d_int) & ADRIFT_BBD_BUF_MASK;
        int32_t  ym1 = buf[(i0 + 1u) & ADRIFT_BBD_BUF_MASK];
        int32_t  y0  = buf[i0];
        int32_t  y1  = buf[(i0 - 1u) & ADRIFT_BBD_BUF_MASK];
        int32_t  y2  = buf[(i0 - 2u) & ADRIFT_BBD_BUF_MASK];

        int32_t c1_q1 = y1 - ym1;
        int32_t c2_q1 = (ym1 * 2) - (y0 * 5) + (y1 * 4) - y2;
        int32_t c3_q1 = (y2 - ym1) + (y0 - y1) * 3;

        int32_t acc = (c3_q1 * f_q11) >> 11;
        acc = ((acc + c2_q1) * f_q11) >> 11;
        acc = (((acc + c1_q1) * f_q11) + 2048) >> 12;
        int32_t delayed = y0 + acc;

        /* 2. 1024-stage BBD output reconstruction/CTI filter + bucket-well soft saturator */
        int32_t clk_filtered = bbd_onepole_step(&clk_st, clk_a, delayed);
        int32_t companded    = (int32_t)dsp_bbd_saturate_q15(clk_filtered);
        wet_out_q15[i]       = companded;

        int32_t abs_w = companded < 0 ? -companded : companded;
        if (abs_w > peak) peak = abs_w;

        /* 3. 6 dB/oct Feedback Low-Pass Filter + 2-Stage All-Pass Dispersion + 0.43 Hz DC-blocker */
        int32_t fb_damped = bbd_onepole_step(&fb_st, fb_a, companded);
        if (use_disp && disp_r > 0) {
            int32_t x0    = fb_damped * 256; /* Q15 -> Q23 (8 sub-LSB fractional bits) */
            int32_t diff1 = dy1 - x0;
            int32_t ap1   = dx1 + (diff1 >> 15) * disp_r + (((diff1 & 0x7FFF) * disp_r + 16384) >> 15);
            dx1 = x0;
            dy1 = ap1;

            int32_t diff2 = dy2 - ap1;
            int32_t ap2   = dx2 + (diff2 >> 15) * disp_r + (((diff2 & 0x7FFF) * disp_r + 16384) >> 15);
            dx2 = ap1;
            dy2 = ap2;

            fb_damped = dsp_sat_q15((ap2 + 128) >> 8);
        }
        int32_t fb_q16    = fb_damped * 2;
        int32_t fb_ac_q16 = fb_q16 - (dc_cap >> 14);
        dc_cap += fb_ac_q16;
        int32_t fb_ac     = (fb_ac_q16 + 1) >> 1;
        prev_fb           = fb_ac;

        /* 4. Feedback VCA (0..115% in Q14) & Polarity Invert (+1 / -1) */
        int32_t fb_sig = dsp_mul_q14(fb_ac, fb_g);
        if (invert) {
            fb_sig = -fb_sig;
        }

        /* 5. Unbiased zero-mean BBD thermal/clock noise floor (~-78 dBFS) seeds self-oscillation */
        int32_t thermal = (dsp_white_noise_q15(&rng) * noise_q6 + 1048576) >> 21;

        /* 6. Sum exciter + feedback + thermal floor -> 1024-stage input anti-aliasing/CTI LPF -> bucket well */
        int32_t bbd_in      = x_in + fb_sig + thermal;
        int32_t aa_filtered = bbd_onepole_step(&aa_st, aa_a, bbd_in);
        buf[widx]           = dsp_bbd_saturate_q15(aa_filtered);
        widx = (widx + 1u) & ADRIFT_BBD_BUF_MASK;
    }

    bbd->write_idx         = (uint16_t)widx;
    bbd->clk_lpf.state_q29 = clk_st;
    bbd->fb_lpf.state_q29  = fb_st;
    bbd->aa_lpf.state_q29  = aa_st;
    if (use_disp) {
        bbd->disp_x1 = dx1;
        bbd->disp_y1 = dy1;
        bbd->disp_x2 = dx2;
        bbd->disp_y2 = dy2;
    }
    bbd->dc_cap_q23   = dc_cap;
    bbd->prev_fb_q15  = prev_fb;
    bbd->noise_rng    = rng;
    bbd->peak_wet_q15 = peak;
}

static DSP_HOT_RAM void bbd_loop_nodisp(adrift_bbd_t *bbd,
                                        bool invert,
                                        int32_t d_step,
                                        int32_t fb_step,
                                        int32_t noise_q6,
                                        const int32_t *in_q15,
                                        int32_t *wet_out_q15,
                                        uint32_t frames) {
    bbd_loop_impl(bbd, invert, d_step, fb_step, 0, noise_q6, in_q15, wet_out_q15, frames, false);
}

static DSP_HOT_RAM void bbd_loop_disp(adrift_bbd_t *bbd,
                                      bool invert,
                                      int32_t d_step,
                                      int32_t fb_step,
                                      int32_t disp_fm_k,
                                      int32_t noise_q6,
                                      const int32_t *in_q15,
                                      int32_t *wet_out_q15,
                                      uint32_t frames) {
    bbd_loop_impl(bbd, invert, d_step, fb_step, disp_fm_k, noise_q6, in_q15, wet_out_q15, frames, true);
}

void adrift_bbd_process(adrift_bbd_t *bbd,
                        const adrift_bbd_params_t *p,
                        int32_t target_fb_q14,
                        const int32_t *in_q15,
                        int32_t *wet_out_q15,
                        uint32_t frames) {
    if (frames == 0u) {
        bbd->delay_q16   = bbd->target_delay_q16;
        bbd->fb_gain_q14 = target_fb_q14;
        return;
    }

    int32_t cur_d     = bbd->delay_q16;
    int32_t d_step    = dsp_div_frames_i32(bbd->target_delay_q16 - cur_d, frames);
    int32_t fb_step   = dsp_div_frames_i32(target_fb_q14 - bbd->fb_gain_q14, frames);
    int32_t disp_fm_k = (dsp_clamp_i32(p->dispersion, 0, 1023) * (cur_d >> 16)) >> 13;
    int32_t noise_q6  = dsp_clamp_i32(p->bbd_noise, 0, 1023); /* 0..1023 -> 0..16 LSB unbiased */

    if (bbd->disp_r_q15 == 0u && disp_fm_k == 0) {
        DSP_CALL_RAM(bbd_loop_nodisp)(bbd, p->invert, d_step, fb_step, noise_q6, in_q15, wet_out_q15, frames);
    } else {
        DSP_CALL_RAM(bbd_loop_disp)(bbd, p->invert, d_step, fb_step, disp_fm_k, noise_q6, in_q15, wet_out_q15, frames);
    }

    if (bbd->steal_tail_q15 != 0) {
        int32_t tail = bbd->steal_tail_q15;
        int32_t step = bbd->steal_step_q15;
        for (uint32_t i = 0; i < frames && tail != 0; i++) {
            wet_out_q15[i] = dsp_sat_q15(wet_out_q15[i] + tail);
            if (tail > 0) {
                tail -= step;
                if (tail < 0) tail = 0;
            } else {
                tail -= step;
                if (tail > 0) tail = 0;
            }
        }
        bbd->steal_tail_q15 = (int16_t)tail;
    }

    bbd->delay_q16   = bbd->target_delay_q16;
    bbd->fb_gain_q14 = target_fb_q14;
}
