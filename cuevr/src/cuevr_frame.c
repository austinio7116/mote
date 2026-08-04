/*
 * CueVR — a table to stand the slate on.
 *
 * Four designs, because a table's body is most of what tells you what KIND of
 * table it is — long before you can see the pocket cut or count the balls:
 *
 *   REGENCY    the full-size match table. A deep apron with a moulded top edge
 *              and a bead struck along it, a cabinet of beams under the slate,
 *              and heavy octagonal legs on plinth feet.
 *   CABINET    the 7 ft pub table. Not a table on legs at all but a box, down
 *              to a low plinth, panelled, with corner posts and a ball return.
 *   VICTORIAN  the small snooker table everybody pictures: turned baluster legs
 *              under a deep beaded apron.
 *   AMERICAN   the 9 ft tournament table. A very deep skirt with a contrasting
 *              inlaid band, and enormous square tapered legs at the corners.
 *
 * cuevr_frame_default() picks the one that suits a table when the player has
 * not chosen; the menu can override it.
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
 *
 * A frame's wood is THE PLAYER'S WOOD — whatever they picked for the cushion
 * rails on the FRAME row of the menu. It used to be a set of fixed mahoganies
 * and oaks, so choosing a walnut table gave you walnut rails standing on a
 * mahogany body, and the two halves of one table disagreed with each other.
 *
 * So a design names a ROLE rather than a colour: the timber itself, a lit
 * course to catch the light along a moulding, and a dark course for the shadow
 * lines and the pieces that should recede. cuevr_frame_set_timber() resolves
 * those against the selected rail colour before a design is built.
 *
 * Metal and the black down a pocket stay absolute — brass is brass whatever the
 * table is made of. */
static float PAL_WOOD[3]  = { 0.286f, 0.129f, 0.086f };
static float PAL_LIT[3]   = { 0.353f, 0.169f, 0.106f };
static float PAL_DARK[3]  = { 0.086f, 0.055f, 0.047f };

void cuevr_frame_set_timber(const float rgb[3]) {
    for (int i = 0; i < 3; i++) {
        float c = rgb[i] < 0.0f ? 0.0f : (rgb[i] > 1.0f ? 1.0f : rgb[i]);
        PAL_WOOD[i] = c;
        /* The lit course is the same timber caught by the light, so it lifts
         * toward white rather than simply scaling — scaling a dark walnut by
         * 1.25 gives a slightly less dark walnut and reads as nothing. */
        PAL_LIT[i]  = c + (1.0f - c) * 0.18f;
        /* And the dark course is the same timber in shadow. */
        PAL_DARK[i] = c * 0.34f;
    }
}

static const float BRASS[3] = { 0.451f, 0.333f, 0.145f };

/* ---- what is timber and what is not --------------------------------------- *
 *
 * The renderer draws the body through the RAILS' wood shader, which is right for
 * the apron and the legs and quite wrong for brass, chrome, laminate and the
 * black inside a pocket. timber() adds a varnish specular, and a varnish
 * specular on a near-black surface is a grey square — which is exactly what
 * appeared around every pocket the moment the two shaders were merged.
 *
 * So a design is emitted TWICE and the mesh comes out in two runs: timber first,
 * everything else after, with the boundary recorded. Two draw calls, one buffer,
 * and no material tag needed on the vertex — the colour a piece is made of says
 * which it is, and every design draws from these arrays. */
static int s_pass;      /* 0 = emitting timber, 1 = emitting the rest */
/* The pocket positions live on the WORLD, not the table, and only the liners
 * want them — threading a second parameter through four design signatures and
 * every helper to reach one call each would be worse than this. */
static const CueWorld *WRLD;

