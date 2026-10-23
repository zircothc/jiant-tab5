/* SPDX-License-Identifier: GPL-3.0-only
 * Host test: USB-MIDI packets and MIDI learn, on the real firmware, with scripted fingers. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "tab5_platform.h"
#include "panel_ui.h"

static uint16_t fb[1280 * 720], scratch[480 * 480];
static uint8_t nor[0x100000];
static int fails, saves, tones, lvl;
static void on_tone(void) { tones++; }
static void on_level(int d) { lvl += d; }
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
                           else { printf("  ok:   "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void blit(int x, int y, int w, int h, const uint16_t *px)
{ for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) if (x + i < 1280 && y + j < 720) fb[(y + j) * 1280 + x + i] = px[j * w + i]; }
static panel_pt P[5]; static int NP;
static void tick(int ms)
{
    static uint32_t t;
    while (ms--) { tab5_step_ms(); t++; if (t % 5 == 0) panel_touch(P, NP, tab5_now_ms()); if (t % 33 == 0) panel_render(); }
}
static void down(int slot, int id, int x, int y) { P[slot].id = (uint8_t)id; P[slot].x = (int16_t)x; P[slot].y = (int16_t)y; if (slot >= NP) NP = slot + 1; }
static void lift_all(void) { NP = 0; }
static void tap(int x, int y) { down(0, 1, x, y); tick(80); lift_all(); tick(120); }
static uint32_t pkt(int st, int d1, int d2) { return (uint32_t)(st >> 4) | (uint32_t)st << 8 | (uint32_t)d1 << 16 | (uint32_t)d2 << 24; }
static void midi(int st, int d1, int d2) { panel_midi_in(pkt(st, d1, d2)); tick(10); }
static uint32_t screen_hash(void) { const uint16_t *f = tab5_screen(); uint32_t h = 2166136261u; for (int i = 0; i < 240 * 240; i++) { h ^= f[i]; h *= 16777619u; } return h; }
static uint32_t fb_hash(int x, int y, int w, int h) { uint32_t r = 2166136261u; for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) { r ^= fb[(y + j) * 1280 + x + i]; r *= 16777619u; } return r; }
static float peak_now(int *hz)
{
    static int32_t st[2 * 8820]; static double m[8820]; float pk = 0;
    tab5_audio_read(st, 8820);
    for (int i = 0; i < 8820; i++) { m[i] = st[2 * i] / 8388608.0; if (fabs(m[i]) > pk) pk = (float)fabs(m[i]); }
    if (hz) { double best = 0; int bf = 0; for (int f = 40; f < 3000; f += 2) { double re = 0, im = 0; for (int i = 0; i < 8820; i += 2) { double a = 2 * M_PI * f * i / 44100.0; re += m[i] * cos(a); im += m[i] * sin(a); } double p = re * re + im * im; if (p > best) { best = p; bf = f; } } *hz = bf; }
    return pk;
}
static void save(const char *dir, const char *name)
{
    char path[512]; snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb"); fprintf(f, "P6\n1280 720\n255\n");
    for (int i = 0; i < 1280 * 720; i++) { uint16_t p = fb[i]; uint8_t c[3] = {(uint8_t)(((p >> 11) & 31) * 255 / 31), (uint8_t)(((p >> 5) & 63) * 255 / 63), (uint8_t)((p & 31) * 255 / 31)}; fwrite(c, 1, 3, f); }
    fclose(f);
}
static void on_save(const uint8_t *blob, size_t len) { (void)blob; (void)len; saves++; }

#define BTNC(b) (b < 12 ? 520 + (b % 6) * 124 + 58 : 984 + (b - 12) * 96 + 44), (b < 12 ? 24 + (b / 6) * 96 + 44 : 386 + 44)
#define KEYC(i) ((int)(28 + ((i) + 0.5f) * 76.5f)), 660
#define MIDI_BTN BTNC(14)
#define SOFT_MODE 126, 394
#define SOFT_CLEAR 370, 394
#define SOFT_CLEAR_ALL 126, 458
#define SOFT_EXIT 370, 458
#define SOFT_LVL_DN 66, 328
#define SOFT_LVL_UP 190, 328
#define KNOB_K1 600, 292

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    uint32_t h_idle, h_held, h;
    int hz;
    float pk;
    memset(nor, 0xFF, sizeof nor);
    tab5_set_nor(nor);
    tab5_power_on();
    panel_init(scratch, 480 * 480, blit);
    panel_midi_map_set_save_cb(on_save);
    panel_set_tone_cb(on_tone);
    panel_set_level_cb(on_level);
    panel_set_audio_info(4, 0);
    tab5_test_pitched_track();                                       /* JIANT starts with a DRUM-X track: a held voice instead */
    tick(3200);                                                      /* JIANT's splash lasts ~2.3 s */

    printf("an unmapped keyboard plays the tracks\n");
    midi(0x90, 60, 100); tick(200);
    pk = peak_now(&hz);
    CHECK(pk > 0.02f && hz > 225 && hz < 600, "note 60 sounds at %d Hz (peak %.3f)", hz, pk);
    midi(0x80, 60, 0); tick(2500);
    CHECK(peak_now(NULL) < 0.01f, "and stops with the note off");

    printf("learn mode\n");
    h_idle = screen_hash();
    tap(MIDI_BTN);
    CHECK(panel_midi_learning() == 1, "the MIDI button switches learn mode on");
    tap(MIDI_BTN); tick(200);
    CHECK(panel_midi_learning() == 0, "tapping it again leaves learn mode and it stays left (the finger is not re-read as a new touch)");
    tap(MIDI_BTN);
    CHECK(panel_midi_learning() == 1, "and once more it is on");
    save(dir, "m01-learn-empty.ppm");
    midi(0x90, 62, 100); tick(200);
    CHECK(peak_now(NULL) < 0.01f, "notes do not play while learning");
    midi(0x80, 62, 0);

    tap(432, 40);
    CHECK(tones == 1, "the TONE button on the learn page asks the app for a test tone");
    tap(SOFT_LVL_UP); tap(SOFT_LVL_UP); tap(SOFT_LVL_DN);
    CHECK(lvl == 1, "LEVEL + + - on the learn page asks the app for net +1 (%d)", lvl);
    panel_set_audio_info(5, 1); tick(100);
    save(dir, "m00-learn-audio.ppm");

    printf("a button from a CC (momentary)\n");
    tap(BTNC(TAB5_B_FX));
    midi(0xB0, 21, 127);
    CHECK(panel_midi_mapped_count() == 1 && saves >= 1, "FX <- CC 21 learnt (%d mapped, saved %d times)", panel_midi_mapped_count(), saves);
    save(dir, "m02-learn-fx.ppm");
    tap(SOFT_EXIT);
    CHECK(panel_midi_learning() == 0, "EXIT leaves learn mode");
    tick(100);
    h = screen_hash();
    down(0, 1, BTNC(TAB5_B_FX)); tick(300); h_held = screen_hash(); lift_all(); tick(400);   /* what a finger does */
    midi(0xB0, 21, 127); tick(300);
    CHECK(tab5_held_buttons() == (1u << TAB5_B_FX), "CC 21 = 127 holds FX exactly like a finger does (mask 0x%x)", tab5_held_buttons());
    midi(0xB0, 21, 0); tick(300);

    printf("a knob from a CC (absolute)\n");
    tap(MIDI_BTN);
    tap(KNOB_K1);
    midi(0xB0, 74, 20);
    CHECK(panel_midi_mapped_count() == 2, "KNOB 1 <- CC 74 learnt");
    tap(SOFT_EXIT);
    {
        int32_t e0 = tab5_enc_total(TAB5_E_K1);
        midi(0xB0, 74, 20); tick(100);
        CHECK(tab5_enc_total(TAB5_E_K1) == e0, "the first value only sets the baseline: nothing jumps");
    }
    midi(0xB0, 74, 50); midi(0xB0, 74, 80); tick(200);
    CHECK(screen_hash() != h, "turning the controller moves KNOB 1 in the firmware (%d CC units)", 60);

    printf("a pad as a toggle (lock) for SEQ, from a note\n");
    tap(MIDI_BTN);
    tap(BTNC(TAB5_B_SEQ));
    midi(0x90, 36, 100); midi(0x80, 36, 0);
    CHECK(panel_midi_mapped_count() == 3, "SEQ <- NOTE 36 learnt");
    tap(SOFT_MODE);
    save(dir, "m03-learn-toggle.ppm");
    tap(SOFT_EXIT);
    midi(0x90, 36, 100); tick(100); midi(0x80, 36, 0); tick(300);
    CHECK(panel_latched_buttons() == (1u << TAB5_B_SEQ), "pad press 1 locks SEQ (the pad's release is ignored)");
    CHECK(peak_now(NULL) < 0.01f, "a mapped note does not also play the synth");
    midi(0x90, 36, 100); tick(100); midi(0x80, 36, 0); tick(300);
    CHECK(panel_latched_buttons() == 0, "pad press 2 unlocks it");

    printf("a key from a note: step entry\n");
    tap(MIDI_BTN);
    tap(KEYC(0));
    midi(0x90, 40, 100); midi(0x80, 40, 0);
    CHECK(panel_midi_mapped_count() == 4, "key 0 <- NOTE 40 learnt");
    tap(SOFT_EXIT);
    CHECK(tab5_pattern_steps() == 0, "no steps yet");
    midi(0x90, 36, 100); midi(0x80, 36, 0); tick(300);          /* lock SEQ */
    midi(0x90, 40, 100); tick(100); midi(0x80, 40, 0); tick(300);
    midi(0x90, 40, 100); tick(100);
    CHECK(tab5_held_keys() & 1u, "pad 40 acts as key 0: the firmware sees key 0 held (0x%x)", tab5_held_keys());
    midi(0x80, 40, 0); tick(100);
    midi(0x90, 36, 100); midi(0x80, 36, 0); tick(300);          /* unlock */

    printf("persistence\n");
    {
        uint8_t blob[256]; size_t len;
        panel_midi_map_export(blob, &len);
        panel_init(scratch, 480 * 480, blit);                   /* "a reboot": the panel starts from nothing ... */
        panel_midi_map_set_save_cb(on_save);
        tick(100);
        CHECK(panel_midi_mapped_count() == 4, "(the map survives a panel_init in RAM: it is the app's to keep)");
        CHECK(panel_midi_map_import(blob, len) == 0 && panel_midi_mapped_count() == 4, "export -> import restores 4 mappings (%u bytes)", (unsigned)len);
        blob[0] = 'X';
        CHECK(panel_midi_map_import(blob, len) != 0, "a corrupt blob is refused");
    }
    h_idle = screen_hash();
    midi(0xB0, 21, 127); tick(300);
    CHECK(tab5_held_buttons() == (1u << TAB5_B_FX), "and the restored FX mapping works (mask 0x%x)", tab5_held_buttons());
    midi(0xB0, 21, 0); tick(300);

    printf("clear\n");
    tap(MIDI_BTN);
    tap(BTNC(TAB5_B_FX));
    tap(SOFT_CLEAR);
    CHECK(panel_midi_mapped_count() == 3, "CLEAR removes the selected one");
    tap(SOFT_CLEAR_ALL);
    CHECK(panel_midi_mapped_count() == 0, "CLEAR ALL removes everything");
    save(dir, "m04-learn-cleared.ppm");
    tap(SOFT_EXIT);
    midi(0xB0, 21, 127); tick(200);
    CHECK(panel_latched_buttons() == 0, "a cleared CC no longer does anything");
    midi(0xB0, 21, 0);

    printf("%s (%d failed)\n", fails ? "FAILED" : "ALL OK", fails);
    return fails != 0;
}
