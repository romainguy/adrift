/* SPDX-License-Identifier: MPL-2.0
 * Adrift M-VAVE FM-1 Hardware Platform Layer (built on hugelton/felucca-bsp) */

#include <stdint.h>
#include "../app.h"
#include "platform.h"

#define FELUCCA_CDC 0
#define FELUCCA_OTA 1
#define FELUCCA_ID               "FM-1_462000200"
#define FELUCCA_USB_PID          0x0003
#define FELUCCA_USB_MANUFACTURER "Adrift Synth"
#define FELUCCA_USB_PRODUCT      "Adrift FM-1"
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
#define FM1_CPU1          1
#define FM1_CPU1_FALLBACK 1

#include "fm1_time.h"
#include "fm1_sys.h"
#include "fm1_irq.h"
#include "fm1_guard.h"
#include "fm1_cpu1.h"
#include "fm1_timer.h"
#include "fm1_input.h"
#include "fm1_adc.h"
#include "fm1_audio.h"
#include "fm1_lcd.h"

/* Reserve 25 x 4 KiB NOR flash sectors (0x0C0000..0x0D9000) inside the unencrypted
 * app data region [0x097000, 0x0E0000) for 24 User Presets + 1 atomic backup slot. */
#define ADRIFT_FLASH_PRESETS_LO 0x0C0000u
#define ADRIFT_FLASH_PRESETS_HI 0x0D9000u
#define FM1_FLASH_WRITE_REGIONS(X) \
    X(ADRIFT_FLASH_PRESETS_LO, ADRIFT_FLASH_PRESETS_HI) \
    X(FL_OTA_LO, FL_OTA_HI)

#include "fm1_flash.h"
#include "fm1_clock.h"
#include "fm1_uart.h"

#include "libc.c"
#include "usb.c"
#include "ota.c"

extern uint32_t _data_start[], _data_end[], _data_load[], _bss_start[], _bss_end[];
extern uint32_t _rt_start[], _rt_end[], _rt_load[];
extern void isr_alnk0(void);
extern void isr_timer5(void);

/* Boot-loop guard in .noinit: 2 consecutive crashes before 2s drop cleanly into UBOOT */
static volatile struct {
    uint32_t magic;
    uint32_t pending;
    uint32_t failed;
} g_boot_guard __attribute__((section(".noinit")));
#define BOOT_GUARD_MAGIC 0x41445231u /* "ADR1" */

static volatile uint32_t g_ms;
static volatile uint8_t  g_in_audio;
static volatile uint8_t  g_flash_io_active;
static volatile uint8_t  g_usb_due;
static volatile uint32_t g_cpu_q12;
static volatile uint8_t  g_cpu_pct;
static uint8_t           g_flash_ok;

/* ---- Double-buffered ALNK0 I2S DMA audio (64 stereo frames per half) ---- */
#define HALF_FRAMES ADRIFT_BLOCK_SIZE
#define HALF_WORDS  (HALF_FRAMES * 2u)
static int32_t g_abuf[2u * HALF_WORDS] __attribute__((aligned(4)));

/* ---- Panel note-key MIDI queue (main/timer -> audio ISR) ---- */
#define KBD_Q_LEN 32u
static adrift_midi_msg_t g_kbd_q[KBD_Q_LEN];
static volatile uint32_t g_kbd_w, g_kbd_r;

static void kbd_midi_push(uint8_t status, uint8_t d1, uint8_t d2) {
    if (g_kbd_w - g_kbd_r >= KBD_Q_LEN) return;
    uint32_t idx = g_kbd_w % KBD_Q_LEN;
    g_kbd_q[idx].status        = status;
    g_kbd_q[idx].data1         = d1;
    g_kbd_q[idx].data2         = d2;
    g_kbd_q[idx].sample_offset = 0u;
    RING_PUBLISH();
    g_kbd_w++;
}

/* ---- TRS 31250-baud UART1 MIDI IN (also fed by fm1-live CoreMIDI!) ---- */
#define UM_RING 128u
static volatile uint8_t g_um_ring[UM_RING] __attribute__((aligned(16)));
static struct {
    uint32_t rd;
    uint32_t pend;
    uint8_t  st;
    uint8_t  need;
    uint8_t  got;
    uint8_t  d0;
    uint8_t  sysex;
} g_um;

