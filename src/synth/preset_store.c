/* SPDX-License-Identifier: MPL-2.0
 * Versioned little-endian User Preset Store (U1..U24) with CRC32 & backup-sector recovery */

#include "synth_internal.h"

#define PRESET_REC_VERSION     1u
#define PRESET_REC_FLAG_VALID  0x01u
#define PRESET_REC_HDR_BYTES   16u
#define PRESET_REC_CRC_BYTES   4u

typedef struct {
    bool                    occupied;
    uint32_t                seq;
    adrift_synth_snapshot_t snap;
} adrift_user_slot_t;

static adrift_user_slot_t g_user_slots[ADRIFT_NUM_USER_PRESETS];
static uint32_t           g_pending_mask = 0u;
static uint32_t           g_max_seq      = 0u;

static uint32_t crc32_ieee(const uint8_t *data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0u; i < len; i++) {
        crc ^= (uint32_t)data[i];
        for (uint32_t b = 0u; b < 8u; b++) {
            uint32_t mask = (crc & 1u) ? 0xEDB88320u : 0u;
            crc = (crc >> 1) ^ mask;
        }
    }
    return ~crc;
}

static inline void wr_u8(uint8_t *buf, uint32_t *off, uint8_t v) {
    buf[(*off)++] = v;
}

static inline void wr_i8(uint8_t *buf, uint32_t *off, int8_t v) {
    buf[(*off)++] = (uint8_t)v;
}

static inline void wr_u16(uint8_t *buf, uint32_t *off, uint16_t v) {
    uint32_t p = *off;
    buf[p + 0u] = (uint8_t)(v & 0xFFu);
    buf[p + 1u] = (uint8_t)((v >> 8) & 0xFFu);
    *off = p + 2u;
}

static inline void wr_i16(uint8_t *buf, uint32_t *off, int16_t v) {
    wr_u16(buf, off, (uint16_t)v);
}

static inline void wr_u32(uint8_t *buf, uint32_t *off, uint32_t v) {
    uint32_t p = *off;
    buf[p + 0u] = (uint8_t)(v & 0xFFu);
    buf[p + 1u] = (uint8_t)((v >> 8) & 0xFFu);
    buf[p + 2u] = (uint8_t)((v >> 16) & 0xFFu);
    buf[p + 3u] = (uint8_t)((v >> 24) & 0xFFu);
    *off = p + 4u;
}

static inline uint8_t rd_u8(const uint8_t *buf, uint32_t *off) {
    return buf[(*off)++];
}

static inline int8_t rd_i8(const uint8_t *buf, uint32_t *off) {
    return (int8_t)buf[(*off)++];
}

static inline uint16_t rd_u16(const uint8_t *buf, uint32_t *off) {
    uint32_t p = *off;
    *off = p + 2u;
    return (uint16_t)((uint32_t)buf[p] | ((uint32_t)buf[p + 1u] << 8));
}

static inline int16_t rd_i16(const uint8_t *buf, uint32_t *off) {
    return (int16_t)rd_u16(buf, off);
}

static inline uint32_t rd_u32(const uint8_t *buf, uint32_t *off) {
    uint32_t p = *off;
    *off = p + 4u;
    return (uint32_t)buf[p] |
           ((uint32_t)buf[p + 1u] << 8) |
           ((uint32_t)buf[p + 2u] << 16) |
           ((uint32_t)buf[p + 3u] << 24);
}

static void sanitize_preset_name(char dst[16], const char *src, uint8_t fallback_slot) {
    for (uint32_t i = 0u; i < 16u; i++) {
        dst[i] = '\0';
    }
    if (src) {
        /* Strip factory numeric prefix "01 " if passed directly */
        if (src[0] >= '0' && src[0] <= '9' &&
            src[1] >= '0' && src[1] <= '9' &&
            src[2] == ' ') {
            src += 3;
        }
        uint32_t n = 0u;
        while (src[n] && n < 12u) {
            char c = src[n];
            if (c >= 'a' && c <= 'z') {
                c = (char)(c - 'a' + 'A');
            }
            if (c < 0x20 || c > 0x5F) {
                c = ' ';
            }
            dst[n] = c;
            n++;
        }
        /* Trim trailing spaces */
        while (n > 0u && dst[n - 1u] == ' ') {
            dst[--n] = '\0';
        }
        /* Trim leading spaces if entire string would otherwise start blank */
        uint32_t lead = 0u;
        while (lead < n && dst[lead] == ' ') {
            lead++;
        }
        if (lead > 0u && lead < n) {
            for (uint32_t i = 0u; i < n - lead; i++) {
                dst[i] = dst[i + lead];
            }
            for (uint32_t i = n - lead; i < 16u; i++) {
                dst[i] = '\0';
            }
        } else if (lead == n) {
            dst[0] = '\0';
        }
    }

    if (dst[0] == '\0') {
        uint8_t num = (uint8_t)((fallback_slot % ADRIFT_NUM_USER_PRESETS) + 1u);
        dst[0] = 'U';
        dst[1] = 'S';
        dst[2] = 'E';
        dst[3] = 'R';
        dst[4] = ' ';
        dst[5] = 'U';
        if (num >= 10u) {
            dst[6] = (char)('0' + (num / 10u));
            dst[7] = (char)('0' + (num % 10u));
        } else {
            dst[6] = (char)('0' + num);
        }
    }
}

