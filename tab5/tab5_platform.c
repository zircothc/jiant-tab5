/* SPDX-License-Identifier: GPL-3.0-only
 *
 * JIANT FM (a Felucca fork for the M-Vave FM-1) on a touch-screen host: platform layer for the M5Stack Tab5.
 *
 * The firmware (JIANT FM (C) 2026 Juan Ignacio Antonucci, based on FELUCCA (C) 2026 Leo Kuroshita / Hügelton
 * Instruments, GPL-3.0) is compiled as ONE unit exactly like src/felucca.c does, but every register-level HAL
 * header is replaced by the small doubles below. The design follows the browser build of JIANT / FELUCCA
 * (web/emu/felucca_web.c, itself after charlesvestal/fm1-x0x): one thread, the device clock driven by the audio
 * that is asked for. Nothing in firmware/src is modified.
 *
 *   - clock    : fm1_ms / TIMER4 ticks advance 1 ms at a time (tab5_step_ms)
 *   - audio    : the real audio ISR (audio.c fm1_alnk0_irq) renders each I2S half when it is due
 *   - panel    : buttons / keys as bit masks, encoders as step counters, MASTER as a 0..1023 "ADC"
 *   - LEDs     : fm1_led / fm1_led_dim / fm1_led_breath / fm1_led_mid read back through the key matrix
 *   - screen   : lcd_fill / lcd_blit into a 240 x 240 RGB565 frame (the panel's byte order)
 *   - flash    : a 1 MiB NOR image behind storage.c's hooks (settings, projects, user presets)
 *   - not here : USB, TRS MIDI, the editor, OTA, console, second core, crash / UBOOT paths
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>

#if defined(TAB5_HOST) || defined(TAB5_STRIP_ATTR)
#define __attribute__(x)                         /* the JieLi section / alignment attributes mean nothing here */
#endif

/* ---- the build configuration (what src/felucca.c defaults to, minus the FM-1's USB, UART, OTA, second core) ---- */
#define FELUCCA_WEB 1
#define FELUCCA_FLASH 1
#define FELUCCA_OTA 0
#define FELUCCA_OTA_DRYRUN 0
#define FELUCCA_CDC 1                            /* MENU > USB SERIAL is there (nothing behind it) */
#define FELUCCA_UAC 0
#define FELUCCA_UART 0
#define FELUCCA_DUAL_CORE 1                      /* the second core: a helper thread the app provides (tab5_mc_run_hook), else one core */
#define FELUCCA_HOTRAM 0
#ifndef FELUCCA_ID
#define FELUCCA_ID "TAB5"
#endif
#ifndef FELUCCA_VERSION
#define FELUCCA_VERSION "TAB5"
#endif

#define FM1_TICKS_PER_US 24u                     /* TIMER4: 24 MHz, as the firmware's time arithmetic assumes */
#define FM1_NCOL 11u                             /* the key matrix is how LEDs are addressed (fm1_input.h) */
#define FM1_NKEY 41u
#define FM1_NENC 7u

/* the SPI NOR (1 MiB, erased = 0xFF). The caller owns the memory and hands it over with tab5_set_nor() before
 * tab5_power_on(). */
#define TAB5_NOR_SIZE 0x100000u
static uint8_t *tab5_nor;
static uint8_t tab5_nor_dirty_map[TAB5_NOR_SIZE / 4096u / 8u];       /* one bit per 4 KB sector written since taken */

/* ------------------------------------------------- libc (tests/hostsim.c's prelude) --- */
#define memset felucca_memset                    /* the firmware's own mem* (libc.c) must not clash with the PC's */
#define memcpy felucca_memcpy
#define memcmp felucca_memcmp
#include "felucca_tables.h"
#include "libc.c"
#undef memset
#undef memcpy
#undef memcmp

static struct { volatile uint32_t notes, buttons; } fm1_in;      /* bit per note key 0..26 / button 0..13 */

/* hal/fm1_multicore.h's double: "core 1" is a helper thread of the app, handed a job with tab5_mc_run_hook and waited for with
 * tab5_mc_wait_hook. Without them (the host tests) the firmware renders every part itself, as on one core. */
