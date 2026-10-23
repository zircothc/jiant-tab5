/* SPDX-License-Identifier: GPL-3.0-only
 * The FM-1's whole panel on a 1280 x 720 landscape touch screen. See panel_ui.h.
 *
 *   +--------------------+  [FX ][SCL][ENV][LFO][EDIT][GLO ]      the firmware's 240 x 240 screen, doubled (8,8)
 *   |  the firmware's    |  [HOME][SAVE][ARP][SEQ][PLAY][REC]     12 buttons in a 6 x 2 grid
 *   |  screen  480x480   |    (K1)    (K2)    (K3)    (K4)         4 knobs: drag up / down
 *   |                    |  (MASTER)(SELECT)(PRESETS)(ALGO) [OCT-][OCT+]
 *   +--------------------+
 *   [ the 27 keys: a row of 11 and a row of 16, the way the FM-1 lays them out ]
 *
 * Touch model: a finger belongs to the control it first lands on until it lifts (so a finger that slides off a knob
 * keeps turning it). Several fingers work at once: hold a button with one, play with the others. A button is held
 * while touched; touch it ALONE for 0.6 s and lift, and it stays down (outlined in yellow) until you tap it again,
 * which frees both hands for the combos the firmware is built on. Using any other control while holding it cancels
 * that, so a normal "hold FX, play keys" never gets stuck.
 *
 * MIDI: every control can be driven by a note or a CC from an external controller ("MIDI learn"): press the MIDI
 * button, touch a control, move something on the controller; it is remembered (panel_midi_map_export/import).
 * Anything that is not mapped goes on to the firmware's own MIDI input, so a keyboard still plays the tracks. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "panel_ui.h"
#include "tab5_platform.h"

#define SCR_X 8
#define SCR_Y 8
#define GRID_X 520
#define GRID_Y 24
#define CELL_W 116
#define CELL_H 88
#define GRID_DX 124
#define GRID_DY 96
#define KB_X 16
#define KB_Y 508
#define KB_W 1248
#define KB_H 200
#define PITCH ((KB_W - 24) / 16.0f)
#define KEY_W 59
#define KEY_H 85
#define LATCH_MS 600
#define PX_PER_STEP 14                                /* a knob moves one step per this many pixels dragged */
#define MASTER_RANGE 320                              /* pixels of drag for MASTER's whole 0..1023 */

#define RGB(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define C_BLACK RGB(0, 0, 0)
#define C_FRAME RGB(70, 76, 92)
#define C_BTN RGB(44, 50, 66)
#define C_BTN_BACK RGB(60, 68, 90)
#define C_BTN_GLOW RGB(140, 98, 20)
#define C_BTN_LED RGB(255, 178, 36)
#define C_DOWN RGB(52, 224, 104)
#define C_LATCH RGB(255, 232, 0)
#define C_TEXT RGB(236, 240, 250)
#define C_TEXT_DK RGB(18, 20, 28)
#define C_KEY_W RGB(196, 200, 212)
#define C_KEY_B RGB(92, 98, 112)
#define C_KNOB RGB(56, 62, 78)
#define C_KNOB_RING RGB(128, 136, 158)
#define C_CYAN RGB(0, 204, 255)
#define C_LEARN_BG RGB(8, 14, 30)
#define C_DIM_TEXT RGB(138, 148, 172)

#define NBTN 15                                       /* 0..13 the firmware's buttons, 14 = MIDI (this panel's own) */
#define B_MIDI 14
static const char *const BTN_LABEL[NBTN] = {"FX", "SCL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE",
                                            "ARP", "SEQ", "PLAY", "REC", "OCT-", "OCT+", "MIDI"};
static const uint8_t WHITE_KEY[16] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};
static const uint8_t BLACK_KEY[11] = {1, 3, 5, 8, 10, 13, 15, 17, 20, 22, 25};
static const uint8_t BLACK_AFTER[11] = {0, 1, 2, 4, 5, 7, 8, 9, 11, 12, 14};

#define K_MASTER 7                                    /* knob ids 0..6 are TAB5_E_*, 7 is MASTER */
#define NKNOB 8
typedef struct { int16_t cx, cy, r; const char *label; } knob_t;
static const knob_t KNOB[NKNOB] = {
    [TAB5_E_SELECT] = {674, 430, 44, "SELECT"},
    [TAB5_E_ALGORITHM] = {914, 430, 44, "ALGORITHM"},
    [TAB5_E_PRESETS] = {794, 430, 44, "PRESETS"},
    [TAB5_E_K1] = {600, 292, 52, "KNOB 1"},
    [TAB5_E_K2] = {790, 292, 52, "KNOB 2"},
    [TAB5_E_K3] = {980, 292, 52, "KNOB 3"},
    [TAB5_E_K4] = {1170, 292, 52, "KNOB 4"},
    [K_MASTER] = {554, 430, 44, "MASTER"},
};

/* ------------------------------------------------------------------------------------------- state --- */
enum { T_NONE, T_BTN, T_KEY, T_KNOB };
typedef struct {
    uint8_t active, id, kind, idx, other_used;
    int16_t x, y;
    int32_t drag;                                     /* knob: pixels not yet turned into a step */
    uint32_t t_down;
} contact_t;
#define NC 6
static contact_t C[NC];
static uint16_t *s_buf;
static size_t s_buf_px;
static panel_blit_fn s_blit;
static uint8_t s_latched[NBTN];
static int16_t s_master = 700;                        /* 0..1023, as tab5_platform starts it */
static int16_t s_angle[NKNOB];                        /* pointer angle, degrees from straight up */
static uint32_t s_sig_btn[NBTN], s_sig_key[TAB5_NKEYS], s_sig_knob[NKNOB];

