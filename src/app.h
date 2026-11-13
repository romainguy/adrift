#pragma once

#include "platform/platform.h"
#include "synth/synth.h"
#include "ui/ui.h"

/* Direct pointer to global synth state for deterministic test inspection & platform helpers */
adrift_synth_t *adrift_app_get_synth(void);
adrift_ui_t    *adrift_app_get_ui(void);
