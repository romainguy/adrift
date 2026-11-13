#include "fx.h"

#define ADRIFT_FX_CHUNK 64u

DSP_ALWAYS_INLINE int32_t fx_abs_i32(int32_t x) {
    return x < 0 ? -x : x;
}

DSP_ALWAYS_INLINE int32_t fx_read_interp_q8(const int16_t *buf,
                                            uint32_t mask,
                                            uint32_t w_idx,
                                            int32_t delay_q8) {
    if (delay_q8 < 256) delay_q8 = 256;
    uint32_t int_part = (uint32_t)(delay_q8 >> 8);
    int32_t  frac_q8  = delay_q8 & 0xFF;
    uint32_t idx0     = (w_idx - int_part) & mask;
    uint32_t idx1     = (idx0 - 1u) & mask;
    int32_t  s0       = buf[idx0];
    int32_t  s1       = buf[idx1];
    return s0 + (((s1 - s0) * frac_q8) >> 8);
}

void adrift_fx_init(adrift_fx_state_t *fx) {
    adrift_fx_reset(fx);
}

void adrift_fx_reset(adrift_fx_state_t *fx) {
    uint32_t saved_clk = fx->clk_period_q4;
    uint8_t *raw = (uint8_t *)fx;
    for (uint32_t i = 0; i < (uint32_t)sizeof(*fx); ++i) {
        raw[i] = 0u;
    }
    fx->delay_len_q8  = (4412 << 8); /* Default ~100 ms in Q8 samples */
    fx->lfo_phase_2   = 0x40000000u; /* 90-degree quadrature offset */
    fx->comp_gain_q14 = 16384;       /* Unity VCA gain (1.0x in Q14) */
    fx->clk_period_q4 = saved_clk;
}

bool adrift_fx_is_active(const adrift_fx_params_t *p) {
    return (p->chorus_mix > 0 || p->delay_mix > 0 || p->reverb_mix > 0 || p->comp_mix > 0);
}

uint32_t adrift_fx_delay_time_ms(int32_t delay_time_1023) {
    int32_t dt = dsp_clamp_i32(delay_time_1023, 0, 1023);
    return 20u + (((uint32_t)dt * 340u) >> 10);
}

static const uint8_t k_delay_subdiv_pulses[8] = {
    3u,  /* 1/32  (3 pulses at 24 PPQN) */
    4u,  /* 1/16T (4 pulses) */
    6u,  /* 1/16  (6 pulses) */
    8u,  /* 1/8T  (8 pulses) */
    9u,  /* 1/16D (9 pulses, dotted 1/16th) */
    12u, /* 1/8   (12 pulses) */
    18u, /* 1/8D  (18 pulses, dotted 1/8th) */
    24u  /* 1/4   (24 pulses, quarter note) */
};

static const char *const k_delay_subdiv_names[8] = {
    "1/32", "1/16T", "1/16", "1/8T", "1/16D", "1/8", "1/8D", "1/4"
};

uint32_t adrift_fx_delay_subdiv_pulses(int32_t delay_time_1023) {
    int32_t dt = dsp_clamp_i32(delay_time_1023, 0, 1023);
    return k_delay_subdiv_pulses[(dt * 8) >> 10];
}

const char *adrift_fx_delay_subdiv_name(int32_t delay_time_1023) {
    int32_t dt = dsp_clamp_i32(delay_time_1023, 0, 1023);
    return k_delay_subdiv_names[(dt * 8) >> 10];
}

int32_t adrift_fx_comp_thresh_db(int32_t thresh_1023) {
    int32_t t = dsp_clamp_i32(thresh_1023, 0, 1023);
    return -24 + ((t * 22 + 511) >> 10);
}

uint32_t adrift_fx_comp_attack_ms(int32_t attack_1023) {
    uint32_t a = (uint32_t)dsp_clamp_i32(attack_1023, 0, 1023);
    return 1u + ((a * a * 49u + 524288u) >> 20);
}

typedef struct {
    int16_t s1_l;
    int16_t s1_r;
    int16_t s2_l;
    int16_t s2_r;
} fx_lfo_smp_t;

typedef struct {
    int32_t l;
    int32_t r;
} fx_stereo_i32_t;

typedef struct {
    int32_t chorus_depth_q8;
    int32_t chorus_wet_q15;
    int32_t target_delay_q8;
    int32_t delay_fb_q15;
    int32_t delay_wet_q15;
    int32_t rvb_fb_q15;
    int32_t rvb_damp_q15;
    int32_t rvb_shim_q15;
    int32_t rvb_wet_q15;
    int32_t comp_thresh_l2;
    int32_t comp_makeup_l2;
    int32_t comp_atk_q15;
    int32_t comp_wet_q15;
} fx_block_coefs_t;

typedef struct {
    fx_block_coefs_t coefs;
    fx_lfo_smp_t     lfo[ADRIFT_FX_CHUNK];
    fx_stereo_i32_t  dly[ADRIFT_FX_CHUNK];
    int32_t          tank_in[ADRIFT_FX_CHUNK];
} fx_chunk_scratch_t;

static fx_chunk_scratch_t g_fx_scratch;

