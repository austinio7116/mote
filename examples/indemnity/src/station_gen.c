/*
 * ThumbyElite — procedural station mesh generation.
 *
 * Growth algorithm (accretion, CA-flavoured):
 *   1. Core: box or octagonal drum, seeded proportions.
 *   2. 5-9 growth steps: pick an existing module, attach a new module
 *      to one of its free faces (habitat box / solar wing / spur /
 *      antenna fin), mirrored across X when the attachment is lateral.
 *   3. Docking bay: the core's +z face gets the glass bay + frame.
 *   4. Trim lights: accent blocks at module corners.
 *
 * Geometry is built in floats then quantised to the int8 mesh format
 * (same rules as obj2mesh). Static buffers — no allocation.
 */
#include "station_gen.h"
#include "elite_types.h"
#include <math.h>
#include <string.h>

#define MAX_SV 200
#define MAX_SF 340
#define MAX_MODULES 24

static MeshVert s_verts[MAX_SV];
static MeshFace s_faces[MAX_SF];
static Mesh     s_mesh;

/* Proposal-look switch (style lab — sheets only). */
static int s_style;
void station_gen_set_style(int s) { s_style = s; }

/* Float-space build buffers. */
static float s_fx[MAX_SV], s_fy[MAX_SV], s_fz[MAX_SV];
static int   s_nv, s_nf;

typedef struct { float cx, cy, cz, hx, hy, hz; } Module;
static Module s_mods[MAX_MODULES];
static int    s_nmods;

static uint32_t s_rng;
static uint32_t rnd(void) {
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return s_rng;
}
static float rndf(float lo, float hi) {
    return lo + (hi - lo) * (float)(rnd() & 0xFFFF) * (1.0f / 65535.0f);
}
static int rndi(int lo, int hi) {
    return lo + (int)(rnd() % (uint32_t)(hi - lo + 1));
}

static int add_vert(float x, float y, float z) {
    if (s_nv >= MAX_SV) return s_nv - 1;
    s_fx[s_nv] = x; s_fy[s_nv] = y; s_fz[s_nv] = z;
    return s_nv++;
}

static void add_face(int a, int b, int c, uint16_t color) {
    if (s_nf >= MAX_SF) return;
    MeshFace *f = &s_faces[s_nf++];
    f->a = (uint8_t)a; f->b = (uint8_t)b; f->c = (uint8_t)c;
    f->color = color;
    /* Normal from the float verts (quantised later with everything). */
    float ux = s_fx[b] - s_fx[a], uy = s_fy[b] - s_fy[a], uz = s_fz[b] - s_fz[a];
    float vx = s_fx[c] - s_fx[a], vy = s_fy[c] - s_fy[a], vz = s_fz[c] - s_fz[a];
    float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
    float l = sqrtf(nx * nx + ny * ny + nz * nz);
    if (l < 1e-9f) l = 1;
    f->nx = (int8_t)(nx / l * 127.0f);
    f->ny = (int8_t)(ny / l * 127.0f);
    f->nz = (int8_t)(nz / l * 127.0f);
}

/* Box with CCW-from-outside winding (gen_ships.py template).
 * face_colors: NULL or 6 entries: front back right left top bottom. */
static void box(float cx, float cy, float cz, float hx, float hy, float hz,
                uint16_t mtl, const uint16_t *fc) {
    if (s_nv + 8 > MAX_SV || s_nf + 12 > MAX_SF) return;
    int i0 = add_vert(cx - hx, cy - hy, cz - hz);
    int i1 = add_vert(cx + hx, cy - hy, cz - hz);
    int i2 = add_vert(cx + hx, cy + hy, cz - hz);
    int i3 = add_vert(cx - hx, cy + hy, cz - hz);
    int i4 = add_vert(cx - hx, cy - hy, cz + hz);
    int i5 = add_vert(cx + hx, cy - hy, cz + hz);
    int i6 = add_vert(cx + hx, cy + hy, cz + hz);
    int i7 = add_vert(cx - hx, cy + hy, cz + hz);
    uint16_t cf = fc ? fc[0] : mtl, cb = fc ? fc[1] : mtl;
    uint16_t cr = fc ? fc[2] : mtl, cl = fc ? fc[3] : mtl;
    uint16_t ct = fc ? fc[4] : mtl, cd = fc ? fc[5] : mtl;
    add_face(i4, i5, i6, cf); add_face(i4, i6, i7, cf);   /* front +z */
    add_face(i1, i0, i3, cb); add_face(i1, i3, i2, cb);   /* back  -z */
    add_face(i5, i1, i2, cr); add_face(i5, i2, i6, cr);   /* right +x */
    add_face(i0, i4, i7, cl); add_face(i0, i7, i3, cl);   /* left  -x */
    add_face(i7, i6, i2, ct); add_face(i7, i2, i3, ct);   /* top   +y */
    add_face(i0, i1, i5, cd); add_face(i0, i5, i4, cd);   /* bottom-y */
    if (s_nmods < MAX_MODULES)
        s_mods[s_nmods++] = (Module){ cx, cy, cz, hx, hy, hz };
}

#if 0   /* helpers used only by the old accretion generator */
/* Octagonal drum along z (the classic station core silhouette). */
static void drum(float r, float hz, uint16_t mtl, uint16_t face_col) {
    if (s_nv + 16 > MAX_SV || s_nf + 28 > MAX_SF) return;
    int ring0[8], ring1[8];
    for (int i = 0; i < 8; i++) {
        float a = (float)i * (6.2831853f / 8.0f) + 0.3927f;
        ring0[i] = add_vert(cosf(a) * r, sinf(a) * r, -hz);
        ring1[i] = add_vert(cosf(a) * r, sinf(a) * r, hz);
    }
    for (int i = 0; i < 8; i++) {
        int j = (i + 1) & 7;
        /* Side quad, outward. */
        add_face(ring0[i], ring0[j], ring1[j], mtl);
        add_face(ring0[i], ring1[j], ring1[i], mtl);
    }
    /* Caps (fans). +z cap CCW from +z; -z cap reversed. */
    for (int i = 1; i < 7; i++) {
        add_face(ring1[0], ring1[i], ring1[i + 1], face_col);
        add_face(ring0[0], ring0[i + 1], ring0[i], mtl);
    }
    if (s_nmods < MAX_MODULES)
        s_mods[s_nmods++] = (Module){ 0, 0, 0, r, r, hz };
}

/* Faceted ball: three octagonal rings + polar caps. */
static void ball(float r, uint16_t mtl, uint16_t face_col) {
    if (s_nv + 26 > MAX_SV || s_nf + 48 > MAX_SF) return;
    int rings[3][8];
    float zs[3] = { -r * 0.55f, 0, r * 0.55f };
    float rs[3] = { r * 0.68f, r, r * 0.68f };
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < 8; i++) {
            float a = (float)i * (6.2831853f / 8.0f) + 0.3927f;
            rings[k][i] = add_vert(cosf(a) * rs[k], sinf(a) * rs[k], zs[k]);
        }
    int south = add_vert(0, 0, -r * 0.95f);
    int north = add_vert(0, 0, r * 0.95f);
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < 8; i++) {
            int j = (i + 1) & 7;
            add_face(rings[k][i], rings[k][j], rings[k + 1][j], mtl);
            add_face(rings[k][i], rings[k + 1][j], rings[k + 1][i],
                     (i & 1) ? mtl : face_col);
        }
    for (int i = 0; i < 8; i++) {
        int j = (i + 1) & 7;
        add_face(rings[0][i], south, rings[0][j], mtl);
        add_face(rings[2][i], rings[2][j], north, face_col);
    }
    if (s_nmods < MAX_MODULES)
        s_mods[s_nmods++] = (Module){ 0, 0, 0, r, r, r };
}

