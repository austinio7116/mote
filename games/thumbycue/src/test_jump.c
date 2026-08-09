/*
 * ThumbyCue — the jump shot, tested against the physics directly.
 *
 * Two claims are being made and neither is safe to assert without checking.
 *
 * The first is that A JUMP CLEARS A BALL: the cue ball leaves the bed, passes
 * over an object ball standing in its path, comes down the far side and settles.
 * That is the shot; if it does not happen there is no feature.
 *
 * The second matters more. THE PLANAR GAME MUST BE UNTOUCHED. Every shot anyone
 * has learned was tuned on physics where the ball never leaves the cloth, and
 * the character of all of it — draw, follow, stun — comes from cloth friction
 * turning slide into roll. A ball that hops for a millisecond stops feeling
 * friction for that millisecond, so an unintended hop does not merely look
 * wrong, it quietly makes every shot in the game behave differently. The
 * deadband exists to make that impossible, and this proves it by playing the
 * same shots through both entry points and requiring the tables to be
 * IDENTICAL, not similar.
 */
#include "cue_physics.h"
#include "cue_table.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int s_fail;
static void ok(int cond, const char *what, const char *detail) {
    if (!cond) { s_fail++; printf("  FAIL %s%s%s\n", what, detail ? " — " : "", detail ? detail : ""); }
    else printf("  ok   %s\n", what);
}

/* Run a table to rest, reporting the highest the cue ball ever got and whether
 * any ball–ball contact happened on the way. */
typedef struct { float peak; int steps; int hit; float land_x; } Run;

static Run settle(CueWorld *w, CueBall *b, int n) {
    Run r = { 0.0f, 0, 0, 0.0f };
    int was_up = 0;
    for (; r.steps < 20000; r.steps++) {
        uint32_t ev = 0;
        int moving = cue_phys_step(w, b, n, 1.0f / 240.0f, &ev);
        if (ev & CUE_EV_BALL_HIT) r.hit = 1;
        float h = b[0].pos.y - w->R;
        if (h > r.peak) r.peak = h;
        if (h > 1e-4f) was_up = 1;
        else if (was_up && r.land_x == 0.0f) r.land_x = b[0].pos.x;
        if (!moving) break;
    }
    return r;
}

/* THE JUMP ONLY, with the roll-out left off. A cue ball that clears a ball then
 * runs into a 0.96-restitution cushion comes back and hits the very ball it
 * jumped, thirty centimetres and two seconds later — which is a correct
 * outcome and says nothing about the jump. The first version of this test
 * asserted on where things finished and failed on exactly that. Stop when the
 * cue ball lands, and everything measured is the flight. */
static Run fly(CueWorld *w, CueBall *b, int n) {
    Run r = { 0.0f, 0, 0, 0.0f };
    int was_up = 0;
    for (; r.steps < 20000; r.steps++) {
        uint32_t ev = 0;
        cue_phys_step(w, b, n, 1.0f / 480.0f, &ev);
        if (ev & CUE_EV_BALL_HIT) r.hit = 1;
        float h = b[0].pos.y - w->R;
        if (h > r.peak) r.peak = h;
        if (h > 1e-4f) was_up = 1;
        else if (was_up) { r.land_x = b[0].pos.x; break; }
    }
    return r;
}

static unsigned table_hash(const CueBall *b, int n) {
    unsigned h = 2166136261u;
    for (int i = 0; i < n; i++) {
        int v[4] = { b[i].on,
                     (int)(b[i].pos.x * 1e6f),
                     (int)(b[i].pos.y * 1e6f),
                     (int)(b[i].pos.z * 1e6f) };
        for (int k = 0; k < 4; k++) { h ^= (unsigned)v[k]; h *= 16777619u; }
    }
    return h;
}

static void rack2(const CueTable *t, CueBall *b, float gap) {
    memset(b, 0, sizeof(CueBall) * 2);
    for (int i = 0; i < 2; i++) {
        b[i].on = 1; b[i].id = (uint8_t)i;
        b[i].orient = m3_identity();
        b[i].pos.y = t->R;
    }
    b[0].pos.x = -0.5f; b[0].pos.z = 0.0f;   /* cue ball */
    b[1].pos.x = -0.5f + gap; b[1].pos.z = 0.0f;   /* the ball in the way */
}

