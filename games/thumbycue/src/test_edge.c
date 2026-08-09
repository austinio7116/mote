/*
 * ThumbyCue — the edge of the table. A ball leaves it by going down a pocket,
 * and by no other route.
 *
 * Two things were reported and both were the surface model added for jump
 * shots getting the frame wrong.
 *
 *   "SOME HARD SHOTS DISAPPEAR THROUGH THE POCKET JAWS." Every hard shot
 *   penetrates the cushion by a millimetre or two before the collision
 *   resolves it — that is what a fixed substep does. The frame top was being
 *   applied as a floor to any ball whose x,z put it outside the cushion line,
 *   regardless of how HIGH the ball was, so a ball at cloth height mid-bounce
 *   was assigned frame height: LIFTED onto the frame, from where it rolled off
 *   and was deleted. Nothing may ever raise a ball because of where it is.
 *
 *   "THE BALL FALLS THROUGH THE CUSHION/RAIL." The wall stopped at
 *   cushion_nose and the floor started at rail_top, and those are 10 mm apart.
 *   cushion_nose is the ball-CONTACT line, 63.5% of a ball up the front face —
 *   not the top of the cushion, which is rail_top, level with the wood. A ball
 *   in that band met no wall and found no floor.
 *
 * So this asks the two questions that follow: can a ball get through the
 * cushion, and can a ball be lost anywhere other than a pocket. It plays every
 * pocket and every cushion, at every speed and angle a player can produce.
 */
#include "cue_physics.h"
#include "cue_table.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int s_fail;

static const int KINDS[] = { CUE_GAME_SNK15, CUE_GAME_UK8, CUE_GAME_SNK6, CUE_GAME_US8 };
static const char *KN[] = { "snooker", "UK8", "SNK6", "US8" };

/* Play one ball from `start` along `dir` at `speed` and let it stop. */
static void play(const CueWorld *w, CueBall *b, Vec3 start, Vec3 dir, float speed) {
    memset(b, 0, sizeof *b);
    b->on = 1; b->id = 0; b->pos = start; b->orient = m3_identity();
    cue_phys_strike(w, b, dir, speed, 0.0f, 0.0f);
    uint32_t ev = 0;
    for (int it = 0; it < 4000; it++)
        if (!cue_phys_step((CueWorld *)w, b, 1, 1.0f / 120.0f, &ev)) break;
}

