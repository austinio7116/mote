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
#include "mote_arena.h"    /* the skittles' rigid bodies want a few KB */
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
    /* THE SKITTLES ARE RIGID BODIES and the solver is the engine's, handed to
     * the physics rather than depended on — so a test that wants them to fall
     * over has to hand it over, exactly as the app does. */
    {   static uint8_t pool[CUE_SKITTLE_ARENA];
        static MoteArena arena;
        mote_arena_init(&arena, pool, sizeof pool);
        if (!mote_phys_configure(&arena, CUE_SKITTLE_BODIES, CUE_SKITTLE_CONTACTS))
            printf("  (the skittles' pools did not fit)\n");
        cue_phys_set_rigid(mote_phys_step);
    }

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

    /* ---- Rule 110(c) and (d): what "in baulk" actually is ----------------
     *
     * The lines RADIATE from the break spot to the side cushions with 150 to
     * 160 degrees between them (Rule 77) — a shallow V a little short of
     * square across the table — so the baulk is the wedge behind them, out at
     * the sides near the base. Read once as a circle about the break spot,
     * which swallowed the red spot 175 mm up the table and made every stroke a
     * foul before it was played. */
    {   CueRules r; fresh(&r);
        int n = cue_table_rack(&T, B);
        for (int i = 0; i < n; i++) B[i].on = 0;
        B[0].on = 1;

        B[0].pos = v3(T.baulk_x + 0.02f, T.R, 0.30f);
        ok(cue_rules_bb_in_baulk(&r, &T, B, n),
           "out at the side by the base is in baulk", NULL);

        B[0].pos = v3(T.baulk_x + 0.01f, T.R, 0.0f);
        ok(cue_rules_bb_in_baulk(&r, &T, B, n),
           "...and so is anything on the D (Rule 110(d))", NULL);

        B[0].pos = v3(T.baulk_x + 0.60f, T.R, 0.0f);
        ok(!cue_rules_bb_in_baulk(&r, &T, B, n),
           "a ball well up the table is not", NULL);

        /* THE ONE THAT MATTERS: the red spot has to be clear of it, or the
         * break position itself is a foul. */
        B[0].pos = v3(T.blue_x, T.R, 0.0f);
        char d[80];
        snprintf(d, sizeof d, "the red spot is %.0f mm up the table",
                 (T.blue_x - T.baulk_x) * 1000.0f);
        ok(!cue_rules_bb_in_baulk(&r, &T, B, n),
           "...and neither is the red on its spot", d);
    }

    /* The clock runs down and drops the bar. */
    {   CueRules r; fresh(&r);
        cue_rules_bb_tick(&r, 600.0f);
        ok(!r.bb_barred, "ten minutes in, the bar is still up", NULL);
        cue_rules_bb_tick(&r, 600.0f);
        char d[64]; snprintf(d, sizeof d, "%.0f s left", r.bb_time);
        ok(r.bb_barred && r.bb_time == 0.0f, "and when it runs out the bar drops", d);
    }

    /* ---- THE SKITTLES ARE ACTUALLY THERE ------------------------------- *
     *
     * They were not. check_skittles recorded that one had gone over and did
     * nothing else — the ball passed straight through — on the argument that a
     * pin modelled as something to rebound off would be a worse lie than
     * ignoring the deflection. It is the opposite: playing around the pins IS
     * bar billiards, and a ball that goes through them is not playing the game.
     *
     * AEBBA rule 74 gives the shape: cylindrical to at least 51 mm above the
     * base, 15-18 mm across, 114 mm tall. A ball is 47.6 mm across, so its
     * highest point is below the flare and it always meets the STEM — which is
     * why one radius is the whole collider and the mushroom is what you see. */
    {   CueTable t; cue_table_init(&t, CUE_GAME_BARBILLIARDS);
        static CueWorld ww;
        int hit = 0, missed = 0, rocked = 0; float worst_dev = 0.0f;
        for (int i = -2; i <= 2; i++) {
            cue_table_build_world(&t, &ww);
            CueBall b; memset(&b, 0, sizeof b);
            b.on = 1; b.id = 1; b.r = t.R; b.orient = m3_identity();
            const Vec3 sk = ww.skittle[2];              /* the black */
            const float off = (float)i * 0.012f;
            b.pos = v3(sk.x - 0.09f, t.R, sk.z + off);
            cue_phys_shot_begin(&ww);
            cue_phys_strike(&ww, &b, v3(1,0,0), 2.0f, 0.0f, 0.0f);
            uint32_t ev; float secs = 0.0f;
            while (secs < 6.0f) {
                cue_phys_step(&ww, &b, 1, 1.0f/240.0f, &ev);
                secs += 1.0f/240.0f;
                if (!cue_phys_moving(&ww, &b, 1)) break;
            }
            if (ww.skittle_down[2]) hit++;
            else { missed++; if (ww.skittle_nudged[2]) rocked++; }
            /* and it must have been turned: a clip off centre changes the line */
            if (i != 0) {
                float dev = fabsf(b.pos.z - (sk.z + off));
                if (dev > worst_dev) worst_dev = dev;
            }
        }
        /* THE THIN ONES DO NOT ALL GO OVER, and that is the game rather than a
         * shortcoming: a full or half-ball contact fells a 48 g pin, a clip off
         * its edge rocks it and leaves it standing, which is what Rule 103 is
         * written for. When the pin was 12 g of balsa everything went over. */
        {   char d[64];
            snprintf(d, sizeof d, "%d of 5 over, %d rocked and left standing", hit, rocked);
            ok(hit == 3, "a ball into the body of a skittle fells it", d);
            ok(rocked == missed && rocked == 2,
               "...and a clip off its edge rocks it without felling it", d); }
        ok(worst_dev > 0.005f,
           "...and the ball is turned by it, which is the whole game",
           "an off-centre clip moves the ball off its line");
    }

    /* Rule 103: OFF ITS SPOT BUT STILL STANDING. A ball arriving at a crawl
     * rocks a pin without felling it, and that is not a foul — the score counts
     * and the skittle is put back before the next shot. */
    {   CueTable t; cue_table_init(&t, CUE_GAME_BARBILLIARDS);
        static CueWorld ww; cue_table_build_world(&t, &ww);
        CueBall b; memset(&b, 0, sizeof b);
        b.on = 1; b.id = 1; b.r = t.R; b.orient = m3_identity();
        const Vec3 sk = ww.skittle[2];
        b.pos = v3(sk.x - (t.R + ww.skittle_r) - 0.004f, t.R, sk.z);
        cue_phys_shot_begin(&ww);
        cue_phys_strike(&ww, &b, v3(1,0,0), 0.06f, 0.0f, 0.0f);
        uint32_t ev; float secs = 0.0f;
        while (secs < 6.0f) {
            cue_phys_step(&ww, &b, 1, 1.0f/240.0f, &ev);
            secs += 1.0f/240.0f;
            if (!cue_phys_moving(&ww, &b, 1)) break;
        }
        ok(!ww.skittle_down[2], "a ball that barely reaches one leaves it standing",
           "rule 103");
        {   char d[72];
            const float dx = ww.sk[2].pos.x - ww.skittle_spot[2].x;
            const float dz = ww.sk[2].pos.z - ww.skittle_spot[2].z;
            snprintf(d, sizeof d, "moved %.1f mm, upright %.3f",
                     (double)(sqrtf(dx*dx + dz*dz) * 1000.0f),
                     (double)ww.sk[2].orient.r[1].y);
            /* Rule 103 is now an OUTCOME, not a threshold: a pin that ends up
             * off its spot and still on its feet is a nudge. A ball this slow
             * may not shift it at all, which is also fine — what must not
             * happen is it being felled. */
            ok(ww.sk[2].orient.r[1].y > 0.70f,
               "...and it is still on its feet afterwards", d); }
    }

    /* ---- the trough feeds the D (Rules 91, 92, 94, 96) ----
     * Pot BOTH balls from the break and the table is empty — the reported
     * stuck game. Setup must hand the striker a ball and rebuild the break
     * position, every time, without being asked twice. */
    {   CueRules r; cue_rules_init(&r, &T, 0);
        for (int i = 0; i < 8; i++) B[i].on = 0;
        int placed = cue_rules_bb_setup(&r, &T, B, 8);
        int red_on = -1;
        for (int i = 0; i < 8; i++)
            if (B[i].on && B[i].id == CUE_ID_BIL_RED) red_on = i;
        ok(placed && B[0].on, "an empty table hands the striker a ball", NULL);
        ok(red_on >= 0, "...and the red goes back on its spot (Rule 94)", NULL);
        ok(r.bb_from_break && !r.ball_in_hand,
           "the break plays from the SPOT, not from hand (Rule 92)", NULL);
        char d[64];
        snprintf(d, sizeof d, "white at %.3f,%.3f", (double)B[0].pos.x, (double)B[0].pos.z);
        ok(fabsf(B[0].pos.x - T.baulk_x) < 1e-4f && fabsf(B[0].pos.z) < 1e-4f,
           "...on the break spot", d);
    }

    /* ---- the struck ball comes off the cloth, not off the D (Rule 96) ---- */
    {   CueRules r; cue_rules_init(&r, &T, 0);
        for (int i = 0; i < 8; i++) B[i].on = 0;
        B[0].on = 1; B[0].id = CUE_ID_CUE;
        B[0].pos = v3(0.30f, T.R, 0.10f);          /* left up the table */
        B[1].on = 1; B[1].id = CUE_ID_BIL_RED;
        B[1].pos = v3(T.blue_x, T.R, 0.0f);        /* an object ball exists */
        cue_rules_bb_setup(&r, &T, B, 8);
        int on_cloth = 0;
        for (int i = 1; i < 8; i++)
            if (B[i].on && fabsf(B[i].pos.x - 0.30f) < 1e-4f) on_cloth = 1;
        ok(B[0].on && fabsf(B[0].pos.x - T.baulk_x) < 1e-4f,
           "a ball left on the cloth stays; the striker takes another", NULL);
        ok(on_cloth, "...and the stranded one is still where it stopped", NULL);
        ok(r.ball_in_hand, "a normal shot places anywhere in the D (Rule 96)", NULL);
    }

    /* ---- Rule 105: rack empty, take the ball furthest from the top ---- */
    {   CueRules r; cue_rules_init(&r, &T, 0);
        r.bb_barred = 1; r.bb_left = 3;
        for (int i = 0; i < 8; i++) B[i].on = 0;
        B[0].on = 1; B[0].id = CUE_ID_CUE;     B[0].pos = v3(0.40f, T.R, 0.00f);
        B[1].on = 1; B[1].id = CUE_ID_BIL_RED; B[1].pos = v3(0.10f, T.R, 0.20f);
        B[2].on = 1; B[2].id = CUE_ID_CUE;     B[2].pos = v3(0.10f, T.R, 0.05f);
        cue_rules_bb_setup(&r, &T, B, 8);
        char d[64];
        snprintf(d, sizeof d, "took the one that stood at 0.10,0.05");
        ok(B[0].on && fabsf(B[0].pos.x - T.baulk_x) < 1e-4f,
           "rack empty: a table ball is lifted to the D", NULL);
        int stranded = 0;
        for (int i = 1; i < 8; i++)
            if (B[i].on && fabsf(B[i].pos.z - 0.05f) < 1e-4f) stranded = 1;
        ok(!stranded, "...the furthest from the top, nearest the centre line", d);
    }

    /* ---- Rules 110(c),(d): the offending ball goes back to the rack ---- */
    {   CueRules r; cue_rules_init(&r, &T, 0);
        for (int i = 0; i < 8; i++) B[i].on = 0;
        B[1].on = 1; B[1].id = CUE_ID_CUE;
        B[1].pos = v3(T.baulk_x + 0.01f, T.R, 0.0f);   /* sat on the D */
        B[2].on = 1; B[2].id = CUE_ID_CUE;
        B[2].pos = v3(0.30f, T.R, 0.0f);               /* fine where it is */
        int m = cue_rules_bb_baulk_return(&r, &T, B, 8);
        ok(m == 1 && !B[1].on && B[2].on,
           "a ball on the D is returned to the rack, and only that one", NULL);
    }

    /* ---- Rule 108: the last-ball shot ---- */
    {   CueRules r; cue_rules_init(&r, &T, 0);
        r.bb_barred = 1; r.bb_left = 1; r.bb_last_ball = 1;
        r.score[0] = 500; r.score[1] = 400; r.turn = 0;
        for (int i = 0; i < 8; i++) B[i].on = 0;
        int hole100 = -1, hole30 = -1;
        for (int p = 0; p < W.npocket; p++) {
            if (W.pocket_score[p] == 100) hole100 = p;
            if (W.pocket_score[p] == 30 && hole30 < 0) hole30 = p;
        }
        /* a miss passes the shot across, and nothing else */
        Shot miss = {0}; miss.first_hit = -1;
        W.side_cushion = 1;
        play(&r, &miss);
        ok(!r.frame_over && r.turn == 1 && r.score[0] == 500,
           "a missed last ball passes to the other player, no penalty", r.msg);
        /* into the 100 without a side cushion: game over, no score */
        r.turn = 0; r.bb_last_ball = 1;
        Shot dry = {0}; dry.n = 1; dry.pot[0] = CUE_ID_CUE; dry.hole[0] = hole100;
        dry.first_hit = -1;
        W.side_cushion = 0;
        play(&r, &dry);
        ok(r.frame_over && r.score[0] == 500,
           "potted without the side cushion: game over, score does not count", r.msg);
        /* into the 100 off a side cushion: it counts, and the game ends */
        cue_rules_init(&r, &T, 0);
        r.bb_barred = 1; r.bb_left = 1; r.bb_last_ball = 1;
        r.score[0] = 500; r.score[1] = 400; r.turn = 0;
        W.side_cushion = 1;
        Shot good = {0}; good.n = 1; good.pot[0] = CUE_ID_CUE; good.hole[0] = hole100;
        good.first_hit = -1;
        play(&r, &good);
        ok(r.frame_over && r.score[0] == 600 && r.winner == 0,
           "off a side cushion into the 100: it counts and the game ends", r.msg);
        /* the black on the last ball still costs everything, and ends it */
        cue_rules_init(&r, &T, 0);
        r.bb_barred = 1; r.bb_left = 1; r.bb_last_ball = 1;
        r.score[0] = 500; r.score[1] = 400; r.turn = 0;
        W.side_cushion = 1;
        Shot blk = {0}; blk.black_down = 1; blk.first_hit = -1;
        play(&r, &blk);
        ok(r.frame_over && r.score[0] == 0 && r.winner == 1,
           "the black on the last ball: score lost, game over", r.msg);
        (void)hole30;
    }

    /* ---- the holes are HOLES, not pockets cut in an edge ----
     * cue_phys_cut_out models a pocket as an arc with two legs running out to
     * the rail. Run the bar billiards holes through that and the legs claimed
     * half the table as "no cloth here", so balls were swallowed from nowhere
     * near a hole. A hole in the open bed is a circle and nothing else. */
    {   cue_table_build_world(&T, &W);
        int wrong = 0; float worst = 0.0f;
        for (float z = -0.36f; z <= 0.361f; z += 0.004f)
            for (float x = -0.70f; x <= 0.701f; x += 0.004f) {
                /* The cut is drawn a hair outside the capture circle so the
                 * two meet exactly; what must never happen is a claim out on
                 * the open cloth. */
                int near = 0;
                for (int p = 0; p < W.npocket; p++) {
                    float dx = x - W.pocket[p].x, dz = z - W.pocket[p].z;
                    float e = W.cut_r[p] + 0.001f;
                    if (dx*dx + dz*dz <= e*e) near = 1;
                }
                if (near) continue;
                for (int p = 0; p < W.npocket; p++) {
                    float o = cue_phys_cut_out(&W, p, x, z);
                    if (o > 0.0f) { wrong++; if (o > worst) worst = o; break; }
                }
            }
        char d[80];
        snprintf(d, sizeof d, "%d cloth samples claimed, worst %.3f m", wrong, worst);
        ok(wrong == 0, "cloth is cloth everywhere except at the nine holes", d);
    }

    /* ---- a ball must be ROLLED in, not driven at it ----
     * The ball is unsupported for the width of the hole and falls under
     * gravity for exactly that long. Cross it fast enough and the far lip is
     * still below the ball's equator, so it kicks it up and onward. */
    {   int down_slow = 0, down_fast = 0; float v_slow = 0, v_fast = 0;
        for (int fast = 0; fast < 2; fast++) {
            cue_table_build_world(&T, &W);
            cue_phys_shot_begin(&W);
            CueBall b; memset(&b, 0, sizeof b);
            b.on = 1; b.id = CUE_ID_CUE;
            b.pos = v3(W.pocket[0].x - 0.30f, T.R, W.pocket[0].z);
            b.orient = (Mat3){{{1,0,0},{0,1,0},{0,0,1}}};
            cue_phys_strike(&W, &b, v3(1,0,0), fast ? 2.4f : 0.9f, 0.0f, 0.0f);
            uint32_t ev; float t = 0.0f; float vlip = 0.0f; int seen = 0;
            while (t < 6.0f) {
                float dx = b.pos.x - W.pocket[0].x, dz = b.pos.z - W.pocket[0].z;
                if (!seen && dx*dx + dz*dz < 0.0036f) {
                    vlip = sqrtf(b.vel.x*b.vel.x + b.vel.z*b.vel.z); seen = 1;
                }
                cue_phys_step(&W, &b, 1, 1.0f/240.0f, &ev);
                t += 1.0f/240.0f;
                if (b.drop > 0.0f) break;
                if (b.pos.x > W.pocket[0].x + 0.12f) break;   /* it got clean past */
                if (!cue_phys_moving(&W, &b, 1)) break;
            }
            if (fast) { down_fast = (b.drop > 0.0f); v_fast = vlip; }
            else      { down_slow = (b.drop > 0.0f); v_slow = vlip; }
        }
        char d[96];
        snprintf(d, sizeof d, "rolled in at %.2f m/s", v_slow);
        ok(down_slow, "a ball rolled at a hole goes down it", d);
        snprintf(d, sizeof d, "crossed at %.2f m/s and stayed up", v_fast);
        ok(!down_fast, "...and one driven at it skips straight over", d);
    }

    /* ---- a skittle is an OBJECT, not an animation (Rules 103, 114) ----
     * It is a mote rigid body: struck, it is knocked off its foot, tumbles and
     * comes to rest somewhere else — and it is stood back on its spot before
     * the next stroke. */
    {   float moved[2] = {0,0}, spin[2] = {0,0};
        for (int hard = 0; hard < 2; hard++) {
            cue_table_build_world(&T, &W);
            cue_phys_shot_begin(&W);
            const int k = 2;                     /* the black, out on its own */
            Vec3 spot = W.skittle_spot[k];
            CueBall b; memset(&b, 0, sizeof b);
            b.on = 1; b.id = CUE_ID_CUE;
            b.pos = v3(spot.x - 0.06f, T.R, spot.z);
            b.orient = (Mat3){{{1,0,0},{0,1,0},{0,0,1}}};
            cue_phys_strike(&W, &b, v3(1,0,0), hard ? 2.9f : 0.7f, 0.0f, 0.0f);
            uint32_t ev; float t = 0.0f;
            while (t < 4.0f) {
                cue_phys_step(&W, &b, 1, 1.0f/240.0f, &ev);
                t += 1.0f/240.0f;
                float ww = sqrtf(W.sk[k].w.x*W.sk[k].w.x + W.sk[k].w.y*W.sk[k].w.y +
                                 W.sk[k].w.z*W.sk[k].w.z);
                if (ww > spin[hard]) spin[hard] = ww;
            }
            float dx = W.sk[k].pos.x - spot.x, dz = W.sk[k].pos.z - spot.z;
            moved[hard] = sqrtf(dx*dx + dz*dz);
            if (hard) ok(W.skittle_down[k], "a pin struck hard goes over", NULL);
            /* ...and it is stood back up when the stroke is JUDGED, which is
             * the host's cue_phys_skittles_respot and not the next stroke. */
            cue_phys_skittles_respot(&W);
            char d[96];
            snprintf(d, sizeof d, "%.4f from the spot, upright %.2f",
                     (double)fabsf(W.sk[k].pos.x - spot.x),
                     (double)W.sk[k].orient.r[1].y);
            if (!hard)
                ok(fabsf(W.sk[k].pos.x - spot.x) < 1e-6f &&
                   fabsf(W.sk[k].pos.z - spot.z) < 1e-6f &&
                   W.sk[k].orient.r[1].y > 0.999f,
                   "a skittle is stood back up on its spot for the next stroke", d);
        }
        char d[96];
        snprintf(d, sizeof d, "%.0f mm and %.0f rad/s gently, %.0f mm and %.0f hard",
                 (double)(moved[0]*1000.0f), (double)spin[0],
                 (double)(moved[1]*1000.0f), (double)spin[1]);
        ok(moved[0] > 0.02f && moved[1] > 0.02f, "a struck pin travels", d);
        /* NOT "further the harder it is hit" — that was an assumption and the
         * bodies disproved it. A hard hit puts its energy into TUMBLING, and a
         * pin that goes end over end digs into the cloth and stops sooner than
         * one that is merely shoved along. What scales with the hit is the
         * spin, which is the thing that makes it read as an object. */
        ok(spin[1] > spin[0] * 1.5f && spin[1] > 20.0f,
           "...and it TUMBLES: a hard hit spins it far faster", d);
    }

    /* ---- the ball carries on through it ----
     * Twelve grams of light wood against a hundred and sixteen of phenolic: the
     * ball should lose a little pace and a little line, not stop. */
    {   cue_table_build_world(&T, &W);
        cue_phys_shot_begin(&W);
        const int k = 2;
        Vec3 spot = W.skittle_spot[k];
        CueBall b; memset(&b, 0, sizeof b);
        b.on = 1; b.id = CUE_ID_CUE;
        b.pos = v3(spot.x - 0.06f, T.R, spot.z);
        b.orient = (Mat3){{{1,0,0},{0,1,0},{0,0,1}}};
        cue_phys_strike(&W, &b, v3(1,0,0), 2.5f, 0.0f, 0.0f);
        uint32_t ev; float t = 0.0f, kept = -1.0f;
        while (t < 2.0f) {
            cue_phys_step(&W, &b, 1, 1.0f/240.0f, &ev); t += 1.0f/240.0f;
            float ww = sqrtf(W.sk[k].w.x*W.sk[k].w.x + W.sk[k].w.y*W.sk[k].w.y +
                             W.sk[k].w.z*W.sk[k].w.z);
            if (kept < 0.0f && ww > 0.5f)
                kept = sqrtf(b.vel.x*b.vel.x + b.vel.z*b.vel.z);
            if (b.drop > 0.0f) break;
        }
        char d[64];
        snprintf(d, sizeof d, "kept %.2f of 2.50 m/s", (double)kept);
        ok(kept > 1.8f && kept < 2.5f, "the ball goes through, losing a little", d);
    }

    printf(s_fail ? "\nFAILED (%d)\n" : "\nPASSED\n", s_fail);
    return s_fail ? 1 : 0;
}
