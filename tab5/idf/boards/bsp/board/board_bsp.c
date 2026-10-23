/* SPDX-License-Identifier: GPL-3.0-only
 * Board layer for the real M5Stack Tab5, on espressif/m5stack_tab5_noglib.
 *
 * NOT COMPILED OR RUN by the author: the registry packages this needs (esp_codec_dev, esp_lcd_st7123, ...) are
 * not reachable from where the rest of this project was built. It is written against the BSP's headers and
 * sources (esp-bsp, bsp/m5stack_tab5 v1.3.2). Expect to fix a name or two on the first `idf.py build`.
 *
 * The panel is 720 x 1280 PORTRAIT; the UI is 1280 x 720 landscape, so every blit and every touch is rotated here.
 * Default rotation "A" puts the UI's top edge on the panel's right side. If the picture comes out upside down,
 * build with -DBOARD_ROTATE_B (and the touch follows, they share the same mapping).
 */
#include <string.h>
#include "board.h"
#include "bsp/m5stack_tab5.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_codec_dev.h"
#include "driver/i2c_master.h"
#include "usb_midi_host.h"

#define TAG "board"
#define PW 720                                       /* the panel, portrait */
#define PH 1280

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;
static esp_codec_dev_handle_t s_spk;
/* The speaker amplifier's enable (bit 1) and the 3.5 mm jack's detect switch (bit 7) both live in the PI4IOE5V6408 I/O expander at
 * I2C 0x43. They are driven here with raw register accesses, exactly as the working Arduino/M5Unified code does (regs: 0x03
 * direction, 0x05 output, 0x0F input), and not through the BSP's bsp_feature_enable(), which was not silencing the speaker.
 * "High on bit 7 = a plug is in"; if your Tab5 behaves the other way round add BOARD_HP_ACTIVE_LOW to this component. */
#define EXP_ADDR 0x43
#define EXP_REG_CONFIG 0x03
#define EXP_REG_OUT 0x05
#define EXP_REG_IN 0x0F
#define EXP_PIN_SPK 0x02
#define EXP_PIN_JACK 0x80
static i2c_master_dev_handle_t s_exp;
static i2c_master_dev_handle_t s_es;               /* the ES8388 codec at 0x10, raw register access (esp_codec_dev_read/write_reg failed here) */
#define ES_ADDR 0x10
static SemaphoreHandle_t s_lock;                      /* the panel is drawn from two tasks */
static uint16_t *s_tmp;
static volatile uint32_t s_blits, s_blits_br, s_blit_px;   /* repaint counters: all, bottom-right quarter of the UI, pixels */
static size_t s_tmp_px;

esp_err_t board_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    bsp_display_config_t dcfg = {
        .dsi_bus = {.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT, .lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS},
    };
    bsp_lcd_handles_t h = {0};
    bsp_touch_config_t tcfg = {0};
    esp_codec_dev_sample_info_t fs = {.bits_per_sample = 32, .channel = 2, .sample_rate = 44100};

    do { esp_err_t e_ = bsp_display_new_with_handles(&dcfg, &h); if (e_ != ESP_OK) return e_; } while (0);
    s_panel = h.panel;
    do { esp_err_t e_ = esp_lcd_panel_disp_on_off(s_panel, true); if (e_ != ESP_OK) return e_; } while (0);
    bsp_display_brightness_set(100);

    {
        esp_err_t te = bsp_touch_new(&tcfg, &s_touch);
        if (te != ESP_OK) {
            s_touch = NULL;
            ESP_LOGE(TAG, "TOUCH INIT FAILED: %s (the screen works, the keys will not)", esp_err_to_name(te));
        } else {
            ESP_LOGI(TAG, "touch ready");
        }
    }

    s_spk = bsp_audio_codec_speaker_init();          /* also switches the amplifier on; board_audio_route() sets it properly later */
    if (!s_spk) {
        ESP_LOGE(TAG, "speaker init failed");
        return ESP_FAIL;
    }
    {
        int oe = esp_codec_dev_open(s_spk, &fs);
        if (oe != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "CODEC OPEN FAILED (%d): try bits_per_sample = 16 and convert in board_audio_write", oe);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "codec open");
        board_audio_route(board_headphones());        /* the speaker, or the headphones if a plug is already in */
    }
    return ESP_OK;
}

