#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "platform.h"

/* Deterministic WAV export & offline rendering helpers for automated DSP/MIDI tests */
bool adrift_headless_write_wav(const char *path,
                               const int16_t *mono_or_stereo,
                               uint32_t frames,
                               uint16_t channels,
                               uint32_t sample_rate);

void adrift_headless_render(int16_t *stereo_out,
                            uint32_t total_frames,
                            const adrift_midi_msg_t *events,
                            uint32_t num_events);

/* Simulated 25 x 1 KiB NOR flash test hooks (0xFF erased state) */
void     adrift_headless_storage_reset(void);
uint8_t *adrift_headless_storage_raw_slot(uint8_t slot);
void     adrift_headless_storage_abort_on_target_write(bool enable);