/* Ring station: octagonal torus + hub drum + four spokes. */
static void ring_core(float R, float tube, uint16_t mtl, uint16_t mtl2,
                      uint16_t face_col) {
    if (s_nv + 32 > MAX_SV || s_nf + 64 > MAX_SF) return;
    int fo[8], fi[8], bo[8], bi[8];
    for (int i = 0; i < 8; i++) {
        float a = (float)i * (6.2831853f / 8.0f);
        float co = cosf(a), si = sinf(a);
        fo[i] = add_vert(co * (R + tube), si * (R + tube), tube);
        fi[i] = add_vert(co * (R - tube), si * (R - tube), tube);
        bo[i] = add_vert(co * (R + tube), si * (R + tube), -tube);
        bi[i] = add_vert(co * (R - tube), si * (R - tube), -tube);
    }
    for (int i = 0; i < 8; i++) {
        int j = (i + 1) & 7;
        add_face(fo[i], fo[j], fi[j], face_col);   /* front +z */
        add_face(fo[i], fi[j], fi[i], face_col);
        add_face(bo[j], bo[i], bi[i], mtl2);       /* back -z */
        add_face(bo[j], bi[i], bi[j], mtl2);
        add_face(bo[i], bo[j], fo[j], mtl);        /* outer wall */
        add_face(bo[i], fo[j], fo[i], mtl);
        add_face(bi[j], bi[i], fi[i], mtl2);       /* inner wall */
        add_face(bi[j], fi[i], fi[j], mtl2);
    }
    /* Register rim segments as accretion hosts — without these, every
     * module grew off the hub and the wheel looked like a core with a
     * hoop (user report). Eight anchor blocks around the rim. */
    for (int i = 0; i < 8; i++) {
        float a = (float)i * (6.2831853f / 8.0f) + 0.3927f;
        if (s_nmods < MAX_MODULES)
            s_mods[s_nmods++] = (Module){ cosf(a) * R, sinf(a) * R, 0,
                                          tube * 1.2f, tube * 1.2f,
                                          tube };
    }

    /* Hub + spokes. */
    drum(R * 0.32f, tube * 1.4f, mtl, face_col);
    /* Axis-aligned spokes: at 45 deg the |cos|x|sin| extents both went
     * large and each spoke became a square plate over the hole (user
     * report). On-axis they collapse to thin radial arms. */
    for (int k = 0; k < 4; k++) {
        float a = (float)k * 1.5707963f;
        float mx = cosf(a) * R * 0.62f, my = sinf(a) * R * 0.62f;
        box(mx, my, 0, fabsf(cosf(a)) * R * 0.34f + tube * 0.35f,
            fabsf(sinf(a)) * R * 0.34f + tube * 0.35f, tube * 0.30f,
            mtl2, NULL);
    }
}

/* Spindle: stretched octahedral bipyramid + equator band. */
static void spindle(float r, float hz, uint16_t mtl, uint16_t mtl2,
                    uint16_t face_col) {
    if (s_nv + 18 > MAX_SV || s_nf + 32 > MAX_SF) return;
    int eq[8];
    for (int i = 0; i < 8; i++) {
        float a = (float)i * (6.2831853f / 8.0f) + 0.3927f;
        eq[i] = add_vert(cosf(a) * r, sinf(a) * r, 0);
    }
    int tip_f = add_vert(0, 0, hz);
    int tip_b = add_vert(0, 0, -hz);
    for (int i = 0; i < 8; i++) {
        int j = (i + 1) & 7;
        add_face(eq[i], eq[j], tip_f, (i & 1) ? mtl : face_col);
        add_face(eq[j], eq[i], tip_b, (i & 1) ? mtl2 : mtl);
    }
    /* Equator band: thin drum overlapping the waist. */
    drum(r * 1.04f, hz * 0.14f, mtl2, mtl2);
}

#endif  /* old-generator helpers */

static const Mesh *station_gen_style1(uint32_t seed);

const Mesh *station_gen_mesh(uint32_t seed) {
    /* ADOPTED 2026-06-12 ('lock them in, replacing the old way
     * completely'): the archetype generator IS station generation now.
     * The accretion grower below it was deleted with the adoption. */
    return station_gen_style1(seed);
}

#if 0   /* old accretion generator (replaced; kept-out reference) */
const Mesh *station_gen_mesh_old(uint32_t seed) {
    s_rng = seed * 2654435761u;
    s_rng = seed * 2654435761u;
    s_rng ^= s_rng >> 15;
    if (!s_rng) s_rng = 1;
    s_nv = s_nf = s_nmods = 0;

    /* Palette: hull greys tinted per station. */
    int tint = rndi(-10, 22);
    uint16_t HULL = rgb565((uint8_t)(182 + tint), (uint8_t)(184 + tint),
                           (uint8_t)(192 + tint));
    uint16_t HULL2 = rgb565((uint8_t)(140 + tint), (uint8_t)(142 + tint),
                            (uint8_t)(154 + tint));
    uint16_t DARK = RGB565C(70, 76, 92);
    uint16_t GLASS = RGB565C(80, 170, 200);
    uint16_t WIN = RGB565C(34, 60, 96);       /* window bands */
    uint16_t PANEL = RGB565C(60, 120, 195);
    uint16_t ACCENT = (rnd() & 1) ? RGB565C(220, 90, 60)
                                  : RGB565C(230, 175, 60);

    /* 1. Core: six silhouettes (user req: the box core read dull).
     * drum 25 / ring 20 / ball 15 / spindle 15 / box 15 / cross 10. */
    int core_roll = rndi(0, 99);
    float core_r;
    if (core_roll < 25) {
        core_r = rndf(11, 15);
        float core_hz = rndf(8, 16);
        drum(core_r, core_hz, HULL, GLASS);   /* +z cap = docking face */
    } else if (core_roll < 45) {
        /* Ring station: the wheel — slim tube, large radius. */
        core_r = rndf(18, 24);
        ring_core(core_r, rndf(1.8f, 2.8f), HULL, HULL2, GLASS);
    } else if (core_roll < 60) {
        core_r = rndf(12, 16);
        ball(core_r, HULL, WIN);
    } else if (core_roll < 75) {
        core_r = rndf(9, 12);
        spindle(core_r, rndf(18, 26), HULL, HULL2, WIN);
    } else if (core_roll < 90) {
        core_r = rndf(10, 14);
        float chy = rndf(7, 12), chz = rndf(9, 15);
        uint16_t fc[6] = { GLASS, DARK, HULL, HULL, HULL2, HULL2 };
        box(0, 0, 0, core_r, chy, chz, HULL, fc);
    } else {
        /* Cross truss: three interlocked slabs. */
        core_r = rndf(11, 14);
        uint16_t fc[6] = { GLASS, DARK, HULL, HULL, HULL2, HULL2 };
        box(0, 0, 0, core_r, core_r * 0.32f, core_r * 0.32f, HULL, fc);
        box(0, 0, 0, core_r * 0.32f, core_r, core_r * 0.32f, HULL2, NULL);
        box(0, 0, 0, core_r * 0.3f, core_r * 0.3f, core_r * 1.1f, HULL,
            fc);
    }

    /* 2. Accretion: attach modules to existing structure. */
    int steps = rndi(5, 9);
    for (int s = 0; s < steps && s_nmods < MAX_MODULES - 2; s++) {
        const Module *host = &s_mods[rndi(0, s_nmods - 1)];
        int kind = rndi(0, 9);
        if (kind < 4) {
            /* Habitat box on a lateral face (mirrored across x). */
            float hx = rndf(2.5f, 6.0f), hy = rndf(2.5f, 5.0f),
                  hz = rndf(3.0f, 8.0f);
            float oz = rndf(-host->hz * 0.6f, host->hz * 0.6f);
            float oy = rndf(-host->hy * 0.5f, host->hy * 0.5f);
            float cx = host->cx + host->hx + hx * 0.9f;
            /* Window bands on the long faces; occasional accent roof. */
            uint16_t roof = (rnd() & 3) ? HULL : ACCENT;
            uint16_t fc[6] = { HULL2, HULL2, WIN, WIN, roof, HULL2 };
            box(cx, host->cy + oy, host->cz + oz, hx, hy, hz, HULL2, fc);
            box(-cx, host->cy + oy, host->cz + oz, hx, hy, hz, HULL2, fc);
        } else if (kind < 6) {
            /* Solar wing pair: arm + thin blue panel. */
            float ay = rndf(-host->hy * 0.4f, host->hy * 0.4f);
            float az = rndf(-host->hz * 0.5f, host->hz * 0.5f);
            float arm = rndf(4, 7), pw = rndf(8, 14), pl = rndf(7, 12);
            float ax = host->cx + host->hx;
            box(ax + arm * 0.5f, host->cy + ay, host->cz + az,
                arm * 0.5f, 1.0f, 1.0f, HULL2, NULL);
            box(-(ax + arm * 0.5f), host->cy + ay, host->cz + az,
                arm * 0.5f, 1.0f, 1.0f, HULL2, NULL);
            box(ax + arm + pw * 0.5f, host->cy + ay, host->cz + az,
                pw * 0.5f, 0.4f, pl, PANEL, NULL);
            box(-(ax + arm + pw * 0.5f), host->cy + ay, host->cz + az,
                pw * 0.5f, 0.4f, pl, PANEL, NULL);
        } else if (kind < 8) {
            /* Dorsal/ventral spur. */
            float dir = (rnd() & 1) ? 1.0f : -1.0f;
            float hy = rndf(3, 7);
            float cy = host->cy + dir * (host->hy + hy * 0.9f);
            box(host->cx, cy, host->cz + rndf(-host->hz * 0.5f, host->hz * 0.5f),
                rndf(2, 4.5f), hy, rndf(2, 4.5f), HULL2, NULL);
        } else {
            /* Antenna fin (thin, accent tip). */
            float dir = (rnd() & 1) ? 1.0f : -1.0f;
            float hy = rndf(5, 9);
            float cy = host->cy + dir * (host->hy + hy);
            uint16_t fc[6] = { HULL2, HULL2, HULL2, HULL2, ACCENT, ACCENT };
            box(host->cx, cy, host->cz, 0.5f, hy, 0.5f, HULL2, fc);
        }
    }

    /* 3. Docking bay frame on the core +z face. */
    {
        float bz = s_mods[0].hz;
        uint16_t fc[6] = { GLASS, DARK, ACCENT, ACCENT, ACCENT, ACCENT };
        box(0, 0, bz + 0.8f, core_r * 0.45f, core_r * 0.35f, 0.9f, ACCENT, fc);
    }

    /* 4. Quantise to the int8 mesh format. */
    float maxc = 1.0f, bound2 = 1.0f;
    for (int i = 0; i < s_nv; i++) {
        float ax = fabsf(s_fx[i]), ay = fabsf(s_fy[i]), az = fabsf(s_fz[i]);
        if (ax > maxc) maxc = ax;
        if (ay > maxc) maxc = ay;
        if (az > maxc) maxc = az;
        float d2 = s_fx[i] * s_fx[i] + s_fy[i] * s_fy[i] + s_fz[i] * s_fz[i];
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
    s_mesh.nverts = (uint16_t)s_nv;
    s_mesh.nfaces = (uint16_t)s_nf;
    s_mesh.scale = maxc;
    s_mesh.bound_r = sqrtf(bound2);
    s_mesh.lod_lo = 0;
    return &s_mesh;
}
#endif  /* old accretion generator */

/* ====================================================================
 * Station archetypes (ADOPTED): four distinct structures
 * picked by seed, each with seeded proportions and per-seed scale:
 *
 *   RING     — polygonal torus (8/10/12 segments) + hub + 2-4 spokes
 *   SPINDLE  — long axis, stacked drums, solar wings, dishes, antennas
 *   CROSS    — central core, 4-6 radial truss arms ending in modules,
 *              one arm carries a dark radiator fin with glow edges
 *   PLATFORM — open drydock: two parallel slabs, lit hangar gap between
 *
 * Docking convention preserved: every archetype keeps a bright GLASS
 * aperture (with ACCENT collar/hazard frame) on the +z side near the
 * center axis — ST_DOCKING glides the player to (0,0,0).
 * ==================================================================== */

/* --- loft helpers (style-1 only) -------------------------------- */

/* n-gon ring of verts in the XY plane at height z. */
static void s1_ngon(int n, float r, float z, float phase, int *out) {
    for (int i = 0; i < n; i++) {
        float a = (float)i * (6.2831853f / (float)n) + phase;
        out[i] = add_vert(cosf(a) * r, sinf(a) * r, z);
    }
}

/* Side wall lo-ring (smaller z) -> hi-ring, outward winding (matches
 * drum()); facet colors alternate c0/c1 for window bands. */
static void s1_wall(int n, const int *lo, const int *hi,
                    uint16_t c0, uint16_t c1) {
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        uint16_t c = (i & 1) ? c1 : c0;
        add_face(lo[i], lo[j], hi[j], c);
        add_face(lo[i], hi[j], hi[i], c);
    }
}