static int is_timber(const float *col) {
    return col == PAL_WOOD || col == PAL_LIT || col == PAL_DARK;
}
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
    if (is_timber(col) != (s_pass == 0)) return;
    float u[3] = { p1[0]-p0[0], p1[1]-p0[1], p1[2]-p0[2] };
    float w[3] = { p2[0]-p0[0], p2[1]-p0[1], p2[2]-p0[2] };
    float cr[3] = { u[1]*w[2]-u[2]*w[1], u[2]*w[0]-u[0]*w[2], u[0]*w[1]-u[1]*w[0] };
    int flip = (cr[0]*n[0] + cr[1]*n[1] + cr[2]*n[2]) < 0.0f;
    const float *q[4] = { p0, p1, p2, p3 };
    int id[4];
    /* Where in the log this piece was cut from. Without it every board starts
     * at the same grain coordinate, so the same cathedral arch and the same
     * knot appear in exactly the same place on all four sides of the apron and
     * on all eight legs — which reads as a tiled texture, because it is one.
     * Derived from the piece's own position, so it is stable frame to frame and
     * a leg is the same leg every time you look at it. */
    float ou = p0[0] * 0.61f + p0[2] * 1.37f + p0[1] * 0.29f;
    float ov = p0[1] * 0.83f + p0[0] * 0.17f + p0[2] * 0.41f;
    for (int k = 0; k < 4; k++) {
        const float *p = q[flip ? 3 - k : k];
        float uu = ou + ((k == 1 || k == 2) ? grain_len : 0.0f);
        float vv = ov + ((k >= 2) ? grain_wid : 0.0f);
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

/* A top course of the frame, WITHOUT its top face.
 *
 * Every design capped its woodwork with a slab spanning the whole footprint. A
 * slab under a slate is invisible — except through the six holes in the slate,
 * where you were then looking at varnished timber a few millimetres below the
 * bed. The bucket below cannot hide it, because the slab is INSIDE the bucket.
 *
 * Losing the face is the right answer rather than a shortcut: it is under the
 * slate everywhere it exists, so the only place it is ever visible is exactly
 * the place it must not be. Cutting proper holes was tried first — a grid with
 * the covered cells dropped, which on a 12 ft table is 179 x 89 cells and 32,000
 * triangles for one moulding. It overran the frame buffer and left the whole
 * body untextured. Five faces and no tessellation costs nothing. */
static void holed_top(CueVrFrameMesh *m, const CueWorld *w,
                      float x0, float y0, float z0, float x1, float y1, float z1,
                      const float *col) {
    (void)w;
    float n[3];
    { float a[3]={x0,y0,z0},b[3]={x1,y0,z0},c[3]={x1,y1,z0},d[3]={x0,y1,z0};
      n[0]=0;n[1]=0;n[2]=-1; quad(m,a,b,c,d,n,x1-x0,y1-y0,col); }
    { float a[3]={x0,y0,z1},b[3]={x1,y0,z1},c[3]={x1,y1,z1},d[3]={x0,y1,z1};
      n[0]=0;n[1]=0;n[2]=1;  quad(m,a,b,c,d,n,x1-x0,y1-y0,col); }
    { float a[3]={x0,y0,z0},b[3]={x0,y0,z1},c[3]={x0,y1,z1},d[3]={x0,y1,z0};
      n[0]=-1;n[1]=0;n[2]=0; quad(m,a,b,c,d,n,z1-z0,y1-y0,col); }
    { float a[3]={x1,y0,z0},b[3]={x1,y0,z1},c[3]={x1,y1,z1},d[3]={x1,y1,z0};
      n[0]=1;n[1]=0;n[2]=0;  quad(m,a,b,c,d,n,z1-z0,y1-y0,col); }
    /* the underside, which you DO see when you stoop for a shot */
    { float a[3]={x0,y0,z0},b[3]={x1,y0,z0},c[3]={x1,y0,z1},d[3]={x0,y0,z1};
      n[0]=0;n[1]=-1;n[2]=0; quad(m,a,b,c,d,n,x1-x0,z1-z0,col); }
}

/* A DARK BUCKET under each pocket. Part of the frame; it does not touch the
 * pocket geometry, which is cue_render's and stays exactly as it is.
 *
 * Two things are being hidden. Looking straight down a pocket you must see
 * blackness with depth rather than a disc — that is what a real pocket looks
 * like, and it used to be a flat black lid across the whole frame. And looking
 * at the pocket from the side, through the gap between the pocket cut and the
 * woodwork, you must not see the inside of the table. That gap is why the bucket
 * is a good deal WIDER than the pocket: a liner that only just lines the hole
 * leaves the sight-line through the gap open, which reads as the table being
 * hollow and unfinished.
 *
 * Double-sided, because both of those views exist: from above you are inside the
 * bucket looking at its far wall, and from the side you are outside it looking
 * at its near wall. Backface culling would drop whichever one you were using. */
static void pocket_liners(CueVrFrameMesh *m, const CueTable *t, const CueWorld *w,
                          float top, float ox, float oz) {
    (void)t;
    if (!w) return;
    const int SIDES = 16;
    /* Deep enough that the floor is nowhere near the mouth — the pocket has to
     * read as HOLLOW. A shallow bucket is a black disc pasted over the hole, and
     * a black disc is exactly what a pocket must not look like. Still comfortably
     * inside the apron, so nothing hangs below the woodwork either. */
    const float DEPTH = 0.130f;
/* Just INSIDE, not flush. Clamped exactly to the footprint the bucket wall
 * lands coplanar with the apron's outer face and shows through it as a black
 * rectangle — the frame's own face and the darkness behind it fighting for the
 * same depth. A few millimetres of clearance and it is simply hidden. */
#define CIN 0.006f
#define CLX(v) ((v) >  ox-CIN ?  ox-CIN : ((v) < -ox+CIN ? -ox+CIN : (v)))
#define CLZ(v) ((v) >  oz-CIN ?  oz-CIN : ((v) < -oz+CIN ? -oz+CIN : (v)))
    const float WIDEN = 1.55f;      /* covers the gap around the cut, not just it */
    for (int k = 0; k < w->npocket; k++) {
        float cx = w->pocket[k].x, cz = w->pocket[k].z;
        float r = w->pocket_r[k] * WIDEN;
        /* STRICTLY below the bed, whatever a design passes in. The bucket is
         * wider than the pocket, so its rim runs under cloth rather than under
         * the hole — if it ever came up to y = 0 it would poke through the bed
         * and lay a black ring around the pocket lip. Clamped here rather than
         * trusted to each design's own top. */
        float y0 = top < -0.006f ? top : -0.006f;
        float y1 = y0 - DEPTH;
        for (int i = 0; i < SIDES; i++) {
            float a0 = 6.2831853f * i / SIDES, a1 = 6.2831853f * (i+1) / SIDES;
            float c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a1), s1 = sinf(a1);
            /* CLAMPED INTO THE FRAME. A pocket sits at or just outside the
             * corner of the woodwork, so a bucket wide enough to close the gap
             * around it is wider than the frame — and hangs out past the apron,
             * which breaks the table's silhouette from every angle you play
             * from. Squaring it off against the frame's own footprint costs
             * nothing: what is being shaped here is a volume of darkness and
             * nobody can see the shape of it. */
            float p0[3] = { CLX(cx + c0*r), y0, CLZ(cz + s0*r) };
            float p1[3] = { CLX(cx + c1*r), y0, CLZ(cz + s1*r) };
            float p2[3] = { CLX(cx + c1*r), y1, CLZ(cz + s1*r) };
            float p3[3] = { CLX(cx + c0*r), y1, CLZ(cz + s0*r) };
            float mc = (c0 + c1) * 0.5f, ms = (s0 + s1) * 0.5f;
            float l = sqrtf(mc*mc + ms*ms);
            if (l < 1e-6f) continue;
            float no[3] = {  mc/l, 0.0f,  ms/l };     /* outward */
            float ni[3] = { -mc/l, 0.0f, -ms/l };     /* and inward */
            quad(m, p0, p1, p2, p3, no, 0.05f, DEPTH, SHADOW);
            quad(m, p0, p1, p2, p3, ni, 0.05f, DEPTH, SHADOW);
        }
        /* The floor, so it is a bucket and not a pipe through the room. A fan of
         * quads spanning two segments each — a fan of triangles would need half
         * of every quad to be degenerate. */
        for (int i = 0; i < SIDES; i += 2) {
            float a0 = 6.2831853f * i / SIDES;
            float a1 = 6.2831853f * (i+1) / SIDES;
            float a2 = 6.2831853f * (i+2) / SIDES;
            float p0[3] = { cx, y1, cz };
            float p1[3] = { CLX(cx + cosf(a0)*r), y1, CLZ(cz + sinf(a0)*r) };
            float p2[3] = { CLX(cx + cosf(a1)*r), y1, CLZ(cz + sinf(a1)*r) };
            float p3[3] = { CLX(cx + cosf(a2)*r), y1, CLZ(cz + sinf(a2)*r) };
            float n[3] = { 0.0f, 1.0f, 0.0f };
            quad(m, p0, p1, p2, p3, n, 0.05f, 0.05f, SHADOW);
        }
    }
#undef CLX
#undef CLZ
#undef CIN
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
    pocket_liners(m, t, WRLD, ap_top, ox, oz);
    holed_top(m, WRLD, -ox - oversail, ap_top - 0.026f, -oz - oversail,
                        ox + oversail, ap_top - 0.007f,  oz + oversail, PAL_LIT);
    /* the apron proper, in four runs so the grain follows each length */
    box(m, -ox, ap_bot, -oz, ox, ap_top - 0.026f, -oz + 0.026f, 0, PAL_WOOD);
    box(m, -ox, ap_bot,  oz - 0.026f, ox, ap_top - 0.026f, oz, 0, PAL_WOOD);
    box(m, -ox, ap_bot, -oz + 0.026f, -ox + 0.026f, ap_top - 0.026f, oz - 0.026f, 2, PAL_WOOD);
    box(m,  ox - 0.026f, ap_bot, -oz + 0.026f, ox, ap_top - 0.026f, oz - 0.026f, 2, PAL_WOOD);
    /* bead: a thin darker line struck along the apron, catching a shadow */
    box(m, -ox - 0.002f, bead_y - bead_d, -oz - 0.002f,
            ox + 0.002f, bead_y,           oz + 0.002f, 0, PAL_DARK);

    /* Cabinet: beams under the slate, seen when you stoop for a shot. Two
     * runners the length of the table and cross members between the legs. */
    const float beam_top = ap_bot + 0.010f;
    const float beam_h   = 0.055f;
    box(m, -hl, beam_top - beam_h, -0.055f, hl, beam_top, 0.055f, 0, PAL_DARK);

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
                   1, PAL_LIT);
            /* the shaft */
            post(m, cx, cz, leg_top - cap_h, floor_y + plinth_h,
                 lw_top, lw_bot, 0.034f, PAL_DARK);
            /* plinth foot, with a brass shoe */
            box(m, cx - lw_bot*0.5f - 0.010f, floor_y + 0.012f, cz - lw_bot*0.5f - 0.010f,
                   cx + lw_bot*0.5f + 0.010f, floor_y + plinth_h, cz + lw_bot*0.5f + 0.010f,
                   1, PAL_WOOD);
            box(m, cx - lw_bot*0.5f - 0.012f, floor_y, cz - lw_bot*0.5f - 0.012f,
                   cx + lw_bot*0.5f + 0.012f, floor_y + 0.012f, cz + lw_bot*0.5f + 0.012f,
                   1, BRASS);
        }
    }
}

