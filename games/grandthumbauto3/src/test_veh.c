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
#include <stdint.h>
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

static void span_n(const MeshVert *v, int n, float *lo, float *hi, int axis) {
    *lo = 1e9f; *hi = -1e9f;
    for (int i = 0; i < n; i++) {
        float c = axis == 0 ? v[i].x : axis == 1 ? v[i].y : v[i].z;
        if (c < *lo) *lo = c;
        if (c > *hi) *hi = c;
    }
}
static void span(const MeshVert *v, float *lo, float *hi, int axis) {
    span_n(v, 8, lo, hi, axis);
}

/* Screen-space signed area of a projected triangle -- see the full comment on
 * screen_winding_ok() below for why this has to be checked in screen space
 * rather than world space. */
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

static Vec3 vert_of(const MeshVert *v, int i) {
    return v3(v[i].x, v[i].y, v[i].z);
}

/* The box centroid is independent of any face's vertex order, and so is a
 * face's vertex mean, so the direction between them is a valid "outward"
 * that does not move when winding changes. */
static Vec3 box_centroid(const MeshVert *v) {
    float x=0, y=0, z=0;
    for (int i = 0; i < 8; i++) { x += v[i].x; y += v[i].y; z += v[i].z; }
    return v3(x/8.0f, y/8.0f, z/8.0f);
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
 * unlike the textured path, which ignores the sign.
 *
 * The camera direction MUST come from an independent ground truth, not from
 * the face's stored normal: mote__face derives that normal from the very
 * vertex order under test, so a camera built from it flips along with the
 * winding and the area stays positive either way — a check that cannot fail.
 * The box centroid and the face's vertex mean are both order-independent, so
 * the direction between them is safe to use instead. */
static int screen_winding_ok(const MeshVert *v, const MeshFace *f) {
    Vec3 a = vert_of(v, f->a), b = vert_of(v, f->b), c = vert_of(v, f->c);
    Vec3 ctr = box_centroid(v);
    Vec3 mid = v3_scale(v3_add(v3_add(a, b), c), 1.0f/3.0f);
    Vec3 out = v3_norm(v3_sub(mid, ctr));           /* order-independent */

    Vec3 va = to_view(a, out, 600.0f);
    Vec3 vb = to_view(b, out, 600.0f);
    Vec3 vc = to_view(c, out, 600.0f);

    return screen_area(va, vb, vc) > 0.0f;
}

static int all_faces_screen_ok(const MeshVert *v, const MeshFace *f, int nf) {
    for (int i = 0; i < nf; i++) if (!screen_winding_ok(v, &f[i])) return 0;
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
        /* HELI, BUS and TANK are deliberately unreachable from
         * gta3_sil_for_class: none is one of the 19 CAR_CLS handling classes,
         * they are vehicle TYPES the game picks directly. Every other
         * silhouette must still be reachable, or it is dead weight in flash
         * and RAM. */
        int all = 1;
        for (int s = 0; s < GTA3_SIL_N; s++)
            if (s != GTA3_SIL_HELI && s != GTA3_SIL_BUS && s != GTA3_SIL_TANK && !seen[s]) all = 0;
        ok(all, "every class-driven silhouette is reachable from some class");
        ok(!seen[GTA3_SIL_HELI] && !seen[GTA3_SIL_BUS] && !seen[GTA3_SIL_TANK],
           "and no handling class maps onto the heli, bus or tank");
    }

    /* Prove the check discriminates. This test was tautological once — it
     * derived its camera from the same normal it was checking — and passed
     * happily while a reversed face rendered nothing. If this line ever stops
     * failing on reversed input, the check above has stopped working. */
    {
        Gta3VehMesh m;
        gta3_veh_build(&m, GTA3_SIL_SEDAN);
        MeshFace rev = m.bf[0];
        uint8_t t = rev.b; rev.b = rev.c; rev.c = t;
        ok(screen_winding_ok(m.bv, &m.bf[0]), "the real face passes the winding check");
        ok(!screen_winding_ok(m.bv, &rev),    "a reversed face FAILS the winding check");
    }

    /* 2, 3, 4, 5 */
    float zlen[GTA3_SIL_N], yhgt[GTA3_SIL_N];
    for (int s = 0; s < GTA3_SIL_N; s++) {
        Gta3VehMesh m;
        gta3_veh_build(&m, s);

        /* wheels are TWO axle boxes now, so 24 faces over 16 vertices */
        ok(m.body.nfaces == 12 && m.cabin.nfaces == 12 && m.wheels.nfaces == 24,
           "body and cabin have 12 faces, the wheels 24");
        ok(m.wheels.nverts == 16, "the wheel mesh has both axle boxes");
        ok(normals_outward(m.bv, m.bf, 12), "body normals point outward");
        ok(normals_outward(m.cv, m.cf, 12), "cabin normals point outward");
        /* Each axle box is checked on its own: normals_outward and
         * all_faces_screen_ok both derive a centroid from the FIRST 8 vertices,
         * so handing them a two-box mesh judges the front axle against the rear
         * box's centre and fails every face. The front box's faces index
         * wv[8..15], so they are de-offset into a local copy first. */
        ok(normals_outward(m.wv, m.wf, 12), "rear axle normals point outward");
        ok(all_faces_screen_ok(m.bv, m.bf, 12), "body faces are front-facing in screen space");
        ok(all_faces_screen_ok(m.cv, m.cf, 12), "cabin faces are front-facing in screen space");
        ok(all_faces_screen_ok(m.wv, m.wf, 12), "rear axle faces are front-facing in screen space");
        { MeshFace ff[12];
          for (int i = 0; i < 12; i++) { ff[i] = m.wf[12+i];
              ff[i].a -= 8; ff[i].b -= 8; ff[i].c -= 8; }
          ok(normals_outward(m.wv + 8, ff, 12), "front axle normals point outward");
          ok(all_faces_screen_ok(m.wv + 8, ff, 12), "front axle faces are front-facing in screen space"); }
        /* The GAP is the whole point: without it the two boxes are one slab
         * again and the car has no wheels, just a rectangle along the sill.
         * A car's pair is separated along Z (front axle ahead of rear); the
         * helicopter's is separated along X (a skid either side of the
         * fuselage). The invariant both share is that the two boxes are
         * DISJOINT in some axis, so that is what is asserted. */
        /* A car's pair is separated along Z (front axle ahead of rear); the
         * helicopter's skids and the tank's tracks are separated along X, one
         * per side. The invariant all three share is that the two boxes are
         * DISJOINT in some axis, so that is what is asserted. */
        { int sideways = (s == GTA3_SIL_HELI || s == GTA3_SIL_TANK);
          float a0,a1,b0,b1; int axis = sideways ? 0 : 2;
          span(m.wv,     &a0,&a1, axis);
          span(m.wv + 8, &b0,&b1, axis);
          ok(b0 > a1 || a0 > b1,
             sideways ? "the two rails do not meet under the hull"
                      : "there is a gap between the rear and front axles"); }
        /* The wheel slab only reads as a tyre track if it is PROUD of the body in x
         * and does not dip below the road plane at y=0. Both are easy to lose to an
         * int8 overflow: 134 wraps to -122 and silently inverts the box. */
        /* Both boxes, not just the first: a car's axles each span the full
         * track, but the helicopter's skids are one per side, so measuring
         * wv[0..7] alone sees only the left rail. */
        { float bxl,bxh,wxl,wxh,wyl,wyh;
          span(m.bv,&bxl,&bxh,0); span_n(m.wv,16,&wxl,&wxh,0); span_n(m.wv,16,&wyl,&wyh,1);
          ok(wxh > bxh && wxl < bxl,
             (s == GTA3_SIL_HELI) ? "the skids stand outboard of the fuselage"
             : (s == GTA3_SIL_TANK) ? "the tracks stand outboard of the hull"
                                    : "the wheel line is wider than the body");
          ok(wyl >= 0.0f, "the wheel line does not sink below the road plane"); }

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

    /* Task 9: the tank turret is built directly from gta3_box (not from
     * gta3_veh_build's silhouette table), with an off-centre asymmetric
     * barrel box whose z-range does not straddle the origin the way the
     * body/cabin pairs above do. A winding bug that only shows up off-centre
     * would slip past every check above it, so prove this shape independently
     * with the same proven helpers rather than trusting the pattern holds. */
    {
        MeshVert cv[8]; MeshFace cf[12]; int ncf;
        gta3_box(cv, cf, &ncf, -32, 32, 0, 25, -44, 19);        /* turret cab */
        ok(ncf == 12, "turret cab box has 12 faces");
        ok(normals_outward(cv, cf, 12), "turret cab normals point outward");
        ok(all_faces_screen_ok(cv, cf, 12), "turret cab faces are front-facing in screen space");

        MeshVert bv2[8]; MeshFace bf2[12]; int nbf;
        gta3_box(bv2, bf2, &nbf, -5, 5, 10, 16, 13, 121);       /* turret barrel: off-centre, asymmetric */
        ok(nbf == 12, "turret barrel box has 12 faces");
        ok(normals_outward(bv2, bf2, 12), "turret barrel normals point outward");
        ok(all_faces_screen_ok(bv2, bf2, 12), "turret barrel faces are front-facing in screen space");
    }

    /* 4. a van and a racer must not be the same box */
    ok(fabsf(yhgt[GTA3_SIL_VAN] - yhgt[GTA3_SIL_RACER]) > 8.0f,
       "a van is visibly taller than a low racer");
    ok(zlen[GTA3_SIL_LONGHOOD] > 0.0f && zlen[GTA3_SIL_COMPACT] > 0.0f,
       "silhouettes have non-zero length");

    /* 5. the silhouettes that were split out of the old single WEDGE have to be
     * genuinely different boxes, or the split bought nothing. COUPE, SPORTS and
     * CLASSICSPT carried 14 of the 54 car types between them as one shape. */
    {
        const int sp[3] = { GTA3_SIL_COUPE, GTA3_SIL_SPORTS, GTA3_SIL_CLASSICSPT };
        const char *nm[3] = { "coupe", "sports", "classicspt" };
        int distinct = 1;
        for (int i = 0; i < 3; i++)
            for (int j = i + 1; j < 3; j++) {
                Gta3VehMesh a, b2;
                gta3_veh_build(&a, sp[i]); gta3_veh_build(&b2, sp[j]);
                /* roofline, and where the glasshouse sits fore/aft */
                if (a.cv[3].y == b2.cv[3].y && a.cv[0].z == b2.cv[0].z &&
                    a.cv[4].z == b2.cv[4].z) {
                    distinct = 0;
                    printf("  FAIL %s and %s are the same cabin\n", nm[i], nm[j]);
                }
            }
        ok(distinct, "coupe, sports and classicspt are three different shapes");
        /* TAXI is folded into WAGON (see gta3_veh.h). What makes a taxi a taxi
         * is now its yellow paint and its roof sign, both keyed on CAR_TAXI in
         * draw_vehicle_mesh, not its silhouette. Assert the fold rather than a
         * distinction that no longer exists. */
        ok(gta3_sil_for_class(14) == GTA3_SIL_WAGON,
           "the taxi handling class draws as a wagon");
    }

    /* 6. lamp styles: in range for every silhouette, out-of-range falls back,
     * and more than one style is actually in use (a table that answered ROUND
     * for everything would pass a range check and change nothing on screen). */
    {
        int used[3] = {0,0,0}, inrange = 1;
        for (int s = 0; s < GTA3_SIL_N; s++) {
            int st = gta3_lamp_style(s);
            if (st < 0 || st > GTA3_LAMP_BAR) inrange = 0; else used[st] = 1;
        }
        ok(inrange, "every silhouette has a lamp style in range");
        ok(used[GTA3_LAMP_ROUND] && used[GTA3_LAMP_RECT] && used[GTA3_LAMP_BAR],
           "all three lamp styles are in use");
        ok(gta3_lamp_style(-1) == GTA3_LAMP_ROUND, "negative silhouette falls back to round");
        ok(gta3_lamp_style(999) == GTA3_LAMP_ROUND, "huge silhouette falls back to round");
    }

    /* SHAPE CHECKS for the two silhouettes that are meant to read as something
     * other than a car. Both were wrong on first authoring in exactly the way
     * these assert against: one long box stacked on another. */
    {
        Gta3VehMesh m;
        gta3_veh_build(&m, GTA3_SIL_TRUCK);
        float bz0,bz1, cz0,cz1, bx0,bx1, cx0,cx1;
        span(m.bv,&bz0,&bz1,2); span(m.cv,&cz0,&cz1,2);
        span(m.bv,&bx0,&bx1,0); span(m.cv,&cx0,&cx1,0);
        /* A truck is a short cab up front, not a box van: the cab takes well
         * under half the chassis and sits in its forward half. */
        ok((cz1-cz0) < (bz1-bz0) * 0.45f, "the truck cab is under half the chassis long");
        ok((cz0+cz1) * 0.5f > 0.0f,        "the truck cab sits in the forward half");
        ok(cz1 < bz1,                      "there is a bonnet in front of the truck cab");
        ok((bz1-bz0) - (cz1-cz0) > (bz1-bz0) * 0.5f, "and a long flat bed behind it");
        ok(cx1 < bx1,                      "the truck bed is wider than its cab");
    }
    {
        Gta3VehMesh m;
        gta3_veh_build(&m, GTA3_SIL_BUS);
        float bz0,bz1, cz0,cz1, bx0,bx1, cx0,cx1, by0,by1, cy0,cy1;
        span(m.bv,&bz0,&bz1,2); span(m.cv,&cz0,&cz1,2);
        span(m.bv,&bx0,&bx1,0); span(m.cv,&cx0,&cx1,0);
        span(m.bv,&by0,&by1,1); span(m.cv,&cy0,&cy1,1);
        /* A bus is one long flush box, not a van with a nose. */
        ok((cz1-cz0) > (bz1-bz0) * 0.9f,  "the bus cabin spans nearly the whole chassis");
        ok((cx1-cx0) > (bx1-bx0) * 0.9f,  "and nearly its whole width");
        ok(cy1 > (bz1-bz0) * 0.4f,        "the bus stands tall against its length");
    }
    {
        Gta3VehMesh m;
        gta3_veh_build(&m, GTA3_SIL_VAN);
        float bz0,bz1, cz0,cz1;
        span(m.bv,&bz0,&bz1,2); span(m.cv,&cz0,&cz1,2);
        /* A van is a box with a SHORT hood: cabin over most of the length,
         * with what is left all at the front. */
        ok((cz1-cz0) > (bz1-bz0) * 0.7f, "the van cabin covers most of the chassis");
        ok(cz1 < bz1,                    "with a hood in front of it");
        ok((bz1-cz1) < (bz1-bz0) * 0.25f, "and that hood is short");
        ok(cz0 - bz0 < bz1 - cz1,        "the hood is longer than anything behind the cabin");
    }
    {
        Gta3VehMesh m;
        gta3_veh_build(&m, GTA3_SIL_TANK);
        float bz0,bz1, cz0,cz1, by0,by1, cy0,cy1, wz0,wz1, wy0,wy1, ax0,ax1, bx0,bx1;
        span(m.bv,&bz0,&bz1,2); span(m.cv,&cz0,&cz1,2);
        span(m.bv,&by0,&by1,1); span(m.cv,&cy0,&cy1,1);
        span(m.wv,&wz0,&wz1,2); span(m.wv,&wy0,&wy1,1);
        span_n(m.wv,16,&ax0,&ax1,0); span(m.bv,&bx0,&bx1,0);
        /* A tank is a slab with a low superstructure and full-length tracks.
         * It used to borrow TRUCK, which since that was reshaped is a short
         * cab over a long open bed — a flatbed lorry with a gun on it. */
        ok((cy1-cy0) < (by1-by0) * 0.6f, "the tank superstructure is shallower than its hull");
        ok((cz1-cz0) > (bz1-bz0) * 0.6f, "and covers most of the hull's length");
        ok((wz1-wz0) > (bz1-bz0) * 0.9f, "the tracks run nearly the whole hull");
        ok((wy1-wy0) > 20.0f,            "and stand taller than any tyre");
        ok(ax1 > bx1 && ax0 < bx0,       "with the hull sitting between them");
    }
    {
        Gta3VehMesh m;
        gta3_veh_build(&m, GTA3_SIL_HELI);
        float cz0,cz1, cx0,cx1, by0,by1, cy0,cy1;
        span(m.cv,&cz0,&cz1,2); span(m.cv,&cx0,&cx1,0);
        span(m.bv,&by0,&by1,1); span(m.cv,&cy0,&cy1,1);
        /* The canopy is SQUARE in plan. As a long rectangle on a longer one it
         * read as a stacked block rather than an aircraft. */
        float len = cz1-cz0, wid = cx1-cx0;
        ok(len > wid*0.8f && len < wid*1.25f, "the helicopter canopy is square in plan");
        ok(cz1 > 0.0f,                        "and sits over the nose");
        ok((cy1-cy0) < len,                   "the canopy is wider than it is tall");
        ok((by1-by0) < len,                   "and so is the fuselage");
    }

    printf(s_fail ? "FAILED (%d)\n" : "passed\n", s_fail);
    return s_fail ? 1 : 0;
}
