/*
 * ThumbyCue — matchplay golf is a different game on the same holes.
 *
 * Strokeplay adds every stroke up and the lowest total wins the round.
 * Matchplay throws the totals away: each hole is won, lost or halved on its
 * own, and the match is decided on holes up with holes remaining — so it can
 * be over before the holes are, and a disaster costs one hole rather than the
 * round. Both of those are worth a test, because both are the SHAPE of the
 * game rather than an arithmetic detail.
 */
#include "cue_table.h"
#include "cue_rules.h"
#include <stdio.h>
#include <string.h>

static int fails;
static void ok(int c, const char *what) {
    if (!c) { printf("  FAIL  %s\n", what); fails++; }
    else    printf("  ok    %s\n", what);
}

int main(void) {
    printf("golf matchplay\n");

    /* the rounds line up: a matchplay round plays the same holes as the
     * strokeplay one it copies */
    ok(cue_golf_first(CUE_GOLF_M18)     == cue_golf_first(CUE_GOLF_18) &&
       cue_golf_last(CUE_GOLF_M18)      == cue_golf_last(CUE_GOLF_18),
       "matchplay 18 plays the same holes as 18");
    ok(cue_golf_first(CUE_GOLF_MFRONT9) == 0 && cue_golf_last(CUE_GOLF_MFRONT9) == 8,
       "matchplay front nine is holes 1-9");
    ok(cue_golf_first(CUE_GOLF_MBACK9)  == 9 && cue_golf_last(CUE_GOLF_MBACK9) == 17,
       "matchplay back nine is holes 10-18");
    ok(CUE_GOLF_IS_MATCH(CUE_GOLF_M18) && !CUE_GOLF_IS_MATCH(CUE_GOLF_18),
       "and the two are told apart");

    CueTable t; cue_table_init(&t, CUE_GAME_GOLF);
    CueRules r; cue_rules_init(&r, &t, 0);
    cue_rules_set_golf_round(&r, CUE_GOLF_MFRONT9);

    /* seat 0 wins holes 1 and 2, seat 1 wins hole 3, hole 4 is halved */
    r.golf_card[0][0] = 3; r.golf_card[1][0] = 5;
    r.golf_card[0][1] = 4; r.golf_card[1][1] = 6;
    r.golf_card[0][2] = 7; r.golf_card[1][2] = 4;
    r.golf_card[0][3] = 4; r.golf_card[1][3] = 4;
    ok(cue_rules_golf_holes_up(&r) == 1, "two holes to one, one half: one up");

    /* THE DISASTER THAT COSTS ONE HOLE. Seat 0 takes fourteen on the fifth —
     * enough to lose a strokeplay round outright — and is still one up. */
    r.golf_card[0][4] = 14; r.golf_card[1][4] = 4;
    ok(cue_rules_golf_holes_up(&r) == 0,
       "a fourteen loses the hole and only the hole: all square");
    {   const int strokes0 = cue_rules_golf_total(&r, 0, 0, 4);
        const int strokes1 = cue_rules_golf_total(&r, 1, 0, 4);
        char b[120];
        snprintf(b, sizeof b, "...though on strokes it would be %d against %d",
                 strokes0, strokes1);
        ok(strokes0 > strokes1 + 8, b);
    }

    /* a hole only one of them has finished belongs to neither yet */
    r.golf_card[0][5] = 3; r.golf_card[1][5] = 0;
    ok(cue_rules_golf_holes_up(&r) == 0, "a hole in progress counts for nobody");

    printf(fails ? "\nFAILED\n" : "\nall good\n");
    return fails ? 1 : 0;
}
