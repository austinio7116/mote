/* G7 — BILLIARDS GOLF.
 *
 * The course comes off the Billiard & Golf scoreboard, so the first thing to
 * check is that it still IS that board: the nines total what the board prints,
 * and every hole racks on the cloth without two balls in the same place. The
 * rest is the scoring, which is the whole game — strokes, the cue ball's
 * one-stroke penalty, the eight-stroke limit, and low wins. */
#include "cue_table.h"
#include "cue_rules.h"
#include "cue_physics.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int s_fail;
static void ok(int c, const char *what, const char *detail) {
    printf("  %-4s %s%s%s%s\n", c ? "ok" : "FAIL", what,
           detail ? "  (" : "", detail ? detail : "", detail ? ")" : "");
    if (!c) s_fail++;
}

static CueTable T; static CueWorld W; static CueBall B[CUE_MAX_BALLS];

/* one stroke: `pot` reds go down, `scratch` says the cue ball did too */
static void stroke(CueRules *r, int pot, int scratch) {
    int potted[CUE_MAX_BALLS], np = 0;
    for (int i = 1; i < CUE_MAX_BALLS && pot > 0; i++)
        if (B[i].on) { B[i].on = 0; potted[np++] = B[i].id; pot--; }
    cue_rules_resolve(r, B, T.nballs, &W, 1, scratch, 1, potted, np);
}

