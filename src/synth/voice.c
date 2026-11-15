#include "synth_internal.h"

/* Constant-power unison gain scaling (Q15) for 1..16 stacked voices in a group */
const int32_t k_unison_gain_q15[ADRIFT_MAX_VOICES + 1] = {
    32767, 32767, 24576, 20480, 18022, 16384, 15073, 14045,
    13210, 12510, 11916, 11400, 10948, 10547, 10188,  9864, 9570
};

/* Asymmetric, center-weighted Demon Core detune ratios in Q10 (-1024..+1024).
 * Even slots (0,2,4..) feed Left/CPU0 and odd slots (1,3,5..) feed Right/CPU1;
 * both the 6 fundamental (0 ST) pairs and the 2 octave (-12/+12 ST) pairs alternate
 * polarity (-/+, +/-) with distinct magnitudes so Left and Right stay centered at 0.0c
 * while no two oscillator pairs ever beat at the same rate. */
static const int16_t k_stack_detune_q10[ADRIFT_MAX_VOICES] = {
    -185,  +195,  +510,  -490,  +415,  -400,  -740,  +765,
    -645,  +670,  +310,  -325,  -855,  +830,  +995, -1024
};

void adrift_synth_reset_voice_dsp(adrift_voice_t *vc) {
    vc->active                 = false;
    vc->key_down               = false;
    vc->sustained              = false;
    vc->pan_cache_valid        = false;
    vc->age                    = 0u;
    vc->exc.ac_cap_q23         = 0;
    vc->exc.hf_slew_q15        = 0;
    vc->exc.vco_gain_q15       = 0;
    vc->exc.noise_gain_q15     = 0;
    vc->exc.impulse_phase      = 0u;
    vc->exc.impulse_amp_q15    = 0;
    vc->exc.pending_phase_lock = 0u;
    adrift_env_reset(&vc->env1);
    adrift_env_reset(&vc->env2);
    adrift_svf_reset(&vc->in_lpf);
    adrift_bbd_reset(&vc->bbd);
}

void adrift_synth_recompute_group_slots(adrift_synth_t *s) {
    uint8_t slot_cnt[ADRIFT_MAX_GROUPS] = { 0 };
    for (uint8_t v = 0; v < ADRIFT_MAX_VOICES; v++) {
        int8_t og = s->voices[v].owner_group;
        if (og >= 0 && og < ADRIFT_MAX_GROUPS) {
            s->voices[v].group_slot = slot_cnt[og]++;
        } else {
            s->voices[v].group_slot = 0u;
        }
        s->voices[v].pan_cache_valid = false;
    }
}

uint8_t adrift_synth_used_voices(const adrift_synth_t *s) {
    uint8_t used = 0u;
    for (uint8_t g = 0; g < ADRIFT_MAX_GROUPS; g++) {
        used = (uint8_t)(used + s->groups[g].num_voices);
    }
    if (used > ADRIFT_MAX_VOICES) used = ADRIFT_MAX_VOICES;
    return used;
}

uint8_t adrift_synth_free_voices(const adrift_synth_t *s) {
    uint8_t used = adrift_synth_used_voices(s);
    return (used >= ADRIFT_MAX_VOICES) ? 0u : (uint8_t)(ADRIFT_MAX_VOICES - used);
}

void adrift_synth_refresh_any_gate(adrift_synth_t *s) {
    bool g = false;
    for (uint8_t v = 0; v < ADRIFT_MAX_VOICES; v++) {
        if (s->voices[v].owner_group >= 0 &&
            (s->voices[v].key_down || s->voices[v].sustained)) {
            g = true;
            break;
        }
    }
    s->any_gate = g;
}

void adrift_synth_wake_group_drone(adrift_synth_t *s, uint8_t g) {
    if (g >= ADRIFT_MAX_GROUPS) return;
    const adrift_group_t *gp = &s->groups[g];
    if (gp->num_voices == 0u || !gp->patch.env2.drone_hold) {
        return;
    }

    bool any_active = false;
    for (uint8_t v = 0; v < ADRIFT_MAX_VOICES; v++) {
        if (s->voices[v].owner_group == (int8_t)g && s->voices[v].active) {
            any_active = true;
            if (!adrift_group_is_stacked(gp->assign_mode)) {
                return;
            }
        }
    }

    for (uint8_t v = 0; v < ADRIFT_MAX_VOICES; v++) {
        if (s->voices[v].owner_group == (int8_t)g) {
            s->voices[v].active = true;
            if (!adrift_group_is_stacked(gp->assign_mode) && !any_active) {
                break; /* Hands-free single initial drone voice in Round-Robin */
            }
        }
    }
}

