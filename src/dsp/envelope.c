#include "envelope.h"

#define ENV_ONE_Q30  (1 << 30)
#define ENV_OVER_Q30 1288490188 /* 1.20 * 2^30: analog RC attack overshoot target */

void adrift_env_init(adrift_env_t *env) {
    adrift_env_reset(env);
}

void adrift_env_reset(adrift_env_t *env) {
    env->stage          = ADRIFT_ENV_IDLE;
    env->gate           = false;
    env->just_triggered = false;
    env->level_q30      = 0;
    env->vel_scale_q15  = 32767;
    env->delay_rem      = 0u;
    env->delay_total    = 0u;
    env->atk_coef_q14   = 4096 << 4;
    env->atk_floor_q30  = 1 << 20;
    env->dec_coef_q14   = 512 << 4;
    env->dec_floor_q30  = 1 << 16;
    env->sus_level_q30  = ENV_ONE_Q30 >> 1;
    env->rel_coef_q14   = 512 << 4;
    env->rel_floor_q30  = 1 << 16;
    env->coefs_valid    = false;
}

/* Maps knob 0..1023 -> sample count [min_samples .. max_samples] with cubic musical taper */
static uint32_t knob_to_samples(int16_t knob_1023, uint32_t min_s, uint32_t max_s) {
    uint32_t k  = (uint32_t)dsp_clamp_i32(knob_1023, 0, 1023);
    uint32_t k2 = (k * k) >> 10;          /* 0..1022 */
    uint32_t k3 = (k2 * k) >> 10;         /* 0..1021 (cubic curve gives fine resolution at 0.5-100 ms) */
    uint32_t span = max_s - min_s;
    return min_s + dsp_umul_q16(span, k3 << 6);
}

static void samples_to_rc_rate(uint32_t samples, int32_t *out_coef_q14, int32_t *out_floor_q30) {
    if (samples < 8u) samples = 8u;
    /* Time constant tau ~= samples / 3 so RC curve settles cleanly within `samples` */
    uint32_t tau = samples / 3u;
    if (tau < 1u) tau = 1u;
    uint32_t c = (tau <= 16384u) ? (262144u / tau) : 0u;
    if (c > 196608u) c = 196608u;
    *out_coef_q14 = (int32_t)c;

    /* Bias current floor guarantees exact termination within `samples` even when c == 0 */
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

    /* Stage time bounds @ 44.118 kHz:
     *   NORM  (0): Attack 0.5 ms (22) .. 5 s (220,590), Decay/Release 1 ms (44) .. 10 s (441,180)
     *   SHORT (1): Attack/Decay/Release ~0.5 ms (16..22) .. ~250 ms per stage
     *   LONG  (2): Attack/Decay/Release ~1 ms (44) .. 25 s (1,102,950) per stage
     */
    uint32_t atk_min = 22u, atk_max = 220590u;
    uint32_t dec_min = 44u, dec_max = 441180u;
    if (p->range == ADRIFT_ENV_RANGE_SHORT) {
        atk_min = 16u;
        atk_max = 30940u;
        dec_min = 22u;
        dec_max = 30940u;
    } else if (p->range == ADRIFT_ENV_RANGE_LONG) {
        atk_min = 44u;
        atk_max = 1102950u;
        dec_min = 44u;
        dec_max = 1102950u;
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
                env->delay_rem = env->delay_total;
                env->stage     = ADRIFT_ENV_DELAY;
                env->level_q30 = 0;
            } else if (env->stage == ADRIFT_ENV_DELAY) {
                env->delay_rem = 0u;
                env->stage     = ADRIFT_ENV_ATTACK;
            }
        } else if (!full && env->stage == ADRIFT_ENV_DELAY && prev_total != env->delay_total) {
            uint32_t elapsed = (prev_total > env->delay_rem) ? (prev_total - env->delay_rem) : 0u;
            env->delay_rem   = (env->delay_total > elapsed) ? (env->delay_total - elapsed) : 0u;
        }
    }
    env->just_triggered = false;

    if (full || env->last_attack != p->attack) {
        uint32_t atk_s = knob_to_samples(p->attack, atk_min, atk_max);
        samples_to_rc_rate(atk_s, &env->atk_coef_q14, &env->atk_floor_q30);
        env->last_attack = p->attack;
    }

    if (full || env->last_decay != p->decay) {
        uint32_t dec_s = knob_to_samples(p->decay, dec_min, dec_max);
        samples_to_rc_rate(dec_s, &env->dec_coef_q14, &env->dec_floor_q30);
        env->last_decay = p->decay;
    }

    if (full || env->last_sustain != p->sustain) {
        /* Sustain: 0..1023 -> 0 .. ENV_ONE_Q30 */
        uint32_t sus = (uint32_t)dsp_clamp_i32(p->sustain, 0, 1023);
        env->sus_level_q30 = (sus >= 1023u) ? ENV_ONE_Q30 : (int32_t)(sus << 20);
        env->last_sustain  = p->sustain;
    }

    if (full || env->last_release != p->release) {
        uint32_t rel_s = knob_to_samples(p->release, dec_min, dec_max);
        samples_to_rc_rate(rel_s, &env->rel_coef_q14, &env->rel_floor_q30);
        env->last_release = p->release;
    }

    env->last_range  = p->range;
    env->coefs_valid = true;
}

