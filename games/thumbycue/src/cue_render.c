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
/* WHETHER THE BAKED TABLE CARRIES THE SKITTLES.
 *
 * The handheld rasterises the whole table every frame, so a skittle built into
 * it animates for free. CueVR uploads the table ONCE as a GPU mesh — so a
 * skittle baked into it can never move, which is exactly what was reported: the
 * ball was deflected, the pin was booked over, and nothing on the table so much
 * as leant. CueVR turns them off here and draws its own, transformed per frame.
 */
static int      s_skittles = 1;
void cue_render_set_skittles(int on) { s_skittles = on ? 1 : 0; }

/* The turned profile: height above the foot, and radius as a multiple of the
 * stem's. Shared so that a front-end building its own mesh cannot end up
 * drawing a different skittle from the one the rules describe. */
static const float CUE_SKITTLE_PROF[][2] = {
        { 0.000f, 1.62f },   /* the flared foot it stands on */
        { 0.005f, 1.34f },
        { 0.011f, 1.00f },
        { 0.018f, 1.14f },   /* a turned ring, as the real ones have */
        { 0.026f, 1.00f },
        { 0.051f, 1.00f },   /* the rule's cylinder — all a ball reaches */
        { 0.082f, 1.00f },   /* the stem stays SLIM right to the cap:
                              * thickening it early is what read as a
                              * spear of asparagus */
        { 0.090f, 1.20f },
        { 0.094f, 2.90f },   /* the underside of the brim, flaring hard */
        { 0.097f, 3.40f },   /* the rim — over three stems wide */
        { 0.104f, 3.26f },   /* a LOW dome: barely rising... */
        { 0.110f, 2.60f },
        { 0.1132f, 1.55f },  /* ...and rounding off to a broad top */
        { 0.114f, 0.00f },
    };

int cue_render_skittle_profile(const float (**pts)[2]) {
    if (pts) *pts = CUE_SKITTLE_PROF;
    return (int)(sizeof CUE_SKITTLE_PROF / sizeof CUE_SKITTLE_PROF[0]);
}

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
    (void)ex; (void)ez;
    if (w->npocket < 7) return;
    Vec3 V[6]; int rf = 3;
    if (!cue_table_L_outline(t, V, &rf)) return;
    const float m = t->rail_w * CUE_SLATE_RAIL;

    /* THE WALK ROUND THE OUTLINE, and nothing here knows which way the table
     * turns. It used to: this was written for a right-handed L and undid the
     * world's mirror with an index macro, redid it with a coordinate one, and
     * reversed the result — one of six places doing the same mirror, and the
     * one that got it wrong. cue_table_L_outline now hands back the vertices
     * for whichever hand this table is, already in the order the boundary meets
     * them, and the pockets come out of build_L in the same order. So this is
     * simply a walk.
     *
     * Each edge has an outward normal from the winding and a slate line one
     * margin beyond it. At a corner the cloth is scalloped between the rail
     * being left and the rail being arrived on; at the elbow — which is timber,
     * not a pocket — the slate just turns. */
    float len[6]; Vec3 out[6], slate[6];
    for (int i = 0; i < 6; i++) {
        Vec3 a = V[i], b = V[(i + 1) % 6];
        float dx = b.x - a.x, dz = b.z - a.z;
        len[i] = sqrtf(dx*dx + dz*dz);
        if (len[i] < 1e-5f) { out[i] = v3(0,0,0); slate[i] = v3(0,0,0); continue; }
        out[i] = v3(dz / len[i], 0.0f, -dx / len[i]);
        /* THE LINE THIS RAIL RUNS OUT TO: the edge pushed out by the margin,
         * and nothing else. An earlier version snapped any outward-facing rail
         * to the slate's extent, which is right for the four outer rails and
         * WRONG for the notch's underside — that faces +z as well, and is
         * interior. It does not need snapping anyway: the margin IS the
         * difference between half_wid and the slate extent, so an outer rail
         * lands on it exactly. */
        slate[i] = v3(a.x + out[i].x * m, 0, a.z + out[i].z * m);
    }
    /* Which pocket sits at each vertex, and which edge carries a middle. The
     * pockets are in walk order, so counting them off as we go is enough. */
    int pk = 0;
    for (int i = 0; i < 6; i++) {
        const int ia = i, ip = (i + 5) % 6;
        if (ia == rf) {
            /* the elbow: solid timber, so the slate turns without a scallop */
            pt_into(B, (out[ip].x > 0.5f || out[ip].x < -0.5f) ? slate[ip].x : slate[i].x,
                       (out[ip].z > 0.5f || out[ip].z < -0.5f) ? slate[ip].z : slate[i].z, -1);
            continue;
        }
        /* the corner at V[ia]: in off the previous rail, out along this one */
        float la_v = (out[ip].x >  0.9f) ? slate[ip].x : (out[ip].x < -0.9f) ? slate[ip].x
                   : (out[ip].z >  0.9f) ? slate[ip].z : slate[ip].z;
        float lb_v = (out[i].x  >  0.9f) ? slate[i].x  : (out[i].x  < -0.9f) ? slate[i].x
                   : (out[i].z  >  0.9f) ? slate[i].z  : slate[i].z;
        Vec3 along = v3((V[(i+1)%6].x - V[ia].x) / (len[i] > 1e-5f ? len[i] : 1.0f), 0,
                        (V[(i+1)%6].z - V[ia].z) / (len[i] > 1e-5f ? len[i] : 1.0f));
        scallop_rails(B, w, pk++, out[ip], la_v, out[i], lb_v, along);
        if (w->pocket_mid[pk % w->npocket] && pk < w->npocket) {
            /* this rail's middle, walking on along it */
            scallop_rails(B, w, pk, out[i], lb_v, out[i], lb_v, along);
            pk++;
        }
    }
}

/* The whole cloth boundary: six cuts, joined by the slate's straight edges.
 * Each cut supplies its own two leg ends, so the "edges" are simply the lines
 * between one cut's exit and the next one's entry, and there is nothing to
 * clip or detect. */
/* ---- S2: A REGULAR BED'S OUTLINE ----------------------------------------- *
 *
 * The rectangle's walk below finds six pockets by the SIGN of their
 * coordinates, calls anything under index four a corner, and runs each
 * scallop's legs out to +-ex/+-ez. None of that means anything on a pentagon.
 *
 * But a regular bed is far kinder than the L was, because it is REGULAR: every
 * corner is the same corner, so the outline is one construction repeated. The
 * slate is the bed polygon pushed out by the rail overhang — which for a
 * regular polygon is another regular polygon, apothem plus the overhang — and
 * at each pocket the cloth is bitten by an arc about the cut centre.
 *
 * The arc's ends are put exactly ON the two slate lines it sits between, so
 * there are no legs to draw: the boundary is arc, straight run, arc, straight
 * run, all the way round. The rectangle needs legs because its arc endpoints
 * stop short of the slate edge; placing them on it is simply the tidier
 * construction and it is available here because the geometry is uniform.
 *
 * Corners with no pocket (which is how a round bed is asked for — a pocket
 * every so many corners) contribute their own outset vertex and nothing else. */
static void build_bed_boundary_ngon(const CueTable *t, const CueWorld *w,
                                    CueBnd *B) {
    const int n = cue_table_ngon_sides(t);
    int every = t->bed_pocket_every < 1 ? 1 : t->bed_pocket_every;
    if (every > n) every = n;
    const float PI = 3.14159265f;
    const float rr = t->half_len;                     /* bed circumradius */
    const float ap = rr * cosf(PI / (float)n);        /* ...and its apothem */
    const float over = t->rail_w * CUE_SLATE_RAIL;    /* how far the slate runs on */
    const float sr = (ap + over) / (ap > 1e-5f ? ap : 1.0f) * rr;  /* slate radius */
    B->n = 0;

    for (int i = 0; i < n && B->n < CUE_BND_MAX; i++) {
        const Vec3 V  = cue_table_ngon_vert(t, i);
        const Vec3 Vn = cue_table_ngon_vert(t, (i + 1) % n);
        const Vec3 Vp = cue_table_ngon_vert(t, (i + n - 1) % n);
        const float vl = sqrtf(V.x*V.x + V.z*V.z);
        if (vl < 1e-5f) continue;

        if (i % every) {                       /* no pocket here: just the corner */
            B->p[B->n] = v3(V.x / vl * sr, 0.0f, V.z / vl * sr);
            B->pk[B->n] = -1; B->n++;
            continue;
        }
        const int pk = i / every;
        if (pk >= w->npocket) continue;
        const Vec3  C = w->cut_c[pk];
        const float R = w->cut_r[pk];

        /* The two edges meeting here, as unit directions leaving the vertex,
         * and the outward normal of each. */
        float ix = Vp.x - V.x, iz = Vp.z - V.z;         /* back along the last edge */
        float ox = Vn.x - V.x, oz = Vn.z - V.z;         /* on along the next */
        float il = sqrtf(ix*ix + iz*iz), ol = sqrtf(ox*ox + oz*oz);
        if (il < 1e-5f || ol < 1e-5f) continue;
        ix /= il; iz /= il; ox /= ol; oz /= ol;
        /* Outward normals: the bed is convex about the origin, so the normal
         * is whichever perpendicular points away from the middle. */
        float inx = -iz, inz =  ix;
        if (inx * V.x + inz * V.z < 0.0f) { inx = -inx; inz = -inz; }
        float onx =  oz, onz = -ox;
        if (onx * V.x + onz * V.z < 0.0f) { onx = -onx; onz = -onz; }

        /* Where the cut circle crosses each slate line. The line is
         * dot(X, n) = ap + over; the foot of the perpendicular from C is
         * h along n, and the chord reaches sqrt(R^2 - h^2) either side. */
        float ain = 0.0f, aout = 0.0f;
        {   float h = (ap + over) - (C.x * inx + C.z * inz);
            float k = R*R - h*h; k = k > 0.0f ? sqrtf(k) : 0.0f;
            float fx = C.x + inx * h, fz = C.z + inz * h;
            ain = atan2f(fz + iz * k - C.z, fx + ix * k - C.x);
        }
        {   float h = (ap + over) - (C.x * onx + C.z * onz);
            float k = R*R - h*h; k = k > 0.0f ? sqrtf(k) : 0.0f;
            float fx = C.x + onx * h, fz = C.z + onz * h;
            aout = atan2f(fz + oz * k - C.z, fx + ox * k - C.x);
        }
        /* Sweep the short way round, through the side facing the table — the
         * bite is out of the CLOTH, so the arc bulges inward, not outward. */
        float d = aout - ain;
        while (d >  PI) d -= 2.0f * PI;
        while (d < -PI) d += 2.0f * PI;
        {   float amid = ain + d * 0.5f;
            float mx = cosf(amid), mz = sinf(amid);
            if (mx * V.x + mz * V.z > 0.0f)         /* bulging outward: go the other way */
                d += (d > 0.0f) ? -2.0f * PI : 2.0f * PI;
        }
        arc_into(B, C, R, ain, ain + d, pk, 20);
    }
}

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
    if (t->bed_shape == CUE_BED_NGON) { build_bed_boundary_ngon(t, w, B); return; }
    if (t->bed_shape == CUE_BED_L) { build_bed_boundary_L(t, w, B, ex, ez); return; }

    /* ---- G6: BAR BILLIARDS' CLOTH IS A PLAIN RECTANGLE ------------------
     *
     * Everything below finds six pockets by the sign of their coordinates and
     * cuts a scallop into the boundary at each. Bar billiards has none on its
     * boundary at all — its nine holes are bored through the MIDDLE of the bed,
     * which a single closed outline cannot express however it is walked.
     *
     * So the cloth is the rectangle, and the holes are cut as discs on top of
     * it (see the bar-billiards block in the table build). That is also what
     * the table looks like: a flat green bed with nine small round holes in
     * it, not a cloth scalloped round its edge. */
    if (t->kind == CUE_GAME_BARBILLIARDS) {
        B->n = 0;
        #define BB_V(x_, z_) do { B->p[B->n] = v3((x_), 0.0f, (z_)); \
                                  B->pk[B->n] = -1; B->n++; } while (0)
        BB_V(-ex, -ez); BB_V( ex, -ez); BB_V( ex,  ez); BB_V(-ex,  ez);
        #undef BB_V
        return;
    }

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