/* Fan cap over a ring. pos_z: outward = +z, else -z (drum() winding). */
static void s1_capf(int n, const int *ring, int pos_z, uint16_t c) {
    for (int i = 1; i < n - 1; i++) {
        if (pos_z) add_face(ring[0], ring[i], ring[i + 1], c);
        else       add_face(ring[0], ring[i + 1], ring[i], c);
    }
}

/* n-gon drum/frustum along z with optional caps (n <= 16). */
static void s1_drum(int n, float r_lo, float r_hi, float z_lo, float z_hi,
                    float phase, uint16_t side0, uint16_t side1,
                    int cap_lo, uint16_t clo, int cap_hi, uint16_t chi) {
    if (s_nv + 2 * n > MAX_SV || s_nf + 4 * n > MAX_SF) return;
    int lo[16], hi[16];
    s1_ngon(n, r_lo, z_lo, phase, lo);
    s1_ngon(n, r_hi, z_hi, phase, hi);
    s1_wall(n, lo, hi, side0, side1);
    if (cap_lo) s1_capf(n, lo, 0, clo);
    if (cap_hi) s1_capf(n, hi, 1, chi);
}

/* Polygonal torus around z (generalised ring_core, n <= 12). Outer
 * wall facets alternate outer0/outer1, back face alternates back0/back1
 * (window bands around the rim and across the wheel face). */
static void s1_quad(int a, int b, int c, int d, uint16_t col);

/* General torus: centred at z=zc, optionally skipping segment `gap`
 * (a construction hole — the open ends get capped). */
static void s1_torus_at(int n, float R, float tr, float tz, float zc,
                        float phase, int gap,
                        uint16_t outer0, uint16_t outer1,
                        uint16_t inner, uint16_t front,
                        uint16_t back0, uint16_t back1) {
    if (s_nv + 4 * n > MAX_SV || s_nf + 8 * n > MAX_SF) return;
    int fo[12], fi[12], bo[12], bi[12];
    s1_ngon(n, R + tr, zc + tz, phase, fo);
    s1_ngon(n, R - tr, zc + tz, phase, fi);
    s1_ngon(n, R + tr, zc - tz, phase, bo);
    s1_ngon(n, R - tr, zc - tz, phase, bi);
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        if (i == gap) {
            /* cap the two exposed ends of the broken arc */
            s1_quad(fo[i], fi[i], bi[i], bo[i], back0);
            s1_quad(bo[j], bi[j], fi[j], fo[j], back0);
            continue;
        }
        add_face(fo[i], fo[j], fi[j], front);   /* front +z */
        add_face(fo[i], fi[j], fi[i], front);
        uint16_t bc = (i & 1) ? back1 : back0;
        add_face(bo[j], bo[i], bi[i], bc);      /* back -z */
        add_face(bo[j], bi[i], bi[j], bc);
        uint16_t oc = (i & 1) ? outer1 : outer0;
        add_face(bo[i], bo[j], fo[j], oc);      /* outer wall */
        add_face(bo[i], fo[j], fo[i], oc);
        add_face(bi[j], bi[i], fi[i], inner);   /* inner wall */
        add_face(bi[j], fi[i], fi[j], inner);
    }
}
static void s1_torus(int n, float R, float tr, float tz, float phase,
                     uint16_t outer0, uint16_t outer1,
                     uint16_t inner, uint16_t front,
                     uint16_t back0, uint16_t back1) {
    s1_torus_at(n, R, tr, tz, 0, phase, -1, outer0, outer1, inner,
                front, back0, back1);
}

static void s1_quad(int a, int b, int c, int d, uint16_t col) {
    add_face(a, b, c, col);
    add_face(a, c, d, col);
}

/* Strut between two XY points at height z: half-width hw (in-plane),
 * half-depth hz (along z). cside = long in-plane faces, cz = +-z faces,
 * cend = end caps. Winding CCW-from-outside (right-handed u,v,w). */
