#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "dsp_math.h"

#define ADRIFT_FX_CHORUS_BITS  10u
#define ADRIFT_FX_CHORUS_LEN   (1u << ADRIFT_FX_CHORUS_BITS)
#define ADRIFT_FX_CHORUS_MASK  (ADRIFT_FX_CHORUS_LEN - 1u)

#define ADRIFT_FX_DELAY_BITS   14u
#define ADRIFT_FX_DELAY_LEN    (1u << ADRIFT_FX_DELAY_BITS)
#define ADRIFT_FX_DELAY_MASK   (ADRIFT_FX_DELAY_LEN - 1u)

#define ADRIFT_FX_AP1_LEN      256u
#define ADRIFT_FX_AP1_MASK     (ADRIFT_FX_AP1_LEN - 1u)

#define ADRIFT_FX_AP2_LEN      512u
#define ADRIFT_FX_AP2_MASK     (ADRIFT_FX_AP2_LEN - 1u)

#define ADRIFT_FX_COMB_LEN     2048u
#define ADRIFT_FX_COMB_MASK    (ADRIFT_FX_COMB_LEN - 1u)

#define ADRIFT_FX_SHIMMER_LEN  1024u
#define ADRIFT_FX_SHIMMER_MASK (ADRIFT_FX_SHIMMER_LEN - 1u)

/*
 * Master Stereo Bus / Send Effects parameters (all 0..1023).
 * Page 1/3: Stereo BBD Dimension Chorus + Stereo Ping-Pong Tape Delay
 * Page 2/3: Stereo Algorithmic Diffusion Reverb + Octave Shimmer Tank
 * Page 3/3: Stereo Bus ("Glue" / Parallel) Soft-Knee Compressor
 */
typedef struct {
    int32_t chorus_mix;     /* 0..1023 -> 0..100% Stereo BBD Dimension Chorus depth & mix */
    int32_t delay_time;     /* 0..1023 -> 20 ms .. 360 ms Stereo Ping-Pong Delay time */
    int32_t delay_fb;       /* 0..1023 -> 0..92% Tape Delay cross-feedback */
    int32_t delay_mix;      /* 0..1023 -> 0..100% Delay wet send level */
    int32_t reverb_size;    /* 0..1023 -> 0..100% Diffusion Reverb tank size / decay */
    int32_t reverb_damp;    /* 0..1023 -> 0..100% Tank high-frequency damping */
    int32_t reverb_shimmer; /* 0..1023 -> 0..100% Octave-up (+12 st) harmonic tank shimmer */
    int32_t reverb_mix;     /* 0..1023 -> 0..100% Reverb wet send level */
    int32_t comp_mix;       /* 0..1023 -> 0..100% Parallel Bus Glue Compressor dry/wet mix */
    int32_t comp_thresh;    /* 0..1023 -> -24 dB .. -2 dB Compressor threshold */
    int32_t comp_attack;    /* 0..1023 -> 1 ms .. 50 ms Compressor attack time */
    int32_t comp_makeup;    /* 0..1023 -> +0 dB .. +12 dB Compressor makeup gain */
} adrift_fx_params_t;

typedef struct {
    /* Stage 1: Stereo BBD Dimension / Ensemble Chorus */
    int16_t  chorus_l[ADRIFT_FX_CHORUS_LEN];
    int16_t  chorus_r[ADRIFT_FX_CHORUS_LEN];
    uint32_t chorus_w;
    uint32_t lfo_phase_1;
    uint32_t lfo_phase_2;
    int32_t  chorus_lp_l;
    int32_t  chorus_lp_r;

    /* Stage 2: Stereo Ping-Pong Tape Delay */
    int16_t  delay_l[ADRIFT_FX_DELAY_LEN];
    int16_t  delay_r[ADRIFT_FX_DELAY_LEN];
    uint32_t delay_w;
    int32_t  delay_len_q8;
    int32_t  delay_len_q14;
    int32_t  delay_lp_l;
    int32_t  delay_lp_r;
    int32_t  delay_dc_x_l;
    int32_t  delay_dc_y_l;
    int32_t  delay_dc_x_r;
    int32_t  delay_dc_y_r;

    /* Stage 3: Stereo Diffusion Reverb + Dual-Grain Octave Shimmer Tank */
    int16_t  ap_in1[ADRIFT_FX_AP1_LEN];
    int16_t  ap_in2[ADRIFT_FX_AP2_LEN];
    uint32_t ap_in1_w;
    uint32_t ap_in2_w;
    int32_t  reverb_in_lp;

    int16_t  comb_l1[ADRIFT_FX_COMB_LEN];
    int16_t  comb_r1[ADRIFT_FX_COMB_LEN];
    int16_t  comb_l2[ADRIFT_FX_COMB_LEN];
    int16_t  comb_r2[ADRIFT_FX_COMB_LEN];
    uint32_t comb_w;
    int32_t  comb_damp_l1;
    int32_t  comb_damp_r1;
    int32_t  comb_damp_l2;
    int32_t  comb_damp_r2;

    int16_t  ap_out_l[ADRIFT_FX_AP2_LEN];
    int16_t  ap_out_r[ADRIFT_FX_AP2_LEN];
    uint32_t ap_out_w;

    int16_t  shimmer_buf[ADRIFT_FX_SHIMMER_LEN];
    uint32_t shimmer_w;
    int32_t  shimmer_lp;
    int32_t  shimmer_dc_x;
    int32_t  shimmer_dc_y;

    /* Stage 4: Stereo Bus ("Glue" / Parallel) Soft-Knee Compressor */
    int32_t  comp_sc_lp_l_q8;
    int32_t  comp_sc_lp_r_q8;
    int32_t  comp_env_q8;
    int32_t  comp_gain_q14;

    /* Real-time metering for UI visualizer & fast bypass */
    int32_t  meter_chorus_lfo_q15;
    int32_t  meter_delay_env_q15;
    int32_t  meter_reverb_env_q15;
    int32_t  meter_comp_gr_q15;
    uint32_t clk_period_q4; /* >0 when synced to external 24 PPQN MIDI clock */
    bool     buffers_dirty;
} adrift_fx_state_t;

void adrift_fx_init(adrift_fx_state_t *fx);
void adrift_fx_reset(adrift_fx_state_t *fx);
bool adrift_fx_is_active(const adrift_fx_params_t *p);
uint32_t adrift_fx_delay_time_ms(int32_t delay_time_1023);
uint32_t adrift_fx_delay_subdiv_pulses(int32_t delay_time_1023);
const char *adrift_fx_delay_subdiv_name(int32_t delay_time_1023);
int32_t  adrift_fx_comp_thresh_db(int32_t thresh_1023);
uint32_t adrift_fx_comp_attack_ms(int32_t attack_1023);

void adrift_fx_process(adrift_fx_state_t *fx,
                       const adrift_fx_params_t *params,
                       int32_t *accum_l,
                       int32_t *accum_r,
                       uint32_t frames);
