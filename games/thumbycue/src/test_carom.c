/* CAROM: four games, one pocketless table, every point a cannon.
 *
 * The whole question per stroke is what the cue ball's touch log says
 * happened, in order — so the cases here feed exact logs and check the count
 * that matters: cushions BEFORE the second object ball is first reached.
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

static CueTable T;
static CueWorld W;
static CueBall  B[CUE_MAX_BALLS];
static int NB;

static void fresh(CueRules *r, CueGameKind k) {
    cue_table_init(&T, k);
    cue_table_build_world(&T, &W);
    NB = cue_table_rack(&T, B);
    cue_rules_init(r, &T, 0);
    r->break_shot = 0;
}

/* one stroke: the touch log verbatim. 'C' is a cushion, anything else a ball id. */
static void play(CueRules *r, const int *log, int nlog) {
    W.ntouch = 0;
    int first = -1;
    for (int i = 0; i < nlog; i++) {
        if (log[i] < 0) {
            W.touch[W.ntouch].what = CUE_TOUCH_CUSHION;
        } else {
            W.touch[W.ntouch].what = CUE_TOUCH_BALL;
            W.touch[W.ntouch].id = (unsigned char)log[i];
            if (first < 0) first = log[i];
        }
        W.ntouch++;
    }
    r->n_off = 0;
    cue_rules_resolve(r, B, NB, &W, first, 0, 1, NULL, 0);
}
#define C (-1)
#define PLAY(r, ...) do { int L[] = { __VA_ARGS__ }; \
                          play(r, L, (int)(sizeof L / sizeof L[0])); } while (0)
#define Y CUE_ID_BIL_YELLOW
#define RD CUE_ID_BIL_RED

int main(void) {
    printf("carom\n");

    /* ---- the table ---- */
    {   CueTable t; cue_table_init(&t, CUE_GAME_CAROM_3C);
        CueWorld w; cue_table_build_world(&t, &w);
        char d[64];
        snprintf(d, sizeof d, "%.2f x %.2f m, %d pockets, %.1f mm balls",
                 t.half_len * 2, t.half_wid * 2, w.npocket, t.R * 2000);
        ok(w.npocket == 0 && t.half_len * 2 > 2.8f && t.R > 0.030f,
           "a 2.84 m table with no pockets and 61.5 mm balls", d);
    }

    /* ---- straight rail: the cannon is the whole of it ---- */
    {   CueRules r; fresh(&r, CUE_GAME_CAROM_STRAIGHT);
        PLAY(&r, RD, Y);
        ok(r.score[0] == 1 && r.turn == 0, "red then yellow is a point, play on", r.msg);
        PLAY(&r, RD, C);
        ok(r.score[0] == 1 && r.turn == 1, "one object only: no point, turn over", r.msg);
    }

    /* ---- the cushion games count rails BEFORE the second object ---- */
    {   CueRules r; fresh(&r, CUE_GAME_CAROM_3C);
        PLAY(&r, RD, C, C, C, Y);
        ok(r.score[0] == 1, "three rails between the balls is the point", r.msg);
    }
    {   CueRules r; fresh(&r, CUE_GAME_CAROM_3C);
        PLAY(&r, RD, C, C, Y, C);
        ok(r.score[0] == 0 && r.turn == 1,
           "...a rail AFTER the cannon does not count", r.msg);
    }
    {   CueRules r; fresh(&r, CUE_GAME_CAROM_3C);
        PLAY(&r, C, C, C, RD, Y);
        ok(r.score[0] == 1, "rails before EITHER ball count — the bank cannon", r.msg);
    }
    {   CueRules r; fresh(&r, CUE_GAME_CAROM_2C);
        PLAY(&r, RD, C, C, Y);
        ok(r.score[0] == 1, "two rails is the two-cushion point", r.msg);
        PLAY(&r, RD, C, Y);
        ok(r.score[0] == 1 && r.turn == 1, "...one rail is not", r.msg);
    }

    /* ---- the turn swaps the balls, and the objects follow ---- */
    {   CueRules r; fresh(&r, CUE_GAME_CAROM_STRAIGHT);
        PLAY(&r, C);                              /* nothing: turn passes */
        ok(r.turn == 1 && r.bil_yellow == 1, "the yellow takes the table", "");
        ok(cue_rules_ball_legal(&r, B, NB, CUE_ID_BIL_WHITE) &&
           !cue_rules_ball_legal(&r, B, NB, CUE_ID_BIL_YELLOW),
           "...and the WHITE is now an object ball, the yellow is not", "");
    }

    /* ---- four-ball: both reds, and never the other white ---- */
    {   CueRules r; fresh(&r, CUE_GAME_CAROM_4B);
        PLAY(&r, RD, 2);
        ok(r.score[0] == 1, "both reds is the four-ball point", r.msg);
        PLAY(&r, RD, Y, 2);
        ok(r.score[0] == 1 && r.turn == 1,
           "touching the opponent's ball ends the turn scoreless", r.msg);
    }
    {   CueRules r; fresh(&r, CUE_GAME_CAROM_4B);
        ok(cue_rules_ball_legal(&r, B, NB, RD) && cue_rules_ball_legal(&r, B, NB, 2) &&
           !cue_rules_ball_legal(&r, B, NB, Y),
           "four-ball's objects are the two reds alone", "");
    }

    /* ---- the race ends at the target ---- */
    {   CueRules r; fresh(&r, CUE_GAME_CAROM_3C);
        r.score[0] = r.target_score - 1;
        PLAY(&r, RD, C, C, C, Y);
        ok(r.frame_over && r.winner == 0, "the target point is the game", r.msg);
    }

    /* ---- the one foul: a ball off the table goes home ---- */
    {   CueRules r; fresh(&r, CUE_GAME_CAROM_3C);
        Vec3 was = B[1].pos;
        B[1].on = 0; B[1].pos = v3(9, 9, 9);
        W.ntouch = 1;
        W.touch[0].what = CUE_TOUCH_BALL; W.touch[0].id = RD;
        r.n_off = 1;                     /* play() would wipe this: go direct */
        cue_rules_resolve(&r, B, NB, &W, RD, 0, 1, NULL, 0);
        ok(r.last_foul && B[1].on &&
           B[1].pos.x == was.x && B[1].pos.z == was.z && r.turn == 1,
           "off the table: foul, back on its opening spot, turn over", r.msg);
    }

    printf(s_fail ? "\n%d FAILED\n" : "\nall good\n", s_fail);
    return s_fail != 0;
}