static void sanitize_patch_name(char dst[16], const char *src) {
    for (uint32_t i = 0u; i < 16u; i++) {
        dst[i] = '\0';
    }
    if (!src) return;
    for (uint32_t i = 0u; i < 15u && src[i]; i++) {
        char c = src[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (c < 0x20 || c > 0x5F) c = ' ';
        dst[i] = c;
    }
}

static uint32_t pack_record(uint8_t buf[ADRIFT_STORAGE_SLOT_BYTES],
                            uint8_t slot_idx,
                            uint32_t seq,
                            const adrift_synth_snapshot_t *snap) {
    for (uint32_t i = 0u; i < ADRIFT_STORAGE_SLOT_BYTES; i++) {
        buf[i] = 0xFFu;
    }

    uint32_t off = PRESET_REC_HDR_BYTES;

    char clean_name[16];
    sanitize_preset_name(clean_name, snap->preset_name, slot_idx);
    for (uint32_t i = 0u; i < 16u; i++) {
        wr_u8(buf, &off, (uint8_t)clean_name[i]);
    }

    wr_u8(buf, &off, (uint8_t)(snap->edit_group % ADRIFT_MAX_GROUPS));
    wr_u8(buf, &off, 0u);
    wr_u8(buf, &off, 0u);
    wr_u8(buf, &off, 0u);

    /* Master Stereo FX (12 x int16) */
    wr_i16(buf, &off, (int16_t)dsp_clamp_i32(snap->fx_params.chorus_mix, 0, 1023));
    wr_i16(buf, &off, (int16_t)dsp_clamp_i32(snap->fx_params.delay_time, 0, 1023));
    wr_i16(buf, &off, (int16_t)dsp_clamp_i32(snap->fx_params.delay_fb, 0, 1023));
    wr_i16(buf, &off, (int16_t)dsp_clamp_i32(snap->fx_params.delay_mix, 0, 1023));
    wr_i16(buf, &off, (int16_t)dsp_clamp_i32(snap->fx_params.reverb_size, 0, 1023));
    wr_i16(buf, &off, (int16_t)dsp_clamp_i32(snap->fx_params.reverb_damp, 0, 1023));
    wr_i16(buf, &off, (int16_t)dsp_clamp_i32(snap->fx_params.reverb_shimmer, 0, 1023));
    wr_i16(buf, &off, (int16_t)dsp_clamp_i32(snap->fx_params.reverb_mix, 0, 1023));
    wr_i16(buf, &off, (int16_t)dsp_clamp_i32(snap->fx_params.comp_mix, 0, 1023));
    wr_i16(buf, &off, (int16_t)dsp_clamp_i32(snap->fx_params.comp_thresh, 0, 1023));
    wr_i16(buf, &off, (int16_t)dsp_clamp_i32(snap->fx_params.comp_attack, 0, 1023));
    wr_i16(buf, &off, (int16_t)dsp_clamp_i32(snap->fx_params.comp_makeup, 0, 1023));

    /* 6 Voice Groups */
    for (uint8_t g = 0u; g < ADRIFT_MAX_GROUPS; g++) {
        const adrift_group_snapshot_t *gs = &snap->groups[g];
        const adrift_patch_t          *p  = &gs->patch;

        wr_u8(buf, &off, gs->num_voices);
        wr_u8(buf, &off, gs->assign_mode);
        wr_u8(buf, &off, gs->midi_channel);
        wr_i8(buf, &off, gs->transpose);
        wr_u8(buf, &off, gs->last_note);
        wr_u8(buf, &off, 0u);
        wr_i16(buf, &off, gs->unison_detune);
        wr_i16(buf, &off, gs->stereo_spread);
        wr_i16(buf, &off, gs->pan);

        char pname[16];
        sanitize_patch_name(pname, p->name);
        for (uint32_t i = 0u; i < 16u; i++) {
            wr_u8(buf, &off, (uint8_t)pname[i]);
        }

        /* Exciter */
        wr_i16(buf, &off, p->exc.wave_morph);
        wr_i16(buf, &off, p->exc.pulse_width);
        wr_i16(buf, &off, p->exc.vco_level);
        wr_i8(buf, &off, p->exc.vco_octave);
        wr_i8(buf, &off, p->exc.vco_semi);
        wr_i16(buf, &off, p->exc.vco_fine);
        wr_i16(buf, &off, p->exc.analog_drift);
        wr_u8(buf, &off, p->exc.vco_keytrack ? 1u : 0u);
        wr_u8(buf, &off, 0u);
        wr_i16(buf, &off, p->exc.noise_level);
        wr_i16(buf, &off, p->exc.sh_rate);
        wr_i16(buf, &off, p->exc.pluck);

        /* Env 1 */
        wr_i16(buf, &off, p->env1.onset_delay);
        wr_i16(buf, &off, p->env1.attack);
        wr_i16(buf, &off, p->env1.decay);
        wr_i16(buf, &off, p->env1.sustain);
        wr_i16(buf, &off, p->env1.release);
        wr_i16(buf, &off, p->env1.vel_sens);
        wr_u8(buf, &off, p->env1.loop_mode);
        wr_u8(buf, &off, p->env1.range);
        wr_u8(buf, &off, p->env1.drone_hold ? 1u : 0u);
        wr_u8(buf, &off, 0u);

        /* Input LPF, Dry/Wet, Master Vol */
        wr_i16(buf, &off, p->in_lpf_cutoff);
        wr_i16(buf, &off, p->in_lpf_res);
        wr_i16(buf, &off, p->dry_wet);
        wr_i16(buf, &off, p->master_vol);

        /* BBD Resonator */
        wr_i16(buf, &off, p->bbd.feedback);
        wr_i16(buf, &off, p->bbd.fb_lpf_cutoff);
        wr_i16(buf, &off, p->bbd.glide);
        wr_i16(buf, &off, p->bbd.bbd_noise);
        wr_i16(buf, &off, p->bbd.dispersion);
        wr_u8(buf, &off, p->bbd.invert ? 1u : 0u);
        wr_u8(buf, &off, p->bbd.invert_pitch_comp ? 1u : 0u);

        /* Env 2 */
        wr_i16(buf, &off, p->env2.onset_delay);
        wr_i16(buf, &off, p->env2.attack);
        wr_i16(buf, &off, p->env2.decay);
        wr_i16(buf, &off, p->env2.sustain);
        wr_i16(buf, &off, p->env2.release);
        wr_i16(buf, &off, p->env2.vel_sens);
        wr_u8(buf, &off, p->env2.loop_mode);
        wr_u8(buf, &off, p->env2.range);
        wr_u8(buf, &off, p->env2.drone_hold ? 1u : 0u);
        wr_u8(buf, &off, 0u);

        /* LFOs 1..3 + Performance */
        wr_u8(buf, &off, p->lfo_wave);
        wr_u8(buf, &off, p->lfo_dest);
        wr_i16(buf, &off, p->lfo_rate);
        wr_i16(buf, &off, p->lfo_depth);
        wr_u8(buf, &off, p->lfo2_wave);
        wr_u8(buf, &off, p->lfo2_dest);
        wr_i16(buf, &off, p->lfo2_rate);
        wr_i16(buf, &off, p->lfo2_depth);
        wr_u8(buf, &off, p->lfo3_wave);
        wr_u8(buf, &off, p->lfo3_dest);
        wr_i16(buf, &off, p->lfo3_rate);
        wr_i16(buf, &off, p->lfo3_depth);
        wr_i16(buf, &off, p->mw_depth);
        wr_u8(buf, &off, p->pb_range);
        wr_u8(buf, &off, p->legato ? 1u : 0u);

        /* Arpeggiator */
        wr_u8(buf, &off, p->arp.mode);
        wr_u8(buf, &off, p->arp.range);
        wr_u16(buf, &off, p->arp.rate_bpm);
        wr_u8(buf, &off, p->arp.gate);
        wr_u8(buf, &off, p->arp.velocity);
        wr_u8(buf, &off, p->arp.latch ? 1u : 0u);
        wr_u8(buf, &off, p->arp.swing);
        wr_u8(buf, &off, p->arp.ratchet);
        wr_u8(buf, &off, 0u);
    }

    uint16_t payload_len = (uint16_t)(off - PRESET_REC_HDR_BYTES);
    uint32_t hdr_off     = 0u;
    wr_u8(buf, &hdr_off, (uint8_t)'A');
    wr_u8(buf, &hdr_off, (uint8_t)'D');
    wr_u8(buf, &hdr_off, (uint8_t)'R');
    wr_u8(buf, &hdr_off, (uint8_t)'P');
    wr_u16(buf, &hdr_off, PRESET_REC_VERSION);
    wr_u8(buf, &hdr_off, slot_idx);
    wr_u8(buf, &hdr_off, PRESET_REC_FLAG_VALID);
    wr_u32(buf, &hdr_off, seq);
    wr_u16(buf, &hdr_off, payload_len);
    wr_u16(buf, &hdr_off, 0u);

    uint32_t crc = crc32_ieee(buf, off);
    wr_u32(buf, &off, crc);
    return off;
}

static bool unpack_record(const uint8_t buf[ADRIFT_STORAGE_SLOT_BYTES],
                          uint8_t *out_slot_idx,
                          uint32_t *out_seq,
                          adrift_synth_snapshot_t *out_snap) {
    if (buf[0] != (uint8_t)'A' || buf[1] != (uint8_t)'D' ||
        buf[2] != (uint8_t)'R' || buf[3] != (uint8_t)'P') {
        return false;
    }

    uint32_t hdr_off = 4u;
    uint16_t ver         = rd_u16(buf, &hdr_off);
    uint8_t  slot_idx    = rd_u8(buf, &hdr_off);
    uint8_t  flags       = rd_u8(buf, &hdr_off);
    uint32_t seq         = rd_u32(buf, &hdr_off);
    uint16_t payload_len = rd_u16(buf, &hdr_off);
    (void)rd_u16(buf, &hdr_off);

    if (ver != PRESET_REC_VERSION ||
        slot_idx >= ADRIFT_NUM_USER_PRESETS ||
        (flags & PRESET_REC_FLAG_VALID) == 0u) {
        return false;
    }

    const uint32_t v1_payload_len = 836u;
    if (payload_len < v1_payload_len ||
        (uint32_t)PRESET_REC_HDR_BYTES + (uint32_t)payload_len + PRESET_REC_CRC_BYTES > ADRIFT_STORAGE_SLOT_BYTES) {
        return false;
    }

    uint32_t crc_off  = (uint32_t)PRESET_REC_HDR_BYTES + (uint32_t)payload_len;
    uint32_t want_crc = rd_u32(buf, &crc_off);
    uint32_t got_crc  = crc32_ieee(buf, (uint32_t)PRESET_REC_HDR_BYTES + (uint32_t)payload_len);
    if (want_crc != got_crc) {
        return false;
    }

    uint8_t *raw = (uint8_t *)out_snap;
    for (uint32_t i = 0u; i < (uint32_t)sizeof(*out_snap); i++) {
        raw[i] = 0u;
    }

    uint32_t off = PRESET_REC_HDR_BYTES;
    char raw_name[16];
    for (uint32_t i = 0u; i < 16u; i++) {
        raw_name[i] = (char)rd_u8(buf, &off);
    }
    raw_name[15] = '\0';
    sanitize_preset_name(out_snap->preset_name, raw_name, slot_idx);
    out_snap->preset_index = (uint8_t)(ADRIFT_USER_PRESET_BASE + slot_idx);

    uint8_t eg = rd_u8(buf, &off);
    (void)rd_u8(buf, &off);
    (void)rd_u8(buf, &off);
    (void)rd_u8(buf, &off);
    out_snap->edit_group = (uint8_t)(eg % ADRIFT_MAX_GROUPS);

    out_snap->fx_params.chorus_mix     = dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);
    out_snap->fx_params.delay_time     = dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);
    out_snap->fx_params.delay_fb       = dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);
    out_snap->fx_params.delay_mix      = dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);
    out_snap->fx_params.reverb_size    = dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);
    out_snap->fx_params.reverb_damp    = dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);
    out_snap->fx_params.reverb_shimmer = dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);
    out_snap->fx_params.reverb_mix     = dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);
    out_snap->fx_params.comp_mix       = dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);
    out_snap->fx_params.comp_thresh    = dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);
    out_snap->fx_params.comp_attack    = dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);
    out_snap->fx_params.comp_makeup    = dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);

    uint8_t used_voices = 0u;
    for (uint8_t g = 0u; g < ADRIFT_MAX_GROUPS; g++) {
        adrift_group_snapshot_t *gs = &out_snap->groups[g];
        adrift_patch_t          *p  = &gs->patch;

        uint8_t nv       = rd_u8(buf, &off);
        uint8_t amode    = rd_u8(buf, &off);
        uint8_t mch      = rd_u8(buf, &off);
        int8_t  tr       = rd_i8(buf, &off);
        uint8_t lnote    = rd_u8(buf, &off);
        (void)rd_u8(buf, &off);
        int16_t udet     = rd_i16(buf, &off);
        int16_t ssprd    = rd_i16(buf, &off);
        int16_t pan      = rd_i16(buf, &off);

        uint8_t rem_v = (uint8_t)(ADRIFT_MAX_VOICES - used_voices);
        if (nv > rem_v) nv = rem_v;
        used_voices = (uint8_t)(used_voices + nv);

        gs->num_voices    = nv;
        gs->assign_mode   = (amode <= (uint8_t)ADRIFT_GROUP_SUPER) ? amode : (uint8_t)ADRIFT_GROUP_ROUND_ROBIN;
        gs->midi_channel  = (uint8_t)dsp_clamp_i32(mch, 1, 16);
        gs->transpose     = (int8_t)dsp_clamp_i32(tr, -24, 24);
        gs->last_note     = (lnote >= 1u && lnote <= 127u) ? lnote : 60u;
        gs->unison_detune = (int16_t)dsp_clamp_i32(udet, 0, 1023);
        gs->stereo_spread = (int16_t)dsp_clamp_i32(ssprd, 0, 1023);
        gs->pan           = (int16_t)dsp_clamp_i32(pan, -512, 512);

        char pname[16];
        for (uint32_t i = 0u; i < 16u; i++) {
            pname[i] = (char)rd_u8(buf, &off);
        }
        pname[15] = '\0';
        sanitize_patch_name(p->name, pname);

        /* Exciter */
        p->exc.wave_morph   = (int16_t)dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);
        p->exc.pulse_width  = (int16_t)dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);
        p->exc.vco_level    = (int16_t)dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);
        p->exc.vco_octave   = (int8_t)dsp_clamp_i32(rd_i8(buf, &off), -2, 2);
        p->exc.vco_semi     = (int8_t)dsp_clamp_i32(rd_i8(buf, &off), -12, 12);
        p->exc.vco_fine     = (int16_t)dsp_clamp_i32(rd_i16(buf, &off), -100, 100);
        p->exc.analog_drift = (int16_t)dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);
        p->exc.vco_keytrack = (rd_u8(buf, &off) != 0u);
        (void)rd_u8(buf, &off);
        p->exc.noise_level  = (int16_t)dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);
        p->exc.sh_rate      = (int16_t)dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);
        p->exc.pluck        = (int16_t)dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);

        /* Env 1 */
        p->env1.onset_delay = (int16_t)dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);
        p->env1.attack      = (int16_t)dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);
        p->env1.decay       = (int16_t)dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);
        p->env1.sustain     = (int16_t)dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);
        p->env1.release     = (int16_t)dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);
        p->env1.vel_sens    = (int16_t)dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);
        uint8_t e1_loop     = rd_u8(buf, &off);
        uint8_t e1_rng      = rd_u8(buf, &off);
        (void)rd_u8(buf, &off);
        (void)rd_u8(buf, &off);
        p->env1.loop_mode   = (e1_loop <= (uint8_t)ADRIFT_ENV_LOOP_DAD) ? e1_loop : (uint8_t)ADRIFT_ENV_LOOP_OFF;
        p->env1.range       = (e1_rng < (uint8_t)ADRIFT_ENV_RANGE_COUNT) ? e1_rng : (uint8_t)ADRIFT_ENV_RANGE_NORM;
        p->env1.drone_hold  = false;

        /* Input LPF, Dry/Wet, Master Vol */
        p->in_lpf_cutoff = (int16_t)dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);
        p->in_lpf_res    = (int16_t)dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);
        p->dry_wet       = (int16_t)dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);
        p->master_vol    = (int16_t)dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);

        /* BBD Resonator */
        p->bbd.feedback          = (int16_t)dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);
        p->bbd.fb_lpf_cutoff     = (int16_t)dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);
        p->bbd.glide             = (int16_t)dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);
        p->bbd.bbd_noise         = (int16_t)dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);
        p->bbd.dispersion        = (int16_t)dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);
        p->bbd.invert            = (rd_u8(buf, &off) != 0u);
        p->bbd.invert_pitch_comp = (rd_u8(buf, &off) != 0u);

        /* Env 2 */
        p->env2.onset_delay = (int16_t)dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);
        p->env2.attack      = (int16_t)dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);
        p->env2.decay       = (int16_t)dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);
        p->env2.sustain     = (int16_t)dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);
        p->env2.release     = (int16_t)dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);
        p->env2.vel_sens    = (int16_t)dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);
        (void)rd_u8(buf, &off);
        uint8_t e2_rng      = rd_u8(buf, &off);
        uint8_t e2_drone    = rd_u8(buf, &off);
        (void)rd_u8(buf, &off);
        p->env2.loop_mode   = (uint8_t)ADRIFT_ENV_LOOP_OFF;
        p->env2.range       = (e2_rng < (uint8_t)ADRIFT_ENV_RANGE_COUNT) ? e2_rng : (uint8_t)ADRIFT_ENV_RANGE_NORM;
        p->env2.drone_hold  = (e2_drone != 0u);

        /* LFOs 1..3 + Performance */
        uint8_t l1_w = rd_u8(buf, &off);
        uint8_t l1_d = rd_u8(buf, &off);
        p->lfo_wave  = (l1_w < (uint8_t)ADRIFT_LFO_COUNT) ? l1_w : 0u;
        p->lfo_dest  = (l1_d < ADRIFT_LFO1_DEST_COUNT) ? l1_d : 0u;
        p->lfo_rate  = (int16_t)dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);
        p->lfo_depth = (int16_t)dsp_clamp_i32(rd_i16(buf, &off), -512, 511);

        uint8_t l2_w = rd_u8(buf, &off);
        uint8_t l2_d = rd_u8(buf, &off);
        p->lfo2_wave  = (l2_w < (uint8_t)ADRIFT_LFO_COUNT) ? l2_w : 0u;
        p->lfo2_dest  = (l2_d < (uint8_t)ADRIFT_MOD_DEST_COUNT) ? l2_d : 0u;
        p->lfo2_rate  = (int16_t)dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);
        p->lfo2_depth = (int16_t)dsp_clamp_i32(rd_i16(buf, &off), -512, 511);

        uint8_t l3_w = rd_u8(buf, &off);
        uint8_t l3_d = rd_u8(buf, &off);
        p->lfo3_wave  = (l3_w < (uint8_t)ADRIFT_LFO_COUNT) ? l3_w : 0u;
        p->lfo3_dest  = (l3_d < (uint8_t)ADRIFT_MOD_DEST_COUNT) ? l3_d : 0u;
        p->lfo3_rate  = (int16_t)dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);
        p->lfo3_depth = (int16_t)dsp_clamp_i32(rd_i16(buf, &off), -512, 511);

        p->mw_depth = (int16_t)dsp_clamp_i32(rd_i16(buf, &off), 0, 1023);
        p->pb_range = (uint8_t)dsp_clamp_i32(rd_u8(buf, &off), 0, 24);
        p->legato   = (rd_u8(buf, &off) != 0u);

        /* Arpeggiator */
        uint8_t arp_m    = rd_u8(buf, &off);
        uint8_t arp_rng  = rd_u8(buf, &off);
        uint16_t arp_bpm = rd_u16(buf, &off);
        uint8_t arp_gate = rd_u8(buf, &off);
        uint8_t arp_vel  = rd_u8(buf, &off);
        uint8_t arp_lat  = rd_u8(buf, &off);
        uint8_t arp_swg  = rd_u8(buf, &off);
        uint8_t arp_rat  = rd_u8(buf, &off);
        (void)rd_u8(buf, &off);

        p->arp.mode     = (arp_m < (uint8_t)ADRIFT_ARP_MODE_COUNT) ? arp_m : (uint8_t)ADRIFT_ARP_OFF;
        p->arp.range    = (uint8_t)dsp_clamp_i32(arp_rng, 1, 4);
        p->arp.rate_bpm = (uint16_t)dsp_clamp_i32((int32_t)arp_bpm, 40, 240);
        p->arp.gate     = (uint8_t)dsp_clamp_i32(arp_gate, 5, 100);
        p->arp.velocity = (uint8_t)(arp_vel & 0x7Fu);
        p->arp.latch    = (arp_lat != 0u);
        p->arp.swing    = (uint8_t)dsp_clamp_i32(arp_swg, 0, 100);
        p->arp.ratchet  = (arp_rat < (uint8_t)ADRIFT_RATCHET_COUNT) ? arp_rat : (uint8_t)ADRIFT_RATCHET_1X;
    }

    if (out_slot_idx) *out_slot_idx = slot_idx;
    if (out_seq)      *out_seq      = seq;
    return true;
}

