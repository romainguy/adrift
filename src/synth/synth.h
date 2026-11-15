#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "../dsp/bbd.h"
#include "../dsp/dsp_math.h"
#include "../dsp/envelope.h"
#include "../dsp/exciter.h"
#include "../dsp/filters.h"
#include "../dsp/fx.h"
#include "../platform/platform.h"

/* 16-voice polyphonic 6 Voice-Group architecture (dual-core AC791N) */
#ifndef ADRIFT_MAX_VOICES
#define ADRIFT_MAX_VOICES 16
#endif

#define ADRIFT_MAX_GROUPS       6
#define ADRIFT_NUM_PRESETS      24
#define ADRIFT_NUM_USER_PRESETS 24
#define ADRIFT_USER_PRESET_BASE 24u /* preset_index 24..47 = U1..U24 */
#define ADRIFT_NUM_SCALES       16
#define ADRIFT_PRESET_INIT      0xFFu
#define ADRIFT_SCOPE_LEN        128

typedef enum {
    ADRIFT_SCALE_CHROMATIC   = 0,
    ADRIFT_SCALE_MAJOR       = 1,
    ADRIFT_SCALE_NAT_MINOR   = 2,
    ADRIFT_SCALE_MIN_PENTA   = 3,
    ADRIFT_SCALE_MAJ_PENTA   = 4,
    ADRIFT_SCALE_BLUES       = 5,
    ADRIFT_SCALE_DORIAN      = 6,
    ADRIFT_SCALE_PHRYGIAN    = 7,
    ADRIFT_SCALE_LYDIAN      = 8,
    ADRIFT_SCALE_MIXOLYDIAN  = 9,
    ADRIFT_SCALE_LOCRIAN     = 10,
    ADRIFT_SCALE_HARM_MINOR  = 11,
    ADRIFT_SCALE_MEL_MINOR   = 12,
    ADRIFT_SCALE_PHRYG_DOM   = 13,
    ADRIFT_SCALE_HIRAJOSHI   = 14,
    ADRIFT_SCALE_WHOLE_TONE  = 15
} adrift_scale_t;

typedef enum {
    ADRIFT_LFO_SINE = 0,
    ADRIFT_LFO_TRI  = 1,
    ADRIFT_LFO_SAW  = 2,
    ADRIFT_LFO_SQR  = 3,
    ADRIFT_LFO_SH   = 4,
    ADRIFT_LFO_COUNT
} adrift_lfo_wave_t;

typedef enum {
    ADRIFT_MOD_VCO_PITCH = 0,
    ADRIFT_MOD_VCO_MORPH = 1,
    ADRIFT_MOD_VCO_PWM   = 2,
    ADRIFT_MOD_IN_LPF    = 3,
    ADRIFT_MOD_BBD_PITCH = 4,
    ADRIFT_MOD_FB_LPF    = 5,
    ADRIFT_MOD_FB_AMT    = 6,
    ADRIFT_MOD_ALL_PITCH = 7,  /* Unified VCO + BBD pitch (locked) */
    ADRIFT_MOD_PAN       = 8,  /* Voice / Group stereo pan */
    ADRIFT_MOD_SPREAD    = 9,  /* Group stereo spread width */
    ADRIFT_MOD_DRY_WET   = 10, /* Post-Input-LPF Dry <-> BBD Wet balance */
    ADRIFT_MOD_VCA       = 11, /* Post-BBD voice output level (tremolo / gate) */
    ADRIFT_MOD_EX_MIX    = 12, /* Exciter VCO wave <-> Noise/S&H mix */
    ADRIFT_MOD_EX_LVL    = 13, /* Pre-BBD exciter send level */
    ADRIFT_MOD_SH_CLK    = 14, /* Exciter Sample & Hold clock rate */
    ADRIFT_MOD_IN_RES    = 15, /* Input LPF resonance Q */
    ADRIFT_MOD_DISPR     = 16, /* BBD loop all-pass dispersion / stiffness */
    ADRIFT_MOD_ENV1_ATK  = 17, /* Env 1 attack time */
    ADRIFT_MOD_ENV1_DEC  = 18, /* Env 1 decay / loop rate */
    ADRIFT_MOD_ENV1_SUS  = 19, /* Env 1 sustain level */
    ADRIFT_MOD_ENV1_REL  = 20, /* Env 1 release time */
    ADRIFT_MOD_ENV1_DLY  = 21, /* Env 1 onset delay */
    ADRIFT_MOD_LFO1_RATE = 22, /* LFO 1 rate (cross-modulation from LFO 2 / LFO 3) */
    ADRIFT_MOD_DEST_COUNT
} adrift_mod_dest_t;

