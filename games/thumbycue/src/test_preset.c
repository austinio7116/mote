/*
 * ThumbyCue — the spec a standard table is cut to.
 *
 * A game did not used to have a table to choose between: snooker was one bed
 * with one set of pockets under one cloth. Now the same frame can be played on
 * a match table, a professional's practice table or the one down the club, and
 * the differences are real numbers rather than a difficulty multiplier — the
 * opening across the pocket, how fast the cloth runs, how lively the rubber is.
 *
 * Four things can go wrong with that, and each is asked about here directly:
 *
 *   TOURNAMENT MUST BE THE SHIPPED TABLE, to the bit. It is the default and it
 *   is what everybody has been playing on; if applying it moves a single field
 *   then every existing table, every saved spec and every match record has
 *   quietly changed underneath. Compared byte for byte, not to a tolerance.
 *
 *   A SPEC MUST GO THE WAY IT SAYS. Pro is the tight table and club is the
 *   generous one, at BOTH pocket types and on every table. A spec that loosens
 *   a middle on the way to "pro" is not a mis-tuned number, it is the family
 *   table being wrong — which is exactly how 6-red first came out, filed under
 *   snooker and given a 12 ft table's middles on a 7 ft bed.
 *
 *   THE SOLVE MUST LAND. The opening is measured and not authored, so a spec
 *   asks for a millimetre figure and cue_table_cut_to bisects for it. If it
 *   silently misses, the table plays nothing like the one named on the menu.
 *
 *   AND EVERY ONE OF THEM MUST BE PLAYABLE. Three specs across every standard
 *   game is a couple of dozen tables that nobody dialled by hand, and the
 *   validator is the only thing standing between a generated pocket and a
 *   frame that cannot be finished.
 *
 * The cloth is the other half of a spec, so it gets the same treatment: it has
 * to reach the physics, survive the wire, and be in the hash — two ends that
 * disagree about how fast the cloth is are not playing the same frame.
 */
#include "cue_table.h"
#include "cue_physics.h"
#include "cue_rules.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>

static int fails;
static void ok(int c, const char *what) {
    printf("%-4s %s\n", c ? "ok" : "FAIL", what);
    if (!c) fails++;
}

/* Every game that has specs, plus two that must not. */
static const struct { CueGameKind k; const char *name; int specs; } G[] = {
    { CUE_GAME_SNK15,       "snooker 12ft",   1 },
    { CUE_GAME_SNK10,       "snooker 10ft",   1 },
    { CUE_GAME_SNK6,        "6-red 7ft",      1 },
    { CUE_GAME_BILLIARDS,   "billiards",      1 },
    { CUE_GAME_UK8,         "UK 8-ball 7ft",  1 },
    { CUE_GAME_US8,         "US 8-ball 9ft",  1 },
    { CUE_GAME_US9,         "9-ball 9ft",     1 },
    { CUE_GAME_STRAIGHT,    "straight 9ft",   1 },
    { CUE_GAME_CN8,         "chinese 10ft",   1 },
    { CUE_GAME_PYRAMID,     "pyramid 12ft",   0 },
    { CUE_GAME_PYRAMID7,    "pyramid 7ft",    0 },
    { CUE_GAME_BARBILLIARDS,"bar billiards",  0 },
    { CUE_GAME_GOLF,        "golf",           0 },
};
#define NG ((int)(sizeof G / sizeof G[0]))

