/*
 * CueVR — a table to stand the slate on.
 *
 * One design so far, "Regency": the shape a match table actually is. A deep
 * apron running the perimeter with a moulded top edge and a bead line picked out
 * along it, a cabinet of beams under the slate, and heavy legs — square,
 * chamfered to an octagon, tapered, with a turned capital and a plinth foot.
 *
 * Everything is proportional to the table it is given, because it has to serve
 * a 7 ft pub table and a 12 ft match table from the same code. The numbers are
 * expressed as fractions of the table's own dimensions or in absolute metres
 * where a real table would be absolute — an apron is about the same depth on
 * every table, but the leg count is not: a 7 ft table stands on four and a full
 * size snooker table needs eight, or the slate sags between them. That is a
 * real constraint and it is why the legs are laid out on a rule rather than
 * placed by hand.
 *
 * No GL here. See cuevr_frame.h.
 */
#include "cuevr_frame.h"

#include <math.h>
#include <string.h>

/* ---- timbers ------------------------------------------------------------ *
 * Two woods and a brass, so the frame reads as made rather than moulded: a
 * warm mahogany for the apron where the light falls on it, something darker
 * for the legs so they recede, and a little metal at the feet. */
static const float MAHOGANY[3] = { 0.286f, 0.129f, 0.086f };
static const float MAHOGANY_LIT[3] = { 0.353f, 0.169f, 0.106f };
static const float EBONISED[3] = { 0.086f, 0.055f, 0.047f };
static const float BRASS[3] = { 0.451f, 0.333f, 0.145f };
/* The top face of the frame is what you see when you look down a pocket, so
 * it is black: a pocket has no floor to show you, and anything lighter reads
 * as the hole having been filled in with wood — which is what it looked like. */
static const float SHADOW[3] = { 0.012f, 0.010f, 0.010f };

/* ---- emit --------------------------------------------------------------- */

static int vtx(CueVrFrameMesh *m, float x, float y, float z,
               float nx, float ny, float nz, float u, float v, const float *col) {
    if (m->nv >= m->cap_v) { m->overflow = 1; return m->nv ? m->nv - 1 : 0; }
    CueVrFrameVtx *o = &m->v[m->nv];
    o->p[0]=x; o->p[1]=y; o->p[2]=z;
    o->n[0]=nx; o->n[1]=ny; o->n[2]=nz;
    o->uv[0]=u; o->uv[1]=v;
    o->c[0]=col[0]; o->c[1]=col[1]; o->c[2]=col[2];
    return m->nv++;
}

static void tri(CueVrFrameMesh *m, int a, int b, int c) {
    if (m->ni + 3 > m->cap_i) { m->overflow = 1; return; }
    m->idx[m->ni++] = (uint16_t)a;
    m->idx[m->ni++] = (uint16_t)b;
    m->idx[m->ni++] = (uint16_t)c;
}

/* A quad with the winding derived from the normal, so a face is always visible
 * from the side it says it faces. Hand-ordering corners against a stated normal
 * is a mistake waiting at every call site, and it fails silently. */
static void quad(CueVrFrameMesh *m, const float *p0, const float *p1,
                 const float *p2, const float *p3, const float *n,
                 float grain_len, float grain_wid, const float *col) {
    float u[3] = { p1[0]-p0[0], p1[1]-p0[1], p1[2]-p0[2] };
    float w[3] = { p2[0]-p0[0], p2[1]-p0[1], p2[2]-p0[2] };
    float cr[3] = { u[1]*w[2]-u[2]*w[1], u[2]*w[0]-u[0]*w[2], u[0]*w[1]-u[1]*w[0] };
    int flip = (cr[0]*n[0] + cr[1]*n[1] + cr[2]*n[2]) < 0.0f;
    const float *q[4] = { p0, p1, p2, p3 };
    int id[4];
    for (int k = 0; k < 4; k++) {
        const float *p = q[flip ? 3 - k : k];
        float uu = (k == 1 || k == 2) ? grain_len : 0.0f;
        float vv = (k >= 2) ? grain_wid : 0.0f;
        id[k] = vtx(m, p[0], p[1], p[2], n[0], n[1], n[2], uu, vv, col);
    }
    tri(m, id[0], id[1], id[2]);
    tri(m, id[0], id[2], id[3]);
}