/* LFO 1 excludes ADRIFT_MOD_LFO1_RATE (cannot cross-modulate its own rate) */
#define ADRIFT_LFO1_DEST_COUNT ((uint8_t)ADRIFT_MOD_LFO1_RATE)

typedef enum {
    ADRIFT_GROUP_ROUND_ROBIN = 0,
    ADRIFT_GROUP_UNISON      = 1,
    ADRIFT_GROUP_SUPER       = 2 /* Multi-oscillator Demon Core stack into stereo BBD pair */
} adrift_group_mode_t;

static inline bool adrift_group_is_stacked(uint8_t assign_mode) {
    return (assign_mode == ADRIFT_GROUP_UNISON) || (assign_mode == ADRIFT_GROUP_SUPER);
}

/* Non-linear Demon Core detune curve: 0..1023 -> 0..307 Q8 semitones (+-120 cents peak)
 * - 0..50% knob covers 0..36 cents with fine resolution for warm analog ensemble chorus
 * - 50..100% knob opens up to +-120 cents for massive supersaw / Demon Core walls of sound */
static inline int32_t adrift_detune_to_q8(int16_t detune_1023) {
    int32_t d = dsp_clamp_i32(detune_1023, 0, 1023);
    int32_t curve_1023 = (d * (256 + ((d * 768) >> 10))) >> 10; /* 0..1023 */
    return (int32_t)dsp_udiv_1023((uint32_t)(curve_1023 * 307 + 511)); /* 0..307 (+-120c in Q8) */
}

static inline int32_t adrift_detune_to_cents(int16_t detune_1023) {
    int32_t d = dsp_clamp_i32(detune_1023, 0, 1023);
    int32_t curve_1023 = (d * (256 + ((d * 768) >> 10))) >> 10;
    return (int32_t)dsp_udiv_1023((uint32_t)(curve_1023 * 120 + 511)); /* 0..120 cents */
}

/* -4.4 dB equal-loudness Dry/Wet voice crossfade (Q15: 50/50 blend of quarter-sine equal-power
 * and linear amplitude laws, matched to partially-correlated VCO + pitch-locked BBD signals) */
static inline void adrift_synth_dry_wet_gains_q15(int32_t dw_1023,
                                                  int32_t *out_dry_q15,
                                                  int32_t *out_wet_q15) {
    if (dw_1023 <= 0) {
        *out_dry_q15 = 32767;
        *out_wet_q15 = 0;
        return;
    }
    if (dw_1023 >= 1023) {
        *out_dry_q15 = 0;
        *out_wet_q15 = 32767;
        return;
    }
    int32_t  lin_w = (int32_t)dsp_udiv_1023((uint32_t)dw_1023 * 32767u);
    int32_t  lin_d = 32767 - lin_w;
    uint32_t angle = dsp_udiv_1023(((uint32_t)dw_1023 << 16) + 511u) << 14;
    int32_t  sin_w = dsp_sine_lut_q15(angle);
    int32_t  sin_d = dsp_sine_lut_q15(0x40000000u - angle);
    *out_wet_q15   = (lin_w + sin_w + 1) >> 1;
    *out_dry_q15   = (lin_d + sin_d + 1) >> 1;
}

#define ADRIFT_ARP_MAX_NOTES 16u

typedef enum {
    ADRIFT_ARP_OFF         = 0,
    ADRIFT_ARP_UP          = 1,
    ADRIFT_ARP_DOWN        = 2,
    ADRIFT_ARP_UP_DOWN     = 3, /* Ascend then descend (exclusive endpoints) */
    ADRIFT_ARP_DOWN_UP     = 4, /* Descend then ascend (exclusive endpoints) */
    ADRIFT_ARP_UP_AND_DOWN = 5, /* Ascend then descend (inclusive endpoints) */
    ADRIFT_ARP_CONVERGE    = 6, /* Outside-in: lowest, highest, 2nd lowest, 2nd highest... */
    ADRIFT_ARP_DIVERGE     = 7, /* Inside-out from middle */
    ADRIFT_ARP_RANDOM      = 8, /* Random note + octave from held pool */
    ADRIFT_ARP_ORDER       = 9, /* As-played order across octaves */
    ADRIFT_ARP_CHORD       = 10,/* Rhythmic chord trigger of all held notes */
    ADRIFT_ARP_MODE_COUNT
} adrift_arp_mode_t;