static void s1_bar(float x0, float y0, float x1, float y1, float z,
                   float hw, float hz,
                   uint16_t cside, uint16_t cz, uint16_t cend) {
    if (s_nv + 8 > MAX_SV || s_nf + 12 > MAX_SF) return;
    float dx = x1 - x0, dy = y1 - y0;
    float l = sqrtf(dx * dx + dy * dy);
    if (l < 1e-6f) return;
    float px = -dy / l * hw, py = dx / l * hw;
    int a0 = add_vert(x0 + px, y0 + py, z - hz);
    int a1 = add_vert(x0 - px, y0 - py, z - hz);
    int a2 = add_vert(x0 - px, y0 - py, z + hz);
    int a3 = add_vert(x0 + px, y0 + py, z + hz);
    int b0 = add_vert(x1 + px, y1 + py, z - hz);
    int b1 = add_vert(x1 - px, y1 - py, z - hz);
    int b2 = add_vert(x1 - px, y1 - py, z + hz);
    int b3 = add_vert(x1 + px, y1 + py, z + hz);
    s1_quad(b3, b2, b1, b0, cend);    /* far end  (+u) */
    s1_quad(a0, a1, a2, a3, cend);    /* near end (-u) */
    s1_quad(a3, a2, b2, b3, cz);      /* +z */
    s1_quad(a1, a0, b0, b1, cz);      /* -z */
    s1_quad(a0, a3, b3, b0, cside);   /* +v */
    s1_quad(a2, a1, b1, b2, cside);   /* -v */
}

/* Octahedral pod (cheap "sphere": 6 verts, 8 faces). */
static void s1_pod(float cx, float cy, float cz, float r, float rz,
                   uint16_t c0, uint16_t c1) {
    if (s_nv + 6 > MAX_SV || s_nf + 8 > MAX_SF) return;
    int vx = add_vert(cx + r, cy, cz), wx = add_vert(cx - r, cy, cz);
    int vy = add_vert(cx, cy + r, cz), wy = add_vert(cx, cy - r, cz);
    int tz = add_vert(cx, cy, cz + rz), bz = add_vert(cx, cy, cz - rz);
    add_face(vx, vy, tz, c0); add_face(vy, wx, tz, c1);
    add_face(wx, wy, tz, c0); add_face(wy, vx, tz, c1);
    add_face(vy, vx, bz, c1); add_face(wx, vy, bz, c0);
    add_face(wy, wx, bz, c1); add_face(vx, wy, bz, c0);
}

/* Comms dish facing +z: hex rim, concave bowl + convex back. */
static void s1_dish(float cx, float cy, float cz, float rd,
                    uint16_t bowl, uint16_t back) {
    if (s_nv + 8 > MAX_SV || s_nf + 12 > MAX_SF) return;
    int ring[6];
    for (int i = 0; i < 6; i++) {
        float a = (float)i * (6.2831853f / 6.0f);
        ring[i] = add_vert(cx + cosf(a) * rd, cy + sinf(a) * rd, cz);
    }
    int af = add_vert(cx, cy, cz - rd * 0.35f);   /* bowl bottom */
    int ab = add_vert(cx, cy, cz - rd * 0.55f);   /* back apex */
    for (int i = 0; i < 6; i++) {
        int j = (i + 1) % 6;
        add_face(ring[i], ring[j], af, bowl);     /* bowl, seen from +z */
        add_face(ring[j], ring[i], ab, back);     /* convex back */
    }
}

/* Blinking-light accent block. */
static void s1_light(float x, float y, float z, float s, uint16_t c) {
    box(x, y, z, s, s, s, c, NULL);
}

/* Faceted ball: three octagonal rings + polar caps (26v / 48f). Lit
 * facets alternate to read as window bands. */
static void s1_ball(float r, uint16_t mtl, uint16_t face_col) {
    if (s_nv + 26 > MAX_SV || s_nf + 48 > MAX_SF) return;
    int rings[3][8];
    float zs[3] = { -r * 0.55f, 0, r * 0.55f };
    float rs[3] = { r * 0.68f, r, r * 0.68f };
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < 8; i++) {
            float a = (float)i * (6.2831853f / 8.0f) + 0.3927f;
            rings[k][i] = add_vert(cosf(a) * rs[k], sinf(a) * rs[k], zs[k]);
        }
    int south = add_vert(0, 0, -r * 0.95f);
    int north = add_vert(0, 0, r * 0.95f);
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < 8; i++) {
            int j = (i + 1) & 7;
            add_face(rings[k][i], rings[k][j], rings[k + 1][j], mtl);
            add_face(rings[k][i], rings[k + 1][j], rings[k + 1][i],
                     (i & 1) ? mtl : face_col);
        }
    for (int i = 0; i < 8; i++) {
        int j = (i + 1) & 7;
        add_face(rings[0][i], south, rings[0][j], mtl);
        add_face(rings[2][i], rings[2][j], north, face_col);
    }
}

/* --- archetypes -------------------------------------------------- */

typedef struct {
    uint16_t HULL, HULL2, DARK, GLASS, WIN, PANEL, ACCENT;
} S1Pal;

/* RING: polygonal torus + hub + spokes; bay collar on the hub +z.
 * Each wheel also rolls TWO of five add-on kits (user: the bare wheel
 * is a great shape but window colours alone can't carry variety):
 * rim blocks / second wheel / hub works / truss spokes / build gap. */