int (*tab5_mc_run_hook)(void (*fn)(uint32_t), uint32_t arg);
int (*tab5_mc_wait_hook)(void);                  /* 0 = done, -1 = the helper did not answer */
static uint8_t fm1_mc_on, fm1_mc_lost;
static uint32_t fm1_mc_max_ticks;
typedef struct { volatile uint32_t job, done; } fm1_mc_box_t;
static fm1_mc_box_t fm1_mc_box;
#define FM1_MC_BAR_MAGIC 0x3152414Au
#define FM1_MC_CLK_ARMED (1u << 16)
#define FM1_MC_CLK_TRIP (2u << 16)
typedef struct { uint32_t magic, barred; } fm1_mc_bar_t;
static fm1_mc_bar_t fm1_mc_bar;
typedef struct { uint32_t count; } fm1_core1_fault_t;
static fm1_core1_fault_t fm1_core1_fault;
static uint32_t fm1_ticks(void);
static uint32_t fm1_mc_ticks(void) { return fm1_ticks(); }
uint32_t (*tab5_cnum_hook)(void);                /* which core the caller is (0 = the audio core, 1 = the helper): the firmware keeps per-core scratch */
static uint32_t fm1_cnum(void) { return tab5_cnum_hook ? tab5_cnum_hook() : 0u; }
typedef struct { uint32_t pll2_con1, sys_div, sfc_baud, sfc_con, cache_b9; } fm1_clk320_t;   /* the 320 MHz switch: never done here */
static int fm1_clk320_check(fm1_clk320_t *p) { (void)p; return 3; }
static void fm1_clk320_switch(const fm1_clk320_t *p) { (void)p; }
static int fm1_clk_320(void) { return 0; }
static int fm1_mc_barred(void) { return 0; }
static void fm1_mc_bar_set(void) {}
static void fm1_mc_hold(void) { fm1_mc_on = 0; }
static int fm1_mc_start(void) { fm1_mc_on = tab5_mc_run_hook != 0 && tab5_mc_wait_hook != 0; return fm1_mc_on; }
static void fm1_mc_park(void) {}
static int fm1_mc_run(void (*fn)(uint32_t), uint32_t arg)
{
    if (!fm1_mc_on || !tab5_mc_run_hook)
        return 0;
    fm1_mc_box.job++;
    return tab5_mc_run_hook(fn, arg);
}
static int fm1_mc_done(uint32_t job) { return (int32_t)(fm1_mc_box.done - job) >= 0; }
static int fm1_mc_wait(void)
{
    int r = tab5_mc_wait_hook ? tab5_mc_wait_hook() : 0;
    if (r) {
        fm1_mc_lost = 2u;
        fm1_mc_hold();
        return -1;
    }
    fm1_mc_box.done = fm1_mc_box.job;
    return 0;
}
#include "core.h"
#include "engines.c"
#include "params.c"
#include "mod.c"
#include "voice.c"
#include "slicer.c"
#include "fx.c"
static void fm1_delay_ms(uint32_t ms) { (void)ms; }       /* no busy waits: the host never blocks */
static void fm1_delay_us(uint32_t us) { (void)us; }
#include "usb.c"
#include "midi_uart.c"                           /* its parser feeds midi_in_q */
#include "song_chain.c"
#include "seq.c"

/* ------------------------------------------------------------------ time --- */
static uint32_t tab5_ticks;
uint32_t (*tab5_ticks_hook)(void);               /* NULL: the deterministic device clock (tests, goldens) */
static uint32_t fm1_ticks(void) { return tab5_ticks_hook ? tab5_ticks_hook() : tab5_ticks; }
static uint32_t fm1_micros(void) { return fm1_ticks() / FM1_TICKS_PER_US; }

