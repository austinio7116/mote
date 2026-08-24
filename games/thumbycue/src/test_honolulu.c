/*
 * HONOLULU — a straight-in pot scores nothing.
 *
 * Every scoring ball has to arrive off a bank, off a kick, or through another
 * ball. The same pot is a point or a spotted ball and a lost visit depending on
 * nothing but how it got there, so that is what every case here changes.
 */
#include "cue_physics.h"
#include "cue_rules.h"
#include "cue_table.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

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
    cue_table_init(&T, CUE_GAME_HONOLULU);
    cue_table_build_world(&T, &W);
    NB = cue_table_rack(&T, B);
    cue_rules_init(r, &T, 0);
    r->break_shot = 0;
    memset(W.rails, 0, sizeof W.rails);
    memset(W.balls_hit, 0, sizeof W.balls_hit);
    memset(W.hit_by_cue, 0, sizeof W.hit_by_cue);
    W.ntouch = 0;
}
static int idx_of(int id) {
    for (int i = 1; i < NB; i++) if (B[i].id == id) return i;
    return -1;
}
/* one stroke, told exactly how the ball got there. `cue` says the white
 * touched it, which is what separates a combination from a straight pot. */
static void shot2(CueRules *r, int id, int rails, int hits, int cue, int kick) {
    const int i = idx_of(id);
    W.ntouch = 0;
    if (kick) { W.touch[W.ntouch].what = CUE_TOUCH_CUSHION; W.ntouch++; }
    W.touch[W.ntouch].what = CUE_TOUCH_BALL;
    W.touch[W.ntouch].id = (unsigned char)id;
    W.touch[W.ntouch].idx = (unsigned char)(i > 0 ? i : 0); W.ntouch++;
    if (i > 0) {
        W.rails[i] = (unsigned char)rails;
        W.balls_hit[i] = (unsigned char)hits;
        W.hit_by_cue[i] = (unsigned char)cue;
        B[i].on = 0;
    }
    int p[1] = { id };
    cue_rules_resolve(r, B, NB, &W, id, 0, 1, p, 1);
}
/* the ordinary case: the white struck it and nothing else did */
static void shot(CueRules *r, int id, int rails, int hits, int kick) {
    shot2(r, id, rails, hits, 1, kick);
}
/* A CAROM: the white touched `first` and came off it onto `id`, which drops.
 * The white's own log is the only thing that records that order, and the order
 * is the whole of it — `id` has one contact, exactly as a straight pot does. */
static void shot_off(CueRules *r, int id, int first) {
    const int i = idx_of(id), f = idx_of(first);
    W.ntouch = 0;
    W.touch[0].what = CUE_TOUCH_BALL;
    W.touch[0].id = (unsigned char)first; W.touch[0].idx = (unsigned char)f;
    W.touch[1].what = CUE_TOUCH_BALL;
    W.touch[1].id = (unsigned char)id;    W.touch[1].idx = (unsigned char)i;
    W.ntouch = 2;
    W.rails[i] = 0; W.balls_hit[i] = 1; W.hit_by_cue[i] = 1; B[i].on = 0;
    W.balls_hit[f] = 1; W.hit_by_cue[f] = 1;
    int p[1] = { id };
    cue_rules_resolve(r, B, NB, &W, first, 0, 1, p, 1);
}


/* ---- THE SHOTS, PLAYED --------------------------------------------------- */

/* Roll everything to a stop and hand the result to the rules the way the host
 * does: what went down, whether the white did, whether anything found a rail. */
static void settle_and_resolve(CueRules *r, CueWorld *w, CueBall *b, int n,
                               const int *was_on) {
    uint32_t ev = 0;
    for (int it = 0; it < 20000; it++)
        if (!cue_phys_step(w, b, n, 1.0f / 240.0f, &ev)) break;
    int potted[CUE_MAX_BALLS], np = 0, cushion = 0;
    for (int i = 1; i < n; i++)
        if (was_on[i] && !b[i].on && b[i].pocket != CUE_OFF_TABLE)
            potted[np++] = b[i].id;
    for (int i = 0; i < CUE_MAX_BALLS; i++) if (w->rails[i]) cushion = 1;
    for (int i = 0; i < w->ntouch; i++)
        if (w->touch[i].what == CUE_TOUCH_CUSHION) cushion = 1;
    cue_rules_resolve(r, b, n, w, w->first_hit, !b[0].on, cushion, potted, np);
}

/* Three balls on the cloth and nothing else, so what scores is not in doubt. */
static void three_balls(CueTable *t, CueWorld *w, CueBall *b, int *n) {
    cue_table_init(t, CUE_GAME_HONOLULU);
    cue_table_build_world(t, w);
    *n = cue_table_rack(t, b);
    for (int i = 1; i < *n; i++) b[i].on = 0;
    b[1].on = 1; b[2].on = 1;
    for (int i = 0; i < 3; i++) {
        b[i].vel = v3(0, 0, 0); b[i].w = v3(0, 0, 0);
        b[i].pocket = 0; b[i].drop = 0;
    }
}

