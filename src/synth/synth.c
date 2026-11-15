#include "synth_internal.h"

void adrift_synth_init(adrift_synth_t *s) {
    uint8_t *raw = (uint8_t *)s;
    for (uint32_t i = 0; i < (uint32_t)sizeof(*s); i++) {
        raw[i] = 0u;
    }
    s->hw_master_pot   = -1;
    s->master_gain_q15 = 32767;
    s->dither_rng      = 0xD17E5A3Cu;
    s->last_note       = 60u;
    s->edit_group      = 0u;
    s->midi_clk_bpm    = 120u;
    adrift_fx_init(&s->fx);

    for (uint8_t v = 0; v < ADRIFT_MAX_VOICES; v++) {
        adrift_voice_t *vc = &s->voices[v];
        vc->owner_group    = -1;
        vc->group_slot     = 0u;
        vc->note           = 60u;
        vc->velocity       = 100u;
        vc->lfo_phase      = (uint32_t)v * (0xFFFFFFFFu / ADRIFT_MAX_VOICES); /* Even spread across V1..V16 (v=0 at 0) */
        vc->lfo2_phase     = (uint32_t)v * 0x517CC1B7u;
        vc->lfo3_phase     = (uint32_t)v * 0x9E3779B9u;
        vc->lfo_rng        = 0xC0FFEE11u ^ ((uint32_t)v * 0x9E3779B9u);
        vc->lfo_sh_q15     = dsp_white_noise_q15(&vc->lfo_rng);
        vc->lfo2_sh_q15    = dsp_white_noise_q15(&vc->lfo_rng);
        vc->lfo3_sh_q15    = dsp_white_noise_q15(&vc->lfo_rng);
        vc->dry_gain_q15   = 19777;
        vc->wet_gain_q15   = 19777;
        vc->grp_gain_l_q15 = 27500;
        vc->grp_gain_r_q15 = 27500;
        adrift_exciter_init(&vc->exc, v);
        adrift_env_init(&vc->env1);
        adrift_svf_init(&vc->in_lpf);
        adrift_bbd_init(&vc->bbd, v);
        adrift_env_init(&vc->env2);
    }

    adrift_synth_load_preset(s, 0u);
}

static inline int32_t master_pot_to_gain_q15(int16_t pot) {
    if (pot < 0) {
        return 32767;
    }
    return (int32_t)dsp_udiv_1023((uint32_t)dsp_clamp_i32(pot, 0, 1023) * 32767u);
}

void adrift_synth_set_master_pot(adrift_synth_t *s, int16_t pot_0_1023, bool snap_gain) {
    if (!s) {
        return;
    }
    if (pot_0_1023 < 0) {
        s->hw_master_pot = -1;
        if (snap_gain) {
            s->master_gain_q15 = 32767;
        }
        return;
    }
    bool    was_unset = (s->hw_master_pot < 0);
    int16_t clamped   = (int16_t)dsp_clamp_i32(pot_0_1023, 0, 1023);
    s->hw_master_pot  = clamped;
    if (snap_gain || was_unset) {
        s->master_gain_q15 = master_pot_to_gain_q15(clamped);
    }
}

static inline int32_t step_lfo_osc(uint32_t *phase_io,
                                   int32_t *sh_q15_io,
                                   uint32_t *rng_io,
                                   uint8_t wave,
                                   int16_t rate,
                                   uint32_t frames,
                                   bool need_out) {
    /* LFO rate 0..1023 -> ~0.05 Hz .. 30 Hz */
    int32_t  r       = dsp_clamp_i32(rate, 0, 1023);
    uint32_t r2      = (uint32_t)(r * r) >> 10; /* 0..1022 */
    uint32_t per_smp = 4870u + r2 * 2852u;
    uint32_t prev_ph = *phase_io;
    uint32_t next_ph = prev_ph + per_smp * frames;
    *phase_io        = next_ph;

    if (!need_out) {
        if (wave >= ADRIFT_LFO_SH && next_ph < prev_ph) {
            *sh_q15_io = dsp_white_noise_q15(rng_io);
        }
        return 0;
    }

    int32_t out_q15 = 0;
    switch (wave) {
        case ADRIFT_LFO_SINE:
            out_q15 = dsp_sine_lut_q15(next_ph);
            break;
        case ADRIFT_LFO_TRI: {
            uint32_t p16 = next_ph >> 16;
            out_q15 = (p16 < 32768u) ? ((int32_t)(p16 << 1) - 32768)
                                     : (32767 - (int32_t)((p16 - 32768u) << 1));
            break;
        }
        case ADRIFT_LFO_SAW:
            out_q15 = (int32_t)(next_ph >> 16) - 32768;
            break;
        case ADRIFT_LFO_SQR:
            out_q15 = (next_ph < 0x80000000u) ? 32767 : -32767;
            break;
        case ADRIFT_LFO_SH:
        default:
            if (next_ph < prev_ph) {
                *sh_q15_io = dsp_white_noise_q15(rng_io);
            }
            out_q15 = *sh_q15_io;
            break;
    }
    return out_q15;
}

typedef struct {
    int32_t vco_pitch_q8;
    int32_t vco_morph;
    int32_t vco_pwm;
    int32_t in_lpf;
    int32_t bbd_pitch_q8;
    int32_t fb_lpf;
    int32_t fb_amt;
    int32_t pan;
    int32_t spread;
    int32_t dry_wet;
    int32_t vca;
    int32_t ex_mix;
    int32_t ex_lvl;
    int32_t sh_clk;
    int32_t in_res;
    int32_t dispr;
    int32_t env1_atk;
    int32_t env1_dec;
    int32_t env1_sus;
    int32_t env1_rel;
    int32_t env1_dly;
    int32_t lfo1_rate;
} adrift_mod_accum_t;

static inline void clear_mod_accum(adrift_mod_accum_t *m) {
    m->vco_pitch_q8 = 0;
    m->vco_morph    = 0;
    m->vco_pwm      = 0;
    m->in_lpf       = 0;
    m->bbd_pitch_q8 = 0;
    m->fb_lpf       = 0;
    m->fb_amt       = 0;
    m->pan          = 0;
    m->spread       = 0;
    m->dry_wet      = 0;
    m->vca          = 0;
    m->ex_mix       = 0;
    m->ex_lvl       = 0;
    m->sh_clk       = 0;
    m->in_res       = 0;
    m->dispr        = 0;
    m->env1_atk     = 0;
    m->env1_dec     = 0;
    m->env1_sus     = 0;
    m->env1_rel     = 0;
    m->env1_dly     = 0;
    m->lfo1_rate    = 0;
}

