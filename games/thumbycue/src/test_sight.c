/*
 * ThumbyCue — what counts as a snooker (C3).
 *
 * Reported from play: on an L-shaped table the opponent was laid a hard snooker
 * behind the elbow, failed to escape three times, and lost the frame. It should
 * not have. Three misses forfeit only when a full ball was available, and one
 * never was — but the cushion doing the snookering was invisible to the sight
 * test, so every honest failure was scored as a genuine miss.
 *
 * The rule book is why. Section 2 defines snookered as obstructed "by a ball or
 * balls not On" — balls, only balls — and that is not an oversight. On a
 * rectangular table the bed is CONVEX, so the straight line between two balls
 * resting on it cannot leave it, and no cushion can ever come between them. The
 * rule did not exclude cushions; it never had to consider them. Custom beds do.
 *
 * So `clear_path` now tests the cushion chain as well, and this file holds the
 * two claims that makes:
 *
 *   IT CHANGES NOTHING ON A RECTANGULAR TABLE. Not "we looked and it seemed
 *   fine" — every rectangular table, thousands of layouts, the answer with the
 *   cushions compared against the answer without them, and they must agree
 *   every single time. This is the half that matters. A sight test that starts
 *   awarding free balls on a snooker table would be far worse than the bug it
 *   was written to fix, and convexity says it cannot, so the test says so too.
 *
 *   AND IT CHANGES SOMETHING ON AN L. There must exist a layout where the only
 *   thing between the cue ball and every ball On is timber. If there is not,
 *   the extension is not doing anything and the frame is still forfeit.
 */
#include "cue_table.h"
#include "cue_physics.h"
#include "cue_rules.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int fails;
static void ck(int cond, const char *what) {
    printf(cond ? "  ok    %s\n" : "  FAIL  %s\n", what);
    if (!cond) fails++;
}

/* A deterministic generator, so a disagreement can be reproduced exactly. */
static uint32_t RNG = 20260818u;
static float rnd(void) {
    RNG = RNG * 1664525u + 1013904223u;
    return (float)((RNG >> 8) & 0xFFFFFF) / (float)0x1000000;
}

static CueTable T;
static CueWorld W;
static CueRules R;
static CueBall  B[32];
static int      N;

/* Scatter the rack at random over the bed, keeping every ball on the cloth and
 * clear of its neighbours. Returns 0 if it could not place them, which on a
 * table with a big bite out of it happens and is not a failure. */
static int scatter(void) {
    for (int i = 0; i < N; i++) {
        int ok = 0;
        for (int tries = 0; tries < 200 && !ok; tries++) {
            float x = (rnd() * 2.0f - 1.0f) * (T.half_len - T.R * 2.0f);
            float z = (rnd() * 2.0f - 1.0f) * (T.half_wid - T.R * 2.0f);
            /* out of the bite, if there is one */
            if (T.bed_shape == CUE_BED_L &&
                x > T.half_len - T.notch_x && z > T.half_wid - T.notch_z) continue;
            int clash = 0;
            for (int j = 0; j < i && !clash; j++) {
                float dx = B[j].pos.x - x, dz = B[j].pos.z - z;
                if (dx*dx + dz*dz < (T.R * 2.2f) * (T.R * 2.2f)) clash = 1;
            }
            if (clash) continue;
            B[i].pos = v3(x, T.R, z);
            B[i].on = 1;
            ok = 1;
        }
        if (!ok) return 0;
    }
    return 1;
}

/* ---- 1. a rectangular table cannot notice --------------------------------- */

static void rectangles(void) {
    printf("cushions change nothing where the bed is convex\n");
    int checked = 0, differed = 0;
    for (int k = 0; k < CUE_GAME_COUNT; k++) {
        cue_table_init(&T, (CueGameKind)k);
        if (T.bed_shape != CUE_BED_RECT) continue;   /* not a rectangle */
        cue_table_build_world(&T, &W);
        N = cue_table_rack(&T, B);
        cue_rules_init(&R, &T, 0);
        if (!R.kind) continue;                     /* snooker's question only */
        int here = 0, bad_here = 0;
        for (int trial = 0; trial < 400; trial++) {
            if (!scatter()) continue;
            /* Ask it from a fresh angle each time: the ball On moves through
             * the sequence, so the test is not always about a red. */
            R.target = (trial % 3 == 0) ? 1 : 0;
            R.seq    = 2 + (trial % 6);
            int with    = cue_rules_is_snookered(&R, B, N, &W);
            int without = cue_rules_is_snookered(&R, B, N, NULL);
            here++;
            if (with != without) {
                bad_here++;
                if (bad_here == 1)
                    printf("  FAIL  %d: cushions changed the answer (%d vs %d) "
                           "on trial %d\n", k, with, without, trial);
            }
        }
        printf("        game %-2d %4d layouts, %d disagreements\n", k, here, bad_here);
        checked += here; differed += bad_here;
    }
    printf("        %d layouts across every rectangular snooker table\n", checked);
    ck(checked > 1000, "enough layouts were actually tried to mean something");
    ck(differed == 0, "no rectangular table's answer moved");
}

