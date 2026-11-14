/* SPDX-License-Identifier: MPL-2.0 */
#include "speech/klatt.h"

/* Fixed-point state clamp (+-4,194,304) guaranteeing zero 32-bit overflow */
#define KLATT_STATE_MAX  4194304
#define KLATT_STATE_MIN -4194304

/* Base samples per synthesis frame at 11,029.5 Hz across speeds 1..5 */
static const uint8_t k_speed_frame_samples[5] = {
    76u, /* Speed 1: slow (~6.9 ms/frame) */
    64u, /* Speed 2: moderately slow (~5.8 ms/frame) */
    54u, /* Speed 3: normal default (~4.9 ms/frame) */
    42u, /* Speed 4: fast (~3.8 ms/frame) */
    32u  /* Speed 5: very fast (~2.9 ms/frame) */
};

/* exp(-x) * 16384 for x = i / 32.0, i = 0..128 (x in [0.0, 4.0]) */
static const uint16_t k_exp_neg_q14[129] = {
    16384, 15880, 15391, 14918, 14459, 14014, 13583, 13165, 12760, 12367, 11987, 11618,
    11261, 10914, 10578, 10253,  9937,  9632,  9335,  9048,  8770,  8500,  8238,  7985,
     7739,  7501,  7270,  7047,  6830,  6620,  6416,  6219,  6027,  5842,  5662,  5488,
     5319,  5155,  4997,  4843,  4694,  4550,  4410,  4274,  4143,  4015,  3892,  3772,
     3656,  3543,  3434,  3329,  3226,  3127,  3031,  2937,  2847,  2760,  2675,  2592,
     2513,  2435,  2360,  2288,  2217,  2149,  2083,  2019,  1957,  1897,  1838,  1782,
     1727,  1674,  1622,  1572,  1524,  1477,  1432,  1388,  1345,  1304,  1263,  1225,
     1187,  1150,  1115,  1081,  1047,  1015,   984,   954,   924,   896,   868,   842,
      816,   791,   766,   743,   720,   698,   676,   655,   635,   616,   597,   578,
      561,   543,   527,   510,   495,   480,   465,   450,   437,   423,   410,   398,
      385,   373,   362,   351,   340,   330,   319,   310,   300
};

/* cos((pi / 2) * i / 256) * 16384 for i = 0..256 */
static const int16_t k_cos_q14[257] = {
    16384, 16384, 16383, 16381, 16379, 16376, 16373, 16369, 16364, 16359, 16353, 16347,
    16340, 16332, 16324, 16315, 16305, 16295, 16284, 16273, 16261, 16248, 16235, 16221,
    16207, 16192, 16176, 16160, 16143, 16125, 16107, 16088, 16069, 16049, 16029, 16008,
    15986, 15964, 15941, 15917, 15893, 15868, 15843, 15817, 15791, 15763, 15736, 15707,
    15679, 15649, 15619, 15588, 15557, 15525, 15493, 15460, 15426, 15392, 15357, 15322,
    15286, 15250, 15213, 15175, 15137, 15098, 15059, 15019, 14978, 14937, 14896, 14854,
    14811, 14768, 14724, 14680, 14635, 14589, 14543, 14497, 14449, 14402, 14354, 14305,
    14256, 14206, 14155, 14104, 14053, 14001, 13949, 13896, 13842, 13788, 13733, 13678,
    13623, 13567, 13510, 13453, 13395, 13337, 13279, 13219, 13160, 13100, 13039, 12978,
    12916, 12854, 12792, 12729, 12665, 12601, 12537, 12472, 12406, 12340, 12274, 12207,
    12140, 12072, 12004, 11935, 11866, 11797, 11727, 11656, 11585, 11514, 11442, 11370,
    11297, 11224, 11151, 11077, 11003, 10928, 10853, 10778, 10702, 10625, 10549, 10471,
    10394, 10316, 10238, 10159, 10080, 10001,  9921,  9841,  9760,  9679,  9598,  9516,
     9434,  9352,  9269,  9186,  9102,  9019,  8935,  8850,  8765,  8680,  8595,  8509,
     8423,  8337,  8250,  8163,  8076,  7988,  7900,  7812,  7723,  7635,  7545,  7456,
     7366,  7276,  7186,  7096,  7005,  6914,  6823,  6731,  6639,  6547,  6455,  6363,
     6270,  6177,  6084,  5990,  5897,  5803,  5708,  5614,  5520,  5425,  5330,  5235,
     5139,  5044,  4948,  4852,  4756,  4660,  4563,  4467,  4370,  4273,  4176,  4078,
     3981,  3883,  3786,  3688,  3590,  3492,  3393,  3295,  3196,  3098,  2999,  2900,
     2801,  2702,  2603,  2503,  2404,  2305,  2205,  2105,  2006,  1906,  1806,  1706,
     1606,  1506,  1406,  1306,  1205,  1105,  1005,   904,   804,   704,   603,   503,
      402,   302,   201,   101,     0
};