static inline void accum_lfo_mod(uint8_t dest, int32_t mod_sig, adrift_mod_accum_t *m) {
    switch (dest) {
        case ADRIFT_MOD_VCO_PITCH: m->vco_pitch_q8 += (mod_sig * (12 << 8)) >> 15; break;
        case ADRIFT_MOD_VCO_MORPH: m->vco_morph    += (mod_sig * 512) >> 15; break;
        case ADRIFT_MOD_VCO_PWM:   m->vco_pwm      += (mod_sig * 480) >> 15; break;
        case ADRIFT_MOD_IN_LPF:    m->in_lpf       += (mod_sig * 512) >> 15; break;
        case ADRIFT_MOD_BBD_PITCH: m->bbd_pitch_q8 += (mod_sig * (4 << 8)) >> 15; break;
        case ADRIFT_MOD_FB_LPF:    m->fb_lpf       += (mod_sig * 512) >> 15; break;
        case ADRIFT_MOD_FB_AMT:    m->fb_amt       += (mod_sig * 256) >> 15; break;
        case ADRIFT_MOD_ALL_PITCH: {
            int32_t dp = (mod_sig * (12 << 8)) >> 15;
            m->vco_pitch_q8 += dp;
            m->bbd_pitch_q8 += dp;
            break;
        }
        case ADRIFT_MOD_PAN:       m->pan          += (mod_sig * 512) >> 15; break;
        case ADRIFT_MOD_SPREAD:    m->spread       += (mod_sig * 512) >> 15; break;
        case ADRIFT_MOD_DRY_WET:   m->dry_wet      += (mod_sig * 512) >> 15; break;
        case ADRIFT_MOD_VCA:       m->vca          += (mod_sig * 1023) >> 15; break;
        case ADRIFT_MOD_EX_MIX:    m->ex_mix       += (mod_sig * 512) >> 15; break;
        case ADRIFT_MOD_EX_LVL:    m->ex_lvl       += (mod_sig * 512) >> 15; break;
        case ADRIFT_MOD_SH_CLK:    m->sh_clk       += (mod_sig * 512) >> 15; break;
        case ADRIFT_MOD_IN_RES:    m->in_res       += (mod_sig * 512) >> 15; break;
        case ADRIFT_MOD_DISPR:     m->dispr        += (mod_sig * 512) >> 15; break;
        case ADRIFT_MOD_ENV1_ATK:  m->env1_atk     += (mod_sig * 512) >> 15; break;
        case ADRIFT_MOD_ENV1_DEC:  m->env1_dec     += (mod_sig * 512) >> 15; break;
        case ADRIFT_MOD_ENV1_SUS:  m->env1_sus     += (mod_sig * 512) >> 15; break;
        case ADRIFT_MOD_ENV1_REL:  m->env1_rel     += (mod_sig * 512) >> 15; break;
        case ADRIFT_MOD_ENV1_DLY:  m->env1_dly     += (mod_sig * 512) >> 15; break;
        case ADRIFT_MOD_LFO1_RATE: m->lfo1_rate    += (mod_sig * 512) >> 15; break;
        default: break;
    }
}

static int32_t step_voice_lfos_and_accum(adrift_voice_t *vc,
                                         const adrift_patch_t *vp,
                                         int32_t lfo1_tot_dep,
                                         uint32_t frames,
                                         adrift_mod_accum_t *m,
                                         bool force_lfo1_out) {
    /* Step LFO 2 & LFO 3 first so cross-modulation targeting L1 RT modulates LFO 1 on the same block */
    bool has_lfo2 = (vp->lfo2_depth != 0);
    int32_t lfo2_q15 = step_lfo_osc(&vc->lfo2_phase, &vc->lfo2_sh_q15, &vc->lfo_rng,
                                    vp->lfo2_wave, vp->lfo2_rate, frames, has_lfo2);
    if (has_lfo2) {
        int32_t dep2 = dsp_clamp_i32(vp->lfo2_depth, -512, 512);
        int32_t sig2 = (lfo2_q15 * dep2) >> 9;
        accum_lfo_mod(vp->lfo2_dest, sig2, m);
    }

    bool has_lfo3 = (vp->lfo3_depth != 0);
    int32_t lfo3_q15 = step_lfo_osc(&vc->lfo3_phase, &vc->lfo3_sh_q15, &vc->lfo_rng,
                                    vp->lfo3_wave, vp->lfo3_rate, frames, has_lfo3);
    if (has_lfo3) {
        int32_t dep3 = dsp_clamp_i32(vp->lfo3_depth, -512, 512);
        int32_t sig3 = (lfo3_q15 * dep3) >> 9;
        accum_lfo_mod(vp->lfo3_dest, sig3, m);
    }

    bool need_lfo1 = (lfo1_tot_dep != 0) || force_lfo1_out;
    int16_t eff_lfo1_rate = (int16_t)dsp_clamp_i32((int32_t)vp->lfo_rate + m->lfo1_rate, 0, 1023);
    int32_t lfo1_q15 = step_lfo_osc(&vc->lfo_phase, &vc->lfo_sh_q15, &vc->lfo_rng,
                                    vp->lfo_wave, eff_lfo1_rate, frames, need_lfo1);
    if (lfo1_tot_dep != 0 && vp->lfo_dest < ADRIFT_LFO1_DEST_COUNT) {
        int32_t sig1 = (lfo1_q15 * lfo1_tot_dep) >> 9;
        accum_lfo_mod(vp->lfo_dest, sig1, m);
    }

    return lfo1_q15;
}

/* Exact 32-bit split Q15 master pot scaling on clamped Q23 bus ([-67108864, +67108864]) */
DSP_ALWAYS_INLINE int32_t master_mul_gain_q23(int32_t acc_q23, int32_t mg_q15) {
    int32_t acc  = dsp_clamp_i32(acc_q23, -67108864, 67108864);
    int32_t mg15 = (mg_q15 >= 32767) ? 32768 : dsp_clamp_i32(mg_q15, 0, 32767);
    if (acc == 0 || mg15 == 0) {
        return 0;
    }
    int32_t acc_hi  = acc >> 12; /* [-16384, +16384] */
    int32_t acc_lo  = acc & 4095;
    int32_t prod_hi = acc_hi * mg15;
    int32_t prod_lo = acc_lo * mg15;
    return (prod_hi >> 3) + ((prod_lo + ((prod_hi & 7) << 12) + 16384) >> 15);
}

/* Smooth C1-continuous rational soft-knee limiter in Q23 above -2.5 dBFS (6,291,456 -> 8,388,607) */
DSP_ALWAYS_INLINE int32_t master_soft_clip_q23(int32_t y) {
    if (y >= -6291456 && y <= 6291456) {
        return y;
    }
    int32_t  ay = (y < 0) ? -y : y;
    uint32_t d  = (uint32_t)(ay - 6291456);
    if (d > 65011712u) d = 65011712u;
    uint32_t den  = 2097152u + d;
    uint32_t n1   = d << 5;
    uint32_t q1   = n1 / den;
    uint32_t r1   = n1 - q1 * den;
    uint32_t n2   = r1 << 5;
    uint32_t q2   = n2 / den;
    uint32_t r2   = n2 - q2 * den;
    uint32_t n3   = r2 << 5;
    uint32_t q3   = n3 / den;
    uint32_t r3   = n3 - q3 * den;
    uint32_t q4   = (r3 << 6) / den;
    int32_t  frac = (int32_t)((q1 << 16) + (q2 << 11) + (q3 << 6) + q4);
    int32_t  out  = 6291456 + frac;
    if (out > 8388607) out = 8388607;
    return (y < 0) ? -out : out;
}

typedef struct {
    int32_t dry_g;
    int32_t wet_g;
    int32_t grp_gl;
    int32_t grp_gr;
    int32_t dry_step;
    int32_t wet_step;
    int32_t grp_l_step;
    int32_t grp_r_step;
} synth_mix_gains_t;

static DSP_HOT_RAM void synth_accum_super_ram(int32_t *exc_buf,
                                              const int32_t *wet_buf,
                                              uint32_t frames) {
    for (uint32_t i = 0; i < frames; i++) {
        exc_buf[i] += wet_buf[i];
    }
}

static DSP_HOT_RAM void synth_exc_vca_ram(int32_t *exc_buf,
                                          const int32_t *env1_buf,
                                          uint32_t frames) {
    for (uint32_t i = 0; i < frames; i++) {
        int32_t e = dsp_clamp_i32(exc_buf[i], -65535, 65535);
        exc_buf[i] = dsp_mul_q15(e, env1_buf[i]);
    }
}

