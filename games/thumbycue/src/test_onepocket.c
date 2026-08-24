/*
 * ONE POCKET — each player owns one foot corner, and a ball is only worth
 * something to whoever owns the hole it went down.
 *
 * The whole game is "which pocket", so every case here is about that: the same
 * ball down three different holes is a point to you, a point to them, or a
 * spot, and nothing else in the rules changes between the three.
 */
#include "cue_rules.h"
#include "cue_table.h"
#include <stdio.h>
#include <string.h>

static int s_fail;
static void ok(int cond, const char *what, const char *why) {
    printf("  %s   %s%s%s\n", cond ? "ok  " : "FAIL", what,
           (why && why[0]) ? "   " : "", (why && why[0]) ? why : "");
    if (!cond) s_fail++;
}

static CueTable T;
static CueWorld W;
static CueBall  B[CUE_MAX_BALLS];
static int NB;
static int LP, RP;                     /* the two foot corners */

static void fresh(CueRules *r) {
    cue_table_init(&T, CUE_GAME_ONEPOCKET);
    cue_table_build_world(&T, &W);
    NB = cue_table_rack(&T, B);
    cue_rules_init(r, &T, 0);
    r->break_shot = 0;
    cue_table_foot_pockets(&T, &W, &LP, &RP);
    r->op_hole[0] = LP; r->op_hole[1] = RP;
}

/* one stroke: ball ids down the holes named, in step. */
static void shot(CueRules *r, int first, int scratch, int cushion,
                 const int *ids, const int *holes, int np) {
    for (int i = 0; i < np && i < 8; i++) r->bb_hole[i] = holes[i];
    for (int k = 0; k < np; k++)
        for (int i = 1; i < NB; i++) if (B[i].id == ids[k]) B[i].on = 0;
    cue_rules_resolve(r, B, NB, &W, first, scratch, cushion, ids, np);
}

int main(void) {
    printf("one pocket\n");

    {   CueRules r; fresh(&r);
        ok(LP != RP && LP >= 0, "the table has two foot corners", "");
        ok(r.target_score == 8, "the game is eight of the fifteen", "");
    }

    /* ---- the same ball, three holes ---- */
    {   CueRules r; fresh(&r);
        int id[1] = { 3 }, h[1]; h[0] = LP;
        shot(&r, 3, 0, 1, id, h, 1);
        ok(r.score[0] == 1 && r.turn == 0,
           "your own pocket scores, and you stay at the table", r.msg);
    }
    {   CueRules r; fresh(&r);
        int id[1] = { 3 }, h[1]; h[0] = RP;
        shot(&r, 3, 0, 1, id, h, 1);
        ok(r.score[1] == 1 && r.score[0] == 0 && r.turn == 1,
           "their pocket scores for THEM, and the visit is over", r.msg);
    }
    {   CueRules r; fresh(&r);
        int nb = -1;
        for (int i = 0; i < W.npocket; i++) if (i != LP && i != RP) { nb = i; break; }
        int id[1] = { 3 }, h[1]; h[0] = nb;
        shot(&r, 3, 0, 1, id, h, 1);
        ok(r.score[0] == 0 && r.score[1] == 0 && r.respot == 1 && r.turn == 1,
           "any other pocket is spotted, and the visit is over", r.msg);
    }

    /* ---- a foul costs a BALL, not just the turn ---- */
    {   CueRules r; fresh(&r);
        r.score[0] = 3;
        shot(&r, -1, 0, 0, NULL, NULL, 0);      /* hit nothing at all */
        ok(r.last_foul && r.score[0] == 2 && r.respot >= 1,
           "a foul puts one of your own balls back on the table", r.msg);
        ok(r.turn == 1, "...and the turn goes with it", "");
    }
    {   CueRules r; fresh(&r);
        shot(&r, -1, 0, 0, NULL, NULL, 0);
        ok(r.op_owed[0] == 1 && r.score[0] == 0,
           "with nothing scored yet the ball is OWED", r.msg);
    }
    {   /* and the debt comes out of the next ball scored */
        CueRules r; fresh(&r);
        shot(&r, -1, 0, 0, NULL, NULL, 0);      /* foul: owe one */
        r.turn = 0;
        int id[1] = { 3 }, h[1]; h[0] = LP;
        shot(&r, 3, 0, 1, id, h, 1);
        ok(r.score[0] == 0 && r.op_owed[0] == 0,
           "the next ball scored pays the debt instead of the score", r.msg);
    }

    /* ---- a scratch is in hand as well as a ball ---- */
    {   CueRules r; fresh(&r);
        r.score[0] = 2;
        shot(&r, 3, 1, 1, NULL, NULL, 0);
        ok(r.last_foul && r.ball_in_hand && r.score[0] == 1,
           "a scratch: a ball back, and the cue ball in hand", r.msg);
    }

    /* ---- no cushion and nothing potted is a foul; a pot needs no cushion ---- */
    {   CueRules r; fresh(&r);
        shot(&r, 3, 0, 0, NULL, NULL, 0);
        ok(r.last_foul, "nothing potted and no cushion is a foul", r.msg);
    }
    {   CueRules r; fresh(&r);
        int id[1] = { 3 }, h[1]; h[0] = LP;
        shot(&r, 3, 0, 0, id, h, 1);
        ok(!r.last_foul && r.score[0] == 1,
           "a pot needs no cushion after it", r.msg);
    }

    /* ---- eight wins it ---- */
    {   CueRules r; fresh(&r);
        r.score[0] = 7;
        int id[1] = { 3 }, h[1]; h[0] = LP;
        shot(&r, 3, 0, 1, id, h, 1);
        ok(r.frame_over && r.winner == 0, "eight is the game", r.msg);
    }
    {   /* ...including eight given away into your own pocket by the opponent */
        CueRules r; fresh(&r);
        r.score[1] = 7;
        int id[1] = { 3 }, h[1]; h[0] = RP;
        shot(&r, 3, 0, 1, id, h, 1);
        ok(r.frame_over && r.winner == 1,
           "a ball in their pocket can finish the game for them", r.msg);
    }

    /* ---- two of yours in one stroke is two points and you carry on ---- */
    {   CueRules r; fresh(&r);
        int id[2] = { 3, 5 }; int h[2]; h[0] = LP; h[1] = LP;
        shot(&r, 3, 0, 1, id, h, 2);
        ok(r.score[0] == 2 && r.turn == 0, "two down your own hole is two", r.msg);
    }

    /* ---- AND IT IS THE RIGHT BALL THAT COMES BACK ------------------------
     *
     * The count alone was not enough and this is the case that proves it: score
     * one into your own pocket and drop another into a neutral one on the same
     * stroke. The respot has to name the neutral ball — asked for "one ball
     * back", the table hands over the LOWEST that is off, which is the one you
     * just scored. */
    {   CueRules r; fresh(&r);
        int nb = -1;
        for (int i = 0; i < W.npocket; i++) if (i != LP && i != RP) { nb = i; break; }
        int id[2] = { 3, 9 }; int h[2]; h[0] = LP; h[1] = nb;
        shot(&r, 3, 0, 1, id, h, 2);
        ok(r.score[0] == 1, "the 3 down your own pocket still scores", r.msg);
        {   char m[48]; snprintf(m, sizeof m, "respot_id[0]=%d", r.respot_id[0]);
            ok(r.respot == 1 && r.respot_id[0] == 9,
               "...and it is the 9 that comes back, not the 3", m); }
    }

    printf(s_fail ? "\n%d FAILED\n" : "\nall good\n", s_fail);
    return s_fail != 0;
}
