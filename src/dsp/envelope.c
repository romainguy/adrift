#include "envelope.h"

#define ENV_ONE_Q30  (1 << 30)
#define ENV_OVER_Q30 1288490188 /* 1.20 * 2^30: analog RC attack overshoot target */

void adrift_env_init(adrift_env_t *env) {
    adrift_env_reset(env);
}

void adrift_env_reset(adrift_env_t *env) {
    env->stage            = ADRIFT_ENV_IDLE;
    env->gate             = false;
    env->just_triggered   = false;
    env->level_q30        = 0;
    env->declick_q30      = 0;
    env->declick_step_q30 = 0;
    env->vel_scale_q15    = 32767;
    env->delay_rem      = 0u;
    env->delay_total    = 0u;
    env->atk_coef_q14   = 27443;
    env->atk_shr        = 3u;
    env->atk_floor_q30  = 1 << 20;
    env->dec_coef_q14   = 24985;
    env->dec_shr        = 8u;
    env->dec_floor_q30  = 1 << 16;
    env->sus_level_q30  = ENV_ONE_Q30 >> 1;
    env->rel_coef_q14   = 24985;
    env->rel_shr        = 8u;
    env->rel_floor_q30  = 1 << 16;
    env->coefs_valid    = false;
}

/* Maps knob 0..1023 -> sample count [min_samples .. max_samples] with musical taper.
 * When extend_top is true (NORM / LONG), preserves exact percussive response at low/mid knobs
 * while smoothly scaling the upper throw so RC completion time matches 5 s / 10 s / 25 s. */
static uint32_t knob_to_samples(int16_t knob_1023, uint32_t min_s, uint32_t max_s, bool extend_top) {
    uint32_t k  = (uint32_t)dsp_clamp_i32(knob_1023, 0, 1023);
    uint32_t k2 = (k * k) >> 10;          /* 0..1022 */
    uint32_t k3 = (k2 * k) >> 10;         /* 0..1021 */
    uint32_t taper_q16;
    if (extend_top) {
        uint32_t w_q10 = 365u + ((659u * k2) >> 10); /* 0.356 at k=0 .. 1.000 at k=1023 */
        taper_q16 = (k3 * w_q10) >> 4;
    } else {
        taper_q16 = k3 << 6;
    }
    uint32_t span = max_s - min_s;
    return min_s + dsp_umul_q16(span, taper_q16);
}

/* Compute normalized 15-bit mantissa + shift RC rate (tau = samples / 3, floor = 1 / samples)
 * so every stage from 0.5 ms to 25 s retains continuous analog RC exponential curvature
 * without zeroing the RC coefficient above 1.11 seconds. */
static void samples_to_rc_rate(uint32_t samples,
                               int32_t *out_coef,
                               uint8_t *out_shr,
                               int32_t *out_floor_q30) {
    if (samples < 8u) samples = 8u;
    uint32_t pre_sh = (samples >= 1024u) ? 6u : 0u;
    uint32_t q      = dsp_div_u32_q16(98304u << pre_sh, samples);
    uint8_t  shr    = (uint8_t)(16u + pre_sh);
    while (q > 32768u && shr > 0u) {
        q >>= 1;
        shr--;
    }
    if (q < 1u) q = 1u;
    *out_coef = (int32_t)q;
    *out_shr  = shr;

    uint32_t flr = (uint32_t)ENV_ONE_Q30 / samples;
    if (flr < 64u) flr = 64u;
    *out_floor_q30 = (int32_t)flr;
}

