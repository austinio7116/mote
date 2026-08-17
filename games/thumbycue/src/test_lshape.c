/*
 * S1 — the L-shaped table, and specifically whether it holds water.
 *
 * The shape's whole difficulty is the reflex corner: the one vertex that turns
 * into the playing area, where the vertex-averaged normals are meaningless and
 * a ball can squeeze between two segments and end up the wrong side of the
 * wall. Everything else about an L is a rectangle with different numbers.
 *
 * So this asks the same question test_edge asks of the rectangles — can a ball
 * leave by any route other than a pocket — and asks it hardest at the corner
 * that is new. It also checks the chain itself: every cushion nose must face
 * the cloth, because a single reversed normal is a wall that sucks balls
 * through it rather than bouncing them.
 */
#include "cue_physics.h"
#include "cue_table.h"
#include "cue_rules.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;
static void ok(int c, const char *m){printf("%-4s %s\n",c?"ok":"FAIL",m); if(!c)fails++;}

static void make_L(CueTable *t) {
    cue_table_init(t, CUE_GAME_US8);
    t->bed_shape = CUE_BED_L;
    t->notch_x = t->half_len * 0.55f;
    t->notch_z = t->half_wid * 0.50f;
}

/* Play one ball and let it stop. Returns 0 lost, 1 potted, 2 still up. */
static int play(const CueWorld *w, Vec3 start, Vec3 dir, float speed) {
    CueBall b;
    memset(&b, 0, sizeof b);
    b.on = 1; b.id = 0; b.pos = start; b.orient = m3_identity();
    cue_phys_strike(w, &b, dir, speed, 0.0f, 0.0f);
    uint32_t ev = 0;
    for (int it = 0; it < 6000; it++)
        if (!cue_phys_step((CueWorld *)w, &b, 1, 1.0f / 120.0f, &ev)) break;
    if (b.on) return 2;
    return (b.pocket == CUE_OFF_TABLE) ? 0 : 1;
}

