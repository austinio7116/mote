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
#include <stdlib.h>
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
/* A fourth tone: a CONTRASTING inlay timber. The American's maple band was
 * PAL_LIT — 18% toward white — which is a subtlety for a moulding to catch
 * light with and invisible as an inlay: the band simply did not read, and the
 * whole skirt photographed as one brown box. An inlay is a different wood on
 * purpose, so it leans well toward maple whatever the body timber is. */
static float PAL_INLAY[3] = { 0.55f, 0.42f, 0.24f };

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
        /* The inlay keeps a little of the body's cast so the two woods look
         * finished together, but most of it is its own pale maple. */
        static const float MAPLE[3] = { 0.60f, 0.47f, 0.28f };
        PAL_INLAY[i] = c * 0.35f + MAPLE[i] * 0.65f;
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
/* The pocket positions live on the WORLD, not the table, and only the cut-outs
 * want them — threading a second parameter through four design signatures and
 * every helper to reach one call each would be worse than this. */
static const CueWorld *WRLD;

static int is_timber(const float *col) {
    return col == PAL_WOOD || col == PAL_LIT || col == PAL_DARK
        || col == PAL_INLAY;
}
/* The top face of the frame is what you see when you look down a pocket, so
 * it is black: a pocket has no floor to show you, and anything lighter reads
 * as the hole having been filled in with wood — which is what it looked like. */
static const float SHADOW[3] = { 0.012f, 0.010f, 0.010f };
/* The tray is the same black, except when CUEVR_TRAYVIS is set — then it is a
 * bright blue, because a black occluder against black woodwork is invisible
 * exactly when you need to see where it actually lies. */
static float TRAY[3] = { 0.012f, 0.010f, 0.010f };

/* ---- where the tray is, so that nothing else goes there ------------------ *
 *
 * The tray is the table's floor: a rim rectangle at the base of the pocket
 * cut-outs, walls leaning in at 30 degrees, a flat bottom below the pockets.
 * Everything else in this file has to keep out of it, and rather than each
 * design carrying its own copy of those numbers (which is how the body ended up
 * cutting across the pocket throats), they are computed once per build and every
 * piece asks tray_x/tray_z how far in it may reach at its own height. */
#define TRAY_RIM_Y   (-0.002f)        /* cue_render's bore_bot */
#define TRAY_SLOPE   (0.5773503f)     /* 30 degrees off vertical */
static float TRAY_RX, TRAY_RZ, TRAY_BOT;
static float SURR_X, SURR_Z;      /* cue_render's rail frame, outer face */

static void tray_measure(const CueTable *t, const CueWorld *w) {
    TRAY_RX = TRAY_RZ = 0.0f;
    if (w) for (int k = 0; k < w->npocket; k++) {
        /* the outer corner of the square the pocket is cut out of: cue_render's
         * bore notch, one bore radius beyond the pocket centre. Over EVERY
         * pocket, not just the corners — on the shallow-railed tables the middle
         * pockets' cut-outs reach 18 mm further out in Z than the corners do. */
        float br = (k < 4) ? t->pr_corner : t->pr_side;
        float ax = fabsf(w->pocket[k].x) + br, az = fabsf(w->pocket[k].z) + br;
        if (ax > TRAY_RX) TRAY_RX = ax;
        if (az > TRAY_RZ) TRAY_RZ = az;
    }
    /* Below the bottom of the pocket, not merely below the cloth lip: the lip
     * stops 23-35 mm down but the throat runs to 55 mm on a pool table and
     * 105 mm on a snooker one. Both are cue_render's own numbers. */
    TRAY_BOT = (t->is_snooker ? -0.105f : -0.055f) - 0.008f;
    /* and the rail above, whose outer face the body has to reach up to */
    SURR_X = t->half_len + t->rail_w + 0.055f;
    SURR_Z = t->half_wid + t->rail_w + 0.055f;
}

/* How far in a piece at height `y` may reach before it fouls the tray.
 *
 * The limit is the RIM rectangle, not the sloped wall at that height. The rim
 * circumscribes every bore — it is one bore radius beyond each pocket centre on
 * both axes — so a piece kept outside it can never appear inside a pocket, while
 * a piece that merely follows the wall down shows as a crescent in the bore the
 * moment the wall has leaned in past the bore's edge. That crescent is what the
 * first attempt at this produced. Below the tray's floor there is nothing to
 * avoid: the floor is opaque and hides everything under it. */
static float tray_lim(float rim, float y) {
    if (y <= TRAY_BOT) return 0.0f;
    return rim + 0.002f;                          /* 2 mm so faces never touch */
}
static float tray_x(float y) { return tray_lim(TRAY_RX, y); }
static float tray_z(float y) { return tray_lim(TRAY_RZ, y); }
/* `v` pulled out to clear the tray at height y, if it does not already. */
static float clear_x(float v, float y) { float l = tray_x(y); return v < l ? l : v; }
static float clear_z(float v, float y) { float l = tray_z(y); return v < l ? l : v; }

/* The woodwork's outer face: the design's own, widened only as far as the tray
 * makes it. Every design was drawn to the rail's width, and on five of the seven
 * tables that is INSIDE the tray's rim — by 25 mm on the 9 ft American, because
 * its pockets are the biggest — so the body would stand in the tray. Widening to
 * the rim plus a hair is the least that fixes it: nothing on the two snooker
 * tables, 9 mm on the Chinese, 17 mm on the pub tables, 25 mm on the Americans.
 *
 * Taking it all the way out to the rail's own face instead was tried and is what
 * "blockier than they need to be" looks like: a 12 ft table gained 45 mm a side
 * for no reason, since it was already clear. The ledge that leaves under the
 * rail is closed by rail_undercut() below, which is a moulding rather than more
 * bulk. */
static void body_box(const CueTable *t, float proud, float *ox, float *oz) {
    (void)proud;
    float bx = t->half_len + t->rail_w, bz = t->half_wid + t->rail_w;
    float lx = TRAY_RX + 0.002f,        lz = TRAY_RZ + 0.002f;
    *ox = bx > lx ? bx : lx;
    *oz = bz > lz ? bz : lz;
}

static void slope_face(CueVrFrameMesh *m, const float *p0, const float *p1,
                       const float *p2, const float *p3,
                       float outx, float outz,
                       float glen, float gwid, const float *col);
static void frustum_band(CueVrFrameMesh *m, float y0, float y1,
                         float ax0, float az0, float ax1, float az1,
                         const float *col);

/* A moulding PROFILE, swept right round the body: stations of (proud, y),
 * where `proud` is how far the surface stands off the face at (ox, oz) — it
 * may go negative for a quirk, the little recessed slot a moulding is struck
 * with. Consecutive stations become frustum rings, so the corners mitre
 * themselves.
 *
 * This is the difference between a moulding and a coloured stripe. Every trim
 * course here used to be a stack of boxes in a lighter or darker paint, and at
 * any distance that is exactly what it looked like. The body is lit — mode 7
 * is N.L wood with a varnish lobe — so a profile that actually curves catches
 * the lamps along its top and shades along its throat with no paint at all,
 * which is what a moulding IS. */
static void moulding(CueVrFrameMesh *m, float ox, float oz,
                     const float st[][2], int n, const float *col) {
    for (int i = 0; i + 1 < n; i++)
        frustum_band(m, st[i][1], st[i + 1][1],
                     ox + st[i][0],     oz + st[i][0],
                     ox + st[i + 1][0], oz + st[i + 1][0], col);
}

/* A raised-and-fielded panel on a vertical face: a bevel ring rising from the
 * face to a flat field standing `proud` off it. The bevel's four slopes take
 * four different normals, so the top edge catches the lamps and the bottom
 * shades itself — the panel reads by light, not by outline.
 * axis 0: the face looks along Z (a long side, extent a0..a1 in X);
 * axis 1: the face looks along X (an end, extent a0..a1 in Z).
 * sg is which of the two faces, face the |coordinate| of its plane. */
static void fielded_panel(CueVrFrameMesh *m, int axis, float sg, float face,
                          float a0, float a1, float y0, float y1,
                          float bevel, float proud, const float *col) {
    if (a1 - a0 < bevel * 3.0f || y1 - y0 < bevel * 3.0f) return;
    float f0 = face, f1 = face + proud;
    float i0 = a0 + bevel, i1 = a1 - bevel;
    float j0 = y0 + bevel, j1 = y1 - bevel;
    /* the four bevel slopes, then the field */
    #define P(A,Y,F) { axis ? (float)sg*(F) : (A), (Y), axis ? (A) : (float)sg*(F) }
    { float p0[4][3] = { P(a0,y0,f0), P(a1,y0,f0), P(i1,j0,f1), P(i0,j0,f1) };
      slope_face(m, p0[0],p0[1],p0[2],p0[3], axis?(float)sg:0.0f, axis?0.0f:(float)sg,
                 a1-a0, bevel, col); }
    { float p0[4][3] = { P(a0,y1,f0), P(a1,y1,f0), P(i1,j1,f1), P(i0,j1,f1) };
      slope_face(m, p0[0],p0[1],p0[2],p0[3], axis?(float)sg:0.0f, axis?0.0f:(float)sg,
                 a1-a0, bevel, col); }
    { float p0[4][3] = { P(a0,y0,f0), P(a0,y1,f0), P(i0,j1,f1), P(i0,j0,f1) };
      slope_face(m, p0[0],p0[1],p0[2],p0[3], axis?(float)sg:0.0f, axis?0.0f:(float)sg,
                 y1-y0, bevel, col); }
    { float p0[4][3] = { P(a1,y0,f0), P(a1,y1,f0), P(i1,j1,f1), P(i1,j0,f1) };
      slope_face(m, p0[0],p0[1],p0[2],p0[3], axis?(float)sg:0.0f, axis?0.0f:(float)sg,
                 y1-y0, bevel, col); }
    { float p0[4][3] = { P(i0,j0,f1), P(i1,j0,f1), P(i1,j1,f1), P(i0,j1,f1) };
      slope_face(m, p0[0],p0[1],p0[2],p0[3], axis?(float)sg:0.0f, axis?0.0f:(float)sg,
                 i1-i0, j1-j0, col); }
    #undef P
}

/* The ledge between the body and the rail above it, closed as an undercut: a
 * band leaning outward from the top of the body up to the rail's own face at the
 * cloth line. Real tables have exactly this — the apron is inset and its top
 * moulding runs out to carry the rail — and without it you look straight in
 * under the rail and see the inside of the table. */
