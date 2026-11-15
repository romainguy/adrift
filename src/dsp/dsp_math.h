#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Fixed-point conventions across Adrift DSP:
 * - Audio sample signals: Q15 in int32_t (nominal [-32768, 32767], with 16 bits
 *   of headroom in int32_t for filter/mixer intermediates before saturation).
 * - Pitch: Q8 MIDI note units (`60 << 8` = Middle C = 15360; 256 units/semitone).
 * - Delay length: Q16 samples (`1.0 sample = 65536`).
 * - Phase accumulators: unsigned 32-bit Q32 (`0 .. 0xFFFFFFFF` wraps modulo 2*pi).
 *
 * Hardware note (AC791N / pi32v2, see felucca-bsp/docs/CPU.md):
 * Never inline 32x32->64-bit multiplies (`int64_t`) beside memory loads on pi32v2.
 * Hot per-sample loops use pure 32-bit `int32_t` multiplies (`dsp_mul_q15` / `dsp_mul_q14`),
 * and wider products use `noinline` helpers defined in `dsp_tables.c`.
 */

#define DSP_NOTE_TABLE_LEN  141
#define DSP_CUTOFF_LUT_LEN  257
#define DSP_SINE_LUT_BITS   10
#define DSP_SINE_LUT_LEN    (1 << DSP_SINE_LUT_BITS)
#define DSP_SINE_LUT_MASK   (DSP_SINE_LUT_LEN - 1)

#ifndef ADRIFT_SAMPLE_RATE
#define ADRIFT_SAMPLE_RATE      44118u
#define ADRIFT_SAMPLE_RATE_Q8   11294118u /* round((24000000.0 / 544.0) * 256) */
#endif

#if defined(__GNUC__) || defined(__clang__)
#define DSP_ALWAYS_INLINE static inline __attribute__((always_inline))
#else
#define DSP_ALWAYS_INLINE static inline
#endif

/* Place pure leaf sample-rate loops and hot LUTs in 1-cycle zero-wait-state SRAM on AC791N (pi32v2).
 * XIP (0x02xxxxxx) -> RAMTEXT (0x01C0xxxx) exceeds the 23-bit PC-relative call/jump range,
 * so callers in .text invoke DSP_HOT_RAM functions via DSP_CALL_RAM(fn) indirect register call. */
#if (defined(__GNUC__) || defined(__clang__)) && \
    !defined(__APPLE__) && !defined(__linux__) && !defined(_WIN32) && !defined(__unix__)
#define DSP_HOT_RAM      __attribute__((section(".ram_text.dsp"), noinline, used))
#define DSP_HOT_DATA     __attribute__((section(".data.dsp")))
static inline void *dsp_far_ptr(void *p) { void *volatile q = p; return q; }
#define DSP_CALL_RAM(fn) ((__typeof__(&fn))dsp_far_ptr((void *)&fn))
#else
#define DSP_HOT_RAM
#define DSP_HOT_DATA
#define DSP_CALL_RAM(fn) (fn)
#endif

extern const int16_t  g_dsp_sine_q15[DSP_SINE_LUT_LEN + 1];
extern const uint32_t g_dsp_note_phase_inc[DSP_NOTE_TABLE_LEN];
extern const uint32_t g_dsp_note_period_q16[DSP_NOTE_TABLE_LEN];
extern const uint32_t g_dsp_note_freq_q8[DSP_NOTE_TABLE_LEN];
extern const uint32_t g_dsp_cutoff_fc_q8[DSP_CUTOFF_LUT_LEN];
extern const uint32_t g_dsp_svf_g_q16[DSP_CUTOFF_LUT_LEN];
extern const uint16_t g_dsp_onepole_a_q15[DSP_CUTOFF_LUT_LEN];
extern const uint16_t g_dsp_atan_q15[65]; /* atan(x) for x in [0..1] * (32768 / (pi/2)), i.e. Q15 radians * 1024/pi? No: exact Q15 radians! */

/* Out-of-line 64-bit math helpers (pi32v2 mul64-near-load safe) */
int32_t  dsp_mul_q30(int32_t a, int32_t b);
int32_t  dsp_mul_q28(int32_t a, int32_t b);
int32_t  dsp_mul_q24(int32_t a, int32_t b);
int32_t  dsp_mul_q16(int32_t a, int32_t b);
uint32_t dsp_umul_q16(uint32_t a, uint32_t b);
uint32_t dsp_div_u32_q16(uint32_t num, uint32_t den);

/* Pitch & frequency lookup helpers */
uint32_t dsp_pitch_to_phase_inc(int32_t pitch_q8);
uint32_t dsp_pitch_to_period_q16(int32_t pitch_q8);
uint32_t dsp_pitch_to_freq_q8(int32_t pitch_q8);

/* Sub-Q8 (Q16 semitone = 0.0015 cent) interpolated phase increment & period helpers */
DSP_ALWAYS_INLINE uint32_t dsp_pitch_q16_to_phase_inc(int32_t pitch_q16) {
    int32_t  p_q8     = pitch_q16 >> 8;
    uint32_t sub_frac = (uint32_t)(pitch_q16 & 0xFF);
    uint32_t inc0     = dsp_pitch_to_phase_inc(p_q8);
    if (sub_frac == 0u) return inc0;
    uint32_t inc1     = dsp_pitch_to_phase_inc(p_q8 + 1);
    if (inc1 <= inc0) return inc0;
    return inc0 + ((((inc1 - inc0) * sub_frac) + 128u) >> 8);
}

