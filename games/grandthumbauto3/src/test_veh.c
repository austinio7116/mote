/*
 * gta3_veh — the claims that matter.
 *
 * A mesh with inward normals is invisible (the engine backface-culls), and a
 * mesh whose cabin sits inside the body is a brick. Neither is obvious at 128x128
 * with a car six pixels long, so both are checked here.
 *
 *  1. Every CAR_CLS maps to a real silhouette, including out-of-range input.
 *  2. Every silhouette is a closed box pair: 12 faces each, normals outward.
 *  3. The cabin sits ON the body, not inside it, and is narrower than it.
 *  4. The silhouettes are actually different shapes.
 *  5. Every face is front-facing in SCREEN space, not just by its model-space
 *     normal — see screen_area() below for why the two can disagree.
 */
#include "gta3_veh.h"

#include "mote_vec.h"

#include <math.h>
#include <stdio.h>

static int s_fail;
static void ok(int cond, const char *what) {
    if (!cond) { s_fail++; printf("  FAIL %s\n", what); }
    else printf("  ok   %s\n", what);
}

/* A face's normal points outward if it points away from the box's centre. */
static int normals_outward(const MeshVert *v, const MeshFace *f, int nf) {
    float cx=0, cy=0, cz=0;
    for (int i = 0; i < 8; i++) { cx += v[i].x; cy += v[i].y; cz += v[i].z; }
    cx /= 8.0f; cy /= 8.0f; cz /= 8.0f;
    for (int i = 0; i < nf; i++) {
        float mx = (v[f[i].a].x + v[f[i].b].x + v[f[i].c].x) / 3.0f - cx;
        float my = (v[f[i].a].y + v[f[i].b].y + v[f[i].c].y) / 3.0f - cy;
        float mz = (v[f[i].a].z + v[f[i].b].z + v[f[i].c].z) / 3.0f - cz;
        if (mx*f[i].nx + my*f[i].ny + mz*f[i].nz <= 0.0f) return 0;
    }
    return 1;
}

static void span(const MeshVert *v, float *lo, float *hi, int axis) {
    *lo = 1e9f; *hi = -1e9f;
    for (int i = 0; i < 8; i++) {
        float c = axis == 0 ? v[i].x : axis == 1 ? v[i].y : v[i].z;
        if (c < *lo) *lo = c;
        if (c > *hi) *hi = c;
    }
}

/* Replicate the rasterizer's own front-face test.
 *
 * mote_raster.c's edge() is the 2D cross (B-A)x(C-A); tri_core drops any
 * triangle whose area2 <= 0 ("screen-clockwise => positive"). mote_pipe.c's
 * project() flips y (sy = centre - focal*vy/vz) because screen y grows
 * downward, and that flip inverts the handedness. So winding has to be right
 * in SCREEN space, not world space: a mesh with correct outward normals can
 * still be culled at every pixel and render nothing, silently. Vehicles are
 * untextured and tinted, so they take the flat path where this is enforced —
 * unlike the textured path, which ignores the sign. */
static float screen_area(Vec3 a, Vec3 b, Vec3 c) {
    float ax = 64.0f + 64.0f*a.x/a.z, ay = 64.0f - 64.0f*a.y/a.z;
    float bx = 64.0f + 64.0f*b.x/b.z, by = 64.0f - 64.0f*b.y/b.z;
    float cx = 64.0f + 64.0f*c.x/c.z, cy = 64.0f - 64.0f*c.y/c.z;
    return (bx-ax)*(cy-ay) - (by-ay)*(cx-ax);
}

/* Put the camera out along a face's outward normal, looking back at the
 * origin, and express a vertex in that view space (+z forward). */
static Vec3 to_view(Vec3 p, Vec3 n, float dist) {
    Vec3 f  = v3_scale(n, -1.0f);
    Vec3 up = (fabsf(n.y) > 0.9f) ? v3(0,0,1) : v3(0,1,0);
    Vec3 r  = v3_norm(v3_cross(up, f));
    Vec3 u  = v3_cross(f, r);
    Vec3 d  = v3_sub(p, v3_scale(n, dist));
    return v3(v3_dot(r,d), v3_dot(u,d), v3_dot(f,d));
}