static void um_put_pkt(uint32_t pkt) {
    if (mi_w - mi_r >= MQ) return;
    midi_in_q[mi_w % MQ] = pkt | 0x10u; /* cable 1 = TRS MIDI jack */
    RING_PUBLISH();
    mi_w++;
}

static void uart_midi_byte(uint32_t b) {
    if (b >= 0xF8u) {
        if (b == 0xF8u || b == 0xFAu || b == 0xFBu || b == 0xFCu) {
            um_put_pkt(0x0Fu | (b << 8));
        }
        return; /* real-time clock bytes do not disturb running status */
    }
    if (b & 0x80u) {
        g_um.sysex = (b == 0xF0u);
        g_um.st    = (b < 0xF0u) ? (uint8_t)b : 0u;
        g_um.need  = ((b & 0xE0u) == 0xC0u) ? 1u : 2u;
        g_um.got   = 0u;
        return;
    }
    if (g_um.sysex || !g_um.st) return;
    if (g_um.need == 2u && !g_um.got) {
        g_um.d0  = (uint8_t)b;
        g_um.got = 1u;
        return;
    }
    uint32_t d1 = (g_um.need == 2u) ? g_um.d0 : b;
    uint32_t d2 = (g_um.need == 2u) ? b : 0u;
    g_um.got = 0u;
    um_put_pkt(((uint32_t)(g_um.st >> 4)) | ((uint32_t)g_um.st << 8) | (d1 << 16) | (d2 << 24));
}

static void uart_midi_poll(void) {
    uint32_t n = fm1_uart1_rx_take();
    if (!n) return;
    g_um.pend += n;
    if (g_um.pend > UM_RING) {
        g_um.rd   = (g_um.rd + g_um.pend - UM_RING) & (UM_RING - 1u);
        g_um.pend = UM_RING;
        g_um.st = g_um.got = g_um.sysex = 0u;
    }
    while (g_um.pend) {
        uart_midi_byte(g_um_ring[g_um.rd]);
        g_um.rd = (g_um.rd + 1u) & (UM_RING - 1u);
        g_um.pend--;
    }
}

/* ---- M-UPGRADE OTA flash callbacks ---- */
static void audio_silence_buf(void) {
    for (uint32_t i = 0; i < 2u * HALF_WORDS; i++) {
        g_abuf[i] = 0;
    }
}

static uint32_t ota_now_ms(void) { return g_ms; }
static void ota_idle(void) { fm1_wdt_feed(); }
static int ota_in_area(uint32_t off, uint32_t n) { return FL_IN(off, n, OTA_AREA, OTA_AREA + OTA_AREA_LEN); }
static int ota_erase(uint32_t off) {
    uint32_t took;
    if (!ota_in_area(off, 0x1000u) || (off & 0xFFFu)) return -8;
    audio_silence_buf();
    return fl_erase4k(off, &took);
}
static int ota_prog(uint32_t off, const void *p, uint32_t n) {
    if (!ota_in_area(off, n)) return -8;
    return fl_write(off, p, n);
}
static int ota_fread(uint32_t off, void *p, uint32_t n) {
    return fl_read(off, p, n);
}
static void lcd_dma_sync(void);

static void ota_show(uint32_t step, int32_t code) {
    lcd_dma_sync();
    fm1_lcd_fill_rect(0, 212, 240, 20, 0x0000u);
    if (step < 9u) {
        uint32_t w = (step > 6u ? 6u : step) * 40u;
        if (w) fm1_lcd_fill_rect(0, 212, w, 20, 0x07FFu);
    } else {
        fm1_lcd_fill_rect(0, 212, 240, 20, code == 1 ? 0x07E0u : 0xF800u);
    }
}
static void ota_commit(const uint8_t *parm) {
    lcd_dma_sync();
    g_boot_guard.pending = 0u;
    g_boot_guard.failed  = 0u;
    usb_detach();
    fm1_delay_ms(30);
    fm1_enter_update(parm);
}

