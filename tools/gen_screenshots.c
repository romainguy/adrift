/* SPDX-License-Identifier: MPL-2.0
 * Headless PNG screenshot generator for all Adrift 240x240 RGB565 UI screens. */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

#include "app.h"
#include "platform/platform_headless.h"

static void write_be32(uint8_t *dst, uint32_t v) {
    dst[0] = (uint8_t)((v >> 24) & 0xFFu);
    dst[1] = (uint8_t)((v >> 16) & 0xFFu);
    dst[2] = (uint8_t)((v >> 8)  & 0xFFu);
    dst[3] = (uint8_t)( v        & 0xFFu);
}

static bool write_png_chunk(FILE *fp, const char type[4], const uint8_t *data, uint32_t len) {
    uint8_t hdr[8];
    write_be32(hdr, len);
    memcpy(hdr + 4, type, 4u);
    if (fwrite(hdr, 1u, 8u, fp) != 8u) return false;
    if (len > 0u && fwrite(data, 1u, len, fp) != len) return false;

    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (const Bytef *)type, 4u);
    if (len > 0u) {
        crc = crc32(crc, (const Bytef *)data, len);
    }
    uint8_t crc_be[4];
    write_be32(crc_be, (uint32_t)crc);
    return fwrite(crc_be, 1u, 4u, fp) == 4u;
}

static bool save_rgb565_png(const char *path, const uint16_t *fb, uint32_t scale) {
    if (scale == 0u) scale = 1u;
    const uint32_t out_w   = ADRIFT_SCREEN_W * scale;
    const uint32_t out_h   = ADRIFT_SCREEN_H * scale;
    const size_t   row_len = 1u + (size_t)out_w * 3u;
    const size_t   raw_len = row_len * (size_t)out_h;

    uint8_t *raw = (uint8_t *)malloc(raw_len);
    if (!raw) return false;

    for (uint32_t oy = 0; oy < out_h; oy++) {
        uint32_t sy   = oy / scale;
        uint8_t *line = raw + (size_t)oy * row_len;
        line[0] = 0u; /* PNG filter 0 (None) */
        for (uint32_t ox = 0; ox < out_w; ox++) {
            uint32_t sx = ox / scale;
            uint16_t c  = fb[sy * ADRIFT_SCREEN_W + sx];
            uint32_t r5 = (c >> 11) & 0x1Fu;
            uint32_t g6 = (c >> 5)  & 0x3Fu;
            uint32_t b5 =  c        & 0x1Fu;
            line[1u + ox * 3u + 0u] = (uint8_t)((r5 * 255u + 15u) / 31u);
            line[1u + ox * 3u + 1u] = (uint8_t)((g6 * 255u + 31u) / 63u);
            line[1u + ox * 3u + 2u] = (uint8_t)((b5 * 255u + 15u) / 31u);
        }
    }

    uLongf zcap = compressBound((uLong)raw_len);
    uint8_t *zbuf = (uint8_t *)malloc((size_t)zcap);
    if (!zbuf) {
        free(raw);
        return false;
    }
    if (compress2(zbuf, &zcap, raw, (uLong)raw_len, Z_BEST_COMPRESSION) != Z_OK) {
        free(zbuf);
        free(raw);
        return false;
    }
    free(raw);

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        free(zbuf);
        return false;
    }

    static const uint8_t k_png_sig[8] = { 137u, 80u, 78u, 71u, 13u, 10u, 26u, 10u };
    uint8_t ihdr[13];
    write_be32(ihdr + 0, out_w);
    write_be32(ihdr + 4, out_h);
    ihdr[8]  = 8u; /* 8 bits per channel */
    ihdr[9]  = 2u; /* Truecolor RGB */
    ihdr[10] = 0u; /* Deflate */
    ihdr[11] = 0u; /* Adaptive filter */
    ihdr[12] = 0u; /* Non-interlaced */

    bool ok = (fwrite(k_png_sig, 1u, 8u, fp) == 8u) &&
              write_png_chunk(fp, "IHDR", ihdr, sizeof(ihdr)) &&
              write_png_chunk(fp, "IDAT", zbuf, (uint32_t)zcap) &&
              write_png_chunk(fp, "IEND", NULL, 0u);

    fclose(fp);
    free(zbuf);
    return ok;
}