int main(void) {
    printf("billiards golf\n");

    /* ---- the course is the board ---- */
    {   char d[64];
        int f = cue_golf_par(0, 8), b = cue_golf_par(9, 17);
        snprintf(d, sizeof d, "%d and %d, total %d", f, b, f + b);
        ok(f == 38 && b == 35, "the nines total what the board prints", d);
        int bad_par = 0;
        for (int h = 0; h < CUE_GOLF_HOLES; h++)
            if (CUE_GOLF_COURSE[h].par != CUE_GOLF_COURSE[h].n + 1) bad_par++;
        ok(bad_par == 0, "par is one more than the balls, on every hole", NULL);
    }

    /* ---- every hole racks legally ---- */
    {   int off = 0, overlap = 0; float worst = 9e9f;
        for (int h = 0; h < CUE_GOLF_HOLES; h++) {
            cue_table_golf_set_hole(h);
            cue_table_init(&T, CUE_GAME_GOLF);
            int n = cue_table_rack(&T, B);
            if (n != CUE_GOLF_COURSE[h].n + 1) off += 100;
            for (int i = 0; i < n; i++) {
                if (fabsf(B[i].pos.x) > T.half_len - T.R ||
                    fabsf(B[i].pos.z) > T.half_wid - T.R) off++;
                for (int j = i + 1; j < n; j++) {
                    float dx = B[i].pos.x - B[j].pos.x, dz = B[i].pos.z - B[j].pos.z;
                    float dd = sqrtf(dx*dx + dz*dz);
                    if (dd < worst) worst = dd;
                    if (dd < 2.0f*T.R - 0.0005f) overlap++;
                }
            }
        }
        char d[72];
        snprintf(d, sizeof d, "closest pair over all 18: %.1f mm, 2R = %.1f",
                 worst*1000.0f, 2000.0f*T.R);
        ok(off == 0, "every hole sets out on the cloth", NULL);
        ok(overlap == 0, "...with no two balls in the same place", d);
    }

    /* ---- the scoring ---- */
    cue_table_golf_set_hole(0);
    cue_table_init(&T, CUE_GAME_GOLF);
    cue_table_build_world(&T, &W);

    {   /* hole 1 is a par 4 with three reds: clear it in three */
        CueRules r; cue_rules_init(&r, &T, 0);
        r.golf_solo = 1;
        cue_table_rack(&T, B);
        stroke(&r, 1, 0); stroke(&r, 1, 0);
        ok(r.golf_card[0][0] == 0, "the hole is not scored until it is cleared", r.msg);
        stroke(&r, 1, 0);
        char d[48]; snprintf(d, sizeof d, "scored %d, par 4: %s", r.golf_card[0][0], r.msg);
        ok(r.golf_card[0][0] == 3, "three strokes to clear three reds scores 3", d);
        ok(r.golf_hole == 1, "...and the course moves to the next hole", NULL);
    }

    {   /* Rule 2: the cue ball down a hole costs a stroke and nothing else */
        CueRules r; cue_rules_init(&r, &T, 0);
        r.golf_solo = 1;
        cue_table_golf_set_hole(0); cue_table_rack(&T, B);
        int was = r.turn;
        stroke(&r, 0, 1);
        ok(r.golf_strokes == 2, "potting the cue ball costs two strokes, not one", r.msg);
        ok(r.golf_reset_cue, "...and asks for it back on its spot", NULL);
        ok(r.turn == was && !r.last_foul,
           "...and is a penalty, not a foul: nothing changes hands", NULL);
    }

    {   /* Rule 3: eight is the most a hole can cost */
        CueRules r; cue_rules_init(&r, &T, 0);
        r.golf_solo = 1;
        cue_table_golf_set_hole(0); cue_table_rack(&T, B);
        for (int i = 0; i < 8; i++) stroke(&r, 0, 0);
        char d[48]; snprintf(d, sizeof d, "%s", r.msg);
        ok(r.golf_card[0][0] == CUE_GOLF_MAX_STROKES,
           "a hole nobody clears is written down as eight", d);
        ok(r.golf_hole == 1, "...and the course moves on regardless", NULL);
    }

    {   /* two players: both play the same hole before the course moves */
        CueRules r; cue_rules_init(&r, &T, 0);
        cue_table_golf_set_hole(0); cue_table_rack(&T, B);
        stroke(&r, 3, 0);                       /* player 0 clears it in one */
        ok(r.golf_hole == 0 && r.turn == 1,
           "the other player plays the SAME hole", NULL);
        ok(r.golf_rack, "...off a fresh rack of it", NULL);
        cue_table_rack(&T, B);
        stroke(&r, 1, 0); stroke(&r, 1, 0); stroke(&r, 1, 0);
        ok(r.golf_hole == 1, "only then does the course move on", NULL);
        char d[48];
        snprintf(d, sizeof d, "%d against %d", r.golf_card[0][0], r.golf_card[1][0]);
        ok(r.golf_card[0][0] == 1 && r.golf_card[1][0] == 3, "both are on the card", d);
    }

    {   /* Rule 4: LOW wins */
        CueRules r; cue_rules_init(&r, &T, 0);
        for (int h = 0; h < CUE_GOLF_HOLES; h++) {
            r.golf_card[0][h] = 4;
            r.golf_card[1][h] = 5;
        }
        char d[48];
        snprintf(d, sizeof d, "%d against %d",
                 cue_rules_golf_total(&r, 0, 0, 17), cue_rules_golf_total(&r, 1, 0, 17));
        ok(cue_rules_golf_leader(&r) == 0, "the LOWEST round leads", d);
    }

    /* ---- the card crosses the wire whole ----
     * Golf's whole state — which hole, the strokes on it, and both players'
     * eighteen — lives inside CueRules, which the lockstep memcpys as one
     * block. That only works while it FITS, and the card is the first thing
     * here big enough to threaten it. */
    {   char d[64];
        snprintf(d, sizeof d, "CueRules is %d bytes", (int)sizeof(CueRules));
        ok(sizeof(CueRules) <= 768, "the rules still fit the state packet", d);
        CueRules a; cue_rules_init(&a, &T, 0);
        for (int h = 0; h < CUE_GOLF_HOLES; h++) {
            a.golf_card[0][h] = (uint8_t)(h % 8 + 1);
            a.golf_card[1][h] = (uint8_t)((h + 3) % 8 + 1);
        }
        a.golf_hole = 7; a.golf_strokes = 3;
        CueRules b2;
        memcpy(&b2, &a, sizeof(CueRules));       /* what the wire does */
        int same = (b2.golf_hole == 7 && b2.golf_strokes == 3);
        for (int h = 0; h < CUE_GOLF_HOLES; h++)
            if (b2.golf_card[0][h] != a.golf_card[0][h] ||
                b2.golf_card[1][h] != a.golf_card[1][h]) same = 0;
        ok(same, "...and the whole card survives the copy", NULL);
        snprintf(d, sizeof d, "%d and %d",
                 cue_rules_golf_total(&b2, 0, 0, 8), cue_rules_golf_total(&b2, 1, 0, 8));
        ok(cue_rules_golf_total(&b2, 0, 0, 8) == cue_rules_golf_total(&a, 0, 0, 8),
           "...totals and all", d);
    }

    /* ---- the hole is set out the same on both ends ----
     * Each end racks from its own copy of golf_hole, so the layout has to be a
     * pure function of it — a hole that came out differently on two machines
     * would be two different games. */
    {   int diff = 0;
        for (int h = 0; h < CUE_GOLF_HOLES; h++) {
            CueBall a[CUE_MAX_BALLS], b2[CUE_MAX_BALLS];
            cue_table_golf_set_hole(h); int na = cue_table_rack(&T, a);
            memcpy(b2, a, sizeof b2);
            cue_table_golf_set_hole(h); int nb = cue_table_rack(&T, b2);
            if (na != nb) { diff++; continue; }
            for (int i = 0; i < na; i++)
                if (a[i].pos.x != b2[i].pos.x || a[i].pos.z != b2[i].pos.z ||
                    a[i].id != b2[i].id) diff++;
        }
        ok(diff == 0, "a hole racks identically every time it is set out", NULL);
    }

    /* ---- the honour ----
     * Golf's turn order is not "whoever did not just play": the lower score on
     * the last hole leads the next, and a tie leaves the honour where it was.
     * That is a thing the round remembers. */
    {   CueRules r; cue_rules_init(&r, &T, 0);
        cue_rules_set_break(&r, 0);              /* the draw: player 0 leads */
        ok(r.golf_honour == 0 && r.turn == 0,
           "the break draw is the first hole's honour", NULL);
        cue_table_golf_set_hole(0); cue_table_rack(&T, B);
        stroke(&r, 3, 0);                        /* 0 clears hole 1 in one */
        cue_table_rack(&T, B);
        stroke(&r, 1, 0); stroke(&r, 1, 0); stroke(&r, 1, 0);   /* 1 takes three */
        char d[56];
        snprintf(d, sizeof d, "%d against %d, honour to %d",
                 r.golf_card[0][0], r.golf_card[1][0], r.golf_honour);
        ok(r.golf_honour == 0 && r.turn == 0, "the lower score leads the next hole", d);

        /* ...and a tied hole leaves it alone */
        cue_table_golf_set_hole(1); cue_table_rack(&T, B);
        stroke(&r, 2, 0);                        /* 0 clears hole 2 in one */
        cue_table_rack(&T, B);
        stroke(&r, 2, 0);                        /* 1 does the same */
        snprintf(d, sizeof d, "%d against %d, honour still %d",
                 r.golf_card[0][1], r.golf_card[1][1], r.golf_honour);
        ok(r.golf_card[0][1] == r.golf_card[1][1] && r.golf_honour == 0,
           "a tied hole leaves the honour where it was", d);

        /* ...and losing a hole hands it over */
        cue_table_golf_set_hole(2); cue_table_rack(&T, B);
        stroke(&r, 1, 0); stroke(&r, 1, 0); stroke(&r, 1, 0); stroke(&r, 1, 0);
        cue_table_rack(&T, B);
        stroke(&r, 4, 0);                        /* 1 clears it in one */
        snprintf(d, sizeof d, "%d against %d, honour to %d",
                 r.golf_card[0][2], r.golf_card[1][2], r.golf_honour);
        ok(r.golf_honour == 1 && r.turn == 1, "...and the honour changes hands", d);
    }

    /* ---- nine-hole rounds ---- */
    {   CueRules r; cue_rules_init(&r, &T, 0);
        r.golf_solo = 1;
        cue_rules_set_golf_round(&r, CUE_GOLF_BACK9);
        char d[56];
        snprintf(d, sizeof d, "starts on hole %d", r.golf_hole + 1);
        ok(r.golf_hole == 9, "the back nine starts at the tenth", d);
        ok(cue_golf_par(cue_golf_first(CUE_GOLF_BACK9),
                        cue_golf_last(CUE_GOLF_BACK9)) == 35,
           "...and its par is the board's 35", NULL);
        ok(cue_golf_par(cue_golf_first(CUE_GOLF_FRONT9),
                        cue_golf_last(CUE_GOLF_FRONT9)) == 38,
           "the front nine's is 38", NULL);
        /* nine holes and it is over, not eighteen */
        for (int h = 0; h < 9 && !r.frame_over; h++) {
            cue_table_golf_set_hole(r.golf_hole);
            cue_table_rack(&T, B);
            stroke(&r, CUE_GOLF_COURSE[r.golf_hole].n, 0);
        }
        snprintf(d, sizeof d, "over after hole %d", r.golf_hole + 1);
        ok(r.frame_over, "a back nine is nine holes, not eighteen", d);
        ok(r.golf_card[0][0] == 0 && r.golf_card[0][9] != 0,
           "...and only the back nine is written on the card", NULL);
    }

    printf(s_fail ? "\nFAILED (%d)\n" : "\nPASSED\n", s_fail);
    return s_fail ? 1 : 0;
}
