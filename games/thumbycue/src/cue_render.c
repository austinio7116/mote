/*
 * ThumbyCue — scene renderer. See cue_render.h.
 */
#include "cue_render.h"
#include <stdlib.h>
#include <stdio.h>
#include "cue_faces.h"
#include "cue_types.h"
#include "r3d_raster.h"
#include "mote_api.h"      /* MoteApi / MoteSphereTex / scene_add_* (engine port) */
#include <math.h>
#include <string.h>
#include <stdint.h>

#define CUE_NEAR    0.05f
#define CUE_DEPTH_K (65535.0f * CUE_NEAR)

/* ---- static table mesh (world space) ---------------------------------- */
/* CueTri now lives in cue_render.h so hosts that draw the table themselves can
 * read it — see cue_render_table_tris(). */
#ifndef MAX_TABLE_TRI
#define MAX_TABLE_TRI 2200
#endif

/* Render-only tessellation of the pocket arcs, throats and lips. Nothing here
 * touches physics — it is the difference between a pocket mouth that reads as a
 * curve and one that reads as a hexagon at close range. */
#ifndef CUE_ARC_SEGS
#define CUE_ARC_SEGS 6
#endif
#define MAX_STRI      3000     /* near-clipping can split a tri into two */
static CueTri  *s_tab;          /* arena-allocated (Mote) — see cue_render_set_buffers() */
static int      s_ntab;
static int      s_bed_ntab;   /* first s_bed_ntab tris are the flat cloth bed */
static int      s_lip_ntab;   /* s_tab[s_lip_ntab..s_ntab) are the pocket drop lips */
static uint16_t s_cloth, s_bg_top, s_bg_bot;
static uint16_t s_cloth_shadow;  /* dark cloth tint for ball shadow-side bounce */
static float    s_ballR = 0.0286f;
static int      s_is_snooker;
/* ids 1..15 mean reds, not solids/stripes */
static int      s_lip_mode = 1;  /* 0=none 1=tight 2=wide 3=deep (CUE_LIP env) */
static int      s_markings = 1;  /* emit the chalk as quads — see cue_render_set_markings */
static int      s_ball_set = 0;  /* 0 PRO, 1 UK Y/B, 2 UK Y/R, 3 dyna */

/* ---- per-frame projected lists ---------------------------------------- */
typedef struct { float x0,y0,x1,y1,x2,y2; uint16_t d0,d1,d2; uint16_t color; } STri;
static STri *s_stri; static int s_nstri;   /* arena-allocated (Mote) */
static int s_bed_nstri;   /* s_stri[0..s_bed_nstri) are the flat cloth bed */
static int s_lip_nstri;   /* s_stri[s_lip_nstri..s_nstri) are the pocket drop lips */

/* Mote: report the two big buffers' sizes and receive arena pointers for them, so they
 * live in the 280 KB arena rather than the module's 128 KB static RAM. Call before any
 * table build (i.e. before cue_game_init). */
size_t cue_render_tab_bytes(void)  { return (size_t)MAX_TABLE_TRI * sizeof(CueTri); }
size_t cue_render_stri_bytes(void) { return (size_t)MAX_STRI * sizeof(STri); }
void   cue_render_set_buffers(void *tab, void *stri) { s_tab = (CueTri *)tab; s_stri = (STri *)stri; }

typedef struct { float cx, cy, rad, viewz; Mat3 orient; uint8_t id; } Sprite;
static Sprite s_spr[CUE_MAX_BALLS]; static int s_nspr;
/* ground-plane shadow decal: centre + two screen-space axis vectors (the
 * projection of world +X and +Z offsets), so it foreshortens with the cloth */
static struct { float cx, cy, ux, uy, vx, vy; } s_shadow[CUE_MAX_BALLS];
static int s_nshadow;

#define MAX_DOTS 48
static struct { float x, y; uint16_t d; } s_dot[MAX_DOTS]; static int s_ndot;
static struct { float x, y; uint16_t d; } s_odot[MAX_DOTS]; static int s_nodot;
static struct { float tx,ty,bx,by; uint16_t color; int on; } s_cue;
static float s_cue_side, s_cue_vert, s_cue_elev;   /* tip offset + elevation for the stick */
static struct { float cx, cy, rad; uint16_t d; int on; } s_ghost;

/* View globals used by the per-pixel pass. */
static CueView s_view;
static float   s_focal;
static Vec3    s_light = { 0.10f, 0.975f, 0.20f };  /* nearly overhead (snooker lamps) */

/* ---- helpers ----------------------------------------------------------- */
static inline uint16_t shade565(uint16_t c, float s) {
    if (s < 0) s = 0;
    if (s > 1.999f) s = 1.999f;
    int r = (int)(((c >> 11) & 0x1F) * s); if (r > 31) r = 31;
    int g = (int)(((c >> 5) & 0x3F) * s);  if (g > 63) g = 63;
    int b = (int)((c & 0x1F) * s);         if (b > 31) b = 31;
    return (uint16_t)((r << 11) | (g << 5) | b);
}
static inline uint16_t add565(uint16_t c, int ar, int ag, int ab) {
    int r = ((c >> 11) & 0x1F) + ar; if (r > 31) r = 31;
    int g = ((c >> 5) & 0x3F) + ag;  if (g > 63) g = 63;
    int b = (c & 0x1F) + ab;         if (b > 31) b = 31;
    return (uint16_t)((r << 11) | (g << 5) | b);
}
/* Blend a→b by t in [0,1]. */
static inline uint16_t mix565(uint16_t a, uint16_t b, float t) {
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    int ar = (a>>11)&0x1F, ag = (a>>5)&0x3F, ab = a&0x1F;
    int br = (b>>11)&0x1F, bg = (b>>5)&0x3F, bb = b&0x1F;
    int rr = ar + (int)((br-ar)*t), gg = ag + (int)((bg-ag)*t), bl = ab + (int)((bb-ab)*t);
    return (uint16_t)((rr<<11)|(gg<<5)|bl);
}

/* Ball lighting style (0=smooth/current, 1=hard spec, 2=toon, 3=gloss). */
static int s_light_mode = 1;
void cue_render_set_light_mode(int m) { s_light_mode = m; }
/* The pocket voids — the cone/pouch drawn down each hole. The handheld game
 * needs them: it has nothing else under the bed, so without them a pocket is a
 * hole onto the background. CueVR does not — it stands the table on a solid
 * body with a black tray under the whole footprint, and a cone inside that
 * tray is a second, shallower floor competing with the first. */
static int s_voids = 1;
void cue_render_pocket_voids(int on) { s_voids = on ? 1 : 0; }
void cue_render_set_cue_tip(float side, float vert, float elev) {
    s_cue_side = side; s_cue_vert = vert; s_cue_elev = elev;
}

int cue_render_project(Vec3 world, float *sx, float *sy, uint16_t *d) {
    Vec3 rel = v3_sub(world, s_view.pos);
    Vec3 vv = m3_mul_v3_t(&s_view.basis, rel);
    if (vv.z <= CUE_NEAR) return 0;
    float inv = 1.0f / vv.z;
    *sx = 64.0f + s_focal * vv.x * inv;
    *sy = 64.0f - s_focal * vv.y * inv;
    if (d) {
        float dd = CUE_DEPTH_K * inv;
        *d = (dd >= 65535.0f) ? 65535u : (dd < 1.0f ? 1u : (uint16_t)dd);
    }
    return 1;
}
/* project + return view-space z (for sphere radius / per-pixel depth). */
static int project_z(Vec3 world, float *sx, float *sy, float *vz) {
    Vec3 rel = v3_sub(world, s_view.pos);
    Vec3 vv = m3_mul_v3_t(&s_view.basis, rel);
    if (vv.z <= CUE_NEAR) return 0;
    float inv = 1.0f / vv.z;
    *sx = 64.0f + s_focal * vv.x * inv;
    *sy = 64.0f - s_focal * vv.y * inv;
    *vz = vv.z;
    return 1;
}

/* ---- table mesh build -------------------------------------------------- */
/* What is being emitted right now. The table is built in runs — the bed, the
 * cushions, then the woodwork — so this is set once per run rather than passed
 * through twenty emitter signatures. */
static uint8_t s_mat = CUE_MAT_CLOTH;

static void tri(Vec3 a, Vec3 b, Vec3 c, uint16_t col) {
    if (s_ntab >= MAX_TABLE_TRI) return;
    CueTri *t = &s_tab[s_ntab++];
    t->v[0] = a; t->v[1] = b; t->v[2] = c;
    t->nrm = v3_norm(v3_cross(v3_sub(b, a), v3_sub(c, a)));
    t->color = col;
    t->mat = s_mat;
}
static void quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, uint16_t col) {
    tri(a, b, c, col); tri(a, c, d, col);
}
/* Ribbon quad a→b→c→d with a CHOSEN diagonal. The cushion strip is non-planar
 * (back verts use per-node normals), so the diagonal must follow the geometry,
 * not the vertex labels — otherwise a jaw renders mirror-broken on one side.
 * alt=0 splits a-c; alt=1 splits b-d. */
static void ribbon(Vec3 a, Vec3 b, Vec3 c, Vec3 d, uint16_t col) {
    /* Split along the SHORTER diagonal — a pure distance test, so mirrored
     * geometry triangulates identically (the jaw was broken on one side
     * because a fixed-label diagonal is not mirror-invariant). */
    if (v3_len2(v3_sub(b, d)) < v3_len2(v3_sub(a, c))) {
        tri(a, b, d, col); tri(b, c, d, col);
    } else {
        tri(a, b, c, col); tri(a, c, d, col);
    }
}

/* Push a felt-boundary point OUT to the cushion back, but ONLY near the CORNER
 * pockets (faded to zero elsewhere) — the corner pocket is set back so its felt
 * needs to run under the cushions to frame it; the middle pockets are already
 * right and must be left untouched. */
static Vec3 jaw_pushed(const CueWorld *w, float pr_corner, float cw, Vec3 P) {
    float dmin = 1e9f;
    for (int p = 0; p < 4 && p < w->npocket; p++) {
        float dx = P.x - w->pocket[p].x, dz = P.z - w->pocket[p].z, d = sqrtf(dx*dx + dz*dz);
        if (d < dmin) dmin = d;
    }
    float thresh = 4.0f * pr_corner;
    float f = (dmin >= thresh) ? 0.0f : (1.0f - dmin/thresh);
    float push = cw * 1.7f * f;                 /* run the corner felt well under the frame */
    float l = sqrtf(P.x*P.x + P.z*P.z) + 1e-6f;
    return v3(P.x + P.x/l*push, 0, P.z + P.z/l*push);
}

/* The pocket-mouth edge as a true CIRCLE arc centred on the FUNCTIONAL drop
 * (`pc`, which moves with the pocket setback), radius `r`, swept the short way
 * between the two jaw tips a→b (so it faces the table). The endpoints are pinned
 * to the jaw tips so the felt still meets the cushion noses. Both the bed cut and
 * the lip use this, so they share one curve anchored to the drop — not the jaws. */
static void pocket_circ_arc(Vec3 pc, float r, Vec3 a, Vec3 b, Vec3 *arc, int N) {
    float a0 = atan2f(a.z - pc.z, a.x - pc.x);
    float a1 = atan2f(b.z - pc.z, b.x - pc.x);
    while (a1 - a0 >  3.14159265f) a1 -= 6.2831853f;
    while (a1 - a0 < -3.14159265f) a1 += 6.2831853f;
    for (int k = 0; k <= N; k++) {
        if (k == 0)      { arc[k] = v3(a.x, 0, a.z); continue; }
        if (k == N)      { arc[k] = v3(b.x, 0, b.z); continue; }
        float th = a0 + (a1 - a0) * (float)k / N;
        arc[k] = v3(pc.x + r*cosf(th), 0, pc.z + r*sinf(th));
    }
}


/* A point at distance `tt` counter-clockwise round the rectangle, starting at
 * (+ex,-ez). One place that knows the perimeter's shape. */
static Vec3 walk_pt(float ex, float ez, float tt) {
    float per = 4.0f * (ex + ez);
    while (tt < 0.0f) tt += per;
    while (tt >= per) tt -= per;
    float w2 = 2.0f * ex, h2 = 2.0f * ez;
    if (tt < h2)            return v3( ex, 0, -ez + tt);
    tt -= h2;
    if (tt < w2)            return v3( ex - tt, 0,  ez);
    tt -= w2;
    if (tt < h2)            return v3(-ex, 0,  ez - tt);
    tt -= h2;
    return v3(-ex + tt, 0, -ez);
}

/* ---- the cloth boundary --------------------------------------------------- *
 *
 * A table is a rectangle of slate that runs OUT UNDER THE CUSHIONS, with a
 * scallop cut into its edge at each pocket. Look down on a stripped table, or a
 * dressed one from above: the baize reaches the wood all the way round, and at
 * each pocket it sweeps away in a single curve.
 *
 * The bed used to stop dead at the cushion NOSE, so there was no slate under
 * the rails at all — at a corner you looked past the frame and out of the
 * bottom of the table, and at a middle the same hole showed as two crescents
 * either side of the drop.
 *
 * WHERE THE SLATE EDGE GOES. On the pocket-centre line, and that is derived
 * rather than dialled in: put it there and the pocket circle is exactly HALF
 * outside the slate at a middle and exactly a QUARTER at a corner, so the
 * scallop comes out at 180 and 90 degrees on its own, on every table in the
 * set, with nothing at all behind the pocket. Which is what a real one does.
 *
 * The boundary is a real polygon — straight runs along the slate edges and
 * uniform arc vertices around each scallop. Not a grid of cells clipped against
 * a circle: that gives stair-steps, and stair-steps are what a pocket must not
 * have. */

/* THE POCKET CUT, as measured on the bench against the real drop, bore and
 * cushion geometry of all seven tables, and signed off:
 *
 *   corner arc = 1.11 x the ball's drop circle
 *   middle arc = 1.06 x
 *   arc centre = the pocket itself
 *   cloth      = out to 98% of the rail's width, under the cushions
 *
 * One pair of numbers for the whole set — every table's drop radius already
 * carries its own size, so the ratio is what transfers and the millimetres
 * follow. */
/* The numbers themselves, and the live tuning of them, belong to the table
 * (CUE_CUT_* in cue_table.h): the edge of this cut is where the ball tips over,
 * so the physics needs the identical curve and there can only be one of it. The
 * renderer reads the derived arc straight out of the world. */
#define CUE_SLATE_RAIL  0.98f

#define CUE_BND_MAX 1400

/* ONE POCKET's slice of that boundary, which is all the lip ever walks at a
 * time. scallop_into emits (NL+1) leg points, then an arc of NA/2+1 (corner) or
 * NA+1 (side), then (NL+1) more: 4 + 41 + 4 = 49 at the widest. 64 is that with
 * room to spare.
 *
 * The lip's working rings used to be CUE_BND_MAX long — the whole table's
 * boundary, for a run that can only ever be one pocket's worth. Three of them
 * on the stack came to 50 KB, against a device that has 134 KB of RAM for the
 * entire game, so the handheld could not have drawn a pocket without going
 * through the bottom of its own stack. */
#define CUE_LIP_MAX 64

typedef struct {
    Vec3 p[CUE_BND_MAX];
    int  pk[CUE_BND_MAX];      /* which pocket this point sits on, or -1 */
    int  n;
} CueBnd;
static CueBnd s_bnd;           /* shared by the bed cut and the lip */

/* THE CLOTH GOES TO THE WOOD. All the way out under the cushions to the frame's
 * inner face — that is what you see looking down on a real table, baize right up
 * to the timber the whole way round. Stopping it at the pocket-centre line left
 * a strip of daylight between the cloth and the frame. */
static void slate_extent(const CueTable *t, const CueWorld *w, float *ex, float *ez) {
    (void)w;
    *ex = t->half_len + t->rail_w * CUE_SLATE_RAIL;
    *ez = t->half_wid + t->rail_w * CUE_SLATE_RAIL;
}

static void arc_into(CueBnd *B, Vec3 c, float R, float th0, float th1, int pk, int N) {
    for (int k = 0; k <= N && B->n < CUE_BND_MAX; k++) {
        float th = th0 + (th1 - th0) * (float)k / N;
        B->p[B->n] = v3(c.x + R*cosf(th), 0, c.z + R*sinf(th));
        B->pk[B->n] = pk; B->n++;
    }
}
static void pt_into(CueBnd *B, float x, float z, int pk) {
    if (B->n >= CUE_BND_MAX) return;
    B->p[B->n] = v3(x, 0, z); B->pk[B->n] = pk; B->n++;
}

