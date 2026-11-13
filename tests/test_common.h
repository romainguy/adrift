/* SPDX-License-Identifier: MPL-2.0
 * Shared Test Harness, Assertions & DSP Measurement Helpers for Adrift */

#ifndef ADRIFT_TEST_COMMON_H
#define ADRIFT_TEST_COMMON_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/app.h"
#include "../src/platform/platform.h"
#include "../src/platform/platform_headless.h"

#define FS_EXACT (24000000.0 / 544.0)

extern int g_failures;

#define CHECK(cond, fmt, ...)                                                       \
    do {                                                                            \
        if (!(cond)) {                                                              \
            fprintf(stderr, "  [FAIL] %s:%d: " fmt "\n", __FILE__, __LINE__,        \
                    ##__VA_ARGS__);                                                 \
            g_failures++;                                                           \
        }                                                                           \
    } while (0)

/* DSP measurement & spectral analysis helpers (implemented in test_dsp.c) */
double compute_rms_dbfs(const int16_t *stereo, uint32_t start_f, uint32_t count_f);
double compute_mean_lsb(const int16_t *stereo, uint32_t start_f, uint32_t count_f);
double estimate_freq_autocorr(const int16_t *stereo,
                              uint32_t start_f,
                              uint32_t window_f,
                              double min_hz,
                              double max_hz);
double goertzel_mag_db(const int16_t *stereo, uint32_t start_f, uint32_t n, double freq_hz);
double estimate_fundamental_dft(const int16_t *stereo, uint32_t start_f, uint32_t n, double target_hz);
double calc_hf_diff_ratio_db(const int16_t *stereo_pcm, uint32_t start_frame, uint32_t count);
void   render_frames(int16_t *buf, uint32_t frames);

/* Test suite entry points */
void test_micro_acb_exciter(void);
void test_env1_delay_and_loop(void);
void test_dry_path_post_input_lpf(void);
void test_bbd_pitch_and_invert(void);
void test_self_oscillation_and_env2_gate(void);
void test_midi_protocol(void);
void test_presets_and_ui(const char *wav_dir);
void test_voice_groups_and_save_menu(const char *wav_dir);
void test_arpeggiator(void);
void test_master_fx(void);
void test_midi_clock_sync(void);
void test_physical_modeling_dispersion_and_pluck(void);
void test_env1_modulation(void);
void test_multi_lfos(void);
void test_env1_range(void);
void test_extended_mod_targets(void);
void test_user_presets_and_save_ui(void);

#endif /* ADRIFT_TEST_COMMON_H */