uint32_t board_audio_rate(void) { return 44100; }

/* the firmware's samples are 24 bit left-justified in int32: exactly an I2S 32-bit slot */
void board_audio_write(const int32_t *lr24, size_t frames)
{
    static int errs;
    int e = esp_codec_dev_write(s_spk, (void *)lr24, (int)(frames * 8));
    if (e != ESP_CODEC_DEV_OK && errs < 5) {
        errs++;
        ESP_LOGE(TAG, "codec write failed (%d)", e);
    }
}

/* landscape (x, y) -> portrait (px, py):   A: px = 719 - y, py = x      B: px = y, py = 1279 - x */
void board_display_blit(int x, int y, int w, int h, const uint16_t *src)
{
    int i, j, i0, j0, px0, py0, pw, ph;
    if (!s_panel || w <= 0 || h <= 0)
        return;
    s_blits++;
    s_blit_px += (uint32_t)w * (uint32_t)h;
    if (x + w > 960 && y + h > 480)
        s_blits_br++;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if ((size_t)w * h > s_tmp_px) {
        heap_caps_free(s_tmp);
        s_tmp_px = (size_t)w * h;
        s_tmp = heap_caps_aligned_alloc(64, s_tmp_px * 2, MALLOC_CAP_SPIRAM);
        if (!s_tmp) {
            s_tmp_px = 0;
            xSemaphoreGive(s_lock);
            return;
        }
    }
#ifndef BOARD_ROTATE_B
    px0 = PW - (y + h);                              /* = 719 - (y + h - 1) */
    py0 = x;
    pw = h;
    ph = w;
    for (j0 = 0; j0 < h; j0 += 32)                   /* 32 x 32 tiles: reads and writes stay inside a few cache lines */
        for (i0 = 0; i0 < w; i0 += 32) {
            int je = j0 + 32 < h ? j0 + 32 : h, ie = i0 + 32 < w ? i0 + 32 : w;
            for (i = i0; i < ie; i++)
                for (j = j0; j < je; j++)
                    s_tmp[i * pw + (h - 1 - j)] = src[j * w + i];
        }
#else
    px0 = y;
    py0 = PH - (x + w);                              /* = 1279 - (x + w - 1) */
    pw = h;
    ph = w;
    for (j0 = 0; j0 < h; j0 += 32)
        for (i0 = 0; i0 < w; i0 += 32) {
            int je = j0 + 32 < h ? j0 + 32 : h, ie = i0 + 32 < w ? i0 + 32 : w;
            for (i = i0; i < ie; i++)
                for (j = j0; j < je; j++)
                    s_tmp[(w - 1 - i) * pw + j] = src[j * w + i];
        }
#endif
    esp_lcd_panel_draw_bitmap(s_panel, px0, py0, px0 + pw, py0 + ph, s_tmp);
    xSemaphoreGive(s_lock);
}

int board_touch_read(board_touch_t *pts, int max)
{
    esp_lcd_touch_point_data_t d[5];
    uint8_t n = 0;
    int i, k = 0;
    if (!s_touch)
        return 0;
    if (esp_lcd_touch_read_data(s_touch) != ESP_OK)
        return 0;
    if (esp_lcd_touch_get_data(s_touch, d, &n, 5) != ESP_OK)
        return 0;
    {
        static int last_n = -1;
        if (n != last_n) {
            last_n = n;
            if (n > 0)
                ESP_LOGI(TAG, "touch raw: %d point(s), first panel (%u,%u)", n, d[0].x, d[0].y);
        }
    }
    if (n == 0)
        return 0;
    for (i = 0; i < n && k < max; i++, k++) {
#ifndef BOARD_ROTATE_B
        pts[k].x = (int16_t)d[i].y;
        pts[k].y = (int16_t)(PW - 1 - d[i].x);
#else
        pts[k].x = (int16_t)(PH - 1 - d[i].y);
        pts[k].y = (int16_t)d[i].x;
#endif
        pts[k].id = (uint8_t)d[i].track_id;
    }
    return k;
}