bool adrift_synth_set_group_voices(adrift_synth_t *s, uint8_t g, uint8_t desired_voices) {
    if (g >= ADRIFT_MAX_GROUPS) return false;
    adrift_group_t *gp = &s->groups[g];

    /* Strictly cap to (current group voices + remaining free voices in the 6-voice pool) */
    uint8_t max_allowed = (uint8_t)(gp->num_voices + adrift_synth_free_voices(s));
    if (max_allowed > ADRIFT_MAX_VOICES) max_allowed = ADRIFT_MAX_VOICES;
    uint8_t target = (desired_voices > max_allowed) ? max_allowed : desired_voices;
    if (target == gp->num_voices) {
        return false;
    }

    int32_t init_grp_g = (int32_t)dsp_udiv_1023((uint32_t)dsp_clamp_i32(gp->patch.master_vol, 0, 1023) * 32767u);

    /* Claim unassigned voices from the free pool */
    while (gp->num_voices < target) {
        int found = -1;
        for (uint8_t v = 0; v < ADRIFT_MAX_VOICES; v++) {
            if (s->voices[v].owner_group < 0) {
                found = (int)v;
                break;
            }
        }
        if (found < 0) break;
        adrift_voice_t *vc = &s->voices[found];
        adrift_synth_reset_voice_dsp(vc);
        vc->owner_group    = (int8_t)g;
        vc->note           = gp->last_note ? gp->last_note : 60u;
        vc->grp_gain_l_q15 = init_grp_g;
        vc->grp_gain_r_q15 = init_grp_g;
        gp->num_voices++;
    }

    /* Release excess voices from this group back into the free pool (prefer idle -> releasing -> oldest) */
    while (gp->num_voices > target) {
        int victim = -1;
        for (uint8_t v = 0; v < ADRIFT_MAX_VOICES; v++) {
            if (s->voices[v].owner_group == (int8_t)g &&
                (!s->voices[v].active || s->voices[v].env2.stage == ADRIFT_ENV_IDLE)) {
                victim = (int)v;
                break;
            }
        }
        if (victim < 0) {
            uint32_t best_age = 0xFFFFFFFFu;
            for (uint8_t v = 0; v < ADRIFT_MAX_VOICES; v++) {
                if (s->voices[v].owner_group == (int8_t)g &&
                    !s->voices[v].key_down && !s->voices[v].sustained &&
                    s->voices[v].age < best_age) {
                    best_age = s->voices[v].age;
                    victim   = (int)v;
                }
            }
        }
        if (victim < 0) {
            uint32_t best_age = 0xFFFFFFFFu;
            for (uint8_t v = 0; v < ADRIFT_MAX_VOICES; v++) {
                if (s->voices[v].owner_group == (int8_t)g && s->voices[v].age <= best_age) {
                    best_age = s->voices[v].age;
                    victim   = (int)v;
                }
            }
        }
        if (victim < 0) break;
        adrift_synth_reset_voice_dsp(&s->voices[victim]);
        s->voices[victim].owner_group = -1;
        gp->num_voices--;
    }

    if (gp->num_voices == 0u) {
        gp->mono_len  = 0u;
        gp->rr_cursor = 0u;
    } else if (gp->rr_cursor >= gp->num_voices) {
        gp->rr_cursor = 0u;
    }

    adrift_synth_recompute_group_slots(s);
    adrift_synth_wake_group_drone(s, g);
    adrift_synth_refresh_any_gate(s);
    return true;
}

void adrift_synth_cycle_edit_group(adrift_synth_t *s) {
    s->edit_group = (uint8_t)((s->edit_group + 1u) % ADRIFT_MAX_GROUPS);
}