/* ---------------------------------------------------------------- audio --- */
static uint32_t tab5_half;                       /* the half the next render fills */
#define FM1_AUDIO_HALF 1u
static uint8_t fm1_audio_pending(void) { return FM1_AUDIO_HALF; }
static void fm1_audio_ack_aux(uint8_t p) { (void)p; }
static uint32_t fm1_audio_free_half(void) { return tab5_half; }
static void fm1_audio_ack_half(void) {}
static void fm1_audio_init(int32_t *buf, uint32_t words, void (*isr)(void), uint32_t prio)
{ (void)buf; (void)words; (void)isr; (void)prio; }
static void fm1_audio_stop(void) {}
void isr_alnk0(void) {}
#include "audio.c"

/* ---------------------------------------------------------------- panel --- */
static const int8_t FM1_KEYMAP[6][FM1_NCOL] = {  /* key id at (row bit, column), as hal/fm1_input.h */
    {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    { 5, 11,  4, 10,  3,  9,  2,  8, -1, -1, -1},
    {34, 35, 36, 37, 38, 40, 39, 13,  7,  6, 12},
    {23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33},
    { 0,  1, 15, 14, 17, 16, 19, 18, 20, 21, 22},
    {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
};
static uint8_t fm1_led[FM1_NCOL], fm1_led_dim[FM1_NCOL], fm1_led_breath[FM1_NCOL], fm1_led_mid[FM1_NCOL], fm1_led_hi[FM1_NCOL];
static uint32_t tab5_dim_lo;
static void fm1_led_dim_level(uint32_t lo) { tab5_dim_lo = lo; }
static void fm1_led_anim_start(uint32_t glow) { (void)glow; }    /* no power-on LED sweep on a touch screen */
static int fm1_led_anim_on(void) { return 0; }
static uint32_t tab5_pressed, tab5_released, tab5_notes_pressed;
static int32_t tab5_enc_steps[FM1_NENC];
static uint32_t tab5_master_adc = 700;           /* MASTER, 0..1023 as the ADC reads it */
enum { FM1_ADC_BATT = 3, FM1_ADC_MASTER = 4 };
/* the touch task writes these, the device (audio task) reads and clears them: atomic, so a press that lands
 * between the read and the clear is not lost */
static uint32_t fm1_input_edges(uint32_t *released)
{
    uint32_t p = __atomic_exchange_n(&tab5_pressed, 0u, __ATOMIC_ACQ_REL);
    uint32_t r = __atomic_exchange_n(&tab5_released, 0u, __ATOMIC_ACQ_REL);
    if (released)
        *released = r;
    return p;
}
static uint32_t fm1_input_note_edges(void) { return __atomic_exchange_n(&tab5_notes_pressed, 0u, __ATOMIC_ACQ_REL); }
static int32_t fm1_enc_take(uint32_t e) { return __atomic_exchange_n(&tab5_enc_steps[e % FM1_NENC], 0, __ATOMIC_ACQ_REL); }
static void fm1_input_init(void) {}
static void fm1_input_tick(void) {}
static int32_t fm1_adc_read(uint32_t ch) { return ch == FM1_ADC_MASTER ? (int32_t)tab5_master_adc : 620; }
static void fm1_adc_init(void) {}
static void fm1_wdt_feed(void) {}
static void fm1_irq_off(void) {}                 /* one thread: nothing to mask */
static void fm1_irq_on(void) {}

/* ----------------------------------------------------------- the screen --- */
static uint16_t tab5_fb[240 * 240];              /* RGB565 as the panel gets it (high byte first) */
static uint32_t tab5_draws;                      /* bumped on every draw: repaint the scaled copy when it moves */
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    uint16_t sw = (uint16_t)((c >> 8) | (c << 8));
    for (j = 0; j < h && y + j < 240u; j++)
        for (i = 0; i < w && x + i < 240u; i++)
            tab5_fb[(y + j) * 240u + x + i] = sw;
    tab5_draws++;
}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *p)
{
    uint32_t i, j;
    for (j = 0; j < h && y + j < 240u; j++)
        for (i = 0; i < w && x + i < 240u; i++)
            tab5_fb[(y + j) * 240u + x + i] = p[j * w + i];
    tab5_draws++;
}
static void lcd_sync(void) {}
static void lcd_power(uint32_t s)                /* MENU > SCREEN OFF (lcd.c): off, the page dark; on: redrawn before */
{
    if (!s) {
        memset(tab5_fb, 0, sizeof tab5_fb);
        tab5_draws++;
    }
}
static void lcd_wake_now(void) {}
static void lcd_init(void) {}