/* Overflow-free 32-bit voice VCA + group/pan scale producing full 24-bit signed Q23 */
DSP_ALWAYS_INLINE int32_t voice_mix_scale_q23(int32_t mixed, int32_t e2_q23, int32_t grp_g) {
    if (mixed == 0 || e2_q23 <= 0 || grp_g <= 0) {
        return 0;
    }
    int32_t mx    = dsp_clamp_i32(mixed, -46340, 46340);
    int32_t gg    = dsp_clamp_i32(grp_g, 0, 60000);
    int32_t e2_hi = e2_q23 >> 8;  /* 0..32768 */
    int32_t e2_lo = e2_q23 & 255; /* 0..255 */
    int32_t p_hi  = e2_hi * gg;
    int32_t p_lo  = e2_lo * gg;
    int32_t g_q22 = (p_hi >> 8) + ((p_lo + ((p_hi & 255) << 8) + 32768) >> 16);
    int32_t gl_hi = g_q22 >> 9;   /* 0..30000 */
    int32_t gl_lo = g_q22 & 511;  /* 0..511 */
    int32_t m_hi  = mx * gl_hi;
    int32_t m_lo  = mx * gl_lo;
    return (m_hi >> 5) + ((m_lo + ((m_hi & 31) << 9) + 8192) >> 14);
}

static DSP_HOT_RAM void synth_mix_voice_ram(const synth_mix_gains_t *g,
                                            const int32_t *inlpf_buf,
                                            const int32_t *wet_buf,
                                            const int32_t *env2_q23_buf,
                                            int32_t *accum_l,
                                            int32_t *accum_r,
                                            uint32_t frames,
                                            bool inlpf_zero) {
    int32_t wet_g      = g->wet_g;
    int32_t grp_gl     = g->grp_gl;
    int32_t grp_gr     = g->grp_gr;
    int32_t wet_step   = g->wet_step;
    int32_t grp_l_step = g->grp_l_step;
    int32_t grp_r_step = g->grp_r_step;

    if (inlpf_zero) {
        if ((wet_step | grp_l_step | grp_r_step) == 0) {
            bool mono = (grp_gl == grp_gr);
            for (uint32_t i = 0; i < frames; i++) {
                int32_t mixed = dsp_mul_q15(wet_buf[i], wet_g);
                int32_t e2    = env2_q23_buf[i];
                int32_t ol    = voice_mix_scale_q23(mixed, e2, grp_gl);
                accum_l[i]   += ol;
                accum_r[i]   += mono ? ol : voice_mix_scale_q23(mixed, e2, grp_gr);
            }
        } else {
            for (uint32_t i = 0; i < frames; i++) {
                wet_g  += wet_step;
                grp_gl += grp_l_step;
                grp_gr += grp_r_step;
                int32_t mixed = dsp_mul_q15(wet_buf[i], wet_g);
                int32_t e2    = env2_q23_buf[i];
                int32_t ol    = voice_mix_scale_q23(mixed, e2, grp_gl);
                accum_l[i]   += ol;
                accum_r[i]   += (grp_gl == grp_gr) ? ol : voice_mix_scale_q23(mixed, e2, grp_gr);
            }
        }
    } else {
        int32_t dry_g    = g->dry_g;
        int32_t dry_step = g->dry_step;
        if ((dry_step | wet_step | grp_l_step | grp_r_step) == 0) {
            bool mono = (grp_gl == grp_gr);
            for (uint32_t i = 0; i < frames; i++) {
                int32_t mixed = dsp_mul_q15(inlpf_buf[i], dry_g) + dsp_mul_q15(wet_buf[i], wet_g);
                int32_t e2    = env2_q23_buf[i];
                int32_t ol    = voice_mix_scale_q23(mixed, e2, grp_gl);
                accum_l[i]   += ol;
                accum_r[i]   += mono ? ol : voice_mix_scale_q23(mixed, e2, grp_gr);
            }
        } else {
            for (uint32_t i = 0; i < frames; i++) {
                dry_g  += dry_step;
                wet_g  += wet_step;
                grp_gl += grp_l_step;
                grp_gr += grp_r_step;
                int32_t mixed = dsp_mul_q15(inlpf_buf[i], dry_g) + dsp_mul_q15(wet_buf[i], wet_g);
                int32_t e2    = env2_q23_buf[i];
                int32_t ol    = voice_mix_scale_q23(mixed, e2, grp_gl);
                accum_l[i]   += ol;
                accum_r[i]   += (grp_gl == grp_gr) ? ol : voice_mix_scale_q23(mixed, e2, grp_gr);
            }
        }
    }
}

static DSP_HOT_RAM void synth_sum_cores_ram(int32_t *accum_l,
                                            int32_t *accum_r,
                                            const int32_t *c1_l,
                                            const int32_t *c1_r,
                                            uint32_t frames) {
    for (uint32_t i = 0; i < frames; i++) {
        accum_l[i] += c1_l[i];
        accum_r[i] += c1_r[i];
    }
}

static DSP_HOT_RAM void synth_master_out_q23_ram(adrift_synth_t *s,
                                                 const int32_t *accum_l,
                                                 const int32_t *accum_r,
                                                 int32_t *stereo_q23_out,
                                                 int32_t target_master,
                                                 int32_t mg_step,
                                                 uint32_t frames) {
    int32_t mg     = s->master_gain_q15;
    int32_t peak   = s->peak_out_q15 - (s->peak_out_q15 >> 5);
    uint8_t swr    = s->scope_wr;
    bool    frozen = s->scope_frozen;

    for (uint32_t i = 0; i < frames; i++) {
        mg += mg_step;
        int32_t yl    = master_mul_gain_q23(accum_l[i], mg);
        int32_t yr    = master_mul_gain_q23(accum_r[i], mg);
        int32_t q23_l = master_soft_clip_q23(yl);
        int32_t q23_r = master_soft_clip_q23(yr);
        stereo_q23_out[2u * i]      = q23_l;
        stereo_q23_out[2u * i + 1u] = q23_r;

        int16_t s16_l = dsp_sat16((q23_l + 128) >> 8);
        int16_t s16_r = dsp_sat16((q23_r + 128) >> 8);
        int32_t ay_l  = s16_l < 0 ? -(int32_t)s16_l : (int32_t)s16_l;
        int32_t ay_r  = s16_r < 0 ? -(int32_t)s16_r : (int32_t)s16_r;
        int32_t ay    = (ay_l > ay_r) ? ay_l : ay_r;
        if (ay > peak) peak = ay;

        /* Decimate by 4 (~11 kHz) into 128-sample UI scope ring (~11.6 ms window) */
        if (!frozen && (i & 3u) == 0u) {
            uint32_t sidx = swr & (ADRIFT_SCOPE_LEN - 1u);
            s->scope_ring_l[sidx] = s16_l;
            s->scope_ring_r[sidx] = s16_r;
            s->scope_ring[sidx]   = (int16_t)(((int32_t)s16_l + (int32_t)s16_r) >> 1);
            swr++;
        }
    }

    s->master_gain_q15 = target_master;
    s->peak_out_q15    = peak;
    s->scope_wr        = swr;
}

typedef struct {
    int32_t tot_dep;
    int32_t pb_q16;
    int32_t spread_q8;
    int32_t base_vol;
    int32_t super_vco_scale_q15;
} adrift_group_block_prep_t;

