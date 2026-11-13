#ifndef ADRIFT_SYNTH_INTERNAL_H
#define ADRIFT_SYNTH_INTERNAL_H

#include "synth.h"
#include "../platform/platform.h"

/* Constant-power unison gain scaling (Q15) for 1..16 stacked voices in a group */
extern const int32_t k_unison_gain_q15[ADRIFT_MAX_VOICES + 1];

/* Voice pool & allocation helpers (voice.c) */
void adrift_synth_reset_voice_dsp(adrift_voice_t *vc);
void adrift_synth_recompute_group_slots(adrift_synth_t *s);
void adrift_synth_refresh_any_gate(adrift_synth_t *s);
void adrift_synth_voice_note_on(adrift_synth_t *s, uint8_t g, uint8_t note, uint8_t velocity);
void adrift_synth_voice_note_off(adrift_synth_t *s, uint8_t g, uint8_t note);
void adrift_synth_compute_stack_slot_voicing(const adrift_group_t *gp,
                                             uint8_t slot,
                                             int32_t spread_q8,
                                             int32_t *out_semi_q8,
                                             int32_t *out_detune_q8,
                                             int32_t *out_slot_gain_q15);
void adrift_synth_compute_voice_pan_gains_q15(const adrift_group_t *gp,
                                              uint8_t slot,
                                              int32_t *out_pan_l_q15,
                                              int32_t *out_pan_r_q15);
void adrift_synth_compute_voice_pan_gains_mod_q15(const adrift_group_t *gp,
                                                  uint8_t slot,
                                                  int32_t mod_pan,
                                                  int32_t mod_spread,
                                                  int32_t *out_pan_l_q15,
                                                  int32_t *out_pan_r_q15);
void adrift_synth_get_voice_pan_gains_cached_q15(adrift_voice_t *vc,
                                                 const adrift_group_t *gp,
                                                 int32_t mod_pan,
                                                 int32_t mod_spread,
                                                 int32_t *out_pan_l_q15,
                                                 int32_t *out_pan_r_q15);

/* Arpeggiator helpers (arp.c) */
void     adrift_arp_reset_state(adrift_arp_state_t *st, uint8_t g);
uint32_t adrift_arp_samples_until_next_event(const adrift_synth_t *s);
void     adrift_arp_advance_samples(adrift_synth_t *s, uint32_t frames);

#endif /* ADRIFT_SYNTH_INTERNAL_H */
