/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "speech/klatt.h"

/* Maximum length of any single screen-reader utterance (Rule 11: <= 240 chars) */
#define ADRIFT_TTS_MAX_TEXT 240u

/* Phoneme IDs (bits 0..5 of packed phoneme byte; bits 6..7 hold stress 0..2) */
enum {
    ADRIFT_PH_SIL = 0,
    /* Monophthong vowels */
    ADRIFT_PH_IY,
    ADRIFT_PH_IH,
    ADRIFT_PH_EH,
    ADRIFT_PH_AE,
    ADRIFT_PH_AA,
    ADRIFT_PH_AO,
    ADRIFT_PH_UH,
    ADRIFT_PH_UW,
    ADRIFT_PH_AH,
    ADRIFT_PH_AX,
    ADRIFT_PH_ER,
    /* Diphthongs */
    ADRIFT_PH_EY,
    ADRIFT_PH_AY,
    ADRIFT_PH_OY,
    ADRIFT_PH_AW,
    ADRIFT_PH_OW,
    /* Glides & liquids */
    ADRIFT_PH_W,
    ADRIFT_PH_Y,
    ADRIFT_PH_L,
    ADRIFT_PH_R,
    /* Nasals */
    ADRIFT_PH_M,
    ADRIFT_PH_N,
    ADRIFT_PH_NG,
    /* Fricatives & aspirate */
    ADRIFT_PH_HH,
    ADRIFT_PH_F,
    ADRIFT_PH_V,
    ADRIFT_PH_TH,
    ADRIFT_PH_DH,
    ADRIFT_PH_S,
    ADRIFT_PH_Z,
    ADRIFT_PH_SH,
    ADRIFT_PH_ZH,
    /* Stops & affricates */
    ADRIFT_PH_P,
    ADRIFT_PH_T,
    ADRIFT_PH_K,
    ADRIFT_PH_B,
    ADRIFT_PH_D,
    ADRIFT_PH_G,
    ADRIFT_PH_CH,
    ADRIFT_PH_JH,
    ADRIFT_PH_COUNT
};

#define ADRIFT_PH_PACK(id, stress) ((uint8_t)(((uint8_t)(id) & 0x3Fu) | (((uint8_t)(stress) & 0x03u) << 6)))
#define ADRIFT_PH_ID(packed)       ((uint8_t)((packed) & 0x3Fu))
#define ADRIFT_PH_STRESS(packed)   ((uint8_t)(((packed) >> 6) & 0x03u))

typedef struct {
    char     text[ADRIFT_TTS_MAX_TEXT + 4u];
    uint16_t len;
    uint16_t cursor;
    uint16_t word_idx;
    uint16_t total_words;
    uint16_t prev_f1;
    uint16_t prev_f2;
    uint16_t prev_f3;
    bool     active;
    bool     is_question;
} adrift_tts_t;

void     adrift_tts_init(adrift_tts_t *tts);
void     adrift_tts_stop(adrift_tts_t *tts);

/* Expand raw on-screen label/patch/preset name via words.tsv & built-in rules (Rules 1-7). */
size_t   adrift_tts_expand_label(const char *raw, char *out, size_t out_cap);

/* Clean punctuation, symbols, repeated whitespace, and 240-char cap (Rules 4, 10, 11). */
size_t   adrift_tts_clean_say(const char *raw, char *out, size_t out_cap);

/* Start streaming synthesis of an utterance into `klatt` frame FIFO. */
void     adrift_tts_start(adrift_tts_t *tts, adrift_klatt_t *klatt, const char *text);

/* Refill `klatt` frame FIFO with the next word(s); returns true while words remain. */
bool     adrift_tts_pump(adrift_tts_t *tts, adrift_klatt_t *klatt);

/* Convert a phrase into packed phoneme bytes (exposed for unit tests). */
uint32_t adrift_tts_phonemize_phrase(const char *phrase, uint8_t *out_ph, uint32_t max_ph);