/* WHERE THE ARC IS CENTRED — its own point, tied to nothing else.
 *
 * Not the slate's corner, not the slate's edge, not the cushion line. Those
 * were all tried and each one dragged the opening's shape around with the
 * slab's dimensions. It sits near the pocket and it is a free parameter, so it
 * can be set by eye against a photograph and then written down.
 *
 * CUE_SCOF nudges it inward from the pocket, in millimetres. */
static Vec3 scallop_centre(const CueWorld *w, int p) {
    /* A corner sets back along its own diagonal, into the corner; a MIDDLE
     * POCKET'S ARC SITS DEEPER, straight back toward the frame — its centre on
     * the pocket puts the curve too far into the table, and set back it reads
     * as a mouth cut into the rail instead of a bite out of the bed. Both are
     * worked out once, by the table. */
    return w->cut_c[p];
}

/* THE SCALLOP: an arc with straight legs, not a circle.
 *
 * A slate cutter does not drill a hole. The cut is an ARC around the pocket —
 * a quarter of one at a corner, a half at a middle — and from each end of that
 * arc a STRAIGHT LINE runs out to the edge of the slab. The lines are tangent
 * to the arc, so the two meet without a corner, and the arc's centre has
 * nothing to do with where the slate's edge or the cushions are: it sits on the
 * pocket, and its radius is its own.
 *
 * Cutting a whole circle instead is what produced a shape bounded by two curves
 * meeting at a cusp, and made the opening depend on the slab's corner.
 *
 * CUE_SCAL sets the radius as a percentage of the ball's drop circle. */
static float scallop_rad(const CueWorld *w, int p) {
    return w->cut_r[p];
}

/* One pocket's cut, in boundary order: leg in, arc, leg out. `sx`,`sz` are the
 * quadrant it sits in; a middle pocket has no x side, so it gets the half arc
 * and two parallel legs to the one edge it is cut into. */
static void scallop_into(CueBnd *B, const CueTable *t, const CueWorld *w, int p,
                         float ex, float ez, int reverse) {
    Vec3 C = scallop_centre(w, p);
    float R = scallop_rad(w, p);
    float sz = (C.z < 0) ? -1.0f : 1.0f;
    const float PI = 3.14159265f;
    const int NA = 40;
    Vec3 e1, e2, l1, l2;
    float a1, a2;
    if (p < 4) {                                  /* a corner: a quarter arc */
        float sx = (C.x < 0) ? -1.0f : 1.0f;
        e1 = v3(C.x - sx*R, 0, C.z);              /* leg drops to the z edge */
        e2 = v3(C.x, 0, C.z - sz*R);              /* leg runs to the x edge */
        l1 = v3(e1.x, 0, sz*ez);
        l2 = v3(sx*ex, 0, e2.z);
        a1 = (sx < 0) ? 0.0f : PI;
        a2 = (sz < 0) ? 0.5f*PI : -0.5f*PI;
        /* sweep through the inward diagonal */
        float ai = atan2f(-sz, -sx);
        float d = a2 - a1;
        while (d >  PI) d -= 2.0f*PI;
        while (d < -PI) d += 2.0f*PI;
        float di = ai - a1;
        while (di >  PI) di -= 2.0f*PI;
        while (di < -PI) di += 2.0f*PI;
        if ((d > 0.0f) != (di > 0.0f)) d += (d > 0.0f) ? -2.0f*PI : 2.0f*PI;
        a2 = a1 + d;
    } else {                                      /* a middle: a half arc */
        e1 = v3(C.x - R, 0, C.z);
        e2 = v3(C.x + R, 0, C.z);
        l1 = v3(e1.x, 0, sz*ez);
        l2 = v3(e2.x, 0, sz*ez);
        a1 = PI; a2 = 0.0f;
        if (sz > 0.0f) { a1 = 0.0f; a2 = -PI; e1 = v3(C.x + R,0,C.z); e2 = v3(C.x - R,0,C.z);
                         l1 = v3(e1.x,0,sz*ez); l2 = v3(e2.x,0,sz*ez); }
    }
    if (reverse) { Vec3 t1=l1; l1=l2; l2=t1; t1=e1; e1=e2; e2=t1;
                   float f=a1; a1=a2; a2=f; }
    /* THE LEGS BELONG TO THE CUT. They carry the same rolled edge as the arc —
     * a straight run of slate edge is still a slate edge. Left flat they showed
     * as square tabs sticking out of the pocket, which is exactly what a
     * turn-down that stops at the arc looks like. Sampled, not just their two
     * ends, so the roll has vertices to follow along them. */
    const int NL = 3;      /* a leg is straight; it needs ends, not samples */
    for (int k = 0; k <= NL; k++) {
        float u = (float)k / NL;
        pt_into(B, l1.x + (e1.x - l1.x) * u, l1.z + (e1.z - l1.z) * u, p);
    }
    arc_into(B, C, R, a1, a2, p, (p < 4) ? NA/2 : NA);
    for (int k = 0; k <= NL; k++) {
        float u = (float)k / NL;
        pt_into(B, e2.x + (l2.x - e2.x) * u, e2.z + (l2.z - e2.z) * u, p);
    }
    (void)t;
}

/* ---- the same cut, described by the rails instead of by the quadrant ------
 *
 * scallop_into above works out everything from the SIGN of the pocket's
 * coordinates: which quadrant it is in, which way its legs run, whether it is a
 * corner at all (`p < 4`). Every one of those is true of a rectangle centred on
 * the origin and none of them survives an L, whose pockets sit in quadrants
 * that say nothing about which rails meet there — the corner under the notch is
 * at +x,+z and its cloth is to −x,−z of it.
 *
 * So this one is told, rather than guessing: the outward normal of the rail
 * arriving at the pocket and of the rail leaving it, and how far out the slate
 * edge lies along each. The arc runs on the CLOTH side between the two, and
 * each leg runs from an arc end out to its own rail's slate line. For a middle
 * both rails are the same rail, so the arc is a half circle and the two legs
 * are parallel — which is exactly what the rectangle version special-cases.
 *
 * `sa`/`sb` are signed slate coordinates: the x of a ±x rail, the z of a ±z one.
 */
static void scallop_rails(CueBnd *B, const CueWorld *w, int p,
                          Vec3 n_a, float sa, Vec3 n_b, float sb, Vec3 along) {
    const Vec3 C = scallop_centre(w, p);
    const float R = scallop_rad(w, p);
    const float PI = 3.14159265f;
    const int NL = 3;
    Vec3 e_a, e_b;
    int mid = (n_a.x * n_b.x + n_a.z * n_b.z) > 0.9f;   /* the same rail twice */

    if (mid) {
        /* half a circle, entered against the walk and left with it */
        e_a = v3(C.x - along.x * R, 0, C.z - along.z * R);
        e_b = v3(C.x + along.x * R, 0, C.z + along.z * R);
    } else {
        /* THE ENDS ARE CROSSED, and this is the whole of the corner cut.
         *
         * The arc end reached FIRST is displaced along the rail being LEFT by,
         * and runs out to the slate line of the rail being ARRIVED on; the last
         * end is the other way about. Pairing each end with its own rail — the
         * obvious reading, and the one I wrote — sends each leg out along the
         * line it already lies on, so it folds back on itself and the lip rolls
         * a strip of cloth over at right angles to where it belongs. That is
         * the twisted patch beside the pocket. */
        e_a = v3(C.x - n_b.x * R, 0, C.z - n_b.z * R);
        e_b = v3(C.x - n_a.x * R, 0, C.z - n_a.z * R);
    }
    /* first leg out to the ARRIVING rail's slate line, last to the LEAVING
     * one's — and for a middle both of those are the same rail anyway. */
    Vec3 l_a = (n_a.x != 0.0f) ? v3(sa, 0, e_a.z) : v3(e_a.x, 0, sa);
    Vec3 l_b = (n_b.x != 0.0f) ? v3(sb, 0, e_b.z) : v3(e_b.x, 0, sb);

    /* the arc, the short way round, through the cloth side */
    float a1 = atan2f(e_a.z - C.z, e_a.x - C.x);
    float a2 = atan2f(e_b.z - C.z, e_b.x - C.x);
    float d = a2 - a1;
    while (d >  PI) d -= 2.0f*PI;
    while (d < -PI) d += 2.0f*PI;
    if (mid) {
        /* a half circle is exactly PI and the short way is ambiguous, so it is
         * chosen: through the point furthest from the rail, which is the cloth
         * side. Left to the wrap above it can sweep the frame side and the cut
         * appears as a bite out of the timber instead of out of the bed. */
        Vec3 inw = v3(-n_a.x, 0, -n_a.z);
        float ai = atan2f(inw.z, inw.x), di = ai - a1;
        while (di >  PI) di -= 2.0f*PI;
        while (di < -PI) di += 2.0f*PI;
        d = (di > 0.0f) ? PI : -PI;
    }

    for (int k = 0; k <= NL; k++) {
        float u = (float)k / NL;
        pt_into(B, l_a.x + (e_a.x - l_a.x) * u, l_a.z + (e_a.z - l_a.z) * u, p);
    }
    arc_into(B, C, R, a1, a1 + d, p, mid ? 40 : 20);
    for (int k = 0; k <= NL; k++) {
        float u = (float)k / NL;
        pt_into(B, e_b.x + (l_b.x - e_b.x) * u, e_b.z + (l_b.z - e_b.z) * u, p);
    }
}

/* THE L's CLOTH: the same walk, round six rails and seven pockets.
 *
 * Pocket order is the outline order build_L adds them in — V0, the bottom
 * middle, V1, V2, V4, the top middle, V5 — and the elbow between V2 and V4 is
 * not a pocket at all, so the boundary simply turns the corner there. */
static void build_bed_boundary_L(const CueTable *t, const CueWorld *w,
                                 CueBnd *B, float ex, float ez) {
    if (w->npocket < 7) return;
    const float m  = t->rail_w * CUE_SLATE_RAIL;
    const float hl = t->half_len, hw = t->half_wid;
    const float nx = t->notch_x,  nz = t->notch_z;
    /* A LEFT-HANDED L IS THE MIRROR OF A RIGHT-HANDED ONE, and this walk is
     * written for the right-handed one. So the mirror is applied to what goes
     * IN — every z, every z-normal, and the pocket indices, which the world
     * reversed when it mirrored itself — and then to what comes OUT: mirroring
     * reverses the winding, and the bed is fanned off this boundary in order, so
     * the points are reversed at the end to put it back.
     *
     * Same transformation as the cushion chain and the frame's outline, applied
     * the same way for the same reason. Written once, three times over. */
    const float h = cue_table_hand(t);
    const int   np = w->npocket;
    #define PK(i)   (h < 0.0f ? (np - 1 - (i)) : (i))
    #define ZN(v)   v3((v).x, 0, (v).z * h)
    #define ZS(v)   ((v) * h)
    const int start = B->n;
    /* each rail's outward normal and the slate line it runs out to */
    const Vec3 NZ0 = ZN(v3(0,0,-1)), NX1 = v3(1,0,0);
    const Vec3 NZ1 = ZN(v3(0,0,1)),  NX0 = v3(-1,0,0);
    const float S_BOT = ZS(-ez);             /* z of the bottom slate edge */
    const float S_RIGHT =  ex;               /* x of the right slate edge  */
    const float S_NOTCH_Z = ZS((hw - nz) + m); /* z of the notch's underside */
    const float S_NOTCH_X = (hl - nx) + m;   /* x of the notch's inner side */
    const float S_TOP = ZS(ez);
    const float S_LEFT = -ex;
    const Vec3 PX = v3(1,0,0), MX = v3(-1,0,0);
    const Vec3 MZ = ZN(v3(0,0,-1));
    /* V0: in off the short arm's outer rail, out along the long arm's */
    scallop_rails(B, w, PK(0), NX0, S_LEFT,    NZ0, S_BOT,      PX);
    /* the long arm's middle, walking +x */
    scallop_rails(B, w, PK(1), NZ0, S_BOT,     NZ0, S_BOT,      PX);
    /* V1: in off that rail, out up the far end */
    scallop_rails(B, w, PK(2), NZ0, S_BOT,     NX1, S_RIGHT,    PX);
    /* V2: in off the far end, out along the underside of the notch */
    scallop_rails(B, w, PK(3), NX1, S_RIGHT,   NZ1, S_NOTCH_Z,  MX);
    /* ...the elbow, which is timber and not a pocket: the slate just turns */
    pt_into(B, S_NOTCH_X, S_NOTCH_Z, -1);
    /* V4: in off the notch's inner rail, out along the short arm's top */
    scallop_rails(B, w, PK(4), NX1, S_NOTCH_X, NZ1, S_TOP,      MX);
    /* V5: in off the top, out down the short arm's outer rail */
    scallop_rails(B, w, PK(5), NZ1, S_TOP,     NX0, S_LEFT,     MZ);
    /* ...and that rail's middle, walking -z */
    scallop_rails(B, w, PK(6), NX0, S_LEFT,    NX0, S_LEFT,     MZ);
    if (h < 0.0f) {
        for (int i = start, j = B->n - 1; i < j; i++, j--) {
            Vec3 p = B->p[i]; B->p[i] = B->p[j]; B->p[j] = p;
            int  k = B->pk[i]; B->pk[i] = B->pk[j]; B->pk[j] = k;
        }
    }
    #undef PK
    #undef ZN
    #undef ZS
}

/* The whole cloth boundary: six cuts, joined by the slate's straight edges.
 * Each cut supplies its own two leg ends, so the "edges" are simply the lines
 * between one cut's exit and the next one's entry, and there is nothing to
 * clip or detect. */
static void build_bed_boundary(const CueTable *t, const CueWorld *w, CueBnd *B) {
    float ex, ez; slate_extent(t, w, &ex, &ez);
    B->n = 0;

    /* ---- S1: AN L IS ITS OWN OUTLINE ------------------------------------
     *
     * Everything below this assumes a rectangle, in four separate ways: six
     * named pockets found BY THE SIGN of x and z, `p < 4` meaning corner, each
     * scallop's legs running out to ±ex/±ez, and the whole thing emitted as
     * bottom-right-top-left. None of those survives a shape with five corners,
     * seven pockets and a vertex that turns inward.
     *
     * So an L gets its outline drawn plainly: the six vertices, out at the
     * slate edge, and no scalloped pocket cut-aways. The SHAPE is right, which
     * is what decides whether the cloth you see is the cloth the balls bounce
     * off — and being right about that matters far more than the arcs, which
     * are a detail of how the cloth is cut around each hole.
     *
     * Generalising the scallops to an arbitrary outline is real work and it is
     * S2's, not this item's. Said plainly so nobody reads a square-cornered
     * pocket as a bug. */
    if (t->bed_shape == CUE_BED_L) { build_bed_boundary_L(t, w, B, ex, ez); return; }

    int BL=-1,BR=-1,TR=-1,TL=-1,MB=-1,MT=-1;
    for (int p = 0; p < w->npocket; p++) {
        Vec3 q = w->pocket[p];
        if (p < 4) {
            if (q.x < 0 && q.z < 0) BL = p; else if (q.x > 0 && q.z < 0) BR = p;
            else if (q.x > 0 && q.z > 0) TR = p; else TL = p;
        } else { if (q.z < 0) MB = p; else MT = p; }
    }
    /* counter-clockwise: bottom edge, right, top, left */
    if (BL >= 0) scallop_into(B, t, w, BL, ex, ez, 1);
    if (MB >= 0) scallop_into(B, t, w, MB, ex, ez, 0);
    if (BR >= 0) scallop_into(B, t, w, BR, ex, ez, 0);
    if (TR >= 0) scallop_into(B, t, w, TR, ex, ez, 1);
    if (MT >= 0) scallop_into(B, t, w, MT, ex, ez, 0);
    if (TL >= 0) scallop_into(B, t, w, TL, ex, ez, 0);
}

/* Baize lip (the drop): rolls the cloth down into each pocket throat. Emitted
 * AFTER the pocket voids so depth-test layers it OVER the void (no rim cutting
 * across it) while the raised cushions still occlude its sides. */