void adrift_preset_store_init(void) {
    uint8_t *raw = (uint8_t *)g_user_slots;
    for (uint32_t i = 0u; i < (uint32_t)sizeof(g_user_slots); i++) {
        raw[i] = 0u;
    }
    g_pending_mask = 0u;
    g_max_seq      = 0u;

    if (!adrift_platform_storage_available()) {
        return;
    }

    uint8_t buf[ADRIFT_STORAGE_SLOT_BYTES];
    for (uint8_t slot = 0u; slot < ADRIFT_NUM_USER_PRESETS; slot++) {
        if (!adrift_platform_storage_read(slot, buf, ADRIFT_STORAGE_SLOT_BYTES)) {
            continue;
        }
        uint8_t                 rec_slot = 0xFFu;
        uint32_t                rec_seq  = 0u;
        adrift_synth_snapshot_t snap;
        if (unpack_record(buf, &rec_slot, &rec_seq, &snap) && rec_slot == slot) {
            g_user_slots[slot].occupied = true;
            g_user_slots[slot].seq      = rec_seq;
            g_user_slots[slot].snap     = snap;
            if (rec_seq > g_max_seq) {
                g_max_seq = rec_seq;
            }
        }
    }

    /* Check backup sector (slot 24) to heal any mid-erase power cut */
    if (adrift_platform_storage_read(ADRIFT_STORAGE_BACKUP_SLOT, buf, ADRIFT_STORAGE_SLOT_BYTES)) {
        uint8_t                 bk_slot = 0xFFu;
        uint32_t                bk_seq  = 0u;
        adrift_synth_snapshot_t bk_snap;
        if (unpack_record(buf, &bk_slot, &bk_seq, &bk_snap) && bk_slot < ADRIFT_NUM_USER_PRESETS) {
            if (!g_user_slots[bk_slot].occupied || bk_seq >= g_user_slots[bk_slot].seq) {
                g_user_slots[bk_slot].occupied = true;
                g_user_slots[bk_slot].seq      = bk_seq;
                g_user_slots[bk_slot].snap     = bk_snap;
                if (bk_seq > g_max_seq) {
                    g_max_seq = bk_seq;
                }
                if (adrift_platform_storage_write(bk_slot, buf, ADRIFT_STORAGE_SLOT_BYTES)) {
                    (void)adrift_platform_storage_erase(ADRIFT_STORAGE_BACKUP_SLOT);
                } else {
                    g_pending_mask |= (1u << bk_slot);
                }
            } else {
                (void)adrift_platform_storage_erase(ADRIFT_STORAGE_BACKUP_SLOT);
            }
        }
    }
}