static void rail_undercut(CueVrFrameMesh *m, float ox, float oz, float y_in,
                          const float *col) {
    if (SURR_X - ox < 0.001f && SURR_Z - oz < 0.001f) return;   /* already flush */
    const float y_out = 0.0f;                     /* the base of the rail */
    for (int sg = -1; sg <= 1; sg += 2) {
        { float a[3]={-ox, y_in, (float)sg*oz},   b[3]={ ox, y_in, (float)sg*oz};
          float c[3]={ SURR_X, y_out, (float)sg*SURR_Z}, d[3]={-SURR_X, y_out, (float)sg*SURR_Z};
          slope_face(m, a, b, c, d, 0.0f, (float)sg, ox*2.0f, 0.030f, col); }
        { float a[3]={(float)sg*ox, y_in, -oz},   b[3]={(float)sg*ox, y_in,  oz};
          float c[3]={(float)sg*SURR_X, y_out,  SURR_Z}, d[3]={(float)sg*SURR_X, y_out, -SURR_Z};
          slope_face(m, a, b, c, d, (float)sg, 0.0f, oz*2.0f, 0.030f, col); }
    }
}

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

/* A band running right round the table: four runs of timber rather than one
 * solid slab. Every bead and trim course here was a slab spanning the whole
 * footprint — invisible under the slate, and very visible down a pocket, where
 * it read as the hole having been floored over in wood. */
static void band(CueVrFrameMesh *m, float ox, float oz, float y0, float y1,
                 float thick, int grain, const float *col) {
    float ix = clear_x(ox - thick, y1), iz = clear_z(oz - thick, y1);
    box(m, -ox, y0, -oz, ox, y1, -iz, grain, col);
    box(m, -ox, y0,  iz, ox, y1,  oz, grain, col);
    box(m, -ox, y0, -iz, -ix, y1, iz, 2, col);
    box(m,  ix, y0, -iz,  ox, y1, iz, 2, col);
}

/* A horizontal face as a RING: the piece's own footprint with the tray's mouth
 * taken out of it.
 *
 * This face is under the slate everywhere it exists, so the only place it can
 * ever be looked at is down a pocket — and that is exactly the place it must not
 * be. It used to be cut as a grid with the cells over each pocket dropped, a few
 * hundred quads to make six holes. With the tray under the whole table the six
 * holes are one hole: anything inside the tray's mouth is invisible, so the ring
 * outside it is the entire face that can ever be seen. Four quads. */
static void ring_under(CueVrFrameMesh *m, float y, float x0, float z0,
                       float x1, float z1, const float *n, const float *col) {
    float ix = tray_x(y), iz = tray_z(y);
    float X = x1 > -x0 ? x1 : -x0, Z = z1 > -z0 ? z1 : -z0;
    if (ix > X) ix = X;
    if (iz > Z) iz = Z;
    /* the two end bands, full width */
    { float p0[3]={x0,y,z0},p1[3]={-ix,y,z0},p2[3]={-ix,y,z1},p3[3]={x0,y,z1};
      if (-ix > x0 + 1e-5f) quad(m, p0,p1,p2,p3, n, -ix-x0, z1-z0, col); }
    { float p0[3]={ix,y,z0},p1[3]={x1,y,z0},p2[3]={x1,y,z1},p3[3]={ix,y,z1};
      if (x1 > ix + 1e-5f) quad(m, p0,p1,p2,p3, n, x1-ix, z1-z0, col); }
    /* and the two side bands, between them */
    { float p0[3]={-ix,y,z0},p1[3]={ix,y,z0},p2[3]={ix,y,-iz},p3[3]={-ix,y,-iz};
      if (-iz > z0 + 1e-5f) quad(m, p0,p1,p2,p3, n, 2.0f*ix, -iz-z0, col); }
    { float p0[3]={-ix,y,iz},p1[3]={ix,y,iz},p2[3]={ix,y,z1},p3[3]={-ix,y,z1};
      if (z1 > iz + 1e-5f) quad(m, p0,p1,p2,p3, n, 2.0f*ix, z1-iz, col); }
}

/* A top course of the frame: its four outer faces, and an underside cut back to
 * the tray. No top face — it is under the slate, and the one view that ever
 * reached it was down a pocket. */
static void top_course(CueVrFrameMesh *m,
                       float x0, float y0, float z0, float x1, float y1, float z1,
                       const float *col) {
    float n[3];
    { float a[3]={x0,y0,z0},b[3]={x1,y0,z0},c[3]={x1,y1,z0},d[3]={x0,y1,z0};
      n[0]=0;n[1]=0;n[2]=-1; quad(m,a,b,c,d,n,x1-x0,y1-y0,col); }
    { float a[3]={x0,y0,z1},b[3]={x1,y0,z1},c[3]={x1,y1,z1},d[3]={x0,y1,z1};
      n[0]=0;n[1]=0;n[2]=1;  quad(m,a,b,c,d,n,x1-x0,y1-y0,col); }
    { float a[3]={x0,y0,z0},b[3]={x0,y0,z1},c[3]={x0,y1,z1},d[3]={x0,y1,z0};
      n[0]=-1;n[1]=0;n[2]=0; quad(m,a,b,c,d,n,z1-z0,y1-y0,col); }
    { float a[3]={x1,y0,z0},b[3]={x1,y0,z1},c[3]={x1,y1,z1},d[3]={x1,y1,z0};
      n[0]=1;n[1]=0;n[2]=0;  quad(m,a,b,c,d,n,z1-z0,y1-y0,col); }
    { n[0]=0;n[1]=-1;n[2]=0; ring_under(m, y0, x0, z0, x1, z1, n, col); }
}

/* ---- the tray ------------------------------------------------------------ *
 *
 * ONE opaque black box under the whole table, and it replaces every previous
 * attempt to close the pockets off individually.
 *
 * What is actually open: cue_render bores each pocket through the wood down to
 * the bed (bore_bot), and the void/pouch below it starts at the DROP circle,
 * which is smaller than the bore. That leaves a thin ring around every pocket
 * with nothing behind it at all — you look through it, past the frame, and out
 * of the bottom of the table into the room. The frame's own gaps (the body is
 * narrower than the wood surround above it, and every design leaves the space
 * between its top course and the slate open) are visible through the same ring.
 *
 * Per-pocket liners were tried twice — a cylinder, then a bucket whose top edge
 * followed the arc of the cloth lip. Both had to thread a curved surface between
 * the lip above and the pouch below without touching either, on seven tables and
 * two pocket profiles; both left seams, and together they cost about 7,000
 * vertices. A single tray under everything has nothing to thread between,
 * because it sits below all of it: eight vertices, five quads, and any
 * sight-line that gets past the cloth lands on it.
 *
 * The shape: a rim rectangle at the outer corners of the pocket cut-outs, taken
 * at their base, walls leaning in at 30 degrees off vertical, and a floor below
 * the bottom of the pocket. */