void adrift_synth_compute_stack_slot_voicing(const adrift_group_t *gp,
                                             uint8_t slot,
                                             int32_t spread_q8,
                                             int32_t *out_semi_q8,
                                             int32_t *out_detune_q8,
                                             int32_t *out_slot_gain_q15) {
    int32_t semi_q8       = 0;
    int32_t detune_q8     = 0;
    int32_t slot_gain_q15 = 32767;
    uint8_t nv            = gp->num_voices;

    if (nv > 1u && adrift_group_is_stacked(gp->assign_mode) && gp->unison_detune > 0) {
        int32_t ratio_q10 = 0;
        if (nv == 2u) {
            ratio_q10 = (slot == 0u) ? -980 : +1024;
        } else if (nv == 3u) {
            ratio_q10 = (slot == 0u) ? -980 : ((slot == 1u) ? +1024 : +180);
        } else if (nv == 4u) {
            static const int16_t k_det4[4] = { -340, +370, +1024, -960 };
            ratio_q10 = k_det4[slot & 3u];
        } else {
            ratio_q10 = k_stack_detune_q10[slot % ADRIFT_MAX_VOICES];
        }
        detune_q8 = (spread_q8 * ratio_q10) >> 10;

        /* Demon Core octave-enriched unison stacking across the voice stack (100% L/R symmetric):
         * - Slots 0, 1: 0 ST Core Fundamental L/R pair (1.00x gain)
         * - Slot 2 (for N>=4) and Slot 3 (for N>=6): -12 ST Sub-Octave foundation (tighter detune)
         * - Slot 5 (for N=6,7) or Slots 6, 7 (for N>=8): +12 ST Upper-Octave brilliance
         * - Slots 4, 5, 8..15 (for N>=12): 0 ST center-weighted Demon Core unison swarm
         *   (0.90x inner/mid swarm on slots 4..11, 0.82x wide stereo wings on slots 12..15) */
        if (nv >= 4u) {
            if (slot == 2u || (slot == 3u && nv >= 6u)) {
                semi_q8       = -(12 << 8);
                slot_gain_q15 = (nv >= 12u) ? 23593 : 26214; /* 0.72x for 12..16v swarm, 0.80x for <=11v */
                detune_q8     = (detune_q8 * ((nv >= 12u) ? 480 : 580)) >> 10;
            } else if ((slot == 5u && nv < 8u && nv >= 6u) ||
                       ((slot == 6u || slot == 7u) && nv >= 8u)) {
                semi_q8       = +(12 << 8);
                slot_gain_q15 = (nv >= 12u) ? 17695 : 21299; /* 0.54x for 12..16v swarm, 0.65x for <=11v */
                if (nv >= 12u) {
                    detune_q8 = (detune_q8 * 780) >> 10;
                }
            } else if (nv >= 12u && slot >= 12u) {
                slot_gain_q15 = 26869; /* 0.82x wide outer unison wings */
            } else if (slot >= 4u) {
                slot_gain_q15 = (nv >= 12u) ? 29491 : 28835; /* 0.90x / 0.88x unison swarm fundamentals */
            }
        }
    }

    *out_semi_q8       = semi_q8;
    *out_detune_q8     = detune_q8;
    *out_slot_gain_q15 = slot_gain_q15;
}

DSP_ALWAYS_INLINE int32_t voice_sdiv_1023(int32_t x) {
    if (x >= 0) {
        return (int32_t)dsp_udiv_1023((uint32_t)x);
    }
    return -(int32_t)dsp_udiv_1023((uint32_t)(-x));
}

/* Constant-power center-normalized stereo panning law (Q15: 32767 = unity at center, up to 46340 = sqrt(2) hard-panned) */
void adrift_synth_compute_voice_pan_gains_mod_q15(const adrift_group_t *gp,
                                                  uint8_t slot,
                                                  int32_t mod_pan,
                                                  int32_t mod_spread,
                                                  int32_t *out_pan_l_q15,
                                                  int32_t *out_pan_r_q15) {
    int32_t sp          = dsp_clamp_i32((int32_t)gp->stereo_spread + mod_spread, 0, 1023);
    int32_t grp_pan     = dsp_clamp_i32((int32_t)gp->pan + mod_pan, -512, 512);
    int32_t gp_num      = grp_pan * 1023;
    int32_t grp_pan_q10 = (gp_num + (gp_num < 0 ? 511 : 0)) >> 9; /* -1023 (hard L) .. 0 (center) .. +1023 (hard R) */
    uint8_t nv          = gp->num_voices;

    int32_t spread_q10 = 0; /* -1023 .. +1023 per-voice stereo spread offset around group pan center */
    if (sp > 0 && nv > 1u) {
        if (gp->assign_mode == ADRIFT_GROUP_SUPER) {
            /* Stereo BBD pair: slot 0 on Left, slot 1 on Right */
            spread_q10 = (slot == 0u) ? -sp : +sp;
        } else if (gp->assign_mode == ADRIFT_GROUP_UNISON) {
            if ((nv & 1u) != 0u && slot == (uint8_t)(nv - 1u)) {
                spread_q10 = 0; /* Unpaired odd voice anchored at group pan center */
            } else {
                uint8_t pair      = slot >> 1;
                uint8_t num_pairs = nv >> 1;
                int32_t width     = sp;
                if (num_pairs > 1u) {
                    if (pair == 0u) {
                        width = (sp * 620) >> 10; /* Core fundamental pair ~60% width */
                    } else if (pair == 1u && (slot == 2u || nv >= 6u)) {
                        width = (sp * 420) >> 10; /* Sub-octave pair anchored tighter (~41% width) */
                    } else {
                        width = ((sp * 720) >> 10) + (((sp * 304) >> 10) * (int32_t)(pair - 1u)) / (int32_t)(num_pairs - 1u);
                    }
                }
                spread_q10 = ((slot & 1u) == 0u) ? -width : +width;
            }
        } else {
            /* Round-Robin polyphony: paired slots alternate Left/Right symmetrically across the stereo stage,
             * with any unpaired odd slot anchored at the group pan center */
            if ((nv & 1u) != 0u && slot == (uint8_t)(nv - 1u)) {
                spread_q10 = 0;
            } else {
                uint8_t pair      = slot >> 1;
                uint8_t num_pairs = nv >> 1;
                int32_t width     = (sp * (int32_t)(pair + 1u)) / (int32_t)(num_pairs > 0u ? num_pairs : 1u);
                spread_q10        = ((slot & 1u) == 0u) ? -width : +width;
            }
        }
    }

    int32_t abs_grp_pan   = (grp_pan_q10 < 0) ? -grp_pan_q10 : grp_pan_q10;
    int32_t spread_scaled = voice_sdiv_1023(spread_q10 * (1023 - abs_grp_pan));
    if (gp->assign_mode == ADRIFT_GROUP_SUPER && abs_grp_pan > 0) {
        int32_t sn = spread_scaled * (2046 - abs_grp_pan);
        spread_scaled = (sn >= 0) ? (int32_t)dsp_udiv_1023((uint32_t)sn >> 1)
                                  : -(int32_t)dsp_udiv_1023((uint32_t)(-sn) >> 1);
    }
    int32_t pan_q10 = grp_pan_q10 + spread_scaled;
    if (pan_q10 == 0) {
        *out_pan_l_q15 = 32767;
        *out_pan_r_q15 = 32767;
        return;
    }
    if (pan_q10 <= -1023) {
        *out_pan_l_q15 = 46340;
        *out_pan_r_q15 = 0;
        return;
    }
    if (pan_q10 >= 1023) {
        *out_pan_l_q15 = 0;
        *out_pan_r_q15 = 46340;
        return;
    }

    uint32_t angle  = (uint32_t)((1024 + pan_q10) << 19); /* [0 .. 0x40000000] -> [0 .. pi/2] */
    int32_t cos_q15 = dsp_sine_lut_q15(0x40000000u - angle);
    int32_t sin_q15 = dsp_sine_lut_q15(angle);
    *out_pan_l_q15  = dsp_clamp_i32((cos_q15 * 32767) / 23170, 0, 46340);
    *out_pan_r_q15  = dsp_clamp_i32((sin_q15 * 32767) / 23170, 0, 46340);
}

