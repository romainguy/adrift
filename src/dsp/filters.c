#include "filters.h"

#if defined(__GNUC__) || defined(__clang__)
#define FILT_NOINLINE __attribute__((noinline))
#else
#define FILT_NOINLINE
#endif

void adrift_svf_init(adrift_svf_lpf12_t *f) {
    f->last_cutoff = -1;
    f->last_res    = -1;
    adrift_svf_reset(f);
    adrift_svf_set_params(f, 1023, 0);
}

void adrift_svf_reset(adrift_svf_lpf12_t *f) {
    f->ic1eq_q20 = 0;
    f->ic2eq_q20 = 0;
}

static FILT_NOINLINE void svf_compute_a123(uint32_t g_q16,
                                           uint32_t k_q16,
                                           int32_t *a1_q28,
                                           int32_t *a2_q28,
                                           int32_t *a3_q28) {
    /*
     * a1 = 1 / (1 + g * (g + k))
     * a2 = g * a1
     * a3 = g * a2
     * Delegated to leaf noinline helpers dsp_umul_q16 / dsp_div_u32_q16 so no
     * 64-bit multiply ever sits beside stack loads on pi32v2.
     */
    uint32_t gk_q16    = g_q16 + k_q16;
    uint32_t ggk_q16   = dsp_umul_q16(g_q16, gk_q16);
    uint32_t denom_q16 = 65536u + ggk_q16;
    uint32_t a1 = dsp_div_u32_q16(1u << 28, denom_q16);
    uint32_t a2 = dsp_umul_q16(a1, g_q16);
    uint32_t a3 = dsp_umul_q16(a2, g_q16);
    if (a1 > 0x0FFFFFFFu) a1 = 0x0FFFFFFFu;
    if (a2 > 0x0FFFFFFFu) a2 = 0x0FFFFFFFu;
    if (a3 > 0x0FFFFFFFu) a3 = 0x0FFFFFFFu;
    *a1_q28 = (int32_t)a1;
    *a2_q28 = (int32_t)a2;
    *a3_q28 = (int32_t)a3;
}

void adrift_svf_set_params(adrift_svf_lpf12_t *f, int32_t cutoff_1023, int32_t res_1023) {
    int32_t c = dsp_clamp_i32(cutoff_1023, 0, 1023);
    int32_t r = dsp_clamp_i32(res_1023, 0, 1023);
    if (f->last_cutoff == (int16_t)c && f->last_res == (int16_t)r) {
        return;
    }
    f->last_cutoff = (int16_t)c;
    f->last_res    = (int16_t)r;
    uint32_t g_q16 = dsp_cutoff_to_svf_g_q16(c);
    /* k = 2.0 - 1.88 * (r / 1023) -> Q in [0.5 .. 8.3] */
    uint32_t k_q16 = 131072u - (((uint32_t)r * 123207u) >> 10);
    svf_compute_a123(g_q16, k_q16, &f->a1_q28, &f->a2_q28, &f->a3_q28);
}

/* Exact 32-bit split Q28 product `((int64_t)a * (int64_t)x) >> 28` with zero 64-bit multiplies
 * (immune to the pi32v2 mul64-near-load silicon hazard and fully inlineable in SRAM). */
DSP_ALWAYS_INLINE int32_t svf_mul_q28_split(int32_t a_hi, int32_t a_lo, int32_t x_hi, int32_t x_lo) {
    return (x_hi * a_hi) + (((x_hi * a_lo) + (x_lo * a_hi) + ((x_lo * a_lo) >> 14)) >> 14);
}

