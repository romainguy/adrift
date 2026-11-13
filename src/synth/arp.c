#include "synth_internal.h"

void adrift_arp_reset_state(adrift_arp_state_t *st, uint8_t g) {
    uint8_t *raw = (uint8_t *)st;
    for (uint32_t i = 0; i < (uint32_t)sizeof(*st); i++) {
        raw[i] = 0u;
    }
    st->rng = 0xA5C39E11u ^ ((uint32_t)g * 0x9E3779B9u);
    st->cur_step_note = 60u;
    st->cur_step_vel  = 100u;
}

/* ============================================================================
 * Arpeggiator Engine (per-group, sample-accurate 1/16th step clock)
 * ============================================================================ */
static uint32_t arp_xorshift32(uint32_t *state) {
    uint32_t x = *state;
    if (x == 0u) x = 0xA5C39E11u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static void arp_stop_sounding_notes(adrift_synth_t *s, uint8_t g) {
    adrift_group_t *gp = &s->groups[g];
    adrift_arp_state_t *st = &gp->arp_state;
    for (uint8_t i = 0; i < st->sounding_count; i++) {
        adrift_synth_voice_note_off(s, g, st->sounding_notes[i]);
    }
    st->sounding_count = 0u;
    st->gate_open      = false;
}

/* Sort held notes ascending into `sorted_n` / `sorted_v` */
static void arp_sort_held_notes(const adrift_arp_state_t *st,
                                uint8_t *sorted_n,
                                uint8_t *sorted_v) {
    uint8_t n = st->num_notes;
    for (uint8_t i = 0; i < n; i++) {
        sorted_n[i] = st->notes[i];
        sorted_v[i] = st->vels[i];
    }
    for (uint8_t i = 1u; i < n; i++) {
        uint8_t kn = sorted_n[i];
        uint8_t kv = sorted_v[i];
        int j = (int)i - 1;
        while (j >= 0 && sorted_n[j] > kn) {
            sorted_n[j + 1] = sorted_n[j];
            sorted_v[j + 1] = sorted_v[j];
            j--;
        }
        sorted_n[j + 1] = kn;
        sorted_v[j + 1] = kv;
    }
}

/* Compute next step's note(s), velocity, and ratchet count */
static uint8_t arp_resolve_step(adrift_group_t *gp,
                                uint32_t step_counter,
                                uint8_t *out_vel,
                                bool *out_is_chord) {
    adrift_arp_state_t *st = &gp->arp_state;
    const adrift_arp_params_t *ap = &gp->patch.arp;
    *out_is_chord = false;

    uint8_t n = st->num_notes;
    if (n == 0u) {
        *out_vel = 100u;
        return 60u;
    }

    uint8_t range = (uint8_t)dsp_clamp_i32(ap->range, 1, 4);
    uint8_t sorted_n[ADRIFT_ARP_MAX_NOTES];
    uint8_t sorted_v[ADRIFT_ARP_MAX_NOTES];

    if (ap->mode == ADRIFT_ARP_ORDER) {
        for (uint8_t i = 0; i < n; i++) {
            sorted_n[i] = st->notes[i];
            sorted_v[i] = st->vels[i];
        }
    } else {
        arp_sort_held_notes(st, sorted_n, sorted_v);
    }

    /* Expand across `range` octaves (max 16 * 4 = 64 notes) */
    uint8_t expanded_n[ADRIFT_ARP_MAX_NOTES * 4u];
    uint8_t expanded_v[ADRIFT_ARP_MAX_NOTES * 4u];
    uint32_t total = 0u;
    for (uint8_t oct = 0; oct < range; oct++) {
        int32_t shift = (int32_t)oct * 12;
        for (uint8_t i = 0; i < n; i++) {
            int32_t nn = dsp_clamp_i32((int32_t)sorted_n[i] + shift, 0, 127);
            expanded_n[total] = (uint8_t)nn;
            expanded_v[total] = sorted_v[i];
            total++;
        }
    }
    if (total == 0u) total = 1u;

    uint32_t idx = 0u;
    switch (ap->mode) {
        case ADRIFT_ARP_UP:
        case ADRIFT_ARP_ORDER:
        default:
            idx = step_counter % total;
            break;

        case ADRIFT_ARP_DOWN:
            idx = (total - 1u) - (step_counter % total);
            break;

        case ADRIFT_ARP_UP_DOWN: /* Exclusive endpoints: 0, 1, .., T-1, T-2, .., 1 */
            if (total <= 2u) {
                idx = step_counter % total;
            } else {
                uint32_t cycle = 2u * total - 2u;
                uint32_t pos   = step_counter % cycle;
                idx = (pos < total) ? pos : (cycle - pos);
            }
            break;

        case ADRIFT_ARP_DOWN_UP: /* Exclusive endpoints starting from top: T-1, T-2, .., 0, 1, .., T-2 */
            if (total <= 2u) {
                idx = (total - 1u) - (step_counter % total);
            } else {
                uint32_t cycle = 2u * total - 2u;
                uint32_t pos   = step_counter % cycle;
                uint32_t asc   = (pos < total) ? pos : (cycle - pos);
                idx = (total - 1u) - asc;
            }
            break;

        case ADRIFT_ARP_UP_AND_DOWN: /* Inclusive endpoints: 0, 1, .., T-1, T-1, .., 0 */
            {
                uint32_t cycle = 2u * total;
                uint32_t pos   = step_counter % cycle;
                idx = (pos < total) ? pos : (cycle - 1u - pos);
            }
            break;

        case ADRIFT_ARP_CONVERGE: /* Outside-in: 0, T-1, 1, T-2, ... */
            {
                uint32_t pos = step_counter % total;
                idx = ((pos & 1u) == 0u) ? (pos >> 1) : (total - 1u - (pos >> 1));
            }
            break;

        case ADRIFT_ARP_DIVERGE: /* Inside-out from center */
            {
                uint32_t pos = step_counter % total;
                uint32_t mid = (total - 1u) >> 1;
                if ((total & 1u) != 0u) {
                    if (pos == 0u) {
                        idx = mid;
                    } else if ((pos & 1u) != 0u) {
                        idx = mid + ((pos + 1u) >> 1);
                    } else {
                        idx = mid - (pos >> 1);
                    }
                } else {
                    idx = ((pos & 1u) == 0u) ? (mid - (pos >> 1)) : (mid + 1u + (pos >> 1));
                }
            }
            break;

        case ADRIFT_ARP_RANDOM:
            idx = arp_xorshift32(&st->rng) % total;
            break;

        case ADRIFT_ARP_CHORD:
            *out_is_chord = true;
            idx = step_counter % total;
            break;
    }

    st->step_idx = (uint8_t)(idx & 0x3Fu);
    uint8_t vel  = (ap->velocity > 0u) ? (uint8_t)dsp_clamp_i32(ap->velocity, 1, 127)
                                       : expanded_v[idx];
    *out_vel = vel;
    return expanded_n[idx];
}

static uint8_t arp_compute_ratchet_div(adrift_group_t *gp, uint32_t step_counter) {
    switch (gp->patch.arp.ratchet) {
        case ADRIFT_RATCHET_1X:    default: return 1u;
        case ADRIFT_RATCHET_2X:    return 2u;
        case ADRIFT_RATCHET_3X:    return 3u;
        case ADRIFT_RATCHET_4X:    return 4u;
        case ADRIFT_RATCHET_ALT_2: return ((step_counter & 1u) != 0u) ? 2u : 1u;
        case ADRIFT_RATCHET_ALT_3: return ((step_counter % 3u) == 2u) ? 3u : (((step_counter % 3u) == 1u) ? 2u : 1u);
        case ADRIFT_RATCHET_ALT_4: return ((step_counter & 3u) == 3u) ? 4u : (((step_counter & 1u) != 0u) ? 2u : 1u);
        case ADRIFT_RATCHET_RANDOM:
            return (uint8_t)((arp_xorshift32(&gp->arp_state.rng) & 3u) + 1u);
    }
}

/* Compute step length in samples for a 1/16th note at `rate_bpm` (or external 24 PPQN MIDI clock) with `swing` (0..100%).
 * Base 1/16th note = (44118 * 60) / (4 * bpm) = 661770 / bpm samples (or 6 * midi_clk_period).
 * Swing delays odd steps: even step = base * (200 + swing) / 200, odd step = base * (200 - swing) / 200.
 * At swing=100%, even:odd ratio is 300:100 (classic 75%/25% 3:1 shuffle) while keeping 2-step pair duration constant! */
static uint32_t arp_compute_step_samples(const adrift_synth_t *s,
                                         const adrift_group_t *gp,
                                         uint32_t step_counter) {
    uint32_t base;
    if (s->midi_clk_active && s->midi_clk_period_q4 > 0u) {
        base = (s->midi_clk_period_q4 * 6u + 8u) >> 4;
    } else {
        uint32_t bpm = (uint32_t)dsp_clamp_i32(gp->patch.arp.rate_bpm, 40, 240);
        base = 661770u / bpm;
    }
    uint32_t swing = (uint32_t)dsp_clamp_i32(gp->patch.arp.swing, 0, 100);
    if (swing == 0u) {
        return base;
    }
    uint32_t delta = (base * swing) / 200u;
    if ((step_counter & 1u) == 0u) {
        return base + delta;
    }
    return (base > delta + 64u) ? (base - delta) : 64u;
}

static void arp_trigger_substep(adrift_synth_t *s, uint8_t g) {
    adrift_group_t *gp = &s->groups[g];
    adrift_arp_state_t *st = &gp->arp_state;
    const adrift_arp_params_t *ap = &gp->patch.arp;

    arp_stop_sounding_notes(s, g);
    if (st->num_notes == 0u || ap->mode == ADRIFT_ARP_OFF || gp->num_voices == 0u) {
        return;
    }

    if (st->substep_idx == 0u) {
        bool is_chord = false;
        uint8_t vel   = 100u;
        uint8_t note  = arp_resolve_step(gp, st->step_counter, &vel, &is_chord);
        uint8_t rdiv  = arp_compute_ratchet_div(gp, st->step_counter);
        uint32_t full_step = arp_compute_step_samples(s, gp, st->step_counter);
        uint32_t sub_len   = full_step / rdiv;
        if (sub_len < 64u) sub_len = 64u;
        uint32_t gate_pct  = (uint32_t)dsp_clamp_i32(ap->gate, 5, 100);
        uint32_t gate_len  = (sub_len * gate_pct) / 100u;
        if (gate_len < 1u) gate_len = 1u;
        if (gate_len > sub_len) gate_len = sub_len;

        st->substep_total        = rdiv;
        st->substep_len_samples  = sub_len;
        st->substep_gate_samples = gate_len;
        st->cur_step_note        = note;
        st->cur_step_vel         = vel;
        st->step_counter++;
    }

    if (ap->mode == ADRIFT_ARP_CHORD) {
        uint8_t sorted_n[ADRIFT_ARP_MAX_NOTES];
        uint8_t sorted_v[ADRIFT_ARP_MAX_NOTES];
        arp_sort_held_notes(st, sorted_n, sorted_v);
        uint8_t oct_shift = 0u;
        uint8_t range = (uint8_t)dsp_clamp_i32(ap->range, 1, 4);
        if (range > 1u) {
            oct_shift = (uint8_t)(((st->step_counter - 1u) % range) * 12u);
        }
        st->sounding_count = 0u;
        for (uint8_t i = 0; i < st->num_notes; i++) {
            uint8_t nn = (uint8_t)dsp_clamp_i32((int32_t)sorted_n[i] + (int32_t)oct_shift, 0, 127);
            uint8_t vv = (ap->velocity > 0u) ? (uint8_t)dsp_clamp_i32(ap->velocity, 1, 127)
                                             : sorted_v[i];
            adrift_synth_voice_note_on(s, g, nn, vv);
            if (st->sounding_count < ADRIFT_ARP_MAX_NOTES) {
                st->sounding_notes[st->sounding_count++] = nn;
            }
        }
    } else {
        adrift_synth_voice_note_on(s, g, st->cur_step_note, st->cur_step_vel);
        st->sounding_notes[0] = st->cur_step_note;
        st->sounding_count    = 1u;
    }

    st->gate_open     = true;
    st->phase_samples = 0u;
}

uint32_t adrift_arp_samples_until_next_event(const adrift_synth_t *s) {
    uint32_t min_rem = 0xFFFFFFFFu;
    for (uint8_t g = 0; g < ADRIFT_MAX_GROUPS; g++) {
        const adrift_group_t *gp = &s->groups[g];
        const adrift_arp_state_t *st = &gp->arp_state;
        if (gp->num_voices == 0u || gp->patch.arp.mode == ADRIFT_ARP_OFF || st->num_notes == 0u) {
            continue;
        }
        if (st->substep_len_samples == 0u) {
            return 0u;
        }
        if (st->gate_open && st->phase_samples < st->substep_gate_samples &&
            st->substep_gate_samples < st->substep_len_samples) {
            uint32_t rem_gate = st->substep_gate_samples - st->phase_samples;
            if (rem_gate < min_rem) min_rem = rem_gate;
        }
        if (st->phase_samples < st->substep_len_samples) {
            uint32_t rem_step = st->substep_len_samples - st->phase_samples;
            if (rem_step < min_rem) min_rem = rem_step;
        } else {
            return 0u;
        }
    }
    return min_rem;
}

void adrift_arp_advance_samples(adrift_synth_t *s, uint32_t frames) {
    for (uint8_t g = 0; g < ADRIFT_MAX_GROUPS; g++) {
        adrift_group_t *gp = &s->groups[g];
        adrift_arp_state_t *st = &gp->arp_state;
        if (gp->num_voices == 0u || gp->patch.arp.mode == ADRIFT_ARP_OFF || st->num_notes == 0u) {
            continue;
        }
        st->phase_samples += frames;
        if (st->gate_open && st->substep_gate_samples < st->substep_len_samples &&
            st->phase_samples >= st->substep_gate_samples) {
            arp_stop_sounding_notes(s, g);
        }
        if (st->phase_samples >= st->substep_len_samples) {
            st->substep_idx++;
            if (st->substep_idx >= st->substep_total) {
                st->substep_idx = 0u;
            }
            arp_trigger_substep(s, g);
        }
    }
}

void adrift_synth_handle_midi_realtime(adrift_synth_t *s, uint8_t status) {
    switch (status) {
        case 0xFAu: /* MIDI Start — reset 24 PPQN pulse & arpeggiator step counters to beat 1 */
            s->midi_clk_running         = true;
            s->midi_clk_active          = true;
            s->midi_clk_pulse_cnt       = 0u;
            s->midi_clk_since_last      = 0u;
            s->midi_clk_timeout_samples = ADRIFT_SAMPLE_RATE * 2u;
            for (uint8_t g = 0; g < ADRIFT_MAX_GROUPS; g++) {
                adrift_group_t *gp = &s->groups[g];
                adrift_arp_state_t *st = &gp->arp_state;
                if (gp->num_voices > 0u && gp->patch.arp.mode != ADRIFT_ARP_OFF && st->num_notes > 0u) {
                    st->step_counter        = 0u;
                    st->substep_idx         = 0u;
                    st->phase_samples       = 0u;
                    st->substep_len_samples = 0u;
                }
            }
            break;

        case 0xFBu: /* MIDI Continue — resume without resetting step position */
            s->midi_clk_running         = true;
            s->midi_clk_active          = true;
            s->midi_clk_since_last      = 0u;
            s->midi_clk_timeout_samples = ADRIFT_SAMPLE_RATE * 2u;
            break;

        case 0xFCu: /* MIDI Stop — release external clock lock */
            s->midi_clk_running         = false;
            s->midi_clk_active          = false;
            s->midi_clk_pulse_cnt       = 0u;
            s->midi_clk_since_last      = 0u;
            s->midi_clk_timeout_samples = 0u;
            break;

        case 0xF8u: { /* MIDI Timing Clock (24 PPQN = 6 pulses per 1/16th note) */
            uint32_t dt = s->midi_clk_since_last;
            bool newly_measured = false;
            /* Valid 24 PPQN inter-pulse interval: 22 BPM .. 440 BPM (~180 .. 5000 samples @ 44.1 kHz) */
            if (dt >= 180u && dt <= 5000u) {
                uint32_t dt_q4 = dt << 4;
                if (!s->midi_clk_active || s->midi_clk_period_q4 == 0u) {
                    s->midi_clk_period_q4 = dt_q4;
                    newly_measured        = true;
                } else {
                    uint32_t cur_dt = s->midi_clk_period_q4 >> 4;
                    uint32_t diff   = (dt > cur_dt) ? (dt - cur_dt) : (cur_dt - dt);
                    if (diff <= (cur_dt >> 2)) {
                        s->midi_clk_period_q4 = (s->midi_clk_period_q4 * 3u + dt_q4 + 2u) >> 2;
                    } else {
                        s->midi_clk_period_q4 = dt_q4;
                        newly_measured        = true;
                    }
                }
                uint32_t step_16th = (s->midi_clk_period_q4 * 6u + 8u) >> 4;
                if (step_16th > 0u) {
                    uint32_t bpm = (661770u + (step_16th >> 1)) / step_16th;
                    s->midi_clk_bpm = (uint16_t)dsp_clamp_i32((int32_t)bpm, 30, 300);
                }
            }

            s->midi_clk_since_last      = 0u;
            s->midi_clk_active          = true;
            s->midi_clk_timeout_samples = ADRIFT_SAMPLE_RATE * 2u;
            uint32_t pulse              = s->midi_clk_pulse_cnt++;

            for (uint8_t g = 0; g < ADRIFT_MAX_GROUPS; g++) {
                adrift_group_t *gp = &s->groups[g];
                adrift_arp_state_t *st = &gp->arp_state;
                if (gp->num_voices == 0u || gp->patch.arp.mode == ADRIFT_ARP_OFF || st->num_notes == 0u) {
                    continue;
                }

                /* Rescale current substep & gate duration when the external period first locks */
                if (newly_measured && st->substep_len_samples > 0u) {
                    uint32_t prev_step_idx = (st->step_counter > 0u) ? (st->step_counter - 1u) : 0u;
                    uint32_t full_step     = arp_compute_step_samples(s, gp, prev_step_idx);
                    uint8_t  rdiv          = st->substep_total ? st->substep_total : 1u;
                    uint32_t sub_len       = full_step / rdiv;
                    if (sub_len < 64u) sub_len = 64u;
                    uint32_t gate_pct      = (uint32_t)dsp_clamp_i32(gp->patch.arp.gate, 5, 100);
                    uint32_t gate_len      = (sub_len * gate_pct) / 100u;
                    if (gate_len < 1u) gate_len = 1u;
                    if (gate_len > sub_len) gate_len = sub_len;
                    st->substep_len_samples  = sub_len;
                    st->substep_gate_samples = gate_len;
                }

                /* Phase-lock 1/16th steps on every 6th pulse (or every 12th pulse when Swing > 0) */
                if ((pulse % 6u) == 0u) {
                    uint32_t clk_step = pulse / 6u;
                    if (pulse == 0u || st->substep_len_samples == 0u) {
                        st->step_counter = 0u;
                        st->substep_idx  = 0u;
                        arp_trigger_substep(s, g);
                    } else if (gp->patch.arp.swing == 0u || (clk_step & 1u) == 0u) {
                        if (st->substep_idx + 1u >= st->substep_total &&
                            st->phase_samples >= (st->substep_len_samples >> 1)) {
                            st->substep_idx = 0u;
                            arp_trigger_substep(s, g);
                        }
                    }
                }
            }
            break;
        }

        default:
            break;
    }
}

void adrift_synth_sync_group_arp(adrift_synth_t *s, uint8_t g) {
    if (g >= ADRIFT_MAX_GROUPS) return;
    adrift_group_t *gp = &s->groups[g];
    adrift_arp_state_t *st = &gp->arp_state;

    if (gp->patch.arp.mode == ADRIFT_ARP_OFF) {
        if (st->sounding_count > 0u || st->num_notes > 0u) {
            arp_stop_sounding_notes(s, g);
            st->num_notes  = 0u;
            st->phys_count = 0u;
        }
        return;
    }

    /* If Latch & Sustain were turned off and no physical keys are held, stop latched notes */
    if (!gp->patch.arp.latch && !gp->sustain_pedal) {
        uint8_t w = 0u;
        for (uint8_t i = 0; i < st->num_notes; i++) {
            if (st->phys_down[i]) {
                st->notes[w]     = st->notes[i];
                st->vels[w]      = st->vels[i];
                st->phys_down[w] = true;
                w++;
            }
        }
        st->num_notes  = w;
        st->phys_count = w;
        if (st->num_notes == 0u) {
            arp_stop_sounding_notes(s, g);
            st->step_counter        = 0u;
            st->substep_idx         = 0u;
            st->substep_len_samples = 0u;
        }
    }
}

void adrift_synth_set_group_arp_mode(adrift_synth_t *s, uint8_t g, adrift_arp_mode_t mode) {
    if (g >= ADRIFT_MAX_GROUPS) return;
    if ((uint8_t)mode >= (uint8_t)ADRIFT_ARP_MODE_COUNT) {
        mode = ADRIFT_ARP_OFF;
    }
    adrift_group_t *gp = &s->groups[g];
    adrift_arp_mode_t prev = gp->patch.arp.mode;
    if (prev == mode) return;

    if (mode == ADRIFT_ARP_OFF) {
        gp->patch.arp.mode = ADRIFT_ARP_OFF;
        adrift_synth_sync_group_arp(s, g);
        return;
    }

    /* Switching from OFF -> active arp mode: transfer any currently held voices into arp buffer */
    if (prev == ADRIFT_ARP_OFF) {
        adrift_arp_state_t *st = &gp->arp_state;
        adrift_arp_reset_state(st, g);
        for (uint8_t v = 0; v < ADRIFT_MAX_VOICES; v++) {
            adrift_voice_t *vc = &s->voices[v];
            if (vc->owner_group == (int8_t)g && vc->active && (vc->key_down || vc->sustained)) {
                bool dup = false;
                for (uint8_t i = 0; i < st->num_notes; i++) {
                    if (st->notes[i] == vc->note) {
                        dup = true;
                        break;
                    }
                }
                if (!dup && st->num_notes < ADRIFT_ARP_MAX_NOTES) {
                    st->notes[st->num_notes]     = vc->note;
                    st->vels[st->num_notes]      = vc->velocity;
                    st->phys_down[st->num_notes] = vc->key_down;
                    if (vc->key_down) st->phys_count++;
                    st->num_notes++;
                }
                vc->key_down  = false;
                vc->sustained = false;
                adrift_env_note_off(&vc->env1, &gp->patch.env1);
                adrift_env_note_off(&vc->env2, &gp->patch.env2);
            }
        }
        gp->mono_len       = 0u;
        gp->patch.arp.mode = mode;
        if (st->num_notes > 0u) {
            arp_trigger_substep(s, g);
        }
        return;
    }

    gp->patch.arp.mode = mode;
}

void adrift_synth_group_note_on(adrift_synth_t *s, uint8_t g, uint8_t note, uint8_t velocity) {
    if (g >= ADRIFT_MAX_GROUPS) return;
    note &= 0x7Fu;
    velocity &= 0x7Fu;
    if (velocity == 0u) {
        adrift_synth_group_note_off(s, g, note);
        return;
    }

    adrift_group_t *gp = &s->groups[g];
    if (gp->num_voices == 0u) return;

    if (gp->patch.arp.mode == ADRIFT_ARP_OFF) {
        adrift_synth_voice_note_on(s, g, note, velocity);
        return;
    }

    adrift_arp_state_t *st = &gp->arp_state;
    bool hold_active = gp->patch.arp.latch || gp->sustain_pedal;

    /* In Latch mode, pressing a new key after releasing all physical keys starts a fresh chord */
    if (hold_active && st->phys_count == 0u && st->num_notes > 0u) {
        arp_stop_sounding_notes(s, g);
        st->num_notes           = 0u;
        st->step_counter        = 0u;
        st->substep_idx         = 0u;
        st->substep_len_samples = 0u;
    }

    bool was_empty = (st->num_notes == 0u);

    /* Update existing note in buffer if already present */
    for (uint8_t i = 0; i < st->num_notes; i++) {
        if (st->notes[i] == note) {
            st->vels[i] = velocity;
            if (!st->phys_down[i]) {
                st->phys_down[i] = true;
                st->phys_count++;
            }
            return;
        }
    }

    if (st->num_notes < ADRIFT_ARP_MAX_NOTES) {
        st->notes[st->num_notes]     = note;
        st->vels[st->num_notes]      = velocity;
        st->phys_down[st->num_notes] = true;
        st->num_notes++;
        st->phys_count++;
    } else {
        if (st->phys_down[0] && st->phys_count > 0u) {
            st->phys_count--;
        }
        for (uint8_t i = 0; i + 1u < ADRIFT_ARP_MAX_NOTES; i++) {
            st->notes[i]     = st->notes[i + 1u];
            st->vels[i]      = st->vels[i + 1u];
            st->phys_down[i] = st->phys_down[i + 1u];
        }
        st->notes[ADRIFT_ARP_MAX_NOTES - 1u]     = note;
        st->vels[ADRIFT_ARP_MAX_NOTES - 1u]      = velocity;
        st->phys_down[ADRIFT_ARP_MAX_NOTES - 1u] = true;
        st->phys_count++;
    }

    if (was_empty || (st->phase_samples == 0u && st->step_counter == 1u && st->substep_idx == 0u)) {
        st->step_counter = 0u;
        st->substep_idx  = 0u;
        arp_trigger_substep(s, g);
    } else if (gp->patch.arp.mode == ADRIFT_ARP_CHORD && st->gate_open) {
        /* If playing a chord and a new chord note is added while gate is open, trigger it immediately */
        uint8_t vv = (gp->patch.arp.velocity > 0u)
                         ? (uint8_t)dsp_clamp_i32(gp->patch.arp.velocity, 1, 127)
                         : velocity;
        adrift_synth_voice_note_on(s, g, note, vv);
        if (st->sounding_count < ADRIFT_ARP_MAX_NOTES) {
            st->sounding_notes[st->sounding_count++] = note;
        }
    }
}

void adrift_synth_group_note_off(adrift_synth_t *s, uint8_t g, uint8_t note) {
    if (g >= ADRIFT_MAX_GROUPS) return;
    note &= 0x7Fu;
    adrift_group_t *gp = &s->groups[g];
    if (gp->num_voices == 0u) return;

    if (gp->patch.arp.mode == ADRIFT_ARP_OFF) {
        adrift_synth_voice_note_off(s, g, note);
        return;
    }

    adrift_arp_state_t *st = &gp->arp_state;
    bool hold_active = gp->patch.arp.latch || gp->sustain_pedal;

    for (uint8_t i = 0; i < st->num_notes; i++) {
        if (st->notes[i] == note) {
            if (st->phys_down[i]) {
                st->phys_down[i] = false;
                if (st->phys_count > 0u) st->phys_count--;
            }
            if (!hold_active) {
                for (uint8_t j = i; j + 1u < st->num_notes; j++) {
                    st->notes[j]     = st->notes[j + 1u];
                    st->vels[j]      = st->vels[j + 1u];
                    st->phys_down[j] = st->phys_down[j + 1u];
                }
                st->num_notes--;
            }
            break;
        }
    }

    if (st->num_notes == 0u) {
        arp_stop_sounding_notes(s, g);
        st->step_counter        = 0u;
        st->substep_idx         = 0u;
        st->substep_len_samples = 0u;
    }
}
