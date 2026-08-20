/* G2 — Russian pyramid.
 *
 * Two things make it a game rather than pool with the groups switched off, and
 * both are what this asks about:
 *
 *   THE POCKETS. 73 mm to a 68 mm ball, so a pot has about two millimetres to
 *   spare each side. Every other table here is between 1.8 and 2.0 ball radii at
 *   the mouth; this one is 1.075, and it is the reason the validator checks a
 *   mouth against the ball at all. If it can be racked but nothing can ever be
 *   potted, it is a diagram and not a table — so this plays real shots into real
 *   pockets and counts.
 *
 *   THE PENALTY. A foul puts one of your OWN potted balls back, so a frame can
 *   go backwards. That is not a rule any other game here has and it is the one
 *   most likely to be got wrong in a way that only shows up after ten minutes of
 *   play, which is exactly what a test is for.
 */
#include "cue_physics.h"
#include "cue_table.h"
#include "cue_rules.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;
static void ok(int c, const char *m){printf("%-4s %s\n",c?"ok":"FAIL",m); if(!c)fails++;}

/* Roll a ball at every pocket from twenty-four directions and count what drops.
 * The starts that are off the cloth are simply not shots — most of the compass
 * round a corner pocket is the room — and eight directions left ten playable
 * starts across a whole table, which is a sample rather than a measurement. */
static void pot_sweep(const CueTable *t, CueWorld *w, int *tried, int *in) {
    *tried = 0; *in = 0;
    for (int p = 0; p < w->npocket; p++) {
        for (int a = 0; a < 24; a++) {
            CueBall q; memset(&q, 0, sizeof q);
            float ang = 6.2831853f * (float)a / 24.0f;
            float px = w->pocket[p].x, pz = w->pocket[p].z;
            float sx = px - cosf(ang) * 0.35f, sz = pz - sinf(ang) * 0.35f;
            if (!cue_table_on_bed(t, sx, sz)) continue;
            q.on = 1; q.id = 1; q.pos = v3(sx, t->R, sz); q.orient = m3_identity();
            Vec3 dir = v3(px - sx, 0, pz - sz);
            float l = sqrtf(dir.x*dir.x + dir.z*dir.z);
            if (l < 1e-4f) continue;
            dir = v3(dir.x/l, 0, dir.z/l);
            (*tried)++;
            cue_phys_strike(w, &q, dir, 1.4f, 0.0f, 0.0f);
            uint32_t ev = 0;
            for (int it = 0; it < 4000; it++)
                if (!cue_phys_step(w, &q, 1, 1.0f/120.0f, &ev)) break;
            if (!q.on && q.pocket != CUE_OFF_TABLE) (*in)++;
        }
    }
}

