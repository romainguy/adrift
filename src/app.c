#include "app.h"

static adrift_synth_t         g_synth;
static adrift_ui_t            g_ui;
static adrift_screen_reader_t g_sr;

adrift_synth_t *adrift_app_get_synth(void) {
    return &g_synth;
}

adrift_ui_t *adrift_app_get_ui(void) {
    return &g_ui;
}

adrift_screen_reader_t *adrift_app_get_screen_reader(void) {
    return &g_sr;
}

void adrift_app_init(void) {
    adrift_preset_store_init();
    adrift_synth_init(&g_synth);
    adrift_ui_init(&g_ui);
    adrift_sr_init(&g_sr);
}

void adrift_app_audio_block(int16_t *stereo_out_interleaved,
                            uint32_t frames,
                            const adrift_midi_msg_t *midi_events,
                            uint32_t num_midi) {
    adrift_synth_render_block(&g_synth, stereo_out_interleaved, frames, midi_events, num_midi);
}

void adrift_app_mix_line_out(int16_t *stereo_inout, uint32_t frames) {
    adrift_sr_mix_line_out(&g_sr, stereo_inout, frames);
}

void adrift_app_tick_ui(const adrift_input_state_t *input,
                        uint16_t *rgb565_fb,
                        adrift_dirty_t *dirty) {
    adrift_ui_step(&g_ui, &g_synth, input, rgb565_fb, dirty);
    adrift_sr_tick(&g_sr, &g_ui, &g_synth, 16u);
}
