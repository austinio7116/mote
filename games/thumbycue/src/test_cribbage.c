/*
 * CRIBBAGE POOL — the pairs, and the ball that belongs to none of them.
 *
 * The potting is faked rather than played: a ball is taken off and told which
 * pocket it fell in, exactly as test_bowlliards and test_straight do it. Every
 * rule in this game is about what the SCORER does with that fact — which pair
 * is open, what a stroke owes, what goes back on the cloth — and driving the
 * integrator would be testing the integrator.
 *
 * Most of the file exists to reach one position: FOUR AND FOUR WITH AN EMPTY
 * TABLE. Seven pairs cannot take either player to five, so the rack runs out
 * level, and a game that does not then spot the 15 does not fail, or complain,
 * or end — it simply stops, with two players who can neither score nor lose.
 * That is the mistake this game invites and it is invisible from anywhere but
 * here, so it is played out ball by ball rather than asserted about.
 */
#include "cue_rules.h"
#include "cue_table.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;
static void ok(int c, const char *m) {
    printf("  %-4s %s\n", c ? "ok" : "FAIL", m);
    if (!c) fails++;
}

static CueTable T;
static CueWorld W;
static CueBall  B[CUE_MAX_BALLS];
static CueRules R;
static int      NB;

/* ---- the host's half of a stroke ---------------------------------------
 * The rules name balls to be spotted and cannot place one, because they hold no
 * table — the same contract cuevr_app.c honours after every resolve. Without
 * this the unpaired ball never comes back, the spotted 15 never reappears, and
 * the test would be measuring a game nobody plays. */
/* What the last resolve asked the host to do, kept because the host CONSUMES
 * it — a test that reads r->rerack after the rack has gone back up reads a
 * zero and learns nothing. */
static int last_rerack;
static void host(void) {
    last_rerack = R.rerack;
    if (R.rerack) {
        if (R.rerack == 2) NB = cue_table_rack(&T, B);
        R.rerack = 0;
    }
    int done = 0;
    while (R.respot > 0) {
        const int want = (done < 8) ? R.respot_id[done] : 0;
        for (int i = 1; i < NB; i++)
            if (B[i].id == want && !B[i].on) {
                cue_table_respot_ball(&T, B, NB, i);
                break;
            }
        R.respot--; done++;
    }
    R.respot = 0;
    if (R.ball_in_hand) { B[0].on = 1; B[0].pocket = 0; }
}

static int up(int id) {
    for (int i = 1; i < NB; i++) if (B[i].id == id) return B[i].on;
    return 0;
}
static int nup(void) {
    int c = 0;
    for (int i = 1; i < NB; i++) if (B[i].on && B[i].id >= 1 && B[i].id <= 15) c++;
    return c;
}
static int first_up(void) {
    for (int i = 1; i < NB; i++) if (B[i].on) return B[i].id;
    return -1;
}
static void take(int id) {
    for (int i = 0; i < NB; i++)
        if (B[i].id == id) { B[i].on = 0; B[i].pocket = 1; return; }
}
static int owes(int id) {
    for (int i = 0; i < (int)R.cr_nowed; i++) if (R.cr_owed[i] == id) return 1;
    return 0;
}

static void fresh(void) {
    cue_table_init(&T, CUE_GAME_CRIBBAGE);
    cue_table_build_world(&T, &W);
    NB = cue_table_rack(&T, B);
    cue_rules_init(&R, &T, 0);
    /* Four object balls to a rail, which is what an open break asks for. Set
     * once and left: nothing but a break ever reads it. */
    for (int i = 1; i < 5; i++) W.cush[i] = 1;
}

/* A stroke: name a ball and a pocket, drop this list of balls in pocket 1. */
static void stroke(int call, const int *ids, int nids) {
    int p[16], np = 0;
    const int hit = call > 0 ? call : first_up();
    cue_rules_call_shot(&R, call, 1);
    for (int i = 0; i < nids; i++) { take(ids[i]); p[np++] = ids[i]; }
    cue_rules_resolve(&R, B, NB, &W, hit, 0, 1, np ? p : NULL, np);
    host();
}
static void pot(int id) { int a[1]; a[0] = id; stroke(id, a, 1); }
static void miss(int call) { stroke(call, NULL, 0); }
static void pair(int a) { pot(a); pot(15 - a); }
/* A scratch, which is the cheapest foul to fake. */
static void scratch(void) {
    const int call = first_up();
    cue_rules_call_shot(&R, call, 1);
    cue_rules_resolve(&R, B, NB, &W, call, 1, 1, NULL, 0);
    host();
}
/* The opening stroke, with a rail count that satisfies the open break. */
static void brk(void) {
    cue_rules_resolve(&R, B, NB, &W, first_up(), 0, 1, NULL, 0);
    host();
}