#include "gfx.c"
#include "panel.c"
#include "ui.c"
#include "menu_items.c"
#include "icons.c"
#include "ui_graph.c"
#include "ui_draw.c"
#include "ui_menu.c"
#include "ui_input.c"
#include "ui_layer.c"

/* ---------------------------------------------------------------- flash --- */
static uint32_t tab5_flash_writes;
static uint8_t flash_ok;
static int st_read(uint32_t off, void *dst, uint32_t n)
{
    if (off >= TAB5_NOR_SIZE || n > TAB5_NOR_SIZE - off)
        return -1;
    memcpy(dst, tab5_nor + off, n);
    return 0;
}
static int st_erase(uint32_t off)
{
    if (off >= TAB5_NOR_SIZE || 4096u > TAB5_NOR_SIZE - off)
        return -8;
    memset(tab5_nor + (off & ~4095u), 0xFF, 4096);
    tab5_nor_dirty_map[(off >> 12) >> 3] |= (uint8_t)(1u << ((off >> 12) & 7u));
    tab5_flash_writes++;
    return 0;
}
static int st_prog(uint32_t off, const void *src, uint32_t n)     /* NOR: a write only clears bits */
{
    const uint8_t *s = src;
    uint32_t i;
    if (off >= TAB5_NOR_SIZE || n > TAB5_NOR_SIZE - off)
        return -8;
    for (i = 0; i < n; i++)
        tab5_nor[off + i] &= s[i];
    for (i = off >> 12; i <= (off + (n ? n - 1u : 0u)) >> 12; i++)
        tab5_nor_dirty_map[i >> 3] |= (uint8_t)(1u << (i & 7u));
    tab5_flash_writes++;
    return 0;
}
static uint32_t irq_save(void) { return 0; }
static void irq_restore(uint32_t f) { (void)f; }
static uint32_t fl_jedec_ram(void) { return 0x856014u; }          /* the FM-1's part (project.c) */
static void fl_plain_window_init(void) {}
#define FL_FAR(fn) (fn)
#include "storage.c"
#include "upreset.c"
#include "project.c"

/* ------------------------------------------------- main.c and its HAL --- */
typedef struct { uint32_t magic, count, vec, pc, emu, dbg, rets; } fm1_crash_t;
#define FM1_CRASH_MAGIC 0x43525348u
static fm1_crash_t fm1_crash;                    /* (none: main.c boot_leds runs the sweep, which is empty here) */
static struct { uint8_t p3_rst, wdt_con; uint32_t rst_src; } fm1_boot;
uint32_t _data_start[1], _data_end[1], _data_load[1], _bss_start[1], _bss_end[1];
uint32_t _pool_start[1], _pool_end[1], _rt_start[1], _rt_end[1], _rt_load[1];
/* the RAM code and the low buffers (app.ld): none here; each start the same array as its end, so main.c's copy and
 * clear loops do nothing */
#define _hot_start _hot_end
#define _low_start _low_end
uint32_t _hot_end[1], _hot_load[1], _low_end[1];
#define _c1t_start _c1t_end
uint32_t _c1t_end[1], _c1t_load[1];
static void fm1_timer5_ack(void) {}
static void fm1_timer5_start(void (*isr)(void), uint32_t prio) { (void)isr; (void)prio; }
void isr_timer5(void) {}
static void fm1_reboot(void) {}
static void fm1_core1_stop(void) {}
static void fm1_enter_uboot(void) {}
static void fm1_guard_lock_top(void) {}
static void fm1_guard_unlock_top(void) {}
static void fm1_irq_enable_all(void) {}
static void fm1_irq_init(void) {}
static void fm1_time_init(void) {}
static void fm1_reset_reason(void) {}
static void fm1_wdt_arm(uint32_t v) { (void)v; }
static void fm1_mailbox_clear(void) {}
static void fm1_guard_enable(uint32_t m) { (void)m; }
enum { FM1_GUARD_STACK = 1, FM1_GUARD_WRITE = 2, FM1_GUARD_BUS = 4, FM1_GUARD_PC = 8 };
#if FELUCCA_CDC
static void cdc_task(void) {}
#endif
/* main.c's calls into the USB driver (registers): nothing here (usb.c's own functions stay as they are) */
#define usb_poll() ((void)0)
#define usb_start() ((void)0)
#define usb_retry(ms) ((void)(ms))
#define usb_detach() ((void)0)
#include "master.c"
#include "main.c"
#undef usb_poll
#undef usb_start
#undef usb_retry
#undef usb_detach

