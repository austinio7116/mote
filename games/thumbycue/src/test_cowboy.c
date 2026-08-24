/*
 * COWBOY POOL — three balls, a hundred and one points, and a game that changes
 * its own rules twice on the way there.
 *
 * Everything scores to 90; from 91 only cannons; the last point is a cannon off
 * the 1. And nothing may overshoot. Those four facts ARE the game, so they are
 * what these check.
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

static void fresh(CueRules *r, int score) {
    cue_table_init(&T, CUE_GAME_COWBOY);
    cue_table_build_world(&T, &W);
    NB = cue_table_rack(&T, B);
    cue_rules_init(r, &T, 0);
    r->break_shot = 0;
    r->score[0] = score;
}
/* one stroke: the cue ball's touch log, then what dropped. */
static void shot(CueRules *r, const int *touch, int nt, int scratch,
                 const int *pots, int np) {
    W.ntouch = 0;
    int first = -1;
    for (int i = 0; i < nt; i++) {
        W.touch[W.ntouch].what = CUE_TOUCH_BALL;
        W.touch[W.ntouch].id = (unsigned char)touch[i];
        if (first < 0) first = touch[i];
        W.ntouch++;
    }
    for (int k = 0; k < np; k++)
        for (int i = 1; i < NB; i++) if (B[i].id == pots[k]) B[i].on = 0;
    cue_rules_resolve(r, B, NB, &W, first, scratch, 1, pots, np);
}

int main(void) {
    printf("cowboy pool\n");

    {   CueRules r; fresh(&r, 0);
        ok(r.target_score == 101, "a hundred and one", "");
        ok(NB == 4, "three balls on the table and the cue ball", "");
        ok(cue_rules_ball_legal(&r, B, NB, 1) &&
           cue_rules_ball_legal(&r, B, NB, 3) &&
           cue_rules_ball_legal(&r, B, NB, 5) &&
           !cue_rules_ball_legal(&r, B, NB, 2),
           "the 1, the 3 and the 5, and nothing else", "");
    }

    /* ---- to ninety, everything counts ---- */
    {   CueRules r; fresh(&r, 0);
        int t[1] = { 5 }, p[1] = { 5 };
        shot(&r, t, 1, 0, p, 1);
        ok(r.score[0] == 5 && r.turn == 0, "a potted 5 is five, and you play on", r.msg);
        ok(r.respot == 1, "...and it goes straight back on its spot", "");
    }
    {   CueRules r; fresh(&r, 0);
        int t[2] = { 3, 5 };
        shot(&r, t, 2, 0, NULL, 0);
        ok(r.score[0] == 1, "a cannon off two balls is one", r.msg);
    }
    {   CueRules r; fresh(&r, 0);
        int t[1] = { 3 };
        shot(&r, t, 1, 1, NULL, 0);
        ok(r.score[0] == 1 && r.ball_in_hand, "an in-off is one, and in hand", r.msg);
    }
    {   /* pot and cannon at once */
        CueRules r; fresh(&r, 0);
        int t[2] = { 1, 3 }, p[1] = { 3 };
        shot(&r, t, 2, 0, p, 1);
        ok(r.score[0] == 4, "the 3 potted with a cannon is three and one", r.msg);
    }

    /* ---- from ninety-one, cannons only ---- */
    {   CueRules r; fresh(&r, 92);
        int t[1] = { 5 }, p[1] = { 5 };
        shot(&r, t, 1, 0, p, 1);
        ok(r.score[0] == 92 && r.turn == 1,
           "past ninety a potted ball scores nothing", r.msg);
    }
    {   CueRules r; fresh(&r, 92);
        int t[2] = { 3, 5 };
        shot(&r, t, 2, 0, NULL, 0);
        ok(r.score[0] == 93 && r.turn == 0, "a cannon still scores its one", r.msg);
    }

    /* ---- and the hundred-and-first is its own shot ---- */
    {   CueRules r; fresh(&r, 100);
        int t[2] = { 3, 5 };
        shot(&r, t, 2, 0, NULL, 0);
        ok(!r.frame_over && r.score[0] == 100,
           "on a hundred, a cannon off the 3 is not the winning one", r.msg);
    }
    {   CueRules r; fresh(&r, 100);
        int t[2] = { 1, 5 };
        shot(&r, t, 2, 0, NULL, 0);
        ok(r.frame_over && r.winner == 0,
           "a cannon off the 1 FIRST wins it", r.msg);
    }

    /* ---- nothing may overshoot ---- */
    {   /* THE CEILING BEFORE NINETY IS NINETY, not a hundred and one: pots
         * carry you no further, so the run-in is a counting problem. */
        CueRules r; fresh(&r, 88);
        int t[1] = { 5 }, p[1] = { 5 };
        shot(&r, t, 1, 0, p, 1);
        ok(r.score[0] == 88 && r.turn == 1,
           "on 88 the 5 would pass ninety, so it scores nothing", r.msg);
    }
    {   CueRules r; fresh(&r, 87);
        int t[1] = { 3 }, p[1] = { 3 };
        shot(&r, t, 1, 0, p, 1);
        ok(r.score[0] == 90 && r.turn == 0,
           "on 87 the 3 arrives exactly on ninety, and counts", r.msg);
    }
    {   CueRules r; fresh(&r, 99);
        int t[2] = { 3, 5 };
        shot(&r, t, 2, 0, NULL, 0);
        ok(r.score[0] == 100 && !r.frame_over,
           "the cannons carry you to a hundred and stop there", r.msg);
    }

    printf(s_fail ? "\n%d FAILED\n" : "\nall good\n", s_fail);
    return s_fail != 0;
}
