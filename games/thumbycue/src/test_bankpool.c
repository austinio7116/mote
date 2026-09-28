/*
 * BANK POOL — every scoring ball must come off a cushion first.
 *
 * One rule, and it is the whole game: the same pot is a point or a spotted
 * ball and a lost visit depending on nothing but whether the OBJECT ball found
 * a rail on the way. So that is what these check, either side of the line.
 */
#include "cue_rules.h"
#include "cue_table.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int s_fail;
static void ok(int cond, const char *what, const char *why) {
    printf("  %s   %s%s%s\n", cond ? "ok  " : "FAIL", what,
           (why && why[0]) ? "   " : "", (why && why[0]) ? why : "");
    if (!cond) s_fail++;
}

static CueTable T;
static CueWorld W;
static CueBall  B[CUE_MAX_BALLS];
static int NB;

static void fresh(CueRules *r) {
    cue_table_init(&T, CUE_GAME_BANKPOOL);
    cue_table_build_world(&T, &W);
    NB = cue_table_rack(&T, B);
    cue_rules_init(r, &T, 0);
    r->break_shot = 0;
    memset(W.rail_hit, 0, sizeof W.rail_hit);
}

static int idx_of(int id) {
    for (int i = 1; i < NB; i++) if (B[i].id == id) return i;
    return -1;
}
/* one stroke: these ids potted, each with that many rails of its own. */
static void shot(CueRules *r, int first, int scratch, int cushion,
                 const int *ids, const int *rails, int np) {
    for (int k = 0; k < np; k++) {
        const int i = idx_of(ids[k]);
        if (i > 0) { W.rail_hit[i] = (rails[k]) ? cue_phys_rail_far(&W, B[i].pocket) : 0; B[i].on = 0; }
    }
    /* the striker called the first of them, pocket unnamed -- see the call
     * cases below for what naming one does */
    if (np > 0 && !r->break_shot) cue_rules_call_shot(r, ids[0], -1);
    cue_rules_resolve(r, B, NB, &W, first, scratch, cushion, ids, np);
}
/* ...and one ball, called for pocket `call_pk`, banked into pocket `in_pk`. */
static void called(CueRules *r, int call_id, int call_pk, int id, int in_pk) {
    const int i = idx_of(id);
    B[i].on = 0; B[i].pocket = (unsigned char)in_pk;
    W.rail_hit[i] = cue_phys_rail_far(&W, in_pk);
    cue_rules_call_shot(r, call_id, call_pk);
    int p[1] = { id };
    cue_rules_resolve(r, B, NB, &W, id, 0, 1, p, 1);
}

