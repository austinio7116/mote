/* The concede rule, and that a free ball actually arms. */
#include "cue_rules.h"
#include "cue_table.h"
#include <stdio.h>
static int fails;
static void ok(int c, const char *m){printf("%-4s %s\n",c?"ok":"FAIL",m); if(!c)fails++;}

int main(void) {
    CueTable t; CueWorld w; CueBall b[CUE_MAX_BALLS]; CueRules r;
    cue_table_init(&t, CUE_GAME_SNK15);
    cue_table_build_world(&t, &w);
    cue_table_rack(&t, b);
    cue_rules_init(&r, &t, 1);

    /* colours only: 27 left on the table */
    r.reds_left = 0; r.seq = 2; r.target = 2;
    r.score[0] = 100; r.score[1] = 0;
    /* deficit 100, remaining 27 → 73 points of snookers: hopeless */
    ok(cue_rules_should_concede(&r, 1) == 1, "hopeless on the colours: concede");
    r.score[0] = 27 + 13;   /* 13 past what is left */
    ok(cue_rules_should_concede(&r, 1) == 1, "13 points of snookers behind: concede");
    r.score[0] = 27 + 12;   /* exactly 12 past */
    ok(cue_rules_should_concede(&r, 1) == 0, "exactly 12: keep playing");
    r.score[0] = 27 + 4;
    ok(cue_rules_should_concede(&r, 1) == 0, "one snooker behind: keep playing");
    r.score[0] = 20;        /* still catchable outright */
    ok(cue_rules_should_concede(&r, 1) == 0, "behind but catchable: keep playing");
    r.score[0] = 0; r.score[1] = 50;
    ok(cue_rules_should_concede(&r, 1) == 0, "in front: never");

    /* with reds up, remaining is large, so it takes a lot */
    cue_rules_init(&r, &t, 1);
    r.reds_left = 15;                       /* 15*8 + 27 = 147 */
    r.score[0] = 147 + 13; r.score[1] = 0;
    ok(cue_rules_should_concede(&r, 1) == 1, "same rule with reds up");
    r.score[0] = 147 + 12;
    ok(cue_rules_should_concede(&r, 1) == 0, "...and the same threshold");

    /* 6-red: a free ball offered and taken actually arms one */
    cue_table_init(&t, CUE_GAME_SNK6);
    cue_table_build_world(&t, &w);
    int n = cue_table_rack(&t, b);
    cue_rules_init(&r, &t, 1);
    r.dec_free_ball = 1; r.dec_offender = 1; r.decision = CUE_DEC_PENDING;
    cue_rules_apply_decision(&r, CUE_DEC_FREEBALL);
    ok(r.free_ball == 1, "taking a free ball sets the flag");
    ok(r.free_ball_id == 0, "...with nothing named yet — it is yours to pick");
    /* name one */
    int pick = 0;
    for (int i = 1; i < n; i++) if (b[i].on) { pick = b[i].id; break; }
    cue_rules_nominate_free(&r, pick);
    ok(r.free_ball_id == pick, "and a ball can be named");
    /* not offered when it was not awarded */
    cue_rules_init(&r, &t, 1);
    r.dec_free_ball = 0; r.dec_offender = 1;
    cue_rules_apply_decision(&r, CUE_DEC_FREEBALL);
    ok(r.free_ball == 0, "no free ball unless one was actually on offer");

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}