/* THE ROLL ITSELF, given a run of cut-edge points and their outward normals.
 *
 * One piece of cloth doing one thing: over the edge on a quarter-cosine, in M
 * rings, darkening as it turns under, and then a wall dropped straight down
 * from the rolled edge. A rail pocket walks a scalloped ARC of the bed's
 * outline through it; a hole bored through the middle of the bed walks a full
 * CIRCLE, which is the same roll closed up — `closed` wraps the last quad back
 * to the first so there is no seam where the ring meets itself.
 *
 * The wall has no apex to converge on, deliberately: it used to be a fan from a
 * point on the pocket floor, and a fan converging on a point is a spray of long
 * thin triangles, visible across the mouth of every pocket as spokes. Below it
 * the frame's tray closes the table off, so it needs no floor. */
static void emit_lip_run(const CueTable *t, Vec3 *ring0, const Vec3 *nrm,
                         int cnt, float ld, int M, int closed)
{
    if (cnt < 2 || ld <= 0.0f) return;
    Vec3 base[CUE_LIP_MAX];
    for (int k = 0; k < cnt; k++) base[k] = ring0[k];
    const int last = closed ? cnt : cnt - 1;
    for (int sring = 1; sring <= M; sring++) {
        float phi = (float)sring / M * 1.5707963f;
        float tn = sinf(phi), yy = -ld * (1.0f - cosf(phi));
        uint16_t col = shade565(t->cloth, 1.0f - 0.92f * (1.0f - cosf(phi)));
        Vec3 ring1[CUE_LIP_MAX];
        for (int k = 0; k < cnt; k++)
            ring1[k] = v3(base[k].x + nrm[k].x * ld * tn, yy,
                          base[k].z + nrm[k].z * ld * tn);
        for (int k = 0; k < last; k++) {
            int k2 = (k + 1) % cnt;
            quad(ring0[k], ring0[k2], ring1[k2], ring1[k], col);
        }
        for (int k = 0; k < cnt; k++) ring0[k] = ring1[k];
    }
    {   uint16_t dark = s_is_snooker ? RGB565C(34, 30, 20) : RGB565C(3, 4, 4);
        float fy = s_is_snooker ? -0.105f : -0.055f;
        for (int k = 0; k < last; k++) {
            int k2 = (k + 1) % cnt;
            quad(ring0[k], ring0[k2],
                 v3(ring0[k2].x, fy, ring0[k2].z),
                 v3(ring0[k].x,  fy, ring0[k].z), dark);
        }
    }
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
            emit_lip_run(t, ring0, nrm, cnt, w->lip_d[p] * lscale, M, 0);
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

    /* ---- AND THE TWO STRAIGHT SIDES OF THE SLOT -------------------------
     *
     * The loop above walks the circle and draws the arc on the WOOD side of the
     * bore's centre. On every table shipped until now that was the whole of it,
     * because the centre sits on the CLOTH side of the plank's front face: the
     * arc inside the timber is LESS than a semicircle and its two ends land on
     * that face, so the hole closes itself.
     *
     * Push the centre past the face and the arc becomes MORE than a semicircle.
     * Its ends are now the circle's widest points, BEHIND the face, and beyond
     * them the circle curves back toward the cloth — so the arc stops in mid-air
     * and there is nothing between its ends and the front of the wood. You look
     * into the pocket and see straight out of the table. Reported on Paul, whose
     * 12.6 mm drop setback is a lot to ask of 28 mm of cushion depth, and which
     * is the first table here whose bore centre is behind the wood.
     *
     * A REAL POCKET IS CUT AS A SLOT, not as a circle: the round end at the back
     * and two straight sides running out to the front edge. So that is what is
     * emitted — one flat wall at each end of the arc, running STRAIGHT to the
     * face. Not a mirrored arc, which would close the hole into a circle and
     * leave a lip of wood standing in the mouth.
     *
     * Nothing at all where the centre is in front of the face, so every table
     * that was right stays identical to the bit. */
    if (axis == 0) {
        const float face = rail_hi ? z0 : z1;
        const float back = cz;                       /* the arc's ends sit here */
        if (rail_hi ? (back > face) : (back < face)) {
            for (int e = 0; e < 2; e++) {
                float ux = e ? cx + r : cx - r;
                if (ux < x0) ux = x0; if (ux > x1) ux = x1;
                quad(v3(ux,ytop,face), v3(ux,ytop,back),
                     v3(ux,ybot,back), v3(ux,ybot,face), wall);
            }
        }
    } else {
        const float face = rail_hi ? x0 : x1;
        const float back = cx;
        if (rail_hi ? (back > face) : (back < face)) {
            for (int e = 0; e < 2; e++) {
                float uz = e ? cz + r : cz - r;
                if (uz < z0) uz = z0; if (uz > z1) uz = z1;
                quad(v3(face,ytop,uz), v3(back,ytop,uz),
                     v3(back,ybot,uz), v3(face,ybot,uz), wall);
            }
        }
    }
}

/* A wood rail plank [xa,xb]×[za,zb] with a clean round bore at each pocket: cut a
 * rectangular notch (the pocket's clipped bounding box) from the plank top, then
 * fill it with bore_fill so the visible cut edge is the smooth circle curve. */
/* ---- S2: THE TIMBER ROUND A REGULAR BED ---------------------------------- *
 *
 * wood_plank_bored takes an axis-aligned rectangle, which is the whole of what
 * a rectangle and an L are made of. A hexagon's faces are diagonal, so its
 * timber is a RING rather than a set of planks, and it is built by sweeping.
 *
 * Along each edge, the wood is the band between two offsets of the bed — the
 * inner one at the cushion's back, the outer one at the frame's face — and the
 * pocket bores eat into it at both ends. So walk the edge, and at each step
 * take the cross-section from the inner ring to the outer one and ask which
 * part of it the bores have swallowed. What is left is at most two pieces: a
 * strip against the cushion and a strip against the outside. Emitting those two
 * between consecutive steps draws the whole ring, bore-edges included, with the
 * curve of each bore falling out of the arithmetic rather than being drawn.
 *
 * The bore edge is EXACT at every step — it is the true intersection of the
 * circle with that cross-section, not a cell kept or dropped — so it reads as a
 * circle rather than as a staircase. That is the same lesson as bar billiards'
 * cloth, applied to timber.
 *
 * The wall down each bore is emitted only where there is wood above it to hang
 * from: on the cloth side of a pocket there is no timber, and a full cylinder
 * would draw a rim standing in the mouth. */
/* A point on a turned skittle: radius `r` at height `y`, `a` radians round it,
 * with the pin leant by (ct, st) toward (fx, fz) about its own foot at (sx, sz).
 *
 * Upright (ct = 1, st = 0) this is the plain lathe: the axis is +Y and the
 * radius lies in the bed. Leaning tips the axis toward the fall direction and
 * carries the radius with it, so the pin stays a solid of revolution about a
 * tilted line rather than shearing. */
static Vec3 pin_at(float sx, float sz, float r, float y, float a,
                   float fx, float fz, float ct, float st) {
    /* the pin's own frame: axis U (tipped), and two radial directions */
    const float ux = fx * st,  uy = ct,   uz = fz * st;      /* the axis */
    const float px = fx * ct,  py = -st,  pz = fz * ct;      /* toward the fall */
    const float qx = -fz,      qz = fx;                      /* across it */
    const float ca = cosf(a), sa = sinf(a);
    return v3(sx + ux * y + (px * ca + qx * sa) * r,
                   uy * y + (py * ca            ) * r,
              sz + uz * y + (pz * ca + qz * sa) * r);
}

