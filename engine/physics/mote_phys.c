/*
 * Mote — rigid-sphere physics solver.
 *
 * Impulse responses ported from ThumbyCue's collide_ball_ball / collide_surface
 * (the proven Coulomb-friction + rotational-inertia formulation), generalised
 * to arbitrary 3D normals and per-body inverse mass. For two equal spheres the
 * tangential effective inverse mass 3.5*(imi+imj) reduces to ThumbyCue's 7/m.
 */
#include "mote_phys.h"
#include "mote_config.h"
#include <math.h>

#define DEFAULT_H   (1.0f / 240.0f)   /* default substep; per-world overridable.
                                       * Few fast bodies (pool) raise the rate for
                                       * accuracy; many slow bodies lower it since
                                       * cost = substeps*iters*collisions. */
#define REST_SLOP   0.45f             /* below this approach speed, no bounce
                                       * (kills gravity micro-bouncing -> bodies
                                       * actually come to rest) */
#define SOLVER_ITERS 4                /* contact relaxation passes per substep —
                                       * lets penetration resolve and stacks
                                       * settle instead of sinking into each
                                       * other */
#define MAX_SUBSTEPS 8                /* default substep cap (per-world overridable)
                                       * + drop the backlog so heavy load can't
                                       * trigger the fixed-step spiral of death */
#define GRID_MAX_BODIES 256           /* broad-phase grid covers up to this many;
                                       * above it, fall back to all-pairs */
#define GRID_CELLS 4096               /* uniform-grid cell budget (16 KB) */

void mote_phys_world_defaults(MoteWorld *w) {
    w->gravity = v3(0.0f, -9.8f, 0.0f);
    w->bmin = v3(-2.0f, -2.0f, -2.0f);
    w->bmax = v3( 2.0f,  2.0f,  2.0f);
    w->restitution = 0.6f;
    w->friction    = 0.3f;
    w->linear_damp  = 0.05f;
    w->angular_damp = 0.4f;
    w->substep = DEFAULT_H;
    w->max_substeps = MAX_SUBSTEPS;
    w->_acc = 0.0f;
}

/* Sphere inverse rotational inertia: I = 0.4 m r^2 -> 1/I = 2.5*inv_mass/r^2. */
static inline float inv_I(const MoteBody *b) {
    return (b->radius > 1e-6f) ? (2.5f * b->inv_mass) / (b->radius * b->radius) : 0.0f;
}

MOTE_HOT static int collide_pair(const MoteWorld *w, MoteBody *bi, MoteBody *bj) {
    Vec3 d = v3_sub(bj->pos, bi->pos);
    float dist = v3_len(d);
    float mind = bi->radius + bj->radius;
    if (dist >= mind || dist < 1e-6f) return 0;

    float imi = bi->inv_mass, imj = bj->inv_mass, ims = imi + imj;
    if (ims <= 0.0f) return 0;

    Vec3 n = v3_scale(d, 1.0f / dist);              /* i -> j */
    float overlap = mind - dist;
    bi->pos = v3_sub(bi->pos, v3_scale(n, overlap * (imi / ims)));
    bj->pos = v3_add(bj->pos, v3_scale(n, overlap * (imj / ims)));

    Vec3 dv = v3_sub(bj->vel, bi->vel);
    float vn = v3_dot(dv, n);
    if (vn >= 0.0f) return 1;                       /* separating; overlap fixed */

    float e = (-vn < REST_SLOP) ? 0.0f : w->restitution;
    float Jn = -(1.0f + e) * vn / ims;
    Vec3 Jn_v = v3_scale(n, Jn);
    bi->vel = v3_sub(bi->vel, v3_scale(Jn_v, imi));
    bj->vel = v3_add(bj->vel, v3_scale(Jn_v, imj));

    /* Tangential friction -> spin transfer. */
    Vec3 ri = v3_scale(n,  bi->radius);
    Vec3 rj = v3_scale(n, -bj->radius);
    Vec3 si = v3_add(bi->vel, v3_cross(bi->w, ri));
    Vec3 sj = v3_add(bj->vel, v3_cross(bj->w, rj));
    Vec3 s  = v3_sub(sj, si);
    Vec3 st = v3_sub(s, v3_scale(n, v3_dot(s, n)));
    float stl = v3_len(st);
    if (stl > 1e-5f) {
        Vec3 that = v3_scale(st, 1.0f / stl);
        float tinv = 3.5f * imi + 3.5f * imj;
        float Jt_stop = stl / tinv;
        float Jt_max = w->friction * fabsf(Jn);
        float Jt = (Jt_stop < Jt_max) ? Jt_stop : Jt_max;
        Vec3 Jt_v = v3_scale(that, Jt);
        bj->vel = v3_add(bj->vel, v3_scale(Jt_v, imj));
        bi->vel = v3_sub(bi->vel, v3_scale(Jt_v, imi));
        bj->w = v3_add(bj->w, v3_scale(v3_cross(rj, Jt_v), inv_I(bj)));
        bi->w = v3_sub(bi->w, v3_scale(v3_cross(ri, Jt_v), inv_I(bi)));
    }
    return 1;
}