/* Advance dual quadrature LFOs into interleaved SRAM buffer (1-cycle RAMTEXT leaf) */
static DSP_HOT_RAM int32_t fx_gen_lfos(adrift_fx_state_t *fx,
                                       fx_lfo_smp_t *lfo,
                                       uint32_t lfo1_inc,
                                       uint32_t lfo2_inc,
                                       uint32_t chunk,
                                       bool full_quad) {
    uint32_t lfo_p1   = fx->lfo_phase_1;
    uint32_t lfo_p2   = fx->lfo_phase_2;
    int32_t  last_lfo = 0;

    if (full_quad) {
        for (uint32_t i = 0; i < chunk; ++i) {
            lfo_p1 += lfo1_inc;
            lfo_p2 += lfo2_inc;
            int32_t sin1_l = dsp_sine_lut_q15(lfo_p1);
            lfo[i].s1_l = (int16_t)sin1_l;
            lfo[i].s1_r = (int16_t)dsp_sine_lut_q15(lfo_p1 + 0x80000000u); /* 180 deg anti-phase */
            lfo[i].s2_l = (int16_t)dsp_sine_lut_q15(lfo_p2);
            lfo[i].s2_r = (int16_t)dsp_sine_lut_q15(lfo_p2 + 0x40000000u); /* 90 deg quadrature */
            last_lfo = sin1_l;
        }
    } else {
        for (uint32_t i = 0; i < chunk; ++i) {
            lfo_p1 += lfo1_inc;
            int32_t sin1_l = dsp_sine_lut_q15(lfo_p1);
            lfo[i].s1_l = (int16_t)sin1_l;
            last_lfo = sin1_l;
        }
        lfo_p2 += lfo2_inc * chunk;
    }

    fx->lfo_phase_1 = lfo_p1;
    fx->lfo_phase_2 = lfo_p2;
    return last_lfo;
}

/* Stage 1: Stereo BBD Dimension / Ensemble Chorus (1-cycle RAMTEXT leaf) */
static DSP_HOT_RAM void fx_pass_chorus(adrift_fx_state_t *fx,
                                       const fx_block_coefs_t *c,
                                       int32_t *accum_l,
                                       int32_t *accum_r,
                                       const fx_lfo_smp_t *lfo,
                                       uint32_t chunk) {
    uint32_t chorus_w        = fx->chorus_w;
    int32_t  chorus_lp_l     = fx->chorus_lp_l;
    int32_t  chorus_lp_r     = fx->chorus_lp_r;
    int32_t  chorus_depth_q8 = c->chorus_depth_q8;
    int32_t  chorus_wet_q15  = c->chorus_wet_q15;

    for (uint32_t i = 0; i < chunk; ++i) {
        int32_t sig_l = accum_l[i];
        int32_t sig_r = accum_r[i];

        fx->chorus_l[chorus_w] = dsp_sat16(sig_l);
        fx->chorus_r[chorus_w] = dsp_sat16(sig_r);

        /* Dual-LFO modulation around 360 samples (~8.2 ms) in Q8 */
        int32_t sin1_l = lfo[i].s1_l;
        int32_t sin1_r = lfo[i].s1_r;
        int32_t sin2_l = lfo[i].s2_l;
        int32_t sin2_r = lfo[i].s2_r;
        int32_t mod_l  = ((sin1_l * 3) + sin2_l) >> 2;
        int32_t mod_r  = ((sin1_r * 3) + sin2_r) >> 2;
        int32_t d_l_q8 = (360 << 8) + dsp_mul_q15(mod_l, chorus_depth_q8);
        int32_t d_r_q8 = (380 << 8) + dsp_mul_q15(mod_r, chorus_depth_q8);

        int32_t raw_l = fx_read_interp_q8(fx->chorus_l, ADRIFT_FX_CHORUS_MASK, chorus_w, d_l_q8);
        int32_t raw_r = fx_read_interp_q8(fx->chorus_r, ADRIFT_FX_CHORUS_MASK, chorus_w, d_r_q8);

        /* Warm BBD anti-imaging lowpass (~9 kHz) */
        chorus_lp_l += ((raw_l - chorus_lp_l) * 22000) >> 15;
        chorus_lp_r += ((raw_r - chorus_lp_r) * 22000) >> 15;

        /* Dimension cross-phase stereo widening */
        int32_t wet_l = chorus_lp_l - (chorus_lp_r >> 2);
        int32_t wet_r = chorus_lp_r - (chorus_lp_l >> 2);

        accum_l[i] = sig_l + dsp_mul_q15(wet_l, chorus_wet_q15);
        accum_r[i] = sig_r + dsp_mul_q15(wet_r, chorus_wet_q15);
        chorus_w   = (chorus_w + 1u) & ADRIFT_FX_CHORUS_MASK;
    }

    fx->chorus_w    = chorus_w;
    fx->chorus_lp_l = chorus_lp_l;
    fx->chorus_lp_r = chorus_lp_r;
}

