/*
 * ThumbyCue — ball in hand must land somewhere legal.
 *
 * Reported: the cue ball placed in the D "often seems to place over a baulk
 * colour and it forces it outside the colour, position forward of the D". It
 * did, and the CPU did it too, because cue_ai_place asks the same function.
 *
 * The clamp alternated "back into the region" with "out of the balls" and
 * returned after a PUSH, so whenever something was in the way the last thing
 * that happened to the position was being shoved off it — over the baulk line
 * and out of the D. The region is a rule and the separation is a courtesy; the
 * loop had that the wrong way round.
 *
 * The three baulk colours sit ON the D's line, which is why this is not an
 * exotic case: green, brown and yellow are exactly where a player asks for the
 * cue ball, every frame.
 */
#include "cue_table.h"
#include "cue_physics.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int s_fail;
static void ok(int cond, const char *what, const char *detail) {
    if (!cond) { s_fail++; printf("  FAIL %s%s%s\n", what, detail ? " — " : "", detail ? detail : ""); }
    else printf("  ok   %s\n", what);
}

/* The D, as the rules draw it: a half-disc of radius d_radius centred on
 * (baulk_x, 0) bulging toward the baulk cushion. A ball is in hand legally if
 * its CENTRE is inside. */
static int in_the_d(const CueTable *t, Vec3 p) {
    float dx = p.x - t->baulk_x, dz = p.z;
    if (dx > 1e-4f) return 0;                       /* forward of the line */
    return dx * dx + dz * dz <= (t->d_radius + 1e-4f) * (t->d_radius + 1e-4f);
}

static int clear_of_all(const CueTable *t, Vec3 p, const CueBall *b, int n) {
    float sep = 2.0f * t->R - 1e-4f;
    for (int i = 1; i < n; i++) {
        if (!b[i].on) continue;
        float dx = p.x - b[i].pos.x, dz = p.z - b[i].pos.z;
        if (dx * dx + dz * dz < sep * sep) return 0;
    }
    return 1;
}