int main(void) {
    printf("bank pool\n");

    {   CueRules r; fresh(&r);
        ok(r.target_score == 8, "the game is eight of the fifteen", "");
    }

    /* ---- the same pot, either side of the one rule ---- */
    {   CueRules r; fresh(&r);
        int id[1] = { 3 }, rl[1] = { 1 };
        shot(&r, 3, 0, 1, id, rl, 1);
        ok(r.score[0] == 1 && r.turn == 0,
           "a ball off one cushion scores, and you stay at the table", r.msg);
    }
    {   CueRules r; fresh(&r);
        int id[1] = { 3 }, rl[1] = { 0 };
        shot(&r, 3, 0, 1, id, rl, 1);
        ok(r.score[0] == 0 && r.respot == 1 && r.turn == 1,
           "the same pot with no rail scores nothing and is spotted", r.msg);
    }
    {   CueRules r; fresh(&r);
        int id[1] = { 3 }, rl[1] = { 4 };
        shot(&r, 3, 0, 1, id, rl, 1);
        ok(r.score[0] == 1, "more rails than one is still one point", r.msg);
    }

    /* ---- a stroke that does both keeps the point and loses the table ---- */
    {   /* A LEGAL SCORE KEEPS THE TABLE. The unbanked one is spotted and is
         * not a foul, so there is nothing to give the table up for — the old
         * reading gave you the point and took the visit, which is a penalty
         * with no rule behind it. */
        CueRules r; fresh(&r);
        int id[2] = { 3, 5 }, rl[2] = { 2, 0 };
        shot(&r, 3, 0, 1, id, rl, 2);
        ok(r.score[0] == 1 && r.respot == 1 && r.turn == 0,
           "banked one and dropped another: one point, one spot, table kept",
           r.msg);
    }
    {   CueRules r; fresh(&r);
        int id[2] = { 3, 5 }, rl[2] = { 1, 3 };
        shot(&r, 3, 0, 1, id, rl, 2);
        ok(r.score[0] == 1 && r.respot == 1 && r.turn == 0,
           "two banked in one stroke: the called one scores, the other is spotted, and you play on", r.msg);
    }

    /* ---- a foul costs a ball, as it does at one pocket ---- */
    {   CueRules r; fresh(&r);
        r.score[0] = 4;
        shot(&r, -1, 0, 0, NULL, NULL, 0);
        ok(r.last_foul && r.score[0] == 3 && r.turn == 1,
           "a foul puts one of your own balls back", r.msg);
    }
    {   CueRules r; fresh(&r);
        shot(&r, -1, 0, 0, NULL, NULL, 0);
        ok(r.op_owed[0] == 1, "with nothing scored the ball is owed", r.msg);
    }
    {   CueRules r; fresh(&r);
        r.score[0] = 2;
        shot(&r, 3, 1, 1, NULL, NULL, 0);
        ok(r.last_foul && r.ball_in_hand && r.score[0] == 1,
           "a scratch: a ball back and the cue ball in hand", r.msg);
    }
    {   /* an unbanked pot is not a foul — it is simply no score */
        CueRules r; fresh(&r);
        int id[1] = { 3 }, rl[1] = { 0 };
        shot(&r, 3, 0, 0, id, rl, 1);
        ok(!r.last_foul, "an unbanked pot is no score, but no foul either", r.msg);
    }

    /* ---- eight wins it ---- */
    {   CueRules r; fresh(&r);
        r.score[0] = 7;
        int id[1] = { 3 }, rl[1] = { 1 };
        shot(&r, 3, 0, 1, id, rl, 1);
        ok(r.frame_over && r.winner == 0, "eight is the game", r.msg);
    }
    {   /* ...and a ball that never banked cannot win it */
        CueRules r; fresh(&r);
        r.score[0] = 7;
        int id[1] = { 3 }, rl[1] = { 0 };
        shot(&r, 3, 0, 1, id, rl, 1);
        ok(!r.frame_over && r.score[0] == 7,
           "an unbanked ball cannot finish the game", r.msg);
    }

    /* ---- and the ball that comes back is the one that did not bank ---- */
    {   CueRules r; fresh(&r);
        int id[2] = { 3, 9 }, rl[2] = { 2, 0 };
        shot(&r, 3, 0, 1, id, rl, 2);
        {   char m[48]; snprintf(m, sizeof m, "respot_id[0]=%d", r.respot_id[0]);
            ok(r.score[0] == 1 && r.respot == 1 && r.respot_id[0] == 9,
               "the banked 3 scores and the unbanked 9 is the one spotted", m); }
    }

    /* ---- CALLED: ball and pocket (WPA 13) --------------------------------- */
    {   CueRules r; fresh(&r);
        called(&r, 3, 2, 3, 2);
        ok(r.score[0] == 1 && r.turn == 0, "the called ball, banked into the called pocket, scores", r.msg);
    }
    {   CueRules r; fresh(&r);
        called(&r, 3, 2, 3, 4);
        ok(r.score[0] == 0 && r.respot == 1 && r.turn == 1,
           "banked into another pocket: spotted, and the visit is over", r.msg);
    }
    {   CueRules r; fresh(&r);
        called(&r, 5, 2, 3, 2);
        ok(r.score[0] == 0 && r.respot == 1 && r.turn == 1,
           "a ball that was not called does not score however well it banked", r.msg);
    }
    {   CueRules r; fresh(&r);
        const int i = idx_of(3); B[i].on = 0; B[i].pocket = 2;
        W.rail_hit[i] = cue_phys_rail_far(&W, 2);
        int p[1] = { 3 };
        cue_rules_resolve(&r, B, NB, &W, 3, 0, 1, p, 1);
        ok(r.score[0] == 0 && r.last_foul == 0, "nothing called: nothing scores, and it is no foul", r.msg);
    }
    {   CueRules r; fresh(&r);
        const int ia = idx_of(3), ib = idx_of(9);
        B[ia].on = 0; B[ia].pocket = 2; W.rail_hit[ia] = cue_phys_rail_far(&W, 2);
        B[ib].on = 0; B[ib].pocket = 4; W.rail_hit[ib] = cue_phys_rail_far(&W, 4);
        cue_rules_call_shot(&r, 3, 2);
        int p[2] = { 3, 9 };
        cue_rules_resolve(&r, B, NB, &W, 3, 0, 1, p, 2);
        ok(r.score[0] == 1 && r.respot == 1 && r.respot_id[0] == 9 && r.turn == 0,
           "one ball a stroke: the called one scores, a second banked ball is spotted", r.msg);
    }
    {   CueRules r; fresh(&r); r.break_shot = 1;
        const int i = idx_of(3); B[i].on = 0; B[i].pocket = 2;
        W.rail_hit[i] = cue_phys_rail_far(&W, 2);
        int p[1] = { 3 };
        cue_rules_resolve(&r, B, NB, &W, 3, 0, 1, p, 1);
        ok(r.score[0] == 1, "the break is not called: what it banks in counts", r.msg);
    }

    /* ---- WHICH RAIL, and whether it is the pocket's own ------------------ *
     * A rail is the cushion between two neighbouring pockets, jaws included.
     * A bank is a touch on a rail that does not end at the pocket the ball
     * went in. Named by the pockets the table actually has. */
    {   CueRules r; fresh(&r);
        const float hl = W.play_x, hw = W.play_z;
        int cxz = -1, mz = -1, cxnz = -1;           /* +x+z corner, +z middle, +x-z corner */
        for (int p = 0; p < W.npocket; p++) {
            if (W.pocket[p].x >  0.5f*hl && W.pocket[p].z >  0.5f*hw) cxz  = p;
            if (fabsf(W.pocket[p].x) < 0.2f*hl && W.pocket[p].z > 0.5f*hw) mz = p;
            if (W.pocket[p].x >  0.5f*hl && W.pocket[p].z < -0.5f*hw) cxnz = p;
        }
        int a, b;
        cue_phys_rail_at(&W, hl, 0.0f, &a, &b);
        ok((a == cxz && b == cxnz) || (a == cxnz && b == cxz),
           "the middle of an end rail is the rail between its two corners", "");
        cue_phys_rail_at(&W, 0.5f*hl, hw, &a, &b);
        ok((a == cxz && b == mz) || (a == mz && b == cxz),
           "half way down a side is the rail between the corner and the middle", "");
        /* a facing: just along the end rail from the corner, inside the mouth */
        cue_phys_rail_at(&W, hl, hw - 0.06f, &a, &b);
        ok((a == cxz || b == cxz) && a != mz && b != mz,
           "the jaw on the end-rail side of a corner belongs to the end rail", "");
        cue_phys_rail_at(&W, hl - 0.06f, hw, &a, &b);
        ok((a == cxz && b == mz) || (a == mz && b == cxz),
           "...and the one on the side-rail side to the side rail", "");

        /* the middle pocket's jaw is part of a side rail, and that rail is a
         * bank into the corner at the far end of it -- but not into the
         * middle pocket itself */
        const int k = cue_phys_rail_at(&W, 0.06f, hw, NULL, NULL);
        W.rail_hit[1] = (uint16_t)(1u << k);
        ok(!cue_phys_banked(&W, 1, mz), "off the middle's own jaw into the middle: no bank", "");
        {   int ncx = -1;                           /* the -x+z corner */
            for (int p = 0; p < W.npocket; p++)
                if (W.pocket[p].x < -0.5f*hl && W.pocket[p].z > 0.5f*hw) ncx = p;
            ok(!cue_phys_banked(&W, 1, cxz),
               "...into the corner at the end of that same rail: no bank either", "");
            ok(cue_phys_banked(&W, 1, cxnz),
               "...into a corner across the table: a bank", "");
            (void)ncx; }
        W.rail_hit[1] = 0;
    }

    /* ---- PLAYED: into a corner past its own jaw, and off a far rail ------- *
     * Real strokes, so the contact point the physics records is the one being
     * judged. A ball rolled at the corner with the aim swept across the mouth
     * clips the corner's jaws -- a touch, and no bank. */
    {   int jaw_only = 0, pots = 0, bad = 0;
        for (int s2 = -12; s2 <= 12; s2++) {
            CueRules r; fresh(&r);
            for (int i = 0; i < NB; i++) B[i].on = 0;
            const float hl = W.play_x, hw = W.play_z;
            int cxz = -1;
            for (int p = 0; p < W.npocket; p++)
                if (W.pocket[p].x > 0.5f*hl && W.pocket[p].z > 0.5f*hw) cxz = p;
            CueBall *o = &B[1];
            o->on = 1; o->pos = v3(hl - 0.45f, o->pos.y, hw - 0.30f);
            o->vel = v3(0,0,0); o->w = v3(0,0,0); o->drop = 0.0f;
            cue_phys_shot_begin(&W);
            Vec3 tgt = v3(W.pocket[cxz].x, 0.0f, W.pocket[cxz].z);
            Vec3 d = v3(tgt.x - o->pos.x, 0.0f, tgt.z - o->pos.z);
            const float L = sqrtf(d.x*d.x + d.z*d.z);
            const float an = atan2f(d.z, d.x) + s2 * 0.012f;
            (void)L;
            cue_phys_strike(&W, o, v3(cosf(an), 0.0f, sinf(an)), 1.2f, 0.0f, 0.0f);
            uint32_t ev = 0;
            for (int it = 0; it < 20000; it++)
                if (!cue_phys_step(&W, B, NB, 1.0f / 240.0f, &ev)) break;
            if (o->on || o->pocket != cxz) continue;
            pots++;
            if (W.rail_hit[1] && !cue_phys_banked(&W, 1, o->pocket)) jaw_only++;
            if (cue_phys_banked(&W, 1, o->pocket)) bad++;
        }
        char m[64]; snprintf(m, sizeof m, "%d pots, %d touched the jaw, %d called banks", pots, jaw_only, bad);
        ok(pots > 0 && jaw_only > 0 && bad == 0,
           "played at a corner past its own jaws: touched, and never a bank", m);
    }
    {   /* off the far side rail and back into a corner on the near side */
        CueRules r; fresh(&r);
        for (int i = 0; i < NB; i++) B[i].on = 0;
        const float hl = W.play_x, hw = W.play_z;
        int cnear = -1;                                       /* +x, -z corner */
        for (int p = 0; p < W.npocket; p++)
            if (W.pocket[p].x > 0.5f*hl && W.pocket[p].z < -0.5f*hw) cnear = p;
        int banked = 0, pots = 0;
        for (int s2 = 0; s2 < 40 && !banked; s2++) {
            CueBall *o = &B[1];
            memset(W.rail_hit, 0, sizeof W.rail_hit);
            o->on = 1; o->pocket = 0; o->drop = 0.0f;
            o->pos = v3(hl * 0.3f, B[0].pos.y, -hw + 0.25f);
            o->vel = v3(0,0,0); o->w = v3(0,0,0);
            cue_phys_shot_begin(&W);
            /* mirror the corner in the far rail and aim at the image */
            const float an = atan2f(2.0f*hw - (W.pocket[cnear].z) - o->pos.z,
                                    W.pocket[cnear].x - o->pos.x) + (s2 - 20) * 0.004f;
            cue_phys_strike(&W, o, v3(cosf(an), 0.0f, sinf(an)), 2.2f, 0.0f, 0.0f);
            uint32_t ev = 0;
            for (int it = 0; it < 30000; it++)
                if (!cue_phys_step(&W, B, NB, 1.0f / 240.0f, &ev)) break;
            if (o->on || o->pocket != cnear) continue;
            pots++;
            if (cue_phys_banked(&W, 1, o->pocket)) banked = 1;
        }
        char m[48]; snprintf(m, sizeof m, "%d pots", pots);
        ok(banked, "off the far rail into a near corner: a bank", m);
    }

    printf(s_fail ? "\n%d FAILED\n" : "\nall good\n", s_fail);
    return s_fail != 0;
}
