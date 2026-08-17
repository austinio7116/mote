/*
 * ThumbyCue — the table as a value.
 *
 * A table stopped being a menu entry and became a set of numbers you can save,
 * send and refuse. Four operations came with that, and each one has a way of
 * being quietly wrong that this asks about directly:
 *
 *   PACK/UNPACK must be lossless. Not "close enough": lockstep means two
 *   machines integrate at 2 kHz from the same geometry, so a table that arrives
 *   with one cushion a float's-last-bit different is a table that diverges. The
 *   round trip is compared BIT for bit, not to a tolerance.
 *
 *   THE HASH must ignore what does not matter and catch what does. It is the
 *   room handshake: too fussy and two players who picked different cloth cannot
 *   play each other; too loose and they rack different tables and neither is
 *   told. So both directions are asked — a colour must not move it, and every
 *   playing number must.
 *
 *   THE VALIDATOR is the product, not paperwork. Every number in here is
 *   reachable from a tuning screen, and a pocket narrower than a ball is one
 *   thumbstick movement away. It is asked about the combinations as well as the
 *   ranges, because the interesting failures are pairs of individually sensible
 *   numbers.
 *
 *   AND THE SHIPPED TABLES must all pass it, which is the check that stops the
 *   ranges being invented rather than measured: if a range excludes a table the
 *   game already ships, the range is wrong.
 */
#include "cue_table.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int fails;
static void ck(int cond, const char *what) {
    if (!cond) { printf("  FAIL  %s\n", what); fails++; }
}

/* A table the game actually ships, as the starting point for each case. */
static CueTable good(void) {
    CueTable t; cue_table_init(&t, CUE_GAME_SNK15); return t;
}

/* The validator should reject this, and say why in the player's terms. */
static void rejects(CueTable t, const char *label) {
    char msg[128];
    int ok = cue_table_validate(&t, msg, sizeof msg);
    if (ok) { printf("  FAIL  accepted %s\n", label); fails++; }
    else    { printf("        %-34s -> \"%s\"\n", label, msg); }
}

