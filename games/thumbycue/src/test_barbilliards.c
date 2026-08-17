/* BAR BILLIARDS, against the All England Bar Billiards Association rules.
 *
 * The scoring is the easy half — the value of the hole, and double for the red
 * (Rule 97). The game is in the PENALTIES, which are unlike anything else in
 * this engine: they do not dock you a couple of points, they take your break
 * away, and one of them takes everything you have.
 *
 *   Rule 110  loss of the break score
 *   Rule 111  loss of the entire score, for the black skittle
 *   Rule 112  and where both skittles went over, whichever fell FIRST decides
 *
 * The last is the reason the physics records the order they fell in.
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

static CueTable T;
static CueWorld W;
static CueBall  B[CUE_MAX_BALLS];

/* Find the hole worth `v`, so a test can say "the 200" and mean it. */
static int hole_worth(int v) {
    for (int i = 0; i < W.npocket; i++) if (W.pocket_score[i] == v) return i;
    return -1;
}
static int skittle_black(void) {
    for (int i = 0; i < W.nskittle; i++) if (W.skittle_black[i]) return i;
    return -1;
}
static int skittle_white(void) {
    for (int i = 0; i < W.nskittle; i++) if (!W.skittle_black[i]) return i;
    return -1;
}

static void fresh(CueRules *r) {
    cue_table_init(&T, CUE_GAME_BARBILLIARDS);
    cue_table_build_world(&T, &W);
    cue_table_rack(&T, B);
    cue_rules_init(r, &T, 0);
}

/* A stroke: what was potted and down which hole, plus what the host saw. */
typedef struct {
    int pot[4], hole[4];   /* ball id and hole index, 0-terminated by hole=-1 */
    int n;
    int first_hit;
    int white_down, black_down, white_first;
    int in_baulk, short_of, off;
    int from_break;
} Shot;

static void play(CueRules *r, const Shot *s) {
    for (int k = 0; k < W.nskittle; k++) { W.skittle_down[k] = 0; W.skittle_order[k] = 0; }
    int order = 0;
    if (s->white_down && s->white_first) {
        int i = skittle_white(); W.skittle_down[i] = 1; W.skittle_order[i] = ++order;
    }
    if (s->black_down) {
        int i = skittle_black(); W.skittle_down[i] = 1; W.skittle_order[i] = ++order;
    }
    if (s->white_down && !s->white_first) {
        int i = skittle_white(); W.skittle_down[i] = 1; W.skittle_order[i] = ++order;
    }
    int potted[8];
    for (int i = 0; i < s->n; i++) { potted[i] = s->pot[i]; r->bb_hole[i] = s->hole[i]; }
    r->n_off = s->off;
    r->bb_in_baulk = s->in_baulk;
    r->bb_short = s->short_of;
    r->bb_from_break = s->from_break;
    cue_rules_resolve(r, B, 8, &W, s->first_hit, 0, 1, potted, s->n);
}

