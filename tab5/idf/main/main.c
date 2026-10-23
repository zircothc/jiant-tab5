/* SPDX-License-Identifier: GPL-3.0-only
 * JIANT FM on the M5Stack Tab5: the glue between tab5_platform.c (the firmware) and the board.
 *
 *   audio task   (core 1, high priority)  tab5_audio_read() -> board_audio_write(). This ONE task runs the whole
 *                                         device: the firmware's clock is paced by the audio it is asked for.
 *   display task (core 0, 30 Hz)          panel_render(): the firmware's screen doubled, plus every control that changed.
 *   input task   (core 0, 200 Hz)         the fingers -> panel_touch(), and the USB MIDI packets -> panel_midi_in().
 *   USB tasks    (board layer)            the MIDI class driver pushes packets into a queue, nothing else.
 *   persist task (core 0, low priority)   the firmware's flash writes -> the "jiantnor" partition, 4 KB at a time.
 */
#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "board.h"
#include "tab5_platform.h"
#include "panel_ui.h"

static const char *TAG = "jiant";
#define BLOCK 256                                    /* frames per audio write: 5.8 ms at 44.1 kHz */

static QueueHandle_t s_midi_q;                       /* USB MIDI packets, from the USB task to the input task */
static uint8_t s_map_blob[256];                      /* the MIDI learn map, waiting to be saved to NVS */
static volatile size_t s_map_len;
static volatile int s_map_dirty;

static void midi_rx(uint32_t pkt) { xQueueSend(s_midi_q, &pkt, 0); }            /* USB task: never block */
static void map_save_cb(const uint8_t *blob, size_t len)                        /* input task: a mapping changed */
{
    if (len <= sizeof s_map_blob) {
        memcpy(s_map_blob, blob, len);
        s_map_len = len;
        s_map_dirty = 1;
    }
}

static const esp_partition_t *nor_part;
static uint8_t *nor_mem;

/* a 0.4 s 440 Hz tone before anything else: if you hear it, the speaker path works whatever the touch does */
static void boot_beep(void)
{
    static int32_t b[BLOCK * 2];
    float ph = 0;
    for (int blk = 0; blk < 69; blk++) {
        for (int i = 0; i < BLOCK; i++, ph += 6.2831853f * 440.0f / 44100.0f) {
            int32_t v = (int32_t)(sinf(ph) * 0.2f * 8388607.0f) << 8;      /* 24 bit, left-justified */
            b[2 * i] = b[2 * i + 1] = v;
        }
        board_audio_write(b, BLOCK);
    }
}

static uint32_t hook_ticks(void) { return (uint32_t)(esp_timer_get_time() * 24); }   /* the firmware's 24 MHz TIMER4 */

static volatile int s_level = 4;                     /* output level 0..12: software gain, 3 dB per step (12 = +36 dB, the limiter does the rest) (see audio_task) */
static volatile int s_hp;                            /* headphones plugged into the 3.5 mm jack */
static volatile int s_level_dirty;
static volatile int s_tone_req;                      /* the learn page's TONE button */
static void level_cb(int delta)                      /* input task: the learn page's LEVEL - / + */
{
    int v = s_level + delta;
    s_level = v < 0 ? 0 : v > 12 ? 12 : v;
    s_level_dirty = 1;
    ESP_LOGI(TAG, "output level %d/12 (%s)", s_level, s_hp ? "headphones" : "speaker");
}

/* a soft limiter: transparent up to 0.6 of full scale, then bends towards 1.0 instead of clipping flat */
static inline float softclip(float x)
{
    float a = fabsf(x), y;
    if (a < 0.6f)
        return x;
    y = 0.6f + 0.4f * tanhf((a - 0.6f) * 2.5f);
    return x < 0 ? -y : y;
}

static void tone_cb(void)                            /* input task */
{
    board_audio_enable();                            /* in case the amplifier lost its enable */
    s_tone_req = 1;
}