void adrift_synth_get_voice_pan_gains_cached_q15(adrift_voice_t *vc,
                                                 const adrift_group_t *gp,
                                                 int32_t mod_pan,
                                                 int32_t mod_spread,
                                                 int32_t *out_pan_l_q15,
                                                 int32_t *out_pan_r_q15) {
    int16_t eff_sp  = (int16_t)dsp_clamp_i32((int32_t)gp->stereo_spread + mod_spread, 0, 1023);
    int16_t eff_pan = (int16_t)dsp_clamp_i32((int32_t)gp->pan + mod_pan, -512, 512);
    uint8_t mode    = gp->assign_mode;
    uint8_t nv      = gp->num_voices;
    uint8_t slot    = vc->group_slot;

    if (vc->pan_cache_valid &&
        vc->cached_pan_mode   == mode &&
        vc->cached_pan_nv     == nv &&
        vc->cached_pan_slot   == slot &&
        vc->cached_eff_pan    == eff_pan &&
        vc->cached_eff_spread == eff_sp) {
        *out_pan_l_q15 = vc->cached_pan_l_q15;
        *out_pan_r_q15 = vc->cached_pan_r_q15;
        return;
    }

    int32_t pl = 32767, pr = 32767;
    adrift_synth_compute_voice_pan_gains_mod_q15(gp, slot, mod_pan, mod_spread, &pl, &pr);
    vc->pan_cache_valid   = true;
    vc->cached_pan_mode   = mode;
    vc->cached_pan_nv     = nv;
    vc->cached_pan_slot   = slot;
    vc->cached_eff_pan    = eff_pan;
    vc->cached_eff_spread = eff_sp;
    vc->cached_pan_l_q15  = pl;
    vc->cached_pan_r_q15  = pr;
    *out_pan_l_q15        = pl;
    *out_pan_r_q15        = pr;
}

void adrift_synth_compute_voice_pan_gains_q15(const adrift_group_t *gp,
                                              uint8_t slot,
                                              int32_t *out_pan_l_q15,
                                              int32_t *out_pan_r_q15) {
    adrift_synth_compute_voice_pan_gains_mod_q15(gp, slot, 0, 0, out_pan_l_q15, out_pan_r_q15);
}

static uint32_t voice_steal_energy(const adrift_voice_t *vc, const adrift_patch_t *p) {
    uint32_t env2_q15 = p->env2.drone_hold
                            ? 32767u
                            : ((((uint32_t)(vc->env2.level_q30 >> 15) * (uint32_t)vc->env2.vel_scale_q15) >> 15) +
                               (uint32_t)(vc->env2.declick_q30 >> 15));
    if (env2_q15 > 32767u) env2_q15 = 32767u;
    uint32_t env1_q15 = p->env1.drone_hold
                            ? 32767u
                            : ((((uint32_t)(vc->env1.level_q30 >> 15) * (uint32_t)vc->env1.vel_scale_q15) >> 15) +
                               (uint32_t)(vc->env1.declick_q30 >> 15));
    if (env1_q15 > 32767u) env1_q15 = 32767u;
    uint32_t wet_q15 = (uint32_t)dsp_clamp_i32(vc->bbd.peak_wet_q15, 0, 32767);
    int32_t  dg_q15  = 0;
    int32_t  wg_q15  = 0;
    adrift_synth_dry_wet_gains_q15(p->dry_wet, &dg_q15, &wg_q15);
    uint32_t src_q15 = 64u + (((env1_q15 * (uint32_t)dg_q15) + (wet_q15 * (uint32_t)wg_q15)) >> 15);
    return (env2_q15 * src_q15) >> 15;
}

