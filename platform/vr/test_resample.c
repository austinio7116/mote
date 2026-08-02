/*
 * Mote VR — does the audio resampler actually resample?
 *
 * The headset build's only bespoke DSP, and the one piece of it that cannot be
 * exercised on the machine it is written on: it runs inside an AAudio callback
 * on a Quest. So it lives in a header with no audio API in it, and this runs
 * the real code against a source whose exact value at every instant is known.
 *
 *   cc -I../../engine/core -o /tmp/test_resample test_resample.c -lm && /tmp/test_resample
 */
#include "mote_vr_resample.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define SRC_RATE 22050

/* A ramp: sample n has value n. Any discontinuity, repeat or skipped block in
 * the buffer handling shows up as a step that is not the expected slope. */
static long g_n;
static void fill_ramp(int16_t *out, int n) {
    for (int i = 0; i < n; i++) out[i] = (int16_t)((g_n++) & 0x7FFF);
}

/* A sine the test can also evaluate in closed form, to check the phase does not
 * drift over a long run. */
static double g_t;
static double g_freq;
static void fill_sine(int16_t *out, int n) {
    for (int i = 0; i < n; i++) {
        out[i] = (int16_t)(12000.0 * sin(2.0 * M_PI * g_freq * g_t / SRC_RATE));
        g_t += 1.0;
    }
}

static int fail;
static void check(int cond, const char *what) {
    if (!cond) { printf("FAIL %s\n", what); fail = 1; }
    else       { printf("ok   %s\n", what); }
}

int main(void) {
    /* ---- 1. identity-ish: 22050 -> 22050 is a straight copy ------------- */
    {
        MoteVrResampler r;
        mote_vr_rs_init(&r, SRC_RATE, SRC_RATE);
        g_n = 0;
        int16_t out[5000];
        mote_vr_rs_render(&r, out, 5000, fill_ramp);
        int bad = 0;
        for (int i = 0; i < 5000; i++) if (out[i] != (int16_t)(i & 0x7FFF)) bad++;
        check(bad == 0, "22050->22050 is sample-exact");
    }

    /* ---- 2. 22050 -> 48000: the ramp keeps a constant slope ------------- *
     * Every output sample must advance by 22050/48000 of a source sample. The
     * point of the test is the seam between internal buffers: a wrong phase
     * rewind repeats or drops a whole block, which is a slope of 0 or a jump
     * of 2048 exactly where the buffer turns over. */
    {
        MoteVrResampler r;
        mote_vr_rs_init(&r, SRC_RATE, 48000);
        g_n = 0;
        const int N = 40000;                  /* ~19 buffer turnovers */
        int16_t *out = malloc(sizeof(int16_t) * N);
        mote_vr_rs_render(&r, out, N, fill_ramp);
        double want = (double)SRC_RATE / 48000.0;
        double worst = 0.0;
        int worst_at = -1;
        for (int i = 1; i < N; i++) {
            double d = (double)out[i] - (double)out[i - 1];
            double err = fabs(d - want);
            if (err > worst) { worst = err; worst_at = i; }
        }
        printf("     worst slope error %.4f at %d (want %.5f/sample)\n",
               worst, worst_at, want);
        check(worst < 1.01, "22050->48000 ramp slope stays continuous across buffers");
        free(out);
    }

    /* ---- 3. 22050 -> 48000: a sine keeps its phase over 10 s ------------ */
    {
        MoteVrResampler r;
        mote_vr_rs_init(&r, SRC_RATE, 48000);
        g_t = 0.0; g_freq = 440.0;
        const int N = 480000;                 /* 10 seconds out */
        int16_t *out = malloc(sizeof(int16_t) * N);
        mote_vr_rs_render(&r, out, N, fill_sine);
        /* compare the tail against the analytic sine at the same instant */
        double worst = 0.0;
        for (int i = N - 2000; i < N; i++) {
            double t = (double)i * SRC_RATE / 48000.0;
            double ref = 12000.0 * sin(2.0 * M_PI * g_freq * t / SRC_RATE);
            double err = fabs((double)out[i] - ref);
            if (err > worst) worst = err;
        }
        printf("     worst amplitude error after 10 s: %.1f of 12000\n", worst);
        check(worst < 200.0, "22050->48000 sine holds phase for 10 s");
        free(out);
    }

    /* ---- 4. upsampling the other way, and an odd rate ------------------- */
    {
        MoteVrResampler r;
        mote_vr_rs_init(&r, SRC_RATE, 44100);
        g_n = 0;
        int16_t out[9000];
        mote_vr_rs_render(&r, out, 9000, fill_ramp);
        double worst = 0.0;
        for (int i = 1; i < 9000; i++)
            worst = fmax(worst, fabs((out[i] - out[i - 1]) - 0.5));
        check(worst < 1.01, "22050->44100 (exact 2x) stays continuous");

        mote_vr_rs_init(&r, SRC_RATE, 16000);
        g_n = 0;
        mote_vr_rs_render(&r, out, 9000, fill_ramp);
        worst = 0.0;
        for (int i = 1; i < 9000; i++)
            worst = fmax(worst, fabs((out[i] - out[i - 1]) - 22050.0 / 16000.0));
        check(worst < 1.01, "22050->16000 (downsampling) stays continuous");
    }

    printf(fail ? "\nFAILED\n" : "\nall good\n");
    return fail;
}