typedef struct {
    int32_t accum_l[ADRIFT_BLOCK_SIZE];
    int32_t accum_r[ADRIFT_BLOCK_SIZE];
    int32_t exc_buf[ADRIFT_BLOCK_SIZE];
    int32_t env1_buf[ADRIFT_BLOCK_SIZE];
    int32_t inlpf_buf[ADRIFT_BLOCK_SIZE];
    int32_t wet_buf[ADRIFT_BLOCK_SIZE];
    int32_t env2_buf[ADRIFT_BLOCK_SIZE];
} adrift_core_scratch_t;

typedef struct {
    adrift_synth_t           *synth;
    adrift_group_block_prep_t grp_prep[ADRIFT_MAX_GROUPS];
    uint8_t                   vlist[ADRIFT_MAX_VOICES];
    uint8_t                   vcount;
} adrift_c1_job_t;

static adrift_core_scratch_t g_core_scratch[2] __attribute__((aligned(64)));
static adrift_c1_job_t       g_c1_job;

static void render_voice_list(adrift_synth_t *s,
                              const adrift_group_block_prep_t *grp_prep,
                              const uint8_t *vlist,
                              uint8_t vcount,
                              uint32_t frames,
                              adrift_core_scratch_t *sc) {
    int32_t *accum_l   = sc->accum_l;
    int32_t *accum_r   = sc->accum_r;
    int32_t *exc_buf   = sc->exc_buf;
    int32_t *env1_buf  = sc->env1_buf;
    int32_t *inlpf_buf = sc->inlpf_buf;
    int32_t *wet_buf   = sc->wet_buf;
    int32_t *env2_buf  = sc->env2_buf;

    for (uint32_t i = 0; i < frames; i++) {
        accum_l[i] = 0;
        accum_r[i] = 0;
    }

    for (uint8_t idx = 0u; idx < vcount; idx++) {
        uint8_t         v  = vlist[idx];
        adrift_voice_t *vc = &s->voices[v];
        int8_t          og = vc->owner_group;
        const adrift_group_t            *gp = &s->groups[og];
        const adrift_patch_t            *vp = &gp->patch;
        const adrift_group_block_prep_t *gr = &grp_prep[og];
        bool is_super = (gp->assign_mode == ADRIFT_GROUP_SUPER);

        /* 1a. Per-voice LFO 1..3 + Env 1 DADSR */
        int32_t tot_dep = gr->tot_dep;

        adrift_mod_accum_t m;
        clear_mod_accum(&m);
        int32_t lfo_q15 = step_voice_lfos_and_accum(vc, vp, tot_dep, frames, &m, v == 0u);
        if (v == 0u) {
            s->lfo_out_q15 = lfo_q15;
        }

        bool    hold_onset      = false;
        bool    onset_fading    = false;
        bool    onset_fade_done = false;
        int32_t env1_q15        = 0;
        if (vc->env1.stage != ADRIFT_ENV_IDLE || vp->env1.drone_hold) {
            adrift_env_params_t env1_p = vp->env1;
            env1_p.attack      = (int16_t)dsp_clamp_i32((int32_t)vp->env1.attack      + m.env1_atk, 0, 1023);
            env1_p.decay       = (int16_t)dsp_clamp_i32((int32_t)vp->env1.decay       + m.env1_dec, 0, 1023);
            env1_p.sustain     = (int16_t)dsp_clamp_i32((int32_t)vp->env1.sustain     + m.env1_sus, 0, 1023);
            env1_p.release     = (int16_t)dsp_clamp_i32((int32_t)vp->env1.release     + m.env1_rel, 0, 1023);
            env1_p.onset_delay = (int16_t)dsp_clamp_i32((int32_t)vp->env1.onset_delay + m.env1_dly, 0, 1023);
            adrift_env_update_coefs(&vc->env1, &env1_p);
            hold_onset   = (vc->env1.stage == ADRIFT_ENV_DELAY && vc->env1.delay_rem > frames);
            onset_fading = hold_onset && (vc->env1.declick_q30 > 0);
            adrift_env_process(&vc->env1, &env1_p, env1_buf, frames);
            onset_fade_done = onset_fading && (vc->env1.declick_q30 <= 0);
            env1_q15 = (env1_buf[0] + env1_buf[frames - 1u]) >> 1;
        }

        int32_t pb_q16        = gr->pb_q16;
        int32_t spread_q8     = gr->spread_q8;
        int32_t semi_q8       = 0;
        int32_t detune_q8     = 0;
        int32_t slot_gain_q15 = 32767;
        adrift_synth_compute_stack_slot_voicing(gp, vc->group_slot, spread_q8, &semi_q8, &detune_q8, &slot_gain_q15);

        int32_t note_t          = dsp_clamp_i32((int32_t)vc->note + (int32_t)gp->transpose, 0, 127);
        int32_t center_pitch_q8 = note_t << 8;
        int32_t base_pitch_q8   = center_pitch_q8 + semi_q8 + detune_q8;
        /* In SUPER mode, slots 0 (Left) and 1 (Right) form a stereo BBD pair with opposite micro-detune (+-11c max) */
        int32_t super_bbd_det_q8 = (is_super && gp->num_voices > 1u)
                                       ? ((spread_q8 * ((vc->group_slot == 0u) ? -88 : +96)) >> 10)
                                       : 0;
        int32_t bbd_pitch_q8    = is_super ? (center_pitch_q8 + super_bbd_det_q8) : base_pitch_q8;

        /* Advance unified voice portamento first so Exciter VCO(s) and BBD Resonator glide in lockstep */
        adrift_bbd_params_t bbd_p = vp->bbd;
        bbd_p.dispersion = (int16_t)dsp_clamp_i32((int32_t)vp->bbd.dispersion + m.dispr, 0, 1023);
        (void)adrift_bbd_step_glide(&vc->bbd, bbd_p.glide, bbd_pitch_q8, frames);
        int32_t porta_delta_q16 = vc->bbd.slew_pitch_q16 - (bbd_pitch_q8 * 256);

        int32_t eff_exc_lvl   = dsp_clamp_i32((int32_t)vp->exc.vco_level + m.ex_lvl, 0, 1023);
        int32_t eff_noise_mix = dsp_clamp_i32((int32_t)vp->exc.noise_level + m.ex_mix, 0, 1023);

        /* 2a. Exciter + Env 1 -> Exciter VCA (with silent-exciter fast path during BBD ringout!) */
        bool exc_silent = (env1_q15 == 0 && vc->env1.stage == ADRIFT_ENV_IDLE && !vp->env1.drone_hold) ||
                          (eff_exc_lvl == 0 &&
                           vc->exc.vco_gain_q15 == 0 && vc->exc.noise_gain_q15 == 0 &&
                           vc->exc.impulse_phase == 0u);
        bool inlpf_zero = false;

        if (exc_silent) {
            vc->exc.vco_phase += vc->exc.vco_inc * frames;
            vc->exc.sub_phase += (vc->exc.vco_inc >> 1) * frames;
            if (is_super && gp->num_voices > 1u) {
                for (uint8_t w = 0; w < ADRIFT_MAX_VOICES; w++) {
                    adrift_voice_t *vw = &s->voices[w];
                    if (w != v && vw->owner_group == og &&
                        vw->group_slot > 1u && (vw->group_slot & 1u) == (vc->group_slot & 1u)) {
                        adrift_mod_accum_t w_dummy;
                        clear_mod_accum(&w_dummy);
                        (void)step_voice_lfos_and_accum(vw, vp, 0, frames, &w_dummy, false);
                        vw->exc.vco_phase += vw->exc.vco_inc * frames;
                        vw->exc.sub_phase += (vw->exc.vco_inc >> 1) * frames;
                    }
                }
            }
            if (vc->in_lpf.ic1eq_q20 > -16 && vc->in_lpf.ic1eq_q20 < 16 &&
                vc->in_lpf.ic2eq_q20 > -16 && vc->in_lpf.ic2eq_q20 < 16) {
                vc->in_lpf.ic1eq_q20 = 0;
                vc->in_lpf.ic2eq_q20 = 0;
                for (uint32_t i = 0; i < frames; i++) {
                    inlpf_buf[i] = 0;
                }
                inlpf_zero = true;
            } else {
                for (uint32_t i = 0; i < frames; i++) {
                    exc_buf[i] = 0;
                }
            }
        } else {
            int16_t morph_1023 = 0;
            adrift_exciter_params_t slot_exc = vp->exc;
            slot_exc.vco_level   = (int16_t)eff_exc_lvl;
            slot_exc.noise_level = (int16_t)eff_noise_mix;
            slot_exc.sh_rate     = (int16_t)dsp_clamp_i32((int32_t)vp->exc.sh_rate + m.sh_clk, 0, 1023);

            int32_t super_vco_scale_q15 = gr->super_vco_scale_q15;
            int32_t super_wave_lvl      = 0;
            if (is_super && gp->num_voices > 1u) {
                /* Scale VCO wave level across per-channel stacked oscillators while preserving 100% stereo noise punch */
                int32_t lvl_1023 = eff_exc_lvl;
                int32_t mix_1023 = eff_noise_mix;
                int32_t raw_wave = (int32_t)dsp_udiv_1023((uint32_t)(lvl_1023 * (1023 - mix_1023)));
                int32_t raw_nois = (int32_t)dsp_udiv_1023((uint32_t)(lvl_1023 * mix_1023));
                super_wave_lvl   = (raw_wave * super_vco_scale_q15) >> 15;
                int32_t tot_lvl  = super_wave_lvl + raw_nois;
                slot_exc.vco_level   = (int16_t)dsp_clamp_i32(tot_lvl, 0, 1023);
                slot_exc.noise_level = (tot_lvl > 0) ? (int16_t)dsp_clamp_i32((raw_nois * 1023) / tot_lvl, 0, 1023) : 0;
            }
            uint32_t saved_vco_ph = vc->exc.vco_phase;
            uint32_t saved_sub_ph = vc->exc.sub_phase;
            uint32_t saved_imp_ph = vc->exc.impulse_phase;
            if (onset_fading) {
                vc->exc.impulse_phase = 0u;
            }
            adrift_exciter_prepare_block_q16(&vc->exc, &slot_exc,
                                             (base_pitch_q8 * 256) + porta_delta_q16 + pb_q16, m.vco_pitch_q8,
                                             m.vco_morph, m.vco_pwm, &morph_1023);
            adrift_exciter_process(&vc->exc, &slot_exc, morph_1023, exc_buf, frames);
            if (hold_onset && slot_exc.pluck > 0) {
                if (!onset_fading) {
                    vc->exc.vco_phase = saved_vco_ph;
                    vc->exc.sub_phase = saved_sub_ph;
                } else if (onset_fade_done && vc->exc.pending_phase_lock != 0u) {
                    uint32_t ph0 = (uint32_t)vc->exc.pending_uni_slot * 0x24924924u;
                    vc->exc.vco_phase          = ph0;
                    vc->exc.sub_phase          = ph0 >> 1;
                    vc->exc.pending_phase_lock = 0u;
                }
                vc->exc.impulse_phase = saved_imp_ph;
            }

            /* In SUPER mode, slot 0 sums even companion exciters (2,4..) and slot 1 sums odd companion exciters (3,5..) */
            if (is_super && gp->num_voices > 2u) {
                adrift_exciter_params_t sub_exc = slot_exc;
                sub_exc.noise_level = 0; /* 0% mix = pure wave on companion oscillators so stereo noise doesn't stack N times */
                sub_exc.pluck       = (int16_t)(((int32_t)slot_exc.pluck * super_vco_scale_q15) >> 15);
                for (uint8_t w = 0; w < ADRIFT_MAX_VOICES; w++) {
                    adrift_voice_t *vw = &s->voices[w];
                    if (w == v || vw->owner_group != og ||
                        vw->group_slot <= 1u || (vw->group_slot & 1u) != (vc->group_slot & 1u)) {
                        continue;
                    }
                    adrift_mod_accum_t w_mod;
                    clear_mod_accum(&w_mod);
                    (void)step_voice_lfos_and_accum(vw, vp, tot_dep, frames, &w_mod, false);

                    int32_t w_semi_q8   = 0;
                    int32_t w_detune_q8 = 0;
                    int32_t w_gain_q15  = 32767;
                    adrift_synth_compute_stack_slot_voicing(gp, vw->group_slot, spread_q8, &w_semi_q8, &w_detune_q8, &w_gain_q15);
                    sub_exc.vco_level = (int16_t)((super_wave_lvl * w_gain_q15) >> 15);

                    int16_t  w_morph  = 0;
                    uint32_t w_vco_ph = vw->exc.vco_phase;
                    uint32_t w_sub_ph = vw->exc.sub_phase;
                    uint32_t w_imp_ph = vw->exc.impulse_phase;
                    if (onset_fading) {
                        vw->exc.impulse_phase = 0u;
                    }
                    int32_t  w_pitch_q16 = ((center_pitch_q8 + w_semi_q8 + w_detune_q8) * 256) + porta_delta_q16;
                    vw->bbd.slew_pitch_q16    = w_pitch_q16;
                    vw->bbd.pitch_initialized = true;
                    adrift_exciter_prepare_block_q16(&vw->exc, &sub_exc,
                                                     w_pitch_q16 + pb_q16, w_mod.vco_pitch_q8,
                                                     w_mod.vco_morph, w_mod.vco_pwm, &w_morph);
                    adrift_exciter_process(&vw->exc, &sub_exc, w_morph, wet_buf, frames);
                    if (hold_onset && sub_exc.pluck > 0) {
                        if (!onset_fading) {
                            vw->exc.vco_phase = w_vco_ph;
                            vw->exc.sub_phase = w_sub_ph;
                        } else if (onset_fade_done && vw->exc.pending_phase_lock != 0u) {
                            uint32_t ph0 = (uint32_t)vw->exc.pending_uni_slot * 0x24924924u;
                            vw->exc.vco_phase          = ph0;
                            vw->exc.sub_phase          = ph0 >> 1;
                            vw->exc.pending_phase_lock = 0u;
                        }
                        vw->exc.impulse_phase = w_imp_ph;
                    }
                    DSP_CALL_RAM(synth_accum_super_ram)(exc_buf, wet_buf, frames);
                }
            }

            DSP_CALL_RAM(synth_exc_vca_ram)(exc_buf, env1_buf, frames);
        }

        /* Subtle per-voice analog filter component tolerance for stacked/poly companion voices (slot 0 = exact 0) */
        int32_t flt_tol = (gp->num_voices > 1u && vc->group_slot > 0u)
                              ? (((int32_t)(vc->group_slot & 7u) - 3) * 4)
                              : 0;

        /* 2b. 12 dB/oct Input Low-Pass Filter (feeds BOTH Dry path and BBD Resonator) */
        if (!inlpf_zero) {
            int32_t in_cut = dsp_clamp_i32((int32_t)vp->in_lpf_cutoff + m.in_lpf + flt_tol, 0, 1023);
            int32_t in_res = dsp_clamp_i32((int32_t)vp->in_lpf_res + m.in_res, 0, 1023);
            adrift_svf_set_params(&vc->in_lpf, (int16_t)in_cut, (int16_t)in_res);
            adrift_svf_process_lpf(&vc->in_lpf, exc_buf, inlpf_buf, frames);
        }

        /* 2c. 1024-stage Pitch-Tracked BBD Resonator (glide already stepped at start of voice block) */
        int32_t target_fb_q14  = 0;
        int32_t saved_slew_q16 = vc->bbd.slew_pitch_q16;
        vc->bbd.slew_pitch_q16 = saved_slew_q16 + pb_q16;
        adrift_bbd_prepare_block_ex(&vc->bbd, &bbd_p,
                                    bbd_pitch_q8, m.bbd_pitch_q8,
                                    m.fb_amt, m.fb_lpf + flt_tol, frames, &target_fb_q14, false);
        vc->bbd.slew_pitch_q16 = saved_slew_q16;
        adrift_bbd_process(&vc->bbd, &bbd_p, target_fb_q14, inlpf_buf, wet_buf, frames);

        /* 2d. Dry/Wet Crossfade + Post-BBD Envelope 2 Output VCA + Group/Unison Gain + Stereo Pan */
        bool env2_active = (vc->env2.stage != ADRIFT_ENV_IDLE || vp->env2.drone_hold);
        if (env2_active) {
            adrift_env_update_coefs(&vc->env2, &vp->env2);
            adrift_env_process_q23(&vc->env2, &vp->env2, env2_buf, frames);
        } else {
            vc->env2.level_q30 = 0;
        }

        int32_t dw = dsp_clamp_i32((int32_t)vp->dry_wet + m.dry_wet, 0, 1023);
        if (is_super && gp->num_voices > 1u && eff_exc_lvl > 0 && eff_noise_mix < 1023 && dw > 640) {
            /* In SUPER mode, preserve at least ~38% post-LPF multi-VCO body alongside the stereo BBD pair
             * so high-Q BBD comb filtering does not strip away the detuned companion VCO sidebands */
            dw = 640 + ((dw - 640) >> 2);
        }
        int32_t target_dry_q15 = 0;
        int32_t target_wet_q15 = 0;
        adrift_synth_dry_wet_gains_q15(dw, &target_dry_q15, &target_wet_q15);

        int32_t uni_scale = 32767;
        if (gp->assign_mode == ADRIFT_GROUP_UNISON) {
            uni_scale = dsp_mul_q15(k_unison_gain_q15[gp->num_voices], slot_gain_q15);
        } else if (is_super && gp->num_voices > 1u) {
            uni_scale = k_unison_gain_q15[2]; /* Stereo BBD pair constant-power normalization */
        }
        int32_t base_vol       = gr->base_vol;
        int32_t vca_scale_1023 = dsp_clamp_i32(1023 + m.vca, 0, 1280);
        int32_t eff_vol        = dsp_clamp_i32((int32_t)dsp_udiv_1023((uint32_t)(base_vol * vca_scale_1023)), 0, 1280);
        int32_t target_grp_q15 = (int32_t)dsp_udiv_1023((uint32_t)(eff_vol * uni_scale));

        int32_t pan_l_q15 = 32767;
        int32_t pan_r_q15 = 32767;
        adrift_synth_get_voice_pan_gains_cached_q15(vc, gp, m.pan, m.spread, &pan_l_q15, &pan_r_q15);
        int32_t target_grp_l_q15 = dsp_mul_q15(target_grp_q15, pan_l_q15);
        int32_t target_grp_r_q15 = dsp_mul_q15(target_grp_q15, pan_r_q15);

        if (env2_active) {
            synth_mix_gains_t mg;
            mg.dry_g      = vc->dry_gain_q15;
            mg.wet_g      = vc->wet_gain_q15;
            mg.grp_gl     = vc->grp_gain_l_q15;
            mg.grp_gr     = vc->grp_gain_r_q15;
            mg.dry_step   = dsp_div_frames_i32(target_dry_q15   - mg.dry_g,  frames);
            mg.wet_step   = dsp_div_frames_i32(target_wet_q15   - mg.wet_g,  frames);
            mg.grp_l_step = dsp_div_frames_i32(target_grp_l_q15 - mg.grp_gl, frames);
            mg.grp_r_step = dsp_div_frames_i32(target_grp_r_q15 - mg.grp_gr, frames);
            DSP_CALL_RAM(synth_mix_voice_ram)(&mg, inlpf_buf, wet_buf, env2_buf, accum_l, accum_r, frames, inlpf_zero);
        }

        vc->dry_gain_q15   = target_dry_q15;
        vc->wet_gain_q15   = target_wet_q15;
        vc->grp_gain_l_q15 = target_grp_l_q15;
        vc->grp_gain_r_q15 = target_grp_r_q15;

        if (!vc->key_down && !vc->sustained &&
            vc->env2.stage == ADRIFT_ENV_IDLE && !vp->env2.drone_hold) {
            if (vc->bbd.peak_wet_q15 < 8) {
                vc->active = false;
                if (is_super) {
                    for (uint8_t w = 0; w < ADRIFT_MAX_VOICES; w++) {
                        if (s->voices[w].owner_group == og &&
                            s->voices[w].group_slot > 1u &&
                            (s->voices[w].group_slot & 1u) == (vc->group_slot & 1u)) {
                            s->voices[w].active = false;
                        }
                    }
                }
            }
        }
    }
}

