#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "dsp_math.h"
#include "filters.h"

#define ADRIFT_BBD_BUF_LEN  2048u /* Power of 2 (mask 0x7FF) -> 4 KB SRAM per voice */
#define ADRIFT_BBD_BUF_MASK (ADRIFT_BBD_BUF_LEN - 1u)

typedef struct {
    int16_t feedback;          /* 0..1023 -> 0% .. 115% loop gain (890 ~= 100.0% self-osc threshold) */
    int16_t fb_lpf_cutoff;     /* 0..1023 -> 32.7 Hz .. 21 kHz 6 dB/oct damping filter */
    int16_t glide;             /* 0..1023 -> portamento / BBD clock slew time */
    bool    invert;            /* Feedback polarity x -1 (odd harmonics at f0/2) */
    bool    invert_pitch_comp; /* Optional +1 oct delay halve when invert is active */
    int16_t bbd_noise;         /* 0..1023 -> analog noise floor intensity (default ~200 = -78 dBFS) */
    int16_t dispersion;        /* 0..1023 -> loop all-pass dispersion / stiffness inharmonicity */
} adrift_bbd_params_t;

typedef struct {
    int16_t               buf[ADRIFT_BBD_BUF_LEN];
    uint16_t              write_idx;
    int32_t               delay_q16;        /* Current smoothed fractional delay (samples * 65536) */
    int32_t               target_delay_q16; /* Pitch + group-delay compensated delay target */
    int32_t               slew_pitch_q16;   /* Portamento pitch tracker (pitch_q8 << 8) */
    int32_t               clk_drift_q16;    /* MN3101-style Ornstein-Uhlenbeck BBD clock wander */
    int32_t               prev_fb_q15;      /* Previous AC feedback sample for MOSFET C(V) dispersion */
    bool                  pitch_initialized;
    adrift_onepole_lpf6_t fb_lpf;           /* 6 dB/oct feedback damping filter */
    adrift_onepole_lpf6_t aa_lpf;           /* 1024-stage BBD input anti-aliasing filter */
    adrift_onepole_lpf6_t clk_lpf;          /* 1024-stage BBD output reconstruction filter */
    int32_t               disp_x1;          /* All-pass dispersion stage 1 input history (Q23) */
    int32_t               disp_y1;          /* All-pass dispersion stage 1 output history (Q23) */
    int32_t               disp_x2;          /* All-pass dispersion stage 2 input history (Q23) */
    int32_t               disp_y2;          /* All-pass dispersion stage 2 output history (Q23) */
    uint16_t              disp_r_q15;       /* All-pass pole radius r in Q15 (0 = bypass) */
    int32_t               dc_cap_q23;       /* 0.43 Hz loop coupling capacitor preventing DC latch-up */
    int32_t               fb_gain_q14;      /* Smoothed feedback gain (16384 = 100.0%) */
    uint32_t              noise_rng;        /* Thermal/clock noise seed for organic self-oscillation */
    int32_t               peak_wet_q15;     /* Peak wet level for UI BBD energy meter */
    bool                  delay_cache_valid;
    int32_t               last_delay_pitch_q16;
    int32_t               last_sounding_pitch_q16;
    int16_t               last_fb_cut;
    int16_t               last_disp_k;
} adrift_bbd_t;

void adrift_bbd_init(adrift_bbd_t *bbd, uint8_t voice_idx);
void adrift_bbd_reset(adrift_bbd_t *bbd);

/* Frame-invariant portamento pitch stepper (returns rounded Q8 pitch; retains Q16 sub-cent state) */
int32_t adrift_bbd_step_glide(adrift_bbd_t *bbd,
                              int16_t glide,
                              int32_t target_note_pitch_q8,
                              uint32_t frames);

/* Control-rate update of pitch target, filter cutoffs, and analytical phase-delay compensation */
void adrift_bbd_prepare_block_ex(adrift_bbd_t *bbd,
                                 const adrift_bbd_params_t *p,
                                 int32_t target_note_pitch_q8,
                                 int32_t mod_pitch_q8,
                                 int32_t mod_fb_1023,
                                 int32_t mod_fb_cutoff_1023,
                                 uint32_t frames,
                                 int32_t *out_target_fb_q14,
                                 bool step_glide);

void adrift_bbd_prepare_block(adrift_bbd_t *bbd,
                              const adrift_bbd_params_t *p,
                              int32_t target_note_pitch_q8,
                              int32_t mod_pitch_q8,
                              int32_t mod_fb_1023,
                              int32_t mod_fb_cutoff_1023,
                              uint32_t frames,
                              int32_t *out_target_fb_q14);

/* Tight 32-bit fixed-point audio loop: reads `in_q15` (post-Input-LPF), writes `wet_out_q15` */
void adrift_bbd_process(adrift_bbd_t *bbd,
                        const adrift_bbd_params_t *p,
                        int32_t target_fb_q14,
                        const int32_t *in_q15,
                        int32_t *wet_out_q15,
                        uint32_t frames);
