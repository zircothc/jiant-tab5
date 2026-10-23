/* SPDX-License-Identifier: GPL-3.0-only
 * Host test of panel_ui.c against the real firmware: scripted fingers, and a picture of the panel. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "tab5_platform.h"
#include "panel_ui.h"

static uint16_t fb[1280 * 720];                       /* what the display would show (landscape, native RGB565) */
static uint16_t scratch[480 * 480];
static int blits;
static uint8_t nor[0x100000];
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
                           else { printf("  ok:   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void blit(int x, int y, int w, int h, const uint16_t *px)
{
    int i, j;
    blits++;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            if (x + i < 1280 && y + j < 720)
                fb[(y + j) * 1280 + x + i] = px[j * w + i];
}
static panel_pt P[5];
static int NP;
static void tick(int ms)                              /* the device runs, fingers are polled every 5 ms, screen every 33 */
{
    static uint32_t t;
    while (ms--) {
        tab5_step_ms();
        t++;
        if (t % 5 == 0) panel_touch(P, NP, tab5_now_ms());
        if (t % 33 == 0) panel_render();
    }
}
static void down(int slot, int id, int x, int y) { P[slot].id = (uint8_t)id; P[slot].x = (int16_t)x; P[slot].y = (int16_t)y; if (slot >= NP) NP = slot + 1; }
static void up(int slot) { for (int i = slot; i < NP - 1; i++) P[i] = P[i + 1]; if (NP > 0) NP--; }
static void lift_all(void) { NP = 0; }

static uint32_t screen_hash(void) { const uint16_t *f = tab5_screen(); uint32_t h = 2166136261u; for (int i = 0; i < 240 * 240; i++) { h ^= f[i]; h *= 16777619u; } return h; }
static int dom_hz(void)
{
    static int32_t st[2 * 8820]; static double m[8820];
    tab5_audio_read(st, 8820);
    for (int i = 0; i < 8820; i++) m[i] = st[2 * i] / 8388608.0;
    double best = 0; int bf = 0;
    for (int f = 40; f < 3000; f += 2) { double re = 0, im = 0; for (int i = 0; i < 8820; i += 2) { double a = 2 * M_PI * f * i / 44100.0; re += m[i] * cos(a); im += m[i] * sin(a); } double p = re * re + im * im; if (p > best) { best = p; bf = f; } }
    return bf;
}
static float peak_now(void)
{
    static int32_t st[2 * 4410]; float pk = 0;
    tab5_audio_read(st, 4410);
    for (int i = 0; i < 4410; i++) { float v = fabsf(st[2 * i] / 8388608.0f); if (v > pk) pk = v; }
    return pk;
}
static void save(const char *path)
{
    FILE *f = fopen(path, "wb"); fprintf(f, "P6\n1280 720\n255\n");
    for (int i = 0; i < 1280 * 720; i++) { uint16_t p = fb[i]; uint8_t c[3] = {(uint8_t)(((p >> 11) & 31) * 255 / 31), (uint8_t)(((p >> 5) & 63) * 255 / 63), (uint8_t)((p & 31) * 255 / 31)}; fwrite(c, 1, 3, f); }
    fclose(f);
}
/* centres of things (landscape) */
#define BTN(b) (b < 12 ? 520 + (b % 6) * 124 + 58 : 984 + (b - 12) * 96 + 44), (b < 12 ? 24 + (b / 6) * 96 + 44 : 386 + 44)
#define KEYC(i) ((int)(28 + ((i) + 0.5f) * 76.5f)), 660     /* the i-th white-key column (lower row) */

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    char path[512];
    uint32_t h0, h1, h2;
    int f, f0;
    memset(nor, 0xFF, sizeof nor);
    tab5_set_nor(nor);
    tab5_power_on();
    panel_init(scratch, 480 * 480, blit);
    tab5_test_pitched_track();                                       /* JIANT starts with a DRUM-X track: a held voice instead */
    tick(3200);                                                      /* JIANT's splash lasts ~2.3 s */
    printf("panel drawn: %d rectangles\n", blits);
    snprintf(path, sizeof path, "%s/p01-idle.ppm", dir); save(path);

    printf("octave buttons + a key (white column 4 = key 7)\n");
    down(0, 1, KEYC(4)); tick(300);
    f0 = dom_hz();
    lift_all(); tick(2500);
    down(0, 1, BTN(13)); tick(100); lift_all(); tick(100);
    down(0, 1, BTN(13)); tick(100); lift_all(); tick(100);
    down(0, 1, KEYC(4)); tick(300);
    f = dom_hz();
    CHECK(f > f0 * 18 / 10 && f < f0 * 22 / 10, "OCT+ twice then key 7 sounds at %d Hz (it was %d Hz: an octave up)", f, f0);
    snprintf(path, sizeof path, "%s/p02-key-held.ppm", dir); save(path);
    lift_all(); tick(2500);

    printf("hold a button with one finger\n");
    h0 = screen_hash();
    down(0, 2, BTN(TAB5_B_SEQ)); tick(300); h1 = screen_hash();
    CHECK(tab5_held_buttons() == (1u << TAB5_B_SEQ), "holding SEQ: the firmware sees SEQ held (mask 0x%x)", tab5_held_buttons());
    lift_all(); tick(300); h2 = screen_hash();
    (void)h2; (void)h0;       /* (a tap on SEQ also changes the firmware's page: that is the firmware, not the panel) */
    CHECK(panel_latched_buttons() == 0, "a short hold (0.3 s) releases normally, nothing locked");

    printf("long press alone locks the button\n");
    down(0, 2, BTN(TAB5_B_SEQ)); tick(800); lift_all(); tick(300);
    CHECK(panel_latched_buttons() == (1u << TAB5_B_SEQ), "SEQ is locked after 0.8 s alone (mask 0x%x)", panel_latched_buttons());
    CHECK(tab5_held_buttons() == (1u << TAB5_B_SEQ), "and the firmware keeps seeing SEQ held, the finger is up (mask 0x%x)", tab5_held_buttons());
    snprintf(path, sizeof path, "%s/p03-seq-locked.ppm", dir); save(path);
    down(0, 3, BTN(TAB5_B_SEQ)); tick(100); lift_all(); tick(300);
    CHECK(panel_latched_buttons() == 0 && tab5_held_buttons() == 0, "a tap on it unlocks it and SEQ is released");
    down(0, 3, BTN(TAB5_B_HOME)); tick(100); lift_all(); tick(300);       /* (the SEQ page's keys edit steps: back to HOME) */

    printf("two fingers: hold SEQ, tap a key (a step)\n");
    CHECK(tab5_pattern_steps() == 0, "no steps yet");
    down(0, 4, BTN(TAB5_B_SEQ)); tick(700);               /* held a long time, but another finger will be used */
    down(1, 5, KEYC(0)); tick(100);
    CHECK(tab5_held_buttons() == (1u << TAB5_B_SEQ) && (tab5_held_keys() & 1u), "the key reaches the firmware while SEQ is held (0x%x, keys 0x%x)", tab5_held_buttons(), tab5_held_keys());
    up(1); tick(100);
    lift_all(); tick(300);
    down(0, 3, BTN(TAB5_B_HOME)); tick(100); lift_all(); tick(300);
    CHECK(panel_latched_buttons() == 0, "and SEQ did NOT lock, because another control was used while holding it");

    printf("slide one finger across keys\n");
    down(0, 20, KEYC(4)); tick(200);
    CHECK(tab5_held_keys() == (1u << 7), "finger on key 7: only key 7 held (0x%x)", tab5_held_keys());
    down(0, 20, KEYC(5)); tick(100);
    CHECK(tab5_held_keys() == (1u << 9), "sliding to the next white key: now key 9, key 7 released (0x%x)", tab5_held_keys());
    down(0, 20, 640, 360); tick(100);
    CHECK(tab5_held_keys() == (1u << 9), "sliding off the keys: the finger keeps key 9 (0x%x)", tab5_held_keys());
    lift_all(); tick(300);
    CHECK(tab5_held_keys() == 0, "lifted: nothing held");

    printf("knobs\n");
    h0 = screen_hash();
    down(0, 6, 600, 300); tick(20);
    for (int y = 300; y > 240; y -= 4) { down(0, 6, 600, y); tick(10); }
    lift_all(); tick(200);
    CHECK(screen_hash() != h0, "dragging KNOB 1 up 60 px changes the firmware's screen");
    snprintf(path, sizeof path, "%s/p04-after-knob.ppm", dir); save(path);

    printf("MASTER\n");
    down(0, 7, 554, 430); tick(20);
    for (int y = 430; y < 800 - 1; y += 10) { down(0, 7, 554, y); tick(5); }
    lift_all(); tick(200);
    down(0, 8, KEYC(4)); tick(300);
    CHECK(peak_now() < 0.01f, "MASTER dragged right down: silent");
    lift_all(); tick(2000);
    down(0, 7, 554, 430); tick(20);
    for (int y = 430; y > 40; y -= 10) { down(0, 7, 554, y); tick(5); }
    lift_all(); tick(200);
    down(0, 8, KEYC(4)); tick(300);
    CHECK(peak_now() > 0.02f, "MASTER dragged back up: sound again");
    lift_all(); tick(2000);

    printf("a chord with three fingers while a button is held\n");
    down(0, 9, BTN(TAB5_B_OCTDN)); tick(50);              /* a button held while the chord is played */
    down(1, 10, KEYC(4)); down(2, 11, KEYC(6)); down(3, 12, KEYC(8)); tick(300);
    CHECK(peak_now() > 0.02f, "three keys + a held button at once: sound");
    snprintf(path, sizeof path, "%s/p05-chord.ppm", dir); save(path);
    lift_all(); tick(500);

    printf("%s (%d failed)\n", fails ? "FAILED" : "ALL OK", fails);
    return fails != 0;
}
