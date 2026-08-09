/*
 * ThumbyCue — a free ball is a NOMINATED ball.
 *
 * Reported: "the game progresses on to the next ball after the free ball is
 * potted, leaving the non-nominated ball as unpotted". It did, and the cause
 * was one line — resolve_snooker read the nominated id and threw it away:
 *
 *     int fb_id = r->free_ball_id; r->free_ball_id = 0; (void)fb_id;
 *
 * With that discarded, "a free ball is in play" became "every ball that is not
 * the ball on is in play". Hitting any of them first was legal, potting any of
 * them scored the ball-on's value, and the frame moved on to the next ball
 * while the ball actually nominated sat there untouched.
 *
 * WPBSA Section 3 Rule 12: the striker nominates a ball, and for that stroke
 * the nominated ball IS the ball on. Any other ball is not.
 */
#include "cue_rules.h"
#include "cue_table.h"
#include "cue_physics.h"

#include <stdio.h>
#include <string.h>

static int s_fail;
static void ok(int cond, const char *what, const char *detail) {
    if (!cond) { s_fail++; printf("  FAIL %s%s%s\n", what, detail ? " — " : "", detail ? detail : ""); }
    else printf("  ok   %s\n", what);
}

static CueTable T;
static CueWorld W;
static CueBall  B[22];
static int      N;

/* On a red, snookered, with a free ball awarded and `nom` named. */
static void setup(CueRules *r, int nom) {
    N = cue_table_rack(&T, B);
    cue_rules_init(r, &T, 0);
    r->turn = 0;
    r->target = 0;              /* on a red */
    r->break_shot = 0;
    r->free_ball = 1;
    r->free_ball_id = nom;
}

static int ball_on_table(int id) {
    for (int i = 1; i < N; i++) if (B[i].id == id && B[i].on) return 1;
    return 0;
}
static void take_off(int id) {
    for (int i = 1; i < N; i++) if (B[i].id == id) B[i].on = 0;
}

