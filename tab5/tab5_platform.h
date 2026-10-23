/* SPDX-License-Identifier: GPL-3.0-only
 * The interface of tab5_platform.c: what a touch UI (and the ESP-IDF audio task) calls. */
#pragma once
#include <stdint.h>

/* printed button labels (panel.c's B_* order): bit b of the mask given to tab5_buttons() */
enum { TAB5_B_FX, TAB5_B_SEL, TAB5_B_ENV, TAB5_B_LFO, TAB5_B_EDIT, TAB5_B_GLO, TAB5_B_HOME, TAB5_B_SAVE,
       TAB5_B_ARP, TAB5_B_SEQ, TAB5_B_PLAY, TAB5_B_REC, TAB5_B_OCTDN, TAB5_B_OCTUP, TAB5_NBUTTONS };
/* encoder roles (panel.c's EN_* order) */
enum { TAB5_E_SELECT, TAB5_E_ALGORITHM, TAB5_E_PRESETS, TAB5_E_K1, TAB5_E_K2, TAB5_E_K3, TAB5_E_K4, TAB5_NENC };
#define TAB5_NKEYS 27                                   /* key k = k semitones above F3 (F3 .. G5) */

void tab5_power_on(void);                               /* boot: settings, splash, project resume */
void tab5_step_ms(void);                                /* the device advances 1 ms (clock, audio, main loop) */
void tab5_audio_read(int32_t *dst, uint32_t n);         /* n stereo frames, 24 bit left-justified int32 */

void tab5_test_pitched_track(void);                       /* PC tests: track 1 = a held, pitched voice */
int32_t tab5_enc_total(uint32_t role);                    /* PC tests: every encoder step given so far */
uint32_t tab5_held_buttons(void);                         /* PC tests: the buttons the firmware sees held (bit per TAB5_B_*) */
uint32_t tab5_held_keys(void);
void tab5_buttons(uint32_t labels);                     /* held buttons, bit per TAB5_B_* */
void tab5_keys(uint32_t mask);                          /* held keys, bit per key 0..26 */
void tab5_enc(uint32_t role, int32_t steps);            /* + = clockwise */
void tab5_master(uint32_t v);                           /* MASTER, 0..1023 */
void tab5_midi_in(uint32_t pkt);                        /* a USB-MIDI event packet (cable/CIN, status, d1, d2); any task */

uint32_t tab5_lit_buttons(void);                        /* the LEDs: lit / glow / backlight, bit per control */
uint32_t tab5_lit_keys(void);
uint32_t tab5_glow_buttons(void);
uint32_t tab5_glow_keys(void);
uint32_t tab5_back_buttons(void);
uint32_t tab5_back_keys(void);
uint32_t tab5_backlight_ns(void);

const uint16_t *tab5_screen(void);                      /* 240 x 240 RGB565, high byte first */
uint32_t tab5_screen_draws(void);                       /* changes whenever the screen was drawn */
uint32_t tab5_now_ms(void);
uint32_t tab5_playing(void);
uint32_t tab5_sample_rate(void);
uint32_t tab5_half_frames(void);
uint32_t tab5_cpu_q8(void);
uint32_t tab5_pattern_steps(void);                      /* programmed steps on the synth tracks (test aid) */
void tab5_set_nor(uint8_t *mem);                        /* give the 1 MiB flash image (0xFF = erased) BEFORE tab5_power_on */
uint8_t *tab5_nor_image(void);                          /* the same image */
int32_t tab5_nor_take_dirty(void);                      /* index of a 4 KB sector written since last call, or -1 */
uint32_t tab5_nor_size(void);
uint32_t tab5_flash_write_count(void);
extern uint32_t (*tab5_ticks_hook)(void);               /* set a real timer here for the CPU load meter */

/* the firmware's pixel font (capitals) for labels: width of a string, and draw it into a native RGB565 buffer */
int tab5_text_width(const char *s, int big);            /* big: 0 = 16 px high, 1 = 32 px */
int tab5_text_draw(uint16_t *dst, int dw, int dh, int x, int y, const char *s, uint16_t color, int big);
