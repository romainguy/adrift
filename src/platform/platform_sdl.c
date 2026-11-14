/* SPDX-License-Identifier: MPL-2.0
 * Native macOS Fast-Iteration Simulator (SDL2 + CoreMIDI) for Adrift FM-1 Firmware */

#include <SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#include <CoreMIDI/CoreMIDI.h>
#endif

#include "../app.h"
#include "platform.h"

/* Window layout: 2x scaled 240x240 TFT (480x480) + right-hand interactive encoder/help deck */
#define SIM_WIN_W 820
#define SIM_WIN_H 520
#define TFT_X     20
#define TFT_Y     20
#define TFT_SCALE 2

/* Lock-free SPSC MIDI ring from CoreMIDI & SDL UI thread -> SDL audio callback */
#define SIM_MIDI_Q_LEN 256u
static adrift_midi_msg_t g_midi_q[SIM_MIDI_Q_LEN];
static volatile uint32_t g_midi_w = 0u;
static volatile uint32_t g_midi_r = 0u;
static SDL_SpinLock      g_midi_lock = 0;

static void sim_midi_push(uint8_t status, uint8_t d1, uint8_t d2) {
    SDL_AtomicLock(&g_midi_lock);
    if (g_midi_w - g_midi_r < SIM_MIDI_Q_LEN) {
        uint32_t idx = g_midi_w % SIM_MIDI_Q_LEN;
        g_midi_q[idx].status        = status;
        g_midi_q[idx].data1         = d1;
        g_midi_q[idx].data2         = d2;
        g_midi_q[idx].sample_offset = 0u;
        g_midi_w++;
    }
    SDL_AtomicUnlock(&g_midi_lock);
}

#ifdef __APPLE__
static MIDIClientRef   g_midi_client = 0;
static MIDIPortRef     g_midi_in_port = 0;
static MIDIEndpointRef g_midi_virt_dst = 0;

static void parse_midi_packet_bytes(const Byte *data, UInt16 len) {
    static uint8_t run_st = 0u;
    UInt16 i = 0u;
    while (i < len) {
        uint8_t b = data[i];
        if (b >= 0xF8u) {
            if (b == 0xF8u || b == 0xFAu || b == 0xFBu || b == 0xFCu) {
                sim_midi_push(b, 0u, 0u);
            }
            i++;
            continue;
        }
        if (b >= 0xF0u) {
            run_st = 0u;
            /* Skip SysEx or system common */
            while (i < len && data[i] != 0xF7u) i++;
            i++;
            continue;
        }
        uint8_t st;
        if (b & 0x80u) {
            st = b;
            run_st = b;
            i++;
        } else {
            if (!run_st) { i++; continue; }
            st = run_st;
        }
        uint8_t cmd = st & 0xF0u;
        uint8_t need = (cmd == 0xC0u || cmd == 0xD0u) ? 1u : 2u;
        if (i + need > len) break;
        uint8_t d1 = data[i] & 0x7Fu;
        uint8_t d2 = (need == 2u) ? (data[i + 1u] & 0x7Fu) : 0u;
        i += need;
        sim_midi_push(st, d1, d2);
    }
}

static void coremidi_read_proc(const MIDIPacketList *pktlist, void *refCon, void *connRefCon) {
    (void)refCon;
    (void)connRefCon;
    const MIDIPacket *pkt = &pktlist->packet[0];
    for (UInt32 i = 0; i < pktlist->numPackets; i++) {
        parse_midi_packet_bytes(pkt->data, pkt->length);
        pkt = MIDIPacketNext(pkt);
    }
}

static void sim_coremidi_init(void) {
    if (MIDIClientCreate(CFSTR("AdriftFM1Client"), NULL, NULL, &g_midi_client) != noErr) {
        return;
    }
    if (MIDIInputPortCreate(g_midi_client, CFSTR("AdriftFM1In"), coremidi_read_proc, NULL, &g_midi_in_port) == noErr) {
        ItemCount nsrc = MIDIGetNumberOfSources();
        for (ItemCount i = 0; i < nsrc; i++) {
            MIDIEndpointRef src = MIDIGetSource(i);
            if (src) MIDIPortConnectSource(g_midi_in_port, src, NULL);
        }
        printf("[Adrift SDL] Connected to %lu CoreMIDI hardware source(s)\n", (unsigned long)nsrc);
    }
    if (MIDIDestinationCreate(g_midi_client, CFSTR("Adrift FM1 Sim"), coremidi_read_proc, NULL, &g_midi_virt_dst) == noErr) {
        printf("[Adrift SDL] Created virtual CoreMIDI destination: \"Adrift FM1 Sim\"\n");
    }
}