int main(void) {
    printf("bar billiards\n");

    /* ---- the table (Rules 71, 75, 76, 79) ---- */
    {   cue_table_init(&T, CUE_GAME_BARBILLIARDS);
        cue_table_build_world(&T, &W);
        char d[96];
        snprintf(d, sizeof d, "%.1f x %.1f cm", T.half_len * 200.0f, T.half_wid * 200.0f);
        ok(T.half_len * 200.0f >= 138.4f && T.half_len * 200.0f <= 143.5f &&
           T.half_wid * 200.0f >= 78.7f,
           "the playing area is inside the AEBBA range", d);
        ok(T.nballs == 8, "eight balls: a red and seven whites (Rule 79)", NULL);
        snprintf(d, sizeof d, "D radius %.0f mm, red spot %.0f mm up the table",
                 T.d_radius * 1000.0f, (T.blue_x - T.baulk_x) * 1000.0f);
        ok(fabsf(T.d_radius * 1000.0f - 40.0f) < 6.0f &&
           (T.blue_x - T.baulk_x) * 1000.0f >= 171.0f &&
           (T.blue_x - T.baulk_x) * 1000.0f <= 179.0f,
           "a 4 cm D and the red spot 171-179 mm from it (Rules 75, 76)", d);
        ok(T.baulk_arc > 0.0f, "and a baulk arc (Rule 77)", NULL);
    }

    /* ---- nine holes, and no pockets on the rails ---- */
    {   char d[128];
        snprintf(d, sizeof d, "%d holes", W.npocket);
        ok(W.npocket == 9, "nine holes (Rule 71's full complement)", d);
        int want[9] = { 10, 20, 20, 30, 30, 50, 50, 100, 200 };
        int got[9], gn = 0;
        for (int i = 0; i < W.npocket && gn < 9; i++) got[gn++] = W.pocket_score[i];
        for (int i = 0; i < gn; i++)
            for (int j = i + 1; j < gn; j++)
                if (got[j] < got[i]) { int t2 = got[i]; got[i] = got[j]; got[j] = t2; }
        int same = 1;
        for (int i = 0; i < 9; i++) if (got[i] != want[i]) same = 0;
        snprintf(d, sizeof d, "%d %d %d %d %d %d %d %d %d",
                 got[0],got[1],got[2],got[3],got[4],got[5],got[6],got[7],got[8]);
        ok(same, "10, 20x2, 30x2, 50x2, 100, 200", d);
        /* Every one of them is in the BED, not on a rail. */
        int in_bed = 1;
        for (int i = 0; i < W.npocket; i++)
            if (fabsf(W.pocket[i].x) > T.half_len || fabsf(W.pocket[i].z) > T.half_wid)
                in_bed = 0;
        ok(in_bed, "...and every one of them is in the bed, not on a rail", NULL);
    }

    /* ---- three skittles, one black (Rule 74) ---- */
    {   char d[96];
        int blacks = 0;
        for (int i = 0; i < W.nskittle; i++) blacks += W.skittle_black[i];
        snprintf(d, sizeof d, "%d skittles, %d black", W.nskittle, blacks);
        ok(W.nskittle == 3 && blacks == 1, "one black and two white skittles", d);
        /* Rule 74: the black stands 6 mm clear of the front of the 200. */
        int bi = skittle_black(), h200 = hole_worth(200);
        float gap = (W.pocket[h200].x - W.pocket_r[h200])
                  - (W.skittle[bi].x + W.skittle_r);
        snprintf(d, sizeof d, "%.1f mm", gap * 1000.0f);
        ok(fabsf(gap * 1000.0f - 6.0f) < 1.5f,
           "the black stands 6 mm in front of the 200 hole", d);
        /* Rule 74: the whites level with and 178 mm either side of the 100. */
        int h100 = hole_worth(100);
        int found = 0;
        for (int i = 0; i < W.nskittle; i++) {
            if (W.skittle_black[i]) continue;
            if (fabsf(W.skittle[i].x - W.pocket[h100].x) < 1e-4f &&
                fabsf(fabsf(W.skittle[i].z - W.pocket[h100].z) - 0.178f) < 1e-3f) found++;
        }
        snprintf(d, sizeof d, "%d of 2", found);
        ok(found == 2, "the whites level with the 100 and 178 mm either side", d);
    }

    /* ---- Rule 97: a white scores the hole ---- */
    {   CueRules r; fresh(&r);
        Shot s = { { CUE_ID_CUE }, { hole_worth(50) }, 1, CUE_ID_CUE, 0,0,0, 0,0,0, 0 };
        play(&r, &s);
        char d[64]; snprintf(d, sizeof d, "scored %d", r.score[0]);
        ok(r.score[0] == 50, "a white in the 50 scores fifty", d);
        ok(r.turn == 0, "...and the break continues (Rule 98)", NULL);
    }

    /* ---- Rule 97: the red doubles it ---- */
    {   CueRules r; fresh(&r);
        Shot s = { { CUE_ID_BIL_RED }, { hole_worth(100) }, 1, CUE_ID_CUE, 0,0,0, 0,0,0, 0 };
        play(&r, &s);
        char d[64]; snprintf(d, sizeof d, "scored %d", r.score[0]);
        ok(r.score[0] == 200, "the red in the 100 scores two hundred", d);
    }

    /* ...and the 200 with the red is four hundred. */
    {   CueRules r; fresh(&r);
        Shot s = { { CUE_ID_BIL_RED }, { hole_worth(200) }, 1, CUE_ID_CUE, 0,0,0, 0,0,0, 0 };
        play(&r, &s);
        char d[64]; snprintf(d, sizeof d, "scored %d", r.score[0]);
        ok(r.score[0] == 400, "the red in the 200 scores four hundred", d);
    }

    /* ---- Rule 98: failing to pot ends the break ---- */
    {   CueRules r; fresh(&r);
        Shot pot = { { CUE_ID_CUE }, { hole_worth(30) }, 1, CUE_ID_CUE, 0,0,0, 0,0,0, 0 };
        play(&r, &pot);
        Shot miss = { { 0 }, { -1 }, 0, CUE_ID_CUE, 0,0,0, 0,0,0, 0 };
        play(&r, &miss);
        char d[64]; snprintf(d, sizeof d, "%d, break %d", r.score[0], r.bb_break);
        ok(r.score[0] == 30 && r.bb_break == 0 && r.turn == 1,
           "a stroke that pots nothing ends the break and keeps the points", d);
    }

    /* ---- Rule 110(b): failing to hit a ball costs the break ---- */
    {   CueRules r; fresh(&r);
        Shot pot = { { CUE_ID_CUE }, { hole_worth(50) }, 1, CUE_ID_CUE, 0,0,0, 0,0,0, 0 };
        play(&r, &pot); play(&r, &pot);
        Shot miss = { { 0 }, { -1 }, 0, -1, 0,0,0, 0,0,0, 0 };
        play(&r, &miss);
        char d[80]; snprintf(d, sizeof d, "%d left of 100, %s", r.score[0], r.msg);
        ok(r.last_foul && r.score[0] == 0,
           "hitting nothing takes the whole break back off", d);
    }

    /* ---- ...and only the break, not an earlier one ---- */
    {   CueRules r; fresh(&r);
        Shot pot = { { CUE_ID_CUE }, { hole_worth(50) }, 1, CUE_ID_CUE, 0,0,0, 0,0,0, 0 };
        Shot miss = { { 0 }, { -1 }, 0, CUE_ID_CUE, 0,0,0, 0,0,0, 0 };
        play(&r, &pot);            /* 50 */
        play(&r, &miss);           /* break ends cleanly, 50 banked */
        r.turn = 0;
        play(&r, &pot);            /* another 50 */
        Shot foul = { { 0 }, { -1 }, 0, -1, 0,0,0, 0,0,0, 0 };
        play(&r, &foul);           /* takes only the second 50 */
        char d[64]; snprintf(d, sizeof d, "%d", r.score[0]);
        ok(r.score[0] == 50, "a foul takes the current break only", d);
    }

    /* ---- Rule 110(c),(d): a ball back over the baulk line ---- */
    {   CueRules r; fresh(&r);
        Shot pot = { { CUE_ID_CUE }, { hole_worth(30) }, 1, CUE_ID_CUE, 0,0,0, 0,0,0, 0 };
        play(&r, &pot);
        Shot s = { { 0 }, { -1 }, 0, CUE_ID_CUE, 0,0,0, 1,0,0, 0 };
        play(&r, &s);
        ok(r.last_foul && r.score[0] == 0,
           "a ball coming back over the baulk line costs the break", r.msg);
    }

    /* ---- Rule 110(e): a ball leaving the table ---- */
    {   CueRules r; fresh(&r);
        Shot pot = { { CUE_ID_CUE }, { hole_worth(30) }, 1, CUE_ID_CUE, 0,0,0, 0,0,0, 0 };
        play(&r, &pot);
        Shot s = { { 0 }, { -1 }, 0, CUE_ID_CUE, 0,0,0, 0,0,1, 0 };
        play(&r, &s);
        ok(r.last_foul && r.score[0] == 0, "a ball leaving the table costs the break", r.msg);
    }

    /* ---- Rule 110(o): short of the black peg, having hit nothing ---- */
    {   CueRules r; fresh(&r);
        Shot pot = { { CUE_ID_CUE }, { hole_worth(30) }, 1, CUE_ID_CUE, 0,0,0, 0,0,0, 0 };
        play(&r, &pot);
        Shot s = { { 0 }, { -1 }, 0, CUE_ID_CUE, 0,0,0, 0,1,0, 0 };
        play(&r, &s);
        ok(r.last_foul && r.score[0] == 0,
           "a cue ball short of the black peg's line costs the break", r.msg);
    }

    /* ---- Rule 110(f): a white skittle costs the break ---- */
    {   CueRules r; fresh(&r);
        Shot pot = { { CUE_ID_CUE }, { hole_worth(100) }, 1, CUE_ID_CUE, 0,0,0, 0,0,0, 0 };
        play(&r, &pot);
        char d[80]; snprintf(d, sizeof d, "was %d", r.score[0]);
        Shot s = { { CUE_ID_CUE }, { hole_worth(50) }, 1, CUE_ID_CUE, 1,0,1, 0,0,0, 0 };
        play(&r, &s);
        ok(r.last_foul && r.score[0] == 0, "a white skittle costs the break", d);
        ok(!r.frame_over, "...but not the frame", NULL);
    }

    /* ---- Rule 111(a): the black skittle costs everything ---- */
    {   CueRules r; fresh(&r);
        Shot pot = { { CUE_ID_CUE }, { hole_worth(100) }, 1, CUE_ID_CUE, 0,0,0, 0,0,0, 0 };
        play(&r, &pot);
        Shot miss = { { 0 }, { -1 }, 0, CUE_ID_CUE, 0,0,0, 0,0,0, 0 };
        play(&r, &miss);           /* bank the 100 in a finished break */
        r.turn = 0;
        play(&r, &pot);            /* another 100 in a new break */
        char d[80]; snprintf(d, sizeof d, "had %d", r.score[0]);
        Shot s = { { 0 }, { -1 }, 0, CUE_ID_CUE, 0,1,0, 0,0,0, 0 };
        play(&r, &s);
        ok(r.last_foul && r.score[0] == 0,
           "the black skittle takes the ENTIRE score, banked breaks and all", d);
    }

    /* ---- Rule 112: whichever fell first decides ---- */
    {   CueRules r; fresh(&r);
        Shot pot = { { CUE_ID_CUE }, { hole_worth(100) }, 1, CUE_ID_CUE, 0,0,0, 0,0,0, 0 };
        Shot miss = { { 0 }, { -1 }, 0, CUE_ID_CUE, 0,0,0, 0,0,0, 0 };
        play(&r, &pot); play(&r, &miss);   /* 100 banked */
        r.turn = 0;
        play(&r, &pot);                    /* 100 in the break */
        /* both go over, the WHITE first: Rule 112 makes it loss of break */
        Shot s = { { 0 }, { -1 }, 0, CUE_ID_CUE, 1,1,1, 0,0,0, 0 };
        play(&r, &s);
        char d[64]; snprintf(d, sizeof d, "%d left", r.score[0]);
        ok(r.score[0] == 100, "white first: the break goes, the banked score stays", d);
    }
    {   CueRules r; fresh(&r);
        Shot pot = { { CUE_ID_CUE }, { hole_worth(100) }, 1, CUE_ID_CUE, 0,0,0, 0,0,0, 0 };
        Shot miss = { { 0 }, { -1 }, 0, CUE_ID_CUE, 0,0,0, 0,0,0, 0 };
        play(&r, &pot); play(&r, &miss);
        r.turn = 0;
        play(&r, &pot);
        /* both go over, the BLACK first: everything */
        Shot s = { { 0 }, { -1 }, 0, CUE_ID_CUE, 1,1,0, 0,0,0, 0 };
        play(&r, &s);
        char d[64]; snprintf(d, sizeof d, "%d left", r.score[0]);
        ok(r.score[0] == 0, "black first: the whole score goes", d);
    }

    /* ---- Rule 110(a): both balls from the break, four times running ---- */
    {   CueRules r; fresh(&r);
        Shot both = { { CUE_ID_CUE, CUE_ID_BIL_RED },
                      { hole_worth(30), hole_worth(20) }, 2, CUE_ID_BIL_RED,
                      0,0,0, 0,0,0, 1 };
        for (int i = 0; i < CUE_BB_MAX_BOTH; i++) play(&r, &both);
        char d[80];
        snprintf(d, sizeof d, "%d in a row, score %d", r.bb_both_potted, r.score[0]);
        ok(!r.last_foul && r.bb_both_potted == CUE_BB_MAX_BOTH,
           "potting both from the break three times running is legal", d);
        play(&r, &both);
        ok(r.last_foul, "...and the fourth is a foul", r.msg);
    }

    /* ---- the clock, not a target ---- */
    {   CueRules r; fresh(&r);
        char d[64]; snprintf(d, sizeof d, "%.0f s", r.bb_time);
        ok(r.bb_time > 900.0f && r.bb_time < 1200.0f,
           "a coin buys between fifteen and twenty minutes", d);
        ok(r.target_score == 0, "...and there is no target score to reach", NULL);
    }

    /* ---- AND IT HAS TO ACTUALLY PLAY -----------------------------------
     *
     * The rules above are arithmetic. This is the table: roll a ball at each
     * hole from the D and see that it goes down, and that the pegs standing
     * among them get knocked over when a ball reaches one. A hole in the
     * middle of a bed is a thing no other table here has, and the physics was
     * written when every pocket was on a rail. */
    {   cue_table_init(&T, CUE_GAME_BARBILLIARDS);
        cue_table_build_world(&T, &W);
        int n = cue_table_rack(&T, B);
        int drops = 0, wrong = 0;
        for (int p = 0; p < W.npocket; p++) {
            for (int i = 0; i < n; i++) B[i].on = 0;
            B[0].on = 1; B[0].pocket = 0; B[0].drop = 0.0f;
            B[0].pos = v3(T.baulk_x, T.R, 0.0f);
            float dx = W.pocket[p].x - B[0].pos.x, dz = W.pocket[p].z - B[0].pos.z;
            float L = sqrtf(dx*dx + dz*dz);
            B[0].vel = v3(dx/L * 1.55f, 0, dz/L * 1.55f);
            B[0].w = v3(0,0,0);
            cue_phys_shot_begin(&W);
            uint32_t ev = 0;
            for (int s2 = 0; s2 < 4000 && cue_phys_moving(&W, B, n); s2++) {
                cue_phys_step(&W, B, n, 1.0f/240.0f, &ev);
                if (!B[0].on) break;
            }
            if (!B[0].on) drops++; else wrong++;
        }
        char d[64]; snprintf(d, sizeof d, "%d of %d holes took the ball", drops, W.npocket);
        ok(wrong == 0, "a ball rolled at any hole goes down it", d);
    }

    /* The black peg guards the 200: a ball rolled straight up the centre line
     * cannot reach it without knocking the peg over — which is the whole
     * reason the 200 is worth two hundred. */
    {   cue_table_init(&T, CUE_GAME_BARBILLIARDS);
        cue_table_build_world(&T, &W);
        int n = cue_table_rack(&T, B);
        int bi = skittle_black();
        for (int i = 0; i < n; i++) B[i].on = 0;
        B[0].on = 1; B[0].pocket = 0; B[0].drop = 0.0f;
        /* Start beyond the 10 hole so the only thing in the way is the peg. */
        B[0].pos = v3(W.skittle[bi].x - 0.08f, T.R, 0.0f);
        B[0].vel = v3(1.2f, 0, 0);
        B[0].w = v3(0,0,0);
        cue_phys_shot_begin(&W);
        uint32_t ev = 0, e2 = 0;
        for (int s2 = 0; s2 < 4000 && cue_phys_moving(&W, B, n); s2++) {
            e2 = 0;
            cue_phys_step(&W, B, n, 1.0f/240.0f, &e2);
            ev |= e2;                      /* the step reports ITS events only */
            if (!B[0].on) break;
        }
        char d[80];
        snprintf(d, sizeof d, "peg %s, ball %s",
                 W.skittle_down[bi] ? "over" : "standing", B[0].on ? "up" : "down");
        ok(W.skittle_down[bi], "straight up the middle knocks the black peg over", d);
        ok(ev & CUE_EV_SKITTLE, "...and the host is told a skittle went", NULL);
    }

    /* A ball that never reaches a peg leaves it standing. */
    {   cue_table_init(&T, CUE_GAME_BARBILLIARDS);
        cue_table_build_world(&T, &W);
        int n = cue_table_rack(&T, B);
        for (int i = 0; i < n; i++) B[i].on = 0;
        B[0].on = 1; B[0].pocket = 0; B[0].drop = 0.0f;
        B[0].pos = v3(T.baulk_x, T.R, 0.0f);
        B[0].vel = v3(0.35f, 0, 0);       /* dies well short */
        B[0].w = v3(0,0,0);
        cue_phys_shot_begin(&W);
        uint32_t ev = 0;
        for (int s2 = 0; s2 < 6000 && cue_phys_moving(&W, B, n); s2++)
            cue_phys_step(&W, B, n, 1.0f/240.0f, &ev);
        int any = 0;
        for (int k = 0; k < W.nskittle; k++) any += W.skittle_down[k];
        char d[64]; snprintf(d, sizeof d, "stopped at x %+.3f", (double)B[0].pos.x);
        ok(!any, "a ball that stops short knocks nothing over", d);
        /* ...and Rule 110(o) calls that short of the black peg's line. */
        ok(cue_rules_bb_short(&T, B[0].pos.x, 0),
           "and the rules call it short of the black peg (Rule 110(o))", NULL);
    }

    /* Rule 110(c): a ball at rest inside the baulk arc. */
    {   CueRules r; fresh(&r);
        int n = cue_table_rack(&T, B);
        for (int i = 0; i < n; i++) B[i].on = 0;
        B[0].on = 1; B[0].pos = v3(T.baulk_x + 0.10f, T.R, 0.0f);
        ok(cue_rules_bb_in_baulk(&r, &T, B, n),
           "a ball resting inside the baulk arc is in baulk", NULL);
        B[0].pos = v3(T.baulk_x + 0.60f, T.R, 0.0f);
        ok(!cue_rules_bb_in_baulk(&r, &T, B, n),
           "...and one well up the table is not", NULL);
    }

    /* The clock runs down and drops the bar. */
    {   CueRules r; fresh(&r);
        cue_rules_bb_tick(&r, 600.0f);
        ok(!r.bb_barred, "ten minutes in, the bar is still up", NULL);
        cue_rules_bb_tick(&r, 600.0f);
        char d[64]; snprintf(d, sizeof d, "%.0f s left", r.bb_time);
        ok(r.bb_barred && r.bb_time == 0.0f, "and when it runs out the bar drops", d);
    }

    printf(s_fail ? "\nFAILED (%d)\n" : "\nPASSED\n", s_fail);
    return s_fail ? 1 : 0;
}