static void s1_arch_ring(float B, const S1Pal *p) {
    int n = 8 + 2 * rndi(0, 2);              /* 8 / 10 / 12 segments */
    float R  = B;
    float tz = R * rndf(0.09f, 0.16f);       /* axial half-thickness  */
    float tr = R * rndf(0.09f, 0.17f);       /* radial half-thickness */
    float ph = rndf(0, 6.2831853f);

    int kit_a = rndi(0, 4);
    int kit_b = (kit_a + 1 + rndi(0, 3)) % 5;
    int has = (1 << kit_a) | (1 << kit_b);

    /* Build gap: one rim segment is bare girders (still being built). */
    int gap_seg = (has & 16) ? rndi(0, n - 1) : -1;
    s1_torus_at(n, R, tr, tz, 0, ph, gap_seg, p->HULL, p->WIN,
                p->HULL2, p->HULL2, p->HULL, p->WIN);
    if (gap_seg >= 0) {
        float a0 = ph + (float)gap_seg * (6.2831853f / (float)n);
        float a1 = ph + (float)(gap_seg + 1) * (6.2831853f / (float)n);
        float gw = R * 0.018f;
        s1_bar(cosf(a0) * R, sinf(a0) * R, cosf(a1) * R, sinf(a1) * R,
               tz * 0.5f, gw, gw, p->DARK, p->DARK, p->DARK);
        s1_bar(cosf(a0) * R, sinf(a0) * R, cosf(a1) * R, sinf(a1) * R,
               -tz * 0.5f, gw, gw, p->DARK, p->DARK, p->DARK);
        float am = (a0 + a1) * 0.5f;
        s1_light(cosf(am) * R, sinf(am) * R, 0, R * 0.025f, p->ACCENT);
    }

    /* Hub drum; docking collar (flared accent throat, glass cap). */
    float hr = R * rndf(0.22f, 0.34f);
    float hl = tz * rndf(1.5f, 2.4f);
    s1_drum(8, hr, hr, -hl, hl, ph, p->HULL, p->HULL,
            1, p->HULL2, 1, p->HULL2);
    s1_drum(8, hr * 0.85f, hr * 0.60f, hl, hl + R * 0.07f, ph,
            p->ACCENT, p->HULL2, 0, 0, 1, p->GLASS);

    /* Spokes, evenly spaced with a seeded phase. */
    int ns = rndi(2, 4);
    float ph2 = ph + rndf(0, 6.2831853f);
    float hw = R * rndf(0.030f, 0.050f);
    for (int k = 0; k < ns; k++) {
        float a = ph2 + (float)k * (6.2831853f / (float)ns);
        if (has & 8) {
            /* truss spokes: twin thin rails, open between */
            float px = -sinf(a) * hw * 1.6f, py = cosf(a) * hw * 1.6f;
            for (int sgn = -1; sgn <= 1; sgn += 2)
                s1_bar(cosf(a) * hr * 0.8f + sgn * px,
                       sinf(a) * hr * 0.8f + sgn * py,
                       cosf(a) * (R - tr * 0.4f) + sgn * px,
                       sinf(a) * (R - tr * 0.4f) + sgn * py, 0,
                       hw * 0.38f, tz * 0.30f,
                       p->HULL2, p->HULL, p->HULL2);
        } else {
            s1_bar(cosf(a) * hr * 0.8f, sinf(a) * hr * 0.8f,
                   cosf(a) * (R - tr * 0.4f), sinf(a) * (R - tr * 0.4f),
                   0, hw, tz * 0.55f, p->HULL2, p->HULL, p->HULL2);
        }
    }

    /* Rim blocks: modules clamped around the wheel — boxes riding the
     * rim front alternating with radial tank pods. Breaks the clean
     * silhouette, which windows alone couldn't. */
    if (has & 1) {
        int nb = rndi(3, 5);
        float ph3 = ph + rndf(0, 6.2831853f);
        for (int k = 0; k < nb; k++) {
            float a = ph3 + (float)k * (6.2831853f / (float)nb);
            if (gap_seg >= 0) {            /* keep clear of the gap */
                float ag = ph + ((float)gap_seg + 0.5f) *
                           (6.2831853f / (float)n);
                float dd = a - ag;
                while (dd > 3.1416f) dd -= 6.2832f;
                while (dd < -3.1416f) dd += 6.2832f;
                if (dd < 0.5f && dd > -0.5f) continue;
            }
            float ca = cosf(a), sa = sinf(a);
            if (k & 1)
                s1_bar(ca * (R - tr * 0.7f), sa * (R - tr * 0.7f),
                       ca * (R + tr * 0.7f), sa * (R + tr * 0.7f),
                       tz * 1.35f, tr * rndf(0.45f, 0.7f), tz * 0.45f,
                       (k & 2) ? p->PANEL : p->HULL2, p->HULL, p->WIN);
            else
                s1_pod(ca * (R + tr * 1.5f), sa * (R + tr * 1.5f), 0,
                       tr * rndf(0.55f, 0.85f), tr * rndf(0.8f, 1.2f),
                       p->HULL, p->HULL2);
        }
    }

    /* Second wheel: a smaller ring stacked behind on the same hub. */
    if (has & 2) {
        float R2 = R * rndf(0.52f, 0.68f);
        float z2 = -(hl + tz * rndf(1.2f, 2.0f));
        s1_torus_at(n, R2, tr * 0.7f, tz * 0.6f, z2, ph + 0.3f, -1,
                    p->HULL2, p->WIN, p->HULL, p->HULL2,
                    p->HULL2, p->WIN);
        s1_drum(8, hr * 0.55f, hr * 0.55f, z2, -hl, ph,
                p->HULL2, p->HULL2, 1, p->HULL2, 0, 0);
        int ns2 = 2 + (rndi(0, 1) ? 1 : 0);
        for (int k = 0; k < ns2; k++) {
            float a = ph + 1.1f + (float)k * (6.2831853f / (float)ns2);
            s1_bar(cosf(a) * hr * 0.5f, sinf(a) * hr * 0.5f,
                   cosf(a) * (R2 - tr * 0.3f),
                   sinf(a) * (R2 - tr * 0.3f),
                   z2, hw * 0.8f, tz * 0.35f, p->HULL2, p->HULL2,
                   p->HULL2);
        }
    }

    /* Hub works: comms dish + a long antenna spur. */
    if (has & 4) {
        float da = ph + rndf(0, 6.2831853f);
        s1_dish(cosf(da) * hr * 0.7f, sinf(da) * hr * 0.7f,
                hl + R * 0.10f, R * 0.13f, p->PANEL, p->HULL2);
        s1_bar(0, 0, -sinf(da) * hr * 1.9f, cosf(da) * hr * 1.9f,
               -hl * 0.5f, R * 0.012f, R * 0.012f,
               p->DARK, p->DARK, p->ACCENT);
    }

    /* Nav lights around the rim front. */
    int nl = rndi(2, 4);
    for (int k = 0; k < nl; k++) {
        float a = ph + 0.6f + (float)k * (6.2831853f / (float)nl);
        s1_light(cosf(a) * R, sinf(a) * R, tz, R * 0.022f, p->ACCENT);
    }
}

/* SPINDLE: long axis, stacked drums, solar wings, dish, antenna. */
static void s1_arch_spindle(float B, const S1Pal *p) {
    float L  = B * rndf(1.05f, 1.30f);       /* half-length */
    float ar = B * rndf(0.055f, 0.085f);     /* axis radius */
    float ph = rndf(0, 6.2831853f);
    /* Shaft (ends buried in the aft pod / dock collar). */
    s1_drum(6, ar, ar, -L * 0.85f, L * 0.80f, ph, p->HULL2, p->HULL2,
            0, 0, 0, 0);
    /* Aft engine pod: tapers down to a small glowing nozzle. */
    s1_drum(6, ar * 1.1f, ar * 2.4f, -L, -L * 0.86f, ph,
            p->HULL2, p->HULL, 1, p->ACCENT, 1, p->HULL2);

    /* Habitat / industry drum stack — one ring is always lit. */
    int nd = rndi(2, 3);
    int lit = rndi(0, nd - 1);
    float zc0 = -0.62f, zstep = 1.18f / (float)(nd - 1 ? nd - 1 : 1);
    float big = 0;
    float bigz = 0;
    for (int k = 0; k < nd; k++) {
        float zc = (zc0 + zstep * (float)k + rndf(-0.04f, 0.04f)) * L;
        float dr = B * rndf(0.18f, 0.32f);
        float dh = B * rndf(0.05f, 0.10f);
        if (dr > big) { big = dr; bigz = zc; }
        s1_drum(8, dr, dr, zc - dh, zc + dh, ph,
                p->HULL, (k == lit) ? p->WIN : p->HULL,
                1, p->HULL2, 1, p->HULL2);
        s1_light(0, dr, zc, B * 0.018f, p->ACCENT);
    }

    /* Docking collar at the +z tip: throat + accent ring + glass. */
    float cr = B * rndf(0.13f, 0.18f);
    s1_drum(8, cr * 0.8f, cr, L * 0.80f, L * 0.92f, ph,
            p->HULL, p->HULL, 1, p->HULL2, 0, 0);
    s1_drum(8, cr, cr * 0.62f, L * 0.92f, L, ph,
            p->ACCENT, p->ACCENT, 0, 0, 1, p->GLASS);

    /* Solar wings off the middle: arm + thin blue panel, mirrored. */
    int np = rndi(1, 2);
    for (int k = 0; k < np; k++) {
        float zc = L * rndf(-0.40f, 0.30f);
        float arm = B * rndf(0.14f, 0.22f);
        float pw  = B * rndf(0.34f, 0.50f);
        float pl  = B * rndf(0.20f, 0.32f);
        /* Panels face +-z (sun-tracking) so the big PANEL faces read. */
        uint16_t pfc[6] = { p->PANEL, p->PANEL, p->DARK, p->DARK,
                            p->DARK, p->DARK };
        for (int s = -1; s <= 1; s += 2) {
            box(s * arm * 0.5f, 0, zc, arm * 0.5f, B * 0.035f,
                B * 0.035f, p->HULL2, NULL);
            box(s * (arm + pw * 0.5f), 0, zc, pw * 0.5f, pl,
                B * 0.014f, p->PANEL, pfc);
        }
    }

    /* Comms dish riding the biggest drum; antenna spur off the aft. */
    s1_dish(0, big + B * 0.10f, bigz + B * 0.06f, B * 0.11f,
            p->HULL, p->HULL2);
    float sl = B * rndf(0.12f, 0.22f);
    uint16_t afc[6] = { p->HULL2, p->ACCENT, p->HULL2, p->HULL2,
                        p->HULL2, p->HULL2 };
    box(0, 0, -L - sl, B * 0.014f, B * 0.014f, sl, p->HULL2, afc);
}