/* Sphere vs an immovable plane with inward unit normal N. */
static int collide_wall(const MoteWorld *w, MoteBody *b, Vec3 N) {
    float im = b->inv_mass;
    if (im <= 0.0f) return 0;
    Vec3 r = v3_scale(N, -b->radius);
    Vec3 vc = v3_add(b->vel, v3_cross(b->w, r));
    float vn = v3_dot(vc, N);
    if (vn >= 0.0f) return 0;

    float e = (-vn < REST_SLOP) ? 0.0f : w->restitution;
    float Jn = -(1.0f + e) * vn / im;
    b->vel = v3_add(b->vel, v3_scale(N, Jn * im));

    if (-vn < 0.02f) return 1;                      /* resting: skip friction churn */

    vc = v3_add(b->vel, v3_cross(b->w, r));
    Vec3 vt = v3_sub(vc, v3_scale(N, v3_dot(vc, N)));
    float vtl = v3_len(vt);
    if (vtl > 1e-5f) {
        Vec3 that = v3_scale(vt, -1.0f / vtl);
        float tinv = 3.5f * im;
        float Jt_stop = vtl / tinv;
        float Jt_max = w->friction * fabsf(Jn);
        float Jt = (Jt_stop < Jt_max) ? Jt_stop : Jt_max;
        Vec3 Jt_v = v3_scale(that, Jt);
        b->vel = v3_add(b->vel, v3_scale(Jt_v, im));
        b->w   = v3_add(b->w, v3_scale(v3_cross(r, Jt_v), inv_I(b)));
    }
    return 1;
}

static int sphere_walls(const MoteWorld *w, MoteBody *b) {
    int hit = 0;
    float lo, hi;
    lo = w->bmin.x + b->radius; hi = w->bmax.x - b->radius;
    if (b->pos.x < lo) { b->pos.x = lo; hit |= collide_wall(w, b, v3( 1, 0, 0)); }
    if (b->pos.x > hi) { b->pos.x = hi; hit |= collide_wall(w, b, v3(-1, 0, 0)); }
    lo = w->bmin.y + b->radius; hi = w->bmax.y - b->radius;
    if (b->pos.y < lo) { b->pos.y = lo; hit |= collide_wall(w, b, v3(0,  1, 0)); }
    if (b->pos.y > hi) { b->pos.y = hi; hit |= collide_wall(w, b, v3(0, -1, 0)); }
    lo = w->bmin.z + b->radius; hi = w->bmax.z - b->radius;
    if (b->pos.z < lo) { b->pos.z = lo; hit |= collide_wall(w, b, v3(0, 0,  1)); }
    if (b->pos.z > hi) { b->pos.z = hi; hit |= collide_wall(w, b, v3(0, 0, -1)); }
    return hit;
}

/* --- box (OBB) physics -------------------------------------------------- */

/* Apply the body-frame diagonal inverse inertia in world space, without
 * forming the tensor: Iinv*v = sum_i d_i * a_i * (a_i . v), a_i = orient.r[i].
 * Box: I_x = (1/3) m (hy^2+hz^2) -> inv = 3*inv_mass/(hy^2+hz^2). */
MOTE_HOT static Vec3 iinv_mul(const MoteBody *b, Vec3 v) {
    Vec3 d;
    if (b->shape == MOTE_SHAPE_BOX) {
        float hx = b->half.x, hy = b->half.y, hz = b->half.z;
        d = v3(3.0f * b->inv_mass / (hy*hy + hz*hz),
               3.0f * b->inv_mass / (hx*hx + hz*hz),
               3.0f * b->inv_mass / (hx*hx + hy*hy));
    } else {
        float s = (b->radius > 1e-6f) ? 2.5f * b->inv_mass / (b->radius * b->radius) : 0.0f;
        d = v3(s, s, s);
    }
    Vec3 a0 = b->orient.r[0], a1 = b->orient.r[1], a2 = b->orient.r[2];
    return v3_add(v3_add(v3_scale(a0, d.x * v3_dot(a0, v)),
                         v3_scale(a1, d.y * v3_dot(a1, v))),
                  v3_scale(a2, d.z * v3_dot(a2, v)));
}