static void fm1_fault(const fm1_crash_t *c) {
    (void)c;
    lcd_dma_sync();
    fm1_lcd_fill_rect(0, 0, 240, 240, 0xF800u);
    fm1_delay_ms(1200);
    fm1_reboot();
}

bool adrift_platform_run_worker(void (*fn)(uint32_t arg), uint32_t arg) {
    return fm1_cpu1_run(fn, arg) != 0;
}

bool adrift_platform_wait_worker(void) {
    return fm1_cpu1_wait() == 0;
}

static void flash_quiesce_enter(void) {
    g_flash_io_active = 1u;
    RING_PUBLISH();
    while (g_in_audio) {}
    while (fm1_cpu1_busy()) {}
    lcd_dma_sync();
    audio_silence_buf();
    fm1_wdt_feed();
}

static void flash_quiesce_leave(void) {
    fm1_wdt_feed();
    RING_PUBLISH();
    g_flash_io_active = 0u;
}

bool adrift_platform_storage_available(void) {
    return g_flash_ok != 0u;
}

bool adrift_platform_storage_read(uint8_t slot, uint8_t *dst, uint32_t len) {
    if (!g_flash_ok || slot >= ADRIFT_STORAGE_TOTAL_SLOTS || !dst || len > ADRIFT_STORAGE_SLOT_BYTES) {
        return false;
    }
    if (len == 0u) {
        return true;
    }
    uint32_t off = ADRIFT_FLASH_PRESETS_LO + (uint32_t)slot * 0x1000u;
    flash_quiesce_enter();
    int rc = fl_read(off, dst, len);
    flash_quiesce_leave();
    return (rc == 0);
}

bool adrift_platform_storage_write(uint8_t slot, const uint8_t *src, uint32_t len) {
    if (!g_flash_ok || slot >= ADRIFT_STORAGE_TOTAL_SLOTS || !src || len == 0u || len > ADRIFT_STORAGE_SLOT_BYTES) {
        return false;
    }
    uint32_t off  = ADRIFT_FLASH_PRESETS_LO + (uint32_t)slot * 0x1000u;
    uint32_t took = 0u;
    flash_quiesce_enter();
    int rc = fl_erase4k(off, &took);
    fm1_wdt_feed();
    if (rc == 0) {
        rc = fl_write(off, src, len);
        fm1_wdt_feed();
    }
    if (rc == 0) {
        uint8_t  vbuf[256];
        uint32_t pos = 0u;
        while (pos < len) {
            uint32_t chunk = (len - pos > 256u) ? 256u : (len - pos);
            if (fl_read(off + pos, vbuf, chunk) != 0) {
                rc = -10;
                break;
            }
            for (uint32_t i = 0u; i < chunk; i++) {
                if (vbuf[i] != src[pos + i]) {
                    rc = -11;
                    break;
                }
            }
            if (rc != 0) {
                break;
            }
            pos += chunk;
        }
    }
    flash_quiesce_leave();
    return (rc == 0);
}

bool adrift_platform_storage_erase(uint8_t slot) {
    if (!g_flash_ok || slot >= ADRIFT_STORAGE_TOTAL_SLOTS) {
        return false;
    }
    uint32_t off  = ADRIFT_FLASH_PRESETS_LO + (uint32_t)slot * 0x1000u;
    uint32_t took = 0u;
    flash_quiesce_enter();
    int rc = fl_erase4k(off, &took);
    flash_quiesce_leave();
    return (rc == 0);
}

