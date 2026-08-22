/* CALLED POCKETS ON THE 8-BALL TABLES.
 *
 * Two strengths, because the codes differ: WPA 8-ball calls ball and pocket on
 * every stroke after the break, while Chinese 8-ball exempts the obvious shots
 * and asks only that the BLACK be called — which is also what most pub tables
 * play. Neither calls on the break. Missing your call is not a foul: the balls
 * stay down and the table passes.
 */
#include "cue_rules.h"
#include "cue_table.h"
#include <stdio.h>
#include <string.h>

static int s_fail;
static void ok(int cond, const char *what, const char *detail) {
    printf("  %-4s %s%s%s\n", cond ? "ok" : "FAIL", what,
           detail && detail[0] ? "   " : "", detail ? detail : "");
    if (!cond) s_fail++;
}

static CueTable T; static CueWorld W; static CueBall B[CUE_MAX_BALLS]; static int NB;

static void fresh(CueRules *r, CueGameKind k, int level) {
    cue_table_init(&T, k);
    cue_table_build_world(&T, &W);
    NB = cue_table_rack(&T, B);
    B[0].on = 1;
    cue_rules_init(r, &T, 0);
    r->call_shot_on = level;
    r->break_shot = 0;
    r->ball_in_hand = 0;
}

/* pot `id` into pocket `pk`, having called `cid` into `cpk` */
static void play(CueRules *r, int cid, int cpk, int first, int id, int pk) {
    W.ntouch = 0;
    cue_rules_call_shot(r, cid, cpk);
    int potted[4]; int np = 0;
    if (id) {
        for (int i = 1; i < NB; i++)
            if (B[i].on && B[i].id == id) {
                B[i].on = 0;
                B[i].pocket = (unsigned char)(pk < 0 ? 0 : pk);
                potted[np++] = id;
                break;
            }
    }
    r->n_off = 0;
    cue_rules_resolve(r, B, NB, &W, first, 0, 1, potted, np);
}

int main(void) {
    printf("called pockets on the 8-ball tables\n");

    /* ---- level 2: every stroke, WPA ---- */
    {   CueRules r; fresh(&r, CUE_GAME_US8, 2);
        r.open = 0; r.group[0] = 1; r.group[1] = 2;
        play(&r, 3, 2, 3, 3, 2);                    /* called the 3 into pocket 2 */
        ok(!r.last_foul && r.turn == 0, "as called: pot stands and you carry on", r.msg);
        play(&r, 5, 0, 5, 5, 3);                    /* called pocket 0, went down 3 */
        ok(!r.last_foul && r.turn == 1, "not as called: no foul, but the table passes", r.msg);
        int down = 0;
        for (int i = 1; i < NB; i++) if (!B[i].on && B[i].id == 5) down = 1;
        ok(down, "...and the ball stays down", "");
    }
    /* the break is never called */
    {   CueRules r; fresh(&r, CUE_GAME_US8, 2);
        r.break_shot = 1;
        play(&r, 0, -1, 1, 1, 4);                   /* nothing called at all */
        ok(!r.last_foul && r.turn == 0, "nothing is called on the break", r.msg);
    }
    /* the table stays open until a CALLED ball goes */
    {   CueRules r; fresh(&r, CUE_GAME_US8, 2);
        play(&r, 5, 0, 5, 5, 3);                    /* slop on an open table */
        ok(r.open, "slop leaves the table open (WPA 8.3)", r.msg);
    }

    /* ---- level 1: the black, and only the black ---- */
    {   CueRules r; fresh(&r, CUE_GAME_CN8, 1);
        r.open = 0; r.group[0] = 1; r.group[1] = 2;
        play(&r, 0, -1, 3, 3, 2);                   /* nothing called, own ball down */
        ok(!r.last_foul && r.turn == 0,
           "the ordinary shot needs no call at this level", r.msg);
    }
    {   CueRules r; fresh(&r, CUE_GAME_CN8, 1);
        /* clear our group, then the black in the WRONG pocket */
        r.open = 0; r.group[0] = 1; r.group[1] = 2;
        for (int i = 1; i < NB; i++)
            if (B[i].on && B[i].id >= 1 && B[i].id <= 7) B[i].on = 0;
        play(&r, 8, 1, 8, 8, 4);
        ok(r.frame_over && r.winner == 1,
           "the black in a pocket you did not call is loss of frame", r.msg);
    }
    {   CueRules r; fresh(&r, CUE_GAME_CN8, 1);
        r.open = 0; r.group[0] = 1; r.group[1] = 2;
        for (int i = 1; i < NB; i++)
            if (B[i].on && B[i].id >= 1 && B[i].id <= 7) B[i].on = 0;
        play(&r, 8, 4, 8, 8, 4);
        ok(r.frame_over && r.winner == 0, "the black as called wins it", r.msg);
    }

    /* ---- level 0: Heyball and WPA-standard call nothing ---- */
    {   CueRules r; fresh(&r, CUE_GAME_CN8, 0);
        r.open = 0; r.group[0] = 1; r.group[1] = 2;
        for (int i = 1; i < NB; i++)
            if (B[i].on && B[i].id >= 1 && B[i].id <= 7) B[i].on = 0;
        play(&r, 0, -1, 8, 8, 4);
        ok(r.frame_over && r.winner == 0,
           "with calling off, the black anywhere wins it", r.msg);
    }

    printf(s_fail ? "\n%d FAILED\n" : "\nall good\n", s_fail);
    return s_fail != 0;
}
