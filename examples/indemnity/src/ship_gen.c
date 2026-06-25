/*
 * ThumbyElite — procedural ship mesh generation.
 *
 * Geometry: 8-point chamfered-octagon rings lofted along z. Ring points
 * run CCW viewed from +z, so side quads wind outward and the nose fan
 * (toward +z) caps cleanly. All attachments mirror across x.
 *
 * Families steer the silhouette: DART (slim racer), FIGHTER (swept
 * wings), INTERCEPTOR (long nose + canards), GUNSHIP (prongs, twin
 * fins), CRUISER (long spine, dorsal fin), HAULER (deep slab + pods).
 */
#include "ship_gen.h"
#include "elite_types.h"
#include <math.h>

#define MAX_SV 240
#define MAX_SF 420

static MeshVert s_verts[MAX_SV];
static MeshFace s_faces[MAX_SF];
static uint16_t s_facecolors[MAX_SF];   /* engine per-face colours (parallel to s_faces) */
static Mesh     s_mesh;
static float    s_fx[MAX_SV], s_fy[MAX_SV], s_fz[MAX_SV];
static int      s_nv, s_nf;

static uint32_t s_rng;
static uint32_t rnd(void) {
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return s_rng;
}

/* Style lab: 0 = shipping look, 1 = proposal. ALL style-1 work draws
 * its randomness from a SECOND xorshift stream (s_rng2) inside its own
 * branches, so the style-0 rnd() sequence — and therefore every
 * shipped mesh — stays byte-identical. */
static int s_style = 1;   /* ADOPTED 2026-06-12 */
#ifdef ELITE_STYLE_LAB
int g_force_gunship = 0;   /* guide harness: only the loft+gun variant */
uint32_t g_force_xfoil_seed = 0;  /* guide harness: force this seed's hull to an x-foil (X-wing) */
#endif
/* Underwriter (grey-issue) hull: the Indemnity's own ships. Uniform dead
 * grey, no accent, no registry, no engine glow -- a deliberate contrast
 * to the varied, accented civilian/pirate hulls so they read instantly.
 * 0 = off; 1..4 select a candidate silhouette (real feature, not lab). */
int g_force_adjuster = 0;
void ship_gen_set_style(int s) { s_style = s; }
static float rndf(float lo, float hi) {
    return lo + (hi - lo) * (float)(rnd() & 0xFFFF) * (1.0f / 65535.0f);
}
static int rndi(int lo, int hi) {
    return lo + (int)(rnd() % (uint32_t)(hi - lo + 1));
}

static int vtx(float x, float y, float z) {
    if (s_nv >= MAX_SV) return s_nv - 1;
    s_fx[s_nv] = x; s_fy[s_nv] = y; s_fz[s_nv] = z;
    return s_nv++;
}

static void face(int a, int b, int c, uint16_t color) {
    if (s_nf >= MAX_SF) return;
    int idx = s_nf;
    MeshFace *f = &s_faces[s_nf++];
    f->a = (uint8_t)a; f->b = (uint8_t)b; f->c = (uint8_t)c;
    s_facecolors[idx] = color;          /* engine: per-face colour in a parallel array */
    float ux = s_fx[b] - s_fx[a], uy = s_fy[b] - s_fy[a], uz = s_fz[b] - s_fz[a];
    float vx = s_fx[c] - s_fx[a], vy = s_fy[c] - s_fy[a], vz = s_fz[c] - s_fz[a];
    float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
    float l = sqrtf(nx * nx + ny * ny + nz * nz);
    if (l < 1e-9f) l = 1;
    f->nx = (int8_t)(nx / l * 127.0f);
    f->ny = (int8_t)(ny / l * 127.0f);
    f->nz = (int8_t)(nz / l * 127.0f);
}

static void quad(int a, int b, int c, int d, uint16_t col) {
    face(a, b, c, col);
    face(a, c, d, col);
}

/* Chamfered-octagon ring, CCW from +z. ch 0 = diamond, 1 = rectangle. */
static void ring(float z, float w, float h, float yo, float ch, int out[8]) {
    float wc = w * ch, hc = h * ch;
    out[0] = vtx(w, yo - hc, z);
    out[1] = vtx(w, yo + hc, z);
    out[2] = vtx(wc, yo + h, z);
    out[3] = vtx(-wc, yo + h, z);
    out[4] = vtx(-w, yo + hc, z);
    out[5] = vtx(-w, yo - hc, z);
    out[6] = vtx(-wc, yo - h, z);
    out[7] = vtx(wc, yo - h, z);
}

/* Side skin between two rings (r0 at smaller z). */
static void skin(const int r0[8], const int r1[8], uint16_t top,
                 uint16_t side, uint16_t bottom) {
    for (int k = 0; k < 8; k++) {
        int k2 = (k + 1) & 7;
        uint16_t c = (k == 2) ? top : (k == 6) ? bottom
                   : (k == 1 || k == 5) ? top : side;
        quad(r0[k], r0[k2], r1[k2], r1[k], c);
    }
}

static void cap_front(const int r[8], uint16_t col) {       /* +z fan */
    for (int k = 1; k < 7; k++) face(r[0], r[k], r[k + 1], col);
}
static void cap_back(const int r[8], uint16_t col) {        /* -z fan */
    for (int k = 1; k < 7; k++) face(r[0], r[k + 1], r[k], col);
}
static void nose_apex(const int r[8], float x, float y, float z,
                      uint16_t col) {
    int a = vtx(x, y, z);
    for (int k = 0; k < 8; k++)
        face(r[k], r[(k + 1) & 7], a, col);
}

/* Swept wing pair: thin slab from root to tip edge (mirrored). */
static void wings(float rootz0, float rootz1, float rootx, float rooty,
                  float th, float tipx, float tipz0, float tipz1,
                  float tipy, uint16_t col) {
    for (int side = 0; side < 2; side++) {
        float sx = side ? -1.0f : 1.0f;
        int r0 = vtx(sx * rootx, rooty - th, rootz0);
        int r1 = vtx(sx * rootx, rooty - th, rootz1);
        int r2 = vtx(sx * rootx, rooty + th, rootz1);
        int r3 = vtx(sx * rootx, rooty + th, rootz0);
        int t0 = vtx(sx * tipx, tipy, tipz0);
        int t1 = vtx(sx * tipx, tipy, tipz1);
        if (side == 0) {
            quad(r3, r2, r1, r0, col);          /* root cap (inner) */
            quad(r2, r3, t0, t1, col);          /* top */
            quad(r0, r1, t1, t0, col);          /* bottom */
            face(r1, r2, t1, col);              /* leading edge */
            face(r3, r0, t0, col);              /* trailing edge */
        } else {
            quad(r0, r1, r2, r3, col);
            quad(r3, r2, t1, t0, col);
            quad(r1, r0, t0, t1, col);
            face(r2, r1, t1, col);
            face(r0, r3, t0, col);
        }
    }
}

/* Vertical fin (top or bottom). dir = +1 up / -1 down. */
static void fin(float xoff, float z0, float z1, float ybase, float th,
                float height, float tipz0, float tipz1, float dir,
                uint16_t col) {
    int r0 = vtx(xoff - th, ybase, z0);
    int r1 = vtx(xoff + th, ybase, z0);
    int r2 = vtx(xoff + th, ybase, z1);
    int r3 = vtx(xoff - th, ybase, z1);
    float yt = ybase + dir * height;
    int t0 = vtx(xoff, yt, tipz0);
    int t1 = vtx(xoff, yt, tipz1);
    if (dir > 0) {
        quad(r0, r1, r2, r3, col);
        quad(t0, t1, r2, r1, col);
        quad(t1, t0, r0, r3, col);
        face(r3, r2, t1, col);
        face(r1, r0, t0, col);
    } else {
        quad(r3, r2, r1, r0, col);
        quad(r1, r2, t1, t0, col);
        quad(r3, r0, t0, t1, col);
        face(r2, r3, t1, col);
        face(r0, r1, t0, col);
    }
}

/* Small lofted pod (nacelle), mirrored when mirror!=0. */
static void nacelle(float x, float y, float z0, float z1, float r,
                    uint16_t body, uint16_t glow, int mirror) {
    for (int side = 0; side < (mirror ? 2 : 1); side++) {
        float sx = side ? -x : x;
        int a[8], b[8], c[8];
        ring(z0, r * 0.7f, r * 0.7f, y, 0.45f, a);
        ring((z0 + z1) * 0.5f, r, r, y, 0.45f, b);
        ring(z1, r * 0.75f, r * 0.75f, y, 0.45f, c);
        /* shift ring x */
        for (int k = 0; k < 8; k++) {
            s_fx[a[k]] += sx; s_fx[b[k]] += sx; s_fx[c[k]] += sx;
        }
        skin(a, b, body, body, body);
        skin(b, c, body, body, body);
        cap_back(a, glow);
        cap_front(c, body);
    }
}

/* Axis-aligned slab (disc mandibles): front face dark muzzle-ish,
 * everything else hull colours. */