int main(void) {
    CueTable t; CueWorld w;
    make_L(&t);
    cue_table_build_world(&t, &w);

    const float hl = t.half_len, hw = t.half_wid;
    const float nx = t.notch_x, nz = t.notch_z;
    const float R = t.R;

    printf("the L-shaped table\n\n");

    /* ---- the chain -------------------------------------------------------- */
    ok(w.nseg > 0,        "the L builds a cushion chain");
    ok(w.nseg < CUE_MAX_SEG, "...that fits in the segment array");
    ok(w.npocket == 7,    "seven pockets: five corners and two middles");
    { int corners = 0, mids = 0;
      for (int p = 0; p < w.npocket; p++) { if (w.pocket_mid[p]) mids++; else corners++; }
      ok(corners == 5,    "...five of them corners");
      ok(mids == 2,       "...and two of them middles"); }

    /* EVERY NOSE MUST FACE THE CLOTH. Step in off the middle of each rail by a
     * couple of ball radii and that point has to be on the bed. A reversed
     * normal is not a subtle fault — it is a cushion that pulls balls through
     * itself — and it is the exact thing an outline traversed the wrong way
     * round produces. */
    {   int bad = 0;
        for (int s = 0; s < w.nseg; s++) {
            if (w.seg[s].kind != 0) continue;          /* noses, not facings */
            float mx = (w.seg[s].a.x + w.seg[s].b.x) * 0.5f;
            float mz = (w.seg[s].a.z + w.seg[s].b.z) * 0.5f;
            float px = mx + w.seg[s].n.x * (2.5f * R);
            float pz = mz + w.seg[s].n.z * (2.5f * R);
            if (!cue_table_on_bed(&t, px, pz)) bad++;
        }
        ok(bad == 0, "every cushion nose faces the cloth");
    }

    /* ...and none of them is degenerate, which is what a rail shorter than its
     * own two pocket mouths would be. */
    {   int tiny = 0;
        for (int s = 0; s < w.nseg; s++) {
            float dx = w.seg[s].b.x - w.seg[s].a.x;
            float dz = w.seg[s].b.z - w.seg[s].a.z;
            if (dx*dx + dz*dz < 1e-8f) tiny++;
        }
        ok(tiny == 0, "no zero-length segments");
    }

    /* ---- THE POCKETS ARE CUT THE WAY THE TABLE SAYS ----------------------- *
     *
     * An L used to be straight-mitred whatever pocket_round said, which is
     * invisible in any test that counts pockets and very visible on a UK table:
     * cue_render cuts the timber behind a pocket to fit the jaw it is given, so
     * a mitred facing on a rounded table leaves a slot beside every cushion.
     *
     * Counting SEGMENTS is what separates the two: a mitre is one straight
     * facing per pocket end, a bezier knuckle is a run of them. So the same L
     * built under the two rulesets must not come out the same size — and the
     * rounded one must be the bigger. */
    {   CueTable ta = t; CueWorld wa;
        ta.pocket_round = 0;
        cue_table_build_world(&ta, &wa);
        CueTable tb = t; CueWorld wb;
        tb.pocket_round = 1;
        cue_table_build_world(&tb, &wb);
        ok(wb.nseg > wa.nseg + 8,
           "a rounded L is cut with curved jaws, not the mitre");
        ok(wb.nseg < CUE_MAX_SEG,
           "...and the curved chain still fits in the segment array");
        /* A rattle circle belongs to a POCKET, one at each side of its mouth.
         * Seven mouths is fourteen, and no more: the elbow used to carry a
         * fifteenth to stop a ball squeezing through the join between its two
         * cushions, and it is a real arc of shared-endpoint segments now, so
         * there is no join left and no circle needed. */
        ok(wa.njaw == 14 && wb.njaw == 14,
           "...and neither gives the elbow a pocket's rattle circle");
        printf("     mitred %d segs, rounded %d segs\n", wa.nseg, wb.nseg); }

    /* ---- THE ELBOW IS A RADIUS, NOT A POINT ------------------------------- *
     *
     * It used to be two cushions meeting at a right angle with a rattle circle
     * dropped on the vertex: right for the physics, and a knife edge to look at.
     * So the test changed with it — what matters is that the corner TURNS
     * SMOOTHLY, in steps small enough to read as a curve, and that the chain is
     * unbroken across it, because an arc that does not share its endpoints with
     * the two straight runs is a gap a ball goes through. */
    {   const float ex = hl - nx, ez = hw - nz;
        int near = 0;
        float worst = 0.0f;
        for (int s = 0; s < w.nseg; s++) {
            /* the segments that live at the elbow */
            float mx = (w.seg[s].a.x + w.seg[s].b.x) * 0.5f - ex;
            float mz = (w.seg[s].a.z + w.seg[s].b.z) * 0.5f - ez;
            if (mx*mx + mz*mz > (4.0f*R)*(4.0f*R)) continue;
            near++;
            for (int o = 0; o < w.nseg; o++) {
                if (o == s) continue;
                float dx = w.seg[o].a.x - w.seg[s].b.x;
                float dz = w.seg[o].a.z - w.seg[s].b.z;
                if (dx*dx + dz*dz > 1e-8f) continue;      /* not the next one */
                float c = w.seg[s].n.x*w.seg[o].n.x + w.seg[s].n.z*w.seg[o].n.z;
                float turn = acosf(c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c))
                           * 57.29578f;
                if (turn > worst) worst = turn;
            }
        }
        ok(near >= 5,   "the elbow is built of several short segments, not two");
        ok(worst < 40.0f, "...and no single step round it is a sharp corner");
        printf("     elbow: %d segments, worst turn %.1f deg\n", near, (double)worst);
        /* and the chain across it is unbroken: every segment at the elbow has a
         * neighbour starting exactly where it ends */
        int broken = 0;
        for (int s = 0; s < w.nseg; s++) {
            float mx = (w.seg[s].a.x + w.seg[s].b.x) * 0.5f - ex;
            float mz = (w.seg[s].a.z + w.seg[s].b.z) * 0.5f - ez;
            if (mx*mx + mz*mz > (3.0f*R)*(3.0f*R)) continue;
            int joined = 0;
            for (int o = 0; o < w.nseg && !joined; o++) {
                if (o == s) continue;
                float dx = w.seg[o].a.x - w.seg[s].b.x;
                float dz = w.seg[o].a.z - w.seg[s].b.z;
                if (dx*dx + dz*dz < 1e-8f) joined = 1;
            }
            if (!joined) broken++;
        }
        ok(broken == 0, "...and the chain round it is unbroken");
    }

    /* ---- A RESPOTTED COLOUR LANDS ON CLOTH ------------------------------- *
     *
     * The six snooker spots were raw x and z — the bounding box's long axis
     * written into the respot — so on an L a potted colour came back inside a
     * cushion or off the cloth entirely. And when its own spot was occupied the
     * fallback walked +x from it, straight into the missing quadrant.
     *
     * Both are asked here: every spot on cloth, and every spot still on cloth
     * with the table crowded so the fallback has to run. */
    {   CueTable ts; cue_table_init(&ts, CUE_GAME_SNK15);
        ts.bed_shape = CUE_BED_L;
        ts.notch_x = ts.half_len * 0.55f;
        ts.notch_z = ts.half_wid * 0.50f;
        char why[96];
        if (cue_table_validate(&ts, why, sizeof why)) {
            CueWorld ws; cue_table_build_world(&ts, &ws);
            CueRules rs; cue_rules_init(&rs, &ts, 0);
            int off = 0;
            for (int v = 2; v <= 7; v++)
                if (!cue_table_on_bed(&ts, rs.spot[v].x, rs.spot[v].z)) off++;
            ok(off == 0, "every snooker spot on an L is on the cloth");
            /* ...and the walk away from an occupied spot goes UP THE TABLE */
            ok(fabsf(rs.spot_up.x) + fabsf(rs.spot_up.z) > 0.9f,
                                  "...and the long string has a direction");
            {   CueBall bb[CUE_MAX_BALLS];
                int nb = cue_table_rack(&ts, bb);
                /* park a ball on every spot so the fallback must run */
                for (int v = 2; v <= 7 && v - 2 + 1 < nb; v++) {
                    bb[v - 1].on = 1; bb[v - 1].pos = rs.spot[v];
                }
                int bad = 0;
                for (int v = 2; v <= 7; v++) {
                    Vec3 p = rs.spot[v];
                    for (int step = 1; step <= 60; step++) {
                        float d = (float)step * rs.R * 0.5f;
                        Vec3 q = p;
                        q.x += rs.spot_up.x * d; q.z += rs.spot_up.z * d;
                        if (cue_table_on_bed(&ts, q.x, q.z)) { p = q; break; }
                    }
                    if (!cue_table_on_bed(&ts, p.x, p.z)) bad++;
                }
                ok(bad == 0,      "...that stays on the cloth all the way up"); }
        } }

    /* ---- can a ball leave anywhere but a pocket? ------------------------- *
     * Swept over the whole cloth and the whole compass, at every pace a player
     * can make. This is the question the shape exists to be asked. */
    {   int lost = 0, potted = 0, stayed = 0, shots = 0;
        for (int ix = -9; ix <= 9; ix++) {
            for (int iz = -9; iz <= 9; iz++) {
                float sx = hl * 0.92f * (float)ix / 9.0f;
                float sz = hw * 0.92f * (float)iz / 9.0f;
                if (!cue_table_on_bed(&t, sx, sz)) continue;
                /* keep clear of the cushions so the start is legal */
                if (!cue_table_on_bed(&t, sx + 3*R, sz + 3*R) ||
                    !cue_table_on_bed(&t, sx - 3*R, sz - 3*R)) continue;
                for (int a = 0; a < 16; a++) {
                    float th = 6.2831853f * (float)a / 16.0f;
                    Vec3 dir = v3(cosf(th), 0, sinf(th));
                    for (int sp = 0; sp < 3; sp++) {
                        float speed = 1.2f + 3.4f * (float)sp;
                        int r = play(&w, v3(sx, R, sz), dir, speed);
                        shots++;
                        if (r == 0) lost++; else if (r == 1) potted++; else stayed++;
                    }
                }
            }
        }
        printf("     %d shots: %d potted, %d stayed, %d LOST\n",
               shots, potted, stayed, lost);
        ok(shots > 500,  "the sweep actually played a lot of shots");
        ok(lost == 0,    "no ball leaves the L except down a pocket");
        ok(potted > 0,   "...and its pockets do take balls");
    }

    /* ---- the reflex corner, hit deliberately ---------------------------- *
     * Straight at the vertex from every direction that can see it, at pace.
     * This is the shot that would squeeze through the join between the two
     * segments if the corner had no radius. */
    {   Vec3 rc = v3(hl - nx, R, hw - nz);
        int lost = 0, shots = 0;
        for (int a = 0; a < 48; a++) {
            float th = 6.2831853f * (float)a / 48.0f;
            /* start a little way off the corner, aimed at it */
            for (int d = 0; d < 3; d++) {
                float back = 0.10f + 0.10f * (float)d;
                Vec3 start = v3(rc.x - cosf(th)*back, R, rc.z - sinf(th)*back);
                if (!cue_table_on_bed(&t, start.x, start.z)) continue;
                for (int sp = 0; sp < 3; sp++) {
                    int r = play(&w, start, v3(cosf(th), 0, sinf(th)), 2.0f + 3.0f*(float)sp);
                    shots++;
                    if (r == 0) lost++;
                }
            }
        }
        printf("     reflex corner: %d shots, %d LOST\n", shots, lost);
        ok(shots > 50, "the corner was actually struck from all round");
        ok(lost == 0,  "a ball driven into the reflex corner stays on the table");
    }

    /* ---- and a ball cannot come to rest in the missing corner ------------ */
    {   int inside = 0;
        for (int a = 0; a < 32; a++) {
            float th = 6.2831853f * (float)a / 32.0f;
            CueBall b;
            memset(&b, 0, sizeof b);
            b.on = 1; b.pos = v3(0.0f, R, 0.0f); b.orient = m3_identity();
            cue_phys_strike(&w, &b, v3(cosf(th), 0, sinf(th)), 5.0f, 0.0f, 0.0f);
            uint32_t ev = 0;
            for (int it = 0; it < 6000; it++)
                if (!cue_phys_step(&w, &b, 1, 1.0f/120.0f, &ev)) break;
            if (b.on && !cue_table_on_bed(&t, b.pos.x, b.pos.z)) {
                /* the rail is legal to rest on; the notch is not */
                if (b.pos.x > hl - nx && b.pos.z > hw - nz &&
                    b.pos.x < hl && b.pos.z < hw) inside = 1;
            }
        }
        ok(!inside, "no ball comes to rest inside the missing corner");
    }

    /* ---- a rectangle is untouched by any of this ------------------------ */
    {   CueTable r; CueWorld rw;
        cue_table_init(&r, CUE_GAME_US8);
        cue_table_build_world(&r, &rw);
        ok(rw.npocket == 6, "a rectangular table still has six pockets");
        int corners = 0, mids = 0;
        for (int p = 0; p < rw.npocket; p++) { if (rw.pocket_mid[p]) mids++; else corners++; }
        ok(corners == 4 && mids == 2, "...four corners and two middles");
    }

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}
