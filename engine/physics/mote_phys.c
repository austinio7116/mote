/*
 * Mote — rigid-body physics solver (spheres + OBB boxes).
 *
 * Sequential-impulse solver with WARM-STARTING: each substep builds a contact
 * manifold (broad-phase grid + SAT/clip narrow phase), then iterates velocity
 * constraints reusing each contact's accumulated impulse cached from the
 * previous frame. Warm-starting is what lets tall stacks stay stable in a
 * handful of iterations — without it, Gauss-Seidel propagates floor support
 * only one body per iteration and deep stacks sink/collapse.
 *
 * Sleeping (position-based) parks settled bodies as immovable anchors so a
 * resting heap costs almost nothing. SI units; per-body inv_mass 0 = immovable.
 */
#include "mote_phys.h"
#include "mote_config.h"
#include <math.h>
#include <string.h>

#define DEFAULT_H    (1.0f / 240.0f)  /* default substep; per-world overridable */
#define REST_SLOP    0.45f            /* below this approach speed e=0 (no jitter-bounce) */
#define SOLVER_ITERS 8                /* velocity iterations per substep */
#define POS_ITERS    4                /* position (split-impulse) iterations */
#define PHYS_MAX_BODIES 256           /* split-impulse pseudo-velocity capacity */
#define MAX_SUBSTEPS 8                /* default cap (per-world overridable) + drop backlog */
#define GRID_MAX_BODIES 256
#define GRID_CELLS   4096
#define MAX_CONTACTS 768              /* manifold capacity (excess pairs dropped) */
#define CACHE_N      1024             /* warm-start impulse cache (power of two) */
#define POS_BETA     0.2f             /* Baumgarte position-bias factor */
#define POS_SLOP     0.005f           /* allowed penetration before bias kicks in */
#define WAKE_PEN     0.02f            /* an awake body intruding this deep wakes a sleeper */
#define WAKE_VEL     0.30f            /* ...or contacting a sleeper while moving this fast
                                       * (a fast strike wakes before it can penetrate WAKE_PEN) */
#define SLEEP_DIST2  (0.025f * 0.025f)
#define SLEEP_ANG2   (0.40f * 0.40f)
#define SLEEP_FRAMES 20

static inline float u2f(uint32_t u) { float f; __builtin_memcpy(&f, &u, 4); return f; }
static inline uint32_t f2u(float f) { uint32_t u; __builtin_memcpy(&u, &f, 4); return u; }
static inline int body_asleep(const MoteBody *b) { return b->_reserved[0] >= SLEEP_FRAMES; }
static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
static inline Vec3 v3_perp(Vec3 n) {                 /* any unit vector perpendicular to n */
    Vec3 a = (fabsf(n.x) < 0.9f) ? v3(1, 0, 0) : v3(0, 1, 0);
    return v3_norm(v3_cross(n, a));
}
static inline float half_i(const MoteBody *b, int i) { return (i == 0) ? b->half.x : (i == 1) ? b->half.y : b->half.z; }

void mote_phys_world_defaults(MoteWorld *w) {
    w->gravity = v3(0.0f, -9.8f, 0.0f);
    w->bmin = v3(-2.0f, -2.0f, -2.0f);
    w->bmax = v3( 2.0f,  2.0f,  2.0f);
    w->walls = 1;
    w->restitution = 0.6f;
    w->friction    = 0.3f;
    w->linear_damp  = 0.05f;
    w->angular_damp = 0.4f;
    w->substep = DEFAULT_H;
    w->max_substeps = MAX_SUBSTEPS;
    w->_acc = 0.0f;
}

/* Asleep bodies act as immovable anchors (no mass, no inertia) so an awake body
 * resting on them can't push them around (which used to collapse stacks). */
static inline float eff_im(const MoteBody *b) { return body_asleep(b) ? 0.0f : b->inv_mass; }

/* World-space inverse-inertia applied to v (body-frame diagonal, no tensor). */
static Vec3 iinv_mul(const MoteBody *b, Vec3 v) {
    if (body_asleep(b)) return v3(0, 0, 0);
    Vec3 d;
    if (b->shape == MOTE_SHAPE_BOX || b->shape == MOTE_SHAPE_CAPSULE) {
        float hx, hy, hz;
        if (b->shape == MOTE_SHAPE_CAPSULE) { hx = hz = b->radius; hy = b->half.y + b->radius; }
        else { hx = b->half.x; hy = b->half.y; hz = b->half.z; }
        d = v3(3.0f * b->inv_mass / (hy*hy + hz*hz),
               3.0f * b->inv_mass / (hx*hx + hz*hz),
               3.0f * b->inv_mass / (hx*hx + hy*hy));
    } else {
        float r = b->radius;
        if (b->shape == MOTE_SHAPE_HULL && b->shape_data) r = ((const MoteHull *)b->shape_data)->bound_r;
        float s = (r > 1e-6f) ? 2.5f * b->inv_mass / (r * r) : 0.0f;
        d = v3(s, s, s);
    }
    Vec3 a0 = b->orient.r[0], a1 = b->orient.r[1], a2 = b->orient.r[2];
    return v3_add(v3_add(v3_scale(a0, d.x * v3_dot(a0, v)),
                         v3_scale(a1, d.y * v3_dot(a1, v))),
                  v3_scale(a2, d.z * v3_dot(a2, v)));
}

/* --- contact manifold --------------------------------------------------- */

typedef struct {
    int a, b;          /* body indices; b < 0 => immovable wall */
    Vec3 n;            /* unit normal: the way body a moves to separate from b */
    Vec3 t;            /* friction tangent (precomputed) */
    Vec3 p;            /* world contact point */
    float pen;         /* penetration depth */
    float kn, kt;      /* effective masses along n, t */
    float bias;        /* target separating velocity (restitution only) */
    float mu, emat;    /* combined friction + restitution for this contact */
    float jn, jt;      /* accumulated impulses (warm-started) */
    float jp;          /* accumulated position (split-impulse) impulse */
    uint32_t key;      /* warm-start match key */
} Contact;

static Contact s_ct[MAX_CONTACTS];
static int     s_nct;
static Vec3    s_pv[PHYS_MAX_BODIES], s_pw[PHYS_MAX_BODIES];  /* position pseudo-velocities */
static uint8_t s_touch[PHYS_MAX_BODIES];                      /* had a contact this substep */
static float   s_pen[PHYS_MAX_BODIES];                        /* deepest penetration this substep */
static uint8_t s_woke[PHYS_MAX_BODIES];                       /* woke this substep -> no warm-start */

typedef struct { uint32_t key; float jn, jt; } Imp;
static Imp  s_cacheA[CACHE_N], s_cacheB[CACHE_N];
static Imp *s_cache_prev = s_cacheA, *s_cache_cur = s_cacheB;

static void cache_lookup(uint32_t key, float *jn, float *jt) {
    uint32_t i = key & (CACHE_N - 1);
    for (int p = 0; p < 8; p++) {
        Imp *e = &s_cache_prev[(i + p) & (CACHE_N - 1)];
        if (e->key == key) { *jn = e->jn; *jt = e->jt; return; }
        if (e->key == 0) break;
    }
    *jn = 0.0f; *jt = 0.0f;
}
static void cache_store(uint32_t key, float jn, float jt) {
    uint32_t i = key & (CACHE_N - 1);
    for (int p = 0; p < 8; p++) {
        Imp *e = &s_cache_cur[(i + p) & (CACHE_N - 1)];
        if (e->key == 0 || e->key == key) { e->key = key; e->jn = jn; e->jt = jt; return; }
    }
}

static void add_contact(int a, int b, Vec3 n, Vec3 p, float pen, uint32_t fid) {
    if (s_nct >= MAX_CONTACTS) return;
    Contact *c = &s_ct[s_nct++];
    c->a = a; c->b = b; c->n = n; c->p = p; c->pen = pen;
    c->jn = c->jt = 0.0f;
    uint32_t k = ((uint32_t)(a + 1) * 73856093u) ^ ((uint32_t)(b + 2) * 19349663u) ^ (fid * 83492791u);
    c->key = k ? k : 1u;
    if (a < PHYS_MAX_BODIES) { s_touch[a] = 1; if (pen > s_pen[a]) s_pen[a] = pen; }
    if (b >= 0 && b < PHYS_MAX_BODIES) { s_touch[b] = 1; if (pen > s_pen[b]) s_pen[b] = pen; }
}

/* --- narrow phase: emit contacts ---------------------------------------- */

/* sphere i vs sphere j */
static void gen_sphere_sphere(MoteBody *bodies, int i, int j) {
    MoteBody *a = &bodies[i], *b = &bodies[j];
    Vec3 d = v3_sub(b->pos, a->pos);
    float dist = v3_len(d), mind = a->radius + b->radius;
    if (dist >= mind || dist < 1e-6f) return;
    Vec3 nij = v3_scale(d, 1.0f / dist);              /* i -> j */
    Vec3 cp = v3_add(a->pos, v3_scale(nij, a->radius));
    add_contact(i, j, v3_scale(nij, -1.0f), cp, mind - dist, 0);  /* a moves -nij */
}