static DSP_HOT_RAM void svf_process_lpf_ram(adrift_svf_lpf12_t *f,
                                            const int32_t *in_q15,
                                            int32_t *out_q15,
                                            uint32_t frames) {
    int32_t ic1   = f->ic1eq_q20;
    int32_t ic2   = f->ic2eq_q20;
    int32_t a1    = f->a1_q28;
    int32_t a2    = f->a2_q28;
    int32_t a3    = f->a3_q28;
    int32_t a1_hi = a1 >> 14;
    int32_t a1_lo = a1 & 0x3FFF;
    int32_t a2_hi = a2 >> 14;
    int32_t a2_lo = a2 & 0x3FFF;
    int32_t a3_hi = a3 >> 14;
    int32_t a3_lo = a3 & 0x3FFF;

    for (uint32_t i = 0; i < frames; i++) {
        int32_t v0     = in_q15[i] * 32; /* Q15 -> Q20 (UBSan-safe signed scale) */
        int32_t v3     = v0 - ic2;
        int32_t ic1_hi = ic1 >> 14;
        int32_t ic1_lo = ic1 & 0x3FFF;
        int32_t v3_hi  = v3 >> 14;
        int32_t v3_lo  = v3 & 0x3FFF;

        int32_t v1 = svf_mul_q28_split(a1_hi, a1_lo, ic1_hi, ic1_lo) +
                     svf_mul_q28_split(a2_hi, a2_lo, v3_hi,  v3_lo);
        int32_t v2 = ic2 +
                     svf_mul_q28_split(a2_hi, a2_lo, ic1_hi, ic1_lo) +
                     svf_mul_q28_split(a3_hi, a3_lo, v3_hi,  v3_lo);

        /* Mild analog integrator clamping at +-8x full scale prevents state blowup under extreme Q */
        v1 = dsp_clamp_i32(v1, -(1 << 27), (1 << 27));
        v2 = dsp_clamp_i32(v2, -(1 << 27), (1 << 27));

        ic1 = (v1 * 2) - ic1;
        ic2 = (v2 * 2) - ic2;
        out_q15[i] = dsp_sat_q15(v2 >> 5);
    }

    f->ic1eq_q20 = ic1;
    f->ic2eq_q20 = ic2;
}

void adrift_svf_process_lpf(adrift_svf_lpf12_t *f, const int32_t *in_q15, int32_t *out_q15, uint32_t frames) {
    DSP_CALL_RAM(svf_process_lpf_ram)(f, in_q15, out_q15, frames);
}

void adrift_onepole_init(adrift_onepole_lpf6_t *f) {
    f->state_q29 = 0;
    f->a_q15     = 32767u;
}

void adrift_onepole_reset(adrift_onepole_lpf6_t *f) {
    f->state_q29 = 0;
}

void adrift_onepole_set_cutoff(adrift_onepole_lpf6_t *f, int32_t cutoff_1023) {
    f->a_q15 = dsp_cutoff_to_onepole_a_q15(cutoff_1023);
}

void adrift_onepole_set_alpha(adrift_onepole_lpf6_t *f, uint16_t a_q15) {
    if (a_q15 < 1u) a_q15 = 1u;
    if (a_q15 > 32767u) a_q15 = 32767u;
    f->a_q15 = a_q15;
}

FILT_NOINLINE uint32_t adrift_onepole_phase_delay_q16(uint16_t a_q15, uint32_t f0_phase_inc) {
    if (a_q15 >= 32767u || f0_phase_inc == 0u) {
        return 0u;
    }
    int32_t oma_q15 = 32768 - (int32_t)a_q15;
    int32_t sin_q15 = dsp_sine_lut_q15(f0_phase_inc);
    int32_t cos_q15 = dsp_sine_lut_q15(f0_phase_inc + 0x40000000u);
    if (sin_q15 <= 0) {
        return 0u;
    }

    /* H(e^{jw}) = a / (1 - (1 - a) * e^{-jw}) -> phase lag = atan2((1-a)*sin(w), 1 - (1-a)*cos(w))
     * Note: oma_q15 in [1..32767] and sin/cos in [-32767..32767], so 32-bit products fit in Q30! */
    int32_t num_q30 = oma_q15 * sin_q15;
    int32_t den_q30 = (1 << 30) - (oma_q15 * cos_q15);
    if (den_q30 <= 1) den_q30 = 1;

    uint32_t phi_q16 = dsp_atan2_pos_q16((uint32_t)(num_q30 >> 8), (uint32_t)(den_q30 >> 8));

    /* omega in Q16 radians: w = 2*pi * (f0_phase_inc / 2^32) -> preserve 24-bit phase_inc precision */
    uint32_t omega_q16 = (dsp_umul_q16(f0_phase_inc >> 8, 411775u) + 128u) >> 8;
    if (omega_q16 == 0u) {
        /* DC limit: (1 - a) / a in Q16 */
        return dsp_div_u32_q16((uint32_t)oma_q15, (uint32_t)a_q15);
    }
    return dsp_div_u32_q16(phi_q16, omega_q16);
}