int main(void) {
    CueTable t; CueWorld w;
    cue_table_init(&t, CUE_GAME_PYRAMID);
    cue_table_build_world(&t, &w);

    printf("russian pyramid\n\n");

    /* ---- the table ------------------------------------------------------- */
    {   char why[128];
        ok(cue_table_validate(&t, why, sizeof why) == 1, "the pyramid table validates");
        ok(cue_table_game_ok(&t, CUE_GAME_PYRAMID, 0, why, sizeof why) == 1,
                                          "...and the game can be set out on it");
        printf("     %.0f x %.0f mm, ball %.0f mm, corner mouth %.0f mm (%.2f R)\n",
               (double)(t.half_len*2000.0f), (double)(t.half_wid*2000.0f),
               (double)(t.R*2000.0f), (double)(t.pr_corner*2000.0f),
               (double)(t.pr_corner/t.R));
        ok(t.pr_corner > t.R,             "the mouth admits the ball");
        ok(t.pr_corner < 1.3f * t.R,      "...and only just, which is the game");
        /* THE FEDERATION'S NUMBERS, to the millimetre. The tournament ball is
         * 67 mm at 255 g — 68 was the OLD one, and is what this shipped with —
         * with the corner about five millimetres wider than the ball and the
         * middle about fifteen. And the middle is the WIDER of the two, which
         * is the other way round from every other table here and was got
         * backwards first time. */
        ok(fabsf(t.R * 2000.0f - 67.0f) < 0.6f,     "the ball is 67 mm");
        ok(fabsf(t.mass - 0.255f) < 0.010f,         "...and 255 g");
        /* MEASURED, NOT AUTHORED. These read pr_corner*2 as the mouth, which
         * it never was and certainly is not now: pr is the DROP and the BORE,
         * and what a ball squeezes through is where the link left the two
         * cushions. On this table the difference is the whole game — a 37 mm
         * pocket radius gives a 74 mm opening, not a 74 mm hole — so the
         * federation's figures have to be checked against the built table. */
        float open_c = 0.0f, open_m = 0.0f;
        cue_table_openings(&t, &open_c, &open_m);
        const float ball_mm = t.R * 2000.0f;
        printf("     openings: corner %.1f mm, middle %.1f mm (ball %.1f mm)\n",
               (double)(open_c*1000.0f), (double)(open_m*1000.0f), (double)ball_mm);
        ok(fabsf(open_c * 1000.0f - (ball_mm + 5.0f)) < 1.5f,
                                          "...the corner five millimetres wider");
        ok(fabsf(open_m * 1000.0f - (ball_mm + 14.5f)) < 1.5f,
                                          "...and the middle about fifteen");
        ok(open_m > open_c,               "...so the middle is the wider of the two");
        ok(t.pocket_round == 0,           "the jaws are cut sharp, not rounded");
        printf("     %s jaws\n", t.pocket_round ? "rounded" : "mitred"); }

    /* ---- the rack -------------------------------------------------------- */
    CueBall b[CUE_MAX_BALLS];
    int n = cue_table_rack(&t, b);
    ok(n == 16,                           "sixteen balls: the white and fifteen");
    {   int on = 0, off = 0;
        for (int i = 0; i < n; i++) {
            if (!b[i].on) continue;
            on++;
            if (!cue_table_on_bed(&t, b[i].pos.x, b[i].pos.z)) off++;
        }
        ok(on == 16,                      "...all of them on the table");
        ok(off == 0,                      "...and every one on the cloth"); }
    /* the white starts in the house, behind the line */
    ok(b[0].pos.x < t.baulk_x + 1e-4f,    "the white starts behind the house line");
    /* ...and nothing else does: the pyramid is racked at the far end */
    {   int behind = 0;
        for (int i = 1; i < n; i++) if (b[i].pos.x < t.baulk_x) behind++;
        ok(behind == 0,                   "...and the pyramid is at the other end"); }
    /* no two balls overlapping, which a triangle laid out wrong gives at once */
    {   int clash = 0;
        for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) {
            float dx = b[i].pos.x - b[j].pos.x, dz = b[i].pos.z - b[j].pos.z;
            if (dx*dx + dz*dz < (1.99f*t.R)*(1.99f*t.R)) clash++;
        }
        ok(clash == 0,                    "...and none of them overlapping"); }

    /* ---- can anything actually be potted? -------------------------------- *
     *
     * The question the pocket size makes worth asking. A ball is placed a foot
     * out from each pocket in turn and rolled straight at it; on a table whose
     * mouths were too tight for the ball, or whose drop circle never caught, all
     * of these would rattle and stay up. */
    {   int tried = 0, in = 0, ptried = 0, pin = 0;
        pot_sweep(&t, &w, &tried, &in);
        {   CueTable pt; CueWorld pw;
            cue_table_init(&pt, CUE_GAME_UK8);
            cue_table_build_world(&pt, &pw);
            pot_sweep(&pt, &pw, &ptried, &pin); }
        ok(tried > 30,                    "a spread of shots was played at the pockets");
        ok(in > 0,                        "...and balls do go in");
        float me = tried ? (float)in * 100.0f / (float)tried : 0.0f;
        float pool = ptried ? (float)pin * 100.0f / (float)ptried : 0.0f;
        printf("     pyramid %d/%d dropped (%.0f%%)   a 7 ft pub table %d/%d (%.0f%%)\n",
               in, tried, (double)me, pin, ptried, (double)pool);
        /* THE CLAIM IS THE COMPARISON, not the number. "Some go in" says
         * nothing: if the same sweep took 90%% on a pub table and 14%% here that
         * is the game, and if it took 20%% there too then the sweep is measuring
         * itself. */
        ok(me < pool - 5.0f,              "...and it is markedly harder than a pool table");
        ok(in < tried,                    "...but not all of them, on these pockets"); }

    /* ---- the rules ------------------------------------------------------- */
    CueRules r;
    cue_rules_init(&r, &t, 0);
    ok(r.mode == CUE_GAME_PYRAMID,        "the rules know which game this is");
    ok(r.kind == 0,                       "...and that it is not snooker");

    /* a pot is a ball, and the turn carries on */
    {   int potted[2] = { 4 };
        r.turn = 0; r.score[0] = 0; r.score[1] = 0;
        cue_rules_resolve(&r, b, n, &w, 4, 0, 1, potted, 1);
        ok(r.score[0] == 1,               "a potted ball scores one");
        ok(r.turn == 0,                   "...and the striker stays at the table");
        ok(r.last_foul == 0,              "...with no foul"); }

    /* ANY ball is on: there are no groups and no order */
    {   int potted[2] = { 15 };
        r.turn = 0;
        cue_rules_resolve(&r, b, n, &w, 15, 0, 1, potted, 1);
        ok(r.score[0] == 2,               "any of the fifteen is on, in any order"); }

    /* a miss ends the visit and scores nothing */
    {   r.turn = 0;
        cue_rules_resolve(&r, b, n, &w, 7, 0, 1, NULL, 0);
        ok(r.score[0] == 2,               "a miss scores nothing");
        ok(r.turn == 1,                   "...and hands the table over");
        ok(r.last_foul == 0,              "...without being a foul"); }

    /* THE PENALTY: a foul gives a ball BACK */
    {   r.turn = 0; r.score[0] = 3; r.score[1] = 1; r.respot = 0;
        cue_rules_resolve(&r, b, n, &w, -1, 0, 0, NULL, 0);   /* hit nothing */
        ok(r.last_foul == 1,              "hitting nothing is a foul");
        ok(r.score[0] == 2,               "...and it costs one of your own balls");
        ok(r.respot == 1,                 "...which goes back on the table");
        ok(r.turn == 1,                   "...and the table goes over");
        ok(r.ball_in_hand == 1,           "...with the cue ball in hand"); }

    /* ...but a player with nothing potted cannot pay */
    {   r.turn = 0; r.score[0] = 0; r.respot = 0;
        cue_rules_resolve(&r, b, n, &w, -1, 0, 0, NULL, 0);
        ok(r.last_foul == 1,              "a foul with no balls potted is still a foul");
        ok(r.score[0] == 0,               "...but the score cannot go below nothing");
        ok(r.respot == 0,                 "...and nothing is put back"); }

    /* the cue ball down a pocket: a foul in CLASSIC... */
    {   cue_rules_init(&r, &t, 0);
        r.pyr_free = CUE_PYR_CLASSIC;
        r.turn = 0; r.score[0] = 2;
        int potted[2] = { 0 };
        cue_rules_resolve(&r, b, n, &w, 6, 1, 1, potted, 0);
        ok(r.last_foul == 1,              "CLASSIC: the cue ball potted is a foul");
        ok(r.score[0] == 1,               "...and costs a ball"); }

    /* ...and a SCORE in combat, which is the shot the game is known for */
    {   cue_rules_init(&r, &t, 0);
        r.pyr_free = CUE_PYR_COMBAT;
        r.turn = 0; r.score[0] = 2;
        cue_rules_resolve(&r, b, n, &w, 6, 1, 1, NULL, 0);
        ok(r.last_foul == 0,              "COMBAT: the cue ball off an object scores");
        ok(r.score[0] == 3,               "...a ball");
        ok(r.turn == 0,                   "...and the visit carries on"); }

    /* ...but only OFF a ball. Straight down a pocket is a foul in both. */
    {   cue_rules_init(&r, &t, 0);
        r.pyr_free = CUE_PYR_COMBAT;
        r.turn = 0; r.score[0] = 2;
        cue_rules_resolve(&r, b, n, &w, -1, 1, 0, NULL, 0);
        ok(r.last_foul == 1,              "COMBAT: straight in off nothing is still a foul"); }

    /* eight takes the frame */
    {   cue_rules_init(&r, &t, 0);
        r.turn = 0; r.score[0] = 7;
        int potted[2] = { 11 };
        cue_rules_resolve(&r, b, n, &w, 11, 0, 1, potted, 1);
        ok(r.frame_over == 1,             "eight balls takes the frame");
        ok(r.winner == 0,                 "...for the player who potted them"); }

    /* seven does not — which is the off-by-one worth pinning */
    {   cue_rules_init(&r, &t, 0);
        r.turn = 0; r.score[0] = 6;
        int potted[2] = { 11 };
        cue_rules_resolve(&r, b, n, &w, 11, 0, 1, potted, 1);
        ok(r.frame_over == 0,             "seven does not"); }

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}