static float obb_radius(const MoteBody *b, Vec3 ax) {
    return fabsf(v3_dot(b->orient.r[0], ax)) * b->half.x
         + fabsf(v3_dot(b->orient.r[1], ax)) * b->half.y
         + fabsf(v3_dot(b->orient.r[2], ax)) * b->half.z;
}
static void face_corners(const MoteBody *bx, int ax, int s, Vec3 out[4]) {
    int u = (ax + 1) % 3, v = (ax + 2) % 3;
    Vec3 c  = v3_add(bx->pos, v3_scale(bx->orient.r[ax], (float)s * half_i(bx, ax)));
    Vec3 du = v3_scale(bx->orient.r[u], half_i(bx, u));
    Vec3 dv = v3_scale(bx->orient.r[v], half_i(bx, v));
    out[0] = v3_add(v3_add(c, du), dv);
    out[1] = v3_add(v3_sub(c, du), dv);
    out[2] = v3_sub(v3_sub(c, du), dv);
    out[3] = v3_sub(v3_add(c, du), dv);
}
static int clip_plane(const Vec3 *in, int n, Vec3 p, Vec3 m, Vec3 *out) {
    int cnt = 0;
    for (int i = 0; i < n; i++) {
        Vec3 A = in[i], B = in[(i + 1) % n];
        float da = v3_dot(v3_sub(A, p), m), db = v3_dot(v3_sub(B, p), m);
        if (da <= 0.0f) out[cnt++] = A;
        if ((da < 0.0f) != (db < 0.0f)) {
            float t = da / (da - db);
            out[cnt++] = v3_add(A, v3_scale(v3_sub(B, A), t));
        }
    }
    return cnt;
}

/* box i vs box j: SAT + clipped face manifold. Emits up to ~4 contacts; the
 * incident box (whichever) becomes contact body `a`, moving along the ref normal. */
static void gen_box_box(MoteBody *bodies, int i, int j) {
    MoteBody *a = &bodies[i], *b = &bodies[j];
    Vec3 d = v3_sub(b->pos, a->pos);
    float br = v3_len(a->half) + v3_len(b->half);
    if (v3_dot(d, d) > br * br) return;

    Vec3 axes[15]; int na = 0;
    for (int k = 0; k < 3; k++) axes[na++] = a->orient.r[k];
    for (int k = 0; k < 3; k++) axes[na++] = b->orient.r[k];
    for (int x = 0; x < 3; x++) for (int y = 0; y < 3; y++) {
        Vec3 c = v3_cross(a->orient.r[x], b->orient.r[y]);
        float l2 = v3_dot(c, c);
        if (l2 > 1e-6f) axes[na++] = v3_scale(c, 1.0f / sqrtf(l2));
    }
    float bestOv = 1e30f; Vec3 bestN = v3(0, 1, 0);
    for (int k = 0; k < na; k++) {
        Vec3 ax = axes[k];
        float dist = v3_dot(d, ax);
        float ov = obb_radius(a, ax) + obb_radius(b, ax) - fabsf(dist);
        if (ov <= 0.0f) return;                          /* separating axis */
        if (ov < bestOv) { bestOv = ov; bestN = (dist < 0.0f) ? v3_scale(ax, -1.0f) : ax; }
    }
    /* bestN points a -> b. reference box = the one with a face most parallel to it. */
    int refIsA = 1, rax = 0, rs = 1; float bestAlign = -1.0f;
    for (int k = 0; k < 3; k++) {
        float da = v3_dot(a->orient.r[k], bestN);
        if (fabsf(da) > bestAlign) { bestAlign = fabsf(da); refIsA = 1; rax = k; rs = (da > 0) ? 1 : -1; }
        float db = v3_dot(b->orient.r[k], bestN);
        if (fabsf(db) > bestAlign) { bestAlign = fabsf(db); refIsA = 0; rax = k; rs = (db > 0) ? -1 : 1; }
    }
    MoteBody *ref = refIsA ? a : b, *inc = refIsA ? b : a;
    int incIdx = refIsA ? j : i, refIdx = refIsA ? i : j;
    Vec3 refN = v3_scale(ref->orient.r[rax], (float)rs);

    int iax = 0, is = 1; float bd = 1e30f;
    for (int k = 0; k < 3; k++) {
        float dp = v3_dot(inc->orient.r[k], refN);
        if (dp < bd)  { bd = dp;  iax = k; is = 1; }
        if (-dp < bd) { bd = -dp; iax = k; is = -1; }
    }
    Vec3 poly[16], tmp[16];
    face_corners(inc, iax, is, poly);
    int u = (rax + 1) % 3, v = (rax + 2) % 3;
    Vec3 refC = v3_add(ref->pos, v3_scale(refN, half_i(ref, rax)));
    Vec3 du = ref->orient.r[u]; float hu = half_i(ref, u);
    Vec3 dv = ref->orient.r[v]; float hv = half_i(ref, v);
    int n = 4;
    n = clip_plane(poly, n, v3_add(refC, v3_scale(du,  hu)), du,                 tmp);
    n = clip_plane(tmp,  n, v3_sub(refC, v3_scale(du,  hu)), v3_scale(du, -1.0f), poly);
    n = clip_plane(poly, n, v3_add(refC, v3_scale(dv,  hv)), dv,                 tmp);
    n = clip_plane(tmp,  n, v3_sub(refC, v3_scale(dv,  hv)), v3_scale(dv, -1.0f), poly);

    int emitted = 0;
    for (int k = 0; k < n; k++) {
        float pen = -v3_dot(v3_sub(poly[k], refC), refN);
        if (pen > 0.0f) {                                /* inc moves +refN off ref */
            add_contact(incIdx, refIdx, refN, poly[k], pen, (uint32_t)(k + 1));
            emitted++;
        }
    }
    if (!emitted) {                                      /* edge-edge fallback: 1 contact */
        Vec3 cp = v3_add(a->pos, v3_scale(bestN, obb_radius(a, bestN)));
        add_contact(j, i, v3_scale(bestN, -1.0f), cp, bestOv, 9);  /* j moves -bestN */
    }
}

/* sphere S(idx si) vs box B(idx bi): nearest point on box. */
static void gen_sphere_box(MoteBody *bodies, int si, int bi) {
    MoteBody *S = &bodies[si], *B = &bodies[bi];
    Vec3 ax[3] = { B->orient.r[0], B->orient.r[1], B->orient.r[2] };
    float h[3] = { B->half.x, B->half.y, B->half.z };
    Vec3 rel = v3_sub(S->pos, B->pos);
    float ql[3] = { v3_dot(rel, ax[0]), v3_dot(rel, ax[1]), v3_dot(rel, ax[2]) };
    float cl[3] = { clampf(ql[0], -h[0], h[0]), clampf(ql[1], -h[1], h[1]), clampf(ql[2], -h[2], h[2]) };
    Vec3 cp = v3_add(v3_add(v3_add(B->pos, v3_scale(ax[0], cl[0])),
                            v3_scale(ax[1], cl[1])), v3_scale(ax[2], cl[2]));
    Vec3 dv = v3_sub(S->pos, cp);
    float dist = v3_len(dv);
    Vec3 n; float pen;
    if (dist > 1e-5f) {
        if (dist >= S->radius) return;
        n = v3_scale(dv, 1.0f / dist); pen = S->radius - dist;
    } else {
        int axis = 0; float minpen = h[0] - fabsf(ql[0]);
        for (int k = 1; k < 3; k++) { float p = h[k] - fabsf(ql[k]); if (p < minpen) { minpen = p; axis = k; } }
        n = v3_scale(ax[axis], (ql[axis] > 0.0f) ? 1.0f : -1.0f); pen = minpen + S->radius;
    }
    add_contact(si, bi, n, cp, pen, 0);                  /* sphere moves +n off box */
}

/* --- GJK + EPA for convex shapes (capsule / hull and any pair with them) - */

/* Farthest world-space point of a body in direction d. */
static Vec3 mote_support(const MoteBody *b, Vec3 d) {
    switch (b->shape) {
    case MOTE_SHAPE_SPHERE:
        return v3_add(b->pos, v3_scale(v3_norm(d), b->radius));
    case MOTE_SHAPE_CAPSULE: {
        Vec3 ax = b->orient.r[1];
        Vec3 c = v3_add(b->pos, v3_scale(ax, v3_dot(d, ax) >= 0.0f ? b->half.y : -b->half.y));
        return v3_add(c, v3_scale(v3_norm(d), b->radius));
    }
    case MOTE_SHAPE_HULL: {
        const MoteHull *h = (const MoteHull *)b->shape_data;
        Vec3 ld = m3_mul_v3_t(&b->orient, d);
        int best = 0; float bd = -1e30f;
        for (int i = 0; i < h->nverts; i++) { float dd = v3_dot(h->verts[i], ld); if (dd > bd) { bd = dd; best = i; } }
        return v3_add(b->pos, m3_mul_v3(&b->orient, h->verts[best]));
    }
    default: {  /* BOX */
        Vec3 ld = m3_mul_v3_t(&b->orient, d);
        Vec3 lp = v3(ld.x >= 0 ? b->half.x : -b->half.x,
                     ld.y >= 0 ? b->half.y : -b->half.y,
                     ld.z >= 0 ? b->half.z : -b->half.z);
        return v3_add(b->pos, m3_mul_v3(&b->orient, lp));
    }
    }
}

