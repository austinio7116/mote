/*
 * SPEED POOL — two attempts at the same task, and the clock is the score.
 *
 * There is no turn to take from anybody: each player clears a rack and the
 * lower time wins. So what these check is the shape of the thing — that a
 * clearance ends YOUR go rather than the frame, that the second player gets a
 * fresh rack, and that the winner is the smaller number.
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
    cue_table_init(&T, CUE_GAME_SPEED);
    cue_table_build_world(&T, &W);
    NB = cue_table_rack(&T, B);
    cue_rules_init(r, &T, 0);
    r->break_shot = 0;
}
static void clear_table(void) { for (int i = 1; i < NB; i++) B[i].on = 0; }
static void one_left(void)    { clear_table(); B[1].on = 1; }

int main(void) {
    printf("speed pool\n");

    {   CueRules r; fresh(&r);
        ok(r.target_score == 0, "no target: the score is a time", "");
        ok(cue_rules_ball_legal(&r, B, NB, 7), "any ball is legal", "");
    }

    /* ---- a stroke with balls still up is just a stroke ---- */
    {   CueRules r; fresh(&r);
        one_left();
        int p[1] = { 2 };
        cue_rules_resolve(&r, B, NB, &W, 2, 0, 1, p, 1);
        ok(!r.frame_over && r.turn == 0 && !r.rerack,
           "balls still up: nothing happens but the clock", r.msg);
    }

    /* ---- clearing it ends YOUR go, and racks up for theirs ---- */
    {   CueRules r; fresh(&r);
        r.sp_cs[0] = 4212;                       /* the host's clock, 42.12 s */
        clear_table();
        int p[1] = { 1 };
        cue_rules_resolve(&r, B, NB, &W, 1, 0, 1, p, 1);
        ok(!r.frame_over, "a clearance does not end the frame", r.msg);
        ok(r.turn == 1 && r.rerack == 2 && r.ball_in_hand,
           "...it hands over, with a fresh rack and ball in hand", r.msg);
        ok(r.sp_done[0] && !r.sp_done[1], "one attempt is recorded", "");
    }

    /* ---- and the lower time takes it ---- */
    {   CueRules r; fresh(&r);
        r.sp_done[0] = 1; r.sp_cs[0] = 4212;
        r.turn = 1;       r.sp_cs[1] = 3907;     /* 39.07 s — quicker */
        clear_table();
        int p[1] = { 1 };
        cue_rules_resolve(&r, B, NB, &W, 1, 0, 1, p, 1);
        ok(r.frame_over && r.winner == 1, "the quicker run wins", r.msg);
    }
    {   CueRules r; fresh(&r);
        r.sp_done[0] = 1; r.sp_cs[0] = 3900;
        r.turn = 1;       r.sp_cs[1] = 4500;
        clear_table();
        int p[1] = { 1 };
        cue_rules_resolve(&r, B, NB, &W, 1, 0, 1, p, 1);
        ok(r.frame_over && r.winner == 0, "...whichever player made it", r.msg);
    }
    {   CueRules r; fresh(&r);
        r.sp_done[0] = 1; r.sp_cs[0] = 4000;
        r.turn = 1;       r.sp_cs[1] = 4000;
        clear_table();
        int p[1] = { 1 };
        cue_rules_resolve(&r, B, NB, &W, 1, 0, 1, p, 1);
        ok(r.frame_over && r.winner == -1, "dead level is a dead heat", r.msg);
    }

    /* ---- a scratch costs time, not the table ---- */
    {   CueRules r; fresh(&r);
        one_left();
        cue_rules_resolve(&r, B, NB, &W, 2, 1, 1, NULL, 0);
        ok(r.turn == 0 && r.ball_in_hand && !r.last_foul,
           "a scratch: ball in hand, same player, no foul — the clock is the "
           "penalty", r.msg);
    }

    printf(s_fail ? "\n%d FAILED\n" : "\nall good\n", s_fail);
    return s_fail != 0;
}