void adrift_env_update_coefs(adrift_env_t *env, const adrift_env_params_t *p) {
    bool full = !env->coefs_valid || (env->last_range != p->range);
    if (!full &&
        env->last_onset_delay == p->onset_delay &&
        env->last_attack      == p->attack &&
        env->last_decay       == p->decay &&
        env->last_sustain     == p->sustain &&
        env->last_release     == p->release) {
        env->just_triggered = false;
        return;
    }

    /* Stage completion bounds @ 44.118 kHz (RC completion time T_atk ~= 0.356 * S):
     *   NORM  (0): Attack 0.5 ms .. 5 s, Decay/Release 1 ms .. 10 s
     *   SHORT (1): Attack/Decay/Release ~0.5 ms .. ~250 ms per stage
     *   LONG  (2): Attack/Decay/Release ~1 ms .. 25 s per stage
     */
    uint32_t atk_min = 22u, atk_max = 618800u;
    uint32_t dec_min = 44u, dec_max = 1237600u;
    bool     ext_top = true;
    if (p->range == ADRIFT_ENV_RANGE_SHORT) {
        atk_min = 16u;
        atk_max = 30940u;
        dec_min = 22u;
        dec_max = 30940u;
        ext_top = false;
    } else if (p->range == ADRIFT_ENV_RANGE_LONG) {
        atk_min = 44u;
        atk_max = 3094000u;
        dec_min = 44u;
        dec_max = 3094000u;
        ext_top = true;
    }

    if (full || env->last_onset_delay != p->onset_delay) {
        uint32_t prev_total = env->delay_total;
        /* Onset delay: 0 = 0 samples, 1..1023 = ~1 ms (44) .. 2000 ms (88,236 samples @ 44.118 kHz) */
        int32_t d = dsp_clamp_i32(p->onset_delay, 0, 1023);
        if (d == 0) {
            env->delay_total = 0u;
        } else {
            uint32_t u  = (uint32_t)d;
            uint32_t u2 = (u * u) >> 10; /* 0..1022 */
            env->delay_total = 44u + ((u2 * 88192u) >> 10);
        }
        env->last_onset_delay = p->onset_delay;

        if (env->just_triggered && env->gate) {
            if (env->delay_total > 0u) {
                int32_t cur_out = (env->stage == ADRIFT_ENV_DELAY)
                                      ? env->declick_q30
                                      : ((env->level_q30 >> 15) * env->vel_scale_q15);
                if (cur_out > 0) {
                    env->declick_q30      = cur_out;
                    env->declick_step_q30 = (env->delay_total >= 64u) ? ((cur_out >> 6) + 1) : ((cur_out >> 5) + 1);
                }
                env->delay_rem = env->delay_total;
                env->stage     = ADRIFT_ENV_DELAY;
                env->level_q30 = 0;
            } else if (env->stage == ADRIFT_ENV_DELAY) {
                env->delay_rem        = 0u;
                env->declick_q30      = 0;
                env->declick_step_q30 = 0;
                env->stage            = ADRIFT_ENV_ATTACK;
            }
        } else if (!full && env->stage == ADRIFT_ENV_DELAY && prev_total != env->delay_total) {
            uint32_t elapsed = (prev_total > env->delay_rem) ? (prev_total - env->delay_rem) : 0u;
            env->delay_rem   = (env->delay_total > elapsed) ? (env->delay_total - elapsed) : 0u;
        }
    }
    env->just_triggered = false;

    if (full || env->last_attack != p->attack) {
        uint32_t atk_s = knob_to_samples(p->attack, atk_min, atk_max, ext_top);
        samples_to_rc_rate(atk_s, &env->atk_coef_q14, &env->atk_shr, &env->atk_floor_q30);
        env->last_attack = p->attack;
    }

    if (full || env->last_decay != p->decay) {
        uint32_t dec_s = knob_to_samples(p->decay, dec_min, dec_max, ext_top);
        samples_to_rc_rate(dec_s, &env->dec_coef_q14, &env->dec_shr, &env->dec_floor_q30);
        env->last_decay = p->decay;
    }

    if (full || env->last_sustain != p->sustain) {
        /* Sustain: 0..1023 -> 0 .. ENV_ONE_Q30 */
        uint32_t sus = (uint32_t)dsp_clamp_i32(p->sustain, 0, 1023);
        env->sus_level_q30 = (sus >= 1023u) ? ENV_ONE_Q30 : (int32_t)(sus << 20);
        env->last_sustain  = p->sustain;
    }

    if (full || env->last_release != p->release) {
        uint32_t rel_s = knob_to_samples(p->release, dec_min, dec_max, ext_top);
        samples_to_rc_rate(rel_s, &env->rel_coef_q14, &env->rel_shr, &env->rel_floor_q30);
        env->last_release = p->release;
    }

    env->last_range  = p->range;
    env->coefs_valid = true;
}