int main(void) {
    CueTable t;
    CueWorld w;
    cue_table_init(&t, CUE_GAME_UK8);
    cue_table_build_world(&t, &w);
    CueBall b[2];
    const Vec3 dir = { 1.0f, 0.0f, 0.0f };

    printf("jump shot\n");

    /* ---- 1. the planar game is bit-for-bit unchanged --------------------
     * The same stroke through strike_elev and through strike_jump with vy = 0,
     * over a full roll-out with a ball–ball collision and cushions in it. If
     * these differ at all, every shot in the game has moved. */
    {
        rack2(&t, b, 0.30f);
        cue_phys_strike_elev(&w, &b[0], dir, 3.0f, 0.15f, -0.25f, 0.20f);
        Run ra = settle(&w, b, 2);
        unsigned a = table_hash(b, 2);

        rack2(&t, b, 0.30f);
        cue_phys_strike_jump(&w, &b[0], dir, 3.0f, 0.15f, -0.25f, 0.20f, 0.0f);
        Run rc = settle(&w, b, 2);
        unsigned c = table_hash(b, 2);

        char d[96];
        snprintf(d, sizeof d, "%08x vs %08x", a, c);
        ok(a == c && ra.steps == rc.steps, "vy=0 is the old physics exactly", d);
    }

    /* ---- 2. an ordinary elevated shot never leaves the bed --------------
     * Elevation on its own is not a jump. This is the cue raised twenty
     * degrees — more than a cushion forces — and struck hard. */
    {
        rack2(&t, b, 0.30f);
        cue_phys_strike_elev(&w, &b[0], dir, 4.0f, 0.0f, 0.20f, 0.35f);
        Run r = settle(&w, b, 2);
        char d[64];
        snprintf(d, sizeof d, "rose %.3f mm", (double)r.peak * 1000.0);
        ok(r.peak == 0.0f, "an elevated shot stays on the cloth", d);
    }

    /* ---- 3. a jump clears a ball ---------------------------------------
     * The object ball sits 30 cm in front. The cue ball has to be over it
     * before it gets there and down again after, and the object ball must not
     * have moved. */
    {
        rack2(&t, b, 0.30f);
        float zx = b[1].pos.x, zz = b[1].pos.z;
        cue_phys_strike_jump(&w, &b[0], dir, 4.0f, 0.0f, 0.30f, 0.79f, 1.10f);
        Run r = fly(&w, b, 2);
        char d[96];
        snprintf(d, sizeof d, "rose %.1f mm, a ball is %.1f",
                 (double)r.peak * 1000.0, (double)(2.0f * w.R) * 1000.0);
        ok(r.peak > 2.0f * w.R, "the cue ball clears a full ball's height", d);
        snprintf(d, sizeof d, "moved %.4f m",
                 (double)sqrtf((b[1].pos.x - zx) * (b[1].pos.x - zx) +
                               (b[1].pos.z - zz) * (b[1].pos.z - zz)));
        ok(!r.hit && fabsf(b[1].pos.x - zx) < 1e-4f && fabsf(b[1].pos.z - zz) < 1e-4f,
           "it passes over without touching it", d);
        snprintf(d, sizeof d, "landed at x = %.3f, the ball is at %.3f",
                 (double)r.land_x, (double)zx);
        ok(r.land_x > zx + w.R, "it lands past the ball it jumped", d);
        snprintf(d, sizeof d, "y = %.5f, R = %.5f", (double)b[0].pos.y, (double)w.R);
        ok(fabsf(b[0].pos.y - w.R) < 1e-5f, "and comes down flat", d);
    }

    /* ---- 4. a ball on the cloth still gets hit --------------------------
     * The height gate must not make the table transparent: same shot, no jump,
     * and the object ball has to move. */
    {
        rack2(&t, b, 0.30f);
        cue_phys_strike_jump(&w, &b[0], dir, 3.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        Run r = settle(&w, b, 2);
        ok(r.hit, "a grounded ball is still collided with",
           r.hit ? NULL : "no ball-ball contact at all");
    }

    /* ---- 5. it does not chatter ----------------------------------------
     * A jumped ball takes a few diminishing hops and stops. If the settle
     * threshold is wrong it micro-bounces for thousands of substeps, and every
     * bounce suspends the cloth friction that makes shots behave. */
    {
        rack2(&t, b, 1.20f);
        cue_phys_strike_jump(&w, &b[0], dir, 3.0f, 0.0f, 0.30f, 0.79f, 0.90f);
        Run r = settle(&w, b, 2);
        char d[64];
        snprintf(d, sizeof d, "%d steps to rest", r.steps);
        ok(r.steps < 4000, "the table settles in reasonable time", d);
        ok(fabsf(b[0].pos.y - w.R) < 1e-5f, "and settles flat", NULL);
    }

    /* ---- 6. a ball cannot leave the world ------------------------------
     * Clearing a cushion is the point of a jump; there is nothing beyond one
     * to stop the ball, so without a boundary a hard jump off a rail rolls to
     * infinity and the shot never settles. It has to end up off the table
     * instead — which the game already understands, because that is what a
     * potted ball looks like to it. */
    {
        memset(b, 0, sizeof b);
        for (int i = 0; i < 2; i++) {
            b[i].on = 1; b[i].id = (uint8_t)i;
            b[i].orient = m3_identity(); b[i].pos.y = t.R;
        }
        b[0].pos.x = t.half_len - 0.10f;   /* right against the top cushion */
        b[1].pos.x = -0.5f;                /* out of the way */
        /* Straight at the rail, steeply, hard: over the cushion and gone. */
        cue_phys_strike_jump(&w, &b[0], dir, 5.0f, 0.0f, 0.35f, 0.90f, 2.0f);
        Run r = settle(&w, b, 2);
        char d[80];
        snprintf(d, sizeof d, "%d steps, finished at x = %.2f",
                 r.steps, (double)b[0].pos.x);
        ok(r.steps < 20000, "a ball flung off the table still settles", d);
        ok(!b[0].on, "and is out of play rather than rolling for ever", d);
    }

    printf(s_fail ? "\nFAILED (%d)\n" : "\nPASSED\n", s_fail);
    return s_fail ? 1 : 0;
}