typedef enum {
    ADRIFT_RATCHET_1X     = 0, /* 1 trigger per step (normal) */
    ADRIFT_RATCHET_2X     = 1, /* 2 triggers per step */
    ADRIFT_RATCHET_3X     = 2, /* 3 triggers per step */
    ADRIFT_RATCHET_4X     = 3, /* 4 triggers per step */
    ADRIFT_RATCHET_ALT_2  = 4, /* Alternating 1x, 2x */
    ADRIFT_RATCHET_ALT_3  = 5, /* 1x, 2x, 3x cycle */
    ADRIFT_RATCHET_ALT_4  = 6, /* 1x, 2x, 1x, 4x cycle */
    ADRIFT_RATCHET_RANDOM = 7, /* Deterministic random 1x..4x per step */
    ADRIFT_RATCHET_COUNT
} adrift_arp_ratchet_t;

typedef struct {
    uint8_t  mode;     /* adrift_arp_mode_t (0 = OFF) */
    uint8_t  range;    /* 1..4 octaves (default 1) */
    uint16_t rate_bpm; /* 40..240 BPM (1/16th note step grid; default 120) */
    uint8_t  gate;     /* 5..100 % of step/substep duration (default 80) */
    uint8_t  velocity; /* 0 = KEY (played velocity), 1..127 = fixed MIDI velocity */
    bool     latch;    /* Hold/latch notes after keys are released */
    uint8_t  swing;    /* 0..100 % swing amount (0% = 50/50 straight, 100% = 75/25 shuffle) */
    uint8_t  ratchet;  /* adrift_arp_ratchet_t (0 = 1X .. 7 = RND) */
} adrift_arp_params_t;

typedef struct {
    uint8_t  notes[ADRIFT_ARP_MAX_NOTES];
    uint8_t  vels[ADRIFT_ARP_MAX_NOTES];
    bool     phys_down[ADRIFT_ARP_MAX_NOTES];
    uint8_t  num_notes;
    uint8_t  phys_count;

    uint8_t  sounding_notes[ADRIFT_ARP_MAX_NOTES];
    uint8_t  sounding_count;
    bool     gate_open;

    uint32_t phase_samples;
    uint32_t substep_len_samples;
    uint32_t substep_gate_samples;
    uint8_t  substep_idx;
    uint8_t  substep_total;
    uint32_t step_counter;
    uint8_t  step_idx;
    uint8_t  cur_step_note;
    uint8_t  cur_step_vel;
    uint32_t rng;
} adrift_arp_state_t;

typedef struct {
    char                    name[16];
    adrift_exciter_params_t exc;
    adrift_env_params_t     env1;          /* Exciter DADSR (with Onset Delay & Loop) */
    int16_t                 in_lpf_cutoff; /* 0..1023: 12 dB/oct Input LPF (32.7 Hz - 21 kHz) */
    int16_t                 in_lpf_res;    /* 0..1023: Input LPF resonance Q */
    adrift_bbd_params_t     bbd;           /* 1024-stage BBD resonator */
    int16_t                 dry_wet;       /* 0..1023: 0 = Dry (post-Input-LPF), 1023 = BBD Wet */
    adrift_env_params_t     env2;          /* Post-Mix Output VCA ADSR */
    int16_t                 master_vol;    /* 0..1023 group level */

    /* LFO 1..3 & MIDI performance parameters */
    uint8_t                 lfo_wave;       /* LFO 1 waveform (adrift_lfo_wave_t) */
    int16_t                 lfo_rate;       /* LFO 1 rate: 0..1023 -> 0.05 Hz .. 30 Hz */
    int16_t                 lfo_depth;      /* LFO 1 bipolar depth: -512..+511 */
    uint8_t                 lfo_dest;       /* LFO 1 destination (adrift_mod_dest_t) */
    uint8_t                 lfo2_wave;      /* LFO 2 waveform (adrift_lfo_wave_t) */
    int16_t                 lfo2_rate;      /* LFO 2 rate: 0..1023 -> 0.05 Hz .. 30 Hz */
    int16_t                 lfo2_depth;     /* LFO 2 bipolar depth: -512..+511 */
    uint8_t                 lfo2_dest;      /* LFO 2 destination (adrift_mod_dest_t) */
    uint8_t                 lfo3_wave;      /* LFO 3 waveform (adrift_lfo_wave_t) */
    int16_t                 lfo3_rate;      /* LFO 3 rate: 0..1023 -> 0.05 Hz .. 30 Hz */
    int16_t                 lfo3_depth;     /* LFO 3 bipolar depth: -512..+511 */
    uint8_t                 lfo3_dest;      /* LFO 3 destination (adrift_mod_dest_t) */
    int16_t                 mw_depth;       /* 0..1023 Mod Wheel -> LFO 1 depth */
    uint8_t                 pb_range;       /* semitones (default 2, up to 24) */
    bool                    legato;         /* mono/unison legato envelope behavior */
    adrift_arp_params_t     arp;            /* Per-group arpeggiator parameters */
} adrift_patch_t;

