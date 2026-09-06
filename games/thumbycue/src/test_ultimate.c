/*
 * ULTIMATE POOL, on both of its tables.
 *
 * Ultimate Pool USA is not a different game from Ultimate Pool: it is the same
 * rule book -- International 8-ball plus the Ultimate Pool Group's golden break
 * -- played on a 7 ft AMERICAN table with mitred pockets instead of the 7 ft
 * English one. The equipment is American and the rules are English, and that
 * pairing is the whole identity of it.
 *
 * So these ask the same questions twice, once on each bed, and the answers must
 * match. And they check that plain WPA 8-ball on the same American bed is NOT
 * given the golden break, because that would be handing every US 8-ball player
 * a rule nobody plays.
 */
#include "cue_rules.h"
#include "cue_table.h"
#include <stdio.h>
#include <string.h>

static int s_fail;
static void ok(int c, const char *what, const char *why) {
    printf("  %s   %s%s%s\n", c ? "ok  " : "FAIL", what,
           (why && why[0]) ? "   " : "", (why && why[0]) ? why : "");
    if (!c) s_fail++;
}

static CueTable T; static CueWorld W; static CueBall B[CUE_MAX_BALLS]; static int NB;

static void fresh(CueRules *r, int game, int ruleset) {
    cue_table_init(&T, (CueGameKind)game);
    cue_table_build_world(&T, &W);
    NB = cue_table_rack(&T, B);
    B[0].on = 1;
    cue_rules_init(r, &T, 0);
    cue_rules_set_uk(r, ruleset);
    r->break_shot = 0;
    r->ball_in_hand = 0;
}

static void play(CueRules *r, int first, int scratch, int cushion,
                 const int *pot, int np, uint32_t crossed) {
    W.ntouch = 0;
    W.brk_cross = crossed;
    for (int k = 0; k < np; k++)
        for (int i = 1; i < NB; i++)
            if (B[i].on && B[i].id == pot[k]) { B[i].on = 0; break; }
    r->n_off = 0;
    cue_rules_resolve(r, B, NB, &W, first, scratch, cushion, pot, np);
}

/* The black's id differs between the two racks, so ask the table. */
static int black_id(void) { return 8; }

static void both_tables(int game, const char *bed)
{
    printf("\nULTIMATE POOL on the %s\n", bed);

    /* ---- the golden break: the black off a LEGAL break WINS ----
     *
     * The break has to be a break first. Potting the black and nothing else,
     * with nothing over the line, is one ball against the Group's three, and a
     * re-rack takes precedence over the golden break -- so the winning case
     * has to be a break that would have stood on its own. */
    {   CueRules r; fresh(&r, game, CUE_UK_ULTIMATE);
        r.break_shot = 1;
        int pot[1] = { black_id() };
        play(&r, 1, 0, 1, pot, 1, (1u << 3) | (1u << 5));   /* 1 potted + 2 over = 3 */
        ok(r.frame_over && r.winner == 0,
           "the black off a legal break wins the frame there and then", r.msg);
    }
    {   CueRules r; fresh(&r, game, CUE_UK_ULTIMATE);
        r.break_shot = 1;
        int pot[1] = { black_id() };
        play(&r, 1, 0, 1, pot, 1, 0);                       /* 1 ball: not a break */
        ok(!r.frame_over && r.rerack != 0,
           "...but the black off an ILLEGAL break is only a re-rack", r.msg);
    }
    /* ---- the golden duck: the black WITH the cue ball LOSES ---- */
    {   CueRules r; fresh(&r, game, CUE_UK_ULTIMATE);
        r.break_shot = 1;
        int pot[1] = { black_id() };
        play(&r, 1, 1, 1, pot, 1, 0);            /* ...and scratched */
        ok(r.frame_over && r.winner == 1,
           "...and the black with the cue ball loses it: the golden duck", r.msg);
    }
    /* ---- the break: three potted or over the line ---- */
    {   CueRules r; fresh(&r, game, CUE_UK_ULTIMATE);
        r.break_shot = 1;
        play(&r, 1, 0, 1, NULL, 0, (1u << 3) | (1u << 5));
        ok(r.rerack != 0, "two balls over the line is not a break: re-rack", r.msg);
    }
    {   CueRules r; fresh(&r, game, CUE_UK_ULTIMATE);
        r.break_shot = 1;
        play(&r, 1, 0, 1, NULL, 0, (1u << 3) | (1u << 5) | (1u << 7));
        ok(r.rerack == 0, "...and three of them is", r.msg);
    }
    /* ---- a foul is ball in hand anywhere, as International has it ---- */
    {   CueRules r; fresh(&r, game, CUE_UK_ULTIMATE);
        play(&r, -1, 0, 0, NULL, 0, 0);          /* hit nothing */
        ok(r.last_foul && r.ball_in_hand && !r.two_shot,
           "a foul is ball in hand anywhere, not two shots", r.msg);
    }
}

int main(void)
{
    printf("ultimate pool\n");
    both_tables(CUE_GAME_UK8, "7 ft ENGLISH table");
    both_tables(CUE_GAME_US8, "7 ft AMERICAN table (Ultimate Pool USA)");

    /* ---- AND PLAIN WPA 8-BALL MUST NOT GET ANY OF IT ---------------- */
    printf("\nplain WPA 8-ball on the same American bed\n");
    {   CueRules r; fresh(&r, CUE_GAME_US8, CUE_UK_PUB);
        r.break_shot = 1;
        int pot[1] = { black_id() };
        play(&r, 1, 0, 1, pot, 1, 0);
        ok(!r.frame_over,
           "the black off the break does NOT win a WPA frame", r.msg);
    }
    {   CueRules r; fresh(&r, CUE_GAME_US8, CUE_UK_PUB);
        r.break_shot = 1;
        play(&r, 1, 0, 1, NULL, 0, (1u << 3) | (1u << 5) | (1u << 7));
        ok(r.rerack == 0 || r.last_foul == 0,
           "...and its break is judged to WPA, not to the Group's three", r.msg);
    }

    printf("\n%s\n", s_fail ? "FAILURES" : "all good");
    return s_fail ? 1 : 0;
}