/* ---- Audio DMA ISR (ALNK0 priority 3) ---- */
static void render_half(int32_t *dma_out) {
    if (g_flash_io_active) {
        for (uint32_t i = 0u; i < HALF_WORDS; i++) {
            dma_out[i] = 0;
        }
        return;
    }
    uint32_t t0 = fm1_ticks();
    const uint32_t budget_us = (HALF_FRAMES * 1000000u) / ADRIFT_SAMPLE_RATE;

    adrift_midi_msg_t events[48];
    uint32_t nev = 0u;

    /* Drain USB + TRS UART1 MIDI queue */
    while (mi_r != mi_w && nev < 36u) {
        uint32_t pkt = midi_in_q[mi_r % MQ];
        mi_r++;
        uint8_t cin = (uint8_t)(pkt & 0x0Fu);
        uint8_t st  = (uint8_t)((pkt >> 8) & 0xFFu);
        if ((cin >= 0x08u && cin <= 0x0Eu) ||
            ((cin == 0x0Fu || cin == 0x05u) && st >= 0xF8u)) {
            events[nev].status        = st;
            events[nev].data1         = (uint8_t)((pkt >> 16) & 0xFFu);
            events[nev].data2         = (uint8_t)((pkt >> 24) & 0xFFu);
            events[nev].sample_offset = 0u;
            nev++;
        }
    }

    /* Drain built-in 27-key keyboard MIDI events */
    while (g_kbd_r != g_kbd_w && nev < 48u) {
        events[nev++] = g_kbd_q[g_kbd_r % KBD_Q_LEN];
        g_kbd_r++;
    }

    int16_t pcm16[HALF_WORDS];
    adrift_app_audio_block(pcm16, HALF_FRAMES, events, nev);

    /* Convert signed 16-bit PCM -> signed Q23 in low 24 bits (-8388608..8388607) */
    for (uint32_t i = 0; i < HALF_WORDS; i++) {
        int32_t q23 = (int32_t)pcm16[i] * 256;
        if (q23 > FM1_AUDIO_SAMPLE_MAX) q23 = FM1_AUDIO_SAMPLE_MAX;
        if (q23 < FM1_AUDIO_SAMPLE_MIN) q23 = FM1_AUDIO_SAMPLE_MIN;
        dma_out[i] = q23;
    }

    uint32_t us = (fm1_ticks() - t0) / FM1_TICKS_PER_US;
    uint32_t q12 = (g_cpu_q12 * 15u + (us * 4096u) / budget_us + 8u) >> 4;
    g_cpu_q12 = q12;
    uint32_t pct = (q12 * 100u + 2048u) >> 12;
    g_cpu_pct = (pct > 100u) ? 100u : (uint8_t)pct;
}

void fm1_alnk0_irq(void) {
    uint8_t p = fm1_audio_pending();
    fm1_audio_ack_aux(p);
    if (p & FM1_AUDIO_HALF) {
        uint32_t half = fm1_audio_free_half();
        g_in_audio = 1u;
        render_half(&g_abuf[half * HALF_WORDS]);
        g_in_audio = 0u;
        fm1_audio_ack_half();
    }
}

/* ---- 10 kHz Timer ISR (TIMER5 priority 4: nests over ALNK0 for rock-steady key/LED scan) ---- */
void fm1_timer5_irq(void) {
    static uint32_t sub, last_ticks, tick_acc;
    fm1_timer5_ack();
    fm1_input_tick();

    uint32_t now = fm1_ticks();
    tick_acc += now - last_ticks;
    last_ticks = now;
    while (tick_acc >= 1000u * FM1_TICKS_PER_US) {
        tick_acc -= 1000u * FM1_TICKS_PER_US;
        g_ms++;
    }

    if (++sub >= 5u) {
        sub = 0u;
        g_usb_due = 1u;
    }
    if (g_usb_due && !g_in_audio) {
        g_usb_due = 0u;
        usb_poll();
        uart_midi_poll();
    }
}

/* Framebuffer & multi-row ping-pong SPI1 DMA blitter for 240x240 RGB565 panel */
#define LCD_DMA_CHUNK_ROWS  12u
#define LCD_DMA_ROW_WORDS   (ADRIFT_SCREEN_W / 2u)
#define LCD_DMA_CHUNK_WORDS (LCD_DMA_CHUNK_ROWS * LCD_DMA_ROW_WORDS)

static uint16_t g_fb[ADRIFT_SCREEN_W * ADRIFT_SCREEN_H] __attribute__((aligned(4)));
static uint32_t g_lcd_dma[2][LCD_DMA_CHUNK_WORDS] __attribute__((aligned(4)));
static uint8_t  g_lcd_dma_bank;
static uint8_t  g_lcd_dma_busy;