/* CROSS: core + 4-6 radial truss arms with end modules + radiator. */
static void s1_arch_cross(float B, const S1Pal *p) {
    float cr = B * rndf(0.20f, 0.26f);
    float ch = B * rndf(0.16f, 0.24f);
    float ph = rndf(0, 6.2831853f);
    s1_drum(8, cr, cr, -ch, ch, ph, p->HULL, p->WIN,
            1, p->HULL, 1, p->HULL2);
    /* Docking collar on the core +z face. */
    s1_drum(8, cr * 0.66f, cr * 0.48f, ch, ch + B * 0.09f, ph,
            p->ACCENT, p->HULL2, 0, 0, 1, p->GLASS);

    int na = rndi(4, 6);
    float ph2 = rndf(0, 6.2831853f);
    int fin_arm = rndi(0, na - 1);
    for (int k = 0; k < na; k++) {
        float a = ph2 + (float)k * (6.2831853f / (float)na);
        float len = B * rndf(0.60f, 1.00f);   /* arms differ in length */
        float ca = cosf(a), sa = sinf(a);
        s1_bar(ca * cr * 0.85f, sa * cr * 0.85f, ca * len, sa * len, 0,
               B * 0.035f, B * 0.035f, p->HULL2, p->HULL, p->HULL2);
        if (k == fin_arm) {
            /* Radiator fin: dark flat panel with glowing edges. */
            s1_bar(ca * len * 0.40f, sa * len * 0.40f,
                   ca * len * 0.96f, sa * len * 0.96f, 0,
                   B * 0.012f, B * 0.17f, p->DARK, p->ACCENT, p->ACCENT);
            continue;
        }
        int kind = rndi(0, 2);
        if (kind == 0) {
            /* Habitat can aligned with the arm, lit z-faces. */
            s1_bar(ca * (len - B * 0.02f), sa * (len - B * 0.02f),
                   ca * len + ca * B * 0.20f, sa * len + sa * B * 0.20f,
                   0, B * 0.085f, B * 0.085f, p->HULL, p->WIN, p->HULL2);
            s1_light(ca * (len + B * 0.20f), sa * (len + B * 0.20f), 0,
                     B * 0.018f, p->ACCENT);
        } else if (kind == 1) {
            s1_pod(ca * (len + B * 0.08f), sa * (len + B * 0.08f), 0,
                   B * 0.11f, B * 0.14f, p->HULL, p->WIN);
        } else {
            s1_dish(ca * (len + B * 0.02f), sa * (len + B * 0.02f),
                    B * 0.05f, B * 0.10f, p->HULL, p->HULL2);
        }
    }

    /* Keel spur off the core -z face. */
    float sl = B * rndf(0.14f, 0.24f);
    uint16_t afc[6] = { p->HULL2, p->ACCENT, p->HULL2, p->HULL2,
                        p->HULL2, p->HULL2 };
    box(0, 0, -ch - sl, B * 0.016f, B * 0.016f, sl, p->HULL2, afc);
}

/* PLATFORM: open drydock — two slabs, lit hangar gap, gantry tower. */
static void s1_arch_platform(float B, const S1Pal *p) {
    /* Three frame layouts (user: 'the box stations look too similar'):
     * 0 classic two-slab drydock; 1 SIDE-WALL dock — one slab rotates
     * into a vertical wall, L-section; 2 DOUBLE-DECK — three slabs,
     * two stacked bays. Plus a crane gantry, side tank farm and per-
     * layout silhouettes below. */
    int lay = rndi(0, 2);
    float W  = B * rndf(0.62f, 0.85f);    /* half-width  x */
    float D  = B * rndf(0.50f, 0.70f);    /* half-depth  z */
    float G  = B * rndf(0.30f, 0.40f);    /* half-gap    y */
    float sh = B * rndf(0.05f, 0.08f);    /* slab half-thickness */
    /* Slabs: faces toward the gap are lit window decks. */
    uint16_t fcT[6] = { p->HULL2, p->HULL, p->HULL2, p->HULL2,
                        p->HULL, p->WIN };
    uint16_t fcB[6] = { p->HULL2, p->HULL, p->HULL2, p->HULL2,
                        p->WIN, p->HULL2 };
    box(0,  G + sh, 0, W, sh, D, p->HULL, fcT);
    if (lay == 1) {
        /* L-section: the lower slab becomes a vertical side wall with
         * a window band facing the bay */
        uint16_t fcW[6] = { p->HULL2, p->HULL2, p->WIN, p->HULL2,
                            p->HULL, p->HULL2 };
        box(-W - sh, 0, 0, sh, G + 2 * sh, D, p->HULL, fcW);
        /* short lower lip so ships still read the floor line */
        box(0, -G - sh, 0, W * 0.45f, sh, D * 0.8f, p->HULL2, fcB);
    } else {
        box(0, -G - sh, 0, W, sh, D, p->HULL, fcB);
    }
    if (lay == 2) {
        /* double-deck: a third slab splits the gap into two bays */
        uint16_t fcM[6] = { p->HULL2, p->HULL, p->HULL2, p->HULL2,
                            p->WIN, p->WIN };
        box(0, 0, 0, W * 0.95f, sh * 0.8f, D * 0.92f, p->HULL, fcM);
    }
    /* Recessed control wall on-axis: lit glass face toward the bay
     * mouth; narrow, so the frame stays see-through at the sides. */
    uint16_t fcK[6] = { p->GLASS, p->HULL2, p->HULL2, p->HULL2,
                        p->HULL2, p->HULL2 };
    box(0, 0, -D + B * 0.04f, W * 0.36f, G, B * 0.04f, p->HULL2, fcK);
    /* Four corner pylons + hazard strips along both bay mouths. */
    float px = W - B * 0.05f, pz = D - B * 0.05f;
    box( px, 0,  pz, B * 0.045f, G, B * 0.045f, p->HULL2, NULL);
    box(-px, 0,  pz, B * 0.045f, G, B * 0.045f, p->HULL2, NULL);
    box( px, 0, -pz, B * 0.045f, G, B * 0.045f, p->HULL2, NULL);
    box(-px, 0, -pz, B * 0.045f, G, B * 0.045f, p->HULL2, NULL);
    for (int s = -1; s <= 1; s += 2) {
        box(0, s * (G + sh * 0.4f),  D, W * 0.9f, B * 0.02f,
            B * 0.02f, p->ACCENT, NULL);
        box(0, s * (G + sh * 0.4f), -D, W * 0.9f, B * 0.02f,
            B * 0.02f, p->ACCENT, NULL);
    }

    /* Control tower on the top slab + antenna. */
    float tx = rndf(-0.5f, 0.5f) * W;
    float th = B * rndf(0.12f, 0.18f);
    float ty = G + 2 * sh + th;
    uint16_t fcC[6] = { p->WIN, p->WIN, p->HULL2, p->HULL2,
                        p->HULL2, p->HULL2 };
    box(tx, ty, -D * 0.3f, B * 0.09f, th, B * 0.09f, p->HULL2, fcC);
    uint16_t fcA[6] = { p->HULL2, p->HULL2, p->HULL2, p->HULL2,
                        p->ACCENT, p->HULL2 };
    box(tx, ty + th + B * 0.08f, -D * 0.3f, B * 0.012f, B * 0.08f,
        B * 0.012f, p->HULL2, fcA);

    /* Crane gantry: a bridge over the deck with a hanging arm (60%). */
    if (rndi(0, 9) < 6) {
        float gx = rndf(-0.4f, 0.4f) * W;
        float gy = G + 2 * sh;
        box(gx - W * 0.35f, gy + B * 0.10f, D * 0.2f, B * 0.022f,
            B * 0.10f, B * 0.022f, p->DARK, NULL);
        box(gx + W * 0.35f, gy + B * 0.10f, D * 0.2f, B * 0.022f,
            B * 0.10f, B * 0.022f, p->DARK, NULL);
        uint16_t fcG[6] = { p->HULL2, p->HULL2, p->HULL2, p->HULL2,
                            p->ACCENT, p->HULL2 };
        box(gx, gy + B * 0.20f, D * 0.2f, W * 0.38f, B * 0.022f,
            B * 0.03f, p->HULL2, fcG);
        box(gx + rndf(-0.25f, 0.25f) * W, gy + B * 0.13f, D * 0.2f,
            B * 0.015f, B * 0.05f, B * 0.015f, p->DARK, NULL);
    }
    /* Side tank farm: a rank of drums off one edge (50%). */
    if (rndi(0, 1)) {
        float side = (rnd() & 1) ? 1.0f : -1.0f;
        int nt = rndi(2, 3);
        for (int k = 0; k < nt; k++)
            s1_pod(side * (W + B * 0.13f),
                   -G * 0.3f + (float)k * B * 0.16f,
                   -D * 0.4f + (float)k * B * 0.10f,
                   B * 0.075f, B * 0.12f, p->HULL, p->HULL2);
    }

    /* Cargo stacks on deck, fuel pods slung beneath. */
    int nc = rndi(2, 4);
    for (int k = 0; k < nc; k++) {
        float cx = rndf(-0.7f, 0.7f) * W, cz = rndf(-0.6f, 0.7f) * D;
        if (fabsf(cx - tx) < B * 0.2f) cx = -cx;
        uint16_t cc = (k & 1) ? p->PANEL : p->ACCENT;
        box(cx, G + 2 * sh + B * 0.045f, cz, B * 0.06f, B * 0.045f,
            B * 0.05f, (rnd() & 1) ? p->HULL2 : cc, NULL);
    }
    s1_pod( W * 0.45f, -G - 2 * sh - B * 0.09f, 0, B * 0.09f,
            B * 0.16f, p->HULL2, p->DARK);
    s1_pod(-W * 0.45f, -G - 2 * sh - B * 0.09f, 0, B * 0.09f,
            B * 0.16f, p->HULL2, p->DARK);
    /* Approach lights at the pylon tips. */
    s1_light( px, G * 0.9f, pz + B * 0.05f, B * 0.022f, p->ACCENT);
    s1_light(-px, G * 0.9f, pz + B * 0.05f, B * 0.022f, p->ACCENT);
}

