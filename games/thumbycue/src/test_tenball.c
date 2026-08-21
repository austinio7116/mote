/* TEN-BALL: the rotation game out of a triangle.
 *
 * It shares its resolver with 9-ball, which is the point — they are one game
 * with a different rack and a different money ball. So what this asks about is
 * exactly the two things that are parameterised and the ways a shared resolver
 * can get them wrong:
 *
 *   THE MONEY BALL. A 10-ball frame must end when the TEN goes down and not
 *   when the nine does. That is the bug a copied resolver has: the 9 is a
 *   perfectly ordinary ball here, potted between the 8 and the 10 on the way
 *   through, and a frame that ended on it would look like the game working
 *   right up until somebody ran the rack.
 *
 *   THE BALL ON. Lowest first, and the ceiling of "lowest" is ten rather than
 *   nine — so with only the 9 and the 10 left the ball on is the 9, and with
 *   only the 10 left it is the 10. Left at nine, the 10 would never be legal
 *   to strike and the frame could not be finished at all.
 *
 * And the rack, which is the thing that makes it a different game to play:
 * WPA 3.3 pins three of the ten balls — the 1 at the apex, the 10 in the middle
 * of the rack, the 2 and 3 on the back corners — and those are checked as
 * positions on the cloth rather than as array indices, because it is where they
 * SIT that decides how the table breaks.
 *
 * The pots are faked rather than played: every rule here is about what the
 * resolver does with the fact that a ball went down.
 */
#include "cue_rules.h"
#include "cue_table.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int fails;
static void ok(int c, const char *m){printf("%-4s %s\n",c?"ok":"FAIL",m); if(!c)fails++;}

static void pot(CueBall *b, int n, int id) {
    for (int i = 0; i < n; i++) if (b[i].id == id) { b[i].on = 0; return; }
}
static const CueBall *ball(const CueBall *b, int n, int id) {
    for (int i = 0; i < n; i++) if (b[i].id == id) return &b[i];
    return NULL;
}
static int up(const CueBall *b, int n) {
    int c = 0;
    for (int i = 1; i < n; i++) if (b[i].on) c++;
    return c;
}
/* One shot: hit `first`, pot `ids`, one cushion seen, no scratch. */
static void shot(CueRules *r, CueBall *b, int n, const CueWorld *w,
                 int first, const int *ids, int nid) {
    for (int k = 0; k < nid; k++) pot(b, n, ids[k]);
    cue_rules_resolve(r, b, n, w, first, 0, 1, ids, nid);
}