static void sim_coremidi_shutdown(void) {
    if (g_midi_virt_dst) MIDIEndpointDispose(g_midi_virt_dst);
    if (g_midi_in_port)  MIDIPortDispose(g_midi_in_port);
    if (g_midi_client)   MIDIClientDispose(g_midi_client);
}
#endif

static volatile uint8_t g_sim_cpu_pct = 0u;
static bool    g_sdl_smoke_mode   = false;
static bool    g_sdl_flash_loaded = false;
static uint8_t g_sdl_flash[ADRIFT_STORAGE_TOTAL_SLOTS][ADRIFT_STORAGE_SLOT_BYTES];
static char    g_sdl_flash_path[1024];

static void sdl_storage_ensure_loaded(void) {
    if (g_sdl_flash_loaded) {
        return;
    }
    memset(g_sdl_flash, 0xFF, sizeof(g_sdl_flash));
    g_sdl_flash_path[0] = '\0';
    g_sdl_flash_loaded  = true;

    if (g_sdl_smoke_mode) {
        return;
    }

    const char *env_p = getenv("ADRIFT_PRESET_FILE");
    if (env_p && env_p[0] != '\0') {
        snprintf(g_sdl_flash_path, sizeof(g_sdl_flash_path), "%s", env_p);
    } else {
        char *pref = SDL_GetPrefPath("AdriftSynth", "Adrift");
        if (pref) {
            snprintf(g_sdl_flash_path, sizeof(g_sdl_flash_path), "%suser_presets.bin", pref);
            SDL_free(pref);
        }
    }

    if (g_sdl_flash_path[0] != '\0') {
        FILE *fp = fopen(g_sdl_flash_path, "rb");
        if (fp) {
            (void)fread(g_sdl_flash, 1u, sizeof(g_sdl_flash), fp);
            fclose(fp);
        }
    }
}

static bool sdl_storage_persist_file(void) {
    if (g_sdl_smoke_mode || g_sdl_flash_path[0] == '\0') {
        return true;
    }
    char tmp_path[1040];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", g_sdl_flash_path);
    FILE *fp = fopen(tmp_path, "wb");
    if (!fp) {
        return false;
    }
    size_t nw = fwrite(g_sdl_flash, 1u, sizeof(g_sdl_flash), fp);
    fflush(fp);
    fclose(fp);
    if (nw != sizeof(g_sdl_flash)) {
        remove(tmp_path);
        return false;
    }
    return rename(tmp_path, g_sdl_flash_path) == 0;
}

bool adrift_platform_storage_available(void) {
    sdl_storage_ensure_loaded();
    return true;
}

bool adrift_platform_storage_read(uint8_t slot, uint8_t *dst, uint32_t len) {
    if (slot >= ADRIFT_STORAGE_TOTAL_SLOTS || !dst || len > ADRIFT_STORAGE_SLOT_BYTES) {
        return false;
    }
    sdl_storage_ensure_loaded();
    if (len > 0u) {
        memcpy(dst, g_sdl_flash[slot], len);
    }
    return true;
}

bool adrift_platform_storage_write(uint8_t slot, const uint8_t *src, uint32_t len) {
    if (slot >= ADRIFT_STORAGE_TOTAL_SLOTS || !src || len == 0u || len > ADRIFT_STORAGE_SLOT_BYTES) {
        return false;
    }
    sdl_storage_ensure_loaded();
    memset(g_sdl_flash[slot], 0xFF, ADRIFT_STORAGE_SLOT_BYTES);
    memcpy(g_sdl_flash[slot], src, len);
    return sdl_storage_persist_file();
}

bool adrift_platform_storage_erase(uint8_t slot) {
    if (slot >= ADRIFT_STORAGE_TOTAL_SLOTS) {
        return false;
    }
    sdl_storage_ensure_loaded();
    memset(g_sdl_flash[slot], 0xFF, ADRIFT_STORAGE_SLOT_BYTES);
    return sdl_storage_persist_file();
}