static void slab(float cx, float cy, float cz, float hx, float hy,
                 float hz, uint16_t top, uint16_t side, uint16_t front) {
    int v000 = vtx(cx - hx, cy - hy, cz - hz);
    int v100 = vtx(cx + hx, cy - hy, cz - hz);
    int v010 = vtx(cx - hx, cy + hy, cz - hz);
    int v110 = vtx(cx + hx, cy + hy, cz - hz);
    int v001 = vtx(cx - hx, cy - hy, cz + hz);
    int v101 = vtx(cx + hx, cy - hy, cz + hz);
    int v011 = vtx(cx - hx, cy + hy, cz + hz);
    int v111 = vtx(cx + hx, cy + hy, cz + hz);
    quad(v001, v101, v111, v011, front);   /* +z bow */
    quad(v100, v000, v010, v110, side);    /* -z stern */
    quad(v101, v100, v110, v111, side);    /* +x */
    quad(v000, v001, v011, v010, side);    /* -x */
    quad(v011, v111, v110, v010, top);     /* +y */
    quad(v000, v100, v101, v001, top);     /* -y */
}

/* Tapered mandible prong, built with slab()'s proven winding: caller
 * passes explicit root x-extent (xl < xr) and tip x-extent — mirroring
 * is the caller's job, so there is exactly one face-order code path. */
static void prong(float xl, float xr, float xtl, float xtr, float hy,
                  float hy_tip, float z0, float z1,
                  uint16_t top, uint16_t side, uint16_t tipc) {
    int r0 = vtx(xl, -hy, z0);
    int r1 = vtx(xr, -hy, z0);
    int r2 = vtx(xr, hy, z0);
    int r3 = vtx(xl, hy, z0);
    int t0 = vtx(xtl, -hy_tip, z1);
    int t1 = vtx(xtr, -hy_tip, z1);
    int t2 = vtx(xtr, hy_tip, z1);
    int t3 = vtx(xtl, hy_tip, z1);
    quad(t3, t2, r2, r3, top);    /* top slope  (+y) */
    quad(r0, r1, t1, t0, top);    /* belly      (-y) */
    quad(t1, r1, r2, t2, side);   /* +x wall */
    quad(r0, t0, t3, r3, side);   /* -x wall */
    quad(t0, t1, t2, t3, tipc);   /* raked tip  (+z) */
}

/* Slim forward gun barrel (X-wing wingtips, prongs). */
static void tip_gun(float x, float y, float z, float len, float r,
                    uint16_t col, uint16_t dark) {
    int g0[8], g1[8];
    ring(z, r, r, y, 0.4f, g0);
    ring(z + len, r * 0.7f, r * 0.7f, y, 0.4f, g1);
    for (int k = 0; k < 8; k++) { s_fx[g0[k]] += x; s_fx[g1[k]] += x; }
    skin(g0, g1, col, col, col);
    cap_back(g0, col);
    cap_front(g1, dark);
}

/* Mirrored pair of forward gun barrels. */
static void gun_pair(float x, float y, float z, float blen, float br,
                     uint16_t col, uint16_t muz) {
    tip_gun(x, y, z, blen, br, col, muz);
    tip_gun(-x, y, z, blen, br, col, muz);
}

/* ===================== STYLE-1 PROPOSAL KIT ========================
 * Only reachable when s_style == 1. Second rng stream keeps style-0
 * byte-identical. Hex (6-gon) sections keep gun/nozzle face costs
 * roughly half of the 8-gon ring equivalents. */
static uint32_t s_rng2;
static uint32_t rnd2(void) {
    s_rng2 ^= s_rng2 << 13; s_rng2 ^= s_rng2 >> 17; s_rng2 ^= s_rng2 << 5;
    return s_rng2;
}
static float rndf2(float lo, float hi) {
    return lo + (hi - lo) * (float)(rnd2() & 0xFFFF) * (1.0f / 65535.0f);
}

/* Hexagonal ring in the xy plane at z, CCW from +z. */
static void hex6(float z, float r, float x, float y, int out[6]) {
    static const float c6[6] = { 1, .5f, -.5f, -1, -.5f, .5f };
    static const float s6[6] = { 0, .866f, .866f, 0, -.866f, -.866f };
    for (int k = 0; k < 6; k++)
        out[k] = vtx(x + r * c6[k], y + r * s6[k], z);
}
static void skin6(const int a[6], const int b[6], uint16_t col) {
    for (int k = 0; k < 6; k++) {
        int k2 = (k + 1) % 6;
        quad(a[k], a[k2], b[k2], b[k], col);
    }
}
static void fan6f(const int r[6], uint16_t col) {      /* faces +z */
    for (int k = 1; k < 5; k++) face(r[0], r[k], r[k + 1], col);
}
static void fan6b(const int r[6], uint16_t col) {      /* faces -z */
    for (int k = 1; k < 5; k++) face(r[0], r[k + 1], r[k], col);
}

/* Stepped cannon: housing block, sharp step down to the barrel, then
 * a muzzle-brake flare. Replaces the weedy single-tube tip_gun. */
static void gun_v2(float x, float y, float z, float len, float r,
                   uint16_t col, uint16_t dark) {
    int g0[6], g1[6], g2[6], g3[6], g4[6];
    hex6(z, r * 1.5f, x, y, g0);
    hex6(z + len * 0.30f, r * 1.35f, x, y, g1);
    hex6(z + len * 0.36f, r * 0.62f, x, y, g2);
    hex6(z + len * 0.82f, r * 0.55f, x, y, g3);
    hex6(z + len, r * 0.95f, x, y, g4);
    skin6(g0, g1, col);
    skin6(g1, g2, col);
    skin6(g2, g3, col);
    skin6(g3, g4, col);
    fan6b(g0, col);
    fan6f(g4, dark);
}

/* Twin-linked cannon: one chamfered housing spanning both barrels,
 * each barrel stepped with a muzzle flare. */
static void gun_twin(float x, float y, float z, float len, float r,
                     uint16_t col, uint16_t dark) {
    int h0[8], h1[8];
    ring(z, x + r * 1.4f, r * 1.35f, y, 0.35f, h0);
    ring(z + len * 0.34f, x + r * 1.25f, r * 1.2f, y, 0.35f, h1);
    skin(h0, h1, col, col, col);
    cap_back(h0, col);
    cap_front(h1, col);
    for (int sd = 0; sd < 2; sd++) {
        float bx = sd ? -x : x;
        int b0[6], b1[6], b2[6];
        hex6(z + len * 0.30f, r * 0.60f, bx, y, b0);
        hex6(z + len * 0.84f, r * 0.52f, bx, y, b1);
        hex6(z + len, r * 0.88f, bx, y, b2);
        skin6(b0, b1, col);
        skin6(b1, b2, col);
        fan6f(b2, dark);
    }
}

/* Recessed engine nozzle on an aft (-z) face: short bell protruding
 * past z, dark interior cone, glow disc tucked INSIDE the bell. */
static void nozzle6(float x, float y, float z, float r, float depth,
                    uint16_t body, uint16_t glow) {
    int b0[6], b1[6], b2[6];
    hex6(z + depth * 0.7f, r * 0.9f, x, y, b0);     /* buried base */
    hex6(z - depth, r, x, y, b1);                   /* bell rim */
    hex6(z - depth * 0.5f, r * 0.66f, x, y, b2);    /* throat */
    skin6(b1, b0, body);                  /* outer bell wall */
    skin6(b2, b1, RGB565C(34, 34, 40));   /* interior cone, faces aft */
    fan6b(b2, glow);                      /* recessed glow disc */
}

/* Octagon drum along x (panel hub bosses). Cap fan on the xout end;
 * winding flips automatically for the mirrored (-x) side. */
static void drum_x(float xin, float xout, float cy, float cz, float ry,
                   float rz, uint16_t col, uint16_t capc) {
    int a[8], b[8];
    for (int k = 0; k < 8; k++) {
        float th = 0.3927f + (float)k * 0.7854f;
        float yy = cy + ry * cosf(th), zz = cz + rz * sinf(th);
        a[k] = vtx(xin, yy, zz);
        b[k] = vtx(xout, yy, zz);
    }
    if (xout > xin) {
        for (int k = 0; k < 8; k++) {
            int k2 = (k + 1) & 7;
            quad(a[k], a[k2], b[k2], b[k], col);
        }
        for (int k = 1; k < 7; k++) face(b[0], b[k], b[k + 1], capc);
    } else {
        for (int k = 0; k < 8; k++) {
            int k2 = (k + 1) & 7;
            quad(a[k2], a[k], b[k], b[k2], col);
        }
        for (int k = 1; k < 7; k++) face(b[0], b[k + 1], b[k], capc);
    }
}

/* class-hint state (set by ship_gen_mesh_class; -1 = free roll) */
static int s_hint = -1;

/* Underwriter hull: a bespoke, deliberately NON-fighter silhouette so the
 * Indemnity's grey ships read instantly (a recoloured fighter still looks
 * like a fighter). Built straight from vtx()/face(); nose = +z. */
