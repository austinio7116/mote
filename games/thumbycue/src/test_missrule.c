/*
 * test_missrule — FOUL AND A MISS, JUDGED AT THREE STANDARDS.
 *
 * WPBSA Section 3 Rule 14 does not call a miss on every failure to hit the
 * ball on: the referee judges whether the striker made a good enough ATTEMPT.
 * Before this, every wrong-hit or no-hit foul was called — correct only for
 * the professional game, and infuriating everywhere below it: an amateur who
 * rolls a genuine attempt a ball-width past a long red does not expect the
 * balls put back.
 *
 * The judgement is one function, miss_attempt_ok, fed by two bookends that
 * measure the stroke — how close the cue ball's path came to a legal ball-on,
 * and whether it had the pace to get there. This test pins the whole matrix:
 * every scenario at every standard, because the tuning IS the feature and a
 * threshold nudged without this file noticing would quietly change what a
 * referee lets go.
 *
 * TWO KINDS OF CASE. The matrix cases set the attempt fields directly and ask
 * the resolve to judge — they test the judge. The integrated cases play REAL
 * STROKES through the physics and let the bookends do the measuring — they
 * test that the instrument reads what actually happened on the cloth. Both are
 * needed: a judge tested only on synthetic numbers may be fed nonsense by a
 * broken instrument, and an instrument tested only end-to-end hides which half
 * failed.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "cue_table.h"
#include "cue_rules.h"

static int fails = 0, checks = 0;
static void ok(int cond, const char *what) {
    checks++;
    if (!cond) { fails++; printf("FAIL %s\n", what); }
    else printf("ok   %s\n", what);
}

static CueTable T;
static CueWorld W;
static CueBall  B[CUE_MAX_BALLS];
static CueRules R;
static int NB;

/* A fresh snooker frame, reds on, no score. */
static void frame(void) {
    cue_table_init(&T, CUE_GAME_SNK15);
    cue_table_build_world(&T, &W);
    NB = cue_table_rack(&T, B);
    cue_rules_init(&R, &T, 0);
    R.turn = 0;
}

/* ---- the matrix: judge a no-hit stroke with these attempt readings -------- */
static int judged_miss(int level, float gap_bw, float pace, float dist_m,
                       int snookered) {
    frame();
    R.miss_level = level;
    R.was_snookered = snookered;
    R.att_have = 2;
    R.att_gap  = gap_bw;
    R.att_pace = pace;
    R.att_dist = dist_m;
    cue_phys_shot_begin(&W);          /* first_hit = -1: hit nothing at all */
    cue_rules_resolve(&R, B, NB, &W, -1, 0, 1, NULL, 0);
    return R.last_miss;
}

