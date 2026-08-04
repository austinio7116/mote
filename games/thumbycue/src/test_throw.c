/*
 * ThumbyCue — how far does the engine throw an object ball off the ghost-ball
 * line, and what does side do to it?
 *
 * This exists because two things were dropped from the port together and each
 * made the other look unnecessary. cue_ai.h says the throw compensation was
 * "intentionally dropped: the engine pots cleanly, so the ghost-ball aim is the
 * true aim" — which is true only because the planner also never used side. Turn
 * side on without the compensation and every sided shot misses by the throw;
 * measured over 60 self-play frames that took the pot rate from 79% to 64%.
 *
 * So: fire the cue ball at an object ball along an EXACT ghost-ball line, with a
 * known cut, speed and tip offset, and measure where the object ball actually
 * goes. No theory, no coefficient of friction picked to make a formula work —
 * the engine's own answer, which is the only one the planner has to aim into.
 *
 *   cc -O2 -I. -I../../../engine/math -o /tmp/test_throw test_throw.c \
 *      cue_table.c cue_physics.c -lm && /tmp/test_throw
 */
#include "cue_table.h"
#include "cue_physics.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEG (180.0f / 3.14159265f)
#define RAD (3.14159265f / 180.0f)

static CueTable T;
static CueWorld W;

/* One measurement. `cut` in degrees, `side` in fractions of R (+ = right
 * english), `speed` m/s, `vert` follow(+)/draw(-).
 *
 * Geometry: the object ball sits at the origin and we WANT it to leave along
 * +x. That fixes the ghost ball at (-2R, 0, 0). The cue ball is placed back
 * along a line making `cut` with +x, so aiming dead at the ghost ball is by
 * construction the perfect ghost-ball aim. Anything the object ball does other
 * than leave along +x is throw.
 *
 * Returns the deviation in degrees, positive meaning thrown toward +z. */
static float measure(float cut, float side, float speed, float vert, int *ok) {
    CueBall b[2];
    memset(b, 0, sizeof b);
    float R = T.R;

    b[1].id = 1; b[1].on = 1;
    b[1].pos = v3(0, 0, 0);
    (void)R;


    Vec3 G = v3(-2.0f * R, 0, 0);
    float d = 0.40f;
    Vec3 C = v3(G.x - d * cosf(cut * RAD), 0, G.z - d * sinf(cut * RAD));
    b[0].id = 0; b[0].on = 1; b[0].pos = C;

    Vec3 aim = v3(G.x - C.x, 0, G.z - C.z);
    float l = sqrtf(aim.x*aim.x + aim.z*aim.z);
    aim.x /= l; aim.z /= l;

    CueWorld w = W;
    w._acc = 0.0f;
    w.first_hit = -1; w.first_hit_idx = -1;
    cue_phys_strike(&w, &b[0], aim, speed, side, vert);

    /* Step until the object ball is clearly separated and rolling, then read its
     * direction. Reading at the instant of contact would catch the impulse
     * mid-resolution; waiting for it to stop would let a cushion answer instead
     * of the collision. Two ball-widths of travel is well clear of both. */
    *ok = 0;
    for (int i = 0; i < 4000; i++) {
        uint32_t ev = 0;
        cue_phys_step(&w, b, 2, 1.0f / 600.0f, &ev);
        float dx = b[1].pos.x, dz = b[1].pos.z;
        if (dx*dx + dz*dz > (4.0f*R)*(4.0f*R)) {
            float sp = sqrtf(b[1].vel.x*b[1].vel.x + b[1].vel.z*b[1].vel.z);
            if (sp < 0.02f) return 0.0f;
            *ok = 1;
            return atan2f(b[1].vel.z, b[1].vel.x) * DEG;
        }
        if (!cue_phys_moving(&w, b, 2)) break;
    }
    return 0.0f;
}

int main(void) {
    int kind = CUE_GAME_SNK15;
    { const char *v = getenv("THROW_GAME"); if (v) kind = atoi(v); }
    cue_table_init(&T, (CueGameKind)kind);
    cue_table_build_world(&T, &W);

    printf("Throw off the ghost-ball line — %s, R = %.4f m\n",
           T.is_snooker ? "snooker" : "pool", T.R);
    printf("Positive = object ball thrown toward +z (the side the cue ball\n"
           "passes on for a positive cut). Degrees.\n\n");

    const float CUTS[]  = { 0, 10, 20, 30, 40, 50, 60, 70 };
    const float SIDES[] = { -0.6f, -0.45f, -0.2f, 0.0f, 0.2f, 0.45f, 0.6f };
    const float SPEEDS[] = { 1.5f, 3.0f, 6.0f };
    const int NC = (int)(sizeof CUTS / sizeof CUTS[0]);
    const int NS = (int)(sizeof SIDES / sizeof SIDES[0]);

    for (int si = 0; si < (int)(sizeof SPEEDS / sizeof SPEEDS[0]); si++) {
        printf("--- %.1f m/s ---\n      cut:", SPEEDS[si]);
        for (int ci = 0; ci < NC; ci++) printf("%8.0f", CUTS[ci]);
        printf("\n");
        for (int i = 0; i < NS; i++) {
            printf("side %+5.2f :", SIDES[i]);
            for (int ci = 0; ci < NC; ci++) {
                int ok = 0;
                float dev = measure(CUTS[ci], SIDES[i], SPEEDS[si], 0.0f, &ok);
                if (ok) printf("%8.2f", dev);
                else    printf("%8s", "-");
            }
            printf("\n");
        }
        printf("\n");
    }

    /* The one number that matters most: with no side at all, is the ghost-ball
     * aim already true? If it is not, the planner has been aiming wrong on every
     * cut since the compensation was dropped, side or no side. */
    printf("--- cut-induced throw with NO side (the claim under test) ---\n");
    printf("  cut   1.5 m/s   3.0 m/s   6.0 m/s\n");
    for (int ci = 0; ci < NC; ci++) {
        printf("%5.0f", CUTS[ci]);
        for (int si = 0; si < 3; si++) {
            int ok = 0;
            float dev = measure(CUTS[ci], 0.0f, SPEEDS[si], 0.0f, &ok);
            printf("%10.2f", ok ? dev : 0.0f);
        }
        printf("\n");
    }
    return 0;
}