/* ================================================================ the device === */
static uint32_t tab5_booted, tab5_boot_ms, tab5_last_frame;
static uint32_t tab5_samples_due;
#define TAB5_RING 8192u
static int32_t tab5_ring[TAB5_RING * 2];         /* stereo, 24 bit left-justified (the codec's format) */
static uint32_t tab5_ring_w, tab5_ring_r;
static uint32_t tab5_seed = 1u;

/* main.c fm1_main(), from the top down to its loop (no USB, UART, guards, second core, panel setup) */
void tab5_power_on(void)
{
    persist_boot();
    settings_init();
    prefs_update();                              /* FX LATCH ON by default, once */
    usb_serial_apply();
    lcd_init();
    spl_anim = 1;                                /* the signature written */
    draw_splash();
    memset(&felucca_dbg, 0, sizeof felucca_dbg);
    felucca_dbg.magic = DBG_MAGIC;
    felucca_dbg.boots++;
    fm1_input_init();
    fm1_adc_init();
    panel_init();
    felucca_init();
    master_boot();                               /* the pot's level from the first block */
    kb_boot_hold = 1;                            /* the keys silent under the splash */
    if (!(cpu_cfg & CPU_1CORE))                  /* MENU > CPU 1 CORE: the helper thread stays unused */
        fm1_mc_start();                          /* (a no-op without the app's hooks) */
    audio_init();
    boot_leds();
    autosave_boot(boot_clean);                   /* the last session's music */
    session_dice(tab5_seed);
    tab5_boot_ms = fm1_ms + 30u + SPL_WRITE + SPL_HOLD;   /* main.c: 30 ms + the splash before the first frame */
    tab5_booted = 1;
}

/* one pass of main.c's loop body (what a device without USB, editor or UBOOT does) */
static void tab5_frame(void)
{
    int32_t b = fm1_adc_read(FM1_ADC_BATT);
    if (b > 0)
        song.batt_raw = song.batt_raw ? song.batt_raw + (b - song.batt_raw) / 32 : b;
    master_poll();
    felucca_dbg.ui_frames++;
    felucca_dbg.page = ui.page;
    felucca_dbg.home = ui.home;
    ui_input();
    settings_poll();
    autosave_poll();
    mutate_poll();
    song_poll();
    ui_leds();
    ui_draw();
}

/* USB-MIDI event packets (4 bytes: cable/CIN, status, d1, d2, as a little-endian uint32) from another task or core.
 * One producer (the input task); the device drains the ring on ITS thread, so the firmware's own MIDI ring keeps
 * its single producer and single consumer. */
#define T5_MQ 256u
static uint32_t t5_midi_q[T5_MQ];
static uint32_t t5_midi_w, t5_midi_r;
void tab5_midi_in(uint32_t pkt)
{
    uint32_t w = __atomic_load_n(&t5_midi_w, __ATOMIC_RELAXED);
    if (w - __atomic_load_n(&t5_midi_r, __ATOMIC_ACQUIRE) >= T5_MQ)
        return;                                                  /* full: drop (the firmware drops too) */
    t5_midi_q[w % T5_MQ] = pkt;
    __atomic_store_n(&t5_midi_w, w + 1u, __ATOMIC_RELEASE);
}
static void t5_midi_drain(void)
{
    uint32_t r = t5_midi_r;
    while (r != __atomic_load_n(&t5_midi_w, __ATOMIC_ACQUIRE)) {
        midi_in_event(t5_midi_q[r % T5_MQ]);
        r++;
        __atomic_store_n(&t5_midi_r, r, __ATOMIC_RELEASE);
    }
}