static void emit_pocket_lips(const CueTable *t, const CueWorld *w) {
    if (!s_lip_mode) return;
    /* Roll the cloth over the edge of the cut and down into the pocket.
     *
     * Along the WHOLE cut — leg, arc, leg — as one continuous edge, because
     * that is what it is. The roll direction is the boundary's own outward
     * normal rather than a radius from the arc centre: a radius is only the
     * right answer on the arc, and on the legs it points somewhere useless.
     *
     * The drop and its profile are the tuned ones: a quarter-cosine fall of
     * 0.45 of the pocket radius, darkening as the cloth turns under. */
    /* CUE_LIP is a coarse debug override; the shipped roll is the table's own
     * `roll` number, per pocket type. */
    int M; float lscale;
    switch (s_lip_mode) {
        case 2:  M = 9;  lscale = 1.25f; break;
        case 3:  M = 11; lscale = 1.80f; break;
        default: M = 8;  lscale = 1.0f; break;
    }
    /* Which way is out of the cloth? Take it from the polygon's own winding so
     * it cannot be got backwards by hand. */
    float area = 0.0f;
    for (int i = 0; i < s_bnd.n; i++) {
        Vec3 a = s_bnd.p[i], b = s_bnd.p[(i + 1) % s_bnd.n];
        area += a.x * b.z - b.x * a.z;
    }
    float wind = (area > 0.0f) ? 1.0f : -1.0f;

    int i = 0;
    while (i < s_bnd.n) {
        int p = s_bnd.pk[i];
        if (p < 0) { i++; continue; }
        int j = i;
        while (j < s_bnd.n && s_bnd.pk[j] == p) j++;
        int cnt = j - i;
        if (cnt > CUE_LIP_MAX) cnt = CUE_LIP_MAX;   /* cannot happen; see above */
        if (cnt >= 2) {
            float ld  = w->lip_d[p] * lscale;
            float din = ld;
            Vec3 ring0[CUE_LIP_MAX], nrm[CUE_LIP_MAX];
            for (int k = 0; k < cnt; k++) {
                /* the outward normal, averaged from the two segments meeting here */
                Vec3 a = s_bnd.p[i + (k > 0 ? k - 1 : 0)];
                Vec3 b = s_bnd.p[i + (k + 1 < cnt ? k + 1 : cnt - 1)];
                float dx = b.x - a.x, dz = b.z - a.z;
                float l = sqrtf(dx*dx + dz*dz) + 1e-9f;
                nrm[k] = v3(wind * dz / l, 0, -wind * dx / l);
                ring0[k] = s_bnd.p[i + k];
            }
            for (int sring = 1; sring <= M; sring++) {
                float phi = (float)sring / M * 1.5707963f;
                float tn = sinf(phi), yy = -ld * (1.0f - cosf(phi));
                uint16_t col = shade565(t->cloth, 1.0f - 0.92f*(1.0f - cosf(phi)));
                Vec3 ring1[CUE_LIP_MAX];
                for (int k = 0; k < cnt; k++)
                    ring1[k] = v3(s_bnd.p[i+k].x + nrm[k].x * din * tn, yy,
                                  s_bnd.p[i+k].z + nrm[k].z * din * tn);
                for (int k = 0; k + 1 < cnt; k++)
                    quad(ring0[k], ring0[k+1], ring1[k+1], ring1[k], col);
                for (int k = 0; k < cnt; k++) ring0[k] = ring1[k];
            }
            /* The inside of the pocket below the roll: a WALL dropped straight
             * down from the rolled edge.
             *
             * It used to be a fan from a single point on the pocket floor, and
             * a fan converging on a point is a spray of long thin triangles —
             * visible across the mouth of every pocket as spokes, which is
             * exactly what it was. A skirt has no apex to converge on. Below it
             * the frame's tray closes the table off, so it needs no floor. */
            {
                uint16_t dark = s_is_snooker ? RGB565C(34, 30, 20) : RGB565C(3, 4, 4);
                float fy = s_is_snooker ? -0.105f : -0.055f;
                for (int k = 0; k + 1 < cnt; k++)
                    quad(ring0[k], ring0[k+1],
                         v3(ring0[k+1].x, fy, ring0[k+1].z),
                         v3(ring0[k].x,   fy, ring0[k].z), dark);
            }
        }
        i = j;
    }
}

/* Fill the wood ring around one pocket bore inside the notch box [x0,x1]×[z0,z1].
 *
 * The plank's wood lies entirely on ONE side of the pocket (the rail side); the
 * mouth opens to the bed on the opposite side. A radial (centre-based) fill
 * leaves slivers because the pocket centre sits OUTSIDE the box (behind the
 * cushion). Instead we fill in fine columns ALONG the rail — each column a
 * trapezoid whose inner edge follows the analytic circle (smooth, gap-free) and
 * a vertical wall dropping to `ybot`.
 *
 *   axis    : 0 = wood spans Z (long rails, columns run along X)
 *             1 = wood spans X (short rails, columns run along Z)
 *   rail_hi : 1 = wood is toward the LARGER coord (mouth at the smaller box edge)
 *             0 = wood toward the smaller coord (mouth at the larger box edge) */
static void bore_fill(float cx, float cz, float r, float x0, float x1, float z0, float z1,
                      float ytop, float ybot, uint16_t top, uint16_t wall,
                      int axis, int rail_hi) {
    const int N = CUE_ARC_SEGS * 3;
    for (int k = 0; k < N; k++) {
        if (axis == 0) {                       /* columns along X, depth along Z */
            float u0 = x0 + (x1-x0)*k/N, u1 = x0 + (x1-x0)*(k+1)/N;
            float d0 = r*r-(u0-cx)*(u0-cx), d1 = r*r-(u1-cx)*(u1-cx);
            d0 = d0 > 0 ? sqrtf(d0) : 0; d1 = d1 > 0 ? sqrtf(d1) : 0;
            float zt0, zt1, wa, wb;            /* rim z, wood far edge */
            if (rail_hi) { zt0 = cz+d0; zt1 = cz+d1; wa = wb = z1; }   /* wood toward +z */
            else         { zt0 = cz-d0; zt1 = cz-d1; wa = wb = z0; }   /* wood toward -z */
            if (zt0 < z0) zt0 = z0; if (zt0 > z1) zt0 = z1;
            if (zt1 < z0) zt1 = z0; if (zt1 > z1) zt1 = z1;
            quad(v3(u0,ytop,zt0), v3(u1,ytop,zt1), v3(u1,ytop,wb), v3(u0,ytop,wa), top);
            quad(v3(u0,ytop,zt0), v3(u1,ytop,zt1), v3(u1,ybot,zt1), v3(u0,ybot,zt0), wall);
        } else {                               /* columns along Z, depth along X */
            float u0 = z0 + (z1-z0)*k/N, u1 = z0 + (z1-z0)*(k+1)/N;
            float d0 = r*r-(u0-cz)*(u0-cz), d1 = r*r-(u1-cz)*(u1-cz);
            d0 = d0 > 0 ? sqrtf(d0) : 0; d1 = d1 > 0 ? sqrtf(d1) : 0;
            float xt0, xt1, wa, wb;
            if (rail_hi) { xt0 = cx+d0; xt1 = cx+d1; wa = wb = x1; }   /* wood toward +x */
            else         { xt0 = cx-d0; xt1 = cx-d1; wa = wb = x0; }   /* wood toward -x */
            if (xt0 < x0) xt0 = x0; if (xt0 > x1) xt0 = x1;
            if (xt1 < x0) xt1 = x0; if (xt1 > x1) xt1 = x1;
            quad(v3(xt0,ytop,u0), v3(xt1,ytop,u1), v3(wb,ytop,u1), v3(wa,ytop,u0), top);
            quad(v3(xt0,ytop,u0), v3(xt1,ytop,u1), v3(xt1,ybot,u1), v3(xt0,ybot,u0), wall);
        }
    }
}

/* A wood rail plank [xa,xb]×[za,zb] with a clean round bore at each pocket: cut a
 * rectangular notch (the pocket's clipped bounding box) from the plank top, then
 * fill it with bore_fill so the visible cut edge is the smooth circle curve. */
static void wood_plank_bored(float xa, float xb, float za, float zb,
                             float ytop, float ybot, uint16_t top, uint16_t wall,
                             const float *hx, const float *hz, const float *hr, int nh,
                             int axis, int rail_hi, float ylow, uint16_t lip) {
    /* notches (clipped pocket bounding boxes) on this plank — note TWO pockets
     * can share the same x-range (the two corners of a short rail). */
    float nx0[CUE_MAX_POCKET], nx1[CUE_MAX_POCKET], nz0[CUE_MAX_POCKET], nz1[CUE_MAX_POCKET];
    int   pid[CUE_MAX_POCKET]; int ni = 0;
    for (int h = 0; h < nh; h++) {
        if (hz[h]+hr[h] <= za || hz[h]-hr[h] >= zb) continue;
        float a = hx[h]-hr[h], b = hx[h]+hr[h];
        if (a < xa) a = xa; if (b > xb) b = xb;
        if (b <= a + 1e-5f) continue;
        float c = hz[h]-hr[h], d = hz[h]+hr[h];
        if (c < za) c = za; if (d > zb) d = zb;
        nx0[ni]=a; nx1[ni]=b; nz0[ni]=c; nz1[ni]=d; pid[ni]=h; ni++;
    }
    /* wood top = plank minus the notch rectangles, split into x-columns at every
     * notch edge (so overlapping-x notches are both subtracted). */
    float ex[2*CUE_MAX_POCKET + 2]; int ne = 0;
    ex[ne++] = xa; ex[ne++] = xb;
    for (int i = 0; i < ni; i++) { ex[ne++] = nx0[i]; ex[ne++] = nx1[i]; }
    for (int i = 1; i < ne; i++) {                              /* sort x-edges */
        float e = ex[i]; int j = i-1;
        while (j >= 0 && ex[j] > e) { ex[j+1] = ex[j]; j--; }
        ex[j+1] = e;
    }
    for (int c = 0; c < ne-1; c++) {
        float cx0 = ex[c], cx1 = ex[c+1];
        if (cx1 <= cx0 + 1e-5f) continue;
        float mx = 0.5f*(cx0+cx1);
        float lo[8], hi[8]; int ns = 1; lo[0] = za; hi[0] = zb;
        for (int i = 0; i < ni; i++) {                          /* subtract active notches */
            if (mx < nx0[i]-1e-5f || mx > nx1[i]+1e-5f) continue;
            float clo = nz0[i], chi = nz1[i];
            float nlo[8], nhi[8]; int nn = 0;
            for (int s = 0; s < ns && nn < 7; s++) {
                if (chi <= lo[s] || clo >= hi[s]) { nlo[nn]=lo[s]; nhi[nn]=hi[s]; nn++; continue; }
                if (clo > lo[s]) { nlo[nn]=lo[s]; nhi[nn]=clo; nn++; }
                if (chi < hi[s] && nn < 8) { nlo[nn]=chi; nhi[nn]=hi[s]; nn++; }
            }
            ns = nn; for (int s = 0; s < ns; s++) { lo[s]=nlo[s]; hi[s]=nhi[s]; }
        }
        int inner_col = (axis == 1) && (rail_hi ? (cx0 <= xa+1e-4f) : (cx1 >= xb-1e-4f));
        float ix = rail_hi ? xa : xb;          /* x-plank inner (mouth) edge */
        for (int s = 0; s < ns; s++) {
            if (hi[s]-lo[s] <= 1e-4f) continue;
            quad(v3(cx0,ytop,lo[s]), v3(cx1,ytop,lo[s]), v3(cx1,ytop,hi[s]), v3(cx0,ytop,hi[s]), top);
            /* inner-edge riser — only where wood actually reaches the mouth edge,
             * so pocket mouths stay open (no wood line across the side pockets). */
            if (ylow < ytop) {
                if (axis == 0 && rail_hi && lo[s] <= za+1e-4f)
                    quad(v3(cx0,ytop,za), v3(cx1,ytop,za), v3(cx1,ylow,za), v3(cx0,ylow,za), lip);
                else if (axis == 0 && !rail_hi && hi[s] >= zb-1e-4f)
                    quad(v3(cx0,ytop,zb), v3(cx1,ytop,zb), v3(cx1,ylow,zb), v3(cx0,ylow,zb), lip);
                else if (inner_col)
                    quad(v3(ix,ytop,lo[s]), v3(ix,ytop,hi[s]), v3(ix,ylow,hi[s]), v3(ix,ylow,lo[s]), lip);
            }
        }
    }
    for (int i = 0; i < ni; i++)                                /* bore EACH pocket */
        bore_fill(hx[pid[i]], hz[pid[i]], hr[pid[i]], nx0[i], nx1[i], nz0[i], nz1[i],
                  ytop, ybot, top, wall, axis, rail_hi);
}

/* ---- cloth markings (baulk line / D / spots) -------------------------- */
#define MARK_Y 0.0015f      /* a hair above the cloth so markings sit on top */
static void cloth_line(float ax, float az, float bx, float bz, float w, uint16_t col) {
    float dx = bx-ax, dz = bz-az, l = sqrtf(dx*dx+dz*dz);
    if (l < 1e-6f) return;
    float px = -dz/l*w*0.5f, pz = dx/l*w*0.5f;
    quad(v3(ax+px,MARK_Y,az+pz), v3(bx+px,MARK_Y,bz+pz),
         v3(bx-px,MARK_Y,bz-pz), v3(ax-px,MARK_Y,az-pz), col);
}
static void cloth_disc(float cx, float cz, float r, uint16_t col) {
    const int N = 8; Vec3 c = v3(cx, MARK_Y, cz);
    for (int k = 0; k < N; k++) {
        float a0 = k*(6.2831853f/N), a1 = (k+1)*(6.2831853f/N);
        tri(c, v3(cx+r*cosf(a0),MARK_Y,cz+r*sinf(a0)),
               v3(cx+r*cosf(a1),MARK_Y,cz+r*sinf(a1)), col);
    }
}
static void cloth_arc(float cx, float cz, float r, float a0, float a1, float w, uint16_t col) {
    const int N = CUE_ARC_SEGS * 3;
    for (int k = 0; k < N; k++) {
        float t0 = a0 + (a1-a0)*k/N, t1 = a0 + (a1-a0)*(k+1)/N;
        cloth_line(cx+r*cosf(t0), cz+r*sinf(t0), cx+r*cosf(t1), cz+r*sinf(t1), w, col);
    }
}
/* Baulk line + D (snooker & UK8), the six colour spots (snooker), or the foot
 * spot (US pool). Drawn in the bed layer so balls/cushions/shadows occlude them. */
/* A spot, and a line across the table, both placed along the SPINE — so on an
 * L the baulk line lies across the arm it belongs to and the D bulges back down
 * it, instead of both being drawn across the bounding box and running off the
 * cloth into the missing corner. On a rectangle cue_table_lay returns exactly
 * what it is given and these are the calls that were here. */