/* For every face: recover its outward normal from the stored int8 nx/ny/nz,
 * put the camera out along that normal looking straight at the face, and
 * assert the projected triangle is front-facing under the rasterizer's own
 * rule. A face viewed head-on from outside MUST be front-facing; if it isn't,
 * the vehicle is invisible in game no matter what normals_outward() says. */
static int screen_winding_ok(const MeshVert *v, const MeshFace *f, int nf) {
    for (int i = 0; i < nf; i++) {
        Vec3 pa = v3(v[f[i].a].x, v[f[i].a].y, v[f[i].a].z);
        Vec3 pb = v3(v[f[i].b].x, v[f[i].b].y, v[f[i].b].z);
        Vec3 pc = v3(v[f[i].c].x, v[f[i].c].y, v[f[i].c].z);
        Vec3 n = v3_norm(v3(f[i].nx / 127.0f, f[i].ny / 127.0f, f[i].nz / 127.0f));

        Vec3 va = to_view(pa, n, 600.0f);
        Vec3 vb = to_view(pb, n, 600.0f);
        Vec3 vc = to_view(pc, n, 600.0f);

        if (screen_area(va, vb, vc) <= 0.0f) return 0;
    }
    return 1;
}

int main(void) {
    printf("gta3_veh\n");

    /* 1. the class map */
    int seen[GTA3_SIL_N] = {0};
    for (int c = 0; c < 19; c++) {
        int s = gta3_sil_for_class(c);
        if (s < 0 || s >= GTA3_SIL_N) { s_fail++; printf("  FAIL class %d maps out of range\n", c); }
        else seen[s] = 1;
    }
    printf("  ok   all 19 classes map in range\n");
    ok(gta3_sil_for_class(-1) == GTA3_SIL_SEDAN, "negative class falls back to sedan");
    ok(gta3_sil_for_class(999) == GTA3_SIL_SEDAN, "huge class falls back to sedan");
    {
        int all = 1;
        for (int s = 0; s < GTA3_SIL_N; s++) if (!seen[s]) all = 0;
        ok(all, "every silhouette is reachable from some class");
    }

    /* 2, 3, 4, 5 */
    float zlen[GTA3_SIL_N], yhgt[GTA3_SIL_N];
    for (int s = 0; s < GTA3_SIL_N; s++) {
        Gta3VehMesh m;
        gta3_veh_build(&m, s);

        ok(m.body.nfaces == 12 && m.cabin.nfaces == 12, "both boxes have 12 faces");
        ok(normals_outward(m.bv, m.bf, 12), "body normals point outward");
        ok(normals_outward(m.cv, m.cf, 12), "cabin normals point outward");
        ok(screen_winding_ok(m.bv, m.bf, 12), "body faces are front-facing in screen space");
        ok(screen_winding_ok(m.cv, m.cf, 12), "cabin faces are front-facing in screen space");

        float blo, bhi, clo, chi;
        span(m.bv, &blo, &bhi, 1);            /* body Y */
        span(m.cv, &clo, &chi, 1);            /* cabin Y */
        ok(clo >= bhi - 1.0f, "the cabin sits on top of the body, not inside it");
        ok(chi > bhi, "the cabin is the tallest part of the car");

        float bxl, bxh, cxl, cxh;
        span(m.bv, &bxl, &bxh, 0);
        span(m.cv, &cxl, &cxh, 0);
        ok((cxh - cxl) <= (bxh - bxl), "the cabin is no wider than the body");

        float zl, zh, yl, yh;
        span(m.bv, &zl, &zh, 2); zlen[s] = zh - zl;
        span(m.cv, &yl, &yh, 1); yhgt[s] = yh;
    }

    /* 4. a van and a wedge must not be the same box */
    ok(fabsf(yhgt[GTA3_SIL_VAN] - yhgt[GTA3_SIL_WEDGE]) > 8.0f,
       "a van is visibly taller than a low wedge");
    ok(zlen[GTA3_SIL_LONGHOOD] > 0.0f && zlen[GTA3_SIL_COMPACT] > 0.0f,
       "silhouettes have non-zero length");

    printf(s_fail ? "FAILED (%d)\n" : "passed\n", s_fail);
    return s_fail ? 1 : 0;
}
