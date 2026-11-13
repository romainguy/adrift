#include "ui_internal.h"

const char *const k_arp_mode_names[ADRIFT_ARP_MODE_COUNT] = {
    "OFF", "UP", "DOWN", "UP/DN", "DN/UP", "UP&DN",
    "CONVRG", "DIVERG", "RANDOM", "ORDER", "CHORD"
};

const char *const k_arp_ratchet_names[ADRIFT_RATCHET_COUNT] = {
    "1X", "2X", "3X", "4X", "1-2X", "1-3X", "1-4X", "RND"
};

const char *const k_mod_dest_names[ADRIFT_MOD_DEST_COUNT] = {
    "VCO PT", "MORPH",  "PWM",    "IN LPF", "BBD PT", "FB LPF", "FB AMT",
    "ALL PT", "PAN",    "SPREAD", "DRYWET", "VCA",    "EX MIX", "EX LVL",
    "SH CLK", "IN RES", "DISPR",  "E1 ATK", "E1 DEC", "E1 SUS", "E1 REL",
    "E1 DLY", "L1 RT"
};

static void apply_patch_encoder(adrift_patch_t *p, uint8_t page, uint8_t enc, int32_t delta) {
    if (delta == 0) return;
    int32_t step = delta * 12; /* ~85 clicks full scale, responsive yet precise */

    switch (page) {
        case 0: /* EXCITER (3 subpages: 1/3 Wave/Tune, 2/3 Fine/Noise/Pluck, 3/3 Drift/Keytrack) */
            switch (enc) {
                case 0: p->exc.wave_morph   = (int16_t)dsp_clamp_i32(p->exc.wave_morph + step, 0, 1023); break;
                case 1: p->exc.pulse_width  = (int16_t)dsp_clamp_i32(p->exc.pulse_width + step, 0, 1023); break;
                case 2: p->exc.vco_level    = (int16_t)dsp_clamp_i32(p->exc.vco_level + step, 0, 1023); break;
                case 3: p->exc.vco_semi     = (int8_t)dsp_clamp_i32(p->exc.vco_semi + delta, -24, 24); break;
                case 4: p->exc.vco_fine     = (int16_t)dsp_clamp_i32(p->exc.vco_fine + delta * 2, -100, 100); break;
                case 5: p->exc.noise_level  = (int16_t)dsp_clamp_i32(p->exc.noise_level + step, 0, 1023); break;
                case 6: p->exc.sh_rate      = (int16_t)dsp_clamp_i32(p->exc.sh_rate + step, 0, 1023); break;
                case 7: p->exc.pluck        = (int16_t)dsp_clamp_i32(p->exc.pluck + step, 0, 1023); break;
                case 8: p->exc.analog_drift = (int16_t)dsp_clamp_i32(p->exc.analog_drift + step, 0, 1023); break;
                case 9: p->exc.vco_keytrack = (delta > 0); break;
            }
            break;
        case 1: /* ENV 1 (EXCITER DADSR + LOOP + ENV RANGE) */
            switch (enc) {
                case 0: p->env1.attack      = (int16_t)dsp_clamp_i32(p->env1.attack + step, 0, 1023); break;
                case 1: p->env1.decay       = (int16_t)dsp_clamp_i32(p->env1.decay + step, 0, 1023); break;
                case 2: p->env1.sustain     = (int16_t)dsp_clamp_i32(p->env1.sustain + step, 0, 1023); break;
                case 3: p->env1.release     = (int16_t)dsp_clamp_i32(p->env1.release + step, 0, 1023); break;
                case 4: p->env1.onset_delay = (int16_t)dsp_clamp_i32(p->env1.onset_delay + step, 0, 1023); break;
                case 5: p->env1.vel_sens    = (int16_t)dsp_clamp_i32(p->env1.vel_sens + step, 0, 1023); break;
                case 6: p->env1.loop_mode   = (uint8_t)dsp_clamp_i32((int32_t)p->env1.loop_mode + (delta > 0 ? 1 : -1), 0, 2); break;
                case 7: {
                    /* Ordered SHORT (0) <-> NORM (1) <-> LONG (2) while enum 0 is NORM default */
                    int32_t ord = (p->env1.range == ADRIFT_ENV_RANGE_SHORT) ? 0
                                : ((p->env1.range == ADRIFT_ENV_RANGE_LONG) ? 2 : 1);
                    ord = dsp_clamp_i32(ord + (delta > 0 ? 1 : -1), 0, 2);
                    p->env1.range = (ord == 0) ? ADRIFT_ENV_RANGE_SHORT
                                  : ((ord == 2) ? ADRIFT_ENV_RANGE_LONG : ADRIFT_ENV_RANGE_NORM);
                    break;
                }
            }
            break;
        case 2: /* BBD RESONATOR */
            switch (enc) {
                case 0: p->in_lpf_cutoff     = (int16_t)dsp_clamp_i32(p->in_lpf_cutoff + step, 0, 1023); break;
                case 1: p->in_lpf_res        = (int16_t)dsp_clamp_i32(p->in_lpf_res + step, 0, 1023); break;
                case 2: p->bbd.feedback      = (int16_t)dsp_clamp_i32(p->bbd.feedback + step, 0, 1023); break;
                case 3: p->bbd.fb_lpf_cutoff = (int16_t)dsp_clamp_i32(p->bbd.fb_lpf_cutoff + step, 0, 1023); break;
                case 4: {
                    /* 3-state Invert mode: OFF -> INV (-1 oct) -> INV+COMP (key pitch) */
                    int32_t st = p->bbd.invert ? (p->bbd.invert_pitch_comp ? 2 : 1) : 0;
                    st = dsp_clamp_i32(st + (delta > 0 ? 1 : -1), 0, 2);
                    p->bbd.invert            = (st > 0);
                    p->bbd.invert_pitch_comp = (st == 2);
                    break;
                }
                case 5: p->bbd.glide         = (int16_t)dsp_clamp_i32(p->bbd.glide + step, 0, 1023); break;
                case 6: p->dry_wet           = (int16_t)dsp_clamp_i32(p->dry_wet + step, 0, 1023); break;
                case 7: p->bbd.dispersion    = (int16_t)dsp_clamp_i32(p->bbd.dispersion + step, 0, 1023); break;
            }
            break;
        case 3: /* ENV 2 (POST-BBD OUTPUT VCA) */
            switch (enc) {
                case 0: p->env2.attack      = (int16_t)dsp_clamp_i32(p->env2.attack + step, 0, 1023); break;
                case 1: p->env2.decay       = (int16_t)dsp_clamp_i32(p->env2.decay + step, 0, 1023); break;
                case 2: p->env2.sustain     = (int16_t)dsp_clamp_i32(p->env2.sustain + step, 0, 1023); break;
                case 3: p->env2.release     = (int16_t)dsp_clamp_i32(p->env2.release + step, 0, 1023); break;
                case 4: p->env2.onset_delay = (int16_t)dsp_clamp_i32(p->env2.onset_delay + step, 0, 1023); break;
                case 5: p->env2.vel_sens    = (int16_t)dsp_clamp_i32(p->env2.vel_sens + step, 0, 1023); break;
                case 6: p->env2.drone_hold  = (delta > 0); break;
            }
            break;
        case 4: /* LFO 1..3 & MOD (4 subpages: LFO1, LFO2, LFO3, MW) */
            switch (enc) {
                case 0:  p->lfo_wave   = (uint8_t)dsp_clamp_i32((int32_t)p->lfo_wave + (delta > 0 ? 1 : -1), 0, ADRIFT_LFO_COUNT - 1); break;
                case 1:  p->lfo_rate   = (int16_t)dsp_clamp_i32(p->lfo_rate + step, 0, 1023); break;
                case 2:  p->lfo_depth  = (int16_t)dsp_clamp_i32(p->lfo_depth + delta * 8, -512, 511); break;
                case 3:  p->lfo_dest   = (uint8_t)dsp_clamp_i32((int32_t)p->lfo_dest + (delta > 0 ? 1 : -1), 0, (int32_t)ADRIFT_LFO1_DEST_COUNT - 1); break;
                case 4:  p->lfo2_wave  = (uint8_t)dsp_clamp_i32((int32_t)p->lfo2_wave + (delta > 0 ? 1 : -1), 0, ADRIFT_LFO_COUNT - 1); break;
                case 5:  p->lfo2_rate  = (int16_t)dsp_clamp_i32(p->lfo2_rate + step, 0, 1023); break;
                case 6:  p->lfo2_depth = (int16_t)dsp_clamp_i32(p->lfo2_depth + delta * 8, -512, 511); break;
                case 7:  p->lfo2_dest  = (uint8_t)dsp_clamp_i32((int32_t)p->lfo2_dest + (delta > 0 ? 1 : -1), 0, ADRIFT_MOD_DEST_COUNT - 1); break;
                case 8:  p->lfo3_wave  = (uint8_t)dsp_clamp_i32((int32_t)p->lfo3_wave + (delta > 0 ? 1 : -1), 0, ADRIFT_LFO_COUNT - 1); break;
                case 9:  p->lfo3_rate  = (int16_t)dsp_clamp_i32(p->lfo3_rate + step, 0, 1023); break;
                case 10: p->lfo3_depth = (int16_t)dsp_clamp_i32(p->lfo3_depth + delta * 8, -512, 511); break;
                case 11: p->lfo3_dest  = (uint8_t)dsp_clamp_i32((int32_t)p->lfo3_dest + (delta > 0 ? 1 : -1), 0, ADRIFT_MOD_DEST_COUNT - 1); break;
                case 12: p->mw_depth   = (int16_t)dsp_clamp_i32(p->mw_depth + step, 0, 1023); break;
            }
            break;
    }
}