int main(void) {
    printf("foul and a miss, judged\n\n");

    /* ---- level 0 is the old behaviour, exactly --------------------------- */
    ok(judged_miss(0, 0.05f, 1.5f, 0.5f, 0) == 1,
       "level OFF: even a whisker at full pace is called");

    /* ---- a whisker at full pace ------------------------------------------ */
    ok(judged_miss(1, 0.10f, 1.2f, 1.0f, 0) == 0, "whisker: AMATEUR lets it go");
    ok(judged_miss(2, 0.10f, 1.2f, 1.0f, 0) == 0, "whisker: CLUB lets it go");
    ok(judged_miss(3, 0.10f, 1.2f, 1.0f, 0) == 0, "whisker: even PRO lets it go");

    /* ---- missed by two ball widths, full pace, a metre away -------------- *
     * AMATEUR allows 3.0 x 1.45 = 4.35; CLUB 1.5 x 1.45 = 2.18; PRO 0.44. */
    ok(judged_miss(1, 2.0f, 1.1f, 1.0f, 0) == 0, "two balls out: AMATEUR lets it go");
    ok(judged_miss(2, 2.0f, 1.1f, 1.0f, 0) == 0, "two balls out: CLUB just lets it go");
    ok(judged_miss(3, 2.0f, 1.1f, 1.0f, 0) == 1, "two balls out: PRO calls it");

    /* ---- missed by four ball widths from close range --------------------- */
    ok(judged_miss(1, 4.0f, 1.1f, 0.3f, 0) == 1, "four balls out, close up: even AMATEUR calls it");
    ok(judged_miss(2, 4.0f, 1.1f, 0.3f, 0) == 1, "four balls out, close up: CLUB calls it");

    /* ---- the same four balls out, but the length of the table ------------ *
     * 3.0 x (1 + 0.45 x 3.2) = 7.3 for AMATEUR: a long roll is judged kinder. */
    ok(judged_miss(1, 4.0f, 1.0f, 3.2f, 0) == 0, "four balls out at full length: AMATEUR lets it go");
    ok(judged_miss(2, 4.0f, 1.0f, 3.2f, 0) == 1, "...but CLUB still calls it");

    /* ---- rolled up short: the classic deliberate miss --------------------- *
     * Dead straight, so the gap reads small the further it got — but the PACE
     * is the tell, and it is judged at every level. */
    ok(judged_miss(1, 0.4f, 0.45f, 1.5f, 0) == 1, "rolled up short: AMATEUR calls it");
    ok(judged_miss(2, 0.4f, 0.75f, 1.5f, 0) == 1, "nearly there but short: CLUB calls it");
    ok(judged_miss(3, 0.1f, 1.0f, 1.5f, 0) == 1, "even touching distance, no legs to pass: PRO calls it");
    ok(judged_miss(3, 0.1f, 1.06f, 1.5f, 0) == 0, "...and with the legs to pass, PRO lets it go");

    /* ---- snookered: the escape earns three times the room ----------------- *
     * CLUB allows 1.5 x 3 x (1 + 0.45 x 2) = 8.5 ball widths on a 2 m escape. */
    ok(judged_miss(2, 6.0f, 1.3f, 2.0f, 1) == 0, "snookered, six balls out off the cushions: CLUB lets it go");
    ok(judged_miss(2, 6.0f, 1.3f, 2.0f, 0) == 1, "...the identical stroke NOT snookered is a miss");
    ok(judged_miss(2, 12.0f, 1.3f, 2.0f, 1) == 1, "snookered but at the wrong cushion entirely: called");
    ok(judged_miss(1, 12.0f, 1.3f, 2.0f, 1) == 0, "...though AMATEUR forgives even that");

    /* ---- no attempt data: judged as level 0 ------------------------------- */
    frame();
    R.miss_level = 2;
    R.att_have = 0;
    cue_phys_shot_begin(&W);
    cue_rules_resolve(&R, B, NB, &W, -1, 0, 1, NULL, 0);
    ok(R.last_miss == 1, "no attempt data: every failure is called, as before 2.0");

    /* ---- snookers-needed still suppresses the call, at every level -------- */
    frame();
    R.miss_level = 1;
    R.att_have = 2; R.att_gap = 9.0f; R.att_pace = 0.2f; R.att_dist = 1.0f;
    R.score[1] = 90; R.score[0] = 0;   /* striker 0 needs snookers */
    for (int i = 0; i < NB; i++)
        if (B[i].id >= 1 && B[i].id <= 15) B[i].on = 0;  /* colours only left */
    cue_rules_resolve(&R, B, NB, &W, -1, 0, 1, NULL, 0);
    ok(R.last_foul == 1 && R.last_miss == 0,
       "needing snookers: a hopeless stroke is a foul but never a miss");

    /* ---- the forfeit counts CALLED misses only ---------------------------- */
    frame();
    R.miss_level = 2;
    for (int k = 0; k < 5; k++) {
        R.att_have = 2; R.att_gap = 0.1f; R.att_pace = 1.2f; R.att_dist = 1.0f;
        R.was_snookered = 0;
        cue_phys_shot_begin(&W);
        cue_rules_resolve(&R, B, NB, &W, -1, 0, 1, NULL, 0);
        R.turn = 0;                    /* keep the same striker at it */
    }
    ok(!R.frame_over, "five ACCEPTED attempts in a row forfeit nothing");
    frame();
    R.miss_level = 2;
    for (int k = 0; k < 3; k++) {
        R.att_have = 2; R.att_gap = 8.0f; R.att_pace = 1.2f; R.att_dist = 1.0f;
        R.was_snookered = 0;
        cue_phys_shot_begin(&W);
        cue_rules_resolve(&R, B, NB, &W, -1, 0, 1, NULL, 0);
        R.turn = 0;
    }
    ok(R.frame_over && R.winner == 1, "three CALLED misses forfeit the frame");

    /* ---- a foul that hit the ball on is never a miss ----------------------- */
    frame();
    R.miss_level = 3;
    R.att_have = 2; R.att_gap = 0.0f; R.att_pace = 1.4f; R.att_dist = 1.0f;
    {   int pot = CUE_ID_BLACK;        /* red on, black potted: foul, not miss */
        int red_idx = -1;
        for (int i = 1; i < NB; i++) if (B[i].id >= 1 && B[i].id <= 15) { red_idx = i; break; }
        cue_phys_shot_begin(&W);
        cue_rules_resolve(&R, B, NB, &W, B[red_idx].id, 0, 1, &pot, 1);
        ok(R.last_foul == 1 && R.last_miss == 0,
           "red struck first, black potted: a foul, never a miss");
    }

    /* ==== THE INSTRUMENT: real strokes, measured by the bookends =========== */
    printf("\n");

    /* A red dead ahead, the cue ball rolled straight at it but too soft. */
    frame();
    R.miss_level = 2;
    for (int i = 1; i < NB; i++) B[i].on = 0;
    B[1].on = 1; B[1].id = 1;                       /* one red */
    B[0].pos = v3(-0.5f, T.R, 0.0f);
    B[1].pos = v3( 0.5f, T.R, 0.0f);                /* a metre away */
    cue_rules_attempt_begin(&R, B, NB);
    cue_phys_shot_begin(&W);
    B[0].vel = v3(0.45f, 0.0f, 0.0f);               /* dies well short */
    {   uint32_t ev; int steps = 0;
        while (cue_phys_step(&W, B, NB, 1.0f / 240.0f, &ev) && steps++ < 4000) { }
    }
    cue_rules_attempt_end(&R, &W, B, NB);
    printf("     [instrument: gap %.2f bw, pace %.2f, dist %.2f m, first_hit %d]\n",
           (double)R.att_gap, (double)R.att_pace, (double)R.att_dist, W.first_hit);
    ok(R.att_have == 2, "a real stroke fills the attempt in");
    ok(R.att_pace < 0.9f, "  rolled short: the instrument reads the missing pace");
    cue_rules_resolve(&R, B, NB, &W, W.first_hit, 0, 1, NULL, 0);
    ok(R.last_foul == 1 && R.last_miss == 1,
       "  and CLUB calls the miss on a stroke that never had the legs");

    /* The same red, full pace, aimed two ball widths wide. */
    frame();
    R.miss_level = 1;
    for (int i = 1; i < NB; i++) B[i].on = 0;
    B[1].on = 1; B[1].id = 1;
    B[0].pos = v3(-0.5f, T.R, 0.0f);
    B[1].pos = v3( 0.5f, T.R, 0.0f);
    cue_rules_attempt_begin(&R, B, NB);
    cue_phys_shot_begin(&W);
    {   /* two ball widths of lateral offset at the target, at pace */
        const float off = 4.0f * T.R + 2.0f * T.R;   /* surface gap = 2 bw */
        const float ang = atan2f(off, 1.0f);
        B[0].vel = v3(2.2f * cosf(ang), 0.0f, 2.2f * sinf(ang));
    }
    {   uint32_t ev; int steps = 0;
        while (cue_phys_step(&W, B, NB, 1.0f / 240.0f, &ev) && steps++ < 4000) { }
    }
    cue_rules_attempt_end(&R, &W, B, NB);
    ok(R.att_gap > 1.0f && R.att_gap < 3.2f,
       "aimed two balls wide at pace: the instrument reads the gap");
    ok(R.att_pace > 1.0f, "  and the pace it carried");
    cue_rules_resolve(&R, B, NB, &W, W.first_hit, 0, 1, NULL, 0);
    ok(R.last_foul == 1 && R.last_miss == 0,
       "  AMATEUR accepts it: a genuine attempt, let go");

    printf("\n%d checks, %d failed\n", checks, fails);
    if (!fails) printf("\nall good\n");
    return fails ? 1 : 0;
}