/* Stage 2: Stereo Ping-Pong Tape Delay (1-cycle RAMTEXT leaf) */
static DSP_HOT_RAM int32_t fx_pass_delay(adrift_fx_state_t *fx,
                                         const fx_block_coefs_t *c,
                                         int32_t *accum_l,
                                         int32_t *accum_r,
                                         const fx_lfo_smp_t *lfo,
                                         fx_stereo_i32_t *dly_out,
                                         uint32_t chunk,
                                         bool save_dly_buf) {
    uint32_t delay_w         = fx->delay_w;
    int32_t  delay_len_q8    = fx->delay_len_q8;
    int32_t  delay_lp_l      = fx->delay_lp_l;
    int32_t  delay_lp_r      = fx->delay_lp_r;
    int32_t  dc_x_l          = fx->delay_dc_x_l;
    int32_t  dc_y_l          = fx->delay_dc_y_l;
    int32_t  dc_x_r          = fx->delay_dc_x_r;
    int32_t  dc_y_r          = fx->delay_dc_y_r;
    int32_t  target_delay_q8 = c->target_delay_q8;
    int32_t  delay_fb_q15    = c->delay_fb_q15;
    int32_t  delay_wet_q15   = c->delay_wet_q15;
    int32_t  peak_delay      = 0;

    for (uint32_t i = 0; i < chunk; ++i) {
        int32_t sig_l  = accum_l[i];
        int32_t sig_r  = accum_r[i];
        int32_t sin1_l = lfo[i].s1_l;

        /* Smooth tape delay time slewing + subtle tape wow/flutter */
        delay_len_q8 += (target_delay_q8 - delay_len_q8) >> 8;
        int32_t wow_q8       = (sin1_l * 3) >> 8;
        int32_t tap_len_l_q8 = delay_len_q8 + wow_q8;
        /* Slight stereo offset on R tap (3/4 ratio at small settings or +11 ms spread) */
        int32_t tap_len_r_q8 = delay_len_q8 - wow_q8 + (256 << 8);
        if (tap_len_r_q8 > ((int32_t)(ADRIFT_FX_DELAY_LEN - 8u) << 8)) {
            tap_len_r_q8 = ((int32_t)(ADRIFT_FX_DELAY_LEN - 8u) << 8);
        }

        int32_t tap_l = fx_read_interp_q8(fx->delay_l, ADRIFT_FX_DELAY_MASK, delay_w, tap_len_l_q8);
        int32_t tap_r = fx_read_interp_q8(fx->delay_r, ADRIFT_FX_DELAY_MASK, delay_w, tap_len_r_q8);

        /* Warm analog tape high-cut (~3.6 kHz) */
        delay_lp_l += ((tap_l - delay_lp_l) * 13500) >> 15;
        delay_lp_r += ((tap_r - delay_lp_r) * 13500) >> 15;

        /* Passive 1-pole DC / low-cut blocker (~40 Hz, |H(w)| <= 1.0 everywhere) */
        int32_t hp_l = dsp_mul_q15(dc_y_l + delay_lp_l - dc_x_l, 32440);
        dc_x_l = delay_lp_l;
        dc_y_l = hp_l;

        int32_t hp_r = dsp_mul_q15(dc_y_r + delay_lp_r - dc_x_r, 32440);
        dc_x_r = delay_lp_r;
        dc_y_r = hp_r;

        /* Ping-pong cross-coupled feedback + asymmetric tape soft saturation */
        int32_t in_l_clamped = dsp_clamp_i32(sig_l, -45000, 45000);
        int32_t in_r_clamped = dsp_clamp_i32(sig_r, -45000, 45000);
        int32_t in_ping_l    = ((in_l_clamped * 3) + in_r_clamped) >> 3;
        int32_t in_ping_r    = ((in_r_clamped * 3) + in_l_clamped) >> 3;
        int32_t fb_in_l      = in_ping_l + dsp_mul_q15(hp_r, delay_fb_q15);
        int32_t fb_in_r      = in_ping_r + dsp_mul_q15(hp_l, delay_fb_q15);

        fx->delay_l[delay_w] = dsp_bbd_saturate_q15(fb_in_l);
        fx->delay_r[delay_w] = dsp_bbd_saturate_q15(fb_in_r);
        delay_w = (delay_w + 1u) & ADRIFT_FX_DELAY_MASK;

        int32_t dly_wet_l = dsp_mul_q14(hp_l, delay_wet_q15);
        int32_t dly_wet_r = dsp_mul_q14(hp_r, delay_wet_q15);
        if (save_dly_buf) {
            dly_out[i].l = dly_wet_l;
            dly_out[i].r = dly_wet_r;
        } else {
            accum_l[i] = sig_l + dly_wet_l;
            accum_r[i] = sig_r + dly_wet_r;
        }

        int32_t abs_dly = fx_abs_i32(hp_l) + fx_abs_i32(hp_r);
        if (abs_dly > peak_delay) peak_delay = abs_dly;
    }

    fx->delay_w      = delay_w;
    fx->delay_len_q8 = delay_len_q8;
    fx->delay_lp_l   = delay_lp_l;
    fx->delay_lp_r   = delay_lp_r;
    fx->delay_dc_x_l = dc_x_l;
    fx->delay_dc_y_l = dc_y_l;
    fx->delay_dc_x_r = dc_x_r;
    fx->delay_dc_y_r = dc_y_r;
    return peak_delay;
}

