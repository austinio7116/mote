/*
 * BANK POOL — every scoring ball must come off a cushion first.
 *
 * One rule, and it is the whole game: the same pot is a point or a spotted
 * ball and a lost visit depending on nothing but whether the OBJECT ball found
 * a rail on the way. So that is what these check, either side of the line.
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

static void fresh(CueRules *r) {
    cue_table_init(&T, CUE_GAME_BANKPOOL);
    cue_table_build_world(&T, &W);
    NB = cue_table_rack(&T, B);
    cue_rules_init(r, &T, 0);
    r->break_shot = 0;
    memset(W.rails, 0, sizeof W.rails);
}

static int idx_of(int id) {
    for (int i = 1; i < NB; i++) if (B[i].id == id) return i;
    return -1;
}
/* one stroke: these ids potted, each with that many rails of its own. */
static void shot(CueRules *r, int first, int scratch, int cushion,
                 const int *ids, const int *rails, int np) {
    for (int k = 0; k < np; k++) {
        const int i = idx_of(ids[k]);
        if (i > 0) { W.rails[i] = (unsigned char)rails[k]; B[i].on = 0; }
    }
    cue_rules_resolve(r, B, NB, &W, first, scratch, cushion, ids, np);
}

int main(void) {
    printf("bank pool\n");

    {   CueRules r; fresh(&r);
        ok(r.target_score == 8, "the game is eight of the fifteen", "");
    }

    /* ---- the same pot, either side of the one rule ---- */
    {   CueRules r; fresh(&r);
        int id[1] = { 3 }, rl[1] = { 1 };
        shot(&r, 3, 0, 1, id, rl, 1);
        ok(r.score[0] == 1 && r.turn == 0,
           "a ball off one cushion scores, and you stay at the table", r.msg);
    }
    {   CueRules r; fresh(&r);
        int id[1] = { 3 }, rl[1] = { 0 };
        shot(&r, 3, 0, 1, id, rl, 1);
        ok(r.score[0] == 0 && r.respot == 1 && r.turn == 1,
           "the same pot with no rail scores nothing and is spotted", r.msg);
    }
    {   CueRules r; fresh(&r);
        int id[1] = { 3 }, rl[1] = { 4 };
        shot(&r, 3, 0, 1, id, rl, 1);
        ok(r.score[0] == 1, "more rails than one is still one point", r.msg);
    }

    /* ---- a stroke that does both keeps the point and loses the table ---- */
    {   CueRules r; fresh(&r);
        int id[2] = { 3, 5 }, rl[2] = { 2, 0 };
        shot(&r, 3, 0, 1, id, rl, 2);
        ok(r.score[0] == 1 && r.respot == 1 && r.turn == 1,
           "banked one and dropped another: one point, one spot, visit over",
           r.msg);
    }
    {   CueRules r; fresh(&r);
        int id[2] = { 3, 5 }, rl[2] = { 1, 3 };
        shot(&r, 3, 0, 1, id, rl, 2);
        ok(r.score[0] == 2 && r.turn == 0,
           "two banked in one stroke is two, and you play on", r.msg);
    }

    /* ---- a foul costs a ball, as it does at one pocket ---- */
    {   CueRules r; fresh(&r);
        r.score[0] = 4;
        shot(&r, -1, 0, 0, NULL, NULL, 0);
        ok(r.last_foul && r.score[0] == 3 && r.turn == 1,
           "a foul puts one of your own balls back", r.msg);
    }
    {   CueRules r; fresh(&r);
        shot(&r, -1, 0, 0, NULL, NULL, 0);
        ok(r.op_owed[0] == 1, "with nothing scored the ball is owed", r.msg);
    }
    {   CueRules r; fresh(&r);
        r.score[0] = 2;
        shot(&r, 3, 1, 1, NULL, NULL, 0);
        ok(r.last_foul && r.ball_in_hand && r.score[0] == 1,
           "a scratch: a ball back and the cue ball in hand", r.msg);
    }
    {   /* an unbanked pot is not a foul — it is simply no score */
        CueRules r; fresh(&r);
        int id[1] = { 3 }, rl[1] = { 0 };
        shot(&r, 3, 0, 0, id, rl, 1);
        ok(!r.last_foul, "an unbanked pot is no score, but no foul either", r.msg);
    }

    /* ---- eight wins it ---- */
    {   CueRules r; fresh(&r);
        r.score[0] = 7;
        int id[1] = { 3 }, rl[1] = { 1 };
        shot(&r, 3, 0, 1, id, rl, 1);
        ok(r.frame_over && r.winner == 0, "eight is the game", r.msg);
    }
    {   /* ...and a ball that never banked cannot win it */
        CueRules r; fresh(&r);
        r.score[0] = 7;
        int id[1] = { 3 }, rl[1] = { 0 };
        shot(&r, 3, 0, 1, id, rl, 1);
        ok(!r.frame_over && r.score[0] == 7,
           "an unbanked ball cannot finish the game", r.msg);
    }

    printf(s_fail ? "\n%d FAILED\n" : "\nall good\n", s_fail);
    return s_fail != 0;
}
