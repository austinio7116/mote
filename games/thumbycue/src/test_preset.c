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

    /* ---- TOURNAMENT is the shipped table, to the bit ------------------- */
    for (int i = 0; i < NG; i++) {
        CueTable a, b;
        cue_table_init(&a, G[i].k);
        cue_table_init(&b, G[i].k);
        cue_table_spec(&b, CUE_SPEC_TOURNAMENT);
        char m[96];
        snprintf(m, sizeof m, "%-14s TOURNAMENT is what ships, byte for byte",
                 G[i].name);
        ok(memcmp(&a, &b, sizeof a) == 0, m);
    }
    printf("\n");

    /* ...and so is a spec index out of range, or a negative one: a saved
     * preference from a build with a different list must not silently become a
     * different table. */
    {   CueTable a, b; cue_table_init(&a, CUE_GAME_SNK15);
        b = a; cue_table_spec(&b, 99);
        ok(memcmp(&a, &b, sizeof a) == 0, "a spec off the end of the list is the shipped table");
        b = a; cue_table_spec(&b, -3);
        ok(memcmp(&a, &b, sizeof a) == 0, "...and so is a negative one");
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
         * somewhere else. Twice the ball, on a snooker corner, is not a pocket
         * this bed can be cut to. */
        CueTable t; cue_table_init(&t, CUE_GAME_SNK15);
        ok(cue_table_cut_to(&t, 0.220f, 0.0f) == 0,
           "an impossible opening is refused rather than approximated");
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

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}
