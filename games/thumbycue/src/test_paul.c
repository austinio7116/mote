/*
 * PAUL — the game two friends invented on a 6 ft home snooker table.
 *
 * It is the only rule set in here that came from a person rather than a
 * federation, which makes the test unusually important: there is no document to
 * check the code against, so the test IS the document. Every assertion below is
 * one sentence of what was described, written down so that neither can drift
 * from the other.
 *
 *   THE WHOLE SET GOES ON AT RANDOM, differently every game, and legally: on
 *   the cloth, clear of the pockets, and not inside another ball. A frame that
 *   starts with a ball resting in a pocket scores for nobody before a cue is
 *   lifted.
 *
 *   THE BREAK MUST TOUCH NOTHING. This is the inverse of every other opening
 *   stroke in the file — everywhere else, failing to reach something is the
 *   offence — so it is exactly the rule a reader will assume is a typo, and the
 *   one most likely to be "fixed" by somebody tidying up.
 *
 *   REDS ONE, COLOURS TWO, THE BLACK FOUR, and nothing ever comes back up.
 *
 *   AN IN-OFF COSTS TWO SHOTS. A plain miss does not: it just ends the visit.
 *   That asymmetry is deliberate and is the thing a federation would have
 *   removed.
 *
 *   AND THE FRAME ENDS ON A LEAD BIGGER THAN WHAT IS LEFT — not on an empty
 *   table, which is a different and much later moment.
 */
#include "cue_rules.h"
#include "cue_table.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int fails;
static void ok(int c, const char *m){printf("%-4s %s\n",c?"ok":"FAIL",m); if(!c)fails++;}

static CueTable T;
static CueWorld W;
static CueBall  B[CUE_MAX_BALLS];
static CueRules R;
static int      N;

static void fresh(uint32_t seed) {
    cue_table_init(&T, CUE_GAME_PAUL);
    cue_table_build_world(&T, &W);
    cue_table_paul_set_seed(seed);
    N = cue_table_rack(&T, B);
    cue_rules_init(&R, &T, 0);
}
static CueBall *ball(int id) {
    for (int i = 0; i < N; i++) if (B[i].id == id) return &B[i];
    return NULL;
}
/* Take `id` off the table. */
static void pot(int id) {
    for (int i = 0; i < N; i++) if (B[i].id == id && B[i].on) { B[i].on = 0; return; }
}
/* One stroke: hit `first`, pot `ids`, and say whether a cushion was reached. */
static void shot(int first, int cushion, const int *ids, int nid) {
    for (int k = 0; k < nid; k++) pot(ids[k]);
    cue_rules_resolve(&R, B, N, &W, first, 0, cushion, ids, nid);
}
static void shot_scratch(int first, const int *ids, int nid) {
    for (int k = 0; k < nid; k++) pot(ids[k]);
    B[0].on = 0;
    cue_rules_resolve(&R, B, N, &W, first, 1, 1, ids, nid);
    B[0].on = 1;                          /* the host puts it back */
}
static int on_table(void) {
    int c = 0;
    for (int i = 1; i < N; i++) if (B[i].on) c++;
    return c;
}

