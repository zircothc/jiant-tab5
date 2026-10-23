/* SPDX-License-Identifier: GPL-3.0-only
 * The FM-1's whole panel on a 1280 x 720 touch screen: the firmware's 240 x 240 screen doubled, 27 keys, 14 buttons,
 * 8 knobs, their LEDs. Portable C: it only needs tab5_platform.h and a function that puts a rectangle on the display
 * (so the same code runs on the Tab5 and, for testing, on a PC). Landscape coordinates throughout. */
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct { int16_t x, y; uint8_t id; } panel_pt;            /* a finger: id stays the same while it is down */
typedef void (*panel_blit_fn)(int x, int y, int w, int h, const uint16_t *px);   /* native-endian RGB565, contiguous */

/* scratch: room for 480 x 480 pixels (the screen); blit: where rectangles go */
void panel_init(uint16_t *scratch, size_t scratch_px, panel_blit_fn blit);
/* the fingers now down (up to 5), called every few ms: drives the firmware (keys, buttons, knobs, MASTER) */
void panel_touch(const panel_pt *pts, int n, uint32_t now_ms);
/* draws what changed since the last call (screen, LEDs, pressed states); ~30 Hz */
void panel_render(void);

uint32_t panel_latched_buttons(void);                             /* bit per button label: locked down (test aid) */

/* ---- MIDI --------------------------------------------------------------------------------------------------------
 * panel_midi_in: one USB-MIDI event packet (cable/CIN, status, d1, d2). Mapped notes / CCs drive the panel's controls;
 * the rest goes on to the firmware (tab5_midi_in). Call it from the SAME task that calls panel_touch. */
void panel_midi_in(uint32_t pkt);
void panel_midi_set_devices(int n);                               /* how many MIDI devices are plugged in (shown while learning) */
int panel_midi_learning(void);                                    /* learn mode is on */
int panel_midi_mapped_count(void);
/* the map is 4 + 49 x 4 bytes: persist it wherever suits (the Tab5 uses NVS) */
void panel_midi_map_export(uint8_t *blob, size_t *len);           /* blob: at least 200 bytes */
int panel_midi_map_import(const uint8_t *blob, size_t len);       /* 0 = ok */
void panel_midi_map_set_save_cb(void (*cb)(const uint8_t *blob, size_t len));   /* called after every change */

/* the learn page has a TONE button for checking the speaker: the app plays a test tone when it is pressed */
void panel_set_tone_cb(void (*cb)(void));
/* the output level (software gain, 0..8) and what is plugged in: shown on the learn page, with LEVEL - / + buttons */
void panel_set_level_cb(void (*cb)(int delta));
void panel_set_audio_info(int level, int headphones);
