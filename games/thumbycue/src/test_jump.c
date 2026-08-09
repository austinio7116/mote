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
#include "cue_rules.h"

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
        /* Straight at the rail, steeply, hard: over the cushion and gone.
         *
         * IT HAS TO CLEAR THE TOP OF THE CUSHION, and the top is rail_top, not
         * cushion_nose — the nose is the contact line 63.5% of a ball up the
         * front face, and there is another 10 mm of cushion above it before the
         * flat the wood is level with. It also has to be up there BEFORE it
         * arrives, because the ball meets the cushion a radius short of the
         * line, so the clearance is wanted a whole ball early. 3 m/s of lift
         * over that distance is comfortably enough; 2 was not, and the ball
         * bounced back onto the table, which is the correct answer to a stroke
         * that does not clear a cushion. */
        cue_phys_strike_jump(&w, &b[0], dir, 5.0f, 0.0f, 0.35f, 0.90f, 3.0f);
        Run r = settle(&w, b, 2);
        char d[80];
        snprintf(d, sizeof d, "%d steps, finished at x = %.2f",
                 r.steps, (double)b[0].pos.x);
        ok(r.steps < 20000, "a ball flung off the table still settles", d);
        ok(!b[0].on, "and is out of play rather than rolling for ever", d);

        /* AND THE OTHER HALF OF IT: not enough lift means it comes back. A
         * cushion that can be jumped is a cushion that can be jumped BY
         * MISTAKE unless failing to clear it is a bounce. */
        memset(b, 0, sizeof b);
        for (int i = 0; i < 2; i++) {
            b[i].on = 1; b[i].id = (uint8_t)i;
            b[i].orient = m3_identity(); b[i].pos.y = t.R;
        }
        b[0].pos.x = t.half_len - 0.10f;
        b[1].pos.x = -0.5f;
        cue_phys_strike_jump(&w, &b[0], dir, 5.0f, 0.0f, 0.35f, 0.90f, 1.0f);
        settle(&w, b, 2);
        snprintf(d, sizeof d, "finished at x = %.3f, cushion at %.3f",
                 (double)b[0].pos.x, (double)t.half_len);
        ok(b[0].on && b[0].pos.x < t.half_len,
           "a shot that does NOT clear the cushion bounces off it", d);
    }

    /* ---- 7. a jump with SCREW on it ------------------------------------
     * Whether the ball leaves the bed is vertical momentum and nothing else.
     * Where on the face the tip lands decides the spin it carries, not whether
     * it flies — so cueing down steeply on the LOWER half is a jump with
     * backspin, which is a real shot. An earlier version refused to jump at
     * all unless the tip was above centre, which is wrong about the physics. */
    {
        rack2(&t, b, 0.30f);
        cue_phys_strike_jump(&w, &b[0], dir, 4.0f, 0.0f, -0.30f, 0.79f, 1.10f);
        Run r = fly(&w, b, 2);
        char d[96];
        snprintf(d, sizeof d, "rose %.1f mm, w.z = %+.1f rad/s",
                 (double)r.peak * 1000.0, (double)b[0].w.z);
        ok(r.peak > 2.0f * w.R, "a jump struck below centre still flies", d);
        /* Backspin about the axis across the travel: the ball is turning
         * backwards relative to a rolling one. */
        ok(b[0].w.z > 0.0f, "and carries screw off the tip", d);
    }

    /* ---- 8. the rules: snooker forbids a jump, pool does not -------------- */
    {
        CueTable st; CueWorld sw; CueRules R;
        cue_table_init(&st, CUE_GAME_SNK15);
        cue_table_build_world(&st, &sw);
        CueBall sb[22];
        int sn = cue_table_rack(&st, sb);
        cue_rules_init(&R, &st, 0);
        int before = R.score[1];
        R.jumped = 1;
        cue_rules_resolve(&R, sb, sn, &sw, 1 /* a red */, 0, 1, NULL, 0);
        char d[96];
        snprintf(d, sizeof d, "opponent %d -> %d, \"%s\"", before, R.score[1], R.msg);
        ok(R.last_foul && R.score[1] > before, "snooker: a jump is a foul", d);
        ok(R.jumped == 0, "and the flag does not foul the next shot too", NULL);

        CueRules P;
        cue_rules_init(&P, &t, 0);
        P.jumped = 1;
        CueBall pb[16];
        int pn = cue_table_rack(&t, pb);
        cue_rules_resolve(&P, pb, pn, &w, 1, 0, 1, NULL, 0);
        ok(!P.last_foul, "pool: a jump is perfectly legal",
           P.last_foul ? P.msg : NULL);
    }

    /* ---- 9. a ball driven off the table is a foul, not a pot ------------- */
    {
        CueRules P;
        cue_rules_init(&P, &t, 0);
        CueBall pb[16];
        int pn = cue_table_rack(&t, pb);
        int one = 1;
        /* Same shot twice: the ball POTTED, then the ball OFF THE TABLE. */
        cue_rules_resolve(&P, pb, pn, &w, 1, 0, 1, &one, 1);
        int pot_foul = P.last_foul;

        CueRules Q;
        cue_rules_init(&Q, &t, 0);
        pn = cue_table_rack(&t, pb);
        Q.n_off = 1;
        cue_rules_resolve(&Q, pb, pn, &w, 1, 0, 1, &one, 1);
        char d[96];
        snprintf(d, sizeof d, "potted: foul=%d; off the table: foul=%d \"%s\"",
                 pot_foul, Q.last_foul, Q.msg);
        ok(!pot_foul && Q.last_foul, "off the table fouls where potting did not", d);
        ok(Q.n_off == 0, "and does not foul the next shot too", NULL);
    }

    /* ---- 10. WPBSA Definition 20, and its three exceptions ---------------
     *
     * "A jump shot is made when the cue-ball passes over any part of an object
     * ball, whether hitting it in the process or not, except (a)...(c)."
     *
     * The offence is passing over a ball, NOT leaving the bed — which is what
     * this used to test for, and which fouled every one of the exceptions plus
     * a hop over empty cloth. */
    {
        /* A jump over EMPTY CLOTH is not a jump shot at all. */
        rack2(&t, b, 2.00f);           /* the other ball miles away */
        w.first_hit = -1; w.first_hit_idx = -1;
        w.jump_over = 0; w.jmp_pending = 0; w.jmp_idx = -1;
        w.jmp_hit_it = 0; w.jmp_bounced = 0;
        cue_phys_strike_jump(&w, &b[0], dir, 3.0f, 0.0f, 0.30f, 0.79f, 1.00f);
        fly(&w, b, 2);
        ok(!w.jump_over, "a hop over open cloth is not a jump shot",
           w.jump_over ? "fouled anyway" : NULL);

        /* Over a ball, having touched nothing first: the offence itself. */
        rack2(&t, b, 0.30f);
        w.first_hit = -1; w.first_hit_idx = -1;
        w.jump_over = 0; w.jmp_pending = 0; w.jmp_idx = -1;
        w.jmp_hit_it = 0; w.jmp_bounced = 0;
        cue_phys_strike_jump(&w, &b[0], dir, 4.0f, 0.0f, 0.30f, 0.79f, 1.10f);
        fly(&w, b, 2);
        ok(w.jump_over, "over a ball before touching anything IS a jump shot",
           w.jump_over ? NULL : "not detected");

        /* Exception (b): jumps, hits the ball, lands NOT beyond it. A short
         * launch straight into the ball rather than over it. */
        rack2(&t, b, 0.16f);
        w.first_hit = -1; w.first_hit_idx = -1;
        w.jump_over = 0; w.jmp_pending = 0; w.jmp_idx = -1;
        w.jmp_hit_it = 0; w.jmp_bounced = 0;
        cue_phys_strike_jump(&w, &b[0], dir, 2.0f, 0.0f, 0.20f, 0.60f, 0.55f);
        Run rb = fly(&w, b, 2);
        char d[110];
        snprintf(d, sizeof d, "landed x = %.3f, ball at x = %.3f, over=%d",
                 (double)rb.land_x, (double)b[1].pos.x, w.jump_over);
        ok(!w.jump_over, "exception (b): into the ball, not past it — legal", d);
    }

    /* ---- 11. exception (a): hit one ball, then jump over another ---------- */
    {
        CueBall c3[3];
        memset(c3, 0, sizeof c3);
        for (int i = 0; i < 3; i++) {
            c3[i].on = 1; c3[i].id = (uint8_t)i;
            c3[i].orient = m3_identity(); c3[i].pos.y = t.R;
        }
        c3[0].pos.x = -0.60f;                      /* white */
        c3[1].pos.x = -0.60f + 0.10f;              /* struck first, close in */
        c3[2].pos.x = -0.60f + 0.42f;              /* jumped over, further on */
        w.first_hit = -1; w.first_hit_idx = -1;
        w.jump_over = 0; w.jmp_pending = 0; w.jmp_idx = -1;
        w.jmp_hit_it = 0; w.jmp_bounced = 0;
        /* Launched, but it meets ball 1 almost at once and is over ball 2 after. */
        cue_phys_strike_jump(&w, &c3[0], dir, 4.0f, 0.0f, 0.30f, 0.79f, 1.10f);
        Run r = fly(&w, c3, 3);
        char d[110];
        snprintf(d, sizeof d, "first_hit=%d, over=%d (id %d), rose %.1f mm",
                 w.first_hit, w.jump_over, w.jump_over_id, (double)r.peak * 1000.0);
        ok(w.first_hit == 1, "it struck the near ball first", d);
        ok(!w.jump_over, "exception (a): over ANOTHER ball after a contact — legal", d);
    }

    /* ---- 12. the rail is a surface, not a cliff -------------------------
     *
     * A ball that clears a cushion used to be deleted the moment it passed the
     * line, which removes a shot that is still happening. It can land on the
     * rail, run along it, and come back down — or sit up there, in which case
     * ten seconds is the referee's patience. */
    {
        /* Lobbed at the side cushion with enough to clear it but not enough to
         * carry the whole rail. */
        memset(b, 0, sizeof b);
        for (int i = 0; i < 2; i++) {
            b[i].on = 1; b[i].id = (uint8_t)i;
            b[i].orient = m3_identity(); b[i].pos.y = t.R;
        }
        b[0].pos.x = 0.0f; b[0].pos.z = t.half_wid - 0.12f;
        b[1].pos.x = -0.8f; b[1].pos.z = 0.0f;          /* out of the way */
        Vec3 side = { 0.0f, 0.0f, 1.0f };
        cue_phys_strike_jump(&w, &b[0], side, 2.2f, 0.0f, 0.30f, 0.75f, 0.95f);

        int on_rail = 0, back_on_cloth = 0, gone = 0;
        for (int i = 0; i < 40000; i++) {
            cue_phys_step(&w, b, 2, 1.0f / 480.0f, NULL);
            if (!b[0].on) { gone = 1; break; }
            float az = b[0].pos.z < 0 ? -b[0].pos.z : b[0].pos.z;
            if (az > w.play_z && b[0].pos.y > w.R + 1e-4f) on_rail = 1;
            else if (on_rail && az <= w.play_z) { back_on_cloth = 1; break; }
            if (!cue_phys_moving(&w, b, 2) && on_rail) break;
        }
        char d[128];
        snprintf(d, sizeof d, "on the rail %d, back on the cloth %d, gone %d, "
                 "finished (%.3f,%.3f,%.3f)", on_rail, back_on_cloth, gone,
                 (double)b[0].pos.x, (double)b[0].pos.y, (double)b[0].pos.z);
        ok(on_rail, "a ball can get up onto the rail at all", d);
        /* Leaving is fine — it can run the width of the rail and off the far
         * side, which is what this one does. What must NOT happen is being
         * deleted AT the cushion line, and having been up on the rail at all
         * is the proof it was not. */
        {
            float az = b[0].pos.z < 0 ? -b[0].pos.z : b[0].pos.z;
            ok(on_rail && (!gone || az > w.play_z),
               "and is not deleted the moment it clears the cushion", d);
        }
    }

    /* ---- 13. but not for ever ------------------------------------------- */
    {
        memset(b, 0, sizeof b);
        for (int i = 0; i < 2; i++) {
            b[i].on = 1; b[i].id = (uint8_t)i;
            b[i].orient = m3_identity(); b[i].pos.y = t.R;
        }
        /* Parked ON the rail, at rest, which is the case that must time out. */
        b[0].pos.z = t.half_wid + 0.02f;
        b[0].pos.y = w.rail_top + t.R;
        b[1].pos.x = -0.8f;
        float secs = 0.0f;
        for (int i = 0; i < 40000 && b[0].on; i++) {
            cue_phys_step(&w, b, 2, 1.0f / 240.0f, NULL);
            secs += 1.0f / 240.0f;
        }
        char d[80];
        snprintf(d, sizeof d, "removed after %.1f s", (double)secs);
        /* Ten seconds of SIMULATED time; this loop's own tally of wall dt is a
         * different clock and runs a little ahead of it. The number is a
         * referee's patience, not a tolerance. */
        ok(!b[0].on && secs > 6.0f && secs < 14.0f,
           "a ball stuck on the rail leaves after about ten seconds", d);
    }

    printf(s_fail ? "\nFAILED (%d)\n" : "\nPASSED\n", s_fail);
    return s_fail ? 1 : 0;
}
