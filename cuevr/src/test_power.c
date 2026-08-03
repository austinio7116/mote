/*
 * CueVR — is the power linear in the stroke?
 *
 * "Gentle shots too weak, power shots overpowered" is not noise, it is a
 * non-linear mapping, and it is not something the other tests can see: they
 * check one stroke at a time against its own speed. This drives a realistic
 * delivery — address, brief backswing, accelerate through — at a range of
 * speeds and checks that the reported power is proportional to the speed the
 * cue was actually doing.
 *
 * It exists because the first two attempts were both non-linear and both
 * passed everything else. A peak over the delivery inflates a hard stroke more
 * than a gentle one; a trailing mean that includes the stationary frames of the
 * address under-reads a short delivery more than a long one.
 *
 *   cc -I. -I../../games/thumbycue/src -I../../engine/math -I../../platform/xr \
 *      -o /tmp/test_power test_power.c cuevr_cue.c -lm && /tmp/test_power
 */
#include "cuevr.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define R  0.02625f
#define DT (1.0f / 72.0f)

static CueVrPlacement PLACE = { { 0.0f, 1.0f, 0.0f }, 0.0f, 1.0f };
static MoteVrV3 BALL = { 0.0f, 1.0f + R, 0.0f };

/* Hands placed so the tip is `gap` short of contact, trigger held. */
static void hold(MoteVrTracking *t, CueVrCue *c, float gap) {
    memset(t, 0, sizeof *t);
    t->dt = DT;
    t->hand[MOTE_VR_LEFT].tracked = t->hand[MOTE_VR_RIGHT].tracked = 1;
    MoteVrV3 ax = mv3(1, 0, 0);
    MoteVrV3 tip = mv3_sub(BALL, mv3_scale(ax, (R + CUEVR_TIP_R) + gap));
    MoteVrV3 grip = mv3_sub(tip, mv3_scale(ax, CUEVR_CUE_LEN - c->grip));
    t->hand[MOTE_VR_RIGHT].pose.p = grip;
    t->hand[MOTE_VR_LEFT].pose.p  = mv3_add(grip, mv3_scale(ax, 0.90f));
    t->hand[MOTE_VR_RIGHT].trigger = 1.0f;
}

/* A delivery at `v` m/s: settle on the ball, draw back, then come through. */
static float deliver(float v, float jitter_mm) {
    CueVrCue c;
    CueVrShot s;
    MoteVrTracking t;
    memset(&s, 0, sizeof s);
    cuevr_cue_init(&c);

    float gap = 0.14f;
    hold(&t, &c, gap);
    cuevr_cue_update(&c, &t, &PLACE, BALL, R, &s);      /* take hold */
    for (int i = 0; i < 4; i++) {                       /* address, still */
        hold(&t, &c, gap);
        cuevr_cue_update(&c, &t, &PLACE, BALL, R, &s);
    }
    for (int i = 0; i < 3; i++) {                       /* backswing */
        gap += v * DT * 0.5f;
        hold(&t, &c, gap);
        cuevr_cue_update(&c, &t, &PLACE, BALL, R, &s);
    }
    unsigned seed = 12345u;
    for (int i = 0; i < 120 && !s.struck; i++) {        /* through the ball */
        seed = seed * 1103515245u + 12345u;
        float n = (((float)((seed >> 16) & 0xFF) / 255.0f) - 0.5f) * jitter_mm * 0.001f;
        gap -= v * DT + n;
        hold(&t, &c, gap);
        cuevr_cue_update(&c, &t, &PLACE, BALL, R, &s);
    }
    return s.struck ? s.speed : 0.0f;
}

int main(void) {
    int fail = 0;

    printf("clean delivery:\n");
    float lo = 0.0f, hi = 0.0f;
    for (float v = 0.5f; v <= 6.5f; v += 1.0f) {
        float got = deliver(v, 0.0f);
        float ratio = got / v;
        printf("  %.1f m/s -> %.2f   ratio %.3f\n", (double)v, (double)got, (double)ratio);
        if (v == 0.5f) lo = ratio;
        hi = ratio;
        if (ratio < 0.90f || ratio > 1.10f) {
            printf("  FAIL: %.1f m/s reported at %.0f%% of the stroke\n",
                   (double)v, (double)(ratio * 100.0f));
            fail = 1;
        }
    }
    /* The real complaint was that soft and hard shots were scaled DIFFERENTLY.
     * So the strong assertion is not "each is accurate" but "they agree". */
    printf("softest/hardest scaling differ by %.1f%%\n",
           (double)(fabsf(hi - lo) / lo * 100.0f));
    if (fabsf(hi - lo) / lo > 0.08f) {
        printf("  FAIL: the mapping is not linear across the range\n");
        fail = 1;
    }

    printf("with 2 mm of per-frame tracking jitter:\n");
    for (float v = 1.0f; v <= 5.0f; v += 2.0f) {
        float a = deliver(v, 2.0f);
        printf("  %.1f m/s -> %.2f   ratio %.3f\n", (double)v, (double)a, (double)(a / v));
        if (a / v < 0.80f || a / v > 1.25f) {
            printf("  FAIL: jitter moved the power by more than a quarter\n");
            fail = 1;
        }
    }

    printf(fail ? "\nFAILED\n" : "\nall good\n");
    return fail;
}
