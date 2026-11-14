/* SPDX-License-Identifier: MPL-2.0 */
#include "speech/tts.h"
#include "speech/tts_tables.h"

/* ---- Freestanding ASCII & String Helpers ---- */

static inline bool is_space_ch(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static inline bool is_digit_ch(char c) {
    return c >= '0' && c <= '9';
}

static inline bool is_upper_ch(char c) {
    return c >= 'A' && c <= 'Z';
}

static inline bool is_lower_ch(char c) {
    return c >= 'a' && c <= 'z';
}

static inline bool is_alpha_ch(char c) {
    return is_upper_ch(c) || is_lower_ch(c);
}

static inline char to_lower_ch(char c) {
    return is_upper_ch(c) ? (char)(c - 'A' + 'a') : c;
}

static inline char to_upper_ch(char c) {
    return is_lower_ch(c) ? (char)(c - 'a' + 'A') : c;
}

static inline bool is_punct_ch(char c) {
    return c == '.' || c == ',' || c == '?' || c == '!' || c == ':' || c == ';';
}

static size_t cstr_len(const char *s) {
    size_t n = 0u;
    if (!s) return 0u;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static int cstr_cmp(const char *a, const char *b) {
    size_t i = 0u;
    while (a[i] != '\0' && a[i] == b[i]) {
        i++;
    }
    return (int)(uint8_t)a[i] - (int)(uint8_t)b[i];
}

static bool cstr_eq_ci(const char *a, size_t a_len, const char *b) {
    size_t i = 0u;
    for (; i < a_len; i++) {
        if (b[i] == '\0' || to_lower_ch(a[i]) != to_lower_ch(b[i])) {
            return false;
        }
    }
    return b[i] == '\0';
}

/* Rule 3: Words after which a preceding 'A' stays the musical note A (EY1) */
static bool is_note_a_follower(const char *w, size_t w_len) {
    static const char *const k_note_followers[] = {
        "minor", "major", "sharp", "flat", "natural", "harmonic", "melodic",
        "pentatonic", "blues", "dorian", "phrygian", "lydian", "mixolydian",
        "locrian", "hirajoshi", "whole", "chromatic"
    };
    for (size_t i = 0u; i < sizeof(k_note_followers) / sizeof(k_note_followers[0]); i++) {
        if (cstr_eq_ci(w, w_len, k_note_followers[i])) {
            return true;
        }
    }
    return false;
}

static void buf_append_ch(char *dst, size_t *pos, size_t cap, char c) {
    if (*pos + 1u < cap) {
        dst[*pos] = c;
        (*pos)++;
        dst[*pos] = '\0';
    }
}

static void buf_append_str(char *dst, size_t *pos, size_t cap, const char *s) {
    if (!s) return;
    while (*s != '\0' && *pos + 1u < cap) {
        dst[*pos] = *s++;
        (*pos)++;
    }
    if (cap > 0u) {
        dst[*pos] = '\0';
    }
}

static void buf_ensure_space(char *dst, size_t *pos, size_t cap) {
    if (*pos > 0u && dst[*pos - 1u] != ' ') {
        buf_append_ch(dst, pos, cap, ' ');
    }
}

/* Match k_words_tsv at `s[idx]` on token boundaries (longest match first) */
static bool match_words_tsv(const char *s, size_t idx, size_t *out_klen, const char **out_val) {
    if (idx > 0u && (is_alpha_ch(s[idx - 1u]) || is_digit_ch(s[idx - 1u]))) {
        return false;
    }
    const size_t count = sizeof(k_words_tsv) / sizeof(k_words_tsv[0]);
    for (size_t i = 0u; i < count; i++) {
        const char *k = k_words_tsv[i].key;
        size_t j = 0u;
        bool ok = true;
        while (k[j] != '\0') {
            if (s[idx + j] == '\0' || to_upper_ch(s[idx + j]) != to_upper_ch(k[j])) {
                ok = false;
                break;
            }
            j++;
        }
        if (ok && j > 0u) {
            char after = s[idx + j];
            if (after == '\0' || is_space_ch(after) || is_punct_ch(after)) {
                *out_klen = j;
                *out_val  = k_words_tsv[i].val;
                return true;
            }
        }
    }
    return false;
}

/* Rule 3b: Musical subdivision tokens ("1/16", "1/8T", "1/8D", "1/8.", etc.) */
static bool match_subdivision(const char *s, size_t idx, size_t *out_len, const char **out_spoken) {
    static const struct {
        const char *pat;
        const char *spoken;
    } k_subs[] = {
        { "1/32T", "thirty second triplet" },
        { "1/32D", "dotted thirty second" },
        { "1/32.", "dotted thirty second" },
        { "1/32",  "thirty second" },
        { "1/16T", "sixteenth triplet" },
        { "1/16D", "dotted sixteenth" },
        { "1/16.", "dotted sixteenth" },
        { "1/16",  "sixteenth" },
        { "1/8T",  "eighth triplet" },
        { "1/8D",  "dotted eighth" },
        { "1/8.",  "dotted eighth" },
        { "1/8",   "eighth" },
        { "1/4T",  "quarter triplet" },
        { "1/4D",  "dotted quarter" },
        { "1/4.",  "dotted quarter" },
        { "1/4",   "quarter" },
        { "1/2D",  "dotted half" },
        { "1/2.",  "dotted half" },
        { "1/2",   "half" }
    };
    if (idx > 0u && (is_alpha_ch(s[idx - 1u]) || is_digit_ch(s[idx - 1u]))) {
        return false;
    }
    for (size_t i = 0u; i < sizeof(k_subs) / sizeof(k_subs[0]); i++) {
        const char *p = k_subs[i].pat;
        size_t j = 0u;
        while (p[j] != '\0' && to_upper_ch(s[idx + j]) == to_upper_ch(p[j])) {
            j++;
        }
        if (p[j] == '\0') {
            char after = s[idx + j];
            if (after == '\0' || is_space_ch(after) || is_punct_ch(after)) {
                *out_len    = j;
                *out_spoken = k_subs[i].spoken;
                return true;
            }
        }
    }
    return false;
}

size_t adrift_tts_clean_say(const char *raw, char *out, size_t out_cap) {
    if (!out || out_cap == 0u) {
        return 0u;
    }
    out[0] = '\0';
    if (!raw) {
        return 0u;
    }

    size_t cap = (out_cap > (ADRIFT_TTS_MAX_TEXT + 1u)) ? (ADRIFT_TTS_MAX_TEXT + 1u) : out_cap;
    size_t pos = 0u;
    size_t i   = 0u;

    while (raw[i] != '\0') {
        char c = raw[i];
        if (is_space_ch(c) || c == '_') {
            if (pos > 0u && out[pos - 1u] != ' ') {
                buf_append_ch(out, &pos, cap, ' ');
            }
            i++;
            continue;
        }

        if (is_punct_ch(c)) {
            /* Rule 10: No space before punctuation, and only one punctuation mark at a time */
            while (pos > 0u && out[pos - 1u] == ' ') {
                pos--;
                out[pos] = '\0';
            }
            if (pos > 0u && is_punct_ch(out[pos - 1u])) {
                /* Keep stronger mark ('?' / '!' > '.' > ':' / ';' > ',') */
                char prev = out[pos - 1u];
                if (c == '?' || c == '!' || (c == '.' && prev == ',')) {
                    out[pos - 1u] = c;
                }
            } else if (pos > 0u) {
                buf_append_ch(out, &pos, cap, c);
            }
            i++;
            continue;
        }

        /* Rule 4: Replace remaining spoken symbols if any slipped into raw text */
        if (c == '+') {
            buf_ensure_space(out, &pos, cap);
            buf_append_str(out, &pos, cap, "plus ");
            i++;
            continue;
        }
        if (c == '&') {
            buf_ensure_space(out, &pos, cap);
            buf_append_str(out, &pos, cap, "and ");
            i++;
            continue;
        }
        if (c == '%') {
            buf_ensure_space(out, &pos, cap);
            buf_append_str(out, &pos, cap, "percent ");
            i++;
            continue;
        }
        if (c == '#') {
            buf_ensure_space(out, &pos, cap);
            buf_append_str(out, &pos, cap, "sharp ");
            i++;
            continue;
        }
        if (c == '/') {
            buf_ensure_space(out, &pos, cap);
            buf_append_str(out, &pos, cap, "or ");
            i++;
            continue;
        }
        if (c == '-') {
            buf_ensure_space(out, &pos, cap);
            if (is_digit_ch(raw[i + 1u])) {
                buf_append_str(out, &pos, cap, "minus ");
            }
            i++;
            continue;
        }

        /* Strip silent noise/eye-only ASCII symbols: | > < * = [ ] { } ~ ^ @ $ */
        if (!is_alpha_ch(c) && !is_digit_ch(c) && c != '\'') {
            if (pos > 0u && out[pos - 1u] != ' ') {
                buf_append_ch(out, &pos, cap, ' ');
            }
            i++;
            continue;
        }

        /* Rule 5: Split glued letter-digit or digit-letter boundaries */
        if (pos > 0u) {
            char prev = out[pos - 1u];
            if ((is_alpha_ch(prev) && is_digit_ch(c)) ||
                (is_digit_ch(prev) && is_alpha_ch(c))) {
                buf_append_ch(out, &pos, cap, ' ');
            }
        }

        buf_append_ch(out, &pos, cap, c);
        i++;
    }

    while (pos > 0u && out[pos - 1u] == ' ') {
        pos--;
        out[pos] = '\0';
    }

    /* Rule 11: If clamped near 240-char limit and raw had more characters, trim to clean word boundary */
    if (raw[i] != '\0' || (pos >= ADRIFT_TTS_MAX_TEXT && raw[cstr_len(raw) > pos ? pos : 0] != '\0')) {
        while (pos > 180u && out[pos - 1u] != ' ' && !is_punct_ch(out[pos - 1u])) {
            pos--;
        }
        while (pos > 0u && out[pos - 1u] == ' ') {
            pos--;
        }
        out[pos] = '\0';
    }

    return pos;
}

size_t adrift_tts_expand_label(const char *raw, char *out, size_t out_cap) {
    if (!out || out_cap == 0u) {
        return 0u;
    }
    out[0] = '\0';
    if (!raw) {
        return 0u;
    }

    /* Step 1: Collapse underscores and leading/trailing spaces into a clean scratch buffer */
    char clean[ADRIFT_TTS_MAX_TEXT + 4u];
    size_t clen = 0u;
    for (size_t i = 0u; raw[i] != '\0' && clen + 1u < sizeof(clean); i++) {
        char c = (raw[i] == '_') ? ' ' : raw[i];
        if (is_space_ch(c)) {
            if (clen > 0u && clean[clen - 1u] != ' ') {
                clean[clen++] = ' ';
            }
        } else {
            clean[clen++] = c;
        }
    }
    while (clen > 0u && clean[clen - 1u] == ' ') {
        clen--;
    }
    clean[clen] = '\0';

    /* Step 2: Expand via words.tsv, musical subdivisions, notes, ratios, multipliers */
    char stage[ADRIFT_TTS_MAX_TEXT + 16u];
    size_t spos = 0u;
    stage[0] = '\0';

    size_t i = 0u;
    while (clean[i] != '\0') {
        if (is_space_ch(clean[i])) {
            buf_ensure_space(stage, &spos, sizeof(stage));
            i++;
            continue;
        }

        size_t      m_len = 0u;
        const char *m_val = NULL;
        if (match_subdivision(clean, i, &m_len, &m_val) ||
            match_words_tsv(clean, i, &m_len, &m_val)) {
            buf_ensure_space(stage, &spos, sizeof(stage));
            buf_append_str(stage, &spos, sizeof(stage), m_val);
            i += m_len;
            continue;
        }

        /* Musical note with sharp, e.g. C#3, F#, A#4 */
        char u0 = to_upper_ch(clean[i]);
        if (u0 >= 'A' && u0 <= 'G' && clean[i + 1u] == '#') {
            buf_ensure_space(stage, &spos, sizeof(stage));
            buf_append_ch(stage, &spos, sizeof(stage), u0);
            buf_append_str(stage, &spos, sizeof(stage), " sharp");
            i += 2u;
            if (clean[i] == '-' && is_digit_ch(clean[i + 1u])) {
                buf_append_str(stage, &spos, sizeof(stage), " minus ");
                i++;
            } else if (is_digit_ch(clean[i])) {
                buf_append_ch(stage, &spos, sizeof(stage), ' ');
            }
            continue;
        }

        /* Multiplier token: 2X, 4X or x2, x4 */
        if (is_digit_ch(clean[i])) {
            size_t j = i;
            while (is_digit_ch(clean[j])) j++;
            if ((clean[j] == 'X' || clean[j] == 'x') &&
                (clean[j + 1u] == '\0' || is_space_ch(clean[j + 1u]) || is_punct_ch(clean[j + 1u]))) {
                buf_ensure_space(stage, &spos, sizeof(stage));
                while (i < j) {
                    buf_append_ch(stage, &spos, sizeof(stage), clean[i++]);
                }
                buf_append_str(stage, &spos, sizeof(stage), " times");
                i++; /* skip X */
                continue;
            }
            /* Ratio or numeric range: 4:1 -> "4 to 1", 1-16 -> "1 to 16" */
            if ((clean[j] == ':' || clean[j] == '-') && is_digit_ch(clean[j + 1u])) {
                buf_ensure_space(stage, &spos, sizeof(stage));
                while (i < j) {
                    buf_append_ch(stage, &spos, sizeof(stage), clean[i++]);
                }
                buf_append_str(stage, &spos, sizeof(stage), " to ");
                i++; /* skip ':' or '-' */
                continue;
            }
        }
        if ((clean[i] == 'x' || clean[i] == 'X') && is_digit_ch(clean[i + 1u]) &&
            (i == 0u || is_space_ch(clean[i - 1u]))) {
            i++;
            size_t j = i;
            while (is_digit_ch(clean[j])) j++;
            buf_ensure_space(stage, &spos, sizeof(stage));
            while (i < j) {
                buf_append_ch(stage, &spos, sizeof(stage), clean[i++]);
            }
            buf_append_str(stage, &spos, sizeof(stage), " times");
            continue;
        }

        /* Word token: Rule 7 lowercase normalization + Rule 3 'A' note vs article */
        if (is_alpha_ch(clean[i])) {
            size_t j = i;
            while (is_alpha_ch(clean[j])) j++;
            size_t wlen = j - i;
            buf_ensure_space(stage, &spos, sizeof(stage));

            if (wlen == 1u) {
                char ch = clean[i];
                if (ch == 'A') {
                    /* Peek next word if any */
                    size_t k = j;
                    while (clean[k] == ' ') k++;
                    if (is_alpha_ch(clean[k])) {
                        size_t k2 = k;
                        while (is_alpha_ch(clean[k2])) k2++;
                        if (is_note_a_follower(&clean[k], k2 - k)) {
                            buf_append_ch(stage, &spos, sizeof(stage), 'A');
                        } else {
                            buf_append_ch(stage, &spos, sizeof(stage), 'a');
                        }
                    } else {
                        /* Standalone 'A' (e.g. "RND PLUCK A", "Root A") stays note/letter A */
                        buf_append_ch(stage, &spos, sizeof(stage), 'A');
                    }
                } else {
                    buf_append_ch(stage, &spos, sizeof(stage), ch);
                }
            } else {
                for (size_t k = i; k < j; k++) {
                    buf_append_ch(stage, &spos, sizeof(stage), to_lower_ch(clean[k]));
                }
            }
            i = j;
            continue;
        }

        buf_append_ch(stage, &spos, sizeof(stage), clean[i]);
        i++;
    }

    return adrift_tts_clean_say(stage, out, out_cap);
}

/* ---- Lexicon Binary Search + NRL-Style Letter-to-Sound Fallback ---- */

static bool lookup_lexicon(const char *word_lc, uint8_t *out_ph, uint32_t *io_len, uint32_t max_ph) {
    int32_t lo = 0;
    int32_t hi = (int32_t)ADRIFT_LEX_COUNT - 1;
    while (lo <= hi) {
        int32_t mid = lo + ((hi - lo) >> 1);
        int cmp = cstr_cmp(word_lc, k_lexicon[mid].word);
        if (cmp == 0) {
            uint16_t off = k_lexicon[mid].ph_off;
            uint8_t  cnt = k_lexicon[mid].ph_len;
            for (uint8_t i = 0u; i < cnt && *io_len < max_ph; i++) {
                out_ph[(*io_len)++] = k_lex_ph_blob[off + i];
            }
            return true;
        }
        if (cmp < 0) {
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    }
    return false;
}

static inline bool is_vowel_letter(char c) {
    return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' || c == 'y';
}

static inline void push_ph(uint8_t *out_ph, uint32_t *io_len, uint32_t max_ph, uint8_t id, uint8_t stress) {
    if (*io_len < max_ph) {
        out_ph[(*io_len)++] = ADRIFT_PH_PACK(id, stress);
    }
}

/* Compact public-domain NRL (1976)-inspired English grapheme-to-phoneme fallback
 * for custom user-named presets not present in the 417-word core dictionary. */
static void lts_fallback_word(const char *w, size_t n, uint8_t *out_ph, uint32_t *io_len, uint32_t max_ph) {
    if (n == 0u) {
        return;
    }
    if (n == 1u) {
        char s[2] = { w[0], '\0' };
        (void)lookup_lexicon(s, out_ph, io_len, max_ph);
        return;
    }

    bool stressed_done = false;
    bool silent_final_e = (n >= 3u && w[n - 1u] == 'e' && !is_vowel_letter(w[n - 2u]));
    size_t limit = silent_final_e ? (n - 1u) : n;

    size_t i = 0u;
    while (i < limit) {
        char c  = w[i];
        char c1 = (i + 1u < n) ? w[i + 1u] : '\0';
        char c2 = (i + 2u < n) ? w[i + 2u] : '\0';
        uint8_t st = stressed_done ? 0u : 1u;

        /* Digraphs */
        if (c == 'c' && c1 == 'h') { push_ph(out_ph, io_len, max_ph, ADRIFT_PH_CH, 0u); i += 2u; continue; }
        if (c == 's' && c1 == 'h') { push_ph(out_ph, io_len, max_ph, ADRIFT_PH_SH, 0u); i += 2u; continue; }
        if (c == 't' && c1 == 'h') { push_ph(out_ph, io_len, max_ph, ADRIFT_PH_TH, 0u); i += 2u; continue; }
        if (c == 'p' && c1 == 'h') { push_ph(out_ph, io_len, max_ph, ADRIFT_PH_F,  0u); i += 2u; continue; }
        if (c == 'n' && c1 == 'g') { push_ph(out_ph, io_len, max_ph, ADRIFT_PH_NG, 0u); i += 2u; continue; }
        if (c == 'c' && c1 == 'k') { push_ph(out_ph, io_len, max_ph, ADRIFT_PH_K,  0u); i += 2u; continue; }
        if (c == 'w' && c1 == 'h') { push_ph(out_ph, io_len, max_ph, ADRIFT_PH_W,  0u); i += 2u; continue; }
        if (c == 'q' && c1 == 'u') {
            push_ph(out_ph, io_len, max_ph, ADRIFT_PH_K, 0u);
            push_ph(out_ph, io_len, max_ph, ADRIFT_PH_W, 0u);
            i += 2u;
            continue;
        }

        /* Vowel digraphs & r-colored vowels */
        if (c == 'e' && (c1 == 'e' || c1 == 'a')) {
            push_ph(out_ph, io_len, max_ph, ADRIFT_PH_IY, st);
            stressed_done = true; i += 2u; continue;
        }
        if (c == 'o' && c1 == 'o') {
            push_ph(out_ph, io_len, max_ph, ADRIFT_PH_UW, st);
            stressed_done = true; i += 2u; continue;
        }
        if (c == 'o' && (c1 == 'u' || c1 == 'w')) {
            push_ph(out_ph, io_len, max_ph, ADRIFT_PH_AW, st);
            stressed_done = true; i += 2u; continue;
        }
        if (c == 'o' && (c1 == 'i' || c1 == 'y')) {
            push_ph(out_ph, io_len, max_ph, ADRIFT_PH_OY, st);
            stressed_done = true; i += 2u; continue;
        }
        if (c == 'a' && (c1 == 'i' || c1 == 'y')) {
            push_ph(out_ph, io_len, max_ph, ADRIFT_PH_EY, st);
            stressed_done = true; i += 2u; continue;
        }
        if (c == 'o' && c1 == 'a') {
            push_ph(out_ph, io_len, max_ph, ADRIFT_PH_OW, st);
            stressed_done = true; i += 2u; continue;
        }
        if ((c == 'e' || c == 'i' || c == 'u') && c1 == 'r') {
            push_ph(out_ph, io_len, max_ph, ADRIFT_PH_ER, st);
            stressed_done = true; i += 2u; continue;
        }
        if (c == 'a' && c1 == 'r') {
            push_ph(out_ph, io_len, max_ph, ADRIFT_PH_AA, st);
            push_ph(out_ph, io_len, max_ph, ADRIFT_PH_R,  0u);
            stressed_done = true; i += 2u; continue;
        }
        if (c == 'o' && c1 == 'r') {
            push_ph(out_ph, io_len, max_ph, ADRIFT_PH_AO, st);
            push_ph(out_ph, io_len, max_ph, ADRIFT_PH_R,  0u);
            stressed_done = true; i += 2u; continue;
        }

        /* Double consonants collapse to single */
        if (!is_vowel_letter(c) && c == c1) {
            i++;
            continue;
        }

        bool long_v = (silent_final_e && i + 2u == n - 1u) ||
                      (!is_vowel_letter(c1) && c1 != '\0' && c2 == 'e' && i + 3u == n);

        switch (c) {
            case 'a': push_ph(out_ph, io_len, max_ph, long_v ? ADRIFT_PH_EY : ADRIFT_PH_AE, st); stressed_done = true; break;
            case 'e': push_ph(out_ph, io_len, max_ph, long_v ? ADRIFT_PH_IY : ADRIFT_PH_EH, st); stressed_done = true; break;
            case 'i': push_ph(out_ph, io_len, max_ph, long_v ? ADRIFT_PH_AY : ADRIFT_PH_IH, st); stressed_done = true; break;
            case 'o': push_ph(out_ph, io_len, max_ph, long_v ? ADRIFT_PH_OW : ADRIFT_PH_AA, st); stressed_done = true; break;
            case 'u': push_ph(out_ph, io_len, max_ph, long_v ? ADRIFT_PH_UW : ADRIFT_PH_AH, st); stressed_done = true; break;
            case 'y':
                if (i == 0u) {
                    push_ph(out_ph, io_len, max_ph, ADRIFT_PH_Y, 0u);
                } else {
                    push_ph(out_ph, io_len, max_ph, (i + 1u == limit) ? ADRIFT_PH_IY : ADRIFT_PH_IH, st);
                    stressed_done = true;
                }
                break;
            case 'b': push_ph(out_ph, io_len, max_ph, ADRIFT_PH_B, 0u); break;
            case 'c':
                push_ph(out_ph, io_len, max_ph,
                        (c1 == 'e' || c1 == 'i' || c1 == 'y') ? ADRIFT_PH_S : ADRIFT_PH_K, 0u);
                break;
            case 'd': push_ph(out_ph, io_len, max_ph, ADRIFT_PH_D, 0u); break;
            case 'f': push_ph(out_ph, io_len, max_ph, ADRIFT_PH_F, 0u); break;
            case 'g':
                push_ph(out_ph, io_len, max_ph,
                        (c1 == 'e' || c1 == 'i' || c1 == 'y') ? ADRIFT_PH_JH : ADRIFT_PH_G, 0u);
                break;
            case 'h': push_ph(out_ph, io_len, max_ph, ADRIFT_PH_HH, 0u); break;
            case 'j': push_ph(out_ph, io_len, max_ph, ADRIFT_PH_JH, 0u); break;
            case 'k':
                if (!(i == 0u && c1 == 'n')) {
                    push_ph(out_ph, io_len, max_ph, ADRIFT_PH_K, 0u);
                }
                break;
            case 'l': push_ph(out_ph, io_len, max_ph, ADRIFT_PH_L, 0u); break;
            case 'm': push_ph(out_ph, io_len, max_ph, ADRIFT_PH_M, 0u); break;
            case 'n': push_ph(out_ph, io_len, max_ph, ADRIFT_PH_N, 0u); break;
            case 'p': push_ph(out_ph, io_len, max_ph, ADRIFT_PH_P, 0u); break;
            case 'q': push_ph(out_ph, io_len, max_ph, ADRIFT_PH_K, 0u); break;
            case 'r': push_ph(out_ph, io_len, max_ph, ADRIFT_PH_R, 0u); break;
            case 's': push_ph(out_ph, io_len, max_ph, ADRIFT_PH_S, 0u); break;
            case 't': push_ph(out_ph, io_len, max_ph, ADRIFT_PH_T, 0u); break;
            case 'v': push_ph(out_ph, io_len, max_ph, ADRIFT_PH_V, 0u); break;
            case 'w': push_ph(out_ph, io_len, max_ph, ADRIFT_PH_W, 0u); break;
            case 'x':
                push_ph(out_ph, io_len, max_ph, ADRIFT_PH_K, 0u);
                push_ph(out_ph, io_len, max_ph, ADRIFT_PH_S, 0u);
                break;
            case 'z': push_ph(out_ph, io_len, max_ph, ADRIFT_PH_Z, 0u); break;
            default: break;
        }
        i++;
    }
}

/* Convert an unsigned integer 0..99999 into spoken English phonemes */
static void phonemize_uint(uint32_t val, uint8_t *out_ph, uint32_t *io_len, uint32_t max_ph) {
    static const char *const k_ones[20] = {
        "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine",
        "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen",
        "seventeen", "eighteen", "nineteen"
    };
    static const char *const k_tens[10] = {
        "", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"
    };

    if (val >= 1000u) {
        uint32_t th = val / 1000u;
        if (th > 99u) th = 99u;
        phonemize_uint(th, out_ph, io_len, max_ph);
        (void)lookup_lexicon("thousand", out_ph, io_len, max_ph);
        val %= 1000u;
        if (val == 0u) return;
    }
    if (val >= 100u) {
        uint32_t h = val / 100u;
        (void)lookup_lexicon(k_ones[h], out_ph, io_len, max_ph);
        (void)lookup_lexicon("hundred", out_ph, io_len, max_ph);
        val %= 100u;
        if (val == 0u) return;
    }
    if (val >= 20u) {
        uint32_t t = val / 10u;
        (void)lookup_lexicon(k_tens[t], out_ph, io_len, max_ph);
        val %= 10u;
        if (val == 0u) return;
    }
    (void)lookup_lexicon(k_ones[val], out_ph, io_len, max_ph);
}

/* Translate a single word token at `s[idx..idx+wlen)` with lookahead into phonemes */
static void phonemize_token(const char *s,
                            size_t idx,
                            size_t wlen,
                            uint8_t *out_ph,
                            uint32_t *io_len,
                            uint32_t max_ph) {
    if (wlen == 0u) return;

    if (is_digit_ch(s[idx])) {
        uint32_t val = 0u;
        for (size_t i = 0u; i < wlen && i < 5u; i++) {
            val = val * 10u + (uint32_t)(s[idx + i] - '0');
        }
        phonemize_uint(val, out_ph, io_len, max_ph);
        return;
    }

    /* Rule 3: 'A' / 'a' article vs musical note A */
    if (wlen == 1u && (s[idx] == 'A' || s[idx] == 'a')) {
        size_t k = idx + 1u;
        while (s[k] == ' ') k++;
        bool next_is_word = is_alpha_ch(s[k]);
        bool stay_note    = false;

        if (!next_is_word) {
            /* Standalone or before punctuation/digits ("Root A.", "A, 1 of 12", "A 4") */
            stay_note = true;
        } else if (s[idx] == 'A') {
            size_t k2 = k;
            while (is_alpha_ch(s[k2])) k2++;
            if (is_note_a_follower(&s[k], k2 - k)) {
                stay_note = true;
            }
        }

        if (stay_note) {
            push_ph(out_ph, io_len, max_ph, ADRIFT_PH_EY, 1u);
        } else {
            push_ph(out_ph, io_len, max_ph, ADRIFT_PH_AX, 0u);
        }
        return;
    }

    char lc[40];
    size_t n = 0u;
    for (size_t i = 0u; i < wlen && n + 1u < sizeof(lc); i++) {
        char c = s[idx + i];
        if (is_alpha_ch(c)) {
            lc[n++] = to_lower_ch(c);
        }
    }
    lc[n] = '\0';
    if (n == 0u) return;

    if (!lookup_lexicon(lc, out_ph, io_len, max_ph)) {
        lts_fallback_word(lc, n, out_ph, io_len, max_ph);
    }
}

uint32_t adrift_tts_phonemize_phrase(const char *phrase, uint8_t *out_ph, uint32_t max_ph) {
    if (!phrase || !out_ph || max_ph == 0u) {
        return 0u;
    }
    uint32_t n_ph = 0u;
    size_t i = 0u;
    while (phrase[i] != '\0' && n_ph < max_ph) {
        if (is_space_ch(phrase[i])) {
            i++;
            continue;
        }
        if (is_punct_ch(phrase[i])) {
            push_ph(out_ph, &n_ph, max_ph, ADRIFT_PH_SIL, 0u);
            i++;
            continue;
        }
        if (is_alpha_ch(phrase[i])) {
            size_t j = i;
            while (is_alpha_ch(phrase[j]) || phrase[j] == '\'') j++;
            phonemize_token(phrase, i, j - i, out_ph, &n_ph, max_ph);
            i = j;
            continue;
        }
        if (is_digit_ch(phrase[i])) {
            size_t j = i;
            while (is_digit_ch(phrase[j])) j++;
            phonemize_token(phrase, i, j - i, out_ph, &n_ph, max_ph);
            i = j;
            continue;
        }
        i++;
    }
    return n_ph;
}

/* ---- Coarticulated Klatt Frame Trajectory Generator ---- */

static inline uint16_t lerp_u16(uint16_t a, uint16_t b, uint32_t num, uint32_t den) {
    if (den == 0u) return b;
    int32_t ia = (int32_t)a;
    int32_t ib = (int32_t)b;
    return (uint16_t)(ia + (((ib - ia) * (int32_t)num) / (int32_t)den));
}

static inline void zero_frame(adrift_klatt_frame_t *fr) {
    uint8_t *p = (uint8_t *)fr;
    for (size_t i = 0u; i < sizeof(*fr); i++) {
        p[i] = 0u;
    }
}

static void push_silence_frames(adrift_klatt_t *klatt, uint32_t count, uint8_t dur_mul) {
    adrift_klatt_frame_t fr;
    zero_frame(&fr);
    fr.f1      = 500u;
    fr.f2      = 1500u;
    fr.f3      = 2500u;
    fr.b1      = 90u;
    fr.b2      = 120u;
    fr.b3      = 160u;
    fr.fnz     = 270u;
    fr.dur_mul = dur_mul;
    for (uint32_t i = 0u; i < count; i++) {
        if (!adrift_klatt_push_frame(klatt, &fr)) {
            break;
        }
    }
}

static void emit_phoneme_list(adrift_tts_t *tts,
                              adrift_klatt_t *klatt,
                              const uint8_t *ph_list,
                              uint32_t n_ph,
                              uint16_t base_f0_hz10,
                              bool rise_pitch) {
    for (uint32_t p = 0u; p < n_ph; p++) {
        uint8_t id     = ADRIFT_PH_ID(ph_list[p]);
        uint8_t stress = ADRIFT_PH_STRESS(ph_list[p]);
        if (id >= ADRIFT_PH_COUNT) {
            continue;
        }
        const adrift_ph_spec_t *sp = &k_ph_specs[id];

        /* Peek next phoneme formants for [h] coarticulation & stop VOT transitions */
        uint16_t next_f1 = sp->f1_end;
        uint16_t next_f2 = sp->f2_end;
        uint16_t next_f3 = sp->f3_end;
        if (p + 1u < n_ph) {
            uint8_t nid = ADRIFT_PH_ID(ph_list[p + 1u]);
            if (nid < ADRIFT_PH_COUNT && nid != ADRIFT_PH_SIL) {
                next_f1 = k_ph_specs[nid].f1;
                next_f2 = k_ph_specs[nid].f2;
                next_f3 = k_ph_specs[nid].f3;
            }
        }

        uint16_t tgt_f1 = (id == ADRIFT_PH_HH) ? next_f1 : sp->f1;
        uint16_t tgt_f2 = (id == ADRIFT_PH_HH) ? next_f2 : sp->f2;
        uint16_t tgt_f3 = (id == ADRIFT_PH_HH) ? next_f3 : sp->f3;

        /* Plosive stop synthesis: closure + burst + VOT aspiration */
        if ((sp->flags & ADRIFT_PHF_STOP) != 0u) {
            bool voiced = (sp->flags & ADRIFT_PHF_VOICED) != 0u;
            uint32_t clos_frames = voiced ? 5u : 6u;
            for (uint32_t f = 0u; f < clos_frames; f++) {
                adrift_klatt_frame_t fr;
                zero_frame(&fr);
                fr.f0_hz10 = voiced ? base_f0_hz10 : 0u;
                fr.f1      = 200u;
                fr.f2      = lerp_u16(tts->prev_f2, tgt_f2, f + 1u, clos_frames);
                fr.f3      = lerp_u16(tts->prev_f3, tgt_f3, f + 1u, clos_frames);
                fr.b1      = 80u;
                fr.b2      = 140u;
                fr.b3      = 190u;
                fr.fnz     = 270u;
                fr.av      = voiced ? 34u : 0u;
                fr.avs     = voiced ? 48u : 0u;
                fr.dur_mul = 16u;
                (void)adrift_klatt_push_frame(klatt, &fr);
            }
            /* Burst frames (2 frames = ~10 ms) */
            for (uint32_t f = 0u; f < 2u; f++) {
                adrift_klatt_frame_t fr;
                zero_frame(&fr);
                fr.f0_hz10 = voiced ? base_f0_hz10 : 0u;
                fr.f1      = tgt_f1;
                fr.f2      = tgt_f2;
                fr.f3      = tgt_f3;
                fr.b1      = sp->b1;
                fr.b2      = sp->b2;
                fr.b3      = sp->b3;
                fr.fnz     = 270u;
                fr.av      = voiced ? 42u : 0u;
                fr.avs     = voiced ? 48u : 0u;
                fr.af      = sp->af;
                fr.a2      = sp->a2;
                fr.a3      = sp->a3;
                fr.a4      = sp->a4;
                fr.a5      = sp->a5;
                fr.a6      = sp->a6;
                fr.ab      = sp->ab;
                fr.dur_mul = 16u;
                (void)adrift_klatt_push_frame(klatt, &fr);
            }
            /* VOT aspiration for voiceless stops (P, T, K) moving toward next vowel */
            if (!voiced) {
                for (uint32_t f = 0u; f < 4u; f++) {
                    adrift_klatt_frame_t fr;
                    zero_frame(&fr);
                    fr.f0_hz10 = 0u;
                    fr.f1      = lerp_u16(tgt_f1, next_f1, f + 1u, 4u);
                    fr.f2      = lerp_u16(tgt_f2, next_f2, f + 1u, 4u);
                    fr.f3      = lerp_u16(tgt_f3, next_f3, f + 1u, 4u);
                    fr.b1      = 140u;
                    fr.b2      = 150u;
                    fr.b3      = 190u;
                    fr.fnz     = 270u;
                    fr.ah      = sp->ah;
                    fr.dur_mul = 16u;
                    (void)adrift_klatt_push_frame(klatt, &fr);
                }
            }
            tts->prev_f1 = tgt_f1;
            tts->prev_f2 = tgt_f2;
            tts->prev_f3 = tgt_f3;
            continue;
        }

        /* Affricates (CH, JH): short stop closure + postalveolar fricative release */
        if ((sp->flags & ADRIFT_PHF_AFFR) != 0u) {
            bool voiced = (sp->flags & ADRIFT_PHF_VOICED) != 0u;
            for (uint32_t f = 0u; f < 5u; f++) {
                adrift_klatt_frame_t fr;
                zero_frame(&fr);
                fr.f0_hz10 = voiced ? base_f0_hz10 : 0u;
                fr.f1      = 220u;
                fr.f2      = tgt_f2;
                fr.f3      = tgt_f3;
                fr.b1      = 90u;
                fr.b2      = 140u;
                fr.b3      = 190u;
                fr.fnz     = 270u;
                fr.av      = voiced ? 34u : 0u;
                fr.avs     = voiced ? 48u : 0u;
                fr.dur_mul = 16u;
                (void)adrift_klatt_push_frame(klatt, &fr);
            }
            for (uint32_t f = 0u; f < 9u; f++) {
                adrift_klatt_frame_t fr;
                zero_frame(&fr);
                fr.f0_hz10 = voiced ? base_f0_hz10 : 0u;
                fr.f1      = tgt_f1;
                fr.f2      = tgt_f2;
                fr.f3      = tgt_f3;
                fr.b1      = sp->b1;
                fr.b2      = sp->b2;
                fr.b3      = sp->b3;
                fr.fnz     = 270u;
                fr.av      = sp->av;
                fr.avs     = sp->avs;
                fr.af      = sp->af;
                fr.a3      = sp->a3;
                fr.a4      = sp->a4;
                fr.a5      = sp->a5;
                fr.sw_par  = sp->sw_par;
                fr.dur_mul = 16u;
                (void)adrift_klatt_push_frame(klatt, &fr);
            }
            tts->prev_f1 = tgt_f1;
            tts->prev_f2 = tgt_f2;
            tts->prev_f3 = tgt_f3;
            continue;
        }

        /* Vowels, diphthongs, liquids, glides, nasals, fricatives */
        uint32_t n_frames = (uint32_t)sp->base_frames;
        if ((sp->flags & ADRIFT_PHF_VOWEL) != 0u) {
            if (stress == 1u) {
                n_frames = (n_frames * 5u + 2u) >> 2; /* 1.25x primary stress lengthening */
            } else if (stress == 0u && id != ADRIFT_PH_AX) {
                n_frames = (n_frames * 7u + 4u) >> 3;
            }
            /* Final syllable lengthening at end of word before pause */
            if (p + 1u == n_ph || (p + 2u == n_ph && (k_ph_specs[ADRIFT_PH_ID(ph_list[p + 1u])].flags & ADRIFT_PHF_VOWEL) == 0u)) {
                n_frames += 2u;
            }
        }
        if (n_frames < 4u) n_frames = 4u;

        for (uint32_t f = 0u; f < n_frames; f++) {
            uint16_t cur_f1 = tgt_f1;
            uint16_t cur_f2 = tgt_f2;
            uint16_t cur_f3 = tgt_f3;

            /* Smooth 3-frame onset coarticulation from previous phoneme */
            if (f < 3u) {
                cur_f1 = lerp_u16(tts->prev_f1, tgt_f1, f + 1u, 3u);
                cur_f2 = lerp_u16(tts->prev_f2, tgt_f2, f + 1u, 3u);
                cur_f3 = lerp_u16(tts->prev_f3, tgt_f3, f + 1u, 3u);
            } else if ((sp->flags & ADRIFT_PHF_DIPH) != 0u && n_frames > 5u) {
                /* Smooth diphthong offglide across second 2/3 of vowel */
                uint32_t g_start = n_frames / 3u;
                if (f > g_start) {
                    uint32_t g_num = f - g_start;
                    uint32_t g_den = n_frames - g_start;
                    cur_f1 = lerp_u16(tgt_f1, sp->f1_end, g_num, g_den);
                    cur_f2 = lerp_u16(tgt_f2, sp->f2_end, g_num, g_den);
                    cur_f3 = lerp_u16(tgt_f3, sp->f3_end, g_num, g_den);
                }
            }

            /* Local pitch accent peak on stressed vowels + rising question intonation */
            uint16_t f0 = 0u;
            if ((sp->flags & ADRIFT_PHF_VOICED) != 0u) {
                int32_t f0_i = (int32_t)base_f0_hz10;
                if ((sp->flags & ADRIFT_PHF_VOWEL) != 0u) {
                    int32_t accent = (stress == 1u) ? 180 : ((stress == 2u) ? 80 : 0);
                    if (accent > 0 && n_frames > 2u) {
                        uint32_t mid = n_frames >> 1;
                        uint32_t dist = (f <= mid) ? (mid - f) : (f - mid);
                        int32_t shape = (mid > 0u) ? ((accent * (int32_t)(mid - (dist > mid ? mid : dist))) / (int32_t)mid) : accent;
                        f0_i += shape;
                    }
                }
                if (rise_pitch) {
                    f0_i += (int32_t)((320u * (f + 1u)) / n_frames);
                }
                if (f0_i < 750)  f0_i = 750;
                if (f0_i > 2400) f0_i = 2400;
                f0 = (uint16_t)f0_i;
            }

            adrift_klatt_frame_t fr;
            zero_frame(&fr);
            fr.f0_hz10 = f0;
            fr.f1      = cur_f1;
            fr.f2      = cur_f2;
            fr.f3      = cur_f3;
            fr.b1      = sp->b1;
            fr.b2      = sp->b2;
            fr.b3      = sp->b3;
            fr.fnz     = sp->fnz;
            fr.av      = sp->av;
            fr.avs     = sp->avs;
            fr.ah      = sp->ah;
            fr.af      = sp->af;
            fr.a2      = sp->a2;
            fr.a3      = sp->a3;
            fr.a4      = sp->a4;
            fr.a5      = sp->a5;
            fr.a6      = sp->a6;
            fr.ab      = sp->ab;
            fr.sw_par  = sp->sw_par;
            fr.dur_mul = 16u;
            (void)adrift_klatt_push_frame(klatt, &fr);
        }

        tts->prev_f1 = ((sp->flags & ADRIFT_PHF_DIPH) != 0u) ? sp->f1_end : tgt_f1;
        tts->prev_f2 = ((sp->flags & ADRIFT_PHF_DIPH) != 0u) ? sp->f2_end : tgt_f2;
        tts->prev_f3 = ((sp->flags & ADRIFT_PHF_DIPH) != 0u) ? sp->f3_end : tgt_f3;
    }

    /* Crisp 1-frame inter-word acoustic boundary so consecutive words articulate cleanly */
    adrift_klatt_frame_t sep;
    zero_frame(&sep);
    sep.f1      = tts->prev_f1;
    sep.f2      = tts->prev_f2;
    sep.f3      = tts->prev_f3;
    sep.b1      = 90u;
    sep.b2      = 120u;
    sep.b3      = 160u;
    sep.fnz     = 270u;
    sep.dur_mul = 10u;
    (void)adrift_klatt_push_frame(klatt, &sep);
}

void adrift_tts_init(adrift_tts_t *tts) {
    if (!tts) return;
    uint8_t *p = (uint8_t *)tts;
    for (size_t i = 0u; i < sizeof(*tts); i++) p[i] = 0u;
    tts->prev_f1 = 500u;
    tts->prev_f2 = 1500u;
    tts->prev_f3 = 2500u;
}

void adrift_tts_stop(adrift_tts_t *tts) {
    if (!tts) return;
    tts->active = false;
    tts->cursor = tts->len;
}

static uint16_t count_words(const char *s) {
    uint16_t cnt = 0u;
    size_t i = 0u;
    while (s[i] != '\0') {
        if (is_alpha_ch(s[i]) || is_digit_ch(s[i])) {
            cnt++;
            while (is_alpha_ch(s[i]) || is_digit_ch(s[i]) || s[i] == '\'') i++;
        } else {
            i++;
        }
    }
    return (cnt > 0u) ? cnt : 1u;
}

void adrift_tts_start(adrift_tts_t *tts, adrift_klatt_t *klatt, const char *text) {
    if (!tts || !klatt) return;
    tts->len         = (uint16_t)adrift_tts_clean_say(text, tts->text, sizeof(tts->text));
    tts->cursor      = 0u;
    tts->word_idx    = 0u;
    tts->total_words = count_words(tts->text);
    tts->prev_f1     = 500u;
    tts->prev_f2     = 1500u;
    tts->prev_f3     = 2500u;
    tts->is_question = false;
    for (uint16_t i = 0u; i < tts->len; i++) {
        if (tts->text[i] == '?') {
            tts->is_question = true;
            break;
        }
    }
    tts->active = (tts->len > 0u);
    if (tts->active) {
        (void)adrift_tts_pump(tts, klatt);
    }
}

bool adrift_tts_pump(adrift_tts_t *tts, adrift_klatt_t *klatt) {
    if (!tts || !klatt || !tts->active) {
        return false;
    }

    /* Synthesize word-by-word as long as Klatt FIFO has comfortable headroom (>= 96 frames) */
    while (tts->cursor < tts->len && adrift_klatt_free_frames(klatt) >= 96u) {
        char c = tts->text[tts->cursor];
        if (is_space_ch(c)) {
            tts->cursor++;
            continue;
        }
        if (is_punct_ch(c)) {
            if (c == ',' || c == ':' || c == ';') {
                push_silence_frames(klatt, 4u, 20u);
            } else {
                push_silence_frames(klatt, 6u, 22u);
            }
            tts->cursor++;
            continue;
        }

        if (is_alpha_ch(c) || is_digit_ch(c)) {
            uint16_t start = tts->cursor;
            if (is_alpha_ch(c)) {
                while (tts->cursor < tts->len &&
                       (is_alpha_ch(tts->text[tts->cursor]) || tts->text[tts->cursor] == '\'')) {
                    tts->cursor++;
                }
            } else {
                while (tts->cursor < tts->len && is_digit_ch(tts->text[tts->cursor])) {
                    tts->cursor++;
                }
            }

            uint8_t  ph_buf[32];
            uint32_t n_ph = 0u;
            phonemize_token(tts->text, start, (size_t)(tts->cursor - start), ph_buf, &n_ph, 32u);

            /* Natural sentence declination from 130 Hz -> 104 Hz; question rises before '?' */
            uint16_t tot = (tts->total_words > 1u) ? (uint16_t)(tts->total_words - 1u) : 1u;
            uint16_t idx = (tts->word_idx < tot) ? tts->word_idx : tot;
            uint16_t base_f0 = (uint16_t)(1300u - ((260u * (uint32_t)idx) / (uint32_t)tot));

            bool rise = false;
            if (tts->text[tts->cursor] == '?') {
                rise = true;
            }
            emit_phoneme_list(tts, klatt, ph_buf, n_ph, base_f0, rise);
            tts->word_idx++;
            continue;
        }

        tts->cursor++;
    }

    if (tts->cursor >= tts->len) {
        push_silence_frames(klatt, 2u, 16u);
        tts->active = false;
    }
    return tts->active;
}