static void lcd_swap_words(uint32_t *dst, const uint32_t *src, uint32_t nwords) {
    while (nwords >= 4u) {
        uint32_t w0 = src[0], w1 = src[1], w2 = src[2], w3 = src[3];
        dst[0] = ((w0 >> 8) & 0x00FF00FFu) | ((w0 & 0x00FF00FFu) << 8);
        dst[1] = ((w1 >> 8) & 0x00FF00FFu) | ((w1 & 0x00FF00FFu) << 8);
        dst[2] = ((w2 >> 8) & 0x00FF00FFu) | ((w2 & 0x00FF00FFu) << 8);
        dst[3] = ((w3 >> 8) & 0x00FF00FFu) | ((w3 & 0x00FF00FFu) << 8);
        src    += 4;
        dst    += 4;
        nwords -= 4u;
    }
    while (nwords > 0u) {
        uint32_t w = *src++;
        *dst++ = ((w >> 8) & 0x00FF00FFu) | ((w & 0x00FF00FFu) << 8);
        nwords--;
    }
}

static void lcd_dma_sync(void) {
    if (g_lcd_dma_busy) {
        (void)fm1_lcd_done();
        g_lcd_dma_busy = 0u;
    }
}

static void lcd_blit_rows_fast(uint32_t y0, uint32_t h) {
    if (h == 0u || y0 >= ADRIFT_SCREEN_H) return;
    if (y0 + h > ADRIFT_SCREEN_H) h = ADRIFT_SCREEN_H - y0;

    uint8_t  bank     = g_lcd_dma_bank ^ 1u;
    uint32_t cur_rows = (h > LCD_DMA_CHUNK_ROWS) ? LCD_DMA_CHUNK_ROWS : h;
    const uint32_t *src = (const uint32_t *)(const void *)&g_fb[y0 * ADRIFT_SCREEN_W];
    lcd_swap_words(g_lcd_dma[bank], src, cur_rows * LCD_DMA_ROW_WORDS);
    src += cur_rows * LCD_DMA_ROW_WORDS;
    uint32_t rem = h - cur_rows;

    lcd_dma_sync();
    if (fm1_lcd_window(0u, y0, ADRIFT_SCREEN_W, h) != 0) {
        return;
    }

    for (;;) {
        RING_PUBLISH();
        fm1_lcd_send_data(g_lcd_dma[bank], cur_rows * ADRIFT_SCREEN_W * 2u);
        g_lcd_dma_bank = bank;
        g_lcd_dma_busy = 1u;

        if (rem == 0u) {
            return;
        }

        uint8_t  next_bank = bank ^ 1u;
        uint32_t next_rows = (rem > LCD_DMA_CHUNK_ROWS) ? LCD_DMA_CHUNK_ROWS : rem;
        lcd_swap_words(g_lcd_dma[next_bank], src, next_rows * LCD_DMA_ROW_WORDS);
        src += next_rows * LCD_DMA_ROW_WORDS;
        rem -= next_rows;

        if (fm1_lcd_done() != 0) {
            g_lcd_dma_busy = 0u;
            return;
        }
        g_lcd_dma_busy = 0u;
        bank     = next_bank;
        cur_rows = next_rows;
    }
}

static inline bool dirty_row_test(const adrift_dirty_t *dirty, uint32_t y) {
    return (dirty->row_mask[y >> 5] & (1u << (y & 31u))) != 0u;
}

static void flush_dirty_lcd(const adrift_dirty_t *dirty) {
    if (dirty->full_redraw) {
        lcd_blit_rows_fast(0u, ADRIFT_SCREEN_H);
        return;
    }
    uint32_t y = 0u;
    while (y < ADRIFT_SCREEN_H) {
        if (!dirty_row_test(dirty, y)) {
            y++;
            continue;
        }
        uint32_t y0 = y;
        for (;;) {
            while (y < ADRIFT_SCREEN_H && dirty_row_test(dirty, y)) {
                y++;
            }
            uint32_t gap = 0u;
            while (y + gap < ADRIFT_SCREEN_H && gap <= 3u && !dirty_row_test(dirty, y + gap)) {
                gap++;
            }
            if (gap <= 3u && y + gap < ADRIFT_SCREEN_H) {
                y += gap;
            } else {
                break;
            }
        }
        lcd_blit_rows_fast(y0, y - y0);
    }
}

