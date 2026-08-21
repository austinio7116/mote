/*
 * ThumbyCue — a ball must never be left sitting on a pocket.
 *
 * Reported from play: on Russian pyramid a ball would reach a corner, roll out
 * over the hole and STOP there, at cloth height, without dropping. The shot
 * never settled, so the frame could not continue and the game was over as far
 * as the player was concerned. It is the worst class of bug in the build — not
 * a wrong answer, no answer at all — and it deserves the only kind of test that
 * can prove it gone.
 *
 * WHY IT HAPPENED, because the test is shaped by it. The cloth turns over the
 * edge of the slate in a quarter circle of radius lip_d, and a ball riding that
 * turn has its centre on an arc of radius lip_d + R. It is released — nothing
 * under it at last — only once it is that far out over the cut. The deepest a
 * ball can EVER get is the drop centre, and on a pyramid corner that point was
 * 0.9 mm short of the release. A 72 mm mouth around a 67 mm ball simply has no
 * room for the roll the table asked for, and nothing had ever checked.
 *
 * So there are two tests here and they are not the same test:
 *
 *   THE INVARIANT, on every pocket of every table, arithmetic only. The roll
 *   must be shallow enough that a ball clears it before reaching the deepest
 *   point it can reach. This is the condition that was violated, stated
 *   directly, and it runs in microseconds — so it will still be here, and still
 *   cheap, when somebody adds the thirteenth table.
 *
 *   AND THE BEHAVIOUR, by firing balls at pockets and watching the clock. The
 *   invariant is a claim about a formula; this is a claim about the game. If
 *   the drop is ever rewritten so that the formula no longer governs it, the
 *   invariant would keep passing and this would not.
 */
#include "cue_table.h"
#include "cue_physics.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int fails;
static void ck(int cond, const char *what) {
    if (!cond) { printf("  FAIL  %s\n", what); fails++; }
}

static const char *NAME[CUE_GAME_COUNT] = {
    "uk8","us8","us9","cn8","snk15","snk10","snk6",
    "straight","pyramid","pyramid7","billiards","barbilliards",
    "golf","us10","paul"
};

/* ---- 1. the invariant ---------------------------------------------------- */

static void invariant(void) {
    printf("the roll is shallower than the pocket is deep\n");
    float worst = 1e9f; const char *worst_t = "?"; int worst_p = -1;
    for (int k = 0; k < CUE_GAME_COUNT; k++) {
        CueTable t; cue_table_init(&t, (CueGameKind)k);
        static CueWorld w; cue_table_build_world(&t, &w);
        for (int p = 0; p < w.npocket; p++) {
            /* How far out over the cut a ball can get at all: the drop centre
             * is the deepest point, because that is where the gather takes it
             * and the throat wall will not let it past. */
            float avail = cue_phys_cut_out(&w, p, w.drop_c[p].x, w.drop_c[p].z);
            float need  = w.lip_d[p] + t.R;      /* where the roll lets go */
            float slack = avail - need;
            if (slack < worst) { worst = slack; worst_t = NAME[k]; worst_p = p; }
            if (slack <= 0.0f) {
                printf("  FAIL  %s pocket %d: a ball can reach %.4f out but the "
                       "roll holds it to %.4f — short by %.2f mm\n",
                       NAME[k], p, avail, need, -1000.0f * slack);
                fails++;
            }
        }
    }
    printf("  tightest: %s pocket %d, %.2f mm of slack\n",
           worst_t, worst_p, 1000.0f * worst);
    ck(worst > 0.0f, "every pocket on every table lets its own ball go");
}

/* ---- 2. the behaviour ---------------------------------------------------- */

static CueWorld w;
static CueBall  b[1];

/* Roll a ball at a pocket and return how long the table took to stop. The cap
 * is thirty seconds of table time: a real shot is over inside ten, and a ball
 * that hangs never stops at all, so anything near the cap is the bug. */
static float settle_time(const CueTable *t, Vec3 from, Vec3 dir, float speed) {
    memset(b, 0, sizeof b);
    b[0].on = 1; b[0].id = 1; b[0].r = t->R;
    b[0].orient.r[0] = v3(1,0,0);
    b[0].orient.r[1] = v3(0,1,0);
    b[0].orient.r[2] = v3(0,0,1);
    b[0].pos = from;
    cue_phys_shot_begin(&w);
    cue_phys_strike(&w, &b[0], dir, speed, 0.0f, 0.0f);
    const float DT = 1.0f / 240.0f;
    float secs = 0.0f; uint32_t ev;
    while (secs < 30.0f) {
        cue_phys_step(&w, b, 1, DT, &ev);
        secs += DT;
        if (!cue_phys_moving(&w, b, 1)) break;
    }
    return secs;
}

/* Every pocket, from straight in front of it, at a spread of paces and with the
 * aim wandering across the mouth — because the hang was not on the middle of
 * the pocket, it was on a ball that arrived at the side and gathered in. */
static void behaviour(CueGameKind k) {
    CueTable t; cue_table_init(&t, k);
    cue_table_build_world(&t, &w);
    int shots = 0, hung = 0, potted = 0; float worst = 0.0f;
    for (int p = 0; p < w.npocket; p++) {
        Vec3 n = w.pmnorm[p];                       /* out of the table */
        Vec3 across = v3(-n.z, 0, n.x);
        for (int si = 0; si < 6; si++) {
            float speed = 0.30f + 0.55f * si;
            for (int ai = -8; ai <= 8; ai++) {
                float off = 0.004f * ai;
                Vec3 from = v3(w.pocket[p].x - n.x * 0.30f + across.x * off, t.R,
                               w.pocket[p].z - n.z * 0.30f + across.z * off);
                if (fabsf(from.x) > t.half_len - t.R) continue;
                if (fabsf(from.z) > t.half_wid - t.R) continue;
                float secs = settle_time(&t, from, n, speed);
                shots++;
                if (!b[0].on) potted++;
                if (secs > worst) worst = secs;
                if (secs > 25.0f) {
                    if (!hung)
                        printf("  FAIL  %s: a ball hung at pocket %d "
                               "(v %.2f, %+.0f mm across) — rest y %.4f\n",
                               NAME[k], p, speed, off * 1000.0f, b[0].pos.y);
                    hung++;
                }
            }
        }
    }
    printf("  %-12s %4d shots, %4d potted, worst settle %.1fs, hung %d\n",
           NAME[k], shots, potted, worst, hung);
    if (hung) fails++;
}

int main(void) {
    invariant();
    printf("\nand no shot leaves the table running\n");
    /* Every table, because the invariant is what makes them safe and this is
     * what checks the invariant is the right one. The pyramids first: they are
     * the ones that failed, and they are the tightest pockets in the build. */
    for (int k = 0; k < CUE_GAME_COUNT; k++) behaviour((CueGameKind)k);
    printf(fails ? "\nFAILED (%d)\n" : "\nPASSED\n", fails);
    return fails ? 1 : 0;
}