static void lay_spot(const CueTable *t, float x, float across, float sr, uint16_t c) {
    Vec3 p = cue_table_lay(t, x, across, NULL);
    cloth_disc(p.x, p.z, sr, c);
}
static void lay_line(const CueTable *t, float x, float half, float lw, uint16_t c) {
    Vec3 a = cue_table_lay(t, x, -half, NULL);
    Vec3 b = cue_table_lay(t, x,  half, NULL);
    cloth_line(a.x, a.z, b.x, b.z, lw, c);
}
static void emit_table_markings(const CueTable *t) {
    uint16_t lc = shade565(t->cloth, 1.65f);     /* lighter cloth line */
    uint16_t sc = RGB565C(220, 220, 205);        /* spot — off-white */
    float hw = t->half_wid, hl = t->half_len, R = t->R;
    float lw = R * 0.22f, sr = R * 0.42f;
    if (t->is_snooker || t->kind == CUE_GAME_UK8) {
        float bx = t->baulk_x, dr = t->d_radius;
        lay_line(t, bx, hw - R*0.5f, lw, lc);                       /* baulk line */
        /* The D, swept in the baulk line's own frame: a half-circle of points
         * on the baulk side. Drawn as a fan of short chords rather than by
         * cloth_arc, because on an L the frame is rotated and cloth_arc only
         * knows about world x and z. */
        {   const int N = 22;
            Vec3 prev = cue_table_lay(t, bx, -dr, NULL);
            for (int i = 1; i <= N; i++) {
                float a = 3.14159265f * (float)i / (float)N;   /* -dr -> +dr */
                /* along the spine is NEGATIVE on the baulk side */
                Vec3 q = cue_table_lay(t, bx - dr * sinf(a),
                                          -dr * cosf(a), NULL);
                cloth_line(prev.x, prev.z, q.x, q.z, lw, lc);
                prev = q;
            } }
    }
    if (t->is_snooker) {
        lay_spot(t, t->baulk_x,  t->d_radius, sr, sc);   /* yellow */
        lay_spot(t, t->baulk_x, -t->d_radius, sr, sc);   /* green  */
        lay_spot(t, t->baulk_x,  0.0f,        sr, sc);   /* brown  */
        lay_spot(t, t->blue_x,   0.0f,        sr, sc);   /* blue   */
        lay_spot(t, t->pink_x,   0.0f,        sr, sc);   /* pink   */
        lay_spot(t, t->black_x,  0.0f,        sr, sc);   /* black  */
    } else {
        lay_spot(t, hl * 0.5f, 0.0f, sr, sc);            /* foot spot (rack apex) */
        /* US-style tables (US 8/9-ball, Chinese 8-ball) break from behind the
         * head string ("kitchen line") — a line across the bed at -hl/2 with a
         * head spot. UK8 uses the baulk line + D drawn above instead. */
        if (t->kind == CUE_GAME_US8 || t->kind == CUE_GAME_US9 ||
            t->kind == CUE_GAME_CN8) {
            lay_line(t, -hl * 0.5f, hw - R*0.5f, lw, lc);   /* head string */
            lay_spot(t, -hl * 0.5f, 0.0f, sr, sc);          /* head spot */
        }
    }
}

void cue_render_build_table(const CueTable *t, const CueWorld *w) {
    /* Lip roll is scaled to each pocket's mouth radius (pr), so mode 1 already
     * gives a proportionate cloth fall on every table — the snooker drop only
     * looked hard earlier because the lips were being dropped (buffer overflow),
     * not because the roll was too shallow. */
    s_lip_mode = 1;
    { extern char *getenv(const char*); const char *e = getenv("CUE_LIP"); if (e) s_lip_mode = e[0]-'0'; }
    { extern char *getenv(const char*); const char *e2 = getenv("CUE_BALLSET"); if (e2) s_ball_set = e2[0]-'0'; }
    s_ntab = 0;
    s_mat = CUE_MAT_CLOTH;   /* the bed and the cushions are cloth; the run below
                              * switches to timber when the woodwork starts */
    s_cloth = t->cloth;
    s_ballR = t->R;
    s_is_snooker = t->is_snooker;
    s_cloth_shadow = shade565(t->cloth, 0.42f);   /* cloth bounce tint */
    s_bg_top = RGB565C(24, 26, 36);
    s_bg_bot = RGB565C(6, 7, 12);
    const float hl = t->half_len, hw = t->half_wid;
    const float rw = t->rail_w;
    const float cw = rw * 0.63f;        /* cushion depth (nose → cushion back); +50% for a beefier rail */
    const float nose_h = t->cushion_h;       /* nose contact line (bottom of the front face) */
    const float flat_h = nose_h * 1.30f;     /* top of the small VERTICAL nose front face */
    const float rail_h = flat_h;             /* flat cushion top & wood top, level at flat_h */
    uint16_t wood = t->rail, woodt = t->rail_top;

    /* Cloth bed — the slate polygon (see build_bed_boundary): out under the
     * cushions to the pocket-centre line, with a scallop at each pocket. Fanned
     * from the centre, which is safe because the shape is star-shaped about it:
     * every scallop bites inward but none of them reaches the middle. */
    build_bed_boundary(t, w, &s_bnd);
#ifdef MOTE_HOST
    /* Host only, and not merely because there is nowhere to write on a Thumby
     * Color: naming fopen at all drags newlib's whole file layer into the link,
     * and that layer wants a _open the device does not have. A debugging
     * convenience must not decide whether the game builds for hardware. */
    { const char *e = getenv("CUE_BNDDUMP");
      if (e) { FILE *f = fopen(e, "w");
        if (f) { for (int i = 0; i < s_bnd.n; i++)
                   fprintf(f, "%.6f %.6f %d\n", s_bnd.p[i].x, s_bnd.p[i].z, s_bnd.pk[i]);
                 fclose(f); } } }
#endif
    for (int i = 0; i < s_bnd.n; i++) {
        Vec3 a = s_bnd.p[i], b = s_bnd.p[(i + 1) % s_bnd.n];
        tri(v3(0, 0, 0), a, b, t->cloth);
    }
    /* The baulk line, the D and the spots, as GEOMETRY — flat quads laid on the
     * bed. That is the only way the handheld can have them: it has no shader to
     * paint with. A host that DOES paint them has to turn this off, or the two
     * are drawn one over the other — which is exactly what CueVR looked like: a
     * baulk line twice, the painted one tucking under the cushion nose with the
     * cloth and the quad standing proud of it and stretching in the distance. */
    if (s_markings) emit_table_markings(t);
    s_bed_ntab = s_ntab;   /* everything after here is raised (cushions/frame/voids) */

    /* Cushions from the chain segments: steep cloth playing face up to the
     * nose, then a cloth top sloping back to the cushion back. The facings
     * (which splay outward) shape the jaws automatically. */
    /* Cushion cross-section (K66-ish): from the bed it leans FORWARD up to the
     * protruding nose (the contact line at ~nose_h), a small vertical flat just
     * above the nose, then the cloth top slopes back to the rail. The base is
     * set back from the nose by `ub` so the nose overhangs (the "cut in below"). */
    uint16_t fdark = shade565(t->cloth, 0.55f);   /* undercut face (in shadow) */
    uint16_t face  = shade565(t->cloth, 0.72f);   /* the vertical nose front face */
    uint16_t ctop  = shade565(t->cloth, 0.92f);   /* cloth top to the rail */
    const float ub = 0.45f * t->R;                /* undercut / overhang */
    for (int s = 0; s < w->nseg; s++) {
        const CueSeg *sg = &w->seg[s];
        /* Per-NODE back normal: average with the neighbouring segment when they
         * share an endpoint, so adjacent cushion tops share their back vertices
         * — a continuous strip with no V-gaps (the "holes in the top"). */
        Vec3 pa = sg->a, pb = sg->b, na = sg->n, nb = sg->n;
        /* Where a FREE tip's back lands on the wood, worked out below. */
        Vec3 fba = v3(0,0,0), fbb = v3(0,0,0);
        int haveFba = 0, haveFbb = 0;
        int sharedA = 0, sharedB = 0;
        if (s > 0) {
            const CueSeg *pr = &w->seg[s-1];
            if (v3_len2(v3_sub(pr->b, sg->a)) < 1e-8f) { na = v3_norm(v3_add(sg->n, pr->n)); sharedA = 1; }
        }
        if (s < w->nseg - 1) {
            const CueSeg *nx = &w->seg[s+1];
            if (v3_len2(v3_sub(sg->b, nx->a)) < 1e-8f) { nb = v3_norm(v3_add(sg->n, nx->n)); sharedB = 1; }
        }
        /* Pocket facing: extend the free-tip NOSE along its own (mitre/tangent)
         * direction — CONTINUING THE SAME ANGLE — to STOP exactly at the frame
         * line (the wood inner edge ±(hw|hl)+cw), NOT past it. Overshooting
         * tucked the cushion under the raised wood and z-fought on device. */
        if (sg->kind == 1 && (!sharedA || !sharedB)) {
            int afree = !sharedA;
            Vec3 kn = afree ? sg->b : sg->a;     /* shared knuckle (toward the rail) */
            Vec3 tp = afree ? sg->a : sg->b;     /* free tip (at the pocket mouth) */
            Vec3 M = v3_norm(v3_sub(tp, kn));    /* the facing's own direction */
            /* RUN IT OUT UNTIL ITS BACK MEETS THE WOOD, not its nose.
             *
             * The nose was being extended to the rail line and the back then
             * collapsed to zero depth, so a facing ended as a spike lying along
             * the rail with no thickness — and between that spike and the
             * timber sat a wedge of nothing. That is the misalignment at the
             * back of the cushion, on the curved and the mitred jaws alike:
             * the two ends of the same piece were being taken to two different
             * lines. The back is the one that has to land on the wood, because
             * the back is what the wood butts against. */
            /* NOSE AND BACK RUN OUT INDEPENDENTLY.
             *
             * A facing is angled, so its nose and its back do not reach the
             * timber at the same distance along it. Solving one extension for
             * both is what went wrong twice: take the nose to the wood and the
             * back collapses to a spike with a wedge of nothing behind it; take
             * the back to the wood and a steeply mitred facing is dragged back
             * off the pocket altogether. Extend each to the wood line on its
             * own and the piece ends flush, with a slanted end and full depth —
             * which is how a facing is actually cut. */
            const Vec3 nn = sg->n;
            /* WHICH LINE THE FACING RUNS OUT TO, taken from the rail it is
             * attached to rather than from the table's outside dimensions.
             *
             * This asked which of hl/hw the knuckle was nearest and then aimed
             * at that one — which is the same answer on a rectangle, where
             * every rail IS at ±hl or ±hw, and wrong on any rail that is not.
             * On an L the two rails around the notch sit at hl−notch_x and
             * hw−notch_z, so their facings were run out to the bounding rail
             * instead: a spike of cushion projecting into the empty air past
             * the end of the table, which is exactly how it drew.
             *
             * The knuckle sits ON the nose line, and the nose's inward normal
             * says which way is out of the table, so the wood inner edge is one
             * cushion-depth from the knuckle along −normal. No table dimension
             * involved, and identical to the old answer on every rectangle. */
            /* ...and the rail's normal is the FIRST STRAIGHT NOSE along the
             * chain from the shared end, not the immediate neighbour.
             *
             * On a mitred table the neighbour IS the nose and the two are the
             * same thing. On a rounded one the jaw is a run of bezier segments,
             * all of them facings, so the immediate neighbour is a curve step
             * whose normal is tilted a few degrees into the pocket — and a
             * target computed off that lands short of the timber, leaving a
             * sliver of daylight between the back of the cushion and the wood
             * at every rounded pocket. Which is exactly what it did. */
            Vec3 nose_n = sg->n;
            {   int step = afree ? +1 : -1;
                for (int k = s + step, hops = 0;
                     k >= 0 && k < w->nseg && hops < 32; k += step, hops++)
                    if (w->seg[k].kind == 0) { nose_n = w->seg[k].n; break; }
            }
            int zrail = fabsf(nose_n.z) > fabsf(nose_n.x);
            float target = zrail ? (kn.z - nose_n.z * cw)
                                 : (kn.x - nose_n.x * cw);
            float md = zrail ? M.z : M.x;
            if (fabsf(md) > 1e-4f) {
                float tn2 = (target - (zrail ? tp.z : tp.x)) / md;
                Vec3 bp = v3(tp.x - nn.x*cw, 0, tp.z - nn.z*cw);
                float tb = (target - (zrail ? bp.z : bp.x)) / md;
                if (tn2 > 0.0f) { Vec3 e = v3_add(tp, v3_scale(M, tn2));
                                  if (afree) pa = e; else pb = e; }
                if (tb > 0.0f) { Vec3 e = v3_add(bp, v3_scale(M, tb));
                                 if (afree) { fba = e; haveFba = 1; }
                                 else       { fbb = e; haveFbb = 1; } }
            }
        }
        float uba = sharedA ? ub : 0.0f, ubb = sharedB ? ub : 0.0f;
        /* Back-vertex depth. Shared ends reach the full depth cw; a FREE tip
         * collapses to 0 because the nose was already extended along its tangent
         * to the rail plane above — the facing continues at the same angle and
         * comes to a clean point there (US mitre and curved jaws alike). */
        /* FULL DEPTH AT BOTH ENDS. A free tip used to collapse to zero, which
         * is what made the facing a spike; it is a piece of cushion and it has
         * a back all the way along. */
        float cwa = cw, cwb = cw;
        Vec3 ba = v3(pa.x - na.x*uba, 0, pa.z - na.z*uba);
        Vec3 bb = v3(pb.x - nb.x*ubb, 0, pb.z - nb.z*ubb);
        Vec3 an = v3(pa.x, nose_h, pa.z), bn = v3(pb.x, nose_h, pb.z);
        Vec3 af = v3(pa.x, flat_h, pa.z), bf = v3(pb.x, flat_h, pb.z);
        /* straight rail nose (kind 0): clean perpendicular back at depth cw (a
         * straight edge at ±(hw|hl)+cw) so the wood inner edge can touch it
         * exactly. Facings keep the averaged normal for top continuity. */
        Vec3 bka = (sg->kind == 0) ? sg->n : na;
        Vec3 bkb = (sg->kind == 0) ? sg->n : nb;
        Vec3 ar = haveFba ? v3(fba.x, rail_h, fba.z)
                          : v3(pa.x - bka.x*cwa, rail_h, pa.z - bka.z*cwa);
        Vec3 br = haveFbb ? v3(fbb.x, rail_h, fbb.z)
                          : v3(pb.x - bkb.x*cwb, rail_h, pb.z - bkb.z*cwb);
        ribbon(ba, bb, bn, an, fdark);      /* undercut face (leans to nose) */
        quad(an, bn, bf, af, face);            /* small flat (planar) */
        ribbon(af, bf, br, ar, ctop);       /* cloth top → rail */
    }

    /* Wood rail frame: full rectangular ring (the pocket caps punch holes
     * through it, flush with the rail). */
    const float fw = rw + 0.055f;       /* wider wood frame to balance the deeper cushions */
    const float ox = hl + fw, oz = hw + fw;
    /* Wood inner edge WIDENED inward to meet the cushion back: the averaged
     * corner-normal pulls the cushion back to ~0.82·cw, so set the wood inset a
     * touch inside that (0.78·cw) — the wood reaches the cushion (no gap), the
     * cushion (drawn after, at rail_h) cleanly covers the tiny overlap. */
    const float ibx = hl + cw, ibz = hw + cw;   /* wood inner edge EXACTLY at the cushion back */
    const float nx = t->notch_x, nz = t->notch_z;   /* zero unless the bed is an L */
    /* Raise the wood top a hair above the flat cushion so the frame OVERLAPS and
     * hides the cushion back that now tucks under it (jaw_back runs the cushion
     * past the inner edge). A short inner riser closes the step down to rail_h. */
    const float frame_lift = 0.085f * t->R;
    const float plank_y = rail_h + frame_lift;
    float hx[CUE_MAX_POCKET], hz[CUE_MAX_POCKET], hr[CUE_MAX_POCKET];
    for (int p = 0; p < w->npocket; p++) {
        /* The TIMBER's bore: its own radius and its own setback along the
         * pocket's outward normal — see CueTable.bore_corner. Both default to
         * "the mouth, concentric", which is what this was before they were
         * fields. */
        /* pocket_mid, not `p < 4`: on an L the fifth pocket is still a corner,
         * and boring it as a middle cuts the wrong hole in the timber. */
        int is_mid = w->pocket_mid[p];
        float bs = is_mid ? t->bore_set_side : t->bore_set_corner;
        hx[p] = w->pocket[p].x + w->pmnorm[p].x * bs;
        hz[p] = w->pocket[p].z + w->pmnorm[p].z * bs;
        hr[p] = is_mid ? t->bore_side : t->bore_corner;
    }
    int nh = w->npocket;
    uint16_t wbore = shade565(woodt, 0.42f);   /* internal bore wall (in shadow) */
    const float bore_bot = -0.002f;            /* bore wall reaches the bed; throat continues below */
    /* Inner-edge risers (the short wood lip dropping from the raised plank top to
     * rail_h along the mouth edge) are drawn INSIDE wood_plank_bored per wood
     * column, so they skip the pocket mouths (no wood line across the side
     * pockets). rail_h is passed as the riser bottom. */
    uint16_t wlip = shade565(woodt, 0.80f);
    s_mat = CUE_MAT_WOOD;          /* everything from here down is timber */
    if (t->bed_shape == CUE_BED_L) {
        /* SIX PLANKS AND A SIX-SIDED SKIRT, because the woodwork is the shape
         * of the table and not the shape of its bounding box.
         *
         * With four planks the frame ran on past the notch — timber floating
         * out where there is no table — and there was none at all behind the
         * notch's own two rails, so those cushions backed onto nothing and the
         * missing corner read as a hole punched in a rectangular table rather
         * than as a table with a corner that is simply not there.
         *
         * The notch's rails face +x and +z, so their timber sits at LARGER x
         * and z than the cushion — inside the notch, between the cloth and the
         * empty air. */
        /* ...and it is the shape of THIS table, which may turn either way. Every
         * z below is multiplied by the hand, so the six planks and the six-sided
         * skirt mirror with the cloth they back onto. A plank whose two z bounds
         * come out the wrong way round is empty rather than inside out, so they
         * are ordered as well as mirrored. */
        const float h = cue_table_hand(t);
        #define ZH(v) ((v) * h)
        #define ZLO(a,b) (ZH(a) < ZH(b) ? ZH(a) : ZH(b))
        #define ZHI(a,b) (ZH(a) < ZH(b) ? ZH(b) : ZH(a))
        const float nxi = (hl - nx) + cw, nxo = (hl - nx) + fw;  /* notch's inner rail */
        const float nzi = (hw - nz) + cw, nzo = (hw - nz) + fw;  /* notch's underside */
        wood_plank_bored(-ox,  ox, ZLO(-oz,-ibz), ZHI(-oz,-ibz), plank_y, bore_bot, woodt, wbore, hx, hz, hr, nh, 0, 0, rail_h, wlip); /* bottom */
        wood_plank_bored( ibx, ox, ZLO(-oz, nzo), ZHI(-oz, nzo), plank_y, bore_bot, woodt, wbore, hx, hz, hr, nh, 1, 1, rail_h, wlip); /* right, up to the notch */
        wood_plank_bored( nxi, ox, ZLO(nzi, nzo), ZHI(nzi, nzo), plank_y, bore_bot, woodt, wbore, hx, hz, hr, nh, 0, 1, rail_h, wlip); /* under the notch */
        wood_plank_bored( nxi, nxo, ZLO(nzi, oz), ZHI(nzi, oz),  plank_y, bore_bot, woodt, wbore, hx, hz, hr, nh, 1, 1, rail_h, wlip); /* beside the notch */
        wood_plank_bored(-ox,  nxo, ZLO(ibz, oz), ZHI(ibz, oz),  plank_y, bore_bot, woodt, wbore, hx, hz, hr, nh, 0, 1, rail_h, wlip); /* top, short leg */
        wood_plank_bored(-ox, -ibx, ZLO(-oz, oz), ZHI(-oz, oz),  plank_y, bore_bot, woodt, wbore, hx, hz, hr, nh, 1, 0, rail_h, wlip); /* left */
        /* the skirt, round the same six sides */
        quad(v3( ox,plank_y,ZH(-oz)), v3(-ox,plank_y,ZH(-oz)), v3(-ox,0,ZH(-oz)), v3( ox,0,ZH(-oz)), wood);
        quad(v3( ox,plank_y,ZH(nzo)), v3( ox,plank_y,ZH(-oz)), v3( ox,0,ZH(-oz)), v3( ox,0,ZH(nzo)), wood);
        quad(v3(nxo,plank_y,ZH(nzo)), v3( ox,plank_y,ZH(nzo)), v3( ox,0,ZH(nzo)), v3(nxo,0,ZH(nzo)), wood);
        quad(v3(nxo,plank_y,ZH( oz)), v3(nxo,plank_y,ZH(nzo)), v3(nxo,0,ZH(nzo)), v3(nxo,0,ZH( oz)), wood);
        quad(v3(-ox,plank_y,ZH( oz)), v3(nxo,plank_y,ZH( oz)), v3(nxo,0,ZH( oz)), v3(-ox,0,ZH( oz)), wood);
        quad(v3(-ox,plank_y,ZH(-oz)), v3(-ox,plank_y,ZH( oz)), v3(-ox,0,ZH( oz)), v3(-ox,0,ZH(-oz)), wood);
        #undef ZH
        #undef ZLO
        #undef ZHI
    } else {
    wood_plank_bored(-ox, ox,  ibz,  oz,  plank_y, bore_bot, woodt, wbore, hx, hz, hr, nh, 0, 1, rail_h, wlip); /* +z */
    wood_plank_bored(-ox, ox, -oz, -ibz,  plank_y, bore_bot, woodt, wbore, hx, hz, hr, nh, 0, 0, rail_h, wlip); /* -z */
    wood_plank_bored(ibx, ox, -ibz, ibz,  plank_y, bore_bot, woodt, wbore, hx, hz, hr, nh, 1, 1, rail_h, wlip); /* +x */
    wood_plank_bored(-ox,-ibx,-ibz, ibz,  plank_y, bore_bot, woodt, wbore, hx, hz, hr, nh, 1, 0, rail_h, wlip); /* -x */
    quad(v3(-ox,plank_y,oz), v3(ox,plank_y,oz), v3(ox,0,oz), v3(-ox,0,oz), wood);
    quad(v3(ox,plank_y,-oz), v3(-ox,plank_y,-oz), v3(-ox,0,-oz), v3(ox,0,-oz), wood);
    quad(v3(ox,plank_y,oz), v3(ox,plank_y,-oz), v3(ox,0,-oz), v3(ox,0,oz), wood);
    quad(v3(-ox,plank_y,-oz), v3(-ox,plank_y,oz), v3(-ox,0,oz), v3(-ox,0,-oz), wood);
    }

    /* Pockets = circular VOIDS you look down into. The bed is already cut at
     * the mouth, so a downward cone gives the recess. The OUTWARD half of each
     * pocket (the half sitting over the wood frame) gets a flush rail-level cap
     * + a frame-thickness wall to punch the hole through the wood; the inward
     * (mouth) half is left open so nothing floats above the playing surface. */
    /* Snooker: a deeper, dark-olive "net bag" pouch the potted ball drops into.
     * Pool: a shallow near-black void. */
  /* NO SEPARATE VOID CIRCLE. The pocket opening is the scallop, and this used
   * to punch a second cone on the physics drop centre — a different centre and
   * a different radius — so every pocket rendered as two overlapping circles.
   * The throat under the lip (emit_pocket_lips) is built from the scallop's own
   * boundary, so there is one opening and one edge. */
  if (0) {
    uint16_t pk_floor = s_is_snooker ? RGB565C(34, 30, 20) : RGB565C(3, 4, 4);
    uint16_t pk_net   = s_is_snooker ? RGB565C(22, 20, 13) : RGB565C(6, 7, 7);
    const float floor_y = s_is_snooker ? -0.105f : -0.055f;
    for (int p = 0; p < w->npocket; p++) {
        float cx = w->pocket[p].x, cz = w->pocket[p].z;
        float r = w->pocket_r[p];     /* void = the functional drop (matches the red line) */
        Vec3 floor_c = v3(cx, floor_y, cz);
        const int N = CUE_ARC_SEGS * 4;
        float base = atan2f(cz, cx);
        for (int k = 0; k < N; k++) {
            float a0 = base + k * (6.2831853f / N), a1 = base + (k + 1) * (6.2831853f / N);
            float c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a1), s1 = sinf(a1);
            Vec3 bed0 = v3(cx + r*c0, -0.002f, cz + r*s0);
            Vec3 bed1 = v3(cx + r*c1, -0.002f, cz + r*s1);
            if (s_is_snooker) {                              /* two-tone net pouch */
                float midy = -0.05f, midr = r * 0.62f;
                Vec3 m0 = v3(cx+midr*c0, midy, cz+midr*s0);
                Vec3 m1 = v3(cx+midr*c1, midy, cz+midr*s1);
                quad(bed0, bed1, m1, m0, pk_floor);
                tri(floor_c, m0, m1, pk_net);
            } else {
                tri(floor_c, bed0, bed1, pk_floor);          /* shallow dark void */
            }
        }
    }

  }
    s_lip_ntab = s_ntab;      /* lips drawn last + depth-write OFF so balls cover them */
    emit_pocket_lips(t, w);   /* drop lip last → layers over the voids cleanly */
    /* CUE_SHOWDROP: a bright ring exactly on the functional pocket — where the
     * ball is actually captured. Purely a measuring aid: the cut is being lined
     * up against it by eye and a number nobody can see cannot be lined up. */
    if (getenv("CUE_SHOWDROP")) {
        const uint16_t mg = RGB565C(255, 0, 255);
        for (int p = 0; p < w->npocket; p++) {
            Vec3 c = w->pocket[p];
            float r = w->pocket_r[p], t2 = r * 0.035f, y = 0.0016f;
            const int N = 96;
            for (int k = 0; k < N; k++) {
                float a0 = 6.2831853f*k/N, a1 = 6.2831853f*(k+1)/N;
                quad(v3(c.x+(r-t2)*cosf(a0), y, c.z+(r-t2)*sinf(a0)),
                     v3(c.x+(r-t2)*cosf(a1), y, c.z+(r-t2)*sinf(a1)),
                     v3(c.x+(r+t2)*cosf(a1), y, c.z+(r+t2)*sinf(a1)),
                     v3(c.x+(r+t2)*cosf(a0), y, c.z+(r+t2)*sinf(a0)), mg);
            }
        }
    }
}