static void apply_group_menu_encoder(adrift_synth_t *s, uint8_t enc, int32_t delta) {
    if (delta == 0) return;
    uint8_t g = s->edit_group % ADRIFT_MAX_GROUPS;
    adrift_group_t *gp = &s->groups[g];

    switch (enc) {
        case 0: { /* 1:VOICES — strictly capped to (gp->num_voices + free_voices) */
            int32_t want = dsp_clamp_i32((int32_t)gp->num_voices + delta, 0, ADRIFT_MAX_VOICES);
            (void)adrift_synth_set_group_voices(s, g, (uint8_t)want);
            break;
        }
        case 1: { /* 2:ASSIGN — POLY (0) / UNISON (1) / SUPER (2) */
            int32_t step = (delta > 0) ? 1 : -1;
            uint8_t next_mode = (uint8_t)dsp_clamp_i32((int32_t)gp->assign_mode + step, 0, 2);
            if (next_mode != gp->assign_mode) {
                adrift_synth_group_all_notes_off(s, g, false);
                gp->assign_mode = next_mode;
                if (gp->patch.env2.drone_hold) {
                    adrift_synth_wake_group_drone(s, g);
                }
            }
            break;
        }
        case 2: /* 3:SPREAD — Per-group Stereo Spread 0..100% */
            gp->stereo_spread = (int16_t)dsp_clamp_i32(gp->stereo_spread + delta * 20, 0, 1023);
            break;
        case 3: /* 4:DETUNE — Unison/Super Detune 0..120 cents (non-linear curve) */
            gp->unison_detune = (int16_t)dsp_clamp_i32(gp->unison_detune + delta * 20, 0, 1023);
            break;
        case 4: { /* 5:MIDI — Channel 1..16 */
            uint8_t next_ch = (uint8_t)dsp_clamp_i32((int32_t)gp->midi_channel + delta, 1, 16);
            if (next_ch != gp->midi_channel) {
                adrift_synth_group_all_notes_off(s, g, false);
                gp->midi_channel = next_ch;
            }
            break;
        }
        case 5: /* 6:PAN — Per-group Stereo Pan -100% (L100) .. 0 (C) .. +100% (R100) */
            gp->pan = (int16_t)dsp_clamp_i32((int32_t)gp->pan + delta * 16, -512, 512);
            break;
        case 6: /* 7:LEGATO — Retrigger vs Legato */
            gp->patch.legato = (delta > 0);
            break;
        case 7: /* 8:GRP LVL — Per-group output volume */
        default:
            gp->patch.master_vol = (int16_t)dsp_clamp_i32(gp->patch.master_vol + delta * 12, 0, 1023);
            break;
    }
}