bool adrift_preset_store_flush_pending(void) {
    if (g_pending_mask == 0u) {
        return true;
    }
    if (!adrift_platform_storage_available()) {
        g_pending_mask = 0u;
        return false;
    }

    uint8_t buf[ADRIFT_STORAGE_SLOT_BYTES];
    bool    all_ok = true;

    for (uint8_t slot = 0u; slot < ADRIFT_NUM_USER_PRESETS; slot++) {
        uint32_t bit = (1u << slot);
        if ((g_pending_mask & bit) == 0u) {
            continue;
        }

        if (g_user_slots[slot].occupied) {
            (void)pack_record(buf, slot, g_user_slots[slot].seq, &g_user_slots[slot].snap);
            if (!adrift_platform_storage_write(ADRIFT_STORAGE_BACKUP_SLOT, buf, ADRIFT_STORAGE_SLOT_BYTES)) {
                all_ok = false;
                continue;
            }
            if (!adrift_platform_storage_write(slot, buf, ADRIFT_STORAGE_SLOT_BYTES)) {
                all_ok = false;
                continue;
            }
            (void)adrift_platform_storage_erase(ADRIFT_STORAGE_BACKUP_SLOT);
            g_pending_mask &= ~bit;
        } else {
            (void)adrift_platform_storage_erase(ADRIFT_STORAGE_BACKUP_SLOT);
            if (!adrift_platform_storage_erase(slot)) {
                all_ok = false;
                continue;
            }
            g_pending_mask &= ~bit;
        }
    }

    return all_ok;
}