void cue_render_set_markings(int on) { s_markings = on ? 1 : 0; }

int cue_render_table_tris(const CueTri **out, int *bed, int *lip) {
    if (out) *out = s_tab;
    if (bed) *bed = s_bed_ntab;
    if (lip) *lip = s_lip_ntab;
    return s_ntab;
}

/* ---- per-frame build --------------------------------------------------- */
static void project_view(Vec3 v, float *sx, float *sy, uint16_t *d) {
    if (v.z < CUE_NEAR) v.z = CUE_NEAR;
    float inv = 1.0f / v.z;
    *sx = 64.0f + s_focal * v.x * inv;
    *sy = 64.0f - s_focal * v.y * inv;
    float dd = CUE_DEPTH_K * inv;
    *d = (dd >= 65535.0f) ? 65535u : (dd < 1.0f ? 1u : (uint16_t)dd);
}
/* Push one screen triangle, ordering verts for positive area (double-sided
 * so a table face is never culled by winding). */
static void push_stri(float ax, float ay, uint16_t da, float bx, float by,
                      uint16_t db, float cx, float cy, uint16_t dc, uint16_t col) {
    if (s_nstri >= MAX_STRI) return;
    float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    STri *st = &s_stri[s_nstri++];
    st->color = col;
    if (area >= 0) {
        st->x0 = ax; st->y0 = ay; st->d0 = da;
        st->x1 = bx; st->y1 = by; st->d1 = db;
        st->x2 = cx; st->y2 = cy; st->d2 = dc;
    } else {
        st->x0 = ax; st->y0 = ay; st->d0 = da;
        st->x1 = cx; st->y1 = cy; st->d1 = dc;
        st->x2 = bx; st->y2 = by; st->d2 = db;
    }
}
/* Transform to view space, clip against the near plane (Sutherland-Hodgman,
 * one plane → 1 or 2 tris), shade and emit. This is what keeps the bed/rails
 * visible when the camera sits on the table (near corners behind it). */
static void add_stri(Vec3 a, Vec3 b, Vec3 c, Vec3 nrm, uint16_t base) {
    Vec3 va = m3_mul_v3_t(&s_view.basis, v3_sub(a, s_view.pos));
    Vec3 vb = m3_mul_v3_t(&s_view.basis, v3_sub(b, s_view.pos));
    Vec3 vc = m3_mul_v3_t(&s_view.basis, v3_sub(c, s_view.pos));
    float ndl = v3_dot(nrm, s_light); if (ndl < 0) ndl = -ndl;
    uint16_t col = shade565(base, 0.32f + 0.68f * ndl);

    Vec3 poly[3] = { va, vb, vc };
    Vec3 out[4]; int no = 0;
    for (int i = 0; i < 3; i++) {
        Vec3 p = poly[i], q = poly[(i + 1) % 3];
        int pin = p.z > CUE_NEAR, qin = q.z > CUE_NEAR;
        if (pin) out[no++] = p;
        if (pin != qin) {
            float t = (CUE_NEAR - p.z) / (q.z - p.z);
            out[no++] = v3_lerp(p, q, t);
        }
    }
    if (no < 3) return;
    float ox[4], oy[4]; uint16_t od[4];
    for (int i = 0; i < no; i++) project_view(out[i], &ox[i], &oy[i], &od[i]);
    push_stri(ox[0], oy[0], od[0], ox[1], oy[1], od[1], ox[2], oy[2], od[2], col);
    if (no == 4)
        push_stri(ox[0], oy[0], od[0], ox[2], oy[2], od[2], ox[3], oy[3], od[3], col);
}

/* ===== Mote engine port: emit the scene through the engine ABI instead of
 * the in-game r3d rasteriser. cue_render_build now feeds scene_add_tri (table),
 * scene_add_sphere_tex (balls), scene_add_point/line (aim/cue) and a background
 * callback; the engine owns projection, depth, dual-core raster and present. */
static const MoteApi *s_api;
void cue_render_set_api(const MoteApi *api) { s_api = api; }

static uint16_t ball_base(uint8_t id);                       /* defined below */
static uint16_t ball_sample(uint8_t id, Vec3 nb, uint16_t base);

/* per-frame ball specular half-vectors (overhead 4-lamp cluster + single H) */
static Vec3 s_ballH, s_ballHl[4];
/* one MoteSphereTex per ball this frame (ud carries the ball id). */
static MoteSphereTex s_balltex[CUE_MAX_BALLS];

/* Per-pixel ball shading — ported verbatim from the old draw_ball inner loop.
 * The engine reconstructs the normal/depth and rotates it into ball space; we
 * return the final colour from the ball-local (nb) + world (nw) normals. */
static uint16_t cue_ball_shade(Vec3 nb, Vec3 nw, float de, float se, float nz, void *ud) {
    (void)de; (void)se;
    uint8_t id = (uint8_t)(uintptr_t)ud;
    uint16_t base = ball_base(id);
    float diff = v3_dot(nw, s_light); if (diff < 0) diff = 0;
    float s = v3_dot(nw, s_ballH);    if (s < 0) s = 0;
    float down = -nw.y;               if (down < 0) down = 0;
    uint16_t bc = ball_sample(id, nb, base);
    uint16_t col;
    switch (s_light_mode) {
    case 0:
        col = shade565(bc, (0.30f + 0.70f*diff) * (0.78f + 0.22f*nz));
        { float ss = s; ss*=ss; ss*=ss; ss*=ss; int hi=(int)(ss*26.0f);
          if (hi>0) col = add565(col, hi, hi*2, hi); }
        break;
    case 2:
        col = shade565(bc, diff>0.62f?1.0f : diff>0.30f?0.74f : 0.52f);
        col = mix565(col, s_cloth_shadow, (1.0f-diff)*0.40f + down*0.22f);
        if (s > 0.82f) col = RGB565C(250,250,250);
        break;
    case 3:
        col = shade565(bc, 0.30f + 0.70f*diff);
        col = mix565(col, s_cloth_shadow, (1.0f-diff)*0.50f + down*0.40f);
        if (s > 0.60f) { float h=(s-0.60f)*2.5f; h*=h*h; int hi=(int)(h*30.0f);
          if (hi>0) col = add565(col, hi, hi, hi); }
        break;
    case 4: case 5: default: {
        float thr = (s_light_mode==5) ? 0.93f : (s_light_mode==4) ? 0.955f : 0.975f;
        float gain = (s_light_mode==5) ? 0.85f : 1.0f;
        col = shade565(bc, 0.46f + 0.54f*diff);
        col = mix565(col, s_cloth_shadow, (1.0f-diff)*0.40f + down*0.42f);
        float refl = 0.0f;
        for (int li = 0; li < 4; li++) {
            float si = v3_dot(nw, s_ballHl[li]);
            if (si > thr) { float h = (si - thr) / (1.0f - thr); refl += h*h; }
        }
        if (refl > 1.0f) refl = 1.0f;
        if (refl > 0.0f) col = mix565(col, RGB565C(255,255,255), refl * gain);
        break;
    }
    }
    return col;
}