int main(void) {
    static CueTable t; static CueWorld w; static CueBall b[CUE_MAX_BALLS];
    static CueRules r;
    int n;
    printf("ten-ball\n\n");

    /* ---- the table and the rack --------------------------------------- */
    cue_table_init(&t, CUE_GAME_US10);
    cue_table_build_world(&t, &w);
    n = cue_table_rack(&t, b);
    ok(!t.is_snooker,             "a pool table, not a snooker one");
    ok(t.nballs == 11,            "eleven balls: the cue and ten objects");
    ok(n == 11,                   "and the rack lays all eleven out");
    ok(fabsf(t.half_len - 1.27f) < 0.001f, "on the 9 ft bed, like 9-ball");
    ok(t.baulk_x < 0.0f,          "with a head string and no D");
    ok(up(b, n) == 10,            "ten object balls up");
    /* No ball outside 1..10 may be in the rack: an eleventh id would be a
     * stripe standing on the cloth that no rule mentions. */
    {   int bad = 0;
        for (int i = 1; i < n; i++) if (b[i].id < 1 || b[i].id > 10) bad++;
        ok(bad == 0,              "...numbered 1 to 10 and nothing else");
    }

    /* ---- WPA 3.3: the three balls that are not free -------------------- */
    {   Vec3 upd; const Vec3 foot = cue_table_foot_spot_dir(&t, &upd);
        const Vec3 side = v3(-upd.z, 0.0f, upd.x);
        /* Distance along the rack's spine, and across it, for a ball. */
        #define ALONG(q) (((q).x - foot.x)*upd.x  + ((q).z - foot.z)*upd.z)
        #define ACROSS(q) (((q).x - foot.x)*side.x + ((q).z - foot.z)*side.z)
        const float row = t.R * 1.7320508f;
        const CueBall *one = ball(b, n, 1), *ten = ball(b, n, 10);
        const CueBall *two = ball(b, n, 2), *three = ball(b, n, 3);
        ok(one && fabsf(ALONG(one->pos)) < 0.001f &&
                  fabsf(ACROSS(one->pos)) < 0.001f,
           "the 1 is on the foot spot, at the apex");
        ok(ten && fabsf(ALONG(ten->pos) - 2*row) < 0.001f &&
                  fabsf(ACROSS(ten->pos)) < 0.001f,
           "the 10 is in the middle of the rack: centre of the third row");
        /* The back row, and the two ends of it. */
        ok(two && three &&
           fabsf(ALONG(two->pos)   - 3*row) < 0.001f &&
           fabsf(ALONG(three->pos) - 3*row) < 0.001f,
           "the 2 and the 3 are both in the back row");
        ok(two && three &&
           fabsf(fabsf(ACROSS(two->pos))   - 3*t.R) < 0.001f &&
           fabsf(fabsf(ACROSS(three->pos)) - 3*t.R) < 0.001f &&
           (ACROSS(two->pos) * ACROSS(three->pos)) < 0.0f,
           "...one on each back CORNER, not next to each other");
        /* And it is a triangle: four rows of 1, 2, 3, 4. */
        {   int cnt[4] = {0,0,0,0}, off = 0;
            for (int i = 1; i < n; i++) {
                const int rw = (int)(ALONG(b[i].pos) / row + 0.5f);
                if (rw >= 0 && rw < 4) cnt[rw]++; else off++;
            }
            ok(!off && cnt[0]==1 && cnt[1]==2 && cnt[2]==3 && cnt[3]==4,
               "and the rack is a triangle: rows of 1, 2, 3, 4");
        }
        #undef ALONG
        #undef ACROSS
    }
    printf("\n");

    /* ---- the ball on, all the way down the rack ------------------------ */
    cue_rules_init(&r, &t, 0);
    n = cue_table_rack(&t, b);
    ok(r.seq == 1,                "the 1 is on to start with");
    ok(cue_rules_ball_legal(&r, b, n, 1),  "...and legal to strike");
    ok(!cue_rules_ball_legal(&r, b, n, 2), "...and the 2 is not");
    ok(!cue_rules_ball_legal(&r, b, n, 10),"...nor the 10");
    /* Walk the rack, checking the ball on each time. The interesting rows are
     * the last two: with the 9 and the 10 left the 9 is on, and with only the
     * 10 left the TEN is on — which a nine-ball ceiling would never allow. */
    r.break_shot = 0;
    for (int id = 1; id <= 9; id++) {
        int one[1] = { id };
        shot(&r, b, n, &w, id, one, 1);
        char m[96];
        snprintf(m, sizeof m, "potted the %d -> on the %d, %d up", id,
                 r.seq, up(b, n));
        ok(r.seq == id + 1 && !r.last_foul && r.turn == 0 && !r.frame_over, m);
    }
    ok(r.seq == 10,               "with only the ten left, the TEN is the ball on");
    ok(cue_rules_ball_legal(&r, b, n, 10), "...and it is legal to strike");
    {   int ten[1] = { 10 };
        shot(&r, b, n, &w, 10, ten, 1);
    }
    ok(r.frame_over && r.winner == 0, "and potting it wins the frame");
    ok(strstr(r.msg, "10-BALL") != NULL, "...saying 10-BALL, not 9-BALL");
    printf("\n");

    /* ---- THE NINE IS NOT THE MONEY BALL ------------------------------- */
    cue_rules_init(&r, &t, 0);
    n = cue_table_rack(&t, b);
    r.break_shot = 0;
    /* Clear 1 to 8 legally, then pot the 9 on the ball on. Nothing about that
     * ends a 10-ball frame. */
    for (int id = 1; id <= 8; id++) { int o[1] = { id }; shot(&r, b, n, &w, id, o, 1); }
    {   int o[1] = { 9 };
        shot(&r, b, n, &w, 9, o, 1);
    }
    ok(!r.frame_over,             "potting the 9 does not end a ten-ball frame");
    ok(r.seq == 10,               "...it just makes the ten the ball on");
    ok(up(b, n) == 1,             "...with one ball left up");
    printf("\n");

    /* ---- and the ten, potted on a foul, comes back ---------------------- */
    cue_rules_init(&r, &t, 0);
    n = cue_table_rack(&t, b);
    r.break_shot = 0;
    {   /* the ten down off the WRONG ball: a foul, so it spots */
        int o[1] = { 10 };
        for (int k = 0; k < 1; k++) pot(b, n, o[k]);
        cue_rules_resolve(&r, b, n, &w, 5, 0, 1, o, 1);
    }
    ok(r.last_foul,               "the ten down off the wrong ball is a foul");
    ok(!r.frame_over,             "...and does not win");
    ok(ball(b, n, 10)->on,        "...the ten comes back on the table");
    ok(r.ball_in_hand,            "...and the opponent has ball in hand");
    ok(r.turn == 1,               "...at the table");
    printf("\n");

    /* ---- three fouls in a row is still a loss -------------------------- */
    cue_rules_init(&r, &t, 0);
    n = cue_table_rack(&t, b);
    r.break_shot = 0;
    for (int f = 0; f < 3; f++) {
        /* hit nothing at all: a foul, three times, by the same player */
        r.turn = 0;
        cue_rules_resolve(&r, b, n, &w, -1, 0, 0, NULL, 0);
    }
    ok(r.frame_over && r.winner == 1, "three consecutive fouls loses the frame");
    printf("\n");

    /* ---- 9-ball is untouched by any of it ------------------------------ */
    {   static CueTable t9; static CueWorld w9; static CueBall b9[CUE_MAX_BALLS];
        static CueRules r9;
        cue_table_init(&t9, CUE_GAME_US9);
        cue_table_build_world(&t9, &w9);
        int n9 = cue_table_rack(&t9, b9);
        ok(n9 == 10,              "9-ball still racks ten balls");
        {   int bad = 0;
            for (int i = 1; i < n9; i++) if (b9[i].id < 1 || b9[i].id > 9) bad++;
            ok(bad == 0,          "...numbered 1 to 9");
        }
        cue_rules_init(&r9, &t9, 0);
        r9.break_shot = 0;
        for (int id = 1; id <= 8; id++) {
            for (int i = 0; i < n9; i++) if (b9[i].id == id) b9[i].on = 0;
            int o[1] = { id };
            cue_rules_resolve(&r9, b9, n9, &w9, id, 0, 1, o, 1);
        }
        ok(r9.seq == 9,           "...with the nine on at the end of it");
        for (int i = 0; i < n9; i++) if (b9[i].id == 9) b9[i].on = 0;
        {   int o[1] = { 9 };
            cue_rules_resolve(&r9, b9, n9, &w9, 9, 0, 1, o, 1);
        }
        ok(r9.frame_over,         "...and the nine still wins it");
        ok(strstr(r9.msg, "9-BALL") != NULL, "...saying 9-BALL");
    }

    /* ==== CALL SHOT (WPA), behind the option ================================
     *
     * Off, slop counts — everything above ran that way. On, every stroke after
     * the break carries a called ball and pocket: made, you continue and the
     * table keeps everything that fell; not made, the table passes as it lies
     * with no foul, and the 10 respots if it dropped. The 10 itself only wins
     * as called. `pocket` on the ball is what makes the call enforceable —
     * the physics records which hole every pot actually fell in. */
    {
        static CueTable tc; static CueWorld wc; static CueBall bc[CUE_MAX_BALLS];
        static CueRules rc;
        printf("\n");
        cue_table_init(&tc, CUE_GAME_US10);
        cue_table_build_world(&tc, &wc);
        int nc = cue_table_rack(&tc, bc);
        cue_rules_init(&rc, &tc, 0);
        rc.turn = 0; rc.break_shot = 0; rc.call_shot_on = 1;

        /* Pot ball `bid` into pocket `pk`, first contact `fh`. The parameters
         * must not be called `id`: a macro argument named after a struct member
         * substitutes into `bc[_i].id` and leaves `bc[_i].1`. */
        #define CPOT(fh, bid, pk) do {                                       \
                int _o[1] = { (bid) };                                       \
                for (int _i = 0; _i < nc; _i++)                              \
                    if (bc[_i].id == (bid)) { bc[_i].on = 0; bc[_i].pocket = (uint8_t)(pk); } \
                cue_rules_resolve(&rc, bc, nc, &wc, (fh), 0, 1, _o, 1);      \
            } while (0)

        cue_rules_call_shot(&rc, 1, 2);
        CPOT(1, 1, 2);
        ok(!rc.last_foul && rc.turn == 0, "called ball in the called pocket: carry on");

        cue_rules_call_shot(&rc, 2, 3);
        CPOT(2, 2, 5);                        /* right ball, wrong pocket */
        ok(!rc.last_foul,                 "the wrong pocket is NOT a foul");
        ok(rc.turn == 1,                  "...but the table passes");
        ok(strstr(rc.msg, "NOT AS CALLED") != NULL, "...and says NOT AS CALLED");
        {   int down = 1;
            for (int i = 0; i < nc; i++) if (bc[i].id == 2 && bc[i].on) down = 0;
            ok(down, "...and the slopped ball STAYS DOWN");
        }

        /* a call with no pocket can never be made */
        rc.turn = 0;
        cue_rules_call_shot(&rc, 3, -1);
        CPOT(3, 3, 1);
        ok(rc.turn == 1, "a pot with no pocket called passes the table");

        /* the 10, potted legally off the lowest ball but not as called */
        rc.turn = 0;
        cue_rules_call_shot(&rc, 4, 0);
        {   int o2[1] = { 10 };
            for (int i = 0; i < nc; i++)
                if (bc[i].id == 10) { bc[i].on = 0; bc[i].pocket = 4; }
            cue_rules_resolve(&rc, bc, nc, &wc, 4, 0, 1, o2, 1);
        }
        ok(!rc.frame_over, "the 10 slopped in: NOT a win under call shot");
        {   int back = 0;
            for (int i = 0; i < nc; i++) if (bc[i].id == 10 && bc[i].on) back = 1;
            ok(back, "...and it goes back on its spot");
        }

        /* the 10, called and made, wins */
        rc.turn = 0; rc.frame_over = 0;
        for (int i = 0; i < nc; i++)
            if (bc[i].id >= 4 && bc[i].id <= 9) bc[i].on = 0;   /* clear to the 10 */
        cue_rules_call_shot(&rc, 10, 1);
        CPOT(10, 10, 1);
        ok(rc.frame_over && rc.winner == 0, "the 10 called and made wins the frame");

        /* and the break is never a called shot */
        cue_table_init(&tc, CUE_GAME_US10);
        cue_table_build_world(&tc, &wc);
        nc = cue_table_rack(&tc, bc);
        cue_rules_init(&rc, &tc, 0);
        rc.turn = 0; rc.call_shot_on = 1;      /* break_shot still set by init */
        CPOT(1, 3, 4);                          /* a ball off the break, no call */
        ok(!rc.last_foul && rc.turn == 0,
           "a ball off the break counts with nothing called: the break is not a called shot");
        #undef CPOT
    }

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}