/* Physical panel encoder mapping:
 *   Parameter slots 0..3 (1x4 card row) <- KNOB1 (2), KNOB2 (3), KNOB3 (4), KNOB4 (5)
 *   Navigation encoders 4..6            <- SELECT (0: Group), PRESETS (6: Preset), ALGORITHM (1: Scale) */
static const uint8_t k_panel_enc_map[ADRIFT_NUM_ENC] = { 2, 3, 4, 5, 0, 6, 1 };

static uint32_t map_hw_buttons(uint32_t hw_b) {
    uint32_t out = 0u;
    if (hw_b & (1u << 0))  out |= ADRIFT_BTN_OCT_DOWN;            /* OCT- -> Octave Down */
    if (hw_b & (1u << 1))  out |= ADRIFT_BTN_OCT_UP;              /* OCT+ -> Octave Up */
    if (hw_b & (1u << 2))  out |= ADRIFT_BTN_FX;                  /* FX   -> Master FX Settings (1/2, 2/2) */
    if (hw_b & (1u << 3))  out |= ADRIFT_BTN_PAGE_1;              /* SCL/SEL -> Screen 1: EXC  (1/2, 2/2) */
    if (hw_b & (1u << 4))  out |= ADRIFT_BTN_PAGE_2;              /* ENV     -> Screen 2: ENV1 (1/2, 2/2) */
    if (hw_b & (1u << 5))  out |= ADRIFT_BTN_PAGE_3;              /* LFO     -> Screen 3: BBD  (1/2, 2/2) */
    if (hw_b & (1u << 6))  out |= ADRIFT_BTN_PAGE_4;              /* EDIT    -> Screen 4: ENV2 (1/2, 2/2) */
    if (hw_b & (1u << 7))  out |= ADRIFT_BTN_PAGE_5;              /* GLO     -> Screen 5: MOD  (1/2, 2/2) */
    if (hw_b & (1u << 8))  out |= ADRIFT_BTN_HOME;                /* HOME    -> Oscilloscope screen */
    if (hw_b & (1u << 9))  out |= ADRIFT_BTN_SAVE_PRESET;         /* SAVE    -> Save User Preset modal */
    if (hw_b & (1u << 10)) out |= ADRIFT_BTN_ARP;                 /* ARP     -> Arpeggiator Settings (1/2, 2/2) */
    if (hw_b & (1u << 11)) out |= ADRIFT_BTN_GROUP_CFG;           /* SEQ     -> Group Settings (1/2, 2/2) */
    if (hw_b & (1u << 12)) out |= ADRIFT_BTN_PANIC;               /* PLAY    -> Panic / All Notes Off */
    /* REC (bit 13): intentionally unmapped */
    return out;
}

static void update_panel_leds(uint32_t held_notes) {
    static const uint8_t k_page_led[5] = { 3u, 4u, 5u, 6u, 7u }; /* SCL/SEL, ENV, LFO, EDIT, GLO */
    const adrift_ui_t    *ui = adrift_app_get_ui();
    const adrift_synth_t *s  = adrift_app_get_synth();
    bool arp_active = (adrift_synth_active_group(s)->patch.arp.mode != ADRIFT_ARP_OFF);
    bool fx_active  = adrift_fx_is_active(&s->fx_params);
    bool in_modal   = ui->in_preset_menu || ui->in_scale_menu || ui->in_save_menu;

    uint8_t active_btn = k_page_led[ui->page % 5u];
    for (uint32_t b = 0; b < 14u; b++) {
        if (b == 13u) {
            /* REC is inactive */
            fm1_led_key(b, 0u);
            continue;
        }
        uint8_t lit = (!in_modal && !ui->in_scope && !ui->in_group_menu && !ui->in_arp_menu && !ui->in_fx_menu && b == active_btn) ||
                      (in_modal && (b == 0u || b == 1u)) ||
                      (b == 2u && ((!in_modal && !ui->in_scope && ui->in_fx_menu) || fx_active)) ||
                      (b == 9u && (ui->in_save_menu || ui->save_toast_frames > 0u)) ||
                      (b == 10u && ((!in_modal && !ui->in_scope && ui->in_arp_menu) || arp_active)) ||
                      (b == 11u && !in_modal && !ui->in_scope && ui->in_group_menu) ||
                      (b == 8u && !in_modal && ui->in_scope) ||
                      ((fm1_in.buttons >> b) & 1u);
        fm1_led_key(b, lit);
    }
    for (uint32_t k = 0; k < 27u; k++) {
        fm1_led_key(14u + k, (held_notes >> k) & 1u);
    }
}