typedef struct { Vec3 v, a, b; } SupPt;   /* CSO point + the two witness supports */

static SupPt cso_support(const MoteBody *A, const MoteBody *B, Vec3 d) {
    SupPt s;
    s.a = mote_support(A, d);
    s.b = mote_support(B, v3_scale(d, -1.0f));
    s.v = v3_sub(s.a, s.b);
    return s;
}

/* GJK: returns 1 if A,B overlap, leaving a tetrahedron simplex in sp[0..3]. */
static int gjk(const MoteBody *A, const MoteBody *B, SupPt sp[4]) {
    Vec3 d = v3_sub(B->pos, A->pos);
    if (v3_len2(d) < 1e-12f) d = v3(1, 0, 0);
    sp[0] = cso_support(A, B, d);
    int n = 1;
    d = v3_scale(sp[0].v, -1.0f);
    for (int iter = 0; iter < 40; iter++) {
        if (v3_len2(d) < 1e-12f) return 1;               /* origin on simplex */
        SupPt s = cso_support(A, B, d);
        if (v3_dot(s.v, d) < 0.0f) return 0;             /* no overlap */
        sp[n++] = s;
        /* --- evolve simplex toward the origin --- */
        Vec3 ao = v3_scale(sp[n - 1].v, -1.0f);
        if (n == 2) {
            Vec3 ab = v3_sub(sp[0].v, sp[1].v);
            d = v3_cross(v3_cross(ab, ao), ab);
            if (v3_len2(d) < 1e-12f) d = v3_cross(ab, v3(1,0,0)), d = (v3_len2(d)<1e-12f)?v3_cross(ab,v3(0,1,0)):d;
        } else if (n == 3) {
            Vec3 a = sp[2].v, b = sp[1].v, c = sp[0].v;
            Vec3 abc = v3_cross(v3_sub(b, a), v3_sub(c, a));
            d = (v3_dot(abc, v3_scale(a, -1.0f)) >= 0.0f) ? abc : v3_scale(abc, -1.0f);
        } else {  /* n == 4 tetrahedron: which face faces the origin? */
            Vec3 a = sp[3].v, b = sp[2].v, c = sp[1].v, dd = sp[0].v;
            Vec3 nabc = v3_cross(v3_sub(b, a), v3_sub(c, a));
            Vec3 nacd = v3_cross(v3_sub(c, a), v3_sub(dd, a));
            Vec3 nadb = v3_cross(v3_sub(dd, a), v3_sub(b, a));
            Vec3  A0 = v3_scale(a, -1.0f);
            if (v3_dot(nabc, v3_sub(dd, a)) > 0.0f) nabc = v3_scale(nabc, -1.0f);
            if (v3_dot(nacd, v3_sub(b, a))  > 0.0f) nacd = v3_scale(nacd, -1.0f);
            if (v3_dot(nadb, v3_sub(c, a))  > 0.0f) nadb = v3_scale(nadb, -1.0f);
            if (v3_dot(nabc, A0) > 0.0f)      { sp[0] = sp[1]; sp[1] = sp[2]; sp[2] = sp[3]; n = 3; d = nabc; }
            else if (v3_dot(nacd, A0) > 0.0f) { sp[1] = sp[2]; sp[2] = sp[3]; n = 3; d = nacd; }
            else if (v3_dot(nadb, A0) > 0.0f) { sp[0] = sp[1]; sp[2] = sp[3]; n = 3; d = nadb; }
            else return 1;                               /* origin inside tetra */
        }
    }
    return 1;
}

#define EPA_MAXF 64
#define EPA_MAXV 40

/* EPA: from the GJK tetrahedron, find penetration normal/depth + contact point. */
static int epa(const MoteBody *A, const MoteBody *B, SupPt sp[4],
               Vec3 *out_n, float *out_depth, Vec3 *out_point) {
    SupPt verts[EPA_MAXV]; int nv = 4;
    for (int i = 0; i < 4; i++) verts[i] = sp[i];
    int faces[EPA_MAXF][3]; int nf = 0;
    /* seed tetra faces (outward winding fixed below) */
    int seed[4][3] = { {0,1,2}, {0,3,1}, {0,2,3}, {1,3,2} };
    for (int f = 0; f < 4; f++) { faces[nf][0]=seed[f][0]; faces[nf][1]=seed[f][1]; faces[nf][2]=seed[f][2]; nf++; }

    for (int iter = 0; iter < 32; iter++) {
        /* closest face to origin */
        int best = -1; float bestDist = 1e30f; Vec3 bestN = v3(0,1,0);
        for (int f = 0; f < nf; f++) {
            Vec3 a = verts[faces[f][0]].v, b = verts[faces[f][1]].v, c = verts[faces[f][2]].v;
            Vec3 nrm = v3_cross(v3_sub(b, a), v3_sub(c, a));
            float l = v3_len(nrm); if (l < 1e-9f) continue;
            nrm = v3_scale(nrm, 1.0f / l);
            float dist = v3_dot(nrm, a);
            if (dist < 0.0f) { nrm = v3_scale(nrm, -1.0f); dist = -dist; }  /* face origin-facing */
            if (dist < bestDist) { bestDist = dist; best = f; bestN = nrm; }
        }
        if (best < 0) return 0;
        SupPt s = cso_support(A, B, bestN);
        float sd = v3_dot(s.v, bestN);
        if (sd - bestDist < 1e-4f || nv >= EPA_MAXV) {
            /* converged: barycentric contact point on the best face */
            int *fa = faces[best];
            Vec3 a = verts[fa[0]].v, b = verts[fa[1]].v, c = verts[fa[2]].v;
            Vec3 p = v3_scale(bestN, bestDist);          /* projection of origin onto face */
            float u, v, ww; {
                Vec3 v0 = v3_sub(b, a), v1 = v3_sub(c, a), v2 = v3_sub(p, a);
                float d00=v3_dot(v0,v0), d01=v3_dot(v0,v1), d11=v3_dot(v1,v1), d20=v3_dot(v2,v0), d21=v3_dot(v2,v1);
                float den = d00*d11 - d01*d01; if (fabsf(den) < 1e-12f) den = 1e-12f;
                v = (d11*d20 - d01*d21)/den; ww = (d00*d21 - d01*d20)/den; u = 1.0f - v - ww;
            }
            Vec3 pa = v3_add(v3_add(v3_scale(verts[fa[0]].a, u), v3_scale(verts[fa[1]].a, v)), v3_scale(verts[fa[2]].a, ww));
            *out_n = bestN; *out_depth = bestDist; *out_point = pa;
            return 1;
        }
        /* expand: remove faces seen by s, re-triangulate the horizon */
        int horizon[EPA_MAXF][2]; int nh = 0;
        for (int f = 0; f < nf; ) {
            Vec3 a = verts[faces[f][0]].v, b = verts[faces[f][1]].v, c = verts[faces[f][2]].v;
            Vec3 nrm = v3_cross(v3_sub(b, a), v3_sub(c, a));
            int vis = v3_dot(nrm, v3_sub(s.v, a)) > 0.0f;
            if (vis) {
                int e[3][2] = { {faces[f][0],faces[f][1]}, {faces[f][1],faces[f][2]}, {faces[f][2],faces[f][0]} };
                for (int k = 0; k < 3; k++) {
                    int found = -1;
                    for (int hh = 0; hh < nh; hh++) if (horizon[hh][0]==e[k][1] && horizon[hh][1]==e[k][0]) { found = hh; break; }
                    if (found >= 0) { horizon[found][0]=horizon[nh-1][0]; horizon[found][1]=horizon[nh-1][1]; nh--; }
                    else if (nh < EPA_MAXF) { horizon[nh][0]=e[k][0]; horizon[nh][1]=e[k][1]; nh++; }
                }
                faces[f][0]=faces[nf-1][0]; faces[f][1]=faces[nf-1][1]; faces[f][2]=faces[nf-1][2]; nf--;
            } else f++;
        }
        int si = nv; verts[nv++] = s;
        for (int hh = 0; hh < nh && nf < EPA_MAXF; hh++) {
            faces[nf][0]=horizon[hh][0]; faces[nf][1]=horizon[hh][1]; faces[nf][2]=si; nf++;
        }
    }
    return 0;
}