/* Per-group configuration + sound settings (6 groups sharing up to 16 hardware voices) */
typedef struct {
    uint8_t        num_voices;     /* 0..ADRIFT_MAX_VOICES (0 = disabled; sum across groups <= 16) */
    uint8_t        assign_mode;    /* adrift_group_mode_t: Round-Robin, Unison, or Super */
    uint8_t        midi_channel;   /* 1..16 (defaults: G1=1, G2=2, ..., G6=6) */
    int16_t        unison_detune;  /* 0..1023 -> 0..120 cents non-linear Demon Core detune spread */
    int16_t        stereo_spread;  /* 0..1023 -> 0%..100% stereo field pan spread across group voices */
    int16_t        pan;            /* -512..+512 -> L100..C..R100 per-group stereo pan offset */
    int8_t         transpose;      /* -24..+24 semitone group transpose (applied to BBD + VCO) */
    adrift_patch_t patch;

    /* Per-group runtime MIDI controllers & note-allocator state */
    int16_t            pitch_bend_q13; /* -8192 .. +8191 */
    uint8_t            mod_wheel;      /* 0 .. 127 */
    uint8_t            aftertouch;     /* 0 .. 127 */
    int32_t            smooth_pb_q20;  /* De-zippered pitch bend: pitch_bend_q13 * 128 */
    int32_t            smooth_mw_q15;  /* De-zippered mod wheel: 0 .. 32767 */
    int32_t            smooth_at_q15;  /* De-zippered aftertouch: 0 .. 32767 */
    bool               sustain_pedal;
    uint8_t            rr_cursor;      /* Round-robin cursor within this group's owned voices */
    uint8_t            mono_notes[16]; /* Held-note stack for Unison & 1-voice legato */
    uint8_t            mono_vels[16];
    uint8_t            mono_len;
    uint8_t            last_note;
    adrift_arp_state_t arp_state;
} adrift_group_t;

typedef struct {
    char           name[16];
    adrift_group_t groups[ADRIFT_MAX_GROUPS];
} adrift_preset_t;

/* Compact snapshot of all user settings for non-destructive Presets Browser preview & cancel */
typedef struct {
    uint8_t        num_voices;
    uint8_t        assign_mode;
    uint8_t        midi_channel;
    int16_t        unison_detune;
    int16_t        stereo_spread;
    int16_t        pan;
    int8_t         transpose;
    uint8_t        last_note;
    adrift_patch_t patch;
} adrift_group_snapshot_t;

typedef struct {
    char                    preset_name[16];
    uint8_t                 preset_index;
    uint8_t                 edit_group;
    adrift_group_snapshot_t groups[ADRIFT_MAX_GROUPS];
    adrift_fx_params_t      fx_params;
} adrift_synth_snapshot_t;

typedef struct {
    bool               active;
    bool               key_down;
    bool               sustained;
    int8_t             owner_group; /* -1 = unallocated free pool, 0..5 = G1..G6 */
    uint8_t            group_slot;  /* 0..num_voices-1 index within owner_group */
    uint8_t            note;
    uint8_t            velocity;
    uint32_t           age;
    uint32_t           lfo_phase;
    uint32_t           lfo2_phase;
    uint32_t           lfo3_phase;
    uint32_t           lfo_rng;
    int32_t            lfo_sh_q15;
    int32_t            lfo2_sh_q15;
    int32_t            lfo3_sh_q15;
    adrift_exciter_t   exc;
    adrift_env_t       env1;
    adrift_svf_lpf12_t in_lpf;
    adrift_bbd_t       bbd;
    adrift_env_t       env2;
    int32_t            dry_gain_q15;
    int32_t            wet_gain_q15;
    int32_t            grp_gain_l_q15;
    int32_t            grp_gain_r_q15;
    bool               pan_cache_valid;
    uint8_t            cached_pan_mode;
    uint8_t            cached_pan_nv;
    uint8_t            cached_pan_slot;
    int16_t            cached_eff_pan;
    int16_t            cached_eff_spread;
    int32_t            cached_pan_l_q15;
    int32_t            cached_pan_r_q15;
} __attribute__((aligned(64))) adrift_voice_t;

