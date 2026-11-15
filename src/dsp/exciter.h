#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "dsp_math.h"

typedef struct {
    /* VCO & Exciter Stage parameters */
    int16_t wave_morph;   /* 0..1023: Sine -> Tri -> Ramp -> Pulse */
    int16_t pulse_width;  /* 0..1023 -> 5%..95% duty cycle */
    int16_t vco_level;    /* 0..1023: Overall exciter level (wave + noise combined) */
    int8_t  vco_octave;   /* -2..+2 */
    int8_t  vco_semi;     /* -12..+12 */
    int16_t vco_fine;     /* -100..+100 cents (mapped to pitch_q8) */
    int16_t analog_drift; /* 0..1023 Micro-ACB drift & sag intensity */
    bool    vco_keytrack; /* true: tracks note pitch, false: fixed C4 ref */

    /* Wave / Noise Mix & Sample & Hold parameters */
    int16_t noise_level;  /* 0..1023: Wave/Noise mix (0 = 100% wave, 1023 = 100% noise) */
    int16_t sh_rate;      /* 0..1023: 1023 = pure white noise, <1000 = stepped S&H (20 Hz..22 kHz) */

    /* Physical Pluck / Mallet Impulse, Note-On Phase Lock & -1 Oct Sub-Oscillator */
    int16_t pluck;        /* 0..1023: 0 = free-running VCO, >0 = phase-locked attack + pick impulse + -1 oct sub */
} adrift_exciter_params_t;

typedef struct {
    uint8_t  voice_idx;
    uint32_t vco_phase;
    uint32_t vco_inc;
    uint32_t inv_dt16;      /* (1 << 30) / max(1, vco_inc >> 16) for division-free PolyBLEP */
    uint32_t pw_thresh;     /* Q32 comparator threshold */
    uint32_t sub_phase;     /* -1 octave sub-oscillator phase */
    uint32_t impulse_phase; /* Single-cycle mechanical pick/mallet impulse phase (0 = inactive) */
    int32_t  impulse_amp_q15; /* Velocity- and pluck-scaled transient gain */

    /* Micro-ACB analog component state */
    int16_t  trim_pitch_q8; /* Per-voice V/Oct tolerance */
    int16_t  trim_sag_q15;  /* Per-voice timing capacitor sag */
    int16_t  trim_sym_q15;  /* Per-voice transistor sine-folder asymmetry */
    int16_t  trim_pw_q15;   /* Per-voice comparator offset */

    int32_t  drift_pitch_q16; /* Ornstein-Uhlenbeck thermal pitch wander */
    int32_t  drift_pw_q15;    /* Slow comparator threshold wander */
    int32_t  ac_cap_q23;      /* ~15 Hz output AC-coupling capacitor state */
    int32_t  hf_slew_q15;     /* Subtle thyristor reset softening */

    /* Noise & S&H state */
    uint32_t rng_state;
    uint32_t sh_phase;
    uint32_t sh_inc;
    uint32_t sh_inv_dt16;
    int32_t  sh_prev_q15;
    int32_t  sh_held_q15;
    int32_t  sh_next_q15;

    /* Smoothed mixer gains */
    int32_t  vco_gain_q15;
    int32_t  noise_gain_q15;

    /* Control-rate caches */
    int32_t  last_pitch_q16;
    int16_t  last_sh_rate;
    uint8_t  pending_phase_lock;
    uint8_t  pending_uni_slot;
} adrift_exciter_t;

void adrift_exciter_init(adrift_exciter_t *exc, uint8_t voice_idx);

/* Note-On trigger: phase-locks VCO/Sub-Osc and arms the mechanical pick/mallet impulse when p->pluck > 0 */
void adrift_exciter_trigger_ex(adrift_exciter_t *exc,
                               const adrift_exciter_params_t *p,
                               uint8_t velocity,
                               bool legato,
                               uint8_t unison_slot,
                               bool exc_audible,
                               bool delayed_onset);

void adrift_exciter_trigger(adrift_exciter_t *exc,
                            const adrift_exciter_params_t *p,
                            uint8_t velocity,
                            bool legato,
                            uint8_t unison_slot);

/* Control-rate update with Q16 sub-cent base pitch (called once per sub-block) */
void adrift_exciter_prepare_block_q16(adrift_exciter_t *exc,
                                      const adrift_exciter_params_t *p,
                                      int32_t base_note_pitch_q16,
                                      int32_t mod_pitch_q8,
                                      int32_t mod_wave_1023,
                                      int32_t mod_pw_1023,
                                      int16_t *out_morph_1023);

/* Control-rate update (called once per 64-sample block) */
void adrift_exciter_prepare_block(adrift_exciter_t *exc,
                                  const adrift_exciter_params_t *p,
                                  int32_t base_note_pitch_q8,
                                  int32_t mod_pitch_q8,
                                  int32_t mod_wave_1023,
                                  int32_t mod_pw_1023,
                                  int16_t *out_morph_1023);

/* Audio-rate render of N samples into out_q15 (before Env 1 VCA) */
void adrift_exciter_process(adrift_exciter_t *exc,
                            const adrift_exciter_params_t *p,
                            int16_t morph_1023,
                            int32_t *out_q15,
                            uint32_t frames);
