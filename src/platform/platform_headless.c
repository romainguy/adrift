/* SPDX-License-Identifier: MPL-2.0
 * Headless deterministic renderer & WAV writer for Adrift DSP/MIDI tests */

#include "platform_headless.h"
#include <stdio.h>
#include <string.h>

static void wr_u16_le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void wr_u32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

bool adrift_headless_write_wav(const char *path,
                               const int16_t *samples,
                               uint32_t frames,
                               uint16_t channels,
                               uint32_t sample_rate) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;

    uint32_t data_bytes = frames * (uint32_t)channels * 2u;
    uint8_t  hdr[44];
    memcpy(&hdr[0], "RIFF", 4);
    wr_u32_le(&hdr[4], 36u + data_bytes);
    memcpy(&hdr[8], "WAVEfmt ", 8);
    wr_u32_le(&hdr[16], 16u);
    wr_u16_le(&hdr[20], 1u); /* PCM */
    wr_u16_le(&hdr[22], channels);
    wr_u32_le(&hdr[24], sample_rate);
    wr_u32_le(&hdr[28], sample_rate * (uint32_t)channels * 2u);
    wr_u16_le(&hdr[32], (uint16_t)(channels * 2u));
    wr_u16_le(&hdr[34], 16u);
    memcpy(&hdr[36], "data", 4);
    wr_u32_le(&hdr[40], data_bytes);

    bool ok = (fwrite(hdr, 1, sizeof(hdr), fp) == sizeof(hdr));
    if (ok && data_bytes > 0u) {
        ok = (fwrite(samples, 1, data_bytes, fp) == data_bytes);
    }
    fclose(fp);
    return ok;
}

void adrift_headless_render(int16_t *stereo_out,
                            uint32_t total_frames,
                            const adrift_midi_msg_t *events,
                            uint32_t num_events) {
    uint32_t done = 0u;
    uint32_t ev_i = 0u;

    while (done < total_frames) {
        uint32_t blk = total_frames - done;
        if (blk > ADRIFT_BLOCK_SIZE) blk = ADRIFT_BLOCK_SIZE;

        /* Deliver events scheduled on the first block when sample_offset is used, or at block 0 */
        const adrift_midi_msg_t *blk_evs = NULL;
        uint32_t blk_nev = 0u;
        if (done == 0u && num_events > 0u) {
            blk_evs = events;
            blk_nev = num_events;
        }
        (void)ev_i;
        adrift_app_audio_block(stereo_out + 2u * done, blk, blk_evs, blk_nev);
        done += blk;
    }
}

bool adrift_platform_run_worker(void (*fn)(uint32_t arg), uint32_t arg) {
    (void)fn;
    (void)arg;
    return false;
}

bool adrift_platform_wait_worker(void) {
    return false;
}

static uint8_t g_headless_flash[ADRIFT_STORAGE_TOTAL_SLOTS][ADRIFT_STORAGE_SLOT_BYTES];
static bool    g_headless_flash_inited = false;
static bool    g_headless_abort_target = false;

static void ensure_headless_flash(void) {
    if (!g_headless_flash_inited) {
        memset(g_headless_flash, 0xFF, sizeof(g_headless_flash));
        g_headless_flash_inited = true;
    }
}

void adrift_headless_storage_reset(void) {
    memset(g_headless_flash, 0xFF, sizeof(g_headless_flash));
    g_headless_flash_inited = true;
    g_headless_abort_target = false;
}

uint8_t *adrift_headless_storage_raw_slot(uint8_t slot) {
    ensure_headless_flash();
    if (slot >= ADRIFT_STORAGE_TOTAL_SLOTS) return NULL;
    return g_headless_flash[slot];
}

void adrift_headless_storage_abort_on_target_write(bool enable) {
    g_headless_abort_target = enable;
}

bool adrift_platform_storage_available(void) {
    ensure_headless_flash();
    return true;
}

bool adrift_platform_storage_read(uint8_t slot, uint8_t *dst, uint32_t len) {
    ensure_headless_flash();
    if (!dst || slot >= ADRIFT_STORAGE_TOTAL_SLOTS || len > ADRIFT_STORAGE_SLOT_BYTES) {
        return false;
    }
    memcpy(dst, g_headless_flash[slot], len);
    return true;
}

bool adrift_platform_storage_write(uint8_t slot, const uint8_t *src, uint32_t len) {
    ensure_headless_flash();
    if (!src || slot >= ADRIFT_STORAGE_TOTAL_SLOTS || len > ADRIFT_STORAGE_SLOT_BYTES) {
        return false;
    }
    if (g_headless_abort_target && slot < ADRIFT_STORAGE_USER_SLOTS) {
        /* Simulate power cut mid-sector-erase on the target user slot after backup slot was written */
        memset(g_headless_flash[slot], 0xFF, ADRIFT_STORAGE_SLOT_BYTES);
        g_headless_flash[slot][0] = 0x41u; /* Partial corrupt byte left behind */
        g_headless_abort_target = false;
        return false;
    }
    memset(g_headless_flash[slot], 0xFF, ADRIFT_STORAGE_SLOT_BYTES);
    memcpy(g_headless_flash[slot], src, len);
    return true;
}

bool adrift_platform_storage_erase(uint8_t slot) {
    ensure_headless_flash();
    if (slot >= ADRIFT_STORAGE_TOTAL_SLOTS) {
        return false;
    }
    memset(g_headless_flash[slot], 0xFF, ADRIFT_STORAGE_SLOT_BYTES);
    return true;
}