/* General contact between a body and an immovable plane (inward normal N) at
 * world contact point cp. Uses the full inertia tensor, so off-centre contacts
 * apply torque (a corner-down box tips, a face-down box rests). */
MOTE_HOT static void resolve_contact(const MoteWorld *w, MoteBody *b, Vec3 cp, Vec3 N) {
    Vec3 r = v3_sub(cp, b->pos);
    Vec3 vc = v3_add(b->vel, v3_cross(b->w, r));
    float vn = v3_dot(vc, N);
    if (vn >= 0.0f) return;

    Vec3 rn = v3_cross(r, N);
    float kn = b->inv_mass + v3_dot(N, v3_cross(iinv_mul(b, rn), r));
    if (kn < 1e-9f) return;

    float e = (-vn < REST_SLOP) ? 0.0f : w->restitution;
    float Jn = -(1.0f + e) * vn / kn;
    Vec3 Jn_v = v3_scale(N, Jn);
    b->vel = v3_add(b->vel, v3_scale(Jn_v, b->inv_mass));
    b->w   = v3_add(b->w, iinv_mul(b, v3_cross(r, Jn_v)));

    /* Friction. */
    vc = v3_add(b->vel, v3_cross(b->w, r));
    Vec3 vt = v3_sub(vc, v3_scale(N, v3_dot(vc, N)));
    float vtl = v3_len(vt);
    if (vtl > 1e-5f) {
        Vec3 t = v3_scale(vt, -1.0f / vtl);
        Vec3 rt = v3_cross(r, t);
        float kt = b->inv_mass + v3_dot(t, v3_cross(iinv_mul(b, rt), r));
        float Jt_stop = (kt > 1e-9f) ? vtl / kt : 0.0f;
        float Jt_max = w->friction * fabsf(Jn);
        float Jt = (Jt_stop < Jt_max) ? Jt_stop : Jt_max;
        Vec3 Jt_v = v3_scale(t, Jt);
        b->vel = v3_add(b->vel, v3_scale(Jt_v, b->inv_mass));
        b->w   = v3_add(b->w, iinv_mul(b, v3_cross(r, Jt_v)));
    }
}

MOTE_HOT static int box_walls(const MoteWorld *w, MoteBody *b) {
    struct { Vec3 N; float d; } wall[6] = {
        { v3( 1, 0, 0),  w->bmin.x }, { v3(-1, 0, 0), -w->bmax.x },
        { v3( 0, 1, 0),  w->bmin.y }, { v3( 0,-1, 0), -w->bmax.y },
        { v3( 0, 0, 1),  w->bmin.z }, { v3( 0, 0,-1), -w->bmax.z },
    };
    int hit = 0;
    for (int k = 0; k < 6; k++) {
        Vec3 N = wall[k].N; float d = wall[k].d;
        Vec3 cps[8]; int np = 0; float maxpen = 0.0f;
        for (int sx = -1; sx <= 1; sx += 2)
        for (int sy = -1; sy <= 1; sy += 2)
        for (int sz = -1; sz <= 1; sz += 2) {
            Vec3 lv = v3(sx * b->half.x, sy * b->half.y, sz * b->half.z);
            Vec3 wv = v3_add(b->pos, m3_mul_v3(&b->orient, lv));
            float pen = d - v3_dot(N, wv);          /* >0 = penetrating */
            if (pen > 0.0f) { cps[np++] = wv; if (pen > maxpen) maxpen = pen; }
        }
        if (np == 0) continue;
        hit = 1;
        b->pos = v3_add(b->pos, v3_scale(N, maxpen));     /* depenetrate */
        for (int i = 0; i < np; i++)
            resolve_contact(w, b, v3_add(cps[i], v3_scale(N, maxpen)), N);
    }
    return hit;
}

/* --- two-body contact (box-box, sphere-box) ----------------------------- */

static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

/* Resolve a contact between two bodies at world point cp with unit normal n
 * (n points the way `a` must move to separate from `b`). Full inertia tensor. */