#ifndef T5_FRAME_MS
#define T5_FRAME_MS 40u                                         /* JIANT's own loop is ~15 ms; the Tab5's 360 MHz core needs half the drawing */
#endif

/* On the Tab5 the device can run as TWO threads, the way the FM-1 does: the audio render (the "ISR", core 1) and the main loop
 * (ui_input, the screen: core 0). The app sets tab5_split = 1 for that; the host tests keep the single deterministic thread. */
int tab5_split;
static uint32_t frame_ms(void) { return tab5_split ? 20u : T5_FRAME_MS; }

/* the clock and the audio halves due by now (one millisecond of the device) */
static void tab5_step_audio_ms(void)
{
    fm1_ms++;
    tab5_ticks += 1000u * FM1_TICKS_PER_US;
    tab5_samples_due += FS;
    while (tab5_samples_due >= 1000u * HALF_FRAMES) {          /* an I2S half is due: the audio ISR renders it */
        uint32_t k;
        const int32_t *o = &abuf[tab5_half * HALF_WORDS];
        tab5_samples_due -= 1000u * HALF_FRAMES;
        fm1_alnk0_irq();
        for (k = 0; k < HALF_FRAMES; k++) {
            uint32_t i = (tab5_ring_w++ % TAB5_RING) * 2u;
            tab5_ring[i] = o[2u * k];
            tab5_ring[i + 1] = o[2u * k + 1u];
        }
        tab5_half ^= 1u;
    }
}

/* the main loop: MIDI in, the splash, ui_input and a frame every frame_ms() */
const char *tab5_title(void) { return "JIANT 0.9.18"; }     /* name and version, shown above the function buttons */
int tab5_cores(void) { return fm1_mc_on ? 2 : 1; }   /* the firmware renders on this many threads */

void tab5_step_ui(void)
{
    static uint32_t last_splash;
    t5_midi_drain();
    if (tab5_booted && (int32_t)(fm1_ms - tab5_boot_ms) < 0 && (fm1_ms - last_splash >= 20u)) {   /* the signature written */
        last_splash = fm1_ms;
        splash_write(SPL_WRITE + SPL_HOLD - (tab5_boot_ms - fm1_ms));
    }
    if (!tab5_booted || (int32_t)(fm1_ms - tab5_boot_ms) < 0)
        return;
    if (tab5_boot_ms) {                                         /* the splash goes, the loop starts */
        tab5_boot_ms = 0;
        kb_boot_hold = 0;
        lcd_fill(0, 0, 240, 240, T_BG);
        tab5_last_frame = fm1_ms - frame_ms();
    }
    if (fm1_ms - tab5_last_frame >= frame_ms()) {               /* a frame, then ui_input until the period is over */
        tab5_last_frame = fm1_ms;
        tab5_frame();
    } else {
        ui_input();
    }
}

/* ONE millisecond of the device, single thread (the host tests) */
void tab5_step_ms(void)
{
    tab5_step_audio_ms();
    tab5_step_ui();
}

/* n stereo frames (24 bit left-justified int32, L R L R ...): the device runs until they exist */
void tab5_audio_read(int32_t *dst, uint32_t n)
{
    uint32_t k;
    while (tab5_ring_w - tab5_ring_r < n) {
        if (tab5_split)
            tab5_step_audio_ms();
        else
            tab5_step_ms();
    }
    for (k = 0; k < n; k++) {
        uint32_t i = (tab5_ring_r++ % TAB5_RING) * 2u;
        dst[2u * k] = tab5_ring[i];
        dst[2u * k + 1u] = tab5_ring[i + 1u];
    }
}