/* Stage 3a: Reverb Input Diffusers + Feed-Forward Octave Shimmer (1-cycle RAMTEXT leaf) */
static DSP_HOT_RAM void fx_pass_reverb_in(adrift_fx_state_t *fx,
                                          const fx_block_coefs_t *c,
                                          const int32_t *accum_l,
                                          const int32_t *accum_r,
                                          const fx_stereo_i32_t *dly_in,
                                          int32_t *tank_in_out,
                                          uint32_t chunk,
                                          bool has_delay) {
    uint32_t ap_in1_w     = fx->ap_in1_w;
    uint32_t ap_in2_w     = fx->ap_in2_w;
    int32_t  rvb_in_lp    = fx->reverb_in_lp;
    uint32_t shimmer_w    = fx->shimmer_w;
    int32_t  shimmer_lp   = fx->shimmer_lp;
    int32_t  shim_dc_x    = fx->shimmer_dc_x;
    int32_t  shim_dc_y    = fx->shimmer_dc_y;
    int32_t  rvb_shim_q15 = c->rvb_shim_q15;

    for (uint32_t i = 0; i < chunk; ++i) {
        /* Scale polyphonic sum into Q15 tank headroom (-32767..32767) so internal
         * allpass diffusers and comb delay lines never clip or overflow 32-bit math */
        int32_t rvb_raw = (accum_l[i] + accum_r[i]) >> 2;
        if (has_delay) {
            rvb_raw += (dly_in[i].l + dly_in[i].r) >> 3;
        }
        int32_t rvb_in = dsp_sat_q15(rvb_raw);

        /* Pre-bandwidth LPF (~6.5 kHz) */
        rvb_in_lp += ((rvb_in - rvb_in_lp) * 18000) >> 15;

        /* Series Input Allpass Diffuser 1 (len 142, g = 0.5) */
        uint32_t ap1_r = (ap_in1_w - 142u) & ADRIFT_FX_AP1_MASK;
        int32_t  z1    = fx->ap_in1[ap1_r];
        int32_t  v1    = dsp_sat_q15(rvb_in_lp + (z1 >> 1));
        fx->ap_in1[ap_in1_w] = (int16_t)v1;
        int32_t  diff1 = z1 - (v1 >> 1);
        ap_in1_w = (ap_in1_w + 1u) & ADRIFT_FX_AP1_MASK;

        /* Series Input Allpass Diffuser 2 (len 379, g = 0.5) */
        uint32_t ap2_r = (ap_in2_w - 379u) & ADRIFT_FX_AP2_MASK;
        int32_t  z2    = fx->ap_in2[ap2_r];
        int32_t  v2    = dsp_sat_q15(diff1 + (z2 >> 1));
        fx->ap_in2[ap_in2_w] = (int16_t)v2;
        int32_t  diff2 = dsp_sat_q15(z2 - (v2 >> 1));
        ap_in2_w = (ap_in2_w + 1u) & ADRIFT_FX_AP2_MASK;

        /* Feed-forward Dual-Grain +12 ST Octave-Up Shimmer driven from diffused input
         * (diff2). Because the pitch shifter is strictly feed-forward into the FDN tank
         * rather than inside the tank's feedback loop, it cannot cause runaway octave
         * escalation or Larsen howling at any combination of RVB SZ and SHIMMR. */
        fx->shimmer_buf[shimmer_w] = (int16_t)diff2;
        int32_t shim_sig = 0;
        if (rvb_shim_q15 > 0) {
            uint32_t p_a   = shimmer_w & 511u;
            uint32_t p_b   = (shimmer_w + 256u) & 511u;
            uint32_t idx_a = (shimmer_w - 512u + (p_a << 1)) & ADRIFT_FX_SHIMMER_MASK;
            uint32_t idx_b = (shimmer_w - 512u + (p_b << 1)) & ADRIFT_FX_SHIMMER_MASK;
            int32_t  win_a = (p_a < 256u) ? (int32_t)(p_a << 7) : (int32_t)((511u - p_a) << 7);
            int32_t  win_b = 32767 - win_a;
            int32_t  grain = ((fx->shimmer_buf[idx_a] * win_a) +
                              (fx->shimmer_buf[idx_b] * win_b)) >> 15;
            /* Smooth grain splices (~3.8 kHz LPF) & passive DC/low-cut blocker (~160 Hz) */
            shimmer_lp += ((grain - shimmer_lp) * 11500) >> 15;
            int32_t shim_hp = dsp_mul_q15(shim_dc_y + shimmer_lp - shim_dc_x, 32000);
            shim_dc_x = shimmer_lp;
            shim_dc_y = shim_hp;
            shim_sig  = dsp_mul_q15(shim_hp, rvb_shim_q15);
        }
        shimmer_w = (shimmer_w + 1u) & ADRIFT_FX_SHIMMER_MASK;

        tank_in_out[i] = (diff2 + shim_sig) >> 1;
    }

    fx->ap_in1_w     = ap_in1_w;
    fx->ap_in2_w     = ap_in2_w;
    fx->reverb_in_lp = rvb_in_lp;
    fx->shimmer_w    = shimmer_w;
    fx->shimmer_lp   = shimmer_lp;
    fx->shimmer_dc_x = shim_dc_x;
    fx->shimmer_dc_y = shim_dc_y;
}

