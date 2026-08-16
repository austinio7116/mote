/*
 * ThumbyCue — the cue ball's account of its own shot.
 *
 * The touch log is new state filled by the integrator, so the question is not
 * "does it compile" but "does it describe the shot that actually happened".
 * Three ways of asking:
 *
 *   AGAINST SOMETHING TRUSTED. first_hit has been right for a long time and is
 *   filled at the same instant from the same collision. The log's first BALL
 *   entry must be that ball, on every shot. If the two ever disagree one of them
 *   is lying, and this says which shot did it.
 *
 *   AGAINST AN INDEPENDENT READING. The helpers — cannon, cushions before the
 *   second ball — are the ones the billiards games will be scored from, so they
 *   are checked against a plain re-scan of the same log written out longhand
 *   here. A helper that agrees with its own shortcut proves nothing.
 *
 *   AND THAT IT IS ACTUALLY EXERCISED. A property test over shots that all miss
 *   would pass while measuring nothing, so the counts are printed: how many of
 *   these shots produced a cannon, how many reached a second ball, the longest
 *   log seen. If those come out at zero the test is not testing.
 */
#include "cue_table.h"
#include "cue_physics.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int fails;
static void ck(int cond, const char *what) {
    if (!cond) { printf("  FAIL  %s\n", what); fails++; }
}

/* A deterministic little generator, so a failure can be reproduced. */
static unsigned rng_s = 12345u;
static float rnd(void) {
    rng_s = rng_s * 1664525u + 1013904223u;
    return (float)((rng_s >> 8) & 0xFFFFFF) / (float)0x1000000;
}

static void put(CueBall *b, int id, float x, float z, float R) {
    memset(b, 0, sizeof *b);
    b->on = 1; b->id = (uint8_t)id;
    b->pos = v3(x, R, z);
    b->orient.r[0] = v3(1,0,0);
    b->orient.r[1] = v3(0,1,0);
    b->orient.r[2] = v3(0,0,1);
}

/* The helpers, written out longhand, from the log alone. */
static int scan_cannon(const CueWorld *w, int a, int b) {
    int ga = 0, gb = 0;
    for (int i = 0; i < cue_touch_count(w); i++) {
        CueTouch t; cue_touch_get(w, i, &t);
        if (t.what != CUE_TOUCH_BALL) continue;
        if (t.id == a) ga = 1;
        if (t.id == b) gb = 1;
    }
    return ga && gb;
}
static int scan_cushions(const CueWorld *w) {
    int n = 0;
    for (int i = 0; i < cue_touch_count(w); i++) {
        CueTouch t; cue_touch_get(w, i, &t);
        if (t.what == CUE_TOUCH_CUSHION) n++;
    }
    return n;
}
static int scan_before_second(const CueWorld *w) {
    int first = -1, cush = 0;
    for (int i = 0; i < cue_touch_count(w); i++) {
        CueTouch t; cue_touch_get(w, i, &t);
        if (t.what == CUE_TOUCH_CUSHION) { cush++; continue; }
        if (first < 0) { first = t.id; continue; }
        if (t.id != first) return cush;
    }
    return -1;
}
static int scan_first_ball(const CueWorld *w) {
    for (int i = 0; i < cue_touch_count(w); i++) {
        CueTouch t; cue_touch_get(w, i, &t);
        if (t.what == CUE_TOUCH_BALL) return t.id;
    }
    return -1;
}