static void c1_voice_worker(uint32_t frames) {
    render_voice_list(g_c1_job.synth,
                      g_c1_job.grp_prep,
                      g_c1_job.vlist,
                      g_c1_job.vcount,
                      frames,
                      &g_core_scratch[1]);
}

static inline int32_t slew_ctrl_exact(int32_t cur, int32_t target, uint32_t frames) {
    int32_t diff = target - cur;
    if (diff == 0) {
        return target;
    }
    /* ~4.4 ms de-zippering time constant (tau = 192 samples), monotonic & deadband-free */
    int32_t f    = (frames > 64u) ? 64 : (int32_t)frames;
    int32_t num  = diff * f;
    int32_t step = (num >= 0) ? ((num + 191) / 192) : -(((-num) + 191) / 192);
    if ((diff > 0 && cur + step > target) || (diff < 0 && cur + step < target)) {
        return target;
    }
    return cur + step;
}

static void render_subblock_q23(adrift_synth_t *s, int32_t *stereo_q23_out, uint32_t frames) {
    if (frames == 0u) return;

    /* Partition active voices by DSP cost (SUPER multi-VCO -> Heavy active exciter -> Light BBD ringout)
     * across CPU1 and CPU0, and track which groups have sounding voices for controller smoothing.
     * CPU1 receives the extra voice when active count is odd so CPU0 retains headroom for Master FX. */
    uint8_t super_v[ADRIFT_MAX_VOICES];
    uint8_t heavy_v[ADRIFT_MAX_VOICES];
    uint8_t light_v[ADRIFT_MAX_VOICES];
    uint8_t n_super = 0u, n_heavy = 0u, n_light = 0u;
    bool    grp_active[ADRIFT_MAX_GROUPS] = { false };

    for (uint8_t v = 0u; v < ADRIFT_MAX_VOICES; v++) {
        adrift_voice_t *vc = &s->voices[v];
        if (!vc->active || vc->owner_group < 0 || vc->owner_group >= ADRIFT_MAX_GROUPS) {
            continue;
        }
        const adrift_group_t *gp = &s->groups[vc->owner_group];
        if (gp->num_voices == 0u) {
            vc->active = false;
            continue;
        }
        grp_active[vc->owner_group] = true;
        bool is_super = (gp->assign_mode == ADRIFT_GROUP_SUPER);
        if (is_super && vc->group_slot > 1u) {
            /* Companion oscillators in a SUPER group are summed and stepped by stereo BBD slots 0 and 1 */
            continue;
        }
        bool exc_idle = (vc->env1.stage == ADRIFT_ENV_IDLE && !gp->patch.env1.drone_hold);
        if (is_super && gp->num_voices > 2u && !exc_idle) {
            super_v[n_super++] = v;
        } else if (!exc_idle) {
            heavy_v[n_heavy++] = v;
        } else {
            light_v[n_light++] = v;
        }
    }

    adrift_group_block_prep_t grp_prep[ADRIFT_MAX_GROUPS];
    for (uint8_t g = 0u; g < ADRIFT_MAX_GROUPS; g++) {
        adrift_group_t *gp = &s->groups[g];
        int32_t target_pb_q20 = (int32_t)gp->pitch_bend_q13 * 128;
        int32_t target_mw_q15 = (int32_t)dsp_udiv_127((uint32_t)gp->mod_wheel * 32767u);
        int32_t target_at_q15 = (int32_t)dsp_udiv_127((uint32_t)gp->aftertouch * 32767u);
        if (!grp_active[g] || gp->num_voices == 0u) {
            gp->smooth_pb_q20 = target_pb_q20;
            gp->smooth_mw_q15 = target_mw_q15;
            gp->smooth_at_q15 = target_at_q15;
            if (gp->num_voices == 0u) {
                continue;
            }
        } else {
            gp->smooth_pb_q20 = slew_ctrl_exact(gp->smooth_pb_q20, target_pb_q20, frames);
            gp->smooth_mw_q15 = slew_ctrl_exact(gp->smooth_mw_q15, target_mw_q15, frames);
            gp->smooth_at_q15 = slew_ctrl_exact(gp->smooth_at_q15, target_at_q15, frames);
        }
        const adrift_patch_t *vp = &gp->patch;
        int32_t at_amt    = (gp->smooth_at_q15 * 120 + 16384) >> 15;
        int32_t mw_amt    = (gp->smooth_mw_q15 * dsp_clamp_i32(vp->mw_depth, 0, 1023) + 16384) >> 15;
        int32_t extra_dep = (mw_amt >> 1) + at_amt;
        int32_t base_dep  = (int32_t)vp->lfo_depth;
        int32_t tot_dep   = (base_dep < 0) ? (base_dep - extra_dep) : (base_dep + extra_dep);
        grp_prep[g].tot_dep             = dsp_clamp_i32(tot_dep, -512, 512);
        grp_prep[g].pb_q16              = (gp->smooth_pb_q20 * (int32_t)vp->pb_range) >> 4;
        grp_prep[g].spread_q8           = adrift_detune_to_q8(gp->unison_detune);
        grp_prep[g].base_vol            = dsp_clamp_i32(vp->master_vol, 0, 1023);
        grp_prep[g].super_vco_scale_q15 = k_unison_gain_q15[(gp->num_voices + 1u) >> 1];
    }

    uint8_t n_total = (uint8_t)(n_super + n_heavy + n_light);
    uint8_t c0_list[ADRIFT_MAX_VOICES];
    uint8_t c0_count = 0u;
    uint8_t c1_count = 0u;

    if (n_total == 1u) {
        c0_list[c0_count++] = (n_super > 0u) ? super_v[0] : ((n_heavy > 0u) ? heavy_v[0] : light_v[0]);
    } else if (n_total >= 2u) {
        bool to_c1 = true;
        for (uint8_t i = 0u; i < n_super; i++) {
            if (to_c1) g_c1_job.vlist[c1_count++] = super_v[i];
            else       c0_list[c0_count++]        = super_v[i];
            to_c1 = !to_c1;
        }
        for (uint8_t i = 0u; i < n_heavy; i++) {
            if (to_c1) g_c1_job.vlist[c1_count++] = heavy_v[i];
            else       c0_list[c0_count++]        = heavy_v[i];
            to_c1 = !to_c1;
        }
        for (uint8_t i = 0u; i < n_light; i++) {
            if (to_c1) g_c1_job.vlist[c1_count++] = light_v[i];
            else       c0_list[c0_count++]        = light_v[i];
            to_c1 = !to_c1;
        }
    }

    bool c1_dispatched = false;
    if (c1_count > 0u) {
        g_c1_job.synth  = s;
        g_c1_job.vcount = c1_count;
        for (uint8_t g = 0u; g < ADRIFT_MAX_GROUPS; g++) {
            g_c1_job.grp_prep[g] = grp_prep[g];
        }
        __asm__ volatile("" ::: "memory");
        c1_dispatched = adrift_platform_run_worker(c1_voice_worker, frames);
    }

    /* CPU0 renders its assigned half of active voices */
    render_voice_list(s, grp_prep, c0_list, c0_count, frames, &g_core_scratch[0]);
    int32_t *accum_l = g_core_scratch[0].accum_l;
    int32_t *accum_r = g_core_scratch[0].accum_r;

    if (c1_count > 0u) {
        if (!c1_dispatched || !adrift_platform_wait_worker()) {
            /* Single-core host simulator or CPU1 fallback: CPU0 renders CPU1's voice list */
            render_voice_list(s, grp_prep, g_c1_job.vlist, c1_count, frames, &g_core_scratch[1]);
        }
        DSP_CALL_RAM(synth_sum_cores_ram)(accum_l, accum_r, g_core_scratch[1].accum_l, g_core_scratch[1].accum_r, frames);
    }

    /* 2b. Master Stereo Bus / Send Effects (Chorus -> Ping-Pong Delay -> Shimmer Reverb)
     * Preserve full 24-bit Q23 sub-LSB dry resolution across FX stages and parallel bus compression. */
    s->fx.clk_period_q4 = s->midi_clk_active ? s->midi_clk_period_q4 : 0u;
    if (adrift_fx_is_active(&s->fx_params)) {
        int32_t *fx_l  = g_core_scratch[0].exc_buf;
        int32_t *fx_r  = g_core_scratch[0].env1_buf;
        int32_t *res_l = g_core_scratch[0].inlpf_buf;
        int32_t *res_r = g_core_scratch[0].wet_buf;
        for (uint32_t i = 0u; i < frames; i++) {
            int32_t ql = dsp_clamp_i32(accum_l[i], -536870912, 536870912);
            int32_t qr = dsp_clamp_i32(accum_r[i], -536870912, 536870912);
            int32_t sl = (ql + 128) >> 8;
            int32_t sr = (qr + 128) >> 8;
            fx_l[i]    = sl;
            fx_r[i]    = sr;
            res_l[i]   = ql - sl * 256;
            res_r[i]   = qr - sr * 256;
        }
        adrift_fx_process(&s->fx, &s->fx_params, fx_l, fx_r, frames);
        int32_t comp_mix = dsp_clamp_i32(s->fx_params.comp_mix, 0, 1023);
        if (comp_mix > 0) {
            int32_t wet_q15   = (int32_t)dsp_udiv_1023((uint32_t)comp_mix * 32768u);
            int32_t eff_g_q14 = 16384 + (((s->fx.comp_gain_q14 - 16384) * wet_q15) >> 15);
            for (uint32_t i = 0u; i < frames; i++) {
                int32_t fl = dsp_clamp_i32(fx_l[i], -4194304, 4194304);
                int32_t fr = dsp_clamp_i32(fx_r[i], -4194304, 4194304);
                accum_l[i] = fl * 256 + ((res_l[i] * eff_g_q14 + 8192) >> 14);
                accum_r[i] = fr * 256 + ((res_r[i] * eff_g_q14 + 8192) >> 14);
            }
        } else {
            for (uint32_t i = 0u; i < frames; i++) {
                int32_t fl = dsp_clamp_i32(fx_l[i], -4194304, 4194304);
                int32_t fr = dsp_clamp_i32(fx_r[i], -4194304, 4194304);
                accum_l[i] = fl * 256 + res_l[i];
                accum_r[i] = fr * 256 + res_r[i];
            }
        }
    } else if (s->fx.buffers_dirty || s->fx.chorus_dirty || s->fx.delay_dirty || s->fx.reverb_dirty) {
        adrift_fx_process(&s->fx, &s->fx_params, g_core_scratch[0].exc_buf, g_core_scratch[0].env1_buf, frames);
    }

    /* 3. Hardware Master Pot, polyphonic Q23 soft-knee limiter, stereo Q23 output & UI scope capture */
    int32_t target_master = master_pot_to_gain_q15(s->hw_master_pot);
    int32_t mg_step       = dsp_div_frames_i32(target_master - s->master_gain_q15, frames);
    DSP_CALL_RAM(synth_master_out_q23_ram)(s, accum_l, accum_r, stereo_q23_out, target_master, mg_step, frames);
}

