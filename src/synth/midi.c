#include "synth.h"

static inline int16_t cc_to_1023(uint8_t val) {
    return (int16_t)(((int32_t)(val & 0x7Fu) * 1023) / 127);
}

static void apply_cc_to_patch(adrift_patch_t *p, uint8_t cc, uint8_t val) {
    int16_t v1023 = cc_to_1023(val);

    switch (cc) {
        case 5: /* Portamento / Glide */
            p->bbd.glide = v1023;
            break;

        /* Exciter section CCs (14..21) */
        case 14: p->exc.wave_morph   = v1023; break;
        case 15: p->exc.pulse_width  = v1023; break;
        case 16: p->exc.vco_level    = v1023; break;
        case 17: p->exc.vco_semi     = (int8_t)(((int32_t)val * 24) / 127 - 12); break;
        case 18: p->exc.vco_fine     = (int16_t)(((int32_t)val * 200) / 127 - 100); break;
        case 19: p->exc.noise_level  = v1023; break;
        case 20: p->exc.sh_rate      = v1023; break;
        case 21: p->exc.analog_drift = v1023; break;

        /* Envelope 1 (Exciter DADSR) CCs (22..28, 39) */
        case 22: p->env1.onset_delay = v1023; break;
        case 23: p->env1.attack      = v1023; break;
        case 24: p->env1.decay       = v1023; break;
        case 25: p->env1.sustain     = v1023; break;
        case 26: p->env1.release     = v1023; break;
        case 27: p->env1.vel_sens    = v1023; break;
        case 28: p->env1.loop_mode   = (val < 42) ? ADRIFT_ENV_LOOP_OFF
                                                  : ((val < 85) ? ADRIFT_ENV_LOOP_AD : ADRIFT_ENV_LOOP_DAD);
            break;
        case 39:
            p->env1.range = (val < 42u) ? ADRIFT_ENV_RANGE_SHORT
                                        : ((val < 85u) ? ADRIFT_ENV_RANGE_NORM : ADRIFT_ENV_RANGE_LONG);
            break;

        /* LFO 2 CCs (31..34) & LFO 3 CCs (35..38) */
        case 31: p->lfo2_wave  = (uint8_t)(((uint32_t)val * ADRIFT_LFO_COUNT) / 128u); break;
        case 32: p->lfo2_rate  = v1023; break;
        case 33: p->lfo2_depth = (int16_t)(((int32_t)val * 1023) / 127 - 512); break;
        case 34: p->lfo2_dest  = (uint8_t)(((uint32_t)val * ADRIFT_MOD_DEST_COUNT) / 128u); break;
        case 35: p->lfo3_wave  = (uint8_t)(((uint32_t)val * ADRIFT_LFO_COUNT) / 128u); break;
        case 36: p->lfo3_rate  = v1023; break;
        case 37: p->lfo3_depth = (int16_t)(((int32_t)val * 1023) / 127 - 512); break;
        case 38: p->lfo3_dest  = (uint8_t)(((uint32_t)val * ADRIFT_MOD_DEST_COUNT) / 128u); break;

        case 68: p->legato          = (val >= 64u); break;
        case 69: p->env2.drone_hold = (val >= 64u); break;

        /* Filter & BBD Resonator standard/sound CCs (70..79) */
        case 70: p->exc.wave_morph        = v1023; break;
        case 71: p->in_lpf_res            = v1023; break;
        case 72: p->env2.release          = v1023; break;
        case 73: p->env2.attack           = v1023; break;
        case 74: p->in_lpf_cutoff         = v1023; break;
        case 75: p->bbd.feedback          = v1023; break;
        case 76: p->bbd.fb_lpf_cutoff     = v1023; break;
        case 77: p->bbd.invert            = (val >= 64u); break;
        case 78: p->bbd.invert_pitch_comp = (val >= 64u); break;
        case 79: p->dry_wet               = v1023; break;

        /* Envelope 2 & LFO CCs (80..87) */
        case 80: p->env2.decay       = v1023; break;
        case 81: p->env2.sustain     = v1023; break;
        case 82: p->env2.vel_sens    = v1023; break;
        case 83: p->lfo_wave         = (uint8_t)(((uint32_t)val * ADRIFT_LFO_COUNT) / 128u); break;
        case 84: p->lfo_rate         = v1023; break;
        case 85: p->lfo_depth        = (int16_t)(((int32_t)val * 1023) / 127 - 512); break;
        case 86: p->lfo_dest         = (uint8_t)(((uint32_t)val * ADRIFT_LFO1_DEST_COUNT) / 128u); break;
        case 87: p->exc.vco_keytrack = (val >= 64u); break;
        case 88: p->exc.pluck        = v1023; break;
        case 89: p->bbd.dispersion   = v1023; break;
        default: break;
    }
}

