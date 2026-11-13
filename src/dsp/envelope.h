#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "dsp_math.h"

typedef enum {
    ADRIFT_ENV_IDLE = 0,
    ADRIFT_ENV_DELAY,
    ADRIFT_ENV_ATTACK,
    ADRIFT_ENV_DECAY,
    ADRIFT_ENV_SUSTAIN,
    ADRIFT_ENV_RELEASE
} adrift_env_stage_t;

typedef enum {
    ADRIFT_ENV_LOOP_OFF = 0,
    ADRIFT_ENV_LOOP_AD  = 1, /* Re-triggers Attack when Decay hits Sustain while Gate is high */
    ADRIFT_ENV_LOOP_DAD = 2  /* Re-triggers Onset Delay -> Attack -> Decay while Gate is high */
} adrift_env_loop_t;

typedef enum {
    ADRIFT_ENV_RANGE_NORM  = 0, /* Normal: Attack 0.5 ms..5 s, Decay/Release 1 ms..10 s */
    ADRIFT_ENV_RANGE_SHORT = 1, /* Short:  ~0.5 ms..250 ms per stage (Morphor ECHON 6 Short range) */
    ADRIFT_ENV_RANGE_LONG  = 2, /* Long:   ~1 ms..25 s per stage (Morphor ECHON 6 Long range) */
    ADRIFT_ENV_RANGE_COUNT = 3
} adrift_env_range_t;

typedef struct {
    int16_t onset_delay; /* 0..1023 -> 0 ms .. 2000 ms (used by Env 1; 0 for Env 2) */
    int16_t attack;      /* 0..1023 -> 0.5 ms .. 5 s (or 0.5..250 ms SHORT / 1 ms..25 s LONG) */
    int16_t decay;       /* 0..1023 -> 1 ms .. 10 s (or 0.5..250 ms SHORT / 1 ms..25 s LONG) */
    int16_t sustain;     /* 0..1023 -> 0% .. 100% */
    int16_t release;     /* 0..1023 -> 1 ms .. 10 s (or 0.5..250 ms SHORT / 1 ms..25 s LONG) */
    int16_t vel_sens;    /* 0..1023 -> 0% .. 100% velocity scaling */
    uint8_t loop_mode;   /* adrift_env_loop_t */
    uint8_t range;       /* adrift_env_range_t (Env 1 stage range: NORM=0, SHORT=1, LONG=2) */
    bool    drone_hold;  /* If true (Env 2 option), holds open at full scale for endless BBD drones */
} adrift_env_params_t;

typedef struct {
    adrift_env_stage_t stage;
    bool               gate;
    bool               just_triggered;
    int32_t            level_q30;     /* 0 .. (1 << 30) */
    int32_t            vel_scale_q15; /* 0 .. 32767 computed on Note On */
    uint32_t           delay_rem;     /* Remaining samples in ENV_DELAY */
    uint32_t           delay_total;
    int32_t            atk_coef_q14;
    int32_t            atk_floor_q30;
    int32_t            dec_coef_q14;
    int32_t            dec_floor_q30;
    int32_t            sus_level_q30;
    int32_t            rel_coef_q14;
    int32_t            rel_floor_q30;
    int16_t            last_onset_delay;
    int16_t            last_attack;
    int16_t            last_decay;
    int16_t            last_sustain;
    int16_t            last_release;
    uint8_t            last_range;
    bool               coefs_valid;
} adrift_env_t;

void adrift_env_init(adrift_env_t *env);
void adrift_env_reset(adrift_env_t *env);

/* Control-rate update of time constants (once per 64-sample block) */
void adrift_env_update_coefs(adrift_env_t *env, const adrift_env_params_t *p);

/* Trigger Note On (velocity 1..127; legato=true skips retrigger if already active) */
void adrift_env_note_on(adrift_env_t *env, const adrift_env_params_t *p, uint8_t velocity, bool legato);

/* Trigger Note Off */
void adrift_env_note_off(adrift_env_t *env, const adrift_env_params_t *p);

/* Render `frames` envelope gain values (Q15: 0..32767) into `out_gain_q15` */
void adrift_env_process(adrift_env_t *env,
                        const adrift_env_params_t *p,
                        int32_t *out_gain_q15,
                        uint32_t frames);