/* Zero-gated, zero-DC-bias 1-LSB TPDF dither from 24-bit Q23 to 16-bit PCM for host audio backends */
static void synth_dither_q23_to_s16(uint32_t *rng_io,
                                    const int32_t *q23_in,
                                    int16_t *s16_out,
                                    uint32_t frames) {
    uint32_t rng = *rng_io;
    for (uint32_t i = 0u; i < frames; i++) {
        int32_t ql = q23_in[2u * i];
        int32_t qr = q23_in[2u * i + 1u];
        if (ql == 0 && qr == 0) {
            s16_out[2u * i]      = 0;
            s16_out[2u * i + 1u] = 0;
            continue;
        }
        uint32_t r0 = dsp_xorshift32(&rng);
        int32_t  dl = (int32_t)(r0 & 0xFFu) - (int32_t)((r0 >> 8) & 0xFFu) + 127 + (int32_t)((r0 >> 16) & 1u);
        int32_t  dr;
        if (qr == ql) {
            dr = dl;
        } else {
            uint32_t r1 = dsp_xorshift32(&rng);
            dr = (int32_t)(r1 & 0xFFu) - (int32_t)((r1 >> 8) & 0xFFu) + 127 + (int32_t)((r1 >> 16) & 1u);
        }
        s16_out[2u * i]      = (ql == 0) ? 0 : dsp_sat16((ql + dl) >> 8);
        s16_out[2u * i + 1u] = (qr == 0) ? 0 : dsp_sat16((qr + dr) >> 8);
    }
    *rng_io = rng;
}