static void sdl_audio_cb(void *userdata, Uint8 *stream, int len) {
    (void)userdata;
    uint32_t total_frames = (uint32_t)len / (2u * sizeof(int16_t));
    int16_t *out = (int16_t *)stream;
    uint64_t t0 = SDL_GetPerformanceCounter();

    uint32_t done = 0u;
    while (done < total_frames) {
        uint32_t chunk = total_frames - done;
        if (chunk > ADRIFT_BLOCK_SIZE) chunk = ADRIFT_BLOCK_SIZE;

        adrift_midi_msg_t evs[32];
        uint32_t nev = 0u;
        if (done == 0u) {
            SDL_AtomicLock(&g_midi_lock);
            while (g_midi_r != g_midi_w && nev < 32u) {
                evs[nev++] = g_midi_q[g_midi_r % SIM_MIDI_Q_LEN];
                g_midi_r++;
            }
            SDL_AtomicUnlock(&g_midi_lock);
        }

        adrift_app_audio_block(out + 2u * done, chunk, evs, nev);
        done += chunk;
    }

    uint64_t dt = SDL_GetPerformanceCounter() - t0;
    double elapsed_s = (double)dt / (double)SDL_GetPerformanceFrequency();
    double budget_s  = (double)total_frames / (double)ADRIFT_SAMPLE_RATE;
    int pct = (int)((elapsed_s / budget_s) * 100.0 + 0.5);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    g_sim_cpu_pct = (uint8_t)pct;
}

/* QWERTY piano mapping -> semitone offset above C4 (MIDI 60) */
static int scancode_to_semitone(SDL_Scancode sc) {
    switch (sc) {
        case SDL_SCANCODE_Z: return 0;  /* C */
        case SDL_SCANCODE_S: return 1;  /* C# */
        case SDL_SCANCODE_X: return 2;  /* D */
        case SDL_SCANCODE_D: return 3;  /* D# */
        case SDL_SCANCODE_C: return 4;  /* E */
        case SDL_SCANCODE_V: return 5;  /* F */
        case SDL_SCANCODE_G: return 6;  /* F# */
        case SDL_SCANCODE_B: return 7;  /* G */
        case SDL_SCANCODE_H: return 8;  /* G# */
        case SDL_SCANCODE_N: return 9;  /* A */
        case SDL_SCANCODE_J: return 10; /* A# */
        case SDL_SCANCODE_M: return 11; /* B */
        case SDL_SCANCODE_COMMA: return 12; /* C+1 */
        case SDL_SCANCODE_Q: return 12; /* Upper octave C */
        case SDL_SCANCODE_W: return 14; /* D */
        case SDL_SCANCODE_E: return 16; /* E */
        case SDL_SCANCODE_R: return 17; /* F */
        case SDL_SCANCODE_T: return 19; /* G */
        case SDL_SCANCODE_Y: return 21; /* A */
        case SDL_SCANCODE_U: return 23; /* B */
        case SDL_SCANCODE_I: return 24; /* C+2 */
        default: return -1;
    }
}

/* Virtual encoder knob centers in right-hand panel */
static void get_knob_center(int enc, int *cx, int *cy) {
    if (enc < 4) {
        *cx = 545 + (enc * 68);
        *cy = 120;
    } else {
        *cx = 575 + ((enc - 4) * 82);
        *cy = 215;
    }
}