/* A box from (x0,y0,z0) to (x1,y1,z1). `grain` picks which axis the timber's
 * grain runs along: 0 = X, 1 = Y, 2 = Z. */
static void box(CueVrFrameMesh *m, float x0, float y0, float z0,
                float x1, float y1, float z1, int grain, const float *col) {
    float dx = x1 - x0, dy = y1 - y0, dz = z1 - z0;
    float gx = grain == 0 ? dx : (grain == 1 ? dy : dz);
    /* six faces; grain length follows the timber, width is the other span */
    float a[3], b[3], c[3], d[3], n[3];
    /* -Z and +Z */
    n[0]=0; n[1]=0; n[2]=-1;
    a[0]=x0;a[1]=y0;a[2]=z0; b[0]=x1;b[1]=y0;b[2]=z0; c[0]=x1;c[1]=y1;c[2]=z0; d[0]=x0;d[1]=y1;d[2]=z0;
    quad(m, a,b,c,d, n, gx, grain==1?dx:dy, col);
    n[2]=1;
    a[2]=b[2]=c[2]=d[2]=z1;
    quad(m, a,b,c,d, n, gx, grain==1?dx:dy, col);
    /* -X and +X */
    n[0]=-1; n[1]=0; n[2]=0;
    a[0]=x0;a[1]=y0;a[2]=z0; b[0]=x0;b[1]=y0;b[2]=z1; c[0]=x0;c[1]=y1;c[2]=z1; d[0]=x0;d[1]=y1;d[2]=z0;
    quad(m, a,b,c,d, n, grain==2?dz:gx, grain==1?dz:dy, col);
    n[0]=1;
    a[0]=b[0]=c[0]=d[0]=x1;
    quad(m, a,b,c,d, n, grain==2?dz:gx, grain==1?dz:dy, col);
    /* -Y and +Y */
    n[0]=0; n[1]=-1; n[2]=0;
    a[0]=x0;a[1]=y0;a[2]=z0; b[0]=x1;b[1]=y0;b[2]=z0; c[0]=x1;c[1]=y0;c[2]=z1; d[0]=x0;d[1]=y0;d[2]=z1;
    quad(m, a,b,c,d, n, gx, dz, col);
    n[1]=1;
    a[1]=b[1]=c[1]=d[1]=y1;
    quad(m, a,b,c,d, n, gx, dz, col);
}

/* ---- Regency ------------------------------------------------------------ */

/* A chamfered, tapered post: square in section with its corners cut back so it
 * reads as an octagon, narrowing from `w0` at the top to `w1` at the bottom.
 * This is the leg, and it is where most of the character is — a plain box leg
 * makes the whole table look like flat-pack. */
/* The cut-corner square a leg is turned to: half-width h with the corners
 * taken back by `ch`, which reads as an octagon at a glance and as a chamfered
 * square when you look. Eight points, walked anticlockwise. */
static void octagon(float h, float ch, float *px, float *pz) {
    const float x[8] = {  h - ch,  h,       h,       h - ch,
                         -h + ch, -h,      -h,      -h + ch };
    const float z[8] = { -h,      -h + ch,  h - ch,  h,
                          h,       h - ch, -h + ch, -h };
    for (int i = 0; i < 8; i++) { px[i] = x[i]; pz[i] = z[i]; }
}

/* A chamfered, tapered post: the leg, and where most of the character is. A
 * plain box leg makes the whole table look like flat-pack. Grain runs UP it. */