int main(void) {
    printf("the spec a standard table is cut to\n\n");

    /* ---- which games have specs at all --------------------------------- */
    for (int i = 0; i < NG; i++) {
        char b[96];
        snprintf(b, sizeof b, "%-14s %s specs", G[i].name,
                 G[i].specs ? "has" : "has no");
        ok(cue_table_spec_applies(G[i].k) == G[i].specs, b);
    }
    printf("\n");

    /* ---- PRO KEEPS THE SHIPPED POCKETS ---------------------------------- *
     *
     * Pro IS the table the game shipped with, because the shipped pockets were
     * already the hard end of anything worth offering — the ladder goes UP from
     * there. So its openings must be the shipped openings to the micron. Its
     * CLOTH is faster, which is the half of a professional table a pocket size
     * cannot express, so the structs are not identical and it is the pockets
     * that are compared. */
    for (int i = 0; i < NG; i++) {
        CueTable a, b;
        cue_table_init(&a, G[i].k);
        cue_table_init(&b, G[i].k);
        cue_table_spec(&b, CUE_SPEC_PRO);
        float ac = 0, am = 0, bc = 0, bm = 0;
        cue_table_openings(&a, &ac, &am);
        cue_table_openings(&b, &bc, &bm);
        char m[128];
        snprintf(m, sizeof m, "%-14s PRO keeps the shipped pockets (%.2f / %.2f)",
                 G[i].name, (double)(bc*1000), (double)(bm*1000));
        ok(fabsf(ac - bc) < 1e-6f && fabsf(am - bm) < 1e-6f, m);
        /* And nothing about the BED moved with them. */
        snprintf(m, sizeof m, "%-14s ...and the same bed and balls", G[i].name);
        ok(a.half_len == b.half_len && a.half_wid == b.half_wid && a.R == b.R, m);
    }
    printf("\n");

    /* THE DEFAULT IS TOURNAMENT, and deliberately not index 0 — which is the
     * one thing about this ladder a reader will assume is a mistake. Every
     * other default in the engine is a zero. */
    ok(CUE_SPEC_DEFAULT == CUE_SPEC_TOURNAMENT, "the default spec is TOURNAMENT");
    ok(CUE_TAB_DEFAULT  == CUE_TAB_TOURNAMENT,  "...and so is the default table");
    ok(CUE_SPEC_PRO == 0 && CUE_TAB_PRO == 0,
       "...while index 0 is PRO, the tightest");

    /* A spec off the end of the list must change nothing: a saved preference
     * from a build with a different list must not silently become a different
     * table. */
    {   CueTable a, b; cue_table_init(&a, CUE_GAME_SNK15);
        b = a; cue_table_spec(&b, 99);
        ok(memcmp(&a, &b, sizeof a) == 0, "a spec off the end of the list changes nothing");
        b = a; cue_table_spec(&b, -3);
        ok(memcmp(&a, &b, sizeof a) == 0, "...and so does a negative one");
    }
    printf("\n");

    /* ---- the ordering, at both pocket types, on every table ------------ */
    for (int i = 0; i < NG; i++) {
        if (!G[i].specs) continue;
        float c[CUE_SPEC_COUNT], m[CUE_SPEC_COUNT], mur[CUE_SPEC_COUNT];
        for (int sp = 0; sp < CUE_SPEC_COUNT; sp++) {
            CueTable t; cue_table_init(&t, G[i].k);
            cue_table_spec(&t, sp);
            cue_table_openings(&t, &c[sp], &m[sp]);
            mur[sp] = (t.mu_r > 0.0f) ? t.mu_r : 0.010f;
        }
        char b[160];
        snprintf(b, sizeof b,
                 "%-14s corner  pro %.1f < tourn %.1f < club %.1f mm",
                 G[i].name, (double)(c[CUE_SPEC_PRO]*1000),
                 (double)(c[CUE_SPEC_TOURNAMENT]*1000),
                 (double)(c[CUE_SPEC_CLUB]*1000));
        (void)0;
        ok(c[CUE_SPEC_PRO] < c[CUE_SPEC_TOURNAMENT] &&
           c[CUE_SPEC_TOURNAMENT] < c[CUE_SPEC_CLUB], b);
        if (m[CUE_SPEC_TOURNAMENT] > 0.0f) {
            snprintf(b, sizeof b,
                     "%-14s middle  pro %.1f < tourn %.1f < club %.1f mm",
                     G[i].name, (double)(m[CUE_SPEC_PRO]*1000),
                     (double)(m[CUE_SPEC_TOURNAMENT]*1000),
                     (double)(m[CUE_SPEC_CLUB]*1000));
            ok(m[CUE_SPEC_PRO] < m[CUE_SPEC_TOURNAMENT] &&
               m[CUE_SPEC_TOURNAMENT] < m[CUE_SPEC_CLUB], b);
        }
        /* And the cloth runs the other way: a pro table is faster, which is a
         * LOWER rolling resistance. */
        snprintf(b, sizeof b,
                 "%-14s cloth   pro %.4f < tourn %.4f < club %.4f",
                 G[i].name, (double)mur[CUE_SPEC_PRO],
                 (double)mur[CUE_SPEC_TOURNAMENT], (double)mur[CUE_SPEC_CLUB]);
        ok(mur[CUE_SPEC_PRO] < mur[CUE_SPEC_TOURNAMENT] &&
           mur[CUE_SPEC_TOURNAMENT] < mur[CUE_SPEC_CLUB], b);
    }
    printf("\n");

    /* ---- and every one of them is playable ----------------------------- */
    for (int i = 0; i < NG; i++) {
        for (int sp = 0; sp < CUE_SPEC_COUNT; sp++) {
            CueTable t; cue_table_init(&t, G[i].k);
            cue_table_spec(&t, sp);
            char why[256] = {0};
            char b[420];
            const int good = cue_table_validate(&t, why, sizeof why);
            snprintf(b, sizeof b, "%-14s %-11s validates%s%s", G[i].name,
                     CUE_SPEC_NAME[sp], good ? "" : ": ", good ? "" : why);
            ok(good, b);
        }
    }
    printf("\n");

    /* ---- the solve lands where it was asked to ------------------------- */
    {   const float want_c[] = { 0.075f, 0.084f, 0.095f, 0.110f };
        for (unsigned i = 0; i < sizeof want_c / sizeof want_c[0]; i++) {
            CueTable t; cue_table_init(&t, CUE_GAME_SNK15);
            const int landed = cue_table_cut_to(&t, want_c[i], 0.0f);
            float c = 0, m = 0; cue_table_openings(&t, &c, &m);
            char b[160];
            snprintf(b, sizeof b, "cut to %.1f mm -> %.2f mm (%.2f off)",
                     (double)(want_c[i]*1000), (double)(c*1000),
                     (double)((c - want_c[i])*1000));
            ok(landed && fabsf(c - want_c[i]) < 0.0001f, b);
            /* The MIDDLE must not have moved: they solve on separate fields. */
            CueTable ref; cue_table_init(&ref, CUE_GAME_SNK15);
            float rc = 0, rm = 0; cue_table_openings(&ref, &rc, &rm);
            snprintf(b, sizeof b, "...and the middle stayed at %.2f mm",
                     (double)(rm*1000));
            ok(fabsf(m - rm) < 0.0001f, b);
        }
        /* A target nothing could give must SAY so rather than quietly land
         * somewhere else — and it must not get there by leaving the range a
         * pocket radius is allowed to be. 300 mm is nearly six ball widths and
         * would want a radius past the validator's own ceiling, so the honest
         * answer is "no".
         *
         * 220 mm used to be the figure here and it was the wrong test: three
         * passes of a purely RELATIVE range compound, so the radius walked
         * 45 -> 100 -> 137 mm and got there. What that caught was the solver
         * being able to hand back a table the validator would refuse, which is
         * now bounded — see cut_one. */
        CueTable t; cue_table_init(&t, CUE_GAME_SNK15);
        ok(cue_table_cut_to(&t, 0.300f, 0.0f) == 0,
           "an impossible opening is refused rather than approximated");
        char why[256];
        ok(cue_table_validate(&t, why, sizeof why),
           "...and the table it gave up on is still a legal one");
    }
    printf("\n");

    /* ---- the cloth reaches the physics -------------------------------- */
    {   static CueWorld w;
        CueTable t; cue_table_init(&t, CUE_GAME_SNK15);
        cue_table_build_world(&t, &w);
        const float dflt = w.mu_r;
        ok(dflt > 0.0f, "a table with no cloth speed gets the engine's own");
        t.mu_r = 0.0135f;
        cue_table_build_world(&t, &w);
        ok(fabsf(w.mu_r - 0.0135f) < 1e-7f, "...and one that names a speed gets it");
        t.mu_r = 0.0f;
        cue_table_build_world(&t, &w);
        ok(fabsf(w.mu_r - dflt) < 1e-7f, "...and zero means the default, not a skating rink");
    }
    printf("\n");

    /* ---- and survives the wire, and is in the hash --------------------- */
    {   CueTable a; cue_table_init(&a, CUE_GAME_SNK15);
        cue_table_spec(&a, CUE_SPEC_CLUB);
        unsigned char buf[CUE_TABLE_SPEC_MAX];
        const int n = cue_table_pack(&a, buf, sizeof buf);
        ok(n > 0, "a club table packs");
        CueTable b;
        ok(cue_table_unpack(&b, buf, n) == 1, "...and unpacks");
        ok(memcmp(&a, &b, sizeof a) == 0, "...bit for bit, cloth speed and all");

        CueTable c; cue_table_init(&c, CUE_GAME_SNK15);
        CueTable d = c; d.mu_r = 0.020f;
        ok(cue_table_hash(&c) != cue_table_hash(&d),
           "a different cloth speed is a different table to the handshake");

        /* THE THREE SPECS MUST BE THREE DIFFERENT TABLES on the wire, or two
         * players who picked differently would be told they agree. */
        uint32_t h[CUE_SPEC_COUNT];
        for (int sp = 0; sp < CUE_SPEC_COUNT; sp++) {
            CueTable t; cue_table_init(&t, CUE_GAME_SNK15);
            cue_table_spec(&t, sp);
            h[sp] = cue_table_hash(&t);
        }
        ok(h[0] != h[1] && h[1] != h[2] && h[0] != h[2],
           "...and the three specs hash three different ways");
    }

    printf("\n");

    /* ---- AND THE SHAPES ------------------------------------------------- *
     *
     * The other four answers to "which table". These are for fun and they are
     * still tables: a frame has to be rackable on one, every ball has to start
     * on the cloth, and the pockets have to be the pockets of the game it says
     * it is. The last one is the whole reason cue_table_cut_to exists — a bed
     * that gains sides gains pocket, because the mouth that falls out of a
     * pull-back depends on the angle it is pulled back from, and a 9 ft
     * American table measured 1.94 ball widths as a rectangle and 2.20 as a
     * round one. A quarter of a ball is the difference between a pot and a
     * gift.
     *
     * WHY EVERY BALL, AND NOT JUST THE VALIDATOR. The validator checks the
     * table; it does not rack it. A hexagon narrows away from its widest point,
     * so a rack laid out against a rectangle's spine can be perfectly legal
     * geometry with the black standing in the timber. Ask the bed. */
    {   static CueWorld w; static CueBall b[CUE_MAX_BALLS];
        for (int i = 0; i < NG; i++) {
            for (int v = CUE_TAB_L; v < CUE_TAB_COUNT; v++) {
                char m[200];
                if (!cue_table_variant_ok(G[i].k, v)) continue;
                CueTable t; cue_table_init(&t, G[i].k);
                float base_c = 0.0f, base_m = 0.0f;
                cue_table_openings(&t, &base_c, &base_m);
                cue_table_variant(&t, v);

                char why[256] = {0};
                snprintf(m, sizeof m, "%-14s %-9s validates%s%s", G[i].name,
                         CUE_TAB_NAME[v], cue_table_validate(&t, why, sizeof why) ? "" : ": ",
                         cue_table_validate(&t, why, sizeof why) ? "" : why);
                ok(cue_table_validate(&t, why, sizeof why), m);

                cue_table_build_world(&t, &w);
                const int n = cue_table_rack(&t, b);
                int off = 0;
                for (int j = 0; j < n; j++)
                    if (b[j].on && !cue_world_on_bed(&w, b[j].pos.x, b[j].pos.z)) off++;
                snprintf(m, sizeof m, "%-14s %-9s racks %d balls, %d off the cloth",
                         G[i].name, CUE_TAB_NAME[v], n, off);
                ok(n >= 2 && off == 0, m);

                /* And the pockets are the game's own, measured. A regular bed
                 * has no middles at all, so there is nothing to compare. */
                float c = 0.0f, mm2 = 0.0f;
                cue_table_openings(&t, &c, &mm2);
                snprintf(m, sizeof m, "%-14s %-9s corner %.1f mm vs %.1f on the flat",
                         G[i].name, CUE_TAB_NAME[v], (double)(c*1000), (double)(base_c*1000));
                ok(base_c <= 0.0f || fabsf(c - base_c) < 0.0005f, m);
                if (mm2 > 0.0f && base_m > 0.0f) {
                    snprintf(m, sizeof m, "%-14s %-9s middle %.1f mm vs %.1f",
                             G[i].name, CUE_TAB_NAME[v], (double)(mm2*1000),
                             (double)(base_m*1000));
                    ok(fabsf(mm2 - base_m) < 0.0005f, m);
                }
            }
        }
    }
    printf("\n");

    /* ...and the games that must NOT be offered a shape, because their whole
     * layout is a fixed set of coordinates on a rectangle. */
    {   const CueGameKind FIXED[] = { CUE_GAME_BARBILLIARDS, CUE_GAME_GOLF,
                                      CUE_GAME_BILLIARDS };
        const char *FN[] = { "bar billiards", "golf", "billiards" };
        for (int i = 0; i < 3; i++) {
            char m[96];
            int any = 0;
            for (int v = CUE_TAB_L; v < CUE_TAB_COUNT; v++)
                if (cue_table_variant_ok(FIXED[i], v)) any = 1;
            snprintf(m, sizeof m, "%-14s is offered no shapes", FN[i]);
            ok(!any, m);
            /* And asking anyway changes nothing. */
            CueTable a, b2; cue_table_init(&a, FIXED[i]); b2 = a;
            cue_table_variant(&b2, CUE_TAB_ROUND);
            snprintf(m, sizeof m, "%-14s ...and asking for one is a no-op", FN[i]);
            ok(memcmp(&a, &b2, sizeof a) == 0, m);
        }
    }

    printf("\n");

    /* ---- A GAME ON SOMEBODY ELSE'S TABLE -------------------------------- *
     *
     * What makes a saved table a table rather than a game. A bed is geometry;
     * how many reds go in the triangle and whether there is a D to play from
     * belong to the game somebody picked. So every crossing has to come out
     * playable, and the geometry has to survive it untouched — a bed that
     * quietly resized itself when you chose a different game would be the
     * saved table not being saved. */
    {   static CueBall bb[CUE_MAX_BALLS];
        const struct { CueGameKind from, to; const char *fn, *tn; } X[] = {
            { CUE_GAME_US8,     CUE_GAME_SNK15,   "US 8-ball 9ft", "snooker 15" },
            { CUE_GAME_US8,     CUE_GAME_SNK6,    "US 8-ball 9ft", "snooker 6"  },
            { CUE_GAME_SNK15,   CUE_GAME_US9,     "snooker 12ft",  "9-ball"     },
            { CUE_GAME_SNK15,   CUE_GAME_US10,    "snooker 12ft",  "10-ball"    },
            { CUE_GAME_SNK15,   CUE_GAME_SNK6,    "snooker 12ft",  "snooker 6"  },
            { CUE_GAME_SNK15,   CUE_GAME_STRAIGHT,"snooker 12ft",  "straight"   },
            { CUE_GAME_UK8,     CUE_GAME_PYRAMID, "UK 8-ball 7ft", "pyramid"    },
            { CUE_GAME_PYRAMID, CUE_GAME_SNK15,   "pyramid 12ft",  "snooker 15" },
            { CUE_GAME_CN8,     CUE_GAME_UK8,     "chinese 10ft",  "UK 8-ball"  },
        };
        for (unsigned i = 0; i < sizeof X / sizeof X[0]; i++) {
            CueTable t; cue_table_init(&t, X[i].from);
            const float hl = t.half_len, hw = t.half_wid;
            const float prc = t.pr_corner, prs = t.pr_side, R = t.R;
            CueTable want; cue_table_init(&want, X[i].to);

            cue_table_set_game(&t, X[i].to);

            char m[160], why[200] = {0};
            snprintf(m, sizeof m, "%-14s played as %-11s validates",
                     X[i].fn, X[i].tn);
            ok(cue_table_validate(&t, why, sizeof why), m);

            snprintf(m, sizeof m, "%-14s ...keeps its bed and its pockets",
                     X[i].fn);
            ok(t.half_len == hl && t.half_wid == hw && t.R == R &&
               t.pr_corner == prc && t.pr_side == prs, m);

            snprintf(m, sizeof m, "%-14s ...racks %s's set: %d reds, %d balls",
                     X[i].fn, X[i].tn, t.reds, t.nballs);
            ok(t.reds == want.reds && t.nballs == want.nballs &&
               t.is_snooker == want.is_snooker, m);

            const int n = cue_table_rack(&t, bb);
            snprintf(m, sizeof m, "%-14s ...and the rack lays out %d of them",
                     X[i].fn, n);
            ok(n == want.nballs, m);

            /* A snooker frame is played from the D and a pool frame is not, so
             * one has to exist and the other has to not. */
            snprintf(m, sizeof m, "%-14s ...with %s", X[i].fn,
                     want.is_snooker ? "a D to play from" : "no D");
            ok(want.is_snooker ? (t.d_radius > 0.0f)
                               : (want.d_radius > 0.0f || t.d_radius == 0.0f), m);
        }

        /* A TABLE ALREADY LAID OUT FOR THE GAME IS LEFT ALONE. Somebody who
         * dialled a D and four spots by hand must not have them replaced with
         * the defaults the moment the game is re-asked. */
        CueTable a; cue_table_init(&a, CUE_GAME_SNK15);
        a.d_radius *= 0.8f;
        a.pink_x   *= 0.9f;
        CueTable b3 = a;
        cue_table_set_game(&b3, CUE_GAME_SNK15);
        ok(b3.d_radius == a.d_radius && b3.pink_x == a.pink_x,
           "a table already laid out for the game keeps its own D and spots");
    }

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}