/* --- faceted convex manifold (SAT + face clipping) ---------------------- *
 * Multi-point contacts for box/hull pairs so they rest and stack stably. A box
 * and a hull both expand to a Cvx (world verts + face planes + edges); a single
 * routine SATs them and clips the incident face for up to ~4 contacts. (Round
 * shapes — sphere/capsule — keep the GJK single-contact path; that's correct
 * for them.) */
#define CVX_MAXV 64
#define CVX_MAXF 40
#define CVX_MAXFV 200
#define CVX_MAXE 110
typedef struct {
    Vec3 v[CVX_MAXV]; int nv;
    Vec3 fn[CVX_MAXF]; uint8_t fo[CVX_MAXF + 1]; int nf;
    uint8_t fvi[CVX_MAXFV];
    uint8_t ev[CVX_MAXE * 2]; int ne;
} Cvx;
static Cvx s_cxa, s_cxb;

/* box face/edge tables; vertex index = (sx>0)*4 + (sy>0)*2 + (sz>0). */
static const uint8_t k_box_fo[7] = { 0, 4, 8, 12, 16, 20, 24 };
static const uint8_t k_box_fv[24] = {
    4,5,7,6,  0,2,3,1,  2,6,7,3,  0,1,5,4,  1,3,7,5,  0,4,6,2,
};
static const uint8_t k_box_ev[24] = {
    0,1, 1,3, 3,2, 2,0,  4,5, 5,7, 7,6, 6,4,  0,4, 1,5, 3,7, 2,6,
};

static void build_cvx(const MoteBody *b, Cvx *c) {
    if (b->shape == MOTE_SHAPE_HULL) {
        const MoteHull *h = (const MoteHull *)b->shape_data;
        c->nv = h->nverts;
        for (int i = 0; i < h->nverts; i++) c->v[i] = v3_add(b->pos, m3_mul_v3(&b->orient, h->verts[i]));
        c->nf = h->nfaces;
        for (int f = 0; f < h->nfaces; f++) c->fn[f] = m3_mul_v3(&b->orient, h->fnorm[f]);
        for (int f = 0; f <= h->nfaces; f++) c->fo[f] = h->faceoff[f];
        for (int k = 0; k < h->faceoff[h->nfaces]; k++) c->fvi[k] = h->facev[k];
        c->ne = h->nedges;
        for (int k = 0; k < h->nedges * 2; k++) c->ev[k] = h->edges[k];
        return;
    }
    /* BOX */
    int idx = 0;
    for (int sx = -1; sx <= 1; sx += 2) for (int sy = -1; sy <= 1; sy += 2) for (int sz = -1; sz <= 1; sz += 2)
        c->v[idx++] = v3_add(b->pos, m3_mul_v3(&b->orient, v3(sx*b->half.x, sy*b->half.y, sz*b->half.z)));
    c->nv = 8;
    c->nf = 6;
    c->fn[0] = b->orient.r[0]; c->fn[1] = v3_scale(b->orient.r[0], -1);
    c->fn[2] = b->orient.r[1]; c->fn[3] = v3_scale(b->orient.r[1], -1);
    c->fn[4] = b->orient.r[2]; c->fn[5] = v3_scale(b->orient.r[2], -1);
    for (int k = 0; k < 7; k++) c->fo[k] = k_box_fo[k];
    for (int k = 0; k < 24; k++) c->fvi[k] = k_box_fv[k];
    c->ne = 12;
    for (int k = 0; k < 24; k++) c->ev[k] = k_box_ev[k];
}

/* deepest separation of B below A's faces; returns best (>0 separated) + face. */
static float face_query(const Cvx *A, const Cvx *B, int *bf) {
    float best = -1e30f; *bf = 0;
    for (int f = 0; f < A->nf; f++) {
        Vec3 n = A->fn[f], p = A->v[A->fvi[A->fo[f]]];
        float minB = 1e30f;
        for (int i = 0; i < B->nv; i++) { float d = v3_dot(n, v3_sub(B->v[i], p)); if (d < minB) minB = d; }
        if (minB > best) { best = minB; *bf = f; }
    }
    return best;
}

/* clip polygon `poly`(n) against the half-space {x: dot(x-p,m) <= 0}. */
static int clip_hs(const Vec3 *in, int n, Vec3 p, Vec3 m, Vec3 *out) {
    int cnt = 0;
    for (int i = 0; i < n; i++) {
        Vec3 a = in[i], b = in[(i + 1) % n];
        float da = v3_dot(v3_sub(a, p), m), db = v3_dot(v3_sub(b, p), m);
        if (da <= 0.0f) out[cnt++] = a;
        if ((da < 0.0f) != (db < 0.0f)) { float t = da / (da - db); out[cnt++] = v3_add(a, v3_scale(v3_sub(b, a), t)); }
    }
    return cnt;
}

/* Triangle (a,b,c) as a thin two-sided Cvx (for mesh-collider contacts). */
static void build_cvx_tri(Vec3 a, Vec3 b, Vec3 c, Cvx *cx) {
    cx->v[0] = a; cx->v[1] = b; cx->v[2] = c; cx->nv = 3;
    Vec3 n = v3_norm(v3_cross(v3_sub(b, a), v3_sub(c, a)));
    cx->fn[0] = n; cx->fn[1] = v3_scale(n, -1.0f); cx->nf = 2;
    cx->fo[0] = 0; cx->fo[1] = 3; cx->fo[2] = 6;
    cx->fvi[0]=0; cx->fvi[1]=1; cx->fvi[2]=2;  cx->fvi[3]=0; cx->fvi[4]=2; cx->fvi[5]=1;
    cx->ev[0]=0;cx->ev[1]=1; cx->ev[2]=1;cx->ev[3]=2; cx->ev[4]=2;cx->ev[5]=0; cx->ne=3;
}

/* Two faceted convexes -> multi-point manifold (SAT + incident-face clip). */
static void convex_manifold(const Cvx *A, const Cvx *B, int ia, int ib, uint32_t fid_base) {
    int fa, fb;
    float sa = face_query(A, B, &fa);
    if (sa > 0.0f) return;
    float sb = face_query(B, A, &fb);
    if (sb > 0.0f) return;

    /* reference = the face with the least penetration (separation closest to 0) */
    int refA = (sa >= sb - 1e-4f);
    const Cvx *R = refA ? A : B, *I = refA ? B : A;
    int rf = refA ? fa : fb;
    Vec3 rn = R->fn[rf];
    /* incident face = I's face most anti-parallel to rn */
    int inf = 0; float bd = 1e30f;
    for (int f = 0; f < I->nf; f++) { float d = v3_dot(I->fn[f], rn); if (d < bd) { bd = d; inf = f; } }

    /* incident polygon */
    Vec3 poly[32], tmp[32];
    int np = I->fo[inf + 1] - I->fo[inf];
    if (np > 32) np = 32;
    for (int k = 0; k < np; k++) poly[k] = I->v[I->fvi[I->fo[inf] + k]];

    /* clip against each side plane of the reference face (winding-independent:
     * orient each side-plane normal away from the face centroid). */
    int rc = R->fo[rf + 1] - R->fo[rf];
    Vec3 cen = v3(0, 0, 0);
    for (int k = 0; k < rc; k++) cen = v3_add(cen, R->v[R->fvi[R->fo[rf] + k]]);
    cen = v3_scale(cen, 1.0f / (float)rc);
    for (int k = 0; k < rc; k++) {
        Vec3 a = R->v[R->fvi[R->fo[rf] + k]];
        Vec3 b = R->v[R->fvi[R->fo[rf] + (k + 1) % rc]];
        Vec3 m = v3_cross(v3_sub(b, a), rn);
        float l = v3_len(m); if (l < 1e-9f) continue;
        m = v3_scale(m, 1.0f / l);
        if (v3_dot(m, v3_sub(cen, a)) > 0.0f) m = v3_scale(m, -1.0f);  /* outward */
        np = clip_hs(poly, np, a, m, tmp);
        for (int t = 0; t < np; t++) poly[t] = tmp[t];
        if (np == 0) break;
    }

    /* keep points below the reference face -> contacts. Normal: body `ia`
     * separates along +n. rn points out of the reference body. */
    Vec3 refPt = R->v[R->fvi[R->fo[rf]]];
    Vec3 n_ia = refA ? v3_scale(rn, -1.0f) : rn;   /* dir ia moves to separate */
    for (int k = 0; k < np; k++) {
        float pen = -v3_dot(v3_sub(poly[k], refPt), rn);
        if (pen > 0.0f) add_contact(ia, ib, n_ia, poly[k], pen, fid_base + (uint32_t)k);
    }
}

static void gen_convex(MoteBody *bodies, int ia, int ib) {
    build_cvx(&bodies[ia], &s_cxa);
    build_cvx(&bodies[ib], &s_cxb);
    convex_manifold(&s_cxa, &s_cxb, ia, ib, 300);
}