typedef struct {
    char            preset_name[16];
    uint8_t         preset_index;
    uint8_t         edit_group;     /* 0..5 = G1..G6 (SELECT knob cycles) */
    int8_t          kbd_octave;     /* -3 .. +3 for built-in / QWERTY mini-keyboard */
    uint8_t         kbd_scale;      /* 0..15 (adrift_scale_t, default 0 = Chromatic) */
    uint8_t         kbd_scale_root; /* 0..11 (C..B root note, default 0 = C) */
    uint8_t         midi_bank;      /* 0 = Factory P1..P24, 1 = User U1..U24 (CC0/CC32) */

    /* 6 independent Voice Groups */
    adrift_group_t  groups[ADRIFT_MAX_GROUPS];

    uint32_t        voice_age_counter;
    int32_t         lfo_out_q15;

    /* 16 hardware DSP voices */
    adrift_voice_t  voices[ADRIFT_MAX_VOICES];

    /* Master Stereo Bus / Send Effects (Chorus -> Ping-Pong Delay -> Shimmer Reverb) */
    adrift_fx_params_t fx_params;
    adrift_fx_state_t  fx;

    /* External MIDI Clock Sync (24 PPQN: 0xF8 Timing Clock, 0xFA Start, 0xFB Continue, 0xFC Stop) */
    bool            midi_clk_active;          /* True while locked to incoming 0xF8 clock stream */
    bool            midi_clk_running;         /* True after 0xFA Start / 0xFB Continue, false after 0xFC Stop */
    uint16_t        midi_clk_bpm;             /* Detected external tempo (30..300 BPM, default 120) */
    uint32_t        midi_clk_period_q4;       /* Smoothed samples per 0xF8 pulse in Q4 (1/24th quarter note) */
    uint32_t        midi_clk_since_last;      /* Samples elapsed since previous 0xF8 pulse */
    uint32_t        midi_clk_timeout_samples; /* Watchdog countdown (2s) before falling back to internal clock */
    uint32_t        midi_clk_pulse_cnt;       /* Monotonic 24 PPQN pulse counter since 0xFA Start */

    /* Hardware master pot, 16-bit TPDF dither RNG & UI telemetry */
    int32_t         master_gain_q15;
    int16_t         hw_master_pot; /* -1 = ignore, else 0..1023 hardware pot scale */
    uint32_t        dither_rng;
    int16_t         scope_ring[ADRIFT_SCOPE_LEN];
    int16_t         scope_ring_l[ADRIFT_SCOPE_LEN];
    int16_t         scope_ring_r[ADRIFT_SCOPE_LEN];
    uint8_t         scope_wr;
    bool            scope_frozen;
    int32_t         peak_out_q15;
    uint8_t         last_note;
    bool            any_gate;
} adrift_synth_t;

void adrift_synth_init(adrift_synth_t *s);
void adrift_synth_set_master_pot(adrift_synth_t *s, int16_t pot_0_1023, bool snap_gain);
void adrift_synth_load_preset(adrift_synth_t *s, uint8_t index);
void adrift_synth_init_patch(adrift_synth_t *s);
void adrift_synth_init_group_patch(adrift_synth_t *s, uint8_t g);
void adrift_synth_save_snapshot(const adrift_synth_t *s, adrift_synth_snapshot_t *snap);
void adrift_synth_restore_snapshot(adrift_synth_t *s, const adrift_synth_snapshot_t *snap);
const adrift_patch_t *adrift_synth_get_factory_patch(uint8_t index);
const char *adrift_synth_get_preset_name(uint8_t index);
void adrift_synth_get_preset_summary(uint8_t index, uint8_t *out_total_voices, uint8_t *out_active_groups);
uint8_t     adrift_synth_get_preset_group_voices(uint8_t preset_id, uint8_t g);
bool        adrift_synth_load_preset_group(adrift_synth_t *s, uint8_t preset_id, uint8_t src_group, uint8_t dst_group);