/* Klatt GETAMP dB-to-linear table: 0 -> 0, d = 1..95 -> round(16384 * 2^((d - 87)/6)) */
static const uint16_t k_db_amp[96] = {
        0,     1,     1,     1,     1,     1,     1,     2,     2,     2,     2,     3,
        3,     3,     4,     4,     4,     5,     6,     6,     7,     8,     9,    10,
       11,    13,    14,    16,    18,    20,    23,    25,    29,    32,    36,    40,
       45,    51,    57,    64,    72,    81,    91,   102,   114,   128,   144,   161,
      181,   203,   228,   256,   287,   323,   362,   406,   456,   512,   575,   645,
      724,   813,   912,  1024,  1149,  1290,  1448,  1625,  1825,  2048,  2299,  2580,
     2896,  3251,  3649,  4096,  4598,  5161,  5793,  6502,  7298,  8192,  9195, 10321,
    11585, 13004, 14596, 16384, 18390, 20643, 23170, 26008, 29193, 32768, 36781, 41285
};

static inline int32_t clamp_i32(int32_t x, int32_t lo, int32_t hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

/* Split-word signed Q14 multiply truncated symmetrically toward zero:
 * trunc((a_q14 * x) / 16384) with zero 64-bit integer math.
 * Exact for |a_q14| <= 65536 and |x| <= 8,388,608. */
static inline int32_t klatt_mul_q14(int32_t a_q14, int32_t x) {
    int32_t lp = a_q14 * (x & 0x3FFF);
    int32_t q  = (a_q14 * (x >> 14)) + (lp >> 14);
    return q + ((q < 0 && (lp & 0x3FFF) != 0) ? 1 : 0);
}

/* Split-word signed Q10 multiply truncated symmetrically toward zero:
 * trunc((g_q10 * x) / 1024) with zero 64-bit integer math. */
static inline int32_t klatt_mul_q10(int32_t g_q10, int32_t x) {
    int32_t lp = g_q10 * (x & 0x03FF);
    int32_t q  = (g_q10 * (x >> 10)) + (lp >> 10);
    return q + ((q < 0 && (lp & 0x03FF) != 0) ? 1 : 0);
}

static inline int32_t db_to_lin(int32_t db_raw, int32_t offset) {
    if (db_raw <= 0) {
        return 0;
    }
    int32_t idx = clamp_i32(db_raw + offset, 1, 95);
    return (int32_t)k_db_amp[idx];
}

static int32_t exp_neg_q14(int32_t x_q10) {
    if (x_q10 <= 0) {
        return 16384;
    }
    if (x_q10 >= 4096) {
        return (int32_t)k_exp_neg_q14[128];
    }
    int32_t idx  = x_q10 >> 5;
    int32_t frac = x_q10 & 31;
    int32_t v0   = (int32_t)k_exp_neg_q14[idx];
    int32_t v1   = (int32_t)k_exp_neg_q14[idx + 1];
    return v0 + (((v1 - v0) * frac) >> 5);
}

static int32_t cos_2pi_q14(uint32_t phase_q12) {
    uint32_t p    = phase_q12 & 4095u;
    uint32_t quad = p >> 10;
    uint32_t off  = p & 1023u;
    uint32_t ro   = 1024u - off;
    uint32_t idx;
    int32_t  frac;
    int32_t  sign;

    if (quad == 0u) {
        idx  = off >> 2;
        frac = (int32_t)(off & 3u);
        sign = 1;
    } else if (quad == 1u) {
        idx  = ro >> 2;
        frac = (int32_t)(ro & 3u);
        sign = -1;
    } else if (quad == 2u) {
        idx  = off >> 2;
        frac = (int32_t)(off & 3u);
        sign = -1;
    } else {
        idx  = ro >> 2;
        frac = (int32_t)(ro & 3u);
        sign = 1;
    }

    int32_t c0  = (int32_t)k_cos_q14[idx];
    int32_t c1  = (int32_t)k_cos_q14[(idx < 256u) ? (idx + 1u) : 256u];
    int32_t val = c0 + (((c1 - c0) * frac) >> 2);
    return (sign > 0) ? val : -val;
}

/* Klatt (1980) SETABC: 2-pole digital resonator at Fs = 11,030 Hz */
static void res_set_pole(adrift_klatt_res_t *r, int32_t f_hz, int32_t bw_hz) {
    int32_t f  = clamp_i32(f_hz, 0, 5200);
    int32_t bw = clamp_i32(bw_hz, 30, 6000);

    /* x = pi * bw / 11030 -> x_q10 = round(bw * pi * 1024 / 11030) = (bw * 299) >> 10 */
    int32_t x_q10  = (bw * 299) >> 10;
    int32_t rad    = exp_neg_q14(x_q10);
    int32_t c_q14  = -klatt_mul_q14(rad, rad);

    /* phase_q12 = f * 4096 / 11030; since 4096 / 11030 = 0.37135 ~= 1521 / 4096 */
    uint32_t ph_q12 = (uint32_t)((f * 1521) >> 12);
    int32_t  cos_v  = cos_2pi_q14(ph_q12);
    int32_t  b_q14  = 2 * klatt_mul_q14(rad, cos_v);
    int32_t  a_q14  = 16384 - b_q14 - c_q14;

    r->a_q14     = a_q14;
    r->b_q14     = b_q14;
    r->c_q14     = c_q14;
    r->inv_a_q10 = 1024;
}

/* Klatt (1980) SETABC antiresonator (2-zero filter with unity DC gain) */
static void res_set_zero(adrift_klatt_res_t *r, int32_t f_hz, int32_t bw_hz) {
    res_set_pole(r, f_hz, bw_hz);
    int32_t a_safe = (r->a_q14 > 256) ? r->a_q14 : 256;
    r->inv_a_q10   = (16384 * 1024) / a_safe;
}

static inline int32_t res_step_pole(adrift_klatt_res_t *r, int32_t x) {
    x = clamp_i32(x, KLATT_STATE_MIN, KLATT_STATE_MAX);
    int32_t lp = (r->a_q14 * (x & 0x3FFF)) +
                 (r->b_q14 * (r->y1 & 0x3FFF)) +
                 (r->c_q14 * (r->y2 & 0x3FFF));
    int32_t q  = (r->a_q14 * (x >> 14)) +
                 (r->b_q14 * (r->y1 >> 14)) +
                 (r->c_q14 * (r->y2 >> 14)) +
                 (lp >> 14);
    int32_t y  = q + ((q < 0 && (lp & 0x3FFF) != 0) ? 1 : 0);
    y = clamp_i32(y, KLATT_STATE_MIN, KLATT_STATE_MAX);
    r->y2 = r->y1;
    r->y1 = y;
    return y;
}

static inline int32_t res_step_zero(adrift_klatt_res_t *r, int32_t x) {
    x = clamp_i32(x, KLATT_STATE_MIN, KLATT_STATE_MAX);
    int32_t lp = (r->b_q14 * (r->y1 & 0x3FFF)) +
                 (r->c_q14 * (r->y2 & 0x3FFF));
    int32_t q  = (r->b_q14 * (r->y1 >> 14)) +
                 (r->c_q14 * (r->y2 >> 14)) +
                 (lp >> 14);
    int32_t fb  = q + ((q < 0 && (lp & 0x3FFF) != 0) ? 1 : 0);
    int32_t num = clamp_i32(x - fb, KLATT_STATE_MIN * 2, KLATT_STATE_MAX * 2);
    r->y2 = r->y1;
    r->y1 = x;
    int32_t y = klatt_mul_q10(r->inv_a_q10, num);
    return clamp_i32(y, KLATT_STATE_MIN, KLATT_STATE_MAX);
}

static void zero_bytes(void *ptr, size_t n) {
    uint8_t *p = (uint8_t *)ptr;
    for (size_t i = 0u; i < n; i++) {
        p[i] = 0u;
    }
}

void adrift_klatt_reset(adrift_klatt_t *k) {
    if (!k) {
        return;
    }
    uint8_t saved_speed = (k->speed >= 1u && k->speed <= 5u) ? k->speed : 3u;
    zero_bytes(k, sizeof(*k));
    k->rng   = 0x6D2B79F5u;
    k->t0    = 88u;
    k->speed = saved_speed;

    /* Fixed source & upper cascade/parallel resonators (Klatt 1980 Table I) */
    res_set_pole(&k->rgp, 0,    100);
    res_set_zero(&k->rgz, 1500, 6000);
    res_set_pole(&k->rgs, 0,    200);
    res_set_zero(&k->rnz, 270,  100);
    res_set_pole(&k->rnp, 270,  100);
    res_set_pole(&k->r1,  500,  70);
    res_set_pole(&k->r2,  1500, 100);
    res_set_pole(&k->r3,  2500, 140);
    res_set_pole(&k->r4,  3500, 250);
    res_set_pole(&k->r5,  4500, 500);
    res_set_pole(&k->r2p, 1500, 100);
    res_set_pole(&k->r3p, 2500, 140);
    res_set_pole(&k->r4p, 3500, 250);
    res_set_pole(&k->r5p, 4500, 500);
    res_set_pole(&k->r6p, 4900, 1000);
}

void adrift_klatt_init(adrift_klatt_t *k) {
    if (!k) {
        return;
    }
    k->speed = 3u;
    adrift_klatt_reset(k);
}

void adrift_klatt_clear_queue(adrift_klatt_t *k) {
    if (!k) {
        return;
    }
    k->flush_target = k->fifo_wr;
    k->flush_seq    = k->flush_seq + 1u;
}

void adrift_klatt_set_speed(adrift_klatt_t *k, uint8_t speed_1_to_5) {
    if (!k) {
        return;
    }
    if (speed_1_to_5 < 1u) speed_1_to_5 = 1u;
    if (speed_1_to_5 > 5u) speed_1_to_5 = 5u;
    k->speed = speed_1_to_5;
}

uint8_t adrift_klatt_get_speed(const adrift_klatt_t *k) {
    if (!k || k->speed < 1u || k->speed > 5u) {
        return 3u;
    }
    return k->speed;
}

uint16_t adrift_klatt_free_frames(const adrift_klatt_t *k) {
    if (!k) {
        return 0u;
    }
    uint32_t used = (uint32_t)(k->fifo_wr - k->fifo_rd);
    if (used >= (ADRIFT_KLATT_FIFO_FRAMES - 1u)) {
        return 0u;
    }
    return (uint16_t)((ADRIFT_KLATT_FIFO_FRAMES - 1u) - used);
}

bool adrift_klatt_is_speaking(const adrift_klatt_t *k) {
    if (!k) {
        return false;
    }
    bool     flush_pending = (k->flush_seq != k->ack_flush_seq);
    uint32_t rd            = k->fifo_rd;
    uint32_t ft            = k->flush_target;
    uint32_t eff_rd        = (flush_pending && (int32_t)(ft - rd) > 0) ? ft : rd;
    if ((uint32_t)(k->fifo_wr - eff_rd) > 0u) {
        return true;
    }
    return (!flush_pending && k->frame_rem > 0u);
}

bool adrift_klatt_push_frame(adrift_klatt_t *k, const adrift_klatt_frame_t *fr) {
    if (!k || !fr) {
        return false;
    }
    uint32_t wr   = k->fifo_wr;
    uint32_t used = (uint32_t)(wr - k->fifo_rd);
    if (used >= (ADRIFT_KLATT_FIFO_FRAMES - 1u)) {
        return false;
    }
    k->fifo[wr & ADRIFT_KLATT_FIFO_MASK] = *fr;
    __asm__ volatile("" ::: "memory");
    k->fifo_wr = wr + 1u;
    return true;
}

static void clear_source_gains(adrift_klatt_t *k) {
    k->frame_rem   = 0u;
    k->cur_f0_hz10 = 0u;
    k->amp_av      = 0;
    k->amp_avs     = 0;
    k->amp_ah      = 0;
    k->amp_af      = 0;
    k->amp_a2      = 0;
    k->amp_a3      = 0;
    k->amp_a4      = 0;
    k->amp_a5      = 0;
    k->amp_a6      = 0;
    k->amp_ab      = 0;
    k->plstep      = 0;
    k->prev_af     = 0u;
    k->prev_ab     = 0u;
}

static void clear_filter_states(adrift_klatt_t *k) {
    clear_source_gains(k);
    k->rgp.y1 = 0; k->rgp.y2 = 0;
    k->rgz.y1 = 0; k->rgz.y2 = 0;
    k->rgs.y1 = 0; k->rgs.y2 = 0;
    k->rnz.y1 = 0; k->rnz.y2 = 0;
    k->rnp.y1 = 0; k->rnp.y2 = 0;
    k->r1.y1  = 0; k->r1.y2  = 0;
    k->r2.y1  = 0; k->r2.y2  = 0;
    k->r3.y1  = 0; k->r3.y2  = 0;
    k->r4.y1  = 0; k->r4.y2  = 0;
    k->r5.y1  = 0; k->r5.y2  = 0;
    k->r2p.y1 = 0; k->r2p.y2 = 0;
    k->r3p.y1 = 0; k->r3p.y2 = 0;
    k->r4p.y1 = 0; k->r4p.y2 = 0;
    k->r5p.y1 = 0; k->r5p.y2 = 0;
    k->r6p.y1 = 0; k->r6p.y2 = 0;
    k->glottal_prev = 0;
    k->fric_prev    = 0;
    k->up_phase     = 0u;
    k->tail_rem     = 0u;
    k->prev_raw     = 0;
    k->cur_raw      = 0;
    k->up_x1        = 0;
    k->up_x2        = 0;
    k->up_y1        = 0;
    k->up_y2        = 0;
    k->dc_x1        = 0;
    k->dc_y1        = 0;
}

static void apply_pending_flush(adrift_klatt_t *k) {
    uint32_t fseq = k->flush_seq;
    if (fseq != k->ack_flush_seq) {
        uint32_t ft = k->flush_target;
        if ((int32_t)(ft - k->fifo_rd) > 0) {
            k->fifo_rd = ft;
        }
        k->ack_flush_seq = fseq;
        clear_source_gains(k);
    }
}

/* Unpack next synthesis frame into resonator coefficients & linear amplitudes */
static void load_next_frame(adrift_klatt_t *k) {
    apply_pending_flush(k);
    uint32_t rd = k->fifo_rd;
    if (rd == k->fifo_wr) {
        clear_source_gains(k);
        return;
    }

    const adrift_klatt_frame_t fr = k->fifo[rd & ADRIFT_KLATT_FIFO_MASK];
    __asm__ volatile("" ::: "memory");
    k->fifo_rd = rd + 1u;

    uint8_t  spd  = (k->speed >= 1u && k->speed <= 5u) ? (uint8_t)(k->speed - 1u) : 2u;
    uint32_t base = (uint32_t)k_speed_frame_samples[spd];
    uint32_t mul  = (fr.dur_mul > 0u) ? (uint32_t)fr.dur_mul : 16u;
    uint32_t nsmp = (base * mul + 8u) >> 4;
    if (nsmp < 8u) {
        nsmp = 8u;
    }
    k->frame_rem = (uint16_t)nsmp;

    int32_t f1  = (fr.f1  > 0u) ? (int32_t)fr.f1  : 500;
    int32_t f2  = (fr.f2  > 0u) ? (int32_t)fr.f2  : 1500;
    int32_t f3  = (fr.f3  > 0u) ? (int32_t)fr.f3  : 2500;
    int32_t b1  = (fr.b1  > 0u) ? (int32_t)fr.b1  : 70;
    int32_t b2  = (fr.b2  > 0u) ? (int32_t)fr.b2  : 100;
    int32_t b3  = (fr.b3  > 0u) ? (int32_t)fr.b3  : 140;
    int32_t fnz = (fr.fnz > 0u) ? (int32_t)fr.fnz : 270;

    res_set_zero(&k->rnz, fnz, 100);
    res_set_pole(&k->rnp, 270, 100);
    res_set_pole(&k->r1,  f1,  b1);
    res_set_pole(&k->r2,  f2,  b2);
    res_set_pole(&k->r3,  f3,  b3);
    res_set_pole(&k->r2p, f2,  b2 + 40);
    res_set_pole(&k->r3p, f3,  b3 + 60);

    /* Plosive release step (Klatt PARCOE PLSTEP) when frication/bypass jumps >= 48 dB */
    if (((int32_t)fr.af - (int32_t)k->prev_af >= 48) ||
        ((int32_t)fr.ab - (int32_t)k->prev_ab >= 48)) {
        k->plstep = -3200;
    }
    k->prev_af = fr.af;
    k->prev_ab = fr.ab;

    k->cur_f0_hz10 = fr.f0_hz10;
    k->cur_sw_par  = fr.sw_par;

    /* Calibrated Klatt linear gains */
    k->amp_av  = db_to_lin((int32_t)fr.av,  15);
    k->amp_avs = db_to_lin((int32_t)fr.avs, 19);
    k->amp_ah  = db_to_lin((int32_t)fr.ah,  -6);
    k->amp_af  = db_to_lin((int32_t)fr.af,   0);

    /* Alternating parallel formant polarities per Klatt (1980) PARCOE */
    k->amp_a2  =  db_to_lin((int32_t)fr.a2, 13);
    k->amp_a3  = -db_to_lin((int32_t)fr.a3, 15);
    k->amp_a4  =  db_to_lin((int32_t)fr.a4, 16);
    k->amp_a5  = -db_to_lin((int32_t)fr.a5, 17);
    k->amp_a6  =  db_to_lin((int32_t)fr.a6, 18);
    k->amp_ab  = -db_to_lin((int32_t)fr.ab, 15);
}

/* Synthesize one internal 11,029.5 Hz Klatt sample */
static int32_t step_klatt_sample(adrift_klatt_t *k) {
    if (k->frame_rem == 0u) {
        load_next_frame(k);
    }
    if (k->frame_rem > 0u) {
        k->frame_rem--;
    }

    /* Pseudo-Gaussian zero-mean noise from 4 xorshift32 steps (~ +-8192) */
    uint32_t rng = k->rng;
    int32_t  acc = 0;
    for (uint32_t i = 0u; i < 4u; i++) {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        acc += (int32_t)(rng & 0xFFFFu) - 32768;
    }
    k->rng = rng;
    int32_t noise = acc >> 4;

    /* Glottal impulse train + RGP/RGZ normal voicing + RGS quasi-sinusoidal voicing */
    int32_t uglot = 0;
    bool    voiced = (k->cur_f0_hz10 >= 500u) && (k->amp_av > 0 || k->amp_avs > 0);
    if (voiced) {
        if (k->nper >= k->t0) {
            k->nper = 0u;
            /* Gentle +-0.5 Hz pitch micro-flutter for natural voice warmth */
            k->flutter_phase = (uint16_t)((k->flutter_phase + 53u) & 4095u);
            int32_t flut = cos_2pi_q14(k->flutter_phase) >> 11; /* -8..+8 (0.1 Hz units) */
            int32_t f0_eff = clamp_i32((int32_t)k->cur_f0_hz10 + flut, 500, 4000);
            int32_t new_t0 = (ADRIFT_KLATT_SR * 10) / f0_eff;
            k->t0 = (uint16_t)clamp_i32(new_t0, 24, 220);
        }

        int32_t imp     = (k->nper == 0u) ? ((int32_t)k->t0 << 12) : 0;
        int32_t gp_out  = res_step_pole(&k->rgp, imp);
        int32_t u_norm  = res_step_zero(&k->rgz, gp_out);
        int32_t u_sin   = res_step_pole(&k->rgs, gp_out);
        int32_t voice   = klatt_mul_q14(k->amp_av, u_norm) + klatt_mul_q14(k->amp_avs, u_sin);

        /* First-difference lip radiation (+6 dB/octave) */
        uglot = voice - k->glottal_prev;
        k->glottal_prev = voice;

        /* 50% glottal-cycle amplitude modulation of noise during closed phase */
        if (k->nper > (k->t0 >> 1)) {
            noise >>= 1;
        }
        k->nper++;
    } else {
        k->nper         = k->t0;
        k->glottal_prev = (k->glottal_prev * 7) / 8;
    }

    /* Cascade vocal-tract branch: RNZ -> RNP -> R5 -> R4 -> R3 -> R2 -> R1 */
    int32_t asp      = klatt_mul_q14(k->amp_ah, noise);
    int32_t casc_in  = (k->cur_sw_par == 0u) ? (uglot + asp) : 0;
    int32_t casc_out = res_step_zero(&k->rnz, casc_in);
    casc_out = res_step_pole(&k->rnp, casc_out);
    casc_out = res_step_pole(&k->r5,  casc_out);
    casc_out = res_step_pole(&k->r4,  casc_out);
    casc_out = res_step_pole(&k->r3,  casc_out);
    casc_out = res_step_pole(&k->r2,  casc_out);
    casc_out = res_step_pole(&k->r1,  casc_out);

    /* Parallel frication & burst branch: first-difference frication -> R2P..R6P + AB */
    int32_t fric  = klatt_mul_q14(k->amp_af, noise);
    int32_t fdiff = fric - k->fric_prev;
    k->fric_prev  = fric;

    if (k->plstep != 0) {
        fdiff += k->plstep;
        k->plstep = (k->plstep * 7) / 8;
        if (k->plstep > -2 && k->plstep < 2) {
            k->plstep = 0;
        }
    }

    int32_t par_out = 0;
    if (k->amp_a2 != 0) par_out += res_step_pole(&k->r2p, klatt_mul_q14(k->amp_a2, fdiff));
    else                (void)res_step_pole(&k->r2p, 0);

    if (k->amp_a3 != 0) par_out += res_step_pole(&k->r3p, klatt_mul_q14(k->amp_a3, fdiff));
    else                (void)res_step_pole(&k->r3p, 0);

    if (k->amp_a4 != 0) par_out += res_step_pole(&k->r4p, klatt_mul_q14(k->amp_a4, fdiff));
    else                (void)res_step_pole(&k->r4p, 0);

    if (k->amp_a5 != 0) par_out += res_step_pole(&k->r5p, klatt_mul_q14(k->amp_a5, fdiff));
    else                (void)res_step_pole(&k->r5p, 0);

    if (k->amp_a6 != 0) par_out += res_step_pole(&k->r6p, klatt_mul_q14(k->amp_a6, fdiff));
    else                (void)res_step_pole(&k->r6p, 0);

    if (k->amp_ab != 0) par_out += klatt_mul_q14(k->amp_ab, fdiff);

    return clamp_i32((casc_out + par_out) * 16, KLATT_STATE_MIN, KLATT_STATE_MAX);
}

static inline int16_t soft_limit_i16(int32_t x) {
    /* Smooth knee above +-22000 into +-30000 so sibilants/vowels never hard-clip */
    if (x > 22000) {
        x = 22000 + ((x - 22000) >> 2);
        if (x > 31000) x = 31000;
    } else if (x < -22000) {
        x = -22000 + ((x + 22000) >> 2);
        if (x < -31000) x = -31000;
    }
    return (int16_t)x;
}

bool adrift_klatt_render(adrift_klatt_t *k, int16_t *mono_out, uint32_t frames) {
    if (!k || !mono_out) {
        return false;
    }

    apply_pending_flush(k);
    bool speaking = adrift_klatt_is_speaking(k);
    if (speaking) {
        k->tail_rem = ADRIFT_KLATT_TAIL_SAMPLES;
    } else if (k->tail_rem == 0u) {
        for (uint32_t i = 0u; i < frames; i++) {
            mono_out[i] = 0;
        }
        return false;
    }

    bool any_speaking = speaking;

    /* 2-pole Butterworth anti-imaging LPF @ fc = 4800 Hz, fs = 44118 Hz (exact unity DC gain):
     *   b0 = 1273, b1 = 2546, b2 = 1273 (sum = 5092)
     *  -a1 = 17561, -a2 = -6269 (16384 - 17561 + 6269 = 5092) */
    for (uint32_t i = 0u; i < frames; i++) {
        if (!speaking && k->tail_rem == 0u) {
            mono_out[i] = 0;
            continue;
        }

        uint8_t ph = k->up_phase & 3u;
        if (ph == 0u) {
            k->prev_raw = k->cur_raw;
            k->cur_raw  = step_klatt_sample(k);
        }
        k->up_phase = (uint8_t)((ph + 1u) & 3u);

        /* 4x linear interpolation step */
        int32_t x = k->prev_raw + (((k->cur_raw - k->prev_raw) * (int32_t)(ph + 1u)) >> 2);

        /* 2-pole Butterworth anti-imaging filter (fused round-toward-zero Q14) */
        int32_t x_sum = x + k->up_x2;
        int32_t lp    = (1273 * (x_sum & 0x3FFF)) +
                        (2546 * (k->up_x1 & 0x3FFF)) +
                        (17561 * (k->up_y1 & 0x3FFF)) +
                        (-6269 * (k->up_y2 & 0x3FFF));
        int32_t q     = (1273 * (x_sum >> 14)) +
                        (2546 * (k->up_x1 >> 14)) +
                        (17561 * (k->up_y1 >> 14)) +
                        (-6269 * (k->up_y2 >> 14)) +
                        (lp >> 14);
        int32_t y     = q + ((q < 0 && (lp & 0x3FFF) != 0) ? 1 : 0);
        y = clamp_i32(y, KLATT_STATE_MIN, KLATT_STATE_MAX);
        k->up_x2 = k->up_x1;
        k->up_x1 = x;
        k->up_y2 = k->up_y1;
        k->up_y1 = y;

        /* 1-pole ~25 Hz DC blocker: y_dc = x - x1 + 0.996 * y_dc_prev */
        int32_t dc = clamp_i32(y - k->dc_x1 + klatt_mul_q14(16320, k->dc_y1),
                               KLATT_STATE_MIN, KLATT_STATE_MAX);
        k->dc_x1 = y;
        k->dc_y1 = dc;

        int32_t out = (int32_t)soft_limit_i16(dc);
        speaking = adrift_klatt_is_speaking(k);
        if (speaking) {
            any_speaking = true;
            k->tail_rem  = ADRIFT_KLATT_TAIL_SAMPLES;
            mono_out[i]  = (int16_t)out;
        } else {
            /* Smoothly fade bounded residual resonator ring-down to exact digital zero */
            out = (out * (int32_t)k->tail_rem) / (int32_t)ADRIFT_KLATT_TAIL_SAMPLES;
            mono_out[i] = (int16_t)out;
            k->tail_rem--;
            if (k->tail_rem == 0u) {
                clear_filter_states(k);
            }
        }
    }

    return any_speaking;
}