/* Closest point on triangle (a,b,c) to p (Ericson, Real-Time Collision Det.). */
static Vec3 closest_tri(Vec3 p, Vec3 a, Vec3 b, Vec3 c) {
    Vec3 ab = v3_sub(b, a), ac = v3_sub(c, a), ap = v3_sub(p, a);
    float d1 = v3_dot(ab, ap), d2 = v3_dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return a;
    Vec3 bp = v3_sub(p, b); float d3 = v3_dot(ab, bp), d4 = v3_dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return b;
    float vc = d1*d4 - d3*d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return v3_add(a, v3_scale(ab, d1/(d1-d3)));
    Vec3 cp = v3_sub(p, c); float d5 = v3_dot(ab, cp), d6 = v3_dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return c;
    float vb = d5*d2 - d1*d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return v3_add(a, v3_scale(ac, d2/(d2-d6)));
    float va = d3*d6 - d5*d4;
    if (va <= 0 && (d4-d3) >= 0 && (d5-d6) >= 0) { float w=(d4-d3)/((d4-d3)+(d5-d6)); return v3_add(b, v3_scale(v3_sub(c,b), w)); }
    float den = 1.0f/(va+vb+vc), v = vb*den, w = vc*den;
    return v3_add(a, v3_add(v3_scale(ab, v), v3_scale(ac, w)));
}

static float body_bound_r(const MoteBody *b) {
    if (b->shape == MOTE_SHAPE_BOX) return v3_len(b->half);
    if (b->shape == MOTE_SHAPE_CAPSULE) return b->half.y + b->radius;
    if (b->shape == MOTE_SHAPE_HULL && b->shape_data) return ((const MoteHull *)b->shape_data)->bound_r;
    return b->radius;
}

/* one triangle of mesh `mi` vs body `i` (faceted -> manifold, round -> face-
 * normal point). Factored out so the grid + brute-force paths share it. */
static void mesh_test_tri(MoteBody *bodies, int i, int mi, int t, int faceted) {
    MoteBody *b = &bodies[i], *m = &bodies[mi];
    const MoteMesh *mesh = (const MoteMesh *)m->shape_data;
    float br = body_bound_r(b);
    Vec3 A = v3_add(m->pos, m3_mul_v3(&m->orient, mesh->verts[mesh->tris[t*3+0]]));
    Vec3 B = v3_add(m->pos, m3_mul_v3(&m->orient, mesh->verts[mesh->tris[t*3+1]]));
    Vec3 C = v3_add(m->pos, m3_mul_v3(&m->orient, mesh->verts[mesh->tris[t*3+2]]));
    Vec3 cen = v3_scale(v3_add(v3_add(A, B), C), 1.0f/3.0f);
    float tr = v3_len(v3_sub(A, cen));
    float trb = v3_len(v3_sub(B, cen)); if (trb > tr) tr = trb;
    float trc = v3_len(v3_sub(C, cen)); if (trc > tr) tr = trc;
    if (v3_len2(v3_sub(b->pos, cen)) > (br+tr+0.05f)*(br+tr+0.05f)) return;   /* narrow phase */
    if (faceted) {
        build_cvx_tri(A, B, C, &s_cxb);
        convex_manifold(&s_cxa, &s_cxb, i, mi, 1000u + (uint32_t)t*8u);
        return;
    }
    Vec3 q = b->pos;
    if (b->shape == MOTE_SHAPE_CAPSULE) {
        Vec3 e0 = v3_add(b->pos, v3_scale(b->orient.r[1], -b->half.y));
        Vec3 cp0 = closest_tri(e0, A, B, C);
        Vec3 e1 = v3_add(b->pos, v3_scale(b->orient.r[1],  b->half.y));
        Vec3 cp1 = closest_tri(e1, A, B, C);
        q = (v3_len2(v3_sub(e0,cp0)) < v3_len2(v3_sub(e1,cp1))) ? e0 : e1;
    }
    Vec3 cp = closest_tri(q, A, B, C);
    Vec3 d = v3_sub(q, cp); float dist = v3_len(d);
    if (dist < b->radius) {
        Vec3 fn = v3_norm(v3_cross(v3_sub(B, A), v3_sub(C, A)));
        if (v3_dot(d, fn) < 0.0f) fn = v3_scale(fn, -1.0f);
        float pen = b->radius - v3_dot(d, fn);
        if (pen > 0.0f) add_contact(i, mi, fn, cp, pen, 1000u + (uint32_t)t);
    }
}

/* dynamic body `i` vs a static triangle MESH `mi`. */
static void gen_vs_mesh(MoteBody *bodies, int i, int mi) {
    MoteBody *b = &bodies[i], *m = &bodies[mi];
    const MoteMesh *mesh = (const MoteMesh *)m->shape_data;
    int faceted = (b->shape == MOTE_SHAPE_BOX || b->shape == MOTE_SHAPE_HULL);
    if (faceted) build_cvx(b, &s_cxa);

    if (mesh->grid_n > 0) {
        /* only the triangles in the 3x3 cells around the body (mesh assumed at
         * rest, so use the body's local XZ vs the grid's world bounds). */
        int gn = mesh->grid_n;
        Vec3 lp = v3_sub(b->pos, m->pos);
        int bx = (int)((lp.x - mesh->grid_x0) * mesh->grid_invx);
        int bz = (int)((lp.z - mesh->grid_z0) * mesh->grid_invz);
        for (int dz = -1; dz <= 1; dz++) {
            int gz = bz + dz; if (gz < 0 || gz >= gn) continue;
            for (int dx = -1; dx <= 1; dx++) {
                int gx = bx + dx; if (gx < 0 || gx >= gn) continue;
                int cell = gz*gn + gx;
                for (int e = mesh->grid_start[cell]; e < mesh->grid_start[cell+1]; e++)
                    mesh_test_tri(bodies, i, mi, mesh->grid_tri[e], faceted);
            }
        }
    } else {
        for (int t = 0; t < mesh->ntris; t++) mesh_test_tri(bodies, i, mi, t, faceted);
    }
}




/* Collision for any pair involving a capsule or hull (single contact). */
static void gen_gjk(MoteBody *bodies, int i, int j) {
    MoteBody *A = &bodies[i], *B = &bodies[j];
    SupPt sp[4];
    if (!gjk(A, B, sp)) return;
    Vec3 n, p; float depth;
    if (!epa(A, B, sp, &n, &depth, &p)) return;
    if (depth <= 0.0f) return;
    /* n points from B into A (A separates along -n? cso = A-B, origin->face = n
     * means A penetrates B along +n) -> body A (=i) moves along +n. */
    add_contact(i, j, n, p, depth, 0);
}

static void gen_pair(MoteBody *bodies, int i, int j) {
    uint32_t si = bodies[i].shape, sj = bodies[j].shape;
    if (si == MOTE_SHAPE_CAPSULE || si == MOTE_SHAPE_HULL ||
        sj == MOTE_SHAPE_CAPSULE || sj == MOTE_SHAPE_HULL) {
        int facA = (si == MOTE_SHAPE_BOX || si == MOTE_SHAPE_HULL);
        int facB = (sj == MOTE_SHAPE_BOX || sj == MOTE_SHAPE_HULL);
        if (facA && facB) gen_convex(bodies, i, j);   /* box/hull manifold */
        else gen_gjk(bodies, i, j);                   /* sphere/capsule single contact */
        return;
    }
    int ab = (si == MOTE_SHAPE_BOX), bb = (sj == MOTE_SHAPE_BOX);
    if (!ab && !bb) gen_sphere_sphere(bodies, i, j);
    else if (ab && bb) gen_box_box(bodies, i, j);
    else if (ab) gen_sphere_box(bodies, j, i);
    else gen_sphere_box(bodies, i, j);
}

/* Dynamic body `d` vs static plane `pl` (infinite half-space, normal = r[1]). */
static void gen_vs_plane(MoteBody *bodies, int d, int pl) {
    MoteBody *b = &bodies[d], *plane = &bodies[pl];
    Vec3 N = plane->orient.r[1];
    if (b->shape == MOTE_SHAPE_SPHERE) {
        float dist = v3_dot(v3_sub(b->pos, plane->pos), N);
        float pen = b->radius - dist;
        if (pen > 0.0f) add_contact(d, pl, N, v3_sub(b->pos, v3_scale(N, b->radius)), pen, 200);
    } else if (b->shape == MOTE_SHAPE_CAPSULE) {          /* two segment ends */
        for (int e = 0; e < 2; e++) {
            Vec3 c = v3_add(b->pos, v3_scale(b->orient.r[1], e ? b->half.y : -b->half.y));
            float pen = b->radius - v3_dot(v3_sub(c, plane->pos), N);
            if (pen > 0.0f) add_contact(d, pl, N, v3_sub(c, v3_scale(N, b->radius)), pen, (uint32_t)(200 + e));
        }
    } else if (b->shape == MOTE_SHAPE_HULL) {             /* each hull vertex */
        const MoteHull *h = (const MoteHull *)b->shape_data;
        for (int k = 0; k < h->nverts; k++) {
            Vec3 wv = v3_add(b->pos, m3_mul_v3(&b->orient, h->verts[k]));
            float pen = -v3_dot(v3_sub(wv, plane->pos), N);
            if (pen > 0.0f) add_contact(d, pl, N, wv, pen, (uint32_t)(200 + k));
        }
    } else {                                              /* box: 8 corners vs plane */
        int corner = 0;
        for (int sx = -1; sx <= 1; sx += 2)
        for (int sy = -1; sy <= 1; sy += 2)
        for (int sz = -1; sz <= 1; sz += 2) {
            Vec3 lv = v3(sx * b->half.x, sy * b->half.y, sz * b->half.z);
            Vec3 wv = v3_add(b->pos, m3_mul_v3(&b->orient, lv));
            float pen = -v3_dot(v3_sub(wv, plane->pos), N);
            if (pen > 0.0f) add_contact(d, pl, N, wv, pen, (uint32_t)(200 + corner));
            corner++;
        }
    }
}