/* ---- the rack, MEASURED ------------------------------------------------ *
 * Read off the cloth rather than out of the rack function's own array order,
 * because "the 15 is in the middle" is a claim about where the ball IS. Rows
 * run away from the apex in steps of R*sqrt(3); within a row the balls are two
 * radii apart across the table. */
static int row_ball[5][5], row_n[5];
static void read_rack(void) {
    float x0 = 1e30f;
    for (int i = 1; i < NB; i++) if (B[i].on && B[i].pos.x < x0) x0 = B[i].pos.x;
    const float dx = T.R * 1.7320508f;
    for (int r = 0; r < 5; r++) row_n[r] = 0;
    for (int i = 1; i < NB; i++) {
        if (!B[i].on) continue;
        const int r = (int)lroundf((B[i].pos.x - x0) / dx);
        if (r < 0 || r > 4 || row_n[r] >= 5) continue;
        /* insert by position across the table, so a row reads left to right */
        int k = row_n[r];
        while (k > 0 && B[row_ball[r][k-1]].pos.z > B[i].pos.z) {
            row_ball[r][k] = row_ball[r][k-1]; k--;
        }
        row_ball[r][k] = i; row_n[r]++;
    }
}
static int row_id(int r, int k) { return B[row_ball[r][k]].id; }