/* the USB-A host port: the BSP powers it and installs the USB Host library, then our MIDI class driver listens */
esp_err_t board_midi_start(board_midi_cb cb)
{
    esp_err_t e = bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, false);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "USB host start failed: %s (no MIDI input)", esp_err_to_name(e));
        return e;
    }
    e = usb_midi_host_start(cb);
    if (e != ESP_OK)
        ESP_LOGE(TAG, "USB MIDI start failed: %s", esp_err_to_name(e));
    else
        ESP_LOGI(TAG, "USB MIDI host ready: plug a class-compliant keyboard or controller into the USB-A port");
    return e;
}

int board_midi_devices(void) { return usb_midi_host_devices(); }

static int exp_rd(uint8_t reg, uint8_t *v)
{
    return s_exp && i2c_master_transmit_receive(s_exp, &reg, 1, v, 1, 50) == ESP_OK;
}

static int exp_wr(uint8_t reg, uint8_t v)
{
    uint8_t b[2] = {reg, v};
    return s_exp && i2c_master_transmit(s_exp, b, 2, 50) == ESP_OK;
}

static void exp_modify(uint8_t reg, uint8_t set, uint8_t clr)
{
    uint8_t v;
    if (exp_rd(reg, &v))
        exp_wr(reg, (uint8_t)((v | set) & ~clr));
}

static int exp_open(void)
{
    if (s_exp)
        return 1;
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus)
        return 0;
    i2c_device_config_t dc = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = EXP_ADDR, .scl_speed_hz = 400000};
    if (i2c_master_bus_add_device(bus, &dc, &s_exp) != ESP_OK) {
        s_exp = NULL;
        ESP_LOGE(TAG, "I/O expander 0x43: cannot open (the speaker cut-off and the jack detect will not work)");
        return 0;
    }
    exp_modify(EXP_REG_CONFIG, EXP_PIN_SPK, EXP_PIN_JACK);   /* bit 1 output (amp enable), bit 7 input (jack), as in the Arduino code */
    exp_modify(0x0B, 0, EXP_PIN_JACK);                       /* M5GFX leaves the jack pin with NO pull (reg 0x0B = 0x7F); a pull on it holds it at 0 */
    return 1;
}

int board_headphones(void)
{
    static int last, shown;
    uint8_t in = 0;
    if (!exp_open())
        return last;
    if (exp_rd(EXP_REG_IN, &in)) {
#ifdef BOARD_HP_ACTIVE_LOW
        int hp = (in & EXP_PIN_JACK) ? 0 : 1;
#else
        int hp = (in & EXP_PIN_JACK) ? 1 : 0;
#endif
        static int prev_in = -1;
        if (in != prev_in) {                          /* ANY bit of the expander input changing: the jack may not be bit 7 on every Tab5 */
            ESP_LOGW(TAG, "EXPANDER INPUT CHANGED: 0x%02X -> 0x%02X (bit7 = jack as assumed, bit4 = ?)", prev_in < 0 ? 0 : prev_in, in);
            prev_in = in;
        }
        if (shown < 3 || hp != last) {                /* a few raw reads at boot, then every change: this is the data to check */
            shown++;
            ESP_LOGI(TAG, "jack: expander input 0x%02X, bit 7 = %d -> %s", in, (in >> 7) & 1, hp ? "plug in" : "empty");
        }
        last = hp;
    }
    return last;
}

static int es_open(void)
{
    if (s_es)
        return 1;
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus)
        return 0;
    i2c_device_config_t dc = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = ES_ADDR, .scl_speed_hz = 400000};
    if (i2c_master_bus_add_device(bus, &dc, &s_es) != ESP_OK) {
        s_es = NULL;
        ESP_LOGE(TAG, "ES8388 0x10: cannot open");
        return 0;
    }
    return 1;
}

static int es_rd(uint8_t reg)                         /* -1 on error */
{
    uint8_t v;
    if (!es_open() || i2c_master_transmit_receive(s_es, &reg, 1, &v, 1, 50) != ESP_OK)
        return -1;
    return v;
}

static int es_wr(uint8_t reg, uint8_t v)
{
    uint8_t b[2] = {reg, v};
    return es_open() && i2c_master_transmit(s_es, b, 2, 50) == ESP_OK;
}

/* The ES8388 as M5Unified sets it up for the Tab5 (this is what the Arduino code gets its loudness from). esp_codec_dev leaves
 * the analog output stage lower: here the four outputs (OUT1/OUT2, L/R) go to +4.5 dB, the mixer takes the DAC, the DAC
 * itself stays at 0 dB. The sample format and clocks (regs 8, 23, 24) are left to esp_codec_dev. */