typedef struct {
    bool voice_audible;
    bool exc_audible;
} voice_aud_ref_t;

static voice_aud_ref_t capture_voice_aud_ref(const adrift_voice_t *vc, const adrift_patch_t *p) {
    voice_aud_ref_t r;
    r.voice_audible = vc->active && (vc->env2.stage != ADRIFT_ENV_IDLE || p->env2.drone_hold);
    r.exc_audible   = r.voice_audible &&
                      (vc->env1.level_q30 > (1 << 20) || vc->env1.declick_q30 > (1 << 20) || p->env1.drone_hold);
    return r;
}

static void trigger_voice_on_ex(adrift_synth_t *s,
                                uint8_t g,
                                uint8_t v,
                                uint8_t note,
                                uint8_t velocity,
                                bool legato,
                                const voice_aud_ref_t *aud_ref) {
    adrift_voice_t *vc = &s->voices[v];
    adrift_group_t *gp = &s->groups[g];
    voice_aud_ref_t ar = aud_ref ? *aud_ref : capture_voice_aud_ref(vc, &gp->patch);
    bool was_active    = vc->active;
    bool voice_audible = ar.voice_audible;
    bool exc_audible   = ar.exc_audible;
    uint8_t prev_note  = vc->note;

    /* Polyphonic portamento: when Round-Robin allocates an idle voice with glide > 2,
     * seed its portamento origin from the group's most recently triggered voice (if any). */
    if (!was_active && gp->patch.bbd.glide > 2 && !adrift_group_is_stacked(gp->assign_mode)) {
        uint32_t best_prev_age = 0u;
        int      best_prev_v   = -1;
        for (uint8_t u = 0; u < ADRIFT_MAX_VOICES; u++) {
            if (s->voices[u].owner_group == (int8_t)g &&
                s->voices[u].bbd.pitch_initialized && s->voices[u].age > best_prev_age) {
                best_prev_age = s->voices[u].age;
                best_prev_v   = (int)u;
            }
        }
        if (best_prev_v >= 0) {
            vc->bbd.slew_pitch_q16    = s->voices[best_prev_v].bbd.slew_pitch_q16;
            vc->bbd.pitch_initialized = true;
        }
    }

    if (!voice_audible) {
        vc->bbd.delay_initialized  = false;
        vc->bbd.steal_damp_pending = false;
        vc->exc.ac_cap_q23         = 0;
        vc->exc.hf_slew_q15        = 0;
        int32_t exc_lvl_q15        = (int32_t)dsp_udiv_1023((uint32_t)dsp_clamp_i32(gp->patch.exc.vco_level, 0, 1023) * 32767u);
        int32_t mix_1023           = dsp_clamp_i32(gp->patch.exc.noise_level, 0, 1023);
        vc->exc.vco_gain_q15       = (int32_t)dsp_udiv_1023((uint32_t)exc_lvl_q15 * (uint32_t)(1023 - mix_1023));
        vc->exc.noise_gain_q15     = (int32_t)dsp_udiv_1023((uint32_t)exc_lvl_q15 * (uint32_t)mix_1023);
        int32_t init_dw            = dsp_clamp_i32(gp->patch.dry_wet, 0, 1023);
        if (gp->assign_mode == ADRIFT_GROUP_SUPER && gp->num_voices > 1u &&
            gp->patch.exc.vco_level > 0 && gp->patch.exc.noise_level < 1023 && init_dw > 640) {
            init_dw = 640 + ((init_dw - 640) >> 2);
        }
        adrift_synth_dry_wet_gains_q15(init_dw, &vc->dry_gain_q15, &vc->wet_gain_q15);
    } else if (!legato && prev_note != note && gp->patch.bbd.glide <= 2 && !gp->patch.env2.drone_hold) {
        /* Stealing an active non-drone voice onto a new pitch without glide:
         * softly release the old BBD tap and start the new string loop cleanly without chirping. */
        vc->bbd.steal_damp_pending = true;
    }

    vc->active    = true;
    vc->key_down  = true;
    vc->sustained = false;
    vc->note      = note;
    vc->velocity  = velocity;
    vc->age       = ++s->voice_age_counter;
    gp->last_note = note;
    s->last_note  = note;
    s->any_gate   = true;

    if (!voice_audible && !legato && (gp->pan != 0 || gp->stereo_spread != 0)) {
        int32_t semi_q8 = 0, detune_q8 = 0, slot_gain_q15 = 32767;
        adrift_synth_compute_stack_slot_voicing(gp, vc->group_slot, adrift_detune_to_q8(gp->unison_detune),
                                                &semi_q8, &detune_q8, &slot_gain_q15);
        int32_t uni_scale = 32767;
        if (gp->assign_mode == ADRIFT_GROUP_UNISON) {
            uni_scale = dsp_mul_q15(k_unison_gain_q15[gp->num_voices], slot_gain_q15);
        } else if (gp->assign_mode == ADRIFT_GROUP_SUPER && gp->num_voices > 1u) {
            uni_scale = k_unison_gain_q15[2];
        }
        int32_t target_grp_q15 = (int32_t)dsp_udiv_1023((uint32_t)(dsp_clamp_i32(gp->patch.master_vol, 0, 1023) * uni_scale));
        int32_t pan_l_q15 = 32767, pan_r_q15 = 32767;
        adrift_synth_get_voice_pan_gains_cached_q15(vc, gp, 0, 0, &pan_l_q15, &pan_r_q15);
        vc->grp_gain_l_q15 = dsp_mul_q15(target_grp_q15, pan_l_q15);
        vc->grp_gain_r_q15 = dsp_mul_q15(target_grp_q15, pan_r_q15);
    }

    adrift_env_note_on(&vc->env1, &gp->patch.env1, velocity, legato);
    adrift_env_note_on(&vc->env2, &gp->patch.env2, velocity, legato);
    uint8_t uni_slot = (gp->assign_mode != ADRIFT_GROUP_ROUND_ROBIN && gp->unison_detune > 0)
                           ? vc->group_slot
                           : 0u;
    bool delayed_onset = (vc->env1.stage == ADRIFT_ENV_DELAY && vc->env1.delay_total > 0u);
    adrift_exciter_trigger_ex(&vc->exc, &gp->patch.exc, velocity, legato, uni_slot, exc_audible, delayed_onset);
}