/* Stage 3b: 4x4 Hadamard Modulated Comb FDN + Modulated Output Diffusers (1-cycle RAMTEXT leaf) */
static DSP_HOT_RAM int32_t fx_pass_reverb_fdn(adrift_fx_state_t *fx,
                                              const fx_block_coefs_t *c,
                                              int32_t *accum_l,
                                              int32_t *accum_r,
                                              const fx_lfo_smp_t *lfo,
                                              const fx_stereo_i32_t *dly_in,
                                              const int32_t *tank_in_buf,
                                              uint32_t chunk,
                                              bool has_delay) {
    uint32_t comb_w       = fx->comb_w;
    int32_t  damp_l1      = fx->comb_damp_l1;
    int32_t  damp_r1      = fx->comb_damp_r1;
    int32_t  damp_l2      = fx->comb_damp_l2;
    int32_t  damp_r2      = fx->comb_damp_r2;
    uint32_t ap_out_w     = fx->ap_out_w;
    int32_t  rvb_fb_q15   = c->rvb_fb_q15;
    int32_t  rvb_damp_q15 = c->rvb_damp_q15;
    int32_t  rvb_wet_q15  = c->rvb_wet_q15;
    int32_t  peak_reverb  = 0;

    for (uint32_t i = 0; i < chunk; ++i) {
        int32_t sig_l  = accum_l[i];
        int32_t sig_r  = accum_r[i];
        int32_t sin1_l = lfo[i].s1_l;
        int32_t sin1_r = lfo[i].s1_r;
        int32_t sin2_l = lfo[i].s2_l;
        int32_t sin2_r = lfo[i].s2_r;

        /* 4 Mutually-Prime Quadrature-Modulated Comb Filters (FDN)
         * Slow +/- 3.5 sample LFO modulation smears standing-wave eigenmodes */
        int32_t c_l1 = fx_read_interp_q8(fx->comb_l1, ADRIFT_FX_COMB_MASK, comb_w,
                                         (1499 << 8) + (sin1_l >> 5));
        int32_t c_r1 = fx_read_interp_q8(fx->comb_r1, ADRIFT_FX_COMB_MASK, comb_w,
                                         (1613 << 8) + (sin1_r >> 5));
        int32_t c_l2 = fx_read_interp_q8(fx->comb_l2, ADRIFT_FX_COMB_MASK, comb_w,
                                         (1777 << 8) + (sin2_l >> 5));
        int32_t c_r2 = fx_read_interp_q8(fx->comb_r2, ADRIFT_FX_COMB_MASK, comb_w,
                                         (1931 << 8) + (sin2_r >> 5));

        /* Per-comb 1-pole HF damping filter + tiny DC leak (-1/512) to prevent DC buildup */
        damp_l1 += dsp_mul_q15(c_l1 - damp_l1, rvb_damp_q15);
        damp_r1 += dsp_mul_q15(c_r1 - damp_r1, rvb_damp_q15);
        damp_l2 += dsp_mul_q15(c_l2 - damp_l2, rvb_damp_q15);
        damp_r2 += dsp_mul_q15(c_r2 - damp_r2, rvb_damp_q15);
        damp_l1 -= (damp_l1 >> 9);
        damp_r1 -= (damp_r1 >> 9);
        damp_l2 -= (damp_l2 >> 9);
        damp_r2 -= (damp_r2 >> 9);

        /* Unitary 4x4 Hadamard-Rotation matrix with -1/2 trace (no +1 common-mode eigenvalue) */
        int32_t s01  = damp_l1 + damp_r1;
        int32_t d01  = damp_l1 - damp_r1;
        int32_t s23  = damp_l2 + damp_r2;
        int32_t d23  = damp_l2 - damp_r2;
        int32_t m_l1 = -((s01 + s23) >> 1);
        int32_t m_r1 =  ((d01 + d23) >> 1);
        int32_t m_l2 =  ((s01 - s23) >> 1);
        int32_t m_r2 =  ((d23 - d01) >> 1);

        int32_t stereo_diff = dsp_clamp_i32((sig_l - sig_r) >> 4, -8192, 8192);
        int32_t tank_in     = tank_in_buf[i];
        int32_t fb_l1 = tank_in + stereo_diff + dsp_mul_q15(m_l1, rvb_fb_q15);
        int32_t fb_r1 = tank_in - stereo_diff + dsp_mul_q15(m_r1, rvb_fb_q15);
        int32_t fb_l2 = -tank_in - stereo_diff + dsp_mul_q15(m_l2, rvb_fb_q15);
        int32_t fb_r2 = -tank_in + stereo_diff + dsp_mul_q15(m_r2, rvb_fb_q15);

        fx->comb_l1[comb_w] = dsp_sat16(fb_l1);
        fx->comb_r1[comb_w] = dsp_sat16(fb_r1);
        fx->comb_l2[comb_w] = dsp_sat16(fb_l2);
        fx->comb_r2[comb_w] = dsp_sat16(fb_r2);
        comb_w = (comb_w + 1u) & ADRIFT_FX_COMB_MASK;

        int32_t tank_l = dsp_sat_q15(c_l1 - c_l2);
        int32_t tank_r = dsp_sat_q15(c_r1 - c_r2);

        /* Modulated Output Allpass Diffusers (L: 317 +/- 8, R: 353 +/- 8, g = 0.5) */
        uint32_t mod_l_ofs = (uint32_t)(317 + (sin2_l >> 12));
        uint32_t mod_r_ofs = (uint32_t)(353 + (sin2_r >> 12));
        int32_t  zo_l = fx->ap_out_l[(ap_out_w - mod_l_ofs) & ADRIFT_FX_AP2_MASK];
        int32_t  vo_l = dsp_sat_q15(tank_l + (zo_l >> 1));
        fx->ap_out_l[ap_out_w] = (int16_t)vo_l;
        int32_t  out_l = dsp_sat_q15(zo_l - (vo_l >> 1));

        int32_t  zo_r = fx->ap_out_r[(ap_out_w - mod_r_ofs) & ADRIFT_FX_AP2_MASK];
        int32_t  vo_r = dsp_sat_q15(tank_r + (zo_r >> 1));
        fx->ap_out_r[ap_out_w] = (int16_t)vo_r;
        int32_t  out_r = dsp_sat_q15(zo_r - (vo_r >> 1));
        ap_out_w = (ap_out_w + 1u) & ADRIFT_FX_AP2_MASK;

        int32_t rvb_wet_l = dsp_mul_q14(out_l, rvb_wet_q15);
        int32_t rvb_wet_r = dsp_mul_q14(out_r, rvb_wet_q15);
        if (has_delay) {
            sig_l += dly_in[i].l;
            sig_r += dly_in[i].r;
        }
        accum_l[i] = sig_l + rvb_wet_l;
        accum_r[i] = sig_r + rvb_wet_r;

        int32_t abs_rvb = fx_abs_i32(out_l) + fx_abs_i32(out_r);
        if (abs_rvb > peak_reverb) peak_reverb = abs_rvb;
    }

    fx->comb_w       = comb_w;
    fx->comb_damp_l1 = damp_l1;
    fx->comb_damp_r1 = damp_r1;
    fx->comb_damp_l2 = damp_l2;
    fx->comb_damp_r2 = damp_r2;
    fx->ap_out_w     = ap_out_w;
    return peak_reverb;
}