static void wood_ring_ngon(const CueTable *t, const CueWorld *w,
                           float rin, float rout, float ytop, float ybot,
                           uint16_t top, uint16_t wall,
                           const float *hx, const float *hz, const float *hr, int nh,
                           float ylow, uint16_t lip)
{
    const int n = cue_table_ngon_sides(t);
    /* STEPS BUNCHED AT THE ENDS, because that is the only place anything
     * happens. A bore reaches about its own radius along the edge — a hundred
     * millimetres of an edge that may be nearly a metre long — so spacing the
     * samples evenly spends them all in the middle where the band is a plain
     * rectangle, and gives the bore three of them. Three samples of a circle is
     * a notch, and that is exactly how it drew.
     *
     * A cosine spacing puts the first step a millimetre or two from the corner
     * and lets them stretch out along the straight, so the arc is smooth where
     * there is an arc and nothing is wasted where there is not. */
    const int MFINE = 48, MPLAIN = 1;
    for (int i = 0; i < n; i++) {
        const Vec3 A = cue_table_ngon_vert(t, i);
        const Vec3 B = cue_table_ngon_vert(t, (i + 1) % n);
        const float al = sqrtf(A.x*A.x + A.z*A.z), bl = sqrtf(B.x*B.x + B.z*B.z);
        if (al < 1e-5f || bl < 1e-5f) continue;
        const Vec3 Ai = v3(A.x/al*rin,  0, A.z/al*rin),  Bi = v3(B.x/bl*rin,  0, B.z/bl*rin);
        const Vec3 Ao = v3(A.x/al*rout, 0, A.z/al*rout), Bo = v3(B.x/bl*rout, 0, B.z/bl*rout);

        /* ONLY THE EDGES WITH A POCKET ON THEM NEED THE FINE STEPS. A round
         * bed is sixty edges of which six carry a bore; spending the same
         * forty-eight samples on the other fifty-four draws eleven thousand
         * triangles of plain rectangle. An edge no bore reaches IS a plain
         * rectangle, and one quad says so. */
        int M = MPLAIN;
        for (int h = 0; h < nh; h++) {
            const float ax2 = hx[h] - Ai.x, az2 = hz[h] - Ai.z;
            const float bx3 = hx[h] - Bi.x, bz3 = hz[h] - Bi.z;
            const float reach = hr[h] + (rout - rin);
            if (ax2*ax2 + az2*az2 < reach*reach ||
                bx3*bx3 + bz3*bz3 < reach*reach) { M = MFINE; break; }
        }
        /* THE BORE, CUT WHERE IT ACTUALLY MEETS THE PLANK.
         *
         * Sampling the run in even columns is what put a RANGE of angles round
         * every pocket. One column straddles the place where the bore starts
         * eating the plank's inner edge, so its inner side runs from a point
         * ON THE PLANK to a point ON THE CIRCLE — a chord that follows
         * neither. That happens at both ends of every bore on every edge, and
         * the boundary comes out as a fan of facets instead of plank, arc,
         * plank.
         *
         * It does not show up in a check of where the VERTICES sit, because
         * both ends of that chord are legitimately on one or the other. The
         * stray thing is the edge between them.
         *
         * So solve for where the circle crosses the inner edge — a quadratic
         * in the distance along it — and cut the run there. Between those
         * crossings the inner boundary is the arc and nothing else; outside
         * them it is the straight plank and nothing else. That is the shape a
         * plank gets from its notch and bore_fill, which is the point. */
        float cut[2*CUE_MAX_POCKET + 2]; int ncut = 0;
        cut[ncut++] = 0.0f; cut[ncut++] = 1.0f;
        {   const float ex = Bi.x - Ai.x, ez = Bi.z - Ai.z;
            const float aq = ex*ex + ez*ez;
            if (aq > 1e-12f) for (int h = 0; h < nh && ncut + 2 <= 2*CUE_MAX_POCKET + 2; h++) {
                const float fx = Ai.x - hx[h], fz = Ai.z - hz[h];
                const float bq = 2.0f*(fx*ex + fz*ez);
                const float cq = fx*fx + fz*fz - hr[h]*hr[h];
                const float disc = bq*bq - 4.0f*aq*cq;
                if (disc <= 0.0f) continue;
                const float sq = sqrtf(disc);
                const float u0 = (-bq - sq) / (2.0f*aq), u1 = (-bq + sq) / (2.0f*aq);
                if (u0 > 1e-5f && u0 < 1.0f - 1e-5f) cut[ncut++] = u0;
                if (u1 > 1e-5f && u1 < 1.0f - 1e-5f) cut[ncut++] = u1;
            }
        }
        for (int a2 = 1; a2 < ncut; a2++) {              /* insertion sort, tiny */
            const float v2 = cut[a2]; int b2 = a2 - 1;
            while (b2 >= 0 && cut[b2] > v2) { cut[b2+1] = cut[b2]; b2--; }
            cut[b2+1] = v2;
        }
        #define INN(u) v3(Ai.x + (Bi.x-Ai.x)*(u), ytop, Ai.z + (Bi.z-Ai.z)*(u))
        #define OUT(u) v3(Ao.x + (Bo.x-Ao.x)*(u), ytop, Ao.z + (Bo.z-Ao.z)*(u))
        for (int seg = 0; seg + 1 < ncut; seg++) {
            const float g0 = cut[seg], g1 = cut[seg+1];
            if (g1 - g0 < 1e-6f) continue;
            const float mid = 0.5f*(g0 + g1);
            const Vec3 M = INN(mid);
            int bored = 0;
            for (int h = 0; h < nh && !bored; h++) {
                const float dx2 = M.x-hx[h], dz2 = M.z-hz[h];
                if (dx2*dx2 + dz2*dz2 < hr[h]*hr[h]) bored = 1;
            }
            if (!bored) {                    /* PLAIN PLANK: one quad, one edge */
                quad(INN(g0), INN(g1), OUT(g1), OUT(g0), top);
                if (ylow < ytop) {
                    const Vec3 ia = INN(g0), ib = INN(g1);
                    quad(v3(ia.x, ytop, ia.z), v3(ib.x, ytop, ib.z),
                         v3(ib.x, ylow, ib.z), v3(ia.x, ylow, ia.z), lip);
                }
                continue;
            }
            /* THE NOTCH: the inner boundary is the arc, in fine columns. */
            const int NC = CUE_ARC_SEGS * 2;
            for (int k = 0; k < NC; k++) {
                const float sa = g0 + (g1-g0)*(float)k/(float)NC;
                const float sb = g0 + (g1-g0)*(float)(k+1)/(float)NC;
                const float ss[2] = { sa, sb };
                float tt[2] = { 0.0f, 0.0f }; int gone[2] = { 0, 0 };
                for (int q = 0; q < 2; q++) {
                    const Vec3 Pi = INN(ss[q]), Po = OUT(ss[q]);
                    const float dxs = Po.x - Pi.x, dzs = Po.z - Pi.z;
                    const float aq = dxs*dxs + dzs*dzs;
                    float best = 0.0f; int g2 = 0;
                    if (aq > 1e-12f) for (int h = 0; h < nh; h++) {
                        const float fx = Pi.x - hx[h], fz = Pi.z - hz[h];
                        const float bq = 2.0f*(fx*dxs + fz*dzs);
                        const float cq = fx*fx + fz*fz - hr[h]*hr[h];
                        const float disc = bq*bq - 4.0f*aq*cq;
                        if (disc <= 0.0f) continue;
                        const float sq = sqrtf(disc);
                        const float r0 = (-bq - sq) / (2.0f*aq);
                        const float r1 = (-bq + sq) / (2.0f*aq);
                        if (r1 <= 0.0f || r0 >= 1.0f) continue;
                        if (r1 >= 1.0f) { g2 = 1; break; }
                        if (r1 > best) best = r1;
                    }
                    tt[q] = best; gone[q] = g2;
                }
                if (gone[0] && gone[1]) continue;
                if (gone[0]) tt[0] = 1.0f;
                if (gone[1]) tt[1] = 1.0f;
                #define BAND(u, f) v3(INN(u).x + (OUT(u).x - INN(u).x)*(f), ytop, \
                                      INN(u).z + (OUT(u).z - INN(u).z)*(f))
                quad(BAND(sa, tt[0]), BAND(sb, tt[1]), BAND(sb, 1.0f), BAND(sa, 1.0f), top);
                #undef BAND
            }
        }
        #undef INN
        #undef OUT
    }

    /* THE POINT OF A SHARP MITRE, filled before the bore takes its share.
     *
     * A mitre corner sits cw/cos(pi/n) beyond the apothem, so it runs away as
     * the angle sharpens: 37.6mm past the pocket on a hexagon, 49.8 on a
     * square, 77.5 on a TRIANGLE. The bore is 52.6mm, so it reaches the corner
     * on every shape but the triangle, and on that one a wedge of nothing is
     * left inside the point — the spur that runs out past the drop.
     *
     * One fan does it: apex at the mitre corner, base on the bore's arc
     * between the two places the circle crosses the adjacent planks' inner
     * edges. That is the void exactly, and its inner boundary is the arc, so
     * the bore is still what stops the timber.
     *
     * Gated on the bore not reaching the corner, which is the condition
     * itself rather than a test for "is this a triangle" — a wide table with a
     * small pocket would want it at four sides and gets it. */
    /* Worked out per bore and KEPT, because the wall below has to know about it:
     * where the fan puts timber, the bore needs a face down it, and the band
     * test cannot see the fan. Zero half-angle means this bore has no fan. */
    float fan_a[CUE_MAX_POCKET], fan_h[CUE_MAX_POCKET];
    for (int h = 0; h < nh && h < CUE_MAX_POCKET; h++) { fan_a[h] = 0.0f; fan_h[h] = 0.0f; }
    for (int h = 0; h < nh && h < CUE_MAX_POCKET; h++) {
        int vi = -1; float vbest = 1e30f;
        for (int i = 0; i < n; i++) {          /* the vertex this bore belongs to */
            const Vec3 V = cue_table_ngon_vert(t, i);
            const float dx = V.x - hx[h], dz = V.z - hz[h];
            const float dd = dx*dx + dz*dz;
            if (dd < vbest) { vbest = dd; vi = i; }
        }
        if (vi < 0) continue;
        const Vec3 V = cue_table_ngon_vert(t, vi);
        const float vl = sqrtf(V.x*V.x + V.z*V.z);
        if (vl < 1e-6f) continue;
        const float mx = V.x / vl * rin, mz = V.z / vl * rin;      /* mitre corner */
        const float md = sqrtf((mx-hx[h])*(mx-hx[h]) + (mz-hz[h])*(mz-hz[h]));
        if (md <= hr[h]) continue;             /* the bore already reaches it */
        /* the arc between the two crossings, swept the short way */
        const float a0 = atan2f(mz - hz[h], mx - hx[h]);
        const float half = acosf(hr[h] / md > 1.0f ? 1.0f : hr[h] / md);
        fan_a[h] = a0; fan_h[h] = half;
        const int NA = CUE_ARC_SEGS;
        for (int k = 0; k < NA; k++) {
            /* THE ARC THE CORNER CAN SEE. The tangents from a point at
             * distance d touch the circle at a0 +/- acos(r/d), so the near
             * arc — the one facing the corner, and the one bounding the void
             * — is a0-half to a0+half. Sweeping a0+pi instead takes the FAR
             * arc and the fan covers the hole rather than the gap beside it,
             * which is what it did on the first run. */
            const float t0 = a0 - half + 2.0f*half*(float)k/(float)NA;
            const float t1 = a0 - half + 2.0f*half*(float)(k+1)/(float)NA;
            quad(v3(mx, ytop, mz),
                 v3(hx[h] + cosf(t0)*hr[h], ytop, hz[h] + sinf(t0)*hr[h]),
                 v3(hx[h] + cosf(t1)*hr[h], ytop, hz[h] + sinf(t1)*hr[h]),
                 v3(mx, ytop, mz), top);
            /* AND THE SAME AGAIN UNDERNEATH.
             *
             * A top face on its own is a lid over the void, not timber in it:
             * from below, and from inside the pocket, the corner was still
             * hollow. The piece is a solid wedge, so it has a bottom as well as
             * a top, and the wall down the bore's arc closes the third side —
             * see the band test below, which had to be taught about this fan
             * because the timber band cannot see it. */
            quad(v3(mx, ybot, mz),
                 v3(hx[h] + cosf(t1)*hr[h], ybot, hz[h] + sinf(t1)*hr[h]),
                 v3(hx[h] + cosf(t0)*hr[h], ybot, hz[h] + sinf(t0)*hr[h]),
                 v3(mx, ybot, mz), top);
        }
    }

    /* THE WALL DOWN EACH BORE, ONLY WHERE THERE IS TIMBER ABOVE IT TO HANG FROM.
     *
     * "Not over the cloth" is not the same question and getting them confused
     * cost the pockets their mouths: the bed polygon stops at the cushion NOSE,
     * so everything from the nose outward — the whole pocket opening included —
     * read as "not cloth", and a wall was drawn straight across every mouth.
     * The scalloped opening and the cloth rolling into it were behind it.
     *
     * The timber is the band between the two ring faces, so that is the test.
     * For a regular polygon a point's distance from the nearest FACE is its
     * projection onto the nearest face normal, and the normals sit at even
     * steps of 2*pi/n starting at +x — so the nearest one is found by rounding
     * rather than by searching. */
    const float ca_n = cosf(3.14159265f / (float)n);
    const float ap_in = rin * ca_n, ap_out = rout * ca_n;
    for (int h = 0; h < nh; h++) {
        const int NA = 28;
        for (int k = 0; k < NA; k++) {
            const float a0 = 6.2831853f * (float)k / NA;
            const float a1 = 6.2831853f * (float)(k + 1) / NA;
            const float m  = 0.5f * (a0 + a1);
            const float mx = hx[h] + hr[h]*cosf(m), mz = hz[h] + hr[h]*sinf(m);
            {   const float rr2 = sqrtf(mx*mx + mz*mz);
                if (rr2 < 1e-5f) continue;
                float th = atan2f(mz, mx);
                float step = 6.2831853f / (float)n;
                float phi = step * floorf(th / step + 0.5f);
                float proj = rr2 * cosf(th - phi);
                if (proj < ap_in || proj > ap_out) {
                    /* OR THE MITRE FAN, which is timber the band cannot see.
                     *
                     * Past a sharp corner the planks have run out and the void
                     * inside the point is filled by the fan above. Without this
                     * the bore had no face there at all: you looked into the
                     * pocket and saw straight through the back of it, which is
                     * the "roof over the issue" — a lid with nothing under it.
                     * With it the bore leaves a proper curved back. */
                    if (h >= CUE_MAX_POCKET || fan_h[h] <= 0.0f) continue;
                    float da = m - fan_a[h];
                    while (da >  3.14159265f) da -= 6.2831853f;
                    while (da < -3.14159265f) da += 6.2831853f;
                    if (fabsf(da) > fan_h[h]) continue;
                }
            }
            const float x0 = hx[h] + hr[h]*cosf(a0), z0 = hz[h] + hr[h]*sinf(a0);
            const float x1 = hx[h] + hr[h]*cosf(a1), z1 = hz[h] + hr[h]*sinf(a1);
            quad(v3(x0, ytop, z0), v3(x1, ytop, z1),
                 v3(x1, ybot, z1), v3(x0, ybot, z0), wall);
        }
    }
}

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
/* A LINE ACROSS THE CLOTH, AND NOT PAST IT.
 *
 * `half` is how far the caller wants it to reach, which for the baulk line is
 * the bed's half-width less half a ball. On a rectangle that IS the cloth's
 * width. On a polygon it is the apothem, and the bed is far narrower than that
 * at the baulk line — so the line ran out over the timber and off into the room,
 * which is what you can see on a hexagon or a triangle.
 *
 * So each end is walked back until it is actually on the cloth. Bisection rather
 * than arithmetic because the bed can be a rectangle, an L or any polygon and
 * cue_table_on_bed already knows about all of them; twelve steps puts the end
 * within a fifth of a millimetre of the edge, which is finer than the line is
 * wide.
 *
 * Only the drawn quads are affected. In VR the markings are painted in the cloth
 * shader — cue_render_set_markings(0) — where the geometry does the clipping and
 * this never arose. */