/* ------------------------------------------------------------- the panel --- */
/* buttons by their printed labels: bit b = B_FX .. B_OCTUP (panel.c); keys: bit k = key k, F3 .. G5 */
void tab5_buttons(uint32_t labels)
{
    uint32_t m = 0, b, old = fm1_in.buttons;
    for (b = 0; b < NB; b++)
        if ((labels >> b) & 1u)
            m |= 1u << panel.btn[b];
    __atomic_fetch_or(&tab5_pressed, m & ~old, __ATOMIC_ACQ_REL);
    __atomic_fetch_or(&tab5_released, old & ~m, __ATOMIC_ACQ_REL);
    fm1_in.buttons = m;
}
void tab5_keys(uint32_t mask)
{
    uint32_t old = fm1_in.notes;
    mask &= (1u << 27) - 1u;
    __atomic_fetch_or(&tab5_notes_pressed, mask & ~old, __ATOMIC_ACQ_REL);
    fm1_in.notes = mask;
}
static int32_t tab5_enc_sum[NE];                                 /* every step ever given (the tests read it) */
int32_t tab5_enc_total(uint32_t role) { return role < NE ? tab5_enc_sum[role] : 0; }
uint32_t tab5_held_buttons(void) { uint32_t b, m = 0; for (b = 0; b < NB; b++) m |= ((fm1_in.buttons >> panel.btn[b]) & 1u) << b; return m; }
uint32_t tab5_held_keys(void) { return fm1_in.notes; }
void tab5_enc(uint32_t role, int32_t n)                         /* role: EN_SELECT .. EN_K4; + = clockwise */
{
    if (role < NE)
        tab5_enc_sum[role] += n;
    if (role < NE)
        __atomic_fetch_add(&tab5_enc_steps[panel.enc[role] % FM1_NENC], n * panel.dir[role], __ATOMIC_ACQ_REL);
}
void tab5_master(uint32_t v) { tab5_master_adc = v > 1023u ? 1023u : v; }

/* the LEDs, one bit per control: buttons bit b = the button labelled b, keys bit k = key k */
static uint32_t tab5_led_on(const uint8_t *l, uint32_t id)
{
    uint32_t c, r;
    for (c = 0; c < FM1_NCOL; c++)
        for (r = 1; r < 5u; r++)
            if (FM1_KEYMAP[r][c] == (int8_t)id)
                return (l[c] >> r) & 1u;
    return 0;
}
static uint32_t tab5_led_buttons(const uint8_t *l)
{
    uint32_t b, m = 0;
    for (b = 0; b < NB; b++)
        m |= tab5_led_on(l, panel.btn[b]) << b;
    return m;
}
static uint32_t tab5_led_keys(const uint8_t *l)
{
    uint32_t k, m = 0;
    for (k = 0; k < 27u; k++)
        m |= tab5_led_on(l, 14u + k) << k;
    return m;
}
uint32_t tab5_lit_buttons(void) { return tab5_led_buttons(fm1_led); }
uint32_t tab5_lit_keys(void) { return tab5_led_keys(fm1_led); }
uint32_t tab5_glow_buttons(void) { return tab5_led_buttons(fm1_led_dim); }
uint32_t tab5_glow_keys(void) { return tab5_led_keys(fm1_led_dim); }
/* JIANT's "breathing" and steady-mid LEDs stand in for the old background level (panel_ui draws it as a soft tint) */
uint32_t tab5_back_buttons(void) { return tab5_led_buttons(fm1_led_breath) | tab5_led_buttons(fm1_led_mid); }
uint32_t tab5_back_keys(void) { return tab5_led_keys(fm1_led_breath) | tab5_led_keys(fm1_led_mid); }
uint32_t tab5_backlight_ns(void) { return 0; }