static void synth_render_block_impl(adrift_synth_t *s,
                                    int16_t *stereo_s16_out,
                                    int32_t *stereo_q23_out,
                                    uint32_t frames,
                                    const adrift_midi_msg_t *midi_events,
                                    uint32_t num_midi) {
    adrift_midi_msg_t sorted_buf[64];
    if (num_midi > 0u && frames > 0u) {
        uint16_t max_off = (frames > 0xFFFFu) ? 0xFFFFu : (uint16_t)(frames - 1u);
        bool need_fix = false;
        for (uint32_t i = 0u; i < num_midi; i++) {
            if (midi_events[i].sample_offset > max_off ||
                (i > 0u && midi_events[i].sample_offset < midi_events[i - 1u].sample_offset)) {
                need_fix = true;
                break;
            }
        }
        if (need_fix && num_midi <= 64u) {
            for (uint32_t i = 0u; i < num_midi; i++) {
                adrift_midi_msg_t m = midi_events[i];
                if (m.sample_offset > max_off) {
                    m.sample_offset = max_off;
                }
                uint32_t j = i;
                while (j > 0u && sorted_buf[j - 1u].sample_offset > m.sample_offset) {
                    sorted_buf[j] = sorted_buf[j - 1u];
                    j--;
                }
                sorted_buf[j] = m;
            }
            midi_events = sorted_buf;
        }
    }

    uint32_t cursor = 0u;
    uint32_t ev_idx = 0u;
    int32_t  q23_sub[2u * ADRIFT_BLOCK_SIZE];

    while (cursor < frames) {
        while (ev_idx < num_midi && midi_events[ev_idx].sample_offset <= cursor) {
            adrift_synth_handle_midi(s,
                                     midi_events[ev_idx].status,
                                     midi_events[ev_idx].data1,
                                     midi_events[ev_idx].data2);
            ev_idx++;
        }

        uint32_t next_stop = frames;
        if (ev_idx < num_midi && midi_events[ev_idx].sample_offset < next_stop) {
            next_stop = midi_events[ev_idx].sample_offset;
        }
        uint32_t chunk = next_stop - cursor;
        if (chunk > ADRIFT_BLOCK_SIZE) {
            chunk = ADRIFT_BLOCK_SIZE;
        }
        uint32_t arp_rem = adrift_arp_samples_until_next_event(s);
        if (arp_rem == 0u) {
            adrift_arp_advance_samples(s, 0u);
            arp_rem = adrift_arp_samples_until_next_event(s);
        }
        if (arp_rem > 0u && arp_rem < chunk) {
            chunk = arp_rem;
        }
        if (stereo_q23_out) {
            render_subblock_q23(s, stereo_q23_out + (2u * cursor), chunk);
        } else {
            render_subblock_q23(s, q23_sub, chunk);
            synth_dither_q23_to_s16(&s->dither_rng, q23_sub, stereo_s16_out + (2u * cursor), chunk);
        }
        adrift_arp_advance_samples(s, chunk);
        if (s->midi_clk_since_last < 0x0FFFFFFFu) {
            s->midi_clk_since_last += chunk;
        }
        if (s->midi_clk_active) {
            if (s->midi_clk_timeout_samples > chunk) {
                s->midi_clk_timeout_samples -= chunk;
            } else {
                s->midi_clk_timeout_samples = 0u;
                s->midi_clk_active          = false;
                s->midi_clk_running         = false;
            }
        }
        cursor += chunk;
    }

    while (ev_idx < num_midi) {
        adrift_synth_handle_midi(s,
                                 midi_events[ev_idx].status,
                                 midi_events[ev_idx].data1,
                                 midi_events[ev_idx].data2);
        ev_idx++;
    }
}

void adrift_synth_render_block_q23(adrift_synth_t *s,
                                   int32_t *stereo_q23_out,
                                   uint32_t frames,
                                   const adrift_midi_msg_t *midi_events,
                                   uint32_t num_midi) {
    synth_render_block_impl(s, 0, stereo_q23_out, frames, midi_events, num_midi);
}

void adrift_synth_render_block(adrift_synth_t *s,
                               int16_t *stereo_out,
                               uint32_t frames,
                               const adrift_midi_msg_t *midi_events,
                               uint32_t num_midi) {
    synth_render_block_impl(s, stereo_out, 0, frames, midi_events, num_midi);
}