int main(void) {
    CueTable t;
    CueBall b[22];
    cue_table_init(&t, CUE_GAME_SNK15);
    int n = cue_table_rack(&t, b);

    printf("ball in hand\n");

    /* Ask for the cue ball exactly where each baulk colour is standing. That is
     * what a player does — the green, brown and yellow spots ARE the places you
     * reach for when putting the white in the D. */
    const struct { int id; const char *name; } BAULK[] = {
        { CUE_ID_GREEN,  "green"  },
        { CUE_ID_BROWN,  "brown"  },
        { CUE_ID_YELLOW, "yellow" },
    };
    for (unsigned k = 0; k < sizeof BAULK / sizeof BAULK[0]; k++) {
        Vec3 on_it = { 0, t.R, 0 };
        for (int i = 1; i < n; i++)
            if (b[i].id == BAULK[k].id && b[i].on) on_it = b[i].pos;
        Vec3 got = cue_table_clamp_placement_balls(&t, on_it, b, n, 0);
        char d[128];
        snprintf(d, sizeof d, "asked on the %s (%.3f,%.3f) -> (%.3f,%.3f), "
                 "baulk line x=%.3f", BAULK[k].name, (double)on_it.x,
                 (double)on_it.z, (double)got.x, (double)got.z, (double)t.baulk_x);
        char w[64];
        snprintf(w, sizeof w, "on the %s, it lands IN the D", BAULK[k].name);
        ok(in_the_d(&t, got), w, d);
        snprintf(w, sizeof w, "on the %s, it lands clear of it", BAULK[k].name);
        ok(clear_of_all(&t, got, b, n), w, d);
    }

    /* And the nastiest one there is: dead on the brown, with green and yellow
     * either side of it, which is the whole width of the D occupied. */
    {
        Vec3 brown = { 0, t.R, 0 };
        for (int i = 1; i < n; i++) if (b[i].id == CUE_ID_BROWN) brown = b[i].pos;
        Vec3 got = cue_table_clamp_placement_balls(&t, brown, b, n, 0);
        char d[96];
        snprintf(d, sizeof d, "-> (%.3f,%.3f)", (double)got.x, (double)got.z);
        ok(in_the_d(&t, got) && clear_of_all(&t, got, b, n),
           "the three baulk colours together still leave room", d);
    }

    /* Somewhere miles away is still brought into the D. */
    {
        Vec3 far = { t.half_len * 0.8f, t.R, t.half_wid * 0.8f };
        Vec3 got = cue_table_clamp_placement_balls(&t, far, b, n, 0);
        char d[96];
        snprintf(d, sizeof d, "-> (%.3f,%.3f)", (double)got.x, (double)got.z);
        ok(in_the_d(&t, got), "a spot up the table is brought back into the D", d);
    }

    /* An empty table must not move a legal request at all. */
    {
        CueBall none[1];
        memset(none, 0, sizeof none);
        Vec3 want = { t.baulk_x - t.d_radius * 0.4f, t.R, t.d_radius * 0.3f };
        Vec3 got = cue_table_clamp_placement_balls(&t, want, none, 1, 0);
        char d[96];
        snprintf(d, sizeof d, "(%.4f,%.4f) -> (%.4f,%.4f)", (double)want.x,
                 (double)want.z, (double)got.x, (double)got.z);
        ok(fabsf(got.x - want.x) < 1e-4f && fabsf(got.z - want.z) < 1e-4f,
           "a legal spot on an empty D is left exactly alone", d);
    }

    /* ---- THE HOUSE IS NOT A D ------------------------------------------
     *
     * Russian pyramid plays from the дом: everything behind the line, the
     * whole width of the table. It arrived carrying that as a D of radius
     * d_radius on the grounds that a house is a D as far as a clamp is
     * concerned — which confined the cue ball to a semicircle a third of the
     * area, and chalked one on the cloth to match. */
    for (int k = 0; k < 2; k++) {
        CueTable h;
        cue_table_init(&h, k ? CUE_GAME_PYRAMID7 : CUE_GAME_PYRAMID);
        CueBall none[1]; memset(none, 0, sizeof none);
        const char *who = k ? "pyramid 7ft" : "pyramid 12ft";
        char d[128];

        ok(h.house, "the pyramid table says it has a house", who);

        /* Hard against the side cushion, right on the baulk line: inside a
         * house, a long way outside any D. */
        {   Vec3 want = { h.baulk_x - 0.01f, h.R, h.half_wid - h.R };
            Vec3 got = cue_table_clamp_placement_balls(&h, want, none, 1, 0);
            snprintf(d, sizeof d, "%s: (%.3f,%.3f) -> (%.3f,%.3f)", who,
                     (double)want.x, (double)want.z, (double)got.x, (double)got.z);
            ok(fabsf(got.x - want.x) < 1e-3f && fabsf(got.z - want.z) < 1e-3f,
               "against the cushion behind the line is left alone", d); }

        /* ...and the corner of the house, hard back on the baulk cushion. */
        {   Vec3 want = { -h.half_len + h.R, h.R, -(h.half_wid - h.R) };
            Vec3 got = cue_table_clamp_placement_balls(&h, want, none, 1, 0);
            snprintf(d, sizeof d, "%s: (%.3f,%.3f) -> (%.3f,%.3f)", who,
                     (double)want.x, (double)want.z, (double)got.x, (double)got.z);
            ok(fabsf(got.x - want.x) < 1e-3f && fabsf(got.z - want.z) < 1e-3f,
               "the back corner of the house is left alone", d); }

        /* Past the line is still past the line. */
        {   Vec3 want = { h.baulk_x + 0.30f, h.R, 0.0f };
            Vec3 got = cue_table_clamp_placement_balls(&h, want, none, 1, 0);
            snprintf(d, sizeof d, "%s: x %.3f -> %.3f (line %.3f)", who,
                     (double)want.x, (double)got.x, (double)h.baulk_x);
            ok(got.x <= h.baulk_x + 1e-3f,
               "up the table is brought back behind the line", d); }

        /* ...and so is through a cushion. */
        {   Vec3 want = { h.baulk_x - 0.05f, h.R, h.half_wid + 0.30f };
            Vec3 got = cue_table_clamp_placement_balls(&h, want, none, 1, 0);
            snprintf(d, sizeof d, "%s: z %.3f -> %.3f (half %.3f)", who,
                     (double)want.z, (double)got.z, (double)h.half_wid);
            ok(got.z <= h.half_wid - h.R + 1e-3f,
               "through the side cushion is brought back onto the cloth", d); }
    }

    /* And a snooker table still has a D, which is the whole point of the
     * flag: the shape is a property of the table, not of the clamp. */
    {   CueTable sn; cue_table_init(&sn, CUE_GAME_SNK15);
        CueBall none[1]; memset(none, 0, sizeof none);
        ok(!sn.house, "snooker has no house", "");
        Vec3 want = { sn.baulk_x - 0.01f, sn.R, sn.half_wid - sn.R };
        Vec3 got = cue_table_clamp_placement_balls(&sn, want, none, 1, 0);
        char d[96];
        snprintf(d, sizeof d, "z %.3f -> %.3f (D radius %.3f)",
                 (double)want.z, (double)got.z, (double)sn.d_radius);
        ok(in_the_d(&sn, got), "against the cushion is pulled into the D", d); }

    printf(s_fail ? "\nFAILED (%d)\n" : "\nPASSED\n", s_fail);
    return s_fail ? 1 : 0;
}