/* ---- Cabinet: the 7 ft pub table ---------------------------------------- *
 *
 * A pub table is a slate on a BODY, and the body is neither a table's apron nor
 * a crate. Three things give it its shape and it had none of them:
 *
 *   - the sides TAPER IN going down, by about 75 mm a side. That slope is the
 *     whole silhouette. Without it you have a wardrobe, which is exactly what
 *     the first version looked like from across the room.
 *   - it stands on LEGS. Short, square, set in at the corners, with the body
 *     stopping 170 mm clear of the floor — you can see under a pub table, and
 *     the shadow under it is a large part of how it reads as furniture.
 *   - the top and bottom of the body are banded in a lighter trim, and there is
 *     a coin door and a ball-return hatch at the foot end.
 *
 * The recessed panels that were here instead are gone. On a sloped face they
 * are wrong anyway, and at any distance you would actually view a table from
 * they read as nothing at all.
 */
static const float CAB_BODY[3]  = { 0.129f, 0.145f, 0.169f };  /* charcoal laminate */
static const float CAB_TRIM[3]  = { 0.216f, 0.235f, 0.267f };
static const float CAB_PANEL[3] = { 0.075f, 0.086f, 0.102f };
static const float CAB_CHROME[3]= { 0.545f, 0.573f, 0.608f };