static void codec_loud(void)
{
    static const uint8_t regs[][2] = {
        {2, 0x0A},                                    /* chip power: everything up */
        {3, 0xFF},                                    /* ADC power: down (we only play) */
        {4, 0x3C},                                    /* DAC power on, LOUT1 ROUT1 LOUT2 ROUT2 enabled */
        {5, 0x00}, {6, 0x00}, {7, 0x7C},
        {25, 0x20},                                   /* DAC unmuted */
        {26, 0x00}, {27, 0x00},                       /* left / right digital volume: 0 dB */
        {28, 0x08},
        {38, 0x00},
        {39, 0xB8}, {42, 0xB8},                       /* DAC -> output mixer, left and right */
        {43, 0x08}, {45, 0x00},
        {46, 0x21}, {47, 0x21}, {48, 0x21}, {49, 0x21}, /* output volume: +4.5 dB (maximum) */
    };
    unsigned i;
    int bad = 0;
    for (i = 0; i < sizeof regs / sizeof regs[0]; i++)
        if (!es_wr(regs[i][0], regs[i][1]))
            bad++;
    ESP_LOGI(TAG, "ES8388: %u registers written, %d failed; DACPOWER 0x%02X mixer 0x%02X outgain 0x%02X (expect 3C B8 21)",
             (unsigned)(sizeof regs / sizeof regs[0]), bad, es_rd(4) & 0xFF, es_rd(39) & 0xFF, es_rd(46) & 0xFF);
}

/* the speaker's amplifier is off while a plug is in (the headphone output of the codec stays on) */
void board_audio_route(int headphones)
{
    uint8_t out = 0xFF;
    if (!s_spk)
        return;
    esp_codec_dev_set_out_mute(s_spk, false);
    esp_codec_dev_set_out_vol(s_spk, 100);
    codec_loud();                                    /* after esp_codec_dev has done its own register writes */
    if (exp_open()) {
        exp_modify(EXP_REG_OUT, headphones ? 0 : EXP_PIN_SPK, headphones ? EXP_PIN_SPK : 0);
        exp_rd(EXP_REG_OUT, &out);
    }
    ESP_LOGI(TAG, "audio output: %s (expander output reg 0x%02X, amp bit %d)", headphones ? "HEADPHONES" : "INTERNAL SPEAKER", out,
             (out >> 1) & 1);
}

void board_audio_enable(void) { board_audio_route(board_headphones()); }

void board_diag(void)
{
    static uint32_t p_all, p_br, p_px;
    ESP_LOGI(TAG, "DIAG repaints in the last ~5 s: %u blits (%u in the bottom-right of the UI), %u kpixels", (unsigned)(s_blits - p_all),
             (unsigned)(s_blits_br - p_br), (unsigned)((s_blit_px - p_px) / 1000));
    p_all = s_blits;
    p_br = s_blits_br;
    p_px = s_blit_px;
    uint8_t in = 0xEE, out = 0xEE, cfg = 0xEE;
    int ok = exp_open();
    if (ok) {
        exp_rd(EXP_REG_IN, &in);
        exp_rd(EXP_REG_OUT, &out);
        exp_rd(EXP_REG_CONFIG, &cfg);
    }
    ESP_LOGI(TAG, "DIAG expander(0x43) %s: input=0x%02X (jack bit7=%d) output=0x%02X (amp bit1=%d) config=0x%02X", ok ? "ok" : "NOT OPEN", in,
             (in >> 7) & 1, out, (out >> 1) & 1, cfg);
    {
        uint8_t pe = 0xEE, ps = 0xEE;
        exp_rd(0x0B, &pe);
        exp_rd(0x0D, &ps);
        ESP_LOGI(TAG, "DIAG expander pull-enable(0x0B)=0x%02X pull-select(0x0D)=0x%02X (M5: 7F 7F)", pe, ps);
    }
    ESP_LOGI(TAG, "DIAG ES8388 (-1 = read failed): chip2=%d dacpower4=%d fmt23=%d mclk24=%d mute25=%d dacvol26=%d mixer39=%d out46=%d out48=%d",
             es_rd(2), es_rd(4), es_rd(23), es_rd(24), es_rd(25), es_rd(26), es_rd(39), es_rd(46), es_rd(48));
}