/* ---- 2. an L can be snookered by its own elbow ----------------------------- */

static void the_elbow(void) {
    printf("\nand an L-shaped table can snooker you with timber\n");
    cue_table_init(&T, CUE_GAME_SNK15);
    T.bed_shape = CUE_BED_L;             /* without this the notch is inert */
    T.notch_x = T.half_len * 0.55f;
    T.notch_z = T.half_wid * 0.50f;
    cue_table_build_world(&T, &W);
    N = cue_table_rack(&T, B);
    cue_rules_init(&R, &T, 0);
    R.target = 0; R.seq = 2;

    /* THE REPORTED POSITION, BUILT RATHER THAN STUMBLED ON.
     *
     * Scattering the rack at random will not find this: with fifteen reds on
     * the table one of them is nearly always visible, which is exactly why the
     * bug survived so long in play and would survive a property test too. The
     * position that matters is the one a player deliberately leaves — every
     * ball On round the elbow, and nothing else to look at.
     *
     * The bite is out of the +x/+z corner, so the bed is a full-width band
     * below it and a full-height column beside it. Put the ball On far down the
     * band and the cue ball high up the column: the straight line between them
     * runs through the corner that is not there, and there is nothing but
     * timber on it. */
    const float hl = T.half_len, hw = T.half_wid;
    int tgt = -1;
    for (int i = 1; i < N; i++)
        if (cue_rules_ball_legal(&R, B, N, B[i].id)) { tgt = i; break; }
    if (tgt < 0) { printf("  FAIL  the rack has no legal target\n"); fails++; return; }

    for (int i = 1; i < N; i++) B[i].on = (i == tgt);
    B[0].on = 1;
    B[0].pos   = v3(0.10f * hl, T.R, 0.90f * hw);   /* up the column */
    B[tgt].pos = v3(0.90f * hl, T.R, 0.20f * hw);   /* along the band */

    int with    = cue_rules_is_snookered(&R, B, N, &W);
    int without = cue_rules_is_snookered(&R, B, N, NULL);
    printf("        round the elbow: cushions say %s, balls alone say %s\n",
           with ? "snookered" : "clear", without ? "snookered" : "clear");
    ck(with,     "the elbow snookers the striker");
    ck(!without, "...and no BALL is doing it — this is the case that was missed");

    /* And the consequence that was reported: a striker in that position must
     * not be counted toward the three-miss forfeit. `was_snookered` is the same
     * call, so what is asserted is that the host, asking the way the host asks,
     * gets the protective answer. */
    R.was_snookered = cue_rules_is_snookered(&R, B, N, &W);
    ck(R.was_snookered, "...so three failures to escape do not forfeit the frame");

    /* THE OTHER HALF, which is the one that stops this being a rule that just
     * says yes: bring the cue ball round into the same arm, in plain sight down
     * the band, and it must read clear. A test that only ever asserts
     * "snookered" would pass just as well if the function returned 1. */
    B[0].pos = v3(0.20f * hl, T.R, 0.20f * hw);     /* same band, clear run */
    int near_ = cue_rules_is_snookered(&R, B, N, &W);
    printf("        same arm, plain sight: %s\n", near_ ? "snookered" : "clear");
    ck(!near_, "a clear view down the same arm is not a snooker");

    /* And a rectangular table with the identical layout is clear both ways —
     * the same two balls, the same two places, only the bed is different. */
    {   CueTable T2; cue_table_init(&T2, CUE_GAME_SNK15);
        static CueWorld W2; cue_table_build_world(&T2, &W2);
        B[0].pos   = v3(0.10f * hl, T2.R, 0.90f * hw);
        B[tgt].pos = v3(0.90f * hl, T2.R, 0.20f * hw);
        int rect = cue_rules_is_snookered(&R, B, N, &W2);
        printf("        the same two balls on a full rectangle: %s\n",
               rect ? "snookered" : "clear");
        ck(!rect, "and on a rectangle those same two balls see each other");
    }
}

int main(void) {
    rectangles();
    the_elbow();
    printf(fails ? "\nFAILED (%d)\n" : "\nPASSED\n", fails);
    return fails ? 1 : 0;
}