static void real_shots(void) {
    /* THE COMBINATION: white into A, A into B, B down. Everything on one line
     * into a corner, so nothing can reach a cushion and only the combination
     * can be what scored it. */
    {   CueTable t; CueWorld w; CueBall b[CUE_MAX_BALLS]; int n;
        three_balls(&t, &w, b, &n);
        const Vec3 pp = w.pocket[0], ax = w.paxis[0];
        const Vec3 in = v3(-ax.x, 0, -ax.z);
        b[2].pos = v3(pp.x + in.x*0.30f, t.R, pp.z + in.z*0.30f);
        b[1].pos = v3(pp.x + in.x*0.60f, t.R, pp.z + in.z*0.60f);
        b[0].pos = v3(pp.x + in.x*1.00f, t.R, pp.z + in.z*1.00f);
        int was_on[CUE_MAX_BALLS];
        for (int i = 0; i < n; i++) was_on[i] = b[i].on;
        CueRules r; cue_rules_init(&r, &t, 0); r.break_shot = 0;
        cue_phys_shot_begin(&w);
        cue_phys_strike(&w, &b[0], ax, 2.2f, 0.0f, 0.0f);
        settle_and_resolve(&r, &w, b, n, was_on);
        ok(!b[2].on && w.hit_by_cue[2] == 0 && w.rails[2] == 0,
           "played: the white never touched the ball that dropped", r.msg);
        ok(r.score[0] == 1 && r.turn == 0,
           "...so the combination scores and keeps the table", r.msg);
    }

    /* THE CAROM: white clips A and goes on to pot B. Built rather than aimed —
     * the white leaves a contact along the TANGENT, square to the line of
     * centres, so A is placed square to the line the white has to leave on. */
    {   CueTable t; CueWorld w; CueBall b[CUE_MAX_BALLS]; int n;
        three_balls(&t, &w, b, &n);
        const Vec3 pp = w.pocket[0], ax = w.paxis[0];
        const Vec3 in = v3(-ax.x, 0, -ax.z);
        const Vec3 pr = v3(-in.z, 0, in.x);
        const float R2 = 2.0f * t.R, k = 0.15f, s = 0.06f;
        const Vec3 Bp = v3(pp.x + in.x*0.30f, t.R, pp.z + in.z*0.30f);
        const Vec3 gh = v3(Bp.x - ax.x*R2, t.R, Bp.z - ax.z*R2);
        const Vec3 Q  = v3(gh.x - ax.x*s,   t.R, gh.z - ax.z*s);
        const float ul = sqrtf((ax.x + pr.x*k)*(ax.x + pr.x*k) +
                               (ax.z + pr.z*k)*(ax.z + pr.z*k));
        const Vec3 u  = v3((ax.x + pr.x*k)/ul, 0, (ax.z + pr.z*k)/ul);
        b[2].pos = Bp;
        b[1].pos = v3(Q.x + pr.x*R2, t.R, Q.z + pr.z*R2);
        b[0].pos = v3(Q.x - u.x*0.45f, t.R, Q.z - u.z*0.45f);
        int was_on[CUE_MAX_BALLS];
        for (int i = 0; i < n; i++) was_on[i] = b[i].on;
        CueRules r; cue_rules_init(&r, &t, 0); r.break_shot = 0;
        cue_phys_shot_begin(&w);
        cue_phys_strike(&w, &b[0], u, 2.4f, 0.0f, 0.0f);
        settle_and_resolve(&r, &w, b, n, was_on);
        /* The point of the case: one contact, by the white, and no rail — the
         * exact fingerprint of a straight pot, which is why it was refused. */
        ok(!b[2].on && w.hit_by_cue[2] == 1 && w.balls_hit[2] == 1 &&
           w.rails[2] == 0,
           "played: the potted ball has ONE contact and no rail, as a straight "
           "pot has", r.msg);
        ok(r.score[0] == 1 && r.turn == 0,
           "...and it still scores, because the white came off another ball "
           "first", r.msg);
    }
}