/* One face of a tapering shell. The normal is taken from the geometry and then
 * turned outward, rather than being stated — on a sloped face, writing the
 * normal by hand is a mistake waiting at every call site and it fails silently
 * (the face just goes black). */
static void slope_face(CueVrFrameMesh *m, const float *p0, const float *p1,
                       const float *p2, const float *p3,
                       float outx, float outz,
                       float glen, float gwid, const float *col) {
    float u[3] = { p1[0]-p0[0], p1[1]-p0[1], p1[2]-p0[2] };
    float w[3] = { p3[0]-p0[0], p3[1]-p0[1], p3[2]-p0[2] };
    float n[3] = { u[1]*w[2]-u[2]*w[1], u[2]*w[0]-u[0]*w[2], u[0]*w[1]-u[1]*w[0] };
    if (n[0]*outx + n[2]*outz < 0.0f) { n[0]=-n[0]; n[1]=-n[1]; n[2]=-n[2]; }
    float l = sqrtf(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
    if (l < 1e-9f) return;
    n[0] /= l; n[1] /= l; n[2] /= l;
    quad(m, p0, p1, p2, p3, n, glen, gwid, col);
}

/* A band of the tapering shell: four sloped faces between the rectangle
 * (ax0, az0) at y0 and the rectangle (ax1, az1) at y1. Stacking bands with
 * different timbers is how the trim runs round without a separate mitre. */
static void frustum_band(CueVrFrameMesh *m, float y0, float y1,
                         float ax0, float az0, float ax1, float az1,
                         const float *col) {
    float h = fabsf(y0 - y1);
    for (int sg = -1; sg <= 1; sg += 2) {
        float z0 = (float)sg * az0, z1 = (float)sg * az1;
        float a[3] = { -ax0, y0, z0 }, b[3] = {  ax0, y0, z0 };
        float c[3] = {  ax1, y1, z1 }, d[3] = { -ax1, y1, z1 };
        slope_face(m, a, b, c, d, 0.0f, (float)sg, ax0 * 2.0f, h, col);

        float x0 = (float)sg * ax0, x1 = (float)sg * ax1;
        float e[3] = { x0, y0, -az0 }, f[3] = { x0, y0,  az0 };
        float g[3] = { x1, y1,  az1 }, i[3] = { x1, y1, -az1 };
        slope_face(m, e, f, g, i, (float)sg, 0.0f, az0 * 2.0f, h, col);
    }
}

static void cabinet(CueVrFrameMesh *m, const CueTable *t) {
    const float hl = t->half_len, hw = t->half_wid, rw = t->rail_w;
    const float ox = hl + rw, oz = hw + rw;
    const float top     = -0.004f;
    const float floor_y = -cuevr_frame_depth(t);
    const float leg_h   = 0.170f;                /* daylight under the body */
    const float body_bot = floor_y + leg_h;
    const float TAPER   = 0.078f;                /* per side, over the body */

    pocket_liners(m, t, WRLD, top, ox, oz);

    /* The body, as three bands of one continuous taper: a trim course at the
     * top, the long laminate flank, and a trim course at the foot. The
     * half-extents are interpolated down the taper so the three bands are one
     * unbroken slope and not three stacked boxes. */
    const float y_a = top - 0.008f;              /* under the black top plate */
    const float y_b = y_a - 0.052f;              /* bottom of the top trim */
    const float y_c = body_bot + 0.050f;         /* top of the foot trim */
    const float H   = y_a - body_bot;
    #define CAB_IN(y)  (TAPER * ((y_a) - (y)) / (H))
    frustum_band(m, y_a, y_b, ox, oz,
                 ox - CAB_IN(y_b), oz - CAB_IN(y_b), CAB_TRIM);
    frustum_band(m, y_b, y_c, ox - CAB_IN(y_b), oz - CAB_IN(y_b),
                 ox - CAB_IN(y_c), oz - CAB_IN(y_c), CAB_BODY);
    frustum_band(m, y_c, body_bot, ox - CAB_IN(y_c), oz - CAB_IN(y_c),
                 ox - TAPER, oz - TAPER, CAB_TRIM);
    /* the underside, so there is no hole when you stoop for a shot */
    box(m, -(ox - TAPER), body_bot - 0.012f, -(oz - TAPER),
           ox - TAPER, body_bot, oz - TAPER, 0, SHADOW);

    /* Coin door and ball-return hatch, at the foot end. The two details that
     * say this one takes pound coins — and they sit on the sloped face, so they
     * are pushed out along it rather than bolted on flat. */
    {
        float ymid = (y_b + y_c) * 0.5f;
        float xf = ox - CAB_IN(ymid);
        box(m, xf - 0.006f, ymid - 0.075f, -hw * 0.30f,
               xf + 0.010f, ymid + 0.075f,  hw * 0.30f, 2, CAB_PANEL);
        box(m, xf - 0.004f, ymid + 0.075f, -hw * 0.32f,
               xf + 0.012f, ymid + 0.092f,  hw * 0.32f, 2, CAB_CHROME);
    }

    /* Legs. Square, chunky, set in at the corners under the narrow end of the
     * taper, with a chromed foot — which is what a pub table stands on and what
     * lets you see that it is standing on anything. */
    const float lw = 0.090f;
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sz = -1; sz <= 1; sz += 2) {
            float cx = (float)sx * (ox - TAPER - lw * 0.60f);
            float cz = (float)sz * (oz - TAPER - lw * 0.60f);
            post(m, cx, cz, body_bot, floor_y + 0.020f, lw, lw * 0.88f,
                 lw * 0.16f, CAB_PANEL);
            box(m, cx - lw * 0.52f, floor_y, cz - lw * 0.52f,
                   cx + lw * 0.52f, floor_y + 0.020f, cz + lw * 0.52f,
                   1, CAB_CHROME);
        }
    #undef CAB_IN
}