/* --- velocity solver ---------------------------------------------------- */

static void apply_impulse(MoteBody *bodies, Contact *c, Vec3 J) {
    MoteBody *A = &bodies[c->a];
    Vec3 rA = v3_sub(c->p, A->pos);
    A->vel = v3_add(A->vel, v3_scale(J, eff_im(A)));
    A->w   = v3_add(A->w, iinv_mul(A, v3_cross(rA, J)));
    if (c->b >= 0) {
        MoteBody *B = &bodies[c->b];
        Vec3 rB = v3_sub(c->p, B->pos);
        B->vel = v3_sub(B->vel, v3_scale(J, eff_im(B)));
        B->w   = v3_sub(B->w, iinv_mul(B, v3_cross(rB, J)));
    }
}

static Vec3 contact_vrel(MoteBody *bodies, Contact *c) {
    MoteBody *A = &bodies[c->a];
    Vec3 vA = v3_add(A->vel, v3_cross(A->w, v3_sub(c->p, A->pos)));
    if (c->b < 0) return vA;
    MoteBody *B = &bodies[c->b];
    Vec3 vB = v3_add(B->vel, v3_cross(B->w, v3_sub(c->p, B->pos)));
    return v3_sub(vA, vB);
}

static float eff_mass(MoteBody *bodies, Contact *c, Vec3 dir) {
    MoteBody *A = &bodies[c->a];
    Vec3 rA = v3_sub(c->p, A->pos);
    float k = eff_im(A) + v3_dot(dir, v3_cross(iinv_mul(A, v3_cross(rA, dir)), rA));
    if (c->b >= 0) {
        MoteBody *B = &bodies[c->b];
        Vec3 rB = v3_sub(c->p, B->pos);
        k += eff_im(B) + v3_dot(dir, v3_cross(iinv_mul(B, v3_cross(rB, dir)), rB));
    }
    return k;
}

static int prepare(MoteBody *bodies, const MoteWorld *w, float h) {
    int impact = 0;
    for (int i = 0; i < s_nct; i++) {
        Contact *c = &s_ct[i];
        Vec3 vrel = contact_vrel(bodies, c);
        float vn = v3_dot(vrel, c->n);
        Vec3 vt = v3_sub(vrel, v3_scale(c->n, vn));
        float vtl = v3_len(vt);
        c->t = (vtl > 1e-4f) ? v3_scale(vt, 1.0f / vtl) : v3_perp(c->n);
        c->kn = eff_mass(bodies, c, c->n);
        c->kt = eff_mass(bodies, c, c->t);
        /* Combine per-body materials (0 -> world default). Wall = body A only. */
        MoteBody *A = &bodies[c->a];
        float fa = A->friction > 0.0f ? A->friction : w->friction;
        float ea = A->restitution > 0.0f ? A->restitution : w->restitution;
        if (c->b >= 0) {
            MoteBody *B = &bodies[c->b];
            float fb = B->friction > 0.0f ? B->friction : w->friction;
            float eb = B->restitution > 0.0f ? B->restitution : w->restitution;
            c->mu = sqrtf(fa * fb); c->emat = ea > eb ? ea : eb;
        } else { c->mu = fa; c->emat = ea; }
        float e = (-vn < REST_SLOP) ? 0.0f : c->emat;
        if (-vn >= REST_SLOP) impact = 1;
        c->bias = (e > 0.0f) ? -e * vn : 0.0f;           /* restitution only; */
        c->jp = 0.0f;                                    /* position via split impulse */
        int woke = (c->a < PHYS_MAX_BODIES && s_woke[c->a]) ||
                   (c->b >= 0 && c->b < PHYS_MAX_BODIES && s_woke[c->b]);
        if (woke) { c->jn = 0.0f; c->jt = 0.0f; }        /* cold start a just-woken body */
        else cache_lookup(c->key, &c->jn, &c->jt);       /* warm start */
        apply_impulse(bodies, c, v3_add(v3_scale(c->n, c->jn), v3_scale(c->t, c->jt)));
    }
    return impact;
}

static void solve_vel(MoteBody *bodies, const MoteWorld *w) {
    for (int i = 0; i < s_nct; i++) {
        Contact *c = &s_ct[i];
        if (c->kn < 1e-9f) continue;
        /* normal */
        float vn = v3_dot(contact_vrel(bodies, c), c->n);
        float dJn = (c->bias - vn) / c->kn;
        float jn0 = c->jn;
        c->jn = fmaxf(0.0f, c->jn + dJn);
        dJn = c->jn - jn0;
        apply_impulse(bodies, c, v3_scale(c->n, dJn));
        /* friction */
        if (c->kt < 1e-9f) continue;
        float vt = v3_dot(contact_vrel(bodies, c), c->t);
        float dJt = -vt / c->kt;
        float jt0 = c->jt, maxf = c->mu * c->jn;
        c->jt = clampf(c->jt + dJt, -maxf, maxf);
        dJt = c->jt - jt0;
        apply_impulse(bodies, c, v3_scale(c->t, dJt));
    }
}

static void store_impulses(void) {
    for (int i = 0; i < s_nct; i++) cache_store(s_ct[i].key, s_ct[i].jn, s_ct[i].jt);
}

/* --- split-impulse position correction (no energy injection) ------------ *
 * Penetration is removed via a separate PSEUDO-velocity field that integrates
 * into positions only — it never touches real velocity, so deep simultaneous
 * landings can't explode the way Baumgarte-in-velocity does. */
static void apply_pseudo(MoteBody *bodies, Contact *c, float Jp) {
    Vec3 J = v3_scale(c->n, Jp);
    int a = c->a;
    s_pv[a] = v3_add(s_pv[a], v3_scale(J, eff_im(&bodies[a])));
    s_pw[a] = v3_add(s_pw[a], iinv_mul(&bodies[a], v3_cross(v3_sub(c->p, bodies[a].pos), J)));
    if (c->b >= 0) {
        int b = c->b;
        s_pv[b] = v3_sub(s_pv[b], v3_scale(J, eff_im(&bodies[b])));
        s_pw[b] = v3_sub(s_pw[b], iinv_mul(&bodies[b], v3_cross(v3_sub(c->p, bodies[b].pos), J)));
    }
}

static void solve_pos(MoteBody *bodies, float h) {
    for (int i = 0; i < s_nct; i++) {
        Contact *c = &s_ct[i];
        if (c->kn < 1e-9f) continue;
        int a = c->a;
        Vec3 pvr = v3_add(s_pv[a], v3_cross(s_pw[a], v3_sub(c->p, bodies[a].pos)));
        if (c->b >= 0) {
            int b = c->b;
            Vec3 pvB = v3_add(s_pv[b], v3_cross(s_pw[b], v3_sub(c->p, bodies[b].pos)));
            pvr = v3_sub(pvr, pvB);
        }
        float pvn = v3_dot(pvr, c->n);
        float corr = clampf(c->pen - POS_SLOP, 0.0f, 0.2f);   /* cap deep-pen correction */
        float target = POS_BETA * corr / h;
        float dJp = (target - pvn) / c->kn;
        float jp0 = c->jp;
        c->jp = fmaxf(0.0f, c->jp + dJp);
        apply_pseudo(bodies, c, c->jp - jp0);
    }
}

/* --- broad phase (uniform grid) ----------------------------------------- */