static void get_group_menu_display(const adrift_synth_t *s,
                                   uint8_t enc,
                                   const char **out_label,
                                   char *val_buf,
                                   int32_t *out_norm_1023,
                                   bool *out_emphasis) {
    const adrift_group_t *gp = adrift_synth_active_group(s);
    *out_norm_1023 = 0;
    *out_emphasis  = false;
    val_buf[0]     = '\0';

    switch (enc) {
        case 0:
            *out_label     = "VOICES";
            *out_norm_1023 = ((int32_t)gp->num_voices * 1023) / ADRIFT_MAX_VOICES;
            *out_emphasis  = (gp->num_voices > 0u);
            if (gp->num_voices == 0u) {
                copy_str(val_buf, "OFF");
            } else {
                fmt_int(val_buf, gp->num_voices, "");
            }
            break;
        case 1: {
            static const char *const k_assign_names[3] = { "POLY", "UNISON", "SUPER" };
            *out_label     = "ASSIGN";
            *out_norm_1023 = (gp->assign_mode == ADRIFT_GROUP_SUPER) ? 1023
                           : (gp->assign_mode == ADRIFT_GROUP_UNISON) ? 512 : 0;
            *out_emphasis  = adrift_group_is_stacked(gp->assign_mode);
            copy_str(val_buf, k_assign_names[gp->assign_mode % 3u]);
            break;
        }
        case 2:
            *out_label     = "SPREAD";
            *out_norm_1023 = gp->stereo_spread;
            *out_emphasis  = (gp->stereo_spread > 0 &&
                              (adrift_group_is_stacked(gp->assign_mode) || gp->num_voices > 1u));
            fmt_int(val_buf, (gp->stereo_spread * 100) / 1023, "%");
            break;
        case 3:
            *out_label     = "DETUNE";
            *out_norm_1023 = gp->unison_detune;
            *out_emphasis  = (adrift_group_is_stacked(gp->assign_mode) && gp->unison_detune > 0);
            fmt_int(val_buf, adrift_detune_to_cents(gp->unison_detune), "C");
            break;
        case 4:
            *out_label     = "MIDI";
            *out_norm_1023 = (((int32_t)gp->midi_channel - 1) * 1023) / 15;
            val_buf[0] = 'C';
            val_buf[1] = 'H';
            val_buf[2] = ' ';
            fmt_int(&val_buf[3], gp->midi_channel, "");
            break;
        case 5: {
            int32_t pan_clamped = dsp_clamp_i32(gp->pan, -512, 512);
            int32_t pct         = (pan_clamped * 100) / 512;
            *out_label          = "PAN";
            *out_norm_1023      = ((pan_clamped + 512) * 1023) / 1024;
            *out_emphasis       = (pct != 0);
            if (pct < 0) {
                val_buf[0] = 'L';
                fmt_int(&val_buf[1], -pct, "");
            } else if (pct > 0) {
                val_buf[0] = 'R';
                fmt_int(&val_buf[1], pct, "");
            } else {
                copy_str(val_buf, "C");
            }
            break;
        }
        case 6:
            *out_label     = "LEGATO";
            *out_norm_1023 = gp->patch.legato ? 1023 : 0;
            *out_emphasis  = gp->patch.legato;
            copy_str(val_buf, gp->patch.legato ? "LEGATO" : "RETRIG");
            break;
        case 7:
        default:
            *out_label     = "GRP LV";
            *out_norm_1023 = gp->patch.master_vol;
            fmt_int(val_buf, (gp->patch.master_vol * 100) / 1023, "%");
            break;
    }
}

static void apply_arp_menu_encoder(adrift_synth_t *s, uint8_t enc, int32_t delta) {
    if (delta == 0) return;
    uint8_t g = s->edit_group % ADRIFT_MAX_GROUPS;
    adrift_group_t *gp = &s->groups[g];
    adrift_arp_params_t *ap = &gp->patch.arp;

    switch (enc) {
        case 0: { /* 1:ALGO — Arpeggiator Algorithm (OFF, UP, DOWN, UP/DN, DN/UP, UP&DN, CONVRG, DIVERG, RANDOM, ORDER, CHORD) */
            int32_t next_m = dsp_clamp_i32((int32_t)ap->mode + delta, 0, (int32_t)ADRIFT_ARP_MODE_COUNT - 1);
            adrift_synth_set_group_arp_mode(s, g, (adrift_arp_mode_t)next_m);
            break;
        }
        case 1: /* 2:RANGE — 1..4 Octaves */
            ap->range = (uint8_t)dsp_clamp_i32((int32_t)ap->range + delta, 1, 4);
            break;
        case 2: /* 3:RATE — 40..240 BPM */
            ap->rate_bpm = (uint16_t)dsp_clamp_i32((int32_t)ap->rate_bpm + delta * 2, 40, 240);
            break;
        case 3: /* 4:GATE — 5..100% */
            ap->gate = (uint8_t)dsp_clamp_i32((int32_t)ap->gate + delta * 5, 5, 100);
            break;
        case 4: /* 5:VEL — 0 (KEY) or 1..127 fixed velocity */
            ap->velocity = (uint8_t)dsp_clamp_i32((int32_t)ap->velocity + delta * 2, 0, 127);
            break;
        case 5: /* 6:LATCH — OFF / ON */
            ap->latch = (delta > 0);
            adrift_synth_sync_group_arp(s, g);
            break;
        case 6: /* 7:SWING — 0..100% */
            ap->swing = (uint8_t)dsp_clamp_i32((int32_t)ap->swing + delta * 2, 0, 100);
            break;
        case 7: /* 8:RATCH — Ratchet subdivision mode */
        default:
            ap->ratchet = (adrift_arp_ratchet_t)dsp_clamp_i32(
                (int32_t)ap->ratchet + delta, 0, (int32_t)ADRIFT_RATCHET_COUNT - 1);
            break;
    }
}