static void app_main(void) {
    /* Bring rails and system clock to M-VAVE stock 360 MHz before flash/audio load */
    for (uint32_t tries = 0; tries < 3u; tries++) {
        fm1_vdc14_raise(7u);
        fm1_sysvdd_raise(15u);
        if (fm1_vdc14() == 7u && fm1_sysvdd() == 15u) break;
    }
    (void)fm1_power_stock();
    (void)fm1_clock_stock();

    #define FL_JEDEC_OK(id) ((id) == 0x856014u || (id) == 0xB36014u)
    g_flash_ok = FL_JEDEC_OK(FL_FAR(fl_jedec_ram)());
    if (g_flash_ok) {
        ota_boot_cleanup();
    }

    fm1_lcd_init();
    fm1_lcd_fill_rect(0, 0, 240, 240, 0x0000u);
    fm1_input_init();
    fm1_adc_init();

    adrift_app_init();

    /* Start second core (CPU1 @ 360 MHz) before audio DMA and TIMER5 interrupts */
    fm1_cpu1_bar_build(0x41445232u);
    (void)fm1_cpu1_start();

    int audio_rc = fm1_audio_init_checked(g_abuf, 2u * HALF_WORDS, HALF_WORDS,
                                          FM1_AUDIO_RATE, isr_alnk0, 3u);
    (void)audio_rc;

    usb_start();
    fm1_uart1_midi_init(g_um_ring, UM_RING); /* Must precede TIMER5 start */
    fm1_timer5_start(isr_timer5, 4u);
    fm1_guard_lock_top();
    fm1_irq_enable_all();
    fm1_lcd_backlight(1);

    uint32_t boots_ok      = 0u;
    uint32_t last_ui_ms    = 0u;
    uint32_t last_adc_ms   = 0u;
    uint32_t prev_notes    = 0u;
    uint32_t sounding_keys = 0u;
    uint32_t prev_log_btn  = 0u;
    uint8_t  key_note[27];
    uint8_t  key_chan[27];
    int32_t  pot_smooth    = -1;

    for (uint32_t k = 0; k < 27u; k++) {
        key_note[k] = 0xFFu;
        key_chan[k] = 0u;
    }

    for (;;) {
        uint32_t now = g_ms;
        fm1_wdt_feed();
        usb_retry(now);

        if (usb.uboot_req) {
            lcd_dma_sync();
            g_boot_guard.pending = 0u;
            g_boot_guard.failed  = 0u;
            fm1_cpu1_stop();
            fm1_delay_ms(30);
            usb_detach();
            fm1_enter_uboot();
        }
        ota_service();
        if (usb.ota_req) {
            usb.ota_req = 0u;
            lcd_dma_sync();
            adrift_synth_all_notes_off(adrift_app_get_synth(), true);
            audio_silence_buf();
            fm1_cpu1_stop();
            if (g_flash_ok) {
                ota_session();
            }
            adrift_app_get_ui()->initialized = false;
        }

        if (!boots_ok && now > 2000u) {
            boots_ok             = 1u;
            g_boot_guard.pending = 0u;
            g_boot_guard.failed  = 0u;
        }

        /* Scan 27-key built-in mini-keyboard (routes to active group's MIDI channel) */
        uint32_t cur_notes = fm1_in.notes;
        if (cur_notes != prev_notes) {
            const adrift_synth_t *s_kbd = adrift_app_get_synth();
            uint8_t  ch_nib = (uint8_t)((adrift_synth_active_group(s_kbd)->midi_channel - 1u) & 0x0Fu);
            uint32_t diff   = cur_notes ^ prev_notes;
            for (uint32_t k = 0; k < 27u; k++) {
                if (diff & (1u << k)) {
                    if (cur_notes & (1u << k)) {
                        int32_t n = adrift_synth_map_kbd_key(s_kbd, (int32_t)k - 7);
                        if (n >= 0) {
                            key_note[k] = (uint8_t)n;
                            key_chan[k] = ch_nib;
                            sounding_keys |= (1u << k);
                            kbd_midi_push((uint8_t)(0x90u | ch_nib), (uint8_t)n, 100u);
                        } else {
                            key_note[k] = 0xFFu;
                            sounding_keys &= ~(1u << k);
                        }
                    } else {
                        sounding_keys &= ~(1u << k);
                        if (key_note[k] <= 127u) {
                            kbd_midi_push((uint8_t)(0x80u | key_chan[k]), key_note[k], 0u);
                            key_note[k] = 0xFFu;
                        }
                    }
                }
            }
            prev_notes = cur_notes;
        }

        /* Smooth hardware MASTER potentiometer at 25 Hz */
        if (now - last_adc_ms >= 40u) {
            last_adc_ms = now;
            int32_t raw_pot = fm1_adc_read(FM1_ADC_MASTER);
            if (raw_pot >= 0 && raw_pot <= 1023) {
                if (pot_smooth < 0) pot_smooth = raw_pot;
                else pot_smooth += (raw_pot - pot_smooth) >> 2;
            }
        }

        /* UI & LED refresh at ~60 Hz (16 ms) with dirty-band SPI blitting */
        if (now - last_ui_ms >= 16u) {
            last_ui_ms = now;

            adrift_input_state_t in_st;
            for (uint32_t e = 0; e < ADRIFT_NUM_ENC; e++) {
                int32_t d = fm1_enc_take(k_panel_enc_map[e]);
                if (d > 64) d = 64;
                if (d < -64) d = -64;
                in_st.enc_delta[e] = (int8_t)d;
            }

            uint32_t log_btn = map_hw_buttons(fm1_in.buttons);
            in_st.buttons_down     = log_btn;
            in_st.buttons_pressed  = log_btn & ~prev_log_btn;
            in_st.buttons_released = prev_log_btn & ~log_btn;
            prev_log_btn           = log_btn;
            /* In fm1-live headless bench mode, ADC reads 0 unless set: only apply pot if > 4 */
            in_st.master_pot       = (pot_smooth > 4) ? (int16_t)pot_smooth : -1;
            in_st.cpu_load_pct     = g_cpu_pct;
            in_st.text_char        = 0;
            in_st.text_cmd         = ADRIFT_TEXT_CMD_NONE;

            adrift_dirty_t dirty;
            adrift_dirty_clear(&dirty);
            adrift_app_tick_ui(&in_st, g_fb, &dirty);
            flush_dirty_lcd(&dirty);
            update_panel_leds(sounding_keys);
            (void)adrift_preset_store_flush_pending();
        }
    }
}