MOTE_HOT static void resolve_pair(const MoteWorld *w, MoteBody *a, MoteBody *b,
                         Vec3 cp, Vec3 n) {
    Vec3 ra = v3_sub(cp, a->pos), rb = v3_sub(cp, b->pos);
    Vec3 vrel = v3_sub(v3_add(a->vel, v3_cross(a->w, ra)),
                       v3_add(b->vel, v3_cross(b->w, rb)));
    float vn = v3_dot(vrel, n);
    if (vn >= 0.0f) return;

    float kn = a->inv_mass + b->inv_mass
             + v3_dot(n, v3_cross(iinv_mul(a, v3_cross(ra, n)), ra))
             + v3_dot(n, v3_cross(iinv_mul(b, v3_cross(rb, n)), rb));
    if (kn < 1e-9f) return;

    float e = (-vn < REST_SLOP) ? 0.0f : w->restitution;
    float Jn = -(1.0f + e) * vn / kn;
    Vec3 Jnv = v3_scale(n, Jn);
    a->vel = v3_add(a->vel, v3_scale(Jnv, a->inv_mass));
    a->w   = v3_add(a->w, iinv_mul(a, v3_cross(ra, Jnv)));
    b->vel = v3_sub(b->vel, v3_scale(Jnv, b->inv_mass));
    b->w   = v3_sub(b->w, iinv_mul(b, v3_cross(rb, Jnv)));

    vrel = v3_sub(v3_add(a->vel, v3_cross(a->w, ra)),
                  v3_add(b->vel, v3_cross(b->w, rb)));
    Vec3 vt = v3_sub(vrel, v3_scale(n, v3_dot(vrel, n)));
    float vtl = v3_len(vt);
    if (vtl > 1e-5f) {
        Vec3 t = v3_scale(vt, -1.0f / vtl);
        float kt = a->inv_mass + b->inv_mass
                 + v3_dot(t, v3_cross(iinv_mul(a, v3_cross(ra, t)), ra))
                 + v3_dot(t, v3_cross(iinv_mul(b, v3_cross(rb, t)), rb));
        float Jt_stop = (kt > 1e-9f) ? vtl / kt : 0.0f;
        float Jt_max = w->friction * fabsf(Jn);
        float Jt = (Jt_stop < Jt_max) ? Jt_stop : Jt_max;
        Vec3 Jtv = v3_scale(t, Jt);
        a->vel = v3_add(a->vel, v3_scale(Jtv, a->inv_mass));
        a->w   = v3_add(a->w, iinv_mul(a, v3_cross(ra, Jtv)));
        b->vel = v3_sub(b->vel, v3_scale(Jtv, b->inv_mass));
        b->w   = v3_sub(b->w, iinv_mul(b, v3_cross(rb, Jtv)));
    }
}

static void depenetrate(MoteBody *a, MoteBody *b, Vec3 n, float pen) {
    float ws = a->inv_mass + b->inv_mass;
    if (ws <= 0.0f) return;
    a->pos = v3_add(a->pos, v3_scale(n, pen * (a->inv_mass / ws)));
    b->pos = v3_sub(b->pos, v3_scale(n, pen * (b->inv_mass / ws)));
}

/* Contacts for P's vertices that lie inside box Q (n = Q's face normal, out of
 * Q — the way P must move). Gives face-on-face stacking 4 contacts. */
MOTE_HOT static int verts_into_box(const MoteWorld *w, MoteBody *P, MoteBody *Q) {
    Vec3 qax[3] = { Q->orient.r[0], Q->orient.r[1], Q->orient.r[2] };
    float qh[3] = { Q->half.x, Q->half.y, Q->half.z };
    int hit = 0;
    for (int sx = -1; sx <= 1; sx += 2)
    for (int sy = -1; sy <= 1; sy += 2)
    for (int sz = -1; sz <= 1; sz += 2) {
        Vec3 lv = v3(sx * P->half.x, sy * P->half.y, sz * P->half.z);
        Vec3 wv = v3_add(P->pos, m3_mul_v3(&P->orient, lv));
        Vec3 rel = v3_sub(wv, Q->pos);
        float ql[3] = { v3_dot(rel, qax[0]), v3_dot(rel, qax[1]), v3_dot(rel, qax[2]) };
        if (fabsf(ql[0]) >= qh[0] || fabsf(ql[1]) >= qh[1] || fabsf(ql[2]) >= qh[2])
            continue;                                   /* vertex outside Q */
        int axis = 0; float minpen = qh[0] - fabsf(ql[0]);
        for (int k = 1; k < 3; k++) {
            float pen = qh[k] - fabsf(ql[k]);
            if (pen < minpen) { minpen = pen; axis = k; }
        }
        Vec3 n = v3_scale(qax[axis], (ql[axis] > 0.0f) ? 1.0f : -1.0f);
        depenetrate(P, Q, n, minpen);
        resolve_pair(w, P, Q, wv, n);
        hit = 1;
    }
    return hit;
}