int main(void) {
    printf("ThumbyCue table spec\n\n");

    /* ---- every shipped table is playable ------------------------------- */
    for (int k = 0; k < CUE_GAME_COUNT; k++) {
        CueTable t; cue_table_init(&t, (CueGameKind)k);
        char msg[128];
        int ok = cue_table_validate(&t, msg, sizeof msg);
        if (!ok) { printf("  FAIL  shipped kind %d rejected: %s\n", k, msg); fails++; }
    }
    printf("all %d shipped tables validate\n", (int)CUE_GAME_COUNT);

    /* ---- pack / unpack is lossless ------------------------------------- */
    unsigned char buf[CUE_TABLE_SPEC_MAX];
    int worst = 0;
    for (int k = 0; k < CUE_GAME_COUNT; k++) {
        CueTable a; cue_table_init(&a, (CueGameKind)k);
        int n = cue_table_pack(&a, buf, sizeof buf);
        ck(n > 0, "pack fits CUE_TABLE_SPEC_MAX");
        if (n > worst) worst = n;

        CueTable b;
        ck(cue_table_unpack(&b, buf, n) == 1, "unpack accepts what pack wrote");

        /* Bit for bit over every described field, via the hash AND a direct
         * compare of the numbers the hash deliberately ignores. */
        ck(cue_table_hash(&a) == cue_table_hash(&b), "round trip preserves the hash");
        ck(a.cloth == b.cloth && a.rail == b.rail &&
           a.rail_top == b.rail_top && a.spot == b.spot,
           "round trip preserves the colours too");
        ck(a.half_len == b.half_len && a.R == b.R && a.pr_corner == b.pr_corner &&
           a.e_cush == b.e_cush && a.black_x == b.black_x,
           "round trip preserves the geometry exactly");
    }
    printf("pack/unpack lossless on all kinds, worst case %d bytes of %d\n",
           worst, CUE_TABLE_SPEC_MAX);

    /* ---- the hash ignores looks, and nothing else ----------------------- */
    {
        CueTable a = good(), b = good();
        b.cloth = 0x1234; b.rail = 0x5678; b.rail_top = 0x9abc; b.spot = 0xdef0;
        ck(cue_table_hash(&a) == cue_table_hash(&b),
           "a recover in a different cloth is the same table");

        /* The description knows how many fields there are, and four of them are
         * the colours. If that ever stops being true, somebody has added a field
         * and this test has not been told which side of the line it belongs on —
         * which is precisely the mistake the single description exists to
         * prevent, so it should fail loudly here rather than pass quietly. */
        /* 43 before F2; the three the bed's shape added — bed_shape, notch_x
         * and notch_z — are all SIM, because the outline of the cloth is the
         * wall a ball bounces off and two ends that disagree about it are
         * playing different tables. bed_hand, which way the L turns, is the
         * fourth and is SIM for exactly the same reason. */
        ck(cue_table_field_count() == 49,
           "field count unchanged (add a field -> decide if it is SIM, then update this)");

        /* And the playing numbers, one per kind of thing a table can be: its
         * size, its balls, its pockets, its cushions, its timber, its drop. */
        CueTable c;
        c = good(); c.half_len   += 0.001f; ck(cue_table_hash(&a) != cue_table_hash(&c), "bed length moves the hash");
        c = good(); c.R          += 0.0001f; ck(cue_table_hash(&a) != cue_table_hash(&c), "ball size moves the hash");
        c = good(); c.pr_corner  += 0.0001f; ck(cue_table_hash(&a) != cue_table_hash(&c), "pocket size moves the hash");
        c = good(); c.e_cush     += 0.001f; ck(cue_table_hash(&a) != cue_table_hash(&c), "cushion bounce moves the hash");
        c = good(); c.bore_side  += 0.0001f; ck(cue_table_hash(&a) != cue_table_hash(&c), "bore moves the hash");
        c = good(); c.drop_back  += 0.0001f; ck(cue_table_hash(&a) != cue_table_hash(&c), "drop depth moves the hash");
    }
    printf("hash ignores colour, tracks every playing number\n");

    /* ---- what the validator refuses ------------------------------------ */
    /* Each of these stays INSIDE every per-field range, so it is the
     * cross-field check that has to catch it — which is the half of the
     * validator that a range table cannot express. The two at the end are
     * range failures, kept to show what those read like. */
    printf("\nrefused, and the reason given:\n");
    { CueTable t = good(); t.pr_corner = t.R * 0.9f;       rejects(t, "pocket narrower than the ball"); }
    { CueTable t = good(); t.cap_corner = t.pr_corner*0.9f;rejects(t, "drop shrunk to nothing"); }
    { CueTable t = good(); t.e_cush = 0.60f;
                           t.e_cush_min = 0.80f;           rejects(t, "cushion livelier under pace"); }
    { CueTable t = good(); t.half_len = 0.90f;
                           t.half_wid = 1.00f;             rejects(t, "wider than it is long"); }
    { CueTable t = good(); t.bore_side = t.pr_side*0.5f;   rejects(t, "bore too small for its mouth"); }
    { CueTable t = good(); t.cushion_h = 0.055f;           rejects(t, "cushion above the ball's centre"); }
    { CueTable t = good(); t.pink_x = t.baulk_x - 0.1f;    rejects(t, "spots out of order"); }
    /* The D is checked against the bed, so it needs a table narrow enough for
     * an in-range radius to overrun it: the 7 ft snooker bed. */
    { CueTable t; cue_table_init(&t, CUE_GAME_SNK6);
                           t.d_radius = t.half_wid + 0.02f;rejects(t, "the D wider than the table"); }
    { CueTable t = good(); t.R = 0.0f;                     rejects(t, "no ball at all (a range)"); }
    { CueTable t = good(); t.half_len = NAN;               rejects(t, "a NaN in the geometry (a range)"); }

    /* ---- and what unpack refuses --------------------------------------- */
    {
        CueTable a = good(), b;
        int n = cue_table_pack(&a, buf, sizeof buf);
        ck(cue_table_unpack(&b, buf, n - 1) == 0, "unpack refuses a short block");
        buf[0] = CUE_TABLE_SPEC_VERSION + 1;
        ck(cue_table_unpack(&b, buf, n) == 0, "unpack refuses a future version");
        buf[0] = CUE_TABLE_SPEC_VERSION;
        buf[1] = (unsigned char)(buf[1] + 1);
        ck(cue_table_unpack(&b, buf, n) == 0, "unpack refuses a different field count");

        /* A block that is well formed but describes an unplayable table must be
         * refused HERE, not accepted and discovered later. */
        CueTable bad = good(); bad.pr_corner = bad.R * 0.5f;
        n = cue_table_pack(&bad, buf, sizeof buf);
        ck(n > 0, "an unplayable table still packs");
        ck(cue_table_unpack(&b, buf, n) == 0, "...and is refused on the way back in");
    }
    printf("\nunpack refuses short, versioned, reshaped and unplayable blocks\n");

    printf("\n%s\n", fails ? "FAILED" : "PASSED");
    return fails ? 1 : 0;
}