static void get_arp_menu_display(const adrift_synth_t *s,
                                 uint8_t enc,
                                 const char **out_label,
                                 char *val_buf,
                                 int32_t *out_norm_1023,
                                 bool *out_emphasis) {
    const adrift_group_t *gp = adrift_synth_active_group(s);
    const adrift_arp_params_t *ap = &gp->patch.arp;
    *out_norm_1023 = 0;
    *out_emphasis  = false;
    val_buf[0]     = '\0';

    switch (enc) {
        case 0: {
            uint8_t m = (uint8_t)ap->mode % (uint8_t)ADRIFT_ARP_MODE_COUNT;
            *out_label     = "ALGO";
            *out_norm_1023 = ((int32_t)m * 1023) / ((int32_t)ADRIFT_ARP_MODE_COUNT - 1);
            *out_emphasis  = (m != (uint8_t)ADRIFT_ARP_OFF);
            copy_str(val_buf, k_arp_mode_names[m]);
            break;
        }
        case 1: {
            int32_t rng = dsp_clamp_i32(ap->range, 1, 4);
            *out_label     = "RANGE";
            *out_norm_1023 = ((rng - 1) * 1023) / 3;
            *out_emphasis  = (rng > 1);
            fmt_int(val_buf, rng, " OCT");
            break;
        }
        case 2: {
            *out_label = "RATE";
            if (s->midi_clk_active) {
                int32_t bpm = dsp_clamp_i32((int32_t)s->midi_clk_bpm, 30, 300);
                *out_norm_1023 = ((bpm - 30) * 1023) / 270;
                *out_emphasis  = true;
                val_buf[0] = 'E';
                val_buf[1] = 'X';
                val_buf[2] = 'T';
                fmt_int(&val_buf[3], bpm, "");
            } else {
                int32_t bpm = dsp_clamp_i32(ap->rate_bpm, 40, 240);
                *out_norm_1023 = ((bpm - 40) * 1023) / 200;
                fmt_int(val_buf, bpm, "BPM");
            }
            break;
        }
        case 3: {
            int32_t gt = dsp_clamp_i32(ap->gate, 5, 100);
            *out_label     = "GATE";
            *out_norm_1023 = ((gt - 5) * 1023) / 95;
            fmt_int(val_buf, gt, "%");
            break;
        }
        case 4:
            *out_label     = "VEL";
            *out_norm_1023 = ((int32_t)ap->velocity * 1023) / 127;
            *out_emphasis  = (ap->velocity > 0u);
            if (ap->velocity == 0u) {
                copy_str(val_buf, "KEY");
            } else {
                fmt_int(val_buf, ap->velocity, "");
            }
            break;
        case 5:
            *out_label     = "LATCH";
            *out_norm_1023 = ap->latch ? 1023 : 0;
            *out_emphasis  = ap->latch;
            copy_str(val_buf, ap->latch ? "ON" : "OFF");
            break;
        case 6: {
            int32_t sw = dsp_clamp_i32(ap->swing, 0, 100);
            *out_label     = "SWING";
            *out_norm_1023 = (sw * 1023) / 100;
            *out_emphasis  = (sw > 0);
            fmt_int(val_buf, sw, "%");
            break;
        }
        case 7:
        default: {
            uint8_t r = (uint8_t)ap->ratchet % (uint8_t)ADRIFT_RATCHET_COUNT;
            *out_label     = "RATCH";
            *out_norm_1023 = ((int32_t)r * 1023) / ((int32_t)ADRIFT_RATCHET_COUNT - 1);
            *out_emphasis  = (r != (uint8_t)ADRIFT_RATCHET_1X);
            copy_str(val_buf, k_arp_ratchet_names[r]);
            break;
        }
    }
}

static void apply_fx_menu_encoder(adrift_synth_t *s, uint8_t enc, int32_t delta) {
    if (delta == 0) return;
    adrift_fx_params_t *fp = &s->fx_params;
    int32_t step = delta * 16;

    switch (enc) {
        case 0: /* 1:CHORUS — Stereo BBD Dimension Chorus Depth & Wet Mix (0..100%) */
            fp->chorus_mix = dsp_clamp_i32(fp->chorus_mix + step, 0, 1023);
            break;
        case 1: /* 2:DLY TM — Stereo Ping-Pong Tape Delay Time (20..360 ms) */
            fp->delay_time = dsp_clamp_i32(fp->delay_time + step, 0, 1023);
            break;
        case 2: /* 3:DLY FB — Tape Delay Feedback (0..92%) */
            fp->delay_fb = dsp_clamp_i32(fp->delay_fb + step, 0, 1023);
            break;
        case 3: /* 4:DLY MX — Delay Wet Send Level (0..100%) */
            fp->delay_mix = dsp_clamp_i32(fp->delay_mix + step, 0, 1023);
            break;
        case 4: /* 5:RVB SZ — Diffusion Reverb Tank Room Size / Decay (0..100%) */
            fp->reverb_size = dsp_clamp_i32(fp->reverb_size + step, 0, 1023);
            break;
        case 5: /* 6:RVB DM — Tank High-Frequency Damping (0..100%) */
            fp->reverb_damp = dsp_clamp_i32(fp->reverb_damp + step, 0, 1023);
            break;
        case 6: /* 7:SHIMMR — Octave-Up (+12 ST) Shimmer Feedback Injection (0..100%) */
            fp->reverb_shimmer = dsp_clamp_i32(fp->reverb_shimmer + step, 0, 1023);
            break;
        case 7: /* 8:RVB MX — Reverb Wet Send Level (0..100%) */
            fp->reverb_mix = dsp_clamp_i32(fp->reverb_mix + step, 0, 1023);
            break;
        case 8: /* 9:COMP — Stereo Bus Glue Compressor Dry/Wet Parallel Mix (0..100%) */
            fp->comp_mix = dsp_clamp_i32(fp->comp_mix + step, 0, 1023);
            break;
        case 9: /* 10:THRESH — Bus Compressor Threshold (-24..-2 dB) */
            fp->comp_thresh = dsp_clamp_i32(fp->comp_thresh + step, 0, 1023);
            break;
        case 10: /* 11:ATTACK — Bus Compressor Attack Time (1..50 ms) */
            fp->comp_attack = dsp_clamp_i32(fp->comp_attack + step, 0, 1023);
            break;
        case 11: /* 12:MAKEUP — Bus Compressor Makeup Gain (+0..+12 dB) */
        default:
            fp->comp_makeup = dsp_clamp_i32(fp->comp_makeup + step, 0, 1023);
            break;
    }
}