/* Continuous pure-register log2(x) in Q10 (1024 units/octave, ~170 units/dB; zero LUTs/calls) */
DSP_ALWAYS_INLINE int32_t fx_log2_q10(uint32_t x) {
    if (x <= 1u) return 0;
    uint32_t v = x;
    uint32_t e = 0u;
    if (v >= (1u << 16)) { v >>= 16; e += 16u; }
    if (v >= (1u << 8))  { v >>= 8;  e += 8u;  }
    if (v >= (1u << 4))  { v >>= 4;  e += 4u;  }
    if (v >= (1u << 2))  { v >>= 2;  e += 2u;  }
    if (v >= (1u << 1))  {           e += 1u;  }
    uint32_t norm  = (e >= 10u) ? (x >> (e - 10u)) : (x << (10u - e));
    int32_t  m     = (int32_t)(norm - 1024u);
    int32_t  c_q10 = 1420 - ((m * 396) >> 10);
    int32_t  frac  = (m * c_q10) >> 10;
    return (int32_t)(e << 10) + frac;
}

/* Continuous pure-register 2^(y_q10 / 1024) in Q14 (16384 = 1.0x, up to 65535 = +12.0 dB) */
DSP_ALWAYS_INLINE int32_t fx_exp2_q14(int32_t y_q10) {
    if (y_q10 <= -14336) return 1;
    if (y_q10 >= 2048)   return 65535;
    int32_t e    = y_q10 >> 10;
    int32_t f    = y_q10 & 0x3FF;
    int32_t poly = 11395 + ((f * 4989) >> 10);
    int32_t m14  = 16384 + ((f * poly) >> 10);
    return (e >= 0) ? (m14 << e) : (m14 >> (-e));
}

/* Exact 32-bit split multiply (sig * gain_q14) >> 14 safe for |sig| <= 262144 and gain_q14 <= 65535 */
DSP_ALWAYS_INLINE int32_t fx_mul_sig_gain_q14(int32_t sig, int32_t gain_q14) {
    return ((sig >> 14) * gain_q14) + (((sig & 0x3FFF) * gain_q14) >> 14);
}

/* Exact 32-bit split multiply (diff * wet_q15) >> 15 safe for |diff| <= 524288 and wet_q15 <= 32768 */
DSP_ALWAYS_INLINE int32_t fx_mul_diff_wet_q15(int32_t diff, int32_t wet_q15) {
    return ((diff >> 15) * wet_q15) + (((diff & 0x7FFF) * wet_q15) >> 15);
}

/* Stage 4: Stereo Bus ("Glue" / Parallel) Soft-Knee 4:1 Compressor (1-cycle RAMTEXT leaf) */
static DSP_HOT_RAM int32_t fx_pass_comp(adrift_fx_state_t *fx,
                                        const fx_block_coefs_t *c,
                                        int32_t *accum_l,
                                        int32_t *accum_r,
                                        uint32_t chunk) {
    int32_t sc_lp_l    = fx->comp_sc_lp_l_q8;
    int32_t sc_lp_r    = fx->comp_sc_lp_r_q8;
    int32_t env_q8     = fx->comp_env_q8;
    int32_t gain_q14   = fx->comp_gain_q14;
    if (gain_q14 <= 0) gain_q14 = 16384;

    int32_t thresh_l2  = c->comp_thresh_l2;
    int32_t makeup_l2  = c->comp_makeup_l2;
    int32_t atk_q15    = c->comp_atk_q15;
    int32_t wet_q15    = c->comp_wet_q15;
    int32_t peak_gr_l2 = 0;

    uint32_t idx = 0u;
    while (idx < chunk) {
        uint32_t sub = chunk - idx;
        if (sub > 8u) sub = 8u;

        int32_t *al = accum_l + idx;
        int32_t *ar = accum_r + idx;

        /* 1. Sidechain ~110 Hz 1-pole HPF + stereo-linked peak/power detector + ballistics */
        for (uint32_t k = 0u; k < sub; ++k) {
            int32_t sl = dsp_clamp_i32(al[k], -262144, 262144);
            int32_t sr = dsp_clamp_i32(ar[k], -262144, 262144);

            sc_lp_l += ((sl * 256) - sc_lp_l) >> 6;
            sc_lp_r += ((sr * 256) - sc_lp_r) >> 6;
            int32_t hp_l = sl - ((sc_lp_l + 128) >> 8);
            int32_t hp_r = sr - ((sc_lp_r + 128) >> 8);

            int32_t abs_l  = fx_abs_i32(hp_l);
            int32_t abs_r  = fx_abs_i32(hp_r);
            int32_t pk_max = (abs_l > abs_r) ? abs_l : abs_r;
            int32_t pk_min = (abs_l > abs_r) ? abs_r : abs_l;
            int32_t det    = dsp_clamp_i32(((pk_max * 3) + pk_min) >> 2, 0, 262144);
            int32_t det_q8 = det << 8;

            int32_t diff = det_q8 - env_q8;
            /* Program-dependent release: ~93 ms fast crest recovery, ~185 ms sustained glue */
            int32_t coef = (diff >= 0) ? atk_q15 : (((env_q8 >> 2) > det_q8) ? 8 : 4);
            env_q8 += ((diff >> 15) * coef) + (((diff & 0x7FFF) * coef) >> 15);
            if (env_q8 < 0) env_q8 = 0;
        }

        /* 2. Soft-knee (6 dB width) 4:1 dB gain computer evaluated every 8 samples (~5.5 kHz) */
        int32_t env_l2 = fx_log2_q10((uint32_t)(env_q8 + 1));
        int32_t over   = env_l2 - thresh_l2;
        int32_t gr_l2  = 0;
        if (over > -512) {
            if (over >= 512) {
                gr_l2 = (over * 3) >> 2;
            } else {
                int32_t u = over + 512;
                gr_l2 = (3 * u * u) >> 13;
            }
        }
        if (gr_l2 > peak_gr_l2) peak_gr_l2 = gr_l2;

        int32_t target_gain_q14 = fx_exp2_q14(makeup_l2 - gr_l2);

        /* 3. Smooth per-sample VCA ramp & parallel dry/wet bus blend */
        if (sub == 8u) {
            int32_t step_q14 = (target_gain_q14 - gain_q14) >> 3;
            for (uint32_t k = 0u; k < 8u; ++k) {
                gain_q14 += step_q14;
                int32_t sl     = dsp_clamp_i32(al[k], -262144, 262144);
                int32_t sr     = dsp_clamp_i32(ar[k], -262144, 262144);
                int32_t comp_l = fx_mul_sig_gain_q14(sl, gain_q14);
                int32_t comp_r = fx_mul_sig_gain_q14(sr, gain_q14);
                int32_t dl     = dsp_clamp_i32(comp_l - sl, -524288, 524288);
                int32_t dr     = dsp_clamp_i32(comp_r - sr, -524288, 524288);
                al[k] = sl + fx_mul_diff_wet_q15(dl, wet_q15);
                ar[k] = sr + fx_mul_diff_wet_q15(dr, wet_q15);
            }
        } else {
            for (uint32_t k = 0u; k < sub; ++k) {
                gain_q14 += (target_gain_q14 - gain_q14) >> 2;
                int32_t sl     = dsp_clamp_i32(al[k], -262144, 262144);
                int32_t sr     = dsp_clamp_i32(ar[k], -262144, 262144);
                int32_t comp_l = fx_mul_sig_gain_q14(sl, gain_q14);
                int32_t comp_r = fx_mul_sig_gain_q14(sr, gain_q14);
                int32_t dl     = dsp_clamp_i32(comp_l - sl, -524288, 524288);
                int32_t dr     = dsp_clamp_i32(comp_r - sr, -524288, 524288);
                al[k] = sl + fx_mul_diff_wet_q15(dl, wet_q15);
                ar[k] = sr + fx_mul_diff_wet_q15(dr, wet_q15);
            }
        }
        gain_q14 = target_gain_q14;
        idx += sub;
    }

    fx->comp_sc_lp_l_q8 = sc_lp_l;
    fx->comp_sc_lp_r_q8 = sc_lp_r;
    fx->comp_env_q8     = env_q8;
    fx->comp_gain_q14   = gain_q14;
    return peak_gr_l2;
}