static void post(CueVrFrameMesh *m, float cx, float cz, float y_top, float y_bot,
                 float w_top, float w_bot, float chamfer, const float *col)
{
    float tx[8], tz[8], bx[8], bz[8];
    octagon(w_top * 0.5f, chamfer, tx, tz);
    octagon(w_bot * 0.5f, chamfer * (w_bot / w_top), bx, bz);
    const float len = fabsf(y_top - y_bot);

    for (int i = 0; i < 8; i++) {
        int j = (i + 1) % 8;
        float a[3] = { cx + tx[i], y_top, cz + tz[i] };
        float b[3] = { cx + tx[j], y_top, cz + tz[j] };
        float c[3] = { cx + bx[j], y_bot, cz + bz[j] };
        float d[3] = { cx + bx[i], y_bot, cz + bz[i] };
        /* Face normal: outward in plan, tilted a little by the taper so the
         * light breaks along each facet instead of flooding the whole leg. */
        float mx = (tx[i] + tx[j]) * 0.5f, mz = (tz[i] + tz[j]) * 0.5f;
        float l = sqrtf(mx * mx + mz * mz);
        float n[3] = { l > 1e-6f ? mx / l : 1.0f, 0.12f, l > 1e-6f ? mz / l : 0.0f };
        float nl = sqrtf(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
        n[0] /= nl; n[1] /= nl; n[2] /= nl;
        quad(m, a, b, c, d, n, len, 0.085f, col);
    }
}

static void regency(CueVrFrameMesh *m, const CueTable *t) {
    const float hl = t->half_len, hw = t->half_wid, rw = t->rail_w;

    /* The outer footprint of the woodwork: the rails' own outer edge, so the
     * apron sits flush under them rather than proud of or inside them. */
    const float ox = hl + rw, oz = hw + rw;

    /* Apron. Deep enough to read as a cabinet, with a moulded top course that
     * oversails slightly and a bead line struck along it — the two details that
     * stop a band of timber looking like a cardboard box. */
    const float ap_top   = -0.004f;                  /* just under the bed */
    const float ap_h     = 0.180f;
    const float ap_bot   = ap_top - ap_h;
    const float oversail = 0.010f;                   /* the moulding's overhang */
    const float bead_y   = ap_top - 0.040f;
    const float bead_d   = 0.006f;

    /* The top course, in two: a black plate at the very top and the moulding
     * below it. The plate is all you see looking down through a pocket, and
     * black is the only right answer — there is nothing down a pocket. The
     * moulding is what you see from the side, so it keeps its timber. */
    box(m, -ox - oversail, ap_top - 0.007f, -oz - oversail,
            ox + oversail, ap_top,           oz + oversail, 0, SHADOW);
    box(m, -ox - oversail, ap_top - 0.026f, -oz - oversail,
            ox + oversail, ap_top - 0.007f,  oz + oversail, 0, MAHOGANY_LIT);
    /* the apron proper, in four runs so the grain follows each length */
    box(m, -ox, ap_bot, -oz, ox, ap_top - 0.026f, -oz + 0.026f, 0, MAHOGANY);
    box(m, -ox, ap_bot,  oz - 0.026f, ox, ap_top - 0.026f, oz, 0, MAHOGANY);
    box(m, -ox, ap_bot, -oz + 0.026f, -ox + 0.026f, ap_top - 0.026f, oz - 0.026f, 2, MAHOGANY);
    box(m,  ox - 0.026f, ap_bot, -oz + 0.026f, ox, ap_top - 0.026f, oz - 0.026f, 2, MAHOGANY);
    /* bead: a thin darker line struck along the apron, catching a shadow */
    box(m, -ox - 0.002f, bead_y - bead_d, -oz - 0.002f,
            ox + 0.002f, bead_y,           oz + 0.002f, 0, EBONISED);

    /* Cabinet: beams under the slate, seen when you stoop for a shot. Two
     * runners the length of the table and cross members between the legs. */
    const float beam_top = ap_bot + 0.010f;
    const float beam_h   = 0.055f;
    box(m, -hl, beam_top - beam_h, -0.055f, hl, beam_top, 0.055f, 0, EBONISED);

    /* Legs. Four is right for a pub table; a full-size snooker table needs
     * eight or the slate sags between them, so the count follows the length.
     * They stand under the corners, inset far enough that the apron carries
     * past them, plus evenly spaced pairs down the long sides. */
    int pairs = 2;
    float span = hl * 2.0f;
    if (span > 3.1f)      pairs = 4;
    else if (span > 2.4f) pairs = 3;

    const float leg_top = ap_bot + 0.004f;
    const float floor_y = -cuevr_frame_depth(t);
    /* Heavy. A match table's legs are baulks of timber carrying a quarter of a
     * tonne of slate — anything slender reads as a garden table, which is what
     * the first pass looked like. Nearly a hand's breadth square at the top,
     * tapering barely a third, and set close to the corners so the apron looks
     * carried rather than cantilevered. */
    const float inset   = 0.042f;
    const float lw_top  = 0.182f;
    const float lw_bot  = 0.138f;
    const float cap_h   = 0.062f;
    const float plinth_h = 0.055f;

    for (int p = 0; p < pairs; p++) {
        float fx = (pairs == 1) ? 0.0f
                 : (-1.0f + 2.0f * (float)p / (float)(pairs - 1));
        float cx = fx * (ox - inset - lw_top * 0.5f);
        for (int sz = -1; sz <= 1; sz += 2) {
            float cz = (float)sz * (oz - inset - lw_top * 0.5f);
            /* capital: a squarer, wider block where the leg meets the apron */
            box(m, cx - lw_top*0.5f - 0.011f, leg_top - cap_h, cz - lw_top*0.5f - 0.011f,
                   cx + lw_top*0.5f + 0.011f, leg_top,          cz + lw_top*0.5f + 0.011f,
                   1, MAHOGANY_LIT);
            /* the shaft */
            post(m, cx, cz, leg_top - cap_h, floor_y + plinth_h,
                 lw_top, lw_bot, 0.034f, EBONISED);
            /* plinth foot, with a brass shoe */
            box(m, cx - lw_bot*0.5f - 0.010f, floor_y + 0.012f, cz - lw_bot*0.5f - 0.010f,
                   cx + lw_bot*0.5f + 0.010f, floor_y + plinth_h, cz + lw_bot*0.5f + 0.010f,
                   1, MAHOGANY);
            box(m, cx - lw_bot*0.5f - 0.012f, floor_y, cz - lw_bot*0.5f - 0.012f,
                   cx + lw_bot*0.5f + 0.012f, floor_y + 0.012f, cz + lw_bot*0.5f + 0.012f,
                   1, BRASS);
        }
    }
}

/* ---- the registry ------------------------------------------------------- */

const CueVrFrameDesign CUEVR_FRAMES[] = {
    { "REGENCY", regency },
};
const int CUEVR_FRAME_COUNT = (int)(sizeof CUEVR_FRAMES / sizeof CUEVR_FRAMES[0]);

void cuevr_frame_build(int which, CueVrFrameMesh *m, const CueTable *t) {
    if (which < 0 || which >= CUEVR_FRAME_COUNT) which = 0;
    m->nv = m->ni = 0;
    m->overflow = 0;
    CUEVR_FRAMES[which].build(m, t);
}

void cuevr_frame_capacity(int *max_verts, int *max_indices) {
    /* Worst case is the eight-leg table: apron and cabinet are a fixed handful
     * of boxes, and each leg is a capital, an eight-sided shaft, a plinth and a
     * shoe. Generous — this is a one-off allocation. */
    if (max_verts)   *max_verts   = 4096;
    if (max_indices) *max_indices = 6144;
}

float cuevr_frame_depth(const CueTable *t) {
    (void)t;
    /* Cloth to floor. A match table is 2 ft 10 in to the bed; the player can
     * move the whole thing up or down in setup, but the table's own proportions
     * do not change with it. */
    return 0.85f;
}
