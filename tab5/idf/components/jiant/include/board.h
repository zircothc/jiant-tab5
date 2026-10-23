/* SPDX-License-Identifier: GPL-3.0-only
 * What the app needs from the hardware. Two implementations: boards/null (headless) and boards/bsp (Tab5). */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#define BOARD_UI_W 1280                 /* the UI is landscape; the panel is 720 x 1280 portrait, board.c rotates */
#define BOARD_UI_H 720

typedef struct { int16_t x, y; uint8_t id; } board_touch_t;      /* landscape coordinates */

esp_err_t board_init(void);
uint32_t board_audio_rate(void);                                  /* Hz the codec runs at */
void board_audio_write(const int32_t *lr24, size_t frames);       /* stereo, 24 bit left-justified; blocks in real time */
void board_display_blit(int x, int y, int w, int h, const uint16_t *rgb565);   /* landscape rect, native-endian RGB565 */
int board_touch_read(board_touch_t *pts, int max);                /* contacts now; 0 = none */

/* USB MIDI host (keyboards, pads, controllers). The callback gets raw 4-byte USB-MIDI event packets from a USB task. */
typedef void (*board_midi_cb)(uint32_t pkt);
esp_err_t board_midi_start(board_midi_cb cb);
int board_midi_devices(void);                                     /* MIDI devices plugged in now */
void board_audio_enable(void);                                    /* switch the speaker amplifier on, unmute, set the volume again */

/* The 3.5 mm jack: 1 when a plug is in. board_audio_route() then silences the internal speaker (its amplifier) and sets
 * the codec level for what is listening; call it when the answer changes. */
int board_headphones(void);
void board_audio_route(int headphones);
/* logs the jack, amplifier and codec registers (called every few seconds so the monitor can be opened at any time) */
void board_diag(void);