/* Background vertical gradient — registered with the engine; runs per band. */
void cue_render_bg(uint16_t *fb, int y0, int y1) {
    for (int y = y0; y < y1; y++) {
        float t = (float)y / 128.0f;
        int r = (int)(((s_bg_top >> 11) & 31) * (1 - t) + ((s_bg_bot >> 11) & 31) * t);
        int g = (int)(((s_bg_top >> 5) & 63) * (1 - t) + ((s_bg_bot >> 5) & 63) * t);
        int b = (int)(((s_bg_top) & 31) * (1 - t) + ((s_bg_bot) & 31) * t);
        uint16_t c = (uint16_t)((r << 11) | (g << 5) | b);
        uint16_t *row = fb + y * 128;
        for (int x = 0; x < 128; x++) row[x] = c;
    }
}

void cue_render_build(const CueView *v, const CueBall *balls, int n,
                      int aim_active, int aim_ball, Vec3 aim_dir,
                      float power, int aim_level) {
    s_view = *v;
    s_focal = 64.0f / tanf(v->fov_deg * (3.14159265f / 180.0f) * 0.5f);
    if (!s_api) return;
    s_api->scene_camera(&v->basis, v->pos, v->fov_deg);   /* resets the draw list */

    /* ball specular half-vectors for cue_ball_shade (4 overhead lamps + one H) */
    Vec3 vcam = v3_scale(v->basis.r[2], -1.0f);
    s_ballH = v3_norm(v3_add(s_light, vcam));
    const float lx = 0.42f, lz = 0.28f;
    s_ballHl[0] = v3_norm(v3_add(v3_norm(v3(s_light.x+lx, s_light.y, s_light.z+lz)), vcam));
    s_ballHl[1] = v3_norm(v3_add(v3_norm(v3(s_light.x-lx, s_light.y, s_light.z+lz)), vcam));
    s_ballHl[2] = v3_norm(v3_add(v3_norm(v3(s_light.x+lx, s_light.y, s_light.z-lz)), vcam));
    s_ballHl[3] = v3_norm(v3_add(v3_norm(v3(s_light.x-lx, s_light.y, s_light.z-lz)), vcam));

    /* Table: shade per tri (light is static), emit as world tris. Pocket-lip
     * tris (>= s_lip_ntab) are depth-tested but not depth-writing so the balls
     * paint over them. scene_add_tri is double-sided, matching the old build. */
    for (int i = 0; i < s_ntab; i++) {
        const CueTri *t = &s_tab[i];
        float ndl = v3_dot(t->nrm, s_light); if (ndl < 0) ndl = -ndl;
        uint16_t col = shade565(t->color, 0.32f + 0.68f * ndl);
        uint32_t fl = (i >= s_lip_ntab) ? MOTE_DRAW_NO_DEPTH_WRITE : 0;
        s_api->scene_add_tri(t->v[0], t->v[1], t->v[2], col, fl);
    }

    /* Ball shadows: soft ground-shadow decals on the cloth under each ball
     * (the engine darkens the felt with a radial falloff, like the original). */
    for (int i = 0; i < n; i++) {
        const CueBall *b = &balls[i];
        if (!b->on || b->drop > 0.0f) continue;
        s_api->scene_add_shadow(v3(b->pos.x, 0.0f, b->pos.z), s_ballR * 1.55f, 0.5f);
    }

    /* Balls → textured/oriented sphere impostors (the engine shades per pixel
     * via cue_ball_shade, rotated by the ball's spin orientation). */
    for (int i = 0; i < n && i < CUE_MAX_BALLS; i++) {
        const CueBall *b = &balls[i];
        if (!b->on) continue;
        s_balltex[i] = (MoteSphereTex){ .shade_mode = MOTE_SHADE_CUSTOM,
                                        .shade = cue_ball_shade,
                                        .ud = (void *)(uintptr_t)b->id };
        s_api->scene_add_sphere_tex(b->pos, s_ballR, &b->orient, &s_balltex[i]);
    }

    /* Aim line, ghost ball, object-ball line, cue stick. */
    if (aim_active && aim_ball >= 0 && aim_ball < n && balls[aim_ball].on) {
        Vec3 cuepos = balls[aim_ball].pos;
        Vec3 dir = v3_norm(v3(aim_dir.x, 0, aim_dir.z));
        const float twoR = 2.0f * s_ballR;
        const float step = s_ballR * 2.2f;
        int hit = -1;
        if (aim_level >= 1) {
            float bests = 1e9f;
            for (int i = 0; i < n; i++) {
                if (i == aim_ball || !balls[i].on) continue;
                Vec3 d = v3_sub(balls[i].pos, cuepos); d.y = 0;
                float along = v3_dot(d, dir);
                if (along <= 0) continue;
                float perp2 = (d.x * d.x + d.z * d.z) - along * along;
                if (perp2 < twoR * twoR) {
                    float s = along - sqrtf(twoR * twoR - perp2);
                    if (s > 0 && s < bests) { bests = s; hit = i; }
                }
            }
            float linelen = (hit >= 0) ? bests : 1.4f;
            int ndots = (int)(linelen / step);
            if (ndots > MAX_DOTS) ndots = MAX_DOTS;
            for (int k = 1; k <= ndots; k++)
                s_api->scene_add_point(v3(cuepos.x + dir.x*(k*step), s_ballR,
                                          cuepos.z + dir.z*(k*step)),
                                       RGB565C(240,240,160), 1);
            if (hit >= 0) {
                Vec3 ghost = v3(cuepos.x + dir.x*bests, s_ballR, cuepos.z + dir.z*bests);
                if (aim_level >= 3) {
                    Vec3 odir = v3_norm(v3(balls[hit].pos.x - ghost.x, 0,
                                           balls[hit].pos.z - ghost.z));
                    for (int k = 1; k <= 10; k++)
                        s_api->scene_add_point(v3(balls[hit].pos.x + odir.x*(k*step), s_ballR,
                                                  balls[hit].pos.z + odir.z*(k*step)),
                                               RGB565C(120,230,235), 1);
                }
                if (aim_level >= 2)   /* ghost ball: camera-facing ring at the contact point */
                    s_api->scene_add_ring(ghost, s_ballR, RGB565C(230, 230, 230));
            }
        }
        /* Cue stick: a tapered quad resting at the english-shifted contact point,
         * running back along the elevated cue axis (two world tris, double-sided). */
        Vec3 up = v3(0,1,0);
        Vec3 rightv = v3_norm(v3_cross(up, dir));
        float ce = cosf(s_cue_elev), se = sinf(s_cue_elev);
        Vec3 cdir = v3(dir.x*ce, -se, dir.z*ce);
        Vec3 contact = v3(cuepos.x + rightv.x*s_cue_side*s_ballR,
                          s_ballR     + s_cue_vert*s_ballR,
                          cuepos.z + rightv.z*s_cue_side*s_ballR);
        float gap = 0.015f + power * 0.18f;
        Vec3 tip  = v3(contact.x - cdir.x*gap, contact.y - cdir.y*gap, contact.z - cdir.z*gap);
        Vec3 butt = v3(tip.x - cdir.x*0.55f, tip.y - cdir.y*0.55f, tip.z - cdir.z*0.55f);
        const float wt = 0.004f, wb = 0.013f;   /* world half-widths: tip, butt */
        Vec3 t0 = v3(tip.x + rightv.x*wt,  tip.y,  tip.z + rightv.z*wt);
        Vec3 t1 = v3(tip.x - rightv.x*wt,  tip.y,  tip.z - rightv.z*wt);
        Vec3 b0 = v3(butt.x + rightv.x*wb, butt.y, butt.z + rightv.z*wb);
        Vec3 b1 = v3(butt.x - rightv.x*wb, butt.y, butt.z - rightv.z*wb);
        uint16_t cue_col = RGB565C(214, 176, 104);
        s_api->scene_add_tri(t0, t1, b1, cue_col, 0);
        s_api->scene_add_tri(t0, b1, b0, cue_col, 0);
    }
}

/* ---- ball sets --------------------------------------------------------- */
/* 0 = PRO (per-number coloured solids/stripes), 1 = UK yellow/blue solids,
 * 2 = UK yellow/red solids, 3 = US "dyna" (yellow solids / maroon stripes). */
void cue_render_set_ball_set(int s) {
    /* Against the COUNT, not a literal. This was `> 7` and stayed `> 7` when a
     * ninth set was added, so selecting it silently gave you the first one —
     * the menu said PYRAMID and the balls stayed numbered pool balls. */
    s_ball_set = (s < 0 || s >= cue_render_ballset_count()) ? 0 : s;
    /* Picking an authored set puts a built one away. Without this the picker
     * would appear to do nothing at all once a custom set was installed — the
     * name in the menu would change and every ball on the table would not. */
    cue_render_set_ballset_custom(NULL);
}

/* the standard pro per-number hues for ids 1..7 (9..15 reuse 1..7's hue) */
static const uint16_t k_prohue[8] = {
    0, RGB565C(235,200,40), RGB565C(30,80,200), RGB565C(200,40,40),
    RGB565C(120,40,160), RGB565C(230,120,30), RGB565C(20,130,50),
    RGB565C(120,30,40) };
#define BALL_YELLOW RGB565C(235,200,40)
#define BALL_GOLD   RGB565C(228,165,20)
#define BALL_BLUE   RGB565C(30,80,200)
#define BALL_RED    RGB565C(200,40,40)
#define BALL_MAROON RGB565C(120,22,42)
#define BALL_BLACK  RGB565C(20,20,22)
#define BALL_WHITE  RGB565C(235,235,225)
/* Pro Tournament per-number palette (from 2dpool "Pro Tournament" ballColors);
 * striped balls use the same hue as their +8 solid, on BLACK poles. */
static const uint16_t k_ptourhue[8] = {
    0, RGB565C(245,180,0),  RGB565C(0,55,237),  RGB565C(255,30,0),
    RGB565C(255,71,123),    RGB565C(154,46,255), RGB565C(0,227,155),
    RGB565C(128,50,11) };
/* SPACE per-number palette (2dpool "Space" ballColors). */
static const uint16_t k_spacehue[8] = {
    0, RGB565C(255,215,0),  RGB565C(0,0,205),   RGB565C(255,0,0),
    RGB565C(75,0,130),      RGB565C(255,140,0), RGB565C(0,100,0),
    RGB565C(128,0,0) };
/* VINTAGE per-number palette (2dpool "Vintage" ballColors — muted gold/orange). */
static const uint16_t k_vintagehue[8] = {
    0, RGB565C(184,135,0),  RGB565C(0,0,205),   RGB565C(255,0,0),
    RGB565C(75,0,130),      RGB565C(255,115,0), RGB565C(0,100,0),
    RGB565C(128,0,0) };
#define BALL_GREY  RGB565C(148,143,143)   /* SPACE poles */
#define BALL_CREAM RGB565C(255,233,153)   /* VINTAGE poles */
#define BALL_PINK  RGB565C(255,0,221)     /* HOT PINK group2 */
#define BALL_INK   RGB565C(19,16,16)      /* HOT PINK group1 */

/* ---- A BALL SET, AS DATA -------------------------------------------------
 *
 * These eight were seven separate switch statements on s_ball_set, in five
 * different functions: the pole colour, whether the set is striped, whether it
 * is numbered, the per-number hue, the body colour, the spoke count and the
 * band width. Adding a ninth set meant finding all seven, and a set that nobody
 * happened to look at could quietly disagree with itself -- carry a stripe and
 * no number, or a number circle with the wrong spokes.
 *
 * One row per set instead. The colour fields use ZERO to mean "take it from the
 * palette", which is safe because no authored ball colour is pure black: the
 * blackest thing here is BALL_BLACK at (20,20,22).
 *
 *   hue    per-number palette for ids 1..7; stripes reuse their +8 solid's hue
 *   lo/hi  a flat body colour for low/high balls, where the set has one
 *          instead of a per-number palette (the UK and hot-pink sets)
 *   pole   what a striped ball's body is, behind the band
 *   eight  the black -- its own field because one set makes it grey
 *   band   a flat stripe colour, where the stripe is not the ball's own hue
 *   half   the band's half-width as a fraction of the ball */
/* CueBallSet itself now lives in cue_render.h — a designer has to be able to
 * hold one, and it cannot do that if the type is private to this file. */

/* Russian pyramid's set: fifteen PLAIN IVORY balls and a coloured cue ball. No
 * stripes and no black — the balls are interchangeable in play, which is why
 * the rack lays them out in id order and the base rules never read an id. They
 * do carry a small black numeral, because the variants that are not the base
 * rules have to be able to name one. It is here as
 * an authored set rather than a special case in the shader because that is what
 * a ball set IS since F3, and because a player who wants to break UK 8-ball off
 * with ivories should be able to. */
static const uint16_t k_ivoryhue[8] = {
    0, RGB565C(238,232,214), RGB565C(238,232,214), RGB565C(238,232,214),
       RGB565C(238,232,214), RGB565C(238,232,214), RGB565C(238,232,214),
       RGB565C(238,232,214),
};

static const CueBallSet k_ballsets[9] = {
  /* 0 */ { "PRO",        k_prohue,     0, 0, BALL_WHITE, BALL_BLACK, 0, 0,
            1, 1, 0, 0.42f },
  /* 1 */ { "UK YELLOW/BLUE", k_prohue, BALL_YELLOW, BALL_BLUE, 0, BALL_BLACK, 0, 0,
            0, 0, 0, 0.42f },
  /* 2 */ { "UK YELLOW/RED",  k_prohue, BALL_YELLOW, BALL_RED,  0, BALL_BLACK, 0, 0,
            0, 0, 0, 0.42f },
  /* 3 */ { "PRO LEAGUE", k_prohue,     BALL_GOLD, 0, BALL_WHITE, BALL_BLACK,
            BALL_MAROON, 0, 1, 1, 3, 0.42f },
  /* 4 */ { "PRO TOUR",   k_ptourhue,   0, 0, BALL_BLACK, BALL_BLACK, 0, 0,
            1, 1, 2, 0.55f },
  /* 5 */ { "HOT PINK",   k_prohue,     BALL_INK, BALL_PINK, 0,
            RGB565C(158,158,158), 0, 0, 0, 0, 0, 0.42f },
  /* 6 */ { "SPACE",      k_spacehue,   0, 0, BALL_GREY,  BALL_BLACK, 0, 0,
            1, 1, 0, 0.42f },
  /* 7 */ { "VINTAGE",    k_vintagehue, 0, 0, BALL_CREAM, BALL_BLACK, 0, 0,
            1, 1, 0, 0.42f },
  /* 8 */ { "PYRAMID",    k_ivoryhue,   RGB565C(238,232,214), RGB565C(238,232,214),
            0, RGB565C(238,232,214), 0,
            /* ...and a COLOURED cue ball, because the other fifteen are
             * identical and its colour is the only thing that says which one
             * you are striking. */
            RGB565C(190, 46, 40),
            /* Not striped; numbered in the BARE style (2) — see number_patch. */
            0, 2, 0, 0.42f },
};

/* THE SET THE PLAYER BUILT, if there is one. Held by value, and the palette
 * and the name held by value beside it: the caller's CueBallSet is a stack
 * struct pointing at the caller's own array, and both are gone the moment the
 * designer screen returns. Copying is the whole of what makes this safe. */
static CueBallSet s_cust;
static uint16_t   s_cust_hue[8];
static char       s_cust_name[24];
static int        s_cust_on;

static const CueBallSet *bset(void) {
    if (s_cust_on) return &s_cust;
    int i = (s_ball_set < 0 || s_ball_set > 8) ? 0 : s_ball_set;
    return &k_ballsets[i];
}
int cue_render_ballset_count(void);   /* below; used by the setter's clamp */

/* WHICH SET IS SELECTED, for a host that CACHES the ball surface. CueVR bakes
 * ball_sample() into an atlas once per table, so it has to know when the answer
 * has changed under it — and "the app told mote, so the app knows" is not true
 * when the app is also told by a menu, a preference load and a game kind. */
int cue_render_ball_set(void) { return s_cust_on ? -1 : s_ball_set; }

int         cue_render_ballset_count(void) { return 9; }
const char *cue_render_ballset_name(int i) {
    return (i >= 0 && i < 9) ? k_ballsets[i].name : "";
}

int cue_render_ballset_get(int i, CueBallSet *out) {
    if (!out || i < 0 || i >= 9) return 0;
    *out = k_ballsets[i];
    return 1;
}