void adrift_fx_process(adrift_fx_state_t *fx,
                       const adrift_fx_params_t *params,
                       int32_t *accum_l,
                       int32_t *accum_r,
                       uint32_t frames) {
    int32_t chorus_mix_param = dsp_clamp_i32(params->chorus_mix, 0, 1023);
    int32_t delay_time_param = dsp_clamp_i32(params->delay_time, 0, 1023);
    int32_t delay_fb_param   = dsp_clamp_i32(params->delay_fb, 0, 1023);
    int32_t delay_mix_param  = dsp_clamp_i32(params->delay_mix, 0, 1023);
    int32_t rvb_size_param   = dsp_clamp_i32(params->reverb_size, 0, 1023);
    int32_t rvb_damp_param   = dsp_clamp_i32(params->reverb_damp, 0, 1023);
    int32_t rvb_shim_param   = dsp_clamp_i32(params->reverb_shimmer, 0, 1023);
    int32_t rvb_mix_param    = dsp_clamp_i32(params->reverb_mix, 0, 1023);
    int32_t comp_mix_param   = dsp_clamp_i32(params->comp_mix, 0, 1023);
    int32_t comp_thr_param   = dsp_clamp_i32(params->comp_thresh, 0, 1023);
    int32_t comp_atk_param   = dsp_clamp_i32(params->comp_attack, 0, 1023);
    int32_t comp_mkp_param   = dsp_clamp_i32(params->comp_makeup, 0, 1023);

    bool any_active = (chorus_mix_param > 0 || delay_mix_param > 0 ||
                       rvb_mix_param > 0 || comp_mix_param > 0);
    if (!any_active) {
        if (fx->buffers_dirty) {
            int32_t saved_len = fx->delay_len_q8;
            adrift_fx_reset(fx);
            fx->delay_len_q8 = saved_len;
        }
        return;
    }
    fx->buffers_dirty = true;

    fx_chunk_scratch_t *sc = &g_fx_scratch;
    fx_block_coefs_t   *c  = &sc->coefs;

    /* Stage 1: Stereo BBD Dimension Chorus parameters */
    c->chorus_wet_q15  = (chorus_mix_param << 5); /* 0..32736 */
    c->chorus_depth_q8 = 6400 + ((chorus_mix_param * 48000) >> 10); /* ~25 .. 212 samples in Q8 */
    uint32_t lfo1_inc  = 58000u + ((uint32_t)chorus_mix_param * 72u); /* ~0.60 Hz .. 1.35 Hz */
    uint32_t lfo2_inc  = 195000u + ((uint32_t)chorus_mix_param * 95u); /* ~2.0 Hz .. 3.0 Hz */

    /* Stage 2: Stereo Ping-Pong Tape Delay parameters */
    /* 20 ms (882 samples) .. 360 ms (15882 samples) in Q8, or tempo-synced to 24 PPQN MIDI clock */
    if (fx->clk_period_q4 > 0u) {
        uint32_t pulses  = adrift_fx_delay_subdiv_pulses(delay_time_param);
        int32_t  sync_q8 = (int32_t)((pulses * fx->clk_period_q4) << 4);
        c->target_delay_q8 = dsp_clamp_i32(sync_q8, 882 << 8, 15882 << 8);
    } else {
        c->target_delay_q8 = (882 + ((delay_time_param * 15000) >> 10)) << 8;
    }
    c->delay_fb_q15  = (delay_fb_param * 30146) >> 10; /* up to ~92% feedback */
    c->delay_wet_q15 = (delay_mix_param << 5);

    /* Stage 3: Stereo Diffusion Reverb + Shimmer Tank parameters */
    /* Decay feedback: 0.40 (13107) .. 0.875 (28672) */
    c->rvb_fb_q15   = 13107 + ((rvb_size_param * 15565) >> 10);
    /* HF damping LPF coefficient: 0% damp -> 26000 (bright), 100% damp -> 4500 (warm/dark) */
    c->rvb_damp_q15 = 26000 - ((rvb_damp_param * 21500) >> 10);
    /* Shimmer octave-up feed-forward injection gain: 0 .. 28672 (0 .. 87.5%) */
    c->rvb_shim_q15 = (rvb_shim_param * 28672) >> 10;
    c->rvb_wet_q15  = (rvb_mix_param << 5);

    /* Stage 4: Stereo Bus ("Glue" / Parallel) Soft-Knee Compressor parameters
     * 0 dBFS in env_q8 (32768 << 8 = 2^23) is log2_q10 = 23552 (~170 units/dB).
     * Threshold maps 0..1023 -> -24 dB (19472) .. -2 dB (23212). */
    c->comp_thresh_l2 = 19472 + ((comp_thr_param * 3740) >> 10);
    c->comp_makeup_l2 = comp_mkp_param << 1; /* 0..2046 (+0.0 .. +12.0 dB) */
    uint32_t atk_q8   = 256u + (((uint32_t)comp_atk_param * (uint32_t)comp_atk_param * 49u) >> 12);
    c->comp_atk_q15   = dsp_clamp_i32((int32_t)(190131u / atk_q8), 14, 742);
    c->comp_wet_q15   = (int32_t)dsp_udiv_1023((uint32_t)comp_mix_param * 32768u);

    bool has_chorus = (c->chorus_wet_q15 > 0);
    bool has_delay  = (c->delay_wet_q15 > 0);
    bool has_reverb = (c->rvb_wet_q15 > 0);
    bool has_comp   = (c->comp_wet_q15 > 0);
    bool need_lfo   = (has_chorus || has_delay || has_reverb);
    bool full_quad  = (has_chorus || has_reverb);

    int32_t  peak_delay  = 0;
    int32_t  peak_reverb = 0;
    int32_t  peak_gr_l2  = 0;
    int32_t  last_lfo    = 0;
    uint32_t offset      = 0u;

    while (offset < frames) {
        uint32_t chunk = frames - offset;
        if (chunk > ADRIFT_FX_CHUNK) chunk = ADRIFT_FX_CHUNK;

        int32_t *al = accum_l + offset;
        int32_t *ar = accum_r + offset;

        if (need_lfo) {
            last_lfo = DSP_CALL_RAM(fx_gen_lfos)(fx, sc->lfo, lfo1_inc, lfo2_inc, chunk, full_quad);
        } else {
            fx->lfo_phase_1 += lfo1_inc * chunk;
            fx->lfo_phase_2 += lfo2_inc * chunk;
        }

        if (has_chorus) {
            DSP_CALL_RAM(fx_pass_chorus)(fx, c, al, ar, sc->lfo, chunk);
        }
        if (has_delay) {
            int32_t pk_d = DSP_CALL_RAM(fx_pass_delay)(fx, c, al, ar, sc->lfo, sc->dly, chunk, has_reverb);
            if (pk_d > peak_delay) peak_delay = pk_d;
        }
        if (has_reverb) {
            DSP_CALL_RAM(fx_pass_reverb_in)(fx, c, al, ar, sc->dly, sc->tank_in, chunk, has_delay);
            int32_t pk_r = DSP_CALL_RAM(fx_pass_reverb_fdn)(fx, c, al, ar, sc->lfo, sc->dly, sc->tank_in, chunk, has_delay);
            if (pk_r > peak_reverb) peak_reverb = pk_r;
        }
        if (has_comp) {
            int32_t pk_gr = DSP_CALL_RAM(fx_pass_comp)(fx, c, al, ar, chunk);
            if (pk_gr > peak_gr_l2) peak_gr_l2 = pk_gr;
        }

        offset += chunk;
    }

    fx->meter_chorus_lfo_q15 = last_lfo;
    fx->meter_delay_env_q15  = dsp_sat_q15((fx->meter_delay_env_q15 * 3 + peak_delay) >> 2);
    fx->meter_reverb_env_q15 = dsp_sat_q15((fx->meter_reverb_env_q15 * 3 + peak_reverb) >> 2);
    if (has_comp) {
        /* Scale 0..18 dB (0..3072 in log2_q10) to 0..32767 Q15 meter with fast attack / smooth decay */
        int32_t gr_q15 = dsp_sat_q15((peak_gr_l2 * 341) >> 5);
        if (gr_q15 >= fx->meter_comp_gr_q15) {
            fx->meter_comp_gr_q15 = gr_q15;
        } else {
            fx->meter_comp_gr_q15 = (fx->meter_comp_gr_q15 * 7 + gr_q15) >> 3;
        }
    } else {
        fx->comp_env_q8       = 0;
        fx->comp_gain_q14     = 16384;
        fx->meter_comp_gr_q15 = 0;
    }
}