/* === new 4th-family concepts (2026-06-13) ========================= *
 * The cuboid drydock (s1_arch_platform) was rejected; these four are
 * the candidate replacements.  All keep the +z docking aperture on the
 * spin axis so ST_DOCKING (glide to origin) reads the same as the other
 * archetypes.  g_force_station_fam (lab) pins one for the contact sheet.
 * ----------------------------------------------------------------- */

#ifdef ELITE_STYLE_LAB
int g_force_station_fam = -1;   /* -1 random; 0..3 pin a concept */
#endif

/* Square-section tube ring around an arbitrary axis `a` (unit), centred
 * at C, mean radius R, tube half-size t, n segments.  Used for the
 * crossed-ring gyrostation (rings that don't lie in the z=0 plane). */
static void s1_tube_ring(float cx, float cy, float cz,
                         float ax, float ay, float az,
                         float R, float t, int n,
                         uint16_t c0, uint16_t c1) {
    if (n > 10) n = 10;
    if (s_nv + 4 * n > MAX_SV || s_nf + 8 * n > MAX_SF) return;
    /* normalise axis, build an in-plane orthonormal basis e1,e2 */
    float al = sqrtf(ax*ax + ay*ay + az*az);
    if (al < 1e-6f) return;
    ax /= al; ay /= al; az /= al;
    float rx = (fabsf(ax) < 0.9f) ? 1.0f : 0.0f;
    float ry = (fabsf(ax) < 0.9f) ? 0.0f : 1.0f;
    float e1x = ay*0.0f - az*ry, e1y = az*rx - ax*0.0f, e1z = ax*ry - ay*rx;
    float e1l = sqrtf(e1x*e1x + e1y*e1y + e1z*e1z);
    e1x /= e1l; e1y /= e1l; e1z /= e1l;
    float e2x = ay*e1z - az*e1y;
    float e2y = az*e1x - ax*e1z;
    float e2z = ax*e1y - ay*e1x;
    int ring[10][4];
    for (int i = 0; i < n; i++) {
        float th = (float)i * (6.2831853f / (float)n);
        float ct = cosf(th), st = sinf(th);
        /* radial unit dir in the ring plane */
        float dx = ct*e1x + st*e2x;
        float dy = ct*e1y + st*e2y;
        float dz = ct*e1z + st*e2z;
        float px = cx + R*dx, py = cy + R*dy, pz = cz + R*dz;
        /* square cross-section spanned by axis a and radial d */
        ring[i][0] = add_vert(px - t*ax - t*dx, py - t*ay - t*dy, pz - t*az - t*dz);
        ring[i][1] = add_vert(px + t*ax - t*dx, py + t*ay - t*dy, pz + t*az - t*dz);
        ring[i][2] = add_vert(px + t*ax + t*dx, py + t*ay + t*dy, pz + t*az + t*dz);
        ring[i][3] = add_vert(px - t*ax + t*dx, py - t*ay + t*dy, pz - t*az + t*dz);
    }
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        uint16_t c = (i & 1) ? c1 : c0;
        s1_quad(ring[i][3], ring[i][2], ring[j][2], ring[j][3], c);   /* outer */
        s1_quad(ring[i][1], ring[i][0], ring[j][0], ring[j][1], c0);  /* inner */
        s1_quad(ring[i][2], ring[i][1], ring[j][1], ring[j][2], c);   /* +a    */
        s1_quad(ring[i][0], ring[i][3], ring[j][3], ring[j][0], c0);  /* -a    */
    }
}

/* (CORIOLIS faceted-sphere builder removed -- dropped from the rotation
 * and reclaimed for flash headroom.) */

/* STACK: a slim spine carrying a tapering tower of habitat discs, an
 * aft engine bell, and the docking collar at the +z crown. */
static void s1_st_stack(float B, const S1Pal *p) {
    float L  = B * rndf(1.15f, 1.40f);       /* half-length */
    float ar = B * rndf(0.07f, 0.10f);       /* visible spine column */
    float ph = rndf(0, 6.2831853f);
    int n = 10 + 2 * rndi(0, 1);             /* rounder plates */
    /* Central spine column — left exposed between the plates so the
     * "coins on a rod" silhouette reads from any angle. */
    s1_drum(8, ar, ar, -L * 0.80f, L * 0.84f, ph, p->HULL2, p->HULL,
            0, 0, 0, 0);
    /* Aft engine bell. */
    s1_drum(6, ar * 1.1f, ar * 2.2f, -L, -L * 0.80f, ph,
            p->HULL2, p->HULL, 1, p->ACCENT, 1, p->HULL2);
    /* Plate stack: thin discs with WIDE gaps and a modest taper. Each
     * plate carries a darker rim band + an edge light so the disc edge
     * reads as a disc rather than merging into a cone. */
    int nd = rndi(4, 5);
    int lit = rndi(0, nd - 1);
    float z0 = -L * 0.62f, z1 = L * 0.74f;
    float top = B * rndf(0.22f, 0.30f);      /* crown plate radius */
    float bot = B * rndf(0.40f, 0.52f);      /* base  plate radius */
    float dh  = B * 0.035f;                  /* thin plates */
    for (int k = 0; k < nd; k++) {
        float f  = (float)k / (float)(nd - 1);
        float zc = z0 + (z1 - z0) * f;
        float dr = bot + (top - bot) * f;
        /* plate body: lit window band on the side wall */
        s1_drum(n, dr, dr, zc - dh, zc + dh, ph,
                p->HULL, (k == lit) ? p->WIN : p->HULL2,
                1, p->HULL2, 1, p->HULL);
        /* thin dark rim lip proud of the plate edge */
        s1_drum(n, dr * 1.04f, dr * 1.04f, zc - dh * 0.5f, zc + dh * 0.5f,
                ph, p->DARK, p->DARK, 0, 0, 0, 0);
        s1_light(dr * 1.04f, 0, zc, B * 0.02f, p->ACCENT);
    }
    /* Docking collar at the crown. */
    float cr = B * rndf(0.13f, 0.17f);
    s1_drum(8, cr, cr, L * 0.74f, L * 0.84f, ph, p->HULL, p->HULL,
            0, 0, 0, 0);
    s1_drum(8, cr, cr * 0.6f, L * 0.84f, L, ph,
            p->ACCENT, p->ACCENT, 0, 0, 1, p->GLASS);
    /* Solar fins off the base plate, mirrored. */
    for (int s = -1; s <= 1; s += 2) {
        uint16_t pfc[6] = { p->PANEL, p->PANEL, p->DARK, p->DARK,
                            p->DARK, p->DARK };
        box(s * (bot + B * 0.30f), 0, z0, B * 0.26f, B * 0.36f,
            B * 0.012f, p->PANEL, pfc);
        box(s * (bot + B * 0.04f), 0, z0, B * 0.05f, B * 0.025f,
            B * 0.025f, p->HULL2, NULL);
    }
}

/* ROCK: an asteroid-anchored port — a lumpy dark rock with bolted-on
 * habitat modules, a carved docking bay on +z, tanks and an antenna. */