MOTE_HOT static int box_box(const MoteWorld *w, MoteBody *a, MoteBody *b) {
    Vec3 d = v3_sub(b->pos, a->pos);
    float br = v3_len(a->half) + v3_len(b->half);
    if (v3_dot(d, d) > br * br) return 0;                /* broad phase */
    int hit = verts_into_box(w, a, b);
    hit |= verts_into_box(w, b, a);
    return hit;
}

/* Sphere S vs box B: nearest point on B to the sphere centre. */
MOTE_HOT static int sphere_box(const MoteWorld *w, MoteBody *S, MoteBody *B) {
    Vec3 ax[3] = { B->orient.r[0], B->orient.r[1], B->orient.r[2] };
    float h[3] = { B->half.x, B->half.y, B->half.z };
    Vec3 rel = v3_sub(S->pos, B->pos);
    float ql[3] = { v3_dot(rel, ax[0]), v3_dot(rel, ax[1]), v3_dot(rel, ax[2]) };
    float cl[3] = { clampf(ql[0], -h[0], h[0]),
                    clampf(ql[1], -h[1], h[1]),
                    clampf(ql[2], -h[2], h[2]) };
    Vec3 cp = v3_add(v3_add(v3_add(B->pos, v3_scale(ax[0], cl[0])),
                            v3_scale(ax[1], cl[1])), v3_scale(ax[2], cl[2]));
    Vec3 dv = v3_sub(S->pos, cp);
    float dist = v3_len(dv);
    Vec3 n; float pen;
    if (dist > 1e-5f) {
        if (dist >= S->radius) return 0;
        n = v3_scale(dv, 1.0f / dist);
        pen = S->radius - dist;
    } else {                                            /* centre inside box */
        int axis = 0; float minpen = h[0] - fabsf(ql[0]);
        for (int k = 1; k < 3; k++) { float p = h[k] - fabsf(ql[k]); if (p < minpen) { minpen = p; axis = k; } }
        n = v3_scale(ax[axis], (ql[axis] > 0.0f) ? 1.0f : -1.0f);
        pen = minpen + S->radius;
    }
    depenetrate(S, B, n, pen);
    resolve_pair(w, S, B, cp, n);
    return 1;
}

MOTE_HOT static int collide_bodies(const MoteWorld *w, MoteBody *a, MoteBody *b) {
    int abox = (a->shape == MOTE_SHAPE_BOX), bbox = (b->shape == MOTE_SHAPE_BOX);
    if (!abox && !bbox) return collide_pair(w, a, b);
    if (abox && bbox)   return box_box(w, a, b);
    return abox ? sphere_box(w, b, a) : sphere_box(w, a, b);
}

static void integrate(const MoteWorld *w, MoteBody *b, float h) {
    if (b->inv_mass <= 0.0f) return;
    b->vel = v3_add(b->vel, v3_scale(w->gravity, h));
    float ld = 1.0f - w->linear_damp * h;  if (ld < 0.0f) ld = 0.0f;
    float ad = 1.0f - w->angular_damp * h; if (ad < 0.0f) ad = 0.0f;
    b->vel = v3_scale(b->vel, ld);
    b->w   = v3_scale(b->w, ad);
    b->pos = v3_add(b->pos, v3_scale(b->vel, h));

    float wl = v3_len(b->w);
    if (wl > 1e-6f) {
        m3_rotate_world(&b->orient, v3_scale(b->w, 1.0f / wl), wl * h);
        m3_orthonormalize(&b->orient);
    }
}

/* --- broad-phase uniform grid ------------------------------------------- *
 * Bin bodies into a uniform grid (cell ~ 2x the largest body) so each body
 * only narrow-phases against its 27 neighbour cells -> ~O(n) instead of the
 * O(n^2) all-pairs check. The single biggest win for high body counts. */
static int   g_cell[GRID_CELLS];
static int   g_next[GRID_MAX_BODIES];
static int   g_nx, g_ny, g_nz;
static float g_cs, g_x0, g_y0, g_z0;

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static inline int cell_index(const MoteBody *b) {
    int cx = clampi((int)((b->pos.x - g_x0) / g_cs), 0, g_nx - 1);
    int cy = clampi((int)((b->pos.y - g_y0) / g_cs), 0, g_ny - 1);
    int cz = clampi((int)((b->pos.z - g_z0) / g_cs), 0, g_nz - 1);
    return (cz * g_ny + cy) * g_nx + cx;
}