/* Persistent User Presets (U1..U24) & flash backup commit queue */
void        adrift_preset_store_init(void);
bool        adrift_preset_store_flush_pending(void);
bool        adrift_synth_user_preset_is_occupied(uint8_t user_slot);
uint8_t     adrift_synth_first_empty_user_preset(void);
const char *adrift_synth_get_user_preset_name(uint8_t user_slot);
void        adrift_synth_get_user_preset_summary(uint8_t user_slot, uint8_t *out_total_voices, uint8_t *out_active_groups);
const adrift_synth_snapshot_t *adrift_synth_get_user_preset_snapshot(uint8_t user_slot);
bool        adrift_synth_save_user_preset(adrift_synth_t *s, uint8_t user_slot, const char *name);
bool        adrift_synth_clear_user_preset(adrift_synth_t *s, uint8_t user_slot);

const char *adrift_synth_get_scale_name(uint8_t scale_idx);
uint8_t     adrift_synth_get_scale_num_notes(uint8_t scale_idx);
const char *adrift_synth_get_root_name(uint8_t root_idx);
int32_t     adrift_synth_map_kbd_key(const adrift_synth_t *s, int32_t semi_from_c4);

/* Voice Group management & pool queries */
uint8_t adrift_synth_used_voices(const adrift_synth_t *s);
uint8_t adrift_synth_free_voices(const adrift_synth_t *s);
bool    adrift_synth_set_group_voices(adrift_synth_t *s, uint8_t g, uint8_t desired_voices);
void    adrift_synth_wake_group_drone(adrift_synth_t *s, uint8_t g);
void    adrift_synth_cycle_edit_group(adrift_synth_t *s);
void    adrift_synth_set_group_arp_mode(adrift_synth_t *s, uint8_t g, adrift_arp_mode_t mode);
void    adrift_synth_sync_group_arp(adrift_synth_t *s, uint8_t g);
void    adrift_synth_compute_stack_slot_voicing(const adrift_group_t *gp,
                                                uint8_t slot,
                                                int32_t spread_q8,
                                                int32_t *out_semi_q8,
                                                int32_t *out_detune_q8,
                                                int32_t *out_slot_gain_q15);
void    adrift_synth_compute_voice_pan_gains_q15(const adrift_group_t *gp,
                                                 uint8_t slot,
                                                 int32_t *out_pan_l_q15,
                                                 int32_t *out_pan_r_q15);

static inline adrift_group_t *adrift_synth_active_group_mut(adrift_synth_t *s) {
    return &s->groups[s->edit_group % ADRIFT_MAX_GROUPS];
}

static inline const adrift_group_t *adrift_synth_active_group(const adrift_synth_t *s) {
    return &s->groups[s->edit_group % ADRIFT_MAX_GROUPS];
}

static inline const adrift_patch_t *adrift_synth_view_patch(const adrift_synth_t *s) {
    return &s->groups[s->edit_group % ADRIFT_MAX_GROUPS].patch;
}

/* MIDI & per-group note control */
void adrift_synth_handle_midi(adrift_synth_t *s, uint8_t status, uint8_t data1, uint8_t data2);
void adrift_synth_handle_midi_realtime(adrift_synth_t *s, uint8_t status);
void adrift_synth_group_note_on(adrift_synth_t *s, uint8_t g, uint8_t note, uint8_t velocity);
void adrift_synth_group_note_off(adrift_synth_t *s, uint8_t g, uint8_t note);
void adrift_synth_group_all_notes_off(adrift_synth_t *s, uint8_t g, bool clear_bbd);
void adrift_synth_note_on(adrift_synth_t *s, uint8_t note, uint8_t velocity);
void adrift_synth_note_off(adrift_synth_t *s, uint8_t note);
void adrift_synth_all_notes_off(adrift_synth_t *s, bool clear_bbd);

/* Render stereo interleaved 16-bit (TPDF-dithered) or 24-bit Q23 audio with sample-offset MIDI splitting */
void adrift_synth_render_block(adrift_synth_t *s,
                               int16_t *stereo_out,
                               uint32_t frames,
                               const adrift_midi_msg_t *midi_events,
                               uint32_t num_midi);
void adrift_synth_render_block_q23(adrift_synth_t *s,
                                   int32_t *stereo_q23_out,
                                   uint32_t frames,
                                   const adrift_midi_msg_t *midi_events,
                                   uint32_t num_midi);