bool adrift_synth_user_preset_is_occupied(uint8_t user_slot) {
    if (user_slot >= ADRIFT_NUM_USER_PRESETS) return false;
    return g_user_slots[user_slot].occupied;
}

uint8_t adrift_synth_first_empty_user_preset(void) {
    for (uint8_t slot = 0u; slot < ADRIFT_NUM_USER_PRESETS; slot++) {
        if (!g_user_slots[slot].occupied) {
            return slot;
        }
    }
    return 0u;
}

const char *adrift_synth_get_user_preset_name(uint8_t user_slot) {
    if (user_slot >= ADRIFT_NUM_USER_PRESETS || !g_user_slots[user_slot].occupied) {
        return "<EMPTY>";
    }
    return g_user_slots[user_slot].snap.preset_name;
}

void adrift_synth_get_user_preset_summary(uint8_t user_slot,
                                          uint8_t *out_total_voices,
                                          uint8_t *out_active_groups) {
    uint8_t tot_v = 0u;
    uint8_t act_g = 0u;
    if (user_slot < ADRIFT_NUM_USER_PRESETS && g_user_slots[user_slot].occupied) {
        const adrift_synth_snapshot_t *sn = &g_user_slots[user_slot].snap;
        for (uint8_t g = 0u; g < ADRIFT_MAX_GROUPS; g++) {
            uint8_t nv = sn->groups[g].num_voices;
            if (nv > 0u) {
                tot_v = (uint8_t)(tot_v + nv);
                act_g++;
            }
        }
    }
    if (out_total_voices)  *out_total_voices  = tot_v;
    if (out_active_groups) *out_active_groups = act_g;
}