static void get_fx_menu_display(const adrift_synth_t *s,
                                uint8_t enc,
                                const char **out_label,
                                char *val_buf,
                                int32_t *out_norm_1023,
                                bool *out_emphasis) {
    const adrift_fx_params_t *fp = &s->fx_params;
    *out_norm_1023 = 0;
    *out_emphasis  = false;
    val_buf[0]     = '\0';

    switch (enc) {
        case 0:
            *out_label     = "CHORUS";
            *out_norm_1023 = fp->chorus_mix;
            *out_emphasis  = (fp->chorus_mix > 0);
            fmt_int(val_buf, (fp->chorus_mix * 100) / 1023, "%");
            break;
        case 1:
            *out_label     = "DLY TM";
            *out_norm_1023 = fp->delay_time;
            if (s->midi_clk_active) {
                *out_emphasis = true;
                copy_str(val_buf, adrift_fx_delay_subdiv_name(fp->delay_time));
            } else {
                fmt_ms(val_buf, (int32_t)adrift_fx_delay_time_ms(fp->delay_time));
            }
            break;
        case 2:
            *out_label     = "DLY FB";
            *out_norm_1023 = fp->delay_fb;
            *out_emphasis  = (fp->delay_fb > 750 && fp->delay_mix > 0);
            fmt_int(val_buf, (fp->delay_fb * 92) / 1023, "%");
            break;
        case 3:
            *out_label     = "DLY MX";
            *out_norm_1023 = fp->delay_mix;
            *out_emphasis  = (fp->delay_mix > 0);
            fmt_int(val_buf, (fp->delay_mix * 100) / 1023, "%");
            break;
        case 4:
            *out_label     = "RVB SZ";
            *out_norm_1023 = fp->reverb_size;
            fmt_int(val_buf, (fp->reverb_size * 100) / 1023, "%");
            break;
        case 5:
            *out_label     = "RVB DM";
            *out_norm_1023 = fp->reverb_damp;
            fmt_int(val_buf, (fp->reverb_damp * 100) / 1023, "%");
            break;
        case 6:
            *out_label     = "SHIMMR";
            *out_norm_1023 = fp->reverb_shimmer;
            *out_emphasis  = (fp->reverb_shimmer > 0 && fp->reverb_mix > 0);
            fmt_int(val_buf, (fp->reverb_shimmer * 100) / 1023, "%");
            break;
        case 7:
            *out_label     = "RVB MX";
            *out_norm_1023 = fp->reverb_mix;
            *out_emphasis  = (fp->reverb_mix > 0);
            fmt_int(val_buf, (fp->reverb_mix * 100) / 1023, "%");
            break;
        case 8:
            *out_label     = "COMP";
            *out_norm_1023 = fp->comp_mix;
            *out_emphasis  = (fp->comp_mix > 0);
            fmt_int(val_buf, (fp->comp_mix * 100) / 1023, "%");
            break;
        case 9:
            *out_label     = "THRESH";
            *out_norm_1023 = fp->comp_thresh;
            fmt_int(val_buf, adrift_fx_comp_thresh_db(fp->comp_thresh), "DB");
            break;
        case 10:
            *out_label     = "ATTACK";
            *out_norm_1023 = fp->comp_attack;
            fmt_ms(val_buf, (int32_t)adrift_fx_comp_attack_ms(fp->comp_attack));
            break;
        case 11:
        default:
            *out_label     = "MAKEUP";
            *out_norm_1023 = fp->comp_makeup;
            *out_emphasis  = (fp->comp_makeup > 0 && fp->comp_mix > 0);
            val_buf[0]     = '+';
            fmt_int(&val_buf[1], (fp->comp_makeup * 12 + 511) >> 10, "DB");
            break;
    }
}

