/* SPDX-License-Identifier: MPL-2.0
 * Automated DSP, Sonic Invariant & MIDI Verification Suite Entry Point for Adrift */

#include "test_common.h"

int g_failures = 0;

int main(int argc, char **argv) {
    const char *wav_dir = (argc > 1) ? argv[1] : NULL;
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== Adrift DSP & MIDI Automated Verification Suite ===\n");

    test_micro_acb_exciter();
    test_env1_delay_and_loop();
    test_dry_path_post_input_lpf();
    test_bbd_pitch_and_invert();
    test_self_oscillation_and_env2_gate();
    test_midi_protocol();
    test_presets_and_ui(wav_dir);
    test_voice_groups_and_save_menu(wav_dir);
    test_arpeggiator();
    test_master_fx();
    test_midi_clock_sync();
    test_physical_modeling_dispersion_and_pluck();
    test_env1_modulation();
    test_multi_lfos();
    test_env1_range();
    test_extended_mod_targets();
    test_user_presets_and_save_ui();
    test_boot_animation();

    if (g_failures > 0) {
        fprintf(stderr, "=== FAILED (%d check(s) failed) ===\n", g_failures);
        return 1;
    }
    printf("=== ALL TESTS PASSED (0 failures) ===\n");
    return 0;
}