/* ON THE CLOTH, and for a polygon that is not what cue_table_on_bed answers.
 *
 * That one tests a set of RECTANGLES covering the bed, which is exact for a
 * rectangle and for an L and a generous bounding box for a polygon: a point out
 * past a hexagon's slanted edge is still inside the box, so the walk-back below
 * never fired on the sides where it was most needed. A polygon gets a real
 * point-in-polygon test off its own vertices. */
static int lay_on_cloth(const CueTable *t, float x, float z) {
    if (t->bed_shape != CUE_BED_NGON) return cue_table_on_bed(t, x, z);
    const int n = cue_table_ngon_sides(t);
    /* NO ASSUMPTION ABOUT THE WINDING. Testing for one sign meant that if the
     * bed happened to wind the other way EVERY interior point read as outside,
     * and the baulk line was clipped to nothing at all — which is a worse
     * failure than the overhang it was there to fix, and it looked like the
     * line had simply gone. So the test is that the signs AGREE, whichever way
     * round they come out. */
    int pos = 0, neg = 0;
    for (int i = 0; i < n; i++) {
        const Vec3 a = cue_table_ngon_vert(t, i);
        const Vec3 b = cue_table_ngon_vert(t, (i + 1) % n);
        const float cr = (b.x - a.x) * (z - a.z) - (b.z - a.z) * (x - a.x);
        if (cr > 1e-9f) pos++; else if (cr < -1e-9f) neg++;
    }
    return !(pos && neg);
}
static float lay_reach(const CueTable *t, float x, float want) {
    const float mag = want < 0.0f ? -want : want;
    if (mag <= 0.0f) return 0.0f;
    const float sgn = want < 0.0f ? -1.0f : 1.0f;
    Vec3 p = cue_table_lay(t, x, want, NULL);
    if (lay_on_cloth(t, p.x, p.z)) return mag;           /* already on it */
    float lo = 0.0f, hi = mag;
    for (int i = 0; i < 12; i++) {
        const float mid = (lo + hi) * 0.5f;
        p = cue_table_lay(t, x, sgn * mid, NULL);
        if (lay_on_cloth(t, p.x, p.z)) lo = mid; else hi = mid;
    }
    return lo;
}
static void lay_line(const CueTable *t, float x, float half, float lw, uint16_t c) {
    const float ha = lay_reach(t, x,  half);
    const float hb = lay_reach(t, x, -half);   /* the two ends can differ on an L */
    Vec3 a = cue_table_lay(t, x, -hb, NULL);
    Vec3 b = cue_table_lay(t, x,  ha, NULL);
    cloth_line(a.x, a.z, b.x, b.z, lw, c);
}
static void emit_table_markings(const CueTable *t) {
    uint16_t lc = shade565(t->cloth, 1.65f);     /* lighter cloth line */
    uint16_t sc = RGB565C(220, 220, 205);        /* spot — off-white */
    float hw = t->half_wid, hl = t->half_len, R = t->R;
    float lw = R * 0.22f, sr = R * 0.42f;
    if (t->is_snooker || t->kind == CUE_GAME_UK8 || t->house) {
        float bx = t->baulk_x, dr = t->d_radius;
        lay_line(t, bx, hw - R*0.5f, lw, lc);                       /* baulk line */
        if (t->house) goto no_arc;   /* a house is the line and nothing else */
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
    no_arc: ;
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
    /* BILLIARDS GOLF'S OBJECT BALLS ARE REDS, and this flag is what says ids
     * 1..15 are drawn as reds rather than as pool solids. It is NOT set on the
     * table, because `is_snooker` also decides the D, the four spots and where
     * a ball in hand may go — golf borrows the ball, not the game. */
    s_is_snooker = t->is_snooker || t->kind == CUE_GAME_GOLF;
    s_cloth_shadow = shade565(t->cloth, 0.42f);   /* cloth bounce tint */
    s_bg_top = RGB565C(24, 26, 36);
    s_bg_bot = RGB565C(6, 7, 12);
    const float hl = t->half_len, hw = t->half_wid;
    const float rw = t->rail_w;
    const float cw = rw * 0.63f;        /* cushion depth (nose → cushion back); +50% for a beefier rail */
    const float nose_h = t->cushion_h;       /* nose contact line (bottom of the front face) */
    const float flat_h = nose_h * 1.30f;     /* top of the small VERTICAL nose front face */
    const float rail_h = flat_h;             /* flat cushion top & wood top, level at flat_h */
    /* HOW FAR THE PLANK'S INNER FACE DROPS.
     *
     * It went down to rail_h, which is the cushion top — so the "face" was the
     * 2.16mm of frame_lift and nothing more, a step-closer rather than a face.
     * Nobody had ever seen an inner face on these tables because there was not
     * one to see, on a plank or on a ring.
     *
     * Dropped to the bed, the plank is a plain plank: a top, an outer skirt,
     * and a full face down its inside. That face is also what closes the
     * triangle a leaning cushion leaves under its nose at a pocket, which is
     * the gap the whole exercise started from. */
    const float face_low = 0.0f;
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
    if (t->kind == CUE_GAME_BARBILLIARDS) {
        /* ---- G6: A BED WITH HOLES THROUGH THE MIDDLE OF IT ---------------
         *
         * Every other table's cloth is one polygon fanned from the middle,
         * because every other table's holes are bites out of its EDGE. Bar
         * billiards has nine of them through the middle, and a fan cannot
         * express a hole — whatever way it is wound, the triangles cover the
         * one thing they are meant to leave out.
         *
         * The first answer was a fine grid with the cells that landed in a
         * hole dropped, and a flat ring drawn afterwards to hide where the
         * grid stopped. That is an approximation with no error bound: a cell
         * is kept or dropped whole, so the edge of the hole is a staircase
         * whose steps are as big as the cells, and the ring covered them on
         * some holes and not on others. Square corners of cloth stood out into
         * the bore. Reported, and correctly.
         *
         * THE HOLE EDGE IS A CIRCLE, SO DRAW A CIRCLE. Put the grid lines
         * where the holes are instead of on a fixed pitch, so that every hole
         * owns one cell outright; fill that cell with an ANNULUS running from
         * the bore's own radius out to the cell's four sides; fill every other
         * cell with a plain quad. The inner edge is then exactly the circle
         * the lip turns over at — not near it — and there is nothing left for
         * a covering ring to cover, so there is no covering ring.
         *
         * This is the general answer for a pocket that is INSIDE a table
         * rather than on its edge, and it holds for any arrangement of them:
         * the only thing it asks is that each hole gets a cell wider than its
         * own bore, which is checked below. */
        /* OUT TO THE TIMBER, not to the cushion nose.
         *
         * The grid this replaced ran to the SLATE EXTENT — under the cushions
         * and on to the frame — and rebuilding it round the holes I took the
         * nose instead, which stops the cloth dead at the cushion and leaves a
         * band of daylight all the way round the table. Every other bed reaches
         * the wood; so does this one. */
        float ex2, ez2; slate_extent(t, w, &ex2, &ez2);
        /* WHERE THE CLOTH STOPS is exactly where the lip's cloth is cut: one
         * roll outside the capture radius, so the two meet with no seam and
         * changing the roll moves both together. */
        const float roll = w->npocket ? (w->lip_d[0] > 0.0f ? w->lip_d[0] : 0.010f)
                                      : 0.010f;

        /* Each hole's patch reaches halfway to its nearest neighbour on that
         * axis, and no further than the bed. Halfway, so two patches meet on a
         * shared line and never overlap — an overlap would put one hole's
         * annulus across another's cell and draw cloth over a bore again. */
        float px0[CUE_MAX_POCKET], px1[CUE_MAX_POCKET];
        float pz0[CUE_MAX_POCKET], pz1[CUE_MAX_POCKET];
        for (int p = 0; p < w->npocket; p++) {
            float hx = ex2, hz = ez2;
            for (int q = 0; q < w->npocket; q++) {
                if (q == p) continue;
                float dxs = fabsf(w->pocket[q].x - w->pocket[p].x);
                float dzs = fabsf(w->pocket[q].z - w->pocket[p].z);
                if (dxs > 1e-5f && dxs * 0.5f < hx) hx = dxs * 0.5f;
                if (dzs > 1e-5f && dzs * 0.5f < hz) hz = dzs * 0.5f;
            }
            px0[p] = w->pocket[p].x - hx; px1[p] = w->pocket[p].x + hx;
            pz0[p] = w->pocket[p].z - hz; pz1[p] = w->pocket[p].z + hz;
            if (px0[p] < -ex2) px0[p] = -ex2;   if (px1[p] > ex2) px1[p] = ex2;
            if (pz0[p] < -ez2) pz0[p] = -ez2;   if (pz1[p] > ez2) pz1[p] = ez2;
        }

        /* The grid lines: the bed's edges, plus both sides of every patch,
         * sorted and with duplicates dropped. Every cell that comes out of
         * this is either exactly one hole's patch or contains no hole at all —
         * which is the whole trick, and it is why the cells are not square. */
        float xs[2 + 2 * CUE_MAX_POCKET], zs[2 + 2 * CUE_MAX_POCKET];
        int nxs = 0, nzs = 0;
        {   float raw_x[2 + 2 * CUE_MAX_POCKET], raw_z[2 + 2 * CUE_MAX_POCKET];
            int nrx = 0, nrz = 0;
            raw_x[nrx++] = -ex2; raw_x[nrx++] = ex2;
            raw_z[nrz++] = -ez2; raw_z[nrz++] = ez2;
            for (int p = 0; p < w->npocket; p++) {
                raw_x[nrx++] = px0[p]; raw_x[nrx++] = px1[p];
                raw_z[nrz++] = pz0[p]; raw_z[nrz++] = pz1[p];
            }
            /* insertion sort, unique — a few dozen values at most */
            for (int i = 0; i < nrx; i++) {
                float v = raw_x[i]; int dup = 0;
                for (int k = 0; k < nxs; k++) if (fabsf(xs[k] - v) < 1e-5f) dup = 1;
                if (dup) continue;
                int k = nxs++;
                while (k > 0 && xs[k-1] > v) { xs[k] = xs[k-1]; k--; }
                xs[k] = v;
            }
            for (int i = 0; i < nrz; i++) {
                float v = raw_z[i]; int dup = 0;
                for (int k = 0; k < nzs; k++) if (fabsf(zs[k] - v) < 1e-5f) dup = 1;
                if (dup) continue;
                int k = nzs++;
                while (k > 0 && zs[k-1] > v) { zs[k] = zs[k-1]; k--; }
                zs[k] = v;
            }
        }

        for (int iz = 0; iz + 1 < nzs; iz++) {
            for (int ix = 0; ix + 1 < nxs; ix++) {
                float x0 = xs[ix], x1 = xs[ix+1];
                float z0 = zs[iz], z1 = zs[iz+1];
                int hole = -1;
                for (int p = 0; p < w->npocket; p++) {
                    if (w->pocket[p].x <= x0 || w->pocket[p].x >= x1) continue;
                    if (w->pocket[p].z <= z0 || w->pocket[p].z >= z1) continue;
                    hole = p; break;
                }
                if (hole < 0) {
                    quad(v3(x0, 0, z0), v3(x1, 0, z0),
                         v3(x1, 0, z1), v3(x0, 0, z1), t->cloth);
                    continue;
                }
                /* THE ANNULUS. Inner ring on the circle the lip turns over at;
                 * outer ring where a ray from the hole's centre leaves this
                 * cell, which for a rectangle is whichever side it reaches
                 * first. The cell is not centred on the hole where the bed's
                 * edge has clipped it, and this does not care.
                 *
                 * THE CELL'S FOUR CORNERS ARE RAYS TOO. Twenty-four even
                 * angles alone leave a triangle of bare bed at each corner,
                 * because the straight edge between two outer points either
                 * side of a corner cuts the corner off — four black wedges per
                 * hole, which is what the first attempt at this drew. Adding
                 * the corner directions means every pair of neighbouring outer
                 * points lies on ONE side of the rectangle, so the quads
                 * between them tile it exactly. */
                {   float cx = w->pocket[hole].x, cz = w->pocket[hole].z;
                    float r0 = w->pocket_r[hole] + roll;
                    /* Never wider than the cell it has to live in, or the
                     * annulus would turn inside out and draw cloth over the
                     * neighbouring cells. */
                    float room = x1 - cx;
                    if (cx - x0 < room) room = cx - x0;
                    if (z1 - cz < room) room = z1 - cz;
                    if (cz - z0 < room) room = cz - z0;
                    if (r0 > room * 0.9f) r0 = room * 0.9f;

                    enum { NA = 24 };
                    float ang[NA + 4]; int na = 0;
                    for (int i = 0; i < NA; i++)
                        ang[na++] = 6.2831853f * (float)i / (float)NA;
                    ang[na++] = atan2f(z1 - cz, x1 - cx);
                    ang[na++] = atan2f(z1 - cz, x0 - cx);
                    ang[na++] = atan2f(z0 - cz, x0 - cx);
                    ang[na++] = atan2f(z0 - cz, x1 - cx);
                    for (int i = 0; i < na; i++) {          /* into [0, 2pi) */
                        while (ang[i] <  0.0f)       ang[i] += 6.2831853f;
                        while (ang[i] >= 6.2831853f) ang[i] -= 6.2831853f;
                    }
                    for (int i = 1; i < na; i++) {          /* insertion sort */
                        float v = ang[i]; int k = i;
                        while (k > 0 && ang[k-1] > v) { ang[k] = ang[k-1]; k--; }
                        ang[k] = v;
                    }
                    for (int i = 0; i < na; i++) {
                        float a0 = ang[i], a1 = ang[(i + 1) % na];
                        if (i + 1 == na) a1 += 6.2831853f;
                        if (a1 - a0 < 1e-5f) continue;      /* a duplicate ray */
                        float c0 = cosf(a0), s0 = sinf(a0);
                        float c1 = cosf(a1), s1 = sinf(a1);
                        float t0 = 1e9f, t1 = 1e9f;
                        if (c0 >  1e-6f) { float u = (x1 - cx) / c0; if (u < t0) t0 = u; }
                        if (c0 < -1e-6f) { float u = (x0 - cx) / c0; if (u < t0) t0 = u; }
                        if (s0 >  1e-6f) { float u = (z1 - cz) / s0; if (u < t0) t0 = u; }
                        if (s0 < -1e-6f) { float u = (z0 - cz) / s0; if (u < t0) t0 = u; }
                        if (c1 >  1e-6f) { float u = (x1 - cx) / c1; if (u < t1) t1 = u; }
                        if (c1 < -1e-6f) { float u = (x0 - cx) / c1; if (u < t1) t1 = u; }
                        if (s1 >  1e-6f) { float u = (z1 - cz) / s1; if (u < t1) t1 = u; }
                        if (s1 < -1e-6f) { float u = (z0 - cz) / s1; if (u < t1) t1 = u; }
                        quad(v3(cx + r0*c0, 0, cz + r0*s0),
                             v3(cx + r0*c1, 0, cz + r0*s1),
                             v3(cx + t1*c1, 0, cz + t1*s1),
                             v3(cx + t0*c0, 0, cz + t0*s0), t->cloth);
                    }
                }
            }
        }
    } else {
    for (int i = 0; i < s_bnd.n; i++) {
        Vec3 a = s_bnd.p[i], b = s_bnd.p[(i + 1) % s_bnd.n];
        tri(v3(0, 0, 0), a, b, t->cloth);
    }
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
/* ---- CUE_CUSHDUMP: THE CUSHION CHAIN AS DRAWN, IN PLAN -------------------
 *
 * The nose line the balls bounce off is cue_table's, and the nose line you see
 * is this file's, and the whole contract between them is that those are the
 * same line. Nothing checked it. A drawn-only softening of the mitred knuckles
 * moved the visible nose 3.4 mm up the rail from the collision one and it took
 * a headset and a player to notice.
 *
 * So: set CUE_CUSHDUMP and every drawn nose and back vertex is printed in table
 * coordinates, ready to be laid over the same table's CueSeg chain. Same spirit
 * as CUE_BNDDUMP below, and the same rule — host only, because naming stdio at
 * all drags in a file layer the device has no syscalls for. */
#ifdef MOTE_HOST
    { const char *e = getenv("CUE_CUSHDUMP");
      if (e) printf("DIMS kind %d hl %.6f hw %.6f rw %.6f cw %.6f R %.6f\n",
                    (int)t->kind, hl, hw, rw, cw, t->R); }
#endif
    /* HOW FAR PAST THE BORE'S EDGE a folded cushion vertex has to land before
     * the wood in front of it hides it. Three millimetres is a hair on a table
     * and takes the end face out of sight from every angle tried. */
    #define CUE_BORE_HIDE 0.003f
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
        /* A MITRED CORNER: two straight noses meeting with no pocket between
         * them. Only bar billiards has one today — every other table puts a
         * pocket in every corner — and the perpendicular-back rule below left
         * an uncovered cw-by-cw square of void behind each meeting, which is
         * the black square in the corner. A mitred back instead runs along
         * the AVERAGED normal, scaled out so it lands on the wood's inner
         * corner. The neighbour test wraps around the chain, because a closed
         * loop's first and last segments share a corner too. */
        int mitreA = 0, mitreB = 0;
        float mscaleA = 1.0f, mscaleB = 1.0f;
        if (w->nseg > 1) {
            const CueSeg *pr = &w->seg[(s + w->nseg - 1) % w->nseg];
            if (v3_len2(v3_sub(pr->b, sg->a)) < 1e-8f) {
                na = v3_norm(v3_add(sg->n, pr->n)); sharedA = 1;
                if (sg->kind == 0 && pr->kind == 0) {
                    mitreA = 1;
                    float c = na.x*sg->n.x + na.z*sg->n.z;
                    mscaleA = 1.0f / (c > 0.35f ? c : 0.35f);
                }
            }
            const CueSeg *nx = &w->seg[(s + 1) % w->nseg];
            if (v3_len2(v3_sub(sg->b, nx->a)) < 1e-8f) {
                nb = v3_norm(v3_add(sg->n, nx->n)); sharedB = 1;
                if (sg->kind == 0 && nx->kind == 0) {
                    mitreB = 1;
                    float c = nb.x*sg->n.x + nb.z*sg->n.z;
                    mscaleB = 1.0f / (c > 0.35f ? c : 0.35f);
                }
            }
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
            /* IN THE RAIL'S OWN FRAME, not in x or z.
             *
             * This picked whichever of x and z the rail's normal leaned towards
             * and then solved in that ONE coordinate. Every rail on a rectangle
             * runs along an axis, so that was exact and nobody noticed. A
             * polygon's rails run at whatever angle the shape gives them, and
             * then "the dominant axis" is not the rail at all: the target came
             * out meaningless and the facing was extended along its tangent to
             * wherever that landed — straight off past the pocket and out
             * through the timber, which is the sliver of cloth you can see
             * poking out at a hexagon's and an octagon's corners.
             *
             * The frame's inner face is a LINE: the nose pushed out by one
             * cushion depth. Meeting it is one dot product and one divide, with
             * no axis in it, and it gives the identical answer on a rectangle. */
            const Vec3 outw = v3(-nose_n.x, 0.0f, -nose_n.z);   /* to the timber */
            const float md = M.x*outw.x + M.z*outw.z;
            if (fabsf(md) > 1e-4f) {
                Vec3 bp = v3(tp.x - nn.x*cw, 0, tp.z - nn.z*cw);
                float tn2 = (cw - ((tp.x-kn.x)*outw.x + (tp.z-kn.z)*outw.z)) / md;
                float tb  = (cw - ((bp.x-kn.x)*outw.x + (bp.z-kn.z)*outw.z)) / md;
                /* NEVER ACROSS A POCKET.
                 *
                 * This extension exists to run a free facing out until it meets
                 * the woodwork, and it is measured along the facing's own
                 * tangent with no notion of the hole in between. On a jaw whose
                 * tip already finishes AT the frame — which is what the yellow
                 * point is — it had nothing left to close, and instead drove the
                 * tip 64.6 mm further on: out of the bore on the far side, with
                 * the strip sweeping straight over the hole to get there. That
                 * is the cushion visible inside the pocket, coming back out
                 * further in.
                 *
                 * Bounding the ray stops it at the bore's edge. A tip already on
                 * that edge and heading inward gets a limit of zero, so it stops
                 * exactly on the yellow point and the geometry ends there. */
                {   const float lim = cue_table_ray_bore_limit(w, tp.x, tp.z,
                                                               M.x, M.z, tn2);
                    if (lim < tn2) tn2 = lim;
                }
                {   const float lim = cue_table_ray_bore_limit(w, bp.x, bp.z,
                                                               M.x, M.z, tb);
                    if (lim < tb) tb = lim;
                }
                if (tn2 > 0.0f) { Vec3 e = v3_add(tp, v3_scale(M, tn2));
                                  if (afree) pa = e; else pb = e; }
                if (tb > 0.0f) { Vec3 e = v3_add(bp, v3_scale(M, tb));
                                 if (afree) { fba = e; haveFba = 1; }
                                 else       { fbb = e; haveFbb = 1; } }
            }
        }
        /* NOTHING PAST THE YELLOW POINT, which is where the jaw is built to
         * finish. The extension above runs a free tip along its own tangent
         * until it reaches the wood, and its target is worked out from `kn` —
         * the previous vertex — on the assumption that `kn` lies on the
         * STRAIGHT nose line. On a rounded jaw it does not: it is a point part
         * way round the curve, so the target lands somewhere else and the tip is
         * driven PAST the frame and into the bore. That is the sliver of cloth
         * you can see inside the pocket.
         *
         * The tip is already at the wood by construction now, so the extension
         * has nothing left to do there — but rather than unpick which tables
         * still need it, the result is simply clamped out of the hole. Every
         * pocket, every shape, and the same helper the collision world uses. */
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
        /* WHERE A NOSE MEETS A FACING, THE NOSE'S LINE WINS.
         *
         * A straight rail keeps a perpendicular back at depth cw — a straight
         * edge the wood's inner face butts against — and that is deliberate
         * (see above). A facing takes the AVERAGED normal so its top runs
         * continuously into its neighbour. At the joint between the two, those
         * are different lines, and the two ends of one joint landed in
         * different places: measured across every table, 1.7 mm on the 7 ft,
         * 1.9 mm on the snooker and THIRTY MILLIMETRES on the US 9 ft, whose
         * facings leave the nose at the steepest angle. That is the sliver of
         * daylight at the back of every pocket mouth.
         *
         * The nose cannot move without lifting the cushion off the timber, so
         * the facing adopts the nose's back at the end they share. Both ends
         * land on one point, the strip closes, and the straight edge the wood
         * needs is untouched. */
        Vec3 bka = (sg->kind == 0 && !mitreA) ? sg->n : na;
        Vec3 bkb = (sg->kind == 0 && !mitreB) ? sg->n : nb;
        if (w->nseg > 1) {
            const CueSeg *pr = &w->seg[(s + w->nseg - 1) % w->nseg];
            const CueSeg *nx = &w->seg[(s + 1) % w->nseg];
            if (sharedA && sg->kind == 1 && pr->kind == 0) bka = pr->n;
            if (sharedB && sg->kind == 1 && nx->kind == 0) bkb = nx->n;
        }
        Vec3 ar = haveFba ? v3(fba.x, rail_h, fba.z)
                          : v3(pa.x - bka.x*cwa*mscaleA, rail_h,
                               pa.z - bka.z*cwa*mscaleA);
        Vec3 br = haveFbb ? v3(fbb.x, rail_h, fbb.z)
                          : v3(pb.x - bkb.x*cwb*mscaleB, rail_h,
                               pb.z - bkb.z*cwb*mscaleB);
        /* THE JAW CURVE IS NEVER TOUCHED — only what comes after it.
         *
         * This folded all eight vertices of every segment along the cushion's
         * outward normal, which included the nose, and included the interior
         * nodes of the bezier. The last vertices of a jaw sit ON the bore's rim
         * by construction, so they were inside "the rim plus a 3 mm margin" and
         * got shoved sideways into the timber. That is the kink in the jaw: the
         * visible curve was being bent to solve a problem that belongs to the
         * cushion's back, past the end of it.
         *
         * The curve runs from the rail to the yellow point and that is the
         * whole of it — every node, nose and back alike, stays exactly where
         * cue_table put it. What can stray into the hole is the geometry AFTER
         * the yellow point: the free tip's back, which is pushed out from the
         * nose by the cushion's depth and so leans across the bore. Only those
         * run straight back along the cushion's own outward normal until they
         * are clear of it — a straight line away from the hole into the wood,
         * which is where the wood hides them. Capped at the cushion depth so it
         * can never drag a vertex past the cushion's own back. */
        if (!sharedA || !sharedB) {
            const Vec3 un = v3(-sg->n.x, 0.0f, -sg->n.z);   /* into the timber */
            if (!sharedA) cue_table_hide_bore(w, &ar.x, &ar.z, un.x, un.z,
                                              CUE_BORE_HIDE, cw);
            if (!sharedB) cue_table_hide_bore(w, &br.x, &br.z, un.x, un.z,
                                              CUE_BORE_HIDE, cw);
        }
#ifdef MOTE_HOST
        { const char *e = getenv("CUE_CUSHDUMP");
          if (e) printf("CUSH %3d kind%d sA%d sB%d fA%d fB%d an %.6f %.6f bn %.6f %.6f"
                        " ar %.6f %.6f br %.6f %.6f\n",
                        s, sg->kind, sharedA, sharedB, haveFba, haveFbb,
                        an.x, an.z, bn.x, bn.z, ar.x, ar.z, br.x, br.z); }
#endif
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

    /* ---- G6: THE NINE HOLES, THE THREE SKITTLES, AND FOUR CORNERS -------
     *
     * Bar billiards' scoring is bored through the middle of the bed, so there
     * is nothing on the boundary to scallop and nothing for the pocket-void
     * cones to hang off. Each hole gets what a real one has and a ring drawn on
     * the cloth does not: a LIP where the cloth rolls over the edge, a shaft
     * you can see down, and a floor a long way below it. The lip is what makes
     * it read as a hole rather than as a circle painted on a flat table — the
     * eye reads the shading round the roll, not the outline.
     *
     * Drawn here, with the rest of the table, so the host uploads them with
     * everything else and does not need to know this game exists. */
    if (t->kind == CUE_GAME_BARBILLIARDS) {
        const int NSEG = 24;
        const uint8_t keep_mat = s_mat;
        s_mat = CUE_MAT_CLOTH;                 /* the lip is cloth, not timber */
        /* THE SAME LIP THE RAIL POCKETS GET, swept the whole way round.
         *
         * A hole in the bed is a pocket whose cut happens to be a complete
         * circle, so it wants the cloth rolling over its edge exactly as a
         * scalloped one does: the same quarter-cosine fall, the same darkening
         * as it turns under, the same wall dropped from the rolled edge. Hand-
         * rolling a second version of it produced something that looked like
         * neither, which is what a second implementation of a thing usually
         * does.
         *
         * The cut sits one roll OUTSIDE the capture radius, so that once the
         * cloth has turned under, the throat lands on the radius the ball is
         * actually taken at. */
        for (int p = 0; p < w->npocket; p++) {
            const float ld = w->lip_d[p] > 0.0f ? w->lip_d[p] : 0.010f;
            const float rc = w->pocket_r[p] + ld;    /* where the cloth is cut */
            Vec3 ring[CUE_LIP_MAX], nrm[CUE_LIP_MAX];
            int cnt = NSEG < CUE_LIP_MAX ? NSEG : CUE_LIP_MAX;
            for (int k = 0; k < cnt; k++) {
                float a0 = 6.2831853f * (float)k / (float)cnt;
                float cx = cosf(a0), sz = sinf(a0);
                ring[k] = v3(w->pocket[p].x + rc * cx, 0.0f,
                             w->pocket[p].z + rc * sz);
                /* "Outward" from the cloth is INWARD to the hole: that is the
                 * direction the cloth disappears in. */
                nrm[k] = v3(-cx, 0.0f, -sz);
            }
            emit_lip_run(t, ring, nrm, cnt, ld, 8, 1);
        }
        /* ---- AND THE FOUR CORNERS OF THE CUSHION -----------------------
         *
         * This is the first table here whose cushions turn a corner. Every
         * other one has a POCKET at each corner, so two runs never meet: the
         * facings run out into the timber either side of a hole and the
         * question does not arise. Here they do meet, at ninety degrees, and
         * the corner-smoothing deliberately keeps a sharp join crisp — so each
         * run ends flush at the corner point and the square of cushion behind
         * it, between the two backs and the wood, was simply not there.
         *
         * It is filled with the piece that belongs there: the top at rail
         * height over that square, and the two inner faces that carry the
         * cushion's own profile round the turn. */
        {   const float bx = hl + cw, bz = hw + cw;
            for (int c = 0; c < 4; c++) {
                float sx = (c & 1) ? 1.0f : -1.0f;
                float sz = (c & 2) ? 1.0f : -1.0f;
                Vec3 N = v3(sx * hl, 0.0f, sz * hw);          /* the nose corner */
                Vec3 B1 = v3(sx * hl, 0.0f, sz * bz);         /* back of the z rail */
                Vec3 B2 = v3(sx * bx, 0.0f, sz * hw);         /* back of the x rail */
                Vec3 W  = v3(sx * bx, 0.0f, sz * bz);         /* the wood corner */
                /* the cloth top, level with the rest of the cushion top */
                quad(v3(N.x, rail_h, N.z), v3(B1.x, rail_h, B1.z),
                     v3(W.x,  rail_h, W.z), v3(B2.x, rail_h, B2.z), ctop);
                /* the two faces that look back at the table, carrying the
                 * cushion's undercut and its little vertical flat round the
                 * corner so the profile does not change at the join */
                ribbon(v3(N.x, 0.0f, N.z),      v3(B1.x, 0.0f, B1.z),
                       v3(B1.x, nose_h, B1.z),  v3(N.x, nose_h, N.z), fdark);
                quad(v3(N.x, nose_h, N.z),      v3(B1.x, nose_h, B1.z),
                     v3(B1.x, flat_h, B1.z),    v3(N.x, flat_h, N.z), face);
                ribbon(v3(N.x, 0.0f, N.z),      v3(B2.x, 0.0f, B2.z),
                       v3(B2.x, nose_h, B2.z),  v3(N.x, nose_h, N.z), fdark);
                quad(v3(N.x, nose_h, N.z),      v3(B2.x, nose_h, B2.z),
                     v3(B2.x, flat_h, B2.z),    v3(N.x, flat_h, N.z), face);
            } }

        /* THE SKITTLES, AS MUSHROOMS THAT FALL OVER.
         *
         * Rule 74 gives the shape and the constraint together: cylindrical to
         * at least 51 mm above the base, 15 to 18 mm across, 114 mm tall. A
         * ball is 47.6 mm across, so its highest point is below 51 mm and it
         * ALWAYS strikes the plain stem — which is why one radius is the whole
         * collider, and why the flare above can be whatever a skittle looks
         * like without changing how the game plays.
         *
         * Turned as a profile rather than built as a cylinder: a lathe of
         * (height, radius) pairs, swept round. The bulb is what makes it read
         * as a skittle across a room instead of as a peg.
         *
         * AND IT LIES DOWN WHEN IT GOES. The physics topples it about its foot
         * — theta from upright, in a direction the ball chose — so the whole
         * profile is rotated by that angle about the base point rather than
         * being swapped for a lying model. At rest upright the rotation is the
         * identity and this draws exactly what it always drew. */
        for (int k = 0; s_skittles && k < w->nskittle; k++) {
            const float sr = w->skittle_r;
            const uint16_t body = w->skittle_black[k] ? RGB565C(24, 22, 26)
                                                      : RGB565C(238, 234, 222);
            const uint16_t top  = shade565(body, 1.25f);

            /* the shape lives at file scope now — see CUE_SKITTLE_PROF —
             * because CueVR turns its own mesh from it. */
            const float (*PROF)[2]; const int NP = cue_render_skittle_profile(&PROF);
            (void)0;
            /* A bar billiards skittle is a MUSHROOM: a wide shallow cap that
             * overhangs, on a slim turned stem with a flared foot. What was
             * here was a spear — a gentle swell tapering to a point, which read
             * as asparagus rather than as a skittle. The cap is the thing you
             * recognise it by, and it has to be much wider than the stem and
             * domed rather than pointed.
             *
             * Rule 74 still governs the only part that matters to the game:
             * cylindrical, 15 to 18 mm across, up to at least 51 mm. Everything
             * above that is above every ball, so the cap can be as wide as a
             * skittle's cap really is without changing a single shot. */

            /* WHEREVER THE BODY HAS PUT IT. The pin is a rigid body now, so
             * its place and its attitude come off the body rather than out of
             * an angle: the profile is turned in body space and carried into
             * the world by the body's own orientation, which is the same
             * transform whether it is standing, tumbling or lying still. */
            const MoteBody *sb = &w->sk[k];
            const Vec3 bo = sb->pos;
            const Vec3 bx = sb->orient.r[0], by = sb->orient.r[1], bz = sb->orient.r[2];
            const float half_len = w->skittle_len * 0.5f;
            #define PIN(rr, yy, aa) v3( \
                bo.x + bx.x*((rr)*sr*cosf(aa)) + by.x*((yy)-half_len) + bz.x*((rr)*sr*sinf(aa)), \
                bo.y + bx.y*((rr)*sr*cosf(aa)) + by.y*((yy)-half_len) + bz.y*((rr)*sr*sinf(aa)), \
                bo.z + bx.z*((rr)*sr*cosf(aa)) + by.z*((yy)-half_len) + bz.z*((rr)*sr*sinf(aa)))

            /* THE FOOT IS CLOSED. The profile starts at the rim of the flare,
             * so the turned surface alone leaves the underside open — invisible
             * while the pin stands and glaring the moment it is knocked over. */
            for (int i = 0; i < NSEG; i++) {
                const float a0 = 6.2831853f * (float)i / NSEG;
                const float a1 = 6.2831853f * (float)(i + 1) / NSEG;
                tri(PIN(PROF[0][1], PROF[0][0], a0),
                    PIN(PROF[0][1], PROF[0][0], a1),
                    PIN(0.0f,       PROF[0][0], a0), shade565(body, 0.62f));
            }
            for (int i = 0; i < NSEG; i++) {
                const float a0 = 6.2831853f * (float)i / NSEG;
                const float a1 = 6.2831853f * (float)(i + 1) / NSEG;
                for (int p = 0; p + 1 < NP; p++) {
                    const float r0 = PROF[p][1],   y0 = PROF[p][0];
                    const float r1 = PROF[p+1][1], y1 = PROF[p+1][0];
                    const uint16_t col = shade565(body, 0.86f + 0.28f * (y0 / 0.114f));
                    if (r1 <= 0.0001f) {                    /* the tip: a cone */
                        tri(PIN(r0, y0, a0), PIN(r0, y0, a1), PIN(0.0f, y1, a0), top);
                    } else {
                        quad(PIN(r0, y0, a0), PIN(r0, y0, a1),
                             PIN(r1, y1, a1), PIN(r1, y1, a0), col);
                    }
                }
            }
            #undef PIN
        }
        s_mat = keep_mat;
    }

    if (t->bed_shape == CUE_BED_NGON) {
        /* A RING AND A MATCHING SKIRT. The offsets are the bed's own polygon
         * pushed out along its faces — which for a regular one is the same
         * polygon at a larger radius, apothem plus the offset, so both rings
         * come off a single scale of the circumradius. */
        const int nn = cue_table_ngon_sides(t);
        const float ca = cosf(3.14159265f / (float)nn);
        const float rin  = t->half_len + cw / ca;    /* the cushion's back */
        const float rout = t->half_len + fw / ca;    /* the frame's outer face */
        wood_ring_ngon(t, w, rin, rout, plank_y, bore_bot, woodt, wbore,
                       hx, hz, hr, nh, face_low, wlip);
        for (int i = 0; i < nn; i++) {               /* the skirt, one face each */
            Vec3 A = cue_table_ngon_vert(t, i);
            Vec3 B = cue_table_ngon_vert(t, (i + 1) % nn);
            float al = sqrtf(A.x*A.x + A.z*A.z), bl = sqrtf(B.x*B.x + B.z*B.z);
            if (al < 1e-5f || bl < 1e-5f) continue;
            Vec3 P = v3(A.x/al*rout, 0, A.z/al*rout);
            Vec3 Q = v3(B.x/bl*rout, 0, B.z/bl*rout);
            quad(v3(Q.x, plank_y, Q.z), v3(P.x, plank_y, P.z),
                 v3(P.x, 0.0f,    P.z), v3(Q.x, 0.0f,    Q.z), wood);
        }
    } else if (t->bed_shape == CUE_BED_L) {
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
        /* AND THE MOUTH SIDE MIRRORS WITH THEM.
         *
         * `rail_hi` says which edge of a plank is its INNER one — the edge the
         * cushion butts against and the bore breaks out of. The z bounds above
         * are mirrored for a left-handed table and this flag was not, so on
         * every rail that runs across the table the timber was built with its
         * mouth on the outside: the wood closed over the cushion and left a
         * square of void beside the pocket where the jaw should be. That is
         * the reported mess on the mirrored L, and it is only ever the rails
         * whose bounds moved — an x-plank's mouth is an x edge and x does not
         * mirror. */
        #define RHI(v) ((h < 0.0f) ? !(v) : (v))
        const float nxi = (hl - nx) + cw, nxo = (hl - nx) + fw;  /* notch's inner rail */
        const float nzi = (hw - nz) + cw, nzo = (hw - nz) + fw;  /* notch's underside */
        wood_plank_bored(-ox,  ox, ZLO(-oz,-ibz), ZHI(-oz,-ibz), plank_y, bore_bot, woodt, wbore, hx, hz, hr, nh, 0, RHI(0), face_low, wlip); /* bottom */
        wood_plank_bored( ibx, ox, ZLO(-oz, nzo), ZHI(-oz, nzo), plank_y, bore_bot, woodt, wbore, hx, hz, hr, nh, 1, 1, face_low, wlip); /* right, up to the notch */
        wood_plank_bored( nxi, ox, ZLO(nzi, nzo), ZHI(nzi, nzo), plank_y, bore_bot, woodt, wbore, hx, hz, hr, nh, 0, RHI(1), face_low, wlip); /* under the notch */
        wood_plank_bored( nxi, nxo, ZLO(nzi, oz), ZHI(nzi, oz),  plank_y, bore_bot, woodt, wbore, hx, hz, hr, nh, 1, 1, face_low, wlip); /* beside the notch */
        wood_plank_bored(-ox,  nxo, ZLO(ibz, oz), ZHI(ibz, oz),  plank_y, bore_bot, woodt, wbore, hx, hz, hr, nh, 0, RHI(1), face_low, wlip); /* top, short leg */
        wood_plank_bored(-ox, -ibx, ZLO(-oz, oz), ZHI(-oz, oz),  plank_y, bore_bot, woodt, wbore, hx, hz, hr, nh, 1, 0, face_low, wlip); /* left */
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
        #undef RHI
    } else {
    wood_plank_bored(-ox, ox,  ibz,  oz,  plank_y, bore_bot, woodt, wbore, hx, hz, hr, nh, 0, 1, face_low, wlip); /* +z */
    wood_plank_bored(-ox, ox, -oz, -ibz,  plank_y, bore_bot, woodt, wbore, hx, hz, hr, nh, 0, 0, face_low, wlip); /* -z */
    wood_plank_bored(ibx, ox, -ibz, ibz,  plank_y, bore_bot, woodt, wbore, hx, hz, hr, nh, 1, 1, face_low, wlip); /* +x */
    wood_plank_bored(-ox,-ibx,-ibz, ibz,  plank_y, bore_bot, woodt, wbore, hx, hz, hr, nh, 1, 0, face_low, wlip); /* -x */
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

/* Bar billiards: seven plain whites and one red, and no marking on any of
 * them. The "cue ball" is whichever white you are striking — they are
 * interchangeable, exactly as the ivories are in pyramid — so the set gives
 * ball 1 (the red) its own colour and leaves everything else white. */
static const uint16_t k_bbhue[8] = {
    0, RGB565C(196, 42, 34), RGB565C(238,238,232), RGB565C(238,238,232),
       RGB565C(238,238,232), RGB565C(238,238,232), RGB565C(238,238,232),
       RGB565C(238,238,232),
};

static const CueBallSet k_ballsets[10] = {
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
  /* 9 */ { "BAR BILLIARDS", k_bbhue, 0, RGB565C(238,238,232),
            0, RGB565C(238,238,232), 0,
            /* No coloured cue ball: every white on the table is one. */
            0,
            0, 0, 0, 0.42f },
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
    /* AGAINST THE ARRAY, not a literal — the third place in this file to have
     * carried a hand-written count of the ball sets, and the third to fall
     * behind when one was added. The setter said 7 when there were 8; the
     * count said 9 when there were 10; and this said 8, so a tenth set was
     * SELECTED, was NAMED correctly by the menu, and still drew as the first
     * one. There is now one number and the array owns it. */
    const int n = (int)(sizeof k_ballsets / sizeof k_ballsets[0]);
    int i = (s_ball_set < 0 || s_ball_set >= n) ? 0 : s_ball_set;
    return &k_ballsets[i];
}
int cue_render_ballset_count(void);   /* below; used by the setter's clamp */

/* WHICH SET IS SELECTED, for a host that CACHES the ball surface. CueVR bakes
 * ball_sample() into an atlas once per table, so it has to know when the answer
 * has changed under it — and "the app told mote, so the app knows" is not true
 * when the app is also told by a menu, a preference load and a game kind. */
int cue_render_ball_set(void) { return s_cust_on ? -1 : s_ball_set; }

/* FROM THE TABLE, not from a literal — which is the whole point of the clamp
 * that reads it. This said 9, and the setter's own comment two hundred lines
 * up explains that it used to say 7 and stayed 7 when an eighth set arrived,
 * so the new set silently gave you the first one. A tenth set arrived and it
 * said 9. The array knows how long it is; nothing else should claim to. */
int         cue_render_ballset_count(void) {
    return (int)(sizeof k_ballsets / sizeof k_ballsets[0]);
}
const char *cue_render_ballset_name(int i) {
    return (i >= 0 && i < cue_render_ballset_count()) ? k_ballsets[i].name : "";
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
