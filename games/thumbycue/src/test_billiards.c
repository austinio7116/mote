/* ENGLISH BILLIARDS, scored against the book.
 *
 * Every case here is a numbered rule from the WPBSA "Rules of the Game of
 * English Billiards", and the rule is quoted beside it. The scoring is the
 * whole game — three balls and six ways to score, but they combine, and the
 * combinations are where a hand-written scorer goes wrong:
 *
 *   Rule 4(a)  a cannon, a pot white and an in-off white each score two
 *   Rule 4(b)  a pot red and an in-off red each score three
 *   Rule 4(c)  all of them count in the same stroke
 *   Rule 4(d)  an in-off with a cannon is priced by the ball struck FIRST
 *
 * ...which is the one nobody expects: the same in-off is worth three or two
 * depending on which object ball the cue ball reached first, not on which one
 * it went in off.
 */
#include "cue_rules.h"
#include "cue_table.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int s_fail;
static void ok(int cond, const char *what, const char *detail) {
    printf("  %-4s %s%s%s\n", cond ? "ok" : "FAIL", what,
           detail && detail[0] ? "   " : "", detail ? detail : "");
    if (!cond) s_fail++;
}

/* A stroke, told to the rules the way the game tells them: what the cue ball
 * touched in order, whether it went in, and what went down. */
typedef struct {
    int touch[4];   /* ball ids the cue ball contacted, in order, 0-terminated */
    int scratch;    /* the cue ball was pocketed */
    int pot[4];     /* ids pocketed, 0-terminated (CUE_ID_CUE never appears) */
    int off;        /* balls forced off the table */
    int cushion_first;  /* the cue ball reached a cushion before any ball */
} Shot;

static CueTable T;
static CueWorld W;
static CueBall  B[CUE_MAX_BALLS];

/* The object white's REAL id is zero — CUE_ID_BIL_WHITE == CUE_ID_CUE — which
 * a 0-terminated array cannot carry. That gap is exactly where the yellow
 * player's fouls hid: no case here could even say "the striker hit the white".
 * TW is the test's stand-in; play() translates it back to the real id. */
#define TW 99
static int untw(int id) { return id == TW ? CUE_ID_BIL_WHITE : id; }
static void play(CueRules *r, const Shot *s) {
    memset(&W, 0, sizeof W);
    W.ntouch = 0;
    /* A CUSHION BEFORE THE BALLS, which Rule 6 turns on: the difference between
     * playing straight onto a ball in baulk and coming back off a cushion is
     * the whole of 6(f), and the touch list is where it shows. */
    if (s->cushion_first) {
        W.touch[W.ntouch].what = CUE_TOUCH_CUSHION;
        W.touch[W.ntouch].id   = 0;
        W.ntouch++;
    }
    for (int i = 0; i < 4 && s->touch[i]; i++) {
        W.touch[W.ntouch].what = CUE_TOUCH_BALL;
        W.touch[W.ntouch].id   = (unsigned char)untw(s->touch[i]);
        W.ntouch++;
    }
    int potted[8], np = 0;
    for (int i = 0; i < 4 && s->pot[i]; i++) potted[np++] = untw(s->pot[i]);
    int first = s->touch[0] ? untw(s->touch[0]) : -1;
    r->n_off = s->off;
    cue_rules_resolve(r, B, 3, &W, first, s->scratch, 1, potted, np);
}

static void fresh(CueRules *r) {
    cue_table_init(&T, CUE_GAME_BILLIARDS);
    cue_table_build_world(&T, &W);
    cue_table_rack(&T, B);
    cue_rules_init(r, &T, 0);
    r->target_score = 1000;      /* out of the way: this is about scoring */
}