const adrift_synth_snapshot_t *adrift_synth_get_user_preset_snapshot(uint8_t user_slot) {
    if (user_slot >= ADRIFT_NUM_USER_PRESETS || !g_user_slots[user_slot].occupied) {
        return (const adrift_synth_snapshot_t *)0;
    }
    return &g_user_slots[user_slot].snap;
}

bool adrift_synth_save_user_preset(adrift_synth_t *s, uint8_t user_slot, const char *name) {
    if (!s || user_slot >= ADRIFT_NUM_USER_PRESETS) {
        return false;
    }

    char clean_name[16];
    sanitize_preset_name(clean_name, name ? name : s->preset_name, user_slot);

    uint8_t new_pidx = (uint8_t)(ADRIFT_USER_PRESET_BASE + user_slot);
    s->preset_index  = new_pidx;
    for (uint32_t i = 0u; i < 16u; i++) {
        s->preset_name[i] = clean_name[i];
    }

    adrift_synth_save_snapshot(s, &g_user_slots[user_slot].snap);
    g_user_slots[user_slot].snap.preset_index = new_pidx;
    for (uint32_t i = 0u; i < 16u; i++) {
        g_user_slots[user_slot].snap.preset_name[i] = clean_name[i];
    }
    g_max_seq++;
    g_user_slots[user_slot].seq      = g_max_seq;
    g_user_slots[user_slot].occupied = true;
    g_pending_mask |= (1u << user_slot);
    return true;
}

bool adrift_synth_clear_user_preset(adrift_synth_t *s, uint8_t user_slot) {
    if (user_slot >= ADRIFT_NUM_USER_PRESETS) {
        return false;
    }
    g_user_slots[user_slot].occupied = false;
    g_pending_mask |= (1u << user_slot);
    if (s && s->preset_index == (uint8_t)(ADRIFT_USER_PRESET_BASE + user_slot)) {
        s->preset_index = ADRIFT_PRESET_INIT;
    }
    return true;
}