int main(void) {
    printf("honolulu\n");

    {   CueRules r; fresh(&r);
        ok(r.target_score == 8, "first to eight", "");
    }

    /* ---- the same pot, four ways ---- */
    {   CueRules r; fresh(&r);
        shot(&r, 3, 0, 1, 0);          /* cue ball straight onto it, straight in */
        ok(r.score[0] == 0 && r.respot == 1 && r.turn == 1,
           "straight in: no score, spotted, visit over", r.msg);
    }
    {   CueRules r; fresh(&r);
        shot(&r, 3, 1, 1, 0);          /* it banked */
        ok(r.score[0] == 1 && r.turn == 0, "off a bank it scores", r.msg);
    }
    {   CueRules r; fresh(&r);
        shot(&r, 3, 0, 1, 1);          /* the cue ball found a rail first */
        ok(r.score[0] == 1 && r.turn == 0, "off a kick it scores", r.msg);
    }
    {   /* THE CAROM, WHICH THIS TEST USED TO STATE BACKWARDS.
         *
         * It asked for a ball the white had struck that then touched something
         * else — two contacts — and called that a carom. It is not one, and the
         * rules agreed with the test, so the shot Mark actually plays was
         * refused every time: the white glances off one ball and pots this with
         * the ONLY contact it has, which is one contact, the same count as a
         * straight pot. What separates them is the order in the white's log. */
        CueRules r; fresh(&r);
        shot_off(&r, 3, 9);
        ok(r.score[0] == 1 && r.turn == 0,
           "the white came off another ball onto it: a carom, and it scores",
           r.msg);
    }
    {   /* ...and the shot the old case described scores nothing, which is the
         * other half of the same mistake: a ball potted straight that brushes
         * another on its way to the hole went in straight. */
        CueRules r; fresh(&r);
        shot(&r, 3, 0, 2, 0);
        ok(r.score[0] == 0 && r.respot == 1,
           "a straight pot that clipped another ball on the way is still straight",
           r.msg);
    }
    {   /* A COMBINATION, which is the case Mark found: the white strikes one
         * ball, that ball strikes this one, and this one drops. It has exactly
         * ONE contact — the same count as a ball the white potted itself — so
         * counting contacts read it as a straight pot and scored nothing. What
         * tells them apart is that the WHITE never touched it. */
        CueRules r; fresh(&r);
        shot2(&r, 3, 0, 1, 0, 0);
        ok(r.score[0] == 1 && r.turn == 0,
           "a ball the white never touched came off a combination", r.msg);
    }

    /* ---- a cushion AFTER the first ball is not a kick ---- */
    {   CueRules r; fresh(&r);
        const int i = idx_of(3);
        W.ntouch = 0;
        W.touch[W.ntouch].what = CUE_TOUCH_BALL; W.touch[W.ntouch].id = 3; W.ntouch++;
        W.touch[W.ntouch].what = CUE_TOUCH_CUSHION; W.ntouch++;
        W.rails[i] = 0; W.balls_hit[i] = 1; W.hit_by_cue[i] = 1; B[i].on = 0;
        int p[1] = { 3 };
        cue_rules_resolve(&r, B, NB, &W, 3, 0, 1, p, 1);
        ok(r.score[0] == 0,
           "the cue ball's rail must come BEFORE the ball, or it is no kick",
           r.msg);
    }

    /* ---- a legal score keeps the table, whatever else drops -------------- */
    {   CueRules r; fresh(&r);
        const int ia = idx_of(3), ib = idx_of(9);
        W.ntouch = 0;
        W.touch[0].what = CUE_TOUCH_BALL; W.touch[0].id = 3; W.ntouch = 1;
        W.rails[ia] = 1; W.balls_hit[ia] = 1; W.hit_by_cue[ia] = 1; B[ia].on = 0;
        W.rails[ib] = 0; W.balls_hit[ib] = 1; W.hit_by_cue[ib] = 1; B[ib].on = 0;
        int p[2] = { 3, 9 };
        cue_rules_resolve(&r, B, NB, &W, 3, 0, 1, p, 2);
        ok(r.score[0] == 1, "the banked one still scores", r.msg);
        ok(r.turn == 0,
           "...and you keep the table: the straight one is spotted, not a foul",
           r.msg);
        ok(r.respot == 1 && r.respot_id[0] == 9,
           "...and it is the straight one that goes back", r.msg);
    }

    /* ---- eight wins it, and a straight one cannot ---- */
    {   CueRules r; fresh(&r); r.score[0] = 7;
        shot(&r, 3, 1, 1, 0);
        ok(r.frame_over && r.winner == 0, "eight is the game", r.msg);
    }
    {   CueRules r; fresh(&r); r.score[0] = 7;
        shot(&r, 3, 0, 1, 0);
        ok(!r.frame_over && r.score[0] == 7,
           "a straight one cannot finish it", r.msg);
    }

    /* ---- a straight pot is no score, but it is not a foul ---- */
    {   CueRules r; fresh(&r);
        shot(&r, 3, 0, 1, 0);
        ok(!r.last_foul, "straight in is no score and no foul", r.msg);
    }

    /* ---- AND THE SAME TWO SHOTS PLAYED, rather than described -------------
     *
     * Everything above hands the rules a CueWorld filled in by hand, which is
     * the right way to ask "given this, what do you say" and no way at all to
     * ask "does the table ever produce this". The carom was refused in play for
     * a fortnight while a test called that same shot a pass, because the test
     * was describing a different stroke. So these two are struck. */
    real_shots();

    printf(s_fail ? "\n%d FAILED\n" : "\nall good\n", s_fail);
    return s_fail != 0;
}