/* ---- Victorian: turned legs, for the small snooker tables ---------------- *
 *
 * The one everybody pictures when they picture a billiard table in a house: a
 * baluster leg turned on a lathe — a square pad at the top, then a swelling
 * vase, a run of beads, a long taper and a turned foot — under a deep apron
 * with a carved bead. It is all in the leg, and a leg turned on a lathe is a
 * PROFILE revolved, so that is what this builds: a radius as a function of
 * height, swept round twelve sides.
 */

/* Revolve a profile. `r(f)` for f in 0..1 down the leg, sampled at `steps`
 * stations and swept `sides` ways. Twelve sides is enough that the facets read
 * as turning marks rather than as a polygon, which is a happy accident worth
 * keeping: real turned work has them too. Ten sides and forty stations is 400
 * quads a leg; the beads are only 8% of the length, so the stations have to be
 * close or they vanish, and it is the stations rather than the sides that this
 * shape needs. */
typedef float (*Profile)(float f);

static void turned(CueVrFrameMesh *m, float cx, float cz, float y_top,
                   float y_bot, Profile r, int steps, const float *col) {
    const int SIDES = 10;
    const float len = fabsf(y_top - y_bot);
    for (int i = 0; i < steps; i++) {
        float f0 = (float)i / (float)steps, f1 = (float)(i + 1) / (float)steps;
        float y0 = y_top + (y_bot - y_top) * f0;
        float y1 = y_top + (y_bot - y_top) * f1;
        float r0 = r(f0), r1 = r(f1);
        /* The face normal has to lean with the profile or a bead looks like a
         * cylinder: the slope over this station is dr/dy, and the outward
         * normal is perpendicular to it. */
        float dr = r1 - r0, dy = y1 - y0;
        float sl = sqrtf(dr * dr + dy * dy);
        float nr = sl > 1e-6f ? -dy / sl : 1.0f;
        float ny = sl > 1e-6f ?  dr / sl : 0.0f;
        if (nr < 0.0f) { nr = -nr; ny = -ny; }
        for (int k = 0; k < SIDES; k++) {
            float a0 = 6.2831853f * (float)k / SIDES;
            float a1 = 6.2831853f * (float)(k + 1) / SIDES;
            float c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a1), s1 = sinf(a1);
            float p0[3] = { cx + c0 * r0, y0, cz + s0 * r0 };
            float p1[3] = { cx + c1 * r0, y0, cz + s1 * r0 };
            float p2[3] = { cx + c1 * r1, y1, cz + s1 * r1 };
            float p3[3] = { cx + c0 * r1, y1, cz + s0 * r1 };
            float mc = (c0 + c1) * 0.5f, ms = (s0 + s1) * 0.5f;
            float ml = sqrtf(mc * mc + ms * ms);
            if (ml > 1e-6f) { mc /= ml; ms /= ml; }
            float n[3] = { mc * nr, ny, ms * nr };
            /* Grain runs UP a leg, and across it is the circumference. */
            quad(m, p0, p1, p2, p3, n, len / (float)steps, r0 * 0.9f, col);
        }
    }
}