/* ---- MIDI learn state ----------------------------------------------------------------------------------------- */
#define NT (14 + NKNOB + TAB5_NKEYS)                  /* targets: 14 buttons, 8 knobs, 27 keys */
#define T_OF_BTN(b) (b)
#define T_OF_KNOB(k) (14 + (k))
#define T_OF_KEY(k) (14 + NKNOB + (k))
enum { SRC_NONE, SRC_NOTE, SRC_CC };
enum { MODE_A, MODE_B };                              /* buttons: momentary / toggle (lock); knobs: absolute / relative */
typedef struct { uint8_t type, ch, num, mode; } map_t;
static map_t s_map[NT];
static int s_learn, s_sel = -1;                       /* learn mode on; the target being learnt (or -1) */
static uint8_t s_mbtn[NBTN], s_mkey[TAB5_NKEYS];      /* held by a MIDI source */
static int16_t s_abs_last[NKNOB];                     /* last absolute CC seen per knob (-1: none yet) */
static int16_t s_abs_acc[NKNOB];
static uint8_t s_last_msg[3];
static uint32_t s_last_seq = 0, s_learn_sig = 0xFFFFFFFFu;
static int s_ndev;
static void (*s_save_cb)(const uint8_t *blob, size_t len);
static uint32_t s_screen_draws = 0xFFFFFFFFu;
static int s_drawn_bg;