static void get_param_display(const adrift_patch_t *p,
                              uint8_t page,
                              uint8_t enc,
                              const char **out_label,
                              char *val_buf,
                              int32_t *out_norm_1023,
                              bool *out_emphasis) {
    *out_norm_1023 = 0;
    *out_emphasis  = false;
    val_buf[0]     = '\0';

    switch (page) {
        case 0:
            switch (enc) {
                case 0: {
                    *out_label     = "MORPH";
                    *out_norm_1023 = p->exc.wave_morph;
                    break;
                }
                case 1:
                    *out_label = "PWM";
                    *out_norm_1023 = p->exc.pulse_width;
                    fmt_int(val_buf, 5 + (p->exc.pulse_width * 90) / 1023, "%");
                    break;
                case 2:
                    *out_label = "VCO LV";
                    *out_norm_1023 = p->exc.vco_level;
                    fmt_int(val_buf, (p->exc.vco_level * 100) / 1023, "%");
                    break;
                case 3:
                    *out_label = "SEMI";
                    *out_norm_1023 = ((p->exc.vco_semi + 24) * 1023) / 48;
                    fmt_int(val_buf, p->exc.vco_semi, "ST");
                    break;
                case 4:
                    *out_label = "FINE";
                    *out_norm_1023 = ((p->exc.vco_fine + 100) * 1023) / 200;
                    fmt_int(val_buf, p->exc.vco_fine, "C");
                    break;
                case 5:
                    *out_label = "MIX";
                    *out_norm_1023 = p->exc.noise_level;
                    fmt_int(val_buf, (p->exc.noise_level * 100) / 1023, "%");
                    break;
                case 6:
                    *out_label = "SH CLK";
                    *out_norm_1023 = p->exc.sh_rate;
                    if (p->exc.sh_rate >= 1000) {
                        copy_str(val_buf, "WHITE");
                    } else {
                        fmt_int(val_buf, (p->exc.sh_rate * 100) / 1000, "%");
                    }
                    break;
                case 7:
                    *out_label = "PLUCK";
                    *out_norm_1023 = p->exc.pluck;
                    *out_emphasis  = (p->exc.pluck > 0);
                    fmt_int(val_buf, (p->exc.pluck * 100) / 1023, "%");
                    break;
                case 8:
                    *out_label = "DRIFT";
                    *out_norm_1023 = p->exc.analog_drift;
                    fmt_int(val_buf, (p->exc.analog_drift * 100) / 1023, "%");
                    break;
                case 9:
                    *out_label = "KEYTRK";
                    *out_norm_1023 = p->exc.vco_keytrack ? 1023 : 0;
                    *out_emphasis  = !p->exc.vco_keytrack;
                    copy_str(val_buf, p->exc.vco_keytrack ? "TRACK" : "FIXED");
                    break;
            }
            break;

        case 1:
            switch (enc) {
                case 0: *out_label = "ATTACK"; *out_norm_1023 = p->env1.attack;      fmt_int(val_buf, (p->env1.attack * 100) / 1023, "%"); break;
                case 1: *out_label = "DECAY";  *out_norm_1023 = p->env1.decay;       fmt_int(val_buf, (p->env1.decay * 100) / 1023, "%"); break;
                case 2: *out_label = "SUSTN";  *out_norm_1023 = p->env1.sustain;     fmt_int(val_buf, (p->env1.sustain * 100) / 1023, "%"); break;
                case 3: *out_label = "RELEAS"; *out_norm_1023 = p->env1.release;     fmt_int(val_buf, (p->env1.release * 100) / 1023, "%"); break;
                case 4: *out_label = "DELAY";  *out_norm_1023 = p->env1.onset_delay; fmt_ms(val_buf, (p->env1.onset_delay * p->env1.onset_delay * 2000) >> 20); break;
                case 5: *out_label = "VEL SN"; *out_norm_1023 = p->env1.vel_sens;    fmt_int(val_buf, (p->env1.vel_sens * 100) / 1023, "%"); break;
                case 6: {
                    static const char *const k_lm[3] = { "OFF", "A-D", "D-A-D" };
                    *out_label = "LOOP";
                    *out_norm_1023 = p->env1.loop_mode * 511;
                    *out_emphasis  = (p->env1.loop_mode != ADRIFT_ENV_LOOP_OFF);
                    copy_str(val_buf, k_lm[p->env1.loop_mode % 3u]);
                    break;
                }
                case 7:
                    *out_label = "RANGE";
                    if (p->env1.range == ADRIFT_ENV_RANGE_SHORT) {
                        *out_norm_1023 = 0;
                        *out_emphasis  = true;
                        copy_str(val_buf, "SHORT");
                    } else if (p->env1.range == ADRIFT_ENV_RANGE_LONG) {
                        *out_norm_1023 = 1023;
                        *out_emphasis  = true;
                        copy_str(val_buf, "LONG");
                    } else {
                        *out_norm_1023 = 512;
                        *out_emphasis  = false;
                        copy_str(val_buf, "NORM");
                    }
                    break;
            }
            break;

        case 2:
            switch (enc) {
                case 0: *out_label = "IN LPF"; *out_norm_1023 = p->in_lpf_cutoff; fmt_hz(val_buf, dsp_cutoff_to_fc_q8(p->in_lpf_cutoff) >> 8); break;
                case 1: *out_label = "IN RES"; *out_norm_1023 = p->in_lpf_res;    fmt_int(val_buf, (p->in_lpf_res * 100) / 1023, "%"); break;
                case 2: {
                    *out_label = "FEEDBK";
                    *out_norm_1023 = p->bbd.feedback;
                    int32_t pct = (p->bbd.feedback <= 890)
                                      ? ((p->bbd.feedback * 100) / 890)
                                      : (100 + ((p->bbd.feedback - 890) * 15) / 133);
                    *out_emphasis = (pct > 100);
                    fmt_int(val_buf, pct, "%");
                    break;
                }
                case 3: *out_label = "FB LPF"; *out_norm_1023 = p->bbd.fb_lpf_cutoff; fmt_hz(val_buf, dsp_cutoff_to_fc_q8(p->bbd.fb_lpf_cutoff) >> 8); break;
                case 4: {
                    *out_label = "INVERT";
                    int32_t st = p->bbd.invert ? (p->bbd.invert_pitch_comp ? 2 : 1) : 0;
                    *out_norm_1023 = st * 511;
                    *out_emphasis  = (st > 0);
                    copy_str(val_buf, (st == 0) ? "OFF" : ((st == 1) ? "INV-1O" : "INV+0O"));
                    break;
                }
                case 5: *out_label = "GLIDE";  *out_norm_1023 = p->bbd.glide; fmt_int(val_buf, (p->bbd.glide * 100) / 1023, "%"); break;
                case 6: *out_label = "DRYWET"; *out_norm_1023 = p->dry_wet;   fmt_int(val_buf, (p->dry_wet * 100) / 1023, "%"); break;
                case 7:
                    *out_label = "DISPR";
                    *out_norm_1023 = p->bbd.dispersion;
                    *out_emphasis  = (p->bbd.dispersion > 0);
                    fmt_int(val_buf, (p->bbd.dispersion * 100) / 1023, "%");
                    break;
            }
            break;

        case 3:
            switch (enc) {
                case 0: *out_label = "ATTACK"; *out_norm_1023 = p->env2.attack;      fmt_int(val_buf, (p->env2.attack * 100) / 1023, "%"); break;
                case 1: *out_label = "DECAY";  *out_norm_1023 = p->env2.decay;       fmt_int(val_buf, (p->env2.decay * 100) / 1023, "%"); break;
                case 2: *out_label = "SUSTN";  *out_norm_1023 = p->env2.sustain;     fmt_int(val_buf, (p->env2.sustain * 100) / 1023, "%"); break;
                case 3: *out_label = "RELEAS"; *out_norm_1023 = p->env2.release;     fmt_int(val_buf, (p->env2.release * 100) / 1023, "%"); break;
                case 4: *out_label = "DELAY";  *out_norm_1023 = p->env2.onset_delay; fmt_ms(val_buf, (p->env2.onset_delay * p->env2.onset_delay * 2000) >> 20); break;
                case 5: *out_label = "VEL SN"; *out_norm_1023 = p->env2.vel_sens;    fmt_int(val_buf, (p->env2.vel_sens * 100) / 1023, "%"); break;
                case 6: {
                    *out_label = "DRONE";
                    *out_norm_1023 = p->env2.drone_hold ? 1023 : 0;
                    *out_emphasis  = p->env2.drone_hold;
                    copy_str(val_buf, p->env2.drone_hold ? "HOLD" : "GATED");
                    break;
                }
            }
            break;

        case 4:
        default: {
            static const char *const k_lw[5] = { "SINE", "TRI", "SAW", "SQR", "S&H" };
            switch (enc) {
                case 0:
                    *out_label = "L1 WAV";
                    *out_norm_1023 = (p->lfo_wave * 1023) / 4;
                    copy_str(val_buf, k_lw[p->lfo_wave % 5u]);
                    break;
                case 1:
                    *out_label = "L1 RT";
                    *out_norm_1023 = p->lfo_rate;
                    fmt_int(val_buf, (p->lfo_rate * 100) / 1023, "%");
                    break;
                case 2:
                    *out_label = "L1 DEP";
                    *out_norm_1023 = p->lfo_depth + 512;
                    *out_emphasis  = (p->lfo_depth != 0);
                    fmt_int(val_buf, (p->lfo_depth * 100) / 512, "%");
                    break;
                case 3: {
                    uint8_t d1 = (p->lfo_dest < ADRIFT_LFO1_DEST_COUNT) ? p->lfo_dest : (uint8_t)(ADRIFT_LFO1_DEST_COUNT - 1u);
                    *out_label = "L1 DST";
                    *out_norm_1023 = ((int32_t)d1 * 1023) / ((int32_t)ADRIFT_LFO1_DEST_COUNT - 1);
                    *out_emphasis  = (p->lfo_depth != 0);
                    copy_str(val_buf, k_mod_dest_names[d1]);
                    break;
                }
                case 4:
                    *out_label = "L2 WAV";
                    *out_norm_1023 = (p->lfo2_wave * 1023) / 4;
                    copy_str(val_buf, k_lw[p->lfo2_wave % 5u]);
                    break;
                case 5:
                    *out_label = "L2 RT";
                    *out_norm_1023 = p->lfo2_rate;
                    fmt_int(val_buf, (p->lfo2_rate * 100) / 1023, "%");
                    break;
                case 6:
                    *out_label = "L2 DEP";
                    *out_norm_1023 = p->lfo2_depth + 512;
                    *out_emphasis  = (p->lfo2_depth != 0);
                    fmt_int(val_buf, (p->lfo2_depth * 100) / 512, "%");
                    break;
                case 7:
                    *out_label = "L2 DST";
                    *out_norm_1023 = ((int32_t)p->lfo2_dest * 1023) / ((int32_t)ADRIFT_MOD_DEST_COUNT - 1);
                    *out_emphasis  = (p->lfo2_depth != 0);
                    copy_str(val_buf, k_mod_dest_names[p->lfo2_dest % (uint8_t)ADRIFT_MOD_DEST_COUNT]);
                    break;
                case 8:
                    *out_label = "L3 WAV";
                    *out_norm_1023 = (p->lfo3_wave * 1023) / 4;
                    copy_str(val_buf, k_lw[p->lfo3_wave % 5u]);
                    break;
                case 9:
                    *out_label = "L3 RT";
                    *out_norm_1023 = p->lfo3_rate;
                    fmt_int(val_buf, (p->lfo3_rate * 100) / 1023, "%");
                    break;
                case 10:
                    *out_label = "L3 DEP";
                    *out_norm_1023 = p->lfo3_depth + 512;
                    *out_emphasis  = (p->lfo3_depth != 0);
                    fmt_int(val_buf, (p->lfo3_depth * 100) / 512, "%");
                    break;
                case 11:
                    *out_label = "L3 DST";
                    *out_norm_1023 = ((int32_t)p->lfo3_dest * 1023) / ((int32_t)ADRIFT_MOD_DEST_COUNT - 1);
                    *out_emphasis  = (p->lfo3_depth != 0);
                    copy_str(val_buf, k_mod_dest_names[p->lfo3_dest % (uint8_t)ADRIFT_MOD_DEST_COUNT]);
                    break;
                case 12:
                    *out_label = "MW DEP";
                    *out_norm_1023 = p->mw_depth;
                    fmt_int(val_buf, (p->mw_depth * 100) / 1023, "%");
                    break;
            }
            break;
        }
    }
}