/* The baluster. Written as one expression per feature so the shape can be read:
 * pad, cove, vase, bead run, taper, foot. `f` is 0 at the apron, 1 at the floor. */
static float baluster(float f) {
    float r = 0.086f;
    if (f < 0.08f) return 0.100f;                       /* square-ish pad */
    if (f < 0.13f) return 0.100f - 0.030f * (f - 0.08f) / 0.05f;   /* cove in */
    if (f < 0.34f) {                                     /* the vase */
        float u = (f - 0.13f) / 0.21f;
        return 0.070f + 0.044f * sinf(u * 3.14159265f);
    }
    if (f < 0.42f) {                                     /* three beads */
        float u = (f - 0.34f) / 0.08f;
        return 0.058f + 0.016f * fabsf(sinf(u * 3.0f * 3.14159265f));
    }
    if (f < 0.90f) {                                     /* the long taper */
        float u = (f - 0.42f) / 0.48f;
        return 0.062f - 0.020f * u;
    }
    r = 0.042f + 0.024f * sinf((f - 0.90f) / 0.10f * 3.14159265f);  /* turned foot */
    return r;
}

static void victorian(CueVrFrameMesh *m, const CueTable *t) {
    const float hl = t->half_len, hw = t->half_wid, rw = t->rail_w;
    const float ox = hl + rw, oz = hw + rw;
    const float ap_top = -0.004f;
    const float ap_h   = 0.205f;                  /* deeper than the Regency */
    const float ap_bot = ap_top - ap_h;
    const float floor_y = -cuevr_frame_depth(t);

    pocket_liners(m, t, WRLD, ap_top, ox, oz);
    /* An ovolo top course that oversails properly — this is a heavier table
     * than the Regency and the mouldings are correspondingly bolder. */
    holed_top(m, WRLD, -ox - 0.016f, ap_top - 0.034f, -oz - 0.016f,
                        ox + 0.016f, ap_top - 0.008f,  oz + 0.016f, PAL_LIT);
    holed_top(m, WRLD, -ox - 0.006f, ap_top - 0.046f, -oz - 0.006f,
                        ox + 0.006f, ap_top - 0.034f,  oz + 0.006f, PAL_DARK);

    box(m, -ox, ap_bot, -oz, ox, ap_top - 0.046f, -oz + 0.028f, 0, PAL_WOOD);
    box(m, -ox, ap_bot,  oz - 0.028f, ox, ap_top - 0.046f, oz, 0, PAL_WOOD);
    box(m, -ox, ap_bot, -oz + 0.028f, -ox + 0.028f, ap_top - 0.046f, oz - 0.028f, 2, PAL_WOOD);
    box(m,  ox - 0.028f, ap_bot, -oz + 0.028f, ox, ap_top - 0.046f, oz - 0.028f, 2, PAL_WOOD);
    /* a carved bead low on the apron, and a beaded bottom edge */
    box(m, -ox - 0.004f, ap_bot + 0.030f, -oz - 0.004f,
            ox + 0.004f, ap_bot + 0.040f,  oz + 0.004f, 0, PAL_DARK);
    box(m, -ox - 0.008f, ap_bot, -oz - 0.008f,
            ox + 0.008f, ap_bot + 0.014f, oz + 0.008f, 0, PAL_LIT);

    /* Legs. A small snooker table is short enough for four; a 10 ft one wants
     * six or the slate sags at the middle pockets, same rule as the Regency. */
    int pairs = (hl * 2.0f > 2.7f) ? 3 : 2;
    const float inset = 0.038f;
    const float leg_top = ap_bot + 0.006f;
    for (int p = 0; p < pairs; p++) {
        float fx = (pairs == 1) ? 0.0f
                 : (-1.0f + 2.0f * (float)p / (float)(pairs - 1));
        float cx = fx * (ox - inset - 0.100f);
        for (int sz = -1; sz <= 1; sz += 2) {
            float cz = (float)sz * (oz - inset - 0.100f);
            /* the square pad the turning starts from */
            box(m, cx - 0.098f, leg_top - 0.052f, cz - 0.098f,
                   cx + 0.098f, leg_top,          cz + 0.098f, 1, PAL_LIT);
            turned(m, cx, cz, leg_top - 0.052f, floor_y + 0.010f, baluster, 40, PAL_WOOD);
            box(m, cx - 0.052f, floor_y, cz - 0.052f,
                   cx + 0.052f, floor_y + 0.010f, cz + 0.052f, 1, BRASS);
        }
    }
}

