/* SPDX-License-Identifier: GPL-3.0-only
 * PC harness for tab5_platform.c: boots SLOOP, plays the panel through the touch-UI API and checks that
 * audio, screen, LEDs and flash behave. Writes PPM screens and a WAV into the directory given as argv[1]. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "tab5_platform.h"

static const char *dir = ".";
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
                           else { printf("  ok:   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void run_ms(uint32_t ms) { while (ms--) tab5_step_ms(); }

static void dump_ppm(const char *name, uint32_t scale)           /* the Tab5 scales the 240 x 240 screen like this */
{
    char path[512];
    const uint16_t *fb = tab5_screen();
    uint32_t x, y, sx, sy, w = 240u * scale;
    FILE *f;
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    f = fopen(path, "wb");
    fprintf(f, "P6\n%u %u\n255\n", w, w);
    for (y = 0; y < w; y++)
        for (x = 0; x < w; x++) {
            uint16_t p = fb[(y / scale) * 240u + x / scale];
            p = (uint16_t)((p >> 8) | (p << 8));                   /* the panel's byte order -> RGB565 */
            uint8_t rgb[3] = {(uint8_t)(((p >> 11) & 31u) * 255u / 31u), (uint8_t)(((p >> 5) & 63u) * 255u / 63u),
                              (uint8_t)((p & 31u) * 255u / 31u)};
            fwrite(rgb, 1, 3, f);
        }
    (void)sx; (void)sy;
    fclose(f);
}

/* render n ms of audio, return its peak (0..1) and the mono signal */
static float capture(int32_t *mono, uint32_t frames, float *rms)
{
    static int32_t st[48000 * 2];
    uint32_t i;
    double e = 0;
    float pk = 0;
    tab5_audio_read(st, frames);
    for (i = 0; i < frames; i++) {
        float l = st[2 * i] / 8388608.0f;
        if (mono) mono[i] = st[2 * i];
        if (fabsf(l) > pk) pk = fabsf(l);
        e += (double)l * l;
    }
    if (rms) *rms = (float)sqrt(e / frames);
    return pk;
}

static double pitch_hz(const int32_t *x, uint32_t n)             /* normalised autocorrelation, first strong peak */
{
    static double r[2048];
    uint32_t fs = tab5_sample_rate(), lag, lo = fs / 1500u, hi = fs / 60u, i, l0 = 0, bestl = 0;
    double e0 = 0, mx = 0;
    for (i = 0; i < n; i++) e0 += (double)x[i] / 8388608.0 * (x[i] / 8388608.0);
    if (e0 < 1e-9 || hi >= 2048u) return 0;
    for (lag = 1; lag < hi && lag < n / 2u; lag++) {
        double v = 0, en = 0;
        for (i = 0; i + lag < n; i++) { v += (double)x[i] / 8388608.0 * (x[i + lag] / 8388608.0); en += (double)x[i] / 8388608.0 * (x[i] / 8388608.0); }
        r[lag] = en > 0 ? v / en : 0;
    }
    for (lag = 1; lag < hi; lag++) if (r[lag] < 0) { l0 = lag; break; }       /* past the first zero crossing */
    if (!l0) return 0;
    for (lag = l0 > lo ? l0 : lo; lag < hi; lag++) if (r[lag] > mx) mx = r[lag];
    for (lag = l0 > lo ? l0 : lo; lag + 1 < hi; lag++)
        if (r[lag] >= 0.85 * mx && r[lag] >= r[lag - 1] && r[lag] >= r[lag + 1]) { bestl = lag; break; }
    return bestl ? (double)fs / bestl : 0;
}

static uint32_t hash_screen(void)
{
    const uint16_t *fb = tab5_screen();
    uint32_t h = 2166136261u, i;
    for (i = 0; i < 240u * 240u; i++) { h ^= fb[i]; h *= 16777619u; }
    return h;
}

static uint8_t host_nor[0x100000];