int main(void) {
    CueTable t; cue_table_init(&t, CUE_GAME_UK8);
    CueWorld w; cue_table_build_world(&t, &w);
    printf("ThumbyCue cue-ball touch log\n\n");

    /* ---- a shot that touches nothing at all ---------------------------- */
    {
        CueBall b[1];
        put(&b[0], 0, -0.3f, 0.0f, t.R);
        cue_table_set_cue_ball(&t, &b[0]);
        cue_phys_shot_begin(&w);
        ck(cue_touch_count(&w) == 0, "the log starts empty");
        /* barely a nudge: it stops before any cushion */
        cue_phys_strike(&w, &b[0], v3(1,0,0), 0.25f, 0.0f, 0.0f);
        for (int i = 0; i < 4000 && cue_phys_step(&w, b, 1, 1.0f/240.0f, 0); i++) {}
        ck(cue_touch_count(&w) == 0, "a shot that reaches nothing records nothing");
        ck(cue_touch_cushions_before_second_ball(&w) == -1,
           "no second ball is -1, which is not zero");
        ck(cue_touch_cannon(&w, 1, 2) == 0, "and no cannon");
    }

    /* ---- cushions only, no ball ---------------------------------------- */
    {
        CueBall b[1];
        put(&b[0], 0, 0.0f, 0.0f, t.R);
        cue_table_set_cue_ball(&t, &b[0]);
        cue_phys_shot_begin(&w);
        cue_phys_strike(&w, &b[0], v3(1,0,0), 3.0f, 0.0f, 0.0f);
        for (int i = 0; i < 20000 && cue_phys_step(&w, b, 1, 1.0f/240.0f, 0); i++) {}
        int c = cue_touch_cushions(&w);
        ck(c >= 1, "a ball sent up the table finds a cushion");
        ck(c == scan_cushions(&w), "the cushion count matches the log");
        ck(cue_touch_cushions_before_second_ball(&w) == -1,
           "cushions alone still never reach a second ball");
        printf("        straight up the table: %d cushions\n", c);
    }

    /* ---- and now a lot of real shots, three balls on the cloth ---------- */
    int shots = 0, cannons = 0, seconds = 0, longest = 0, over = 0;
    for (int s = 0; s < 600; s++) {
        CueBall b[3];
        put(&b[0], 0, -0.55f, (rnd() - 0.5f) * 0.30f, t.R);
        put(&b[1], 1,  0.00f, (rnd() - 0.5f) * 0.30f, t.R);
        put(&b[2], 2,  0.28f, (rnd() - 0.5f) * 0.40f, t.R);
        cue_table_set_cue_ball(&t, &b[0]);

        float ang = (rnd() - 0.5f) * 0.9f;          /* fan about straight up */
        float pw  = 1.2f + rnd() * 4.5f;
        cue_phys_shot_begin(&w);
        cue_phys_strike(&w, &b[0], v3(cosf(ang), 0, sinf(ang)), pw,
                        (rnd() - 0.5f) * 0.6f, (rnd() - 0.5f) * 0.6f);
        for (int i = 0; i < 40000 && cue_phys_step(&w, b, 3, 1.0f/240.0f, 0); i++) {}
        shots++;

        /* the trusted cross-check */
        ck(scan_first_ball(&w) == w.first_hit,
           "the log's first ball is the one first_hit named");

        /* the helpers against a longhand reading of the same log */
        ck(cue_touch_cannon(&w, 1, 2) == scan_cannon(&w, 1, 2), "cannon agrees with the log");
        ck(cue_touch_cushions(&w) == scan_cushions(&w), "cushions agree with the log");
        ck(cue_touch_cushions_before_second_ball(&w) == scan_before_second(&w),
           "cushions-before-second agrees with the log");

        /* a cannon and a second ball are not the same claim: a cannon says both
         * were touched, the other says one then a DIFFERENT one, in order. The
         * second implies the first, never the reverse. */
        if (scan_before_second(&w) >= 0) ck(cue_touch_cannon(&w, 1, 2) == 1,
            "reaching a second ball implies both were touched");

        if (cue_touch_cannon(&w, 1, 2)) cannons++;
        if (cue_touch_cushions_before_second_ball(&w) >= 0) seconds++;
        if (cue_touch_count(&w) > longest) longest = cue_touch_count(&w);
        if (w.touch_over) over++;
        if (fails > 20) break;   /* stop shouting */
    }

    printf("\n%d shots: %d cannons, %d reached a second ball\n", shots, cannons, seconds);
    printf("longest log %d of %d, overflowed on %d\n", longest, CUE_MAX_TOUCH, over);

    /* If the fan of shots never produced a cannon the property test above was
     * measuring nothing, and passing means nothing either. */
    ck(cannons > 0, "the shots actually produced cannons to check");
    ck(seconds > 0, "...and reached a second ball at least once");
    ck(over == 0, "no ordinary shot overflowed the log");

    printf("\n%s\n", fails ? "FAILED" : "PASSED");
    return fails ? 1 : 0;
}