/* ---- American: the 9 ft tournament table -------------------------------- *
 *
 * Where the English tables are about mouldings, an American 9-footer is about
 * mass: a very deep skirt with a contrasting inlaid band running its whole
 * length, and enormous square legs, tapered on all four faces, set right at the
 * corners with almost no inset. The look is architectural rather than furniture
 * — it should read as a plinth carrying a slab, and nothing on it should be
 * turned or beaded.
 */

static void american(CueVrFrameMesh *m, const CueTable *t) {
    const float hl = t->half_len, hw = t->half_wid, rw = t->rail_w;
    const float ox = hl + rw, oz = hw + rw;
    const float top = -0.004f;
    const float floor_y = -cuevr_frame_depth(t);

    const float skirt_h  = 0.255f;               /* deep. this is the whole look */
    const float skirt_bot = top - skirt_h;
    const float inlay_y  = top - 0.118f;

    pocket_liners(m, t, WRLD, top, ox, oz);
    /* a flat oversailing cap, square-edged, no ovolo */
    holed_top(m, WRLD, -ox - 0.016f, top - 0.030f, -oz - 0.016f,
                        ox + 0.016f, top - 0.009f,  oz + 0.016f, PAL_LIT);

    /* The skirt, in four runs, split above and below the inlay so each band is
     * its own timber and the grain follows each side. */
    const float bands[3][2] = { { inlay_y + 0.013f, top - 0.030f },
                                { inlay_y,          inlay_y + 0.013f },
                                { skirt_bot,        inlay_y } };
    const float *bcol[3] = { PAL_WOOD, PAL_LIT, PAL_WOOD };
    for (int b = 0; b < 3; b++) {
        float y0 = bands[b][0], y1 = bands[b][1];
        const float *c = bcol[b];
        float pr = (b == 1) ? 0.005f : 0.0f;     /* the inlay stands slightly proud */
        box(m, -ox, y0, -oz - pr, ox, y1, -oz + 0.032f, 0, c);
        box(m, -ox, y0,  oz - 0.032f, ox, y1, oz + pr, 0, c);
        box(m, -ox - pr, y0, -oz + 0.032f, -ox + 0.032f, y1, oz - 0.032f, 2, c);
        box(m,  ox - 0.032f, y0, -oz + 0.032f, ox + pr, y1, oz - 0.032f, 2, c);
    }
    /* a plain chamfered bottom edge */
    box(m, -ox + 0.006f, skirt_bot - 0.016f, -oz + 0.006f,
            ox - 0.006f, skirt_bot,           oz - 0.006f, 0, PAL_LIT);

    /* Legs: square, tapered on all four faces, at the corners. `post` with no
     * chamfer to speak of gives a square section, which is what this wants —
     * an octagon here would look English. */
    int pairs = (hl * 2.0f > 2.9f) ? 3 : 2;
    const float lw_top = 0.215f, lw_bot = 0.160f;
    const float leg_top = skirt_bot - 0.010f;
    for (int p = 0; p < pairs; p++) {
        float fx = (pairs == 1) ? 0.0f
                 : (-1.0f + 2.0f * (float)p / (float)(pairs - 1));
        float cx = fx * (ox - 0.014f - lw_top * 0.5f);
        for (int sz = -1; sz <= 1; sz += 2) {
            float cz = (float)sz * (oz - 0.014f - lw_top * 0.5f);
            post(m, cx, cz, leg_top, floor_y + 0.022f, lw_top, lw_bot, 0.012f,
                 PAL_WOOD);
            /* a levelling foot, which every 9-footer has and none of them hides */
            box(m, cx - lw_bot*0.5f, floor_y + 0.010f, cz - lw_bot*0.5f,
                   cx + lw_bot*0.5f, floor_y + 0.022f, cz + lw_bot*0.5f, 1, PAL_LIT);
            box(m, cx - 0.052f, floor_y, cz - 0.052f,
                   cx + 0.052f, floor_y + 0.010f, cz + 0.052f, 1, BRASS);
        }
    }
}

