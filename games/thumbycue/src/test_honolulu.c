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
    memset(W.hit_by_cue, 0, sizeof W.hit_by_cue);
    W.ntouch = 0;
}
static int idx_of(int id) {
    for (int i = 1; i < NB; i++) if (B[i].id == id) return i;
    return -1;
}
/* one stroke, told exactly how the ball got there. `cue` says the white
 * touched it, which is what separates a combination from a straight pot. */
static void shot2(CueRules *r, int id, int rails, int hits, int cue, int kick) {
    const int i = idx_of(id);
    W.ntouch = 0;
    if (kick) { W.touch[W.ntouch].what = CUE_TOUCH_CUSHION; W.ntouch++; }
    W.touch[W.ntouch].what = CUE_TOUCH_BALL;
    W.touch[W.ntouch].id = (unsigned char)id; W.ntouch++;
    if (i > 0) {
        W.rails[i] = (unsigned char)rails;
        W.balls_hit[i] = (unsigned char)hits;
        W.hit_by_cue[i] = (unsigned char)cue;
        B[i].on = 0;
    }
    int p[1] = { id };
    cue_rules_resolve(r, B, NB, &W, id, 0, 1, p, 1);
}
/* the ordinary case: the white struck it and nothing else did */
static void shot(CueRules *r, int id, int rails, int hits, int kick) {
    shot2(r, id, rails, hits, 1, kick);
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
        shot(&r, 3, 0, 2, 0);          /* the white hit it, then it hit another */
        ok(r.score[0] == 1 && r.turn == 0,
           "a carom off it scores", r.msg);
    }
    {   /* A COMBINATION, which is the case Mark found: the white strikes one
         * ball, that ball strikes this one, and this one drops. It has exactly
         * ONE contact — the same count as a ball the white potted itself — so
         * counting contacts read it as a straight pot and scored nothing. What
         * tells them apart is that the WHITE never touched it. */
        CueRules r; fresh(&r);
        shot2(&r, 3, 0, 1, 0, 0);
        ok(r.score[0] == 1 && r.turn == 0,
           "a ball the white never touched came off a combination", r.msg);
    }

    /* ---- a cushion AFTER the first ball is not a kick ---- */
    {   CueRules r; fresh(&r);
        const int i = idx_of(3);
        W.ntouch = 0;
        W.touch[W.ntouch].what = CUE_TOUCH_BALL; W.touch[W.ntouch].id = 3; W.ntouch++;
        W.touch[W.ntouch].what = CUE_TOUCH_CUSHION; W.ntouch++;
        W.rails[i] = 0; W.balls_hit[i] = 1; W.hit_by_cue[i] = 1; B[i].on = 0;
        int p[1] = { 3 };
        cue_rules_resolve(&r, B, NB, &W, 3, 0, 1, p, 1);
        ok(r.score[0] == 0,
           "the cue ball's rail must come BEFORE the ball, or it is no kick",
           r.msg);
    }

    /* ---- a legal score keeps the table, whatever else drops -------------- */
    {   CueRules r; fresh(&r);
        const int ia = idx_of(3), ib = idx_of(9);
        W.ntouch = 0;
        W.touch[0].what = CUE_TOUCH_BALL; W.touch[0].id = 3; W.ntouch = 1;
        W.rails[ia] = 1; W.balls_hit[ia] = 1; W.hit_by_cue[ia] = 1; B[ia].on = 0;
        W.rails[ib] = 0; W.balls_hit[ib] = 1; W.hit_by_cue[ib] = 1; B[ib].on = 0;
        int p[2] = { 3, 9 };
        cue_rules_resolve(&r, B, NB, &W, 3, 0, 1, p, 2);
        ok(r.score[0] == 1, "the banked one still scores", r.msg);
        ok(r.turn == 0,
           "...and you keep the table: the straight one is spotted, not a foul",
           r.msg);
        ok(r.respot == 1 && r.respot_id[0] == 9,
           "...and it is the straight one that goes back", r.msg);
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