static void audio_task(void *arg)
{
    static int32_t buf[BLOCK * 2];
    int64_t worst = 0, sum = 0, t_log = esp_timer_get_time();
    uint32_t blocks = 0;
    int32_t peak = 0;
    int tone_left = 0;
    float tph = 0;
    (void)arg;
    for (;;) {
        int64_t t0 = esp_timer_get_time(), dt;
        tab5_audio_read(buf, BLOCK);                 /* runs the device until BLOCK frames exist */
        dt = esp_timer_get_time() - t0;
        for (int i = 0; i < BLOCK * 2; i++) {        /* the loudest sample the firmware produced (24 bit) */
            int32_t v = buf[i] >> 8;
            if (v < 0)
                v = -v;
            if (v > peak)
                peak = v;
        }
        if (s_tone_req) {
            s_tone_req = 0;
            tone_left = 44100 / 2;                   /* half a second of 440 Hz instead of the firmware's output */
        }
        if (tone_left > 0) {                         /* about the level of one note, so it goes through the gain like music */
            for (int i = 0; i < BLOCK; i++, tph += 6.2831853f * 440.0f / 44100.0f)
                buf[2 * i] = buf[2 * i + 1] = (int32_t)(sinf(tph) * 0.09f * 8388607.0f) << 8;
            tone_left -= BLOCK;
        }
        {   /* the Tab5's speaker is small: the firmware's own level (made for a line output) gets a gain, a soft limiter keeps it clean */
            float g = powf(10.0f, (float)(s_level * 3 - (s_hp ? 12 : 0)) / 20.0f);
            for (int i = 0; i < BLOCK * 2; i++) {
                float y = softclip((float)(buf[i] >> 8) * (1.0f / 8388608.0f) * g);
                buf[i] = (int32_t)(y * 8388607.0f) << 8;
            }
        }
        board_audio_write(buf, BLOCK);               /* blocks: this is what keeps time */
        sum += dt;
        blocks++;
        if (dt > worst)
            worst = dt;
        if (esp_timer_get_time() - t_log > 5000000) {
            ESP_LOGI(TAG, "audio: compute avg %lld us, worst %lld us of %lld us per block; firmware cpu %u/256; firmware peak %d%% of full scale, gain %+d dB (%s); stack free %u B",
                     (long long)(sum / blocks), (long long)worst, (long long)BLOCK * 1000000 / 44100,
                     (unsigned)tab5_cpu_q8(), (int)((int64_t)peak * 100 / 8388607), s_level * 3 - (s_hp ? 12 : 0), s_hp ? "headphones" : "speaker",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
            worst = sum = 0;
            peak = 0;
            blocks = 0;
            t_log = esp_timer_get_time();
        }
    }
}

static void display_task(void *arg)
{
    (void)arg;
    for (;;) {
        panel_render();
        vTaskDelay(pdMS_TO_TICKS(33));
    }
}

static void input_task(void *arg)
{
    int hp_cand = s_hp, hp_count = 0;
    uint32_t n_loop = 0;
    (void)arg;
    for (;;) {
        if (++n_loop % 20 == 0) {                    /* every ~100 ms: is a plug in the jack? three equal reads = it changed */
            int hp = board_headphones();
            if (hp == s_hp) {
                hp_count = 0;
            } else if (hp == hp_cand && ++hp_count >= 3) {
                s_hp = hp;
                hp_count = 0;
                ESP_LOGI(TAG, "3.5 mm jack: %s", hp ? "plug inserted" : "plug removed");
                board_audio_route(hp);               /* speaker amplifier off / on, codec level */
            } else if (hp != hp_cand) {
                hp_cand = hp;
                hp_count = 1;
            }
        }
        if (n_loop % 1000 == 500)                    /* every ~5 s: jack, amplifier and codec state (the monitor may be opened at any time) */
            board_diag();
        panel_set_audio_info(s_level, s_hp);
        board_touch_t t[5];
        panel_pt p[5];
        int n = board_touch_read(t, 5);
        for (int i = 0; i < n; i++) {
            p[i].x = t[i].x;
            p[i].y = t[i].y;
            p[i].id = t[i].id;
        }
        {   /* the same task owns the panel's state: fingers and MIDI both go through here */
            uint32_t pkt;
            panel_midi_set_devices(board_midi_devices());
            while (xQueueReceive(s_midi_q, &pkt, 0) == pdTRUE)
                panel_midi_in(pkt);
        }
        panel_touch(p, n, (uint32_t)(esp_timer_get_time() / 1000));
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/* the firmware writes its "NOR" in RAM; persist the 4 KB sectors it touched. Flash writes stall the CPU caches,
 * so while the sequencer plays they wait up to 5 s (a pause in the audio is worse than a late save). */
static void persist_task(void *arg)
{
    int64_t dirty_since = 0;
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(250));
        if (s_level_dirty && !tab5_playing()) {
            nvs_handle_t h;
            s_level_dirty = 0;
            if (nvs_open("jiant", NVS_READWRITE, &h) == ESP_OK) {
                nvs_set_u8(h, "level", (uint8_t)s_level);
                nvs_commit(h);
                nvs_close(h);
            }
        }
        if (s_map_dirty && !tab5_playing()) {          /* the learn map: rare, small, saved when the music is not running */
            nvs_handle_t h;
            s_map_dirty = 0;
            if (nvs_open("jiant", NVS_READWRITE, &h) == ESP_OK) {
                nvs_set_blob(h, "midimap", s_map_blob, s_map_len);
                nvs_commit(h);
                nvs_close(h);
            }
        }
        if (!nor_part)
            continue;
        if (!dirty_since && tab5_flash_write_count() != 0)
            dirty_since = esp_timer_get_time();
        if (tab5_playing() && esp_timer_get_time() - dirty_since < 5000000)
            continue;
        for (int32_t s; (s = tab5_nor_take_dirty()) >= 0;) {
            esp_partition_erase_range(nor_part, (size_t)s * 4096, 4096);
            esp_partition_write(nor_part, (size_t)s * 4096, nor_mem + (size_t)s * 4096, 4096);
            dirty_since = 0;
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "JIANT FM for the Tab5, build %s %s", __DATE__, __TIME__);     /* proves WHICH build is running */
    {   /* NVS keeps the MIDI map; a first boot or a changed layout just starts empty */
        esp_err_t e = nvs_flash_init();
        if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            nvs_flash_erase();
            nvs_flash_init();
        }
    }
    ESP_ERROR_CHECK(board_init());
    s_hp = board_headphones();
    ESP_LOGI(TAG, "3.5 mm jack at boot: %s", s_hp ? "plug in -> headphones" : "empty -> internal speaker");
    s_midi_q = xQueueCreate(256, sizeof(uint32_t));

    nor_mem = heap_caps_malloc(tab5_nor_size(), MALLOC_CAP_SPIRAM);
    if (!nor_mem) {
        ESP_LOGE(TAG, "no memory for the flash image");
        return;
    }
    memset(nor_mem, 0xFF, tab5_nor_size());
    nor_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "jiantnor");
    if (nor_part && nor_part->size >= tab5_nor_size())
        esp_partition_read(nor_part, 0, nor_mem, tab5_nor_size());
    else
        ESP_LOGW(TAG, "no 'jiantnor' partition: nothing will be saved");
    tab5_set_nor(nor_mem);

    tab5_ticks_hook = hook_ticks;                    /* a real clock: the firmware's CPU meter and load shedding work */
    board_audio_enable();
    boot_beep();
    tab5_power_on();
    {
        uint16_t *scratch = heap_caps_malloc(480 * 480 * 2, MALLOC_CAP_SPIRAM);     /* the biggest rectangle: the screen */
        if (!scratch) {
            ESP_LOGE(TAG, "no memory for the panel");
            return;
        }
        panel_init(scratch, 480 * 480, board_display_blit);
        {
            nvs_handle_t h;
            uint8_t blob[256];
            size_t len = sizeof blob;
            if (nvs_open("jiant", NVS_READONLY, &h) == ESP_OK) {
                if (nvs_get_blob(h, "midimap", blob, &len) == ESP_OK && panel_midi_map_import(blob, len) == 0)
                    ESP_LOGI(TAG, "MIDI map restored: %d mapping(s)", panel_midi_mapped_count());
                nvs_close(h);
            }
        }
        panel_midi_map_set_save_cb(map_save_cb);
        {
            nvs_handle_t h;
            uint8_t lv;
            if (nvs_open("jiant", NVS_READONLY, &h) == ESP_OK) {
                if (nvs_get_u8(h, "level", &lv) == ESP_OK && lv <= 12)
                    s_level = lv;
                nvs_close(h);
            }
        }
        panel_set_level_cb(level_cb);
    }
    panel_set_tone_cb(tone_cb);
    if (board_midi_start(midi_rx) != ESP_OK)
        ESP_LOGW(TAG, "no USB MIDI input");
    board_audio_enable();                            /* starting USB touches the board's power switches: make sure the speaker kept its enable */
    ESP_LOGI(TAG, "JIANT up: %u Hz, I2S half %u frames", (unsigned)tab5_sample_rate(), (unsigned)tab5_half_frames());

    xTaskCreatePinnedToCore(display_task, "display", 4096, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(input_task, "input", 4096, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(persist_task, "persist", 4096, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(audio_task, "audio", 24576, NULL, configMAX_PRIORITIES - 2, NULL, 1);
}