/* ---- the registry ------------------------------------------------------- */

const CueVrFrameDesign CUEVR_FRAMES[] = {
    { "REGENCY",   regency },
    { "CABINET",   cabinet },
    { "VICTORIAN", victorian },
    { "AMERICAN",  american },
};
const int CUEVR_FRAME_COUNT = (int)(sizeof CUEVR_FRAMES / sizeof CUEVR_FRAMES[0]);

void cuevr_frame_build(int which, CueVrFrameMesh *m, const CueTable *t,
                       const CueWorld *w) {
    if (which < 0 || which >= CUEVR_FRAME_COUNT) which = 0;
    m->nv = m->ni = 0;
    m->overflow = 0;
    WRLD = w;
    /* Timber first, then everything else, with the boundary recorded — the
     * renderer draws the two runs with two different shaders. See is_timber. */
    s_pass = 0;
    CUEVR_FRAMES[which].build(m, t);
    m->n_timber_idx = m->ni;
    s_pass = 1;
    CUEVR_FRAMES[which].build(m, t);
    WRLD = NULL;
}

/* Which design suits a given table, when the player has not chosen one. Every
 * table kind has an obvious real-world answer and this is it: a pub table is a
 * cabinet, a 9 ft American is an American, a small snooker table is the turned
 * Victorian everyone pictures, and the full-size match tables keep the Regency
 * they were built against. */
int cuevr_frame_default(const CueTable *t) {
    switch (t->kind) {
    case CUE_GAME_UK8:
    case CUE_GAME_SNK6:   return 1;   /* CABINET   — 7 ft, coin-op body */
    case CUE_GAME_US8:
    case CUE_GAME_US9:    return 3;   /* AMERICAN  — 9 ft, deep skirt, square legs */
    case CUE_GAME_SNK10:  return 2;   /* VICTORIAN — the small snooker table */
    default:              return 0;   /* REGENCY   — 12 ft match, and the 10 ft CN */
    }
}

void cuevr_frame_capacity(int *max_verts, int *max_indices) {
    /* Worst case is the eight-leg table: apron and cabinet are a fixed handful
     * of boxes, and each leg is a capital, an eight-sided shaft, a plinth and a
     * shoe. Generous — this is a one-off allocation. */
    /* The Victorian's turned legs dominate: 40 stations x 10 sides is 400 quads
     * a leg, and a 10 ft table has six of them. Everything else is boxes. */
    if (max_verts)   *max_verts   = 16384;
    if (max_indices) *max_indices = 24576;
}

float cuevr_frame_depth(const CueTable *t) {
    (void)t;
    /* Cloth to floor. A match table is 2 ft 10 in to the bed; the player can
     * move the whole thing up or down in setup, but the table's own proportions
     * do not change with it. */
    return 0.85f;
}
