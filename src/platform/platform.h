#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Physical sample rate of AC791N ALNK0 I2S: 24 MHz / 544 = 44,117.647 Hz */
#ifndef ADRIFT_SAMPLE_RATE
#define ADRIFT_SAMPLE_RATE      44118u
#define ADRIFT_SAMPLE_RATE_Q8   11294118u /* round(44117.6470588 * 256) */
#endif
#define ADRIFT_BLOCK_SIZE       64u
#define ADRIFT_SCREEN_W         240u
#define ADRIFT_SCREEN_H         240u
#define ADRIFT_NUM_PARAM_KNOBS  4u

/* Logical encoder indices in adrift_input_state_t::enc_delta */
enum {
    ADRIFT_ENC_KNOB1     = 0, /* Parameter slot 0 (physical KNOB1) */
    ADRIFT_ENC_KNOB2     = 1, /* Parameter slot 1 (physical KNOB2) */
    ADRIFT_ENC_KNOB3     = 2, /* Parameter slot 2 (physical KNOB3) */
    ADRIFT_ENC_KNOB4     = 3, /* Parameter slot 3 (physical KNOB4) */
    ADRIFT_ENC_SELECT    = 4, /* Voice Group selector G1..G6 (physical SELECT) */
    ADRIFT_ENC_PRESETS   = 5, /* Factory Preset selector P1..P24 (physical PRESETS) */
    ADRIFT_ENC_ALGORITHM = 6, /* Keyboard Scale selector (physical ALGORITHM) */
    ADRIFT_NUM_ENC       = 7
};

/* Logical UI buttons normalized across FM-1 hardware and SDL2 simulator */
enum {
    ADRIFT_BTN_PAGE_1     = (1u << 0), /* SEL  -> EXC  (press again for page 2/2) */
    ADRIFT_BTN_PAGE_2     = (1u << 1), /* ENV  -> ENV1 (press again for page 2/2) */
    ADRIFT_BTN_PAGE_3     = (1u << 2), /* LFO  -> BBD  (press again for page 2/2) */
    ADRIFT_BTN_PAGE_4     = (1u << 3), /* EDIT -> ENV2 (press again for page 2/2) */
    ADRIFT_BTN_PAGE_5     = (1u << 4), /* GLO  -> MOD  (press again for page 2/2) */
    ADRIFT_BTN_PREV_PST   = (1u << 5),
    ADRIFT_BTN_NEXT_PST   = (1u << 6),
    ADRIFT_BTN_OCT_DOWN   = (1u << 7),
    ADRIFT_BTN_OCT_UP     = (1u << 8),
    ADRIFT_BTN_PANIC      = (1u << 9), /* PLAY -> Panic / All Notes Off */
    ADRIFT_BTN_HOLD       = (1u << 10),
    ADRIFT_BTN_NEXT_VOICE = (1u << 11),
    ADRIFT_BTN_NEXT_GROUP = ADRIFT_BTN_NEXT_VOICE,
    ADRIFT_BTN_GROUP_CFG  = (1u << 12), /* SEQ  -> GRP  (press again for page 2/2) */
    ADRIFT_BTN_HOME       = (1u << 13), /* HOME -> Oscilloscope screen */
    ADRIFT_BTN_ARP        = (1u << 14), /* ARP  -> ARP  (press again for page 2/2) */
    ADRIFT_BTN_FX         = (1u << 15), /* FX   -> FX   (press again for page 2/2) */
    ADRIFT_BTN_SAVE_PRESET = (1u << 16) /* SAVE -> Save Preset modal */
};

/* Optional host keyboard text editing commands (used by SDL2 simulator in Save Preset modal) */
enum {
    ADRIFT_TEXT_CMD_NONE      = 0,
    ADRIFT_TEXT_CMD_BACKSPACE = 1,
    ADRIFT_TEXT_CMD_DELETE    = 2,
    ADRIFT_TEXT_CMD_LEFT      = 3,
    ADRIFT_TEXT_CMD_RIGHT     = 4,
    ADRIFT_TEXT_CMD_CLEAR     = 5
};

typedef struct {
    uint8_t  status;
    uint8_t  data1;
    uint8_t  data2;
    uint16_t sample_offset; /* 0 .. frames - 1 within current render callback */
} adrift_midi_msg_t;

typedef struct {
    int8_t   enc_delta[ADRIFT_NUM_ENC];
    uint32_t buttons_down;
    uint32_t buttons_pressed;
    uint32_t buttons_released;
    int16_t  master_pot;   /* -1 if unchanged/unused, else 0..1023 */
    uint8_t  cpu_load_pct; /* 0..100 reported by hardware/sim */
    char     text_char;    /* Printable ASCII 0x20..0x5F (or a..z) when typing in simulator, else 0 */
    uint8_t  text_cmd;     /* ADRIFT_TEXT_CMD_* */
} adrift_input_state_t;

/* Dirty-row bitmask for 240 rows (8 * 32 = 256 bits) so SPI TFT only sends changed bands */
typedef struct {
    uint32_t row_mask[8];
    bool     full_redraw;
} adrift_dirty_t;

static inline void adrift_dirty_clear(adrift_dirty_t *d) {
    for (int i = 0; i < 8; i++) d->row_mask[i] = 0u;
    d->full_redraw = false;
}

static inline void adrift_dirty_mark_rows(adrift_dirty_t *d, uint32_t y0, uint32_t h) {
    uint32_t y1 = y0 + h;
    if (y1 > ADRIFT_SCREEN_H) y1 = ADRIFT_SCREEN_H;
    for (uint32_t y = y0; y < y1; y++) {
        d->row_mask[y >> 5] |= (1u << (y & 31u));
    }
}

/* Application entry points invoked by each platform backend */
void adrift_app_init(void);
void adrift_app_audio_block(int16_t *stereo_out_interleaved,
                            uint32_t frames,
                            const adrift_midi_msg_t *midi_events,
                            uint32_t num_midi);
void adrift_app_audio_block_q23(int32_t *stereo_q23_out_interleaved,
                                uint32_t frames,
                                const adrift_midi_msg_t *midi_events,
                                uint32_t num_midi);
void adrift_app_tick_ui(const adrift_input_state_t *input,
                        uint16_t *rgb565_fb,
                        adrift_dirty_t *dirty);

/* Dual-core parallel DSP worker hooks (AC791N CPU1 on FM-1 hardware, single-core fallback on host) */
bool adrift_platform_run_worker(void (*fn)(uint32_t arg), uint32_t arg);
bool adrift_platform_wait_worker(void);

/* Persistent User Preset storage HAL (24 user slots + 1 backup sector, 1 KiB record per 4 KiB sector) */
#define ADRIFT_STORAGE_SLOT_BYTES  1024u
#define ADRIFT_STORAGE_USER_SLOTS  24u
#define ADRIFT_STORAGE_BACKUP_SLOT 24u
#define ADRIFT_STORAGE_TOTAL_SLOTS 25u

bool adrift_platform_storage_available(void);
bool adrift_platform_storage_read(uint8_t slot, uint8_t *dst, uint32_t len);
bool adrift_platform_storage_write(uint8_t slot, const uint8_t *src, uint32_t len);
bool adrift_platform_storage_erase(uint8_t slot);