void adrift_env_note_on(adrift_env_t *env, const adrift_env_params_t *p, uint8_t velocity, bool legato) {
    adrift_env_update_coefs(env, p);

    int32_t vsens = dsp_clamp_i32(p->vel_sens, 0, 1023);
    int32_t v_q15 = (int32_t)dsp_udiv_127((uint32_t)(velocity & 0x7Fu) * 32767u);
    /* Interpolate between 32767 (vsens=0) and v_q15 (vsens=1023) */
    env->vel_scale_q15 = 32767 - (((32767 - v_q15) * vsens) >> 10);

    env->gate = true;
    if (legato && env->stage != ADRIFT_ENV_IDLE && env->stage != ADRIFT_ENV_RELEASE) {
        return;
    }

    env->just_triggered = true;
    if (env->delay_total > 0u) {
        env->delay_rem = env->delay_total;
        env->stage     = ADRIFT_ENV_DELAY;
        /* Reset to 0 during onset delay so delayed strikes emerge cleanly after silence */
        env->level_q30 = 0;
    } else {
        env->stage = ADRIFT_ENV_ATTACK;
    }
}

void adrift_env_note_off(adrift_env_t *env, const adrift_env_params_t *p) {
    (void)p;
    env->gate           = false;
    env->just_triggered = false;
    if (env->stage != ADRIFT_ENV_IDLE) {
        if (env->stage == ADRIFT_ENV_DELAY || env->level_q30 <= 0) {
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
    int32_t  sus        = env->sus_level_q30;
    int32_t  lvl        = env->level_q30;
    uint32_t drem       = env->delay_rem;
    uint32_t dtotal     = env->delay_total;
    int32_t  atk_coef   = env->atk_coef_q14;
    int32_t  atk_floor  = env->atk_floor_q30;
    int32_t  dec_coef   = env->dec_coef_q14;
    int32_t  dec_floor  = env->dec_floor_q30;
    int32_t  rel_coef   = env->rel_coef_q14;
    int32_t  rel_floor  = env->rel_floor_q30;
    int32_t  dec_target = (loop != ADRIFT_ENV_LOOP_OFF && sus > (ENV_ONE_Q30 - (1 << 24)))
                              ? 0 /* If sustain is ~100% and loop enabled, still cycle to 0 */
                              : sus;

    for (uint32_t i = 0; i < frames; i++) {
        if (st == ADRIFT_ENV_DECAY) {
            if (lvl > dec_target) {
                int32_t diff_hi = (lvl - dec_target) >> 18;
                int32_t step    = (diff_hi * dec_coef) + dec_floor;
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
                        drem = dtotal;
                        lvl  = 0;
                        st   = ADRIFT_ENV_DELAY;
                    } else {
                        st = ADRIFT_ENV_ATTACK;
                    }
                } else {
                    st = (lvl <= 0 && !gate) ? ADRIFT_ENV_IDLE : ADRIFT_ENV_SUSTAIN;
                }
            }
        } else if (st == ADRIFT_ENV_RELEASE) {
            int32_t diff_hi = lvl >> 18;
            int32_t step    = (diff_hi * rel_coef) + rel_floor;
            lvl -= step;
            if (lvl <= 0) {
                lvl = 0;
                st  = ADRIFT_ENV_IDLE;
            }
        } else if (st == ADRIFT_ENV_ATTACK) {
            int32_t diff_hi = (ENV_OVER_Q30 - lvl) >> 18;
            int32_t step    = (diff_hi * atk_coef) + atk_floor;
            lvl += step;
            if (lvl >= ENV_ONE_Q30) {
                lvl = ENV_ONE_Q30;
                st  = ADRIFT_ENV_DECAY;
            }
        } else if (st == ADRIFT_ENV_SUSTAIN) {
            lvl = sus;
            if (!gate) {
                st = (lvl <= 0) ? ADRIFT_ENV_IDLE : ADRIFT_ENV_RELEASE;
            } else if (loop != ADRIFT_ENV_LOOP_OFF) {
                st = ADRIFT_ENV_ATTACK;
            }
        } else if (st == ADRIFT_ENV_DELAY) {
            lvl = 0;
            if (drem > 0u) {
                drem--;
            }
            if (drem == 0u) {
                st = ADRIFT_ENV_ATTACK;
            }
        } else {
            lvl = 0;
        }

        int32_t g_q15 = lvl >> 15;
        out_gain_q15[i] = (g_q15 * vel_q15) >> 15;
    }

    env->stage     = st;
    env->level_q30 = lvl;
    env->delay_rem = drem;
}

void adrift_env_process(adrift_env_t *env,
                        const adrift_env_params_t *p,
                        int32_t *out_gain_q15,
                        uint32_t frames) {
    env->just_triggered = false;
    if (p->drone_hold) {
        env->level_q30 = ENV_ONE_Q30;
        for (uint32_t i = 0; i < frames; i++) {
            out_gain_q15[i] = 32767;
        }
        return;
    }

    adrift_env_stage_t st = env->stage;
    if (st == ADRIFT_ENV_IDLE) {
        env->level_q30 = 0;
        for (uint32_t i = 0; i < frames; i++) {
            out_gain_q15[i] = 0;
        }
        return;
    }

    uint32_t drem = env->delay_rem;
    if (st == ADRIFT_ENV_DELAY && drem > frames) {
        env->level_q30 = 0;
        env->delay_rem = drem - frames;
        for (uint32_t i = 0; i < frames; i++) {
            out_gain_q15[i] = 0;
        }
        return;
    }

    uint8_t loop = p->loop_mode;
    if (st == ADRIFT_ENV_SUSTAIN && env->gate && loop == ADRIFT_ENV_LOOP_OFF) {
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
