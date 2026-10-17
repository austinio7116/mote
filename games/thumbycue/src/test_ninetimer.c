/*
 * NINE-BALL AGAINST THE CLOCK, AND WINNER BREAKS (CueVR 7.6.5).
 *
 * The rules' half of the World Nineball Tour's shot clock: which shot has the
 * long clock (the shot after the break, a push-out and the shot after it), and
 * that a rack's extensions start unused. And winner breaks: the next frame of a
 * match is broken by whoever won the last, where it otherwise alternates. Both
 * are match settings and must survive the next frame.
 *
 *   cc -O2 -DMOTE_HOST=1 -DCUE_JAW_SEGS=10 -DCUE_ARC_SEGS=20 -DCUE_MAX_SEG=256 \
 *      -I. -I../../../engine/core -I../../../engine/math -I../../../engine/physics \
 *      -o /tmp/t_ninetimer test_ninetimer.c cue_rules.c cue_table.c cue_physics.c \
 *      ../../../engine/physics/mote_phys.c ../../../engine/core/mote_arena.c -lm && /tmp/t_ninetimer
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

static void fresh(CueRules *r, CueGameKind k, int clock, int wb) {
    cue_table_init(&T, k);
    cue_table_build_world(&T, &W);
    NB = cue_table_rack(&T, B);
    cue_rules_init(r, &T, 0);
    cue_rules_set_rot(r, clock, wb);
    cue_rules_set_break(r, 0);
    r->best_of = 5;
}
static void off(int id) { for (int i = 1; i < NB; i++) if (B[i].id == id) B[i].on = 0; }
static void shot(CueRules *r, int first, int scratch, int cushion, const int *ids, int np) {
    for (int k = 0; k < np; k++) off(ids[k]);
    cue_rules_resolve(r, B, NB, &W, first, scratch, cushion, ids, np);
}
/* the frame won by `w`, as book_frame leaves it, then the next one racked */
static void won(CueRules *r, int w) {
    r->frame_over = 1; r->winner = w; r->frames[w]++;
    cue_rules_next_frame(r, &T);
}

int main(void) {
    printf("nine-ball's shot clock\n");
    {   CueRules r; fresh(&r, CUE_GAME_US9, 1, 0);
        ok(r.rot_clock == 1 && r.clk_long == 0, "the break is a shot of its own, no long clock", "");
        ok(r.clk_ext[0] == 0 && r.clk_ext[1] == 0, "both extensions unused at the rack", "");
        int id[1] = { 3 };
        r.break_shot = 1;
        shot(&r, 1, 0, 1, id, 1);                 /* the break pots the 3 */
        ok(r.clk_long == 1, "the shot after the break has the long clock", r.msg);
        ok(r.pushout_offer == 1, "...and may be a push-out", r.msg);
        r.pushout_offer = 0; r.is_pushout = 1;
        shot(&r, 1, 0, 1, NULL, 0);               /* the push-out */
        ok(r.clk_long == 1, "the shot after a push-out has the long clock", r.msg);
        r.pushout_resp = 0;
        shot(&r, 1, 0, 1, NULL, 0);               /* an ordinary shot, a miss */
        ok(r.clk_long == 0, "...and the one after that the short", r.msg);
        r.clk_ext[0] = 1;
        won(&r, 0);
        ok(r.rot_clock == 1, "the shot clock is the match's: it survives the next frame", "");
        ok(r.clk_ext[0] == 0 && r.clk_ext[1] == 0, "a new rack, the extensions back", "");
    }
    {   CueRules r; fresh(&r, CUE_GAME_US9, 0, 0);
        r.break_shot = 1;
        int id[1] = { 3 };
        shot(&r, 1, 0, 1, id, 1);
        ok(r.rot_clock == 0, "without the clock, nothing is timed", "");
    }

    printf("winner breaks\n");
    {   CueRules r; fresh(&r, CUE_GAME_US9, 0, 1);
        won(&r, 1);
        ok(r.turn == 1, "frame 1 won by player 1: player 1 breaks frame 2", "");
        won(&r, 1);
        ok(r.turn == 1, "...and wins it: breaks frame 3 as well", "");
        won(&r, 0);
        ok(r.turn == 0, "player 0 wins frame 3: breaks frame 4", "");
        ok(r.winner_breaks == 1 && r.break_first == 0, "the setting and the first breaker are the match's", "");
    }
    {   CueRules r; fresh(&r, CUE_GAME_US10, 0, 1);
        won(&r, 1); ok(r.turn == 1, "ten-ball too: the winner breaks", "");
    }
    {   CueRules r; fresh(&r, CUE_GAME_US9, 0, 0);
        won(&r, 1);
        ok(r.turn == 1, "alternate breaks: frame 2 is player 1's", "");
        won(&r, 1);
        ok(r.turn == 0, "...frame 3 player 0's, whoever won", "");
    }
    printf(s_fail ? "%d FAILED\n" : "all ok\n", s_fail);
    return s_fail ? 1 : 0;
}