void adrift_env_note_on(adrift_env_t *env, const adrift_env_params_t *p, uint8_t velocity, bool legato) {
    int32_t prev_out_q30 = (env->stage == ADRIFT_ENV_IDLE)
                               ? 0
                               : ((env->stage == ADRIFT_ENV_DELAY)
                                      ? env->declick_q30
                                      : ((env->level_q30 >> 15) * env->vel_scale_q15) + env->declick_q30);
    if (prev_out_q30 < 0) prev_out_q30 = 0;
    if (prev_out_q30 > ENV_ONE_Q30) prev_out_q30 = ENV_ONE_Q30;

    adrift_env_update_coefs(env, p);

    env->gate = true;
    if (legato && env->stage != ADRIFT_ENV_IDLE && env->stage != ADRIFT_ENV_RELEASE) {
        return;
    }

    int32_t vsens       = dsp_clamp_i32(p->vel_sens, 0, 1023);
    int32_t v_q15       = (int32_t)dsp_udiv_127((uint32_t)(velocity & 0x7Fu) * 32767u);
    /* Interpolate between 32767 (vsens=0) and v_q15 (vsens=1023) */
    int32_t new_vel_q15 = 32767 - (((32767 - v_q15) * vsens) >> 10);
    env->vel_scale_q15  = new_vel_q15;

    env->just_triggered = true;
    if (env->delay_total > 0u) {
        env->delay_rem = env->delay_total;
        env->stage     = ADRIFT_ENV_DELAY;
        env->level_q30 = 0;
        /* Ramp any active envelope level smoothly to 0 over <=64 samples instead of clicking */
        if (prev_out_q30 > 0) {
            env->declick_q30      = prev_out_q30;
            env->declick_step_q30 = (env->delay_total >= 64u) ? ((prev_out_q30 >> 6) + 1) : ((prev_out_q30 >> 5) + 1);
        } else {
            env->declick_q30      = 0;
            env->declick_step_q30 = 0;
        }
    } else {
        env->stage = ADRIFT_ENV_ATTACK;
        if (prev_out_q30 > 0) {
            int32_t prev_g15 = prev_out_q30 >> 15;
            if (new_vel_q15 > 0 && prev_g15 < new_vel_q15) {
                uint32_t ratio_q15    = (((uint32_t)prev_g15 << 15) + (uint32_t)(new_vel_q15 >> 1)) / (uint32_t)new_vel_q15;
                if (ratio_q15 > 32767u) ratio_q15 = 32767u;
                env->level_q30        = (int32_t)(ratio_q15 << 15);
                env->declick_q30      = 0;
                env->declick_step_q30 = 0;
            } else {
                env->level_q30     = (new_vel_q15 > 0) ? ENV_ONE_Q30 : 0;
                int32_t excess_q30 = prev_out_q30 - (new_vel_q15 << 15);
                if (excess_q30 > 0) {
                    env->declick_q30      = excess_q30;
                    env->declick_step_q30 = (excess_q30 >> 6) + 1;
                } else {
                    env->declick_q30      = 0;
                    env->declick_step_q30 = 0;
                }
            }
        } else {
            env->level_q30        = 0;
            env->declick_q30      = 0;
            env->declick_step_q30 = 0;
        }
    }
}

void adrift_env_note_off(adrift_env_t *env, const adrift_env_params_t *p) {
    (void)p;
    env->gate           = false;
    env->just_triggered = false;
    if (env->stage != ADRIFT_ENV_IDLE) {
        if (env->stage == ADRIFT_ENV_DELAY) {
            if (env->declick_q30 > 0) {
                env->delay_rem = 64u;
            } else {
                env->level_q30 = 0;
                env->stage     = ADRIFT_ENV_IDLE;
            }
        } else if (env->level_q30 <= 0 && env->declick_q30 <= 0) {
            env->level_q30 = 0;
            env->stage     = ADRIFT_ENV_IDLE;
        } else {
            env->stage = ADRIFT_ENV_RELEASE;
        }
    }
}

