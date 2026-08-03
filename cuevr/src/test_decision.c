/*
 * CueVR — after a snooker foul, WHO gets the choice?
 *
 * The answer is the player who was fouled against, and CueVR was asking the
 * wrong one: it routed the decision on r->turn, and cue_rules deliberately
 * leaves the turn sitting on the offender until the decision is applied. So you
 * were offered "play on / make them play again" after your OWN foul, and when
 * the CPU fouled and missed it quietly answered for itself and played on.
 *
 * This pins the invariant the routing depends on rather than the routing itself,
 * because that invariant is the surprising part: if cue_rules ever starts moving
 * the turn when it parks a decision, this fails and says so, instead of the app
 * silently going backwards again.
 *
 *   cc -I. -I../../games/thumbycue/src -I../../engine/math -o /tmp/test_decision \
 *      test_decision.c ../../games/thumbycue/src/cue_rules.c \
 *      ../../games/thumbycue/src/cue_table.c -lm && /tmp/test_decision
 */
#include "cue_rules.h"
#include "cue_table.h"

#include <stdio.h>
#include <string.h>

static int fails = 0;

static void ok(const char *what, int cond) {
    printf("%-4s %s\n", cond ? "ok" : "FAIL", what);
    if (!cond) fails = 1;
}
static void eq(const char *what, int got, int want) {
    printf("%-4s %s (got %d, want %d)\n", got == want ? "ok" : "FAIL", what, got, want);
    if (got != want) fails = 1;
}

/* A foul and a miss: the striker had a clear ball-on and hit nothing at all. */
static void foul_and_miss(CueRules *r, CueTable *t, CueWorld *w,
                          CueBall *b, int n, int striker) {
    r->turn = striker;
    r->was_snookered = 0;            /* they could see it, so a miss is called */
    cue_rules_resolve(r, b, n, w, -1 /* hit nothing */, 0, 0, NULL, 0);
    (void)t;
}

int main(void) {
    CueTable t;
    CueWorld w;
    CueBall  b[64];
    int n;

    for (int striker = 0; striker <= 1; striker++) {
        CueRules r;
        cue_table_init(&t, CUE_GAME_SNOOKER);
        cue_table_build_world(&t, &w);
        n = cue_table_rack(&t, b);
        cue_rules_init(&r, &t, 1);               /* player 1 is the CPU */

        printf("\nplayer %d fouls and misses:\n", striker);
        foul_and_miss(&r, &t, &w, b, n, striker);

        eq("  a decision is pending", r.decision, CUE_DEC_PENDING);
        eq("  the offender is the striker", r.dec_offender, striker);
        ok("  a replay is on offer", r.dec_can_restore != 0);

        /* THE point of this file. The turn is still on the offender, so the turn
         * is not the question "whose choice is this?" */
        eq("  the turn has NOT moved off the offender", r.turn, striker);

        int decider = 1 - r.dec_offender;
        eq("  the decider is the other player", decider, 1 - striker);

        /* And once answered, the turn goes where the choice says. */
        int next = cue_rules_apply_decision(&r, CUE_DEC_REPLAY);
        eq("  a replay hands it back to the offender", next, striker);
        eq("  decision cleared", r.decision, CUE_DEC_NONE);
    }

    /* Play-on instead: the fouled-against player takes it on. */
    {
        CueRules r;
        cue_table_init(&t, CUE_GAME_SNOOKER);
        cue_table_build_world(&t, &w);
        n = cue_table_rack(&t, b);
        cue_rules_init(&r, &t, 1);
        printf("\nplayer 1 fouls, player 0 chooses to play on:\n");
        foul_and_miss(&r, &t, &w, b, n, 1);
        int next = cue_rules_apply_decision(&r, CUE_DEC_PLAY);
        eq("  play on gives the table to the non-offender", next, 0);
    }

    printf(fails ? "\nFAILED\n" : "\nall good\n");
    return fails;
}
