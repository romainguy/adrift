#include "synth_internal.h"

#define ADRIFT_NUM_PATCH_TEMPLATES 32u

static const adrift_patch_t k_factory_patches[ADRIFT_NUM_PATCH_TEMPLATES] = {
    /* 0: Classic plucked acoustic guitar / BBD string (ECHON "Guitar" / Karplus-Strong) */
    {
        .name = "ACOUST GUIT",
        .exc = {
            .wave_morph = 620, .pulse_width = 512, .vco_level = 920,
            .vco_octave = 0, .vco_semi = 0, .vco_fine = 0,
            .analog_drift = 280, .vco_keytrack = true,
            .noise_level = 680, .sh_rate = 1023
        },
        .env1 = {
            .onset_delay = 0, .attack = 10, .decay = 190, .sustain = 0,
            .release = 160, .vel_sens = 800, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .in_lpf_cutoff = 780,
        .in_lpf_res    = 180,
        .bbd = {
            .feedback = 880, /* ~98.9% singing acoustic string decay */
            .fb_lpf_cutoff = 710, .glide = 0,
            .invert = false, .invert_pitch_comp = false, .bbd_noise = 200
        },
        .dry_wet    = 820,
        .env2 = {
            .onset_delay = 0, .attack = 8, .decay = 680, .sustain = 900,
            .release = 620, .vel_sens = 320, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .master_vol = 860,
        .lfo_wave = ADRIFT_LFO_SINE, .lfo_rate = 380, .lfo_depth = 12,
        .lfo_dest = ADRIFT_MOD_BBD_PITCH, .mw_depth = 180, .pb_range = 2, .legato = false
    },
    /* 1: Bowed Cello Swell showcasing Env 1 Onset Delay + Micro-ACB Saw */
    {
        .name = "BOWED CELLO",
        .exc = {
            .wave_morph = 660, .pulse_width = 480, .vco_level = 720,
            .vco_octave = 0, .vco_semi = 0, .vco_fine = 4,
            .analog_drift = 380, .vco_keytrack = true,
            .noise_level = 220, .sh_rate = 820
        },
        .env1 = {
            .onset_delay = 360, /* ~250 ms onset delay before bow blooms */
            .attack = 560, .decay = 640, .sustain = 680,
            .release = 520, .vel_sens = 600, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .in_lpf_cutoff = 640,
        .in_lpf_res    = 240,
        .bbd = {
            .feedback = 855, .fb_lpf_cutoff = 640, .glide = 180,
            .invert = false, .invert_pitch_comp = false, .bbd_noise = 220
        },
        .dry_wet    = 760,
        .env2 = {
            .onset_delay = 0, .attack = 60, .decay = 700, .sustain = 950,
            .release = 680, .vel_sens = 400, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .master_vol = 850,
        .lfo_wave = ADRIFT_LFO_SINE, .lfo_rate = 410, .lfo_depth = 28,
        .lfo_dest = ADRIFT_MOD_BBD_PITCH, .mw_depth = 260, .pb_range = 2, .legato = false
    },
    /* 2: Hollow Clarinet / Flute Pipe using Polarity Invert + Pitch Comp */
    {
        .name = "HOLLOW PIPE",
        .exc = {
            .wave_morph = 300, .pulse_width = 512, .vco_level = 840,
            .vco_octave = 0, .vco_semi = 0, .vco_fine = 0,
            .analog_drift = 300, .vco_keytrack = true,
            .noise_level = 480, .sh_rate = 920
        },
        .env1 = {
            .onset_delay = 0, .attack = 260, .decay = 520, .sustain = 440,
            .release = 380, .vel_sens = 650, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .in_lpf_cutoff = 680,
        .in_lpf_res    = 220,
        .bbd = {
            .feedback = 875, .fb_lpf_cutoff = 630, .glide = 110,
            .invert = true, .invert_pitch_comp = true, .bbd_noise = 240
        },
        .dry_wet    = 880,
        .env2 = {
            .onset_delay = 0, .attack = 85, .decay = 600, .sustain = 920,
            .release = 540, .vel_sens = 450, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .master_vol = 860,
        .lfo_wave = ADRIFT_LFO_TRI, .lfo_rate = 430, .lfo_depth = 20,
        .lfo_dest = ADRIFT_MOD_BBD_PITCH, .mw_depth = 220, .pb_range = 2, .legato = true
    },
    /* 3: Pure BBD Self-Oscillation (>100% FB, zero exciter, ECHON "Auto feedback") */
    {
        .name = "AUTO FEEDBK",
        .exc = {
            .wave_morph = 0, .pulse_width = 512, .vco_level = 0,
            .vco_octave = 0, .vco_semi = 0, .vco_fine = 0,
            .analog_drift = 340, .vco_keytrack = true,
            .noise_level = 0, .sh_rate = 1023
        },
        .env1 = {
            .onset_delay = 0, .attack = 20, .decay = 300, .sustain = 0,
            .release = 200, .vel_sens = 0, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .in_lpf_cutoff = 700,
        .in_lpf_res    = 100,
        .bbd = {
            .feedback = 965, /* ~108% self-oscillation seeded by BBD thermal floor! */
            .fb_lpf_cutoff = 580, .glide = 220,
            .invert = false, .invert_pitch_comp = false, .bbd_noise = 420
        },
        .dry_wet    = 1023,
        .env2 = {
            .onset_delay = 0, .attack = 340, .decay = 580, .sustain = 860,
            .release = 560, .vel_sens = 500, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .master_vol = 800,
        .lfo_wave = ADRIFT_LFO_SINE, .lfo_rate = 440, .lfo_depth = 24,
        .lfo_dest = ADRIFT_MOD_BBD_PITCH, .mw_depth = 300, .pb_range = 2, .legato = true
    },
    /* 4: Looping A-D Exciter Envelope (Mandolin / Tremolo Resonator) */
    {
        .name = "MANDOLIN LP",
        .exc = {
            .wave_morph = 750, .pulse_width = 320, .vco_level = 880,
            .vco_octave = 0, .vco_semi = 0, .vco_fine = 0,
            .analog_drift = 260, .vco_keytrack = true,
            .noise_level = 610, .sh_rate = 880
        },
        .env1 = {
            .onset_delay = 0, .attack = 35, .decay = 280, .sustain = 0,
            .release = 220, .vel_sens = 700, .loop_mode = ADRIFT_ENV_LOOP_AD, .drone_hold = false
        },
        .in_lpf_cutoff = 720,
        .in_lpf_res    = 260,
        .bbd = {
            .feedback = 850, .fb_lpf_cutoff = 720, .glide = 0,
            .invert = false, .invert_pitch_comp = false, .bbd_noise = 180
        },
        .dry_wet    = 800,
        .env2 = {
            .onset_delay = 0, .attack = 12, .decay = 500, .sustain = 940,
            .release = 480, .vel_sens = 400, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .master_vol = 840,
        .lfo_wave = ADRIFT_LFO_TRI, .lfo_rate = 320, .lfo_depth = 120,
        .lfo_dest = ADRIFT_MOD_IN_LPF, .mw_depth = 320, .pb_range = 2, .legato = false
    },
    /* 5: Stepped S&H Noise + Inverted Pitch-Compensated Modal Pluck (ECHON "Mistery pluck") */
    {
        .name = "MISTERY PLK",
        .exc = {
            .wave_morph = 160, .pulse_width = 512, .vco_level = 920,
            .vco_octave = 1, .vco_semi = 7, .vco_fine = 0,
            .analog_drift = 320, .vco_keytrack = true,
            .noise_level = 700, .sh_rate = 540, .pluck = 520 /* crunchy S&H clock + mallet strike */
        },
        .env1 = {
            .onset_delay = 0, .attack = 8, .decay = 260, .sustain = 0,
            .release = 220, .vel_sens = 820, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .in_lpf_cutoff = 820,
        .in_lpf_res    = 460,
        .bbd = {
            .feedback = 876, .fb_lpf_cutoff = 820, .glide = 0,
            .invert = true, .invert_pitch_comp = true, .bbd_noise = 200, .dispersion = 420
        },
        .dry_wet    = 840,
        .env2 = {
            .onset_delay = 0, .attack = 6, .decay = 760, .sustain = 650,
            .release = 720, .vel_sens = 500, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .master_vol = 840,
        .lfo_wave = ADRIFT_LFO_SH, .lfo_rate = 520, .lfo_depth = 180,
        .lfo_dest = ADRIFT_MOD_IN_LPF, .mw_depth = 280, .pb_range = 2, .legato = false
    },
    /* 6: Micro-ACB Virtual Analog PWM/Saw Bass with BBD Sub-Body */
    {
        .name = "SUPER BASS",
        .exc = {
            .wave_morph = 860, .pulse_width = 360, .vco_level = 920,
            .vco_octave = -1, .vco_semi = 0, .vco_fine = 0,
            .analog_drift = 450, .vco_keytrack = true,
            .noise_level = 90, .sh_rate = 1023, .pluck = 580
        },
        .env1 = {
            .onset_delay = 0, .attack = 10, .decay = 520, .sustain = 620,
            .release = 320, .vel_sens = 550, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .in_lpf_cutoff = 720,
        .in_lpf_res    = 440,
        .bbd = {
            .feedback = 855, .fb_lpf_cutoff = 650, .glide = 90,
            .invert = false, .invert_pitch_comp = false, .bbd_noise = 160
        },
        .dry_wet    = 580, /* balanced ACB VCO + resonant BBD body */
        .env2 = {
            .onset_delay = 0, .attack = 8, .decay = 560, .sustain = 880,
            .release = 340, .vel_sens = 380, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .master_vol = 940,
        .lfo_wave = ADRIFT_LFO_TRI, .lfo_rate = 290, .lfo_depth = 180,
        .lfo_dest = ADRIFT_MOD_VCO_PWM, .mw_depth = 260,
        .pb_range = 2, .legato = true
    },
    /* 7: Hands-free Self-Oscillating Ambient Drone (Env 2 Drone Hold = ON) */
    {
        .name = "INF DRONE",
        .exc = {
            .wave_morph = 420, .pulse_width = 512, .vco_level = 240,
            .vco_octave = -1, .vco_semi = 0, .vco_fine = -5,
            .analog_drift = 520, .vco_keytrack = true,
            .noise_level = 180, .sh_rate = 640
        },
        .env1 = {
            .onset_delay = 180, .attack = 480, .decay = 680, .sustain = 300,
            .release = 620, .vel_sens = 300, .loop_mode = ADRIFT_ENV_LOOP_DAD, .drone_hold = false
        },
        .in_lpf_cutoff = 560,
        .in_lpf_res    = 310,
        .bbd = {
            .feedback = 945, /* ~106% continuous self-oscillation */
            .fb_lpf_cutoff = 520, .glide = 420,
            .invert = true, .invert_pitch_comp = true, .bbd_noise = 320
        },
        .dry_wet    = 900,
        .env2 = {
            .onset_delay = 0, .attack = 200, .decay = 500, .sustain = 1023,
            .release = 800, .vel_sens = 0, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = true
        },
        .master_vol = 760,
        .lfo_wave = ADRIFT_LFO_SINE, .lfo_rate = 180, .lfo_depth = 180,
        .lfo_dest = ADRIFT_MOD_FB_LPF,
        .lfo2_wave = ADRIFT_LFO_TRI, .lfo2_rate = 130, .lfo2_depth = 16,
        .lfo2_dest = ADRIFT_MOD_BBD_PITCH,
        .lfo3_wave = ADRIFT_LFO_SINE, .lfo3_rate = 240, .lfo3_depth = 190,
        .lfo3_dest = ADRIFT_MOD_VCO_MORPH,
        .mw_depth = 300, .pb_range = 12, .legato = true
    },
    /* 8: Strummer String #2 (~120 ms onset delay, bright acoustic pluck) */
    {
        .name = "STRUM DLY 1",
        .exc = {
            .wave_morph = 650, .pulse_width = 480, .vco_level = 920,
            .vco_octave = 0, .vco_semi = 0, .vco_fine = 3,
            .analog_drift = 300, .vco_keytrack = true,
            .noise_level = 650, .sh_rate = 1023
        },
        .env1 = {
            .onset_delay = 250, .attack = 8, .decay = 195, .sustain = 0,
            .release = 160, .vel_sens = 780, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .in_lpf_cutoff = 800,
        .in_lpf_res    = 200,
        .bbd = {
            .feedback = 881, .fb_lpf_cutoff = 730, .glide = 0,
            .invert = false, .invert_pitch_comp = false, .bbd_noise = 190
        },
        .dry_wet    = 830,
        .env2 = {
            .onset_delay = 0, .attack = 6, .decay = 680, .sustain = 900,
            .release = 620, .vel_sens = 320, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .master_vol = 850,
        .lfo_wave = ADRIFT_LFO_SINE, .lfo_rate = 390, .lfo_depth = 10,
        .lfo_dest = ADRIFT_MOD_BBD_PITCH, .mw_depth = 180, .pb_range = 2, .legato = false
    },
    /* 9: Strummer String #3 (~235 ms onset delay, upper chord string) */
    {
        .name = "STRUM DLY 2",
        .exc = {
            .wave_morph = 700, .pulse_width = 420, .vco_level = 920,
            .vco_octave = 0, .vco_semi = 0, .vco_fine = -4,
            .analog_drift = 320, .vco_keytrack = true,
            .noise_level = 630, .sh_rate = 1023
        },
        .env1 = {
            .onset_delay = 350, .attack = 8, .decay = 190, .sustain = 0,
            .release = 160, .vel_sens = 760, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .in_lpf_cutoff = 820,
        .in_lpf_res    = 220,
        .bbd = {
            .feedback = 882, .fb_lpf_cutoff = 760, .glide = 0,
            .invert = false, .invert_pitch_comp = false, .bbd_noise = 190
        },
        .dry_wet    = 840,
        .env2 = {
            .onset_delay = 0, .attack = 6, .decay = 680, .sustain = 900,
            .release = 620, .vel_sens = 320, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .master_vol = 840,
        .lfo_wave = ADRIFT_LFO_SINE, .lfo_rate = 410, .lfo_depth = 12,
        .lfo_dest = ADRIFT_MOD_BBD_PITCH, .mw_depth = 180, .pb_range = 2, .legato = false
    },
    /* 10: Strummer String #4 (~355 ms onset delay, high harp chikari string) */
    {
        .name = "STRUM DLY 3",
        .exc = {
            .wave_morph = 760, .pulse_width = 360, .vco_level = 920,
            .vco_octave = 0, .vco_semi = 0, .vco_fine = 5,
            .analog_drift = 320, .vco_keytrack = true,
            .noise_level = 610, .sh_rate = 1023
        },
        .env1 = {
            .onset_delay = 430, .attack = 8, .decay = 185, .sustain = 0,
            .release = 160, .vel_sens = 740, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .in_lpf_cutoff = 850,
        .in_lpf_res    = 240,
        .bbd = {
            .feedback = 884, .fb_lpf_cutoff = 790, .glide = 0,
            .invert = false, .invert_pitch_comp = false, .bbd_noise = 190
        },
        .dry_wet    = 850,
        .env2 = {
            .onset_delay = 0, .attack = 6, .decay = 680, .sustain = 900,
            .release = 620, .vel_sens = 320, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .master_vol = 830,
        .lfo_wave = ADRIFT_LFO_SINE, .lfo_rate = 430, .lfo_depth = 14,
        .lfo_dest = ADRIFT_MOD_BBD_PITCH, .mw_depth = 180, .pb_range = 2, .legato = false
    },
    /* 11: Slowpad Warm PWM + BBD Pad (ECHON "Slowpad" Layer A) */
    {
        .name = "SLOWPAD PWM",
        .exc = {
            .wave_morph = 840, .pulse_width = 420, .vco_level = 820,
            .vco_octave = 0, .vco_semi = 0, .vco_fine = -4,
            .analog_drift = 520, .vco_keytrack = true,
            .noise_level = 120, .sh_rate = 1023
        },
        .env1 = {
            .onset_delay = 0, .attack = 380, .decay = 680, .sustain = 840,
            .release = 680, .vel_sens = 450, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .in_lpf_cutoff = 650,
        .in_lpf_res    = 260,
        .bbd = {
            .feedback = 848, .fb_lpf_cutoff = 600, .glide = 140,
            .invert = false, .invert_pitch_comp = false, .bbd_noise = 220
        },
        .dry_wet    = 560,
        .env2 = {
            .onset_delay = 0, .attack = 340, .decay = 700, .sustain = 920,
            .release = 720, .vel_sens = 380, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .master_vol = 840,
        .lfo_wave = ADRIFT_LFO_SINE, .lfo_rate = 260, .lfo_depth = 220,
        .lfo_dest = ADRIFT_MOD_VCO_PWM,
        .lfo2_wave = ADRIFT_LFO_TRI, .lfo2_rate = 150, .lfo2_depth = 140,
        .lfo2_dest = ADRIFT_MOD_IN_LPF,
        .lfo3_wave = ADRIFT_LFO_SINE, .lfo3_rate = 340, .lfo3_depth = 14,
        .lfo3_dest = ADRIFT_MOD_BBD_PITCH,
        .mw_depth = 280, .pb_range = 2, .legato = false
    },
    /* 12: Slowpad Shimmering BBD Resonator Layer (ECHON "Slowpad" Layer B) */
    {
        .name = "SLOWPAD SHM",
        .exc = {
            .wave_morph = 260, .pulse_width = 512, .vco_level = 680,
            .vco_octave = 0, .vco_semi = 0, .vco_fine = 6,
            .analog_drift = 560, .vco_keytrack = true,
            .noise_level = 180, .sh_rate = 900
        },
        .env1 = {
            .onset_delay = 160, .attack = 420, .decay = 680, .sustain = 800,
            .release = 700, .vel_sens = 480, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .in_lpf_cutoff = 700,
        .in_lpf_res    = 220,
        .bbd = {
            .feedback = 868, .fb_lpf_cutoff = 660, .glide = 180,
            .invert = false, .invert_pitch_comp = false, .bbd_noise = 240
        },
        .dry_wet    = 780,
        .env2 = {
            .onset_delay = 0, .attack = 380, .decay = 720, .sustain = 940,
            .release = 740, .vel_sens = 380, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .master_vol = 820,
        .lfo_wave = ADRIFT_LFO_TRI, .lfo_rate = 310, .lfo_depth = 24,
        .lfo_dest = ADRIFT_MOD_BBD_PITCH,
        .lfo2_wave = ADRIFT_LFO_SINE, .lfo2_rate = 190, .lfo2_depth = 180,
        .lfo2_dest = ADRIFT_MOD_VCO_MORPH,
        .lfo3_wave = ADRIFT_LFO_TRI, .lfo3_rate = 120, .lfo3_depth = 130,
        .lfo3_dest = ADRIFT_MOD_FB_LPF,
        .mw_depth = 260, .pb_range = 2, .legato = false
    },
    /* 13: Melancholy Bath Tape-Warbled BBD Keys (ECHON "Melancholy bath" Layer A) */
    {
        .name = "MELANCH KEY",
        .exc = {
            .wave_morph = 210, .pulse_width = 512, .vco_level = 740,
            .vco_octave = 0, .vco_semi = 0, .vco_fine = -3,
            .analog_drift = 720, .vco_keytrack = true,
            .noise_level = 240, .sh_rate = 780
        },
        .env1 = {
            .onset_delay = 0, .attack = 28, .decay = 480, .sustain = 280,
            .release = 520, .vel_sens = 680, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .in_lpf_cutoff = 580,
        .in_lpf_res    = 220,
        .bbd = {
            .feedback = 850, .fb_lpf_cutoff = 530, .glide = 80,
            .invert = false, .invert_pitch_comp = false, .bbd_noise = 390
        },
        .dry_wet    = 720,
        .env2 = {
            .onset_delay = 0, .attack = 20, .decay = 680, .sustain = 720,
            .release = 680, .vel_sens = 420, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .master_vol = 860,
        .lfo_wave = ADRIFT_LFO_TRI, .lfo_rate = 270, .lfo_depth = 36,
        .lfo_dest = ADRIFT_MOD_BBD_PITCH, .mw_depth = 280, .pb_range = 2, .legato = false
    },
    /* 14: Melancholy Bath Delayed Ghost Halo (ECHON "Melancholy bath" Layer B) */
    {
        .name = "MELANCH HLO",
        .exc = {
            .wave_morph = 140, .pulse_width = 512, .vco_level = 480,
            .vco_octave = 0, .vco_semi = 0, .vco_fine = 5,
            .analog_drift = 680, .vco_keytrack = true,
            .noise_level = 320, .sh_rate = 720
        },
        .env1 = {
            .onset_delay = 360, .attack = 440, .decay = 620, .sustain = 500,
            .release = 640, .vel_sens = 540, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .in_lpf_cutoff = 540,
        .in_lpf_res    = 260,
        .bbd = {
            .feedback = 872, .fb_lpf_cutoff = 500, .glide = 160,
            .invert = true, .invert_pitch_comp = true, .bbd_noise = 360
        },
        .dry_wet    = 900,
        .env2 = {
            .onset_delay = 0, .attack = 120, .decay = 700, .sustain = 880,
            .release = 720, .vel_sens = 380, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .master_vol = 800,
        .lfo_wave = ADRIFT_LFO_SINE, .lfo_rate = 230, .lfo_depth = 140,
        .lfo_dest = ADRIFT_MOD_FB_LPF, .mw_depth = 240, .pb_range = 2, .legato = false
    },
    /* 15: 7 12 Glitter Looping D-A-D Crystal Chime (ECHON "7 12 glitter") */
    {
        .name = "GLITTER LP",
        .exc = {
            .wave_morph = 180, .pulse_width = 512, .vco_level = 900,
            .vco_octave = 0, .vco_semi = 0, .vco_fine = 4,
            .analog_drift = 320, .vco_keytrack = true,
            .noise_level = 610, .sh_rate = 740
        },
        .env1 = {
            .onset_delay = 270, .attack = 25, .decay = 210, .sustain = 0,
            .release = 220, .vel_sens = 720, .loop_mode = ADRIFT_ENV_LOOP_DAD, .drone_hold = false
        },
        .in_lpf_cutoff = 860,
        .in_lpf_res    = 380,
        .bbd = {
            .feedback = 874, .fb_lpf_cutoff = 840, .glide = 0,
            .invert = false, .invert_pitch_comp = false, .bbd_noise = 180
        },
        .dry_wet    = 880,
        .env2 = {
            .onset_delay = 0, .attack = 12, .decay = 680, .sustain = 920,
            .release = 620, .vel_sens = 400, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .master_vol = 820,
        .lfo_wave = ADRIFT_LFO_SH, .lfo_rate = 560, .lfo_depth = 180,
        .lfo_dest = ADRIFT_MOD_IN_LPF, .mw_depth = 300, .pb_range = 2, .legato = false
    },
    /* 16: Sub-Octave Hollow Tube (Invert = ON, Pitch Comp = OFF -> sounds at f0/2) */
    {
        .name = "SUB TUBE",
        .exc = {
            .wave_morph = 340, .pulse_width = 512, .vco_level = 820,
            .vco_octave = 0, .vco_semi = 0, .vco_fine = 0,
            .analog_drift = 340, .vco_keytrack = true,
            .noise_level = 460, .sh_rate = 860
        },
        .env1 = {
            .onset_delay = 0, .attack = 35, .decay = 420, .sustain = 320,
            .release = 380, .vel_sens = 640, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .in_lpf_cutoff = 660,
        .in_lpf_res    = 260,
        .bbd = {
            .feedback = 870, .fb_lpf_cutoff = 600, .glide = 110,
            .invert = true, .invert_pitch_comp = false, .bbd_noise = 220
        },
        .dry_wet    = 880,
        .env2 = {
            .onset_delay = 0, .attack = 24, .decay = 620, .sustain = 880,
            .release = 540, .vel_sens = 420, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .master_vol = 840,
        .lfo_wave = ADRIFT_LFO_TRI, .lfo_rate = 340, .lfo_depth = 18,
        .lfo_dest = ADRIFT_MOD_BBD_PITCH, .mw_depth = 220, .pb_range = 2, .legato = false
    },
    /* 17: Random Pluck A — Per-voice S&H LFO -> VCO Morph (ECHON "Random pluck 1") */
    {
        .name = "RND PLUCK A",
        .exc = {
            .wave_morph = 512, .pulse_width = 400, .vco_level = 920,
            .vco_octave = 0, .vco_semi = 0, .vco_fine = 0,
            .analog_drift = 420, .vco_keytrack = true,
            .noise_level = 440, .sh_rate = 680
        },
        .env1 = {
            .onset_delay = 0, .attack = 6, .decay = 230, .sustain = 0,
            .release = 180, .vel_sens = 820, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .in_lpf_cutoff = 820,
        .in_lpf_res    = 420,
        .bbd = {
            .feedback = 878, .fb_lpf_cutoff = 790, .glide = 0,
            .invert = false, .invert_pitch_comp = false, .bbd_noise = 200
        },
        .dry_wet    = 780,
        .env2 = {
            .onset_delay = 0, .attack = 6, .decay = 680, .sustain = 820,
            .release = 580, .vel_sens = 400, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .master_vol = 860,
        .lfo_wave = ADRIFT_LFO_SH, .lfo_rate = 680, .lfo_depth = 460,
        .lfo_dest = ADRIFT_MOD_VCO_MORPH, .mw_depth = 340,
        .lfo2_wave = ADRIFT_LFO_SH, .lfo2_rate = 520, .lfo2_depth = 220,
        .lfo2_dest = ADRIFT_MOD_IN_LPF,
        .pb_range = 2, .legato = false
    },
    /* 18: Random Pluck B — Per-voice S&H LFO -> Feedback LPF (ECHON "Random pluck 2") */
    {
        .name = "RND PLUCK B",
        .exc = {
            .wave_morph = 780, .pulse_width = 260, .vco_level = 920,
            .vco_octave = 0, .vco_semi = 0, .vco_fine = 4,
            .analog_drift = 440, .vco_keytrack = true,
            .noise_level = 520, .sh_rate = 520
        },
        .env1 = {
            .onset_delay = 0, .attack = 10, .decay = 250, .sustain = 0,
            .release = 200, .vel_sens = 800, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .in_lpf_cutoff = 760,
        .in_lpf_res    = 540,
        .bbd = {
            .feedback = 880, .fb_lpf_cutoff = 720, .glide = 0,
            .invert = true, .invert_pitch_comp = true, .bbd_noise = 220
        },
        .dry_wet    = 820,
        .env2 = {
            .onset_delay = 0, .attack = 6, .decay = 680, .sustain = 820,
            .release = 580, .vel_sens = 420, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .master_vol = 850,
        .lfo_wave = ADRIFT_LFO_SH, .lfo_rate = 590, .lfo_depth = -420,
        .lfo_dest = ADRIFT_MOD_FB_LPF, .mw_depth = 340, .pb_range = 2, .legato = false
    },
    /* 19: Overdub Tine Key (ECHON "7th overdub" Immediate Fundamental Layer) */
    {
        .name = "OVERDUB KEY",
        .exc = {
            .wave_morph = 120, .pulse_width = 512, .vco_level = 780,
            .vco_octave = 0, .vco_semi = 0, .vco_fine = 0,
            .analog_drift = 380, .vco_keytrack = true,
            .noise_level = 340, .sh_rate = 900, .pluck = 540
        },
        .env1 = {
            .onset_delay = 0, .attack = 8, .decay = 360, .sustain = 180,
            .release = 340, .vel_sens = 760, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .in_lpf_cutoff = 700,
        .in_lpf_res    = 280,
        .bbd = {
            .feedback = 852, .fb_lpf_cutoff = 660, .glide = 0,
            .invert = false, .invert_pitch_comp = false, .bbd_noise = 200, .dispersion = 380
        },
        .dry_wet    = 740,
        .env2 = {
            .onset_delay = 0, .attack = 8, .decay = 620, .sustain = 780,
            .release = 560, .vel_sens = 380, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .master_vol = 860,
        .lfo_wave = ADRIFT_LFO_SINE, .lfo_rate = 340, .lfo_depth = 16,
        .lfo_dest = ADRIFT_MOD_BBD_PITCH, .mw_depth = 220, .pb_range = 2, .legato = false
    },
    /* 20: Overdub Delayed Harmonic Swell (ECHON "7th overdub" +7/+10 ST Bloom Layer) */
    {
        .name = "OVERDUB SWL",
        .exc = {
            .wave_morph = 520, .pulse_width = 480, .vco_level = 640,
            .vco_octave = 0, .vco_semi = 0, .vco_fine = 5,
            .analog_drift = 480, .vco_keytrack = true,
            .noise_level = 200, .sh_rate = 840
        },
        .env1 = {
            .onset_delay = 370, .attack = 420, .decay = 600, .sustain = 640,
            .release = 580, .vel_sens = 580, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .in_lpf_cutoff = 640,
        .in_lpf_res    = 320,
        .bbd = {
            .feedback = 868, .fb_lpf_cutoff = 640, .glide = 120,
            .invert = false, .invert_pitch_comp = false, .bbd_noise = 220
        },
        .dry_wet    = 820,
        .env2 = {
            .onset_delay = 0, .attack = 80, .decay = 680, .sustain = 900,
            .release = 660, .vel_sens = 380, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .master_vol = 800,
        .lfo_wave = ADRIFT_LFO_SINE, .lfo_rate = 340, .lfo_depth = 180,
        .lfo_dest = ADRIFT_MOD_FB_LPF, .mw_depth = 280, .pb_range = 2, .legato = false
    },
    /* 21: Granular Fast Loop A-D Grain Stream (ECHON "Granular-like" Layer A) */
    {
        .name = "GRAIN FAST",
        .exc = {
            .wave_morph = 280, .pulse_width = 512, .vco_level = 920,
            .vco_octave = 0, .vco_semi = 0, .vco_fine = -4,
            .analog_drift = 460, .vco_keytrack = true,
            .noise_level = 560, .sh_rate = 420
        },
        .env1 = {
            .onset_delay = 0, .attack = 16, .decay = 130, .sustain = 0,
            .release = 180, .vel_sens = 680, .loop_mode = ADRIFT_ENV_LOOP_AD, .drone_hold = false
        },
        .in_lpf_cutoff = 760,
        .in_lpf_res    = 480,
        .bbd = {
            .feedback = 862, .fb_lpf_cutoff = 760, .glide = 0,
            .invert = false, .invert_pitch_comp = false, .bbd_noise = 220
        },
        .dry_wet    = 840,
        .env2 = {
            .onset_delay = 0, .attack = 18, .decay = 620, .sustain = 920,
            .release = 580, .vel_sens = 380, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .master_vol = 840,
        .lfo_wave = ADRIFT_LFO_SH, .lfo_rate = 780, .lfo_depth = 360,
        .lfo_dest = ADRIFT_MOD_IN_LPF, .mw_depth = 360, .pb_range = 2, .legato = false
    },
    /* 22: Granular Staggered Loop D-A-D Cloud (ECHON "Granular-like" Layer B) */
    {
        .name = "GRAIN DLY",
        .exc = {
            .wave_morph = 640, .pulse_width = 360, .vco_level = 900,
            .vco_octave = 0, .vco_semi = 0, .vco_fine = 6,
            .analog_drift = 500, .vco_keytrack = true,
            .noise_level = 580, .sh_rate = 360
        },
        .env1 = {
            .onset_delay = 210, .attack = 22, .decay = 155, .sustain = 0,
            .release = 200, .vel_sens = 680, .loop_mode = ADRIFT_ENV_LOOP_DAD, .drone_hold = false
        },
        .in_lpf_cutoff = 780,
        .in_lpf_res    = 440,
        .bbd = {
            .feedback = 870, .fb_lpf_cutoff = 740, .glide = 0,
            .invert = true, .invert_pitch_comp = true, .bbd_noise = 240
        },
        .dry_wet    = 860,
        .env2 = {
            .onset_delay = 0, .attack = 24, .decay = 640, .sustain = 920,
            .release = 600, .vel_sens = 380, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .master_vol = 820,
        .lfo_wave = ADRIFT_LFO_SH, .lfo_rate = 720, .lfo_depth = 320,
        .lfo_dest = ADRIFT_MOD_VCO_MORPH, .mw_depth = 340, .pb_range = 2, .legato = false
    },
    /* 23: Into The Void — Self-Oscillating Groaning Iron Tube (ECHON "Into the void") */
    {
        .name = "VOID IRON",
        .exc = {
            .wave_morph = 720, .pulse_width = 300, .vco_level = 860,
            .vco_octave = 0, .vco_semi = -7, .vco_fine = 0,
            .analog_drift = 680, .vco_keytrack = true,
            .noise_level = 620, .sh_rate = 310
        },
        .env1 = {
            .onset_delay = 0, .attack = 180, .decay = 580, .sustain = 460,
            .release = 540, .vel_sens = 500, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .in_lpf_cutoff = 540,
        .in_lpf_res    = 520,
        .bbd = {
            .feedback = 925, /* Over-unity self-oscillation modulated by LFO */
            .fb_lpf_cutoff = 520, .glide = 380,
            .invert = true, .invert_pitch_comp = false, .bbd_noise = 480
        },
        .dry_wet    = 920,
        .env2 = {
            .onset_delay = 0, .attack = 140, .decay = 640, .sustain = 900,
            .release = 640, .vel_sens = 400, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .master_vol = 820,
        .lfo_wave = ADRIFT_LFO_SINE, .lfo_rate = 240, .lfo_depth = 220,
        .lfo_dest = ADRIFT_MOD_FB_AMT, .mw_depth = 360, .pb_range = 12, .legato = false
    },
    /* 24: Pulsing — Rhythmic Loop A-D Resonant Gate (ECHON "Pulsing") */
    {
        .name = "PULSE GATE",
        .exc = {
            .wave_morph = 880, .pulse_width = 280, .vco_level = 840,
            .vco_octave = 0, .vco_semi = 0, .vco_fine = 0,
            .analog_drift = 360, .vco_keytrack = true,
            .noise_level = 260, .sh_rate = 820
        },
        .env1 = {
            .onset_delay = 0, .attack = 12, .decay = 215, .sustain = 0,
            .release = 180, .vel_sens = 700, .loop_mode = ADRIFT_ENV_LOOP_AD, .drone_hold = false
        },
        .in_lpf_cutoff = 620,
        .in_lpf_res    = 580,
        .bbd = {
            .feedback = 825, .fb_lpf_cutoff = 700, .glide = 0,
            .invert = false, .invert_pitch_comp = false, .bbd_noise = 180
        },
        .dry_wet    = 660,
        .env2 = {
            .onset_delay = 0, .attack = 8, .decay = 500, .sustain = 940,
            .release = 420, .vel_sens = 380, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .master_vol = 860,
        .lfo_wave = ADRIFT_LFO_TRI, .lfo_rate = 360, .lfo_depth = 260,
        .lfo_dest = ADRIFT_MOD_IN_LPF, .mw_depth = 320,
        .lfo2_wave = ADRIFT_LFO_TRI, .lfo2_rate = 220, .lfo2_depth = 240,
        .lfo2_dest = ADRIFT_MOD_VCO_MORPH,
        .lfo3_wave = ADRIFT_LFO_SINE, .lfo3_rate = 140, .lfo3_depth = 160,
        .lfo3_dest = ADRIFT_MOD_ENV1_DEC,
        .pb_range = 2, .legato = false
    },
    /* 25: Sitar Jawari — Dispersive Buzzing Bridge + Sympathetic Resonance */
    {
        .name = "SITAR JAWAR",
        .exc = {
            .wave_morph = 860, .pulse_width = 200, .vco_level = 920,
            .vco_octave = 0, .vco_semi = 0, .vco_fine = 0,
            .analog_drift = 380, .vco_keytrack = true,
            .noise_level = 490, .sh_rate = 940, .pluck = 720
        },
        .env1 = {
            .onset_delay = 0, .attack = 6, .decay = 260, .sustain = 0,
            .release = 220, .vel_sens = 820, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .in_lpf_cutoff = 900,
        .in_lpf_res    = 420,
        .bbd = {
            .feedback = 886, .fb_lpf_cutoff = 900, .glide = 130,
            .invert = false, .invert_pitch_comp = false, .bbd_noise = 240, .dispersion = 820
        },
        .dry_wet    = 860,
        .env2 = {
            .onset_delay = 0, .attack = 6, .decay = 820, .sustain = 880,
            .release = 700, .vel_sens = 360, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .master_vol = 860,
        .lfo_wave = ADRIFT_LFO_SINE, .lfo_rate = 280, .lfo_depth = 220,
        .lfo_dest = ADRIFT_MOD_FB_LPF, .mw_depth = 300,
        .pb_range = 5, .legato = false
    },
    /* 26: Marimba Wood — +2 Oct Mallet Strike into Hollow Inverted Wooden Bar */
    {
        .name = "MARIMBA BAR",
        .exc = {
            .wave_morph = 60, .pulse_width = 512, .vco_level = 940,
            .vco_octave = 2, .vco_semi = 0, .vco_fine = 0,
            .analog_drift = 240, .vco_keytrack = true,
            .noise_level = 500, .sh_rate = 760, .pluck = 840
        },
        .env1 = {
            .onset_delay = 0, .attack = 0, .decay = 120, .sustain = 0,
            .release = 100, .vel_sens = 860, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .in_lpf_cutoff = 760,
        .in_lpf_res    = 360,
        .bbd = {
            .feedback = 866, .fb_lpf_cutoff = 650, .glide = 0,
            .invert = true, .invert_pitch_comp = true, .bbd_noise = 160, .dispersion = 680
        },
        .dry_wet    = 880,
        .env2 = {
            .onset_delay = 0, .attack = 4, .decay = 580, .sustain = 520,
            .release = 480, .vel_sens = 480, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .master_vol = 880,
        .lfo_wave = ADRIFT_LFO_SINE, .lfo_rate = 420, .lfo_depth = 14,
        .lfo_dest = ADRIFT_MOD_BBD_PITCH, .mw_depth = 180, .pb_range = 2, .legato = false
    },
    /* 27: Glass Bowls — +19 ST Sine Exciter Rubbing High-Q Singing BBD Rim */
    {
        .name = "GLASS BOWL",
        .exc = {
            .wave_morph = 0, .pulse_width = 512, .vco_level = 540,
            .vco_octave = 1, .vco_semi = 7, .vco_fine = 0,
            .analog_drift = 260, .vco_keytrack = true,
            .noise_level = 180, .sh_rate = 1023, .pluck = 320
        },
        .env1 = {
            .onset_delay = 0, .attack = 240, .decay = 560, .sustain = 320,
            .release = 520, .vel_sens = 640, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .in_lpf_cutoff = 840,
        .in_lpf_res    = 220,
        .bbd = {
            .feedback = 886, .fb_lpf_cutoff = 860, .glide = 0,
            .invert = false, .invert_pitch_comp = false, .bbd_noise = 160, .dispersion = 640
        },
        .dry_wet    = 900,
        .env2 = {
            .onset_delay = 0, .attack = 80, .decay = 820, .sustain = 920,
            .release = 780, .vel_sens = 420, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .master_vol = 840,
        .lfo_wave = ADRIFT_LFO_SINE, .lfo_rate = 390, .lfo_depth = 18,
        .lfo_dest = ADRIFT_MOD_BBD_PITCH, .mw_depth = 240, .pb_range = 2, .legato = false
    },
    /* 28: Clavinet D6 — Funky Narrow-Pulse Pickup + Resonant Filter Bite */
    {
        .name = "CLAVINET D6",
        .exc = {
            .wave_morph = 1023, .pulse_width = 150, .vco_level = 820,
            .vco_octave = 0, .vco_semi = 0, .vco_fine = 0,
            .analog_drift = 320, .vco_keytrack = true,
            .noise_level = 420, .sh_rate = 1023, .pluck = 780
        },
        .env1 = {
            .onset_delay = 0, .attack = 0, .decay = 180, .sustain = 120,
            .release = 140, .vel_sens = 850, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .in_lpf_cutoff = 880,
        .in_lpf_res    = 560,
        .bbd = {
            .feedback = 860, .fb_lpf_cutoff = 880, .glide = 0,
            .invert = false, .invert_pitch_comp = false, .bbd_noise = 180, .dispersion = 260
        },
        .dry_wet    = 760,
        .env2 = {
            .onset_delay = 0, .attack = 4, .decay = 560, .sustain = 760,
            .release = 320, .vel_sens = 420, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .master_vol = 860,
        .lfo_wave = ADRIFT_LFO_TRI, .lfo_rate = 440, .lfo_depth = 140,
        .lfo_dest = ADRIFT_MOD_VCO_PWM, .mw_depth = 300,
        .pb_range = 2, .legato = false
    },
    /* 29: BBD Chorus — Vintage Polysynth + 50/50 Pitch-Modulated BBD Ensemble */
    {
        .name = "BBD CHORUS",
        .exc = {
            .wave_morph = 780, .pulse_width = 440, .vco_level = 880,
            .vco_octave = 0, .vco_semi = 0, .vco_fine = 0,
            .analog_drift = 480, .vco_keytrack = true,
            .noise_level = 80, .sh_rate = 1023
        },
        .env1 = {
            .onset_delay = 0, .attack = 45, .decay = 580, .sustain = 780,
            .release = 480, .vel_sens = 520, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .in_lpf_cutoff = 720,
        .in_lpf_res    = 320,
        .bbd = {
            .feedback = 620, .fb_lpf_cutoff = 760, .glide = 60,
            .invert = false, .invert_pitch_comp = false, .bbd_noise = 220
        },
        .dry_wet    = 512, /* 50/50 Dry VCO + Pitch-Modulated Wet BBD = true BBD Chorus! */
        .env2 = {
            .onset_delay = 0, .attack = 35, .decay = 620, .sustain = 880,
            .release = 520, .vel_sens = 400, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .master_vol = 860,
        .lfo_wave = ADRIFT_LFO_TRI, .lfo_rate = 350, .lfo_depth = 44,
        .lfo_dest = ADRIFT_MOD_BBD_PITCH, .mw_depth = 240, .pb_range = 2, .legato = false
    },
    /* 30: Modal Kick / Bass Tom (ECHON "5 drumkits" Low Drum Shell) */
    {
        .name = "MODAL KICK",
        .exc = {
            .wave_morph = 180, .pulse_width = 512, .vco_level = 960,
            .vco_octave = -1, .vco_semi = 0, .vco_fine = 0,
            .analog_drift = 360, .vco_keytrack = true,
            .noise_level = 380, .sh_rate = 640, .pluck = 860
        },
        .env1 = {
            .onset_delay = 0, .attack = 0, .decay = 165, .sustain = 0,
            .release = 150, .vel_sens = 700, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .in_lpf_cutoff = 520,
        .in_lpf_res    = 760,
        .bbd = {
            .feedback = 825, .fb_lpf_cutoff = 540, .glide = 340,
            .invert = false, .invert_pitch_comp = false, .bbd_noise = 220, .dispersion = 340
        },
        .dry_wet    = 720,
        .env2 = {
            .onset_delay = 0, .attack = 0, .decay = 440, .sustain = 200,
            .release = 360, .vel_sens = 480, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .master_vol = 900,
        .lfo_wave = ADRIFT_LFO_SAW, .lfo_rate = 520, .lfo_depth = -220,
        .lfo_dest = ADRIFT_MOD_VCO_PITCH, .mw_depth = 260,
        .pb_range = 12, .legato = false
    },
    /* 31: Modal Snare / Metallic Percussion (ECHON "5 drumkits" S&H Snare/Hat) */
    {
        .name = "MODAL SNARE",
        .exc = {
            .wave_morph = 920, .pulse_width = 240, .vco_level = 950,
            .vco_octave = 0, .vco_semi = 6, .vco_fine = 0,
            .analog_drift = 420, .vco_keytrack = false, /* Fixed drum membrane pitch */
            .noise_level = 650, .sh_rate = 620, .pluck = 740
        },
        .env1 = {
            .onset_delay = 0, .attack = 0, .decay = 195, .sustain = 0,
            .release = 180, .vel_sens = 750, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .in_lpf_cutoff = 880,
        .in_lpf_res    = 480,
        .bbd = {
            .feedback = 845, .fb_lpf_cutoff = 840, .glide = 0,
            .invert = true, .invert_pitch_comp = true, .bbd_noise = 280, .dispersion = 760
        },
        .dry_wet    = 820,
        .env2 = {
            .onset_delay = 0, .attack = 0, .decay = 480, .sustain = 240,
            .release = 380, .vel_sens = 500, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
        },
        .master_vol = 860,
        .lfo_wave = ADRIFT_LFO_SH, .lfo_rate = 820, .lfo_depth = 300,
        .lfo_dest = ADRIFT_MOD_IN_LPF, .mw_depth = 300,
        .pb_range = 12, .legato = false
    }
};

/* Compact per-group configuration descriptor for Multi-Group Factory Presets */
typedef struct {
    uint8_t num_voices;    /* 0..16 (sum across groups <= 16) */
    uint8_t assign_mode;   /* ADRIFT_GROUP_ROUND_ROBIN, UNISON, or SUPER */
    uint8_t midi_channel;  /* 1..16 */
    int16_t unison_detune; /* 0..1023 -> 0..50 cents (UNISON) / 0..80 cents (SUPER) */
    int8_t  transpose;     /* -24..+24 semitone offset for both BBD & VCO */
    uint8_t init_note;     /* Initial MIDI note (60=C4 default, 48=C3/55=G3 for drones) */
    uint8_t patch_idx;     /* Index into k_factory_patches[0..31] */
} adrift_preset_group_cfg_t;

typedef struct {
    char                      name[16];
    adrift_preset_group_cfg_t groups[ADRIFT_MAX_GROUPS];
    adrift_fx_params_t        fx;
} adrift_preset_desc_t;

#define RR  ADRIFT_GROUP_ROUND_ROBIN
#define UNI ADRIFT_GROUP_UNISON
#define SUP ADRIFT_GROUP_SUPER

static const adrift_preset_desc_t k_factory_presets[ADRIFT_NUM_PRESETS] = {
    /* P1: Acoustic Guitar Pluck (Default Boot: G1=3, G2..G6=0 with 13 free voices, ECHON "Guitar") */
    {
        .name = "ACOUST GUIT",
        .groups = {
            { 3, RR,  1,   0,   0, 60,  0 }, /* G1: 3v Karplus-Strong BBD Guitar (Ch 1) */
            { 0, RR,  2,   0,   0, 60,  1 }, /* G2: off (Ch 2) */
            { 0, RR,  3,   0,   0, 60,  2 }, /* G3: off (Ch 3) */
            { 0, RR,  4,   0,   0, 60,  3 }, /* G4: off (Ch 4) */
            { 0, RR,  5,   0,   0, 60,  4 }, /* G5: off (Ch 5) */
            { 0, RR,  6,   0,   0, 60,  5 }  /* G6: off (Ch 6) */
        }
    },
    /* P2: Bowed Cello Duo — 8v Bowed BBD Cello (G1) + 6v +1 Oct Harmonic Bloom (G2) on Ch 1 */
    {
        .name = "BOWED CELLO",
        .groups = {
            { 8, RR,  1,   0,   0, 60,  1 }, /* G1: 8v Bowed Delay Cello (Ch 1) */
            { 6, RR,  1,   0, +12, 60,  2 }, /* G2: 6v +1 Oct Hollow Harmonic Swell (Ch 1) */
            { 0, RR,  3,   0,   0, 60,  0 },
            { 0, RR,  4,   0,   0, 60,  3 },
            { 0, RR,  5,   0,   0, 60,  4 },
            { 0, RR,  6,   0,   0, 60,  5 }
        }
    },
    /* P3: Strummer — 4-Group Cascading Onset-Delay Acoustic Chord Strummer on Ch 1 (ECHON "Strummer") */
    {
        .name = "STRUMMER",
        .groups = {
            { 4, RR,  1,   0,   0, 60,  0 }, /* G1: 4v 0ms Root String (0 ST, Ch 1) */
            { 4, RR,  1,   0,  +7, 67,  8 }, /* G2: 4v ~120ms Fifth String (+7 ST, Ch 1) */
            { 4, RR,  1,   0, +12, 72,  9 }, /* G3: 4v ~235ms Octave String (+12 ST, Ch 1) */
            { 4, RR,  1,   0, +19, 79, 10 }, /* G4: 4v ~355ms 12th String (+19 ST, Ch 1) */
            { 0, RR,  5,   0,   0, 60,  4 },
            { 0, RR,  6,   0,   0, 60,  5 }
        }
    },
    /* P4: Auto Feedback — 8v Pure BBD Self-Oscillation (G1) + 4v Unison Sub-Octave (G2) (ECHON "Auto feedback") */
    {
        .name = "AUTO FEEDBK",
        .groups = {
            { 8, RR,  1,   0,   0, 60,  3 }, /* G1: 8v Pure BBD Self-Oscillation (Ch 1) */
            { 4, UNI, 1, 450, -12, 48,  3 }, /* G2: 4v Unison (22c) -1 Oct Self-Osc Sub (Ch 1) */
            { 0, RR,  3,   0,   0, 60,  0 },
            { 0, RR,  4,   0,   0, 60,  1 },
            { 0, RR,  5,   0,   0, 60,  2 },
            { 0, RR,  6,   0,   0, 60,  5 }
        }
    },
    /* P5: Slowpad — 8v Lush PWM/BBD Pad (G1) + 8v Shimmering BBD Resonator Pad (G2) (ECHON "Slowpad") */
    {
        .name = "SLOWPAD",
        .groups = {
            { 8, RR,  1,   0,   0, 60, 11 }, /* G1: 8v Warm PWM + BBD Pad (Ch 1) */
            { 8, RR,  1,   0,   0, 60, 12 }, /* G2: 8v Detuned Shimmer BBD Pad (Ch 1) */
            { 0, RR,  3,   0,   0, 60,  0 },
            { 0, RR,  4,   0,   0, 60,  1 },
            { 0, RR,  5,   0,   0, 60,  2 },
            { 0, RR,  6,   0,   0, 60,  5 }
        },
        .fx = { .chorus_mix = 360, .delay_time = 520, .delay_fb = 480, .delay_mix = 220,
                .reverb_size = 780, .reverb_damp = 360, .reverb_shimmer = 420, .reverb_mix = 380 }
    },
    /* P6: Melancholy Bath — 8v Tape-Warbled Felted Keys (G1) + 6v Delayed Ghost Halo (G2) (ECHON "Melancholy bath") */
    {
        .name = "MELANCHOLY",
        .groups = {
            { 8, RR,  1,   0,   0, 60, 13 }, /* G1: 8v Lo-Fi Drift Felted BBD Keys (Ch 1) */
            { 6, RR,  1,   0, +12, 72, 14 }, /* G2: 6v +1 Oct Delayed Inverted Halo (Ch 1) */
            { 0, RR,  3,   0,   0, 60,  0 },
            { 0, RR,  4,   0,   0, 60,  1 },
            { 0, RR,  5,   0,   0, 60,  2 },
            { 0, RR,  6,   0,   0, 60,  3 }
        },
        .fx = { .chorus_mix = 240, .delay_time = 640, .delay_fb = 560, .delay_mix = 300,
                .reverb_size = 820, .reverb_damp = 540, .reverb_shimmer = 280, .reverb_mix = 440 }
    },
    /* P7: 7 12 Glitter — 6v Harp Pluck (G1) + 5v +7ST Loop Chime (G2) + 5v +12ST Loop Glitter (G3) (ECHON "7 12 glitter") */
    {
        .name = "7 12 GLITTER",
        .groups = {
            { 6, RR,  1,   0,   0, 60,  0 }, /* G1: 6v Fundamental Harp Pluck (0 ST, Ch 1) */
            { 5, RR,  1,   0,  +7, 67, 15 }, /* G2: 5v Looping D-A-D Fifth Glitter (+7 ST, Ch 1) */
            { 5, RR,  1,   0, +12, 72,  4 }, /* G3: 5v Looping A-D Octave Shimmer (+12 ST, Ch 1) */
            { 0, RR,  4,   0,   0, 60,  1 },
            { 0, RR,  5,   0,   0, 60,  2 },
            { 0, RR,  6,   0,   0, 60,  5 }
        },
        .fx = { .chorus_mix = 280, .delay_time = 460, .delay_fb = 520, .delay_mix = 320,
                .reverb_size = 840, .reverb_damp = 260, .reverb_shimmer = 640, .reverb_mix = 420 }
    },
    /* P8: Mistery Pluck — 8v S&H Inverted Modal Pluck (G1) + 6v Sub-Octave Hollow Tube (G2) (ECHON "Mistery pluck") */
    {
        .name = "MISTERY PLK",
        .groups = {
            { 8, RR,  1,   0,   0, 60,  5 }, /* G1: 8v Stepped S&H Inverted Modal Bell (Ch 1) */
            { 6, RR,  1,   0,   0, 60, 16 }, /* G2: 6v Non-Comp Inverted Sub-Octave Tube (Ch 1) */
            { 0, RR,  3,   0,   0, 60,  0 },
            { 0, RR,  4,   0,   0, 60,  1 },
            { 0, RR,  5,   0,   0, 60,  2 },
            { 0, RR,  6,   0,   0, 60,  3 }
        }
    },
    /* P9: Random Pluck — 8v S&H Wave-Morph Pluck (G1) + 8v S&H Feedback-Filter Pluck (G2) (ECHON "Random pluck") */
    {
        .name = "RANDOM PLK",
        .groups = {
            { 8, RR,  1,   0,   0, 60, 17 }, /* G1: 8v Per-Voice S&H -> VCO Morph Pluck (Ch 1) */
            { 8, RR,  1,   0,   0, 60, 18 }, /* G2: 8v Per-Voice S&H -> FB LPF Inverted Pluck (Ch 1) */
            { 0, RR,  3,   0,   0, 60,  0 },
            { 0, RR,  4,   0,   0, 60,  1 },
            { 0, RR,  5,   0,   0, 60,  2 },
            { 0, RR,  6,   0,   0, 60,  5 }
        }
    },
    /* P10: 7th Overdub — 6v Tine Keys (G1) + 5v Delayed +7ST 5th (G2) + 5v Delayed +10ST Min7th (G3) (ECHON "7th overdub") */
    {
        .name = "7TH OVERDUB",
        .groups = {
            { 6, RR,  1,   0,   0, 60, 19 }, /* G1: 6v Immediate Warm BBD Tine Keys (0 ST, Ch 1) */
            { 5, RR,  1,   0,  +7, 67, 20 }, /* G2: 5v Delayed +7 ST Fifth Overdub Swell (Ch 1) */
            { 5, RR,  1,   0, +10, 70, 20 }, /* G3: 5v Delayed +10 ST Minor-7th Overdub Swell (Ch 1) */
            { 0, RR,  4,   0,   0, 60,  3 },
            { 0, RR,  5,   0,   0, 60,  4 },
            { 0, RR,  6,   0,   0, 60,  5 }
        }
    },
    /* P11: Granular — 4-Group Asynchronous Loop A-D / D-A-D Micro-Grain Cloud on Ch 1 (ECHON "Granular-like") */
    {
        .name = "GRANULAR",
        .groups = {
            { 4, RR,  1,   0,   0, 60, 21 }, /* G1: 4v Fast Loop A-D Grain Stream (0 ST, Ch 1) */
            { 4, RR,  1,   0, +12, 72, 22 }, /* G2: 4v Staggered Loop D-A-D +1 Oct Grains (+12 ST, Ch 1) */
            { 4, RR,  1,   0,  +7, 67, 15 }, /* G3: 4v Staggered Loop D-A-D +7 ST Grains (+7 ST, Ch 1) */
            { 4, RR,  1,   0,   0, 60, 12 }, /* G4: 4v Warm Shimmer Foundation Pad (0 ST, Ch 1) */
            { 0, RR,  5,   0,   0, 60,  4 },
            { 0, RR,  6,   0,   0, 60,  5 }
        },
        .fx = { .chorus_mix = 260, .delay_time = 580, .delay_fb = 540, .delay_mix = 340,
                .reverb_size = 860, .reverb_damp = 300, .reverb_shimmer = 560, .reverb_mix = 460 }
    },
    /* P12: Into The Void — 6v Self-Osc Iron Groan (G1) + 6v Metallic Pipe (G2) + 4v Abyssal Sub (G3) (ECHON "Into the void") */
    {
        .name = "INTO VOID",
        .groups = {
            { 6, RR,  1,   0,   0, 60, 23 }, /* G1: 6v Over-Unity Groaning Iron Tube (Ch 1) */
            { 6, RR,  1,   0,   0, 60, 16 }, /* G2: 6v Sub-Octave Hollow Metal Resonator (Ch 1) */
            { 4, UNI, 1, 580, -12, 48,  6 }, /* G3: 4v Unison (29c) -1 Oct Abyssal Sub Bass (Ch 1) */
            { 0, RR,  4,   0,   0, 60,  1 },
            { 0, RR,  5,   0,   0, 60,  2 },
            { 0, RR,  6,   0,   0, 60,  5 }
        },
        .fx = { .chorus_mix = 180, .delay_time = 760, .delay_fb = 620, .delay_mix = 300,
                .reverb_size = 900, .reverb_damp = 620, .reverb_shimmer = 320, .reverb_mix = 480 }
    },
    /* P13: Pulsing — 8v Loop A-D Resonant Gate (G1) + 6v Syncopated +1 Oct Loop D-A-D Counter-Pulse (G2) (ECHON "Pulsing") */
    {
        .name = "PULSING",
        .groups = {
            { 8, RR,  1,   0,   0, 60, 24 }, /* G1: 8v Rhythmic Loop A-D Resonant Pulse (0 ST, Ch 1) */
            { 6, RR,  1,   0, +12, 72, 22 }, /* G2: 6v Syncopated Loop D-A-D Octave Pulse (+12 ST, Ch 1) */
            { 0, RR,  3,   0,   0, 60,  0 },
            { 0, RR,  4,   0,   0, 60,  1 },
            { 0, RR,  5,   0,   0, 60,  2 },
            { 0, RR,  6,   0,   0, 60,  5 }
        }
    },
    /* P14: Hollow Flute — 8v Inverted Cylindrical Pipe (G1) + 4v Detuned Unison Reed Chiff (G2) on Ch 1 */
    {
        .name = "HOLLOW FLUTE",
        .groups = {
            { 8, RR,  1,   0,   0, 60,  2 }, /* G1: 8v Inverted Hollow Woodwind Pipe (Ch 1) */
            { 4, UNI, 1, 370,   0, 60,  0 }, /* G2: 4v Unison (18c) Plucked Reed Chiff Transient (Ch 1) */
            { 0, RR,  3,   0,   0, 60,  3 },
            { 0, RR,  4,   0,   0, 60,  4 },
            { 0, RR,  5,   0,   0, 60,  5 },
            { 0, RR,  6,   0,   0, 60,  6 }
        }
    },
    /* P15: Sitar Jawari — 8v Buzzing Sitar Bridge (G1) + 4v +7ST Sympathetic (G2) + 4v +12ST Chikari (G3) */
    {
        .name = "SITAR JAWARI",
        .groups = {
            { 8, RR,  1,   0,   0, 60, 25 }, /* G1: 8v Dispersive Buzzing Sitar Bridge (0 ST, Ch 1) */
            { 4, RR,  1,   0,  +7, 67,  8 }, /* G2: 4v Delayed Sympathetic Fifth String (+7 ST, Ch 1) */
            { 4, RR,  1,   0, +12, 72,  9 }, /* G3: 4v Delayed Chikari Octave String (+12 ST, Ch 1) */
            { 0, RR,  4,   0,   0, 60,  1 },
            { 0, RR,  5,   0,   0, 60,  2 },
            { 0, RR,  6,   0,   0, 60,  5 }
        }
    },
    /* P16: Marimba Wood — 8v Rosewood Marimba Bar (G1) + 6v +12ST Kalimba Tine Overtone (G2) on Ch 1 */
    {
        .name = "MARIMBA WOOD",
        .groups = {
            { 8, RR,  1,   0,   0, 60, 26 }, /* G1: 8v Hollow Inverted Rosewood Marimba (0 ST, Ch 1) */
            { 6, RR,  1,   0, +12, 72,  0 }, /* G2: 6v Bright Kalimba Overtone Tine (+12 ST, Ch 1) */
            { 0, RR,  3,   0,   0, 60,  1 },
            { 0, RR,  4,   0,   0, 60,  2 },
            { 0, RR,  5,   0,   0, 60,  3 },
            { 0, RR,  6,   0,   0, 60,  5 }
        }
    },
    /* P17: Glass Bowls — 8v Singing Tibetan Bowl (G1) + 8v +12ST Crystalline Rim Shimmer (G2) on Ch 1 */
    {
        .name = "GLASS BOWLS",
        .groups = {
            { 8, RR,  1,   0,   0, 60, 27 }, /* G1: 8v Singing Tibetan Glass Bowl (0 ST, Ch 1) */
            { 8, RR,  1,   0, +12, 72, 15 }, /* G2: 8v +1 Oct Crystalline Shimmer Halo (+12 ST, Ch 1) */
            { 0, RR,  3,   0,   0, 60,  0 },
            { 0, RR,  4,   0,   0, 60,  1 },
            { 0, RR,  5,   0,   0, 60,  2 },
            { 0, RR,  6,   0,   0, 60,  5 }
        },
        .fx = { .chorus_mix = 220, .delay_time = 620, .delay_fb = 500, .delay_mix = 260,
                .reverb_size = 880, .reverb_damp = 220, .reverb_shimmer = 680, .reverb_mix = 450 }
    },
    /* P18: Clavinet Funk — 8v Narrow-Pulse D6 Clavinet (G1) + 4v +12ST Bridge Snap (G2) on Ch 1 */
    {
        .name = "CLAVINET FN",
        .groups = {
            { 8, RR,  1,   0,   0, 60, 28 }, /* G1: 8v Funky Narrow-Pulse D6 Clavinet (0 ST, Ch 1) */
            { 4, RR,  1,   0, +12, 72, 28 }, /* G2: 4v +1 Oct Upper-Bridge Key Click (+12 ST, Ch 1) */
            { 0, RR,  3,   0,   0, 60,  0 },
            { 0, RR,  4,   0,   0, 60,  1 },
            { 0, RR,  5,   0,   0, 60,  2 },
            { 0, RR,  6,   0,   0, 60,  5 }
        }
    },
    /* P19: Super Bass — 8v SUPER Detuned VCOs into 1 Shared BBD (G1) + 4v Unison Pluck Punch (G2) on Ch 1 */
    {
        .name = "SUPER BASS",
        .groups = {
            { 8, SUP, 1, 620,   0, 48,  6 }, /* G1: 8v SUPER (50c) 8 Detuned VCOs -> 1 BBD Bass (Ch 1) */
            { 4, UNI, 1, 240,   0, 60,  0 }, /* G2: 4v Unison Plucked String Transient (Ch 1) */
            { 0, RR,  3,   0,   0, 60,  1 },
            { 0, RR,  4,   0,   0, 60,  2 },
            { 0, RR,  5,   0,   0, 60,  3 },
            { 0, RR,  6,   0,   0, 60,  5 }
        }
    },
    /* P20: 16V Monster — All 16 Hardware Voices Stacked in Unison (37c Detune) on Ch 1 */
    {
        .name = "16V MONSTER",
        .groups = {
            { 16, UNI, 1, 760,  0, 60,  6 }, /* G1: 16v Unison (37c) Stacked Micro-ACB + BBD Lead (Ch 1) */
            { 0,  RR,  2,   0,  0, 60,  0 },
            { 0,  RR,  3,   0,  0, 60,  1 },
            { 0,  RR,  4,   0,  0, 60,  2 },
            { 0,  RR,  5,   0,  0, 60,  3 },
            { 0,  RR,  6,   0,  0, 60,  5 }
        }
    },
    /* P21: BBD Chorus — 8v Polysynth + BBD Ensemble Chorus (G1) + 8v PWM BBD Flanger (G2) on Ch 1 */
    {
        .name = "BBD CHORUS",
        .groups = {
            { 8, RR,  1,   0,   0, 60, 29 }, /* G1: 8v 50/50 Dry/Wet BBD Chorus Polysynth (Ch 1) */
            { 8, RR,  1,   0,   0, 60, 11 }, /* G2: 8v Warm PWM Companion Layer (Ch 1) */
            { 0, RR,  3,   0,   0, 60,  0 },
            { 0, RR,  4,   0,   0, 60,  1 },
            { 0, RR,  5,   0,   0, 60,  2 },
            { 0, RR,  6,   0,   0, 60,  5 }
        },
        .fx = { .chorus_mix = 680, .delay_time = 400, .delay_fb = 420, .delay_mix = 180,
                .reverb_size = 660, .reverb_damp = 380, .reverb_shimmer = 200, .reverb_mix = 260 }
    },
    /* P22: Pipe Organ — 16v 4-Rank Cathedral BBD Organ on Ch 1: 16' Sub (-12) + 8' (0) + Quint (+7) + 4' (+12) */
    {
        .name = "PIPE ORGAN",
        .groups = {
            { 4, RR,  1,   0,   0, 60,  2 }, /* G1: 4v 8' Principal Hollow Pipe (0 ST, Ch 1) */
            { 4, RR,  1,   0,   0, 60, 16 }, /* G2: 4v 16' Sub-Octave Bourdon Pipe (-12 ST via Invert, Ch 1) */
            { 4, RR,  1,   0,  +7, 67,  3 }, /* G3: 4v 5-1/3' Nazard Fifth Pipe (+7 ST, Ch 1) */
            { 4, RR,  1,   0, +12, 72,  2 }, /* G4: 4v 4' Octave Clarion Pipe (+12 ST, Ch 1) */
            { 0, RR,  5,   0,   0, 60,  4 },
            { 0, RR,  6,   0,   0, 60,  5 }
        },
        .fx = { .chorus_mix = 160, .delay_time = 520, .delay_fb = 360, .delay_mix = 140,
                .reverb_size = 860, .reverb_damp = 420, .reverb_shimmer = 240, .reverb_mix = 420 }
    },
    /* P23: 5 Drumkits — Modal Percussion Kit on Ch 1 (Kick+Snare) & Multi-Timbral Drums on Ch 2..4 (ECHON "5 drumkits") */
    {
        .name = "5 DRUMKITS",
        .groups = {
            { 4, RR,  1,   0,   0, 48, 30 }, /* G1: 4v Modal Kick / Bass Tom (Ch 1) */
            { 4, RR,  1,   0,   0, 60, 31 }, /* G2: 4v S&H Metallic Snare / Rim (Ch 1) */
            { 3, RR,  2,   0, -12, 36, 30 }, /* G3: 3v Sub-Kick Drum (Ch 2) */
            { 3, RR,  3,   0,   0, 60, 31 }, /* G4: 3v Crispy Snare / Clap (Ch 3) */
            { 2, RR,  4,   0, +12, 72,  5 }, /* G5: 2v S&H Metallic Hi-Hat / Cymbal (Ch 4) */
            { 0, RR,  6,   0,   0, 60, 26 }
        }
    },
    /* P24: Drone + Lead — 8v Lead (G1 Ch1) over Hands-Free C3 Drone (G2 Ch2) + G3 Drone (G3 Ch3) + Pluck (G4 Ch4) */
    {
        .name = "DRONE + LEAD",
        .groups = {
            { 8, RR,  1,   0,   0, 60,  1 }, /* G1: 8v Playable Bowed BBD Lead (Ch 1) */
            { 4, UNI, 2, 490,   0, 48,  7 }, /* G2: 4v Unison (24c) Continuous C3 Root Drone (Ch 2) */
            { 2, UNI, 3, 320,   0, 55,  7 }, /* G3: 2v Unison Continuous G3 Fifth Drone (Ch 3) */
            { 2, RR,  4,   0,   0, 60,  0 }, /* G4: 2v Polyphonic Pluck Companion (Ch 4) */
            { 0, RR,  5,   0,   0, 60,  2 },
            { 0, RR,  6,   0,   0, 60,  5 }
        },
        .fx = { .chorus_mix = 240, .delay_time = 600, .delay_fb = 520, .delay_mix = 280,
                .reverb_size = 800, .reverb_damp = 400, .reverb_shimmer = 380, .reverb_mix = 360 }
    }
};

#undef RR
#undef UNI
#undef SUP

/* Clean dry sawtooth init patch template for INIT SYNTH and INIT GROUP */
static const adrift_patch_t k_init_patch = {
    .name = "INIT PATCH",
    .exc = {
        .wave_morph = 682, .pulse_width = 512, .vco_level = 900,
        .vco_octave = 0, .vco_semi = 0, .vco_fine = 0,
        .analog_drift = 0, .vco_keytrack = true,
        .noise_level = 0, .sh_rate = 1023, .pluck = 0
    },
    .env1 = {
        .onset_delay = 0, .attack = 0, .decay = 512, .sustain = 1023,
        .release = 220, .vel_sens = 512, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
    },
    .in_lpf_cutoff = 1023,
    .in_lpf_res    = 0,
    .bbd = {
        .feedback = 0, .fb_lpf_cutoff = 800, .glide = 0,
        .invert = false, .invert_pitch_comp = false, .bbd_noise = 0, .dispersion = 0
    },
    .dry_wet    = 0,
    .env2 = {
        .onset_delay = 0, .attack = 8, .decay = 512, .sustain = 1023,
        .release = 220, .vel_sens = 512, .loop_mode = ADRIFT_ENV_LOOP_OFF, .drone_hold = false
    },
    .master_vol = 880,
    .lfo_wave  = ADRIFT_LFO_SINE, .lfo_rate  = 340, .lfo_depth  = 0, .lfo_dest  = ADRIFT_MOD_VCO_PITCH,
    .lfo2_wave = ADRIFT_LFO_TRI,  .lfo2_rate = 260, .lfo2_depth = 0, .lfo2_dest = ADRIFT_MOD_VCO_MORPH,
    .lfo3_wave = ADRIFT_LFO_SINE, .lfo3_rate = 160, .lfo3_depth = 0, .lfo3_dest = ADRIFT_MOD_IN_LPF,
    .mw_depth = 200,
    .pb_range = 2, .legato = false,
    .arp = {
        .mode = ADRIFT_ARP_OFF, .range = 1, .rate_bpm = 120, .gate = 80,
        .velocity = 0, .latch = false, .swing = 0, .ratchet = ADRIFT_RATCHET_1X
    }
};

const adrift_patch_t *adrift_synth_get_factory_patch(uint8_t index) {
    return &k_factory_patches[index % ADRIFT_NUM_PATCH_TEMPLATES];
}

const char *adrift_synth_get_preset_name(uint8_t index) {
    if (index >= ADRIFT_USER_PRESET_BASE &&
        index < (uint8_t)(ADRIFT_USER_PRESET_BASE + ADRIFT_NUM_USER_PRESETS)) {
        return adrift_synth_get_user_preset_name((uint8_t)(index - ADRIFT_USER_PRESET_BASE));
    }
    return k_factory_presets[index % ADRIFT_NUM_PRESETS].name;
}

void adrift_synth_get_preset_summary(uint8_t index, uint8_t *out_total_voices, uint8_t *out_active_groups) {
    if (index >= ADRIFT_USER_PRESET_BASE &&
        index < (uint8_t)(ADRIFT_USER_PRESET_BASE + ADRIFT_NUM_USER_PRESETS)) {
        adrift_synth_get_user_preset_summary((uint8_t)(index - ADRIFT_USER_PRESET_BASE),
                                             out_total_voices,
                                             out_active_groups);
        return;
    }
    const adrift_preset_desc_t *desc = &k_factory_presets[index % ADRIFT_NUM_PRESETS];
    uint8_t total_v = 0u;
    uint8_t active_g = 0u;
    for (uint8_t g = 0; g < ADRIFT_MAX_GROUPS; g++) {
        uint8_t nv = desc->groups[g].num_voices;
        if (nv > 0u) {
            total_v = (uint8_t)(total_v + nv);
            active_g++;
        }
    }
    if (out_total_voices) *out_total_voices = total_v;
    if (out_active_groups) *out_active_groups = active_g;
}

uint8_t adrift_synth_get_preset_group_voices(uint8_t preset_id, uint8_t g) {
    uint8_t gi = (uint8_t)(g % ADRIFT_MAX_GROUPS);
    if (preset_id < ADRIFT_NUM_PRESETS) {
        return k_factory_presets[preset_id].groups[gi].num_voices;
    }
    if (preset_id >= ADRIFT_USER_PRESET_BASE &&
        preset_id < (uint8_t)(ADRIFT_USER_PRESET_BASE + ADRIFT_NUM_USER_PRESETS)) {
        const adrift_synth_snapshot_t *snap =
            adrift_synth_get_user_preset_snapshot((uint8_t)(preset_id - ADRIFT_USER_PRESET_BASE));
        return snap ? snap->groups[gi].num_voices : 0u;
    }
    return 0u;
}

static void normalize_factory_patch_defaults(adrift_patch_t *p) {
    if (p->arp.range == 0u) {
        p->arp.mode     = ADRIFT_ARP_OFF;
        p->arp.range    = 1u;
        p->arp.rate_bpm = 120u;
        p->arp.gate     = 80u;
        p->arp.velocity = 0u;
        p->arp.latch    = false;
        p->arp.swing    = 0u;
        p->arp.ratchet  = ADRIFT_RATCHET_1X;
    }
    if (p->lfo2_rate == 0 && p->lfo2_depth == 0) {
        p->lfo2_wave = ADRIFT_LFO_TRI;
        p->lfo2_rate = 260;
        p->lfo2_dest = ADRIFT_MOD_VCO_MORPH;
    }
    if (p->lfo3_rate == 0 && p->lfo3_depth == 0) {
        p->lfo3_wave = ADRIFT_LFO_SINE;
        p->lfo3_rate = 160;
        p->lfo3_dest = ADRIFT_MOD_IN_LPF;
    }
}

bool adrift_synth_load_preset_group(adrift_synth_t *s,
                                    uint8_t preset_id,
                                    uint8_t src_group,
                                    uint8_t dst_group) {
    if (!s || dst_group >= ADRIFT_MAX_GROUPS) {
        return false;
    }
    uint8_t sg = (uint8_t)(src_group % ADRIFT_MAX_GROUPS);

    adrift_patch_t src_patch;
    uint8_t        src_nv     = 0u;
    uint8_t        src_mode   = ADRIFT_GROUP_ROUND_ROBIN;
    int16_t        src_detune = 0;
    int16_t        src_spread = 0;
    int8_t         src_tr     = 0;
    uint8_t        src_note   = 60u;

    if (preset_id < ADRIFT_NUM_PRESETS) {
        const adrift_preset_group_cfg_t *cfg = &k_factory_presets[preset_id].groups[sg];
        src_patch  = k_factory_patches[cfg->patch_idx % ADRIFT_NUM_PATCH_TEMPLATES];
        normalize_factory_patch_defaults(&src_patch);
        src_nv     = cfg->num_voices;
        src_mode   = cfg->assign_mode;
        src_detune = cfg->unison_detune;
        src_spread = adrift_group_is_stacked(cfg->assign_mode) ? 820 : 0;
        src_tr     = cfg->transpose;
        src_note   = cfg->init_note ? cfg->init_note : 60u;
    } else if (preset_id >= ADRIFT_USER_PRESET_BASE &&
               preset_id < (uint8_t)(ADRIFT_USER_PRESET_BASE + ADRIFT_NUM_USER_PRESETS)) {
        const adrift_synth_snapshot_t *snap =
            adrift_synth_get_user_preset_snapshot((uint8_t)(preset_id - ADRIFT_USER_PRESET_BASE));
        if (!snap) {
            return false;
        }
        const adrift_group_snapshot_t *gs = &snap->groups[sg];
        src_patch  = gs->patch;
        src_nv     = gs->num_voices;
        src_mode   = gs->assign_mode;
        src_detune = gs->unison_detune;
        src_spread = gs->stereo_spread;
        src_tr     = gs->transpose;
        src_note   = gs->last_note ? gs->last_note : 60u;
    } else {
        return false;
    }

    adrift_synth_group_all_notes_off(s, dst_group, true);

    adrift_group_t *gp = &s->groups[dst_group];
    gp->assign_mode    = src_mode;
    gp->unison_detune  = src_detune;
    gp->stereo_spread  = src_spread;
    gp->transpose      = src_tr;
    gp->patch          = src_patch;
    gp->pitch_bend_q13 = 0;
    gp->mod_wheel      = 0u;
    gp->aftertouch     = 0u;
    gp->sustain_pedal  = false;
    gp->rr_cursor      = 0u;
    gp->mono_len       = 0u;
    gp->last_note      = src_note;
    adrift_arp_reset_state(&gp->arp_state, dst_group);

    /* If destination group was disabled (0 voices) and free voices exist, claim voices so it plays right away */
    if (gp->num_voices == 0u) {
        uint8_t free_v = adrift_synth_free_voices(s);
        if (free_v > 0u) {
            uint8_t want = (src_nv > 0u) ? src_nv : 3u;
            if (want > free_v) want = free_v;
            (void)adrift_synth_set_group_voices(s, dst_group, want);
        }
    }

    int32_t grp_g = (int32_t)dsp_udiv_1023((uint32_t)dsp_clamp_i32(gp->patch.master_vol, 0, 1023) * 32767u);
    for (uint8_t v = 0u; v < ADRIFT_MAX_VOICES; v++) {
        if (s->voices[v].owner_group == (int8_t)dst_group) {
            adrift_synth_reset_voice_dsp(&s->voices[v]);
            s->voices[v].note           = src_note;
            s->voices[v].grp_gain_l_q15 = grp_g;
            s->voices[v].grp_gain_r_q15 = grp_g;
        }
    }
    adrift_synth_recompute_group_slots(s);
    adrift_synth_wake_group_drone(s, dst_group);
    adrift_synth_refresh_any_gate(s);
    return true;
}

void adrift_synth_load_preset(adrift_synth_t *s, uint8_t index) {
    if (index >= ADRIFT_USER_PRESET_BASE &&
        index < (uint8_t)(ADRIFT_USER_PRESET_BASE + ADRIFT_NUM_USER_PRESETS)) {
        uint8_t u_slot = (uint8_t)(index - ADRIFT_USER_PRESET_BASE);
        const adrift_synth_snapshot_t *snap = adrift_synth_get_user_preset_snapshot(u_slot);
        if (!snap) {
            return;
        }
        adrift_synth_restore_snapshot(s, snap);
        s->preset_index = index;
        return;
    }

    uint8_t idx = index % ADRIFT_NUM_PRESETS;
    s->preset_index = idx;
    s->edit_group   = 0u;

    const adrift_preset_desc_t *desc = &k_factory_presets[idx];
    for (uint32_t i = 0; i < 16u; i++) {
        s->preset_name[i] = desc->name[i];
    }

    /* Reset all hardware voices to unassigned pool before applying preset's group layout */
    for (uint8_t v = 0; v < ADRIFT_MAX_VOICES; v++) {
        adrift_synth_reset_voice_dsp(&s->voices[v]);
        s->voices[v].owner_group = -1;
        s->voices[v].group_slot  = 0u;
    }

    /* Initialize all Voice Groups from the preset's group descriptors */
    for (uint8_t g = 0; g < ADRIFT_MAX_GROUPS; g++) {
        const adrift_preset_group_cfg_t *cfg = &desc->groups[g];
        adrift_group_t *gp = &s->groups[g];
        gp->num_voices     = 0u;
        gp->assign_mode    = cfg->assign_mode;
        gp->midi_channel   = cfg->midi_channel;
        gp->unison_detune  = cfg->unison_detune;
        gp->stereo_spread  = adrift_group_is_stacked(cfg->assign_mode) ? 820 : 0;
        gp->pan            = 0;
        gp->transpose      = cfg->transpose;
        gp->patch          = k_factory_patches[cfg->patch_idx % ADRIFT_NUM_PATCH_TEMPLATES];
        normalize_factory_patch_defaults(&gp->patch);
        gp->pitch_bend_q13 = 0;
        gp->mod_wheel      = 0u;
        gp->aftertouch     = 0u;
        gp->sustain_pedal  = false;
        gp->rr_cursor      = 0u;
        gp->mono_len       = 0u;
        gp->last_note      = cfg->init_note ? cfg->init_note : 60u;
        adrift_arp_reset_state(&gp->arp_state, g);
    }

    /* Allocate voices from the voice pool to each enabled group in the preset */
    for (uint8_t g = 0; g < ADRIFT_MAX_GROUPS; g++) {
        uint8_t want = desc->groups[g].num_voices;
        if (want > 0u) {
            (void)adrift_synth_set_group_voices(s, g, want);
        }
    }

    /* Load preset Master Stereo FX parameters (with musical standby defaults when dry) */
    s->fx_params = desc->fx;
    if (s->fx_params.delay_time == 0 && s->fx_params.reverb_size == 0) {
        s->fx_params.delay_time     = 380; /* ~146 ms */
        s->fx_params.delay_fb       = 460; /* ~45% */
        s->fx_params.reverb_size    = 620; /* ~61% */
        s->fx_params.reverb_damp    = 400; /* ~39% */
        s->fx_params.reverb_shimmer = 0;   /* Pure reverb by default until user turns up SHIMMR */
    }
    if (s->fx_params.comp_mix == 0 && s->fx_params.comp_thresh == 0 &&
        s->fx_params.comp_attack == 0 && s->fx_params.comp_makeup == 0) {
        s->fx_params.comp_thresh = 512; /* -13 dB standby glue threshold */
        s->fx_params.comp_attack = 480; /* ~12 ms punchy transient-pass attack */
        s->fx_params.comp_makeup = 256; /* +3 dB gentle makeup ready when COMP > 0% */
    }
    adrift_fx_reset(&s->fx);
}

void adrift_synth_init_group_patch(adrift_synth_t *s, uint8_t g) {
    if (g >= ADRIFT_MAX_GROUPS) return;
    adrift_synth_group_all_notes_off(s, g, true);

    adrift_group_t *gp = &s->groups[g];
    gp->assign_mode    = ADRIFT_GROUP_ROUND_ROBIN;
    gp->unison_detune  = 0;
    gp->stereo_spread  = 0;
    gp->pan            = 0;
    gp->transpose      = 0;
    gp->patch          = k_init_patch;
    gp->pitch_bend_q13 = 0;
    gp->mod_wheel      = 0u;
    gp->aftertouch     = 0u;
    gp->sustain_pedal  = false;
    gp->rr_cursor      = 0u;
    gp->mono_len       = 0u;
    gp->last_note      = 60u;
    adrift_arp_reset_state(&gp->arp_state, g);

    int32_t init_grp_g = ((int32_t)dsp_clamp_i32(gp->patch.master_vol, 0, 1023) * 32767) / 1023;
    for (uint8_t v = 0; v < ADRIFT_MAX_VOICES; v++) {
        if (s->voices[v].owner_group == (int8_t)g) {
            adrift_synth_reset_voice_dsp(&s->voices[v]);
            s->voices[v].note           = 60u;
            s->voices[v].grp_gain_l_q15 = init_grp_g;
            s->voices[v].grp_gain_r_q15 = init_grp_g;
        }
    }
    adrift_synth_recompute_group_slots(s);
    adrift_synth_refresh_any_gate(s);
}

void adrift_synth_init_patch(adrift_synth_t *s) {
    s->preset_index = ADRIFT_PRESET_INIT;
    s->edit_group   = 0u;
    for (uint32_t i = 0; i < 16u; i++) {
        s->preset_name[i] = k_init_patch.name[i];
    }

    for (uint8_t v = 0; v < ADRIFT_MAX_VOICES; v++) {
        adrift_synth_reset_voice_dsp(&s->voices[v]);
        s->voices[v].owner_group = -1;
        s->voices[v].group_slot  = 0u;
    }

    for (uint8_t g = 0; g < ADRIFT_MAX_GROUPS; g++) {
        adrift_group_t *gp = &s->groups[g];
        gp->num_voices     = 0u;
        gp->assign_mode    = ADRIFT_GROUP_ROUND_ROBIN;
        gp->midi_channel   = (uint8_t)(g + 1u);
        gp->unison_detune  = 0;
        gp->stereo_spread  = 0;
        gp->pan            = 0;
        gp->transpose      = 0;
        gp->patch          = k_init_patch;
        gp->pitch_bend_q13 = 0;
        gp->mod_wheel      = 0u;
        gp->aftertouch     = 0u;
        gp->sustain_pedal  = false;
        gp->rr_cursor      = 0u;
        gp->mono_len       = 0u;
        gp->last_note      = 60u;
        adrift_arp_reset_state(&gp->arp_state, g);
    }

    (void)adrift_synth_set_group_voices(s, 0u, 3u);

    s->fx_params.chorus_mix     = 0;
    s->fx_params.delay_time     = 380;
    s->fx_params.delay_fb       = 460;
    s->fx_params.delay_mix      = 0;
    s->fx_params.reverb_size    = 620;
    s->fx_params.reverb_damp    = 400;
    s->fx_params.reverb_shimmer = 0;
    s->fx_params.reverb_mix     = 0;
    s->fx_params.comp_mix       = 0;
    s->fx_params.comp_thresh    = 512;
    s->fx_params.comp_attack    = 480;
    s->fx_params.comp_makeup    = 256;
    adrift_fx_reset(&s->fx);
}

void adrift_synth_save_snapshot(const adrift_synth_t *s, adrift_synth_snapshot_t *snap) {
    for (uint32_t i = 0; i < 16u; i++) {
        snap->preset_name[i] = s->preset_name[i];
    }
    snap->preset_index = s->preset_index;
    snap->edit_group   = s->edit_group;
    for (uint8_t g = 0; g < ADRIFT_MAX_GROUPS; g++) {
        const adrift_group_t *gp = &s->groups[g];
        snap->groups[g].num_voices    = gp->num_voices;
        snap->groups[g].assign_mode   = gp->assign_mode;
        snap->groups[g].midi_channel  = gp->midi_channel;
        snap->groups[g].unison_detune = gp->unison_detune;
        snap->groups[g].stereo_spread = gp->stereo_spread;
        snap->groups[g].pan           = gp->pan;
        snap->groups[g].transpose     = gp->transpose;
        snap->groups[g].last_note     = gp->last_note;
        snap->groups[g].patch         = gp->patch;
    }
    snap->fx_params = s->fx_params;
}

void adrift_synth_restore_snapshot(adrift_synth_t *s, const adrift_synth_snapshot_t *snap) {
    adrift_synth_all_notes_off(s, true);
    for (uint32_t i = 0; i < 16u; i++) {
        s->preset_name[i] = snap->preset_name[i];
    }
    s->preset_index = snap->preset_index;
    s->edit_group   = (uint8_t)(snap->edit_group % ADRIFT_MAX_GROUPS);

    for (uint8_t v = 0; v < ADRIFT_MAX_VOICES; v++) {
        adrift_synth_reset_voice_dsp(&s->voices[v]);
        s->voices[v].owner_group = -1;
        s->voices[v].group_slot  = 0u;
    }

    for (uint8_t g = 0; g < ADRIFT_MAX_GROUPS; g++) {
        const adrift_group_snapshot_t *gs = &snap->groups[g];
        adrift_group_t *gp = &s->groups[g];
        gp->num_voices     = 0u;
        gp->assign_mode    = gs->assign_mode;
        gp->midi_channel   = gs->midi_channel;
        gp->unison_detune  = gs->unison_detune;
        gp->stereo_spread  = gs->stereo_spread;
        gp->pan            = gs->pan;
        gp->transpose      = gs->transpose;
        gp->patch          = gs->patch;
        gp->pitch_bend_q13 = 0;
        gp->mod_wheel      = 0u;
        gp->aftertouch     = 0u;
        gp->sustain_pedal  = false;
        gp->rr_cursor      = 0u;
        gp->mono_len       = 0u;
        gp->last_note      = gs->last_note ? gs->last_note : 60u;
        adrift_arp_reset_state(&gp->arp_state, g);
    }

    for (uint8_t g = 0; g < ADRIFT_MAX_GROUPS; g++) {
        uint8_t want = snap->groups[g].num_voices;
        if (want > 0u) {
            (void)adrift_synth_set_group_voices(s, g, want);
        }
    }

    s->fx_params = snap->fx_params;
    adrift_fx_reset(&s->fx);
}

/* ---- Keyboard Musical Scales (ALGORITHM Encoder + KNOB1 Root) ---- */
typedef struct {
    char    name[12];
    uint8_t num_notes;
    uint8_t intervals[12];
} adrift_scale_desc_t;

static const adrift_scale_desc_t k_scales[ADRIFT_NUM_SCALES] = {
    { "CHROMATIC",  12u, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 } },
    { "MAJOR",       7u, { 0, 2, 4, 5, 7, 9, 11, 0, 0, 0, 0, 0 } },
    { "NAT MINOR",   7u, { 0, 2, 3, 5, 7, 8, 10, 0, 0, 0, 0, 0 } },
    { "MIN PENTA",   5u, { 0, 3, 5, 7, 10, 0, 0, 0, 0, 0, 0, 0 } },
    { "MAJ PENTA",   5u, { 0, 2, 4, 7, 9, 0, 0, 0, 0, 0, 0, 0 } },
    { "BLUES",       6u, { 0, 3, 5, 6, 7, 10, 0, 0, 0, 0, 0, 0 } },
    { "DORIAN",      7u, { 0, 2, 3, 5, 7, 9, 10, 0, 0, 0, 0, 0 } },
    { "PHRYGIAN",    7u, { 0, 1, 3, 5, 7, 8, 10, 0, 0, 0, 0, 0 } },
    { "LYDIAN",      7u, { 0, 2, 4, 6, 7, 9, 11, 0, 0, 0, 0, 0 } },
    { "MIXOLYDIAN",  7u, { 0, 2, 4, 5, 7, 9, 10, 0, 0, 0, 0, 0 } },
    { "LOCRIAN",     7u, { 0, 1, 3, 5, 6, 8, 10, 0, 0, 0, 0, 0 } },
    { "HARM MINOR",  7u, { 0, 2, 3, 5, 7, 8, 11, 0, 0, 0, 0, 0 } },
    { "MEL MINOR",   7u, { 0, 2, 3, 5, 7, 9, 11, 0, 0, 0, 0, 0 } },
    { "PHRYG DOM",   7u, { 0, 1, 4, 5, 7, 8, 10, 0, 0, 0, 0, 0 } },
    { "HIRAJOSHI",   5u, { 0, 2, 3, 7, 8, 0, 0, 0, 0, 0, 0, 0 } },
    { "WHOLE TONE",  6u, { 0, 2, 4, 6, 8, 10, 0, 0, 0, 0, 0, 0 } }
};

static const char *const k_root_names[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

static inline int32_t floor_div_pos(int32_t a, int32_t b, int32_t *out_rem) {
    int32_t q = a / b;
    int32_t r = a % b;
    if (r < 0) {
        r += b;
        q -= 1;
    }
    *out_rem = r;
    return q;
}

const char *adrift_synth_get_scale_name(uint8_t scale_idx) {
    return k_scales[scale_idx % ADRIFT_NUM_SCALES].name;
}

uint8_t adrift_synth_get_scale_num_notes(uint8_t scale_idx) {
    return k_scales[scale_idx % ADRIFT_NUM_SCALES].num_notes;
}

const char *adrift_synth_get_root_name(uint8_t root_idx) {
    return k_root_names[root_idx % 12u];
}

int32_t adrift_synth_map_kbd_key(const adrift_synth_t *s, int32_t semi_from_c4) {
    uint8_t sc_idx    = s->kbd_scale % ADRIFT_NUM_SCALES;
    int32_t oct_shift = (int32_t)s->kbd_octave * 12;

    if (sc_idx == ADRIFT_SCALE_CHROMATIC) {
        return dsp_clamp_i32(60 + semi_from_c4 + oct_shift, 0, 127);
    }

    /* Map 12-TET pitch class to white-key index (0..6 for C..B, -1 for black keys) */
    static const int8_t k_pc_to_white[12] = {
        0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6
    };
    int32_t pc      = 0;
    int32_t chr_oct = floor_div_pos(semi_from_c4, 12, &pc);
    int8_t  w_in_oct = k_pc_to_white[pc];
    if (w_in_oct < 0) {
        return -1; /* Black keys are muted in non-chromatic scales */
    }

    int32_t white_step = chr_oct * 7 + (int32_t)w_in_oct;
    const adrift_scale_desc_t *sc = &k_scales[sc_idx];
    int32_t deg       = 0;
    int32_t scale_oct = floor_div_pos(white_step, (int32_t)sc->num_notes, &deg);
    int32_t root_semi = (int32_t)(s->kbd_scale_root % 12u);
    int32_t note      = 60 + root_semi + scale_oct * 12 + (int32_t)sc->intervals[deg] + oct_shift;
    return dsp_clamp_i32(note, 0, 127);
}