static DSP_HOT_RAM void env_process_stages_ram(adrift_env_t *env,
                                               uint8_t loop,
                                               int32_t *out_gain_q15,
                                               uint32_t frames) {
    adrift_env_stage_t st = env->stage;
    bool     gate       = env->gate;
    int32_t  vel_q15    = env->vel_scale_q15;
    int32_t  sus          = env->sus_level_q30;
    int32_t  lvl          = env->level_q30;
    int32_t  declick      = env->declick_q30;
    int32_t  declick_step = env->declick_step_q30;
    uint32_t drem         = env->delay_rem;
    uint32_t dtotal     = env->delay_total;
    int32_t  atk_coef   = env->atk_coef_q14;
    uint32_t atk_shr    = env->atk_shr;
    int32_t  atk_floor  = env->atk_floor_q30;
    int32_t  dec_coef   = env->dec_coef_q14;
    uint32_t dec_shr    = env->dec_shr;
    int32_t  dec_floor  = env->dec_floor_q30;
    int32_t  rel_coef   = env->rel_coef_q14;
    uint32_t rel_shr    = env->rel_shr;
    int32_t  rel_floor  = env->rel_floor_q30;
    int32_t  dec_target = (loop != ADRIFT_ENV_LOOP_OFF && sus > (ENV_ONE_Q30 - (1 << 24)))
                              ? 0 /* If sustain is ~100% and loop enabled, still cycle to 0 */
                              : sus;

    for (uint32_t i = 0; i < frames; i++) {
        if (st == ADRIFT_ENV_DECAY) {
            if (lvl > dec_target) {
                int32_t diff_hi = (lvl - dec_target) >> 15;
                int32_t step    = ((diff_hi * dec_coef) >> dec_shr) + dec_floor;
                lvl -= step;
                if (lvl <= dec_target) {
                    lvl = dec_target;
                }
            } else {
                lvl = dec_target;
            }

            if (lvl <= dec_target) {
                if (gate && loop != ADRIFT_ENV_LOOP_OFF) {
                    if (loop == ADRIFT_ENV_LOOP_DAD && dtotal > 0u) {
                        int32_t cur_out = ((lvl >> 15) * vel_q15) + declick;
                        if (cur_out > ENV_ONE_Q30) cur_out = ENV_ONE_Q30;
                        if (cur_out > 0) {
                            declick      = cur_out;
                            declick_step = (dtotal >= 64u) ? ((cur_out >> 6) + 1) : ((cur_out >> 5) + 1);
                        }
                        drem = dtotal;
                        lvl  = 0;
                        st   = ADRIFT_ENV_DELAY;
                    } else {
                        st = ADRIFT_ENV_ATTACK;
                    }
                } else {
                    st = (lvl <= 0 && declick <= 0 && !gate) ? ADRIFT_ENV_IDLE : ADRIFT_ENV_SUSTAIN;
                }
            }
        } else if (st == ADRIFT_ENV_RELEASE) {
            int32_t diff_hi = lvl >> 15;
            int32_t step    = ((diff_hi * rel_coef) >> rel_shr) + rel_floor;
            lvl -= step;
            if (lvl <= 0) {
                lvl = 0;
                if (declick <= 0) {
                    st = ADRIFT_ENV_IDLE;
                }
            }
        } else if (st == ADRIFT_ENV_ATTACK) {
            int32_t diff_hi = (ENV_OVER_Q30 - lvl) >> 15;
            int32_t step    = ((diff_hi * atk_coef) >> atk_shr) + atk_floor;
            lvl += step;
            if (lvl >= ENV_ONE_Q30) {
                lvl = ENV_ONE_Q30;
                st  = ADRIFT_ENV_DECAY;
            }
        } else if (st == ADRIFT_ENV_SUSTAIN) {
            lvl = sus;
            if (!gate) {
                st = (lvl <= 0 && declick <= 0) ? ADRIFT_ENV_IDLE : ADRIFT_ENV_RELEASE;
            } else if (loop != ADRIFT_ENV_LOOP_OFF) {
                st = ADRIFT_ENV_ATTACK;
            }
        } else if (st == ADRIFT_ENV_DELAY) {
            lvl = 0;
            if (!gate && declick <= 0) {
                drem = 0u;
                st   = ADRIFT_ENV_IDLE;
            } else {
                if (drem > 0u) {
                    drem--;
                }
                if (drem == 0u) {
                    st = gate ? ADRIFT_ENV_ATTACK : ADRIFT_ENV_IDLE;
                }
            }
        } else {
            lvl = 0;
        }

        int32_t g_q15 = ((lvl >> 15) * vel_q15) >> 15;
        if (declick > 0) {
            g_q15 += (declick >> 15);
            if (g_q15 > 32767) g_q15 = 32767;
            declick -= declick_step;
            if (declick < 0) declick = 0;
        }
        out_gain_q15[i] = g_q15;
    }

    env->stage            = st;
    env->level_q30        = lvl;
    env->declick_q30      = declick;
    env->declick_step_q30 = declick_step;
    env->delay_rem        = drem;
}

void adrift_env_process(adrift_env_t *env,
                        const adrift_env_params_t *p,
                        int32_t *out_gain_q15,
                        uint32_t frames) {
    env->just_triggered = false;
    if (p->drone_hold) {
        env->level_q30   = ENV_ONE_Q30;
        env->declick_q30 = 0;
        for (uint32_t i = 0; i < frames; i++) {
            out_gain_q15[i] = 32767;
        }
        return;
    }

    adrift_env_stage_t st = env->stage;
    if (st == ADRIFT_ENV_IDLE) {
        env->level_q30   = 0;
        env->declick_q30 = 0;
        for (uint32_t i = 0; i < frames; i++) {
            out_gain_q15[i] = 0;
        }
        return;
    }

    uint32_t drem = env->delay_rem;
    if (st == ADRIFT_ENV_DELAY && drem > frames && env->declick_q30 <= 0) {
        env->level_q30 = 0;
        env->delay_rem = drem - frames;
        for (uint32_t i = 0; i < frames; i++) {
            out_gain_q15[i] = 0;
        }
        return;
    }

    uint8_t loop = p->loop_mode;
    if (st == ADRIFT_ENV_SUSTAIN && env->gate && loop == ADRIFT_ENV_LOOP_OFF && env->declick_q30 <= 0) {
        int32_t sus     = env->sus_level_q30;
        env->level_q30  = sus;
        int32_t const_g = ((sus >> 15) * env->vel_scale_q15) >> 15;
        for (uint32_t i = 0; i < frames; i++) {
            out_gain_q15[i] = const_g;
        }
        return;
    }

    DSP_CALL_RAM(env_process_stages_ram)(env, loop, out_gain_q15, frames);
}
