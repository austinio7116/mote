/* SNOOKER SHOOTOUT, the rules half.
 *
 * The format: every stroke must pot a ball or drive one to a cushion; every
 * foul is ball in hand — anywhere on the cloth, not the D — and there is no
 * miss rule and nothing to decide. The clocks and the blue-ball tie-break
 * live in the host; what is tested here is that the resolver enforces the
 * stroke requirement and hands the table over the Shootout way, and that
 * ordinary snooker is untouched by the switch existing.
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
static int      NB;

static void fresh(CueRules *r, int shootout) {
    cue_table_init(&T, CUE_GAME_SNK6);
    cue_table_build_world(&T, &W);
    NB = cue_table_rack(&T, B);
    B[0].on = 1;
    cue_rules_init(r, &T, 0);
    r->snk_shootout = shootout;
    r->break_shot = 0;
}

/* One stroke, told straight to the resolver. */
static void play(CueRules *r, int first, int scratch, int cushion,
                 const int *potted, int np) {
    W.ntouch = 0;
    cue_rules_resolve(r, B, NB, &W, first, scratch, cushion, potted, np);
}

int main(void) {
    printf("snooker shootout\n");

    /* ---- pot a ball or reach a cushion ---- */
    {   CueRules r; fresh(&r, 1);
        play(&r, 1, 0, 0, NULL, 0);   /* hit a red, nothing else */
        ok(r.last_foul, "a stroke with no pot and no cushion is a foul", r.msg);
        ok(r.ball_in_hand, "...and the table changes hands IN HAND", "");
        ok(r.decision == CUE_DEC_NONE, "...with nothing to decide", "");
        ok(cue_rules_in_hand_anywhere(&r), "...and in hand means ANYWHERE", "");
    }
    {   CueRules r; fresh(&r, 1);
        play(&r, 1, 0, 1, NULL, 0);
        ok(!r.last_foul, "the same stroke reaching a cushion is legal", r.msg);
    }
    {   CueRules r; fresh(&r, 1);
        int pot[1] = { 1 };
        for (int i = 1; i < NB; i++) if (B[i].id == 1) B[i].on = 0;
        play(&r, 1, 0, 0, pot, 1);
        ok(!r.last_foul && r.score[0] == 1,
           "a pot needs no cushion, and scores as snooker", r.msg);
    }

    /* ---- every foul is in hand, no miss machinery ---- */
    {   CueRules r; fresh(&r, 1);
        play(&r, -1, 0, 1, NULL, 0);            /* hit nothing at all */
        ok(r.last_foul && r.ball_in_hand,
           "an air shot: foul, in hand to the opponent", r.msg);
        ok(!r.last_miss && r.decision == CUE_DEC_NONE,
           "...and no miss is called, no replay offered", "");
    }
    {   CueRules r; fresh(&r, 1);
        play(&r, 1, 1, 1, NULL, 0);   /* scratch off a legal contact */
        ok(r.last_foul && r.ball_in_hand, "a scratch: foul, in hand", r.msg);
    }

    /* ---- plain snooker does not feel the switch ---- */
    {   CueRules r; fresh(&r, 0);
        play(&r, 1, 0, 0, NULL, 0);
        ok(!r.last_foul, "ordinary snooker: no pot, no cushion, no foul", r.msg);
        ok(!cue_rules_in_hand_anywhere(&r), "...and in hand still means the D", "");
    }

    printf(s_fail ? "\n%d FAILED\n" : "\nall good\n", s_fail);
    return s_fail != 0;
}