static void adjuster_build(int form, uint16_t hull, uint16_t hull2,
                           uint16_t glass) {
    (void)form; (void)glass;   /* only the BLADE shipped (disc/trident dropped) */
    /* BLADE / MONOLITH: a tall, flat, double-edged blade that flies
     * nose-first -- vertical where fighters are horizontal. */
    float Lf = 1.25f, Lb = 0.75f;              /* nose / tail reach */
    float Hy = 0.62f, Wx = 0.14f;              /* tall, thin */
    int nose = vtx(0, 0, Lf);
    int mt = vtx(0, Hy, 0), mb = vtx(0, -Hy, 0);
    int ml = vtx(-Wx, 0, 0), mr = vtx(Wx, 0, 0);
    int tt = vtx(0, Hy * 0.7f, -Lb), tb = vtx(0, -Hy * 0.7f, -Lb);
    int tl = vtx(-Wx * 0.7f, 0, -Lb), tr = vtx(Wx * 0.7f, 0, -Lb);
    /* nose fan */
    face(nose, mt, mr, hull);  face(nose, mr, mb, hull2);
    face(nose, mb, ml, hull);  face(nose, ml, mt, hull2);
    /* mid -> tail */
    face(mt, tt, tr, hull2); face(mt, tr, mr, hull);
    face(mr, tr, tb, hull2); face(mr, tb, mb, hull);
    face(mb, tb, tl, hull2); face(mb, tl, ml, hull);
    face(ml, tl, tt, hull2); face(ml, tt, mt, hull);
    /* tail cap (dim) */
    face(tt, tb, tr, hull2); face(tt, tl, tb, hull2);
    /* THE EYE: one bright cold aperture proud of each forward flank --
     * the only light on an otherwise dead-grey hull. (void)glass: the
     * eye is its own brighter colour so it actually reads as an eye. */
    (void)glass;
    uint16_t eye = RGB565C(150, 210, 235);
    float ez = Lf * 0.46f, ey = Hy * 0.22f, eh = 0.16f;
    for (int s = -1; s <= 1; s += 2) {
        float ex = (float)s * Wx * 0.60f;
        float px = (float)s * (Wx * 0.60f + 0.12f);   /* proud tip */
        int q0 = vtx(ex, ey - eh, ez - eh), q1 = vtx(ex, ey + eh, ez - eh);
        int q2 = vtx(ex, ey + eh, ez + eh), q3 = vtx(ex, ey - eh, ez + eh);
        int pk = vtx(px, ey, ez);
        if (s > 0) {
            face(q0, q1, pk, eye); face(q1, q2, pk, eye);
            face(q2, q3, pk, eye); face(q3, q0, pk, eye);
        } else {
            face(q1, q0, pk, eye); face(q2, q1, pk, eye);
            face(q3, q2, pk, eye); face(q0, q3, pk, eye);
        }
    }
}