static void trigger_voice_on(adrift_synth_t *s, uint8_t g, uint8_t v, uint8_t note, uint8_t velocity, bool legato) {
    trigger_voice_on_ex(s, g, v, note, velocity, legato, (const voice_aud_ref_t *)0);
}

static void trigger_voice_off(adrift_synth_t *s, uint8_t g, uint8_t v) {
    adrift_voice_t *vc = &s->voices[v];
    const adrift_group_t *gp = &s->groups[g];

    vc->key_down = false;
    if (gp->sustain_pedal && gp->patch.arp.mode == ADRIFT_ARP_OFF) {
        vc->sustained = true;
        return;
    }
    vc->sustained = false;
    adrift_env_note_off(&vc->env1, &gp->patch.env1);
    adrift_env_note_off(&vc->env2, &gp->patch.env2);
}

static void push_group_mono_stack(adrift_group_t *gp, uint8_t note, uint8_t velocity) {
    for (uint8_t i = 0; i < gp->mono_len; i++) {
        if (gp->mono_notes[i] == note) {
            for (uint8_t j = i; j + 1u < gp->mono_len; j++) {
                gp->mono_notes[j] = gp->mono_notes[j + 1u];
                gp->mono_vels[j]  = gp->mono_vels[j + 1u];
            }
            gp->mono_len--;
            break;
        }
    }
    if (gp->mono_len < 16u) {
        gp->mono_notes[gp->mono_len] = note;
        gp->mono_vels[gp->mono_len]  = velocity;
        gp->mono_len++;
    } else {
        for (uint8_t i = 0; i < 15u; i++) {
            gp->mono_notes[i] = gp->mono_notes[i + 1u];
            gp->mono_vels[i]  = gp->mono_vels[i + 1u];
        }
        gp->mono_notes[15] = note;
        gp->mono_vels[15]  = velocity;
    }
}

static void remove_group_mono_stack(adrift_group_t *gp, uint8_t note) {
    for (uint8_t i = 0; i < gp->mono_len; i++) {
        if (gp->mono_notes[i] == note) {
            for (uint8_t j = i; j + 1u < gp->mono_len; j++) {
                gp->mono_notes[j] = gp->mono_notes[j + 1u];
                gp->mono_vels[j]  = gp->mono_vels[j + 1u];
            }
            gp->mono_len--;
            break;
        }
    }
}