static void handle_group_cc(adrift_synth_t *s, uint8_t g, uint8_t cc, uint8_t val) {
    adrift_group_t *gp = &s->groups[g];

    switch (cc) {
        case 1: /* Mod Wheel (per group) */
            gp->mod_wheel = val;
            return;

        case 7: /* Channel / Group Volume */
            gp->patch.master_vol = cc_to_1023(val);
            return;

        case 64: { /* Sustain Pedal (per group) */
            bool down = (val >= 64u);
            if (gp->sustain_pedal && !down) {
                gp->sustain_pedal = false;
                for (uint8_t v = 0; v < ADRIFT_MAX_VOICES; v++) {
                    if (s->voices[v].owner_group == (int8_t)g &&
                        s->voices[v].sustained && !s->voices[v].key_down) {
                        s->voices[v].sustained = false;
                        adrift_env_note_off(&s->voices[v].env1, &gp->patch.env1);
                        adrift_env_note_off(&s->voices[v].env2, &gp->patch.env2);
                    }
                }
                bool any_g = false;
                for (uint8_t v = 0; v < ADRIFT_MAX_VOICES; v++) {
                    if (s->voices[v].owner_group >= 0 &&
                        (s->voices[v].key_down || s->voices[v].sustained)) {
                        any_g = true;
                        break;
                    }
                }
                s->any_gate = any_g;
                adrift_synth_sync_group_arp(s, g);
            } else {
                gp->sustain_pedal = down;
            }
            return;
        }

        case 10: /* Pan / Stereo Spread (per group) */
        case 93: /* Chorus / Stereo Width -> Stereo Spread */
            gp->stereo_spread = cc_to_1023(val);
            return;

        case 94: /* Detune Depth -> Unison Detune */
            gp->unison_detune = cc_to_1023(val);
            return;

        /* Arpeggiator CCs (102..109) */
        case 102: { /* Arp Algorithm */
            uint8_t m = (val < (uint8_t)ADRIFT_ARP_MODE_COUNT)
                            ? val
                            : (uint8_t)(((uint32_t)val * (uint32_t)ADRIFT_ARP_MODE_COUNT) / 128u);
            adrift_synth_set_group_arp_mode(s, g, (adrift_arp_mode_t)m);
            return;
        }
        case 103: /* Arp Range (1..4 octaves) */
            gp->patch.arp.range = (val >= 1u && val <= 4u)
                                      ? val
                                      : (uint8_t)(1u + ((uint32_t)val * 4u) / 128u);
            return;
        case 104: /* Arp Rate (40..240 BPM) */
            gp->patch.arp.rate_bpm = (uint16_t)(40u + ((uint32_t)val * 200u) / 127u);
            return;
        case 105: /* Arp Gate (5..100%) */
            gp->patch.arp.gate = (uint8_t)(5u + ((uint32_t)val * 95u) / 127u);
            return;
        case 106: /* Arp Velocity (0 = KEY, 1..127 = fixed) */
            gp->patch.arp.velocity = val & 0x7Fu;
            return;
        case 107: /* Arp Latch (OFF / ON) */
            gp->patch.arp.latch = (val >= 64u);
            adrift_synth_sync_group_arp(s, g);
            return;
        case 108: /* Arp Swing (0..100%) */
            gp->patch.arp.swing = (uint8_t)(((uint32_t)val * 100u) / 127u);
            return;
        case 109: { /* Arp Ratchet (0..7) */
            uint8_t r = (val < (uint8_t)ADRIFT_RATCHET_COUNT)
                            ? val
                            : (uint8_t)(((uint32_t)val * (uint32_t)ADRIFT_RATCHET_COUNT) / 128u);
            gp->patch.arp.ratchet = (adrift_arp_ratchet_t)r;
            return;
        }

        /* Master Stereo FX CCs (91, 92, 95 standard + 110..119 & 29..30 full FX bank) */
        case 29:
            s->fx_params.comp_attack = cc_to_1023(val);
            return;
        case 30:
            s->fx_params.comp_makeup = cc_to_1023(val);
            return;
        case 91:
        case 117:
            s->fx_params.reverb_mix = cc_to_1023(val);
            return;
        case 92:
        case 116:
            s->fx_params.reverb_shimmer = cc_to_1023(val);
            return;
        case 95:
        case 110:
            s->fx_params.chorus_mix = cc_to_1023(val);
            return;
        case 111:
            s->fx_params.delay_time = cc_to_1023(val);
            return;
        case 112:
            s->fx_params.delay_fb = cc_to_1023(val);
            return;
        case 113:
            s->fx_params.delay_mix = cc_to_1023(val);
            return;
        case 114:
            s->fx_params.reverb_size = cc_to_1023(val);
            return;
        case 115:
            s->fx_params.reverb_damp = cc_to_1023(val);
            return;
        case 118:
            s->fx_params.comp_mix = cc_to_1023(val);
            return;
        case 119:
            s->fx_params.comp_thresh = cc_to_1023(val);
            return;

        /* Channel Mode Messages (scoped to matching group) */
        case 120: /* All Sound Off */
            adrift_synth_group_all_notes_off(s, g, true);
            adrift_fx_reset(&s->fx);
            return;
        case 121: /* Reset All Controllers */
            gp->pitch_bend_q13 = 0;
            gp->mod_wheel      = 0u;
            gp->aftertouch     = 0u;
            if (gp->sustain_pedal) {
                handle_group_cc(s, g, 64u, 0u);
            }
            return;
        case 123: /* All Notes Off */
            adrift_synth_group_all_notes_off(s, g, false);
            return;
        default:
            break;
    }

    apply_cc_to_patch(&gp->patch, cc, val);
    if (cc == 69u && gp->patch.env2.drone_hold) {
        adrift_synth_wake_group_drone(s, g);
    }
}

