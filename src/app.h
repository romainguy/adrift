#pragma once

#include "platform/platform.h"
#include "speech/screen_reader.h"
#include "synth/synth.h"
#include "ui/ui.h"

/* Direct pointer to global synth, UI & screen reader state for platform helpers & tests */
adrift_synth_t         *adrift_app_get_synth(void);
adrift_ui_t            *adrift_app_get_ui(void);
adrift_screen_reader_t *adrift_app_get_screen_reader(void);

/* Mix synthesized screen-reader speech into line-out/headphone buffer ONLY (never USB audio out) */
void adrift_app_mix_line_out(int16_t *stereo_inout, uint32_t frames);