void adrift_synth_voice_note_on(adrift_synth_t *s, uint8_t g, uint8_t note, uint8_t velocity) {
    if (g >= ADRIFT_MAX_GROUPS) return;
    note &= 0x7Fu;
    velocity &= 0x7Fu;
    if (velocity == 0u) {
        adrift_synth_voice_note_off(s, g, note);
        return;
    }

    adrift_group_t *gp = &s->groups[g];
    if (gp->num_voices == 0u) {
        return; /* Disabled group */
    }

    uint8_t v_list[ADRIFT_MAX_VOICES];
    uint8_t n_owned     = 0u;
    bool    any_audible = false;
    for (uint8_t v = 0; v < ADRIFT_MAX_VOICES; v++) {
        if (s->voices[v].owner_group == (int8_t)g) {
            v_list[n_owned++] = v;
            if (s->voices[v].active &&
                (s->voices[v].env2.stage != ADRIFT_ENV_IDLE || gp->patch.env2.drone_hold)) {
                any_audible = true;
            }
        }
    }
    if (n_owned == 0u) return;
    if (!any_audible) {
        gp->smooth_pb_q20 = (int32_t)gp->pitch_bend_q13 * 128;
        gp->smooth_mw_q15 = (int32_t)dsp_udiv_127((uint32_t)gp->mod_wheel * 32767u);
        gp->smooth_at_q15 = (int32_t)dsp_udiv_127((uint32_t)gp->aftertouch * 32767u);
    }

    /* Unison / Super mode (stacks all voices in the group) or 1-voice Mono group */
    if (adrift_group_is_stacked(gp->assign_mode) || n_owned == 1u) {
        bool was_held = (gp->mono_len > 0u);
        /* Capture only the 2-byte pre-trigger audibility flags of BBD carrier slots 0 & 1
         * (never copy ~4.6 KB adrift_voice_t structs onto the 7.75 KB audio ISR stack). */
        voice_aud_ref_t super_ref[2] = { { false, false }, { false, false } };
        if (gp->assign_mode == ADRIFT_GROUP_SUPER && n_owned > 2u) {
            super_ref[0] = capture_voice_aud_ref(&s->voices[v_list[0]], &gp->patch);
            super_ref[1] = capture_voice_aud_ref(&s->voices[v_list[1]], &gp->patch);
        }
        push_group_mono_stack(gp, note, velocity);
        for (uint8_t k = 0; k < n_owned; k++) {
            uint8_t v = v_list[k];
            bool v_was_active = was_held || s->voices[v].sustained ||
                                (gp->patch.env2.drone_hold && s->voices[v].age > 0u);
            const voice_aud_ref_t *ref = (gp->assign_mode == ADRIFT_GROUP_SUPER && n_owned > 2u && k >= 2u)
                                             ? &super_ref[k & 1u]
                                             : (const voice_aud_ref_t *)0;
            trigger_voice_on_ex(s, g, v, note, velocity, gp->patch.legato && v_was_active, ref);
        }
        return;
    }

    /* Round-Robin polyphonic allocation strictly within this group's `n_owned` voices:
     * 1) Reuse voice in group already playing same note (unless droning)
     * 2) Next idle voice in group from `rr_cursor`
     * 3) Steal quietest releasing voice in group (tie-breaking by oldest age)
     * 4) Steal oldest held voice in group
     * 5) Drone Hold fallback within group */
    int chosen_k = -1;
    bool drone   = gp->patch.env2.drone_hold;

    if (!drone) {
        for (uint8_t k = 0; k < n_owned; k++) {
            uint8_t v = v_list[k];
            if (s->voices[v].active && s->voices[v].note == note) {
                chosen_k = (int)k;
                break;
            }
        }
        if (chosen_k < 0) {
            for (uint8_t step = 0; step < n_owned; step++) {
                uint8_t k = (uint8_t)((gp->rr_cursor + step) % n_owned);
                uint8_t v = v_list[k];
                if (!s->voices[v].active || s->voices[v].env2.stage == ADRIFT_ENV_IDLE) {
                    chosen_k      = (int)k;
                    gp->rr_cursor = (uint8_t)((k + 1u) % n_owned);
                    break;
                }
            }
        }
        if (chosen_k < 0) {
            uint32_t best_energy = 0xFFFFFFFFu;
            uint32_t best_age    = 0xFFFFFFFFu;
            for (uint8_t k = 0; k < n_owned; k++) {
                uint8_t v = v_list[k];
                if (!s->voices[v].key_down && !s->voices[v].sustained) {
                    uint32_t e = voice_steal_energy(&s->voices[v], &gp->patch);
                    if (e < best_energy || (e == best_energy && s->voices[v].age < best_age)) {
                        best_energy = e;
                        best_age    = s->voices[v].age;
                        chosen_k    = (int)k;
                    }
                }
            }
        }
        if (chosen_k < 0) {
            uint32_t best_age = 0xFFFFFFFFu;
            for (uint8_t k = 0; k < n_owned; k++) {
                uint8_t v = v_list[k];
                if (s->voices[v].age < best_age) {
                    best_age = s->voices[v].age;
                    chosen_k = (int)k;
                }
            }
        }
    } else {
        for (uint8_t k = 0; k < n_owned; k++) {
            uint8_t v = v_list[k];
            if (s->voices[v].active && s->voices[v].age > 0u && s->voices[v].note == note) {
                chosen_k = (int)k;
                break;
            }
        }
        if (chosen_k < 0) {
            for (uint8_t k = 0; k < n_owned; k++) {
                uint8_t v = v_list[k];
                if (s->voices[v].age == 0u || !s->voices[v].active) {
                    chosen_k      = (int)k;
                    gp->rr_cursor = (uint8_t)((k + 1u) % n_owned);
                    break;
                }
            }
        }
        if (chosen_k < 0) {
            uint32_t best_energy = 0xFFFFFFFFu;
            uint32_t best_age    = 0xFFFFFFFFu;
            for (uint8_t k = 0; k < n_owned; k++) {
                uint8_t v = v_list[k];
                if (!s->voices[v].key_down && !s->voices[v].sustained) {
                    uint32_t e = voice_steal_energy(&s->voices[v], &gp->patch);
                    if (e < best_energy || (e == best_energy && s->voices[v].age < best_age)) {
                        best_energy = e;
                        best_age    = s->voices[v].age;
                        chosen_k    = (int)k;
                    }
                }
            }
        }
        if (chosen_k < 0) {
            uint32_t best_age = 0xFFFFFFFFu;
            for (uint8_t k = 0; k < n_owned; k++) {
                uint8_t v = v_list[k];
                if (s->voices[v].age < best_age) {
                    best_age = s->voices[v].age;
                    chosen_k = (int)k;
                }
            }
        }
    }

    uint8_t cv = v_list[chosen_k >= 0 ? chosen_k : 0];
    bool was_active = s->voices[cv].active && (s->voices[cv].age > 0u);
    trigger_voice_on(s, g, cv, note, velocity, gp->patch.legato && was_active);
}