int main(int argc, char **argv)
{
    static int32_t sig[48000];
    float rms, pk;
    uint32_t h0, h1, draws0, i;
    FILE *wav;
    int32_t *rec;
    uint32_t rec_n = 0;

    int resume = argc > 2 && !strcmp(argv[2], "--resume");
    memset(host_nor, 0xFF, sizeof host_nor);
    tab5_set_nor(host_nor);
    if (argc > 1) dir = argv[1];
    {   /* the flash image of the previous run (the Tab5 keeps it in a partition) */
        char path[512];
        FILE *nf;
        snprintf(path, sizeof path, "%s/nor.bin", dir);
        if (resume && (nf = fopen(path, "rb"))) {
            if (fread(tab5_nor_image(), 1, tab5_nor_size(), nf) != tab5_nor_size()) { printf("short nor image\n"); return 2; }
            fclose(nf);
        }
    }
    if (resume) {
        tab5_power_on();
        run_ms(3000);
        dump_ppm("10-resumed", 2);
        printf("resume: boot from the saved flash image\n");
        CHECK(tab5_pattern_steps() == 3, "the three steps are back after a power cycle (%u)", tab5_pattern_steps());
        printf("%s (%d failed)\n", fails ? "FAILED" : "ALL OK", fails);
        return fails ? 1 : 0;
    }
    printf("SLOOP on the Tab5 platform layer (PC harness)\n");
    printf("  sample rate %u Hz, I2S half = %u frames\n", tab5_sample_rate(), tab5_half_frames());

    /* ---- boot ------------------------------------------------------------------------------------ */
    printf("boot\n");
    tab5_power_on();
    run_ms(60);
    dump_ppm("01-splash", 2);
    CHECK(tab5_screen_draws() > 0, "the splash was drawn (%u draws)", tab5_screen_draws());
    run_ms(1500);
    dump_ppm("02-home", 2);
    CHECK(tab5_now_ms() >= 1500u, "device clock runs (%u ms)", tab5_now_ms());
    CHECK(hash_screen() != 0, "home screen drawn");

    /* ---- silence before any key ------------------------------------------------------------------ */
    printf("audio\n");
    pk = capture(NULL, 4410, &rms);
    CHECK(pk < 0.01f, "silent before a key (peak %.5f)", pk);

    /* ---- keys: sound, release, and the pitch relations (track 1 starts in a low register of its own) ---- */
    h0 = hash_screen();
    {
        static const uint32_t keys[3] = {7, 9, 19};              /* a note, +2 semitones, +12 */
        double f[3];
        for (i = 0; i < 3; i++) {
            tab5_keys(1u << keys[i]);
            run_ms(400);
            pk = capture(sig, 8820, &rms);
            f[i] = pitch_hz(sig, 8820);
            if (i == 0) {
                CHECK(pk > 0.02f, "a key makes sound (peak %.3f, rms %.4f)", pk, rms);
                CHECK((tab5_lit_keys() >> keys[0]) & 1u, "the held key lights its LED (lit keys 0x%08x)", tab5_lit_keys());
                dump_ppm("03-key-held", 2);
            }
            tab5_keys(0);
            run_ms(2500);
            if (i == 0) {
                pk = capture(NULL, 4410, &rms);
                CHECK(pk < 0.02f, "silent again after release (peak %.4f)", pk);
            }
        }
        printf("  keys 7, 9, 19 sound at %.1f, %.1f, %.1f Hz\n", f[0], f[1], f[2]);
        /* (the default patch is detuned / chorused, so a single-note pitch estimate wanders by a few tenths of a
         * semitone: the octave is exact, the order between is checked) */
        CHECK(f[0] > 0 && f[1] > f[0] * 1.05 && f[1] < f[2], "key 9 sits above key 7 and below key 19 (ratio to key 7: %.3f)", f[1] / f[0]);
        CHECK(f[0] > 0 && fabs(f[2] / f[0] - 2.0) < 0.03, "key 19 is an octave above key 7 (ratio %.4f, want 2.0000)", f[2] / f[0]);
    }

    /* ---- the panel: buttons, knobs, the screen follows ------------------------------------------- */
    printf("panel\n");
    h0 = hash_screen();
    draws0 = tab5_screen_draws();
    tab5_buttons(1u << TAB5_B_SEQ);                                 /* hold SEQ: its layer opens */
    run_ms(300);
    CHECK(tab5_screen_draws() != draws0 && hash_screen() != h0, "holding SEQ redraws the screen");
    dump_ppm("04-seq-held", 2);
    CHECK((tab5_lit_buttons() >> TAB5_B_SEQ) & 1u, "the held SEQ button lights its LED (lit 0x%04x)", tab5_lit_buttons());
    tab5_buttons(0);
    run_ms(300);

    h1 = hash_screen();
    tab5_enc(TAB5_E_K1, 5);
    tab5_enc(TAB5_E_K1, 5);
    run_ms(200);
    CHECK(hash_screen() != h1, "turning KNOB 1 changes the screen");
    dump_ppm("05-knob1", 2);

    h1 = hash_screen();
    tab5_enc(TAB5_E_PRESETS, 1);
    run_ms(200);
    tab5_enc(TAB5_E_ALGORITHM, 1);
    run_ms(200);
    dump_ppm("06-preset-algo", 2);

    /* ---- edit a pattern: hold SEQ, tap three white keys = steps 1, 8, 13 ------------------------------ */
    printf("pattern + autosave\n");
    CHECK(tab5_pattern_steps() == 0, "the sequencer starts empty (%u steps)", tab5_pattern_steps());
    {
        static const uint32_t step_keys[3] = {0, 7, 14};
        tab5_buttons(1u << TAB5_B_SEQ);
        run_ms(120);
        for (i = 0; i < 3; i++) {
            tab5_keys(1u << step_keys[i]);
            run_ms(80);
            tab5_keys(0);
            run_ms(80);
        }
        tab5_buttons(0);
        run_ms(200);
    }
    CHECK(tab5_pattern_steps() == 3, "three steps programmed from the touch panel (%u)", tab5_pattern_steps());
    dump_ppm("04b-pattern", 2);
    {
        uint32_t w0 = tab5_flash_write_count();
        run_ms(60000);                                              /* idle: autosave writes the project to flash */
        CHECK(tab5_flash_write_count() > w0, "idle autosave wrote to flash (%u -> %u writes)", w0, tab5_flash_write_count());
    }

    /* ---- transport: PLAY with an empty pattern, then record a note ------------------------------- */
    printf("transport\n");
    tab5_buttons(1u << TAB5_B_PLAY);
    run_ms(80);
    tab5_buttons(0);
    run_ms(1000);
    CHECK(tab5_playing() == 1u, "PLAY starts the transport");
    tab5_buttons(1u << TAB5_B_PLAY);
    run_ms(80);
    tab5_buttons(0);
    run_ms(500);
    CHECK(tab5_playing() == 0u, "PLAY again stops it");

    /* ---- chord + a 3 s WAV (a musical check you can listen to) ----------------------------------- */
    printf("render\n");
    rec = malloc(sizeof(int32_t) * 2 * 44100 * 6);
    tab5_buttons(1u << TAB5_B_PLAY);
    run_ms(60);
    tab5_buttons(0);
    for (i = 0; i < 6; i++) {
        static const uint32_t chord[6] = {1u << 7 | 1u << 11 | 1u << 14, 1u << 5 | 1u << 9 | 1u << 12,
                                          1u << 3 | 1u << 7 | 1u << 10, 1u << 5 | 1u << 9 | 1u << 12,
                                          1u << 7 | 1u << 11 | 1u << 14, 0};
        tab5_keys(chord[i % 6]);
        tab5_audio_read(rec + 2u * rec_n, 22050);
        rec_n += 22050;
    }
    tab5_keys(0);
    {
        char path[512];
        uint32_t bytes = rec_n * 4u, v;
        uint16_t u16;
        snprintf(path, sizeof path, "%s/session.wav", dir);
        wav = fopen(path, "wb");
        fwrite("RIFF", 1, 4, wav); v = 36 + bytes; fwrite(&v, 4, 1, wav);
        fwrite("WAVEfmt ", 1, 8, wav); v = 16; fwrite(&v, 4, 1, wav);
        u16 = 1; fwrite(&u16, 2, 1, wav); u16 = 2; fwrite(&u16, 2, 1, wav);
        v = tab5_sample_rate(); fwrite(&v, 4, 1, wav); v *= 4; fwrite(&v, 4, 1, wav);
        u16 = 4; fwrite(&u16, 2, 1, wav); u16 = 16; fwrite(&u16, 2, 1, wav);
        fwrite("data", 1, 4, wav); fwrite(&bytes, 4, 1, wav);
        for (i = 0; i < rec_n * 2u; i++) {
            int32_t s = rec[i] >> 8;                                  /* 24 bit left-justified -> 16 bit */
            int16_t o = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
            fwrite(&o, 2, 1, wav);
        }
        fclose(wav);
    }
    free(rec);
    dump_ppm("07-after", 2);

    /* ---- MASTER and the clock-independent counters ---------------------------------------------- */
    tab5_master(0);
    run_ms(600);
    tab5_keys(1u << 7);
    run_ms(300);
    pk = capture(NULL, 4410, &rms);
    CHECK(pk < 0.01f, "MASTER at 0 silences the output (peak %.4f)", pk);
    tab5_master(1023);
    run_ms(600);
    pk = capture(NULL, 4410, &rms);
    CHECK(pk > 0.02f, "MASTER at max brings it back (peak %.3f)", pk);
    tab5_keys(0);
    run_ms(500);

    {
        char path[512];
        FILE *nf;
        snprintf(path, sizeof path, "%s/nor.bin", dir);
        nf = fopen(path, "wb");
        fwrite(tab5_nor_image(), 1, tab5_nor_size(), nf);
        fclose(nf);
    }
    printf("flash: %u writes, image saved to %s/nor.bin\n", tab5_flash_write_count(), dir);
    printf("%s (%d failed)\n", fails ? "FAILED" : "ALL OK", fails);
    return fails ? 1 : 0;
}
