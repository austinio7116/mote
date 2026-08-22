/* KILLER, the pub elimination game, two players.
 *
 * One shot each, strictly alternating. Pot any object ball and you are safe;
 * fail to pot — or foul — and one of your three lives goes. A scratch is a
 * life AND ball in hand. The opening break is exempt from the life, the rack
 * goes back on when the table runs dry, and the frame ends when somebody is
 * out of lives.
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
    B[0].on = 1;
    cue_rules_init(r, &T, 0);
    r->ball_in_hand = 0;
}

static void play(CueRules *r, int first, int scratch, const int *pot, int np) {
    W.ntouch = 0;
    /* the potted leave the table, as resolve_shot would have made true */
    for (int k = 0; k < np; k++)
        for (int i = 1; i < NB; i++)
            if (B[i].on && B[i].id == pot[k]) { B[i].on = 0; break; }
    r->n_off = 0;
    cue_rules_resolve(r, B, NB, &W, first, scratch, 1, pot, np);
}

int main(void) {
    printf("killer\n");

    /* ---- the three tables are the base games' tables ---- */
    {   CueTable uk, us, cn, b8;
        cue_table_init(&uk, CUE_GAME_KILLER_UK);
        cue_table_init(&us, CUE_GAME_KILLER_US);
        cue_table_init(&cn, CUE_GAME_KILLER_CN);
        cue_table_init(&b8, CUE_GAME_UK8);
        char d[64];
        snprintf(d, sizeof d, "uk %.2fm us %.2fm cn %.2fm",
                 uk.half_len * 2, us.half_len * 2, cn.half_len * 2);
        ok(uk.half_len == b8.half_len && uk.R == b8.R &&
           us.half_len > uk.half_len && cn.half_len > us.half_len,
           "UK / US / Chinese killer borrow their base tables", d);
        ok(uk.kind == CUE_GAME_KILLER_UK, "...and keep their own kind", "");
    }

    /* ---- lives, and how they go ---- */
    {   CueRules r; fresh(&r, CUE_GAME_KILLER_UK);
        ok(r.score[0] == 3 && r.score[1] == 3, "three lives each", "");
        play(&r, 1, 0, NULL, 0);                     /* dry BREAK: exempt */
        ok(r.score[0] == 3 && r.turn == 1, "a dry break costs nothing", r.msg);
        play(&r, 1, 0, NULL, 0);                     /* dry ordinary shot */
        ok(r.score[1] == 2 && r.turn == 0, "a dry shot after it is a life", r.msg);
        int pot[1] = { 5 };
        play(&r, 1, 0, pot, 1);
        ok(r.score[0] == 3 && r.turn == 1, "a pot is safe — and the turn STILL passes", r.msg);
        play(&r, 1, 1, NULL, 0);                     /* scratch */
        ok(r.score[1] == 1 && r.ball_in_hand, "a scratch: a life and ball in hand", r.msg);
        play(&r, 1, 0, pot + 0, 0);                  /* dry again: last life */
        r.turn = 1;                                  /* force their shot */
        play(&r, -1, 0, NULL, 0);                    /* air shot: out */
        ok(r.frame_over && r.winner == 0 && r.score[1] == 0,
           "out of lives is the frame", r.msg);
    }

    /* ---- any ball is legal ---- */
    {   CueRules r; fresh(&r, CUE_GAME_KILLER_US);
        ok(cue_rules_ball_legal(&r, B, NB, 3) && cue_rules_ball_legal(&r, B, NB, 12),
           "pot what you like: every object ball is legal", "");
    }

    /* ---- the rack comes back when the table runs dry ---- */
    {   CueRules r; fresh(&r, CUE_GAME_KILLER_UK);
        r.break_shot = 0;
        int pot[CUE_MAX_BALLS], np = 0;
        for (int i = 1; i < NB; i++) if (B[i].on) pot[np++] = B[i].id;
        play(&r, 1, 0, pot, np);                     /* everything down at once */
        ok(r.rerack == 2 && !r.frame_over, "an empty table calls for the rack", r.msg);
    }

    printf(s_fail ? "\n%d FAILED\n" : "\nall good\n", s_fail);
    return s_fail != 0;
}