void adrift_synth_voice_note_off(adrift_synth_t *s, uint8_t g, uint8_t note) {
    if (g >= ADRIFT_MAX_GROUPS) return;
    note &= 0x7Fu;
    adrift_group_t *gp = &s->groups[g];
    if (gp->num_voices == 0u) return;

    if (adrift_group_is_stacked(gp->assign_mode) || gp->num_voices == 1u) {
        remove_group_mono_stack(gp, note);
        if (gp->mono_len > 0u) {
            uint8_t prev_note = gp->mono_notes[gp->mono_len - 1u];
            uint8_t prev_vel  = gp->mono_vels[gp->mono_len - 1u];
            for (uint8_t v = 0; v < ADRIFT_MAX_VOICES; v++) {
                if (s->voices[v].owner_group == (int8_t)g && s->voices[v].note != prev_note) {
                    trigger_voice_on(s, g, v, prev_note, prev_vel, gp->patch.legato);
                }
            }
        } else {
            for (uint8_t v = 0; v < ADRIFT_MAX_VOICES; v++) {
                if (s->voices[v].owner_group == (int8_t)g &&
                    (s->voices[v].note == note || s->voices[v].key_down)) {
                    trigger_voice_off(s, g, v);
                }
            }
        }
    } else {
        for (uint8_t v = 0; v < ADRIFT_MAX_VOICES; v++) {
            if (s->voices[v].owner_group == (int8_t)g &&
                s->voices[v].active && s->voices[v].key_down && s->voices[v].note == note) {
                trigger_voice_off(s, g, v);
            }
        }
    }
    adrift_synth_refresh_any_gate(s);
}

void adrift_synth_note_on(adrift_synth_t *s, uint8_t note, uint8_t velocity) {
    adrift_synth_group_note_on(s, s->edit_group % ADRIFT_MAX_GROUPS, note, velocity);
}

void adrift_synth_note_off(adrift_synth_t *s, uint8_t note) {
    for (uint8_t g = 0; g < ADRIFT_MAX_GROUPS; g++) {
        adrift_synth_group_note_off(s, g, note);
    }
}

void adrift_synth_group_all_notes_off(adrift_synth_t *s, uint8_t g, bool clear_bbd) {
    if (g >= ADRIFT_MAX_GROUPS) return;
    adrift_group_t *gp = &s->groups[g];
    gp->mono_len      = 0u;
    gp->sustain_pedal = false;
    adrift_arp_reset_state(&gp->arp_state, g);
    if (clear_bbd) {
        gp->rr_cursor = 0u;
    }
    for (uint8_t v = 0; v < ADRIFT_MAX_VOICES; v++) {
        adrift_voice_t *vc = &s->voices[v];
        if (vc->owner_group != (int8_t)g) continue;
        vc->key_down  = false;
        vc->sustained = false;
        if (clear_bbd) {
            adrift_synth_reset_voice_dsp(vc);
        } else {
            adrift_env_note_off(&vc->env1, &gp->patch.env1);
            adrift_env_note_off(&vc->env2, &gp->patch.env2);
        }
    }
    adrift_synth_refresh_any_gate(s);
}

void adrift_synth_all_notes_off(adrift_synth_t *s, bool clear_bbd) {
    if (clear_bbd) {
        s->voice_age_counter = 0u;
        adrift_fx_reset(&s->fx);
    }
    for (uint8_t g = 0; g < ADRIFT_MAX_GROUPS; g++) {
        adrift_synth_group_all_notes_off(s, g, clear_bbd);
    }
}