static int   g_cell[GRID_CELLS], g_next[GRID_MAX_BODIES];
static int   g_nx, g_ny, g_nz;
static float g_cs, g_x0, g_y0, g_z0;

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void grid_build(const MoteWorld *w, MoteBody *bodies, int n) {
    float maxr = 0.05f;
    for (int i = 0; i < n; i++) {
        const MoteBody *b = &bodies[i];
        if (b->shape == MOTE_SHAPE_PLANE || b->shape == MOTE_SHAPE_MESH) continue; /* infinite/static */
        float r = (b->shape == MOTE_SHAPE_BOX) ? v3_len(b->half)
                : (b->shape == MOTE_SHAPE_CAPSULE) ? b->half.y + b->radius
                : (b->shape == MOTE_SHAPE_HULL && b->shape_data) ? ((const MoteHull *)b->shape_data)->bound_r
                : b->radius;
        if (r > maxr) maxr = r;
    }
    g_cs = 2.0f * maxr; g_x0 = w->bmin.x; g_y0 = w->bmin.y; g_z0 = w->bmin.z;
    for (;;) {
        g_nx = (int)((w->bmax.x - w->bmin.x) / g_cs) + 1; if (g_nx < 1) g_nx = 1;
        g_ny = (int)((w->bmax.y - w->bmin.y) / g_cs) + 1; if (g_ny < 1) g_ny = 1;
        g_nz = (int)((w->bmax.z - w->bmin.z) / g_cs) + 1; if (g_nz < 1) g_nz = 1;
        if ((long)g_nx * g_ny * g_nz <= GRID_CELLS) break;
        g_cs *= 1.5f;
    }
    int nc = g_nx * g_ny * g_nz;
    for (int c = 0; c < nc; c++) g_cell[c] = -1;
    for (int i = 0; i < n; i++) {
        if (bodies[i].shape == MOTE_SHAPE_PLANE || bodies[i].shape == MOTE_SHAPE_MESH) { g_next[i] = -1; continue; }
        int cx = clampi((int)((bodies[i].pos.x - g_x0) / g_cs), 0, g_nx - 1);
        int cy = clampi((int)((bodies[i].pos.y - g_y0) / g_cs), 0, g_ny - 1);
        int cz = clampi((int)((bodies[i].pos.z - g_z0) / g_cs), 0, g_nz - 1);
        int c = (cz * g_ny + cy) * g_nx + cx;
        g_next[i] = g_cell[c]; g_cell[c] = i;
    }
}

/* Wake a sleeper that an awake body has hit: deep intrusion OR a fast approach
 * (done during build so the solve this substep sees it movable). A freshly woken
 * body is flagged so prepare() skips its (stale, immovable-era) warm-start. */
static void wake_check(MoteBody *bodies, int ia, int ib, int before) {
    if (s_nct == before) return;
    float pen = 0.0f;
    for (int k = before; k < s_nct; k++) if (s_ct[k].pen > pen) pen = s_ct[k].pen;
    int hard = pen > WAKE_PEN;
    float wv2 = WAKE_VEL * WAKE_VEL;
    MoteBody *a = &bodies[ia], *b = &bodies[ib];
    if (body_asleep(a) && (hard || v3_dot(b->vel, b->vel) > wv2)) { a->_reserved[0] = 0; s_woke[ia] = 1; }
    if (body_asleep(b) && (hard || v3_dot(a->vel, a->vel) > wv2)) { b->_reserved[0] = 0; s_woke[ib] = 1; }
}

static void build_pairs_grid(MoteBody *bodies, int n) {
    for (int i = 0; i < n; i++) {
        MoteBody *bi = &bodies[i];
        if (bi->shape == MOTE_SHAPE_PLANE || bi->shape == MOTE_SHAPE_MESH) continue; /* handled separately */
        int cx = clampi((int)((bi->pos.x - g_x0) / g_cs), 0, g_nx - 1);
        int cy = clampi((int)((bi->pos.y - g_y0) / g_cs), 0, g_ny - 1);
        int cz = clampi((int)((bi->pos.z - g_z0) / g_cs), 0, g_nz - 1);
        for (int z = (cz ? cz - 1 : 0); z <= (cz < g_nz - 1 ? cz + 1 : cz); z++)
        for (int y = (cy ? cy - 1 : 0); y <= (cy < g_ny - 1 ? cy + 1 : cy); y++)
        for (int x = (cx ? cx - 1 : 0); x <= (cx < g_nx - 1 ? cx + 1 : cx); x++) {
            int c = (z * g_ny + y) * g_nx + x;
            for (int jj = g_cell[c]; jj >= 0; jj = g_next[jj]) {
                if (jj <= i) continue;
                if (body_asleep(bi) && body_asleep(&bodies[jj])) continue;
                int before = s_nct;
                gen_pair(bodies, i, jj);
                wake_check(bodies, i, jj, before);
            }
        }
    }
}

static void build_pairs_all(MoteBody *bodies, int n) {
    for (int i = 0; i < n; i++) {
        if (bodies[i].shape == MOTE_SHAPE_PLANE || bodies[i].shape == MOTE_SHAPE_MESH) continue;
        for (int j = i + 1; j < n; j++) {
            if (bodies[j].shape == MOTE_SHAPE_PLANE || bodies[j].shape == MOTE_SHAPE_MESH) continue;
            if (body_asleep(&bodies[i]) && body_asleep(&bodies[j])) continue;
            int before = s_nct;
            gen_pair(bodies, i, j);
            wake_check(bodies, i, j, before);
        }
    }
}

/* body i vs the 6 box walls -> wall contacts (b = -1). */
static void build_walls(const MoteWorld *w, MoteBody *bodies, int i) {
    MoteBody *b = &bodies[i];
    if (body_asleep(b) || b->shape == MOTE_SHAPE_PLANE || b->shape == MOTE_SHAPE_MESH) return;
    struct { Vec3 N; float d; } wall[6] = {
        { v3( 1, 0, 0),  w->bmin.x }, { v3(-1, 0, 0), -w->bmax.x },
        { v3( 0, 1, 0),  w->bmin.y }, { v3( 0,-1, 0), -w->bmax.y },
        { v3( 0, 0, 1),  w->bmin.z }, { v3( 0, 0,-1), -w->bmax.z },
    };
    if (b->shape == MOTE_SHAPE_SPHERE) {
        for (int k = 0; k < 6; k++) {
            float pen = wall[k].d + b->radius - v3_dot(wall[k].N, b->pos);
            if (pen > 0.0f)
                add_contact(i, -1, wall[k].N,
                            v3_sub(b->pos, v3_scale(wall[k].N, b->radius)), pen, (uint32_t)(100 + k));
        }
        return;
    }
    for (int k = 0; k < 6; k++) {
        Vec3 N = wall[k].N; float d = wall[k].d;
        int corner = 0;
        for (int sx = -1; sx <= 1; sx += 2)
        for (int sy = -1; sy <= 1; sy += 2)
        for (int sz = -1; sz <= 1; sz += 2) {
            Vec3 lv = v3(sx * b->half.x, sy * b->half.y, sz * b->half.z);
            Vec3 wv = v3_add(b->pos, m3_mul_v3(&b->orient, lv));
            float pen = d - v3_dot(N, wv);
            if (pen > 0.0f) add_contact(i, -1, N, wv, pen, (uint32_t)(100 + k * 8 + corner));
            corner++;
        }
    }
}

/* --- integration -------------------------------------------------------- */

static void integrate(const MoteWorld *w, MoteBody *b, float h) {
    if (b->inv_mass <= 0.0f) return;
    b->vel = v3_add(b->vel, v3_scale(w->gravity, h));
    b->vel = v3_scale(b->vel, 1.0f / (1.0f + w->linear_damp * h));
    b->w   = v3_scale(b->w,   1.0f / (1.0f + w->angular_damp * h));
    b->pos = v3_add(b->pos, v3_scale(b->vel, h));
    float wl = v3_len(b->w);
    if (wl > 1e-6f) {
        m3_rotate_world(&b->orient, v3_scale(b->w, 1.0f / wl), wl * h);
        m3_orthonormalize(&b->orient);
    }
}

