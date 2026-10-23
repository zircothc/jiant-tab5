/* SPDX-License-Identifier: GPL-3.0-only
 * Headless board: no drivers. Audio is paced like a real sink (so the app's timing is realistic), the
 * screen and touch do nothing. It exists to compile, size and (on a real P4) run the firmware without a Tab5. */
#include "board.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

esp_err_t board_init(void) { return ESP_OK; }
uint32_t board_audio_rate(void) { return 44100; }

void board_audio_write(const int32_t *lr24, size_t frames)
{
    static int64_t due;
    int64_t now = esp_timer_get_time();
    (void)lr24;
    if (!due)
        due = now;
    due += (int64_t)frames * 1000000 / 44100;
    if (due - now > 1000)
        vTaskDelay(pdMS_TO_TICKS((due - now) / 1000));
    else if (now - due > 20000)
        due = now;                                   /* an underrun: do not try to catch up */
}

void board_display_blit(int x, int y, int w, int h, const uint16_t *rgb565) { (void)x; (void)y; (void)w; (void)h; (void)rgb565; }
int board_touch_read(board_touch_t *pts, int max) { (void)pts; (void)max; return 0; }

esp_err_t board_midi_start(board_midi_cb cb) { (void)cb; return ESP_OK; }
int board_midi_devices(void) { return 0; }
void board_audio_enable(void) {}
int board_headphones(void) { return 0; }
void board_audio_route(int headphones) { (void)headphones; }
void board_diag(void) {}