static void pump_audio_frames(uint32_t total_frames) {
    int16_t pcm[512 * 2];
    uint32_t rem = total_frames;
    while (rem > 0u) {
        uint32_t chunk = (rem > 512u) ? 512u : rem;
        adrift_headless_render(pcm, chunk, NULL, 0u);
        rem -= chunk;
    }
}

static void render_ui_frame(uint16_t *fb, uint8_t cpu_pct, bool force_full) {
    adrift_ui_t *ui = adrift_app_get_ui();
    if (force_full) {
        ui->initialized = false;
    }
    adrift_input_state_t in_st;
    memset(&in_st, 0, sizeof(in_st));
    in_st.master_pot   = -1;
    in_st.cpu_load_pct = cpu_pct;

    adrift_dirty_t dirty;
    adrift_dirty_clear(&dirty);
    adrift_app_tick_ui(&in_st, fb, &dirty);
}

static bool capture_screen(const char *out_dir,
                           const char *filename,
                           uint16_t *fb,
                           uint8_t cpu_pct,
                           uint32_t scale) {
    render_ui_frame(fb, cpu_pct, true);
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", out_dir, filename);
    if (!save_rgb565_png(path, fb, scale)) {
        fprintf(stderr, "ERROR: Failed to write %s\n", path);
        return false;
    }
    printf("  [png]    %s\n", path);
    return true;
}

