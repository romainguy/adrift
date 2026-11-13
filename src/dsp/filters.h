#pragma once

#include <stdint.h>
#include "dsp_math.h"

/*
 * 12 dB/oct Cytomic Zero-Delay-Feedback TPT State-Variable Low-Pass Filter.
 * Cutoff range: 32.7 Hz (C1) .. 21 kHz.
 */
typedef struct {
    int32_t ic1eq_q20;
    int32_t ic2eq_q20;
    int32_t a1_q28;
    int32_t a2_q28;
    int32_t a3_q28;
    int16_t last_cutoff;
    int16_t last_res;
} adrift_svf_lpf12_t;

void adrift_svf_init(adrift_svf_lpf12_t *f);
void adrift_svf_reset(adrift_svf_lpf12_t *f);
void adrift_svf_set_params(adrift_svf_lpf12_t *f, int32_t cutoff_1023, int32_t res_1023);
void adrift_svf_process_lpf(adrift_svf_lpf12_t *f, const int32_t *in_q15, int32_t *out_q15, uint32_t frames);

/*
 * 6 dB/oct 1-Pole Low-Pass Filter with exact discrete-time phase-delay helper
 * for BBD feedback damping & BBD clock anti-imaging.
 */
typedef struct {
    int32_t  state_q29; /* Q29 state (14 sub-LSB fractional bits; zero deadzone) */
    uint16_t a_q15;     /* 1..32767 */
} adrift_onepole_lpf6_t;

void adrift_onepole_init(adrift_onepole_lpf6_t *f);
void adrift_onepole_reset(adrift_onepole_lpf6_t *f);
void adrift_onepole_set_cutoff(adrift_onepole_lpf6_t *f, int32_t cutoff_1023);
void adrift_onepole_set_alpha(adrift_onepole_lpf6_t *f, uint16_t a_q15);

DSP_ALWAYS_INLINE int32_t adrift_onepole_tick(adrift_onepole_lpf6_t *f, int32_t x_q15) {
    int32_t x_q29   = (int32_t)dsp_sat16(x_q15) * 16384;
    int32_t diff    = x_q29 - f->state_q29;
    int32_t a       = (int32_t)f->a_q15;
    int32_t step_hi = (diff >> 15) * a;
    int32_t step_lo = ((diff & 0x7FFF) * a) >> 15;
    f->state_q29   += step_hi + step_lo;
    return (f->state_q29 + 8192) >> 14;
}

/* Exact discrete phase delay (-arg(H(e^{jw})) / w) in Q16 samples at fundamental phase_inc */
uint32_t adrift_onepole_phase_delay_q16(uint16_t a_q15, uint32_t f0_phase_inc);