MOTE_HOT
uint32_t mote_phys_step(MoteWorld *w, MoteBody *bodies, int n, float dt) {
    float h = (w->substep > 0.0f) ? w->substep : DEFAULT_H;
    int cap = (w->max_substeps > 0) ? w->max_substeps : MAX_SUBSTEPS;
    int use_grid = (n > 24 && n <= GRID_MAX_BODIES);
    uint32_t ev = 0;
    if (dt > 0.1f) dt = 0.1f;

    w->_acc += dt;
    int sub = 0;
    while (w->_acc >= h && sub < cap) {
        w->_acc -= h;
        for (int i = 0; i < n; i++) if (!body_asleep(&bodies[i])) integrate(w, &bodies[i], h);

        s_nct = 0;
        if (n <= PHYS_MAX_BODIES) {
            memset(s_touch, 0, (size_t)n);
            memset(s_woke, 0, (size_t)n);
            memset(s_pen, 0, (size_t)n * sizeof s_pen[0]);
        }
        if (w->walls) for (int i = 0; i < n; i++) build_walls(w, bodies, i);  /* support first */
        for (int p = 0; p < n; p++) if (bodies[p].shape == MOTE_SHAPE_PLANE)  /* static planes */
            for (int i = 0; i < n; i++)
                if (bodies[i].shape != MOTE_SHAPE_PLANE && bodies[i].shape != MOTE_SHAPE_MESH && !body_asleep(&bodies[i]))
                    gen_vs_plane(bodies, i, p);
        for (int mm = 0; mm < n; mm++) if (bodies[mm].shape == MOTE_SHAPE_MESH)  /* static meshes */
            for (int i = 0; i < n; i++)
                if (bodies[i].shape != MOTE_SHAPE_PLANE && bodies[i].shape != MOTE_SHAPE_MESH && !body_asleep(&bodies[i]))
                    gen_vs_mesh(bodies, i, mm);
        if (use_grid) { grid_build(w, bodies, n); build_pairs_grid(bodies, n); }
        else            build_pairs_all(bodies, n);

        memset(s_cache_cur, 0, sizeof s_cacheA);
        if (prepare(bodies, w, h)) ev |= MOTE_PHYS_HIT;
        for (int it = 0; it < SOLVER_ITERS; it++) solve_vel(bodies, w);
        store_impulses();

        /* position correction via split impulse (positions only, no energy). */
        if (n <= PHYS_MAX_BODIES) {
            memset(s_pv, 0, (size_t)n * sizeof s_pv[0]);
            memset(s_pw, 0, (size_t)n * sizeof s_pw[0]);
            for (int it = 0; it < POS_ITERS; it++) solve_pos(bodies, h);
            for (int i = 0; i < n; i++) {
                if (body_asleep(&bodies[i])) continue;
                bodies[i].pos = v3_add(bodies[i].pos, v3_scale(s_pv[i], h));
                float wl = v3_len(s_pw[i]);
                if (wl > 1e-6f) {
                    m3_rotate_world(&bodies[i].orient, v3_scale(s_pw[i], 1.0f / wl), wl * h);
                    m3_orthonormalize(&bodies[i].orient);
                }
            }
        }
        Imp *t = s_cache_prev; s_cache_prev = s_cache_cur; s_cache_cur = t;

        /* Contact-gated sleep: a body may only fall asleep while it is RESTING on
         * something (had a contact) and barely moving — so it can never sleep in
         * mid-air (the floating-sphere bug). Once asleep it stays put (immovable)
         * until displaced; an awake body intruding past WAKE_PEN wakes it. */
        if (n <= PHYS_MAX_BODIES) for (int i = 0; i < n; i++) {
            MoteBody *b = &bodies[i];
            Vec3 anchor = v3(u2f(b->_reserved[1]), u2f(b->_reserved[2]), u2f(b->_reserved[3]));
            Vec3 dd = v3_sub(b->pos, anchor);
            float disp2 = v3_dot(dd, dd);
            if (body_asleep(b)) {
                if (disp2 > SLEEP_DIST2) {                 /* moved -> wake + re-anchor */
                    b->_reserved[0] = 0;
                    b->_reserved[1] = f2u(b->pos.x); b->_reserved[2] = f2u(b->pos.y); b->_reserved[3] = f2u(b->pos.z);
                }
            } else if (s_touch[i] && s_pen[i] < 0.012f && disp2 < SLEEP_DIST2 && v3_dot(b->w, b->w) < SLEEP_ANG2) {
                b->_reserved[0]++;                         /* resting (not deeply overlapping) + still -> sleep */
            } else {                                       /* airborne / moving -> stay awake, re-anchor */
                b->_reserved[0] = 0;
                b->_reserved[1] = f2u(b->pos.x); b->_reserved[2] = f2u(b->pos.y); b->_reserved[3] = f2u(b->pos.z);
            }
        }
        sub++;
    }
    if (w->_acc > h) w->_acc = 0.0f;
    return ev;
}

/* --- queries (stateless; no simulation) --------------------------------- */

static int ray_sphere(Vec3 o, Vec3 d, Vec3 c, float r, float *t, Vec3 *p, Vec3 *nm) {
    Vec3 m = v3_sub(o, c);
    float b = v3_dot(m, d), cc = v3_dot(m, m) - r * r;
    if (cc > 0.0f && b > 0.0f) return 0;
    float disc = b * b - cc;
    if (disc < 0.0f) return 0;
    float tt = -b - sqrtf(disc);
    if (tt < 0.0f) tt = 0.0f;
    *t = tt; *p = v3_add(o, v3_scale(d, tt)); *nm = v3_norm(v3_sub(*p, c));
    return 1;
}
static int ray_plane(Vec3 o, Vec3 d, Vec3 pt, Vec3 N, float *t, Vec3 *p, Vec3 *nm) {
    float dn = v3_dot(d, N);
    if (dn > -1e-8f && dn < 1e-8f) return 0;
    float tt = v3_dot(v3_sub(pt, o), N) / dn;
    if (tt < 0.0f) return 0;
    *t = tt; *p = v3_add(o, v3_scale(d, tt)); *nm = (dn < 0.0f) ? N : v3_scale(N, -1.0f);
    return 1;
}
static int ray_box(Vec3 o, Vec3 d, const MoteBody *b, float *t, Vec3 *p, Vec3 *nm) {
    Vec3 lo = m3_mul_v3_t(&b->orient, v3_sub(o, b->pos));
    Vec3 ld = m3_mul_v3_t(&b->orient, d);
    float lop[3] = { lo.x, lo.y, lo.z }, ldp[3] = { ld.x, ld.y, ld.z };
    float h[3] = { b->half.x, b->half.y, b->half.z };
    float tmin = -1e30f, tmax = 1e30f, sign = 1.0f; int axis = 0;
    for (int i = 0; i < 3; i++) {
        if (ldp[i] > -1e-8f && ldp[i] < 1e-8f) { if (lop[i] < -h[i] || lop[i] > h[i]) return 0; continue; }
        float inv = 1.0f / ldp[i], t1 = (-h[i] - lop[i]) * inv, t2 = (h[i] - lop[i]) * inv, s = -1.0f;
        if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; s = 1.0f; }
        if (t1 > tmin) { tmin = t1; axis = i; sign = s; }
        if (t2 < tmax) tmax = t2;
        if (tmin > tmax) return 0;
    }
    if (tmax < 0.0f) return 0;
    *t = tmin >= 0.0f ? tmin : 0.0f;
    *p = v3_add(o, v3_scale(d, *t));
    Vec3 ln = v3(axis == 0 ? sign : 0.0f, axis == 1 ? sign : 0.0f, axis == 2 ? sign : 0.0f);
    *nm = m3_mul_v3(&b->orient, ln);
    return 1;
}

int mote_phys_raycast(const MoteWorld *w, const MoteBody *bodies, int n,
                      Vec3 origin, Vec3 dir, float max_dist, int skip, MoteRayHit *hit) {
    (void)w;
    dir = v3_norm(dir);
    float best = max_dist; int bi = -1; Vec3 bp = origin, bn = v3(0, 1, 0);
    for (int i = 0; i < n; i++) {
        if (i == skip) continue;
        const MoteBody *b = &bodies[i];
        float t; Vec3 p, nm; int got;
        if (b->shape == MOTE_SHAPE_SPHERE)    got = ray_sphere(origin, dir, b->pos, b->radius, &t, &p, &nm);
        else if (b->shape == MOTE_SHAPE_BOX)  got = ray_box(origin, dir, b, &t, &p, &nm);
        else                                  got = ray_plane(origin, dir, b->pos, b->orient.r[1], &t, &p, &nm);
        if (got && t < best) { best = t; bi = i; bp = p; bn = nm; }
    }
    if (bi < 0) return 0;
    hit->body = bi; hit->t = best; hit->point = bp; hit->normal = bn;
    return 1;
}

static int sphere_obb_overlap(Vec3 c, float r, const MoteBody *b) {
    Vec3 rel = v3_sub(c, b->pos);
    float ql[3] = { v3_dot(rel, b->orient.r[0]), v3_dot(rel, b->orient.r[1]), v3_dot(rel, b->orient.r[2]) };
    float h[3] = { b->half.x, b->half.y, b->half.z }, d2 = 0.0f;
    for (int i = 0; i < 3; i++) {
        float e = ql[i] < -h[i] ? -h[i] - ql[i] : ql[i] > h[i] ? ql[i] - h[i] : 0.0f;
        d2 += e * e;
    }
    return d2 < r * r;
}

int mote_phys_overlap(const MoteWorld *w, const MoteBody *bodies, int n,
                      Vec3 center, float radius, int *out, int max) {
    (void)w;
    int cnt = 0;
    for (int i = 0; i < n && cnt < max; i++) {
        const MoteBody *b = &bodies[i];
        int ov;
        if (b->shape == MOTE_SHAPE_SPHERE) { float rr = radius + b->radius; ov = v3_len2(v3_sub(b->pos, center)) < rr * rr; }
        else if (b->shape == MOTE_SHAPE_BOX) ov = sphere_obb_overlap(center, radius, b);
        else ov = v3_dot(v3_sub(center, b->pos), b->orient.r[1]) < radius;
        if (ov) out[cnt++] = i;
    }
    return cnt;
}