void cue_render_set_ballset_custom(const CueBallSet *bs) {
    if (!bs) { s_cust_on = 0; return; }
    s_cust = *bs;
    if (bs->hue) for (int k = 0; k < 8; k++) s_cust_hue[k] = bs->hue[k];
    else         for (int k = 0; k < 8; k++) s_cust_hue[k] = k_prohue[k];
    s_cust.hue = s_cust_hue;
    if (bs->name) { size_t n = 0; while (n + 1 < sizeof s_cust_name && bs->name[n]) { s_cust_name[n] = bs->name[n]; n++; } s_cust_name[n] = 0; }
    else s_cust_name[0] = 0;
    s_cust.name = s_cust_name;
    s_cust_on = 1;
}

int cue_render_ballset_is_custom(void) { return s_cust_on; }

/* ---- ball texture ------------------------------------------------------ */
static const CueBallSet *bset(void);

static uint16_t ball_base(uint8_t id) {
    switch (id) {
        case CUE_ID_CUE: {
            /* THE SET'S OWN CUE BALL, where it has one. Zero means the near-
             * white every game but pyramid is played with. */
            uint16_t c = bset()->cue;
            return c ? c : RGB565C(245, 245, 235);
        }
        case CUE_ID_YELLOW: return RGB565C(235, 200, 40);
        case CUE_ID_GREEN:  return RGB565C(20, 130, 50);
        case CUE_ID_BROWN:  return RGB565C(120, 70, 35);
        case CUE_ID_BLUE:   return RGB565C(30, 80, 200);
        case CUE_ID_PINK:   return RGB565C(235, 120, 150);
        case CUE_ID_BLACK:  return RGB565C(20, 20, 22);
    }
    if (s_is_snooker) return RGB565C(190, 30, 30);          /* reds 1..15 */
    const CueBallSet *bs = bset();
    if (id == 8) return bs->eight;
    if (id <= 7) return bs->lo ? bs->lo : bs->hue[id];      /* flat lows, or per-number */
    return bs->hi ? bs->hi : bs->pole;                      /* flat highs, or the band's ground */
}
/* 3x5 digit glyphs, packed top row first, 3 bits/row (MSB = left column). */
static const uint16_t k_digit3x5[10] = {
    0x7B6F, /* 0: 111 101 101 101 111 */
    0x2C97, /* 1: 010 110 010 010 111 */
    0x73E7, /* 2: 111 001 111 100 111 */
    0x73CF, /* 3: 111 001 111 001 111 */
    0x5BC9, /* 4: 101 101 111 001 001 */
    0x79CF, /* 5: 111 100 111 001 111 */
    0x79EF, /* 6: 111 100 111 101 111 */
    0x7292, /* 7: 111 001 010 010 010 */
    0x7BEF, /* 8: 111 101 111 101 111 */
    0x7BCF, /* 9: 111 101 111 001 111 */
};

/* Render the white number circle (and, for the dyna set, the dynasphere black
 * ring + three spoke radii) onto the +x pole cap. `us` selects numbered sets. */
/* numbered == 2: A BARE NUMBER, no disc under it.
 *
 * A Russian ball is a plain ivory with a small black numeral printed straight
 * onto it — there is no white patch, because the ball is already white, and the
 * numeral is a good deal smaller than a pool ball's because it is a label
 * rather than the ball's identity. The fifteen play interchangeably; the number
 * is only there so a variant that has to name a ball can. Printed on BOTH poles
 * the way a real one is, which also means one is nearly always facing you. */
static uint16_t number_patch(uint8_t id, Vec3 nb, uint16_t base, int us) {
    const int bare = (us == 2);
    float ax = bare ? fabsf(nb.x) : nb.x;
    if (!us || ax <= 0.90f) return base;
    /* Map the pole cap to a unit disc (py,pz); edge of the patch -> r2 ~ 1.
     * The bare number sits in a tighter cap, which is what makes it small. */
    float scale = bare ? 3.70f : 2.30f;
    float py = nb.y * scale, pz = nb.z * scale;
    /* On the far pole the cap is seen from behind, so the glyph would read
     * mirrored. Flip it back. */
    if (bare && nb.x < 0.0f) pz = -pz;
    float r2 = py * py + pz * pz;
    if (r2 > 1.0f) return base;
    const uint16_t WHT = bare ? base : RGB565C(245, 245, 245);
    const uint16_t INK = RGB565C(15, 15, 18);
    /* dynasphere-style number circle: black ring + N spoke radii.
     * set 3 (pro league) = 3 spokes; set 4 (pro tournament) = 2 spokes. */
    int nspoke = bset()->spokes;
    if (nspoke) {
        if (r2 > 0.78f) return INK;        /* outer black ring */
        if (r2 > 0.30f) {                  /* spoke radii, evenly spaced */
            static const float dk[3][2] = {
                {1.0f, 0.0f}, {-0.5f, 0.86603f}, {-0.5f, -0.86603f} };
            /* 2-spoke set uses a vertical pair; 3-spoke uses the tripod above */
            static const float dk2[2][2] = { {0.0f, 1.0f}, {0.0f, -1.0f} };
            const float (*sp)[2] = (nspoke == 2) ? dk2 : dk;
            for (int k = 0; k < nspoke; k++) {
                float dot = py * sp[k][0] + pz * sp[k][1];
                if (dot <= 0.0f) continue;
                float cr = py * sp[k][1] - pz * sp[k][0];
                if (cr * cr < 0.018f * r2) return INK;
            }
        }
    }
    /* Digit(s): 1 cell for 1-9, two side-by-side cells for 10-15. */
    int two = id >= 10;
    float uw = two ? 0.78f : 0.40f;         /* half-width of the glyph area */
    float gx = (pz + uw) / (2.0f * uw) * (two ? 7.0f : 3.0f);
    float gy = (0.62f - py) / 1.24f * 5.0f;
    int col = (int)gx, row = (int)gy;
    if (gx < 0.0f || gy < 0.0f || row > 4) return WHT;
    int d, dc;
    if (!two) { d = id % 10; dc = col; if (col > 2) return WHT; }
    else if (col < 3) { d = id / 10; dc = col; }
    else if (col < 4) return WHT;           /* gap column */
    else { d = id % 10; dc = col - 4; if (dc > 2) return WHT; }
    int rowbits = (k_digit3x5[d] >> ((4 - row) * 3)) & 7;
    return ((rowbits >> (2 - dc)) & 1) ? INK : WHT;
}

/* Sample the ball's surface colour for a ball-local unit normal. */
/* The measles spots. On by default because they are the only thing that shows
 * what the white is doing, and optional because not everybody wants a spotted
 * ball on the table. */
static int s_cue_spots = 1;
void cue_render_set_cue_spots(int on) { s_cue_spots = on ? 1 : 0; }

static uint16_t ball_sample(uint8_t id, Vec3 nb, uint16_t base) {
    /* Cue ball: a "measles" spotted ball — six small red dots, one centred on
     * each axis pole (±x, ±y, ±z), so spin reads clearly however it rolls. */
    if (id == CUE_ID_CUE) {
        if (!s_cue_spots) return base;          /* a plain white, if that is wanted */
        float ax = fabsf(nb.x), ay = fabsf(nb.y), az = fabsf(nb.z);
        float m = ax > ay ? (ax > az ? ax : az) : (ay > az ? ay : az);
        /* 0.980, not 0.965. The threshold is a cosine, so the dot's angular
         * radius is acos(m) — 15.2 degrees at 0.965 and 11.5 at 0.980, which
         * is a quarter off the area. They are there to show spin, and they read
         * as spin at a size a real measles ball actually has; bigger than that
         * and the white starts looking like a different ball. */
        /* Red on white, and near-white on anything else: the spots exist to
         * show spin, and a red spot on a red cue ball shows nothing. */
        if (m > 0.980f)
            return bset()->cue ? RGB565C(242, 238, 228) : RGB565C(198, 58, 46);
        return base;
    }
    if (s_is_snooker) return base;              /* snooker balls are unmarked */
    const CueBallSet *bs = bset();
    int us = bs->numbered;
    if (id >= 9 && id <= 15) {
        if (bs->striped && fabsf(nb.y) < bs->half) {
            /* don't paint the stripe over the number circle */
            if (!(us && nb.x > 0.90f))
                return bs->band ? bs->band : bs->hue[id - 8];
        }
        return number_patch(id, nb, base, us);  /* UK: solid body, no stripe */
    }
    if (id >= 1 && id <= 8)                       /* solids + 8 */
        return number_patch(id, nb, base, us);
    return base;
}

/* ---- raster ------------------------------------------------------------ */
static void draw_ball(uint16_t *fb, uint16_t *depth, const Sprite *sp,
                      int y0, int y1) {
    int rad = (int)(sp->rad + 0.999f);
    float inv_rad = 1.0f / sp->rad;
    int icx = (int)(sp->cx + 0.5f), icy = (int)(sp->cy + 0.5f);
    uint16_t base = ball_base(sp->id);
    /* camera-to-surface dir (specular) and light, world space. */
    Vec3 vcam = v3_scale(s_view.basis.r[2], -1.0f);   /* toward camera */
    Vec3 H = v3_norm(v3_add(s_light, vcam));
    /* Overhead fixture = 4 lamps in a 2×2 cluster → 4 sharp reflection dots.
     * Each reflects where the surface normal ≈ that lamp's half-vector. */
    const float lx = 0.42f, lz = 0.28f;   /* wide enough to read as 4 dots */
    Vec3 Hl[4];
    Hl[0] = v3_norm(v3_add(v3_norm(v3(s_light.x+lx, s_light.y, s_light.z+lz)), vcam));
    Hl[1] = v3_norm(v3_add(v3_norm(v3(s_light.x-lx, s_light.y, s_light.z+lz)), vcam));
    Hl[2] = v3_norm(v3_add(v3_norm(v3(s_light.x+lx, s_light.y, s_light.z-lz)), vcam));
    Hl[3] = v3_norm(v3_add(v3_norm(v3(s_light.x-lx, s_light.y, s_light.z-lz)), vcam));
    float R = s_ballR;
    for (int py = icy - rad; py <= icy + rad; py++) {
        if (py < y0 || py >= y1 || py < 0 || py >= CUE_FB_H) continue;
        float v = (py - sp->cy) * inv_rad;
        uint16_t *frow = fb + py * R3D_FB_W;
        uint16_t *drow = depth + py * R3D_FB_W;
        for (int px = icx - rad; px <= icx + rad; px++) {
            if (px < 0 || px >= CUE_FB_W) continue;
            float u = (px - sp->cx) * inv_rad;
            float rr = u * u + v * v;
            if (rr > 1.0f) continue;
            float nz = sqrtf(1.0f - rr);
            /* per-pixel depth: nearer than centre by R*nz. */
            float zpix = sp->viewz - R * nz;
            if (zpix < CUE_NEAR) zpix = CUE_NEAR;
            uint16_t d = (uint16_t)(CUE_DEPTH_K / zpix);
            if (d <= drow[px]) continue;
            /* view-space normal (screen y down → view up = -v). */
            Vec3 Nv = v3(u, -v, -nz);
            Vec3 Nw = m3_mul_v3(&s_view.basis, Nv);     /* view→world */
            float diff = v3_dot(Nw, s_light); if (diff < 0) diff = 0;
            float s = v3_dot(Nw, H); if (s < 0) s = 0;  /* specular base */
            float down = -Nw.y; if (down < 0) down = 0; /* underside (faces cloth) */
            Vec3 Nb = m3_mul_v3_t(&sp->orient, Nw);     /* world→ball-local */
            uint16_t bc = ball_sample(sp->id, Nb, base);
            uint16_t col;
            switch (s_light_mode) {
            case 0:  /* SMOOTH (original soft look) */
                col = shade565(bc, (0.30f + 0.70f*diff) * (0.78f + 0.22f*nz));
                { float ss = s; ss*=ss; ss*=ss; ss*=ss; int hi=(int)(ss*26.0f);
                  if (hi>0) col = add565(col, hi, hi*2, hi); }
                break;
            case 2:  /* TOON: banded diffuse + cloth shadow + crisp dot */
                col = shade565(bc, diff>0.62f?1.0f : diff>0.30f?0.74f : 0.52f);
                col = mix565(col, s_cloth_shadow, (1.0f-diff)*0.40f + down*0.22f);
                if (s > 0.82f) col = RGB565C(250,250,250);
                break;
            case 3:  /* GLOSS: smooth body, strong cloth tint, sharp hotspot */
                col = shade565(bc, 0.30f + 0.70f*diff);
                col = mix565(col, s_cloth_shadow, (1.0f-diff)*0.50f + down*0.40f);
                if (s > 0.60f) { float h=(s-0.60f)*2.5f; h*=h*h; int hi=(int)(h*30.0f);
                  if (hi>0) col = add565(col, hi, hi, hi); }
                break;
            case 4:  /* 4-DOT medium */
            case 5:  /* 4-DOT large/soft */
            default: /* 1 = 4-DOT sharp: polished ball reflecting the 4 overhead
                      * lamps as crisp bright dots; saturated body, cloth-tinted
                      * lower half. */
            {
                float thr = (s_light_mode==5) ? 0.93f : (s_light_mode==4) ? 0.955f : 0.975f;
                float gain = (s_light_mode==5) ? 0.85f : 1.0f;
                col = shade565(bc, 0.46f + 0.54f*diff);
                col = mix565(col, s_cloth_shadow, (1.0f-diff)*0.40f + down*0.42f);
                float refl = 0.0f;
                for (int li = 0; li < 4; li++) {
                    float si = v3_dot(Nw, Hl[li]);
                    if (si > thr) { float h = (si - thr) / (1.0f - thr); refl += h*h; }
                }
                if (refl > 1.0f) refl = 1.0f;
                /* lamp reflections are neutral white, NOT a brighter shade of
                 * the ball — blend toward pure white so every ball shows the
                 * same white dots (no pink/coloured tinge). */
                if (refl > 0.0f) col = mix565(col, RGB565C(255,255,255), refl * gain);
                break;
            }
            }
            frow[px] = col;
            drow[px] = d;
        }
    }
}