/* ---------------------------------------------------------------------------------------- geometry --- */
static void btn_rect(int b, int *x, int *y)
{
    if (b < 12) {
        *x = GRID_X + (b % 6) * GRID_DX;
        *y = GRID_Y + (b / 6) * GRID_DY;
    } else {
        *x = 984 + (b - 12) * 96;                     /* OCT-, OCT+, MIDI: 88 x 88 */
        *y = 386;
    }
}
static int btn_w(int b) { return b < 12 ? CELL_W : 88; }
static int btn_h(int b) { return b < 12 ? CELL_H : 88; }
static void key_rect(int k, int *x, int *y, int *black)
{
    int i;
    for (i = 0; i < 16; i++)
        if (WHITE_KEY[i] == k) {
            *x = (int)(KB_X + 12 + (i + 0.5f) * PITCH) - KEY_W / 2;
            *y = (int)(KB_Y + 0.75f * KB_H) - KEY_H / 2;
            *black = 0;
            return;
        }
    for (i = 0; i < 11; i++)
        if (BLACK_KEY[i] == k) {
            *x = (int)(KB_X + 12 + (BLACK_AFTER[i] + 1) * PITCH) - KEY_W / 2;
            *y = (int)(KB_Y + 0.25f * KB_H) - KEY_H / 2;
            *black = 1;
            return;
        }
    *x = *y = *black = 0;
}
static void knob_rect(int k, int *x, int *y, int *w, int *h)
{
    *x = KNOB[k].cx - 58;
    *y = KNOB[k].cy - KNOB[k].r - 4;
    *w = 116;
    *h = 2 * KNOB[k].r + 8 + 20;
}
static int key_at(int x, int y)                       /* the touch zones are larger than the drawn keys */
{
    float fx = (x - KB_X - 12) / PITCH;
    int i;
    if (y < KB_Y || y >= KB_Y + KB_H || fx < 0 || fx >= 16)
        return -1;
    if (y < KB_Y + KB_H / 2) {
        for (i = 0; i < 11; i++) {
            float c = BLACK_AFTER[i] + 1;
            if (fx >= c - 0.45f && fx < c + 0.45f)
                return BLACK_KEY[i];
        }
        return -1;
    }
    return WHITE_KEY[(int)fx];
}
static int hit(int x, int y, int *kind, int *idx)
{
    int b, k, kx, ky, kw, kh;
    for (b = 0; b < NBTN; b++) {
        btn_rect(b, &kx, &ky);
        if (x >= kx && x < kx + btn_w(b) && y >= ky && y < ky + btn_h(b)) {
            *kind = T_BTN;
            *idx = b;
            return 1;
        }
    }
    for (k = 0; k < NKNOB; k++) {
        knob_rect(k, &kx, &ky, &kw, &kh);
        if (x >= kx && x < kx + kw && y >= ky && y < ky + kh) {
            *kind = T_KNOB;
            *idx = k;
            return 1;
        }
    }
    if ((k = key_at(x, y)) >= 0) {
        *kind = T_KEY;
        *idx = k;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------------------- primitives --- */
static void fill(int bw, int bh, uint16_t c)
{
    int i;
    for (i = 0; i < bw * bh; i++)
        s_buf[i] = c;
}
static void rrect(int bw, int bh, int x, int y, int w, int h, int r, uint16_t c)
{
    int i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++) {
            int dx = i < r ? r - i : (i >= w - r ? i - (w - 1 - r) : 0);
            int dy = j < r ? r - j : (j >= h - r ? j - (h - 1 - r) : 0);
            if (dx * dx + dy * dy <= r * r && x + i >= 0 && y + j >= 0 && x + i < bw && y + j < bh)
                s_buf[(y + j) * bw + x + i] = c;
        }
}
static void disc(int bw, int bh, int cx, int cy, int r, uint16_t c)
{
    int i, j;
    for (j = -r; j <= r; j++)
        for (i = -r; i <= r; i++)
            if (i * i + j * j <= r * r && cx + i >= 0 && cy + j >= 0 && cx + i < bw && cy + j < bh)
                s_buf[(cy + j) * bw + cx + i] = c;
}
static void thick_line(int bw, int bh, int x0, int y0, int x1, int y1, uint16_t c)
{
    int n = 40, i;
    for (i = 0; i <= n; i++)
        disc(bw, bh, x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n, 2, c);
}
static void text_centered(int bw, int bh, int cx, int y, const char *s, uint16_t c)
{
    tab5_text_draw(s_buf, bw, bh, cx - tab5_text_width(s, 0) / 2, y, s, c, 0);
}

/* --------------------------------------------------------------------------------------- drawing --- */
#define CYAN_BOX 3
/* learn marks of a target in learn mode: bit0 it has a MIDI source, bit1 it is the one being learnt */
static uint32_t learn_bits(int t)
{
    if (!s_learn)
        return 0;
    return (s_map[t].type != SRC_NONE ? 1u : 0u) | (t == s_sel ? 2u : 0u);
}
static void learn_overlay(int bw, int bh, uint32_t lb, int dot_x, int dot_y)
{
    int i;
    if (lb & 2u) {                                     /* a cyan frame round the target being learnt */
        for (i = 0; i < CYAN_BOX; i++) {
            int x, y;
            for (x = 0; x < bw; x++) {
                s_buf[(i)*bw + x] = C_CYAN;
                s_buf[(bh - 1 - i) * bw + x] = C_CYAN;
            }
            for (y = 0; y < bh; y++) {
                s_buf[y * bw + i] = C_CYAN;
                s_buf[y * bw + bw - 1 - i] = C_CYAN;
            }
        }
    }
    if (lb & 1u)
        disc(bw, bh, dot_x, dot_y, 6, C_CYAN);         /* "this one answers to a MIDI source" */
}

static void draw_button(int b)
{
    int x, y, w = btn_w(b), h = btn_h(b), down = 0, c, i;
    uint32_t lit = tab5_lit_buttons(), glow = tab5_glow_buttons(), back = tab5_back_buttons(), sig, lb = 0;
    uint16_t fg = C_TEXT, bg;
    if (b < TAB5_NBUTTONS) {
        for (i = 0; i < NC; i++)
            if (C[i].active && C[i].kind == T_BTN && C[i].idx == b)
                down = 1;
        down |= s_mbtn[b];
        lb = learn_bits(T_OF_BTN(b));
    } else {                                          /* the MIDI button itself: lit while learning */
        down = s_learn;
        lit = glow = back = 0;
    }
    sig = (uint32_t)down | ((uint32_t)s_latched[b] << 1) | (b < 14 ? (((lit >> b) & 1u) << 2) | (((glow >> b) & 1u) << 3) |
          (((back >> b) & 1u) << 4) : 0u) | (lb << 5);
    if (sig == s_sig_btn[b])
        return;
    s_sig_btn[b] = sig;
    btn_rect(b, &x, &y);
    bg = b < 14 && (lit >> b) & 1u ? C_BTN_LED : b < 14 && (glow >> b) & 1u ? C_BTN_GLOW :
         b < 14 && (back >> b) & 1u ? C_BTN_BACK : C_BTN;
    if (down)
        bg = b == B_MIDI ? C_CYAN : C_DOWN;
    if ((b < 14 && ((lit >> b) & 1u)) || down)
        fg = C_TEXT_DK;
    fill(w, h, C_BLACK);
    c = s_latched[b] ? C_LATCH : C_FRAME;
    rrect(w, h, 0, 0, w, h, 12, c);
    rrect(w, h, s_latched[b] ? 4 : 2, s_latched[b] ? 4 : 2, w - (s_latched[b] ? 8 : 4), h - (s_latched[b] ? 8 : 4), 10, bg);
    text_centered(w, h, w / 2, h / 2 - 8, BTN_LABEL[b], fg);
    learn_overlay(w, h, lb, w - 14, 14);
    s_blit(x, y, w, h, s_buf);
}

static void draw_key(int k)
{
    int x, y, black, down = 0, i;
    uint32_t lit = tab5_lit_keys(), glow = tab5_glow_keys(), back = tab5_back_keys(), sig, lb = learn_bits(T_OF_KEY(k));
    uint16_t col;
    for (i = 0; i < NC; i++)
        if (C[i].active && C[i].kind == T_KEY && C[i].idx == k)
            down = 1;
    down |= s_mkey[k];
    sig = (uint32_t)down | (((lit >> k) & 1u) << 1) | (((glow >> k) & 1u) << 2) | (((back >> k) & 1u) << 3) | (lb << 4);
    if (sig == s_sig_key[k])
        return;
    s_sig_key[k] = sig;
    key_rect(k, &x, &y, &black);
    col = black ? C_KEY_B : C_KEY_W;
    if ((back >> k) & 1u)
        col = black ? RGB(118, 128, 160) : RGB(176, 196, 236);
    if ((glow >> k) & 1u)
        col = C_BTN_GLOW;
    if ((lit >> k) & 1u)
        col = C_BTN_LED;
    if (down)
        col = C_DOWN;
    fill(KEY_W, KEY_H, C_BLACK);
    rrect(KEY_W, KEY_H, 0, 0, KEY_W, KEY_H, 8, col);
    rrect(KEY_W, KEY_H, KEY_W / 2 - 2, 14, 4, KEY_H - 28, 2, black ? RGB(60, 64, 76) : RGB(150, 154, 168));   /* the slit */
    learn_overlay(KEY_W, KEY_H, lb, KEY_W - 12, 12);
    s_blit(x, y, KEY_W, KEY_H, s_buf);
}

static void draw_knob(int k)
{
    int x, y, w, h, down = 0, i, r = KNOB[k].r, cx = 58, cy = r + 4, ang;
    uint32_t sig, lb = learn_bits(T_OF_KNOB(k));
    float a;
    for (i = 0; i < NC; i++)
        if (C[i].active && C[i].kind == T_KNOB && C[i].idx == k)
            down = 1;
    ang = k == K_MASTER ? (s_master * 270 / 1023) - 135 : s_angle[k];
    sig = (uint32_t)down | ((uint32_t)((ang + 720) % 360) << 1) | (lb << 10);
    if (sig == s_sig_knob[k])
        return;
    s_sig_knob[k] = sig;
    knob_rect(k, &x, &y, &w, &h);
    fill(w, h, C_BLACK);
    disc(w, h, cx, cy, r, down ? C_DOWN : (lb & 2u) ? C_CYAN : C_KNOB_RING);
    disc(w, h, cx, cy, r - 4, C_KNOB);
    a = (float)ang * 3.14159265f / 180.0f;
    thick_line(w, h, cx, cy, cx + (int)(sinf(a) * (r - 8)), cy - (int)(cosf(a) * (r - 8)), C_TEXT);
    disc(w, h, cx, cy, 4, C_TEXT);
    text_centered(w, h, w / 2, 2 * r + 10, KNOB[k].label, C_TEXT);
    if (lb & 1u)
        disc(w, h, cx + r - 4, cy - r + 6, 6, C_CYAN);
    s_blit(x, y, w, h, s_buf);
}

static void draw_screen(void)
{
    const uint16_t *fb = tab5_screen();
    uint32_t d = tab5_screen_draws(), x, y, y0;
    if (d == s_screen_draws)
        return;
    s_screen_draws = d;
    /* in bands of 16 source rows (480 x 32 pixels, 30 KB): the working set stays in the CPU cache instead of making three
     * trips through the 460 KB of PSRAM per frame, which starved the audio (the oscilloscope redraws every frame) */
    for (y0 = 0; y0 < 240; y0 += 16) {
        for (y = y0; y < y0 + 16; y++) {
            uint16_t *r0 = s_buf + ((y - y0) * 2) * 480, *r1 = r0 + 480;
            for (x = 0; x < 240; x++) {
                uint16_t p = fb[y * 240 + x];
                p = (uint16_t)((p >> 8) | (p << 8));      /* the panel's byte order -> native RGB565 */
                r0[2 * x] = r0[2 * x + 1] = r1[2 * x] = r1[2 * x + 1] = p;
            }
        }
        s_blit(SCR_X, SCR_Y + y0 * 2, 480, 32, s_buf);
    }
}

/* ------------------------------------------------------------------------------------- MIDI learn --- */
static const char *const NOTE_NAME[12] = {"F", "F#", "G", "G#", "A", "A#", "B", "C", "C#", "D", "D#", "E"};

static void target_name(int t, char *out, size_t n)
{
    if (t < 0)
        snprintf(out, n, "-");
    else if (t < 14)
        snprintf(out, n, "BUTTON %s", BTN_LABEL[t]);
    else if (t < 14 + NKNOB)
        snprintf(out, n, "KNOB %s", KNOB[t - 14].label);
    else {
        int k = t - 14 - NKNOB;
        snprintf(out, n, "KEY %d  %s%d", k, NOTE_NAME[k % 12], 3 + (k + 5) / 12);
    }
}
static int target_is_knob(int t) { return t >= 14 && t < 14 + NKNOB; }
static int target_is_button(int t) { return t >= 0 && t < 14; }

typedef struct { int16_t x, y, w, h; const char *label; } soft_t;
enum { S_MODE, S_CLEAR, S_CLEAR_ALL, S_EXIT, S_TONE, S_VOLDN, S_VOLUP, NSOFT };
static const soft_t SOFT[NSOFT] = {
    {8, 364, 236, 60, "MODE"}, {252, 364, 236, 60, "CLEAR"}, {8, 428, 236, 60, "CLEAR ALL"}, {252, 428, 236, 60, "EXIT"},
    {384, 12, 96, 56, "TONE"}, {8, 300, 116, 56, "LEVEL -"}, {132, 300, 116, 56, "LEVEL +"}};
static void (*s_tone_cb)(void);                       /* a 440 Hz test tone through the audio path (a speaker check) */
static void (*s_level_cb)(int delta);                 /* the output level, +1 / -1 */
static int s_out_level = -1, s_out_hp;                /* shown on the page: 0..12, and whether headphones are in */
void panel_set_tone_cb(void (*cb)(void)) { s_tone_cb = cb; }
void panel_set_level_cb(void (*cb)(int delta)) { s_level_cb = cb; }
void panel_set_audio_info(int level, int headphones) { s_out_level = level; s_out_hp = headphones; }

static void map_changed(void)
{
    if (s_save_cb) {
        uint8_t blob[4 + NT * 4];
        size_t n = 0;
        panel_midi_map_export(blob, &n);
        s_save_cb(blob, n);
    }
    s_learn_sig = 0xFFFFFFFFu;
}

static void draw_learn_page(void)
{
    char line[64], tn[40];
    int i, y = 0;
    uint32_t sig = 2166136261u;
    {
        uint32_t parts[8] = {(uint32_t)(s_sel + 2), s_sel >= 0 ? s_map[s_sel].type : 0u, s_sel >= 0 ? s_map[s_sel].num : 0u,
                             s_sel >= 0 ? s_map[s_sel].ch : 0u, s_sel >= 0 ? s_map[s_sel].mode : 0u, s_last_seq,
                             (uint32_t)s_last_msg[0] | (uint32_t)s_last_msg[1] << 8 | (uint32_t)s_last_msg[2] << 16,
                             (uint32_t)s_ndev | (uint32_t)(s_out_level + 1) << 8 | (uint32_t)s_out_hp << 16};
        for (i = 0; i < 8; i++)
            sig = (sig ^ parts[i]) * 16777619u;
    }
    if (sig == s_learn_sig)
        return;
    s_learn_sig = sig;
    fill(480, 480, C_LEARN_BG);
    tab5_text_draw(s_buf, 480, 480, 16, 12, "MIDI LEARN", C_CYAN, 1);
    snprintf(line, sizeof line, "USB MIDI: %s", s_ndev ? "CONNECTED" : "NOTHING CONNECTED");
    tab5_text_draw(s_buf, 480, 480, 16, 56, line, s_ndev ? C_DOWN : C_BTN_LED, 0);
    tab5_text_draw(s_buf, 480, 480, 16, 84, "1  TOUCH A CONTROL ON THE PANEL", C_TEXT, 0);
    tab5_text_draw(s_buf, 480, 480, 16, 104, "2  MOVE A KNOB OR PRESS A PAD", C_TEXT, 0);
    tab5_text_draw(s_buf, 480, 480, 16, 124, "   ON YOUR CONTROLLER", C_TEXT, 0);
    target_name(s_sel, tn, sizeof tn);
    snprintf(line, sizeof line, "TARGET  %s", tn);
    tab5_text_draw(s_buf, 480, 480, 16, 164, line, C_CYAN, 0);
    if (s_sel < 0) {
        y = 190;
        tab5_text_draw(s_buf, 480, 480, 16, y, "(NONE SELECTED)", C_DIM_TEXT, 0);
    } else if (s_map[s_sel].type == SRC_NONE) {
        tab5_text_draw(s_buf, 480, 480, 16, 190, "WAITING FOR MIDI...", C_BTN_LED, 0);
    } else {
        const map_t *m = &s_map[s_sel];
        const char *mode = target_is_knob(s_sel) ? (m->mode ? "RELATIVE" : "ABSOLUTE") : (m->mode ? "TOGGLE" : "MOMENTARY");
        snprintf(line, sizeof line, "%s %d  CH %d", m->type == SRC_NOTE ? "NOTE" : "CC", m->num, m->ch + 1);
        tab5_text_draw(s_buf, 480, 480, 16, 190, line, C_DOWN, 0);
        snprintf(line, sizeof line, "MODE  %s", mode);
        tab5_text_draw(s_buf, 480, 480, 16, 212, line, C_TEXT, 0);
    }
    if (s_last_seq) {
        const uint8_t t = s_last_msg[0] & 0xF0u;
        snprintf(line, sizeof line, "LAST  %s %d  %d  CH %d", t == 0x90u ? "NOTE ON" : t == 0x80u ? "NOTE OFF" : t == 0xB0u ? "CC" : "MSG",
                 s_last_msg[1], s_last_msg[2], (s_last_msg[0] & 15) + 1);
        tab5_text_draw(s_buf, 480, 480, 16, 262, line, C_DIM_TEXT, 0);
    } else {
        tab5_text_draw(s_buf, 480, 480, 16, 262, "LAST  NO MIDI RECEIVED YET", C_DIM_TEXT, 0);
    }
    if (s_out_level >= 0) {
        snprintf(line, sizeof line, "OUTPUT LEVEL %d/12", s_out_level);
        tab5_text_draw(s_buf, 480, 480, 262 - 8, 296 + 6, line, C_TEXT, 0);
        tab5_text_draw(s_buf, 480, 480, 262 - 8, 296 + 28, s_out_hp ? "HEADPHONES" : "SPEAKER", s_out_hp ? C_CYAN : C_DOWN, 0);
    }
    for (i = 0; i < NSOFT; i++) {
        int dim = (i == S_MODE || i == S_CLEAR) && s_sel < 0;
        rrect(480, 480, SOFT[i].x - 8, SOFT[i].y - 8, SOFT[i].w, SOFT[i].h, 12,
              dim ? RGB(30, 34, 46) : i == S_EXIT ? RGB(60, 90, 140) : (i == S_TONE || i == S_VOLDN || i == S_VOLUP) ? RGB(40, 110, 80) : C_BTN);
        text_centered(480, 480, SOFT[i].x - 8 + SOFT[i].w / 2, SOFT[i].y - 8 + SOFT[i].h / 2 - 8, SOFT[i].label, dim ? C_DIM_TEXT : C_TEXT);
    }
    s_blit(SCR_X, SCR_Y, 480, 480, s_buf);
}

static void clear_outputs(void)
{
    memset(s_mbtn, 0, sizeof s_mbtn);
    memset(s_mkey, 0, sizeof s_mkey);
}

static void set_learn(int on)
{
    int i;
    for (i = 0; i < NC; i++)                           /* fingers already down stay down but hold nothing across the switch */
        if (C[i].active) {                             /* (forgetting them would make the same finger count as a new touch) */
            C[i].kind = T_NONE;
            C[i].other_used = 1;
        }
    clear_outputs();
    s_learn = on;
    s_sel = -1;
    s_learn_sig = 0xFFFFFFFFu;
    s_screen_draws = 0xFFFFFFFFu;                      /* the firmware's screen comes back when learning ends */
    memset(s_sig_btn, 0xFF, sizeof s_sig_btn);
    memset(s_sig_key, 0xFF, sizeof s_sig_key);
    memset(s_sig_knob, 0xFF, sizeof s_sig_knob);
}

/* ---- the map's storage: 'M' 'L' 1 <NT> then NT x 4 bytes ------------------------------------------------------- */
void panel_midi_map_export(uint8_t *blob, size_t *len)
{
    int t;
    blob[0] = 'M'; blob[1] = 'L'; blob[2] = 1; blob[3] = (uint8_t)NT;
    for (t = 0; t < NT; t++) {
        blob[4 + t * 4] = s_map[t].type;
        blob[5 + t * 4] = s_map[t].ch;
        blob[6 + t * 4] = s_map[t].num;
        blob[7 + t * 4] = s_map[t].mode;
    }
    *len = 4 + NT * 4;
}
int panel_midi_map_import(const uint8_t *blob, size_t len)
{
    int t;
    if (len != 4 + (size_t)NT * 4 || blob[0] != 'M' || blob[1] != 'L' || blob[2] != 1 || blob[3] != NT)
        return -1;
    for (t = 0; t < NT; t++) {
        s_map[t].type = blob[4 + t * 4] <= SRC_CC ? blob[4 + t * 4] : 0;
        s_map[t].ch = blob[5 + t * 4] & 15u;
        s_map[t].num = blob[6 + t * 4] & 127u;
        s_map[t].mode = blob[7 + t * 4] & 1u;
    }
    s_learn_sig = 0xFFFFFFFFu;
    return 0;
}
void panel_midi_map_set_save_cb(void (*cb)(const uint8_t *blob, size_t len)) { s_save_cb = cb; }
void panel_midi_set_devices(int n) { s_ndev = n; }
int panel_midi_learning(void) { return s_learn; }
int panel_midi_mapped_count(void)
{
    int t, n = 0;
    for (t = 0; t < NT; t++)
        n += s_map[t].type != SRC_NONE;
    return n;
}

static void learn_assign(int t, int type, int ch, int num)
{
    int i;
    for (i = 0; i < NT; i++)                           /* one source answers one control */
        if (s_map[i].type == type && s_map[i].ch == ch && s_map[i].num == num)
            memset(&s_map[i], 0, sizeof s_map[i]);
    s_map[t].type = (uint8_t)type;
    s_map[t].ch = (uint8_t)ch;
    s_map[t].num = (uint8_t)num;
    s_map[t].mode = 0;
    s_abs_last[t >= 14 && t < 14 + NKNOB ? t - 14 : 0] = -1;
    map_changed();
}

/* push what is held (fingers, locks, MIDI sources) to the firmware */
static void push_outputs(void)
{
    uint32_t keys = 0, btns = 0;
    int j, i;
    if (!s_learn) {
        for (j = 0; j < NC; j++) {
            if (!C[j].active)
                continue;
            if (C[j].kind == T_KEY)
                keys |= 1u << C[j].idx;
            else if (C[j].kind == T_BTN && C[j].idx < TAB5_NBUTTONS)
                btns |= 1u << C[j].idx;
        }
        for (i = 0; i < TAB5_NBUTTONS; i++)
            if (s_latched[i] || s_mbtn[i])
                btns |= 1u << i;
        for (i = 0; i < TAB5_NKEYS; i++)
            if (s_mkey[i])
                keys |= 1u << i;
    }
    tab5_keys(keys);
    tab5_buttons(btns);
}

static void knob_step(int k, int steps)                /* a MIDI-driven turn: same effect as a drag */
{
    if (!steps)
        return;
    tab5_enc((uint32_t)k, steps);
    s_angle[k] = (int16_t)(((s_angle[k] + steps * 12) % 360 + 360) % 360);
}

/* a mapped source arrived: type, value (note on = velocity, note off = 0, CC = value) */
static void midi_apply(int t, int type, int value)
{
    if (t < 14 + NKNOB && t >= 14) {                   /* a knob: CC only */
        int k = t - 14, v = value;
        if (type != SRC_CC)
            return;
        if (k == K_MASTER) {
            if (s_map[t].mode) {                       /* relative */
                int d = v < 64 ? v : v > 64 ? v - 128 : 0;
                int m = s_master + d * 8;
                s_master = (int16_t)(m < 0 ? 0 : m > 1023 ? 1023 : m);
            } else {
                s_master = (int16_t)(v * 1023 / 127);
            }
            tab5_master((uint32_t)s_master);
            return;
        }
        if (s_map[t].mode) {                           /* relative encoder: 1..63 up, 65..127 down */
            knob_step(k, v < 64 ? v : v > 64 ? v - 128 : 0);
        } else {                                       /* absolute pot: one step per two CC units */
            if (s_abs_last[k] >= 0) {
                s_abs_acc[k] = (int16_t)(s_abs_acc[k] + v - s_abs_last[k]);
                knob_step(k, s_abs_acc[k] / 2);
                s_abs_acc[k] = (int16_t)(s_abs_acc[k] % 2);
            }
            s_abs_last[k] = (int16_t)v;
        }
        return;
    }
    {
        int on = type == SRC_NOTE ? value > 0 : value >= 64;
        if (t < 14) {                                  /* a button */
            if (s_map[t].mode) {                       /* toggle: each press locks / unlocks it */
                if (on)
                    s_latched[t] = (uint8_t)!s_latched[t];
            } else {
                s_mbtn[t] = (uint8_t)on;
            }
        } else {
            s_mkey[t - 14 - NKNOB] = (uint8_t)on;
        }
    }
}

void panel_midi_in(uint32_t pkt)
{
    uint32_t cin = pkt & 15u, st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = (pkt >> 24) & 0x7Fu;
    int type = SRC_NONE, ch = (int)(st & 15u), t;
    if (cin == 9u && (st >> 4) == 9u)
        type = SRC_NOTE;
    else if (cin == 8u && (st >> 4) == 8u)
        type = SRC_NOTE, d2 = 0;                       /* note off = velocity 0 */
    else if (cin == 0xBu && (st >> 4) == 0xBu)
        type = SRC_CC;
    if (type == SRC_NONE) {                            /* not mappable (program change, bend, clock, SysEx ...) */
        if (!s_learn)
            tab5_midi_in(pkt);
        return;
    }
    s_last_msg[0] = (uint8_t)st;
    s_last_msg[1] = (uint8_t)d1;
    s_last_msg[2] = (uint8_t)d2;
    s_last_seq++;
    if (s_learn) {                                     /* learning: nothing reaches the firmware */
        if (s_sel >= 0 && !(target_is_knob(s_sel) && type == SRC_NOTE) && (type == SRC_CC || d2 > 0))
            learn_assign(s_sel, type, ch, (int)d1);
        s_learn_sig = 0xFFFFFFFFu;
        return;
    }
    for (t = 0; t < NT; t++)
        if (s_map[t].type == type && s_map[t].ch == ch && s_map[t].num == d1) {
            midi_apply(t, type, (int)d2);
            push_outputs();
            return;
        }
    tab5_midi_in(pkt);                                 /* unmapped: it plays the tracks as usual */
}

/* -------------------------------------------------------------------------------------- the API --- */
void panel_init(uint16_t *scratch, size_t scratch_px, panel_blit_fn blit)
{
    s_buf = scratch;
    s_buf_px = scratch_px;
    s_blit = blit;
    memset(C, 0, sizeof C);
    memset(s_latched, 0, sizeof s_latched);
    memset(s_sig_btn, 0xFF, sizeof s_sig_btn);
    memset(s_sig_key, 0xFF, sizeof s_sig_key);
    memset(s_sig_knob, 0xFF, sizeof s_sig_knob);
    memset(s_abs_last, 0xFF, sizeof s_abs_last);       /* -1: no absolute value seen yet */
    memset(s_abs_acc, 0, sizeof s_abs_acc);
    clear_outputs();
    s_screen_draws = 0xFFFFFFFFu;
    s_learn_sig = 0xFFFFFFFFu;
    s_drawn_bg = 0;
    s_learn = 0;
    s_sel = -1;
    tab5_master((uint32_t)s_master);
}

void panel_render(void)
{
    int i;
    if (!s_buf || !s_blit)
        return;
    if (!s_drawn_bg) {
        s_drawn_bg = 1;
        fill(1280, 144, C_BLACK);                     /* 1280 x 144 stripes: fits the 480 x 480 scratch */
        for (i = 0; i < 720; i += 144)
            s_blit(0, i, 1280, 144, s_buf);
    }
    if (s_learn)
        draw_learn_page();
    else
        draw_screen();
    for (i = 0; i < NBTN; i++)
        draw_button(i);
    for (i = 0; i < NKNOB; i++)
        draw_knob(i);
    for (i = 0; i < TAB5_NKEYS; i++)
        draw_key(i);
}

uint32_t panel_latched_buttons(void)
{
    uint32_t m = 0;
    int b;
    for (b = 0; b < TAB5_NBUTTONS; b++)
        m |= (uint32_t)s_latched[b] << b;
    return m;
}

static void other_used(void)                          /* something else was touched: a held button is a modifier now */
{
    int i;
    for (i = 0; i < NC; i++)
        if (C[i].active && C[i].kind == T_BTN)
            C[i].other_used = 1;
}

static void knob_drag(contact_t *c, int y)
{
    int k = c->idx, dy = c->y - y;                    /* up = clockwise */
    c->y = (int16_t)y;
    if (k == K_MASTER) {
        int v = s_master + dy * 1023 / MASTER_RANGE;
        s_master = (int16_t)(v < 0 ? 0 : v > 1023 ? 1023 : v);
        tab5_master((uint32_t)s_master);
        return;
    }
    c->drag += dy;
    while (c->drag >= PX_PER_STEP || c->drag <= -PX_PER_STEP) {
        int s = c->drag > 0 ? 1 : -1;
        c->drag -= s * PX_PER_STEP;
        knob_step(k, s);
        other_used();
    }
}

/* a touch in learn mode: pick a target, or press one of the page's own buttons */
static void learn_touch(int x, int y)
{
    int i, kind, idx;
    for (i = 0; i < NSOFT; i++)
        if (x >= SOFT[i].x && x < SOFT[i].x + SOFT[i].w && y >= SOFT[i].y && y < SOFT[i].y + SOFT[i].h) {
            if (i == S_EXIT) {
                set_learn(0);
            } else if (i == S_TONE) {
                if (s_tone_cb)
                    s_tone_cb();
            } else if (i == S_VOLDN || i == S_VOLUP) {
                if (s_level_cb)
                    s_level_cb(i == S_VOLUP ? 1 : -1);
            } else if (i == S_CLEAR_ALL) {
                memset(s_map, 0, sizeof s_map);
                map_changed();
            } else if (s_sel >= 0) {
                if (i == S_CLEAR)
                    memset(&s_map[s_sel], 0, sizeof s_map[s_sel]);
                else if (s_map[s_sel].type != SRC_NONE)
                    s_map[s_sel].mode ^= 1u;
                map_changed();
            }
            s_learn_sig = 0xFFFFFFFFu;
            return;
        }
    if (hit(x, y, &kind, &idx)) {
        int t = kind == T_BTN ? T_OF_BTN(idx) : kind == T_KNOB ? T_OF_KNOB(idx) : T_OF_KEY(idx);
        if (kind == T_BTN && idx == B_MIDI) {
            set_learn(0);
            return;
        }
        s_sel = t;
        s_learn_sig = 0xFFFFFFFFu;
        memset(s_sig_btn, 0xFF, sizeof s_sig_btn);     /* the frames move: redraw the marks */
        memset(s_sig_key, 0xFF, sizeof s_sig_key);
        memset(s_sig_knob, 0xFF, sizeof s_sig_knob);
    }
}

void panel_touch(const panel_pt *pts, int n, uint32_t now_ms)
{
    uint8_t seen[NC] = {0};
    int i, j;
    for (i = 0; i < n && i < 5; i++) {
        int slot = -1;
        for (j = 0; j < NC; j++)
            if (C[j].active && C[j].id == pts[i].id)
                slot = j;
        if (slot < 0) {                                /* a new finger */
            int kind, idx;
            for (j = 0; j < NC && slot < 0; j++)
                if (!C[j].active)
                    slot = j;
            if (slot < 0)
                continue;
            memset(&C[slot], 0, sizeof C[slot]);
            C[slot].active = 1;
            C[slot].id = pts[i].id;
            C[slot].x = pts[i].x;
            C[slot].y = pts[i].y;
            C[slot].t_down = now_ms;
            if (s_learn) {
                learn_touch(pts[i].x, pts[i].y);       /* a tap: nothing is held while learning */
                C[slot].kind = T_NONE;
            } else if (hit(pts[i].x, pts[i].y, &kind, &idx)) {
                if (kind == T_BTN && idx == B_MIDI) {  /* the MIDI button: learn mode on */
                    set_learn(1);
                    C[slot].kind = T_NONE;
                    seen[slot] = 1;
                    continue;
                }
                C[slot].kind = (uint8_t)kind;
                C[slot].idx = (uint8_t)idx;
                if (kind != T_BTN) {
                    other_used();
                } else {
                    for (j = 0; j < NC; j++)           /* two buttons at once: both are modifiers, neither locks */
                        if (j != slot && C[j].active && C[j].kind == T_BTN) {
                            C[j].other_used = 1;
                            C[slot].other_used = 1;
                        }
                    if (s_latched[idx] && !C[slot].other_used)
                        C[slot].other_used = 2;        /* this touch will release the lock when it lifts */
                }
            } else {
                C[slot].kind = T_NONE;
            }
        } else if (C[slot].kind == T_KNOB) {
            knob_drag(&C[slot], pts[i].y);
        } else if (C[slot].kind == T_KEY && !s_learn) {   /* a glissando: sliding onto another key plays that key */
            int kind, idx;
            C[slot].x = pts[i].x;
            C[slot].y = pts[i].y;
            if (hit(pts[i].x, pts[i].y, &kind, &idx) && kind == T_KEY)
                C[slot].idx = (uint8_t)idx;            /* (off the keys, or on a button: the finger keeps its key) */
        }
        seen[slot] = 1;
    }
    for (j = 0; j < NC; j++) {
        contact_t *c = &C[j];
        if (!c->active || seen[j])
            continue;
        if (c->kind == T_BTN) {                        /* the finger lifts */
            int b = c->idx;
            if (c->other_used == 2)
                s_latched[b] = 0;
            else if (!s_latched[b] && !c->other_used && now_ms - c->t_down >= LATCH_MS)
                s_latched[b] = 1;
        }
        c->active = 0;
    }
    push_outputs();
}