DSP_ALWAYS_INLINE uint32_t dsp_pitch_q16_to_period_q16(int32_t pitch_q16) {
    int32_t  p_q8     = pitch_q16 >> 8;
    uint32_t sub_frac = (uint32_t)(pitch_q16 & 0xFF);
    uint32_t per0     = dsp_pitch_to_period_q16(p_q8);
    if (sub_frac == 0u) return per0;
    uint32_t per1     = dsp_pitch_to_period_q16(p_q8 + 1);
    if (per0 <= per1) return per0;
    return per0 - ((((per0 - per1) * sub_frac) + 128u) >> 8);
}

/* Cutoff 0..1023 -> filter parameters (32.7 Hz .. 21 kHz exponential) */
uint32_t dsp_cutoff_to_fc_q8(int32_t cutoff_1023);
uint32_t dsp_cutoff_to_svf_g_q16(int32_t cutoff_1023);
uint16_t dsp_cutoff_to_onepole_a_q15(int32_t cutoff_1023);

/* Exact arctan(num / den) in Q16 radians for num >= 0, den > 0 */
uint32_t dsp_atan2_pos_q16(uint32_t num, uint32_t den);

/* Fast inline 32-bit arithmetic (zero int64_t!) */
DSP_ALWAYS_INLINE int32_t dsp_clamp_i32(int32_t x, int32_t lo, int32_t hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

DSP_ALWAYS_INLINE int16_t dsp_sat16(int32_t x) {
    if (x > 32767) return 32767;
    if (x < -32768) return -32768;
    return (int16_t)x;
}

DSP_ALWAYS_INLINE int32_t dsp_sat_q15(int32_t x) {
    if (x > 32767) return 32767;
    if (x < -32767) return -32767;
    return x;
}

/* Safe for |a|, |b| <= 46340 (no 64-bit multiply!) */
DSP_ALWAYS_INLINE int32_t dsp_mul_q15(int32_t a, int32_t b) {
    return (a * b) >> 15;
}

/* Safe for |a|, |b| <= 32768 (1.0 in Q14 = 16384; allows gains up to 2.0) */
DSP_ALWAYS_INLINE int32_t dsp_mul_q14(int32_t a, int32_t b) {
    return (a * b) >> 14;
}

/* Analog BBD MOSFET bucket-well soft saturator (C1-continuous linear-core cubic in Q15):
 * 1:1 bit-exact linear up to -12 dBFS (+8192 / -8704) so normal plucks decay naturally without
 * per-pass dynamic compression, then smoothly saturating to +21504 / -22528 analog charge-well rails. */
DSP_ALWAYS_INLINE int16_t dsp_bbd_saturate_q15(int32_t x) {
    if (x >= 0) {
        if (x <= 8192) return (int16_t)x;
        if (x >= 28160) return 21504;
        int32_t d        = x - 8192;
        int32_t u_q15    = (d * 53773) >> 15;
        int32_t u2_q15   = (u_q15 * u_q15) >> 15;
        int32_t gain_q15 = 32768 - ((u2_q15 * 10923) >> 15);
        return (int16_t)(8192 + (((d * gain_q15) + 16384) >> 15));
    } else {
        if (x >= -8704) return (int16_t)x;
        if (x <= -29440) return -22528;
        int32_t d        = (-x) - 8704;
        int32_t u_q15    = (d * 51781) >> 15;
        int32_t u2_q15   = (u_q15 * u_q15) >> 15;
        int32_t gain_q15 = 32768 - ((u2_q15 * 10923) >> 15);
        return (int16_t)(-(8704 + (((d * gain_q15) + 16384) >> 15)));
    }
}

/* Exact division by 1023 (2^10 - 1) for x in [0, 67042305] without hardware divide or int64_t */
DSP_ALWAYS_INLINE uint32_t dsp_udiv_1023(uint32_t x) {
    uint32_t q   = (x + (x >> 10) + (x >> 20)) >> 10;
    uint32_t rem = x - ((q << 10) - q);
    return q + (rem >= 1023u ? 1u : 0u);
}

/* Exact division by 127 (2^7 - 1) for x in [0, 4194303] without hardware divide or int64_t */
DSP_ALWAYS_INLINE uint32_t dsp_udiv_127(uint32_t x) {
    uint32_t q   = (x + (x >> 7) + (x >> 14)) >> 7;
    uint32_t rem = x - ((q << 7) - q);
    return q + (rem >= 127u ? 1u : 0u);
}

/* Exact signed division `diff / (int32_t)frames` with shift fast-path for standard 64-frame blocks */
DSP_ALWAYS_INLINE int32_t dsp_div_frames_i32(int32_t diff, uint32_t frames) {
    if (frames == 64u) {
        return (diff + ((diff < 0) ? 63 : 0)) >> 6;
    }
    return diff / (int32_t)frames;
}

/* Fast deterministic xorshift32 PRNG */
DSP_ALWAYS_INLINE uint32_t dsp_xorshift32(uint32_t *state) {
    uint32_t x = *state;
    if (x == 0u) x = 0x6D2B79F5u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* Uniform white noise sample in [-32767, 32767] */
DSP_ALWAYS_INLINE int32_t dsp_white_noise_q15(uint32_t *state) {
    int32_t r = (int32_t)(dsp_xorshift32(state) >> 16) - 32768;
    return r < -32767 ? -32767 : r;
}

/* Linear-interpolated 1024-entry sine LUT from Q32 phase */
DSP_ALWAYS_INLINE int32_t dsp_sine_lut_q15(uint32_t phase_q32) {
    uint32_t idx  = phase_q32 >> (32 - DSP_SINE_LUT_BITS);
    int32_t  frac = (int32_t)((phase_q32 >> (32 - DSP_SINE_LUT_BITS - 15)) & 0x7FFFu);
    int32_t  y0   = g_dsp_sine_q15[idx];
    int32_t  y1   = g_dsp_sine_q15[idx + 1u];
    return y0 + (((y1 - y0) * frac) >> 15);
}