int main(void) {
    printf("cribbage pool\n");

    /* ---- the table -------------------------------------------------------- */
    fresh();
    ok(!T.is_snooker,           "a pool table, not a snooker one");
    ok(T.nballs == 16,          "sixteen balls: the cue ball and all fifteen");
    ok(NB == 16,                "and the rack lays all sixteen out");
    ok(T.half_len > 1.2f && T.half_len < 1.3f, "on the 9 ft bed (2.54 m long)");
    ok(T.baulk_x < 0.0f,        "with a head string, like the other US tables");
    ok(R.target_score == 5,     "five cribbages take the game");
    ok(R.break_shot && R.ball_in_hand, "the frame opens with a break from in hand");
    ok(cue_rules_in_hand_anywhere(&R) == 2, "...behind the head string");

    /* ---- THE RACK, and the constraint that is the point of it ------------- */
    read_rack();
    ok(row_n[0] == 1 && row_n[1] == 2 && row_n[2] == 3 &&
       row_n[3] == 4 && row_n[4] == 5, "a five-row triangle, apex first");
    ok(row_id(2, 1) == 15,      "the 15 is buried in the centre of the third row");
    {   const int c[3] = { row_id(0, 0), row_id(4, 0), row_id(4, 4) };
        ok(c[0] + c[1] != 15 && c[0] + c[2] != 15 && c[1] + c[2] != 15,
           "no two of the three corner balls total fifteen");
        /* ...and the reason that is worth a rule: a corner pair adding to
         * fifteen is a cribbage lying in the open before a ball is struck. */
        int seen[16]; memset(seen, 0, sizeof seen);
        for (int r = 0; r < 5; r++)
            for (int k = 0; k < row_n[r]; k++) seen[row_id(r, k)]++;
        int all = 1;
        for (int i = 1; i <= 15; i++) if (seen[i] != 1) all = 0;
        ok(all, "...and each of the fifteen is in the rack exactly once"); }

    /* ---- THE OPEN BREAK --------------------------------------------------- */
    fresh();
    brk();
    ok(!R.last_foul && !R.rerack, "four balls to a rail is a legal open break");
    ok(R.turn == 1,              "...but it potted nothing, so the table passes");

    fresh();
    for (int i = 0; i < CUE_MAX_BALLS; i++) W.cush[i] = 0;
    brk();
    ok(!R.last_foul,             "a short break is not a foul");
    ok(last_rerack == 2 && R.break_shot, "...it is a re-rack and another break");
    ok(R.turn == 1,              "...taken by the other player");
    ok(nup() == 15,              "...off a full rack");

    fresh();
    { int a[1]; a[0] = 4;
      for (int i = 0; i < CUE_MAX_BALLS; i++) W.cush[i] = 0;
      take(4);
      cue_rules_resolve(&R, B, NB, &W, 4, 0, 1, a, 1); host(); }
    ok(!last_rerack && R.turn == 0, "a ball potted satisfies the break on its own");
    ok(owes(11) && R.cr_nowed == 1, "...and it counts: the breaker is on the 11");

    /* THE BREAK IS NOT EXEMPT FROM FOULS, which is where this game and
     * bowlliards next door part company. */
    fresh();
    cue_rules_resolve(&R, B, NB, &W, 3, 1, 1, NULL, 0);   /* scratch */
    host();
    ok(R.last_foul,              "a scratch on the break is a foul");
    ok(R.turn == 1 && R.ball_in_hand, "...and hands over the ball in hand");

    /* ---- ONE BALL PUTS YOU ON A CRIBBAGE ---------------------------------- */
    fresh();
    brk();
    R.turn = 0;
    pot(4);
    ok(R.cr_nowed == 1 && owes(11), "potting the 4 puts the striker on the 11");
    ok(R.turn == 0,              "...and he stays at the table to take it");
    ok(R.score[0] == 0,          "half a cribbage is worth nothing");
    ok(cue_rules_ball_legal(&R, B, NB, 11),  "the 11 is the ball on");
    ok(!cue_rules_ball_legal(&R, B, NB, 3),  "...and nothing else is legal");
    ok(!cue_rules_ball_legal(&R, B, NB, 15), "...the 15 least of all");

    pot(11);
    ok(R.score[0] == 1,          "the companion makes the cribbage");
    ok(R.cr_nowed == 0,          "...and the debt is paid");
    ok(R.turn == 0,              "...and the striker plays on");
    ok(cue_rules_ball_legal(&R, B, NB, 3) &&
       cue_rules_ball_legal(&R, B, NB, 12),
       "off a cribbage, any ball may open the next one");
    ok(!cue_rules_ball_legal(&R, B, NB, 15),
       "...except the 15, which can start nothing");

    /* ---- MISSING THE COMPANION -------------------------------------------- */
    pot(6);
    ok(R.cr_nowed == 1 && owes(9), "the 6 opens a second pair");
    miss(9);
    ok(R.last_foul,              "failing to pot the companion is a foul");
    ok(R.turn == 1,              "...and the inning ends");
    ok(R.ball_in_hand,           "...with the cue ball in hand");
    ok(up(6),                    "...the unpaired 6 goes back on the table");
    ok(R.cr_nowed == 0,          "...and the pair is forgotten");
    ok(R.score[0] == 1,          "the cribbage already made is untouched");
    ok(!up(4) && !up(11),        "...and its two balls stay down");

    /* ---- BOTH BALLS ON ONE STROKE ----------------------------------------- */
    fresh();
    brk();
    R.turn = 0;
    { int a[2]; a[0] = 4; a[1] = 11; stroke(4, a, 2); }
    ok(R.score[0] == 1,          "a pair on one stroke is a cribbage");
    ok(R.cr_nowed == 0 && R.turn == 0, "...and leaves nothing owed");

    /* ---- SEVERAL BALLS ON ONE STROKE BUILD A LIST ------------------------- */
    fresh();
    brk();
    R.turn = 0;
    { int a[3]; a[0] = 2; a[1] = 4; a[2] = 6; stroke(4, a, 3); }
    ok(R.score[0] == 0,          "three unpaired balls score nothing yet");
    ok(R.cr_nowed == 3 && owes(13) && owes(11) && owes(9),
       "...they leave three companions owed");
    ok(cue_rules_ball_legal(&R, B, NB, 13) &&
       cue_rules_ball_legal(&R, B, NB, 11) &&
       cue_rules_ball_legal(&R, B, NB, 9),
       "any of the three may be taken next");
    ok(!cue_rules_ball_legal(&R, B, NB, 5), "...and nothing outside the list");
    pot(11);
    ok(R.score[0] == 1 && R.cr_nowed == 2, "taking one of them scores it");
    ok(R.turn == 0,              "...and the rest are still owed");
    { int a[2]; a[0] = 9; a[1] = 3; stroke(9, a, 2); }
    ok(R.score[0] == 2,          "a ball potted while working through the list");
    ok(R.cr_nowed == 2 && owes(13) && owes(12), "...joins it");

    /* ---- THE 15 TAKEN EARLY ----------------------------------------------- */
    fresh();
    brk();
    R.turn = 0;
    { int a[2]; a[0] = 4; a[1] = 15; stroke(4, a, 2); }
    ok(!R.last_foul,             "the 15 potted early is not a foul");
    ok(up(15),                   "...it is spotted straight back on");
    ok(R.cr_nowed == 1 && owes(11), "...and the ball beside it still counts");
    ok(R.turn == 0,              "...and the striker keeps the table");

    fresh();
    brk();
    R.turn = 0;
    { int a[1]; a[0] = 15; stroke(15, a, 1); }
    ok(!R.last_foul,             "the 15 alone is not a foul either");
    ok(up(15) && R.score[0] == 0, "...it comes back and scores nothing");
    ok(R.cr_nowed == 0,          "...and it starts no cribbage");
    ok(R.turn == 1,              "...but a stroke that did nothing ends the inning");

    /* ---- THREE FOULS IN A ROW LOSE THE GAME ------------------------------- */
    fresh();
    brk();
    R.turn = 0;
    for (int i = 0; i < 3; i++) {
        scratch();
        if (R.frame_over) break;
        miss(first_up());             /* the other player, doing nothing much */
    }
    ok(R.frame_over && R.winner == 1, "three successive fouls lose the game");

    /* ---- FIVE TAKES IT, AND THE RACK IS NOT PLAYED OUT -------------------- */
    fresh();
    brk();
    R.turn = 0;
    pair(1); pair(2); pair(3); pair(4);
    ok(R.score[0] == 4 && !R.frame_over, "four cribbages is not yet a game");
    pair(5);
    ok(R.frame_over && R.winner == 0, "the fifth wins it on the spot");
    ok(nup() == 5,                "...with five balls still on the table");

    /* ---- FOUR AND FOUR: THE 15 DECIDES IT --------------------------------- *
     * Seven pairs between two players is four and three at best, so the rack
     * runs out level once the 15 has been taken. Played out rather than
     * asserted, because the failure being looked for is the game quietly
     * having nothing left to do. */
    fresh();
    brk();
    R.turn = 0;
    pair(1); pair(2); pair(3); pair(4);          /* seat 0: four */
    scratch();                                    /* ...and hands over */
    ok(R.turn == 1 && R.score[0] == 4, "seat 0 has four and has left the table");
    pair(5); pair(6); pair(7);                    /* seat 1: three */
    ok(R.score[1] == 3,          "seat 1 has three, and the pairs are gone");
    ok(nup() == 1 && up(15),     "...leaving the 15 on its own");
    ok(cue_rules_ball_legal(&R, B, NB, 15), "which is now the ball on");
    ok(R.turn == 1,              "...and seat 1 is still at the table");
    pot(15);
    ok(R.score[1] == 4,          "the 15 alone is the eighth cribbage");
    ok(!R.frame_over,            "four and four: nobody has five");
    ok(up(15),                   "...so it is spotted and played again");
    ok(nup() == 1,               "...and it is the only ball there is");
    ok(R.turn == 1,              "...by the player who potted it");
    {   /* What the planner is actually offered, which is the question
         * cue_rules_ball_legal exists to answer: it is asked of the balls that
         * are UP, and there had better be exactly one of them. */
        int legal = 0;
        for (int i = 1; i < NB; i++)
            if (B[i].on && cue_rules_ball_legal(&R, B, NB, B[i].id)) legal++;
        ok(legal == 1 && cue_rules_ball_legal(&R, B, NB, 15),
           "the deciding cribbage is the only shot the planner is offered"); }
    pot(15);
    ok(R.frame_over && R.winner == 1, "and potting it takes the game");

    /* ...and the same position missed, which has to be a foul: with nothing
     * else on the cloth a striker who may miss for free is the deadlock back
     * again, taking turns instead of standing still. */
    fresh();
    brk();
    R.turn = 0;
    pair(1); pair(2); pair(3); pair(4);
    scratch();
    pair(5); pair(6); pair(7);
    pot(15);
    ok(R.score[0] == 4 && R.score[1] == 4, "level again, with the 15 respotted");
    miss(15);
    ok(R.last_foul,              "missing the deciding 15 is a foul");
    ok(R.turn == 0,              "...and the other player is invited to try");
    ok(up(15) && nup() == 1,     "...at the same ball");
    pot(15);
    ok(R.frame_over && R.winner == 0, "who takes it, five to four");

    printf(fails ? "\n%d FAILED\n" : "\nall good\n", fails);
    return fails != 0;
}