void fm1_cstart(void) {
    uint32_t *s, *d;
    fm1_cpu1_stop();
    fm1_time_init();
    fm1_wdt_arm(0x0Du);

    if (g_boot_guard.magic != BOOT_GUARD_MAGIC ||
        g_boot_guard.pending > 1u ||
        g_boot_guard.failed > 2u) {
        g_boot_guard.magic   = BOOT_GUARD_MAGIC;
        g_boot_guard.pending = 0u;
        g_boot_guard.failed  = 0u;
    }
    if (g_boot_guard.pending) {
        g_boot_guard.failed++;
    }
    g_boot_guard.pending = 1u;
    if (g_boot_guard.failed >= 2u) {
        g_boot_guard.pending = 0u;
        g_boot_guard.failed  = 0u;
        fm1_enter_uboot();
    }

    fm1_irq_init();
    for (d = _bss_start; d < _bss_end; d++) *d = 0u;
    for (s = _data_load, d = _data_start; d < _data_end; s++, d++) *d = *s;
    for (s = _rt_load, d = _rt_start; d < _rt_end; s++, d++) *d = *s;
    fm1_mailbox_clear();
    fm1_guard_enable(FM1_GUARD_STACK | FM1_GUARD_WRITE | FM1_GUARD_BUS | FM1_GUARD_PC);

    app_main();
    for (;;) {}
}