int main(void) {
    printf("English billiards\n");

    /* ---- the table (Section 1 Rule 1) ---- */
    {   cue_table_init(&T, CUE_GAME_BILLIARDS);
        char d[96];
        snprintf(d, sizeof d, "%.0f x %.0f mm", T.half_len * 2000, T.half_wid * 2000);
        ok(fabsf(T.half_len * 2000.0f - 3569.0f) < 14.0f &&
           fabsf(T.half_wid * 2000.0f - 1778.0f) < 14.0f,
           "the standard table, 3569 x 1778 mm", d);
        snprintf(d, sizeof d, "baulk %.0f mm from the bottom cushion, D radius %.0f mm",
                 (T.baulk_x + T.half_len) * 1000.0f, T.d_radius * 1000.0f);
        ok(fabsf((T.baulk_x + T.half_len) * 1000.0f - 737.0f) < 6.0f &&
           fabsf(T.d_radius * 1000.0f - 292.0f) < 6.0f,
           "...the baulk-line at 737 mm and a D of 292 mm", d);
        snprintf(d, sizeof d, "Spot %.0f mm below the top cushion",
                 (T.half_len - T.black_x) * 1000.0f);
        ok(fabsf((T.half_len - T.black_x) * 1000.0f - 324.0f) < 6.0f,
           "...and the Spot at 324 mm", d);
        ok(fabsf(T.blue_x) < 1e-4f, "the Centre Spot is the middle of the table", NULL);
        ok(fabsf(T.pink_x - T.half_len * 0.5f) < 1e-4f,
           "the Pyramid Spot is midway between the Centre Spot and the top cushion", NULL);
        ok(T.nballs == 3, "three balls", NULL);
    }

    /* ---- the rack (Section 3 Rule 2(b)) ---- */
    {   cue_table_init(&T, CUE_GAME_BILLIARDS);
        cue_table_rack(&T, B);
        ok(B[1].id == CUE_ID_BIL_RED && B[1].on &&
           fabsf(B[1].pos.x - T.black_x) < 1e-3f,
           "the red starts on the Spot", NULL);
        ok(B[0].id == CUE_ID_BIL_WHITE && B[0].on,
           "the striker's white is the cue ball", NULL);
        ok(!B[2].on, "the other side's ball is off the table until its turn", NULL);
    }

    /* ---- Rule 4(a): a cannon scores two ---- */
    {   CueRules r; fresh(&r);
        Shot s = { { CUE_ID_BIL_RED, CUE_ID_BIL_YELLOW }, 0, { 0 }, 0 };
        play(&r, &s);
        char d[64]; snprintf(d, sizeof d, "scored %d", r.score[0]);
        ok(r.score[0] == 2, "a cannon scores two", d);
        ok(r.turn == 0, "...and the striker plays on", NULL);
    }

    /* ---- Rule 4(b): a pot red scores three ---- */
    {   CueRules r; fresh(&r);
        Shot s = { { CUE_ID_BIL_RED }, 0, { CUE_ID_BIL_RED }, 0 };
        play(&r, &s);
        char d[64]; snprintf(d, sizeof d, "scored %d", r.score[0]);
        ok(r.score[0] == 3, "a pot red scores three", d);
        ok(r.bil_respot_red == CUE_BIL_SPOT_SPOT, "...and the red goes back on the Spot", NULL);
    }

    /* ---- Rule 4(a): a pot white scores two ---- */
    {   CueRules r; fresh(&r);
        Shot s = { { CUE_ID_BIL_YELLOW }, 0, { CUE_ID_BIL_YELLOW }, 0 };
        play(&r, &s);
        char d[64]; snprintf(d, sizeof d, "scored %d", r.score[0]);
        ok(r.score[0] == 2, "a pot white scores two", d);
    }

    /* ---- Rule 4(b): an in-off red scores three ---- */
    {   CueRules r; fresh(&r);
        Shot s = { { CUE_ID_BIL_RED }, 1, { 0 }, 0 };
        play(&r, &s);
        char d[64]; snprintf(d, sizeof d, "scored %d", r.score[0]);
        ok(r.score[0] == 3, "an in-off red scores three", d);
        ok(r.ball_in_hand, "...and the striker is in hand (Rule 3)", NULL);
        ok(r.turn == 0, "...and still at the table", NULL);
    }

    /* ---- Rule 4(a): an in-off white scores two ---- */
    {   CueRules r; fresh(&r);
        Shot s = { { CUE_ID_BIL_YELLOW }, 1, { 0 }, 0 };
        play(&r, &s);
        char d[64]; snprintf(d, sizeof d, "scored %d", r.score[0]);
        ok(r.score[0] == 2, "an in-off white scores two", d);
    }

    /* ---- Rule 4(c): everything in the stroke counts ---- */
    {   CueRules r; fresh(&r);
        /* cannon (2) + pot red (3) + pot white (2) = 7 */
        Shot s = { { CUE_ID_BIL_RED, CUE_ID_BIL_YELLOW }, 0,
                   { CUE_ID_BIL_RED, CUE_ID_BIL_YELLOW }, 0 };
        play(&r, &s);
        char d[64]; snprintf(d, sizeof d, "scored %d, wanted 7", r.score[0]);
        ok(r.score[0] == 7, "cannon and both pots score all three", d);
    }

    /* ---- Rule 4(d)(i): in-off with a cannon, red struck first, is three ---- */
    {   CueRules r; fresh(&r);
        Shot s = { { CUE_ID_BIL_RED, CUE_ID_BIL_YELLOW }, 1, { 0 }, 0 };
        play(&r, &s);
        char d[80]; snprintf(d, sizeof d, "scored %d, wanted 5 (cannon 2 + in-off 3)", r.score[0]);
        ok(r.score[0] == 5, "in-off after a cannon, red first, is priced as red", d);
    }

    /* ---- Rule 4(d)(ii): ...white struck first, is two ---- */
    {   CueRules r; fresh(&r);
        Shot s = { { CUE_ID_BIL_YELLOW, CUE_ID_BIL_RED }, 1, { 0 }, 0 };
        play(&r, &s);
        char d[80]; snprintf(d, sizeof d, "scored %d, wanted 4 (cannon 2 + in-off 2)", r.score[0]);
        ok(r.score[0] == 4, "...and white first is priced as white", d);
    }

    /* ---- a legal stroke that scores nothing ends the turn ---- */
    {   CueRules r; fresh(&r);
        Shot s = { { CUE_ID_BIL_RED }, 0, { 0 }, 0 };
        play(&r, &s);
        ok(r.score[0] == 0 && r.turn == 1, "a stroke that scores nothing ends the turn", NULL);
        ok(r.bil_yellow, "...and the other side is on the yellow", NULL);
    }

    /* ---- Rule 16: a miss is two to the opponent ---- */
    {   CueRules r; fresh(&r);
        Shot s = { { 0 }, 0, { 0 }, 0 };
        play(&r, &s);
        char d[64]; snprintf(d, sizeof d, "%d - %d", r.score[0], r.score[1]);
        ok(r.last_foul && r.score[1] == 2, "hitting nothing is a foul worth two", d);
        ok(r.turn == 1, "...and the turn passes", NULL);
    }

    /* ---- Section 2 Definition 17: a coup is a foul, not an in-off ---- */
    {   CueRules r; fresh(&r);
        Shot s = { { 0 }, 1, { 0 }, 0 };
        play(&r, &s);
        char d[64]; snprintf(d, sizeof d, "%d - %d", r.score[0], r.score[1]);
        ok(r.last_foul && r.score[0] == 0 && r.score[1] == 2,
           "running a coup scores nothing and gives two away", d);
    }

    /* ---- Rule 15(c): never more than two in one stroke ---- */
    {   CueRules r; fresh(&r);
        Shot s = { { 0 }, 1, { 0 }, 2 };     /* a miss, a coup AND two off the table */
        play(&r, &s);
        char d[64]; snprintf(d, sizeof d, "opponent got %d", r.score[1]);
        ok(r.score[1] == 2, "several fouls in one stroke are still only two", d);
    }

    /* ---- Rule 15(b): the break's points stand, the foul stroke's do not ---- */
    {   CueRules r; fresh(&r);
        Shot good = { { CUE_ID_BIL_RED }, 0, { CUE_ID_BIL_RED }, 0 };
        play(&r, &good);
        Shot bad  = { { 0 }, 0, { 0 }, 0 };
        play(&r, &bad);
        char d[64]; snprintf(d, sizeof d, "%d - %d", r.score[0], r.score[1]);
        ok(r.score[0] == 3 && r.score[1] == 2,
           "points made before a foul stand; the foul stroke scores none", d);
    }

    /* ---- Rule 8(b),(c): the Spot twice, then the Centre Spot ---- */
    {   CueRules r; fresh(&r);
        Shot s = { { CUE_ID_BIL_RED }, 0, { CUE_ID_BIL_RED }, 0 };
        play(&r, &s);
        int a = r.bil_respot_red;
        play(&r, &s);
        int b2 = r.bil_respot_red;
        play(&r, &s);
        int c = r.bil_respot_red;
        play(&r, &s);
        int d4 = r.bil_respot_red;
        char d[96];
        snprintf(d, sizeof d, "%d %d %d %d (1 = Spot, 2 = Centre)", a, b2, c, d4);
        ok(a == CUE_BIL_SPOT_SPOT && b2 == CUE_BIL_SPOT_SPOT &&
           c == CUE_BIL_SPOT_CENTRE && d4 == CUE_BIL_SPOT_SPOT,
           "continued pots of the red: Spot, Spot, Centre, then round again", d);
    }

    /* ...and a stroke that scores otherwise breaks that sequence. */
    {   CueRules r; fresh(&r);
        Shot pot = { { CUE_ID_BIL_RED }, 0, { CUE_ID_BIL_RED }, 0 };
        Shot both = { { CUE_ID_BIL_RED, CUE_ID_BIL_YELLOW }, 0, { CUE_ID_BIL_RED }, 0 };
        play(&r, &pot);
        play(&r, &pot);
        play(&r, &both);           /* a cannon with it: not a continued pot */
        play(&r, &pot);
        char d[64]; snprintf(d, sizeof d, "%d (1 = Spot)", r.bil_respot_red);
        ok(r.bil_respot_red == CUE_BIL_SPOT_SPOT,
           "a pot in conjunction with another score restarts the sequence", d);
    }

    /* ---- Rule 9: seventy-five consecutive cannons, then a foul ---- */
    {   CueRules r; fresh(&r);
        Shot c = { { CUE_ID_BIL_RED, CUE_ID_BIL_YELLOW }, 0, { 0 }, 0 };
        for (int i = 0; i < CUE_BIL_MAX_CANNONS; i++) play(&r, &c);
        char d[96];
        snprintf(d, sizeof d, "%d cannons, score %d", r.bil_cannons, r.score[0]);
        ok(!r.last_foul && r.bil_cannons == CUE_BIL_MAX_CANNONS &&
           r.score[0] == CUE_BIL_MAX_CANNONS * 2,
           "seventy-five consecutive cannons are legal", d);
        play(&r, &c);
        snprintf(d, sizeof d, "%s", r.msg);
        ok(r.last_foul, "...and the seventy-sixth is a foul", d);
    }

    /* ...and a hazard in among them resets the count. */
    {   CueRules r; fresh(&r);
        Shot c = { { CUE_ID_BIL_RED, CUE_ID_BIL_YELLOW }, 0, { 0 }, 0 };
        Shot h = { { CUE_ID_BIL_RED, CUE_ID_BIL_YELLOW }, 0, { CUE_ID_BIL_RED }, 0 };
        for (int i = 0; i < 40; i++) play(&r, &c);
        play(&r, &h);              /* a cannon in conjunction with a hazard */
        char d[64]; snprintf(d, sizeof d, "%d", r.bil_cannons);
        ok(r.bil_cannons == 0, "a cannon with a hazard does not count toward the limit", d);
    }

    /* ---- Rule 10: fifteen consecutive hazards, then a foul ---- */
    {   CueRules r; fresh(&r);
        Shot h = { { CUE_ID_BIL_RED }, 0, { CUE_ID_BIL_RED }, 0 };
        for (int i = 0; i < CUE_BIL_MAX_HAZARDS; i++) play(&r, &h);
        char d[64]; snprintf(d, sizeof d, "%d hazards", r.bil_hazards);
        ok(!r.last_foul && r.bil_hazards == CUE_BIL_MAX_HAZARDS,
           "fifteen consecutive hazards are legal", d);
        play(&r, &h);
        ok(r.last_foul, "...and the sixteenth is a foul", r.msg);
    }

    /* ---- Rule 5(d): the game ends when a player reaches the number ---- */
    {   CueRules r; fresh(&r);
        r.target_score = 10;
        Shot pot = { { CUE_ID_BIL_RED }, 0, { CUE_ID_BIL_RED }, 0 };
        for (int i = 0; i < 4 && !r.frame_over; i++) play(&r, &pot);
        char d[64]; snprintf(d, sizeof d, "%d of %d", r.score[0], r.target_score);
        ok(r.frame_over && r.winner == 0, "the game ends at the agreed number", d);
    }

    /* ---- Rule 8(a): the Spot, and the walk when it is occupied ---- */
    {   CueRules r; fresh(&r);
        /* Take the red off and ask for the Spot with nothing in the way. */
        B[1].on = 0;
        r.bil_respot_red = CUE_BIL_SPOT_SPOT;
        ok(cue_rules_billiards_respot(&r, &T, B, 3), "the red is put back", NULL);
        char d[96];
        snprintf(d, sizeof d, "(%.3f,%.3f) vs the Spot (%.3f,%.3f)",
                 (double)B[1].pos.x, (double)B[1].pos.z,
                 (double)r.spot[CUE_BIL_SPOT_SPOT].x, (double)r.spot[CUE_BIL_SPOT_SPOT].z);
        ok(B[1].on && fabsf(B[1].pos.x - r.spot[CUE_BIL_SPOT_SPOT].x) < 1e-4f,
           "...on the Spot when the Spot is free", d);

        /* Now stand a ball ON the Spot and ask again: Rule 8(a)(i) sends it to
         * the Pyramid Spot. */
        B[1].on = 0;
        B[2].on = 1; B[2].pos = r.spot[CUE_BIL_SPOT_SPOT];
        r.bil_respot_red = CUE_BIL_SPOT_SPOT;
        cue_rules_billiards_respot(&r, &T, B, 3);
        snprintf(d, sizeof d, "(%.3f) vs the Pyramid Spot (%.3f)",
                 (double)B[1].pos.x, (double)r.spot[CUE_BIL_SPOT_PYRAMID].x);
        ok(fabsf(B[1].pos.x - r.spot[CUE_BIL_SPOT_PYRAMID].x) < 1e-4f,
           "an occupied Spot sends the red to the Pyramid Spot", d);

        /* Both occupied: Rule 8(a)(ii) sends it to the Centre Spot. */
        B[1].on = 0;
        B[2].pos = r.spot[CUE_BIL_SPOT_SPOT];
        B[0].on = 1; B[0].pos = r.spot[CUE_BIL_SPOT_PYRAMID];
        r.bil_respot_red = CUE_BIL_SPOT_SPOT;
        cue_rules_billiards_respot(&r, &T, B, 3);
        snprintf(d, sizeof d, "(%.3f) vs the Centre Spot (%.3f)",
                 (double)B[1].pos.x, (double)r.spot[CUE_BIL_SPOT_CENTRE].x);
        ok(fabsf(B[1].pos.x - r.spot[CUE_BIL_SPOT_CENTRE].x) < 1e-4f,
           "...and both occupied sends it to the Centre Spot", d);
    }

    /* ---- Rule 8(b)(i): from the Centre Spot the fallback is the Pyramid ---- */
    {   CueRules r; fresh(&r);
        B[1].on = 0;
        B[2].on = 1; B[2].pos = r.spot[CUE_BIL_SPOT_CENTRE];
        r.bil_respot_red = CUE_BIL_SPOT_CENTRE;
        cue_rules_billiards_respot(&r, &T, B, 3);
        char d[96];
        snprintf(d, sizeof d, "(%.3f) vs the Pyramid Spot (%.3f)",
                 (double)B[1].pos.x, (double)r.spot[CUE_BIL_SPOT_PYRAMID].x);
        ok(fabsf(B[1].pos.x - r.spot[CUE_BIL_SPOT_PYRAMID].x) < 1e-4f,
           "an occupied Centre Spot sends the red to the Pyramid Spot", d);
    }

    /* ---- the two whites exchange at a change of turn ---- */
    {   CueRules r; fresh(&r);
        ok(B[0].id == CUE_ID_BIL_WHITE, "index 0 starts as the white", NULL);
        cue_rules_billiards_swap(B, 3);
        ok(B[0].id == CUE_ID_BIL_YELLOW && B[2].id == CUE_ID_BIL_WHITE,
           "after the exchange index 0 is the yellow", NULL);
        cue_rules_billiards_swap(B, 3);
        ok(B[0].id == CUE_ID_BIL_WHITE && B[2].id == CUE_ID_BIL_YELLOW,
           "...and back again", NULL);
    }

    /* ---- and the whole thing plays a break end to end ---- */
    {   CueRules r; fresh(&r);
        Shot pot = { { CUE_ID_BIL_RED }, 0, { CUE_ID_BIL_RED }, 0 };
        int placed = 0;
        for (int i = 0; i < 6; i++) {
            play(&r, &pot);
            B[1].on = 0;                       /* the host takes the potted red off */
            placed += cue_rules_billiards_respot(&r, &T, B, 3);
        }
        char d[64]; snprintf(d, sizeof d, "%d respots, break %d", placed, r.score[0]);
        ok(placed == 6 && r.score[0] == 18,
           "six pots of the red: six respots and eighteen points", d);
    }

    /* ---- THE YELLOW SIDE (Section 3 Rule 4, same rules, other ball) ------- *
     * The striker's ball is the yellow and the OBJECT white wears id zero.
     * Every case below was impossible to express before TW existed — and the
     * first one was a foul on the device for as long as that was true. */
    {   CueRules r; fresh(&r);
        r.bil_yellow = 1;
        cue_rules_billiards_swap(B, 3);
        int sc0 = r.score[0], sc1 = r.score[1];
        Shot s = { .touch = { TW } };            /* a clean stroke onto the white */
        play(&r, &s);
        ok(!r.last_foul, "yellow onto the object white: no foul", r.msg);
        ok(r.score[0] == sc0 && r.score[1] == sc1,
           "...and no score: the turn simply passes", "");
    }
    {   CueRules r; fresh(&r);
        r.bil_yellow = 1;
        cue_rules_billiards_swap(B, 3);
        Shot s = { .touch = { TW, CUE_ID_BIL_RED }, .scratch = 1 };
        play(&r, &s);
        ok(!r.last_foul, "yellow: white first, red, in-off", r.msg);
        ok(r.brk == 2 + 2,
           "...scores the cannon AND the in-off priced by the WHITE (Rule 4(d))",
           r.msg);
    }
    {   CueRules r; fresh(&r);
        r.bil_yellow = 1;
        ok(cue_rules_ball_legal(&r, B, 3, CUE_ID_BIL_WHITE),
           "yellow's object white is a legal ball", "");
        ok(!cue_rules_ball_legal(&r, B, 3, CUE_ID_BIL_YELLOW),
           "...and his own yellow is not", "");
        r.bil_yellow = 0;
        ok(cue_rules_ball_legal(&r, B, 3, CUE_ID_BIL_YELLOW),
           "white's object yellow is a legal ball", "");
        ok(!cue_rules_ball_legal(&r, B, 3, CUE_ID_BIL_WHITE),
           "...and the white in his hand is not", "");
    }

    /* ---- BAULK, AND THE DOUBLE BAULK ------------------------------------
     *
     * Section 3 Rule 1(e) names it as a tactic: "to leave both object balls in
     * Baulk when the next player is in-hand such that any attempt at disturbing
     * the balls must be by means of an indirect stroke." Rule 6 is what makes
     * it one, and Rule 16 is what it costs to fail.
     *
     * `bil_from_hand` is what the host sets when it places the ball, and the
     * two baulk flags are what cue_rules_attempt_begin reads off the table
     * before the stroke — so a case here sets the same three things the game
     * does and asks the resolver the same question. */
    {   CueRules r; fresh(&r);
        /* Both object balls behind the line, striker in hand: a double baulk. */
        r.bil_from_hand = 1; r.bil_red_baulk = 1; r.bil_wht_baulk = 1;
        Shot s = { .touch = { CUE_ID_BIL_RED } };   /* straight at it */
        play(&r, &s);
        ok(r.last_foul, "6(f): from hand, straight onto a red in baulk = foul",
           r.msg);
        ok(!strncmp(r.msg, "FOUL", 4), "...and the board calls it a foul", r.msg);
        ok(r.score[1] == 2, "...two to the opponent (Rule 15(c))", r.msg);
    }
    {   CueRules r; fresh(&r);
        r.bil_from_hand = 1; r.bil_red_baulk = 1; r.bil_wht_baulk = 1;
        /* Up the table, off a cushion, and back onto it: the legal escape. */
        Shot s = { .touch = { CUE_ID_BIL_RED }, .cushion_first = 1 };
        play(&r, &s);
        ok(!r.last_foul, "6(d)/(e): a cushion first and the same red is legal",
           r.msg);
    }
    {   CueRules r; fresh(&r);
        /* The same stroke NOT from hand is nobody's business but the striker's:
         * Rule 6 binds a player in hand and no one else. */
        r.bil_from_hand = 0; r.bil_red_baulk = 1; r.bil_wht_baulk = 1;
        Shot s = { .touch = { CUE_ID_BIL_RED } };
        play(&r, &s);
        ok(!r.last_foul, "...and none of it applies when not in hand", r.msg);
    }
    {   CueRules r; fresh(&r);
        /* Rule 16: double baulked, played properly, hit nothing. A MISS. */
        r.bil_from_hand = 1; r.bil_red_baulk = 1; r.bil_wht_baulk = 1;
        Shot s = { .touch = { 0 } };
        play(&r, &s);
        ok(r.score[1] == 2, "16: double baulked and missed = two away", r.msg);
        ok(!strncmp(r.msg, "MISS", 4),
           "...and it is a MISS, not a foul -- no spotting option", r.msg);
    }
    {   CueRules r; fresh(&r);
        /* ...but with a ball OUT of baulk to go at, the same failure is a foul:
         * Rule 16 only excuses the striker who had nothing to aim at. */
        r.bil_from_hand = 1; r.bil_red_baulk = 1; r.bil_wht_baulk = 0;
        Shot s = { .touch = { 0 } };
        play(&r, &s);
        ok(!strncmp(r.msg, "FOUL", 4),
           "16: a ball WAS out of baulk, so missing is a foul", r.msg);
    }
    {   CueRules r; fresh(&r);
        /* "all direct 'coups' are fouls" — the cue ball into a pocket having
         * hit nothing, however baulked the striker was. */
        r.bil_from_hand = 1; r.bil_red_baulk = 1; r.bil_wht_baulk = 1;
        Shot s = { .touch = { 0 }, .scratch = 1 };
        play(&r, &s);
        ok(!strncmp(r.msg, "FOUL", 4), "16: a coup is a foul from any position",
           r.msg);
    }

    /* ---- THE CLOCK (Section 3 Rule 5) ------------------------------------ */
    {   CueRules r; fresh(&r);
        cue_rules_bil_set_time(&r, 10.0f);
        ok(r.target_score == 0,
           "1(f): a timed game has no points target", "");
        cue_rules_bil_tick(&r, 4.0f);
        ok(!r.bil_timeup && !r.frame_over, "...still running at 6 seconds", "");
        cue_rules_bil_tick(&r, 8.0f);
        ok(r.bil_timeup, "5(a): the clock runs out and the referee calls TIME", "");
        ok(!r.frame_over,
           "...and the game is NOT over yet: the stroke made may finish", "");
        /* "Any stroke that has been made shall be allowed to finish and any
         * points scored shall be added to the appropriate side." */
        r.score[0] = 20; r.score[1] = 24;
        Shot s = { .touch = { CUE_ID_BIL_RED }, .pot = { CUE_ID_BIL_RED } };
        play(&r, &s);
        ok(r.score[0] == 23, "...the pot in flight still scores its three", r.msg);
        ok(r.frame_over, "...and THEN it is time", r.msg);
        ok(r.winner == 1, "1(f)(i): most points in the time wins", r.msg);
    }
    {   CueRules r; fresh(&r);
        cue_rules_bil_set_time(&r, 1.0f);
        r.score[0] = r.score[1] = 30;
        cue_rules_bil_tick(&r, 2.0f);
        cue_rules_bil_expire(&r);
        ok(r.frame_over, "5(c): level at time, and the game still ends", r.msg);
        ok(r.winner == -1, "...as a draw, with no tie-break set", r.msg);
        ok(r.frames[0] == 0 && r.frames[1] == 0,
           "...and a draw is booked to nobody", "");
    }
    {   CueRules r; fresh(&r);
        cue_rules_bil_set_time(&r, 600.0f);
        cue_rules_bil_tick(&r, 100.0f);
        cue_rules_next_frame(&r, &T);
        ok(r.bil_time > 599.0f,
           "the next frame of a timed match gets a FULL clock", "");
        ok(r.bil_time_len == 600.0f, "...of the same length", "");
    }

    /* IN-HAND IS THE D AND NOTHING ELSE -- Section 3 Rule 6, "the cue-ball must
     * be struck from a position on or within the lines of the 'D'". Billiards
     * is not scored as snooker, so kind is 0 and this fell through to the pool
     * answer: ball in hand anywhere on the cloth after every in-off. */
    {   CueRules r; fresh(&r);
        ok(cue_rules_in_hand_anywhere(&r) == 0,
           "in hand is the D, never anywhere on the table", "");
    }

    printf(s_fail ? "\nFAILED (%d)\n" : "\nPASSED\n", s_fail);
    return s_fail ? 1 : 0;
}