/* ------------------------------------------------------------- the rest --- */
const uint16_t *tab5_screen(void) { return tab5_fb; }           /* 240 x 240 RGB565, byte-swapped (high byte first) */
uint32_t tab5_screen_draws(void) { return tab5_draws; }
uint32_t tab5_now_ms(void) { return fm1_ms; }
uint32_t tab5_playing(void) { return song.playing; }
uint32_t tab5_sample_rate(void) { return FS; }
uint32_t tab5_half_frames(void) { return HALF_FRAMES; }
uint32_t tab5_cpu_q8(void) { return song.cpu_q8; }
void tab5_set_nor(uint8_t *mem) { tab5_nor = mem; }
uint8_t *tab5_nor_image(void) { return tab5_nor; }
uint32_t tab5_nor_size(void) { return TAB5_NOR_SIZE; }
void tab5_seed_set(uint32_t s) { tab5_seed = s; }
/* the next 4 KB sector written since the last call (its index), or -1: persist these to the flash partition */
int32_t tab5_nor_take_dirty(void)
{
    uint32_t i;
    for (i = 0; i < TAB5_NOR_SIZE / 4096u; i++)
        if (tab5_nor_dirty_map[i >> 3] & (1u << (i & 7u))) {
            tab5_nor_dirty_map[i >> 3] &= (uint8_t)~(1u << (i & 7u));
            return (int32_t)i;
        }
    return -1;
}
uint32_t tab5_flash_write_count(void) { return tab5_flash_writes; }

/* for the PC tests: track 1 (power-on: DRUM-X, which decays at once) becomes a held, pitched ANALOG voice, so the
 * tests can hold a key and hear it (as hostsim's host_preset does) */
void tab5_test_pitched_track(void)
{
    static const uint8_t FX_DEF[4] = {0, 24, 28, 36};
    track_t *t = &trk[0];
    const preset_t *p = &ENGINES[ENGI_ANALOG]->presets[0];
    uint32_t i;
    t->eng_req = ENGI_ANALOG;
    t->engine = ENGI_ANALOG;
    t->preset = 0;
    for (i = 0; i < 8u; i++)
        t->p[P_E0 + i] = p->e[i];
    t->p[P_ATK] = p->env[0];
    t->p[P_DEC] = p->env[1];
    t->p[P_SUS] = 127;
    t->p[P_REL] = p->env[3];
    t->p[P_VOICE] = V_POLY;
    for (i = 0; i < 4u; i++)
        t->p[P_DIST + i] = (int16_t)(p->fx[i] ? p->fx[i] - 1 : FX_DEF[i]);
    ui.force = 1;
}

/* a count of the programmed steps on the synth tracks (the persistence test reads it) */
uint32_t tab5_pattern_steps(void)
{
    uint32_t k, i, n = 0;
    for (k = 0; k < NPART; k++)
        for (i = 0; i < NSTEP; i++)
            n += trk[k].step[i].n > 0;
    return n;
}

/* ---- text with the firmware's own 5 x 7 pixel face (AF_X), for a touch UI to label its controls ------------------
 * big = 0: 2x (14 px high), 1: 4x (28 px). Only constant tables are read, so it is safe from another task than the
 * device's. Draws into the caller's NATIVE-endian RGB565 buffer. */
int tab5_text_width(const char *s, int big)
{
    int32_t pen = 0;
    for (; *s; s++)
        pen += AF_X.gh[glyph(&AF_X, fold(&AF_X, (uint8_t)*s)) >> AF_X.psh].adv;
    return (pen * (big ? 4 : 2) + 8) >> 4;
}

int tab5_text_draw(uint16_t *dst, int dw, int dh, int x, int y, const char *s, uint16_t color, int big)
{
    const int sc = big ? 4 : 2;
    for (; *s; s++) {
        aag_t g = ag(&AF_X, glyph(&AF_X, fold(&AF_X, (uint8_t)*s)));
        const uint8_t *d = AF_X.data + g.off;
        uint32_t gx, gy;
        int a, b;
        for (gy = 0; gy < g.bh; gy++)
            for (gx = 0; gx < g.bw; gx++) {
                uint32_t bit = gy * g.bw + gx;
                if (!((d[bit >> 3] >> (7u - (bit & 7u))) & 1u))
                    continue;
                for (b = 0; b < sc; b++)
                    for (a = 0; a < sc; a++) {
                        int px = x + ((int)g.bx + (int)gx) * sc + a, py = y + ((int)g.by + (int)gy) * sc + b;
                        if (px >= 0 && py >= 0 && px < dw && py < dh)
                            dst[py * dw + px] = color;
                    }
            }
        x += (g.adv * sc + 8) >> 4;
    }
    return x;
}