int main(void) {
    cue_table_init(&T, CUE_GAME_SNK15);
    cue_table_build_world(&T, &W);
    printf("free ball\n");

    /* ---- 1. potting the ball you nominated is legal ---------------------- */
    {
        CueRules r; setup(&r, CUE_ID_BROWN);
        int before = r.score[0];
        int potted[1] = { CUE_ID_BROWN };
        take_off(CUE_ID_BROWN);
        cue_rules_resolve(&r, B, N, &W, CUE_ID_BROWN, 0, 1, potted, 1);
        char d[128];
        snprintf(d, sizeof d, "score %d -> %d, target %d, turn %d, foul %d",
                 before, r.score[0], r.target, r.turn, r.last_foul);
        ok(!r.last_foul && r.turn == 0, "the nominated ball: legal, still your turn", d);
        ok(r.score[0] == before + 1, "and scores the value of the ball ON (a red)", d);
        ok(r.target == 1, "and you are on a colour next", d);
        ok(ball_on_table(CUE_ID_BROWN), "and the free ball goes back on its spot", d);
    }

    /* ---- 2. potting a DIFFERENT ball is a foul --------------------------- *
     * The blue is not the ball on and it is not what was nominated, so it is
     * simply an illegal pot. This is the reported bug: it used to score, move
     * the frame on to the next ball, and leave the nominated brown standing. */
    {
        CueRules r; setup(&r, CUE_ID_BROWN);
        int potted[1] = { CUE_ID_BLUE };
        take_off(CUE_ID_BLUE);
        cue_rules_resolve(&r, B, N, &W, CUE_ID_BLUE, 0, 1, potted, 1);
        char d[128];
        snprintf(d, sizeof d, "foul %d, opponent %d, target %d, brown still up %d",
                 r.last_foul, r.score[1], r.target, ball_on_table(CUE_ID_BROWN));
        ok(r.last_foul, "a ball that was NOT nominated is a foul", d);
        ok(r.score[1] > 0, "and the opponent is awarded the penalty", d);
        ok(r.target == 0, "and the frame does NOT move on to the next ball", d);
    }

    /* ---- 3. hitting a different ball first is a foul --------------------- */
    {
        CueRules r; setup(&r, CUE_ID_BROWN);
        cue_rules_resolve(&r, B, N, &W, CUE_ID_BLUE, 0, 1, NULL, 0);
        char d[96];
        snprintf(d, sizeof d, "foul %d, opponent %d", r.last_foul, r.score[1]);
        ok(r.last_foul && r.score[1] > 0,
           "first contact on a ball that was not nominated is a foul", d);
    }

    /* ---- 4. the real ball on is always legal ----------------------------- *
     * A free ball does not stop you simply potting a red. */
    {
        CueRules r; setup(&r, CUE_ID_BROWN);
        int red = 0;
        for (int i = 1; i < N; i++) if (B[i].id >= 1 && B[i].id <= 15) { red = B[i].id; break; }
        int potted[1] = { red };
        take_off(red);
        cue_rules_resolve(&r, B, N, &W, red, 0, 1, potted, 1);
        char d[96];
        snprintf(d, sizeof d, "foul %d, score %d, target %d",
                 r.last_foul, r.score[0], r.target);
        ok(!r.last_foul && r.score[0] == 1 && r.target == 1,
           "potting an actual red is still legal with a free ball up", d);
    }

    /* ---- 5. no nomination means any not-on ball stands in ---------------- *
     * The rules make the striker name one; if the game somehow awards a free
     * ball without a nomination, the old permissive behaviour is the safe one —
     * refusing every ball would leave the player unable to play at all. */
    {
        CueRules r; setup(&r, 0);
        int potted[1] = { CUE_ID_BLUE };
        take_off(CUE_ID_BLUE);
        cue_rules_resolve(&r, B, N, &W, CUE_ID_BLUE, 0, 1, potted, 1);
        char d[96];
        snprintf(d, sizeof d, "foul %d, score %d", r.last_foul, r.score[0]);
        ok(!r.last_foul && r.score[0] == 1,
           "with nothing nominated, any free ball still plays", d);
    }

    /* ---- 6. THE CLEARANCE. A free ball must not skip a colour ------------ *
     *
     * Reported after the nomination fix, which was a different bug in the same
     * rule: "the AI chose free ball, potted the pink nominated as the blue, the
     * pink returned, and it went on to pot pink and black without potting the
     * blue". The blue was never potted and could never be potted again.
     *
     * WPBSA Section 3 Rule 12: the free ball is SPOTTED and the value of the
     * ball on is scored. The ball on has NOT been potted. The sequence stands
     * still — which the earlier tests could not see, because every one of them
     * was played while there were still reds on the table. */
    {
        CueRules r; setup(&r, CUE_ID_PINK);
        r.reds_left = 0;
        r.target = 2;                  /* the clearance */
        r.seq = CUE_ID_BLUE - CUE_ID_YELLOW + 2;   /* on the blue */
        int before = r.score[0];
        int potted[1] = { CUE_ID_PINK };
        take_off(CUE_ID_PINK);
        cue_rules_resolve(&r, B, N, &W, CUE_ID_PINK, 0, 1, potted, 1);
        char d[160];
        snprintf(d, sizeof d, "score %d -> %d, seq %d (blue is %d), "
                 "blue up %d, pink back %d, foul %d",
                 before, r.score[0], r.seq, CUE_ID_BLUE - CUE_ID_YELLOW + 2,
                 ball_on_table(CUE_ID_BLUE), ball_on_table(CUE_ID_PINK), r.last_foul);
        ok(!r.last_foul, "the pink as a free ball on the blue is legal", d);
        ok(r.score[0] == before + 5, "and scores FIVE, the blue's value", d);
        ok(r.seq == CUE_ID_BLUE - CUE_ID_YELLOW + 2,
           "and you are STILL on the blue — the sequence does not move", d);
        ok(ball_on_table(CUE_ID_PINK), "and the pink goes back on its spot", d);
        ok(ball_on_table(CUE_ID_BLUE), "and the blue is still there to be potted", d);
    }

    /* ...and the ball on itself still advances it, which is the other half:
     * a fix that stopped the sequence moving at all would pass the test above
     * and lose every frame. */
    {
        CueRules r; setup(&r, CUE_ID_PINK);
        r.reds_left = 0;
        r.target = 2;
        r.seq = CUE_ID_BLUE - CUE_ID_YELLOW + 2;
        int before = r.score[0];
        int potted[1] = { CUE_ID_BLUE };
        take_off(CUE_ID_BLUE);
        cue_rules_resolve(&r, B, N, &W, CUE_ID_BLUE, 0, 1, potted, 1);
        char d[128];
        snprintf(d, sizeof d, "score %d -> %d, seq %d, blue up %d",
                 before, r.score[0], r.seq, ball_on_table(CUE_ID_BLUE));
        ok(!r.last_foul && r.score[0] == before + 5,
           "potting the BLUE itself scores five", d);
        ok(r.seq == CUE_ID_PINK - CUE_ID_YELLOW + 2,
           "and moves the sequence on to the pink", d);
        ok(!ball_on_table(CUE_ID_BLUE), "and the blue stays down", d);
    }

    printf(s_fail ? "\nFAILED (%d)\n" : "\nPASSED\n", s_fail);
    return s_fail ? 1 : 0;
}