int main(void) {
    printf("the edge of the table\n\n");

    /* ---- 1. into the pockets, hard ------------------------------------- *
     * Drive at each pocket centre from inside, with the aim swept either side
     * so the jaws are struck as well as the mouth. Every shot must finish
     * POTTED or still ON THE TABLE — never removed. */
    for (unsigned k = 0; k < sizeof KINDS / sizeof KINDS[0]; k++) {
        CueTable t; CueWorld w;
        cue_table_init(&t, KINDS[k]);
        cue_table_build_world(&t, &w);
        int lost = 0, potted = 0, stayed = 0, shots = 0;

        for (int p = 0; p < w.npocket; p++) {
            Vec3 pc = w.pocket[p];
            for (int a = -14; a <= 14; a++) {
                for (int s = 0; s < 6; s++) {
                    float speed = 1.0f + (float)s * 1.4f;      /* 1.0 .. 8.0 m/s */
                    float ang = (float)a * 0.035f;             /* +-28 deg */
                    float ax = -pc.x, az = -pc.z;
                    float l = sqrtf(ax * ax + az * az);
                    if (l < 1e-5f) { ax = 0; az = 1; l = 1; }
                    ax /= l; az /= l;
                    Vec3 start = v3(pc.x + ax * 0.45f, w.R, pc.z + az * 0.45f);
                    float lx = t.half_len - t.R * 1.2f, lz = t.half_wid - t.R * 1.2f;
                    if (start.x >  lx) start.x =  lx;
                    if (start.x < -lx) start.x = -lx;
                    if (start.z >  lz) start.z =  lz;
                    if (start.z < -lz) start.z = -lz;
                    float dx = pc.x - start.x, dz = pc.z - start.z;
                    float dl = sqrtf(dx * dx + dz * dz);
                    if (dl < 1e-5f) continue;
                    dx /= dl; dz /= dl;
                    float c = cosf(ang), si = sinf(ang);
                    CueBall b;
                    play(&w, &b, start, v3(dx * c - dz * si, 0, dx * si + dz * c), speed);
                    shots++;
                    if (!b.on && b.pocket == CUE_OFF_TABLE) { lost++; s_fail++; }
                    else if (!b.on) potted++;
                    else stayed++;
                }
            }
        }
        printf("  %-8s at the pockets: %4d shots, %4d potted, %4d up, %d LOST\n",
               KN[k], shots, potted, stayed, lost);
    }

    /* ---- 2. into the cushions, hard ------------------------------------ *
     * The same sweep aimed at the middle of each rail rather than at a pocket.
     * A ball played flat cannot get through a cushion at any speed: it must
     * finish on the cloth, or down a pocket it rebounded into. Anything that
     * ends up removed has gone through the table. */
    printf("\n");
    for (unsigned k = 0; k < sizeof KINDS / sizeof KINDS[0]; k++) {
        CueTable t; CueWorld w;
        cue_table_init(&t, KINDS[k]);
        cue_table_build_world(&t, &w);
        int lost = 0, shots = 0, outside = 0;
        float worst = 0.0f;   /* deepest any ball ever got past the cushion line */

        /* Four rails; aim at a point on each, from the middle of the table. */
        const Vec3 aim[4] = {
            { t.half_len, 0,  0.0f }, { -t.half_len, 0, 0.0f },
            { 0.0f,       0,  t.half_wid }, { 0.0f,   0, -t.half_wid },
        };
        for (int r = 0; r < 4; r++) {
            for (int a = -16; a <= 16; a++) {
                for (int s = 0; s < 8; s++) {
                    float speed = 1.0f + (float)s * 1.0f;      /* 1 .. 8 m/s */
                    float ang = (float)a * 0.05f;              /* +-46 deg */
                    /* Start a third of the way back from the target rail. */
                    Vec3 start = v3(aim[r].x * 0.35f, w.R, aim[r].z * 0.35f);
                    float dx = aim[r].x - start.x, dz = aim[r].z - start.z;
                    float dl = sqrtf(dx * dx + dz * dz);
                    dx /= dl; dz /= dl;
                    float c = cosf(ang), si = sinf(ang);
                    CueBall b;
                    play(&w, &b, start, v3(dx * c - dz * si, 0, dx * si + dz * c), speed);
                    shots++;
                    if (!b.on && b.pocket == CUE_OFF_TABLE) { lost++; s_fail++; }
                    if (b.on) {
                        /* Where it came to rest, in the region it is allowed. */
                        float ox = fabsf(b.pos.x) - w.play_x;
                        float oz = fabsf(b.pos.z) - w.play_z;
                        float o = ox > oz ? ox : oz;
                        if (o > worst) worst = o;
                        if (o > 0.0f) { outside++; s_fail++; }
                    }
                }
            }
        }
        printf("  %-8s at the cushions: %4d shots, %d LOST, %d resting outside "
               "the cushion line (deepest %+.4f m)\n",
               KN[k], shots, lost, outside, (double)worst);
    }

    /* ---- 3. nothing is ever lifted ------------------------------------- *
     * The specific mechanism of the jaw bug, asked directly: put a ball at
     * cloth height at every point across the frame — including out over the
     * pocket mouths — step once, and require that its height never INCREASES.
     * Gravity may take it down a pocket; nothing may raise it. */
    printf("\n");
    for (unsigned k = 0; k < sizeof KINDS / sizeof KINDS[0]; k++) {
        CueTable t; CueWorld w;
        cue_table_init(&t, KINDS[k]);
        cue_table_build_world(&t, &w);
        int lifted = 0, tried = 0;
        float most = 0.0f;
        for (int ix = -140; ix <= 140; ix++) {
            for (int iz = -140; iz <= 140; iz++) {
                float x = (float)ix / 140.0f * w.bound_x * 1.02f;
                float z = (float)iz / 140.0f * w.bound_z * 1.02f;
                CueBall b;
                memset(&b, 0, sizeof b);
                b.on = 1; b.pos = v3(x, w.R, z); b.orient = m3_identity();
                b.vel = v3(0.4f, 0, 0.3f);      /* awake, so it is integrated */
                uint32_t ev = 0;
                cue_phys_step(&w, &b, 1, 1.0f / 2000.0f, &ev);
                tried++;
                if (b.on && b.drop <= 0.0f) {
                    float rise = b.pos.y - w.R;
                    if (rise > 1e-6f) { lifted++; if (rise > most) most = rise; }
                }
            }
        }
        if (lifted) s_fail++;
        printf("  %-8s %6d places on and around the frame, %d lifted "
               "(worst %+.4f m)\n", KN[k], tried, lifted, (double)most);
    }

    printf(s_fail ? "\nFAILED (%d)\n" : "\nPASSED\n", s_fail);
    return s_fail ? 1 : 0;
}