static void grid_build(const MoteWorld *w, MoteBody *bodies, int n) {
    float maxr = 0.05f;
    for (int i = 0; i < n; i++) {
        const MoteBody *b = &bodies[i];
        float r = (b->shape == MOTE_SHAPE_BOX) ? v3_len(b->half) : b->radius;
        if (r > maxr) maxr = r;
    }
    g_cs = 2.0f * maxr;
    g_x0 = w->bmin.x; g_y0 = w->bmin.y; g_z0 = w->bmin.z;
    for (;;) {
        g_nx = (int)((w->bmax.x - w->bmin.x) / g_cs) + 1; if (g_nx < 1) g_nx = 1;
        g_ny = (int)((w->bmax.y - w->bmin.y) / g_cs) + 1; if (g_ny < 1) g_ny = 1;
        g_nz = (int)((w->bmax.z - w->bmin.z) / g_cs) + 1; if (g_nz < 1) g_nz = 1;
        if ((long)g_nx * g_ny * g_nz <= GRID_CELLS) break;
        g_cs *= 1.5f;                                  /* too many cells: coarsen */
    }
    int nc = g_nx * g_ny * g_nz;
    for (int c = 0; c < nc; c++) g_cell[c] = -1;
    for (int i = 0; i < n; i++) {
        int c = cell_index(&bodies[i]);
        g_next[i] = g_cell[c];
        g_cell[c] = i;
    }
}

MOTE_HOT static uint32_t grid_collide(const MoteWorld *w, MoteBody *bodies, int n) {
    uint32_t ev = 0;
    for (int i = 0; i < n; i++) {
        MoteBody *bi = &bodies[i];
        int cx = clampi((int)((bi->pos.x - g_x0) / g_cs), 0, g_nx - 1);
        int cy = clampi((int)((bi->pos.y - g_y0) / g_cs), 0, g_ny - 1);
        int cz = clampi((int)((bi->pos.z - g_z0) / g_cs), 0, g_nz - 1);
        for (int z = (cz ? cz - 1 : 0); z <= (cz < g_nz - 1 ? cz + 1 : cz); z++)
        for (int y = (cy ? cy - 1 : 0); y <= (cy < g_ny - 1 ? cy + 1 : cy); y++)
        for (int x = (cx ? cx - 1 : 0); x <= (cx < g_nx - 1 ? cx + 1 : cx); x++) {
            int c = (z * g_ny + y) * g_nx + x;
            for (int j = g_cell[c]; j >= 0; j = g_next[j])
                if (j > i && collide_bodies(w, bi, &bodies[j])) ev |= MOTE_PHYS_HIT;
        }
    }
    return ev;
}

MOTE_HOT
uint32_t mote_phys_step(MoteWorld *w, MoteBody *bodies, int n, float dt) {
    float h = (w->substep > 0.0f) ? w->substep : DEFAULT_H;
    uint32_t ev = 0;
    int use_grid = (n > 24 && n <= GRID_MAX_BODIES);   /* grid pays off past ~24 */
    int cap = (w->max_substeps > 0) ? w->max_substeps : MAX_SUBSTEPS;
    if (dt > 0.1f) dt = 0.1f;
    w->_acc += dt;
    int sub = 0;
    while (w->_acc >= h && sub < cap) {
        w->_acc -= h;
        for (int i = 0; i < n; i++) integrate(w, &bodies[i], h);
        if (use_grid) grid_build(w, bodies, n);
        for (int it = 0; it < SOLVER_ITERS; it++) {     /* relaxation passes */
            if (use_grid) {
                ev |= grid_collide(w, bodies, n);
            } else {
                for (int i = 0; i < n; i++)
                    for (int j = i + 1; j < n; j++)
                        if (collide_bodies(w, &bodies[i], &bodies[j])) ev |= MOTE_PHYS_HIT;
            }
            for (int i = 0; i < n; i++) {
                int h2 = (bodies[i].shape == MOTE_SHAPE_BOX)
                           ? box_walls(w, &bodies[i]) : sphere_walls(w, &bodies[i]);
                if (h2) ev |= MOTE_PHYS_HIT;
            }
        }
        sub++;
    }
    if (w->_acc > h) w->_acc = 0.0f;                    /* drop backlog: no spiral */
    return ev;
}