void adrift_synth_handle_midi(adrift_synth_t *s, uint8_t status, uint8_t data1, uint8_t data2) {
    if (status < 0x80u) {
        return;
    }
    if (status >= 0xF0u) {
        if (status == 0xF8u || status == 0xFAu || status == 0xFBu || status == 0xFCu) {
            adrift_synth_handle_midi_realtime(s, status);
        }
        return;
    }
    uint8_t cmd  = status & 0xF0u;
    uint8_t chan = (status & 0x0Fu) + 1u; /* 1..16 */

    if (cmd == 0xB0u && (data1 & 0x7Fu) == 0u) {
        /* CC0 Bank Select MSB: 0 = Factory (P1..P24), 1 = User (U1..U24) */
        uint8_t bank = data2 & 0x7Fu;
        if (bank <= 1u) {
            s->midi_bank = bank;
        }
        return;
    }

    /* Program Change loads the full multi-group preset across all 6 groups */
    if (cmd == 0xC0u) {
        uint8_t pgm = data1 & 0x7Fu;
        uint8_t target;
        if (s->midi_bank == 1u) {
            if (pgm >= ADRIFT_NUM_USER_PRESETS) {
                return;
            }
            target = (uint8_t)(ADRIFT_USER_PRESET_BASE + pgm);
        } else {
            if (pgm >= ADRIFT_USER_PRESET_BASE + ADRIFT_NUM_USER_PRESETS) {
                return;
            }
            target = pgm;
        }
        if (target >= ADRIFT_USER_PRESET_BASE &&
            !adrift_synth_user_preset_is_occupied((uint8_t)(target - ADRIFT_USER_PRESET_BASE))) {
            return;
        }
        adrift_synth_all_notes_off(s, true);
        adrift_synth_load_preset(s, target);
        return;
    }

    for (uint8_t g = 0; g < ADRIFT_MAX_GROUPS; g++) {
        adrift_group_t *gp = &s->groups[g];
        if (gp->midi_channel != chan) {
            continue;
        }

        switch (cmd) {
            case 0x80u: /* Note Off */
                adrift_synth_group_note_off(s, g, data1 & 0x7Fu);
                break;
            case 0x90u: /* Note On */
                adrift_synth_group_note_on(s, g, data1 & 0x7Fu, data2 & 0x7Fu);
                break;
            case 0xA0u: /* Poly Aftertouch */
            case 0xD0u: /* Channel Pressure */
                gp->aftertouch = (cmd == 0xD0u) ? (data1 & 0x7Fu) : (data2 & 0x7Fu);
                break;
            case 0xB0u: /* Control Change */
                handle_group_cc(s, g, data1 & 0x7Fu, data2 & 0x7Fu);
                break;
            case 0xE0u: { /* 14-bit Pitch Bend */
                int32_t bend14 = ((int32_t)(data2 & 0x7Fu) << 7) | (int32_t)(data1 & 0x7Fu);
                gp->pitch_bend_q13 = (int16_t)(bend14 - 8192);
                break;
            }
            default:
                break;
        }
    }
}