const Mesh *ship_gen_mesh(uint32_t seed) {
    /* Proper mix — seed|1 made adjacent even/odd seeds identical. */
    s_rng = seed * 2654435761u;
    s_rng ^= s_rng >> 15;
    s_rng *= 1274126177u;
    if (s_rng == 0) s_rng = 1;
    s_nv = s_nf = 0;
    int s1 = (s_style == 1);
    s_rng2 = (seed ^ 0x9E3779B9u) * 747796405u + 2891336453u;
    s_rng2 ^= s_rng2 >> 13;
    if (s_rng2 == 0) s_rng2 = 1;

    /* --- palette ------------------------------------------------------ */
    int tone = rndi(0, 3);
    uint16_t HULL, HULL2;
    switch (tone) {
    case 0: HULL = RGB565C(168, 170, 178); HULL2 = RGB565C(120, 122, 132); break;
    case 1: HULL = RGB565C(150, 160, 180); HULL2 = RGB565C(104, 114, 138); break;
    case 2: HULL = RGB565C(176, 166, 148); HULL2 = RGB565C(130, 120, 102); break;
    default: HULL = RGB565C(126, 132, 142); HULL2 = RGB565C(88, 94, 104); break;
    }
    uint16_t GLASS = RGB565C(40, 120, 150);
    static const uint16_t k_glow[4] = {
        RGB565C(245, 120, 30), RGB565C(70, 150, 255),
        RGB565C(80, 230, 180), RGB565C(200, 90, 230),
    };
    uint16_t GLOW = k_glow[rndi(0, 3)];
    static const uint16_t k_acc[4] = {
        RGB565C(190, 60, 45), RGB565C(220, 170, 50),
        RGB565C(60, 140, 200), RGB565C(150, 150, 160),
    };
    uint16_t ACC = k_acc[rndi(0, 3)];
    if (g_force_adjuster) {
        /* dead matte grey, a single cold aperture, no accent or glow,
         * and a bespoke non-fighter silhouette built right here. */
        HULL  = RGB565C(124, 128, 136);
        HULL2 = RGB565C(84, 88, 96);
        GLASS = RGB565C(70, 86, 100);
        adjuster_build(g_force_adjuster, HULL, HULL2, GLASS);
        goto finish;
    }

    /* --- archetype + family + fuselage genes --------------------------
     * archetype: 0 = loft families, 1 = x-foil, 2 = panel pod (TIE),
     * 3 = saucer. Class hints bias both. */
    int arch, family = 0;
    int hint = s_hint;
    s_hint = -1;
#ifdef ELITE_STYLE_LAB
    if (g_force_gunship) { arch = 0; family = 3; goto archdone; }
    if (g_force_xfoil_seed && seed == g_force_xfoil_seed) {
        arch = 1; family = 1; goto archdone;   /* X-wing hero */
    }
#endif
    if (hint < 0) {
        int r = rndi(0, 99);
        arch = (r < 55) ? 0 : (r < 75) ? 1 : (r < 87) ? 2 : 3;
        if (arch == 0) family = rndi(0, 5);
    } else {
        /* Class hints BIAS the silhouette but every class draws from
         * 3+ archetype/family combos — single-shape hints made whole
         * classes feel cloned in-game (user report) even though the
         * free-roll demo sheets were varied. */
        int r2 = rndi(0, 99);
        switch (hint) {
        case 0:   /* starter: scrappy anything-small */
            arch = (r2 < 40) ? 0 : (r2 < 65) ? 0 : (r2 < 85) ? 3 : 2;
            family = (r2 < 40) ? 0 : 5;
            break;
        case 1:   /* courier: slim + fast shapes */
            arch = (r2 < 50) ? 0 : (r2 < 75) ? 1 : 3;
            family = 0;
            break;
        case 2:   /* light fighter */
            arch = (r2 < 40) ? 0 : (r2 < 75) ? 1 : 2;
            family = 1;
            break;
        case 3:   /* patrol interceptor */
            arch = (r2 < 35) ? 1 : (r2 < 75) ? 0 : 2;
            family = rndi(1, 2);
            break;
        case 4:   /* raider */
            arch = (r2 < 40) ? 0 : (r2 < 70) ? 2 : 1;
            family = 3;
            break;
        case 5:   /* heavy fighter */
            arch = (r2 < 40) ? 1 : (r2 < 75) ? 0 : 2;
            family = 3;
            break;
        case 6:   /* light hauler */
            arch = (r2 < 35) ? 3 : (r2 < 75) ? 0 : 2;
            family = 5;
            break;
        case 7:   /* hauler */
            arch = (r2 < 40) ? 3 : 0;
            family = 5;
            break;
        case 8:   /* heavy hauler */
            arch = (r2 < 60) ? 0 : 3;
            family = 5;
            break;
        case 99:  /* debug: force saucer (variety audits) */
            arch = 3; family = 5;
            break;
        default:  /* dreadnought: long warships of all schools */
            arch = (r2 < 55) ? 0 : (r2 < 80) ? 1 : 0;
            family = (r2 < 55) ? 4 : 3;     /* cruiser / foil / gunship */
            break;
        }
    }
archdone:;
    float len = (family == 0) ? rndf(8, 12)
              : (family == 4) ? rndf(18, 26)
              : (family == 5) ? rndf(14, 20)
              : rndf(10, 16);
    if (hint == 0) len = rndf(7, 10);
    if (hint == 9) len = rndf(20, 28);
    if (arch == 2) len = rndf(5, 8);              /* pods are compact */
    if (arch == 3) len = rndf(9, 15);
    float zb = -len * 0.5f, zf = len * 0.5f;
    float w_mid = len * ((family == 5) ? rndf(0.16f, 0.22f)
                                       : rndf(0.09f, 0.14f));
    float flat = (family == 5) ? rndf(0.65f, 1.0f) : rndf(0.36f, 0.74f);
    float ch = rndf(0.22f, 0.68f);           /* section chamfer */
    float rake = rndf(-0.06f, 0.22f) * len;  /* spine curve (can droop) */
    float nose_len = len * ((family == 2) ? rndf(0.30f, 0.42f)
                                          : rndf(0.18f, 0.30f));


    if (arch == 1) {
        /* --- X-FOIL: slim fuselage + 4 wings in X + tip cannons ------- */
        float zb2 = -len * 0.5f, zf2 = len * 0.5f;
        float w = len * rndf(0.06f, 0.09f);
        float h = w * rndf(0.9f, 1.2f);
        int a[8], b[8], c[8], d[8];
        ring(zb2, w * 0.8f, h * 0.8f, 0, 0.45f, a);
        ring(zb2 + len * 0.3f, w, h, 0, 0.45f, b);
        ring(zb2 + len * 0.62f, w * 0.95f, h * 1.05f, h * 0.1f, 0.5f, c);
        ring(zf2 - len * 0.22f, w * 0.55f, h * 0.6f, 0, 0.45f, d);
        skin(a, b, HULL, HULL, HULL2);
        skin(b, c, HULL, HULL, HULL2);
        skin(c, d, HULL, HULL, HULL2);
        cap_back(a, GLOW);
        nose_apex(d, 0, 0, zf2, HULL2);
        /* canopy bump */
        {
            int e[8], f2[8];
            ring(zb2 + len * 0.42f, w * 0.5f, w * 0.3f, h, 0.5f, e);
            ring(zb2 + len * 0.58f, w * 0.35f, w * 0.18f, h * 0.9f, 0.5f, f2);
            skin(e, f2, GLASS, GLASS, HULL);
            cap_back(e, HULL);
            cap_front(f2, HULL2);
        }
        /* Foils: the X is just one configuration now — dihedrals roll
         * independently per pair, pairs can scissor fore/aft, sweep
         * can run forward, and some frames are Y-foils (user req:
         * every X-wing had the same angles). */
        float span = len * rndf(0.30f, 0.50f);
        float dihed_u = span * rndf(0.25f, 0.95f);
        float dihed_l = span * rndf(0.25f, 0.95f);
        float wz0 = zb2 + len * rndf(0.10f, 0.26f);
        float wz1 = wz0 + len * rndf(0.14f, 0.24f);
        float scissor = len * rndf(0.0f, 0.14f);     /* lower pair aft */
        float sweep = len * rndf(-0.06f, 0.14f);     /* - = forward */
        int yfoil = (rnd() % 5u) == 0;               /* 20%: 3 foils */
        float gl = len * rndf(0.16f, 0.32f);
        uint16_t GMUZ = RGB565C(40, 40, 48);
        /* upper pair (or single dorsal blade on Y-foils) */
        if (yfoil) {
            fin(0, wz0, wz1, h * 0.8f, w * 0.18f,
                dihed_u * 1.2f, wz0 - sweep, wz0 - sweep + len * 0.1f,
                1.0f, HULL2);
        } else {
            wings(wz0, wz1, w * 0.9f, h * 0.4f, h * 0.14f,
                  w + span, wz0 - sweep, wz0 - sweep + len * 0.1f,
                  h * 0.4f + dihed_u, HULL2);
            if (s1) {
                gun_v2(w + span, h * 0.4f + dihed_u, wz0 - sweep, gl,
                       w * 0.15f, ACC, GMUZ);
                gun_v2(-(w + span), h * 0.4f + dihed_u, wz0 - sweep, gl,
                       w * 0.15f, ACC, GMUZ);
            } else
            {
                tip_gun(w + span, h * 0.4f + dihed_u, wz0 - sweep, gl,
                        w * 0.16f, ACC, GMUZ);
                tip_gun(-(w + span), h * 0.4f + dihed_u, wz0 - sweep, gl,
                        w * 0.16f, ACC, GMUZ);
            }
        }
        /* lower pair, possibly scissored aft with its own dihedral */
        wings(wz0 + scissor, wz1 + scissor, w * 0.9f, -h * 0.4f,
              h * 0.14f, w + span, wz0 + scissor - sweep,
              wz0 + scissor - sweep + len * 0.1f,
              -h * 0.4f - dihed_l, HULL2);
        if (s1) {
            gun_v2(w + span, -h * 0.4f - dihed_l, wz0 + scissor - sweep,
                   gl, w * 0.15f, ACC, GMUZ);
            gun_v2(-(w + span), -h * 0.4f - dihed_l,
                   wz0 + scissor - sweep, gl, w * 0.15f, ACC, GMUZ);
        } else
        {
            tip_gun(w + span, -h * 0.4f - dihed_l, wz0 + scissor - sweep,
                    gl, w * 0.16f, ACC, GMUZ);
            tip_gun(-(w + span), -h * 0.4f - dihed_l,
                    wz0 + scissor - sweep, gl, w * 0.16f, ACC, GMUZ);
        }
        goto finish;
    }
    if (arch == 2) {
        /* --- PANEL POD: varied body + small symmetric panels ---------- */
        float r = len * rndf(0.30f, 0.40f);
        int body = rndi(0, 3);   /* 0 ball, 1 capsule, 2 twin, 3 angular */
        float px;                /* pylon anchor x */
        /* style-1 hatch-ring band: per-body waist position/size */
        float bz = 0, bw2 = 0, bh2 = 0, bch = 0.5f;
        if (body == 1) {
            /* capsule: stretched 4-ring pod */
            int a[8], b[8], c[8], d[8];
            ring(-r * 1.6f, r * 0.55f, r * 0.55f, 0, 0.5f, a);
            ring(-r * 0.5f, r * 0.95f, r * 0.9f, 0, 0.55f, b);
            ring(r * 0.5f, r * 0.95f, r * 0.9f, 0, 0.55f, c);
            ring(r * 1.3f, r * 0.55f, r * 0.55f, 0, 0.5f, d);
            skin(a, b, HULL, HULL, HULL2);
            skin(b, c, HULL, HULL, HULL2);
            skin(c, d, HULL, HULL, HULL2);
            cap_back(a, GLOW);
            cap_front(d, GLASS);
            px = r * 0.95f;
            bz = 0; bw2 = r * 0.95f * 1.07f; bh2 = r * 0.9f * 1.07f;
            bch = 0.55f;
        } else if (body == 2) {
            /* twin: cockpit ball forward + engine block aft */
            int a[8], b[8], c[8];
            ring(-r * 1.7f, r * 0.7f, r * 0.7f, 0, 0.25f, a);
            ring(-r * 0.6f, r * 0.8f, r * 0.8f, 0, 0.3f, b);
            skin(a, b, HULL2, HULL2, HULL2);
            cap_back(a, GLOW);
            cap_front(b, HULL2);
            ring(-r * 0.3f, r * 0.6f, r * 0.6f, 0, 0.5f, a);
            ring(0.4f * r, r * 0.9f, r * 0.85f, 0, 0.55f, b);
            ring(r * 1.2f, r * 0.6f, r * 0.6f, 0, 0.5f, c);
            skin(a, b, HULL, HULL, HULL2);
            skin(b, c, HULL, HULL, HULL2);
            cap_back(a, HULL2);
            cap_front(c, GLASS);
            px = r * 0.9f;
            bz = r * 0.4f; bw2 = r * 0.9f * 1.07f;
            bh2 = r * 0.85f * 1.07f; bch = 0.55f;
        } else {
            /* ball (chamfer .55) or angular (chamfer .2) */
            float chb = (body == 3) ? rndf(0.15f, 0.3f) : rndf(0.5f, 0.6f);
            float rh = rndf(0.8f, 1.0f);
            int a[8], b[8], c[8];
            ring(-r, r * 0.55f, r * 0.55f, 0, chb, a);
            ring(0, r, r * rh, 0, chb, b);
            ring(r * 0.85f, r * 0.6f, r * 0.6f, 0, chb, c);
            skin(a, b, HULL, HULL, HULL2);
            skin(b, c, HULL, HULL, HULL2);
            cap_back(a, GLOW);
            cap_front(c, GLASS);
            px = r;
            bz = 0; bw2 = r * 1.06f; bh2 = r * rh * 1.06f; bch = chb;
        }
        if (s1) {
            /* hatch detail ring, slightly proud of the body waist */
            int e0[8], e1[8];
            float bt = r * rndf2(0.05f, 0.09f);
            uint16_t BANDC = (rnd2() & 1) ? ACC : RGB565C(62, 64, 72);
            ring(bz - bt, bw2, bh2, 0, bch, e0);
            ring(bz + bt, bw2, bh2, 0, bch, e1);
            skin(e0, e1, BANDC, BANDC, BANDC);
        }
        /* pylons */
        float pylon = r * rndf(1.4f, 1.8f);
        if (s1) {
            /* truss pylons: two angled spars per side, converging on
             * the panel hub — reads as a frame, not a stick */
            for (int sd = 0; sd < 2; sd++) {
                float sx = sd ? -1.0f : 1.0f;
                for (int sp = 0; sp < 2; sp++) {
                    float oy = sp ? -r * 0.20f : r * 0.20f;
                    float oz = sp ? -r * 0.12f : r * 0.12f;
                    float t = r * 0.06f;
                    int p0 = vtx(sx * px * 0.78f, oy - t, oz - t);
                    int p1 = vtx(sx * px * 0.78f, oy - t, oz + t);
                    int p2 = vtx(sx * px * 0.78f, oy + t, oz + t);
                    int p3 = vtx(sx * px * 0.78f, oy + t, oz - t);
                    float hy = oy * 0.3f, hz = oz * 0.3f;
                    int q0 = vtx(sx * pylon, hy - t, hz - t);
                    int q1 = vtx(sx * pylon, hy - t, hz + t);
                    int q2 = vtx(sx * pylon, hy + t, hz + t);
                    int q3 = vtx(sx * pylon, hy + t, hz - t);
                    if (sd == 0) {
                        quad(p1, q1, q2, p2, HULL2);
                        quad(q0, p0, p3, q3, HULL2);
                        quad(p2, q2, q3, p3, HULL2);
                        quad(q1, p1, p0, q0, HULL2);
                    } else {
                        quad(q1, p1, p2, q2, HULL2);
                        quad(p0, q0, q3, p3, HULL2);
                        quad(q2, p2, p3, q3, HULL2);
                        quad(p1, q1, q0, p0, HULL2);
                    }
                }
            }
        } else
        for (int sd = 0; sd < 2; sd++) {
            float sx = sd ? -1.0f : 1.0f;
            int p0 = vtx(sx * px * 0.8f, -r * 0.12f, -r * 0.18f);
            int p1 = vtx(sx * px * 0.8f, -r * 0.12f, r * 0.18f);
            int p2 = vtx(sx * px * 0.8f, r * 0.12f, r * 0.18f);
            int p3 = vtx(sx * px * 0.8f, r * 0.12f, -r * 0.18f);
            int q0 = vtx(sx * pylon, -r * 0.1f, -r * 0.15f);
            int q1 = vtx(sx * pylon, -r * 0.1f, r * 0.15f);
            int q2 = vtx(sx * pylon, r * 0.1f, r * 0.15f);
            int q3 = vtx(sx * pylon, r * 0.1f, -r * 0.15f);
            if (sd == 0) {
                quad(p1, q1, q2, p2, HULL2);
                quad(q0, p0, p3, q3, HULL2);
                quad(p2, q2, q3, p3, HULL2);
                quad(q1, p1, p0, q0, HULL2);
            } else {
                quad(q1, p1, p2, q2, HULL2);
                quad(p0, q0, q3, p3, HULL2);
                quad(q2, p2, p3, q3, HULL2);
                quad(p1, q1, q0, p0, HULL2);
            }
        }
        /* Chin guns under the cockpit ball. */
        if (s1) {
            /* same rnd() count as the else branch keeps the twin's
             * downstream genes aligned with its style-0 sibling */
            gun_twin(r * 0.35f, -r * 0.55f, r * 0.5f,
                     r * rndf(0.5f, 0.9f) * 1.2f, r * 0.09f, HULL2,
                     RGB565C(40, 40, 48));
        } else
        gun_pair(r * 0.35f, -r * 0.55f, r * 0.5f, r * rndf(0.5f, 0.9f),
                 r * 0.08f, HULL2, RGB565C(40, 40, 48));

        /* Panels: COMPACT, fore-aft SYMMETRIC shapes only. */
        {
            int style = rndi(0, 3);
            float ph = r * rndf(0.9f, 1.5f);
            float pz = r * rndf(0.7f, 1.1f);
            float cant = r * rndf(-0.3f, 0.4f);
            float zz[6], yy[6];
            int np;
            switch (style) {
            case 0:   /* classic hex */
                np = 6;
                zz[0] = 0;          yy[0] = -ph;
                zz[1] = pz * 0.85f; yy[1] = -ph * 0.45f;
                zz[2] = pz * 0.85f; yy[2] = ph * 0.45f;
                zz[3] = 0;          yy[3] = ph;
                zz[4] = -pz * 0.85f; yy[4] = ph * 0.45f;
                zz[5] = -pz * 0.85f; yy[5] = -ph * 0.45f;
                break;
            case 1:   /* chamfered rectangle (classic TIE) */
                np = 6;
                zz[0] = pz * 0.55f;  yy[0] = -ph;
                zz[1] = pz;          yy[1] = -ph * 0.55f;
                zz[2] = pz;          yy[2] = ph * 0.55f;
                zz[3] = pz * 0.55f;  yy[3] = ph;
                zz[4] = -pz * 0.55f; yy[4] = ph;
                zz[5] = -pz * 0.55f; yy[5] = -ph;
                break;
            case 2:   /* symmetric trapezoid, narrow top */
                np = 4;
                zz[0] = pz;   yy[0] = -ph;
                zz[1] = pz * 0.5f;  yy[1] = ph;
                zz[2] = -pz * 0.5f; yy[2] = ph;
                zz[3] = -pz;  yy[3] = -ph;
                break;
            default:  /* slim vertical blade */
                np = 4;
                zz[0] = pz * 0.4f;  yy[0] = -ph;
                zz[1] = pz * 0.4f;  yy[1] = ph;
                zz[2] = -pz * 0.4f; yy[2] = ph;
                zz[3] = -pz * 0.4f; yy[3] = -ph;
                break;
            }
            if (s1) {
                /* TIE-style panel: raised outer frame loop around an
                 * inset dark panel face, plus a hub boss at the pylon */
                float cy2 = 0, cz2 = 0;
                for (int k = 0; k < np; k++) { cy2 += yy[k]; cz2 += zz[k]; }
                cy2 /= (float)np; cz2 /= (float)np;
                uint16_t PNL = RGB565C(38, 44, 60);
                for (int sd = 0; sd < 2; sd++) {
                    float sxn = sd ? -1.0f : 1.0f;
                    int rim[6], fro[6], inn[6];
                    for (int k = 0; k < np; k++) {
                        float ay = yy[k] < 0 ? -yy[k] : yy[k];
                        float xo = pylon + cant * (ay / ph);
                        inn[k] = vtx(sxn * xo, yy[k], zz[k]);
                        rim[k] = vtx(sxn * (xo + r * 0.14f), yy[k], zz[k]);
                        fro[k] = vtx(sxn * (xo + r * 0.05f),
                                     cy2 + (yy[k] - cy2) * 0.70f,
                                     cz2 + (zz[k] - cz2) * 0.70f);
                    }
                    for (int k = 0; k < np; k++) {
                        int k2 = (k + 1) % np;
                        if (sd == 0) {
                            quad(rim[k], rim[k2], fro[k2], fro[k], ACC);
                            quad(inn[k], inn[k2], rim[k2], rim[k], HULL2);
                        } else {
                            quad(rim[k2], rim[k], fro[k], fro[k2], ACC);
                            quad(inn[k2], inn[k], rim[k], rim[k2], HULL2);
                        }
                    }
                    for (int k = 1; k < np - 1; k++) {
                        if (sd == 0) {
                            face(fro[0], fro[k], fro[k + 1], PNL);
                            face(inn[0], inn[k + 1], inn[k], HULL2);
                        } else {
                            face(fro[0], fro[k + 1], fro[k], PNL);
                            face(inn[0], inn[k], inn[k + 1], HULL2);
                        }
                    }
                    /* hub boss where the truss meets the panel */
                    drum_x(sxn * (pylon - r * 0.30f),
                           sxn * (pylon + r * 0.16f), 0, 0,
                           r * 0.20f, r * 0.20f, HULL2,
                           RGB565C(50, 52, 60));
                }
                goto finish;
            }
            for (int sd = 0; sd < 2; sd++) {
                float sxn = sd ? -1.0f : 1.0f;
                int outer[6], inner[6];
                for (int k = 0; k < np; k++) {
                    float ay = yy[k] < 0 ? -yy[k] : yy[k];
                    float xo = pylon + cant * (ay / ph);
                    outer[k] = vtx(sxn * (xo + r * 0.06f), yy[k], zz[k]);
                    inner[k] = vtx(sxn * xo, yy[k], zz[k]);
                }
                for (int k = 1; k < np - 1; k++) {
                    if (sd == 0) {
                        face(outer[0], outer[k], outer[k + 1], ACC);
                        face(inner[0], inner[k + 1], inner[k], HULL2);
                    } else {
                        face(outer[0], outer[k + 1], outer[k], ACC);
                        face(inner[0], inner[k], inner[k + 1], HULL2);
                    }
                }
            }
        }
        goto finish;
    }
    if (arch == 3) {
        /* --- DISC FREIGHTER (Falcon-inspired rethink) -----------------
         * Built in the TOP-VIEW plane: a convex elliptical outline with
         * a flattened front edge, extruded into a lens (thin top plate,
         * vertical rim band, thin bottom plate). Mandibles are slabs
         * off the flat front edge, flush with the rim by construction.
         * Cockpit tube embeds INTO the disc so the join is buried. */
        float az = len * 0.5f;                       /* half-length */
        float ax = az * rndf(0.7f, 1.05f);           /* half-width */
        float ry = len * rndf(0.045f, 0.07f);        /* rim half-height */
        float py = ry * rndf(1.9f, 2.6f);            /* lens half-height */
        float ml = (rndi(0, 99) < 80) ? len * rndf(0.18f, 0.42f) : 0.0f;
        float front_k = ml > 0 ? rndf(0.35f, 0.55f) : 0.85f;
        float frontz = az * front_k;                 /* flat front edge */

        int N = 12;
        float ox[12], oz[12];
        for (int i2 = 0; i2 < N; i2++) {
            float th = (float)i2 * (6.2831853f / (float)N) + 0.2618f;
            ox[i2] = ax * sinf(th);
            oz[i2] = az * cosf(th);
            if (oz[i2] > frontz) oz[i2] = frontz;    /* flatten the bow */
        }
        int rimT[12], rimB[12], capT[12], capB[12];
        for (int i2 = 0; i2 < N; i2++) {
            rimT[i2] = vtx(ox[i2], ry, oz[i2]);
            rimB[i2] = vtx(ox[i2], -ry, oz[i2]);
            capT[i2] = vtx(ox[i2] * 0.70f, py, oz[i2] * 0.70f);
            capB[i2] = vtx(ox[i2] * 0.70f, -py, oz[i2] * 0.70f);
        }
        for (int i2 = 0; i2 < N; i2++) {
            int j2 = (i2 + 1) % N;
            /* Rim band — stern arc glows (drive slit). */
            uint16_t rc = (oz[i2] < -az * 0.62f && oz[j2] < -az * 0.62f)
                              ? GLOW : HULL2;
            quad(rimB[i2], rimB[j2], rimT[j2], rimT[i2], rc);
            /* Plate slopes. */
            quad(rimT[i2], rimT[j2], capT[j2], capT[i2],
                 (i2 & 1) ? HULL : HULL2);
            quad(rimB[j2], rimB[i2], capB[i2], capB[j2], HULL);
        }
        for (int i2 = 1; i2 < N - 1; i2++) {         /* lens caps */
            face(capT[0], capT[i2], capT[i2 + 1], HULL);
            face(capB[0], capB[i2 + 1], capB[i2], HULL2);
        }

        /* Mandibles: rectangular slabs off the flat bow, rim-flush. */
        if (ml > 0) {
            float gap = ax * rndf(0.14f, 0.3f);
            float pw = ax * rndf(0.16f, 0.26f);
            /* Wide taper space: from near-parallel rails (0.9) down to
             * needle points (0.15); height taper rolls independently
             * so prongs can be blade-flat, knife-thin, or chunky. */
            float tw = rndf(0.15f, 0.9f);    /* tip width fraction */
            float th2 = rndf(0.25f, 0.9f);   /* tip height fraction */
            float xo = gap + pw * 2.0f;      /* outer root edge */
            float xt = gap + pw * 2.0f * tw; /* outer tip edge */
            uint16_t MUZ2 = RGB565C(40, 40, 48);
            /* Right prong: inner wall straight at +gap, outer tapers. */
            prong(gap, xo, gap, xt, ry * 0.95f, ry * 0.95f * th2,
                  frontz, frontz + ml, HULL, HULL2, MUZ2);
            /* Left prong: mirrored extents, same winding path. */
            prong(-xo, -gap, -xt, -gap, ry * 0.95f, ry * 0.95f * th2,
                  frontz, frontz + ml, HULL, HULL2, MUZ2);
        }

        /* Cockpit: embedded tube emerging at one rim edge. */
        {
            float side = (rnd() & 1) ? 1.0f : -1.0f;
            float cx2 = side * ax * rndf(0.5f, 0.68f);
            float tip = frontz + (ml > 0 ? ml * rndf(0.3f, 0.7f)
                                         : az * 0.18f);
            int e[8], f2[8];
            ring(-az * 0.1f, ry * 1.15f, ry * 1.05f, 0, 0.5f, e);
            ring(tip, ry * 0.85f, ry * 0.8f, 0, 0.5f, f2);
            for (int k = 0; k < 8; k++) {
                s_fx[e[k]] += cx2; s_fx[f2[k]] += cx2;
            }
            skin(e, f2, HULL2, HULL2, HULL2);
            cap_front(f2, GLASS);
        }

        /* Chin gun(s) under the bow — mandible ships gun the notch,
         * pure discs carry a belly turret. */
        if (ml > 0) {
            if (s1) {
                gun_v2(0, -ry * 0.4f, frontz, ml * rndf(0.35f, 0.6f) * 1.15f,
                       ry * 0.45f, HULL2, RGB565C(40, 40, 48));
            } else
            tip_gun(0, -ry * 0.4f, frontz, ml * rndf(0.35f, 0.6f),
                    ry * 0.5f, HULL2, RGB565C(40, 40, 48));
        } else {
            if (s1) {
                gun_twin(ax * 0.25f, -py * 0.8f, az * 0.4f,
                         az * rndf(0.15f, 0.25f) * 1.4f, ry * 0.4f,
                         HULL2, RGB565C(40, 40, 48));
            } else
            gun_pair(ax * 0.25f, -py * 0.8f, az * 0.4f,
                     az * rndf(0.15f, 0.25f), ry * 0.45f, HULL2,
                     RGB565C(40, 40, 48));
        }

        /* Top dome (sensor bump), often present, sometimes offset. */
        if (rndi(0, 2)) {
            float dr = ax * rndf(0.14f, 0.24f);
            float dx2 = (rndi(0, 2) == 0) ? rndf(-0.3f, 0.3f) * ax : 0.0f;
            int d0[8], d1[8];
            ring(-az * 0.05f, dr, dr * 0.8f, py * 0.85f, 0.55f, d0);
            ring(-az * 0.05f + dr * 0.8f, dr * 0.55f, dr * 0.45f,
                 py * 0.85f + ry, 0.55f, d1);
            for (int k = 0; k < 8; k++) {
                s_fx[d0[k]] += dx2; s_fx[d1[k]] += dx2;
            }
            skin(d0, d1, HULL2, HULL2, HULL2);
            cap_front(d1, (rnd() & 1) ? GLASS : ACC);
        }
        if (s1 && hint >= 6 && hint <= 8) {
            /* cargo character: lofted cargo drums clamped on the rim
             * (read from every angle, never bare boxes) */
            float tr = ax * rndf2(0.18f, 0.24f);
            float tz0 = -az * rndf2(0.35f, 0.45f);
            float tz1 = az * rndf2(0.15f, 0.30f);
            float tx = ax * rndf2(0.95f, 1.05f);
            nacelle(tx, 0, tz0, tz1, tr, HULL, HULL2, 1);
            if (rnd2() & 1) {
                /* belly sensor dome (mirrors the dorsal one) */
                float dr = ax * rndf2(0.12f, 0.18f);
                int d0[8], d1[8];
                ring(-az * 0.05f, dr, dr * 0.8f, -py * 0.85f, 0.55f, d0);
                ring(-az * 0.05f + dr * 0.8f, dr * 0.55f, dr * 0.45f,
                     -py * 0.85f - ry, 0.55f, d1);
                skin(d0, d1, HULL2, HULL2, HULL2);
                cap_front(d1, HULL2);
            }
        }
        goto finish;
    }

    /* Stations: tail, aft, mid, fore; then nose apex. */
    float z0 = zb, z1 = zb + len * 0.22f, z2 = zb + len * rndf(0.45f, 0.6f),
          z3 = zf - nose_len;
    float w0 = w_mid * rndf(0.55f, 0.8f), w1 = w_mid * rndf(0.9f, 1.0f);
    float w3 = w_mid * rndf(0.45f, 0.65f);
    float h0 = w0 * flat, h1 = w1 * flat, h2 = w_mid * flat, h3 = w3 * flat;
    float y0 = rake * 0.5f, y1 = rake * 0.2f, y3 = -rake * 0.1f;

    int rA[8], rB[8], rC[8], rD[8];
    ring(z0, w0, h0, y0, ch, rA);
    ring(z1, w1, h1, y1, ch, rB);
    ring(z2, w_mid, h2, 0, ch, rC);
    ring(z3, w3, h3, y3, ch * 0.8f, rD);
    skin(rA, rB, HULL, HULL, HULL2);
    skin(rB, rC, HULL, HULL, HULL2);
    skin(rC, rD, HULL, HULL, HULL2);
    if (s1 && (family == 4 || family == 5)) {
        /* engine cluster: dark stern plate + recessed glow nozzles
         * instead of one flat glow cap */
        cap_back(rA, HULL2);
        float er = (h0 < w0 ? h0 : w0);
        if (family == 5) {
            float nr = (er * 0.55f < w0 * 0.40f) ? er * 0.55f : w0 * 0.40f;
            nozzle6(w0 * 0.44f, y0, z0, nr, nr * 0.6f, HULL2, GLOW);
            nozzle6(-w0 * 0.44f, y0, z0, nr, nr * 0.6f, HULL2, GLOW);
        } else {
            float nr = (er * 0.85f < w0 * 0.36f) ? er * 0.85f : w0 * 0.36f;
            nozzle6(w0 * 0.42f, y0, z0, nr, nr * 1.1f, HULL2, GLOW);
            nozzle6(-w0 * 0.42f, y0, z0, nr, nr * 1.1f, HULL2, GLOW);
        }
    } else
    cap_back(rA, GLOW);                       /* integrated engine tail */
    if (s1 && family == 5) {
        /* blunt tug prow: lofted bow block, clear front face */
        int rE[8];
        ring(zf, w3 * 0.45f, h3 * 0.45f, y3 - h3 * 0.15f, ch * 0.8f, rE);
        skin(rD, rE, HULL, HULL, HULL2);
        cap_front(rE, HULL2);
    } else
    nose_apex(rD, 0, y3 - h3 * 0.3f, zf, HULL2);

    /* --- canopy: small glass loft on the fore-mid deck ----------------- */
    if (family != 5 || rndi(0, 1)) {
        float cz0 = z2 - len * 0.05f, cz1 = z2 + len * rndf(0.12f, 0.2f);
        float cw = w_mid * rndf(0.28f, 0.4f);
        int c0[8], c1[8];
        ring(cz0, cw, cw * 0.5f, h2 * 0.95f, 0.5f, c0);
        ring(cz1, cw * 0.6f, cw * 0.3f, h2 * 0.8f, 0.5f, c1);
        skin(c0, c1, GLASS, GLASS, HULL);
        cap_back(c0, HULL);
        cap_front(c1, HULL2);
    }

    /* --- family attachments ------------------------------------------- */
    float span = len * rndf(0.35f, 0.55f);
    switch (family) {
    case 0:   /* dart: stub fins only */
        wings(z0 + len * 0.05f, z0 + len * 0.3f, w_mid * 0.8f, 0,
              h2 * 0.12f, w_mid * 0.8f + span * 0.45f,
              z0, z0 + len * 0.12f, h2 * 0.4f, ACC);
        break;
    case 1: { /* fighter: swept main wings + tail fin */
        float sweep = rndf(0.15f, 0.4f) * len;
        wings(z1, z1 + len * rndf(0.25f, 0.35f), w1 * 0.85f, 0,
              h1 * 0.10f, w1 + span,
              z1 - sweep, z1 - sweep + len * 0.10f,
              rndf(-0.5f, 1.2f) * h1, HULL2);
        fin(0, z0 + len * 0.02f, z0 + len * 0.2f, h0 * 0.8f,
            w_mid * 0.05f, h0 * rndf(1.2f, 2.2f),
            z0, z0 + len * 0.1f, 1.0f, ACC);
        break;
    }
    case 2: { /* interceptor: canards + rear wings */
        wings(z3 - len * 0.08f, z3, w3 * 0.9f, 0, h3 * 0.12f,
              w3 + span * 0.4f, z3 - len * 0.14f, z3 - len * 0.05f,
              h3 * 0.3f, ACC);
        float sweep = rndf(0.2f, 0.35f) * len;
        wings(z1, z1 + len * 0.22f, w1 * 0.85f, 0, h1 * 0.10f,
              w1 + span * 0.8f, z1 - sweep, z1 - sweep + len * 0.09f,
              h1 * rndf(0.2f, 0.9f), HULL2);
        break;
    }
    case 3: { /* gunship / heavy fighter */
        float px = w_mid * rndf(0.4f, 0.85f);
        int npr = rndi(1, 2) * 2;            /* 2 or 4 prongs */
        uint16_t MUZ = RGB565C(40, 40, 48);
        if (s1) {
            /* MAULER-class premium fighters (user: boring + guns float
             * off the hull). FOUR distinct silhouettes, and every gun is
             * physically seated on a pod / wing / nose shoulder so it
             * always meets the ship. */
            int gstyle = (int)(rnd2() % 4u);
            float gl = len * rndf2(0.14f, 0.20f);
            float gr = w_mid * 0.10f;
            if (gstyle == 0) {
                /* TWIN-SPONSON DESTROYER: a weapon pod each side, its
                 * nose carrying a forward cannon (gun meets pod). */
                float spx = w_mid * rndf2(0.95f, 1.15f);
                float pz1 = z3 + len * 0.04f;
                nacelle(spx, -h2 * 0.10f, z1, pz1,
                        w_mid * rndf2(0.30f, 0.40f), HULL2, GLOW, 1);
                gun_pair(spx, -h2 * 0.10f, pz1, gl, gr, ACC, MUZ);
                fin(0, z0 + len * 0.02f, z0 + len * 0.20f, h0 * 0.8f,
                    w_mid * 0.05f, h0 * 1.7f, z0, z0 + len * 0.08f, 1.0f,
                    ACC);
            } else if (gstyle == 1) {
                /* GUNWING: short swept wings with wingtip cannons. */
                float span = len * rndf2(0.26f, 0.42f);
                float wty = h2 * rndf2(-0.10f, 0.55f);
                wings(z2 - len * 0.04f, z2 + len * 0.12f, w_mid * 0.82f, 0,
                      h2 * 0.10f, w_mid + span, z2 - len * 0.14f,
                      z2 - len * 0.06f, wty, HULL2);
                gun_pair(w_mid + span, wty, z2 - len * 0.14f, gl * 1.15f,
                         gr * 0.9f, ACC, MUZ);
                fin(0, z0 + len * 0.02f, z0 + len * 0.18f, h0 * 0.7f,
                    w_mid * 0.05f, h0 * 1.5f, z0, z0 + len * 0.08f, 1.0f,
                    ACC);
            } else if (gstyle == 2) {
                /* NOSE LANCE: 2 or 4 cannons flush on the nose shoulders
                 * (mount x clamped to the local hull half-width). */
                float gpx = w3 * 0.80f;
                int n2 = (rnd2() & 1u) ? 2 : 4;
                float stag = h3 * 0.50f;
                for (int s2 = 0; s2 < n2; s2++) {
                    float sx = (s2 & 1) ? -gpx : gpx;
                    float by = y3 + ((n2 == 2) ? 0.0f
                                    : (s2 >= 2) ? -stag : stag);
                    int g0[8], g1[8];
                    ring(z3 + len * 0.05f, gr, gr, by, 0.4f, g0);
                    ring(zf - len * 0.02f, gr * 0.8f, gr * 0.8f, by, 0.4f,
                         g1);
                    for (int k = 0; k < 8; k++) { s_fx[g0[k]] += sx;
                                                  s_fx[g1[k]] += sx; }
                    skin(g0, g1, ACC, ACC, ACC);
                    cap_back(g0, HULL2);
                    int m0[6], m1[6];
                    hex6(zf - len * 0.02f, gr * 0.6f, sx, by, m0);
                    hex6(zf + len * 0.04f, gr * 0.8f, sx, by, m1);
                    skin6(m0, m1, HULL2);
                    fan6f(m1, MUZ);
                }
                fin(w_mid * 0.5f, z0, z0 + len * 0.16f, h0 * 0.6f,
                    w_mid * 0.05f, h0 * 1.6f, z0, z0 + len * 0.08f, 1.0f,
                    ACC);
                fin(-w_mid * 0.5f, z0, z0 + len * 0.16f, h0 * 0.6f,
                    w_mid * 0.05f, h0 * 1.6f, z0, z0 + len * 0.08f, 1.0f,
                    ACC);
            } else {
                /* CHIN GONDOLA: under-slung weapon block + twin chin
                 * cannons (gondola meets the belly; guns meet gondola). */
                float gz1 = z3 + len * 0.02f;
                nacelle(0, -h2 * 0.88f, z2 - len * 0.05f, gz1,
                        w_mid * rndf2(0.34f, 0.46f), HULL2, GLOW, 0);
                gun_twin(0, -h2 * 0.88f, gz1, gl * 1.15f, gr, HULL2, MUZ);
                fin(0, z1, z1 + len * 0.22f, h1 * 0.85f, w_mid * 0.06f,
                    h1 * rndf2(1.6f, 2.4f), z1, z1 + len * 0.12f, 1.0f,
                    ACC);
            }
        } else {
            for (int s2 = 0; s2 < npr; s2++) {
                float sx = (s2 & 1) ? -px : px;
                int g0[8], g1[8];
                ring(z3 - len * 0.05f, w_mid * 0.10f, w_mid * 0.10f, 0,
                     0.4f, g0);
                ring(zf + len * 0.08f, w_mid * 0.07f, w_mid * 0.07f, 0,
                     0.4f, g1);
                for (int k = 0; k < 8; k++) { s_fx[g0[k]] += sx;
                                              s_fx[g1[k]] += sx; }
                skin(g0, g1, ACC, ACC, ACC);
                cap_back(g0, HULL2);
                cap_front(g1, RGB565C(40, 40, 48));
            }
            fin(w_mid * 0.55f, z0, z0 + len * 0.16f, h0 * 0.6f,
                w_mid * 0.05f, h0 * 1.6f, z0, z0 + len * 0.08f, 1.0f, ACC);
            fin(-w_mid * 0.55f, z0, z0 + len * 0.16f, h0 * 0.6f,
                w_mid * 0.05f, h0 * 1.6f, z0, z0 + len * 0.08f, 1.0f, ACC);
        }
        break;
    }
    case 4: { /* cruiser: four superstructure schools (user req: the
                 dreadnought class read as clones) */
        int cstyle = rndi(0, 3);
        if (cstyle == 0) {
            /* classic: dorsal + ventral fins, mid winglets */
            fin(0, z1, z1 + len * 0.25f, h1 * 0.9f, w_mid * 0.06f,
                h1 * rndf(1.4f, 2.4f), z1, z1 + len * 0.12f, 1.0f, HULL2);
            fin(0, z1 + len * 0.05f, z1 + len * 0.2f, -h1 * 0.9f,
                w_mid * 0.05f, h1 * 1.2f, z1 + len * 0.05f,
                z1 + len * 0.12f, -1.0f, ACC);
            wings(z2 - len * 0.05f, z2 + len * 0.1f, w_mid * 0.9f, 0,
                  h2 * 0.10f, w_mid + span * 0.5f, z2 - len * 0.12f,
                  z2 - len * 0.04f, 0, HULL2);
        } else if (cstyle == 1) {
            /* twin canted dorsal blades + belly keel */
            for (int sd2 = 0; sd2 < 2; sd2++) {
                float sx2 = sd2 ? -1.0f : 1.0f;
                fin(sx2 * w_mid * 0.45f, z1, z1 + len * 0.20f,
                    h1 * 0.8f, w_mid * 0.05f, h1 * rndf(1.2f, 2.0f),
                    z1, z1 + len * 0.10f, 1.0f, ACC);
            }
            fin(0, z1, z1 + len * rndf(0.3f, 0.45f), -h1 * 0.9f,
                w_mid * 0.06f, h1 * rndf(0.8f, 1.4f),
                z1, z1 + len * 0.2f, -1.0f, HULL2);
        } else if (cstyle == 2) {
            /* side sponsons (gun cheeks) + small tail fin */
            nacelle(w_mid * 1.05f, 0, z1, z2 + len * 0.05f,
                    w_mid * rndf(0.28f, 0.4f), HULL2, GLOW, 1);
            fin(0, z0 + len * 0.02f, z0 + len * 0.18f, h0 * 0.8f,
                w_mid * 0.05f, h0 * rndf(1.0f, 1.8f),
                z0, z0 + len * 0.08f, 1.0f, ACC);
        } else {
            /* spine of stepped fins down the back */
            int nf2 = rndi(3, 5);
            for (int k = 0; k < nf2; k++) {
                float fz = z1 + (z3 - z1) * (float)k / (float)nf2;
                fin(0, fz, fz + len * 0.07f, h1 * 0.85f,
                    w_mid * 0.05f, h1 * (0.7f + 0.35f * (float)k),
                    fz, fz + len * 0.05f, 1.0f,
                    (k & 1) ? ACC : HULL2);
            }
        }
        break;
    }
    default:  /* hauler: side pods (cargo nacelles) */
        if (s1) {
            /* deliberate cargo massing: shoulder-mounted saddle drums
             * plus a belly keel tank (lofted, chamfered — no boxes) */
            float pr = w_mid * rndf(0.35f, 0.5f);  /* shared roll */
            nacelle(w_mid * 0.95f, h2 * 0.55f, z0 + len * 0.08f,
                    z2 - len * 0.04f, pr * 0.85f, HULL, HULL2, 1);
            nacelle(0, -h2 * 0.9f, z0 + len * 0.14f, z2,
                    w_mid * rndf2(0.30f, 0.40f), HULL, HULL2, 0);
        } else
        nacelle(w_mid * 1.15f, 0, z0 + len * 0.1f, z2,
                w_mid * rndf(0.35f, 0.5f), HULL2, GLOW, 1);
        break;
    }

    /* Visible armament (user req: ships should show their guns). */
    {
        uint16_t MUZ = RGB565C(40, 40, 48);
        float gl2 = len * rndf(0.10f, 0.18f);
        switch (family) {
        case 0:   /* dart: single chin gun */
            if (s1) {
                gun_v2(0, -h3 * 0.8f, z3, gl2 * 1.35f, w_mid * 0.065f,
                       HULL2, MUZ);
                break;
            }
            tip_gun(0, -h3 * 0.8f, z3, gl2 * 1.3f, w_mid * 0.07f,
                    HULL2, MUZ);
            break;
        case 1:
        case 2:   /* fighters: twin chin barrels under the nose */
            if (s1) {
                /* same rnd() count as the else path (gene alignment) */
                gun_twin(w3 * rndf(0.4f, 0.65f), -h3 * 0.75f, z3,
                         gl2 * rndf(1.0f, 1.6f) * 1.1f, w_mid * 0.06f,
                         HULL2, MUZ);
                break;
            }
            gun_pair(w3 * rndf(0.4f, 0.7f), -h3 * 0.7f, z3,
                     gl2 * rndf(1.0f, 1.6f), w_mid * 0.06f, HULL2, MUZ);
            break;
        case 3:   /* gunship: prongs already; add a top barrel */
            if (s1) {
                gun_v2(0, h2 * 0.9f, z2, gl2 * 1.2f, w_mid * 0.06f,
                       ACC, MUZ);
                break;
            }
            tip_gun(0, h2 * 0.9f, z2, gl2, w_mid * 0.07f, ACC, MUZ);
            break;
        case 4:   /* cruiser: sponson barrels along both flanks */
            if (s1) {
                /* recessed flank turrets: low mount pad + stepped gun */
                for (int sd = 0; sd < 2; sd++) {
                    float sx = sd ? -1.0f : 1.0f;
                    slab(sx * w_mid * 0.92f, h2 * 0.45f, z2 - len * 0.02f,
                         w_mid * 0.15f, h2 * 0.16f, len * 0.05f,
                         HULL2, HULL2, HULL2);
                    gun_v2(sx * w_mid * 0.92f, h2 * 0.45f,
                           z2 + len * 0.03f, gl2 * 1.5f, w_mid * 0.055f,
                           HULL2, MUZ);
                }
                break;
            }
            gun_pair(w_mid * 1.0f, 0, z2 + len * 0.05f,
                     gl2 * 1.2f, w_mid * 0.06f, HULL2, MUZ);
            gun_pair(w_mid * 0.9f, h2 * 0.5f, z1 + len * 0.08f,
                     gl2, w_mid * 0.05f, HULL2, MUZ);
            break;
        default:  /* hauler: one defensive top turret nub */
            if (s1) {
                /* turret moved aft of the canopy: low pad + stepped gun */
                slab(0, y1 + h1 * 0.92f, z1 + len * 0.10f, w_mid * 0.20f,
                     h1 * 0.14f, w_mid * 0.22f, HULL2, HULL2, HULL2);
                gun_v2(0, y1 + h1 * 1.08f, z1 + len * 0.10f, gl2 * 1.1f,
                       w_mid * 0.05f, HULL2, MUZ);
                break;
            }
            slab(0, h2 * 0.95f, z2, w_mid * 0.16f, h2 * 0.22f,
                 w_mid * 0.16f, HULL2, HULL2, HULL2);
            tip_gun(0, h2 * 1.05f, z2 + w_mid * 0.1f, gl2,
                    w_mid * 0.05f, HULL2, MUZ);
            break;
        }
    }

    /* Engine nacelles for fighters/interceptors/cruisers (50%). */
    if ((family == 1 || family == 2 || family == 4) && rndi(0, 1)) {
        /* s1 cruisers already carry a stern nozzle cluster + turrets;
         * the pods would also bust the face budget. Burn the shared
         * rolls so nothing downstream shifts, then skip. */
        if (s1 && family == 4) {
            (void)rndf(0.95f, 1.25f);
            (void)rndf(0.3f, 0.42f);
            (void)rndf(0.22f, 0.34f);
        } else
        nacelle(w_mid * rndf(0.95f, 1.25f), -h2 * 0.2f,
                z0, z0 + len * rndf(0.3f, 0.42f),
                w_mid * rndf(0.22f, 0.34f), HULL2, GLOW, 1);
    }


finish:;
    /* --- quantise ------------------------------------------------------ */
    float maxc = 1.0f, bound2 = 1.0f;
    for (int i = 0; i < s_nv; i++) {
        float ax = fabsf(s_fx[i]), ay = fabsf(s_fy[i]), az = fabsf(s_fz[i]);
        if (ax > maxc) maxc = ax;
        if (ay > maxc) maxc = ay;
        if (az > maxc) maxc = az;
        float d2 = s_fx[i] * s_fx[i] + s_fy[i] * s_fy[i] +
                   s_fz[i] * s_fz[i];
        if (d2 > bound2) bound2 = d2;
    }
    float q = 127.0f / maxc;
    for (int i = 0; i < s_nv; i++) {
        s_verts[i].x = (int8_t)(s_fx[i] * q);
        s_verts[i].y = (int8_t)(s_fy[i] * q);
        s_verts[i].z = (int8_t)(s_fz[i] * q);
    }
    s_mesh.verts = s_verts;
    s_mesh.faces = s_faces;
    s_mesh.face_colors = s_facecolors;
    s_mesh.color = 0;
    s_mesh.nverts = (uint16_t)s_nv;
    s_mesh.nfaces = (uint16_t)s_nf;
    s_mesh.scale = maxc;
    s_mesh.bound_r = sqrtf(bound2);
    s_mesh.lod_lo = 0;
    return &s_mesh;
}

const Mesh *ship_gen_mesh_class(uint32_t seed, int class_hint) {
    s_hint = class_hint;
    return ship_gen_mesh(seed);
}

int ship_gen_copy(MeshVert *verts, int max_v, MeshFace *faces, uint16_t *colors,
                  int max_f, Mesh *out) {
    int nv = s_mesh.nverts < max_v ? s_mesh.nverts : max_v;
    int nf = s_mesh.nfaces < max_f ? s_mesh.nfaces : max_f;
    for (int i = 0; i < nv; i++) verts[i] = s_verts[i];
    for (int i = 0; i < nf; i++) { faces[i] = s_faces[i]; colors[i] = s_facecolors[i]; }
    *out = s_mesh;
    out->verts = verts;
    out->faces = faces;
    out->face_colors = colors;
    out->nverts = (uint16_t)nv;
    out->nfaces = (uint16_t)nf;
    return nf;
}