static void black_tray(CueVrFrameMesh *m, const CueTable *t, const CueWorld *w) {
    if (!w || w->npocket == 0) return;
    if (getenv("CUEVR_TRAYVIS")) { TRAY[0]=0.05f; TRAY[1]=0.35f; TRAY[2]=1.0f; }

    /* Its rim, its slope and its floor are tray_measure()'s — the same numbers
     * every design keeps out of, so the tray and the woodwork can never disagree
     * about where the tray is. */
    (void)t;
    const float rim_x = TRAY_RX, rim_z = TRAY_RZ;
    const float rim_y = TRAY_RIM_Y, bot = TRAY_BOT;
    const float drop  = rim_y - bot;
    const float inset = drop * TRAY_SLOPE;
    const float fx = rim_x - inset, fz = rim_z - inset;

    /* Four walls. The normal faces up and inward — this is the inside of a
     * funnel, and inside is the only side it is ever seen from. */
    const float nl = sqrtf(drop*drop + inset*inset);
    const float nh = drop / nl, nv = inset / nl;
    struct { float sx, sz; } side[4] = { {1,0}, {-1,0}, {0,1}, {0,-1} };
    for (int s = 0; s < 4; s++) {
        float sx = side[s].sx, sz = side[s].sz;
        float n[3] = { -sx * nh, nv, -sz * nh };
        float p0[3], p1[3], p2[3], p3[3];
        if (sx != 0.0f) {                        /* a wall at x = +/- rim_x */
            p0[0]=sx*rim_x; p0[1]=rim_y; p0[2]=-rim_z;
            p1[0]=sx*rim_x; p1[1]=rim_y; p1[2]= rim_z;
            p2[0]=sx*fx;    p2[1]=bot;   p2[2]= fz;
            p3[0]=sx*fx;    p3[1]=bot;   p3[2]=-fz;
        } else {                                 /* a wall at z = +/- rim_z */
            p0[0]=-rim_x; p0[1]=rim_y; p0[2]=sz*rim_z;
            p1[0]= rim_x; p1[1]=rim_y; p1[2]=sz*rim_z;
            p2[0]= fx;    p2[1]=bot;   p2[2]=sz*fz;
            p3[0]=-fx;    p3[1]=bot;   p3[2]=sz*fz;
        }
        quad(m, p0, p1, p2, p3, n, 0.05f, 0.05f, TRAY);
    }
    /* and the floor */
    { float n[3] = { 0.0f, 1.0f, 0.0f };
      float p0[3]={-fx,bot,-fz}, p1[3]={fx,bot,-fz};
      float p2[3]={ fx,bot, fz}, p3[3]={-fx,bot, fz};
      quad(m, p0, p1, p2, p3, n, 0.05f, 0.05f, TRAY); }
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

/* A chamfered, tapered post whose foot is somewhere other than under its head:
 * the leg of a pub table rakes outward, and that splay is most of what the
 * silhouette is. `post()` below is this with the foot under the head. */
static void raked_post(CueVrFrameMesh *m, float cx, float cz, float bx_, float bz_,
                       float y_top, float y_bot,
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
        float c[3] = { bx_ + bx[j], y_bot, bz_ + bz[j] };
        float d[3] = { bx_ + bx[i], y_bot, bz_ + bz[i] };
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

static void post(CueVrFrameMesh *m, float cx, float cz, float y_top, float y_bot,
                 float w_top, float w_bot, float chamfer, const float *col) {
    raked_post(m, cx, cz, cx, cz, y_top, y_bot, w_top, w_bot, chamfer, col);
}

static void regency(CueVrFrameMesh *m, const CueTable *t) {
    const float hl = t->half_len, hw = t->half_wid;
    (void)hw;

    /* Out to the rail above, less the 10 mm this design's moulding oversails,
     * so the moulding is the widest thing and lands flush with the rail. */
    float ox, oz; body_box(t, 0.010f, &ox, &oz);

    /* Apron. Deep enough to read as a cabinet, under a proper crown. */
    const float ap_top   = -0.004f;                  /* just under the bed */
    const float ap_h     = 0.180f;
    const float ap_bot   = ap_top - ap_h;
    const float oversail = 0.016f;                   /* the crown's overhang */

    /* The crown: fascia, then a quarter-round ovolo easing back to the apron,
     * struck with a quirk — the recessed slot that draws a shadow line under
     * every real moulding. It replaces an oversailing BOX, which read as
     * exactly that. The fascia keeps top_course for its under-slate closure. */
    top_course(m, -ox - oversail, ap_top - 0.012f, -oz - oversail,
                   ox + oversail, ap_top - 0.004f,  oz + oversail, PAL_LIT);
    rail_undercut(m, ox + oversail, oz + oversail, ap_top - 0.004f, PAL_LIT);
    {
        const float crown[][2] = {
            { oversail,          ap_top - 0.012f },
            { oversail,          ap_top - 0.019f },   /* fascia, flat */
            { oversail * 0.92f,  ap_top - 0.0245f },  /* the ovolo, four cuts */
            { oversail * 0.71f,  ap_top - 0.0295f },
            { oversail * 0.38f,  ap_top - 0.0330f },
            { 0.000f,            ap_top - 0.0345f },
            { -0.0025f,          ap_top - 0.0345f },  /* quirk in */
            { -0.0025f,          ap_top - 0.0395f },  /* the shadow slot */
            { 0.000f,            ap_top - 0.0415f },  /* ease back to the face */
        };
        moulding(m, ox, oz, crown, 9, PAL_WOOD);
    }

    /* The apron proper, in four runs so the grain follows each length. The inner
     * faces are pulled out to clear the tray where the two would have met — on a
     * wide-railed table they never do and the apron keeps its drawn thickness. */
    const float ap_y = ap_top - 0.0415f;                 /* the apron's own top */
    const float iz_a = clear_z(oz - 0.026f, ap_y);
    const float ix_a = clear_x(ox - 0.026f, ap_y);
    box(m, -ox, ap_bot, -oz, ox, ap_y, -iz_a, 0, PAL_WOOD);
    box(m, -ox, ap_bot,  iz_a, ox, ap_y, oz, 0, PAL_WOOD);
    box(m, -ox, ap_bot, -iz_a, -ix_a, ap_y, iz_a, 2, PAL_WOOD);
    box(m,  ix_a, ap_bot, -iz_a, ox, ap_y, iz_a, 2, PAL_WOOD);
    /* and a struck bead as its bottom edge: a half-round that catches the light
     * along its crown, in place of the dark painted stripe that was here */
    {
        const float by = ap_bot + 0.016f;
        const float bead[][2] = {
            { 0.000f,  by + 0.006f },
            { -0.002f, by + 0.005f },                  /* quirk above */
            { 0.004f,  by + 0.002f },                  /* the round */
            { 0.005f,  by - 0.001f },
            { 0.004f,  by - 0.004f },
            { -0.002f, by - 0.007f },                  /* quirk below */
            { 0.000f,  by - 0.008f },
        };
        moulding(m, ox, oz, bead, 7, PAL_WOOD);
    }

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

    float legx[4];
    for (int p = 0; p < pairs; p++) {
        float fx = (pairs == 1) ? 0.0f
                 : (-1.0f + 2.0f * (float)p / (float)(pairs - 1));
        float cx = fx * (ox - inset - lw_top * 0.5f);
        legx[p] = cx;
        for (int sz = -1; sz <= 1; sz += 2) {
            float cz = (float)sz * (oz - inset - lw_top * 0.5f);
            /* capital: a squarer, wider block where the leg meets the apron,
             * with a collar struck beneath it — the necking every real leg has
             * where the square work meets the shaft. */
            box(m, cx - lw_top*0.5f - 0.011f, leg_top - cap_h, cz - lw_top*0.5f - 0.011f,
                   cx + lw_top*0.5f + 0.011f, leg_top,          cz + lw_top*0.5f + 0.011f,
                   1, PAL_LIT);
            post(m, cx, cz, leg_top - cap_h, leg_top - cap_h - 0.012f,
                 lw_top + 0.018f, lw_top + 0.004f, 0.036f, PAL_LIT);
            /* The shaft — in the BODY'S OWN TIMBER. It was PAL_DARK, and a
             * near-black leg under a warm apron read as two different pieces
             * of furniture; worse, the dark hid the octagon completely, so the
             * one shaped thing on the table photographed as a plain box. The
             * facets shade themselves now that they can be seen. */
            post(m, cx, cz, leg_top - cap_h - 0.012f, floor_y + plinth_h,
                 lw_top, lw_bot, 0.034f, PAL_WOOD);
            /* an eased ankle into the plinth, then the foot and its brass shoe */
            post(m, cx, cz, floor_y + plinth_h + 0.016f, floor_y + plinth_h,
                 lw_bot + 0.002f, lw_bot + 0.016f, 0.030f, PAL_WOOD);
            box(m, cx - lw_bot*0.5f - 0.010f, floor_y + 0.012f, cz - lw_bot*0.5f - 0.010f,
                   cx + lw_bot*0.5f + 0.010f, floor_y + plinth_h, cz + lw_bot*0.5f + 0.010f,
                   1, PAL_WOOD);
            box(m, cx - lw_bot*0.5f - 0.012f, floor_y, cz - lw_bot*0.5f - 0.012f,
                   cx + lw_bot*0.5f + 0.012f, floor_y + 0.012f, cz + lw_bot*0.5f + 0.012f,
                   1, BRASS);
        }
    }

    /* Fielded panels along the apron, one to each bay between the legs and one
     * to each end — the joinery that says CABINET MAKER rather than box. The
     * field stands seven millimetres proud behind a bevel; the bevel's top edge
     * takes the lamps and its bottom shades itself, so the panel reads by light
     * alone, at any distance, in the body's own timber. */
    {
        const float caphw = lw_top * 0.5f + 0.013f;
        const float pm    = 0.055f;                    /* margin from the capitals */
        const float py0   = ap_bot + 0.034f;
        const float py1   = ap_y - 0.020f;
        for (int p = 0; p + 1 < pairs; p++) {
            float a0 = legx[p] + caphw + pm, a1 = legx[p + 1] - caphw - pm;
            for (int sg = -1; sg <= 1; sg += 2) {
                fielded_panel(m, 0, (float)sg, oz, a0, a1, py0, py1,
                              0.018f, 0.007f, PAL_WOOD);
            }
        }
        float ez = oz - inset - lw_top * 0.5f;
        for (int sg = -1; sg <= 1; sg += 2)
            fielded_panel(m, 1, (float)sg, ox, -(ez - caphw - pm), ez - caphw - pm,
                          py0, py1, 0.018f, 0.007f, PAL_WOOD);
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
    const float hw = t->half_wid;
    /* A pub table's body has no moulding proud of the rail — the flank IS the
     * widest thing, so it goes right out to the rail's own face. */
    float ox, oz; body_box(t, 0.0f, &ox, &oz);
    const float top     = 0.0f;               /* flush with the base of the rail */
    const float floor_y = -cuevr_frame_depth(t);
    /* HALF the drop from the cushion to the floor, which is what the reference
     * photograph actually measures: a Supreme-pattern body is a slab about as
     * deep as the legs are long. Guessing it by eye gave a deep chest on stubs
     * twice running. */
    const float leg_h   = cuevr_frame_depth(t) * 0.50f;
    const float body_bot = floor_y + leg_h;
    /* Barely any. The body of a Supreme-pattern table is a slab with square
     * ends; the splay that reads from across the room is in the LEGS, and
     * putting it in the body instead — 78 mm a side, as this had — gives you a
     * plant pot. 18 mm is what the mouldings actually take up. */
    const float TAPER   = 0.018f;
    const float y_a = top - 0.006f;
    const float H   = y_a - body_bot;
    #define CAB_IN(y)  (TAPER * ((y_a) - (y)) / (H))

    /* A polished strip right under the rail and another along the foot: the two
     * bright lines that separate the black body from everything above and below
     * it, and the first thing you see in a photograph of one of these. */
    const float y_b = y_a - 0.026f;              /* bottom of the top chrome band */
    const float y_c = body_bot + 0.030f;         /* top of the foot chrome band */
    rail_undercut(m, ox, oz, y_a, CAB_CHROME);
    frustum_band(m, y_a, y_b, ox, oz,
                 ox - CAB_IN(y_b), oz - CAB_IN(y_b), CAB_CHROME);
    frustum_band(m, y_b, y_c, ox - CAB_IN(y_b), oz - CAB_IN(y_b),
                 ox - CAB_IN(y_c), oz - CAB_IN(y_c), CAB_BODY);
    frustum_band(m, y_c, body_bot, ox - CAB_IN(y_c), oz - CAB_IN(y_c),
                 ox - TAPER, oz - TAPER, CAB_CHROME);
    /* the underside, so there is no hole when you stoop for a shot */
    box(m, -(ox - TAPER), body_bot - 0.012f, -(oz - TAPER),
           ox - TAPER, body_bot, oz - TAPER, 0, SHADOW);

    /* Chamfered corner posts with a bright quirk down them. On the real table
     * the corner is a casting rather than a mitre, and the break of light down
     * that chamfer is what stops the body reading as a plain black box. The
     * casting is collared in chrome at the head AND the foot — a casting is
     * fixed at both ends, and the matching foot collar is what ties the bright
     * line along the plinth into the corners instead of dying at them. */
    /* The castings start BELOW THE TRAY'S FLOOR, full stop. The first fix
     * dropped them to where the funnel wall crossed their chamfer — measured
     * through the bore with CUEVR_TRAYVIS — and the sliver survived it,
     * because a corner post is not one facet: it is a block standing 127 mm
     * inside the rim rectangle, and everything inside the rim above the
     * tray's floor is inside the visible cone of some sight-line down the
     * bore. There is no partial height that works. Below TRAY_BOT the floor
     * is opaque and hides everything; outside, the casting reads as applied
     * to the corner under the body bands, which is how the real one is
     * fixed to the box anyway. */
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sz = -1; sz <= 1; sz += 2) {
            float cx = (float)sx * (ox - 0.052f), cz = (float)sz * (oz - 0.052f);
            float ctop = TRAY_BOT - 0.002f;
            post(m, cx, cz, ctop, body_bot,
                 0.150f, 0.150f - TAPER * 2.0f, 0.062f, CAB_PANEL);
            post(m, cx, cz, ctop, ctop - 0.020f, 0.152f, 0.152f, 0.063f, CAB_CHROME);
            post(m, cx, cz, y_c, body_bot + 0.002f,
                 0.150f - TAPER * 1.6f, 0.150f - TAPER * 2.0f, 0.062f, CAB_CHROME);
        }

    /* Coin mech at the foot end — a recessed plate with a chromed surround,
     * which is the one piece of clutter a pub table always has. The bezel now
     * goes right round: it had a top and a bottom strip and open sides, which
     * is not how anything is riveted to anything. */
    {
        float ymid = (y_b + y_c) * 0.5f + 0.045f;
        float xf = ox - CAB_IN(ymid);
        box(m, xf - 0.004f, ymid - 0.055f, -hw * 0.16f,
               xf + 0.008f, ymid + 0.055f,  hw * 0.16f, 2, CAB_PANEL);
        box(m, xf - 0.002f, ymid - 0.062f, -hw * 0.18f,
               xf + 0.010f, ymid - 0.055f,  hw * 0.18f, 2, CAB_CHROME);
        box(m, xf - 0.002f, ymid + 0.055f, -hw * 0.18f,
               xf + 0.010f, ymid + 0.062f,  hw * 0.18f, 2, CAB_CHROME);
        box(m, xf - 0.002f, ymid - 0.062f, -hw * 0.18f - 0.007f,
               xf + 0.010f, ymid + 0.062f, -hw * 0.18f, 2, CAB_CHROME);
        box(m, xf - 0.002f, ymid - 0.062f,  hw * 0.18f,
               xf + 0.010f, ymid + 0.062f,  hw * 0.18f + 0.007f, 2, CAB_CHROME);
        /* and the slot itself: the one detail everyone who has fed one of
         * these tables looks for */
        box(m, xf + 0.008f, ymid - 0.030f, -0.012f,
               xf + 0.0105f, ymid + 0.030f, 0.012f, 1, CAB_CHROME);

        /* The ball-return hatch, LOW and central under the coin door: a wide
         * recessed tray mouth with a chrome sill. This is where the game gives
         * the balls back, it is the biggest single feature of the real body,
         * and the table did not have one — a coin slot that never pays out. */
        float hy1 = y_c + 0.085f;
        float hx  = ox - CAB_IN((y_c + hy1) * 0.5f);
        box(m, hx - 0.010f, y_c + 0.006f, -hw * 0.26f,
               hx + 0.004f, hy1,           hw * 0.26f, 2, SHADOW);
        box(m, hx - 0.002f, y_c + 0.006f, -hw * 0.26f - 0.007f,
               hx + 0.008f, hy1 + 0.007f, -hw * 0.26f, 2, CAB_CHROME);
        box(m, hx - 0.002f, y_c + 0.006f,  hw * 0.26f,
               hx + 0.008f, hy1 + 0.007f,  hw * 0.26f + 0.007f, 2, CAB_CHROME);
        box(m, hx - 0.002f, hy1, -hw * 0.26f - 0.007f,
               hx + 0.008f, hy1 + 0.007f, hw * 0.26f + 0.007f, 2, CAB_CHROME);
        /* the sill the balls roll onto */
        box(m, hx - 0.002f, y_c, -hw * 0.26f - 0.007f,
               hx + 0.014f, y_c + 0.006f, hw * 0.26f + 0.007f, 2, CAB_CHROME);
    }

    /* Legs. RAKED: square in section, set well in from the corners at the top
     * and splaying out and forward to land almost under them, on a chromed
     * shoe. Vertical stubs under the corners — which is what these were — read
     * as a box on blocks; the rake is the whole stance of the table. */
    /* Heavy. These carry a slate bed and they look it: near enough a hand's
     * breadth square at the head, tapering by a third to the shoe. */
    const float lw   = 0.190f;
    const float in_x = 0.130f;                 /* how far in the leg head sits */
    const float in_z = 0.075f;
    const float rake = 0.105f;                 /* how far the foot travels out */
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sz = -1; sz <= 1; sz += 2) {
            float hx = (float)sx * (ox - TAPER - in_x);
            float hz = (float)sz * (oz - TAPER - in_z);
            float fx = hx + (float)sx * rake;
            float fz = hz + (float)sz * rake * 0.55f;
            raked_post(m, hx, hz, fx, fz, body_bot, floor_y + 0.026f,
                       lw, lw * 0.66f, lw * 0.10f, CAB_BODY);
            box(m, fx - lw * 0.36f, floor_y, fz - lw * 0.36f,
                   fx + lw * 0.36f, floor_y + 0.026f, fz + lw * 0.36f,
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
    /* Twelve. At ten the vase read as a polygon before it read as a vase —
     * the swelling is the widest, best-lit thing on the leg, which makes it
     * exactly where the facets are most visible. */
    const int SIDES = 12;
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

/* The baluster, rewritten against how turned work is actually cut.
 *
 * The old profile had three hidden jumps — the vase ended at 0.070 where the
 * beads began at 0.058, the beads ended where the taper began 4 mm wider, and
 * the sampling smeared each jump across one station into a mush that read as
 * nothing. On a lathe those steps are FILLETS: deliberate flat collars a tool
 * is squared into, and they are half of what makes turning look like turning —
 * every curve lands on a crisp flat before the next one starts.
 *
 * So this one is built of features that MEET: a collar under the pad, an
 * astragal (a full round bead) riding on it, the vase swelling wide and
 * sweeping in to a narrow neck, three crisp beads separated by real flats, a
 * long taper with a touch of entasis (dead-straight tapers look hollow at
 * distance — every column ever cut bows a hair for exactly this reason), an
 * ankle collar, and a bun foot. `f` is 0 at the apron, 1 at the floor. */
static float baluster(float f) {
    /* a smooth 0..1 ease, for the sweeps */
    #define EASE(u) (0.5f - 0.5f * cosf((u) * 3.14159265f))
    if (f < 0.030f) return 0.098f;                        /* collar under the pad */
    if (f < 0.075f) {                                     /* astragal: full round */
        float u = (f - 0.030f) / 0.045f;
        return 0.098f + 0.014f * sinf(u * 3.14159265f);
    }
    if (f < 0.100f) return 0.096f;                        /* fillet into the vase */
    if (f < 0.175f) {                                     /* the vase swells... */
        float u = (f - 0.100f) / 0.075f;
        return 0.096f + 0.018f * EASE(u);
    }
    if (f < 0.360f) {                                     /* ...and sweeps to the neck */
        float u = (f - 0.175f) / 0.185f;
        return 0.114f - 0.056f * EASE(u);
    }
    if (f < 0.385f) return 0.058f;                        /* neck fillet */
    if (f < 0.475f) {                                     /* three beads, real flats */
        float u = (f - 0.385f) / 0.090f;
        float b = fmodf(u * 3.0f, 1.0f);
        /* each bead: flat 15%, round 70%, flat 15% */
        if (b < 0.15f || b > 0.85f) return 0.058f;
        return 0.058f + 0.013f * sinf((b - 0.15f) / 0.70f * 3.14159265f);
    }
    if (f < 0.500f) return 0.058f;                        /* fillet out of the beads */
    if (f < 0.870f) {                                     /* the taper, with entasis */
        float u = (f - 0.500f) / 0.370f;
        return 0.056f - 0.018f * u + 0.004f * sinf(u * 3.14159265f);
    }
    if (f < 0.895f) return 0.038f;                        /* ankle collar */
    if (f < 0.975f) {                                     /* the bun foot */
        float u = (f - 0.895f) / 0.080f;
        return 0.040f + 0.022f * sinf(u * 3.14159265f * 0.82f);
    }
    return 0.044f;                                        /* the shoe */
    #undef EASE
}

static void victorian(CueVrFrameMesh *m, const CueTable *t) {
    const float hl = t->half_len, hw = t->half_wid;
    (void)hw;
    /* less the 16 mm the ovolo oversails */
    float ox, oz; body_box(t, 0.016f, &ox, &oz);
    const float ap_top = -0.004f;
    const float ap_h   = 0.205f;                  /* deeper than the Regency */
    const float ap_bot = ap_top - ap_h;
    const float floor_y = -cuevr_frame_depth(t);

    /* An ogee crown that oversails properly — this is a heavier table than the
     * Regency and the mouldings are correspondingly bolder. The S-curve is the
     * whole Victorian signature: it catches the lamps along its swell and
     * shades its own throat, where the old stacked boxes just changed paint. */
    top_course(m, -ox - 0.018f, ap_top - 0.014f, -oz - 0.018f,
                   ox + 0.018f, ap_top - 0.006f,  oz + 0.018f, PAL_LIT);
    rail_undercut(m, ox + 0.018f, oz + 0.018f, ap_top - 0.006f, PAL_LIT);
    {
        const float ogee[][2] = {
            { 0.018f,  ap_top - 0.014f },
            { 0.018f,  ap_top - 0.020f },              /* fascia */
            { 0.0155f, ap_top - 0.027f },              /* the swell out... */
            { 0.0105f, ap_top - 0.032f },
            { 0.0045f, ap_top - 0.036f },              /* ...through the waist... */
            { 0.0035f, ap_top - 0.041f },
            { 0.0075f, ap_top - 0.0455f },             /* ...into the cove */
            { 0.009f,  ap_top - 0.049f },
            { -0.0025f, ap_top - 0.050f },             /* quirk */
            { 0.000f,  ap_top - 0.054f },
        };
        moulding(m, ox, oz, ogee, 10, PAL_WOOD);
    }

    const float ap_y = ap_top - 0.054f;
    const float iz_a = clear_z(oz - 0.028f, ap_y);
    const float ix_a = clear_x(ox - 0.028f, ap_y);
    box(m, -ox, ap_bot, -oz, ox, ap_y, -iz_a, 0, PAL_WOOD);
    box(m, -ox, ap_bot,  iz_a, ox, ap_y, oz, 0, PAL_WOOD);
    box(m, -ox, ap_bot, -iz_a, -ix_a, ap_y, iz_a, 2, PAL_WOOD);
    box(m,  ix_a, ap_bot, -iz_a, ox, ap_y, iz_a, 2, PAL_WOOD);
    /* a struck half-round low on the apron where the carved bead sat as a dark
     * stripe, and a bolder torus as the bottom edge */
    {
        const float by = ap_bot + 0.036f;
        const float bead[][2] = {
            { 0.000f,  by + 0.007f },
            { -0.002f, by + 0.006f },
            { 0.005f,  by + 0.002f },
            { 0.006f,  by - 0.002f },
            { 0.005f,  by - 0.005f },
            { -0.002f, by - 0.008f },
            { 0.000f,  by - 0.009f },
        };
        moulding(m, ox, oz, bead, 7, PAL_WOOD);
        const float torus[][2] = {
            { 0.000f,  ap_bot + 0.017f },
            { 0.006f,  ap_bot + 0.014f },
            { 0.009f,  ap_bot + 0.009f },
            { 0.009f,  ap_bot + 0.004f },
            { 0.006f,  ap_bot + 0.001f },
            { 0.000f,  ap_bot },
        };
        moulding(m, ox, oz, torus, 6, PAL_WOOD);
    }

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
            /* 96 stations, not 40. A bead is 18 mm of a 600 mm leg; at 40
             * uniform stations that is barely one sample, and the whole bead
             * run melted into the shaft — twice, because the first profile was
             * blamed before the sampling was. */
            turned(m, cx, cz, leg_top - 0.052f, floor_y + 0.010f, baluster, 96, PAL_WOOD);
            box(m, cx - 0.052f, floor_y, cz - 0.052f,
                   cx + 0.052f, floor_y + 0.010f, cz + 0.052f, 1, BRASS);
        }
    }
}

/* ---- American: the 9 ft tournament table -------------------------------- *
 *
 * Built against the user's reference photograph (a Dominator-pattern
 * competition table), read feature by feature this time rather than from
 * memory of it — the first attempt hung black wedges off the skirt corners
 * and was rightly rejected:
 *
 *   - a TALL dark fascia under the cloth line, leaning outward as it drops,
 *     which is the band that makes the rail look a hand deep;
 *   - smooth BLACK POCKET CASTINGS riding on that fascia at all six pockets —
 *     they are the black you see at every corner of the photo, and they are
 *     rail furniture, not legs;
 *   - a skirt in the body timber filling only the UPPER half of the drop;
 *   - big near-vertical columns set well inboard of the ends, open air all
 *     around them, on round black adjustable feet;
 *   - the ball-return slot across the head end, full width, with the shorter
 *     stepped tray under it.
 *
 * The corner castings are built as an L of two boxes, one lying along each
 * rail face, each strictly OUTSIDE its own face's tray rim — the cabinet's
 * pocket-sliver lesson, applied at design time: any solid crossing inside the
 * rim rectangle above the tray floor shows through the bore, and no partial
 * height fixes it. An L never crosses in both axes at once, so it can never
 * appear down the pocket it is wrapped around. */

/* Matte black that still shades (mode 12 is N.L — void-black would swallow
 * the castings' own form), and the fascia's gunmetal a shade above it. */
static const float INK[3]    = { 0.055f, 0.058f, 0.064f };
static const float GUNML[3]  = { 0.085f, 0.089f, 0.098f };

/* The adjustable foot: a fat black disc under a short ankle, the one piece of
 * hardware every competition table shows off rather than hides. */
static float foot_disc(float f) {
    if (f < 0.34f) return 0.050f;                       /* the ankle */
    if (f < 0.52f) return 0.050f + 0.042f * (f - 0.34f) / 0.18f;
    if (f < 0.90f) return 0.092f;                       /* the disc */
    return 0.080f;                                      /* under-bevel */
}

static void american(CueVrFrameMesh *m, const CueTable *t) {
    const float hl = t->half_len, hw = t->half_wid;
    (void)hw;
    float ox, oz; body_box(t, 0.0f, &ox, &oz);
    const float top      = -0.004f;
    const float floor_y  = -cuevr_frame_depth(t);
    const float fas_h    = 0.105f;               /* the fascia's drop */
    const float fas_out  = 0.020f;               /* how far it leans out */
    const float fas_bot  = top - fas_h;
    const float skirt_bot = top - 0.295f;        /* the wood stops here; open below */

    /* The fascia: one tall band leaning outward as it drops. */
    rail_undercut(m, ox, oz, top, GUNML);
    frustum_band(m, top, fas_bot, ox, oz, ox + fas_out, oz + fas_out, GUNML);

    /* The skirt: the player's timber, upper half of the drop only, tucked
     * back under the fascia's bottom edge. */
    {
        const float in = 0.012f;                 /* set back from the fascia lip */
        const float sx = ox + fas_out - in, sz = oz + fas_out - in;
        const float iz_s = clear_z(sz - 0.030f, fas_bot);
        const float ix_s = clear_x(sx - 0.030f, fas_bot);
        box(m, -sx, skirt_bot, -sz, sx, fas_bot, -iz_s, 0, PAL_WOOD);
        box(m, -sx, skirt_bot,  iz_s, sx, fas_bot, sz, 0, PAL_WOOD);
        box(m, -sx, skirt_bot, -iz_s, -ix_s, fas_bot, iz_s, 2, PAL_WOOD);
        box(m,  ix_s, skirt_bot, -iz_s, sx, fas_bot, iz_s, 2, PAL_WOOD);
        /* its underside, so stooping for a shot shows a shadow, not a void */
        box(m, -(sx - 0.02f), skirt_bot - 0.012f, -(sz - 0.02f),
               sx - 0.02f, skirt_bot, sz - 0.02f, 0, SHADOW);
    }

    /* The pocket castings.
     *
     * Corners: an L wrapping each corner, proud of the fascia, running from
     * just under the rail down past the fascia onto the skirt. Each arm keeps
     * outside ITS face's tray rim, so nothing can show down the bore. */
    {
        const float c_top = -0.002f, c_bot = fas_bot - 0.030f;
        const float arm   = 0.150f;              /* how far along each face */
        const float pr    = fas_out + 0.014f;    /* proud of the fascia lip */
        /* Each arm's INNER face sits on the tray's own rim, read straight off
         * TRAY_RX/RZ rather than derived from the body and hoped about. The
         * end arm is outside the rim in x everywhere, the side arm in z, and
         * a point is only visible down a bore when it is inside BOTH. */
        const float ixn = TRAY_RX + 0.003f;
        const float izn = TRAY_RZ + 0.003f;
        for (int cs = 0; cs < 4; cs++) {
            float X = (cs & 1) ? -1.0f : 1.0f;
            float Z = (cs & 2) ? -1.0f : 1.0f;
            /* the arm along the END face: thin in x, running `arm` along z */
            {
                float x0 = ixn,      x1 = ox + pr;
                float z0 = oz - arm, z1 = oz + pr;
                box(m, X > 0 ? x0 : -x1, c_bot, Z > 0 ? z0 : -z1,
                       X > 0 ? x1 : -x0, c_top, Z > 0 ? z1 : -z0, 1, INK);
            }
            /* the arm along the SIDE face: thin in z, running `arm` along x */
            {
                float x0 = ox - arm, x1 = ox + pr;
                float z0 = izn,      z1 = oz + pr;
                box(m, X > 0 ? x0 : -x1, c_bot, Z > 0 ? z0 : -z1,
                       X > 0 ? x1 : -x0, c_top, Z > 0 ? z1 : -z0, 0, INK);
            }
        }
    }

    /* Middles: a small black cap on each long face at the pocket. */
    if (WRLD) for (int k = 4; k < WRLD->npocket; k++) {
        float px = WRLD->pocket[k].x;
        float sgz = WRLD->pocket[k].z > 0.0f ? 1.0f : -1.0f;
        float c_top = -0.002f, c_bot = fas_bot + 0.012f;
        float z0 = TRAY_RZ + 0.003f, z1 = oz + fas_out + 0.014f;
        box(m, px - 0.105f, c_bot, sgz > 0 ? z0 : -z1,
               px + 0.105f, c_top, sgz > 0 ? z1 : -z0, 0, INK);
    }

    /* The legs — read off the photograph properly at the third asking: the
     * table does NOT stand on four columns. Each end has ONE pedestal, a
     * slab spanning most of the width with a tall ARCH cut out of its
     * middle, leaving two limbs that curve down onto round black feet. Four
     * feet, two legs. The arch is the silhouette: the open air UNDER the
     * middle of each pedestal is what makes the stance.
     *
     * Built as vertical strips across the width whose bottom edges follow
     * the arch curve — twenty-two strips read as a curve at any distance a
     * table is seen from, and the strip bottoms give the arch its stepped
     * soffit, which the real pressed panel has too. */
    int pairs = (hl * 2.0f > 2.9f) ? 3 : 2;
    {
        /* The arch is CUT, not stacked. The first version built it of vertical
         * strips whose box bottoms stepped along the curve, and the verdict
         * was exact: a child's lego, on a premium table. A pressed pedestal
         * has three surfaces and all of them are smooth — the two faces, cut
         * to the curve; and the soffit, a band sweeping under the arch whose
         * normals turn with it and catch the light the way the real pressing
         * does. So that is what is built: face quads whose lower edge IS the
         * curve, and a swept soffit, at forty segments. */
        const float thk  = 0.135f;               /* the slab, through the length */
        const float W    = oz - 0.100f;          /* pedestal half-width */
        const float A    = W * 0.76f;            /* the arch's half-span */
        const float foot_h = 0.058f;
        const float ped_bot = floor_y + foot_h;
        const float ped_top = skirt_bot + 0.020f;
        const float arch_top = floor_y + 0.50f;
        const int   NSEG = 40;
        for (int p = 0; p < pairs; p++) {
            float fx = (pairs == 1) ? 0.0f
                     : (-1.0f + 2.0f * (float)p / (float)(pairs - 1));
            float cx = fx * (ox - 0.270f);
            float xf = cx + thk * 0.5f, xb = cx - thk * 0.5f;

            /* the lower boundary across the whole width */
            float zs[NSEG + 1], ys[NSEG + 1];
            for (int k = 0; k <= NSEG; k++) {
                float z = -W + 2.0f * W * (float)k / NSEG;
                float y = ped_bot;
                if (fabsf(z) < A) {
                    float u = z / A;
                    y = ped_bot + (arch_top - ped_bot) * sqrtf(1.0f - u * u);
                }
                zs[k] = z; ys[k] = y;
            }
            for (int k = 0; k < NSEG; k++) {
                /* front and back faces: top edge straight, bottom edge on the
                 * curve — a quad may be a trapezoid, and these are */
                { float n[3] = { 1, 0, 0 };
                  float a0[3]={xf,ys[k],zs[k]},   b0[3]={xf,ys[k+1],zs[k+1]};
                  float c0[3]={xf,ped_top,zs[k+1]}, d0[3]={xf,ped_top,zs[k]};
                  quad(m, a0,b0,c0,d0, n, zs[k+1]-zs[k], ped_top-ys[k], PAL_WOOD); }
                { float n[3] = { -1, 0, 0 };
                  float a0[3]={xb,ys[k],zs[k]},   b0[3]={xb,ys[k+1],zs[k+1]};
                  float c0[3]={xb,ped_top,zs[k+1]}, d0[3]={xb,ped_top,zs[k]};
                  quad(m, a0,b0,c0,d0, n, zs[k+1]-zs[k], ped_top-ys[k], PAL_WOOD); }
                /* the soffit: a band under the curve, normal turning with it */
                {
                  float dz = zs[k+1]-zs[k], dy = ys[k+1]-ys[k];
                  float l = sqrtf(dz*dz + dy*dy);
                  if (l > 1e-6f) {
                    float n[3] = { 0, -dz / l, dy / l };
                    float a0[3]={xf,ys[k],zs[k]},   b0[3]={xf,ys[k+1],zs[k+1]};
                    float c0[3]={xb,ys[k+1],zs[k+1]}, d0[3]={xb,ys[k],zs[k]};
                    quad(m, a0,b0,c0,d0, n, l, thk, PAL_WOOD);
                  }
                }
            }
            /* the outer side faces */
            for (int sgz = -1; sgz <= 1; sgz += 2) {
                float z = (float)sgz * W;
                float n[3] = { 0, 0, (float)sgz };
                float a0[3]={xb,ped_bot,z}, b0[3]={xf,ped_bot,z};
                float c0[3]={xf,ped_top,z}, d0[3]={xb,ped_top,z};
                quad(m, a0,b0,c0,d0, n, ped_top-ped_bot, thk, PAL_WOOD);
            }
            /* the two feet, under the limbs */
            float zf = (A + W) * 0.5f;
            turned(m, cx,  zf, floor_y + foot_h + 0.002f, floor_y, foot_disc, 10, INK);
            turned(m, cx, -zf, floor_y + foot_h + 0.002f, floor_y, foot_disc, 10, INK);
        }
    }

    /* The ball return, across the head end: the full-width slot just under
     * the fascia, and the shorter stepped tray below it, exactly the two
     * black horizontals of the reference. */
    {
        const float sx = ox + fas_out - 0.012f;  /* the skirt's face */
        const float zr = oz - 0.190f;            /* between the corner castings */
        box(m, sx - 0.006f, fas_bot - 0.040f, -zr, sx + 0.006f, fas_bot - 0.010f, zr, 2, SHADOW);
        box(m, sx + 0.002f, fas_bot - 0.010f, -zr - 0.012f,
               sx + 0.018f, fas_bot,           zr + 0.012f, 2, INK);   /* top lip */
        box(m, sx + 0.002f, fas_bot - 0.052f, -zr - 0.012f,
               sx + 0.018f, fas_bot - 0.040f,  zr + 0.012f, 2, INK);   /* the sill */
        /* the tray: lower, shorter, one side, stepped out a little further */
        box(m, sx - 0.004f, fas_bot - 0.118f, -zr, sx + 0.010f, fas_bot - 0.072f, 0.0f, 2, SHADOW);
        box(m, sx + 0.006f, fas_bot - 0.072f, -zr - 0.010f,
               sx + 0.026f, fas_bot - 0.058f,  0.010f, 2, INK);
        box(m, sx + 0.006f, fas_bot - 0.130f, -zr - 0.010f,
               sx + 0.026f, fas_bot - 0.118f,  0.010f, 2, INK);
    }
}

/* ---- the arena ------------------------------------------------------------ *
 *
 * The Crucible, near enough: the theatre snooker is played in is an intimate
 * octagonal bowl — a dark carpeted floor, a waist-high barrier with a red rail
 * a few strides from the table, banks of close-set seats rising steeply on
 * every side but the players' entrance, and above it all a black ceiling
 * carrying hundreds of small lamps, which is the single most recognisable
 * thing about the room: a night sky indoors.
 *
 * Built from the user's two reference photographs of the house. ARENA-LOCAL
 * space, floor at y = 0, +X toward the players' entrance (the baulk end, where
 * the table's own panel already hangs). Everything is mode-12 flat colour
 * except the lamps, which are emitted LAST and marked off by n_timber_idx so
 * the renderer can draw them unlit.
 */

/* The scoreboard's height above the arena floor: high enough to clear a
 * standing player and everything on the table, low enough to read. */
#define CUEVR_ARENA_BOARD_Y 3.05f

/* THE SCREEN ON THE WALL. The players' entrance side has no seating — it is a
 * flat dark face from the floor to the ceiling — and that is where a televised
 * match hangs its board. Arena-local: the wall is the plane x = 4.05, so the
 * screen sits just in front of it, facing back down the room at the table. */
/* The panel's own plane, a hair in FRONT of the casing — smaller x is toward
 * the table. Put at the casing's centre it is buried inside the box and draws
 * nothing at all, which is what happened first time. */
#define CUEVR_ARENA_SCR_X  3.975f
#define CUEVR_ARENA_SCR_Y  2.35f
#define CUEVR_ARENA_SCR_W  2.20f

static uint32_t arena_h(uint32_t x) {          /* tiny hash, stable per seat */
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}

/* Where the scoreboard hangs in the arena, in ARENA-LOCAL space. The app puts
 * the real HUD panel on this face so the board in the room IS the scoreboard,
 * rather than a prop with a second panel floating near it. */
float cuevr_arena_board_y(void)   { return CUEVR_ARENA_BOARD_Y; }
float cuevr_arena_board_half(void){ return 0.34f * 0.5f; }
void cuevr_arena_screen(float *x, float *y, float *w) {
    if (x) *x = CUEVR_ARENA_SCR_X;
    if (y) *y = CUEVR_ARENA_SCR_Y;
    if (w) *w = CUEVR_ARENA_SCR_W;
}

void cuevr_arena_capacity(int *max_verts, int *max_indices) {
    if (max_verts)   *max_verts   = 42000;
    if (max_indices) *max_indices = 64000;
}

void cuevr_arena_build(CueVrFrameMesh *m) {
    m->nv = m->ni = 0;
    m->overflow = 0;
    s_pass = 1;                     /* nothing here is timber; emit everything */

    static const float CARPET[3]  = { 0.055f, 0.050f, 0.058f };
    static const float WALLDK[3]  = { 0.038f, 0.038f, 0.044f };
    static const float BARRIER[3] = { 0.070f, 0.070f, 0.080f };
    static const float RAILRED[3] = { 0.380f, 0.045f, 0.060f };
    static const float PANELRD[3] = { 0.240f, 0.035f, 0.048f };
    static const float STEP[3]    = { 0.058f, 0.056f, 0.062f };
    static const float CEIL[3]    = { 0.024f, 0.024f, 0.030f };
    static const float GANTRY[3]  = { 0.046f, 0.048f, 0.054f };
    static const float LAMP[3]    = { 0.95f,  0.90f,  0.78f  };

    const float TAN22 = 0.4142136f;            /* tan(22.5): octagon half-side */
    const float CEIL_H = 8.6f;

    /* The carpet, one big plate. */
    box(m, -13.0f, -0.012f, -13.0f, 13.0f, 0.0f, 13.0f, 0, CARPET);

    /* The bank's geometry, shared by the seats and the wall behind them. */
    const int   ROWS   = 13;
    const float r_seat = 4.05f;
    const float rise   = 0.42f, run = 0.82f;
    const float y_base = 0.55f;

    /* Sides of the octagon. k = 0 faces +X — the players' entrance. */
    for (int k = 0; k < 8; k++) {
        float a  = (float)k * 0.7853982f;
        float ca = cosf(a), sa = sinf(a);
        int entrance = (k == 0);

        /* One ring segment: a band from radius r0..r1, heights y0..y1, as a
         * single outward-leaning quad pair is overkill — everything here is
         * flat quads placed along the side's chord. Chord frame: at inscribed
         * radius r the side runs from -r*TAN22 to +r*TAN22 across, `across`
         * being the direction perpendicular to the outward normal (ca,sa). */
        #define P(R, T, Y, out) { (R)*ca - (T)*sa + (out)*ca*0.0f, (Y), (R)*sa + (T)*ca }

        /* The barrier, with its red rail and red facing panel. */
        if (!entrance) {
            const float rb = 3.1f;
            float t0 = -rb * TAN22, t1 = rb * TAN22;
            { float p0[3]=P(rb,t0,0.0f,0), p1[3]=P(rb,t1,0.0f,0);
              float p2[3]=P(rb,t1,0.86f,0), p3[3]=P(rb,t0,0.86f,0);
              float n[3] = { -ca, 0, -sa };
              quad(m, p0,p1,p2,p3, n, t1-t0, 0.86f, BARRIER); }
            /* ...and its back, because a player leans over this rail to reach
             * a shot and a one-sided wall vanishes the moment they do. */
            { float bo = rb + 0.055f;
              float u0 = -bo * TAN22, u1 = bo * TAN22;
              float p0[3]=P(bo,u0,0.0f,0), p1[3]=P(bo,u1,0.0f,0);
              float p2[3]=P(bo,u1,0.86f,0), p3[3]=P(bo,u0,0.86f,0);
              float n[3] = { ca, 0, sa };
              quad(m, p0,p1,p2,p3, n, u1-u0, 0.86f, BARRIER); }
            { float p0[3]=P(rb-0.02f,t0,0.30f,0), p1[3]=P(rb-0.02f,t1,0.30f,0);
              float p2[3]=P(rb-0.02f,t1,0.80f,0), p3[3]=P(rb-0.02f,t0,0.80f,0);
              float n[3] = { -ca, 0, -sa };
              quad(m, p0,p1,p2,p3, n, t1-t0, 0.5f, PANELRD); }
            /* The rail's chord is measured at its OUTER radius, so adjacent
             * sides meet at the octagon's vertices — measured at the wall's
             * it fell short and every corner had a gap in the red. */
            { float ro = rb + 0.055f;
              float u0 = -ro * TAN22, u1 = ro * TAN22;
              float p0[3]=P(rb-0.03f,u0,0.86f,0), p1[3]=P(rb-0.03f,u1,0.86f,0);
              float p2[3]=P(ro,u1,0.86f,0), p3[3]=P(ro,u0,0.86f,0);
              float n[3] = { 0, 1, 0 };
              quad(m, p0,p1,p2,p3, n, u1-u0, 0.09f, RAILRED); }
        }

        /* The seating bank: thirteen rows, rising steeply. */
        if (!entrance) {
            for (int rr = 0; rr < ROWS; rr++) {
                float r0 = r_seat + run * (float)rr;
                float r1 = r0 + run;
                float y0 = y_base + rise * (float)rr;
                float y1 = y0 + rise;
                float t0 = -r0 * TAN22, t1 = r0 * TAN22;
                /* riser then tread */
                { float p0[3]=P(r0,t0,y0-rise,0), p1[3]=P(r0,t1,y0-rise,0);
                  float p2[3]=P(r0,t1,y0,0),      p3[3]=P(r0,t0,y0,0);
                  float n[3] = { -ca, 0, -sa };
                  quad(m, p0,p1,p2,p3, n, t1-t0, rise, STEP); }
                { float q0[3]=P(r0,t0,y0,0), q1[3]=P(r0,t1,y0,0);
                  float q2[3]=P(r1,r1*TAN22,y0,0), q3[3]=P(r1,-r1*TAN22,y0,0);
                  float n[3] = { 0, 1, 0 };
                  quad(m, q0,q1,q2,q3, n, t1-t0, run, STEP); }
                /* The step's NOSING, pale. In both references it is the
                 * brightest thing in the bank — a lit line running the whole
                 * width of every row — and without it the tiers read as one
                 * dark mass with seats floating on it. */
                { static const float NOSE[3] = { 0.62f, 0.60f, 0.57f };
                  float rn = r0 + 0.10f;
                  float q0[3]=P(r0,t0,y0+0.004f,0),  q1[3]=P(r0,t1,y0+0.004f,0);
                  float q2[3]=P(rn,t1,y0+0.004f,0),  q3[3]=P(rn,t0,y0+0.004f,0);
                  float n[3] = { 0, 1, 0 };
                  quad(m, q0,q1,q2,q3, n, t1-t0, 0.10f, NOSE); }
                /* THE SEATS. Three shapes to pick between — CUEVR_SEAT
                 * chooses — because a bank of seats is most of what this room
                 * looks like, and which one is right is taste rather than
                 * measurement. Hash-varied per seat either way, so a bank reads
                 * as hundreds of objects and not one striped slab. */
                /* TIP-UP, with the side rests. */
                int style = 1;
                { const char *e = getenv("CUEVR_SEAT"); if (e) style = atoi(e); }
                float span = r0 * TAN22 * 2.0f - 0.5f;
                int   nst  = (int)(span / 0.56f);
                if (nst < 1) nst = 1;
                float pitch = span / (float)nst;

                if (style == 3) {
                    /* BENCH — one continuous padded run per row with a back
                     * rail, no individual seats. A distant bank reads as a
                     * stripe of colour anyway, and this is a fraction of the
                     * triangles of the other two. */
                    uint32_t h = arena_h((uint32_t)(k * 131 + rr * 17));
                    float v = 0.86f + 0.24f * ((float)(h & 255) / 255.0f);
                    float col[3] = { 0.52f*v, 0.20f*v, 0.055f*v };
                    float rs = r0 + 0.36f, hs = y0, t1 = span * 0.5f;
                    { float p0[3]=P(rs,-t1,hs,0),       p1[3]=P(rs,t1,hs,0);
                      float p2[3]=P(rs,t1,hs+0.42f,0),  p3[3]=P(rs,-t1,hs+0.42f,0);
                      float n[3] = { -ca, 0, -sa };
                      quad(m, p0,p1,p2,p3, n, t1*2, 0.42f, col); }
                    { float p0[3]=P(rs-0.32f,-t1,hs+0.16f,0), p1[3]=P(rs-0.32f,t1,hs+0.16f,0);
                      float p2[3]=P(rs,t1,hs+0.20f,0),        p3[3]=P(rs,-t1,hs+0.20f,0);
                      float n[3] = { 0, 1, 0 };
                      quad(m, p0,p1,p2,p3, n, t1*2, 0.32f, col); }
                } else
                for (int sfi = 0; sfi < nst; sfi++) {
                    float tm = -span * 0.5f + pitch * ((float)sfi + 0.5f);
                    uint32_t h = arena_h((uint32_t)(k * 131 + rr * 17 + sfi));
                    float v = 0.82f + 0.36f * ((float)(h & 255) / 255.0f);
                    float col[3] = { 0.52f * v, 0.20f * v, 0.055f * v };
                    float dk[3]  = { col[0]*0.45f, col[1]*0.45f, col[2]*0.45f };
                    float rs = r0 + 0.36f, hs = y0, w = pitch * 0.44f;

                    if (style == 2) {
                        /* BUCKET — a back in three facets with the outer two
                         * turned forward, so each seat takes the light a little
                         * differently across its width and the row stops
                         * reading as a flat painted band. */
                        float bk = 0.44f, wl = w * 0.42f;
                        float n[3] = { -ca, 0, -sa };
                        { float p0[3]=P(rs,tm-wl,hs,0),     p1[3]=P(rs,tm+wl,hs,0);
                          float p2[3]=P(rs,tm+wl,hs+bk,0),  p3[3]=P(rs,tm-wl,hs+bk,0);
                          quad(m, p0,p1,p2,p3, n, wl*2, bk, col); }
                        for (int wsg = -1; wsg <= 1; wsg += 2) {
                            float a1 = tm + wsg*wl, a2 = tm + wsg*w;
                            float p0[3]=P(rs,a1,hs,0),          p1[3]=P(rs-0.10f,a2,hs,0);
                            float p2[3]=P(rs-0.10f,a2,hs+bk,0), p3[3]=P(rs,a1,hs+bk,0);
                            slope_face(m, p0,p1,p2,p3, -ca, -sa, w*0.6f, bk, col);
                        }
                        { float p0[3]=P(rs-0.32f,tm-w*0.9f,hs+0.16f,0), p1[3]=P(rs-0.32f,tm+w*0.9f,hs+0.16f,0);
                          float p2[3]=P(rs,tm+w*0.9f,hs+0.20f,0),       p3[3]=P(rs,tm-w*0.9f,hs+0.20f,0);
                          float nn[3] = { 0, 1, 0 };
                          quad(m, p0,p1,p2,p3, nn, w*1.8f, 0.32f, col); }
                    } else {
                        /* TIP-UP — the theatre seat as photographed: a scooped
                         * back leaning away, its outer thirds turned forward so
                         * the velvet catches the light unevenly across a row; a
                         * squab; and a dark divider standing proud between each
                         * pair, which is what separates one seat from the next
                         * once you are more than a few rows back. */
                        float bk = 0.42f, wl = w * 0.55f;
                        { float p0[3]=P(rs,tm-wl,hs,0),           p1[3]=P(rs,tm+wl,hs,0);
                          float p2[3]=P(rs+0.07f,tm+wl,hs+bk,0),  p3[3]=P(rs+0.07f,tm-wl,hs+bk,0);
                          slope_face(m, p0,p1,p2,p3, -ca, -sa, wl*2, bk, col); }
                        for (int wsg = -1; wsg <= 1; wsg += 2) {
                            float a1 = tm + wsg*wl, a2 = tm + wsg*w;
                            float p0[3]=P(rs,a1,hs,0),               p1[3]=P(rs-0.08f,a2,hs,0);
                            float p2[3]=P(rs-0.02f,a2,hs+bk*0.92f,0), p3[3]=P(rs+0.07f,a1,hs+bk,0);
                            slope_face(m, p0,p1,p2,p3, -ca, -sa, w*0.5f, bk, col);
                        }
                        { float p0[3]=P(rs-0.32f,tm-w,hs+0.15f,0), p1[3]=P(rs-0.32f,tm+w,hs+0.15f,0);
                          float p2[3]=P(rs,tm+w,hs+0.21f,0),       p3[3]=P(rs,tm-w,hs+0.21f,0);
                          float nn[3] = { 0, 1, 0 };
                          quad(m, p0,p1,p2,p3, nn, w*2, 0.32f, col); }
                        for (int dsg = -1; dsg <= 1; dsg += 2) {
                            float a1 = tm + dsg * w;
                            float p0[3]=P(rs-0.30f,a1,hs+0.20f,0), p1[3]=P(rs,a1,hs+0.20f,0);
                            float p2[3]=P(rs,a1,hs+0.34f,0),       p3[3]=P(rs-0.30f,a1,hs+0.30f,0);
                            float nn[3] = { 0, 1, 0 };
                            quad(m, p0,p1,p2,p3, nn, 0.30f, 0.14f, dk);
                        }
                    }
                }
            }
        }
        /* ...and the SCREEN, a real one bolted to that wall: a bezel standing
         * proud of the face with a recess for the panel the app draws into it.
         * A scoreboard in a room like this is a piece of hardware on a wall,
         * not a rectangle hanging in the air near one. */
        if (entrance) {
            const float sw = CUEVR_ARENA_SCR_W * 0.5f;
            const float sh = CUEVR_ARENA_SCR_W * 0.5f * (84.0f / 128.0f);
            const float bz = 0.075f;                 /* bezel */
            /* The bezel has to be LIGHTER than the wall or it is invisible:
             * the wall is 0.038 and the first casing was 0.045, which is the
             * same black by eye. A screen on a dark wall reads by its frame. */
            static const float CASE[3]  = { 0.115f, 0.118f, 0.130f };
            static const float RECESS[3]= { 0.010f, 0.010f, 0.013f };
            float x0 = CUEVR_ARENA_SCR_X + 0.010f;   /* behind the panel */
            float yc = CUEVR_ARENA_SCR_Y;
            /* the casing face, with the middle left dark for the panel */
            box(m, x0, yc - sh - bz, -sw - bz, x0 + 0.10f, yc + sh + bz, sw + bz, 1, CASE);
            box(m, x0 + 0.005f, yc - sh, -sw, x0 + 0.11f, yc + sh, sw, 1, RECESS);
        }
        if (entrance) {
            const float rw = 4.03f;
            float p0[3]=P(rw,-1.1f,0.0f,0), p1[3]=P(rw,1.1f,0.0f,0);
            float p2[3]=P(rw,1.1f,2.6f,0),  p3[3]=P(rw,-1.1f,2.6f,0);
            float n[3] = { -ca, 0, -sa };
            quad(m, p0,p1,p2,p3, n, 2.2f, 2.6f, SHADOW);
        }

        /* The back wall, tier top to ceiling. */
        {
            float rw = (k == 0) ? 4.05f : r_seat + run * (float)ROWS;
            float yw = (k == 0) ? 0.0f  : y_base + rise * (float)ROWS;
            float t1 = rw * TAN22;
            float p0[3]=P(rw,-t1,yw,0), p1[3]=P(rw,t1,yw,0);
            float p2[3]=P(rw,t1,CEIL_H,0), p3[3]=P(rw,-t1,CEIL_H,0);
            float n[3] = { -ca, 0, -sa };
            quad(m, p0,p1,p2,p3, n, t1*2, CEIL_H-yw, WALLDK);
        }
        #undef P
    }

    /* The ceiling plate. */
    box(m, -14.0f, CEIL_H, -14.0f, 14.0f, CEIL_H + 0.05f, 14.0f, 0, CEIL);

    /* THE GANTRY. Three to choose between — CUEVR_GANTRY picks one — and the
     * third is the one that carries the scoreboard, so the panel hangs over the
     * table where a televised match puts it instead of floating past the end of
     * the room. cuevr_arena_board() reports where its screen face is so the app
     * can mount the real HUD there. */
    {
        /* THE TRUSS, and nothing hanging off it. The scoreboard block is still
         * here behind gs 3 if it is ever wanted, but the room reads better with
         * the lattice alone and the board where it already was. */
        int gs = 2;
        { const char *e = getenv("CUEVR_GANTRY"); if (e) gs = atoi(e); }

        if (gs == 1) {
            /* PLAIN RINGS — two slim rails, the original. */
            for (int g = 0; g < 2; g++) {
                float rg = g ? 7.6f : 5.0f;
                float yg = CEIL_H - (g ? 1.7f : 0.9f);
                for (int k = 0; k < 8; k++) {
                    float a = (float)k * 0.7853982f;
                    float ca = cosf(a), sa = sinf(a);
                    float t1 = rg * 0.4142136f;
                    float p0[3]={rg*ca + t1*sa, yg, rg*sa - t1*ca};
                    float p1[3]={rg*ca - t1*sa, yg, rg*sa + t1*ca};
                    float p2[3]={rg*ca - t1*sa, yg+0.16f, rg*sa + t1*ca};
                    float p3[3]={rg*ca + t1*sa, yg+0.16f, rg*sa - t1*ca};
                    float n[3] = { -ca, 0, -sa };
                    quad(m, p0,p1,p2,p3, n, t1*2, 0.16f, GANTRY);
                }
            }
        } else {
            /* TRUSS — a lattice ring: two chords with diagonals zig-zagging
             * between them, which is what a real lighting truss is and what
             * makes it read as engineering rather than as a hoop. */
            for (int g = 0; g < 2; g++) {
                float rg = g ? 7.6f : 5.0f;
                float yg = CEIL_H - (g ? 1.8f : 1.0f);
                const float dp = 0.34f;                 /* truss depth */
                for (int k = 0; k < 8; k++) {
                    float a = (float)k * 0.7853982f;
                    float ca = cosf(a), sa = sinf(a);
                    float t1 = rg * 0.4142136f;
                    #define TP(tt, yy) { rg*ca - (tt)*sa, (yy), rg*sa + (tt)*ca }
                    for (int ch = 0; ch < 2; ch++) {
                        float yy = yg + (ch ? dp : 0.0f);
                        float p0[3]=TP(-t1,yy),        p1[3]=TP(t1,yy);
                        float p2[3]=TP(t1,yy+0.09f),   p3[3]=TP(-t1,yy+0.09f);
                        float n[3] = { -ca, 0, -sa };
                        quad(m, p0,p1,p2,p3, n, t1*2, 0.09f, GANTRY);
                    }
                    int nd = 6;
                    for (int dd = 0; dd < nd; dd++) {
                        float u0 = -t1 + 2*t1*dd/nd, u1 = -t1 + 2*t1*(dd+1)/nd;
                        float lo = (dd & 1) ? yg + dp : yg, hi = (dd & 1) ? yg : yg + dp;
                        float p0[3]=TP(u0,lo),        p1[3]=TP(u1,hi);
                        float p2[3]=TP(u1,hi+0.05f),  p3[3]=TP(u0,lo+0.05f);
                        float n[3] = { -ca, 0, -sa };
                        quad(m, p0,p1,p2,p3, n, 0.4f, 0.05f, GANTRY);
                    }
                    #undef TP
                }
            }
        }

        if (gs >= 3) {
            /* ...and the SCOREBOARD, hung over the middle of the table on a
             * cross beam. Four faces: the two long ones carry the screen, the
             * ends are blank casing. The screen face is left to the app to
             * fill — see cuevr_arena_board(). */
            const float bw = 1.30f, bh = 0.78f, bd = 0.34f;
            const float by = CUEVR_ARENA_BOARD_Y;
            static const float CASE[3] = { 0.052f, 0.054f, 0.060f };
            /* the two hangers up to the ceiling */
            for (int sg = -1; sg <= 1; sg += 2) {
                float x = sg * bw * 0.55f;
                box(m, x - 0.035f, by + bh*0.5f, -0.035f,
                       x + 0.035f, CEIL_H,        0.035f, 1, GANTRY);
            }
            /* the casing: ends and top and bottom, the long faces left for the
             * screen quads the app draws over them */
            box(m, -bw*0.5f, by - bh*0.5f, -bd*0.5f,
                    bw*0.5f, by + bh*0.5f,  bd*0.5f, 0, CASE);
        }
    }

    /* THE LAMPS, last, beyond the boundary: the night sky. Rings of small
     * downward quads with hashed jitter, denser toward the rim, exactly the
     * look of the reference — hundreds of point sources on a black field. */
    m->n_timber_idx = m->ni;
    {
        static const float ring_r[6] = { 2.2f, 3.6f, 5.0f, 6.4f, 7.8f, 9.2f };
        static const int   ring_n[6] = { 14,   24,   36,   46,   58,   70   };
        for (int q = 0; q < 6; q++)
            for (int i = 0; i < ring_n[q]; i++) {
                uint32_t h = arena_h((uint32_t)(q * 977 + i));
                float a = 6.2831853f * ((float)i + 0.5f * ((float)(h & 63) / 63.0f))
                        / (float)ring_n[q];
                float r = ring_r[q] * (0.92f + 0.16f * ((float)((h >> 8) & 63) / 63.0f));
                float x = r * cosf(a), z = r * sinf(a);
                float y = CEIL_H - 0.06f - 0.4f * ((float)((h >> 16) & 3) / 3.0f);
                float sz2 = 0.055f;
                float p0[3]={x-sz2,y,z-sz2}, p1[3]={x+sz2,y,z-sz2};
                float p2[3]={x+sz2,y,z+sz2}, p3[3]={x-sz2,y,z+sz2};
                float n[3] = { 0, -1, 0 };
                quad(m, p0,p1,p2,p3, n, 0.1f, 0.1f, LAMP);
            }
    }
    s_pass = 0;
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
    tray_measure(t, w);         /* before anything asks where the tray is */
    /* Timber first, then everything else, with the boundary recorded — the
     * renderer draws the two runs with two different shaders. See is_timber. */
    /* CUEVR_TRAYONLY: the tray with no table around it, so its shape can be
     * checked on its own instead of guessed at through a pocket. */
    int only = getenv("CUEVR_TRAYONLY") != NULL;
    s_pass = 0;
    if (!only) CUEVR_FRAMES[which].build(m, t);
    black_tray(m, t, w);            /* emits nothing in this pass: it is not timber */
    m->n_timber_idx = m->ni;
    s_pass = 1;
    if (!only) CUEVR_FRAMES[which].build(m, t);
    /* Every design gets the tray, and gets it from here rather than from its own
     * body, so a new design cannot forget to close the table off. */
    black_tray(m, t, w);
    WRLD = NULL;
}

/* Which design suits a given table, when the player has not chosen one. Every
 * table kind has an obvious real-world answer and this is it: a pub table is a
 * cabinet, a 9 ft American is an American, a small snooker table is the turned
 * Victorian everyone pictures, and the full-size match tables keep the Regency
 * they were built against. */
int cuevr_frame_default(const CueTable *t) {
    switch (t->kind) {
    case CUE_GAME_UK8:    return 1;   /* CABINET   — 7 ft, coin-op body */
    case CUE_GAME_SNK6:   return 0;   /* REGENCY   — snooker on a 7 ft bed is
                                       * still snooker, and a coin-op cabinet
                                       * under it looks like a pub table that
                                       * has been dressed up */
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
    /* The Victorian's turned legs dominate, and the pocket cut-outs in the
     * horizontal faces add a few thousand more. Indices are 16-bit, so the vertex
     * ceiling is the real constraint; this is one malloc at start-up. */
    /* The Victorian at 96 stations x 12 sides is ~4600 verts and ~6900 indices
     * a leg, six legs on a 10 ft table. */
    if (max_verts)   *max_verts   = 52000;
    if (max_indices) *max_indices = 96000;
}

float cuevr_frame_depth(const CueTable *t) {
    (void)t;
    /* Cloth to floor. A match table is 2 ft 10 in to the bed; the player can
     * move the whole thing up or down in setup, but the table's own proportions
     * do not change with it. */
    return 0.85f;
}