void apply_encoder_for_synth(adrift_synth_t *synth,
                             bool in_group_menu,
                             bool in_arp_menu,
                             bool in_fx_menu,
                             uint8_t page,
                             uint8_t enc,
                             int32_t delta) {
    if (delta == 0) return;

    if (in_group_menu) {
        apply_group_menu_encoder(synth, enc, delta);
        return;
    }
    if (in_arp_menu) {
        apply_arp_menu_encoder(synth, enc, delta);
        return;
    }
    if (in_fx_menu) {
        apply_fx_menu_encoder(synth, enc, delta);
        return;
    }

    uint8_t g = synth->edit_group % ADRIFT_MAX_GROUPS;
    adrift_group_t *gp = &synth->groups[g];
    apply_patch_encoder(&gp->patch, page, enc, delta);
    if (page == 3u && enc == 6u && gp->patch.env2.drone_hold) {
        adrift_synth_wake_group_drone(synth, g);
    }
}

uint8_t page_num_subpages(uint8_t page, bool in_group_menu, bool in_arp_menu, bool in_fx_menu) {
    if (in_fx_menu) return 3u; /* FX: 1/3..3/3 */
    if (in_group_menu || in_arp_menu) return 2u;
    if (page == 0u) return 3u; /* EXC: 1/3..3/3 */
    if (page == 4u) return 4u; /* MOD: 1/4..4/4 */
    return 2u;
}

bool is_unmapped_param_slot(uint8_t page, uint8_t param_idx) {
    return (page == 0u && param_idx >= 10u) || /* EXC  Page 3/3 slots 2..3 */
           (page == 3u && param_idx >= 7u)  || /* ENV2 Page 2/2 slot  3    */
           (page == 4u && param_idx >= 13u);   /* MOD  Page 4/4 slots 1..3 */
}

