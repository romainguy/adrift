/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Clean-room fixed-point Klatt (1980) cascade/parallel formant synthesizer
 * ("Software for a cascade/parallel formant synthesizer", JASA 67(3), 971-995).
 *
 * Synthesizes speech internally at 11,029.5 Hz (ADRIFT_SAMPLE_RATE / 4) and
 * upsamples 4x with linear interpolation + 2-pole Butterworth anti-imaging
 * filtering to Adrift's native 44,118 Hz output sample rate.
 *
 * 100% freestanding C99, zero floating-point, zero 64-bit integer math.
 */

#define ADRIFT_KLATT_SR           11030
#define ADRIFT_KLATT_UPSAMPLE     4u
#define ADRIFT_KLATT_FIFO_FRAMES  512u
#define ADRIFT_KLATT_FIFO_MASK    (ADRIFT_KLATT_FIFO_FRAMES - 1u)
#define ADRIFT_KLATT_TAIL_SAMPLES 256u

/* Single 5 ms (nominal) Klatt synthesis frame parameter vector */
typedef struct {
    uint16_t f0_hz10; /* Fundamental frequency in 0.1 Hz (e.g. 1250 = 125.0 Hz; 0 = unvoiced) */
    uint16_t f1;      /* Formant 1 frequency (Hz, ~180..1100) */
    uint16_t f2;      /* Formant 2 frequency (Hz, ~550..2800) */
    uint16_t f3;      /* Formant 3 frequency (Hz, ~1200..3600) */
    uint16_t b1;      /* Formant 1 bandwidth (Hz, ~40..500) */
    uint16_t b2;      /* Formant 2 bandwidth (Hz, ~50..500) */
    uint16_t b3;      /* Formant 3 bandwidth (Hz, ~60..600) */
    uint16_t fnz;     /* Nasal antiresonator zero frequency (Hz; 270 = neutral/canceled by fnp) */
    uint8_t  av;      /* Normal voicing amplitude (dB, 0..70) */
    uint8_t  avs;     /* Quasi-sinusoidal voicing amplitude (dB, 0..70) */
    uint8_t  ah;      /* Aspiration noise amplitude into cascade branch (dB, 0..70) */
    uint8_t  af;      /* Frication noise amplitude into parallel branch (dB, 0..70) */
    uint8_t  a2;      /* Parallel F2 amplitude (dB, 0..70) */
    uint8_t  a3;      /* Parallel F3 amplitude (dB, 0..70) */
    uint8_t  a4;      /* Parallel F4 amplitude (dB, 0..70) */
    uint8_t  a5;      /* Parallel F5 amplitude (dB, 0..70) */
    uint8_t  a6;      /* Parallel F6 amplitude (dB, 0..70) */
    uint8_t  ab;      /* Parallel bypass amplitude (dB, 0..70) */
    uint8_t  sw_par;  /* 0 = cascade + parallel frication, 1 = all-parallel */
    uint8_t  dur_mul; /* Frame duration multiplier in Q4 (16 = 1.0x normal 1 frame) */
} adrift_klatt_frame_t;

typedef struct {
    int32_t a_q14;
    int32_t b_q14;
    int32_t c_q14;
    int32_t inv_a_q10;
    int32_t y1;
    int32_t y2;
} adrift_klatt_res_t;

typedef struct {
    /* Glottal source & vocal-tract resonators (Klatt 1980 Fig. 6) */
    adrift_klatt_res_t rgp;
    adrift_klatt_res_t rgz;
    adrift_klatt_res_t rgs;
    adrift_klatt_res_t rnz;
    adrift_klatt_res_t rnp;
    adrift_klatt_res_t r1;
    adrift_klatt_res_t r2;
    adrift_klatt_res_t r3;
    adrift_klatt_res_t r4;
    adrift_klatt_res_t r5;
    adrift_klatt_res_t r2p;
    adrift_klatt_res_t r3p;
    adrift_klatt_res_t r4p;
    adrift_klatt_res_t r5p;
    adrift_klatt_res_t r6p;

    /* Glottal period, radiation & plosive step state */
    uint32_t rng;
    uint16_t nper;
    uint16_t t0;
    uint16_t flutter_phase;
    int32_t  glottal_prev;
    int32_t  fric_prev;
    int32_t  plstep;
    uint8_t  prev_af;
    uint8_t  prev_ab;

    /* Current active frame linear amplitudes & samples remaining */
    int32_t  amp_av;
    int32_t  amp_avs;
    int32_t  amp_ah;
    int32_t  amp_af;
    int32_t  amp_a2;
    int32_t  amp_a3;
    int32_t  amp_a4;
    int32_t  amp_a5;
    int32_t  amp_a6;
    int32_t  amp_ab;
    uint16_t cur_f0_hz10;
    uint8_t  cur_sw_par;
    uint8_t  speed; /* 1..5, default 3 */
    uint16_t frame_rem;

    /* 4x upsampler (11,029.5 Hz -> 44,118 Hz) + 2-pole Butterworth anti-imaging LPF */
    uint8_t  up_phase;
    uint16_t tail_rem;
    int32_t  prev_raw;
    int32_t  cur_raw;
    int32_t  up_x1;
    int32_t  up_x2;
    int32_t  up_y1;
    int32_t  up_y2;
    int32_t  dc_x1;
    int32_t  dc_y1;

    /* Lock-free SPSC ring buffer of pending synthesis frames (power-of-two) */
    adrift_klatt_frame_t fifo[ADRIFT_KLATT_FIFO_FRAMES];
    volatile uint32_t    fifo_rd;
    volatile uint32_t    fifo_wr;
    volatile uint32_t    flush_target;
    volatile uint32_t    flush_seq;
    volatile uint32_t    ack_flush_seq;
} adrift_klatt_t;

void     adrift_klatt_init(adrift_klatt_t *k);
void     adrift_klatt_reset(adrift_klatt_t *k);
void     adrift_klatt_clear_queue(adrift_klatt_t *k);
void     adrift_klatt_set_speed(adrift_klatt_t *k, uint8_t speed_1_to_5);
uint8_t  adrift_klatt_get_speed(const adrift_klatt_t *k);
uint16_t adrift_klatt_free_frames(const adrift_klatt_t *k);
bool     adrift_klatt_is_speaking(const adrift_klatt_t *k);
bool     adrift_klatt_push_frame(adrift_klatt_t *k, const adrift_klatt_frame_t *fr);

/* Render `frames` mono 44,118 Hz 16-bit PCM samples into `mono_out`.
 * Returns true if any active speech frames were rendered in this block. */
bool     adrift_klatt_render(adrift_klatt_t *k, int16_t *mono_out, uint32_t frames);