void cue_render_raster(uint16_t *fb, int y0, int y1) {
    if (y0 < 0) y0 = 0;
    if (y1 > CUE_FB_H) y1 = CUE_FB_H;
    if (y0 >= y1) return;
    r3d_raster_set_fb(fb);
    uint16_t *depth = r3d_depth_buffer();

    /* background vertical gradient + depth clear */
    for (int y = y0; y < y1; y++) {
        float t = (float)y / (float)CUE_FB_H;
        int r = (int)(((s_bg_top >> 11) & 31) * (1 - t) + ((s_bg_bot >> 11) & 31) * t);
        int g = (int)(((s_bg_top >> 5) & 63) * (1 - t) + ((s_bg_bot >> 5) & 63) * t);
        int b = (int)(((s_bg_top) & 31) * (1 - t) + ((s_bg_bot) & 31) * t);
        uint16_t c = (uint16_t)((r << 11) | (g << 5) | b);
        uint16_t *row = fb + y * R3D_FB_W;
        for (int x = 0; x < R3D_FB_W; x++) row[x] = c;
    }
    r3d_depth_clear(y0, y1);

    /* table triangles — BED first (flat cloth), so shadows can paint over it
     * without the slate occluding them; the RAISED geometry (cushions, rails,
     * pocket voids) is drawn after the shadows and depth-tests over them. */
    for (int i = 0; i < s_bed_nstri; i++) {
        const STri *t = &s_stri[i];
        r3d_tri(t->x0, t->y0, t->d0, t->x1, t->y1, t->d1,
                t->x2, t->y2, t->d2, t->color, y0, y1);
    }

    /* soft ground-plane shadow decals lying flat on the cloth. Each is an
     * ellipse C + s*U + t*V (|(s,t)|<=1) where U,V are the screen projections
     * of world +X/+Z offsets, so it foreshortens with the table and spreads
     * toward the camera (stays visible at the low aim-cam). We invert the 2×2
     * [U V] per pixel to recover (s,t) and fade darkness from centre to edge. */
    for (int i = 0; i < s_nshadow; i++) {
        float cx = s_shadow[i].cx, cy = s_shadow[i].cy;
        float ux = s_shadow[i].ux, uy = s_shadow[i].uy;
        float vx = s_shadow[i].vx, vy = s_shadow[i].vy;
        float det = ux * vy - uy * vx;
        if (det > -1e-4f && det < 1e-4f) continue;
        float inv = 1.0f / det;
        int bx = (int)(fabsf(ux) + fabsf(vx)) + 1;   /* screen bounding box */
        int by = (int)(fabsf(uy) + fabsf(vy)) + 1;
        int x0 = (int)cx - bx, x1b = (int)cx + bx;
        int yy0 = (int)cy - by, yy1 = (int)cy + by;
        for (int py = yy0; py <= yy1; py++) {
            if (py < y0 || py >= y1 || py < 0 || py >= CUE_FB_H) continue;
            uint16_t *frow = fb + py * R3D_FB_W;
            float ry = py - cy;
            for (int px = x0; px <= x1b; px++) {
                if (px < 0 || px >= CUE_FB_W) continue;
                float rx = px - cx;
                float s = ( rx * vy - ry * vx) * inv;
                float t = (-rx * uy + ry * ux) * inv;
                float r2 = s * s + t * t;
                if (r2 > 1.0f) continue;
                /* No depth test: shadows are drawn AFTER the cloth bed but
                 * BEFORE the raised geometry, so the slate never occludes them
                 * while cushions/rails (drawn next, depth-tested) paint over. */
                float k = 0.5f + 0.5f * r2 * r2;
                frow[px] = shade565(frow[px], k);
            }
        }
    }

    /* raised table geometry (cushions, rail frame, pocket voids) — depth-tested
     * over the shadows so a cushion/rail correctly hides a shadow behind it. */
    for (int i = s_bed_nstri; i < s_lip_nstri; i++) {
        const STri *t = &s_stri[i];
        r3d_tri(t->x0, t->y0, t->d0, t->x1, t->y1, t->d1,
                t->x2, t->y2, t->d2, t->color, y0, y1);
    }
    /* Pocket drop lips: depth-TESTED (cushions/wood occlude them) but NOT
     * depth-WRITING, so the balls drawn afterwards always cover them. Uses the
     * no-write triangle (per-call flag, NOT a shared global) so the two cores
     * can raster concurrently — one core's lip pass no longer stops the other
     * core writing depth for the frame/cushions (that was the device flicker). */
    for (int i = s_lip_nstri; i < s_nstri; i++) {
        const STri *t = &s_stri[i];
        r3d_tri_nowrite(t->x0, t->y0, t->d0, t->x1, t->y1, t->d1,
                        t->x2, t->y2, t->d2, t->color, y0, y1);
    }

    /* aim dots (cue path, pale yellow) + object-ball path (cyan) */
    for (int i = 0; i < s_ndot; i++)
        r3d_point((int)s_dot[i].x, (int)s_dot[i].y, 65000, RGB565C(240,240,160),
                  1, y0, y1);
    for (int i = 0; i < s_nodot; i++)
        r3d_point((int)s_odot[i].x, (int)s_odot[i].y, 65000, RGB565C(120,230,235),
                  1, y0, y1);

    /* ghost-ball ring */
    if (s_ghost.on) {
        int seg = 18;
        for (int k = 0; k < seg; k++) {
            float a0 = k * (6.2831853f / seg), a1 = (k + 1) * (6.2831853f / seg);
            r3d_line(s_ghost.cx + s_ghost.rad * cosf(a0),
                     s_ghost.cy + s_ghost.rad * sinf(a0), 65000,
                     s_ghost.cx + s_ghost.rad * cosf(a1),
                     s_ghost.cy + s_ghost.rad * sinf(a1), 65000,
                     RGB565C(230, 230, 230), y0, y1);
        }
    }

    /* balls */
    for (int i = 0; i < s_nspr; i++) draw_ball(fb, depth, &s_spr[i], y0, y1);

    /* cue stick (over the balls): a tapered shaded shaft drawn as three FULL-WIDTH
     * sections along its length — a 1px blue tip, a ~4px ivory ferrule, then the
     * wood shaft widening to the butt. Each section spans the full cue cross-
     * section (not a centre line), so the tip cap reads as the end of a cylinder. */
    if (s_cue.on) {
        float dx = s_cue.bx - s_cue.tx, dy = s_cue.by - s_cue.ty;
        float L = sqrtf(dx*dx + dy*dy);
        if (L > 1.0f) {
            float ux = dx/L, uy = dy/L;               /* along */
            float px = -uy, py = ux;                  /* perpendicular */
            float wt = 1.4f, wb = 4.3f;               /* half-width: tip → butt */
            float seg[4] = { 0.0f, 1.0f, 5.0f, L };   /* tip | ferrule | shaft */
            uint16_t col[3] = { RGB565C(70,90,180), RGB565C(238,234,212), s_cue.color };
            if (seg[1] > L) seg[1] = L;
            if (seg[2] > L) seg[2] = L;
            for (int s = 0; s < 3; s++) {
                float s0 = seg[s], s1 = seg[s+1]; if (s1 <= s0) continue;
                float w0 = wt + (wb-wt)*(s0/L), w1 = wt + (wb-wt)*(s1/L);
                float ax = s_cue.tx+ux*s0, ay = s_cue.ty+uy*s0;
                float bx2 = s_cue.tx+ux*s1, by2 = s_cue.ty+uy*s1;
                float aLx=ax+px*w0, aLy=ay+py*w0, aRx=ax-px*w0, aRy=ay-py*w0;
                float bLx=bx2+px*w1, bLy=by2+py*w1, bRx=bx2-px*w1, bRy=by2-py*w1;
                r3d_tri(aLx,aLy,65000, aRx,aRy,65000, bRx,bRy,65000, col[s], y0,y1);
                r3d_tri(aLx,aLy,65000, bRx,bRy,65000, bLx,bLy,65000, col[s], y0,y1);
            }
            /* sheen down the wood for a rounded look */
            float ss = seg[2];
            r3d_line(s_cue.tx+ux*ss, s_cue.ty+uy*ss, 65000, s_cue.bx, s_cue.by, 65000,
                     RGB565C(244,214,150), y0,y1);
        }
    }
}

/* Draw one flat sphere-shaded ball icon using the live ball_base/ball_sample.
 * face_x: 0 = +z toward viewer (stripe reads as a mid band; for HUD group hint);
 *         1 = +x toward viewer (the number circle faces out; for menu previews). */
/* ---- HUD icon target ---------------------------------------------------- *
 * The icon helpers below used to write with a hard-coded CUE_FB_W stride and clip
 * to CUE_FB_H. That is right for the handheld, where the HUD IS the framebuffer,
 * and silently wrong for the VR build, whose panel is 512x288: every icon was
 * clipped away at x >= 128 and simply never appeared. Same shape of fix as
 * craft_font_set_target — the caller says how big the target is, and the default
 * keeps the handheld byte-identical. */
static int s_icon_w = CUE_FB_W, s_icon_h = CUE_FB_H;
void cue_render_icon_target(int w, int h) {
    s_icon_w = (w > 0) ? w : CUE_FB_W;
    s_icon_h = (h > 0) ? h : CUE_FB_H;
}

/* A persona's avatar, from the 2dpool portraits baked into cue_faces.h. Nearest
 * sampled from the 32x32 source into whatever box is asked for, with the alpha
 * respected so the cut-out sits on the panel rather than in a black square. */
void cue_render_face(uint16_t *fb, int cx, int cy, int size, int persona) {
    if (!fb || persona < 0 || persona >= CUE_NUM_FACES || size <= 0) return;
    const uint16_t *src = cue_face32_rgb[persona];
    const uint8_t  *sa  = cue_face32_a[persona];
    int x0 = cx - size / 2, y0 = cy - size / 2;
    for (int j = 0; j < size; j++) {
        int y = y0 + j;
        if (y < 0 || y >= s_icon_h) continue;
        int sy = j * CUE_FACE32_H / size;
        for (int i = 0; i < size; i++) {
            int x = x0 + i;
            if (x < 0 || x >= s_icon_w) continue;
            int sx = i * CUE_FACE32_W / size;
            int a = sa[sy * CUE_FACE32_W + sx];
            if (a < 24) continue;
            uint16_t c = src[sy * CUE_FACE32_W + sx];
            if (a >= 232) { fb[y * s_icon_w + x] = c; continue; }
            uint16_t d = fb[y * s_icon_w + x];
            int r = ((c >> 11) & 31), g = ((c >> 5) & 63), b = (c & 31);
            int dr = ((d >> 11) & 31), dg = ((d >> 5) & 63), db = (d & 31);
            dr += ((r - dr) * a) >> 8;
            dg += ((g - dg) * a) >> 8;
            db += ((b - db) * a) >> 8;
            fb[y * s_icon_w + x] = (uint16_t)((dr << 11) | (dg << 5) | db);
        }
    }
}

static void draw_ball_icon(uint16_t *fb, int cx, int cy, int rad, uint8_t id, int face_x) {
    uint16_t base = ball_base(id);
    for (int dy = -rad; dy <= rad; dy++) {
        int y = cy + dy; if (y < 0 || y >= s_icon_h) continue;
        for (int dx = -rad; dx <= rad; dx++) {
            int x = cx + dx; if (x < 0 || x >= s_icon_w) continue;
            float u = (float)dx / rad, v = (float)dy / rad;
            float r2 = u * u + v * v; if (r2 > 1.0f) continue;
            float nz = sqrtf(1.0f - r2);
            Vec3 nb = face_x ? v3(nz, -v, u) : v3(u, -v, nz);
            uint16_t c = ball_sample(id, nb, base);
            float hl = -0.5f * u - 0.5f * v + 0.7f * nz;   /* top-left spec */
            if (hl > 0.95f) c = RGB565C(255, 255, 255);
            else            c = shade565(c, 0.45f + 0.55f * nz);
            fb[y * s_icon_w + x] = c;
        }
    }
}

/* The flat colour of a ball, so a HUD can write a NUMBER in it. A break broken
 * down as "5 reds, 4 blacks" wants the counts themselves coloured — that is the
 * whole reading of it at a glance — and only this file knows what colour a ball
 * is under the live set. */
uint16_t cue_render_ball_colour(int id) { return ball_base((uint8_t)id); }

/* HUD group hint. Pick boldly-distinct reps so the two sides never read the same:
 * low/solids = a red SOLID (3), high/stripes = a blue STRIPE (10). Avoids the
 * yellow solid-vs-stripe pair (1 / 9) which was hard to tell apart at icon size.
 * In UK sets (no stripes) these map to the two group colours anyway. */
void cue_render_group_icon(uint16_t *fb, int cx, int cy, int rad, int group) {
    draw_ball_icon(fb, cx, cy, rad, (group == 2) ? 10 : 3, 0);
}

/* HUD: draw a specific ball id (number circle facing out) with the live set —
 * used to show the 9-ball "ball to pot next". */
void cue_render_ball_icon(uint16_t *fb, int cx, int cy, int rad, int id) {
    draw_ball_icon(fb, cx, cy, rad, (uint8_t)id, 1);
}

/* Snooker "ball on" icon: target 0 = a RED ball, 2 = the sequence colour
 * (value `seq`, 2..7), 1 = "any colour" drawn as a 6-wedge multicolour ball. */
void cue_render_onball_icon(uint16_t *fb, int cx, int cy, int rad, int target, int seq) {
    int was = s_is_snooker; s_is_snooker = 1;
    if (target == 0) { draw_ball_icon(fb, cx, cy, rad, 1, 0); s_is_snooker = was; return; }
    if (target == 2) { draw_ball_icon(fb, cx, cy, rad, (uint8_t)(18 + seq), 0); s_is_snooker = was; return; }
    /* any colour → 6 angular wedges of the snooker colours */
    static const uint16_t cols[6] = {
        RGB565C(235,200,40), RGB565C(20,130,50), RGB565C(120,70,35),
        RGB565C(30,80,200), RGB565C(235,120,150), RGB565C(40,40,44) };
    for (int dy = -rad; dy <= rad; dy++) {
        int y = cy + dy; if (y < 0 || y >= s_icon_h) continue;
        for (int dx = -rad; dx <= rad; dx++) {
            int x = cx + dx; if (x < 0 || x >= s_icon_w) continue;
            float u = (float)dx/rad, v = (float)dy/rad;
            float r2 = u*u + v*v; if (r2 > 1.0f) continue;
            float nz = sqrtf(1.0f - r2);
            int w = (int)((atan2f(v, u) + 3.14159265f) * (6.0f / 6.2831853f));
            if (w < 0) w = 0; if (w > 5) w = 5;
            float hl = -0.5f*u - 0.5f*v + 0.7f*nz;
            uint16_t c = (hl > 0.95f) ? RGB565C(255,255,255) : shade565(cols[w], 0.45f + 0.55f*nz);
            fb[y*s_icon_w + x] = c;
        }
    }
    s_is_snooker = was;
}

/* 3D-shaded cue ball for the spin/aim HUD: a white sphere with a specular
 * highlight and the tip-contact marker drawn on its front face at (side,vert)
 * (fractions of R; +side = right english, +vert = top/follow). Replaces the old
 * flat 2D disc so the spin readout matches the game balls. */
void cue_render_spin_ball(uint16_t *fb, int cx, int cy, int rad,
                          float side, float vert) {
    const uint16_t body = RGB565C(238, 238, 228);
    const uint16_t spot = RGB565C(205, 45, 40);
    const uint16_t ring = RGB565C(120, 24, 22);
    float ms = 0.30f;                 /* marker radius (fraction of ball) */
    for (int dy = -rad; dy <= rad; dy++) {
        int y = cy + dy; if (y < 0 || y >= s_icon_h) continue;
        for (int dx = -rad; dx <= rad; dx++) {
            int x = cx + dx; if (x < 0 || x >= s_icon_w) continue;
            float u = (float)dx / rad, v = (float)dy / rad;
            float r2 = u * u + v * v; if (r2 > 1.0f) continue;
            float nz = sqrtf(1.0f - r2);
            float mu = u - side, mv = v + vert;       /* offset to contact point */
            float md = sqrtf(mu * mu + mv * mv);
            uint16_t c;
            if (md < ms * 0.62f)        c = spot;     /* contact dot */
            else if (md < ms)           c = ring;     /* dark rim around it */
            else {
                float hl = -0.5f * u - 0.5f * v + 0.7f * nz;
                c = (hl > 0.95f) ? RGB565C(255,255,255)
                                 : shade565(body, 0.5f + 0.5f * nz);
            }
            fb[y * s_icon_w + x] = c;
        }
    }
}

/* Ball-set preview row for the menu: a representative solid, stripe and the 8
 * (or red/colour/black for snooker), drawn with the given set so the player can
 * see what they're picking. Temporarily overrides the active set/snooker flag. */
void cue_render_set_preview(uint16_t *fb, int cx, int cy, int rad,
                            int ballset, int snooker) {
    int sb = s_ball_set, ss = s_is_snooker;
    s_ball_set = (ballset < 0 || ballset >= cue_render_ballset_count()) ? 0 : ballset;
    s_is_snooker = snooker;
    /* A small 6-ball triangle rack (rows of 1/2/3). Mixed solids + stripes so
     * each set reads clearly, with the BLACK (8) in the centre of the base row
     * (the rack's centre line). Snooker shows reds + colours instead. */
    /* distinct hues (per-number sets): yellow,blue,red,purple,green + black; idx4
     * is the rack centre. Mix of solids (1,3,6) and stripes (10,12). */
    static const uint8_t rack_pool[6] = { 1, 10, 3, 12, 8, 6 };
    static const uint8_t rack_snk[6]  = { 1, CUE_ID_YELLOW, CUE_ID_GREEN,
                                          CUE_ID_BROWN, CUE_ID_BLACK, CUE_ID_BLUE };
    const uint8_t *ids = snooker ? rack_snk : rack_pool;
    float dx = rad * 2.0f, dy = rad * 1.78f;
    int idx = 0;
    for (int row = 0; row < 3; row++) {
        float ry = cy + (row - 1) * dy;
        for (int j = 0; j <= row; j++) {
            float rx = cx + (j - row * 0.5f) * dx;
            draw_ball_icon(fb, (int)(rx + 0.5f), (int)(ry + 0.5f), rad, ids[idx++], 1);
        }
    }
    s_ball_set = sb; s_is_snooker = ss;
}

uint16_t cue_render_ball_texel(uint8_t id, Vec3 nb) {
    return ball_sample(id, nb, ball_base(id));
}
