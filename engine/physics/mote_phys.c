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

#define DEFAULT_H (1.0f / 480.0f)   /* ~2 ms substep */

void mote_phys_world_defaults(MoteWorld *w) {
    w->gravity = v3(0.0f, -9.8f, 0.0f);
    w->bmin = v3(-2.0f, -2.0f, -2.0f);
    w->bmax = v3( 2.0f,  2.0f,  2.0f);
    w->restitution = 0.6f;
    w->friction    = 0.3f;
    w->linear_damp  = 0.05f;
    w->angular_damp = 0.4f;
    w->substep = DEFAULT_H;
    w->_acc = 0.0f;
}

/* Sphere inverse rotational inertia: I = 0.4 m r^2 -> 1/I = 2.5*inv_mass/r^2. */
static inline float inv_I(const MoteBody *b) {
    return (b->radius > 1e-6f) ? (2.5f * b->inv_mass) / (b->radius * b->radius) : 0.0f;
}

static int collide_pair(const MoteWorld *w, MoteBody *bi, MoteBody *bj) {
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

    float Jn = -(1.0f + w->restitution) * vn / ims;
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

    float Jn = -(1.0f + w->restitution) * vn / im;
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

static int walls(const MoteWorld *w, MoteBody *b) {
    int hit = 0;
    float lo, hi;
    /* X */
    lo = w->bmin.x + b->radius; hi = w->bmax.x - b->radius;
    if (b->pos.x < lo) { b->pos.x = lo; hit |= collide_wall(w, b, v3( 1, 0, 0)); }
    if (b->pos.x > hi) { b->pos.x = hi; hit |= collide_wall(w, b, v3(-1, 0, 0)); }
    /* Y */
    lo = w->bmin.y + b->radius; hi = w->bmax.y - b->radius;
    if (b->pos.y < lo) { b->pos.y = lo; hit |= collide_wall(w, b, v3(0,  1, 0)); }
    if (b->pos.y > hi) { b->pos.y = hi; hit |= collide_wall(w, b, v3(0, -1, 0)); }
    /* Z */
    lo = w->bmin.z + b->radius; hi = w->bmax.z - b->radius;
    if (b->pos.z < lo) { b->pos.z = lo; hit |= collide_wall(w, b, v3(0, 0,  1)); }
    if (b->pos.z > hi) { b->pos.z = hi; hit |= collide_wall(w, b, v3(0, 0, -1)); }
    return hit;
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

MOTE_HOT
uint32_t mote_phys_step(MoteWorld *w, MoteBody *bodies, int n, float dt) {
    float h = (w->substep > 0.0f) ? w->substep : DEFAULT_H;
    uint32_t ev = 0;
    if (dt > 0.1f) dt = 0.1f;                         /* clamp after a stall */
    w->_acc += dt;
    int guard = 0;
    while (w->_acc >= h && guard++ < 64) {
        w->_acc -= h;
        for (int i = 0; i < n; i++) integrate(w, &bodies[i], h);
        for (int i = 0; i < n; i++)
            for (int j = i + 1; j < n; j++)
                if (collide_pair(w, &bodies[i], &bodies[j])) ev |= MOTE_PHYS_HIT;
        for (int i = 0; i < n; i++)
            if (walls(w, &bodies[i])) ev |= MOTE_PHYS_HIT;
    }
    return ev;
}