static void draw_filled_circle(SDL_Renderer *ren, int cx, int cy, int r) {
    for (int dy = -r; dy <= r; dy++) {
        int dx = 0;
        while (dx * dx + dy * dy <= r * r) dx++;
        dx--;
        SDL_RenderDrawLine(ren, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}

int main(int argc, char **argv) {
    int smoke_frames = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--smoke-frames") == 0 && i + 1 < argc) {
            smoke_frames = atoi(argv[++i]);
        }
    }

    if (smoke_frames > 0) {
        g_sdl_smoke_mode = true;
        setenv("SDL_VIDEODRIVER", "dummy", 1);
        setenv("SDL_AUDIODRIVER", "dummy", 1);
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    adrift_app_init();

#ifdef __APPLE__
    if (smoke_frames == 0) {
        sim_coremidi_init();
    }
#endif

    SDL_Window *win = SDL_CreateWindow(
        "Adrift — Firmware Simulator (M-VAVE FM-1)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        SIM_WIN_W, SIM_WIN_H, SDL_WINDOW_SHOWN);
    if (!win) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) {
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    }
    SDL_Texture *tft_tex = SDL_CreateTexture(
        ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING,
        ADRIFT_SCREEN_W, ADRIFT_SCREEN_H);

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq     = ADRIFT_SAMPLE_RATE;
    want.format   = AUDIO_S16SYS;
    want.channels = 2;
    want.samples  = 256;
    want.callback = sdl_audio_cb;
    SDL_AudioDeviceID adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (adev != 0) {
        SDL_PauseAudioDevice(adev, 0);
    }

    if (smoke_frames == 0) {
        printf("[Adrift SDL] Controls (16-Voice Multi-Group):\n");
        printf("  QWERTY Piano : Z..M (lower oct), Q..I (upper oct), [ / ] Octave -/+\n");
        printf("  Screens 1..5 : Keys 1..5 (SEL=EXC, ENV=ENV1, LFO=BBD, EDIT=ENV2, GLO=MOD; press again for page 2/2)\n");
        printf("  Oscilloscope : Key 6 (HOME: Oscilloscope screen)\n");
        printf("  Group Select : Tab / 7 or Knob 5 SELECT (select G1..G6, or left-click Gx:y badge)\n");
        printf("  Group Config : 0, `, or Enter (SEQ/GRP: Group Settings 1/2 & 2/2, or right-click Gx:y badge)\n");
        printf("  Save Preset  : 8, \\, or Cmd+S/Ctrl+S (SAVE: Save User Preset U1..U24 + QWERTY name typing)\n");
        printf("  Arpeggiator  : 9 or A (ARP: Arpeggiator Parameters 1/2 & 2/2)\n");
        printf("  Master FX    : F or / (FX: Master Stereo Chorus / Delay / Reverb 1..3)\n");
        printf("  Presets      : - / = or Knob 6 PRESETS (open/scroll Presets; ] = OCT+ Load, [ = OCT- Cancel)\n");
        printf("  Key Scales   : ; / ' or Knob 7 ALGORITHM (open/scroll Scales, Knob 1 = Root; ] = OCT+ Apply, [ = OCT- Cancel)\n");
        printf("  Knobs 1..4   : Top 4 encoders edit the 4 parameters on the active page\n");
        printf("  Space / Bksp : PLAY = Panic (All Notes Off / clear BBD), Esc = Quit\n");
    }

    uint16_t rgb565_fb[ADRIFT_SCREEN_W * ADRIFT_SCREEN_H];
    memset(rgb565_fb, 0, sizeof(rgb565_fb));

    uint8_t active_scancode_note[SDL_NUM_SCANCODES];
    uint8_t active_scancode_chan[SDL_NUM_SCANCODES];
    memset(active_scancode_note, 0xFF, sizeof(active_scancode_note));
    memset(active_scancode_chan, 0, sizeof(active_scancode_chan));

    int     drag_enc    = -1;
    int     drag_accum  = 0;
    int32_t enc_acc[ADRIFT_NUM_ENC] = {0};
    int32_t knob_vis_angle[ADRIFT_NUM_ENC] = {0};
    uint32_t btn_down   = 0u;
    uint32_t prev_btn   = 0u;
    bool running        = true;
    int  frames_run     = 0;

    while (running) {
        uint32_t click_pulse_btn = 0u;
        char     frame_text_char = 0;
        uint8_t  frame_text_cmd  = ADRIFT_TEXT_CMD_NONE;
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
                case SDL_QUIT:
                    running = false;
                    break;

                case SDL_KEYDOWN: {
                    SDL_Keymod mods     = SDL_GetModState();
                    bool       cmd_ctrl = (mods & (KMOD_GUI | KMOD_CTRL)) != 0;
                    bool       shift_on = (mods & KMOD_SHIFT) != 0;
                    bool       in_save  = adrift_app_get_ui()->in_save_menu;

                    if (cmd_ctrl && !ev.key.repeat && ev.key.keysym.scancode == SDL_SCANCODE_S) {
                        click_pulse_btn |= ADRIFT_BTN_SAVE_PRESET;
                        break;
                    }

                    if (in_save) {
                        SDL_Scancode sc  = ev.key.keysym.scancode;
                        SDL_Keycode  sym = ev.key.keysym.sym;
                        if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) {
                            if (!ev.key.repeat) click_pulse_btn |= ADRIFT_BTN_OCT_UP;
                        } else if (sc == SDL_SCANCODE_ESCAPE) {
                            if (!ev.key.repeat) click_pulse_btn |= ADRIFT_BTN_OCT_DOWN;
                        } else if (sc == SDL_SCANCODE_BACKSPACE) {
                            frame_text_cmd = ADRIFT_TEXT_CMD_BACKSPACE;
                        } else if (sc == SDL_SCANCODE_DELETE) {
                            frame_text_cmd = ADRIFT_TEXT_CMD_DELETE;
                        } else if (sc == SDL_SCANCODE_LEFT) {
                            frame_text_cmd = ADRIFT_TEXT_CMD_LEFT;
                        } else if (sc == SDL_SCANCODE_RIGHT) {
                            frame_text_cmd = ADRIFT_TEXT_CMD_RIGHT;
                        } else if (sc == SDL_SCANCODE_UP) {
                            enc_acc[ADRIFT_ENC_PRESETS] -= 1;
                            knob_vis_angle[ADRIFT_ENC_PRESETS] -= 6;
                        } else if (sc == SDL_SCANCODE_DOWN) {
                            enc_acc[ADRIFT_ENC_PRESETS] += 1;
                            knob_vis_angle[ADRIFT_ENC_PRESETS] += 6;
                        } else if (sc == SDL_SCANCODE_TAB) {
                            int step = shift_on ? -1 : 1;
                            enc_acc[ADRIFT_ENC_PRESETS] += step;
                            knob_vis_angle[ADRIFT_ENC_PRESETS] += step * 6;
                        } else if (sym >= 'a' && sym <= 'z') {
                            frame_text_char = (char)(sym - 'a' + 'A');
                        } else if (sym >= '0' && sym <= '9' && !shift_on) {
                            frame_text_char = (char)sym;
                        } else if (sc == SDL_SCANCODE_SPACE) {
                            frame_text_char = ' ';
                        } else if (sc == SDL_SCANCODE_MINUS) {
                            frame_text_char = shift_on ? '_' : '-';
                        } else if (sc == SDL_SCANCODE_EQUALS && shift_on) {
                            frame_text_char = '+';
                        } else if (sc == SDL_SCANCODE_PERIOD && !shift_on) {
                            frame_text_char = '.';
                        } else if (sc == SDL_SCANCODE_SLASH && !shift_on) {
                            frame_text_char = '/';
                        }
                        break;
                    }

                    if (ev.key.repeat) break;
                    switch (ev.key.keysym.scancode) {
                        case SDL_SCANCODE_1: click_pulse_btn |= ADRIFT_BTN_PAGE_1; break;
                        case SDL_SCANCODE_2: click_pulse_btn |= ADRIFT_BTN_PAGE_2; break;
                        case SDL_SCANCODE_3: click_pulse_btn |= ADRIFT_BTN_PAGE_3; break;
                        case SDL_SCANCODE_4: click_pulse_btn |= ADRIFT_BTN_PAGE_4; break;
                        case SDL_SCANCODE_5: click_pulse_btn |= ADRIFT_BTN_PAGE_5; break;
                        case SDL_SCANCODE_6: click_pulse_btn |= ADRIFT_BTN_HOME;   break;
                        case SDL_SCANCODE_7:
                        case SDL_SCANCODE_TAB:
                            enc_acc[ADRIFT_ENC_SELECT] += shift_on ? -1 : 1;
                            knob_vis_angle[ADRIFT_ENC_SELECT] += 6;
                            break;
                        case SDL_SCANCODE_8:
                        case SDL_SCANCODE_BACKSLASH:    click_pulse_btn |= ADRIFT_BTN_SAVE_PRESET; break;
                        case SDL_SCANCODE_0:
                        case SDL_SCANCODE_GRAVE:
                        case SDL_SCANCODE_RETURN:       click_pulse_btn |= ADRIFT_BTN_GROUP_CFG;  break;
                        case SDL_SCANCODE_9:
                        case SDL_SCANCODE_A:            click_pulse_btn |= ADRIFT_BTN_ARP;        break;
                        case SDL_SCANCODE_F:
                        case SDL_SCANCODE_SLASH:        click_pulse_btn |= ADRIFT_BTN_FX;         break;
                        case SDL_SCANCODE_MINUS:
                            enc_acc[ADRIFT_ENC_PRESETS] -= 1;
                            knob_vis_angle[ADRIFT_ENC_PRESETS] -= 6;
                            break;
                        case SDL_SCANCODE_EQUALS:
                            enc_acc[ADRIFT_ENC_PRESETS] += 1;
                            knob_vis_angle[ADRIFT_ENC_PRESETS] += 6;
                            break;
                        case SDL_SCANCODE_SEMICOLON:
                            enc_acc[ADRIFT_ENC_ALGORITHM] -= 1;
                            knob_vis_angle[ADRIFT_ENC_ALGORITHM] -= 6;
                            break;
                        case SDL_SCANCODE_APOSTROPHE:
                            enc_acc[ADRIFT_ENC_ALGORITHM] += 1;
                            knob_vis_angle[ADRIFT_ENC_ALGORITHM] += 6;
                            break;
                        case SDL_SCANCODE_LEFTBRACKET:  click_pulse_btn |= ADRIFT_BTN_OCT_DOWN;   break;
                        case SDL_SCANCODE_RIGHTBRACKET: click_pulse_btn |= ADRIFT_BTN_OCT_UP;     break;
                        case SDL_SCANCODE_SPACE:
                        case SDL_SCANCODE_BACKSPACE:    click_pulse_btn |= ADRIFT_BTN_PANIC;      break;
                        case SDL_SCANCODE_ESCAPE:       running = false; break;
                        default: {
                            int semi = scancode_to_semitone(ev.key.keysym.scancode);
                            if (semi >= 0) {
                                adrift_ui_dismiss_boot(adrift_app_get_ui());
                                const adrift_synth_t *s_kbd = adrift_app_get_synth();
                                int32_t n = adrift_synth_map_kbd_key(s_kbd, semi);
                                if (n >= 0) {
                                    uint8_t ch_nib = (uint8_t)((adrift_synth_active_group(s_kbd)->midi_channel - 1u) & 0x0Fu);
                                    active_scancode_note[ev.key.keysym.scancode] = (uint8_t)n;
                                    active_scancode_chan[ev.key.keysym.scancode] = ch_nib;
                                    sim_midi_push((uint8_t)(0x90u | ch_nib), (uint8_t)n, 104u);
                                } else {
                                    active_scancode_note[ev.key.keysym.scancode] = 0xFFu;
                                }
                            }
                            break;
                        }
                    }
                    break;
                }

                case SDL_KEYUP: {
                    uint8_t n = active_scancode_note[ev.key.keysym.scancode];
                    if (n <= 127u) {
                        uint8_t ch_nib = active_scancode_chan[ev.key.keysym.scancode];
                        sim_midi_push((uint8_t)(0x80u | ch_nib), n, 0u);
                        active_scancode_note[ev.key.keysym.scancode] = 0xFFu;
                    }
                    break;
                }

                case SDL_MOUSEBUTTONDOWN:
                    if (adrift_app_get_ui()->boot_active &&
                        ev.button.x >= TFT_X && ev.button.x < TFT_X + 480 &&
                        ev.button.y >= TFT_Y && ev.button.y < TFT_Y + 480) {
                        adrift_ui_dismiss_boot(adrift_app_get_ui());
                        break;
                    }
                    if (ev.button.button == SDL_BUTTON_RIGHT) {
                        int mx = ev.button.x, my = ev.button.y;
                        if (mx >= TFT_X + 10 && mx < TFT_X + 260 && my >= TFT_Y && my <= TFT_Y + 42) {
                            click_pulse_btn |= ADRIFT_BTN_SAVE_PRESET;
                        } else if (mx >= TFT_X + 384 && mx < TFT_X + 480 && my >= TFT_Y && my <= TFT_Y + 42) {
                            click_pulse_btn |= ADRIFT_BTN_GROUP_CFG;
                        }
                    } else if (ev.button.button == SDL_BUTTON_LEFT) {
                        int mx = ev.button.x, my = ev.button.y;
                        for (int e = 0; e < (int)ADRIFT_NUM_ENC; e++) {
                            int kx, ky;
                            get_knob_center(e, &kx, &ky);
                            int dx = mx - kx, dy = my - ky;
                            if (dx * dx + dy * dy <= 28 * 28) {
                                drag_enc   = e;
                                drag_accum = 0;
                                break;
                            }
                        }
                        /* Allow clicking header preset badge/title (x=5..130, y=2..20 -> scaled 2x) to open Presets Browser */
                        if (!adrift_app_get_ui()->in_preset_menu &&
                            !adrift_app_get_ui()->in_save_menu &&
                            mx >= TFT_X + 10 && mx < TFT_X + 260 && my >= TFT_Y && my <= TFT_Y + 42) {
                            enc_acc[ADRIFT_ENC_PRESETS] += 1;
                            knob_vis_angle[ADRIFT_ENC_PRESETS] += 6;
                        }
                        /* In Save Preset modal, clicking any of the 12 character cells focuses that cell */
                        if (adrift_app_get_ui()->in_save_menu &&
                            mx >= TFT_X + 38 && mx < TFT_X + 446 &&
                            my >= TFT_Y + 236 && my <= TFT_Y + 288) {
                            int cell = (mx - (TFT_X + 38)) / 34;
                            if (cell >= 0 && cell < 12) {
                                adrift_app_get_ui()->save_cursor = (uint8_t)cell;
                            }
                        }
                        /* Allow clicking header [Gx:y] badge (x=193..235, y=2..20 -> scaled 2x) */
                        if (mx >= TFT_X + 384 && mx < TFT_X + 480 && my >= TFT_Y && my <= TFT_Y + 42) {
                            enc_acc[ADRIFT_ENC_SELECT] += 1;
                            knob_vis_angle[ADRIFT_ENC_SELECT] += 6;
                        }
                        /* Allow clicking top-left screen/page badge [EXC 1/2] or [SCOPE 1/6] to cycle subpages */
                        if (!adrift_app_get_ui()->in_preset_menu &&
                            !adrift_app_get_ui()->in_scale_menu &&
                            mx >= TFT_X + 10 && mx < TFT_X + 160 &&
                            my >= TFT_Y + 60 && my <= TFT_Y + 96) {
                            if (adrift_app_get_ui()->in_scope) {
                                click_pulse_btn |= ADRIFT_BTN_HOME;
                            } else if (adrift_app_get_ui()->in_group_menu) {
                                click_pulse_btn |= ADRIFT_BTN_GROUP_CFG;
                            } else if (adrift_app_get_ui()->in_arp_menu) {
                                click_pulse_btn |= ADRIFT_BTN_ARP;
                            } else if (adrift_app_get_ui()->in_fx_menu) {
                                click_pulse_btn |= ADRIFT_BTN_FX;
                            } else {
                                click_pulse_btn |= (1u << (adrift_app_get_ui()->page % 5u));
                            }
                        }
                        /* In Group Settings menu, clicking any G1..G6 column selects that group */
                        if (!adrift_app_get_ui()->in_preset_menu &&
                            !adrift_app_get_ui()->in_scale_menu &&
                            !adrift_app_get_ui()->in_scope &&
                            adrift_app_get_ui()->in_group_menu &&
                            mx >= TFT_X + 18 && mx < TFT_X + 462 &&
                            my >= TFT_Y + 96 && my <= TFT_Y + 264) {
                            int col = (mx - (TFT_X + 18)) / 74;
                            if (col >= 0 && col < ADRIFT_MAX_GROUPS) {
                                adrift_app_get_synth()->edit_group = (uint8_t)col;
                            }
                        }
                    }
                    break;

                case SDL_MOUSEBUTTONUP:
                    if (ev.button.button == SDL_BUTTON_LEFT) {
                        drag_enc = -1;
                    }
                    break;

                case SDL_MOUSEMOTION:
                    if (drag_enc >= 0) {
                        drag_accum -= ev.motion.yrel;
                        int steps = drag_accum / 4;
                        if (steps != 0) {
                            drag_accum -= steps * 4;
                            enc_acc[drag_enc] += steps;
                            knob_vis_angle[drag_enc] += steps * 6;
                        }
                    }
                    break;

                case SDL_MOUSEWHEEL: {
                    int mx, my;
                    SDL_GetMouseState(&mx, &my);
                    for (int e = 0; e < (int)ADRIFT_NUM_ENC; e++) {
                        int kx, ky;
                        get_knob_center(e, &kx, &ky);
                        int dx = mx - kx, dy = my - ky;
                        if (dx * dx + dy * dy <= 34 * 34) {
                            enc_acc[e] += ev.wheel.y;
                            knob_vis_angle[e] += ev.wheel.y * 6;
                            break;
                        }
                    }
                    break;
                }
            }
        }

        if (smoke_frames > 0 && frames_run == 1) {
            sim_midi_push(0x90u, 60u, 100u);
        }

        adrift_input_state_t in_st;
        for (uint32_t e = 0; e < ADRIFT_NUM_ENC; e++) {
            int32_t d = enc_acc[e];
            if (d > 64) d = 64;
            if (d < -64) d = -64;
            in_st.enc_delta[e] = (int8_t)d;
            enc_acc[e] = 0;
        }
        uint32_t cur_b         = btn_down | click_pulse_btn;
        in_st.buttons_down     = cur_b;
        in_st.buttons_pressed  = cur_b & ~prev_btn;
        in_st.buttons_released = prev_btn & ~cur_b;
        prev_btn               = btn_down;
        in_st.master_pot       = -1;
        in_st.cpu_load_pct     = g_sim_cpu_pct;
        in_st.text_char        = frame_text_char;
        in_st.text_cmd         = frame_text_cmd;

        adrift_dirty_t dirty;
        adrift_dirty_clear(&dirty);
        adrift_app_tick_ui(&in_st, rgb565_fb, &dirty);
        (void)adrift_preset_store_flush_pending();

        SDL_UpdateTexture(tft_tex, NULL, rgb565_fb, ADRIFT_SCREEN_W * sizeof(uint16_t));

        /* Render simulator chassis & 7 interactive encoders */
        SDL_SetRenderDrawColor(ren, 18, 20, 26, 255);
        SDL_RenderClear(ren);

        /* Bezel around 240x240 TFT */
        SDL_Rect bezel = { TFT_X - 4, TFT_Y - 4, 488, 488 };
        SDL_SetRenderDrawColor(ren, 52, 60, 78, 255);
        SDL_RenderFillRect(ren, &bezel);

        SDL_Rect tft_dst = { TFT_X, TFT_Y, 480, 480 };
        SDL_RenderCopy(ren, tft_tex, NULL, &tft_dst);

        /* Draw 7 interactive rotary encoders on right-hand deck in active group theme color */
        const adrift_ui_t *sdl_ui = adrift_app_get_ui();
        uint16_t base_acc565 = adrift_ui_group_accent_rgb565(adrift_app_get_synth()->edit_group);

        for (int e = 0; e < (int)ADRIFT_NUM_ENC; e++) {
            uint16_t acc565 = base_acc565;
            uint32_t bright = 256u;
            if (sdl_ui->boot_active) {
                int32_t bf   = (int32_t)sdl_ui->boot_frame;
                int32_t peak = 6 + e * 5;
                int32_t dist = (bf > peak) ? (bf - peak) : (peak - bf);
                uint32_t wave = (dist < 14) ? (uint32_t)(((14 - dist) * 256) / 14) : 0u;
                uint32_t hold = (bf >= peak && bf < 52) ? 140u : 0u;
                bright = (wave > hold) ? wave : hold;
                if (bf >= 52) {
                    bright = (uint32_t)(((bf - 52) * 256) / 16);
                    acc565 = base_acc565;
                } else {
                    acc565 = adrift_ui_group_accent_rgb565((uint8_t)(e % ADRIFT_MAX_GROUPS));
                }
            }
            Uint8 full_r = (Uint8)(((acc565 >> 11) & 0x1Fu) * 255u / 31u);
            Uint8 full_g = (Uint8)(((acc565 >> 5)  & 0x3Fu) * 255u / 63u);
            Uint8 full_b = (Uint8)(( acc565        & 0x1Fu) * 255u / 31u);
            Uint8 acc_r  = (Uint8)(((uint32_t)40u * (256u - bright) + (uint32_t)full_r * bright) >> 8);
            Uint8 acc_g  = (Uint8)(((uint32_t)46u * (256u - bright) + (uint32_t)full_g * bright) >> 8);
            Uint8 acc_b  = (Uint8)(((uint32_t)62u * (256u - bright) + (uint32_t)full_b * bright) >> 8);

            int kx, ky;
            get_knob_center(e, &kx, &ky);
            SDL_SetRenderDrawColor(ren, 40, 46, 62, 255);
            draw_filled_circle(ren, kx, ky, 26);
            if (drag_enc == e) {
                SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
            } else {
                SDL_SetRenderDrawColor(ren, acc_r, acc_g, acc_b, 255);
            }
            draw_filled_circle(ren, kx, ky, 19);
            SDL_SetRenderDrawColor(ren, 22, 26, 36, 255);
            draw_filled_circle(ren, kx, ky, 15);

            /* Indicator tick */
            uint32_t ang_q32 = (uint32_t)((knob_vis_angle[e] * 11930465));
            int32_t sx_q15 = dsp_sine_lut_q15(ang_q32);
            int32_t cy_q15 = dsp_sine_lut_q15(ang_q32 + 0x40000000u);
            int ix = kx + ((sx_q15 * 14) >> 15);
            int iy = ky - ((cy_q15 * 14) >> 15);
            SDL_SetRenderDrawColor(ren, acc_r, acc_g, acc_b, 255);
            SDL_RenderDrawLine(ren, kx, ky, ix, iy);
        }

        SDL_RenderPresent(ren);

        if (smoke_frames > 0) {
            /* Pump audio manually if dummy driver doesn't run fast enough */
            int16_t dummy_pcm[ADRIFT_BLOCK_SIZE * 2];
            sdl_audio_cb(NULL, (Uint8 *)dummy_pcm, sizeof(dummy_pcm));
            if (++frames_run >= smoke_frames) {
                running = false;
            }
        } else {
            SDL_Delay(16);
        }
    }

    if (adev != 0) SDL_CloseAudioDevice(adev);
    SDL_DestroyTexture(tft_tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
#ifdef __APPLE__
    if (smoke_frames == 0) sim_coremidi_shutdown();
#endif
    SDL_Quit();
    return 0;
}

bool adrift_platform_run_worker(void (*fn)(uint32_t arg), uint32_t arg) {
    (void)fn;
    (void)arg;
    return false;
}

bool adrift_platform_wait_worker(void) {
    return false;
}