/* Set up a rich 16-voice, 6-group multi-timbral stereo performance scene for HOME & GRP */
static void setup_multigroup_performance_scene(uint16_t *fb) {
    adrift_app_init();
    adrift_synth_t *s  = adrift_app_get_synth();
    adrift_ui_t    *ui = adrift_app_get_ui();

    adrift_synth_load_preset(s, 23u); /* P24: DRONE + LEAD base */
    snprintf(s->preset_name, sizeof(s->preset_name), "POLYPRISM");

    /* Reallocate all 16 voices across G1..G6 so all 6 groups & themes are live */
    adrift_synth_all_notes_off(s, true);
    for (uint8_t g = 0; g < ADRIFT_MAX_GROUPS; g++) {
        (void)adrift_synth_set_group_voices(s, g, 0u);
    }

    const uint8_t  k_nv[6]    = { 4u, 3u, 3u, 2u, 2u, 2u };
    const uint8_t  k_mode[6]  = {
        ADRIFT_GROUP_ROUND_ROBIN,
        ADRIFT_GROUP_ROUND_ROBIN,
        ADRIFT_GROUP_ROUND_ROBIN,
        ADRIFT_GROUP_SUPER,
        ADRIFT_GROUP_ROUND_ROBIN,
        ADRIFT_GROUP_UNISON
    };
    const uint8_t  k_patch[6] = { 0u, 11u, 4u, 6u, 5u, 7u };
    const int16_t  k_pan[6]   = { -70, -330, 200, -250, 340, 220 };
    const int16_t  k_sprd[6]  = { 560, 680, 640, 960, 820, 920 };
    const int16_t  k_det[6]   = { 0,   0,   0,   580, 0,   360 };
    const int16_t  k_vol[6]   = { 540, 480, 520, 620, 460, 440 };
    const int16_t  k_sust[6]  = { 520, 680, 560, 820, 480, 620 };

    for (uint8_t g = 0; g < ADRIFT_MAX_GROUPS; g++) {
        adrift_group_t *gp = &s->groups[g];
        gp->midi_channel         = (uint8_t)(g + 1u);
        gp->assign_mode          = k_mode[g];
        gp->pan                  = k_pan[g];
        gp->stereo_spread        = k_sprd[g];
        gp->unison_detune        = k_det[g];
        gp->transpose            = 0;
        gp->patch                = *adrift_synth_get_factory_patch(k_patch[g]);
        gp->patch.master_vol     = k_vol[g];
        gp->patch.env2.attack    = 20;
        gp->patch.env2.decay     = 380;
        gp->patch.env2.sustain   = k_sust[g];
        gp->patch.arp.range      = 1u;
        gp->patch.arp.rate_bpm   = 138u;
        gp->patch.arp.gate       = 80u;
        (void)adrift_synth_set_group_voices(s, g, k_nv[g]);
    }

    /* Enable melodic arpeggiators on G3 (UP/DN) & G5 (CONVRG) */
    s->groups[2].patch.arp.mode     = ADRIFT_ARP_UP_DOWN;
    s->groups[2].patch.arp.range    = 2u;
    s->groups[2].patch.arp.rate_bpm = 138u;
    s->groups[2].patch.arp.gate     = 80u;
    s->groups[2].patch.arp.latch    = true;
    adrift_synth_sync_group_arp(s, 2u);

    s->groups[4].patch.arp.mode     = ADRIFT_ARP_CONVERGE;
    s->groups[4].patch.arp.range    = 2u;
    s->groups[4].patch.arp.rate_bpm = 138u;
    s->groups[4].patch.arp.gate     = 75u;
    s->groups[4].patch.arp.latch    = true;
    adrift_synth_sync_group_arp(s, 4u);

    /* Master Stereo FX + Bus Compressor active */
    s->fx_params.chorus_mix     = 430;
    s->fx_params.delay_time     = 520;
    s->fx_params.delay_fb       = 520;
    s->fx_params.delay_mix      = 350;
    s->fx_params.reverb_size    = 840;
    s->fx_params.reverb_damp    = 320;
    s->fx_params.reverb_shimmer = 500;
    s->fx_params.reverb_mix     = 440;
    s->fx_params.comp_mix       = 740;
    s->fx_params.comp_thresh    = 380;
    s->fx_params.comp_attack    = 320;
    s->fx_params.comp_makeup    = 220;

    /* Stage notes progressively across 28 frames so the FFT waterfall shows rich time evolution
     * and all 16 voices stratify cleanly in pitch & stereo space for STAGE/POLY/PERF. */
    const uint8_t ch2_pad[3]   = { 48u, 53u, 57u };       /* Fmaj/C warm left pad on G2 */
    const uint8_t ch1_notes[4] = { 60u, 62u, 65u, 69u };  /* C-D-F-A mid chord on G1 */
    const uint8_t ch3_arp[4]   = { 67u, 72u, 76u, 79u };  /* Cmaj7 upper-mid arp on G3 */
    const uint8_t ch5_arp[3]   = { 81u, 86u, 91u };       /* High crystalline chime arp on G5 */

    adrift_synth_handle_midi(s, 0x93u, 34u, 116u); /* Bb1 Super Bass on G4 */

    ui->in_scope = true;
    for (int frame = 0; frame < 28; frame++) {
        if (frame == 4) {
            adrift_synth_handle_midi(s, 0x95u, 43u, 98u); /* G2 Unison Drone on G6 */
        } else if (frame == 9) {
            for (int i = 0; i < 3; i++) {
                adrift_synth_handle_midi(s, 0x91u, ch2_pad[i], 104u);
            }
        } else if (frame == 14) {
            for (int i = 0; i < 4; i++) {
                adrift_synth_handle_midi(s, 0x92u, ch3_arp[i], 106u);
            }
        } else if (frame == 19) {
            for (int i = 0; i < 3; i++) {
                adrift_synth_handle_midi(s, 0x94u, ch5_arp[i], 112u);
            }
        } else if (frame == 23) {
            for (int i = 0; i < 4; i++) {
                adrift_synth_handle_midi(s, 0x90u, ch1_notes[i], 114u);
            }
        }
        pump_audio_frames(1024u);

        ui->scope_gain = 400u;
        ui->scope_page = ADRIFT_HOME_SPEC;
        render_ui_frame(fb, 48u, frame == 0);

        ui->scope_gain = 75u;
        ui->scope_page = ADRIFT_HOME_POLY16;
        render_ui_frame(fb, 48u, frame == 0);

        ui->scope_page = ADRIFT_HOME_PERF;
        render_ui_frame(fb, 48u, frame == 0);
    }
}

