/*
 * HONOLULU — a straight-in pot scores nothing.
 *
 * Every scoring ball has to arrive off a bank, off a kick, or through another
 * ball. The same pot is a point or a spotted ball and a lost visit depending on
 * nothing but how it got there, so that is what every case here changes.
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
    cue_table_init(&T, CUE_GAME_HONOLULU);
    cue_table_build_world(&T, &W);
    NB = cue_table_rack(&T, B);
    cue_rules_init(r, &T, 0);
    r->break_shot = 0;
    memset(W.rails, 0, sizeof W.rails);
    memset(W.balls_hit, 0, sizeof W.balls_hit);
    W.ntouch = 0;
}
static int idx_of(int id) {
    for (int i = 1; i < NB; i++) if (B[i].id == id) return i;
    return -1;
}
/* one stroke, told exactly how the ball got there. */
static void shot(CueRules *r, int id, int rails, int hits, int kick) {
    const int i = idx_of(id);
    W.ntouch = 0;
    if (kick) { W.touch[W.ntouch].what = CUE_TOUCH_CUSHION; W.ntouch++; }
    W.touch[W.ntouch].what = CUE_TOUCH_BALL;
    W.touch[W.ntouch].id = (unsigned char)id; W.ntouch++;
    if (i > 0) {
        W.rails[i] = (unsigned char)rails;
        W.balls_hit[i] = (unsigned char)hits;
        B[i].on = 0;
    }
    int p[1] = { id };
    cue_rules_resolve(r, B, NB, &W, id, 0, 1, p, 1);
}

int main(void) {
    printf("honolulu\n");

    {   CueRules r; fresh(&r);
        ok(r.target_score == 8, "first to eight", "");
    }

    /* ---- the same pot, four ways ---- */
    {   CueRules r; fresh(&r);
        shot(&r, 3, 0, 1, 0);          /* cue ball straight onto it, straight in */
        ok(r.score[0] == 0 && r.respot == 1 && r.turn == 1,
           "straight in: no score, spotted, visit over", r.msg);
    }
    {   CueRules r; fresh(&r);
        shot(&r, 3, 1, 1, 0);          /* it banked */
        ok(r.score[0] == 1 && r.turn == 0, "off a bank it scores", r.msg);
    }
    {   CueRules r; fresh(&r);
        shot(&r, 3, 0, 1, 1);          /* the cue ball found a rail first */
        ok(r.score[0] == 1 && r.turn == 0, "off a kick it scores", r.msg);
    }
    {   CueRules r; fresh(&r);
        shot(&r, 3, 0, 2, 0);          /* it came through another ball */
        ok(r.score[0] == 1 && r.turn == 0,
           "through another ball it scores", r.msg);
    }

    /* ---- a cushion AFTER the first ball is not a kick ---- */
    {   CueRules r; fresh(&r);
        const int i = idx_of(3);
        W.ntouch = 0;
        W.touch[W.ntouch].what = CUE_TOUCH_BALL; W.touch[W.ntouch].id = 3; W.ntouch++;
        W.touch[W.ntouch].what = CUE_TOUCH_CUSHION; W.ntouch++;
        W.rails[i] = 0; W.balls_hit[i] = 1; B[i].on = 0;
        int p[1] = { 3 };
        cue_rules_resolve(&r, B, NB, &W, 3, 0, 1, p, 1);
        ok(r.score[0] == 0,
           "the cue ball's rail must come BEFORE the ball, or it is no kick",
           r.msg);
    }

    /* ---- eight wins it, and a straight one cannot ---- */
    {   CueRules r; fresh(&r); r.score[0] = 7;
        shot(&r, 3, 1, 1, 0);
        ok(r.frame_over && r.winner == 0, "eight is the game", r.msg);
    }
    {   CueRules r; fresh(&r); r.score[0] = 7;
        shot(&r, 3, 0, 1, 0);
        ok(!r.frame_over && r.score[0] == 7,
           "a straight one cannot finish it", r.msg);
    }

    /* ---- a straight pot is no score, but it is not a foul ---- */
    {   CueRules r; fresh(&r);
        shot(&r, 3, 0, 1, 0);
        ok(!r.last_foul, "straight in is no score and no foul", r.msg);
    }

    printf(s_fail ? "\n%d FAILED\n" : "\nall good\n", s_fail);
    return s_fail != 0;
}
