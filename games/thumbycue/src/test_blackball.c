/* BLACKBALL, scored against the WPA Blackball Rules 2005 (the 12-page book).
 * Every case names its rule. The pub and international readings share this
 * resolver, so the last cases prove the switch leaves them alone.
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

static void fresh(CueRules *r, int ruleset) {
    cue_table_init(&T, CUE_GAME_UK8);
    cue_table_build_world(&T, &W);
    NB = cue_table_rack(&T, B);
    B[0].on = 1;
    cue_rules_init(r, &T, 0);
    cue_rules_set_uk(r, ruleset);
    r->break_shot = 0;
    r->ball_in_hand = 0;
}

/* one stroke, the facts handed straight to the referee */
static void play(CueRules *r, int first, int scratch, int cushion,
                 const int *pot, int np, uint32_t crossed_mask) {
    W.ntouch = 0;
    W.brk_cross = crossed_mask;
    for (int k = 0; k < np; k++)
        for (int i = 1; i < NB; i++)
            if (B[i].on && B[i].id == pot[k]) { B[i].on = 0; break; }
    r->n_off = 0;
    cue_rules_resolve(r, B, NB, &W, first, scratch, cushion, pot, np);
}

static void groups(CueRules *r, int mine) {
    r->open = 0; r->group[r->turn] = mine; r->group[1 - r->turn] = 3 - mine;
}

int main(void) {
    printf("blackball, against the 2005 book\n");

    /* ---- 6a/6b: any foul is a free shot plus one visit ---- */
    {   CueRules r; fresh(&r, CUE_UK_BLACKBALL);
        groups(&r, 1);
        play(&r, 9, 0, 1, NULL, 0, 0);           /* wrong ball: foul */
        ok(r.last_foul && r.turn == 1 && r.free_shot && r.shots_remaining == 2,
           "6a: a foul hands over a free shot plus one visit", r.msg);
        ok(!r.ball_in_hand, "6c: the cue ball is played from where it lies", "");
        /* the free shot: opponent's ball first, potted — legal, and play on */
        int pot[1] = { 3 };                      /* group 1 = the other side's */
        play(&r, 3, 0, 1, pot, 1, 0);
        ok(!r.last_foul && r.turn == 1,
           "6b: the free shot may strike and pot ANY ball, and play goes on", r.msg);
    }

    /* ---- 4f: pots on the free shot never decide groups ---- */
    {   CueRules r; fresh(&r, CUE_UK_BLACKBALL);      /* open table */
        play(&r, -1, 0, 1, NULL, 0, 0);          /* an air shot: foul, free shot */
        int pot[1] = { 5 };
        play(&r, 5, 0, 1, pot, 1, 0);            /* free shot pots a solid */
        ok(r.open, "4f: a ball potted on the free shot leaves the table open", "");
    }

    /* ---- 4b: the break must pot or send two over the centre line ---- */
    {   CueRules r; fresh(&r, CUE_UK_BLACKBALL);
        r.break_shot = 1;
        play(&r, 1, 0, 1, NULL, 0, 0);           /* contact, nothing crossed */
        ok(r.last_foul, "4b: a dry break with nothing over the line is a foul", r.msg);
    }
    {   CueRules r; fresh(&r, CUE_UK_BLACKBALL);
        r.break_shot = 1;
        play(&r, 1, 0, 1, NULL, 0, (1u << 3) | (1u << 7));
        ok(!r.last_foul, "...two balls fully over it make it legal", r.msg);
        ok(r.open, "4e: and groups are never decided on the break", "");
    }
    {   CueRules r; fresh(&r, CUE_UK_BLACKBALL);
        r.break_shot = 1;
        int pot[1] = { 4 };
        play(&r, 1, 0, 1, pot, 1, 0);
        ok(!r.last_foul && r.open, "...as does a ball potted", r.msg);
    }

    /* ---- 4d: the black off the break is a re-rack, same breaker ---- */
    {   CueRules r; fresh(&r, CUE_UK_BLACKBALL);
        r.break_shot = 1;
        int pot[2] = { 8, 2 };
        play(&r, 1, 1, 1, pot, 2, 0);            /* 8 down, a ball down, AND a scratch */
        ok(!r.last_foul && r.rerack == 2 && r.turn == 0 && r.break_shot,
           "4d: black off the break — re-rack, same breaker, no penalty at all", r.msg);
    }

    /* ---- 5e: a jump shot is a foul ---- */
    {   CueRules r; fresh(&r, CUE_UK_BLACKBALL);
        groups(&r, 1);
        r.jumped = 1;
        play(&r, 1, 0, 1, NULL, 0, 0);
        ok(r.last_foul, "5e: a jump shot is a foul here", r.msg);
        r.jumped = 0;
    }

    /* ---- 5d/5g: pot or cushion, unless snookered ---- */
    {   CueRules r; fresh(&r, CUE_UK_BLACKBALL);
        groups(&r, 1);
        play(&r, 1, 0, 0, NULL, 0, 0);           /* legal contact, nothing else */
        ok(r.last_foul, "5d: no pot and no cushion is a foul", r.msg);
    }
    {   CueRules r; fresh(&r, CUE_UK_BLACKBALL);
        groups(&r, 1);
        r.was_snookered = 1;
        play(&r, 1, 0, 0, NULL, 0, 0);
        ok(!r.last_foul, "5g: ...but contact alone is enough out of a snooker", r.msg);
        r.was_snookered = 0;
    }

    /* ---- 5a + 4h: an in-off is played from BAULK, the area, not the D ---- */
    {   CueRules r; fresh(&r, CUE_UK_BLACKBALL);
        groups(&r, 1);
        play(&r, 1, 1, 1, NULL, 0, 0);
        ok(r.last_foul && r.ball_in_hand, "5a: an in-off is a foul, in hand", r.msg);
        ok(cue_rules_in_hand_anywhere(&r) == 2,
           "4c/4h: and in hand means the baulk AREA (region 2)", "");
    }

    /* ---- 7b: the black with your own balls still up loses the frame ---- */
    {   CueRules r; fresh(&r, CUE_UK_BLACKBALL);
        groups(&r, 1);
        int pot[1] = { 8 };
        play(&r, 8, 0, 1, pot, 1, 0);
        ok(r.frame_over && r.winner == 1,
           "7b: potting the black with your group still up is loss of frame", r.msg);
    }

    /* ---- the other readings are untouched ---- */
    {   CueRules r; fresh(&r, CUE_UK_INTL);
        groups(&r, 1);
        play(&r, 9, 0, 1, NULL, 0, 0);
        ok(r.last_foul && r.ball_in_hand && cue_rules_in_hand_anywhere(&r) == 1,
           "international still gives ball in hand anywhere", r.msg);
    }
    {   CueRules r; fresh(&r, CUE_UK_PUB);
        groups(&r, 1);
        r.break_shot = 1;
        play(&r, 1, 0, 1, NULL, 0, 0);
        ok(!r.last_foul, "the pub break never needed the crossing rule", r.msg);
    }

    printf(s_fail ? "\n%d FAILED\n" : "\nall good\n", s_fail);
    return s_fail != 0;
}