static void s1_st_rock(float B, const S1Pal *p) {
    float R = B * rndf(0.70f, 0.90f);
    int tn = rndi(-4, 12);
    uint16_t ROCK  = rgb565((uint8_t)(118 + tn), (uint8_t)(102 + tn),
                            (uint8_t)(86 + tn));
    uint16_t ROCK2 = rgb565((uint8_t)(80 + tn), (uint8_t)(68 + tn),
                            (uint8_t)(56 + tn));
    /* Main body + a couple of overlapping lumps for an irregular mass. */
    s1_ball(R, ROCK, ROCK2);
    for (int k = 0; k < 2; k++) {
        float a = rndf(0, 6.2831853f), e = rndf(0, 6.2831853f);
        float lr = R * rndf(0.45f, 0.70f);
        s1_pod(cosf(a) * R * 0.7f, sinf(a) * R * 0.6f,
               cosf(e) * R * 0.5f, lr, lr * 0.85f, ROCK, ROCK2);
    }
    /* Carved docking bay on +z: bright lit recess + glass control face,
     * framed by an accent collar so it reads as a working port mouth. */
    float bw = R * rndf(0.36f, 0.48f);
    uint16_t fcBay[6] = { p->GLASS, p->DARK, p->WIN, p->WIN,
                          p->WIN, p->WIN };
    box(0, 0, R * 0.70f, bw, bw * 0.72f, R * 0.20f, p->WIN, fcBay);
    s1_drum(8, bw * 1.18f, bw * 1.06f, R * 0.80f, R * 0.96f,
            rndf(0, 6.2831853f), p->ACCENT, p->HULL2, 0, 0, 0, 0);
    s1_light( bw, 0, R * 0.92f, B * 0.024f, p->ACCENT);
    s1_light(-bw, 0, R * 0.92f, B * 0.024f, p->ACCENT);
    /* Bolted-on habitat modules clinging to the surface. */
    int nm = rndi(3, 5);
    for (int k = 0; k < nm; k++) {
        float a = rndf(0, 6.2831853f), el = rndf(-0.6f, 0.9f);
        float rr = R * sqrtf(1.0f - el * el * 0.6f);
        float mx = cosf(a) * rr, my = sinf(a) * rr, mz = el * R * 0.8f;
        uint16_t mc = (k & 1) ? p->HULL : p->HULL2;
        uint16_t fcM[6] = { p->WIN, mc, mc, mc, mc, mc };
        box(mx, my, mz, B * 0.10f, B * 0.07f, B * 0.07f, mc, fcM);
        if (k == 0) s1_light(mx, my + B * 0.10f, mz, B * 0.018f, p->ACCENT);
    }
    /* Fuel tanks slung off one flank + a tall antenna mast. */
    float side = (rnd() & 1) ? 1.0f : -1.0f;
    for (int k = 0; k < 2; k++)
        s1_pod(side * (R + B * 0.10f), -R * 0.2f + k * B * 0.20f, -R * 0.3f,
               B * 0.09f, B * 0.13f, p->HULL2, p->DARK);
    float sl = R * rndf(0.30f, 0.46f);
    uint16_t afc[6] = { p->HULL2, p->ACCENT, p->HULL2, p->HULL2,
                        p->HULL2, p->HULL2 };
    box(0, R * 0.6f + sl * 0.5f, R * 0.2f, R * 0.018f, sl, R * 0.018f,
        p->HULL2, afc);
}

/* SPINE: an open gyroscopic frame — two crossed tube rings on a central
 * hub, node pods around the rims, and the dock collar on +z. */
static void s1_st_spin(float B, const S1Pal *p) {
    float R = B * rndf(0.78f, 0.96f);
    float t = B * rndf(0.035f, 0.055f);
    int n = 8;
    float ph = rndf(0, 6.2831853f);
    /* Hub: small faceted drum on the z axis. */
    float hr = B * rndf(0.16f, 0.22f);
    s1_drum(8, hr, hr, -hr * 0.8f, hr * 0.8f, ph, p->HULL, p->WIN,
            1, p->HULL2, 0, 0);
    /* Docking collar on the hub +z. */
    s1_drum(8, hr * 0.72f, hr * 0.5f, hr * 0.8f, hr * 0.8f + B * 0.09f, ph,
            p->ACCENT, p->ACCENT, 0, 0, 1, p->GLASS);
    /* Ring A: around z (lies in the z=0 plane). */
    s1_tube_ring(0, 0, 0, 0, 0, 1, R, t, n, p->HULL, p->WIN);
    /* Ring B: around x, tilted (crosses ring A like a gyroscope). */
    float tlt = rndf(0.5f, 0.9f);
    s1_tube_ring(0, 0, 0, 1, sinf(tlt) * 0.4f, 0, R * 0.92f, t, n,
                 p->HULL2, p->ACCENT);
    /* Spokes from hub to ring A at a few nodes + node pods. */
    int ns = 4;
    for (int k = 0; k < ns; k++) {
        float a = ph + (float)k * (6.2831853f / (float)ns);
        float ca = cosf(a), sa = sinf(a);
        s1_bar(ca * hr, sa * hr, ca * (R - t), sa * (R - t), 0,
               B * 0.022f, B * 0.022f, p->HULL2, p->HULL, p->HULL2);
        s1_pod(ca * R, sa * R, 0, B * 0.07f, B * 0.07f,
               (k & 1) ? p->HULL : p->ACCENT, p->HULL2);
    }
    /* A comms dish riding one node. */
    s1_dish(0, R, 0, B * 0.13f, p->HULL, p->HULL2);
}

static void s1_arch_exotic(float B, const S1Pal *p) {
    /* Kept families: STACK / ROCK / SPINE (CORIOLIS dropped). */
    int fam = rndi(0, 2);        /* 0=stack 1=rock 2=spin */
#ifdef ELITE_STYLE_LAB
    if (g_force_station_fam >= 0) fam = g_force_station_fam % 3;
#endif
    switch (fam) {
        case 0:  s1_st_stack(B, p); break;
        case 1:  s1_st_rock(B, p);  break;
        default: s1_st_spin(B, p);  break;
    }
}

/* --- style-1 entry ------------------------------------------------ */

static const Mesh *station_gen_style1(uint32_t seed) {
    s_rng = seed * 2654435761u;
    s_rng ^= s_rng >> 15;
    if (!s_rng) s_rng = 1;
    s_nv = s_nf = s_nmods = 0;

    S1Pal pal;
    int tint = rndi(-12, 20);
    pal.HULL  = rgb565((uint8_t)(180 + tint), (uint8_t)(182 + tint),
                       (uint8_t)(190 + tint));
    pal.HULL2 = rgb565((uint8_t)(132 + tint), (uint8_t)(136 + tint),
                       (uint8_t)(150 + tint));
    pal.DARK  = RGB565C(58, 64, 80);
    pal.GLASS = RGB565C(95, 185, 215);
    pal.WIN   = RGB565C(70, 130, 175);
    pal.PANEL = RGB565C(55, 110, 185);
    pal.ACCENT = (rnd() & 1) ? RGB565C(235, 100, 50)
                             : RGB565C(240, 185, 60);

    /* Per-seed world scale: some stations are 1.5x+ the size of
     * others (mesh->scale carries it; sheets normalise by bound_r). */
    float B = rndf(24, 34) * rndf(1.0f, 1.6f);

    int arch = rndi(0, 99);
#ifdef ELITE_STYLE_LAB
    if (g_force_station_fam >= 0) arch = 99;   /* pin the exotic branch */
#endif
    if (arch < 30)      s1_arch_ring(B, &pal);
    else if (arch < 55) s1_arch_spindle(B, &pal);
    else if (arch < 80) s1_arch_cross(B, &pal);
    else                s1_arch_exotic(B, &pal);   /* cuboid platform retired */

    /* Quantise to the int8 mesh format (same rules as style 0). */
    float maxc = 1.0f, bound2 = 1.0f;
    for (int i = 0; i < s_nv; i++) {
        float ax = fabsf(s_fx[i]), ay = fabsf(s_fy[i]), az = fabsf(s_fz[i]);
        if (ax > maxc) maxc = ax;
        if (ay > maxc) maxc = ay;
        if (az > maxc) maxc = az;
        float d2 = s_fx[i] * s_fx[i] + s_fy[i] * s_fy[i] + s_fz[i] * s_fz[i];
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
    s_mesh.nverts = (uint16_t)s_nv;
    s_mesh.nfaces = (uint16_t)s_nf;
    s_mesh.scale = maxc;
    s_mesh.bound_r = sqrtf(bound2);
    s_mesh.lod_lo = 0;
    return &s_mesh;
}