/* Continuous Micro-ACB waveform preview (Sine -> Triangle -> Sagged Saw -> PWM Pulse) inside MORPH card */
static void draw_morph_wave(uint16_t *fb,
                            uint32_t cx,
                            uint32_t cy,
                            uint32_t cw,
                            const adrift_patch_t *p,
                            const adrift_group_theme_t *th) {
    const uint32_t wx0   = cx + 5u;
    const uint32_t ww    = cw - 10u; /* 46 px wide, aligned with bottom progress bar */
    const int32_t  mid_y = (int32_t)(cy + 51u);
    const int32_t  amp_y = 17;
    uint16_t fill_col    = make_env_fill_col(th->accent);

    /* Dotted zero-crossing baseline */
    for (uint32_t dx = 0; dx < ww; dx += 3u) {
        fb[(uint32_t)mid_y * ADRIFT_SCREEN_W + (wx0 + dx)] = COL_BAR_TRACK;
    }

    uint32_t prev_py = (uint32_t)mid_y;
    for (uint32_t dx = 0; dx < ww; dx++) {
        uint32_t p16   = (dx * 65535u) / (ww - 1u);
        int32_t  y_q15 = eval_morph_wave_q15(p16, p);
        int32_t  dy    = (y_q15 * amp_y) >> 15;
        uint32_t py    = (uint32_t)dsp_clamp_i32(mid_y - dy, mid_y - amp_y, mid_y + amp_y);
        uint32_t x     = wx0 + dx;

        /* Subtle vertical fill between wave trace and centerline */
        uint32_t fy0 = (py < (uint32_t)mid_y) ? py : (uint32_t)mid_y;
        uint32_t fy1 = (py < (uint32_t)mid_y) ? (uint32_t)mid_y : py;
        for (uint32_t fy = fy0; fy <= fy1; fy++) {
            fb[fy * ADRIFT_SCREEN_W + x] = fill_col;
        }

        /* 2px connected bright wave trace */
        if (dx > 0u) {
            uint32_t y0 = (prev_py < py) ? prev_py : py;
            uint32_t y1 = (prev_py < py) ? py : prev_py;
            for (uint32_t y = y0; y <= y1; y++) {
                fb[y * ADRIFT_SCREEN_W + x] = COL_WHITE;
                if (y + 1u <= (uint32_t)(mid_y + amp_y + 1)) {
                    fb[(y + 1u) * ADRIFT_SCREEN_W + x] = th->accent;
                }
            }
        } else {
            fb[py * ADRIFT_SCREEN_W + x] = COL_WHITE;
            fb[(py + 1u) * ADRIFT_SCREEN_W + x] = th->accent;
        }
        prev_py = py;
    }
}

/* 1x4 Parameter Card Row: 4 cards mapped 1:1 to physical KNOB1..KNOB4 (4th cell empty on Page 2/2 except GRP/ARP/FX) */
void draw_cards_1x4(uint16_t *fb,
                    const adrift_synth_t *s,
                    uint8_t page,
                    uint8_t subpage,
                    bool in_group_menu,
                    bool in_arp_menu,
                    bool in_fx_menu) {
    const adrift_group_theme_t *th = get_group_theme(s->edit_group);
    const adrift_patch_t       *p  = adrift_synth_view_patch(s);

    const uint32_t grid_y = 136u;
    const uint32_t grid_h = 104u;
    const uint32_t cy     = 139u;
    const uint32_t ch     = 97u;
    const uint32_t card_w = 56u;

    fb_fill_rect(fb, 0, grid_y, ADRIFT_SCREEN_W, grid_h, COL_BG);

    for (uint8_t slot = 0; slot < ADRIFT_NUM_PARAM_KNOBS; slot++) {
        uint32_t cx        = 5u + (uint32_t)slot * 58u;
        uint8_t  num_sub   = page_num_subpages(page, in_group_menu, in_arp_menu, in_fx_menu);
        uint8_t  param_idx = (uint8_t)((subpage % num_sub) * 4u + slot);

        /* Leave unmapped card slots empty (EXC 3/3 slots 2..3; ENV2 2/2 slot 3; MOD 4/4 slots 1..3) */
        if (!in_group_menu && !in_arp_menu && !in_fx_menu && is_unmapped_param_slot(page, param_idx)) {
            fb_fill_rect(fb, cx, cy, card_w, ch, COL_SCOPE_BG);
            fb_rect_outline(fb, cx, cy, card_w, ch, COL_BAR_TRACK);
            continue;
        }

        const char *label = "";
        char val_str[16];
        int32_t norm_1023 = 0;
        bool emphasis     = false;
        if (in_group_menu) {
            get_group_menu_display(s, param_idx, &label, val_str, &norm_1023, &emphasis);
        } else if (in_arp_menu) {
            get_arp_menu_display(s, param_idx, &label, val_str, &norm_1023, &emphasis);
        } else if (in_fx_menu) {
            get_fx_menu_display(s, param_idx, &label, val_str, &norm_1023, &emphasis);
        } else {
            get_param_display(p, page, param_idx, &label, val_str, &norm_1023, &emphasis);
        }

        fb_fill_rect(fb, cx, cy, card_w, ch, COL_CARD);

        /* Card top header band with 7x10 bold parameter label */
        uint32_t hdr_h = 22u;
        fb_fill_rect(fb, cx + 1u, cy + 1u, card_w - 2u, hdr_h, COL_CARD_HDR);
        fb_hline(fb, cx + 1u, cy + 1u + hdr_h, card_w - 2u, th->border);
        fb_rect_outline(fb, cx, cy, card_w, ch, emphasis ? th->accent : th->border);

        fb_draw_str_centered_spec(fb, cx, card_w, cy + 7u, label,
                                  emphasis ? th->accent : COL_MUTED, &k_font_md);

        /* Value area: draw continuous wave shape in EXC -> MORPH cell, otherwise centered value text */
        if (!in_group_menu && !in_arp_menu && !in_fx_menu && page == 0u && param_idx == 0u) {
            draw_morph_wave(fb, cx, cy, card_w, p, th);
        } else {
            uint32_t vlen = str_len(val_str);
            const ui_font_spec_t *vf = (vlen <= 4u) ? &k_font_xl
                                     : ((vlen == 5u) ? &k_font_xl_cond : &k_font_xl_tight);
            fb_draw_str_centered_spec(fb, cx, card_w, cy + 42u, val_str,
                                      emphasis ? th->accent : COL_WHITE, vf);
        }

        /* Bottom progress bar */
        uint32_t bw_max = card_w - 10u;
        uint32_t bw     = (uint32_t)((dsp_clamp_i32(norm_1023, 0, 1023) * (int32_t)bw_max) / 1023);
        uint32_t bar_y  = cy + ch - 18u;
        uint32_t bar_h  = 10u;
        fb_fill_rect(fb, cx + 5u, bar_y, bw_max, bar_h, COL_BAR_TRACK);
        if (bw > 0u) {
            fb_fill_rect(fb, cx + 5u, bar_y, bw, bar_h, th->accent);
        }
    }
}