int main(void) {
    printf("paul\n\n");

    /* ---- the table ------------------------------------------------------ */
    fresh(1);
    ok(fabsf(T.half_len * 2000.0f - 1829.0f) < 1.0f, "a 6 ft bed (1829 mm of cloth)");
    ok(fabsf(T.half_wid * 2000.0f -  914.0f) < 1.0f, "...by 3 ft (914 mm)");
    ok(fabsf(T.R * 2000.0f - 42.0f) < 0.01f,         "42 mm balls");
    ok(T.cue_R == 0.0f,                              "...and a matched white, not a pool one");
    ok(T.pocket_round == 1,                          "curved pockets, not mitred");
    {   float c = 0, m = 0;
        cue_table_openings(&T, &c, &m);
        char b[96];
        snprintf(b, sizeof b, "54 mm pockets: corner %.1f, middle %.1f (%.2f ball widths)",
                 (double)(c*1000), (double)(m*1000), (double)(c/(2*T.R)));
        ok(fabsf(c - 0.054f) < 0.0005f && fabsf(m - 0.054f) < 0.0005f, b); }
    {   /* The nose height is not a style choice: 63.5% of the ball, always. */
        char b[96];
        snprintf(b, sizeof b, "a %.1f mm nose, which is 63.5%% of the ball",
                 (double)(T.cushion_h*1000));
        ok(fabsf(T.cushion_h - T.R * 2.0f * 0.635f) < 1e-6f, b); }
    {   char b[96];
        snprintf(b, sizeof b, "thin cushions: a %.0f mm rail", (double)(T.rail_w*1000));
        ok(T.rail_w < 0.050f, b); }
    ok(cue_table_validate(&T, NULL, 0),              "and the whole table validates");
    printf("\n");

    /* ---- the scatter ---------------------------------------------------- */
    ok(N == 22,              "the whole set goes on: the white and twenty-one balls");
    {   int reds = 0, cols = 0, blacks = 0;
        for (int i = 1; i < N; i++) {
            if (B[i].id >= 1 && B[i].id <= 15) reds++;
            else if (B[i].id == CUE_ID_BLACK) blacks++;
            else if (B[i].id >= CUE_ID_YELLOW) cols++;
        }
        char b[96];
        snprintf(b, sizeof b, "...fifteen reds, five colours and a black (%d/%d/%d)",
                 reds, cols, blacks);
        ok(reds == 15 && cols == 5 && blacks == 1, b); }

    /* LEGAL, on every one of thirty layouts. A ball off the cloth, inside a
     * pocket or inside another ball is a frame that is wrong before it starts,
     * and rejection sampling with a budget is exactly the sort of thing that
     * works for twenty-nine seeds and not the thirtieth. */
    {   int bad_off = 0, bad_pkt = 0, bad_lap = 0;
        float worst = 1e30f;
        for (uint32_t sd = 1; sd <= 30; sd++) {
            fresh(sd);
            for (int i = 0; i < N; i++) {
                if (!cue_world_on_bed(&W, B[i].pos.x, B[i].pos.z)) bad_off++;
                for (int p = 0; p < W.npocket; p++) {
                    const float dx = B[i].pos.x - W.drop_c[p].x;
                    const float dz = B[i].pos.z - W.drop_c[p].z;
                    if (dx*dx + dz*dz < W.pocket_r[p]*W.pocket_r[p]) bad_pkt++;
                }
                for (int k = i + 1; k < N; k++) {
                    const float dx = B[i].pos.x - B[k].pos.x;
                    const float dz = B[i].pos.z - B[k].pos.z;
                    const float g = sqrtf(dx*dx + dz*dz) - 2.0f * T.R;
                    if (g < worst) worst = g;
                    if (g < -1e-5f) bad_lap++;
                }
            }
        }
        char b[128];
        snprintf(b, sizeof b, "thirty layouts, all legal (closest pair %.1f mm apart)",
                 (double)(worst*1000));
        ok(!bad_off && !bad_pkt && !bad_lap, b);
        if (bad_off) printf("     %d off the cloth\n", bad_off);
        if (bad_pkt) printf("     %d resting in a pocket\n", bad_pkt);
        if (bad_lap) printf("     %d overlapping\n", bad_lap);
    }

    /* ...AND ON EVERY SHAPE PAUL CAN BE PLAYED ON, with the WHOLE BALL judged
     * rather than its centre.
     *
     * The scatter's only guard was a rectangular window inset by a ball and a
     * half. That is the entire answer on a rectangle and none at all on a
     * polygon, where the cushion line cuts diagonally across that window: the
     * centre passes the point test and most of the ball is in the rubber.
     * Measured before the fix, over two hundred racks a bed — hexagon 4 balls,
     * octagon 5, round 3, L 3, rectangle 0, which is why it was only ever
     * reported on the odd shapes. */
    {   static const char *const NM[CUE_TAB_COUNT] =
            { "PRO", "TOURNAMENT", "CLUB", "L-SHAPE", "HEX", "OCT", "ROUND" };
        int worst_v = -1, worst_n = 0;
        for (int v = 0; v < CUE_TAB_COUNT; v++) {
            if (v != CUE_TAB_DEFAULT && !cue_table_variant_ok(CUE_GAME_PAUL, v)) continue;
            CueTable t; cue_table_init(&t, CUE_GAME_PAUL); cue_table_variant(&t, v);
            static CueWorld w; cue_table_build_world(&t, &w);
            for (uint32_t sd = 1; sd <= 60; sd++) {
                CueBall bb[CUE_MAX_BALLS];
                cue_table_paul_set_seed(sd);
                const int n = cue_table_rack(&t, bb);
                int off = 0;
                for (int i = 0; i < n; i++)
                    if (bb[i].on &&
                        !cue_world_ball_on_bed(&w, bb[i].pos.x, bb[i].pos.z, t.R))
                        off++;
                if (off > worst_n) { worst_n = off; worst_v = v; }
            }
        }
        char b[128];
        if (worst_n) snprintf(b, sizeof b, "no ball in the cushions on any shape "
                              "(worst: %d on %s)", worst_n, NM[worst_v]);
        else snprintf(b, sizeof b, "no ball in the cushions on any shape, "
                      "sixty racks a bed");
        ok(!worst_n, b);
    }

    /* DIFFERENT EVERY GAME, and the same every time for one seed. */
    {   fresh(11);
        Vec3 a = B[0].pos, c = B[7].pos;
        fresh(11);
        ok(B[0].pos.x == a.x && B[7].pos.z == c.z, "one seed lays out one table");
        fresh(12);
        ok(B[0].pos.x != a.x, "...and another seed lays out another");
        /* AND CONSECUTIVE SEEDS ARE NOT NEIGHBOURS. A caller counting frames
         * hands over 1, 2, 3 — and xorshift's first outputs from a small state
         * are so alike that the white marched across the table in even steps
         * with the same two z values. Measured: the spread of the white over
         * twenty consecutive seeds has to cover most of the bed. */
        float lo = 1e30f, hi = -1e30f;
        for (uint32_t sd = 1; sd <= 20; sd++) {
            fresh(sd);
            if (B[0].pos.x < lo) lo = B[0].pos.x;
            if (B[0].pos.x > hi) hi = B[0].pos.x;
        }
        char b[128];
        snprintf(b, sizeof b, "...and twenty in a row spread the white over %.0f mm "
                 "of a %.0f mm bed", (double)((hi-lo)*1000),
                 (double)(T.half_len*2000));
        ok((hi - lo) > T.half_len * 1.2f, b);
    }
    printf("\n");

    /* ---- there is no ball on ------------------------------------------- */
    fresh(3);
    ok(!R.kind,                       "it is not scored as snooker");
    ok(R.mode == CUE_GAME_PAUL,       "...it is scored as Paul");
    ok(cue_rules_ball_legal(&R, B, N, 1),               "a red is legal to strike");
    ok(cue_rules_ball_legal(&R, B, N, CUE_ID_BLACK),    "...and so is the black");
    ok(cue_rules_ball_legal(&R, B, N, CUE_ID_YELLOW),   "...and any colour");
    ok(!cue_rules_ball_legal(&R, B, N, CUE_ID_CUE),     "...and the white is not");
    ok(R.paul_left == 29,             "twenty-nine points on the table to start with");
    printf("\n");

    /* ---- THE BREAK MUST TOUCH NOTHING --------------------------------- */
    fresh(4);
    ok(R.break_shot,                  "the frame opens on a break");
    /* AND THE WHITE LIES WHERE IT LANDED. The whole set is thrown on at random
     * including the cue ball, so there is nothing to place — and the break is
     * the problem of finding it room to move. Put in the D it would start in
     * the one part of the table the scatter had been cleared out of. */
    ok(!R.ball_in_hand,               "...with the white where it landed, not in hand");
    shot(-1, 0, NULL, 0);
    ok(!R.last_foul,                  "a break that touches nothing is legal");
    ok(R.turn == 1,                   "...and the visit passes, having scored nothing");
    ok(R.shots_remaining == 1,        "...with no shots owed");

    fresh(5);
    shot(1, 0, NULL, 0);                            /* touched a red */
    ok(R.last_foul,                   "a break that touches a BALL is a foul");
    ok(strstr(R.msg, "HIT A BALL") != NULL, "...and says so");
    ok(R.turn == 1 && R.shots_remaining == 2, "...and gives the opponent two shots");

    fresh(6);
    shot(-1, 1, NULL, 0);                           /* reached a cushion */
    ok(R.last_foul,                   "a break that reaches a CUSHION is a foul");
    ok(strstr(R.msg, "HIT A RAIL") != NULL, "...and says so");
    ok(R.shots_remaining == 2,        "...two shots again");

    fresh(7);
    {   int one[1] = { 1 };
        shot(1, 1, one, 1); }
    ok(R.last_foul,                   "a break that pots is a foul, whatever it potted");
    ok(R.score[0] == 0,               "...and scores nothing for it");
    printf("\n");

    /* ---- what a ball is worth ------------------------------------------ */
    fresh(8);
    R.break_shot = 0;
    {   int one[1] = { 1 };            shot(1, 1, one, 1); }
    ok(R.score[0] == 1,               "a red is one");
    ok(R.turn == 0,                   "...and the visit continues");
    {   int one[1] = { CUE_ID_BLUE };  shot(CUE_ID_BLUE, 1, one, 1); }
    ok(R.score[0] == 3,               "a colour is two");
    {   int one[1] = { CUE_ID_BLACK }; shot(CUE_ID_BLACK, 1, one, 1); }
    ok(R.score[0] == 7,               "the black is four");
    ok(R.brk == 7,                    "...and the three of them are one break of 7");
    {   char b[64];
        snprintf(b, sizeof b, "...leaving %d on the table", R.paul_left);
        ok(R.paul_left == 29 - 7, b); }
    /* NOTHING COMES BACK. Three balls potted, three balls gone. */
    ok(on_table() == 21 - 3,          "and nothing is ever spotted");
    ok(ball(CUE_ID_BLACK) && !ball(CUE_ID_BLACK)->on, "...the black stays down");

    /* Several on one stroke all count. */
    {   int two[2] = { 1, CUE_ID_PINK };
        shot(1, 1, two, 2); }
    ok(R.score[0] == 7 + 1 + 2,       "two balls on one stroke both count");
    printf("\n");

    /* ---- the miss, and the in-off -------------------------------------- */
    fresh(9);
    R.break_shot = 0;
    shot(1, 1, NULL, 0);              /* hit a red, potted nothing */
    ok(!R.last_foul,                  "a miss is NOT a foul in this game");
    ok(R.turn == 1,                   "...it just ends the visit");
    ok(R.shots_remaining == 1,        "...and owes nothing");
    /* Not even missing everything. The only penalty this game names is the
     * in-off, and a table with no referee does not fine you for trying. */
    shot(-1, 1, NULL, 0);
    ok(!R.last_foul,                  "missing everything is not a foul either");
    ok(R.turn == 0,                   "...and the visit still just passes");

    fresh(10);
    R.break_shot = 0;
    {   int one[1] = { 1 };
        shot_scratch(1, one, 1); }
    ok(R.last_foul,                   "an in-off is a foul");
    ok(strstr(R.msg, "IN OFF") != NULL, "...and says so");
    ok(R.score[0] == 0,               "...the red it potted scores nothing");
    ok(R.turn == 1,                   "...the table changes hands");
    ok(R.shots_remaining == 2,        "...with two shots");
    ok(R.ball_in_hand,                "...and the white in hand, because it went down");
    /* AND IN THE D, not anywhere. Paul is not SCORED as snooker so `kind` is 0,
     * which is what sent this down the pool answer — the white could be put
     * down anywhere on the table after an in-off. */
    ok(!cue_rules_in_hand_anywhere(&R), "...to be placed in the D, not anywhere");
    {   /* And the clamp agrees: a point up at the far end comes back to the D. */
        const Vec3 far_ = v3(T.half_len * 0.8f, T.R, 0.0f);
        const Vec3 got = cue_table_clamp_placement_balls(&T, far_, B, N, 0);
        char b[128];
        snprintf(b, sizeof b, "...and a placement at +%.0f mm is pulled back to %+.0f",
                 (double)(far_.x*1000), (double)(got.x*1000));
        ok(got.x <= T.baulk_x + 0.001f, b); }
    printf("\n");

    /* ---- what two shots actually buys --------------------------------- */
    fresh(13);
    R.break_shot = 0;
    {   int one[1] = { 1 };
        shot_scratch(1, one, 1); }     /* player 0 fouls, player 1 has two */
    ok(R.turn == 1 && R.shots_remaining == 2, "two shots in hand");
    shot(1, 1, NULL, 0);                                  /* misses */
    ok(R.turn == 1,                   "a miss on the first of two stays at the table");
    ok(R.shots_remaining == 1,        "...with one left");
    shot(1, 1, NULL, 0);                                  /* misses again */
    ok(R.turn == 0,                   "...and a miss on the second passes it over");
    /* And a POT cancels the carry, as it does under pub rules. */
    fresh(14);
    R.break_shot = 0;
    {   int one[1] = { 1 };
        shot_scratch(1, one, 1); }
    ok(R.shots_remaining == 2,        "two shots again");
    {   int one[1] = { CUE_ID_BLUE };
        shot(CUE_ID_BLUE, 1, one, 1); }
    ok(R.shots_remaining == 1,        "...and potting spends them");
    ok(R.turn == 1,                   "...while the visit carries on");
    printf("\n");

    /* ---- how the frame ends ------------------------------------------- */
    /* A LEAD BIGGER THAN WHAT IS LEFT, which is usually long before the table
     * is empty: clear everything but one red and a four-point lead wins. */
    fresh(15);
    R.break_shot = 0;
    /* Player 0 takes fifteen reds and four colours, leaving the black and one
     * colour: 6 left, and a lead of 23. */
    {   for (int i = 0; i < 15; i++) { int o[1] = { 1 }; shot(1, 1, o, 1); }
        const int cols[4] = { CUE_ID_YELLOW, CUE_ID_GREEN, CUE_ID_BROWN, CUE_ID_BLUE };
        for (int i = 0; i < 4; i++) { int o[1] = { cols[i] }; shot(cols[i], 1, o, 1); } }
    {   char b[128];
        snprintf(b, sizeof b, "%d-%d with %d left: %s", R.score[0], R.score[1],
                 R.paul_left, R.frame_over ? "over" : "still on");
        ok(R.frame_over && R.winner == 0, b); }
    ok(R.paul_left == 6,              "...and it ended with the pink and the black up");

    /* LEVEL AND EMPTY re-spots the black. */
    fresh(16);
    R.break_shot = 0;
    /* Take everything but the black, splitting it evenly: 15 reds and 5
     * colours is 25 points, which does not divide — so give 0 the black's
     * five colours (10) and 1 fifteen reds (15), then pot the black to 1's
     * account... simpler: hand-set the scores and clear the table. */
    for (int i = 1; i < N; i++) if (B[i].id != CUE_ID_BLACK) B[i].on = 0;
    R.score[0] = R.score[1] = 12;
    pot(CUE_ID_BLACK);
    R.turn = 0;
    cue_rules_resolve(&R, B, N, &W, CUE_ID_BLACK, 0, 1, (int[]){CUE_ID_BLACK}, 1);
    /* That pot is worth four, so 0 is four ahead of nothing and has won. */
    ok(R.frame_over && R.winner == 0, "potting the last black wins from level");

    fresh(17);
    R.break_shot = 0;
    for (int i = 1; i < N; i++) B[i].on = 0;     /* an empty table */
    R.score[0] = R.score[1] = 14;
    R.turn = 0;
    cue_rules_resolve(&R, B, N, &W, -1, 0, 1, NULL, 0);
    ok(!R.frame_over,                 "level with an empty table is not a result");
    ok(ball(CUE_ID_BLACK) && ball(CUE_ID_BLACK)->on,
                                      "...the black goes back up");
    ok(R.paul_left == 4,              "...so there are four on the table again");
    {   char b[96];
        const CueBall *k = ball(CUE_ID_BLACK);
        snprintf(b, sizeof b, "...on its own spot, %.0f mm up the table",
                 k ? (double)(k->pos.x * 1000.0f) : 0.0);
        ok(k && fabsf(k->pos.x - T.black_x) < 0.001f, b); }

    /* ---- and every reason FITS THE BOARD ------------------------------- *
     *
     * CueRules::msg is 24 characters, which is one HUD line, and snprintf cuts
     * silently. The first three reasons written here were 30, 33 and 22 long
     * and two of them reached the player as "FOUL: THE BREAK TOUCHE". A reason
     * nobody can read is worse than no reason, and nothing else in the file
     * checks this. */
    {   const char *r_[] = { "BREAK HIT A BALL", "BREAK HIT A RAIL",
                             "BREAK POTTED", "IN OFF", "OFF THE TABLE" };
        int bad = 0;
        for (unsigned i = 0; i < sizeof r_ / sizeof r_[0]; i++) {
            char line[64];
            snprintf(line, sizeof line, "FOUL: %s", r_[i]);
            if (strlen(line) >= sizeof R.msg) {
                printf("     \"%s\" is %d, the board holds %d\n",
                       line, (int)strlen(line), (int)sizeof R.msg - 1);
                bad = 1;
            }
        }
        ok(!bad, "every foul reason fits the 24 characters the board has");
    }

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}