int main(int argc, char **argv) {
    const char *out_dir = "screenshots";
    uint32_t    scale   = 2u;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            int s = atoi(argv[++i]);
            if (s >= 1 && s <= 8) scale = (uint32_t)s;
        } else if (argv[i][0] != '-') {
            out_dir = argv[i];
        }
    }

    if (mkdir(out_dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "ERROR: Failed to create directory '%s': %s\n", out_dir, strerror(errno));
        return 1;
    }

    uint16_t *fb = (uint16_t *)calloc(ADRIFT_SCREEN_W * ADRIFT_SCREEN_H, sizeof(uint16_t));
    if (!fb) return 1;

    printf("==> Generating Adrift UI screenshots (%ux%u, %ux scale) -> %s/\n",
           ADRIFT_SCREEN_W * scale, ADRIFT_SCREEN_H * scale, scale, out_dir);

    /* =========================================================================
     * 1..6: HOME Full-Screen Real-Time Visualizers (Pages 1/6 .. 6/6)
     * ========================================================================= */
    setup_multigroup_performance_scene(fb);
    adrift_synth_t *s  = adrift_app_get_synth();
    adrift_ui_t    *ui = adrift_app_get_ui();

    /* 01: HOME 1/6 — SCOPE (Stereo Dual Oscilloscope, G1 Warm Gold) */
    s->edit_group                     = 0u;
    ui->in_scope                      = true;
    ui->scope_page                    = ADRIFT_HOME_SCOPE;
    ui->scope_zoom                    = 1u; /* 2X */
    ui->scope_gain                    = 75u;
    ui->scope_mode[ADRIFT_HOME_SCOPE] = 1u; /* DUAL */
    if (!capture_screen(out_dir, "01_home_scope.png", fb, 46u, scale)) return 1;

    /* 02: HOME 2/6 — VECT (Stereo Vectorscope / Lissajous Goniometer, G2 Emerald) */
    s->edit_group                    = 1u;
    ui->scope_page                   = ADRIFT_HOME_VECT;
    ui->scope_zoom                   = 2u; /* 4X */
    ui->scope_gain                   = 75u;
    ui->scope_mode[ADRIFT_HOME_VECT] = 0u; /* GONI */
    if (!capture_screen(out_dir, "02_home_vect.png", fb, 48u, scale)) return 1;

    /* 03: HOME 3/6 — SPEC (64-pt FFT Spectrum Analyzer + 24-Row Waterfall, G3 Cyan) */
    s->edit_group                    = 2u;
    ui->scope_page                   = ADRIFT_HOME_SPEC;
    ui->scope_zoom                   = 0u; /* 1X */
    ui->scope_gain                   = 325u;
    ui->scope_mode[ADRIFT_HOME_SPEC] = 0u; /* SPLT */
    if (!capture_screen(out_dir, "03_home_spec.png", fb, 49u, scale)) return 1;

    /* 04: HOME 4/6 — STAGE (2D Stereo Soundstage & 16-Voice Constellation, G4 Coral) */
    s->edit_group                     = 3u;
    ui->scope_page                    = ADRIFT_HOME_STAGE;
    ui->scope_zoom                    = 0u; /* 1X */
    ui->scope_gain                    = 100u;
    ui->scope_mode[ADRIFT_HOME_STAGE] = 2u; /* HRM */
    if (!capture_screen(out_dir, "04_home_stage.png", fb, 48u, scale)) return 1;

    /* 05: HOME 5/6 — POLY (16-Voice Telemetry Bridge, G5 Synthwave Magenta) */
    s->edit_group                      = 4u;
    ui->scope_page                     = ADRIFT_HOME_POLY16;
    ui->scope_zoom                     = 0u; /* 1X */
    ui->scope_gain                     = 75u;
    ui->scope_mode[ADRIFT_HOME_POLY16] = 0u; /* TRI */
    if (!capture_screen(out_dir, "05_home_poly.png", fb, 50u, scale)) return 1;

    /* 06: HOME 6/6 — PERF (6-Group Performance, Arp & Master FX Dashboard, G1 Gold) */
    s->edit_group                    = 0u;
    ui->scope_page                   = ADRIFT_HOME_PERF;
    ui->scope_zoom                   = 0u; /* 1X */
    ui->scope_gain                   = 75u;
    ui->scope_mode[ADRIFT_HOME_PERF] = 0u; /* DASH */
    if (!capture_screen(out_dir, "06_home_perf.png", fb, 47u, scale)) return 1;

    /* =========================================================================
     * 7..9: EXC — Micro-ACB Exciter, Noise/S&H & Pluck (Subpages 1/3 .. 3/3)
     * ========================================================================= */
    adrift_app_init();
    s  = adrift_app_get_synth();
    ui = adrift_app_get_ui();
    adrift_synth_load_preset(s, 7u); /* P8: MISTERY PLK (Pluck + S&H Noise + +7 ST) */
    s->groups[0].patch.exc.wave_morph   = 780; /* SAW-PLS for expressive morph curve */
    s->groups[0].patch.exc.pulse_width  = 360;
    s->groups[0].patch.exc.analog_drift = 380;
    adrift_synth_handle_midi(s, 0x90u, 60u, 112u);
    adrift_synth_handle_midi(s, 0x90u, 64u, 108u);
    adrift_synth_handle_midi(s, 0x90u, 67u, 114u);
    pump_audio_frames(4096u);

    ui->in_scope = false;
    ui->page     = 0u;
    ui->subpage  = 0u;
    if (!capture_screen(out_dir, "07_exc_1_wave.png", fb, 32u, scale)) return 1;

    ui->subpage = 1u;
    if (!capture_screen(out_dir, "08_exc_2_noise_pluck.png", fb, 32u, scale)) return 1;

    ui->subpage = 2u;
    if (!capture_screen(out_dir, "09_exc_3_drift_keytrk.png", fb, 32u, scale)) return 1;

    /* =========================================================================
     * 10..11: ENV1 — Pre-BBD Exciter DADSR & Loop Envelope (Subpages 1/2 .. 2/2)
     * ========================================================================= */
    adrift_app_init();
    s  = adrift_app_get_synth();
    ui = adrift_app_get_ui();
    adrift_synth_load_preset(s, 1u); /* P2: BOWED CELLO (DADSR Onset Delay + Swell) */
    s->edit_group = 1u;              /* G2 Lush Emerald */
    s->groups[1].patch.env1.onset_delay = 340;
    s->groups[1].patch.env1.attack      = 520;
    s->groups[1].patch.env1.decay       = 600;
    s->groups[1].patch.env1.sustain     = 640;
    s->groups[1].patch.env1.release     = 520;
    adrift_synth_handle_midi(s, 0x90u, 57u, 110u);
    adrift_synth_handle_midi(s, 0x90u, 64u, 108u);
    pump_audio_frames(12288u);

    ui->page    = 1u;
    ui->subpage = 0u;
    if (!capture_screen(out_dir, "10_env1_1_adsr.png", fb, 34u, scale)) return 1;

    /* ENV1 2/2 showcasing multi-cycle D-A-D Looping Envelope */
    adrift_synth_load_preset(s, 6u); /* P7: 7 12 GLITTER (G2 = Looping D-A-D Fifth Glitter) */
    s->edit_group = 1u;              /* G2 Lush Emerald */
    adrift_synth_handle_midi(s, 0x90u, 60u, 112u);
    adrift_synth_handle_midi(s, 0x90u, 67u, 110u);
    pump_audio_frames(8192u);
    ui->page    = 1u;
    ui->subpage = 1u;
    if (!capture_screen(out_dir, "11_env1_2_loop.png", fb, 38u, scale)) return 1;

    /* =========================================================================
     * 12..13: BBD — 1024-Stage Key-Tracked BBD Resonator (Subpages 1/2 .. 2/2)
     * ========================================================================= */
    adrift_app_init();
    s  = adrift_app_get_synth();
    ui = adrift_app_get_ui();
    adrift_synth_load_preset(s, 14u); /* P15: SITAR JAWARI (G1..G3 active) */
    s->edit_group                       = 2u; /* G3 Electric Cyan */
    s->groups[2].patch.in_lpf_cutoff    = 740;
    s->groups[2].patch.in_lpf_res       = 460;
    s->groups[2].patch.bbd.feedback     = 880;
    s->groups[2].patch.bbd.fb_lpf_cutoff = 720;
    adrift_synth_handle_midi(s, 0x90u, 50u, 114u);
    adrift_synth_handle_midi(s, 0x90u, 57u, 110u);
    pump_audio_frames(8192u);

    ui->page    = 2u;
    ui->subpage = 0u;
    if (!capture_screen(out_dir, "12_bbd_1_filter_fb.png", fb, 39u, scale)) return 1;

    /* BBD 2/2 showcasing Polarity Invert (INV+0O) & All-Pass Dispersion */
    s->groups[2].patch.bbd.invert            = true;
    s->groups[2].patch.bbd.invert_pitch_comp = true;
    s->groups[2].patch.bbd.glide             = 240;
    s->groups[2].patch.bbd.dispersion        = 540;
    ui->page    = 2u;
    ui->subpage = 1u;
    if (!capture_screen(out_dir, "13_bbd_2_invert_disp.png", fb, 39u, scale)) return 1;

    /* =========================================================================
     * 14..15: ENV2 — Post-BBD Output VCA & Drone Hold (Subpages 1/2 .. 2/2)
     * ========================================================================= */
    adrift_app_init();
    s  = adrift_app_get_synth();
    ui = adrift_app_get_ui();
    adrift_synth_load_preset(s, 2u); /* P3: STRUMMER (G1..G4 active) */
    s->edit_group = 3u;              /* G4 Sunset Coral */
    s->groups[3].patch.env2.attack  = 180;
    s->groups[3].patch.env2.decay   = 620;
    s->groups[3].patch.env2.sustain = 720;
    s->groups[3].patch.env2.release = 640;
    adrift_synth_handle_midi(s, 0x90u, 60u, 114u);
    adrift_synth_handle_midi(s, 0x90u, 64u, 110u);
    pump_audio_frames(18000u);

    ui->page    = 3u;
    ui->subpage = 0u;
    if (!capture_screen(out_dir, "14_env2_1_adsr.png", fb, 37u, scale)) return 1;

    /* ENV2 2/2 showcasing hands-free Continuous Drone Hold */
    s->groups[3].patch.env2.onset_delay = 220;
    s->groups[3].patch.env2.drone_hold  = true;
    adrift_synth_wake_group_drone(s, 3u);
    pump_audio_frames(4096u);
    ui->page    = 3u;
    ui->subpage = 1u;
    if (!capture_screen(out_dir, "15_env2_2_drone.png", fb, 38u, scale)) return 1;

    /* =========================================================================
     * 16..19: MOD — Multi-LFO 1..3 Overlay & Mod Matrix (Subpages 1/4 .. 4/4)
     * ========================================================================= */
    adrift_app_init();
    s  = adrift_app_get_synth();
    ui = adrift_app_get_ui();
    adrift_synth_load_preset(s, 22u); /* P23: 5 DRUMKITS -> G1..G5 active */
    snprintf(s->preset_name, sizeof(s->preset_name), "MOD MATRIX");
    s->edit_group = 4u; /* G5 Synthwave Magenta */
    (void)adrift_synth_set_group_voices(s, 0u, 2u);
    (void)adrift_synth_set_group_voices(s, 4u, 4u);
    adrift_patch_t *mp = &s->groups[4].patch;
    mp->lfo_wave   = ADRIFT_LFO_SINE;
    mp->lfo_rate   = 360;
    mp->lfo_depth  = 310;
    mp->lfo_dest   = ADRIFT_MOD_FB_LPF;
    mp->lfo2_wave  = ADRIFT_LFO_TRI;
    mp->lfo2_rate  = 580;
    mp->lfo2_depth = -240;
    mp->lfo2_dest  = ADRIFT_MOD_VCO_MORPH;
    mp->lfo3_wave  = ADRIFT_LFO_SH;
    mp->lfo3_rate  = 680;
    mp->lfo3_depth = 360;
    mp->lfo3_dest  = ADRIFT_MOD_PAN;
    mp->mw_depth   = 460;
    adrift_synth_handle_midi(s, 0x94u, 60u, 112u);
    adrift_synth_handle_midi(s, 0x94u, 67u, 108u);
    pump_audio_frames(6144u);

    ui->page    = 4u;
    ui->subpage = 0u;
    if (!capture_screen(out_dir, "16_mod_1_lfo1.png", fb, 36u, scale)) return 1;

    ui->subpage = 1u;
    if (!capture_screen(out_dir, "17_mod_2_lfo2.png", fb, 36u, scale)) return 1;

    ui->subpage = 2u;
    if (!capture_screen(out_dir, "18_mod_3_lfo3.png", fb, 36u, scale)) return 1;

    ui->subpage = 3u;
    if (!capture_screen(out_dir, "19_mod_4_mw.png", fb, 36u, scale)) return 1;

    /* =========================================================================
     * 20..21: GRP — 6-Column Voice-Group Allocation Matrix (Subpages 1/2 .. 2/2)
     * ========================================================================= */
    setup_multigroup_performance_scene(fb);
    s  = adrift_app_get_synth();
    ui = adrift_app_get_ui();
    /* Free 2 voices on G6 (USED 14/16) so matrix shows colored slots, free-pool slots & OFF state */
    (void)adrift_synth_set_group_voices(s, 5u, 0u);
    s->groups[1].assign_mode  = ADRIFT_GROUP_UNISON;
    s->edit_group             = 3u; /* Select G4 (SUPER mode, 2 allocated + 2 free slots visible) */
    s->groups[3].pan          = -180;
    s->groups[3].patch.legato = true;
    ui->in_scope         = false;
    ui->in_group_menu    = true;
    ui->subpage          = 0u;
    if (!capture_screen(out_dir, "20_grp_1_matrix_assign.png", fb, 42u, scale)) return 1;

    ui->subpage = 1u;
    if (!capture_screen(out_dir, "21_grp_2_midi_pan.png", fb, 42u, scale)) return 1;

    /* =========================================================================
     * 22..23: ARP — 8-Step Arpeggiator & Ratchet Visualizer (Subpages 1/2 .. 2/2)
     * ========================================================================= */
    adrift_app_init();
    s  = adrift_app_get_synth();
    ui = adrift_app_get_ui();
    adrift_synth_load_preset(s, 6u); /* P7: 7 12 GLITTER */
    s->edit_group = 1u;              /* G2 Lush Emerald */
    adrift_arp_params_t *ap = &s->groups[1].patch.arp;
    ap->mode     = ADRIFT_ARP_UP_DOWN;
    ap->range    = 3u;
    ap->rate_bpm = 136u;
    ap->gate     = 75u;
    ap->velocity = 0u;
    ap->latch    = true;
    ap->swing    = 28u;
    ap->ratchet  = ADRIFT_RATCHET_ALT_3; /* 1-3X ratchet sub-slices */
    adrift_synth_sync_group_arp(s, 1u);

    adrift_synth_handle_midi(s, 0x90u, 60u, 112u);
    adrift_synth_handle_midi(s, 0x90u, 64u, 108u);
    adrift_synth_handle_midi(s, 0x90u, 67u, 110u);
    adrift_synth_handle_midi(s, 0x90u, 71u, 114u);
    /* Advance ~3.5 sixteenth-note steps so step 4 playhead is lit mid-pattern */
    pump_audio_frames(18500u);

    ui->in_arp_menu = true;
    ui->subpage     = 0u;
    if (!capture_screen(out_dir, "22_arp_1_pattern.png", fb, 35u, scale)) return 1;

    ui->subpage = 1u;
    if (!capture_screen(out_dir, "23_arp_2_groove_ratchet.png", fb, 35u, scale)) return 1;

    /* =========================================================================
     * 24..26: FX — Master Stereo FX & Soft-Knee Bus Compressor (Subpages 1/3 .. 3/3)
     * ========================================================================= */
    setup_multigroup_performance_scene(fb);
    s  = adrift_app_get_synth();
    ui = adrift_app_get_ui();
    s->edit_group               = 5u; /* G6 Luminous Violet */
    s->fx_params.comp_mix       = 780;
    s->fx_params.comp_thresh    = 280; /* ~-18 dB for healthy visible GR bar */
    s->fx_params.comp_attack    = 280;
    s->fx_params.comp_makeup    = 360;
    pump_audio_frames(8192u);

    ui->in_scope   = false;
    ui->in_fx_menu = true;
    ui->subpage    = 0u;
    if (!capture_screen(out_dir, "24_fx_1_chorus_delay.png", fb, 45u, scale)) return 1;

    ui->subpage = 1u;
    if (!capture_screen(out_dir, "25_fx_2_shimmer_reverb.png", fb, 45u, scale)) return 1;

    ui->subpage = 2u;
    if (!capture_screen(out_dir, "26_fx_3_bus_compressor.png", fb, 45u, scale)) return 1;

    /* =========================================================================
     * 27: PRESETS Browser (ALL/USER/FACT, FULL/GRP Single-Group Load, P1..P24 + U1..U24)
     * ========================================================================= */
    adrift_headless_storage_reset();
    adrift_app_init();
    s  = adrift_app_get_synth();
    ui = adrift_app_get_ui();
    adrift_synth_load_preset(s, 2u); /* Seed U1 from P3 (16V 4G) */
    adrift_synth_save_user_preset(s, 0u, "NEON DRIFT");
    adrift_preset_store_flush_pending();
    adrift_synth_load_preset(s, 6u); /* Seed U2 from P7 (12V 2G) */
    adrift_synth_save_user_preset(s, 1u, "TAPE CHOIR");
    adrift_preset_store_flush_pending();

    adrift_synth_load_preset(s, 23u); /* P24 = ACTIVE */
    adrift_synth_save_snapshot(s, &ui->preset_snapshot);
    adrift_synth_load_preset(s, 24u); /* Previewing U1: NEON DRIFT */
    adrift_synth_handle_midi(s, 0x90u, 60u, 112u);
    adrift_synth_handle_midi(s, 0x90u, 67u, 108u);
    pump_audio_frames(8192u);

    ui->in_preset_menu   = true;
    ui->preset_previewed = true;
    ui->preset_bank      = ADRIFT_PRESET_BANK_ALL;
    ui->preset_cursor    = 26u; /* U1: shows P22, P23, P24 (ACTIVE), [U1 NEON DRIFT], U2 TAPE CHOIR, U3..U4 <EMPTY> */
    if (!capture_screen(out_dir, "27_presets_browser.png", fb, 33u, scale)) return 1;

    /* =========================================================================
     * 28: SCALES Browser (16 Musical Scales + KNOB1 Root Note Selector)
     * ========================================================================= */
    adrift_app_init();
    s  = adrift_app_get_synth();
    ui = adrift_app_get_ui();
    adrift_synth_load_preset(s, 14u); /* P15: SITAR JAWARI */
    s->edit_group      = 2u;          /* G3 Electric Cyan */
    s->kbd_scale       = ADRIFT_SCALE_DORIAN;
    s->kbd_scale_root  = 2u;          /* Root = D */
    adrift_synth_handle_midi(s, 0x90u, 62u, 112u);
    pump_audio_frames(6144u);

    ui->in_scale_menu   = true;
    ui->scale_previewed = true;
    ui->scale_orig_type = ADRIFT_SCALE_CHROMATIC;
    ui->scale_orig_root = 0u;
    ui->scale_cursor    = ADRIFT_SCALE_MIN_PENTA; /* Shows CHR (ACTIVE), MAJOR, NAT MINOR, [MIN PENTA], MAJ PENTA, BLUES, DORIAN */
    if (!capture_screen(out_dir, "28_scales_browser.png", fb, 30u, scale)) return 1;

    /* =========================================================================
     * 29: SAVE PRESET Modal (U1..U24 Target Slot, 12-Char Name Editor & 6G+FX Summary)
     * ========================================================================= */
    setup_multigroup_performance_scene(fb);
    s  = adrift_app_get_synth();
    ui = adrift_app_get_ui();
    s->edit_group   = 0u;
    ui->in_scope    = false;
    ui->in_save_menu = true;
    ui->save_slot   = 1u; /* U02 (OVR: TAPE CHOIR) */
    ui->save_cursor = 6u; /* Active cell after "VELVET" */
    memcpy(ui->save_name,      "VELVET DRIFT", 13u);
    memcpy(ui->save_orig_name, "VELVET DRIFT", 13u);
    if (!capture_screen(out_dir, "29_preset_save.png", fb, 44u, scale)) return 1;

    adrift_headless_storage_reset();
    free(fb);
    printf("==> Done! Wrote 29 screenshots to %s/\n", out_dir);
    return 0;
}
